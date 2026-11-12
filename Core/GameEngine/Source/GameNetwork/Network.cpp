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

// FILE: Network.cpp ////////////////////////////////////////////////////
// Implementation of Network singleton
// Author: Matthew D. Campbell, July 2001
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/udp.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/networkutil.h"
#include "WWLib/strtok_r.h"
#include "GameClient/Shell.h"
#include "Common/CRCDebug.h"
#include "GameLogic/GameLogic.h"
#include "Common/FramePacer.h"	// GeneralsX: fast forward: timeForNewFrame yields to the main loop
#include "Common/CatchUpPolicy.h"	// GeneralsX: tick catch-up: the pacer step and its debt
#include "Common/GXAdminOrderMsg.h"	// GeneralsX: signing an admin's cheat orders (gxSealAdminOrder)
#include "GameClient/GXAdminKey.h"

#include "Common/RandomValue.h"


#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "Common/Recorder.h"
#include "GameClient/DesyncReport.h"
#include "GameClient/GameWindowManager.h"


#if defined(DEBUG_CRC) && !RETAIL_COMPATIBLE_NETWORKING
Int NET_CRC_INTERVAL = 1;
#else
// GeneralsX: A frame count standing in for 3.33 s of wall clock. Left as a
// fixed 100 it would fire twice as often at 60 Hz, doubling CRC exchange traffic for no benefit.
// Evaluates to exactly 100 at the default 30 Hz.
//
// Safe to scale ONLY because both peers of a match necessarily share a tick rate: engineID hashes
// LOGICFRAMES_PER_SECOND, so a 30 Hz and a 60 Hz peer are refused at join. That matters here more
// than elsewhere - this interval is serialised host->client (GameInfo.cpp) and a MISMATCHED value
// makes peers emit CRCs on different frames, which trips `m_cachedCRCs.size() < numPlayers` and
// reports a FAKE desync indistinguishable from a real one.
Int NET_CRC_INTERVAL = (100 * LOGICFRAMES_PER_SECOND) / 30;
#endif

// DEFINES ////////////////////////////////////////////////////////////////////

#define RESEND_INTERVAL 1

// PRIVATE TYPES //////////////////////////////////////////////////////////////

/**
 * Connection message - encapsulating info kept by the connection layer about each
 * packet.  These structs make up the in/out buffers at the connection layer.
 */
#pragma pack(push, 1)
struct ConnectionMessage
{
	Int id;
	NetMessageFlags flags;
	UnsignedByte data[MAX_NETWORK_MESSAGE_LEN];
	time_t lastSendTime;
	Int retries;
	Int length;
};
#pragma pack(pop)

static const int CmdMsgLen = 6; //< Minimum size of a command packet (Int + Unsigned Short)

// PRIVATE DATA ///////////////////////////////////////////////////////////////



// PUBLIC DATA ////////////////////////////////////////////////////////////////

/// The Network singleton instance
NetworkInterface *TheNetwork = nullptr;

// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////

/**
 * The Network class is used to instantiate a singleton which
 * implements the interface to all Network operations such as message stream processing and network communications.
 */
class Network : public NetworkInterface
{
public:
	//---------------------------------------------------------------------------------------
	// Setup / Teardown functions
	Network();
	virtual ~Network() override;
	virtual void init() override;																				///< Initialize or re-initialize the instance
	virtual void reset() override;																				///< Reinitialize the network
	virtual void update() override;																			///< Process command list
	virtual void liteupdate() override;																	///< Do a lightweight update to send packets and pass messages.
	virtual void sendLocalCommands() override;															///< GeneralsX: send translated orders now, relay nothing (H02).
	Bool deinit();																			///< Shutdown connections, release memory

	virtual void setLocalAddress(UnsignedInt ip, UnsignedInt port) override;
	virtual UnsignedInt getRunAhead() override { return m_runAhead; }
	virtual UnsignedInt getFrameRate() override { return m_frameRate; }
	virtual Int getPacerFramesDue() override;
	virtual UnsignedInt getPacketArrivalCushion() override;								///< Returns the smallest packet arrival cushion since this was last called.
	virtual Bool isFrameDataReady() override;
	virtual Bool isStalling() override;
	virtual void parseUserList( const GameInfo *game ) override;
	virtual void startGame() override;																			///< Sets the network game frame counter to -1

	virtual void sendChat(UnicodeString text, RelayMaskType playerMask) override;
	virtual void sendDisconnectChat(UnicodeString text) override;

	virtual void sendFile(AsciiString path, RelayMaskType playerMask, UnsignedShort commandID) override;
	virtual UnsignedShort sendFileAnnounce(AsciiString path, RelayMaskType playerMask) override;
	virtual Int getFileTransferProgress(Int playerID, AsciiString path) override;
	virtual Bool areAllQueuesEmpty() override;

	virtual void quitGame() override;
	virtual void selfDestructPlayer(Int index) override;


	virtual void voteForPlayerDisconnect(Int slot) override;
	virtual Bool isPacketRouter() override;
	virtual Int getPacketRouterSlot() override;

	// Bandwidth metrics
	virtual Real getIncomingBytesPerSecond() override;
	virtual Real getIncomingPacketsPerSecond() override;
	virtual Real getOutgoingBytesPerSecond() override;
	virtual Real getOutgoingPacketsPerSecond() override;
	virtual Real getUnknownBytesPerSecond() override;
	virtual Real getUnknownPacketsPerSecond() override;

	// Multiplayer Load Progress Functions
	virtual void updateLoadProgress( Int percent ) override;
	virtual void loadProgressComplete() override;
	virtual void sendTimeOutGameStart() override;

#if defined(RTS_DEBUG)
	// Disconnect screen testing
	virtual void toggleNetworkOn() override;
#endif

	// Exposing some info contained in the Connection Manager
	virtual UnsignedInt getLocalPlayerID() override;
	virtual UnicodeString getPlayerName(Int playerNum) override;
	virtual Int getNumPlayers() override;

	virtual Int getAverageFPS() override { return m_conMgr->getAverageFPS(); }
	virtual Int getSlotAverageFPS(Int slot) override;

	// GeneralsX: Latency for the HUD, seconds -> milliseconds here.
	//
	// Null-guarded unlike getAverageFPS above, deliberately: this one is read by InGameUI on the
	// render path, which runs during the load screen and the tail of a match teardown, and
	// m_conMgr is null in both. getAverageFPS is only ever read from places that already know a
	// game is up.
	virtual Real getDisplayLatencyMs(Int &fromSlot) override
	{
		fromSlot = -1;

		if (m_conMgr == NULL)
			return -1.0f;

		// Measured ack round trips, not getMaximumLatency(): that is only set for the local slot in updateRunAhead, which
		// only the packet router runs, so a non-router peer would show 0 ms. Reduced with the minimum (the mean adds half
		// a local frame), taken within one peer, and the worst peer is reported with its slot. A mixed lobby reduced over
		// the shared ring would report the fastest peer. See FrameMetrics::getWorstPeerLatency.
		Int worstSlot = -1;
		const Real seconds = m_conMgr->getWorstPeerLatency(worstSlot);

		// Negative means "no measurement yet", which the HUD must render differently from a
		// genuine low ping. GeneralsOnline does the same thing - every latency accessor in their
		// client returns -1 when unavailable and the UI prints "Unknown" rather than a number.
		if (seconds <= 0.0f)
			return -1.0f;

		fromSlot = worstSlot;
		return seconds * 1000.0f;
	}

	// GeneralsX: Per-slot ping for the in-game player list.
	//
	// Null-guarded for the same reason getDisplayLatencyMs above is: this is read from the render
	// path, which runs while the Diplomacy window is up during match teardown with m_conMgr
	// already gone.
	virtual Int getPeerPingMs(Int slot) override
	{
		if (m_conMgr == NULL)
			return -1;

		return m_conMgr->getPeerPingMs(slot);
	}

	virtual void attachTransport(Transport *transport) override;
	virtual void initTransport() override;

	virtual void setSawCRCMismatch(const std::map<Int, UnsignedInt> &playerCRCs, Int numPlayers) override;
	virtual Bool sawCRCMismatch() override { return m_sawCRCMismatch; }
	virtual Bool isPlayerConnected( Int playerID ) override;

	virtual void notifyOthersOfCurrentFrame() override;														///< Tells all the other players what frame we are on.
	virtual void notifyOthersOfNewFrame(UnsignedInt frame) override;								///< Tells all the other players that we are on a new frame.

	virtual Int  getExecutionFrame() override;																			///< Returns the next valid frame for simultaneous command execution.

