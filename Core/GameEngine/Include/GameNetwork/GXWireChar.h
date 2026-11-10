/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// GeneralsX: THE WIDE CHARACTER IS 16 BITS ON THE WIRE AND IN THE .rep,
// ON EVERY PLATFORM.
//
// `WideChar` is a bare `typedef wchar_t` (Lib/BaseType.h:36) with no platform guard, and wchar_t is
// 2 bytes under MSVC and 4 under clang/gcc. MEASURED on this tree, not assumed: no `-fshort-wchar`
// appears anywhere in the repository or in either build/*/compile_commands.json, and a probe built
// with the project's own macOS flags prints sizeof(wchar_t)=4.
//
// Four sites serialised a wide character at the HOST's width:
//
//   NetPacketGameCommandData::getSize / ::copyBytes   (NetPacketStructs.cpp) - the LOCKSTEP wire
//   NetPacket::readGameMessageArgumentFromPacket      (NetPacket.cpp)        - its reader
//   RecorderClass::writeArgument / ::readArgument     (Recorder.cpp, x2 games) - the .rep body
//
// so a macOS/Linux/iPadOS peer emitted 4 bytes per character where a Windows peer emitted 2. The
// harm is not mojibake. NetPacket::getCommandList walks ONE shared cursor `i` through the datagram
// and every reader advances it; readGameMessage is not even handed a bound. A game command whose
// payload is 2n bytes on the wire but consumed as 4n leaves the cursor 2n bytes away from the next
// command's type byte, so every command packed behind it in that datagram - all of which are
// NETCOMMANDTYPE_GAMECOMMAND, the lockstep-critical kind - is decoded from the wrong offset.
//
// The chat payloads were given exactly this treatment on 17/08/2026 (network::NetWireChar, which is
// now an alias of the type below) and the LAN lobby before that (`typedef uint16_t
// WideCharWindows`, LANAPI.h:239). The game-command and replay paths were simply never included.
// Sixteen bits is the retail-compatible choice: it is what the Windows build has always written, so
// the three clang platforms conform to Windows rather than inventing a third format.
//
// WHY THIS LIVES IN ITS OWN HEADER RATHER THAN BESIDE NetWireChar. NetPacketStructs.h cannot be
// included on its own - it pulls NetworkDefs.h -> MessageStream.h -> GameCommon.h -> WWCommon.h and
// then the CompatLib/dxvk windows headers, i.e. most of the engine. A width bug is exactly the kind
// that needs a two-arm `-fshort-wchar` test (scripts/test/widechar-wire-test.sh), and that test has
// to be able to include the REAL code it grades rather than a copy of the rule. Same reason
// GXChatLine.h exists.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/// The wide character as it appears in a packet or a .rep, on every platform.
typedef uint16_t GXWireChar;

enum { GX_WIRECHAR_BYTES = sizeof(GXWireChar) };

/// Fold a host wide character down to the 16-bit wire unit. Anything outside the BMP becomes '?'
/// so that the count of units written always equals the count getSize() billed - a writer that can
/// emit fewer units than its estimator promised is the boundary overrun fixed in 3c340511f.
///
/// The mask keeps a SIGNED wchar_t (gcc on Linux makes it `int`) from sign-extending into the '?'
/// branch through the widening cast; it is a no-op for every valid code point on 32-bit hosts.
///
/// GeneralsX: SURROGATE UNITS FOLD TOO, ON BOTH SIDES. A 16-bit host has no
/// out-of-BMP value to fold: a Windows edit box holds U+1F600 as the pair 0xD83D 0xDE00
/// (IMEManager forwards WM_CHAR one UTF-16 unit at a time), and each unit is <= 0xFFFF, so the test
/// above let both through. A 32-bit peer then rebuilt them as two standalone surrogate values -
/// not characters in UTF-32 - while a 32-bit writer folded the same emoji to '?'. Folding every unit
/// in 0xD800-0xDFFF gives one rule on every width: the wire never carries a surrogate. The count of
/// units still equals the count billed; an emoji typed on Windows reads "??" rather than "?".
/// Chat needs no change: GXSanitizeChatText already drops surrogates at its reader.
inline bool gxWireCharIsSurrogate(unsigned long long cp)
{
	return cp >= 0xD800ull && cp <= 0xDFFFull;
}

inline GXWireChar gxWireCharToWire(wchar_t c)
{
	const unsigned long long hostMask = (sizeof(wchar_t) >= 4) ? 0xFFFFFFFFull : 0xFFFFull;
	const unsigned long long cp = (unsigned long long)c & hostMask;
	return (cp > 0xFFFFull || gxWireCharIsSurrogate(cp)) ? (GXWireChar)'?' : (GXWireChar)cp;
}

/// Widen a wire unit back to a host wide character. Exact for every non-surrogate unit. A surrogate
/// unit - from a pre-fix Windows writer, an older Windows .rep, or a modified peer - becomes '?' so
/// that no host string built from the wire holds one, whatever sizeof(wchar_t) is.
inline wchar_t gxWireCharToHost(GXWireChar unit)
{
	return gxWireCharIsSurrogate(unit) ? (wchar_t)'?' : (wchar_t)unit;
}

/// Write one wide character as its 16-bit wire unit. Returns the bytes written, which is
/// GX_WIRECHAR_BYTES unconditionally.
inline size_t gxWireCharWrite(unsigned char *dest, wchar_t c)
{
	const GXWireChar unit = gxWireCharToWire(c);
	memcpy(dest, &unit, sizeof(unit));
	return sizeof(unit);
}

/// Read one 16-bit wire unit into a host wide character. Returns the bytes consumed.
inline size_t gxWireCharRead(const unsigned char *src, wchar_t &out)
{
	GXWireChar unit = 0;
	memcpy(&unit, src, sizeof(unit));
	out = gxWireCharToHost(unit);
	return sizeof(unit);
}
