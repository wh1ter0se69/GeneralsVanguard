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

// GeneralsX @bugfix Claude 25/09/2026 The match timer never shares pixels with the readout row.
//
// The row (latency, resends, fps, tps, system clock) starts at the GXHud.ini anchor and grows
// rightward; the timer is right-aligned, inset from the RIGHT edge by the same anchor X. At the 50%
// cap both therefore end at the screen centre, and large sizes at a small resolution meet at any X;
// side by side they read as one string ("00:00:39.2060[max 60]60tps", device lane 25/09/2026). When
// they would touch or overlap, the timer yields and drops one gap under the row - the row's position
// is the one the player chose.
//
// Pure arithmetic so scripts/test/hud-timer-layout-test.cpp can drive it without an engine.

#pragma once

#include "Lib/BaseType.h"

/// Top edge to draw the match timer at. The row's box is [rowLeft,rowRight) x [rowTop,rowBottom); an
/// empty box (nothing drew on the row) never moves the timer. The timer's box is
/// [timerLeft,timerRight) x [timerTop,timerBottom). Horizontally the two must stay `gap` apart,
/// because side by side with no gap the digits run together; vertically they only must not overlap.
inline Int gxHudTimerTop( Int rowLeft, Int rowTop, Int rowRight, Int rowBottom,
	Int timerLeft, Int timerTop, Int timerRight, Int timerBottom, Int gap )
{
	if( rowRight <= rowLeft || rowBottom <= rowTop )
		return timerTop;
	const Bool meetX = timerLeft < rowRight + gap && rowLeft < timerRight + gap;
	const Bool meetY = timerTop < rowBottom && rowTop < timerBottom;
	return ( meetX && meetY ) ? rowBottom + gap : timerTop;
}
