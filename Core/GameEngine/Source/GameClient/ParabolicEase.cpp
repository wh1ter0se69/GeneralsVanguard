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

// ============================================================================
// Copyright (C) 2003, 2004 Electronic Arts
//
// ParabolicEase.cpp
// Ease in and out based on a parabolic function.
// Author: Robert Minsk May 12, 2003
// ============================================================================
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameClient/ParabolicEase.h"
#include "Common/ScriptedCameraClock.h"
// ============================================================================
// ============================================================================
// ============================================================================
// GeneralsX: The arithmetic is ScriptedCameraClock::Ease (Common/ScriptedCameraClock.h): a scripted
// camera move's time multiplier follows it, so it is simulation code now and lives in the digest. This class keeps its
// debug reports.
void
ParabolicEase::setEaseTimes(Real easeInTime, Real easeOutTime)
{
	DEBUG_ASSERTCRASH(easeInTime >= 0.0f && easeInTime <= 1.0f, ("Ease-in out of range (in = %g)", easeInTime));
	DEBUG_ASSERTCRASH(easeOutTime >= 0.0f && easeOutTime <= 1.0f, ("Ease-out out of range (out = %g)", 1.0f - easeOutTime));
	DEBUG_ASSERTCRASH(easeInTime <= 1.0f - easeOutTime + FLT_EPSILON, ("Ease-in and ease-out overlap (in = %g, out = %g)", easeInTime, 1.0f - easeOutTime));
	ScriptedCameraClock::Ease ease;
	ease.setEaseTimes(easeInTime, easeOutTime);
	m_in = ease.in;
	m_out = ease.out;
}

// ----------------------------------------------------------------------------
Real
ParabolicEase::operator ()(Real param) const
{
	DEBUG_ASSERTCRASH(param >= 0.0f && param <= 1.0f, ("Ease-in/ease-out parameter out of range (param = %g)", param));
	ScriptedCameraClock::Ease ease;
	ease.in = m_in;
	ease.out = m_out;
	return ease(param);
}
