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

#include "Compression.h"
#include "WWLib/strtok_r.h"
#include "Common/AudioEventRTS.h"
#include "Common/CRCDebug.h"
#include "Common/Debug.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameAudio.h"
#include "Common/GameState.h"
#include "Common/LocalFileSystem.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"

#include "GameClient/Diplomacy.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/GXSharedAddr.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NAT.h"
#include "GameNetwork/NetCommandWrapperList.h"
#include "GameNetwork/TransferFileValidation.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/VictoryConditions.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/InGameUI.h"
// GXLobbySaid - the one place a chat line's speaker/body separator lives. GameClient/, so it is
// outside the sim digest; this .cpp is inside it, which is stated in the report for this change.
#include "GameClient/GXLobbyCommands.h"
#include "WWLib/TARGA.h"

static Bool hasValidTransferFileExtension(const AsciiString& filePath)
{
	static const char* const validExtensions[] = {
		"map",
		"ini",
		"str",
		"wak",
		"tga",
		"txt"
	};

	const char* fileExt = strrchr(filePath.str(), '.');

	if (fileExt == nullptr || fileExt[1] == '\0')
	{
		return false;
	}

	fileExt++;

	for (Int i = 0; i < ARRAY_SIZE(validExtensions); ++i)
	{
		if (stricmp(fileExt, validExtensions[i]) == 0)
		{
			return true;
		}
	}

	return false;
}

enum TransferFileType
{
	TransferFileType_Invalid = -1,
	TransferFileType_Map,
	TransferFileType_Ini,
	TransferFileType_Str,
	TransferFileType_Txt,
	TransferFileType_Tga,
	TransferFileType_Wak,
	TransferFileType_Count
};

struct TransferFileRule
{
	const char* ext;
	UnsignedInt maxSize;
};

static const TransferFileRule transferFileRules[TransferFileType_Count] =
{
	{ ".map", 5 * 1024 * 1024 },
	{ ".ini", 2 * 1024 * 1024 },
	{ ".str", 512 * 1024 },
	{ ".txt", 1 * 1024 * 1024 },
	{ ".tga", 2 * 1024 * 1024 },
	{ ".wak", 128 * 1024 },
};

static TransferFileType getTransferFileType(const char* extension)
{
	for (Int i = 0; i < TransferFileType_Count; ++i)
	{
		if (stricmp(extension, transferFileRules[i].ext) == 0)
		{
			return static_cast<TransferFileType>(i);
		}
	}
	return TransferFileType_Invalid;
}

static Bool hasValidTransferFileContent(const AsciiString& filePath, const UnsignedByte* data, UnsignedInt dataSize)
{
	const char* fileExt = strrchr(filePath.str(), '.');
	if (fileExt == nullptr)
	{
		DEBUG_LOG(("File '%s' has no extension for content validation.", filePath.str()));
		return false;
	}

	const TransferFileType fileType = getTransferFileType(fileExt);
	if (fileType == TransferFileType_Invalid)
	{
		DEBUG_LOG(("File '%s' has unrecognized extension '%s' for content validation.", filePath.str(), fileExt));
		return false;
	}

	// Check size limit
	const TransferFileRule& rule = transferFileRules[fileType];
	if (dataSize > rule.maxSize)
	{
		DEBUG_LOG(("File '%s' exceeds maximum size (%u bytes, limit %u bytes).", filePath.str(), dataSize, rule.maxSize));
		return false;
	}

	// Extension-specific content validation
	switch (fileType)
	{
	case TransferFileType_Map:
		break;

	case TransferFileType_Ini:
	{
		for (UnsignedInt i = 0; i < dataSize; ++i)
		{
			if (data[i] == 0)
			{
				DEBUG_LOG(("INI file '%s' contains null bytes (likely binary).", filePath.str()));
				return false;
			}
		}
		break;
	}

	case TransferFileType_Tga:
	{
		// GeneralsX: A TARGA is validated by its HEADER, not by the optional
		// TGA 2.0 footer, which this used to demand. See TransferFileValidation.h for the format
		// argument, the measured failure it caused, and why the replacement is the stronger check.
		const char *whyNot = "malformed";
		if (!TransferValidation::isValidTgaImage(
				(const uint8_t *)data, (uint32_t)dataSize, &whyNot))
		{
			DEBUG_LOG(("TGA file '%s' rejected: %s.", filePath.str(), whyNot));
			return false;
		}
		break;
	}

	default:
	{
		break;
	}
	}

	return true;
}

/**
 * Le destructor.
 */
ConnectionManager::~ConnectionManager()
{
	deleteInstance(m_localUser);
	m_localUser = nullptr;

	// Network will delete transports; we just forget them
	delete m_transport;
	m_transport = nullptr;

	Int i = 0;
	for (; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	// This is done here since TheDisconnectMenu should only be there if we are in a network game.
	delete TheDisconnectMenu;
	TheDisconnectMenu = nullptr;

	delete m_disconnectManager;
	m_disconnectManager = nullptr;

	deleteInstance(m_pendingCommands);
	m_pendingCommands = nullptr;

	deleteInstance(m_relayedCommands);
	m_relayedCommands = nullptr;

	deleteInstance(m_netCommandWrapperList);
	m_netCommandWrapperList = nullptr;

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Le constructor
 */
ConnectionManager::ConnectionManager()
{
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_frameData[i] = nullptr;
	}
	m_transport = nullptr;
	m_recvFrom = nullptr;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_lockstepPlan[i] = MAX_SLOTS;
		m_softCapCache[i] = -1;
	}
	m_lockstepGone = 0;
	m_sharedAddrSlots = 0;
	m_disconnectManager = nullptr;
	m_pendingCommands = nullptr;
	m_relayedCommands = nullptr;
	m_localAddr = 0;
	m_localPort = 0;
	m_netCommandWrapperList = nullptr;
	m_localUser = nullptr;
	m_localUser = newInstance(User);
}

/**
 * Initialize the connection manager and any subsystems.
 */
void ConnectionManager::init()
{
//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	UnsignedInt i = 0;
	for (; i < MAX_SLOTS; ++i) {
		m_connections[i] = nullptr;
	}

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	// GeneralsX: Was a bare `m_packetRouterSlot = 0;` carrying retail's
	// "@todo The LAN/WOL interface should be telling us who the packet router is based on machine
	// specs passed around through game options."
	//
	// This is only the pre-lobby placeholder now. The real choice is made by electPacketRouter(),
	// called from parseUserList() once the slot list is known - see the long note there for why the
	// election is what it is, and for what a MEASURED election would actually require.
	//
	// Leaving a literal 0 here was not merely untidy. Nothing guarantees slot 0 holds a human: if it
	// does not, m_connections[0] is null on every peer, isPacketRouter() is false for everyone, and
	// updateRunAhead's non-router branch dereferences m_connections[m_packetRouterSlot] with no null
	// check - a hard crash on every peer at the first metrics interval. electPacketRouter() picks a
	// slot that is provably occupied, and the null guard added at that call site catches the rest.
	m_packetRouterSlot = 0;
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

//	m_averageFps = 30;			// since 30 fps is the desired rate, we'll start off at that.
//	m_averageLatency = (Real)0.2; // 200ms seems like a good starting point.

	for (i = 0; i < MAX_SLOTS; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_latencyAverages[i] = 0.0; // using zero since all floating point standards should be able to specify 0.0 accurately.
	}
	// GeneralsX: 0 is "no live figure", not "0 ms". See m_peerPingMs.
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_peerPingMs[i] = 0;
		m_peerPingTime[i] = 0;
		m_peerFps[i] = -1;
		m_peerFpsTime[i] = 0;
	}
	m_smallestPacketArrivalCushion = -1;

	m_frameMetrics.init();

	TheDisconnectMenu = NEW DisconnectMenu;
	TheDisconnectMenu->init();

	m_disconnectManager = NEW DisconnectManager;
	m_disconnectManager->init();

	TheDisconnectMenu->attachDisconnectManager(m_disconnectManager);
	TheDisconnectMenu->hideScreen();

	m_netCommandWrapperList = newInstance(NetCommandWrapperList);
	m_netCommandWrapperList->init();

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Reset the connection manager and any subsystems.
 */
void ConnectionManager::reset()
{
//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	delete m_transport;
	m_transport = nullptr;

	UnsignedInt i = 0;
	for (; i < (UnsignedInt)MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	for (i=0; i<(UnsignedInt)MAX_SLOTS; ++i)
	{
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	if (m_netCommandWrapperList == nullptr) {
		m_netCommandWrapperList = newInstance(NetCommandWrapperList);
		m_netCommandWrapperList->init();
	}
	m_netCommandWrapperList->reset();

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	m_packetRouterSlot = -1;

	// GeneralsX: BOUND BY THE ARRAY, NOT BY A HISTORY LENGTH.
	//
	// These two are m_fpsAverages[MAX_SLOTS] and m_latencyAverages[MAX_SLOTS] - one entry PER PLAYER,
	// written by processRunAheadMetrics at [playerID]. They are not history rings; the rings live in
	// FrameMetrics and are the things m_network*HistoryLength is actually for. Walking a MAX_SLOTS
	// array to a history length was a straight out-of-bounds write into whatever follows.
	//
	// THIS WAS LIVE FOR ALMOST THE WHOLE PROJECT, not merely latent. An earlier revision of this
	// comment said it "has not fired yet", which was true of the tree the fix landed on and false of
	// every tree before it. Corrected here rather than left to be quoted onward:
	//
	//   * retail import 3d0ee53a0..c757f213b - defaults 30 and 200, MAX_PLAYER 7 so MAX_SLOTS 8. The
	//     fps loop wrote 22 Ints past its array and the latency loop 192 Reals - 768 bytes - through
	//     m_fpsAverages, the scalars after it, and into the file-transfer maps. Live.
	//   * c757f213b dropped both defaults to 10, but MAX_SLOTS was still 8. Still live, 2 past each.
	//   * 3c8e01846 (12/08/2026) raised MAX_SLOTS to 12. 10 < 12, so it went latent - ONE DAY before
	//     this fix.
	//
	// Both lengths are INI-parsed (GlobalData.cpp: "NetworkFPSHistoryLength",
	// "NetworkLatencyHistoryLength"), so a data file raising either past MAX_SLOTS makes it live
	// again - a DATA file silently corrupting engine memory on every reset(). Bounding by the array
	// is what closes that, not the current defaults. The init path a few hundred lines above always
	// used MAX_SLOTS; only reset() was wrong.
	//
	// An unbounded search of the tree at 91f386ad0 found no second instance in either direction: no
	// other MAX_SLOTS array walked by a history length, and no history-length ring indexed by a slot
	// without a guard.
	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_latencyAverages[i] = 0.0;
	}

	// GeneralsX: Bounded by MAX_SLOTS, which is what these arrays are
	// sized by - as are the two loops above, now.
	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_peerPingMs[i] = 0;
		m_peerPingTime[i] = 0;
		m_peerFps[i] = -1;
		m_peerFpsTime[i] = 0;
	}

	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
	}

	m_frameMetrics.reset();
}

UnsignedInt ConnectionManager::getPingFrame()
{
	return (m_disconnectManager)?m_disconnectManager->getPingFrame():0;
}

Int ConnectionManager::getPingsSent()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsSent():0;
}

Int ConnectionManager::getPingsReceived()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsReceived():0;
}

Bool ConnectionManager::isPlayerConnected( Int playerID )
{
	DEBUG_ASSERTCRASH( playerID < MAX_SLOTS, ("ConnectionManager::isPlayerConnected - %d is an invalid player number", playerID) );
	return ( playerID == m_localSlot || (m_connections[playerID] && !m_connections[playerID]->isQuitting()) );
}

// GeneralsX: Did the datagram doRelay is processing come from @p slot's
// peer? Matched by address, and by port only when another slot's peer shares that address (two
// instances behind one address on a LAN; through a relay every peer has its own virtual address and
// the port is the relay's). FALSE for anything without a datagram behind it - a reassembled wrapper -
// for the local slot, and for a slot with no connection: none of those is a peer that sent this.
// GeneralsX: "Shares" is m_sharedAddrSlots, fixed at parseUserList, and no
// longer a scan of the live connections - see GXSharedAddr.h for the bypass that scan allowed.
Bool ConnectionManager::arrivedFrom(UnsignedInt slot) {
	if ((m_recvFrom == nullptr) || (slot >= MAX_SLOTS) || (m_connections[slot] == nullptr)) {
		return FALSE;
	}
	User *user = m_connections[slot]->getUser();
	if (user->GetIPAddr() != m_recvFrom->addr) {
		return FALSE;
	}
	return ((m_sharedAddrSlots & (gxSlotBit(slot))) == 0) || (user->GetPort() == m_recvFrom->port);
}

// GeneralsX: Which peer sent the datagram doRelay is processing: the one slot arrivedFrom
// accepts (addresses are unique per slot, ports decide where two slots share one - m_sharedAddrSlots). MAX_SLOTS when
// there is no datagram (a reassembled wrapper, m_recvFrom nullptr) or no connected peer matches.
UnsignedInt ConnectionManager::arrivalSlot() {
	for (UnsignedInt i = 0; i < MAX_SLOTS; ++i) {
		if (arrivedFrom(i)) {
			return i;
		}
	}
	return MAX_SLOTS;
}

// GeneralsX: [GXNET] resent / resent-acked / resent-unacked (GX_NET only): every ack-required
// command sendSingleFrameToPlayer queues, and which ack - from whom, through whom - takes it off the queue again. A
// resent FRAMEINFO carries a fresh ID from the RESENDER's counter but the original slot as its player, and the ack that
// clears it must name both (Connection::processAck), so this is the direct record of whether the resender ever hears.
// A ring: entries are dropped when acked; one still here GX_RESEND_UNACKED_MS after it was queued is printed once by
// gxNetSample (and an ack that arrives later still prints).
struct GxResent { UnsignedInt to, orig, id, ms; Bool live, told; };
static GxResent s_gxResent[256];
static UnsignedInt s_gxResentNext = 0;
static const Bool s_gxResendTrace = (getenv("GX_NET") != nullptr);
static const UnsignedInt GX_RESEND_UNACKED_MS = 2000;

static void gxNoteResent(UnsignedInt to, NetCommandMsg *c) {
	if (!s_gxResendTrace || !CommandRequiresAck(c))
		return;
	GxResent &r = s_gxResent[s_gxResentNext++ % ARRAY_SIZE(s_gxResent)];
	r.to = to; r.orig = c->getPlayerID(); r.id = c->getID(); r.ms = timeGetTime(); r.live = TRUE; r.told = FALSE;
	static Int s_told = 0;
	if (s_told < 128) {
		++s_told;
		fprintf(stderr, "[GXNET] resent to=%u type=%s orig=%u id=%u frame=%u\n", to, GetNetCommandTypeAsString(c->getNetCommandType()),
			r.orig, r.id, (unsigned)c->getExecutionFrame());
	}
}

static void gxNoteResendAcked(UnsignedInt acker, NetCommandMsg *c, UnsignedInt via) {
	if (!s_gxResendTrace)
		return;
	for (size_t k = 0; k < ARRAY_SIZE(s_gxResent); ++k) {
		GxResent &r = s_gxResent[k];
		if (r.live && (r.to == acker) && (r.orig == (UnsignedInt)c->getPlayerID()) && (r.id == (UnsignedInt)c->getID())) {
			r.live = FALSE;
			static Int s_told = 0;
			if (s_told < 128) {
				++s_told;
				fprintf(stderr, "[GXNET] resent-acked to=%u orig=%u id=%u via=%d after_ms=%u\n", acker, r.orig, r.id,
					(via < MAX_SLOTS) ? (Int)via : -1, (unsigned)(timeGetTime() - r.ms));
			}
			return;
		}
	}
}

static void gxReportResendUnacked() {
	if (!s_gxResendTrace)
		return;
	const UnsignedInt now = timeGetTime();
	static Int s_told = 0;
	for (size_t k = 0; (k < ARRAY_SIZE(s_gxResent)) && (s_told < 128); ++k) {
		GxResent &r = s_gxResent[k];
		if (r.live && !r.told && ((now - r.ms) > GX_RESEND_UNACKED_MS)) {
			r.told = TRUE;
			++s_told;
			fprintf(stderr, "[GXNET] resent-unacked to=%u orig=%u id=%u age_ms=%u\n", r.to, r.orig, r.id, (unsigned)(now - r.ms));
		}
	}
}

// GeneralsX: A disconnect-range command that did not arrive from the peer it
// names - refused in processNetCommand, and NOT ACKED in doRelay either. The ack
// goes to the NAMED peer and carries the forger's command ID, so it could cancel the retransmission
// of that peer's own queued command with the same ID. Every other acked type can still be forged to
// the same end - a GAMECOMMAND's sender field is just as unbound - so this closes only the range the
// gate can judge; the envelope needs per-hop authentication for the rest.
// GeneralsX: FRAMERESENDREQUEST joins the range (and the name drops
// "Disconnect"): it too is only ever sent DIRECT by the peer it names (requestFrameDataResend ->
// sendLocalCommandDirect) and never relayed, so it can be judged the same way - and it must be judged
// HERE, before doRelay's ackCommand, or a forged one still gets an ack sent to the named peer with the
// forger's (originalPlayerID, commandID). processFrameResendRequest refuses the request itself.
// GeneralsX: The three file-transfer types join it. Each is CommandRequiresDirectSend
// and sent only by the peer it names (sendFileAnnounce / sendFile by the host, a progress report by its
// receiver, all through sendLocalCommandDirect, relay = the one recipient), so the datagram carrying one came
// from that peer. FILEANNOUNCE and FILEPROGRESS always fit one datagram (a name of at most _MAX_PATH; two
// numbers), so one reassembled from a wrapper (m_recvFrom nullptr, arrivedFrom FALSE) is forged too - review
// net-hardening r1: a one-chunk wrapper from the attacker's own slot carried a "failed" report in another
// peer's name. A FILE is legitimately reassembled; that case is not judged here. (With RETAIL_COMPATIBLE_NETWORKING's
// 476-byte packets - no preset builds it - a long map path does wrap an announce, so there it is judged like a FILE.)
Bool ConnectionManager::isForgedDirect(NetCommandMsg *msg) {
	const NetCommandType cmdType = msg->getNetCommandType();
#if RETAIL_COMPATIBLE_NETWORKING
	const Bool announceAlwaysFits = FALSE;
#else
	const Bool announceAlwaysFits = TRUE;
#endif
	const Bool judged = ((cmdType > NETCOMMANDTYPE_DISCONNECTSTART) && (cmdType < NETCOMMANDTYPE_DISCONNECTEND))
		|| (cmdType == NETCOMMANDTYPE_FRAMERESENDREQUEST) || (cmdType == NETCOMMANDTYPE_FILEPROGRESS)
		|| ((cmdType == NETCOMMANDTYPE_FILEANNOUNCE) && (announceAlwaysFits || (m_recvFrom != nullptr)))
		|| ((cmdType == NETCOMMANDTYPE_FILE) && (m_recvFrom != nullptr));
	return judged && !arrivedFrom(msg->getPlayerID());
}

void ConnectionManager::attachTransport(Transport *transport) {
	delete m_transport;
	m_transport = transport;
}

/**
 * zero out the command counts for the given frames.  Presently this is used for
 * the start of a game since there won't be any commands for the first few frames due to runahead.
 */
void ConnectionManager::zeroFrames(UnsignedInt startingFrame, UnsignedInt numFrames) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
//			DEBUG_LOG(("Calling zeroFrames on player %d, starting frame %d, numFrames %d", i, startingFrame, numFrames));
			m_frameData[i]->zeroFrames(startingFrame, numFrames);
		}
	}
}

/**
 * Destroy any game messages that are left over due to the run ahead.
 */
void ConnectionManager::destroyGameMessages() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		// Need to destroy these game messages because when the game ends, there are
		// still some game messages left over because of the run ahead aspect of
		// network play.
		if (m_frameData[i] != nullptr) {
			m_frameData[i]->destroyGameMessages();
		}
	}
}

/**
 * ConnectionManager::doRelay()
 * Queries the transport for commands that need to be relayed to another client.
 * Get those commands and relay them to the appropriate Connection(s). We make the
 * assumption that a command will only be relayed once.
 */
void ConnectionManager::doRelay() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_softCapCache[i] = -1;
	}
	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer); ++i) {
		if (m_transport->m_inBuffer[i].length > 0) {
			// This transport buffer has yet to be processed.

			// make a NetPacket out of this data so it can be broken up into individual commands.
			NetPacket packet(m_transport->m_inBuffer[i]);

			//DEBUG_LOG(("ConnectionManager::doRelay() - got a packet with %d commands", packet.getNumCommands()));
			//LOGBUFFER( packet.getData(), packet.getLength() );

			// Get the command list from the packet.
			NetCommandList *cmdList = packet.getCommandList();

			// Iterate through the commands in this packet and send them to the proper connections.
			m_recvFrom = &m_transport->m_inBuffer[i];
			for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
				//DEBUG_LOG(("ConnectionManager::doRelay() - Looking at a command of type %s",
					//GetNetCommandTypeAsString(cmd->getCommand()->getNetCommandType())));

				relayDatagramCommand(cmd);
			}
			m_recvFrom = nullptr;

			deleteInstance(cmdList);
			cmdList = nullptr;

			// signal that this has been processed.
			m_transport->m_inBuffer[i].length = 0;
		} else {
			break;
		}
	}

	// Every completed serialization is its own item, in order, never deduped (design v16 section 5).
	std::vector<GXReadyItem> ready;
	m_netCommandWrapperList->getReadyCommands(ready);
	for (size_t k = 0; k < ready.size(); ++k) {
		processReadyWrapped(ready[k]);
		deleteInstance(ready[k].ref);
	}
}

// One command of the datagram doRelay is processing (m_recvFrom): ack it, process it, relay it.
void ConnectionManager::relayDatagramCommand(NetCommandRef *cmd) {
	// GeneralsX: Not a wrapper chunk: it is acked by WRAPPERACK, and only once admitted
	// (processWrapper - design v16 section 2). Every other acked type as base.
	if (CommandRequiresAck(cmd->getCommand()) && (cmd->getCommand()->getNetCommandType() != NETCOMMANDTYPE_WRAPPER)
			&& !isForgedDirect(cmd->getCommand())) {
		ackCommand(cmd, m_localSlot);
	}
	if (!processNetCommand(cmd)) {
		sendRemoteCommand(cmd);
	}
}

