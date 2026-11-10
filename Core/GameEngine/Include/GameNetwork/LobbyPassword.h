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

// LobbyPassword.h ////////////////////////////////////////////////////////////
// GeneralsX: Password-protected lobbies.
//
// WHAT IS ON THE WIRE, AND WHY IT IS NOT THE PASSWORD.
//
// The password itself never leaves the machine it was typed on, and the host never holds it
// either. Two derivations do all the work:
//
//   verifier = trunc16( SHA-256( "GXLOBBYPW/1" | salt_be32 | password ) )
//   response = trunc16( SHA-256( "GXLOBBYRS/1" | verifier  | challenge_be32 ) )
//
// The HOST keeps only `salt` and `verifier`. The JOINER is told `salt` and a fresh `challenge`
// in the refusal it gets for knocking without one, recomputes the verifier from the password the
// player typed, and answers with `response`. The host recomputes the same response and compares.
//
// Three properties follow, and they are the reason for the shape:
//
//   * A relay or LAN observer never sees the password. Nor does the host - a host that is later
//     compromised leaks a salted digest, not a credential its players may have reused.
//   * The salt is random per lobby, so the same password produces a different verifier in every
//     game. No precomputed table works against more than one lobby at a time.
//   * The challenge is fresh per refusal and is CONSUMED on the first response that matches it,
//     so a captured join packet cannot be replayed. This is the part a plain salted digest
//     cannot do, and it is why the exchange is two-phase rather than one-shot.
//
// What it does NOT buy, stated plainly: the password space a human picks is small, so an observer
// who captures a salt and a matching response can grind the password offline. Nothing short of a
// PAKE fixes that, and a PAKE is not worth its weight for a game lobby. The guarantee here is
// "not in clear text, not replayable, not reusable across lobbies", not "unbreakable".
//
// SHA-256 rather than the engine's FNV or CRC: both of those are trivially invertible, which
// would let anyone who saw one challenge/response pair forge the next one without ever learning
// the password - i.e. the enforcement would be decorative. Preimage resistance is the only
// property being leaned on here, and it is the one SHA-256 still has unreservedly.

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

// GeneralsX: The compression function and the initial state, exported so
// that a second construction can be built on THIS code rather than on a second copy of SHA-256.
//
// Transport's per-packet MAC needs HMAC, and HMAC done naively re-derives the key's ipad/opad
// blocks and re-compresses them on every packet. Those two blocks are fixed per key, so a caller
// that owns a long-lived key compresses them ONCE at key install and keeps the two resulting
// states. Measured on this box with the exact code below, at the three packet sizes lockstep
// actually produces: 0.645 -> 0.307 us (47 B), 0.874 -> 0.583 us (143 B), 1.592 -> 1.307 us
// (499 B). That is worth an export on the per-packet hot path and worth nothing anywhere else,
// which is why this is two symbols and not a streaming API.
//
// `state` is LOBBYPW_SHA256_STATE_WORDS words and `block` is exactly LOBBYPW_SHA256_BLOCK_BYTES
// bytes; neither length is checked, because this is the inner loop and the only callers are in
// this fork. Length padding and the big-endian output conversion are the CALLER's job here -
// LobbyPasswordSha256 above remains the safe, complete entry point and every existing user of it
// is unaffected.
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
