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

// GX_HOOK_TIMER(name) times a scope on this thread CPU clock, summing one hook calls per logic frame.
// [GXHOOK] lines: per 60 s window, slow past 1,000 us per frame, over at once past 2,000 us, and a flush at shutdown.
// A new match or a lower frame number restarts it; GX_HOOK_FLUSH_ALL() is explicit because no static destructors run.
// Test-instruments builds only; compiled out in release, never read by the simulation.
#pragma once

#if defined(GX_TEST_INSTRUMENTS)
#include <chrono>
#include <cstdio>
#include <ctime>
#include "GameLogic/GameLogic.h"

extern unsigned g_gxMatchSerial;	///< GameLogic.cpp: +1 per GameLogic::startNewGame (test-instruments builds)

struct GXHookStats
{
	const char *name;
	unsigned frame;				// the frame frameCpu/frameWall belong to
	long long frameCpu, frameWall;
	bool frameOver;				// the "over" line was printed for this frame
	unsigned windowStart;
	long long maxCpu, maxWall, totalCpu, calls;
	unsigned serial;			// g_gxMatchSerial these statistics belong to
	bool listed;				// linked into g_gxHookList (on the first call)
	GXHookStats *next;
};

inline GXHookStats *g_gxHookList = nullptr;	///< every hook that was ever called in this process (GX_HOOK_FLUSH_ALL walks it)

// the pending frame is complete: fold it into the window, a 'slow' line if it cost more than 1,000 us
inline void gxHookCloseFrame(GXHookStats &s)
{
	if (s.frameCpu > s.maxCpu) s.maxCpu = s.frameCpu;
	if (s.frameWall > s.maxWall) s.maxWall = s.frameWall;
	if (s.frameCpu > 1000)
	{
		fprintf(stderr, "[GXHOOK] f=%u hook=%s slow us=%lld wall=%lld\n", s.frame, s.name, s.frameCpu, s.frameWall);
		fflush(stderr);
	}
}
inline void gxHookPrintWindow(const GXHookStats &s)
{
	fprintf(stderr, "[GXHOOK] f=%u hook=%s us=%lld wall=%lld calls=%lld total_us=%lld\n", s.frame, s.name, s.maxCpu, s.maxWall, s.calls,
		s.totalCpu);
	fflush(stderr);
}
// close the pending frame and print the open window of a hook that was called in this match (the new-match reset and process exit)
inline void gxHookFlush(GXHookStats &s)
{
	if (!s.listed || s.calls == 0)
		return;						// never called, or the window was printed and nothing came after it
	gxHookCloseFrame(s);
	gxHookPrintWindow(s);
	s.calls = 0;
}
// shutdown: every hook's open frame and window, its 'flushed' line, then one 'exit' line (once per process; a second call prints nothing)
/// rb-core: what else must print before "[GXHOOK] exit" (the rule bank's ev=cost / ev=activity of
/// every bank still alive, GXRuleBank.cpp) - set by whoever needs it, nullptr otherwise
extern void (*g_gxHookFlushExtra)();
inline void GXHookFlushAll()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	if (g_gxHookFlushExtra != nullptr)
		g_gxHookFlushExtra();
	for (GXHookStats *s = g_gxHookList; s != nullptr; s = s->next)
	{
		gxHookFlush(*s);
		fprintf(stderr, "[GXHOOK] f=%u hook=%s flushed\n", s->frame, s->name);
	}
	fprintf(stderr, "[GXHOOK] exit\n");
	fflush(stderr);
}