// GeneralsX: ONE REASSEMBLED SERIALIZATION (v16 section 5).
// Its chunks were acked hop by hop (WRAPPERACK); the command itself is acked as base acks any command - to the hop it came from,
// the source, as ackCommand acks a datagram's sender. Then:
//  - From its own sender (source == sender): as base, processNetCommand and, unless it says no, sendRemoteCommand, which re-wraps
//    it per connection with the recipient-only mask - the router relays the COMMAND, never the chunks. One exception keeps base's
//    forwarding obligation: a synchronized command for a frame this peer has already executed is still owed to the
//    rest of its relay, through the connection loop only - never into local frame data (FrameDataManager indexes frame %
//    FRAME_DATA_LENGTH unchecked, so a stale command would count into a later frame).
//  - From anyone else (a frame resend, or a forgery): local delivery only. Stated deviation: base forwarded it on
//    its relay; honest ones carry recipient-only masks, so nothing honest changes, and a forgery is not exported.
//  - A replay (a whole copy of a recorded synchronized serialization arrived again: its sender retried after a lost ack): handled as
//    base handles a duplicate - acked and processed against the CURRENT frame data - and owed to the record's relay as one full copy
//    per recipient (Task 5's obligations).
void ConnectionManager::processReadyWrapped(const GXReadyItem &item) {
	NetCommandRef *cmd = item.ref;
	NetCommandMsg *c = cmd->getCommand();
	if (CommandRequiresAck(c) && (c->getNetCommandType() != NETCOMMANDTYPE_WRAPPER) && !isForgedDirect(c)) {
		ackCommand(cmd, m_localSlot, (UnsignedInt)item.source);
	}
	const Bool ownSender = (item.source == item.sender) && !item.isReplay;
	if (!ownSender) {
		// Local delivery only: the relay keeps just our own bit, so sendRemoteCommand adds it to our frame data and sends nothing.
		const RelayMaskType fullRelay = cmd->getRelay();
		cmd->setRelay(fullRelay & (gxSlotBit(m_localSlot)));
		if (!processNetCommand(cmd)) {
			sendRemoteCommand(cmd);
		}
		cmd->setRelay(fullRelay);
		// GeneralsX: A REPLAY owes one FULL copy to every live recipient on the record's relay (design v16
		// section 4): a recipient that reset its frame data can rebuild the command from nothing less, and a previous
		// serialization with chunks still outstanding does not stand in for it. Coalesced per (recipient, record) - the record is
		// (source, wrapSeq), never (player, 16-bit ID), which wraps - and never dropped: it waits in the recipient's
		// pending-wrap queue with its own reference to the command, so the record may retire meanwhile. Only a record from its own
		// sender is forwarded at all (section 5). Not to a quitting connection (base sendRemoteCommand's filter). Nothing for an
		// originator no longer connected - a replay queued in the doRelay pass that dropped it: base's
		// processNetCommand rejected a departed player's command, and copies or a ledger entry of it would outlive it.
		const UnsignedInt origin = c->getPlayerID();
		const Bool originLive = (origin < MAX_SLOTS) && ((m_connections[origin] != nullptr) || (origin == (UnsignedInt)m_localSlot));
		if (item.isReplay && (item.source == item.sender) && originLive) {
			const RelayMaskType owed = fullRelay & ~(gxSlotBit(m_localSlot));
			RelayMaskType ledger = 0;
			for (Int j = 0; j < MAX_SLOTS; ++j) {
				if ((owed & (gxSlotBit(j))) && (m_connections[j] != nullptr) && !m_connections[j]->isQuitting()) {
					ledger |= (RelayMaskType)(gxSlotBit(j));
					if (!m_connections[j]->hasPendingObligation(item.source, item.wrapSeq)) {
						m_connections[j]->enqueueObligation(c, gxSlotBit(j), item.source, item.wrapSeq);
					}
				}
			}
			// ...and recorded in m_relayedCommands for every recipient owed, exactly as base's re-forward of a complete duplicate
			// recorded it (sendRemoteCommand's addMessage): their acks then produce the ACKSTAGE2 that clears the originator's
			// m_pendingCommands even when the first one was lost. A NEW entry only: an existing entry of
			// this (player, 16-bit ID) - this command's, or another's after the ID wrapped - is left as it is, as
			// base's addMessage refused a held key. Re-arming bits already acked made the entry wait for acks of the copies, and a
			// recipient leaving before it acked (dropConnection keeps ledger bits) kept the key for good. None for a
			// command whose frame we already executed: base never re-forwarded a stale reassembly.
			if ((ledger != 0) && CommandRequiresAck(c) && !isOutOfDate(c)) {
				if (NetCommandRef *entry = m_relayedCommands->addMessage(c)) {
					entry->setRelay(ledger);
				}
			}
		}
		return;
	}
	const UnsignedInt player = c->getPlayerID();
	const Bool live = (player < MAX_SLOTS) && ((m_connections[player] != nullptr) || (player == (UnsignedInt)m_localSlot));
	const Bool successor = live && isOutOfDate(c);
	if (!processNetCommand(cmd)) {
		sendRemoteCommand(cmd);
	} else if (successor) {
		forwardOnConnections(cmd, cmd->getRelay() & ~(gxSlotBit(m_localSlot)));
	}
}

// GeneralsX: THE PER-ORIGINATOR SOFT CAP (design v16 section 3). Every pending-wrap queue on this peer
// (replay obligations included) holding @p originator's commands, counted once per distinct command however many recipients it
// waits for: over GX_RELAY_SOFTCAP_COMMANDS of them or GX_RELAY_SOFTCAP_BYTES of their bodies, a NEW serialization from that source
// is refused unacked (NetCommandWrapperList::processWrapper). Backpressure, not loss: the source retries it and its own window holds
// the rest, while what is already accepted keeps draining. Our own commands are never capped. Computed once per doRelay pass.
Bool ConnectionManager::isOverSoftCap(Int originator) {
	if ((originator < 0) || (originator >= MAX_SLOTS) || (originator == m_localSlot)) {
		return FALSE;
	}
	if (m_softCapCache[originator] < 0) {
		std::set<NetCommandMsg *> commands;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			if (m_connections[i] != nullptr) {
				m_connections[i]->gatherPending(originator, commands);
			}
		}
		UnsignedInt bytes = 0;
		for (std::set<NetCommandMsg *>::const_iterator it = commands.begin(); (it != commands.end()) && (bytes < GX_RELAY_SOFTCAP_BYTES); ++it) {
			bytes += (*it)->getSizeForNetPacket();	// each at most 5 MiB + 4096 (Connection::queueWrap), so no wrap before the test
		}
		m_softCapCache[originator] = ((commands.size() >= GX_RELAY_SOFTCAP_COMMANDS) || (bytes >= GX_RELAY_SOFTCAP_BYTES)) ? 1 : 0;
	}
	return m_softCapCache[originator] == 1;
}

Bool ConnectionManager::isOutOfDate(NetCommandMsg *msg) {
	return IsCommandSynchronized(msg->getNetCommandType()) && (msg->getExecutionFrame() < TheGameLogic->getFrame());
}

/**
 * This is where the non-synchronized network commands should be processed.
 * Return TRUE if the command should not be relayed. Return FALSE if it should be relayed.
 */
Bool ConnectionManager::processNetCommand(NetCommandRef *ref) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandType cmdType = msg->getNetCommandType();

	// Handle ACK commands first (before connection validation)
	if ((cmdType == NETCOMMANDTYPE_ACKSTAGE1) ||
			(cmdType == NETCOMMANDTYPE_ACKSTAGE2) ||
			(cmdType == NETCOMMANDTYPE_ACKBOTH)) {
		// GeneralsX: An ack must come from the peer it names (the acker,
		// msg->getPlayerID()) or from our packet router. processAckStage1 trusted the field, so a forged
		// ACKBOTH (playerID R, commandID X, originalPlayerID V) sent to V deleted V's queued command X
		// to R (Connection::processAck) and its retransmission stopped; IDs are 16-bit and sequential.
		// Honest sources, from every sender in this file:
		//  - ackCommand sends each ack straight from the acker to its destination, playerID = acker.
		//  - An ack carries no relay field (NetAckCommandMsg::getSmallNetPacketSelect), so it inherits
		//    the previous command's mask in its packet; behind a non-router's own commands that is
		//    "everyone but me", and the router's sendRemoteCommand below forwards it to them. Those
		//    copies arrive from the ROUTER naming someone else, so they must still pass. (Until 02/10/2026
		//    they were also the only way a frame resent to that acker (sendSingleFrameToPlayer) got cleared
		//    from the resender's queue; ackCommand now acks the resender directly.)
		//  - processAckStage2's ACKSTAGE2 never sets playerID (0 on the wire) and is sent by the
		//    relayer - the router - so it too arrives from the router.
		// The router itself has no connection to itself, so it accepts only acks from their acker and
		// relays nothing else: a forged ACK PACKET cannot be laundered through it. A refused ack is not
		// relayed.
		// Known limit: command envelopes are not authenticated per hop (inherited lockstep design).












		if (!arrivedFrom(msg->getPlayerID()) && !arrivedFrom(m_packetRouterSlot)) {
			static Int s_ackRefusals = 0;	// Trade-off: capped, as the disconnect gate below
			if (s_ackRefusals < 32) {
				++s_ackRefusals;
				fprintf(stderr, "[GXGATE] ack type %d refused: it names slot %u but came from neither that peer nor the packet router\n",
					(Int)cmdType, (unsigned)msg->getPlayerID());
			}
			return TRUE;
		}
		processAck(msg);
		return FALSE;
	}

	// GeneralsX: A chunk's ack, scoped to the connection it answers (design v16 section 2)
	// wrapSeq is per sender-connection, so it is applied only when it came straight from the acker, and never relayed.
	if (cmdType == NETCOMMANDTYPE_WRAPPERACK) {
		const UnsignedInt acker = msg->getPlayerID();
		if ((acker < MAX_SLOTS) && arrivedFrom(acker)) {
			NetWrapperAckCommandMsg *ack = (NetWrapperAckCommandMsg *)msg;
			m_connections[acker]->processWrapperAck(ack->getOriginalPlayerID(), ack->getWrapSeq(), ack->getChunkNumber());
		}
		return TRUE;
	}

	// GeneralsX: A wrapper chunk is reassembled here and NEVER forwarded raw (design v16 section 5): a
	// wrapSeq means something only on the hop that drew it, and the router relays the reassembled command instead (doRelay ->
	// processReadyWrapped). Before the departed-player checks below: every chunk that is not refused must be acked, or the
	// sender's window never passes it (a chunk naming a player who has left still came from a live hop). A wrapper nested in a
	// reassembled command is never honest (m_recvFrom nullptr) and is dropped.
	if (cmdType == NETCOMMANDTYPE_WRAPPER) {
		if (m_recvFrom != nullptr) {
			processWrapper(ref);
		}
		return TRUE;
	}

	// Early validation checks
	if (msg->getPlayerID() >= MAX_SLOTS) {
		return TRUE;
	}

	if ((m_connections[msg->getPlayerID()] == nullptr) && (msg->getPlayerID() != m_localSlot)) {
		// if this is from a player that is no longer in the game, then ignore them.
		return TRUE;
	}

	if (msg->getPlayerID() != m_localSlot) {
		if (m_connections[msg->getPlayerID()] == nullptr) {
			return TRUE;
		}
	}

	// Don't allow an out of date command to be sent through.
	// Its unnecessary traffic and it could cause problems.
	//
	// This was a fix for a command count bug where a command would be
	// executed, then a command for that old frame would be added to the
	// FrameData for that frame + 256, and would screw up the command count.
	if (isOutOfDate(msg)) {
		return TRUE;
	}

	// Handle disconnect commands as a range
	if ((cmdType > NETCOMMANDTYPE_DISCONNECTSTART) && (cmdType < NETCOMMANDTYPE_DISCONNECTEND)) {
		// GeneralsX: A disconnect command speaks for the peer that SENT it.
		//
		// Its sender is getPlayerID(), a packet field, and the disconnect machinery trusted it as a
		// person: a vote counted for whoever the packet named, so one peer could cast every vote
		// needed to vote anyone out, and a DISCONNECTPLAYER naming the router as its sender passed
		// for the router's. Measured: forged as the router by slot 1, it dropped slot 2 from the
		// router's mesh (scripts/test/net-kill-gates.sh forge-eject-as-router).
		//
		// This whole range can be bound to the transport, unlike a synchronised order: every one of
		// these is sent DIRECT by the peer it names (CommandRequiresDirectSend, or sendLocalCommandDirect
		// for the keepalive and the router query/ack), is never relayed (this branch returns TRUE,
		// which stops the router forwarding it) and is never re-sent on another peer's behalf
		// (sendSingleFrameToPlayer only resends frame data). So the datagram it arrived in came from
		// the peer it names, or it is forged. What the datagram came from is not a claim: through a
		// relay it is the source the relay checked against the sender's registration (relay.js
		// refuses a mismatch), direct it is the source whose pair key verified the tag
		// (Transport::doRecv, authVerifyAndStrip), and on a LAN it is the socket's source address.
		if (isForgedDirect(msg)) {
			static Int s_refusals = 0;	// Trade-off: capped so a flood of forgeries cannot turn into a flood of fflush
			if (s_refusals < 32) {
				++s_refusals;
				fprintf(stderr, "[GXGATE] disconnect command %d refused: it names slot %u but did not come from that peer (frame %u)\n",
					(Int)cmdType, (unsigned)msg->getPlayerID(), (unsigned)TheGameLogic->getFrame());
				fflush(stderr);
			}
			return TRUE;
		}
		m_disconnectManager->processDisconnectCommand(ref, this);
		return TRUE;
	}

	// Process command by type
	switch (cmdType) {

		case NETCOMMANDTYPE_FRAMEINFO: {
			processFrameInfo((NetFrameCommandMsg *)msg);
			// need to set the relay so we don't send it to ourselves.
			RelayMaskType relay = ref->getRelay();
			relay = relay & (ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_PROGRESS: {
			//DEBUG_LOG(("ConnectionManager::processNetCommand - got a progress net command from player %d", msg->getPlayerID()));
			processProgress((NetProgressCommandMsg *) msg);
			// need to set the relay so we don't send it to ourselves.
			RelayMaskType relay = ref->getRelay();
			relay = relay & (ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_TIMEOUTSTART:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a TimeOut GameStart net command from player %d", msg->getPlayerID()));
			processTimeOutGameStart(msg);
			return FALSE;

		case NETCOMMANDTYPE_RUNAHEADMETRICS:
			processRunAheadMetrics((NetRunAheadMetricsCommandMsg *)msg);
			return TRUE;

		case NETCOMMANDTYPE_LATENCYREPORT:
			processLatencyReport((NetLatencyReportCommandMsg *)msg);
			return TRUE;

		case NETCOMMANDTYPE_KEEPALIVE:
			return TRUE;

		case NETCOMMANDTYPE_DISCONNECTCHAT:
			processDisconnectChat((NetDisconnectChatCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_LOADCOMPLETE:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a Load Complete net command from player %d", msg->getPlayerID()));
			processLoadComplete(msg);
			return FALSE;

		case NETCOMMANDTYPE_CHAT:
			processChat((NetChatCommandMsg *)msg);
			return FALSE;

		// GeneralsX: A file transfer speaks for the peer that sent it, and only the
		// game host sends files. Any connected peer could send a FILE and have it written (sweep v-files-01),
		// or an announce that replaced the host's (its name is what the host's progress poll matches), or a
		// "failed" report in another peer's name that aborted the host's transfer. isForgedDirect binds a
		// datagram to the slot it names; slot 0 is the host (GameInfo::amIHost) and the only one that calls
		// sendFileAnnounce / sendFile (FileTransfer.cpp doFileTransfer). None of the three is ever relayed: each is
		// sent direct to its one recipient (relay = that slot). An accepted one carrying a wider relay mask was
		// forwarded, the copy was refused downstream (it arrives from us, naming its sender) and so never acked,
		// and the host's queue never emptied - the transfer stalled (net-hardening r2). TRUE = do not relay.
		case NETCOMMANDTYPE_FILE:
		case NETCOMMANDTYPE_FILEANNOUNCE:
		case NETCOMMANDTYPE_FILEPROGRESS:
			if (isForgedDirect(msg) || ((cmdType != NETCOMMANDTYPE_FILEPROGRESS) && (msg->getPlayerID() != 0))) {
				static Int s_fileRefusals = 0;	// Trade-off: capped, as the disconnect gate above
				if (s_fileRefusals < 32) {
					++s_fileRefusals;
					fprintf(stderr, "[GXFILE] %s refused: it names slot %u and %s\n", GetNetCommandTypeAsString(cmdType),
						(unsigned)msg->getPlayerID(), isForgedDirect(msg) ? "did not come from that peer" : "only the host sends files");
					fflush(stderr);
				}
				return TRUE;
			}
			if (cmdType == NETCOMMANDTYPE_FILE) {
				processFile((NetFileCommandMsg *)msg);
			} else if (cmdType == NETCOMMANDTYPE_FILEANNOUNCE) {
				processFileAnnounce((NetFileAnnounceCommandMsg *)msg);
			} else {
				processFileProgress((NetFileProgressCommandMsg *)msg);
			}
			return TRUE;

		case NETCOMMANDTYPE_FRAMERESENDREQUEST:
			processFrameResendRequest((NetFrameResendRequestCommandMsg *)msg);
			return TRUE;

		default:
			return FALSE;
	}
}

void ConnectionManager::processFrameResendRequest(NetFrameResendRequestCommandMsg *msg) {
	// first make sure this is a valid slot
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}

	// make sure this player is still in our game.
	if ((m_connections[playerID] == nullptr) || (m_connections[playerID]->isQuitting() == TRUE)) {
		return;
	}

	// GeneralsX: The request must come from the peer it names. The answer is
	// up to FRAMES_TO_KEEP frames of every other slot's FRAMEINFOs plus their commands, all ack-required,
	// sent DIRECT to the named slot - so a request forged in another slot's name reflected that burst at
	// a third peer. A real one is always sent direct by its requester (requestFrameDataResend ->
	// sendLocalCommandDirect, CommandRequiresDirectSend) and is never relayed (processNetCommand returns
	// TRUE for it), so its datagram came from that peer; same binding the disconnect range uses.
	// doRelay has already declined to ACK a forged one (isForgedDirect); this refuses the resend.
	if (!arrivedFrom(playerID)) {
		return;
	}

	sendFrameDataToPlayer(playerID, msg->getFrameToResend());
}

/**
 * We have received a wrapper for a command too big to fit in a packet.
 */
void ConnectionManager::processWrapper(NetCommandRef *ref)
{
	NetWrapperCommandMsg *wrapperMsg = (NetWrapperCommandMsg *)(ref->getCommand());
	UnsignedShort commandID = wrapperMsg->getWrappedCommandID();
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - wrapped commandID is %d, commandID is %d",
		commandID, wrapperMsg->getID()));
	Int origProgress = 0;
	FileCommandMap::iterator fcIt = s_fileCommandMap.find(commandID);
	if (fcIt != s_fileCommandMap.end())
	{
		origProgress = s_fileProgressMap[m_localSlot][commandID];
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - origProgress[%d] == %d for command %d",
		m_localSlot, origProgress, commandID));

	// GeneralsX: The datagram's source keys the reassembly, so one peer's chunks never reach
	// another peer's node - see NetCommandWrapperList::processWrapper.
	// GeneralsX: ...and is acked by WRAPPERACK only once admitted (design v16 section 4): a refused chunk
	// (outside the source's window, or a new serialization over its soft cap) is not, so its sender retries it later; a dropped
	// one had no connected source to ack.
	const UnsignedInt source = arrivalSlot();
	const GXWrapVerdict verdict = m_netCommandWrapperList->processWrapper(ref, (Int)source, isOverSoftCap((Int)source));
	if (verdict == GXWRAP_ACK) {
		sendWrapperAck((Int)source, wrapperMsg);
	}

	if (fcIt != s_fileCommandMap.end())
	{
		Int newProgress = m_netCommandWrapperList->getPercentComplete((Int)source, wrapperMsg->getWrapSeq());
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - newProgress[%d] == %d for command %d",
			m_localSlot, newProgress, commandID));
		if (newProgress > origProgress && newProgress < 100)
		{
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - sending a NetFileProgressCommandMsg"));
			s_fileProgressMap[m_localSlot][commandID] = newProgress;

			RelayMaskType progressMask = ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot));
			NetFileProgressCommandMsg *msg = newInstance(NetFileProgressCommandMsg);
			msg->setPlayerID(m_localSlot);
			msg->setID(0);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()))
			{
				msg->setID(GenerateNextCommandID());
			}
			msg->setFileID(commandID);
			msg->setProgress(newProgress);
			sendLocalCommand(msg, progressMask);
			processFileProgress(msg);
			msg->detach();
		}
	}
}

/**
 * A client has sent us their run ahead metrics, lets store them away for future calculations.
 */
void ConnectionManager::processRunAheadMetrics(NetRunAheadMetricsCommandMsg *msg)
{
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (isPlayerConnected(playerID)) {
		m_latencyAverages[playerID] = msg->getAverageLatency();
		m_fpsAverages[playerID] = msg->getAverageFps();
		//DEBUG_LOG(("ConnectionManager::processRunAheadMetrics - player %d, fps = %d, latency = %f", playerID, msg->getAverageFps(), msg->getAverageLatency()));
		if (m_fpsAverages[playerID] > 100) {
			// limit the reported frame rate average to 100.  This is done because if a
			// user alt-tab's out of the game their frame rate climbs to in the neighborhood of
			// 300, that was deemed "ugly" by the powers that be.
			m_fpsAverages[playerID] = 100;
		}
	}
}

// ---------------------------------------------------------------------------
// GeneralsX: THE ROUTER PUBLISHES ITS ROW; EVERY PEER RENDERS IT.
//
// WHY THIS EXISTS AT ALL. The in-game topology is a star. A non-router peer exchanges timed
// traffic with the packet router and with nobody else (sendLocalCommand's else branch puts the
// per-frame FRAMEINFO on the router's connection alone), so it can measure exactly ONE number:
// its own round trip to the host. The router, by contrast, has every peer's leg, because its own
// FRAMEINFO is fanned out to every connection and every peer acks it. A per-player ping column is
// therefore not a rendering problem - the data does not exist on a client until somebody sends
// it. This is that send. It is also exactly the wire change electPacketRouter's note specified.
//
// WHAT IT IS NOT. It is not a latency MATRIX. Nobody measures d(i, j) between two non-router
// peers and this does not change that; every figure published here is a leg to the router, which
// is the only thing anybody measured. The scoreboard is honest about this because that is what a
// ping column means in a star: your latency to the host.
//
// WHY IT CANNOT PERTURB THE SIMULATION, structurally rather than by assertion:
//   * The values land in m_peerPingMs, which is a different array from m_latencyAverages. The
//     two pacing inputs, getMaximumLatency() and getMinimumFps(), read m_latencyAverages and
//     m_fpsAverages and are called from exactly one place each - the router-only branch of
//     updateRunAhead. Neither reads m_peerPingMs. Nothing else does either, bar the display
//     accessor.
//   * NETCOMMANDTYPE_LATENCYREPORT is absent from IsCommandSynchronized, so it never enters
//     frame data and never gates allCommandsReady. It is processed on arrival and discarded.
//   * It is absent from CommandRequiresAck and DoesCommandRequireACommandID, so it adds nothing
//     to the pending-command or retransmit machinery. A lost report costs one stale interval.
// ---------------------------------------------------------------------------
void ConnectionManager::publishLatencyReport()
{
	// Only the centre of the star has anything to say.
	if (m_localSlot != m_packetRouterSlot) {
		return;
	}

	NetLatencyReportCommandMsg *msg = newInstance(NetLatencyReportCommandMsg);
	msg->setPlayerID(m_localSlot);

	const time_t curTime = timeGetTime();
	Bool anyLive = FALSE;

	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		UnsignedShort ms = 0;

		// The router's own slot has no round trip to itself, and a peer we are not connected to
		// has none either. Both stay 0, which is the "no figure" sentinel rather than a fast link.
		if ((slot != (Int)m_localSlot) && isPlayerConnected(slot)) {
			const Real seconds = m_frameMetrics.getPeerLatency(slot);
			if (seconds > 0.0f) {
				// Clamped into the wire's range on the way OUT as well as on the way in. A local
				// measurement should never be out of range, but the receiving side's clamp is the
				// one that matters and this keeps the two ends agreeing about what is
				// representable instead of relying on a silent narrowing cast.
				const Real msReal = seconds * 1000.0f;
				ms = (UnsignedShort)clamp<Int>(1, (Int)(msReal + 0.5f), 65535);
				anyLive = TRUE;
			}
		}

		msg->setLatencyMs(slot, ms);

		// GeneralsX: The fps the router holds for this slot - the same figure the run-ahead clamp
		// reads (getMinimumFps), read here and never written. Its own slot's is set just after this call (updateRunAhead),
		// so the router reads its fresh average directly; a client's is the one it last sent (processRunAheadMetrics).
		if (slot == (Int)m_localSlot) {
			msg->setAverageFps(slot, m_frameMetrics.getAverageFPS());
		} else if (isPlayerConnected(slot)) {
			msg->setAverageFps(slot, m_fpsAverages[slot]);
			// A remote peer's fps is something to publish on its own: on loopback or a fast LAN every round trip can
			// measure 0 ms, and the row would then never go out.
			if (m_fpsAverages[slot] >= 0) {
				anyLive = TRUE;
			}
		}
	}

	// Nothing measured yet - stay quiet rather than broadcast an all-zero row every interval for
	// the first seconds of every match. (A remote fps figure counts as measured - see the loop.)
	if (anyLive) {
		// The router's own copy, so the host's scoreboard does not depend on hearing itself.
		for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
			const UnsignedShort ms = msg->getLatencyMs(slot);
			if (ms != 0) {
				m_peerPingMs[slot] = ms;
				m_peerPingTime[slot] = curTime;
			}
		}

		// ALL_SLOTS_MASK is "every slot"; sendLocalCommand's router branch turns that into one send
		// per open connection with the relay mask narrowed to that peer, so no peer re-relays it.
		//
		// GeneralsX: Was a literal 0xff. The NetworkDefs.h note claims
		// ALL_SLOTS_MASK replaced every one of those; this is the one it missed, and it is the last
		// numeric relay mask in the tree. As a RelayMaskType, 0xff is 0x00FF - so the latency report
		// addressed slots 0-7 and never slots 8-11, and four players in a twelve-player game would
		// have had no ping row for anyone and no one would have had a row for them.
		sendLocalCommand(msg, ALL_SLOTS_MASK);
	}

	msg->detach();
}

