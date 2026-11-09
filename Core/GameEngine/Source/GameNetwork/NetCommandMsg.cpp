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

#include "GameNetwork/NetCommandMsg.h"
#include "GameNetwork/NetPacketStructs.h"
#include "GameNetwork/GameMessageParser.h"
#include "Common/GameState.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"

/**
 * Base constructor
 */
NetCommandMsg::NetCommandMsg()
{
	m_executionFrame = 0;
	m_id = 0;
	m_playerID = 0;
	m_timestamp = 0;
	m_referenceCount = 1; // start this off as 1.  This means that an "attach" is implied by creating a NetCommandMsg object.
	m_commandType = NETCOMMANDTYPE_UNKNOWN;
}

/**
 * Destructor
 */
NetCommandMsg::~NetCommandMsg() {
}

/**
 * Adds one to the reference count.
 */
void NetCommandMsg::attach() {
	++m_referenceCount;
}

/**
 * Subtracts one from the reference count. If the reference count is 0, the this object is destroyed.
 */
void NetCommandMsg::detach() {
	--m_referenceCount;
	if (m_referenceCount == 0) {
		deleteInstance(this);
		return;
	}
	DEBUG_ASSERTCRASH(m_referenceCount > 0, ("Invalid reference count for NetCommandMsg")); // Just to make sure...
	if (m_referenceCount < 0) {
		deleteInstance(this);
	}
}

/**
 * Returns the value by which this type of message should be sorted.
 */
Int NetCommandMsg::getSortNumber() const {
	return m_id;
}

//-------------------------------
// NetGameCommandMsg
//-------------------------------

/**
 * Constructor with no argument, sets everything to default values.
 */
NetGameCommandMsg::NetGameCommandMsg() {
	m_type = (GameMessage::Type)0;
	m_commandType = NETCOMMANDTYPE_GAMECOMMAND;
}

/**
 * Constructor with a GameMessage argument. Sets member variables appropriately for this GameMessage.
 * Also copies all the arguments.
 */
NetGameCommandMsg::NetGameCommandMsg(GameMessage *msg) {
	m_commandType = NETCOMMANDTYPE_GAMECOMMAND;
	m_type = msg->getType();

	const size_t argsCount = msg->getArgumentCount();
	m_argList.reserve(argsCount);

	for (size_t i = 0; i < argsCount; ++i) {
		GameMessageArgumentDataType argType = msg->getArgumentDataType(i);
		const GameMessageArgumentType* arg = msg->getArgument(i);
		addArgument(argType, *arg);
	}
}

/**
 * Destructor
 */
NetGameCommandMsg::~NetGameCommandMsg() {
	for (size_t i = 0; i < m_argList.size(); ++i) {
		deleteInstance(m_argList[i]);
	}
}

/**
 * Add an argument to this command.
 */
void NetGameCommandMsg::addArgument(const GameMessageArgumentDataType type, GameMessageArgumentType arg)
{
	GameMessageArgument *newArg = newInstance(GameMessageArgument);
	newArg->m_data = arg;
	newArg->m_type = type;
	m_argList.push_back(newArg);
}

// here's where we figure out which slot corresponds to which player
static Int indexFromMask(PlayerMaskType mask)
{
	Player *player = nullptr;
	Int i;

	for( i = 0; i < MAX_PLAYER_COUNT; i++ )
	{
		player = ThePlayerList->getNthPlayer( i );
		if( player && player->getPlayerMask() == mask )
			return i;
	}

	return -1;
}

/**
 * Construct a new GameMessage object from the data in this object.
 */
