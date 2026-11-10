/*
**	GeneralsX: when an identical UI message is a REPEAT, not a new event.
**
**	WHY THIS IS A HEADER OF ITS OWN, AND PURE.
**
**	Same reason as GXOnlineGate.h and GXUpgradeLedger.h next door: InGameUI::addMessageText is
**	unreachable from a test - it needs TheDisplayStringManager, TheFontLibrary,
**	TheGlobalLanguageData and TheGameLogic before it will run a single line - so the decision it
**	makes is lifted out to where it can be driven with `c++ -std=c++17 -I Core/GameEngine/Include`
**	and nothing else. See scripts/test/message-coalesce-test.cpp.
**
**	WHAT IT IS FOR. The message list is six slots deep and every call shifts it. Anything that
**	emits the same line N times in a burst - N selected buildings finishing the same upgrade is the
**	case that prompted this - therefore wipes the whole list and leaves six copies of one sentence.
**	Folding the repeat into the newest slot as "... (x3)" costs one slot instead of six.
**
**	This coalesces TEXT ONLY. It is not a fix for a duplicated radar blip or a duplicated sound;
**	those are separate emissions at their own call sites and this never sees them.
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
 *
 * `identityMatches` is TEXT **AND** COLOUR, not text alone. messageColor() and the coloured
 * messageNoFormat() overload let the same sentence arrive in two different colours - in-game chat
 * is coloured by the speaker's player colour - and folding those together would silently repaint
 * the second one. Folding is a display convenience; losing a colour the caller asked for is a
 * behaviour change, so identity has to include it.
 *
 * Only when the newest slot matches on that identity and is younger than the window. The window is
 * measured from the most recent occurrence, not the first, so a sustained burst keeps folding into
 * one line - which is the whole point when the alternative is six copies. The consequence, stated
 * because it is a real tradeoff and not an oversight: a line that genuinely recurs just inside the
 * window forever shows a growing count rather than a fresh entry.
 *
 * `nowFrame < newestFrame` refuses rather than wrapping. Unsigned subtraction would turn a
 * rewound or reset frame counter - a new game, a replay restart - into a gigantic difference that
 * happens to compare fine against nothing, and the correct answer there is "this is not a repeat".
 *
 * THE CLOCK IS LOGIC FRAMES, DELIBERATELY, and it is the same clock the fade uses. A wall clock
 * was considered and rejected: a replay running at 4x advances four seconds of logic per real
 * second, so a wall-clock window could fold a message that had already started fading and the
 * caller's "no need to repaint the colour" shortcut would become wrong. The cost is the mirror
 * case - while logic is PAUSED or stalled the frame number does not move, so two identical
 * messages arriving minutes apart in real time still count as immediate repeats. That is accepted
 * rather than overlooked: nothing fades during a pause either, so coalescing and fading stay
 * consistent, and the alternative needs a second timestamp per message for a cosmetic gain.
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
