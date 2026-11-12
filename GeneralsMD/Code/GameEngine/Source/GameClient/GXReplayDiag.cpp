/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// FILE: GXReplayDiag.cpp /////////////////////////////////////////////////////////////////////////
//
// See GXReplayDiag.h, and GXReplayDiagFormat.h for the bytes and why they are where they are.
//
// PRIVACY IS AN ALLOW-LIST. Players share replays and this repo is public, so nothing here reads a host name, an IP, a user
// name, an install path, a machine id, a serial or a MAC address. Options.ini holds addresses (IPAddress, RelayAddress,
// LocalVirtualIP...), which is why only the named keys in s_graphicsKeys leave it, and scripts/test/replay-diag-keys-test.py
// fails on any key the game reads that is in neither that table nor its own not-graphics list. Every value then goes through
// gxDiagSanitize as a backstop, and an Options.ini value only leaves as a number or a known word (gxDiagOptionValue).

#include "PreRTS.h"

#include "GameClient/GXReplayDiag.h"
#include "GameClient/GXReplayDiagFormat.h"
#include "GameClient/GXHwReport.h"
#include "GameClient/GXPerfReport.h"	// gxPerfMonoNs, gxPerfRestartWindow
#include "GameClient/Display.h"
#include "GameClient/GXGraphicsSettings.h"

#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GameLOD.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/Recorder.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/networkutil.h"	// TheGxNetRetries, TheGxNetRecvDrops: read only
#include "gitinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>	// usleep: GX_TEST_INSTRUMENTS only
#endif

#ifndef _WIN32
extern char **environ;
#endif

