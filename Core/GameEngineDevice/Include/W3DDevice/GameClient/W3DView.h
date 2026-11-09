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

// FILE: W3DView.h ////////////////////////////////////////////////////////////////////////////////
//
// W3D implementation of the game view window.  View windows are literally
// a window into the game world that have width, height, and camera
// controls for what to look at
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "Common/MapObject.h"
#include "Common/STLTypedefs.h"
#include "Common/ScriptedCameraClock.h"
#include "GameClient/ParabolicEase.h"
#include "GameClient/View.h"
#include "WW3D2/camera.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////////////////////////
class Drawable;

///////////////////////////////////////////////////////////////////////////////////////////////////
enum {MAX_WAYPOINTS=25};

constexpr const Real TERRAIN_SAMPLE_SIZE = MAP_XY_FACTOR * 4;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
typedef struct
{
	Int			numWaypoints;
	Coord3D	waypoints[MAX_WAYPOINTS+2];		// We pad first & last for interpolation.
	Real		waySegLength[MAX_WAYPOINTS+2];	// Length of each segment;
	Real		cameraAngle[MAX_WAYPOINTS+2];	// Camera Angle;
	// GeneralsX @bugfix Claude 08/10/2026 The time (total, elapsed, ease) and the time multiplier are W3DView::m_camTimeline's.
	Real		totalDistance;								// Total length of paths.
	Real		curSegDistance;								// How far we are along the current seg.
	Int			shutter;
	Int			curSegment;										// The current segment.
	Int			curShutter;										// The current shutter.
	Int			rollingAverageFrames;					// Number of frames to roll.
} TMoveAlongWaypointPathInfo;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
typedef struct
{
	// GeneralsX @bugfix Claude 08/10/2026 Its frames, hold, ease and time multiplier are W3DView::m_camTimeline's.
	Bool		trackObject;					///< Are we tracking an object or just rotating?
	struct Target {
		ObjectID targetObjectID;			///< Target if we are tracking an object instead of just rotating
		Coord3D	targetObjectPos;			///< Target's position (so we can stay looking at that spot if he dies)
	};
	struct Angle {
		Real startAngle;
		Real endAngle;
	};
	union {
		Target target;
		Angle angle;
	};
} TRotateCameraInfo;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
typedef struct
{
	Real		startPitch;						///< (its frames are W3DView::m_camTimeline's)
	Real		endPitch;
	ParabolicEase ease;
} TPitchCameraInfo;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
typedef struct
{
	Real		startZoom;						///< (its frames are W3DView::m_camTimeline's)
	Real		endZoom;
	ParabolicEase ease;
} TZoomCameraInfo;

// ------------------------------------------------------------------------------------------------
/** W3DView implementation of the game view class.  This allows us to create
  * a "window" into the game world that can be sized and contains controls
	* for manipulating the camera */
// ------------------------------------------------------------------------------------------------
class W3DView : public View, public SubsystemInterface
{
	// GeneralsX @bugfix Claude 08/10/2026 The moves are m_camTimeline's (Common/ScriptedCameraClock.h); the lock is the view's.
	enum Scripted
	{
		Scripted_Rotate = ScriptedCameraTimeline::ROTATE, // Set when rotating the camera
		Scripted_Pitch = ScriptedCameraTimeline::PITCH, // Set when pitching the camera
		Scripted_Zoom = ScriptedCameraTimeline::ZOOM, // Set when zooming the camera
		Scripted_MoveOnWaypointPath = ScriptedCameraTimeline::PATH, // Set when moving camera along waypoint path
		Scripted_CameraLock = 1<<4, // Set when following a unit via script
	};
	typedef UnsignedInt ScriptedState;

public:
	W3DView();
	virtual ~W3DView() override;

	virtual void init() override;  ///< init/re-init the W3DView
	virtual void reset() override;
	virtual void drawView() override;  ///< draw this view
	virtual void updateView() override;	///<called once per frame to determine the final camera and object transforms
	virtual void stepView() override; ///< Update view for every fixed time step
	virtual void stepFrozenLogicUpdate() override;	///< the scripted camera's step in a script freeze (once per frozen logic update)

	virtual void draw() override;  ///< draw this view
	virtual void update() override;	///<called once per frame to determine the final camera and object transforms

	virtual Drawable *pickDrawable( const ICoord2D *screen, Bool forceAttack, PickType pickType ) override;  ///< pick drawable given the screen pixel coords.  If force attack, picks bridges.

