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

// View.cpp ///////////////////////////////////////////////////////////////////
// A "view", or window, into the World
// Author: Michael S. Booth, February 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/Xfer.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GXHostCamera.h"
#include "GameClient/View.h"

UnsignedInt View::m_idNext = 1;

// the tactical view singleton
View *TheTacticalView = nullptr;


View::View()
{
	m_userControlLockedUntilFrame = 0u;
	m_isUserControlled = true;
	m_currentHeightAboveGround = 0.0f;
	m_defaultAngle = 0.0f;
	m_defaultPitch = 0.0f;
	m_heightAboveGround = 0.0f;
	m_lockDist = 0.0f;
	m_maxHeightAboveGround = 0.0f;
	m_minHeightAboveGround = 0.0f;
	m_next = nullptr;
	m_okToAdjustHeight = TRUE;
	m_originX = 0;
	m_originY = 0;
	m_snapImmediate = FALSE;
	m_terrainHeightAtPivot = 0.0f;
	m_zoom = 0.0f;
	m_pos.zero();
	m_width = 0;
	m_height = 0;
	m_angle = 0.0f;
	m_pitch = 0.0f;
	m_cameraLock = INVALID_ID;
	m_cameraLockDrawable = nullptr;
	m_zoomLimited = TRUE;

	// create unique view ID
	m_id = m_idNext++;

	// default field of view
	m_FOV = DEG_TO_RADF(50.0f);

	m_mouseLocked = FALSE;

	m_guardBandBias.x = 0.0f;
	m_guardBandBias.y = 0.0f;
}

View::~View()
{
}

void View::init()
{
	m_width = DEFAULT_VIEW_WIDTH;
	m_height = DEFAULT_VIEW_HEIGHT;
	m_originX = DEFAULT_VIEW_ORIGIN_X;
	m_originY = DEFAULT_VIEW_ORIGIN_Y;
	m_pos.zero();
	m_angle = 0.0f;
	m_cameraLock = INVALID_ID;
	m_cameraLockDrawable = nullptr;
	m_zoomLimited = TRUE;

	m_zoom = 1.0f;

	// GeneralsX: Options.ini camera limits apply at startup, not only
	// through the Extras menu.
	//
	// The launcher and the Extras menu both write MinCameraHeight/MaxCameraHeight into
	// Options.ini, and OptionPreferences has parsed the keys all along - but nothing on the
	// startup path ever read them back. The GlobalData startup override block applies its other
	// Options.ini keys and skips these two, so the only consumer was the Extras menu, which
	// applies them on Accept. A launcher-written camera limit therefore did nothing until the
	// player opened Extras.
	//
	// Applied HERE, not in GlobalData.cpp, on purpose: GlobalData.cpp is Common/, inside the
	// SimID source digest, and a purely client-side camera limit must not cost every peer a wire
	// break. GameClient/ is excluded from the digest - the same reasoning that put the HUD anchor
	// preference in GXHud.ini rather than through GlobalData/Options (see the GXHud note in
	// GeneralsMD InGameUI.cpp). This file is also shared by both game twins, and both consult
	// TheGlobalData camera limits the same way, so the override is twin-safe.
	//
	// Presence is tested on the map itself because the getters return the TheGlobalData value
	// when the key is absent - indistinguishable from "player chose the default". Absent keys
	// change nothing: a player who never touched the launcher or Extras sees exactly today's
	// behaviour. If the two resolved values would invert (min > max), both are left at the data
	// defaults - applying either half alone would manufacture a limit range the settings UI
	// itself cannot express.
	Real cameraPitch;
	{
		OptionPreferences prefs;
		const Bool hasMin = prefs.find("MinCameraHeight") != prefs.end();
		const Bool hasMax = prefs.find("MaxCameraHeight") != prefs.end();
		if (hasMin || hasMax)
		{
			const Real prefMinHeight = getPrefMinCameraHeight(prefs);	// clamped 50..300 (Debug menu: 10..300) when present
			const Real prefMaxHeight = getPrefMaxCameraHeight(prefs);	// clamped 100..1000 (Debug menu: 100..3000) when present
			if (prefMinHeight <= prefMaxHeight)
			{
				if (hasMin)
					TheWritableGlobalData->m_minCameraHeight = prefMinHeight;
				if (hasMax)
					TheWritableGlobalData->m_maxCameraHeight = prefMaxHeight;
			}
		}

		// GeneralsX: Options.ini's TerrainDrawDistanceScale applies at startup
		// too. The Extras and Debug menus write it and apply it live, but only SagePatch.ini fed it at
		// startup, so a player's draw distance reverted on every launch. Same placement and reasoning as
		// the camera limits above; like them, a present key wins over SagePatch.ini.
		//
		// Only a value the menus can write, 1.00-2.00, is taken; anything else leaves SagePatch.ini's.
		// Until 25/09/2026 the Extras menu wrote the slider PERCENT ("105"), which the pref getter clamps
		// to 2.0 - so a reopened Extras showed 200 and the next Accept saved "200", a maximum nobody
		// chose. A percent therefore cannot be trusted and is ignored, which is exactly how startup
		// treated it before; the fixed menu's next Accept writes the real. The range test is also false
		// for a NaN, which clamp() would pass to the renderer. Not through the getter, whose clamp would
		// turn that percent into 2.0; fixing the getter would move the SimID digest (it is Common/).
		TheWritableGlobalData->m_terrainDrawDistanceScale = getPrefTerrainDrawDistanceScale(prefs);

		// GeneralsX: A camera pitch the player chose in Extras or Debug applies at
		// startup too; until now only the live view took it, so it reverted on every launch. What counts
		// as a choice is getPrefCameraPitch's call. Unlike the draw distance, TheGlobalData's pitch is NOT
		// overwritten: it stays GameData.ini's value, because that is what "no choice" resolves to - here,
		// in both menus and for their Defaults - and the view's own default pitch is the only consumer.
		cameraPitch = getPrefCameraPitch(prefs);
	}

	// GeneralsX: Routed through GXHostCamera so the HOST's limits govern
	// every peer in a networked match. Outside a networked match - which includes right here, since
	// View::init runs at engine startup - the accessor returns TheGlobalData's pair unchanged, so
	// this is the same read it has always been. See GXHostCamera.h.
	const GXHostCamera::Limits camLimits = GXHostCamera::effectiveLimits();
	m_maxHeightAboveGround = camLimits.maxHeight;
	m_minHeightAboveGround = camLimits.minHeight;
	m_okToAdjustHeight = FALSE;

	m_defaultAngle = DEG_TO_RADF(TheGlobalData->m_cameraYaw);
	m_defaultPitch = DEG_TO_RADF(cameraPitch);
	m_angle = m_defaultAngle;
	m_pitch = m_defaultPitch;
}