GameMessage *NetGameCommandMsg::constructGameMessage() const
{
	GameMessage *retval = newInstance(GameMessage)(m_type);

	retval->friend_setPlayerIndex(ThePlayerList->getPlayerFromSlotIndex(getPlayerID())->getPlayerIndex());
	// GeneralsX @feature Claude 24/09/2026 Keep the slot too: several observer slots share one Player.
	retval->friend_setSenderSlot(getPlayerID());

	for (size_t i = 0; i < m_argList.size(); ++i) {
		const GameMessageArgument* arg = m_argList[i];
		switch (arg->m_type) {

		case ARGUMENTDATATYPE_INTEGER:
			retval->appendIntegerArgument(arg->m_data.integer);
			break;
		case ARGUMENTDATATYPE_REAL:
			retval->appendRealArgument(arg->m_data.real);
			break;
		case ARGUMENTDATATYPE_BOOLEAN:
			retval->appendBooleanArgument(arg->m_data.boolean);
			break;
		case ARGUMENTDATATYPE_OBJECTID:
			retval->appendObjectIDArgument(arg->m_data.objectID);
			break;
		case ARGUMENTDATATYPE_DRAWABLEID:
			retval->appendDrawableIDArgument(arg->m_data.drawableID);
			break;
		case ARGUMENTDATATYPE_TEAMID:
			retval->appendTeamIDArgument(arg->m_data.teamID);
			break;
		case ARGUMENTDATATYPE_LOCATION:
			retval->appendLocationArgument(arg->m_data.location);
			break;
		case ARGUMENTDATATYPE_PIXEL:
			retval->appendPixelArgument(arg->m_data.pixel);
			break;
		case ARGUMENTDATATYPE_PIXELREGION:
			retval->appendPixelRegionArgument(arg->m_data.pixelRegion);
			break;
		case ARGUMENTDATATYPE_TIMESTAMP:
			retval->appendTimestampArgument(arg->m_data.timestamp);
			break;
		case ARGUMENTDATATYPE_WIDECHAR:
			retval->appendWideCharArgument(arg->m_data.wChar);
			break;

		}
	}
	return retval;
}

/**
 * Sets the type of game message
 */
void NetGameCommandMsg::setGameMessageType(GameMessage::Type type) {
	m_type = type;
}

GameMessage::Type NetGameCommandMsg::getGameMessageType() const {
	return m_type;
}

NetCommandMsg::Select NetGameCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetAckCommandMsg
//-------------------------

/**
 * Returns the command ID of the command being ack'd.
 */
UnsignedShort NetAckCommandMsg::getCommandID() const {
	return m_commandID;
}

/**
 * Set the command ID of the command being ack'd.
 */
void NetAckCommandMsg::setCommandID(UnsignedShort commandID) {
	m_commandID = commandID;
}

/**
 * Get the player id of the player who originally sent the command.
 */
UnsignedByte NetAckCommandMsg::getOriginalPlayerID() const {
	return m_originalPlayerID;
}

/**
 * Set the player id of the player who originally sent the command.
 */
void NetAckCommandMsg::setOriginalPlayerID(UnsignedByte originalPlayerID) {
	m_originalPlayerID = originalPlayerID;
}

Int NetAckCommandMsg::getSortNumber() const {
	return m_commandID;
}

NetCommandMsg::Select NetAckCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 0;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetAckBothCommandMsg
//-------------------------
/**
 * Constructor.  Sets the member variables according to the given message.
 */
NetAckBothCommandMsg::NetAckBothCommandMsg(NetCommandMsg *msg) : NetAckCommandMsg(msg) {
	m_commandType = NETCOMMANDTYPE_ACKBOTH;
}

/**
 * Constructor.  Sets the member variables to default values.
 */
NetAckBothCommandMsg::NetAckBothCommandMsg() {
	m_commandType = NETCOMMANDTYPE_ACKBOTH;
}

/**
 * Destructor.
 */
NetAckBothCommandMsg::~NetAckBothCommandMsg() {
}

//-------------------------
// NetAckStage1CommandMsg
//-------------------------
/**
 * Constructor.  Sets the member variables according to the given message.
 */
NetAckStage1CommandMsg::NetAckStage1CommandMsg(NetCommandMsg *msg) : NetAckCommandMsg(msg) {
	m_commandType = NETCOMMANDTYPE_ACKSTAGE1;
}

