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

#include "Lib/BaseType.h"
#include "Common/MessageStream.h"
#include <chrono>

// GeneralsX: Wall-clock stamp for [GXXFER] traces (GX_MAPDBG): Unix-epoch
// milliseconds truncated to 32 bits, the same t= domain those lines had on macOS/Linux/iPadOS before
// timeGetTime() became an uptime clock. The probes exist to sequence one peer's send against another
// peer's receive, which needs one shared time domain; uptime has a per-machine origin. Now also
// comparable on Windows, where timeGetTime() was always uptime. Diagnostics only - never simulation.
inline UnsignedInt GXWallClockMs()
{
	return (UnsignedInt)std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

static const Int WOL_NAME_LEN = 64;

/// Max number of commands per frame
static const Int MAX_COMMANDS = 256;

extern Int MIN_LOGIC_FRAMES;
extern Int MAX_FRAMES_AHEAD;
extern Int MIN_RUNAHEAD;

// FRAME_DATA_LENGTH needs to be MAX_FRAMES_AHEAD+1 because a player on a different
// computer can send commands for a frame that is one beyond twice the max runahead.
extern Int FRAME_DATA_LENGTH;
extern Int FRAMES_TO_KEEP;

// This is the connection numbering: 1-8 are for players
enum ConnectionNumbers CPP_11(: Int)
{
	// 34 lobby slots = 32 players + 2 observers. Observers share the ReplayObserver, so 32 players need 34+ Player
	// objects: MAX_PLAYER_COUNT is 40 and PlayerMaskType 64 bits (GameCommon.h), RelayMaskType 64 bits (below).
	// Changing this breaks the all-players wire, save and replay format.
	MAX_PLAYER = 33,		// The index of the highest possible player number.  This is 0 based, so the most players allowed in a game is MAX_PLAYER+1.
};

#pragma pack(push, 1)
struct TransportMessageHeader
{
	UnsignedInt crc;											///< packet-level CRC (must be first in packet)
	UnsignedShort magic;									///< Magic number identifying Generals packets
	//	Int id;
	//	NetMessageFlags flags;
};
#pragma pack(pop)

static constexpr const Int MAX_SLOTS = MAX_PLAYER+1;

// The per-command relay destination mask: one bit per slot, carried in every game packet (NetPacketRelayField::relay)
// and in NetCommandRef::m_relay. Its width is a wire format: every peer must move together. Reader and writer are
// sizeof-driven off this type, and ALL_SLOTS_MASK stands for every slot.
// Build a slot bit with gxSlotBit(), never `1 << slot`: an int shift is undefined from slot 31.
typedef UnsignedInt64 RelayMaskType;
inline constexpr RelayMaskType gxSlotBit(Int slot) { return (RelayMaskType)1 << slot; }
static constexpr const RelayMaskType ALL_SLOTS_MASK = (RelayMaskType)(((RelayMaskType)1 << MAX_SLOTS) - 1u);
static_assert((Int)(sizeof(RelayMaskType) * 8) >= MAX_SLOTS,
	"the relay destination mask must hold one bit per slot");

// Slots a human may occupy; every slot is wire-addressable. AI players never cross the wire (simulated on every peer).
// Join placement (LANAPIhandlers) and match start (GameLogic) both enforce this bound.
// Two independent constraints, only the first asserted here: the relay destination mask, and the lobby announce budget
// (asserted in LANAPI.h; no circular include). Raising this needs both.
static constexpr const Int MAX_HUMAN_SLOTS = 34;
static_assert(MAX_HUMAN_SLOTS <= (Int)(sizeof(RelayMaskType) * 8),
	"humans must fit the relay destination mask (constraint 1 of 2 - see LANAPI.h for the "
	"announce budget)");
static_assert(MAX_HUMAN_SLOTS <= MAX_SLOTS, "human slots are a subset of all slots");

// TheSuperHackers @info As we are not detecting for network fragmentation and dynamically adjusting payload sizes, we set an 1100 bytes UDP payload as a safe upper limit for various networks
// We chose 1100 bytes as when taking mobile networks into account, maximum transmission unit sizes can vary from 1340 - 1500 bytes
// and when the packet headers for PPPOE, IPV6 and other virtual network encapsulation are considered, we need a lower safe UDP payload to prevent fragmentation.
static constexpr const Int MAX_UDP_PAYLOAD_SIZE = 1100;
// UDP (8 bytes) + IP header (28 bytes) = 36 bytes total.  We want a total packet size of 512, so 512 - 36 = 476
static constexpr const Int RETAIL_GAME_PACKET_SIZE = 476;

// TheSuperHackers @info The legacy lanapi cannot use a larger packet size without breaking the gameinfo command
//
// sizeof(LANMessage) is what a receiver blind-casts, so a short older datagram reads stale buffer bytes. Announces go
// out at their own length (LANMessageWireSize): lobbies fitting the old budget are byte-identical, and only roughly 25+
// humans exceed MAX_UDP_PAYLOAD_SIZE (1100). Game packets are unchanged.
static constexpr const Int MAX_LANAPI_PACKET_SIZE = 1400;
// The largest CONTROL datagram a peer must accept: a lobby message plus the relay header, and the relay's
// 34-member GXMEMB roster (39 + 59 x 34 = 2045 B), GXPKEY (54 + 33 x 39 = 1341 B), GXSTAT (~2377 B) and
// GXSLOT (~2465 B) and GXSEAT (seats alone 2866 B; client GX_SEAT_LINE_MAX 3072, names after seats) lines.
// Game packets never come near it.
static constexpr const Int GX_CONTROL_DATAGRAM_MAX = 3200;
static_assert(MAX_LANAPI_PACKET_SIZE + 12 <= GX_CONTROL_DATAGRAM_MAX,
	"a relayed lobby datagram must fit the raw receive");

// Relay transport. Every relayed datagram carries a fixed header ahead of the game packet (magic, source virtual IP,
// destination virtual IP) so the relay can route without reading the payload. The 12 bytes come out of the payload
// budget on every platform, LAN included: MAX_UDP_PAYLOAD_SIZE is 1100 because mobile MTUs run 1340-1500, and a
// fragmented lockstep packet would look like a desync only on cellular.
static constexpr const Int GX_RELAY_HEADER_SIZE = 12;

// TheSuperHackers @bugfix Mauller 08/02/2026 Allow larger ethernet UDP payload to be used for game messages, this fixes connection issues and eliminates disconnection bugs
#if RETAIL_COMPATIBLE_NETWORKING
// Untouched: this branch exists to match the retail wire format exactly, and the relay is not
// part of that format.
static constexpr const Int MAX_PACKET_SIZE = RETAIL_GAME_PACKET_SIZE;
// GeneralsX: 1024 -> GX_CONTROL_DATAGRAM_MAX here too: it is a receive size, not the
// retail wire, and a 34-slot LANMessage (1395 B) and the 34-member relay control lines must fit a receive slot in both branches.
static constexpr const Int MAX_NETWORK_MESSAGE_LEN = GX_CONTROL_DATAGRAM_MAX;
#else
// GeneralsX: The auth trailer comes out of the budget too. A DIRECT datagram carries
// Transport's keyId/seq/tag trailer after the game packet (Transport.h, GX_AUTH_TRAILER_SIZE, asserted equal to
// this there), so without it a full packet went out as 12 + 6 + 1082 + 13 = 1113 bytes: over the 1100 the raw
// receive reads, so the receiver refused exactly the fullest packets - the ones carrying the most commands.
static constexpr const Int GX_AUTH_TRAILER_RESERVE = 13;
static constexpr const Int MAX_PACKET_SIZE = MAX_UDP_PAYLOAD_SIZE - sizeof(TransportMessageHeader) - GX_RELAY_HEADER_SIZE - GX_AUTH_TRAILER_RESERVE;
// The RAW receive size still has to be the full payload: what arrives is the relay header plus a
// game packet that was already capped above plus the trailer, so the total is exactly MAX_UDP_PAYLOAD_SIZE.
// GeneralsX: ... and with 34 slots the raw receive is the control-datagram
// ceiling (lobby and relay control lines past 1100 B at 25+ humans); game packets stay capped above.
static constexpr const Int MAX_NETWORK_MESSAGE_LEN = GX_CONTROL_DATAGRAM_MAX;
#endif

// TheSuperHackers @bugfix Mauller 08/02/2026 Double send and receive buffer sizes to alleviate the occurance of disconnection issues in retail and non retail code.
static constexpr const Int MAX_MESSAGES = 256;

/**
 * Command packet - contains frame #, total # of commands, and each command.  This is what gets sent
 * to each player every frame
 */
static const Int numCommandsPerCommandPacket = (MAX_NETWORK_MESSAGE_LEN - sizeof(UnsignedInt) - sizeof(UnsignedShort))/sizeof(GameMessage);
#pragma pack(push, 1)
struct CommandPacket
{
	UnsignedInt m_frame;
	UnsignedShort m_numCommands;
	unsigned char m_commands[numCommandsPerCommandPacket * sizeof(GameMessage)];
};
#pragma pack(pop)

#define MAX_TRANSPORT_STATISTICS_SECONDS 30

/**
 * Transport message - encapsulating info kept by the transport layer about each
 * packet.  These structs make up the in/out buffers at the transport layer.
 */
#pragma pack(push, 1)
struct TransportMessage
{
	TransportMessageHeader header;
#if RETAIL_COMPATIBLE_NETWORKING
	// TheSuperHackers @info This value is not the correct one that should be used here, it should have been max packet size
	// The non retail max network message len takes the extra bytes of the network message header into account when handling UDP payload data
	// In retail this only works since no data larger than the retail game packet size is put into a network message
	UnsignedByte data[MAX_NETWORK_MESSAGE_LEN];
#else
	// GeneralsX: a slot also carries LAN lobby messages (LANAPI casts it to a LANMessage), and those
	// grew to MAX_LANAPI_PACKET_SIZE (1400) for 34 slots, past MAX_PACKET_SIZE (1069). Game packets are still bounded by
	// MAX_PACKET_SIZE where they are built (NetPacket); only the slot grew.
	UnsignedByte data[MAX_PACKET_SIZE > MAX_LANAPI_PACKET_SIZE ? MAX_PACKET_SIZE : MAX_LANAPI_PACKET_SIZE];
#endif
	Int length;
	UnsignedInt addr;
	UnsignedShort port;
};
#pragma pack(pop)

#if defined(RTS_DEBUG)
#pragma pack(push, 1)
struct DelayedTransportMessage
{
	UnsignedInt deliveryTime;
	TransportMessage message;
};
#pragma pack(pop)
#endif

/**
 * Message types
 */
enum NetMessageFlag CPP_11(: Int) {
	MSG_ACK = 1,
	MSG_NEEDACK = 2,
	MSG_SEQUENCED = 4,
	MSG_SUPERCEDING = 8
};
typedef UnsignedByte NetMessageFlags;

enum NetCommandType CPP_11(: Int) {
	NETCOMMANDTYPE_UNKNOWN = -1,
	NETCOMMANDTYPE_ACKBOTH = 0,
	NETCOMMANDTYPE_ACKSTAGE1,
	NETCOMMANDTYPE_ACKSTAGE2,
	NETCOMMANDTYPE_FRAMEINFO,
	NETCOMMANDTYPE_GAMECOMMAND,
	NETCOMMANDTYPE_PLAYERLEAVE,
	NETCOMMANDTYPE_RUNAHEADMETRICS,
	NETCOMMANDTYPE_RUNAHEAD,
	NETCOMMANDTYPE_DESTROYPLAYER,
	NETCOMMANDTYPE_KEEPALIVE,
	NETCOMMANDTYPE_DISCONNECTCHAT,
	NETCOMMANDTYPE_CHAT,
	NETCOMMANDTYPE_MANGLERQUERY,
	NETCOMMANDTYPE_MANGLERRESPONSE,
	NETCOMMANDTYPE_PROGRESS,
	NETCOMMANDTYPE_LOADCOMPLETE,
	NETCOMMANDTYPE_TIMEOUTSTART,
	NETCOMMANDTYPE_WRAPPER,							// A wrapper command that holds a command that's too big to fit in a single packet.
	NETCOMMANDTYPE_FILE,
	NETCOMMANDTYPE_FILEANNOUNCE,
	NETCOMMANDTYPE_FILEPROGRESS,
	NETCOMMANDTYPE_FRAMERESENDREQUEST,

	// Disconnect menu command section.
	NETCOMMANDTYPE_DISCONNECTSTART,
	NETCOMMANDTYPE_DISCONNECTKEEPALIVE,
	NETCOMMANDTYPE_DISCONNECTPLAYER,
	NETCOMMANDTYPE_PACKETROUTERQUERY,
	NETCOMMANDTYPE_PACKETROUTERACK,
	NETCOMMANDTYPE_DISCONNECTVOTE,
	NETCOMMANDTYPE_DISCONNECTFRAME,
	NETCOMMANDTYPE_DISCONNECTSCREENOFF,
	NETCOMMANDTYPE_DISCONNECTEND,

	// The packet router's latency row, published so every peer can show a ping per player.
	// Appended after DISCONNECTEND on purpose: DISCONNECTSTART and DISCONNECTEND are sentinels (ConnectionManager tests
	// cmdType between them to route to the disconnect handler), so landing inside would hand every report to it.
	// Appending also leaves every existing type value unchanged, so the wire change is additive.
	NETCOMMANDTYPE_LATENCYREPORT,

	// GeneralsX: The ack of ONE wrapper chunk, naming (original player, wrapSeq, chunk number)
	// rather than the 16-bit ID that wraps within seconds during a transfer (v16 section 2).
	// Appended after LATENCYREPORT for the same two reasons: outside the DISCONNECTSTART..DISCONNECTEND sentinels, and no
	// existing type renumbers. Connection-scoped: never relayed, applied only when it arrived direct from the acker.
	NETCOMMANDTYPE_WRAPPERACK,
};

// GeneralsX: The wrapper send window (v16 section 3). Per connection, a
// sender never has more than GX_WRAP_WINDOW serializations from its lowWater to its next wrapSeq, nor more than GX_WRAP_BYTES
// claimed bytes in that interval; the receiver refuses (unacked) anything outside it, so it never holds more per source.
// GX_RELAY_SOFTCAP_* bound what a router queues for one relayed originator (distinct commands, and their body bytes).
static constexpr const UnsignedInt GX_WRAP_WINDOW = 64;
static constexpr const UnsignedInt GX_WRAP_BYTES = 16 * 1024 * 1024;
static constexpr const UnsignedInt GX_RELAY_SOFTCAP_COMMANDS = 1024;
static constexpr const UnsignedInt GX_RELAY_SOFTCAP_BYTES = 16 * 1024 * 1024;
/// What a receiver reserves for one serialization (NetCommandWrapperListNode: the buffer plus one Bool per chunk). The sender
/// charges its window the same, so it never sends what the receiver would refuse.
inline UnsignedInt GXWrapReserve(UnsignedInt totalDataLength, UnsignedInt numChunks) { return totalDataLength + numChunks * (UnsignedInt)sizeof(Bool); }

enum NetLocalStatus CPP_11(: Int) {
	NETLOCALSTATUS_PREGAME = 0,
	NETLOCALSTATUS_INGAME,
	NETLOCALSTATUS_LEAVING,
	NETLOCALSTATUS_LEFT,
	NETLOCALSTATUS_POSTGAME
};

enum PlayerLeaveCode CPP_11(: Int) {
	PLAYERLEAVECODE_CLIENT = 0,
	PLAYERLEAVECODE_LOCAL,
	PLAYERLEAVECODE_PACKETROUTER,
	PLAYERLEAVECODE_UNKNOWN
};

// Magic number for identifying a Generals packet.
static const UnsignedShort GENERALS_MAGIC_NUMBER = 0xF00D;

// The number of fps history entries.
//static const Int NETWORK_FPS_HISTORY_LENGTH = 30;

// The number of ping history entries.
//static const Int NETWORK_LATENCY_HISTORY_LENGTH = 200;

// The number of miliseconds between run ahead metrics things
//static const Int NETWORK_RUN_AHEAD_METRICS_TIME = 5000;

// The number of cushion values to keep.
//static const Int NETWORK_CUSHION_HISTORY_LENGTH = 10;

// The amount of slack in the run ahead value.  This is the percentage of the calculated run ahead that is added.
//static const Int NETWORK_RUN_AHEAD_SLACK = 20;

// The number of seconds between when the connections to each player send a keep-alive packet.
// This should be less than 30 just to keep firewall ports open.
//static const Int NETWORK_KEEPALIVE_DELAY = 20;

// The number of milliseconds between when the game gets stuck on a frame for a network stall and
// and when the disconnect dialog comes up.
//static const Int NETWORK_DISCONNECT_TIME = 5000;

// The number of miliseconds between when a player's last disconnect keep alive command
// was received and when they are considered disconnected from the game.
//static const Int NETWORK_PLAYER_TIMEOUT_TIME = 60000;

// The base port number used for the transport socket.  A players slot number is added to this
// value to get their actual port number.
static const Int NETWORK_BASE_PORT_NUMBER = 8088;

// the singleton
class NetworkInterface;
extern NetworkInterface *TheNetwork;

#define PRINTF_IP_AS_4_INTS(ip) ((ip) >> 24) & 0xff, ((ip) >> 16) & 0xff, ((ip) >> 8 ) & 0xff, (ip) & 0xff