// Only a pitch the player CHOSE is taken; otherwise TheGlobalData's (GameData.ini's 37.5), exactly what startup
// always used. Until 25/09/2026 both menus saved the pitch slider on EVERY Accept, and the slider opened at the
// truncated default, so an Accept that never touched it wrote "37" - "%d" of (Int)37.5. That is not a choice:
// taking it would drop every such player to 37 degrees, which is not above ViewDefaultLowPitchRadians, so it
// would also switch their terrain to the larger LOW_ANGLE draw window - a perf change nobody asked for. So
// exactly that string is ignored - derived from the active default, since that is what the old menus truncated
// - and setPrefCameraPitch never writes it (a chosen 37 is stored "37.0"). Any
// other legacy whole number was moved to on purpose and is kept. Outside the sliders' 20-60, or NaN (the range
// test is false for it), is ignored like a bad draw distance. Not through OptionPreferences::getCameraPitch,
// which cannot tell the legacy value from a choice; changing it would move the SimID digest (it is Common/).
Real View::getPrefCameraPitch( const OptionPreferences &prefs )
{
	OptionPreferences::const_iterator it = prefs.find("CameraPitch");
	if (it != prefs.end())
	{
		AsciiString untouched;
		untouched.format("%d", (Int)TheGlobalData->m_cameraPitch);
		const Real pitch = (Real)atof(it->second.str());
		if (it->second != untouched && pitch >= 20.0f && pitch <= 60.0f)
			return pitch;
	}
	return TheGlobalData->m_cameraPitch;
}

// The one writer: Accept in Extras, Camera Apply in Debug. Both menus call it only for a slider the player MOVED
// (they watch GSM_SLIDER_TRACK), so an untouched slider writes nothing and whatever the file held - a choice, or
// nothing - stands; its position alone cannot say, since the default 37.5 and a chosen 38 both open at 38.
// A choice is written with a decimal so a chosen 37 can never read as the legacy "37". Defaults removes the
// key, so the next start uses TheGlobalData's pitch itself rather than a rounded copy of it.
void View::setPrefCameraPitch( OptionPreferences &prefs, Int sliderDegrees )
{
	AsciiString chosen;
	chosen.format("%d.0", sliderDegrees);
	prefs["CameraPitch"] = chosen;
}

void View::clearPrefCameraPitch( OptionPreferences &prefs )
{
	prefs.erase("CameraPitch");
}

// GeneralsX: Two caps on one key (owner): the launcher's camera range is the player's -
// MaxCameraHeight 100 .. 1000, Generals Online's own lobby cap - while the Debug menu goes down to 10 and up to 3000.
// Both write MinCameraHeight / MaxCameraHeight, so the Debug menu also marks the pair when it leaves the player's
// range; without that mark the player's range clamps, so a MaxCameraHeight above 1000 from the launcher or a hand
// edit plays at 1000, exactly as OptionPreferences' getter always clamped it. Clamped the way that getter clamps (an
// out-of-range value is pinned, not ignored), and a NaN pins to the floor instead of reaching the view.
static const char *const s_debugCameraHeightsKey = "DebugCameraHeights";

