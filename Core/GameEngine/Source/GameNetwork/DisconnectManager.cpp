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

#include "Common/Recorder.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/DisconnectManager.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/networkutil.h"
#include "GameNetwork/GameSpy/PingThread.h"
#include "GameNetwork/GameSpy/GSConfig.h"

// GeneralsX: A VANISHED PEER IS DROPPED AFTER s_silenceMs OF SILENCE, NOT THE ~65 s STOCK HOLD.
//
// A peer that dies without a PLAYERLEAVE (crash, pulled cable, killed process) used to freeze every survivor on the
// disconnect screen for NetworkDisconnectTime (5 s) plus the whole NetworkPlayerTimeoutTime (60 s), because
// turnOnScreen restarts every timeout, and ~20 s more when the survivors stood a frame apart (the catch-up advance
// restarts them again in nextFrame). One crash in a twelve-seat match froze eleven people for over a minute.
//
// SILENT is a different fact from TIMED OUT, and only it gets the short clock: no keep-alive and no frame from the
// player for s_silenceMs, while this peer's disconnect screen is on and this peer still hears SOMEBODY else. Every
// peer whose own frame has stood still for NetworkDisconnectTime sends DISCONNECTKEEPALIVE every 500 ms, direct, and
// ConnectionManager::processNetCommand refuses one that did not come from the peer it names - so a stalled but alive
// peer is heard, and nobody can be heard on its behalf. A slow peer answers the keep-alives it receives
// (processDisconnectKeepAlive); in plain play every frame refreshes the clock (nextFrame).
// The "hears somebody else" half keeps a peer whose OWN network is down from finding everyone silent: it hears
// nobody, so it keeps the stock timeout and cannot drop the match around itself.
//
// The verdict is still the router's, through the unchanged protocol: silence only feeds hasPlayerTimedOut and
// updateDisconnectStatus, so the next router issues DISCONNECTPLAYER + DESTROYPLAYER at a frame, every receiver's walk
// (isPlayerInGame) skips the silent player and applies a held order from that router, and allOnSameFrame waits for
// every survivor's frame notice - which updateDisconnectStatus now sends on silence, as it does at two thirds of the
// stock timeout. No peer defeats anyone from its own clock.
//
// 30 s, the owner's call (01/10/2026, over the 10 s proposed): it gives a player whose connection drops 30 s to come
// back, and it is above everything a live peer does - iOS keeps the network running for its ~30 s background grant
// (IOSBackgroundTask.mm), Windows simulates while minimised, measured hitches are under 0.5 s, and net-kill-gates.sh
// honest-acks freezes a live peer for 8 s. Not an INI field: changing the shipped INI moves dataID and would also
// shorten an alive peer's grace.
//
// NOT IN A 1v1. With one other player left there is nobody else to hear, so the "hears somebody else" half can never
// hold and the last two players keep the stock timeout: a peer that hears nothing cannot tell its opponent's crash from
// its own outage, and dropping on that would end a 1v1 on any 30 s blip on either side. Deliberate, and the owner's
// call too; asking the relay "can you hear me?" during the stall is the follow-up that would cover it.
static time_t s_silenceMs = 30000;
static const time_t SILENCE_HEARD_MS = 2000;	///< a keep-alive this recent proves this peer can still hear its sender
static const time_t CATCHUP_MS = 2000;	///< after a release a frame notice bought, a re-stall this soon is the same stall (see update)
static const time_t ANSWER_STILL_MS = 100;	///< a peer answers keep-alives only once its own frame has stood still this long (see processDisconnectKeepAlive)
static const time_t ANSWER_SLOW_MS = 1000;	///< or within this long of a frame that took ANSWER_STILL_MS or more, whatever the phase (same place)
static const time_t CATCHUP_RESTALL_MS = 250;	///< a frame standing still this long inside that window is the same stall again

void DisconnectManager::setSilenceTimeout(time_t ms) {
	s_silenceMs = ms;
}

DisconnectManager::DisconnectManager()
{
	Int i;
	m_currentPacketRouterIndex = 0;
	m_lastFrame = 0;
	m_lastFrameTime = 0;
	m_lastKeepAliveSendTime = 0;
	m_haveNotifiedOtherPlayersOfCurrentFrame = FALSE;
	m_timeOfDisconnectScreenOn = 0;

	for( i = 0; i < MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = 0;
		m_heldDisconnectMask[i] = 0;
	}

	m_packetRouterTimeout = 0;
	for( i = 0; i < MAX_SLOTS -1; ++i) {
		m_playerTimeouts[i] = 0;
		m_keepAliveTimes[i] = 0;
	}
	m_liveFrameTime = 0;
	m_screenOffTime = 0;
	m_catchUpUntil = 0;
	m_slowFrameTime = 0;

	for( i = 0; i < MAX_SLOTS; ++i) {
		for (Int j = 0; j < MAX_SLOTS; ++j) {
			m_playerVotes[i][j].vote = FALSE;
			m_playerVotes[i][j].frame = 0;
		}
	}
}

DisconnectManager::~DisconnectManager() {
}

void DisconnectManager::init() {
	TheDisconnectMenu->hideScreen(); // make sure the screen starts out hidden.
	m_lastFrame = 0;
	m_lastFrameTime = -1;
	m_lastKeepAliveSendTime = -1;
	m_disconnectState = DISCONNECTSTATETYPE_SCREENOFF;
	m_currentPacketRouterIndex = 0;
	m_timeOfDisconnectScreenOn = 0;

	Int i = 0;
	for (; i < MAX_SLOTS; ++i) {
		for (Int j = 0; j < MAX_SLOTS; ++j) {
			m_playerVotes[i][j].vote = FALSE;
			m_playerVotes[i][j].frame = 0;
		}
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		m_disconnectFrames[i] = 0;
		m_disconnectFramesReceived[i] = FALSE;
		m_heldDisconnectMask[i] = 0;
	}

	for (i = 0; i < MAX_SLOTS - 1; ++i) {
		m_keepAliveTimes[i] = 0;
	}
	m_liveFrameTime = 0;
	m_screenOffTime = 0;
	m_catchUpUntil = 0;
	m_slowFrameTime = 0;

	m_pingFrame = 0;
	m_pingsSent = 0;
	m_pingsReceived = 0;
}

