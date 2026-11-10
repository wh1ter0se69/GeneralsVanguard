/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later */
#include "types_compat.h"
#include "time_compat.h"

#include <time.h>

// GeneralsX: Phase 5 macOS timer support (CLOCK_BOOTTIME -> CLOCK_UPTIME)
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

DWORD timeGetTime(void)
{
  // Boost could be used but is slow
#ifdef __APPLE__
  // macOS: Use mach_absolute_time() for uptime
  static mach_timebase_info_data_t tb = { 0, 0 };
  if (tb.denom == 0) {
    mach_timebase_info(&tb);
  }
  uint64_t elapsed = mach_absolute_time();
  uint64_t nanoseconds = elapsed * tb.numer / tb.denom;
  DWORD diff = (DWORD)(nanoseconds / 1000000);
#elif defined(__linux__)
  // Linux: CLOCK_BOOTTIME is Linux-specific (system uptime)
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  DWORD diff = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
  // Other POSIX: Use CLOCK_MONOTONIC (fallback for BSD, etc.)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  DWORD diff = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
  return diff;
}

DWORD GetTickCount(void)
{
  return timeGetTime();
}

void Sleep(DWORD ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

// GeneralsX: Fill wMilliseconds like Win32 does. It was always 0, so the
// screenshot name sshot_<date>_<hhmmss>_<ms>.png (W3DScreenshot.cpp) repeated within a second and every
// capture after the first in that second overwrote the file: a GX_SHOT burst of 16 consecutive frames on
// macOS left ONE png while logging 16 [GXSHOT] captures. Also makes localtime reentrant (localtime_r).
// Readers of wMilliseconds: that name, the debug log timestamp, the save-game date. None is simulation.
void GetLocalTime(SYSTEMTIME* st)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	time_t t = ts.tv_sec;
	struct tm tmv;
	localtime_r(&t, &tmv);
	st->wYear = tmv.tm_year + 1900;
	st->wMonth = tmv.tm_mon + 1;
	st->wDayOfWeek = tmv.tm_wday;
	st->wDay = tmv.tm_mday;
	st->wHour = tmv.tm_hour;
	st->wMinute = tmv.tm_min;
	st->wSecond = tmv.tm_sec;
	st->wMilliseconds = (uint16_t)(ts.tv_nsec / 1000000);
}