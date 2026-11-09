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

// GeneralsX @feature Claude 11/08/2026 The signed identity assertion, and the client's half of it,
// which is VERIFICATION AND NOTHING ELSE.
//
// WHAT THIS IS FOR. GXPlayerIdentity.h:37 states plainly that today's player id "IS A BEARER TOKEN.
// KNOWING IT IS BEING IT." Everything the project wants next runs into that: a short xxxx-nnnn
// friend code is ~32 bits and brute-forceable, adding an offline player needs somewhere durable to
// put the request, and moderation needs a durable answer to "who is banned". None of the three is
// safe while the id is a secret. An assertion signed by a service converts the id into a HANDLE -
// knowing someone's handle grants nothing - and that is what unblocks all three.
//
// WHY THE CLIENT ONLY EVER VERIFIES, WHICH IS THE DECISION THE WHOLE DESIGN TURNS ON.
// The Windows engine has no HTTPS at all: vcpkg.json gates curl "platform": "!windows", and that
// box has no CA store either. It cannot perform an OAuth round trip, and it never will. So the
// browser does the OAuth dance, a service returns a SIGNED ASSERTION rather than a session, the
// launcher writes it beside GXPlayer.ini, and the game checks a signature. Verification needs no
// TLS, no CA store, and no secret in the client - which is exactly why it works on the platform
// that cannot do anything else.
//
// FETCHING IS NOT IN THIS FILE AND MUST NOT ARRIVE IN IT. If a future edit here opens a socket,
// the Windows build breaks and the design has been inverted.
//
// LAN AND OFFLINE PLAY MUST NEVER REQUIRE A LOGIN. An absent assertion is GX_ASSERT_ABSENT, which
// is a normal state and not an error - it is what every LAN player and every offline player has,
// forever. An invalid one is refused and the caller falls back to the local GXPlayerIdentity token
// exactly as today. A service outage, an expired assertion, a player with no account and a player
// whose system clock is wrong all land in the same place: they play, unauthenticated. As of Phase
// 3a NOTHING CALLS THIS AT ALL, so that property is currently true by construction; the first
// caller must keep it true by treating every non-OK status as "no account this session".
//
// THE FORMAT IS NOT NEW. It is tools/master/gxtoken.js's admission ticket with different claims,
// deliberately: '~'-separated, version first, signature last over every byte before the final
// separator, base64url, forward-compatible by appending a field before the signature. That file's
// header sets the goal - "one algorithm, one key format, one set of test vectors" - and a second
// bespoke format here would have cost exactly that. '~' is in none of the field charsets, so there
// is no escaping rule to get wrong, and an assertion contains no space, so it survives a
// space-delimited relay line whole if it ever has to travel on one.
//
//     1~<sub>~<name>~<iat>~<exp>~<sig>
//
//     1      format version, one digit. A version this code does not know is REFUSED, never
//            guessed at: a newer signer may mean the same bytes differently, and misreading an
//            identity claim is the one failure that must not be silent.
//     sub    the account id at the issuing provider. ^[0-9]{1,20}$ TODAY because the provider is
//            Discord and a snowflake is decimal - but see the warning on GXAssertionClaims::sub,
//            because that is a validation policy and NOT a licence to treat it as a number.
//     name   display name, 0-20 characters of [A-Za-z0-9_.-], the relay's own NICK_RE
//            (tools/master/store.js:81). May be empty.
//     iat    issued-at, unix seconds, decimal.
//     exp    expiry, unix seconds, decimal.
//     sig    Ed25519 over every byte before the final '~', base64url, 86 characters.
//
// DIGEST. This file and its .cpp live under GameClient/, which simsourcedigest_watcher.cmake does
// NOT digest, so a change to the login format costs no four-platform rebuild. Nothing here may ever
// be read by GameLogic - identity is social state, never simulation state. Measured, not assumed:
// the digest was recorded before and after this work landed and did not move.

#ifndef GX_IDENTITY_ASSERTION_H
#define GX_IDENTITY_ASSERTION_H

#include <stddef.h>

// Deliberately NO PreRTS.h, no AsciiString, no Bool. This header and its .cpp compile standing
// alone, outside the engine and outside cmake, which is what lets scripts/test/
// identity-assert-test.cpp drive them with nothing but clang. A security primitive whose only test
// needs a working full-engine build is a primitive that silently stops being tested the first time
// that build breaks - and this one has to be verifiable on a machine that cannot build the engine
// at all.

