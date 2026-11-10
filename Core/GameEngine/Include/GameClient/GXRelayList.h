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

// GeneralsX: S1-3 relay-list-v2: THE ENGINE READS THE SIGNED RELAY LIST.
//
// WHY. "Adding a relay must not require updating clients" ("The goal"). Until this file the
// signed list in the release manifest reached only the Windows launcher; Mac, Linux and the launcher-less
// iPad dialled the compiled-in bootstrap name, so a relay could move only by DNS and a standby could never
// exist (#4-#6, §2). This is the engine half of S1-3: it finds the newest
// verified list, and OptionPreferences::getRelayAddress() uses its HOME when the player has pinned nothing.
//
// THE DOCUMENT is the release manifest's wire form, "<payload>|<sig>" (scripts/tooling/gx-manifest.py):
//
//     2|<ordinal>|<minimum>|<sha>|<home>|<base>|<builds>|<master>|<alt>|<serial>[|<future>...]
//
// field 4 = the HOME relay (first entry; the signer keeps it to one), field 8 = STANDBY relays, field 9 =
// the signing time. Items are "host,port,region"; an empty port is 8086, the relay LOBBY port, and the
// relay's GAME port is always the lobby port + 2. A document without field 9 - every manifest published
// before S1-3 - is NOT a relay list here: it cannot be ordered against another, so it is refused and the
// bootstrap is used, which is exactly what the engine did before.
//
// THE CHECKS, IN THIS ORDER, AND THE ORDER IS THE SECURITY PROPERTY (the same shape as
// GXIdentityAssertion.cpp and gxlaunch.c's parse_manifest):
//
//   shape  ->  SIGNATURE (ranked keys)  ->  meaning (every item, all or nothing)  ->  freshness
//
//   * RANKED KEYS (§3.4). Three pinned keys, K1 < K2 < K3. The rank of a document is the rank of the key
//     that verifies it. The highest rank this install has ever FULLY accepted is persisted, and anything
//     lower is refused from then on: one K2 document kills K1 on this machine, one-way, with no revoke list
//     to forge. An unprovisioned slot (all zero) is skipped explicitly, never "verified" against.
//   * FRESHNESS IS (rank, serial) AND NOTHING ELSE (§8.7). The release ordinal belongs to the
//     update manifest; it is parsed and never consulted here, so a relay-only re-sign at the same ordinal
//     still beats the list it replaces, and a recovery document from an older release still wins.
//   * THE HORIZON, NOT A CLAMP (§3.3, §8.8-8.10, and relaylist-v2 r1+r2). A stolen key can sign
//     serial = 2^40. A list ARRIVING with a serial more than 7 days past this machine's clock is REFUSED
//     ("too far ahead"); every list accepted therefore carries a serial at most 7 days past its own arrival,
//     and freshness compares those raw serials. Consequences, each the point: (1) a replay of anything
//     already seen can never win again - its serial is fixed, and once an honest list signed after it is held
//     it is below the floor; (2) a stolen key can keep honest re-signs out for at most 7 days PER DELIVERY,
//     and only by delivering again (rotation to K2 ends it); (3) no per-document memory exists to overflow or
//     cycle - the stored-clamp design this replaced was beaten twice by review (a re-offered document re-clamped
//     from a later clock; a 4-entry history evicted by five pre-signed documents). The clock is
//     max(wall clock, a persisted high-water mark of the wall clock), so winding it back cannot shrink the
//     horizon below what was already seen. The engine's OWN cache is not re-checked against the horizon at load,
//     so a clock that later goes slow never discards an honest cache. Stated ceiling: a device whose clock is
//     more than 7 days SLOW refuses NEWER lists until its clock is right, and keeps the one it holds.
//     DELIBERATE DEPARTURE FROM §8.10's TEXT: the high-water mark takes WALL-CLOCK readings only, never a
//     document's signing time, which is attacker-chosen under a stolen key.
//   * PER-RANK FLOOR. The floor a document must reach is its OWN rank's, so a poisoned K1 floor can never
//     block the K2 recovery document. Plus a compiled-in term: the serial must not be older than this
//     build's own commit time (§8.5: serial, never ordinal). A dev build newer than the published list
//     therefore refuses it and uses the bootstrap - the behaviour before this file existed.
//
// WHERE DOCUMENTS COME FROM (§3.5), best (rank, serial) wins, ties keep what is held:
//   1. the launcher's own cache (Windows: gxmanifest.txt beside the game; macOS/Linux: the Rust launcher's
//      state folder) - RE-VERIFIED here, the launcher's verdict is never trusted;
//   2. this engine's own cache, GXRelayList.txt + GXRelayList.state in the user-data folder;
//   3. GXUPD to the compiled-in bootstrap ALWAYS, plus the held home and every standby, on a background
//      thread started at the Online lobby. A reply is taken only from one of those addresses. Without the
//      bootstrap leg a hostile list naming only hostile relays would trap a launcher-less iPad for ever
//      (§3.5).
//   Nothing verifiable anywhere: the compiled-in bootstrap, no standby - today's behaviour.
//
// THE HOME IS LATCHED FOR THE PROCESS. The first home() call fixes it; a better list found later is cached
// and takes effect at the next launch. Otherwise the lobby presence socket (which resolves its relay once)
// and a Transport created after a background fetch could sit on two different relays in one session.
//
// NOTHING IS PARTIALLY USED. One malformed item anywhere refuses the whole document, and a refused document
// is never cached. Every refusal prints one release-visible "[GXRELAYLIST] refused <source>: <reason>".
//
// TEST BUILDS (GX_RELAYLIST_TEST_KEYS) pin the three fixture keys of scripts/test/relay-list-fixtures.py
// instead of the production set, add a "-test" suffix to every file they keep, skip the launcher caches unless
// GX_RELAYLIST_LAUNCHER_CACHE names one, and take GX_RELAYLIST_BOOTSTRAP=host[:port] as the bootstrap EVERYWHERE -
// the fallback relay address as well as the refresh - so a test build can never wedge, or be wedged by, a
// player's install, and never registers with the production relay unless it is told to. A shipped binary
// carries none of that.
//
// NOT IN THE SIMULATION DIGEST (resources/gitinfo/simdigest_exclusions.txt, "@Lobby, identity and local
// files"): it chooses which relay a lobby talks to and computes nothing the simulation reads. The digested
// OptionPreferences.cpp calls it, which is why the exclusion is named rather than implied.
//
// The pure layer below (verify / judge / horizon / state text) takes everything as arguments and is what
// scripts/test/relay-list-test.cpp drives over the shared fixtures; it includes no engine header.

