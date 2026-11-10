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

// GeneralsX: Cb-06a: the client animations' motion trace and its test triggers, for a rendered
// A/B at several fps caps. Both are off unless their variable is set, and then only print or trigger client state.
//
//   GX_ANIMTRACE=1   [GXANIM] lines: the camera lock's position per draw, the WW3D clocks per draw, the radar glow and
//                    command-button flash at each state change, each 30 Hz shell update. wall= is ms since the first line.
//   GX_ANIMTEST=glow@F,flash@F,camlock@F
//                    at logic frame F (of the first match): start the radar-attack glow, flash the first command button
//                    shown, lock the view on the first enemy command center (the client view alone, as a script's
//                    camera follow does; the simulation's ScriptedCamera lock is not set).
//
// Client-only and header-only: no file on a simulation-digested path includes it.

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GXAnimTrace
{
	inline bool on()
	{
		// GeneralsX: GX_ANIMTRACE and GX_ANIMTEST: only an instrumented build reads them - camlock
		// locked the view on an unseen enemy command center and the trace printed where it was.
#if defined(GX_TEST_INSTRUMENTS)
		static const bool s_on = []() { const char *e = getenv("GX_ANIMTRACE"); return e != nullptr && e[0] != '\0' && e[0] != '0'; }();
#else
		static const bool s_on = false;
#endif
		return s_on;
	}

	// ms since the first call, from a monotonic clock
	inline double wallMs()
	{
		static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_t0).count();
	}

	// True once, at the first call whose logic frame has reached the frame GX_ANIMTEST gives `what` ("glow@900").
	// frame 0 never fires: callers pass 0 outside a real match (the shell map's menu game advances frames too, and would
	// spend a trigger before the match).
	inline bool testAt(const char *what, unsigned frame)
	{
		enum { MaxItems = 8 };
		struct Item { char name[16]; unsigned at; bool done; };
		static Item s_items[MaxItems];
		static int s_count = -1;
		if (s_count < 0)
		{
			s_count = 0;
#if defined(GX_TEST_INSTRUMENTS)
			const char *e = getenv("GX_ANIMTEST");
#else
			const char *e = nullptr;
#endif
			while (e != nullptr && *e != '\0' && s_count < MaxItems)
			{
				const char *end = strchr(e, ',');
				const size_t len = end ? (size_t)(end - e) : strlen(e);
				const char *at = (const char *)memchr(e, '@', len);
				if (at != nullptr && (size_t)(at - e) < sizeof(s_items[0].name))
				{
					Item &it = s_items[s_count++];
					memcpy(it.name, e, (size_t)(at - e));
					it.name[at - e] = '\0';
					it.at = (unsigned)strtoul(at + 1, nullptr, 10);
					it.done = false;
				}
				e = end ? end + 1 : nullptr;
			}
		}
		if (frame == 0)
			return false;
		for (int i = 0; i < s_count; ++i)
		{
			Item &it = s_items[i];
			if (!it.done && frame >= it.at && strcmp(it.name, what) == 0)
			{
				it.done = true;
				if (on())
				{
					fprintf(stderr, "[GXANIM] test %s fired f=%u wall=%.3f\n", what, frame, wallMs());
					fflush(stderr);
				}
				return true;
			}
		}
		return false;
	}
}
