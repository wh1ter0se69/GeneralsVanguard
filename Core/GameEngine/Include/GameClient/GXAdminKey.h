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

// GeneralsX: THIS CLIENT'S ADMIN KEY: the private half that signs its cheat orders when the
// relay proves it an admin for a match (Common/GXAdminOrder.h says why a signature and not the sender field).
//
// ONE KEY PER MATCH. The client always holds a CURRENT key and offers its public half to the relay on its game socket
// (GXAKEY, declared by gxDeclareIdentityIfChanged). The relay publishes it only while the account is a live admin
// (relay.js GXAKR). When a match really begins with that key stamped for our slot, it becomes the MATCH key - the one
// this match's orders are signed with - and is marked USED. A used key goes on being offered for as long as that
// match's network is up, because the other peers verify the stamp against the relay's answer at their own start, which
// can be after ours; the moment the network is gone a fresh key replaces it (gxAdminKeyCurrentB64). So a key signs
// orders in one match - or, when a rematch is stamped with it because the relay had not yet heard the fresh one, in
// further matches under seeds it never signed under (gxAdminKeyBeginMatch) - and an order signed in one match cannot
// be replayed into another even by a host that reuses the seed. (Measured: minting the replacement AT the start made a joiner that started a moment after the admin
// read the new key from the relay and refuse the start - "the relay proves 0x2 with other keys".)
//
// NOTHING HERE IS SIMULATION STATE. The simulation holds the stamped PUBLIC keys (GameLogic) and decides with
// Common/GXAdminOrder.h, which is digested; this header only keeps secrets on the machine that owns them. Header only,
// with one function-local static, main thread only.

#pragma once

#include "Common/GXAdminOrder.h"
#include "GameClient/GXPlayerIdentity.h"	// cryptoRandomBytes, the engine's one CSPRNG

#include <string.h>

struct GXAdminKeyState
{
	unsigned char curSeed[32];
	unsigned char curPub[32];
	bool curOk;
	unsigned char matchSeed[32];
	unsigned char matchPub[32];
	bool matchOk;
	unsigned int matchSeq;
	bool curUsed;		///< the current key became a match key; replaced once that match's network is gone
	unsigned int matchGameSeeds[16];	///< every match seed the match key has signed under (gxAdminKeyBeginMatch)
	int matchGameSeedCount;
};

inline GXAdminKeyState &gxAdminKeyState()
{
	static GXAdminKeyState s_state;		// zero-initialised: no key until the first use
	return s_state;
}

/// Mint a fresh current key. FALSE when the CSPRNG failed - the client then has no admin key this session, which
/// costs an admin their cheats and nothing else; never a weaker seed.
inline bool gxAdminKeyMint( GXAdminKeyState &s )
{
	s.curOk = GXPlayerIdentity::cryptoRandomBytes( s.curSeed, 32 ) && gx_ed25519_public_from_seed( s.curSeed, s.curPub ) == 1;
	return s.curOk;
}

/// The public half of the key this client offers the relay, as base64url (44 bytes with the NUL). FALSE when there
/// is none. `matchRunning` is whether a match's network is up (TheNetwork): a key that was used for a match is
/// replaced here, and only once that is false.
inline bool gxAdminKeyCurrentB64( char out[44], bool matchRunning )
{
	GXAdminKeyState &s = gxAdminKeyState();
	if( s.curUsed && !matchRunning )
	{
		s.curUsed = false;
		gxAdminKeyMint( s );
	}
	if( !s.curOk && !gxAdminKeyMint( s ) )
		return false;
	gxAdminKeyToB64( s.curPub, out );
	return true;
}

/// Is `stamped` - the key a start stamped for OUR slot - the key we are offering right now? The start-time check an
/// admin makes on itself at a real start (LANAPI::OnGameStart): a key it no longer offers is one it cannot sign with.
inline bool gxAdminKeyIsCurrent( const unsigned char stamped[32] )
{
	const GXAdminKeyState &s = gxAdminKeyState();
	return s.curOk && memcmp( s.curPub, stamped, 32 ) == 0;
}

/// A match really began. `stamped` is the key it stamped for our slot, or NULL when our slot is no admin slot, and
/// `gameSeed` the match's seed - the one every order's payload binds. When `stamped` is our current key - and not one
/// already used for a match - that key becomes the match key (sequence numbers restart) and is marked used.
///
/// GeneralsX: ...OR IT IS STILL THE LAST MATCH KEY. The fresh key goes to the
/// relay on the lobby socket (GXAKEY, resent on the keepalive), so a rematch started before it lands - or after it was
/// lost - is stamped with the previous match's key, which every peer's own relay confirms, and the admin could not sign
/// a thing all match. What one key per match protects is that no order signed in one match verifies in another, and a
/// payload binds the match seed, so it is kept exactly by never signing twice under one seed: the last match key signs
/// again only under a seed it has not signed under. Anything else leaves no match key: this client cannot sign this match.
inline void gxAdminKeyBeginMatch( const unsigned char *stamped, unsigned int gameSeed )
{
	GXAdminKeyState &s = gxAdminKeyState();
	s.matchOk = false;
	s.matchSeq = 0;
	if( stamped == NULL )
		return;
	if( gxAdminKeyIsCurrent( stamped ) && !s.curUsed )
	{
		memcpy( s.matchSeed, s.curSeed, 32 );
		memcpy( s.matchPub, s.curPub, 32 );
		s.matchGameSeedCount = 0;
		s.curUsed = true;
	}
	else if( s.matchGameSeedCount == 0 || memcmp( s.matchPub, stamped, 32 ) != 0 )
		return;
	const int cap = (int)( sizeof( s.matchGameSeeds ) / sizeof( s.matchGameSeeds[0] ) );
	for( int i = 0; i < s.matchGameSeedCount; ++i )
		if( s.matchGameSeeds[i] == gameSeed )
			return;		// signed under this seed before: an order from then would verify now
	if( s.matchGameSeedCount >= cap )
		return;
	s.matchGameSeeds[s.matchGameSeedCount++] = gameSeed;
	s.matchOk = true;
}

/// The next sequence number for one admin order of this match (the caller builds the payload for it and signs it with
/// gxAdminKeySign). FALSE when this client holds no match key.
inline bool gxAdminKeyNextSeq( unsigned int *seq )
{
	GXAdminKeyState &s = gxAdminKeyState();
	if( !s.matchOk || seq == NULL )
		return false;
	*seq = ++s.matchSeq;
	return true;
}

inline bool gxAdminKeySign( const GXAdminPayload *p, unsigned char sig[64] )
{
	const GXAdminKeyState &s = gxAdminKeyState();
	return s.matchOk && p != NULL && p->ok && gx_ed25519_sign( sig, p->b, (size_t)p->len, s.matchSeed ) == 1;
}
