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

// GeneralsX @feature Claude 29/10/2026 S1-3 relay-list-v2. See the header for the design; this file keeps
// to its order of checks. GX_RELAYLIST_STANDALONE (scripts/test/relay-list-test.sh) compiles the pure layer,
// the store and the fetch without any engine header; the engine build adds the process-wide class at the end.

#include "GameClient/GXRelayList.h"
#include "GameClient/GXEd25519.h"
#include "GameClient/GXReplaceFile.h"	// gxCreateTempFor / gxCommitAndReplace: the cache never half-written

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>	// TARGET_OS_IPHONE: iOS has no launcher cache to read
#endif

#ifdef _WIN32
#include <windows.h>
#include <winsock.h>	// the engine's sockets are Winsock 1.1 (GXLoopback.cpp); winsock2 would clash with windows.h
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// ===================================================================================================
// Pure layer
// ===================================================================================================

const char *gxRelayListStatusName(GXRelayListStatus st)
{
	switch (st)
	{
		case GXRL_OK:			return "ok";
		case GXRL_TOO_LONG:		return "too long or not printable ASCII";
		case GXRL_NO_SIG:		return "no signature";
		case GXRL_BAD_SIG_FORM:	return "signature is not 64 bytes of base64url";
		case GXRL_BAD_SIG:		return "signature verifies under no pinned key";
		case GXRL_BAD_VERSION:	return "format version is not 2";
		case GXRL_NO_SERIAL:	return "no serial (field 9) - a pre-S1-3 manifest, not a relay list";
		case GXRL_BAD_FIELD:	return "ordinal or serial is not a bounded decimal";
		case GXRL_BAD_ITEM:		return "a relay item is malformed - the whole document is refused";
		case GXRL_NO_HOME:		return "no home relay";
		case GXRL_RANK_RETIRED:	return "signed by a key rank this install has retired";
		case GXRL_BELOW_FLOOR:	return "older than the list already accepted under that key";
		case GXRL_BELOW_BUILD:	return "older than this build";
		case GXRL_TOO_FAR_AHEAD:	return "signed more than 7 days ahead of this machine's clock";
	}
	return "unknown";
}

namespace
{

const size_t SIG_CHARS = 86;	// 64 bytes of unpadded base64url
// A serial is a Unix time; 12 digits reach the year 33658. Bounding it keeps every sum below overflow.
const unsigned long long SERIAL_MAX = 999999999999ULL;
const unsigned long long ORDINAL_MAX = 999999999999ULL;

int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-') return 62;
	if (c == '_') return 63;
	return -1;
}

