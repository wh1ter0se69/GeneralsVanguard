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

#include "thread_compat.h"
#include <map>
#include <mutex>

// GeneralsX: Phase 5 - Thread ID mapper for legacy int-based APIs
static std::map<pthread_t, int> thread_id_map;
static std::mutex thread_id_map_mutex;
static int next_thread_id = 1;

THREAD_ID GetCurrentThreadId()
{
	pthread_t thread_id = pthread_self();
	return (THREAD_ID)thread_id;
}

int GetCurrentThreadIdAsInt()
{
	// GeneralsX: Phase 5 - Convert pthread_t to unique int for legacy code
	pthread_t current = pthread_self();
	
	std::lock_guard<std::mutex> lock(thread_id_map_mutex);
	
	auto it = thread_id_map.find(current);
	if (it != thread_id_map.end()) {
		return it->second;
	}
	
	// Add new thread mapping
	int new_id = next_thread_id++;
	if (new_id < 0) next_thread_id = 1;  // Prevent overflow
	thread_id_map[current] = new_id;
	return new_id;
}

struct thread_data
{
	start_routine start_routine_arg;
	void *lpParameter;
};

void* thread_start(void *arg)
{
	thread_data *data = (thread_data*)arg;
	data->start_routine_arg(data->lpParameter);
	delete data;
	return NULL;
}

void* CreateThread(void *lpSecure, size_t dwStackSize, start_routine lpStartAddress, void *lpParameter, unsigned long dwCreationFlags, unsigned long *lpThreadId)
{
	pthread_t thread_id;
	thread_data *data = new thread_data;
	data->start_routine_arg = lpStartAddress;
	data->lpParameter = lpParameter;
	pthread_create(&thread_id, NULL, thread_start, data);
	return (void*)thread_id;
}

int TerminateThread(void *hThread, unsigned long dwExitCode)
{
	return pthread_cancel((pthread_t)hThread);
}