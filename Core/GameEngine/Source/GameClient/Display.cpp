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

// FILE: Display.cpp //////////////////////////////////////////////////////////
// The implementation of the Display class
// Author: Michael S. Booth, March 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/Display.h"
#include "GameClient/Mouse.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameText.h"
#include "GameClient/GlobalLanguage.h"
//#include "GameLogic/ScriptEngine.h"
#include "GameClient/GameClient.h"
#include "GameLogic/GameLogic.h"
#include "Common/FramePacer.h"
#include "Common/GameEngine.h"
#include "GameNetwork/NetworkInterface.h"

#include <chrono>

/// The Display singleton instance.
Display *TheDisplay = nullptr;


Display::Display()
{
	m_renderPhase = FALSE;
	m_viewList = nullptr;
	m_width = 0;
	m_height = 0;
	m_bitDepth = 0;
	m_windowed = FALSE;
	m_videoBuffer = nullptr;
	m_videoStream = nullptr;
	m_debugDisplayCallback = nullptr;
	m_debugDisplayUserData = nullptr;
	m_debugDisplay = nullptr;
	m_letterBoxFadeLevel = 0;
	m_letterBoxEnabled = FALSE;
	m_cinematicText = AsciiString::TheEmptyString;
	m_cinematicFont = nullptr;
	m_cinematicTextDeadline = 0;

	m_currentlyPlayingMovie.clear();
	m_letterBoxFadeStartTime = 0;
	m_isBatching = FALSE;
}

/**
 * Destructor for the Display.  Destroy all views attached to it.
 */
Display::~Display()
{

	stopMovie();
	// delete all our views if present
	deleteViews();

}

/**
	* Delete all views in the Display
	*/
void Display::deleteViews()
{
	View *v, *next;

	for( v = m_viewList; v; v = next )
	{
		next = v->getNextView();
		delete v;
	}
	m_viewList = nullptr;
}

/**
 * Attach the given view to the world
 * @todo Rethink the "attachView" notion...
 */
void Display::attachView( View *view )
{
	// prepend to head of list
	m_viewList = view->prependViewToList( m_viewList );
}

/**
 * Render all views of the world
 */
// GeneralsX @feature Claude 25/09/2026 Every world render funnels through here (W3DDisplay does not
// override it), so this count says whether the scene was drawn during a window of interest - the
// map-transfer screen logs its delta, which makes a scene leak under that screen readable from the log.
static UnsignedInt s_drawViewsCount = 0;
UnsignedInt Display::getDrawViewsCount() { return s_drawViewsCount; }

void Display::drawViews()
{
	++s_drawViewsCount;

	for( View *v = m_viewList; v; v = v->getNextView() )
		v->drawView();

}

/**
 * Updates all views of the world.  This forces state variables
   to refresh without actually drawing anything.
 */
void Display::updateViews()
{

	for( View *v = m_viewList; v; v = v->getNextView() )
		v->updateView();

}

void Display::stepViews()
{

	for( View *v = m_viewList; v; v = v->getNextView() )
		v->stepView();

}

/// Redraw the entire display
void Display::draw()
{
	// redraw all views
	drawViews();

	// redraw the in-game user interface
	/// @todo Switch between in-game and shell interfaces

}

/** Sets screen resolution/mode*/
Bool Display::setDisplayMode( UnsignedInt xres, UnsignedInt yres, UnsignedInt bitdepth, Bool windowed )
{
	//Get old values
	UnsignedInt oldDisplayHeight=getHeight();
	UnsignedInt oldDisplayWidth=getWidth();
	Int oldViewWidth=TheTacticalView->getWidth();
	Int oldViewHeight=TheTacticalView->getHeight();
	Int oldViewOriginX,oldViewOriginY;
	TheTacticalView->getOrigin(&oldViewOriginX,&oldViewOriginY);

	setWidth(xres);
	setHeight(yres);

	//Adjust view to match previous proportions
	TheTacticalView->setWidth((Real)oldViewWidth/(Real)oldDisplayWidth*(Real)xres);
	TheTacticalView->setHeight((Real)oldViewHeight/(Real)oldDisplayHeight*(Real)yres);
	TheTacticalView->setOrigin((Real)oldViewOriginX/(Real)oldDisplayWidth*(Real)xres,
	(Real)oldViewOriginY/(Real)oldDisplayHeight*(Real)yres);
	return TRUE;
}