/**
 * Constructor.  Sets the member variables to default values.
 */
NetAckStage1CommandMsg::NetAckStage1CommandMsg() {
	m_commandType = NETCOMMANDTYPE_ACKSTAGE1;
}

/**
 * Destructor.
 */
NetAckStage1CommandMsg::~NetAckStage1CommandMsg() {
}

//-------------------------
// NetAckStage2CommandMsg
//-------------------------
/**
 * Constructor.  Sets the member variables according to the given message.
 */
NetAckStage2CommandMsg::NetAckStage2CommandMsg(NetCommandMsg *msg) : NetAckCommandMsg(msg) {
	m_commandType = NETCOMMANDTYPE_ACKSTAGE2;
}

/**
 * Constructor.  Sets the member variables to default values.
 */
NetAckStage2CommandMsg::NetAckStage2CommandMsg() {
	m_commandType = NETCOMMANDTYPE_ACKSTAGE2;
}

/**
 * Destructor.
 */
NetAckStage2CommandMsg::~NetAckStage2CommandMsg() {
}

//-------------------------
// NetFrameCommandMsg
//-------------------------
/**
 * Constructor.
 */
NetFrameCommandMsg::NetFrameCommandMsg() {
	m_commandCount = 0;
	m_resent = FALSE;
	m_commandType = NETCOMMANDTYPE_FRAMEINFO;
}

/**
 * Destructor
 */
NetFrameCommandMsg::~NetFrameCommandMsg() {
}

/**
 * Set the command count of this frame.
 */
void NetFrameCommandMsg::setCommandCount(UnsignedShort commandCount) {
	m_commandCount = commandCount;
}

/**
 * Return the command count of this frame.
 */
UnsignedShort NetFrameCommandMsg::getCommandCount() const {
	return m_commandCount;
}

NetCommandMsg::Select NetFrameCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetPlayerLeaveCommandMsg
//-------------------------
/**
 * Constructor
 */
NetPlayerLeaveCommandMsg::NetPlayerLeaveCommandMsg() {
	m_leavingPlayerID = 0;
	m_commandType = NETCOMMANDTYPE_PLAYERLEAVE;
}

/**
 * Destructor
 */
NetPlayerLeaveCommandMsg::~NetPlayerLeaveCommandMsg() {
}

/**
 * Get the id of the player leaving the game.
 */
UnsignedByte NetPlayerLeaveCommandMsg::getLeavingPlayerID() const {
	return m_leavingPlayerID;
}

/**
 * Set the id of the player leaving the game.
 */
void NetPlayerLeaveCommandMsg::setLeavingPlayerID(UnsignedByte id) {
	m_leavingPlayerID = id;
}

NetCommandMsg::Select NetPlayerLeaveCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetRunAheadMetricsCommandMsg
//-------------------------
/**
 * Constructor
 */
NetRunAheadMetricsCommandMsg::NetRunAheadMetricsCommandMsg() {
	m_averageLatency = 0.0;
	m_averageFps = 0;
	m_commandType = NETCOMMANDTYPE_RUNAHEADMETRICS;
}

/**
 * Destructor
 */
NetRunAheadMetricsCommandMsg::~NetRunAheadMetricsCommandMsg() {
}

/**
 * set the average latency
 */
void NetRunAheadMetricsCommandMsg::setAverageLatency(Real avgLat) {
	m_averageLatency = avgLat;
}

/**
 * get the average latency
 */
Real NetRunAheadMetricsCommandMsg::getAverageLatency() const {
	return m_averageLatency;
}

/**
 * set the average fps
 */
void NetRunAheadMetricsCommandMsg::setAverageFps(Int fps) {
	m_averageFps = fps;
}

/**
 * get the average fps
 */
Int NetRunAheadMetricsCommandMsg::getAverageFps() const {
	return m_averageFps;
}

