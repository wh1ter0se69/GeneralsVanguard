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

// FILE: .cpp /////////////////////////////////////////////////////////////////////////////////////
// Author: Colin Day
// Description: Game Client message dispatcher
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/MessageStream.h"
#include "GameClient/GameClient.h"

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
/** The Client message dispatcher, this is the last "translator" on the message
	* stream before the messages go to the network for processing.  It gives
	* the client itself the opportunity to respond to any messages on the stream
	* or create new ones to pass along to the network and logic */
// A selection of more than 254 units reached the simulation as (N + 1) % 256 - 1: GameMessage::getArgumentCount()
// is an UnsignedByte, read by GameLogic, NetGameCommandMsg / GameMessageParser and the replay writer. The list is
// re-sent here as messages of at most 255 arguments: the first keeps the type, the rest are silent and ADD to the
// group. Hotkey team parts start with a Boolean read as add to the squad. Wire format unchanged.
static const Int GX_MAX_MESSAGE_ARGUMENTS = 255;	// the largest getArgumentCount() can report

static Bool gxResendOversizedIdList(const GameMessage *msg)
{
	const GameMessage::Type type = msg->getType();
	const Bool selects = type == GameMessage::MSG_CREATE_SELECTED_GROUP || type == GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND;
	const Bool team = type >= GameMessage::MSG_CREATE_TEAM0 && type <= GameMessage::MSG_CREATE_TEAM9;
	if (!selects && !team && type != GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP)
		return FALSE;
	if (msg->getArgumentDataType(GX_MAX_MESSAGE_ARGUMENTS) == ARGUMENTDATATYPE_UNKNOWN)
		return FALSE;	// it fits

	Int total = GX_MAX_MESSAGE_ARGUMENTS;
	while (msg->getArgumentDataType(total) != ARGUMENTDATATYPE_UNKNOWN)
		++total;

	GameMessage *after = const_cast<GameMessage *>(msg);
	// GeneralsX: A REPLACEMENT SELECTION IS CLEARED FIRST, THEN EVERY PART ADDS. The logic replaces
	// the selection only at the first id of a createNewGroup message that still resolves (GameLogic::onCreateSelectedGroup),
	// so when every id of the first part had died before it executed, the OLD group stayed and the later parts added to
	// it: units the player had deselected followed the next order (p1-mass-order). A DESTROY
	// followed by a CREATE without createNewGroup is a pair MessageStream::isRedundantMessage keeps (it drops the DESTROY
	// only before a createNewGroup CREATE). The one difference from a createNewGroup first part: a first resolved object
	// that is not mass-selectable is no longer let in - a client multi-select never holds one (InGameUI and the selection
	// translator filter on isMassSelectable), and retail already drops every such object after the first.
	const Int firstStart = selects ? 1 : 0;
	if (selects && msg->getArgument(0)->boolean)
		after = TheMessageStream->insertMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP, after);
	for (Int next = firstStart; next < total; )
	{
		const Bool first = next == firstStart;
		GameMessage *part = TheMessageStream->insertMessage(
			(selects && !first) ? GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND : type, after);
		if (selects)
			part->appendBooleanArgument(FALSE);	// add: the group was cleared above, or the player asked to add
		else if (team && !first)
			part->appendBooleanArgument(TRUE);	// "add to the squad": Player::processCreateTeamGameMessage
		for (; next < total && part->getArgumentCount() < GX_MAX_MESSAGE_ARGUMENTS; ++next)
			part->appendObjectIDArgument(msg->getArgument(next)->objectID);
		after = part;
	}
	return TRUE;
}

GameMessageDisposition GameClientMessageDispatcher::translateGameMessage(const GameMessage *msg)
{
	if (gxResendOversizedIdList(msg))
		return DESTROY_MESSAGE;	// its parts follow it on the stream; this translator keeps them below
	if (msg->getType() >= GameMessage::MSG_BEGIN_NETWORK_MESSAGES && msg->getType() <= GameMessage::MSG_END_NETWORK_MESSAGES)
		return KEEP_MESSAGE;
	if (msg->getType() == GameMessage::MSG_NEW_GAME || msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA)
		return KEEP_MESSAGE;

	if (msg->getType() == GameMessage::MSG_FRAME_TICK)
		return KEEP_MESSAGE;

	//DEBUG_LOG(("GameClientMessageDispatcher::translateGameMessage() - eating a %s on frame %d",
		//((GameMessage *)msg)->getCommandAsString(), TheGameClient->getFrame()));

	return DESTROY_MESSAGE;
}
