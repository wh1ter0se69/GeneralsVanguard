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

/*
**	When an identical UI message is a REPEAT, not a new event. Pure, so scripts/test/message-coalesce-test.cpp
**	drives it. The message list is six slots deep, so N copies of one line in a burst wipe it; folding repeats into
**	the newest slot as "... (x3)" costs one slot. TEXT ONLY: not a fix for a duplicated radar blip or sound.
*/

#pragma once

#ifndef GX_MESSAGE_COALESCE_H
#define GX_MESSAGE_COALESCE_H

namespace GXMessageCoalesce
{

/*
 * The coalesce window, in logic frames.
 *
 * One second, but NEVER longer than the message's own lifetime, and that clamp is load-bearing
 * rather than defensive. The caller does not repaint the colour when it folds a repeat in, which
 * is only safe because a message inside this window provably has not begun to fade: the fade in
 * InGameUI::update() starts at `now - timestamp > messageTimeout`, so as long as the window is
 * <= messageTimeout, a coalescable message has never entered that branch. MessageDelayMS is INI
 * data (the shipped file sets 75000) and can in principle be set below a second, which is the
 * configuration this clamp exists for.
 */
inline unsigned int windowFrames( unsigned int oneSecondInFrames, unsigned int messageTimeoutFrames )
{
	return (oneSecondInFrames < messageTimeoutFrames) ? oneSecondInFrames : messageTimeoutFrames;
}

/*
 * Should this message be folded into the newest slot instead of pushed onto the ring?
 * `identityMatches` is TEXT **AND** COLOUR: the same sentence in two colours must not be repainted.
 * Only when the newest slot matches and is younger than the window, measured from the latest occurrence.
 * `nowFrame < newestFrame` refuses rather than wrapping. The clock is LOGIC FRAMES, the one the fade uses
 * (wall clock would misfold in a fast replay); while logic is paused, identical messages still count as repeats.
 */
inline bool shouldCoalesce( bool haveNewest, bool identityMatches,
                            unsigned int nowFrame, unsigned int newestFrame,
                            unsigned int windowInFrames )
{
	if (!haveNewest || !identityMatches)
		return false;
	if (nowFrame < newestFrame)
		return false;
	return (nowFrame - newestFrame) <= windowInFrames;
}

}	// namespace GXMessageCoalesce

#endif	// GX_MESSAGE_COALESCE_H