NetCommandMsg::Select NetRunAheadMetricsCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetRunAheadCommandMsg
//-------------------------
NetRunAheadCommandMsg::NetRunAheadCommandMsg() {
	// GeneralsX @fix Claude 05/08/2026 Was a raw 20 frames (0.67 s at 30 Hz) and a raw 30. Same
	// reasoning as Network.cpp's initial values: a run-ahead expressed as a frame count halves in
	// wall-clock terms at 60 Hz. (2 * rate) / 3 preserves 0.67 s at either tick rate, and evaluates
	// to exactly 20 at the default 30 Hz, so this is a no-op there.
	m_runAhead = min(max((Int)((2 * LOGICFRAMES_PER_SECOND) / 3), MIN_RUNAHEAD), MAX_FRAMES_AHEAD/2);
	m_frameRate = LOGICFRAMES_PER_SECOND;
	m_commandType = NETCOMMANDTYPE_RUNAHEAD;
}

NetRunAheadCommandMsg::~NetRunAheadCommandMsg() {
}

UnsignedShort NetRunAheadCommandMsg::getRunAhead() const {
	return m_runAhead;
}

void NetRunAheadCommandMsg::setRunAhead(UnsignedShort runAhead) {
	m_runAhead = runAhead;
}

UnsignedByte NetRunAheadCommandMsg::getFrameRate() const {
	return m_frameRate;
}

void NetRunAheadCommandMsg::setFrameRate(UnsignedByte frameRate) {
	m_frameRate = frameRate;
}

NetCommandMsg::Select NetRunAheadCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetDestroyPlayerCommandMsg
//-------------------------
/**
 * Constructor
 */
NetDestroyPlayerCommandMsg::NetDestroyPlayerCommandMsg()
{
	m_playerIndex = 0;
	m_commandType = NETCOMMANDTYPE_DESTROYPLAYER;
}

/**
 * Destructor
 */
NetDestroyPlayerCommandMsg::~NetDestroyPlayerCommandMsg()
{
}

/**
 * set the CRC
 */
void NetDestroyPlayerCommandMsg::setPlayerIndex( UnsignedInt playerIndex )
{
	m_playerIndex = playerIndex;
}

/**
 * get the average CRC
 */
UnsignedInt NetDestroyPlayerCommandMsg::getPlayerIndex() const
{
	return m_playerIndex;
}

NetCommandMsg::Select NetDestroyPlayerCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetLatencyReportCommandMsg
//-------------------------
/**
 * Constructor
 */
NetLatencyReportCommandMsg::NetLatencyReportCommandMsg() {
	// Zero is the "no live measurement for this slot" sentinel, so a report that is built but
	// never filled in says "I know nothing" rather than "everyone is at 0 ms". The readout has
	// been burned by a confident zero before - see FrameMetrics::getWorstPeerLatency.
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		m_latencyMs[slot] = 0;
		m_fps[slot] = -1;
	}
	m_commandType = NETCOMMANDTYPE_LATENCYREPORT;
}

NetLatencyReportCommandMsg::~NetLatencyReportCommandMsg() {
}

UnsignedShort NetLatencyReportCommandMsg::getLatencyMs(Int slot) const {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return 0;
	}
	return m_latencyMs[slot];
}

void NetLatencyReportCommandMsg::setLatencyMs(Int slot, UnsignedShort ms) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return;
	}
	m_latencyMs[slot] = ms;
}

Int NetLatencyReportCommandMsg::getAverageFps(Int slot) const {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1;
	}
	return m_fps[slot];
}

void NetLatencyReportCommandMsg::setAverageFps(Int slot, Int fps) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return;
	}
	m_fps[slot] = (Short)((fps < 0) ? -1 : min(fps, 254));
}

NetCommandMsg::Select NetLatencyReportCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetKeepAliveCommandMsg
//-------------------------
/**
 * Constructor
 */
