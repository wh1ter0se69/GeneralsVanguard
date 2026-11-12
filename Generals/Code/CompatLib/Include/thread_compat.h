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

#include <pthread.h>
#include <stdint.h>
typedef pthread_t THREAD_ID;
typedef uint32_t (*start_routine)(void *);

// Avoid conflict with old Dependencies/Utility/Utility/thread_compat.h
// The old header defines int GetCurrentThreadId(), we define THREAD_ID GetCurrentThreadId()
// When both are included, we only declare if not already defined as the old int version
#if !defined(GetCurrentThreadId) || !defined(__THREAD_COMPAT_OLD_INCLUDED)
THREAD_ID GetCurrentThreadId();
#endif

// GeneralsX: Phase 5 - Convert pthread_t to int for compatibility
// Maps pthread_t to unique integers (max 256 threads tracked)
int GetCurrentThreadIdAsInt();

void* CreateThread(void *lpSecure, size_t dwStackSize, start_routine lpStartAddress, void *lpParameter, unsigned long dwCreationFlags, unsigned long *lpThreadId);
int TerminateThread(void *hThread, unsigned long dwExitCode);