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

// Occupancy pips at 7x16 / 10x24 above 1080p, hand-drawn, never stretched (retail draws the 5x12 pips 1:1 at
// every resolution; the larger art is in !GXUIHD.big, from scripts/build/make-gxuihd-archive.py).
// Size by the render height in PIXELS: under 1437: 5x12; 1437 to 2159: 7x16; 2160 and up: 10x24. Width never
// enters. A missing variant falls back to the largest one present, then to retail.

#pragma once

/// The hand-drawn pip variants, as their scale x 100 against retail's 5x12: SCPPipFull@133 is 7x16, @200 is 10x24.
enum { GX_PIP_VARIANTS = 2 };
static const int GX_PIP_VARIANT_PCT[GX_PIP_VARIANTS] = { 133, 200 };

/// Which pip art to draw on a render `heightPx` pixels tall: the index of the largest variant whose scale is at or under
/// heightPx * 100 / 1080 and whose art is `present`, or -1 for retail's 5x12 (also for a zero or negative height).
inline int gxPickPipVariant( int heightPx, const bool present[GX_PIP_VARIANTS] )
{
	const long long scale = (long long)heightPx * 100 / 1080;
	int best = -1;
	for (int i = 0; i < GX_PIP_VARIANTS; ++i)
		if (present[i] && GX_PIP_VARIANT_PCT[i] <= scale && (best < 0 || GX_PIP_VARIANT_PCT[i] > GX_PIP_VARIANT_PCT[best]))
			best = i;
	return best;
}

/// Rows the empty pip is drawn below the full one: retail's art 1 row down (as today), the hand-drawn art top-aligned.
inline int gxPipEmptyYOffset( int variant )
{
	return variant < 0 ? 1 : 0;
}