NetKeepAliveCommandMsg::NetKeepAliveCommandMsg() {
	m_commandType = NETCOMMANDTYPE_KEEPALIVE;
}

NetKeepAliveCommandMsg::~NetKeepAliveCommandMsg() {
}

NetCommandMsg::Select NetKeepAliveCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetDisconnectKeepAliveCommandMsg
//-------------------------
/**
 * Constructor
 */
NetDisconnectKeepAliveCommandMsg::NetDisconnectKeepAliveCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTKEEPALIVE;
}

NetDisconnectKeepAliveCommandMsg::~NetDisconnectKeepAliveCommandMsg() {
}

NetCommandMsg::Select NetDisconnectKeepAliveCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetDisconnectPlayerCommandMsg
//-------------------------
/**
 * Constructor
 */
NetDisconnectPlayerCommandMsg::NetDisconnectPlayerCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTPLAYER;
	m_disconnectSlot = 0;
}

/**
 * Destructor
 */
NetDisconnectPlayerCommandMsg::~NetDisconnectPlayerCommandMsg() {
}

/**
 * Returns the disconnecting slot number
 */
UnsignedByte NetDisconnectPlayerCommandMsg::getDisconnectSlot() const {
	return m_disconnectSlot;
}

/**
 * Sets the disconnecting slot number
 */
void NetDisconnectPlayerCommandMsg::setDisconnectSlot(UnsignedByte slot) {
	m_disconnectSlot = slot;
}

/**
 * Sets the disconnect frame
 */
void NetDisconnectPlayerCommandMsg::setDisconnectFrame(UnsignedInt frame) {
	m_disconnectFrame = frame;
}

/**
 * returns the disconnect frame
 */
UnsignedInt NetDisconnectPlayerCommandMsg::getDisconnectFrame() const {
	return m_disconnectFrame;
}

NetCommandMsg::Select NetDisconnectPlayerCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetPacketRouterQueryCommandMsg
//-------------------------
/**
 * Constructor
 */
NetPacketRouterQueryCommandMsg::NetPacketRouterQueryCommandMsg() {
	m_commandType = NETCOMMANDTYPE_PACKETROUTERQUERY;
}

/**
 * Destructor
 */
NetPacketRouterQueryCommandMsg::~NetPacketRouterQueryCommandMsg() {
}

NetCommandMsg::Select NetPacketRouterQueryCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetPacketRouterAckCommandMsg
//-------------------------
/**
 * Constructor
 */
NetPacketRouterAckCommandMsg::NetPacketRouterAckCommandMsg() {
	m_commandType = NETCOMMANDTYPE_PACKETROUTERACK;
}

/**
 * Destructor
 */
NetPacketRouterAckCommandMsg::~NetPacketRouterAckCommandMsg() {
}

NetCommandMsg::Select NetPacketRouterAckCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetDisconnectChatCommandMsg
//-------------------------
/**
 * Constructor
 */
NetDisconnectChatCommandMsg::NetDisconnectChatCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTCHAT;
}

/**
 * Destructor
 */
NetDisconnectChatCommandMsg::~NetDisconnectChatCommandMsg() {
}

/**
 * Set the chat text for this message.
 */
void NetDisconnectChatCommandMsg::setText(UnicodeString text) {
	m_text = text;
}

/**
 * Get the chat text for this message.
 */
UnicodeString NetDisconnectChatCommandMsg::getText() const {
	return m_text;
}

NetCommandMsg::Select NetDisconnectChatCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetChatCommandMsg
//-------------------------
/**
 * Constructor
 */
NetChatCommandMsg::NetChatCommandMsg()
{
	m_commandType = NETCOMMANDTYPE_CHAT;
	m_playerMask = 0;
}

/**
 * Destructor
 */
NetChatCommandMsg::~NetChatCommandMsg()
{
}

/**
 * Set the chat text for this message.
 */
void NetChatCommandMsg::setText(UnicodeString text)
{
	m_text = text;
}

/**
 * Get the chat text for this message.
 */
