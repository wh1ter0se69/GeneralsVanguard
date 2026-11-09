// GeneralsX @feature Claude 28/09/2026 - [GXGFX], the per-frame render split (graphics P2, item i-01).
//
// WHY. No perf claim, governor or upscaler decision can be made until a rendered frame can be split
// honestly, and the live build had no per-frame render timer at all: [GXDRAW] counts draws (badly, see
// W3DDisplay.cpp) and [GXPACE] times the whole loop. This header owns the clocks and the accumulator;
// W3DDisplay::draw marks the spans, DX8Wrapper::End_Scene calls gxGfxPresentMark around Present through a
// function pointer (WW3D2 cannot include this file), and GameClient::update prints one line per window.
//
// WHAT `pres` IS, because the name invites the wrong reading. It is the time the MAIN thread spends inside
// IDirect3DDevice8::Present. Under DXVK that call waits in SyncFrameLatency for frame N-latency to clear the
// ENTIRE downstream chain: the dxvk-cs translation thread, the submission thread (on Apple that is where
// MoltenVK encodes Metal), the GPU, presentation/vsync and DXVK's own frame limiter. It is NOT GPU time and it
// cannot tell a busy GPU from an idle one on a FIFO swapchain. Classes that need that distinction (gpu,
// display, cs) need DXVK's own counters (graphics P1 items i-03/i-04) and are reserved, not faked.
//
// DIGEST. Core/GameEngine/Include/GameClient is not in the SimID source digest (simid-digest.sh lists only
// Common/GameLogic/GameNetwork under it), and every caller is in GameClient/, GameEngineDevice/ or WW3D2
// source, none digested. The headless peer runs none of it: the frame marks sit after W3DDisplay::draw's
// m_headless return, and nothing is printed until a rendered frame completes.
//
// OFF COSTS A BRANCH. Everything is gated on GX_GFX (or GX_HITCH for [GXDRAWP]) read once into a static.
// With both unset no clock is read and nothing is allocated; the state below is plain static storage.

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <bit>
#include <thread>
#include <chrono>
#include "GameClient/GXPerfReport.h"	// gxPerfMonoNs: the one wall clock [GXPACE] also uses

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

enum GxGfxSpan
{
	GXGFX_PREP,		// draw() entry after the headless return: dynamic LOD, terrain tracks, shroud render, WW3D::Sync
	GXGFX_VIEWS,	// updateViews
	GXGFX_PART,		// ParticleSystemManager::update
	GXGFX_RTT,		// water / projected-shadow render-to-texture pre-pass, Pillarbox_Begin
	GXGFX_BEGIN,	// WW3D::Begin_Render (clear, BeginScene)
	GXGFX_SCENE,	// drawViews: the 3D world, particles and sorted translucents submitted inside it
	GXGFX_UI,		// InGameUI, GameClient 2D, mouse, video, letterbox, debug displays: up to End_Render
	GXGFX_FLUSH,	// WW3D::End_Render minus Present: the sorting flush, EndScene and end-of-frame unbinds
	GXGFX_PRES,		// IDirect3DDevice8::Present on the main thread: the whole DXVK downstream wait (see banner)
	GXGFX_SPANS
};

// The printed names, in enum order. `<name>` is wall ms, `<name>c` the same span's thread CPU ms.
static const char *const kGxGfxSpanName[GXGFX_SPANS] = {
	"prep", "views", "part", "rtt", "begin", "scene", "ui", "flush", "pres" };

enum { GXGFX_HIST = 288 };	// log-linear, 8 buckets per octave (12.5%), the W8 GXInstr.h layout

inline int gxGfxBucket( int64_t ns )
{
	if( ns < 8 )
		return ns < 0 ? 0 : (int)ns;
	const int bw = (int)std::bit_width( (uint64_t)ns );
	const int i = 8 * ( bw - 3 ) + (int)( ( ns >> ( bw - 4 ) ) & 7 );
	return i < GXGFX_HIST ? i : GXGFX_HIST - 1;
}

// Upper edge of bucket i in ns: a percentile read from it is an upper bound, 12.5% coarse.
inline int64_t gxGfxBucketHigh( int i )
{
	if( i < 8 )
		return i + 1;
	const int bw = i / 8 + 3;
	return (int64_t)( 9 + i % 8 ) << ( bw - 4 );
}

