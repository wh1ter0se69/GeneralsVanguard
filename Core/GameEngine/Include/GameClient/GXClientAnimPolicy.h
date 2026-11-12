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

// Client animations that ran at the render rate (camera lock easing, radar-attack glow, command-button flash, the
// shell's 30 Hz gate) are measured against a 60 FPS REFERENCE FRAME (1/RefFps s); refFrames converts BaseFps =
// 30 ratios. Idioms: easeStep 1-(1-k)^n; takeBaseFrames (remainder carried, step floored at 5 fps); stepEvery;
// gateDue (keeps phase). Client-only, header-only (scripts/test/client-anim-policy-test.sh).

#pragma once

#include <cmath>

namespace GXClientAnimPolicy
{
	static const float RefFps = 60.0f;	///< the reference frame rate

	// Reference frames in a step given in base frames of baseFps (BaseFps = 30: m_drawFpsRatio, gxRenderBaseOverUpdateFpsRatio).
	inline float refFrames(float baseFrames, float baseFps)
	{
		return baseFrames * (RefFps / baseFps);
	}

	// The share of the remaining distance an ease covers in `baseFrames` frames, when it covers `k` per frame. One frame
	// returns k itself, bit for bit; k is clamped to [0, 1], no time covers nothing.
	inline float easeStep(float k, float baseFrames)
	{
		if (baseFrames == 1.0f)
			return k;
		if (k <= 0.0f || baseFrames <= 0.0f)
			return 0.0f;
		if (k >= 1.0f)
			return 1.0f;
		return 1.0f - std::pow(1.0f - k, baseFrames);
	}

	// Whole base frames to step this update, given the base frames it covers; the remainder is carried to the next one.
	inline int takeBaseFrames(float &carry, float baseFrames)
	{
		if (baseFrames > 0.0f)
			carry += baseFrames;
		const int n = (int)carry;
		carry -= (float)n;
		return n;
	}

	// A step every `every` base frames: true (once) when the base frames since the last step reach it. `since` starts at
	// `every` for a step on the first update. The caller's frame count is floored at 5 fps, so no update owes two steps.
	inline bool stepEvery(float &since, float baseFrames, float every)
	{
		if (baseFrames > 0.0f)
			since += baseFrames;
		if (since < every)
			return false;
		since -= every;
		if (since >= every)
			since = 0.0f;
		return true;
	}

	// A fixed-rate gate: true when this call runs the update. dueAtMs is the next due point (the caller starts it at its
	// first call's time plus one period); frameMs is the time since the previous call. The frame nearest the due point runs
	// it (up to half a frame early, at most half a period), and the due point then advances by one period rather than to
	// now, so a render rate that does not divide the period still averages it. More than a period behind (a stall, a render
	// rate under the gate's) it restarts from now: one update, no burst.
	inline bool gateDue(double &dueAtMs, double nowMs, double periodMs, double frameMs)
	{
		double early = 0.5 * frameMs;
		if (early > 0.5 * periodMs)
			early = 0.5 * periodMs;
		if (early < 0.0)
			early = 0.0;
		if (nowMs + early < dueAtMs)
			return false;
		dueAtMs += periodMs;
		if (dueAtMs <= nowMs)
			dueAtMs = nowMs + periodMs;
		return true;
	}
}
