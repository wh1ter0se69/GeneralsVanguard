/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

/*
** MilesStub.h
**
** Stub definitions for Miles Sound System types when compiling on Linux with OpenAL.
**
** TheSuperHackers @build 15/12/2024
** This header provides minimal type definitions to allow WWAudio.h and related
** files to compile without the Miles SDK headers. These are only used when
** SAGE_USE_OPENAL is defined (Linux builds).
**
** Compile-only shim for the Miles API that WWAudio references, used under SAGE_USE_OPENAL (no Miles SDK headers).
** Nothing links libwwaudio in those builds, so every entry point is inert: enumerators return "no more entries" and
** provider/open calls fail, so no stubbed loop can spin forever.
** HANDLE is a plain typedef, not a macro: a macro collided with the real typedef in CompatLib's types_compat.h.
*/

#pragma once

#if defined(SAGE_USE_OPENAL) && !defined(_MILES_STUB_H)
#define _MILES_STUB_H

// VC++ calling convention stubs
#if !defined(_WIN32)
    #define __stdcall
    #define __cdecl
    #define AILCALLBACK
#else
    #define AILCALLBACK
#endif

#if !defined(_WIN32)

#include <stdint.h>
#include <string.h>

// CompatLib pieces the WWAudio sources need next to the Miles stubs.
// threads_compat.h -> CRITICAL_SECTION + Initialize/DeleteCriticalSection (Utils.h, WWAudio.cpp)
// tchar_compat.h   -> TCHAR / LPCTSTR (Utils.h)
#include "threads_compat.h"
#include "tchar_compat.h"

// Windows type stubs
typedef void*          HANDLE;

#else // _WIN32

// Windows needs a header here too: this is the OpenAL arm of Utils.h's include, taken when mss.h is not included,
// and mss.h was what pulled in <windows.h> (CRITICAL_SECTION, InitializeCriticalSection, DeleteCriticalSection).
// WIN32_LEAN_AND_MEAN: only the synchronisation primitives are wanted, not the min/max and PlaySound macros.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#endif // !_WIN32

// The Miles scalar typedefs (U8, S32, ...) must NOT be inside the !_WIN32 guard above: the inline stub bodies use them
// unconditionally, and on Windows mss.h is never reached, so nothing else defines them.
// Only HANDLE stays guarded, because Windows defines it. The widths hold everywhere (MSVC x64 is LLP64, int is 32-bit).
typedef unsigned char  U8;
typedef signed char    S8;
typedef unsigned short U16;
typedef signed short   S16;
typedef unsigned int   U32;
typedef signed int     S32;
typedef float          F32;
typedef double         F64;

// Miles Sound System type stubs (not including callback typedefs - those are defined in AudioEvents.h)
// The digital driver is dereferenced by WWAudio.cpp (m_Driver2D->emulated_ds), so it needs a body.
struct DIG_DRIVER {
    S32 emulated_ds;
};
typedef DIG_DRIVER* HDIGDRIVER;

typedef void* HPROVIDER;
typedef void* HSAMPLE;
typedef void* H3DSAMPLE;
typedef void* H3DPOBJECT;
typedef void* HSTREAM;
// HTIMER is an integer handle in Miles, and WWAudio compares/assigns it against -1.
typedef S32 HTIMER;

// Provider/filter enumeration cursor
typedef S32 HPROENUM;
#define HPROENUM_FIRST 0

// Wave format stub
typedef struct {
    U16 wFormatTag;
    U16 nChannels;
    U32 nSamplesPerSec;
    U32 nAvgBytesPerSec;
    U16 nBlockAlign;
    U16 wBitsPerSample;
} WAVEFORMAT;
typedef WAVEFORMAT* LPWAVEFORMAT;

// mmreg.h's PCM flavour of the above (WWAudio.cpp builds one to open the 2D device)
typedef struct {
    WAVEFORMAT wf;
    U16        wBitsPerSample;
} PCMWAVEFORMAT;
typedef PCMWAVEFORMAT* LPPCMWAVEFORMAT;

// Driver info stub
struct DRIVER_INFO_STRUCT {
    char name[256];
    int capabilities;
    void* handle;
};

// Sound description returned by AIL_WAV_info()
struct AILSOUNDINFO {
    S32         format;
    void const* data_ptr;
    U32         data_len;
    U32         rate;
    S32         bits;
    S32         channels;
    U32         samples;
    U32         block_size;
    void const* initial_ptr;
};

