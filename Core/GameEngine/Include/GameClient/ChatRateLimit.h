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

// GeneralsX @feature Claude 21/08/2026 ONE chat cooldown, shared by every chat box in the game.
//
// The engine has six places a player can type a chat line - the in-game overlay, the LAN lobby, the
// LAN game setup, the online lobby, the online game setup, and the disconnect screen - and each one
// owns its own text entry and its own send call. A limiter written into any single one of them is
// not a chat limiter, it is a limiter on that screen, and the next screen is a hole. So the state
// lives here and every screen consults the SAME instance: leaving a lobby and opening the in-game
// chat does not hand you a fresh allowance.
//
// HEADER ONLY AND ENGINE-FREE, deliberately, following LanStartCountdown.h next door. It has no
// engine dependencies at all, so it can be reasoned about and tested on its own, and adding it
// costs no CMake edit. The caller supplies `now`; this file never reads a clock, which is what
// makes it testable.
//
// WALL CLOCK, NOT FRAMES, and the same trap LanStartCountdown.h documents applies here: comparisons
// use the SIGNED difference rather than `now >= deadline`, because timeGetTime() is a 32-bit
// millisecond counter that wraps about every 49.7 days. A plain comparison across the wrap either
// lets everything through for 49 days or nothing through ever.
//
// WHAT THIS IS NOT. It is a sender-side courtesy gate, not an anti-abuse control: it lives in the
// client, so a modified client can ignore it. Stopping a determined spammer needs the same rule on
// the relay, where the sender cannot edit it. That is a separate change and it is not this one.
//
// GeneralsX @tweak Claude 20/09/2026 3000 -> 1000 ms. The owner's report was "chat is 3 s delayed".
// It is not latency - wire time is 0.3 ms - it is this gate plus the relay's, stacked. Under a second
// reads as immediate to a person, and nobody types meaningful lobby chat faster than one short line a
// second sustained, so this is the smallest number that removes the complaint rather than merely
// improving it. The relay is deliberately 300 ms LOOSER than this (relay.js CHAT_REFILL_MS = 700), so
// jitter can never make it drop a line this gate considered legal.
//
// THE ADMIN EXEMPTION, AND WHY IT IS ALLOWED TO LIVE IN THE CLIENT.
//
// GeneralsX @feature Claude 20/09/2026, owner's decision after this file argued the other way and was
// overruled with an argument rather than an instruction. The argument is recorded because the rule it
// turns on is one this codebase takes seriously.
//
// GXIdentityAssertion.h forbids a client-held role deciding "whether an action is ALLOWED - who may
// speak, who may enter a lobby", and says that check "belongs at the relay". THIS GATE IS NOT THAT.
// It decides PACING, not permission: the relay meters chat on both paths independently
// (CHAT_BURST/CHAT_REFILL_MS for the lobby, GX_MATCH_BURST/GX_MATCH_REFILL_MS for in-match), so the
// enforcement boundary is there and stays there. What this gate does is stop a player typing into a
// silent drop. And decisively - a patched client can delete this check outright, so exempting it on a
// locally-held role confers nothing a patched client did not already have. It gives away no security
// property because it holds none.
//
// So: presentation category, not permission category, PROVIDED the relay-side exemption stays keyed
// on the assertion the relay verified itself. Do not "simplify" this by having the relay trust a flag
// in the datagram - the relay learns an identity only through its own verified assertion path.
//
// IT IS A FLOOR, NOT ZERO, AND THE FLOOR IS THE HONEST PART. An exempt client is held to
// EXEMPT_INTERVAL_MS, never released entirely. The full decision record - why a floor, why 250, why
// not surfacing the relay's refusal instead, and what the structural fix would cost - is at that
// constant, and is deliberately NOT repeated here: two copies of a rationale is two places for it to
// drift, and the constant is where somebody stands when they are about to change it.

#pragma once

namespace ChatRate
{

	/// How long a player must wait between chat lines, in milliseconds. See the note above before
	/// changing it: the relay's own interval is derived from this one, 300 ms below it.
	const unsigned int MIN_INTERVAL_MS = 1000u;

	/*
	 * What an EXEMPT player waits instead. A FLOOR, NOT ZERO.
	 *
	 * OWNER'S DECISION, 20/09/2026: ZERO WAS CONSIDERED AND DECLINED, and the number was confirmed
	 * as 250. That is recorded here rather than in a report because a limitation with no record of a
	 * decision behind it gets re-opened, or assumed to have been missed - and the obvious
	 * "simplification" of this constant is to set it to zero, which is exactly the case it exists to
	 * bound. If you are here to do that: it was already asked and answered.
	 *
	 * WHY A FLOOR AT ALL. The client's belief and the relay's come from the same assertion, so they
	 * normally agree. When they DIVERGE - the assertion expired mid-session, or the relay predates
	 * the exemption - the client skips its pacing while the relay still meters, and every line over
	 * the relay's rate is DROPPED SILENTLY. That is the original complaint this whole change exists
	 * to fix ("chat is 3 s delayed", which was really lines vanishing), aimed squarely at the one
	 * person nobody can tell. At zero, a diverged admin leaning on Enter loses an unbounded number of
	 * lines. At 250 ms the loss is bounded to something the relay's existing notice can cover, and
	 * the limiter stays ARMED so secsLeft() and the "wait N seconds" UI keep working instead of
	 * silently doing nothing.
	 *
	 * WHY 250 AND NOT SOMETHING LARGER. Four lines a second. No human types meaningful chat at that
	 * rate, so to the player it is indistinguishable from no limit - which is what was asked for.
	 *
	 * WHY NOT INSTEAD SURFACE THE RELAY'S REFUSAL, the other option that was on the table: the only
	 * refusal signal that exists on the wire is the relay's grey "Sending too fast" notice, throttled
	 * to once per CHAT_THROTTLE_NOTICE_MS (10 s), so most refusals in a burst carry no signal at all.
	 * Consuming it would mean parsing an English sentence off the wire to drive client behaviour -
	 * which breaks the day somebody rewords the sentence, and breaks SILENTLY, turning this back into
	 * the unbounded case with nothing to show for it.
	 *
	 * THE STRUCTURAL FIX, COSTED AND DELIBERATELY NOT BUILT. Gate the exemption on a relay capability
	 * token: GXCAPS already carries a comma-separated flag list whose own comment says "a later
	 * capability costs no new tag", it rides the 5 s registration keepalive, and it has a TTL - so
	 * the client would be exempt only while a relay that implements the exemption keeps saying so,
	 * and a rollback would heal itself. It was not built because it costs a new token parse in
	 * Transport.cpp's captureRelayCaps AND loosening test-pktauth.js:264, which pins the caps string
	 * with exact equality (`caps.split(' ')[1] === 'p2pauth'`) on purpose - and because the window it
	 * closes is already eliminated in practice by the relay-first deploy order this change requires.
	 * If the exemption ever needs to be trustworthy rather than merely bounded, that is the path.
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