	/// all drawables in the 2D screen region will call the 'callback'
	virtual Int iterateDrawablesInRegion( IRegion2D *screenRegion,
																Bool (*callback)( Drawable *draw, void *userData ),
																void *userData ) override;

	virtual void setWidth( Int width ) override;
	virtual void setHeight( Int height ) override;
	virtual void setOrigin( Int x, Int y) override;			///< Sets location of top-left view corner on display

	virtual void scrollBy( const Coord2D *delta ) override;  ///< Shift the view by the given delta

	virtual void forceRedraw() override;
	virtual void onHeightMapChanged() override { m_lastScreenToTerrainValid = false; }
	virtual void onBridgeChanged() override { m_lastScreenToTerrainValid = false; }

	virtual Bool isDoingScriptedCamera() override;
	virtual void stopDoingScriptedCamera() override;

	virtual void setAngle( Real radians ) override;									///< Rotate the view around the vertical axis to the given angle (yaw)
	virtual void setPitch( Real radians ) override;									///< Rotate the view around the horizontal axis to the given angle (pitch)
	virtual void setDefaultPitch( Real radians ) override;						///< Set new default camera pitch. It affects the camera distance to the ground
	virtual void setAngleToDefault() override;									///< Set the view angle back to default
	virtual void setPitchToDefault() override;									///< Set the view pitch back to default

	virtual void lookAt( const Coord3D *o ) override;											///< Center the view on the given coordinate
	virtual void initHeightForMap() override;												///< Init the camera height for the map at the current position.
	virtual void resetPivotToGround() override;												///< Set the camera pivot to the terrain height at the current position.
	virtual void moveCameraTo(const Coord3D *o, Int milliseconds,  Int shutter, Bool orient, Real easeIn, Real easeOut) override;
	virtual void moveCameraAlongWaypointPath(Waypoint *pWay, Int frames, Int shutter, Bool orient, Real easeIn, Real easeOut) override;
	virtual Bool isCameraMovementFinished() override;

