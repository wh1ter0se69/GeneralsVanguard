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

/**
 * The connection manager maintains a collection of Connections to each player.  Messages are queued up
 * from here, as well as removed through here.
 */

#pragma once

#include "GameNetwork/Connection.h"
#include "GameNetwork/NetCommandList.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/FrameDataManager.h"
#include "GameNetwork/FrameMetrics.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/DisconnectManager.h"

class GameInfo;
class NetCommandWrapperList;
struct GXReadyItem;

typedef std::map<UnsignedShort, AsciiString> FileCommandMap;
typedef std::map<UnsignedShort, RelayMaskType> FileMaskMap;	// GeneralsX: widened with RelayMaskType
typedef std::map<UnsignedShort, Int> FileProgressMap;

class ConnectionManager
{
public:
	ConnectionManager();
	~ConnectionManager();

	virtual void init();				///< Initialize this instance.
	virtual void reset();				///< Take this instance back to the initial state.
	virtual void update(Bool isInGame);			///< Service the Connections being managed by this instance.
	void flushSends();											///< GeneralsX (H02): drain the socket into the transport buffer, then send - no relay, no diagnostics.
	void gxNetSample();							///< [GXNET] 1 Hz liveness probe, off unless GX_NET is set. See the .cpp.
	void gxStallWatch();						///< [GXSTALL] one line per stall EVENT; separates waiting from computing. See the .cpp.
	void gxRouterTrace();						///< [GXROUTE] the router's own latency row, off unless GX_ROUTER_TRACE is set. See the .cpp.

	// End SubsystemInterface functions

	void updateRunAhead(Int oldRunAhead, Int frameRate, Bool didSelfSlug, Int nextExecutionFrame);	///< Update the run ahead value.  If we are the current packet router, issue the command.

	void attachTransport(Transport *transport);

	void parseUserList(const GameInfo *game);
	void sendChat(UnicodeString text, RelayMaskType playerMask, UnsignedInt executionFrame);
	void sendDisconnectChat(UnicodeString text);
	void sendLocalCommand(NetCommandMsg *msg, RelayMaskType relay = ALL_SLOTS_MASK);		///< Send command to the players specified in the relay, goes through packet router.
	void sendLocalCommandDirect(NetCommandMsg *msg, RelayMaskType relay);		///< Send command directly to the players specified, doesn't go through packet router.
	void sendLocalGameMessage(GameMessage *msg, UnsignedInt frame);
	void sendCommand(NetCommandMsg *msg);
	Bool allCommandsReady(UnsignedInt frame, Bool justTesting = FALSE);
	void handleAllCommandsReady();
	NetCommandList *getFrameCommandList(UnsignedInt frame);
//	void AddConnection(User *user, UnsignedInt slot);
	// GeneralsX: determineRouterFallbackPlan() was declared here and never
	// defined - its only body was a commented-out block in the .cpp. That body stored a 1-BASED RANK
	// at a SLOT index (m_packetRouterFallback[slot] = rank), which is the exact inverse of the live
	// convention built by parseUserList (m_packetRouterFallback[rank] = slot) and read by
	// getPacketRouterFallbackSlot, disconnectPlayer and getNextPacketRouterSlot. Anyone who "restored"
	// it would have silently transposed the fallback plan on every peer. Removed rather than fixed:
	// parseUserList already builds the plan, and a declaration with no definition is a landmine.
	void zeroFrames(UnsignedInt startingFrame, UnsignedInt numFrames);
	void destroyGameMessages();
//	void createConnections(UnsignedInt numberOfPlayers, UnsignedInt localSlot);
	void setLocalAddress(UnsignedInt ip, UnsignedInt port);
	void initTransport();
	void processFrameTick(UnsignedInt frame);
	void handleLocalPlayerLeaving(UnsignedInt frame);

	void sendFile(AsciiString path, RelayMaskType playerMask, UnsignedShort commandID);
	UnsignedShort sendFileAnnounce(AsciiString path, RelayMaskType playerMask);
	Int getFileTransferProgress(Int playerID, AsciiString path);
	Bool areAllQueuesEmpty();

	UnsignedInt getLocalPlayerID();
	UnicodeString getPlayerName(Int playerNum);
	Int getNumPlayers();

