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

// GeneralsX @feature Claude 08/08/2026 Durable per-install player identity. See the header for the
// design and for the exact reason this is a bearer token rather than a keypair.
//
// DIGEST: GameClient/ is not in simsourcedigest_watcher.cmake's path list, so nothing in this file
// moves sourceID. Keep it that way - identity is social state, never simulation state.

// _CRT_RAND_S MUST be defined before ANY header that reaches <stdlib.h>, which on MSVC is what
// declares rand_s. Without it the Windows branch of cryptoRandom below fails to compile with
// "error C3861: 'rand_s': identifier not found" - and nothing else in the file changes, so the
// error reads as a missing library rather than a missing macro.
//
// This sat undetected because the Windows box was off while this file was written and every other
// platform takes the /dev/urandom branch. It is exactly the break the four-platform rule exists to
// catch: the file compiled clean on macOS, Linux and iOS and could never have compiled on Windows.
// Defined on the command line by Core/GameEngine/CMakeLists.txt, because a define here cannot work:
// PreRTS.h is a precompiled header and <stdlib.h> is already processed before this line compiles.
// Kept as a guarded fallback for any build that compiles this file without that target's
// definitions, and #ifndef so the normal path does not warn about redefinition.
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "PreRTS.h"
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXIdentityAssertion.h"
#include "GameClient/ChatRateLimit.h"	// setSharedExempt - the admin chat exemption
#include "GameClient/GXSignOutFiles.h"	// clearVerifiedIdentity: the file half, testable alone
#include "GameClient/GXScopedWipe.h"	// loadVerified: the re-read's stdio buffer is wiped
#include "Common/GlobalData.h"
#include "GameClient/GXReplaceFile.h"

#include <errno.h>		// loadStore: ENOENT is the only "no file"
#include <stdio.h>
#include <stdlib.h>		// rand_s on MSVC; also the only place _CRT_RAND_S has any effect
#include <string.h>
#include <time.h>		// the ONE clock read in the verified-identity path; see loadVerified()
#include <map>

// rand_s is the CRT's wrapper over RtlGenRandom. Chosen over BCryptGenRandom deliberately: BCrypt
// would add a link library, and a link library is a build-config change - and while
// Core/GameEngine/CMakeLists.txt is itself outside the digest, reaching for bcrypt.lib is the kind
// of change that ends up in cmake/ where it would NOT be. A CSPRNG that needs no new dependency is
// worth more here than a marginally nicer API.
//
// The _CRT_RAND_S define that used to sit HERE is gone, and its position was the whole bug: it came
// after #include "PreRTS.h", which is a precompiled header that has already processed <stdlib.h>.
// The macro therefore did nothing, and rand_s was never declared. It is now set on the command line
// by Core/GameEngine/CMakeLists.txt, with a guarded fallback at the top of this file.

