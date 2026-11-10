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

//------------------------------------------------------------------------------------
// The CommandMsg class is a linked-list wrapper around GameMessage objects that
// are queued up to execute at a later time

#pragma once

#include "Lib/BaseType.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetPacketStructs.h"
#include "Common/UnicodeString.h"

class GameMessageArgument;
class NetCommandRef;

//-----------------------------------------------------------------------------
class NetCommandDataChunk
{
	NetCommandDataChunk(const NetCommandDataChunk&) FUNCTION_DELETE;
	void operator=(const NetCommandDataChunk&) FUNCTION_DELETE;

public:
	NetCommandDataChunk(Byte *data, UnsignedInt size)
		: m_data(reinterpret_cast<UnsignedByte *>(data))
		, m_size(size)
	{}

	NetCommandDataChunk(UnsignedByte *data, UnsignedInt size)
		: m_data(data)
		, m_size(size)
	{}

	NetCommandDataChunk(UnsignedInt size)
		: m_data(NEW UnsignedByte[size])
		, m_size(size)
	{}

	~NetCommandDataChunk()
	{
		delete[] m_data;
	}

	const UnsignedByte *data() const
	{
		return m_data;
	}

	UnsignedByte *data()
	{
		return m_data;
	}

	UnsignedInt size() const
	{
		return m_size;
	}

	UnsignedByte *release()
	{
		UnsignedByte *ret = m_data;
		m_data = nullptr;
		m_size = 0;
		return ret;
	}

private:
	UnsignedByte *m_data;
	UnsignedInt m_size;
};

//-----------------------------------------------------------------------------
class NetCommandMsg : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetCommandMsg, "NetCommandMsg")
public:
	typedef SmallNetPacketCommandBaseSelect Select;

	NetCommandMsg();
	//virtual ~NetCommandMsg();
	UnsignedInt GetTimestamp() { return m_timestamp; }
	void SetTimestamp(UnsignedInt timestamp) { m_timestamp = timestamp; }
	void setExecutionFrame(UnsignedInt frame) { m_executionFrame = frame; }
	void setPlayerID(UnsignedInt playerID) { m_playerID = playerID; }
	void setID(UnsignedShort id) { m_id = id; }
	UnsignedInt getExecutionFrame() const { return m_executionFrame; }
	UnsignedInt getPlayerID() const { return m_playerID; }
	UnsignedShort getID() const { return m_id; }
	void setNetCommandType(NetCommandType type) { m_commandType = type; }
	NetCommandType getNetCommandType() const { return m_commandType; }
	virtual Int getSortNumber() const;
	virtual size_t getSizeForNetPacket() const = 0;
	virtual size_t copyBytesForNetPacket(UnsignedByte* buffer, const NetCommandRef& ref) const = 0;
	virtual size_t getSizeForSmallNetPacket(const Select* select = nullptr) const = 0;
	virtual size_t copyBytesForSmallNetPacket(UnsignedByte* buffer, const NetCommandRef& ref, const Select* select = nullptr) const = 0;
	virtual Select getSmallNetPacketSelect() const = 0;
	void attach();
	void detach();

protected:
	UnsignedInt m_timestamp;
	UnsignedInt m_executionFrame;
	UnsignedInt m_playerID;
	UnsignedShort m_id;

	NetCommandType m_commandType;
	Int m_referenceCount;
};

//-----------------------------------------------------------------------------
template<typename NetPacketType, typename SmallNetPacketType>
class NetCommandMsgT : public NetCommandMsg
{
public:
	virtual size_t getSizeForNetPacket() const final
	{
		return NetPacketType::getSize(*this);
	}

	virtual size_t copyBytesForNetPacket(UnsignedByte* buffer, const NetCommandRef& ref) const final
	{
		return NetPacketType::copyBytes(buffer, ref);
	}

	virtual size_t getSizeForSmallNetPacket(const Select* select = nullptr) const final
	{
		return SmallNetPacketType::getSize(*this, select);
	}

	virtual size_t copyBytesForSmallNetPacket(UnsignedByte* buffer, const NetCommandRef& ref, const Select* select = nullptr) const final
	{
		return SmallNetPacketType::copyBytes(buffer, ref, select);
	}
};