	// For disconnect blame assignment
	virtual UnsignedInt getPingFrame() override;
	virtual Int getPingsSent() override;
	virtual Int getPingsReceived() override;

protected:
	void GetCommandsFromCommandList();														///< Remove commands from TheCommandList and put them on the Network command list.
	void SendCommandsToConnectionManager();												///< Send the new commands to the ConnectionManager
	Bool AllCommandsReady(UnsignedInt frame);											///< Do we have all the commands for the given frame?
	void RelayCommandsToCommandList(UnsignedInt frame);						///< Put the commands for the given frame onto TheCommandList.
	static Bool isMessageTypeWithinNetworkRange(GameMessage::Type type);
	Bool processCommand(GameMessage *msg);												///< Whatever needs to be done as a result of this command, do it now.
	void processFrameSynchronizedNetCommand(NetCommandRef *msg);	///< If there is a network command that needs to be executed at the same frame number on all clients, it happens here.
	void processRunAheadCommand(NetRunAheadCommandMsg *msg);			///< Do what needs to be done when we get a new run ahead command.
	void processDestroyPlayerCommand(NetDestroyPlayerCommandMsg *msg);	///< Do what needs to be done when we need to destroy a player.
#if defined(GX_TEST_INSTRUMENTS)
	void gxTestWrapFlood();		///< GX_NET_TEST_WRAPFLOOD test instrument, see its definition.
	void gxTestForge();																					///< GX_TEST_FORGE test instrument, see its definition.
#endif
	void endOfGameCheck();																				///< Checks to see if its ok to leave this game.  If it is, send the apropriate command to the game logic.
	Bool timeForNewFrame();

	ConnectionManager *m_conMgr;																	///< The connection manager object

	UnsignedInt m_lastFrame;																			///< The last game logic frame that was processed.

	NetLocalStatus m_localStatus;															///< My local status as a player in this game.

	Int m_runAhead;																						///< The current run ahead of the game.
	Int m_frameRate;
	Int m_lastExecutionFrame;																	///< The highest frame number that a command could have been executed on.
	Int m_lastFrameCompleted;
	Bool m_didSelfSlug;
	// GeneralsX: Use int64_t instead of __int64 (C99 standard, fighter19 pattern)
	int64_t m_perfCountFreq;														///< The frequency of the performance counter.

	int64_t m_lastFrameDelay;	///< GeneralsX: the delay timeForNewFrame last used (self-slug included), for getPacerFramesDue
	int64_t m_nextFrameTime;														///< When did we execute the last frame?  For slugging the GameLogic...

	Bool m_frameDataReady;																		///< Is the frame data for the next frame ready to be executed by TheGameLogic?
	Bool m_isStalling;

	// CRC info
	Bool m_checkCRCsThisFrame;
	Bool m_sawCRCMismatch;
	std::vector<UnsignedInt> m_CRC[MAX_SLOTS];
	std::list<Int> m_playersToDisconnect;
	GameWindow *m_messageWindow;

#if defined(RTS_DEBUG)
	Bool m_networkOn;
#endif
};

UnsignedInt Network::getPingFrame()
{
	return (m_conMgr)?m_conMgr->getPingFrame():0;
}

Int Network::getPingsSent()
{
	return (m_conMgr)?m_conMgr->getPingsSent():0;
}

Int Network::getPingsReceived()
{
	return (m_conMgr)?m_conMgr->getPingsReceived():0;
}

Bool Network::isPlayerConnected( Int playerID ) {
	if (playerID == getLocalPlayerID()) {
		return m_localStatus == NETLOCALSTATUS_INGAME || m_localStatus == NETLOCALSTATUS_LEAVING;
	}
	return m_conMgr->isPlayerConnected(playerID);
}


// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

/**
 * This creates a network object and returns it.
 */
NetworkInterface *NetworkInterface::createNetwork()
{
	return NEW Network;
}

////////////////////////////////////////////////////////////////////////////////

/**
 * The constructor.
 */
Network::Network()
{
	m_checkCRCsThisFrame = FALSE;
	m_didSelfSlug = FALSE;
	m_frameDataReady = FALSE;
	m_isStalling = FALSE;
	m_sawCRCMismatch = FALSE;
	m_conMgr = nullptr;
	m_messageWindow = nullptr;

#if defined(RTS_DEBUG)
	m_networkOn = TRUE;
#endif
}

/**
 * The destructor.
 */
Network::~Network()
{
	deinit();
}

/**
 * This basically releases all the memory.
 */
Bool Network::deinit()
{
	if (m_conMgr)
	{
		m_conMgr->destroyGameMessages();
		delete m_conMgr;
		m_conMgr = nullptr;
	}
	if (m_messageWindow) {
		TheWindowManager->winDestroy(m_messageWindow);
		m_messageWindow = nullptr;
	}

	return true;
}

/**
 * Takes the network back to the initial state.
 */
void Network::reset() {
	init();
}

/**
 * Initializes all the network subsystems.
 */
