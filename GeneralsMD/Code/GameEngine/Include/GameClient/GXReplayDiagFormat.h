// FILE: GXReplayDiagFormat.h /////////////////////////////////////////////////////////////////////
//
// GeneralsX: The replay diagnostics block: the bytes, and the arithmetic of a timeline row.
//
// and we asked for his replay: a replay said nothing about
// HIS machine. This makes "send me the replay" the whole ask: build, renderer, settings, coarse machine facts and a 10 s
// performance timeline travel inside the .rep. scripts/tooling/replay-info.py reads them.
//
// THE RECORDS ARE COMMENTS IN THE COMMAND STREAM. A record has the exact shape RecorderClass::writeToFile gives a command
// (frame, type, player, one INTEGER argument group), with type MSG_BEGIN_NETWORK_MESSAGES. That marker is never recorded as a
// command, and RecorderClass::appendNextCommand parses it like any other and deletes it instead of appending it to the command
// list, on every build back to before this block existed. So nothing on the read path changed, and the simulation cannot
// see a record. Why not the header or a trailer: a new magic hides the replay from an old build's list, a trailer reads as
// a bad frame number and a crash never writes it. In the stream it reaches disk with the same flush as the commands, so a
// crashed match keeps it up to the last window.
//
// A RECORD CARRIES THE FRAME OF THE LAST COMMAND WRITTEN, 0 before the first. Playback ends at end of file and waits for each
// frame it reads, so a record stamped later would make a match that quit or crashed simulate on past its last command. Stamped
// this way it is consumed in that command's own loop and never adds a frame to the sequence playback waits on. A window's
// time is in its row (frame_end), not in this field. RecorderClass::writeDiagRecord stamps it.
//
// This header includes nothing of the engine's, so scripts/test/replay-diag-format-test.cpp compiles it alone.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum
{
	GX_DIAG_TYPE          = 1000,		// GameMessage::MSG_BEGIN_NETWORK_MESSAGES, static_assert in Recorder.cpp
	GX_DIAG_VERSION       = 1,
	GX_DIAG_KIND_STATIC   = 1,			// ASCII key=value lines, one snapshot over [total] records
	GX_DIAG_KIND_WINDOW   = 2,			// one timeline row, int32 each, -1 = not measured; fields are only ever appended
	GX_DIAG_MAX_WORDS     = 255,		// a record's argument count is a byte
	GX_DIAG_PRELUDE_WORDS = 4,
	GX_DIAG_MAX_DATA      = (GX_DIAG_MAX_WORDS - GX_DIAG_PRELUDE_WORDS) * 4,
	GX_DIAG_STATIC_MAX    = 8192,
	GX_DIAG_VALUE_MAX     = 96,
};

inline uint32_t gxDiagCrc32( const void *data, size_t len )
{
	const unsigned char *p = (const unsigned char *)data;
	uint32_t crc = 0xFFFFFFFFu;
	for( size_t i = 0; i < len; ++i )
	{
		crc ^= p[i];
		for( int b = 0; b < 8; ++b )
			crc = ( crc >> 1 ) ^ ( 0xEDB88320u & ( 0u - ( crc & 1u ) ) );
	}
	return ~crc;
}

/// One record's words: prelude + data, zero padded. Returns the word count, 0 when it does not fit.
inline int gxDiagBuildRecord( int32_t *words, int kind, int seq, int total, const void *data, size_t len )
{
	if( len > GX_DIAG_MAX_DATA )
		return 0;
	const uint32_t head = 0x47u | ( 0x58u << 8 ) | ( (uint32_t)GX_DIAG_VERSION << 16 ) | ( (uint32_t)kind << 24 );
	words[0] = (int32_t)head;
	words[1] = (int32_t)( ( (uint32_t)seq & 0xFFFFu ) | ( ( (uint32_t)total & 0xFFFFu ) << 16 ) );
	words[2] = (int32_t)len;
	words[3] = (int32_t)gxDiagCrc32( data, len );
	const int dataWords = (int)( ( len + 3 ) / 4 );
	if( dataWords > 0 )
	{
		words[ GX_DIAG_PRELUDE_WORDS + dataWords - 1 ] = 0;
		memcpy( &words[ GX_DIAG_PRELUDE_WORDS ], data, len );
	}
	return GX_DIAG_PRELUDE_WORDS + dataWords;
}

/// Copy `in` into `out` (cap includes the NUL) so that it cannot carry a path, an address or a line break: printable ASCII
/// only, and the characters every path and address needs become '_'. This is the backstop behind the allow-list, so a key
/// added by mistake still cannot print C:\Users\<name> or 1.2.3.4:80 whole.
inline void gxDiagSanitize( char *out, size_t cap, const char *in )
{
	size_t n = 0;
	if( cap == 0 )
		return;
	for( ; in != NULL && *in != 0 && n + 1 < cap; ++in )
	{
		const unsigned char c = (unsigned char)*in;
		if( c < 0x20 || c > 0x7E )
			continue;
		out[ n++ ] = ( c == '/' || c == '\\' || c == ':' || c == '@' || c == '=' ) ? '_' : (char)c;
	}
	out[ n ] = 0;
}

/// True when the text holds four dot-separated numbers of at most three digits each: the shape of an IPv4 address.
inline bool gxDiagHasDottedQuad( const char *s )
{
	for( ; *s != 0; ++s )
	{
		if( *s < '0' || *s > '9' )
			continue;
		const char *p = s;
		int groups = 0;
		for( ;; )
		{
			int digits = 0;
			while( *p >= '0' && *p <= '9' )
			{
				++p;
				++digits;
			}
			if( digits == 0 || digits > 3 )
				break;
			if( ++groups == 4 )
				return !( *p >= '0' && *p <= '9' );
			if( *p != '.' )
				break;
			++p;
		}
	}
	return false;
}

/// True when `needle` (three characters or more) is inside `text`, in any case. A model string is free text its owner can change, and no
/// filter can know every name; what it can know is this machine's own user and host, and refuse to write a value that contains them.
inline bool gxDiagContainsName( const char *text, const char *needle )
{
	if( text == NULL || needle == NULL || strlen( needle ) < 3 )
		return false;
	const auto lower = []( char c ) { return ( c >= 'A' && c <= 'Z' ) ? (char)( c + 'a' - 'A' ) : c; };
	for( ; *text != 0; ++text )
	{
		const char *t = text, *n = needle;
		while( *t != 0 && *n != 0 && lower( *t ) == lower( *n ) )
		{
			++t;
			++n;
		}
		if( *n == 0 )
			return true;
	}
	return false;
}