/**
 * The packet router has published its latency row. Display only - see publishLatencyReport.
 */
void ConnectionManager::processLatencyReport(NetLatencyReportCommandMsg *msg)
{
	// THIS ARRIVED OVER THE WIRE FROM A PEER THAT MAY BE LYING OR CORRUPT, so everything below
	// treats it as input rather than as fact.
	//
	// Only the packet router has any business publishing this. Without that test any peer could
	// overwrite the whole lobby's ping column at will - a cosmetic attack, but a free one, and
	// the same check the router-directed metrics path gets by construction (clients only ever
	// send those TO the router). A report from anyone else is dropped silently: it is either a
	// stale in-flight message from just before a router failover, or someone playing games, and
	// neither deserves a log line every interval.
	if (msg->getPlayerID() != m_packetRouterSlot) {
		return;
	}

	// The router does not take its row off the wire - it has the measurements. This also means a
	// forged report cannot reach the host at all.
	if (m_localSlot == m_packetRouterSlot) {
		return;
	}

	const time_t curTime = timeGetTime();

	// GeneralsX: [GXFPS] the row as received (GX_NET only; one line per report, every 5 s): the
	// harness's proof that a client holds every slot's figure.
	if (s_gxResendTrace) {
		char row[MAX_SLOTS * 12 + 1] = "";
		size_t n = 0;
		for (Int slot = 0; slot < MAX_SLOTS && n < sizeof(row); ++slot) {
			const Int fps = msg->getAverageFps(slot);
			if (fps >= 0) {
				const int w = snprintf(row + n, sizeof(row) - n, " s%d=%d", (int)slot, (int)fps);
				n += (w > 0) ? (size_t)w : 0;
			}
		}
		fprintf(stderr, "[GXFPS] router row:%s\n", row);
	}

	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		// GeneralsX: The fps row: the same sender check above, the same slot test below, the same
		// "-1 is not a figure" rule. This peer's OWN slot is kept too - it is the router's view of this peer.
		const Int fps = msg->getAverageFps(slot);
		if ((fps >= 0) && isPlayerConnected(slot)) {
			m_peerFps[slot] = (Short)fps;
			m_peerFpsTime[slot] = curTime;
		}

		const UnsignedShort ms = msg->getLatencyMs(slot);

		// 0 is the "no figure" sentinel and must NOT be stored as a measurement - storing it
		// would render as a dash anyway, but it would also refresh the timestamp and so keep a
		// slot alive that the router is telling us it knows nothing about.
		if (ms == 0) {
			continue;
		}

		// A slot the local peer does not believe is playing is ignored rather than displayed.
		if (!isPlayerConnected(slot)) {
			continue;
		}

		m_peerPingMs[slot] = ms;
		m_peerPingTime[slot] = curTime;
	}
}

/**
 * Round trip to this slot in milliseconds for the in-game player list, or -1 for "no live figure".
 */
Int ConnectionManager::getPeerPingMs(Int slot)
{
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1;
	}

	if (m_peerPingMs[slot] == 0) {
		return -1;	// never reported, or reported as unknown
	}

	// A figure that stopped being refreshed must LEAVE the scoreboard rather than sit there
	// frozen. Same failure mode and same window as FrameMetrics::getWorstPeerLatency: a peer that
	// has been silent longer than the disconnect timeout is the disconnect UI's business, not the
	// ping column's. Floored so a small INI value cannot expire a peer that is merely slow.
	//
	// The window has to be generous relative to the PUBLISH interval as well as the sample
	// interval, because on a client this figure is only as fresh as the router's last broadcast -
	// m_networkRunAheadMetricsTime apart, not once per frame.
	time_t staleWindow = (time_t)TheGlobalData->m_networkDisconnectTime;
	const time_t publishFloor = (time_t)TheGlobalData->m_networkRunAheadMetricsTime * 3;
	if (staleWindow < publishFloor) {
		staleWindow = publishFloor;
	}
	if (staleWindow < 2000) {
		staleWindow = 2000;
	}

	if ((timeGetTime() - m_peerPingTime[slot]) > staleWindow) {
		return -1;
	}

	return (Int)m_peerPingMs[slot];
}

void ConnectionManager::processDisconnectChat(NetDisconnectChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
	} else if (isPlayerConnected(playerID)) {
		name = m_connections[playerID]->getUser()->GetName();
	}

	// GeneralsX: The NAME is remote text too, and it is drawn on the same
	// row as the message - so a bidi override in a player name reorders the row exactly as one in
	// the body would. The body arrives already sanitised (NetPacket::readChatMessage); the name
	// comes from the lobby roster and has not been through that path.
	GXSanitizeChatText(name);

	UnicodeString body = msg->getText();
	if (body.isEmpty()) {
		return;
	}

	// GeneralsX: The
	// separator comes from GXLobbySaid, which is where every chat renderer in the tree now gets it -
	// see the block on its declaration. The bracket decoration is the in-match idiom and stays;
	// what it never was is a separator, and a LAN slot name ending in ':' does not take a second one.
	UnicodeString decorated;
	decorated.format(L"[%ls]", name.str());
	unitext = GXLobbySaid(decorated, body);
	// Never log the body. See the note in sendChat.
	TheDisconnectMenu->showChat(unitext); // <-- need to implement this
}

void ConnectionManager::processChat(NetChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	//DEBUG_LOG(("processChat(): playerID = %d", playerID));
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
		//DEBUG_LOG(("connection is null, using %ls", name.str()));
	} else if ((m_connections[playerID] != nullptr) && (m_connections[playerID]->isQuitting() == FALSE)) {
		name = m_connections[playerID]->getUser()->GetName();
		//DEBUG_LOG(("connection is non-null, using %ls", name.str()));
	}

	// GeneralsX: See processDisconnectChat: the speaker's NAME is remote
	// text on the same drawn row as the body, so it gets the same treatment. The body itself was
	// already cleaned at the wire boundary in NetPacket::readChatMessage - and if that cleaning left
	// nothing, there is no row worth drawing and no sound worth playing.
	GXSanitizeChatText(name);

	const UnicodeString body = msg->getText();
	if (body.isEmpty()) {
		return;
	}

	// GeneralsX: The
	// separator comes from GXLobbySaid, which is where every chat renderer in the tree now gets it -
	// see the block on its declaration. The bracket decoration is the in-match idiom and stays;
	// what it never was is a separator, and a LAN slot name ending in ':' does not take a second one.
	UnicodeString decorated;
	decorated.format(L"[%ls]", name.str());
	unitext = GXLobbySaid(decorated, body);
	// Never log the body. See the note in sendChat.

	const Player *player = ThePlayerList->getPlayerFromSlotIndex(playerID);
	if (!player)
	{
		TheInGameUI->message(L"%ls", unitext.str());
		return;
	}

	Bool fromObserver = !player->isPlayerActive();
	Bool amIObserver = !ThePlayerList->getLocalPlayer()->isPlayerActive();
	Bool canSeeChat = (amIObserver || !fromObserver) && !TheGameInfo->getConstSlot(playerID)->isMuted();

	if ( (gxSlotBit(m_localSlot) & msg->getPlayerMask() ) && canSeeChat  )
	{
		RGBColor rgb;
		rgb.setFromInt(player->getPlayerColor());
		TheInGameUI->messageColor(&rgb, L"%ls", unitext.str());

		// feedback for received chat messages in-game
		AudioEventRTS audioEvent("GUICommunicatorIncoming");
		TheAudio->addAudioEvent(&audioEvent);
	}
}

void ConnectionManager::processFile(NetFileCommandMsg *msg)
{
#ifdef DEBUG_LOGGING
	UnicodeString log;
	log.format(L"Saw file transfer: '%hs' of %d bytes from %d", msg->getPortableFilename().str(), msg->getFileLength(), msg->getPlayerID());
	DEBUG_LOG(("%ls", log.str()));
#endif

	// GeneralsX: Write only the transfer the host announced to us: the FILE carries
	// the announced ID (sendFile is handed sendFileAnnounce's fileID) and name, and our slot is in the
	// announced recipient mask. Anything else is dropped without a progress report - for a transfer that
	// excludes us the announce already counts us as done (100), and nobody is waiting on one never announced.
	// The host sends the announce and waits for every recipient's ack (areAllQueuesEmpty) before the FILE, and a
	// recipient's ack cannot leave before it has processed the announce: doRelay queues the ack (ackCommand ->
	// Connection::sendNetCommandMsg only queues), runs processNetCommand, and the queue is sent afterwards. So an
	// honest FILE always finds its announce here.
	{
		FileCommandMap::const_iterator announced = s_fileCommandMap.find(msg->getID());
		FileMaskMap::const_iterator recipients = s_fileRecipientMaskMap.find(msg->getID());
		const Bool nameOk = (announced != s_fileCommandMap.end()) && (announced->second == msg->getPortableFilename());
		const Bool forUs = (recipients != s_fileRecipientMaskMap.end()) && (m_localSlot < MAX_SLOTS)
			&& ((recipients->second & (gxSlotBit(m_localSlot))) != 0);
		if (!nameOk || !forUs) {
			static Int s_unannounced = 0;	// Trade-off: capped so a flood cannot become a flood of fflush
			if (s_unannounced < 32) {
				++s_unannounced;
				fprintf(stderr, "[GXFILE] REFUSED file '%s' (command %d): %s\n", msg->getPortableFilename().str(),
					(Int)msg->getID(), nameOk ? "this peer is not a recipient" : "no announce for that ID and name");
				fflush(stderr);
			}
			return;
		}
	}

	// GeneralsX: The two refusal paths below used to bare-return, which
	// told the sender NOTHING - the security comment even said "let the transfer time out". That
	// starves the whole lobby for the full per-file timeout on a file this peer decided in
	// microseconds it would never store. Refusing the WRITE is the security property; refusing to
	// ANSWER is not part of it. Both paths now fall through to the same failure report the
	// content-validation refusal already uses, so the sender aborts at once instead.
	AsciiString realFileName = msg->getRealFilename();
	const Bool nameOk = !realFileName.isEmpty();
	if (!nameOk)
	{
		// TheSuperHackers @security slurmlord 18/06/2025 As the file name/path from the NetFileCommandMsg failed to normalize,
		// in other words is bogus and points outside of the approved target directory, avoid an arbitrary file overwrite vulnerability
		// by not writing it.
		DEBUG_LOG(("Got a file name transferred that failed to normalize: '%s'!", msg->getPortableFilename().str()));
		fprintf(stderr, "[GXFILE] REFUSED transferred file '%s' - name failed to normalize; reporting failure to the sender\n",
			msg->getPortableFilename().str());
		fflush(stderr);
	}

	// TheSuperHackers @security bobtista 06/11/2025 Validate file extension to prevent arbitrary file types
	const Bool extOk = nameOk && hasValidTransferFileExtension(realFileName);
	if (nameOk && !extOk)
	{
		DEBUG_LOG(("File '%s' has invalid extension for transfer operations.", realFileName.str()));
		fprintf(stderr, "[GXFILE] REFUSED transferred file '%s' - invalid extension; reporting failure to the sender\n",
			realFileName.str());
		fflush(stderr);
	}

	if (extOk && TheFileSystem->doesFileExist(realFileName.str()))
	{
		DEBUG_LOG(("File exists already!"));
		//return;
	}

	UnsignedByte *buf = msg->getFileData();
	Int len = msg->getFileLength();

	// uncompress Targas
#ifdef COMPRESS_TARGAS
	Bool deleteBuf = FALSE;
	if (extOk && msg->getFilename().endsWith(".tga") && CompressionManager::isDataCompressed(buf, len))
	{
		Int uncompLen = CompressionManager::getUncompressedSize(buf, len);
		UnsignedByte *uncompBuffer = NEW UnsignedByte[uncompLen];
		Int actualLen = CompressionManager::decompressData(buf, len, uncompBuffer, uncompLen);
		if (actualLen == uncompLen)
		{
			DEBUG_LOG(("Uncompressed Targa after map transfer"));
			deleteBuf = TRUE;
			buf = uncompBuffer;
			len = uncompLen;
		}
		else
		{
			DEBUG_LOG(("Failed to uncompress Targa after map transfer"));
			delete[] uncompBuffer; // failed to decompress, so just use the source
		}
	}
#endif // COMPRESS_TARGAS

	// TheSuperHackers @security bobtista 12/02/2026 Validate file content in memory before writing to disk
	//
	// GeneralsX: This used to `return` outright, which sent the host NOTHING
	// - so a rejected file left the host's transfer loop spinning until its two-minute timeout with
	// no indication of why. Fall through to the progress report instead, so a refused file is
	// reported as a failure exactly like an unwritable one. The buffer cleanup this used to duplicate
	// already runs at the end of the function.
	const Bool contentOk = extOk && hasValidTransferFileContent(realFileName, buf, len);
	if (extOk && !contentOk)
	{
		DEBUG_LOG(("File '%s' failed content validation. Transfer aborted.", realFileName.str()));
	}

	// GeneralsX: Report the truth. This reported a flat 100 below whether or
	// not the write succeeded, so a peer that stored nothing still told the host "done" - and the
	// DEBUG_LOG that was the only sign of trouble is ((void)0) in every shipping build. See
	// GX_FILE_PROGRESS_FAILED in networkutil.h for the full account of the symptom that caused.
	Bool wroteOk = FALSE;

	if (contentOk)
	{
		File *fp = TheFileSystem->openFile(realFileName.str(), File::CREATE | File::BINARY | File::WRITE);
		if (fp)
		{
			wroteOk = (fp->write(buf, len) == len);
			fp->close();
			fp = nullptr;
			if (wroteOk)
			{
				DEBUG_LOG(("Wrote %d bytes to file %s!", len, realFileName.str()));
			}
			else
			{
				DEBUG_LOG(("ConnectionManager::processFile - short write to '%s'", realFileName.str()));
			}
		}
		else
		{
			DEBUG_LOG(("Cannot open file!"));
		}
	}

	// Survives a release build, unlike the DEBUG_LOGs above. A failed map transfer is exactly the
	// kind of thing an operator has to be able to see from a log after the fact, and the house
	// pattern for that in this engine is fprintf(stderr) (INI.cpp:196-197, SimulationId.cpp:663).
	if (!wroteOk)
	{
		fprintf(stderr, "[GXFILE] FAILED to write '%s' (%d bytes) - reporting failure to the host\n",
			nameOk ? realFileName.str() : msg->getPortableFilename().str(), len);
		fflush(stderr);
	}

	// GeneralsX: Narrate each received file: yellow [LOG] chat for the
	// room (visible the moment the transfer screen lifts), [GXLOG] stderr for the harnesses.
	if (wroteOk && TheLAN != nullptr)
	{
		UnicodeString leaf;
		leaf.translate(GetFileFromPath(msg->getPortableFilename()));
		UnicodeString logLine;
		logLine.format(L"received %ls (%d bytes)", leaf.str(), len);
		fprintf(stderr, "[GXLOG] %ls\n", logLine.str());
		fflush(stderr);
		TheLAN->OnChat(UnicodeString(L"LOG"), 0, logLine, LANAPIInterface::LANCHAT_LOG);
	}

	DEBUG_LOG(("ConnectionManager::processFile() - sending a NetFileProgressCommandMsg"));

	Int commandID = msg->getID();
	Int newProgress = wroteOk ? 100 : GX_FILE_PROGRESS_FAILED;

	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXMAPDBG] processFile: cmdID=%d real='%s' contentOk=%d wroteOk=%d len=%d -> reporting %d\n",
			commandID, realFileName.str(), contentOk ? 1 : 0, wroteOk ? 1 : 0, len, newProgress);
		fflush(stderr);
	}

	s_fileProgressMap[m_localSlot][commandID] = newProgress;

	RelayMaskType progressMask = ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot));
	NetFileProgressCommandMsg *progressMsg = newInstance(NetFileProgressCommandMsg);
	progressMsg->setPlayerID(m_localSlot);
	progressMsg->setID(0);
	if (DoesCommandRequireACommandID(progressMsg->getNetCommandType()))
	{
		progressMsg->setID(GenerateNextCommandID());
	}
	progressMsg->setFileID(commandID);
	progressMsg->setProgress(newProgress);
	sendLocalCommand(progressMsg, progressMask);
	processFileProgress(progressMsg);
	progressMsg->detach();

#ifdef COMPRESS_TARGAS
	if (deleteBuf)
	{
		delete[] buf;
		buf = nullptr;
	}
#endif // COMPRESS_TARGAS
}

void ConnectionManager::processFileAnnounce(NetFileAnnounceCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileAnnounce() - expecting '%s' (%s) in command %d", msg->getPortableFilename().str(), msg->getRealFilename().str(), msg->getFileID()));
	// GeneralsX @debug /08/2026 GX_MAPDBG: A1 measurement probe - the realFilename stored
	// here is the string every later getFileTransferProgress(path) poll has to match exactly.
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXMAPDBG] processFileAnnounce: fileID=%d portable='%s' real='%s' mask=%llX\n",
			(Int)msg->getFileID(), msg->getPortableFilename().str(), msg->getRealFilename().str(), (unsigned long long)msg->getPlayerMask());
		fflush(stderr);
	}
	// GeneralsX: A retransmitted announce (our ack was lost) used to
	// re-run the stores below, and the reseed zeroed every mask slot's progress - including our
	// own, which disarmed processWrapper's newProgress > origProgress guard and let ghost-node
	// chunk counts (1,2,3...) go back on the wire after a completed transfer (the host-side
	// "100 -> 1,2,3" bursts of the 2026-08-10 session; its sticky max defused them, but the
	// traffic and the misleading trace remain). A true retransmit carries the same fileID AND
	// the same portable name - drop it with the progress intact. A matching fileID with a
	// DIFFERENT name is 16-bit ID reuse on a genuinely new transfer: fall through and reseed.
	FileCommandMap::iterator existingIt = s_fileCommandMap.find(msg->getFileID());
	if (existingIt != s_fileCommandMap.end())
	{
		if (existingIt->second == msg->getPortableFilename())
		{
			if (s_mapDbg)
			{
				fprintf(stderr, "[GXXFER] t=%u RE-ANNOUNCE fileID=%d portable='%s' ownProgress=%d -> duplicate, progress preserved\n",
					GXWallClockMs(), (Int)msg->getFileID(), msg->getPortableFilename().str(),
					s_fileProgressMap[m_localSlot][msg->getFileID()]);
				fflush(stderr);
			}
			return;
		}
		if (s_mapDbg)
		{
			fprintf(stderr, "[GXXFER] t=%u RE-ANNOUNCE fileID=%d portable='%s' (was '%s') -> ID reuse, reseeding\n",
				GXWallClockMs(), (Int)msg->getFileID(), msg->getPortableFilename().str(),
				existingIt->second.str());
			fflush(stderr);
		}
	}
	// GeneralsX: Store the PORTABLE name, not a reconstructed real path.
	// The progress poll in doFileTransfer compares against this entry by string, and the real
	// path it polls with is built locally from getMap() - which does not have to spell the same
	// path the same way. Measured live 2026-08-10 (GX_MAPDBG, Linux relay harness): the host
	// polled the lowercased MapCache-key spelling while this entry held the mixed-case round-trip
	// through portableMapPathToRealMapPath, differing by CASE alone - so every poll missed, the
	// miss path returns 0, and the host timed out after 60 s and bounced to the lobby while the
	// transfer had completed 59 s earlier. The portable name is the one string both ends provably
	// share (it crossed the wire, canonical lowercase-backslash form), so the poll side converts
	// its path to portable and compares in that space instead - see getFileTransferProgress.
	s_fileCommandMap[msg->getFileID()] = msg->getPortableFilename();
	s_fileRecipientMaskMap[msg->getFileID()] = msg->getPlayerMask();
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if ( gxSlotBit(i) & msg->getPlayerMask() )
		{
			s_fileProgressMap[i][msg->getFileID()] = 0;
		}
		else
		{
			s_fileProgressMap[i][msg->getFileID()] = 100; // they don't need to get it, so they're already done.
		}
	}
}

void ConnectionManager::processFileProgress(NetFileProgressCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileProgress() - command %d is at %d%%",
		msg->getFileID(), msg->getProgress()));

	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	const UnsignedShort fileID = msg->getFileID();
	const Int oldProgress = s_fileProgressMap[playerID][fileID];
	const Int newProgress = msg->getProgress();

	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg && newProgress != oldProgress)
	{
		fprintf(stderr, "[GXMAPDBG] processFileProgress: player=%u fileID=%d %d -> %d\n",
			playerID, (Int)fileID, oldProgress, newProgress);
		fflush(stderr);
	}

	// GeneralsX: The failure sentinel has to survive this aggregation, and
	// has to be sticky once set.
	//
	// This is a MONOTONIC max, and processFileAnnounce seeds the entry with 0, so max(0, -1) is 0 -
	// GX_FILE_PROGRESS_FAILED would be silently swallowed here and the host would wait out its full
	// two-minute timeout exactly as before. The failure path would have been dead code that read as
	// live, which is the failure mode where a gate is never observed to fire because it cannot.
	// Caught by cross-model review; the sentinel was inert as first written.
	//
	// Sticky in both directions: once a peer has reported it cannot store the file, a later stale or
	// retransmitted progress packet must not resurrect it into a partial success.
	if (newProgress == GX_FILE_PROGRESS_FAILED || oldProgress == GX_FILE_PROGRESS_FAILED)
	{
		s_fileProgressMap[playerID][fileID] = GX_FILE_PROGRESS_FAILED;
		return;
	}

	s_fileProgressMap[playerID][fileID] = max(oldProgress, newProgress);
}

void ConnectionManager::processProgress( NetProgressCommandMsg *msg )
{
	TheGameLogic->processProgress(msg->getPlayerID(), msg->getPercentage());
}

void ConnectionManager::processLoadComplete( NetCommandMsg *msg )
{
	TheGameLogic->processProgressComplete(msg->getPlayerID());
}

void ConnectionManager::processTimeOutGameStart( NetCommandMsg *msg )
{
	TheGameLogic->timeOutGameStart();
}

/**
 * Another client has sent us the command count for a new frame.
 */
void ConnectionManager::processFrameInfo(NetFrameCommandMsg *msg) {
	//stupid frame info, why don't you process yourself?

	const UnsignedInt playerID = msg->getPlayerID();

	if (playerID < MAX_SLOTS) {
		if (m_frameData[playerID] != nullptr) {
//			DEBUG_LOG(("ConnectionManager::processFrameInfo - player %d, frame %d, command count %d, received on frame %d", playerID, msg->getExecutionFrame(), msg->getCommandCount(), TheGameLogic->getFrame()));
			// GeneralsX: The first copy of a FRAMEINFO samples the arrival cushion here:
			// processNetCommand clears our relay bit before sendRemoteCommand sees it, so there it never counts as fresh,
			// and an idle match (FRAMEINFOs only) would sample nothing. See sampleArrivalCushion.
			if (m_frameData[playerID]->setFrameCommandCount(msg->getExecutionFrame(), msg->getCommandCount())) {
				sampleArrivalCushion(msg);
			}
		}
	}
}

