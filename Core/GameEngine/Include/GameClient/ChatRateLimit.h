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

// ONE chat cooldown shared by every chat box: each screen owns its send call, so the state lives here.
// Header-only; the caller supplies `now`. Compare the SIGNED difference (timeGetTime wraps ~49.7 days).
// A courtesy gate, not anti-abuse: a modified client can ignore it; the relay (CHAT_REFILL_MS = 700) enforces.
// The admin exemption is pacing, not permission; the relay's exemption stays keyed on its own verified assertion.

#pragma once

namespace ChatRate
{

	/// How long a player must wait between chat lines, in milliseconds. See the note above before
	/// changing it: the relay's own interval is derived from this one, 300 ms below it.
	const unsigned int MIN_INTERVAL_MS = 1000u;

	/*
	 * What an EXEMPT player waits instead: a FLOOR, NOT ZERO (250 ms, four lines a second). If the client and relay
	 * disagree about the exemption (assertion expired mid-session, or a relay that predates it), the relay still
	 * meters and lines over its rate are DROPPED SILENTLY; at zero that loss is unbounded. The limiter stays armed so
	 * secsLeft() keeps working. Do not set this to zero.
	 */
	const unsigned int EXEMPT_INTERVAL_MS = 250u;

	struct State
	{
		unsigned int nextAllowed;  ///< timeGetTime() domain; meaningless while armed is false
		bool         armed;        ///< false until the first line goes out, so the first is free
		bool         exempt;       ///< set from the local verified role; see setExempt
	};

	inline void reset( State &s )
	{
		s.nextAllowed = 0u;
		s.armed = false;
		s.exempt = false;
	}

	/// The interval in force for this state. One definition, so allows(), consume() and any future
	/// caller cannot disagree about which number applies.
	inline unsigned int intervalOf( const State &s )
	{
		return s.exempt ? EXEMPT_INTERVAL_MS : MIN_INTERVAL_MS;
	}

	/// Turn the exemption on or off. Called from the one place that knows the local verified role.
	///
	/// IT NEVER SHORTENS A COOLDOWN ALREADY RUNNING. Clearing the exemption leaves nextAllowed where
	/// it was, so an admin whose sign-in lapses does not get a free line out of the transition; and
	/// granting it does not retroactively forgive a wait the player is already serving. The change
	/// takes effect from the NEXT consume, which is the only moment at which it is meaningful.
	inline void setExempt( State &s, bool on )
	{
		s.exempt = on;
	}

	/// Would a line be allowed right now? No side effect - safe to call from a draw or a tooltip.
	inline bool allows( const State &s, unsigned int now )
	{
		if( !s.armed )
			return true;
		return (int)( now - s.nextAllowed ) >= 0;
	}

	/// Is this state currently exempt? For a caller that wants to say so in the UI.
	inline bool isExempt( const State &s )
	{
		return s.exempt;
	}

	/// How much longer the caller has to wait, in milliseconds. Zero when a line is allowed.
	inline unsigned int msecLeft( const State &s, unsigned int now )
	{
		if( allows( s, now ) )
			return 0u;
		return (unsigned int)( (int)( s.nextAllowed - now ) );
	}

	/// Same, rounded UP to whole seconds, because "wait 0 seconds" is a confusing thing to be told.
	inline unsigned int secsLeft( const State &s, unsigned int now )
	{
		const unsigned int ms = msecLeft( s, now );
		return ( ms + 999u ) / 1000u;
	}

	/// TRUE if a line may go out now, and starts the next cooldown. FALSE leaves the cooldown
	/// exactly as it was - a refused attempt must not extend the wait, or holding Enter down would
	/// lock the player out indefinitely.
	inline bool consume( State &s, unsigned int now )
	{
		if( !allows( s, now ) )
			return false;
		s.nextAllowed = now + intervalOf( s );
		s.armed = true;
		return true;
	}

	/// The one instance every chat box shares. A function-local static rather than a file-scope
	/// object so that there is exactly one across every translation unit that includes this header,
	/// without needing a .cpp to define it.
	inline State &shared()
	{
		static State s = { 0u, false, false };
		return s;
	}

	/// Set the shared exemption. One caller: GXPlayerIdentity, where the local verified role is read.
	inline void setSharedExempt( bool on )
	{
		setExempt( shared(), on );
	}

	/// Convenience for the common call: "may I send, and if not, how many seconds are left?"
	inline bool consumeShared( unsigned int now, unsigned int *outSecsLeft )
	{
		State &s = shared();
		if( consume( s, now ) )
			return true;
		if( outSecsLeft != 0 )
			*outSecsLeft = secsLeft( s, now );
		return false;
	}

} // namespace ChatRate