// GeneralsX @feature Claude 28/09/2026 [GXGFXF] (graphics P2 c3-12): where the `scene` span goes, and the per-feature
// work the T3 features (lights, water, particles) will grow. Printed with GX_GFX=1, one line per [GXGFX] window.
// Sub-spans (wall only, one read per boundary) split RTS3DScene::Customized_Render and RTS3DScene::Flush in their
// own order: vis (visibility check), fupd (every render object's On_Frame_Update: terrain dynamic relight,
// emitters), terr (terrain draw), obj (object submit), then Flush's decal (projected shadows/decals), opaque
// (TheDX8MeshRenderer flush), occl (occluded objects into stencil), shd (shader meshes), trees, vol (stencil
// shadow volumes), static (static sort lists: water), transl (translucent meshes), partq (particle queue build),
// sort (the sorting flush: particles and alpha). Summed over every scene render in the frame, including the
// shadow/water render-to-texture passes. Counters: lights (dynamic lights terrain relit), relit (terrain
// vertices relit, one doTheDynamicLight call each), smcopy (heat-haze full back-buffer copies), pon/ptot
// (particles on screen / alive), and hmupd (ms in HeightMapRenderObjClass::On_Frame_Update, inside fupd).
// NOT covered: DXVK's CS thread (translation, pipelines) - main-thread CPU only; renderMirror (dead while the
// shipped GameData has WaterType=0; add it when T3 turns water type 2 on).
enum GxGfxSub
{
	GXSUB_VIS, GXSUB_FUPD, GXSUB_TERR, GXSUB_OBJ, GXSUB_DECAL, GXSUB_OPAQUE, GXSUB_OCCL, GXSUB_SHD, GXSUB_TREES,
	GXSUB_VOL, GXSUB_STATIC, GXSUB_TRANSL, GXSUB_PARTQ, GXSUB_SORT, GXSUB_N
};
static const char *const kGxGfxSubName[GXSUB_N] = {
	"vis", "fupd", "terr", "obj", "decal", "opaque", "occl", "shd", "trees", "vol", "static", "transl", "partq", "sort" };
enum GxGfxFeat { GXF_LIGHTS, GXF_RELIT, GXF_SMCOPY, GXF_PON, GXF_PTOT, GXF_HMUPD, GXF_N };
static const char *const kGxGfxFeatName[GXF_N] = { "lights", "relit", "smcopy", "pon", "ptot", "hmupd" };

// GeneralsX @feature Claude 28/09/2026 [GXSHADOW] (graphics P2 c1-01): where the stencil-shadow CPU goes TODAY.
// Every shadow profile on record predates the 23/09 shadow fixes (TS-06 reuse, the 64K ring, setShadowWorld,
// world-space upload, the cull-before-height), so the current cost is unknown. GX_SHADOWSTATS=1 (with GX_GFX=1,
// whose window it shares) counts and times inside W3DVolumetricShadowManager::renderShadows and the player-colour
// stencil flush. Counters are what the earlier profiles found dominant: casters walked vs enabled, dynamic vs static
// volumes drawn, upload bytes, ring DISCARDs (including the forced first one of each frame), TS-06 reuse hits vs
// re-uploads in the DECR pass, setShadowWorld sets vs skips, silhouette rebuilds by trigger, volumes turned
// SHADOW_DYNAMIC, slot failures. Timers (wall): Update per caster (a pair per enabled caster), the INCR pass minus
// Update, the DECR pass, the resolve quad, the whole of renderShadows, the occluded-object stencil flush and the
// renderStenciledPlayerColor quads inside it (occ - pcol is ordinary mesh submission, NOT stencil work).
enum GxShadowStat
{
	GXS_CAST, GXS_CASTON, GXS_DYNV, GXS_STATV, GXS_UPB, GXS_DISC, GXS_REUSE, GXS_REUP, GXS_WSET, GXS_WSKIP,
	GXS_RBROT, GXS_RBLIGHT, GXS_RBZ, GXS_DYNFLIP, GXS_SLOTFAIL, GXS_SREFILL,
	GXS_GPUV, GXS_GPUSW, GXS_GPUDRAW, GXS_GPUBLD, GXS_GPUREF,	// GX_SHADOW_GPU (08/10/2026): volumes queued, meshes drawn, draws, edge meshes built / refused
	GXS_T_UPD, GXS_T_INCR, GXS_T_DECR, GXS_T_QUAD, GXS_T_ALL, GXS_T_OCC, GXS_T_PCOL,
	GXS_N
};
static const char *const kGxShadowName[GXS_N] = {
	"cast", "caston", "dynv", "statv", "upkb", "disc", "reuse", "reup", "wset", "wskip",
	"rbrot", "rblight", "rbz", "dynflip", "slotfail", "srefill",
	"gpuv", "gpusw", "gpudraw", "gpubld", "gpuref",
	"upd", "incr", "decr", "quad", "all", "occ", "pcol" };