/**
 * We just got a stage 1 ack from someone.  So we should remove it from the connection that sent it so
 * it doesn't keep resending it.
 */
void ConnectionManager::processAckStage1(NetCommandMsg *msg) {
#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	const UnsignedInt playerID = msg->getPlayerID();
	NetCommandRef *ref = nullptr;

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processAck - processing ack for command %d from player %d", ((NetAckStage1CommandMsg *)msg)->getCommandID(), playerID));
	}
#endif

	if (playerID < MAX_SLOTS) {
		if (m_connections[playerID] != nullptr) {
			ref = m_connections[playerID]->processAck(msg);
		}
	} else {
		DEBUG_CRASH(("ConnectionManager::processAck - %d is an invalid player number", playerID));
	}

	if (ref != nullptr) {
		if (s_gxResendTrace)
			gxNoteResendAcked(playerID, ref->getCommand(), arrivalSlot());
		// GeneralsX: Only OUR OWN FRAMEINFOs are a latency sample. The router
		// also queues every relayed FRAMEINFO (sendRemoteCommand) and every one sendSingleFrameToPlayer
		// resends on another slot's behalf, and the ack for those comes back here too - matched by
		// (id, originator) in Connection::processAck. FrameMetrics times it against m_pendingLatencies,
		// which holds OUR stamp for that frame, so the sample was originator's lag + originator->router
		// + router->acker->router, and it landed in the same frame%L slot as our own sample and usually
		// overwrote it. With N humans the line ran (N-1)^2 times per frame instead of N-1, and the
		// router's run-ahead input (m_latencyAverages[m_localSlot], updateRunAhead) was inflated in
		// every 3+ player match. 2-player matches relay nothing and are unchanged.
		// GeneralsX: ...and not one sendSingleFrameToPlayer RESENT: that copy of an old frame carries
		// a fresh ID, and m_pendingLatencies holds the stamp of the frame's FIRST send (or of a newer frame in the same
		// slot), so the sample read the stall the resend answered - seconds - into the run-ahead mean. Stock reached this
		// only when the resender was the acker's router; since ackCommand acks the hop, every resender would.
		if ((ref->getCommand()->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) &&
				(ref->getCommand()->getPlayerID() == m_localSlot) &&
				!((NetFrameCommandMsg *)(ref->getCommand()))->isResent()) {
			// GeneralsX: playerID - who sent this ack - is now passed down.
			// On the packet router this line is reached once per peer per frame, because
			// sendLocalCommand's router branch fans the FRAMEINFO out to every connection
			// individually and every peer acks it. Without the slot those samples were an
			// unattributable mixture and the display reduction over them returned the FASTEST peer.
			// See FrameMetrics::getWorstPeerLatency.
			m_frameMetrics.processLatencyResponse(((NetFrameCommandMsg *)(ref->getCommand()))->getExecutionFrame(), (Int)playerID);
		}

		deleteInstance(ref);
		ref = nullptr;
	}
}

/**
 * We just got a stage 2 ack from someone.  So remove it from the pending commands list so it doesn't
 * get sent in the case of a new packet router.
 */
void ConnectionManager::processAckStage2(NetCommandMsg *msg) {
	UnsignedShort commandID = 0;
	UnsignedByte playerID = 0;
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) {
		commandID = ((NetAckStage2CommandMsg *)msg)->getCommandID();
		playerID = ((NetAckStage2CommandMsg *)msg)->getOriginalPlayerID();
	} else if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		commandID = ((NetAckBothCommandMsg *)msg)->getCommandID();
		playerID = ((NetAckBothCommandMsg *)msg)->getOriginalPlayerID();
	} else {
		return;
	}

	NetCommandRef *ref = m_pendingCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - removing command %d from the pending commands list.", commandID));
		DEBUG_ASSERTCRASH((m_localSlot == playerID), ("Found a command in the pending commands list that wasn't originated by the local player"));
		m_pendingCommands->removeMessage(ref);
		deleteInstance(ref);
		ref = nullptr;
	} else {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - Couldn't find command %d from player %d in the pending commands list.", commandID, playerID));
	}

	// GeneralsX: msg->getPlayerID() is the acker's raw wire byte (0-255) and acks
	// skip processNetCommand's MAX_SLOTS check, so the shift below was UB for 32+ (arm64/x86 mask the
	// count: ID 33 cleared slot 1's bit). No real peer has a slot >= MAX_SLOTS.
	if (msg->getPlayerID() >= MAX_SLOTS) {
		return;
	}

	ref = m_relayedCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - found command ID %d from player %d in the relayed commands list.", commandID, playerID));
		// GeneralsX: RelayMaskType, not UnsignedByte: truncating here drops
		// slot 8+ from the outstanding-relay set, so a command still owed to the ninth peer looks
		// fully acked and the stage-2 ack goes back to the originator early.
		RelayMaskType prevRelay = ref->getRelay();
		RelayMaskType relay = prevRelay & ~(gxSlotBit(msg->getPlayerID()));
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay was %d and is now %d", relay, prevRelay));
		if (relay == 0) {
			//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay is 0, removing command from the relayed commands list."));
			m_relayedCommands->removeMessage(ref);
			NetAckStage2CommandMsg *ackmsg = newInstance(NetAckStage2CommandMsg)(ref->getCommand());
			sendLocalCommand(ackmsg, gxSlotBit(ackmsg->getOriginalPlayerID()));
			deleteInstance(ref);
			ref = nullptr;

			ackmsg->detach();
			ackmsg = nullptr;
		} else {
			ref->setRelay(relay);
		}
	}
}

/**
 * We just got a "both" ack from someone.  So process it as both a stage 1 and stage 2 ack.
 */
void ConnectionManager::processAck(NetCommandMsg *msg) {
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage1(msg);
	}
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage2(msg);
	}
}

/**
 * A player has just left our game. Delete their connection and frame data manager.
 * return codes are:
 * PLAYERLEAVECODE_UNKNOWN - player didn't have a valid slot number.
 * PLAYERLEAVECODE_CLIENT - someone in the game that wasn't us or the packet router.
 * PLAYERLEAVECODE_LOCAL - We are leaving the game, we could also be the packet router.
 * PLAYERLEAVECODE_PACKETROUTER - The packet router left the game.
 *
 * If we are leaving and are also the packet router, it will return the PLAYERLEAVECODE_LOCAL return code.
 */
PlayerLeaveCode ConnectionManager::processPlayerLeave(NetPlayerLeaveCommandMsg *msg) {
	UnsignedByte playerID = msg->getLeavingPlayerID();
	// GeneralsX @bugfix Validate the slot before indexing.
	//
	// getLeavingPlayerID() is a separate payload field from the already-validated
	// sender ID and carries any value 0-255, while m_connections/m_frameData hold
	// MAX_SLOTS entries. An out-of-range value read far past both arrays and then
	// called setQuitting()/setQuitFrame() through whatever word happened to be
	// there — a write at an attacker-influenced address, reachable from a network
	// packet. Every sibling handler in this file (processFrameResendRequest,
	// processRunAheadMetrics, processDisconnectChat, disconnectPlayer) already
	// guards this; processPlayerLeave was the one that did not.
	if (playerID >= MAX_SLOTS) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave() - rejecting out-of-range player id %d", playerID));
		return PLAYERLEAVECODE_UNKNOWN;
	}
	// GeneralsX: A player may only announce its OWN leave. The leaving slot
	// is a payload field and nothing tied it to the sender, so any peer could eject any other:
	// measured, a PLAYERLEAVE naming slot 2 sent from slot 1 ended slot 2's match on its own machine
	// (scripts/test/net-kill-gates.sh forge-leave). getPlayerID() is the slot whose frame data carried
	// this order - the same on every peer - and this runs at the same frame on all of them, so every
	// peer refuses the same forgery; it was already counted, so nobody waits for it.
	// What this stops is a client that names ITSELF as the sender. Both slots are packet fields and a
	// synchronised order cannot be bound to the transport the way the disconnect range is (see
	// processNetCommand): the router relays every other slot's orders and any peer may answer a frame
	// resend with them. So a router can still put a PLAYERLEAVE into the victim's own frame data, and
	// so may a peer that also forges the victim's frame count (not measured).
	//
	// AND THAT RESIDUAL IS NOT LIMITED TO PLAYERLEAVE, WHICH THIS NOTE USED TO IMPLY. The same forged
	// envelope carries a plain GAMECOMMAND, whose acting player is also just the packet's sender field
	// (NetGameCommandMsg::constructGameMessage builds "player%d" from getPlayerID()), and
	// MSG_SELF_DESTRUCT reaches Player::killPlayer() through GameLogic::onSelfDestruct with none of
	// the checks in this change in its path - they are scoped to DESTROYPLAYER, PLAYERLEAVE and the
	// disconnect range. It is the SHORTER kill path, not a longer one. Same precondition, so not a new
	// class: the forgery must also land in the victim's frame data with a matching command count. The
	// fix is the same one named there - authenticate the frame-data envelope per hop, or accept a
	// slot's GAMECOMMAND/FRAMEINFO only from that slot's own transport where it is directly reachable.
	// Until that exists, these gates narrow the kill paths; they do not close them.
	if (msg->getPlayerID() != playerID) {
		static Int s_refusals = 0;	// Trade-off: capped so a flood of forgeries cannot turn into a flood of fflush
		if (s_refusals < 32) {
			++s_refusals;
			fprintf(stderr, "[GXGATE] PLAYERLEAVE refused: slot %u named slot %u (frame %u)\n",
				(unsigned)msg->getPlayerID(), (unsigned)playerID, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
		return PLAYERLEAVECODE_UNKNOWN;
	}
	{
		static Int s_accepted = 0;	// a slot leaves once; capped all the same
		if (s_accepted < 32) {
			++s_accepted;
			fprintf(stderr, "[GXGATE] PLAYERLEAVE accepted: slot %u (frame %u)\n",
				(unsigned)playerID, (unsigned)TheGameLogic->getFrame());
			fflush(stderr);
		}
	}
	lockstepRemove(playerID);	// the executed leave moves the succession DESTROYPLAYER is judged by
	if ((playerID != m_localSlot) && (m_connections[playerID] != nullptr)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave() - setQuitting() on player %d on frame %d", playerID, TheGameLogic->getFrame()));
		m_connections[playerID]->setQuitting();
	}
	// GeneralsX @bugfix The assert below dereferenced m_frameData[playerID] before
	// the null test on the following line, so a null entry crashed in debug and the
	// check was pointless in release. Test first.
	DEBUG_ASSERTCRASH(m_frameData[playerID] == nullptr || m_frameData[playerID]->getIsQuitting() == FALSE,
	                  ("Player %d is already quitting", playerID));
	if ((playerID != m_localSlot) && (m_frameData[playerID] != nullptr) && (m_frameData[playerID]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave - setQuitFrame on player %d for frame %d", playerID, TheGameLogic->getFrame()+1));
		m_frameData[playerID]->setQuitFrame(TheGameLogic->getFrame() + FRAMES_TO_KEEP + 1);
	}

	if (playerID == m_localSlot)
	{
		// we're leaving, so mark our connections and frame datas to go away.
		for (Int i=0; i<MAX_SLOTS; ++i)
		{
			if (m_connections[i])
			{
				m_connections[i]->clearCommandsExceptFrom(m_localSlot);
				m_connections[i]->setQuitting();
			}
		}
	}

	PlayerLeaveCode code = disconnectPlayer(playerID);
	DEBUG_LOG(("ConnectionManager::processPlayerLeave() - just disconnected player %d with ret code %d", playerID, code));
	if (code == PLAYERLEAVECODE_PACKETROUTER)
		resendPendingCommands();

	PopulateInGameDiplomacyPopup();
	return code;
}

UnsignedInt ConnectionManager::getPacketRouterFallbackSlot(Int packetRouterNumber) {
	if ((packetRouterNumber >= 0) && (packetRouterNumber < MAX_SLOTS)) {
		return m_packetRouterFallback[packetRouterNumber];
	}
	return MAX_SLOTS;
}

UnsignedInt ConnectionManager::getPacketRouterSlot() {
	return m_packetRouterSlot;
}

// GeneralsX: A slot's connection goes, and with it everything the receiver still holds of that source
// (design v16 section 4, leave/reset): open nodes, records, its lowWater. Nothing of it can be acked or completed any more.
void ConnectionManager::dropConnection(Int slot) {
	deleteInstance(m_connections[slot]);
	m_connections[slot] = nullptr;
	if (m_netCommandWrapperList != nullptr) {
		m_netCommandWrapperList->freeSource(slot);
	}
	// GeneralsX: A dropped slot never acks again (its acks are refused without a connection), so its bit
	// must not hold a relay entry: an entry owed only to it kept its (player, 16-bit ID) key for the rest of the match, and once the
	// originator's ID wrapped, a newer command's entry could not be recorded and its recipients' acks cleared the old one instead -
	// no ACKSTAGE2 for it. Removed silently: an ACKSTAGE2 for an entry that old could alias a newer
	// pending command of the same ID. Base kept these entries (the same leak, on chunk keys).
	if (m_relayedCommands != nullptr) {
		NetCommandRef *ref = m_relayedCommands->getFirstMessage();
		while (ref != nullptr) {
			NetCommandRef *next = ref->getNext();
			const RelayMaskType relay = ref->getRelay() & ~(RelayMaskType)(gxSlotBit(slot));
			if (relay == 0) {
				m_relayedCommands->removeMessage(ref);
				deleteInstance(ref);
			} else {
				ref->setRelay(relay);
			}
			ref = next;
		}
	}
}

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: GX_NET_TEST_WRAPFLOOD (see Connection::gxTestWrapFlood): every live connection gets the flood,
// each naming its own recipient as the sender - a forgery every receiver can see is one.
void ConnectionManager::gxTestWrapFlood(UnsignedInt count) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_connections[i] != nullptr) && !m_connections[i]->isQuitting()) {
			m_connections[i]->gxTestWrapFlood(count, (UnsignedInt)i, (RelayMaskType)(gxSlotBit(i)));
		}
	}
}
#endif

Bool ConnectionManager::areAllQueuesEmpty() {
	Bool retval = TRUE;
	for (Int i = 0; (i < MAX_SLOTS) && retval; ++i) {
		if (m_connections[i] != nullptr) {
			if (m_connections[i]->isQueueEmpty() == FALSE) {
				//DEBUG_LOG(("ConnectionManager::areAllQueuesEmpty() - m_connections[%d] is not empty", i));
				//m_connections[i]->debugPrintCommands();
				retval = FALSE;
			}
		}
	}

	return retval;
}

Bool ConnectionManager::canILeave() {
	return areAllQueuesEmpty();
}

/**
 * The local player is leaving. Tell the local player as well as the other players
 * to remove this player at the specified frame.
 */
void ConnectionManager::handleLocalPlayerLeaving(UnsignedInt frame) {
	NetPlayerLeaveCommandMsg *msg = newInstance(NetPlayerLeaveCommandMsg);

	msg->setLeavingPlayerID(m_localSlot);
	msg->setExecutionFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	DEBUG_LOG(("ConnectionManager::handleLocalPlayerLeaving - Local player leaving on frame %d", frame));
	DEBUG_ASSERTCRASH(m_packetRouterSlot >= 0, ("ConnectionManager::handleLocalPlayerLeaving, packet router is %d, illegal value.", m_packetRouterSlot));

	sendLocalCommand(msg);

	msg->detach();
}

/**
 * We just got a message that needs to be ack'd, so ack it!
 */
void ConnectionManager::ackCommand(NetCommandRef *ref, UnsignedInt localSlot, UnsignedInt hopSlot) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandMsg *ackmsg;
	UnsignedShort commandID;
	UnsignedByte originalPlayerID;
	// GeneralsX: RelayMaskType, not UnsignedByte. This mask decides ack
	// BOTH vs ack STAGE 1. Truncated, a command whose only destination is slot 8+ yields 0 here and
	// is acked as BOTH - telling the originator it reached its final destination before it was ever
	// relayed, so it is never retransmitted and the ninth peer loses it permanently.
	RelayMaskType sendRelay = 0;

	// Make send relay a bitmask for the connections that the relay will actually be sent to. This is
	// necessary to determine whether or not we have to wait to send a stage 2 ack.
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			sendRelay = sendRelay | (gxSlotBit(i));
		}
	}

#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	sendRelay = sendRelay & ref->getRelay();
	if (sendRelay == 0) {
		NetAckBothCommandMsg *bothmsg = newInstance(NetAckBothCommandMsg)(ref->getCommand());
		ackmsg = bothmsg;
		commandID = bothmsg->getCommandID();
		originalPlayerID = bothmsg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack both for command %d from player %d", bothmsg->getCommandID(), bothmsg->getOriginalPlayerID()));
		}
#endif
	} else {
		NetAckStage1CommandMsg *stage1msg = newInstance(NetAckStage1CommandMsg)(ref->getCommand());
		ackmsg = stage1msg;
		commandID = stage1msg->getCommandID();
		originalPlayerID = stage1msg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack stage 1 for command %d from player %d", stage1msg->getCommandID(), stage1msg->getOriginalPlayerID()));
		}
#endif
	}

	ackmsg->setPlayerID(localSlot); // Tell the player who this ack is coming from.

	if (CommandRequiresDirectSend(msg) && CommandRequiresAck(msg)) {
		// Send this ack directly back to the sending player, don't go through the packet router.
		if (msg->getPlayerID() < MAX_SLOTS) {
			if (m_connections[msg->getPlayerID()] != nullptr) {
				m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, gxSlotBit(msg->getPlayerID()));
			}
		}
	} else {
		// GeneralsX: ACK THE HOP: a non-direct command is acked to the peer whose datagram
		// carried it. Every hop's sender keeps the command on its Connection queue, retransmitting, until the hop's
		// RECEIVER acks it (Connection::processAck, keyed on the command's ID and named slot). The stock rule - ack my
		// packet router, or, on the router, the slot the command names - names that sender on every ordinary path: a
		// non-router hears every relayed command from the router, and the router hears each command from the slot it
		// names. It names the wrong peer for a frame resent on another slot's behalf (sendSingleFrameToPlayer: the
		// disconnect catch-up and FRAMERESENDREQUEST), which the RESENDER sends direct with the original slot as its
		// player. The resender then heard only when it was the router itself, or when this ack happened to inherit a
		// mask naming it (an ack has no relay field) and the router forwarded it; when this peer WAS the router, never.
		// It retransmitted the frame for the rest of the match (
		// section 5; net-kill-gates.sh honest-handover). The misrouted ack also cancelled whatever REAL command shared its
		// (ID, slot) on the router's queue to us, or on the named slot's queue to us when we route - and a resent
		// FRAMEINFO's ID comes from the resender's counter, which runs about in step with every other peer's. So: ack whoever
		// sent the datagram. On every ordinary path that is the peer the stock rule named, so ordinary traffic is
		// unchanged. The one other direct sender of these types, sendLocalCommand's fallback for a peer with no
		// router connection, queues them on its own connections too, so it also needs the ack. A resend carries the
		// resender's own commands as well, and this ACKBOTH now clears them from its m_pendingCommands, as the
		// router-forwarded copy always did when it got through: harmless, since a resend covers only frames before
		// the resender's own, and every peer still missing them is behind and gets its own resend. Without a datagram - a reassembled wrapper, whose chunks were acked hop by hop as they arrived - the
		// stock rule stands.
		// GeneralsX: A reassembled command has no datagram; its hop is the slot its chunks came from
		// (processReadyWrapped passes it - design v16 residual 6.3, the same trade).
		const UnsignedInt from = (hopSlot < MAX_SLOTS) ? hopSlot : arrivalSlot();
		if ((from < MAX_SLOTS) && (m_connections[from] != nullptr)) {
			m_connections[from]->sendNetCommandMsg(ackmsg, gxSlotBit(from));
		} else if ((m_packetRouterSlot >= 0) && (m_packetRouterSlot < MAX_SLOTS)) {
			// The local connection may be the packet router, in that case, the connection would be null.  So do something about it!
			if (m_connections[m_packetRouterSlot] != nullptr) {
//				DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d to packet router.", commandID, m_packetRouterSlot));
				m_connections[m_packetRouterSlot]->sendNetCommandMsg(ackmsg, gxSlotBit(m_packetRouterSlot));
			} else if (m_localSlot == m_packetRouterSlot) {
				// we are the packet router, send the ack to the player that sent the command.
				if (msg->getPlayerID() < MAX_SLOTS) {
					if (m_connections[msg->getPlayerID()] != nullptr) {
//						DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d directly to player.", commandID, msg->getPlayerID()));
						m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, gxSlotBit(msg->getPlayerID()));
					} else {
	//					DEBUG_CRASH(("Connection to player is null"));
					}
				} else {
					DEBUG_CRASH(("Command sent by an invalid player ID."));
				}
			} else {
				DEBUG_CRASH(("Connection to packet router is null"));
			}
		} else {
			DEBUG_CRASH(("I don't know who the packet router is."));
		}
	}

	ackmsg->detach();
}

/**
 * This is where we relay a command from one client to others (including ourselves).
 * This should only be done by the current packet router.
 */
void ConnectionManager::sendRemoteCommand(NetCommandRef *msg) {
	if (msg->getCommand() == nullptr) {
		return;
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - sending net command %d of type %s from player %d, relay is 0x%llx",
		msg->getCommand()->getID(), GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), (unsigned long long)msg->getRelay()));

	const RelayMaskType relay = msg->getRelay();
	const UnsignedInt playerID = msg->getCommand()->getPlayerID();
	FrameDataManager *frameDataMgr = playerID < MAX_SLOTS ? m_frameData[playerID] : nullptr;
	Bool freshCopy = FALSE;	// the first copy of this command to reach our frame data - see the cushion below
	if ((relay & (gxSlotBit(m_localSlot))) && (frameDataMgr != nullptr)) {
		if (IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), msg->getCommand()->getExecutionFrame()));
			freshCopy = frameDataMgr->addNetCommandMsg(msg->getCommand());
		}
	}

	forwardOnConnections(msg, relay);

	// Do some metrics to find the minimum packet arrival cushion (a FRAMEINFO is sampled in processFrameInfo).
	if (freshCopy && IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
		sampleArrivalCushion(msg->getCommand());
	}
}

// GeneralsX: sendRemoteCommand's connection loop, split out so processReadyWrapped can forward a
// command it must not add to local frame data (design v16 section 5). It KEEPS the m_relayedCommands add: without it an
// out-of-date forward never produces the ACKSTAGE2 that clears the originator's m_pendingCommands.
void ConnectionManager::forwardOnConnections(NetCommandRef *msg, RelayMaskType relay) {
	// GeneralsX: RelayMaskType, not UnsignedByte. This records who the
	// command was actually relayed to. Truncated, a relay to slot 8+ is not recorded, so a command
	// sent only to the ninth peer leaves actualRelay == 0 and is never added to m_relayedCommands -
	// no ack tracking and no retransmission for exactly the peers the widening was meant to add.
	RelayMaskType actualRelay = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (gxSlotBit(i))) && ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - relaying command %d to player %d", msg->getCommand()->getID(), i));
			m_connections[i]->sendNetCommandMsg(msg->getCommand(), gxSlotBit(i));
			actualRelay = actualRelay | (gxSlotBit(i));
		}
	}

	if ((actualRelay != 0) && (CommandRequiresAck(msg->getCommand()) == TRUE)) {
		NetCommandRef *ref = m_relayedCommands->addMessage(msg->getCommand());
		if (ref != nullptr) {
			ref->setRelay(actualRelay);
			//DEBUG_LOG(("ConnectionManager::sendRemoteCommand - command %d added to relayed commands with relay %d", msg->getCommand()->getID(), ref->getRelay()));
		}
	}
}

