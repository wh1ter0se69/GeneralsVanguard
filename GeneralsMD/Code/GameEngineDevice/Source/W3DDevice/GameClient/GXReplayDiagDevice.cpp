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

// FILE: GXReplayDiagDevice.cpp ///////////////////////////////////////////////////////////////////
//
// What only the device layer knows, for the replay diagnostics block (GameClient/GXReplayDiag.h).
//
// Every native call the block needs lives here (platform code stays in the device layer). Of D3DADAPTER_IDENTIFIER8
// only the model and the driver version leave: DeviceIdentifier is a GUID, SubSysId names the board, and Driver can be a path on
// some stacks. None is needed to reproduce a report, and each says more about one machine than a replay strangers read should.
// os.* and cpu.* are model strings; uname()'s nodename is the host name and is never touched.

#include "W3DDevice/GameClient/GXReplayDiagDevice.h"

#include "GameClient/GXReplayDiag.h"
#include "GameClient/GXReplayDiagFormat.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#include "W3DDevice/GameClient/GxNativeInstall.h"
#include "W3DDevice/GameClient/GxDeviceSummary.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#endif

/// A hardware model string, screened: if it holds this machine's user or host name it reads "other". The names are only compared, never kept.
static void putModel( GXReplayDiagPutFn put, const char *key, const char *value )
{
	const char *const user = getenv( "USER" ), *const userWin = getenv( "USERNAME" );
	char host[ 256 ] = "";
#ifdef _WIN32
	const char *const computer = getenv( "COMPUTERNAME" );
	if( computer != NULL )
		snprintf( host, sizeof( host ), "%s", computer );
#else
	if( gethostname( host, sizeof( host ) ) != 0 )
		host[ 0 ] = 0;
	host[ sizeof( host ) - 1 ] = 0;
	if( char *dot = strchr( host, '.' ) )	// the short name is the one a person would have typed
		*dot = 0;
#endif
	const bool named = gxDiagContainsName( value, user ) || gxDiagContainsName( value, userWin ) || gxDiagContainsName( value, host );
	put( key, named ? "other" : value );
}

static void putNumber( GXReplayDiagPutFn put, const char *key, int value )
{
	char buf[ 16 ];
	snprintf( buf, sizeof( buf ), "%d", value );
	put( key, buf );
}

static void putMachine( GXReplayDiagPutFn put )
{
	char buf[ 128 ] = "";
#ifdef _WIN32
	put( "os.name", "Windows" );
	// RtlGetVersion: GetVersionEx lies to an unmanifested process.
	typedef LONG (WINAPI *RtlGetVersionFn)( OSVERSIONINFOW * );
	HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
	RtlGetVersionFn getVersion = ntdll != NULL ? (RtlGetVersionFn)GetProcAddress( ntdll, "RtlGetVersion" ) : NULL;
	OSVERSIONINFOW vi;
	memset( &vi, 0, sizeof( vi ) );
	vi.dwOSVersionInfoSize = sizeof( vi );
	if( getVersion != NULL && getVersion( &vi ) == 0 )
	{
		snprintf( buf, sizeof( buf ), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber );
		put( "os.version", buf );
	}
	DWORD size = sizeof( buf );
	if( RegGetValueA( HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
		RRF_RT_REG_SZ, NULL, buf, &size ) == ERROR_SUCCESS )
		putModel( put, "cpu.model", buf );
	MEMORYSTATUSEX mem;
	mem.dwLength = sizeof( mem );
	if( GlobalMemoryStatusEx( &mem ) )
		putNumber( put, "ram.gb", (int)( ( mem.ullTotalPhys + 512ull * 1024 * 1024 ) / ( 1024ull * 1024 * 1024 ) ) );
#else
#if defined( __APPLE__ )
	#if TARGET_OS_IPHONE
	put( "os.name", "iOS" );
	const char *const cpuKey = "hw.machine";	// iOS has no brand string; "iPad14,3" is a model, not a serial
	#else
	put( "os.name", "macOS" );
	const char *const cpuKey = "machdep.cpu.brand_string";
	#endif
	size_t size = sizeof( buf );
	if( sysctlbyname( "kern.osproductversion", buf, &size, NULL, 0 ) == 0 )
		put( "os.version", buf );
	size = sizeof( buf );
	if( sysctlbyname( cpuKey, buf, &size, NULL, 0 ) == 0 )
		putModel( put, "cpu.model", buf );
#else
	struct utsname u;
	if( uname( &u ) == 0 )	// .release only, and only its number: .nodename is the host name, and a custom kernel's suffix can be anything
	{
		gxDiagLeadingVersion( buf, sizeof( buf ), u.release );
		put( "os.version", buf );
	}
	put( "os.name", "Linux" );
	char line[ 256 ];
	if( FILE *f = fopen( "/etc/os-release", "r" ) )
	{
		while( fgets( line, sizeof( line ), f ) != NULL )
		{
			// ID and VERSION_ID, never PRETTY_NAME: this file is user-editable, so only a known name and a number get out of it.
			const Bool isId = strncmp( line, "ID=", 3 ) == 0, isVersion = strncmp( line, "VERSION_ID=", 11 ) == 0;
			if( !isId && !isVersion )
				continue;
			char *value = line + ( isId ? 3 : 11 );
			value += strspn( value, "\"" );
			value[ strcspn( value, "\"\r\n" ) ] = 0;
			if( isId )
				put( "os.distro", gxDiagDistroName( value ) );
			else
			{
				gxDiagLeadingVersion( buf, sizeof( buf ), value );
				put( "os.distro_version", buf );
			}
		}
		fclose( f );
	}
	if( FILE *f = fopen( "/proc/cpuinfo", "r" ) )
	{
		while( fgets( line, sizeof( line ), f ) != NULL )
		{
			if( strncmp( line, "model name", 10 ) == 0 && strchr( line, ':' ) != NULL )
			{
				putModel( put, "cpu.model", strchr( line, ':' ) + 2 );
				break;
			}
		}
		fclose( f );
	}
#endif
	const long long pages = sysconf( _SC_PHYS_PAGES ), pageSize = sysconf( _SC_PAGESIZE );
	if( pages > 0 && pageSize > 0 )
		putNumber( put, "ram.gb", (int)( ( pages * pageSize + 512ll * 1024 * 1024 ) / ( 1024ll * 1024 * 1024 ) ) );
#endif
	putNumber( put, "cpu.logical", (int)std::thread::hardware_concurrency() );
}

