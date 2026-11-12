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

// The simulation's half of the scripted camera: GameLogic/ScriptedCamera.h says why.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameLogic/ScriptedCamera.h"

#include "Common/FramePacer.h"
#include "Common/GameCommon.h"
#include "Common/GameEngine.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"

ScriptedCamera TheScriptedCamera;

ScriptedCamera::ScriptedCamera()
{
	reset();
}

void ScriptedCamera::reset()
{
	m_timeline.reset(MSEC_PER_LOGICFRAME_REAL);
	m_lock = INVALID_ID;
	m_freezePending = FALSE;
}

// ------------------------------------------------------------------------------------------------
// The script actions. Each feeds the timeline what W3DView's function of that name decides the TIMING by, then makes the
// same call on the view, which moves the camera. The camera's start (its position, angle, zoom) never reaches the timeline.
// ------------------------------------------------------------------------------------------------
void ScriptedCamera::moveCameraTo(const Coord3D *o, Real milliseconds, Real shutter, Bool orient, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.path(ms, 1, easeIn, easeOut);	// W3DView::moveCameraTo ignores the shutter: one step a frame
	TheTacticalView->moveCameraTo(o, ms, ScriptedCameraClock::toIntSaturate(shutter), orient, easeIn, easeOut);
}

void ScriptedCamera::moveCameraAlongWaypointPath(Waypoint *way, Real milliseconds, Real shutterMs, Bool orient, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	const Int shutter = ScriptedCameraClock::toIntSaturate(shutterMs);
	// a path always runs its time: W3DView dropped a first waypoint within MAP_XY_FACTOR of the camera and did not start a
	// path left with one, which depended on where this peer's camera stood
	m_timeline.path(ms, ScriptedCameraClock::toIntSaturate(shutter/MSEC_PER_LOGICFRAME_REAL), easeIn, easeOut);
	TheTacticalView->moveCameraAlongWaypointPath(way, ms, shutter, orient, easeIn, easeOut);
}

void ScriptedCamera::resetCamera(const Coord3D *location, Real milliseconds, Real easeIn, Real easeOut)
{
	// W3DView::resetCamera: moveCameraTo, then a zoom and a pitch over the same time
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.path(ms, 1, easeIn, easeOut);
	m_timeline.zoom(ms);
	m_timeline.pitch(ms);
	TheTacticalView->resetCamera(location, ms, easeIn, easeOut);
}

void ScriptedCamera::rotateCamera(Real rotations, Real milliseconds, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.rotate(ms, 0, easeIn, easeOut);
	TheTacticalView->rotateCamera(rotations, ms, easeIn, easeOut);
}

void ScriptedCamera::rotateCameraTowardObject(ObjectID id, Real milliseconds, Real holdMilliseconds, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	const Int holdMs = ScriptedCameraClock::toIntSaturate(holdMilliseconds);
	m_timeline.rotate(ms, holdMs, easeIn, easeOut);
	TheTacticalView->rotateCameraTowardObject(id, ms, holdMs, easeIn, easeOut);
}

void ScriptedCamera::rotateCameraTowardPosition(const Coord3D *pLoc, Real milliseconds, Real easeIn, Real easeOut, Bool reverseRotation)
{
	// always runs its time: W3DView refused to start within 0.1 of the camera, which is where this peer's camera stood
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.rotate(ms, 0, easeIn, easeOut);
	TheTacticalView->rotateCameraTowardPosition(pLoc, ms, easeIn, easeOut, reverseRotation);
}

void ScriptedCamera::zoomCamera(Real finalZoom, Real milliseconds, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.zoom(ms);
	TheTacticalView->zoomCamera(finalZoom, ms, easeIn, easeOut);
}

void ScriptedCamera::pitchCamera(Real finalPitch, Real milliseconds, Real easeIn, Real easeOut)
{
	const Int ms = ScriptedCameraClock::toIntSaturate(milliseconds);
	m_timeline.pitch(ms);
	TheTacticalView->pitchCamera(finalPitch, ms, easeIn, easeOut);
}