//-----------------------------------------------------------------------------
/**
 * The NetGameCommandMsg is the NetCommandMsg representation of a GameMessage
 */
class NetGameCommandMsg : public NetCommandMsgT<NetPacketGameCommand, SmallNetPacketGameCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetGameCommandMsg, "NetGameCommandMsg")
public:
	NetGameCommandMsg();
	NetGameCommandMsg(GameMessage *msg);
	//virtual ~NetGameCommandMsg();

	GameMessage *constructGameMessage() const;
	void addArgument(const GameMessageArgumentDataType type, GameMessageArgumentType arg);
	void setGameMessageType(GameMessage::Type type);
	GameMessage::Type getGameMessageType() const;

	virtual Select getSmallNetPacketSelect() const override;

protected:
	GameMessage::Type m_type;
	std::vector<GameMessageArgument*> m_argList;
};

//-----------------------------------------------------------------------------
/**
 * The NetAckCommandMsg is the base class for other ack command messages.
 */
class NetAckCommandMsg : public NetCommandMsgT<NetPacketAckCommand, SmallNetPacketAckCommand>
{
protected:
	NetAckCommandMsg(NetCommandMsg *msg)
	{
		m_commandID = msg->getID();
		m_originalPlayerID = msg->getPlayerID();
	}
	NetAckCommandMsg()
	{
		m_commandID = 0;
		m_originalPlayerID = 0;
	}

public:
	UnsignedShort getCommandID() const;
	void setCommandID(UnsignedShort commandID);
	UnsignedByte getOriginalPlayerID() const;
	void setOriginalPlayerID(UnsignedByte originalPlayerID);
	virtual Int getSortNumber() const override;

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedShort m_commandID;
	UnsignedByte m_originalPlayerID;
};

//-----------------------------------------------------------------------------
/**
 * The NetAckBothCommandMsg is the NetCommandMsg representation of the combination of a
 * stage 1 ack and a stage 2 ack.
 */
class NetAckBothCommandMsg : public NetAckCommandMsg
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetAckBothCommandMsg, "NetAckBothCommandMsg")
public:
	NetAckBothCommandMsg(NetCommandMsg *msg);
	NetAckBothCommandMsg();
	//virtual ~NetAckBothCommandMsg();
};

//-----------------------------------------------------------------------------
/**
 * The NetAckStage1CommandMsg is the NetCommandMsg representation of an ack message for the initial
 * recipient.
 */
class NetAckStage1CommandMsg : public NetAckCommandMsg
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetAckStage1CommandMsg, "NetAckStage1CommandMsg")
public:
	NetAckStage1CommandMsg(NetCommandMsg *msg);
	NetAckStage1CommandMsg();
	//virtual ~NetAckStage1CommandMsg();
};

//-----------------------------------------------------------------------------
/**
 * The NetAckStage2CommandMsg is the NetCommandMsg representation of an ack message for all eventual
 * recipients. (when this is returned, all the players in the relay mask have received the packet)
 */
class NetAckStage2CommandMsg : public NetAckCommandMsg
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetAckStage2CommandMsg, "NetAckStage2CommandMsg")
public:
	NetAckStage2CommandMsg(NetCommandMsg *msg);
	NetAckStage2CommandMsg();
	//virtual ~NetAckStage2CommandMsg();
};

//-----------------------------------------------------------------------------
class NetFrameCommandMsg : public NetCommandMsgT<NetPacketFrameCommand, SmallNetPacketFrameCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetFrameCommandMsg, "NetFrameCommandMsg")
public:
	NetFrameCommandMsg();
	//virtual ~NetFrameCommandMsg();

	void setCommandCount(UnsignedShort commandCount);
	UnsignedShort getCommandCount() const;

	// GeneralsX: queued by ConnectionManager::sendSingleFrameToPlayer, so its ack times nothing - see processAckStage1.
	// Local bookkeeping only: never written to the wire, FALSE on every FRAMEINFO read from a packet.
	void setResent(Bool resent) { m_resent = resent; }
	Bool isResent() const { return m_resent; }

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedShort m_commandCount;
	Bool m_resent;
};

