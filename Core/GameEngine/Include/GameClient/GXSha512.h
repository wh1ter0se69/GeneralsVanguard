/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// SHA-512 (FIPS 180-4), portable: Ed25519 verification hashes R || A || M with it; no OS hash exists on all platforms.
// Public inputs only, so not constant time, but it must be exactly SHA-512 (scripts/test/identity-assert-test.cpp).
// In the digest by name in simsourcedigest_watcher.cmake: GXAdminOrder.h verifies through gx_ed25519_verify and so
// through this, so a peer with a different hash accepts different orders.

#ifndef GX_SHA512_H
#define GX_SHA512_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One-shot. `out` receives 64 bytes. A NULL msg is legal when len is 0.
void gx_sha512(const void *msg, size_t len, unsigned char out[64]);

// Three parts hashed as one message, without concatenating them into a buffer first.
//
// This shape exists because Ed25519 hashes R || A || M, where M is caller-supplied and unbounded -
// concatenating would mean an allocation sized by an attacker. Any part may be NULL when its length
// is 0. It is the only entry point gx_ed25519_verify uses; gx_sha512 is this with two empty parts.
void gx_sha512_3(const void *a, size_t alen, const void *b, size_t blen,
                 const void *c, size_t clen, unsigned char out[64]);

#ifdef __cplusplus
}
#endif

#endif
