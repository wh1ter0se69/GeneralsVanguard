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

// Engine half of the signed relay list: the newest verified list's HOME is the default relay when none is pinned.
// Check order is the security property: shape, SIGNATURE (ranked keys K1<K2<K3), meaning, freshness.
// Freshness is (rank, serial): a lower rank than the best ever accepted is refused, as is a serial >7 days ahead.
// One malformed item refuses the whole list; the home is latched per process; else the bootstrap.

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

// envOptOut: GENERALSX_NO_RELAY non-empty and not starting with '0'. key: RelayAddress, NULL when absent.
// listHome: NULL when no list is held. bootstrap: "host" or "host:port". An unparseable pin is returned VERBATIM.
// launcherKey: RelayAddressLauncher. While equal to RelayAddress, that is the launcher's choice and a list's home wins;
// an edited RelayAddress is a pin; a bare bootstrap value with no marker counts as launcher-written.
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
