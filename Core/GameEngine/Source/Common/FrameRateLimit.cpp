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

#include "PreRTS.h"
#include "Common/FrameRateLimit.h"

// GeneralsX: Platform-specific high-resolution timing
#ifndef _WIN32
#include <time.h>    // clock_gettime, nanosleep, clock_nanosleep
#include <errno.h>   // EINTR
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>  // mach_wait_until, mach_absolute_time, mach_timebase_info
#include <sys/event.h>       // kqueue, kevent64, NOTE_CRITICAL
#endif

namespace
{

// GeneralsX: One clock, read one way, for the whole file.
//
// The constructor, wait() and reset() each used to read the clock themselves, and reset() got it
// wrong: it called QueryPerformanceCounter with no #ifdef, and off Windows that is the CompatLib
// shim (CompatLib/Include/time_compat.h), which reports 100-NANOSECOND units while the POSIX
// constructor sets m_freq = 1000000000 and stores NANOSECONDS. m_start landed ~100x too small, so
// the next wait() measured an elapsed of roughly the machine's whole uptime and did not pace that
// frame at all. Three readers meant three chances to disagree; there is now one.
Int64 nowTicks()
{
#ifdef _WIN32
	LARGE_INTEGER tick;
	QueryPerformanceCounter(&tick);
	return static_cast<Int64>(tick.QuadPart);
#else
	struct timespec tick;
	clock_gettime(CLOCK_MONOTONIC, &tick);
	return static_cast<Int64>(tick.tv_sec) * 1000000000 + tick.tv_nsec;
#endif
}

Int64 clockFrequency()
{
#ifdef _WIN32
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	return static_cast<Int64>(freq.QuadPart);
#else
	// nanoseconds per second. nowTicks() returns nanoseconds off Windows, and sleepUntilTick()
	// below relies on that identity rather than re-deriving it.
	return 1000000000;
#endif
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

HANDLE createFrameTimer()
{
	// Win10 1803+. Falls back to an ordinary waitable timer, then to Sleep(), so an older kernel
	// loses precision rather than the ability to pace.
	HANDLE timer = ::CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer == NULL)
	{
		timer = ::CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	}
	return timer;
}
#endif

// GeneralsX: Sleep until an absolute deadline, and do NOT busy-wait.
//
// What this replaces: sleep to 2 ms short of the target, then spin on the clock until the target
// arrives. That guard band is an ABSOLUTE 2 ms against a target that is not - at the 8x network
// floor the target is 1/480 = 2.083 ms, so sleepSeconds was negative on essentially every frame
// and the limiter degenerated into a pure busy-wait. PERF_LOCAL_FRAMETIME_2026-08-13.md measured
// exactly that: at a 480 cap, nanosleep 0.00% of the main thread and the clock_gettime spin
// 33.46%; at a 60 cap the spin still cost 0.69 ms of every 16.67 ms frame, 19% on top of all the
// CPU the game actually uses. On a weak CPU that is a core-slice spent watching a clock, and it
// competes with the pathfinder for it.
//
// Every platform here has a kernel primitive that wakes a thread at a deadline, which is what the
// spin was emulating in userspace. None of them was being used. There is deliberately no spin tail
// on any arm - a guard band small enough not to cost CPU is also too small to correct anything,
// and the deadline grid in wait() already absorbs the wake-up error by shortening the next frame.
void sleepUntilTick(Int64 deadlineTicks, Int64 freq)
{
	const Int64 remaining = deadlineTicks - nowTicks();
	if (remaining <= 0)
	{
		return;
	}

#if defined(_WIN32)
	static HANDLE s_timer = createFrameTimer();
	if (s_timer != NULL)
	{
		// A NEGATIVE due time means "relative, in 100 ns units".
		LARGE_INTEGER due;
		due.QuadPart = -((remaining * 10000000) / freq);
		if (due.QuadPart < 0 && ::SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE))
		{
			::WaitForSingleObject(s_timer, INFINITE);
			return;
		}
	}
	// No high-resolution timer: Sleep whole milliseconds only. FramePacer's constructor holds
	// timeBeginPeriod(1), so this lands within ~1 ms. Rounding DOWN can return fractionally early;
	// that is deliberate, because the deadline grid keeps the next frame on schedule and the mean
	// rate still equals the cap, whereas rounding up would need a spin to correct.
	const Int64 remainingMs = (remaining * 1000) / freq;
	if (remainingMs > 0)
	{
		::Sleep(static_cast<DWORD>(remainingMs));
	}
#elif defined(__APPLE__)
	// macOS and iOS have no clock_nanosleep - confirmed absent from MacOSX.sdk and iPhoneOS.sdk.
	// mach_wait_until is the kernel absolute-deadline wait, but it counts in mach absolute-time
	// units, which are (a) on a DIFFERENT epoch from CLOCK_MONOTONIC - measured ~504450 s apart on
	// an Apple Silicon machine that had been up six days - and (b) NOT nanoseconds: mach_timebase
	// _info reports 125/3 on Apple Silicon, i.e. a 24 MHz counter. Feeding it a CLOCK_MONOTONIC
	// value would put every deadline in the past and skip every sleep.
	//
	// So convert the REMAINING INTERVAL and add it to mach_absolute_time(). An interval needs only
	// the scale, never the epoch. The same conversion idiom is already in CompatLib/Source/
	// time_compat.cpp for timeGetTime().
	static mach_timebase_info_data_t s_timebase = { 0, 0 };
	if (s_timebase.denom == 0)
	{
		mach_timebase_info(&s_timebase);
	}
	if (s_timebase.denom != 0 && s_timebase.numer != 0)
	{
		// remaining is nanoseconds here - see clockFrequency().
		const uint64_t machTicks = static_cast<uint64_t>(remaining) * s_timebase.denom / s_timebase.numer;
		const uint64_t machDeadline = mach_absolute_time() + machTicks;

		// GeneralsX: mach_wait_until is a NORMAL-urgency kernel timer, and XNU coalesces those:
		// it fires anywhere in a leeway window that grows with the wait (~a quarter of it), even on a user-interactive
		// thread. Measured on M-series Macs at a 60 cap: woke 2.3-3.7 ms late at the median, up to ~7 ms, differently
		// every frame, so the grid turned each late wake into a long + short frame pair - p99 ~22 ms and ~10% of frames
		// over 20 ms, in full G3 games on the Mini with both renderers (the "worst frame every second" was the max of
		// that spread per 1-s window). NOTE_CRITICAL asks the kernel for no coalescing on THIS timer only: ~0.02 ms
		// late at the median. No spin, no thread priority change. Falls back to mach_wait_until if the timer cannot be
		// armed. scripts/test/frame-limit-wake-test.sh pins it.
		static const int s_kq = kqueue();
		if (s_kq >= 0)
		{
			struct kevent64_s ev;
			EV_SET64(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_MACHTIME | NOTE_CRITICAL,
				static_cast<int64_t>(machDeadline), 0, 0, 0);
			int fired;
			while ((fired = kevent64(s_kq, &ev, 1, &ev, 1, 0, NULL)) < 0 && errno == EINTR) {}
			if (fired == 1 && (ev.flags & EV_ERROR) == 0)
			{
				return;
			}
		}
		mach_wait_until(machDeadline);
		return;
	}
	struct timespec ts;
	ts.tv_sec  = static_cast<time_t>(remaining / 1000000000);
	ts.tv_nsec = static_cast<long>(remaining % 1000000000);
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#elif defined(__linux__) && defined(TIMER_ABSTIME)
	// A true kernel absolute deadline on the same clock nowTicks() reads, so wake-up error cannot
	// accumulate across frames the way a chain of relative sleeps can.
	struct timespec ts;
	ts.tv_sec  = static_cast<time_t>(deadlineTicks / 1000000000);
	ts.tv_nsec = static_cast<long>(deadlineTicks % 1000000000);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
#else
	// Portable arm. POSIX guarantees nanosleep suspends for AT LEAST the interval, so this never
	// returns early and never overshoots the cap; it only oversleeps, which the deadline grid
	// repays on the following frame.
	struct timespec ts;
	ts.tv_sec  = static_cast<time_t>(remaining / 1000000000);
	ts.tv_nsec = static_cast<long>(remaining % 1000000000);
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#endif
}

} // namespace