/**
 * GeneralsX: The WRAPPERACK of one admitted chunk: (its player, wrapSeq, chunk number), sent DIRECT to the
 * hop the chunk arrived from - the only connection on which that wrapSeq means anything (design v16 section 2).
 */
void ConnectionManager::sendWrapperAck(Int hopSlot, const NetWrapperCommandMsg *chunk) {
	if ((hopSlot < 0) || (hopSlot >= MAX_SLOTS) || (m_connections[hopSlot] == nullptr)) {
		return;
	}
	NetWrapperAckCommandMsg *ack = newInstance(NetWrapperAckCommandMsg);
	ack->setPlayerID(m_localSlot);
	ack->setOriginalPlayerID((UnsignedByte)chunk->getPlayerID());
	ack->setWrapSeq(chunk->getWrapSeq());
	ack->setChunkNumber(chunk->getChunkNumber());
	m_connections[hopSlot]->sendNetCommandMsg(ack, gxSlotBit(hopSlot));
	ack->detach();
}

// GeneralsX: FIRST COPIES ONLY. A duplicate copy (a retransmission, or one the router relayed)
// arrives AFTER the original, finds the frame already ready, and used to be sampled with that later time: a cushion near
// zero. Network::timeForNewFrame slows the frame rate 10% whenever the minimum cushion drops under the run-ahead slack,
// so duplicates pulled the whole lobby to ~55 fps: with frame data resent every 30 ms a network lab ran at
// 56 TPS with ZERO loss (cushion 0-2 frames vs 10-11, same latency). The cushion is how early the input ARRIVED; only the
// copy that delivered it can say. Callers: sendRemoteCommand for a command FrameData had not seen, processFrameInfo for
// a frame's first FRAMEINFO.
void ConnectionManager::sampleArrivalCushion(NetCommandMsg *cmd) {
	if (allCommandsReady(cmd->getExecutionFrame(), TRUE)) {
		UnsignedInt cushion = cmd->getExecutionFrame() - TheGameLogic->getFrame();
		if ((cushion < m_smallestPacketArrivalCushion) || (m_smallestPacketArrivalCushion == -1)) {
			m_smallestPacketArrivalCushion = cushion;
		}
		m_frameMetrics.addCushion(cushion);
	}
}

// ---------------------------------------------------------------------------
// GeneralsX: [GXNET], a 1 Hz network-liveness probe.
//
// WHY IT EXISTS. [GXPACE] (GameClient/GameClient.cpp) proved the stutter is time blocked on peer
// input rather than the local frame limiter, but it cannot say WHY the input is late, and every
// DEBUG_LOG on every loss path compiles to ((void)0) in a deploy build. That is why two shipped
// "stutter fixes" both targeted the render loop: it was the only observable part of the system.
//
// WHAT IT DECIDES. The standing hypothesis is ordinary WAN packet loss repaired by
// Connection::m_retryTime, a hard-coded 2000 ms with no NACK for the missing-packet case
// (FrameData::allCommandsReady only asks for a resend when there are too MANY commands). If that
// is right, `retries` is non-zero in exactly the seconds [GXPACE] reports as slow, and each retry
// costs ~2 s of lockstep. If `retries` is 0 through a slow second, the hypothesis is WRONG and the
// block is somewhere else - which is the outcome this probe exists to be able to report.
//
// blocked= names the first slot whose frame data is incomplete for the frame we are waiting on,
// with its counts, so "we are stalled" becomes "we are stalled on slot 2, which has 0 of 1
// commands for frame N". Read through FrameDataManager's public getters and NOT through
// allCommandsReady(), which is not const - its FRAMEDATA_RESEND branch calls reset() and would
// make the probe change the thing it measures.
//
// cushion= and latency= both had to be re-sourced after the first run, and the reasons are worth
// keeping because both mistakes produced plausible numbers rather than obvious ones:
//
//   * NOT getPacketArrivalCushion(), which is a DESTRUCTIVE read - it returns the value and
//     immediately resets it to -1. Its only other consumer is the network line on the in-game
//     debug display (W3DDisplay.cpp:1659), which would have silently started reading -1 on a
//     rendering peer the moment this probe was switched on.
//     Reading the member m_smallestPacketArrivalCushion directly avoided that but was WORSE as a
//     measurement: nothing else resets it on a headless peer, so it is a minimum-since-the-match-
//     began that decays to 0 and stays there. The first run duly reported "cushion 60 -> 7 -> 4
//     -> 1 -> 0", which reads exactly like a draining buffer and is really a running minimum.
//     m_frameMetrics.getMinimumCushion() is the honest one: a rolling minimum over
//     m_networkCushionHistoryLength samples, reset per window (FrameMetrics.cpp:125-134), and it
//     is the same value Network::timeForNewFrame() self-slugs on.
//
//   * NOT getMaximumLatency(), which averages m_latencyAverages[] - a table populated only on the
//     PACKET ROUTER, by processRunAheadMetrics. On every other peer it is all zeros, so the first
//     run reported latency=0.000 for the whole match on the joining peer and looked like a broken
//     field rather than an inapplicable one. getLocalAverageLatency() is this peer's own measured
//     round trip and is populated everywhere.
//
// Off unless GX_NET is set, env read once. 1 Hz, its own tag on its own line, never folded into
// [GXCRC] - several harnesses md5sum and diff the .crc streams across peers byte for byte, so a
// per-peer clock in that stream would make every one of them report a false desync.
// ---------------------------------------------------------------------------
// A monotonic nanosecond clock. timeGetTime() is milliseconds and is what the engine uses for its
// own pacing, but an iteration period on a free-running headless peer is tens of MICROseconds, and
// the whole point of iter_max is to compare a single long iteration against a multi-hundred-ms
// stall. Millisecond resolution would quantise the common case to 0 and the interesting case would
// still be visible - but the mean would be a lie.
// The longest main-loop iteration since the last [GXNET] emit. Written by gxStallWatch (which runs
// first and already has the measurement) and drained by gxNetSample, so a peer that HITCHES is
// visible on its own 1 Hz line even when it never stalls itself - which is the whole point, since
// the hitching peer is by definition the one that is NOT waiting.
static int64_t s_gxIterMaxNs = 0;

static int64_t gxMonoNs( void )
{
#ifdef _WIN32
	static int64_t s_freq = 0;
	if (s_freq == 0)
		QueryPerformanceFrequency((LARGE_INTEGER *)&s_freq);
	int64_t raw = 0;
	QueryPerformanceCounter((LARGE_INTEGER *)&raw);
	return (s_freq > 0) ? (int64_t)((raw / s_freq) * 1000000000LL + ((raw % s_freq) * 1000000000LL) / s_freq) : 0;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

// ---------------------------------------------------------------------------
// GeneralsX: [GXSTALL], the event-level companion to [GXNET].
//
// WHY A SECOND PROBE. The first live measurement (18,000 frames, mac+linux, evidence/
// ) killed the packet-loss hypothesis: total retransmissions were
// 5 and 6, while the ~1.7 s stall clusters carried ZERO retries. So the stalls are real and the
// wire is not losing anything. That leaves two possibilities that a 1 Hz aggregate CANNOT tell
// apart, and this probe exists solely to separate them:
//
//   (a) THIS peer is WAITING - somebody else's input is genuinely late on the wire.
//   (b) SOME peer is COMPUTING - one long logic frame (a big AI pathfind, a page fault, a log
//       flush) that lockstep then charges to everyone else as a stall.
//
// iter_max_ms is what separates them, and it is measured rather than inferred. ConnectionManager::
// update() is called exactly once per main-loop iteration from Network::liteupdate(), so the gap
// between consecutive calls IS the iteration period - everything the engine did outside the network
// update, which on a headless peer is essentially the logic frame. A peer that HITCHES shows a huge
// iter_max and reports little or no stall of its own; the peers waiting on it show a long stall
// with a small iter_max. Comparing the two streams at the same frame names the culprit.
//
// Also why [GXPACE]'s stall_ms could never answer this: it is a per-SECOND SUM, so a contiguous
// block longer than a second is chopped across buckets and its shape is destroyed. That is exactly
// why "13 buckets averaging 693 ms" would not reconcile with "4-5 two-second events". This emits
// one line per EVENT, on stall exit, with the true duration.
//
// Event-driven rather than sampled, so it is silent on a healthy match and cannot flood the iOS
// peer's 8 MB filtered stderr sink.
// ---------------------------------------------------------------------------
void ConnectionManager::gxStallWatch( void )
{
	static const Bool s_enabled = (getenv("GX_NET") != NULL);
	if (!s_enabled || TheGameLogic == NULL)
		return;

	const int64_t now = gxMonoNs();

	static int64_t     s_lastCall     = 0;
	static Bool        s_inStall      = FALSE;
	static int64_t     s_stallStart   = 0;
	static int64_t     s_stallIterMax = 0;
	static UnsignedInt s_stallFrame   = 0;
	static UnsignedInt s_stallRetries = 0;
	static UnsignedInt s_stallIters   = 0;
	static Int         s_stallSlot    = -1;
	static UnsignedInt s_stallHave    = 0;
	static UnsignedInt s_stallWant    = 0;

	// The iteration period. Skipped on the first call and across any gap longer than two seconds,
	// which is a load screen or a breakpoint rather than a hitch, and would otherwise be reported
	// as the largest stall of the match.
	const int64_t iterNs = (s_lastCall != 0) ? (now - s_lastCall) : 0;
	s_lastCall = now;
	if (iterNs > s_gxIterMaxNs && iterNs <= 2000000000LL)
		s_gxIterMaxNs = iterNs;
	if (iterNs > 2000000000LL)
	{
		s_inStall = FALSE;	// whatever this was, it is not a lockstep stall
		return;
	}

	// Is the frame the engine is about to execute complete for every participating slot? This is
	// the same predicate allCommandsReady() applies, read through non-mutating getters - see the
	// note on gxNetSample above for why calling allCommandsReady() itself is not an option.
	const UnsignedInt frame = TheGameLogic->getFrame();
	Bool ready = TRUE;
	Int  slot = -1;
	UnsignedInt have = 0, want = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if ((m_frameData[i] == NULL) || (m_frameData[i]->getIsQuitting() == TRUE))
			continue;
		const UnsignedInt h = m_frameData[i]->getCommandCount(frame);
		const UnsignedInt w = m_frameData[i]->getFrameCommandCount(frame);
		if (h != w)
		{
			ready = FALSE; slot = i; have = h; want = w;
			break;
		}
	}

	if (!ready)
	{
		if (!s_inStall)
		{
			s_inStall = TRUE;
			s_stallStart = now;
			s_stallIterMax = 0;
			s_stallFrame = frame;
			s_stallRetries = TheGxNetRetries;
			s_stallIters = 0;
			s_stallSlot = slot; s_stallHave = have; s_stallWant = want;
		}
		++s_stallIters;
		if (iterNs > s_stallIterMax)
			s_stallIterMax = iterNs;
		return;
	}

	if (s_inStall)
	{
		s_inStall = FALSE;
		const double durMs = (double)(now - s_stallStart) / 1000000.0;
		// One frame period of blocking is the NORMAL lockstep handshake, not a stall. Reporting it
		// would emit ~60 lines a second and bury the events that matter.
		if (durMs >= 50.0)
		{
			fprintf(stderr,
				"[GXSTALL] frame=%u dur_ms=%.1f blocked=%d have=%u want=%u retries=%u "
				"iters=%u iter_max_ms=%.1f\n",
				s_stallFrame, durMs, s_stallSlot, s_stallHave, s_stallWant,
				TheGxNetRetries - s_stallRetries, s_stallIters,
				(double)s_stallIterMax / 1000000.0);
			fflush(stderr);
		}
	}
}

void ConnectionManager::gxNetSample( void )
{
	static const Bool s_enabled = (getenv("GX_NET") != NULL);
	if (!s_enabled || TheGameLogic == NULL)
		return;

	const UnsignedInt now = timeGetTime();

	static UnsignedInt s_windowStart = 0;
	static UnsignedInt s_retries = 0, s_sendFails = 0, s_sendDrops = 0, s_recvDrops = 0;

	if (s_windowStart == 0) {
		s_windowStart = now;
		s_retries = TheGxNetRetries; s_sendFails = TheGxNetSendFails;
		s_sendDrops = TheGxNetSendDrops; s_recvDrops = TheGxNetRecvDrops;
		return;
	}
	if ((now - s_windowStart) < 1000)
		return;

	// Which slot, if any, is holding up the frame we are trying to execute.
	const UnsignedInt frame = TheGameLogic->getFrame();
	Int blockedSlot = -1;
	UnsignedInt blockedHave = 0, blockedWant = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_frameData[i] == NULL) || (m_frameData[i]->getIsQuitting() == TRUE))
			continue;
		const UnsignedInt have = m_frameData[i]->getCommandCount(frame);
		const UnsignedInt want = m_frameData[i]->getFrameCommandCount(frame);
		if (have != want) {
			blockedSlot = i; blockedHave = have; blockedWant = want;
			break;
		}
	}

	// GeneralsX: rtx_old / rtx_old_to: re-offers of commands unacked GX_STALE_MS after their first
	// send, in total and per destination slot (Connection::doSend). Per destination is what tells retries to a peer that
	// vanished (bounded by its drop) from a resend nobody acks (the rest of the match) - SCOPE 2e.6.
	UnsignedInt rtxOld = 0;
	char rtxTo[256] = "-";
	size_t rtxAt = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] == NULL)
			continue;
		const UnsignedInt n = m_connections[i]->gxTakeStaleRetx();
		rtxOld += n;
		if (rtxAt < sizeof(rtxTo))
			rtxAt += (size_t)snprintf(rtxTo + rtxAt, sizeof(rtxTo) - rtxAt, "%s%d:%u", rtxAt ? "," : "", i, n);
	}

	fprintf(stderr,
		"[GXNET] frame=%u blocked=%d have=%u want=%u retries=%u sendfail=%u senddrop=%u "
		"recvdrop=%u in_pps=%.1f out_pps=%.1f latency=%.3f cushion=%d iter_max_ms=%.1f "
		"router=%u local=%u rtx_old=%u rtx_old_to=%s\n",
		frame, blockedSlot, blockedHave, blockedWant,
		TheGxNetRetries - s_retries, TheGxNetSendFails - s_sendFails,
		TheGxNetSendDrops - s_sendDrops, TheGxNetRecvDrops - s_recvDrops,
		(double)getIncomingPacketsPerSecond(), (double)getOutgoingPacketsPerSecond(),
		(double)getLocalAverageLatency(), m_frameMetrics.getMinimumCushion(),
		(double)s_gxIterMaxNs / 1000000.0,
		m_packetRouterSlot, m_localSlot, rtxOld, rtxTo);
	static Int s_staleBudget[MAX_SLOTS] = { 0 };	// [GXNET] stale lines per destination, 16 each for the match
	static Bool s_staleInit = FALSE;
	if (!s_staleInit) {
		for (Int i = 0; i < MAX_SLOTS; ++i) s_staleBudget[i] = 16;
		s_staleInit = TRUE;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != NULL)
			m_connections[i]->gxTraceStale((UnsignedInt)i, s_staleBudget[i]);
	}
	gxReportResendUnacked();
	fflush(stderr);

	s_windowStart = now;
	s_retries = TheGxNetRetries; s_sendFails = TheGxNetSendFails;
	s_sendDrops = TheGxNetSendDrops; s_recvDrops = TheGxNetRecvDrops;
	s_gxIterMaxNs = 0;
}

/**
 * ConnectionManager::update
 * Update the connections. Tell them to do the receive and send.  Also relay
 * commands to their final destinations as necessary.
 */
void ConnectionManager::update(Bool isInGame) {
//
// 1. do this
// 2. do that
// 3. do something else
// 4. blow something up
// 5. bust some cap
//

	if ((m_localAddr == 0) || (m_localPort == 0)) {
		// we don't have a local address or port yet, this is bad.
		DEBUG_ASSERTCRASH((m_localAddr != 0) && (m_localPort != 0), ("ConnectionManager doesn't have a local address."));
		return;
	}

	m_transport->doRecv();

	if (isInGame) {
		gxStallWatch();
		gxNetSample();
		gxRouterTrace();
		m_disconnectManager->update(this);
	}

	// take the packets from the transport, break them up into commands, and give them to the appropriate connections.
	doRelay();

	// send any necessary keep-alive packets.
	doKeepAlive();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			/*
			if (m_connections[i]->isQueueEmpty() == FALSE) {
//				DEBUG_LOG(("ConnectionManager::update - calling doSend on connection %d", i));
			}
			*/

			m_connections[i]->doSend();

			if (m_connections[i]->isQuitting() && m_connections[i]->isQueueEmpty())
			{
				DEBUG_LOG(("ConnectionManager::update - deleting connection for slot %d", i));
				dropConnection(i);
			}
		}

		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == TRUE)) {
			if (m_frameData[i]->getQuitFrame() == TheGameLogic->getFrame()) {
				DEBUG_LOG(("ConnectionManager::update - deleting frame data for slot %d on quitting frame %d", i, m_frameData[i]->getQuitFrame()));
				deleteInstance(m_frameData[i]);
				m_frameData[i] = nullptr;
			}
		}
	}

	m_transport->doSend();
}

// GeneralsX: H02: the send half of update() for Network::sendLocalCommands, the orders translated
// before the draw. The socket is drained first, as update() always does (doRecv only buffers datagrams; doRelay still
// processes them in update(), after the draw): Transport::doSend runs p2pUpdate, which demotes a direct route whose last
// direct receipt is older than GX_P2P_DEAD_MS, and must not judge that on datagrams still sitting unread in the socket
// Then each connection's doSend (its frame-grouping deferral included) and the transport's
// send, nothing else: no relay, no keep-alive, no disconnect manager and none of the per-call diagnostics (gxStallWatch
// measures the gap between update() calls as one main-loop iteration - a second call per iteration would halve its
// iter_max). update() still runs once per iteration and deletes what a quitting connection has flushed.
void ConnectionManager::flushSends() {
	if ((m_localAddr == 0) || (m_localPort == 0)) {
		return;
	}
	m_transport->doRecv();
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_connections[i]->doSend();
		}
	}
	m_transport->doSend();
}

