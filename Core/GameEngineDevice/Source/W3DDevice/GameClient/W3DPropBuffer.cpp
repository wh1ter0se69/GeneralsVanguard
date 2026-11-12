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

// FILE: W3DPropBuffer.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: W3DPropBuffer.cpp
//
// Created:   John Ahlquist, May 2001
//
// Desc:      Draw buffer to handle all the props in a scene.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------

#include "W3DDevice/GameClient/W3DPropBuffer.h"

#include <WW3D2/assetmgr.h>
#include "Common/GameUtility.h"
#include "Common/Geometry.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "WW3D2/camera.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/light.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8renderer.h"
#include "W3DDevice/GameClient/Module/W3DPropDraw.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "GameLogic/PartitionManager.h"
#include "GameClient/GXNoShroud.h"





//-----------------------------------------------------------------------------
//         Private Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DPropBuffer::cull
//=============================================================================
/** Culls the props, marking the visible flag.  If a prop becomes visible, it sets
it's sortKey */
//=============================================================================
void W3DPropBuffer::cull(CameraClass * camera)
{
	Int curProp;

	for (curProp=0; curProp<m_numProps; curProp++) {
		Bool visible = !camera->Cull_Sphere(m_props[curProp].bounds);
		m_props[curProp].visible=visible;
	}
}



//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DPropBuffer::~W3DPropBuffer
//=============================================================================
/** Destructor. Releases w3d assets. */
//=============================================================================
W3DPropBuffer::~W3DPropBuffer()
{
	clearAllProps();
	REF_PTR_RELEASE(m_light);
	REF_PTR_RELEASE(m_propShroudMaterialPass);
}

//=============================================================================
// W3DPropBuffer::W3DPropBuffer
//=============================================================================
/** Constructor. Sets m_initialized to true if it finds the w3d models it needs
for the props. */
//=============================================================================
W3DPropBuffer::W3DPropBuffer()
{
	m_initialized = false;
	m_numProps = 0;
	m_numPropTypes = 0;
	m_anythingChanged = false;
	m_doCull = true;
	m_shroudChanged = false;
	m_light = NEW_REF( LightClass, (LightClass::DIRECTIONAL) );
	m_propShroudMaterialPass = NEW_REF(W3DShroudMaterialPassClass,());
	m_initialized = true;
}





//=============================================================================
// W3DPropBuffer::clearAllProps
//=============================================================================
/** Removes all props. */
//=============================================================================
void W3DPropBuffer::clearAllProps()
{
	Int i;
	for (i=0; i<m_numPropTypes; i++) {
		REF_PTR_RELEASE(m_propTypes[i].m_robj);
		m_propTypes[i].m_robjName.clear();
	}
	for (i=0; i<m_numProps; i++) {
		REF_PTR_RELEASE(m_props[i].m_robj);
	}
	m_numPropTypes = 0;
	m_numProps = 0;
}

//=============================================================================
// W3DPropBuffer::addPropTypes
//=============================================================================
/** Adds a type of prop (model & texture). */
//=============================================================================
Int W3DPropBuffer::addPropType(const AsciiString &modelName)
{
	if (m_numPropTypes>=MAX_TYPES) {
		DEBUG_CRASH(("Too many kinds of props in map.  Reduce kinds of props, or raise prop limit. jba."));
		return 0;
	}

	m_propTypes[m_numPropTypes].m_robj = WW3DAssetManager::Get_Instance()->Create_Render_Obj(modelName.str());
	if (m_propTypes[m_numPropTypes].m_robj==nullptr) {
		DEBUG_CRASH(("Unable to find model for prop %s", modelName.str()));
		return -1;
	}
	m_propTypes[m_numPropTypes].m_robjName = modelName;

	SphereClass bounds = m_propTypes[m_numPropTypes].m_robj->Get_Bounding_Sphere();
	m_propTypes[m_numPropTypes].m_bounds = bounds;
	m_numPropTypes++;
	return m_numPropTypes-1;
}

