// GeneralsX @bugfix Claude 19/09/2026 - the relay's registration secret, for the LOBBY PRESENCE socket.
//
// The relay mints a secret on the first claim of a (room, id) and hands it back with
// "GXAUTH <room> <id> <secret> [caps]"; a registration from a DIFFERENT endpoint for that id is refused
// unless it presents it as a fourth word (tools/relay/relay.js, AUTH_TAG and RECLAIM_GRACE_MS). The game
// socket has done this since 08/08/2026 (Transport::captureRelayAuth / buildRelayRegistration). The
// presence socket never did, so a player whose public address changed was refused on every keepalive
// and - because each refusal extends the retired binding - stayed out of the shared lobby for as long as
// the client kept asking. Silently: the relay sends nothing back on a refusal.
//
// A MIRROR OF TRANSPORT'S LOGIC, NOT A CALL INTO IT. Transport.cpp/.h are inside the simulation digest
// (resources/gitinfo/simsourcedigest_watcher.cmake); exposing a helper from there moves the digest, and a
// moved digest refuses every peer on the previous build. This header is under GameClient, which is not
// digested - the same trade GXLobbyPresence.cpp's GX_PEER_TAG note makes.
//
// PURE, so scripts/test/lobby-presence-auth-test.cpp can drive the real parser and composer.
//
// ponytail: the RAW secret, like Transport - not the relay's hmac1 proof. It rides every 5 s keepalive in
// cleartext, so an on-path observer can capture it; the GXIDN assertion on the same socket already has
// that exposure. Upgrade path: both sockets move to hmac1 together (relay.js PROOF_CTR_BYTES).
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "GameClient/GXScopedWipe.h"	// the secret is a bearer credential - no stack copy outlives the call

enum
{
	GX_RELAY_SECRET_LEN = 22,						///< relay.js SECRET_RE: 16 bytes, base64url, no padding
	GX_RELAY_SECRET_BUF = GX_RELAY_SECRET_LEN + 1
};

// GeneralsX @security Claude 07/11/2026 (Codex vnext1107-r2 P1) The word that says this client can read a relay id past .14. relay.js
// SEATS_WORD has the argument: a 0.9.69 client overruns its lobby tables on such an id and nothing on the wire names it, so an id past
// .14 goes only to clients that say so, in rooms where everybody did.
static const char GX_RELAY_SEATS_WORD[] = "seats34";
enum
{
	GX_RELAY_SEATS_SILENCE_MS = 20000,				///< four missed 5 s keepalives
	GX_RELAY_SEATS_UNANSWERED = 4					///< ...and they have to have been sent
};

/// Whether this client says the word. Apart from the clock and the socket so a test can drive it; Transport holds the one copy.
///
/// DARK UNTIL THE RELAY SPEAKS, like the secret: a relay that predates the word REFUSES a GXRLY carrying it. So `said` starts false, and
/// a relay that never declares it never sees it. GXCAPS rides every accepted registration, which makes it a heartbeat: a relay rolled
/// back under a running client refuses the word and answers nothing, and the silence takes it off the line again.
///
/// EVERY SEAT, .15 AND ABOVE INCLUDED (Codex caps34 r1). Going silent costs nothing on a relay that reads the word: it refuses the plain
/// registration past .14 and answers GXCAPS, and the word is back on the next keepalive. Keeping it would strand the seat for good on
/// a relay that does not read it, which refuses the word and never says anything again.
///
/// SILENCE IS UNANSWERED REGISTRATIONS, NOT TIME. A map load blocks the loop for longer than the window, sending nothing and reading
/// nothing, and judged by the clock alone that read as a dead relay. Measured on a real binary paused for 30 s beside a .20: the
/// next registration went out plain, the lobby seat had lapsed, and the relay refused it for the 5 s until GXCAPS brought the word
/// back. Counted, the same pause costs nothing. The game port was never in it: the relay holds that seat through a match load.
struct GXRelaySeats
{
	bool said;			///< the relay in use has told us it reads the word
	unsigned heardMs;	///< when it last did
	unsigned unanswered;	///< registrations carrying the word since it did