#pragma once

#include <stddef.h>

enum
{
	GX_RELAYLIST_HOST_MAX = 63,			// gx-manifest.py HOST_RE {1,63}
	GX_RELAYLIST_REGION_MAX = 8,		// REGION_RE {1,8}
	GX_RELAYLIST_MAX_ITEMS = 8,			// per relay field; the launchers' MAX_RELAYS
	GX_RELAYLIST_WIRE_MAX = 4095,		// the C launcher's WIRE_MAX - 1; production serves <= 900
	GX_RELAYLIST_RANKS = 3,
	GX_RELAYLIST_DEFAULT_PORT = 8086,	// an empty port field: the relay LOBBY port
	GX_RELAYLIST_GAME_PORT_OFFSET = 2,	// game port = lobby port + 2 (8086 -> 8088)
};

// The horizon: a list arriving with a serial further than this past this machine's clock is refused.
static const unsigned long long GX_RELAYLIST_HORIZON_S = 7ULL * 24ULL * 3600ULL;

struct GXRelayEndpoint
{
	char host[GX_RELAYLIST_HOST_MAX + 1];
	unsigned short port;				// the relay's LOBBY port; its game port is port + 2
	char region[GX_RELAYLIST_REGION_MAX + 1];
};

enum GXRelayListStatus
{
	GXRL_OK = 0,
	// shape and signature
	GXRL_TOO_LONG,			// over GX_RELAYLIST_WIRE_MAX, or a non-printable byte
	GXRL_NO_SIG,			// no '|' separating a signature
	GXRL_BAD_SIG_FORM,		// the signature is not 86 base64url characters / 64 bytes
	GXRL_BAD_SIG,			// verifies under no pinned key
	// meaning - only ever reached by a document one of our keys signed
	GXRL_BAD_VERSION,		// field 0 is not 2
	GXRL_NO_SERIAL,			// fewer than 10 fields: a pre-S1-3 manifest, not a relay list
	GXRL_BAD_FIELD,			// ordinal or serial is not a bounded decimal
	GXRL_BAD_ITEM,			// a relay item in field 4 or 8 is malformed (the WHOLE document goes)
	GXRL_NO_HOME,			// field 4 is empty
	// freshness
	GXRL_RANK_RETIRED,		// below the highest rank this install has accepted, or the build's minimum
	GXRL_BELOW_FLOOR,		// serial below its rank's floor
	GXRL_BELOW_BUILD,		// serial older than this build's commit time
	GXRL_TOO_FAR_AHEAD,		// serial more than GX_RELAYLIST_HORIZON_S past this machine's clock (on arrival)
};