FrameRateLimit::FrameRateLimit()
{
	m_freq = clockFrequency();
	m_start = nowTicks();
	m_deadline = m_start;
	m_totalHoldNs = 0;
}

Real FrameRateLimit::wait(UnsignedInt maxFps, UnsignedInt pollFps, Bool (*poll)(), UnsignedInt maxDebtPeriods)
{
	PROFILER_SECTION;

	// GeneralsX: Validate FPS limit to prevent division by zero and underflow
	// Skip limiting if maxFps is 0 or extremely high (uncapped mode)
	//
	// GeneralsX: Was `maxFps > 1000000`, one short of
	// RenderFpsPreset::UncappedFpsValue, so the value the UI actually means by "uncapped" missed
	// this branch and went through the limiter with a 1 microsecond target. Named, and inclusive.
	if (maxFps == 0 || maxFps >= RenderFpsPreset::UncappedFpsValue)
	{
		// Uncapped or invalid: just return elapsed time without limiting.
		// Nothing was held back, so the running total does not move. The deadline grid is re-seeded
		// to now, so that re-enabling a cap paces from this frame instead of inheriting a deadline
		// left over from before the uncapped stretch.
		const Int64 tick = nowTicks();
		const double elapsedSeconds = static_cast<double>(tick - m_start) / static_cast<double>(m_freq);
		m_start = tick;
		m_deadline = tick;
		return static_cast<Real>(elapsedSeconds);
	}

	// Round to nearest rather than truncating: truncation biases every period short, and although
	// that bias is under 100 ns it is systematic and points at "faster than the cap".
	const Int64 targetTicks = (m_freq + static_cast<Int64>(maxFps) / 2) / static_cast<Int64>(maxFps);

	// Advance the GRID by exactly one period. This is the debt carry, and it is the entire fix for
	// the undershoot: a frame that ends late leaves the next deadline nearer, so the next frame is
	// shortened by what this one overran. Sleep granularity, which is the dominant error at low
	// caps, is therefore repaid rather than accumulated.
	m_deadline += targetTicks;

	Int64 tick = nowTicks();
	const Int64 entryTick = tick; // work done before the limiter got involved

	// Forgive debt beyond one whole period. Without this, a frame that ran long - a map load, a
	// pathfinding burst, a window drag - would be repaid by a burst of consecutive unpaced frames,
	// which is worse than simply losing the time. Bounded at one period, at most one frame can run
	// unpaced, and that frame still costs its own work. Network::timeForNewFrame() forgives at two
	// periods; one is the tighter choice here because this side must never overshoot the cap.
	//
	// GeneralsX: ...except under fast forward (FramePacer::update), which asks for ~100 ms: there
	// the loop IS the logic's pacer, one tick per iteration, and a draw that costs several periods would otherwise lose
	// those ticks for good. The burst of unpaced iterations that repays it is exactly the catch-up fast forward wants.
	if (tick - m_deadline > targetTicks * static_cast<Int64>(maxDebtPeriods > 0 ? maxDebtPeriods : 1))
	{
		m_deadline = tick;
	}

	// GeneralsX: The hold is sliced for the network: poll() runs every
	// 1/pollFps s until the deadline (FramePacer::update - a network and logic pass with no draw), so
	// a cap at the tick rate still reads the socket and ticks on time. The draw deadline is unchanged.
	Int64 pollSpent = 0; // time inside poll() is work, not a hold
	if (poll != nullptr && pollFps > maxFps)
	{
		const Int64 pollTicks = (m_freq + static_cast<Int64>(pollFps) / 2) / static_cast<Int64>(pollFps);
		while (tick + pollTicks < m_deadline)
		{
			sleepUntilTick(tick + pollTicks, m_freq);
			const Int64 before = nowTicks();
			const Bool again = poll();
			tick = nowTicks();
			pollSpent += tick - before;
			if (!again)
				break;
		}
	}

	if (m_deadline > tick)
	{
		sleepUntilTick(m_deadline, m_freq);
		tick = nowTicks();
	}

	const double elapsedSeconds = static_cast<double>(tick - m_start) / static_cast<double>(m_freq);

	// Unchanged meaning: sleep plus any spin, i.e. everything the limiter held. There is no spin
	// any more, so this is now the sleep alone - which is what makes a before/after hold_ms
	// comparison in [GXPACE] read correctly instead of hiding the change.
	m_totalHoldNs += static_cast<Int64>(
		(static_cast<double>(tick - entryTick - pollSpent) / static_cast<double>(m_freq)) * 1000000000.0);

	m_start = tick;
	return static_cast<Real>(elapsedSeconds);
}