/// An os-release ID, which is a file the owner can edit: a name from this list, or "other". "ID=someones-workstation" is a perfectly
/// well-formed word, so the shape of a value proves nothing; only knowing the name does.
inline const char *gxDiagDistroName( const char *in )
{
	static const char *const s_known[] =
	{
		"ubuntu", "debian", "fedora", "arch", "manjaro", "endeavouros", "cachyos", "steamos", "bazzite", "nobara", "pop", "linuxmint", "zorin",
		"elementary", "kali", "opensuse", "opensuse-leap", "opensuse-tumbleweed", "rhel", "centos", "rocky", "almalinux", "alpine", "gentoo", "nixos", "void",
	};
	for( size_t i = 0; in != NULL && i < sizeof( s_known ) / sizeof( s_known[0] ); ++i )
		if( strcmp( in, s_known[i] ) == 0 )
			return s_known[i];
	return "other";
}

/// The leading x.y.z of a version string: a kernel release may end in anything its builder typed ("6.8.0-someones-box").
inline void gxDiagLeadingVersion( char *out, size_t cap, const char *in )
{
	size_t n = 0;
	if( cap == 0 )
		return;
	for( ; in != NULL && n + 1 < cap && ( ( *in >= '0' && *in <= '9' ) || *in == '.' ); ++in )
		out[ n++ ] = *in;
	out[ n ] = 0;
}

/// Append "key=value\n" to the snapshot text. A line that does not fit is dropped whole and the text ends with truncated=1,
/// so a reader never sees half a value.
inline void gxDiagAppendLine( char *text, size_t cap, size_t *len, const char *key, const char *value )
{
	static const char s_truncated[] = "truncated=1\n";
	char clean[ GX_DIAG_VALUE_MAX + 1 ];
	gxDiagSanitize( clean, sizeof( clean ), value );
	if( gxDiagHasDottedQuad( clean ) )	// no value the block carries is an address: whatever this is, it does not go in a shared file
		memcpy( clean, "other", 6 );
	const size_t keyLen = strlen( key ), valLen = strlen( clean );
	const size_t need = keyLen + 1 + valLen + 1;
	if( *len + need + sizeof( s_truncated ) - 1 > cap )
	{
		const size_t markLen = sizeof( s_truncated ) - 1;
		const bool marked = *len >= markLen && memcmp( text + *len - markLen, s_truncated, markLen ) == 0;	// no NUL in text
		if( !marked && *len + markLen <= cap )
		{
			memcpy( text + *len, s_truncated, markLen );
			*len += markLen;
		}
		return;
	}
	memcpy( text + *len, key, keyLen );
	*len += keyLen;
	text[ (*len)++ ] = '=';
	memcpy( text + *len, clean, valLen );
	*len += valLen;
	text[ (*len)++ ] = '\n';
}

/// An Options.ini value is whatever the player typed, so it is VALIDATED, not copied. It passes only if it is one number or a
/// pair of them ("50", "0.5", "1920 1080": one dot per number, so no IP address) or a word from the game's own vocabulary;
/// everything else reads "other". The sanitizer above is no help here: it would leave "wh1ter0se" standing out of C:\Users\wh1ter0se.
inline const char *gxDiagOptionValue( const char *in )
{
	static const char *const s_words[] =
	{
		"yes", "no", "true", "false", "on", "off", "low", "medium", "high", "veryhigh", "custom", "classic", "native", "default",
		"bilinear", "trilinear", "point", "none", "anisotropic", "fsr1", "fsr3", "dlss", "metalfx", "metalfx-temporal", "xess",
		"quality", "balanced", "performance", "ultra",
	};
	if( in == NULL || *in == 0 )
		return "other";
	for( size_t w = 0; w < sizeof( s_words ) / sizeof( s_words[0] ); ++w )
	{
		const char *a = in, *b = s_words[w];
		for( ; *a != 0 && *b != 0; ++a, ++b )
		{
			const char lower = ( *a >= 'A' && *a <= 'Z' ) ? (char)( *a + 'a' - 'A' ) : *a;
			if( lower != *b )
				break;
		}
		if( *a == 0 && *b == 0 )
			return in;
	}
	int numbers = 0, digits = 0, dots = 0;
	const char *p = in;
	size_t length = 0;
	for( ; *p != 0; ++p, ++length )
	{
		if( *p >= '0' && *p <= '9' )
		{
			if( digits == 0 )
				++numbers;
			++digits;
		}
		else if( *p == '.' && digits > 0 && dots == 0 )
			dots = 1;
		else if( *p == '-' && digits == 0 && dots == 0 && ( p == in || p[-1] == ' ' ) )
			continue;
		else if( *p == ' ' && digits > 0 && p[-1] != '.' )
		{
			digits = 0;
			dots = 0;
		}
		else
			return "other";
	}
	return ( numbers >= 1 && numbers <= 2 && digits > 0 && p[-1] != '.' && length <= 24 ) ? in : "other";
}

// ------------------------------------------------------------------------------------------------
// The timeline row. Pure arithmetic on one POD: the sampler in GXReplayDiag.cpp feeds it one iteration at a time.
// ------------------------------------------------------------------------------------------------
enum
{
	GX_DIAG_ROW_FRAME_END, GX_DIAG_ROW_WALL_MS, GX_DIAG_ROW_ITERS, GX_DIAG_ROW_DRAWN, GX_DIAG_ROW_FPS_MIN_X10,
	GX_DIAG_ROW_TICKS, GX_DIAG_ROW_TPS_MIN_X10, GX_DIAG_ROW_ITER_MAX_MS, GX_DIAG_ROW_SIM_MS, GX_DIAG_ROW_TICK_MAX_MS,
	GX_DIAG_ROW_STALL_MS, GX_DIAG_ROW_HOLD_MS, GX_DIAG_ROW_CUSHION_MIN, GX_DIAG_ROW_CAP_FPS, GX_DIAG_ROW_NET_RATE,
	GX_DIAG_ROW_FLAGS,
	GX_DIAG_ROW_DRAW_MS, GX_DIAG_ROW_DRAW_MAX_MS,	// time in Display::draw, -1 on a headless peer. New fields go on the end.
	GX_DIAG_ROW_FIELDS,

