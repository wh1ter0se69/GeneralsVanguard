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

// GeneralsX @bugfix Claude 08/10/2026 The scripted camera's clock.
//
// A scripted camera move ends by CAMERA_MOVEMENT_FINISHED, a script condition, and GameEngine::isTimeFrozen reads the same
// answer, so the logic frame a move ends on is simulation state. It steps once per new logic frame, always one logic frame
// long - whatever the draws did: tick catch-up on or off, a skipped draw, fast forward, a pause, a replay (Codex r10 and r10b
// found three ways the earlier per-draw clocks ended a move on another frame).
//
// Step B (GeneralsX 08/10/2026): ScriptedCameraTimeline below is that clock without a pose - which moves run, their step
// counts, the time multiplier and its divider, the camera freeze, a motion blur's logic lifetime - and it is SIMULATION code:
// in the digest (Common/). The simulation's instance (GameLogic/ScriptedCamera.h, TheScriptedCamera) is fed by script actions
// alone and stepped by GameLogic::update, so every peer - rendered, -headless, the standing host, ReplaySimulation - answers
// CAMERA_MOVEMENT_FINISHED alike (a headless peer's ViewDummy answered a constant TRUE). W3DView runs its own instance for its
// pose, fed by every caller (the player's camera keys too), and draws the camera between the last two logic frames' poses.
//
// Header-only and free of engine types so scripts/test/scripted-camera-clock-test.sh runs this exact code.

#pragma once

#include <cmath>

namespace ScriptedCameraClock
{
	// The camera time multiplier (CAMERA_MOD_FINAL_TIME_MULTIPLIER: the game runs k times faster under a scripted move)
	// has W3DDisplay::draw reach the views on one draw in max(1, k - 1), and the logic ticks once per draw meanwhile, so the
	// scripted camera stepped once in max(1, k - 1) logic frames. The same counter, counted on logic frames: TRUE when this
	// logic frame steps the camera. A multiplier of 1 leaves the counter alone, as the draw's does.
	inline bool multiplierStepDue(int multiplier, int &counter)
	{
		if (multiplier <= 1)
			return true;
		if (--counter > 1)
			return false;
		counter = multiplier;
		return true;
	}

	// The logic frames a motion blur IN/OUT answers CAMERA_MOVEMENT_FINISHED with TRUE (W3DView::isCameraMovementFinished):
	// ScreenMotionBlurFilter's count, once per logic frame. setup() starts an OUT mode at maxCount counting down, any other at
	// 0 counting up; up, it ends on reaching maxCount unless it is an IN_AND_OUT with a jump target (setViewFilterPos),
	// which turns there and ends below 1.
	inline int blurLogicFrames(bool inAndOut, bool outOnly, bool zoomTo, int maxCount, int countStep)
	{
		int count = outOnly ? maxCount : 0;
		bool down = outOnly;
		for (int frames = 1; countStep > 0; ++frames)
		{
			if (down)
			{
				count -= countStep;
				if (count < 1)
					return frames;
			}
			else
			{
				count += countStep;
				if (count >= maxCount)
				{
					if (!(inAndOut && zoomTo))
						return frames;
					down = true;
				}
			}
		}
		return 0;
	}

	// The angle drawn between two logic frames: the short way round, exact at both ends.
	inline float lerpAngle(float prev, float cur, float alpha)
	{
		if (alpha <= 0.0f)
			return prev;
		if (alpha >= 1.0f)
			return cur;
		const float twoPi = 6.28318530718f;
		float delta = cur - prev;
		while (delta > 0.5f * twoPi)
			delta -= twoPi;
		while (delta < -0.5f * twoPi)
			delta += twoPi;
		return prev + delta * alpha;
	}

	// ScreenMotionBlurFilter's count (W3DShaderManager.h uses these): MAX_COUNT, COUNT_STEP. Here because a motion blur IN/OUT
	// answers CAMERA_MOVEMENT_FINISHED for blurLogicFrames(...) of them.
	enum { BLUR_MAX_COUNT = 60, BLUR_COUNT_STEP = 5 };

	// A script's time multiplier, bounded: an interpolation between INT_MAX-sized ones leaves int range, and that float -> int
	// conversion differs by CPU (arm64 saturates, x64 gives INT_MIN: Codex camera-sim-B r3 P2), and a divider of thousands of
	// frames is no camera move anyone scripts. Symmetric, so every value <= 1 still means "no speed-up", as before.
	enum { MAX_TIME_MULTIPLIER = 1000 };
	inline int clampTimeMultiplier(int m)
	{
		return m > MAX_TIME_MULTIPLIER ? MAX_TIME_MULTIPLIER : (m < -MAX_TIME_MULTIPLIER ? -MAX_TIME_MULTIPLIER : m);
	}