// GeneralsX: This read the clock through a raw QueryPerformanceCounter
// with no #ifdef _WIN32. Off Windows that is the CompatLib shim, which reports 100-NANOSECOND
// units (CompatLib/Include/time_compat.h) while the POSIX constructor sets m_freq = 1000000000 and
// stores NANOSECONDS. Measured on an Apple Silicon machine: m_start landed exactly 100.00x too
// small, so the next wait() computed an elapsed of 513548 SECONDS - roughly the uptime.
//
// Consequences, and only these two - the third one is a claim that does not survive the code:
//   * that frame is not paced. sleepSeconds is hugely negative and the spin exits on its first
//     test, whatever the cap.
//   * wait() RETURNS 513548 s into FramePacer::m_updateTime, so for one frame every
//     getUpdateTime()/getUpdateFps() consumer reads garbage. getBaseOverUpdateFpsRatio() floors
//     the fps at 5 so the window transition merely runs ~6x fast for that frame; Intro.cpp:342,350
//     and ControlBar.cpp:1706 do not clamp at all.
//   * NOT m_totalHoldNs. entrySeconds is captured AFTER the bogus elapsed, so the bogus part
//     cancels in (elapsedSeconds - entrySeconds) and the accumulator is unharmed - confirmed in a
//     live base-binary run, where hold_ms never exceeded 914 of a 1000 ms window.
//
// Its only caller is FramePacer::reset(), immediately before the FadeWholeScreen loop in
// GameLogic::startNewGame() - i.e. ONE frame per match start on macOS, iOS and Linux, in the loop
// that upstream #2889 added reset() to fix in the first place. It is NOT the undershoot: it cannot
// recur between resets and does not explain a sustained 405 against a 480 cap.
void FrameRateLimit::reset()
{
	m_start = nowTicks();
	m_deadline = m_start;
}