///////////////////////////////////////////////////////////////////////////////////////////
// Miles constants
///////////////////////////////////////////////////////////////////////////////////////////
#ifndef AIL_NO_ERROR
#define AIL_NO_ERROR 0
#endif
#ifndef M3D_NOERR
#define M3D_NOERR 0
#endif
#ifndef AIL_LOCK_PROTECTION
#define AIL_LOCK_PROTECTION 0
#endif
#ifndef DIG_USE_WAVEOUT
#define DIG_USE_WAVEOUT 1
#endif
#ifndef AIL_3D_2_SPEAKER
#define AIL_3D_2_SPEAKER 0
#endif
#ifndef DP_FILTER
#define DP_FILTER 0
#endif
#ifndef ENVIRONMENT_GENERIC
#define ENVIRONMENT_GENERIC 0
#endif
#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 0x0001
#endif
#ifndef WAVE_FORMAT_IMA_ADPCM
#define WAVE_FORMAT_IMA_ADPCM 0x0011
#endif
#ifndef AIL_FILE_SEEK_BEGIN
#define AIL_FILE_SEEK_BEGIN 0
#endif
#ifndef AIL_FILE_SEEK_CURRENT
#define AIL_FILE_SEEK_CURRENT 1
#endif
#ifndef AIL_FILE_SEEK_END
#define AIL_FILE_SEEK_END 2
#endif
#ifndef NO
#define NO 0
#endif
#ifndef YES
#define YES 1
#endif

///////////////////////////////////////////////////////////////////////////////////////////
// Miles entry points.
//
// These are variadic templates rather than fixed signatures on purpose: the call sites are
// qualified (::AIL_xxx), so function-like macros cannot be used, and the stubs must accept
// whatever the original Miles prototypes accepted without this header having to restate
// ~70 SDK signatures it cannot verify. Every one is a no-op; only the return types matter.
///////////////////////////////////////////////////////////////////////////////////////////

// -- system ------------------------------------------------------------------------------
template <typename... Args> inline void  AIL_startup(Args&&...)   {}
template <typename... Args> inline void  AIL_shutdown(Args&&...)  {}
template <typename... Args> inline void  AIL_lock(Args&&...)      {}
template <typename... Args> inline void  AIL_unlock(Args&&...)    {}
template <typename... Args> inline char* AIL_last_error(Args&&...) { return const_cast<char*>("Miles is not available in this build"); }
template <typename... Args> inline S32   AIL_set_preference(Args&&...) { return AIL_NO_ERROR; }
template <typename... Args> inline void  AIL_set_file_callbacks(Args&&...) {}

// -- 2D driver ---------------------------------------------------------------------------
template <typename... Args> inline S32  AIL_waveOutOpen(Args&&...)  { return AIL_NO_ERROR; }
template <typename... Args> inline void AIL_waveOutClose(Args&&...) {}

// -- 2D samples --------------------------------------------------------------------------
template <typename... Args> inline HSAMPLE AIL_allocate_sample_handle(Args&&...) { return nullptr; }
template <typename... Args> inline void AIL_release_sample_handle(Args&&...)     {}
template <typename... Args> inline void AIL_init_sample(Args&&...)               {}
template <typename... Args> inline S32  AIL_set_named_sample_file(Args&&...)     { return AIL_NO_ERROR; }
template <typename... Args> inline void AIL_start_sample(Args&&...)              {}
template <typename... Args> inline void AIL_stop_sample(Args&&...)               {}
template <typename... Args> inline void AIL_resume_sample(Args&&...)             {}
template <typename... Args> inline void AIL_end_sample(Args&&...)                {}
template <typename... Args> inline void AIL_set_sample_volume_pan(Args&&...)     {}
template <typename... Args> inline void AIL_sample_volume_pan(Args&&...)         {}
template <typename... Args> inline void AIL_set_sample_loop_count(Args&&...)     {}
template <typename... Args> inline S32  AIL_sample_loop_count(Args&&...)         { return 0; }
template <typename... Args> inline void AIL_set_sample_ms_position(Args&&...)    {}
template <typename... Args> inline void AIL_sample_ms_position(Args&&...)        {}
template <typename... Args> inline void AIL_set_sample_user_data(Args&&...)      {}
template <typename... Args> inline void* AIL_sample_user_data(Args&&...)         { return nullptr; }
template <typename... Args> inline void AIL_set_sample_playback_rate(Args&&...)  {}
template <typename... Args> inline S32  AIL_sample_playback_rate(Args&&...)      { return 0; }
template <typename... Args> inline void AIL_set_sample_processor(Args&&...)      {}
template <typename... Args> inline void AIL_set_filter_sample_preference(Args&&...) {}

// -- 3D providers / listeners ------------------------------------------------------------
// Returns 0 so that "while (AIL_enumerate_3D_providers(...) > 0)" terminates immediately.
template <typename... Args> inline S32 AIL_enumerate_3D_providers(Args&&...) { return 0; }
// Returns 0 so that "if (AIL_enumerate_filters(...) == 0)" takes the "no filter" path.
template <typename... Args> inline S32 AIL_enumerate_filters(Args&&...)      { return 0; }
// Anything but M3D_NOERR: there is no 3D provider without Miles.
template <typename... Args> inline S32 AIL_open_3D_provider(Args&&...)       { return M3D_NOERR - 1; }
template <typename... Args> inline void AIL_close_3D_provider(Args&&...)     {}
template <typename... Args> inline void AIL_set_3D_speaker_type(Args&&...)   {}
template <typename... Args> inline H3DPOBJECT AIL_3D_open_listener(Args&&...) { return nullptr; }

