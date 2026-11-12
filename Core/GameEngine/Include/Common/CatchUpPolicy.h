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

// Tick catch-up: at most one logic tick runs per main-loop iteration, so a slow draw slowed the simulation.
// When the network pacer already has a second frame due, the iteration skips its draw and runs a logic-only pass,
// so the tick sequence is the 1x one. Every tick still needs Network::timeForNewFrame and AllCommandsReady.
// Bounds: MaxLogicOnlyPassesPerDraw (2) in a row, a draw every MaxUsWithoutDraw (100 ms), no skipping for
// CpuBoundBackoffUs (1 s) after a pass took a whole frame; pacer debt up to CatchUpDebtFrames (4) instead of retail 2.
// Header only and engine-free: scripts/test/catchup-pacing-test.sh.

#pragma once

#include <cstdint>

namespace CatchUpPolicy
{
	enum
	{
		RetailDebtFrames = 2,			///< Network::timeForNewFrame's forgiveness before catch-up
		CatchUpDebtFrames = 4,			///< ...and while catch-up is on: enough for a draw of ~3 periods
		MaxLogicOnlyPassesPerDraw = 2	///< so at most 3 ticks per drawn frame
	};
	const int64_t MaxUsWithoutDraw = 100000;	///< a draw at least 10 times a second
	const int64_t CpuBoundBackoffUs = 1000000;

	// Network::timeForNewFrame's pacer step: may a frame start at `now`? Advances `next` by one period, or re-anchors it
	// to now when more than debtFrames periods behind. Units: whatever clock the caller keeps `next` in.
	inline bool pacerStep(int64_t &next, int64_t now, int64_t frameDelay, int debtFrames)
	{
		if (now >= next) {
			if ((next + (debtFrames * frameDelay)) < now) {
				next = now;
			} else {
				next += frameDelay;
			}
			return true;
		}
		return false;
	}

	// How many frame periods are due at `now`: 0 before the next frame's time, 1 within a period of it, and so on.
	inline int framesDue(int64_t next, int64_t now, int64_t frameDelay)
	{
		if (now < next || frameDelay <= 0)
			return 0;
		return 1 + (int)((now - next) / frameDelay);
	}

	// The frames the pacer owes THIS machine: none when the frame after the next one has no commands yet. Debt that comes
	// from waiting on a slow remote peer's commands is not this machine's to repay - skipping a draw there ticks nothing
	// sooner and only costs a frame.
	inline int framesOwed(int framesDue, bool followingFrameReady)
	{
		return followingFrameReady ? framesDue : 0;
	}

	// Regular gate (single player, skirmish, replays). catchUp FALSE is the retail gate: accumulator mode credits
	// min(elapsed, period) per pass; tick-every-iteration mode ticks on every call and leaves carry alone.
	// catchUp TRUE, period = 1 / min(logic rate, render cap): accumulator mode credits the whole pass, so a slow
	// draw leaves debt repaid one tick per pass; tick-every-iteration mode carries debt = wall time - ticks x period.
	// A pass over CatchUpDebtFrames + 1 periods is a hitch, credited at the retail rate. At most one tick per call.
	inline bool regularStep(float &carry, float elapsed, float period, bool tickEveryIteration, bool catchUp)
	{
		if (!catchUp)
		{
			if (tickEveryIteration)
				return true;
			carry += (elapsed < period ? elapsed : period);
			if (carry >= period)
			{
				carry -= period;
				return true;
			}
			return false;
		}
		if (elapsed > (float)(CatchUpDebtFrames + 1) * period)
			elapsed = (elapsed < period ? elapsed : period);
		const float most = (float)CatchUpDebtFrames * period;
		if (tickEveryIteration)
		{
			carry += elapsed - period;
			if (carry < 0.0f)
				carry = 0.0f;
			if (carry > most)
				carry = most;
			return true;
		}
		carry += elapsed;
		if (carry > most + period)
			carry = most + period;
		if (carry >= period)
		{
			carry -= period;
			return true;
		}
		return false;
	}

