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

// GeneralsX: ONE max-FPS setting, and it is exactly what it says.
//
//
//
// Before this, the render cap was chosen in six places and the limiter switched in five more: the
// retail INI's FramesPerSecondLimit = 30 was the default, Options.ini "FPSLimit" yes/no was a second
// setting beside the launcher's number, every detail preset and every solo start re-forced the limiter,
// the skirmish game-speed slider REPLACED the cap for the match (a launcher 144 became 1000 at the
// slider's right end), a map script could set it, and the pacer lifted any cap under 120 to 480 in a
// network game. The HUD bracket then printed the most frames drawn in a second, so nobody could see
// the limit at all.
//
// Now the cap is resolved ONCE, in GameEngine::init (resolveCap), and nothing else writes it:
//   * the launcher's value, which reaches the engine only as argv: -fps N, or -noFPSLimit for 0;
//   * else the refresh rate of the display the game window is on, read by the device layer
//     (GameEngine::getDisplayRefreshRate - SDL3 on macOS, Linux and iOS, Win32 on Windows);
//   * else the tick rate.
// 0 means unlimited. The skirmish slider is a GAME SPEED again - the logic time scale - and never the
// render cap. Options.ini "FPSLimit" is no longer a setting: a legacy "FPSLimit = no" (the old launcher's
// "uncapped") reads as 0 - in the launcher, and in GameEngine::init when no -fps / -noFPSLimit was given.
//
// The ONE floor that remains is the tick rate, and it is a floor on the main loop, not a rewrite of
// the setting: the engine advances at most one logic frame per main-loop iteration, so a loop slower
// than LOGICFRAMES_PER_SECOND would slow the simulation itself (FramePacer::getActualFramesPerSecondLimit).
// The launcher offers nothing below it; only a hand-typed -fps or a sub-60 Hz display reaches it.
//
// HEADER ONLY AND ENGINE-FREE, so the decisions are testable without the engine:
// scripts/test/fps-limit-policy-test.sh splices the real call sites out of a tree and drives them.

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