void ConnectionManager::updateRunAhead(Int oldRunAhead, Int frameRate, Bool didSelfSlug, Int nextExecutionFrame) {
	static time_t lasttimesent = 0;
	time_t curTime = timeGetTime();

	if ((lasttimesent == 0) || ((curTime - lasttimesent) > TheGlobalData->m_networkRunAheadMetricsTime)) {
		if (m_localSlot == m_packetRouterSlot) {
			// GeneralsX: Publish the latency row on the same tick that
			// already recomputes the run-ahead. Same cadence (m_networkRunAheadMetricsTime), no
			// second timer to keep in step, and it rides a path that by construction only the
			// packet router executes. Called BEFORE the run-ahead arithmetic below purely so that
			// an early return added to that arithmetic later cannot silently stop the scoreboard
			// updating - the two are independent and nothing here reads what follows.
			publishLatencyReport();

			// We are the packet router, time to compute a new run ahead for this game.
			m_latencyAverages[m_localSlot] = m_frameMetrics.getAverageLatency();

			// since we are now using the display frame rate rather than the logic frame rate to get our average FPS,
			// it doesn't make sense to send the desired logic frame rate if we "slugged" ourself.
//			if (didSelfSlug) {
//				m_fpsAverages[m_localSlot] = frameRate;
//			} else {
				m_fpsAverages[m_localSlot] = m_frameMetrics.getAverageFPS();
//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, actual fps = %d, latency = %f, didSelfSlug = true", m_fpsAverages[m_localSlot], m_frameMetrics.getAverageFPS(), m_latencyAverages[m_localSlot]));
			} else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, latency = %f, didSelfSlug = false", m_fpsAverages[m_localSlot], m_latencyAverages[m_localSlot]));
			}
			Int minFps;
			Int minFpsPlayer;
			getMinimumFps(minFps, minFpsPlayer);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - max latency = %f, min fps = %d, min fps player = %d old FPS = %d", getMaximumLatency(), minFps, minFpsPlayer, frameRate));
			// Counts this peer too: isPlayerConnected(m_localSlot) is always true.
			Int connected = 0;
			for (Int i = 0; i < MAX_SLOTS; ++i) {
				if (isPlayerConnected(i)) {
					++connected;
				}
			}
			// GeneralsX: NO RENDER-FPS CLAMP WITHOUT A REMOTE PEER. The clamp exists so the
			// other peers do not outrun a slow one and then stall on its commands (FrameData::allCommandsReady). With no
			// remote peer connected - a LAN host alone with AI, an observer host watching AI - there is nobody to wait
			// for, and the clamp only slows this machine's own game to its 30-second AVERAGE render rate
			// (FrameMetrics::doPerFrameMetrics). After a slow stretch (zoomed out) the game then stays slow for 3-30 s
			// once the frames are back (perf-6v6 PART 1, #4: Mini 55 tps for ~3 s at 120 fps, MacBook 18-32 tps for
			// 20+ s). Nothing is lost by dropping it: Network::timeForNewFrame still paces to the tick rate, and a
			// machine that cannot draw that fast still ticks once per drawn frame. Any remote peer, observer included,
			// keeps the clamp exactly as before - it simulates every frame, so its rate binds like a player's.
			// Only the RATE sent is lifted (the two setFrameRate calls below): the run-ahead keeps the measured minFps, so
			// the local player's command delay is integration's exactly (lifting minFps itself
			// made a slow solo match's run-ahead 7 frames instead of 4).
			const Bool noRemotePeer = (connected <= 1);
			if ((minFps >= ((frameRate * 9) / 10)) && (minFps < frameRate)) {
				// if the minimum fps is within 10% of the desired framerate, then keep the current minimum fps.
				minFps = frameRate;
			}

			// GeneralsX @diag /08/2026 Env-gated trace of the rate negotiation.
			// DEBUG_LOG_LEVEL below is dead in every deploy preset, so the two DEBUG_LOG_LEVEL lines
			// bracketing this clamp print nothing in a shipping build - which is why three theories
			// about why a 60 Hz build ticks at 33 all had to be guesses. This prints the actual
			// inputs. Kept on purpose (was marked TEMPORARY): scripts/test/nl1-latency-*,
			// net-pacing-ab.sh and fps-cap-lockstep-local.sh read these [GXRA] lines. Reads only.
			{
				static const Bool s_traceRA = (getenv("GX_RUNAHEAD_TRACE") != nullptr);
				if (s_traceRA)
				{
					fprintf(stderr, "[GXRA] minFps_in=%d minFpsPlayer=%d frameRate_in=%d fpsLimit=%d "
					                "localAvgFps=%d LOGICRATE=%d MIN_LOGIC=%d connected=%d\n",
					        minFps, minFpsPlayer, frameRate, (Int)TheGlobalData->m_framesPerSecondLimit,
					        (Int)m_frameMetrics.getAverageFPS(), (Int)LOGICFRAMES_PER_SECOND, (Int)MIN_LOGIC_FRAMES, connected);
					fflush(stderr);
				}
			}

			// TheSuperHackers @info this clamps the logic time scale fps in network games
			//
			// GeneralsX: The ceiling must never be below the tick rate.
			//
			// MEASURED on an instrumented 60 Hz build: minFps_in=127 fpsLimit=30 LOGICRATE=60. The
			// peer was rendering at 127 and perfectly able to simulate 60, but this clamp pulled the
			// negotiated LOGIC rate down to the RENDER limit of 30, and the 10% ramp below turned
			// that into exactly (30 * 11) / 10 == 33 - the value a 60 Hz build was stuck at across
			// 4000 frames. The 30 comes from the saved render preset, whose ladder starts at 30, so
			// it is the ordinary case rather than a misconfiguration.
			//
			// Clamping the logic rate by the render cap is sound in principle - the engine advances
			// at most one logic frame per main-loop iteration, so you cannot simulate faster than
			// you iterate - but a ceiling BELOW the tick rate is never meaningful: it asks the
			// simulation to run slower than its own nominal rate. Taking the max with
			// LOGICFRAMES_PER_SECOND keeps the real constraint and drops the nonsensical one.
			//
			// Fixed HERE rather than by raising TheGlobalData->m_framesPerSecondLimit globally:
			// that was tried and broke match start outright ("simulated 0 frames"), because the same
			// global also feeds the frame pacer and the logic-time-scale gate.
			//
			// No behavioural change at the default 30 Hz: the ladder's lowest entry IS 30, so the
			// max() picks the same value the clamp already used.
			//
			// GeneralsX: THE CEILING MUST NOT BE ABOVE THE TICK RATE EITHER.
			//
			//
			//
			// A logic frame IS 1/LOGICFRAMES_PER_SECOND of game time - SECONDS_PER_LOGICFRAME_REAL,
			// and every duration in the INI is denominated in it. Negotiating a rate ABOVE the tick
			// rate therefore does not make the game smoother, it makes the game world run fast: at
			// 210 tps on a 60 Hz build every build time, every reload, every unit's speed is 3.5x.
			//
			// The value being clamped is min(measured average RENDER fps) over the live slots
			// (getMinimumFps). Remote reports are clamped to 100 in processRunAheadMetrics, but the
			// router's OWN entry (:2024) is not clamped at all, so once a peer is the only live slot
			// left the ceiling is the only thing standing between the lobby rate and its render
			// rate. FramePacer::getActualFramesPerSecondLimit used to floor render fps to 8x the tick
			// rate inside a network game, so "its render rate" was a big number by construction; since
			// 27/09/2026 it is the player's max-FPS cap, anything from the tick rate up, or unlimited.
			//
			// WHO THIS CHANGES, precisely: only a peer whose SAVED render preset is above the tick
			// rate, and only while it is the packet router. GameEngine::init clamps an unset
			// m_framesPerSecondLimit to LOGICFRAMES_PER_SECOND, so a default install computed
			// max(60, 60) == 60 and is bit-identical before and after. The ladder
			// (FrameRateLimit.cpp:280) offers 65 through 480 and Uncapped(1000000) above it, and
			// picking any of those was enough: at 240 the lobby could negotiate 240 tps, which is
			// the ~210 logic frames per second seen in the field.
			//
			// The 05/08/2026 fix above stays fixed - 60 is still not below the tick rate, so the
			// 30 -> 33 collapse it cured cannot come back. And this makes the two branches of this
			// function agree: the SLOWEST player's rate at :2149 has always been capped at
			// LOGICFRAMES_PER_SECOND. Only the rate sent to everyone ELSE was uncapped. That
			// asymmetry was the whole defect.
			//
			// AND THE WIRE FIELD IS A BYTE. NetRunAheadCommandMsg::setFrameRate takes an
			// UnsignedByte (NetCommandMsg.h:319-320) while minFps here is an Int, so the
			// setFrameRate(minFps) below narrows silently. Under the old ceiling minFps could reach
			// m_framesPerSecondLimit, and the render ladder's top two entries are 480 and
			// UncappedFpsValue == 1000000 (FrameRateLimit.cpp:280) - so a router on those presets
			// could put a value on the wire that truncates mod 256. 480 becomes 224; anything
			// landing on a multiple of 256 becomes 0, and every receiving peer then divides by it
			// in Network::processRunAheadCommand and again in Network::timeForNewFrame. Capping at
			// the tick rate makes the narrowing unreachable, since LOGICFRAMES_PER_SECOND is 30 or
			// 60. The 0 case is separately defended at the consumer; see Network.cpp.
			//
			// Dropping m_framesPerSecondLimit from the expression loses nothing. It was there for
			// "you cannot simulate faster than you iterate", but that constraint is already carried
			// by the measured average this clamps, and the raw global is the WRONG number for it in
			// a network game anyway - a player who picked 30 iterated at 480 because of the (since
			// removed) network floor, which is exactly why using it as a ceiling caused the 33 Hz bug.
			const Int fpsCeiling = (Int)LOGICFRAMES_PER_SECOND;
			minFps = clamp<Int>(MIN_LOGIC_FRAMES, minFps, fpsCeiling);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - minFps after adjustment is %d", minFps));

			// TheSuperHackers @bugfix Mauller 21/08/2025 calculate the runahead so it always follows the latency
			// The runahead should always be rounded up to the next integer value to prevent variations in latency from causing stutter
			// The network slack pushes the runahead up to the next value when the latency is within the slack percentage of the current runahead
			const Real runAheadSlackScale = 1.0f + ( (Real)TheGlobalData->m_networkRunAheadSlack / 100.0f );
			// GeneralsX: With 3+ humans, allow for the RELAYED path's frame-grouping tail.
			// The input that arrives last is relayed: A's send deferral (capped at 2 frames,
			// Network::processRunAheadCommand) + A->router + the router's deferral (half that, setFrameGrouping)
			// + router->B. Up to 3 frames of deferral, 1.5 on average. The latency below is a round trip whose
			// samples carry the same 1.5-frame MEAN, so the mean is covered and the 1.5-frame tail above it is not.
			// And the usable headroom is RA - 1 ticks, not RA: Network::processCommand sends FRAMEINFO only through
			// frame + RA - 1 (`i < executionFrame`). So the allowance is that tail + 1 = 2.5 frames; the 10% slack
			// alone covers it only past ~370 ms at 60 Hz. Until processAckStage1 stopped timing relayed FRAMEINFOs (NL-1)
			// that inflated sample hid this. 2 humans relay nothing and are unchanged. Router-only, broadcast as
			// NETCOMMANDTYPE_RUNAHEAD: deterministic, but it moves the frame every 3+ human command executes on.
			// See scripts/test/audit/bench/latency_own_frameinfo_only.cpp.
			const Real groupingTailFrames = (connected >= 3) ? 2.5f : 0.0f;
			// GeneralsX: Saturate before the float->int conversion. The latency is
			// a raw wire Real (processRunAheadMetrics), so +Inf or anything past INT_MAX reached `(Int)ceilf`,
			// which is UB: arm64 saturates (-> clamped to MAX_FRAMES_AHEAD / 2) but x86 yields INT_MIN
			// (-> MIN_RUNAHEAD), so the broadcast run-ahead depended on the ROUTER's CPU. +-1e6 is exact in
			// a float and far outside [MIN_RUNAHEAD, MAX_FRAMES_AHEAD / 2], so the clamp below gives the same
			// result as before for every input whose conversion was defined; NaN maps to 0 as arm64's did.
			// See scripts/test/audit/bench/runahead_ceil_saturate.cpp.
			Real wantRunAhead = ceilf( getMaximumLatency() * runAheadSlackScale * (Real)minFps + groupingTailFrames );
			wantRunAhead = (wantRunAhead != wantRunAhead) ? 0.0f : clamp<Real>(-1.0e6f, wantRunAhead, 1.0e6f);
			Int newRunAhead = (Int)wantRunAhead;

			// TheSuperHackers @info if the runahead goes below 3 logic frames it can start to introduce stutter
			// We also limit the upper range of the runahead to prevent it getting out of hand
			newRunAhead = clamp<Int>(MIN_RUNAHEAD, newRunAhead, MAX_FRAMES_AHEAD / 2);

			// GeneralsX @diag /09/2026 [GXRA] out - the run-ahead the router is about to broadcast. The
			// [GXRA] line above prints only the INPUTS, and the one line that shows the negotiated value,
			// [GXPACE] runahead=, lives in GameClient::update, which a -headless peer never runs. So a headless
			// multi-peer run (the NL-1 gate, scripts/test/nl1-latency-local.sh) could not see the number it is
			// meant to measure. Same env gate as [GXRA]; reads only, so the simulation is untouched.
			{
				static const Bool s_traceRAOut = (getenv("GX_RUNAHEAD_TRACE") != nullptr);
				if (s_traceRAOut)
				{
					fprintf(stderr, "[GXRA] out frame=%u runahead=%d old=%d maxlat_ms=%.1f minFps=%d\n",
					        (unsigned)TheGameLogic->getFrame(), newRunAhead, oldRunAhead,
					        (double)getMaximumLatency() * 1000.0, minFps);
					fflush(stderr);
				}
			}

			NetRunAheadCommandMsg *msg = newInstance(NetRunAheadCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}

			// needs to be set to the greater of getExecutionFrame and TheGameLogic->getFrame() + oldRunAhead
			// This prevents the case of...
			// run ahead starts at 30
			// run ahead changes to 10 at frame 31 (the command was created on frame 1)
			// run ahead changes to 10 at frame 56 (the command was created on frame 46)
			// notice that 56 is within the previous run ahead of 30 which has triggered
			// the frame command count being set for frames 1 through 60 since run ahead
			// didn't change for the first time till frame 31.  This creates an extra command
			// for frame 56 that isn't accounted for in the frame command count that is sent
			// out in the NetFrameCommandMsg.  sheesh.
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg->setExecutionFrame(nextExecutionFrame);
			} else {
				msg->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

			msg->setRunAhead(newRunAhead);
			msg->setFrameRate(noRemotePeer ? (Int)LOGICFRAMES_PER_SECOND : minFps);
			//DEBUG_LOG(("ConnectionManager::updateRunAhead - new run ahead = %d, new frame rate = %d, execution frame %d", newRunAhead, minFps, msg->getExecutionFrame()));
			sendLocalCommand(msg, ALL_SLOTS_MASK ^ (gxSlotBit(minFpsPlayer))); // Send the packet to everyone but the lowest FPS player.

			NetRunAheadCommandMsg *msg2 = newInstance(NetRunAheadCommandMsg);
			msg2->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg2->getNetCommandType())) {
				/*
				 * Ok there needs to be a big friggin comment about this change...
				 * What happens is that the two run ahead commands get sent to different players
				 * using different command ID's.  So player 1 has the run ahead command as command x
				 * and player 2 has the command as command x+1.  This is all good except when it comes
				 * to players being disconnected.  With the new disconnect scheme player 1 could potentially
				 * send his run ahead command to player 2 (or the other way around) to let player 2 catch
				 * up to him.  So if player 2 has his run ahead command as x+1 and now he gets player 1's
				 * command list with the run ahead command listed as command x, he won't see them as being
				 * the same command and will now think he has two different run ahead commands for that frame
				 * and thus his command list will have an extra command and he will never be able to recover.
				 * So to fix this we have to use the same command ID for both run ahead commands.  That way
				 * when the commands are copied places for the disconnect screen they will be seen as the
				 * same command, and all will be good.
				 */
//				msg2->setID(GenerateNextCommandID());
				msg2->setID(msg->getID());
			}
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg2->setExecutionFrame(nextExecutionFrame);
			} else {
				msg2->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

			// Let the player with the slowest FPS run a little faster than the other computers...
			// just in case they are able to.  Then we might be able to run the game faster which would be good.
			Int newMinFps = (minFps * 11) / 10;
			if (newMinFps == minFps) {
				newMinFps = minFps + 1;
			}
			// GeneralsX: Was a literal 30. This is THE cap that decides the
			// simulation rate of every network match, and hard-coding it meant a GX_LOGIC_60HZ build
			// would negotiate 30 ticks/sec while every duration was denominated in 60ths - i.e. every
			// multiplayer game would run at exactly half speed, permanently. Made rate-relative so
			// the cap means "the nominal tick rate" rather than "thirty".
			// No behavioural change at the default 30 Hz: LOGICFRAMES_PER_SECOND is 30 there.
			if (newMinFps > LOGICFRAMES_PER_SECOND) {
				newMinFps = LOGICFRAMES_PER_SECOND; // Cap FPS to the nominal logic rate.
			}
			msg2->setRunAhead(newRunAhead);
			msg2->setFrameRate(noRemotePeer ? (Int)LOGICFRAMES_PER_SECOND : newMinFps);

			sendLocalCommand(msg2, gxSlotBit(minFpsPlayer));

			msg->detach();
			msg2->detach();
		} else {
			// We are not the packet router, send our metrics info to the packet router.
			NetRunAheadMetricsCommandMsg *msg = newInstance(NetRunAheadMetricsCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}
			msg->setAverageLatency(m_frameMetrics.getAverageLatency());

			// see above for explanation.
//			if (didSelfSlug) {
//				msg->setAverageFps(frameRate);
//			} else {
				msg->setAverageFps(m_frameMetrics.getAverageFPS());
//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, actual fps = %d, didSelfSlug = true", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS(), m_frameMetrics.getAverageFPS()));
			} else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, didSelfSlug = false", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS()));
			}
			// GeneralsX: Guard the router connection before dereferencing it.
			// This branch runs on every peer that is not the router, every m_networkRunAheadMetricsTime
			// ms, and had no null check at all - so any state in which m_packetRouterSlot named a slot
			// with no connection was a hard crash on every non-router peer simultaneously, which reads
			// as a mass disconnect rather than as a null deref. Reachable before the election fix
			// whenever slot 0 held no human. Dropping a metrics sample costs nothing: the router's
			// run-ahead calculation simply keeps the previous average for that peer.
			if ((m_packetRouterSlot < (UnsignedInt)MAX_SLOTS) && (m_connections[m_packetRouterSlot] != nullptr)) {
				m_connections[m_packetRouterSlot]->sendNetCommandMsg(msg, gxSlotBit(m_packetRouterSlot));
			} else {
				DEBUG_CRASH(("ConnectionManager::updateRunAhead - no connection to packet router %u, dropping metrics", (unsigned)m_packetRouterSlot));
			}
			msg->detach();
		}
		lasttimesent = curTime;
	}
}

Real ConnectionManager::getMaximumLatency() {

	Real lat1 = 0.0f;
	Real lat2 = 0.0f;

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (isPlayerConnected(i)) {
			if (m_latencyAverages[i] != 0.0f) {
				if (m_latencyAverages[i] > lat1) {
					lat2 = lat1;
					lat1 = m_latencyAverages[i];
				}
				else if (m_latencyAverages[i] > lat2) {
					lat2 = m_latencyAverages[i];
				}
			}
		}
	}

	return (lat1 + lat2) / 2.0f;
}

void ConnectionManager::getMinimumFps(Int &minFps, Int &minFpsPlayer) {
	minFps = -1;
	minFpsPlayer = -1;
//	DEBUG_LOG_RAW(("ConnectionManager::getMinimumFps -"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_connections[i] != nullptr) || (i == m_localSlot)) {
//			DEBUG_LOG_RAW((" %d: %d,", i, m_fpsAverages[i]));
			if (m_fpsAverages[i] != -1) {
				if ((minFps == -1) || (m_fpsAverages[i] < minFps)) {
					minFps = m_fpsAverages[i];
					minFpsPlayer = i;
				}
			}
		}
	}
//	DEBUG_LOG_RAW(("\n"));
}

UnsignedInt ConnectionManager::getMinimumCushion() {
	return m_frameMetrics.getMinimumCushion();
}

/**
 * The commands for the given frame are all ready, time to send out our command count for that frame.
 */
void ConnectionManager::processFrameTick(UnsignedInt frame) {
	if ((m_frameData[m_localSlot] == nullptr) || (m_frameData[m_localSlot]->getIsQuitting() == TRUE)) {
		// if the local frame data stuff is null, we must be leaving the game.
		return;
	}
	UnsignedShort commandCount = m_frameData[m_localSlot]->getCommandCount(frame);
	NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
	msg->setExecutionFrame(frame);
	msg->setCommandCount(commandCount);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	m_frameMetrics.doPerFrameMetrics(frame);

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processFrameTick - sending frame info for frame %d, ID %d, command count %d", frame, msg->getID(), commandCount));

	sendLocalCommand(msg, ALL_SLOTS_MASK & ~(gxSlotBit(m_localSlot)));

	msg->detach();
}

/**
 * Set the local address.
 */
void ConnectionManager::setLocalAddress(UnsignedInt ip, UnsignedInt port) {
	DEBUG_LOG(("ConnectionManager::setLocalAddress() - local address is %X:%d", ip, port));
	m_localAddr = ip;
	m_localPort = port;
}

/**
 * Initialize the transport object
 */
void ConnectionManager::initTransport() {
	DEBUG_ASSERTCRASH((m_transport == nullptr), ("m_transport already exists when trying to init it."));
	DEBUG_LOG(("ConnectionManager::initTransport - Initializing Transport"));

	delete m_transport;
	m_transport = new Transport;
	m_transport->reset();
	m_transport->init(m_localAddr, m_localPort);
}

/**
 * This is where the commands from the local client are sent to the other clients in
 * the game.  This is also where the local commands are put into the frame data for
 * future execution.
 */
void ConnectionManager::sendLocalGameMessage(GameMessage *msg, UnsignedInt frame) {
	UnsignedShort currentID = 0;
	if (DoesCommandRequireACommandID(NETCOMMANDTYPE_GAMECOMMAND)) {
		currentID = GenerateNextCommandID();
	}

	NetCommandMsg *netmsg = newInstance(NetGameCommandMsg)(msg);
	netmsg->setExecutionFrame(frame);
	netmsg->setPlayerID(m_localSlot);
	netmsg->setID(currentID);

	sendLocalCommand(netmsg);

	netmsg->detach();
}

/**
 * This is a NetCommandMsg that originated on the local computer. Send this to everyone specified
 * in the relay field.  Commands sent in this way go through the packet router.
 */
void ConnectionManager::sendLocalCommand(NetCommandMsg *msg, RelayMaskType relay /* = ALL_SLOTS_MASK by default*/) {
	if (CommandRequiresDirectSend(msg) || (m_packetRouterSlot < 0) || (m_packetRouterSlot >= MAX_SLOTS) || (m_connections[m_packetRouterSlot] == nullptr)) {
		sendLocalCommandDirect(msg, relay);
		return;
	}
	msg->attach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - sending net command %d of type %s", msg->getID(),
		GetNetCommandTypeAsString(msg->getNetCommandType())));

	if (relay & (gxSlotBit(m_localSlot))) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
		m_frameData[m_localSlot]->addNetCommandMsg(msg);
	}

	// Send the packet to everyone else
	if (m_localSlot == m_packetRouterSlot) {
		// I am the packet router, I need to send this packet to everyone individually.
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			// Send it to all open connections.
			if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) && (relay & (gxSlotBit(i)))) {
				// Set the relay mask to only go to this player so he knows not to relay it to anyone else.
				RelayMaskType temprelay = gxSlotBit(i);
				m_connections[i]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.
			}
		}
	} else {
		// Send the command to everyone else via the packet router.
		RelayMaskType temprelay = relay & ~(gxSlotBit(m_localSlot));	// Tell the packet router to relay the message to everyone but myself.
														// Hopefully the packet router is smart enough to not send it
														// to slots that are not in the game.

		m_connections[m_packetRouterSlot]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.

		if (CommandRequiresAck(msg)) {
			NetCommandRef *ref = m_pendingCommands->addMessage(msg);
			//DEBUG_LOG(("ConnectionManager::sendLocalCommand - added command %d to pending commands list.", msg->getID()));
			if (ref != nullptr) {
				ref->setRelay(temprelay);
			}
		}
	}

	msg->detach(); // detach from the command msg.
}

/**
 * This is a NetCommandMsg that originated on the local computer.  Send this to everyone specified
 * in the relay field.  Commands sent in this way do not go through the packet router.
 */
void ConnectionManager::sendLocalCommandDirect(NetCommandMsg *msg, RelayMaskType relay) {
	msg->attach();

	if (((relay & (gxSlotBit(m_localSlot))) != 0) && (m_frameData[m_localSlot] != nullptr)) {
		if (IsCommandSynchronized(msg->getNetCommandType()) == TRUE) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
			m_frameData[m_localSlot]->addNetCommandMsg(msg);
		}
	}

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (gxSlotBit(i))) != 0) {
			if ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) {
				RelayMaskType temprelay = gxSlotBit(i);
				m_connections[i]->sendNetCommandMsg(msg, temprelay);
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - Sending direct command %d of type %s to player %d", msg->getID(), GetNetCommandTypeAsString(msg->getNetCommandType()), i));
			}
		}
	}

	msg->detach();
}

Int commandsReadyDebugSpewage = 0;

/**
 * Returns true if all the commands for the given frame are ready to be executed.
 */
Bool ConnectionManager::allCommandsReady(UnsignedInt frame, Bool justTesting /* = FALSE */) {
	Bool retval = TRUE;
	FrameDataReturnType frameRetVal = FRAMEDATA_NOTREADY;
//	retval = FALSE;  // ****for testing purposes only!!!!!!****
	Int i = 0;
	for (; (i < MAX_SLOTS) && retval; ++i) {
		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == FALSE)) {
/*
			if (!(m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE)))) {
				if ((frame != commandsReadyDebugSpewage) && (justTesting == FALSE)) {
					DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d not ready.", frame, i));
					commandsReadyDebugSpewage = frame;
				}
				retval = FALSE;
			} else {
//				DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d is ready.", frame, i));
			}
*/

			frameRetVal = m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE));
			if (frameRetVal == FRAMEDATA_NOTREADY) {
				retval = FALSE;
			} else if (frameRetVal == FRAMEDATA_RESEND) {
				requestFrameDataResend(i, frame);
				retval = FALSE;
			}
		}
	}

	if (frameRetVal == FRAMEDATA_RESEND) {
		// this frame's data is really screwed up, we need to clean it out so it can be resent to us.
		for (i = 0; i < MAX_SLOTS; ++i) {
			if ((m_frameData[i] != nullptr) && (i != m_localSlot)) {
				m_frameData[i]->resetFrame(frame, FALSE);
			}
		}
	}

	if ((retval == TRUE) && (justTesting == FALSE)) {
		m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this);
		retval = m_disconnectManager->allowedToContinue(); // allow the disconnect manager to keep us on this frame
																											// in case we are waiting for a new packet router or something.
	}

	return retval;
}

void ConnectionManager::handleAllCommandsReady()
{
	m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this, FALSE);
}


/**
 * Only call this after making sure that all the commands are there for this frame.
 * After calling this the commands for this frame will be removed from the connection.
 *
 * BGC - To account for the case where the host disconnects without sending the
 *       same commands to all players, we now have to keep around the last 'run ahead'
 *       frames so we can potentially send those commands to the other players in the
 *       game so they can catch up.
 */
NetCommandList *ConnectionManager::getFrameCommandList(UnsignedInt frame)
{
	NetCommandList *retlist = newInstance(NetCommandList);
	retlist->init();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
			retlist->appendList(m_frameData[i]->getFrameCommandList(frame));
			if (frame > FRAMES_TO_KEEP) {
				m_frameData[i]->resetFrame(frame - FRAMES_TO_KEEP);	// After getting the commands for that frame from this
													// FrameDataManager object, we need to tell it that we're
													// done with the messages for that frame.
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("getFrameCommandList - called reset frame on player %d for frame %d", i, frame - FRAMES_TO_KEEP));
			}
		}
	}

	return retlist; // retlist deallocated by calling function.
}

void ConnectionManager::setFrameGrouping(time_t frameGrouping) {
	// Since we are the packet router, we should send more packets per second since we
	// may become the latency bottleneck for sending packets from one player to the next.
	// This is probably ok since the packet router should have the fastest connection of all
	// the players in the game.
	if (m_localSlot == m_packetRouterSlot) {
		frameGrouping = frameGrouping / 2;
	}
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_connections[i]->setFrameGrouping(frameGrouping);
		}
	}
}

