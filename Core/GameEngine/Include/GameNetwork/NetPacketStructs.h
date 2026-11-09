/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#pragma once

#include "GameNetwork/GXWireChar.h"
#include "GameNetwork/NetworkDefs.h"
#include "WWLib/stringex.h"

class AsciiString;
class UnicodeString;
class GameMessage;

class NetCommandRef;
class NetCommandMsg;
class NetGameCommandMsg;
class NetAckCommandMsg;
class NetAckBothCommandMsg;
class NetAckStage1CommandMsg;
class NetAckStage2CommandMsg;
class NetFrameCommandMsg;
class NetPlayerLeaveCommandMsg;
class NetRunAheadMetricsCommandMsg;
class NetRunAheadCommandMsg;
class NetLatencyReportCommandMsg;
class NetDestroyPlayerCommandMsg;
class NetKeepAliveCommandMsg;
class NetDisconnectKeepAliveCommandMsg;
class NetDisconnectPlayerCommandMsg;
class NetPacketRouterQueryCommandMsg;
class NetPacketRouterAckCommandMsg;
class NetDisconnectChatCommandMsg;
class NetChatCommandMsg;
class NetDisconnectVoteCommandMsg;
class NetProgressCommandMsg;
class NetWrapperCommandMsg;
class NetWrapperAckCommandMsg;
class NetFileCommandMsg;
class NetFileAnnounceCommandMsg;
class NetFileProgressCommandMsg;
class NetDisconnectFrameCommandMsg;
class NetDisconnectScreenOffCommandMsg;
class NetFrameResendRequestCommandMsg;
class NetLoadCompleteCommandMsg;
class NetTimeOutGameStartCommandMsg;

////////////////////////////////////////////////////////////////////////////////
// Helper functions for raw byte data handling
////////////////////////////////////////////////////////////////////////////////

namespace network
{

template<typename T>
size_t writePrimitive(UnsignedByte *dest, T value)
{
	memcpy(dest, &value, sizeof(value));
	return sizeof(value);
}

template<typename T>
size_t writeObject(UnsignedByte *dest, const T& value)
{
	memcpy(dest, &value, sizeof(value));
	return sizeof(value);
}

inline size_t writeBytes(UnsignedByte *dest, const UnsignedByte* src, size_t len)
{
	memcpy(dest, src, len);
	return len;
}

// GeneralsX @bugfix Claude 17/08/2026 THE IN-GAME CHAT WIRE CHARACTER IS 16 BITS ON EVERY PLATFORM.
//
// WideChar is `typedef wchar_t` (BaseType.h:36) with no platform guard, and wchar_t is 2 bytes on
// MSVC and 4 bytes on clang/gcc. Every chat payload in this file used to be sized and copied as
// `n * sizeof(WideChar)`, so a macOS/Linux/iPadOS peer emitted 4 bytes per character where a
// Windows peer emitted 2, and NetPacket::readChatMessage consumed whichever its own compiler
// believed. That is not merely mojibake:
//
//   NetPacket::getCommandList walks one shared cursor `i` through m_packet and every reader
//   advances it (NetPacket.cpp:566-737). A chat command whose payload is 2n bytes on the wire but
//   consumed as 4n leaves the cursor 2n bytes past the next command's type byte, so EVERY command
//   packed behind the chat line in that datagram is decoded from the wrong offset - including
//   NETCOMMANDTYPE_GAMECOMMAND, which is the lockstep-critical one. Chat is not a synchronised
//   command (NetworkUtil.cpp IsCommandSynchronized) and cannot desync the simulation by itself,
//   but corrupting the game commands sitting behind it in the same packet certainly can.
//
// The LAN lobby already hit this and fixed it the same way, with `typedef uint16_t
// WideCharWindows` (LANAPI.h:231-234). The game packet path was simply never given the same
// treatment. Sixteen bits is also the retail-compatible choice: it is what the Windows build has
// always put on the wire, so this makes the three SDL3 platforms conform to Windows rather than
// inventing a third format, and it halves the bytes a chat line costs inside a 476-byte packet.
// GeneralsX @bugfix Claude 22/09/2026 One definition, not two. This was its own `typedef uint16_t`
// until the identical split was found on the GAME COMMAND path (NetPacketGameCommandData, which
// carries MSG_SET_BEACON_TEXT's per-character WIDECHAR arguments) and in the .rep body. Aliasing
// GXWireChar rather than restating uint16_t means there is a single answer to "how wide is a wide
// character on the wire", which is the property that was missing when this file was fixed alone.
typedef GXWireChar NetWireChar;

/// Write `maxLen` code units of `value` as 16-bit units. Anything outside the BMP becomes '?' so
/// that the count written always equals the count getSize() billed - a writer that could emit fewer
/// units than its estimator promised is the exact shape of the boundary overrun fixed in 3c340511f.
inline size_t writeStringAsWireChars(UnsignedByte *dest, const UnicodeString& value, size_t maxLen)
{
	const size_t copyLen = std::min<size_t>(value.getLength(), maxLen);
	const WideChar *src = value.str();
	for (size_t n = 0; n < copyLen; ++n)
	{
		const UnsignedInt cp = (UnsignedInt)(unsigned long)src[n];
		const NetWireChar unit = (cp > 0xFFFFU) ? (NetWireChar)'?' : (NetWireChar)cp;
		memcpy(dest + (n * sizeof(NetWireChar)), &unit, sizeof(unit));
	}
	return copyLen * sizeof(NetWireChar);
}

// The `n * sizeof(WideChar)` writer that used to live here has been deleted rather than left
// unused: its two callers were the two chat payloads above, and an idle helper with exactly the
// wrong sizing is an invitation to reintroduce the split. Wide text on the wire goes through
// writeStringAsWireChars.

inline size_t writeStringWithNull(UnsignedByte *dest, const AsciiString& value)
{
	memcpy(dest, value.str(), value.getByteCount() + 1);
	return static_cast<size_t>(value.getByteCount() + 1);
}

} // namespace network