//-----------------------------------------------------------------------------
class NetPlayerLeaveCommandMsg : public NetCommandMsgT<NetPacketPlayerLeaveCommand, SmallNetPacketPlayerLeaveCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetPlayerLeaveCommandMsg, "NetPlayerLeaveCommandMsg")
public:
	NetPlayerLeaveCommandMsg();
	//virtual ~NetPlayerLeaveCommandMsg();

	UnsignedByte getLeavingPlayerID() const;
	void setLeavingPlayerID(UnsignedByte id);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedByte m_leavingPlayerID;
};

//-----------------------------------------------------------------------------
class NetRunAheadMetricsCommandMsg : public NetCommandMsgT<NetPacketRunAheadMetricsCommand, SmallNetPacketRunAheadMetricsCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetRunAheadMetricsCommandMsg, "NetRunAheadMetricsCommandMsg")
public:
	NetRunAheadMetricsCommandMsg();
	//virtual ~NetRunAheadMetricsCommandMsg();

	Real getAverageLatency() const;
	void setAverageLatency(Real avgLat);
	Int  getAverageFps() const;
	void setAverageFps(Int fps);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	Real m_averageLatency;
	Int  m_averageFps;
};

//-----------------------------------------------------------------------------
class NetRunAheadCommandMsg : public NetCommandMsgT<NetPacketRunAheadCommand, SmallNetPacketRunAheadCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetRunAheadCommandMsg, "NetRunAheadCommandMsg")
public:
	NetRunAheadCommandMsg();
	//virtual ~NetRunAheadCommandMsg();

	UnsignedShort getRunAhead() const;
	void setRunAhead(UnsignedShort runAhead);

	UnsignedByte getFrameRate() const;
	void setFrameRate(UnsignedByte frameRate);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedShort m_runAhead;
	UnsignedByte m_frameRate;
};

//-----------------------------------------------------------------------------
// GeneralsX: A flag in DESTROYPLAYER's slot word: hand the victim's assets to
// a living ally instead of destroying them, as its own quit would have. Set only for a hard quit (see
// DisconnectManager::processDisconnectPlayer). Trade-off: rides the oversized index word rather than a
// new wire field; give it its own field the next time this packet's layout changes anyway.
enum { DESTROYPLAYER_TRANSFER_ASSETS = 0x100 };

class NetDestroyPlayerCommandMsg : public NetCommandMsgT<NetPacketDestroyPlayerCommand, SmallNetPacketDestroyPlayerCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDestroyPlayerCommandMsg, "NetDestroyPlayerCommandMsg")
public:
	NetDestroyPlayerCommandMsg();
	//virtual ~NetDestroyPlayerCommandMsg();

	UnsignedInt getPlayerIndex() const;
	void setPlayerIndex(UnsignedInt playerIndex);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedInt m_playerIndex;
};

//-----------------------------------------------------------------------------
class NetKeepAliveCommandMsg : public NetCommandMsgT<NetPacketKeepAliveCommand, SmallNetPacketKeepAliveCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetKeepAliveCommandMsg, "NetKeepAliveCommandMsg")
public:
	NetKeepAliveCommandMsg();
	//virtual ~NetKeepAliveCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};

//-----------------------------------------------------------------------------
// GeneralsX: The packet router's latency row.
//
// One slot-indexed array of round trips in MILLISECONDS. Only the router fills it in; every peer
// receives it and reads it for display only. See NetPacketLatencyReportCommandData for why the
// unit is integer milliseconds and why the payload is fixed size.
//
// setLatencyMs clamps rather than asserts. The values reaching it on the SEND side come from the
// engine's own measurements, but the class is also what the READ side populates straight off the
// wire, and a peer is free to lie; a single clamp here means neither path can seat an
// out-of-range number into the array behind it.
class NetLatencyReportCommandMsg : public NetCommandMsgT<NetPacketLatencyReportCommand, SmallNetPacketLatencyReportCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetLatencyReportCommandMsg, "NetLatencyReportCommandMsg")
public:
	NetLatencyReportCommandMsg();
	//virtual ~NetLatencyReportCommandMsg();

	/// Round trip to @p slot in milliseconds, or 0 for "this slot has no live measurement".
	UnsignedShort getLatencyMs(Int slot) const;
	void setLatencyMs(Int slot, UnsignedShort ms);

	/// GeneralsX: The router's average render fps for @p slot (what the run-ahead clamp reads),
	/// 0..254, or -1 for "no figure". Display only, like the latency row it rides with.
	Int getAverageFps(Int slot) const;
	void setAverageFps(Int slot, Int fps);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedShort m_latencyMs[MAX_SLOTS];
	Short m_fps[MAX_SLOTS];		///< -1 = no figure
};

