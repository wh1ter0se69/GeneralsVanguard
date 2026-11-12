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

// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
// Cross-platform threading primitives (Win32 critical sections for Linux)

#pragma once

#ifndef THREADS_COMPAT_H
#define THREADS_COMPAT_H

#ifdef _UNIX

// POSIX threads support
#include <pthread.h>

// Emulate Windows CRITICAL_SECTION with pthread_mutex_t
// Based on fighter19's CritSec implementation
typedef struct _CRITICAL_SECTION_COMPAT {
    pthread_mutex_t mutex;
    pthread_t owner;     // Thread ID of current owner (for recursive locking)
    int ref_count;       // Recursion depth counter
} CRITICAL_SECTION;

// Windows-style critical section API wrappers
#ifdef __cplusplus
extern "C" {
#endif

// Initialize a critical section (maps to pthread_mutex_init)
// A NORMAL mutex, with the recursion Windows gives CRITICAL_SECTION
// counted below instead. Darwin's libpthread only takes its inline fast path for a NORMAL mutex; a
// RECURSIVE one goes through _pthread_mutex_lock_init_slow -> firstfit_lock_slow on every call.
// MemoryPool enters TheMemoryPoolCriticalSection on every block alloc and free, and on the ring stress
// fixture that slow path was 10-11.6% of the macOS main thread, uncontended (no psynch waits).
inline void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_init(&cs->mutex, NULL);
    cs->owner = 0;
    cs->ref_count = 0;
}

// Destroy a critical section (maps to pthread_mutex_destroy)
inline void DeleteCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutex_destroy(&cs->mutex);
    cs->owner = 0;
    cs->ref_count = 0;
}

// Enter a critical section (blocking lock). Only the thread holding the mutex ever writes its own id
// into owner, and it clears owner before unlocking, so reading our own id back proves we hold it.
inline void EnterCriticalSection(CRITICAL_SECTION *cs) {
    pthread_t self = pthread_self();
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) == self) {
        cs->ref_count++;
        return;
    }
    pthread_mutex_lock(&cs->mutex);
    __atomic_store_n(&cs->owner, self, __ATOMIC_RELAXED);
    cs->ref_count = 1;
}

// Leave a critical section (unlock). The mutex is taken once per outermost enter, so it is released
// once, when the count returns to 0 (an unlock per leave would underflow it on a nested enter, which
// AsciiString::set -> releaseBuffer does on every assignment). A leave by a thread that does not own
// it is refused and changes nothing, as the RECURSIVE mutex refused it with EPERM: unlocking a NORMAL
// mutex from another thread would hand the owner's critical section to a third thread.
inline void LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (__atomic_load_n(&cs->owner, __ATOMIC_RELAXED) != pthread_self())
        return;
    if (--cs->ref_count == 0) {
        __atomic_store_n(&cs->owner, (pthread_t)0, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&cs->mutex);
    }
}

#ifdef __cplusplus
}
#endif

#endif // _UNIX

#endif // THREADS_COMPAT_H
