/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

#include "Common/FrameRateLimit.h"
#include "Common/CatchUpPolicy.h"


// TheSuperHackers @todo Use unsigned integers for fps values
// TheSuperHackers @todo Consolidate the GlobalData::m_useFpsLimit and FramePacer::m_enableFpsLimit
// TheSuperHackers @todo Implement new fast forward in here (GeneralsX: setFastForward, 30/09/2026)
class FramePacer
{
public:

	typedef UnsignedInt LogicTimeQueryFlags;
	enum LogicTimeQueryFlags_ CPP_11(: LogicTimeQueryFlags)
	{
		IgnoreFrozenTime = 1<<0, ///< Ignore frozen time for the query
		IgnoreHaltedGame = 1<<1, ///< Ignore halted game for the query
	};

	FramePacer();
	~FramePacer();

	void update(); ///< Signal that the app/render update is done and wait for the fps limit if applicable.
	// GeneralsX: The main loop's form (GameEngine::execute, HeadlessMatch): in a network
	// game, while it holds for the cap, betweenDraws - GameEngine::updateBetweenDraws, a network and logic pass
	// with no draw - runs at NetworkPollFps. The loop ticks the logic at most once per pass and there is no
	// network thread, so a cap at the tick rate lost ~1 tick in 60 in a match (W7 lockstep runs). The cap still
	// caps the draws. A separate overload so the load screen's update() never runs a logic pass.
	void update(Bool (*betweenDraws)());
	enum { NetworkPollFps = 8 * LOGICFRAMES_PER_SECOND }; ///< the old network floor's rate, now a poll rate only
	void reset(); ///< Move the frame timing anchor to now and predict the next update time from the target frame rate. Call after a long blocking operation so its duration does not leak into the next frame delta.

	void setFramesPerSecondLimit( Int fps ); ///< Set the update fps limit.
	Int  getFramesPerSecondLimit() const; ///< Get the update fps limit.
	void enableFramesPerSecondLimit( Bool enable ); ///< Enable or disable the update fps limit.
	Bool isFramesPerSecondLimitEnabled() const; ///< Returns whether the fps limit is enabled here.
	Bool isActualFramesPerSecondLimitEnabled() const; ///< Returns whether the fps limit is actually enabled when considering all game settings and setups.
	Int  getActualFramesPerSecondLimit() const; // Get the actual update fps limit.
	Bool isScriptFastForward() const; ///< GeneralsX: SET_VISUAL_SPEED_MULTIPLIER above 1 or the debug fast-forward - lifts the cap, and the logic runs with the loop.

	Real getUpdateTime() const; ///< Get the last update delta time in seconds.
	Real getUpdateFps() const; ///< Get the last update fps.

	// GeneralsX: Exposed so a probe can separate "my own limiter held
	// me" from "I am blocked on a peer's input". getUpdateTime() cannot: it reports the whole
	// iteration and reads the same whether the frame was slow or the limiter slept.
	Int64 getTotalHoldNs() const { return m_frameRateLimit.getTotalHoldNs(); } ///< Cumulative ns spent limiting. Baseline it and diff; see FrameRateLimit.h.
	Real getBaseOverUpdateFpsRatio(Real minUpdateFps = 5.0f); ///< Get the last engine base over update fps ratio. Used to scale user inputs to a frame rate independent speed.

	void setTimeFrozen(Bool frozen); ///< Set time frozen. Allows scripted camera movement.
	void setGameHalted(Bool halted); ///< Set game halted. Does not allow scripted camera movement.
	Bool isTimeFrozen() const;
	Bool isGameHalted() const;

	void setLogicTimeScaleFps( Int fps ); ///< Set the logic time scale fps and therefore scale the simulation time. Is capped by the max render fps and does not apply to network matches.
	Int  getLogicTimeScaleFps() const; ///< Get the raw logic time scale fps value.
	void enableLogicTimeScale( Bool enable ); ///< Enable or disable the logic time scale setup. If disabled, the simulation time scale is bound to the render frame time or network update time.
	Bool isLogicTimeScaleEnabled() const; ///< Check whether the logic time scale setup is enabled.
	Int  getActualLogicTimeScaleFps(LogicTimeQueryFlags flags = 0) const; ///< Get the real logic time scale fps, depending on the max render fps, network state and enabled state.
	Real getActualLogicTimeScaleRatio(LogicTimeQueryFlags flags = 0) const; ///< Get the real logic time scale ratio, depending on the max render fps, network state and enabled state.
	Real getActualLogicTimeScaleOverFpsRatio(LogicTimeQueryFlags flags = 0) const; ///< Get the real logic time scale over render fps ratio, used to scale down steps in render updates to match logic updates.
	Real getLogicTimeStepSeconds(LogicTimeQueryFlags flags = 0) const; ///< Get the logic time step in seconds
	Real getLogicTimeStepMilliseconds(LogicTimeQueryFlags flags = 0) const; ///< Get the logic time step in milliseconds