	// A float's int value, the same on every CPU: the C++ conversion (toward zero) in range, saturated beyond it, 0 for NaN. The
	// plain conversion of a NaN or out-of-range float is undefined and differs by CPU (arm64 saturates, x64 gives INT_MIN), and
	// a scripted camera move's numbers are a map's raw floats (Codex camera-sim-B r3/r4). Every float -> int here goes through it.
	inline int toIntSaturate(double x)
	{
		if (!(x == x))
			return 0;
		if (x >= 2147483647.0)
			return 2147483647;
		if (x <= -2147483648.0)
			return -2147483647 - 1;
		return (int)x;
	}

	// REAL_TO_INT_FLOOR(0.5 + x) (Lib/BaseType.h: floorf of the sum rounded to a float), the multiplier's rounding
	inline int floorHalfUp(float x)
	{
		return toIntSaturate(std::floor((float)(0.5 + x)));
	}

	// ParabolicEase's arithmetic (GameClient/ParabolicEase.cpp calls this): an ease-in/ease-out of t in [0, 1], parabolic at
	// both ends, linear between. The time multiplier follows it, so it is simulation code. Out-of-range inputs are clamped;
	// the return says whether any was (ParabolicEase reports that in debug builds).
	struct Ease
	{
		float in = 0.0f;
		float out = 1.0f;

		bool setEaseTimes(float easeInTime, float easeOutTime)
		{
			// a NaN (a map's raw float) clamps like a negative: every comparison with it is false (Codex camera-sim-B r4 P2)
			bool ok = true;
			in = easeInTime;
			if (!(in >= 0.0f && in <= 1.0f)) {
				ok = false;
				in = (in > 1.0f) ? 1.0f : 0.0f;
			}
			out = 1.0f - easeOutTime;
			if (!(out >= 0.0f && out <= 1.0f)) {
				ok = false;
				out = (out > 1.0f) ? 1.0f : 0.0f;
			}
			if (in > out)
				in = out;	// overlapping (ParabolicEase asserts beyond FLT_EPSILON)
			return ok;
		}

		float operator()(float param) const
		{
			if (!(param >= 0.0f))
				param = 0.0f;
			else if (param > 1.0f)
				param = 1.0f;
			const float v0 = 1.0f + out - in;
			if (param < in)	// "param < in" avoids a division by zero at param = 0.0f and in = 0.0f
				return param*param/(v0*in);
			else if (param <= out)	// "param <= out" avoids a division by zero when param = 1.0f and out = 1.0f
				return (in + 2.0*(param - in))/v0;
			else
				return (in + 2.0*(out - in) + (2.0*(param - out) + out*out - param*param)/(1.0f - out))/v0;
		}
	};
}

// GeneralsX @bugfix Claude 08/10/2026 Step B: the timing of scripted camera moves, with no pose (see the top of this file).
// Every rule is W3DView's (its setup and OneFrame functions, A's logic-frame clock), except where W3DView's answer read the
// camera: a path ends on its duration alone (W3DView also ended it when the eased distance reached the last waypoint - the
// first segment runs from the camera), a path's time multiplier follows its eased time (W3DView: the eased distance through
// per-waypoint values), and a rotation toward a position always runs (W3DView refused within 0.1 of the camera). The camera
// starts where each peer's own camera is, so none of those may decide what a script reads.
class ScriptedCameraTimeline
{
public:
	enum	// the moves (W3DView's Scripted_*)
	{
		ROTATE = 1<<0,
		PITCH = 1<<1,
		ZOOM = 1<<2,
		PATH = 1<<3,
	};
	enum	// what a step did, for the view's pose
	{
		STEPPED_ROTATE = 1<<0,
		STEPPED_PITCH = 1<<1,
		STEPPED_ZOOM = 1<<2,
		STEPPED_PATH = 1<<3,
		ENDED_ROTATE = 1<<4,
		ENDED_PITCH = 1<<5,
		ENDED_ZOOM = 1<<6,
		ENDED_PATH = 1<<7,
		BLUR_TURN = 1<<8,	///< a blur jump's turn: it ended the rotation and the path (the view jumps to its target)
		BLUR_ENDED = 1<<9,	///< a blur's logic lifetime ran out on this step
		DUE = 1<<10,		///< the time multiplier's divider let this logic frame step the camera
	};