	// GeneralsX @bugfix Claude 08/10/2026 The scripted camera steps on LOGIC frames (Common/ScriptedCameraClock.h): once
	// per new logic frame from stepView, from the draw only in frozen time, and it is drawn between the last two logic
	// frames' poses. See updateCameraMovements() in the .cpp for why the frame it ends on is simulation state.
	struct ScriptedPose
	{
		Coord3D pos;
		Real angle;
		Real zoom;
		Real fxPitch;
	};
	Bool stepScriptedCameraOnce(Real waypointMs);	///< zoom, pitch, then rotate or the waypoint path: one step
	Bool applyScriptedStep(unsigned did, Real waypointMs);	///< the pose of a timeline step (ScriptedCameraTimeline::stepOnce)
	void stepScriptedCamera();						///< one step per new logic frame, from stepView
	Bool blendScriptedCamera(ScriptedPose &logic);	///< swap in the drawn pose; logic receives the pose to put back
	void captureScriptedPose(ScriptedPose &pose) const;
	void applyScriptedPose(const ScriptedPose &pose);
	void noteViewFilterForCameraClock();			///< a filter was set: (re)start or end a motion blur's logic lifetime
	void endWaypointPathForCameraLock();			///< a camera lock ends a waypoint path, on every logic update that runs scripts
	UnsignedInt m_gxCamStepFrame;	///< the logic frame stepScriptedCamera last ran for
	UnsignedInt m_gxCamPoseFrame;	///< the logic frame whose step left m_gxCamCur; m_gxCamPrev is the pose before it
	UnsignedInt m_gxCamDrawnFrame;	///< the logic frame the last draw saw
	Bool m_gxCamContinues;			///< that frame's first draw followed a draw of the frame before (Drawable's rule)
	ScriptedPose m_gxCamPrev;
	ScriptedPose m_gxCamCur;
	Coord3D m_gxBlurJumpPos;		///< a motion blur jump's target (setViewFilterPos); m_camTimeline counts the blur
	// GeneralsX @bugfix Claude 08/10/2026 Step B: this view's scripted camera timing - which moves run, their steps, the time
	// multiplier and its divider, the camera freeze, a motion blur's logic lifetime - in the class the simulation's
	// TheScriptedCamera (GameLogic/ScriptedCamera.h) runs, so the pose ends where scripts see the move end. This instance is fed
	// by every caller, the player's camera keys too, and decides nothing scripts read.
	ScriptedCameraTimeline m_camTimeline;
	Bool m_scriptedCameraLock;		///< Scripted_CameraLock (setSnapMode); the moves are m_camTimeline's
	Coord3D m_gxCamDrawnLookAt;		///< the look-at the last draw showed (the pan motion blur's movement, calcDeltaScroll)
	// GeneralsX @bugfix Claude 03/10/2026 The camera easing's step, once per draw (GameClient/CameraStepPolicy.h).
	double m_lastDrawLoopSeconds;	///< gxRenderLoopSeconds() at the last update (draw)
	Real m_drawFpsRatio;			///< BaseFps over the time since the last draw: what the zoom and ground-follow easing step by
	virtual Bool isCameraMovementAtWaypointAlongPath();
 	virtual void resetCamera(const Coord3D *location, Int frames, Real easeIn, Real easeOut) override;	///< Move camera to location, and reset to default angle & zoom.
 	virtual void rotateCamera(Real rotations, Int frames, Real easeIn, Real easeOut) override;					///< Rotate camera about current viewpoint.
	virtual void rotateCameraTowardObject(ObjectID id, Int milliseconds, Int holdMilliseconds, Real easeIn, Real easeOut) override;	///< Rotate camera to face an object, and hold on it
	virtual void rotateCameraTowardPosition(const Coord3D *pLoc, Int milliseconds, Real easeIn, Real easeOut, Bool reverseRotation) override;	///< Rotate camera to face a location.
	virtual void cameraModFreezeTime() override { m_camTimeline.freezeTime(); }					///< Freezes time during the next camera movement.
	virtual void cameraModFreezeAngle() override;												///< Freezes time during the next camera movement.
	virtual Bool isTimeFrozen() override { return m_camTimeline.isTimeFrozen(); }					///< Freezes time during the next camera movement.
	virtual void cameraModFinalZoom(Real finalZoom, Real easeIn, Real easeOut) override;	///< Final zoom for current camera movement.
	virtual void cameraModRollingAverage(Int framesToAverage) override;			///< Number of frames to average movement for current camera movement.
	virtual void cameraModFinalTimeMultiplier(Int finalMultiplier) override; ///< Final time multiplier for current camera movement.
	virtual void cameraModFinalPitch(Real finalPitch, Real easeIn, Real easeOut) override;	///< Final pitch for current camera movement.
	virtual void cameraModLookToward(Coord3D *pLoc) override;								///< Sets a look at point during camera movement.
	virtual void cameraModFinalLookToward(Coord3D *pLoc) override;						///< Sets a look at point during camera movement.
	virtual void cameraModFinalMoveTo(Coord3D *pLoc) override;			///< Sets a final move to.
	// (gth) C&C3 animation controlled camera feature
	virtual void cameraEnableSlaveMode(const AsciiString & thingtemplateName, const AsciiString & boneName) override;
	virtual void cameraDisableSlaveMode() override;
	virtual void cameraEnableRealZoomMode(); //WST 10.18.2002
	virtual void cameraDisableRealZoomMode();
	virtual	void Add_Camera_Shake(const Coord3D & position,float radius, float duration, float power) override; //WST 10.18.2002
	virtual Int	 getTimeMultiplier() override {return m_camTimeline.timeMultiplier();};///< Get the time multiplier.
	virtual void setTimeMultiplier(Int multiple) override {m_camTimeline.setTimeMultiplier(multiple);}; ///< Set the time multiplier.
	virtual void setDefaultView(Real pitch, Real angle, Real maxHeight) override;
	virtual void setCameraHeightAboveGroundLimitsToDefault(Real heightScale = 1.0f);
	virtual void zoomCamera( Real finalZoom, Int milliseconds, Real easeIn, Real easeOut ) override;
	virtual void pitchCamera( Real finalPitch, Int milliseconds, Real easeIn, Real easeOut ) override;

	virtual void setHeightAboveGround(Real z) override;
	virtual void setZoom(Real z) override;
	virtual void setZoomToMax();
	virtual void setZoomToDefault() override;									///< Set zoom to default value
	virtual Real getRetailReferenceZoom( Real *iconScale = nullptr ) override;

	virtual void setFieldOfView( Real angle ) override;							///< Set the horizontal field of view angle

  virtual WorldToScreenReturn worldToScreenTriReturn( const Coord3D *w, ICoord2D *s ) override;	///< Transform world coordinate "w" into screen coordinate "s"
	virtual Bool screenToTerrain( const ICoord2D *screen, Coord3D *world ) override;
	virtual PlaneClass::IntersectionResType screenToWorldAtZ( const ICoord2D *screen, Coord3D *world, Real z ) override;