static Real clampPrefCameraHeight( const OptionPreferences &prefs, const char *key, Real fallback, Real lo, Real hi )
{
	OptionPreferences::const_iterator it = prefs.find(key);
	if (it == prefs.end())
		return fallback;
	const Real val = (Real)atof(it->second.str());
	if (!(val >= lo))
		return lo;
	return val > hi ? hi : val;
}

Bool View::getPrefDebugCameraHeights( const OptionPreferences &prefs )
{
	OptionPreferences::const_iterator it = prefs.find(s_debugCameraHeightsKey);
	if (it == prefs.end() || it->second.compareNoCase("yes") != 0)
		return FALSE;
	// Only while a height really is outside the player's range: once the launcher or the Config tab has brought both
	// back inside it, the pair is an ordinary one again whatever a stale mark says. Inside the
	// range both clamps give the same value, so this changes nothing for the readers below. NaN is never outside.
	OptionPreferences::const_iterator mx = prefs.find("MaxCameraHeight");
	OptionPreferences::const_iterator mn = prefs.find("MinCameraHeight");
	return (mx != prefs.end() && (Real)atof(mx->second.str()) > (Real)CameraMaxHeightHi)
		|| (mn != prefs.end() && (Real)atof(mn->second.str()) < (Real)CameraMinHeightLo);
}

void View::setPrefDebugCameraHeights( OptionPreferences &prefs, Real minHeight, Real maxHeight )
{
	if (minHeight < (Real)CameraMinHeightLo || maxHeight > (Real)CameraMaxHeightHi)
		prefs[s_debugCameraHeightsKey] = "yes";
	else
		prefs.erase(s_debugCameraHeightsKey);
}

Real View::getPrefMaxCameraHeight( const OptionPreferences &prefs )
{
	const Real hi = getPrefDebugCameraHeights(prefs) ? (Real)DebugCameraMaxHeightHi : (Real)CameraMaxHeightHi;
	return clampPrefCameraHeight(prefs, "MaxCameraHeight", TheGlobalData->m_maxCameraHeight, (Real)CameraMaxHeightLo, hi);
}

Real View::getPrefMinCameraHeight( const OptionPreferences &prefs )
{
	const Real lo = getPrefDebugCameraHeights(prefs) ? (Real)DebugCameraMinHeightLo : (Real)CameraMinHeightLo;
	return clampPrefCameraHeight(prefs, "MinCameraHeight", TheGlobalData->m_minCameraHeight, lo, (Real)CameraMinHeightHi);
}

Real View::getPrefTerrainDrawDistanceScale( const OptionPreferences &prefs )
{
	OptionPreferences::const_iterator it = prefs.find("TerrainDrawDistanceScale");
	if (it != prefs.end())
	{
		const Real scale = (Real)atof(it->second.str());
		if (scale >= 1.0f && scale <= 2.0f)
			return scale;
	}
	return TheGlobalData->m_terrainDrawDistanceScale;
}

void View::reset()
{
	// Only fixing the reported bug.  Who knows what side effects resetting the rest could have.
	m_zoomLimited = TRUE;

	m_userControlLockedUntilFrame = 0u;
	m_isUserControlled = true;
}

/**
 * Prepend this view to the given list, return the new list.
 */
View *View::prependViewToList( View *list )
{
	m_next = list;
	return this;
}

void View::zoom( Real height )
{
	setHeightAboveGround(getHeightAboveGround() + height);
}

void View::setZoomToMax()
{
	setHeightAboveGround(getHeightAboveGround() + m_maxHeightAboveGround);
}

/**
 * Center the view on the given coordinate.
 */
void View::lookAt( const Coord3D *o )
{
	/// @todo this needs to be changed to be 3D, this is still old 2D stuff
	Coord2D pos = getPosition2D();
	pos.x = o->x - m_width * 0.5f;
	pos.y = o->y - m_height * 0.5f;
	setPosition2D(pos);
}

/**
 * Shift the view by the given delta.
 */
void View::scrollBy( const Coord2D *delta )
{
	// update view's world position
	m_pos.x += delta->x;
	m_pos.y += delta->y;
}

/**
 * Rotate the view around the vertical axis to the given angle.
 */
void View::setAngle( Real radians )
{
	m_angle = WWMath::Normalize_Angle(radians);
}

#define CLAMP_VIEW_PITCH 1
/**
 * Rotate the view around the horizontal (X) axis to the given angle.
 */
