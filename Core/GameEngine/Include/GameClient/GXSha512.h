/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX: SHA-512, FIPS 180-4, portable.
//
// WHY THIS EXISTS. Ed25519 verification hashes R || A || M with SHA-512, and there was no portable
// SHA-512 in this tree. The one implementation that existed lived in the Windows launcher's
// GXEd25519 and hashed through CNG (BCryptCreateHash), which is an OS call three of our four
// platforms do not have. So the engine could not verify an Ed25519 signature at all.
//
// WHY NOT REACH FOR AN OS CALL PER PLATFORM. CommonCrypto on Apple, OpenSSL on Linux, CNG on
// Windows - three implementations, three link lines, and the one that matters most is the one that
// cannot be exercised here. GXPlayerIdentity.cpp:50-56 already recorded the same conclusion for the
// CSPRNG and refused to add bcrypt.lib for it. ~180 lines with published test vectors is a smaller
// liability than a build-configuration difference between platforms that must agree bit for bit.
//
// WHAT IS AND IS NOT CLAIMED. This is a hash, not a MAC and not a KDF. Every byte it touches here
// is public - a signature, a public key, and a document that was broadcast - so it is not written
// to be constant time and does not need to be. What it MUST be is exactly SHA-512, because a
// verifier built on a nearly-correct hash rejects valid signatures and, worse, would accept a
// different message set than every other implementation. That is checked in
// scripts/test/identity-assert-test.cpp against the five FIPS 180-4 published examples, against the
// block and length-encoding boundaries where an off-by-one pad hides, and transitively against
// OpenSSL through the RFC 8032 and launcher-vectors Ed25519 runs.
//
// DIGEST. Inside it since 03/10/2026, by name in simsourcedigest_watcher.cmake: GXAdminOrder.h, which
// GameLogicDispatch uses to decide whether a signed admin order runs, verifies through gx_ed25519_verify and
// so through this. A peer with a different hash accepts a different set of orders.

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