	GX_DIAG_FLAG_PAUSED      = 1,
	GX_DIAG_FLAG_FRAME_RESET = 2,
	GX_DIAG_FLAG_MATCH_END   = 4,
	GX_DIAG_FLAG_HEADLESS    = 8,	///< no renderer exists: drawn and fps_min_x10 are -1, the rest is real
};

static const long long GX_DIAG_WINDOW_NS = 10000000000LL;
static const long long GX_DIAG_SECOND_NS = 1000000000LL;

/// What one main-loop iteration hands the window. Cumulative counters stay cumulative: the window takes deltas.
struct GXDiagIter
{
	long long nowNs;
	unsigned int logicFrame;
	long long holdTotalNs;		///< the limiter's own total (FrameRateLimit::getTotalHoldNs)
	unsigned int drawnTotal;	///< frames that completed End_Render: a catch-up pass or a fast-forward skip iterates and draws nothing
	bool stalling;				///< TheNetwork->isStalling()
	int cushion;				///< execution frame - logic frame, -1 outside a network game
	bool paused;
	long long drawTotalNs;		///< time in Display::draw so far: a stalled loop keeps drawing, and that is not time spent waiting
};

struct GXDiagWindow
{
	bool primed;
	long long startNs, lastNs, secondStartNs;
	unsigned int startFrame, lastFrame, secondFrame;
	long long holdBaseNs, lastHoldNs;
	unsigned int drawnBase, secondDrawn;
	long long stallNs, iterMaxNs;
	int iters;
	int fpsMinX10, tpsMinX10, cushionMin, flags;
	bool lastPaused;
	long long lastDrawNs;
};

inline void gxDiagWindowStart( GXDiagWindow &w, const GXDiagIter &it )
{
	memset( &w, 0, sizeof( w ) );
	w.primed = true;
	w.startNs = w.lastNs = w.secondStartNs = it.nowNs;
	w.startFrame = w.lastFrame = w.secondFrame = it.logicFrame;
	w.holdBaseNs = w.lastHoldNs = it.holdTotalNs;
	w.drawnBase = w.secondDrawn = it.drawnTotal;
	w.fpsMinX10 = w.tpsMinX10 = w.cushionMin = -1;
	w.lastPaused = it.paused;
	w.flags = it.paused ? GX_DIAG_FLAG_PAUSED : 0;
	w.lastDrawNs = it.drawTotalNs;
}

/// One iteration. Returns true when the window is due: read it with gxDiagWindowRow, then start the next one. A pause or a resume
/// closes it early, so a row is all play or all pause: a deliberate pause must not take the dip beside it out of the whole-match rates.
inline bool gxDiagWindowIter( GXDiagWindow &w, const GXDiagIter &it )
{
	const long long since = it.nowNs - w.lastNs;
	w.lastNs = it.nowNs;
	++w.iters;
	if( since > w.iterMaxNs )
		w.iterMaxNs = since;

	// Stall and hold stay disjoint, the rule gxPaceSample and gxPerfSample both follow: wall time since the last iteration
	// already contains the limiter's sleep, so the same milliseconds would otherwise count twice in exactly the case a
	// blocked AND capped window is read for.
	const long long holdDelta = it.holdTotalNs - w.lastHoldNs;
	w.lastHoldNs = it.holdTotalNs;
#ifdef GX_DIAG_RED_DOUBLE_COUNT_STALL
	(void)holdDelta;
	if( it.logicFrame == w.lastFrame && it.stalling )
		w.stallNs += since;
#else
	const long long drawDelta = it.drawTotalNs - w.lastDrawNs;
	w.lastDrawNs = it.drawTotalNs;
	// And the draw comes out too: a stalled loop keeps drawing, and a slow GPU must not read as a wait on a peer.
	if( it.logicFrame == w.lastFrame && it.stalling && since - holdDelta - drawDelta > 0 )
		w.stallNs += since - holdDelta - drawDelta;
#endif

	if( it.logicFrame < w.lastFrame )
		w.flags |= GX_DIAG_FLAG_FRAME_RESET;
	w.lastFrame = it.logicFrame;
	const bool pauseFlipped = it.paused != w.lastPaused;
	w.lastPaused = it.paused;
	if( it.cushion >= 0 && ( w.cushionMin < 0 || it.cushion < w.cushionMin ) )
		w.cushionMin = it.cushion;

	// A gap of a second or more holds a whole second in which nothing happened, whatever the seconds either side did. Dividing the frames
	// around it by the gap would report a freeze as a slow patch.
	if( since >= GX_DIAG_SECOND_NS )
		w.fpsMinX10 = w.tpsMinX10 = 0;

	// The worst whole second inside the window: a mean over 10 s is what hides "42 fps at times".
	const long long secondNs = it.nowNs - w.secondStartNs;
	if( secondNs >= GX_DIAG_SECOND_NS )
	{
		const int fps = (int)( (long long)( it.drawnTotal - w.secondDrawn ) * 10 * GX_DIAG_SECOND_NS / secondNs );
		if( w.fpsMinX10 < 0 || fps < w.fpsMinX10 )
			w.fpsMinX10 = fps;
		if( it.logicFrame >= w.secondFrame )
		{
			const int tps = (int)( (long long)( it.logicFrame - w.secondFrame ) * 10 * GX_DIAG_SECOND_NS / secondNs );
			if( w.tpsMinX10 < 0 || tps < w.tpsMinX10 )
				w.tpsMinX10 = tps;
		}
		w.secondStartNs = it.nowNs;
		w.secondFrame = it.logicFrame;
		w.secondDrawn = it.drawnTotal;
	}
	return pauseFlipped || it.nowNs - w.startNs >= GX_DIAG_WINDOW_NS;
}