	struct Counter	///< a rotation, a zoom or a pitch: frames to go, frames held after them (a rotation toward an object)
	{
		int cur = 0;
		int num = 0;
		int hold = 0;
		int startMultiplier = 1;
		int endMultiplier = 1;
		ScriptedCameraClock::Ease ease;
	};
	struct Path
	{
		float total = 0.0f;
		float elapsed = 0.0f;
		int shutter = 1;
		int curShutter = 1;
		int startMultiplier = 1;
		int endMultiplier = 1;
		ScriptedCameraClock::Ease ease;
	};

	ScriptedCameraTimeline() { reset(1000.0f / 30.0f); }

	/// frameMs: one logic frame (MSEC_PER_LOGICFRAME_REAL, which is also TheW3DFrameLengthInMsec)
	void reset(float frameMs)
	{
		m_frameMs = frameMs;
		m_moves = 0;
		m_freeze = false;
		m_multiplier = 1;
		m_multiplierCount = 1;
		m_rotate = Counter();
		m_zoom = Counter();
		m_pitch = Counter();
		m_path = Path();
		m_blurLive = m_blurInAndOut = m_blurOutOnly = m_blurTarget = false;
		m_blurSteps = m_blurLife = m_blurTurn = 0;
	}

	// --- the script calls (W3DView's setup functions, their timing only)

	/// rotateCamera (holdMs 0), rotateCameraTowardObject, rotateCameraTowardPosition: ends a path
	void rotate(int ms, int holdMs, float easeIn, float easeOut)
	{
		if (holdMs < 1)
			holdMs = 0;
		m_rotate.hold = ScriptedCameraClock::toIntSaturate(holdMs/m_frameMs);
		if (ms < 1)
			ms = 1;
		m_rotate.num = ScriptedCameraClock::toIntSaturate(ms/m_frameMs);
		if (m_rotate.num < 1)
			m_rotate.num = 1;
		m_rotate.cur = 0;
		m_rotate.startMultiplier = m_rotate.endMultiplier = m_multiplier;
		m_rotate.ease.setEaseTimes(easeIn/ms, easeOut/ms);
		m_moves = (m_moves | ROTATE) & ~PATH;
	}
	void zoom(int ms) { start(m_zoom, ms, ZOOM); }	///< zoomCamera
	void pitch(int ms) { start(m_pitch, ms, PITCH); }	///< pitchCamera

	/// moveCameraTo (shutterFrames 1), moveCameraAlongWaypointPath (its shutter ms / frameMs): ends a rotation
	void path(int ms, int shutterFrames, float easeIn, float easeOut)
	{
		if (ms < 1)
			ms = 1;
		m_path.total = (float)ms;
		m_path.elapsed = 0.0f;
		m_path.shutter = m_path.curShutter = shutterFrames < 1 ? 1 : shutterFrames;
		m_path.startMultiplier = m_path.endMultiplier = m_multiplier;
		m_path.ease.setEaseTimes(easeIn/ms, easeOut/ms);
		m_moves = (m_moves | PATH) & ~ROTATE;
	}

	/// cameraModFinalZoom / cameraModFinalPitch: over what is left of the rotation, then of the path (the later wins). The
	/// view passed those Real milliseconds to an Int parameter.
	void finalZoom() { finalOver(m_zoom, ZOOM); }
	void finalPitch() { finalOver(m_pitch, PITCH); }

	/// cameraModFinalTimeMultiplier: the multiplier a rotation or a path ends at, else the multiplier now
	void finalTimeMultiplier(int multiplier)
	{
		multiplier = ScriptedCameraClock::clampTimeMultiplier(multiplier);
		if (m_moves & ROTATE)
			m_rotate.endMultiplier = multiplier;
		else if (m_moves & PATH)
			m_path.endMultiplier = multiplier;
		else
			m_multiplier = multiplier;
	}
	void setTimeMultiplier(int multiplier) { m_multiplier = ScriptedCameraClock::clampTimeMultiplier(multiplier); }
	void freezeTime() { m_freeze = true; }	///< cameraModFreezeTime: time stands while the next move runs

	void end(unsigned moves) { m_moves &= ~moves; }	///< end these moves where they are (no end pose, the freeze stays)
	void stopAll() { end(ROTATE | PITCH | ZOOM | PATH); }	///< stopDoingScriptedCamera (setAngle, setPitch, setZoom...)
	void endRotateAndPath() { end(ROTATE | PATH); }	///< lookAt
	void endPath() { end(PATH); }	///< a camera lock, on every logic update that runs scripts

