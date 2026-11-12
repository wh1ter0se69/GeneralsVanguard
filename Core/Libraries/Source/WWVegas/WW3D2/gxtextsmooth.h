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

// SMOOTH HUD TEXT. The GeneralsX panels (GXSpectatorHud) ask for the font name
// "GXHUD:Arial"; on FreeType (macOS, iPadOS) those get LIGHT hinting instead of the v35 grid-fit.
// On Windows the marker is only a name: every GDI font is ClearType (gxClearTypeCoverage).
// Pure so scripts/test/text-smooth-test.cpp can check it.

#pragma once

#include <math.h>
#include <string.h>

enum { GX_TEXT_SMOOTH_SS = 3 };

/// True when `name` carries the marker; `*rest` is then the real font name.
inline bool gxTextSmoothName( const char *name, const char **rest )
{
	static const char kMarker[] = "GXHUD:";
	if (name != nullptr && strncmp( name, kMarker, sizeof(kMarker) - 1 ) == 0)
	{
		if (rest != nullptr)
			*rest = name + sizeof(kMarker) - 1;
		return true;
	}
	if (rest != nullptr)
		*rest = name;
	return false;
}

/// Coverage of one pixel of a ClearType glyph that GDI drew white on black into a 24-bit DIB (`bgr` = its three bytes):
/// the mean of the three subpixel samples. Windows draws every game font this way (render2dsentence.cpp Create_GDI_Font).
inline unsigned char gxClearTypeCoverage( const unsigned char *bgr )
{
	return (unsigned char)((bgr[0] + bgr[1] + bgr[2] + 1) / 3);
}

/// A mild gamma on the averaged coverage: an unhinted stem is thinner on screen than a grid-fitted one, so the edge pixels
/// are lifted a little (0 and 255 stay put) to keep the weight readable without a bolder face.
inline unsigned char gxSmoothWeight( unsigned char coverage )
{
	return (unsigned char)( powf( coverage / 255.0f, 0.80f ) * 255.0f + 0.5f );
}