const char *gxRelayListStatusName(GXRelayListStatus st);

struct GXRelayListDoc
{
	int rank;							// 1..3, which pinned key verified it
	unsigned long long ordinal;			// field 1, parsed and NOT consulted (§8.7)
	unsigned long long serial;			// field 9, raw
	GXRelayEndpoint home;				// field 4's FIRST entry
	int nalt;
	GXRelayEndpoint alt[GX_RELAYLIST_MAX_ITEMS];
};

// The pinned keys, rank order. A NULL or all-zero entry is an UNPROVISIONED slot and is skipped.
struct GXRelayKeySet
{
	const unsigned char *keys[GX_RELAYLIST_RANKS];
};

// Shape, signature, meaning. No freshness: that needs state, see gxRelayListJudge. `out` is written only on
// GXRL_OK. `wire` need not be NUL-terminated; exactly `len` bytes are read.
GXRelayListStatus gxRelayListVerify(const GXRelayKeySet *keys, const char *wire, size_t len,
	GXRelayListDoc *out);

// Persisted between launches. Never fails to parse: an unreadable file is a fresh install, which can
// only make this machine MORE permissive than it was, bounded by the build's own floor and minimum rank.
struct GXRelayListState
{
	int rank;											// highest rank ever FULLY accepted, 0 = none
	unsigned long long floor[GX_RELAYLIST_RANKS + 1];	// per rank: highest serial accepted; [0] unused
	unsigned long long hwm;								// wall-clock high-water mark
};

void gxRelayListStateInit(GXRelayListState *st);
void gxRelayListStateParse(const char *text, GXRelayListState *st);
// Returns the bytes written (excluding the NUL), 0 if `cap` is too small.
size_t gxRelayListStateRender(const GXRelayListState *st, char *out, size_t cap);

// max(wallNow, st->hwm): the clock every arrival is judged on.
unsigned long long gxRelayListNowEff(const GXRelayListState *st, unsigned long long wallNow);
// Nonzero when `serial` is no more than GX_RELAYLIST_HORIZON_S past `nowEff` (overflow-safe).
int gxRelayListWithinHorizon(unsigned long long serial, unsigned long long nowEff);

// May `doc` be used by an install in state `st`? buildFloor is this build's commit time (0 = no compiled-in
// floor), minRank its compiled-in minimum rank. The horizon is NOT judged here: it applies to an ARRIVAL
// (gxRelayListStoreOffer), never to the engine's own cache at load.
GXRelayListStatus gxRelayListJudge(const GXRelayListState *st, const GXRelayListDoc *doc,
	unsigned long long buildFloor, int minRank);

