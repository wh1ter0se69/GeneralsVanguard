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

// Durable per-install player identity plus the friends and ignore lists keyed by it; minted locally once.
// IT IS A BEARER TOKEN: KNOWING IT IS BEING IT. 128 CSPRNG bits, safe only because it is never published or logged.
// No proof of possession (the engine has no signing-capable crypto): an accepted friend could impersonate you.
// Client-only state, not in the simulation digest: GameLogic must never read it.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
// GXAssertRole and GX_ASSERTION_MAX. Both under GameClient/,
// so this include adds nothing to the sim digest.
#include "GameClient/GXIdentityAssertion.h"
#include "GameClient/GXSignOutFiles.h"	// GXSignInRemoval

// 128 bits, base32-encoded with no padding: ceil(128/5) = 26 characters.
//
// The alphabet is RFC 4648 (A-Z, 2-7), chosen so every character is inside the relay's TOKEN_RE
// ([A-Za-z0-9._-], relay.js:58). An id that could not survive a relay token field would be useless
// on the wire, and discovering that at the socket rather than here would be a bad afternoon.
enum { GX_PLAYER_ID_LEN = 26 };

class GXPlayerIdentity
{
public:
	// This install's id. Generated and persisted on first call, then stable forever. Never empty:
	// see the .cpp for why a CSPRNG failure is fatal rather than falling back to rand().
	static const char *getPlayerId();

	// TRUE if s is exactly GX_PLAYER_ID_LEN characters of the base32 alphabet. Everything arriving
	// from the network goes through this before it is stored or echoed - an id is re-emitted inside
	// space-separated, bar-delimited relay lines, so a stray space or '|' would corrupt the reply
	// for every client in the room and not only for the sender. Same rule NICK_RE enforces on the
	// relay, applied on the client because a guarantee must hold for a datagram that did not come
	// from the stock client.
	static Bool isValidPlayerId(const char *s);

	// --- friends -----------------------------------------------------------------------------
	// The edge lives HERE, on the client, never on the relay. The relay is multi-relay by design
	// (that is what the signed manifest's relay list is for), so an edge stored on relay A is
	// meaningless on relay B; it has no persistent storage; and restarting it drops every match in
	// progress, so the store could never be migrated at a convenient moment.
	static Int         getFriendCount();
	static const char *getFriendId(Int i);		///< "" if i is out of range
	static const char *getFriendNick(Int i);	///< last name we saw them use, "" if never
	static Bool        isFriend(const char *id);
	// The name cached against an id, "" when the id is not a
	// friend or has no name yet. Added when the lobby learned how to bind a room slot to a real id
	// (GXKEY/the friend card): before that there was nothing to look a friend up BY except the name,
	// so every caller walked the list comparing names and there was no reason for this to exist.
	//
	// Returns a pointer INTO the store. It is valid until the next mutation - addFriend,
	// removeFriend, noteFriendNick or reset - exactly like getFriendNick(Int), and for the same
	// reason: std::map nodes are erased by those calls. Copy before mutating; the /friend remove
	// path already documents having been bitten by that.
	static const char *getFriendNickById(const char *id);
	static Bool        addFriend(const char *id, const char *nick);	///< FALSE if id is malformed
	static Bool        removeFriend(const char *id);
	static Bool        noteFriendNick(const char *id, const char *nick);	///< refresh a known friend's name

	// --- ignore ---
	// A durable ignore needs a real id: `/ignore` on a slot whose id is known (friend card exchange) lands here and lasts.
	// A slot with no known id stays session-scoped in the lobby UI (a nickname key would mute whoever claims it next).
	// Never sent to the relay: it has no storage for it, and an ignore list is more sensitive than a friends list.
	static Int         getIgnoreCount();
	static const char *getIgnoreId(Int i);
	static Bool        isIgnored(const char *id);
	static Bool        addIgnore(const char *id);
	static Bool        removeIgnore(const char *id);

	// --- persistence ---
	// The VERIFIED identity: a Discord account attested by a signed assertion checked against a compiled-in public key.
	// It sits beside the local id, which works with no account; then everything here answers "" or FALSE (normal).
	// Loaded lazily, once per session.

	static Bool        hasVerifiedIdentity();
	static const char *getVerifiedName();		///< "" when there is no verified identity
	static const char *getVerifiedSub();		///< "" when none. OPAQUE - compare it, never parse it
	/// The verified assertion VERBATIM, for the relay handshake. "" when there is no verified
	/// identity, and never set from a document that failed to verify locally.
	static const char *getVerifiedAssertionText();
	/// PRESENTATION ONLY - a decoration and a colour, never a permission. GX_ROLE_PLAYER when none.
	static GXAssertRole getVerifiedRole();
	static long long   getVerifiedExpiry();		///< unix seconds, 0 when none
	static Int         getVerifiedDaysLeft();	///< 0 when none; for the "log in again soon" hint

	/// Bot mode: GX_IDENTITY_FILE is set and non-empty (the .cpp's botFileEnv). ONE predicate for the
	/// loader, the relay declare (GXIdentityPush.h) and /login, so an empty value cannot be bot mode to
	/// one of them and player mode - the player's GXLogin.txt - to another.
	static Bool isBotMode();

	/// Re-read the assertion file. Called after a login completes and after a logout, and nowhere
	/// else - it is the only thing that makes a mid-session login visible.
	static void refreshVerifiedIdentity();

	/// Sign out: delete the assertion file (and an interrupted write's .tmp) and drop the loaded
	/// claims and the raw assertion. Deleting a file needs no HTTPS, so unlike logging IN this works
	/// identically on all four platforms. Says WHICH file survived, if any - see GXSignOutFiles.h,
	/// because the two survivals have different consequences. Callers wanting the whole sequence
	/// use GXLobbySignOut().
	static GXSignInRemoval clearVerifiedIdentity();

	/// The platform CSPRNG, shared so that nothing else in the engine grows a second answer to
	/// "where do unguessable bytes come from". FALSE on failure, and a caller that gets FALSE must
	/// abandon whatever it was doing rather than fall back to something weaker - see the comment
	/// on cryptoRandom in the .cpp, and the FATAL path in ensureLoaded.
	static Bool cryptoRandomBytes(unsigned char *out, Int count);

	static Bool save();		///< write through; callers above already do this on every mutation

	/// Do not mint a new id this session. WinMain calls it when
	/// the one-time Windows migration (GXUserDataMigrate.h) could not bring the old GXPlayer.ini
	/// across. A minted file would be "already there" to every later retry, and the player's real
	/// identity and friends list would never arrive. With no id, friends are off for the session -
	/// the state a CSPRNG failure already produces, and every caller already handles.
	static void holdNewIdentity();
	static void reset();	///< test seam: drop all in-memory state so the next call re-reads disk

	// A short unguessable token from the SAME CSPRNG the id
	// is minted from, base32 so it travels anywhere the id can. The card exchange binds each
	// offer to one of these: a recycled room slot cannot echo a nonce the previous occupant was
	// unicast, which is the continuity proof no roster field can give. FALSE (and an empty out)
	// when the platform CSPRNG fails - callers must refuse the exchange then, not proceed
	// guessably. chars is capped small; this is a nonce, not a key.
	static Bool mintNonce( char *out, Int chars );

private:
	static void ensureLoaded();
};
