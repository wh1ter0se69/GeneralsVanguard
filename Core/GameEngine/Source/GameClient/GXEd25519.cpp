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

/* Ed25519 verification. See GXEd25519.h for why this is written rather than called, and for the
 * accept set it is deliberately stricter about than libsodium.
 * One implementation serves the launcher and the engine, since the Windows engine has no OpenSSL. SHA-512 goes
 * through GXSha512.cpp everywhere; CNG remains for SHA-256 only, behind GX_ED25519_WITH_CNG. */

#if defined(GX_ED25519_WITH_CNG)
#include <windows.h>
#include <bcrypt.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "GameClient/GXEd25519.h"
#include "GameClient/GXSha512.h"

#if defined(GX_ED25519_WITH_CNG)
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#endif

/* ============================================================================================
 * Hashing.
 * ========================================================================================== */

/* The one hash gx_ed25519_verify needs: SHA-512 over R || A || M, as three parts so an unbounded
 * caller-supplied message never has to be concatenated into a buffer first.
 *
 * PORTABLE ON EVERY PLATFORM, including Windows: it does not use BCryptCreateHash, so this
 * file is not Windows-only for the sake of one hash. GXSha512.cpp is checked against the five FIPS
 * 180-4 published examples and against OpenSSL over every length from 0 to 300; the return value is
 * kept so the call site below keeps its "a hash failure is a refusal" shape, but there is nothing
 * left in the path that can fail - no handle, no allocation, no provider to open. */
static int sha512_3(const void *a, size_t alen, const void *b, size_t blen,
                    const void *c, size_t clen, unsigned char out[64])
{
	gx_sha512_3(a, alen, b, blen, c, clen, out);
	return 1;
}

#if defined(GX_ED25519_WITH_CNG)

/* BCryptHashData takes a ULONG. size_t is 64-bit on the x64 build this ships as, so a plain cast
 * would silently hash only the low 32 bits of the length: a caller passing 2^32 + n bytes would get
 * a hash of the first n, and a signature over the short message would verify against the long one.
 * No caller here can reach 4 GiB - the manifest is one UDP datagram - but truncating a length is
 * not a failure mode an exported hashing helper should have at all. */
static int hash_chunked(BCRYPT_HASH_HANDLE hh, const void *p, size_t len)
{
	const unsigned char *b = (const unsigned char *)p;
	while (len > 0) {
		ULONG take = (len > 0x40000000u) ? 0x40000000u : (ULONG)len;
		if (BCryptHashData(hh, (PUCHAR)b, take, 0) != STATUS_SUCCESS) {
			return 0;
		}
		b += take;
		len -= take;
	}
	return 1;
}

static int hash_multi(LPCWSTR alg, const void *a, size_t alen, const void *b, size_t blen,
                      const void *c, size_t clen, unsigned char *out, size_t outlen)
{
	BCRYPT_ALG_HANDLE h = NULL;
	BCRYPT_HASH_HANDLE hh = NULL;
	int ok = 0;

	if (BCryptOpenAlgorithmProvider(&h, alg, NULL, 0) != STATUS_SUCCESS) {
		return 0;
	}
	if (BCryptCreateHash(h, &hh, NULL, 0, NULL, 0, 0) != STATUS_SUCCESS) {
		BCryptCloseAlgorithmProvider(h, 0);
		return 0;
	}
	/* A zero-length chunk is legal and common - the manifest is the third chunk and is never
	 * empty, but the first two are fixed 32-byte fields and a caller may pass NULL for the rest. */
	if (hash_chunked(hh, a, alen)
		&& hash_chunked(hh, b, blen)
		&& hash_chunked(hh, c, clen)
		&& BCryptFinishHash(hh, out, (ULONG)outlen, 0) == STATUS_SUCCESS) {
		ok = 1;
	}
	BCryptDestroyHash(hh);
	BCryptCloseAlgorithmProvider(h, 0);
	return ok;
}

int gx_sha256(const void *data, size_t len, unsigned char out[32])
{
	return hash_multi(BCRYPT_SHA256_ALGORITHM, data, len, NULL, 0, NULL, 0, out, 32);
}

