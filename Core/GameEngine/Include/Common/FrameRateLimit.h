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

	// Cumulative nanoseconds the limiter has spent holding; wait() cannot tell a held frame from a slow one.
	// A monotonic total that a reader baselines and diffs, because wait() callers and readers run at different
	// rates (the loading loop in startNewGame; HeadlessMatch::run never reaches the pacer).
	// Int64 ns: a 32-bit Real would round small addends away over a long match.
	Int64 getTotalHoldNs() const { return m_totalHoldNs; } ///< Cumulative ns spent sleeping/spinning inside wait(). Never decreases; unchanged when uncapped.

private:
	Int64 m_freq;
	Int64 m_start;

	// Deadline this frame is paced to, on an absolute grid advanced one period per call, not re-derived from m_start.
	// Pacing from m_start (when wait() last exited) makes every period at least the target; the rate only undershoots.
	// m_start still gives the caller the real frame period.
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