	void note( bool declared, unsigned nowMs )
	{
		said = declared;
		unanswered = 0;
		if (declared)
			heardMs = nowMs;
	}

	void sent()
	{
		if (said)
			++unanswered;
	}

	bool wanted( unsigned nowMs )
	{
		if (said && unanswered >= GX_RELAY_SEATS_UNANSWERED && nowMs - heardMs > GX_RELAY_SEATS_SILENCE_MS)
			said = false;
		return said;
	}
};

/// GXRelaySeats, one per relay (Codex caps34-client-r2 P1). Whether a relay reads the word is a fact about THAT relay, and two sockets of one
/// process can stand on two relays after a failover: held once, the new relay's GXCAPS would keep the word on for a socket registered at
/// an old one, which refuses it for good. Keyed by address and lobby port, which is what both sockets of one relay share. Zero-initialised;
/// a fifth relay takes the oldest slot, which costs only a relearning.
struct GXRelaySeatsByRelay
{
	enum { SLOTS = 4 };
	unsigned addr[SLOTS];
	unsigned short port[SLOTS];
	bool used[SLOTS];
	GXRelaySeats seats[SLOTS];
	unsigned next;

	GXRelaySeats &at( unsigned relayAddr, unsigned short relayLobbyPort )
	{
		for (unsigned i = 0; i < SLOTS; ++i)
		{
			if (used[i] && addr[i] == relayAddr && port[i] == relayLobbyPort)
				return seats[i];
		}
		const unsigned slot = next++ % SLOTS;
		used[slot] = true;
		addr[slot] = relayAddr;
		port[slot] = relayLobbyPort;
		seats[slot].said = false;
		seats[slot].heardMs = 0;
		seats[slot].unanswered = 0;
		return seats[slot];
	}
};

/// Is `word` one whole token of a GXCAPS comma-separated flag list? Never a substring: see captureRelayCaps.
inline bool gxRelayFlagsHave( const char *flags, const char *word )
{
	if (flags == NULL || word == NULL || word[0] == '\0')
		return false;
	const size_t wordLen = strlen(word);
	for (const char *tok = flags; ; )
	{
		const char *sep = strchr(tok, ',');
		const size_t n = (sep != NULL) ? (size_t)(sep - tok) : strlen(tok);
		if (n == wordLen && strncmp(tok, word, n) == 0)
			return true;
		if (sep == NULL)
			return false;
		tok = sep + 1;
	}
}

/// "GXRLY <room> <a.b.c.d>" with no secret - the first claim, and the only shape a relay that predates
/// GXAUTH accepts (it refuses four words outright) - or "GXRLY <room> <a.b.c.d> <secret>" once the relay
/// has issued one, and either with GX_RELAY_SEATS_WORD last when `seats34` says so. Returns the length, or 0 if it does not fit (never a
/// truncated line).
inline int gxRelayRegistrationLine( char *out, size_t size, const char *room, unsigned id, const char *secret, bool seats34 = false )
{
	int len = (secret != NULL && secret[0] != '\0')
		? snprintf(out, size, "GXRLY %s %u.%u.%u.%u %s", room,
			(id >> 24) & 0xFFu, (id >> 16) & 0xFFu, (id >> 8) & 0xFFu, id & 0xFFu, secret)
		: snprintf(out, size, "GXRLY %s %u.%u.%u.%u", room,
			(id >> 24) & 0xFFu, (id >> 16) & 0xFFu, (id >> 8) & 0xFFu, id & 0xFFu);
	if (len <= 0 || (size_t)len >= size)
		return 0;
	if (seats34)
	{
		const int more = snprintf(out + len, size - (size_t)len, " %s", GX_RELAY_SEATS_WORD);
		if (more <= 0 || (size_t)(len + more) >= size)
			return 0;
		len += more;
	}
	return len;
}