	UnsignedInt getPacketRouterFallbackSlot(Int packetRouterNumber);	///< Returns the slot of the given packet router number in the fallback plan.
	UnsignedInt getPacketRouterSlot();																///< Returns the current packet router's slot.
	PlayerLeaveCode disconnectPlayer(Int slot);												///< Disconnect this player immediately.  This should only be called by the disconnect manager.
	void disconnectLocalPlayer();																			///< Does whatever is necessary to get TheNetwork to realize that it should be leaving the game now.
	void quitGame();																									///< Disconnect from the game RIGHT NOW!!  Tell everyone else we are disconnecting.

	void voteForPlayerDisconnect(Int slot);														///< Register a vote for a player to be disconnected.

	void resendPendingCommands();																			///< Resend the pending commands to the packet router.

	void setFrameGrouping(time_t frameGrouping);				///< Set the number of frames that are grouped together into packets.

	PlayerLeaveCode processPlayerLeave(NetPlayerLeaveCommandMsg *msg);
	Bool canILeave();																///< Returns true if the local player is allowed to leave.

	// Bandwidth metrics
	Real getIncomingBytesPerSecond();
	Real getIncomingPacketsPerSecond();
	Real getOutgoingBytesPerSecond();
	Real getOutgoingPacketsPerSecond();
	Real getUnknownBytesPerSecond();
	Real getUnknownPacketsPerSecond();
	UnsignedInt getPacketArrivalCushion();

	UnsignedInt getMinimumCushion();

	void flushConnections();

	void processChat(NetChatCommandMsg *msg); // this actually needs to be public because it is frame-synchronized

	void updateLoadProgress( Int progress );
	void loadProgressComplete();
	void sendTimeOutGameStart();

	Bool isPacketRouter();

	Bool isPlayerConnected( Int playerID );

	void notifyOthersOfCurrentFrame(Int frame);
	void sendFrameDataToPlayer(UnsignedInt playerID, UnsignedInt startingFrame);
	void sendSingleFrameToPlayer(UnsignedInt playerID, UnsignedInt frame);
	void notifyOthersOfNewFrame(UnsignedInt frame);

	UnsignedInt getNextPacketRouterSlot(UnsignedInt playerID); ///< returns the packet router player that comes after the given player.
	UnsignedInt lockstepRouter(UnsignedInt without);	///< The router as the EXECUTED order stream alone decides it, @p without skipped. See the .cpp.
	void lockstepRemove(UnsignedInt slot);				///< An executed leave or kill took @p slot out of the match. See lockstepRouter.

	Int getAverageFPS();
	Int getSlotAverageFPS(Int slot);

#if defined(RTS_DEBUG)
	void debugPrintConnectionCommands();
#endif

	// For disconnect blame assignment
	UnsignedInt getPingFrame();
	Int getPingsSent();
	Int getPingsReceived();

	// GeneralsX: THIS peer's own measured round trip, in SECONDS.
	//
	// This is the one to show a player. FrameMetrics::processLatencyResponse stamps the send time
	// per frame into m_pendingLatencies and, on the matching ack, records curTime - sendTime - a
	// real round trip - then averages it over m_networkLatencyHistoryLength. Every peer measures
	// it, host or not.
	//
	// Use this rather than getMaximumLatency() for anything player-facing. That one is only
	// assigned for the LOCAL slot inside updateRunAhead (ConnectionManager.cpp:1387), which only
	// the packet router runs, so on a non-host peer the array stays zero and it returns 0 - which
	// a HUD then displays as a perfect 0 ms connection. It also averages the TWO highest peers, so
	// with a single measured peer it reports half the real figure.
	//
	// Returns 0 before the first ack lands. Callers must distinguish that from a genuine 0.
	Real getLocalAverageLatency() { return m_frameMetrics.getAverageLatency(); }

	// GeneralsX: Was getLocalMinimumLatency(), which reduced a ring that
	// mixes every peer's samples together on the packet router and therefore reported the FASTEST
	// peer - the one number that cannot be hurting a lockstep match. This returns the WORST peer we
	// are still hearing from, in SECONDS, and names it in worstSlot. Negative, with worstSlot -1,
	// means no peer has a live measurement.
	//
	// On the router that is genuinely the lobby's worst leg. On a non-router peer only one leg is
	// ever measured (the router's), so it is d(me, router) and worstSlot names the router - which is
	// why the slot travels with the number instead of being dropped. See FrameMetrics.cpp.
	Real getWorstPeerLatency(Int &worstSlot) { return m_frameMetrics.getWorstPeerLatency(worstSlot); }