// Display::setWidth ==========================================================
/** Set the width of the display */
//=============================================================================
void Display::setWidth( UnsignedInt width )
{

	// set the new width
	m_width = width;

	// set the new mouse limits
	if( TheMouse )
		TheMouse->setMouseLimits();

}

// Display::setHeight =========================================================
/** Set the height of the display */
//=============================================================================
void Display::setHeight( UnsignedInt height )
{

	// se the new height
	m_height = height;

	// set the new mouse limits
	if( TheMouse )
		TheMouse->setMouseLimits();

}

//============================================================================
// Display::playMovie
//============================================================================

void Display::playMovie( AsciiString movieName)
{
	if (TheGlobalData->m_headless)
		return;

	stopMovie();



	m_videoStream = TheVideoPlayer->open( movieName );

	if ( m_videoStream == nullptr )
	{
		return;
	}

	m_currentlyPlayingMovie = movieName;

	m_videoBuffer = createVideoBuffer();
	if (	m_videoBuffer == nullptr ||
				!m_videoBuffer->allocate(	m_videoStream->width(),
													m_videoStream->height())
		)
	{
		stopMovie();
		return;
	}

}

//============================================================================
// Display::stopMovie
//============================================================================

void Display::stopMovie()
{
	delete m_videoBuffer;
	m_videoBuffer = nullptr;

	if ( m_videoStream )
	{
		m_videoStream->close();
		m_videoStream = nullptr;
	}

	if (!m_currentlyPlayingMovie.isEmpty()) {
		//TheScriptEngine->notifyOfCompletedVideo(m_currentlyPlayingMovie); // Removing this sync-error cause MDC
		m_currentlyPlayingMovie = AsciiString::TheEmptyString;
	}
}

//============================================================================
// Display::update
//============================================================================

void Display::update()
{
	if ( m_videoStream && m_videoBuffer )
	{
		if ( m_videoStream->isFrameReady())
		{
			m_videoStream->frameDecompress();
			m_videoStream->frameRender( m_videoBuffer );
			if( m_videoStream->frameIndex() != m_videoStream->frameCount() - 1)
			{
				m_videoStream->frameNext();
			}
			else
			{
				stopMovie();
			}
		}
	}
}

//============================================================================
// Display::reset
//============================================================================

void Display::reset()
{
	//Remove letterbox border that may have been enabled by a script
	m_letterBoxFadeLevel = 0;
	m_letterBoxEnabled = FALSE;
	stopMovie();

	// Reset all views that need resetting
	for( View *v = m_viewList; v; v = v->getNextView() )
		v->reset();
}

//============================================================================
// Display::isMoviePlaying
//============================================================================

Bool Display::isMoviePlaying()
{
	return m_videoStream != nullptr && m_videoBuffer != nullptr;
}

//============================================================================
// Display::setDebugDisplayCallback
//============================================================================

void Display::setDebugDisplayCallback( DebugDisplayCallback *callback, void *userData )
{
	m_debugDisplayCallback = callback;
	m_debugDisplayUserData = userData;
}

//============================================================================
// Display::getDebugDisplayCallback
//============================================================================

Display::DebugDisplayCallback *Display::getDebugDisplayCallback()
{
	return m_debugDisplayCallback;
}

void Display::beginBatch()
{
	if (m_isBatching)
	{
		return;
	}
	m_isBatching = TRUE;
	onBeginBatch();
}

void Display::endBatch()
{
	if (!m_isBatching)
	{
		return;
	}
	onFlush();
	m_isBatching = FALSE;
	onEndBatch();
}

void Display::flush()
{
	if (m_isBatching)
	{
		onFlush();
	}
}

