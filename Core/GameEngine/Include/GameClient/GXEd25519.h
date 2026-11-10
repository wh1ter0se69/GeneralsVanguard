/* Ed25519 signature VERIFICATION, for the engine and for the launcher.
 *
 * GeneralsX: moved out of the launcher 11/08/2026
 *
 * WHY THIS FILE EXISTS AT ALL. The release manifest is Ed25519-signed (scripts/tooling/
 * gx-manifest.py) and the launcher must refuse a manifest that does not verify - that check is the
 * only thing standing between "the relay told me where to download my game from" and "anyone who
 * can forge a UDP source address told me where to download my game from". Windows CNG has no
 * Ed25519: BCrypt offers RSA, ECDSA over the NIST curves, and DH/ECDH, and X25519 only for key
 * agreement. There is no OS call to make here, and the project deliberately carries no OpenSSL on
 * Windows, so the verifier is code.
 *
 * IT NOW HAS A SECOND CALLER, WHICH IS WHY IT LIVES HERE. The identity assertion
 * (GXIdentityAssertion.h) is Ed25519-signed too, and the client's only job is to verify it - the
 * Windows engine has no HTTPS at all, so fetching is the launcher's problem and verifying is the
 * engine's. One verifier serves both. See the .cpp header for why it moved rather than being
 * copied, and for the single behavioural change that came with the move: the SHA-512 underneath is
 * GXSha512.cpp on every platform now, not CNG.
 *
 * NOTE FOR THE LAUNCHER BUILD: define GX_ED25519_WITH_CNG to get gx_sha256/gx_sha256_file. The
 * engine does not, and must not - it would pull in bcrypt.lib for helpers it never calls.
 *
 * The autoship design doc already anticipated this ("an Ed25519 verifier is ~100 lines of
 * public-domain code and vendoring one is preferable to adding OpenSSL to three platforms"). This
 * is that, written rather than fetched.
 *
 * WHAT IS AND IS NOT CLAIMED. This verifies; it never signs, and it holds no secret. Every input
 * it touches is public: a compiled-in public key, a signature and a manifest that were both
 * broadcast over plain UDP. So it is not written to be constant-time, and it does not need to be -
 * there is no secret for a timing side channel to leak. What it DOES need to be is correct in one
 * specific direction: it must never accept a signature the real Ed25519 would reject. That is
 * checked by --selftest against vectors produced by OpenSSL through Python's `cryptography`, with
 * both good and deliberately corrupted cases, plus an internal check on the group order.
 *
 * VECTORS ARE NOT A PROOF, and the ones this file shipped with proved only the happy path. The
 * arithmetic was later differential-tested against OpenSSL and against Python big integers, on
 * macOS, over inputs no signer would ever produce: 36372 field comparisons, 8280 fe_reduce9 carry
 * cases, 9000 deliberately non-canonical operands, 4860 point encodings, and 2455 verifications
 * covering small-order and identity keys, small-order R, S at 0, L-1, L, L+1, L+8, 8L, 2^255 and
 * 2^256-1, the malleable S+L, and a NULL message. Everything matched except ONE class - public
 * keys carrying a torsion component - which is why gx_ed25519_verify now requires the key to lie
 * in the prime-order subgroup. See the comment at that check.
 *
 * The field arithmetic is schoolbook: 8 x 32-bit limbs, 64-bit products, fold 2^256 == 38 (mod p).
 * It is several times slower than a radix-2^25.5 implementation and that is a deliberate trade -
 * this runs ONCE per launch, and the fast representations are the ones whose carry chains are easy
 * to get subtly wrong.
 */

#ifndef GX_ED25519_H
#define GX_ED25519_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if `sig` (64 bytes) is a valid Ed25519 signature by `pk` (32 bytes) over msg[0..len).
 * 0 for every other reason, including a malformed key, a malformed signature, or a hash failure.
 * `msg` may be NULL when len is 0.
 *
 * THREAD SAFE as of the move into the engine. It previously was not, and said so, on the grounds
 * that the launcher is single-threaded; the engine is not, so the one-time table init is now a
 * C++11 function-local static rather than a bare flag. See ge_init_once in the .cpp for why a race
 * there would have been worse than a crash.
 *
 * ACCEPT SET. Strictly conservative: canonical encodings only, S < L, and the public key must lie
 * in the prime-order subgroup and not be the identity. That last requirement is stricter than
 * libsodium, which rejects small-order keys but accepts mixed-order ones. No key any real
 * generator produces is affected - every Ed25519 public key is [s]B and therefore has order L -
 * and the direction of the strictness is the one that matters for a code-signing anchor: this
 * verifier refuses things a real Ed25519 accepts, never the reverse. */
int gx_ed25519_verify(const unsigned char *pk, const unsigned char *sig,
                      const void *msg, size_t len);

/* [L]B == identity, where L is this file's group order and B its base point. A self-consistency
 * check on the two constants that cannot be checked by inspection. 1 on success. */
int gx_ed25519_selftest_group(void);

/* GeneralsX: SIGNING, for exactly one caller: an admin's per-match key
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