/// The row for the window so far. simNs / tickMaxNs come from GameEngine, taken once per window.
inline void gxDiagWindowRow( const GXDiagWindow &w, const GXDiagIter &it, long long simNs, long long tickMaxNs,
	int capFps, int netRate, int extraFlags, int32_t *row )
{
	const int flags = w.flags | extraFlags;
	row[ GX_DIAG_ROW_FRAME_END ] = (int32_t)it.logicFrame;
	row[ GX_DIAG_ROW_WALL_MS ] = (int32_t)( ( it.nowNs - w.startNs ) / 1000000 );
	row[ GX_DIAG_ROW_ITERS ] = w.iters;
	row[ GX_DIAG_ROW_DRAWN ] = ( flags & GX_DIAG_FLAG_HEADLESS ) ? -1 : (int32_t)( it.drawnTotal - w.drawnBase );
	row[ GX_DIAG_ROW_FPS_MIN_X10 ] = ( flags & GX_DIAG_FLAG_HEADLESS ) ? -1 : w.fpsMinX10;
	row[ GX_DIAG_ROW_TICKS ] = ( flags & GX_DIAG_FLAG_FRAME_RESET ) || it.logicFrame < w.startFrame ? -1 : (int32_t)( it.logicFrame - w.startFrame );
	row[ GX_DIAG_ROW_TPS_MIN_X10 ] = w.tpsMinX10;
	row[ GX_DIAG_ROW_ITER_MAX_MS ] = (int32_t)( w.iterMaxNs / 1000000 );
	row[ GX_DIAG_ROW_SIM_MS ] = (int32_t)( simNs / 1000000 );
	row[ GX_DIAG_ROW_TICK_MAX_MS ] = (int32_t)( tickMaxNs / 1000000 );
	row[ GX_DIAG_ROW_STALL_MS ] = (int32_t)( w.stallNs / 1000000 );
	row[ GX_DIAG_ROW_HOLD_MS ] = (int32_t)( ( it.holdTotalNs - w.holdBaseNs ) / 1000000 );
	row[ GX_DIAG_ROW_CUSHION_MIN ] = w.cushionMin;
	row[ GX_DIAG_ROW_CAP_FPS ] = capFps;
	row[ GX_DIAG_ROW_NET_RATE ] = netRate;
	row[ GX_DIAG_ROW_FLAGS ] = flags;
}

// ------------------------------------------------------------------------------------------------
// TELEMETRY: per-second rows on the same comment-record path. Kinds 3 PERF, 4 VIEW, 5 INPUT keep
// prelude ver 1 (an old replay-info.py ignores a kind it does not know; a ver above its own reads as "future"). One record is one
// BATCH: a 14-byte header, `rows` fixed-size rows, then for PERF a 16-bucket frame-time histogram. Every field is little endian.
//
//   batch header: u8 rowBytes | u8 rows | u8 flags (bit0 histogram follows the rows, bit1 telemetry was dropped before this batch)
//                 | u8 droppedSeconds (slow-I/O skips since this kind's previous batch, saturating 255) | u32 frameStart
//                 | u32 msSinceStart | u16 droppedEntries (INPUT ring overflow, saturating 65535; 0 for other kinds)
//
// A reader takes rowBytes from the header, so fields are only ever APPENDED to a row: an old reader parses the prefix it knows.
//
//   PERF row, 58 bytes (offset field unit):
//     0 wallMs ms | 2 drawn | 4 iters | 6 ftP50 | 8 ftP95 | 10 ftP99 | 12 ftMax | 14 ftP99_3s (frame times in 0.1 ms) | 16 ticks
//     18 tickMaxX10 0.1 ms | 20 simMs | 22 stallMs | 24 holdMs | 26 drawMs | 28 cushionMin u8 frames | 29 netRate u8 Hz
//     30 netRetries | 32 netRecvDrop | 34 cpuProcMs | 36 cpuMainMs | 38 cpuRenderMs | 40 cpuAudioMs | 42 cpuPcoreMs
//     44 sysCpuX10 0.1 % | 46 gpuProcMs | 48 gpuFrameX10 0.1 ms | 50 sysGpuPct u8 % | 51 rss16M | 53 sysAvailMb16M (16 MiB units)
//     55 flags u8 | 56 iterMaxX10 0.1 ms: the longest main-loop ITERATION in the second (a frame interval spans skipped draws, so
//     ft_max cannot stand in for it; "waiting on a peer" needs the loop's own iteration time normal - measurement rules)
//
// Every field is u16 unless marked. NOTHING IS TRUNCATED: a value too large for its field is the CENSORED marker (u16 0xFFFE, u8
// 0xFE), a value not measured is UNKNOWN (u16 0xFFFF, u8 0xFF). A 90 s wallMs cast to u16 would read 24,464 ms; censored it reads
// ">= 65.5 s", and the WINDOW row's i32 fields keep the exact long values.
// ------------------------------------------------------------------------------------------------
enum
{
	GX_DIAG_KIND_PERF  = 3,
	GX_DIAG_KIND_VIEW  = 4,
	GX_DIAG_KIND_INPUT = 5,
};

// Frame-time buckets: 0.1 ms to 100 ms (1000), 1 ms to 1 s (900), 100 ms to 6.5 s (55), one censored bucket >= 6.5 s (6.5 s = 65000
// in 0.1 ms, the last value under GX_TEL_CENSORED a u16 can carry).
enum { GX_TEL_FT_BUCKETS = 1956 };
static const uint16_t GX_TEL_CENSORED    = 0xFFFE;	///< a value at or above what its field holds; a percentile in the >= 6.5 s bucket
static const uint16_t GX_TEL_U16_UNKNOWN = 0xFFFF;	///< also 0xFF for u8 fields
static const uint8_t  GX_TEL_U8_CENSORED = 0xFE;
static const uint8_t  GX_TEL_U8_UNKNOWN  = 0xFF;

inline uint16_t gxTelNarrowU16( long long v )
{
	return v < 0 ? GX_TEL_U16_UNKNOWN : v >= (long long)GX_TEL_CENSORED ? GX_TEL_CENSORED : (uint16_t)v;
}

inline uint8_t gxTelNarrowU8( long long v )
{
	return v < 0 ? GX_TEL_U8_UNKNOWN : v >= (long long)GX_TEL_U8_CENSORED ? GX_TEL_U8_CENSORED : (uint8_t)v;
}

/// Intervals between completed draws. Counts saturate at 0xFFFF (a second holds a few hundred), n is exact.
struct GxTelFrameHist
{
	uint16_t b[ GX_TEL_FT_BUCKETS ];
	uint32_t n;
	uint32_t maxX10;	///< 0.1 ms; GX_TEL_CENSORED once any frame reached 6.5 s, never a clipped number
};

inline void gxTelHistReset( GxTelFrameHist &h )
{
	memset( &h, 0, sizeof( h ) );
}

inline int gxTelHistBucket( long long ns )
{
	if( ns < 0 )
		ns = 0;
	if( ns < 100000000LL )
		return (int)( ns / 100000LL );
	if( ns < 1000000000LL )
		return 1000 + (int)( ( ns - 100000000LL ) / 1000000LL );
	if( ns < 6500000000LL )
		return 1900 + (int)( ( ns - 1000000000LL ) / 100000000LL );
	return GX_TEL_FT_BUCKETS - 1;
}