	// GeneralsX: Promoted from private so the HUD can read it.
	//
	// This is the ONLY latency number the engine already maintains for the whole lobby, and it is
	// the one the router itself trusts - updateRunAhead sizes the run-ahead window from exactly
	// this call. Surfacing a different, prettier number beside the fps would mean the thing the
	// player sees and the thing the engine acts on could disagree, which is worse than no readout.
	//
	// Note the semantics before using it: it is the average of the two HIGHEST per-player average
	// latencies, i.e. a lobby-wide worst case, not "my round trip to the host". It is in SECONDS.
	Real getMaximumLatency(); ///< Returns the average of the two highest average latencies between players.

	// GeneralsX: Round trip to @p slot in MILLISECONDS for the in-game
	// player list, or -1 for "no live figure" - which the scoreboard must render as a dash and
	// never as a zero.
	//
	// -1 is returned in every case where the peer genuinely does not know, and those cases are
	// ordinary rather than exceptional: the slot is empty or is the local player; the router has
	// not published yet (the first report is up to m_networkRunAheadMetricsTime into the match);
	// this peer never received one; or the last one aged out. A client that is not the router
	// depends entirely on the router's broadcast for every slot but its own.
	Int getPeerPingMs(Int slot);

private:
	void doRelay();
	void publishLatencyReport();
	void processLatencyReport(NetLatencyReportCommandMsg *msg);
	void doKeepAlive();
	void sendRemoteCommand(NetCommandRef *msg);
	void forwardOnConnections(NetCommandRef *msg, RelayMaskType relay);	///< sendRemoteCommand's connection loop + m_relayedCommands; no frame data
	void ackCommand(NetCommandRef *ref, UnsignedInt localSlot, UnsignedInt hopSlot = MAX_SLOTS);
	void sendWrapperAck(Int hopSlot, const NetWrapperCommandMsg *chunk);	///< GeneralsX: the WRAPPERACK of one admitted chunk, direct to its hop
	void relayDatagramCommand(NetCommandRef *cmd);		///< doRelay, per command of a datagram: ack, process, relay
	void processReadyWrapped(const GXReadyItem &item);	///< GeneralsX: one reassembled serialization (doRelay) - see the .cpp
	Bool isOutOfDate(NetCommandMsg *msg);	///< a synchronized command for a frame this peer has already executed
	Bool isOverSoftCap(Int originator);	///< GeneralsX: this router holds too much of @p originator's relayed big commands - see the .cpp
	void dropConnection(Int slot);		///< GeneralsX: delete a slot's connection and free its receiver state
#if defined(GX_TEST_INSTRUMENTS)
public:
	void gxTestWrapFlood(UnsignedInt count);	///< GX_NET_TEST_WRAPFLOOD: Connection::gxTestWrapFlood on every live connection
private:
#endif

	Bool processNetCommand(NetCommandRef *ref);
	Bool arrivedFrom(UnsignedInt slot);	///< Did the datagram being processed come from this slot's peer? See the .cpp.
	UnsignedInt arrivalSlot();	///< The slot whose peer sent the datagram being processed; MAX_SLOTS if none. See the .cpp.
	Bool isForgedDirect(NetCommandMsg *msg);	///< A disconnect-range or FRAMERESENDREQUEST command not from the peer it names: refused and not acked.
	void processAckStage1(NetCommandMsg *msg);
	void processAckStage2(NetCommandMsg *msg);
	void processAck(NetCommandMsg *msg);
	void processFrameInfo(NetFrameCommandMsg *msg);
	void sampleArrivalCushion(NetCommandMsg *cmd);	///< GeneralsX: first copies only - see the .cpp.
	void processRunAheadMetrics(NetRunAheadMetricsCommandMsg *msg);
	void processDisconnectChat(NetDisconnectChatCommandMsg *msg);
	void processProgress( NetProgressCommandMsg *msg );
	void processLoadComplete( NetCommandMsg *msg );
	void processTimeOutGameStart( NetCommandMsg *msg );
	void processWrapper(NetCommandRef *ref);
	void processFrameResendRequest(NetFrameResendRequestCommandMsg *msg);

	void processFile(NetFileCommandMsg *ref);
	void processFileAnnounce(NetFileAnnounceCommandMsg *ref);
	void processFileProgress(NetFileProgressCommandMsg *ref);