// Exactly 86 base64url characters -> 64 bytes, and the two trailing pad bits must be zero: a signature
// with a second spelling is one more thing two verifiers can disagree about.
bool decodeSig(const char *s, size_t n, unsigned char out[64])
{
	if (n != SIG_CHARS)
		return false;
	unsigned int acc = 0;
	int bits = 0;
	size_t o = 0;
	for (size_t i = 0; i < n; ++i)
	{
		const int v = b64val(s[i]);
		if (v < 0)
			return false;
		acc = (acc << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			if (o >= 64)
				return false;
			out[o++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	return o == 64 && (acc & ((1u << bits) - 1u)) == 0;
}

bool keyIsZero(const unsigned char *k)
{
	if (k == NULL)
		return true;
	unsigned char any = 0;
	for (int i = 0; i < 32; ++i)
		any |= k[i];
	return any == 0;
}

// 1..12 decimal digits, value <= max. No sign, no space, no leading '+'.
bool parseDec(const char *s, size_t n, unsigned long long max, unsigned long long *out)
{
	if (n == 0 || n > 12)
		return false;
	unsigned long long v = 0;
	for (size_t i = 0; i < n; ++i)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (unsigned long long)(s[i] - '0');
	}
	if (v > max)
		return false;
	*out = v;
	return true;
}

bool hostChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool regionChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

// One "host,port,region" item. The port is the LOBBY port and must leave room for the game port (+2).
bool parseItem(const char *s, size_t n, GXRelayEndpoint *out)
{
	const char *c1 = (const char *)memchr(s, ',', n);
	if (c1 == NULL)
		return false;
	const char *c2 = (const char *)memchr(c1 + 1, ',', (size_t)(s + n - (c1 + 1)));
	if (c2 == NULL || memchr(c2 + 1, ',', (size_t)(s + n - (c2 + 1))) != NULL)
		return false;				// exactly three parts
	const size_t hn = (size_t)(c1 - s), pn = (size_t)(c2 - c1 - 1), rn = (size_t)(s + n - c2 - 1);
	if (hn == 0 || hn > GX_RELAYLIST_HOST_MAX || rn == 0 || rn > GX_RELAYLIST_REGION_MAX)
		return false;
	for (size_t i = 0; i < hn; ++i)
		if (!hostChar(s[i]))
			return false;
	for (size_t i = 0; i < rn; ++i)
		if (!regionChar(c2[1 + i]))
			return false;
	unsigned long long port = GX_RELAYLIST_DEFAULT_PORT;
	if (pn > 0 && (pn > 5 || !parseDec(c1 + 1, pn, 65535 - GX_RELAYLIST_GAME_PORT_OFFSET, &port) || port == 0))
		return false;
	memcpy(out->host, s, hn);
	out->host[hn] = '\0';
	out->port = (unsigned short)port;
	memcpy(out->region, c2 + 1, rn);
	out->region[rn] = '\0';
	return true;
}

// A relay field: items joined by ';'. ALL must parse - one bad item refuses the document (§3.5).
bool parseRelayField(const char *s, size_t n, GXRelayEndpoint *out, int *count)
{
	*count = 0;
	if (n == 0)
		return true;
	size_t start = 0;
	for (size_t i = 0; i <= n; ++i)
	{
		if (i < n && s[i] != ';')
			continue;
		if (*count >= GX_RELAYLIST_MAX_ITEMS)
			return false;
		if (!parseItem(s + start, i - start, &out[*count]))
			return false;
		++*count;
		start = i + 1;
	}
	return true;
}

} // namespace

GXRelayListStatus gxRelayListVerify(const GXRelayKeySet *keys, const char *wire, size_t len, GXRelayListDoc *out)
{
	// SHAPE. Length and character class only; nothing is interpreted yet.
	if (wire == NULL || len == 0 || len > GX_RELAYLIST_WIRE_MAX)
		return GXRL_TOO_LONG;
	for (size_t i = 0; i < len; ++i)
		if ((unsigned char)wire[i] < 0x21 || (unsigned char)wire[i] > 0x7e)
			return GXRL_TOO_LONG;
	size_t cut = len;
	while (cut > 0 && wire[cut - 1] != '|')
		--cut;
	if (cut == 0)
		return GXRL_NO_SIG;
	const size_t plen = cut - 1;			// the payload is everything before the LAST '|', verbatim
	unsigned char sig[64];
	if (!decodeSig(wire + cut, len - cut, sig))
		return GXRL_BAD_SIG_FORM;

	// SIGNATURE, rank order. The keys are distinct, so at most one verifies; the first that does names
	// the rank. An unprovisioned (zero) slot is skipped by an explicit test, not left to the verifier's
	// small-order rejection - see GXIdentityAssertion.cpp's note on keyIsPinned for why.
	int rank = 0;
	for (int r = 0; r < GX_RELAYLIST_RANKS && rank == 0; ++r)
	{
		const unsigned char *k = keys != NULL ? keys->keys[r] : NULL;
		if (keyIsZero(k))
			continue;
		if (gx_ed25519_verify(k, sig, wire, plen) == 1)
			rank = r + 1;
	}
	if (rank == 0)
		return GXRL_BAD_SIG;

	// MEANING. A document one of our keys signed can still be one this client disagrees with, and it is
	// refused whole rather than partly used.
	size_t fs[16], fl[16];
	int nf = 0;
	size_t start = 0;
	for (size_t i = 0; i <= plen && nf < 16; ++i)
	{
		if (i < plen && wire[i] != '|')
			continue;
		fs[nf] = start;
		fl[nf] = i - start;
		++nf;
		start = i + 1;
	}
	if (nf < 1 || fl[0] != 1 || wire[fs[0]] != '2')
		return GXRL_BAD_VERSION;
	if (nf < 10)
		return GXRL_NO_SERIAL;

	GXRelayListDoc d;
	memset(&d, 0, sizeof(d));
	d.rank = rank;
	if (!parseDec(wire + fs[1], fl[1], ORDINAL_MAX, &d.ordinal)
		|| !parseDec(wire + fs[9], fl[9], SERIAL_MAX, &d.serial) || d.serial == 0)
		return GXRL_BAD_FIELD;

	GXRelayEndpoint homes[GX_RELAYLIST_MAX_ITEMS];
	int nhome = 0;
	if (!parseRelayField(wire + fs[4], fl[4], homes, &nhome))
		return GXRL_BAD_ITEM;
	if (!parseRelayField(wire + fs[8], fl[8], d.alt, &d.nalt))
		return GXRL_BAD_ITEM;
	if (nhome == 0)
		return GXRL_NO_HOME;
	d.home = homes[0];			// §3.1: the FIRST entry is home; the signer keeps it to one

	*out = d;
	return GXRL_OK;
}

void gxRelayListStateInit(GXRelayListState *st)
{
	memset(st, 0, sizeof(*st));
}

void gxRelayListStateParse(const char *text, GXRelayListState *st)
{
	gxRelayListStateInit(st);
	if (text == NULL)
		return;
	const char *p = text;
	while (*p)
	{
		const char *eol = strchr(p, '\n');
		const size_t n = eol ? (size_t)(eol - p) : strlen(p);
		char line[128];
		if (n < sizeof(line))
		{
			memcpy(line, p, n);
			line[n] = '\0';
			unsigned long long a = 0;
			int r = 0;
			if (sscanf(line, "rank %d", &r) == 1 && r >= 0 && r <= GX_RELAYLIST_RANKS)
				st->rank = r;
			else if (sscanf(line, "hwm %llu", &a) == 1 && a <= SERIAL_MAX)
				st->hwm = a;
			else if (sscanf(line, "floor %d %llu", &r, &a) == 2 && r >= 1 && r <= GX_RELAYLIST_RANKS
				&& a <= SERIAL_MAX)
				st->floor[r] = a;
			// "cached" and "future" lines from the stored-clamp design this replaced are ignored on purpose.
		}
		p = eol ? eol + 1 : p + n;
	}
}

size_t gxRelayListStateRender(const GXRelayListState *st, char *out, size_t cap)
{
	std::string s = "gxrelaylist 1\n";
	char line[96];
	snprintf(line, sizeof(line), "rank %d\nhwm %llu\n", st->rank, st->hwm);
	s += line;
	for (int r = 1; r <= GX_RELAYLIST_RANKS; ++r)
	{
		if (st->floor[r] == 0)
			continue;
		snprintf(line, sizeof(line), "floor %d %llu\n", r, st->floor[r]);
		s += line;
	}
	if (s.size() + 1 > cap)
		return 0;
	memcpy(out, s.c_str(), s.size() + 1);
	return s.size();
}

unsigned long long gxRelayListNowEff(const GXRelayListState *st, unsigned long long wallNow)
{
	return wallNow > st->hwm ? wallNow : st->hwm;
}

int gxRelayListWithinHorizon(unsigned long long serial, unsigned long long nowEff)
{
	// nowEff is a wall-clock reading or the stored mark, both bounded by the parsers; the guard is for a caller that
	// passes something else.
	const unsigned long long limit = nowEff > ~0ULL - GX_RELAYLIST_HORIZON_S ? ~0ULL : nowEff + GX_RELAYLIST_HORIZON_S;
	return serial <= limit;
}

GXRelayListStatus gxRelayListJudge(const GXRelayListState *st, const GXRelayListDoc *doc,
	unsigned long long buildFloor, int minRank)
{
	if (doc->rank < minRank || doc->rank < st->rank)
		return GXRL_RANK_RETIRED;
	// The build's own commit time (§8.5: serial, never ordinal): a fresh install is not fed a list older than itself.
	if (buildFloor != 0 && doc->serial < buildFloor)
		return GXRL_BELOW_BUILD;
	// Its OWN rank's floor, never a lower rank's (§3.3): a poisoned K1 floor cannot block a K2 document.
	// Equal is allowed: the same document fetched again is not a rollback.
	if (doc->serial < st->floor[doc->rank])
		return GXRL_BELOW_FLOOR;
	return GXRL_OK;
}

int gxRelayListNewer(int rankA, unsigned long long serialA, int rankB, unsigned long long serialB)
{
	if (rankA != rankB)
		return rankA > rankB;
	return serialA > serialB;
}

void gxRelayListAccept(GXRelayListState *st, const GXRelayListDoc *doc, unsigned long long nowEff)
{
	if (doc->rank > st->rank)
		st->rank = doc->rank;
	if (doc->serial > st->floor[doc->rank])
		st->floor[doc->rank] = doc->serial;
	if (nowEff > st->hwm)
		st->hwm = nowEff;
}

int gxRelayParseHostPort(const char *text, char *host, size_t hostCap, unsigned short *port, unsigned short defPort)
{
	if (text == NULL || host == NULL || hostCap == 0)
		return 0;
	const char *colon = strrchr(text, ':');
	const size_t hn = colon ? (size_t)(colon - text) : strlen(text);
	if (hn == 0 || hn + 1 > hostCap)
		return 0;
	for (size_t i = 0; i < hn; ++i)
		if (!hostChar(text[i]))
			return 0;
	unsigned long long p = defPort;
	if (colon != NULL)
	{
		const size_t pn = strlen(colon + 1);
		if (pn == 0 || pn > 5 || !parseDec(colon + 1, pn, 65535 - GX_RELAYLIST_GAME_PORT_OFFSET, &p) || p == 0)
			return 0;
	}
	memcpy(host, text, hn);
	host[hn] = '\0';
	*port = (unsigned short)p;
	return 1;
}

namespace
{

bool sameHost(const char *a, const char *b)
{
	for (;; ++a, ++b)
	{
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
		if (x != y)
			return false;
		if (x == '\0')
			return true;
	}
}

// AsciiString::trim's whitespace (isspace), so "blank" means exactly what it meant before.
bool isWs(char c)
{
	return isspace((unsigned char)c) != 0;
}

bool isBlank(const char *s)
{
	for (; *s; ++s)
		if (!isWs(*s))
			return false;
	return true;
}

} // namespace

namespace
{

std::string trimmedWs(const char *v)
{
	while (*v != '\0' && isWs(*v))
		++v;
	std::string out(v);
	while (!out.empty() && isWs(out.back()))
		out.pop_back();
	return out;
}

} // namespace

GXRelaySource gxRelayResolve(int envOptOut, const char *key, const char *launcherKey, const GXRelayEndpoint *listHome,
	const char *bootstrap, char *host, size_t hostCap, unsigned short *port)
{
	host[0] = '\0';
	*port = GX_RELAYLIST_DEFAULT_PORT;
	// Checked BEFORE the key, so a sandbox can force "no relay" even over an inherited Options.ini.
	if (envOptOut)
		return GX_RELAY_SRC_NONE;
	// The Windows launcher's own value (Codex r6): not a pin while a verified list is held. "none" always means no
	// relay at all, whatever a marker says.
	//
	// AND THE LEGACY ONE (Codex r8). Every launcher before the marker wrote RelayAddress = the list's field-4 home, which
	// has only ever been the compiled-in bootstrap name, and a Tauri-driven install runs the updater with --check only,
	// so the marker may never be written there. A RelayAddress naming exactly the bootstrap on the default port is
	// therefore read as the launcher's too: a pin to that host can only mean "the official relay", which is what
	// following the signed list's home is. A pin to any other host, or to that host on another port, stays a pin.
	bool launcherWritten = key != NULL && !isBlank(key) && !sameHost(trimmedWs(key).c_str(), "none")
		&& launcherKey != NULL && sameHost(trimmedWs(key).c_str(), trimmedWs(launcherKey).c_str());
	if (!launcherWritten && key != NULL && !isBlank(key) && bootstrap != NULL)
	{
		char ph[256], bh[256];
		unsigned short pp = GX_RELAYLIST_DEFAULT_PORT, bp = GX_RELAYLIST_DEFAULT_PORT;
		const std::string pin = trimmedWs(key);
		launcherWritten = gxRelayParseHostPort(pin.c_str(), ph, sizeof(ph), &pp, GX_RELAYLIST_DEFAULT_PORT)
			&& gxRelayParseHostPort(bootstrap, bh, sizeof(bh), &bp, GX_RELAYLIST_DEFAULT_PORT)
			&& pp == bp && sameHost(ph, bh);
	}
	if (key == NULL || isBlank(key) || (launcherWritten && listHome != NULL))
	{
		if (listHome != NULL)
		{
			snprintf(host, hostCap, "%s", listHome->host);
			*port = listHome->port;
			return GX_RELAY_SRC_LIST;
		}
		const char *b = bootstrap != NULL ? bootstrap : "";
		if (!gxRelayParseHostPort(b, host, hostCap, port, GX_RELAYLIST_DEFAULT_PORT))
		{
			snprintf(host, hostCap, "%s", b);
			*port = GX_RELAYLIST_DEFAULT_PORT;
		}
		return GX_RELAY_SRC_BOOTSTRAP;
	}
	// Trim the way OptionPreferences always did (AsciiString::trim: isspace at both ends).
	while (isWs(*key))
		++key;
	std::string v(key);
	while (!v.empty() && isWs(v.back()))
		v.pop_back();
	if (sameHost(v.c_str(), "none"))
		return GX_RELAY_SRC_NONE;
	if (!gxRelayParseHostPort(v.c_str(), host, hostCap, port, GX_RELAYLIST_DEFAULT_PORT))
	{
		snprintf(host, hostCap, "%s", v.c_str());
		*port = GX_RELAYLIST_DEFAULT_PORT;
	}
	return GX_RELAY_SRC_PIN;
}

int gxRelayStandbyAllowed(GXRelaySource src, const char *host, unsigned short port, const GXRelayEndpoint *listHome)
{
	if (listHome == NULL)
		return 0;
	if (src == GX_RELAY_SRC_LIST)
		return 1;
	return src == GX_RELAY_SRC_PIN && port == listHome->port && sameHost(host, listHome->host);
}

int gxRelayMayFetch(GXRelaySource src, const char *host, unsigned short port, const GXRelayEndpoint *listHome,
	const char *bootstrap)
{
	if (src == GX_RELAY_SRC_LIST || src == GX_RELAY_SRC_BOOTSTRAP)
		return 1;
	if (src != GX_RELAY_SRC_PIN)
		return 0;
	if (gxRelayStandbyAllowed(src, host, port, listHome))
		return 1;
	char bh[256];
	unsigned short bp = GX_RELAYLIST_DEFAULT_PORT;
	return bootstrap != NULL && gxRelayParseHostPort(bootstrap, bh, sizeof(bh), &bp, GX_RELAYLIST_DEFAULT_PORT)
		&& port == bp && sameHost(host, bh);
}

// ===================================================================================================
// The store
// ===================================================================================================

namespace
{

// Up to cap bytes; NULL-free content is not required here (verify checks the bytes). -1 = unreadable,
// -2 = larger than cap. A missing file and an unreadable one are both "nothing from this source".
long readFileBounded(const char *path, char *buf, size_t cap)
{
	if (path == NULL || path[0] == '\0')
		return -1;
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	const size_t n = fread(buf, 1, cap + 1, f);
	const bool err = ferror(f) != 0;
	fclose(f);
	if (err)
		return -1;
	if (n > cap)
		return -2;
	return (long)n;
}

bool writeFileAtomic(const char *path, const char *data, size_t n)
{
	if (path == NULL || path[0] == '\0')
		return false;
	std::string tmp;
	FILE *f = gxCreateTempFor(path, tmp);
	if (f == NULL)
		return false;
	const bool wrote = fwrite(data, 1, n, f) == n;
	if (!wrote)
	{
		fclose(f);
		remove(tmp.c_str());
		return false;
	}
	return gxCommitAndReplace(f, tmp.c_str(), path);
}

void say(const GXRelayListStore *s, const char *fmt, const char *a, const char *b)
{
	if (s->cfg.quiet)
		return;
	fprintf(stderr, "[GXRELAYLIST] ");
	fprintf(stderr, fmt, a, b);
	fputc('\n', stderr);
	fflush(stderr);
}

void sayUsing(const GXRelayListStore *s, const char *source)
{
	if (s->cfg.quiet)
		return;
	fprintf(stderr, "[GXRELAYLIST] using %s: key rank %d, serial %llu, home %s:%u, %d standby\n", source,
		s->doc.rank, s->doc.serial, s->doc.home.host, (unsigned)s->doc.home.port, s->doc.nalt);
	fflush(stderr);
}

void persist(GXRelayListStore *s)
{
	char text[1024];
	const size_t n = gxRelayListStateRender(&s->state, text, sizeof(text));
	// The document first, then the state that describes it. A crash between the two leaves a cached document
	// above the stored floor; the next load re-judges it against that floor, and accepts it - it had passed the
	// horizon on its arrival, which is the one check a load does not repeat.
	// And the state ONLY if the document landed (Codex relaylist-v2-r9): a floor saved for a document that is not on disk
	// makes the next load refuse the older cache it still holds - the working home and standbys lost for nothing. With
	// both unsaved, this session keeps the newer list in memory and the next launch is offered it again.
	if (s->have && s->cfg.cachePath[0] != '\0' && !writeFileAtomic(s->cfg.cachePath, s->wire, s->wireLen))
	{
		say(s, "could not save the list to %s%s - its state is not advanced either", s->cfg.cachePath, "");
		return;
	}
	if (n > 0 && s->cfg.statePath[0] != '\0')
		writeFileAtomic(s->cfg.statePath, text, n);
}

} // namespace

void gxRelayListStoreLoad(GXRelayListStore *s, const GXRelayListConfig *cfg, unsigned long long wallNow)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	gxRelayListStateInit(&s->state);

	char buf[GX_RELAYLIST_WIRE_MAX + 1];
	long n = readFileBounded(cfg->statePath, buf, 1024);
	if (n > 0)
	{
		buf[n] = '\0';
		gxRelayListStateParse(buf, &s->state);
	}
	const unsigned long long hwmBefore = s->state.hwm;
	const unsigned long long nowEff = gxRelayListNowEff(&s->state, wallNow);
	s->state.hwm = nowEff;

	// The engine's own cache: verified and judged against the floor, but NOT against the horizon - it passed that on
	// its arrival, and a clock that has since gone slow must not discard an honest cache (§8.9).
	n = readFileBounded(cfg->cachePath, buf, GX_RELAYLIST_WIRE_MAX);
	if (n > 0)
	{
		GXRelayListDoc d;
		const GXRelayListStatus st = gxRelayListVerify(&cfg->keys, buf, (size_t)n, &d);
		const GXRelayListStatus j = (st == GXRL_OK) ? gxRelayListJudge(&s->state, &d, cfg->buildFloor, cfg->minRank) : st;
		if (j != GXRL_OK)
			say(s, "refused own cache: %s%s", gxRelayListStatusName(j), "");
		else
		{
			s->have = 1;
			s->doc = d;
			memcpy(s->wire, buf, (size_t)n);
			s->wire[n] = '\0';
			s->wireLen = (size_t)n;
			gxRelayListAccept(&s->state, &d, nowEff);		// a floor lost with a torn state file comes back
		}
	}

	// The launchers' caches: offered like any network reply - verified here, adopted only if newer.
	bool offeredNewer = false;
	for (int i = 0; i < cfg->nlauncher; ++i)
	{
		n = readFileBounded(cfg->launcherCache[i], buf, GX_RELAYLIST_WIRE_MAX);
		if (n == -2)
			say(s, "refused launcher cache %s: %s", cfg->launcherCache[i], "larger than any manifest");
		if (n <= 0)
			continue;
		int adopted = 0;
		gxRelayListStoreOffer(s, buf, (size_t)n, wallNow, "launcher cache", &adopted);
		offeredNewer = offeredNewer || adopted;
	}

	if (s->have && !offeredNewer)
		sayUsing(s, "own cache");
	else if (!s->have)
		say(s, "no verified relay list held - the compiled-in bootstrap is home%s%s", "", "");
	if (s->state.hwm != hwmBefore && !offeredNewer)
		persist(s);		// the clock mark only; Offer persisted everything it adopted
}