//-----------------------------------------------------------------------------
class NetDisconnectKeepAliveCommandMsg : public NetCommandMsgT<NetPacketDisconnectKeepAliveCommand, SmallNetPacketDisconnectKeepAliveCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectKeepAliveCommandMsg, "NetDisconnectKeepAliveCommandMsg")
public:
	NetDisconnectKeepAliveCommandMsg();
	//virtual ~NetDisconnectKeepAliveCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};

//-----------------------------------------------------------------------------
class NetDisconnectPlayerCommandMsg : public NetCommandMsgT<NetPacketDisconnectPlayerCommand, SmallNetPacketDisconnectPlayerCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectPlayerCommandMsg, "NetDisconnectPlayerCommandMsg")
public:
	NetDisconnectPlayerCommandMsg();
	//virtual ~NetDisconnectPlayerCommandMsg();

	UnsignedByte getDisconnectSlot() const;
	void setDisconnectSlot(UnsignedByte slot);

	UnsignedInt getDisconnectFrame() const;
	void setDisconnectFrame(UnsignedInt frame);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedByte m_disconnectSlot;
	UnsignedInt m_disconnectFrame;
};

//-----------------------------------------------------------------------------
class NetPacketRouterQueryCommandMsg : public NetCommandMsgT<NetPacketRouterQueryCommand, SmallNetPacketRouterQueryCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetPacketRouterQueryCommandMsg, "NetPacketRouterQueryCommandMsg")
public:
	NetPacketRouterQueryCommandMsg();
	//virtual ~NetPacketRouterQueryCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};

//-----------------------------------------------------------------------------
class NetPacketRouterAckCommandMsg : public NetCommandMsgT<NetPacketRouterAckCommand, SmallNetPacketRouterAckCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetPacketRouterAckCommandMsg, "NetPacketRouterAckCommandMsg")
public:
	NetPacketRouterAckCommandMsg();
	//virtual ~NetPacketRouterAckCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};

//-----------------------------------------------------------------------------
class NetDisconnectChatCommandMsg : public NetCommandMsgT<NetPacketDisconnectChatCommand, SmallNetPacketDisconnectChatCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectChatCommandMsg, "NetDisconnectChatCommandMsg")
public:
	NetDisconnectChatCommandMsg();
	//virtual ~NetDisconnectChatCommandMsg();

	UnicodeString getText() const;
	void setText(UnicodeString text);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnicodeString m_text;
};

//-----------------------------------------------------------------------------
class NetChatCommandMsg : public NetCommandMsgT<NetPacketChatCommand, SmallNetPacketChatCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetChatCommandMsg, "NetChatCommandMsg")
public:
	NetChatCommandMsg();
	//virtual ~NetChatCommandMsg();

	UnicodeString getText() const;
	void setText(UnicodeString text);

	RelayMaskType getPlayerMask() const;	// GeneralsX: scale32: 64 bit
	void setPlayerMask( RelayMaskType playerMask );

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnicodeString m_text;
	RelayMaskType m_playerMask;
};

//-----------------------------------------------------------------------------
class NetDisconnectVoteCommandMsg : public NetCommandMsgT<NetPacketDisconnectVoteCommand, SmallNetPacketDisconnectVoteCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectVoteCommandMsg, "NetDisconnectVoteCommandMsg")
public:
	NetDisconnectVoteCommandMsg();
	//virtual ~NetDisconnectVoteCommandMsg();

	UnsignedByte getSlot() const;
	void setSlot(UnsignedByte slot);

	UnsignedInt getVoteFrame() const;
	void setVoteFrame(UnsignedInt voteFrame);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedByte m_slot;
	UnsignedInt m_voteFrame;
};

