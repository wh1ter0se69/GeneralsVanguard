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

// GeneralsX: A PEER WITH NO MENUS COULD NEVER DECLARE ITS IDENTITY.
//
// THE DEFECT, traced end to end against 0b84d8c1e:
//
//   Transport::resendRelayIdentity() runs after EVERY registration keepalive (Transport.cpp:980),
//   so the wire is already plumbed on every platform, headless included. It sends m_relayIdentity.
//   m_relayIdentity is written in exactly one place - Transport::sendRelayIdentity (Transport.cpp:2387)
//   - and every caller of that is GUI menu code: WOLLobbyMenu.cpp:1725 and :1786, GXLobbyCommands.cpp:186
//   (/login, /logout) and GXLobbyLANAPI::init (PopupJoinGame.cpp:595), which only the GeneralsMD lobby
//   screens construct. Unbounded grep, no head, 20/09/2026.
//
//   A HEADLESS PEER NEVER OPENS A MENU. So a headless host with a perfectly valid, unexpired
//   GXLogin.txt on disk and hasVerifiedIdentity() returning TRUE still sent no GXIDN, and a relay that
//   requires sign-in drops an unidentified member IDENT_GRACE_MS after it joins (relay.js:7781) - every
//   time, silently, with the lobby simply going empty.
//
//   (24/09/2026: that account is a BOT login handed over in GX_IDENTITY_FILE. A scripted peer no longer
//   declares a GXLogin.txt at all - see the guard in gxDeclareIdentityIfChanged.)
//
// WHY IT LIVES IN A HEADER, AND IS CALLED FROM THREE PLACES.
//
//   TWO ENGINE CLASSES, SO THE BODY LIVES HERE RATHER THAN IN EITHER. d19090d14 is the commit where
//   that pair drifted and a fix shipped to three platforms while Windows stayed silently broken.
//   GameEngineDevice/ is not digested, and both engine classes already pump TheLAN and
//   TheGXLobbyPresence from update() for the identical reason: a pump hung off a UI callback only runs
//   while that screen is on the stack (SDL3GameEngine.cpp:1110).
//
//   AND THE HEADLESS LOBBY LOOP, WHICH THE ENGINE PUMP DOES NOT REACH. This was planned as an
//   engine-pump-only change on the grounds that GameClient/ and GameEngineDevice/ are outside the
//   simulation source digest, so nothing would move sourceID. MEASURED 20/09/2026, and the plan was
//   wrong: a headless host with a valid login was still dropped by a relay at
//   RELAY_REQUIRE_IDENTITY=1, every run. TheGameEngine->update() is called from exactly ONE place in
//   HeadlessMatch.cpp - runMatch's loop - and runMatch does not begin until driveLobby has returned.
//   The relay's IDENT_GRACE_MS expires during driveLobby, inside HeadlessMatch::pumpUntil, where the
//   only thing that runs is TheLAN->update(). The engine pump had never been called once by the time
//   the peer was swept.
//
//   There is no non-digested code in that loop to hang this off, so the call in
//   HeadlessMatch::pumpUntil is inside the digest and DOES move sourceID. That cost is paid
//   deliberately: a headless peer that cannot identify is the entire defect this file exists for.
//   scripts/test/headless-identity-test.sh is the arm that fails without it - and be exact about how
//   (review r2, 23/09/2026): its unsigned relay arms fail without the call because the [GXIDENT] line
//   never appears; that the relay then KEEPS a signed peer is only the signed-enforced arm, which needs
//   a bot login (plan T1, tools/relay/gxbot.mjs) and a relay that admits bots (plan R1). Until both
//   exist that half rests on 03c9266dc's measurement with a human login, a path b12288191 removed.
//
//   STILL REFUSED: Transport.cpp and LANAPI.cpp. Both are digested too, and Transport.cpp:2378
//   separately forbids calling into GXIdentityAssertion from inside GameNetwork/ - that decoupling is
//   what keeps a change to the login FORMAT off the four-platform path, which is a different and
//   still-worthwhile property.
//
// WHAT IT IS NOT. It is not an admission check. The relay verifies the signature and decides who may
// stay; a check in the client would be a check a patched client skips (Transport.cpp:2385). This only
// makes sure the peer SAYS what it already holds.
//
// LAN AND OFFLINE ARE UNTOUCHED, by construction and not by intent:
//   * no relay configured  -> isRelayEnabled() is FALSE -> returns before doing anything;
//   * nobody signed in     -> getVerifiedAssertionText() is "" -> setLobbyIdentity("") lands in
//     sendRelayIdentity's clear branch, which with nothing declared sends NO datagram and returns
//     FALSE (Transport.cpp:2389-2419).
// That is the invariant GXIdentityAssertion.h:41 states, and it survives this file.
//
// READ "no relay configured" LITERALLY, because the DEFAULT IS NOT THAT.
// OptionPreferences::getRelayAddress() returns GX_BOOTSTRAP_RELAY when RelayAddress is absent or
// blank (OptionPreferences.cpp:268-277); only the literal "none" or GENERALSX_NO_RELAY gives the
// empty string initRelay refuses on. So on a default install the LAN screen IS relay-enabled and
// this file DOES declare there - which is the whole point, since that is how a headless host is
// reached. The first bullet is the offline/"none" case, not "the player picked NETWORK".
// fix/lan-local-default is changing that default on its own branch; nothing here has to move when
// it lands, because the predicate is isRelayEnabled() and not a screen name.

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