int gx_sha256_file(const wchar_t *path, unsigned char out[32])
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hh = NULL;
	HANDLE f = INVALID_HANDLE_VALUE;
	unsigned char *buf = NULL;
	int ok = 0;

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		return 0;
	}
	buf = (unsigned char *)malloc(1 << 20);
	if (!buf) {
		CloseHandle(f);
		return 0;
	}
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == STATUS_SUCCESS
		&& BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS) {
		DWORD got = 0;
		ok = 1;
		/* An I/O error is NOT end of file. The old loop condition conflated them: `while (ReadFile
		 * (...) && got > 0)` exits identically whether the file ended or the read failed, and `ok`
		 * was already 1, so a failure mid-file produced a confident SHA-256 OF THE PREFIX and a
		 * failure on the first read produced SHA-256 of nothing at all - both reported as success.
		 * The caller (hash_matches) compares that against a signed manifest hash and would reject
		 * it, so this fails closed in practice, but an integrity helper that returns success on a
		 * read it did not complete is the wrong shape regardless of who happens to call it. */
		for (;;) {
			if (!ReadFile(f, buf, 1 << 20, &got, NULL)) {
				ok = 0;
				break;
			}
			if (got == 0) {
				break;		/* genuine end of file */
			}
			if (BCryptHashData(hh, buf, got, 0) != STATUS_SUCCESS) {
				ok = 0;
				break;
			}
		}
		if (ok && BCryptFinishHash(hh, out, 32, 0) != STATUS_SUCCESS) {
			ok = 0;
		}
	}
	if (hh) {
		BCryptDestroyHash(hh);
	}
	if (alg) {
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	free(buf);
	CloseHandle(f);
	return ok;
}

#endif	/* GX_ED25519_WITH_CNG */

/* ============================================================================================
 * GF(2^255 - 19)
 *
 * A field element is 8 little-endian 32-bit limbs. Every operation leaves its result FULLY
 * REDUCED into [0, p), which is what makes fe_eq a memcmp and lets the parity bit be read
 * straight off limb 0 - both of which are wrong on a representation that carries slack.
 * ========================================================================================== */

typedef uint32_t fe[8];

static const uint32_t P[8] = {
	0xffffffedu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
	0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu
};
/* 2p = 2^256 - 38. Used to make subtraction all-unsigned: a + 2p - b never goes negative for
 * a, b < p, so there is no reliance on the sign behaviour of a shifted negative integer. */
static const uint32_t TWOP[8] = {
	0xffffffdau, 0xffffffffu, 0xffffffffu, 0xffffffffu,
	0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu
};

static void fe_zero(fe r) { memset(r, 0, sizeof(fe)); }

static void fe_small(fe r, uint32_t v) { fe_zero(r); r[0] = v; }

static void fe_copy(fe r, const fe a) { memcpy(r, a, sizeof(fe)); }

/* r -= p while r >= p. r < 2^256 = 2p + 38, so twice is always enough; the third pass is free
 * insurance and costs one comparison. */
static void fe_canon(fe r)
{
	int k;
	for (k = 0; k < 3; k++) {
		uint32_t t[8];
		uint32_t borrow = 0;
		int i;
		for (i = 0; i < 8; i++) {
			uint64_t sub = (uint64_t)P[i] + borrow;
			uint64_t cur = (uint64_t)r[i];
			if (cur >= sub) {
				t[i] = (uint32_t)(cur - sub);
				borrow = 0;
			} else {
				t[i] = (uint32_t)((cur + 0x100000000ull) - sub);
				borrow = 1;
			}
		}
		if (borrow) {
			return;		/* r < p already */
		}
		memcpy(r, t, sizeof(t));
	}
}

/* r = t (9 limbs) mod p. Folds the top limb in at 2^256 == 38 until nothing is left over. */
static void fe_reduce9(fe r, const uint32_t t[9])
{
	uint64_t carry;
	int i, pass;

	memcpy(r, t, 8 * sizeof(uint32_t));
	carry = (uint64_t)t[8] * 38u;
	for (pass = 0; pass < 3 && carry; pass++) {
		for (i = 0; i < 8; i++) {
			carry += r[i];
			r[i] = (uint32_t)carry;
			carry >>= 32;
		}
		carry *= 38u;
	}
	fe_canon(r);
}

static void fe_add(fe r, const fe a, const fe b)
{
	uint32_t t[9];
	uint64_t carry = 0;
	int i;
	for (i = 0; i < 8; i++) {
		carry += (uint64_t)a[i] + b[i];
		t[i] = (uint32_t)carry;
		carry >>= 32;
	}
	t[8] = (uint32_t)carry;
	fe_reduce9(r, t);
}

