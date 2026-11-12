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

// Building rotation: the octant of a direction by
// comparisons only (tan 22.5 deg = 0.41421356), so x64 and arm64 pick the same angle - no atan2 in a simulation decision.
#pragma once

inline float gxOctantAngle(float dx, float dy)
{
	const float P = 3.14159265358979f, T = 0.41421356f;
	const float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
	if (ax == 0.0f && ay == 0.0f)
		return 0.0f;
	if (ay <= ax * T)
		return dx >= 0 ? 0.0f : P;
	if (ax <= ay * T)
		return dy >= 0 ? P / 2 : -P / 2;
	if (dx >= 0)
		return dy >= 0 ? P / 4 : -P / 4;
	return dy >= 0 ? 3 * P / 4 : -3 * P / 4;
}
