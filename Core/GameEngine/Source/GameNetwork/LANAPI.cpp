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

#define WIN32_LEAN_AND_MEAN  // only bare bones windows stuff wanted

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"	// lanUpdateSlotList - the local echo in RequestAccept
#include "Common/Diagnostic/SimulationId.h"
#include "GameNetwork/networkutil.h"
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "Common/UserPreferences.h"
#include "GameLogic/GameLogic.h"
#include "Common/MultiplayerSettings.h"		// RequestGameOptions: a seat's colour, for GXSLOT
#include "Common/PlayerTemplate.h"			// RequestGameOptions: a seat's faction, for GXSLOT
#include "GameNetwork/GXRelaySlotsWire.h"	// RequestGameOptions: the GXSLOT seat list
#include "Common/GXAdminOrder.h"			// GX_ADMIN_MAX_PER_MATCH

// GeneralsX: The seat states GXRelaySlotsWire.h restates, pinned to SlotState so
// a reordered enum is a build error rather than an AI reported as a human.
static_assert((Int)SLOT_OPEN == (Int)GX_SEAT_OPEN, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert((Int)SLOT_CLOSED == (Int)GX_SEAT_CLOSED, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert((Int)SLOT_EASY_AI == (Int)GX_SEAT_EASY_AI, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert((Int)SLOT_MED_AI == (Int)GX_SEAT_MED_AI, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert((Int)SLOT_BRUTAL_AI == (Int)GX_SEAT_BRUTAL_AI, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert((Int)SLOT_PLAYER == (Int)GX_SEAT_PLAYER, "GXRelaySlotsWire.h GXSeatState must match SlotState");
static_assert(MAX_SLOTS <= GX_RELAY_SEATS_MAX, "GXSLOT carries at most GX_RELAY_SEATS_MAX seats");
// GeneralsX: Host-authoritative camera height limits. Resolves to
// GameClient/GXHostCamera.h when feat/hostcam-apply is present, to inert no-ops when it is not.
#include "GameNetwork/GXHostCamSeam.h"


static const UnsignedShort lobbyPort = 8086; ///< This is the UDP port used by all LANAPI communication

// Lobby password and private flag, carried across a LANAPI rebuild: relayJoinSelectedGame deletes TheLAN and
// builds a new one mid-join, so a member would be lost. A fixed char buffer, not AsciiString, because a
// namespace-scope AsciiString would destruct at exit after the memory pool is gone. Lives until changed,
// cleared with /nopassword, or exit; never written to disk or sent.
static char s_carriedLobbyPassword[65] = { 0 };
static Bool s_carriedLobbyPrivate = FALSE;

AsciiString GetMessageTypeString(UnsignedInt type);

// GeneralsX: Darwin fails every sendto(255.255.255.255) with EHOSTUNREACH - SO_BROADCAST,
// SO_DONTROUTE and IP_BOUND_IF make no difference - so a limited broadcast means the LAN lobby
// never announces itself at all. The subnet-directed broadcast of the interface we are speaking
// from (192.168.10.255, or 172.20.10.255 on a phone hotspot) is delivered normally, and is the
// more precise destination anyway on a multi-homed host: it goes out the interface the player
// actually chose instead of whichever one the default route happens to pick. Falls back to the
// limited broadcast when the interface cannot supply one, which is what Windows always does.
static UnsignedInt findBroadcastAddrForIP( UnsignedInt localIP )
{
	if (localIP == 0)
	{
		return INADDR_BROADCAST;
	}

	IPEnumeration IPs;
	for (EnumeratedIP *ip = IPs.getAddresses(); ip != nullptr; ip = ip->getNext())
	{
		if (ip->getIP() == localIP && ip->getBroadcastIP() != 0)
		{
			return ip->getBroadcastIP();
		}
	}

	return INADDR_BROADCAST;
}

const UnsignedInt LANAPI::s_resendDelta = 10 * 1000;	///< This is how often we announce ourselves to the world

// GeneralsX: The two lobby-name caps must agree, and this is the only
// translation unit that can see both.
//
// GameInfo::setLobbyName caps what a host may type (g_lobbyDisplayNameLength, GameInfo.h);
// Transport caps what may go on the relay wire (MAX_LOBBY_NAME_LEN, Transport.h), and the relay
// caps what it will accept (MAX_LOBBY_LEN, tools/relay/relay.js). Transport.h cannot include
// GameInfo.h for one integer - it is a network header and that would drag the whole lobby model
// into it - so the tie is asserted here instead of hoped for. If they drift, the failure is a
// silently truncated name in the browser but not in the game options, which is exactly the kind of
// mismatch nobody would think to look for.
static_assert(Transport::MAX_LOBBY_NAME_LEN == g_lobbyDisplayNameLength,
	"Transport::MAX_LOBBY_NAME_LEN must equal g_lobbyDisplayNameLength, and both must equal "
	"MAX_LOBBY_LEN in tools/relay/relay.js");
/*
LANGame::LANGame()
{
	m_gameName = L"";

	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		m_playerName[player] = L"";
		m_playerIP[player]= 0;
		m_playerAccepted[player] = false;
	}
	m_lastHeard = 0;
	m_inProgress = false;
	m_next = nullptr;
}
*/




LANAPI::LANAPI() : m_transport(nullptr)
{
	DEBUG_LOG(("LANAPI::LANAPI() - max game option size is %d, sizeof(LANMessage)=%d, MAX_LANAPI_PACKET_SIZE=%d",
		m_lanMaxOptionsLength, sizeof(LANMessage), MAX_LANAPI_PACKET_SIZE));

	m_lastResendTime = 0;
	//
	m_lobbyPlayers = nullptr;
	m_games = nullptr;
	m_name = L""; // safe default?
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_localIP = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_sawNetworkMatch = FALSE;
	m_currentGame = nullptr;
	m_broadcastAddr = INADDR_BROADCAST;	// replaced by the interface's subnet broadcast once SetLocalIP picks an interface
	m_directConnectRemoteIP = 0;
	m_actionTimeout = 5000; // ms
	m_lastUpdate = 0;

	// GeneralsX: Reassert state - see the block comment in update().
	// The memset matters: a stack-garbage m_lastJoinRequest must never be sendable.
	m_hasMapBurstNext = 0;
	m_hasMapBurstLeft = 0;
	memset(&m_lastJoinRequest, 0, sizeof(m_lastJoinRequest));
	m_lastJoinRequestDest = 0;
	m_lastJoinRequestValid = FALSE;
	m_joinResendNext = 0;
	m_gameStartBurstLeft = 0;
	m_gameStartBurstNext = 0;
	memset(&m_lastStartMsg, 0, sizeof(m_lastStartMsg));
	m_lastStartValid = FALSE;
	m_startNonceCounter = 0;
	m_lastStartNonceProcessed = 0;
	m_gxStartAdminUnconfirmed = FALSE;
	m_startResendNext = 0;

	m_transport = new Transport;
	m_isActive = TRUE;

	// GeneralsX: Uninitialised members would otherwise be formatted
	// into the user-facing join-failure dialog as if they were a real peer's version numbers.
	m_remoteSimValid = FALSE;
	memset(&m_remoteSim, 0, sizeof(m_remoteSim));
	m_remoteSimVerdict = SIMID_OK;
	m_pendingSimWarnFlags = 0;

	// GeneralsX: A brand new LAN subsystem has told nobody anything yet.
	// Stack garbage here would MUTE a real desync warning rather than duplicate one, so this is the
	// one of the two failure directions that is silent - which is why it is initialised explicitly
	// even though the relay join path rebuilds LANAPI and would mask the omission most of the time.
	m_saidSimWarnFlags = 0;

	// GeneralsX: Host-camera lobby notice. A stack-garbage flag word would
	// make a brand new LANAPI emit a warning about numbers that were never on any wire.
	m_pendingHostCamFlags = 0;
	m_pendingHostCamHostMin = 0.0f;
	m_pendingHostCamHostMax = 0.0f;
	m_pendingHostCamLocalMin = 0.0f;
	m_pendingHostCamLocalMax = 0.0f;

	// GeneralsX: A fresh LAN subsystem holds no host camera override.
	// Hygiene rather than a fix: the seam refuses to apply a pair outside a multiplayer match and
	// drops it when one ends, and the relay join path DESTROYS AND REBUILDS LANAPI, so this runs
	// on exactly the transition where a carried-over pair would be least expected.
	GXHostCamera::clearHostLimits();

	// GeneralsX: Lobby password / private game state. All of it has to
	// start at a known value: m_hostPwRequired governs whether joins are refused at all, and a
	// stack-garbage TRUE would make a brand new lobby reject everyone with no way to find out why.
	m_hostPwRequired = FALSE;
	m_hostPwSalt = 0;
	memset(m_hostPwVerifier, 0, sizeof(m_hostPwVerifier));
	memset(m_hostPwChallenge, 0, sizeof(m_hostPwChallenge));
	memset(m_hostPwChallengeTime, 0, sizeof(m_hostPwChallengeTime));
	m_hostPwChallengeNext = 0;
	m_hostPrivate = FALSE;

	m_joinPassword.clear();
	m_joinPwHost = 0;
	m_joinPwSalt = 0;
	m_joinPwChallenge = 0;
	m_joinPwOffered = FALSE;
	m_joinPwAutoRetries = 0;
	m_pendingLobbyNotice = FALSE;

	// GeneralsX: Headless and scripted peers have no chat box, so the
	// password arrives through the environment instead - the same channel and the same precedence
	// rule GENERALSX_LANROOM already uses. Applies to BOTH roles: a headless host binds it as the
	// lobby password, a headless joiner answers with it.
	//
	// The environment is read once, into the carried buffer, so a LANAPI rebuilt mid-join by
	// relayJoinSelectedGame reaches the same state whether the password came from the environment
	// or from /password.
	{
		const char *envPassword = getenv("GENERALSX_LANPASS");
		if (envPassword != nullptr && *envPassword != '\0' && s_carriedLobbyPassword[0] == '\0')
		{
			strlcpy(s_carriedLobbyPassword, envPassword, sizeof(s_carriedLobbyPassword));
			DEBUG_LOG(("LANAPI - GENERALSX_LANPASS is set; a game hosted here will require a password"));
		}

		// A private game never advertises. Same channel, same reason.
		const char *envPrivate = getenv("GENERALSX_LANPRIVATE");
		if (envPrivate != nullptr && *envPrivate != '\0' && *envPrivate != '0')
		{
			s_carriedLobbyPrivate = TRUE;
			DEBUG_LOG(("LANAPI - GENERALSX_LANPRIVATE is set; this game will not be advertised"));
		}
	}

	// Adopt whatever the previous instance (or the environment) left behind. setHostPassword mints
	// a NEW salt here, which is correct and deliberate: a rebuilt LANAPI is a new lobby as far as
	// anyone on the wire is concerned, and reusing the old salt would let a verifier captured from
	// the previous one still be meaningful.
	m_hostPrivate = s_carriedLobbyPrivate;
	if (s_carriedLobbyPassword[0] != '\0')
	{
		m_joinPassword = s_carriedLobbyPassword;
		setHostPassword(m_joinPassword);
	}
}

LANAPI::~LANAPI()
{
	reset();
	delete m_transport;
}

void LANAPI::init()
{
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
	m_transport->reset();
	m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_sawNetworkMatch = FALSE;
	m_currentGame = nullptr;
	m_directConnectRemoteIP = 0;

	// GeneralsX: A re-init is a fresh lobby; drop any reassert state.
	m_hasMapBurstNext = 0;
	m_hasMapBurstLeft = 0;
	m_lastJoinRequestValid = FALSE;
	m_joinResendNext = 0;
	m_gameStartBurstLeft = 0;
	m_gameStartBurstNext = 0;
	m_lastStartValid = FALSE;
	m_lastStartNonceProcessed = 0;
	m_gxStartAdminUnconfirmed = FALSE;
	m_startResendNext = 0;

	m_lastGameopt = "";

#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
	char userName[UNLEN + 1];
	DWORD bufSize = ARRAY_SIZE(userName);
	if (GetUserNameA(userName, &bufSize))
	{
		m_userName.set(userName, bufSize - 1);
	}
	else
	{
		m_userName = "unknown";
	}

	char computerName[MAX_COMPUTERNAME_LENGTH + 1];
	bufSize = ARRAY_SIZE(computerName);
	if (GetComputerNameA(computerName, &bufSize))
	{
		m_hostName.set(computerName, bufSize - 1);
	}
	else
	{
		m_hostName = "unknown";
	}
#endif
}

void LANAPI::reset()
{
	if (m_inLobby)
	{
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
		sendMessage(&msg);
	}
	m_transport->update();

	LANGameInfo *theGame = m_games;
	LANGameInfo *deletableGame = nullptr;

	while (theGame)
	{
		deletableGame = theGame;
		theGame = theGame->getNext();
		delete deletableGame;
	}

	LANPlayer *thePlayer = m_lobbyPlayers;
	LANPlayer *deletablePlayer = nullptr;

	while (thePlayer)
	{
		deletablePlayer = thePlayer;
		thePlayer = thePlayer->getNext();
		delete deletablePlayer;
	}

	m_games = nullptr;
	m_lobbyPlayers = nullptr;
	m_directConnectRemoteIP = 0;
	m_pendingAction = ACT_NONE;
	m_expiration = 0;
	m_inLobby = true;
	m_isInLANMenu = TRUE;
	m_sawNetworkMatch = FALSE;
	m_currentGame = nullptr;

	// GeneralsX: The address dies with the lobby too. LanLobbyMenuInit runs
	// reset(), init(), SetLocalIP(nic) on a TheLAN that can have come from the ONLINE screen, so init()
	// bound ONLINE's relay address (10.42.x.x). With the LAN screen local (Transport::enterLocalScreen)
	// nothing widens that bind any more, and on Windows - which binds the address it is given - it is on
	// no interface: a 1 s WSAEADDRNOTAVAIL retry loop and a DEBUG_CRASH on every entry. 0 is what a
	// fresh LANAPI hands init(): the wildcard, until SetLocalIP chooses. Every caller of reset() either
	// calls SetLocalIP next or deletes this object.
	m_localIP = 0;

	// GeneralsX: Reassert state dies with the lobby it belonged to.
	m_hasMapBurstNext = 0;
	m_hasMapBurstLeft = 0;
	m_lastJoinRequestValid = FALSE;
	m_joinResendNext = 0;
	m_gameStartBurstLeft = 0;
	m_gameStartBurstNext = 0;

	// GeneralsX: Same reasoning as in the constructor. reset() is only ever
	// called on entry to the LAN lobby and the Direct Connect screen (LanLobbyMenu,
	// NetworkDirectConnect) - never mid-match, and TheLAN is not in the engine's subsystem list,
	// so nothing calls this behind our back at match start. Verified before adding the call:
	// clearing an adopted pair during a match would be a real defect.
	GXHostCamera::clearHostLimits();
}

// Send the bytes the message needs, not sizeof the whole union (see g_lanLegacyOptionsLength in LANAPI.h):
// padded datagrams were dropped by peers on a 476-capped build. Only MSG_GAME_OPTIONS grew, so only it is
// measured by its string length. The rest go at the legacy extent: the receiver casts the datagram to
// LANMessage with no length check (LANAPI.cpp:678), so under-sending would silently misparse.
Int LANAPI::wireSize(const LANMessage *msg)
{
	// GeneralsX: One definition, in LANAPI.h, so the self-test measures
	// the function the engine sends with. A long MSG_GAME_ANNOUNCE now goes out at its own length
	// too - see LANMessageWireSize.
	return LANMessageWireSize(msg);
}

void LANAPI::sendMessage(LANMessage *msg, UnsignedInt ip /* = 0 */)
{
	const Int msgSize = wireSize(msg);

	if (ip != 0)
	{
		m_transport->queueSend(ip, lobbyPort, (unsigned char *)msg, msgSize /*, 0, 0 */);
	}
	else if ((m_currentGame != nullptr) && (m_currentGame->getIsDirectConnect()))
	{
		// GeneralsX: Trace the unicast fan-out too.
		//
		// The broadcast arm below has always been traced and this one was not, on the reasoning
		// that an established peer has already demonstrated discovery. That left a real failure
		// invisible: a client that receives nothing after its join handshake cannot be told apart
		// from a host that is sending to the wrong address, because the host log shows no send
		// either way. Trace the fan-out and the slot it is aimed at, so "sent to nobody",
		// "sent to the wrong IP" and "sent and lost" are three different pictures.
		Int localSlot = m_currentGame->getLocalSlotNum();
		Int sentTo = 0;
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			if (i != localSlot) {
				GameSlot *slot = m_currentGame->getSlot(i);
				if ((slot != nullptr) && (slot->isHuman())) {
					fprintf(stderr, "[LAN] unicast %s to slot %d %d.%d.%d.%d:%u\n",
						GetMessageTypeString(msg->messageType).str(), i,
						PRINTF_IP_AS_4_INTS(slot->getIP()), (unsigned)lobbyPort);
					m_transport->queueSend(slot->getIP(), lobbyPort, (unsigned char *)msg, msgSize /*, 0, 0 */);
					++sentTo;
				}
			}
		}
		if (sentTo == 0)
		{
			fprintf(stderr, "[LAN] unicast %s went to NOBODY - no human slot other than local slot %d\n",
				GetMessageTypeString(msg->messageType).str(), localSlot);
		}
		fflush(stderr);
	}
	else
	{
		// GeneralsX: Release-visible. Pairs with the [LAN] recv trace in
		// update(): together they tell a send that never happened from a send that was never
		// received. Only the broadcast arm is traced - the unicast arms are driven by an already
		// established peer, so by then discovery has demonstrably worked.
		fprintf(stderr, "[LAN] send %s to %d.%d.%d.%d:%u\n",
			GetMessageTypeString(msg->messageType).str(),
			PRINTF_IP_AS_4_INTS(m_broadcastAddr), (unsigned)lobbyPort);
		fflush(stderr);

		m_transport->queueSend(m_broadcastAddr, lobbyPort, (unsigned char *)msg, msgSize /*, 0, 0 */);
	}
}


AsciiString GetMessageTypeString(UnsignedInt type)
{
	AsciiString returnString;

	switch (type)
	{
		case LANMessage::MSG_REQUEST_LOCATIONS:
			returnString.format( "Request Locations (%d)",type);
			break;
		case LANMessage::MSG_GAME_ANNOUNCE:
			returnString.format("Game Announce (%d)",type);
			break;
		case LANMessage::MSG_LOBBY_ANNOUNCE:
			returnString.format("Lobby Announce (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_JOIN:
			returnString.format("Request Join (%d)",type);
			break;
		case LANMessage::MSG_JOIN_ACCEPT:
			returnString.format("Join Accept (%d)",type);
			break;
		case LANMessage::MSG_JOIN_DENY:
			returnString.format("Join Deny (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_LEAVE:
			returnString.format("Request Game Leave (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_LOBBY_LEAVE:
			returnString.format("Request Lobby Leave (%d)",type);
			break;
		case LANMessage::MSG_SET_ACCEPT:
			returnString.format("Set Accept(%d)",type);
			break;
		case LANMessage::MSG_MAP_AVAILABILITY:
			// GeneralsX: Was missing, so every availability announce logged
			// as "Unknown Message (9)" - which cost a whole evening of mis-classified evidence.
			returnString.format("Map Status (%d)",type);
			break;
		case LANMessage::MSG_CHAT:
			returnString.format("Chat (%d)",type);
			break;
		case LANMessage::MSG_GAME_START:
			returnString.format("Game Start (%d)",type);
			break;
		case LANMessage::MSG_GAME_START_TIMER:
			returnString.format("Game Start Timer (%d)",type);
			break;
		case LANMessage::MSG_GAME_OPTIONS:
			returnString.format("Game Options (%d)",type);
			break;
		case LANMessage::MSG_REQUEST_GAME_INFO:
			returnString.format("Request GameInfo (%d)", type);
			break;
		case LANMessage::MSG_INACTIVE:
			returnString.format("Inactive (%d)", type);
			break;
		default:
			returnString.format("Unknown Message (%d)",type);
	}
	return returnString;
}


void LANAPI::checkMOTD()
{
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_useLocalMOTD)
	{
		// for a playtest, let's log some play statistics, eh?
		if (TheGlobalData->m_playStats <= 0)
			TheWritableGlobalData->m_playStats = 30;

		static UnsignedInt oldMOTDCRC = 0;
		UnsignedInt newMOTDCRC = 0;
		AsciiString asciiMOTD;
		char buf[4096];
		FILE *fp = fopen(TheGlobalData->m_MOTDPath.str(), "r");
		Int len;
		if (fp)
		{
			while( (len = fread(buf, 1, 4096, fp)) > 0 )
			{
				buf[len] = 0;
				asciiMOTD.concat(buf);
			}
			fclose(fp);
			CRC crcObj;
			crcObj.computeCRC(asciiMOTD.str(), asciiMOTD.getLength());
			newMOTDCRC = crcObj.get();
		}

		if (oldMOTDCRC != newMOTDCRC)
		{
			// different MOTD... display it
			oldMOTDCRC = newMOTDCRC;
			AsciiString line;
			while (asciiMOTD.nextToken(&line, "\n"))
			{
				if (line.getCharAt(line.getLength()-1) == '\r')
					line.removeLastChar();	// there is a trailing '\r'

				if (line.isEmpty())
				{
					line = " ";
				}

				UnicodeString uniLine;
				uniLine.translate(line);
				OnChat( L"MOTD", 0, uniLine, LANCHAT_SYSTEM );
			}
		}
	}
#endif
}

extern Bool LANbuttonPushed;
extern Bool LANSocketErrorDetected;
void LANAPI::update()
{
	// Reset the already-said tier2 mask whenever we are not in a game, so a surviving mask cannot suppress a real
	// desync warning in the next lobby. Keyed on state, not on a leave event, so a new leave path cannot forget it.
	// Above the early returns. Not tied to isGameInProgress: a match is the same lobby session.
	if (m_inLobby || m_currentGame == nullptr)
		m_saidSimWarnFlags = 0;

	// GeneralsX: Deferred tier2 warnings for the joining side. Must
	// run before both early returns: LANbuttonPushed stays TRUE from OnGameJoin(RET_OK) until
	// LanGameOptionsMenuInit clears it, and the 200 ms throttle below returns just as early -
	// that window is exactly when the chat listbox comes into existence.
	if (m_pendingSimWarnFlags != 0)
		flushPendingSimWarnings();

	// GeneralsX: Same deferral, same window, same reason - see the note
	// on flushPendingLobbyNotice. Must be above the early returns for the same reason as above.
	if (m_pendingLobbyNotice)
		flushPendingLobbyNotice();

	// GeneralsX: And the host-camera notice, third user of the same
	// deferral. Above the early returns for the same reason: the window in which the LAN game
	// options listbox comes into existence is exactly the window in which both returns fire.
	if (m_pendingHostCamFlags != 0)
		flushPendingHostCameraNotice();

	// GeneralsX: Say so when the latch kills the pump.
	//
	// This return disables BOTH halves of LANAPI - no periodic send and, via the
	// `!LANbuttonPushed` condition on the receive loop below, no draining of the inbound queue.
	// A peer in that state looks alive, draws its lobby, and is deaf and mute. It is worth one
	// throttled line, because from the outside the symptom is "the host stopped talking to me"
	// and the cause is entirely local.
	if(LANbuttonPushed)
	{
		static UnsignedInt s_lastLatchLog = 0;
		const UnsignedInt nowLatch = timeGetTime();
		if (nowLatch - s_lastLatchLog > 5000)
		{
			s_lastLatchLog = nowLatch;
			fprintf(stderr, "[LAN] update() SUPPRESSED - LANbuttonPushed is set, no send and no receive\n");
			fflush(stderr);
		}

		// Keep the joiner keepalive running while LANbuttonPushed is set. The latch is a UI guard, not network
		// state, but returning early also stops RequestGameOptions(HELLO), the only thing refreshing the joiner
		// slot on the host, which reaps it after s_resendDelta*8 = 80s. Only the keepalive runs, not the
		// receive loop: draining inbound can fire OnGameJoin/OnGameCreate and push shell screens.
		const UnsignedInt nowKeep = timeGetTime();
		if (nowKeep > s_resendDelta + m_lastResendTime
			&& m_currentGame != NULL && !m_currentGame->isGameInProgress() && !AmIHost())
		{
			m_lastResendTime = nowKeep;
			RequestGameOptions( "HELLO", false );
		}
		return;
	}
	static const UnsignedInt LANAPIUpdateDelay = 200;
	UnsignedInt now = timeGetTime();

	// Drain the socket on every call: the 200 ms delay below throttles sends, not receives. Servicing inbound
	// only after it delayed relay messages by up to 200 ms and quantised the lobby latency readout.
	// Safe: Transport::update() is doRecv() + doSend(), and doSend() rate-limits the relay keepalive itself
	// (RELAY_REGISTRATION_INTERVAL, 5 s).
	if ((m_transport->update() == FALSE) && (LANSocketErrorDetected == FALSE)) {
		if (m_isInLANMenu == TRUE) {
			LANSocketErrorDetected = TRUE;
		}
	}

	// The relay listing is derived state: we advertise exactly while hosting a game that exists. RequestGameOptions
	// arms it and this disarms it the frame that stops being true. An armed advertisement is re-sent every 5 s and
	// never expires on the relay (ADVERT_TIMEOUT_MS, 15 s). A running match still counts, so a player who left can
	// find it. withdrawGameAdvertisement() tells the relay the game is gone instead of waiting for expiry.
	{
		const Bool matchLive = (TheGameLogic != NULL)
			&& (TheGameLogic->isInLanGame() || TheGameLogic->isInInternetGame());

		// The latch below exists because OnGameStart marks the LANGameInfo in progress and only
		// QUEUES the MSG_NEW_GAME that puts GameLogic into GAME_LAN. So for the frames in between,
		// "in progress" is true and isInLanGame() is still false - and a naive `inProgress implies
		// isInLanGame` rule would delist the game at the exact instant it starts, which is the §3.4
		// bug d08348a4f fixed. Requiring isInLanGame() only AFTER we have actually observed it
		// closes both ends: the start window keeps advertising, and the end of the match - which is
		// the case with no other signal at all, because nothing calls LANAPI when a host walks out
		// of a finished game back to the shell - stops it.
		if (matchLive)
			m_sawNetworkMatch = TRUE;

		const Bool hosting = (m_currentGame != NULL) && AmIHost();
		const Bool shouldAdvertise = hosting
			&& (!m_currentGame->isGameInProgress() || !m_sawNetworkMatch || matchLive);

		if (!shouldAdvertise && m_transport->isAdvertising())
		{
			fprintf(stderr, "[LAN] delisting - not hosting a live game (game=%d host=%d inProgress=%d sawMatch=%d matchLive=%d)\n",
				(Int)(m_currentGame != NULL), (Int)AmIHost(),
				(Int)((m_currentGame != NULL) ? m_currentGame->isGameInProgress() : FALSE),
				(Int)m_sawNetworkMatch, (Int)matchLive);
			fflush(stderr);
			m_transport->withdrawGameAdvertisement();
		}
	}

	if( now > m_lastUpdate + LANAPIUpdateDelay)
	{
		m_lastUpdate = now;
	}
	else
	{
		return;
	}

	// Handle any new messages
	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer) && !LANbuttonPushed; ++i)
	{
		if (m_transport->m_inBuffer[i].length > 0)
		{
			// Process the new message
			UnsignedInt senderIP = m_transport->m_inBuffer[i].addr;
			if (senderIP == m_localIP)
			{
				m_transport->m_inBuffer[i].length = 0;
				continue;
			}

			LANMessage *msg = (LANMessage *)(m_transport->m_inBuffer[i].data);
			// GeneralsX: Terminate before any handler reads a string; see
			// SanitizeReceivedLANMessage in LANAPI.h.
			static_assert(sizeof(m_transport->m_inBuffer[i].data) >= sizeof(LANMessage),
				"the receive slot must hold a whole LANMessage for the blind cast and the sanitizer");
			SanitizeReceivedLANMessage(msg, (size_t)m_transport->m_inBuffer[i].length);
			//DEBUG_LOG(("LAN message type %s from %ls (%s@%s)", GetMessageTypeString(msg->messageType).str(),
			//	msg->name, msg->userName, msg->hostName));

			// GeneralsX: Release-visible inbound trace. This is the only
			// place that can distinguish the two failures that look identical from outside the
			// process: nothing arrived (firewall drop, wrong broadcast address, wrong interface)
			// versus something arrived and was rejected further down. Both otherwise present as a
			// lobby that sits empty and times out after 5 s with no output whatsoever.
			//
			// Watch for senderIP equal to one of OUR OWN addresses: the self-filter above compares
			// against m_localIP only, so when the announced identity and the interface the kernel
			// actually sends from disagree, our own broadcast fails that test and the machine
			// appears in its own lobby. That is the signature of a bad interface election.
			fprintf(stderr, "[LAN] recv %s from %d.%d.%d.%d len=%d\n",
				GetMessageTypeString(msg->messageType).str(), PRINTF_IP_AS_4_INTS(senderIP),
				(int)m_transport->m_inBuffer[i].length);
			fflush(stderr);

			// GeneralsX: ANY packet from a slot proves that slot is alive.
			//
			// Defence in depth for the drop described at the top of update(). The host used to
			// refresh a slot's last-heard from exactly one message type, MSG_GAME_OPTIONS, so a
			// peer that was demonstrably talking could still be timed out and evicted: one
			// session logged 13 Set Accept and 11 Game Options arriving from a peer the host had
			// already dropped. Liveness is a property of the socket, not of one message type.
			//
			// This does not replace the keepalive fix - a peer really can go silent, and the 80s
			// eviction is what stops a dead slot wedging the lobby forever. It removes the case
			// where the eviction fires against a peer whose packets are in this very loop.
			if (AmIHost() && m_currentGame != NULL && senderIP != m_localIP)
			{
				for (Int lp = 1; lp < MAX_SLOTS; ++lp)
				{
					if (m_currentGame->getIP(lp) == senderIP)
					{
						m_currentGame->setPlayerLastHeard(lp, timeGetTime());
						break;
					}
				}
			}

			switch (msg->messageType)
			{
				// Location specification
			case LANMessage::MSG_REQUEST_LOCATIONS:		// Hey, where is everybody?
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOCATIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLocations( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_ANNOUNCE:				// Here someone is, and here's his game info!
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_LOBBY_ANNOUNCE:			// Hey, I'm in the lobby!
				DEBUG_LOG(("LANAPI::update - got a MSG_LOBBY_ANNOUNCE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleLobbyAnnounce( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_GAME_INFO:
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_INFO from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameInfo( msg, senderIP );
				break;

				// Joining games
			case LANMessage::MSG_REQUEST_JOIN:				// Let me in!  Let me in!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_JOIN from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestJoin( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_ACCEPT:					// Okay, you can join.
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_ACCEPT from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinAccept( msg, senderIP );
				break;
			case LANMessage::MSG_JOIN_DENY:						// Go away!  We don't want any!
				DEBUG_LOG(("LANAPI::update - got a MSG_JOIN_DENY from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleJoinDeny( msg, senderIP );
				break;

				// Leaving games, lobby
			case LANMessage::MSG_REQUEST_GAME_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_GAME_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestGameLeave( msg, senderIP );
				break;
			case LANMessage::MSG_REQUEST_LOBBY_LEAVE:				// I'm outa here!
				DEBUG_LOG(("LANAPI::update - got a MSG_REQUEST_LOBBY_LEAVE from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleRequestLobbyLeave( msg, senderIP );
				break;

				// Game options, chat, etc
			case LANMessage::MSG_SET_ACCEPT:					// I'm cool with everything as is.
				handleSetAccept( msg, senderIP );
				break;
			case LANMessage::MSG_MAP_AVAILABILITY:		// Map status
				handleHasMap( msg, senderIP );
				break;
			case LANMessage::MSG_CHAT:								// Just spouting my mouth off.
				handleChat( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START:					// Hold on; we're starting!
				handleGameStart( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_START_TIMER:
				handleGameStartTimer( msg, senderIP );
				break;
			case LANMessage::MSG_GAME_OPTIONS:				// Here's some info about the game.
				DEBUG_LOG(("LANAPI::update - got a MSG_GAME_OPTIONS from %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(senderIP)));
				handleGameOptions( msg, senderIP );
				break;
			case LANMessage::MSG_INACTIVE:		// someone is telling us that we're inactive.
				handleInActive( msg, senderIP );
				break;

			default:
				DEBUG_LOG(("Unknown LAN message type %d", msg->messageType));
			}

			// Mark it as read
			m_transport->m_inBuffer[i].length = 0;
		}
		// Scan the whole buffer, do not break at the first empty slot. LANbuttonPushed ends this loop and handlers set
		// it mid-drain, so a drain can stop at slot k with k..255 occupied behind a hole. Transport::doRecv restarts
		// its free-slot scan at 0 each call, so a break would strand messages above the hole (join, accept, leave
		// replies). Cost: MAX_MESSAGES ints at 5 Hz.
	}
	// GeneralsX: The second gate. Distinct message from the first on
	// purpose: reaching here means the latch was CLEAR at the top of update() and got SET while
	// this very call was processing inbound messages - almost certainly by OnGameJoin or
	// OnGameCreate firing out of the receive loop above. That is a different bug from arriving
	// with it already set, and the two were indistinguishable while neither was logged.
	if(LANbuttonPushed)
	{
		static UnsignedInt s_lastMidLatchLog = 0;
		if (now - s_lastMidLatchLog > 5000)
		{
			s_lastMidLatchLog = now;
			fprintf(stderr, "[LAN] periodic send SUPPRESSED - LANbuttonPushed was set during this update()\n");
			fflush(stderr);
		}

		// GeneralsX: The SECOND gate needs the keepalive too.
		//
		// The first fix only patched the early return at the top of update(). Driving the Windows
		// GUI through a real join proved that insufficient: the gate that actually fired was this
		// one - "periodic send SUPPRESSED" x1, "update() SUPPRESSED" x0 - because OnGameJoin sets
		// the latch out of the receive loop above, i.e. DURING the very call that would otherwise
		// have sent. Same consequence either way: the joiner skips the one message the host counts
		// as a sign of life, and the host evicts it 80 s later.
		const UnsignedInt nowKeep2 = timeGetTime();
		if (nowKeep2 > s_resendDelta + m_lastResendTime
			&& m_currentGame != NULL && !m_currentGame->isGameInProgress() && !AmIHost())
		{
			m_lastResendTime = nowKeep2;
			RequestGameOptions( "HELLO", false );
		}
		return;
	}
	// GeneralsX: Say why the keepalive is or is not going out.
	//
	// Measured over a driven join: the client sent 5 Game Options at join and then nothing for 90
	// seconds, with NEITHER latch gate firing in that window. So update() was running past both
	// gates and the periodic block still was not sending, and no log anywhere said which of its
	// four preconditions was false. GX_NET-gated, 1 Hz.
	{
		static const Bool s_netTrace = (getenv("GX_NET") != NULL);
		static UnsignedInt s_lastWhy = 0;
		if (s_netTrace && (now - s_lastWhy) > 1000)
		{
			s_lastWhy = now;
			fprintf(stderr, "[GXNET] keepalive gate: dueIn=%dms inLobby=%d haveGame=%d "
				"inProgress=%d amHost=%d latch=%d\n",
				(Int)((s_resendDelta + m_lastResendTime) - now), (Int)m_inLobby,
				(Int)(m_currentGame != NULL),
				(Int)((m_currentGame != NULL) ? m_currentGame->isGameInProgress() : FALSE),
				(Int)AmIHost(), (Int)LANbuttonPushed);
			fflush(stderr);
		}
	}

	// Send out periodic I'm Here messages
	if (now > s_resendDelta + m_lastResendTime)
	{
		m_lastResendTime = now;

		if (m_inLobby)
		{
			RequestSetName(m_name);
		}
		else if (m_currentGame && !m_currentGame->isGameInProgress())
		{
			if (AmIHost())
			{
				RequestGameOptions( GenerateGameOptionsString(), true );
				RequestGameAnnounce();
			}
			else
			{
#if TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
				AsciiString text;
				text.format("User=%s", m_userName.str());
				RequestGameOptions( text, true );
				text.format("Host=%s", m_hostName.str());
				RequestGameOptions( text, true );
#endif
				RequestGameOptions( "HELLO", false );
				// GeneralsX: Re-announce map availability with the
				// keepalive. Closes the tail the burst below can miss (all three shots lost, or
				// a status change the host's stale slot list keeps overriding on other peers).
				sendHasMapNow();
			}
		}
		else if (m_currentGame)
		{
			// game is in progress - RequestGameAnnounce will check if we should send it
			RequestGameAnnounce();
		}
	}

	// Reassert unacked datagrams that gate decisions: a lost hasMap=false leaves peers assuming the sender has the map
	// (slots default TRUE); a lost join request or accept times out. hasMap: extra shots at +1 s and +2 s after each
	// announce, plus a 10 s piggyback on the joiner keepalive. Join: the stored request is re-sent verbatim each
	// second while ACT_JOIN is pending; re-deriving it would re-read one-shot password state (m_joinPwOffered).
	if (m_hasMapBurstLeft > 0 && m_hasMapBurstNext != 0 && now >= m_hasMapBurstNext
		&& !m_inLobby && m_currentGame != NULL && !m_currentGame->isGameInProgress())
	{
		--m_hasMapBurstLeft;
		m_hasMapBurstNext = (m_hasMapBurstLeft > 0) ? now + 2000 : 0;
		fprintf(stderr, "[LAN] reasserting map availability (%d left)\n", m_hasMapBurstLeft);
		fflush(stderr);
		sendHasMapNow();
	}

	if ((m_pendingAction == ACT_JOIN || m_pendingAction == ACT_JOINDIRECTCONNECT)
		&& m_lastJoinRequestValid
		&& now >= m_joinResendNext && now < m_expiration)
	{
		m_joinResendNext = now + 1000;
		fprintf(stderr, "[LAN] re-sending %s (no reply yet)\n",
			(m_pendingAction == ACT_JOIN) ? "join request" : "game info request");
		fflush(stderr);
		sendMessage(&m_lastJoinRequest, m_lastJoinRequestDest);
	}

	// Host only: the MSG_GAME_START burst armed by RequestGameStart. Datagram only - the local
	// OnGameStart already ran. Re-sends the STORED message so the nonce rides along: receivers
	// that ran this start drop it by nonce at any time (the isGameInProgress gate alone is
	// disarmed during their drain-before-teardown epilogue and after a transfer-only restore),
	// and a receiver that MISSED every earlier copy is started by this one - the 1.5 s x 5
	// spacing exists to outlive that epilogue (bounded 10 s), the original run-8 shape.
	if (m_gameStartBurstLeft > 0 && m_gameStartBurstNext != 0 && now >= m_gameStartBurstNext
		&& !m_inLobby && m_currentGame != NULL && m_currentGame->getIP(0) == m_localIP
		&& m_lastStartValid)
	{
		--m_gameStartBurstLeft;
		m_gameStartBurstNext = (m_gameStartBurstLeft > 0) ? now + 1500 : 0;
		fprintf(stderr, "[LAN] re-sending game start (%d left, nonce %u)\n",
			m_gameStartBurstLeft, m_lastStartMsg.GameStart.nonce);
		fflush(stderr);
		sendMessage(&m_lastStartMsg);
	}

	Bool playerListChanged = false;
	Bool gameListChanged = false;

	// Weed out people we haven't heard from in a while
	LANPlayer *player = m_lobbyPlayers;
	while (player)
	{
		if (player->getLastHeard() + s_resendDelta*2 < now)
		{
			// He's gone!
			removePlayer(player);
			LANPlayer *nextPlayer = player->getNext();
			delete player;
			player = nextPlayer;
			playerListChanged = true;
		}
		else
		{
			player = player->getNext();
		}
	}

	// Weed out people we haven't heard from in a while
	LANGameInfo *game = m_games;
	while (game)
	{
		if (game != m_currentGame && game->getLastHeard() + s_resendDelta*2 < now)
		{
			// He's gone!
			removeGame(game);
			LANGameInfo *nextGame = game->getNext();
			delete game;
			game = nextGame;
			gameListChanged = true;
		}
		else
		{
			game = game->getNext();
		}
	}
	if ( m_currentGame && !m_currentGame->isGameInProgress() )
	{
		if ( !AmIHost() && (m_currentGame->getLastHeard() + s_resendDelta*16 < now) )
		{
			// Do not offer back a game whose host just stopped answering. A crashed host sent no leave or
			// withdrawal, so the relay serves its listing until its own 15 s expiry. Suppressed on the room, taken
			// from the transport before handleRequestGameLeave deletes the game. Bounded by the same expiry, so a
			// host that is still alive reappears.
			if (m_transport != NULL && m_transport->isRelayEnabled())
				Transport::suppressRoomListing(m_transport->getRelayRoom());

			// We haven't heard from the host in a while.  Bail.
			// Actually, fake a host leaving message. :)
			LANMessage msg;
			fillInLANMessage( &msg );
			msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
			// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
			CopyWcharToWindowsWideChar(msg.name, m_currentGame->getPlayerName(0).str(), ARRAY_SIZE(msg.name) - 1);
			handleRequestGameLeave(&msg, m_currentGame->getIP(0));
			UnicodeString text;
			text = TheGameText->fetch("LAN:HostNotResponding");
			OnChat(UnicodeString::TheEmptyString, m_localIP, text, LANCHAT_SYSTEM);
		}
		else if ( AmIHost() )
		{
			// Check each player for timeouts
			for (int p=1; p<MAX_SLOTS; ++p)
			{
				if (m_currentGame->getIP(p) && m_currentGame->getPlayerLastHeard(p) + s_resendDelta*8 < now)
				{
					LANMessage msg;
					fillInLANMessage( &msg );
					UnicodeString theStr;
					theStr.format(TheGameText->fetchGameOnly("LAN:PlayerDropped"), m_currentGame->getPlayerName(p).str());
					msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
					// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(msg.name, m_currentGame->getPlayerName(p).str(), ARRAY_SIZE(msg.name) - 1);
					handleRequestGameLeave(&msg, m_currentGame->getIP(p));
					OnChat(UnicodeString::TheEmptyString, m_localIP, theStr, LANCHAT_SYSTEM);
				}
			}
		}
	}

	if (playerListChanged)
	{
		OnPlayerList(m_lobbyPlayers);
	}

	if (gameListChanged)
	{
		OnGameList(m_games);
	}

	// Time out old actions
	if (m_pendingAction != ACT_NONE && now > m_expiration)
	{
		switch (m_pendingAction)
		{
		case ACT_JOIN:
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_LEAVE:
			OnPlayerLeave(m_name);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		case ACT_JOINDIRECTCONNECT:
			OnGameJoin(RET_TIMEOUT, nullptr);
			m_pendingAction = ACT_NONE;
			m_currentGame = nullptr;
			m_inLobby = true;
			break;
		default:
			m_pendingAction = ACT_NONE;
		}
	}

	// send out "game starting" messages
	if ( m_gameStartTime && m_gameStartSeconds && m_gameStartTime <= now )
	{
		// m_gameStartTime is when the next message goes out
		// m_gameStartSeconds is how many seconds remain in the message

		RequestGameStartTimer( m_gameStartSeconds );
	}
	else if (m_gameStartTime && m_gameStartTime <= now)
	{
//		DEBUG_LOG(("m_gameStartTime=%d, now=%d, m_gameStartSeconds=%d", m_gameStartTime, now, m_gameStartSeconds));
		ResetGameStartTimer();
		RequestGameStart();
	}

	// Check for an MOTD every few seconds
	static UnsignedInt lastMOTDCheck = 0;
	static const UnsignedInt motdInterval = 30000;
	if (now > lastMOTDCheck + motdInterval)
	{
		checkMOTD();
		lastMOTDCheck = now;
	}
}

// Request functions generate network traffic
void LANAPI::RequestLocations()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOCATIONS;
	fillInLANMessage( &msg );
	sendMessage(&msg);
}

void LANAPI::RequestGameJoin( LANGameInfo *game, UnsignedInt ip /* = 0 */ )
{
	if ((m_pendingAction != ACT_NONE) && (m_pendingAction != ACT_JOINDIRECTCONNECT))
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (!game)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}

	// GeneralsX: Set the verdict BEFORE calling OnGameJoin, or the
	// dialog's switch has no matching case and will either render empty or quote a stale peer
	// that was never contacted. This fires when GameEngine::init threw between the XferCRC and
	// the m_iniCRC assignment and was swallowed by the handlers at the bottom of init.
	if (!SimulationId::get().valid)
	{
		m_remoteSimValid = FALSE;
		memset(&m_remoteSim, 0, sizeof(m_remoteSim));
		m_remoteSimVerdict = SIMID_LOCAL_INVALID;
		// GeneralsX: Terminal. Reached from
		// handleGameAnnounce mid-direct-connect, leaving ACT_JOINDIRECTCONNECT
		// pending would let the game-info reassert keep knocking: every announce would re-enter here and
		// reopen this dialog, and the player would get a second, misleading RET_TIMEOUT at expiry.
		// A machine that failed to load its own data has nothing to retry.
		m_pendingAction = ACT_NONE;
		m_expiration = 0;
		m_lastJoinRequestValid = FALSE;
		OnGameJoin( RET_CRC_MISMATCH, nullptr );
		return;
	}

	// GeneralsX: A user-initiated join resets the automatic password
	// retry budget. Without this, the second game you try in one lobby session would inherit an
	// exhausted budget from the first and never get past the "needs a password" refusal.
	m_joinPwAutoRetries = 0;

	sendJoinRequest( game->getSlot(0)->getIP(), ip );

	m_pendingAction = ACT_JOIN;
	m_expiration = timeGetTime() + m_actionTimeout;
}

// GeneralsX: The one place a MSG_REQUEST_JOIN is built.
//
// Factored out of RequestGameJoin because handleJoinDeny has to send the identical message again
// with a password attached, and two copies of a join request would eventually disagree about the
// SimID or the serial - a divergence that would present as a version-mismatch refusal on a retry
// only, which is a miserable thing to debug.
void LANAPI::sendJoinRequest( UnsignedInt gameIP, UnsignedInt destIP )
{
	LANMessage msg;
	// GeneralsX: The appended sim field must not ship stack garbage.
	// This memset must stay HERE and must NOT be hoisted into fillInLANMessage: this function
	// and RequestLocations both set messageType BEFORE calling fillInLANMessage, and
	// MSG_REQUEST_LOCATIONS is enum value 0, so a memset in there would silently rewrite every
	// join request into a location request.
	//
	// It is also what leaves pwTag zero on an ordinary join, which is how the host tells "no
	// password offered" from "password offered".
	memset(&msg, 0, sizeof(msg));
	msg.messageType = LANMessage::MSG_REQUEST_JOIN;
	fillInLANMessage( &msg );
	msg.GameToJoin.gameIP = gameIP;
	msg.GameToJoin.exeCRC = TheGlobalData->m_exeCRC;
	msg.GameToJoin.iniCRC = TheGlobalData->m_iniCRC;
	SimulationId::get().toWire( msg.GameToJoin.sim );

	// GeneralsX: Declare what kind of build this is, so the host can
	// refuse a mismatch. The tag is what separates "a plain release build" (flags 0, tag present)
	// from "a peer too old to have been asked" (tag absent) - the receive path blind-casts the
	// datagram, so without the sentinel those two are the same eight bytes of dead union.
	msg.GameToJoin.buildTag   = BUILDKIND_WIRE_TAG;
	msg.GameToJoin.buildFlags = gxLocalBuildKind();

	// GeneralsX: Declare OUR camera height limits to the host. Nobody
	// adopts these - the host's pair governs the lobby, not ours - so this is not a negotiation.
	// It exists so the host can be told which of its players are having their own settings
	// overridden, and by how much; the host is the only machine that can do anything about it.
	//
	// Cleared rather than left alone when we cannot answer, for the same reason as everywhere
	// else. The memset above already zeroes it and must stay - this is the statement of intent,
	// and it is what keeps the three fill sites reading identically.
	//
	// Stamped BEFORE m_lastJoinRequest is taken below, so the 1 Hz reassert re-sends the same
	// bytes. A retry that disagreed with the original would be a second, contradictory answer.
	{
		GXHostCamera::Limits mine;
		if (GXHostCamera::getLocalLimits(&mine))
			GXHostCamWireWrite(msg.GameToJoin.cam, mine.minHeight, mine.maxHeight);
		else
			GXHostCamWireClear(msg.GameToJoin.cam);
	}

	AsciiString s;
	GetStringFromRegistry("\\ergc", "", s);
	strlcpy(msg.GameToJoin.serial, s.str(), ARRAY_SIZE(msg.GameToJoin.serial));

	// GeneralsX: Attach a password answer when we have one for THIS
	// host. The salt and challenge only ever arrive in a refusal from that host, so the first
	// knock at any game is always password-free - which is exactly what an unprotected game
	// expects to receive, and is why no advertisement flag is needed anywhere.
	if (m_joinPwOffered && m_joinPwHost == gameIP && !m_joinPassword.isEmpty())
	{
		UnsignedByte verifier[LOBBYPW_DIGEST_BYTES];
		LobbyPasswordVerifier( m_joinPwSalt, m_joinPassword.str(), verifier );
		LobbyPasswordResponse( verifier, m_joinPwChallenge, msg.GameToJoin.pwResponse );
		msg.GameToJoin.pwTag = LOBBYPW_WIRE_TAG;
		msg.GameToJoin.pwChallenge = m_joinPwChallenge;
		memset(verifier, 0, sizeof(verifier));

		// One challenge, one answer. If this one is refused the host mints a new one in the next
		// refusal; keeping the old one would only ever produce a second identical failure.
		m_joinPwOffered = FALSE;
	}

	// GeneralsX: Keep the exact bytes for the 1 Hz reassert in
	// update(). Verbatim on purpose: rebuilding the request on a retry would re-read password
	// state this function deliberately consumes (m_joinPwOffered is one-shot, just above), and
	// two builds of the same request that disagree would present as a version-mismatch refusal
	// on the retry only. The password auto-retry in handleJoinDeny re-enters here and re-stamps,
	// so the reassert always carries the newest request.
	m_lastJoinRequest = msg;
	m_lastJoinRequestDest = destIP;
	m_lastJoinRequestValid = TRUE;
	m_joinResendNext = timeGetTime() + 1000;

	sendMessage(&msg, destIP);
}

// GeneralsX: Host: mint a single-use challenge.
UnsignedInt LANAPI::issuePasswordChallenge()
{
	const UnsignedInt challenge = LobbyPasswordRandom();

	// A plain ring rather than an LRU: the only way to overflow it is eight simultaneous knocks,
	// which is more than the lobby has seats, and the cost of dropping the oldest is one extra
	// round trip for whoever it belonged to.
	m_hostPwChallenge[m_hostPwChallengeNext] = challenge;
	m_hostPwChallengeTime[m_hostPwChallengeNext] = timeGetTime();
	m_hostPwChallengeNext = (m_hostPwChallengeNext + 1) % LOBBYPW_CHALLENGE_RING;

	return challenge;
}

// GeneralsX: Host: verify one answer. THIS IS THE GATE.
Bool LANAPI::checkPasswordResponse( UnsignedInt challenge, const UnsignedByte *response )
{
	if (!m_hostPwRequired || challenge == 0)
		return FALSE;

	const UnsignedInt now = timeGetTime();

	for (Int i = 0; i < LOBBYPW_CHALLENGE_RING; ++i)
	{
		if (m_hostPwChallenge[i] != challenge)
			continue;

		// Expired challenges are dropped rather than compared, so a datagram captured now cannot
		// be held and used later. timeGetTime wraps every ~49 days; the unsigned subtraction is
		// correct across the wrap, which a `stored + lifetime < now` comparison would not be.
		if ((now - m_hostPwChallengeTime[i]) > (UnsignedInt)LOBBYPW_CHALLENGE_LIFETIME_MS)
		{
			m_hostPwChallenge[i] = 0;
			return FALSE;
		}

		UnsignedByte expected[LOBBYPW_DIGEST_BYTES];
		LobbyPasswordResponse( m_hostPwVerifier, challenge, expected );
		const Bool ok = LobbyPasswordEqual( expected, response );
		memset(expected, 0, sizeof(expected));

		if (ok)
			m_hostPwChallenge[i] = 0;		// single use - a replay of this packet now finds nothing

		return ok;
	}

	// Unknown or already-consumed challenge. Refused, and the caller mints a fresh one in the
	// refusal so an honest joiner recovers on the next attempt.
	return FALSE;
}

// GeneralsX: Host: bind (or clear) the lobby password.
void LANAPI::setHostPassword( const AsciiString &password )
{
	// Every outstanding challenge was minted against the old verifier. Dropping them means a
	// joiner who is mid-handshake when the host changes the password is refused and retries,
	// rather than being let in on the password that was just revoked.
	memset(m_hostPwChallenge, 0, sizeof(m_hostPwChallenge));
	memset(m_hostPwChallengeTime, 0, sizeof(m_hostPwChallengeTime));
	m_hostPwChallengeNext = 0;

	if (password.isEmpty())
	{
		m_hostPwRequired = FALSE;
		m_hostPwSalt = 0;
		memset(m_hostPwVerifier, 0, sizeof(m_hostPwVerifier));
		return;
	}

	// A fresh salt per binding, so the same password typed in two lobbies produces two unrelated
	// verifiers and nothing precomputed against one is worth anything against the other.
	m_hostPwSalt = LobbyPasswordRandom();
	LobbyPasswordVerifier( m_hostPwSalt, password.str(), m_hostPwVerifier );
	m_hostPwRequired = TRUE;
}

// GeneralsX: The chat-command entry point. See the note on the
// declaration in LANAPI.h for why this is a chat command and not a text box.
Bool LANAPI::applyLobbyCommand( const AsciiString &line, UnicodeString &reply )
{
	// isEmpty first: getCharAt has a DEBUG_ASSERTCRASH on the index, so reading [0] out of an
	// empty string is a debug-build crash even though it returns 0 in release.
	if (line.isEmpty() || line.getCharAt(0) != '/')
		return FALSE;

	AsciiString rest = line.str() + 1;
	AsciiString verb;
	rest.nextToken(&verb);
	verb.toLower();
	rest.trim();

	if (verb == "password" || verb == "pass")
	{
		// NOTE the ordering: the password is bound and the local echo is built here, and
		// RequestChat returns without sending. The typed line never reaches sendMessage.
		m_joinPassword = rest;
		setHostPassword( rest );
		strlcpy(s_carriedLobbyPassword, rest.str(), sizeof(s_carriedLobbyPassword));

		if (rest.isEmpty())
		{
			reply = UnicodeString(L"Lobby password cleared. Anyone may join a game you host.");
		}
		else if (m_currentGame != nullptr && AmIHost())
		{
			reply = UnicodeString(L"Password set. Players joining this game must supply it; anyone already in a slot stays.");
		}
		else if (m_currentGame != nullptr)
		{
			reply = UnicodeString(L"Password stored. It will be used the next time you join a protected game.");
		}
		else
		{
			reply = UnicodeString(L"Password set. It applies to the next game you host, and to protected games you join.");
		}
		return TRUE;
	}

	if (verb == "nopassword")
	{
		m_joinPassword.clear();
		setHostPassword( AsciiString::TheEmptyString );
		memset(s_carriedLobbyPassword, 0, sizeof(s_carriedLobbyPassword));
		reply = UnicodeString(L"Lobby password cleared. Anyone may join a game you host.");
		return TRUE;
	}

	if (verb == "private")
	{
		m_hostPrivate = TRUE;
		s_carriedLobbyPrivate = TRUE;
		// Pull an existing listing immediately rather than waiting for the next slot-list push;
		// "private" that takes ten seconds to take effect is not private.
		if (m_transport != nullptr)
			m_transport->clearGameAdvertisement();
		reply = UnicodeString(L"Private. This game is not listed in the browser and is not announced on the LAN - joiners need the room token.");
		return TRUE;
	}

	if (verb == "public")
	{
		m_hostPrivate = FALSE;
		s_carriedLobbyPrivate = FALSE;
		reply = UnicodeString(L"Public. This game will be listed again on the next update.");
		return TRUE;
	}

	if (verb == "lobbyhelp")
	{
		reply = UnicodeString(L"/password <text> - require a password  |  /nopassword - clear it  |  /private, /public - hide or list this game");
		return TRUE;
	}

	return FALSE;
}

void LANAPI::RequestGameJoinDirectConnect(UnsignedInt ipaddress)
{
	if (m_pendingAction != ACT_NONE)
	{
		OnGameJoin( RET_BUSY, nullptr );
		return;
	}

	if (ipaddress == 0)
	{
		OnGameJoin( RET_GAME_GONE, nullptr );
		return;
	}

	m_directConnectRemoteIP = ipaddress;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_INFO;
	fillInLANMessage(&msg);
	msg.PlayerInfo.ip = GetLocalIP();
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName) - 1);

	// GeneralsX: The direct-connect handshake's FIRST datagram gets the
	// same 1 Hz reassert as the join request (see update()). This was deferred in the first cut
	// as "the relay path uses ACT_JOIN" - measured wrong the same night: the harness joins by
	// IP, so its opening MSG_REQUEST_GAME_INFO was still a single unacked shot, and a netem run
	// lost it and died with RET_TIMEOUT exactly like the baseline. sendJoinRequest re-stamps
	// this slot when the flow advances to the real join, so the reassert always carries
	// whichever request is currently unanswered.
	m_lastJoinRequest = msg;
	m_lastJoinRequestDest = ipaddress;
	m_lastJoinRequestValid = TRUE;
	m_joinResendNext = timeGetTime() + 1000;

	sendMessage(&msg, ipaddress);

	m_pendingAction = ACT_JOINDIRECTCONNECT;
	m_expiration = timeGetTime() + m_actionTimeout;
}

void LANAPI::RequestGameLeave()
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_GAME_LEAVE;
	fillInLANMessage( &msg );
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.PlayerInfo.playerName, m_name.str(), ARRAY_SIZE(msg.PlayerInfo.playerName) - 1);
	sendMessage(&msg);
	m_transport->update();  // Send immediately, before OnPlayerLeave below resets everything.

	// Stop advertising the moment we leave so a browser does not offer a game that has gone. Withdraw here, not
	// just clear: this runs before OnPlayerLeave pops the screen, and returning to the browser runs
	// SetUpOnlineLobbyLAN(), which deletes TheLAN and its transport. A guest holds no advertisement, so this
	// is a no-op for it.
	m_transport->withdrawGameAdvertisement();

	// GeneralsX: We are leaving this game, so whatever its host told us
	// about camera limits stops applying. Callers are the game-options Back button and the
	// headless harness's teardown - both unambiguously outside a running match.
	GXHostCamera::clearHostLimits();

	// A joiner leaves its own screen at once, like the host. Leaving is unilateral: handleRequestGameLeave has no
	// refuse branch, so there is nothing to wait for. Waiting on ACT_LEAVE ran OnPlayerLeave twice and popped one
	// screen too many. If the datagram is lost the host reaps the slot. The host game stays in the joiner list
	// (we only drop our own game), so removeGame is not called for a joiner.
	const Bool wasHost = (m_currentGame != nullptr) && (m_currentGame->getIP(0) == m_localIP);

	OnPlayerLeave(m_name);

	if (wasHost && m_currentGame)
	{
		removeGame(m_currentGame);
		// TheSuperHackers @bugfix tintinhamans 10/10/2026 Delete the game we hosted once it is off the list (#3462).
		// A guest's game is an entry of the host's that stays listed, and is not ours to delete.
		delete m_currentGame;
	}

	m_currentGame = nullptr;
	m_pendingAction = ACT_NONE;
	m_inLobby = true;
}

void LANAPI::RequestGameAnnounce()
{
	// GeneralsX: A private game does not broadcast either. The relay
	// half is handled in RequestGameOptions; this is the LAN half - MSG_GAME_ANNOUNCE is what
	// puts a row in every other machine's LAN browser.
	//
	// handleRequestGameInfo deliberately still answers a DIRECTED MSG_REQUEST_GAME_INFO, which is
	// what makes a private game joinable at all: someone who is told the address (or the relay
	// room) can still dial it. "Private" here means unlisted, not unreachable - and unlisted is
	// exactly the claim being made. Access control is the password, not the absence of a listing.
	if (m_hostPrivate)
		return;

	// In game - are we a game host?
	if (m_currentGame && !(m_currentGame->getIsDirectConnect()))
	{
		if (m_currentGame->getIP(0) == m_localIP || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			// TheSuperHackers @bugfix bobtista 23/08/2026 Build the options string first and bail on
			// empty, so an unfittable lobby announces nothing rather than a truncated payload every
			// receiver would reject.
			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			if (gameOpts.isEmpty())
			{
				return;
			}

			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
			CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			sendMessage(&reply);
		}
	}
}

void LANAPI::RequestAccept()
{
	if (m_inLobby || !m_currentGame)
		return;

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_SET_ACCEPT;
	msg.Accept.isAccepted = true;
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.Accept.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.Accept.gameName) - 1);
	sendMessage(&msg);

	// Show the accept on the machine that pressed it. The tick is drawn from slot->isAccepted() (UpdateSlotList)
	// and nothing set it locally. Safe because the host cannot refuse an accept (handleSetAccept -> OnAccept has no
	// reject branch) and the next MSG_GAME_OPTIONS overwrites it either way. Host excluded: it is the writer of
	// record and goes through OnAccept.
	if (!AmIHost())
	{
		const Int localSlot = m_currentGame->getLocalSlotNum();
		if (localSlot >= 0 && localSlot < MAX_SLOTS)
		{
			LANGameSlot *slot = m_currentGame->getLANSlot(localSlot);
			if (slot != nullptr && !slot->isAccepted())
			{
				slot->setAccept();
				lanUpdateSlotList();
			}
		}
	}
}

// GeneralsX: The datagram half of RequestHasMap, callable from the
// reassert timer in update() without re-printing the transfer notice or re-arming the burst.
void LANAPI::sendHasMapNow()
{
	if (m_inLobby || !m_currentGame)
		return;

	// GeneralsX: Announce the TRUTH, never the unexamined default. A fresh
	// slot's hasMap is TRUE by construction, and the headless driver announces immediately after
	// joining - before any options parse has run the map-cache recheck - so the joiner's first
	// datagram claimed it had a map it had never looked for. The host accepted it, started, and
	// the honest post-parse FALSE was a single unacked datagram away from a stranded lobby
	// (measured: every one-announce netem run failed). LanGameOptionsMenuInit already forces
	// this exact recheck before ITS announce; doing it here covers every caller forever.
	m_currentGame->setMapCRC( m_currentGame->getMapCRC() );
	m_currentGame->setMapSize( m_currentGame->getMapSize() );

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_MAP_AVAILABILITY;
	msg.MapStatus.hasMap = m_currentGame->getSlot(m_currentGame->getLocalSlotNum())->hasMap();
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.MapStatus.gameName, m_currentGame->getName().str(), ARRAY_SIZE(msg.MapStatus.gameName) - 1);
	CRC mapNameCRC;
//mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
	AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
	mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
	msg.MapStatus.mapCRC = mapNameCRC.get();

	// GeneralsX: Sender half of the handleHasMap drop probe: what CRC
	// we are announcing, over what string. If the receiver logs a DROP with a different CRC,
	// these two lines are the whole diagnosis.
	fprintf(stderr, "[LAN] announcing map availability: hasMap=%d mapCRC=%08X portable='%s'\n",
		(int)msg.MapStatus.hasMap, msg.MapStatus.mapCRC, portableMapName.str());
	fflush(stderr);

	sendMessage(&msg);
}

void LANAPI::RequestHasMap()
{
	if (m_inLobby || !m_currentGame)
		return;

	sendHasMapNow();

	// GeneralsX: Arm the reassert burst - see update(). One announce is
	// one unacked datagram, and a lost hasMap=false reads as "has the map" (slots default TRUE).
	m_hasMapBurstLeft = 2;
	m_hasMapBurstNext = timeGetTime() + 1000;

	if (!m_currentGame->getSlot(m_currentGame->getLocalSlotNum())->hasMap())
	{
		UnicodeString text;
		UnicodeString mapDisplayName;
		const MapMetaData *mapData = TheMapCache->findMap( m_currentGame->getMap() );
		Bool willTransfer = TRUE;
		if (mapData)
		{
			mapDisplayName.format(L"%ls", mapData->m_displayName.str());
			if (mapData->m_isOfficial)
				willTransfer = FALSE;
		}
		else
		{
			mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(m_currentGame->getMap()).str());
			willTransfer = WouldMapTransfer(m_currentGame->getMap());
		}
		// GeneralsX: M1 step 2b: when this peer HAS the host's .map and
		// only its map.ini/solo.ini differ, "you do not have the map" is false and points the player at the wrong fix.
		const Bool mapDataOnly = mapData != nullptr && mapData->m_CRC == m_currentGame->getMapCRC()
			&& GetMapDataHash(m_currentGame->getMap()) != m_currentGame->getMapDataHash();
		if (mapDataOnly && willTransfer)
			text = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:LocalMapDataDiffersWillTransfer",
				L"Your map.ini/solo.ini for %ls differ from the host's; the host's will be sent when the game starts.",
				mapDisplayName.str());
		else if (mapDataOnly)
			text = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:LocalMapDataDiffers",
				L"Your map.ini/solo.ini for %ls differ from the host's. An official map's files are never sent: remove "
				L"your changed copy (or install the host's) to play.", mapDisplayName.str());
		else if (willTransfer)
			text.format(TheGameText->fetchGameOnly("GUI:LocalPlayerNoMapWillTransfer"), mapDisplayName.str());
		else
			text.format(TheGameText->fetchGameOnly("GUI:LocalPlayerNoMap"), mapDisplayName.str());
		OnChat(L"SYSTEM", m_localIP, text, LANCHAT_SYSTEM);
	}
}

void LANAPI::RequestChat( UnicodeString message, ChatType format )
{
	// GeneralsX: Lobby commands are intercepted HERE, before anything
	// is built or sent, because /password must never reach the wire in any form. This is the one
	// funnel both LanLobbyMenu and LanGameOptionsMenu call, in both the Generals and the Zero
	// Hour tree, so one interception covers every LAN screen on all four platforms.
	{
		AsciiString ascii;
		ascii.translate(message);

		UnicodeString reply;
		if (applyLobbyCommand(ascii, reply))
		{
			OnChat(UnicodeString::TheEmptyString, m_localIP, reply, LANCHAT_SYSTEM);
			return;
		}
	}

	LANMessage msg;
	fillInLANMessage( &msg );
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.Chat.gameName, (m_currentGame) ? m_currentGame->getName().str() : L"", ARRAY_SIZE(msg.Chat.gameName) - 1);
	msg.messageType = LANMessage::MSG_CHAT;
	msg.Chat.chatType = format;
	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg.Chat.message, message.str(), ARRAY_SIZE(msg.Chat.message) - 1);
	sendMessage(&msg);

	OnChat(m_name, m_localIP, message, format);
}

void LANAPI::RequestGameStart()
{
	if (m_inLobby || !m_currentGame || m_currentGame->getIP(0) != m_localIP)
		return;

	m_currentGame->stampAiColours();	// GeneralsX: AI in three colours: the host's choice, only with one human

	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START;
	fillInLANMessage( &msg );

	// GeneralsX: Stamp a fresh nonce so every re-send of THIS start -
	// the immediate double-send, the post-launch burst, and the delivery-window resends from
	// doFileTransfer - is verbatim and idempotent at the receiver. handleGameStart drops a
	// repeated nonce at any time, which is what makes re-sending safe after a transfer-only
	// round restores the lobby (the legacy !isGameInProgress gate is disarmed exactly there,
	// and a repeat would be a stray start that launches the peers without the host).
	++m_startNonceCounter;
	if (m_startNonceCounter == 0)
		++m_startNonceCounter;	// 0 means "none yet" on the receive side; never issue it
	msg.GameStart.nonceTag = STARTNONCE_WIRE_TAG;
	msg.GameStart.nonce = m_startNonceCounter;

	// Host camera height limits, stamped on the authoritative message; every peer resolves its view from this pair.
	// Written unconditionally, including the clear: msg is an unzeroed stack LANMessage, so skipping the else would
	// let stale bytes pass as a host decision. Stamped before m_lastStartMsg is stored so burst and delivery-window
	// resends stay byte-identical (the nonce makes them inert at a peer that already ran this start).
	{
		GXHostCamera::Limits mine;
		if (GXHostCamera::getLocalLimits(&mine))
			GXHostCamWireWrite(msg.GameStart.cam, mine.minHeight, mine.maxHeight);
		else
			GXHostCamWireClear(msg.GameStart.cam);
	}

	// GeneralsX: THE RELAY-PROVEN ADMIN SLOTS AND THEIR KEYS, stamped on the authoritative
	// message, from an answer asked for NOW (one round trip, bounded) and tied to that question by its nonce. With no
	// fresh answer nothing is stamped: this peer grants nothing on its own word, and every other peer refuses to start
	// unless a fresh answer of its own says exactly what is stamped (OnGameStart). Set on our own game too, so this
	// peer's simulation and the replay header run on exactly what every other peer adopts (handleGameStart).
	{
		RelayMaskType adminMask = 0;
		unsigned char adminKeys[MAX_SLOTS][32];
		memset(adminKeys, 0, sizeof(adminKeys));
		// GeneralsX: The host waits longer than a joiner does (its 2 s, OnGameStart): the
		// question is re-asked every 250 ms until an answer lands, so this only lengthens a start whose relay is not
		// answering - and a host with no answer at the end stamps "unconfirmed", never a bare empty mask.
		const Bool relay = (m_transport != NULL && m_transport->isRelayEnabled());
		Bool fresh = relay && m_transport->refreshRelayAdminKeysNow(5000);
#if defined(GX_TEST_INSTRUMENTS)
		// GX_TEST_ADMIN_NOANSWER=1 - a TEST INSTRUMENT: the host's admin query ends as if it had timed out, while every
		// joiner's own query is answered. scripts/test/net-kill-gates.sh admin-host-noanswer.
		if (fresh && getenv("GX_TEST_ADMIN_NOANSWER") != NULL)
		{
			fresh = FALSE;
			fprintf(stderr, "[GXFORGE] the host's admin query dropped, as a timeout\n");
		}
#endif
		if (fresh)
			gxRelayAdmins(m_currentGame, &adminMask, adminKeys);
		else if (relay)
		{
			fprintf(stderr, "[GXADMIN] start: no fresh answer from the relay - stamping no admin, marked unconfirmed\n");
			fflush(stderr);
		}
#if defined(GX_TEST_INSTRUMENTS)
		// GX_TEST_ADMIN_MASK=<hex> - a TEST INSTRUMENT: a host that stamps a mask the relay does not back, which is
		// what a modified client can send. Bits the relay named keep their keys; any other gets a made-up one.
		// scripts/test/net-kill-gates.sh admin-forged-mask and admin-stripped-mask put it through a match.
		if (const char *forged = getenv("GX_TEST_ADMIN_MASK"))
		{
			adminMask = (RelayMaskType)strtoull(forged, NULL, 16) & ALL_SLOTS_MASK;
			for (Int i = 0; i < MAX_SLOTS; ++i)
				if ((adminMask & (gxSlotBit(i))) != 0 && adminKeys[i][0] == 0 && adminKeys[i][31] == 0)
					memset(adminKeys[i], 0x5A, 32);
			fprintf(stderr, "[GXFORGE] admin mask forged to 0x%llX\n", (unsigned long long)adminMask);
		}
		// GX_TEST_ADMIN_KEYSWAP=1 - a host that keeps the relay's admin slots but swaps their keys for its own, so it
		// could sign in an admin's name. scripts/test/net-kill-gates.sh admin-swapped-key.
		if (getenv("GX_TEST_ADMIN_KEYSWAP") != NULL)
		{
			for (Int i = 0; i < MAX_SLOTS; ++i)
				if ((adminMask & (gxSlotBit(i))) != 0)
					adminKeys[i][7] ^= 0x01;
			fprintf(stderr, "[GXFORGE] admin keys swapped for mask 0x%llX\n", (unsigned long long)adminMask);
		}
#endif
		m_currentGame->clearGxAdmin();
		m_currentGame->setGxAdminMask(adminMask);
		for (Int i = 0; i < MAX_SLOTS; ++i)
			if ((adminMask & (gxSlotBit(i))) != 0)
				m_currentGame->setGxAdminKey(i, adminKeys[i]);
		GXAdminWireWrite(msg.GameStart.admin, *m_currentGame, relay && !fresh && adminMask == 0);
		fprintf(stderr, "[GXADMIN] start: stamped admin mask 0x%llX\n", (unsigned long long)m_currentGame->getGxAdminMask());
		fflush(stderr);
	}

	m_lastStartMsg = msg;
	m_lastStartValid = TRUE;
	m_startResendNext = 0;
	m_lastStartNonceProcessed = m_startNonceCounter;	// we run it ourselves below

	sendMessage(&msg);
	m_transport->update(); // force a send

	// A peer that misses the single start datagram stays in the lobby, so send redundant copies. An immediate second
	// copy covers a map-delivery run (OnGameStart blocks in DoAnyMapTransfers); doFileTransfer re-sends the stored
	// start every 2 s inside the delivery window (update() does not run there); the timer burst of five shots at 1.5 s
	// covers a real launch and a peer still in its drain-before-teardown epilogue (bounded 10 s).
	sendMessage(&msg);
	m_transport->update();

	m_gameStartBurstLeft = 5;
	m_gameStartBurstNext = timeGetTime() + 1500;

	OnGameStart();

	// Transfer-only start: lobby restored, in-progress false. The delivery window is over, so
	// the resend path disarms with it; a late burst datagram would start a peer that missed the
	// round INTO a delivery run whose host has already left it, so the timers die too. (Peers
	// that DID run this start would drop the repeats by nonce - the kill is about not starting
	// the stragglers into a window that no longer exists.)
	if (m_currentGame == NULL || !m_currentGame->isGameInProgress())
	{
		m_gameStartBurstLeft = 0;
		m_gameStartBurstNext = 0;
		m_lastStartValid = FALSE;
	}
}

// GeneralsX: Re-emit the stored MSG_GAME_START, verbatim (same
// nonce). Called from the transfer loop while the host blocks in its delivery run - the one
// window where a lost start has NO further chance to arrive (the joiner misses the single... then double... start,
// never builds a TheNetwork, and the host sends 60 s of unackable chunks into a void).
// Self-throttled to one datagram per 2 s.
void LANAPI::resendPendingGameStart()
{
	if (m_inLobby || !m_currentGame || m_currentGame->getIP(0) != m_localIP || !m_lastStartValid)
		return;

	UnsignedInt now = timeGetTime();
	if (m_startResendNext != 0 && now < m_startResendNext)
		return;
	m_startResendNext = now + 2000;

	fprintf(stderr, "[LAN] re-sending game start (delivery window, nonce %u)\n",
		m_lastStartMsg.GameStart.nonce);
	fflush(stderr);
	sendMessage(&m_lastStartMsg);
	m_transport->update();
}

void LANAPI::ResetGameStartTimer()
{
	m_gameStartTime = 0;
	m_gameStartSeconds = 0;
}

void LANAPI::RequestGameStartTimer( Int seconds )
{
	if (m_inLobby || !m_currentGame || m_currentGame->getIP(0) != m_localIP)
		return;

	UnsignedInt now = timeGetTime();
	m_gameStartTime = now + 1000;
	m_gameStartSeconds = (seconds) ? seconds - 1 : 0;

	LANMessage msg;
	msg.messageType = LANMessage::MSG_GAME_START_TIMER;
	msg.StartTimer.seconds = seconds;
	fillInLANMessage( &msg );
	sendMessage(&msg);
	m_transport->update(); // force a send

	OnGameStartTimer(seconds);
}

void LANAPI::RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip /* = 0 */ )
{
	// TheSuperHackers @bugfix bobtista 23/08/2026 The buffer is char[m_lanMaxOptionsLength+1], so a
	// string of exactly m_lanMaxOptionsLength bytes fits with its terminator. Reject at > , not >= .
	DEBUG_ASSERTCRASH(gameOptions.getLength() <= m_lanMaxOptionsLength, ("Game options string is too long!"));

	if (!m_currentGame || gameOptions.isEmpty())
	{
		return;
	}

	LANMessage msg;
	fillInLANMessage( &msg );
	msg.messageType = LANMessage::MSG_GAME_OPTIONS;
	strlcpy(msg.GameOptions.options, gameOptions.str(), ARRAY_SIZE(msg.GameOptions.options));
	sendMessage(&msg, ip);

	m_lastGameopt = gameOptions;

	// Refresh the relay listing whenever the slot list changes: the UI host and the headless host both pass through
	// here, so the advertised count tracks the lobby without a second timer. A relay host lists itself unless
	// /private is set. That is local policy, neither advertised nor persisted: the relay lists a room only while
	// advertisements keep arriving, so withdrawing is the whole implementation.
	if (m_transport && m_transport->isRelayEnabled() && AmIHost() && m_hostPrivate)
	{
		m_transport->clearGameAdvertisement();
	}
	else if (m_transport && m_transport->isRelayEnabled() && AmIHost())
	{
		// The denominator is the seats this lobby really has, not MAX_SLOTS: relayJoinSelectedGame refuses a join on
		// players >= slots. occupied = SLOT_PLAYER plus the three AI states; open = SLOT_OPEN only (SLOT_CLOSED counts
		// for neither); capacity = occupied + open, capped by the map m_numPlayers when the cache reports a sane
		// count, else slot arithmetic, because 0 slots is refused by the relay parser and would delist the game.
		Int players = 0;
		Int openSeats = 0;
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			GameSlot *slot = m_currentGame->getSlot(s);
			if (slot == nullptr)
				continue;
			if (slot->isOccupied())
				++players;
			else if (slot->isOpen())
				++openSeats;
		}

		Int advertisedSlots = players + openSeats;
		const MapMetaData *advMapData = TheMapCache ? TheMapCache->findMap(m_currentGame->getMap()) : nullptr;
		if (advMapData != nullptr && advMapData->m_numPlayers > 0 && advMapData->m_numPlayers < advertisedSlots)
			advertisedSlots = advMapData->m_numPlayers;
		if (advertisedSlots < players)
			advertisedSlots = players;			// an over-capacity lobby still has the people in it
		if (advertisedSlots < 1)
			advertisedSlots = 1;				// the relay refuses slots < 1 and would drop the listing

		// NOT the game's name. RequestGameCreate builds that as "<hexIP><hexSeed>" and then
		// truncates to g_lanGameNameLength, which is 16 - exactly the length of the hex prefix -
		// so the name the host actually asked for is always cut off and every game ends up called
		// something like "0A2A000101249D48". It is a uniqueness token, not a display name.
		// A browser wants to show who is hosting, so advertise the host player's name.
		AsciiString gameName;
		GameSlot *hostSlot = m_currentGame->getSlot(0);
		if (hostSlot != nullptr && !hostSlot->getName().isEmpty())
			gameName.translate(hostSlot->getName());
		else
			gameName.translate(m_currentGame->getName());

		// Advertise the map's DISPLAY name, not its path. getMap() returns what the slot list
		// carries - "maps\twilight flame\twilight flame.map" - and putting that in a browser row
		// overflows the widget, which then renders the TAIL of the string: the player is shown
		// "s\twilight flame\twilight flame.map)" and nothing that identifies the game.
		AsciiString mapPath = m_currentGame->getMap();
		const char *mapLeaf = mapPath.str();
		for (const char *c = mapPath.str(); *c != '\0'; ++c)
		{
			if (*c == '\\' || *c == '/')
				mapLeaf = c + 1;
		}

		char mapName[64];
		strlcpy(mapName, mapLeaf, sizeof(mapName));

		// Drop a trailing ".map" - it is on every entry, so it costs width and distinguishes
		// nothing.
		const Int nameLen = (Int)strlen(mapName);
		if (nameLen > 4 && _stricmp(mapName + nameLen - 4, ".map") == 0)
			mapName[nameLen - 4] = '\0';

		// The lobby name the host typed, for the browser. The LAN announce is room-scoped and cannot reach it, so it
		// travels separately as GXLNAM (see Transport::setRelayLobbyName). Called every pass, even when empty: the
		// relay keeps it on a per-room record that outlives one game, and only an empty name retracts it. Calling is
		// not sending (it compares first). Called before the advertisement so a new game is never listed with the old name.
		m_transport->setRelayLobbyName(m_currentGame->getLobbyName().str());

		m_transport->setGameAdvertisement(gameName.str(), mapName, players, advertisedSlots);

		// GeneralsX: Every seat, for the admin console: what each one holds - a
		// human, an AI and its difficulty, open, closed - and its faction, team and colour where chosen, with
		// the MAP's player count as the denominator the advertisement cannot carry. GXRelaySlotsWire.h has the
		// format and why it is its own tag. After the advertisement, because the relay accepts it only from
		// the room's advertiser. Calling is not sending: setRelaySlots compares first.
		//
		// The names are the DATA's own - the template's INI name and the colour's TooltipName - so a colour
		// or general added in data needs no client change to be reported.
		GXRelaySeat seats[MAX_SLOTS];
		AsciiString factions[MAX_SLOTS];
		AsciiString colours[MAX_SLOTS];
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			GameSlot *slot = m_currentGame->getSlot(s);
			seats[s] = { SLOT_CLOSED, "", -1, "", 0 };
			if (slot == nullptr)
				continue;
			const Int t = slot->getPlayerTemplate();
			const PlayerTemplate *pt = (t >= 0 && ThePlayerTemplateStore) ? ThePlayerTemplateStore->getNthPlayerTemplate(t) : nullptr;
			if (pt != nullptr)
				factions[s] = pt->getName();
			const Int c = slot->getColor();
			const MultiplayerColorDefinition *def = (c >= 0 && TheMultiplayerSettings) ? TheMultiplayerSettings->getColor(c) : nullptr;
			if (def != nullptr)
				colours[s] = def->getTooltipName();
			// An observer is a SLOT_PLAYER seat on PLAYERTEMPLATE_OBSERVER: its own seat kind, never a faction name,
			// because a mod may make a template called FactionObserver playable.
			seats[s].state = (slot->getState() == SLOT_PLAYER && t == PLAYERTEMPLATE_OBSERVER) ? (Int)GX_SEAT_OBSERVER : (Int)slot->getState();
			seats[s].faction = factions[s].str();
			seats[s].team = slot->getTeamNumber();
			seats[s].colour = colours[s].str();
			seats[s].ip = slot->isHuman() ? slot->getIP() : 0;
		}
		char seatSpec[GX_RELAY_SEATS_SPEC_MAX + 1];
		const Int mapMax = (advMapData != nullptr) ? advMapData->m_numPlayers : 0;
		if (gxComposeRelaySeats(seatSpec, sizeof(seatSpec), mapMax, seats, MAX_SLOTS) > 0)
			m_transport->setRelaySlots(seatSpec);
	}

	int player;
	for (player = 0; player<MAX_SLOTS; ++player)
	{
		if (m_currentGame->getIP(player) == m_localIP)
		{
			OnGameOptions(m_localIP, player, AsciiString(msg.GameOptions.options));
			break;
		}
	}

	// We can request game options (side, color, etc) while we don't have a slot yet.  Of course, we don't need to
	// call OnGameOptions for those, so it's okay to silently fail.
	//DEBUG_ASSERTCRASH(player != MAX_SLOTS, ("Requested game options, but we're not in slot list!");
}