void Network::init()
{
	if (!deinit())
	{
		DEBUG_LOG(("Could not deinit network prior to init!"));
		return;
	}

	m_conMgr = NEW ConnectionManager;
	m_conMgr->init();

	m_lastFrame = 0;
	// GeneralsX: Both were raw 30s. The run-ahead is a LATENCY BUFFER measured
	// in frames, so a fixed frame count means a HALVED wall-clock buffer at 60 Hz (1.00 s -> 0.50 s)
	// - i.e. half the tolerance for a late peer before the whole lobby stalls. Expressed against the
	// tick rate it stays 1.00 s of real time at either rate. Identical at the default 30 Hz.
	m_runAhead = min(max((Int)LOGICFRAMES_PER_SECOND, MIN_RUNAHEAD), MAX_FRAMES_AHEAD/2); ///< @todo: don't hard-code the run-ahead.
	m_frameRate = LOGICFRAMES_PER_SECOND;
	m_lastExecutionFrame = m_runAhead - 1; // subtract 1 since we're starting on frame 0
	m_lastFrameCompleted = m_runAhead - 1; // subtract 1 since we're starting on frame 0
	m_frameDataReady = FALSE;
	m_isStalling = FALSE;
	m_didSelfSlug = FALSE;

	m_localStatus = NETLOCALSTATUS_PREGAME;

	// GeneralsX: Align Linux network timing with nanosecond clock frequency
	#ifdef _WIN32
	QueryPerformanceFrequency((LARGE_INTEGER *)&m_perfCountFreq);
	#else
	m_perfCountFreq = 1000000000; // Linux CLOCK_MONOTONIC uses nanoseconds
	#endif
	m_nextFrameTime = 0;
	m_lastFrameDelay = 0;
	m_sawCRCMismatch = FALSE;
	m_checkCRCsThisFrame = FALSE;

	DEBUG_LOG(("Network timing values:"));
	DEBUG_LOG(("NetworkFPSHistoryLength: %d", TheGlobalData->m_networkFPSHistoryLength));
	DEBUG_LOG(("NetworkLatencyHistoryLength: %d", TheGlobalData->m_networkLatencyHistoryLength));
	DEBUG_LOG(("NetworkRunAheadMetricsTime: %d", TheGlobalData->m_networkRunAheadMetricsTime));
	DEBUG_LOG(("NetworkCushionHistoryLength: %d", TheGlobalData->m_networkCushionHistoryLength));
	DEBUG_LOG(("NetworkRunAheadSlack: %d", TheGlobalData->m_networkRunAheadSlack));
	DEBUG_LOG(("NetworkKeepAliveDelay: %d", TheGlobalData->m_networkKeepAliveDelay));
	DEBUG_LOG(("NetworkDisconnectTime: %d", TheGlobalData->m_networkDisconnectTime));
	DEBUG_LOG(("NetworkPlayerTimeoutTime: %d", TheGlobalData->m_networkPlayerTimeoutTime));
	DEBUG_LOG(("NetworkDisconnectScreenNotifyTime: %d", TheGlobalData->m_networkDisconnectScreenNotifyTime));
	DEBUG_LOG(("Other network stuff:"));
	DEBUG_LOG(("FRAME_DATA_LENGTH = %d", FRAME_DATA_LENGTH));
	DEBUG_LOG(("FRAMES_TO_KEEP = %d", FRAMES_TO_KEEP));


#if defined(RTS_DEBUG)
	m_networkOn = TRUE;
#endif
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: NAME THE PEERS A DESYNC HAPPENED TO. Retail's whole answer to a
// CRC mismatch is Menus/CRCMismatch.wnd, which names nobody, while the CRC table that answers "who?"
// is alive at this call site. GeneralsX: The report and its window moved to
// GameClient/DesyncReport.{h,cpp} - a table with a column per field, outside the SimID digest, so its
// wording and layout can change without refusing every peer that has not updated.

// The divergence happened runAhead frames before it was noticed, but that is unsigned arithmetic
// on a frame counter: the first CRC is generated at frame 0 and validated ~runAhead frames later,
// so an immediate data mismatch would otherwise put "the split began around frame -1" on screen.
// Clamped, because a negative frame is not a fact about the match.
static Int gxDivergedFrame( UnsignedInt noticedFrame, Int runAhead )
{
	const Int back = runAhead + 1;
	return ( (Int)noticedFrame > back ) ? ( (Int)noticedFrame - back ) : 0;
}

void Network::setSawCRCMismatch(const std::map<Int, UnsignedInt> &playerCRCs, Int numPlayers)
{
	m_sawCRCMismatch = TRUE;

	TheScriptActions->closeWindows( TRUE );
	// numPlayers is not passed on: the report counts the rows it lists, so its header cannot disagree
	// with its own table (DesyncReport.h).
	(void)numPlayers;
	m_messageWindow = DesyncReportShow( playerCRCs, (Int)TheGameLogic->getFrame(),
		gxDivergedFrame( TheGameLogic->getFrame(), m_runAhead ) );
	TheScriptEngine->startEndGameTimer();

	TheRecorder->logCRCMismatch();

	// GeneralsX: Release-visible. Everything below this point is
	// DEBUG_LOG or #ifdef DEBUG_CRC and vanishes in a shipping build, so the user-visible
	// behaviour was a CRCMismatch.wnd popup followed by exitGame() 120 frames later with nothing
	// written anywhere. m_runAhead matters: the divergence happened that many frames BEFORE the
	// frame we noticed it on, so the raw GameLogic frame alone points at the wrong place.
	fprintf(stderr, "[CRC] DESYNC CONFIRMED - divergence frame~=%d noticedAtFrame=%d runAhead=%d seedCRC=%d\n",
		(int)(TheGameLogic->getFrame() - m_runAhead - 1), (int)TheGameLogic->getFrame(),
		(int)m_runAhead, (int)GetGameLogicRandomSeedCRC());
	fflush(stderr);

	// dump GameLogic random seed
	DEBUG_LOG(("Latest frame for mismatch = %d GameLogic frame = %d",
		TheGameLogic->getFrame()-m_runAhead-1, TheGameLogic->getFrame()));
	DEBUG_LOG(("GetGameLogicRandomSeedCRC() = %d", GetGameLogicRandomSeedCRC()));

	// dump CRCs
	{
		DEBUG_LOG(("--- GameState Dump ---"));
#ifdef DEBUG_CRC
		outputCRCDumpLines();
#endif
		DEBUG_LOG(("------ End Dump ------"));
	}
	{
		DEBUG_LOG(("--- DebugInfo Dump ---"));
#ifdef DEBUG_CRC
		outputCRCDebugLines();
#endif
		DEBUG_LOG(("------ End Dump ------"));
	}
}

/**
 * Take a user list and build the connection queues and player lists and stuff like that.
 */
void Network::parseUserList( const GameInfo *game )
{
	if (!game)
	{
		DEBUG_LOG(("FAILED parseUserList with a null game"));
		return;
	}

	m_conMgr->parseUserList(game);

	// Now that we have the players in this game, we need to reset the FrameData stuff.
	m_conMgr->destroyGameMessages();
	m_conMgr->zeroFrames(1, m_runAhead-1); ///< we zero out m_runAhead frames +1 because the game actually starts at frame 1.
}

/**
 * Guess what, we're starting a game!
 */
void Network::startGame() {
}

/**
 * Tell the network which ip address the user has chosen to use.  Well ok, they probably didn't choose
 * it explicitly, but regardless, this is the one we're going to use.
 */
void Network::setLocalAddress(UnsignedInt ip, UnsignedInt port) {
	DEBUG_ASSERTCRASH(m_conMgr != nullptr, ("Connection manager does not exist."));
	if (m_conMgr != nullptr) {
		m_conMgr->setLocalAddress(ip, port);
	}
}

/**
 * Tell the network to initialize the transport object
 */
void Network::initTransport() {
	DEBUG_ASSERTCRASH(m_conMgr != nullptr, ("Connection manager does not exist."));
	if (m_conMgr != nullptr) {
		m_conMgr->initTransport();
	}
}

void Network::attachTransport(Transport *transport) {
	DEBUG_ASSERTCRASH(m_conMgr != nullptr, ("Connection manager does not exist."));
	if (m_conMgr != nullptr) {
		m_conMgr->attachTransport(transport);
	}
}

Bool Network::isMessageTypeWithinNetworkRange(GameMessage::Type type) {
	return type > GameMessage::MSG_BEGIN_NETWORK_MESSAGES && type < GameMessage::MSG_END_NETWORK_MESSAGES;
}

/**
 * Take commands from TheCommandList and give them to the connection manager for transport.
 */
// GeneralsX: SIGN THIS PEER'S CHEAT ORDER when it is an admin of a match with more than one
// human (Common/GXAdminOrder.h). Here because every order this peer sends leaves through this one loop - the Debug
// menu's and GX_TEST_FORGE's alike - and because the signed copy is what the network re-delivers to this peer too, so
// every simulation checks the same bytes. The one-human sandbox stays unsigned, as before; a peer that is no admin, or
// holds no match key, sends the order as it is and every simulation refuses it.
static void gxSealAdminOrder( GameMessage *msg, Int localSlot )
{
	if( TheGameLogic == nullptr || GameLogic::gxCheatsAllowed() || localSlot < 0 || localSlot >= MAX_SLOTS
			|| ( TheGameLogic->getGxAdminSlotMask() & ( gxSlotBit(localSlot) ) ) == 0
			|| gxAdminLogicalArgCount( msg ) != (Int)msg->getArgumentCount() )
		return;
	UnsignedInt seq = 0;
	if( !gxAdminKeyNextSeq( &seq ) )
		return;
	GXAdminPayload payload;
	gxAdminPayloadOf( msg, localSlot, seq, TheGameLogic->getGxAdminSeed(), &payload );
	unsigned char sig[64];
	if( !gxAdminKeySign( &payload, sig ) )
		return;
#if defined(GX_TEST_INSTRUMENTS)
	// GX_TEST_ADMIN_BADSIG=1 - a TEST INSTRUMENT: this admin's orders go out with one signature bit flipped, which is
	// what any peer without the key can at best send. scripts/test/net-kill-gates.sh admin-badsig.
	static const Bool s_badSig = getenv( "GX_TEST_ADMIN_BADSIG" ) != nullptr;
	if( s_badSig )
		sig[37] ^= 0x04;
#endif
	gxAdminAppendTrailer( msg, seq, sig );
	fprintf( stderr, "[GXADMIN] signed cheat %d as slot %d, seq %u\n", (Int)msg->getType(), (int)localSlot, (unsigned)seq );
	fflush( stderr );
}

void Network::GetCommandsFromCommandList() {
	GameMessage *msg = TheCommandList->getFirstMessage();
	GameMessage *next = nullptr;
	while (msg != nullptr) {
		next = msg->next();
		if (isMessageTypeWithinNetworkRange(msg->getType())) { // Is this something we should be sending to the other players?
			if (m_localStatus == NETLOCALSTATUS_INGAME) {
				if (msg->getType() >= GameMessage::MSG_GX_CHEAT_GIVE_MONEY && msg->getType() <= GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS)
					gxSealAdminOrder(msg, (Int)m_conMgr->getLocalPlayerID());
				m_conMgr->sendLocalGameMessage(msg, getExecutionFrame());
			}
			TheCommandList->removeMessage(msg); // This does not destroy msg's prev and next pointers, so they should still be valid.
			deleteInstance(msg);
		} else {
			// GeneralsX: The frame tick is eaten here, once processCommand has seen it.
			// Past the translators the network is its only reader (GameClientMessageDispatcher keeps it for us; the
			// logic and the recorder ignore it), yet it would stay on the list until the logic tick's reset. Every main-
			// loop pass appends one, so a -headless peer spinning between network frames piles up ~2,200 per tick and
			// re-walks them all on every pass: O(n^2) between ticks, and ~45 us inside each tick for the logic's walk
			// and the reset. processCommand acts on the logic frame and the network status
			// alone, so a stale tick only ever repeated a call that does nothing. The one action whose place in the
			// list matters - PREGAME -> INGAME, which decides whether the network messages ahead of it are sent - needs
			// frame 1, and the logic reaches frame 1 only in a tick that has just reset the list.
			if (processCommand(msg) || msg->getType() == GameMessage::MSG_FRAME_TICK) {
				TheCommandList->removeMessage(msg);
				deleteInstance(msg);
			}
		}
		msg = next;
	}
}

Int Network::getExecutionFrame() {
	Int logicFrame = TheGameLogic->getFrame() + m_runAhead;
	if (logicFrame > m_lastExecutionFrame) {
		m_lastExecutionFrame = logicFrame;
		return (logicFrame);
	}
	return m_lastExecutionFrame;
}

/**
 * This is where any processing that needs to be done for specific game messages.
 * Also check here to see if the logic frame number has changed to see if we need to
 * send our info for the last frame to the other players.
 * Return true if the message should be "eaten" by the network.
 */
Bool Network::processCommand(GameMessage *msg)
{
	if ((m_lastFrame != TheGameLogic->getFrame()) || (m_localStatus == NETLOCALSTATUS_PREGAME)) {
		// If this is the start of a new game logic frame, then tell the connection manager that the last
		// frame is over and that it should now produce a frame info packet for the other players.

		if (m_localStatus == NETLOCALSTATUS_PREGAME) {
			// a sort-of-hack that prevents extraneous frames from being executed before the game actually starts.
			// Idealy this shouldn't be necessary, but I don't think its hurting anything by being here.
			if (TheGameLogic->getFrame() == 1) {
				m_localStatus = NETLOCALSTATUS_INGAME;
				NetCommandList *netcmdlist = m_conMgr->getFrameCommandList(0); // clear out frame 0 since we skipped it
				deleteInstance(netcmdlist);
			} else {
				return FALSE;
			}
		}

		// Only send frame info packets for frames where we are actually going to be in the game.
		// The reason is so we don't have to wait for frame info packets to be sent that aren't going to
		// matter anyways when we actually try to leave the game ourselves.
		if (m_localStatus == NETLOCALSTATUS_INGAME) {
			Int executionFrame = getExecutionFrame();

			// Send command counts for all the frames we can.
			for (Int i = m_lastFrameCompleted + 1; i < executionFrame; ++i) {
				m_conMgr->processFrameTick(i);
				//DEBUG_LOG(("Network::processCommand - calling processFrameTick for frame %d", i));
				m_lastFrameCompleted = i;
			}
		}

		//DEBUG_LOG(("Next Execution Frame - %d, last frame completed - %d", getExecutionFrame(), m_lastFrameCompleted));
		m_lastFrame = TheGameLogic->getFrame();
	}

	// Are we leaving the game?
	// This has to happen after the check to see if this is the start of a new logic frame.
	// The reason is that we have to send all the frame info packets necessary to get to the
	// frame where everyone else is going to see that we left.
	if ((msg->getType() == GameMessage::MSG_CLEAR_GAME_DATA) && (m_localStatus == NETLOCALSTATUS_INGAME)) {
		Int executionFrame = getExecutionFrame();
		DEBUG_LOG(("Network::processCommand - local player leaving, executionFrame = %d, player leaving on frame %d", executionFrame, executionFrame+1));

		m_conMgr->handleLocalPlayerLeaving(executionFrame+1);
		m_conMgr->processFrameTick(executionFrame); // This is the last command we will execute, so send the command count.
																								// Also, we are guaranteed not to send any more commands for this frame
																								// since the local status will change to "Leaving" so we don't have to
																								// worry about messing up the other players.
		m_conMgr->processFrameTick(executionFrame+1); // since we send it for executionFrame+1, we need to process both ticks
		m_lastFrameCompleted = executionFrame;
		DEBUG_LOG(("Network::processCommand - player leaving on frame %d", executionFrame));
		m_localStatus = NETLOCALSTATUS_LEAVING;
		return TRUE;
	}
	return FALSE;
}

/**
 * returns true if all the commands are ready for the given frame.
 */
Bool Network::AllCommandsReady(UnsignedInt frame) {
	if (m_conMgr == nullptr) {
		return TRUE;
	}

	if (m_localStatus == NETLOCALSTATUS_PREGAME) {
		return TRUE;
	}

	if (m_localStatus == NETLOCALSTATUS_POSTGAME) {
		return TRUE;
	}

	return m_conMgr->allCommandsReady(frame);// && m_conMgr->allCRCsReady(frame);
}

/**
 * Take commands from the connection manager and put them on TheCommandList.
 * The commands need to be put on in the same order across all clients.
 */
void Network::RelayCommandsToCommandList(UnsignedInt frame) {
	if ((m_conMgr == nullptr) || (m_localStatus == NETLOCALSTATUS_PREGAME)) {
		return;
	}
	m_checkCRCsThisFrame = FALSE;
	NetCommandList *netcmdlist = m_conMgr->getFrameCommandList(frame);
	NetCommandRef *msg = netcmdlist->getFirstMessage();
	while (msg != nullptr) {
		NetCommandType cmdType = msg->getCommand()->getNetCommandType();
		if (cmdType == NETCOMMANDTYPE_GAMECOMMAND) {
			NetGameCommandMsg* gmsg = static_cast<NetGameCommandMsg*>(msg->getCommand());
			// GeneralsX: The upstream check is gated on RETAIL_COMPATIBLE_CRC, which defaults
			// to 1 (GameDefines.h:91) and is overridden by no preset, so upstream ships it compiled out.
			// We run it unconditionally, and it cannot cost us a legitimate message: the SENDING side
			// filters on this exact predicate in GetCommandsFromCommandList, so no conforming peer ever
			// puts an out-of-range type on the wire. Only malformed or hostile input can fail it - and
			// through a relay, anyone who can reach the relay can supply that.
			// TheSuperHackers @fix stephanmeesters 14/05/2026 Verify accepted type of incoming game messages
			if (isMessageTypeWithinNetworkRange(gmsg->getGameMessageType())) {
				TheCommandList->appendMessage(gmsg->constructGameMessage());
			} else {
				DEBUG_LOG(("Network::RelayCommandsToCommandList - rejecting game message from player %d of type %s, which is not a network type.",
					gmsg->getPlayerID(), GameMessage::getCommandTypeAsString(gmsg->getGameMessageType())));
			}
		} else {
			processFrameSynchronizedNetCommand(msg);
		}
		msg = msg->getNext();
	}

	for (std::list<Int>::iterator selfDestructIt = m_playersToDisconnect.begin(); selfDestructIt != m_playersToDisconnect.end(); ++selfDestructIt)
	{
		//Int playerToDisconnect = *selfDestructIt;
		//GameMessage *msg = newInstance(GameMessage)( GameMessage::MSG_SELF_DESTRUCT );
		//msg->friend_setPlayerIndex(playerToDisconnect);
		//msg->appendBooleanArgument(TRUE);
		//TheCommandList->appendMessage(msg);
	}
	m_playersToDisconnect.clear();

	deleteInstance(netcmdlist);
}

/**
 * This is where network commands that need to be executed on the same frame should be executed.
 */
void Network::processFrameSynchronizedNetCommand(NetCommandRef *msg) {
	NetCommandMsg *cmdMsg = msg->getCommand();
	if (cmdMsg->getNetCommandType() == NETCOMMANDTYPE_PLAYERLEAVE) {
		PlayerLeaveCode retval = m_conMgr->processPlayerLeave((NetPlayerLeaveCommandMsg *)cmdMsg);
		if (retval == PLAYERLEAVECODE_LOCAL) {
			DEBUG_LOG(("Network::processFrameSynchronizedNetCommand - Local player left the game on frame %d.", TheGameLogic->getFrame()));
			m_localStatus = NETLOCALSTATUS_LEFT;
		} else if (retval == PLAYERLEAVECODE_PACKETROUTER) {
			DEBUG_LOG(("Network::processFrameSynchronizedNetCommand - Packet router left the game on frame %d", TheGameLogic->getFrame()));
		} else {
			DEBUG_LOG(("Network::processFrameSynchronizedNetCommand - Client left the game on frame %d", TheGameLogic->getFrame()));
		}
	}
	else if (cmdMsg->getNetCommandType() == NETCOMMANDTYPE_RUNAHEAD) {
		NetRunAheadCommandMsg *netmsg = (NetRunAheadCommandMsg *)cmdMsg;
		processRunAheadCommand(netmsg);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("command to set run ahead to %d and frame rate to %d on frame %d actually executed on frame %d", netmsg->getRunAhead(), netmsg->getFrameRate(), netmsg->getExecutionFrame(), TheGameLogic->getFrame()));
	}
	else if (cmdMsg->getNetCommandType() == NETCOMMANDTYPE_DESTROYPLAYER) {
		NetDestroyPlayerCommandMsg *netmsg = (NetDestroyPlayerCommandMsg *)cmdMsg;
		processDestroyPlayerCommand(netmsg);
	}
}