struct GxGfxWindow
{
	int64_t  span[ GXGFX_SPANS ];		// ns, summed over completed frames
	int64_t  spanCpu[ GXGFX_SPANS ];
	int64_t  draw, drawCpu, drawMax;
	int64_t  pint, pintMax, presMax;
	uint32_t pintN;
	uint32_t n, skip;
	uint64_t marks;						// clock-read pairs (one wall + one CPU read each)
	int64_t  cli;
	uint32_t cliN;
	uint64_t dc, dcw, tex, rs;			// i-02: device draws (Present to Present), wrapper draws, texture and
	uint32_t dcMax, dcN;				// render-state changes; dcN frames with a known dc
	uint32_t histDraw[ GXGFX_HIST ];
	uint32_t histPint[ GXGFX_HIST ];
	int64_t  sub[ GXSUB_N ];			// [GXGFXF] ns
	uint64_t feat[ GXF_N ];				// [GXGFXF] counts; GXF_HMUPD in ns
	uint64_t shadow[ GXS_N ];			// [GXSHADOW] sums; GXS_T_* in ns, GXS_UPB in bytes
	uint32_t pintHistN;					// intervals actually in histPint (at most 8 per draw)
};

struct GxGfxState
{
	// frame in progress
	bool     open;
	int64_t  lastW, lastC;				// the previous mark
	int64_t  startW, startC;
	int64_t  span[ GXGFX_SPANS ];
	int64_t  spanCpu[ GXGFX_SPANS ];
	uint32_t marks;
	uint32_t presents;
	// Present returns, across frames: pint is the interval between the last two
	int64_t  lastRet, prevRet;
	uint64_t dcAtPresent;				// DX8Wrapper::Get_Draw_Calls_Total() at the last Present
	bool     dcHavePresent;				// a Present has been seen (a total of 0 is a valid reading, not "none")
	// this draw()'s Presents (the freeze-time loop can Present more than once per draw): device draws summed over
	// them (dcUnknown when one had no earlier Present to count from), and every Present-to-Present interval
	uint64_t dcDraw;
	bool     dcUnknown;
	int64_t  pintSum, pintMaxF, presMaxF;
	uint32_t pintCnt;
	int64_t  pintSample[ 8 ];			// the first 8 intervals of this draw feed the p99 histogram
	int64_t  cliStart;
	int64_t  subLast;					// [GXGFXF] the previous sub-span boundary
	// this draw's [GXGFXF] and [GXSHADOW] measurements: merged into the window only by a completed draw's commit,
	// dropped by a skip, so a pass that never presents is never charged to another frame (Codex gfxp2-r2)
	int64_t  subDraw[ GXSUB_N ];
	uint64_t featDraw[ GXF_N ];
	uint64_t shadowDraw[ GXS_N ];
	int64_t  lastDrawW, lastDrawC;		// the last committed frame, for [GXDRAWP]
	// window
	GxGfxWindow win;
	int64_t  winStart;
	uint32_t winFrame0;
	bool     inited;
	// init-time calibration
	double   cyclesPerNs;				// Windows only; 0 elsewhere
	double   readWallNs, readCpuNs;
#if defined(_WIN32)
	// Codex gfxp2-r5: the cycles->ns ratio is measured once. Each window re-derives it from the kernel's own thread
	// accounting (GetThreadTimes, 100 ns units but tick-quantised), printed as cpnw=, so a rate that drifted with
	// boost or throttling is SEEN rather than trusted. These hold the previous window's readings.
	ULONG64  winCycles0;
	ULONG64  winThreadTime0;			// kernel + user, 100 ns
#endif
	// GX_GFX_LOAD known load (gxGfxParseLoad): span index or -1, microseconds, sleep instead of spin, and whether
	// the CURRENT window is a loaded one (the emitter flips it at every print)
	int      loadSpan, loadUs;
	bool     loadSleep, loadActive;
};

inline GxGfxState g_gxGfx;

// GeneralsX @test Claude 08/10/2026 [GXCENSUS] (GX_DRAWCENSUS=1 with GX_GFX=1; R-CPU roadmap step 2): the draw/state
// census per [GXGFXF] sub-span. The counters are WW3D2's never-reset totals (DX8Wrapper::Census, names and meaning
// in dx8wrapper.h enum DX8CensusCounter); W3DDisplay attaches them here because WW3D2 cannot include this file. Each
// sub-span mark charges the counters' movement since the previous mark to that sub-span, exactly as it charges the
// wall time, and a committed draw charges its whole movement (UI, render-to-texture passes, Present included) to
// "all", so all - (sum of the sub-spans) is the work outside the scene sub-spans. A draw that never commits is
// dropped, as [GXGFXF] drops it.
enum { GXCEN_MAX = 80 };	// >= DX8C_N (dx8wrapper.h)
struct GxCensus
{
	const unsigned long long *src;					// null = off
	int n;
	const char *const *names;
	unsigned long long last[ GXCEN_MAX ];			// at the previous sub-span boundary
	unsigned long long frame0[ GXCEN_MAX ];			// at gxGfxFrameBegin
	unsigned long long draw[ GXSUB_N + 1 ][ GXCEN_MAX ];	// this draw; [GXSUB_N] = the whole draw
	unsigned long long win[ GXSUB_N + 1 ][ GXCEN_MAX ];		// window sums over committed draws
};
inline GxCensus g_gxCensus;

