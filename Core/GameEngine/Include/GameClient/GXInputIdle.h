/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 super:hackers
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

// FILE: GXInputIdle.h ///////////////////////////////////////////////////////////////////////////
//
// GeneralsX @feature How long since the player last moved the mouse, clicked, scrolled, typed or touched
// the screen. The Online lobby reports it to the relay (GXPRES), which lists a player as AFK past its own threshold.
//
// Fed by WindowTranslator (WindowXlat.cpp), which every raw mouse and key message passes through; touch arrives there as
// mouse messages. Header-only so it needs no build-file change; one clock for the process (a function-local static in an
// inline function has one instance across translation units). Never simulation state: nothing in the logic may read it.

#pragma once

#ifndef __GXINPUTIDLE_H_
#define __GXINPUTIDLE_H_

#include "Lib/BaseType.h"

inline UnsignedInt &gxInputLastMs( void )
{
	static UnsignedInt s_last = 0;
	return s_last;
}

/// An input happened at `nowMs` (timeGetTime()).
inline void GXInputIdleTouch( UnsignedInt nowMs )
{
	gxInputLastMs() = nowMs;
}

/// Whole seconds since the last input, 0 before the first one is seen (a client that has not been touched since it
/// started is not called idle on that evidence alone), capped at 999999 (the relay's six digits).
inline UnsignedInt GXInputIdleSeconds( UnsignedInt nowMs )
{
	const UnsignedInt last = gxInputLastMs();
	if (last == 0 || nowMs - last > 0x7FFFFFFFu)
		return 0;
	const UnsignedInt s = (nowMs - last) / 1000u;
	return s > 999999u ? 999999u : s;
}

#endif // __GXINPUTIDLE_H_