//=============================================================================
// W3DPropBuffer::addProp
//=============================================================================
/** Adds a prop.  Name is the W3D model name, supported models are
ALPINE, DECIDUOUS and SHRUB. */
//=============================================================================
void W3DPropBuffer::addProp(Int id, Coord3D location, Real angle,Real scale, const AsciiString &modelName)
{
	if (m_numProps >= MAX_PROPS) {
		return;
	}
	if (!m_initialized) {
		return;
	}
	Int propType = -1;
	Int i;
	for (i=0; i<m_numPropTypes; i++) {
		if (m_propTypes[i].m_robjName.compareNoCase(modelName)==0) {
			propType = i;
			break;
		}
	}
	if (propType<0) {
		propType = addPropType(modelName);
		if (propType<0) {
			return;
		}
	}

	Matrix3D mtx(true);
	mtx.Rotate_Z(angle);
	mtx.Scale(scale);
	mtx.Set_Translation(Vector3(location.x, location.y, location.z));

	m_props[m_numProps].location = location;
	m_props[m_numProps].id = id;
	m_props[m_numProps].ss = OBJECTSHROUD_INVALID;
	m_props[m_numProps].m_robj = m_propTypes[propType].m_robj->Clone();
	m_props[m_numProps].m_robj->Set_Transform(mtx);
	m_props[m_numProps].m_robj->Set_ObjectScale(scale);
	m_props[m_numProps].propType = propType;
	// Translate the bounding sphere of the model.
	m_props[m_numProps].bounds = m_propTypes[propType].m_bounds;
	m_props[m_numProps].bounds.Center += Vector3(location.x, location.y, location.z);
	// Initially set it invisible.  cull will update it's visiblity flag.
	m_props[m_numProps].visible = false;

	m_numProps++;
}

//=============================================================================
// W3DPropBuffer::updatePropPosition
//=============================================================================
/** Updates a prop's position */
//=============================================================================
Bool W3DPropBuffer::updatePropPosition(Int id, const Coord3D &location, Real angle, Real scale)
{
	Int i;
	for (i=0; i<m_numProps; i++) {
		if (m_props[i].id == id) {
			Matrix3D mtx(true);
			mtx.Rotate_Z(angle);
			mtx.Scale(scale);
			mtx.Set_Translation(Vector3(location.x, location.y, location.z));
			m_props[i].location = location;
			m_props[i].m_robj->Set_Transform(mtx);
			m_props[i].m_robj->Set_ObjectScale(scale);
			// Translate the bounding sphere of the model.
			m_props[i].bounds = m_propTypes[m_props[i].propType].m_bounds;
			m_props[i].bounds.Center += Vector3(location.x, location.y, location.z);
			m_anythingChanged = true;
			return true;
		}
	}
	return false;
}

//=============================================================================
// W3DPropBuffer::removeProp
//=============================================================================
/** Removes a prop.  */
//=============================================================================
void W3DPropBuffer::removeProp(Int id)
{
	Int i;
	for (i=0; i<m_numProps; i++) {
		if (m_props[i].id == id) {
			m_props[i].location.set(0,0,0);
			m_props[i].propType = -1;
			REF_PTR_RELEASE(m_props[i].m_robj);
			// Translate the bounding sphere of the model.
			m_props[i].bounds.Center = Vector3(0,0,0);
			m_props[i].bounds.Radius = 1;
			m_anythingChanged = true;
		}
	}
}

//=============================================================================
// W3DPropBuffer::removePropsForConstruction
//=============================================================================
/** Removes any props that would be under a building.  */
//=============================================================================
void W3DPropBuffer::removePropsForConstruction(const Coord3D* pos, const GeometryInfo& geom, Real angle )
{
	// Just iterate all trees, as even non-collidable ones get removed. jba. [7/11/2003]
	Int i;
	for (i=0; i<m_numProps; i++) {
		if (m_props[i].m_robj == nullptr) {
			continue; // already deleted.
		}
		Real radius = m_props[i].bounds.Radius;
		GeometryInfo info(GEOMETRY_CYLINDER, false, 5*radius, 2*radius, 2*radius);
		if (ThePartitionManager->geomCollidesWithGeom( pos, geom, angle, &m_props[i].location, info, 0.0f)) {
			// remove it [7/11/2003]
			m_props[i].location.set(0,0,0);
			m_props[i].propType = -1;
			REF_PTR_RELEASE(m_props[i].m_robj);
			// Translate the bounding sphere of the model.
			m_props[i].bounds.Center = Vector3(0,0,0);
			m_props[i].bounds.Radius = 1;
			m_anythingChanged = true;
		}
	}
}