inline bool gxCensusWanted();
inline bool gxCensusOn() { return g_gxCensus.src != NULL; }

inline void gxCensusAttach( const unsigned long long *src, int n, const char *const *names )
{
	GxCensus &c = g_gxCensus;
	c.n = n < (int)GXCEN_MAX ? n : (int)GXCEN_MAX;
	c.names = names;
	memcpy( c.last, src, sizeof( c.last[ 0 ] ) * (size_t)c.n );
	memcpy( c.frame0, src, sizeof( c.frame0[ 0 ] ) * (size_t)c.n );
	c.src = src;
}

inline void gxCensusCharge( int span, unsigned long long *base )
{
	GxCensus &c = g_gxCensus;
	for( int k = 0; k < c.n; ++k )
	{
		const unsigned long long v = c.src[ k ];
		c.draw[ span ][ k ] += v - base[ k ];
		base[ k ] = v;
	}
}

// [GXGFX] and the [GXDRAWP] per-hitch line both need the marks; either env var turns them on.
inline bool gxGfxLineOn()
{
	static const bool s_on = ( getenv( "GX_GFX" ) != NULL );
	return s_on;
}

inline int gxGfxHitchMs()
{
	static const int s_ms = ( getenv( "GX_HITCH" ) != NULL ) ? atoi( getenv( "GX_HITCH" ) ) : 0;
	return s_ms;
}

// GeneralsX @feature Claude 17/10/2026 The look governor (GXLookGovernor.h, S14) needs this frame's draw CPU and Present wait
// without the printed lines: W3DDisplay sets this once, before the first frame it governs. The clocks then run as with
// GX_HITCH alone (frames are marked, nothing accumulates or prints unless GX_GFX is set too).
inline bool g_gxGfxGovTiming = false;

inline bool gxGfxOn()
{
	static const bool s_on = gxGfxLineOn() || gxGfxHitchMs() > 0;
	return s_on || g_gxGfxGovTiming;
}

inline int64_t gxGfxWallNs() { return (int64_t)gxPerfMonoNs(); }

#if defined(_WIN32)
// The calling thread's kernel + user time in 100 ns units (GetThreadTimes), for the per-window cpnw= cross-check.
inline ULONG64 gxGfxThreadTime100ns()
{
	FILETIME c, e, k, u;
	if( !GetThreadTimes( GetCurrentThread(), &c, &e, &k, &u ) )
		return 0;
	return ( ( (ULONG64)k.dwHighDateTime << 32 ) | k.dwLowDateTime ) + ( ( (ULONG64)u.dwHighDateTime << 32 ) | u.dwLowDateTime );
}
#endif

// Thread CPU time of the calling thread. POSIX (macOS, iOS, Linux): CLOCK_THREAD_CPUTIME_ID, the GameEngine.cpp
// [GXTICK] precedent. Windows: QueryThreadCycleTime cycles converted with a ratio measured at init against the
// wall clock (gxGfxCalibrate). Microsoft warns cycles are not time; the ratio is only trusted because the
// GX_GFX_LOAD known-load test proves a spin of N us reads N us here. Returns 0 when the ratio is unknown.
inline int64_t gxGfxCpuNs()
{
#if defined(_WIN32)
	ULONG64 cycles = 0;
	QueryThreadCycleTime( GetCurrentThread(), &cycles );
	return g_gxGfx.cyclesPerNs > 0.0 ? (int64_t)( (double)cycles / g_gxGfx.cyclesPerNs ) : 0;
#else
	struct timespec t;
	clock_gettime( CLOCK_THREAD_CPUTIME_ID, &t );
	return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
#endif
}

// GX_GFX_LOAD=<span>:<us>[:sleep], e.g. scene:2000 or ui:2000:sleep. The calibration test for [GXGFX]
// (graphics P2 i-06): a KNOWN load at a KNOWN place, so the report tool can check it lands in that span's wall
// time, and in its CPU time for a spin but not for a sleep (gx-gfx-report.py --calib). Render-only: it delays
// the main thread inside W3DDisplay::draw and touches no state. Loaded and unloaded windows alternate, so one
// run compares itself. Never set it in a timing arm.
inline void gxGfxParseLoad()
{
	GxGfxState &g = g_gxGfx;
	g.loadSpan = -1;
	const char *e = getenv( "GX_GFX_LOAD" );
	if( e == NULL || !gxGfxLineOn() )
		return;
	char name[ 16 ] = { 0 };
	int us = 0;
	char mode[ 16 ] = { 0 };
	if( sscanf( e, "%15[a-z]:%d:%15s", name, &us, mode ) < 2 || us <= 0 || us > 100000 )
	{
		fprintf( stderr, "[GXGFX] GX_GFX_LOAD='%s' rejected (want <span>:<us 1-100000>[:sleep])\n", e );
		return;
	}
	for( int s = 0; s < GXGFX_SPANS; ++s )
		if( strcmp( name, kGxGfxSpanName[ s ] ) == 0 && s != GXGFX_PRES )
			g.loadSpan = s;
	if( g.loadSpan < 0 )
	{
		fprintf( stderr, "[GXGFX] GX_GFX_LOAD='%s' rejected (no such span, or pres, which is DXVK's)\n", e );
		return;
	}
	g.loadUs = us;
	g.loadSleep = strcmp( mode, "sleep" ) == 0;
}

