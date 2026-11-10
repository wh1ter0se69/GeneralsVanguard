/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#include "PreRTS.h"
#include "GameNetwork/NetPacketStructs.h"

#include "GameNetwork/GameMessageParser.h"
#include "GameNetwork/NetCommandRef.h"

////////////////////////////////////////////////////////////////////////////////

size_t SmallNetPacketCommandBase::getSize(const SmallNetPacketCommandBaseSelect *select)
{
	size_t size = 0;

	if (select != nullptr)
	{
		if (select->useCommandType)
		{
			size += sizeof(NetPacketCommandTypeField);
		}
		if (select->useRelay)
		{
			size += sizeof(NetPacketRelayField);
		}
		if (select->useFrame)
		{
			size += sizeof(NetPacketFrameField);
		}
		if (select->usePlayerId)
		{
			size += sizeof(NetPacketPlayerIdField);
		}
		if (select->useCommandId)
		{
			size += sizeof(NetPacketCommandIdField);
		}
		size += sizeof(NetPacketDataField);
	}
	else
	{
		size += sizeof(SmallNetPacketCommandBase::CommandBase);
	}
	return size;
}

size_t SmallNetPacketCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref, const SmallNetPacketCommandBaseSelect *select)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	size_t size = 0;

	if (select != nullptr)
	{
		// If necessary, put the command type into the packet.
		if (select->useCommandType)
		{
			size += network::writePrimitive(buffer + size, base.commandType);
		}
		// If necessary, put the relay into the packet.
		if (select->useRelay)
		{
			size += network::writePrimitive(buffer + size, base.relay);
		}
		// If necessary, put the execution frame into the packet.
		if (select->useFrame)
		{
			size += network::writePrimitive(buffer + size, base.frame);
		}
		// If necessary, put the player ID into the packet.
		if (select->usePlayerId)
		{
			size += network::writePrimitive(buffer + size, base.playerId);
		}
		// If necessary, put the command ID into the packet.
		if (select->useCommandId)
		{
			size += network::writePrimitive(buffer + size, base.commandId);
		}
		// Always write the data header and mark the end of the command.
		size += network::writePrimitive(buffer + size, base.dataHeader);
	}
	else
	{
		size += network::writeObject(buffer + size, base);
	}
	return size;
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketAckCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketAckCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.commandId = cmdMsg->getCommandID();
	data.originalPlayerId = cmdMsg->getOriginalPlayerID();

	return network::writeObject(buffer, data);
}

size_t NetPacketAckCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	//base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketFrameCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.commandCount = cmdMsg->getCommandCount();

	return network::writeObject(buffer, data);
}

size_t NetPacketFrameCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketPlayerLeaveCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketPlayerLeaveCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.leavingPlayerId = cmdMsg->getLeavingPlayerID();

	return network::writeObject(buffer, data);
}

size_t NetPacketPlayerLeaveCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadMetricsCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketRunAheadMetricsCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.averageLatency = cmdMsg->getAverageLatency();
	data.averageFps = cmdMsg->getAverageFps();

	return network::writeObject(buffer, data);
}

size_t NetPacketRunAheadMetricsCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketLatencyReportCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketLatencyReportCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		data.latencyMs[slot] = cmdMsg->getLatencyMs(slot);
		const Int fps = cmdMsg->getAverageFps(slot);
		data.fpsPlusOne[slot] = (UnsignedByte)((fps < 0) ? 0 : (min(fps, 254) + 1));
	}

	return network::writeObject(buffer, data);
}

size_t NetPacketLatencyReportCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketRunAheadCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.runAhead = cmdMsg->getRunAhead();
	data.frameRate = cmdMsg->getFrameRate();

	return network::writeObject(buffer, data);
}

size_t NetPacketRunAheadCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDestroyPlayerCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDestroyPlayerCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.playerIndex = cmdMsg->getPlayerIndex();

	return network::writeObject(buffer, data);
}

size_t NetPacketDestroyPlayerCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketKeepAliveCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectKeepAliveCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectPlayerCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectPlayerCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.disconnectSlot = cmdMsg->getDisconnectSlot();
	data.disconnectFrame = cmdMsg->getDisconnectFrame();

	return network::writeObject(buffer, data);
}

size_t NetPacketDisconnectPlayerCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterQueryCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketRouterQueryCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterAckCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketRouterAckCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectVoteCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectVoteCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.slot = cmdMsg->getSlot();
	data.voteFrame = cmdMsg->getVoteFrame();

	return network::writeObject(buffer, data);
}

size_t NetPacketDisconnectVoteCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketChatCommand
////////////////////////////////////////////////////////////////////////////////

// GeneralsX: sizeof(NetWireChar), not sizeof(WideChar) - see the note on
// writeStringAsWireChars in NetPacketStructs.h. getSize and copyBytes must stay literally identical
// in their arithmetic: getSize is the room check NetPacket::addCommand makes before copyBytes
// writes into m_packet[MAX_PACKET_SIZE].
size_t NetPacketChatCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);
	const Int textLength = std::min<Int>(cmdMsg->getText().getLength(), 255);

	size_t size = 0;
	size += sizeof(UnsignedByte);
	size += textLength * sizeof(network::NetWireChar);
	// GeneralsX: scale32: the chat recipient mask is RelayMaskType (64 bit) on the wire, same width in
	// getSize, copyBytes and NetPacket::readChatMessage - a Int mask cannot address slots 32-33.
	size += sizeof(RelayMaskType);
	return size;
}

size_t NetPacketChatCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	const Int textLength = std::min<Int>(cmdMsg->getText().getLength(), 255);

	size_t size = 0;
	size += network::writePrimitive(buffer + size, (UnsignedByte)textLength);
	size += network::writeStringAsWireChars(buffer + size, cmdMsg->getText(), textLength);
	size += network::writePrimitive(buffer + size, (RelayMaskType)cmdMsg->getPlayerMask());
	return size;
}

size_t NetPacketChatCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectChatCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectChatCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);
	const Int textLength = std::min<Int>(cmdMsg->getText().getLength(), 255);

	size_t size = 0;
	size += sizeof(UnsignedByte);
	// GeneralsX: Same 16-bit wire character as the in-game chat above. The
	// disconnect screen's chat is a different UI but the identical payload shape, and leaving one of
	// the pair on sizeof(WideChar) would keep the cross-platform split alive on the one screen you
	// reach precisely when the connection is already in trouble.
	size += textLength * sizeof(network::NetWireChar);
	return size;
}

size_t NetPacketDisconnectChatCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	const Int textLength = std::min<Int>(cmdMsg->getText().getLength(), 255);

	size_t size = 0;
	size += network::writePrimitive(buffer + size, (UnsignedByte)textLength);
	size += network::writeStringAsWireChars(buffer + size, cmdMsg->getText(), textLength);
	return size;
}

size_t NetPacketDisconnectChatCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketGameCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketGameCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);
	GameMessage *gmsg = cmdMsg->constructGameMessage();
	GameMessageParser *parser = newInstance(GameMessageParser)(gmsg);

	size_t size = 0;

	size += sizeof(Int);
	size += sizeof(UnsignedByte);

	GameMessageParserArgumentType *arg = parser->getFirstArgumentType();
	while (arg != nullptr)
	{
		size += sizeof(UnsignedByte); // argument type
		size += sizeof(UnsignedByte); // argument count

		const GameMessageArgumentDataType type = arg->getType();
		switch (type)
		{
		case ARGUMENTDATATYPE_INTEGER:
			size += arg->getArgCount() * sizeof(Int);
			break;
		case ARGUMENTDATATYPE_REAL:
			size += arg->getArgCount() * sizeof(Real);
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			size += arg->getArgCount() * sizeof(Bool);
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			size += arg->getArgCount() * sizeof(ObjectID);
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			size += arg->getArgCount() * sizeof(DrawableID);
			break;
		case ARGUMENTDATATYPE_TEAMID:
			size += arg->getArgCount() * sizeof(UnsignedInt);
			break;
		case ARGUMENTDATATYPE_LOCATION:
			size += arg->getArgCount() * sizeof(Coord3D);
			break;
		case ARGUMENTDATATYPE_PIXEL:
			size += arg->getArgCount() * sizeof(ICoord2D);
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			size += arg->getArgCount() * sizeof(IRegion2D);
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			size += arg->getArgCount() * sizeof(UnsignedInt);
			break;
		// GeneralsX: sizeof(GXWireChar), not sizeof(WideChar). Must stay
		// literally in step with the copyBytes case below and with
		// NetPacket::readGameMessageArgumentFromPacket - see GXWireChar.h.
		case ARGUMENTDATATYPE_WIDECHAR:
			size += arg->getArgCount() * (size_t)GX_WIRECHAR_BYTES;
			break;
		}
		arg = arg->getNext();
	}

	deleteInstance(parser);
	deleteInstance(gmsg);

	return size;
}