// Nonzero when (rankA, serialA) is STRICTLY newer than (rankB, serialB). Ties are not newer, so a held
// document is never replaced by its equal.
int gxRelayListNewer(int rankA, unsigned long long serialA, int rankB, unsigned long long serialB);

// Record a FULLY accepted document: raise the rank, its rank's floor and the high-water mark.
void gxRelayListAccept(GXRelayListState *st, const GXRelayListDoc *doc, unsigned long long nowEff);

// "host" or "host:port" -> parts. Port defaults to `defPort`. 0 if the text is not a plain host[:port]
// (empty, a host that does not fit hostCap or has a character outside [A-Za-z0-9.-], a port that is not
// 1..65533 - the game port is +2). Used for RelayAddress pins and test overrides.
int gxRelayParseHostPort(const char *text, char *host, size_t hostCap, unsigned short *port,
	unsigned short defPort);

// Where the relay address came from - OptionPreferences::getRelayAddress()'s whole decision, here so it is
// tested without an engine (scripts/test/relay-list-test.cpp, row 6).
enum GXRelaySource
{
	GX_RELAY_SRC_NONE = 0,		// GENERALSX_NO_RELAY, or RelayAddress = none: no relay at all (unchanged)
	GX_RELAY_SRC_PIN,			// a non-blank RelayAddress: the player's or the Windows launcher's choice
	GX_RELAY_SRC_LIST,			// absent/blank RelayAddress and a verified list is held: its home
	GX_RELAY_SRC_BOOTSTRAP,		// absent/blank RelayAddress and no list: the compiled-in name (as before)
};

// envOptOut: GENERALSX_NO_RELAY is set to a value that is not empty and does not start with '0'.
// key: the RelayAddress value, NULL when the key is absent. listHome: NULL when no list is held.
// bootstrap: "host" or "host:port" (a port only from a test build's override, GXRelayList::bootstrap).
// A pin that is not a plain host[:port] is returned VERBATIM on the default port - exactly what the engine
// did with it before ports were data, so a pin this cannot parse fails (or works) the way it always did.
// launcherKey: the RelayAddressLauncher value (NULL when absent) - the Windows launcher writes the SAME value there as
// in RelayAddress (gxlaunch.c publish_relay). While the two agree, RelayAddress is the LAUNCHER's choice, not the
// player's: with a verified list held the list's home wins (the engine's own rank/serial floor governs on Windows too,
// ); with none, the launcher's value is used as before. A RelayAddress the player edited no longer
// matches, and is a pin. A RelayAddress naming exactly the bootstrap on its port, with no marker, is a LEGACY
// launcher-written value (every launcher before the marker wrote the bootstrap name there) and is treated the same.
GXRelaySource gxRelayResolve(int envOptOut, const char *key, const char *launcherKey, const GXRelayEndpoint *listHome,
	const char *bootstrap, char *host, size_t hostCap, unsigned short *port);

// §3.5: a standby is used ONLY when the address is the list's own home - no pin, or a pin naming exactly
// the home (host case-insensitive, and port). A pinned private relay never fails over onto ours.
int gxRelayStandbyAllowed(GXRelaySource src, const char *host, unsigned short port, const GXRelayEndpoint *listHome);

// May this client send GXUPD to the compiled-in bootstrap? Not when relay play is off, and not for a pin to
// somebody else's relay (a private or lab relay must not make a client talk to ours); yes for the list's
// home, the bootstrap itself, or a pin naming either.
int gxRelayMayFetch(GXRelaySource src, const char *host, unsigned short port, const GXRelayEndpoint *listHome,
	const char *bootstrap);

// ---------------------------------------------------------------------------------------------------
// The store: state + caches + candidates, on explicit paths (the engine fills them in; the test points
// them at a scratch folder). Not thread-safe by itself; the engine wraps it in a mutex.
// ---------------------------------------------------------------------------------------------------