GXRelayListStatus gxRelayListStoreOffer(GXRelayListStore *s, const char *wire, size_t len,
	unsigned long long wallNow, const char *source, int *adopted)
{
	*adopted = 0;
	GXRelayListDoc d;
	GXRelayListStatus st = gxRelayListVerify(&s->cfg.keys, wire, len, &d);
	if (st != GXRL_OK)
	{
		say(s, "refused %s: %s", source, gxRelayListStatusName(st));
		return st;
	}
	const unsigned long long nowEff = gxRelayListNowEff(&s->state, wallNow);
	// An ARRIVAL further ahead than the horizon is refused (header: "THE HORIZON, NOT A CLAMP").
	st = gxRelayListWithinHorizon(d.serial, nowEff) ? gxRelayListJudge(&s->state, &d, s->cfg.buildFloor, s->cfg.minRank)
		: GXRL_TOO_FAR_AHEAD;
	if (st != GXRL_OK)
	{
		say(s, "refused %s: %s", source, gxRelayListStatusName(st));
		return st;
	}
	if (s->have && !gxRelayListNewer(d.rank, d.serial, s->doc.rank, s->doc.serial))
		return GXRL_OK;			// valid, and not newer than what is held: nothing to do, nothing to say

	s->have = 1;
	s->doc = d;
	memcpy(s->wire, wire, len);
	s->wire[len] = '\0';
	s->wireLen = len;
	gxRelayListAccept(&s->state, &d, nowEff);
	persist(s);
	*adopted = 1;
	sayUsing(s, source);
	return GXRL_OK;
}