void Network::processRunAheadCommand(NetRunAheadCommandMsg *msg) {
	// GeneralsX: Same consumer-side invariant as m_frameRate below, for the
	// run-ahead. The value is a raw wire UnsignedShort. 0 deadlocks the whole lobby: getExecutionFrame()
	// stops advancing, processCommand's `i < executionFrame` loop never sends FRAMEINFO(G), and the
	// router's corrective RUNAHEAD would itself execute on G. 65535 queues ~65k ack-required FRAMEINFOs
	// in one processCommand and pushes m_lastExecutionFrame past the FRAME_DATA_LENGTH ring. The
	// producer (ConnectionManager::updateRunAhead) clamps to [MIN_RUNAHEAD, MAX_FRAMES_AHEAD / 2] and
	// release MIN_RUNAHEAD is 4, so this is a no-op for every value a router sends. The floor is a
	// constant 1, not MIN_RUNAHEAD, so every peer maps a bad value identically.
	m_runAhead = clamp<Int>(1, (Int)msg->getRunAhead(), MAX_FRAMES_AHEAD / 2);

	// Clamp where the value is used, not only in ConnectionManager::updateRunAhead (which only the packet router runs).
	// m_frameRate divides here and in timeForNewFrame(); 0 from a truncated or hostile packet would divide by zero.
	// Not simulation-visible, so no desync. Update() checks timeForNewFrame() before relaying, so a suppressed frame
	// never had commands appended; throttling in canUpdateNetworkGameLogic() instead would strand them.
	m_frameRate = min(max((Int)MIN_LOGIC_FRAMES, (Int)msg->getFrameRate()), (Int)LOGICFRAMES_PER_SECOND);

	time_t frameGrouping = (1000 * m_runAhead) / m_frameRate; // number of miliseconds between packet sends
	// GX_GROUPING_DIV: experimental divisor override, default 2. Frame grouping defers sends until m_frameGrouping ms
	// have passed (Connection::doSend, no urgency bypass), so a peer receives input in bursts. Changes only when a
	// packet is sent, never what is simulated, and is not negotiated. Not a shipping knob.
	static const Int s_groupingDiv = (getenv("GX_GROUPING_DIV") != NULL)
	                                 ? max(1, atoi(getenv("GX_GROUPING_DIV")))
	                                 : 2;
	frameGrouping = frameGrouping / s_groupingDiv; // since we only want the latency for one way to be a factor.
//	DEBUG_LOG(("Network::processRunAheadCommand - trying to set frame grouping to %d.  run ahead = %d, m_frameRate = %d", frameGrouping, m_runAhead, m_frameRate));
	if (frameGrouping < 1) {
		frameGrouping = 1; // Having a value less than 1 doesn't make sense.
	}

	// Cap the grouping deferral at two logic frames, in frames so it scales with the tick rate. Uncapped, every stall
	// was an input datagram not yet sent. Changes only when a packet leaves, not what is simulated; not negotiated.
	if (m_frameRate > 0) {
		const time_t groupingCap = max((time_t)1, (time_t)((2 * 1000) / m_frameRate));
		if (frameGrouping > groupingCap) {
			frameGrouping = groupingCap;
		}
	}

	if (frameGrouping > 500) {
		frameGrouping = 500; // Max of a half a second.
	}
	m_conMgr->setFrameGrouping(frameGrouping);
}