void ScriptedCamera::cameraModFinalZoom(Real finalZoom, Real easeIn, Real easeOut)
{
	m_timeline.finalZoom();
	TheTacticalView->cameraModFinalZoom(finalZoom, easeIn, easeOut);
}

void ScriptedCamera::cameraModFinalPitch(Real finalPitch, Real easeIn, Real easeOut)
{
	m_timeline.finalPitch();
	TheTacticalView->cameraModFinalPitch(finalPitch, easeIn, easeOut);
}

void ScriptedCamera::cameraModFinalTimeMultiplier(Int finalMultiplier)
{
	m_timeline.finalTimeMultiplier(finalMultiplier);
	TheTacticalView->cameraModFinalTimeMultiplier(finalMultiplier);
}

void ScriptedCamera::cameraModFreezeTime()
{
	m_timeline.freezeTime();
	TheTacticalView->cameraModFreezeTime();
}

void ScriptedCamera::setTimeMultiplier(Int multiple)
{
	m_timeline.setTimeMultiplier(multiple);
	TheTacticalView->setTimeMultiplier(multiple);
}

void ScriptedCamera::setCameraLock(ObjectID id)
{
	// W3DView refuses a lock under -noCameraMovement (GlobalData::m_disableCameraMovement): a command-line switch of this
	// peer's, so the simulation takes the lock regardless
	m_lock = id;
	TheTacticalView->setCameraLock(id);
}

void ScriptedCamera::setAngle(Real radians)
{
	m_timeline.stopAll();
	TheTacticalView->setAngle(radians);
}

void ScriptedCamera::setPitch(Real radians)
{
	m_timeline.stopAll();
	TheTacticalView->setPitch(radians);
}

void ScriptedCamera::noteMotionBlur(Bool inAndOut, Bool outOnly, Bool target)
{
	// the final state of the action's filter, mode and target sets (each restarted the count; nothing steps between them)
	m_timeline.blur(true, inAndOut, outOnly);
	if (target)
		m_timeline.blurTarget();
}

void ScriptedCamera::noteViewFilterOff()
{
	m_timeline.blur(false, false, false);
}

// ------------------------------------------------------------------------------------------------
// The steps, at the points of GameLogic::update where W3DView stepped (GameClient::step after an update that advanced the
// frame; GameEngine::updateLogic after a script-frozen one; the draw's freeze loop for a camera-only freeze, which ran the
// move to its end before the next update): the same logic frames, on every peer, with or without a draw.
// ------------------------------------------------------------------------------------------------

// A camera lock ends a waypoint path, and a lock whose object is gone ends (W3DView::update did both), on every logic update
// that runs scripts, so a lock a script sets ends the path before the next script reads CAMERA_MOVEMENT_FINISHED.
void ScriptedCamera::endPathForLock()
{
	if (m_lock == INVALID_ID)
		return;
	m_timeline.endPath();
	if (TheGameLogic->findObjectByID(m_lock) == nullptr)
		m_lock = INVALID_ID;
}

void ScriptedCamera::beforeScripts()
{
	if (!m_freezePending)
		return;
	m_freezePending = FALSE;
	endPathForLock();
	m_timeline.runToEnd();
}

void ScriptedCamera::frozenUpdate()
{
	if (TheScriptEngine->isTimeFrozenDebug())
		return;	// a debug freeze moves nothing
	if (TheScriptEngine->isTimeFrozenScript())
	{
		// FREEZE_TIME: the frame stands but the scripts run on every such update and read CAMERA_MOVEMENT_FINISHED, so the
		// camera steps on it, once, through the divider - as 2003's coupled loop stepped it once per draw, once per update
		endPathForLock();
		m_timeline.stepScriptFrozen();
		return;
	}
	// a camera-only freeze (CAMERA_MOD_FREEZE_TIME): no logic frame passes and no script runs until its moves end - the
	// rendered peer's draw loop ran them out before the next update - so they end before the next update's scripts
	if (m_timeline.isTimeFrozen() && !m_timeline.isFinished())
		m_freezePending = TRUE;
}