	//	void doPerFrameMetrics(UnsignedInt frame);
	void getMinimumFps(Int &minFps, Int &minFpsPlayer);			///< Returns the smallest FPS in the m_fpsAverages list.

	void requestFrameDataResend(Int playerID, UnsignedInt frame); ///< request of this player that he send the specified frame's data.

	void electPacketRouter();		///< Choose the packet router for this match. Runs on every peer, must agree. See the .cpp.

	// The connections are set up like the slot list. The connection corresponding to the local
	// player's position in the slot list will be null.  Connections corresponding to slots that
	// do not have a player will also be null.
	Connection *m_connections[MAX_SLOTS];

	Transport *m_transport;
	const TransportMessage *m_recvFrom;	///< the datagram doRelay is processing now, nullptr for reassembled wrappers
	Int m_softCapCache[MAX_SLOTS];		///< GeneralsX: isOverSoftCap per originator for this doRelay pass; -1 = not computed yet
	UnsignedInt m_localSlot;
	UnsignedInt m_packetRouterSlot;
	UnsignedInt m_packetRouterFallback[MAX_SLOTS];
	UnsignedInt m_lockstepPlan[MAX_SLOTS];	///< the router succession parseUserList elected, router first; fixed for the match
	RelayMaskType m_lockstepGone;				///< bit per slot an executed leave or kill took out of it
	RelayMaskType m_sharedAddrSlots;			///< bit per slot whose address another slot shares, from parseUserList; fixed for the match
	UnsignedInt m_localAddr;
	UnsignedInt m_localPort;
	User* m_localUser;

	DisconnectManager *m_disconnectManager;						///< Controls the disconnect dialog.

	FrameDataManager *m_frameData[MAX_SLOTS];

	NetCommandList *m_pendingCommands;
	NetCommandList *m_relayedCommands;

	FrameMetrics m_frameMetrics;

	NetCommandWrapperList *m_netCommandWrapperList;

	// These variables are used to keep track of the other players' average fps and latency.
	// yup.
	Real m_latencyAverages[MAX_SLOTS];
	Int  m_fpsAverages[MAX_SLOTS];

	// GeneralsX: THE LOBBY-WIDE PING ROW, FOR DISPLAY ONLY.
	//
	// Deliberately a SEPARATE array from m_latencyAverages above, which feeds getMaximumLatency()
	// and therefore the run-ahead window. Nothing that arrives over the wire from another peer
	// may reach that one, and keeping the two apart is what makes "this cannot perturb the
	// simulation" a structural property instead of a promise: grep the readers of
	// m_peerPingMs and they are the display accessor and nothing else.
	//
	// Milliseconds, slot-indexed. 0 means "no live measurement for this slot" - the same sentinel
	// the wire uses - so a slot that was never reported renders as a dash rather than as 0 ms.
	//
	// On the packet router this is filled from its OWN FrameMetrics every publish tick, so the
	// host needs no wire traffic to populate its scoreboard. On every other peer it is filled by
	// processLatencyReport from the router's broadcast.
	UnsignedShort m_peerPingMs[MAX_SLOTS];

	// When each slot's figure last changed hands, so a router that stops publishing (or a peer
	// that stops being reported) expires out of the scoreboard instead of freezing at its last
	// value for the rest of the match. Same reasoning, and the same failure mode, as
	// FrameMetrics::m_peerLastSampleTime.
	time_t m_peerPingTime[MAX_SLOTS];
	// GeneralsX: Every slot's average render fps as the ROUTER last published it (the latency
	// report's fps row), for getSlotAverageFPS on a client. -1 = no figure. Display only: the pacing inputs stay
	// m_fpsAverages, which only processRunAheadMetrics and the router itself write; nothing here writes them.
	Short m_peerFps[MAX_SLOTS];
	time_t m_peerFpsTime[MAX_SLOTS];
	Int  m_minFpsPlayer;
	Int  m_minFps;
	UnsignedInt m_smallestPacketArrivalCushion;
	Bool m_didSelfSlug;

	// -----------------------------------------------------------------------------
	FileCommandMap s_fileCommandMap;
	FileMaskMap s_fileRecipientMaskMap;
	FileProgressMap s_fileProgressMap[MAX_SLOTS];
	// -----------------------------------------------------------------------------
};
