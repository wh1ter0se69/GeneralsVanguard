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

// GeneralsX @feature Claude 11/08/2026 SHA-512, FIPS 180-4 section 6.4. See the header for why this
// is written rather than called.
//
// This is the specification transcribed, with no cleverness anywhere on purpose: eight 64-bit
// working variables, the eighty published round constants, a big-endian message schedule, and a
// streaming context so the three-part entry point never has to concatenate.
//
// DELIBERATELY NOT INCLUDING PreRTS.h. This file must compile standing alone, outside the engine and
// outside cmake, so that scripts/test/identity-assert-test.cpp can drive it with nothing but clang.
// A crypto primitive whose only test requires a full engine build is a primitive that stops being
// tested the first time the engine build breaks. Everything here is <stddef.h> and <string.h>.

#include "GameClient/GXSha512.h"

#include <string.h>

namespace
{

// The type is spelled out rather than taken from <stdint.h>'s uint64_t so that this file has no
// opinion about which headers the engine's precompiled header has already pulled in. `unsigned long
// long` is at least 64 bits on every compiler this project targets, and the masking below makes the
// arithmetic exact even if it were wider.
typedef unsigned long long u64;

const u64 M64 = 0xFFFFFFFFFFFFFFFFull;

// FIPS 180-4 section 4.2.3. The first sixty-four bits of the fractional parts of the cube roots of
// the first eighty primes.
const u64 K[80] = {
	0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
	0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
	0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
	0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
	0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
	0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
	0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
	0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
	0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
	0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
	0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
	0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
	0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
	0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
	0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
	0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
	0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
	0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
	0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
	0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

// FIPS 180-4 section 5.3.5. The first sixty-four bits of the fractional parts of the square roots
// of the first eight primes.
const u64 IV[8] = {
	0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
	0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull
};

inline u64 rotr(u64 x, unsigned n) { return ((x >> n) | (x << (64 - n))) & M64; }

inline u64 Ch(u64 x, u64 y, u64 z)  { return (x & y) ^ ((~x & M64) & z); }
inline u64 Maj(u64 x, u64 y, u64 z) { return (x & y) ^ (x & z) ^ (y & z); }

inline u64 BigS0(u64 x)   { return rotr(x, 28) ^ rotr(x, 34) ^ rotr(x, 39); }
inline u64 BigS1(u64 x)   { return rotr(x, 14) ^ rotr(x, 18) ^ rotr(x, 41); }
inline u64 SmallS0(u64 x) { return rotr(x, 1)  ^ rotr(x, 8)  ^ (x >> 7); }
inline u64 SmallS1(u64 x) { return rotr(x, 19) ^ rotr(x, 61) ^ (x >> 6); }

struct Ctx
{
	u64           h[8];
	unsigned char buf[128];
	size_t        used;	// bytes currently in buf, always < 128 between calls
	u64           bits;	// message length in BITS, low 64 only - see finish()
};

void compress(u64 h[8], const unsigned char block[128])
{
	u64 w[80];
	int i;

	for (i = 0; i < 16; ++i)
	{
		const unsigned char *p = block + i * 8;
		w[i] = ((u64)p[0] << 56) | ((u64)p[1] << 48) | ((u64)p[2] << 40) | ((u64)p[3] << 32)
		     | ((u64)p[4] << 24) | ((u64)p[5] << 16) | ((u64)p[6] << 8)  | ((u64)p[7]);
	}
	for (i = 16; i < 80; ++i)
		w[i] = (SmallS1(w[i - 2]) + w[i - 7] + SmallS0(w[i - 15]) + w[i - 16]) & M64;

	u64 a = h[0], b = h[1], c = h[2], d = h[3];
	u64 e = h[4], f = h[5], g = h[6], hh = h[7];

	for (i = 0; i < 80; ++i)
	{
		const u64 t1 = (hh + BigS1(e) + Ch(e, f, g) + K[i] + w[i]) & M64;
		const u64 t2 = (BigS0(a) + Maj(a, b, c)) & M64;
		hh = g;
		g  = f;
		f  = e;
		e  = (d + t1) & M64;
		d  = c;
		c  = b;
		b  = a;
		a  = (t1 + t2) & M64;
	}

	h[0] = (h[0] + a)  & M64;
	h[1] = (h[1] + b)  & M64;
	h[2] = (h[2] + c)  & M64;
	h[3] = (h[3] + d)  & M64;
	h[4] = (h[4] + e)  & M64;
	h[5] = (h[5] + f)  & M64;
	h[6] = (h[6] + g)  & M64;
	h[7] = (h[7] + hh) & M64;

	// The schedule is derived entirely from the block and the block is public, so there is nothing
	// secret to scrub. Said here so that its absence reads as a decision rather than an omission.
}

void init(Ctx *c)
{
	memcpy(c->h, IV, sizeof(c->h));
	c->used = 0;
	c->bits = 0;
}

void update(Ctx *c, const void *data, size_t len)
{
	// A NULL pointer with a non-zero length is a caller bug; a NULL pointer with zero length is the
	// documented way to say "this part is empty" and must not reach memcpy.
	if (data == 0 || len == 0)
		return;

	const unsigned char *p = (const unsigned char *)data;
	c->bits = (c->bits + ((u64)len << 3)) & M64;

	if (c->used != 0)
	{
		size_t want = 128 - c->used;
		if (want > len)
			want = len;
		memcpy(c->buf + c->used, p, want);
		c->used += want;
		p       += want;
		len     -= want;
		if (c->used == 128)
		{
			compress(c->h, c->buf);
			c->used = 0;
		}
	}

	while (len >= 128)
	{
		compress(c->h, p);
		p   += 128;
		len -= 128;
	}

	if (len != 0)
	{
		memcpy(c->buf, p, len);
		c->used = len;
	}
}

void finish(Ctx *c, unsigned char out[64])
{
	// FIPS 180-4 section 5.1.2: append 0x80, then zeroes, then the message length as a 128-bit
	// big-endian integer. The high 64 bits are WRITTEN rather than assumed absent - they are always
	// zero here because `bits` is 64-bit and the engine will never hash 2^61 bytes, but writing the
	// field means the pad below is literally the standard's and not a specialisation of it that
	// someone later has to re-derive.
	const u64 bits = c->bits;

	unsigned char pad[1] = { 0x80 };
	update(c, pad, 1);
	c->bits = bits;		// update() just counted the pad byte; the length field must not include it

	static const unsigned char ZERO[128] = { 0 };
	while (c->used != 112)
	{
		const size_t want = (c->used < 112) ? (112 - c->used) : (128 - c->used);
		update(c, ZERO, want);
		c->bits = bits;
	}

	unsigned char len[16];
	memset(len, 0, 8);					// the high 64 bits of the 128-bit length
	for (int i = 0; i < 8; ++i)
		len[8 + i] = (unsigned char)((bits >> (56 - 8 * i)) & 0xFF);
	update(c, len, 16);

	for (int i = 0; i < 8; ++i)
	{
		out[i * 8 + 0] = (unsigned char)((c->h[i] >> 56) & 0xFF);
		out[i * 8 + 1] = (unsigned char)((c->h[i] >> 48) & 0xFF);
		out[i * 8 + 2] = (unsigned char)((c->h[i] >> 40) & 0xFF);
		out[i * 8 + 3] = (unsigned char)((c->h[i] >> 32) & 0xFF);
		out[i * 8 + 4] = (unsigned char)((c->h[i] >> 24) & 0xFF);
		out[i * 8 + 5] = (unsigned char)((c->h[i] >> 16) & 0xFF);
		out[i * 8 + 6] = (unsigned char)((c->h[i] >>  8) & 0xFF);
		out[i * 8 + 7] = (unsigned char)((c->h[i]      ) & 0xFF);
	}
}

} // anonymous namespace

extern "C" void gx_sha512_3(const void *a, size_t alen, const void *b, size_t blen,
                            const void *c, size_t clen, unsigned char out[64])
{
	Ctx ctx;
	init(&ctx);
	update(&ctx, a, alen);
	update(&ctx, b, blen);
	update(&ctx, c, clen);
	finish(&ctx, out);
}

extern "C" void gx_sha512(const void *msg, size_t len, unsigned char out[64])
{
	gx_sha512_3(msg, len, 0, 0, 0, 0, out);
}
