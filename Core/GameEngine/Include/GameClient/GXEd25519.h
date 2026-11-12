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

/* Ed25519 signature VERIFICATION, shared by the engine and the launcher. It verifies only:
 * every input is public, so it is not constant time. It must never accept a signature real Ed25519 rejects, and
 * it requires the public key to lie in the prime-order subgroup. SHA-512 is GXSha512.cpp on every platform.
 * Define GX_ED25519_WITH_CNG for gx_sha256/gx_sha256_file in the launcher; the engine must not (bcrypt.lib). */

#ifndef GX_ED25519_H
#define GX_ED25519_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if sig (64 bytes) is a valid Ed25519 signature by pk (32 bytes) over msg[0..len), else 0.
 * msg may be NULL when len is 0. Thread safe: the one-time table init is a function-local static.
 * Strictly conservative: canonical encodings only, S < L, key in the prime-order subgroup and not the identity.
 * It refuses things real Ed25519 accepts, never the reverse. */
int gx_ed25519_verify(const unsigned char *pk, const unsigned char *sig,
                      const void *msg, size_t len);

/* [L]B == identity, where L is this file's group order and B its base point. A self-consistency
 * check on the two constants that cannot be checked by inspection. 1 on success. */
int gx_ed25519_selftest_group(void);

/* SIGNING, for exactly one caller: an admin's per-match key
 * (GameClient/GXAdminKey.h), which signs that admin's cheat orders so no other peer can send one in the
 * admin's name (Common/GXAdminOrder.h). RFC 8032 Ed25519 from a 32-byte seed; deterministic, so the output
 * is checked byte for byte against RFC 8032's vectors and against node:crypto by
 * scripts/test/ed25519-sign-test.sh.
 *
 * NOT CONSTANT TIME, and the verifier's reason for not needing to be does not carry over - this one holds a
 * secret. It is acceptable for this caller and no other: the seed is a throwaway minted per match on the
 * player's own machine, and what it protects is the attribution of cheat orders for that one match. Do not
 * point it at a long-lived key. 1 on success, 0 when the group tables could not be built. */
int gx_ed25519_public_from_seed(const unsigned char seed[32], unsigned char pk[32]);
int gx_ed25519_sign(unsigned char sig[64], const void *msg, size_t len, const unsigned char seed[32]);

/* One-shot hashes through CNG. 1 on success.
 *
 * LAUNCHER ONLY, and gated so the engine cannot accidentally acquire bcrypt.lib by calling one.
 * These are the manifest hash and the downloaded-artifact hash; the second wants a streaming file
 * read that has no engine equivalent. The Ed25519 path above does NOT go through here - it hashes
 * with GXSha512.cpp on every platform. */
#if defined(GX_ED25519_WITH_CNG)
int gx_sha256(const void *data, size_t len, unsigned char out[32]);
int gx_sha256_file(const wchar_t *path, unsigned char out[32]);
#endif

#ifdef __cplusplus
}
#endif

#endif
