/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: AN ADMIN'S CHEAT ORDER IS SIGNED BY THE ADMIN, so no other peer can send one
// in the admin's name.
//
// WHY A SIGNATURE AND NOT THE ORDER'S SENDER FIELD. A network order names its sender in a packet field
// (NetCommandMsg::getPlayerID) that is not bound to the peer the datagram came from: the packet router relays
// everybody's orders and a frame resend carries a third peer's, and command envelopes are not authenticated
// per hop (a known limit of the inherited lockstep design). So "the admin's slot sent it" is not evidence.
// A signature is: each
// relay-proven admin holds a per-match Ed25519 key (GameClient/GXAdminKey.h), the relay publishes the public half
// for live admins only (relay.js GXAKR), every peer confirms the host stamped exactly that key at the start
// (LANAPI::OnGameStart), and the simulation accepts an admin order only with a valid signature under the stamped key
// for the sender's slot and a sequence number above the last one it accepted from that slot. Whoever relays, resends
// or forges the envelope, nobody without the private half can make an order the simulation accepts.
//
// WHAT IS SIGNED - the payload below, byte for byte, over the order's LOGICAL arguments (the trailer excluded):
//     "GXAO1" | slot u8 | seq u32 | seed u32 | type u32 | argc u32 | per argument: tag u8 + value
// little-endian throughout; tags 'I' int32, 'B' bool (1 byte), 'R' float32 bits, 'L' three float32 bits. Any other
// argument type makes the order unsignable, and so unacceptable - no cheat uses one. `seed` is the match's random
// seed and `seq` a per-slot counter, so an order signed for one match or one moment is refused in any other.
//
// THE TRAILER is 18 INTEGER arguments appended after the logical ones: seq, the 64-byte signature as 16
// little-endian words, then GX_ADMIN_ORDER_MAGIC last. Its presence is recognised by that last word alone, so an
// order without one keeps meaning exactly what it did (the one-human sandbox never signs).
//
// INSIDE THE DIGEST (Common/), header only, and standalone: scripts/test/admin-order-test.sh compiles it with
// nothing but GXEd25519.cpp and GXSha512.cpp. The GameMessage adapters live in the engine files that use them.

#pragma once

#include "GameClient/GXEd25519.h"

#include <string.h>

enum
{
	GX_ADMIN_ORDER_MAGIC   = 0x4F415847,	///< "GXAO", the trailer's last word
	GX_ADMIN_TRAILER_ARGS  = 18,			///< seq + 16 signature words + magic
	GX_ADMIN_PAYLOAD_MAX   = 512,
	GX_ADMIN_MAX_PER_MATCH = 4,				///< admins a start can stamp (LANMessage GameStart has room for this many keys)
};

struct GXAdminPayload
{
	unsigned char b[GX_ADMIN_PAYLOAD_MAX];
	int len;
	bool ok;
};

inline void gxAdminPayloadU32( GXAdminPayload *p, unsigned int v )
{
	if( !p->ok || p->len + 4 > GX_ADMIN_PAYLOAD_MAX ) { p->ok = false; return; }
	p->b[p->len++] = (unsigned char)v;
	p->b[p->len++] = (unsigned char)( v >> 8 );
	p->b[p->len++] = (unsigned char)( v >> 16 );
	p->b[p->len++] = (unsigned char)( v >> 24 );
}

inline void gxAdminPayloadU8( GXAdminPayload *p, unsigned char v )
{
	if( !p->ok || p->len + 1 > GX_ADMIN_PAYLOAD_MAX ) { p->ok = false; return; }
	p->b[p->len++] = v;
}

inline void gxAdminPayloadBegin( GXAdminPayload *p, int slot, unsigned int seq, unsigned int seed, unsigned int type,
                                 unsigned int argc )
{
	memcpy( p->b, "GXAO1", 5 );
	p->len = 5;
	p->ok = ( slot >= 0 && slot < 256 );
	gxAdminPayloadU8( p, (unsigned char)slot );
	gxAdminPayloadU32( p, seq );
	gxAdminPayloadU32( p, seed );
	gxAdminPayloadU32( p, type );
	gxAdminPayloadU32( p, argc );
}