//=============================================================================
// W3DPropBuffer::notifyShroudChanged
//=============================================================================
/** Sets the shroud to status, so it is recomputed.  */
//=============================================================================
void W3DPropBuffer::notifyShroudChanged()
{
	// GeneralsX: This is called once per changed shroud CELL (W3DDisplay::setShroudLevel), up to
	// the whole partition grid from refreshShroudForLocalPlayer. Defer the O(numProps) invalidation to
	// drawProps, the only reader of TProp::ss, so it runs at most once per frame.
	m_shroudChanged = true;
}


DECLARE_PERF_TIMER(Prop_Render)

//=============================================================================
// W3DPropBuffer::drawProps
//=============================================================================
/** Draws the props.  Uses camera to cull. */
//=============================================================================
void W3DPropBuffer::drawProps(RenderInfoClass &rinfo)
{
	USE_PERF_TIMER(Prop_Render)

	Int i;
	if (m_doCull) {
		cull(&rinfo.Camera);
	}
	if (m_shroudChanged) {
		m_shroudChanged = false;
		for (i=0; i<m_numProps; i++) {
			m_props[i].ss = ThePartitionManager?OBJECTSHROUD_INVALID:OBJECTSHROUD_CLEAR;
		}
	}
	const GlobalData::TerrainLighting *objectLighting = TheGlobalData->m_terrainObjectsLighting[TheGlobalData->m_timeOfDay];

	LightEnvironmentClass lightEnv;
	Vector3 center(0,0,0); // arbitrary center point. [6/6/2003]
	Vector3 ambient(objectLighting[0].ambient.red, objectLighting[0].ambient.green, objectLighting[0].ambient.blue);
	lightEnv.Reset(center, ambient);

	Matrix3D mtx;
	const Vector3 zeroVector(0.0f, 0.0f, 0.0f);
	const Vector3 xVector(1.0f, 0.0f, 0.0f);
	const Vector3 yVector(0.0f, 1.0f, 0.0f);

	for (i = 0; i < MAX_GLOBAL_LIGHTS; ++i)
	{
			m_light->Set_Ambient(zeroVector);
			m_light->Set_Diffuse(Vector3(objectLighting[i].diffuse.red,
																		 objectLighting[i].diffuse.green,
																		 objectLighting[i].diffuse.blue));
			m_light->Set_Specular(zeroVector);
			mtx.Set(xVector, yVector, Vector3(objectLighting[i].lightPos.x, objectLighting[i].lightPos.y, objectLighting[i].lightPos.z), zeroVector);
			m_light->Set_Transform(mtx);
			lightEnv.Add_Light(*m_light);
	}

	rinfo.light_environment = &lightEnv;
	for	(i=0; i<m_numProps; i++) {
		if (!m_props[i].visible) {
			continue;
		}
		if (m_props[i].m_robj==nullptr) {
			continue;
		}
		if (!ThePlayerList || !ThePartitionManager) {
			//  in Worldbuilder. No shroud either. jba. [6/9/2003]
			m_props[i].ss = OBJECTSHROUD_CLEAR;
		}
#if ENABLE_CONFIGURABLE_SHROUD
		else if (!gxShroudDrawn()) {
			// -noshroud / GX_NOSHROUD: every prop draws on this client; the sim's shroud grid is untouched
			m_props[i].ss = OBJECTSHROUD_CLEAR;
		}
#endif
		if (m_props[i].ss == OBJECTSHROUD_INVALID) {
			const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();
			m_props[i].ss = ThePartitionManager->getPropShroudStatusForPlayer(localPlayerIndex, &m_props[i].location);
			// GeneralsX: CLEAR now skips the shroud pass (below), so it must hold for the
			// whole prop, not just the 2x2 cells at its origin: a big or scaled prop straddling a fog edge would
			// draw its fogged part fully lit. Check every cell the shroud texture is
			// bilinearly sampled from within the prop's radius (rotation-proof: the type sphere's offset plus
			// its radius, times the prop's scale). Off-map cells read SHROUDED, so edge props keep the pass.
			if (m_props[i].ss == OBJECTSHROUD_CLEAR && m_props[i].propType >= 0) {
				// Trade-off: a prop wider than this many cells simply keeps the pass (rare, and the old behaviour)
				enum { MAX_FOOTPRINT_CELLS = 16 };
				const SphereClass &typeBounds = m_propTypes[m_props[i].propType].m_bounds;
				const Real r = (typeBounds.Center.Length() + typeBounds.Radius) * m_props[i].m_robj->Get_ObjectScale();
				const Real reach = r + ThePartitionManager->getCellSize() * 0.5f;
				const Coord3D &at = m_props[i].location;
				Int x0, y0, x1, y1;
				ThePartitionManager->worldToCell(at.x - reach, at.y - reach, &x0, &y0);
				ThePartitionManager->worldToCell(at.x + reach, at.y + reach, &x1, &y1);
				if (x1 - x0 > MAX_FOOTPRINT_CELLS || y1 - y0 > MAX_FOOTPRINT_CELLS) {
					m_props[i].ss = OBJECTSHROUD_PARTIAL_CLEAR;
				}
				for (Int cy = y0; cy <= y1 && m_props[i].ss == OBJECTSHROUD_CLEAR; ++cy) {
					for (Int cx = x0; cx <= x1; ++cx) {
						if (ThePartitionManager->getShroudStatusForPlayer(localPlayerIndex, cx, cy) != CELLSHROUD_CLEAR) {
							m_props[i].ss = OBJECTSHROUD_PARTIAL_CLEAR;
							break;
						}
					}
				}
			}
		}
		if (m_props[i].ss >= OBJECTSHROUD_SHROUDED) {
			continue;
		}
		if (m_props[i].ss <= OBJECTSHROUD_INVALID) {
			continue;
		}
		// GeneralsX: ss is an ObjectShroudStatus. It was compared to CELLSHROUD_CLEAR,
		// which is 0 like OBJECTSHROUD_INVALID, already skipped above, so every visible prop drew the shroud pass.
		// A map or mod with ClearAlpha below 255 tints even clear cells, so there the pass still has work to do.
		// GeneralsX: ... or while the drawn fog under a CLEAR prop is still fading (fog interpolation,
		// W3DShroud::isFadingUnder). ss is cached until the next logical change; the fade is not. The
		// test uses the scale-aware footprint of the CLEAR test above: a mesh's cached sphere keeps its unscaled radius
		Bool fading = FALSE;
		if (TheTerrainRenderObject->getShroud() && m_props[i].ss == OBJECTSHROUD_CLEAR && TheGlobalData->m_clearAlpha == 255) {
			SphereClass footprint = m_props[i].m_robj->Get_Bounding_Sphere();
			if (m_props[i].propType >= 0) {
				const SphereClass &typeBounds = m_propTypes[m_props[i].propType].m_bounds;
				const Coord3D &at = m_props[i].location;
				footprint = SphereClass(Vector3(at.x, at.y, at.z),
					(typeBounds.Center.Length() + typeBounds.Radius) * m_props[i].m_robj->Get_ObjectScale());
			}
			fading = TheTerrainRenderObject->getShroud()->isFadingUnder(footprint);
		}
		if (TheTerrainRenderObject->getShroud() && (m_props[i].ss != OBJECTSHROUD_CLEAR || TheGlobalData->m_clearAlpha != 255 || fading)) {
			rinfo.Push_Material_Pass(m_propShroudMaterialPass);
			m_props[i].m_robj->Render(rinfo);
			rinfo.Pop_Material_Pass();
		} else {
			m_props[i].m_robj->Render(rinfo);
		}
	}
	rinfo.light_environment = nullptr;

}


// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void W3DPropBuffer::crc( Xfer *xfer )
{
	// empty. jba [8/11/2003]
}

// ------------------------------------------------------------------------------------------------
/** Xfer
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void W3DPropBuffer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );


}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void W3DPropBuffer::loadPostProcess()
{
	// empty. jba [8/11/2003]
}

