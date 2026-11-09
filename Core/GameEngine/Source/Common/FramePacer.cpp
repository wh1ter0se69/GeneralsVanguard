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
#include "PreRTS.h"

#include "Common/FramePacer.h"

#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"

#include <chrono>


FramePacer* TheFramePacer = nullptr;
Bool FramePacer::s_headlessMaxSpeed = FALSE;

FramePacer::FramePacer()
{
	// GeneralsX @bugfix BenderAI 11/05/2026 Protect Windows-specific timer API calls
#ifdef _WIN32
	// Set the time slice size to 1 ms.
	timeBeginPeriod(1);
#endif

	m_maxFPS = BaseFps;
	m_logicTimeScaleFPS = LOGICFRAMES_PER_SECOND;
	m_updateTime = 1.0f / (Real)BaseFps; // initialized to something to avoid division by zero on first use
	m_enableFpsLimit = FALSE;
	m_enableLogicTimeScale = FALSE;
	m_isTimeFrozen = FALSE;
	m_isGameHalted = FALSE;
	m_fastForward = 1;
	m_ffNextDrawNs = 0;
	m_ffItersSinceDraw = 0;
	m_ffDrawCredit = 0;
	m_ffWindowStartNs = 0;
	m_ffWindowStartFrame = 0;
	m_ffAchieved = 0.0f;
	m_catchUpEnabled = FALSE;
	m_catchUpPass = FALSE;
	m_lastIterationCatchUpPass = FALSE;
	m_catchUpPassFrame = 0;
	m_catchUpPasses = 0;
	m_catchUpPassFraction = 0.0f;
	m_regularCarry = 0.0f;
	m_regularKept = 0.0f;
	m_updatesSinceRegularGate = 2;
}

FramePacer::~FramePacer()
{
	// GeneralsX @bugfix BenderAI 11/05/2026 Protect Windows-specific timer API calls
#ifdef _WIN32
	// Restore the previous time slice for Windows.
	timeEndPeriod(1);
#endif
}

void FramePacer::update()
{
	// GeneralsX @feature Claude 07/10/2026 a loop iteration without the regular gate (isRegularCatchUpEligible)
	if (m_updatesSinceRegularGate < 2)
		++m_updatesSinceRegularGate;
	// TheSuperHackers @bugfix xezon 05/08/2025 Re-implements the frame rate limiter
	// with higher resolution counters to cap the frame rate more accurately to the desired limit.
	const UnsignedInt maxFps = getActualFramesPerSecondLimit();// allowFpsLimit ? getFramesPerSecondLimit() : RenderFpsPreset::UncappedFpsValue;
	m_updateTime = m_frameRateLimit.wait(maxFps);
}

void FramePacer::update(Bool (*betweenDraws)())
{
	// GeneralsX @feature Claude 07/10/2026 a loop iteration without the regular gate (isRegularCatchUpEligible)
	if (m_updatesSinceRegularGate < 2)
		++m_updatesSinceRegularGate;
	// GeneralsX @feature Claude 02/10/2026 Tick catch-up: a logic-only pass drew nothing, and the cap is a cap on DRAWS, so
	// it is not held. The uncapped wait holds nothing and re-seeds the grid, so the next draw is paced from here. Held to
	// the cap instead, a pass at a 60 cap would last a whole tick period and could never repay the draw it skipped.
	// ...and only if it ticked: a pass whose frame did not come (rare since framesOwed) moved no game time (review r2 P3).
	m_lastIterationCatchUpPass = m_catchUpPass && TheGameLogic != nullptr && TheGameLogic->getFrame() != m_catchUpPassFrame;
	if (m_catchUpPass)
	{
		m_catchUpPass = FALSE;
		m_updateTime = m_frameRateLimit.wait(RenderFpsPreset::UncappedFpsValue);
		return;
	}
	const UnsignedInt maxFps = getActualFramesPerSecondLimit();
	const UnsignedInt pollFps = (TheNetwork != nullptr) ? (UnsignedInt)NetworkPollFps : 0;
	// Fast forward: the loop is the logic's pacer, so a long iteration (a draw) is repaid by catch-up iterations
	// rather than lost - up to ~100 ms behind, past which the machine simply cannot keep the speed (FrameRateLimit::wait).
	const UnsignedInt debtPeriods = isFastForwardActive() ? max(1u, maxFps / 10) : 1u;
	m_updateTime = m_frameRateLimit.wait(maxFps, pollFps, betweenDraws, debtPeriods);
	sampleFastForward();
}