inline void gxGfxRunLoad()
{
	GxGfxState &g = g_gxGfx;
	if( g.loadSleep )
	{
		std::this_thread::sleep_for( std::chrono::microseconds( g.loadUs ) );
		return;
	}
	const int64_t until = gxGfxWallNs() + (int64_t)g.loadUs * 1000;
	while( gxGfxWallNs() < until )
	{
	}
}

// Once, on the first marked frame (never on a headless peer). Measures what a read of each clock costs, so the
// line can state its own overhead, and on Windows the cycles-per-ns ratio: the MAX of five 4 ms spins, because
// preemption during a spin can only LOWER the ratio (the thread's cycles stop while the wall clock runs on).
inline void gxGfxCalibrate()
{
	GxGfxState &g = g_gxGfx;
#if defined(_WIN32)
	for( int k = 0; k < 5; ++k )
	{
		ULONG64 c0 = 0, c1 = 0;
		const int64_t w0 = gxGfxWallNs();
		QueryThreadCycleTime( GetCurrentThread(), &c0 );
		int64_t w1 = w0;
		while( w1 - w0 < 4000000 )
			w1 = gxGfxWallNs();
		QueryThreadCycleTime( GetCurrentThread(), &c1 );
		const double r = (double)( c1 - c0 ) / (double)( w1 - w0 );
		if( r > g.cyclesPerNs )
			g.cyclesPerNs = r;
	}
#endif
	const int READS = 2000;
	volatile int64_t sink = 0;
	int64_t t0 = gxGfxWallNs();
	for( int k = 0; k < READS; ++k )
		sink += gxGfxWallNs();
	int64_t t1 = gxGfxWallNs();
	g.readWallNs = (double)( t1 - t0 ) / READS;
	t0 = gxGfxWallNs();
	for( int k = 0; k < READS; ++k )
		sink += gxGfxCpuNs();
	t1 = gxGfxWallNs();
	g.readCpuNs = (double)( t1 - t0 ) / READS;
	(void)sink;
	gxGfxParseLoad();
	g.inited = true;
}

// Start of a draw() past the headless return. A frame begun and never committed or skipped (an early return
// inside draw) is counted as skipped here.
inline void gxGfxFrameBegin()
{
	if( !gxGfxOn() )
		return;
	GxGfxState &g = g_gxGfx;
	if( !g.inited )
		gxGfxCalibrate();
	if( g.open )
		++g.win.skip;
	g.open = true;
	memset( g.span, 0, sizeof( g.span ) );
	memset( g.spanCpu, 0, sizeof( g.spanCpu ) );
	g.presents = 0;
	g.dcDraw = 0;
	g.dcUnknown = false;
	g.pintSum = g.pintMaxF = g.presMaxF = 0;
	g.pintCnt = 0;
	memset( g.subDraw, 0, sizeof( g.subDraw ) );
	memset( g.featDraw, 0, sizeof( g.featDraw ) );
	memset( g.shadowDraw, 0, sizeof( g.shadowDraw ) );
	if( gxCensusOn() )
	{
		memset( g_gxCensus.draw, 0, sizeof( g_gxCensus.draw ) );
		memcpy( g_gxCensus.frame0, g_gxCensus.src, sizeof( g_gxCensus.frame0[ 0 ] ) * (size_t)g_gxCensus.n );
	}
	g.startW = g.lastW = gxGfxWallNs();
	g.startC = g.lastC = gxGfxCpuNs();
	g.marks = 1;
}

// Close span s: everything since the previous mark belongs to it. A span marked twice in one draw (the
// freeze-time loop repeats the body) accumulates. injectLoad=false for commit's closing flush mark, so a
// GX_GFX_LOAD on flush runs once per Present (in the pre-Present mark), not twice.
inline void gxGfxMark( GxGfxSpan s, bool injectLoad = true )
{
	if( !gxGfxOn() )
		return;
	GxGfxState &g = g_gxGfx;
	if( !g.open )
		return;
	if( injectLoad && g.loadActive && s == g.loadSpan )
		gxGfxRunLoad();		// GX_GFX_LOAD: inside span s, before its closing mark
	const int64_t w = gxGfxWallNs();
	const int64_t c = gxGfxCpuNs();
	g.span[ s ] += w - g.lastW;
	g.spanCpu[ s ] += c - g.lastC;
	g.lastW = w;
	g.lastC = c;
	++g.marks;
}

