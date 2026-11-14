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

// What a refused sign-in says. A signed assertion that fails ONLY on the clock is a fact about this PC, not
// about the login: NOTYET means the PC clock is behind the server's, EXPIRED that it is ahead. Say so, plainly.
// No "by N hours": verify() writes nothing to its caller on a refusal (that is its safety property), so the
// issue and expiry times are not available here. Every other refusal keeps the original text.
// Dependency-free apart from the status enum, so scripts/test/signin-clock-text-test.cpp includes it alone.
#pragma once

#include "GameClient/GXIdentityAssertion.h"

inline const char *gxLoginRefusalText( GXAssertStatus st )
{
	switch (st)
	{
		case GX_ASSERT_NOTYET:
		case GX_ASSERT_EXPIRED:
			return "The date or time on this PC is wrong, so the sign-in was refused. "
				"Fix the date, time and time zone in your system settings, then sign in again.";
		default:
			return "That login could not be verified.";
	}
}