static void gxReplayDiagDeviceFacts( GXReplayDiagPutFn put )
{
	putMachine( put );

	// A headless peer never creates a device, so it has no renderer to describe.
	IDirect3D8 *d3d = DX8Wrapper::_Get_D3D8();
	if( d3d == nullptr )
	{
		put( "render.backend", "none" );
		return;
	}

#if defined( _WIN32 )
	// Windows loads whichever d3d8.dll comes first: DXVK's beside the exe, or its own. Only the answer leaves here, never the path.
	char dll[ MAX_PATH ] = "";
	HMODULE d3d8 = GetModuleHandleA( "d3d8.dll" );
	if( d3d8 == NULL || GetModuleFileNameA( d3d8, dll, MAX_PATH ) == 0 )
		put( "render.backend", "d3d8-unknown" );
	else
	{
		for( char *c = dll; *c != 0; ++c )
			*c = (char)tolower( (unsigned char)*c );
		put( "render.backend", ( strstr( dll, "\\system32\\" ) != NULL || strstr( dll, "\\syswow64\\" ) != NULL ) ? "d3d8-native" : "d3d8-dxvk" );
	}
#elif defined( SAGE_USE_MOLTENVK )
	put( "render.backend", "d3d8-dxvk-moltenvk" );
#else
	put( "render.backend", "d3d8-dxvk" );
#endif

	char buf[ 64 ];
	put( "gfx.borderless", DX8Wrapper::Is_Borderless() ? "1" : "0" );
	put( "gfx.vsync_windowed", DX8Wrapper::Get_Windowed_VSync() ? "1" : "0" );
	const unsigned int interval = DX8Wrapper::Get_FullScreen_Present_Interval();
	put( "gfx.present_fullscreen", interval == D3DPRESENT_INTERVAL_IMMEDIATE ? "immediate" : interval == D3DPRESENT_INTERVAL_ONE ? "one" : interval == D3DPRESENT_INTERVAL_TWO ? "two" : "default" );

	put( "render.native_active", GxNative::SessionLive() ? "1" : "0" );

	const int adapter = WW3D::Get_Render_Device();	// after the check above: its getter asserts that WW3D was initted
	if( adapter < 0 )
		return;
	D3DADAPTER_IDENTIFIER8 id;
	memset( &id, 0, sizeof( id ) );
	if( FAILED( d3d->GetAdapterIdentifier( adapter, D3DENUM_NO_WHQL_LEVEL, &id ) ) )
		return;
	putModel( put, "gpu.name", id.Description );
	snprintf( buf, sizeof( buf ), "%04X", (unsigned)id.VendorId );
	put( "gpu.vendor", buf );
	snprintf( buf, sizeof( buf ), "%04X", (unsigned)id.DeviceId );
	put( "gpu.device", buf );
	// DXVK answers every adapter with one fixed number, 0x7FFFFFFFFFFFFFFF, which reads like a version and is none (measured: 32767.65535.65535.65535).
	if( id.DriverVersion.HighPart == 0x7FFFFFFF && id.DriverVersion.LowPart == 0xFFFFFFFF )
		put( "gpu.driver", "unknown" );
	else
	{
		snprintf( buf, sizeof( buf ), "%u.%u.%u-%u", (unsigned)( id.DriverVersion.HighPart >> 16 ), (unsigned)( id.DriverVersion.HighPart & 0xFFFF ),
			(unsigned)( id.DriverVersion.LowPart >> 16 ), (unsigned)( id.DriverVersion.LowPart & 0xFFFF ) );
		put( "gpu.driver", buf );
	}
}