enum GXAssertStatus
{
	GX_ASSERT_OK = 0,		///< signed by the expected key, well formed, and current
	GX_ASSERT_ABSENT,		///< nothing to check: no text, or no file. NOT an error - see above
	GX_ASSERT_LENGTH,		///< longer than GX_ASSERTION_MAX
	GX_ASSERT_FIELDS,		///< fewer than six '~'-separated fields
	GX_ASSERT_VERSION_BAD,	///< a format version this code does not speak
	GX_ASSERT_SIGSHAPE,		///< the signature is not 86 characters of canonical base64url
	GX_ASSERT_SIGNATURE,	///< it did not verify under the given key
	GX_ASSERT_MALFORMED,	///< signed, but a field is outside its charset or length
	GX_ASSERT_LIFETIME,		///< signed, but exp <= iat or the window is longer than we accept
	GX_ASSERT_NOTYET,		///< signed and current, but issued in the future beyond the skew
	GX_ASSERT_EXPIRED,		///< signed, but past exp beyond the skew
	GX_ASSERT_NOKEY			///< no trust anchor is compiled in, so nothing can be trusted
};

// GeneralsX @feature Claude 11/08/2026 The role the signer claims this account holds.
//
// WHAT A ROLE IS, AND MUCH MORE IMPORTANTLY WHAT IT IS NOT. This is a claim the SIGNER made. It is
// not a permission, and no code in this client may treat it as one. Every rule that actually binds -
// who may speak, who may enter a lobby - is enforced by the relay against the durable `sub`, because
// a role the CLIENT decides is a COLOUR: anyone can patch a client to paint themselves orange, and a
// patched client is exactly what the rule exists to stop.
//
// So the only legitimate use of this value in the engine is PRESENTATION - which decoration to draw
// round a name, and which colour to draw a chat row in. If a future edit reaches for it to decide
// whether an action is allowed, the design has been inverted and the check belongs at the relay.
//
// THE WIRE FORM IS ONE LOWER-CASE LETTER in field 5, and an ordinary player has NO field 5 at all -
// so the assertion almost everybody holds is byte-identical to what was minted before roles existed.
enum GXAssertRole
{
	GX_ROLE_PLAYER = 0,		///< the overwhelming majority; also what an absent or unknown field means
	GX_ROLE_MOD,			///< wire 'm'
	GX_ROLE_ADMIN			///< wire 'a'
};

enum
{
	GX_ASSERTION_VERSION      = 1,
	GX_ASSERTION_MAX          = 256,		///< bytes, including the signature
	GX_ASSERTION_SUB_MAX      = 20,
	GX_ASSERTION_NAME_MAX     = 20,
	GX_ASSERTION_ROLE_MAX     = 1,			///< one lower-case letter; see GXAssertRole

	// Both directions, and generous on purpose. Two machines nobody controls agreeing to within a
	// minute is a weaker assumption than it looks, and the failure mode of getting this wrong is
	// "this player cannot log in and has no idea why". tools/master/gxtoken.js uses the same 60 s
	// for the same reason. A wrong clock still plays - it simply plays unauthenticated.
	GX_ASSERTION_SKEW_S       = 60,

	// 30 days. An assertion has to survive a plane trip with no internet, so it cannot be minted
	// with the 120 s life an admission ticket gets. It is safe to be this long because AN
	// ASSERTION IS NOT ADMISSION: it says an account holds a name, and nothing else. Bans and
	// admission control are enforced at join time by the relay and the master, so a stale
	// assertion does not extend a banned player's reach. The cap is enforced by the CLIENT as well
	// as the signer, so a signer bug - or a compromised signer minting ten-year assertions - is
	// bounded by code that has already shipped.
	GX_ASSERTION_MAX_LIFETIME_S = 30 * 24 * 3600,

	// GeneralsX @feature Claude 23/09/2026 A BOT login's cap: 48 h, the relay's BOT_MAX_LIFETIME_S. A
	// bot's key sits on an unattended machine, so its assertion is minted for 24 h and re-minted on a
	// timer; the cap bounds a stolen one. See verifyBot below.
	GX_BOT_MAX_LIFETIME_S = 48 * 3600,

	// GeneralsX @feature Claude 23/09/2026 How often a running bot looks at its file for a renewal. See
	// botRecheckDue below.
	GX_BOT_RECHECK_S = 60
};