// DX8Wrapper::End_Scene calls this through DX8Wrapper::Present_Probe, before (after=false) and after Present.
// Present returns are recorded even outside a frame (the load screen), so pint stays a true interval.
inline void gxGfxPresentMark( bool after, uint64_t drawCallsTotal )
{
	GxGfxState &g = g_gxGfx;
	if( !after )
	{
		gxGfxMark( GXGFX_FLUSH );
		return;
	}
	const bool dcKnown = g.dcHavePresent;
	const uint64_t dc = drawCallsTotal - g.dcAtPresent;
	g.dcAtPresent = drawCallsTotal;
	g.dcHavePresent = true;
	const int64_t pres0 = g.span[ GXGFX_PRES ];
	gxGfxMark( GXGFX_PRES );
	g.prevRet = g.lastRet;
	g.lastRet = g.open ? g.lastW : gxGfxWallNs();
	if( !g.open )
		return;
	++g.presents;
	if( g.span[ GXGFX_PRES ] - pres0 > g.presMaxF )
		g.presMaxF = g.span[ GXGFX_PRES ] - pres0;
	if( dcKnown )
		g.dcDraw += dc;
	else
		g.dcUnknown = true;
	if( g.prevRet != 0 )
	{
		const int64_t pint = g.lastRet - g.prevRet;
		g.pintSum += pint;
		if( pint > g.pintMaxF )
			g.pintMaxF = pint;
		if( g.pintCnt < 8 )
			g.pintSample[ g.pintCnt ] = pint;
		++g.pintCnt;
	}
}

// A draw() that returned before the headless return because nothing may render (minimised on Windows, iOS
// render-suspended). Counted as skip so those windows still print; the caller excludes -headless peers.
inline void gxGfxSkipUnmarked()
{
	if( gxGfxLineOn() )
		++g_gxGfx.win.skip;
}

inline void gxGfxFrameSkip()
{
	if( !gxGfxOn() )
		return;
	GxGfxState &g = g_gxGfx;
	if( g.open )
		++g.win.skip;
	g.open = false;
}

// A draw() that completed End_Render. Everything after the last mark (post-Present bookkeeping) is flush.
inline void gxGfxFrameCommit( uint32_t wrapperDraws, uint32_t textureChanges, uint32_t renderStateChanges )
{
	if( !gxGfxOn() )
		return;
	GxGfxState &g = g_gxGfx;
	if( !g.open )
		return;
	gxGfxMark( GXGFX_FLUSH, false );
	g.open = false;
	const int64_t drawW = g.lastW - g.startW;
	const int64_t drawC = g.lastC - g.startC;
	g.lastDrawW = drawW;
	g.lastDrawC = drawC;
	if( !gxGfxLineOn() )
		return;	// GX_HITCH alone: [GXDRAWP] reads the frame, nothing accumulates
	GxGfxWindow &w = g.win;
	for( int s = 0; s < GXGFX_SPANS; ++s )
	{
		w.span[ s ] += g.span[ s ];
		w.spanCpu[ s ] += g.spanCpu[ s ];
	}
	w.draw += drawW;
	w.drawCpu += drawC;
	if( drawW > w.drawMax ) w.drawMax = drawW;
	if( g.presMaxF > w.presMax ) w.presMax = g.presMaxF;
	++w.histDraw[ gxGfxBucket( drawW ) ];
	w.pint += g.pintSum;
	w.pintN += g.pintCnt;
	if( g.pintMaxF > w.pintMax ) w.pintMax = g.pintMaxF;
	for( uint32_t k = 0; k < g.pintCnt && k < 8; ++k )
	{
		++w.histPint[ gxGfxBucket( g.pintSample[ k ] ) ];
		++w.pintHistN;
	}
	for( int k = 0; k < GXSUB_N; ++k )
		w.sub[ k ] += g.subDraw[ k ];
	for( int k = 0; k < GXF_N; ++k )
		w.feat[ k ] += g.featDraw[ k ];
	for( int k = 0; k < GXS_N; ++k )
		w.shadow[ k ] += g.shadowDraw[ k ];
	if( gxCensusOn() )
	{
		GxCensus &c = g_gxCensus;
		gxCensusCharge( GXSUB_N, c.frame0 );
		for( int s = 0; s <= GXSUB_N; ++s )
			for( int k = 0; k < c.n; ++k )
				c.win[ s ][ k ] += c.draw[ s ][ k ];
	}
	w.marks += g.marks;
	if( g.presents > 0 && !g.dcUnknown )
	{
		w.dc += g.dcDraw;
		if( g.dcDraw > w.dcMax ) w.dcMax = (uint32_t)g.dcDraw;
		++w.dcN;
	}
	w.dcw += wrapperDraws;
	w.tex += textureChanges;
	w.rs += renderStateChanges;
	++w.n;
}

