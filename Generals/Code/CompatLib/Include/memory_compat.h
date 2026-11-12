/*
**	Command & Conquer Generals(tm)
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

#pragma once

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <malloc.h>
#elif __APPLE__
#include <malloc/malloc.h>
#endif

#define GMEM_FIXED 0

static void *GlobalAlloc(int, size_t size)
{
  return malloc(size);
}

static void GlobalFree(void *ptr)
{
  free(ptr);
}

static size_t GlobalSize(void *ptr)
{
#ifdef __linux__
  return malloc_usable_size(ptr);
#elif defined(__APPLE__)
  return malloc_size(ptr);
#else
  #error "GlobalSize not implemented for this platform"
#endif
}

// MEMORYSTATUS - Windows memory status structure (for GlobalMemoryStatus)
// GeneralsX: Linux stub - memory profiling disabled
// GeneralsX: Add conditional guard - DXVK may have defined it
// NOTE: Game uses this for DEBUG_LOG only (dwAvailPageFile, dwAvailPhys, dwAvailVirtual).
#ifndef _MEMORYSTATUS_DEFINED
typedef struct MEMORYSTATUS {
    unsigned long dwLength;
    unsigned long dwMemoryLoad;
    unsigned long dwTotalPhys;
    unsigned long dwAvailPhys;
    unsigned long dwTotalPageFile;
    unsigned long dwAvailPageFile;
    unsigned long dwTotalVirtual;
    unsigned long dwAvailVirtual;
} MEMORYSTATUS;
#endif // _MEMORYSTATUS_DEFINED

// Pointer typedef for MEMORYSTATUS (always define)
typedef MEMORYSTATUS *LPMEMORYSTATUS;

// GlobalMemoryStatus stub - no-op on Linux (returns zeros)
// Used for debug logging only - safe to stub
static inline void GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer) {
    if (lpBuffer) {
        memset(lpBuffer, 0, sizeof(MEMORYSTATUS));
        lpBuffer->dwLength = sizeof(MEMORYSTATUS);
    }
}