void LANAPI::RequestGameCreate( UnicodeString gameName, Bool isDirectConnect )
{
	// No games of the same name should exist...  Ignore that for now.
	/// @todo: make sure LAN games with identical names don't crash things like in RA2.

	// GeneralsX: Same pre-flight as RequestGameJoin - a
	// half-initialised engine must not be able to host either, and the verdict must be set
	// before the callback. Without this, a build whose identity never initialised can still
	// host: it writes tag=0, every joiner sees SIMID_PEER_SILENT and is told the HOST is too
	// old, which is the wrong diagnosis.
	if (!SimulationId::get().valid)
	{
		m_remoteSimValid = FALSE;
		memset(&m_remoteSim, 0, sizeof(m_remoteSim));
		m_remoteSimVerdict = SIMID_LOCAL_INVALID;
		OnGameCreate(LANAPIInterface::RET_CRC_MISMATCH);
		return;
	}
	// GeneralsX: M1 Determinism-1: a build without git provenance
	// (sourceID 0) is refused at every join now (SimIdCompare, SIMID_SOURCE_UNKNOWN) - even by an identical build - so a
	// room it hosted could never be joined. Refuse the room instead of advertising one. Not for a headless host
	// its solo -lanhost AI match admits no peer and must still run; a scripted host that does
	// wait for peers gets them refused at the join, which its lobby timeout reports.
	if (SimulationId::get().sourceID == 0u && !TheGlobalData->m_headless)
	{
		m_remoteSimValid = FALSE;
		memset(&m_remoteSim, 0, sizeof(m_remoteSim));
		m_remoteSimVerdict = SIMID_SOURCE_UNKNOWN;
		fprintf(stderr, "[SIMID] cannot host: this build has no git provenance (sourceID 0), so no peer could join it\n");
		fflush(stderr);
		OnGameCreate(LANAPIInterface::RET_CRC_MISMATCH);
		return;
	}

	if ((!m_inLobby || m_currentGame) && !isDirectConnect)
	{
		DEBUG_ASSERTCRASH(m_inLobby && m_currentGame, ("Can't create a game while in one!"));
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	if (m_pendingAction != ACT_NONE)
	{
		OnGameCreate(LANAPIInterface::RET_BUSY);
		return;
	}

	// Create the local game object
	m_inLobby = false;
	LANGameInfo *myGame = NEW LANGameInfo;

	myGame->setSeed(GetTickCount());

//	myGame->setInProgress(false);
	myGame->enterGame();
	UnicodeString s;
	s.format(L"%8.8X%8.8X", m_localIP, myGame->getSeed());
	if (gameName.isEmpty())
		s.concat(m_name);
	else
		s.concat(gameName);

	s.truncateTo(g_lanGameNameLength);

	DEBUG_LOG(("Setting local game name to '%ls'", s.str()));

	myGame->setName(s);

	LANGameSlot newSlot;
	newSlot.setState(SLOT_PLAYER, m_name);
	newSlot.setIP(m_localIP);
	newSlot.setPort(NETWORK_BASE_PORT_NUMBER); // LAN game, everyone has a unique IP, so it's ok to use the same port.
	newSlot.setLastHeard(0);
	newSlot.setLogin(m_userName);
	newSlot.setHost(m_hostName);

	myGame->setSlot(0,newSlot);
	myGame->setNext(nullptr);
	LANPreferences pref;

	AsciiString mapName = pref.getPreferredMap();

	myGame->setMap(mapName);
	myGame->setIsDirectConnect(isDirectConnect);

	myGame->setLastHeard(timeGetTime());
	m_currentGame = myGame;

/// @todo: Need to initialize the players elsewere.
/*	for (int player = 1; player < MAX_SLOTS; ++player)
	{
		myGame->setPlayerName(player, L"");
		myGame->setIP(player, 0);
		myGame->setAccepted(player, false);
	}*/

	// Add the game to the local game list
	addGame(myGame);

	// Send an announcement
	//RequestSlotList();
/*
	LANMessage msg;
	wcslcpy(msg.name, m_name.str(), ARRAY_SIZE(msg.name));
	wcscpy(msg.GameInfo.gameName, myGame->getName().str());
	for (player=0; player<MAX_SLOTS; ++player)
	{
		wcscpy(msg.GameInfo.name[player], myGame->getPlayerName(player).str());
		msg.GameInfo.ip[player] = myGame->getIP(player);
		msg.GameInfo.playerAccepted[player] = myGame->getAccepted(player);
	}
	msg.messageType = LANMessage::MSG_GAME_ANNOUNCE;
*/

	// GeneralsX: Report the lobby's password/private state to its own
	// host once the game options screen exists. Set here rather than inside OnGameCreate because
	// that callback lives in LANAPICallbacks.cpp and immediately pushes a shell screen - this is
	// the last point at which we are still plainly "the code that created a game".
	m_pendingLobbyNotice = (m_hostPwRequired || m_hostPrivate);

	OnGameCreate(LANAPIInterface::RET_OK);
}


/*static const char slotListID		= 'S';
static const char gameOptionsID	= 'G';
static const char acceptID			= 'A';
static const char wannaStartID	= 'W';

AsciiString LANAPI::createSlotString()
{
	AsciiString slotList;
	slotList.concat(slotListID);
	for (int i=0; i<MAX_SLOTS; ++i)
	{
		LANGameSlot *slot = GetMyGame()->getLANSlot(i);
		AsciiString str;
		if (slot->isHuman())
		{
			str = "H";
			LANPlayer *user = slot->getUser();
			DEBUG_ASSERTCRASH(user, ("Human player has no User*!"));
			AsciiString name;
			name.translate(user->getName());
			str.concat(name);
			str.concat(',');
		}
		else if (slot->isAI())
		{
			if (slot->getState() == SLOT_EASY_AI)
				str = "CE,";
			if (slot->getState() == SLOT_MED_AI)
				str = "CM,";
			else
				str = "CB,";
		}
		else if (slot->getState() == SLOT_OPEN)
		{
			str = "O,";
		}
		else if (slot->getState() == SLOT_CLOSED)
		{
			str = "X,";
		}
		else
		{
			DEBUG_CRASH(("Bad slot type"));
			str = "X,";
		}

		slotList.concat(str);
	}
	return slotList;
}
*/
/*
void LANAPI::RequestSlotList()
{

	LANMessage reply;
	reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
	wcslcpy(reply.name, m_name.str(), ARRAY_SIZE(reply.name));
	int player;
	for (player = 0; player < MAX_SLOTS; ++player)
	{
		wcslcpy(reply.GameInfo.name[player], m_currentGame->getPlayerName(player).str(), ARRAY_SIZE(reply.GameInfo.name[player]));
		reply.GameInfo.ip[player] = m_currentGame->getIP(player);
		reply.GameInfo.playerAccepted[player] = m_currentGame->getSlot(player)->isAccepted();
	}
	wcslcpy(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName));
	reply.GameInfo.inProgress = m_currentGame->isGameInProgress();

	sendMessage(&reply);

	OnSlotList(LANAPIInterface::RET_OK, m_currentGame);
}
*/
void LANAPI::RequestSetName( UnicodeString newName )
{
	newName.trim();
	if (m_pendingAction != ACT_NONE)
	{
		// Can't change name while joining games
		OnNameChange(m_localIP, newName);
		return;
	}

	// Set up timer
	m_lastResendTime = timeGetTime();

	// GeneralsX: Set the name even when we are not in the LOBBY.
	//
	// m_name was assigned only inside the `m_inLobby` branch below, so calling this from a game -
	// which is what /nickname on the GAME OPTIONS screen does - silently did nothing: the
	// confirmation said "You are now known as yessirr" while the slot list kept "r0se-winx64".
	// The same gap can leave a slot showing no name at all when the name is set across the join.
	//
	// m_name is also what fillInLANMessage() stamps on every outgoing LAN message, so assigning it
	// here is what makes the new name propagate at all - the host picks it up from the next
	// message we send rather than needing a dedicated rename packet.
	m_name = newName;

	// A joined game keeps its own copy in the slot list; update ours so the local screen is right
	// immediately, instead of waiting for the host to echo the change back.
	if (m_currentGame != NULL)
	{
		const Int mySlot = m_currentGame->getLocalSlotNum();
		if (mySlot >= 0)
		{
			LANGameSlot *slot = m_currentGame->getLANSlot(mySlot);
			if (slot != NULL)
				slot->setName(newName);
		}
	}

	if (m_inLobby && m_pendingAction == ACT_NONE)
	{
		LANMessage msg;
		fillInLANMessage( &msg );
		msg.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;
		sendMessage(&msg);

		// Update the interface
		LANPlayer *player = LookupPlayer(m_localIP);
		if (!player)
		{
			player = NEW LANPlayer;
			player->setIP(m_localIP);
		}
		else
		{
			removePlayer(player);
		}
		player->setName(m_name);
		player->setHost(m_hostName);
		player->setLogin(m_userName);
		player->setLastHeard(timeGetTime());

		addPlayer(player);

		OnNameChange(player->getIP(), player->getName());
	}
}

void LANAPI::fillInLANMessage( LANMessage *msg )
{
	if (!msg)
		return;

	// GeneralsX: Use CopyWcharToWindowsWideChar (fighter19 pattern)
	CopyWcharToWindowsWideChar(msg->name, m_name.str(), ARRAY_SIZE(msg->name) - 1);
	strlcpy(msg->userName, m_userName.str(), ARRAY_SIZE(msg->userName));
	strlcpy(msg->hostName, m_hostName.str(), ARRAY_SIZE(msg->hostName));
}

void LANAPI::RequestLobbyLeave( Bool forced )
{
	LANMessage msg;
	msg.messageType = LANMessage::MSG_REQUEST_LOBBY_LEAVE;
	fillInLANMessage( &msg );
	sendMessage(&msg);

	if (forced)
		m_transport->update();
}

// Misc utility functions
LANGameInfo * LANAPI::LookupGame( UnicodeString gameName )
{
	LANGameInfo *theGame = m_games;

	while (theGame && theGame->getName() != gameName)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo * LANAPI::LookupGameByListOffset( Int offset )
{
	LANGameInfo *theGame = m_games;

	if (offset < 0)
		return nullptr;

	while (offset-- && theGame)
	{
		theGame = theGame->getNext();
	}

	return theGame; // null means we didn't find anything.
}

LANGameInfo* LANAPI::LookupGameByHost(UnsignedInt hostIP)
{
	LANGameInfo* lastGame = nullptr;
	UnsignedInt lastHeard = 0;

	for (LANGameInfo* game = m_games; game; game = game->getNext())
	{
		if (game->getHostIP() == hostIP && game->getLastHeard() >= lastHeard)
		{
			lastGame = game;
			lastHeard = game->getLastHeard();
		}
	}

	return lastGame;
}

void LANAPI::removeGame( LANGameInfo *game )
{
	LANGameInfo *g = m_games;
	if (!game)
	{
		return;
	}
	else if (m_games == game)
	{
		m_games = m_games->getNext();
	}
	else
	{
		while (g->getNext() && g->getNext() != game)
		{
			g = g->getNext();
		}
		if (g->getNext() == game)
		{
			g->setNext(game->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANGameInfo wasn't in the list"));
		}
	}
}

LANPlayer * LANAPI::LookupPlayer( UnsignedInt playerIP )
{
	LANPlayer *thePlayer = m_lobbyPlayers;

	while (thePlayer && thePlayer->getIP() != playerIP)
	{
		thePlayer = thePlayer->getNext();
	}

	return thePlayer; // null means we didn't find anything.
}

void LANAPI::removePlayer( LANPlayer *player )
{
	LANPlayer *p = m_lobbyPlayers;
	if (!player)
	{
		return;
	}
	else if (m_lobbyPlayers == player)
	{
		m_lobbyPlayers = m_lobbyPlayers->getNext();
	}
	else
	{
		while (p->getNext() && p->getNext() != player)
		{
			p = p->getNext();
		}
		if (p->getNext() == player)
		{
			p->setNext(player->getNext());
		}
		else
		{
			// Odd.  We went the whole way without finding it in the list.
			DEBUG_CRASH(("LANPlayer wasn't in the list"));
		}
	}
}

void LANAPI::addGame( LANGameInfo *game )
{
	if (!m_games)
	{
		m_games = game;
		game->setNext(nullptr);
		return;
	}
	else
	{
		if (game->getName().compareNoCase(m_games->getName()) < 0)
		{
			game->setNext(m_games);
			m_games = game;
			return;
		}
		else
		{
			LANGameInfo *g = m_games;
			while (g->getNext() && g->getNext()->getName().compareNoCase(game->getName()) > 0)
			{
				g = g->getNext();
			}
			game->setNext(g->getNext());
			g->setNext(game);
			return;
		}
	}
}

void LANAPI::addPlayer( LANPlayer *player )
{
	if (!m_lobbyPlayers)
	{
		m_lobbyPlayers = player;
		player->setNext(nullptr);
		return;
	}
	else
	{
		if (player->getName().compareNoCase(m_lobbyPlayers->getName()) < 0)
		{
			player->setNext(m_lobbyPlayers);
			m_lobbyPlayers = player;
			return;
		}
		else
		{
			LANPlayer *p = m_lobbyPlayers;
			while (p->getNext() && p->getNext()->getName().compareNoCase(player->getName()) > 0)
			{
				p = p->getNext();
			}
			player->setNext(p->getNext());
			p->setNext(player);
			return;
		}
	}
}

Bool LANAPI::SetLocalIP( UnsignedInt localIP )
{
	Bool retval = TRUE;
	m_localIP = localIP;
	m_broadcastAddr = findBroadcastAddrForIP(m_localIP);

	DEBUG_LOG(("LANAPI::SetLocalIP - local IP is %d.%d.%d.%d, broadcasting to %d.%d.%d.%d",
		PRINTF_IP_AS_4_INTS(m_localIP), PRINTF_IP_AS_4_INTS(m_broadcastAddr)));

	m_transport->reset();
	retval = m_transport->init(m_localIP, lobbyPort);
	m_transport->allowBroadcasts(true);

	// GeneralsX: Release-visible. This is the single point where the
	// announced identity and the discovery destination are decided, and both were invisible in a
	// shipping build - a wrong interface, a bad broadcast address and a failed bind all presented
	// identically as an empty lobby that times out after 5 s.
	fprintf(stderr, "[LAN] localIP=%d.%d.%d.%d broadcast=%d.%d.%d.%d port=%u transportInit=%s\n",
		PRINTF_IP_AS_4_INTS(m_localIP), PRINTF_IP_AS_4_INTS(m_broadcastAddr),
		(unsigned)lobbyPort, retval ? "ok" : "FAILED");
	fflush(stderr);

	return retval;
}

void LANAPI::SetLocalIP( AsciiString localIP )
{
	UnsignedInt resolvedIP = ResolveIP(localIP);
	SetLocalIP(resolvedIP);
}

Bool LANAPI::AmIHost()
{
	return m_currentGame && m_currentGame->getIP(0) == m_localIP;
}

void LANAPI::setIsActive(Bool isActive) {
	DEBUG_LOG(("LANAPI::setIsActive - entering"));
	if (isActive != m_isActive) {
		DEBUG_LOG(("LANAPI::setIsActive - m_isActive changed to %s", isActive ? "TRUE" : "FALSE"));
		if (isActive == FALSE) {
			if ((m_inLobby == FALSE) && (m_currentGame != nullptr)) {
				LANMessage msg;
				fillInLANMessage( &msg );
				msg.messageType = LANMessage::MSG_INACTIVE;
				sendMessage(&msg);
				DEBUG_LOG(("LANAPI::setIsActive - sent an IsActive message"));
			}
		}
	}
	m_isActive = isActive;
}


// GeneralsX: Relay lobby chat. Thin passes to the transport - LANAPI owns
// the socket the lobby talks on, so it is the natural place for the menu to reach it, and keeping
// the protocol itself in Transport means the relay plaintext lives in exactly one file.
Bool LANAPI::sendLobbyChat( const char *text )
{
	if (m_transport == NULL)
		return FALSE;
	return m_transport->sendRelayChat(text);
}

Bool LANAPI::takeLobbyChat( char *outText, Int outSize, UnsignedInt *outSenderIp )
{
	if (m_transport == NULL)
		return FALSE;
	return m_transport->takeRelayChat(outText, outSize, outSenderIp);
}

Bool LANAPI::requestLobbyRoster()
{
	if (m_transport == NULL)
		return FALSE;
	return m_transport->requestRelayRoster();
}

Int LANAPI::getLobbyRoster( UnsignedInt *outIps, Int maxIps )
{
	if (m_transport == NULL)
		return 0;
	return m_transport->getRelayRoster(outIps, maxIps);
}

Bool LANAPI::setLobbyNickname( const char *name )
{
	if (m_transport == NULL)
		return FALSE;
	return m_transport->sendRelayNickname(name);
}

const char *LANAPI::getLobbyRosterNick( Int i )
{
	if (m_transport == NULL)
		return "";
	return m_transport->getRosterNick(i);
}

const char *LANAPI::getLobbyRosterOS( Int i )
{
	if (m_transport == NULL)
		return "";
	return m_transport->getRosterOS(i);
}

// GeneralsX: The signed identity, and the role the relay read out of it.
// See Transport::sendRelayIdentity for why the client only ever forwards this and never trusts it.
Bool LANAPI::setLobbyIdentity( const char *assertion )
{
	if (m_transport == NULL)
		return FALSE;
	return m_transport->sendRelayIdentity(assertion);
}

const char *LANAPI::getLobbyRosterRole( Int i )
{
	if (m_transport == NULL)
		return "";
	return m_transport->getRosterRole(i);
}

// GeneralsX: See LANAPI.h. Human slots only: an AI or an open slot has no relay
// membership, so a bit there is a claim no relay answer can back and every peer refuses it.
static_assert((Int)GX_ADMIN_WIRE_KEYS == (Int)GX_ADMIN_MAX_PER_MATCH, "the start message carries every admin a match may have");
void LANAPI::gxRelayAdmins( const LANGameInfo *game, RelayMaskType *mask, unsigned char keys[MAX_SLOTS][32] ) const
{
	*mask = 0;
	memset(keys, 0, MAX_SLOTS * 32);
	if (m_transport == NULL || game == NULL || !m_transport->isRelayEnabled())
		return;
	Int n = 0;
	for (Int i = 0; i < MAX_SLOTS && n < GX_ADMIN_WIRE_KEYS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot != NULL && slot->isHuman() && m_transport->getRelayAdminKeyForIP(slot->getIP(), keys[i]))
		{
			*mask |= gxSlotBit(i);
			++n;
		}
	}
}

UnsignedInt LANAPI::getRelayRttMs()
{
	if (m_transport == NULL)
		return 0;
	return m_transport->getRelayRttMs();
}