void FramePacer::reset()
{
	m_frameRateLimit.reset();
	m_updateTime = 1.0f / (Real)getActualFramesPerSecondLimit();
}

void FramePacer::setFramesPerSecondLimit( Int fps )
{
	DEBUG_LOG(("FramePacer::setFramesPerSecondLimit() - setting max fps to %d (TheGlobalData->m_useFpsLimit == %d)", fps, TheGlobalData->m_useFpsLimit));
	// GeneralsX @feature Claude 27/09/2026 0 is "unlimited" (Common/FpsLimitPolicy.h), stored as the
	// ladder's own Uncapped entry so every reader that already knows that value - the max-FPS hotkeys,
	// the Debug screen, changeLogicTimeScale - keeps working without learning a second spelling.
	m_maxFPS = (fps <= 0 || fps > (Int)RenderFpsPreset::UncappedFpsValue) ? (Int)RenderFpsPreset::UncappedFpsValue : fps;
}

Int FramePacer::getFramesPerSecondLimit()  const
{
	return m_maxFPS;
}

void FramePacer::enableFramesPerSecondLimit( Bool enable )
{
	m_enableFpsLimit = enable;
}

Bool FramePacer::isFramesPerSecondLimitEnabled() const
{
	return m_enableFpsLimit;
}

Bool FramePacer::isActualFramesPerSecondLimitEnabled() const
{
	Bool allowFpsLimit = true;

	allowFpsLimit &= !isScriptFastForward();

	if (TheGameLogic != nullptr)
	{
#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode);
#else	//always allow this cheat key if we're in a replay game.
		allowFpsLimit &= !(!TheGameLogic->isGamePaused() && TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame());
#endif
	}

	allowFpsLimit &= TheGlobalData->m_useFpsLimit;
	allowFpsLimit &= isFramesPerSecondLimitEnabled();

	return allowFpsLimit;
}

// GeneralsX @bugfix Claude 27/09/2026 One definition for the two readers: the cap above, lifted, and
// GameEngine::canUpdateRegularGameLogic, which then steps the logic with the loop as retail's coupled logic
// did. With the logic time scale on it held the logic at the tick rate, so a campaign's scripted fast-forward
// did nothing - in every Zero Hour game, and in base Generals whenever the cap exceeds the tick rate (Codex W7-r8).
Bool FramePacer::isScriptFastForward() const
{
	return (TheTacticalView != nullptr && TheTacticalView->getTimeMultiplier() > 1)
		|| (TheScriptEngine != nullptr && TheScriptEngine->isTimeFast());
}

