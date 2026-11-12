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

// Password-protected lobbies. The password never leaves the typing machine; the host keeps only salt and verifier:
//   verifier = trunc16( SHA-256( "GXLOBBYPW/1" | salt_be32 | password ) )
//   response = trunc16( SHA-256( "GXLOBBYRS/1" | verifier | challenge_be32 ) )
// The challenge is consumed on first match, so a captured join cannot be replayed. SHA-256, not invertible FNV or CRC.

#pragma once

// BaseTypeCore.h rather than BaseType.h: this header is pulled into LANAPI.h, and everything
// needed here (Int/UnsignedInt/UnsignedByte/Bool) is in the core file. The full BaseType.h drags
// in Lib/trig.h, which is one of this fork's documented simulation-math divergences and has no
// business being reachable from a lobby credential header.
#include "Lib/BaseTypeCore.h"

/// Bytes of SHA-256 output kept for a verifier or a response. 128 bits is far past what a lobby
/// needs and keeps the appended wire fields inside the union's existing dead space.
enum { LOBBYPW_DIGEST_BYTES = 16 };

/// How many outstanding challenges a host remembers. One per joiner knocking at the same moment;
/// eight is more than a lobby can seat.
enum { LOBBYPW_CHALLENGE_RING = 8 };

/// A challenge older than this is refused even if it is still in the ring, so a packet captured
/// and held cannot be used later. Milliseconds.
enum { LOBBYPW_CHALLENGE_LIFETIME_MS = 120000 };

/// Raw SHA-256. `out` receives 32 bytes.
void LobbyPasswordSha256( const void *data, UnsignedInt len, UnsignedByte *out32 );

// The compression function and initial state, exported so the per-packet HMAC in Transport can reuse this code:
// a caller with a long-lived key compresses the fixed ipad/opad blocks once and keeps the states.
// state is LOBBYPW_SHA256_STATE_WORDS words, block exactly LOBBYPW_SHA256_BLOCK_BYTES bytes; neither is checked.
// Padding and big-endian output are the caller job; LobbyPasswordSha256 is the safe entry point.
enum { LOBBYPW_SHA256_BLOCK_BYTES = 64, LOBBYPW_SHA256_STATE_WORDS = 8 };
extern const UnsignedInt LOBBYPW_SHA256_IV[LOBBYPW_SHA256_STATE_WORDS];
void LobbyPasswordSha256Block( UnsignedInt *state, const UnsignedByte *block );

/// verifier = trunc16( SHA-256( "GXLOBBYPW/1" | salt_be32 | password ) ).
/// `password` is NUL terminated and may be empty; an empty password still produces a verifier,
/// so "the password is the empty string" and "there is no password" stay distinguishable at the
/// caller rather than here.
void LobbyPasswordVerifier( UnsignedInt salt, const char *password, UnsignedByte *outDigest );

/// response = trunc16( SHA-256( "GXLOBBYRS/1" | verifier | challenge_be32 ) ).
void LobbyPasswordResponse( const UnsignedByte *verifier, UnsignedInt challenge, UnsignedByte *outDigest );

/// Constant-time compare of two LOBBYPW_DIGEST_BYTES buffers. Constant time is not load-bearing
/// against a network attacker at this scale, but a variable-time memcmp on a secret is the kind
/// of thing that gets copied into somewhere it does matter.
Bool LobbyPasswordEqual( const UnsignedByte *a, const UnsignedByte *b );

/// A non-simulation random 32-bit value for salts and challenges.
///
/// DELIBERATELY NOT GameLogicRandomValue: that generator is part of the lockstep simulation and
/// drawing from it out of a lobby handler would desync every peer. This is a SHA-256 chained
/// state seeded from wall clock, a monotonically increasing counter and a stack address, which is
/// not a CSPRNG and is not claimed to be one - it only has to make a salt unguessable in advance
/// and a challenge non-repeating.
UnsignedInt LobbyPasswordRandom( void );