size_t NetPacketGameCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	GameMessage *gmsg = cmdMsg->constructGameMessage();
	GameMessageParser *parser = newInstance(GameMessageParser)(gmsg);

	size_t size = 0;

	size += network::writePrimitive(buffer + size, (Int)gmsg->getType());
	size += network::writePrimitive(buffer + size, (UnsignedByte)parser->getNumTypes());

	GameMessageParserArgumentType *argType = parser->getFirstArgumentType();
	while (argType != nullptr)
	{
		size += network::writePrimitive(buffer + size, (UnsignedByte)argType->getType());
		size += network::writePrimitive(buffer + size, (UnsignedByte)argType->getArgCount());
		argType = argType->getNext();
	}

	const Int numArgs = gmsg->getArgumentCount();
	for (Int i = 0; i < numArgs; ++i)
	{
		GameMessageArgumentDataType type = gmsg->getArgumentDataType(i);
		GameMessageArgumentType arg = *gmsg->getArgument(i);
		switch (type)
		{
		case ARGUMENTDATATYPE_INTEGER:
			size += network::writePrimitive(buffer + size, arg.integer);
			break;
		case ARGUMENTDATATYPE_REAL:
			size += network::writePrimitive(buffer + size, arg.real);
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			size += network::writePrimitive(buffer + size, arg.boolean);
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			size += network::writePrimitive(buffer + size, arg.objectID);
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			size += network::writePrimitive(buffer + size, arg.drawableID);
			break;
		case ARGUMENTDATATYPE_TEAMID:
			size += network::writePrimitive(buffer + size, arg.teamID);
			break;
		case ARGUMENTDATATYPE_LOCATION:
			size += network::writePrimitive(buffer + size, arg.location);
			break;
		case ARGUMENTDATATYPE_PIXEL:
			size += network::writePrimitive(buffer + size, arg.pixel);
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			size += network::writePrimitive(buffer + size, arg.pixelRegion);
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			size += network::writePrimitive(buffer + size, arg.timestamp);
			break;
		// GeneralsX: A fixed 16-bit unit, not the host's wchar_t. The
		// count of units written always equals the count getSize() billed above, because
		// gxWireCharWrite folds anything outside the BMP to '?' rather than emitting nothing.
		case ARGUMENTDATATYPE_WIDECHAR:
			size += gxWireCharWrite(buffer + size, arg.wChar);
			break;
		}
	}

	deleteInstance(parser);
	deleteInstance(gmsg);

	return size;
}

size_t NetPacketGameCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketWrapperCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketWrapperCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);

	size_t size = 0;
	size += sizeof(FixedData);
	size += cmdMsg->getDataLength();
	return size;
}

size_t NetPacketWrapperCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.wrappedCommandId = cmdMsg->getWrappedCommandID();
	data.chunkNumber = cmdMsg->getChunkNumber();
	data.numChunks = cmdMsg->getNumChunks();
	data.totalDataLength = cmdMsg->getTotalDataLength();
	data.dataLength = cmdMsg->getDataLength();
	data.dataOffset = cmdMsg->getDataOffset();
	data.wrapSeq = cmdMsg->getWrapSeq();
	data.lowWater = cmdMsg->getLowWater();

	size_t size = 0;
	size += network::writeObject(buffer + size, data);
	size += network::writeBytes(buffer + size, cmdMsg->getData(), cmdMsg->getDataLength());
	return size;
}