// ---------------------------------------------------------------------------
// GeneralsX: Packet router election.
//
// WHAT THE ROUTER COSTS. The in-game topology is a STAR, not a mesh: sendLocalCommand sends a
// non-router peer's commands ONLY to m_connections[m_packetRouterSlot], and the router then fans
// them out one connection at a time. So peer j receives peer i's input after
// leg(i->router) + leg(router->j). Under lockstep no peer executes frame N until EVERY peer's
// input for N has arrived, so the quantity that actually binds the match is the sum of the two
// worst router-adjacent legs. Everything else is slack. Choosing the centre badly is therefore
// paid by every player, twice.
//
// WHAT DATA EXISTS, AND WHERE. Measured against this tree, not assumed:
//
//  * A non-router peer's m_frameMetrics.getAverageLatency() is a genuine one-hop round trip to the
//    ROUTER. processFrameTick() stamps the send time (FrameMetrics::doPerFrameMetrics) and hands
//    the FRAMEINFO to sendLocalCommand, which for a non-router puts it on the router's connection
//    alone; the router acks it itself (ackCommand's "we are the packet router, send the ack to the
//    player that sent the command" branch), and processAckStage1 turns that ack into
//    FrameMetrics::processLatencyResponse. One hop out, one hop back.
//  * Every non-router peer unicasts that number to the router every m_networkRunAheadMetricsTime ms
//    (updateRunAhead's else branch), and processRunAheadMetrics files it in m_latencyAverages.
//    => THE ROUTER KNOWS d(i, router) FOR EVERY PEER i. That is one ROW of the latency matrix.
//  * Nothing sends it back out. On a non-router peer m_latencyAverages is all zeroes - even its own
//    entry, which is written only inside updateRunAhead's router-only branch. A non-router has no
//    table at all.
//  * NOBODY, router included, knows d(i, j) for two non-router peers. Nothing ever measures it.
//    Peers exchange no timed traffic with each other: the only packets that bypass the router are
//    the CommandRequiresDirectSend set (disconnect, file transfer, frame resend), and the one
//    periodic direct peer-to-peer message, NetKeepAliveCommandMsg from doKeepAlive(), carries no
//    command ID and is not in CommandRequiresAck - it is fire-and-forget and cannot be timed.
//
// WHY THAT ROW CANNOT ELECT A BETTER ROUTER. This is a proof, not a shortage of cleverness.
// Write C(x) for the binding cost with x as the centre: the largest d(.,x) plus the second largest.
// C(router) is directly measured. For any other candidate x, every bound on d(i,j) derivable from
// the row {d(.,router)} is a bound through the router, i.e. the triangle inequality
// d(i,j) <= d(i,router) + d(router,j). Feeding that in gives C(x) <= C(router) + 2*d(x,router),
// which is ALWAYS >= C(router). The lower bound d(i,x) >= |d(i,router) - d(x,router)| runs the
// other way and can only ever certify that a candidate is WORSE (it proves x is worse than the
// incumbent when d(x,router) exceeds the sum of the two worst legs - the "never promote the peer on
// bad wifi" rule). So one row of the matrix can PROVE a candidate is worse and can NEVER prove one
// is better. Any "elect the best-connected peer" rule built on this data is an inference from an
// unvalidated additive-link model, and being wrong about it costs every player in the lobby.
//
// Hence: NO MEASURED ELECTION SHIPS. What ships is (a) a deterministic election that is correct
// where the retail literal was not, and (b) the measurement, [GXROUTE] in gxRouterTrace(), that
// makes C(router) observable so the decision can be made on numbers from a real run.
//
// THE SMALLEST WIRE CHANGE THAT WOULD MAKE A REAL ELECTION POSSIBLE, for whoever picks this up:
// a new acked, direct-send NETCOMMANDTYPE_PEERPROBE (round-robin, one peer per second, exactly like
// doKeepAlive already walks the connections) so each peer measures its OWN row; then widen
// NetRunAheadMetricsCommandMsg - today a Real latency plus an Int fps - by MAX_SLOTS latencies so
// each peer ships its row to the router. As UnsignedShort milliseconds that is 8 * 2 = 16 bytes on
// a command that today carries 8, inside a NetCommandMsg whose packet budget is
// RETAIL_GAME_PACKET_SIZE = 476 and which is nowhere near it. That gives the router the whole
// matrix and makes C(x) computable for every candidate. It does NOT make the handover safe on its
// own - see the note in electPacketRouter() about when a change may be applied.
//
// WHY ELECTION IS AT MATCH START AND THE ROUTER NEVER CHANGES MID-MATCH BY CHOICE. There is no
// latency data at match start (the boxcar in FrameMetrics::init is pre-loaded, not measured), so a
// measured election is inherently a mid-match handover, and a handover is the risky operation: a
// peer that flips the centre at a different moment from its neighbours routes its input somewhere
// the others are not reading. It is survivable rather than instantly fatal - doRelay/
// sendRemoteCommand fan out purely on the relay mask and never test isPacketRouter(), and
// FrameData::addCommand dedupes by command identity before it increments the count that
// allCommandsReady tests, so the retransmit path already makes duplicate delivery harmless - but
// run-ahead negotiation breaks (two routers fight, or none computes one) and the null deref noted
// in init() becomes reachable. If a handover is ever added it must ride the frame-synchronised
// path that NETCOMMANDTYPE_RUNAHEAD already uses (IsCommandSynchronized -> every peer's frame data
// -> Network::processFrameSynchronizedNetCommand at the command's execution frame), so that every
// peer flips on the SAME logic frame, and it must call resendPendingCommands() exactly as the
// PLAYERLEAVECODE_PACKETROUTER path does. Nothing less is safe.
//
// HOW EVERY PEER AGREES ON THE ANSWER. This function reads only the replicated slot list, runs the
// same integer code on every peer, and sends nothing. No latency, no floating point, no timing, no
// arrival order. Peers cannot disagree because there is nothing peer-local in the input.
// ---------------------------------------------------------------------------
void ConnectionManager::electPacketRouter() {
	// m_packetRouterFallback was just built by parseUserList as an ordered, compacted list of the
	// slots that hold humans: rank 0 is the lowest-numbered human slot, and unused ranks are -1.
	// Entry 0 is therefore "the first human in the lobby", which is the host in every lobby this
	// engine can build, and which is what the retail literal 0 was reaching for.
	UnsignedInt elected = MAX_SLOTS;
	if (m_packetRouterFallback[0] < (UnsignedInt)MAX_SLOTS) {
		elected = m_packetRouterFallback[0];
	}

	// GX_ROUTER_SLOT - harness-only override, default off, and DANGEROUS in a way the other GX_*
	// knobs in this file are not.
	//
	// GX_GROUPING_DIV can safely differ between peers because it only changes WHEN a packet is sent.
	// This one changes WHERE input is sent, so it MUST be set to the same value on every peer in the
	// lobby or the peers disagree about the centre (see the paragraph on handovers above for what
	// that does and does not break). It exists for one purpose: C(x) for a candidate other than the
	// incumbent is not computable from the incumbent's row, so the only sound way to obtain it is to
	// run the same lobby again with the centre moved and read the [GXROUTE] line. That is a
	// measurement, not a policy.
	//
	// Refused unless it names a slot that is actually in the fallback plan, so a typo degrades to
	// the elected value on every peer identically rather than to a different value on each.
	const char *routerOverride = getenv("GX_ROUTER_SLOT");
	if (routerOverride != NULL) {
		const Int wanted = atoi(routerOverride);
		Bool isParticipant = FALSE;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			if ((wanted >= 0) && (m_packetRouterFallback[i] == (UnsignedInt)wanted)) {
				isParticipant = TRUE;
				break;
			}
		}
		if (isParticipant) {
			fprintf(stderr, "[GXROUTE] override: packet router forced to slot %d "
			                "(elected would have been %u). This MUST match on every peer.\n",
			        wanted, (unsigned)elected);
			elected = (UnsignedInt)wanted;
		} else {
			fprintf(stderr, "[GXROUTE] override: GX_ROUTER_SLOT=%s does not name a human slot in "
			                "this lobby, ignoring and using elected slot %u.\n",
			        routerOverride, (unsigned)elected);
		}
		fflush(stderr);
	}

	if (elected >= (UnsignedInt)MAX_SLOTS) {
		// No humans in the slot list at all. parseUserList has already failed in that case and the
		// match cannot start; leave the placeholder alone rather than write a nonsense slot that the
		// fallback walk would then have to survive.
		DEBUG_CRASH(("ConnectionManager::electPacketRouter - no human slots, keeping packet router %u", (unsigned)m_packetRouterSlot));
		return;
	}

	m_packetRouterSlot = elected;
	DEBUG_LOG(("ConnectionManager::electPacketRouter - packet router is slot %u (local slot %u)", (unsigned)m_packetRouterSlot, (unsigned)m_localSlot));
}

// ---------------------------------------------------------------------------
// GeneralsX: [GXROUTE] - the router's latency row, once a second.
//
// This is the measurement that the election above deliberately does not act on. Only the router has
// the data (see electPacketRouter), so only the router prints, and one line carries the whole row
// plus the derived quantity that matters:
//
//   cost_ms = worst leg + second-worst leg, i.e. C(router). Under lockstep that sum is what every
//   peer waits for, so it is the number to compare between runs. A single leg is not - improving
//   the third-worst leg changes nothing at all.
//
// Read it with GX_ROUTER_SLOT: run the lobby, note cost_ms, re-run the identical lobby with the
// centre forced elsewhere on EVERY peer, note cost_ms again. That difference is a measured A/B of
// two centres, which is the only sound way to compare them.
//
// Two biases to state rather than discover later. The legs include frame-grouping deferral, which
// is a deliberate send delay and not propagation - setFrameGrouping halves it for the router, so
// the router's own outbound share is smaller than everyone else's. And local_ms is the ROUTER's own
// m_frameMetrics figure, which is the last ack to arrive for a frame rather than any particular
// peer's, so it tracks the worst leg and is not an independent sample; it is printed to show the
// row is live, not as an eighth measurement.
//
// Its own tag on its own line, never folded into [GXCRC] - harnesses diff those streams byte for
// byte across peers and a per-peer clock in that stream would report a false desync.
// ---------------------------------------------------------------------------
void ConnectionManager::gxRouterTrace( void ) {
	static const Bool s_enabled = (getenv("GX_ROUTER_TRACE") != NULL);
	if (!s_enabled || TheGameLogic == NULL)
		return;
	if (m_localSlot != m_packetRouterSlot)
		return;		// only the router holds the row; a non-router would print eight zeroes

	const UnsignedInt now = timeGetTime();
	static UnsignedInt s_windowStart = 0;
	if (s_windowStart == 0) {
		s_windowStart = now;
		return;
	}
	if ((now - s_windowStart) < 1000)
		return;
	s_windowStart = now;

	// The two worst legs, which are the pair that binds the lobby. Skipping the router's own entry:
	// it is a restatement of the worst leg, not a separate peer.
	Real worst = 0.0f;
	Real second = 0.0f;
	Int worstSlot = -1;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((i == (Int)m_localSlot) || !isPlayerConnected(i))
			continue;
		const Real lat = m_latencyAverages[i];
		if (lat > worst) {
			second = worst;
			worst = lat;
			worstSlot = i;
		} else if (lat > second) {
			second = lat;
		}
	}

	// m_latencyAverages is populated straight off the wire by processRunAheadMetrics, so a corrupt or
	// hostile peer can put anything in it, including a value whose decimal expansion is longer than
	// this buffer. Clamp what is printed and treat truncation as a hard stop.
	char row[MAX_SLOTS * 16];
	Int used = 0;
	row[0] = '\0';
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (!isPlayerConnected(i) && (i != (Int)m_localSlot))
			continue;
		const Int room = (Int)sizeof(row) - used;
		if (room <= 1)
			break;
		const double ms = clamp<double>(0.0, (double)m_latencyAverages[i] * 1000.0, 999999.0);
		const Int written = snprintf(row + used, (size_t)room, "%s%d:%.0f", (used > 0) ? "," : "", i, ms);
		if ((written < 0) || (written >= room))
			break;		// truncated; keep what fits rather than walking off the end
		used += written;
	}

	fprintf(stderr,
		"[GXROUTE] frame=%u router=%u local_ms=%.0f rtt_ms=[%s] worst=%d cost_ms=%.0f\n",
		(unsigned)TheGameLogic->getFrame(), (unsigned)m_packetRouterSlot,
		clamp<double>(0.0, (double)getLocalAverageLatency() * 1000.0, 999999.0), row, worstSlot,
		clamp<double>(0.0, (double)(worst + second) * 1000.0, 999999.0));
	fflush(stderr);
}

void ConnectionManager::doKeepAlive() {
	static Int nextIndex = 0;
	static time_t startTime = 0;

	time_t curTime = timeGetTime();

	if (startTime == 0) {
		startTime = curTime;
		return;
	}

	time_t numSeconds = (curTime - startTime) / 1000;

	while ((nextIndex <= numSeconds) && (nextIndex < MAX_SLOTS)) {
//		DEBUG_LOG(("ConnectionManager::doKeepAlive - trying to send keep alive message to player %d", nextIndex));
		if (m_connections[nextIndex] != nullptr) {
			NetKeepAliveCommandMsg *msg = newInstance(NetKeepAliveCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
				msg->setID(GenerateNextCommandID());
			}
//			DEBUG_LOG(("ConnectionManager::doKeepAlive - sending keep alive message to player %d", nextIndex));
			sendLocalCommandDirect(msg, gxSlotBit(nextIndex));
			msg->detach();
		}
		++nextIndex;
	}
	if (nextIndex == MAX_SLOTS) {
		nextIndex = 0;
		startTime = curTime;
	}
}

PlayerLeaveCode ConnectionManager::disconnectPlayer(Int slot) {
	// Need to do the deletion of the slot's connection and frame data here.
	PlayerLeaveCode retval = PLAYERLEAVECODE_CLIENT;
	DEBUG_LOG(("ConnectionManager::disconnectPlayer - disconnecting slot %d on frame %d", slot, TheGameLogic->getFrame()));

	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return PLAYERLEAVECODE_UNKNOWN;
	}

	if (TheGameInfo)
	{
		GameSlot *gSlot = TheGameInfo->getSlot( slot );
		if (gSlot && !gSlot->lastFrameInGame())
		{
			DEBUG_LOG(("ConnectionManager::disconnectPlayer(%d) - slot is last in the game on frame %d",
				slot, TheGameLogic->getFrame()));
			gSlot->setLastFrameInGame(TheGameLogic->getFrame());
		}
	}

	UnicodeString unicodeName;
	unicodeName = getPlayerName(slot);
	if (!unicodeName.isEmpty() && m_connections[slot]) {
		TheInGameUI->message("Network:PlayerLeftGame", unicodeName.str());

		// People are boneheads. Also play a sound
		static AudioEventRTS leftGameSound("GUIMessageReceived");
		TheAudio->addAudioEvent(&leftGameSound);
	}

	if ((m_frameData[slot] != nullptr) && (m_frameData[slot]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d frame data", slot));
		deleteInstance(m_frameData[slot]);
		m_frameData[slot] = nullptr;
	}

	if (m_connections[slot] != nullptr && !m_connections[slot]->isQuitting()) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d connection", slot));
		dropConnection(slot);
	}

//	if (playerID == m_localSlot) {
//		TheMessageStream->appendMessage(GameMessage::MSG_CLEAR_GAME_DATA);
//	}

	if (slot == m_packetRouterSlot) {
		// GeneralsX: This walk read one past the end of the array.
		//
		// Retail: `while ((index < (MAX_SLOTS-1)) && (fallback[index] != router)) ++index; ++index;
		// m_packetRouterSlot = m_packetRouterFallback[index];`
		//
		// Two defects, one visible. The bound MAX_SLOTS-1 means rank 7 is never COMPARED, so a router
		// sitting at the last rank is never found - the loop simply runs out and exits at index 7
		// claiming a match it never made. Either way ++index then indexes m_packetRouterFallback[8]
		// on an array of MAX_SLOTS == 8: an out-of-bounds read whose value is written straight into
		// m_packetRouterSlot. That is the worst possible failure for this variable. Every peer runs
		// this same code but reads its own adjacent stack, so peers can come out of it disagreeing
		// about the centre, which is exactly the lobby split this whole change exists to avoid.
		//
		// Reachable whenever the incumbent is not in the plan - which is precisely the case the old
		// hardcoded `m_packetRouterSlot = 0` created when slot 0 held no human. After the election
		// fix the incumbent is always in the plan (and, because the plan is compacted on every leave,
		// always at rank 0), so this is now belt and braces - but it must be correct belt and braces,
		// because it decides the centre.
		//
		// Search the FULL range, then take the next rank that names a slot still in the game.
		// Deterministic and input-identical on every peer, so they all land on the same successor.
		Int index = 0;
		while ((index < MAX_SLOTS) && (m_packetRouterFallback[index] != m_packetRouterSlot)) {
			++index;
		}
		for (Int probe = index + 1; probe < MAX_SLOTS; ++probe) {
			const UnsignedInt candidate = m_packetRouterFallback[probe];
			if ((candidate < (UnsignedInt)MAX_SLOTS) && (candidate != (UnsignedInt)slot)) {
				m_packetRouterSlot = candidate;
				break;
			}
		}
		// If there is no successor the router is left unchanged rather than set to garbage. The
		// caller reads PLAYERLEAVECODE_PACKETROUTER either way, and DisconnectManager's own walk
		// (isLocalPlayerNextPacketRouter) already treats "no next router" as the end of the game.
		DEBUG_LOG(("Packet router left.  New packet router is slot %d", m_packetRouterSlot));
		retval = PLAYERLEAVECODE_PACKETROUTER;
	}
	if (m_localSlot == slot) {
		DEBUG_LOG(("Disconnecting self"));
		retval = PLAYERLEAVECODE_LOCAL;
	}

	// Take the player out of the fallback plan
	Int fallbackindex = 0;
	while ((fallbackindex < MAX_SLOTS) && (m_packetRouterFallback[fallbackindex] != slot)) {
		++fallbackindex;
	}

	for (Int i = fallbackindex; i < MAX_SLOTS-1; ++i) {
		m_packetRouterFallback[i] = m_packetRouterFallback[i+1];
	}
	m_packetRouterFallback[MAX_SLOTS-1] = -1;

	return retval;
}

void ConnectionManager::quitGame() {
	// Need to do the NetDisconnectPlayerCommandMsg creation and sending here.
	NetDisconnectPlayerCommandMsg *disconnectMsg = newInstance(NetDisconnectPlayerCommandMsg);
	disconnectMsg->setDisconnectSlot(m_localSlot);
	disconnectMsg->setDisconnectFrame(TheGameLogic->getFrame());
	disconnectMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(disconnectMsg->getNetCommandType())) {
		disconnectMsg->setID(GenerateNextCommandID());
	}
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to send disconnect command"));
	sendLocalCommandDirect(disconnectMsg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));

	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to flush connections"));
	flushConnections(); // need to do this so our packet actually gets sent before the connections are deleted.
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - done flushing connections"));

	disconnectMsg->detach();

#if RTS_GENERALS
	// if we get here, we hit Quit on the disconnect screen.  Mark everyone as having disconnected from us
	// so the online stats can give us appropriate feedback.
	if (TheGameInfo)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			GameSlot *gSlot = TheGameInfo->getSlot( i );
			if (gSlot && !gSlot->lastFrameInGame())
			{
				gSlot->markAsDisconnected();
			}
		}
	}
#endif

	disconnectLocalPlayer();
}

void ConnectionManager::disconnectLocalPlayer() {
	// kill the frame data and the connections for all the other players.
	DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer()"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (i != m_localSlot) {
			disconnectPlayer(i);
		}
	}
}

/**
 * Takes all the commands that are ready to send and sends them right now.
 */
void ConnectionManager::flushConnections() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			// GeneralsX: A flush must send NOW. Connection::doSend returns
			// without sending while the frame-grouping interval since its last send has not elapsed,
			// and the only caller, quitGame, deletes every connection straight after - so a hard
			// quit's DISCONNECTPLAYER was dropped whenever the last send was recent, and the others sat
			// out the full player timeout instead. Measured: net-kill-gates.sh legit-hardquit-router,
			// quitting mid-match, lost it in both runs measured. The connections do not outlive this call.
			m_connections[i]->setFrameGrouping(0);
//			DEBUG_LOG(("ConnectionManager::flushConnections - flushing connection to player %d", i));
			/*
			if (m_connections[i]->isQueueEmpty()) {
//				DEBUG_LOG(("ConnectionManager::flushConnections - connection queue empty"));
			}
			*/
			m_connections[i]->doSend();
		}
	}

	if (m_transport != nullptr) {
		m_transport->doSend();
	}
}

void ConnectionManager::resendPendingCommands() {
	//DEBUG_LOG(("ConnectionManager::resendPendingCommands()"));
	if (m_pendingCommands == nullptr) {
		return;
	}

	NetCommandRef *ref = m_pendingCommands->getFirstMessage();
	while (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::resendPendingCommands - resending command %d", ref->getCommand()->getID()));
		sendLocalCommand(ref->getCommand(), ref->getRelay());
		ref = ref->getNext();
	}
}

UnsignedInt ConnectionManager::getLocalPlayerID() {
	return m_localSlot;
}

UnicodeString ConnectionManager::getPlayerName(Int playerNum) {
	UnicodeString retval;
	if( playerNum == m_localSlot ) {
		retval = m_localUser->GetName();
	}	else if (((m_connections[playerNum] != nullptr) && (m_connections[playerNum]->isQuitting() == FALSE))) {
		retval = m_connections[playerNum]->getUser()->GetName();
	}
	return retval;
}

/**
 * Take a user list and make connections and frame data manager objects for each of the players.
 * For now, this is also how we'll determine the packet router fallback plan.
 */