// The filename the assertion is stored under, beside GXPlayer.ini in the user data directory.
//
// IT IS DEFINED HERE BECAUSE TWO PROGRAMS HAVE TO AGREE ON IT. The Windows launcher writes this
// file and the game reads it; they are separate builds with separate build systems, and a string
// duplicated across that boundary is a string that drifts. When it drifts the symptom is not an
// error - the game simply never sees a login, forever, on the one platform that cannot log in any
// other way.
#define GX_ASSERTION_FILENAME "GXLogin.txt"

struct GXAssertionClaims
{
	// TREAT sub AS AN OPAQUE STRING. It is validated as decimal digits because today's provider is
	// Discord, but that is a POLICY at one gate, not a promise about the type. A 20-digit snowflake
	// does not fit in a signed 64-bit integer, so parsing it numerically is an overflow waiting for
	// a popular account; and loosening the charset for a second provider later must not have to
	// hunt down arithmetic. Compare it, store it, print it. Never atoi it.
	char      sub[GX_ASSERTION_SUB_MAX + 1];
	char      name[GX_ASSERTION_NAME_MAX + 1];
	long long iat;
	long long exp;

	// GeneralsX @feature Claude 11/08/2026 GX_ROLE_PLAYER whenever field 5 is absent, empty, longer
	// than one character, or a letter this build does not know.
	//
	// FAIL TO THE LEAST PRIVILEGE, NEVER REFUSE OVER A ROLE. Refusing an unknown letter would mean
	// that the day a third role is minted, every client built before it stops recognising its
	// holder's login entirely - which is exactly the breakage the append-before-the-signature rule
	// exists to prevent. Being wrong in this direction costs a moderator their colour until they
	// update; being wrong in the other direction costs them their account.
	// GeneralsX @bugfix wh1ter0se 12/08/2026 `enum` KEYWORD REQUIRED - this header is included by C.
	//
	// A bare `GXAssertRole role;` is valid C++ and invalid C, and gxlaunch.c is C. Without the
	// keyword the C compiler does not recognise the type, the whole struct fails to parse, and every
	// later use of GXAssertionClaims collapses ("left of '.exp' must have struct/union type") - so
	// the real error is nowhere near the real cause. The game builds either way because it is all
	// C++, which is exactly why the four-platform verify stayed green while the launcher CI went red.
	enum GXAssertRole role;
};

#ifdef __cplusplus

class GXIdentityAssertion
{
public:
	// Verify against a caller-supplied key. Pure: no globals, no file access, and the clock is a
	// PARAMETER rather than a call to time() - which is what makes expiry and not-yet-valid
	// testable at all, and is the same trick scripts/test/lan-start-policy-test.cpp uses on the
	// LAN start policy.
	//
	// `text` is a C string; embedded NULs therefore truncate it, and a truncated payload cannot
	// verify. `nowS` is unix seconds.
	//
	// *out IS WRITTEN ONLY ON GX_ASSERT_OK. On every refusal it is left exactly as the caller left
	// it. That is deliberate: it means a caller who forgets to check the status still cannot read a
	// display name out of an expired or forged assertion, which turns a sloppy call site into a
	// harmless one instead of an exploitable one. `out` may be NULL if only the status is wanted.
	static GXAssertStatus verifyWithKey(const unsigned char pk[32], const char *text,
	                                    long long nowS, GXAssertionClaims *out);

	// Verify against the compiled-in trust anchor. Returns GX_ASSERT_NOKEY - refusing everything -
	// until Phase 3b mints a key and it is pinned in the .cpp.
	static GXAssertStatus verify(const char *text, long long nowS, GXAssertionClaims *out);

	// Read one assertion from a file and verify it. A missing or empty file is GX_ASSERT_ABSENT.
	// Only the first line is read, trailing whitespace is stripped, and anything larger than
	// GX_ASSERTION_MAX is refused without being parsed.
	//
	// GeneralsX @bugfix Claude 23/09/2026 `text`, when not NULL, receives the line that was VERIFIED
	// (GX_ASSERTION_MAX + 1 bytes) and, like *out, is written only on GX_ASSERT_OK. A caller that sends
	// the assertion on sends THESE bytes: reading the file again for them can return a file swapped in
	// between, which nothing checked (review P3-1).
	static GXAssertStatus loadFileWithKey(const unsigned char pk[32], const char *path,
	                                      long long nowS, GXAssertionClaims *out, char *text = NULL);
	static GXAssertStatus loadFile(const char *path, long long nowS, GXAssertionClaims *out,
	                               char *text = NULL);