static void fe_sub(fe r, const fe a, const fe b)
{
	uint32_t u[9], t[9];
	uint64_t carry = 0;
	uint32_t borrow = 0;
	int i;

	/* u = a + 2p. Larger than any b < p, so the subtraction below never underflows. */
	for (i = 0; i < 8; i++) {
		carry += (uint64_t)a[i] + TWOP[i];
		u[i] = (uint32_t)carry;
		carry >>= 32;
	}
	u[8] = (uint32_t)carry;

	for (i = 0; i < 9; i++) {
		uint64_t sub = (uint64_t)(i < 8 ? b[i] : 0u) + borrow;
		uint64_t cur = (uint64_t)u[i];
		if (cur >= sub) {
			t[i] = (uint32_t)(cur - sub);
			borrow = 0;
		} else {
			t[i] = (uint32_t)((cur + 0x100000000ull) - sub);
			borrow = 1;
		}
	}
	fe_reduce9(r, t);
}

static void fe_mul(fe r, const fe a, const fe b)
{
	uint32_t w[16];
	uint32_t t[9];
	uint64_t carry;
	int i, j;

	memset(w, 0, sizeof(w));
	for (i = 0; i < 8; i++) {
		carry = 0;
		for (j = 0; j < 8; j++) {
			/* Exactly fits: (2^32-1)^2 + 2*(2^32-1) == 2^64 - 1. */
			uint64_t cur = (uint64_t)w[i + j] + (uint64_t)a[i] * (uint64_t)b[j] + carry;
			w[i + j] = (uint32_t)cur;
			carry = cur >> 32;
		}
		{
			int k = i + 8;
			while (carry && k < 16) {
				uint64_t cur = (uint64_t)w[k] + carry;
				w[k] = (uint32_t)cur;
				carry = cur >> 32;
				k++;
			}
		}
	}

	carry = 0;
	for (i = 0; i < 8; i++) {
		carry += (uint64_t)w[i] + (uint64_t)w[8 + i] * 38u;
		t[i] = (uint32_t)carry;
		carry >>= 32;
	}
	t[8] = (uint32_t)carry;
	fe_reduce9(r, t);
}

static void fe_sq(fe r, const fe a) { fe_mul(r, a, a); }

static void fe_neg(fe r, const fe a)
{
	fe z;
	fe_zero(z);
	fe_sub(r, z, a);
}

static int fe_is_zero(const fe a)
{
	int i;
	uint32_t acc = 0;
	for (i = 0; i < 8; i++) {
		acc |= a[i];
	}
	return acc == 0;
}

static int fe_eq(const fe a, const fe b) { return memcmp(a, b, sizeof(fe)) == 0; }

static void fe_from_bytes(fe r, const unsigned char s[32])
{
	int i;
	for (i = 0; i < 8; i++) {
		r[i] = (uint32_t)s[4 * i] | ((uint32_t)s[4 * i + 1] << 8)
			| ((uint32_t)s[4 * i + 2] << 16) | ((uint32_t)s[4 * i + 3] << 24);
	}
	fe_canon(r);
}

/* r = a^e, e given as `bits` bits of a little-endian byte string. Square-and-multiply, MSB
 * first. Not constant time; every exponent here is a public constant. */
static void fe_pow(fe r, const fe a, const unsigned char *e, int bits)
{
	fe acc;
	int i;
	fe_small(acc, 1);
	for (i = bits - 1; i >= 0; i--) {
		fe_sq(acc, acc);
		if ((e[i >> 3] >> (i & 7)) & 1) {
			fe_mul(acc, acc, a);
		}
	}
	fe_copy(r, acc);
}

/* p - 2 = 2^255 - 21. */
static const unsigned char EXP_INV[32] = {
	0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
};
/* (p - 5) / 8 = 2^252 - 3. */
static const unsigned char EXP_SQRT[32] = {
	0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f
};
/* (p - 1) / 4 = 2^253 - 5, the exponent that produces sqrt(-1). */
static const unsigned char EXP_I[32] = {
	0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f
};

static void fe_inv(fe r, const fe a) { fe_pow(r, a, EXP_INV, 255); }

