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

// A clear the optimiser is not allowed to remove. A memset of a buffer that is never read again is a dead store the
// compiler may delete, leaving a bearer token in memory. Every byte is written through a volatile pointer, and the
// empty asm with a memory clobber (GCC and Clang) stops assumptions about the buffer afterwards.
// Dependency-free so a test can include it on its own.
#pragma once

#include <stddef.h>

/// Zero `n` bytes at `p`, in a way the compiler cannot remove.
inline void gxSecureClear( void *p, size_t n )
{
	if (p == NULL)
		return;
	volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
	while (n--)
		*v++ = 0;
#if defined(__GNUC__) || defined(__clang__)
	__asm__ __volatile__("" : : "r"(p) : "memory");
#endif
}

/// The WHOLE array, sized by its type - so a caller cannot clear "the first byte" or pass the wrong
/// length by accident. Prefer this form for fixed buffers.
template <size_t N>
inline void gxSecureClear( char (&buf)[N] )
{
	gxSecureClear(static_cast<void *>(buf), N);
}
