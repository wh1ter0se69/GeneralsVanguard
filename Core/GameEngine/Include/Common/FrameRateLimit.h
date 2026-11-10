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

#include "Common/GameCommon.h"


class FrameRateLimit
{
public:
	FrameRateLimit();

	// GeneralsX: poll/pollFps: while holding for the cap, call poll() every
	// 1/pollFps s until the deadline or until it returns FALSE (FramePacer::update's network pass).
	Real wait(UnsignedInt maxFps, UnsignedInt pollFps = 0, Bool (*poll)() = nullptr, UnsignedInt maxDebtPeriods = 1); ///< maxDebtPeriods: how far behind the grid may run before it is forgiven (FramePacer fast forward)
	void reset(); ///< Move the timing anchor AND the deadline grid to now, discarding any time elapsed since the last call to wait.

	// GeneralsX: How long the limiter has spent limiting, cumulatively,
	// as opposed to how long iterations took. Needed because "my own limiter held me" and "I am
	// blocked on a peer's input" are different problems with different fixes, and nothing in the
	// engine could tell them apart. wait()'s return value cannot: it is the same number whether
	// the frame was slow or the limiter slept.
	//
	// Deliberately a CUMULATIVE TOTAL that a reader baselines and diffs, rather than a "last hold"
	// the reader adds up. Callers of wait() and readers of this do not run at the same rate in
	// either direction:
	//   * the loading transition in GameLogic::startNewGame() drives the pacer in its own loop
	//     without going through GameClient::update(), so several holds can land between two
	//     reads - a last-value reader silently drops all but the final one;
	//   * HeadlessMatch::run() calls TheGameEngine->update() directly and never reaches
	//     TheFramePacer->update() at all, so a reader can sample hundreds of times a second while
	//     wait() never runs - a last-value reader re-counts one stale hold forever.
	// A monotonic total with a per-window delta is immune to both.
	//
	// Nanoseconds in Int64, not seconds in Real: Real is 32-bit float, and a match accumulating
	// tens of thousands of ~33 ms holds would lose the small addend to rounding long before it
	// ended.
	Int64 getTotalHoldNs() const { return m_totalHoldNs; } ///< Cumulative ns spent sleeping/spinning inside wait(). Never decreases; unchanged when uncapped.

private:
	Int64 m_freq;
	Int64 m_start;

	// GeneralsX: The deadline this frame is being paced to, on an absolute
	// grid advanced by exactly one period per call - NOT re-derived from m_start.
	//
	// m_start is the tick wait() last EXITED at, so pacing from it makes every period independently
	// >= the target: sleep granularity and long frames both push a period over, and nothing ever
	// pulls one back under. The error is one-sided and the delivered rate can only sit below the
	// requested cap. Network::timeForNewFrame() has carried its debt on an absolute grid since the
	// beginning (Network.cpp, m_nextFrameTime += frameDelay); this did not, and that asymmetry is
	// the whole undershoot.
	//
	// Kept as a separate member rather than replacing m_start, because the two answer different
	// questions and wait() needs both: m_start gives the caller the real frame period it returns,
	// m_deadline gives the limiter the target it must not drift from.
	Int64 m_deadline;

	Int64 m_totalHoldNs;
};


enum FpsValueChange
{
	FpsValueChange_Increase,
	FpsValueChange_Decrease,
};


class RenderFpsPreset
{
public:
	enum CPP_11(: UnsignedInt)
	{
		UncappedFpsValue = 1000000,
	};

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);

private:
	static const UnsignedInt s_fpsValues[];
};


class LogicTimeScaleFpsPreset
{
public:
	enum CPP_11(: UnsignedInt)
	{
#if RTS_DEBUG
		MinFpsValue = 5,
#else
		MinFpsValue = LOGICFRAMES_PER_SECOND,
#endif
		StepFpsValue = 5,
	};

	static UnsignedInt getNextFpsValue(UnsignedInt value);
	static UnsignedInt getPrevFpsValue(UnsignedInt value);
	static UnsignedInt changeFpsValue(UnsignedInt value, FpsValueChange change);
};