size_t NetPacketWrapperCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketWrapperAckCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketWrapperAckCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.originalPlayerId = cmdMsg->getOriginalPlayerID();
	data.wrapSeq = cmdMsg->getWrapSeq();
	data.chunkNumber = cmdMsg->getChunkNumber();

	return network::writeObject(buffer, data);
}

size_t NetPacketWrapperAckCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.playerId.playerId = msg->getPlayerID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketFileCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);

	size_t size = 0;
	size += cmdMsg->getPortableFilename().getByteCount() + 1;
	size += sizeof(UnsignedInt);
	size += cmdMsg->getFileLength();
	return size;
}

size_t NetPacketFileCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());

	size_t size = 0;
	size += network::writeStringWithNull(buffer + size, cmdMsg->getPortableFilename());
	size += network::writePrimitive(buffer + size, (UnsignedInt)cmdMsg->getFileLength());
	size += network::writeBytes(buffer + size, cmdMsg->getFileData(), cmdMsg->getFileLength());
	return size;
}

size_t NetPacketFileCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileAnnounceCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketFileAnnounceCommandData::getSize(const NetCommandMsg &msg)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(&msg);

	size_t size = 0;
	size += cmdMsg->getPortableFilename().getByteCount() + 1;
	size += sizeof(UnsignedShort);
	// GeneralsX: sizeof(RelayMaskType), not sizeof(UnsignedByte). The mask
	// was widened for twelve human slots and copyBytes below followed it; this did not, and this is
	// the ONLY size estimator in the file that disagreed with its own writer.
	//
	// It is not a cosmetic byte. getSize is the room check NetPacket::addCommand makes before
	// copyBytes writes into m_packet[MAX_PACKET_SIZE] - so under-reporting by one byte lets a
	// file-announce that lands exactly on the boundary write one byte past the end of the packet
	// buffer. Rare by construction and therefore the worst kind: it needs a map transfer whose
	// announce falls at a particular offset in a particular packet.
	size += sizeof(RelayMaskType);
	return size;
}

size_t NetPacketFileAnnounceCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());

	size_t size = 0;
	size += network::writeStringWithNull(buffer + size, cmdMsg->getPortableFilename());
	size += network::writePrimitive(buffer + size, (UnsignedShort)cmdMsg->getFileID());
	size += network::writePrimitive(buffer + size, (RelayMaskType)cmdMsg->getPlayerMask());	// GeneralsX: widened - wire grows 1 byte
	return size;
}

size_t NetPacketFileAnnounceCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileProgressCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketFileProgressCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.fileId = cmdMsg->getFileID();
	data.progress = cmdMsg->getProgress();

	return network::writeObject(buffer, data);
}

size_t NetPacketFileProgressCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketProgressCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketProgressCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.percentage = cmdMsg->getPercentage();

	return network::writeObject(buffer, data);
}

size_t NetPacketProgressCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	//base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketLoadCompleteCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketLoadCompleteCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketTimeOutGameStartCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketTimeOutGameStartCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectFrameCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectFrameCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.disconnectFrame = cmdMsg->getDisconnectFrame();

	return network::writeObject(buffer, data);
}

size_t NetPacketDisconnectFrameCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectScreenOffCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketDisconnectScreenOffCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.newFrame = cmdMsg->getNewFrame();

	return network::writeObject(buffer, data);
}

size_t NetPacketDisconnectScreenOffCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameResendRequestCommand
////////////////////////////////////////////////////////////////////////////////

size_t NetPacketFrameResendRequestCommandData::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const CommandMsg *cmdMsg = static_cast<const CommandMsg *>(ref.getCommand());
	FixedData data;
	data.frameToResend = cmdMsg->getFrameToResend();

	return network::writeObject(buffer, data);
}

size_t NetPacketFrameResendRequestCommandBase::copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
{
	const NetCommandMsg *msg = ref.getCommand();
	CommandBase base;
	base.commandType.commandType = msg->getNetCommandType();
	base.relay.relay = ref.getRelay();
	//base.frame.frame = msg->getExecutionFrame();
	base.playerId.playerId = msg->getPlayerID();
	base.commandId.commandId = msg->getID();

	return network::writeObject(buffer, base);
}