	/// A filter or mode was set (W3DView::noteViewFilterForCameraClock): live = the motion blur filter in an IN/OUT mode. It
	/// starts the blur's logic lifetime over; anything else ends it.
	void blur(bool live, bool inAndOut, bool outOnly)
	{
		m_blurLive = live;
		m_blurInAndOut = inAndOut;
		m_blurOutOnly = outOnly;
		m_blurSteps = 0;
		m_blurLife = live ? blurFrames(m_blurTarget) : 0;
		m_blurTurn = (live && inAndOut) ? ScriptedCameraClock::blurLogicFrames(false, false, false,
			ScriptedCameraClock::BLUR_MAX_COUNT, ScriptedCameraClock::BLUR_COUNT_STEP) : 0;
	}
	/// setViewFilterPos: an IN_AND_OUT turns there (and jumps) instead of ending
	void blurTarget()
	{
		m_blurTarget = true;
		if (m_blurLive)
			m_blurLife = blurFrames(true);
	}

	// --- the steps

	/// One step of every running move: zoom, pitch, then the rotation or the path (W3DView::stepScriptedCameraOnce's order).
	/// pathMs: the path's step, one logic frame (the view's frozen-time draw passes its slice).
	unsigned stepOnce(float pathMs)
	{
		unsigned did = 0;
		if (m_moves & ZOOM)
			did |= stepCounter(m_zoom, ZOOM, STEPPED_ZOOM, ENDED_ZOOM);
		if (m_moves & PITCH)
			did |= stepCounter(m_pitch, PITCH, STEPPED_PITCH, ENDED_PITCH);
		if (m_moves & ROTATE)
		{
			did |= STEPPED_ROTATE;
			++m_rotate.cur;
			if (m_rotate.cur < m_rotate.num)
				m_multiplier = m_rotate.startMultiplier + ScriptedCameraClock::floorHalfUp(
					(m_rotate.endMultiplier - m_rotate.startMultiplier)*m_rotate.ease(((float)m_rotate.cur)/m_rotate.num));
			else if (m_rotate.cur == m_rotate.num)
				m_multiplier = m_rotate.endMultiplier;	// ease(1) is 1 to a few ulps: the end, exactly (runToEnd's too)
			if (m_rotate.cur >= m_rotate.num + m_rotate.hold)
			{
				m_moves &= ~ROTATE;
				m_freeze = false;
				did |= ENDED_ROTATE;
			}
		}
		else if (m_moves & PATH)
		{
			did |= STEPPED_PATH;
			const float before = m_path.elapsed;
			m_path.elapsed += pathMs;
			// ...or stops growing under a real step: past ~2^28 ms a float sum of 16.7 ms steps rounds back to itself, and a path of
			// days never ended - the rendered freeze loop (W3DDisplay::draw) never returned (Codex camera-sim-B r5 P2). A zero step
			// (the halted draw's slice) is no stall.
			if (m_path.elapsed > m_path.total || (pathMs > 0.0f && m_path.elapsed == before))
			{
				m_moves &= ~PATH;
				m_freeze = false;
				// its final multiplier, however the steps fell: W3DView kept the last one it sampled, which depends on the step's
				// length - a frozen path run out at frameMs here and at the draw's slice in the view kept different ones
				// (Codex camera-sim-B r2 P2). A rotation's last sample (cur == num) is its end multiplier already.
				m_multiplier = m_path.endMultiplier;
				did |= ENDED_PATH;
			}
			else if (--m_path.curShutter <= 0)
			{
				m_path.curShutter = m_path.shutter;
				const float f1 = 1.0f - m_path.ease(m_path.elapsed/m_path.total);
				const float f2 = 1.0f - f1;
				m_multiplier = ScriptedCameraClock::floorHalfUp(m_path.startMultiplier*f1 + m_path.endMultiplier*f2);
			}
		}
		return did;
	}

	/// The step of a new logic frame: the divider (every logic frame counts), the blur's count and its turn, then one step.
	/// A camera lock's end of a path comes first, from the caller (it runs on every logic update that runs scripts).
	unsigned stepLogicFrame()
	{
		const unsigned did = beginLogicFrame();
		return (did & DUE) ? (did | stepOnce(m_frameMs)) : did;
	}
	/// ...its first half, for a caller that acts between the two (W3DView jumps to the blur's target, then takes the pose the
	/// step starts from): the divider and the blur's count; DUE when stepOnce(frameMs()) is to follow.
	unsigned beginLogicFrame()
	{
		if (!ScriptedCameraClock::multiplierStepDue(m_multiplier, m_multiplierCount))
			return 0;
		unsigned did = DUE;
		if (m_blurLive && m_blurSteps < m_blurLife)
		{
			++m_blurSteps;
			if (m_blurTarget && m_blurSteps == m_blurTurn)
			{
				endRotateAndPath();	// the jump's lookAt
				did |= BLUR_TURN;
			}
			if (m_blurSteps >= m_blurLife)
			{
				m_blurTarget = false;	// the filter forgets its target as its effect ends
				did |= BLUR_ENDED;
			}
		}
		return did;
	}