/// A bucket's LOWER edge in 0.1 ms; the censored bucket has none.
inline uint16_t gxTelHistEdgeX10( int bucket )
{
	if( bucket < 1000 )
		return (uint16_t)bucket;
	if( bucket < 1900 )
		return (uint16_t)( 1000 + ( bucket - 1000 ) * 10 );
	if( bucket < GX_TEL_FT_BUCKETS - 1 )
		return (uint16_t)( 10000 + ( bucket - 1900 ) * 1000 );
	return GX_TEL_CENSORED;
}

inline void gxTelHistAdd( GxTelFrameHist &h, long long ns )
{
	uint16_t &c = h.b[ gxTelHistBucket( ns ) ];
	if( c != 0xFFFF )
		++c;
	if( h.n != 0xFFFFFFFFu )
		++h.n;
	const uint32_t x10 = ns >= 6500000000LL ? GX_TEL_CENSORED : (uint32_t)( ( ns < 0 ? 0 : ns ) / 100000LL );
	if( x10 > h.maxX10 )
		h.maxX10 = x10;
}

/// The pctX10/1000 percentile: the lower edge (0.1 ms) of the bucket where the running count first reaches ceil(n * pct). UNKNOWN when
/// empty, GX_TEL_CENSORED in the last bucket. Saturated counts can fall short of the target: then the highest non-empty bucket answers.
inline uint16_t gxTelHistPct( const GxTelFrameHist &h, unsigned pctX10 )
{
	if( h.n == 0 )
		return GX_TEL_U16_UNKNOWN;
	unsigned long long target = ( (unsigned long long)h.n * pctX10 + 999 ) / 1000;
	if( target == 0 )
		target = 1;
	unsigned long long cum = 0;
	int last = -1;
	for( int i = 0; i < GX_TEL_FT_BUCKETS; ++i )
	{
		if( h.b[i] == 0 )
			continue;
		cum += h.b[i];
		last = i;
		if( cum >= target )
			return gxTelHistEdgeX10( i );
	}
	return last < 0 ? GX_TEL_U16_UNKNOWN : gxTelHistEdgeX10( last );
}

inline void gxTelHistSum( GxTelFrameHist &dst, const GxTelFrameHist &src )
{
	for( int i = 0; i < GX_TEL_FT_BUCKETS; ++i )
	{
		const unsigned s = (unsigned)dst.b[i] + src.b[i];
		dst.b[i] = (uint16_t)( s > 0xFFFFu ? 0xFFFFu : s );
	}
	const unsigned long long n = (unsigned long long)dst.n + src.n;
	dst.n = (uint32_t)( n > 0xFFFFFFFFull ? 0xFFFFFFFFull : n );
	if( src.maxX10 > dst.maxX10 )
		dst.maxX10 = src.maxX10;
}

/// The batch's 16-bucket histogram, so a reader gets run-level percentiles that per-second rows cannot be merged into. Edges in ms;
/// a frame belongs to the first bucket whose upper edge it is below: <4, 4-6, ..., 200-500, >=500.
static const long long GX_TEL_LOG_EDGES_MS[15] = { 4, 6, 8, 10, 12, 14, 17, 20, 25, 33, 50, 67, 100, 200, 500 };
enum { GX_TEL_LOG_BUCKETS = 16 };

inline int gxTelLogBucket( long long ns )
{
	for( int i = 0; i < GX_TEL_LOG_BUCKETS - 1; ++i )
		if( ns < GX_TEL_LOG_EDGES_MS[i] * 1000000LL )
			return i;
	return GX_TEL_LOG_BUCKETS - 1;
}

struct GxTelPerfRow
{
	uint16_t wallMs, drawn, iters, ftP50, ftP95, ftP99, ftMax, ftP99_3s, ticks;
	uint16_t tickMaxX10, simMs, stallMs, holdMs, drawMs;
	uint8_t cushionMin;		///< frames, 0..253; GX_TEL_U8_CENSORED above, GX_TEL_U8_UNKNOWN outside a network game
	uint8_t netRate;
	uint16_t netRetries, netRecvDrop, cpuProcMs, cpuMainMs, cpuRenderMs, cpuAudioMs, cpuPcoreMs, sysCpuX10, gpuProcMs, gpuFrameX10;
	uint8_t sysGpuPct;
	uint16_t rss16M, sysAvailMb16M;	///< memory in 16 MiB units, so a u16 reaches 1 TiB
	uint8_t flags;
	uint16_t iterMaxX10;	///< appended after flags: the longest iteration in the row, 0.1 ms
};

enum { GX_TEL_PERF_ROW_BYTES = 58 };

enum
{
	GX_TEL_F_PAUSED       = 1,
	GX_TEL_F_HEADLESS     = 2,
	GX_TEL_F_FRAME_RESET  = 4,
	GX_TEL_F_UNFOCUSED    = 8,
	GX_TEL_F_SAMPLER_LATE = 16,
	GX_TEL_F_DROPPED      = 32,
};

inline void gxTelPut16( uint8_t *p, unsigned v )
{
	p[0] = (uint8_t)( v & 0xFF );
	p[1] = (uint8_t)( ( v >> 8 ) & 0xFF );
}

inline void gxTelPut32( uint8_t *p, uint32_t v )
{
	gxTelPut16( p, v & 0xFFFFu );
	gxTelPut16( p + 2, v >> 16 );
}

inline size_t gxTelPackPerfRow( const GxTelPerfRow &r, uint8_t *out )
{
	const uint16_t head[] = { r.wallMs, r.drawn, r.iters, r.ftP50, r.ftP95, r.ftP99, r.ftMax, r.ftP99_3s, r.ticks,
		r.tickMaxX10, r.simMs, r.stallMs, r.holdMs, r.drawMs };
	size_t o = 0;
	for( size_t i = 0; i < sizeof( head ) / sizeof( head[0] ); ++i, o += 2 )
		gxTelPut16( out + o, head[i] );
	out[ o++ ] = r.cushionMin;
	out[ o++ ] = r.netRate;
	const uint16_t mid[] = { r.netRetries, r.netRecvDrop, r.cpuProcMs, r.cpuMainMs, r.cpuRenderMs, r.cpuAudioMs, r.cpuPcoreMs, r.sysCpuX10,
		r.gpuProcMs, r.gpuFrameX10 };
	for( size_t i = 0; i < sizeof( mid ) / sizeof( mid[0] ); ++i, o += 2 )
		gxTelPut16( out + o, mid[i] );
	out[ o++ ] = r.sysGpuPct;
	gxTelPut16( out + o, r.rss16M );
	o += 2;
	gxTelPut16( out + o, r.sysAvailMb16M );
	o += 2;
	out[ o++ ] = r.flags;
	gxTelPut16( out + o, r.iterMaxX10 );
	o += 2;
	return o;
}