	CameraClass *get3DCamera() const { return m_3DCamera; }

	virtual Coord3D get3DCameraPosition() const override; ///< Returns the actual camera position
	virtual Coord3D get3DCameraDirection() const override; ///< Returns the actual camera view direction
	virtual void set3DCameraLookAt(const Coord3D &pos, const Coord3D &dir, Real roll) override; ///< Set the actual camera position and view direction

	virtual void setCameraLock(ObjectID id) override;
	virtual void setSnapMode( CameraLockType lockType, Real lockDist ) override;

	/// Add an impulse force to shake the camera
	virtual void shake( const Coord3D *epicenter, CameraShakeType shakeType ) override;

	virtual Real getFXPitch() const override { return m_FXPitch; }					///< returns the FX pitch angle

	virtual Bool setViewFilterMode(FilterModes filterMode) override;			///< Turns on viewport special effect (black & white mode)
	virtual Bool setViewFilter(FilterTypes filter) override;			///< Turns on viewport special effect (black & white mode)
	virtual void setViewFilterPos(const Coord3D *pos) override;			///<  Passes a position to the special effect filter.
	virtual FilterModes getViewFilterMode() override {return m_viewFilterMode;}			///< Turns on viewport special effect (black & white mode)
	virtual FilterTypes getViewFilterType() override {return m_viewFilter;}			///< Turns on viewport special effect (black & white mode)
	virtual void setFadeParameters(Int fadeFrames, Int direction) override;
	virtual void set3DWireFrameMode(Bool enable) override;	///<enables custom wireframe rendering of 3D viewport

	Bool updateCameraMovements();
	virtual void forceCameraAreaConstraintRecalc() override { m_cameraAreaConstraintsValid = false; }

	virtual void setGuardBandBias( const Coord2D *gb ) override { m_guardBandBias.x = gb->x; m_guardBandBias.y = gb->y; }

private:

	CameraClass *m_3DCamera;												///< camera representation for 3D scene
	CameraClass *m_2DCamera;												///< camera for UI overlayed on top of 3D scene
	FilterModes m_viewFilterMode;
	FilterTypes m_viewFilter;
	Bool m_isWireFrameEnabled;
	Bool m_nextWireFrameEnabled;											///< used to delay wireframe changes by 1 frame (needed for transitions).


	Coord2D m_shakeOffset;													///< the offset to add to the camera position
	Real m_shakeAngleCos;														///< the cosine of the orientation of the oscillation
	Real m_shakeAngleSin;														///< the sine of the orientation of the oscillation
	Real m_shakeIntensity;													///< the intensity of the oscillation
	Vector3 m_shakerAngles;													//WST 11/12/2002 new multiple instance camera shaker system

	TRotateCameraInfo m_rcInfo;
	TPitchCameraInfo m_pcInfo;
	TZoomCameraInfo m_zcInfo;
	TMoveAlongWaypointPathInfo m_mcwpInfo;					///< Move camera along waypoint path info.
	Bool m_CameraArrivedAtWaypointOnPathFlag;

	Real		m_FXPitch;															///< Camera effects pitch.  0 = flat, infinite = look down, 1 = normal.

	// TheSuperHackers @performance Cache only the latest screen-to-terrain result.
	Bool		m_lastScreenToTerrainValid;
	ICoord2D	m_lastScreenToTerrainScreen;
	Coord3D		m_lastScreenToTerrainWorld;

	Coord3D m_previousLookAtPosition;
	Coord2D m_scrollAmount;													///< scroll speed
	Real m_scrollAmountCutoffSqr;										///< scroll speed at which we do not adjust height

#if PRESERVE_RETAIL_SCRIPTED_CAMERA
	// TheSuperHackers @tweak Uses the initial ground level for preserving the original look of the scripted camera,
	// because alterations to the ground level do affect the positioning in subtle ways.
	Real m_initialGroundLevel;
#endif

	Region2D m_cameraAreaConstraints; ///< Camera should be constrained to be within this area
	Bool m_cameraAreaConstraintsValid; ///< If false, recalculates the camera area constraints in the next render update
	Bool m_recalcCameraConstraintsAfterScrolling; ///< Recalculates the camera area constraints after the user has moved the camera
	Bool m_recalcCamera; ///< Recalculates the camera transform in the next render update