/* ============================================================================================
 * The curve: twisted Edwards, a = -1, extended coordinates (X:Y:Z:T) with x = X/Z, y = Y/Z,
 * xy = T/Z. Formulas are add-2008-hwcd-3 and dbl-2008-hwcd.
 *
 * EVERY CONSTANT IS DERIVED AT RUNTIME rather than typed in as 64 hex digits: d comes from
 * -121665 * inverse(121666), sqrt(-1) from 2^((p-1)/4), and the base point from decompressing
 * y = 4/5. A mistyped digit in a hardcoded constant would fail in a way that looks exactly like
 * a bad signature, and there is no way to eyeball it. The one constant that cannot be derived is
 * the group order L, and gx_ed25519_selftest_group checks it by multiplying the base point by it.
 * ========================================================================================== */

typedef struct { fe X, Y, Z, T; } ge;

static fe GE_D, GE_D2, GE_SQRTM1;
static ge GE_BASE;
static int ge_ready = 0;

static void ge_zero(ge *p)
{
	fe_zero(p->X);
	fe_small(p->Y, 1);
	fe_small(p->Z, 1);
	fe_zero(p->T);
}

static void ge_add(ge *r, const ge *p, const ge *q)
{
	fe a, b, c, d, e, f, g, h, t1, t2;

	fe_sub(t1, p->Y, p->X);
	fe_sub(t2, q->Y, q->X);
	fe_mul(a, t1, t2);

	fe_add(t1, p->Y, p->X);
	fe_add(t2, q->Y, q->X);
	fe_mul(b, t1, t2);

	fe_mul(c, p->T, GE_D2);
	fe_mul(c, c, q->T);

	fe_mul(d, p->Z, q->Z);
	fe_add(d, d, d);

	fe_sub(e, b, a);
	fe_sub(f, d, c);
	fe_add(g, d, c);
	fe_add(h, b, a);

	fe_mul(r->X, e, f);
	fe_mul(r->Y, g, h);
	fe_mul(r->T, e, h);
	fe_mul(r->Z, f, g);
}

static void ge_dbl(ge *r, const ge *p)
{
	fe a, b, c, d, e, f, g, h, t;

	fe_sq(a, p->X);
	fe_sq(b, p->Y);
	fe_sq(c, p->Z);
	fe_add(c, c, c);
	fe_neg(d, a);

	fe_add(t, p->X, p->Y);
	fe_sq(e, t);
	fe_sub(e, e, a);
	fe_sub(e, e, b);

	fe_add(g, d, b);
	fe_sub(f, g, c);
	fe_sub(h, d, b);

	fe_mul(r->X, e, f);
	fe_mul(r->Y, g, h);
	fe_mul(r->T, e, h);
	fe_mul(r->Z, f, g);
}

/* Projective equality: X1*Z2 == X2*Z1 and Y1*Z2 == Y2*Z1.
 *
 * The Z != 0 refusal is NOT a curve property - no point reachable through the complete formulas
 * above can have Z = 0. It is a fail-closed latch, and it is the cheapest one in this file. A ge
 * whose Z is zero compares equal to EVERY point, because both cross-products collapse to 0 == 0.
 * So any future path that manages to produce one - an uninitialised global, a formula transcribed
 * wrong, a half-applied edit - would quietly turn this function from "are these the same point"
 * into "accept anything", and this function is the last line of gx_ed25519_verify. One comparison
 * converts that entire class of mistake into a refused signature. */
static int ge_eq(const ge *p, const ge *q)
{
	fe l, r;
	if (fe_is_zero(p->Z) || fe_is_zero(q->Z)) {
		return 0;
	}
	fe_mul(l, p->X, q->Z);
	fe_mul(r, q->X, p->Z);
	if (!fe_eq(l, r)) {
		return 0;
	}
	fe_mul(l, p->Y, q->Z);
	fe_mul(r, q->Y, p->Z);
	return fe_eq(l, r);
}

/* r = [k]p, k a little-endian byte string of `bytes` bytes. Public data only; not constant time. */
static void ge_scalarmult(ge *r, const ge *p, const unsigned char *k, int bytes)
{
	ge acc;
	int i;
	ge_zero(&acc);
	for (i = bytes * 8 - 1; i >= 0; i--) {
		ge_dbl(&acc, &acc);
		if ((k[i >> 3] >> (i & 7)) & 1) {
			ge_add(&acc, &acc, p);
		}
	}
	*r = acc;
}

static int ge_decompress_raw(ge *r, const unsigned char s[32]);