UnicodeString NetChatCommandMsg::getText() const
{
	return m_text;
}

/**
 * Get the bitmask of chat recipients from this message.
 */
RelayMaskType NetChatCommandMsg::getPlayerMask() const
{
	return m_playerMask;
}

/**
 * Set a bitmask of chat recipients in this message.
 */
void NetChatCommandMsg::setPlayerMask( RelayMaskType playerMask )
{
	m_playerMask = playerMask;
}

NetCommandMsg::Select NetChatCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetDisconnectVoteCommandMsg
//-------------------------
/**
 * Constructor
 */
NetDisconnectVoteCommandMsg::NetDisconnectVoteCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTVOTE;
	m_slot = 0;
}

/**
 * Destructor
 */
NetDisconnectVoteCommandMsg::~NetDisconnectVoteCommandMsg() {
}

/**
 * Set the slot that is being voted for.
 */
void NetDisconnectVoteCommandMsg::setSlot(UnsignedByte slot) {
	m_slot = slot;
}

/**
 * Get the slot that is being voted for.
 */
UnsignedByte NetDisconnectVoteCommandMsg::getSlot() const {
	return m_slot;
}

/**
 * Get the vote frame.
 */
UnsignedInt NetDisconnectVoteCommandMsg::getVoteFrame() const {
	return m_voteFrame;
}

/**
 * Set the vote frame.
 */
void NetDisconnectVoteCommandMsg::setVoteFrame(UnsignedInt voteFrame) {
	m_voteFrame = voteFrame;
}

NetCommandMsg::Select NetDisconnectVoteCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetProgressCommandMsg
//-------------------------
NetProgressCommandMsg::NetProgressCommandMsg()
{
	m_commandType = NETCOMMANDTYPE_PROGRESS;
	m_percent = 0;
}

NetProgressCommandMsg::~NetProgressCommandMsg() {}

UnsignedByte NetProgressCommandMsg::getPercentage() const
{
	return m_percent;
}

void NetProgressCommandMsg::setPercentage( UnsignedByte percent )
{
	m_percent = percent;
}

NetCommandMsg::Select NetProgressCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetWrapperCommandMsg
//-------------------------
NetWrapperCommandMsg::NetWrapperCommandMsg() {
	m_commandType = NETCOMMANDTYPE_WRAPPER;
	m_numChunks = 0;
	m_data = nullptr;
	m_totalDataLength = 0;
	m_chunkNumber = 0;
	m_dataLength = 0;
	m_dataOffset = 0;
	m_wrappedCommandID = 0;
	m_wrapSeq = 0;
	m_lowWater = 0;
}

NetWrapperCommandMsg::~NetWrapperCommandMsg() {
	delete[] m_data;
	m_data = nullptr;
}

const UnsignedByte * NetWrapperCommandMsg::getData() const {
	return m_data;
}

UnsignedByte * NetWrapperCommandMsg::getData() {
	return m_data;
}

void NetWrapperCommandMsg::setData(UnsignedByte *data, UnsignedInt dataLength)
{
	delete[] m_data;
	m_data = NEW UnsignedByte[dataLength];	// pool[]ify
	memcpy(m_data, data, dataLength);
	m_dataLength = dataLength;
}

UnsignedInt NetWrapperCommandMsg::getDataLength() const {
	return m_dataLength;
}

UnsignedInt NetWrapperCommandMsg::getDataOffset() const {
	return m_dataOffset;
}

void NetWrapperCommandMsg::setDataOffset(UnsignedInt offset) {
	m_dataOffset = offset;
}

UnsignedInt NetWrapperCommandMsg::getChunkNumber() const {
	return m_chunkNumber;
}

void NetWrapperCommandMsg::setChunkNumber(UnsignedInt chunkNumber) {
	m_chunkNumber = chunkNumber;
}

UnsignedInt NetWrapperCommandMsg::getNumChunks() const {
	return m_numChunks;
}