namespace {

enum { GX_DIAG_MAX_ROWS = 8640 };	// 24 hours of windows: a forgotten game cannot grow a replay without bound

/// GRAPHICS KEYS OF Options.ini. The test reads this table, so keep the one-string-per-entry shape.
const char *const s_graphicsKeys[] =
{
	"AnisotropyLevel", "AntiAliasing", "BuildingOcclusion", "DynamicLOD", "ExtraAnimations", "FPSLimit", "Gamma", "HeatEffects",
	"IdealStaticGameLOD", "MaxParticleCount", "Resolution", "ShowSoftWaterEdge", "ShowTrees", "StaticGameLOD",
	"TerrainDrawDistanceScale", "TextureFilter", "TextureReduction", "UseCloudMap", "UseLightMap", "UseShadowDecals",
	"UseShadowVolumes", "GXAlphaToCoverage", "GXFogInterpolation", "GXFrameGen", "GXFxLight", "GXGround", "GXLodDetector",
	"GXLookGovernor", "GXLookGovernorFps", "GXReflex", "GXRemastered", "GXRenderScale", "GXRenderer", "GXShadowMap",
	"GXTextureFilterPicked", "GXUpscaleQuality", "GXUpscaleSharpness", "GXUpscaler", "GXWaterClear", "GXWaterClearStrength",
	"GXWaterLive", "GXWaterLiveStrength", "GXWaterRA3",
};

char s_text[ GX_DIAG_STATIC_MAX ];
size_t s_textLen = 0;
UnsignedInt s_startCrc = 0;
GXReplayDiagDeviceFn s_deviceFacts = NULL;
GXDiagWindow s_window;
unsigned int s_framesDrawn = 0;
long long s_drawNs = 0;
long long s_drawTotalNs = 0;
long long s_drawMaxNs = 0;
int s_rows = 0;

// Telemetry (GXReplayDiagFormat.h, kind 3 PERF). One batch per window, written beside its WINDOW row.
GxTelLevel s_level = GX_TEL_PERF;
int s_shareOk = 0;
GxTelPerf s_tel;
GxTelBatch s_batch;
GxTelSlowIo s_slowIo;
long long s_recStartNs = 0;
bool s_suspended = false;
bool s_away = false;				///< gxReplayTelAway: kept across recordings, so a recording that starts away begins flagged
bool s_writeTiming = false;			///< GX_TEL_WRITE_TIMING=1: one [GXTEL] line per telemetry record (task 4 L)
int s_testSlowWriteMs = 0;			///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_SLOW_WRITE_MS: a write that sleeps, for the slow-I/O test
bool s_testRedGate = false;			///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_RED_GATE=1: the sampler ignores off - the red arm of the off test
bool s_netBase = false;
UnsignedInt s_lastRetries = 0, s_lastRecvDrops = 0;
long long s_lastCpuMainNs = -1;
GXReplayTelOsFn s_osReader = NULL;
GXReplayTelControlFn s_samplerControl = NULL;	///< phase 2: the device layer's sampler thread (start 1 / idle 0)
uint32_t s_osLastSeq = 0;
bool s_osHaveSeq = false;
long long s_osNotBeforeNs = 0;	///< when the loop last resumed (gxReplayTelSuspend) or a load ended: no OS snapshot begun before it fills a row
int s_testSpinMs = 0, s_testSpinFromS = 0, s_testSpinForS = 0;	///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_SPIN_*: a known load ON the main thread

/// GameEngine::takeLogicTickNs (take-and-reset). GxTelPerf drains it at every close, so the window and its rows share every tick.
void takeTicks( long long &sum, long long &max )
{
	sum = max = 0;
	if( TheGameEngine != NULL )
		TheGameEngine->takeLogicTickNs( sum, max );
}

bool sameWord( const char *a, const char *b )
{
	for( ; *a != 0 && *b != 0; ++a, ++b )
		if( ( *a >= 'A' && *a <= 'Z' ? *a + 'a' - 'A' : *a ) != *b )
			return false;
	return *a == 0 && *b == 0;
}

GxTelLevel readLevel( OptionPreferences &prefs )
{
	const OptionPreferences::const_iterator it = prefs.find( "GXReplayTelemetry" );
	if( it == prefs.end() )
		return GX_TEL_PERF;
	if( sameWord( it->second.str(), "off" ) )
		return GX_TEL_OFF;
	if( sameWord( it->second.str(), "full" ) )
		return GX_TEL_FULL;
	return GX_TEL_PERF;	// "perf", and anything else: the default (phase 1 records perf)
}

int readShareOk( OptionPreferences &prefs )
{
	const OptionPreferences::const_iterator it = prefs.find( "GXReplayShareOk" );
	return it != prefs.end() && strcmp( it->second.str(), "1" ) == 0 ? 1 : 0;
}

const char *levelWord( GxTelLevel level )
{
	return level == GX_TEL_OFF ? "off" : level == GX_TEL_FULL ? "full" : "perf";
}

/// This thread's CPU time, ns; -1 when the OS will not say. Not gxGfxCpuNs: that returns 0 on Windows until a GX_GFX probe calibrates it.
long long threadCpuNs( void )
{
#ifdef _WIN32
	FILETIME created, exited, kernel, user;
	if( !GetThreadTimes( GetCurrentThread(), &created, &exited, &kernel, &user ) )
		return -1;
	const unsigned long long k = ( (unsigned long long)kernel.dwHighDateTime << 32 ) | kernel.dwLowDateTime;
	const unsigned long long u = ( (unsigned long long)user.dwHighDateTime << 32 ) | user.dwLowDateTime;
	return (long long)( ( k + u ) * 100 );
#else
	struct timespec ts;
	if( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &ts ) != 0 )
		return -1;
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

unsigned int msSinceStart( long long nowNs )
{
	const long long ms = ( nowNs - s_recStartNs ) / 1000000;
	return ms < 0 ? 0 : ms > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (unsigned int)ms;
}

void putText( const char *key, const char *value )
{
	gxDiagAppendLine( s_text, sizeof( s_text ), &s_textLen, key, value );
}

void putInt( const char *key, int value )
{
	char buf[ 16 ];
	snprintf( buf, sizeof( buf ), "%d", value );
	putText( key, buf );
}

Bool isHeadless( void )
{
	return TheGlobalData != NULL && TheGlobalData->m_headless;
}

void putBuild( void )
{
	// SHA, dirty flag and SimID are in the replay header already; the reader prints them from there.
	putText( "build.tag", GitTag );
	putInt( "build.tick_hz", LOGICFRAMES_PER_SECOND );
	char flags[ 128 ] = "";
#if GX_LOGIC_60HZ	// defined as 0 on a 30 Hz build (WWCommon.h)
	strlcat( flags, "GX_LOGIC_60HZ ", sizeof( flags ) );
#endif
#ifdef GX_TEST_INSTRUMENTS
	strlcat( flags, "GX_TEST_INSTRUMENTS ", sizeof( flags ) );
#endif
#ifdef RTS_DEBUG
	strlcat( flags, "RTS_DEBUG ", sizeof( flags ) );
#endif
	putText( "build.flags", flags );
#if defined( __aarch64__ ) || defined( _M_ARM64 )
	putText( "build.arch", "arm64" );
#elif defined( __x86_64__ ) || defined( _M_X64 )
	putText( "build.arch", "x86_64" );
#else
	putText( "build.arch", "other" );
#endif
}

void putGraphics( void )
{
	putInt( "gfx.headless", isHeadless() ? 1 : 0 );
	if( s_deviceFacts != NULL )
		s_deviceFacts( putText );
	if( TheDisplay != NULL )
	{
		char res[ 32 ];
		snprintf( res, sizeof( res ), "%dx%d", (int)TheDisplay->getWidth(), (int)TheDisplay->getHeight() );
		putText( "gfx.res", res );
		putInt( "gfx.windowed", TheDisplay->getWindowed() ? 1 : 0 );
	}
	if( TheGameEngine != NULL )
		putInt( "gfx.refresh_hz", TheGameEngine->getDisplayRefreshRate() );
	if( TheFramePacer != NULL )
	{
		putInt( "gfx.fps_cap", TheFramePacer->getFramesPerSecondLimit() );
		putInt( "gfx.fps_cap_actual", TheFramePacer->getActualFramesPerSecondLimit() );
	}
	if( TheGameLODManager != NULL )
	{
		putInt( "gfx.lod_static", (int)TheGameLODManager->getStaticLODLevel() );
		putInt( "gfx.lod_dynamic", (int)TheGameLODManager->getDynamicLODLevel() );
	}
	if( TheGlobalData != NULL )
	{
		putInt( "gfx.texture_reduction", TheGlobalData->m_textureReductionFactor );
		putInt( "gfx.max_particles", TheGlobalData->m_maxParticleCount );
		putInt( "gfx.shadow_volumes", TheGlobalData->m_useShadowVolumes ? 1 : 0 );
		putInt( "gfx.shadow_decals", TheGlobalData->m_useShadowDecals ? 1 : 0 );
	}
	// What is IN FORCE, which an Options.ini line alone cannot say: the resolvers fold in what this build and GPU really have.
	putText( "look.upscaler", GXGraphics::upscalerName( GXGraphics::upscaler() ) );
	putText( "look.upscaler_requested", GXGraphics::upscalerName( GXGraphics::requestedUpscaler() ) );
	putInt( "look.render_scale", GXGraphics::renderScalePercent() );
	putInt( "look.sharpness", GXGraphics::upscaleSharpness() );
	putInt( "look.frame_gen", GXGraphics::frameGen() );
	putInt( "look.reflex", GXGraphics::reflex() );
	putInt( "look.remastered", GXGraphics::isRemastered() ? 1 : 0 );
	putInt( "look.shadow_map", GXGraphics::shadowMap() ? 1 : 0 );
	putInt( "look.alpha_to_coverage", GXGraphics::alphaToCoverage() ? 1 : 0 );
	putInt( "render.native_requested", GXGraphics::nativeRendererRequested() ? 1 : 0 );	// the device facts say whether it really started
}

void putOptions( OptionPreferences &prefs )
{
	for( size_t i = 0; i < sizeof( s_graphicsKeys ) / sizeof( s_graphicsKeys[0] ); ++i )
	{
		const OptionPreferences::const_iterator it = prefs.find( s_graphicsKeys[i] );
		if( it != prefs.end() )
		{
			char key[ 64 ];
			snprintf( key, sizeof( key ), "opt.%s", s_graphicsKeys[i] );
			putText( key, gxDiagOptionValue( it->second.str() ) );
		}
	}
}

/// HOW MANY env vars steer the game, never their names or values: non-zero marks a test run or a tuned setup.
void putEnvCount( void )
{
	int count = 0;
#ifdef _WIN32
	char **env = _environ;
#else
	char **env = environ;
#endif
	for( char **e = env; e != NULL && *e != NULL; ++e )
		if( strncmp( *e, "GX_", 3 ) == 0 || strncmp( *e, "DXVK_", 5 ) == 0 || strncmp( *e, "MVK_", 4 ) == 0 )
			++count;
	putInt( "env.gx_vars", count );
}

/// The snapshot's body, without its snap= line, so the end of a match can tell whether anything changed.
void buildBody( OptionPreferences &prefs )
{
	s_textLen = 0;
	putBuild();
	putGraphics();
	putOptions( prefs );
	putEnvCount();
	putText( "telemetry.level", levelWord( s_level ) );	// the level in force since the start, not the file's current line
	putInt( "telemetry.share_ok", s_shareOk );
}

void writeSnapshot( const char *which )
{
	// The name goes last so that it does not take part in the comparison; the reader does not care about line order.
	putText( "snap", which );
	const int total = (int)( ( s_textLen + GX_DIAG_MAX_DATA - 1 ) / GX_DIAG_MAX_DATA );
	int32_t words[ GX_DIAG_MAX_WORDS ];
	for( int s = 0; s < total; ++s )
	{
		const size_t off = (size_t)s * GX_DIAG_MAX_DATA;
		const size_t left = s_textLen - off;
		const int n = gxDiagBuildRecord( words, GX_DIAG_KIND_STATIC, s, total, s_text + off, left < GX_DIAG_MAX_DATA ? left : (size_t)GX_DIAG_MAX_DATA );
		TheRecorder->writeDiagRecord( (const Int *)words, (UnsignedByte)n );
	}
}

GXDiagIter readIter( void )
{
	GXDiagIter it;
	it.nowNs = (long long)gxPerfMonoNs();
	it.logicFrame = TheGameLogic != NULL ? TheGameLogic->getFrame() : 0;
	it.holdTotalNs = TheFramePacer != NULL ? (long long)TheFramePacer->getTotalHoldNs() : 0;
	it.drawnTotal = s_framesDrawn;
	it.drawTotalNs = s_drawTotalNs;
	it.stalling = TheNetwork != NULL && TheNetwork->isStalling();
	it.cushion = -1;
	if( TheNetwork != NULL )
	{
		const int cushion = TheNetwork->getExecutionFrame() - (int)it.logicFrame;
		if( cushion >= 0 )
			it.cushion = cushion;
	}
	it.paused = TheGameLogic != NULL && TheGameLogic->isGamePaused();
	return it;
}

/// What only the engine's globals and the OS can say, on a row GxTelPerf closed (it began at rowStartNs). Every value narrowed; unreadable =
/// UNKNOWN. The sampler's CPU and GPU fields are ms per wall second (its interval is not the row's, so a rate is the only honest unit);
/// cpu_main keeps phase 1's meaning, main-thread CPU ms IN the row, which the reader divides by the row's length.
void fillRow( GxTelPerfRow &r, const GXDiagIter &it, long long rowStartNs )
{
	if( TheNetwork != NULL )
	{
		r.netRate = gxTelNarrowU8( (long long)TheNetwork->getFrameRate() );
		if( s_netBase )
		{
			// unsigned deltas: a counter that wrapped still reads right. TheGxNetRetries is a 2000 ms blind-repair counter, not lost packets.
			r.netRetries = gxTelNarrowU16( (long long)(UnsignedInt)( TheGxNetRetries - s_lastRetries ) );
			r.netRecvDrop = gxTelNarrowU16( (long long)(UnsignedInt)( TheGxNetRecvDrops - s_lastRecvDrops ) );
		}
		s_lastRetries = TheGxNetRetries;
		s_lastRecvDrops = TheGxNetRecvDrops;
		s_netBase = true;
	}
	else
		s_netBase = false;
	const long long cpu = threadCpuNs();
	if( cpu >= 0 && s_lastCpuMainNs >= 0 && cpu >= s_lastCpuMainNs )
		r.cpuMainMs = gxTelNarrowU16( ( cpu - s_lastCpuMainNs ) / 1000000 );
	s_lastCpuMainNs = cpu;
	if( s_osReader != NULL )
	{
		GxTelOsSnapshot os;
		const bool fresh = s_osReader( &os ) && gxTelOsUsable( os, s_osHaveSeq, s_osLastSeq, it.nowNs, rowStartNs, s_osNotBeforeNs );
		if( fresh )
		{
			s_osLastSeq = os.seq;
			s_osHaveSeq = true;
			r.cpuProcMs = os.cpuProcMs;
			r.cpuRenderMs = os.cpuRenderMs;
			r.cpuAudioMs = os.cpuAudioMs;
			r.cpuPcoreMs = os.cpuPcoreMs;
			r.sysCpuX10 = os.sysCpuX10;
			r.gpuProcMs = os.gpuProcMs;
			r.sysGpuPct = os.sysGpuPct;
			r.rss16M = os.rssBytes == UINT64_MAX ? GX_TEL_U16_UNKNOWN : gxTelNarrowU16( (long long)( os.rssBytes >> 24 ) );
			r.sysAvailMb16M = os.sysAvailBytes == UINT64_MAX ? GX_TEL_U16_UNKNOWN : gxTelNarrowU16( (long long)( os.sysAvailBytes >> 24 ) );
		}
		else
			r.flags |= GX_TEL_F_SAMPLER_LATE;
	}
}

/// The open PERF row closes at `it`; the next one starts there. telFlags: GX_TEL_F_SUSPENDED.
void closeRow( const GXDiagIter &it, int telFlags )
{
	GxTelPerfRow r;
	const long long rowStartNs = s_tel.rowStartNs;	// before the close moves it to the next row
	gxTelPerfCloseRow( s_tel, it, telFlags, isHeadless(), r );
	fillRow( r, it, rowStartNs );
	uint8_t bytes[ GX_TEL_PERF_ROW_BYTES ];
	gxTelPackPerfRow( r, bytes );
	gxTelBatchAppend( s_batch, bytes );	// a window holds at most 10 rows (anchored seconds) and a record 17
}

/// A new window, and its batch, at `it`. dropTicks at a restart (the load is no row's).
void startTelWindow( const GXDiagIter &it, bool dropTicks )
{
	gxTelPerfStartWindow( s_tel, it, dropTicks );
	gxTelBatchBegin( s_batch, GX_TEL_PERF_ROW_BYTES, it.logicFrame, msSinceStart( it.nowNs ), 0, 0 );
	if( dropTicks )
	{
		s_netBase = TheNetwork != NULL;
		s_lastRetries = TheGxNetRetries;
		s_lastRecvDrops = TheGxNetRecvDrops;
		s_lastCpuMainNs = threadCpuNs();
		// the load these restarts drop is OS work too: no snapshot that began before now fills a row; an ordinary
		// ten-second window boundary (dropTicks false) keeps the barrier where it was
		s_osNotBeforeNs = (long long)gxPerfMonoNs();
	}
}

/// The window's PERF batch, beside its WINDOW row. The slow-I/O rule: a write over 20 ms skips the next 60 s of batches; the first one
/// written after says how many seconds are missing, and the recording's last one (`last`) is always written for that. The write itself
/// is synchronous, as the recorder's own command writes are.
void writeBatch( const GXDiagIter &it, bool last )
{
	if( s_batch.rows == 0 )
		return;
	gxTelBatchAppendHist( s_batch, s_tel.batchLog );
	if( !gxTelSlowIoWrites( s_slowIo, it.nowNs, last ) )
	{
		gxTelSlowIoDropped( s_slowIo, it.nowNs - s_tel.anchorNs );
		return;
	}
	const unsigned dropped = gxTelSlowIoTakeDroppedSeconds( s_slowIo );
	gxTelBatchSetDropped( s_batch, dropped, 0 );
	if( dropped != 0 )
		s_batch.data[ GX_TEL_BATCH_HEADER + 55 ] |= GX_TEL_F_DROPPED;	// the first row's flags byte (offset 55)
	int32_t words[ GX_DIAG_MAX_WORDS ];
	const int n = gxDiagBuildRecord( words, GX_DIAG_KIND_PERF, 0, 1, s_batch.data, s_batch.len );
	const long long t0 = (long long)gxPerfMonoNs();
#ifdef GX_TEST_INSTRUMENTS
	if( s_testSlowWriteMs > 0 )
	{
#ifdef _WIN32
		Sleep( (DWORD)s_testSlowWriteMs );
#else
		usleep( (useconds_t)s_testSlowWriteMs * 1000 );
#endif
	}
#endif
	TheRecorder->writeDiagRecord( (const Int *)words, (UnsignedByte)n );
	const long long t1 = (long long)gxPerfMonoNs();
	gxTelSlowIoWrote( s_slowIo, t1, t1 - t0 );
	if( s_writeTiming )
		fprintf( stderr, "[GXTEL] write_us=%lld kind=3 rows=%d bytes=%d frame=%u dropped_s=%u worst_us=%lld\n", ( t1 - t0 ) / 1000, (int)s_batch.rows,
			(int)( 15 + n * 4 ), it.logicFrame, dropped, s_slowIo.worstWriteNs / 1000 );
}

void closeWindow( const GXDiagIter &it, int extraFlags )
{
	closeRow( it, ( extraFlags & GX_DIAG_FLAG_SUSPENDED ) ? GX_TEL_F_SUSPENDED : 0 );
	long long simNs = 0, tickMaxNs = 0;
	gxTelPerfTakeWindowTicks( s_tel, simNs, tickMaxNs );	// every drain since the window began: its rows' sums
	if( s_rows >= GX_DIAG_MAX_ROWS )
		return;
	int cap = TheFramePacer != NULL ? TheFramePacer->getActualFramesPerSecondLimit() : -1;
	if( cap >= (int)RenderFpsPreset::UncappedFpsValue )
		cap = 0;
	const int netRate = TheNetwork != NULL ? (int)TheNetwork->getFrameRate() : -1;
	int32_t row[ GX_DIAG_ROW_FIELDS ];
	gxDiagWindowRow( s_window, it, simNs, tickMaxNs, cap, netRate, extraFlags | ( isHeadless() ? GX_DIAG_FLAG_HEADLESS : 0 ), row );
	row[ GX_DIAG_ROW_DRAW_MS ] = isHeadless() ? -1 : (int32_t)( s_drawNs / 1000000 );
	row[ GX_DIAG_ROW_DRAW_MAX_MS ] = isHeadless() ? -1 : (int32_t)( s_drawMaxNs / 1000000 );
	s_drawNs = 0;
	s_drawMaxNs = 0;
	int32_t words[ GX_DIAG_MAX_WORDS ];
	const int n = gxDiagBuildRecord( words, GX_DIAG_KIND_WINDOW, s_rows, 0, row, sizeof( row ) );
	TheRecorder->writeDiagRecord( (const Int *)words, (UnsignedByte)n );
	writeBatch( it, ( extraFlags & GX_DIAG_FLAG_MATCH_END ) != 0 );
	++s_rows;
}

} // namespace

