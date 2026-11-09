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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DLaserDraw.h ///////////////////////////////////////////////////////////////////////////
// Author:
// Desc:
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "Common/DrawModule.h"
//#include "WW3D2/Line3D.h"
#include "GameClient/Color.h"

class SegmentedLineClass;
class TextureClass;
class Vector3;

class W3DLaserDrawModuleData : public ModuleData
{
public:

  Color m_innerColor;
  Color m_outerColor;
	Real m_innerBeamWidth;
	Real m_outerBeamWidth;
	Real m_scrollRate;
	Bool m_tile;
  UnsignedInt m_numBeams;
  UnsignedInt m_maxIntensityFrames;
  UnsignedInt m_fadeFrames;
	AsciiString m_textureName;
	UnsignedInt m_segments;
	Real m_arcHeight;
	Real m_segmentOverlapRatio;
	Real m_tilingScalar;

	W3DLaserDrawModuleData();
	virtual ~W3DLaserDrawModuleData() override;
	static void buildFieldParse(MultiIniFieldParse& p);
};

//-------------------------------------------------------------------------------------------------
/** W3D laser draw */
//-------------------------------------------------------------------------------------------------
class W3DLaserDraw : public DrawModule, public LaserDrawInterface
{

	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( W3DLaserDraw, "W3DLaserDraw" )
	MAKE_STANDARD_MODULE_MACRO_WITH_MODULE_DATA( W3DLaserDraw, W3DLaserDrawModuleData )

public:

	W3DLaserDraw( Thing *thing, const ModuleData* moduleData );
	// virtual destructor prototype provided by memory pool declaration

	virtual void doDrawModule(const Matrix3D* transformMtx) override;
	virtual void releaseShadows() override {};	///< we don't care about preserving temporary shadows.
	virtual void allocateShadows() override {};	///< we don't care about preserving temporary shadows.
	virtual void setShadowsEnabled(Bool enable) override { }
	virtual void setFullyObscuredByShroud(Bool fullyObscured) override { };
	virtual void reactToTransformChange(const Matrix3D* oldMtx, const Coord3D* oldPos, Real oldAngle) override { }
	virtual void reactToGeometryChange() override { }
	virtual Bool isLaser() const override { return true; }
	virtual Real getLaserTemplateWidth() const override;

	virtual LaserDrawInterface* getLaserDrawInterface() override { return this; }
	virtual const LaserDrawInterface* getLaserDrawInterface() const override { return this; }

protected:

	SegmentedLineClass **m_line3D;  ///< line 3D for effect
	TextureClass *m_texture;
	Real m_textureAspectRatio;			///< aspect ratio of texture
	Bool m_selfDirty;								// not saved
	Coord3D m_drawnStart;						///< GeneralsX: the ends the geometry was last built for (render interpolation); not saved
	Coord3D m_drawnEnd;
	Bool m_effectsPlaced;						///< GeneralsX: muzzle/target effects moved to the drawn ends on the first frame; not saved

	// GeneralsX @perf wh1ter0se 11/08/2026 One polyline per beam instead of one 2-point render
	// object per segment: a Segments=20 beam was 20 scene objects, 20 dynamic VB/IB lock pairs
	// and 20 translucent-sort insertions per frame. Consolidation is gated on
	// SegmentOverlapRatio == 0 so INIs that rely on overlapping segment quads keep the legacy
	// path and pixel-identical output.
	Bool m_consolidated;						///< one line of (segments+1) points per beam
	UnsignedInt m_lineCount;				///< lines actually allocated in m_line3D
	Vector3 *m_points;							///< scratch polyline points (segments+1), consolidated only
	Real *m_pointDist;							///< cumulative arc length at each point, consolidated only
	Real *m_pointV;								///< per-beam V scratch (arc length scaled by width)

};