static void ge_init(void)
{
	fe num, den, t;

	if (ge_ready) {
		return;
	}

	/* d = -121665 / 121666 */
	fe_small(num, 121665);
	fe_neg(num, num);
	fe_small(den, 121666);
	fe_inv(t, den);
	fe_mul(GE_D, num, t);
	fe_add(GE_D2, GE_D, GE_D);

	/* sqrt(-1) = 2^((p-1)/4) */
	fe_small(t, 2);
	fe_pow(GE_SQRTM1, t, EXP_I, 254);

	/* B: y = 4/5, x the even root. Encoded with the sign bit clear, which is exactly the
	 * standard encoding of the base point, so this is a decompression rather than a constant.
	 *
	 * ge_decompress needs GE_D and GE_SQRTM1, which are set above - it does NOT need ge_ready and
	 * deliberately does not consult it. A re-entrant call guarded by setting
	 * ge_ready = 1 FIRST would publish readiness while GE_BASE was still all zeroes. A caller
	 * landing in that window would multiply by a zero point, and a zeroed ge compares equal to
	 * every point - that window is "accept any forged signature". So
	 * readiness is published once, last, and only on success. */
	{
		unsigned char enc[32];
		fe y;
		int i;
		fe_small(num, 4);
		fe_small(den, 5);
		fe_inv(t, den);
		fe_mul(y, num, t);
		for (i = 0; i < 8; i++) {
			enc[4 * i] = (unsigned char)(y[i] & 0xff);
			enc[4 * i + 1] = (unsigned char)((y[i] >> 8) & 0xff);
			enc[4 * i + 2] = (unsigned char)((y[i] >> 16) & 0xff);
			enc[4 * i + 3] = (unsigned char)((y[i] >> 24) & 0xff);
		}
		enc[31] &= 0x7f;
		if (!ge_decompress_raw(&GE_BASE, enc)) {
			return;		/* cannot happen for a valid curve; refuses everything if it does */
		}
	}

	ge_ready = 1;
}

/* y is canonical (< p) - checked, because a non-canonical encoding is a different point on some
 * implementations and the same one here, and "the same signature verifies differently in two
 * places" is the class of bug that splits a lockstep network. */
static int fe_bytes_canonical(const unsigned char s[32])
{
	fe a;
	int i;
	unsigned char re[32];
	fe_from_bytes(a, s);		/* fe_from_bytes canonicalises */
	for (i = 0; i < 8; i++) {
		re[4 * i] = (unsigned char)(a[i] & 0xff);
		re[4 * i + 1] = (unsigned char)((a[i] >> 8) & 0xff);
		re[4 * i + 2] = (unsigned char)((a[i] >> 16) & 0xff);
		re[4 * i + 3] = (unsigned char)((a[i] >> 24) & 0xff);
	}
	return memcmp(re, s, 32) == 0;
}

/* The worker. PRECONDITION: GE_D and GE_SQRTM1 are initialised. It must NOT call ge_init - that
 * recursion would force readiness to be published before the base point existed. ge_init calls
 * this one directly, mid-initialisation; everybody else goes through the wrapper below, which is
 * caller-proof.
 *
 * Splitting it rather than documenting a precondition on the single function is not ceremony:
 * a harness that drives decompression directly would silently get GE_D = 0 and produce
 * wrong points, and a comment does nothing to prevent that. A precondition a caller can forget is a bug with a
 * waiting period. */
static int ge_decompress_raw(ge *r, const unsigned char s[32])
{
	unsigned char yb[32];
	fe u, v, v3, v7, x, t, chk;
	int sign;

	memcpy(yb, s, 32);
	sign = yb[31] >> 7;
	yb[31] &= 0x7f;
	if (!fe_bytes_canonical(yb)) {
		return 0;
	}
	fe_from_bytes(r->Y, yb);

	/* u = y^2 - 1, v = d*y^2 + 1 */
	fe_sq(u, r->Y);
	fe_mul(v, u, GE_D);
	fe_small(t, 1);
	fe_sub(u, u, t);
	fe_add(v, v, t);

	/* x = u*v^3 * (u*v^7)^((p-5)/8) */
	fe_sq(v3, v);
	fe_mul(v3, v3, v);			/* v^3 */
	fe_sq(v7, v3);
	fe_mul(v7, v7, v);			/* v^7 */
	fe_mul(t, u, v7);
	fe_pow(t, t, EXP_SQRT, 252);
	fe_mul(x, u, v3);
	fe_mul(x, x, t);

	/* v*x^2 must be u, or -u in which case x is off by sqrt(-1). */
	fe_sq(chk, x);
	fe_mul(chk, chk, v);
	if (!fe_eq(chk, u)) {
		fe_neg(t, u);
		if (!fe_eq(chk, t)) {
			return 0;
		}
		fe_mul(x, x, GE_SQRTM1);
	}

	if (fe_is_zero(x) && sign) {
		return 0;		/* the only encoding of x = 0 has a clear sign bit */
	}
	if ((int)(x[0] & 1) != sign) {
		fe_neg(x, x);
	}

	fe_copy(r->X, x);
	fe_small(r->Z, 1);
	fe_mul(r->T, r->X, r->Y);
	return 1;
}

