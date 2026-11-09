// GeneralsX @feature Claude 18/09/2026 - a clear the optimiser is not allowed to remove.
//
// WHY NOT memset. A memset whose result is never read again is a dead store, and the standard lets a
// compiler delete it - which it does, at -O2, as soon as it can see the buffer is not read before it
// dies. Measured, not assumed: scripts/test/secure-clear-elision.sh compiles the same function both
// ways at -O2 and shows the memset version emits no clear at all. For a buffer that held a bearer
// token (the sign-in assertion), "cleared in the source, still in memory in the binary" is the
// failure this exists to prevent.
//
// HOW. Every byte is written through a pointer to VOLATILE. An access through a volatile lvalue is
// observable behaviour ([intro.execution]), so no conforming compiler may elide, merge or reorder
// these stores away - this is the same technique behind SecureZeroMemory and libsodium's fallback.
// The empty asm with a memory clobber, on GCC and Clang, additionally stops the compiler assuming
// anything about the buffer's contents across the call. Both are portable to every compiler this
// project builds with (MSVC honours volatile stores; the asm is only emitted where it is understood).
//
// PURE and dependency-free, so scripts/test/relay-identity-clear-test.cpp can include it on its own.
// It lives under GameNetwork because Transport.cpp is its caller; that path is inside the SimID
// source digest, which the change that introduced it moves on purpose (see that commit).
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