// Ensure structs are packed to 1-byte alignment for network protocol compatibility
#pragma pack(push, 1)

////////////////////////////////////////////////////////////////////////////////
// Network packet field type definitions
////////////////////////////////////////////////////////////////////////////////

typedef UnsignedByte NetPacketFieldType;

namespace NetPacketFieldTypes
{
	constexpr const NetPacketFieldType CommandType = 'T';
	constexpr const NetPacketFieldType Relay = 'R';
	constexpr const NetPacketFieldType Frame = 'F';
	constexpr const NetPacketFieldType PlayerId = 'P';
	constexpr const NetPacketFieldType CommandId = 'C';
	constexpr const NetPacketFieldType Data = 'D';
	constexpr const NetPacketFieldType Repeat = 'Z';
}

////////////////////////////////////////////////////////////////////////////////
// Common packet field structures
////////////////////////////////////////////////////////////////////////////////

struct NetPacketCommandTypeField
{
	NetPacketCommandTypeField() : fieldType(NetPacketFieldTypes::CommandType) {}
	char fieldType;
	UnsignedByte commandType;
};

struct NetPacketRelayField
{
	NetPacketRelayField() : fieldType(NetPacketFieldTypes::Relay) {}
	char fieldType;
	// GeneralsX @feature Claude 12/08/2026 Widened with RelayMaskType (NetworkDefs.h) for 12
	// human machines - grows the 'R' field by one byte in every game packet. Reader and writer
	// are sizeof-driven off this member; do not size anything against a literal here.
	RelayMaskType relay;
};

struct NetPacketFrameField
{
	NetPacketFrameField() : fieldType(NetPacketFieldTypes::Frame) {}
	char fieldType;
	UnsignedInt frame;
};

struct NetPacketPlayerIdField
{
	NetPacketPlayerIdField() : fieldType(NetPacketFieldTypes::PlayerId) {}
	char fieldType;
	UnsignedByte playerId;
};

