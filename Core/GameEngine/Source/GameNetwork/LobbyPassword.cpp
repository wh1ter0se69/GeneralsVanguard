/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// LobbyPassword.cpp //////////////////////////////////////////////////////////
// GeneralsX @feature Claude 08/08/2026 See LobbyPassword.h for the protocol and the honest
// statement of what it does and does not protect against.
//
// SHA-256 is implemented here rather than pulled in because nothing in this build ships one:
// Core/Libraries/Source/WWVegas/WWLib/md5.cpp is commented out of its own CMakeLists, and the
// engine's own digests (SimulationId) are FNV-1a, which is invertible and therefore useless for
// a challenge-response. This is FIPS 180-4, no allocation, no dependencies, endian-independent
// because every load and store goes through explicit byte arithmetic.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameNetwork/LobbyPassword.h"

#include <stdint.h>	// uintptr_t, for the stack-address seed material
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------------------------

namespace
{

const UnsignedInt SHA256_K[64] =
{
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

inline UnsignedInt Rotr( UnsignedInt x, UnsignedInt n ) { return (x >> n) | (x << (32u - n)); }

void Sha256Block( UnsignedInt *state, const UnsignedByte *block )
{
	UnsignedInt w[64];
	Int i;

	for (i = 0; i < 16; ++i)
	{
		w[i] = ((UnsignedInt)block[i * 4 + 0] << 24)
			 | ((UnsignedInt)block[i * 4 + 1] << 16)
			 | ((UnsignedInt)block[i * 4 + 2] <<  8)
			 | ((UnsignedInt)block[i * 4 + 3]);
	}
	for (i = 16; i < 64; ++i)
	{
		const UnsignedInt s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const UnsignedInt s1 = Rotr(w[i -  2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i -  2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	UnsignedInt a = state[0], b = state[1], c = state[2], d = state[3];
	UnsignedInt e = state[4], f = state[5], g = state[6], h = state[7];

	for (i = 0; i < 64; ++i)
	{
		const UnsignedInt S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
		const UnsignedInt ch = (e & f) ^ ((~e) & g);
		const UnsignedInt t1 = h + S1 + ch + SHA256_K[i] + w[i];
		const UnsignedInt S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
		const UnsignedInt maj = (a & b) ^ (a & c) ^ (b & c);
		const UnsignedInt t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // anonymous namespace

// GeneralsX @feature Claude 08/08/2026 The initial state and the compression function, exported.
// See the note in LobbyPassword.h for who wants them and why. ONE copy of the constants: the
// one-shot below now reads this array rather than repeating it, so the two entry points cannot
// drift apart.
const UnsignedInt LOBBYPW_SHA256_IV[LOBBYPW_SHA256_STATE_WORDS] =
{
	0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
	0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
};

void LobbyPasswordSha256Block( UnsignedInt *state, const UnsignedByte *block )
{
	Sha256Block(state, block);
}

void LobbyPasswordSha256( const void *data, UnsignedInt len, UnsignedByte *out32 )
{
	UnsignedInt state[8];
	memcpy(state, LOBBYPW_SHA256_IV, sizeof(state));

	const UnsignedByte *p = (const UnsignedByte *)data;
	UnsignedInt remaining = len;

	while (remaining >= 64u)
	{
		Sha256Block(state, p);
		p += 64;
		remaining -= 64u;
	}

	// Final block(s): the tail, 0x80, zero padding, then the 64-bit big-endian bit count.
	UnsignedByte tail[128];
	memset(tail, 0, sizeof(tail));
	memcpy(tail, p, remaining);
	tail[remaining] = 0x80;

	const UnsignedInt tailLen = (remaining < 56u) ? 64u : 128u;
	const UnsignedInt64 bits = (UnsignedInt64)len * 8u;
	for (Int i = 0; i < 8; ++i)
		tail[tailLen - 1 - i] = (UnsignedByte)((bits >> (8 * i)) & 0xFFu);

	Sha256Block(state, tail);
	if (tailLen == 128u)
		Sha256Block(state, tail + 64);

	for (Int i = 0; i < 8; ++i)
	{
		out32[i * 4 + 0] = (UnsignedByte)((state[i] >> 24) & 0xFFu);
		out32[i * 4 + 1] = (UnsignedByte)((state[i] >> 16) & 0xFFu);
		out32[i * 4 + 2] = (UnsignedByte)((state[i] >>  8) & 0xFFu);
		out32[i * 4 + 3] = (UnsignedByte)( state[i]        & 0xFFu);
	}
}

// ---------------------------------------------------------------------------------------------
// Derivations
// ---------------------------------------------------------------------------------------------

namespace
{

/// Big-endian so the four client platforms agree without anyone having to think about it - the
/// same reason the relay header is big-endian (see Transport::writeRelayHeader).
void PutBE32( UnsignedByte *dest, UnsignedInt v )
{
	dest[0] = (UnsignedByte)((v >> 24) & 0xFFu);
	dest[1] = (UnsignedByte)((v >> 16) & 0xFFu);
	dest[2] = (UnsignedByte)((v >>  8) & 0xFFu);
	dest[3] = (UnsignedByte)( v        & 0xFFu);
}

/// Longest password we will hash. Anything past this is truncated rather than refused, because a
/// silent truncation on ONE side would be a mismatch nobody could diagnose - both sides run this
/// same function on the same input, so both truncate identically.
enum { LOBBYPW_MAX_PASSWORD = 64 };

} // anonymous namespace

void LobbyPasswordVerifier( UnsignedInt salt, const char *password, UnsignedByte *outDigest )
{
	static const char domain[] = "GXLOBBYPW/1";

	UnsignedByte buf[sizeof(domain) - 1 + 4 + LOBBYPW_MAX_PASSWORD];
	UnsignedInt n = 0;

	memcpy(buf + n, domain, sizeof(domain) - 1);
	n += (UnsignedInt)(sizeof(domain) - 1);

	PutBE32(buf + n, salt);
	n += 4;

	UnsignedInt pwLen = 0;
	if (password != nullptr)
	{
		while (pwLen < (UnsignedInt)LOBBYPW_MAX_PASSWORD && password[pwLen] != '\0')
			++pwLen;
		memcpy(buf + n, password, pwLen);
	}
	n += pwLen;

	UnsignedByte full[32];
	LobbyPasswordSha256(buf, n, full);
	memcpy(outDigest, full, LOBBYPW_DIGEST_BYTES);

	// The password was copied into this frame; do not leave it there for whatever runs next.
	memset(buf, 0, sizeof(buf));
}

void LobbyPasswordResponse( const UnsignedByte *verifier, UnsignedInt challenge, UnsignedByte *outDigest )
{
	static const char domain[] = "GXLOBBYRS/1";

	UnsignedByte buf[sizeof(domain) - 1 + LOBBYPW_DIGEST_BYTES + 4];
	UnsignedInt n = 0;

	memcpy(buf + n, domain, sizeof(domain) - 1);
	n += (UnsignedInt)(sizeof(domain) - 1);

	memcpy(buf + n, verifier, LOBBYPW_DIGEST_BYTES);
	n += (UnsignedInt)LOBBYPW_DIGEST_BYTES;

	PutBE32(buf + n, challenge);
	n += 4;

	UnsignedByte full[32];
	LobbyPasswordSha256(buf, n, full);
	memcpy(outDigest, full, LOBBYPW_DIGEST_BYTES);
}

Bool LobbyPasswordEqual( const UnsignedByte *a, const UnsignedByte *b )
{
	UnsignedInt diff = 0;
	for (Int i = 0; i < LOBBYPW_DIGEST_BYTES; ++i)
		diff |= (UnsignedInt)(a[i] ^ b[i]);
	return (diff == 0);
}

// ---------------------------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------------------------

UnsignedInt LobbyPasswordRandom( void )
{
	// Chained: each call hashes the previous 32-byte state together with fresh environmental
	// input, so knowing one output does not give you the next even though the environmental
	// inputs are individually poor. Zero-initialised on first use, which is fine - the seed
	// material below is what makes the first output unpredictable, not the starting state.
	static UnsignedByte s_state[32] = { 0 };
	static UnsignedInt s_counter = 0;

	++s_counter;

	struct SeedMaterial
	{
		UnsignedByte prev[32];
		UnsignedInt counter;
		UnsignedInt wallClock;
		UnsignedInt processorClock;
		UnsignedInt64 stackAddress;
	} seed;

	memcpy(seed.prev, s_state, sizeof(seed.prev));
	seed.counter = s_counter;
	seed.wallClock = (UnsignedInt)time(nullptr);
	seed.processorClock = (UnsignedInt)clock();
	seed.stackAddress = (UnsignedInt64)(uintptr_t)&seed;

	LobbyPasswordSha256(&seed, (UnsignedInt)sizeof(seed), s_state);

	const UnsignedInt out = ((UnsignedInt)s_state[0] << 24)
						  | ((UnsignedInt)s_state[1] << 16)
						  | ((UnsignedInt)s_state[2] <<  8)
						  | ((UnsignedInt)s_state[3]);

	// 0 is the "no challenge" sentinel everywhere this is used; never hand it out as a value.
	return (out == 0) ? 1u : out;
}