// GeneralsX: Nothing below the tick rate at GX_LOGIC_60HZ. The engine advances
// AT MOST ONE logic frame per main-loop iteration, so a render cap under the tick rate would also cap
// the LOGIC; FramePacer::getActualFramesPerSecondLimit therefore lifts any cap under it to it, in every
// game, and 30, 50 and 56 were steps that changed nothing. At 30 Hz logic the ladder keeps them: 30 is
// the tick rate there. (This closes the 05/08/2026 TODO that stood here: "filter s_fpsValues below
// LOGICFRAMES_PER_SECOND before GX_LOGIC_60HZ is offered to players".)
const UnsignedInt RenderFpsPreset::s_fpsValues[] = {
#if !GX_LOGIC_60HZ
	30, 50, 56,
#endif
	60, 65, 70, 72, 75, 80, 85, 90, 100, 110, 120, 144, 240, 480, UncappedFpsValue };

static_assert(LOGICFRAMES_PER_SECOND <= 30 || GX_LOGIC_60HZ, "Min FPS values need to be revisited!");

UnsignedInt RenderFpsPreset::getNextFpsValue(UnsignedInt value)
{
	const Int first = 0;
	const Int last = ARRAY_SIZE(s_fpsValues) - 1;
	// A cap under the ladder (-fps 30, a 50 Hz display) steps up to its first entry, not to Uncapped.
	if (value < s_fpsValues[first])
	{
		return s_fpsValues[first];
	}
	for (Int i = first; i < last; ++i)
	{
		if (value >= s_fpsValues[i] && value < s_fpsValues[i + 1])
		{
			return s_fpsValues[i + 1];
		}
	}
	return s_fpsValues[last];
}

UnsignedInt RenderFpsPreset::getPrevFpsValue(UnsignedInt value)
{
	const Int first = 0;
	const Int last = ARRAY_SIZE(s_fpsValues) - 1;
	for (Int i = last; i > first; --i)
	{
		if (value <= s_fpsValues[i] && value > s_fpsValues[i - 1])
		{
			return s_fpsValues[i - 1];
		}
	}
	return s_fpsValues[first];
}

UnsignedInt RenderFpsPreset::changeFpsValue(UnsignedInt value, FpsValueChange change)
{
	switch (change)
	{
	default:
	case FpsValueChange_Increase: return getNextFpsValue(value);
	case FpsValueChange_Decrease: return getPrevFpsValue(value);
	}
}


UnsignedInt LogicTimeScaleFpsPreset::getNextFpsValue(UnsignedInt value)
{
	return value + StepFpsValue;
}

UnsignedInt LogicTimeScaleFpsPreset::getPrevFpsValue(UnsignedInt value)
{
	if (value - StepFpsValue < MinFpsValue)
	{
		return MinFpsValue;
	}
	else
	{
		return value - StepFpsValue;
	}
}

UnsignedInt LogicTimeScaleFpsPreset::changeFpsValue(UnsignedInt value, FpsValueChange change)
{
	switch (change)
	{
	default:
	case FpsValueChange_Increase: return getNextFpsValue(value);
	case FpsValueChange_Decrease: return getPrevFpsValue(value);
	}
}