void ConnectionManager::parseUserList(const GameInfo *game)
{
	if (!game)
		return;

	Int i;
	Int numUsers = 0;
	UnsignedInt remoteAddr[MAX_SLOTS] = {0};	// for m_sharedAddrSlots: 0 = local or no human
	m_localSlot = -1;
	DEBUG_LOG(("Local slot is %d", game->getLocalSlotNum()));
	for (i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);	// badness, but since we cast right back to const, we should be ok
		if (slot->isHuman())
		{
			if (game->getLocalSlotNum() == i)
			{
				m_localSlot = i;
   			m_localUser->setName(slot->getName());
			}

			if (m_localSlot != i)
			{
				m_connections[i] = newInstance(Connection)();
				m_connections[i]->init();
				m_connections[i]->attachTransport(m_transport);
//				UnsignedShort port = (TheNAT)?TheNAT->getSlotPort(i):8088;
				UnsignedShort port = slot->getPort();
				m_connections[i]->setUser(newInstance(User)(slot->getName(), slot->getIP(), port));
				remoteAddr[i] = slot->getIP();
				m_frameData[i] = newInstance(FrameDataManager)(FALSE);
				DEBUG_LOG(("Remote user is at %X:%d", slot->getIP(), slot->getPort()));
			}
			else
			{
				DEBUG_LOG(("Local user is %d (%X:%d)", m_localSlot, slot->getIP(), slot->getPort()));
				m_frameData[i] = newInstance(FrameDataManager)(TRUE);
			}
			m_frameData[i]->init();
			m_frameData[i]->reset();

			m_packetRouterFallback[numUsers] = i;

			++numUsers;

		}
	}
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif

	// GeneralsX: Choose the packet router here rather than leaving it on the
	// literal 0 that init() set. This is the one point where every peer runs identical code over the
	// identical replicated slot list, before a single game packet has moved, so it is the only place
	// an election can be made without a wire field and without any chance of the peers disagreeing.
	electPacketRouter();

	// GeneralsX: The succession lockstepRouter reads: the plan the live
	// fallback walks, from the elected router on, copied BEFORE any local disconnect can reshape it.
	Int routerRank = 0;
	while ((routerRank < MAX_SLOTS) && (m_packetRouterFallback[routerRank] != m_packetRouterSlot)) {
		++routerRank;
	}
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_lockstepPlan[i] = (routerRank + i < MAX_SLOTS) ? m_packetRouterFallback[routerRank + i] : MAX_SLOTS;
	}
	m_lockstepGone = 0;
	m_sharedAddrSlots = GXSharedAddrSlots(remoteAddr, MAX_SLOTS);

	/*
	if ( numUsers < 2 || m_localSlot == -1 )
	{
		DEBUG_CRASH(("FAILED parseUserList - network game won't work as expected"));
		return;
	}

	char * list = strdup(buf);
	char *listPtr = list;
	if (!list)
		return;

	User users[MAX_SLOTS];
	int localUser = -1;
	int i;

	for (i=0; i<MAX_SLOTS; i++)
	{
		users[i].setName("");
	}

	char *userStr, *nameStr, *addrStr, *portStr;
	AsciiString addrAsciiStr;

	char *listPos;

	DEBUG_LOG(("ConnectionManager::parseUserList - looking for local user at %d.%d.%d.%d:%d",
		PRINTF_IP_AS_4_INTS(m_localAddr),
		m_localPort));

	int numUsers = 0;
	while ( (userStr=strtok_r(listPtr, ",", &listPos)) != nullptr )
	{
		listPtr = nullptr;
		char *pos = nullptr;

		nameStr = strtok_r(userStr, "@", &pos);
		addrStr = strtok_r(nullptr, "@:", &pos);
		portStr = strtok_r(nullptr, ": ", &pos);

		if (!portStr || numUsers >= MAX_SLOTS)
		{
			DEBUG_LOG(("ConnectionManager::parseUserList - (numUsers = %d) FAILED parseUserList with list [%s]", numUsers, buf));
			return;
		}

		addrAsciiStr = addrStr;
		UnsignedInt addr = ResolveIP(addrAsciiStr);
		UnsignedInt port = atoi(portStr);

//		if ((m_localAddr != addr) || (m_localPort != port)) {
		if (loginName.compare(nameStr) != 0) {
			m_connections[numUsers] = newInstance(Connection)();
			m_connections[numUsers]->init();
			m_connections[numUsers]->attachTransport(m_transport);
			m_connections[numUsers]->setUser(newInstance(User)(nameStr, addr, port));

			m_frameData[numUsers] = newInstance(FrameDataManager)(FALSE);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
		} else {
			m_localSlot = numUsers;
			m_localUser.setName(nameStr);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
			DEBUG_LOG(("Local user is %d", m_localSlot));

			m_frameData[numUsers] = newInstance(FrameDataManager)(TRUE);
		}
		m_frameData[numUsers]->init();
		m_frameData[numUsers]->reset();

		m_packetRouterFallback[numUsers] = numUsers;

		numUsers++;
	}

	if (numUsers < 2 || m_localSlot == -1)
	{
		DEBUG_LOG(("ConnectionManager::parseUserList - FAILED (local user = %d, num players = %d) with list [%s]", m_localSlot, numUsers, buf));
		return;
	}

	free(list); // from the strdup above.
	list = nullptr;
	*/
}

/**
 * Return the number of incoming bytes per second averaged over 30 sec.
 */
Real ConnectionManager::getIncomingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of incoming packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getIncomingPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing bytes per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getOutgoingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingPacketsPerSecond()
{
	if (m_transport) {
		return m_transport->getOutgoingPacketsPerSecond();
	} else {
	  return 0.0;
	}
}

/**
 * Return the number of bytes not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownBytesPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number ov packets not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the smallest packet arrival cushion since this was last called.
 */
UnsignedInt ConnectionManager::getPacketArrivalCushion() {
	UnsignedInt retval = m_smallestPacketArrivalCushion;
	m_smallestPacketArrivalCushion = -1;
	return retval;
}

void ConnectionManager::sendChat(UnicodeString text, RelayMaskType playerMask, UnsignedInt executionFrame)
{
	// GeneralsX: Sanitise BEFORE the message is built, so the wire copy
	// and the local echo below are the same string - a sender who saw their own line rendered
	// differently from everyone else's copy of it would have no way to tell.
	//
	// Bailing out on an empty result also means a line that was nothing but control characters
	// costs no packet at all, rather than producing a bare "[name]" row on every peer.
	if (!GXSanitizeChatText(text))
	{
		return;
	}

	NetChatCommandMsg *msg = newInstance(NetChatCommandMsg);
	msg->setText(text);
	msg->setPlayerMask(playerMask);
	msg->setPlayerID(m_localSlot);
	msg->setID(0);
	msg->setExecutionFrame(executionFrame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType()))
	{
		msg->setID(GenerateNextCommandID());
	}
	// GeneralsX: The message BODY used to be printed here. Chat contents
	// do not belong in a log: this file is written to disk, is routinely pasted into bug reports,
	// and the text is other players' private conversation. The id and mask are the parts that were
	// ever diagnostically useful and they stay.
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Chat message has ID of %d, mask of %16.16llX", msg->getID(), (unsigned long long)msg->getPlayerMask()));

	sendLocalCommand(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
	processChat(msg);

	msg->detach();
}

void ConnectionManager::sendDisconnectChat(UnicodeString text) {
	// Same treatment as sendChat: one sanitiser, applied on the way out as well as on the way in.
	if (!GXSanitizeChatText(text)) {
		return;
	}

	NetDisconnectChatCommandMsg *msg = newInstance(NetDisconnectChatCommandMsg);
	msg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setText(text);

	sendLocalCommandDirect(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
	processDisconnectChat(msg);

	msg->detach();
}

UnsignedShort ConnectionManager::sendFileAnnounce(AsciiString path, RelayMaskType playerMask)
{
	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		log.format(L"Not sending file '%hs' to %llX", path.str(), (unsigned long long)playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return 0;
	}

	theFile->close();

	RelayMaskType announceMask = ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot));
	NetFileAnnounceCommandMsg *announceMsg = newInstance(NetFileAnnounceCommandMsg);
	announceMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(announceMsg->getNetCommandType()) == TRUE) {
		announceMsg->setID(GenerateNextCommandID());
	}
	announceMsg->setRealFilename(path);
	announceMsg->setPlayerMask(playerMask);

	// GeneralsX: If the portable conversion fell back to the raw local
	// path (seen on the wire in the 2026-08-10 session-5 run 3), every receiver's normalization
	// refuses the file SILENTLY and the lobby waits out the full transfer timeout. Refuse at the
	// send side instead, loudly and at once. 0 is the caller-visible failure sentinel - the same
	// one the open-failure path above already returns.
	if (!TheGameState->isPortableFilePath(announceMsg->getPortableFilename()))
	{
		fprintf(stderr, "[GXFILE] NOT announcing '%s' - portable conversion failed ('%s' would be refused by every receiver)\n",
			path.str(), announceMsg->getPortableFilename().str());
		fflush(stderr);
		if (TheLAN)
		{
			UnicodeString log;
			log.format(L"Cannot send '%hs' - untransferable path", path.str());
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		}
		announceMsg->detach();
		return 0;
	}

	UnsignedShort fileID = GenerateNextCommandID();
	if (fileID == 0)
	{
		// 16-bit wraparound: 0 is reserved as the failure sentinel above, so never issue it as a
		// real file ID. Command IDs need uniqueness within the retry window, not contiguity.
		fileID = GenerateNextCommandID();
	}
	announceMsg->setFileID(fileID);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFileAnnounce() - creating announce message with ID of %d from %d to mask %llX for '%s' going to %llX as command %d",
		announceMsg->getID(), announceMsg->getPlayerID(), (unsigned long long)announceMask, announceMsg->getRealFilename().str(),
		(unsigned long long)announceMsg->getPlayerMask(), announceMsg->getFileID()));

	// GeneralsX: GX_MAPDBG: [GXXFER] transfer-pipeline probes. The t= stamp
	// is timeGetTime() ms - the existing probe lines carry no time at all, and sequencing a
	// send against the far peer's receive is exactly what the round-2 run 6 class needs.
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXXFER] t=%u announce-send: fileID=%d cmdID=%d portable='%s' mask=%llX\n",
			GXWallClockMs(), (Int)fileID, (Int)announceMsg->getID(),
			announceMsg->getPortableFilename().str(), (unsigned long long)playerMask);
		fflush(stderr);
	}

	processFileAnnounce(announceMsg); // set up things for the host

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file announce to %llX", (unsigned long long)announceMask));
	sendLocalCommand(announceMsg, announceMask);
	announceMsg->detach();

	return fileID;
}

void ConnectionManager::sendFile(AsciiString path, RelayMaskType playerMask, UnsignedShort commandID)
{
	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		log.format(L"Not sending file '%hs' to %llX", path.str(), (unsigned long long)playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return;
	}

	Int len = theFile->size();
	char *buf = theFile->readEntireAndClose();

	// compress Targas
#ifdef COMPRESS_TARGAS
	char *compressedBuf = nullptr;
	Int compressedLen = path.endsWith(".tga")?CompressionManager::getMaxCompressedSize(len, CompressionManager::getPreferredCompression()):0;
	Int compressedSize = 0;
	if (compressedLen)
		compressedSize = CompressionManager::compressData(CompressionManager::getPreferredCompression(),
		buf, len, compressedBuf, compressedLen);

	if (!compressedSize)
	{
		delete[] compressedBuf;
		compressedBuf = nullptr;
	}
#endif // COMPRESS_TARGAS

	NetFileCommandMsg *fileMsg = newInstance(NetFileCommandMsg);
	fileMsg->setPlayerID(m_localSlot);
	fileMsg->setID(commandID);
	fileMsg->setRealFilename(path);
#ifdef COMPRESS_TARGAS
	if (compressedBuf)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Compressed '%s' from %d to %d (%g%%) before transfer", path.str(), len, compressedSize,
			(Real)compressedSize/(Real)len*100.0f));
		fileMsg->setFileData((unsigned char *)compressedBuf, compressedSize);
	}
	else
#endif // COMPRESS_TARGAS
	{
		fileMsg->setFileData((unsigned char *)buf, len);
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFile() - creating file message with ID of %d for '%s' going to %llX from %d, size of %d",
		fileMsg->getID(), fileMsg->getRealFilename().str(), (unsigned long long)playerMask, fileMsg->getPlayerID(), fileMsg->getFileLength()));

	delete[] buf;
	buf = nullptr;
#ifdef COMPRESS_TARGAS
	delete[] compressedBuf;
	compressedBuf = nullptr;
#endif // COMPRESS_TARGAS

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file: '%s', len %d, to %llX", path.str(), len, (unsigned long long)playerMask));

	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);
	if (s_mapDbg)
	{
		fprintf(stderr, "[GXXFER] t=%u file-send: cmdID=%d portable='%s' len=%d mask=%llX\n",
			GXWallClockMs(), (Int)commandID, fileMsg->getPortableFilename().str(),
			fileMsg->getFileLength(), (unsigned long long)playerMask);
		fflush(stderr);
	}

	sendLocalCommand(fileMsg, playerMask);

	fileMsg->detach();
}

Int ConnectionManager::getFileTransferProgress(Int playerID, AsciiString path)
{
	// GeneralsX @debug /08/2026 GX_MAPDBG: A1 measurement probes. The miss path below
	// returns 0 - the same value as "announced but not started" - so a poll that can never match
	// is indistinguishable from a transfer that never progresses. Log the first hits and the first
	// misses WITH the announced map contents so a run shows which of the two is happening.
	static const Bool s_mapDbg = (getenv("GX_MAPDBG") != NULL);

	// GeneralsX: Compare in PORTABLE space. The caller's path is built
	// locally (getMap() plus the sidecar joins), the stored key is the portable name off the
	// wire, and any spelling difference between the two - case, separators, base directory -
	// used to make every poll miss and return the same 0 as "no progress yet", which reads as a
	// stalled transfer and times the whole game start out. Converting the poll path to the same
	// canonical portable form the announce carried makes the two agree by construction on every
	// platform. See the note in processFileAnnounce for the measured failure.
	const AsciiString portable = TheGameState->realMapPathToPortableMapPath(path);

	FileCommandMap::iterator commandIt = s_fileCommandMap.begin();
	while (commandIt != s_fileCommandMap.end())
	{
		//DEBUG_LOG(("ConnectionManager::getFileTransferProgress(%s): looking at existing transfer of '%s'",
		//	path.str(), commandIt->second.str()));
		if (commandIt->second == portable)
		{
			if (s_mapDbg)
			{
				static Int s_hitLogs = 0;
				if (s_hitLogs < 4)
				{
					++s_hitLogs;
					fprintf(stderr, "[GXMAPDBG] getFileTransferProgress HIT: player=%d path='%s' portable='%s' fileID=%d progress=%d\n",
						playerID, path.str(), portable.str(), (Int)commandIt->first, s_fileProgressMap[playerID][commandIt->first]);
					fflush(stderr);
				}
			}
			return s_fileProgressMap[playerID][commandIt->first];
		}
		++commandIt;
	}
	//DEBUG_LOG(("Falling back to 0, since we couldn't find the map"));
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::getFileTransferProgress: path %s not found",path.str()));
	if (s_mapDbg)
	{
		static Int s_missLogsEmpty = 0;
		static Int s_missLogs = 0;
		if (s_fileCommandMap.empty())
		{
			if (s_missLogsEmpty < 1)
			{
				++s_missLogsEmpty;
				fprintf(stderr, "[GXMAPDBG] getFileTransferProgress MISS (nothing announced yet): player=%d path='%s'\n",
					playerID, path.str());
				fflush(stderr);
			}
		}
		else if (s_missLogs < 8)
		{
			++s_missLogs;
			fprintf(stderr, "[GXMAPDBG] getFileTransferProgress MISS: player=%d path='%s' portable='%s'; announced:\n",
				playerID, path.str(), portable.str());
			for (FileCommandMap::iterator it = s_fileCommandMap.begin(); it != s_fileCommandMap.end(); ++it)
				fprintf(stderr, "[GXMAPDBG]   fileID=%d portable='%s' progress[player]=%d\n",
					(Int)it->first, it->second.str(), s_fileProgressMap[playerID][it->first]);
			fflush(stderr);
		}
	}
	return 0;
}


void ConnectionManager::voteForPlayerDisconnect(Int slot) {
	if (m_disconnectManager != nullptr) {
		m_disconnectManager->voteForPlayerDisconnect(slot, this);
	}
}

Int ConnectionManager::getNumPlayers()
{
	Int retval = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (isPlayerConnected(i)) {
			++retval;
		}
	}

	return retval;
}

void ConnectionManager::updateLoadProgress( Int progress )
{
	NetProgressCommandMsg *msg = newInstance(NetProgressCommandMsg);
	msg->setPercentage( progress );
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processProgress(msg);
	sendLocalCommand(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));

	msg->detach();
}

void ConnectionManager::loadProgressComplete()
{
	NetLoadCompleteCommandMsg *msg = newInstance(NetLoadCompleteCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processLoadComplete(msg);
	sendLocalCommand(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));

	msg->detach();
}

void ConnectionManager::sendTimeOutGameStart()
{
	NetTimeOutGameStartCommandMsg *msg = newInstance(NetTimeOutGameStartCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processTimeOutGameStart(msg);
	sendLocalCommand(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));

	msg->detach();
}

Bool ConnectionManager::isPacketRouter()
{
	return m_localSlot == m_packetRouterSlot;
}

Int ConnectionManager::getAverageFPS()
{
	return m_frameMetrics.getAverageFPS();
}

Int ConnectionManager::getSlotAverageFPS(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1;
	}
	// GeneralsX: A client answers from the router's published row (processLatencyReport), its
	// own slot included: that is the figure every other peer sees and the clamp uses. Retail returned -1 here for its own
	// slot and, never having m_fpsAverages filled for anyone else, -1 for everybody. Stale after the same window as the
	// ping column, so a peer that stopped reporting leaves the board instead of freezing on it.
	if (m_packetRouterSlot != m_localSlot) {
		if (m_peerFps[slot] < 0) {
			return -1;
		}
		time_t staleWindow = (time_t)TheGlobalData->m_networkDisconnectTime;
		const time_t publishFloor = (time_t)TheGlobalData->m_networkRunAheadMetricsTime * 3;
		if (staleWindow < publishFloor) {
			staleWindow = publishFloor;
		}
		if (staleWindow < 2000) {
			staleWindow = 2000;
		}
		return ((timeGetTime() - m_peerFpsTime[slot]) > staleWindow) ? -1 : (Int)m_peerFps[slot];
	}
	return m_fpsAverages[slot];
}

#if defined(RTS_DEBUG)
void ConnectionManager::debugPrintConnectionCommands() {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - begin commands"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - commands for connection %d", i));
			m_connections[i]->debugPrintCommands();
		}
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - end commands"));
}
#endif

void ConnectionManager::notifyOthersOfCurrentFrame(Int frame) {
	NetDisconnectFrameCommandMsg *msg = newInstance(NetDisconnectFrameCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setDisconnectFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - sending disconnect frame of %d, command ID = %d", frame, msg->getID()));
	sendLocalCommandDirect(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(gxSlotBit(m_localSlot));
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - start screen on debug stuff"));
#if defined(RTS_DEBUG)
	debugPrintConnectionCommands();
#endif
}

void ConnectionManager::notifyOthersOfNewFrame(UnsignedInt frame) {
	NetDisconnectScreenOffCommandMsg *msg = newInstance(NetDisconnectScreenOffCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setNewFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	sendLocalCommandDirect(msg, ALL_SLOTS_MASK ^ (gxSlotBit(m_localSlot)));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(gxSlotBit(m_localSlot));
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();
}

void ConnectionManager::sendFrameDataToPlayer(UnsignedInt playerID, UnsignedInt startingFrame) {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame data to player %d starting with frame %d", playerID, startingFrame));
	// GeneralsX: [GXNET] resend. A frame resend - a FRAMERESENDREQUEST answered, or
	// the disconnect screen's catch-up (DisconnectManager::processDisconnectFrame) - is invisible in a release
	// build, where every trace on this path is a DEBUG_LOG, so a test could not tell "resent, and its acks
	// cleared the queue" from "never resent" (scripts/test/net-kill-gates.sh honest-acks). One line per
	// resend, GX_NET only, capped like the [GXGATE] lines. The range is half-open: frames the loop sends.
	static const Bool s_resendTrace = (getenv("GX_NET") != nullptr);
	static Int s_resendsTold = 0;
	if (s_resendTrace && (s_resendsTold < 32)) {
		++s_resendsTold;
		fprintf(stderr, "[GXNET] resend frames [%u,%u) to slot %u\n", startingFrame, (unsigned)TheGameLogic->getFrame(), playerID);
		fflush(stderr);
	}
	for (UnsignedInt frame = startingFrame; frame < TheGameLogic->getFrame(); ++frame) {
		sendSingleFrameToPlayer(playerID, frame);
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - done sending commands to player %d", playerID));
}

void ConnectionManager::sendSingleFrameToPlayer(UnsignedInt playerID, UnsignedInt frame) {
	// GeneralsX: getFrame() is UnsignedInt and FRAMES_TO_KEEP is Int, so the
	// bare subtraction converted to uint32_t and WRAPPED while getFrame() <= FRAMES_TO_KEEP. The test
	// then read ~4.29e9 > frame and refused every resend - both the requestFrameDataResend route and
	// the disconnect screen's catch-up (DisconnectManager.cpp:532/539) - for any stall that began
	// before frame FRAMES_TO_KEEP, and under lockstep the counter does not advance while stalled, so
	// the window is the whole stall rather than the first two seconds. Guard the subtraction first,
	// the way getFrameCommandList already does at :2651.
	if ((TheGameLogic->getFrame() > (UnsignedInt)FRAMES_TO_KEEP) && ((TheGameLogic->getFrame() - FRAMES_TO_KEEP) > frame)) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendSingleFrameToPlayer - player %d requested frame %d when we are on frame %d, this is too far in the past.", playerID, frame, TheGameLogic->getFrame()));
		return;
	}

	RelayMaskType relay = gxSlotBit(playerID);

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending data for frame %d", frame));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_frameData[i] != nullptr) && (i != playerID)) { // no need to send his own commands to him.
			NetCommandList *list = m_frameData[i]->getFrameCommandList(frame);
			if (list != nullptr) {
				NetCommandRef *ref = list->getFirstMessage();
				while (ref != nullptr) {
					DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending command %d from player %d to player %d using relay 0x%llx", ref->getCommand()->getID(), i, playerID, (unsigned long long)relay));
					sendLocalCommandDirect(ref->getCommand(), relay);
					gxNoteResent(playerID, ref->getCommand());
					ref = ref->getNext();
				}
			}
			UnsignedInt frameCommandCount = m_frameData[i]->getFrameCommandCount(frame);
			NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
			msg->setExecutionFrame(frame);
			msg->setCommandCount(frameCommandCount);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}
			msg->setPlayerID(i);
			msg->setResent(TRUE);	// its ack is no latency sample (processAckStage1)
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame info from player %d to player %d for frame %d with command count %d and ID %d and relay %d", i, playerID, msg->getExecutionFrame(), msg->getCommandCount(), msg->getID(), relay));
			sendLocalCommandDirect(msg, relay);
			gxNoteResent(playerID, msg);
			msg->detach();
		}
	}
}

// GeneralsX: Who may send a DESTROYPLAYER naming @p without - see
// Network::processDestroyPlayerCommand. The live m_packetRouterSlot cannot answer that: it moves when
// a DIRECT DISCONNECTPLAYER arrives, at a different moment on every peer. This moves only when an
// executed order says so (lockstepRemove), so every peer answers the same on every frame. It walks
// the plan exactly as disconnectPlayer's migration does - forward from the elected router - so for
// a single drop it names the peer updateDisconnectStatus elects.
UnsignedInt ConnectionManager::lockstepRouter(UnsignedInt without) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		const UnsignedInt slot = m_lockstepPlan[i];
		if ((slot < (UnsignedInt)MAX_SLOTS) && (slot != without) && ((m_lockstepGone & gxSlotBit(slot)) == 0)) {
			return slot;
		}
	}
	return MAX_SLOTS;
}

void ConnectionManager::lockstepRemove(UnsignedInt slot) {
	if (slot < (UnsignedInt)MAX_SLOTS) {
		m_lockstepGone |= (gxSlotBit(slot));
	}
}

UnsignedInt ConnectionManager::getNextPacketRouterSlot(UnsignedInt playerID) {
	// GeneralsX: Same one-past-the-end read as disconnectPlayer had: the bound
	// MAX_SLOTS-1 leaves rank 7 uncompared, and the unconditional ++index then read
	// m_packetRouterFallback[8]. The sole caller, DisconnectManager::isLocalPlayerNextPacketRouter,
	// loops on this until it finds a connected player and only stops on `>= MAX_SLOTS`, so a garbage
	// return either spun that loop or named a slot nobody else would have named.
	//
	// Search the full range and return MAX_SLOTS when there is no successor - the same out-of-range
	// sentinel getPacketRouterFallbackSlot already returns, and exactly what that caller tests for.
	Int index = 0;
	while ((index < MAX_SLOTS) && (m_packetRouterFallback[index] != playerID)) {
		++index;
	}
	for (Int probe = index + 1; probe < MAX_SLOTS; ++probe) {
		const UnsignedInt candidate = m_packetRouterFallback[probe];
		if (candidate < (UnsignedInt)MAX_SLOTS) {
			return candidate;
		}
	}
	return MAX_SLOTS;
}

void ConnectionManager::requestFrameDataResend(Int playerID, UnsignedInt frame) {
	NetFrameResendRequestCommandMsg *msg = newInstance(NetFrameResendRequestCommandMsg);
	msg->setPlayerID(m_localSlot);
	msg->setFrameToResend(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	if (isPlayerConnected(playerID) == FALSE) {
		playerID = 0;
		while ((playerID < MAX_SLOTS) && (isPlayerConnected(playerID) == FALSE)) {
			++playerID;
		}
	}

	if (playerID < MAX_SLOTS) {
		sendLocalCommandDirect(msg, gxSlotBit(playerID));
	}

	msg->detach();
}