void gxReplayDiagFrameDrawn( void )
{
	++s_framesDrawn;
	if( s_window.primed )
		gxTelPerfFrameDrawn( s_tel, (long long)gxPerfMonoNs() );
}

void gxReplayDiagDrawSpan( long long ns )
{
	s_drawNs += ns;
	s_drawTotalNs += ns;
	if( ns > s_drawMaxNs )
		s_drawMaxNs = ns;
}

// The hardware report's facts (GameClient/GXHwReport.h): the device layer's, its extras, the display and the build. Linux's distro
// stands in for the bare kernel name and number, which say less about the machine.
static GXReplayDiagDeviceFn s_hwExtras = NULL;
static GXHwPutFn s_hwPut = NULL;

static void hwPut( const char *key, const char *value )
{
	if( strcmp( key, "os.distro" ) == 0 )
	{
		char os[ 64 ];
		snprintf( os, sizeof( os ), "Linux %s", value );
		s_hwPut( "os.name", os );
	}
	else if( strcmp( key, "os.distro_version" ) == 0 )
		s_hwPut( "os.version", value );
	else
		s_hwPut( key, value );
}

static void gxReplayDiagHwFacts( GXHwPutFn put )
{
	s_hwPut = put;
	if( s_deviceFacts != NULL )
		s_deviceFacts( hwPut );
	if( s_hwExtras != NULL )
		s_hwExtras( hwPut );
	char buf[ 32 ];
	if( TheDisplay != NULL )
	{
		snprintf( buf, sizeof( buf ), "%dx%d", (int)TheDisplay->getWidth(), (int)TheDisplay->getHeight() );
		put( "gfx.res", buf );
		put( "gfx.windowed", TheDisplay->getWindowed() ? "1" : "0" );
	}
	if( TheGameEngine != NULL )
	{
		snprintf( buf, sizeof( buf ), "%d", TheGameEngine->getDisplayRefreshRate() );
		put( "gfx.refresh_hz", buf );
	}
	put( "gx.version", GitTag[ 0 ] != '\0' ? GitTag : GitShortSHA1 );
}