void Network::processDestroyPlayerCommand(NetDestroyPlayerCommandMsg *msg)
{
	const Bool quitterTransfer = (msg->getPlayerIndex() & DESTROYPLAYER_TRANSFER_ASSETS) != 0;	// a hard quit - see its definition
	UnsignedInt playerIndex = msg->getPlayerIndex() & ~(UnsignedInt)DESTROYPLAYER_TRANSFER_ASSETS;
	DEBUG_ASSERTCRASH(playerIndex < MAX_SLOTS, ("Bad player index"));
	if (playerIndex >= MAX_SLOTS)
		return;

	// DESTROYPLAYER is accepted only from ConnectionManager::lockstepRouter(victim): the first slot of the elected router
	// succession that no executed leave or kill has removed. The decision reads only the executed order stream, so every
	// peer answers identically; connection state would split the match. A refused order is still consumed.
	// Limit: the router, or its successor when the router is the victim, can still defeat anyone.
	const UnsignedInt sender = msg->getPlayerID();
	const UnsignedInt entitled = m_conMgr->lockstepRouter(playerIndex);
	if (sender != entitled)
	{
		static Int s_refusals = 0;	// Trade-off: capped so a flood of forgeries cannot turn into a flood of fflush
		if (s_refusals < 32)
		{
			++s_refusals;
			fprintf(stderr, "[GXGATE] DESTROYPLAYER refused: slot %u named slot %u, only slot %u may (frame %u)\n",
				(unsigned)sender, (unsigned)playerIndex, (unsigned)entitled, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
		return;
	}
	m_conMgr->lockstepRemove(playerIndex);

	Player *pPlayer = ThePlayerList->getPlayerFromSlotIndex(playerIndex);

	// GeneralsX: An observer slot is not a Player of its own: every one maps onto
	// the ONE ReplayObserver (PlayerList::assignSlotIndices), so self-destructing it would take out the Player
	// the other observers still are. The slot has already left the succession above, and
	// the disconnect path drops its connection - that IS the observer leaving - so nothing is queued. The
	// last observer leaving takes this branch too, which changes nothing it did: the ReplayObserver is dead
	// from its creation (Player::init), so the already-dead check below refused it. The input is the slot
	// map fixed at the start, so every peer takes this branch alike; every player, and an observer of a
	// legacy replay or save, owns its slot and falls through unchanged.
	if (pPlayer != nullptr && ThePlayerList->isSharedBySlots(pPlayer->getPlayerIndex()))
	{
		static Int s_kept = 0;	// Trade-off: capped like the refusals above
		if (s_kept < 32)
		{
			++s_kept;
			fprintf(stderr, "[GXGATE] DESTROYPLAYER kept: slot %u named slot %u, which shares its Player - the slot leaves, the Player stays (frame %u)\n",
				(unsigned)sender, (unsigned)playerIndex, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
		return;
	}

	// Idempotent destruct: onSelfDestruct has no dead check and killPlayer walks every team and object, so a duplicate
	// order (voted out after already quitting through the menu) must be a no-op, identically on every peer.
	// Mirrors onGxCheatKillPlayer, which refuses observers and dead players.
	if (pPlayer != nullptr && pPlayer->isPlayerDead())
	{
		DEBUG_LOG(("Network::processDestroyPlayerCommand - player %d already dead on frame %d, ignoring",
			playerIndex, TheGameLogic->getFrame()));
		return;
	}

	if (pPlayer)
	{
		GameMessage *msg = newInstance(GameMessage)(GameMessage::MSG_SELF_DESTRUCT);
#if RETAIL_COMPATIBLE_CRC
		const Bool transferAssets = quitterTransfer;
#else
		const Bool transferAssets = TRUE;
#endif
		msg->appendBooleanArgument(transferAssets);
		msg->friend_setPlayerIndex(pPlayer->getPlayerIndex());
		TheCommandList->appendMessage(msg);

		// Once per player at most: the already-dead return above stops any repeat.
		fprintf(stderr, "[GXGATE] DESTROYPLAYER accepted: slot %u named slot %u (frame %u)%s\n",
			(unsigned)sender, (unsigned)playerIndex, (unsigned)TheGameLogic->getFrame(), quitterTransfer ? " transfer" : "");
		fflush(stderr);
	}

	DEBUG_LOG(("Saw DestroyPlayer from %d about %d for frame %d on frame %d", msg->getPlayerID(), msg->getPlayerIndex(),
		msg->getExecutionFrame(), TheGameLogic->getFrame()));
}

#if defined(GX_TEST_INSTRUMENTS)
// GX_TEST_FORGE: test instrument; makes this peer send one kill-path order at a chosen logic frame, as kind:slot@frame
// (destroy, leave, kill, eject, ejectasrouter, vote, ejectdestroy, timeout, silence) or quit@frame, hardquit@frame.
// A kind may end in +ms (fire after the frame stood still that long) or *ms (repeat while it has not moved).
// Only with -DGX_TEST_INSTRUMENTS=ON, never in a shipping build; fires once per process.
void Network::gxTestForge()
{
	static Int s_state = -1;	// -1 unparsed, 0 off or spent, 1 armed
	static char s_kind[16];
	static UnsignedInt s_target = 0;
	static UnsignedInt s_frame = 0;
	static Int s_stage2 = 0;	// ejectdestroy: 1 while the issuer's own teardown is still owed
	static UnsignedInt s_stage2Frame = 0;
	static UnsignedInt s_stallMs = 0;	// the +<ms> suffix
	static UnsignedInt s_seenFrame = 0;
	static UnsignedInt s_seenAt = 0;	// when the logic frame last moved
	static UnsignedInt s_repeatMs = 0;	// the *<ms> suffix
	static UnsignedInt s_nextAt = 0;	// with it: when the next repeat is due
	static UnsignedInt s_firedFrame = 0;	// and the frame the first send stood on
	const UnsignedInt now = timeGetTime();
	if (TheGameLogic->getFrame() != s_seenFrame)
	{
		s_seenFrame = TheGameLogic->getFrame();
		s_seenAt = now;
	}
	if (s_stage2 == 1 && TheGameLogic->getFrame() >= s_stage2Frame)
	{
		s_stage2 = 0;
		m_conMgr->disconnectPlayer((Int)s_target);
		fprintf(stderr, "[GXFORGE] ejectdestroy: issuer dropped slot %u at frame %u\n",
			s_target, (unsigned)TheGameLogic->getFrame());
		fflush(stderr);
	}
	if (s_state < 0)
	{
		s_state = 0;
		const char *env = getenv("GX_TEST_FORGE");
		if (env != nullptr)
		{
			unsigned target = 0, frame = 0, stall = 0, repeat = 0;
			if (sscanf(env, "%15[a-z]:%u@%u+%u*%u", s_kind, &target, &frame, &stall, &repeat) >= 3
				|| (sscanf(env, "%15[a-z]@%u+%u", s_kind, &frame, &stall) >= 2 && (strcmp(s_kind, "quit") == 0 || strcmp(s_kind, "hardquit") == 0)))
			{
				s_target = target;
				s_frame = frame;
				s_stallMs = stall;
				s_repeatMs = (stall != 0) ? repeat : 0;
				s_state = 1;
				fprintf(stderr, "[GXFORGE] armed: %s target=%u at frame %u, stalled %u ms, repeat %u ms\n", s_kind, s_target, s_frame, s_stallMs, s_repeatMs);
			}
			else
			{
				fprintf(stderr, "[GXFORGE] ignoring unparseable GX_TEST_FORGE='%s'\n", env);
			}
			fflush(stderr);
		}
	}
	if (s_state == 1 && s_nextAt != 0 && TheGameLogic->getFrame() != s_firedFrame)
		s_state = 0;	// repeating, and the frame has moved: the stall it was aimed at is over
	if (s_state != 1 || m_localStatus != NETLOCALSTATUS_INGAME || TheGameLogic->getFrame() < s_frame
		|| (now - s_seenAt) < s_stallMs || (s_nextAt != 0 && (Int)(now - s_nextAt) < 0))
		return;
	if (s_repeatMs != 0)
	{
		s_firedFrame = TheGameLogic->getFrame();
		s_nextAt = now + s_repeatMs;
	}
	else
	{
		s_state = 0;
	}

	const UnsignedInt local = m_conMgr->getLocalPlayerID();
	if (strcmp(s_kind, "destroy") == 0)
	{
		NetDestroyPlayerCommandMsg *netmsg = newInstance(NetDestroyPlayerCommandMsg);
		netmsg->setExecutionFrame(getExecutionFrame() + 1);
		netmsg->setPlayerID(local);
		netmsg->setID(GenerateNextCommandID());
		netmsg->setPlayerIndex(s_target);
		m_conMgr->sendLocalCommand(netmsg);
		netmsg->detach();
	}
	else if (strcmp(s_kind, "leave") == 0)
	{
		NetPlayerLeaveCommandMsg *netmsg = newInstance(NetPlayerLeaveCommandMsg);
		netmsg->setLeavingPlayerID((UnsignedByte)s_target);
		netmsg->setExecutionFrame(getExecutionFrame() + 1);
		netmsg->setID(GenerateNextCommandID());
		netmsg->setPlayerID(local);
		m_conMgr->sendLocalCommand(netmsg);
		netmsg->detach();
	}
	else if (strcmp(s_kind, "kill") == 0)
	{
		// The cheat carries a player INDEX; resolve the slot's player the way
		// processDestroyPlayerCommand does, so the harness can name slots on any map.
		AsciiString playerName;
		playerName.format("player%u", s_target);
		const Player *victim = ThePlayerList->findPlayerWithNameKey(NAMEKEY(playerName));
		if (victim == nullptr)
		{
			fprintf(stderr, "[GXFORGE] no player in slot %u, nothing sent\n", s_target);
			fflush(stderr);
			return;
		}
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_CHEAT_KILL_PLAYER);
		msg->appendIntegerArgument(victim->getPlayerIndex());
		msg->appendBooleanArgument(FALSE);
	}
	else if (strcmp(s_kind, "hardquit") == 0)
	{
		quitGame();
	}
	else if (strcmp(s_kind, "quit") == 0)
	{
		TheGameLogic->exitGame();
	}
	else if (strcmp(s_kind, "timeout") == 0)
	{
		TheWritableGlobalData->m_networkPlayerTimeoutTime = s_target;
	}
	else if (strcmp(s_kind, "silence") == 0)
	{
		DisconnectManager::setSilenceTimeout((time_t)s_target);
	}
	else if (strcmp(s_kind, "vote") == 0)
	{
		NetDisconnectVoteCommandMsg *netmsg = newInstance(NetDisconnectVoteCommandMsg);
		netmsg->setPlayerID(local);
		netmsg->setSlot((UnsignedByte)s_target);
		netmsg->setVoteFrame(TheGameLogic->getFrame());
		if (DoesCommandRequireACommandID(netmsg->getNetCommandType())) {
			netmsg->setID(GenerateNextCommandID());
		}
		m_conMgr->sendLocalCommandDirect(netmsg, ALL_SLOTS_MASK & ~(gxSlotBit(local)));
		netmsg->detach();
	}
	else if (strcmp(s_kind, "ejectdestroy") == 0)
	{
		NetDisconnectPlayerCommandMsg *ejectmsg = newInstance(NetDisconnectPlayerCommandMsg);
		ejectmsg->setDisconnectSlot((UnsignedByte)s_target);
		ejectmsg->setDisconnectFrame(TheGameLogic->getFrame());
		ejectmsg->setPlayerID(local);
		if (DoesCommandRequireACommandID(ejectmsg->getNetCommandType())) {
			ejectmsg->setID(GenerateNextCommandID());
		}
		m_conMgr->sendLocalCommand(ejectmsg);
		ejectmsg->detach();

		NetDestroyPlayerCommandMsg *netmsg = newInstance(NetDestroyPlayerCommandMsg);
		netmsg->setExecutionFrame(getExecutionFrame() + 1);
		netmsg->setPlayerID(local);
		netmsg->setID(GenerateNextCommandID());
		netmsg->setPlayerIndex(s_target);
		m_conMgr->sendLocalCommand(netmsg);
		netmsg->detach();

		// Issuer teardown is deferred until the DESTROYPLAYER has executed here (the only difference from
		// updateDisconnectStatus). Both orders are still queued on the victim's Connection when the real path deletes it,
		// which drops its copy unsent. The issuer keeps relaying, so every peer has frame data for getExecutionFrame() + 1.
		s_stage2Frame = (UnsignedInt)getExecutionFrame() + 5;
		s_stage2 = 1;
	}
	else if (strcmp(s_kind, "eject") == 0 || strcmp(s_kind, "ejectasrouter") == 0)
	{
		NetDisconnectPlayerCommandMsg *netmsg = newInstance(NetDisconnectPlayerCommandMsg);
		netmsg->setDisconnectSlot((UnsignedByte)s_target);
		netmsg->setDisconnectFrame(TheGameLogic->getFrame());
		netmsg->setPlayerID(strcmp(s_kind, "eject") == 0 ? local : m_conMgr->getPacketRouterSlot());
		if (DoesCommandRequireACommandID(netmsg->getNetCommandType())) {
			netmsg->setID(GenerateNextCommandID());
		}
		m_conMgr->sendLocalCommand(netmsg);
		netmsg->detach();
	}
	else
	{
		fprintf(stderr, "[GXFORGE] unknown kind '%s', nothing sent\n", s_kind);
		fflush(stderr);
		return;
	}
	fprintf(stderr, "[GXFORGE] sent %s target=%u from slot %u at frame %u\n",
		s_kind, s_target, local, TheGameLogic->getFrame());
	fflush(stderr);
}

// GeneralsX: GX_NET_TEST_WRAPFLOOD=<n>@<frame> - a TEST INSTRUMENT (plan Task 9): at logic frame <frame>, in
// game, queue <n> forged wrapper chunks on every connection, ignoring the send window (ConnectionManager::gxTestWrapFlood). Once
// per process. Compiled in only with -DGX_TEST_INSTRUMENTS=ON.
void Network::gxTestWrapFlood()
{
	static Int s_state = -1;	// -1 unparsed, 0 off or spent, 1 armed
	static UnsignedInt s_count = 0;
	static UnsignedInt s_frame = 0;
	if (s_state < 0)
	{
		s_state = 0;
		const char *env = getenv("GX_NET_TEST_WRAPFLOOD");
		unsigned count = 0, frame = 0;
		if (env != nullptr && sscanf(env, "%u@%u", &count, &frame) == 2 && count > 0)
		{
			s_count = count;
			s_frame = frame;
			s_state = 1;
			fprintf(stderr, "[GXWRAP] flood armed: %u chunks per connection at frame %u\n", s_count, s_frame);
			fflush(stderr);
		}
	}
	if (s_state != 1 || m_localStatus != NETLOCALSTATUS_INGAME || TheGameLogic->getFrame() < s_frame)
		return;
	s_state = 0;
	m_conMgr->gxTestWrapFlood(s_count);
}
#endif // GX_TEST_INSTRUMENTS

/**
 * Service queues, process message stream, etc
 */
void Network::update()
{
//
// 1. Take Commands off TheCommandList, hand them off to the ConnectionManager.
// 2. Call ConnectionManager->update;
// 3. Check to see if all the commands for the next frame are there.
// 4. If all commands are there, put that frame's commands on TheCommandList.
//
	m_frameDataReady = FALSE;
	m_isStalling = FALSE;

#if defined(RTS_DEBUG)
	if (m_networkOn == FALSE) {
		return;
	}
#endif

	GetCommandsFromCommandList(); // Remove commands from TheCommandList and send them to the connection manager.
#if defined(GX_TEST_INSTRUMENTS)
	if (m_conMgr != nullptr) {
		gxTestForge();
		gxTestWrapFlood();
	}
#endif
	if (m_conMgr != nullptr) {
		if (m_localStatus == NETLOCALSTATUS_INGAME) {
			m_conMgr->updateRunAhead(m_runAhead, m_frameRate, m_didSelfSlug, getExecutionFrame());
			m_didSelfSlug = FALSE;
		}
		//m_conMgr->update(); // Do the priority thing, packetize thing. This also calls the Transport::update function.
									 // depacketizes the incoming packets and puts them on the Network command list.
	}

	liteupdate();

	if (m_localStatus == NETLOCALSTATUS_LEFT) {// || (m_localStatus == NETLOCALSTATUS_LEAVING)) {
		endOfGameCheck();
	}

	if (AllCommandsReady(TheGameLogic->getFrame())) { // If all the commands are ready for the next frame...
		m_conMgr->handleAllCommandsReady();
//		DEBUG_LOG(("Network::update - frame %d is ready", TheGameLogic->getFrame()));
		if (timeForNewFrame()) { // This needs to come after any other pre-frame execution checks as this changes the timing variables.
			RelayCommandsToCommandList(TheGameLogic->getFrame());	// Put the commands for the next frame on TheCommandList.
			m_frameDataReady = TRUE; // Tell the GameEngine to run the commands for the new frame.
		}
	}
	else {
		// GeneralsX: Keep stall check in nanoseconds on Linux
		int64_t curTime;
		#ifdef _WIN32
		QueryPerformanceCounter((LARGE_INTEGER *)&curTime);
		#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		curTime = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
		#endif
		m_isStalling = curTime >= m_nextFrameTime;
	}
}

// Transport half of update(), for orders GameClient::update translates before its draw (gxSendOrdersBeforeDraw).
// Stamps them with getExecutionFrame() as update() would (no logic tick runs in between) and sends the command counts
// of finished frames. Never relays a frame, touches m_frameDataReady or renegotiates run-ahead: that stays in update().
// Only the send half of the connection service runs (ConnectionManager::flushSends).
void Network::sendLocalCommands()
{
#if defined(RTS_DEBUG)
	if (m_networkOn == FALSE) {
		return;
	}
#endif
	GetCommandsFromCommandList();
	if (m_conMgr != nullptr) {
		m_conMgr->flushSends();
	}
}

void Network::liteupdate() {

#if defined(RTS_DEBUG)
	if (m_networkOn == FALSE) {
		return;
	}
#endif

	if (m_conMgr != nullptr) {
		if (m_localStatus == NETLOCALSTATUS_PREGAME) {
			m_conMgr->update(FALSE);
		} else {
			m_conMgr->update(TRUE);
		}
	}
}

void Network::endOfGameCheck() {
	if (m_conMgr != nullptr) {
		if (m_conMgr->canILeave()) {
			m_conMgr->disconnectLocalPlayer();
			TheGameLogic->exitGame();
			m_localStatus = NETLOCALSTATUS_POSTGAME;

			DEBUG_LOG(("Network::endOfGameCheck - about to show the shell"));
		}
#if defined(RTS_DEBUG)
		else {
			m_conMgr->debugPrintConnectionCommands();
		}
#endif
	}
}

Bool Network::timeForNewFrame() {
	// GeneralsX: Fast forward (FramePacer::setFastForward): the main loop is the pacer. It iterates
	// at k x the tick rate, and a frame is relayed on every pass that has one ready - still at most one per client update,
	// because both callers of Network::update (GameEngine::update, updateBetweenDraws) run the logic right after it and
	// the between-draws pass only relays for a client update that has not ticked yet. A second pacer here would only
	// fight the loop's (two grids of one period drift apart and drop ticks). Fast forward is only ever on in a match
	// whose only human is this player (changeFastForward), so there is no other peer whose pace this could outrun;
	// the self-slug above run-ahead is moot for the same reason. Back at 1x m_nextFrameTime is at most one
	// period ahead (kept current below), so the first 1x frame waits at most one period.
	const Bool ffBypass = TheFramePacer != nullptr && TheFramePacer->isFastForwardActive();
	// GeneralsX: -maxspeed (CommandLine.cpp): a -headless peer in a match with no human - the
	// harness's -lanobserver AI matches, whose only "human" is this observer seat (GameLogic::gxCheatsAllowed) - is not
	// paced at all: the headless loop never waits (HeadlessMatch::run leaves the pacer alone), so the match runs as fast
	// as one core simulates it. Same one-tick-per-client-update loop, so the same tick sequence as 1x. A second human
	// (a -lanjoin peer) turns it off on every peer alike, from simulation state.
	const Bool maxSpeedBypass = FramePacer::isHeadlessMaxSpeed() && TheGlobalData->m_headless && GameLogic::gxCheatsAllowed();
	if (ffBypass || maxSpeedBypass) {
		// Keep the stall deadline current while the pacer is bypassed, as a frame issued now would: one normal frame period
		// ahead. Left at its pre-bypass value it is far in the past, so the first pass whose commands are not ready reads as
		// a stall (Network::update: m_isStalling = curTime >= m_nextFrameTime) and the next GameClient::update skips its
		// drawable/shroud refresh on a pass that would not have been a stall at 1x - client state the simulation reads
		// A real wait of a whole frame period still reads as one.
		int64_t now;
		#ifdef _WIN32
		QueryPerformanceCounter((LARGE_INTEGER *)&now);
		#else
		struct timespec fts;
		clock_gettime(CLOCK_MONOTONIC, &fts);
		now = static_cast<int64_t>(fts.tv_sec) * 1000000000 + fts.tv_nsec;
		#endif
		m_nextFrameTime = now + m_perfCountFreq / max(1, m_frameRate);
		return TRUE;
	}

	// GeneralsX: Keep network frame pacing in nanoseconds on Linux
	int64_t curTime;
	#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER *)&curTime);
	#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	curTime = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
	#endif
	int64_t frameDelay = m_perfCountFreq / m_frameRate;

	/*
	 * If we're pushing up against the edge of our run ahead, we should slow the framerate down a bit
	 * to avoid being frozen by spikes in network lag.  This will happen if another user's computer is
	 * running too far behind us, so we need to slow down to let them catch up.
	 */
	if (m_conMgr != nullptr) {
		Real cushion = m_conMgr->getMinimumCushion();
		Real runAheadPercentage = m_runAhead * (TheGlobalData->m_networkRunAheadSlack / (Real)100.0); // If we are at least 50% into our slack, we need to slow down.
		if (cushion < runAheadPercentage) {
			int64_t oldFrameDelay = frameDelay;
			frameDelay += oldFrameDelay / 10; // temporarily decrease the frame rate by 20%.
//			DEBUG_LOG(("Average cushion = %f, run ahead percentage = %f.  Adjusting frameDelay from %I64d to %I64d", cushion, runAheadPercentage, oldFrameDelay, frameDelay));
			m_didSelfSlug = TRUE;
//		} else {
//			DEBUG_LOG(("Average cushion = %f, run ahead percentage = %f", cushion, runAheadPercentage));
		}
	}

	// Check to see if we can run another frame: one frame period per frame, and a pacer that falls more than
	// debtFrames periods behind forgives the rest (re-anchors to now).
	//
	// GeneralsX: TICK CATCH-UP (Common/CatchUpPolicy.h). Retail forgave anything past TWO
	// periods, so a draw longer than two periods (a zoomed-out 6v6 at 27 fps: 37 ms) lost its ticks for good however the
	// loop tried to repay them. While catch-up is on - a Zero Hour client that draws (W3DDisplay::init) - the pacer keeps
	// up to CatchUpDebtFrames, and the draws it owes are skipped (FramePacer::isCatchUpDrawSkip) so the logic-only passes
	// that follow repay it, one tick each. Off - every -headless peer, base Generals, GX_CATCHUP=0 - this is the retail
	// arithmetic exactly (scripts/test/catchup-pacing-test.sh checks it bit for bit). Still at most one frame per call,
	// still only after AllCommandsReady: the average rate cannot exceed m_frameRate.
	const int debtFrames = (TheFramePacer != nullptr && TheFramePacer->isCatchUpEnabled())
		? (int)CatchUpPolicy::CatchUpDebtFrames : (int)CatchUpPolicy::RetailDebtFrames;
	m_lastFrameDelay = frameDelay;
	return CatchUpPolicy::pacerStep(m_nextFrameTime, curTime, frameDelay, debtFrames);
}

// GeneralsX: Tick catch-up: how many frame periods the pacer owes this machine now, read at the
// draw, before this iteration's network pass (FramePacer::isCatchUpDrawSkip). 0 outside a running match: before
// NETLOCALSTATUS_INGAME m_nextFrameTime is the stale value the load left, and reading it as debt would skip the loading
// screen's draws. 0 too while the frame after the next one has no commands (CatchUpPolicy::framesOwed): that debt is a
// remote peer's. The probe is allCommandsReady's justTesting form, the one that leaves the disconnect manager alone; its
// FRAMEDATA_RESEND branch fires only for a frame that really holds too many commands, which the normal path would reset and
// re-request one tick later anyway.
Int Network::getPacerFramesDue()
{
	if (m_localStatus != NETLOCALSTATUS_INGAME || m_frameRate == 0 || m_conMgr == nullptr || TheGameLogic == nullptr) {
		return 0;
	}
	int64_t curTime;
	#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER *)&curTime);
	#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	curTime = static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
	#endif
	const int64_t frameDelay = (m_lastFrameDelay > 0) ? m_lastFrameDelay : (int64_t)(m_perfCountFreq / m_frameRate);
	// Both the frame this iteration ticks and the one after it (frame+1 can complete first after a loss).
	const UnsignedInt f = TheGameLogic->getFrame();
	return CatchUpPolicy::framesOwed(CatchUpPolicy::framesDue(m_nextFrameTime, curTime, frameDelay),
		m_conMgr->allCommandsReady(f, TRUE) && m_conMgr->allCommandsReady(f + 1, TRUE));
}

/**
 * Returns true if the game commands for the next frame have been put on the command list.
 */
// KNOWN, not fixed: the NETLOCALSTATUS_LEFT term is not paced by timeForNewFrame(). After the local PLAYERLEAVE
// executes this is TRUE on every main-loop iteration until the game exits, so logic free-runs on unrelayed frames.
// Fix shape: handle LEFT in Network::update() behind timeForNewFrame() and drop this term.
Bool Network::isFrameDataReady() {
	return (m_frameDataReady || (m_localStatus == NETLOCALSTATUS_LEFT));
}

Bool Network::isStalling()
{
	return m_isStalling;
}

/**
 * returns the number of incoming bytes per second averaged over the last 30 sec.
 */
Real Network::getIncomingBytesPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getIncomingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * returns the number of incoming packets per second averaged over the last 30 sec.
 */
Real Network::getIncomingPacketsPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getIncomingPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * returns the number of outgoing bytes per second averaged over the last 30 sec.
 */
Real Network::getOutgoingBytesPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getOutgoingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * returns the number of outgoing packets per second averaged over the last 30 sec.
 */
Real Network::getOutgoingPacketsPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getOutgoingPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * returns the number of bytes received per second that are not from a generals client averaged over 30 sec.
 */
Real Network::getUnknownBytesPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getUnknownBytesPerSecond();
	else
	  return 0.0;
}

/**
 * returns the number of packets received per second that are not from a generals client averaged over 30 sec.
 */
Real Network::getUnknownPacketsPerSecond()
{
	if (m_conMgr)
		return m_conMgr->getUnknownPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * returns the smallest packet arrival cushion since this was last called.
 */
UnsignedInt Network::getPacketArrivalCushion()
{
	if (m_conMgr)
		return m_conMgr->getPacketArrivalCushion();
	else
		return 0;
}

/**
 * Sends a line of chat to the other players
 */
void Network::sendChat(UnicodeString text, RelayMaskType playerMask) {
	Int executionFrame = getExecutionFrame();
	m_conMgr->sendChat(text, playerMask, executionFrame);
}

/**
 * Sends a line of chat to the other players using the disconnect manager.
 */
void Network::sendDisconnectChat(UnicodeString text) {
	m_conMgr->sendDisconnectChat(text);
}

// send a file.  woohoo.
void Network::sendFile(AsciiString path, RelayMaskType playerMask, UnsignedShort commandID)
{
	m_conMgr->sendFile(path, playerMask, commandID);
}

// send a file.  woohoo.
UnsignedShort Network::sendFileAnnounce(AsciiString path, RelayMaskType playerMask)
{
	return m_conMgr->sendFileAnnounce(path, playerMask);
}

Int Network::getFileTransferProgress(Int playerID, AsciiString path)
{
	return m_conMgr->getFileTransferProgress(playerID, path);
}

Bool Network::areAllQueuesEmpty()
{
	return m_conMgr->canILeave();
}

/**
 * Quit the game now.  This should only be called from the disconnect screen.
 */
void Network::quitGame() {
	if (m_conMgr != nullptr) {
		m_conMgr->quitGame();
	}

#if !RTS_GENERALS || !RETAIL_COMPATIBLE_CRC
	// Blow up / Transfer your units when you quit.  Like a normal quit menu quit.
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
	msg->appendBooleanArgument(TRUE);
#endif

	TheGameLogic->exitGame();
	m_localStatus = NETLOCALSTATUS_POSTGAME;
	DEBUG_LOG(("Network::quitGame - quitting game..."));
}

void Network::selfDestructPlayer(Int index)
{
	m_playersToDisconnect.push_back(index);
}


Bool Network::isPacketRouter()
{
	return m_conMgr && m_conMgr->isPacketRouter();
}

Int Network::getPacketRouterSlot()
{
	// -1 rather than 0 when there is no ConnectionManager yet: slot 0 is a real slot, and returning
	// it here would make "no network" indistinguishable from "the host is in slot 0", which is the
	// most common arrangement there is.
	return m_conMgr ? (Int)m_conMgr->getPacketRouterSlot() : -1;
}

/**
 * Register a vote towards a player being disconnected.
 */
void Network::voteForPlayerDisconnect(Int slot) {
	if (m_conMgr != nullptr) {
		m_conMgr->voteForPlayerDisconnect(slot);
	}
}

void Network::updateLoadProgress( Int percent )
{
	if (m_conMgr != nullptr) {
		m_conMgr->updateLoadProgress( percent );
	}
}

void Network::loadProgressComplete()
{
	if (m_conMgr != nullptr) {
		m_conMgr->loadProgressComplete();
	}
}

void Network::sendTimeOutGameStart()
{
	if (m_conMgr != nullptr) {
		m_conMgr->sendTimeOutGameStart();
	}
}


UnsignedInt Network::getLocalPlayerID()
{
if (m_conMgr != nullptr) {
		return m_conMgr->getLocalPlayerID();
	}
	return 49;
}
UnicodeString Network::getPlayerName(Int playerNum)
{
	if (playerNum == m_conMgr->getLocalPlayerID()) {
		if (m_localStatus != NETLOCALSTATUS_INGAME) {
			return UnicodeString::TheEmptyString;
		}
	}
	if (m_conMgr != nullptr) {
		return m_conMgr->getPlayerName( playerNum );
	}
	return UnicodeString::TheEmptyString;
}
Int Network::getNumPlayers()
{
	if (m_conMgr != nullptr) {
		return m_conMgr->getNumPlayers();
	}
	return -1;
}

Int Network::getSlotAverageFPS(Int slot) {
	if (m_conMgr != nullptr) {
		return m_conMgr->getSlotAverageFPS(slot);
	}
	return -1;
}

#if defined(RTS_DEBUG)
void Network::toggleNetworkOn() {
	if (m_networkOn == TRUE) {
		m_networkOn = FALSE;
	} else {
		m_networkOn = TRUE;
	}
}
#endif

void Network::notifyOthersOfCurrentFrame() {
	if (m_conMgr != nullptr) {
		m_conMgr->notifyOthersOfCurrentFrame(TheGameLogic->getFrame());
	}
}

void Network::notifyOthersOfNewFrame(UnsignedInt frame) {
	if (m_conMgr != nullptr) {
		m_conMgr->notifyOthersOfNewFrame(frame);
	}
}