	// The ticks a regular-gate pass would owe at draw time: its carry plus the pass about to be credited. In both modes 1 is
	// the tick this iteration takes anyway; 2 or more lets FramePacer::isCatchUpDrawSkip skip the draw.
	inline int regularFramesDue(float carry, float elapsed, float period)
	{
		if (period <= 0.0f)
			return 0;
		// a hitch is credited at the retail rate (regularStep): count it the same way, so the draw never skips for a debt the
		// gate is about to forgive
		if (elapsed > (float)(CatchUpDebtFrames + 1) * period)
			elapsed = (elapsed < period ? elapsed : period);
		const float total = carry + elapsed;
		return total < period ? 0 : (int)(total / period);
	}

	// The regular gate's catch-up state, shared by its two halves: GameEngine::canUpdateRegularGameLogic keeps carry (its
	// m_logicTimeAccumulator) and kept, the catch-up period of its last pass (0 = that pass was not catch-up, so the carry is
	// the retail one); FramePacer::isCatchUpDrawSkip reads both at the draw. Pure, so the model test runs this exact code.
	//
	// Nothing kept survives an ineligible pass (a pause, frozen time, a menu, fast forward): the debt is dropped to 0, and
	// only if catch-up kept some, so a run that never used catch-up (GX_CATCHUP=0, a -headless peer, base Generals) is the
	// retail gate bit for bit.
	inline void regularDrop(float &carry, float &kept)
	{
		if (kept > 0.0f)
		{
			carry = 0.0f;
			kept = 0.0f;
		}
	}

	// One gate pass with a logic rate above 0. Ineligible: drop, then the retail gate at retailPeriod. Eligible: a FRESH
	// pass (catch-up just turned on, or its period changed - the replay strip's 0.5x <-> 1x) starts from a zero carry and is
	// credited at the retail rate, since the elapsed it carries was measured under other rules (a load's fade, a half-speed
	// period) and must not become debt; every later pass is regularStep with catch-up.
	inline bool regularGate(float &carry, float &kept, float elapsed, float retailPeriod, float catchUpPeriod,
		bool tickEveryIteration, bool eligible)
	{
		if (!eligible)
		{
			regularDrop(carry, kept);
			return regularStep(carry, elapsed, retailPeriod, tickEveryIteration, false);
		}
		const bool fresh = catchUpPeriod != kept;
		if (fresh)
			carry = 0.0f;
		kept = catchUpPeriod;
		return regularStep(carry, elapsed, catchUpPeriod, tickEveryIteration, !fresh);
	}

	// The frames the regular gate owes at the draw: 0 unless its last pass was catch-up at this very period, so the draw
	// that precedes a fresh pass is never skipped (the gate credits that pass at the retail rate).
	inline int regularDueAtDraw(float carry, float kept, float elapsed, float period)
	{
		return (kept > 0.0f && kept == period) ? regularFramesDue(carry, elapsed, period) : 0;
	}

	struct Tracker
	{
		int passesSinceDraw = 0;
		bool lastWasSkip = false;
		int64_t lastDecisionUs = 0;
		int64_t lastDrawUs = 0;
		int64_t backoffUntilUs = 0;
	};

	// This iteration draws: an iteration catch-up is not eligible for (not in a match, stalled, fast forward...) counts as
	// a draw too, so nothing measured before it carries over.
	inline void drew(Tracker &t, int64_t nowUs)
	{
		t.passesSinceDraw = 0;
		t.lastWasSkip = false;
		t.lastDecisionUs = nowUs;
		t.lastDrawUs = nowUs;
	}

	// Called once per main-loop iteration, at its draw. framesDue: the pacer's, before this iteration's network pass.
	// TRUE: skip this draw (run a logic-only pass).
	inline bool skipDraw(Tracker &t, int framesDue, int64_t nowUs, int64_t periodUs)
	{
		// The iteration since the last decision was a logic-only pass that took a whole period: the tick and the client
		// update alone fill it, so skipping draws cannot repay anything.
		if (t.lastWasSkip && nowUs - t.lastDecisionUs >= periodUs)
			t.backoffUntilUs = nowUs + CpuBoundBackoffUs;
		// One frame due is the one this iteration ticks anyway: drawing first loses nothing.
		const bool skip = framesDue >= 2
			&& t.passesSinceDraw < MaxLogicOnlyPassesPerDraw
			&& nowUs - t.lastDrawUs < MaxUsWithoutDraw
			&& nowUs >= t.backoffUntilUs;
		if (!skip) {
			drew(t, nowUs);
			return false;
		}
		++t.passesSinceDraw;
		t.lastWasSkip = true;
		t.lastDecisionUs = nowUs;
		return true;
	}
}