void gxReplayDiagSetDeviceFacts( GXReplayDiagDeviceFn fn )
{
	s_deviceFacts = fn;
	gxHwSetFactsProvider( fn != NULL ? gxReplayDiagHwFacts : NULL );
}

void gxReplayDiagSetHwExtras( GXReplayDiagDeviceFn fn )
{
	s_hwExtras = fn;
}

void gxReplayDiagRecordingStarted( void )
{
	if( TheRecorder == NULL || !TheRecorder->isWritingFile() )
		return;
	s_window.primed = false;
	s_rows = 0;
	s_suspended = false;
	OptionPreferences prefs;	// one read of Options.ini: the level, the consent flag and the snapshot's settings
	s_level = readLevel( prefs );
	s_shareOk = readShareOk( prefs );
#ifdef GX_TEST_INSTRUMENTS
	const char *red = getenv( "GX_TEL_TEST_RED_GATE" );
	s_testRedGate = red != NULL && strcmp( red, "1" ) == 0;
#endif
	if( s_level == GX_TEL_OFF )
		return;	// off means off: not even the settings snapshot
	s_recStartNs = (long long)gxPerfMonoNs();
	gxTelPerfInit( s_tel, takeTicks, s_recStartNs );
	gxTelPerfAway( s_tel, s_away );
	gxTelSlowIoInit( s_slowIo );
	const char *timing = getenv( "GX_TEL_WRITE_TIMING" );
	s_writeTiming = timing != NULL && strcmp( timing, "1" ) == 0;
	s_testSlowWriteMs = 0;
#ifdef GX_TEST_INSTRUMENTS
	const char *slow = getenv( "GX_TEL_TEST_SLOW_WRITE_MS" );
	s_testSlowWriteMs = slow != NULL ? atoi( slow ) : 0;
	// task 6's red arm: GX_TEL_TEST_SPIN_MS of busy work per iteration, from GX_TEL_TEST_SPIN_FROM_S for GX_TEL_TEST_SPIN_FOR_S seconds of the
	// recording - the same load as the external burner, but the game's own, so cpu_main / cpu_proc must carry it
	const char *spin = getenv( "GX_TEL_TEST_SPIN_MS" ), *spinFrom = getenv( "GX_TEL_TEST_SPIN_FROM_S" ), *spinFor = getenv( "GX_TEL_TEST_SPIN_FOR_S" );
	s_testSpinMs = spin != NULL ? atoi( spin ) : 0;
	s_testSpinFromS = spinFrom != NULL ? atoi( spinFrom ) : 0;
	s_testSpinForS = spinFor != NULL ? atoi( spinFor ) : 0;
#endif
	if( s_samplerControl != NULL )
		s_samplerControl( 1 );	// after the off gate: off starts no thread either
	buildBody( prefs );
	s_startCrc = gxDiagCrc32( s_text, s_textLen );
	writeSnapshot( "start" );
}