/* The one-time init is synchronised because the engine is threaded. ge_init publishes ge_ready last: publishing it
 * first opens a window where GE_BASE is still zero and any forged signature is accepted. A C++11 function-local
 * static is initialised once with a happens-before edge to every observer, which is why this file is C++. */
static void ge_init_once(void)
{
	struct Initialiser { Initialiser() { ge_init(); } };
	static Initialiser once;
	(void)once;
}

/* The wrapper every caller outside ge_init uses. Initialises on demand and refuses if the tables
 * could not be built, so no caller can reach the arithmetic with a zeroed GE_D. */
static int ge_decompress(ge *r, const unsigned char s[32])
{
	ge_init_once();
	if (!ge_ready) {
		return 0;
	}
	return ge_decompress_raw(r, s);
}

/* L = 2^252 + 27742317777372353535851937790883648493, little-endian. The one constant that is
 * not derived; gx_ed25519_selftest_group is what checks it. */
static const unsigned char GROUP_L[32] = {
	0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static int scalar_lt_L(const unsigned char s[32])
{
	int i;
	for (i = 31; i >= 0; i--) {
		if (s[i] != GROUP_L[i]) {
			return s[i] < GROUP_L[i];
		}
	}
	return 0;		/* s == L is not < L */
}

/* out = in / p for a small p, returning the remainder. Little-endian, most significant limb first
 * so the running remainder carries downward. */
static unsigned scalar_div_small(const unsigned char in[32], unsigned p, unsigned char out[32])
{
	unsigned rem = 0;
	int i;
	for (i = 31; i >= 0; i--) {
		unsigned cur = (rem << 8) | in[i];
		out[i] = (unsigned char)(cur / p);
		rem = cur % p;
	}
	return rem;
}

int gx_ed25519_selftest_group(void)
{
	/* The cofactors this can catch. The true L is an odd prime, so NONE of these divide it and
	 * for a correct constant this whole loop is arithmetic only - it never reaches a scalarmult. */
	static const unsigned SMALL[] = { 2u, 3u, 5u, 7u, 11u, 13u };

	ge t, id, probe;
	unsigned char q[32];
	unsigned k;

	ge_init_once();
	if (!ge_ready) {
		return 0;
	}
	ge_zero(&id);

	ge_scalarmult(&t, &GE_BASE, GROUP_L, 32);
	if (!ge_eq(&t, &id)) {
		return 0;
	}

	/* [L]B == identity alone holds for ANY multiple of the true order: a GROUP_L of k*L would let scalar_lt_L admit
	 * every S in [L, k*L), disabling the malleability guard since [S+L]B == [S]B. Halving catches even multiples only,
	 * so divide exactly: if a small prime divides GROUP_L and the quotient is an order of B, refuse.
	 * Not caught: k a large prime. */
	for (k = 0; k < sizeof(SMALL) / sizeof(SMALL[0]); k++) {
		if (scalar_div_small(GROUP_L, SMALL[k], q) != 0) {
			continue;		/* does not divide - nothing to say */
		}
		ge_scalarmult(&probe, &GE_BASE, q, 32);

		/* Fail CLOSED on a degenerate point. ge_eq returns 0 both for "different points" and for
		 * "Z is zero", so `return !ge_eq(...)` - which is what this function used to end with -
		 * turned exactly the state the Z != 0 latch exists to catch into a reported PASS. */
		if (fe_is_zero(probe.Z)) {
			return 0;
		}
		if (ge_eq(&probe, &id)) {
			return 0;		/* GROUP_L is a multiple of a real order */
		}
	}
	return 1;
}

int gx_ed25519_verify(const unsigned char *pk, const unsigned char *sig,
                      const void *msg, size_t len)
{
	ge a, rpoint, sb, ha, rhs, id, la;
	unsigned char h[64];

	ge_init_once();
	if (!ge_ready) {
		return 0;
	}
	ge_zero(&id);

	/* Malleability guard. Not needed for the security property this launcher cares about - the
	 * manifest is not a token whose uniqueness matters - but it is one comparison and it costs
	 * nothing.
	 *
	 * It does NOT make this verifier's accept set identical to every other implementation's.
	 * The narrowing is deliberate and is the whole point of the
	 * subgroup check below. The direction is what matters for a code-signing anchor, and the
	 * honest statement of it lives in GXEd25519.h. */
	if (!scalar_lt_L(sig + 32)) {
		return 0;
	}
	if (!ge_decompress(&a, pk)) {
		return 0;
	}

	/* A must lie in the prime-order subgroup; this makes the [h]A shortcut below correct. Against OpenSSL over 2455
	 * cases, a key with a torsion component was the one class where this code disagreed, in both directions.
	 * [L]A == identity gives order dividing L; L is prime, so rejecting the identity separately leaves order exactly L.
	 * Costs one 253-bit scalar multiplication per launch. */
	ge_scalarmult(&la, &a, GROUP_L, 32);
	if (!ge_eq(&la, &id)) {
		return 0;		/* A carries torsion - not a key any real generator produces */
	}
	if (ge_eq(&a, &id)) {
		return 0;		/* the identity as a public key: every message would verify under it */
	}

	if (!ge_decompress(&rpoint, sig)) {
		return 0;
	}
	if (!sha512_3(sig, 32, pk, 32, msg, len, h)) {
		return 0;
	}

	/* [h]A rather than [h mod L]A: the same point, because A was just checked to have order L. That check is load-bearing
	 * once the key becomes data (a second signer, a rotation file, a relay-supplied key). Skipping the reduction avoids a
	 * 512-bit modular reduction, which has no cheap self-check, for 256 extra doublings. */
	ge_scalarmult(&sb, &GE_BASE, sig + 32, 32);
	ge_scalarmult(&ha, &a, h, 64);
	ge_add(&rhs, &ha, &rpoint);
	return ge_eq(&sb, &rhs);
}

/* ============================================================================================
 * SIGNING (see GXEd25519.h for who may use it and why).
 *
 * RFC 8032 section 5.1.6, over the arithmetic above. The two pieces that did not exist are the point
 * encoding and arithmetic modulo L; both are written the plainest way there is - an inverse and a
 * parity bit, and binary long division - because a signer whose output is checked byte for byte against
 * two independent implementations is only as good as the part nobody can eyeball.
 * ========================================================================================== */

/* s = [x/z, y/z] encoded: y little-endian, the parity of x in the top bit. */
static void ge_encode(unsigned char s[32], const ge *p)
{
	fe zi, x, y;
	int i;
	fe_inv(zi, p->Z);
	fe_mul(x, p->X, zi);
	fe_mul(y, p->Y, zi);
	for (i = 0; i < 8; i++) {
		s[4 * i] = (unsigned char)y[i];
		s[4 * i + 1] = (unsigned char)(y[i] >> 8);
		s[4 * i + 2] = (unsigned char)(y[i] >> 16);
		s[4 * i + 3] = (unsigned char)(y[i] >> 24);
	}
	s[31] |= (unsigned char)((x[0] & 1u) << 7);
}

/* out = in mod L, in a little-endian number of `bytes` bytes. Binary long division, most significant bit
 * first: the running remainder stays below 2L < 2^254, so nine 32-bit limbs always hold it. */
static void sc_reduce_bytes(unsigned char out[32], const unsigned char *in, int bytes)
{
	uint32_t r[9] = { 0 }, l[9] = { 0 };
	int i, bit;
	for (i = 0; i < 8; i++) {
		l[i] = (uint32_t)GROUP_L[4 * i] | ((uint32_t)GROUP_L[4 * i + 1] << 8)
			| ((uint32_t)GROUP_L[4 * i + 2] << 16) | ((uint32_t)GROUP_L[4 * i + 3] << 24);
	}
	for (bit = bytes * 8 - 1; bit >= 0; bit--) {
		uint32_t carry = (uint32_t)((in[bit >> 3] >> (bit & 7)) & 1);
		int ge_l = 1;
		for (i = 0; i < 9; i++) {
			const uint32_t next = r[i] >> 31;
			r[i] = (r[i] << 1) | carry;
			carry = next;
		}
		for (i = 8; i >= 0; i--) {
			if (r[i] != l[i]) {
				ge_l = r[i] > l[i];
				break;
			}
		}
		if (ge_l) {
			uint64_t borrow = 0;
			for (i = 0; i < 9; i++) {
				const uint64_t d = (uint64_t)r[i] - l[i] - borrow;
				r[i] = (uint32_t)d;
				borrow = (d >> 63) & 1;
			}
		}
	}
	for (i = 0; i < 8; i++) {
		out[4 * i] = (unsigned char)r[i];
		out[4 * i + 1] = (unsigned char)(r[i] >> 8);
		out[4 * i + 2] = (unsigned char)(r[i] >> 16);
		out[4 * i + 3] = (unsigned char)(r[i] >> 24);
	}
}

/* out = (r + k*a) mod L, all three little-endian 32-byte scalars (a is the clamped secret, below 2^255). */
static void sc_muladd(unsigned char out[32], const unsigned char k[32], const unsigned char a[32],
                      const unsigned char r[32])
{
	uint32_t kl[8], al[8], rl[8];
	uint64_t acc[17] = { 0 };
	unsigned char wide[64];
	int i, j;
	for (i = 0; i < 8; i++) {
		kl[i] = (uint32_t)k[4 * i] | ((uint32_t)k[4 * i + 1] << 8) | ((uint32_t)k[4 * i + 2] << 16) | ((uint32_t)k[4 * i + 3] << 24);
		al[i] = (uint32_t)a[4 * i] | ((uint32_t)a[4 * i + 1] << 8) | ((uint32_t)a[4 * i + 2] << 16) | ((uint32_t)a[4 * i + 3] << 24);
		rl[i] = (uint32_t)r[4 * i] | ((uint32_t)r[4 * i + 1] << 8) | ((uint32_t)r[4 * i + 2] << 16) | ((uint32_t)r[4 * i + 3] << 24);
	}
	/* Column sums with the carry propagated per column, so no 64-bit accumulator can overflow. */
	for (i = 0; i < 8; i++) {
		acc[i] += rl[i];
	}
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			const uint64_t p = (uint64_t)kl[i] * al[j];
			acc[i + j] += p & 0xffffffffu;
			acc[i + j + 1] += p >> 32;
		}
		for (j = 0; j < 16; j++) {
			acc[j + 1] += acc[j] >> 32;
			acc[j] &= 0xffffffffu;
		}
	}
	for (i = 0; i < 16; i++) {
		wide[4 * i] = (unsigned char)acc[i];
		wide[4 * i + 1] = (unsigned char)(acc[i] >> 8);
		wide[4 * i + 2] = (unsigned char)(acc[i] >> 16);
		wide[4 * i + 3] = (unsigned char)(acc[i] >> 24);
	}
	sc_reduce_bytes(out, wide, 64);
}