class GXHookTimer
{
public:
	explicit GXHookTimer(GXHookStats &s) : m_s(s)
	{
		m_cpu0 = cpuNowUs();
		m_t0 = std::chrono::steady_clock::now();
	}
	~GXHookTimer()
	{
		const long long wall = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_t0).count();
		const long long c = cpuNowUs();
		const long long cpu = (c >= 0 && m_cpu0 >= 0) ? c - m_cpu0 : wall;		// no thread CPU clock (Windows): wall
		const unsigned f = (unsigned)TheGameLogic->getFrame();
		if (!m_s.listed)
		{
			m_s.listed = true;			// first call in this process: on the shutdown flush's list
			m_s.next = g_gxHookList;
			g_gxHookList = &m_s;
		}
		if (m_s.serial != g_gxMatchSerial || f < m_s.frame)
		{
			// a new match: the frame counter restarted - the old match's open frame and window are printed, nothing is carried over
			gxHookFlush(m_s);
			m_s.serial = g_gxMatchSerial;
			m_s.windowStart = f;
			m_s.maxCpu = m_s.maxWall = m_s.totalCpu = m_s.calls = 0;
			startFrame(f);
		}
		else if (f != m_s.frame)
		{
			gxHookCloseFrame(m_s);			// the previous frame is complete
			if (m_s.frame - m_s.windowStart >= 60u * (unsigned)LOGICFRAMES_PER_SECOND)
			{
				gxHookPrintWindow(m_s);
				m_s.windowStart = f;
				m_s.maxCpu = m_s.maxWall = m_s.totalCpu = m_s.calls = 0;
			}
			startFrame(f);
		}
		m_s.frameCpu += cpu;
		m_s.frameWall += wall;
		m_s.totalCpu += cpu;
		++m_s.calls;
		if (!m_s.frameOver && m_s.frameCpu > 2000)
		{
			m_s.frameOver = true;
			fprintf(stderr, "[GXHOOK] f=%u hook=%s over us=%lld wall=%lld\n", f, m_s.name, m_s.frameCpu, m_s.frameWall);
			fflush(stderr);
		}
	}

private:
	void startFrame(unsigned f)
	{
		m_s.frame = f;
		m_s.frameCpu = m_s.frameWall = 0;
		m_s.frameOver = false;
	}
	static long long cpuNowUs()
	{
#ifndef _WIN32
		struct timespec t;
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0)
			return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
#endif
		return -1;
	}
	GXHookStats &m_s;
	long long m_cpu0;
	std::chrono::steady_clock::time_point m_t0;
};

// One call site: GX_HOOK_TIMER("name"); at the top of the hook's body.
#define GX_HOOK_TIMER(hookName) \
	static GXHookStats gxHookStats_ = { hookName, 0u, 0LL, 0LL, false, 0u, 0LL, 0LL, 0LL, 0LL, 0u, false, nullptr }; \
	GXHookTimer gxHookTimer_(gxHookStats_)
// Several call sites of ONE arm summed per frame (the 2 ms bound is per tick, not per site): GX_HOOK_STATS(var, "name"); once at file
// scope, GX_HOOK_TIMER_ON(var); at the top of each site. Never nest two timers on one var (the inner time would count twice).
#define GX_HOOK_STATS(var, hookName) static GXHookStats var = { hookName, 0u, 0LL, 0LL, false, 0u, 0LL, 0LL, 0LL, 0LL, 0u, false, nullptr }
#define GX_HOOK_TIMER_ON(var) GXHookTimer gxHookTimer_(var)
// One arm's sites in SEVERAL source files share one per-frame sum: GX_HOOK_STATS_DEFINE(var, "name"); at file scope in one of them,
// GX_HOOK_STATS_EXTERN(var); in the others, GX_HOOK_TIMER_ON(var); at each site.
#define GX_HOOK_STATS_DEFINE(var, hookName) GXHookStats var = { hookName, 0u, 0LL, 0LL, false, 0u, 0LL, 0LL, 0LL, 0LL, 0u, false, nullptr }
#define GX_HOOK_STATS_EXTERN(var) extern GXHookStats var
// Shutdown (SDL3Main.cpp, WinMain.cpp): print what every hook still holds, then "[GXHOOK] exit".
#define GX_HOOK_FLUSH_ALL() GXHookFlushAll()
#else
#define GX_HOOK_TIMER(hookName) do { } while (0)
#define GX_HOOK_STATS(var, hookName) static_assert(true, "")
#define GX_HOOK_TIMER_ON(var) do { } while (0)
#define GX_HOOK_STATS_DEFINE(var, hookName) static_assert(true, "")
#define GX_HOOK_STATS_EXTERN(var) static_assert(true, "")
#define GX_HOOK_FLUSH_ALL() do { } while (0)
#endif
