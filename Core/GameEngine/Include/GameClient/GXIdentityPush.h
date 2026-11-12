/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// A headless or scripted peer never opens a menu, so nothing declared its identity (only GUI menu code wrote
// it), and with RELAY_REQUIRE_IDENTITY=1 a signed-in standing host was dropped IDENT_GRACE_MS after joining.
// This declares it from the engine pump and HeadlessMatch::pumpUntil (that call is in the simulation digest,
// deliberately). Not an admission check: the relay verifies; this only makes the peer SAY what it holds.

#pragma once

#include "Common/GlobalData.h"	// m_lanRole: which peers are scripted. Zero Hour's field - and every includer is Zero Hour
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXAdminKey.h"	// the admin key offered on the game socket
#include "GameClient/GXLobbyPresence.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"	// extern LANAPI *TheLAN
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"	// TheNetwork: a used admin key is replaced once the match is gone

#include <stdio.h>	// fprintf: the [GXIDENT] line

/// A FINGERPRINT, NOT A COPY, for the same reason WOLLobbyMenu.cpp's own detector keeps one: the
/// comparison only needs to know whether the value CHANGED, and holding the assertion itself would
/// put a second copy of a bearer token in process memory for the life of the process, outliving
/// every sign-out. FNV-1a 64; a collision costs one skipped re-declare at odds of about 2^-64.
///
/// Deliberately a second copy of those eight lines rather than a refactor of WOLLobbyMenu.cpp:
/// scripts/test/lobby-identity-clear-test.cpp:299 asserts against that function's own body, and
/// rewriting a tested file to save eight lines is the more expensive change.
inline unsigned long long gxAssertionFingerprint64( const char *s )
{
	unsigned long long h = 1469598103934665603ULL;		// FNV-1a 64 offset basis
	if (s == NULL)
		return h;
	for (int i = 0; s[i] != '\0' && i < GX_ASSERTION_MAX; ++i)
	{
		h ^= (unsigned char)s[i];
		h *= 1099511628211ULL;							// FNV-1a 64 prime
	}
	return h;
}

/// Declare this peer's signed identity on both relay sockets when it changed or TheLAN was rebuilt; call it from
/// the engine's per-frame update beside the TheLAN and TheGXLobbyPresence pumps.
/// The game socket is keyed on (TheLAN, fingerprint); the presence on the fingerprint alone (it outlives every
/// TheLAN) and only while ADMITTED. The unchanged path is cheap and sends nothing.
inline void gxDeclareIdentityIfChanged( void )
{
	static const void      *s_lastLan = NULL;		// the game socket: keyed on (TheLAN, fingerprint)
	static unsigned long long s_lastFp = 0;
	static Bool             s_pushed  = FALSE;
	static unsigned long long s_presenceFp = 0;		// the lobby socket: keyed on the fingerprint alone
	static Bool             s_presencePushed = FALSE;

	// A TEARDOWN FORGETS EVERYTHING. TheLAN is rebuilt on the GUI paths and a fresh allocation often reuses the
	// freed address, so "same pointer, same fingerprint" can name a NEW transport with an empty m_relayIdentity;
	// clearing the flag re-declares. A same-frame rebuild is not seen here: GXLobbyLANAPI::init (PopupJoinGame.cpp)
	// covers it. Asking the transport would need a Transport.h accessor, which is in the simulation digest.
	if (TheLAN == NULL)
		s_pushed = FALSE;

	// isRelayEnabled(), NOT "is this the Online screen". The rule is that anything using OUR
	// networking needs an account, and NETWORK/Direct Connect becomes our networking the moment
	// RelayAddress resolves (Transport.cpp:881) - which is exactly how the standing host runs and how
	// players reach it.
	const Transport *transport = TheLAN != NULL ? TheLAN->getTransport() : NULL;
	const Bool game     = transport != NULL && transport->isRelayEnabled();
	// The shared-lobby socket, a SECOND relay member with its own sub (GXLobbyPresence.h:177). NULL in a
	// headless build, and IDLE on a client that never entered Online - see the header on why ADMITTED.
	const Bool presence = TheGXLobbyPresence != NULL && TheGXLobbyPresence->isAdmitted();
	// THE ADMIN KEY rides the same pump, on the game socket only: offer the public
	// half of this client's current per-match key (GameClient/GXAdminKey.h) whenever the transport does not already
	// hold exactly it. It is public and grants nothing by itself - the relay publishes it only for a live admin
	// (relay.js gxLiveAdmin) - so every client offers one, scripted peers included. ASKED OF THE TRANSPORT ITSELF, not
	// remembered here: a GUI path that deletes TheLAN and rebuilds it in the same frame can land the new
	// one on the old address, and a (pointer, key) memo would then skip a transport that holds nothing. The transport
	// repeats what it holds on every keepalive.
	{
		char key[44];
		if (game && gxAdminKeyCurrentB64(key, TheNetwork != NULL) && strcmp(TheLAN->getTransport()->getRelayAdminKey(), key) != 0)
			TheLAN->getTransport()->sendRelayAdminKey(key);
	}

	if (!game && !presence)
		return;													// no relay member is up: read nothing, send nothing

	// A SCRIPTED PEER DECLARES A BOT LOGIN OR NOTHING - NEVER THE PROFILE'S OWN SIGN-IN: every -lanhost/-lanjoin
	// peer runs HeadlessMatch and could load a real profile's GXLogin.txt, presenting that Discord account (admin
	// role included) to the production relay, so a kick or ban aimed at a harness peer would land on the person. It
	// declares ONLY what bot mode (GX_IDENTITY_FILE) verified, never a moderator or admin role. "" is the wire clear.
	const Bool scripted = TheGlobalData->m_lanRole != LANROLE_NONE;
	const char *now = (scripted && !GXPlayerIdentity::isBotMode()) ? NULL : GXPlayerIdentity::getVerifiedAssertionText();
	if (now == NULL || (scripted && GXPlayerIdentity::getVerifiedRole() != GX_ROLE_PLAYER))
		now = "";
	const unsigned long long fp = gxAssertionFingerprint64(now);

	if (game && !(s_pushed && s_lastLan == (const void *)TheLAN && s_lastFp == fp))
	{
		s_pushed  = TRUE;
		s_lastLan = (const void *)TheLAN;
		s_lastFp  = fp;

		// ONE LINE PER DECLARE, never the value: the only
		// thing that shows this ran. headless-identity-test.sh reads it - present in the relay arms (fails
		// without HeadlessMatch::pumpUntil's call, signed or not), absent in the relay-less one (fails if
		// the guard above is inverted). Sending "" is silent on the wire, so the relay alone cannot tell.
		fprintf(stderr, "[GXIDENT] identity declared to the relay: %s\n", now[0] != '\0' ? "signed" : "none");

		// The game socket. "" is the wire clear, and on a transport holding nothing it sends nothing.
		TheLAN->setLobbyIdentity(now);
	}

	if (presence && !(s_presencePushed && s_presenceFp == fp))
	{
		s_presencePushed = TRUE;
		s_presenceFp     = fp;
		fprintf(stderr, "[GXIDENT] identity declared to the lobby presence: %s\n", now[0] != '\0' ? "signed" : "none");
		TheGXLobbyPresence->setIdentity(now);
	}
}
