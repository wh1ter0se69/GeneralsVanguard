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

// One max-FPS setting, resolved once in GameEngine::init (resolveCap): the launcher value (-fps N, or -noFPSLimit
// for 0), else the display refresh rate (GameEngine::getDisplayRefreshRate), else the tick rate. 0 is unlimited.
// The skirmish slider is game speed, never the render cap. The tick rate is a floor on the main loop (one logic
// frame per iteration; FramePacer::getActualFramesPerSecondLimit).

#pragma once

namespace FpsLimitPolicy
{
	// The ONE render cap, 0 = unlimited. argvGiven: -fps N or -noFPSLimit was on the command line.
	// refreshHz: the display's refresh rate, 0 when the device layer could not read one.
	inline int resolveCap(bool argvGiven, int argvCap, int refreshHz, int tickRate)
	{
		if (argvGiven)
			return argvCap > 0 ? argvCap : 0;
		if (refreshHz > 0)
			return refreshHz;
		return tickRate;
	}

	// Whether the logic must be decoupled from rendering: whenever the render rate CAN exceed the tick
	// rate - a cap above it, or no cap at all. Coupled, the simulation would tick once per rendered
	// frame and run at render speed.
	inline bool needsLogicTimeScale(int cap, int tickRate)
	{
		return cap <= 0 || cap > tickRate;
	}

	// A solo start's game speed (the skirmish slider, a replay's recorded speed, a restart). Below the
	// tick rate it slows the match; at or above it (the slider's "--" end posts 1000) it is normal speed.
	inline int soloStartLogicTimeScaleFps(int gameSpeedFps, int tickRate)
	{
		return (gameSpeedFps > 0 && gameSpeedFps < tickRate) ? gameSpeedFps : tickRate;
	}
}
