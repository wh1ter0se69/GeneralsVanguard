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

// GeneralsX: The host-side policy that decides when a LAN match starts.
//
// WHY THIS IS A SEPARATE, ENGINE-FREE HEADER
//
// The arithmetic below is the only thing standing between "the host clicked something" and a match
// beginning, and a mistake in it is expensive: it either starts a game nobody agreed to, or leaves
// a lobby permanently stuck showing CANCEL. It is therefore written against plain ints and a clock
// value passed in by the caller - no GameInfo, no GameWindow, no TheLAN - so it can be driven
// exhaustively by a standalone test with a fake clock. See scripts/test/lan-start-policy-test.cpp,
// which compiles THIS header and nothing else.
//
// WALL CLOCK, NOT FRAMES. Every deadline here is in the timeGetTime() millisecond domain. Counting
// rendered frames instead is a mistake this codebase has already paid for once - 37ca1b7c1 fixed a
// UI animation that strobed on a fast machine because it counted frames - and a countdown that ran
// at a different speed per peer would be far worse than a cosmetic strobe.
//
// Comparisons use the signed difference (now - deadline) rather than `now >= deadline`, because
// timeGetTime() is a 32-bit millisecond counter that wraps roughly every 49.7 days. A plain
// comparison across the wrap either fires 49 days early or never fires at all.

#pragma once

namespace LanStart
{

	enum Phase
	{
		PHASE_IDLE = 0,      ///< normal, editable lobby
		PHASE_FORCE_WINDOW,  ///< host forced a start; unready players have a deadline to accept
		PHASE_COUNTDOWN      ///< the engine's MSG_GAME_START_TIMER is ticking towards the start
	};

	enum Event
	{
		EVENT_NONE = 0,
		EVENT_ANNOUNCE_FORCE, ///< say how long is left in the force window
		EVENT_ALL_READY,      ///< everybody accepted before the window ran out - start counting now
		EVENT_FORCE_EXPIRED,  ///< the window ran out - start anyway
		EVENT_COUNTDOWN_LOST  ///< the countdown is no longer real; put the lobby back
	};

	/// How long a forced start gives the stragglers.
	const unsigned int FORCE_WINDOW_MS = 30000u;

	/// How long after the countdown should have started the match we give up and restore the lobby.
	///
	/// The tick is owned by LANAPI::RequestGameStartTimer, and LANGameInfo::resetAccepted() cancels
	/// it from deep inside the engine whenever the slot list changes - without telling the menu. So
	/// the menu keeps its own deadline purely as a dead-man switch: if the start did not happen and
	/// nothing else noticed, this is what stops the lobby sitting on a CANCEL button forever.
	const unsigned int COUNTDOWN_WATCHDOG_SLACK_MS = 4000u;

	struct State
	{
		Phase        phase;
		unsigned int deadline;      ///< timeGetTime() domain
		int          lastAnnounced; ///< last whole second already announced; -1 = none yet
	};

	inline void reset( State &s )
	{
		s.phase = PHASE_IDLE;
		s.deadline = 0;
		s.lastAnnounced = -1;
	}

	/// Wrap-safe "has `now` reached `deadline`?".
	inline bool reached( unsigned int now, unsigned int deadline )
	{
		return (int)(now - deadline) >= 0;
	}

	/// Whole seconds left, rounded up, saturating at 0.
	inline int remainingSeconds( const State &s, unsigned int now )
	{
		if( reached( now, s.deadline ) )
			return 0;
		return (int)((s.deadline - now + 999u) / 1000u);
	}

	/// The force window is announced sparsely on purpose: it shares the chat window with the
	/// 5..1 countdown that follows it, and 30 consecutive lines would bury everything else said
	/// in the lobby. 5 is deliberately the last one - below that the countdown takes over.
	inline bool isAnnouncedSecond( int secondsLeft )
	{
		return secondsLeft == 30 || secondsLeft == 20 || secondsLeft == 10 || secondsLeft == 5;
	}

	inline void beginForceWindow( State &s, unsigned int now )
	{
		s.phase = PHASE_FORCE_WINDOW;
		s.deadline = now + FORCE_WINDOW_MS;
		s.lastAnnounced = (int)(FORCE_WINDOW_MS / 1000u); // the arming message is the "30" line
	}

	inline void beginCountdown( State &s, unsigned int now, int seconds )
	{
		s.phase = PHASE_COUNTDOWN;
		s.deadline = now + (unsigned int)(seconds * 1000) + COUNTDOWN_WATCHDOG_SLACK_MS;
		s.lastAnnounced = -1;
	}

	/// The single place the clock is read.
	///
	/// `everyPeerReady` is the caller's verdict on the slot list: TRUE when every human other than
	/// the host has accepted. During a countdown it doubles as the cancellation detector - the only
	/// thing that un-accepts a whole lobby is LANGameInfo::resetAccepted(), which the engine calls
	/// on a join, a leave or a slot change, and which also silently kills the start timer.
	inline Event tick( State &s, unsigned int now, bool everyPeerReady )
	{
		switch( s.phase )
		{
			case PHASE_FORCE_WINDOW:
			{
				if( everyPeerReady )
					return EVENT_ALL_READY;
				if( reached( now, s.deadline ) )
					return EVENT_FORCE_EXPIRED;
				const int left = remainingSeconds( s, now );
				if( left < s.lastAnnounced && isAnnouncedSecond( left ) )
				{
					s.lastAnnounced = left;
					return EVENT_ANNOUNCE_FORCE;
				}
				return EVENT_NONE;
			}

			case PHASE_COUNTDOWN:
			{
				if( !everyPeerReady )
					return EVENT_COUNTDOWN_LOST;
				if( reached( now, s.deadline ) )
					return EVENT_COUNTDOWN_LOST;
				return EVENT_NONE;
			}

			case PHASE_IDLE:
			default:
				return EVENT_NONE;
		}
	}

} // namespace LanStart