namespace
{

// RFC 4648 base32. Every character is inside the relay's TOKEN_RE ([A-Za-z0-9._-], relay.js:58),
// which is what lets an id travel in a relay token field unescaped.
const char BASE32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

const char *const IDENTITY_FILE = "GXPlayer.ini";

Bool  s_loaded = FALSE;
char  s_playerId[GX_PLAYER_ID_LEN + 1] = { 0 };
Bool  s_newIdHeld = FALSE;	// holdNewIdentity(); deliberately NOT cleared by reset()

// Insertion order is not preserved by std::map, and that is deliberate rather than accidental: the
// file is rewritten sorted, so two installs with the same friends produce byte-identical files and
// a diff of a pasted config is readable. Nothing about the feature depends on ordering.
typedef std::map<AsciiString, AsciiString> IdMap;
IdMap s_friends;	// id -> last known nickname ("" if never seen)
IdMap s_ignored;	// id -> "1"

AsciiString identityFilePath()
{
	AsciiString path = TheGlobalData->getPath_UserData();
	path.concat(IDENTITY_FILE);
	return path;
}

// ------------------------------------------------------------------------------------------------
// The verified identity (Phase 3b). See the header for what it is and is not.
// ------------------------------------------------------------------------------------------------

Bool               s_verifiedLoaded = FALSE;
GXAssertionClaims  s_claims = { { 0 }, { 0 }, 0, 0, GX_ROLE_PLAYER };
Bool               s_verified = FALSE;
// GeneralsX @bugfix Claude 23/09/2026 Set by a sign-out, cleared by a sign-in or reset(). While set, the bot
// reload in loadVerified() does not run: clearVerifiedIdentity() leaves s_assertionText empty, so a bot
// file the sign-out could not delete - or one the host's timer wrote afterwards - always "differs" from it
// and would sign the bot straight back in within a minute (review P3-2).
Bool               s_signedOut = FALSE;
// GeneralsX @feature Claude 11/08/2026 The raw assertion line, kept so the relay handshake can send
// the bytes rather than re-reading and re-parsing the file on the network path.
//
// WRITTEN ONLY ON GX_ASSERT_OK and cleared on every other status, so a document that did not verify
// locally can never be offered to a relay. That matters more than it looks: a client that forwarded
// an unverified assertion would turn every relay into the thing that discovers the client's own
// login is broken, and the player would see a lobby refusing them rather than a login error.
//
// IT IS NOT A SECRET - it is a signed public statement about an account, and it is designed to
// travel. It is also not a bearer credential for anything, and nothing may treat it as one.
char               s_assertionText[GX_ASSERTION_MAX + 1] = { 0 };

// A SEPARATE FILE FROM GXPlayer.ini, and not a field in it, for two independent reasons.
//
// The first is mechanical: loadStore() is a hand-rolled key=value reader and save() rewrites the
// whole file from the state it holds, so this store is not forward-compatible - a comment three
// lines above it already says the first field added to it needs a read-modify-write. An assertion
// is written by a DIFFERENT PROGRAM on Windows (the launcher, because the engine there has no
// HTTPS), and two programs rewriting one file from partial state is how a friends list gets eaten
// by a login.
//
// The second is lifecycle: friends are forever and an assertion expires in thirty days. Deleting
// the login must not risk the friend list, and it does not if they are not the same file.
//
// GeneralsX @feature Claude 23/09/2026 BOT MODE. Set only by our harness and the standing host's start
// script, never by a player's client. When set, this file IS the identity and ONLY a bot assertion is
// accepted from it: a bot file dropped into a player's data dir does nothing (the player path verifies
// under the identity key only), and Discord cannot write into a bot's run dir (GXLoginFlow::start
// refuses in bot mode). An absolute path is used as-is (a leading \\, // or \?? is refused: Windows would
// authenticate to that server); anything else must be a bare filename in the user data dir - how the
// iPad, one container, is given one. Those rules live in GXIdentityAssertion::resolveBotPath, which is
// pure so gxidtest can drive them. See docs/superpowers/plans/2026-09-23-bot-identity.md.
const char *botFileEnv()
{
	const char *v = getenv("GX_IDENTITY_FILE");
	return (v != NULL && v[0] != '\0') ? v : NULL;
}

AsciiString assertionFilePath()
{
	AsciiString path = TheGlobalData->getPath_UserData();
	const char *bot = botFileEnv();
	if (bot == NULL)
	{
		path.concat(GX_ASSERTION_FILENAME);
		return path;
	}
	char buf[1024];
	if (!GXIdentityAssertion::resolveBotPath(bot, path.str(), buf, sizeof(buf)))
		return AsciiString();										// refused: loadVerified reports it, sends nothing
	return AsciiString(buf);
}

// The first line of `path` into out, trailing whitespace stripped; "" when there is none. The bot
// reload's cheap "did the bytes change" test, and NOTHING ELSE: the bytes a relay is sent are the ones
// the loader verified (loadFile's `text`), never a second read of the file (review P3-1).
//
// GeneralsX @bugfix Claude 18/09/2026 With a stream buffer that is OURS, and wiped, for the reason
// GXIdentityAssertion::loadFileWithKey gives: stdio's own would be freed at fclose still holding
// the whole sign-in.
void readFirstLine(const char *path, char *out, size_t cap)
{
	out[0] = '\0';
	char iobuf[512];
	GXScopedWipe wipeIo(iobuf, sizeof(iobuf));
	FILE *fp = fopen(path, "rb");
	if (fp == NULL)
		return;
	setvbuf(fp, iobuf, _IOFBF, sizeof(iobuf));
	if (fgets(out, (int)cap, fp) == NULL)
		out[0] = '\0';
	fclose(fp);
	// Strip the line ending and any trailing whitespace, exactly as loadFile does, so the bytes sent
	// match the bytes verified.
	size_t n = strlen(out);
	while (n > 0 && (unsigned char)out[n - 1] <= ' ')
		out[--n] = '\0';
}

void loadVerified()
{
	const Bool bot = botFileEnv() != NULL;

	// THE CLOCK IS READ HERE, ONCE, AND NOWHERE ELSE. GXIdentityAssertion deliberately contains no
	// time() call - the header says so and its whole test suite depends on it, because expiry and
	// not-yet-valid are only testable when the caller supplies the clock. This is the first caller,
	// so this is the one place that decides what "now" means, and every check inside one load sees
	// the same instant rather than a clock that moved between two of them.
	const long long nowS = (long long)time(NULL);

	// GeneralsX @feature Claude 23/09/2026 A bot's file is renewed underneath a running process (the host's
	// 6-hourly timer). At most once a minute, and a full re-verify only when the BYTES changed; bot mode only -
	// a player's file changes only through GXLoginFlow, which reloads explicitly (refreshVerifiedIdentity).
	// gxDeclareIdentityIfChanged then re-declares the new assertion, because its fingerprint moved.
	// GeneralsX @bugfix Claude 23/09/2026 The decision is GXIdentityAssertion::botRecheckDue/botReloadNeeded,
	// pure, so gxidtest drives it (review P2) - and it re-verifies a held login that has EXPIRED with the
	// file unchanged, so that is refused and logged rather than declared until the process exits (review P3).
	static long long s_botCheckedAt = 0;
	if (bot && s_verifiedLoaded && !s_signedOut && GXIdentityAssertion::botRecheckDue(nowS, &s_botCheckedAt))
	{
		char line[GX_ASSERTION_MAX + 1];
		GXScopedWipe wipeLine(line, sizeof(line));
		readFirstLine(assertionFilePath().str(), line, sizeof(line));
		if (GXIdentityAssertion::botReloadNeeded(line, s_assertionText, s_verified ? s_claims.exp : 0, nowS))
			s_verifiedLoaded = FALSE;
	}

	if (s_verifiedLoaded)
		return;
	s_verifiedLoaded = TRUE;
	// GeneralsX @feature Claude 20/09/2026 THE CHAT EXEMPTION IS CLEARED FIRST, so every path out of
	// this function that is not "a verified admin" leaves it off - absent, expired, malformed, wrong
	// signer, and a reload that used to succeed and no longer does. Set only on the GX_ASSERT_OK
	// branch below. ChatRateLimit.h explains why a client may hold this at all (it is pacing, not
	// permission) and why it is a floor rather than no limit.
	ChatRate::setSharedExempt(false);

	const AsciiString path = assertionFilePath();

	// Bot mode accepts ONLY a bot assertion, the player path ONLY a production one - never either.
	// GeneralsX @bugfix Claude 23/09/2026 The loader hands back the line it VERIFIED, on OK only. This used to
	// re-read the file for the bytes, and a bot's file is replaced under a running process: a swap between
	// the two reads held - and sent - bytes nothing had checked (review P3-1).
	// GeneralsX @bugfix Claude 23/09/2026 INTO LOCALS, adopted only on OK: a bot reload that is refused must
	// not throw away a held login that is still valid - see the refusal below (review P3).
	GXAssertionClaims claims = { { 0 }, { 0 }, 0, 0, GX_ROLE_PLAYER };
	char text[GX_ASSERTION_MAX + 1];
	GXScopedWipe wipeText(text, sizeof(text));
	const GXAssertStatus st = bot
		? GXIdentityAssertion::loadBotFile(path.str(), nowS, &claims, text)
		: GXIdentityAssertion::loadFile(path.str(), nowS, &claims, text);

	if (st == GX_ASSERT_OK)
	{
		// GeneralsX @bugfix Claude 18/09/2026 THE WHOLE BUFFER, not the first byte: a SHORTER assertion than
		// the one held before would otherwise leave the rest of the previous token behind the terminator -
		// the same tail GXLobbyIdentity::set clears for the lobby copy.
		gxSecureClear(s_assertionText);
		memcpy(s_assertionText, text, strlen(text) + 1);
		s_claims = claims;
		s_verified = TRUE;
		// The local verified role, and the ONLY thing in this client that reads it for anything but
		// presentation - see ChatRateLimit.h for why that is allowed here and nowhere else. loadFile
		// has already checked exp against nowS above, so this is a role from an assertion that was
		// valid at this instant.
		ChatRate::setSharedExempt(s_claims.role == GX_ROLE_ADMIN);
		return;
	}

	// GeneralsX @bugfix Claude 23/09/2026 A REFUSED BOT RELOAD KEEPS A HELD LOGIN THAT IS STILL VALID (review
	// P3). One unreadable, missing or bad read at the minute check used to drop a login with up to 24 h left;
	// gxDeclareIdentityIfChanged then sent the relay a clear, and under enforcement the host was dropped. The
	// held bytes are the ones verified when they were adopted, so nothing unchecked is sent; the next minute
	// check tries the file again. Its expiry uses the verifier's rule, so an expired held login is dropped.
	const Bool keep = bot && s_verified && nowS - GX_ASSERTION_SKEW_S <= s_claims.exp;
	if (!keep)
	{
		s_verified = FALSE;
		gxSecureClear(s_assertionText);
	}

	// ABSENT is the permanent normal state for every LAN player, every offline player and everyone
	// who never wanted an account, so it is SILENT. Anything else is a player who tried to log in
	// and did not get what they expected, and the status name is the only thing that distinguishes
	// "your clock is wrong" from "this build does not trust that signer".
	//
	// In BOT MODE every refusal is logged, ABSENT included: a bot with no usable login is a broken
	// deployment, never a player who chose not to sign in. The path is named - it is not a secret, and
	// an empty one means GX_IDENTITY_FILE itself was refused.
	if (bot)
	{
		fprintf(stderr, "[GXID] bot mode: %s from '%s': %s\n",
			keep ? "keeping the held login, refused a reload" : "no verified identity",
			path.str(), GXIdentityAssertion::statusName(st));
	}
	else if (st != GX_ASSERT_ABSENT)
	{
		fprintf(stderr, "[GXID] no verified identity this session: %s\n",
			GXIdentityAssertion::statusName(st));
	}
}

/*
 * Fill buf with n cryptographically strong bytes. FALSE on any failure.
 *
 * A FAILURE HERE MUST NEVER FALL BACK TO rand(). The id's entire security property is that it
 * cannot be guessed; an id drawn from a 32-bit seeded PRNG is enumerable in seconds, and it would
 * be indistinguishable at every other layer from a good one - the feature would look like it
 * worked. Refusing is the only safe answer, and the caller turns it into a loud failure.
 */
Bool cryptoRandom(unsigned char *buf, size_t n)
{
#ifdef _WIN32
	for (size_t i = 0; i < n; ++i)
	{
		unsigned int v = 0;
		if (rand_s(&v) != 0)
			return FALSE;
		buf[i] = (unsigned char)(v & 0xFF);
	}
	return TRUE;
#else
	// /dev/urandom rather than arc4random_buf: it is present on macOS, Linux and iOS alike, so
	// there is one code path to reason about instead of three. It never blocks after boot.
	FILE *fp = fopen("/dev/urandom", "rb");
	if (fp == NULL)
		return FALSE;
	size_t got = fread(buf, 1, n, fp);
	fclose(fp);
	return got == n ? TRUE : FALSE;
#endif
}

/*
 * 16 random bytes -> 26 base32 characters.
 *
 * 26 characters carry 130 bits and we have 128, so the last character's low 2 bits are always
 * zero. That is fine and is NOT trimmed: a fixed 26-character shape makes isValidPlayerId a length
 * check plus an alphabet check, with no special case for the tail.
 */
void encodeBase32(const unsigned char *in, char *out)
{
	unsigned int acc = 0;
	int bits = 0;
	int o = 0;
	for (int i = 0; i < 16; ++i)
	{
		acc = (acc << 8) | in[i];
		bits += 8;
		while (bits >= 5)
		{
			bits -= 5;
			out[o++] = BASE32[(acc >> bits) & 0x1F];
		}
	}
	if (bits > 0)
		out[o++] = BASE32[(acc << (5 - bits)) & 0x1F];
	out[o] = '\0';
}

Bool mintPlayerId(char *out)
{
	unsigned char raw[16];
	if (!cryptoRandom(raw, sizeof(raw)))
		return FALSE;
	encodeBase32(raw, out);
	return TRUE;
}

// GeneralsX @bugfix Claude 22/10/2026 What the read found. ONLY a file that is not there (ENOENT) is a first start.
// Every other failure - no read permission, a sharing violation, a OneDrive placeholder that cannot be fetched, a
// directory in its place, a read error part way - used to look exactly like one: ensureLoaded() minted a new id and
// save() moved a file holding only that over the player's, and their identity and every friend were gone (Codex
// uow-r1 P1). Such a file is now left exactly as it is and the session has no identity, as a CSPRNG failure gives.
enum StoreRead
{
	STORE_LOADED,
	STORE_MISSING,
	STORE_UNREADABLE
};

Bool s_storeUnreadable = FALSE;	// this session's read failed on a file that exists: save() writes nothing

/*
 * Read the store. Absent file is the normal first-run state and is not an error.
 *
 * Parsed here rather than through Common/UserPreferences deliberately. That class is a
 * std::map<AsciiString,AsciiString> whose loader has documented sharp edges around lines with no
 * '=' (see the @bugfix at UserPreferences.cpp:130), and it lives INSIDE the digest - so any future
 * fix to it would be a four-platform event that this file would silently inherit. Twenty lines here
 * keeps a social-state file from taking a dependency on simulation-digested code.
 */
StoreRead loadStore(int *errOut)
{
	errno = 0;
	const AsciiString path = identityFilePath();
	FILE *fp = fopen(path.str(), "r");
	if (fp == NULL)
	{
		*errOut = errno;
		// A dangling symlink opens as ENOENT too, and saving over its name would cut the player's real file off.
		return (*errOut == ENOENT && !gxNameExists(path.str())) ? STORE_MISSING : STORE_UNREADABLE;
	}

	char line[512];
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		// Strip the newline and any trailing whitespace, then skip blanks and comments. A hand-
		// edited file is expected - the first line of what we write is a warning aimed at a human.
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
			|| line[len - 1] == ' ' || line[len - 1] == '\t'))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#')
			continue;

		char *eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;

		if (strcmp(key, "id") == 0)
		{
			// Validated on the way in. A truncated or hand-mangled id would otherwise be published
			// to the relay and stored by every friend, and the damage would outlive the bad file.
			if (GXPlayerIdentity::isValidPlayerId(val))
				strcpy(s_playerId, val);
		}
		else if (strncmp(key, "friend.", 7) == 0)
		{
			if (GXPlayerIdentity::isValidPlayerId(key + 7))
				s_friends[AsciiString(key + 7)] = AsciiString(val);
		}
		else if (strncmp(key, "ignore.", 7) == 0)
		{
			if (GXPlayerIdentity::isValidPlayerId(key + 7))
				s_ignored[AsciiString(key + 7)] = AsciiString("1");
		}
		// Anything else is from a newer build. Ignored rather than refused, so an older client does
		// not destroy a newer one's data on the next save... which it does anyway, because the save
		// below rewrites the whole file. Noted as a real limitation: this store is not
		// forward-compatible, and the first field added to it needs a read-modify-write instead.
	}
	// fgets returns NULL for an error as well as for the end, so a read that failed part way (EISDIR for a
	// directory, EIO) would otherwise be a short file whose missing id is minted again.
	const Bool failed = ferror(fp) ? TRUE : FALSE;
	*errOut = failed ? errno : 0;
	fclose(fp);
	return failed ? STORE_UNREADABLE : STORE_LOADED;
}