/// GeneralsX @security Claude 07/11/2026 (lead C77, Codex vnext1107b-r1 P1-3) "GXLIST", or "GXLIST seats34" when `seats34` says so. A relay
/// that reads the word cuts a plain GXLIST to what a 0.9.69 browser can hold (no host past .14, at most 14 slots, see relay.js SEATS_WORD),
/// so a 32-seat game shows its real count only to a list asked for with the word. The same rule as GXRLY and GXROOM decides `seats34`: only
/// a relay that has said it reads the word is sent it, because an older one does not answer the marked form. Returns the length, or 0 if it
/// does not fit (never a truncated line).
inline int gxRelayListLine( char *out, size_t size, bool seats34 )
{
	const int len = seats34 ? snprintf(out, size, "GXLIST %s", GX_RELAY_SEATS_WORD) : snprintf(out, size, "GXLIST");
	return (len <= 0 || (size_t)len >= size) ? 0 : len;
}

/// `rest` is a GXAUTH line after its tag and space: "<room> <id> <secret> [caps]". Copies the secret into
/// `out` and returns true ONLY if it names our room and our id and the secret is exactly SECRET_RE - it is
/// about to be pasted into a space-separated line, so anything else could forge a field. A refusal leaves
/// `out` untouched. Never logs, and wipes its own copy on every path.
inline bool gxRelayAuthTake( const char *rest, const char *room, unsigned id, char *out, size_t outSize )
{
	if (rest == NULL || room == NULL || out == NULL || outSize < GX_RELAY_SECRET_BUF)
		return false;
	char gotRoom[32] = { 0 };
	char gotId[24] = { 0 };
	char gotSecret[48] = { 0 };		// wider than a secret, so an over-length word is SEEN to be over-length
	GXScopedWipe wipeSecret(gotSecret, sizeof(gotSecret));
	if (sscanf(rest, "%31s %23s %47s", gotRoom, gotId, gotSecret) != 3)
		return false;
	char ourId[24];
	snprintf(ourId, sizeof(ourId), "%u.%u.%u.%u",
		(id >> 24) & 0xFFu, (id >> 16) & 0xFFu, (id >> 8) & 0xFFu, id & 0xFFu);
	if (strcmp(gotRoom, room) != 0 || strcmp(gotId, ourId) != 0 || strlen(gotSecret) != GX_RELAY_SECRET_LEN)
		return false;
	for (const char *c = gotSecret; *c != '\0'; ++c)
	{
		const bool b64url = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')
			|| *c == '-' || *c == '_';		// explicit ranges: isalnum() follows the locale
		if (!b64url)
			return false;
	}
	memcpy(out, gotSecret, GX_RELAY_SECRET_BUF);
	return true;
}

/// GeneralsX @feature Claude 25/09/2026 Does a GXAUTH `rest` ("<room> <id> <secret> [caps...]") carry the
/// capability word `cap`, AFTER the three fields gxRelayAuthTake reads? Whole-word compare, never a
/// substring, so "gxm10" is not "gxm1" - and nothing in the first three fields can count, so a room or a
/// secret that happens to spell the word proves nothing. relay.js sends "hmac1 gxm1" (AUTH_CAPS, GXM1_CAP).
inline bool gxRelayAuthHasCap( const char *rest, const char *cap )
{
	if (rest == NULL || cap == NULL || cap[0] == '\0')
		return false;
	const size_t capLen = strlen(cap);
	const char *p = rest;
	for (int field = 0; *p != '\0'; ++field)
	{
		while (*p == ' ')
			++p;
		const char *word = p;
		while (*p != '\0' && *p != ' ')
			++p;
		if (p == word)
			break;
		if (field >= 3 && (size_t)(p - word) == capLen && strncmp(word, cap, capLen) == 0)
			return true;
	}
	return false;
}
