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

// The signed identity assertion. The client only VERIFIES (the Windows engine has no HTTPS: a service returns a
// signed assertion the launcher writes beside GXPlayer.ini). Fetching must never arrive in this file.
// 1~<sub>~<name>~<iat>~<exp>[~<role>]~<sig>: unknown versions are REFUSED; Ed25519 covers all bytes before '~<sig>'.
// LAN and offline play never require a login: ABSENT is normal. Not digested; GameLogic must not read it.

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

// The role the signer claims this account holds: a claim, NOT a permission. Rules that bind (who may speak,
// enter a lobby) are enforced by the relay against the durable `sub`; a role the CLIENT decides is only a
// COLOUR. Use it for PRESENTATION only. Wire form: ONE lower-case letter in field 5; an ordinary player has no
// field 5, so most assertions are byte-identical to ones minted before roles existed.
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
	// for the same reason. A wrong clock cannot sign in: sign-in is required on the public relay. The refusal says so
	// (GXLoginClockText.h), and the server mints with iat 27 h in the past so a slow clock still verifies.
	GX_ASSERTION_SKEW_S       = 60,

	// 30 days. An assertion has to survive a plane trip with no internet, so it cannot be minted
	// with the 120 s life an admission ticket gets. It is safe to be this long because AN
	// ASSERTION IS NOT ADMISSION: it says an account holds a name, and nothing else. Bans and
	// admission control are enforced at join time by the relay and the master, so a stale
	// assertion does not extend a banned player's reach. The cap is enforced by the CLIENT as well
	// as the signer, so a signer bug - or a compromised signer minting ten-year assertions - is
	// bounded by code that has already shipped.
	GX_ASSERTION_MAX_LIFETIME_S = 30 * 24 * 3600,

	// A BOT login's cap: 48 h, the relay's BOT_MAX_LIFETIME_S. A
	// bot's key sits on an unattended machine, so its assertion is minted for 24 h and re-minted on a
	// timer; the cap bounds a stolen one. See verifyBot below.
	GX_BOT_MAX_LIFETIME_S = 48 * 3600,

	// How often a running bot looks at its file for a renewal. See
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

	// GX_ROLE_PLAYER whenever field 5 is absent, empty, longer than one character, or a letter this build does not
	// know. FAIL TO THE LEAST PRIVILEGE, NEVER REFUSE OVER A ROLE: refusing an unknown letter would log out every
	// older client when a third role is minted. `enum` KEYWORD REQUIRED - this header is included by C (gxlaunch.c),
	// where a bare `GXAssertRole role;` is invalid.
	enum GXAssertRole role;
};

#ifdef __cplusplus

class GXIdentityAssertion
{
public:
	// Verify against a caller-supplied key. Pure: no globals, no file access, and the clock is a PARAMETER (nowS,
	// unix seconds). `text` is a C string: embedded NULs truncate it and a truncated payload cannot verify.
	// *out IS WRITTEN ONLY ON GX_ASSERT_OK and left as the caller left it on every refusal, so a caller who forgets
	// to check the status still cannot read a name out of a forged or expired assertion. `out` may be NULL.
	static GXAssertStatus verifyWithKey(const unsigned char pk[32], const char *text,
	                                    long long nowS, GXAssertionClaims *out);

	// Verify against the compiled-in trust anchor. Returns GX_ASSERT_NOKEY - refusing everything -
	// until Phase 3b mints a key and it is pinned in the .cpp.
	static GXAssertStatus verify(const char *text, long long nowS, GXAssertionClaims *out);

	// Read one assertion from a file and verify it. A missing or empty file is GX_ASSERT_ABSENT.
	// Only the first line is read, trailing whitespace is stripped, and anything larger than
	// GX_ASSERTION_MAX is refused without being parsed.
	//
	// `text`, when not NULL, receives the line that was VERIFIED
	// (GX_ASSERTION_MAX + 1 bytes) and, like *out, is written only on GX_ASSERT_OK. A caller that sends
	// the assertion on sends THESE bytes: reading the file again for them can return a file swapped in
	// between, which nothing checked.
	static GXAssertStatus loadFileWithKey(const unsigned char pk[32], const char *path,
	                                      long long nowS, GXAssertionClaims *out, char *text = NULL);
	static GXAssertStatus loadFile(const char *path, long long nowS, GXAssertionClaims *out,
	                               char *text = NULL);

	// BOT LOGINS.
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
	// THE BOT RELOAD'S DECISION, pure so gxidtest drives it: it keeps the standing host's 24 h login alive across the re-mint.
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

// THE LAUNCHER'S DOOR: gxlaunch.c (C, its own .cmd) FETCHES the assertion on Windows, where the engine has no
// HTTPS. It must not write a document the game will refuse, so it runs the same checks (charsets, clock,
// lifetime cap, pinned key): one verifier, one pin, one set of test vectors. *out is written ONLY on
// GX_ASSERT_OK. The status is returned as int across the C boundary.

int gx_identity_verify_text(const char *text, long long nowS, struct GXAssertionClaims *out);
int gx_identity_key_pinned(void);
const char *gx_identity_status_name(int status);

#ifdef __cplusplus
}	// extern "C"
#endif

#endif