//-----------------------------------------------------------------------------
class NetProgressCommandMsg: public NetCommandMsgT<NetPacketProgressCommand, SmallNetPacketProgressCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetProgressCommandMsg, "NetProgressCommandMsg")
public:
	NetProgressCommandMsg();
	//virtual ~NetProgressCommandMsg();

	UnsignedByte getPercentage() const;
	void setPercentage( UnsignedByte percent );

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedByte m_percent;
};

//-----------------------------------------------------------------------------
class NetWrapperCommandMsg : public NetCommandMsgT<NetPacketWrapperCommand, SmallNetPacketWrapperCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetWrapperCommandMsg, "NetWrapperCommandMsg")
public:
	NetWrapperCommandMsg();
	//virtual ~NetWrapperCommandMsg();

	const UnsignedByte * getData() const;
	UnsignedByte * getData();
	void setData(UnsignedByte *data, UnsignedInt dataLength);

	UnsignedInt getChunkNumber() const;
	void setChunkNumber(UnsignedInt chunkNumber);

	UnsignedInt getNumChunks() const;
	void setNumChunks(UnsignedInt numChunks);

	UnsignedInt getDataLength() const;

	UnsignedInt getTotalDataLength() const;
	void setTotalDataLength(UnsignedInt totalDataLength);

	UnsignedInt getDataOffset() const;
	void setDataOffset(UnsignedInt offset);

	UnsignedShort getWrappedCommandID() const;
	void setWrappedCommandID(UnsignedShort wrappedCommandID);

	// GeneralsX: The wrapping hop's per-connection serialization number and its lowWater
	// (v16 section 2; NetPacketWrapperCommandData::FixedData).
	UnsignedInt getWrapSeq() const { return m_wrapSeq; }
	void setWrapSeq(UnsignedInt wrapSeq) { m_wrapSeq = wrapSeq; }
	UnsignedInt getLowWater() const { return m_lowWater; }
	void setLowWater(UnsignedInt lowWater) { m_lowWater = lowWater; }

	virtual Select getSmallNetPacketSelect() const override;

private:
	UnsignedInt m_wrapSeq;
	UnsignedInt m_lowWater;
	UnsignedByte *m_data;
	// using UnsignedInt's so we can send around files of effectively unlimited size easily
	UnsignedInt m_dataLength;
	UnsignedInt m_dataOffset;
	UnsignedInt m_totalDataLength;
	UnsignedInt m_chunkNumber;
	UnsignedInt m_numChunks;
	UnsignedShort m_wrappedCommandID;
};

//-----------------------------------------------------------------------------
/**
 * GeneralsX: The ack of ONE wrapper chunk: (original player, wrapSeq, chunk number), matched only
 * against wrapper chunks queued on the connection it arrived from (v16 section 2).
 */
class NetWrapperAckCommandMsg : public NetCommandMsgT<NetPacketWrapperAckCommand, SmallNetPacketWrapperAckCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetWrapperAckCommandMsg, "NetWrapperAckCommandMsg")
public:
	NetWrapperAckCommandMsg();
	//virtual ~NetWrapperAckCommandMsg();

	UnsignedByte getOriginalPlayerID() const { return m_originalPlayerID; }
	void setOriginalPlayerID(UnsignedByte originalPlayerID) { m_originalPlayerID = originalPlayerID; }
	UnsignedInt getWrapSeq() const { return m_wrapSeq; }
	void setWrapSeq(UnsignedInt wrapSeq) { m_wrapSeq = wrapSeq; }
	UnsignedInt getChunkNumber() const { return m_chunkNumber; }
	void setChunkNumber(UnsignedInt chunkNumber) { m_chunkNumber = chunkNumber; }

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedByte m_originalPlayerID;
	UnsignedInt m_wrapSeq;
	UnsignedInt m_chunkNumber;
};

//-----------------------------------------------------------------------------
class NetFileCommandMsg : public NetCommandMsgT<NetPacketFileCommand, SmallNetPacketFileCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetFileCommandMsg, "NetFileCommandMsg")
public:
	NetFileCommandMsg();
	//virtual ~NetFileCommandMsg();

	AsciiString getRealFilename() const;
	void setRealFilename(AsciiString filename);

	AsciiString getPortableFilename() const { return m_portableFilename; }
	void setPortableFilename(AsciiString filename) { m_portableFilename = filename; }

	UnsignedInt getFileLength() const;

	const UnsignedByte * getFileData() const;
	UnsignedByte * getFileData();
	void setFileData(UnsignedByte *data, UnsignedInt dataLength);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	AsciiString m_portableFilename;

	UnsignedByte *m_data;
	UnsignedInt m_dataLength;
};

