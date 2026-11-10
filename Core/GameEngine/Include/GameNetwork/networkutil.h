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

// GeneralsX: In-game chat is attacker-controlled text.
//
// The only thing standing between another player's keyboard and TheInGameUI's message list is this
// function. It is applied TWICE on purpose: once in ConnectionManager::sendChat, so the local echo
// and the wire copy are identical and a local IME cannot smuggle anything out; and once in
// NetPacket::readChatMessage, because a hostile peer obviously does not run our send path.
//
// What it removes and why:
//   * C0 (< 0x20) and DEL and C1 (0x80-0x9F). A newline in a chat line is the classic way to push
//     a single message across several rows of the message list, and the message list is drawn
//     over the tactical view.
//   * Lone surrogates and the U+FFFE/U+FFFF non-characters, which are not text and which the
//     Windows 16-bit and POSIX 32-bit wchar_t paths disagree about.
//   * The bidi controls: U+200B-U+200F, U+202A-U+202E, U+2066-U+2069, U+FEFF, and the U+2028/2029
//     separators. An unterminated RLO reorders every glyph drawn AFTER it, so one player could
//     scramble the rest of another player's HUD text rather than merely their own line.
//
// It is deliberately a whitelist-shaped blacklist over ranges rather than a call into any locale
// aware classifier: this runs on four platforms whose iswprint() do not agree, and a chat filter
// that behaves differently per platform is a filter that has not been tested.
//
// GX_NET_CHAT_MAX_CHARS bounds the result. The shipped InGameChat.wnd entry field is MAXLEN 64, so
// this is nearly double what any stock client can produce, while staying far below
// UnicodeString::MAX_FORMAT_BUF_LEN (2048) - which matters because processChat runs the text
// through UnicodeString::format, and POSIX vswprintf returns -1 rather than truncating on overflow,
// so an over-long line would silently discard the WHOLE row instead of clipping it.
const Int GX_NET_CHAT_MAX_CHARS = 127;

/// Strip everything that is not safely displayable and clamp to GX_NET_CHAT_MAX_CHARS.
/// Returns TRUE when anything survived; a line that sanitises away to nothing is not worth drawing.
Bool GXSanitizeChatText(UnicodeString &text);

// GeneralsX: [GXNET] counters.
//
// Free-running totals for the four events that decide whether a lockstep stall is packet loss or
// something else. They exist because every DEBUG_LOG on every one of those paths compiles to
// ((void)0) in a deploy build (RTS_DEBUG is not defined for RelWithDebInfo without
// RTS_BUILD_OPTION_DEBUG), which is exactly why two previous stutter fixes were aimed at the
// render loop - it was the only observable part of the system.
//
// Plain globals rather than members, deliberately: a member on Connection or Transport would be
// per-object and would have to be summed over slots by something that can see them all, and both
// classes reset their own metrics on their own schedules (Connection::doRetryMetrics zeroes
// m_numRetries every 10 s). These only ever go up; the reader takes deltas.
//
// Reading them costs nothing when the trace is off - the increments are unconditional but are a
// single add on paths that already do a syscall or a memcpy.
extern UnsignedInt TheGxNetRetries;    ///< ack-requiring commands re-offered after m_retryTime
extern UnsignedInt TheGxNetSendFails;  ///< Transport::doSend, socket write returned <= 0
extern UnsignedInt TheGxNetSendDrops;  ///< of those, the ones discarded as permanently undeliverable
extern UnsignedInt TheGxNetRecvDrops;  ///< datagrams read off the socket with no free in-buffer slot

// GeneralsX: A file-transfer progress value meaning "this peer could not
// write the file". The progress field is a signed Int on the wire (NetCommandMsg.h:597-604), so a
// negative value costs no protocol change and cannot collide with a real 0..100 percentage.
//
// WHY THIS EXISTS. ConnectionManager::processFile reported a flat 100 whether or not openFile
// succeeded - the only trace of a failure was a DEBUG_LOG that is compiled out of shipping builds.
// The host's transfer loop takes the MIN across slots and calls 100 "MapTransfer:Done", so a peer
// that wrote nothing still reported the map delivered. That is the reported 2026-08-10 symptom in
// full: the owner watched a map be "sent, delivered, done" and then found that only Windows could
// play it. The bytes really were lost on the other three; nothing on either side said so.
//
// A silent false success is the worst available failure mode here, because the joiner then bails on
// its own map check (LANAPICallbacks.cpp:261) and the host is left believing the transfer worked.
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
