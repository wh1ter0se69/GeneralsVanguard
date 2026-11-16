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
**	Which powers of a map-placed object that changes owner at frame 0 get a superweapon timer; pure
**	(scripts/test/superweapon-add-policy-test.cpp). Same rule as the frame-0 add in SpecialPowerModule::resolveSpecialPower:
**	public timer AND structure AND not a command center. A unit ability (Missile Defender laser missiles) has no
**	"GUI:<power>" label, so a timer for it prints "MISSING:" on screen.
*/

#pragma once

#ifndef GX_SUPERWEAPON_ADD_POLICY_H
#define GX_SUPERWEAPON_ADD_POLICY_H

namespace GXSuperweaponAddPolicy
{

inline bool addUnlisted( bool frame0, bool underConstruction, bool commandCenter, bool structure, bool publicTimer )
{
	return frame0 && !underConstruction && !commandCenter && structure && publicTimer;
}

}

#endif
