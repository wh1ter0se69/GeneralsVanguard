/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: The time a once-per-DRAW camera step covers (wh1ter0se, 03/10, fast forward: "moving/
// zooming out in and moving around ... is buggy and freezing just the moving thing").
//
// The camera's zoom and ground-follow easing (W3DView::zoomCameraToDesiredHeight, movePivotToGround) run once per draw,
// from W3DView::update, and stepped by the pacer's LAST ITERATION (FramePacer::getBaseOverUpdateFpsRatio). At 1x every
// iteration draws, so the two are the same time. Under fast forward only 1 iteration in k draws (FramePacer::
// isFastForwardDrawDue) - and so does a network tick catch-up pass - so each draw stepped 1/k of the time that passed:
// the zoom eased at half speed at 2x, a quarter at 4x, a tenth at 10x, and nearly stopped whenever the iteration before
// a draw was a short unpaced one repaying a slow draw. The step is the loop time since the previous draw instead.
//
// Client-only: nothing here or at its call sites is in the simulation digest (the loop clock is GameClient.cpp's
// gxRenderLoopSeconds, not FramePacer's), and nothing it moves is read by the logic.
// Header-only and free of engine types so scripts/test/camera-step-test.sh can drive it.

#pragma once

#include <algorithm>

namespace CameraStepPolicy
{
	// Seconds since the previous draw: the loop clock (gxRenderLoopSeconds, the sum of every iteration's update time,
	// FramePacer::getUpdateTime, added at each client update) now, less its value at that draw. A second draw inside the same iteration (W3DDisplay::draw's freeze-time
	// loop) has no loop time of its own and steps by the iteration's update time, as every draw did before.
	inline float drawStepSeconds(double loopNow, double loopAtLastDraw, float updateTime)
	{
		return loopNow > loopAtLastDraw ? (float)(loopNow - loopAtLastDraw) : updateTime;
	}

	// FramePacer::getBaseOverUpdateFpsRatio's formula over a step instead of the last iteration, floored the same way
	// (a step past 1 / minFps - a stall, a minimised window - counts as 1 / minFps). Bit for bit the old value at 1x.
	inline float baseOverStepFpsRatio(float stepSeconds, float baseFps, float minFps = 5.0f)
	{
		return baseFps / std::max(1.0f / stepSeconds, minFps);
	}
}
