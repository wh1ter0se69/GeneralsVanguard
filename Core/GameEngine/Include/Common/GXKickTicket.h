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

// In-match kick ticket: a moderator kick signed with GX_KICK_NOTICE_SEED.
// Wire: M = gxkick1|room|vip|n or a|@handle or empty|iat (ASCII); bytes = M || Ed25519(M), last 64 bytes the signature.
// Changes no simulation state: it only decides whether the victim surrenders and each peer draws a line.
// In Common so builds with different keys or checks refuse each other at join. Test: scripts/test/kick-ticket-test.cpp.

#pragma once

#include "GameClient/GXEd25519.h"

#include <stdlib.h>
#include <string.h>

/* Ed25519 public key of GX_KICK_NOTICE_SEED. All zeroes is the unpinned sentinel (gxKickPubkeyPinned checks it):
 * with no key no ticket verifies, so nobody surrenders and no line is drawn; the relay still removes the player.
 * Mint with scripts/tooling/mint-identity-key.py using its own GX_MANIFEST_KEY_DIR (a separate key). */
static const unsigned char GX_KICK_PUBKEY[32] = {
	0xce, 0x30, 0x20, 0xb6, 0x6a, 0xc6, 0x95, 0x0c,
	0xbe, 0x7f, 0xe9, 0x93, 0x60, 0x69, 0xc2, 0xf3,
	0x5b, 0x17, 0xf5, 0xd4, 0x61, 0x8f, 0x33, 0x49,
	0xb1, 0xe0, 0x42, 0x37, 0xaf, 0xf0, 0x9b, 0x72,
};

enum
{
	GX_KICK_SIG_LEN = 64,
	GX_KICK_MSG_MAX = 160,							///< M: 8 + 31 + 15 + 1 + 33 + 10 + 5 separators = 103
	GX_KICK_BYTES_MAX = GX_KICK_MSG_MAX + GX_KICK_SIG_LEN,
	GX_KICK_B64_MAX = 400,							///< the relay's /^[A-Za-z0-9+/=]{1,400}$/
	GX_KICK_ROOM_MAX = 31,							///< Transport's s_assignedRoom[32]
	GX_KICK_LABEL_MAX = 33,							///< "@" + a Discord username, 2-32 chars
	GX_KICK_EARLY_S = 120,							///< clock skew allowed before iat, both uses
	GX_KICK_VICTIM_LATE_S = 120,					///< the victim acts on a ticket at most this old
	/// A peer draws the line for a ticket at most this old. Covers the slow path - the relay's 20 s backstop,
	/// then the stock disconnect wait - and stays inside the relay's RECLAIM_GRACE_MS (5 min by default),
	/// which is what keeps the kicked vip in that room from being handed to anybody else while a harvested
	/// copy of this ticket would still draw.
	GX_KICK_SHOW_LATE_S = 300,
};

enum GXKickVerdict
{
	GXKICK_OK = 0,
	GXKICK_MALFORMED,		///< not a ticket: length, magic, a field, or a character out of its set
	GXKICK_UNPINNED,		///< this build has no key
	GXKICK_BADSIG,			///< not signed by GX_KICK_NOTICE_SEED
	GXKICK_VIP,				///< signed for somebody else
	GXKICK_ROOM,			///< signed for another room
	GXKICK_CLOCK,			///< outside the window on this machine's clock
};

inline const char *gxKickVerdictName( GXKickVerdict v )
{
	switch( v )
	{
		case GXKICK_OK:			return "ok";
		case GXKICK_MALFORMED:	return "malformed";
		case GXKICK_UNPINNED:	return "unpinned";
		case GXKICK_BADSIG:		return "badsig";
		case GXKICK_VIP:		return "vip";
		case GXKICK_ROOM:		return "room";
		case GXKICK_CLOCK:		return "clock";
	}
	return "?";
}

struct GXKickTicket
{
	char room[GX_KICK_ROOM_MAX + 1];
	unsigned int vip;								///< host order, a<<24|b<<16|c<<8|d, as Transport and GameSlot hold it
	int anonymous;
	char label[GX_KICK_LABEL_MAX + 1];			///< "@handle", or "" when anonymous
	long long iat;
};