/// The hardware report's extras (GameClient/GXHwReport.h), not written to replays: physical cores, and the Vulkan device's driver,
/// device-local memory and API version, which D3D8's adapter identifier cannot give through DXVK.
static void gxReplayDiagHwExtras( GXReplayDiagPutFn put )
{
	const int cores = GxPhysicalCores();
	if( cores > 0 )
		putNumber( put, "cpu.cores", cores );
	if( DX8Wrapper::_Get_D3D8() == nullptr )
		return;
	GxDeviceSummary s;
	if( !GxDeviceSummaryRead( DX8Wrapper::_Get_D3D_Device8(), &s ) )
		return;
	if( s.driver[ 0 ] != 0 )
		put( "gpu.driver_info", s.driver );
	if( s.vramMb > 0 )
		putNumber( put, "gpu.vram_mb", (int)s.vramMb );
	if( s.apiVersion != 0 )
	{
		char vk[ 24 ];
		snprintf( vk, sizeof( vk ), "%u.%u.%u", ( s.apiVersion >> 22 ) & 0x7Fu, ( s.apiVersion >> 12 ) & 0x3FFu, s.apiVersion & 0xFFFu );
		put( "gpu.vk", vk );
	}
}

// GX-TEL-SAMPLER-BEGIN
// Telemetry phase 2: the game's CPU and memory beside the machine's, for the PERF row (GXReplayDiagFormat.h kind 3).
// No GPU is read, so cpu_render / cpu_audio / gpu_proc / sys_gpu are always UNKNOWN. Privacy allow-list: OS APIs
// only, totals only, never a serial, UUID, MAC, host or user name; defines no macro (a test greps this span).
// The sampler thread is detached and never joined (a join could wait on an in-flight OS query); its shared block is
// heap-allocated and never freed. Normal priority, so it keeps sampling when the machine is saturated.
// The main thread try_locks and never waits.
#include "GameClient/GXPerfReport.h"	// gxPerfMonoNs: the rows' clock, so a snapshot's interval compares with theirs

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <thread>

#if defined( _WIN32 )
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#if defined( __APPLE__ )
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread/qos.h>
#if !TARGET_OS_IPHONE
#include <libproc.h>
#endif
#endif

