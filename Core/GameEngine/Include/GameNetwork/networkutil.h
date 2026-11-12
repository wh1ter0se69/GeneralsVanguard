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

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"

UnsignedInt AssembleIp(UnsignedByte a, UnsignedByte b, UnsignedByte c, UnsignedByte d);
UnsignedInt ResolveIP(AsciiString host);
UnsignedShort GenerateNextCommandID();
Bool DoesCommandRequireACommandID(NetCommandType type);
Bool CommandRequiresAck(NetCommandMsg *msg);
Bool CommandRequiresDirectSend(NetCommandMsg *msg);
Bool IsCommandSynchronized(NetCommandType type);
const char* GetNetCommandTypeAsString(NetCommandType type);

// In-game chat is attacker-controlled text. Applied on send (local echo matches the wire copy) and again in
// NetPacket::readChatMessage, since a hostile peer skips our send path. Strips C0, DEL, C1, lone surrogates,
// U+FFFE/U+FFFF and bidi controls, by range because iswprint() differs per platform. The length cap stays under
// UnicodeString::MAX_FORMAT_BUF_LEN (2048): POSIX vswprintf returns -1 on overflow, dropping the whole row.
const Int GX_NET_CHAT_MAX_CHARS = 127;

/// Strip everything that is not safely displayable and clamp to GX_NET_CHAT_MAX_CHARS.
/// Returns TRUE when anything survived; a line that sanitises away to nothing is not worth drawing.
Bool GXSanitizeChatText(UnicodeString &text);

// [GXNET] counters: free-running totals separating packet loss from other lockstep stalls. Plain globals, not
// Connection/Transport members, which reset their own metrics (doRetryMetrics zeroes m_numRetries every 10 s).
// They only go up; the reader takes deltas. DEBUG_LOG is compiled out of deploy builds.
extern UnsignedInt TheGxNetRetries;    ///< ack-requiring commands re-offered after m_retryTime
extern UnsignedInt TheGxNetSendFails;  ///< Transport::doSend, socket write returned <= 0
extern UnsignedInt TheGxNetSendDrops;  ///< of those, the ones discarded as permanently undeliverable
extern UnsignedInt TheGxNetRecvDrops;  ///< datagrams read off the socket with no free in-buffer slot

// File-transfer progress value meaning this peer could not write the file. Progress is a signed Int on the wire
// (NetCommandMsg.h:597-604), so a negative value needs no protocol change and cannot collide with a real 0..100.
// Without it processFile reported 100 even when openFile failed, and the host counted the map as delivered.
const Int GX_FILE_PROGRESS_FAILED = -1;

#ifdef DEBUG_LOGGING
extern "C" {
void dumpBufferToLog(const void *vBuf, Int len, const char *fname, Int line);
};
#define LOGBUFFER(buf, len) dumpBufferToLog(buf, len, __FILE__, __LINE__)
#else
#define LOGBUFFER(buf, len)
#endif // DEBUG_LOGGING

inline UnsignedInt AssembleIp(UnsignedByte a, UnsignedByte b, UnsignedByte c, UnsignedByte d)
{
    return ((UnsignedInt)(a) << 24) |
           ((UnsignedInt)(b) << 16) |
           ((UnsignedInt)(c) << 8) |
           ((UnsignedInt)(d));
}
