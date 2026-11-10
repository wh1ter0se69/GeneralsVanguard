// GeneralsX: How much smaller world-anchored text and 2D world animations draw above the stock
// camera ceiling (owner, 02/10/2026, watching a 12-AI match at max zoom-out: the "Building: x%" labels cover the map;
// "make [them] dynamic ... slightly smaller on the max zoom out"; and the promotion chevrons "are too big").
//
// `iconScale` is View::getRetailReferenceZoom's: 1.0 up to the height the stock MaxCameraHeight would allow this view,
// and ceiling / height above it - the proportional shrink every unit on screen gets, which the health bar, pips and
// veterancy icon already follow. Text cannot follow that all the way (at the Debug menu's 3000 it is a tenth), so this
// is the gentle version: its square root, never under GX_WORLD_LABEL_MIN_SCALE. 1.0 at and below the ceiling - the
// retail look, unchanged.
//
// Header-only and pure: both games' Drawable.cpp and InGameUI.cpp, and scripts/test/world-label-scale-test.cpp.

#pragma once

#include <cmath>

/// The smallest a label gets, as a fraction of its retail size (calibration knob: 0.6 keeps the caption font at
/// 1440p - 20 pt - at 12 pt).
static const float GX_WORLD_LABEL_MIN_SCALE = 0.6f;

/// The smallest point size a label shrinks to, whatever its scale: Arial at 8 pt is about 11 px tall and still reads.
/// A label already smaller than this at retail size is never shrunk (adversarial review P3: at 800x600 the 10 pt caption
/// would otherwise have gone to 6 pt).
static const int GX_WORLD_LABEL_MIN_POINTS = 8;

/// The gentle shrink for `iconScale`; 1.0 for anything not under 1 (NaN included).
inline float gxWorldLabelScale( float iconScale )
{
	if (!(iconScale < 1.0f))
		return 1.0f;
	const float s = std::sqrt(iconScale);	// NaN for a negative, which fails the test below
	return s > GX_WORLD_LABEL_MIN_SCALE ? s : GX_WORLD_LABEL_MIN_SCALE;
}

/// The point size a `basePoints` label draws at for `scale`, given the size `current` in use (<= 0: none yet). A font
/// only comes in whole points, so the size steps; it moves only once the exact size is a whole point away from the one
/// in use - half a point past where rounding would flip - so a camera resting near a boundary (or bobbing over terrain)
/// cannot flip every label between two sizes from frame to frame. `basePoints` exactly for a scale of 1 (or NaN), never
/// above it, never under GX_WORLD_LABEL_MIN_POINTS (or the base, when that is smaller).
inline int gxWorldLabelPoints( int basePoints, float scale, int current )
{
	if (basePoints < 1 || !(scale < 1.0f))
		return basePoints;		// at and below the ceiling: retail's size, whatever was in use
	const float exact = (float)basePoints * scale;
	if (current >= 1 && current <= basePoints && std::fabs(exact - (float)current) < 1.0f)
		return current;
	const long points = std::lround(exact);
	const int floor = basePoints < GX_WORLD_LABEL_MIN_POINTS ? basePoints : GX_WORLD_LABEL_MIN_POINTS;
	return points < floor ? floor : (points > basePoints ? basePoints : (int)points);
}
