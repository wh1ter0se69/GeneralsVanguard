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

// Identity assertion verification (format and verify-only rationale: see the header).
// Check order is the security property: shape only -> SIGNATURE -> meaning -> clock (as tools/master/gxtoken.js).
// Nothing attacker-chosen is interpreted before the signature check; signed but malformed assertions are refused.
// No PreRTS.h and no allocation: every buffer is fixed-size.

#include "GameClient/GXIdentityAssertion.h"
#include "GameClient/GXEd25519.h"
#include "GameClient/GXScopedWipe.h"	// the stack and stdio copies of the assertion are wiped

#include <stdio.h>
#include <string.h>

namespace
{

const char SEP = '~';

// 64 signature bytes is 512 bits; base64url without padding is ceil(512/6) = 86 characters.
const size_t SIG_TEXT_LEN = 86;

// TRUST ANCHOR: an all-zero key is the "unset" sentinel, and keyIsPinned() checks for it explicitly rather than relying
// on gx_ed25519_verify rejecting the all-zero (small-order) point. The key below is pinned.
// The assertion key must differ from the manifest's, or a compromise of either is a compromise of both.
// Generate it with `gx-manifest.py pubkey --c-array`, not by typing.
/* Ed25519 public key of the GeneralsX identity signer, from scripts/tooling/mint-identity-key.py.
 * base64url: RDybn5Cr2920FQPaqlW49q0v1fY4U-oFqzl3VbJAuq4
 * The private half lives only in the Worker's wrangler secrets (write-only) and the operator's password manager.
 * NOT GX_MANIFEST_PUBKEY (which binary to download): this key says who the player is; the key dirs are separate. */
const unsigned char GX_IDENTITY_PUBKEY[32] = {
	0x44, 0x3c, 0x9b, 0x9f, 0x90, 0xab, 0xdb, 0xdd,
	0xb4, 0x15, 0x03, 0xda, 0xaa, 0x55, 0xb8, 0xf6,
	0xad, 0x2f, 0xd5, 0xf6, 0x38, 0x53, 0xea, 0x05,
	0xab, 0x39, 0x77, 0x55, 0xb2, 0x40, 0xba, 0xae,
};

bool isDigit(char c)   { return c >= '0' && c <= '9'; }

// The relay's NICK_RE, [A-Za-z0-9_.-] (tools/master/store.js:81). Applied on the client because a
// guarantee has to hold for a document that did not come from the stock signer, and because a name
// that escaped this would later be re-emitted into a space-separated, '~'-delimited relay line and
// corrupt it for every client in the room, not only for its owner - the same argument
// GXPlayerIdentity::isValidPlayerId makes about ids.
bool isNickChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c)
		|| c == '_' || c == '.' || c == '-';
}

int b64urlValue(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-') return 62;
	if (c == '_') return 63;
	return -1;		// '+' and '/' are standard base64, not base64url, and are refused
}

/*
 * Strict base64url of exactly `chars` characters into exactly `bytes` bytes.
 *
 * CANONICAL ENCODINGS ONLY. 86 characters carry 516 bits and a signature is 512, so the final
 * character's low 4 bits MUST be zero; an encoder that leaves rubbish there produces a second
 * spelling of the same signature. Refusing it is the same stance GXEd25519.h:52-57 takes on point
 * encodings, and for the same reason: every additional accepted spelling is a second name for one
 * object, and code elsewhere that compares spellings then draws a wrong conclusion.
 *
 * No padding is accepted either. There is exactly one way to write a valid signature.
 *
 * Generalised from the signature-only decoder for the bot
 * login's public key (43 characters, 32 bytes, two leftover bits) - one decoder, one canonical rule.
 */