	Real getHeightAroundPos(Real x, Real y, Real terrainSampleSize = TERRAIN_SAMPLE_SIZE) const;
	// GeneralsX @bugfix Claude 10/09/2026 Shell map uses the stock INI camera height limits, not the
	// user's Extras/SagePatch override. See the implementation comment in W3DView.cpp.
	//
	// Out-params rather than a returned GXHostCamera::Limits so this header keeps its current include
	// set - the pair type lives in GameClient/GXHostCamera.h, which W3DView.h does not include today.
	void getEffectiveCameraHeightLimits(Real *minHeight, Real *maxHeight) const;
	Real getEffectiveMaxCameraHeight() const;
	Real getCameraOffsetZ() const;
	Real getDesiredHeight(Real x, Real y) const;
	Real getDesiredZoom(Real x, Real y) const;
	Real getMaxHeight(Real x, Real y) const;
	Real getMaxZoom(Real x, Real y) const;
	void updateCameraTransform(); ///< update the transform matrix of m_3DCamera, based on m_pos & m_angle
	void updateCameraClipPlanes(const Matrix3D &transform);
	void setCameraTransform(const Matrix3D &transform);
	void buildCameraPosition(Vector3 &sourcePos, Vector3 &targetPos);
	void buildCameraTransform(Matrix3D *transform, const Vector3 &sourcePos, const Vector3 &targetPos); ///< calculate (but do not set) the transform matrix of m_3DCamera, based on m_pos & m_angle
	Bool zoomCameraToDesiredHeight();
	Real gxBodyClearedZoom(Real desiredZoom);	///< GeneralsX: the desired zoom raised so the camera body clears the ground under it
	Bool movePivotToGround();
	void updateCameraAreaConstraints();
	void calcCameraAreaConstraints(); ///< Recalculates the camera area constraints
	Real calcCameraAreaOffset(Real maxEdgeZ);
	void clipCameraIntoAreaConstraints();
	Bool isWithinCameraAreaConstraints() const;
	Bool isWithinCameraHeightConstraints() const;
	virtual void setUserControlled(Bool value) override;
	Bool hasScriptedState(ScriptedState state) const;
	void addScriptedState(ScriptedState state);
	void removeScriptedState(ScriptedState state);
	void moveAlongWaypointPath(Real elapsed, Real totalTime, Real milliseconds, Bool ended); ///< The pose of a path step (elapsed after it)
	void getPickRay(const ICoord2D *screen, Vector3 *rayStart, Vector3 *rayEnd);	///<returns a line segment (ray) originating at the given screen position
	void setupWaypointPath(Bool orient);					///< Calculates distances & angles for moving along a waypoint path.
	void rotateCameraOneFrame(Bool ended);					///< The pose of one frame of a rotate camera movement.
	void zoomCameraOneFrame(Bool ended);					///< The pose of one frame of a zoom camera movement.
	void pitchCameraOneFrame(Bool ended);					///< The pose of one frame of a pitch camera movement.
	void getAxisAlignedViewRegion(Region3D &axisAlignedRegion);	///< Find 3D Region enclosing all possible drawables.
	void calcDeltaScroll(Coord2D &screenDelta);
	bool getDesiredTerrainDrawSize(ICoord2D &dimensions) const;
	void updateTerrain();

	// (gth) C&C3 animation controlled camera feature
	Bool				m_isCameraSlaved;
	Bool				m_useRealZoomCam;
	AsciiString		m_cameraSlaveObjectName;
	AsciiString		m_cameraSlaveObjectBoneName;

	// GeneralsX @performance Claude 23/09/2026 The drawables update()'s draw walk visited, kept so
	// draw()'s post-draw walk can skip a second pass over every drawable in the world. Valid only
	// while GameClient::getDrawableChangeCount() and the view region both still match.
	std::vector<Drawable*>	m_drawWalkList;
	Region3D				m_drawWalkRegion;
	UnsignedInt			m_drawWalkChangeCount;
	Bool						m_drawWalkValid;

	// GeneralsX @performance Claude 26/09/2026 The largest terrain draw size asked for this game; the
	// window grows to what the camera needs and never shrinks back until reset() (see updateTerrain).
	ICoord2D				m_terrainDrawSize;
	Real						m_terrainDrawScale;		///< the TerrainDrawDistanceScale m_terrainDrawSize was grown under
	Bool						m_terrainDrawEntire;	///< the DrawEntireTerrain it was grown under
	Real						m_terrainDrawMaxHeight;	///< the camera max it was grown under; a lower one starts it over
};

// EXTERNALS //////////////////////////////////////////////////////////////////////////////////////
extern Real TheW3DFrameLengthInMsec;
