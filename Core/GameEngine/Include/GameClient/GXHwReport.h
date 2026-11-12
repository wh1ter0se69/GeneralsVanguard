// FILE: GXHwReport.h /////////////////////////////////////////////////////////////////////////////
//
// The hardware report: what kind of computer this client runs on, sent once per lobby connection to a relay that asks for it
// (relay.js GX_HW_TAG), for the relay's admin console only.
//
//     GXHWINF <room> gpu=... drv=... vram=... vk=... cpu=... cc=... ct=... ram=... os=... osv=... res=... hz=... win=... be=... gx=...
//
// Facts only, never identifiers: no serials, PCI ids, MACs, host or user names (the device layer scrubs model strings that carry a
// user or host name before they reach gxHwPut). Values keep to the relay's alphabet, a space sent as '_'.
//
// Header-only, so scripts/test/hw-report-test.cpp tests it without the engine. Here in GameClient/, outside the SimID digest: nothing
// in it is read by the simulation.

#pragma once

#include <stddef.h>
#include <string.h>

#define GX_HW_TAG	"GXHWINF"
#define GX_HW_WORD	"hw1"	///< in GXCAPS: this relay reads GXHWINF
#define GX_HW_ASK	"hwq"	///< in GXCAPS: and it holds no report of ours

enum
{
	GX_HW_LINE_MAX = 400,			///< relay.js GX_HW_MAX_LINE: a longer datagram is refused whole
	GX_HW_ASK_GAP_MS = 5000,		///< a repeated ask inside this is the answer still in flight
	GX_HW_CHANGE_GAP_MS = 60000,	///< a changed report goes at most once a minute
	GX_HW_VALUE_MAX = 64
};

/// The device layer's facts, by the replay diagnostics' key names (GXReplayDiagDevice.cpp). Without a provider nothing is sent.
typedef void (*GXHwPutFn)( const char *key, const char *value );
typedef void (*GXHwFactsFn)( GXHwPutFn put );
void gxHwSetFactsProvider( GXHwFactsFn fn );

/// The fields, in the order they go on the line (most useful first, so a cut line keeps them). `max` and `min` are relay.js
/// GX_HW_FIELDS' caps; `from` is the key the device layer puts.
struct GXHwField
{
	const char *from;
	const char *to;
	unsigned max;
	bool number;
	unsigned min;
};
static const GXHwField kGxHwFields[] =
{
	{ "gpu.name",			"gpu",	64,	false,	0 },
	{ "gpu.driver_info",	"drv",	48,	false,	0 },
	{ "gpu.vram_mb",		"vram",	7,	true,	1 },
	{ "gpu.vk",				"vk",	16,	false,	0 },
	{ "cpu.model",			"cpu",	64,	false,	0 },
	{ "cpu.cores",			"cc",	4,	true,	1 },
	{ "cpu.logical",		"ct",	4,	true,	1 },
	{ "ram.gb",				"ram",	5,	true,	1 },
	{ "os.name",			"os",	24,	false,	0 },
	{ "os.version",			"osv",	32,	false,	0 },
	{ "gfx.res",			"res",	11,	false,	0 },
	{ "gfx.refresh_hz",		"hz",	4,	true,	1 },
	{ "gfx.windowed",		"win",	1,	true,	0 },
	{ "render.backend",		"be",	24,	false,	0 },
	{ "gx.version",			"gx",	24,	false,	0 },
};
enum { GX_HW_FIELDS = sizeof( kGxHwFields ) / sizeof( kGxHwFields[ 0 ] ) };

struct GXHwFacts
{
	char v[ GX_HW_FIELDS ][ GX_HW_VALUE_MAX + 1 ];
};

inline void gxHwClear( GXHwFacts &f )
{
	memset( &f, 0, sizeof( f ) );
}

inline bool gxHwAllowed( char c )
{
	return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || strchr( "._()+,:/@#-", c ) != NULL;
}