/* The clamped secret scalar and the nonce prefix, from the seed (RFC 8032 5.1.5). */
static void ed_expand(const unsigned char seed[32], unsigned char a[32], unsigned char prefix[32])
{
	unsigned char h[64];
	gx_sha512(seed, 32, h);
	memcpy(a, h, 32);
	memcpy(prefix, h + 32, 32);
	a[0] &= 248;
	a[31] &= 127;
	a[31] |= 64;
}

int gx_ed25519_public_from_seed(const unsigned char seed[32], unsigned char pk[32])
{
	unsigned char a[32], prefix[32];
	ge p;
	ge_init_once();
	if (!ge_ready) {
		return 0;
	}
	ed_expand(seed, a, prefix);
	ge_scalarmult(&p, &GE_BASE, a, 32);
	ge_encode(pk, &p);
	return 1;
}

int gx_ed25519_sign(unsigned char sig[64], const void *msg, size_t len, const unsigned char seed[32])
{
	unsigned char a[32], prefix[32], pk[32], h[64], r[32], k[32];
	ge p;
	ge_init_once();
	if (!ge_ready) {
		return 0;
	}
	ed_expand(seed, a, prefix);
	ge_scalarmult(&p, &GE_BASE, a, 32);
	ge_encode(pk, &p);

	gx_sha512_3(prefix, 32, msg, len, NULL, 0, h);
	sc_reduce_bytes(r, h, 64);
	ge_scalarmult(&p, &GE_BASE, r, 32);
	ge_encode(sig, &p);

	gx_sha512_3(sig, 32, pk, 32, msg, len, h);
	sc_reduce_bytes(k, h, 64);
	sc_muladd(sig + 32, k, a, r);
	return 1;
}