void ScriptedCamera::logicFrame()
{
	const UnsignedInt frame = TheGameLogic->getFrame();
	camTest(frame, FALSE);
	endPathForLock();
	const unsigned did = m_timeline.stepLogicFrame();
	// a blur's TRUE ending can make a camera freeze in flight hold - the only way one starts after the update's own frozen
	// check: GameEngine::updateLogic re-reads it after the client step, the rendered peer's draw loop runs the move out, and
	// it ends before the next update's scripts here as well (the next update's scripts read FALSE)
	if ((did & ScriptedCameraTimeline::BLUR_ENDED) && TheGameEngine != nullptr && TheGameEngine->isTimeFrozen()
		&& !TheScriptEngine->isTimeFrozenScript() && !TheScriptEngine->isTimeFrozenDebug())
		m_freezePending = TRUE;
	camTest(frame, TRUE);
}

// ------------------------------------------------------------------------------------------------
// The scripted camera's in-game check (scripts/test/scripted-camera-clock-test.sh covers
// the model). GX_CAMTEST=<frame>:<ms>:<pan|rot>[,...] starts a scripted camera move in the logic step of that frame - where
// a script action in the update before it would have started it - through this class, so the view moves too, and prints
// "[GXCAMDONE] kind= start= end= ms=", end = the logic frame whose scripts first see CAMERA_MOVEMENT_FINISHED. It runs on
// every peer, -headless and ReplaySimulation included, so their end frames compare with a rendered peer's. It changes what
// scripts read, so a run sets it on every peer or none; never in a shipped launch.
// ------------------------------------------------------------------------------------------------
namespace
{
	struct GXCamTestMove { UnsignedInt at; Int ms; Bool rot; Int state; };	// state: 0 waiting, 1 running, 2 done
	GXCamTestMove s_gxCamTest[4];
	Int s_gxCamTestCount = -1;

	Bool gxCamTestOn()
	{
		if (s_gxCamTestCount < 0)
		{
			s_gxCamTestCount = 0;
			const char *spec = getenv("GX_CAMTEST");
			while (spec != nullptr && *spec != '\0' && s_gxCamTestCount < 4)
			{
				unsigned at = 0;
				int ms = 0;
				char kind[8] = "";
				if (sscanf(spec, "%u:%d:%7[a-z]", &at, &ms, kind) < 2 || ms < 1)
					break;
				s_gxCamTest[s_gxCamTestCount].at = at;
				s_gxCamTest[s_gxCamTestCount].ms = ms;
				s_gxCamTest[s_gxCamTestCount].rot = strcmp(kind, "rot") == 0;
				s_gxCamTest[s_gxCamTestCount].state = 0;
				++s_gxCamTestCount;
				spec = strchr(spec, ',');
				if (spec != nullptr)
					++spec;
			}
			fprintf(stderr, "[GXCAMTEST] %d move(s)\n", s_gxCamTestCount);
		}
		return s_gxCamTestCount > 0;
	}
}

Bool ScriptedCamera::isCamTestOn() const
{
	return gxCamTestOn();
}

void ScriptedCamera::camTest(UnsignedInt frame, Bool done)
{
	if (!gxCamTestOn())
		return;
	for (Int i = 0; i < s_gxCamTestCount; ++i)
	{
		GXCamTestMove &m = s_gxCamTest[i];
		if (!done && m.state == 0 && frame >= m.at)
		{
			const Real ease = m.ms * 0.25f;
			if (m.rot)
				rotateCamera(0.25f, m.ms, ease, ease);
			else
			{
				Coord3D to = TheTacticalView->getPosition();	// where to: the view's business; the timing is the simulation's
				to.x += 300.0f;
				to.y += 200.0f;
				moveCameraTo(&to, m.ms, 0, FALSE, ease, ease);
			}
			m.state = 1;
			m.at = frame;
			fprintf(stderr, "[GXCAMTEST] start f=%u kind=%s ms=%d\n", frame, m.rot ? "rot" : "pan", m.ms);
		}
		else if (done && m.state == 1 && isCameraMovementFinished())
		{
			m.state = 2;
			fprintf(stderr, "[GXCAMDONE] kind=%s start=%u end=%u ms=%d\n", m.rot ? "rot" : "pan", m.at, frame, m.ms);
		}
	}
}