// GameClient::update wall, top to the end of its DRAW: cli - draw is client work outside the renderer. Counted only
// once a frame has been marked (g.inited), which never happens on a -headless peer.
inline void gxGfxClientBegin()
{
	if( gxGfxLineOn() && g_gxGfx.inited )
		g_gxGfx.cliStart = gxGfxWallNs();
}

// GeneralsX @test Claude 02/10/2026 Leave a span out of cli= (H02's orders-before-draw pass, GameClient.cpp): cli - draw stays
// client work outside the renderer, comparable with builds that translated the orders after the client update.
inline void gxGfxClientExclude( int64_t ns )
{
	if( g_gxGfx.cliStart != 0 && ns > 0 )
		g_gxGfx.cliStart += ns;
}

inline void gxGfxClientEnd()
{
	GxGfxState &g = g_gxGfx;
	if( !gxGfxLineOn() || g.cliStart == 0 )
		return;
	g.win.cli += gxGfxWallNs() - g.cliStart;
	++g.win.cliN;
	g.cliStart = 0;
}

// GeneralsX @feature Claude 28/09/2026 [GXDRAWP] (graphics P2 f-06): the W8 per-hitch draw split
// (test/w8-probe9 6064c467e), ported onto the [GXGFX] marks instead of merged. W8 read CLOCK_THREAD_CPUTIME_ID
// unconditionally on every frame (it does not compile under MSVC, and its reads ran with the print off); here
// the marks run only when GX_GFX or GX_HITCH is set, Windows gets the calibrated cycle clock, and every span is
// printed as wall AND CPU. The threshold is WALL time (what a player feels), 0.7 x GX_HITCH ms as in W8.
// `flush` + `pres` are W8's `end`: the sort flush and EndScene, then the whole DXVK downstream wait in Present.
inline void gxGfxHitchLine( unsigned logicFrame, int particles )
{
	const int ms = gxGfxHitchMs();
	GxGfxState &g = g_gxGfx;
	if( ms <= 0 || g.lastDrawW <= (int64_t)ms * 700000LL )
		return;
	char buf[ 768 ];
	int len = snprintf( buf, sizeof( buf ), "[GXDRAWP] f=%u wall_ms=%.2f cpu_ms=%.2f", logicFrame,
		(double)g.lastDrawW * 1.0e-6, (double)g.lastDrawC * 1.0e-6 );
	for( int s = 0; s < GXGFX_SPANS && len > 0 && len < (int)sizeof( buf ); ++s )
		len += snprintf( buf + len, sizeof( buf ) - (size_t)len, " %s=%.2f %sc=%.2f", kGxGfxSpanName[ s ],
			(double)g.span[ s ] * 1.0e-6, kGxGfxSpanName[ s ], (double)g.spanCpu[ s ] * 1.0e-6 );
	if( len > 0 && len < (int)sizeof( buf ) )
		fprintf( stderr, "%s particles_n=%d\n", buf, particles );
	fflush( stderr );
}


inline bool gxShadowOn()
{
	static const bool s_on = gxGfxLineOn() && getenv( "GX_SHADOWSTATS" ) != NULL;
	return s_on;
}

inline bool gxCensusWanted()
{
	static const bool s_on = getenv( "GX_GFX" ) != NULL && getenv( "GX_DRAWCENSUS" ) != NULL;	// not GX_HITCH alone: nothing would print
	return s_on;
}

inline void gxShadowAdd( GxShadowStat s, uint64_t by = 1 )
{
	if( gxShadowOn() && g_gxGfx.open )
		g_gxGfx.shadowDraw[ s ] += by;
}

inline int64_t gxShadowT0() { return gxShadowOn() && g_gxGfx.open ? gxGfxWallNs() : 0; }

inline void gxShadowTAdd( GxShadowStat s, int64_t t0 )
{
	if( t0 != 0 && g_gxGfx.open )
		g_gxGfx.shadowDraw[ s ] += (uint64_t)( gxGfxWallNs() - t0 );
}

struct GxShadowScope	// times a whole function across every return
{
	GxShadowStat s;
	int64_t t0;
	explicit GxShadowScope( GxShadowStat stat ) : s( stat ), t0( gxShadowT0() ) {}
	~GxShadowScope() { gxShadowTAdd( s, t0 ); }
};

