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

// FILE: W3DView.cpp //////////////////////////////////////////////////////////////////////////////
//
// W3D implementation of the game view class.  This view allows us to have
// a "window" into the game world that can change its width, height as
// well as camera positioning controls
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////////////////////////
#include <chrono>
#if !defined(_WIN32)
#include <execinfo.h>	// GX_CAMTEST: who ends a test move (gxCamTestWhoEnds)
#endif
#include <stdlib.h>
#include <windows.h>

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"

#include "Common/BuildAssistant.h"
#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "GameClient/GXNoShroud.h"
#include "Common/Module.h"
#include "Common/Radar.h"
#include "Common/RandomValue.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingSort.h"
#include "Common/PerfTimer.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"

#include "GameClient/Color.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/CameraStepPolicy.h"
#include "GameClient/GXAnimTrace.h"
#include "GameClient/GXClientAnimPolicy.h"
#include "Common/ScriptedCameraClock.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GXHostCamera.h"
#include "GameClient/GXWholeMapView.h"
#include "GameClient/GXPickProbe.h"
#include "GameClient/Image.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Line2D.h"
#include "GameClient/SelectionInfo.h"
#include "GameClient/Shell.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/Water.h"

#include "GameLogic/ScriptedCamera.h"
#include "GameLogic/AI.h"			///< For AI debug (yes, I'm cheating for now)
#include "GameLogic/AIPathfind.h"			///< For AI debug (yes, I'm cheating for now)
#include "GameLogic/ExperienceTracker.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/OpenContain.h"
#include "GameLogic/Object.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/TerrainLogic.h"									///< @todo This should be TerrainVisual (client side)
#include "Common/AudioEventInfo.h"

#include "W3DDevice/Common/W3DConvert.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "W3DDevice/GameClient/W3DDisplay.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DView.h"
#include "W3DDevice/GameClient/W3DWorldSeam.h"
#include "W3DDevice/GameClient/GXShadowMapHooks.h"   // S7
#include "d3dx8math.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/Module/W3DModelDraw.h"
#include "W3DDevice/GameClient/W3DCustomScene.h"

#include "WW3D2/dx8renderer.h"
#include "WW3D2/light.h"
#include "WW3D2/predlod.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/Renderer.h"

#include "W3DDevice/GameClient/CameraShakeSystem.h"
#include "GameClient/GXRenderSplit.h"	// GeneralsX: render-split step 6: shakes from the tick

// 30 fps
Real TheW3DFrameLengthInMsec = MSEC_PER_LOGICFRAME_REAL; // default is 33msec/frame == 30fps. but we may change it depending on sys config.
static const Real DRAWABLE_OVERSCAN = 75.0f;  ///< 3D world coords of how much to overscan in the 3D screen region

constexpr const Real NearZ = MAP_XY_FACTOR; ///< Set the near to MAP_XY_FACTOR. Improves z buffer resolution.
constexpr const Real GX_CAMERA_AREA_OFFSET_SCALE = 0.85f; ///< calcCameraAreaOffset's scale; the whole-map cap models it

//-------------------------------------------------------------------------------------------------
// Normalizes angle to +- PI.
//-------------------------------------------------------------------------------------------------
static void normAngle(Real &angle)
{
	angle = WWMath::Normalize_Angle(angle);
}