struct NetPacketCommandIdField
{
	NetPacketCommandIdField() : fieldType(NetPacketFieldTypes::CommandId) {}
	char fieldType;
	UnsignedShort commandId;
};

struct NetPacketDataField
{
	NetPacketDataField() : fieldType(NetPacketFieldTypes::Data) {}
	char fieldType;
};

struct NetPacketRepeatField
{
	NetPacketRepeatField() : fieldType(NetPacketFieldTypes::Repeat) {}
	char fieldType;
};

////////////////////////////////////////////////////////////////////////////////
// Packed Network structures
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// NetPacketRepeatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRepeatCommand
{
	struct CommandBase
	{
		NetPacketRepeatField repeat;
	};

	static size_t getSize()
	{
		return sizeof(CommandBase);
	}
	static size_t copyBytes(UnsignedByte *buffer)
	{
		CommandBase base;
		return network::writeObject(buffer, base);
	}
};

////////////////////////////////////////////////////////////////////////////////
// SmallNetPacketCommandBase
////////////////////////////////////////////////////////////////////////////////

struct SmallNetPacketCommandBaseSelect
{
	UnsignedByte useCommandType : 1;
	UnsignedByte useRelay : 1;
	UnsignedByte useFrame : 1;
	UnsignedByte usePlayerId : 1;
	UnsignedByte useCommandId : 1;
};

struct SmallNetPacketCommandBase
{
	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize(const SmallNetPacketCommandBaseSelect *select = nullptr);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref, const SmallNetPacketCommandBaseSelect *select = nullptr);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketCommandTemplate
////////////////////////////////////////////////////////////////////////////////

template<typename Base, typename Data>
struct NetPacketCommandTemplate
{
	typedef Base CommandBase;
	typedef Data CommandData;

	static size_t getSize(const NetCommandMsg &msg)
	{
		size_t size = 0;
		size += CommandBase::getSize();
		size += CommandData::getSize(msg);
		return size;
	}
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref)
	{
		size_t size = 0;
		size += CommandBase::copyBytes(buffer + size, ref);
		size += CommandData::copyBytes(buffer + size, ref);
		return size;
	}
};

////////////////////////////////////////////////////////////////////////////////
// SmallNetPacketCommandTemplate
////////////////////////////////////////////////////////////////////////////////

template<typename Base, typename Data>
struct SmallNetPacketCommandTemplate
{
	typedef Base CommandBase;
	typedef Data CommandData;

	static size_t getSize(const NetCommandMsg &msg, const SmallNetPacketCommandBaseSelect *select = nullptr)
	{
		size_t size = 0;
		size += CommandBase::getSize(select);
		size += CommandData::getSize(msg);
		return size;
	}
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref, const SmallNetPacketCommandBaseSelect *select = nullptr)
	{
		size_t size = 0;
		size += CommandBase::copyBytes(buffer + size, ref, select);
		size += CommandData::copyBytes(buffer + size, ref);
		return size;
	}
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketNoData
////////////////////////////////////////////////////////////////////////////////

struct NetPacketNoData
{
	typedef void CommandMsg;

