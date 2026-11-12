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

// The wide character is 16 bits on the wire and in the .rep, on every platform. wchar_t is 2 bytes under MSVC and
// 4 under clang/gcc. One shared cursor walks the datagram, so a writer using the host width misaligns every command
// packed behind it, all of them lockstep GAMECOMMANDs. 16 bits is the retail Windows format.
// Own header so the width test can include the real code without pulling in the engine.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/// The wide character as it appears in a packet or a .rep, on every platform.
typedef uint16_t GXWireChar;

enum { GX_WIRECHAR_BYTES = sizeof(GXWireChar) };

/// Fold a host wide character to the 16-bit wire unit. Anything outside the BMP, and every surrogate unit
/// (0xD800-0xDFFF), becomes a question mark, so the units written always equal the count getSize() billed and the
/// wire never carries a surrogate on any host width. The mask keeps a signed wchar_t from sign-extending into the
/// fold branch.
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