enum
{
	GX_TEL_BATCH_HEADER    = 14,
	GX_TEL_BATCH_F_HIST    = 1,
	GX_TEL_BATCH_F_DROPPED = 2,
};

struct GxTelBatch
{
	uint8_t data[ GX_DIAG_MAX_DATA ];
	size_t len;
	uint8_t rowBytes, rows;
};

inline void gxTelBatchBegin( GxTelBatch &b, uint8_t rowBytes, uint32_t frameStart, uint32_t msSinceStart,
	unsigned droppedSeconds, unsigned droppedEntries )
{
	b.rowBytes = rowBytes;
	b.rows = 0;
	b.data[0] = rowBytes;
	b.data[1] = 0;
	b.data[2] = ( droppedSeconds != 0 || droppedEntries != 0 ) ? GX_TEL_BATCH_F_DROPPED : 0;
	b.data[3] = (uint8_t)( droppedSeconds > 255 ? 255 : droppedSeconds );
	gxTelPut32( b.data + 4, frameStart );
	gxTelPut32( b.data + 8, msSinceStart );
	gxTelPut16( b.data + 12, droppedEntries > 0xFFFFu ? 0xFFFFu : droppedEntries );
	b.len = GX_TEL_BATCH_HEADER;
}

/// False when the next row would not fit the record, or the histogram (which ends the batch) is already in.
inline bool gxTelBatchAppend( GxTelBatch &b, const uint8_t *row )
{
	if( b.rowBytes == 0 || b.rows == 255 || ( b.data[2] & GX_TEL_BATCH_F_HIST ) || b.len + b.rowBytes > (size_t)GX_DIAG_MAX_DATA )
		return false;
	memcpy( b.data + b.len, row, b.rowBytes );
	b.len += b.rowBytes;
	b.data[1] = ++b.rows;
	return true;
}

inline bool gxTelBatchAppendHist( GxTelBatch &b, const uint16_t counts[ GX_TEL_LOG_BUCKETS ] )
{
	if( ( b.data[2] & GX_TEL_BATCH_F_HIST ) || b.len + GX_TEL_LOG_BUCKETS * 2 > (size_t)GX_DIAG_MAX_DATA )
		return false;
	for( int i = 0; i < GX_TEL_LOG_BUCKETS; ++i )
		gxTelPut16( b.data + b.len + 2 * i, counts[i] );
	b.len += GX_TEL_LOG_BUCKETS * 2;
	b.data[2] |= GX_TEL_BATCH_F_HIST;
	return true;
}

/// One record's dropped counts, set when the batch is WRITTEN: the slow-I/O rule decides at write time, after the batch began.
inline void gxTelBatchSetDropped( GxTelBatch &b, unsigned droppedSeconds, unsigned droppedEntries )
{
	b.data[2] = (uint8_t)( ( b.data[2] & ~GX_TEL_BATCH_F_DROPPED ) | ( ( droppedSeconds != 0 || droppedEntries != 0 ) ? GX_TEL_BATCH_F_DROPPED : 0 ) );
	b.data[3] = (uint8_t)( droppedSeconds > 255 ? 255 : droppedSeconds );
	gxTelPut16( b.data + 12, droppedEntries > 0xFFFFu ? 0xFFFFu : droppedEntries );
}

// ------------------------------------------------------------------------------------------------
// The per-second PERF state. Pure, like the window above: GXReplayDiag.cpp feeds it one iteration and one completed draw at a time
// and fills the fields only the engine or the OS can give; scripts/test/replay-diag-format-test.cpp drives it with scripted lists.
//
// ROWS ARE ANCHORED TO THE WINDOW'S START. Second k of a window closes on the first iteration at or past start + k s, so ten rows
// tile a 10 s window exactly and the window's sums are its rows' sums (the crosscheck relies on it). One iteration longer than a
// second makes one long row, never synthetic empty ones. A pause flip, a suspension and the match end close the window and with
// it a partial row; wall_ms says how long each row is.
//
// TICKS ARE DRAINED AT EVERY CLOSE. GameEngine::takeLogicTickNs is take-and-reset, so each drain is added to the open row AND to
// the window; the window's SIM_MS is the sum of every drain since it began, exactly what one drain at its end used to give.
// ------------------------------------------------------------------------------------------------
enum
{
	GX_TEL_F_SUSPENDED     = 64,	///< PERF row: the loop stopped (minimised Windows solo, inactive iOS solo) at the row's end; a gap follows
	GX_DIAG_FLAG_SUSPENDED = 16,	///< WINDOW row: the same, for the window it closed
	GX_TEL_TRAIL_FRAMES    = 4096,	///< the newest frames kept for the trailing 3 s: every frame of 3 s up to 1365 fps; past it, the newest 4096
};
static const long long GX_TEL_TRAIL_NS = 3000000000LL;

typedef void (*GxTelTakeTicksFn)( long long &totalNs, long long &maxNs );

struct GxTelPerf
{
	GxTelTakeTicksFn takeTicks;
	long long originNs;						///< the recording's start: row walls are differences of ms since it, so they add up exactly
	long long winSimNs, winTickMaxNs;		///< every drain since the window began
	long long anchorNs;						///< the window's start: row k ends on the first iteration at or past anchorNs + k s
	long long nextK;
	long long rowStartNs, lastNs;
	unsigned int rowStartFrame, lastFrame, rowDrawnBase;
	int rowIters, rowCushionMin, rowFlags;
	long long rowHoldBaseNs, lastHoldNs, rowDrawBaseNs, lastDrawTotalNs, rowStallNs, rowSimNs, rowTickMaxNs, rowIterMaxNs;
	long long prevDrawNs;					///< the last completed draw; -1 after a start or a suspension, so no frame spans one
	GxTelFrameHist hist;					///< this row's frame times
	uint16_t rowLog[ GX_TEL_LOG_BUCKETS ];
	uint16_t batchLog[ GX_TEL_LOG_BUCKETS ];	///< the closed rows' frames since the window began: the batch's 16-bucket histogram
	long long trailEndNs[ GX_TEL_TRAIL_FRAMES ];	///< each kept frame's completion time and length, oldest overwritten first
	long long trailFrameNs[ GX_TEL_TRAIL_FRAMES ];
	int trailNext, trailCount;
	GxTelFrameHist trailSum;				///< scratch for the trailing percentile; here so nothing 4 KB lands on the stack
	bool away;								///< gxTelPerfAway: the player is not looking (unfocused, minimised, backgrounded)
};