	// GeneralsX @feature Claude 23/09/2026 BOT LOGINS (docs/superpowers/plans/2026-09-23-bot-identity.md).
	//
	//     1~<sub>~<name>~<iat>~<exp>~b~<pub43>~<sig86>
	//
	// Exactly eight fields; field 5 is the letter b; field 6 is the device's Ed25519 public key in
	// canonical base64url, and the signature is checked under THAT key. So this proves only that the
	// file is intact and self-consistent - it is never trust: the relay admits the account only when
	// its root-owned registry maps <sub> to that key. It exists so a corrupt or foreign file is never
	// SENT. On top of verifyWithKey: the account must be 98 + 18 digits (above 2^64-1, so never a
	// Discord id), the lifetime at most GX_BOT_MAX_LIFETIME_S, and the role is always GX_ROLE_PLAYER.
	// Same *out contract as verifyWithKey: written only on GX_ASSERT_OK, and `out` may be NULL.
	static GXAssertStatus verifyBot(const char *text, long long nowS, GXAssertionClaims *out);
	// The first line of `path`, read exactly as loadFileWithKey reads it, then verifyBot. `text` as above.
	static GXAssertStatus loadBotFile(const char *path, long long nowS, GXAssertionClaims *out,
	                                  char *text = NULL);
	// GX_IDENTITY_FILE's path rules, pure so a test can drive them. An absolute path (a leading / or \,
	// or a drive letter) is used as-is, EXCEPT a leading pair of separators - \\srv or //srv - or a
	// separator then '?' - \??\UNC\srv - which are refused, because Windows would authenticate to that
	// server to open it. A bare filename is joined onto userDataDir (which ends in a separator): that is
	// how the iPad, one app container, is given one. Anything else with a separator in it is refused, as
	// is a result that would not fit `cap` bytes with its NUL. `out` is written only on true.
	static bool resolveBotPath(const char *env, const char *userDataDir, char *out, size_t cap);
	// GeneralsX @bugfix Claude 23/09/2026 THE BOT RELOAD'S DECISION, pure so gxidtest drives it (review P2:
	// it keeps the standing host's 24 h login alive across the re-mint, and a dead one passed every test).
	// botRecheckDue: the first call, then at most once per GX_BOT_RECHECK_S - a clock that stepped back
	// re-arms - and records `now` in *checkedAt when due. NULL is never due.
	// botReloadNeeded: the file's first line differs from the held line, or the held login has expired by
	// the verifier's own rule (now - GX_ASSERTION_SKEW_S > heldExp; heldExp 0 = nothing held). NULL reads "".
	static bool botRecheckDue(long long now, long long *checkedAt);
	static bool botReloadNeeded(const char *fileLine, const char *held, long long heldExp, long long now);

	// FALSE until a real public key is compiled in. Callers should not need this - verify() already
	// fails closed - but a launcher or a diagnostic wants to say "accounts are not configured in
	// this build" rather than "your login is invalid".
	static bool keyIsPinned(void);

	// A stable, distinct short name per status, for logs and for test output. Never NULL.
	static const char *statusName(GXAssertStatus s);

	// The same, per role. Never NULL, and total over the enum - an unknown value reads "player",
	// which is the same direction the parser fails in.
	static const char *roleName(GXAssertRole r);
};

extern "C" {
#endif	// __cplusplus

// THE LAUNCHER'S DOOR INTO THIS FILE, and the reason it is a door rather than a copy.
//
// scripts/build/windows/launcher/gxlaunch.c is C, is built by its own .cmd with no CMake, and is
// the program that FETCHES the assertion on Windows - because the Windows engine has no HTTPS at
// all. It must not write a document the game will then refuse, so it has to run the same checks
// the game runs: the same field charsets, the same clock rules, the same lifetime cap, above all
// the same pinned key.
//
// The alternative was a second, simpler check in the launcher - "it looks like six fields, ship
// it". That is how a launcher ends up writing an assertion the game rejects, on a player's
// machine, with the launcher reporting success. One verifier, one pin, one set of test vectors:
// the same argument that moved GXEd25519 out of the launcher directory in Phase 3a.
//
// These mirror the class exactly, including the rule that *out is written ONLY on GX_ASSERT_OK.
// The status is returned as int because GXAssertStatus is a C enum here and an int there, and
// spelling it int at the boundary is honest about that.

int gx_identity_verify_text(const char *text, long long nowS, struct GXAssertionClaims *out);
int gx_identity_key_pinned(void);
const char *gx_identity_status_name(int status);

#ifdef __cplusplus
}	// extern "C"
#endif

#endif
