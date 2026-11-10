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

// GeneralsX: A durable per-install player identity, and the friends and
// ignore lists keyed by it.
//
// WHY THIS EXISTS. A friendship is an edge between two persistent people, and this project had no
// persistent people. It had two things that look like identity and are not:
//
//   * the nickname (GXNICK), which lives on the relay's PER-ROOM member record. The relay resolves
//     who is speaking from the source endpoint, so a client can only name ITSELF - but it may name
//     itself anything, and two players may hold one name at once. A label, not an identity.
//   * the virtual IP (GXWHO/GXYOU), which is scoped to a room, released on the sweep, and RECYCLED
//     across humans. The operator watched his own slot walk P1 -> P3 -> P4 -> P5 across a few
//     joins. That is the documented behaviour of a per-room address space, not a bug.
//
// A friend edge stored against either is wrong within minutes. So the id here is minted locally,
// once, and never changes.
//
// WHAT THIS ID IS, STATED PLAINLY SO IT IS NOT MISTAKEN FOR MORE:
//
//     IT IS A BEARER TOKEN. KNOWING IT IS BEING IT.
//
// It is 128 bits from the platform CSPRNG. It is safe because it is never published - not because
// it is verified. There is no proof of possession, and there cannot be one without asymmetric
// crypto: with no central registry, a verifier must check a claim against NO STORED STATE, and
// public-key signatures are the only construction that does that. A shared secret would require the
// verifier to hold it, which is a registry again, and a registry that leaks every secret in it.
//
// WHY NOT A KEYPAIR TODAY. Measured, not assumed: an unbounded grep for
// ed25519|curve25519|tweetnacl|libsodium|mbedtls across Core/, GeneralsMD/ and Generals/ returns
// NOTHING. The engine has no asymmetric crypto and no SHA-256. The one implementation in the tree
// is GameClient/GXEd25519.cpp (moved out of the launcher 11/08/2026), which is VERIFY-ONLY (signing needs the scalar
// multiplication and key expansion a verifier does not have) and belongs to a different binary.
// Vendoring a signing-capable one means ~2000 lines of field arithmetic landing in
// Core/GameEngine/Source/Common - INSIDE the simulation digest - forcing an atomic four-platform
// rebuild for a component whose failure mode is silent and cryptographic.
//
// And the security delta is smaller than it looks. Split the id into its two properties:
//
//   1. UNGUESSABILITY - nobody can discover it. Needs no crypto: 128 CSPRNG bits, full stop.
//   2. UNFORGEABILITY of a claim - nobody who KNOWS it can assert it. Needs the keypair.
//
// To exploit (2) you must first know the id, and an id is NEVER in a roster, NEVER in a game
// listing, NEVER broadcast and NEVER logged - it is disclosed only to someone who accepted a friend
// request. So the whole attack surface of the missing property is "an accepted friend can
// impersonate you to your other friends". Real, written down, and strictly smaller than today's
// hole, where any stranger can claim any nickname in any room.
//
// The upgrade is a drop-in: the id becomes base32(SHA-256(pubkey)[0..15]) and one challenge round
// is added. No wire shape changes. The one cost that cannot be engineered away is that a key-bound
// id is bound to its key BY CONSTRUCTION, so it cannot equal a previously random token - the
// migration re-mints every id once and friends must be re-added. With lists of a dozen entries that
// is the right trade against shipping unreviewed field arithmetic.
//
// DIGEST. This file and its .cpp live under GameClient/, which simsourcedigest_watcher.cmake does
// NOT digest. Identity is presentation and social state, never simulation state - nothing here may
// ever be read by GameLogic, or it stops being free to change per platform.

#pragma once

#include "Lib/BaseType.h"
#include "Common/AsciiString.h"
// GeneralsX: GXAssertRole and GX_ASSERTION_MAX. Both under GameClient/,
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
	// GeneralsX: The name cached against an id, "" when the id is not a
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

	// --- ignore ------------------------------------------------------------------------------
	// Durable scope only, and as of 08/08/2026 it is finally REACHABLE. It stayed deliberately
	// unwired for as long as the lobby had no way to learn anybody's id: an ignore keyed on a
	// nickname durably mutes whoever claims that name next, which is a worse failure than no durable
	// ignore at all. The lobby can now bind a room slot to a real id through the friend card
	// exchange, so `/ignore` on a slot whose id is known lands here and lasts; `/ignore` on a slot
	// whose id is not known stays SESSION-scoped and slot-keyed, in the lobby UI, exactly as before.
	// Both paths still exist on purpose - the second is the honest answer for a stranger.
	//
	// Never sent to the relay. It needs storage the relay does not have, and an ignore list is more
	// socially sensitive than a friends list - it is not going on a box the player does not own.
	static Int         getIgnoreCount();
	static const char *getIgnoreId(Int i);
	static Bool        isIgnored(const char *id);
	static Bool        addIgnore(const char *id);
	static Bool        removeIgnore(const char *id);

	// --- persistence -------------------------------------------------------------------------
	// --------------------------------------------------------------------------------------------
	// The VERIFIED identity, from Phase 3b: a Discord account, attested by a signed assertion that
	// the service minted and this client checked against a compiled-in public key.
	//
	// IT SITS BESIDE THE LOCAL ID AND DOES NOT REPLACE IT. The id above is what friend edges are
	// stored against and it keeps working with no account, on LAN, and offline - which is not a
	// fallback path, it is what most players will always have. Everything here answers "" or FALSE
	// in that case, and that is a normal state rather than an error.
	//
	// WHAT A VERIFIED NAME IS AND IS NOT. It says the service saw this Discord account and vouched
	// for this display name, to THIS machine, at the moment the file was read. It is checked
	// locally and IS NOT PRESENTED TO THE RELAY - nothing puts it on the wire, and it must stay
	// that way until a client keypair and a challenge round exist to bind it. Handed to a relay as
	// proof of who someone is, a thirty-day document is replayable bearer material; that is the
	// same failure tools/master/gxtoken.js:52-64 already analyses, and its stated fix is the
	// keypair upgrade described further up this header. So today this is a name the player has
	// earned the right to display, not a permission.
	//
	// Loading is lazy and happens ONCE per session. The assertion has a thirty-day life and the
	// game is not running for thirty days; re-reading it per call would buy nothing and would put
	// a file open on paths that are called per frame.
	// --------------------------------------------------------------------------------------------

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
	/// one of them and player mode - the owner's GXLogin.txt - to another.
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

	/// GeneralsX: Do not mint a new id this session. WinMain calls it when
	/// the one-time Windows migration (GXUserDataMigrate.h) could not bring the old GXPlayer.ini
	/// across. A minted file would be "already there" to every later retry, and the player's real
	/// identity and friends list would never arrive. With no id, friends are off for the session -
	/// the state a CSPRNG failure already produces, and every caller already handles.
	static void holdNewIdentity();
	static void reset();	///< test seam: drop all in-memory state so the next call re-reads disk

	// GeneralsX: A short unguessable token from the SAME CSPRNG the id
	// is minted from, base32 so it travels anywhere the id can. The card exchange binds each
	// offer to one of these: a recycled room slot cannot echo a nonce the previous occupant was
	// unicast, which is the continuity proof no roster field can give. FALSE (and an empty out)
	// when the platform CSPRNG fails - callers must refuse the exchange then, not proceed
	// guessably. chars is capped small; this is a nonce, not a key.
	static Bool mintNonce( char *out, Int chars );

private:
	static void ensureLoaded();
};
