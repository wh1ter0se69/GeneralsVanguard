/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// FILE: GXPickProbe.h ///////////////////////////////////////////////////////////////////////////
//
// GeneralsX: [GXPICK] - "the item spawns not aligned with the preview
// of the drone/scan". A targeted special power has two picks of the same mouse: the PREVIEW (the
// radius-cursor decal, InGameUI::handleRadiusCursor, every rendered frame from the mouse status
// position, radar first then terrain) and the SEND (CommandXlat's click handler, from the click
// message's pixel, terrain only). Read side by side they run the same screenToTerrain on the same
// scaled pixel, so the code alone does not show where an offset would come from.
//
// This records what the preview last put the decal on and where the last rendered frame drew it, so
// the send side can print both. Reprojecting a picked point through the pick's own camera math only
// gives the click back (see gxPickLog in CommandXlat.cpp); the drawn pixel is computed from render-side
// inputs instead, so it is the field that can disagree with the click. One line per issued location
// special power; client only, outside the sim digest. Remove once the rendered repro has named the cause.
//
// The G build compiles CommandXlat.cpp too but never records a preview; it prints preview=none.

#pragma once

#include "Lib/BaseType.h"

struct GXPickPreview
{
	Bool valid;
	Bool fromRadar;	///< TheRadar->screenPixelToWorld answered, not the terrain pick
	ICoord2D pixel;	///< TheMouse status position the preview picked from
	Coord3D world;	///< the decal's world centre

	// Written by W3DView::draw right after the 3D pass: where THAT frame drew the decal centre, from
	// the render side's own inputs (the camera state it rendered with, its normalised Viewport, the
	// render target size), not from View::getWidth/getHeight/origin, which the pick uses.
	Bool drawnValid;
	Coord3D drawnWorld;	///< the preview world point that frame drew
	ICoord2D drawnPx;	///< ...and the display pixel it landed on
	Int rtW, rtH;		///< WW3D render target resolution
	Int vpX, vpY, vpW, vpH;	///< the 3D pass's D3D viewport, in render-target pixels
};

/// Defined (zero-initialised) in CommandXlat.cpp, which both games compile; written by ZH's InGameUI
/// and by W3DView::draw.
extern GXPickPreview TheGXPickPreview;

/// The D3D viewport CameraClass::Apply sets for a camera Viewport (normalised min/max) on an rtW x rtH
/// render target - the same float-to-DWORD truncation Apply does.
inline void gxPickRenderViewport( Real minX, Real minY, Real maxX, Real maxY, Int rtW, Int rtH,
	Int &x, Int &y, Int &w, Int &h )
{
	x = (Int)(UnsignedInt)(minX * (Real)rtW);
	y = (Int)(UnsignedInt)(minY * (Real)rtH);
	w = (Int)(UnsignedInt)((maxX - minX) * (Real)rtW);
	h = (Int)(UnsignedInt)((maxY - minY) * (Real)rtH);
}

/// The display pixel a view-plane point (-1..1, y up, as CameraClass::Project returns it) is rasterised
/// at: D3D maps it into the viewport rect in render-target pixels, and the render target spans the
/// display. It shares no input with the pick's PixelScreenToW3DLogicalScreen(view width/height/origin),
/// so a camera Viewport or render target that disagrees with the View shows up as drawnPx != click.
inline ICoord2D gxPickRenderedPixel( Real ndcX, Real ndcY, Int vpX, Int vpY, Int vpW, Int vpH,
	Int rtW, Int rtH, Int dispW, Int dispH )
{
	const Real rx = (Real)vpX + (Real)vpW * (ndcX + 1.0f) * 0.5f;
	const Real ry = (Real)vpY + (Real)vpH * (1.0f - ndcY) * 0.5f;
	ICoord2D p;
	p.x = (Int)(rx * (Real)dispW / (Real)rtW);
	p.y = (Int)(ry * (Real)dispH / (Real)rtH);
	return p;
}