inline void gxAdminPayloadInt( GXAdminPayload *p, int v )		{ gxAdminPayloadU8( p, 'I' ); gxAdminPayloadU32( p, (unsigned int)v ); }
inline void gxAdminPayloadBool( GXAdminPayload *p, bool v )		{ gxAdminPayloadU8( p, 'B' ); gxAdminPayloadU8( p, v ? 1 : 0 ); }
inline unsigned int gxAdminFloatBits( float f )					{ unsigned int u; memcpy( &u, &f, 4 ); return u; }
inline void gxAdminPayloadReal( GXAdminPayload *p, float v )	{ gxAdminPayloadU8( p, 'R' ); gxAdminPayloadU32( p, gxAdminFloatBits( v ) ); }
inline void gxAdminPayloadLoc( GXAdminPayload *p, float x, float y, float z )
{
	gxAdminPayloadU8( p, 'L' );
	gxAdminPayloadU32( p, gxAdminFloatBits( x ) );
	gxAdminPayloadU32( p, gxAdminFloatBits( y ) );
	gxAdminPayloadU32( p, gxAdminFloatBits( z ) );
}
inline void gxAdminPayloadUnsupported( GXAdminPayload *p )		{ p->ok = false; }

inline void gxAdminSigToWords( const unsigned char sig[64], int words[16] )
{
	for( int i = 0; i < 16; ++i )
		words[i] = (int)( (unsigned int)sig[4 * i] | ( (unsigned int)sig[4 * i + 1] << 8 )
			| ( (unsigned int)sig[4 * i + 2] << 16 ) | ( (unsigned int)sig[4 * i + 3] << 24 ) );
}

inline void gxAdminWordsToSig( const int words[16], unsigned char sig[64] )
{
	for( int i = 0; i < 16; ++i )
	{
		const unsigned int w = (unsigned int)words[i];
		sig[4 * i] = (unsigned char)w;
		sig[4 * i + 1] = (unsigned char)( w >> 8 );
		sig[4 * i + 2] = (unsigned char)( w >> 16 );
		sig[4 * i + 3] = (unsigned char)( w >> 24 );
	}
}

/// THE decision, the same on every peer and in a replay: may an admin order with this payload, signature and seq
/// run, given the key stamped for its slot and the last seq accepted from that slot? Pure; the caller advances
/// lastSeq on true. An all-zero key is no key (a slot the start did not stamp).
inline bool gxAdminOrderDecide( const GXAdminPayload *p, const unsigned char pk[32], const unsigned char sig[64],
                                unsigned int seq, unsigned int lastSeq )
{
	if( p == NULL || !p->ok || pk == NULL || sig == NULL || seq <= lastSeq )
		return false;
	unsigned char any = 0;
	for( int i = 0; i < 32; ++i )
		any |= pk[i];
	if( any == 0 )
		return false;
	return gx_ed25519_verify( pk, sig, p->b, (size_t)p->len ) == 1;
}

/// base64url (RFC 4648 section 5, no padding) of 32 bytes: 43 characters. The relay's GXAKEY / GXAKR form and the
/// replay header's. out must hold 44 bytes.
inline void gxAdminKeyToB64( const unsigned char key[32], char out[44] )
{
	static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	int o = 0;
	unsigned int acc = 0;
	int bits = 0;
	for( int i = 0; i < 32; ++i )
	{
		acc = ( acc << 8 ) | key[i];
		bits += 8;
		while( bits >= 6 )
		{
			bits -= 6;
			out[o++] = A[( acc >> bits ) & 63];
		}
	}
	if( bits > 0 )
		out[o++] = A[( acc << ( 6 - bits ) ) & 63];
	out[o] = '\0';
}

/// The inverse, CANONICAL ONLY: exactly 43 characters of the alphabet, and the last one carrying two zero bits -
/// relay.js BOT_PUB_RE's rule - so one key has exactly one spelling and a comparison of spellings is one of keys.
inline bool gxAdminKeyFromB64( const char *in, int inLen, unsigned char key[32] )
{
	if( in == NULL || inLen != 43 )
		return false;
	unsigned int acc = 0;
	int bits = 0, o = 0;
	for( int i = 0; i < 43; ++i )
	{
		const char c = in[i];
		int v;
		if( c >= 'A' && c <= 'Z' ) v = c - 'A';
		else if( c >= 'a' && c <= 'z' ) v = c - 'a' + 26;
		else if( c >= '0' && c <= '9' ) v = c - '0' + 52;
		else if( c == '-' ) v = 62;
		else if( c == '_' ) v = 63;
		else return false;
		if( i == 42 && ( v & 3 ) != 0 )
			return false;
		acc = ( acc << 6 ) | (unsigned int)v;
		bits += 6;
		if( bits >= 8 )
		{
			bits -= 8;
			if( o < 32 )
				key[o++] = (unsigned char)( acc >> bits );
		}
	}
	return o == 32;
}