namespace gxtel {

// GX-TEL-PURE-BEGIN
// Nothing between here and GX-TEL-PURE-END calls the OS: scripts/test/replay-tel-sampler-test.py compiles it alone and tests it.

/// One CUMULATIVE sample. Every long long is -1 when not readable at this sample; a row is the difference of two samples.
struct Raw
{
	uint32_t seq, gen;		///< gen: which recording's sampler run made it (a row never subtracts across two)
	long long beginNs;		///< gxPerfMonoNs when the sampler began this pass, before it read any counter
	long long tNs;			///< gxPerfMonoNs right after the process CPU counters were read (they are the pass's last reads)
	long long procNs;		///< process user + sys
	long long pcoreNs;		///< time on performance cores
	long long sysBusy, sysTotal;	///< whole machine, in one unit of the OS's choosing
	uint32_t sysEpoch;		///< how many reads so far found a machine counter LOWER than the read before (Linux /proc/stat's iowait
					///< may fall, proc(5)): no delta spans one (osDelta)
	uint64_t rss, avail;		///< bytes at the sample; UINT64_MAX = unknown
};

inline void rawUnknown( Raw &r )
{
	r.seq = r.gen = 0;
	r.sysEpoch = 0;
	r.beginNs = r.tNs = r.procNs = r.pcoreNs = r.sysBusy = r.sysTotal = -1;
	r.rss = r.avail = UINT64_MAX;
}

/// ms per wall second of a counter in ns between two samples; -1 when either end is unknown, the counter went down or no time passed.
inline long long perSecond( long long a, long long b, long long dtNs )
{
	if( a < 0 || b < 0 || b < a || dtNs <= 0 )
		return -1;
#ifdef GX_TEL_RED_NO_NORMALISE
	return ( b - a ) / 1000000;	// fail arm: the raw delta, not per second
#else
	return ( ( b - a ) * 1000 + dtNs / 2 ) / dtNs;
#endif
}

/// The row's OS fields from the sample `a` the previous row used to the newest one `b`, narrowed (GXReplayDiagFormat.h rules: too large
/// is censored, unknown is all ones, never a truncated or invented 0).
inline void osDelta( const Raw &a, const Raw &b, GxTelOsSnapshot &out )
{
	const long long dt = b.tNs - a.tNs;
	out.seq = b.seq;
	out.startNs = a.tNs;
	out.endNs = b.tNs;
	out.beginNs = a.beginNs >= 0 && a.beginNs < a.tNs ? a.beginNs : a.tNs;
	out.cpuProcMs = gxTelNarrowU16( perSecond( a.procNs, b.procNs, dt ) );
	// Per-thread class CPU is cut to a phase 2b: no thread is walked, so render and audio are always UNKNOWN
	out.cpuRenderMs = GX_TEL_U16_UNKNOWN;
	out.cpuAudioMs = GX_TEL_U16_UNKNOWN;
	out.cpuPcoreMs = gxTelNarrowU16( perSecond( a.pcoreNs, b.pcoreNs, dt ) );
	// No GPU reader on any platform in phase 2: both GPU fields are always UNKNOWN
	out.gpuProcMs = GX_TEL_U16_UNKNOWN;
	out.sysGpuPct = GX_TEL_U8_UNKNOWN;
	long long x10 = -1;
	if( dt > 0 && a.sysEpoch == b.sysEpoch && a.sysBusy >= 0 && a.sysTotal >= 0 && b.sysBusy >= a.sysBusy && b.sysTotal > a.sysTotal )
	{
		x10 = ( ( b.sysBusy - a.sysBusy ) * 1000 + ( b.sysTotal - a.sysTotal ) / 2 ) / ( b.sysTotal - a.sysTotal );
		if( x10 > 1000 )
			x10 = 1000;
	}
	out.sysCpuX10 = gxTelNarrowU16( x10 );
	out.rssBytes = b.rss;
	out.sysAvailBytes = b.avail;
}

/// The machine CPU counters carried from one read of /proc/stat's all-cores line to the next (Linux; the sampler thread's own).
struct ProcStatCpu
{
	enum { FIELDS = 16 };	///< today's kernels report 10 (guest and guest_nice last); a line with more is refused, never half-watched
	long long v[ FIELDS ];
	int n;			///< fields in the previous read; 0 = none yet
	uint32_t breaks;	///< reads at which some field was LOWER than at the read before: Raw::sysEpoch
};

/// /proc/stat's all-cores line into the sample: total = the first eight fields, iowait INCLUDED (it is elapsed time the CPUs spent idle,
/// waiting), busy = total - idle - iowait. A field may go DOWN between two reads (proc(5): iowait is not reliable); a delta across that
/// would read the fall as busy time, and leaving iowait out of total instead counts a waiting second as busy
/// - so a read with any field below the previous read's breaks the machine CPU's continuity and no delta spans it.
/// EVERY field the line reports is watched, guest and guest_nice included; only the first eight are summed, since guest time is already
/// inside user / nice.
inline bool procStatSample( const char *line, ProcStatCpu &st, Raw &r )
{
	if( strncmp( line, "cpu ", 4 ) != 0 )
		return false;
	long long v[ ProcStatCpu::FIELDS ];
	int n = 0;
	const char *p = line + 4;
	for( ;; )
	{
		while( *p == ' ' )
			++p;
		if( *p < '0' || *p > '9' )
			break;
		if( n == ProcStatCpu::FIELDS )
			return false;	// more fields than it can watch: one it cannot see could fall
		long long x = 0;
		for( ; *p >= '0' && *p <= '9'; ++p )
			x = x * 10 + ( *p - '0' );
		v[ n++ ] = x;
	}
	if( n < 4 )
		return false;
#ifndef GX_TEL_RED_EIGHT_ONLY	// fail arm: only the summed fields are watched (7482ff4d75: a fall in guest alone went unseen)
	const int watched = n;
#else
	const int watched = n < 8 ? n : 8;
#endif
#ifndef GX_TEL_RED_FIELD_DOWN	// fail arm: a field that went down is taken as it is (5e1fdeb678: a fall in iowait read as busy time)
	bool down = st.n != 0 && st.n != n;
	for( int i = 0; i < watched && st.n == n; ++i )
		down = down || v[ i ] < st.v[ i ];
	if( down )
		++st.breaks;
#else
	(void)watched;
#endif
	for( int i = 0; i < n; ++i )
		st.v[ i ] = v[ i ];
	st.n = n;
	long long total = 0;
	for( int i = 0; i < n && i < 8; ++i )
		total += v[ i ];
	const long long wait = n > 4 ? v[ 4 ] : 0;
#ifndef GX_TEL_RED_IOWAIT_OUT	// fail arm: iowait left out of total too (6fc2deeba8: 50 busy + 50 iowait ticks read 100 %)
	r.sysTotal = total;
#else
	r.sysTotal = total - wait;
#endif
	r.sysBusy = total - v[ 3 ] - wait;
	r.sysEpoch = st.breaks;
	return true;
}

/// A kB value from /proc/meminfo, e.g. key "MemAvailable:"; -1 when absent.
inline long long meminfoKb( const char *text, const char *key )
{
	const char *p = strstr( text, key );
	if( p == NULL || ( p != text && p[ -1 ] != '\n' ) )
		return -1;
	p += strlen( key );
	while( *p == ' ' || *p == '\t' )
		++p;
	if( *p < '0' || *p > '9' )
		return -1;
	long long x = 0;
	for( ; *p >= '0' && *p <= '9'; ++p )
		x = x * 10 + ( *p - '0' );
	return x;
}

/// Which sample the row closing now subtracts from: 2 = the one the previous row ended on, 1 = the sampler's own previous sample (a new
/// recording's first row), 0 = none - nothing new since the last row, or no sample of the ACTIVE recording's run. A query in flight across
/// a restart publishes the ended run's gen and is never used, so no delta spans two recordings.
inline int pickBase( const Raw &cur, const Raw &prev, bool havePrev, const Raw &last, bool haveLast, uint32_t activeGen )
{
#ifndef GX_TEL_RED_ANY_GEN	// fail arm: a sample of an ended run is used
	if( cur.gen != activeGen )
		return 0;
#else
	(void)activeGen;
#endif
	if( haveLast && last.gen == cur.gen )
		return last.seq < cur.seq ? 2 : 0;
	return havePrev && prev.gen == cur.gen && prev.seq < cur.seq ? 1 : 0;
}
// GX-TEL-PURE-END

// ---- The sampler thread's own state: touched by that thread only, allocated once, never freed (the thread never ends).
struct Sampler
{
#if defined( __APPLE__ )
	mach_port_t host;
	mach_timebase_info_data_t timebase;
	vm_size_t pageSize;
#elif defined( __linux__ )
	long pageSize;
	ProcStatCpu stat;
#endif
};

#if defined( __APPLE__ )
static long long timevalNs( const struct timeval &t )
{
	return (long long)t.tv_sec * 1000000000LL + (long long)t.tv_usec * 1000LL;
}

static void sampleOs( Sampler &s, Raw &r, long long now )
{
	(void)now;
	host_cpu_load_info_data_t load;
	mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
	if( host_statistics( s.host, HOST_CPU_LOAD_INFO, (host_info_t)&load, &n ) == KERN_SUCCESS )
	{
		r.sysBusy = (long long)load.cpu_ticks[ CPU_STATE_USER ] + load.cpu_ticks[ CPU_STATE_SYSTEM ] + load.cpu_ticks[ CPU_STATE_NICE ];
		r.sysTotal = r.sysBusy + (long long)load.cpu_ticks[ CPU_STATE_IDLE ];
	}

	mach_task_basic_info_data_t task;
	n = MACH_TASK_BASIC_INFO_COUNT;
	if( task_info( mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&task, &n ) == KERN_SUCCESS )
		r.rss = task.resident_size;
	vm_statistics64_data_t vm;
	n = HOST_VM_INFO64_COUNT;
	if( host_statistics64( s.host, HOST_VM_INFO64, (host_info64_t)&vm, &n ) == KERN_SUCCESS && s.pageSize != 0 )
		r.avail = ( (uint64_t)vm.free_count + vm.inactive_count ) * (uint64_t)s.pageSize;	// free_count already holds the speculative pages (vm_statistics.h)

	// The process's CPU counters LAST, then their stamp at once: nothing read between them can stretch their interval.
	// Machine CPU is a ratio and memory a level, so their own read time does not enter a row's numbers.
#if !TARGET_OS_IPHONE && defined( __arm64__ )
	// Time on P-cores. rusage_info's times are mach ticks, not ns (ri_user_time + ri_system_time x 125/3 =
	// getrusage's total), so they go through the timebase.
	rusage_info_v6 ri;
	if( proc_pid_rusage( getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ri ) == 0 && s.timebase.denom != 0 )
		r.pcoreNs = (long long)( ( ri.ri_user_ptime + ri.ri_system_ptime ) * s.timebase.numer / s.timebase.denom );
#endif
	struct rusage ru;
	if( getrusage( RUSAGE_SELF, &ru ) == 0 )
		r.procNs = timevalNs( ru.ru_utime ) + timevalNs( ru.ru_stime );
	r.tNs = gxPerfMonoNs();		// the CPU counters' time
}

static void samplerInit( Sampler &s )
{
	s.host = mach_host_self();	// once: every call adds a send right
	mach_timebase_info( &s.timebase );
	if( host_page_size( s.host, &s.pageSize ) != KERN_SUCCESS )
		s.pageSize = 0;
}

#elif defined( _WIN32 )
static unsigned long long fileTimeRaw( const FILETIME &f )
{
	return ( (unsigned long long)f.dwHighDateTime << 32 ) | f.dwLowDateTime;
}

static long long fileTimeNs( const FILETIME &f )
{
	return (long long)fileTimeRaw( f ) * 100;
}

static void sampleOs( Sampler &, Raw &r, long long now )
{
	(void)now;
	FILETIME idle, kernel, user;
	// with more than one processor group (> 64 logical CPUs) GetSystemTimes may cover only this thread's group: unknown then, never one
	// group's idle standing for the machine
	if( GetActiveProcessorGroupCount() <= 1 && GetSystemTimes( &idle, &kernel, &user ) )
	{
		r.sysTotal = fileTimeNs( kernel ) + fileTimeNs( user );	// kernel time includes idle
		r.sysBusy = r.sysTotal - fileTimeNs( idle );
	}
	PROCESS_MEMORY_COUNTERS memory;
	if( K32GetProcessMemoryInfo( GetCurrentProcess(), &memory, sizeof( memory ) ) )
		r.rss = memory.WorkingSetSize;
	MEMORYSTATUSEX machine;
	machine.dwLength = sizeof( machine );
	if( GlobalMemoryStatusEx( &machine ) )
		r.avail = machine.ullAvailPhys;

	// The process's CPU counters LAST, then their stamp at once
	FILETIME created, exited;
	if( GetProcessTimes( GetCurrentProcess(), &created, &exited, &kernel, &user ) )
		r.procNs = fileTimeNs( kernel ) + fileTimeNs( user );
	r.tNs = gxPerfMonoNs();		// the CPU counters' time
}

static void samplerInit( Sampler & )
{
	// nothing to open: every Windows counter is one call per sample
}

#elif defined( __linux__ )
static bool readSmall( const char *path, char *buf, size_t size )
{
	const int fd = open( path, O_RDONLY | O_CLOEXEC );
	if( fd < 0 )
		return false;
	size_t got = 0;
	for( ssize_t n; got + 1 < size && ( n = read( fd, buf + got, size - 1 - got ) ) > 0; )
		got += (size_t)n;
	close( fd );
	buf[ got ] = 0;
	return got > 0;
}

static void sampleOs( Sampler &s, Raw &r, long long now )
{
	(void)now;
	static char text[ 8192 ];	// sampler thread only
	if( readSmall( "/proc/stat", text, sizeof( text ) ) )
		procStatSample( text, s.stat, r );
	if( readSmall( "/proc/self/statm", text, sizeof( text ) ) && s.pageSize > 0 )
	{
		const char *p = strchr( text, ' ' );	// size resident ...: the second field
		if( p != NULL )
			r.rss = (uint64_t)atoll( p + 1 ) * (uint64_t)s.pageSize;
	}
	if( readSmall( "/proc/meminfo", text, sizeof( text ) ) )
	{
		const long long kb = meminfoKb( text, "MemAvailable:" );
		if( kb >= 0 )
			r.avail = (uint64_t)kb * 1024u;
	}
	// The process's CPU counters LAST, then their stamp at once
	struct rusage ru;
	if( getrusage( RUSAGE_SELF, &ru ) == 0 )
		r.procNs = ( (long long)ru.ru_utime.tv_sec + ru.ru_stime.tv_sec ) * 1000000000LL + ( (long long)ru.ru_utime.tv_usec + ru.ru_stime.tv_usec ) * 1000LL;
	r.tNs = gxPerfMonoNs();
}

static void samplerInit( Sampler &s )
{
	s.pageSize = sysconf( _SC_PAGESIZE );
}

#else
static void sampleOs( Sampler &, Raw &, long long ) {}
static void samplerInit( Sampler & ) {}
#endif

// ---- What the main thread and the sampler share. Heap-allocated on the first start and never freed.
struct Shared
{
	std::mutex pub;				///< held by the sampler only to copy a finished sample in; the main thread only try_locks it
	Raw cur, prev;
	bool haveCur, havePrev;
	uint32_t seq;
	std::mutex wake;			///< the sampler sleeps on cv under this one, never holding pub
	std::condition_variable cv;
	std::atomic<bool> idle;
	std::atomic<uint32_t> gen;
	std::atomic<long long> lastReadNs;	///< the main thread's last read: the sampler aims its next sample 150 ms before the next one
	std::atomic<long long> costMaxNs, costSumNs;
	std::atomic<uint32_t> costN, lateReads;
	std::atomic<bool> writeTiming;
	bool testFail;				///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_OS_FAIL=1: every reader fails (the stub arm)
	int testHoldMs;				///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_SAMPLER_HOLD_MS: the sampler sits on pub that long
	bool testBlockingRead;			///< GX_TEST_INSTRUMENTS + GX_TEL_TEST_BLOCKING_READ=1: the hold test's fail arm, a reader that WAITS
};

static Shared *s_shared = NULL;
static bool s_threadStarted = false;	///< main thread only

static void samplerMain( Shared *sh )
{
#if defined( __APPLE__ )
	pthread_set_qos_class_self_np( QOS_CLASS_DEFAULT, 0 );	// normal, not background: see the banner
	pthread_setname_np( "gx-tel-sampler" );
#elif defined( __linux__ )
	pthread_setname_np( pthread_self(), "gx-tel-sampler" );
#endif
	Sampler *const s = new Sampler();	// value-initialised: every counter 0, every pointer null
	samplerInit( *s );
	for( ;; )
	{
		{
			std::unique_lock<std::mutex> lk( sh->wake );
			while( sh->idle.load() )
				sh->cv.wait_for( lk, std::chrono::seconds( 1 ) );
		}
		const uint32_t gen = sh->gen.load();
		const long long t0 = gxPerfMonoNs();
		Raw raw;
		rawUnknown( raw );
		raw.gen = gen;
		raw.beginNs = t0;
		if( !sh->testFail )
			sampleOs( *s, raw, t0 );
		const long long t1 = gxPerfMonoNs();
		if( raw.tNs < 0 )
			raw.tNs = t1;		// nothing was read (the stub arm, or an OS without readers): the sample's own time
		const long long cost = t1 - t0;
		sh->costSumNs.fetch_add( cost );
		sh->costN.fetch_add( 1 );
		if( cost > sh->costMaxNs.load() )
			sh->costMaxNs.store( cost );
		{
			std::lock_guard<std::mutex> g( sh->pub );
#ifdef GX_TEST_INSTRUMENTS
			if( sh->testHoldMs > 0 )
				std::this_thread::sleep_for( std::chrono::milliseconds( sh->testHoldMs ) );
#endif
			if( gen == sh->gen.load() )	// a recording that started while this sample ran owns nothing of it
			{
				raw.seq = ++sh->seq;
				sh->prev = sh->cur;
				sh->havePrev = sh->haveCur;
				sh->cur = raw;
				sh->haveCur = true;
			}
		}
		// Next: 150 ms before the main thread's next read (rows close once a second), at least 200 ms away; plain 1 s when it is not reading.
		const long long now = gxPerfMonoNs(), read = sh->lastReadNs.load();
		long long next = t0 + 1000000000LL;	// t0: this sample's start
		if( read > 0 && now - read < 3000000000LL )
		{
			next = read + 850000000LL;
			while( next < now + 200000000LL )
				next += 1000000000LL;
		}
		std::unique_lock<std::mutex> lk( sh->wake );
		sh->cv.wait_until( lk, std::chrono::steady_clock::time_point( std::chrono::nanoseconds( next ) ),
			[ sh, gen ]() { return sh->idle.load() || sh->gen.load() != gen; } );
	}
}

/// GXReplayDiag's OS reader, on the main thread at a row's close: never waits.
static bool readOs( GxTelOsSnapshot *out )
{
	Shared *const sh = s_shared;
	if( sh == NULL )
		return false;
	sh->lastReadNs.store( gxPerfMonoNs() );
	std::unique_lock<std::mutex> lk( sh->pub, std::try_to_lock );
#ifdef GX_TEST_INSTRUMENTS
	if( sh->testBlockingRead && !lk.owns_lock() )
		lk.lock();	// what try_lock exists to prevent: the main thread waiting out the sampler
#endif
	if( !lk.owns_lock() )
	{
		sh->lateReads.fetch_add( 1 );
		return false;
	}
	if( !sh->haveCur )
		return false;
	const Raw cur = sh->cur, prev = sh->prev;
	const bool havePrev = sh->havePrev;
	lk.unlock();
	static Raw s_last;			// main thread only: the sample the previous row ended on
	static bool s_haveLast = false;
	const int which = pickBase( cur, prev, havePrev, s_last, s_haveLast, sh->gen.load() );
	if( which == 0 )
		return false;
	osDelta( which == 2 ? s_last : prev, cur, *out );
	s_last = cur;
	s_haveLast = true;
	return true;
}

/// GXReplayDiag's sampler control: start 1 at a recording's start, 0 at its end. Lock-free on the main thread; a wake that is missed
/// costs the sampler at most its 1 s timeout.
static void samplerControl( int start )
{
	Shared *const sh = s_shared;
	if( !start )
	{
		if( sh == NULL )
			return;
		sh->idle.store( true );
		sh->cv.notify_all();
		if( sh->writeTiming.load() )
		{
			const uint32_t n = sh->costN.load();
			fprintf( stderr, "[GXTEL] sampler samples=%u mean_us=%lld max_us=%lld late_reads=%u\n", n,
				n != 0 ? sh->costSumNs.load() / n / 1000 : -1LL, sh->costMaxNs.load() / 1000, sh->lateReads.load() );
		}
		return;
	}
	if( sh == NULL )
	{
		Shared *const fresh = new Shared();	// never freed: the detached thread may outlive every destructor
		fresh->haveCur = fresh->havePrev = false;
		fresh->seq = 0;
		fresh->idle.store( true );
		fresh->gen.store( 0 );
		fresh->lastReadNs.store( 0 );
		fresh->testFail = false;
		fresh->testHoldMs = 0;
		fresh->testBlockingRead = false;
#ifdef GX_TEST_INSTRUMENTS
		const char *const fail = getenv( "GX_TEL_TEST_OS_FAIL" ), *const hold = getenv( "GX_TEL_TEST_SAMPLER_HOLD_MS" );
		const char *const blocking = getenv( "GX_TEL_TEST_BLOCKING_READ" );
		fresh->testFail = fail != NULL && strcmp( fail, "1" ) == 0;
		fresh->testHoldMs = hold != NULL ? atoi( hold ) : 0;
		fresh->testBlockingRead = blocking != NULL && strcmp( blocking, "1" ) == 0;
#endif
		s_shared = fresh;
	}
	const char *const timing = getenv( "GX_TEL_WRITE_TIMING" );
	s_shared->writeTiming.store( timing != NULL && strcmp( timing, "1" ) == 0 );
	s_shared->costMaxNs.store( 0 );
	s_shared->costSumNs.store( 0 );
	s_shared->costN.store( 0 );
	s_shared->lateReads.store( 0 );
	s_shared->gen.fetch_add( 1 );
	s_shared->idle.store( false );
	if( !s_threadStarted )
	{
		try
		{
			std::thread( samplerMain, s_shared ).detach();
			s_threadStarted = true;
		}
		catch( ... )
		{
			// no thread: every OS field reads UNKNOWN + sampler late, and the game is untouched
		}
	}
	s_shared->cv.notify_all();
}

} // namespace gxtel

/// From gxReplayDiagRegisterDevice (W3DDisplay::init): the reader and the control.
static void gxReplayTelRegisterSampler( void )
{
	gxReplayTelSetOsReader( gxtel::readOs );
	gxReplayTelSetSamplerControl( gxtel::samplerControl );
}
// GX-TEL-SAMPLER-END

void gxReplayDiagRegisterDevice( void )
{
	gxReplayDiagSetDeviceFacts( gxReplayDiagDeviceFacts );
	gxReplayDiagSetHwExtras( gxReplayDiagHwExtras );
	gxReplayTelRegisterSampler();
}