/// Declare this peer's signed identity on both relay sockets whenever it has changed, or whenever
/// TheLAN has been rebuilt underneath us. Call from the engine's per-frame update, beside the
/// TheLAN->update() and TheGXLobbyPresence->update() pumps.
///
/// KEYED ON (TheLAN, fingerprint) AND NOT ON THE FINGERPRINT ALONE. A fresh LANAPI brings a fresh
/// Transport whose m_relayIdentity is empty (PopupJoinGame.cpp:570-589 records that defect for the
/// GUI screens), so an unchanged assertion still has to be re-declared to it. In a GUI build
/// GXLobbyLANAPI::init already covers that case and this is a second net; in a headless build TheLAN
/// is built once in HeadlessMatch::setup and lives for the process, so the pair is exact.
///
/// THE LOBBY PRESENCE IS KEYED ON THE FINGERPRINT ALONE, AND NOT ON TheLAN AT ALL.
/// TheGXLobbyPresence is one object for the life of the process, keeps its identity across a
/// re-acquire, and outlives every TheLAN: Back from a lobby deletes TheLAN and the presence keepalive
/// goes on sending whatever it holds every 5 s. This used to return at `TheLAN == NULL` before reading
/// the assertion, so a bot file renewed while the client sat on the main menu was never re-read and
/// the presence membership was dropped by an enforcing relay with a fresh file on disk. Now the
/// assertion is read whenever EITHER relay member is up, and each is brought current on its own key.
/// Presence is pushed only while ADMITTED: before that there is no membership to bring current, and
/// a client that never entered Online - LAN, offline - must not be handed a copy of the token at all.
///
/// SAFE TO CALL FROM A 10 ms POLL, which is what HeadlessMatch::pumpUntil does: the unchanged path
/// is a pointer compare plus a hash of at most GX_ASSERTION_MAX (256) bytes and sends nothing.
///
/// CHEAP ENOUGH TO RUN EVERY FRAME: on the unchanged path it is two pointer loads, one
/// getVerifiedAssertionText() (a cached static after the first call) and a hash of at most
/// GX_ASSERTION_MAX bytes. It sends nothing.
inline void gxDeclareIdentityIfChanged( void )
{
	static const void      *s_lastLan = NULL;		// the game socket: keyed on (TheLAN, fingerprint)
	static unsigned long long s_lastFp = 0;
	static Bool             s_pushed  = FALSE;
	static unsigned long long s_presenceFp = 0;		// the lobby socket: keyed on the fingerprint alone
	static Bool             s_presencePushed = FALSE;

	// A TEARDOWN FORGETS EVERYTHING. TheLAN is deleted and rebuilt constantly on the GUI paths
	// (LanLobbyMenu, NetworkDirectConnect and WOLLobbyMenu all `delete TheLAN` and then
	// GXCreateLobbyLANAPI), and a fresh allocation lands on the address the old one just freed more
	// often than not - so "same pointer, same fingerprint" can name a BRAND NEW transport whose
	// m_relayIdentity is empty. Clearing the flag whenever TheLAN is absent makes the next frame
	// re-declare.
	//
	// IT DOES NOT CLOSE THAT HOLE ON ITS OWN, and saying otherwise would be the more dangerous
	// comment: NetworkDirectConnect's `TheLAN = nullptr;` is immediately followed by the rebuild in
	// the same function, so the pump never observes the NULL and this branch never runs for it. Only
	// a teardown that outlives a frame - the shutdown path at LanLobbyMenu.cpp:1039 - is caught here.
	//
	// What actually covers the same-frame rebuild is GXLobbyLANAPI::init (PopupJoinGame.cpp), which
	// declares on every GUI-built transport and is the one door all of those call sites go through -
	// added 18/09/2026 as a fix for precisely this shape. The exact test would be asking the
	// transport what it holds, and there is no accessor for m_relayIdentity; adding one means editing
	// Transport.h, which is inside the simulation source digest and would turn a login fix into an
	// atomic four-platform ship. So: this is a cheap second net, not the primary guard, and anything
	// that stops going through GXLobbyLANAPI::init needs its own declare.
	if (TheLAN == NULL)
		s_pushed = FALSE;

	// isRelayEnabled(), NOT "is this the Online screen". The owner's rule is that anything using OUR
	// networking needs an account, and NETWORK/Direct Connect becomes our networking the moment
	// RelayAddress resolves (Transport.cpp:881) - which is exactly how a headless host runs and how
	// players reach it.
	const Transport *transport = TheLAN != NULL ? TheLAN->getTransport() : NULL;
	const Bool game     = transport != NULL && transport->isRelayEnabled();
	// The shared-lobby socket, a SECOND relay member with its own sub (GXLobbyPresence.h:177). NULL in a
	// headless build, and IDLE on a client that never entered Online - see the header on why ADMITTED.
	const Bool presence = TheGXLobbyPresence != NULL && TheGXLobbyPresence->isAdmitted();
	// GeneralsX: THE ADMIN KEY rides the same pump, on the game socket only: offer the public
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

	// GeneralsX: A SCRIPTED PEER DECLARES A BOT LOGIN OR NOTHING - NEVER THE
	// PROFILE'S OWN SIGN-IN (merge-candidate review, P1).
	//
	// Every -lanhost/-lanjoin peer runs HeadlessMatch, headless or rendered, and outside bot mode the
	// loader reads GXLogin.txt from whatever user-data folder the peer was started in - so a scripted peer
	// started in a player's real profile would present that player's account, roles included, and a kick
	// or ban aimed at the scripted peer would land on them. On integration no scripted peer
	//
	//
	// declared anything (PopupJoinGame.cpp, GXLobbyLANAPI::init: "HeadlessLANAPI must NOT pick this up").
	//
	// So a scripted peer declares ONLY what bot mode (GX_IDENTITY_FILE) verified, and never a moderator or
	// admin role - verifyBot admits role 'b' only, as player, and this does not lean on that. Outside bot
	// mode this does not even LOAD the profile's sign-in (every other loader call is menu code, which a
	// HeadlessMatch process never reaches). "" is the wire clear, which on a transport holding nothing
	// sends nothing: integration's behaviour exactly. A peer with no -lan* flags is someone at the
	// keyboard, and declares their own account as any client does. -lanlist has no role and reaches no
	// declare: its loop pumps TheLAN and nothing else.
	//
	// PINNED AT THE SOURCE (scripts/test/signin-copies-test.cpp), not by a run: the red arm would need a
	// production-signed login in a scripted peer's profile, which is exactly what must never be used.
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

		// GeneralsX: ONE LINE PER DECLARE, never the value (review r2): the only
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
