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

#pragma once

#include "ref_ptr.h"
#include "refcount.h"
#include "STLUtils.h"
#include "stringex.h"
#include <Utility/stdio_adapter.h>
#include <rts/profile.h>

// TheSuperHackers @build 10/02/2026 Bender
// Linux port: Include platform compatibility headers BEFORE any DirectX/Windows headers are parsed
// This ensures Win32 types (DWORD, HANDLE, etc.) and COM types (IUnknown, GUID, etc.) are defined
#if !defined(_WIN32)
    // Define guard to tell old Utility/compat.h that new compat is already included
    #define DEPENDENCIES_UTILITY_COMPAT_H 1
    
    // Order matters: bittype.h defines basic types, types_compat adds Windows types, com_compat adds COM
    #include "bittype.h"           // Basic types: uint32, DWORD, BYTE, etc.
    #include "types_compat.h"      // Windows types: HANDLE, HWND, HMONITOR, RGNDATA, etc.
    #include "windows_compat.h"    // Windows API compatibility: GetDoubleClickTime, HIWORD, etc.
    #include "com_compat.h"        // COM/DirectX: IUnknown, GUID, REFIID, DECLARE_INTERFACE_, etc.
    
    // GeneralsX @build fbraz 10/02/2026 Bender
    // CRITICAL: Include time_compat.h and gdi_compat.h DIRECTLY here because windows_compat.h
    // skips them when DEPENDENCIES_UTILITY_COMPAT_H is defined (see windows_compat.h line 138)
    // ww3d.cpp needs MMRESULT/timeBeginPeriod (time_compat.h) and BITMAP structures (gdi_compat.h)
    #include "time_compat.h"       // MMRESULT, timeBeginPeriod, timeEndPeriod stubs
    #include "gdi_compat.h"        // BITMAPFILEHEADER, BITMAPINFOHEADER, BI_RGB
    #include "string_compat.h"     // lstrlen, lstrcpy, lstrcmpi mappings
    #include "file_compat.h"       // GetCurrentDirectory, GetFileAttributes stubs
    #include "vfw_compat.h"        // VFW/AVI API stubs (FramGrab.cpp movie capture - Phase 3 out-of-scope)
#endif

// TheSuperHackers @build (merged from upstream) Utility COM macro for safe Release calls
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if(p) { (p)->Release(); (p)=nullptr; } }
#endif

// This macro serves as a general way to determine the number of elements within an array.
#ifndef ARRAY_SIZE
#if defined(_MSC_VER) && _MSC_VER < 1300
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#else
template <typename Type, size_t Size> char (*ArraySizeHelper(Type(&)[Size]))[Size];
#define ARRAY_SIZE(arr) sizeof(*ArraySizeHelper(arr))
#endif
#endif // ARRAY_SIZE

// GeneralsX: GX_LOGIC_60HZ - opt-in 60 Hz simulation tick.
//
// DEFAULT IS 30 AND MUST STAY 30. This is the shipped rate, the rate the four-platform determinism
// corpus in docs/WORKDIR/baseline-2026-08-04/ was captured at, and the rate every .rep recorded so
// far replays at. Flipping the default would silently invalidate all three.
// [23/09/2026] Still true of the CMake OPTION default, but no longer "the shipped rate": every
// shipping preset sets GX_LOGIC_60HZ=ON since 0b9bed517, so 60 Hz is what players run.
//
// A 30 Hz and a 60 Hz build CANNOT desync against each other: engineID hashes LOGICFRAMES_PER_SECOND
// (SimID epoch 3, GameDefines.h), so a mismatched pair refuses to join with ENGINE_DIFFERS instead
// of diverging on contact. That safety net is why this can be a build-time choice at all.
//
// WHAT IS NOT DONE YET, so nobody switches this on expecting a finished feature. Status updated
// 23/09/2026 - the original three points are kept, each with what became of it:
//   * [DONE, 44107218d] ConnectionManager caps the negotiated rate at a literal 30, so a 60 Hz
//     NETWORK match would run at half speed until that is made rate-relative. -> It now clamps to
//     LOGICFRAMES_PER_SECOND (ConnectionManager.cpp, the fpsCeiling clamp).
//   * [DONE, d11d68de5 + 644a00372 + 9b3db2ea1 + branch fix/logic-60hz-retail-timers] ~20 gameplay
//     constants are raw frame counts (build times, reload delays, spawn throttles, AI wait timers)
//     and halve in wall-clock duration at 60 Hz. -> Frame-count durations go through GX_FRAMES30;
//     per-frame rates, impulses and thresholds are #if GX_LOGIC_60HZ literals (x1/2, x1/4, exact);
//     per-frame decays are square-root literals or sqrtf of the INI value (never powf); INI msec
//     durations round on the retail 30 Hz grid. The full site table, and the handful left as owner
//     calls (altitude controller, zero-delay script cadence, MIN_LOGIC_FRAMES / MIN_RUNAHEAD, the
//     network history lengths), is docs/WORKDIR/LOGIC_60HZ_TIMERS_2026-09-23.md, and
//     scripts/test/audit/logic-frame-literals-lint.sh fails on any new raw frame literal.
//   * [STILL TRUE] a network peer must RENDER at >= the tick rate, because canUpdateNetworkGameLogic
//     has no time accumulator - so 60 Hz logic demands 60+ fps on EVERY peer.
// The converters in GameCommon.h (ConvertDurationFromMsecsToFrames, ConvertVelocityInSecsToFrames,
// ConvertAccelerationInSecsToFrames) already scale correctly, so INI-authored values are fine; it
// is only the hard-coded frame counts that are not.
#ifndef GX_LOGIC_60HZ
#define GX_LOGIC_60HZ 0
#endif

enum
{
	// TheSuperHackers @info The original WWSync was 33 ms, ~30 fps, integer.
	// Changing this will require tweaking all Drawable code that concerns the ww3d time step, including locomotion physics.
#if GX_LOGIC_60HZ
	WWSyncPerSecond = 60,
#else
	WWSyncPerSecond = 30,
#endif
	// NOTE: integer division. 1000/30 = 33 against a true 33.33 (1% fast); 1000/60 = 16 against a
	// true 16.67 (4% fast). The error TRIPLES at 60 Hz, so any consumer of this that matters for
	// pacing needs the real-valued MSEC_PER_LOGICFRAME_REAL in GameCommon.h instead.
	WWSyncMilliseconds = 1000 / WWSyncPerSecond,
};

#if defined(_MSC_VER) && _MSC_VER < 1300
typedef unsigned MemValueType;
typedef long Interlocked32; // To use with Interlocked functions
#else
typedef unsigned long long MemValueType;
typedef volatile long Interlocked32; // To use with Interlocked functions
#endif