void NetWrapperCommandMsg::setNumChunks(UnsignedInt numChunks) {
	m_numChunks = numChunks;
}

UnsignedInt NetWrapperCommandMsg::getTotalDataLength() const {
	return m_totalDataLength;
}

void NetWrapperCommandMsg::setTotalDataLength(UnsignedInt totalDataLength) {
	m_totalDataLength = totalDataLength;
}

UnsignedShort NetWrapperCommandMsg::getWrappedCommandID() const {
	return m_wrappedCommandID;
}

void NetWrapperCommandMsg::setWrappedCommandID(UnsignedShort wrappedCommandID) {
	m_wrappedCommandID = wrappedCommandID;
}

NetCommandMsg::Select NetWrapperCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetWrapperAckCommandMsg
//-------------------------
NetWrapperAckCommandMsg::NetWrapperAckCommandMsg() {
	m_commandType = NETCOMMANDTYPE_WRAPPERACK;
	m_originalPlayerID = 0;
	m_wrapSeq = 0;
	m_chunkNumber = 0;
}

NetWrapperAckCommandMsg::~NetWrapperAckCommandMsg() {
}

// The ack's select (NetAckCommandMsg::getSmallNetPacketSelect): type and player only.
NetCommandMsg::Select NetWrapperAckCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 0;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 0;
	return select;
}

//-------------------------
// NetFileCommandMsg
//-------------------------
NetFileCommandMsg::NetFileCommandMsg() {
	m_commandType = NETCOMMANDTYPE_FILE;
	m_data = nullptr;
	m_portableFilename.clear();
	m_dataLength = 0;
}

NetFileCommandMsg::~NetFileCommandMsg() {
	delete[] m_data;
	m_data = nullptr;
}

AsciiString NetFileCommandMsg::getRealFilename() const
{
	return TheGameState->portableMapPathToRealMapPath(m_portableFilename);
}

void NetFileCommandMsg::setRealFilename(AsciiString filename)
{
	m_portableFilename = TheGameState->realMapPathToPortableMapPath(filename);
}

UnsignedInt NetFileCommandMsg::getFileLength() const {
	return m_dataLength;
}

const UnsignedByte * NetFileCommandMsg::getFileData() const {
	return m_data;
}

UnsignedByte * NetFileCommandMsg::getFileData() {
	return m_data;
}

void NetFileCommandMsg::setFileData(UnsignedByte *data, UnsignedInt dataLength)
{
	m_dataLength = dataLength;
	m_data = NEW UnsignedByte[dataLength];	// pool[]ify
	memcpy(m_data, data, dataLength);
}

NetCommandMsg::Select NetFileCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetFileAnnounceCommandMsg
//-------------------------
NetFileAnnounceCommandMsg::NetFileAnnounceCommandMsg() {
	m_commandType = NETCOMMANDTYPE_FILEANNOUNCE;
	m_portableFilename.clear();
	m_fileID = 0;
	m_playerMask = 0;
}

NetFileAnnounceCommandMsg::~NetFileAnnounceCommandMsg() {
}

AsciiString NetFileAnnounceCommandMsg::getRealFilename() const
{
	return TheGameState->portableMapPathToRealMapPath(m_portableFilename);
}

void NetFileAnnounceCommandMsg::setRealFilename(AsciiString filename)
{
	m_portableFilename = TheGameState->realMapPathToPortableMapPath(filename);
}

UnsignedShort NetFileAnnounceCommandMsg::getFileID() const {
	return m_fileID;
}

void NetFileAnnounceCommandMsg::setFileID(UnsignedShort fileID) {
	m_fileID = fileID;
}

RelayMaskType NetFileAnnounceCommandMsg::getPlayerMask() const {
	return m_playerMask;
}

void NetFileAnnounceCommandMsg::setPlayerMask(RelayMaskType playerMask) {
	m_playerMask = playerMask;
}