// -- 3D samples --------------------------------------------------------------------------
template <typename... Args> inline H3DSAMPLE AIL_allocate_3D_sample_handle(Args&&...) { return nullptr; }
template <typename... Args> inline void AIL_release_3D_sample_handle(Args&&...)  {}
template <typename... Args> inline void AIL_start_3D_sample(Args&&...)           {}
template <typename... Args> inline void AIL_stop_3D_sample(Args&&...)            {}
template <typename... Args> inline void AIL_resume_3D_sample(Args&&...)          {}
template <typename... Args> inline void AIL_end_3D_sample(Args&&...)             {}
template <typename... Args> inline U32  AIL_set_3D_sample_file(Args&&...)        { return 1; }
template <typename... Args> inline U32  AIL_3D_sample_length(Args&&...)          { return 0; }
template <typename... Args> inline U32  AIL_3D_sample_offset(Args&&...)          { return 0; }
template <typename... Args> inline void AIL_set_3D_sample_offset(Args&&...)      {}
template <typename... Args> inline void AIL_set_3D_sample_volume(Args&&...)      {}
template <typename... Args> inline F32  AIL_3D_sample_volume(Args&&...)          { return 0.0F; }
template <typename... Args> inline void AIL_set_3D_sample_loop_count(Args&&...)  {}
template <typename... Args> inline S32  AIL_3D_sample_loop_count(Args&&...)      { return 0; }
template <typename... Args> inline void AIL_set_3D_sample_playback_rate(Args&&...) {}
template <typename... Args> inline S32  AIL_3D_sample_playback_rate(Args&&...)   { return 0; }
template <typename... Args> inline void AIL_set_3D_sample_distances(Args&&...)   {}
template <typename... Args> inline void AIL_set_3D_sample_effects_level(Args&&...) {}
template <typename... Args> inline void AIL_set_3D_position(Args&&...)           {}
template <typename... Args> inline void AIL_set_3D_orientation(Args&&...)        {}
template <typename... Args> inline void AIL_set_3D_velocity_vector(Args&&...)    {}
template <typename... Args> inline void AIL_set_3D_object_user_data(Args&&...)   {}
template <typename... Args> inline void* AIL_3D_object_user_data(Args&&...)      { return nullptr; }

// -- streams -----------------------------------------------------------------------------
template <typename... Args> inline HSTREAM AIL_open_stream(Args&&...)            { return nullptr; }
template <typename... Args> inline void AIL_close_stream(Args&&...)              {}
template <typename... Args> inline void AIL_start_stream(Args&&...)              {}
template <typename... Args> inline void AIL_pause_stream(Args&&...)              {}
template <typename... Args> inline void AIL_set_stream_loop_count(Args&&...)     {}
template <typename... Args> inline S32  AIL_stream_loop_count(Args&&...)         { return 0; }
template <typename... Args> inline void AIL_set_stream_loop_block(Args&&...)     {}
template <typename... Args> inline void AIL_set_stream_ms_position(Args&&...)    {}
template <typename... Args> inline void AIL_stream_ms_position(Args&&...)        {}
template <typename... Args> inline void AIL_set_stream_playback_rate(Args&&...)  {}
template <typename... Args> inline S32  AIL_stream_playback_rate(Args&&...)      { return 0; }
template <typename... Args> inline void AIL_set_stream_volume_pan(Args&&...)     {}
template <typename... Args> inline void AIL_stream_volume_pan(Args&&...)         {}

// -- timers ------------------------------------------------------------------------------
template <typename... Args> inline void AIL_stop_timer(Args&&...)            {}
template <typename... Args> inline void AIL_release_timer_handle(Args&&...)  {}

// -- misc --------------------------------------------------------------------------------
// Returns 0 ("no info") so callers keep their defaults instead of reading uninitialised data.
template <typename... Args> inline S32 AIL_WAV_info(Args&&...) { return 0; }

#if !defined(_WIN32)
///////////////////////////////////////////////////////////////////////////////////////////
// Win32 event / thread entry points used by WWAudio's delayed-release thread and timer.
// CompatLib provides GetTickCount, and its CloseHandle lives in the windows_compat.h block
// that WWCommon.h suppresses via DEPENDENCIES_UTILITY_COMPAT_H, so it is covered here too.
// These are function templates, so if CompatLib's non-template overloads are also visible
// they win overload resolution and nothing here changes. All of them are inert - none of
// this code path runs on a SAGE_USE_OPENAL build.
///////////////////////////////////////////////////////////////////////////////////////////
template <typename... Args> inline HANDLE CreateEvent(Args&&...) { return nullptr; }
template <typename... Args> inline int    CloseHandle(Args&&...) { return 1; }
template <typename... Args> inline int    SetEvent(Args&&...)    { return 1; }
template <typename... Args> inline int    ResetEvent(Args&&...)  { return 1; }
// WAIT_OBJECT_0 (0) - "the object is signalled", so any wait loop keyed on WAIT_TIMEOUT exits.
template <typename... Args> inline unsigned long WaitForSingleObject(Args&&...) { return 0; }
template <typename... Args> inline intptr_t _beginthread(Args&&...) { return -1; }
#endif // !_WIN32

#endif