bool b64urlDecode(const char *text, size_t chars, unsigned char *out, size_t bytes)
{
	unsigned int acc = 0;
	int bits = 0;
	size_t o = 0;

	for (size_t i = 0; i < chars; ++i)
	{
		const int v = b64urlValue(text[i]);
		if (v < 0)
			return false;
		acc = (acc << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			if (o >= bytes)
				return false;
			out[o++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	if (o != bytes)
		return false;
	// The leftover bits of the last character: 86 * 6 = 516 against 512, 43 * 6 = 258 against 256.
	if ((acc & ((1u << bits) - 1u)) != 0)
		return false;
	return true;
}

bool b64urlDecodeSig(const char *text, unsigned char out[64])
{
	return b64urlDecode(text, SIG_TEXT_LEN, out, 64);
}

/*
 * Decimal to long long, for a field already known to be digits-only and short.
 *
 * Bounded at 11 characters by the caller, so the largest value is 99999999999 - eleven digits, well
 * inside a 64-bit signed integer, with no overflow path to reason about. Eleven rather than ten
 * because unix seconds pass 10 digits in 2286 and there is no reason to build in an end date; the
 * bound exists to stop a 4000-digit field, not to express a calendar opinion.
 */
long long parseDecimal(const char *s, size_t len)
{
	long long v = 0;
	for (size_t i = 0; i < len; ++i)
		v = v * 10 + (s[i] - '0');
	return v;
}

bool keyIsAllZero(const unsigned char pk[32])
{
	unsigned char acc = 0;
	for (int i = 0; i < 32; ++i)
		acc = (unsigned char)(acc | pk[i]);
	return acc == 0;
}

/*
 * The first line of `path` into buf, trailing whitespace stripped: ABSENT for no file or an empty
 * line, LENGTH for a first line over GX_ASSERTION_MAX, otherwise OK with buf NUL-terminated. The CALLER
 * owns buf and wipes it; the stdio buffer is this function's and is wiped here.
 *
 * Extracted from loadFileWithKey, unchanged, so the bot loader
 * reads a file exactly the way the player loader does rather than through a second reader.
 */
GXAssertStatus readAssertionLine(const char *path, char buf[GX_ASSERTION_MAX + 2])
{
	if (path == NULL)
		return GX_ASSERT_ABSENT;

	// The stream's buffer is OURS, and wiped. Left to itself stdio
	// reads the file into a buffer it allocates and frees at fclose with the bytes still in it - a heap
	// copy of the whole sign-in that no sign-out can reach. Declared before the stream so it outlives it.
	char iobuf[512];
	GXScopedWipe wipeIo(iobuf, sizeof(iobuf));

	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
		return GX_ASSERT_ABSENT;	// no file is the normal unauthenticated state, not a failure
	setvbuf(fp, iobuf, _IOFBF, sizeof(iobuf));	// before ANY read, or it is too late to matter

	// One byte more than the maximum, so an over-long file is DETECTED rather than silently
	// truncated into something that might parse. Truncating here would be the worst kind of bug:
	// the prefix of a valid assertion cannot verify, so it would look like a signature failure and
	// send whoever debugged it after the signer. GX_ASSERTION_MAX + 1 is the caller's buffer less its NUL.
	const size_t got = fread(buf, 1, GX_ASSERTION_MAX + 1, fp);
	buf[got] = '\0';

	// Only the first line. A file may reasonably grow a second line later - a comment, a second
	// account - and this must not start failing when it does.
	char *nl = (char *)memchr(buf, '\n', got);

	// If the first line ran past the buffer, it is over-length only if what remains before the
	// newline (or EOF) is real content. A maximum-length assertion whose CRLF or trailing spaces
	// straddle the buffer edge is legal - the trim below restores it to the cap - and refusing it
	// here would reject a valid signed assertion for its line ending. The scan stops at the first
	// newline, so a long second line costs nothing.
	bool overlong = false;
	if (nl == NULL)
	{
		for (int ch = fgetc(fp); ch != EOF && ch != '\n'; ch = fgetc(fp))
		{
			if (ch != '\r' && ch != ' ' && ch != '\t')
			{
				overlong = true;
				break;
			}
		}
	}
	fclose(fp);

	size_t len = (nl != NULL) ? (size_t)(nl - buf) : got;

	while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t'))
		--len;
	buf[len] = '\0';

	if (len == 0)
		return GX_ASSERT_ABSENT;	// an empty file: signed out, or an interrupted write
	// Over-length is only a real over-length if the first line itself ran past the cap. A large
	// file whose first line is short is fine.
	if (len > GX_ASSERTION_MAX || overlong)
		return GX_ASSERT_LENGTH;

	return GX_ASSERT_OK;
}

// Hand back the line that was VERIFIED, on OK only, for a caller that
// will send it. Reading the file again for the bytes could return a file swapped in between that
// nothing checked. readAssertionLine has already bounded buf to GX_ASSERTION_MAX.
GXAssertStatus keepVerified(GXAssertStatus st, const char *buf, char *text)
{
	if (st == GX_ASSERT_OK && text != NULL)
		memcpy(text, buf, strlen(buf) + 1);
	return st;
}

} // anonymous namespace

GXAssertStatus GXIdentityAssertion::verifyWithKey(const unsigned char pk[32], const char *text,
                                                  long long nowS, GXAssertionClaims *out)
{
	// ---- 1. nothing to check ------------------------------------------------------------------
	// The normal state for every LAN and offline player, forever. Not an error.
	if (pk == NULL || text == NULL || text[0] == '\0')
		return GX_ASSERT_ABSENT;

	// ---- 2. length, before anything walks the string ------------------------------------------
	// strnlen rather than strlen: an unterminated buffer must cost a bounded read, not a scan to
	// the end of the address space.
	size_t len = 0;
	while (len <= GX_ASSERTION_MAX && text[len] != '\0')
		++len;
	if (len > GX_ASSERTION_MAX)
		return GX_ASSERT_LENGTH;

	// ---- 3. character class ------------------------------------------------------------------
	// Printable ASCII only, and no space. Every field charset in the format already excludes
	// whitespace, so anything here is either corruption or an attempt to smuggle a separator past
	// a downstream consumer - a newline into a log, a space into a relay line. Refused as a class
	// rather than field by field, because the fields have not been established yet.
	for (size_t i = 0; i < len; ++i)
	{
		const unsigned char c = (unsigned char)text[i];
		if (c < 0x21 || c > 0x7E)
			return GX_ASSERT_MALFORMED;
	}

	// ---- 4. field count ----------------------------------------------------------------------
	// Six is the minimum: version, sub, name, iat, exp, signature. More is legal and means a newer
	// signer appended a field before the signature - the manifest and gxtoken both use that rule.
	// The extras are ignored, NOT refused, which is what lets a signer add a field without every
	// client having to be rebuilt first.
	const char *field[16];
	size_t      flen[16];
	int         nf = 0;
	{
		const char *start = text;
		for (size_t i = 0; i <= len; ++i)
		{
			if (i == len || text[i] == SEP)
			{
				if (nf >= 16)
					return GX_ASSERT_FIELDS;	// absurdly many fields; nothing legitimate is close
				field[nf] = start;
				flen[nf] = (size_t)(text + i - start);
				++nf;
				start = text + i + 1;
			}
		}
	}
	if (nf < 6)
		return GX_ASSERT_FIELDS;

	// ---- 5. version --------------------------------------------------------------------------
	// Refused rather than guessed at. A newer signer may mean the same bytes differently, and
	// misreading an identity claim is the failure that must never be silent.
	if (flen[0] != 1 || field[0][0] != ('0' + GX_ASSERTION_VERSION))
		return GX_ASSERT_VERSION_BAD;

	// ---- 6. signature shape ------------------------------------------------------------------
	const char * const sigText = field[nf - 1];
	if (flen[nf - 1] != SIG_TEXT_LEN)
		return GX_ASSERT_SIGSHAPE;
	// Wiped on every one of the returns below. The decoded signature
	// is the part of the token nobody can recompute: with the claims beside it, which the caller keeps,
	// it IS the token.
	unsigned char sig[64];
	GXScopedWipe wipeSig(sig, sizeof(sig));
	if (!b64urlDecodeSig(sigText, sig))
		return GX_ASSERT_SIGSHAPE;

	// ---- 7. THE SIGNATURE --------------------------------------------------------------------
	// The payload is every byte before the final separator, verbatim, exactly as it was signed. No
	// re-serialisation and no canonical form to agree on with the signer - that is the entire point
	// of a delimited wire form, and it is what gxlaunch.c:610-613 says about the manifest.
	const size_t payloadLen = (size_t)(sigText - text) - 1;
	if (!gx_ed25519_verify(pk, sig, text, payloadLen))
		return GX_ASSERT_SIGNATURE;

	// ==========================================================================================
	// Everything below this line is signed data. It is still not trusted data.
	// ==========================================================================================

	// ---- 8. field charsets and lengths -------------------------------------------------------
	if (flen[1] < 1 || flen[1] > GX_ASSERTION_SUB_MAX)
		return GX_ASSERT_MALFORMED;
	for (size_t i = 0; i < flen[1]; ++i)
		if (!isDigit(field[1][i]))
			return GX_ASSERT_MALFORMED;

	if (flen[2] > GX_ASSERTION_NAME_MAX)		// may be empty
		return GX_ASSERT_MALFORMED;
	for (size_t i = 0; i < flen[2]; ++i)
		if (!isNickChar(field[2][i]))
			return GX_ASSERT_MALFORMED;

	for (int f = 3; f <= 4; ++f)
	{
		if (flen[f] < 1 || flen[f] > 11)
			return GX_ASSERT_MALFORMED;
		for (size_t i = 0; i < flen[f]; ++i)
			if (!isDigit(field[f][i]))
				return GX_ASSERT_MALFORMED;
	}

	// ---- 8b. the role, which is optional and is never fatal ----
	// Field 5, before the signature, so it is signed data: splicing a role into a captured assertion fails the signature.
	// NOT A REFUSAL PATH: an unknown letter means a newer signer, read as "ordinary player"; refusing would log out every
	// older client when a third role is minted. Empty, over-long and wrong-case land there too (one spelling per role).
	GXAssertRole role = GX_ROLE_PLAYER;
	if (nf >= 7 && flen[5] == (size_t)GX_ASSERTION_ROLE_MAX)
	{
		if (field[5][0] == 'm')
			role = GX_ROLE_MOD;
		else if (field[5][0] == 'a')
			role = GX_ROLE_ADMIN;
	}

	const long long iat = parseDecimal(field[3], flen[3]);
	const long long exp = parseDecimal(field[4], flen[4]);

	// ---- 9. lifetime -------------------------------------------------------------------------
	// Checked BEFORE the clock, on purpose: a nonsensical window is a fact about the document and
	// is true regardless of what this machine thinks the time is, so it should not be reported as
	// "expired" to a player whose clock happens to be wrong.
	if (exp <= iat)
		return GX_ASSERT_LIFETIME;
	if (exp - iat > GX_ASSERTION_MAX_LIFETIME_S)
		return GX_ASSERT_LIFETIME;

	// ---- 10. the clock, both directions ------------------------------------------------------
	// Generous skew in both directions - see the header. A machine with a wrong clock loses its
	// account for the session and plays anyway; it does not lose the game.
	if (nowS + GX_ASSERTION_SKEW_S < iat)
		return GX_ASSERT_NOTYET;
	if (nowS - GX_ASSERTION_SKEW_S > exp)
		return GX_ASSERT_EXPIRED;

	// ---- 11. accepted ------------------------------------------------------------------------
	// ONLY NOW is anything written to the caller. See the header: this is what makes a call site
	// that ignores the return value harmless rather than exploitable.
	if (out != NULL)
	{
		memset(out, 0, sizeof(*out));
		memcpy(out->sub, field[1], flen[1]);
		out->sub[flen[1]] = '\0';
		memcpy(out->name, field[2], flen[2]);
		out->name[flen[2]] = '\0';
		out->iat = iat;
		out->exp = exp;
		out->role = role;
	}
	return GX_ASSERT_OK;
}

const char *GXIdentityAssertion::roleName(GXAssertRole r)
{
	switch (r)
	{
		case GX_ROLE_PLAYER: return "player";
		case GX_ROLE_MOD:    return "mod";
		case GX_ROLE_ADMIN:  return "admin";
	}
	// Unreachable through the parser, which emits no other value. Total anyway, and it fails in the
	// same direction everything else here does.
	return "player";
}

bool GXIdentityAssertion::keyIsPinned(void)
{
	return !keyIsAllZero(GX_IDENTITY_PUBKEY);
}

GXAssertStatus GXIdentityAssertion::verify(const char *text, long long nowS,
                                           GXAssertionClaims *out)
{
	// FAILS CLOSED WHEN UNCONFIGURED. A build with no trust anchor cannot establish that anything
	// was signed by anyone, so it refuses everything rather than accepting everything. The opposite
	// default is the one that ships by accident and is never noticed, because it looks exactly like
	// the feature working - tools/master/gxtoken.js:264-271 makes the same argument about a relay
	// that has not been told its own id.
	if (!keyIsPinned())
		return GX_ASSERT_NOKEY;
	return verifyWithKey(GX_IDENTITY_PUBKEY, text, nowS, out);
}

GXAssertStatus GXIdentityAssertion::loadFileWithKey(const unsigned char pk[32], const char *path,
                                                    long long nowS, GXAssertionClaims *out, char *text)
{
	// WIPED ON EVERY RETURN - the empty file, the over-long one,
	// and after verification. It holds the whole assertion, a bearer token.
	char buf[GX_ASSERTION_MAX + 2];
	GXScopedWipe wipeBuf(buf, sizeof(buf));
	const GXAssertStatus st = readAssertionLine(path, buf);
	return st != GX_ASSERT_OK ? st : keepVerified(verifyWithKey(pk, buf, nowS, out), buf, text);
}

GXAssertStatus GXIdentityAssertion::loadFile(const char *path, long long nowS,
                                             GXAssertionClaims *out, char *text)
{
	if (!keyIsPinned())
		return GX_ASSERT_NOKEY;
	return loadFileWithKey(GX_IDENTITY_PUBKEY, path, nowS, out, text);
}

GXAssertStatus GXIdentityAssertion::verifyBot(const char *text, long long nowS, GXAssertionClaims *out)
{
	// A BOT assertion - see the header. Self-verified ONLY so a corrupt
	// file is never sent: the relay's registry decides whether this key may speak for this account.
	if (text == NULL || text[0] == '\0')
		return GX_ASSERT_ABSENT;
	// Shape first, as verifyWithKey does: separators only, nothing interpreted. t[i] is the i-th '~'.
	// Bounded like verifyWithKey's own scan, so an unterminated buffer costs a bounded read.
	const char *t[8] = { 0 };										// initialised: the launcher builds this at /W4 /WX
	int n = 0;
	for (size_t i = 0; i <= GX_ASSERTION_MAX && text[i] != '\0'; ++i)
	{
		if (text[i] != SEP)
			continue;
		if (n == 8)
			return GX_ASSERT_FIELDS;
		t[n++] = text + i;
	}
	if (n != 7)
		return GX_ASSERT_FIELDS;										// exactly eight fields
	// Field 5 is EXACTLY "b", which verifyWithKey reads as a player - THIS is what keeps a bot from being a
	// moderator or an admin (I1). A later `role = PLAYER` override used to sit below and could never fire;
	// gxidtest's "role is player" rows now test this line, and its role 'a' and 'x' rows are refused here.
	if (t[5] - t[4] != 2 || t[4][1] != 'b' || t[6] - t[5] != 44)
		return GX_ASSERT_MALFORMED;										// field 5 is "b", field 6 is 43 characters
	unsigned char pk[32];
	if (!b64urlDecode(t[5] + 1, 43, pk, 32))
		return GX_ASSERT_MALFORMED;										// canonical base64url only
	// Into a LOCAL, copied out only on OK: the header promises *out is untouched on every refusal, and
	// verifyWithKey's OK is not yet this function's OK.
	GXAssertionClaims c;
	const GXAssertStatus st = verifyWithKey(pk, text, nowS, &c);
	if (st != GX_ASSERT_OK)
		return st;
	// 98 + 18 digits: above 2^64-1, so never a Discord id. verifyWithKey has already required digits.
	if (strlen(c.sub) != 20 || c.sub[0] != '9' || c.sub[1] != '8')
		return GX_ASSERT_MALFORMED;
	if (c.exp - c.iat > GX_BOT_MAX_LIFETIME_S)
		return GX_ASSERT_LIFETIME;
	if (out != NULL)													// NULL is allowed: status only (header)
		*out = c;
	return GX_ASSERT_OK;
}

GXAssertStatus GXIdentityAssertion::loadBotFile(const char *path, long long nowS, GXAssertionClaims *out,
                                                char *text)
{
	char buf[GX_ASSERTION_MAX + 2];
	GXScopedWipe wipeBuf(buf, sizeof(buf));
	const GXAssertStatus st = readAssertionLine(path, buf);
	return st != GX_ASSERT_OK ? st : keepVerified(verifyBot(buf, nowS, out), buf, text);
}

bool GXIdentityAssertion::resolveBotPath(const char *env, const char *userDataDir, char *out, size_t cap)
{
	// See the header. Both separators on every platform: Windows
	// takes either, and a rule that only knew one would let the other through.
	if (env == NULL || env[0] == '\0' || out == NULL || cap == 0)
		return false;
	const bool sep0 = env[0] == '/' || env[0] == '\\';
	// And \??\UNC\srv\share: Windows hands a \??\ path to
	// the NT layer as-is, which reaches the same server. '?' is never in a real file name.
	if (sep0 && (env[1] == '/' || env[1] == '\\' || env[1] == '?'))
		return false;													// \\srv, //srv, \/srv, \??\UNC: a server
	const bool drive = ((env[0] >= 'A' && env[0] <= 'Z') || (env[0] >= 'a' && env[0] <= 'z'))
		&& env[1] == ':' && (env[2] == '/' || env[2] == '\\');
	const char *dir = "";
	if (!sep0 && !drive)
	{
		if (strchr(env, '/') != NULL || strchr(env, '\\') != NULL || userDataDir == NULL)
			return false;												// relative with a separator: refused
		dir = userDataDir;
	}
	const size_t dl = strlen(dir), el = strlen(env);
	if (dl + el + 1 > cap)
		return false;
	memcpy(out, dir, dl);
	memcpy(out + dl, env, el + 1);
	return true;
}

bool GXIdentityAssertion::botRecheckDue(long long now, long long *checkedAt)
{
	if (checkedAt == NULL)
		return false;
	if (*checkedAt != 0 && now >= *checkedAt && now - *checkedAt < GX_BOT_RECHECK_S)
		return false;
	*checkedAt = now;
	return true;
}

bool GXIdentityAssertion::botReloadNeeded(const char *fileLine, const char *held, long long heldExp,
                                          long long now)
{
	if (strcmp(fileLine != NULL ? fileLine : "", held != NULL ? held : "") != 0)
		return true;
	return heldExp != 0 && now - GX_ASSERTION_SKEW_S > heldExp;
}

const char *GXIdentityAssertion::statusName(GXAssertStatus s)
{
	switch (s)
	{
		case GX_ASSERT_OK:          return "OK";
		case GX_ASSERT_ABSENT:      return "ABSENT";
		case GX_ASSERT_LENGTH:      return "LENGTH";
		case GX_ASSERT_FIELDS:      return "FIELDS";
		case GX_ASSERT_VERSION_BAD: return "VERSION_BAD";
		case GX_ASSERT_SIGSHAPE:    return "SIGSHAPE";
		case GX_ASSERT_SIGNATURE:   return "SIGNATURE";
		case GX_ASSERT_MALFORMED:   return "MALFORMED";
		case GX_ASSERT_LIFETIME:    return "LIFETIME";
		case GX_ASSERT_NOTYET:      return "NOTYET";
		case GX_ASSERT_EXPIRED:     return "EXPIRED";
		case GX_ASSERT_NOKEY:       return "NOKEY";
	}
	// No default label above, so a new status added to the enum is a compiler warning here rather
	// than a silent "?" in a log six months later.
	return "?";
}

// ------------------------------------------------------------------------------------------------
// The C seam. See the header for why the launcher gets the real verifier rather than its own.
// Three thin wrappers and nothing else - a seam with logic in it is a second implementation
// wearing a smaller hat.
// ------------------------------------------------------------------------------------------------

extern "C" int gx_identity_verify_text(const char *text, long long nowS, GXAssertionClaims *out)
{
	return (int)GXIdentityAssertion::verify(text, nowS, out);
}

extern "C" int gx_identity_key_pinned(void)
{
	return GXIdentityAssertion::keyIsPinned() ? 1 : 0;
}

extern "C" const char *gx_identity_status_name(int status)
{
	return GXIdentityAssertion::statusName((GXAssertStatus)status);
}