Int FramePacer::getActualFramesPerSecondLimit() const
{
	// Fast forward: the loop runs at the tick rate it wants, whatever the player's cap or limiter switch - one tick per
	// iteration (GameEngine::canUpdateRegularGameLogic steps with the loop at a scale >= this cap, and Network::
	// timeForNewFrame yields to the loop), and the draws are thinned separately (isFastForwardDrawDue).
	if (isFastForwardActive())
		return getFastForwardTickFps();

	if (!isActualFramesPerSecondLimitEnabled())
		return RenderFpsPreset::UncappedFpsValue;

	const Int limit = getFramesPerSecondLimit();
	if (limit >= (Int)RenderFpsPreset::UncappedFpsValue)
		return RenderFpsPreset::UncappedFpsValue;

	// GeneralsX @feature Claude 27/09/2026 THE CAP IS THE CAP (Common/FpsLimitPolicy.h). The one floor
	// left is the tick rate, and it is a floor on the main loop rather than a rewrite of the setting: the
	// engine advances at most one logic frame per main-loop iteration (canUpdateRegularGameLogic steps
	// once per iteration when the scale is at or above the render cap, and canUpdateNetworkGameLogic
	// once per isFrameDataReady), so a loop slower than LOGICFRAMES_PER_SECOND would slow the simulation
	// itself. The launcher offers nothing below it; only a hand-typed -fps, or a display under 60 Hz,
	// reaches it.
	//
	// This used to carry two more floors, both gone. A network game lifted any cap under 2x the tick
	// rate to 8x it (480 at 60 Hz) - "in game it shows 480 for everyone" - to keep the socket polled
	// often, since there is no network thread; and the same 2x was the starvation headroom from before
	// the limiter paced to a deadline grid (FrameRateLimit.cpp, 14/08/2026). The polling is now done
	// WITHOUT drawing: update(betweenDraws) runs network + logic passes at NetworkPollFps while it holds
	// for the cap. Measured without that (W7, two paced headless peers, cap 60): logic 59.0 a second
	// against 60.0 at 480 and unlimited, with identical CRC streams.
	if (limit < (Int)LOGICFRAMES_PER_SECOND)
		return (Int)LOGICFRAMES_PER_SECOND;

	return limit;
}

Real FramePacer::getUpdateTime()  const
{
	return m_updateTime;
}

Real FramePacer::getUpdateFps()  const
{
	return 1.0f / m_updateTime;
}

Real FramePacer::getBaseOverUpdateFpsRatio(Real minUpdateFps)
{
	// Update fps is floored to default 5 fps, 200 ms.
	// Useful to prevent insane ratios on frame spikes/stalls.
	return (Real)BaseFps / std::max(getUpdateFps(), minUpdateFps);
}

void FramePacer::setTimeFrozen(Bool frozen)
{
	m_isTimeFrozen = frozen;
}

void FramePacer::setGameHalted(Bool halted)
{
	m_isGameHalted = halted;
}

Bool FramePacer::isTimeFrozen() const
{
	return m_isTimeFrozen;
}

Bool FramePacer::isGameHalted() const
{
	return m_isGameHalted;
}

void FramePacer::setLogicTimeScaleFps( Int fps )
{
	m_logicTimeScaleFPS = fps;
}

Int FramePacer::getLogicTimeScaleFps() const
{
	return m_logicTimeScaleFPS;
}

void FramePacer::enableLogicTimeScale( Bool enable )
{
	m_enableLogicTimeScale = enable;
}

Bool FramePacer::isLogicTimeScaleEnabled() const
{
	return m_enableLogicTimeScale;
}

Int FramePacer::getActualLogicTimeScaleFps(LogicTimeQueryFlags flags) const
{
	if (m_isTimeFrozen && (flags & IgnoreFrozenTime) == 0)
	{
		return 0;
	}

	if (m_isGameHalted && (flags & IgnoreHaltedGame) == 0)
	{
		return 0;
	}

	// Fast forward, network game or not: the rate the loop now ticks at, which is also what every render-side
	// reader of this (animation time step, interpolation) must scale by.
	if (isFastForwardActive())
	{
		return getFastForwardTickFps();
	}

	if (TheNetwork != nullptr)
	{
		return TheNetwork->getFrameRate();
	}

	if (isLogicTimeScaleEnabled())
	{
		return getLogicTimeScaleFps();
	}

	// Returns uncapped value to align with the render update as per the original game behavior.
	return RenderFpsPreset::UncappedFpsValue;
}

Real FramePacer::getActualLogicTimeScaleRatio(LogicTimeQueryFlags flags) const
{
	return (Real)getActualLogicTimeScaleFps(flags) / LOGICFRAMES_PER_SECONDS_REAL;
}