// The load ensureLoaded acts on. FALSE when GXPlayer.ini exists and could not be read: whatever the failed read did take
// in is dropped too - half a friends list would be saved as the whole one if anything wrote, and nothing may - and
// save() refuses until reset(). Checked once a session, like the rest of the load; the next start reads the file again.
Bool readStore()
{
	int err = 0;
	if (loadStore(&err) != STORE_UNREADABLE)
		return TRUE;
	s_storeUnreadable = TRUE;
	s_playerId[0] = '\0';
	s_friends.clear();
	s_ignored.clear();
	fprintf(stderr, "[GXID] %s exists but could not be read (%s, errno %d). It is left exactly as it is: no player "
		"identity this session and friends are off. A new id is not made, because saving it would replace "
		"the player's own.\n", identityFilePath().str(), strerror(err), err);
	return FALSE;
}

} // anonymous namespace

void GXPlayerIdentity::ensureLoaded()
{
	if (s_loaded)
		return;
	s_loaded = TRUE;	// set FIRST: save() below calls back in, and a re-entrant load would clear
						// the very map it is being asked to write.
	if (!readStore())
		return;	// it exists and could not be read: left as it is, no identity this session
	if (s_playerId[0] == '\0')
	{
		if (s_newIdHeld)
		{
			// save() refuses an empty id, so nothing is written either, and the next start's
			// migration still finds no GXPlayer.ini in its way.
			fprintf(stderr, "[GXID] No player identity this session: the old GXPlayer.ini has not been "
				"copied into this folder yet (see [gxmigrate] above). Friends are off until it is. A new "
				"id is not made, because it would stop the old one from ever coming across.\n");
			return;
		}
		if (!mintPlayerId(s_playerId))
		{
			// Loud, and it stays empty. Callers treat an empty id as "identity unavailable" and
			// simply do not offer friends this session. The alternative - a predictable id - would
			// silently hand every player the same identity, which is worse than the feature being
			// off, and would look exactly like it was working.
			s_playerId[0] = '\0';
			fprintf(stderr, "[GXID] FATAL: the platform CSPRNG failed. No player identity this "
				"session; friends are unavailable. Refusing to invent a guessable id.\n");
			return;
		}
		save();
	}
}