inline void gxTelPerfInit( GxTelPerf &p, GxTelTakeTicksFn takeTicks, long long originNs = 0 )
{
	memset( &p, 0, sizeof( p ) );
	p.takeTicks = takeTicks;
	p.originNs = originNs;
	p.prevDrawNs = -1;
}

inline void gxTelPerfStartRow( GxTelPerf &p, const GXDiagIter &it )
{
	p.rowStartNs = p.lastNs = it.nowNs;
	p.rowStartFrame = p.lastFrame = it.logicFrame;
	p.rowDrawnBase = it.drawnTotal;
	p.rowIters = 0;
	p.rowCushionMin = -1;
	p.rowFlags = it.paused ? GX_TEL_F_PAUSED : 0;
#ifndef GX_TEL_RED_AWAY	// fail arm: a row started while away is not flagged, only the one open when it began
	if( p.away )
		p.rowFlags |= GX_TEL_F_UNFOCUSED;
#endif
	p.rowHoldBaseNs = p.lastHoldNs = it.holdTotalNs;
	p.rowDrawBaseNs = p.lastDrawTotalNs = it.drawTotalNs;
	p.rowStallNs = p.rowSimNs = p.rowTickMaxNs = p.rowIterMaxNs = 0;
	gxTelHistReset( p.hist );
	memset( p.rowLog, 0, sizeof( p.rowLog ) );
}

/// Every close drains the engine's tick counter into the open row and the window.
inline void gxTelPerfDrain( GxTelPerf &p )
{
	long long sum = 0, max = 0;
	if( p.takeTicks != NULL )
		p.takeTicks( sum, max );
	p.rowSimNs += sum;
	p.winSimNs += sum;
	if( max > p.rowTickMaxNs )
		p.rowTickMaxNs = max;
	if( max > p.winTickMaxNs )
		p.winTickMaxNs = max;
}

/// A window begins (with its first row) at `it`. dropTicks at a restart: the load's ticks belong to no row (gxPerfRestartWindow).
inline void gxTelPerfStartWindow( GxTelPerf &p, const GXDiagIter &it, bool dropTicks )
{
	if( dropTicks )
	{
		long long a = 0, b = 0;
		if( p.takeTicks != NULL )
			p.takeTicks( a, b );
		p.prevDrawNs = -1;
		p.trailNext = p.trailCount = 0;	// and its frames: the load's never reach the play's trailing 3 s
	}
	p.winSimNs = p.winTickMaxNs = 0;
	p.anchorNs = it.nowNs;
	p.nextK = 1;
	memset( p.batchLog, 0, sizeof( p.batchLog ) );
	gxTelPerfStartRow( p, it );
}

inline void gxTelPerfFrameDrawn( GxTelPerf &p, long long nowNs )
{
	if( p.prevDrawNs >= 0 && nowNs >= p.prevDrawNs )
	{
		const long long ns = nowNs - p.prevDrawNs;
		gxTelHistAdd( p.hist, ns );
		p.trailEndNs[ p.trailNext ] = nowNs;
		p.trailFrameNs[ p.trailNext ] = ns;
		p.trailNext = ( p.trailNext + 1 ) % GX_TEL_TRAIL_FRAMES;
		if( p.trailCount < GX_TEL_TRAIL_FRAMES )
			++p.trailCount;
		uint16_t &c = p.rowLog[ gxTelLogBucket( ns ) ];
		if( c != 0xFFFF )
			++c;
	}
	p.prevDrawNs = nowNs;
}

/// The loop stopped (gxReplayTelSuspend): the frame that spans the stop is not a frame anyone saw.
inline void gxTelPerfSuspend( GxTelPerf &p )
{
	p.prevDrawNs = -1;
}

/// GeneralsX: The player is away (true) or back (false) while the loop KEEPS running: a match that
/// goes on unwatched (iOS in the background, a minimised or Alt+Tabbed Windows peer). Every row that overlaps the away span closes
/// with GX_TEL_F_UNFOCUSED - the one open when it began, every one started during it, and the one open when it ended - so fps 0 at
/// 60 TPS reads as "nobody was looking", never as a render failure. A flag only: no row closes here and no field moves.
inline void gxTelPerfAway( GxTelPerf &p, bool away )
{
	p.away = away;
	if( away )
		p.rowFlags |= GX_TEL_F_UNFOCUSED;
}

/// One iteration, after gxDiagWindowIter's rules (stall disjoint from hold and draw, frame reset, cushion). True when a row is due.
inline bool gxTelPerfIter( GxTelPerf &p, const GXDiagIter &it )
{
	const long long since = it.nowNs - p.lastNs;
	p.lastNs = it.nowNs;
	++p.rowIters;
	if( since > p.rowIterMaxNs )
		p.rowIterMaxNs = since;
	const long long holdDelta = it.holdTotalNs - p.lastHoldNs;
	p.lastHoldNs = it.holdTotalNs;
	const long long drawDelta = it.drawTotalNs - p.lastDrawTotalNs;
	p.lastDrawTotalNs = it.drawTotalNs;
	if( it.logicFrame == p.lastFrame && it.stalling && since - holdDelta - drawDelta > 0 )
		p.rowStallNs += since - holdDelta - drawDelta;
	if( it.logicFrame < p.lastFrame )
		p.rowFlags |= GX_TEL_F_FRAME_RESET;
	p.lastFrame = it.logicFrame;
	if( it.cushion >= 0 && ( p.rowCushionMin < 0 || it.cushion < p.rowCushionMin ) )
		p.rowCushionMin = it.cushion;
	return it.nowNs >= p.anchorNs + p.nextK * GX_DIAG_SECOND_NS;
}

