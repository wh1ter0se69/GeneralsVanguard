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

// Render interpolation phase: one alpha in [0,1] per client update, where the displayed world sits between the
// previous (0) and current (1) logic frame; consumers draw lerp(previous, current, alpha) on render-local copies.
// RENDER-ONLY, no engine includes (scripts/test/audit/bench/logic_phase.cpp tests it). Lockstep: the digested
// tick grid is estimated from tick-START stamps. Single player and replays: the accumulator's leftover.

#pragma once

#ifndef GX_LOGIC_PHASE_TEST_ARM
#define GX_LOGIC_PHASE_TEST_ARM 0	// the bench's fail arms; always 0 in the game
#endif

class GXLogicPhase
{
public:
	// A logic update is starting at frame `frame` (GameClient::setFrame). Only the stamp is kept here: an update that
	// does not advance (time frozen, a paused game) calls it too, so update() takes it only when the frame it
	// shows is exactly the one after the stamped update's. nowMs: a monotonic clock.
	void tick(unsigned frame, double nowMs)
	{
		m_tickFrame = frame;
		m_tickMs = nowMs;
		m_tickSeen = true;
	}

	// Once per client update, before anything is drawn. frame: the current logic frame. spCarry: the regular gate's
	// accumulator leftover in ticks (single player; unused in lockstep). lockstep: a network game.
	float update(unsigned frame, double nowMs, double tickMs, float spCarry, bool lockstep)
	{
		const int n = m_seen ? (int)(frame - m_frame) : -1;
		const double dtMs = m_seen ? nowMs - m_nowMs : 0.0;
		m_seen = true;
		m_frame = frame;
		m_nowMs = nowMs;

		if (n < 0 || tickMs <= 0.0)
		{
			// The first update, a new game (the frame counter went back) or no tick period: retail.
			m_alpha = 1.0f;
			m_ticksPerDraw = 1.0f;
			m_pinned = true;
			m_blend = 1.0f;
			m_gridValid = false;
			return m_alpha;
		}

		// Ticks per draw, smoothed. At or near one, every draw already shows a fresh frame: pin to 1.
		m_ticksPerDraw += 0.1f * ((n > 1 ? 1.0f : (float)n) - m_ticksPerDraw);
		if (m_ticksPerDraw >= PinEnter)
			m_pinned = true;
		else if (m_ticksPerDraw < PinLeave)
			m_pinned = false;
		const float ramp = (float)(dtMs / BlendMs);
		m_blend = m_pinned ? minF(1.0f, m_blend + ramp) : maxF(0.0f, m_blend - ramp);

		float base;
		if (lockstep)
		{
			if (n >= 1 && m_tickSeen && frame == m_tickFrame + 1)
				grid(m_tickFrame, m_tickMs, tickMs);
			base = m_gridValid ? clamp01((float)((nowMs - m_gridMs) / m_periodMs)) : 1.0f;
		}
		else
		{
#if GX_LOGIC_PHASE_TEST_ARM == 2
			spCarry = 1.0f;	// fail arm: single player ignores the accumulator
#endif
			// Time frozen: the gate keeps consuming ticks that GameLogic::update drops before m_frame++, so the leftover
			// saws within one logic frame. Once it has wrapped, the display has caught up with the frame shown: 1 until
			// the frame advances. The no-backwards rule below alone kept the highest point of the saw,
			// short of 1, for the whole pause.
			if (n != 0)
				m_spWrapped = false;
#if GX_LOGIC_PHASE_TEST_ARM != 6	// fail arm 6: the wrap is ignored
			else if (spCarry < m_spCarry)
				m_spWrapped = true;
#endif
			m_spCarry = spCarry;
			base = m_spWrapped ? 1.0f : clamp01(spCarry);
		}

		const float before = m_alpha;
#if GX_LOGIC_PHASE_TEST_ARM == 1
		m_alpha = base;	// fail arm: no pin to 1 when render <= logic
#else
		m_alpha = (m_blend >= 1.0f) ? 1.0f : base + m_blend * (1.0f - base);
#endif
#if GX_LOGIC_PHASE_TEST_ARM != 4	// fail arm 4: may run backwards
		// Never backwards within a logic frame: the poses have not changed, so a smaller alpha would walk moving units
		// back toward where they were. That happens when the single-player gate keeps accumulating through a scripted
		// time freeze (no frame advances), and when the pin releases between two ticks.
		if (n == 0 && m_alpha < before)
			m_alpha = before;
#endif
		return m_alpha;
	}