Real W3DView::getHeightAroundPos(Real x, Real y, Real terrainSampleSize) const
{
	Real terrainHeight = TheTerrainLogic->getGroundHeight(x, y);

#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	// TheSuperHackers @info xezon 06/12/2025 To preserve the exact look of the original scripted camera
	// it is not possible to change the terrain height sampling method during cinematic scenes.
	const Bool useSmoothTerrainSample = m_isUserControlled;
#else
	const Bool useSmoothTerrainSample = true;
#endif
	if (useSmoothTerrainSample)
	{
		// TheSuperHackers @tweak Now finds approximation of average terrain height instead of maximum terrain height.
		// 4 additional sample points around the center point will be averaged.
		// (3)   (2)
		//    (1)   
		// (5)   (4)
		terrainHeight += TheTerrainLogic->getGroundHeight(x+terrainSampleSize, y-terrainSampleSize);
		terrainHeight += TheTerrainLogic->getGroundHeight(x-terrainSampleSize, y-terrainSampleSize);
		terrainHeight += TheTerrainLogic->getGroundHeight(x+terrainSampleSize, y+terrainSampleSize);
		terrainHeight += TheTerrainLogic->getGroundHeight(x-terrainSampleSize, y+terrainSampleSize);
		terrainHeight /= 5;
	}
	else
	{
		// find best approximation of max terrain height we can see
		terrainHeight = max(terrainHeight, TheTerrainLogic->getGroundHeight(x+terrainSampleSize, y-terrainSampleSize));
		terrainHeight = max(terrainHeight, TheTerrainLogic->getGroundHeight(x-terrainSampleSize, y-terrainSampleSize));
		terrainHeight = max(terrainHeight, TheTerrainLogic->getGroundHeight(x+terrainSampleSize, y+terrainSampleSize));
		terrainHeight = max(terrainHeight, TheTerrainLogic->getGroundHeight(x-terrainSampleSize, y+terrainSampleSize));
	}

	return terrainHeight;
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// GeneralsX: S7 (GXShadowMapHooks.h): defined here (Core, both games); the ZH glue installs them.
GxShadowMapFrameHooks *GxShadowMapFrame = nullptr;
CameraClass *GxShadowMapArmedCamera = nullptr;	// A49: armed here, fired by the scene after its On_Frame_Update loop
bool GxShadowMapXNoFire = false;	// A50 TEST-ONLY (GXShadowMapHooks.h)
SceneClass *GxShadowMapArmedScene = nullptr;	// A51 (GXShadowMapHooks.h)
SceneClass *GxShadowMapFrameScene = nullptr;
bool GxShadowMapActive = false;
bool GxShadowMapTerrainChanged = false;

W3DView::W3DView()
{

	m_3DCamera = nullptr;
	m_2DCamera = nullptr;
	m_gxCamStepFrame = 0xffffffff;	// the scripted camera clock (stepScriptedCamera): nothing stepped yet
	m_gxCamPoseFrame = 0xffffffff;
	m_gxCamDrawnFrame = 0xffffffff;
	m_gxCamContinues = FALSE;
	m_gxCamPrev.pos.zero();
	m_gxCamPrev.angle = m_gxCamPrev.zoom = m_gxCamPrev.fxPitch = 0.0f;
	m_gxCamCur = m_gxCamPrev;
	m_gxBlurJumpPos.zero();
	m_gxCamDrawnLookAt.zero();
	m_camTimeline.reset(MSEC_PER_LOGICFRAME_REAL);	// no move, multiplier 1 (its divider starts at 1, as W3DDisplay::draw's), no freeze
	m_scriptedCameraLock = FALSE;
	m_lastDrawLoopSeconds = 0.0;
	m_drawFpsRatio = 0.0f;

#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	m_initialGroundLevel = 10.0f;
#endif

	m_viewFilterMode = FM_VIEW_DEFAULT;
	m_viewFilter = FT_VIEW_DEFAULT;
	m_isWireFrameEnabled = m_nextWireFrameEnabled = FALSE;
	m_shakeOffset.x = 0.0f;
	m_shakeOffset.y = 0.0f;
	m_shakeIntensity = 0.0f;
	m_FXPitch = 1.0f;
	m_lastScreenToTerrainValid = false;

	//Enhancements from CNC3 WST 4/15/2003. JSC Integrated 5/20/03.
	m_CameraArrivedAtWaypointOnPathFlag = false;	// Scripts for polling camera reached targets
	m_isCameraSlaved = false;						// This is for 3DSMax camera playback
	m_useRealZoomCam = false;						// true;	//WST 10/18/2002
	m_shakerAngles.X =0.0f;							// Proper camera shake generator & sources
	m_shakerAngles.Y =0.0f;
	m_shakerAngles.Z =0.0f;
	m_drawWalkValid = FALSE;
	m_terrainDrawSize.x = 0;
	m_terrainDrawSize.y = 0;
	m_terrainDrawScale = 0.0f;
	m_terrainDrawEntire = FALSE;
	m_terrainDrawMaxHeight = 0.0f;
	m_gxUncappedMax = 0.0f;
	m_gxCappedMax = 0.0f;
	m_gxCapSetting = 0.0f;
	m_gxCapPlayerTop = 0.0f;
	m_gxCapExtentX = 0.0f;
	m_gxCapExtentY = 0.0f;
	m_drawWalkChangeCount = 0;

	m_cameraAreaConstraints.zero();
	m_recalcCamera = false;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
W3DView::~W3DView()
{
	CameraShakerSystem.Reset();

	REF_PTR_RELEASE( m_2DCamera );
	REF_PTR_RELEASE( m_3DCamera );

}

//-------------------------------------------------------------------------------------------------
/** Sets the height of the viewport, while maintaining original camera perspective. */
//-------------------------------------------------------------------------------------------------
void W3DView::setHeight(Int height)
{
	// extend View functionality
	View::setHeight(height);

	Vector2 vMin,vMax;
	m_3DCamera->Set_Aspect_Ratio((Real)getWidth()/(Real)height);
 	m_3DCamera->Get_Viewport(vMin,vMax);
 	vMax.Y=(Real)(m_originY+height)/(Real)TheDisplay->getHeight();
 	m_3DCamera->Set_Viewport(vMin,vMax);

	// TheSuperHackers @bugfix Now recalculates the camera constraints because
	// showing or hiding the control bar will change the viewable area.
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
	m_lastScreenToTerrainValid = false;
	recapToWholeMap();
}

//-------------------------------------------------------------------------------------------------
/** Sets the width of the viewport, while maintaining original camera perspective. */
//-------------------------------------------------------------------------------------------------
void W3DView::setWidth(Int width)
{
	// extend View functionality
	View::setWidth(width);

	Vector2 vMin,vMax;
	m_3DCamera->Set_Aspect_Ratio((Real)width/(Real)getHeight());
 	m_3DCamera->Get_Viewport(vMin,vMax);
 	vMax.X=(Real)(m_originX+width)/(Real)TheDisplay->getWidth();
 	m_3DCamera->Set_Viewport(vMin,vMax);

	//we want to maintain the same scale, so we'll need to adjust the fov.
	//default W3D fov for full-screen is 50 degrees.
	m_3DCamera->Set_View_Plane((Real)width/(Real)TheDisplay->getWidth()*DEG_TO_RADF(50.0f),-1);

	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
	m_lastScreenToTerrainValid = false;
	recapToWholeMap();
}

//-------------------------------------------------------------------------------------------------
/** Sets location of top-left view corner on display */
//-------------------------------------------------------------------------------------------------
void W3DView::setOrigin( Int x, Int y)
{
	// extend View functionality
	View::setOrigin(x,y);

	Vector2 vMin,vMax;

 	m_3DCamera->Get_Viewport(vMin,vMax);
 	vMin.X=(Real)x/(Real)TheDisplay->getWidth();
	vMin.Y=(Real)y/(Real)TheDisplay->getHeight();
 	m_3DCamera->Set_Viewport(vMin,vMax);

	// bottom-right border was also moved my this call, so force an update of extents.
	setWidth(m_width);
	setHeight(m_height);
}

//-------------------------------------------------------------------------------------------------
/** @todo This is inefficient. We should construct the matrix directly using vectors. */
//-------------------------------------------------------------------------------------------------
#define MIN_CAPPED_ZOOM (0.5f) //WST 10.19.2002. JSC integrated 5/20/03.
void W3DView::buildCameraPosition( Vector3& sourcePos, Vector3& targetPos )
{
	const Real zoom = getZoom();
	const Real angle = getAngle();
	const Real pitch = getPitch();
	Coord3D pos = getPosition();

	// add in the camera shake, if any
	pos.x += m_shakeOffset.x;
	pos.y += m_shakeOffset.y;

	// TheSuperHackers @info The default pitch affects the look-at distance to the target.
	// This is strange math which would need special attention when changed.
	sourcePos.Z = getCameraOffsetZ();
	sourcePos.Y = -(sourcePos.Z / tan(ViewDefaultPitchRadians));
	sourcePos.X = -(sourcePos.Y * tan(ViewDefaultYawRadians));

	// set position of camera itself
	if (m_useRealZoomCam) //WST 10/10/2002 Real Zoom using FOV
	{
		Real cappedZoom = clamp(MIN_CAPPED_ZOOM, zoom, 1.0f);
		m_FOV = DEG_TO_RADF(50.0f) * cappedZoom * cappedZoom;
	}
	else
	{
		sourcePos.X *= zoom;
		sourcePos.Y *= zoom;
		sourcePos.Z *= zoom;
	}


	// TheSuperHackers @info Scales the source position later by this much
	// to achieve the intended camera height. Must not scale before pitching!
	const Real heightScale = 1.0f - (pos.z / sourcePos.Z);

	// construct a matrix to rotate around the up vector by the given angle
	const Matrix3D angleTransform( Vector3( 0.0f, 0.0f, 1.0f ), angle - ViewDefaultYawRadians );

	// construct a matrix to rotate around the left vector by the given angle
	const Matrix3D pitchTransform( Vector3( -1.0f, 0.0f, 0.0f ), pitch - ViewDefaultPitchRadians );

	// rotate camera position (pitch, then angle)
#ifdef ALLOW_TEMPORARIES
	sourcePos = pitchTransform * sourcePos;
	sourcePos = angleTransform * sourcePos;
#else
	pitchTransform.mulVector3(sourcePos);
	angleTransform.mulVector3(sourcePos);
#endif

	sourcePos *= heightScale;

	// set look at position
	targetPos.X = pos.x;
	targetPos.Y = pos.y;
	targetPos.Z = pos.z;

	// translate to world space
	sourcePos += targetPos;

	// do m_FXPitch adjustment.
	//WST Real height = sourcePos.Z - targetPos.Z;
	//WST height *= m_FXPitch;
	//WST targetPos.Z = sourcePos.Z - height;

	// The following code moves camera down and pitch up when player zooms in.
	// Use scripts to switch to useRealZoomCam
	if (m_useRealZoomCam)
	{
		Real pitchAdjust = 1.0f;

		if (!TheDisplay->isLetterBoxed())
		{
			Real cappedZoom = clamp(MIN_CAPPED_ZOOM, zoom, 1.0f);
			sourcePos.Z = sourcePos.Z * (0.5f + cappedZoom * 0.5f); // move camera down physically
			pitchAdjust = cappedZoom; // adjust camera to pitch up
		}
		m_FXPitch = 1.0f * (0.25f + pitchAdjust*0.75f);
		sourcePos.X = targetPos.X + ((sourcePos.X - targetPos.X) / m_FXPitch);
		sourcePos.Y = targetPos.Y + ((sourcePos.Y - targetPos.Y) / m_FXPitch);
	}
	else
	{
		// TheSuperHackers @todo Investigate whether the non Generals code is correct for Zero Hour.
		// It certainly is incorrect for Generals when m_FXPitch goes above 1:
		// Seen in USA mission 1 second cut scene with SCUD Storm.
#if RTS_GENERALS
		Real height = sourcePos.Z - targetPos.Z;
		height *= m_FXPitch;
		targetPos.Z = sourcePos.Z - height;
#else
		if (m_FXPitch <= 1.0f)
		{
			targetPos.Z = sourcePos.Z - ((sourcePos.Z - targetPos.Z) * m_FXPitch);
		}
		else
		{
			sourcePos.X = targetPos.X + ((sourcePos.X - targetPos.X) / m_FXPitch);
			sourcePos.Y = targetPos.Y + ((sourcePos.Y - targetPos.Y) / m_FXPitch);
		}
#endif
	}
}

void W3DView::buildCameraTransform( Matrix3D *transform, const Vector3 &sourcePos, const Vector3 &targetPos )
{
	//m_3DCamera->Set_View_Plane(DEG_TO_RADF(50.0f));

	// build new camera transform
	transform->Make_Identity();
	transform->Look_At( sourcePos, targetPos, 0 );

	//WST 11/12/2002 New camera shaker system
	// TheSuperHackers @tweak The camera shaker is now decoupled from the render update.
	// TheSuperHackers @todo Move Update_Camera_Shaker to the W3DView::update function.
	// GeneralsX: GX_SHOT capture clock (GameClient.cpp): the pacer's value unless capturing.
	CameraShakerSystem.Timestep(gxRenderStepMs());
	CameraShakerSystem.Update_Camera_Shaker(sourcePos, &m_shakerAngles);
	transform->Rotate_X(m_shakerAngles.X);
	transform->Rotate_Y(m_shakerAngles.Y);
	transform->Rotate_Z(m_shakerAngles.Z);

	//if (m_shakerAngles.X >= 0.0f)
	//{
	//	DEBUG_LOG(("m_shakerAngles %f, %f, %f", m_shakerAngles.X, m_shakerAngles.Y, m_shakerAngles.Z));
	//}

	// (gth) check if the camera is being controlled by an animation
	if (m_isCameraSlaved) {
		// find object named m_cameraSlaveObjectName
		Object * obj = TheScriptEngine->getUnitNamed(m_cameraSlaveObjectName);

		if (obj != nullptr) {
			// dig out the drawable
			Drawable * draw = obj->getDrawable();
			if (draw != nullptr) {

				// dig out the first draw module with an ObjectDrawInterface
				for (DrawModule ** dm = draw->getDrawModules(); *dm; ++dm) {
					const ObjectDrawInterface* di = (*dm)->getObjectDrawInterface();
					if (di) {
						Matrix3D tm;
						di->clientOnly_getRenderObjBoneTransform(m_cameraSlaveObjectBoneName,&tm);

						// Ok, slam it into the camera!
						*transform = tm;

						//--------------------------------------------------------------------
						// WST 10.22.2002. Update the Listener positions used by audio system
						//--------------------------------------------------------------------
						Vector3 position = transform->Get_Translation();
						Coord2D coord = { position.X, position.Y };
						View::setPosition2D(coord);
						break;
					}
				}

			} else {
				m_isCameraSlaved = false;
			}
		} else {
			m_isCameraSlaved = false;
		}
	}
}

// TheSuperHackers @info Original logic responsible for zooming the camera to the desired height.
// GeneralsX: raise the zoom by GX_GORGE_ZOOM_OUT x how much higher the ground under the camera body (5-sample average)
// is than at the look-at point, so the body clears a rim behind it; never closer than m_minHeightAboveGround.
// Nothing on flat ground or uphill. User camera only (scripted and replay cameras untouched); capped at the max zoom.
static const Real GX_GORGE_ZOOM_OUT = 1.5f;	// x the extra height of the ground under the camera; 1.0 = keep the zoom height over it

Real W3DView::gxBodyClearedZoom(Real desiredZoom)
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	if (!m_isUserControlled)
		return desiredZoom;
#endif
	const Real offsetZ = getCameraOffsetZ();
	const Real h0 = offsetZ * m_zoom - m_pos.z;
	if (m_useRealZoomCam || offsetZ <= 0.0f || h0 <= 1.0f || TheTerrainLogic == nullptr)
		return desiredZoom;
	Vector3 sourcePos;
	Vector3 targetPos;
	buildCameraPosition(sourcePos, targetPos);
	// the body's offset from the PIVOT per unit of h: measured from m_pos.z, not targetPos.Z, which an FX pitch below 1 moves with
	// the zoom (an FX pitch above 1 scales the body's offset by a constant, which u absorbs)
	const Vector3 u(sourcePos.X - targetPos.X, sourcePos.Y - targetPos.Y, sourcePos.Z - m_pos.z);
	const Real pivotZ = m_pos.z;
	if (u.Z < 0.05f * h0)
		return desiredZoom;	// a camera looking nearly level: leave it to the NearZ clamp

	const Vector3 uh = u * (1.0f / h0);
	const Real hWanted = offsetZ * desiredZoom - m_pos.z;
	const Real maxZoom = getMaxZoom(m_pos.x, m_pos.y);
	const Real hMax = offsetZ * maxZoom - m_pos.z;
	// For the ground under the body at height h: floorOf = the least h at which the body clears it by the minimum zoom height
	// (the physical floor); goalOf = the valley rule's height, k x its rise over the look-at's ground; needAt(h) = both. A
	// height h is good when needAt(h) <= h.
	auto groundAt = [&](Real hh) -> Real
	{
		return getHeightAroundPos(targetPos.X + uh.X * hh, targetPos.Y + uh.Y * hh, TERRAIN_SAMPLE_SIZE);
	};
	auto floorOf = [&](Real ground) -> Real { return (ground + m_minHeightAboveGround - pivotZ) / uh.Z; };
	auto goalOf = [&](Real ground) -> Real
	{
		const Real rise = ground - pivotZ;	// how much higher the ground under the body is than the look-at's
		return hWanted + (rise > 0.0f ? GX_GORGE_ZOOM_OUT * rise / uh.Z : 0.0f);
	};
	auto needAt = [&](Real hh) -> Real
	{
		const Real ground = groundAt(hh);
		const Real hFloor = floorOf(ground);
		const Real hGoal = goalOf(ground);
		return (hFloor > hGoal) ? hFloor : hGoal;
	};
	// The ground behind the camera is arbitrary (ridges make needAt non-monotonic), so the range is SCANNED rather than chased:
	// 24 even steps from the wanted height up to the max, the first good one refined by bisection against the step before it
	// (which was bad); if none is good, the best fallback below (an unchecked, an exact, a
	// too-short and an overshooting climb each picked a wrong height).
	if (needAt(hWanted) <= hWanted || hMax <= hWanted)
		return desiredZoom;
	enum { GX_CLEAR_STEPS = 24 };
	// the fallback ranks a height that meets the physical floor above one that does not, then by the smaller shortfall (of
	// the valley goal for the first kind, of the floor for the second)
	Real h = hWanted;
	Bool bestMeetsFloor = false;
	Real bestShort = 0.0f;
	auto consider = [&](Real hh)
	{
		const Real ground = groundAt(hh);
		const Bool meetsFloor = floorOf(ground) <= hh;
		const Real shortfall = meetsFloor ? goalOf(ground) - hh : floorOf(ground) - hh;
		if ((meetsFloor && !bestMeetsFloor) || (meetsFloor == bestMeetsFloor && shortfall < bestShort))
		{
			bestMeetsFloor = meetsFloor;
			bestShort = shortfall;
			h = hh;
		}
	};
	{
		// the wanted height itself ranks first, so a correction never trades a camera that meets the floor for one that does not
		// then the max
		const Real ground = groundAt(hWanted);
		bestMeetsFloor = floorOf(ground) <= hWanted;
		bestShort = bestMeetsFloor ? goalOf(ground) - hWanted : floorOf(ground) - hWanted;
		h = hWanted;
	}
	consider(hMax);
	Real prevH = hWanted;
	for (Int i = 1; i <= GX_CLEAR_STEPS; ++i)
	{
		const Real hi = hWanted + (hMax - hWanted) * (Real)i / (Real)GX_CLEAR_STEPS;
		const Real shortfall = needAt(hi) - hi;
		if (shortfall <= 0.0f)
		{
			Real lo = prevH, up = hi;	// needAt(lo) > lo, needAt(up) <= up
			for (Int step = 0; step < 12 && up - lo > 1.0f; ++step)
			{
				const Real mid = 0.5f * (lo + up);
				if (needAt(mid) <= mid)
					up = mid;
				else
					lo = mid;
			}
			h = up;
			break;
		}
		consider(hi);
		prevH = hi;
	}
	const Real cleared = (h + m_pos.z) / offsetZ;
	if (cleared <= desiredZoom)
		return desiredZoom;
	// never past the max zoom, at it included; a desired zoom already above the max is left as it was
	if (cleared > maxZoom)
		return (desiredZoom > maxZoom) ? desiredZoom : maxZoom;
	return cleared;
}

Bool W3DView::zoomCameraToDesiredHeight()
{
	const Real desiredZoom = gxBodyClearedZoom(getDesiredZoom(m_pos.x, m_pos.y));
	const Real adjustZoom = desiredZoom - m_zoom;
	// GeneralsX @feature The zoom is normalised by the max camera height setting (getCameraOffsetZ), so a fixed 0.001 would
	// stop up to 50 units short with the Debug menu's 50000: past the player's 1000 the tolerance shrinks with the setting.
	const Real effectiveMax = getEffectiveMaxCameraHeight();
	const Real settleTolerance = (effectiveMax > (Real)CameraMaxHeightHi) ? 0.001f * (Real)CameraMaxHeightHi / effectiveMax : 0.001f;
	if (fabs(adjustZoom) >= settleTolerance)
	{
		// GeneralsX: Steps by the time since the last draw, not the last iteration (update()).
		const Real fpsRatio = m_drawFpsRatio;
		const Real adjustFactor = std::min(TheGlobalData->m_cameraAdjustSpeed * fpsRatio, 1.0f);
		m_zoom += adjustZoom * adjustFactor;
		return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @bugfix New logic responsible for moving the camera pivot to the terrain ground.
// This is essential to correctly center the camera above the ground when playing.
Bool W3DView::movePivotToGround()
{
	const Real groundLevel = m_pos.z;
	const Real groundLevelDiff = m_terrainHeightAtPivot - groundLevel;
	if (fabs(groundLevelDiff) > 0.1f)
	{
		// GeneralsX: Steps by the time since the last draw, not the last iteration (update()).
		const Real fpsRatio = m_drawFpsRatio;
		const Real adjustFactor = std::min(TheGlobalData->m_cameraAdjustSpeed * fpsRatio, 1.0f);
		// Adjust the ground level. This will change the world height of the camera.
		m_pos.z += groundLevelDiff * adjustFactor;

		// Reposition the camera relative to its pitch.
		// This effectively zooms the camera in the view direction together with the ground level change.
		Vector3 sourcePos;
		Vector3 targetPos;
		buildCameraPosition(sourcePos, targetPos);
		const Vector3 delta = targetPos - sourcePos;

		if (fabs(delta.Z) > 0.1f)
		{
			Vector2 groundLevelCenter;
			Vector2 terrainHeightCenter;
			groundLevelCenter.X = Vector3::Find_X_At_Z(groundLevel, sourcePos, targetPos);
			groundLevelCenter.Y = Vector3::Find_Y_At_Z(groundLevel, sourcePos, targetPos);
			terrainHeightCenter.X = Vector3::Find_X_At_Z(m_terrainHeightAtPivot, sourcePos, targetPos);
			terrainHeightCenter.Y = Vector3::Find_Y_At_Z(m_terrainHeightAtPivot, sourcePos, targetPos);
			Vector2 posDiff = terrainHeightCenter - groundLevelCenter;

			// Adjust the strength of the repositioning for low camera pitch, because
			// it feels bad to move the camera around when it looks over the terrain.
			const Real pitch = asin(fabs(delta.Z) / delta.Length());
			constexpr const Real lowerPitch = DEG_TO_RADF(15.f);
			constexpr const Real upperPitch = DEG_TO_RADF(30.f);
			Real repositionStrength = WWMath::Inverse_Lerp(lowerPitch, upperPitch, pitch);
			repositionStrength = WWMath::Clamp(repositionStrength, 0.0f, 1.0f);
			posDiff *= repositionStrength;

			Coord2D pos = getPosition2D();
			pos.x += posDiff.X * adjustFactor;
			pos.y += posDiff.Y * adjustFactor;
			setPosition2D(pos);
		}

		return true;
	}
	return false;
}

void W3DView::updateCameraAreaConstraints()
{
#if defined(RTS_DEBUG)
	if (!TheGlobalData->m_useCameraConstraints)
		return;
#endif

	if (!m_cameraAreaConstraintsValid)
	{
		calcCameraAreaConstraints();
	}

	if (m_cameraAreaConstraintsValid && !isWithinCameraAreaConstraints())
	{
		clipCameraIntoAreaConstraints();
		m_recalcCamera = true;
	}
}

//-------------------------------------------------------------------------------------------------
/*
Note the following restrictions on camera constraints!

-- they assume that all maps are raised to the common ground level at the edges.
		(since you need to add some "buffer" around the edges of your map
		anyway, this shouldn't be an issue.)

-- for angles/pitches other than zero, it may show boundaries.
		since we currently plan the game to be restricted to this,
		it shouldn't be an issue.
*/
void W3DView::calcCameraAreaConstraints()
{
//	DEBUG_LOG(("*** rebuilding cam constraints"));

	// ok, now check to ensure that we can't see outside the map region,
	// and twiddle the camera if needed
	if (TheTerrainLogic)
	{
		Region3D mapRegion;
		TheTerrainLogic->getExtent( &mapRegion );

		// GeneralsX @feature A script that switches the map's active boundary (ScriptActions::doBorderSwitch) lands here
		// through the constraints it invalidates: the whole-map cap is measured on the boundary, so take it again.
		if (isWholeMapCapInForce() && (mapRegion.hi.x != m_gxCapExtentX || mapRegion.hi.y != m_gxCapExtentY))
			recapToWholeMap();

		// Update the 3D camera before using its transform to calculate the constraints with.
		Vector3 sourcePos;
		Vector3 targetPos;
		buildCameraPosition(sourcePos, targetPos);
		Matrix3D cameraTransform;
		buildCameraTransform(&cameraTransform, sourcePos, targetPos);

		Matrix3D prevCameraTransform = m_3DCamera->Get_Transform();
		m_3DCamera->Set_Transform(cameraTransform);

		Real offset = calcCameraAreaOffset(m_pos.z);
		offset = std::min(offset, (mapRegion.hi.x - mapRegion.lo.x) / 2);
		offset = std::min(offset, (mapRegion.hi.y - mapRegion.lo.y) / 2);

		// Revert the 3D camera transform.
		m_3DCamera->Set_Transform(prevCameraTransform);

		m_cameraAreaConstraints.lo.x = mapRegion.lo.x + offset;
		m_cameraAreaConstraints.hi.x = mapRegion.hi.x - offset;
		m_cameraAreaConstraints.lo.y = mapRegion.lo.y + offset;
		m_cameraAreaConstraints.hi.y = mapRegion.hi.y - offset;

		m_cameraAreaConstraintsValid = true;
	}
}

//-------------------------------------------------------------------------------------------------
Real W3DView::calcCameraAreaOffset(Real maxEdgeZ)
{
	Coord2D center;
	ICoord2D screen;
	Vector3 rayStart;
	Vector3 rayEnd;

	// Pick at the center
	screen.x = 0.5f * getWidth() + m_originX;
	screen.y = 0.5f * getHeight() + m_originY;
	getPickRay(&screen, &rayStart, &rayEnd);

	// Looking at the horizon would yield infinite numbers.
	if (fabs(rayStart.Z - rayEnd.Z) < 1.0f)
		return 1e+6f;

	center.x = Vector3::Find_X_At_Z(maxEdgeZ, rayStart, rayEnd);
	center.y = Vector3::Find_Y_At_Z(maxEdgeZ, rayStart, rayEnd);

	const Bool isLookingDown = rayStart.Z >= rayEnd.Z;
	const Real height = isLookingDown ? getHeight() : 0.0f;
	screen.y = height + m_originY;
	getPickRay(&screen, &rayStart, &rayEnd);

	Real bottomX = Vector3::Find_X_At_Z(maxEdgeZ, rayStart, rayEnd);
	Real bottomY = Vector3::Find_Y_At_Z(maxEdgeZ, rayStart, rayEnd);

	center.x -= bottomX;
	center.y -= bottomY;

	Real offset = center.length();

	// TheSuperHackers @tweak Reduces the offset to allow scrolling closer to the edges.
	offset *= GX_CAMERA_AREA_OFFSET_SCALE;

	if (TheGlobalData->m_debugAI) {
		offset -= 1000; // push out the constraints so we can look at staging areas.
	}

	return offset;
}

//-------------------------------------------------------------------------------------------------
void W3DView::clipCameraIntoAreaConstraints()
{
	constexpr const Real eps = 0.1f;
	Coord2D pos = getPosition2D();
	pos.x = clamp(m_cameraAreaConstraints.lo.x + eps, pos.x, m_cameraAreaConstraints.hi.x - eps);
	pos.y = clamp(m_cameraAreaConstraints.lo.y + eps, pos.y, m_cameraAreaConstraints.hi.y - eps);
	setPosition2D(pos);
}

//-------------------------------------------------------------------------------------------------
Bool W3DView::isWithinCameraAreaConstraints() const
{
	const Coord3D &pos = getPosition();
	return m_cameraAreaConstraints.isInRegion(pos.x, pos.y);
}

//-------------------------------------------------------------------------------------------------
Bool W3DView::isWithinCameraHeightConstraints() const
{
	const Bool isAboveMinHeight = m_currentHeightAboveGround >= m_minHeightAboveGround;
	const Bool isBelowMaxHeight = m_currentHeightAboveGround <= m_maxHeightAboveGround;
	return isAboveMinHeight && (isBelowMaxHeight || !TheGlobalData->m_enforceMaxCameraHeight);
}

//-------------------------------------------------------------------------------------------------
/** Returns a world-space ray originating at a given screen pixel position
	and ending at the far clip plane for current camera.  Screen coordinates
	assumed in absolute values relative to full display resolution.*/
//-------------------------------------------------------------------------------------------------
void W3DView::getPickRay(const ICoord2D *screen, Vector3 *rayStart, Vector3 *rayEnd)
{
	Real logX;
	Real logY;
	Real screenX = screen->x - m_originX;
	Real screenY = screen->y - m_originY;

	//W3D Screen coordinates are -1 to 1, so we need to do some conversion:
	PixelScreenToW3DLogicalScreen(screenX, screenY, &logX, &logY, getWidth(), getHeight());

	*rayStart = m_3DCamera->Get_Position();	//get camera location
	m_3DCamera->Un_Project(*rayEnd,Vector2(logX,logY));	//get world space point
	*rayEnd -= *rayStart;	//vector camera to world space point
	rayEnd->Normalize();	//make unit vector
	*rayEnd *= m_3DCamera->Get_Depth() * 2;	//adjust length to reach far clip plane and beyond
	*rayEnd += *rayStart;	//get point on far clip plane along ray from camera.
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
/** GeneralsX: on the shell map return the height limits parsed from the stock INI, so a zoom preference
  * does not reframe the menu backdrop (MaxCameraHeight scales getCameraOffsetZ). Min and max are set together:
  * setCameraHeightAboveGroundLimitsToDefault() clamps max up to min. effectiveLimits() (GXHostCamera) runs first,
  * even on the shell arm, so its match-state update still runs. Client-only; no effect on determinism. */
//-------------------------------------------------------------------------------------------------
void W3DView::getEffectiveCameraHeightLimits(Real *minHeight, Real *maxHeight) const
{
	// One call, not two - preserves the invariant the host-camera feature put on this pair.
	const GXHostCamera::Limits hostCamLimits = GXHostCamera::effectiveLimits();

	if (TheGameLogic != nullptr && TheGameLogic->isInShellGame()
		&& TheGlobalData->m_defaultCameraHeightsCaptured)
	{
		*minHeight = TheGlobalData->m_defaultMinCameraHeight;
		*maxHeight = TheGlobalData->m_defaultMaxCameraHeight;
		return;
	}

	*minHeight = hostCamLimits.minHeight;
	*maxHeight = hostCamLimits.maxHeight;
}

//-------------------------------------------------------------------------------------------------
Real W3DView::getEffectiveMaxCameraHeight() const
{
	Real minHeight, maxHeight;
	getEffectiveCameraHeightLimits(&minHeight, &maxHeight);
	return maxHeight;
}

//-------------------------------------------------------------------------------------------------
Real W3DView::getCameraOffsetZ() const
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	// TheSuperHackers @info xezon 04/12/2025 It is necessary to use the initial ground level for the
	// scripted camera height to preserve the original look of it. Otherwise the forward distance
	// of the camera will slightly change the view pitch.
	if (!m_isUserControlled)
	{
		return m_initialGroundLevel + TheGlobalData->m_cameraHeight;
	}
#endif

	// GeneralsX: Host-authoritative camera limits: see GXHostCamera.h.
	//
	// This read was NOT on the design pass's list of five and has to be here anyway. getCameraOffsetZ
	// is the denominator that turns a height into a zoom (getZoom/getMaxZoom below, and the inverse
	// at m_currentHeightAboveGround), so it is the reference height that zoom==1.0 means. Leaving it
	// on TheGlobalData while m_maxHeightAboveGround follows the host would make the two disagree and
	// the zoom scale would no longer land on the limit it is supposed to.
	return m_pos.z + getEffectiveMaxCameraHeight();
}

// GeneralsX: the zoom retail would report. getCameraOffsetZ() follows the effective max height, not retail's 310,
// so a raised max gives a smaller zoom and overlays drawn at size / zoom (health bars, pips) would grow.
// Up to the stock ceiling this is plain zoom (iconScale 1.0); above it overlays follow the height above the PIVOT.
// A scripted camera keeps plain zoom, as in retail. m_defaultMaxCameraHeight is the stock GameData value.
Real W3DView::getRetailReferenceZoom( Real *iconScale )
{
	if (iconScale)
		*iconScale = 1.0f;
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	if (!m_isUserControlled)
		return m_zoom;
#endif
	const Real retailMax = TheGlobalData->m_defaultMaxCameraHeight;
	const Real effectiveMax = getEffectiveMaxCameraHeight();
	const Real retailOffset = m_pos.z + retailMax;
	if (retailMax <= 0.0f || effectiveMax <= 0.0f || retailOffset <= 0.0f)
		return m_zoom;
	const Real offset = getCameraOffsetZ();
	// GeneralsX @feature against the max before the whole-map cap: the cap stops the camera, it is not a new setting
	const Real stockCeiling = (isWholeMapCapInForce() ? m_gxUncappedMax : m_maxHeightAboveGround) * retailMax / effectiveMax;
	const Real heightAbovePivot = offset * m_zoom - m_pos.z;
	// One world unit of slack: at the stock max zoom offset * m_zoom - m_pos.z lands on the ceiling only to
	// float precision, and "a hair above" would shave a pixel off every fixed-size icon in ordinary play.
	if (stockCeiling <= 0.0f || heightAbovePivot <= stockCeiling + 1.0f)
		return (effectiveMax == retailMax) ? m_zoom : m_zoom * offset / retailOffset;
	// Above the stock ceiling - a raised limit, or the stock one with the zoom lock off.
	if (iconScale)
		*iconScale = stockCeiling / heightAbovePivot;
	return (m_pos.z + stockCeiling) / retailOffset * (heightAbovePivot / stockCeiling);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Real W3DView::getDesiredHeight(Real x, Real y) const
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	// TheSuperHackers @info xezon 06/12/2025 The height above ground must be relative to the current
	// terrain height because the ground level is not updated for it.
	if (!m_isUserControlled)
	{
		return getHeightAroundPos(x, y) + m_heightAboveGround;
	}
#endif

	return m_pos.z + m_heightAboveGround;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Real W3DView::getMaxHeight(Real x, Real y) const
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	if (!m_isUserControlled)
	{
		return getHeightAroundPos(x, y) + m_maxHeightAboveGround;
	}
#endif

	return m_pos.z + m_maxHeightAboveGround;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Real W3DView::getDesiredZoom(Real x, Real y) const
{
	return getDesiredHeight(x, y) / getCameraOffsetZ();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
Real W3DView::getMaxZoom(Real x, Real y) const
{
	return getMaxHeight(x, y) / getCameraOffsetZ();
}

//-------------------------------------------------------------------------------------------------
/** set the transform matrix of m_3DCamera, based on m_pos & m_angle */
//-------------------------------------------------------------------------------------------------
void W3DView::updateCameraTransform()
{
	if (TheGlobalData->m_headless)
		return;

	Vector3 sourcePos;
	Vector3 targetPos;
	buildCameraPosition(sourcePos, targetPos);

#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	const Bool clipCameraAboveTerrain = m_isUserControlled;
#else
	const Bool clipCameraAboveTerrain = true;
#endif
	if (clipCameraAboveTerrain)
	{
		// TheSuperHackers @fix Moves the camera above the terrain.
		// Uses averaged terrain height sampling to reduce bumpy movements.
		const Real minAcceptableCameraHeight = getHeightAroundPos(sourcePos.X, sourcePos.Y, MAP_XY_FACTOR) + NearZ;
		if (sourcePos.Z < minAcceptableCameraHeight)
		{
			const Real repositionZ = minAcceptableCameraHeight - sourcePos.Z;
			sourcePos.Z += repositionZ;
			targetPos.Z += repositionZ;
		}
	}

	Matrix3D cameraTransform;
	buildCameraTransform(&cameraTransform, sourcePos, targetPos);

	setCameraTransform(cameraTransform);
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @tweak The far clip plane is now generally aligned with the actual terrain
// draw size. This is most useful for low camera angles.
//-------------------------------------------------------------------------------------------------
void W3DView::updateCameraClipPlanes(const Matrix3D &transform)
{
	Real farZ;

	if (TheGlobalData->m_drawEntireTerrain)
	{
		farZ = 100000.0f;
	}
	else if (TheTerrainRenderObject && TheTerrainRenderObject->getMap())
	{
		WorldHeightMap *heightMap = TheTerrainRenderObject->getMap();

		const Vector3 camPos = transform.Get_Translation();
		const Vector3 camDir = -transform.Get_Z_Vector();

		const Region2D region = heightMap->getDrawRegion2D();
		const Real minZ = TheTerrainRenderObject->getMinHeight();

		// Bounding sphere
		Vector3 center;
		center.X = (region.lo.x + region.hi.x) * 0.5f;
		center.Y = (region.lo.y + region.hi.y) * 0.5f;
		center.Z = minZ - 1.0f; // -1 to avoid Z clipping when looking straight down

		// Half extents
		const Real dx = (region.hi.x - region.lo.x) * 0.5f;
		const Real dy = (region.hi.y - region.lo.y) * 0.5f;

		// Project center
		const Vector3 v = center - camPos;
		const Real projectedDistanceToCenter = fabs(Vector3::Dot_Product(v, camDir));

		// Project radius
		const Real projectedRadiusToEdge = fabs(dx * camDir.X) + fabs(dy * camDir.Y);

		// Final far plane
		farZ = std::max(projectedDistanceToCenter + projectedRadiusToEdge, 0.0f);
	}
	else
	{
		farZ = WorldHeightMap::NORMAL_DRAW_WIDTH * MAP_XY_FACTOR;
	}

	if (m_useRealZoomCam)	//WST 10.19.2002
	{
		if (m_FXPitch < 0.95f)
		{
			// Extend far Z when we pitch up for RealZoomCam
			farZ /= m_FXPitch;
		}
	}
	else
	{
		if (m_FXPitch < 0.95f)
		{
			// Extend far clip plane so entire terrain can be visible
			farZ *= 10.0f;
		}
	}

	m_3DCamera->Set_Clip_Planes(NearZ, farZ);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setCameraTransform(const Matrix3D &transform)
{
	m_lastScreenToTerrainValid = false;

#if defined(RTS_DEBUG)
	m_3DCamera->Set_View_Plane( m_FOV, -1 );
#endif

	m_3DCamera->Set_Transform(transform);

	if (TheTerrainRenderObject)
	{
		updateTerrain();
	}

	// TheSuperHackers @info Camera clip planes must be updated after the terrain,
	// because the new terrain draw size is needed to calculate the far clip plane.
	updateCameraClipPlanes(transform);

	// TheSuperHackers @fix Notify the Radar about the changed view always.
	// This way the radar view box should always be in sync with the camera view.
	if (TheRadar)
	{
		TheRadar->notifyViewChanged();
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::init()
{
	// extend View functionality
	View::init();
	setName("W3DView");
	// set default camera "look at" point
	Coord3D pos;
	pos.x = 87.0f * MAP_XY_FACTOR;
	pos.y = 77.0f * MAP_XY_FACTOR;
	pos.z = 10.0f;

	setPosition(pos);

	// create our 3D camera
	m_3DCamera = NEW_REF( CameraClass, () );

	// create our 2D camera for the GUI overlay
	m_2DCamera = NEW_REF( CameraClass, () );
	m_2DCamera->Set_Position( Vector3( 0, 0, 1 ) );
	Vector2 min = Vector2( -1, -0.75f );
	Vector2 max = Vector2( +1, +0.75f );
	m_2DCamera->Set_View_Plane( min, max );
	m_2DCamera->Set_Clip_Planes( 0.995f, 2.0f );

	m_scrollAmountCutoffSqr = sqr(TheGlobalData->m_scrollAmountCutoff);

	m_cameraAreaConstraintsValid = false;
	m_recalcCameraConstraintsAfterScrolling = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
Coord3D W3DView::get3DCameraPosition() const
{
	Vector3 camera = m_3DCamera->Get_Position();
	Coord3D pos = { camera.X, camera.Y, camera.Z };
	return pos;
}

//-------------------------------------------------------------------------------------------------
Coord3D W3DView::get3DCameraDirection() const
{
	Vector3 forward = m_3DCamera->Get_Forward_Dir();
	Coord3D dir = { forward.X, forward.Y, forward.Z };
	return dir;
}

//-------------------------------------------------------------------------------------------------
void W3DView::set3DCameraLookAt(const Coord3D &pos, const Coord3D &dir, Real roll)
{
	Vector3 camPos(pos.x, pos.y, pos.z);
	Vector3 camDir(dir.x, dir.y, dir.z);
	Matrix3D transform;
	transform.Look_At_Dir(camPos, camDir, roll);

	setCameraTransform(transform);

	m_recalcCamera = false;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::reset()
{
	View::reset();

	// Just in case...
	// GeneralsX: The scripted camera's timing starts over: no move, time multiplier 1 and its divider,
	// no freeze, no blur (the freeze used to outlive a game).
	m_camTimeline.reset(MSEC_PER_LOGICFRAME_REAL);

	stopDoingScriptedCamera();
	setUserControlled(true);
	// GeneralsX: The scripted camera clock starts over with the next game, so a replay steps the same
	// whatever played before it in this process (the divider included: W3DDisplay::draw's own counter never reset).
	m_gxCamStepFrame = m_gxCamPoseFrame = m_gxCamDrawnFrame = 0xffffffff;
	m_gxCamContinues = FALSE;

	// Just move the camera to zero. It'll get repositioned at the beginning of the next game anyways.
	Coord3D arbitraryPos = { 0, 0, 0 };
	setPosition(arbitraryPos);
	setAngleToDefault();
	setPitchToDefault();
	setZoomToDefault();

	setViewFilter(FT_VIEW_DEFAULT);

	Coord2D gb = { 0,0 };
	setGuardBandBias( &gb );

	m_recalcCameraConstraintsAfterScrolling = false;

	// GeneralsX: The terrain window only grows within one game (updateTerrain);
	// the next one starts from its own default view, not from the menu backdrop's or the last match's size.
	m_terrainDrawSize.x = 0;
	m_terrainDrawSize.y = 0;
}

//-------------------------------------------------------------------------------------------------
/** draw worker for drawables in the view region */
//-------------------------------------------------------------------------------------------------
static void drawDrawable( Drawable *draw, void *userData )
{
	// GeneralsX: Also record the visit, for W3DView::draw's post-draw walk.
	static_cast<std::vector<Drawable*>*>(userData)->push_back( draw );

	draw->draw();

}

// ------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
static void drawTerrainNormal( Drawable *draw, void *userData )
{
	UnsignedInt color = GameMakeColor( 255, 255, 0, 255 );
  if (TheTerrainLogic)
  {
    Coord3D pos = *draw->getPosition();
    Coord3D normal;
    pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y, &normal);
    const Real NORMLEN = 20;
    normal.x = pos.x + normal.x * NORMLEN;
    normal.y = pos.y + normal.y * NORMLEN;
    normal.z = pos.z + normal.z * NORMLEN;
    ICoord2D start, end;
		TheTacticalView->worldToScreen(&pos, &start);
		TheTacticalView->worldToScreen(&normal, &end);
		TheDisplay->drawLine(start.x, start.y, end.x, end.y, 1.0f, color);
  }
}

#if defined(RTS_DEBUG)
// ------------------------------------------------------------------------------------------------
// Draw a crude circle. Appears on top of any world geometry
// ------------------------------------------------------------------------------------------------
void drawDebugCircle( const Coord3D & center, Real radius, Real width, Color color )
{
  const Real inc = PI/4.0f;
  Real angle = 0.0f;
  Coord3D pnt, lastPnt;
  ICoord2D start, end;
  Bool endValid, startValid;

  lastPnt.x = center.x + radius * (Real)cos(angle);
  lastPnt.y = center.y + radius * (Real)sin(angle);
  lastPnt.z = center.z;
  endValid = ( TheTacticalView->worldToScreenTriReturn( &lastPnt, &end ) != View::WTS_INVALID );

  for( angle = inc; angle <= 2.0f * PI; angle += inc )
  {
    pnt.x = center.x + radius * (Real)cos(angle);
    pnt.y = center.y + radius * (Real)sin(angle);
    pnt.z = center.z;
    startValid = ( TheTacticalView->worldToScreenTriReturn( &pnt, &start ) != View::WTS_INVALID );

    if ( startValid && endValid )
      TheDisplay->drawLine( start.x, start.y, end.x, end.y, width, color );

    lastPnt = pnt;
    end = start;
    endValid = startValid;
  }
}

static void drawDrawableExtents( Drawable *draw, void *userData );  // FORWARD DECLARATION
// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void drawContainedDrawable( Object *obj, void *userData )
{
	Drawable *draw = obj->getDrawable();

	if( draw )
		drawDrawableExtents( draw, userData );

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
static void drawDrawableExtents( Drawable *draw, void *userData )
{
	UnsignedInt color = GameMakeColor( 0, 255, 0, 255 );

	switch( draw->getDrawableGeometryInfo().getGeomType() )
	{

		//---------------------------------------------------------------------------------------------
		case GEOMETRY_BOX:
		{
			Real angle = draw->getOrientation();
			Real c = (Real)cos(angle);
			Real s = (Real)sin(angle);
			Real exc = draw->getDrawableGeometryInfo().getMajorRadius()*c;
			Real eyc = draw->getDrawableGeometryInfo().getMinorRadius()*c;
			Real exs = draw->getDrawableGeometryInfo().getMajorRadius()*s;
			Real eys = draw->getDrawableGeometryInfo().getMinorRadius()*s;
			Coord3D pts[4];
			pts[0].x = draw->getPosition()->x - exc - eys;
			pts[0].y = draw->getPosition()->y + eyc - exs;
			pts[0].z = 0;
			pts[1].x = draw->getPosition()->x + exc - eys;
			pts[1].y = draw->getPosition()->y + eyc + exs;
			pts[1].z = 0;
			pts[2].x = draw->getPosition()->x + exc + eys;
			pts[2].y = draw->getPosition()->y - eyc + exs;
			pts[2].z = 0;
			pts[3].x = draw->getPosition()->x - exc + eys;
			pts[3].y = draw->getPosition()->y - eyc - exs;
			pts[3].z = 0;
			Real z = draw->getPosition()->z;
			for( int i = 0; i < 2; i++ )
			{

				for (int corner = 0; corner < 4; corner++)
				{
					ICoord2D start, end;
					pts[corner].z = z;
					pts[(corner+1)&3].z = z;
					TheTacticalView->worldToScreen(&pts[corner], &start);
					TheTacticalView->worldToScreen(&pts[(corner+1)&3], &end);
					TheDisplay->drawLine(start.x, start.y, end.x, end.y, 1.0f, color);
				}

				z += draw->getDrawableGeometryInfo().getMaxHeightAbovePosition();

			}

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GEOMETRY_SPHERE:	// not quite right, but close enough
		case GEOMETRY_CYLINDER:
		{
      Coord3D center = *draw->getPosition();
      const Real radius = draw->getDrawableGeometryInfo().getMajorRadius();

			// draw cylinder
			for( int i=0; i<2; i++ )
			{
        drawDebugCircle( center, radius, 1.0f, color );

        // next time 'round, draw the top of the cylinder
        center.z += draw->getDrawableGeometryInfo().getMaxHeightAbovePosition();
			}

			// draw centerline
      ICoord2D start, end;
      center = *draw->getPosition();
      TheTacticalView->worldToScreen( &center, &start );
      center.z += draw->getDrawableGeometryInfo().getMaxHeightAbovePosition();
      TheTacticalView->worldToScreen( &center, &end );
			TheDisplay->drawLine( start.x, start.y, end.x, end.y, 1.0f, color );

			break;

		}

	}

	// draw any extents for things that are contained by this
	Object *obj = draw->getObject();
	if( obj )
	{
		ContainModuleInterface *contain = obj->getContain();

		if( contain )
			contain->iterateContained( drawContainedDrawable, userData, FALSE );

	}

}


static void drawAudioLocations( Drawable *draw, void *userData );
// ------------------------------------------------------------------------------------------------
// Helper for drawAudioLocations
// ------------------------------------------------------------------------------------------------
static void drawContainedAudioLocations( Object *obj, void *userData )
{
  Drawable *draw = obj->getDrawable();

  if( draw )
    drawAudioLocations( draw, userData );

}


//-------------------------------------------------------------------------------------------------
// Draw the location of audio objects in the world
//-------------------------------------------------------------------------------------------------
static void drawAudioLocations( Drawable *draw, void *userData )
{
  // draw audio for things that are contained by this
  Object *obj = draw->getObject();
  if( obj )
  {
    ContainModuleInterface *contain = obj->getContain();

    if( contain )
      contain->iterateContained( drawContainedAudioLocations, userData, FALSE );

  }

  const ThingTemplate * thingTemplate = draw->getTemplate();

  if ( thingTemplate == nullptr || thingTemplate->getEditorSorting() != ES_AUDIO )
  {
    return; // All done
  }

  // Copied in hideously inappropriate code copying ways from DrawObject.cpp
  // Should definitely be a global, probably read in from an INI file <gasp>
  static const Int poleHeight = 20;
  static const Int flagHeight = 10;
  static const Int flagWidth = 10;
  const Color color = GameMakeColor(0x25, 0x25, 0xEF, 0xFF);

  // Draw flag for audio-only objects:
  //  *
  //  * *
  //  *   *
  //  *     *
  //  *   *
  //  * *
  //  *
  //  *
  //  *
  //  *
  //  *

  Coord3D worldPoint;
  ICoord2D start, end;

  worldPoint = *draw->getPosition();
  TheTacticalView->worldToScreen( &worldPoint, &start );
  worldPoint.z += poleHeight;
  TheTacticalView->worldToScreen( &worldPoint, &end );
  TheDisplay->drawLine( start.x, start.y, end.x, end.y, 1.0f, color );

  worldPoint.z -= flagHeight / 2;
  worldPoint.x += flagWidth;
  TheTacticalView->worldToScreen( &worldPoint, &start );
  TheDisplay->drawLine( start.x, start.y, end.x, end.y, 1.0f, color );

  worldPoint.z -= flagHeight / 2;
  worldPoint.x -= flagWidth;
  TheTacticalView->worldToScreen( &worldPoint, &end );
  TheDisplay->drawLine( start.x, start.y, end.x, end.y, 1.0f, color );
}

//-------------------------------------------------------------------------------------------------
// Draw the radii of sounds attached to any type of object.
//-------------------------------------------------------------------------------------------------
static void drawAudioRadii( const Drawable * drawable )
{

  // Draw radii, if sound is playing
  const AudioEventRTS * ambientSound = drawable->getAmbientSound();

  if ( ambientSound && ambientSound->isCurrentlyPlaying() )
  {
    const AudioEventInfo * ambientInfo = ambientSound->getAudioEventInfo();

    if ( ambientInfo == nullptr )
    {
      // I don't think that's right...
      OutputDebugString( ("Playing sound has null AudioEventInfo?\n" ) );

      if ( TheAudio != nullptr )
      {
        ambientInfo = TheAudio->findAudioEventInfo( ambientSound->getEventName() );
      }
    }

    if ( ambientInfo != nullptr )
    {
      // Colors match those used in WorldBuilder
      drawDebugCircle( *drawable->getPosition(), ambientInfo->m_minDistance, 1.0f, GameMakeColor(0x00, 0x00, 0xFF, 0xFF) );
      drawDebugCircle( *drawable->getPosition(), ambientInfo->m_maxDistance, 1.0f, GameMakeColor(0xFF, 0x00, 0xFF, 0xFF) );
    }
  }
}

#endif

//-------------------------------------------------------------------------------------------------
/** An opportunity to draw something after all drawables have been drawn once */
//-------------------------------------------------------------------------------------------------
static void drawablePostDraw( Drawable *draw, void *userData )
{
	Real FXPitch = TheTacticalView->getFXPitch();
	if (draw->isDrawableEffectivelyHidden() || FXPitch < 0.0f)
		return;

	Object* obj = draw->getObject();
	const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();
	// GeneralsX: query first, then drop it: getShroudedStatus recomputes a lazy sim cache
	// (m_everSeenByPlayer) logic reads, so -noshroud / GX_NOSHROUD must not change which queries run.
	ObjectShroudStatus ss = (!obj) ? OBJECTSHROUD_CLEAR : obj->getShroudedStatus(localPlayerIndex);
#if ENABLE_CONFIGURABLE_SHROUD
	if (!gxShroudDrawn())
		ss = OBJECTSHROUD_CLEAR;
#endif
	if (ss > OBJECTSHROUD_PARTIAL_CLEAR)
		return;

	// draw the any "icon" UI for a drawable (health bars, veterency, etc);

	//*****
	//@TODO: Create a way to reject this call easily -- like objects that have no compatible modules.
	//*****
	//if( draw->getStatusBits() )
	//{
			draw->drawIconUI();
	//}

#if defined(RTS_DEBUG)
	// debug collision extents
	if( TheGlobalData->m_showCollisionExtents )
	  drawDrawableExtents( draw, userData );

  if ( TheGlobalData->m_showAudioLocations )
    drawAudioLocations( draw, userData );
#endif

	// debug terrain normals at object positions
	if( TheGlobalData->m_showTerrainNormals )
	  drawTerrainNormal( draw, userData );

	TheGameClient->incrementRenderedObjectCount();

}

//-------------------------------------------------------------------------------------------------
// Display AI debug visuals
//-------------------------------------------------------------------------------------------------
static void renderAIDebug()
{
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool W3DView::updateCameraMovements()
{
	// GeneralsX: steps once per new logic frame (stepScriptedCamera), as the end frame of a move is simulation state.
	// A camera-only freeze advances no frame, so the draw steps it; a script freeze steps in stepFrozenLogicUpdate.
	// TheSuperHackers @tweak The scripted camera movement is now decoupled from the render update.
	// The scripted camera will still move when the time is frozen, but not when the game is halted.
	// A SCRIPT freeze is not stepped here: its scripts still run once per gate pass and the camera steps on those updates
	// (stepFrozenLogicUpdate); stepping it per draw would move it fps times faster than the scripts watching it.
	// Only a camera-only freeze steps here.
	const Bool frozen = (TheFramePacer != NULL) && TheFramePacer->isTimeFrozen()
		&& !(TheScriptEngine->isTimeFrozenScript() || TheScriptEngine->isTimeFrozenDebug());
	Bool didUpdate = false;
	if (frozen)
		didUpdate = stepScriptedCameraOnce(TheFramePacer->getLogicTimeStepMilliseconds(FramePacer::IgnoreFrozenTime));
	else
		didUpdate = hasScriptedState(Scripted_Rotate | Scripted_Pitch | Scripted_Zoom | Scripted_MoveOnWaypointPath)
			|| (TheGameLogic != NULL && m_gxCamPoseFrame == TheGameLogic->getFrame());	// the last step, still blending
	if (hasScriptedState(Scripted_CameraLock))
	{
		didUpdate = true;
	}
	// GeneralsX: A move stepped with no draw (fast forward's skipped draws, the script
	// fast-time return - W3DDisplay::draw) still has to reach the camera: updateView sets this after the same call, but
	// a move that finishes on a skipped draw would leave the next drawn frame seeing no active move.
	if (didUpdate)
		m_recalcCamera = true;
	return didUpdate;
}

// GeneralsX: One scripted camera step: zoom, pitch, then the rotation or the waypoint path (a
// rotation ends a path, rotateCamera), in the order and with the bookkeeping updateCameraMovements always had.
Bool W3DView::stepScriptedCameraOnce(Real waypointMs)
{
	return applyScriptedStep(m_camTimeline.stepOnce(waypointMs), waypointMs);
}

// GeneralsX: The pose of one step of m_camTimeline (ScriptedCameraTimeline::stepOnce, which counted the
// frames, ended the moves, set the time multiplier and cleared the freeze): zoom, pitch, then the rotation or the path, in the
// order updateCameraMovements always stepped them.
Bool W3DView::applyScriptedStep(unsigned did, Real waypointMs)
{
	typedef ScriptedCameraTimeline TL;
	if (did & TL::STEPPED_ZOOM)
		zoomCameraOneFrame((did & TL::ENDED_ZOOM) != 0);
	if (did & TL::STEPPED_PITCH)
		pitchCameraOneFrame((did & TL::ENDED_PITCH) != 0);
	if (did & TL::STEPPED_ROTATE)
	{
		rotateCameraOneFrame((did & TL::ENDED_ROTATE) != 0);
	}
	else if (did & TL::STEPPED_PATH)
	{
		const ScriptedCameraTimeline::Path &path = m_camTimeline.waypointPath();
		moveAlongWaypointPath(path.elapsed, path.total, waypointMs, (did & TL::ENDED_PATH) != 0);
	}
	return (did & (TL::STEPPED_ZOOM | TL::STEPPED_PITCH | TL::STEPPED_ROTATE | TL::STEPPED_PATH)) != 0;
}

// GeneralsX: GX_CAMTEST, the scripted camera's in-game check, lives in GameLogic/ScriptedCamera.cpp: it
// starts its moves through the simulation, so a -headless peer runs it too. The view adds "[GXCAMDRAW]" on every draw while
// a move runs (the drawn camera, the blend's phase).

// GeneralsX: GX_CAMTEST: the first time the VIEW drops a scripted move's state, say from where (a
// backtrace where the platform has one). The test moves live in TheScriptedCamera (simulation); this finds
// what cuts the view's own move (fullscreen edge scroll). Once per run, off unless GX_CAMTEST is set.
static void gxCamTestWhoEnds(const char *what)
{
	static Bool said = FALSE;
	if (said || !TheScriptedCamera.isCamTestOn())
		return;
	said = TRUE;
	fprintf(stderr, "[GXCAMTEST] %s at f=%u\n", what, TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u);
#if !defined(_WIN32)
	void *frames[14];
	backtrace_symbols_fd(frames, backtrace(frames, 14), 2);
#endif
}

// GeneralsX: The scripted camera's logic step (see updateCameraMovements): once per new logic frame,
// from stepView. GameClient::step runs after every logic update that did not leave time frozen; one that did not advance
// the frame either (a paused game's, the first of a new game) steps nothing. The camera time multiplier's divider counts
// every logic frame, as W3DDisplay::draw's counter counted every draw, moving or not. A motion blur's logic lifetime counts
// here too (isCameraMovementFinished). The poses before and after the step are what a draw blends between.
void W3DView::stepScriptedCamera()
{
	// never in frozen time: base Generals also steps its views once per iteration (GameEngine::execute), so after a load into
	// a script freeze the first one would step a frame no logic update advanced; a freeze's
	// moves are stepped by stepFrozenLogicUpdate or the draw's freeze loop
	if (TheGameLogic == NULL || (TheFramePacer != NULL && TheFramePacer->isTimeFrozen()))
		return;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame == m_gxCamStepFrame)
		return;	// base Generals calls stepView twice per iteration; one step per logic frame all the same
	m_gxCamStepFrame = frame;
	endWaypointPathForCameraLock();
	// the divider and the blur's count (ScriptedCameraTimeline::beginLogicFrame); an IN_AND_OUT with a target jumps there at the
	// top of its count - on this step, not on the draw that reached it: lookAt ends a scripted rotation or path,
	// and the timeline has ended them already. The simulation's blur end re-reads the camera
	// freeze (ScriptedCamera::logicFrame).
	const unsigned begun = m_camTimeline.beginLogicFrame();
	if (!(begun & ScriptedCameraTimeline::DUE))
		return;
	if (begun & ScriptedCameraTimeline::BLUR_TURN)
		lookAt(&m_gxBlurJumpPos);
	if (m_camTimeline.moves() != 0)
	{
		captureScriptedPose(m_gxCamPrev);
		applyScriptedStep(m_camTimeline.stepOnce(MSEC_PER_LOGICFRAME_REAL), MSEC_PER_LOGICFRAME_REAL);
		captureScriptedPose(m_gxCamCur);
		m_gxCamPoseFrame = frame;
		m_recalcCamera = true;
	}
}

// GeneralsX: A SCRIPT freeze (FREEZE_TIME) stops the frame, but GameLogic::update still runs the
// scripts on every gate pass, and they read CAMERA_MOVEMENT_FINISHED and count their timers on that clock: a move in flight
// steps once per such frozen update (GameEngine::updateLogic), one logic frame long, through the divider - as 2003's coupled
// loop stepped it once per draw, which was once per update. A camera-only freeze is stepped by the draw's freeze loop
// (updateCameraMovements), a debug freeze moves nothing, and a motion blur does not count in frozen time, all as before.
void W3DView::stepFrozenLogicUpdate()
{
	if (TheScriptEngine == NULL || !TheScriptEngine->isTimeFrozenScript() || TheScriptEngine->isTimeFrozenDebug())
		return;
	endWaypointPathForCameraLock();	// a lock set by a frozen update's script ends the path on that update
	if (applyScriptedStep(m_camTimeline.stepScriptFrozen(), MSEC_PER_LOGICFRAME_REAL))	// through the divider
		m_recalcCamera = true;
}

// GeneralsX: A camera lock ends a waypoint path (update() did it on the next DRAW, so a logic-only
// pass left the path running), and a lock whose object is gone ends, as update() ends it. On
// every logic update that runs scripts - each new frame ahead of the time multiplier's divider and each script-frozen
// update - so a lock a script sets ends the path before the next script reads CAMERA_MOVEMENT_FINISHED.
void W3DView::endWaypointPathForCameraLock()
{
	if (getCameraLock() == INVALID_ID)
		return;
	removeScriptedState(Scripted_MoveOnWaypointPath);
	m_CameraArrivedAtWaypointOnPathFlag = false;
	if (TheGameLogic != NULL && TheGameLogic->findObjectByID(getCameraLock()) == nullptr)
	{
		setCameraLock(INVALID_ID);
		setCameraLockDrawable(nullptr);
	}
}

void W3DView::captureScriptedPose(ScriptedPose &pose) const
{
	pose.pos = m_pos;
	pose.angle = m_angle;
	pose.zoom = m_zoom;
	pose.fxPitch = m_FXPitch;
}

void W3DView::applyScriptedPose(const ScriptedPose &pose)
{
	m_pos = pose.pos;
	m_angle = pose.angle;
	m_zoom = pose.zoom;
	m_FXPitch = pose.fxPitch;
}

// GeneralsX: The drawn scripted camera, between the last two logic frames' poses at the drawables'
// phase (Display::getLogicPhase; Drawable::isInterpolating), so a scripted move glides on a 120-240 Hz screen instead of
// stepping at the logic rate, and stays where the units it frames are drawn. update() swaps the blend in for
// updateCameraTransform() alone and puts the logic pose back, so nothing a step or a script reads ever sees it. Only for the
// frame this logic step moved the camera, when that frame's first draw followed a draw of the frame before (else it draws
// the logic pose, as a drawable does after a skipped draw), when nothing has moved the camera since the step (a camera
// lock, a scroll, a draw's frozen-time step) and never in frozen time. TRUE: the blend is in, logic holds the pose to restore.
Bool W3DView::blendScriptedCamera(ScriptedPose &logic)
{
	// a slaved camera (cameraEnableSlaveMode) takes its transform and its position from a bone in updateCameraTransform:
	// nothing to blend, and the restore would undo that position
	if (TheGameLogic == NULL || TheDisplay == NULL || m_isCameraSlaved || (TheFramePacer != NULL && TheFramePacer->isTimeFrozen()))
		return FALSE;
	if (m_gxCamPoseFrame != TheGameLogic->getFrame() || !m_gxCamContinues)
		return FALSE;
	const Real alpha = TheDisplay->getLogicPhase();
	if (!(alpha < 1.0f))
		return FALSE;
	captureScriptedPose(logic);
	if (logic.pos.x != m_gxCamCur.pos.x || logic.pos.y != m_gxCamCur.pos.y || logic.pos.z != m_gxCamCur.pos.z
		|| logic.angle != m_gxCamCur.angle || logic.zoom != m_gxCamCur.zoom || logic.fxPitch != m_gxCamCur.fxPitch)
		return FALSE;
	ScriptedPose drawn;
	drawn.pos.x = m_gxCamPrev.pos.x + (m_gxCamCur.pos.x - m_gxCamPrev.pos.x) * alpha;
	drawn.pos.y = m_gxCamPrev.pos.y + (m_gxCamCur.pos.y - m_gxCamPrev.pos.y) * alpha;
	drawn.pos.z = m_gxCamPrev.pos.z + (m_gxCamCur.pos.z - m_gxCamPrev.pos.z) * alpha;
	drawn.angle = ScriptedCameraClock::lerpAngle(m_gxCamPrev.angle, m_gxCamCur.angle, alpha);
	drawn.zoom = m_gxCamPrev.zoom + (m_gxCamCur.zoom - m_gxCamPrev.zoom) * alpha;
	drawn.fxPitch = m_gxCamPrev.fxPitch + (m_gxCamCur.fxPitch - m_gxCamPrev.fxPitch) * alpha;
	applyScriptedPose(drawn);
	return TRUE;
}


/** This function performs all actions which affect the camera transform or 3D objects
	rendered in this frame.

   MW: I moved this code out out W3DView::draw() so that we can get final camera and object
   positions before any rendering begins.  This was necessary so that reflection textures
   (which update before the main rendering loop) could get a correct version of the scene.
   Without this change, the reflections were always 1 frame behind the non-reflected view.
*/
void W3DView::updateView()
{
	UPDATE();
}

// TheSuperHackers @tweak xezon 12/08/2025 The camera shaker is no longer tied to the render
// update. The shake does sharp shakes on every fixed time step, and is not intended to have
// linear interpolation during the render update.
void W3DView::stepView()
{
	// GeneralsX: The scripted camera's one step per new logic frame (updateCameraMovements says why).
	stepScriptedCamera();

	//
	// Process camera shake
	//
	if (m_shakeIntensity > 0.01f)
	{
		m_shakeOffset.x = m_shakeIntensity * m_shakeAngleCos;
		m_shakeOffset.y = m_shakeIntensity * m_shakeAngleSin;

		// fake a stiff spring/damper
		const Real dampingCoeff = 0.75f;
		m_shakeIntensity *= dampingCoeff;

		// spring is so "stiff", it pulls 180 degrees opposite each frame
		m_shakeAngleCos = -m_shakeAngleCos;
		m_shakeAngleSin = -m_shakeAngleSin;
	}
	else
	{
		m_shakeIntensity = 0.0f;
		m_shakeOffset.x = 0.0f;
		m_shakeOffset.y = 0.0f;
	}
}

//DECLARE_PERF_TIMER(W3DView_updateView)
void W3DView::update()
{
	//USE_PERF_TIMER(W3DView_updateView)
	Bool didScriptedMovement = false;
	m_drawWalkValid = FALSE; // any early return below leaves no draw walk for draw() to reuse

	// GeneralsX: Did this frame's first draw follow a draw of the frame before? A scripted camera
	// blends between two logic frames' poses only then, as a drawable does (blendScriptedCamera).
	const Bool firstDrawOfGame = (m_gxCamDrawnFrame == 0xffffffff);	// reset() clears it
	if (TheGameLogic != NULL && TheGameLogic->getFrame() != m_gxCamDrawnFrame)
	{
		m_gxCamContinues = (TheGameLogic->getFrame() == m_gxCamDrawnFrame + 1);
		m_gxCamDrawnFrame = TheGameLogic->getFrame();
	}

	// GeneralsX: The zoom and ground-follow easing below (zoomCameraToDesiredHeight,
	// movePivotToGround) run once per DRAW, so they step by the time since the last draw. They stepped by the pacer's last
	// ITERATION, the same time at 1x, but fast forward draws 1 iteration in k (FramePacer::isFastForwardDrawDue) and a
	// network catch-up pass skips a draw too: the zoom eased at 1/k speed and near-stopped after a slow draw ("moving/
	// zooming ... buggy and freezing"). GameClient/CameraStepPolicy.h, scripts/test/camera-step-test.sh.
	// Under GX_SHOT capture every iteration draws and the capture clock steps it, as before.
	{
		const double loopNow = gxRenderLoopSeconds();
		m_drawFpsRatio = gxShotCaptureOn() ? gxRenderBaseOverUpdateFpsRatio()
			: CameraStepPolicy::baseOverStepFpsRatio(
				CameraStepPolicy::drawStepSeconds(loopNow, m_lastDrawLoopSeconds, TheFramePacer->getUpdateTime()), (Real)BaseFps);
		m_lastDrawLoopSeconds = loopNow;
	}

	// GeneralsX: GX_ANIMTEST camlock@F (GameClient/GXAnimTrace.h): lock this view on the first enemy command
	// center, as a script's camera follow does, so the follow easing below can be traced at several fps caps.
	if (TheGameLogic != nullptr && ThePlayerList != nullptr && GXAnimTrace::testAt("camlock",
		(TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isLoadingMap()) ? TheGameLogic->getFrame() : 0u))
	{
		const Player *me = ThePlayerList->getLocalPlayer();
		for (Object *o = TheGameLogic->getFirstObject(); o != nullptr && me != nullptr; o = o->getNextObject())
		{
			if (o->isKindOf(KINDOF_COMMANDCENTER) && !o->isEffectivelyDead() && o->getControllingPlayer() != nullptr
				&& me->getRelationship(o->getTeam()) == ENEMIES)
			{
				setCameraLock(o->getID());
				break;
			}
		}
	}

#ifdef LOG_FRAME_TIMES
	__int64 curTime64,freq64;
	static __int64 prevTime64=0;
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&curTime64);
	freq64 /= 1000;

	Int elapsedTimeMs = (curTime64 - prevTime64)/freq64;
	prevTime64 = curTime64;

#endif

//	Int elapsedTimeMs = TheW3DFrameLengthInMsec; // Assume a constant time flow.  It just works out better.  jba.

	// GeneralsX: [GXTERRAIN], with GX_TERRAIN set on a rendered peer: the cost of each
	// terrain rebuild this frame - "partial" (ground a building flattened) or "full" (the whole draw window).
	// GeneralsX: Only an instrumented build: the "partial" rebuilds it logs are an enemy's
	// foundation being flattened under the fog.
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool s_terrainTrace = (getenv("GX_TERRAIN") != nullptr) && !TheGlobalData->m_headless;
#else
	static const Bool s_terrainTrace = FALSE;
#endif

	// GeneralsX: Ground a building flattened since the last frame: rebuilt here, once,
	// instead of through a full terrain update per lowered cell (BaseHeightMapRenderObjClass::terrainHeightChanged).
	// Before the full-update check, so a pending full update makes it skip the partial rebuild.
	if (TheTerrainRenderObject && TheTerrainRenderObject->hasTerrainHeightChanges() && W3DDisplay::m_3DScene)
	{
		GxShadowMapTerrainChanged = true;	// S7 (GXShadowMapHooks.h): the shadow fit re-measures the terrain's height range
		const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		RefRenderObjListIterator it(W3DDisplay::m_3DScene->getLightList());
		const Int cells = TheTerrainRenderObject->flushTerrainHeightChanges(&it);
		if (s_terrainTrace && cells > 0)
		{
			fprintf(stderr, "[GXTERRAIN] f=%u partial cells=%d ms=%.3f\n", TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u,
				cells, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
	}

	if (TheTerrainRenderObject && TheTerrainRenderObject->doesNeedFullUpdate())
	{
		const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		updateTerrain();	// S7: updateTerrain itself flags the shadow fit's terrain re-scan (every caller of a full rebuild)
		if (s_terrainTrace)
		{
			fprintf(stderr, "[GXTERRAIN] f=%u full ms=%.3f\n", TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u,
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
	}

	static Real followFactor = -1;
	static Real s_gxFollowCarry = 0.0f;	// Cb-06a: the follow ramp's part-frame (it steps in whole 60 fps frames)
	static const Real GX_FOLLOW_RAMP_HEAD_START = 0.1f;	// of a 60 fps frame, at each new lock (see below)
	const Real gxRef = GXClientAnimPolicy::refFrames(m_drawFpsRatio, (Real)BaseFps);	// this draw's 60 fps frames (Cb-06a)
	ObjectID cameraLock = getCameraLock();
	if (cameraLock == INVALID_ID)
	{
		followFactor = -1;
	}
	if (cameraLock != INVALID_ID)
	{
		removeScriptedState(Scripted_MoveOnWaypointPath);
		m_CameraArrivedAtWaypointOnPathFlag = false;

		Object* cameraLockObj = TheGameLogic->findObjectByID(cameraLock);
		Bool loseLock = false;

		// check if object has been destroyed or is dead -> lose lock
		if (cameraLockObj == nullptr)
		{
			loseLock = true;
		}
#if 0
		else
		{
			AIUpdateInterface *ai = cameraLockObj->getAIUpdateInterface();
			if (ai && ai->isDead())
				loseLock = true;
		}
#endif


		if (loseLock)
		{
			setCameraLock(INVALID_ID);
			setCameraLockDrawable(nullptr);
			followFactor = -1;
		}
		else
		{
			// GeneralsX: every easing of the lock below was a fixed share per DRAW, so the camera
			// caught its target 4x as fast at 240 fps as at 60. Each now covers the draw's 60 fps reference frames (gxRef, from
			// m_drawFpsRatio; GameClient/GXClientAnimPolicy.h): at any fps the path in time this game drew at 60 fps.
			// The ramp steps in WHOLE reference frames, as it stepped once per 60 fps draw: a ramp growing
			// every draw held more strength on average and arrived 6-15% early at 240 fps.
			// Its carry starts a tenth of a frame in: a 60 fps cap really draws at 59-61 fps, and from 0 the ramp fell a
			// whole step behind there (client-anim-policy-test.sh: 0.12 off the 60 fps path from 0, 0.015 from 0.1).
			if (followFactor<0) {
				followFactor = 0.05f;
				s_gxFollowCarry = GX_FOLLOW_RAMP_HEAD_START;
			} else {
				followFactor += 0.05f * (Real)GXClientAnimPolicy::takeBaseFrames(s_gxFollowCarry, gxRef);
				if (followFactor>1.0f) followFactor = 1.0f;
			}
			if (getCameraLockDrawable() != nullptr)
			{
				Drawable* cameraLockDrawable = (Drawable *)getCameraLockDrawable();

				if (!cameraLockDrawable)
				{
					setCameraLockDrawable(nullptr);
				}
				else
				{
					Coord3D pos;
					Real boundingSphereRadius;
					Matrix3D transform;
					// this method must ONLY be called from the client, NEVER From the logic, not even indirectly.
					if (cameraLockDrawable->clientOnly_getFirstRenderObjInfo(&pos, &boundingSphereRadius, &transform))
					{
						Vector3 zaxis(0,0,1);

						Vector3 objPos;
						objPos.X = pos.x;
						objPos.Y = pos.y;
						objPos.Z = pos.z;

						//get position of top of object, assuming world z roughly along local z.
						objPos += boundingSphereRadius * 1.0f * zaxis;
						Vector3 objview = transform.Get_X_Vector();	//get view vector of object
						//move camera back behind object far enough not to intersect bounding sphere
						Vector3 camtran = objPos - objview * boundingSphereRadius*4.5f;

						Vector3 prevCamTran = m_3DCamera->Get_Position();	//get current camera position.

						Vector3 tranDiff = (camtran - prevCamTran);	//vector old position to new position.

						camtran = prevCamTran + tranDiff * GXClientAnimPolicy::easeStep(0.1f, gxRef);	//slowly move camera to new position.

						Matrix3D camXForm;
						camXForm.Look_At(camtran,objPos,0);
						m_3DCamera->Set_Transform(camXForm);
						m_lastScreenToTerrainValid = false;
					}
				}
			}
			else
			{	Coord3D objpos = *cameraLockObj->getPosition();
				// GeneralsX: A superweapon missile is drawn along its visual flight (MissileVisual.h),
				// away from its logic position: a lock follows what is drawn.
				if (const Drawable *lockDraw = cameraLockObj->getDrawable())
				{
					if (const Matrix3D *visual = lockDraw->getVisualTransform())
					{
						objpos.x = visual->Get_X_Translation();
						objpos.y = visual->Get_Y_Translation();
						objpos.z = visual->Get_Z_Translation();
					}
				}
				Coord3D curpos = getPosition();
				// don't "snap" directly to the pos, but move there smoothly.
				Real snapThreshSqr = sqr(TheGlobalData->m_partitionCellSize);
				Real curDistSqr = sqr(curpos.x - objpos.x) + sqr(curpos.y - objpos.y);
				if ( m_snapImmediate)
				{
					// close enough.
					curpos.x = objpos.x;
					curpos.y = objpos.y;
				}
				else
				{
//					Real ratio = 1.0f - snapThreshSqr/curDistSqr;
					Real dx = objpos.x-curpos.x;
					Real dy = objpos.y-curpos.y;
					if (m_lockType == LOCK_TETHER)
					{
						//snapThreshSqr = sqr( m_lockDist * TheGlobalData->m_partitionCellSize );
						if (curDistSqr >= snapThreshSqr)
						{
							Real ratio = 1.0f - snapThreshSqr/curDistSqr;

							// move halfway there.
							const Real share = GXClientAnimPolicy::easeStep(ratio*0.5f, gxRef);
							curpos.x += dx*share;
							curpos.y += dy*share;
						}
						else
						{
							// we're inside our 'play' tolerance.  Move slowly to the obj
							Real ratio = GXClientAnimPolicy::easeStep(0.01f * m_lockDist, gxRef);
							Real dx = objpos.x-curpos.x;
							Real dy = objpos.y-curpos.y;
							curpos.x += dx*ratio;
							curpos.y += dy*ratio;
						}
					}
					else
					{
						const Real share = GXClientAnimPolicy::easeStep(followFactor, gxRef);
						curpos.x += dx*share;
						curpos.y += dy*share;
					}
				}
				if (!(TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript()) && !TheGameLogic->isGamePaused()) {
					m_previousLookAtPosition = getPosition();
				}
				setPosition(curpos);
				if (GXAnimTrace::on())
					fprintf(stderr, "[GXANIM] cam wall=%.3f f=%u ratio=%.4f follow=%.4f x=%.3f y=%.3f tx=%.3f ty=%.3f\n", GXAnimTrace::wallMs(),
						(unsigned)TheGameLogic->getFrame(), (double)m_drawFpsRatio, (double)followFactor, (double)curpos.x, (double)curpos.y,
						(double)objpos.x, (double)objpos.y);

				// GeneralsX: ...but not while a scripted rotation runs: it stepped after this on every draw
				// and so won; on the logic step now, this pull would fight it between steps.
				if (m_lockType == LOCK_FOLLOW && !hasScriptedState(Scripted_Rotate))
				{
					// camera follow objects if they are flying
					// A drawn superweapon missile (a nuke has no locomotor) is followed too.
					const Drawable *followDraw = cameraLockObj->getDrawable();
					if ((cameraLockObj->isUsingAirborneLocomotor() && cameraLockObj->isAboveTerrainOrWater())
						|| (followDraw != nullptr && followDraw->getVisualTransform() != nullptr))
					{
						Matrix3D camXForm;
						Real heading = cameraLockObj->getOrientation();
						// A drawn superweapon missile faces along its visual flight.
						if (followDraw != nullptr)
						{
							if (const Matrix3D *visual = followDraw->getVisualTransform())
							{
								const Vector3 fwd = visual->Get_X_Vector();
								heading = atan2f(fwd.Y, fwd.X);
							}
						}
						Real idealZRot = heading - M_PI_2;

						if (m_snapImmediate)
						{
							View::setAngle(idealZRot);
						}
						else
						{
							normAngle(idealZRot);
							Real oldZRot = m_angle;
							normAngle(oldZRot);
							Real diffRot = idealZRot - oldZRot;
							normAngle(diffRot);
							View::setAngle(m_angle + diffRot * GXClientAnimPolicy::easeStep(0.1f, gxRef));
						}
					}
				}
				if (m_snapImmediate)
					m_snapImmediate = FALSE;

				m_pos.z = objpos.z;
				didScriptedMovement = true;
				m_recalcCamera = true;
			}
		}
	}

	if (!(TheScriptEngine->isTimeFrozenDebug()/* || TheScriptEngine->isTimeFrozenScript()*/) && !TheGameLogic->isGamePaused()) {
		// If we aren't frozen for debug, allow the camera to follow scripted movements.
		if (updateCameraMovements()) {
			didScriptedMovement = true;
			m_recalcCamera = true;
		}
	} else {
		if (isDoingScriptedCamera()) {
			didScriptedMovement = true; // don't mess up the scripted movement
		}
	}

	if (!m_isUserControlled)
	{
		didScriptedMovement = true;
	}

	//
	// Process camera shake
	//
	if (m_shakeIntensity > 0.01f)
	{
		m_recalcCamera = true;
	}

	//Process New C3 Camera Shaker system
	if (CameraShakerSystem.IsCameraShaking())
	{
		m_recalcCamera = true;
	}

	/*
	 * In order to have the camera follow the terrain in a non-dizzying way, we will have a
	 * "desired height" value that the user sets.  While scrolling, the actual height (set by
	 * zoom) will not get updated unless we are scrolling uphill and our view either goes
	 * underground or higher than the max allowed height.  When the camera is at rest (not
	 * scrolling), the zoom will move toward matching the desired height.
	 */
	// TheSuperHackers @tweak Can now also zoom when the game is paused.
	// TheSuperHackers @tweak The camera zoom speed is now decoupled from the render update.
	// TheSuperHackers @bugfix The camera terrain height adjustment now also works in replay playback.
	// TheSuperHackers @bugfix xezon 26/10/2025 The camera area constraints are now recalculated when
	// the camera zoom changes, for example because of terrain elevation changes in the camera's view.
	// Additionally, the camera can be smoothly pushed away from the constraints, but not while the user
	// is scrolling, to make the scrolling along the map border a pleasant experience. This behavior
	// ensures that the view can reach and see all areas of the map, and especially the bottom map border.

	m_terrainHeightAtPivot = getHeightAroundPos(m_pos.x, m_pos.y);
	m_currentHeightAboveGround = getCameraOffsetZ() * m_zoom - m_terrainHeightAtPivot;

	if (m_okToAdjustHeight)
	{

		if (didScriptedMovement)
		{
			// if we are in a scripted camera movement, take its height above ground as our desired height.
			m_heightAboveGround = m_currentHeightAboveGround;
		}

		const Real scrollLenSqr = m_scrollAmount.lengthSqr();
		const Bool isScrolling = scrollLenSqr > FLT_EPSILON;
		const Bool isScrollingTooFast = scrollLenSqr >= m_scrollAmountCutoffSqr;
		const Bool isWithinHeightConstraints = isWithinCameraHeightConstraints();

		// GeneralsX: a camera body under its clearance floor counts as "too close" (gxBodyClearedZoom)
		// - judged from the WANTED zoom, and only when the clearance rule is why the target is above the camera
		Bool isBodyTooLow = FALSE;
		if (isScrolling && isScrollingTooFast && isWithinHeightConstraints)
		{
			const Real wantedZoom = getDesiredZoom(m_pos.x, m_pos.y);
			const Real clearedZoom = gxBodyClearedZoom(wantedZoom);
			isBodyTooLow = clearedZoom > m_zoom + 0.001f && clearedZoom > wantedZoom + 0.001f;
		}

		// if scrolling, only adjust if we're too close or too far
		const Bool adjustZoomWhenScrolling = isScrolling && (!isScrollingTooFast || !isWithinHeightConstraints || isBodyTooLow);

		// if not scrolling, settle toward desired height above ground
		const Bool adjustZoomWhenNotScrolling = !isScrolling && !didScriptedMovement;

		if (adjustZoomWhenScrolling || adjustZoomWhenNotScrolling)
		{
			// TheSuperHackers @info The camera zoom has two modes:
			// 1. Zoom by scaling the distance of the camera origin to the look-at target.
			//    Used by user zooming and the scripted camera.
			// 2. Zoom by moving the camera pivot to the ground while repositioning the
			//    camera origin towards the look-at target. Visually this looks identical
			//    to (1), but changes the pivot point which is important for the rotation
			//    origin and map border collisions.
			Bool isZoomingOrMovingPivot = false;

			if (zoomCameraToDesiredHeight())
			{
				isZoomingOrMovingPivot = true;
			}

			if (movePivotToGround())
			{
				isZoomingOrMovingPivot = true;
			}

			if (isZoomingOrMovingPivot)
			{
				m_recalcCamera = true;

				if (isScrolling)
				{
					// Does not update the constraints while scrolling to maintain consistent edge collisions.
					m_recalcCameraConstraintsAfterScrolling = true;
				}
				else
				{
					m_cameraAreaConstraintsValid = false;
				}
			}
		}

		if (m_recalcCameraConstraintsAfterScrolling && !isScrolling)
		{
			m_recalcCameraConstraintsAfterScrolling = false;
			m_cameraAreaConstraintsValid = false;
		}
	}

	// GeneralsX: [GXCAMT], with GX_CAMTRACE set on a rendered peer: where the camera is at
	// each drawn frame, against the wall clock - ms since the first line, wall= ms since the last update (the last
	// draw), upd= the pacer's last iteration (FramePacer::getUpdateTime, what the zoom/pivot smoothing above steps by),
	// pos, zoom, the wanted and actual height above ground, the scroll this iteration and the fast-forward speed.
	// Measures camera speed and smoothness per wall-clock second at 1x and under fast forward. Off by default.
	static const Bool s_camTrace = (getenv("GX_CAMTRACE") != nullptr) && !TheGlobalData->m_headless;
	if (s_camTrace)
	{
		static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();
		static std::chrono::steady_clock::time_point s_last = s_t0;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		fprintf(stderr, "[GXCAMT] ms=%.3f f=%u wall=%.3f upd=%.3f x=%.2f y=%.2f zoom=%.5f want=%.2f cur=%.2f scr=%.4f ff=%d\n",
			std::chrono::duration<double, std::milli>(now - s_t0).count(), TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u,
			std::chrono::duration<double, std::milli>(now - s_last).count(), (double)TheFramePacer->getUpdateTime() * 1000.0,
			m_pos.x, m_pos.y, m_zoom, m_heightAboveGround, m_currentHeightAboveGround, sqrtf(m_scrollAmount.lengthSqr()),
			TheFramePacer->isFastForwardActive() ? TheFramePacer->getFastForward() : 1);
		s_last = now;
	}

	if (TheScriptEngine->isTimeFast()) {
		return; // don't draw - makes it faster :) jba.
	}

	if (!didScriptedMovement)
	{
		updateCameraAreaConstraints();
	}

	// (gth) C&C3 if m_isCameraSlaved then force the camera to update each frame
	// GeneralsX: The pan motion blur (calcDeltaScroll) measures how far the DRAWN camera moved since
	// the last draw: the scripted camera steps once per logic frame now, so the step's own start no longer tells it.
	Coord3D drawnLookAt = m_pos;
	if (m_recalcCamera || m_isCameraSlaved)
	{
		// GeneralsX: A scripted move is drawn between logic frames; the logic pose goes back after.
		ScriptedPose logicPose;
		const Bool blended = blendScriptedCamera(logicPose);
		updateCameraTransform();
		drawnLookAt = m_pos;	// the look-at this draw shows, blended or not
		if (blended)
			applyScriptedPose(logicPose);
		// a blended camera is rebuilt on the next draw too, so the last blend of a move is followed by its end pose even if
		// nothing else moves the camera
		m_recalcCamera = blended;
		if (TheScriptedCamera.isCamTestOn() && (blended || isCameraMovementFinished() == FALSE))
		{
			static const std::chrono::steady_clock::time_point s_t0 = std::chrono::steady_clock::now();
			const Vector3 at = m_3DCamera->Get_Position();
			fprintf(stderr, "[GXCAMDRAW] ms=%.3f f=%u blend=%d a=%.4f x=%.3f y=%.3f\n",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s_t0).count(),
				TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, blended ? 1 : 0, (double)TheDisplay->getLogicPhase(), at.X, at.Y);
		}
	}
	// only when the drawn look-at moved: the blur keeps the last movement while the camera stands, as it always did, and an
	// END_PAN normalises it (a zero vector there drew NaNs)
	// the first draw of a game starts the history where the camera is, not at the last game's position, and so does
	// every load-screen draw: the start camera is placed after the first of them
	if (firstDrawOfGame || TheGlobalData->m_loadScreenRender)
	{
		m_previousLookAtPosition = drawnLookAt;
		m_gxCamDrawnLookAt = drawnLookAt;
	}
	else if (drawnLookAt.x != m_gxCamDrawnLookAt.x || drawnLookAt.y != m_gxCamDrawnLookAt.y)
	{
		m_previousLookAtPosition = m_gxCamDrawnLookAt;
		m_gxCamDrawnLookAt = drawnLookAt;
	}

#ifdef DO_SEISMIC_SIMULATIONS
	TheTerrainVisual->updateSeismicSimulations();
#endif

	Region3D axisAlignedRegion;
	getAxisAlignedViewRegion(axisAlignedRegion);

	// render all of the visible Drawables
	/// @todo this needs to use a real region partition or something
	// GeneralsX: Record the visit list for the post-draw walk in draw().
	// The change count is read BEFORE the walk, so a drawable that moves, appears or is destroyed
	// during it (or at any point before draw()) invalidates the list.
	m_drawWalkChangeCount = GameClient::getDrawableChangeCount();
	m_drawWalkList.clear();
	TheGameClient->iterateDrawablesInRegion( &axisAlignedRegion, drawDrawable, &m_drawWalkList );
	m_drawWalkRegion = axisAlignedRegion;
	m_drawWalkValid = TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Find region which contains all drawables in 3D space. */
// TheSuperHackers @fix Now gives back a proper region on low camera pitch by falling back to
// the drawn terrain area or map extents.
//-------------------------------------------------------------------------------------------------
void W3DView::getAxisAlignedViewRegion(Region3D &axisAlignedRegion)
{
	Region3D mapExtent;
	TheTerrainLogic->getExtent( &mapExtent );

	//
	// get the 4 points in 3D space of the 4 corners of the view, we will use a z = 0.0f
	// value so that we can get everything ... even stuff below the terrain
	//
	//  1-------2
	//   \     /
	//    4---3
	Coord3D box[ 4 ];
	if( getScreenCornerWorldPointsAtZ( &box[ 0 ], &box[ 1 ], &box[ 2 ], &box[ 3 ], 0.0f ) == PlaneClass::INSIDE_SEGMENT )
	{
		//
		// take those 4 corners projected into the world and create an axis aligned bounding
		// box, we will use this box to iterate the drawables in 3D space
		//
		axisAlignedRegion.setXYFromPoints(box, ARRAY_SIZE(box));
	}
	else
	{
		if( WorldHeightMap *heightMap = TheTerrainRenderObject->getMap() )
		{
			const Region2D region = heightMap->getDrawRegion2D();
			axisAlignedRegion.setXY(region);
		}
		else
		{
			axisAlignedRegion = mapExtent;
		}
	}

	// low and high regions will be based of the extent of the map
	constexpr const Real safeValue = 999999;
	axisAlignedRegion.lo.z = mapExtent.lo.z - safeValue;
	axisAlignedRegion.hi.z = mapExtent.hi.z + safeValue;

	// we want to overscan a little bit so that we get objects that are partially offscreen
	axisAlignedRegion.lo.x -= (DRAWABLE_OVERSCAN + m_guardBandBias.x);
	axisAlignedRegion.lo.y -= (DRAWABLE_OVERSCAN + m_guardBandBias.y + 60.0f );
	axisAlignedRegion.hi.x += (DRAWABLE_OVERSCAN + m_guardBandBias.x);
	axisAlignedRegion.hi.y += (DRAWABLE_OVERSCAN + m_guardBandBias.y);

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::setFadeParameters(Int fadeFrames, Int direction)
{
	ScreenBWFilter::setFadeParameters(fadeFrames, direction);
	ScreenCrossFadeFilter::setFadeParameters(fadeFrames,direction);
}

void W3DView::set3DWireFrameMode(Bool enable)
{
	m_nextWireFrameEnabled = enable;
}

//-------------------------------------------------------------------------------------------------
/** Sets the view filter mode. */
//-------------------------------------------------------------------------------------------------
void W3DView::setViewFilterPos(const Coord3D *pos)
{
	ScreenMotionBlurFilter::setZoomToPos(pos);
	// GeneralsX: A jump target makes an IN_AND_OUT blur turn instead of ending (its logic lifetime)
	m_gxBlurJumpPos = *pos;
	m_camTimeline.blurTarget();
}

// GeneralsX: After a filter or mode was set (or a failed set put the old one back): a motion blur
// IN/OUT now set starts its logic lifetime over - every set re-runs the filter's setup, which restarts its count - and
// anything else ends it. The draw ending the effect does not end it: isCameraMovementFinished counts logic frames.
void W3DView::noteViewFilterForCameraClock()
{
	const FilterModes mode = m_viewFilterMode;
	const Bool inAndOut = mode == FM_VIEW_MB_IN_AND_OUT_ALPHA || mode == FM_VIEW_MB_IN_AND_OUT_SATURATE;
	const Bool outOnly = mode == FM_VIEW_MB_OUT_ALPHA || mode == FM_VIEW_MB_OUT_SATURATE;
	const Bool live = m_viewFilter == FT_VIEW_MOTION_BLUR_FILTER
		&& (inAndOut || outOnly || mode == FM_VIEW_MB_IN_ALPHA || mode == FM_VIEW_MB_IN_SATURATE);
	m_camTimeline.blur(live, inAndOut, outOnly);
}
//-------------------------------------------------------------------------------------------------
/** Sets the view filter mode. */
//-------------------------------------------------------------------------------------------------
Bool W3DView::setViewFilterMode(FilterModes filterMode)
{
	FilterModes oldMode = m_viewFilterMode;	//save previous mode in case setup fails.

	m_viewFilterMode = filterMode;
	if (m_viewFilterMode != FM_NULL_MODE &&
		m_viewFilter != FT_NULL_FILTER) {
		if (!W3DShaderManager::filterSetup(m_viewFilter, m_viewFilterMode))
		{	//setup failed so restore previous mode.
			m_viewFilterMode = oldMode;
			noteViewFilterForCameraClock();
			return FALSE;
		}
	}
	noteViewFilterForCameraClock();
	return TRUE;
}
//-------------------------------------------------------------------------------------------------
/** Sets the view filter. */
//-------------------------------------------------------------------------------------------------
Bool W3DView::setViewFilter(FilterTypes filter)
{
	FilterTypes oldFilter = m_viewFilter;	//save previous filter in case setup fails.

	m_viewFilter = filter;
	if (m_viewFilterMode != FM_NULL_MODE &&
		m_viewFilter != FT_NULL_FILTER) {
		if (!W3DShaderManager::filterSetup(m_viewFilter, m_viewFilterMode))
		{	//setup failed so restore previous mode.
			m_viewFilter = oldFilter;
			noteViewFilterForCameraClock();
			return FALSE;
		};
	}
	noteViewFilterForCameraClock();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Calculates how many pixels we scrolled since last frame for motion blur calculations. */
//-------------------------------------------------------------------------------------------------
void W3DView::calcDeltaScroll(Coord2D &screenDelta)
{
	screenDelta.x = 0;
	screenDelta.y = 0;
	Vector3 prevPos(m_previousLookAtPosition.x, m_previousLookAtPosition.y, m_pos.z);
	Vector3 prevScreen;
	if (m_3DCamera->Project( prevScreen, prevPos ) != CameraClass::INSIDE_FRUSTUM)
	{
		return;
	}
	Vector3 pos(m_gxCamDrawnLookAt.x, m_gxCamDrawnLookAt.y, m_pos.z);	// GeneralsX: what this draw shows (update)
	Vector3 screen;
	if (m_3DCamera->Project( screen, pos ) != CameraClass::INSIDE_FRUSTUM)
	{
		return;
	}
	screenDelta.x = screen.X-prevScreen.X;
	screenDelta.y = screen.Y-prevScreen.Y;
}


//-------------------------------------------------------------------------------------------------
/** Draw member for the W3D window, this will literally draw the window
  * for this view */
//-------------------------------------------------------------------------------------------------
void W3DView::drawView()
{
	DRAW();
}

//DECLARE_PERF_TIMER(W3DView_drawView)
void W3DView::draw()
{
	//USE_PERF_TIMER(W3DView_drawView)
	Bool skipRender = false;
	Bool doExtraRender = false;
	CustomScenePassModes customScenePassMode  = SCENE_PASS_DEFAULT;
	Bool preRenderResult = false;

	// GeneralsX: the world/HUD seam (W3DWorldSeam.h): with an upscaler on, the world below draws into a private target at the
	// render scale and pop() upscales it under everything drawn after. One branch while GXUpscaler is off. Only the tactical view.
	// Bypassed only for a filter that redirects the scene: every view sits on FT_VIEW_DEFAULT, whose preRender returns FALSE
	// (W3DShaderManager.cpp), so it must not count.
	if (this == TheTacticalView)
		W3DWorldSeam::push(m_3DCamera, m_viewFilterMode && m_viewFilter > FT_NULL_FILTER && m_viewFilter < FT_MAX &&
			m_viewFilter != FT_VIEW_DEFAULT);

	if (m_viewFilterMode &&
			m_viewFilter > FT_NULL_FILTER &&
			m_viewFilter < FT_MAX)
	{
		// Most likely will redirect rendering to a texture.
		preRenderResult=W3DShaderManager::filterPreRender(m_viewFilter, skipRender, customScenePassMode);
		if (!skipRender && getCameraLock())
		{
			Object* cameraLockObj = TheGameLogic->findObjectByID(getCameraLock());
			if (cameraLockObj)
			{
				Drawable *drawable = cameraLockObj->getDrawable();
				drawable->setDrawableHidden(true);
			}
		}
	}

	if (!skipRender)
	{
		// Render 3D scene from our camera
		W3DDisplay::m_3DScene->setCustomPassMode(customScenePassMode);
		if (m_isWireFrameEnabled)
			W3DDisplay::m_3DScene->Set_Extra_Pass_Polygon_Mode(SceneClass::EXTRA_PASS_CLEAR_LINE);
		// GeneralsX: S7 (GXShadowMapHooks.h): the shadow map's frame - the tactical view's
		// default pass only (a filter that redirects the scene draws no map). A49: ARMED here; the scene starts it
		// after its On_Frame_Update loop (GxShadowMapFireArmed), so its poses land where key off's do
		const bool gxShadowFrame = this == TheTacticalView && GxShadowMapFrame && customScenePassMode == SCENE_PASS_DEFAULT;
		if (gxShadowFrame)
		{
			static int s_xNoFire = -1;	// A50 TEST-ONLY GX_SHADOWMAP_X_NOFIRE=1, read once
			if (s_xNoFire < 0)
			{
				const char *e = getenv("GX_SHADOWMAP_X_NOFIRE");
				s_xNoFire = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
				GxShadowMapXNoFire = s_xNoFire == 1;
			}
			GxShadowMapArmedCamera = m_3DCamera;
			GxShadowMapArmedScene = W3DDisplay::m_3DScene;
		}
		W3DDisplay::m_3DScene->doRender( m_3DCamera );
		GxShadowMapArmedCamera = nullptr;	// never left armed past the pass (a scene that did not fire it starts no frame)
		GxShadowMapArmedScene = nullptr;
		if (gxShadowFrame)
			GxShadowMapFrame->tacticalEnd();
		GxShadowMapFrameScene = nullptr;	// A51: only valid inside the pass that fired it
		W3DDisplay::m_3DScene->Set_Extra_Pass_Polygon_Mode(SceneClass::EXTRA_PASS_DISABLE);
		m_isWireFrameEnabled = m_nextWireFrameEnabled;

		// GeneralsX: [GXPICK] where this frame drew the targeting decal's
		// centre, from the camera as it rendered and the viewport CameraClass::Apply gave D3D. See
		// GXPickProbe.h.
		GXPickPreview &gx = TheGXPickPreview;
		gx.drawnValid = FALSE;
		if( gx.valid && TheDisplay )
		{
			Vector2 vMin, vMax;
			Vector3 ndc;
			int bits;
			bool windowed;
			m_3DCamera->Get_Viewport( vMin, vMax );
			WW3D::Get_Render_Target_Resolution( gx.rtW, gx.rtH, bits, windowed );
			if( gx.rtW > 0 && gx.rtH > 0 &&
				m_3DCamera->Project( ndc, Vector3( gx.world.x, gx.world.y, gx.world.z ) ) == CameraClass::INSIDE_FRUSTUM )
			{
				gxPickRenderViewport( vMin.X, vMin.Y, vMax.X, vMax.Y, gx.rtW, gx.rtH, gx.vpX, gx.vpY, gx.vpW, gx.vpH );
				gx.drawnPx = gxPickRenderedPixel( ndc.X, ndc.Y, gx.vpX, gx.vpY, gx.vpW, gx.vpH, gx.rtW, gx.rtH,
					(Int)TheDisplay->getWidth(), (Int)TheDisplay->getHeight() );
				gx.drawnWorld = gx.world;
				gx.drawnValid = TRUE;
			}
		}
	}

	if (m_viewFilterMode &&
			m_viewFilter > FT_NULL_FILTER &&
			m_viewFilter < FT_MAX)
	{
		Coord2D deltaScroll;
		calcDeltaScroll(deltaScroll);
		Bool continueTheEffect = false;
		if (preRenderResult)	//if prerender passed, do the post render.
			continueTheEffect = W3DShaderManager::filterPostRender(m_viewFilter, m_viewFilterMode, deltaScroll,doExtraRender);
		if (!skipRender && getCameraLock())
		{
			Object* cameraLockObj = TheGameLogic->findObjectByID(getCameraLock());
			if (cameraLockObj)
			{
				Drawable *drawable = cameraLockObj->getDrawable();
				drawable->setDrawableHidden(false);
				RenderInfoClass rinfo(*m_3DCamera);
				// Apply the camera and viewport (including depth range)
				m_3DCamera->Apply();
				TheDX8MeshRenderer.Set_Camera(&rinfo.Camera);
				W3DDisplay::m_3DScene->renderSpecificDrawables(rinfo, 1, &drawable);
				WW3D::Flush(rinfo);
			}
		}
		if (!continueTheEffect)
		{
			// shut it down.
			m_viewFilter = FT_VIEW_DEFAULT;
			m_viewFilterMode = FM_VIEW_DEFAULT;
		}
	}

	//Some effects require that we render a modified version of the scene into a texture but also require
	//an unaltered version in the framebuffer.  So we re-render again into framebuffer after texture rendering
	//was turned off by filterPostRender().
	if (doExtraRender)
	{
		//Reset to normal scene rendering.
		//The pass that rendered into a texture may have left the z-buffer in a weird state
		//so clear it before rendering normal scene.
		///@todo: Don't clear z-buffer unless shader uses z-bias or anything else that would cause <= z to fail on normal render.
		Renderer::Clear(false, true, Vector3(0.0f,0.0f,0.0f), TheWaterTransparency->m_minWaterOpacity);	// Clear z but not color
		W3DDisplay::m_3DScene->setCustomPassMode(SCENE_PASS_DEFAULT);
		W3DDisplay::m_3DScene->doRender( m_3DCamera );
		Coord2D deltaScroll;
		W3DShaderManager::filterPostRender(m_viewFilter, m_viewFilterMode, deltaScroll, doExtraRender);
	}

	W3DWorldSeam::pop();	// the world ends here; everything below is screen space at full resolution

	if( TheGlobalData->m_debugAI )
	{
		if (TheAI->pathfinder()->getDebugPath())
		{
			// setup screen clipping region
			IRegion2D clipRegion;
			clipRegion.lo.x = 0;
			clipRegion.lo.y = 0;
			clipRegion.hi.x = getWidth();
			clipRegion.hi.y = getHeight();

			UnsignedInt color = 0xFFFFFF00;  //0xAARRGGBB
			ICoord2D start, end;
			PathNode *prevNode = TheAI->pathfinder()->getDebugPath()->getFirstNode();

			if (worldToScreen( prevNode->getPosition(), &start )) {
				TheDisplay->drawLine( start.x-3, start.y-3, start.x+3, start.y-3, 1.0f, color );
				TheDisplay->drawLine( start.x+3, start.y-3, start.x+3, start.y+3, 1.0f, color );
				TheDisplay->drawLine( start.x+3, start.y+3, start.x-3, start.y+3, 1.0f, color );
				TheDisplay->drawLine( start.x-3, start.y+3, start.x-3, start.y-3, 1.0f, color );
			}
			for( PathNode *node = prevNode->getNext(); node; node = node->getNext() )
			{
				Int k;
				Coord3D s, e;
				Coord3D delta;
				s = *node->getPosition();
				e = *prevNode->getPosition();
				delta.x = e.x-s.x;
				delta.y = e.y-s.y;
				delta.z = e.z-s.z;
				for (k = 0; k<10; k++) {
					Real factor1 = (k)/10.0;
					Real factor2 = (k+1)/10.0;
					s = *node->getPosition();
					e = *node->getPosition();
					s.x += delta.x*factor1;
					s.y += delta.y*factor1;
					s.z += delta.z*factor1;
					e.x += delta.x*factor2;
					e.y += delta.y*factor2;
					e.z += delta.z*factor2;
					Bool onScreen1 = worldToScreen( &e, &end );
					Bool onScreen2 = worldToScreen( &s, &start );
					if (!onScreen1 && !onScreen2) {
						continue; // neither point visible.
					}
					ICoord2D clipStart, clipEnd;

					if( ClipLine2D( &start, &end, &clipStart, &clipEnd, &clipRegion ) ) {
						TheDisplay->drawLine( clipStart.x, clipStart.y, clipEnd.x, clipEnd.y, 1.0f, color );
					}
				}
				prevNode = node;
				if (node->getNext()) {
					if (worldToScreen( node->getPosition(), &start )) {
 						TheDisplay->drawLine( start.x-4, start.y, start.x+3, start.y, 1.0f, color );
					}
				}
			}
			if (prevNode && worldToScreen( prevNode->getPosition(), &start )) {
 				TheDisplay->drawLine( start.x-4, start.y, start.x+3, start.y, 1.0f, color );
				TheDisplay->drawLine( start.x, start.y-4, start.x, start.y+3, 1.0f, color );
			}
			color = 0xFFFF0000;  //0xAARRGGBB
			if (worldToScreen( TheAI->pathfinder()->getDebugPathPosition(), &start )) {
				TheDisplay->drawLine( start.x-3, start.y, start.x+3, start.y, 1.0f, color );
				TheDisplay->drawLine( start.x, start.y-3, start.x, start.y+3, 1.0f, color );
			}
		}

	}

#if defined(RTS_DEBUG)
	if( TheGlobalData->m_debugCamera )
	{
		UnsignedInt c = 0xaaffffff;
		Coord3D worldPos = getPosition();
		worldPos.z = TheTerrainLogic->getGroundHeight(worldPos.x, worldPos.y);

		Coord3D p1, p2;
		ICoord2D s1, s2;
		p1 = worldPos;
		p1.x += TERRAIN_SAMPLE_SIZE;
		p1.y += TERRAIN_SAMPLE_SIZE;
		p1.z = TheTerrainLogic->getGroundHeight(p1.x, p1.y);
		p2 = worldPos;
		p2.x += TERRAIN_SAMPLE_SIZE;
		p2.y -= TERRAIN_SAMPLE_SIZE;
		p2.z = TheTerrainLogic->getGroundHeight(p2.x, p2.y);
		worldToScreen( &p1, &s1 );
		worldToScreen( &p2, &s2 );
		TheDisplay->drawLine(s1.x, s1.y, s2.x, s2.y, 1.0f, c);

		p1 = worldPos;
		p1.x += TERRAIN_SAMPLE_SIZE;
		p1.y -= TERRAIN_SAMPLE_SIZE;
		p1.z = TheTerrainLogic->getGroundHeight(p1.x, p1.y);
		p2 = worldPos;
		p2.x -= TERRAIN_SAMPLE_SIZE;
		p2.y -= TERRAIN_SAMPLE_SIZE;
		p2.z = TheTerrainLogic->getGroundHeight(p2.x, p2.y);
		worldToScreen( &p1, &s1 );
		worldToScreen( &p2, &s2 );
		TheDisplay->drawLine(s1.x, s1.y, s2.x, s2.y, 1.0f, c);

		p1 = worldPos;
		p1.x -= TERRAIN_SAMPLE_SIZE;
		p1.y -= TERRAIN_SAMPLE_SIZE;
		p1.z = TheTerrainLogic->getGroundHeight(p1.x, p1.y);
		p2 = worldPos;
		p2.x -= TERRAIN_SAMPLE_SIZE;
		p2.y += TERRAIN_SAMPLE_SIZE;
		p2.z = TheTerrainLogic->getGroundHeight(p2.x, p2.y);
		worldToScreen( &p1, &s1 );
		worldToScreen( &p2, &s2 );
		TheDisplay->drawLine(s1.x, s1.y, s2.x, s2.y, 1.0f, c);

		p1 = worldPos;
		p1.x -= TERRAIN_SAMPLE_SIZE;
		p1.y += TERRAIN_SAMPLE_SIZE;
		p1.z = TheTerrainLogic->getGroundHeight(p1.x, p1.y);
		p2 = worldPos;
		p2.x += TERRAIN_SAMPLE_SIZE;
		p2.y += TERRAIN_SAMPLE_SIZE;
		p2.z = TheTerrainLogic->getGroundHeight(p2.x, p2.y);
		worldToScreen( &p1, &s1 );
		worldToScreen( &p2, &s2 );
		TheDisplay->drawLine(s1.x, s1.y, s2.x, s2.y, 1.0f, c);

	}

  if ( TheGlobalData->m_showAudioLocations )
  {
    // Draw audio radii for ALL drawables, not just those on screen
    const Drawable * drawable = TheGameClient->getDrawableList();

    while ( drawable != nullptr )
    {
      drawAudioRadii( drawable );
      drawable = drawable->getNextDrawable();
    }
  }
#endif // RTS_DEBUG

	Region3D axisAlignedRegion;
	getAxisAlignedViewRegion(axisAlignedRegion);

	//
	// there are several things we might want to do as a post pass on the objects after
	// they are all drawn
	/// @todo we might want to consider wiping this iterate out if there is nothing to post draw
	//
	TheGameClient->resetRenderedObjectCount();

	TheDisplay->beginBatch();
	// GeneralsX: Upstream TheSuperHackers PR #2044 (open) attacks the same
	// double walk with a retained list fed from GameLogic; this is a client-only subset. If no drawable
	// was added, removed or moved since update()'s draw walk and the region is unchanged, that walk's
	// visit list IS what a fresh walk would produce, in the same order, so replay it instead of
	// walking every drawable in the world again. If a callback itself changes anything, finish with a
	// live walk from the list successor it had before the callback - exactly what the plain walk does.
	if( m_drawWalkValid
			&& m_drawWalkChangeCount == GameClient::getDrawableChangeCount()
			&& m_drawWalkRegion.lo.x == axisAlignedRegion.lo.x && m_drawWalkRegion.hi.x == axisAlignedRegion.hi.x
			&& m_drawWalkRegion.lo.y == axisAlignedRegion.lo.y && m_drawWalkRegion.hi.y == axisAlignedRegion.hi.y
			&& m_drawWalkRegion.lo.z == axisAlignedRegion.lo.z && m_drawWalkRegion.hi.z == axisAlignedRegion.hi.z )
	{
		Drawable *resumeFrom = nullptr;
		Bool changed = FALSE;
		for( size_t i = 0; i < m_drawWalkList.size(); ++i )
		{
			Drawable *draw = m_drawWalkList[i];
			resumeFrom = draw->getNextDrawable();
			drawablePostDraw( draw, this );
			if( m_drawWalkChangeCount != GameClient::getDrawableChangeCount() )
			{
				changed = TRUE;
				break;
			}
		}
		if( changed )
		{
			m_drawWalkValid = FALSE;
			for( Drawable *draw = resumeFrom, *next; draw; draw = next )
			{
				next = draw->getNextDrawable();
				const Coord3D *pos = draw->getPosition();
				if( draw->getVisualTransform() != nullptr ||	// a drawn superweapon missile is always walked, as in GameClient::iterateDrawablesInRegion
						(pos->x >= axisAlignedRegion.lo.x && pos->x <= axisAlignedRegion.hi.x &&
						 pos->y >= axisAlignedRegion.lo.y && pos->y <= axisAlignedRegion.hi.y &&
						 pos->z >= axisAlignedRegion.lo.z && pos->z <= axisAlignedRegion.hi.z) )
					drawablePostDraw( draw, this );
			}
		}
	}
	else
	{
		TheGameClient->iterateDrawablesInRegion( &axisAlignedRegion, drawablePostDraw, this );
	}
	TheDisplay->endBatch();

	TheGameClient->flushTextBearingDrawables();

	// Render 2D scene
	W3DDisplay::m_2DScene->doRender( m_2DCamera );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::setCameraLock(ObjectID id)
{
	// If we're disabling camera movements, don't lock onto the object.
	if (TheGlobalData->m_disableCameraMovement && id!=INVALID_ID) {
		return;
	}
	View::setCameraLock(id);
	removeScriptedState(Scripted_CameraLock);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::setSnapMode( CameraLockType lockType, Real lockDist )
{
	View::setSnapMode(lockType, lockDist);
	addScriptedState(Scripted_CameraLock);
}

//-------------------------------------------------------------------------------------------------
// Scroll the view by the given delta in SCREEN COORDINATES, this interface
// assumes we will be scrolling along the X,Y plane
//
// TheSuperHackers @bugfix Now rotates the view plane on the Z axis only to properly discard the
// camera pitch. The aspect ratio also no longer modifies the vertical scroll speed.
//-------------------------------------------------------------------------------------------------
void W3DView::scrollBy( const Coord2D *delta )
{
	if( delta && (delta->x != 0 || delta->y != 0) )
	{
		constexpr const Real SCROLL_RESOLUTION = 250.0f;

		Vector3 world, worldStart, worldEnd;
		Vector2 start, end;

		m_scrollAmount = *delta;

		start.X = getWidth();
		start.Y = getHeight();

		end.X = start.X + delta->x * SCROLL_RESOLUTION;
		end.Y = start.Y + delta->y * SCROLL_RESOLUTION;

		m_3DCamera->Device_To_View_Space( start, &worldStart );
		m_3DCamera->Device_To_View_Space( end, &worldEnd );

		const Real zRotation = m_3DCamera->Get_Transform().Get_Z_Rotation();
		worldStart.Rotate_Z(zRotation);
		worldEnd.Rotate_Z(zRotation);

		world.X = worldEnd.X - worldStart.X;
		world.Y = worldEnd.Y - worldStart.Y;
		world.Z = worldEnd.Z - worldStart.Z;

		// scroll by delta
		Coord2D pos = getPosition2D();
		pos.x += world.X;
		pos.y += world.Y;
		//DEBUG_LOG(("Delta %.2f, %.2f", world.X, world.Z));
		setPosition2D(pos);

		//m_cameraConstraintValid = false;	// pos change does NOT invalidate cam constraints

		removeScriptedState(Scripted_Rotate);
		m_recalcCamera = true;
	}
	else
	{
		m_scrollAmount.x = 0;
		m_scrollAmount.y = 0;
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::forceRedraw()
{
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Rotate the view around the up axis to the given angle. */
//-------------------------------------------------------------------------------------------------
void W3DView::setAngle( Real radians )
{
	View::setAngle( radians );

	stopDoingScriptedCamera();
	m_CameraArrivedAtWaypointOnPathFlag = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Rotate the view around the horizontal (X) axis to the given angle. */
//-------------------------------------------------------------------------------------------------
void W3DView::setPitch( Real radians )
{
	View::setPitch( radians );

	stopDoingScriptedCamera();
	// TheSuperHackers @fix Now recalculates the camera constraints because
	// the camera pitch can change the camera distance towards the map border.
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setDefaultPitch( Real radians )
{
	View::setDefaultPitch( radians );

	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Set the view angle back to default */
//-------------------------------------------------------------------------------------------------
void W3DView::setAngleToDefault()
{
	View::setAngleToDefault();

	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Set the view pitch back to default */
//-------------------------------------------------------------------------------------------------
void W3DView::setPitchToDefault()
{
	View::setPitchToDefault();

	m_FXPitch = 1.0f;

	// TheSuperHackers @fix Now recalculates the camera constraints because
	// the camera pitch can change the camera distance towards the map border.
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
// TheSuperHackers @fix Mauller Adjust the camera height to compensate for the screen aspect ratio
static Real gxCameraAspectRatioScale()
{
	Real baseAspectRatio = (Real)DEFAULT_DISPLAY_WIDTH / (Real)DEFAULT_DISPLAY_HEIGHT;
	Real currentAspectRatio = (Real)TheTacticalView->getWidth() / (Real)TheTacticalView->getHeight();
	Real aspectRatioScale = 0.0f;

	if (currentAspectRatio > baseAspectRatio)
	{
		aspectRatioScale = fabs(( 1 + ( currentAspectRatio - baseAspectRatio) ));
	}
	else
	{
		aspectRatioScale = fabs(( 1 - ( baseAspectRatio - currentAspectRatio) ));
	}
	return aspectRatioScale;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature THE TOP OF THE DEBUG MENU'S CAMERA RANGE IS THE WHOLE LOADED MAP.
// The height above ground at which a camera looking at the middle of the map has all four
// corners of its playable area on screen, for every pitch the player may take here (View::getPlayerPitchRange, and the
// current default pitch) at the default yaw - so tilting never cuts the map, and turning the camera off its default yaw
// can - and from every pivot the area constraints still allow at that height (calcCameraAreaConstraints: the pivot keeps
// GX_CAMERA_AREA_OFFSET_SCALE x the bottom edge's ground distance from each edge, at most the shorter half side, so a
// rectangular map's pivot roams along its long side). The body sits at the pivot plus h / sin(default pitch) along the
// view (buildCameraPosition), and the view's own frustum supplies the fields of view. 0 with no map, no view or the scripted real-zoom camera.
Real W3DView::getWholeMapHeightAboveGround()
{
	if (TheTerrainLogic == nullptr || m_3DCamera == nullptr || m_useRealZoomCam)
		return 0.0f;
	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	const Real halfX = 0.5f * (extent.hi.x - extent.lo.x);
	const Real halfY = 0.5f * (extent.hi.y - extent.lo.y);
	Vector2 planeMin, planeMax;
	m_3DCamera->Get_View_Plane(planeMin, planeMax);
	Real lo, hi;
	View::getPlayerPitchRange(&lo, &hi);
	const Real yaw = m_defaultAngle - ViewDefaultYawRadians;
	const Real maxDist = 1.0e6f;
	const Real offsetBias = TheGlobalData->m_debugAI ? -1000.0f : 0.0f;	// calcCameraAreaOffset's own
	enum { GX_WHOLE_MAP_PITCH_STEPS = 16 };
	Real worst = 0.0f;
	for (Int i = 0; i <= GX_WHOLE_MAP_PITCH_STEPS + 1; ++i)
	{
		const Real pitch = (i <= GX_WHOLE_MAP_PITCH_STEPS) ? lo + (hi - lo) * (Real)i / (Real)GX_WHOLE_MAP_PITCH_STEPS : getDefaultPitch();
		const Real dist = gxWholeMapDistance(halfX, halfY, pitch, yaw, planeMax.X, planeMax.Y, maxDist, GX_CAMERA_AREA_OFFSET_SCALE, offsetBias);
		if (dist < 0.0f)
			return 0.0f;	// not within reach at all: leave the setting alone
		if (dist > worst)
			worst = dist;
	}
	return worst * sinf(ViewDefaultPitchRadians);
}

//-------------------------------------------------------------------------------------------------
// A Debug-range max (a setting above the player's CameraMaxHeightHi) stops at the whole map: past it the map only
// shrinks into the middle of the screen. Never below the player's own top, so the Debug range never gives less than the
// launcher's. Inside the player's range - every multiplayer match with a second human - nothing changes.
// What it capped is kept (m_gxUncappedMax...): the selection overlays size themselves against the uncapped max
// (getRetailReferenceZoom), and the cap depends on the viewport, so setWidth/setHeight take it again (recapToWholeMap).
Real W3DView::capToWholeMap(Real maxAboveGround, Real effectiveMaxHeight, Real playerTopAboveGround)
{
	m_gxCappedMax = 0.0f;
	if (effectiveMaxHeight <= (Real)CameraMaxHeightHi)
		return maxAboveGround;
	const Real whole = getWholeMapHeightAboveGround();
	if (whole <= 0.0f)
		return maxAboveGround;
	m_gxUncappedMax = maxAboveGround;
	m_gxCapSetting = effectiveMaxHeight;
	m_gxCapPlayerTop = playerTopAboveGround;
	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	m_gxCapExtentX = extent.hi.x;
	m_gxCapExtentY = extent.hi.y;
	m_gxCappedMax = std::min(maxAboveGround, std::max(whole, playerTopAboveGround));
	return m_gxCappedMax;
}

//-------------------------------------------------------------------------------------------------
// TRUE while the max in force is the one capToWholeMap set (anything that sets the max since has replaced it).
Bool W3DView::isWholeMapCapInForce() const
{
	return m_gxCappedMax > 0.0f && m_maxHeightAboveGround == m_gxCappedMax;
}

//-------------------------------------------------------------------------------------------------
// setZoom's scale is the max before the cap, as it was before the cap existed: a bookmark (View::getLocation keeps the
// zoom, which getCameraOffsetZ normalises by the setting) comes back at the height it was taken at.
Real W3DView::getZoomReferenceHeight()
{
	return isWholeMapCapInForce() ? m_gxUncappedMax : m_maxHeightAboveGround;
}

//-------------------------------------------------------------------------------------------------
void W3DView::recapToWholeMap()
{
	if (!isWholeMapCapInForce())
		return;
	m_maxHeightAboveGround = capToWholeMap(m_gxUncappedMax, m_gxCapSetting, m_gxCapPlayerTop);
	if (m_minHeightAboveGround > m_maxHeightAboveGround)
		m_maxHeightAboveGround = m_minHeightAboveGround;
	// a lower cap brings the player's camera down to it now, as the wheel's own clamp would
	if (m_isUserControlled && m_heightAboveGround > m_maxHeightAboveGround)
		setHeightAboveGround(m_maxHeightAboveGround);
}

//-------------------------------------------------------------------------------------------------
Real W3DView::getWholeMapCameraHeightSetting()
{
	const Real whole = getWholeMapHeightAboveGround();
	const Real aspectRatioScale = gxCameraAspectRatioScale();
	return (whole > 0.0f && aspectRatioScale > 0.0f) ? whole / aspectRatioScale : 0.0f;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setCameraHeightAboveGroundLimitsToDefault(Real heightScale)
{
	const Real aspectRatioScale = gxCameraAspectRatioScale();

	// GeneralsX: Host-authoritative camera limits: see GXHostCamera.h.
	// One call, not two, so min and max can never come from different sides of a match boundary.
	// GeneralsX: Shell map pin layered over the host-camera limits; still
	// ONE resolution for the pair, so min and max cannot come from different sides of a boundary.
	Real effectiveMinHeight, effectiveMaxHeight;
	getEffectiveCameraHeightLimits(&effectiveMinHeight, &effectiveMaxHeight);
	m_maxHeightAboveGround = effectiveMaxHeight * aspectRatioScale * heightScale;
	m_maxHeightAboveGround = capToWholeMap(m_maxHeightAboveGround, effectiveMaxHeight, (Real)CameraMaxHeightHi * aspectRatioScale * heightScale);
	// GeneralsX: Only the MAX takes the aspect allowance, as in Generals Online:
	// GO scales m_maxHeightAboveGround alone (GameClient W3DView.cpp setDefaultView,
	// 1 + (w/h - 4/3)) and its view takes the min height unscaled (View.cpp View::init). Scaling the min too left a
	// 16:9 player unable to zoom in past 144 for a setting of 100 - a widescreen zooms OUT further, not in less.
	m_minHeightAboveGround = effectiveMinHeight;

	if (m_minHeightAboveGround > m_maxHeightAboveGround)
		m_maxHeightAboveGround = m_minHeightAboveGround;

	// GeneralsX @feature One line whenever the player's camera range changes (a match start, Camera Apply, a resolution
	// change): the limits above ground, the whole-map height, the pitch range and the default pitch in force, so a run
	// can show which range applied. Release-visible like [GXHCAM]; printed on a change only.
	{
		static Real s_said[6] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
		Real pitchLo, pitchHi;
		View::getPlayerPitchRange(&pitchLo, &pitchHi);
		const Real now[6] = { m_minHeightAboveGround, m_maxHeightAboveGround, getWholeMapHeightAboveGround(),
			RAD_TO_DEGF(pitchLo), RAD_TO_DEGF(pitchHi), RAD_TO_DEGF(getDefaultPitch()) };
		if (memcmp(now, s_said, sizeof(now)) != 0)
		{
			memcpy(s_said, now, sizeof(now));
			fprintf(stderr, "[GXCAMLIMIT] min=%.1f max=%.1f whole_map=%.1f setting=%.1f pitch_range=%.1f..%.1f default_pitch=%.1f debug_range=%s\n",
				now[0], now[1], now[2], effectiveMaxHeight, now[3], now[4], now[5], GXHostCamera::debugRangeAllowed() ? "yes" : "no");
			fflush(stderr);
		}
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setDefaultView(Real pitch, Real angle, Real maxHeight)
{
	// MDC - we no longer want to rotate maps (design made all of them right to begin with)
	//	m_defaultAngle = angle * M_PI/180.0f;
	setDefaultPitch(pitch);
	// GeneralsX: Host-authoritative camera limits: see GXHostCamera.h.
	m_maxHeightAboveGround = getEffectiveMaxCameraHeight()*maxHeight;
	m_maxHeightAboveGround = capToWholeMap(m_maxHeightAboveGround, getEffectiveMaxCameraHeight(), (Real)CameraMaxHeightHi * maxHeight);
	if (m_minHeightAboveGround > m_maxHeightAboveGround)
		m_maxHeightAboveGround = m_minHeightAboveGround;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setHeightAboveGround(Real z)
{
	View::setHeightAboveGround(z);

	stopDoingScriptedCamera();
	m_CameraArrivedAtWaypointOnPathFlag = false;
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
// TheSuperHackers @bugfix xezon 18/09/2025 setZoom is no longer clamped by a min and max zoom.
// Instead the min and max camera height will be clamped elsewhere. Clamping the zoom would cause
// issues with camera playback in replay playback where changes in terrain elevation would not raise
// the camera height.
void W3DView::setZoom(Real z)
{
	m_heightAboveGround = getZoomReferenceHeight() * z;
	m_zoom = getDesiredZoom(m_pos.x, m_pos.y);

	stopDoingScriptedCamera();
	m_CameraArrivedAtWaypointOnPathFlag = false;
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setZoomToMax()
{
	// default zoom has to be max, otherwise players will just zoom to max always
	// GeneralsX: The start rises with the max, as in Generals Online: GO
	// starts a match at m_defaultMaxHeightAboveGround, which setDefaultView makes the max itself, aspect allowance
	// included (GameClient ecb828ebd, Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DView.cpp:2295 and :2304,
	// setZoomToDefault :2349). So 447.8 at 16:9 for the default 310, 1444.4 for 1000, 4333.3 for the Debug menu's 3000.
	m_heightAboveGround = m_maxHeightAboveGround;
	m_zoom = getMaxZoom(m_pos.x, m_pos.y);

	m_cameraAreaConstraintsValid = false; // recalc it.
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::setZoomToDefault()
{
	// default zoom has to be max, otherwise players will just zoom to max always
	stopDoingScriptedCamera();
	m_CameraArrivedAtWaypointOnPathFlag = false;
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Set the horizontal field of view angle */
//-------------------------------------------------------------------------------------------------
void W3DView::setFieldOfView( Real angle )
{
	View::setFieldOfView( angle );

#if defined(RTS_DEBUG)
	// this is only for testing, and recalculating the
	// camera every frame is wasteful
	m_cameraAreaConstraintsValid = false;
	m_recalcCamera = true;
#endif
}

//-------------------------------------------------------------------------------------------------
/** Using the W3D camera translate the world coordinate to a screen coord.
	Screen coordinates returned in absolute values relative to full display resolution.
  Returns if the point is on screen, off screen, or not transformable */
//-------------------------------------------------------------------------------------------------
View::WorldToScreenReturn W3DView::worldToScreenTriReturn( const Coord3D *w, ICoord2D *s )
{
	// sanity
	if( w == nullptr || s == nullptr )
    return WTS_INVALID;

	if( m_3DCamera )
	{
		Vector3 world;
		Vector3 screen;

		world.Set( w->x, w->y, w->z );
		enum CameraClass::ProjectionResType projection = m_3DCamera->Project( screen, world );
		if (projection != CameraClass::INSIDE_FRUSTUM && projection!=CameraClass::OUTSIDE_FRUSTUM)
		{
			// Can't get a valid number if it's beyond the clip planes.  jba
			s->x = 0;
			s->y = 0;
      return WTS_INVALID;
		}

		//
		// note that the screen coord returned from the project W3D camera
		// gave us a screen coords that range from (-1,-1) bottom left to
		// (1,1) top right ... we are turning that into (0,0) upper left
		// coords now
		//
		W3DLogicalScreenToPixelScreen( screen.X, screen.Y,
																	 &s->x, &s->y,
																	 getWidth(), getHeight());
		s->x += m_originX;	//convert viewport coordinates to full screen coordinates
		s->y += m_originY;

//		s->x = (getWidth()  * (screen.X + 1.0f)) / 2.0f;
//		s->y = (getHeight() * (-screen.Y + 1.0f)) / 2.0f;
		if (projection != CameraClass::INSIDE_FRUSTUM)
		{
      return WTS_OUTSIDE_FRUSTUM;
		}

    return WTS_INSIDE_FRUSTUM;

	}

  return WTS_INVALID;
}

//-------------------------------------------------------------------------------------------------
/** all the drawables in the view, that fall within the 2D screen region
	* will call the callback function.  The number of drawables that passed
	* the test are returned.
	Screen coordinates assumed in absolute values relative to full display resolution. */
//-------------------------------------------------------------------------------------------------
Int W3DView::iterateDrawablesInRegion( IRegion2D *screenRegion,
																			 Bool (*callback)( Drawable *draw, void *userData ),
																			 void *userData )
{
	Bool inside = FALSE;
	Int count = 0;
	Drawable *draw;
	Vector3 screen, world;
	Coord3D pos;
	Region2D normalizedRegion;

	/** @todo we need to have partitions of which drawables are in the
	view so we don't have to march through the whole list */

	//
	// to do this we are projecting the drawable centers onto the screen,
	// the W3D camera->project method is used to do this and that method
	// will return normalized screen coords from (-1,-1) bottom left to
	// (1,1) top right, normalize our screen region for comparison
	//
	/// @todo use fast int->real type casts here later

	Bool regionIsPoint = FALSE;

	if( screenRegion )
	{
		if (screenRegion->height() == 0 && screenRegion->width() == 0)
		{
			regionIsPoint = TRUE;
		}

		normalizedRegion.lo.x = ((Real)(screenRegion->lo.x - m_originX) / (Real)getWidth()) * 2.0f - 1.0f;
		normalizedRegion.lo.y = -(((Real)(screenRegion->hi.y - m_originY) / (Real)getHeight()) * 2.0f - 1.0f);
		normalizedRegion.hi.x = ((Real)(screenRegion->hi.x - m_originX) / (Real)getWidth()) * 2.0f - 1.0f;
		normalizedRegion.hi.y = -(((Real)(screenRegion->lo.y - m_originY) / (Real)getHeight()) * 2.0f - 1.0f);

	}


	Drawable *onlyDrawableToTest = nullptr;
	if (regionIsPoint)
	{
		// Allow all drawables to be picked.
		onlyDrawableToTest = pickDrawable(&screenRegion->lo, TRUE, (PickType) getPickTypesForContext(TheInGameUI->isInForceAttackMode()));
		if (onlyDrawableToTest == nullptr) {
			return 0;
		}
	}

	for( draw = TheGameClient->firstDrawable();
			 draw;
			 draw = draw->getNextDrawable() )
	{
		if (onlyDrawableToTest)
		{
		 draw = onlyDrawableToTest;
		 inside = TRUE;
		}
		else
		{

			// not inside
			inside = FALSE;

			// no screen region, means all drawbles
			if( screenRegion == nullptr )
				inside = TRUE;
			else
			{

				// project the center of the drawable to the screen
				/// @todo use a real 3D position in the drawable
				pos = *draw->getPosition();
				world.X = pos.x;
				world.Y = pos.y;
				world.Z = pos.z;

				// project the world point to the screen
				if( m_3DCamera->Project( screen, world ) == CameraClass::INSIDE_FRUSTUM &&
						screen.X >= normalizedRegion.lo.x &&
						screen.X <= normalizedRegion.hi.x &&
						screen.Y >= normalizedRegion.lo.y &&
						screen.Y <= normalizedRegion.hi.y )
				{

					inside = TRUE;

				}
			}

		}

		// if inside do the callback and count up
		if( inside )
		{

			if( callback( draw, userData ) )
				++count;

		}

		// If onlyDrawableToTest, then we should bail out now.
		if (onlyDrawableToTest != nullptr)
			break;

	}

	return count;

}

//-------------------------------------------------------------------------------------------------
/** cast a ray from the screen coords into the scene and return a drawable
  * there if present. Screen coordinates assumed in absolute values relative
  * to full display resolution. */
//-------------------------------------------------------------------------------------------------
Drawable *W3DView::pickDrawable( const ICoord2D *screen, Bool forceAttack, PickType pickType )
{
	RenderObjClass *renderObj = nullptr;
	Drawable *draw = nullptr;
	DrawableInfo *drawInfo = nullptr;

	// sanity
	if( screen == nullptr )
		return nullptr;

	// don't pick a drawable if there is a window under the cursor
	GameWindow *window = nullptr;
	if (TheWindowManager)
		window = TheWindowManager->getWindowUnderCursor(screen->x, screen->y);

	while (window)
	{
		// check to see if it or any of its parents are opaque.  If so, we can't select anything.
		if (!BitIsSet( window->winGetStatus(), WIN_STATUS_SEE_THRU ))
			return nullptr;

		window = window->winGetParent();
	}

	Vector3 rayStart,rayEnd;
	getPickRay(screen,&rayStart,&rayEnd);

	LineSegClass lineseg;
	lineseg.Set(rayStart,rayEnd);

	CastResultStruct result;

	if (forceAttack)
		result.ComputeContactPoint = true;

	//Don't check against translucent or hidden objects
	RayCollisionTestClass raytest(lineseg,&result,COLL_TYPE_ALL,false,false);

	if( W3DDisplay::m_3DScene->castRay( raytest, false, (Int)pickType ) )
		renderObj = raytest.CollidedRenderObj;

	// for right now there is no drawable data in a render object which is			 	// if we've found a render object, return our drawable associated with it,

	// the terrain, therefore the userdata is null
	/// @todo terrain and picking!
	if( renderObj )
		drawInfo = (DrawableInfo *)renderObj->Get_User_Data();
	if (drawInfo)
		draw=drawInfo->m_drawable;

	return draw;

}

//-------------------------------------------------------------------------------------------------
/** convert a pixel (x,y) to a location in the world on the terrain.
	Screen coordinates assumed in absolute values relative to full display resolution.  */
// TheSuperHackers @fix Now returns whether a terrain intersection exists to let callers handle the
// failure condition.
//-------------------------------------------------------------------------------------------------
Bool W3DView::screenToTerrain( const ICoord2D *screen, Coord3D *world )
{
	if( screen == nullptr || world == nullptr || TheTerrainRenderObject == nullptr )
		return false;

	if (m_lastScreenToTerrainValid &&
		m_lastScreenToTerrainScreen.x == screen->x && m_lastScreenToTerrainScreen.y == screen->y)
	{
		*world = m_lastScreenToTerrainWorld;
		return true;
	}

	Vector3 rayStart,rayEnd;
	LineSegClass lineseg;
	CastResultStruct result;
	Vector3 intersection(0,0,0);
	Bool hasIntersection = false;

	getPickRay(screen,&rayStart,&rayEnd);

	lineseg.Set(rayStart,rayEnd);

	RayCollisionTestClass raytest(lineseg,&result);

	// Get the point of intersection according to W3D
	if( TheTerrainRenderObject->Cast_Ray(raytest) )
	{
		intersection = result.ContactPoint;
		hasIntersection = true;
	}

	// Pick bridges.
	Vector3 bridgePt;
	Drawable *bridge = TheTerrainLogic->pickBridge(rayStart, rayEnd, &bridgePt);
	if (bridge && bridgePt.Z > intersection.Z) {
		intersection = bridgePt;
		hasIntersection = true;
	}

	if (!hasIntersection)
		return false;

	world->x = intersection.X;
	world->y = intersection.Y;
	world->z = intersection.Z;

	m_lastScreenToTerrainScreen = *screen;
	m_lastScreenToTerrainWorld = *world;
	m_lastScreenToTerrainValid = true;

	return true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::lookAt( const Coord3D *o )
{
	Coord3D pos = *o;

// no, don't call the super-lookAt, since it will munge our coords
// as for a 2d view. just call setPosition.
//View::lookAt(&pos);

	if (o->z > PATHFIND_CELL_SIZE_F+TheTerrainLogic->getGroundHeight(pos.x, pos.y)) {
		// Pos.z is not used, so if we want to look at something off the ground,
		// we have to look at the spot on the ground such that the object intersects
		// with the look at vector in the center of the screen.  jba.
		Vector3 rayStart,rayEnd;
		LineSegClass lineseg;
		CastResultStruct result;
		Vector3 intersection(0,0,0);

		rayStart = m_3DCamera->Get_Position();	//get camera location
		m_3DCamera->Un_Project(rayEnd,Vector2(0.0f,0.0f));	//get world space point
		rayEnd -= rayStart;	//vector camera to world space point
		rayEnd.Normalize();	//make unit vector
		rayEnd *= m_3DCamera->Get_Depth() * 2;	//adjust length to reach far clip plane and beyond
		rayStart.Set(pos.x, pos.y, pos.z);
		rayEnd += rayStart;	//get point on far clip plane along ray from camera.
		lineseg.Set(rayStart,rayEnd);

		RayCollisionTestClass raytest(lineseg,&result);

		if( TheTerrainRenderObject->Cast_Ray(raytest) )
		{
			// get the point of intersection according to W3D
			pos.x = result.ContactPoint.X;
			pos.y = result.ContactPoint.Y;

		}
	}

	setPosition2D(pos.asCoord2D());

	resetPivotToGround();

#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	if (!m_isUserControlled)
	{
		m_zoom = getDesiredZoom(m_pos.x, m_pos.y);
	}
#endif

	removeScriptedState(Scripted_Rotate | Scripted_CameraLock | Scripted_MoveOnWaypointPath);
	m_CameraArrivedAtWaypointOnPathFlag = false;

	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::initHeightForMap()
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	// jba - starting ground level can't exceed this height.
	constexpr const Real MAX_GROUND_LEVEL = 120.0f;
	const Real accurateGroundLevel = TheTerrainLogic->getGroundHeight(m_pos.x, m_pos.y);
	m_initialGroundLevel = min(MAX_GROUND_LEVEL, accurateGroundLevel);
#endif

	resetPivotToGround();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::resetPivotToGround()
{
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	if (!m_isUserControlled)
	{
		m_pos.z = m_initialGroundLevel;
		m_cameraAreaConstraintsValid = false; // possible ground level change invalidates camera constraints
		m_recalcCamera = true;
		return;
	}
#endif
	m_pos.z = getHeightAroundPos(m_pos.x, m_pos.y);
	m_cameraAreaConstraintsValid = false; // possible ground level change invalidates camera constraints
	m_recalcCamera = true;
}

//-------------------------------------------------------------------------------------------------
/** Move camera to in an interesting fashion.  Sets up parameters that get
 * evaluated in draw(). */
//-------------------------------------------------------------------------------------------------
void W3DView::moveCameraTo(const Coord3D *o, Int milliseconds, Int shutter, Bool orient, Real easeIn, Real easeOut)
{
	m_mcwpInfo.waypoints[0] = getPosition();
	m_mcwpInfo.cameraAngle[0] = getAngle();
	m_mcwpInfo.waySegLength[0] = 0;

	m_mcwpInfo.waypoints[1] = getPosition();
	m_mcwpInfo.waySegLength[1] = 0;

	m_mcwpInfo.waypoints[2] = *o;
	m_mcwpInfo.waySegLength[2] = 0;

	m_mcwpInfo.numWaypoints = 2;
	// GeneralsX: Its time is m_camTimeline's (one step a logic frame, the shutter ignored as before): a
	// move of under 1 ms ends on its first logic step.
	m_camTimeline.path(milliseconds, 1, easeIn, easeOut);
	m_mcwpInfo.shutter = 1;
	m_mcwpInfo.curSegment = 1;
	m_mcwpInfo.curSegDistance = 0;
	m_mcwpInfo.totalDistance = 0;

	setupWaypointPath(orient);
	// ...and its "do it instantly" pose step stays, 1 ms of its 1 ms: base Generals' walk (>) puts the camera at the target here,
	// so a rotation chained in the same script starts there; Zero Hour's (>=) ends the walk first and
	// moves it on the first step, as it always did
	if (m_camTimeline.waypointPath().total == 1.0f)
	{
		moveAlongWaypointPath(1.0f, 1.0f, 1.0f, FALSE);
		m_CameraArrivedAtWaypointOnPathFlag = false;
	}
}

//-------------------------------------------------------------------------------------------------
/** Rotate the camera */
//-------------------------------------------------------------------------------------------------
void W3DView::rotateCamera(Real rotations, Int milliseconds, Real easeIn, Real easeOut)
{
	m_rcInfo.trackObject = FALSE;
	m_camTimeline.rotate(milliseconds, 0, easeIn, easeOut);	// GeneralsX: its frames and multiplier; it ends a path
	setUserControlled(false);	// before the start pose, where addScriptedState ran
	m_rcInfo.angle.startAngle = m_angle;
	m_rcInfo.angle.endAngle = m_angle + 2*PI*rotations;
	m_CameraArrivedAtWaypointOnPathFlag = false;
}

//-------------------------------------------------------------------------------------------------
/** Rotate the camera to follow a unit */
//-------------------------------------------------------------------------------------------------
void W3DView::rotateCameraTowardObject(ObjectID id, Int milliseconds, Int holdMilliseconds, Real easeIn, Real easeOut)
{
	m_rcInfo.trackObject = TRUE;
	m_camTimeline.rotate(milliseconds, holdMilliseconds, easeIn, easeOut);	// GeneralsX: its frames, hold and multiplier
	setUserControlled(false);
	m_rcInfo.target.targetObjectID = id;
	m_CameraArrivedAtWaypointOnPathFlag = false;
}

//-------------------------------------------------------------------------------------------------
/** Rotate camera to face a location */
//-------------------------------------------------------------------------------------------------
void W3DView::rotateCameraTowardPosition(const Coord3D *pLoc, Int milliseconds, Real easeIn, Real easeOut, Bool reverseRotation)
{
	m_rcInfo.trackObject = FALSE;

	// GeneralsX: It runs its time wherever the camera stands (m_camTimeline, as the simulation's does):
	// within 0.1 of the target there is no direction to turn to, so it turns nowhere - it used not to start at all, which made
	// CAMERA_MOVEMENT_FINISHED depend on this peer's camera.
	Real endAngle = m_angle;
	Coord2D curPos = getPosition2D();
	Vector2 dir(pLoc->x-curPos.x, pLoc->y-curPos.y);
	const Real dirLength = dir.Length();
	if (dirLength >= 0.1f) {
		Real angle = WWMath::Acos(dir.X/dirLength);
		if (dir.Y<0.0f) {
			angle = -angle;
		}
		// Default camera is rotated 90 degrees, so match.
		angle -= PI/2;
		normAngle(angle);

		if (reverseRotation) {
			if (m_angle < angle) {
				angle -= 2.0f*WWMATH_PI;
			} else {
				angle += 2.0f*WWMATH_PI;
			}
		}
		// TheSuperHackers @todo Investigate if the non Generals code is correct for Zero Hour.
		// It certainly is incorrect for Generals: Seen in GLA mission 1 opening cut scene.
#if RTS_GENERALS
		endAngle = m_angle + angle;
#else
		endAngle = angle;
#endif
	}

	m_camTimeline.rotate(milliseconds, 0, easeIn, easeOut);
	setUserControlled(false);
	m_rcInfo.angle.startAngle = m_angle;
	m_rcInfo.angle.endAngle = endAngle;
	m_CameraArrivedAtWaypointOnPathFlag = false;
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::zoomCamera( Real finalZoom, Int milliseconds, Real easeIn, Real easeOut )
{
	m_camTimeline.zoom(milliseconds);	// GeneralsX: its frames (Common/ScriptedCameraClock.h)
	// before the start zoom is taken, where addScriptedState ran: under PRESERVE_RETAIL_SCRIPTED_CAMERA it converts m_zoom into
	// scripted coordinates (the zoom would otherwise start from the user's)
	setUserControlled(false);
	if (milliseconds<1) milliseconds = 1;
	m_zcInfo.startZoom = m_zoom;
	m_zcInfo.endZoom = finalZoom;
	m_zcInfo.ease.setEaseTimes(easeIn/milliseconds, easeOut/milliseconds);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void W3DView::pitchCamera( Real finalPitch, Int milliseconds, Real easeIn, Real easeOut )
{
	m_camTimeline.pitch(milliseconds);	// GeneralsX: its frames (Common/ScriptedCameraClock.h)
	setUserControlled(false);	// before the start pose, where addScriptedState ran
	if (milliseconds<1) milliseconds = 1;
	m_pcInfo.startPitch = m_FXPitch;
	m_pcInfo.endPitch = finalPitch;
	m_pcInfo.ease.setEaseTimes(easeIn/milliseconds, easeOut/milliseconds);
}

//-------------------------------------------------------------------------------------------------
/** Sets the final zoom for a camera movement. */
//-------------------------------------------------------------------------------------------------
void W3DView::cameraModFinalZoom( Real finalZoom, Real easeIn, Real easeOut )
{
	if (hasScriptedState(Scripted_Rotate))
	{
		const Int time = m_camTimeline.rotationMsLeft();	// saturated Int ms, as the timeline takes them
		zoomCamera( finalZoom*getMaxZoom(m_pos.x, m_pos.y), time, time*easeIn, time*easeOut );
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		Coord3D pos = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
		const Int time = m_camTimeline.pathMsLeft();
		zoomCamera( finalZoom*getMaxZoom(pos.x, pos.y), time, time*easeIn, time*easeOut );
	}
}

//-------------------------------------------------------------------------------------------------
/** Sets the final zoom for a camera movement. */
//-------------------------------------------------------------------------------------------------
void W3DView::cameraModFreezeAngle()
{
	if (hasScriptedState(Scripted_Rotate))
	{
		if (m_rcInfo.trackObject) {
			m_rcInfo.target.targetObjectID = INVALID_ID;
		} else {
			m_rcInfo.angle.startAngle = m_rcInfo.angle.endAngle = m_angle; // Silly, but consistent.
		}
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		Int i;
//		Real curDistance = 0;
		for (i=0; i<m_mcwpInfo.numWaypoints; i++) {
			m_mcwpInfo.cameraAngle[i+1] = m_mcwpInfo.cameraAngle[0];
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Sets the look toward point for a camera movement. */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModLookToward(Coord3D *pLoc)
{
	if (hasScriptedState(Scripted_Rotate))
	{
		return; // Doesn't apply to rotate about a point.
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		Int i;
//		Real curDistance = 0;
		for (i=2; i<=m_mcwpInfo.numWaypoints; i++) {
			Coord3D start, mid, end;
			Real factor = 0.5;
			start = m_mcwpInfo.waypoints[i-1];
			start.x += m_mcwpInfo.waypoints[i].x;
			start.y += m_mcwpInfo.waypoints[i].y;
			start.x /= 2;
			start.y /= 2;
			mid = m_mcwpInfo.waypoints[i];
			end = m_mcwpInfo.waypoints[i];
			end.x += m_mcwpInfo.waypoints[i+1].x;
			end.y += m_mcwpInfo.waypoints[i+1].y;
			end.x /= 2;
			end.y /= 2;
			Coord3D result = start;
			result.x += factor*(end.x-start.x);
			result.y += factor*(end.y-start.y);
			result.x += (1-factor)*factor*(mid.x-end.x + mid.x-start.x);
			result.y += (1-factor)*factor*(mid.y-end.y + mid.y-start.y);
			result.z = 0;
			Vector2 dir(pLoc->x-result.x, pLoc->y-result.y);
			const Real dirLength = dir.Length();
			if (dirLength<0.1f) continue;
			Real angle = WWMath::Acos(dir.X/dirLength);
			if (dir.Y<0.0f) {
				angle = -angle;
			}
			// Default camera is rotated 90 degrees, so match.
			angle -= PI/2;
			normAngle(angle);
			m_mcwpInfo.cameraAngle[i] = angle;
		}
		// base Generals: a move of under 1 ms whose walk had not reached its end took its second "instant" step here, ending it
		// at the target facing the look-toward point (Zero Hour's walk had ended: no step). Pose only; its time ends it next step.
		if (m_camTimeline.waypointPath().total == 1.0f && m_mcwpInfo.curSegment < m_mcwpInfo.numWaypoints)
		{
			moveAlongWaypointPath(2.0f, 1.0f, 1.0f, TRUE);
			m_CameraArrivedAtWaypointOnPathFlag = false;
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Sets the look toward point for the end of a camera movement. */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModFinalMoveTo(Coord3D *pLoc)
{
	if (hasScriptedState(Scripted_Rotate))
	{
		return; // Doesn't apply to rotate about a point.
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		Int i;
		Coord3D start, delta;
		start = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
		delta.x = pLoc->x - start.x;
		delta.y = pLoc->y - start.y;
		for (i=2; i<=m_mcwpInfo.numWaypoints; i++) {
			Coord3D result = m_mcwpInfo.waypoints[i];
			result.x += delta.x;
			result.y += delta.y;
			m_mcwpInfo.waypoints[i] = result;
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Sets the look toward point for the end of a camera movement. */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModFinalLookToward(Coord3D *pLoc)
{
	if (hasScriptedState(Scripted_Rotate))
	{
		return; // Doesn't apply to rotate about a point.
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		Int i;
		Int min = m_mcwpInfo.numWaypoints-1;
		if (min<2) min=2;
//		Real curDistance = 0;
		for (i=min; i<=m_mcwpInfo.numWaypoints; i++) {
			Coord3D start, mid, end;
			Real factor = 0.5;
			start = m_mcwpInfo.waypoints[i-1];
			start.x += m_mcwpInfo.waypoints[i].x;
			start.y += m_mcwpInfo.waypoints[i].y;
			start.x /= 2;
			start.y /= 2;
			mid = m_mcwpInfo.waypoints[i];
			end = m_mcwpInfo.waypoints[i];
			end.x += m_mcwpInfo.waypoints[i+1].x;
			end.y += m_mcwpInfo.waypoints[i+1].y;
			end.x /= 2;
			end.y /= 2;
			Coord3D result = start;
			result.x += factor*(end.x-start.x);
			result.y += factor*(end.y-start.y);
			result.x += (1-factor)*factor*(mid.x-end.x + mid.x-start.x);
			result.y += (1-factor)*factor*(mid.y-end.y + mid.y-start.y);
			result.z = 0;
			Vector2 dir(pLoc->x-result.x, pLoc->y-result.y);
			const Real dirLength = dir.Length();
			if (dirLength<0.1f) continue;
			Real angle = WWMath::Acos(dir.X/dirLength);
			if (dir.Y<0.0f) {
				angle = -angle;
			}
			// Default camera is rotated 90 degrees, so match.
			angle -= PI/2;
			normAngle(angle);
			if (i==m_mcwpInfo.numWaypoints) {
				m_mcwpInfo.cameraAngle[i] = angle;
			} else {
				Real deltaAngle = angle - m_mcwpInfo.cameraAngle[i];
				normAngle(deltaAngle);
				angle = m_mcwpInfo.cameraAngle[i] + deltaAngle/2;
				normAngle(angle);
				m_mcwpInfo.cameraAngle[i] = angle;
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Sets the final time multiplier for a camera movement. */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModFinalTimeMultiplier(Int finalMultiplier)
{
	// GeneralsX: The multiplier a rotation or a path ends at (else the multiplier now): m_camTimeline's.
	// A path's used to be blended per waypoint by distance, the first segment running from the camera; it follows the path's
	// eased time now, as the simulation's does.
	m_camTimeline.finalTimeMultiplier(finalMultiplier);
}

// ------------------------------------------------------------------------------------------------
/** Sets the number of frames to average motion for a camera movement */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModRollingAverage(Int framesToAverage)
{
	if (framesToAverage < 1) framesToAverage = 1;
	m_mcwpInfo.rollingAverageFrames = framesToAverage;
}

// ------------------------------------------------------------------------------------------------
/** Sets the final pitch for a camera movement. */
// ------------------------------------------------------------------------------------------------
void W3DView::cameraModFinalPitch(Real finalPitch, Real easeIn, Real easeOut)
{
	if (hasScriptedState(Scripted_Rotate))
	{
		const Int time = m_camTimeline.rotationMsLeft();	// saturated Int ms, as the timeline takes them
		pitchCamera( finalPitch, time, time*easeIn, time*easeOut );
	}
	if (hasScriptedState(Scripted_MoveOnWaypointPath))
	{
		const Int time = m_camTimeline.pathMsLeft();
		pitchCamera( finalPitch, time, time*easeIn, time*easeOut );
	}
}

// ------------------------------------------------------------------------------------------------
/** Move camera to a waypoint, resetting the default angle, pitch & zoom along the way.. */
// ------------------------------------------------------------------------------------------------
void W3DView::resetCamera(const Coord3D *location, Int milliseconds, Real easeIn, Real easeOut)
{
	moveCameraTo(location, milliseconds, 0, false, easeIn, easeOut);
	m_mcwpInfo.cameraAngle[2] = 0.0f; // default angle.
	// m_mcwpInfo.cameraAngle[2] = m_defaultAngle;
	View::setAngle(m_mcwpInfo.cameraAngle[0]);

	zoomCamera( getMaxZoom(location->x, location->y), milliseconds, easeIn, easeOut );

	pitchCamera( 1.0f, milliseconds, easeIn, easeOut );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool W3DView::isCameraMovementFinished()
{
	// Several of the motion blur effects are similar to camera movements.
	// GeneralsX: ...and answer for their LOGIC lifetime. This is the VIEW's answer (W3DDisplay::draw's
	// freeze loop, the replay camera); scripts read the simulation's, TheScriptedCamera (GameLogic/ScriptedCamera.h).
	return m_camTimeline.isFinished();
}


Bool W3DView::isCameraMovementAtWaypointAlongPath()
{
	Bool returnValue = m_CameraArrivedAtWaypointOnPathFlag;
	m_CameraArrivedAtWaypointOnPathFlag = false;
	return returnValue;
}

// ------------------------------------------------------------------------------------------------
/** Move camera along a waypoint path in an interesting fashion.  Sets up parameters that get
 * evaluated in draw(). */
 // ------------------------------------------------------------------------------------------------
void W3DView::moveCameraAlongWaypointPath(Waypoint *pWay, Int milliseconds, Int shutter, Bool orient, Real easeIn, Real easeOut)
{
	const Real MIN_DELTA = MAP_XY_FACTOR;

	m_mcwpInfo.waypoints[0] = getPosition();
	m_mcwpInfo.cameraAngle[0] = getAngle();
	m_mcwpInfo.waySegLength[0] = 0;
	m_mcwpInfo.waypoints[1] = getPosition();
	m_mcwpInfo.numWaypoints = 1;
	m_mcwpInfo.shutter = shutter/TheW3DFrameLengthInMsec;
	if (m_mcwpInfo.shutter<1) m_mcwpInfo.shutter = 1;
	// GeneralsX: Its time and multiplier are m_camTimeline's, and it runs its time even when the drops
	// below leave one waypoint (the camera stood on it): that peer's camera used to decide whether the path started.
	m_camTimeline.path(milliseconds, m_mcwpInfo.shutter, easeIn, easeOut);

	while (pWay && m_mcwpInfo.numWaypoints <MAX_WAYPOINTS) {
		m_mcwpInfo.numWaypoints++;
		m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints] = *pWay->getLocation();
		if (pWay->getNumLinks()>0) {
			pWay = pWay->getLink(0);
		} else {
			pWay = nullptr;
		}
		Vector2 dir(m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints].x-m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints-1].x, m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints].y-m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints-1].y);
		if (dir.Length()<MIN_DELTA) {
			if (pWay) {
				m_mcwpInfo.numWaypoints--; // drop this one.
			} else {
				m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints-1] = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
				m_mcwpInfo.numWaypoints--; // Push this one back.
			}
		}
	}
	setupWaypointPath(orient);
}

// ------------------------------------------------------------------------------------------------
/** Calculates angles and distances for moving along a waypoint path.  Sets up parameters that get
 * evaluated in draw(). */
// ------------------------------------------------------------------------------------------------
void W3DView::setupWaypointPath(Bool orient)
{
	m_mcwpInfo.curSegment = 1;
	m_mcwpInfo.curSegDistance = 0;
	m_mcwpInfo.totalDistance = 0;
	m_mcwpInfo.rollingAverageFrames = 1;
	Int i;
	Real angle = getAngle();
	for (i=1; i<m_mcwpInfo.numWaypoints; i++) {
		Vector2 dir(m_mcwpInfo.waypoints[i+1].x-m_mcwpInfo.waypoints[i].x, m_mcwpInfo.waypoints[i+1].y-m_mcwpInfo.waypoints[i].y);
		const Real dirLength = dir.Length();
		m_mcwpInfo.waySegLength[i] = dirLength;
		m_mcwpInfo.totalDistance += m_mcwpInfo.waySegLength[i];
		if (orient && dirLength >= 0.1f) {
			angle = WWMath::Acos(dir.X/dirLength);
			if (dir.Y<0.0f) {
				angle = -angle;
			}
			// Default camera is rotated 90 degrees, so match.
			angle -= PI/2;
			normAngle(angle);
		}
		//DEBUG_LOG(("Original Index %d, angle %.2f", i, angle*180/PI));
		m_mcwpInfo.cameraAngle[i] = angle;
	}
	m_mcwpInfo.cameraAngle[1] = getAngle();
	m_mcwpInfo.cameraAngle[m_mcwpInfo.numWaypoints] = m_mcwpInfo.cameraAngle[m_mcwpInfo.numWaypoints-1];
	for (i=m_mcwpInfo.numWaypoints-1; i>1; i--) {
		m_mcwpInfo.cameraAngle[i] = (m_mcwpInfo.cameraAngle[i] + m_mcwpInfo.cameraAngle[i-1]) / 2;
	}
	m_mcwpInfo.waySegLength[m_mcwpInfo.numWaypoints+1] = m_mcwpInfo.waySegLength[m_mcwpInfo.numWaypoints];

	// Prevent a possible divide by zero.
	if (m_mcwpInfo.totalDistance<1.0) {
		m_mcwpInfo.waySegLength[m_mcwpInfo.numWaypoints-1] += 1.0-m_mcwpInfo.totalDistance;
		m_mcwpInfo.totalDistance = 1.0;
	}

	Real curDistance = 0;
	Coord3D finalPos = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
	Real newGround = TheTerrainLogic->getGroundHeight(finalPos.x, finalPos.y);
	for (i=0; i<=m_mcwpInfo.numWaypoints+1; i++) {
		Real factor2 = curDistance / m_mcwpInfo.totalDistance;
		Real factor1 = 1.0-factor2;
		m_mcwpInfo.waypoints[i].z = m_pos.z*factor1 + newGround*factor2;
		curDistance += m_mcwpInfo.waySegLength[i];
		//DEBUG_LOG(("New Index %d, angle %.2f", i, m_mcwpInfo.cameraAngle[i]*180/PI));
	}

	// Pad the end.
	m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints+1] = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
	Coord3D cur = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
	Coord3D prev = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints-1];
	m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints+1].x += cur.x-prev.x;
	m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints+1].y += cur.y-prev.y;
	m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints+1].z = newGround;
	m_mcwpInfo.cameraAngle[m_mcwpInfo.numWaypoints+1] = m_mcwpInfo.cameraAngle[m_mcwpInfo.numWaypoints];

	cur = m_mcwpInfo.waypoints[2];
	prev = m_mcwpInfo.waypoints[1];
	m_mcwpInfo.waypoints[0].x -= cur.x-prev.x;
	m_mcwpInfo.waypoints[0].y -= cur.y-prev.y;

	// GeneralsX: m_camTimeline started the path (and ended a rotation) with its time; a path left with
	// one waypoint runs its time without moving (moveAlongWaypointPath)
	m_CameraArrivedAtWaypointOnPathFlag = false;
	m_mcwpInfo.curShutter = m_mcwpInfo.shutter;
	setUserControlled(false);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Real makeQuadraticS(Real t)
{
	// for t = linear 0-1, convert to quadratic s where 0==0, 0.5==0.5 && 1.0 == 1.0.
	Real tPrime = t;
	if (t<0.5) {
		tPrime = 0.5 * (2*t*2*t);
	} else {
		tPrime = (t-0.5)*2;
		tPrime = WWMath::Sqrt(tPrime);
		tPrime = 0.5 + 0.5*(tPrime);
	}
	return tPrime*0.5 + t*0.5;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::rotateCameraOneFrame(Bool ended)
{
	// GeneralsX: The pose of a rotation step: m_camTimeline counted the frame, set the time multiplier
	// (from its own clock) and decided the end, which also clears the camera freeze.
	const ScriptedCameraTimeline::Counter &rc = m_camTimeline.rotation();
	if (TheGlobalData->m_disableCameraMovement) {
		return;
	}

	if (m_rcInfo.trackObject)
	{
		if (rc.cur <= rc.num + rc.hold)
		{
			const Object *obj = TheGameLogic->findObjectByID(m_rcInfo.target.targetObjectID);
			if (obj)
			{
				// object has not been destroyed
				m_rcInfo.target.targetObjectPos = *obj->getPosition();
			}

			const Vector2 dir(m_rcInfo.target.targetObjectPos.x - m_pos.x, m_rcInfo.target.targetObjectPos.y - m_pos.y);
			const Real dirLength = dir.Length();
			if (dirLength>=0.1f)
			{
				Real angle = WWMath::Acos(dir.X/dirLength);
				if (dir.Y<0.0f) {
					angle = -angle;
				}
				// Default camera is rotated 90 degrees, so match.
				angle -= PI/2;
				normAngle(angle);

				if (rc.cur <= rc.num)
				{
					Real factor = rc.ease(((Real)rc.cur)/rc.num);
					Real angleDiff = angle - m_angle;
					normAngle(angleDiff);
					angleDiff *= factor;
					View::setAngle(m_angle + angleDiff);
				}
				else
				{
					View::setAngle(angle);
				}
			}
		}
	}
	else if (rc.cur <= rc.num)
	{
		Real factor = rc.ease(((Real)rc.cur)/rc.num);
		Real angle = WWMath::Lerp(m_rcInfo.angle.startAngle, m_rcInfo.angle.endAngle, factor);
		View::setAngle(angle);
	}

	if (ended && !m_rcInfo.trackObject)
	{
		View::setAngle(m_rcInfo.angle.endAngle);
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::zoomCameraOneFrame(Bool ended)
{
	const ScriptedCameraTimeline::Counter &zc = m_camTimeline.zooming();	// GeneralsX: counted and ended by m_camTimeline
	if (TheGlobalData->m_disableCameraMovement) {
		return;
	}
	if (zc.cur <= zc.num)
	{
		// not just holding; do the camera adjustment
		Real factor = m_zcInfo.ease(((Real)zc.cur)/zc.num);
		m_zoom = WWMath::Lerp(m_zcInfo.startZoom, m_zcInfo.endZoom, factor);
	}

	if (ended) {
		m_zoom = m_zcInfo.endZoom;
	}

	//DEBUG_LOG(("W3DView::zoomCameraOneFrame() - m_zoom = %g", m_zoom));
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::pitchCameraOneFrame(Bool ended)
{
	const ScriptedCameraTimeline::Counter &pc = m_camTimeline.pitching();	// GeneralsX: counted and ended by m_camTimeline
	if (TheGlobalData->m_disableCameraMovement) {
		return;
	}
	if (pc.cur <= pc.num)
	{
		// not just holding; do the camera adjustment
		Real factor = m_pcInfo.ease(((Real)pc.cur)/pc.num);
		m_FXPitch = WWMath::Lerp(m_pcInfo.startPitch, m_pcInfo.endPitch, factor);
	}

	if (ended) {
		m_FXPitch = m_pcInfo.endPitch;
	}
}

//-------------------------------------------------------------------------------------------------
void W3DView::setUserControlled(Bool value)
{
	if (m_isUserControlled != value)
	{
		m_isUserControlled = value;
#if PRESERVE_RETAIL_SCRIPTED_CAMERA
		m_zoom = getDesiredZoom(m_pos.x, m_pos.y);
#endif
	}
}

// ------------------------------------------------------------------------------------------------
Bool W3DView::isDoingScriptedCamera()
{
	return m_camTimeline.moves() != 0 || m_scriptedCameraLock;
}

// ------------------------------------------------------------------------------------------------
void W3DView::stopDoingScriptedCamera()
{
	if (m_camTimeline.moves() != 0)
		gxCamTestWhoEnds("stopDoingScriptedCamera");
	m_camTimeline.stopAll();	// the freeze stays for the next move, as before
	m_scriptedCameraLock = FALSE;
}

// ------------------------------------------------------------------------------------------------
Bool W3DView::hasScriptedState(ScriptedState state) const
{
	const ScriptedState now = m_camTimeline.moves() | (m_scriptedCameraLock ? (ScriptedState)Scripted_CameraLock : 0);
	return (now & state) != 0;
}

// ------------------------------------------------------------------------------------------------
void W3DView::addScriptedState(ScriptedState state)
{
	// GeneralsX: only the lock is added here; a move starts on m_camTimeline from its setup function
	if (state & Scripted_CameraLock)
		m_scriptedCameraLock = TRUE;
	setUserControlled(false);
}

// ------------------------------------------------------------------------------------------------
void W3DView::removeScriptedState(ScriptedState state)
{
	if (m_camTimeline.moves() & state & (Scripted_Rotate | Scripted_MoveOnWaypointPath))
		gxCamTestWhoEnds("removeScriptedState");
	m_camTimeline.end(state & (Scripted_Rotate | Scripted_Pitch | Scripted_Zoom | Scripted_MoveOnWaypointPath));
	if (state & Scripted_CameraLock)
		m_scriptedCameraLock = FALSE;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void W3DView::moveAlongWaypointPath(Real elapsed, Real totalTime, Real milliseconds, Bool ended)
{
	// GeneralsX: The pose of a path step: m_camTimeline counted the time, ended the path on it (and
	// cleared the camera freeze) and set the time multiplier.
	if (TheGlobalData->m_disableCameraMovement) {
		return;
	}
	if (ended) {
		m_CameraArrivedAtWaypointOnPathFlag = false;
		View::setAngle(m_mcwpInfo.cameraAngle[m_mcwpInfo.numWaypoints]);

		Coord3D pos = m_mcwpInfo.waypoints[m_mcwpInfo.numWaypoints];
		setPosition(pos);
		// Note - assuming that the scripter knows what he is doing, we adjust the constraints so that
		// the scripted action can occur.
		m_cameraAreaConstraints.lo.x = min(m_cameraAreaConstraints.lo.x, pos.x);
		m_cameraAreaConstraints.hi.x = max(m_cameraAreaConstraints.hi.x, pos.x);
		m_cameraAreaConstraints.lo.y = min(m_cameraAreaConstraints.lo.y, pos.y);
		m_cameraAreaConstraints.hi.y = max(m_cameraAreaConstraints.hi.y, pos.y);
		return;
	}
	// One waypoint left (the camera stood on the only one), or the eased distance already reached the last waypoint - this
	// used to zero the time and end the path on the next step, which its time does on that same step
	// (scripted-camera-clock-test): nothing to move until the end.
	if (m_mcwpInfo.numWaypoints < 2 || m_mcwpInfo.curSegment >= m_mcwpInfo.numWaypoints) {
		return;
	}

	const ScriptedCameraClock::Ease &ease = m_camTimeline.waypointPath().ease;
	const Real deltaTime = ease(elapsed/totalTime) - ease((elapsed - milliseconds)/totalTime);
	m_mcwpInfo.curSegDistance += deltaTime*m_mcwpInfo.totalDistance;
	// TheSuperHacker @todo Investigate which one is really correct.
	// The non Generals condition causes camera bug in Generals Shell Map.
#if RTS_GENERALS
	while (m_mcwpInfo.curSegDistance > m_mcwpInfo.waySegLength[m_mcwpInfo.curSegment])
#else
	while (m_mcwpInfo.curSegDistance >= m_mcwpInfo.waySegLength[m_mcwpInfo.curSegment])
#endif
	{
		if (hasScriptedState(Scripted_MoveOnWaypointPath))
		{
			//WWDEBUG_SAY(( "MBL TEST: Camera waypoint along path reached!" ));
			m_CameraArrivedAtWaypointOnPathFlag = true;
		}

		m_mcwpInfo.curSegDistance -= m_mcwpInfo.waySegLength[m_mcwpInfo.curSegment];
		m_mcwpInfo.curSegment++;
		if (m_mcwpInfo.curSegment >= m_mcwpInfo.numWaypoints) {
			return;	// at the last waypoint: still until the path's time ends it
		}
	}
	Real avgFactor = 1.0/m_mcwpInfo.rollingAverageFrames;
	m_mcwpInfo.curShutter--;
	if (m_mcwpInfo.curShutter>0) {
		return;
	}
	m_mcwpInfo.curShutter = m_mcwpInfo.shutter;
	Real factor = m_mcwpInfo.curSegDistance / m_mcwpInfo.waySegLength[m_mcwpInfo.curSegment];
	if (m_mcwpInfo.curSegment == m_mcwpInfo.numWaypoints-1) {
		avgFactor = avgFactor + (1.0-avgFactor)*factor;
	}
	Real factor1;
	Real factor2;
	factor1 = 1.0-factor;
	//factor1 = makeQuadraticS(factor1);
	factor2 = 1.0-factor1;
	Real angle1 = m_mcwpInfo.cameraAngle[m_mcwpInfo.curSegment];
	Real angle2 = m_mcwpInfo.cameraAngle[m_mcwpInfo.curSegment+1];
	if (angle2-angle1 > PI) angle1 += 2*PI;
	if (angle2-angle1 < -PI) angle1 -= 2*PI;
	Real angle = angle1 * (factor1) + angle2 * (factor2);

	normAngle(angle);
	Real deltaAngle = angle-m_angle;
	normAngle(deltaAngle);
	if (fabs(deltaAngle) > PI/10) {
		DEBUG_LOG(("Huh."));
	}
	View::setAngle(m_angle + (avgFactor*deltaAngle));

	Coord3D start, mid, end;
	if (factor<0.5) {
		start = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment-1];
		start.x += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment].x;
		start.y += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment].y;
		start.x /= 2;
		start.y /= 2;
		mid = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment];
		end = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment];
		end.x += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1].x;
		end.y += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1].y;
		end.x /= 2;
		end.y /= 2;
		factor += 0.5;
	} else {
		start = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment];
		start.x += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1].x;
		start.y += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1].y;
		start.x /= 2;
		start.y /= 2;
		mid = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1];
		end = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1];
		end.x += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+2].x;
		end.y += m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+2].y;
		end.x /= 2;
		end.y /= 2;
		factor -= 0.5;
	}

	Coord3D result = start;
	result.x += factor*(end.x-start.x);
	result.y += factor*(end.y-start.y);
	result.x += (1-factor)*factor*(mid.x-end.x + mid.x-start.x);
	result.y += (1-factor)*factor*(mid.y-end.y + mid.y-start.y);
	result.z = m_mcwpInfo.waypoints[m_mcwpInfo.curSegment].z*factor1 +
			m_mcwpInfo.waypoints[m_mcwpInfo.curSegment+1].z*factor2;
/*
	DEBUG_LOG(("Dx %.2f, dy %.2f, DeltaANgle = %.2f, %.2f DeltaGround %.2f", m_pos.x-result.x, m_pos.y-result.y, deltaAngle, result.z, result.z-m_pos.z));
*/
	setPosition(result);
	// Note - assuming that the scripter knows what he is doing, we adjust the constraints so that
	// the scripted action can occur.
	m_cameraAreaConstraints.lo.x = min(m_cameraAreaConstraints.lo.x, result.x);
	m_cameraAreaConstraints.hi.x = max(m_cameraAreaConstraints.hi.x, result.x);
	m_cameraAreaConstraints.lo.y = min(m_cameraAreaConstraints.lo.y, result.y);
	m_cameraAreaConstraints.hi.y = max(m_cameraAreaConstraints.hi.y, result.y);

}


// ------------------------------------------------------------------------------------------------
/** Add an impulse force to shake the camera.
 * The camera shake is a simple simulation of an oscillating spring/damper.
 * The idea is that some sort of shock has "pushed" the camera once, as an
 * impluse, after which the camera vibrates back to its rest position.
 * @todo This should be part of "View", not "W3DView". */
// ------------------------------------------------------------------------------------------------
void W3DView::shake( const Coord3D *epicenter, CameraShakeType shakeType )
{
	Real angle = GameClientRandomValueReal( 0, 2*PI );

	const Real angleCos = (Real)cos( angle );
	const Real angleSin = (Real)sin( angle );

	Real intensity = 0.0f;
	switch( shakeType )
	{
		case SHAKE_SUBTLE:
			intensity = TheGlobalData->m_shakeSubtleIntensity;
			break;

		case SHAKE_NORMAL:
			intensity = TheGlobalData->m_shakeNormalIntensity;
			break;

		case SHAKE_STRONG:
			intensity = TheGlobalData->m_shakeStrongIntensity;
			break;

		case SHAKE_SEVERE:
			intensity = TheGlobalData->m_shakeSevereIntensity;
			break;

		case SHAKE_CINE_EXTREME:
			intensity = TheGlobalData->m_shakeCineExtremeIntensity;
			break;

		case SHAKE_CINE_INSANE:
			intensity = TheGlobalData->m_shakeCineInsaneIntensity;
			break;
	}

	// intensity falls off with distance
	/// @todo make this 3D once we have the real "lookat" spot
	const Coord2D viewPos = getPosition2D();
	Coord2D d;
	d.x = epicenter->x - viewPos.x;
	d.y = epicenter->y - viewPos.y;

	Real dist = (Real)sqrt( d.x*d.x + d.y*d.y );

	const Bool inRange = !(dist > TheGlobalData->m_maxShakeRange);
	if (inRange)
		intensity *= 1.0f - (dist/TheGlobalData->m_maxShakeRange);

	// GeneralsX: render-split step 6 (GXRenderSplit.h): the shake's angle and strength are worked
	// out at the call, as before (the client-random draw stays in its place in the stream); the writes into the view's
	// shake state, which its per-frame update damps, are deferred when the tick calls - an explosion's FX or a script.
	// As before, an epicenter out of range still sets the new angle.
	auto apply = [this, angleCos, angleSin, inRange, intensity]()
	{
		GXRenderSplit &split = GXRenderSplit::get();
		if (split.traced())
		{
			const Real args[4] = { angleCos, angleSin, inRange ? 1.0f : 0.0f, intensity };
			split.notePhase( 19, true );	// [GXRSPHASE] sink 19: the view's shake
			split.traceFx( 1, args, sizeof(args) );
		}
		m_shakeAngleCos = angleCos;
		m_shakeAngleSin = angleSin;

		if (!inRange)
			return;

		// add intensity and clamp
		m_shakeIntensity += intensity;

		//const Real maxIntensity = 10.0f;
		const Real maxIntensity = 3.0f;
		if (m_shakeIntensity > TheGlobalData->m_maxShakeIntensity)
			m_shakeIntensity = maxIntensity;
	};
	GXRenderSplit &split = GXRenderSplit::get();
	if (split.simSide())
		split.defer( GXRenderSplit::Fx, 0, apply );	// no replayed body here reads the logic frame
	else
		apply();
}

//-------------------------------------------------------------------------------------------------
/** Transform the screen pixel coord passed in, to a world coordinate at the specified z value */
// TheSuperHackers @fix Now returns whether a Z plane intersection exists to let callers handle the
// failure condition.
//-------------------------------------------------------------------------------------------------
PlaneClass::IntersectionResType W3DView::screenToWorldAtZ( const ICoord2D *screen, Coord3D *world, Real z )
{
	Vector3 rayStart, rayEnd;

	getPickRay( screen, &rayStart, &rayEnd );

	PlaneClass plane;
	plane.N = Vector3(0, 0, 1);
	plane.D = z;
	float t;
	PlaneClass::IntersectionResType intersectionType = plane.Compute_Intersection(rayStart, rayEnd, &t);

	if (intersectionType != PlaneClass::NO_INTERSECTION)
	{
		Vector3 intersectPos;
		intersectPos = rayStart + (rayEnd-rayStart) * t;
		world->x = intersectPos.X;
		world->y = intersectPos.Y;
		world->z = z;
	}

	return intersectionType;
}

void W3DView::cameraEnableSlaveMode(const AsciiString & objectName, const AsciiString & boneName)
{
	m_isCameraSlaved = true;
	m_cameraSlaveObjectName = objectName;
	m_cameraSlaveObjectBoneName = boneName;
}

void W3DView::cameraDisableSlaveMode()
{
	m_isCameraSlaved = false;
}

void W3DView::cameraEnableRealZoomMode() //WST added 10/18/2002
{
	m_useRealZoomCam = true;
	m_FXPitch = 1.0f;	//Reset to default
	//m_zoom = 1.0f;
	updateView();
}

void W3DView::cameraDisableRealZoomMode() //WST added 10/18/2002
{
	m_useRealZoomCam = false;
	m_FXPitch = 1.0f;	//Reset to default
	//m_zoom = 1.0f;
	m_FOV = DEG_TO_RADF(50.0f);
	m_recalcCamera = true;
	updateView();
}

void W3DView::Add_Camera_Shake (const Coord3D & position,float radius,float duration,float power) //WST added 11/13/02
{
	Vector3 vpos;

	vpos.X = position.x;
	vpos.Y = position.y;
	vpos.Z = position.z;


	CameraShakerSystem.Add_Camera_Shake(vpos,radius,duration,power);
}

bool W3DView::getDesiredTerrainDrawSize(ICoord2D &dimensions) const
{
	if (TheGlobalData && TheGlobalData->m_drawEntireTerrain)
	{
		DEBUG_ASSERTCRASH(TheTerrainRenderObject != nullptr, ("TheTerrainRenderObject is null"));

		if (const WorldHeightMap *heightMap = TheTerrainRenderObject->getMap())
		{
			dimensions.x = heightMap->getXExtent();
			dimensions.y = heightMap->getYExtent();
			return true;
		}

		return false;
	}

	const Real cameraPitch = asin(fabs(m_3DCamera->Get_Forward_Dir().Z));

	if (cameraPitch > ViewDefaultLowPitchRadians || !m_isUserControlled)
	{
		// TheSuperHackers @info The scripted camera always uses the regular draw sizes
		// and uses terrain oversize if it needs to enlarge.
		dimensions.x = WorldHeightMap::NORMAL_DRAW_WIDTH;
		dimensions.y = WorldHeightMap::NORMAL_DRAW_HEIGHT;
	}
	else
	{
		// TheSuperHackers @tweak xezon 31/12/2025 Increases visible terrain area when lowering the camera pitch.
		// Note: The default camera pitch in Generals was 37.5, which we prefer to keep the normal draw size for.
		dimensions.x = WorldHeightMap::LOW_ANGLE_DRAW_WIDTH;
		dimensions.y = WorldHeightMap::LOW_ANGLE_DRAW_HEIGHT;
	}

	// GeneralsX: TerrainDrawDistanceScale widens the tuned draw window.
	const Real terrainScale = TheGlobalData != nullptr ? TheGlobalData->m_terrainDrawDistanceScale : 1.0f;
	if (terrainScale != 1.0f)
	{
		dimensions.x = static_cast<Int>(dimensions.x * terrainScale);
		dimensions.y = static_cast<Int>(dimensions.y * terrainScale);
	}

	// GeneralsX: scale the terrain draw window with camera HEIGHT. Zoom leaves pitch unchanged, so the window was a fixed
	// square (129 cells; 135 at the default draw scale) and zoomed-out views rendered black beyond it.
	// Snapped to the renderer's 1 + 32k tiling; the ratio is clamped at 1.0, so normal play is unchanged.
	// Sized for the MAX height too, so the zoom-out range is drawn at once instead of rebuilding the terrain per step.
	const Real sizingHeight = m_isUserControlled ? std::max(m_currentHeightAboveGround, m_maxHeightAboveGround) : m_currentHeightAboveGround;
	const Real heightRatio = sizingHeight / ViewDefaultMaxHeightAboveTerrain;
	if (heightRatio > 1.0f)
	{
		const Int tileLen = VERTEX_BUFFER_TILE_LENGTH;	// a #define in WorldHeightMap.h, not a member
		const Int tilesW = static_cast<Int>(ceilf((dimensions.x - 1) * heightRatio / (Real)tileLen));
		const Int tilesH = static_cast<Int>(ceilf((dimensions.y - 1) * heightRatio / (Real)tileLen));
		dimensions.x = 1 + tilesW * tileLen;
		dimensions.y = 1 + tilesH * tileLen;
	}

	return true;
}

void W3DView::updateTerrain()
{
	DEBUG_ASSERTCRASH(TheTerrainRenderObject != nullptr, ("TheTerrainRenderObject is null"));

	ICoord2D drawSize;

	if (getDesiredTerrainDrawSize(drawSize))
	{
		// GeneralsX: grow at once, never shrink within a game (reset() clears it); each size change rebuilds the whole terrain
		// (setTerrainDrawSize); ground wobble near max zoom flipped the 225 step 76 times in 6 minutes of scrolling.
		// A match starts at max zoom with the largest window already drawn, so keeping it draws no extra terrain.
		// A live settings change (TerrainDrawDistanceScale, DrawEntireTerrain) or a lower camera max restarts the window.
		const Real drawScale = TheGlobalData != nullptr ? TheGlobalData->m_terrainDrawDistanceScale : 1.0f;
		const Bool drawEntire = TheGlobalData != nullptr && TheGlobalData->m_drawEntireTerrain;
		if (drawScale != m_terrainDrawScale || drawEntire != m_terrainDrawEntire || m_maxHeightAboveGround < m_terrainDrawMaxHeight)
		{
			m_terrainDrawSize.x = 0;
			m_terrainDrawSize.y = 0;
			m_terrainDrawScale = drawScale;
			m_terrainDrawEntire = drawEntire;
		}
		m_terrainDrawMaxHeight = m_maxHeightAboveGround;
		if (drawSize.x < m_terrainDrawSize.x)
			drawSize.x = m_terrainDrawSize.x;
		if (drawSize.y < m_terrainDrawSize.y)
			drawSize.y = m_terrainDrawSize.y;
		m_terrainDrawSize = drawSize;
		TheTerrainRenderObject->setTerrainDrawSize(drawSize.x, drawSize.y);
	}

	// GeneralsX: Iterator on the stack instead of NEW/delete on every call.
	// Upstream reference: Mauller, PR #2646
	// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2646
	RefRenderObjListClass* lightlist = W3DDisplay::m_3DScene->getLightList();
	RefRenderObjListIterator it(lightlist);

	const Vector3 cameraPivot(m_pos.x, m_pos.y, m_pos.z);
	// S7 (GXShadowMapHooks.h): a full rebuild through ANY caller (update(), or a camera move such as a replay's
	// camera command, which rebuilds and clears m_needFullUpdate before update() runs) re-measures the shadow fit's
	// terrain height range
	if (TheTerrainRenderObject->doesNeedFullUpdate())
		GxShadowMapTerrainChanged = true;
	TheTerrainRenderObject->updateCenter(m_3DCamera, &cameraPivot, &it);
}