NetCommandMsg::Select NetFileAnnounceCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetFileProgressCommandMsg
//-------------------------
NetFileProgressCommandMsg::NetFileProgressCommandMsg() {
	m_commandType = NETCOMMANDTYPE_FILEPROGRESS;
	m_fileID = 0;
	m_progress = 0;
}

NetFileProgressCommandMsg::~NetFileProgressCommandMsg() {
}

UnsignedShort NetFileProgressCommandMsg::getFileID() const {
	return m_fileID;
}

void NetFileProgressCommandMsg::setFileID(UnsignedShort val) {
	m_fileID = val;
}

Int NetFileProgressCommandMsg::getProgress() const {
	return m_progress;
}

void NetFileProgressCommandMsg::setProgress(Int val) {
	m_progress = val;
}

NetCommandMsg::Select NetFileProgressCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetDisconnectFrameCommandMsg
//-------------------------
NetDisconnectFrameCommandMsg::NetDisconnectFrameCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTFRAME;
	m_disconnectFrame = 0;
}

NetDisconnectFrameCommandMsg::~NetDisconnectFrameCommandMsg() {
}

UnsignedInt NetDisconnectFrameCommandMsg::getDisconnectFrame() const {
	return m_disconnectFrame;
}

void NetDisconnectFrameCommandMsg::setDisconnectFrame(UnsignedInt disconnectFrame) {
	m_disconnectFrame = disconnectFrame;
}

NetCommandMsg::Select NetDisconnectFrameCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetDisconnectScreenOffCommandMsg
//-------------------------
NetDisconnectScreenOffCommandMsg::NetDisconnectScreenOffCommandMsg() {
	m_commandType = NETCOMMANDTYPE_DISCONNECTSCREENOFF;
	m_newFrame = 0;
}

NetDisconnectScreenOffCommandMsg::~NetDisconnectScreenOffCommandMsg() {
}

UnsignedInt NetDisconnectScreenOffCommandMsg::getNewFrame() const {
	return m_newFrame;
}

void NetDisconnectScreenOffCommandMsg::setNewFrame(UnsignedInt newFrame) {
	m_newFrame = newFrame;
}

NetCommandMsg::Select NetDisconnectScreenOffCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetFrameResendRequestCommandMsg
//-------------------------
NetFrameResendRequestCommandMsg::NetFrameResendRequestCommandMsg() {
	m_commandType = NETCOMMANDTYPE_FRAMERESENDREQUEST;
	m_frameToResend = 0;
}

NetFrameResendRequestCommandMsg::~NetFrameResendRequestCommandMsg() {
}

UnsignedInt NetFrameResendRequestCommandMsg::getFrameToResend() const {
	return m_frameToResend;
}

void NetFrameResendRequestCommandMsg::setFrameToResend(UnsignedInt frame) {
	m_frameToResend = frame;
}

NetCommandMsg::Select NetFrameResendRequestCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 1;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetLoadCompleteCommandMsg
//-------------------------
NetLoadCompleteCommandMsg::NetLoadCompleteCommandMsg() {
	m_commandType = NETCOMMANDTYPE_LOADCOMPLETE;
}

NetLoadCompleteCommandMsg::~NetLoadCompleteCommandMsg() {
}

NetCommandMsg::Select NetLoadCompleteCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}

//-------------------------
// NetTimeOutGameStartCommandMsg
//-------------------------
NetTimeOutGameStartCommandMsg::NetTimeOutGameStartCommandMsg() {
	m_commandType = NETCOMMANDTYPE_TIMEOUTSTART;
}

NetTimeOutGameStartCommandMsg::~NetTimeOutGameStartCommandMsg() {
}

NetCommandMsg::Select NetTimeOutGameStartCommandMsg::getSmallNetPacketSelect() const {
	Select select;
	select.useCommandType = 1;
	select.useRelay = 1;
	select.useFrame = 0;
	select.usePlayerId = 1;
	select.useCommandId = 1;
	return select;
}