/// Close the open row at `it` (drains the ticks first) and start the next one there. Fills what the engine's own counters say;
/// the network, CPU and OS fields are UNKNOWN here and GXReplayDiag.cpp fills what it can read. extraFlags: GX_TEL_F_SUSPENDED.
inline void gxTelPerfCloseRow( GxTelPerf &p, const GXDiagIter &it, int extraFlags, bool headless, GxTelPerfRow &r )
{
	gxTelPerfDrain( p );
	memset( &r, 0xFF, sizeof( r ) );	// every field UNKNOWN (0xFFFF / 0xFF) until set
	const int flags = p.rowFlags | extraFlags | ( headless ? GX_TEL_F_HEADLESS : 0 );
	r.wallMs = gxTelNarrowU16( ( it.nowNs - p.originNs ) / 1000000 - ( p.rowStartNs - p.originNs ) / 1000000 );
	r.drawn = headless ? GX_TEL_U16_UNKNOWN : gxTelNarrowU16( (long long)( it.drawnTotal - p.rowDrawnBase ) );
	r.iters = gxTelNarrowU16( p.rowIters );
	r.ftP50 = gxTelHistPct( p.hist, 500 );
	r.ftP95 = gxTelHistPct( p.hist, 950 );
	r.ftP99 = gxTelHistPct( p.hist, 990 );
	r.ftMax = p.hist.n == 0 ? GX_TEL_U16_UNKNOWN : gxTelNarrowU16( p.hist.maxX10 );

	// The trailing 3 s: every frame that COMPLETED less than 3 s before this row ends, newest first - never a whole row because it
	// ended inside them (a row that a 12 s frame closed holds frames 12 s old).
	gxTelHistReset( p.trailSum );
	for( int k = 0; k < p.trailCount; ++k )
	{
		const int i = ( p.trailNext - 1 - k + GX_TEL_TRAIL_FRAMES ) % GX_TEL_TRAIL_FRAMES;
		if( p.trailEndNs[i] <= it.nowNs - GX_TEL_TRAIL_NS )
			break;	// completion order: everything older is older still
		if( p.trailEndNs[i] <= it.nowNs )
			gxTelHistAdd( p.trailSum, p.trailFrameNs[i] );
	}
	r.ftP99_3s = gxTelHistPct( p.trailSum, 990 );

	r.ticks = ( flags & GX_TEL_F_FRAME_RESET ) || it.logicFrame < p.rowStartFrame ? GX_TEL_U16_UNKNOWN
		: gxTelNarrowU16( (long long)( it.logicFrame - p.rowStartFrame ) );
	r.tickMaxX10 = gxTelNarrowU16( p.rowTickMaxNs / 100000 );
	r.simMs = gxTelNarrowU16( p.rowSimNs / 1000000 );
	r.stallMs = gxTelNarrowU16( p.rowStallNs / 1000000 );
	r.holdMs = gxTelNarrowU16( ( it.holdTotalNs - p.rowHoldBaseNs ) / 1000000 );
	r.drawMs = headless ? GX_TEL_U16_UNKNOWN : gxTelNarrowU16( ( it.drawTotalNs - p.rowDrawBaseNs ) / 1000000 );
	r.cushionMin = gxTelNarrowU8( p.rowCushionMin );
	r.flags = (uint8_t)flags;
	r.iterMaxX10 = gxTelNarrowU16( p.rowIterMaxNs / 100000 );
	for( int i = 0; i < GX_TEL_LOG_BUCKETS; ++i )
	{
		const unsigned s = (unsigned)p.batchLog[i] + p.rowLog[i];
		p.batchLog[i] = (uint16_t)( s > 0xFFFFu ? 0xFFFFu : s );
	}
	p.nextK = ( it.nowNs - p.anchorNs ) / GX_DIAG_SECOND_NS + 1;
	gxTelPerfStartRow( p, it );
}

/// The window's tick sums for its WINDOW row, after its last row closed; reset for the next window.
inline void gxTelPerfTakeWindowTicks( GxTelPerf &p, long long &simNs, long long &maxNs )
{
	simNs = p.winSimNs;
	maxNs = p.winTickMaxNs;
	p.winSimNs = p.winTickMaxNs = 0;
}

// The slow-I/O rule (spec 2.5): a telemetry write over 20 ms skips kinds 3-5 for the next 60 s of WALL time (a deadline, not a window
// count: pause flips close windows early). The skipped rows are not written; the next batch that is carries their seconds.
struct GxTelSlowIo
{
	long long skipUntilNs, droppedNs, worstWriteNs;
};
static const long long GX_TEL_SLOW_WRITE_NS = 20000000LL;
static const long long GX_TEL_SLOW_SKIP_NS = 60000000000LL;

inline void gxTelSlowIoInit( GxTelSlowIo &s )
{
	s.skipUntilNs = -1;
	s.droppedNs = s.worstWriteNs = 0;
}

inline void gxTelSlowIoWrote( GxTelSlowIo &s, long long nowNs, long long writeNs )
{
	if( writeNs > s.worstWriteNs )
		s.worstWriteNs = writeNs;
	if( writeNs > GX_TEL_SLOW_WRITE_NS )
		s.skipUntilNs = nowNs + GX_TEL_SLOW_SKIP_NS;
}

inline bool gxTelSlowIoSkip( const GxTelSlowIo &s, long long nowNs )
{
	return s.skipUntilNs >= 0 && nowNs < s.skipUntilNs;
}

inline void gxTelSlowIoDropped( GxTelSlowIo &s, long long wallNs )
{
	if( wallNs > 0 )
		s.droppedNs += wallNs;
}

/// Whether this window's batch is written. Inside the cooldown it is skipped - except the recording's LAST batch, which carries what
/// was skipped: without it a match that ends inside the cooldown loses its tail with no trace. At most one
/// more write per recording, at the match end.
inline bool gxTelSlowIoWrites( const GxTelSlowIo &s, long long nowNs, bool last )
{
	return last || !gxTelSlowIoSkip( s, nowNs );
}

/// Whether a suspension or the match end closes the open window (a partial row flagged suspended, or the last batch). Every primed
/// window does, even one that started on this very iteration with no iteration in it yet (a rollover): otherwise no row says
/// "suspended", the reader shows the gap as unexplained, and a match-end batch with skipped seconds is never written.
inline bool gxTelWindowToClose( const GXDiagWindow &w )
{
	return w.primed;
}

inline unsigned gxTelSlowIoTakeDroppedSeconds( GxTelSlowIo &s )
{
	const long long sec = ( s.droppedNs + GX_DIAG_SECOND_NS - 1 ) / GX_DIAG_SECOND_NS;
	s.droppedNs = 0;
	return sec > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (unsigned)sec;
}