void View::setPitch( Real radians )
{
#if CLAMP_VIEW_PITCH
	m_pitch = clamp(DEG_TO_RADF(0.1f), radians, DEG_TO_RADF(89.9f));
#else
	m_pitch = WWMath::Normalize_Angle(radians);
#endif
}

void View::setDefaultPitch( Real radians )
{
#if CLAMP_VIEW_PITCH
	m_defaultPitch = clamp(DEG_TO_RADF(0.1f), radians, DEG_TO_RADF(89.9f));
#else
	m_defaultPitch = WWMath::Normalize_Angle(radians);
#endif
}

/**
 * Set the view angle back to default
 */
void View::setAngleToDefault()
{
	m_angle = m_defaultAngle;
}

/**
 * Set the view pitch back to default
 */
void View::setPitchToDefault()
{
	m_pitch = m_defaultPitch;
}

void View::setHeightAboveGround(Real z)
{
	// if our zoom is limited, we will stay within a predefined distance from the terrain
	if( m_zoomLimited )
	{
		m_heightAboveGround = clamp(m_minHeightAboveGround, z, m_maxHeightAboveGround);
	}
	else
	{
		m_heightAboveGround = z;
	}
}

/**
 * write the view's current location in to the view location object
 */
void View::getLocation( ViewLocation *location )
{
	location->init( getPosition(), getAngle(), getPitch(), getZoom() );
}


/**
 * set the view's current location from to the view location object
 */
void View::setLocation( const ViewLocation *location )
{
	if ( location->isValid() )
	{
		setPosition(location->getPosition());
		setAngle(location->getAngle());
		setPitch(location->getPitch());
		setZoom(location->getZoom());
	}

}

Bool View::isUserControlLocked() const
{
	return m_userControlLockedUntilFrame > TheGameClient->getFrame();
}

//-------------------------------------------------------------------------------------------------
/** project the 4 corners of this view into the world and return each point as a parameter,
		the world points are at the requested Z */
//-------------------------------------------------------------------------------------------------
PlaneClass::IntersectionResType View::getScreenCornerWorldPointsAtZ( Coord3D *topLeft, Coord3D *topRight,
																					Coord3D *bottomRight, Coord3D *bottomLeft,
																					Real z, ViewportClass viewPort )
{
	if( topLeft == nullptr || topRight == nullptr || bottomRight == nullptr || bottomLeft == nullptr)
		return PlaneClass::NO_INTERSECTION;

	ICoord2D screenTopLeft;
	ICoord2D screenTopRight;
	ICoord2D screenBottomRight;
	ICoord2D screenBottomLeft;
	ICoord2D origin;
	const Int viewWidth = getWidth();
	const Int viewHeight = getHeight();

	// setup the screen coords for the 4 corners of the viewable display
	getOrigin( &origin.x, &origin.y );

	screenTopLeft.x = origin.x + viewWidth * viewPort.Min.X;
	screenTopLeft.y = origin.y + viewHeight * viewPort.Min.Y;
	screenTopRight.x = origin.x + viewWidth * viewPort.Max.X;
	screenTopRight.y = origin.y + viewHeight * viewPort.Min.Y;
	screenBottomRight.x = origin.x + viewWidth * viewPort.Max.X;
	screenBottomRight.y = origin.y + viewHeight * viewPort.Max.Y;
	screenBottomLeft.x = origin.x + viewWidth * viewPort.Min.X;
	screenBottomLeft.y = origin.y + viewHeight * viewPort.Max.Y;

	PlaneClass::IntersectionResType combinedResult = PlaneClass::INSIDE_SEGMENT;
	PlaneClass::IntersectionResType individualResults[4];
	individualResults[0] = screenToWorldAtZ( &screenTopLeft, topLeft, z );
	individualResults[1] = screenToWorldAtZ( &screenTopRight, topRight, z );
	individualResults[2] = screenToWorldAtZ( &screenBottomRight, bottomRight, z );
	individualResults[3] = screenToWorldAtZ( &screenBottomLeft, bottomLeft, z );

	for( Int i = 0; i < 4; ++i )
	{
		if( individualResults[i] == PlaneClass::NO_INTERSECTION )
		{
			combinedResult = PlaneClass::NO_INTERSECTION;
			break;
		}
		if( individualResults[i] == PlaneClass::OUTSIDE_LINE )
		{
			combinedResult = PlaneClass::OUTSIDE_LINE;
		}
	}

	return combinedResult;
}

// ------------------------------------------------------------------------------------------------
/** Xfer method for a view */
// ------------------------------------------------------------------------------------------------
void View::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// camera angle
	Real angle = getAngle();
	xfer->xferReal( &angle );
	setAngle( angle );

	// view position
	Coord3D viewPos = getPosition();
	xfer->xferReal( &viewPos.x );
	xfer->xferReal( &viewPos.y );
	xfer->xferReal( &viewPos.z );
	lookAt( &viewPos );

}