inline int gxKickHexNibble( char c )
{
	if( c >= '0' && c <= '9' ) return c - '0';
	if( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/// The key tickets are checked against. A GX_TEST_INSTRUMENTS build (never shipped, cmake/config-build.cmake)
/// takes GX_TEST_KICK_PUBKEY - 64 hex chars - instead, so the harness can sign with a throwaway key.
inline const unsigned char *gxKickPubkey()
{
#if defined(GX_TEST_INSTRUMENTS)
	static unsigned char s_test[32];
	static int s_state = -1;					// -1 unread, 0 none, 1 set
	if( s_state < 0 )
	{
		s_state = 0;
		const char *hex = getenv( "GX_TEST_KICK_PUBKEY" );
		if( hex != NULL && strlen( hex ) == 64 )
		{
			s_state = 1;
			for( int i = 0; i < 32; ++i )
			{
				const int hi = gxKickHexNibble( hex[2 * i] ), lo = gxKickHexNibble( hex[2 * i + 1] );
				if( hi < 0 || lo < 0 ) { s_state = 0; break; }
				s_test[i] = (unsigned char)( ( hi << 4 ) | lo );
			}
		}
	}
	if( s_state == 1 )
		return s_test;
#endif
	return GX_KICK_PUBKEY;
}

inline bool gxKickPubkeyPinned( const unsigned char *pk )
{
	for( int i = 0; i < 32; ++i )
		if( pk[i] != 0 )
			return true;
	return false;
}

/// Standard base64, padding required. Returns the decoded length, or -1 for anything that is not exactly that.
inline int gxKickB64Decode( const char *in, int inLen, unsigned char *out, int outMax )
{
	if( in == NULL || inLen <= 0 || ( inLen % 4 ) != 0 )
		return -1;
	int n = 0;
	for( int i = 0; i < inLen; i += 4 )
	{
		int v[4];
		int pad = 0;
		for( int k = 0; k < 4; ++k )
		{
			const char c = in[i + k];
			if( c >= 'A' && c <= 'Z' ) v[k] = c - 'A';
			else if( c >= 'a' && c <= 'z' ) v[k] = c - 'a' + 26;
			else if( c >= '0' && c <= '9' ) v[k] = c - '0' + 52;
			else if( c == '+' ) v[k] = 62;
			else if( c == '/' ) v[k] = 63;
			else if( c == '=' && i + 4 == inLen && k >= 2 ) { v[k] = 0; ++pad; }
			else return -1;
			if( pad > 0 && c != '=' )
				return -1;						// data after padding
		}
		const unsigned int w = ( (unsigned int)v[0] << 18 ) | ( (unsigned int)v[1] << 12 ) | ( (unsigned int)v[2] << 6 ) | (unsigned int)v[3];
		const int take = 3 - pad;
		if( n + take > outMax )
			return -1;
		out[n++] = (unsigned char)( w >> 16 );
		if( take > 1 ) out[n++] = (unsigned char)( w >> 8 );
		if( take > 2 ) out[n++] = (unsigned char)w;
	}
	return n;
}

/// Split M (not NUL-terminated) into its six fields and check each against its character set. No signature.
inline bool gxKickParseFields( const unsigned char *m, int len, GXKickTicket *out )
{
	static const char MAGIC[] = "gxkick1|";
	const int magicLen = (int)sizeof( MAGIC ) - 1;
	if( len <= magicLen || len > GX_KICK_MSG_MAX || memcmp( m, MAGIC, magicLen ) != 0 )
		return false;
	memset( out, 0, sizeof( *out ) );

	// Fields 1..5 after the magic: room, vip, flag, label, iat.
	const unsigned char *f[5];
	int flen[5];
	int field = 0;
	int start = magicLen;
	for( int i = magicLen; i <= len; ++i )
	{
		if( i == len || m[i] == '|' )
		{
			if( field >= 5 )
				return false;
			f[field] = m + start;
			flen[field] = i - start;
			++field;
			start = i + 1;
		}
	}
	if( field != 5 )
		return false;

	// room: the relay's TOKEN_RE, [A-Za-z0-9._-], 1..31
	if( flen[0] < 1 || flen[0] > GX_KICK_ROOM_MAX )
		return false;
	for( int i = 0; i < flen[0]; ++i )
	{
		const char c = (char)f[0][i];
		if( !( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '.' || c == '_' || c == '-' ) )
			return false;
		out->room[i] = c;
	}

	// vip: a dotted quad, no leading zeros beyond a lone 0
	{
		unsigned int ip = 0;
		int octets = 0, digits = 0, value = 0;
		for( int i = 0; i <= flen[1]; ++i )
		{
			const char c = ( i == flen[1] ) ? '.' : (char)f[1][i];
			if( c == '.' )
			{
				if( digits == 0 || ( digits > 1 && f[1][i - digits] == '0' ) || value > 255 || octets >= 4 )
					return false;
				ip = ( ip << 8 ) | (unsigned int)value;
				++octets;
				digits = 0;
				value = 0;
			}
			else if( c >= '0' && c <= '9' && digits < 3 )
			{
				value = value * 10 + ( c - '0' );
				++digits;
			}
			else
				return false;
		}
		if( octets != 4 )
			return false;
		out->vip = ip;
	}

	// flag and label: 'n' + "@" + [a-z0-9_.]{2,32}, or 'a' + empty
	if( flen[2] != 1 )
		return false;
	if( f[2][0] == 'a' )
	{
		if( flen[3] != 0 )
			return false;
		out->anonymous = 1;
	}
	else if( f[2][0] == 'n' )
	{
		if( flen[3] < 3 || flen[3] > GX_KICK_LABEL_MAX || f[3][0] != '@' )
			return false;
		for( int i = 1; i < flen[3]; ++i )
		{
			const char c = (char)f[3][i];
			if( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '.' ) )
				return false;
		}
		memcpy( out->label, f[3], flen[3] );
		out->anonymous = 0;
	}
	else
		return false;

	// iat: 1..10 digits, no leading zero
	if( flen[4] < 1 || flen[4] > 10 || f[4][0] == '0' )
		return false;
	long long iat = 0;
	for( int i = 0; i < flen[4]; ++i )
	{
		const char c = (char)f[4][i];
		if( c < '0' || c > '9' )
			return false;
		iat = iat * 10 + ( c - '0' );
	}
	out->iat = iat;
	return true;
}

/// THE decision, the same function on the victim and on every peer: is `bytes` a genuine ticket for the
/// player at `vip` in `room`, valid at `nowS` on this machine's clock, at most `lateS` old? Cheap checks
/// first, then the signature, then the binding. `out` is filled whenever the fields parsed.
inline GXKickVerdict gxKickDecide( const unsigned char *bytes, int len, unsigned int vip, const char *room,
                                   long long nowS, int lateS, GXKickTicket *out )
{
	GXKickTicket scratch;
	if( out == NULL )
		out = &scratch;
	if( bytes == NULL || len <= GX_KICK_SIG_LEN || len > GX_KICK_BYTES_MAX )
		return GXKICK_MALFORMED;
	const int mlen = len - GX_KICK_SIG_LEN;
	if( !gxKickParseFields( bytes, mlen, out ) )
		return GXKICK_MALFORMED;
	const unsigned char *pk = gxKickPubkey();
	if( !gxKickPubkeyPinned( pk ) )
		return GXKICK_UNPINNED;
	if( gx_ed25519_verify( pk, bytes + mlen, bytes, (size_t)mlen ) != 1 )
		return GXKICK_BADSIG;
	if( out->vip != vip )
		return GXKICK_VIP;
	if( room == NULL || strcmp( out->room, room ) != 0 )
		return GXKICK_ROOM;
	if( nowS < out->iat - GX_KICK_EARLY_S || nowS > out->iat + lateS )
		return GXKICK_CLOCK;
	return GXKICK_OK;
}
