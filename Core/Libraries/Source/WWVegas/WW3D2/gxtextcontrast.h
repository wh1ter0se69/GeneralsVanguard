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

// GX_TEXT_CONTRAST's curve, pure so scripts/test/text-shadow-test.cpp can check it.
// DirectWrite-style "enhanced contrast" on 8-bit glyph coverage: a' = a(k+1) / (ak+1). Partial-coverage edge pixels get
// stronger, 0 and 255 stay put. k outside (0, 100] - off, NaN, infinite, absurd - is the identity (k = inf gave
// NaN for every value and a huge k turned every edge pixel solid).

#pragma once

inline unsigned char gxTextContrast( float k, unsigned char coverage )
{
	if (!(k > 0.0f && k <= 100.0f))
		return coverage;
	const float a = coverage / 255.0f;
	return (unsigned char)(a * (k + 1.0f) / (a * k + 1.0f) * 255.0f + 0.5f);
}
