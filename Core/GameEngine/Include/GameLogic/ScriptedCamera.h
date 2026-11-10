/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: THE SIMULATION'S HALF OF THE SCRIPTED CAMERA (catch-up re-land, step B).
//
// CAMERA_MOVEMENT_FINISHED is a script condition and GameEngine::isTimeFrozen reads the same answer, so whether a scripted
// camera move has ended is simulation state. Both used to ask TheTacticalView: W3DView on a rendered peer, and ViewDummy -
// a constant TRUE - on a -headless peer, the standing host and ReplaySimulation, so a map whose scripts wait for a camera
// move split a match with a headless peer and a replay played headless. They ask TheScriptedCamera now: a
// ScriptedCameraTimeline (Common/ScriptedCameraClock.h) fed by the SCRIPT ACTIONS ALONE - each method below has View's
// signature, feeds the timeline and passes the same call on to TheTacticalView, so ScriptActions changes only the receiver -
// and stepped by GameLogic::update. The player's own camera calls (a rotate key, a scroll, a radar jump, InGameUI's reset, a
// replay's recorded camera) reach the view only: they end or start the view's moves, never this answer.
//
// Not saved: the view's scripted state never was either (View::xfer keeps the position and angle), so a save loaded
// mid-move answers TRUE, as before. Reset with the script engine (ScriptEngine::newMap).

#pragma once

#include "Common/ScriptedCameraClock.h"
#include "Common/GameType.h"
#include "Lib/BaseType.h"

class Waypoint;

class ScriptedCamera
{
public:
	ScriptedCamera();

	void reset();	///< ScriptEngine::newMap: every game starts with no move running

	// --- script actions: the timeline, then the same call on TheTacticalView. The times are the script's Real milliseconds
	// (seconds * 1000 of a map's raw float), turned into View's Int here, once, for both: the implicit conversion at the call
	// site was undefined for a NaN or huge value and differed by CPU (ScriptedCameraClock::toIntSaturate).
	void moveCameraTo(const Coord3D *o, Real milliseconds, Real shutter, Bool orient, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void moveCameraAlongWaypointPath(Waypoint *way, Real milliseconds, Real shutter, Bool orient, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void resetCamera(const Coord3D *location, Real milliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void rotateCamera(Real rotations, Real milliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void rotateCameraTowardObject(ObjectID id, Real milliseconds, Real holdMilliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void rotateCameraTowardPosition(const Coord3D *pLoc, Real milliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f, Bool reverseRotation = FALSE);
	void zoomCamera(Real finalZoom, Real milliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void pitchCamera(Real finalPitch, Real milliseconds, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void cameraModFinalZoom(Real finalZoom, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void cameraModFinalPitch(Real finalPitch, Real easeIn = 0.0f, Real easeOut = 0.0f);
	void cameraModFinalTimeMultiplier(Int finalMultiplier);
	void cameraModFreezeTime();
	void setTimeMultiplier(Int multiple);
	void setCameraLock(ObjectID id);
	void setAngle(Real radians);	///< W3DView ends every scripted move on it (stopDoingScriptedCamera)
	void setPitch(Real radians);	///< ...and on this

	// --- script actions that set a view filter: the simulation's half ONLY, as if the set succeeds (a -headless peer's view
	// has no filter). The action then makes its own view calls.
	void noteMotionBlur(Bool inAndOut, Bool outOnly, Bool target);	///< the motion blur filter in an IN, OUT or IN_AND_OUT mode
	void noteViewFilterOff();	///< any other filter or mode: ends a blur's answer

	// --- GameLogic::update
	void beforeScripts();	///< a camera-only freeze from the last update ends before this update's scripts read it
	void frozenUpdate();	///< an update that left time frozen: a script freeze steps once; a camera-only one ends next
	void logicFrame();		///< after m_frame++: a camera lock ends a path, then one logic frame's step

	// --- the answers
	Bool isCameraMovementFinished() const { return m_timeline.isFinished(); }	///< CAMERA_MOVEMENT_FINISHED
	Bool isTimeFrozen() const { return m_timeline.isTimeFrozen(); }	///< the camera freeze flag (GameEngine::isTimeFrozen)

	Bool isCamTestOn() const;	///< GX_CAMTEST is set (W3DView's [GXCAMDRAW] lines)

private:
	void endPathForLock();
	void camTest(UnsignedInt frame, Bool done);

	ScriptedCameraTimeline m_timeline;
	ObjectID m_lock;			///< the script's camera lock (W3DView's ends a path; the player's own lock never reaches here)
	Bool m_freezePending;		///< a camera-only freeze held the last update
};

extern ScriptedCamera TheScriptedCamera;
