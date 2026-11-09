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

#pragma once

#include "GameNetwork/NetCommandRef.h"
#include "Lib/BaseType.h"
//#include "GameNetwork/ConnectionManager.h"

enum DisconnectStateType CPP_11(: Int) {
	DISCONNECTSTATETYPE_SCREENON,
	DISCONNECTSTATETYPE_SCREENOFF
//	DISCONNECTSTATETYPE_WAITINGFORPACKETROUTER
};

class ConnectionManager;

struct DisconnectVoteStruct {
	Bool vote;
	UnsignedInt frame;
};

class DisconnectManager
{
public:
	DisconnectManager();
	virtual ~DisconnectManager();

	void init();
	void update(ConnectionManager *conMgr);

	void processDisconnectCommand(NetCommandRef *ref, ConnectionManager *conMgr);
	void allCommandsReady(UnsignedInt frame, ConnectionManager *conMgr, Bool waitForPacketRouter = TRUE);
	void nextFrame(UnsignedInt frame, ConnectionManager *conMgr);
	Bool allowedToContinue();			///< Allow the next frame to go through?
	void playerHasAdvancedAFrame(Int slot, UnsignedInt frame); ///< this player has advanced to that frame.

	void voteForPlayerDisconnect(Int slot, ConnectionManager *conMgr);

	static void setSilenceTimeout(time_t ms);	///< GeneralsX: the GX_TEST_FORGE silence:<ms> instrument; see silenceLeft

	// For disconnect blame assignment
	UnsignedInt getPingFrame();
	Int getPingsSent();
	Int getPingsReceived();

protected:
	void sendKeepAlive(ConnectionManager *conMgr);
	void populateDisconnectScreen(ConnectionManager *conMgr);
	Int translatedSlotPosition(Int slot, Int localSlot);
	Int untranslatedSlotPosition(Int slot, Int localSlot);
	void resetPlayerTimeouts(ConnectionManager *conMgr);
	void resetPlayerTimeout(Int slot);
	void resetPacketRouterTimeout();
	void updateDisconnectStatus(ConnectionManager *conMgr);
	void disconnectPlayer(Int slot, ConnectionManager *conMgr);
	void sendDisconnectCommand(Int slot, ConnectionManager *conMgr);
	void sendVoteCommand(Int slot, ConnectionManager *conMgr);
	void updateWaitForPacketRouter(ConnectionManager *conMgr);
	void recalculatePacketRouterIndex(ConnectionManager *conMgr);
	Bool allOnSameFrame(ConnectionManager *conMgr); ///< returns true if all players are stuck on the same frame.
	Bool isLocalPlayerNextPacketRouter(ConnectionManager *conMgr); ///< returns true if the local player is next in line to be the packet router with all the players that have timed out being taken out of the picture.
	UnsignedInt nextPacketRouter(ConnectionManager *conMgr); ///< who that is - the only peer whose DISCONNECTPLAYER this peer honours. MAX_SLOTS when nobody is.
	Bool hasPlayerTimedOut(Int slot, ConnectionManager *conMgr); ///< returns true if this player has timed out (or is silent, see silenceLeft).
	time_t silenceLeft(Int slot, ConnectionManager *conMgr); ///< GeneralsX: ms until this (translated) slot counts as vanished; the stock timeout when no verdict applies
	void sendPlayerDestruct(Int slot, ConnectionManager *conMgr); ///< send a destruct player network message.
	Bool isPlayerVotedOut(Int slot, ConnectionManager *conMgr);	///< returns true if this player has been voted out.
	Bool isPlayerInGame(Int slot, ConnectionManager *conMgr); ///< returns true if the player has neither timed out or been voted out.
	UnsignedInt getMaxDisconnectFrame();	///< returns the highest frame that people have got to.
	Int countVotesForPlayer(Int slot); ///< return the number of disconnect votes a player has.
	void resetPlayersVotes(Int playerID, UnsignedInt frame, ConnectionManager *conMgr); ///< reset the votes for this player.

	void turnOnScreen(ConnectionManager *conMgr); ///< This gets called when the disconnect screen is first turned on.

	void processDisconnectKeepAlive(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processDisconnectPlayer(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processPacketRouterQuery(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processPacketRouterAck(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processDisconnectVote(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processDisconnectFrame(NetCommandMsg *msg, ConnectionManager *conMgr);
	void processDisconnectScreenOff(NetCommandMsg *msg, ConnectionManager *conMgr);

	void applyDisconnectVote(Int slot, UnsignedInt frame, Int castingSlot, ConnectionManager *conMgr);

	UnsignedInt m_lastFrame;
	time_t m_lastFrameTime;
	DisconnectStateType m_disconnectState;

	UnsignedInt m_packetRouterFallback[MAX_SLOTS];
	UnsignedInt m_currentPacketRouterIndex;

	time_t m_lastKeepAliveSendTime;

	time_t m_playerTimeouts[MAX_SLOTS - 1];
	time_t m_keepAliveTimes[MAX_SLOTS - 1];	///< GeneralsX: per translated slot, when its last DISCONNECTKEEPALIVE arrived, 0 never
	time_t m_liveFrameTime;	///< GeneralsX: when the logic frame last moved in plain play, the silence clock's floor (see nextFrame)
	time_t m_screenOffTime;	///< GeneralsX: when a release that followed this peer's frame notice last turned the screen off, 0 never
	time_t m_slowFrameTime;	///< GeneralsX: when this peer last completed a frame that took ANSWER_STILL_MS or more, 0 never (see processDisconnectKeepAlive)
	time_t m_catchUpUntil;	///< GeneralsX: a re-stall before then is the same stall, after a release this peer's frame notice bought (see update), 0 none
	time_t m_packetRouterTimeout;

	DisconnectVoteStruct m_playerVotes[MAX_SLOTS][MAX_SLOTS];
//	Bool m_myVotes[MAX_SLOTS - 1];

	UnsignedInt m_disconnectFrames[MAX_SLOTS];
	Bool m_disconnectFramesReceived[MAX_SLOTS];
	RelayMaskType m_heldDisconnectMask[MAX_SLOTS];	///< per victim: bit per sender of a DISCONNECTPLAYER held until that sender is this peer's next router, 0 none
	Bool m_haveNotifiedOtherPlayersOfCurrentFrame;

	time_t m_timeOfDisconnectScreenOn;
	Int m_pingsSent;
	Int m_pingsReceived;
	UnsignedInt m_pingFrame;
};
