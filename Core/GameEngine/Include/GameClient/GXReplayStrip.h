// GeneralsX: the replay strip's pure model (GXReplayStrip.inl draws and acts on it).
//
// Client-only: nothing here is read by the simulation, and GameClient/ is outside the SimID digest. The decisions that
// can go wrong silently live here so they can be unit-tested with no engine (scripts/test/replay-strip-model-test.cpp).
#pragma once

#include <algorithm>

namespace GXReplayStrip
{

enum Speed { SPEED_HALF, SPEED_1X, SPEED_2X, SPEED_4X, SPEED_8X, SPEED_COUNT };

/// The label and the fast-forward multiplier of a speed button; SPEED_HALF has no multiplier (a slower logic pace).
inline const wchar_t *speedLabel( int s )
{
	static const wchar_t *const t[SPEED_COUNT] = { L"0.5x", L"1x", L"2x", L"4x", L"8x" };
	return (s >= 0 && s < SPEED_COUNT) ? t[s] : L"";
}

inline int speedMultiplier( int s )
{
	static const int m[SPEED_COUNT] = { 1, 1, 2, 4, 8 };
	return (s >= 0 && s < SPEED_COUNT) ? m[s] : 1;
}

/// The button that shows as lit: the fast-forward multiplier when one is on (the nearest button at or below it), else
/// 0.5x when the logic pace is halved, else 1x.
inline int speedFromState( int fastForward, bool halved )
{
	if (fastForward >= 8)
		return SPEED_8X;
	if (fastForward >= 4)
		return SPEED_4X;
	if (fastForward >= 2)
		return SPEED_2X;
	return halved ? SPEED_HALF : SPEED_1X;
}

/// The frame under a click at x on a bar [left, left+width) of a replay `length` frames long. Never the last frame: a
/// replay that reaches it ends and leaves the match, so the strip stops one short and the viewer is left paused there.
inline unsigned frameAtX( int x, int left, int width, unsigned length )
{
	if (length < 2 || width < 1)
		return 0;
	const long long rel = (long long)x - left;
	if (rel <= 0)
		return 0;
	if (rel >= width)
		return length - 1;
	return (unsigned)(rel * (long long)(length - 1) / width);
}

/// Where the playhead sits on the bar, 0..width.
inline int xAtFrame( unsigned frame, int width, unsigned length )
{
	if (length < 2 || width < 1)
		return 0;
	if (frame >= length - 1)
		return width;
	return (int)((long long)frame * width / (length - 1));
}

enum Plan { PLAN_NONE, PLAN_RACE_AHEAD, PLAN_RESTART_AND_RACE, PLAN_RESTART };

/// A click on the bar at `target` while the replay stands at `frame`. Ahead = race to it from here. Behind = there is no
/// way back without a checkpoint (replay step 3), so restart and race forward again. A click within the first second is
/// a plain restart. A target on the current frame is nothing to do.
inline Plan planSeek( unsigned frame, unsigned target, unsigned ticksPerSecond )
{
	if (target == frame)
		return PLAN_NONE;
	if (target > frame)
		return PLAN_RACE_AHEAD;
	return target < ticksPerSecond ? PLAN_RESTART : PLAN_RESTART_AND_RACE;
}

/// A race is over once the frame has reached its target.
inline bool raceDone( unsigned frame, unsigned target ) { return frame >= target; }


/// A screen rectangle (the strip, its controls) for the drag rules below.
struct Cell
{
	int x, y, w, h;
	bool contains( int px, int py ) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

/// Dragging the strip: a press starts a drag only on the strip's own empty background, never on a control (pause, a
/// speed, the seek bar) - controls are tested first by the caller and pass `onControl`.
inline bool startsDrag( const Cell &strip, bool onControl, int x, int y ) { return !onControl && strip.contains( x, y ); }

/// Keeps the dragged strip wholly on screen: `dx`/`dy` is its offset from home (homeX/homeY, w x h) and is pulled back
/// in place, so a drag past an edge (or a smaller window after a resolution change) never accumulates a hidden overshoot.
/// A strip wider or taller than the screen pins to the top-left.
inline void clampOffset( int homeX, int homeY, int w, int h, int screenW, int screenH, int &dx, int &dy )
{
	const int maxX = std::max( 0, screenW - w ), maxY = std::max( 0, screenH - h );
	dx = std::min( std::max( homeX + dx, 0 ), maxX ) - homeX;
	dy = std::min( std::max( homeY + dy, 0 ), maxY ) - homeY;
}

enum { RACE_DRAW_MS = 250 };	///< a seek race draws one progress frame per this much wall time, nothing else

/// Does this render pass draw (W3DDisplay::draw)? `fastMode` = the replay fast mode runs (m_TiVOFastMode, a replay, not
/// paused); `seekRace` = the strip is racing to a frame; `ffDrawDue` = fast forward's own cadence (FramePacer::
/// isFastForwardDrawDue - a caller asks it only when this needs it, it keeps credit).
/// A seek race skips every frame but a progress one every RACE_DRAW_MS of wall time,
/// so the strip and playhead visibly move at any speed the machine reaches. The fast mode alone - the
/// retail replay hotkey - keeps retail's one frame in 30. Fast forward's cadence is never asked under the fast mode: with
/// one tick per pass, its every-k-th pass and frame 30n+1 can be disjoint for good, and a 2x-10x seek drew nothing until
/// it landed (07/10, "it froze").
inline bool passRenders( bool fastMode, bool seekRace, unsigned frame, unsigned nowMs, unsigned lastRaceDrawMs, bool ffDrawDue )
{
	if (!fastMode)
		return ffDrawDue;
	if (seekRace)
		return lastRaceDrawMs == 0 || nowMs - lastRaceDrawMs >= RACE_DRAW_MS;
	return frame % 30 == 1;
}

/// The speed a seek race puts back when it lands (the race itself runs with fast forward suspended - see begin()): the
/// one in force when the first seek began. A second seek mid-race keeps it; a speed button pressed mid-race replaces it.
struct RaceSpeed
{
	bool on = false;
	int ff = 1;
	bool halved = false;
	void begin( int curFF, bool curHalved ) { if (!on) { on = true; ff = curFF; halved = curHalved; } }
	void choose( int speed ) { halved = speed == SPEED_HALF; ff = speedMultiplier( speed ); }
	/// A fast-forward step asked for mid-race (a speed key, the debug screen): the multiplier it lands at. 1 is a request too.
	void chooseFF( int mult ) { if (mult > 1) halved = false; ff = std::min( std::max( mult, 1 ), 10 ); }	// a 1x request leaves a halved pace alone, as the key does outside a race
	void end() { on = false; }
	int shown() const { return speedFromState( ff, halved ); }
};

/// The next fast-forward step from `now` (CommandXlat's changeFastForward). During a seek race `now` must be the speed the
/// race LANDS at, not the pacer's value: the race holds the pacer at 1x, so stepping from it ignored a decrease (1 is no
/// change, nothing to record) and answered every increase with 2x.
inline int ffStep( int now, bool increase )
{
	static const int s_steps[] = { 1, 2, 3, 4, 5, 10 };
	int next = now;
	if (increase)
	{
		for (int i = 0; i < 6 && next == now; ++i)
			if (s_steps[i] > now)
				next = s_steps[i];
	}
	else
	{
		for (int i = 5; i >= 0 && next == now; --i)
			if (s_steps[i] < now)
				next = s_steps[i];
	}
	return next;
}

/// A restart in flight (a seek behind the playhead): the frame to race to once the new playback is up (0 = none, a plain
/// restart), and whether the new playback is still awaited. finish( started ) is what gxRestartReplay answered: a restart
/// that FAILED (the replay file is gone) leaves the engine at no game for good, so nothing will ever call it up and the
/// race's sound mute and speed must be given back NOW (returns true = drop the race), 07/10.
struct SeekFlight
{
	bool restarting = false;
	unsigned pending = 0;
	void begin( unsigned target ) { restarting = true; pending = target; }
	bool finish( bool started )
	{
		if (started)
			return false;
		restarting = false;
		pending = 0;
		return true;
	}
};

/// The strip's lost-release recovery (draw): a drag whose button the mouse says is up. A release that is merely QUEUED -
/// playback has no network, so mouse messages are translated after the strip draws - looks the same for one pass, and
/// cancelling then swallowed an ordinary release: the strip stopped short of where it was let go. Only a
/// button seen up on two draws in a row, with the drag still held, is lost: a queued release is translated before the next one.
struct LostRelease
{
	int up = 0;
	void reset() { up = 0; }	// a gesture ended or a new press began: what was seen up belonged to the last one
	bool observe( bool dragging, bool buttonDown )
	{
		if (!dragging || buttonDown)
		{
			up = 0;
			return false;
		}
		return ++up >= 2;
	}
};

} // namespace GXReplayStrip