// [GXGFXF] sub-spans and counters: only inside a marked frame with the line on, else one branch.
inline bool gxGfxFeatOn() { return gxGfxLineOn() && g_gxGfx.open; }

inline void gxGfxSubStart()
{
	if( gxGfxFeatOn() )
	{
		g_gxGfx.subLast = gxGfxWallNs();
		if( gxCensusOn() )
			memcpy( g_gxCensus.last, g_gxCensus.src, sizeof( g_gxCensus.last[ 0 ] ) * (size_t)g_gxCensus.n );
	}
}

inline void gxGfxSubMark( GxGfxSub s )
{
	GxGfxState &g = g_gxGfx;
	if( !gxGfxFeatOn() || g.subLast == 0 )
		return;
	const int64_t now = gxGfxWallNs();
	g.subDraw[ s ] += now - g.subLast;
	g.subLast = now;
	if( gxCensusOn() )
		gxCensusCharge( s, g_gxCensus.last );
}

inline void gxGfxFeat( GxGfxFeat f, uint64_t by = 1 )
{
	if( gxGfxFeatOn() )
		g_gxGfx.featDraw[ f ] += by;
}

struct GxGfxFeatScope	// times a whole function into a feature counter, across every return
{
	int64_t t0;
	GxGfxFeat f;
	explicit GxGfxFeatScope( GxGfxFeat feat ) : t0( gxGfxFeatOn() ? gxGfxWallNs() : 0 ), f( feat ) {}
	~GxGfxFeatScope() { if( t0 != 0 && gxGfxFeatOn() ) g_gxGfx.featDraw[ f ] += (uint64_t)( gxGfxWallNs() - t0 ); }
};

#if defined(__APPLE__)
// GeneralsX @feature Claude 28/09/2026 [GXGFX] therm= lpm= (graphics P2 i-08): the OS's own throttling signals, the
// iPad governor's slow prior. NSProcessInfo.thermalState is 0 nominal, 1 fair, 2 serious, 3 critical (macOS 10.10.3+,
// iOS 11+; by Apple's header only serious/critical mean performance may be reduced), isLowPowerModeEnabled is 0/1
// (iOS 9+, macOS 12+). Asked through the Objective-C runtime from plain C++, under PRIVATE asm-label aliases: this
// header is included by translation units that see the Win32-compat BOOL, so <objc/objc.h> (whose BOOL differs) must
// never be included here, and an alias cannot collide with a TU that does include it. -1 = unknown (omitted).
// Linked by z_gameengine's explicit `objc` + Foundation on Apple (GeneralsMD/Code/GameEngine/CMakeLists.txt).
extern "C" void *gxObjcGetClass( const char *name ) __asm__( "_objc_getClass" );
extern "C" void *gxSelRegisterName( const char *name ) __asm__( "_sel_registerName" );
extern "C" void *gxObjectGetClass( void *obj ) __asm__( "_object_getClass" );
extern "C" signed char gxClassRespondsToSelector( void *cls, void *sel ) __asm__( "_class_respondsToSelector" );
extern "C" void gxObjcMsgSend( void ) __asm__( "_objc_msgSend" );

inline void *gxAppleProcessInfo()
{
	void *cls = gxObjcGetClass( "NSProcessInfo" );
	return cls != NULL ? ( (void *(*)( void *, void * ))gxObjcMsgSend )( cls, gxSelRegisterName( "processInfo" ) ) : NULL;
}

inline int gxAppleThermalState()
{
	void *pi = gxAppleProcessInfo();
	void *sel = gxSelRegisterName( "thermalState" );
	if( pi == NULL || !gxClassRespondsToSelector( gxObjectGetClass( pi ), sel ) )
		return -1;
	return (int)( (long (*)( void *, void * ))gxObjcMsgSend )( pi, sel );
}

inline int gxAppleLowPowerMode()
{
	void *pi = gxAppleProcessInfo();
	void *sel = gxSelRegisterName( "isLowPowerModeEnabled" );
	if( pi == NULL || !gxClassRespondsToSelector( gxObjectGetClass( pi ), sel ) )
		return -1;
	return ( (signed char (*)( void *, void * ))gxObjcMsgSend )( pi, sel ) != 0 ? 1 : 0;
}
#endif

// Upper bound of the p-th percentile (0..100) of a window histogram, in ns; 0 when empty.
inline int64_t gxGfxHistPct( const uint32_t *h, uint32_t count, double p )
{
	if( count == 0 )
		return 0;
	const uint32_t want = (uint32_t)( p / 100.0 * (double)count + 0.999999 );
	uint32_t seen = 0;
	for( int i = 0; i < GXGFX_HIST; ++i )
	{
		seen += h[ i ];
		if( seen >= want && h[ i ] != 0 )
			return gxGfxBucketHigh( i );
	}
	return gxGfxBucketHigh( GXGFX_HIST - 1 );
}