void gxReplayDiagRecordingEnding( void )
{
	if( s_samplerControl != NULL )
		s_samplerControl( 0 );	// a flag store: the sampler idles at its next wake, nothing here waits for it
	if( TheRecorder == NULL || !TheRecorder->isWritingFile() || s_level == GX_TEL_OFF )
		return;
	if( gxTelWindowToClose( s_window ) )
	{
		// The time since the last sample belongs to this window. On the clear path this runs inside GameLogic::update, so the tick in
		// progress cannot be in sim_ms; the row's wall time and other_ms hold it instead.
		const GXDiagIter it = readIter();
		gxTelPerfIter( s_tel, it );
		gxDiagWindowIter( s_window, it );
		closeWindow( it, GX_DIAG_FLAG_MATCH_END );
	}
	s_window.primed = false;
	OptionPreferences prefs;
	buildBody( prefs );
	if( gxDiagCrc32( s_text, s_textLen ) != s_startCrc )
		writeSnapshot( "end" );
	if( s_writeTiming )
		fprintf( stderr, "[GXTEL] end worst_write_us=%lld\n", s_slowIo.worstWriteNs / 1000 );
}

void gxReplayDiagSample( void )
{
	// Off gates the sampler too, not only the two entry points: it primes and closes windows on its own.
	if( TheRecorder == NULL || !TheRecorder->isWritingFile() || ( s_level == GX_TEL_OFF && !s_testRedGate ) )
	{
		s_window.primed = false;
		return;
	}
#ifdef GX_TEST_INSTRUMENTS
	if( s_testSpinMs > 0 && s_window.primed )
	{
		const long long sinceS = ( (long long)gxPerfMonoNs() - s_recStartNs ) / 1000000000LL;
		if( sinceS >= s_testSpinFromS && sinceS < (long long)s_testSpinFromS + s_testSpinForS )
		{
			const long long until = (long long)gxPerfMonoNs() + (long long)s_testSpinMs * 1000000LL;
			volatile unsigned int sink = 0;
			while( (long long)gxPerfMonoNs() < until )
				sink = sink * 1664525u + 1013904223u;
		}
	}
#endif
	const GXDiagIter it = readIter();
	// The load, its progress barrier and its fade are one enormous iteration; frame 0 is not a hitch (GXPerfReport.h).
	if( gxPerfRestartWindow( s_window.primed, s_window.lastFrame ) )
	{
		gxDiagWindowStart( s_window, it );
		startTelWindow( it, true );
		s_drawNs = 0;
		s_drawMaxNs = 0;
		return;
	}
	const bool rowDue = gxTelPerfIter( s_tel, it );
	if( gxDiagWindowIter( s_window, it ) )
	{
		closeWindow( it, 0 );
		gxDiagWindowStart( s_window, it );
		startTelWindow( it, false );
	}
	else if( rowDue )
		closeRow( it, 0 );
}