const char *GXPlayerIdentity::getPlayerId()
{
	ensureLoaded();
	return s_playerId;
}

Bool GXPlayerIdentity::isValidPlayerId(const char *s)
{
	if (s == NULL)
		return FALSE;
	if (strlen(s) != (size_t)GX_PLAYER_ID_LEN)
		return FALSE;
	for (int i = 0; i < GX_PLAYER_ID_LEN; ++i)
	{
		const char c = s[i];
		const Bool ok = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

Int GXPlayerIdentity::getFriendCount()
{
	ensureLoaded();
	return (Int)s_friends.size();
}

const char *GXPlayerIdentity::getFriendId(Int i)
{
	ensureLoaded();
	if (i < 0 || i >= (Int)s_friends.size())
		return "";
	IdMap::const_iterator it = s_friends.begin();
	std::advance(it, i);
	return it->first.str();
}

const char *GXPlayerIdentity::getFriendNick(Int i)
{
	ensureLoaded();
	if (i < 0 || i >= (Int)s_friends.size())
		return "";
	IdMap::const_iterator it = s_friends.begin();
	std::advance(it, i);
	return it->second.str();
}

Bool GXPlayerIdentity::isFriend(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return FALSE;
	return s_friends.find(AsciiString(id)) != s_friends.end() ? TRUE : FALSE;
}

// GeneralsX @feature Claude 08/08/2026 See the header. This exists because the lobby can now
// resolve a room slot to an id, so "what do I call this person" is finally a lookup BY ID rather
// than the name-against-name scan every caller used to have to do.
const char *GXPlayerIdentity::getFriendNickById(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return "";
	IdMap::const_iterator it = s_friends.find(AsciiString(id));
	return (it != s_friends.end()) ? it->second.str() : "";
}

Bool GXPlayerIdentity::addFriend(const char *id, const char *nick)
{
	ensureLoaded();
	// Validated because this is reached from the network: a card arrives over the relay carrying an
	// id somebody else chose. Storing a malformed one would put it back on the wire later.
	if (!isValidPlayerId(id))
		return FALSE;
	// Befriending yourself is not an error worth a message, but it must not create an edge - it
	// would show you as your own friend and, worse, make you a valid DM destination for yourself.
	if (strcmp(id, s_playerId) == 0)
		return FALSE;
	s_friends[AsciiString(id)] = AsciiString(nick != NULL ? nick : "");
	return save();
}

Bool GXPlayerIdentity::removeFriend(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return FALSE;
	if (s_friends.erase(AsciiString(id)) == 0)
		return FALSE;
	return save();
}

Bool GXPlayerIdentity::noteFriendNick(const char *id, const char *nick)
{
	ensureLoaded();
	if (!isValidPlayerId(id) || nick == NULL)
		return FALSE;
	IdMap::iterator it = s_friends.find(AsciiString(id));
	if (it == s_friends.end())
		return FALSE;	// only ever refresh a name we already have an edge for
	if (it->second.compare(nick) == 0)
		return TRUE;	// unchanged - do not rewrite the file on every roster tick
	it->second = AsciiString(nick);
	return save();
}

Int GXPlayerIdentity::getIgnoreCount()
{
	ensureLoaded();
	return (Int)s_ignored.size();
}

const char *GXPlayerIdentity::getIgnoreId(Int i)
{
	ensureLoaded();
	if (i < 0 || i >= (Int)s_ignored.size())
		return "";
	IdMap::const_iterator it = s_ignored.begin();
	std::advance(it, i);
	return it->first.str();
}

Bool GXPlayerIdentity::isIgnored(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return FALSE;
	return s_ignored.find(AsciiString(id)) != s_ignored.end() ? TRUE : FALSE;
}

Bool GXPlayerIdentity::addIgnore(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return FALSE;
	if (strcmp(id, s_playerId) == 0)
		return FALSE;
	s_ignored[AsciiString(id)] = AsciiString("1");
	return save();
}

Bool GXPlayerIdentity::removeIgnore(const char *id)
{
	ensureLoaded();
	if (!isValidPlayerId(id))
		return FALSE;
	if (s_ignored.erase(AsciiString(id)) == 0)
		return FALSE;
	return save();
}

Bool GXPlayerIdentity::save()
{
	// Deliberately NOT ensureLoaded(): this is called FROM ensureLoaded() to persist a freshly
	// minted id, and calling back in would recurse.
	if (s_playerId[0] == '\0' || s_storeUnreadable)
		return FALSE;	// nothing worth writing, and writing an empty id would poison the file

	// GeneralsX @bugfix Claude 23/09/2026 Written beside the file and swapped into place. Truncating
	// the live file first means a crash or a full disk between here and the last fprintf leaves an
	// EMPTY identity file, and ensureLoaded() would then mint a new player id - the player loses
	// who they are and every friend. On Windows it lives under Documents, which OneDrive syncs.
	const AsciiString path = identityFilePath();
	AsciiString tmp = path;
	tmp.concat(".tmp");
	FILE *fp = fopen(tmp.str(), "w");
	if (fp == NULL)
		return FALSE;

	// The warning is first and is in capitals on purpose. The first thing a player does when a
	// lobby misbehaves is paste their config into a chat window, and this file is a bearer
	// credential - anyone holding this id can be this player.
	fprintf(fp, "# GeneralsX player identity.\n");
	fprintf(fp, "# DO NOT SHARE THIS FILE OR POST ITS CONTENTS. The id below IS your identity:\n");
	fprintf(fp, "# anyone who has it can appear as you to your friends.\n");
	fprintf(fp, "id=%s\n", s_playerId);
	for (IdMap::const_iterator it = s_friends.begin(); it != s_friends.end(); ++it)
		fprintf(fp, "friend.%s=%s\n", it->first.str(), it->second.str());
	for (IdMap::const_iterator it = s_ignored.begin(); it != s_ignored.end(); ++it)
		fprintf(fp, "ignore.%s=1\n", it->first.str());

	return gxCommitAndReplace(fp, tmp.str(), path.str()) ? TRUE : FALSE;
}

void GXPlayerIdentity::holdNewIdentity()
{
	s_newIdHeld = TRUE;
}

void GXPlayerIdentity::reset()
{
	s_loaded = FALSE;
	s_storeUnreadable = FALSE;
	s_playerId[0] = '\0';
	s_friends.clear();
	s_ignored.clear();
	s_verifiedLoaded = FALSE;
	s_verified = FALSE;
	s_signedOut = FALSE;
	// GeneralsX @feature Claude 20/09/2026 ...and the chat exemption with it. Clearing
	// s_verifiedLoaded means loadVerified() will re-run and clear this anyway, but only at the next
	// call that happens to need the identity - and a chat send does not. Cleared HERE so the window
	// between reset() and that call cannot exist at all.
	ChatRate::setSharedExempt(false);
}

// ------------------------------------------------------------------------------------------------
// The verified identity.
//
// Every accessor below answers the unauthenticated shape - "" , 0, FALSE - when there is no
// verified identity, and that path is taken by every LAN player and every offline player forever.
// A caller that forgets to check hasVerifiedIdentity() therefore gets an empty name rather than a
// stale or forged one, which is the same "a sloppy call site is harmless rather than exploitable"
// rule GXIdentityAssertion applies to its own out parameter.
// ------------------------------------------------------------------------------------------------

Bool GXPlayerIdentity::cryptoRandomBytes(unsigned char *out, Int count)
{
	if (out == NULL || count <= 0)
		return FALSE;
	return cryptoRandom(out, (size_t)count);
}

Bool GXPlayerIdentity::hasVerifiedIdentity()
{
	loadVerified();
	return s_verified;
}

const char *GXPlayerIdentity::getVerifiedName()
{
	loadVerified();
	return s_verified ? s_claims.name : "";
}

// GeneralsX @feature Claude 11/08/2026 See the note by s_assertionText. "" when there is no
// verified identity, so a caller that forgets to check hasVerifiedIdentity() sends nothing rather
// than something stale.
const char *GXPlayerIdentity::getVerifiedAssertionText()
{
	loadVerified();
	return s_verified ? s_assertionText : "";
}

// PRESENTATION ONLY. See the note on GXAssertRole: every permission in this system is enforced by
// the relay against the durable sub, because a role the client decides is a colour.
GXAssertRole GXPlayerIdentity::getVerifiedRole()
{
	loadVerified();
	return s_verified ? s_claims.role : GX_ROLE_PLAYER;
}

const char *GXPlayerIdentity::getVerifiedSub()
{
	loadVerified();
	return s_verified ? s_claims.sub : "";
}

long long GXPlayerIdentity::getVerifiedExpiry()
{
	loadVerified();
	return s_verified ? s_claims.exp : 0;
}

Int GXPlayerIdentity::getVerifiedDaysLeft()
{
	loadVerified();
	if (!s_verified)
		return 0;
	const long long left = s_claims.exp - (long long)time(NULL);
	if (left <= 0)
		return 0;
	// Rounded DOWN, so "1 day left" means at least one day and never a few minutes. This number is
	// shown to a player to decide whether to log in again now or later, and a hint that rounds up
	// tells them they have a day when they have ten minutes.
	return (Int)(left / (24 * 3600));
}

Bool GXPlayerIdentity::isBotMode()
{
	return botFileEnv() != NULL;
}

void GXPlayerIdentity::refreshVerifiedIdentity()
{
	s_signedOut = FALSE;		// a sign-in: the bot reload may run again
	s_verifiedLoaded = FALSE;
	loadVerified();
}

GXSignInRemoval GXPlayerIdentity::clearVerifiedIdentity()
{
	const AsciiString path = assertionFilePath();

	// GeneralsX @bugfix Claude 18/09/2026 The file half lives in GXSignOutFiles.h, pure, so a test
	// can hold its properties against real files: the interrupted write's .tmp goes too (a bearer
	// token on disk, even if nothing reads it), a file that was ALREADY absent counts as gone rather
	// than as a failure, and a surviving main file is told apart from a surviving copy - only the
	// first signs the player back in next launch, so only the first may be reported that way.
	//
	// GeneralsX @bugfix Claude 23/09/2026 NOT IN BOT MODE (review P3). GX_IDENTITY_FILE is the operator's,
	// and the harnesses hand ONE path to every peer, so one GUI /logout deleted every peer's login. The
	// in-memory sign-out below still holds (s_signedOut); the file stays, so the next launch signs the bot
	// in again - which is exactly what GX_SIGNIN_FILE_SURVIVED tells the player.
	const GXSignInRemoval removed = botFileEnv() != NULL ? GX_SIGNIN_FILE_SURVIVED : gxRemoveSavedSignIn(path.str());

	// The in-memory claims are dropped whether or not the file went away. If the delete failed -
	// permissions - the player asked to be signed out and must be signed out for this session
	// regardless; the alternative is a logout that silently does nothing and leaves their name on
	// screen.
	s_verifiedLoaded = TRUE;
	s_verified = FALSE;
	s_signedOut = TRUE;		// and stays out: see s_signedOut - the bot reload would undo it
	/*
	 * GeneralsX @bugfix Claude 20/09/2026 THE CHAT EXEMPTION GOES ON SIGN-OUT, and it has to be done
	 * HERE rather than left to loadVerified().
	 *
	 * This function sets s_verifiedLoaded = TRUE deliberately - the line above - so that
	 * loadVerified() does NOT re-run and re-read the file it has just deleted. That is correct for
	 * the identity, and it is exactly why the exemption cannot rely on loadVerified() to clear it:
	 * the one function that clears it is the one function guaranteed never to be called again.
	 * Found by asking what happens to an admin who signs out; without this they would keep unlimited
	 * chat for the rest of the session, with no verified identity behind it at all.
	 */
	ChatRate::setSharedExempt(false);
	memset(&s_claims, 0, sizeof(s_claims));
	// GeneralsX @bugfix Claude 18/09/2026 The raw assertion too. getVerifiedAssertionText() already
	// refuses to return it once s_verified is FALSE, so this was unreachable rather than exposed -
	// but it was the bearer token itself, still sitting in process memory after a sign-out.
	memset(s_assertionText, 0, sizeof(s_assertionText));
	return removed;
}

// GeneralsX @feature Claude 11/08/2026 See the header. One CSPRNG byte per character, masked to
// five bits - wasteful of entropy and trivially uniform, which is the right trade for a nonce.
Bool GXPlayerIdentity::mintNonce( char *out, Int chars )
{
	if (out == NULL)
		return FALSE;
	if (chars <= 0 || chars > 32)
	{
		out[0] = '\0';
		return FALSE;
	}
	unsigned char raw[32];
	if (!cryptoRandom(raw, (size_t)chars))
	{
		out[0] = '\0';
		return FALSE;
	}
	for (Int i = 0; i < chars; ++i)
		out[i] = BASE32[raw[i] & 0x1F];
	out[chars] = '\0';
	return TRUE;
}