enum { GX_RELAYLIST_PATH_MAX = 1024, GX_RELAYLIST_LAUNCHER_CACHES = 2 };

struct GXRelayListConfig
{
	GXRelayKeySet keys;
	int minRank;									// compiled-in minimum rank (1 today)
	unsigned long long buildFloor;					// this build's commit time; 0 = none
	char statePath[GX_RELAYLIST_PATH_MAX];			// "" = keep nothing on disk
	char cachePath[GX_RELAYLIST_PATH_MAX];
	int nlauncher;
	char launcherCache[GX_RELAYLIST_LAUNCHER_CACHES][GX_RELAYLIST_PATH_MAX];
	int quiet;										// tests: no stderr lines
};

struct GXRelayListStore
{
	GXRelayListConfig cfg;
	GXRelayListState state;
	int have;										// a document is held
	GXRelayListDoc doc;
	char wire[GX_RELAYLIST_WIRE_MAX + 1];
	size_t wireLen;
};

// Read state, the own cache and the launcher caches; hold the best. Writes back only what it accepts.
void gxRelayListStoreLoad(GXRelayListStore *s, const GXRelayListConfig *cfg, unsigned long long wallNow);
// A document from anywhere (a launcher file, a GXUPD reply). Verified, judged, and adopted + persisted only
// if strictly newer than what is held. Returns the verdict; GXRL_OK with *adopted = 0 means "valid, not newer".
GXRelayListStatus gxRelayListStoreOffer(GXRelayListStore *s, const char *wire, size_t len,
	unsigned long long wallNow, const char *source, int *adopted);

// ---------------------------------------------------------------------------------------------------
// GXUPD fetch: one UDP socket, "GXUPD <ordinal>" to every target, replies taken ONLY from a target's
// exact (address, port). Synchronous; the engine runs it on a background thread. Returns the number of
// verified-and-offered replies. `onReply` gets each reply's wire bytes (after "GXMANI ").
// ---------------------------------------------------------------------------------------------------

typedef void (*GXRelayListReplyFn)(void *ctx, const char *wire, size_t len, const char *fromHost);

int gxRelayListFetch(const GXRelayEndpoint *targets, int ntargets, unsigned long long ordinal,
	unsigned int windowMs, GXRelayListReplyFn onReply, void *ctx);

// ---------------------------------------------------------------------------------------------------
// The engine's process-wide view. Defined only in the engine build (not GX_RELAYLIST_STANDALONE).
// ---------------------------------------------------------------------------------------------------

class GXRelayList
{
public:
	// The held list's home. FALSE when no verified list is held (the caller uses the bootstrap). The first
	// call loads and LATCHES the process's home; later calls return the same answer.
	static bool home(char *host, size_t hostCap, unsigned short *port);
	// Standby i (0-based) of the latched list. FALSE past the end or with no list. The pin rule (a standby
	// only when nothing is pinned or the pin IS the home) is OptionPreferences', which knows the pin.
	static bool standby(int i, char *host, size_t hostCap, unsigned short *port);
	// The latched list's identity, for S1-4's start-time tag: rank, serial, release ordinal. FALSE = none.
	static bool identity(int *rank, unsigned long long *serial, unsigned long long *ordinal);
	// The Online lobby was entered. Starts ONE background GXUPD refresh per process when allowFetch, to the
	// bootstrap (host[:port], from OptionPreferences, which owns the name - already passed through bootstrap())
	// plus the held home and standbys; its result is cached for the next launch (see "latched" above).
	static void lobbyEntry(bool allowFetch, const char *bootstrapHost);
	// The bootstrap to use: `compiled` (OptionPreferences' GX_BOOTSTRAP_RELAY) in every shipped build; in a
	// GX_RELAYLIST_TEST_KEYS build GX_RELAYLIST_BOOTSTRAP=host[:port] when set.
	static const char *bootstrap(const char *compiled);
};