Real FramePacer::getActualLogicTimeScaleOverFpsRatio(LogicTimeQueryFlags flags) const
{
	// TheSuperHackers @info Clamps ratio to min 1, because the logic
	// frame rate is currently capped by the render frame rate.
	const Real ratio = min(1.0f, (Real)getActualLogicTimeScaleFps(flags) / getUpdateFps());
	// GeneralsX @feature Claude 02/10/2026 Tick catch-up: the last iteration was a logic-only pass, which ticked one whole logic
	// frame in a few ms of wall time. Scaled by its wall time, every render-side clock (WW3D animations, sway, fades, water
	// offsets) would advance ~1/3 of a frame for it, so under catch-up units would animate at ~80% of the speed they move.
	return (m_lastIterationCatchUpPass && ratio > 0.0f) ? 1.0f : ratio;
}

Real FramePacer::getLogicTimeStepSeconds(LogicTimeQueryFlags flags) const
{
	return SECONDS_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}

Real FramePacer::getLogicTimeStepMilliseconds(LogicTimeQueryFlags flags) const
{
	return MSEC_PER_LOGICFRAME_REAL * getActualLogicTimeScaleOverFpsRatio(flags);
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 30/09/2026 FAST FORWARD - the whole match at k times its normal tick rate, for a match whose
// only human is this player (wh1ter0se, 30/09: "sandboxed with 1 player as viewer and rest AIs ... 2x 3x 4x x5 10x").
//
// IT IS PACING, NOT SIMULATION. The engine's invariant stays exactly as it is: at most one logic tick per main-loop
// iteration, i.e. per GameClient::update - which refreshes shroud state the logic reads back (GameEngine::
// updateBetweenDraws, Codex W7-r4), so a second tick per client update would change the simulation. Fast forward only
// makes the loop iterate k times as often and stops most of those iterations from drawing. The tick sequence is the
// 1x one by construction, which is why a replay of a 10x match plays back at 1x with the same CRCs and why nothing here
// needs to be recorded, broadcast or CRC'd.
//
// How the rate is reached: getActualFramesPerSecondLimit returns the fast-forward tick rate, and the logic steps
// with the loop (a solo game: the scale reported below is >= that cap, so canUpdateRegularGameLogic ticks every
// iteration; a network game: Network::timeForNewFrame yields to the loop). Draws are cut to FastForwardDrawFps
// (W3DDisplay::draw); a draw costs several tick periods and the limiter's catch-up debt (update above) repays them.
// Above what the CPU can simulate the speed is simply lower - getFastForwardAchieved says by how much.
// ------------------------------------------------------------------------------------------------
static Int64 gxSteadyNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FramePacer::setFastForward( Int speed )
{
	m_fastForward = max(1, min(speed, 10));
	m_ffWindowStartNs = 0;	// a new window, so a reading never mixes two speeds
	m_ffAchieved = 0.0f;
}

Int FramePacer::getFastForward() const
{
	return m_fastForward;
}

Bool FramePacer::isFastForwardActive() const
{
	// Not while a scripted freeze holds the simulation: the loop would run at k x the tick rate and, with the draw thinning
	// off during a freeze, render every iteration for nothing (Codex ffwd-r12). NOT on m_isGameHalted: in a network game
	// that is "the network is stalling" (GameEngine::isGameHalted), which a solo match sees for a moment whenever a frame's
	// commands are not ready yet - dropping the speed then re-anchors the network pacer at 1x on every such wait
	// (Codex ffwd-r13 P1). A paused game is caught by isGamePaused. A scripted fast forward (SET_VISUAL_SPEED_MULTIPLIER, the
	// debug fast-forward) keeps the authored behaviour - uncapped, logic with the loop - rather than being held to the manual
	// speed (Codex ffwd-r21 P2). The requested speed stays through all of them.
	return m_fastForward > 1 && TheGameLogic != nullptr && !TheGameLogic->isGamePaused() && !m_isTimeFrozen
		&& !isScriptFastForward();
}

Int FramePacer::getFastForwardTickFps() const
{
	// The normal rate: a solo game's own game speed (the skirmish slider, a replay's recorded speed) when its scale is
	// on, else the tick rate - a network game's included, whatever rate it last negotiated for its draws. Deliberately not
	// the negotiated rate (Codex ffwd-r10 asked): in a one-human match that rate falls below the tick rate only when this
	// machine's own draws cannot keep 1x, so k x it would be k x a failure; k x game time is what "10x" means, and the
	// on-screen reading (getFastForwardAchieved) says what the machine really reaches.
	const Int base = (TheNetwork == nullptr && isLogicTimeScaleEnabled()) ? getLogicTimeScaleFps() : (Int)LOGICFRAMES_PER_SECOND;
	return max(1, base) * m_fastForward;
}

Bool FramePacer::isFastForwardDrawDue()
{
	if (!isFastForwardActive())
		return TRUE;
	// Once every k iterations at speed k - FastForwardDrawFps a second at the target rate, fewer when the machine cannot keep
	// it - so every draw is followed by logic-only passes. A time grid alone draws on EVERY iteration once a draw itself takes
	// a period, and fast forward collapses to ~1x on a slow renderer (Codex ffwd-r10 P1). Plus a floor of 10 draws a second
	// for a machine far below the target, never two draws in a row.
	const Int64 now = gxSteadyNs();
	++m_ffItersSinceDraw;
	// Every iteration earns FastForwardDrawFps of credit and a draw costs the tick rate, so draws come FastForwardDrawFps
	// a second at ANY tick rate - a rate that is no multiple of 60 (a 30 Hz build at 3x, a 45 Hz game speed at 2x) draws on
	// alternate 1- and 2-iteration gaps instead of every iteration (Codex ffwd-r14 P2; the real tick rate as in r11).
	const Int tickFps = max(1, getFastForwardTickFps());
	m_ffDrawCredit += (Int)FastForwardDrawFps;
	const Bool due = m_ffDrawCredit >= tickFps
		|| (m_ffItersSinceDraw >= 2 && now - m_ffNextDrawNs >= 100000000LL);
	if (!due)
		return FALSE;
	m_ffDrawCredit = (m_ffDrawCredit >= tickFps) ? m_ffDrawCredit - tickFps : 0;	// a floor draw starts a fresh cadence
	m_ffItersSinceDraw = 0;
	m_ffNextDrawNs = now;	// the last draw's time (the floor measures from it)
	return TRUE;
}

Real FramePacer::getFastForwardAchieved() const
{
	return m_ffAchieved;
}

// Once a second while active: the speed actually reached, and a [GXFF] line for the logs.
void FramePacer::sampleFastForward()
{
	if (!isFastForwardActive())
	{
		m_ffWindowStartNs = 0;
		return;
	}
	const Int64 now = gxSteadyNs();
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (m_ffWindowStartNs == 0 || frame < m_ffWindowStartFrame)
	{
		m_ffWindowStartNs = now;
		m_ffWindowStartFrame = frame;
		return;
	}
	const Int64 span = now - m_ffWindowStartNs;
	if (span < 1000000000LL)
		return;
	const double tps = (double)(frame - m_ffWindowStartFrame) * 1e9 / (double)span;
	m_ffAchieved = (Real)(tps * m_fastForward / getFastForwardTickFps());
	fprintf(stderr, "[GXFF] speed=%d achieved=%.2f tps=%.1f target=%d f=%u\n", m_fastForward, (double)m_ffAchieved, tps,
		getFastForwardTickFps(), frame);
	m_ffWindowStartNs = now;
	m_ffWindowStartFrame = frame;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 02/10/2026 TICK CATCH-UP (Common/CatchUpPolicy.h has the why and the bounds). Called once per
// draw by Zero Hour's W3DDisplay::draw, where fast forward decides its logic-only passes. Only in a running match that is
// not stalled, paused, frozen or fast-forwarded. A network match reads the network pacer (Network::getPacerFramesDue); a
// solo game, skirmish or replay reads the regular gate's debt (GeneralsX @feature Claude 07/10/2026,
// isRegularCatchUpEligible). A stalled match (waiting on a peer's commands, isGameHalted) never skips: the frame it waits
// for is not this machine's to catch up.
// ------------------------------------------------------------------------------------------------
Bool FramePacer::isCatchUpDrawSkip()
{
	const Int64 nowUs = gxSteadyNs() / 1000;
	m_catchUpPass = FALSE;
	const Bool network = m_catchUpEnabled && TheNetwork != nullptr && TheGameLogic != nullptr
		&& TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused()
		&& !TheGlobalData->m_loadScreenRender
		&& !m_isTimeFrozen && !m_isGameHalted && !isFastForwardActive() && !isScriptFastForward()
		&& TheNetwork->getFrameRate() > 0;
	const Bool regular = !network && isRegularCatchUpEligible();
	if (network)
	{
		m_catchUpPassFrame = TheGameLogic->getFrame();
		m_catchUpPass = CatchUpPolicy::skipDraw(m_catchUp, TheNetwork->getPacerFramesDue(), nowUs,
			1000000 / (Int64)TheNetwork->getFrameRate());
	}
	else if (regular)
	{
		// the frames the gate owes with this iteration's elapsed (it credits the same m_updateTime after this draw); 0 on
		// the pass it starts fresh, so that draw is never skipped (review r2 P3-3)
		const Int rate = getRegularCatchUpRate();
		m_catchUpPassFrame = TheGameLogic->getFrame();
		m_catchUpPass = CatchUpPolicy::skipDraw(m_catchUp,
			CatchUpPolicy::regularDueAtDraw(m_regularCarry, m_regularKept, m_updateTime, 1.0f / (Real)rate), nowUs,
			1000000 / (Int64)rate);
	}
	else
		CatchUpPolicy::drew(m_catchUp, nowUs);
	if (m_catchUpPass)
		++m_catchUpPasses;
	// ~30 iterations, the window of the render fps average, which counts every iteration (W3DDisplay::updateAverageFPS):
	// the HUD takes this share off it so it shows frames DRAWN (InGameUI::updateRenderFpsString).
	m_catchUpPassFraction += ((m_catchUpPass ? 1.0f : 0.0f) - m_catchUpPassFraction) / 30.0f;
	return m_catchUpPass;
}

// GeneralsX @feature Claude 07/10/2026 Tick catch-up without a network (single player, skirmish, replays): the regular gate
// keeps debt, and owed draws become logic-only passes, only while nothing else owns the pace - the network branch's
// conditions above, plus the replay fast mode (the gate bypasses itself there). GameEngine::canUpdateRegularGameLogic asks
// this too, so the gate keeps debt exactly when a draw may be skipped to repay it. The freeze/halt flags are the ones the
// gate set last iteration: the first pass of a freeze may still be a logic-only pass (review r1 P3-6, accepted - the
// simulation is unaffected, the camera steps as in fast forward).
// ...and only if the gate ran on the previous iteration: an iteration that ran this pacer without it (iOS inactive outside
// a network match returns before GameEngine::update; a load's fade loop) leaves an elapsed the gate never credited, which
// must not become debt (review r3 minor 2). Ineligible for that one pass, the gate drops its debt; the next starts fresh.
Bool FramePacer::isRegularCatchUpEligible() const
{
	return m_catchUpEnabled && m_updatesSinceRegularGate <= 1 && TheNetwork == nullptr && TheGameLogic != nullptr
		&& TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused()
		&& !TheGlobalData->m_loadScreenRender && !TheGlobalData->m_TiVOFastMode
		&& !m_isTimeFrozen && !m_isGameHalted && !isFastForwardActive() && !isScriptFastForward()
		&& getRegularCatchUpRate() > 0;
}

// The regular gate's catch-up tick rate: the logic time scale, but never above the render cap. With the scale off (render
// cap <= tick rate, FpsLimitPolicy::needsLogicTimeScale) getActualLogicTimeScaleFps reads UncappedFpsValue and the gate
// ticks once per iteration: the cap is the rate it keeps (review r1 P1-1). 0 = no usable rate.
Int FramePacer::getRegularCatchUpRate() const
{
	const Int scale = getActualLogicTimeScaleFps(IgnoreFrozenTime);
	const Int cap = getActualFramesPerSecondLimit();
	const Int rate = (scale < cap) ? scale : cap;
	return (rate > 0 && rate < (Int)RenderFpsPreset::UncappedFpsValue) ? rate : 0;
}