// GeneralsX @bugfix Claude 06/08/2026 See Display.h. Converts the script's frame count to a
// wall-clock deadline once, so the caption lasts the authored time at any render rate and still
// expires inside a frozen-time cinematic.
void Display::setCinematicTextFrames( Int frames )
{
	m_cinematicTextDeadline = (frames > 0)
	                          ? (UnsignedInt)(timeGetTime() + (UnsignedInt)(frames * MSEC_PER_LOGICFRAME_REAL))
	                          : 0;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 28/09/2026 The render interpolation phase. See GXLogicPhase.h for the two sources.
// GX_INTERP=0 pins it to 1.0f (the same-binary control arm, and a way out); a headless peer never computes it. So does a
// GX_SHOT capture (GameClient.cpp's capture clock, test/gfx-shotdiff): every render clock then follows the logic frame, so
// a shot at frame N shows frame N's logic pose whatever the wall clock, and two runs' shots compare.
// GX_PHASE=1 prints one "[GXPHASE]" line per client update.
static Bool gxLogicPhaseEnabled()
{
	static const Bool s_on = !(getenv("GX_INTERP") != nullptr && strcmp(getenv("GX_INTERP"), "0") == 0);
	return s_on && !TheGlobalData->m_headless && !gxShotCaptureOn();
}

static double gxLogicPhaseNowMs()
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The period the logic ticks at: the negotiated lobby rate in a network game, the logic time scale otherwise. 0 when
// the logic is not paced (it then ticks on every update, and the phase stays pinned at 1).
static double gxLogicPhaseTickMs()
{
	const Int fps = TheFramePacer->getActualLogicTimeScaleFps(FramePacer::IgnoreFrozenTime | FramePacer::IgnoreHaltedGame);
	return (fps > 0 && fps < RenderFpsPreset::UncappedFpsValue) ? 1000.0 / fps : 0.0;
}

// GameEngine keeps the regular gate's accumulator protected and its header is digested: read it through a pointer to member
// formed in a class derived from it, which the language allows without changing it. Never instantiated.
class GXEngineAccumulator : public GameEngine
{
public:
	static Real GameEngine::* member() { return &GXEngineAccumulator::m_logicTimeAccumulator; }
};

void Display::noteLogicTick( UnsignedInt frame )
{
	m_renderPhase = FALSE;
	if (gxLogicPhaseEnabled())
		m_logicPhase.tick(frame, gxLogicPhaseNowMs());
}

void Display::updateLogicPhase()
{
	m_renderPhase = TRUE;
	if (!gxLogicPhaseEnabled() || TheGameLogic == nullptr || TheFramePacer == nullptr)
		return;

	// Single player: the leftover canUpdateRegularGameLogic carried out of its last pass, in ticks (seconds x fps, the
	// scale it compares against), as the gate queries the rate - frozen time included.
	// When the gate ticks on every update without touching its accumulator (the logic rate reaches the render cap, a
	// script's fast-forward, the replay fast mode: GameEngine::canUpdateRegularGameLogic), the leftover is stale and
	// every draw already has a fresh frame: the phase is 1 (Codex interp-r6). Same conditions, read the same way;
	// the fast mode is taken without its replay-only test, which can only pin more often.
	const Int gateFps = TheFramePacer->getActualLogicTimeScaleFps(FramePacer::IgnoreFrozenTime);
	const Bool gateBypassed = TheGlobalData->m_TiVOFastMode || TheFramePacer->isScriptFastForward()
		|| gateFps >= TheFramePacer->getActualFramesPerSecondLimit();
	const Real spCarry = (TheGameEngine != nullptr && gateFps > 0 && !gateBypassed)
		? (TheGameEngine->*GXEngineAccumulator::member()) * gateFps : 1.0f;
	const double nowMs = gxLogicPhaseNowMs();
	const double tickMs = gxLogicPhaseTickMs();
	const UnsignedInt frame = TheGameLogic->getFrame();
	const Real alpha = m_logicPhase.update(frame, nowMs, tickMs, spCarry, TheNetwork != nullptr);

	static const Bool s_trace = getenv("GX_PHASE") != nullptr;
	if (s_trace)
		fprintf(stderr, "[GXPHASE] ms=%.3f f=%u a=%.6f pin=%d T=%.3f carry=%.4f net=%d\n", nowMs, frame, alpha, alpha == 1.0f ? 1 : 0, tickMs,
			spCarry, TheNetwork != nullptr ? 1 : 0);
}