	float value() const { return m_alpha; }
	bool interpolating() const { return m_blend < 1.0f; }	///< not pinned to 1: the coming draws may blend, whatever this one's alpha
	double periodMs() const { return m_periodMs; }	///< the lockstep period in use (the bench checks it)

private:
	static constexpr double GridDriftMs = 0.1;	// how fast the envelope follows a grid that slipped late
	static constexpr double BlendMs = 250.0;		// the time to blend between interpolating and pinned
	static constexpr float PinEnter = 0.97f;
#if GX_LOGIC_PHASE_TEST_ARM == 3
	static constexpr float PinLeave = 0.97f;		// fail arm: no hysteresis
#else
	static constexpr float PinLeave = 0.85f;		// two late ticks in quick succession, not one
#endif
	static constexpr double PeriodGain = 0.02;		// the mean start-to-start interval: ~50 ticks
	static constexpr double SlugEnter = 1.06;		// mean / nominal above which the ticks are being stretched
	static constexpr double SlugLeave = 1.03;
	static constexpr double SlugMax = 1.1;			// Network::timeForNewFrame's stretch: frameDelay + frameDelay / 10

	// The tick `frame` started at startMs (it produced the current frame): fold it into the grid estimate. Re-anchor on
	// the first one, on one more than half a period late (a stall, or the network re-basing its grid after falling two
	// periods behind), and when the lobby rate changes.
	//
	// The period is the lobby rate's, except while the network self-slugs: Network::timeForNewFrame stretches its frame
	// delay by a tenth when the run-ahead cushion is low and leaves the rate alone, so on the nominal period the envelope
	// falls behind a sustained slug and re-anchors every few ticks - the stepping the phase exists to remove, just when
	// the network is under pressure. The mean start-to-start interval of the stamps (a stall's gap left
	// out) says when the ticks are stretched; with hysteresis, and exactly nominal otherwise, so an unslugged game is
	// what it was.
	void grid(unsigned frame, double startMs, double tickMs)
	{
		const double drift = tickMs - m_gridTickMs;
		const bool rateChanged = drift > tickMs * 0.01 || -drift > tickMs * 0.01;
		if (!m_gridValid || rateChanged)
		{
			m_meanPeriodMs = tickMs;
			m_slugging = false;
		}
		else if (frame > m_startFrame)
		{
			const double interval = (startMs - m_startMs) / (double)(frame - m_startFrame);
			if (interval > 0.0 && interval < tickMs * 1.5)
				m_meanPeriodMs += PeriodGain * (interval - m_meanPeriodMs);
		}
		m_startFrame = frame;
		m_startMs = startMs;
#if GX_LOGIC_PHASE_TEST_ARM == 5
		m_slugging = false;	// fail arm: the nominal period always
#else
		if (m_meanPeriodMs > tickMs * SlugEnter)
			m_slugging = true;
		else if (m_meanPeriodMs < tickMs * SlugLeave)
			m_slugging = false;
#endif
		m_periodMs = m_slugging ? minD(maxD(m_meanPeriodMs, tickMs), tickMs * SlugMax) : tickMs;

		if (!m_gridValid || rateChanged || startMs - (m_gridMs + m_periodMs) > m_periodMs * 0.5)
			m_gridMs = startMs;
		else
			m_gridMs = minD(startMs, m_gridMs + m_periodMs + GridDriftMs);
		m_gridTickMs = tickMs;
		m_gridValid = true;
	}

	static double minD(double a, double b) { return a < b ? a : b; }
	static double maxD(double a, double b) { return a > b ? a : b; }
	static float minF(float a, float b) { return a < b ? a : b; }
	static float maxF(float a, float b) { return a > b ? a : b; }
	static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

	float m_alpha = 1.0f;			// retail until the first update; a headless peer never updates
	bool m_seen = false;
	unsigned m_frame = 0;
	double m_nowMs = 0.0;
	float m_ticksPerDraw = 1.0f;
	bool m_pinned = true;
	float m_blend = 1.0f;
	bool m_tickSeen = false;
	unsigned m_tickFrame = 0;
	double m_tickMs = 0.0;
	bool m_gridValid = false;
	double m_gridMs = 0.0;
	double m_gridTickMs = 0.0;
	double m_periodMs = 1.0;		// the period alpha runs over: the lobby rate's, or the self-slugged one
	double m_meanPeriodMs = 0.0;
	bool m_slugging = false;
	unsigned m_startFrame = 0;
	double m_startMs = 0.0;
	float m_spCarry = 0.0f;
	bool m_spWrapped = false;
};