GxTelLevel gxReplayTelemetryLevel( void )
{
	return s_level;
}

void gxReplayTelSuspend( bool suspended )
{
	if( suspended == s_suspended )
		return;
	s_suspended = suspended;
	if( !suspended )
	{
		s_osNotBeforeNs = (long long)gxPerfMonoNs();	// the sampler kept counting through the stop: its samples before now span it
		return;	// the next sample restarts the window: the stopped seconds are a gap, not a row
	}
	gxTelPerfSuspend( s_tel );
	if( TheRecorder == NULL || !TheRecorder->isWritingFile() || s_level == GX_TEL_OFF || !gxTelWindowToClose( s_window ) )
	{
		s_window.primed = false;
		return;
	}
	// What ran since the last sample (this iteration's ticks and draw) belongs to the row that ends here.
	const GXDiagIter it = readIter();
	gxTelPerfIter( s_tel, it );
	gxDiagWindowIter( s_window, it );
	closeWindow( it, GX_DIAG_FLAG_SUSPENDED );
	s_window.primed = false;
}

void gxReplayTelAway( bool away )
{
	if( away == s_away )
		return;
	s_away = away;
	gxTelPerfAway( s_tel, away );	// the open row (if any) carries it; s_tel is re-initialised before any later recording writes
}

void gxReplayTelSetOsReader( GXReplayTelOsFn fn )
{
	s_osReader = fn;
}

void gxReplayTelSetSamplerControl( GXReplayTelControlFn fn )
{
	s_samplerControl = fn;
}