	/// A script-frozen logic update (FREEZE_TIME): the scripts run on it, so the camera steps on it, through the divider; a
	/// blur does not count in frozen time.
	unsigned stepScriptFrozen()
	{
		if (!ScriptedCameraClock::multiplierStepDue(m_multiplier, m_multiplierCount))
			return 0;
		return DUE | stepOnce(m_frameMs);
	}

	/// A camera-only freeze: no logic frame passes until its moves end, and no script runs meanwhile, so they end at once - in
	/// the state stepping them out leaves (a rotation's or a path's end multiplier, its freeze cleared), without the steps: a
	/// path of days stepped 16.7 ms at a time never ends once its float elapsed time stops growing (2^29 ms: Codex
	/// camera-sim-B r3 P2), and this loop would never return.
	void runToEnd()
	{
		if (m_moves & ROTATE)
		{
			m_multiplier = m_rotate.endMultiplier;
			m_freeze = false;
		}
		else if (m_moves & PATH)
		{
			m_multiplier = m_path.endMultiplier;
			m_freeze = false;
		}
		m_moves = 0;
	}

	// --- the answers

	/// CAMERA_MOVEMENT_FINISHED: no move runs, or a motion blur IN/OUT answers TRUE for its logic lifetime
	bool isFinished() const
	{
		if (m_blurLive && m_blurSteps < m_blurLife)
			return true;
		return m_moves == 0;
	}
	bool isTimeFrozen() const { return m_freeze; }	///< the freeze flag; the freeze holds while !isFinished()
	int timeMultiplier() const { return m_multiplier; }
	unsigned moves() const { return m_moves; }
	float frameMs() const { return m_frameMs; }
	const Counter &rotation() const { return m_rotate; }
	const Counter &zooming() const { return m_zoom; }
	const Counter &pitching() const { return m_pitch; }
	const Path &waypointPath() const { return m_path; }
	/// the milliseconds left of the rotation (its hold included) and of the path, as the Int a zoom or pitch over them takes
	/// (cameraModFinalZoom/Pitch, here and in W3DView): a rotation and a hold of up to INT_MAX ms each leave more than an int
	int rotationMsLeft() const
	{
		// W3DView's float product (an Int count times the Real frame), saturated: num and hold are each below 2^27
		return ScriptedCameraClock::toIntSaturate((float)(m_rotate.num + m_rotate.hold - m_rotate.cur)*m_frameMs);
	}
	int pathMsLeft() const { return ScriptedCameraClock::toIntSaturate(m_path.total - m_path.elapsed); }

private:
	void start(Counter &c, int ms, unsigned move)
	{
		if (ms < 1)
			ms = 1;
		c.num = ScriptedCameraClock::toIntSaturate(ms/m_frameMs);
		if (c.num < 1)
			c.num = 1;
		c.cur = 0;
		m_moves |= move;
	}
	void finalOver(Counter &c, unsigned move)
	{
		if (m_moves & ROTATE)
			start(c, rotationMsLeft(), move);
		if (m_moves & PATH)
			start(c, pathMsLeft(), move);
	}
	unsigned stepCounter(Counter &c, unsigned move, unsigned stepped, unsigned ended)
	{
		if (++c.cur >= c.num)
		{
			m_moves &= ~move;
			return stepped | ended;
		}
		return stepped;
	}
	int blurFrames(bool target) const
	{
		return ScriptedCameraClock::blurLogicFrames(m_blurInAndOut, m_blurOutOnly, target,
			ScriptedCameraClock::BLUR_MAX_COUNT, ScriptedCameraClock::BLUR_COUNT_STEP);
	}

	float m_frameMs;
	unsigned m_moves;
	bool m_freeze;
	int m_multiplier;
	int m_multiplierCount;		///< multiplierStepDue's counter
	Counter m_rotate;
	Counter m_zoom;
	Counter m_pitch;
	Path m_path;
	bool m_blurLive;
	bool m_blurInAndOut;
	bool m_blurOutOnly;
	bool m_blurTarget;			///< setViewFilterPos gave it a jump target (ScreenMotionBlurFilter's m_zoomToValid)
	int m_blurSteps;
	int m_blurLife;
	int m_blurTurn;
};