	static size_t getSize(const NetCommandMsg &) { return 0; }
	static size_t copyBytes(UnsignedByte *, const NetCommandRef &) { return 0; }
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketAckCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketAckCommandData
{
	typedef NetAckCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort commandId; // Command ID being acknowledged
		UnsignedByte originalPlayerId; // Original player who sent the command
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketAckCommandBase
{
	typedef NetAckCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		//NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFrameCommandData
{
	typedef NetFrameCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort commandCount;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketFrameCommandBase
{
	typedef NetFrameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketPlayerLeaveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketPlayerLeaveCommandData
{
	typedef NetPlayerLeaveCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte leavingPlayerId;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketPlayerLeaveCommandBase
{
	typedef NetPlayerLeaveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadMetricsCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRunAheadMetricsCommandData
{
	typedef NetRunAheadMetricsCommandMsg CommandMsg;

	struct FixedData
	{
		Real averageLatency;
		UnsignedShort averageFps;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketRunAheadMetricsCommandBase
{
	typedef NetRunAheadMetricsCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRunAheadCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRunAheadCommandData
{
	typedef NetRunAheadCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort runAhead;
		UnsignedByte frameRate;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketRunAheadCommandBase
{
	typedef NetRunAheadCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDestroyPlayerCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDestroyPlayerCommandData
{
	typedef NetDestroyPlayerCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt playerIndex;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDestroyPlayerCommandBase
{
	typedef NetDestroyPlayerCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketLatencyReportCommand
//
// GeneralsX @feature Claude 13/08/2026 The packet router's whole latency row in one fixed-size
// payload: MAX_SLOTS round trips as UnsignedShort MILLISECONDS, indexed by slot.
//
// Milliseconds rather than the Real seconds the engine measures in, for three reasons that all
// point the same way: it halves the payload (2 bytes per slot instead of 4), it is the unit the
// readout actually prints so no peer can disagree about the conversion, and an integer has no
// NaN/Inf to smuggle in from a hostile peer - the whole 16-bit range is a legal, bounded number.
// 65535 ms is ~65 s, far past any link a lockstep match survives, so the range costs nothing.
//
// Slot-indexed and FIXED SIZE rather than a packed list of (slot, ms) pairs: the reader then has
// no count field to trust, which is one fewer thing an attacker controls, and 12 * 2 = 24 bytes
// sits nowhere near MAX_PACKET_SIZE (476, RETAIL_COMPATIBLE_NETWORKING being live).
//
// Modelled on KeepAlive rather than RunAheadMetrics: no frame, no command id, no ack. This is a
// periodic display broadcast and losing one costs a single stale refresh interval, which the
// staleness window already handles. Making it acked would put a purely cosmetic message into the
// retransmit and pending-command machinery for no benefit.
////////////////////////////////////////////////////////////////////////////////

struct NetPacketLatencyReportCommandData
{
	typedef NetLatencyReportCommandMsg CommandMsg;

	// GeneralsX @feature Claude 06/10/2026 ...and each slot's average render fps as the router holds it, one byte per slot:
	// 0 = no figure, n = n - 1 fps (0..254). Every peer can then show every player's fps, which until now only the router
	// knew (clients send theirs to the router alone, NetRunAheadMetricsCommandMsg). MAX_SLOTS more bytes every
	// m_networkRunAheadMetricsTime (5 s); slot-indexed and fixed-size for the reasons above. A byte cannot carry a NaN.
	struct FixedData
	{
		UnsignedShort latencyMs[MAX_SLOTS];
		UnsignedByte fpsPlusOne[MAX_SLOTS];
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketLatencyReportCommandBase
{
	typedef NetLatencyReportCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

// The packet budget, ENFORCED rather than asserted in prose. MAX_PACKET_SIZE is 476 under
// RETAIL_COMPATIBLE_NETWORKING and 1069 without it (GameDefines.h defaults it to 0 since 01/10/2026),
// and NetPacket::addCommand silently REFUSES a command that does not fit - the message would then
// never be sent, in both directions, and the symptom is an unreachable-looking peer rather than a
// size error. At MAX_SLOTS 12 this payload is 24 bytes, so the check has enormous headroom today;
// it exists so that raising MAX_SLOTS fails the build here instead of quietly disabling the
// scoreboard at some future slot count.
static_assert(sizeof(NetPacketLatencyReportCommandData::FixedData)
                  + sizeof(NetPacketLatencyReportCommandBase::CommandBase) <= (size_t)MAX_PACKET_SIZE,
              "latency report must fit one packet; NetPacket::addCommand drops it silently otherwise");

////////////////////////////////////////////////////////////////////////////////
// NetPacketKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketKeepAliveCommandBase
{
	typedef NetKeepAliveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectKeepAliveCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectKeepAliveCommandBase
{
	typedef NetDisconnectKeepAliveCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectPlayerCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectPlayerCommandData
{
	typedef NetDisconnectPlayerCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte disconnectSlot;
		UnsignedInt disconnectFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDisconnectPlayerCommandBase
{
	typedef NetDisconnectPlayerCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterQueryCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRouterQueryCommandBase
{
	typedef NetPacketRouterQueryCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketRouterAckCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketRouterAckCommandBase
{
	typedef NetPacketRouterAckCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectVoteCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectVoteCommandData
{
	typedef NetDisconnectVoteCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte slot;
		UnsignedInt voteFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDisconnectVoteCommandBase
{
	typedef NetDisconnectVoteCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketChatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketChatCommandData
{
	typedef NetChatCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketChatCommandBase
{
	typedef NetChatCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectChatCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectChatCommandData
{
	typedef NetDisconnectChatCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDisconnectChatCommandBase
{
	typedef NetDisconnectChatCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketGameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketGameCommandData
{
	typedef NetGameCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketGameCommandBase
{
	typedef NetGameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketFrameField frame;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketWrapperCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketWrapperCommandData
{
	typedef NetWrapperCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort wrappedCommandId;
		UnsignedInt chunkNumber;
		UnsignedInt numChunks;
		UnsignedInt totalDataLength;
		UnsignedInt dataLength;
		UnsignedInt dataOffset;
		// GeneralsX @security Claude 06/10/2026 Appended (wire order: append only). wrapSeq = the wrapping hop's per-connection
		// serialization number; lowWater = min(oldest wrapSeq with an unacked chunk on that connection, wrapSeq), so always
		// <= wrapSeq. The receiver frees everything of this source below lowWater (DESIGN_wrapper-memory-cap v16 section 2).
		UnsignedInt wrapSeq;
		UnsignedInt lowWater;
	};

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketWrapperCommandBase
{
	typedef NetWrapperCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketWrapperAckCommand
////////////////////////////////////////////////////////////////////////////////

// GeneralsX @security Claude 06/10/2026 The ack of one wrapper chunk (NETCOMMANDTYPE_WRAPPERACK). The header is the ack's:
// type and player only.
struct NetPacketWrapperAckCommandData
{
	typedef NetWrapperAckCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte originalPlayerId;	// the chunk's player (the inner command's originator)
		UnsignedInt wrapSeq;
		UnsignedInt chunkNumber;
	};

	static size_t getSize(const NetCommandMsg &) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketWrapperAckCommandBase
{
	typedef NetWrapperAckCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketPlayerIdField playerId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileCommandData
{
	typedef NetFileCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketFileCommandBase
{
	typedef NetFileCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileAnnounceCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileAnnounceCommandData
{
	typedef NetFileAnnounceCommandMsg CommandMsg;

	static size_t getSize(const NetCommandMsg &msg);
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketFileAnnounceCommandBase
{
	typedef NetFileAnnounceCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFileProgressCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFileProgressCommandData
{
	typedef NetFileProgressCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedShort fileId;
		Int progress;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketFileProgressCommandBase
{
	typedef NetFileProgressCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketProgressCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketProgressCommandData
{
	typedef NetProgressCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedByte percentage;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketProgressCommandBase
{
	typedef NetProgressCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		//NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketLoadCompleteCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketLoadCompleteCommandBase
{
	typedef NetLoadCompleteCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketTimeOutGameStartCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketTimeOutGameStartCommandBase
{
	typedef NetTimeOutGameStartCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectFrameCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectFrameCommandData
{
	typedef NetDisconnectFrameCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt disconnectFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDisconnectFrameCommandBase
{
	typedef NetDisconnectFrameCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketDisconnectScreenOffCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketDisconnectScreenOffCommandData
{
	typedef NetDisconnectScreenOffCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt newFrame;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketDisconnectScreenOffCommandBase
{
	typedef NetDisconnectScreenOffCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		//NetPacketFrameField frame;
		NetPacketPlayerIdField playerId;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////
// NetPacketFrameResendRequestCommand
////////////////////////////////////////////////////////////////////////////////

struct NetPacketFrameResendRequestCommandData
{
	typedef NetFrameResendRequestCommandMsg CommandMsg;

	struct FixedData
	{
		UnsignedInt frameToResend;
	};

	static size_t getSize(const NetCommandMsg &msg) { return sizeof(FixedData); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

struct NetPacketFrameResendRequestCommandBase
{
	typedef NetFrameResendRequestCommandMsg CommandMsg;

	struct CommandBase
	{
		NetPacketCommandTypeField commandType;
		NetPacketRelayField relay;
		NetPacketPlayerIdField playerId;
		//NetPacketFrameField frame;
		NetPacketCommandIdField commandId;
		NetPacketDataField dataHeader;
	};

	static size_t getSize() { return sizeof(CommandBase); }
	static size_t copyBytes(UnsignedByte *buffer, const NetCommandRef &ref);
};

////////////////////////////////////////////////////////////////////////////////

struct NetPacketAckCommand                      : public NetPacketCommandTemplate<NetPacketAckCommandBase, NetPacketAckCommandData> {};
struct NetPacketFrameCommand                    : public NetPacketCommandTemplate<NetPacketFrameCommandBase, NetPacketFrameCommandData> {};
struct NetPacketPlayerLeaveCommand              : public NetPacketCommandTemplate<NetPacketPlayerLeaveCommandBase, NetPacketPlayerLeaveCommandData> {};
struct NetPacketRunAheadMetricsCommand          : public NetPacketCommandTemplate<NetPacketRunAheadMetricsCommandBase, NetPacketRunAheadMetricsCommandData> {};
struct NetPacketRunAheadCommand                 : public NetPacketCommandTemplate<NetPacketRunAheadCommandBase, NetPacketRunAheadCommandData> {};
struct NetPacketDestroyPlayerCommand            : public NetPacketCommandTemplate<NetPacketDestroyPlayerCommandBase, NetPacketDestroyPlayerCommandData> {};
struct NetPacketLatencyReportCommand            : public NetPacketCommandTemplate<NetPacketLatencyReportCommandBase, NetPacketLatencyReportCommandData> {};
struct NetPacketKeepAliveCommand                : public NetPacketCommandTemplate<NetPacketKeepAliveCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectKeepAliveCommand      : public NetPacketCommandTemplate<NetPacketDisconnectKeepAliveCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectPlayerCommand         : public NetPacketCommandTemplate<NetPacketDisconnectPlayerCommandBase, NetPacketDisconnectPlayerCommandData> {};
struct NetPacketRouterQueryCommand              : public NetPacketCommandTemplate<NetPacketRouterQueryCommandBase, NetPacketNoData> {};
struct NetPacketRouterAckCommand                : public NetPacketCommandTemplate<NetPacketRouterAckCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectVoteCommand           : public NetPacketCommandTemplate<NetPacketDisconnectVoteCommandBase, NetPacketDisconnectVoteCommandData> {};
struct NetPacketChatCommand                     : public NetPacketCommandTemplate<NetPacketChatCommandBase, NetPacketChatCommandData> {};
struct NetPacketDisconnectChatCommand           : public NetPacketCommandTemplate<NetPacketDisconnectChatCommandBase, NetPacketDisconnectChatCommandData> {};
struct NetPacketGameCommand                     : public NetPacketCommandTemplate<NetPacketGameCommandBase, NetPacketGameCommandData> {};
struct NetPacketWrapperCommand                  : public NetPacketCommandTemplate<NetPacketWrapperCommandBase, NetPacketWrapperCommandData> {};
struct NetPacketWrapperAckCommand               : public NetPacketCommandTemplate<NetPacketWrapperAckCommandBase, NetPacketWrapperAckCommandData> {};
struct NetPacketFileCommand                     : public NetPacketCommandTemplate<NetPacketFileCommandBase, NetPacketFileCommandData> {};
struct NetPacketFileAnnounceCommand             : public NetPacketCommandTemplate<NetPacketFileAnnounceCommandBase, NetPacketFileAnnounceCommandData> {};
struct NetPacketFileProgressCommand             : public NetPacketCommandTemplate<NetPacketFileProgressCommandBase, NetPacketFileProgressCommandData> {};
struct NetPacketProgressCommand                 : public NetPacketCommandTemplate<NetPacketProgressCommandBase, NetPacketProgressCommandData> {};
struct NetPacketLoadCompleteCommand             : public NetPacketCommandTemplate<NetPacketLoadCompleteCommandBase, NetPacketNoData> {};
struct NetPacketTimeOutGameStartCommand         : public NetPacketCommandTemplate<NetPacketTimeOutGameStartCommandBase, NetPacketNoData> {};
struct NetPacketDisconnectFrameCommand          : public NetPacketCommandTemplate<NetPacketDisconnectFrameCommandBase, NetPacketDisconnectFrameCommandData> {};
struct NetPacketDisconnectScreenOffCommand      : public NetPacketCommandTemplate<NetPacketDisconnectScreenOffCommandBase, NetPacketDisconnectScreenOffCommandData> {};
struct NetPacketFrameResendRequestCommand       : public NetPacketCommandTemplate<NetPacketFrameResendRequestCommandBase, NetPacketFrameResendRequestCommandData> {};

struct SmallNetPacketAckCommand                 : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketAckCommandData> {};
struct SmallNetPacketFrameCommand               : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFrameCommandData> {};
struct SmallNetPacketPlayerLeaveCommand         : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketPlayerLeaveCommandData> {};
struct SmallNetPacketRunAheadMetricsCommand     : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketRunAheadMetricsCommandData> {};
struct SmallNetPacketRunAheadCommand            : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketRunAheadCommandData> {};
struct SmallNetPacketDestroyPlayerCommand       : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDestroyPlayerCommandData> {};
struct SmallNetPacketLatencyReportCommand       : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketLatencyReportCommandData> {};
struct SmallNetPacketKeepAliveCommand           : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectKeepAliveCommand : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectPlayerCommand    : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectPlayerCommandData> {};
struct SmallNetPacketRouterQueryCommand         : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketRouterAckCommand           : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectVoteCommand      : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectVoteCommandData> {};
struct SmallNetPacketChatCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketChatCommandData> {};
struct SmallNetPacketDisconnectChatCommand      : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectChatCommandData> {};
struct SmallNetPacketGameCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketGameCommandData> {};
struct SmallNetPacketWrapperCommand             : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketWrapperCommandData> {};
struct SmallNetPacketWrapperAckCommand          : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketWrapperAckCommandData> {};
struct SmallNetPacketFileCommand                : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileCommandData> {};
struct SmallNetPacketFileAnnounceCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileAnnounceCommandData> {};
struct SmallNetPacketFileProgressCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFileProgressCommandData> {};
struct SmallNetPacketProgressCommand            : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketProgressCommandData> {};
struct SmallNetPacketLoadCompleteCommand        : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketTimeOutGameStartCommand    : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketNoData> {};
struct SmallNetPacketDisconnectFrameCommand     : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectFrameCommandData> {};
struct SmallNetPacketDisconnectScreenOffCommand : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketDisconnectScreenOffCommandData> {};
struct SmallNetPacketFrameResendRequestCommand  : public SmallNetPacketCommandTemplate<SmallNetPacketCommandBase, NetPacketFrameResendRequestCommandData> {};

// Restore normal struct packing
#pragma pack(pop)