void DisconnectManager::update(ConnectionManager *conMgr) {
	if (m_lastFrameTime == -1) {
		m_lastFrameTime = timeGetTime();
		m_liveFrameTime = m_lastFrameTime;
	}

	// The game logic stalls on the frame we are currently waiting for commands on,
	// so we have to check for the current logic frame being one higher than
	// the last one we had the commands ready for.
	if (TheGameLogic->getFrame() == m_lastFrame) {
		time_t curTime = timeGetTime();
		// GeneralsX: A frame that stops again right after a catch-up (allCommandsReady) is the
		// SAME stall: the screen comes back at once instead of after another NetworkDisconnectTime, so the frame
		// notice that follows - and the next catch-up, when the survivors leapfrog - costs no extra 5 s each time.
		const Bool reStalled = (m_catchUpUntil != 0) && ((curTime - m_catchUpUntil) < 0)
			&& ((curTime - m_lastFrameTime) > CATCHUP_RESTALL_MS);
		if (((curTime - m_lastFrameTime) > TheGlobalData->m_networkDisconnectTime) || reStalled) {
			if (m_disconnectState == DISCONNECTSTATETYPE_SCREENOFF) {
				turnOnScreen(conMgr);
			}
			sendKeepAlive(conMgr);
		}
	} else {
		nextFrame(TheGameLogic->getFrame(), conMgr);
	}

	if (m_disconnectState != DISCONNECTSTATETYPE_SCREENOFF) {
		// GeneralsX: A DISCONNECTPLAYER held by processDisconnectPlayer
		// applies once its sender is this peer's next router - here, when this peer's own timeout or
		// the votes it has seen take out whoever stood ahead of it.
		for (Int v = 0; v < MAX_SLOTS; ++v) {
			const UnsignedInt elected = nextPacketRouter(conMgr);
			if ((elected < (UnsignedInt)MAX_SLOTS) && ((m_heldDisconnectMask[v] & gxSlotBit(elected)) != 0)) {
				fprintf(stderr, "[GXGATE] DISCONNECTPLAYER from slot %u naming slot %d applied: it is now this peer's next router (frame %u)\n",
					(unsigned)elected, v, (unsigned)TheGameLogic->getFrame());
				fflush(stderr);
				m_heldDisconnectMask[v] = 0;
				if (conMgr->isPlayerConnected(v)) {
					disconnectPlayer(v, conMgr);
				}
			}
		}
		updateDisconnectStatus(conMgr);

		// check to see if we need to send pings
		if (m_pingFrame < TheGameLogic->getFrame())
		{
			time_t curTime = timeGetTime();
			if ((curTime - m_lastFrameTime) > 10000) /// @todo: plug in some better measure here
			{
				m_pingFrame = TheGameLogic->getFrame();
				m_pingsSent = 0;
				m_pingsReceived = 0;

				// Send the pings
				if (ThePinger)
				{
					//use next ping server
					static size_t serverIndex = 0;
					serverIndex++;

					AsciiStringList pingServers = TheGameSpyConfig->getPingServers();

					if( serverIndex >= pingServers.size() )
						serverIndex = 0;  //wrap back to first ping server

					std::list<AsciiString>::iterator it = pingServers.begin();
					for( size_t i = 0;  i < serverIndex;  i++ )
						it++;

					PingRequest req;
					req.hostname = it->str();
					req.repetitions = 5;
					req.timeout = 2000;
					m_pingsSent = req.repetitions;
					ThePinger->addRequest(req);
					DEBUG_LOG(("DisconnectManager::update() - requesting %d pings of %d from %s",
						req.repetitions, req.timeout, req.hostname.c_str()));
				}
			}
		}

		// update the ping thread, tracking pings if we are on the same frame
		if (ThePinger)
		{
			PingResponse resp;
			while (ThePinger->getResponse(resp))
			{
				if (m_pingFrame != TheGameLogic->getFrame())
				{
					// wrong frame - we're not pinging yet
					DEBUG_LOG(("DisconnectManager::update() - discarding ping of %d from %s (%d reps)",
						resp.avgPing, resp.hostname.c_str(), resp.repetitions));
				}
				else
				{
					// right frame
					DEBUG_LOG(("DisconnectManager::update() - keeping ping of %d from %s (%d reps)",
						resp.avgPing, resp.hostname.c_str(), resp.repetitions));
					if (resp.avgPing < 2000)
					{
						m_pingsReceived += resp.repetitions;
					}
				}
			}
		}
	}
}

UnsignedInt DisconnectManager::getPingFrame()
{
	return m_pingFrame;
}

Int DisconnectManager::getPingsSent()
{
	return m_pingsSent;
}

Int DisconnectManager::getPingsReceived()
{
	return m_pingsReceived;
}


void DisconnectManager::updateDisconnectStatus(ConnectionManager *conMgr) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (conMgr->isPlayerConnected(i)) {
			Int slot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
			if (slot != -1) {
				time_t curTime = timeGetTime();
				time_t newTime = TheGlobalData->m_networkPlayerTimeoutTime - (curTime - m_playerTimeouts[slot]);
				const time_t silence = silenceLeft(slot, conMgr);	// GeneralsX: see s_silenceMs

// if someone is more than 2/3 timed out, lets get our frame numbers sync'd up.  Also if someone is voted out
// lets do the same thing.

				if (m_haveNotifiedOtherPlayersOfCurrentFrame == FALSE) {
					if ((newTime < TheGlobalData->m_networkPlayerTimeoutTime / 3) || (isPlayerVotedOut(slot, conMgr) == TRUE) || (silence < 0)) {
						TheNetwork->notifyOthersOfCurrentFrame();
						m_haveNotifiedOtherPlayersOfCurrentFrame = TRUE;
					}

					DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - curTime = %d, m_timeOfDisconnectScreenOn = %d, curTime - m_timeOfDisconnectScreenOn = %d", curTime, m_timeOfDisconnectScreenOn, curTime - m_timeOfDisconnectScreenOn));

					if (m_timeOfDisconnectScreenOn != 0) {
						if ((curTime - m_timeOfDisconnectScreenOn) > TheGlobalData->m_networkDisconnectScreenNotifyTime) {
							TheNetwork->notifyOthersOfCurrentFrame();
							m_haveNotifiedOtherPlayersOfCurrentFrame = TRUE;
						}
					}
				}

				// GeneralsX: a silent player is dropped, and counted down on the screen, on the shorter clock.
				if (silence < newTime) {
					newTime = silence;
				}
				if ((newTime < 0) || (isPlayerVotedOut(slot, conMgr) == TRUE)) {
					newTime = 0;
					DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - player %d(translated slot %d) has been voted out or timed out", i, slot));
					if (allOnSameFrame(conMgr) == TRUE) {
						DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - all on same frame"));
						if (isLocalPlayerNextPacketRouter(conMgr) == TRUE) {
							DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - local player is next packet router"));
							DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - about to do the disconnect procedure for player %d", i));
							// GeneralsX: Queue the kill BEFORE tearing the mesh down.
							//
							// "kick = defeat": a voted-out player must lose their base, not stand
							// there AWOL earning money. The kill order already exists and is correct -
							// sendPlayerDestruct emits NETCOMMANDTYPE_DESTROYPLAYER, which IS
							// frame-synchronised (NetworkUtil.cpp IsCommandSynchronized), is stamped
							// getExecutionFrame()+1, and is released by RelayCommandsToCommandList on
							// that one frame on every peer, reaching Player::killPlayer() via
							// MSG_SELF_DESTRUCT. VictoryConditions::update then derives the defeat -
							// banner, reveal, score screen - for free, because defeat in this engine is
							// polled from object ownership and never set directly.
							//
							// The bug was the ORDER. disconnectPlayer() deletes m_frameData[slot] and
							// m_connections[slot] and, if the victim held the role, migrates
							// m_packetRouterSlot (ConnectionManager.cpp:1891-1915). Emitting the
							// synchronised kill AFTER that asked the frame machinery to schedule and
							// ack a command for a future frame using state that had just been
							// dismantled underneath it, while the AWOL marking beside it rides
							// DISCONNECTPLAYER, which is direct-send and unsynchronised and therefore
							// always lands. That asymmetry - marking always arrives, kill sometimes
							// does not - is exactly the observed "AWOL, base standing, still earning"
							// ghost. Queue the kill first; the teardown is unconditional either way.
							//
							// The kill lands on every SURVIVOR. The victim's own copies of both
							// orders are only queued on its Connection, which disconnectPlayer
							// deletes in this same update, so they never leave this machine
							// (19/09/2026, net-kill-gates.sh legit-kick-alive).
							sendDisconnectCommand(i, conMgr);
							sendPlayerDestruct(i, conMgr);
							disconnectPlayer(i, conMgr);
						} else {
							DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - local player is not the next packet router"));
						}
					} else {
						DEBUG_LOG(("DisconnectManager::updateDisconnectStatus - not all on same frame"));
					}
				}
				TheDisconnectMenu->setPlayerTimeoutTime(slot, newTime);
			}
		}
	}
}