/// Keep one fact. Text: the relay's alphabet only, everything else dropped, spaces as one '_', cut to the field's cap. A number:
/// digits only and at least the field's minimum, or no fact. A key outside kGxHwFields is ignored. An empty result clears the field.
inline void gxHwPut( GXHwFacts &f, const char *key, const char *value )
{
	if( key == NULL || value == NULL )
		return;
	for( int i = 0; i < GX_HW_FIELDS; ++i )
	{
		const GXHwField &fd = kGxHwFields[ i ];
		if( strcmp( key, fd.from ) != 0 )
			continue;
		char *out = f.v[ i ];
		size_t n = 0;
		if( fd.number )
		{
			unsigned long long num = 0;
			const char *p = value;
			for( ; *p >= '0' && *p <= '9' && n < fd.max; ++p, ++n )
				num = num * 10 + (unsigned)( *p - '0' );
			if( n == 0 || *p != '\0' || num < fd.min )
				n = 0;
			else
				memcpy( out, value, n );
		}
		else
		{
			bool space = false;
			for( const char *p = value; *p != '\0' && n < fd.max; ++p )
			{
				if( *p == ' ' || *p == '\t' || *p == '_' )
					space = n > 0;
				else if( gxHwAllowed( *p ) )
				{
					if( space )
					{
						if( n + 1 >= fd.max )
							break;			// no room for the next word: stop rather than run two words together
						out[ n++ ] = '_';
					}
					space = false;
					out[ n++ ] = *p;
				}
			}
		}
		out[ n ] = '\0';
		return;
	}
}

/// "GXHWINF <room> k=v ..." into `out`, the facts in kGxHwFields order. Stops at the first fact that would not fit `cap` or
/// GX_HW_LINE_MAX, so a cut line keeps whole fields. 0 when there is no room name or no fact.
inline int gxHwBuildLine( char *out, size_t cap, const char *room, const GXHwFacts &f )
{
	if( out == NULL || cap == 0 || room == NULL )
		return 0;
	const size_t roomLen = strlen( room );
	if( roomLen == 0 || roomLen > 31 )
		return 0;
	for( const char *p = room; *p != '\0'; ++p )
		if( !( ( *p >= 'A' && *p <= 'Z' ) || ( *p >= 'a' && *p <= 'z' ) || ( *p >= '0' && *p <= '9' ) || *p == '.' || *p == '_' || *p == '-' ) )
			return 0;
	const size_t limit = cap - 1 < (size_t)GX_HW_LINE_MAX ? cap - 1 : (size_t)GX_HW_LINE_MAX;
	const size_t head = sizeof( GX_HW_TAG ) - 1 + 1 + roomLen;
	if( head > limit )
		return 0;
	memcpy( out, GX_HW_TAG " ", sizeof( GX_HW_TAG ) );
	memcpy( out + sizeof( GX_HW_TAG ), room, roomLen );
	size_t len = head;
	int fields = 0;
	for( int i = 0; i < GX_HW_FIELDS; ++i )
	{
		const size_t vlen = strlen( f.v[ i ] );
		if( vlen == 0 )
			continue;
		const size_t klen = strlen( kGxHwFields[ i ].to );
		if( len + 1 + klen + 1 + vlen > limit )
			break;
		out[ len++ ] = ' ';
		memcpy( out + len, kGxHwFields[ i ].to, klen );
		len += klen;
		out[ len++ ] = '=';
		memcpy( out + len, f.v[ i ], vlen );
		len += vlen;
		++fields;
	}
	out[ len ] = '\0';
	return fields > 0 ? (int)len : 0;
}

/// When lobby presence sends. Only to a relay that said GX_HW_WORD, only while signed in (the relay keeps a report under the
/// account it arrived with). On the relay's ask (GX_HW_ASK), again after GX_HW_ASK_GAP_MS if it still asks; unasked, only a
/// changed report, at most once per GX_HW_CHANGE_GAP_MS. Reset with the socket: a new socket is a new membership.
struct GXHwGate
{
	bool sent;
	unsigned lastMs;
	char last[ GX_HW_LINE_MAX + 1 ];
};

inline void gxHwGateReset( GXHwGate &g )
{
	memset( &g, 0, sizeof( g ) );
}

inline bool gxHwWantSend( const GXHwGate &g, bool relayReads, bool relayAsks, bool signedIn, const char *line, unsigned nowMs )
{
	if( !relayReads || !signedIn || line == NULL || line[ 0 ] == '\0' )
		return false;
	const unsigned since = nowMs - g.lastMs;	// unsigned: right across the millisecond clock's wrap
	if( relayAsks )
		return !g.sent || since >= (unsigned)GX_HW_ASK_GAP_MS;
	return g.sent && strcmp( line, g.last ) != 0 && since >= (unsigned)GX_HW_CHANGE_GAP_MS;
}

inline void gxHwSent( GXHwGate &g, const char *line, unsigned nowMs )
{
	g.sent = true;
	g.lastMs = nowMs;
	strncpy( g.last, line, sizeof( g.last ) - 1 );
	g.last[ sizeof( g.last ) - 1 ] = '\0';
}