//-----------------------------------------------------------------------------
class NetFileAnnounceCommandMsg : public NetCommandMsgT<NetPacketFileAnnounceCommand, SmallNetPacketFileAnnounceCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetFileAnnounceCommandMsg, "NetFileAnnounceCommandMsg")
public:
	NetFileAnnounceCommandMsg();
	//virtual ~NetFileAnnounceCommandMsg();

	AsciiString getRealFilename() const;
	void setRealFilename(AsciiString filename);

	AsciiString getPortableFilename() const { return m_portableFilename; }
	void setPortableFilename(AsciiString filename) { m_portableFilename = filename; }

	UnsignedShort getFileID() const;
	void setFileID(UnsignedShort fileID);

	RelayMaskType getPlayerMask() const;
	void setPlayerMask(RelayMaskType playerMask);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	AsciiString m_portableFilename;
	UnsignedShort m_fileID;
	RelayMaskType m_playerMask;	// GeneralsX: widened with RelayMaskType - the map-transfer recipient mask must address slots 8-11
};

//-----------------------------------------------------------------------------
class NetFileProgressCommandMsg : public NetCommandMsgT<NetPacketFileProgressCommand, SmallNetPacketFileProgressCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetFileProgressCommandMsg, "NetFileProgressCommandMsg")
public:
	NetFileProgressCommandMsg();
	//virtual ~NetFileProgressCommandMsg();

	UnsignedShort getFileID() const;
	void setFileID(UnsignedShort val);

	Int getProgress() const;
	void setProgress(Int val);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedShort m_fileID;
	Int m_progress;
};

//-----------------------------------------------------------------------------
class NetDisconnectFrameCommandMsg : public NetCommandMsgT<NetPacketDisconnectFrameCommand, SmallNetPacketDisconnectFrameCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectFrameCommandMsg, "NetDisconnectFrameCommandMsg")
public:
	NetDisconnectFrameCommandMsg();

	UnsignedInt getDisconnectFrame() const;
	void setDisconnectFrame(UnsignedInt disconnectFrame);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedInt m_disconnectFrame;
};

//-----------------------------------------------------------------------------
class NetDisconnectScreenOffCommandMsg : public NetCommandMsgT<NetPacketDisconnectScreenOffCommand, SmallNetPacketDisconnectScreenOffCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetDisconnectScreenOffCommandMsg, "NetDisconnectScreenOffCommandMsg")
public:
	NetDisconnectScreenOffCommandMsg();

	UnsignedInt getNewFrame() const;
	void setNewFrame(UnsignedInt newFrame);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedInt m_newFrame;
};

//-----------------------------------------------------------------------------
class NetFrameResendRequestCommandMsg : public NetCommandMsgT<NetPacketFrameResendRequestCommand, SmallNetPacketFrameResendRequestCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetFrameResendRequestCommandMsg, "NetFrameResendRequestCommandMsg")
public:
	NetFrameResendRequestCommandMsg();

	UnsignedInt getFrameToResend() const;
	void setFrameToResend(UnsignedInt frame);

	virtual Select getSmallNetPacketSelect() const override;

protected:
	UnsignedInt m_frameToResend;
};

class NetLoadCompleteCommandMsg : public NetCommandMsgT<NetPacketLoadCompleteCommand, SmallNetPacketLoadCompleteCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetLoadCompleteCommandMsg, "NetLoadCompleteCommandMsg")
public:
	NetLoadCompleteCommandMsg();
	//virtual ~NetLoadCompleteCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};

class NetTimeOutGameStartCommandMsg : public NetCommandMsgT<NetPacketTimeOutGameStartCommand, SmallNetPacketTimeOutGameStartCommand>
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(NetTimeOutGameStartCommandMsg, "NetTimeOutGameStartCommandMsg")
public:
	NetTimeOutGameStartCommandMsg();
	//virtual ~NetTimeOutGameStartCommandMsg();

	virtual Select getSmallNetPacketSelect() const override;
};