// ===================================================================================================
// GXUPD fetch
// ===================================================================================================

namespace
{

#ifdef _WIN32
typedef SOCKET FetchSock;
const FetchSock FETCH_NONE = INVALID_SOCKET;
void sockClose(FetchSock s) { closesocket(s); }
#else
typedef int FetchSock;
const FetchSock FETCH_NONE = -1;
void sockClose(FetchSock s) { close(s); }
#endif

// IPv4, network order; 0 = unresolved. On POSIX getaddrinfo, because this runs off the main thread and
// gethostbyname's static result is not thread-safe there; Winsock's gethostbyname is per-thread.
unsigned long resolve4(const char *host)
{
#ifdef _WIN32
	const unsigned long lit = inet_addr(host);
	if (lit != INADDR_NONE)
		return lit;
	const hostent *h = gethostbyname(host);
	if (h == NULL || h->h_addrtype != AF_INET || h->h_addr_list == NULL || h->h_addr_list[0] == NULL)
		return 0;
	unsigned long a;
	memcpy(&a, h->h_addr_list[0], 4);
	return a;
#else
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = NULL;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
		return 0;
	unsigned long a = ((const sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(res);
	return a;
#endif
}

} // namespace

int gxRelayListFetch(const GXRelayEndpoint *targets, int ntargets, unsigned long long ordinal,
	unsigned int windowMs, GXRelayListReplyFn onReply, void *ctx)
{
	if (targets == NULL || ntargets <= 0 || onReply == NULL)
		return 0;
	if (ntargets > 2 * GX_RELAYLIST_MAX_ITEMS)
		ntargets = 2 * GX_RELAYLIST_MAX_ITEMS;
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0)		// reference counted; see GXLoopback::open
		return 0;
#endif
	sockaddr_in addr[2 * GX_RELAYLIST_MAX_ITEMS];
	bool live[2 * GX_RELAYLIST_MAX_ITEMS];
	int nlive = 0;
	for (int i = 0; i < ntargets; ++i)
	{
		memset(&addr[i], 0, sizeof(addr[i]));
		addr[i].sin_family = AF_INET;
		addr[i].sin_port = htons(targets[i].port);
		addr[i].sin_addr.s_addr = resolve4(targets[i].host);
		live[i] = addr[i].sin_addr.s_addr != 0;
		nlive += live[i] ? 1 : 0;
	}

	int offered = 0;
	FetchSock s = nlive > 0 ? socket(AF_INET, SOCK_DGRAM, 0) : FETCH_NONE;
	if (s != FETCH_NONE)
	{
#ifdef _WIN32
		const DWORD tv = 100;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
		timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
		char req[40];
		const int reqLen = snprintf(req, sizeof(req), "GXUPD %llu", ordinal);
		const auto t0 = std::chrono::steady_clock::now();
		int sent = 0;
		char buf[GX_RELAYLIST_WIRE_MAX + 16];
		for (;;)
		{
			const unsigned int el = (unsigned int)std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
			if (el >= windowMs || nlive == 0)
				break;
			// Three sends spread over the window, to every target that has not answered: UDP loses things.
			if (sent < 3 && el >= (windowMs / 3) * (unsigned int)sent)
			{
				for (int i = 0; i < ntargets; ++i)
					if (live[i])
						sendto(s, req, reqLen, 0, (const sockaddr *)&addr[i], (int)sizeof(addr[i]));
				++sent;
			}
			sockaddr_in from;
#ifdef _WIN32
			int fromLen = sizeof(from);
#else
			socklen_t fromLen = sizeof(from);
#endif
			const int n = (int)recvfrom(s, buf, (int)sizeof(buf) - 1, 0, (sockaddr *)&from, &fromLen);
			if (n <= 0)
				continue;
			// The SOURCE is matched against the targets - address AND port - before a byte is read: a
			// datagram from anywhere else on this ephemeral port is not an answer (gxlaunch.c select_relay).
			int who = -1;
			for (int i = 0; i < ntargets && who < 0; ++i)
				if (live[i] && from.sin_addr.s_addr == addr[i].sin_addr.s_addr && from.sin_port == addr[i].sin_port)
					who = i;
			if (who < 0)
				continue;
			static const char TAG[] = "GXMANI ";
			const int tagLen = (int)sizeof(TAG) - 1;
			if (n <= tagLen || memcmp(buf, TAG, (size_t)tagLen) != 0)
				continue;
			live[who] = false;		// answered, whatever it said
			--nlive;
			if (n == tagLen + 1 && buf[tagLen] == '-')
				continue;			// "GXMANI -": this relay serves no manifest
			onReply(ctx, buf + tagLen, (size_t)(n - tagLen), targets[who].host);
			++offered;
		}
		sockClose(s);
	}
#ifdef _WIN32
	WSACleanup();
#endif
	return offered;
}

// ===================================================================================================
// The engine's process-wide view
// ===================================================================================================

#ifndef GX_RELAYLIST_STANDALONE

#include "Common/GlobalData.h"
#include "GameClient/GXUserDataDir.h"
#include "gitinfo.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace
{

#ifdef GX_RELAYLIST_TEST_KEYS
// TEST KEYS - scripts/test/relay-list-fixtures.py derives them from fixed public strings; never a production
// key. relay-list-fixtures.py --check fails if these markers and the fixtures disagree.
// GX_RELAYLIST_TEST_K1 79379e76ed44c7a1633c9b981396584ec3041a57f7c5f2ec14e65118159725b7
// GX_RELAYLIST_TEST_K2 3b83f149150388400756c2523a72e85bf50f46e584d5a4debb75180bbae1bd7e
// GX_RELAYLIST_TEST_K3 e69ff53e81b45a15680613c5ba2ff653cdb01a02fbfa56eec47a4e06fd360635
const unsigned char KEY1[32] = {
	0x79, 0x37, 0x9e, 0x76, 0xed, 0x44, 0xc7, 0xa1, 0x63, 0x3c, 0x9b, 0x98, 0x13, 0x96, 0x58, 0x4e,
	0xc3, 0x04, 0x1a, 0x57, 0xf7, 0xc5, 0xf2, 0xec, 0x14, 0xe6, 0x51, 0x18, 0x15, 0x97, 0x25, 0xb7,
};
const unsigned char KEY2[32] = {
	0x3b, 0x83, 0xf1, 0x49, 0x15, 0x03, 0x88, 0x40, 0x07, 0x56, 0xc2, 0x52, 0x3a, 0x72, 0xe8, 0x5b,
	0xf5, 0x0f, 0x46, 0xe5, 0x84, 0xd5, 0xa4, 0xde, 0xbb, 0x75, 0x18, 0x0b, 0xba, 0xe1, 0xbd, 0x7e,
};
const unsigned char KEY3[32] = {
	0xe6, 0x9f, 0xf5, 0x3e, 0x81, 0xb4, 0x5a, 0x15, 0x68, 0x06, 0x13, 0xc5, 0xba, 0x2f, 0xf6, 0x53,
	0xcd, 0xb0, 0x1a, 0x02, 0xfb, 0xfa, 0x56, 0xee, 0xc4, 0x7a, 0x4e, 0x06, 0xfd, 0x36, 0x06, 0x35,
};
const char FILE_SUFFIX[] = "-test";
#else
// K1: the release manifest key every launcher already pins (gxlaunch.c GX_MANIFEST_PUBKEY, gxcore verify.rs).
// base64url KMjUdzzvoydFxhiZ3lR3kGUjZmjfgO0zG6STHoAmIMQ
const unsigned char KEY1[32] = {
	0x28, 0xc8, 0xd4, 0x77, 0x3c, 0xef, 0xa3, 0x27, 0x45, 0xc6, 0x18, 0x99, 0xde, 0x54, 0x77, 0x90,
	0x65, 0x23, 0x66, 0x68, 0xdf, 0x80, 0xed, 0x33, 0x1b, 0xa4, 0x93, 0x1e, 0x80, 0x26, 0x20, 0xc4,
};
// K2 and K3: the successors (§3.4). UNPROVISIONED - the operator generates them offline
// (`gx-manifest.py keygen --successor k2`) and pastes `pubkey --key k2 --c-array` here, in gxlaunch.c and in
// verify.rs, BEFORE the release that ships this file: a build that pins no successor can never rotate.
// All zero = an empty slot, skipped explicitly by gxRelayListVerify.
const unsigned char KEY2[32] = { 0 };
const unsigned char KEY3[32] = { 0 };
const char FILE_SUFFIX[] = "";
#endif

const int MIN_RANK = 1;		// raise in a release once every supported build has seen the next rank

struct Engine
{
	std::mutex mu;
	bool loaded = false;
	GXRelayListStore store;
	bool latched = false;
	bool haveActive = false;
	GXRelayListDoc active;
	std::atomic<bool> fetchStarted{false};
};

// Never destroyed: the fetch thread may still be running when the process exits, and a static destructor
// would pull the mutex out from under it.
Engine &eng()
{
	static Engine *e = new Engine;
	return *e;
}

unsigned long long wallNow()
{
	const time_t t = time(NULL);
	return t > 0 ? (unsigned long long)t : 0ULL;
}

void addLauncherCache(GXRelayListConfig &c, const std::string &path)
{
	if (path.empty() || c.nlauncher >= GX_RELAYLIST_LAUNCHER_CACHES || path.size() >= GX_RELAYLIST_PATH_MAX)
		return;
	snprintf(c.launcherCache[c.nlauncher++], GX_RELAYLIST_PATH_MAX, "%s", path.c_str());
}

void configure(GXRelayListConfig &c)
{
	memset(&c, 0, sizeof(c));
	c.keys.keys[0] = KEY1;
	c.keys.keys[1] = KEY2;
	c.keys.keys[2] = KEY3;
	c.minRank = MIN_RANK;
	c.buildFloor = GitCommitTimeStamp > 0 ? (unsigned long long)GitCommitTimeStamp : 0ULL;

	if (TheGlobalData != NULL)
	{
		const std::string dir = TheGlobalData->getPath_UserData().str();
		if (!dir.empty() && dir.size() + 32 < GX_RELAYLIST_PATH_MAX)
		{
			snprintf(c.cachePath, sizeof(c.cachePath), "%sGXRelayList%s.txt", dir.c_str(), FILE_SUFFIX);
			snprintf(c.statePath, sizeof(c.statePath), "%sGXRelayList%s.state", dir.c_str(), FILE_SUFFIX);
		}
	}

#ifdef GX_RELAYLIST_TEST_KEYS
	// A test build never reads a player's launcher cache: it would only be refused (wrong keys) and noise.
	if (const char *v = getenv("GX_RELAYLIST_LAUNCHER_CACHE"))
		addLauncherCache(c, v);
#else
	// The launchers' caches. Windows: GeneralsXUpdate.exe keeps gxmanifest.txt beside the game, which is the
	// working directory the game runs in (dataID is cwd-relative for the same reason). macOS/Linux: the Rust
	// launcher's state folder (gxio state.rs state_dir), which follows GX_USERDATA_DIR as <dir>/launcher.
#if defined(_WIN32)
	addLauncherCache(c, "gxmanifest.txt");
#elif defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	std::string ovr;
	if (gxUserDataDirOverride(ovr))
		addLauncherCache(c, ovr + "launcher/gxmanifest.txt");
	else if (const char *h = getenv("HOME"))
		addLauncherCache(c, std::string(h) + "/Library/Application Support/GeneralsX/gxmanifest.txt");
#elif !defined(__APPLE__)
	std::string ovr;
	if (gxUserDataDirOverride(ovr))
		addLauncherCache(c, ovr + "launcher/gxmanifest.txt");
	else
	{
		const char *x = getenv("XDG_DATA_HOME");
		if (x != NULL && x[0] == '/')
			addLauncherCache(c, std::string(x) + "/GeneralsX/gxmanifest.txt");
		else if (const char *h = getenv("HOME"))
			addLauncherCache(c, std::string(h) + "/.local/share/GeneralsX/gxmanifest.txt");
	}
#endif
	// iOS has no launcher: its sources are its own cache and the GXUPD fetch.
#endif
}

// Under eng().mu.
void ensureLatched()
{
	Engine &e = eng();
	if (!e.loaded)
	{
		GXRelayListConfig cfg;
		configure(cfg);
		gxRelayListStoreLoad(&e.store, &cfg, wallNow());
		e.loaded = true;
	}
	if (!e.latched)
	{
		e.latched = true;
		e.haveActive = e.store.have != 0;
		if (e.haveActive)
			e.active = e.store.doc;
	}
}

void copyHost(char *dst, size_t cap, const char *src)
{
	if (dst != NULL && cap > 0)
		snprintf(dst, cap, "%s", src);
}

struct FetchJob
{
	GXRelayEndpoint targets[2 * GX_RELAYLIST_MAX_ITEMS];
	int n = 0;
	unsigned long long ordinal = 0;
};

void onFetchReply(void *, const char *wire, size_t len, const char *fromHost)
{
	Engine &e = eng();
	std::lock_guard<std::mutex> lock(e.mu);
	char source[96];
	snprintf(source, sizeof(source), "GXUPD reply from %s", fromHost);
	int adopted = 0;
	gxRelayListStoreOffer(&e.store, wire, len, wallNow(), source, &adopted);
	if (adopted)
	{
		fprintf(stderr, "[GXRELAYLIST] the newer list is cached; this session keeps its home, the next launch uses it\n");
		fflush(stderr);
	}
}

void addTarget(FetchJob &j, const char *host, unsigned short port)
{
	if (host == NULL || host[0] == '\0' || j.n >= 2 * GX_RELAYLIST_MAX_ITEMS)
		return;
	for (int i = 0; i < j.n; ++i)
		if (j.targets[i].port == port && strcmp(j.targets[i].host, host) == 0)
			return;
	GXRelayEndpoint &t = j.targets[j.n++];
	memset(&t, 0, sizeof(t));
	copyHost(t.host, sizeof(t.host), host);
	t.port = port;
}

} // namespace

bool GXRelayList::home(char *host, size_t hostCap, unsigned short *port)
{
	Engine &e = eng();
	std::lock_guard<std::mutex> lock(e.mu);
	ensureLatched();
	if (!e.haveActive)
		return false;
	copyHost(host, hostCap, e.active.home.host);
	if (port != NULL)
		*port = e.active.home.port;
	return true;
}

bool GXRelayList::standby(int i, char *host, size_t hostCap, unsigned short *port)
{
	Engine &e = eng();
	std::lock_guard<std::mutex> lock(e.mu);
	ensureLatched();
	if (!e.haveActive || i < 0 || i >= e.active.nalt)
		return false;
	copyHost(host, hostCap, e.active.alt[i].host);
	if (port != NULL)
		*port = e.active.alt[i].port;
	return true;
}

bool GXRelayList::identity(int *rank, unsigned long long *serial, unsigned long long *ordinal)
{
	Engine &e = eng();
	std::lock_guard<std::mutex> lock(e.mu);
	ensureLatched();
	if (!e.haveActive)
		return false;
	if (rank != NULL) *rank = e.active.rank;
	if (serial != NULL) *serial = e.active.serial;
	if (ordinal != NULL) *ordinal = e.active.ordinal;
	return true;
}

const char *GXRelayList::bootstrap(const char *compiled)
{
#ifdef GX_RELAYLIST_TEST_KEYS
	if (const char *v = getenv("GX_RELAYLIST_BOOTSTRAP"))
		if (v[0] != '\0')
			return v;
#endif
	return compiled;
}

void GXRelayList::lobbyEntry(bool allowFetch, const char *bootstrapHost)
{
	if (!allowFetch)
		return;
	Engine &e = eng();
	if (e.fetchStarted.exchange(true))
		return;				// one refresh per process: the result is for the next launch anyway

	FetchJob *job = new FetchJob;
	{
		std::lock_guard<std::mutex> lock(e.mu);
		ensureLatched();
		// The compiled-in bootstrap ALWAYS (§3.5 source 3), then the held home and its standbys.
		char bh[GX_RELAYLIST_HOST_MAX + 1];
		unsigned short bp = GX_RELAYLIST_DEFAULT_PORT;
		if (bootstrapHost != NULL && gxRelayParseHostPort(bootstrapHost, bh, sizeof(bh), &bp, GX_RELAYLIST_DEFAULT_PORT))
			addTarget(*job, bh, bp);
		if (e.haveActive)
		{
			addTarget(*job, e.active.home.host, e.active.home.port);
			for (int i = 0; i < e.active.nalt; ++i)
				addTarget(*job, e.active.alt[i].host, e.active.alt[i].port);
		}
		job->ordinal = GitCommitTimeStamp > 0 ? (unsigned long long)GitCommitTimeStamp : 0ULL;
	}
	if (job->n == 0)
	{
		delete job;
		return;
	}
	// Detached on purpose: it owns `job`, touches only the never-destroyed Engine, and is bounded by the
	// window below. Joining would put DNS and three UDP round trips in front of whoever joined.
	std::thread([job]() {
		gxRelayListFetch(job->targets, job->n, job->ordinal, 1500, onFetchReply, NULL);
		delete job;
	}).detach();
}

#endif // GX_RELAYLIST_STANDALONE