void DisconnectManager::updateWaitForPacketRouter(ConnectionManager *conMgr) {
/*
	time_t curTime = timeGetTime();
	time_t newTime = TheGlobalData->m_networkPlayerTimeoutTime - (curTime - m_packetRouterTimeout);
	if (newTime < 0) {
		newTime = 0;

		// The guy that we were hoping would be the new packet router isn't.  We're screwed, get out of the game.

		DEBUG_LOG(("DisconnectManager::updateWaitForPacketRouter - timed out waiting for new packet router, quitting game"));
		TheNetwork->quitGame();
	}
	TheDisconnectMenu->setPacketRouterTimeoutTime(newTime);
*/
}

void DisconnectManager::processDisconnectCommand(NetCommandRef *ref, ConnectionManager *conMgr) {
	NetCommandMsg *msg = ref->getCommand();
	
	switch (msg->getNetCommandType()) {
		case NETCOMMANDTYPE_DISCONNECTKEEPALIVE:
			processDisconnectKeepAlive(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_DISCONNECTPLAYER:
			processDisconnectPlayer(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_PACKETROUTERQUERY:
			processPacketRouterQuery(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_PACKETROUTERACK:
			processPacketRouterAck(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_DISCONNECTVOTE:
			processDisconnectVote(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_DISCONNECTFRAME:
			processDisconnectFrame(msg, conMgr);
			break;
			
		case NETCOMMANDTYPE_DISCONNECTSCREENOFF:
			processDisconnectScreenOff(msg, conMgr);
			break;
			
		default:
			break;
	}
}

void DisconnectManager::processDisconnectKeepAlive(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetDisconnectKeepAliveCommandMsg *cmdMsg = (NetDisconnectKeepAliveCommandMsg *)msg;
	Int slot = translatedSlotPosition(cmdMsg->getPlayerID(), conMgr->getLocalPlayerID());
	if (slot != -1) {
		resetPlayerTimeout(slot);
		m_keepAliveTimes[slot] = timeGetTime();
		// GeneralsX: ANSWER IT while this peer's own frame still moves. A keep-alive means
		// somebody has stood still for NetworkDisconnectTime; if that is waiting on THIS peer - slow, not gone - its
		// frames are the only other sign of life, and a frame that lands inside a catch-up window does not count
		// (nextFrame). So a peer that is only slow says so: once per 500 ms (sendKeepAlive), only while asked, and a
		// stalled peer already sends its own. A peer whose loop has stopped cannot answer - which is the point.
		// Only while this peer's own frame has stood still for ANSWER_STILL_MS: a peer waiting on
		// nobody (its frames moving) has no reason to, and two of them answering each other's answers would keep a
		// broadcast going after the stall was over. A peer that others wait on cannot keep its own
		// frame moving for long: lockstep lets it run only as far as the input the stalled peers sent
		// before they stopped - at most MAX_FRAMES_AHEAD frames, all that FRAME_DATA_LENGTH can hold - and after that
		// its frame stands still too and it answers. That is ~4 s at full speed but not for a SLOW peer: at 1.5 fps,
		// 60 buffered frames take 40 s. Hence ANSWER_STILL_MS = 100: a peer whose frame moves faster
		// than one per 100 ms runs out of those frames within MAX_FRAMES_AHEAD x 100 ms (25.6 s at 60 Hz, 12.8 s at
		// 30 Hz), under s_silenceMs, and one any slower has a frame standing still that long between two of its frames
		// and answers there. A match running at full speed still never answers - its frames move every 17-33 ms.
		//
		// Whatever the PHASE: a peer at a few frames per second whose frame happened to advance just
		// before every keep-alive would see each one inside the 100 ms and never answer. So a frame that itself took
		// ANSWER_STILL_MS or more (nextFrame) keeps this peer answering for ANSWER_SLOW_MS: keep-alives come every
		// 500 ms, so one lands in that window, and a peer that still never answers has run every frame in under
		// 100 ms - the bound above. Full-speed play still never answers, so the r7 loop stays closed.
		const time_t now = timeGetTime();
		const Bool slow = ((now - m_lastFrameTime) >= ANSWER_STILL_MS) || ((m_slowFrameTime != 0) && ((now - m_slowFrameTime) < ANSWER_SLOW_MS));
		if ((m_disconnectState == DISCONNECTSTATETYPE_SCREENOFF) && slow) {
			sendKeepAlive(conMgr);
		}
	}
}

void DisconnectManager::processDisconnectPlayer(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetDisconnectPlayerCommandMsg *cmdMsg = (NetDisconnectPlayerCommandMsg *)msg;
	DEBUG_LOG(("DisconnectManager::processDisconnectPlayer - Got disconnect player command from player %d.  Disconnecting player %d on frame %d", msg->getPlayerID(), cmdMsg->getDisconnectSlot(), cmdMsg->getDisconnectFrame()));
	DEBUG_ASSERTCRASH(TheGameLogic->getFrame() == cmdMsg->getDisconnectFrame(), ("disconnecting player on the wrong frame!!!"));

	// GeneralsX: Only this peer's next router may disconnect somebody else.
	//
	// This order drops its victim from this peer's mesh - frame data, connection, router plan - and
	// nothing checked who sent it: measured, slot 1 sending it about slot 2 took slot 2 out of the
	// router's mesh (scripts/test/net-kill-gates.sh forge-eject), and by ejecting the router first
	// any peer could make itself the one DESTROYPLAYER is accepted from. The only legitimate
	// senders are updateDisconnectStatus on the peer isLocalPlayerNextPacketRouter elects, and
	// ConnectionManager::quitGame, where a peer names itself. So honour exactly those: the sender is
	// the victim, or the sender is who THIS peer elects by the same walk. That the sender is who the
	// packet says is ConnectionManager::processNetCommand's job (arrivedFrom), not this one's.
	//
	// HELD, NOT DROPPED. The walk skips players this peer has
	// timed out or seen voted out, and the issuer decided on ITS clock: when the router itself has
	// dropped, its successor can act a moment before this peer's own timeout of the router expires.
	// Dropping that order would strand this peer - nobody re-sends it, and it cannot pass the stalled
	// frame without the router's input or its disconnection - so it waits in m_heldDisconnectMask
	// until update() sees the sender become this peer's next router. A frame completing does NOT
	// forget it - see allCommandsReady for the measured strand that used to cause.
	//
	// GeneralsX: Held WHATEVER the screen state; it used to be held only while the screen
	// was on and refused otherwise. A peer that has just completed a frame - its screen off, playing the frames it
	// already holds - can receive the successor's one order after the silence verdict has lapsed with the screen
	// (silenceLeft), elect the vanished router again, and refuse; when those frames run out it stalls waiting for an
	// order nobody sends twice. Holding grants nobody anything: a held order applies only once its
	// sender is this peer's elected router (update(), screen on), who could send it again then anyway, and a sender
	// that withholds its own input could already put a stall around any order it wanted held.
	//
	// Residual, stated: the elected router can still disconnect anybody - the retail design makes it
	// the judge of who has stopped responding, and nothing a receiver can see tells a real timeout
	// from a claimed one. Local corroboration of the VICTIM was considered and is wrong: a victim cut
	// off from the router but not from this peer never times out here, and refusing would leave this
	// peer waiting for input the router no longer relays.
	const UnsignedInt sender = msg->getPlayerID();
	const UnsignedInt victim = cmdMsg->getDisconnectSlot();
	if (victim >= MAX_SLOTS) {
		return;
	}
	const UnsignedInt elected = nextPacketRouter(conMgr);
	if ((sender != victim) && (sender != elected)) {
		// GeneralsX: A SET of senders, not one. This used to be one Int per
		// victim, overwritten unconditionally, so any peer could erase the genuine successor's held
		// order just by sending its own for the same victim, during the whole stalled window the hold
		// exists for. The receiver would then never apply the real order (nobody
		// re-sends it) and would sit on the stalled frame until its own timeouts made it the elected
		// router - the stranding the hold was added to prevent. A bit per sender costs the same word.
		const Bool hold = (sender < (UnsignedInt)MAX_SLOTS);
		if (hold) {
			m_heldDisconnectMask[victim] |= (gxSlotBit(sender));
		}
		static Int s_refusals = 0;	// Trade-off: capped so a flood of forgeries cannot turn into a flood of fflush
		if (s_refusals < 32) {
			++s_refusals;
			fprintf(stderr, "[GXGATE] DISCONNECTPLAYER from slot %u naming slot %u %s: this peer's next router is %u (frame %u)\n",
				(unsigned)sender, (unsigned)victim, hold ? "held" : "refused", (unsigned)elected, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
		return;
	}
	{
		static Int s_accepted = 0;	// capped all the same
		if (s_accepted < 32) {
			++s_accepted;
			fprintf(stderr, "[GXGATE] DISCONNECTPLAYER from slot %u naming slot %u accepted (frame %u)\n",
				(unsigned)sender, (unsigned)victim, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
	}

	// GeneralsX: A hard quit - the disconnect screen's Quit,
	// Network::quitGame - sends ONLY this direct order: the quitter drops every connection before its
	// own MSG_SELF_DESTRUCT could leave it. So nothing synchronised ever took it out of the match. It
	// stood on every survivor as a ghost, and it stayed in the succession DESTROYPLAYER is judged by
	// (ConnectionManager::lockstepRouter): a router that hard-quit made every later kill refused
	// The peer that routes without the quitter turns the quit into the kill the
	// quitter asked for, exactly as updateDisconnectStatus does for a drop - the synchronised order
	// first, the teardown after - and every peer then executes it on one frame. What it asked for is
	// Network::quitGame's MSG_SELF_DESTRUCT(TRUE): assets to a living ally, which a drop's
	// DESTROYPLAYER does not do under RETAIL_COMPATIBLE_CRC - hence the flag. Only where
	// quitGame asks for it: base Generals under RETAIL_COMPATIBLE_CRC compiles that message out, so its
	// hard quit keeps the plain destroy it always had.
#if !RTS_GENERALS || !RETAIL_COMPATIBLE_CRC
	const UnsignedInt quitterAsked = DESTROYPLAYER_TRANSFER_ASSETS;
#else
	const UnsignedInt quitterAsked = 0;
#endif
	if ((sender == victim) && conMgr->isPlayerConnected((Int)victim)) {
		const UnsignedInt router = conMgr->getPacketRouterSlot();
		const UnsignedInt issuer = (router == victim) ? conMgr->getNextPacketRouterSlot(victim) : router;
		if (issuer == conMgr->getLocalPlayerID()) {
			sendPlayerDestruct((Int)(victim | quitterAsked), conMgr);
		}
	}
	disconnectPlayer(victim, conMgr);
}

void DisconnectManager::processPacketRouterQuery(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetPacketRouterQueryCommandMsg *cmdMsg = (NetPacketRouterQueryCommandMsg *)msg;
	DEBUG_LOG(("DisconnectManager::processPacketRouterQuery - got a packet router query command from player %d", msg->getPlayerID()));

	if (conMgr->getPacketRouterSlot() == conMgr->getLocalPlayerID()) {
		NetPacketRouterAckCommandMsg *ackmsg = newInstance(NetPacketRouterAckCommandMsg);
		ackmsg->setPlayerID(conMgr->getLocalPlayerID());
		if (DoesCommandRequireACommandID(ackmsg->getNetCommandType()) == TRUE) {
			ackmsg->setID(GenerateNextCommandID());
		}
		DEBUG_LOG(("DisconnectManager::processPacketRouterQuery - We are the new packet router, responding with an packet router ack. Local player is %d", ackmsg->getPlayerID()));
		conMgr->sendLocalCommandDirect(ackmsg, gxSlotBit(cmdMsg->getPlayerID()));
		ackmsg->detach();
	} else {
		DEBUG_LOG(("DisconnectManager::processPacketRouterQuery - We are NOT the new packet router, these are not the droids you're looking for."));
	}
}

void DisconnectManager::processPacketRouterAck(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetPacketRouterAckCommandMsg *cmdMsg = (NetPacketRouterAckCommandMsg *)msg;
	DEBUG_LOG(("DisconnectManager::processPacketRouterAck - got packet router ack command from player %d", msg->getPlayerID()));

	if (conMgr->getPacketRouterSlot() == cmdMsg->getPlayerID()) {
		DEBUG_LOG(("DisconnectManager::processPacketRouterAck - packet router command is from who it should be."));
		resetPacketRouterTimeout();
		Int currentPacketRouterSlot = conMgr->getPacketRouterSlot();
		Int currentPacketRouterIndex = 0;
		while ((currentPacketRouterSlot != conMgr->getPacketRouterFallbackSlot(currentPacketRouterIndex)) && (currentPacketRouterIndex < MAX_SLOTS)) {
			++currentPacketRouterIndex;
		}
		DEBUG_ASSERTCRASH((currentPacketRouterIndex < MAX_SLOTS), ("Invalid packet router index"));

		DEBUG_LOG(("DisconnectManager::processPacketRouterAck - New packet router confirmed, resending pending commands"));
		conMgr->resendPendingCommands();
		m_currentPacketRouterIndex = currentPacketRouterIndex;
		DEBUG_LOG(("DisconnectManager::processPacketRouterAck - Setting disconnect state to screen on."));
		m_disconnectState = DISCONNECTSTATETYPE_SCREENON; ///< set it to screen on so that the next call to AllCommandsReady can set up everything for the next frame properly.
	}
}

void DisconnectManager::processDisconnectVote(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetDisconnectVoteCommandMsg *cmdMsg = (NetDisconnectVoteCommandMsg *)msg;
	DEBUG_LOG(("DisconnectManager::processDisconnectVote - Got a disconnect vote for player %d command from player %d", cmdMsg->getSlot(), cmdMsg->getPlayerID()));
	Int transSlot = translatedSlotPosition(msg->getPlayerID(), conMgr->getLocalPlayerID());

	if (isPlayerInGame(transSlot, conMgr) == FALSE) {
		// if they've been timed out, voted out, disconnected, don't count their vote.
		return;
	}

	applyDisconnectVote(cmdMsg->getSlot(), cmdMsg->getVoteFrame(), cmdMsg->getPlayerID(), conMgr);
}

void DisconnectManager::processDisconnectFrame(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetDisconnectFrameCommandMsg *cmdMsg = (NetDisconnectFrameCommandMsg *)msg;
	const UnsignedInt playerID = cmdMsg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}

	if (m_disconnectFrames[playerID] >= cmdMsg->getDisconnectFrame()) {
		// this message isn't valid, we have a disconnect frame that is later than this already.
		return;
	}

	if (m_disconnectFramesReceived[playerID] == TRUE) {
		DEBUG_LOG(("DisconnectManager::processDisconnectFrame - Got two disconnect frames without an intervening disconnect screen off command from player %d. Frames are %d and %d", playerID, m_disconnectFrames[playerID], cmdMsg->getDisconnectFrame()));
	}

	DEBUG_LOG(("DisconnectManager::processDisconnectFrame - about to call resetPlayersVotes for player %d", playerID));
	resetPlayersVotes(playerID, cmdMsg->getDisconnectFrame()-1, conMgr);

	m_disconnectFrames[playerID] = cmdMsg->getDisconnectFrame();
	m_disconnectFramesReceived[playerID] = TRUE;
	DEBUG_LOG(("DisconnectManager::processDisconnectFrame - Got a disconnect frame for player %d, frame = %d, local player is %d, local disconnect frame = %d, command id = %d", cmdMsg->getPlayerID(), cmdMsg->getDisconnectFrame(), conMgr->getLocalPlayerID(), m_disconnectFrames[conMgr->getLocalPlayerID()], cmdMsg->getID()));

	if (playerID == conMgr->getLocalPlayerID()) {
		DEBUG_LOG(("DisconnectManager::processDisconnectFrame - player %d is the local player", playerID));
		// we just got the message from the local player, check to see if we need to send
		// commands to anyone we already have heard from.
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			if (i != playerID) {
				Int transSlot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
				if (isPlayerInGame(transSlot, conMgr) == TRUE) {
					if ((m_disconnectFrames[i] < m_disconnectFrames[playerID]) && (m_disconnectFramesReceived[i] == TRUE)) {
						DEBUG_LOG(("DisconnectManager::processDisconnectFrame - I have more frames than player %d, my frame = %d, their frame = %d", i, m_disconnectFrames[conMgr->getLocalPlayerID()], m_disconnectFrames[i]));
						conMgr->sendFrameDataToPlayer(i, m_disconnectFrames[i]);
					}
				}
			}
		}
	} else if ((m_disconnectFrames[playerID] < m_disconnectFrames[conMgr->getLocalPlayerID()]) && (m_disconnectFramesReceived[playerID] == TRUE)) {
		DEBUG_LOG(("DisconnectManager::processDisconnectFrame - I have more frames than player %d, my frame = %d, their frame = %d", playerID, m_disconnectFrames[conMgr->getLocalPlayerID()], m_disconnectFrames[playerID]));
		conMgr->sendFrameDataToPlayer(playerID, m_disconnectFrames[playerID]);
	}
}

void DisconnectManager::processDisconnectScreenOff(NetCommandMsg *msg, ConnectionManager *conMgr) {
	NetDisconnectScreenOffCommandMsg *cmdMsg = (NetDisconnectScreenOffCommandMsg *)msg;
	UnsignedInt playerID = cmdMsg->getPlayerID();

	DEBUG_LOG(("DisconnectManager::processDisconnectScreenOff - got a screen off command from player %d for frame %d", cmdMsg->getPlayerID(), cmdMsg->getNewFrame()));

	if (playerID >= MAX_SLOTS) {
		return;
	}

	UnsignedInt newFrame = cmdMsg->getNewFrame();
	if (newFrame >= m_disconnectFrames[playerID]) {
		DEBUG_LOG(("DisconnectManager::processDisconnectScreenOff - resetting the disconnect screen status for player %d", playerID));
		m_disconnectFramesReceived[playerID] = FALSE;
		m_disconnectFrames[playerID] = newFrame; // just in case we get packets out of order and the disconnect screen off message gets here before the disconnect frame message.

		DEBUG_LOG(("DisconnectManager::processDisconnectScreenOff - about to call resetPlayersVotes for player %d", playerID));
		resetPlayersVotes(playerID, cmdMsg->getNewFrame(), conMgr);
	}
}

void DisconnectManager::applyDisconnectVote(Int slot, UnsignedInt frame, Int fromSlot, ConnectionManager *conMgr) {
	// GeneralsX: Both indices are bounds-checked HERE, where every caller
	// routes through, rather than at the one call site that happens to be reachable from the wire.
	// processDisconnectVote passes cmdMsg->getSlot() straight in; that is an UnsignedByte read off the
	// packet (NetPacket::readDisconnectVoteMessage) with no bound anywhere between, and this line
	// writes m_playerVotes[slot][fromSlot] into a [MAX_SLOTS][MAX_SLOTS] array - slot 255 writes about
	// 24 KB past it. The sender-side check above validates the SENDER only, and countVotesForPlayer()
	// already bounds its own read, so this write was the whole of the hole. Pre-existing at 5600ce15b.
	// Measured: net-kill-gates.sh forge-vote.
	if ((slot < 0) || (slot >= MAX_SLOTS) || (fromSlot < 0) || (fromSlot >= MAX_SLOTS)) {
		static Int s_refusals = 0;	// Trade-off: capped so a flood of forgeries cannot turn into a flood of fflush
		if (s_refusals < 32) {
			++s_refusals;
			fprintf(stderr, "[GXGATE] DISCONNECTVOTE refused: slot %d from slot %d is out of range (frame %u)\n",
				slot, fromSlot, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
		return;
	}
	m_playerVotes[slot][fromSlot].vote = TRUE;
	m_playerVotes[slot][fromSlot].frame = frame;
	Int numVotes = countVotesForPlayer(slot);
	DEBUG_LOG(("DisconnectManager::applyDisconnectVote - added a vote to disconnect slot %d, from slot %d, for frame %d, current votes are %d", slot, fromSlot, frame, numVotes));
	Int transSlot = translatedSlotPosition(slot, conMgr->getLocalPlayerID());
	if (transSlot != -1) {
		TheDisconnectMenu->updateVotes(transSlot, numVotes);
	}
}

void DisconnectManager::nextFrame(UnsignedInt frame, ConnectionManager *conMgr) {
	m_lastFrame = frame;
	const time_t frameStart = m_lastFrameTime;
	m_lastFrameTime = timeGetTime();
	if ((m_lastFrameTime - frameStart) >= ANSWER_STILL_MS) {
		m_slowFrameTime = m_lastFrameTime;	// GeneralsX: see processDisconnectKeepAlive
	}
	// GeneralsX: The silence clock's floor moves only with PLAIN play: a frame that moves
	// while the disconnect screen is on, or within NetworkDisconnectTime of it going off, may be running on input the
	// vanished peer sent before it died - a survivor's catch-up resend (processDisconnectFrame, at any time during the
	// stall), or the run of frames this peer already held behind it - and must not restart its silence (
	// vpd-r5; measured on the stock path, survivors two frames apart leapfrogged each other's resends for 130 s,
	// net-kill-gates.sh legit-destroy VICTIM=0). Frames are not what keeps a live peer from being found silent during
	// a stall: its keep-alives are - its own once its frame stands still, its answers while it moves
	// (processDisconnectKeepAlive) - so a peer whose loop runs is heard whatever the frames do.
	//
	// THE TRADE, chosen on purpose. A catch-up that takes longer than NetworkDisconnectTime to play through (a match
	// below a few frames per second) can still refresh this floor from old input and add up to one more s_silenceMs
	// to a drop. Freezing the floor by frames instead (through MAX_FRAMES_AHEAD) was tried in 270da7109
	// and is worse: at that frame rate it stays frozen past s_silenceMs while fresh play goes on, and a live peer that
	// then pauses briefly is dropped the moment the screen opens. A late drop is the safe side.
	//
	// Only a release that followed this peer's frame notice starts that window (allCommandsReady): a catch-up resend
	// answers a notice (processDisconnectFrame), so a stall that ended on its own, before any notice, was ended by
	// input that really arrived, and its frames are plain play. Freezing the floor after it too would leave the clock
	// at that stall's start, and a peer that went quiet just after it would get less than s_silenceMs.
	// The cost is the safe side again: a peer that vanished DURING such a stall (a second fault) can have the frames it
	// sent before it died restart its clock once - up to that stall's length later, never sooner.
	if ((m_screenOffTime == 0) || ((m_lastFrameTime - m_screenOffTime) >= TheGlobalData->m_networkDisconnectTime)) {
		m_liveFrameTime = m_lastFrameTime;
	}
	resetPlayerTimeouts(conMgr);
}

void DisconnectManager::allCommandsReady(UnsignedInt frame, ConnectionManager *conMgr, Bool waitForPacketRouter) {
		if (m_disconnectState != DISCONNECTSTATETYPE_SCREENOFF) {
			DEBUG_LOG(("DisconnectManager::allCommandsReady - setting screen state to off."));

			TheDisconnectMenu->hideScreen();
			m_disconnectState = DISCONNECTSTATETYPE_SCREENOFF;
			if (m_haveNotifiedOtherPlayersOfCurrentFrame) {
				m_screenOffTime = timeGetTime();	// GeneralsX: see nextFrame
				m_catchUpUntil = m_screenOffTime + CATCHUP_MS;	// GeneralsX: see update
			}
			TheNetwork->notifyOthersOfNewFrame(frame);

			// reset the votes since we're moving to a new frame.
			for (Int i = 0; i < MAX_SLOTS; ++i) {
				m_playerVotes[i][conMgr->getLocalPlayerID()].vote = FALSE;
			}
			// GeneralsX: Held DISCONNECTPLAYERs (processDisconnectPlayer) are
			// NOT cleared here, though they used to be, on the theory that a frame completing proved
			// the victim alive. It proves only that its input for THAT frame arrived - late, or by
			// another peer's frame resend - from a victim that died a few frames later. Measured
			// (net-kill-gates.sh legit-destroy, router SIGTERMed): C held B's order for A, completed
			// the frame, lost the hold, stalled on the next, timed A out itself and then waited
			// forever for the order B had already sent once - B waiting on C in turn. Keeping it
			// costs nothing: it applies only once its sender is this peer's elected router, who could
			// send it again at that point anyway.

			DEBUG_LOG(("DisconnectManager::allCommandsReady - resetting m_timeOfDisconnectScreenOn"));
			m_timeOfDisconnectScreenOn = 0;
		}
}

Bool DisconnectManager::allowedToContinue() {
	if (m_disconnectState != DISCONNECTSTATETYPE_SCREENOFF) {
		return FALSE;
	}
	return TRUE;
}

void DisconnectManager::sendKeepAlive(ConnectionManager *conMgr) {
	time_t curTime = timeGetTime();

	if (((curTime - m_lastKeepAliveSendTime) > 500) || (m_lastKeepAliveSendTime == -1)) {
		NetDisconnectKeepAliveCommandMsg *msg = newInstance(NetDisconnectKeepAliveCommandMsg);
		msg->setPlayerID(conMgr->getLocalPlayerID());
		if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
			msg->setID(GenerateNextCommandID());
		}
		conMgr->sendLocalCommandDirect(msg, ALL_SLOTS_MASK ^ (gxSlotBit(msg->getPlayerID())));
		msg->detach();

		m_lastKeepAliveSendTime = curTime;
	}
}

void DisconnectManager::populateDisconnectScreen(ConnectionManager *conMgr) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		UnicodeString name = conMgr->getPlayerName(i);
		Int slot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
		if (slot != -1) {
			TheDisconnectMenu->setPlayerName(slot, name);

			Int numVotes = countVotesForPlayer(i);
			TheDisconnectMenu->updateVotes(slot, numVotes);
		}
	}
}

Int DisconnectManager::translatedSlotPosition(Int slot, Int localSlot) {
	if (slot < localSlot) {
		return slot;
	}

	if (slot == localSlot) {
		return -1;
	}

	return (slot - 1);
}

Int DisconnectManager::untranslatedSlotPosition(Int slot, Int localSlot) {
	if (slot == -1) {
		return localSlot;
	}

	if (slot < localSlot) {
		return slot;
	}

	return (slot + 1);
}

void DisconnectManager::resetPlayerTimeouts(ConnectionManager *conMgr) {
	// reset the player timeouts.
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		Int slot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
		if (slot != -1) {
			resetPlayerTimeout(slot);
		}
	}
}

void DisconnectManager::resetPlayerTimeout(Int slot) {
	m_playerTimeouts[slot] = timeGetTime();
}

void DisconnectManager::resetPacketRouterTimeout() {
	m_packetRouterTimeout = timeGetTime();
}

void DisconnectManager::turnOnScreen(ConnectionManager *conMgr) {
	TheDisconnectMenu->showScreen();
	DEBUG_LOG(("DisconnectManager::turnOnScreen - turning on screen on frame %d", TheGameLogic->getFrame()));
	m_disconnectState = DISCONNECTSTATETYPE_SCREENON;
	m_lastKeepAliveSendTime = -1;
	populateDisconnectScreen(conMgr);
	resetPlayerTimeouts(conMgr);
	TheDisconnectMenu->hidePacketRouterTimeout();

	m_haveNotifiedOtherPlayersOfCurrentFrame = FALSE;

	m_timeOfDisconnectScreenOn = timeGetTime();
	DEBUG_LOG(("DisconnectManager::turnOnScreen - turned on screen at time %d", m_timeOfDisconnectScreenOn));
}

void DisconnectManager::disconnectPlayer(Int slot, ConnectionManager *conMgr) {
	DEBUG_LOG(("DisconnectManager::disconnectPlayer - Disconnecting slot number %d on frame %d", slot, TheGameLogic->getFrame()));
	DEBUG_ASSERTCRASH((slot >= 0) && (slot < MAX_SLOTS), ("Attempting to disconnect an invalid slot number"));
	if ((slot < 0) || (slot >= (MAX_SLOTS))) {
		return;
	}

	if (TheGameInfo)
	{
		GameSlot *gSlot = TheGameInfo->getSlot( slot );
		if (gSlot)
		{
			gSlot->markAsDisconnected();
		}
	}

	Int transSlot = translatedSlotPosition(slot, conMgr->getLocalPlayerID());

	if (transSlot != -1) {
		// Ignore any disconnect commands that tell us to disconnect ourselves.

		// Get the disconnecting player off the disconnect window.
		UnicodeString uname = conMgr->getPlayerName(slot);
		TheRecorder->logPlayerDisconnect(uname, slot);
		TheDisconnectMenu->removePlayer(transSlot, uname);

		PlayerLeaveCode retcode = conMgr->disconnectPlayer(slot);
		DEBUG_ASSERTCRASH((retcode != PLAYERLEAVECODE_UNKNOWN), ("Invalid player leave code"));

		if (retcode == PLAYERLEAVECODE_PACKETROUTER) {
			DEBUG_LOG(("DisconnectManager::disconnectPlayer - disconnecting player was packet router."));

			conMgr->resendPendingCommands();
		}
	}
}

void DisconnectManager::sendDisconnectCommand(Int slot, ConnectionManager *conMgr) {
	DEBUG_LOG(("DisconnectManager::sendDisconnectCommand - Sending disconnect command for slot number %d", slot));
	DEBUG_ASSERTCRASH((slot >= 0) && (slot < MAX_SLOTS), ("Attempting to send a disconnect command for an invalid slot number"));
	if ((slot < 0) || (slot >= (MAX_SLOTS))) {
		return;
	}

	UnsignedInt disconnectFrame = getMaxDisconnectFrame();

	// Need to do the NetDisconnectPlayerCommandMsg creation and sending here.
	NetDisconnectPlayerCommandMsg *msg = newInstance(NetDisconnectPlayerCommandMsg);
	msg->setDisconnectSlot(slot);
	msg->setDisconnectFrame(disconnectFrame);
	msg->setPlayerID(conMgr->getLocalPlayerID());
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	conMgr->sendLocalCommand(msg);

	DEBUG_LOG(("DisconnectManager::sendDisconnectCommand - Sending disconnect command for slot number %d for frame %d", slot, disconnectFrame));

	msg->detach();
}

void DisconnectManager::sendVoteCommand(Int slot, ConnectionManager *conMgr) {
	NetDisconnectVoteCommandMsg *msg = newInstance(NetDisconnectVoteCommandMsg);

	msg->setPlayerID(conMgr->getLocalPlayerID());
	msg->setSlot(slot);
	msg->setVoteFrame(TheGameLogic->getFrame());
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}

	conMgr->sendLocalCommandDirect(msg, ALL_SLOTS_MASK & ~(gxSlotBit(conMgr->getLocalPlayerID())));

	msg->detach();
}

void DisconnectManager::voteForPlayerDisconnect(Int slot, ConnectionManager *conMgr) {
	Int transSlot = untranslatedSlotPosition(slot, conMgr->getLocalPlayerID());

	if (m_playerVotes[transSlot][conMgr->getLocalPlayerID()].vote == FALSE) {
		m_playerVotes[transSlot][conMgr->getLocalPlayerID()].vote = TRUE;

		sendVoteCommand(transSlot, conMgr);

		// we use the game logic frame cause we might not have sent out our own disconnect frame yet.
		applyDisconnectVote(transSlot, TheGameLogic->getFrame(), conMgr->getLocalPlayerID(), conMgr);
	}
}

void DisconnectManager::recalculatePacketRouterIndex(ConnectionManager *conMgr) {
	Int currentPacketRouterSlot = conMgr->getPacketRouterSlot();
	m_currentPacketRouterIndex = 0;
	while ((currentPacketRouterSlot != conMgr->getPacketRouterFallbackSlot(m_currentPacketRouterIndex)) && (m_currentPacketRouterIndex < MAX_SLOTS)) {
		++m_currentPacketRouterIndex;
	}
	DEBUG_ASSERTCRASH((m_currentPacketRouterIndex < MAX_SLOTS), ("Invalid packet router index"));
}

Bool DisconnectManager::allOnSameFrame(ConnectionManager *conMgr) {
	Bool retval = TRUE;
	for (Int i = 0; (i < MAX_SLOTS) && (retval == TRUE); ++i) {
		Int transSlot = translatedSlotPosition(i, conMgr->getLocalPlayerID());
		if (transSlot == -1) {
			continue;
		}
		if ((conMgr->isPlayerConnected(i) == TRUE) && (isPlayerInGame(transSlot, conMgr) == TRUE)) {
			// ok, i is someone who is in the game and hasn't timed out yet or been voted out.
			if (m_disconnectFramesReceived[i] == FALSE) {
				// we don't know what frame they are on yet.
				retval = FALSE;
			}
			if ((m_disconnectFramesReceived[i] == TRUE) && (m_disconnectFrames[conMgr->getLocalPlayerID()] != m_disconnectFrames[i])) {
				// We know their frame, but they aren't on the same frame as us.
				retval = FALSE;
			}
		}
	}
	return retval;
}

Bool DisconnectManager::isLocalPlayerNextPacketRouter(ConnectionManager *conMgr) {
	return nextPacketRouter(conMgr) == conMgr->getLocalPlayerID();
}

// GeneralsX: The walk isLocalPlayerNextPacketRouter always did, returning
// the slot it lands on, so processDisconnectPlayer can ask who that is for a sender other than us.
UnsignedInt DisconnectManager::nextPacketRouter(ConnectionManager *conMgr) {
	UnsignedInt localSlot = conMgr->getLocalPlayerID();
	UnsignedInt packetRouterSlot = conMgr->getPacketRouterSlot();
	Int transSlot = translatedSlotPosition(packetRouterSlot, localSlot);

	// stop when we have found a packet router that is connected
	while ((transSlot != -1) && (isPlayerInGame(transSlot, conMgr) == FALSE)) {
		packetRouterSlot = conMgr->getNextPacketRouterSlot(packetRouterSlot);
		if (packetRouterSlot >= MAX_SLOTS) {
			// don't know who the next packet router is going to be,
			// so this game is not going to go anywhere anymore.
			DEBUG_CRASH(("no more players left to be the packet router, this shouldn't happen."));
			return MAX_SLOTS;
		}
		transSlot = translatedSlotPosition(packetRouterSlot, localSlot);
	}

	return packetRouterSlot;
}

Bool DisconnectManager::hasPlayerTimedOut(Int slot, ConnectionManager *conMgr) {
	if (slot == -1) {
		return FALSE;
	}

	time_t newTime = TheGlobalData->m_networkPlayerTimeoutTime - (timeGetTime() - m_playerTimeouts[slot]);
	if ((newTime <= 0) || (silenceLeft(slot, conMgr) <= 0)) {
		return TRUE;
	}

	return FALSE;
}

// GeneralsX: The silence clock - see s_silenceMs at the top of this file. @p slot is a
// translated slot. Outside a disconnect screen, or while this peer hears no other player's keep-alive, there is no
// silence verdict and the stock timeout is returned, which the callers' min() leaves in charge.
time_t DisconnectManager::silenceLeft(Int slot, ConnectionManager *conMgr) {
	const time_t now = timeGetTime();
	// The verdict outlives a catch-up release (m_catchUpUntil): the successor may send its one DISCONNECTPLAYER just as
	// this peer completes a buffered frame and its screen goes off for a moment, and refusing that order then - the
	// silent router elected again - would leave this peer waiting on a router that will never send it twice
	//
	const Bool inCatchUp = (m_catchUpUntil != 0) && ((now - m_catchUpUntil) < 0);
	if ((slot < 0) || (slot >= MAX_SLOTS - 1) || ((m_disconnectState == DISCONNECTSTATETYPE_SCREENOFF) && !inCatchUp)) {
		return TheGlobalData->m_networkPlayerTimeoutTime;
	}
	Bool hearOthers = FALSE;
	for (Int j = 0; j < MAX_SLOTS - 1; ++j) {
		// Still connected: a peer that just quit leaves a fresh keep-alive behind, and counting it would hand the last
		// two players the fast verdict a 1v1 must not have.
		if ((j != slot) && (m_keepAliveTimes[j] != 0) && ((now - m_keepAliveTimes[j]) < SILENCE_HEARD_MS)
			&& conMgr->isPlayerConnected(untranslatedSlotPosition(j, conMgr->getLocalPlayerID()))) {
			hearOthers = TRUE;
		}
	}
	// A gap here only DELAYS a verdict, it never strands one. The case: this peer holds the successor's
	// order, the successor plays on without the vanished player and stops sending keep-alives, and with two survivors
	// there is nobody else to hear. But the successor can play only the input this peer sent before it stalled - its run-ahead,
	// at most MAX_FRAMES_AHEAD / 2 frames, ~2 s at full speed and 0.2-0.5 s at the usual latencies - and then its frame
	// stands still and it answers this peer's keep-alives within ANSWER_STILL_MS (processDisconnectKeepAlive). The
	// verdict, and the held order with it, comes back with that answer.
	if (!hearOthers) {
		return TheGlobalData->m_networkPlayerTimeoutTime;
	}
	const time_t lastHeard = (m_keepAliveTimes[slot] > m_liveFrameTime) ? m_keepAliveTimes[slot] : m_liveFrameTime;
	return s_silenceMs - (now - lastHeard);
}

// this function assumes that we are the packet router. (or at least that
// we will be after everyone is getting disconnected)
void DisconnectManager::sendPlayerDestruct(Int slot, ConnectionManager *conMgr) {
	UnsignedShort currentID = 0;
	if (DoesCommandRequireACommandID(NETCOMMANDTYPE_DESTROYPLAYER))
	{
		currentID = GenerateNextCommandID();
	}

	DEBUG_LOG(("Queueing DestroyPlayer %d for frame %d on frame %d as command %d",
		slot, TheNetwork->getExecutionFrame()+1, TheGameLogic->getFrame(), currentID));

	NetDestroyPlayerCommandMsg *netmsg = newInstance(NetDestroyPlayerCommandMsg);
	netmsg->setExecutionFrame(TheNetwork->getExecutionFrame()+1);
	netmsg->setPlayerID(conMgr->getLocalPlayerID());
	netmsg->setID(currentID);
	netmsg->setPlayerIndex(slot);
	conMgr->sendLocalCommand(netmsg);
	netmsg->detach();
}

// the 'slot' variable is supposed to be a translated slot position. (translated slot meaning
// that it is the player's position in the disconnect menu)
Bool DisconnectManager::isPlayerVotedOut(Int slot, ConnectionManager *conMgr) {
	if (slot == -1) {
		// we can't vote out ourselves.
		return FALSE;
	}
	Int transSlot = untranslatedSlotPosition(slot, conMgr->getLocalPlayerID());
	Int numVotes = countVotesForPlayer(transSlot);
	if (numVotes >= (conMgr->getNumPlayers() - 1)) {
		return TRUE;
	}
	return FALSE;
}

UnsignedInt DisconnectManager::getMaxDisconnectFrame() {
	UnsignedInt retval = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_disconnectFrames[i] > retval) {
			retval = m_disconnectFrames[i];
		}
	}
	return retval;
}

Bool DisconnectManager::isPlayerInGame(Int slot, ConnectionManager *conMgr) {
	Int transSlot = untranslatedSlotPosition(slot, conMgr->getLocalPlayerID());
	DEBUG_ASSERTCRASH((transSlot >= 0) && (transSlot < MAX_SLOTS), ("invalid slot number"));
	if (((transSlot < 0) || (transSlot >= MAX_SLOTS)) || conMgr->isPlayerConnected(transSlot) == FALSE) {
		return FALSE;
	}

	if (isPlayerVotedOut(slot, conMgr) == TRUE) {
		return FALSE;
	}

	if (hasPlayerTimedOut(slot, conMgr) == TRUE) {
		return FALSE;
	}

	return TRUE;
}

void DisconnectManager::playerHasAdvancedAFrame(Int slot, UnsignedInt frame) {
	// if they have advanced beyond the frame they had been previously disconnecting on.
	if (frame >= m_disconnectFrames[slot]) {
		m_disconnectFrames[slot] = frame; // just in case we get a disconnect frame command after this is called.
		m_disconnectFramesReceived[slot] = FALSE;
	}
}

Int DisconnectManager::countVotesForPlayer(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return 0;
	}

	Int retval = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		// using TheGameLogic->getFrame() cause we might not have sent our disconnect frame yet.
		if ((m_playerVotes[slot][i].vote == TRUE) && (m_playerVotes[slot][i].frame == TheGameLogic->getFrame())) {
			++retval;
		}
	}

	return retval;
}

void DisconnectManager::resetPlayersVotes(Int playerID, UnsignedInt frame, ConnectionManager *conMgr) {
	DEBUG_LOG(("DisconnectManager::resetPlayersVotes - resetting player %d's votes on frame %d", playerID, frame));

	// we need to reset this player's votes that happened before or on the given frame.
	for(Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_playerVotes[i][playerID].frame <= frame) {
			DEBUG_LOG(("DisconnectManager::resetPlayersVotes - resetting player %d's vote for player %d from frame %d on frame %d", playerID, i, m_playerVotes[i][playerID].frame, frame));
			m_playerVotes[i][playerID].vote = FALSE;
		}
	}

	Int numVotes = countVotesForPlayer(playerID);
	DEBUG_LOG(("DisconnectManager::resetPlayersVotes - after adjusting votes, player %d has %d votes", playerID, numVotes));
	Int transSlot = translatedSlotPosition(playerID, conMgr->getLocalPlayerID());
	if (transSlot != -1) {
		TheDisconnectMenu->updateVotes(transSlot, numVotes);
	}
}