	// GeneralsX: Fast forward: the match at 2, 3, 4, 5 or 10 times its normal tick rate.
	// LOCAL PACING ONLY - nothing here is simulation state; see FramePacer.cpp. Who may turn it on (a match whose only
	// human is this player) is the caller's business: changeFastForward, CommandXlat.cpp.
	enum { FastForwardDrawFps = 60 }; ///< draws per second at the target rate while fast forward is on; the other iterations only tick
	void setFastForward( Int speed ); ///< 1 = off; clamped to 1..10
	Int  getFastForward() const; ///< the requested speed
	Bool isFastForwardActive() const; ///< a speed above 1 and a match that is not paused
	Int  getFastForwardTickFps() const; ///< the tick rate fast forward runs the loop at: the speed times the normal rate
	Bool isFastForwardDrawDue(); ///< should this iteration draw? Always TRUE unless fast forward is active
	Real getFastForwardAchieved() const; ///< ticks over the last whole second / the normal rate; 0 until measured

	// GeneralsX: -maxspeed (CommandLine.cpp): a -headless match with no human runs as fast as the
	// CPU allows. Set from the command line before any pacer exists, hence static; Network::timeForNewFrame applies it.
	// GeneralsX: Tick catch-up (Common/CatchUpPolicy.h): a network match whose draws are slow keeps
	// its tick rate by skipping draws the pacer is owed, never by running two ticks per client update.
	void enableCatchUp( Bool enable ) { m_catchUpEnabled = enable; } ///< W3DDisplay::init (Zero Hour) unless GX_CATCHUP=0
	Bool isCatchUpEnabled() const { return m_catchUpEnabled; } ///< also lets Network::timeForNewFrame carry CatchUpDebtFrames
	Bool isCatchUpDrawSkip(); ///< once per draw: should this iteration skip its draw and run a logic-only pass?
	UnsignedInt getCatchUpPasses() const { return m_catchUpPasses; } ///< logic-only catch-up passes so far ([GXPACE] cu=)
	Real getCatchUpPassFraction() const { return m_catchUpPassFraction; } ///< recent share of iterations that skipped their draw
	// GeneralsX: ...and single player, skirmish and replays (the regular gate, no network): the gate
	// keeps CatchUpPolicy::regularGate's state and reports it here after each pass; the draw reads it.
	Bool isRegularCatchUpEligible() const; ///< the regular gate may keep debt and the draw may skip to repay it: see FramePacer.cpp
	Int getRegularCatchUpRate() const; ///< min(logic time scale, render cap); 0 = none
	void noteRegularCarry( Real carry, Real kept ) { m_regularCarry = carry; m_regularKept = kept; m_updatesSinceRegularGate = 0; } ///< GameEngine::canUpdateRegularGameLogic, every pass

	static void setHeadlessMaxSpeed( Bool on ) { s_headlessMaxSpeed = on; }
	static Bool isHeadlessMaxSpeed() { return s_headlessMaxSpeed; }

protected:

	void sampleFastForward();

	FrameRateLimit m_frameRateLimit;

	Int m_maxFPS; ///< Maximum frames per second for rendering
	Int m_logicTimeScaleFPS; ///< Maximum frames per second for logic time scale

	Real m_updateTime; ///< Last update delta time in seconds

	Bool m_enableFpsLimit;
	Bool m_enableLogicTimeScale;
	Bool m_isTimeFrozen;
	Bool m_isGameHalted;

	static Bool s_headlessMaxSpeed;
	Int m_fastForward;
	Int64 m_ffNextDrawNs;	///< the last fast-forward draw (isFastForwardDrawDue)
	Int m_ffItersSinceDraw;
	Int m_ffDrawCredit;	///< FastForwardDrawFps per iteration, a draw costs the tick rate: an exact fractional cadence
	Int64 m_ffWindowStartNs;
	UnsignedInt m_ffWindowStartFrame;
	Real m_ffAchieved;

	Bool m_catchUpEnabled;
	Bool m_catchUpPass;	///< this iteration skipped its draw for catch-up: FramePacer::update does not hold it
	Bool m_lastIterationCatchUpPass;	///< the iteration FramePacer::update last measured was a pass that ticked a whole frame
	UnsignedInt m_catchUpPassFrame;	///< the logic frame when this iteration's pass was decided
	CatchUpPolicy::Tracker m_catchUp;
	UnsignedInt m_catchUpPasses;
	Real m_catchUpPassFraction;
	Real m_regularCarry;	///< the regular gate's carry after its last pass (seconds)
	Real m_regularKept;	///< the catch-up period of that pass, 0 if it was not a catch-up pass (CatchUpPolicy::regularGate)
	UnsignedInt m_updatesSinceRegularGate;	///< update() calls since the gate's last pass, saturating at 2: catch-up needs the gate on every iteration
};

extern FramePacer* TheFramePacer;
