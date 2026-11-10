/*
**	Command & Conquer Generals(tm)
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

// FILE: GameClient.cpp ////////////////////////////////////////////////////
// Implementation of GameClient singleton
// Author: Michael S. Booth, March 2001
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdio>
#include <cstdlib>
#include "GameClient/GameClient.h"
#include "Common/FramePacer.h"
#include "GameClient/GXLoginFlow.h"

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/ActionManager.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/Recorder.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "GameClient/Anim2D.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Diplomacy.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/DrawGroupInfo.h"
#include "GameClient/Eva.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/GraphDraw.h"
#include "GameClient/GUICommandTranslator.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/HintSpy.h"
#include "GameClient/HotKey.h"
#include "GameClient/IMEManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Intro.h"
#include "GameClient/Keyboard.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/Mouse.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/PlaceEventTranslator.h"
#include "GameClient/RayEffect.h"
#include "GameClient/SaveLoadFeedback.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/Shell.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/View.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/WindowXlat.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"
#include "GameLogic/Object.h"
#include "GameLogic/ScriptEngine.h"		// For TheScriptEngine - jkmcd
extern Bool gxSkyScriptOff();
extern void gxSetSkyScriptOff(Bool off);

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
#define DRAWABLE_HASH_SIZE	8192

/// The GameClient singleton instance
GameClient *TheGameClient = nullptr;
UnsignedInt GameClient::s_drawableChangeCount = 0;
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
GameClient::GameClient()
{

	// zero our translator list
	for( Int i = 0; i < MAX_CLIENT_TRANSLATORS; i++ )
		m_translators[ i ] = TRANSLATOR_ID_INVALID;
	m_numTranslators = 0;
	m_commandTranslator = nullptr;

	m_drawableTOC.clear();
	m_textBearingDrawableList.clear();

	m_frame = 0;

	m_drawableList = nullptr;

	m_nextDrawableID = (DrawableID)1;
	TheDrawGroupInfo = new DrawGroupInfo;

	m_intro = nullptr;
}
// GX-SIMID-END

//std::vector<std::string>	preloadTextureNamesGlobalHack;
//std::vector<std::string>	preloadTextureNamesGlobalHack2;

//-------------------------------------------------------------------------------------------------
GameClient::~GameClient()
{
#ifdef PERF_TIMERS
	delete TheGraphDraw;
	TheGraphDraw = nullptr;
#endif

	delete TheDrawGroupInfo;
	TheDrawGroupInfo = nullptr;

	// clear any drawable TOC we might have
	m_drawableTOC.clear();

	delete TheCampaignManager;
	TheCampaignManager = nullptr;

	// destroy all Drawables
	Drawable *draw, *nextDraw;
	for( draw = m_drawableList; draw; draw = nextDraw )
	{
		nextDraw = draw->getNextDrawable();
		destroyDrawable( draw );
	}
	m_drawableList = nullptr;

	// Should be destroyed before the Display String Manager
	delete m_intro;
	m_intro = nullptr;

	// delete the ray effects
	delete TheRayEffects;
	TheRayEffects = nullptr;

	// delete the hot key manager
	delete TheHotKeyManager;
	TheHotKeyManager = nullptr;

	// destroy the in-game user interface
	delete TheInGameUI;
	TheInGameUI = nullptr;

	// delete the shell
	delete TheShell;
	TheShell = nullptr;

	delete TheIMEManager;
	TheIMEManager = nullptr;

	// delete window manager
	delete TheWindowManager;
	TheWindowManager = nullptr;

	// delete the font library
	TheFontLibrary->reset();
	delete TheFontLibrary;
	TheFontLibrary = nullptr;

	TheMouse->reset();
	delete TheMouse;
	TheMouse = nullptr;

	///@todo :  TheTerrainVisual used to be the first thing destroyed.
	//I had to put in here so that drawables free their track marks before
	//the terrain visual deletes the track laying system. MW

	// destroy the terrain visual representation
	delete TheTerrainVisual;
	TheTerrainVisual = nullptr;

	// destroy the display
	delete TheDisplay;
	TheDisplay = nullptr;

	delete TheHeaderTemplateManager;
	TheHeaderTemplateManager = nullptr;

	delete TheLanguageFilter;
	TheLanguageFilter = nullptr;

	delete TheVideoPlayer;
	TheVideoPlayer = nullptr;

	// destroy all translators
	for( UnsignedInt i = 0; i < m_numTranslators; i++ )
		TheMessageStream->removeTranslator( m_translators[ i ] );
	m_numTranslators = 0;
	m_commandTranslator = nullptr;

	delete TheAnim2DCollection;
	TheAnim2DCollection = nullptr;

	delete TheMappedImageCollection;
	TheMappedImageCollection = nullptr;

	delete TheKeyboard;
	TheKeyboard = nullptr;

	delete TheDisplayStringManager;
	TheDisplayStringManager = nullptr;

	delete TheEva;
	TheEva = nullptr;

	// GeneralsX: Abandon any login still polling; its worker thread is
	// detached and would otherwise keep waking up inside statics whose engine has gone.
	GXLoginFlow::cancel();
}

//-------------------------------------------------------------------------------------------------
/** Initialize resources for the game client */
//-------------------------------------------------------------------------------------------------
void GameClient::init()
{

	setFrameRate(MSEC_PER_LOGICFRAME_REAL);		// from GameCommon.h... tell W3D what our expected framerate is

	INI ini;
	// Load the DrawGroupInfo here, before the Display Manager is loaded.
	ini.loadFileDirectory("Data\\INI\\DrawGroupInfo", INI_LOAD_OVERWRITE, nullptr);

	// Override the ini values with localized versions:
	if (TheGlobalLanguageData && TheGlobalLanguageData->m_drawGroupInfoFont.name.isNotEmpty())
	{
		TheDrawGroupInfo->m_fontName = TheGlobalLanguageData->m_drawGroupInfoFont.name;
		TheDrawGroupInfo->m_fontSize = TheGlobalLanguageData->m_drawGroupInfoFont.size;
		TheDrawGroupInfo->m_fontIsBold = TheGlobalLanguageData->m_drawGroupInfoFont.bold;
	}

	// create the display string factory
	TheDisplayStringManager = createDisplayStringManager();
	if( TheDisplayStringManager )	{
		TheDisplayStringManager->init();
		TheDisplayStringManager->setName("TheDisplayStringManager");
	}

	if (!TheGlobalData->m_headless)
	{
		// create the keyboard
		TheKeyboard = createKeyboard();
		TheKeyboard->init();
		TheKeyboard->setName("TheKeyboard");
	}

	// allocate and load image collection for the GUI and just load the 256x256 ones for now
	TheMappedImageCollection = MSGNEW("GameClientSubsystem") ImageCollection;
	TheMappedImageCollection->load( 512 );

	// now that we have all the images loaded ... load any animation definitions from those images
	TheAnim2DCollection = MSGNEW("GameClientSubsystem") Anim2DCollection;
	TheAnim2DCollection->init();
 	TheAnim2DCollection->setName("TheAnim2DCollection");

	// register message translators
	if( TheMessageStream )
	{

		//
		// NOTE: Make sure m_translators[] is large enough to accommodate all the translators you
		// are loading here.  See MAX_CLIENT_TRANSLATORS
		//

		// since we only allocate one of each, don't bother pooling 'em
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") WindowTranslator,     10 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") MetaEventTranslator,	20 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") HotKeyTranslator,	25 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") PlaceEventTranslator,	30 );
		m_translators[ m_numTranslators++ ] = TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") GUICommandTranslator, 40 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") SelectionTranslator,	50 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") LookAtTranslator,			60 );
		m_translators[ m_numTranslators ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") CommandTranslator,		70 );
		// we keep a pointer to the command translator because it's useful
		m_commandTranslator = (CommandTranslator *)TheMessageStream->findTranslator( m_translators[ m_numTranslators++ ] );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") HintSpyTranslator,		100 );

		//
		// the client message translator should probably remain as the last reaction of the
		// client before the messages are given to the network for processing.  This
		// lets all systems in the client give events that can be processed by the
		// client message translator
		//
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") GameClientMessageDispatcher, 999999999 );

	}

	// create the font library
	TheFontLibrary = createFontLibrary();
	if( TheFontLibrary )
		TheFontLibrary->init();

	// create the mouse
	TheMouse = TheGlobalData->m_headless ? NEW MouseDummy : createMouse();
	TheMouse->parseIni();
	TheMouse->initCursorResources();
 	TheMouse->setName("TheMouse");

	// instantiate the display
	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	TheDisplay = createGameDisplay();
	if( TheDisplay ) {
		TheDisplay->init();
 		TheDisplay->setName("TheDisplay");
	}
	// GX-SIMID-END

	TheHeaderTemplateManager = MSGNEW("GameClientSubsystem") HeaderTemplateManager;
	if(TheHeaderTemplateManager){
		TheHeaderTemplateManager->init();
	}

	// create the window manager
	TheWindowManager = TheGlobalData->m_headless ? NEW GameWindowManagerDummy : createWindowManager();
	if( TheWindowManager )
	{

		TheWindowManager->init();
 		TheWindowManager->setName("TheWindowManager");
//		TheWindowManager->initTestGUI();

	}

	// create the IME manager
	TheIMEManager = CreateIMEManagerInterface();
	if ( TheIMEManager )
	{
		TheIMEManager->init();
 		TheIMEManager->setName("TheIMEManager");
	}

	// create the shell
	TheShell = MSGNEW("GameClientSubsystem") Shell;
	if( TheShell ) {
		TheShell->init();
 		TheShell->setName("TheShell");
	}

	// instantiate the in-game user interface
	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	TheInGameUI = createInGameUI();
	if( TheInGameUI ) {
		TheInGameUI->init();
 		TheInGameUI->setName("TheInGameUI");
	}
	// GX-SIMID-END

	TheHotKeyManager = MSGNEW("GameClientSubsystem") HotKeyManager;
	if( TheHotKeyManager ) {
		TheHotKeyManager->init();
 		TheHotKeyManager->setName("TheHotKeyManager");
	}

	// instantiate the terrain visual display
	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	TheTerrainVisual = createTerrainVisual();
	if( TheTerrainVisual ) {
		TheTerrainVisual->init();
 		TheTerrainVisual->setName("TheTerrainVisual");
	}
	// GX-SIMID-END

	// allocate the ray effects manager
	TheRayEffects = MSGNEW("GameClientSubsystem") RayEffectSystem;
	if( TheRayEffects )	{
		TheRayEffects->init();
 		TheRayEffects->setName("TheRayEffects");
	}

	// set the limits of the mouse now that we've created the display and such
	if( TheMouse )
	{
		// finish initializing the mouse.
		TheMouse->init();
		TheMouse->initCapture();
		TheMouse->setPosition( 0, 0 );
		TheMouse->setMouseLimits();
		TheMouse->setName("TheMouse");
	}

	// create the video player
	TheVideoPlayer = createVideoPlayer();
	if ( TheVideoPlayer )
	{
		TheVideoPlayer->init();
 		TheVideoPlayer->setName("TheVideoPlayer");
	}

	// create the language filter.
	TheLanguageFilter = createLanguageFilter();
	if (TheLanguageFilter)
	{
		TheLanguageFilter->init();
 		TheLanguageFilter->setName("TheLanguageFilter");
	}

	TheCampaignManager = MSGNEW("GameClientSubsystem") CampaignManager;
	TheCampaignManager->init();

	TheEva = MSGNEW("GameClientSubsystem") Eva;
	TheEva->init();
 	TheEva->setName("TheEva");

	TheDisplayStringManager->postProcessLoad();

	m_intro = NEW Intro;

#ifdef PERF_TIMERS
	TheGraphDraw = new GraphDraw;
#endif

}

//-------------------------------------------------------------------------------------------------
/** Reset the game client for a new game */
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::reset()
{
	Drawable *draw, *nextDraw;
	m_drawableHash.clear();
#if USING_STLPORT
	m_drawableHash.resize(DRAWABLE_HASH_SIZE);
#else
	m_drawableHash.reserve(DRAWABLE_HASH_SIZE);
#endif

	// need to reset the in game UI to clear drawables before they are destroyed
	TheInGameUI->reset();

	// the selection translator outlives the game, so drop any hotkey assignment it is still
	// waiting on before the logic frame counter restarts at zero
	if (TheSelectionTranslator)
		TheSelectionTranslator->reset();

	// destroy all Drawables
	for( draw = m_drawableList; draw; draw = nextDraw )
	{
		nextDraw = draw->getNextDrawable();
		destroyDrawable( draw );
	}
	m_drawableList = nullptr;

	TheDisplay->reset();
	TheTerrainVisual->reset();
	TheRayEffects->reset();
	TheVideoPlayer->reset();
	TheEva->reset();

	// clear any drawable TOC we might have
	m_drawableTOC.clear();

	// TheSuperHackers @fix Mauller 13/04/2025 Reset the drawable id so it does not keep growing over the lifetime of the game.
	m_nextDrawableID = (DrawableID)1;

}
// GX-SIMID-END

/** -----------------------------------------------------------------------------------------------
 * Return a new unique object id.
 */
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
DrawableID GameClient::allocDrawableID()
{
	/// @todo Find unused value in current set
	DrawableID ret = m_nextDrawableID;
	m_nextDrawableID = (DrawableID)((UnsignedInt)m_nextDrawableID + 1);
	return ret;
}
// GX-SIMID-END

/** -----------------------------------------------------------------------------------------------
 * Given a drawable, register it with the GameClient and give it a unique ID.
 */
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::registerDrawable( Drawable *draw )
{

	// assign this drawable a unique ID, this will add it to the fast lookup table too
	draw->setID( allocDrawableID() );

	// add the drawable to the master list
	draw->prependToList( &m_drawableList );
	noteDrawableChange();

}
// GX-SIMID-END

/** -----------------------------------------------------------------------------------------------
 * Redraw all views, update the GUI, play sound effects, etc.
 */
DECLARE_PERF_TIMER(GameClient_update)
DECLARE_PERF_TIMER(GameClient_draw)
// GeneralsX: GX_SHOT=<frame>[,<frame>...] takes the same PNG screenshot as the screenshot hotkey
// (TheDisplay->takeScreenShot, the MSG_META_TAKE_SCREENSHOT_PNG path in CommandXlat.cpp) once at each listed logic
// frame of the process's first match; never in the shell, and never in a later match (28/09/2026: the W3D capture
// clock counts from the first match, so a later match's frame N would not show a fresh run's phase at frame N). Files go where the hotkey writes
// them, <user data>/Screenshots/sshot_<local time>.png, so they sort in request order; each capture logs
// "[GXSHOT] n=<k> f=<frame> asked=<frame>". It lets an A/B harness compare two arms' renders of the same
// deterministic scene at the same logic frame on machines where an outside screen grabber has no permission. The
// hotkey's "screenshot saved" message is not shown while GX_SHOT is set (W3D_UpdateScreenshotMessages), so no shot
// contains an earlier one's message. scripts/test/gx-shotdiff pairs and compares the shots.
// Off when GX_SHOT is unset, and on a -headless peer (one log line). Strict parse: 1-16 comma-separated decimal
// frames, each 1..999999999, strictly ascending, nothing else; anything else turns it off and logs the reason once.
// Client-only: it reads the logic frame and game mode and writes nothing the simulation reads.
static Bool gxShotParse( const char *env, UnsignedInt *frames, Int maxFrames, Int *count, const char **why )
{
	*count = 0;
	const char *p = env;
	for (;;)
	{
		if (*count >= maxFrames) { *why = "more than 16 frames"; return FALSE; }
		UnsignedInt v = 0;
		Int digits = 0;
		while (*p >= '0' && *p <= '9')
		{
			if (++digits > 9) { *why = "a frame has more than 9 digits"; return FALSE; }
			v = v * 10 + (UnsignedInt)(*p - '0');
			++p;
		}
		if (digits == 0) { *why = "expected a decimal frame number"; return FALSE; }
		if (v == 0) { *why = "frame 0 is not a game frame"; return FALSE; }
		if (*count > 0 && v <= frames[*count - 1]) { *why = "frames must be strictly ascending"; return FALSE; }
		frames[(*count)++] = v;
		if (*p == '\0')
			return TRUE;
		if (*p != ',') { *why = "unexpected character"; return FALSE; }
		++p;
	}
}

enum { GXSHOT_MAX = 16 };
static Int s_count = -1;	// -1: GX_SHOT not read yet; 0: off
static UnsignedInt s_frames[GXSHOT_MAX];

// GeneralsX: Reads GX_SHOT once; true while it holds an accepted frame list on a peer with a
// display. W3DDisplay asks too, before the first capture: it runs the capture clock only then, so an empty or
// malformed GX_SHOT (screenshots off) leaves the render exactly as a player's.
Bool gxShotCaptureOn( void )
{
	if (s_count < 0)
	{
		s_count = 0;
		const char *env = getenv("GX_SHOT");
		// A -headless peer runs GameClient::update too, and TheDisplay is a real W3DDisplay there, but headless init
		// never creates the D3D device: takeScreenShot would dereference a null device on the first requested frame.
		if (env != NULL && TheGlobalData != NULL && TheGlobalData->m_headless)
		{
			fprintf(stderr, "[GXSHOT] GX_SHOT='%s' ignored: headless peer has no display device - screenshots off\n", env);
			fflush(stderr);
			env = NULL;
		}
		if (env != NULL)
		{
			const char *why = "";
			if (gxShotParse(env, s_frames, GXSHOT_MAX, &s_count, &why))
				fprintf(stderr, "[GXSHOT] armed: %d frame(s) from GX_SHOT='%s'\n", (int)s_count, env);
			else
			{
				s_count = 0;
				fprintf(stderr, "[GXSHOT] GX_SHOT='%s' rejected (%s) - screenshots off\n", env, why);
			}
			fflush(stderr);
		}
	}
	return s_count > 0;
}

// GeneralsX: The GX_SHOT capture clock for render-side animation. It advances once per client
// update (GameClient::update, before anything draws), by the logic frames the MATCH advanced since the previous one:
// rebased to frame 0 on entering the first match, still in the shell and while loading. While gxShotCaptureOn() the
// render sites that stepped by the pacer's fps ratio or the wall clock (tree sway, water, water tracks, shroud fade,
// camera shake, Drawable and InGameUI effects, the police-car light, the cloud texture on roads) and W3DDisplay's WW3D
// clock read it instead, so a capture at a logic frame shows one animation phase however the frames fell into draws
// Without GX_SHOT each helper returns exactly what the site read before. When capture mode
// ends (gxShotCaptureEnd) the wall-clock helpers continue from the capture clock's last value, so no site sees a jump
// between the two clocks' origins for the rest of the match.
static UnsignedInt s_gxClockFrame = 0;	// the match frame the clock has reached
static UnsignedInt s_gxClockSteps = 0;	// logic frames it advanced at this client update
static Bool s_gxClockInMatch = FALSE;
static Bool s_gxClockRebased = FALSE;
static Int s_gxDrawRenders = 0;	// scene renders this update's W3DDisplay::draw completed (gxShotDrawRendered)
static void gxShotCaptureEnd( void );

// GeneralsX: The loop clock: every iteration's time (FramePacer::getUpdateTime, the iteration
// before this one), added once per client update - one per main-loop iteration, drawn or not - before this update draws.
// W3DView::update steps the camera's once-per-draw easing by its advance since the last draw (GameClient/CameraStepPolicy.h):
// under fast forward only 1 iteration in k draws. Not FramePacer's: that file is in the simulation digest.
static double s_gxLoopSeconds = 0.0;

static void gxShotClockTick( void )
{
	if (TheFramePacer != NULL)
		s_gxLoopSeconds += TheFramePacer->getUpdateTime();
	s_gxClockSteps = 0;
	s_gxDrawRenders = 0;
	if (!gxShotCaptureOn() || TheGameLogic == NULL)
		return;
	const Bool inMatch = TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isLoadingMap();
	const UnsignedInt frame = TheGameLogic->getFrame();
	// Rebased once, entering the first match. A later match (or a restart, whose frame goes back) ends capture mode in
	// this same update (gxShotDrive); until then the clock holds still rather than jumping back to that match's frame.
	if (inMatch && !s_gxClockInMatch && !s_gxClockRebased)
	{
		s_gxClockFrame = 0;
		s_gxClockRebased = TRUE;
	}
	s_gxClockInMatch = inMatch;
	if (inMatch && frame > s_gxClockFrame)
	{
		s_gxClockSteps = frame - s_gxClockFrame;
		s_gxClockFrame = frame;
	}
	// The clock steps 0 or 1 per update: the engine runs at most one logic frame per client update
	// (GameEngine::updateBetweenDraws), and a fresh match enters at frame 0 or 1 (logic does not tick while the map
	// loads). The render-side sites rely on it - some are not compositional (a fade's 1 - (1 - s) * n is not
	// (1 - (1 - s))^n, the shroud truncates each step) - so a capture whose state would depend on how frames fell into
	// draws is ended instead of taken; that includes a save loaded at its own frame, which enters with that many
	// steps at once.
	if (s_gxClockSteps > 1)
	{
		fprintf(stderr, "[GXSHOT] %u logic frames in one client update at frame %u (a loaded save, or a skipped update) - "
			"screenshots off: the render-side animations would depend on how frames fell into draws\n",
			(unsigned)s_gxClockSteps, (unsigned)frame);
		fflush(stderr);
		gxShotCaptureEnd();
	}
}

Real gxRenderTimeScale( void )
{
	return gxShotCaptureOn() ? (Real)s_gxClockSteps : TheFramePacer->getActualLogicTimeScaleOverFpsRatio();
}

// GeneralsX: The camera's own easing (W3DView zoomCameraToDesiredHeight, movePivotToGround) steps by
// min(speed * BaseFps / render fps, 1) per draw and STOPS once the remainder is under a threshold, so where it comes to
// rest depended on the render cadence: a constant sub-pixel camera offset for the rest of the match, measured as per-pixel
// noise over 60-68% of an uncapped A-vs-A pair on uneven ground (Mini 2026-09-29, Red Rock). Under capture it steps by
// the same total per logic frame, BaseFps / tick rate, once per logic frame.
Real gxRenderBaseOverUpdateFpsRatio( void )
{
	return gxShotCaptureOn() ? (Real)s_gxClockSteps * (Real)BaseFps / (Real)LOGICFRAMES_PER_SECOND
		: TheFramePacer->getBaseOverUpdateFpsRatio();
}

double gxRenderLoopSeconds( void )
{
	return s_gxLoopSeconds;
}

Real gxRenderStepMs( void )
{
	return gxShotCaptureOn() ? (Real)s_gxClockSteps * MSEC_PER_LOGICFRAME_REAL : TheFramePacer->getLogicTimeStepMilliseconds();
}

// whole milliseconds from the absolute match frame, so a delta never depends on how frames fell into updates
static UnsignedInt gxShotClockMs( void )
{
	return (UnsignedInt)(((unsigned long long)s_gxClockFrame * 1000u) / (UnsignedInt)LOGICFRAMES_PER_SECOND);
}

static UnsignedInt s_gxTimeOffset = 0;	// added to timeGetTime() once capture mode has ended; 0 without GX_SHOT
static UnsignedInt s_gxTickOffset = 0;	// added to GetTickCount() once capture mode has ended; 0 without GX_SHOT

UnsignedInt gxRenderNowMs( void )
{
	return gxShotCaptureOn() ? gxShotClockMs() : (UnsignedInt)timeGetTime() + s_gxTimeOffset;
}

UnsignedInt gxRenderTickMs( void )
{
	return gxShotCaptureOn() ? gxShotClockMs() : (UnsignedInt)GetTickCount() + s_gxTickOffset;
}

// Ends capture mode: gxShotCaptureOn() is false from here, and each wall clock is offset so that it reads the capture
// clock's current value now and advances in real time after (unsigned arithmetic, wrap-safe like the clocks themselves).
static void gxShotCaptureEnd( void )
{
	const UnsignedInt nowMs = gxShotClockMs();
	s_gxTimeOffset = nowMs - (UnsignedInt)timeGetTime();
	s_gxTickOffset = nowMs - (UnsignedInt)GetTickCount();
	s_count = 0;
}

// GeneralsX: W3DDisplay::draw calls this after a completed scene render (WW3D::End_Render on its
// main path), so gxShotDrive knows whether the back buffer it is about to read holds this update's frame.
void gxShotDrawRendered( void )
{
	++s_gxDrawRenders;
}

// GeneralsX: This update's draw did not render the scene exactly once. Nothing rendered - a
// minimised window, iOS rendering suspended, scripted fast time, a TiVO replay frame skip, a failed Begin_Render - while
// the simulation ran on: the draw-side animations (trees, water, tracks, shroud, drawables' own draw) miss those logic
// frames, and a shot due now would read the last rendered back buffer, not its frame. Several renders - W3DDisplay's
// render loop repeats while a script freezes time with a camera move unfinished: every pass applies this update's clock
// step again, so the draw-side state runs ahead of the one logic frame. Neither can be carried over, so capture mode
// ends: the run captures fewer frames than GX_SHOT armed and gx-shotdiff pair refuses it.
// Outside a match (the clock stands still there) it does nothing.
static void gxShotDrawSkipped( void )
{
	if (!gxShotCaptureOn() || !s_gxClockInMatch)
		return;
	if (s_gxDrawRenders == 0)
		fprintf(stderr, "[GXSHOT] a draw was skipped at frame %u (nothing rendered: window minimised, rendering suspended, fast "
			"time, a replay frame skip or a failed Begin_Render) - screenshots off: the rest of the run is not frame-deterministic\n",
			(unsigned)s_gxClockFrame);
	else
		fprintf(stderr, "[GXSHOT] one draw rendered the scene %d times at frame %u (a script froze time during a camera move) - "
			"screenshots off: the draw-side animations stepped once per pass\n", (int)s_gxDrawRenders, (unsigned)s_gxClockFrame);
	fflush(stderr);
	gxShotCaptureEnd();
}

// GeneralsX: TRUE only while gxShotDrive takes a GX_SHOT screenshot (never the hotkey's): GXCAP
// (W3DScreenshot.cpp) marks only that readback as the requested reference picture.
static Bool s_gxShotDriving = FALSE;
Bool gxShotDriving( void )
{
	return s_gxShotDriving;
}

static void gxShotDrive( void )
{
	static Int s_next = 0;
	if (!gxShotCaptureOn() || TheGameLogic == NULL || TheDisplay == NULL)
		return;
	if (s_gxDrawRenders != 1)
	{
		gxShotDrawSkipped();
		return;
	}

	// Only inside a match, and only the first one. The logic frame also ticks in the shell (the menu's background
	// scene) and restarts at every new game - after the shell, or when the frame counter goes backwards without passing
	// through it (a restarted game or replay). A second match turns the shots off rather than taking shots whose
	// animation phase no fresh run would reproduce.
	static Bool s_inMatch = FALSE;
	static Int s_matches = 0;
	static UnsignedInt s_lastFrame = 0;
	const Bool inMatch = TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isLoadingMap();
	if (!inMatch)
	{
		s_inMatch = FALSE;
		return;
	}
	// The match began during this update (a -loadsave or pending save load enters it after the clock ticked at the top
	// of GameClient::update): this draw ran on the shell's clock, so no shot until the next update's tick has seen the
	// match and stepped the clock to its frame.
	if (!s_gxClockInMatch)
		return;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (!s_inMatch || frame < s_lastFrame)
	{
		s_inMatch = TRUE;
		s_next = 0;
		if (++s_matches > 1)
		{
			fprintf(stderr, "[GXSHOT] a second match in this process - screenshots off: the capture clock counts from the first match\n");
			fflush(stderr);
			gxShotCaptureEnd();
			return;
		}
	}
	s_lastFrame = frame;

	if (s_next >= s_count || frame < s_frames[s_next])
		return;
	s_gxShotDriving = TRUE;
	TheDisplay->takeScreenShot(SCREENSHOT_PNG);
	s_gxShotDriving = FALSE;
	fprintf(stderr, "[GXSHOT] n=%d f=%u asked=%u\n", (int)(s_next + 1), (unsigned)frame, (unsigned)s_frames[s_next]);
	++s_next;
	// The last requested shot is queued: capture mode ends here, so the rest of the run (the match, the menus, a
	// hotkey screenshot and its message) behaves as without GX_SHOT; the render clocks carry
	// on from where the capture clock stopped (gxShotCaptureEnd).
	if (s_next >= s_count)
	{
		fprintf(stderr, "[GXSHOT] all %d shot(s) queued - capture mode off\n", (int)s_count);
		gxShotCaptureEnd();
	}
	fflush(stderr);
}

// GeneralsX: -loadsave through the save gate. The gate's Yes button calls this
// again (a message box callback takes no arguments, hence the static), and lets the save through once.
// m_loadSaveGame is what loadQueuedSaveGame reads, and it clears it; it is cleared here too when the gate
// stops the load, so nothing else ever sees a request that was refused or is waiting on the box.
static AsciiString s_queuedSaveGame;
static void loadQueuedSaveThroughGate()
{
	TheWritableGlobalData->m_loadSaveGame = s_queuedSaveGame;
	if (gateSaveLoad(s_queuedSaveGame, loadQueuedSaveThroughGate))
	{
		TheWritableGlobalData->m_loadSaveGame.clear();
		return;
	}
	TheGameState->loadQueuedSaveGame();
	// Its own pre-checks show a message box and stay in the shell; a failed load only sets quitting: say which.
	// A load a guard stopped because it would have crashed stays in the menu instead, as every interactive
	// route does after a failed load (PopupSaveLoad::doLoadGame), so the player can read why.
	if (TheGameEngine->getQuitting() && reportPreventedLoadCrash(s_queuedSaveGame))
	{
		// The failed load left GameLogic's loading flags set, as presentLoadResult says for the other
		// routes; left set, the next match has no quit menu. This route never passes through there.
		fprintf(stderr, "[SAVELOAD] -loadsave '%s' stopped; staying in the menu (left loadingMap=%d loadingSave=%d; cleared)\n",
			s_queuedSaveGame.str(), (Int)TheGameLogic->isLoadingMap(), (Int)TheGameLogic->isLoadingSave());
		TheGameLogic->setLoadingMap(FALSE);
		TheGameLogic->setLoadingSave(FALSE);
		TheGameEngine->setQuitting(FALSE);
		TheShell->showShell(TRUE);
	}
	else if (TheGameEngine->getQuitting())
		fprintf(stderr, "[SAVELOAD] -loadsave '%s' failed to load; the game will close\n", s_queuedSaveGame.str());
	else if (TheGameLogic != nullptr && TheGameLogic->isInGame())
		fprintf(stderr, "[SAVELOAD] loaded '%s'\n", s_queuedSaveGame.str());
	else
		fprintf(stderr, "[SAVELOAD] -loadsave '%s' not loaded; staying in the menu\n", s_queuedSaveGame.str());
}

void GameClient::update()
{
	USE_PERF_TIMER(GameClient_update)
	// GeneralsX: First, so its clock read is as close to the loop's wake as it can be: the
	// render interpolation phase every drawable of this frame is drawn at (GXLogicPhase.h). One sample per update, on
	// purpose: camera tracking, the drawables' client updates, attached emitters and the draw all read this one phase
	// and so agree with each other; a second sample just before the draw would split them (declined).
	// The client work before the draw adds its variable time as latency jitter; measured on the whole path, judder is cv
	// 0.037-0.038 at ~600 fps uncapped (Windows, 10,800 frames) against 2.9-3.9 without interpolation.
	TheDisplay->updateLogicPhase();
	gxShotClockTick();	// GeneralsX: before anything this update animates or draws
	runPendingSaveLoad();	// GeneralsX: a "load it anyway" Yes from the save gate, now that its box is gone
	PROFILER_FRAME_MARK;
	PROFILER_SECTION_COLOR(0x2196F3);
	// create the FRAME_TICK message
	GameMessage *frameMsg = TheMessageStream->appendMessage( GameMessage::MSG_FRAME_TICK );
	frameMsg->appendTimestampArgument( getFrame() );

	// Update intro
	if (m_intro != nullptr)
	{
		m_intro->update();

		if (m_intro->isDone())
		{
			delete m_intro;
			m_intro = nullptr;

			TheShell->showShellMap(TRUE);
			TheShell->showShell();

			if (TheGlobalData->m_loadSaveGame.isNotEmpty())
			{
				// GeneralsX: Through the save gate (SaveLoadFeedback.cpp): a LAN/online
				// save is refused, a load that would crash is stopped, a save from another build asks first.
				s_queuedSaveGame = TheGlobalData->m_loadSaveGame;
				loadQueuedSaveThroughGate();
			}
			// GeneralsX: Guarded on TheRecorder, which upstream omits.
			// ReplayMenu.cpp's own readReplayMapInfo already tests it, so it is reachable as null.
			else if (TheGlobalData->m_loadReplayGame.isNotEmpty() && TheRecorder != nullptr)
			{
				TheRecorder->loadQueuedReplay();
			}
		}
	}

	// update animation 2d collection
	TheAnim2DCollection->UPDATE();

	// update the keyboard
	if( TheKeyboard )
	{
		TheKeyboard->UPDATE();
		TheKeyboard->createStreamMessages();

	}

	// Update the Eva stuff
	TheEva->UPDATE();

	// update the mouse
	if( TheMouse )
	{
		TheMouse->UPDATE();
		TheMouse->createStreamMessages();

	}

	if (m_intro != nullptr)
	{
		// redraw all views, update the GUI
		TheDisplay->UPDATE();
		TheDisplay->DRAW();
		return;
	}

	// update the window system itself
	{
		TheWindowManager->UPDATE();
	}

	// GeneralsX: Consume a finished Discord login.
	//
	// WIRED HERE EVEN THOUGH THIS PRODUCT HAS NO ONLINE LOBBY TO START ONE FROM. GXLoginFlow lives
	// in Core and is compiled into both products, and a module that one product ticks and the
	// other does not is a module whose behaviour depends on which binary you are in - which is
	// exactly how a command comes to exist that starts something nothing ever finishes. Today this
	// call is a single atomic load returning immediately; the day the Generals tree grows a
	// surface that can log in, it already works.
	GXLoginFlow::update();

	// update the video player
	{
		TheVideoPlayer->UPDATE();
	}

	const Bool freezeTime = TheGameEngine->isTimeFrozen() || TheGameEngine->isGameHalted();

	const Int localPlayerIndex = rts::getObservedOrLocalPlayer()->getPlayerIndex();

	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	if (!freezeTime)
	{
		Int numPlayers = ThePlayerList->getPlayerCount();
		Int numNonLocalPlayers = 0;
		Int nonLocalPlayerIndices[MAX_PLAYER_COUNT];

#if ENABLE_CONFIGURABLE_SHROUD
		if (TheGlobalData->m_shroudOn)
#else
		if (true)
#endif
		{
			if (TheGhostObjectManager->trackAllPlayers())
			{
				//Find indices of all active players
				for (Int i=0; i < numPlayers; i++)
				{
					Player *player = ThePlayerList->getNthPlayer(i);
					if (player->getPlayerTemplate() != nullptr && player->getPlayerIndex() != localPlayerIndex)
						nonLocalPlayerIndices[numNonLocalPlayers++] = player->getPlayerIndex();
				}
				//update ghost objects which don't have drawables or objects.
				TheGhostObjectManager->updateOrphanedObjects(nonLocalPlayerIndices, numNonLocalPlayers);
			}
			else
			{
				TheGhostObjectManager->updateOrphanedObjects(nullptr, 0);
			}
		}


		// call the update for all client drawables
		Drawable* draw = firstDrawable();
		while (draw)
		{	// update() could free the Drawable, so go ahead and grab 'next'
			Drawable* next = draw->getNextDrawable();
#if ENABLE_CONFIGURABLE_SHROUD
			if (TheGlobalData->m_shroudOn)
#else
			if (true)
#endif
			{
				//immobile objects need to take snapshots whenever they become fogged
				//so need to refresh their status.  We can't rely on external calls
				//to getShroudStatus() because they are only made for visible on-screen
				//objects.
				Object *object=draw->getObject();
				if (object)
				{
					if (TheGhostObjectManager->trackAllPlayers())
					{
						// TheSuperHackers @info Update the shrouded status for all objects
						// that own a ghost object for all non local players. This is costly.
						if (object->hasGhostObject())
						{
							Int *playerIndex = nonLocalPlayerIndices;
							Int *const playerIndexEnd = nonLocalPlayerIndices + numNonLocalPlayers;
							for (; playerIndex < playerIndexEnd; ++playerIndex)
							{
								object->getShroudedStatus(*playerIndex);
							}
						}
					}

					ObjectShroudStatus ss=object->getShroudedStatus(localPlayerIndex);
					if (ss >= OBJECTSHROUD_FOGGED && draw->getShroudClearFrame() != InvalidShroudClearFrame) {
						UnsignedInt limit = 2*LOGICFRAMES_PER_SECOND;
						if (object->isEffectivelyDead()) {
							// extend the time, so we can see the dead plane blow up & crash.
							limit += 3*LOGICFRAMES_PER_SECOND;
						}
						if (TheGameLogic->getFrame() < limit + draw->getShroudClearFrame()) {
							// It's been less than 2 seconds since we could see them clear, so keep showing them.
							ss = OBJECTSHROUD_CLEAR;
						}
					}
					draw->setFullyObscuredByShroud(ss >= OBJECTSHROUD_FOGGED);
				}
			}
			draw->updateDrawable();
			draw = next;
		}
	}
	// GX-SIMID-END

#if defined(RTS_DEBUG)
	// need to draw the first frame, then don't draw again until TheGlobalData->m_noDraw
	if (TheGlobalData->m_noDraw > TheGameLogic->getFrame() && TheGameLogic->getFrame() > 0)
	{
		gxShotDrawSkipped();	// GeneralsX: no draw: ends a GX_SHOT capture
		return;
	}
#endif

	// update all particle systems
	if( !freezeTime )
	{
		// update particle systems
		TheParticleSystemManager->setLocalPlayerIndex(localPlayerIndex);
		TheParticleSystemManager->update();

	}

	// update the terrain visuals
	{
		// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
		TheTerrainVisual->UPDATE();
		// GX-SIMID-END
	}

	// update display
	{
		TheDisplay->UPDATE();
	}

	{
		USE_PERF_TIMER(GameClient_draw)

	// redraw all views, update the GUI
	//if(TheGameLogic->getFrame() >= 2)

		TheDisplay->DRAW();
	}

	// GeneralsX: GX_SHOT capture, right after this frame was drawn and presented: the image is
	// the state at the logged logic frame. It reads the back buffer after Present, exactly when the screenshot hotkey
	// does (CommandXlat runs in propagateMessages, after this update). D3D leaves a DISCARD back buffer undefined
	// after Present; under DXVK the last rendered image stays there (the Mini's shots show the scene), and the hotkey
	// has always relied on the same.
	gxShotDrive();

	{
		// let display string factory handle its update
		TheDisplayStringManager->update();
	}

	{
		// update the shell
		TheShell->UPDATE();
	}

	{
		// update the in game UI
		TheInGameUI->UPDATE();
	}
}

void GameClient::draw()
{
	if (m_intro != nullptr)
	{
		m_intro->draw();
	}
}

void GameClient::step()
{
	TheDisplay->step();
}

// GeneralsX: GameLogic::update calls this at the start of every logic update: the tick-start
// stamp the render interpolation phase estimates the lockstep grid from (GXLogicPhase.h).
void GameClient::setFrame( UnsignedInt frame )
{
	m_frame = frame;
	if (TheDisplay != nullptr)
		TheDisplay->noteLogicTick(frame);
}

void GameClient::updateHeadless()
{
	// TheSuperHackers @info helmutbuhler 03/05/2025 bobtista 02/02/2026
	// Update particles to prevent accumulation in headless mode. Particles are generated
	// during GameLogic and only cleaned up during rendering. update() lets particles finish
	// their lifecycle naturally instead of abruptly removing them with reset().
	TheParticleSystemManager->update();
}

Bool GameClient::skipCurrentIntroStage()
{
	if (m_intro != nullptr)
	{
		return m_intro->skipCurrentIntroStage();
	}
	return false;
}

Bool GameClient::isMovieAbortRequested()
{
	if (TheGameEngine)
	{
		TheGameEngine->serviceWindowsOS();
	}

	// TheSuperHackers @feature User can skip video by pressing ESC
	if (TheKeyboard)
	{
		TheKeyboard->UPDATE();
		KeyboardIO *io = TheKeyboard->findKey(KEY_ESC, KeyboardIO::STATUS_UNUSED);
		if (io && BitIsSet(io->state, KEY_STATE_UP))
		{
			io->setUsed();
			return TRUE;
		}
	}

	if (TheGameEngine && TheGameEngine->getQuitting())
	{
		return TRUE;
	}

	if (TheMessageStream && TheMessageStream->containsMessageOfType(GameMessage::MSG_META_DEMO_INSTANT_QUIT))
	{
		return TRUE;
	}

	if (TheGameLogic && TheGameLogic->isQuitToDesktopRequested())
	{
		return TRUE;
	}

	return FALSE;
}

/** -----------------------------------------------------------------------------------------------
 * Call the given callback function for each object contained within the given region.
 */
void GameClient::iterateDrawablesInRegion( Region3D *region, GameClientFuncPtr userFunc, void *userData )
{
	Drawable *draw, *nextDrawable;

	for( draw = m_drawableList; draw; draw=nextDrawable )
	{
		nextDrawable = draw->getNextDrawable();

		Coord3D pos = *draw->getPosition();
		if( region == nullptr ||
			  (pos.x >= region->lo.x && pos.x <= region->hi.x &&
			   pos.y >= region->lo.y && pos.y <= region->hi.y &&
				 pos.z >= region->lo.z && pos.z <= region->hi.z) )
		{
			(*userFunc)( draw, userData );
		}
	}
}

/**Helper function to update fake GLA structures to become visible to certain players.
We should only call this during critical moments, such as changing teams, changing to
observer, etc.*/
void GameClient::updateFakeDrawables()
{
}

/** -----------------------------------------------------------------------------------------------
 * Given an object id, return the associated object.
 * For efficiency, a small Least Recently Used cache is incorporated.
 * This method is the primary interface for accessing objects, and should be used
 * instead of pointers to "attach" objects to each other.
 */
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Drawable* GameClient::findDrawableByID( const DrawableID id )
{
	DrawablePtrHashIt it = m_drawableHash.find(id);
	if (it == m_drawableHash.end()) {
		// no such drawable
		return nullptr;
	}

	return (*it).second;
}
// GX-SIMID-END

/** -----------------------------------------------------------------------------------------------
 * Destroy the drawable immediately.
 */
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::destroyDrawable( Drawable *draw )
{

	// remove any notion of the Drawable in the in-game user interface
	TheInGameUI->disregardDrawable( draw );

	// remove from the master list
	draw->removeFromList(&m_drawableList);
	noteDrawableChange();

	//
	// because drawables and objects are tightly coupled, not only MUST we maintain
	// our links in all instances, but it is NECESSARY for the client to actually
	// modify data in the logic, that is the pointer in an object to *this* drawable
	//
	Object *obj = draw->getObject();
	if( obj )
	{

		DEBUG_ASSERTCRASH( obj->getDrawable() == draw, ("Object/Drawable pointer mismatch!") );
		obj->friend_bindToDrawable( nullptr );

	}

	// remove the drawable from our hash of drawables
	removeDrawableFromLookupTable( draw );

	// free storage
	deleteInstance(draw);

}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** Add drawable to lookup table for fast id searching */
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::addDrawableToLookupTable(Drawable *draw )
{

	// sanity
	if( draw == nullptr )
		return;

	// add to lookup
	m_drawableHash[ draw->getID() ] = draw;

}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** Remove drawable from lookup table of fast id searching */
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::removeDrawableFromLookupTable( Drawable *draw )
{

	// sanity
	if( draw == nullptr )
		return;

	// remove from table
	m_drawableHash.erase( draw->getID() );

}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
/** Load a map into the game interface */
Bool GameClient::loadMap( AsciiString mapName )
{

	// sanity
	if( mapName.isEmpty() )
		return false;

	assert( 0 );  // who calls this?

	return TRUE;

}

//-------------------------------------------------------------------------------------------------
/** Unload a map from the game interface */
void GameClient::unloadMap( AsciiString mapName )
{

	assert( 0 );  // who calls this?

}

//-------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::setTimeOfDay( TimeOfDay tod )
{
	Drawable *draw = firstDrawable();

	while( draw )
	{
		draw->setTimeOfDay( tod );

		draw = draw->getNextDrawable();
	}
}
// GX-SIMID-END

//-------------------------------------------------------------------------------------------------
void GameClient::assignSelectedDrawablesToGroup( Int group )
{
/*
	Drawable *draw = firstDrawable();
	while( draw )
	{

		if( draw->isSelected() && draw->getObject()->isLocallyControlled())
		{
			draw->setDrawableGroup( group );
		}
		else if( draw->getDrawableGroup() == group )
		{
			draw->setDrawableGroup( 0 );
		}

		draw = draw->getNextDrawable();

	}
*/
}

//-------------------------------------------------------------------------------------------------
void GameClient::selectDrawablesInGroup( Int group )
{
/*
	Drawable *draw = firstDrawable();

	// create a message that will assign a group ID to all the selected drawables, this
	// way when we do things with this current selected group of units, we only have
	// to refer to the group ID and not each individual object ID
	GameMessage *teamMsg = TheMessageStream->appendMessage( GameMessage::MSG_CREATE_SELECTED_GROUP );

	//We are creating a new group from scratch.
	teamMsg->appendBooleanArgument( true );

	while( draw )
	{
		int counter = 0;

		const Object *object = draw->getObject();

		if( object && draw->getDrawableGroup() == group && object->isLocallyControlled() && !object->isContained() )
		{
			//Only select the object if it is locally controlled and not contained by anything.
			TheInGameUI->selectDrawable(draw);
			teamMsg->appendObjectIDArgument( draw->getObject()->getID() );
		}
		else
		{
			TheInGameUI->deselectDrawable(draw);
		}

		draw = draw->getNextDrawable();
		counter++;
	}

*/
}

// ------------------------------------------------------------------------------------------------
void GameClient::addTextBearingDrawable( Drawable *tbd )
{
	if ( tbd != nullptr )
		m_textBearingDrawableList.push_back( tbd );
}
// ------------------------------------------------------------------------------------------------
void GameClient::flushTextBearingDrawables()
{

	/////////////////////////////
	// WALK THIS LIST AND CALL EACH DRAWABLES TEXTY STUFF
	/////////////////////////////

	for( TextBearingDrawableListIterator it = m_textBearingDrawableList.begin(); it != m_textBearingDrawableList.end(); ++it )
	  (*it)->drawUIText();

	m_textBearingDrawableList.clear();
}

// ------------------------------------------------------------------------------------------------
GameMessage::Type GameClient::evaluateContextCommand( Drawable *draw,
																											const Coord3D *pos,
																											CommandTranslator::CommandEvaluateType cmdType )
{

	if( m_commandTranslator )
		return m_commandTranslator->evaluateContextCommand( draw, pos, cmdType );
	else
		return GameMessage::MSG_INVALID;

}

//-------------------------------------------------------------------------------------------------
/** Get the ray effect data for a drawable */
void GameClient::getRayEffectData( Drawable *draw, RayEffectData *effectData )
{

	TheRayEffects->getRayEffectData( draw, effectData );

}

//-------------------------------------------------------------------------------------------------
/** remove the drawable from the ray effects system if present */
void GameClient::removeFromRayEffects( Drawable *draw )
{

	TheRayEffects->deleteRayEffect( draw );

}

/** frees all shadow resources used by this module - used by Options screen.*/
void GameClient::releaseShadows()
{
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->releaseShadows();
}

/** create shadow resources if not already present. Used by Options screen.*/
void GameClient::allocateShadows()
{
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->allocateShadows();
}

//-------------------------------------------------------------------------------------------------
/** Preload assets for the currently loaded map.  Those assets include all the damage states
	* for every building loaded, as well as any faction units/structures we can build and
	* all their damage states */
//-------------------------------------------------------------------------------------------------
void GameClient::preloadAssets( TimeOfDay timeOfDay )
{

	MEMORYSTATUS before, after;
	GlobalMemoryStatus(&before);

	// first, for every drawable in the map load the assets for all states we care about
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->preloadAssets( timeOfDay );

	//
	// now create a temporary drawable for each of the faction things we can create, preload
	// their assets, and dump the drawable
	//
	AsciiString side;
	const ThingTemplate *tTemplate;
	for( tTemplate = TheThingFactory->firstTemplate();
			 tTemplate;
			 tTemplate = tTemplate->friend_getNextTemplate() )
	{

		// if this isn't one of the objects that can be preloaded ignore it
		if( tTemplate->isKindOf( KINDOF_PRELOAD ) == FALSE && !TheGlobalData->m_preloadEverything )
			continue;

		// create the drawable and do the preloading
		draw = TheThingFactory->newDrawable( tTemplate );
		if( draw )
		{

			// preload the assets
			draw->preloadAssets( timeOfDay );

			// destroy the drawable
			destroyDrawable( draw );

		}

	}
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));
	/*
	DEBUG_LOG(("Preloading memory dwLength        %d --> %d : %d",
		before.dwLength, after.dwLength, before.dwLength - after.dwLength));
	DEBUG_LOG(("Preloading memory dwMemoryLoad    %d --> %d : %d",
		before.dwMemoryLoad, after.dwMemoryLoad, before.dwMemoryLoad - after.dwMemoryLoad));
	DEBUG_LOG(("Preloading memory dwTotalPageFile %d --> %d : %d",
		before.dwTotalPageFile, after.dwTotalPageFile, before.dwTotalPageFile - after.dwTotalPageFile));
	DEBUG_LOG(("Preloading memory dwTotalPhys     %d --> %d : %d",
		before.dwTotalPhys , after.dwTotalPhys, before.dwTotalPhys - after.dwTotalPhys));
	DEBUG_LOG(("Preloading memory dwTotalVirtual  %d --> %d : %d",
		before.dwTotalVirtual , after.dwTotalVirtual, before.dwTotalVirtual - after.dwTotalVirtual));
	*/

	GlobalMemoryStatus(&before);
	extern std::vector<AsciiString>	debrisModelNamesGlobalHack;
	size_t i=0;
	for (; i<debrisModelNamesGlobalHack.size(); ++i)
	{
		TheDisplay->preloadModelAssets(debrisModelNamesGlobalHack[i]);
	}
	GlobalMemoryStatus(&after);
	debrisModelNamesGlobalHack.clear();

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

	TheControlBar->preloadAssets( timeOfDay );

	GlobalMemoryStatus(&before);
	TheParticleSystemManager->preloadAssets( timeOfDay );
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

	const char *const textureNames[] = {
		"ptspruce01.tga",
		"exrktflame.tga",
		"cvlimo3_d2.tga",
		"exfthrowerstream.tga",
		"uvrockbug_d1.tga",
		"arcbackgroundc.tga",
		"grade3.tga",
		"framebasec.tga",
		"gradec.tga",
		"frametopc.tga",
		"arcbackgrounda.tga",
		"arcglow2.tga",
		"framebasea.tga",
		"gradea.tga",
		"frametopa.tga",
		"sauserinterface256_002.tga",
		"sauserinterface256_001.tga",
		"unitbackgrounda.tga",
		"sauserinterface256_004.tga",
		"sagentank.tga",
		"sauserinterface256_005.tga",
		"sagenair.tga",
		"sauserinterface256_003.tga",
		"sagenspec.tga",
		"snuserinterface256_003.tga",
		"snuserinterface256_002.tga",
		"unitbackgroundc.tga",
		"snuserinterface256_004.tga",
		"sngenredarm.tga",
		"snuserinterface256_001.tga",
		"sngenspewea.tga",
		"sngensecpol.tga",
		"ciburn.tga",
		"ptmaple02.tga",
		"scuserinterface256_005.tga",
		"scuserinterface256_002.tga",
		"sauserinterface256_006.tga",
		"pmcrates.tga",
		""
	};

	GlobalMemoryStatus(&before);
	for (i=0; *textureNames[i]; ++i)
		TheDisplay->preloadTextureAssets(textureNames[i]);
	GlobalMemoryStatus(&after);

	DEBUG_LOG(("Preloading memory dwAvailPageFile %d --> %d : %d",
		before.dwAvailPageFile, after.dwAvailPageFile, before.dwAvailPageFile - after.dwAvailPageFile));
	DEBUG_LOG(("Preloading memory dwAvailPhys     %d --> %d : %d",
		before.dwAvailPhys, after.dwAvailPhys, before.dwAvailPhys - after.dwAvailPhys));
	DEBUG_LOG(("Preloading memory dwAvailVirtual  %d --> %d : %d",
		before.dwAvailVirtual, after.dwAvailVirtual, before.dwAvailVirtual - after.dwAvailVirtual));

//	preloadTextureNamesGlobalHack2 = preloadTextureNamesGlobalHack;
//	preloadTextureNamesGlobalHack.clear();

}

// ------------------------------------------------------------------------------------------------
/** Given a string name, find the drawable TOC entry (if any) associated with it */
// ------------------------------------------------------------------------------------------------
GameClient::DrawableTOCEntry *GameClient::findTOCEntryByName( AsciiString name )
{

	for( DrawableTOCListIterator it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		if( (*it).name == name )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Given a drawable TOC identifier, find the drawable TOC if any */
// ------------------------------------------------------------------------------------------------
GameClient::DrawableTOCEntry *GameClient::findTOCEntryById( UnsignedShort id )
{

	for( DrawableTOCListIterator it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		if( (*it).id == id )
			return &(*it);

	return nullptr;

}

// ------------------------------------------------------------------------------------------------
/** Add an drawable TOC entry */
// ------------------------------------------------------------------------------------------------
void GameClient::addTOCEntry( AsciiString name, UnsignedShort id )
{

	DrawableTOCEntry tocEntry;
	tocEntry.name = name;
	tocEntry.id = id;
	m_drawableTOC.push_back( tocEntry );

}

// ------------------------------------------------------------------------------------------------
static Bool shouldSaveDrawable(const Drawable* draw)
{
	if (draw->testDrawableStatus(DRAWABLE_STATUS_NO_SAVE))
	{
		if (draw->getObject() == nullptr)
		{
			return false;
		}
		else
		{
			DEBUG_CRASH(("You should not ever set DRAWABLE_STATUS_NO_SAVE for a Drawable with an object. (%s)",draw->getTemplate()->getName().str()));
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
/** Xfer drawable table of contents */
// ------------------------------------------------------------------------------------------------
void GameClient::xferDrawableTOC( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// clear our current table of contents
	m_drawableTOC.clear();

	// xfer the table
	UnsignedInt tocCount = 0;
	if( xfer->getXferMode() == XFER_SAVE )
	{
		AsciiString templateName;

		// generate a new TOC based on the drawables that are in the map
		for( Drawable *draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		{
			if (!shouldSaveDrawable(draw))
				continue;

			// get the name we're working with
			templateName = draw->getTemplate()->getName();

			// if is this drawable name already in the TOC, skip it
			if( findTOCEntryByName( templateName ) != nullptr )
				continue;

			// add this entry to the TOC
			addTOCEntry( draw->getTemplate()->getName(), ++tocCount );

		}

		// xfer entries in the TOC
		xfer->xferUnsignedInt( &tocCount );

		// xfer each TOC entry
		DrawableTOCListIterator it;
		DrawableTOCEntry *tocEntry;
		for( it = m_drawableTOC.begin(); it != m_drawableTOC.end(); ++it )
		{

			// get this toc entry
			tocEntry = &(*it);

			// xfer the name
			xfer->xferAsciiString( &tocEntry->name );

			// xfer the paired id
			xfer->xferUnsignedShort( &tocEntry->id );

		}

	}
	else
	{
		AsciiString templateName;
		UnsignedShort id;

		// how many entries are we going to read
		xfer->xferUnsignedInt( &tocCount );

		// read all the entries
		for( UnsignedInt i = 0; i < tocCount; ++i )
		{

			// read the name
			xfer->xferAsciiString( &templateName );

			// read the id
			xfer->xferUnsignedShort( &id );

			// add this to the TOC
			addTOCEntry( templateName, id );

		}

	}

}

// ------------------------------------------------------------------------------------------------
/** Xfer method for Game Client
  * Version History:
  * 1: Initial
  * 2: Adding mission briefing history
	* 3: Added block markers around drawable data, no version checking is done and therefore
	*		 this version breaks compatibility with previous versions. (CBD)
 */
// ------------------------------------------------------------------------------------------------
void GameClient::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 4;	// GeneralsX: 4 = the skybox script latch
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// client frame number
	xfer->xferUnsignedInt( &m_frame );

	//
	// note that we do not do the id counter here, we did it in the game state block because
	// it's important to do that part very early in the load process
	//
	// !!!DON'T DO THIS!!! ----> xfer->xferDrawableID( &m_nextDrawableID ); <---- !!!DON'T DO THIS!!!

	//
	// xfer a table of contents that contain thing template and identifier pairs.  this
	// table of contents is good for this save file only as unique numbers are only
	// generated and stored for the actual things that are on this map
	//
	xferDrawableTOC( xfer );

	// drawable count
	Drawable *draw;
	UnsignedShort drawableCount = 0;
	for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
	{
		if (xfer->getXferMode() == XFER_SAVE && !shouldSaveDrawable(draw))
			continue;
		drawableCount++;
	}
	xfer->xferUnsignedShort( &drawableCount );

	// drawable data
	DrawableTOCEntry *tocEntry;
	ObjectID objectID;
	if( xfer->getXferMode() == XFER_SAVE )
	{

		// iterate all drawables
		for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		{
			if (!shouldSaveDrawable(draw))
				continue;

			// get TOC entry for this drawable
			tocEntry = findTOCEntryByName( draw->getTemplate()->getName() );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - Drawable TOC entry not found for '%s'", draw->getTemplate()->getName().str() ));
				throw SC_INVALID_DATA;

			}

			// xfer toc id entry
			xfer->xferUnsignedShort( &tocEntry->id );

			// begin data block
			xfer->beginBlock();

			// write the object ID this drawable is attached to
			objectID = draw->getObject() ? draw->getObject()->getID() : INVALID_ID;
			xfer->xferObjectID( &objectID );

			// write snapshot data
			xfer->xferSnapshot( draw );

			// end data block
			xfer->endBlock();

		}

	}
	else
	{
		UnsignedShort tocID;
		const ThingTemplate *thingTemplate;
		Int dataSize;

		// read all entries
		for( UnsignedShort i = 0; i < drawableCount; ++i )
		{

			// read toc id entry
			xfer->xferUnsignedShort( &tocID );

			// find TOC entry with this identifier
			tocEntry = findTOCEntryById( tocID );
			if( tocEntry == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - No TOC entry match for id '%d'", tocID ));
				throw SC_INVALID_DATA;

			}

			// read data block size
			dataSize = xfer->beginBlock();

			// find matching thing template
			thingTemplate = TheThingFactory->findTemplate( tocEntry->name );
			if( thingTemplate == nullptr )
			{

				DEBUG_CRASH(( "GameClient::xfer - Unrecognized thing template '%s', skipping.  ENGINEERS - Are you *sure* it's OK to be ignoring this object from the save file???  Think hard about it!",
											tocEntry->name.str() ));
				xfer->skip( dataSize );
				continue;

			}

			// read the object ID this drawable is attached to (if any)
			xfer->xferObjectID( &objectID );

			//
			// if we have an attached object ID, we won't create a new drawable, we'll use the
			// one that has been created and attached to the object already
			//
			if( objectID != INVALID_ID )
			{
				Object *object = TheGameLogic->findObjectByID( objectID );

				// sanity
				if( object == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - Cannot find object '%d' that is supposed to be attached to this drawable '%s'",
												objectID, thingTemplate->getName().str() ));
					throw SC_INVALID_DATA;

				}

				// get the drawable from the object
				draw = object->getDrawable();
				if( draw == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - There is no drawable attached to the object '%s' (%d) and there should be",
												object->getTemplate()->getName().str(), object->getID() ));
					throw SC_INVALID_DATA;

				}

				// srj sez: some objects (eg, diguised bombtrucks) may have an "abnormal" drawable. so check.
				//
				// note carefully: we do NOT want to use isEquivalentTo() here, because different object reskins
				// SHOULD count as different templates for our purposes here (which are purely visual). however, we
				// do need to compare getFinalOverride, because draw->getTemplate() is always gonna return the final
				// override, while TheThingFactory->findTemplate does not.
				//
				const ThingTemplate* drawTemplate = draw->getTemplate();
				if (drawTemplate->getFinalOverride() != thingTemplate->getFinalOverride())
				{
					destroyDrawable( draw );
					draw = TheThingFactory->newDrawable( thingTemplate );
					TheGameLogic->bindObjectAndDrawable(object, draw);
				}

			}
			else
			{

				//
				// there was no object attached to this drawable when we saved, we need to create a
				// whole brand new drawable now
				//
				draw = TheThingFactory->newDrawable( thingTemplate );

				// sanity
				if( draw == nullptr )
				{

					DEBUG_CRASH(( "GameClient::xfer - Unable to create drawable for '%s'",
												thingTemplate->getName().str() ));
					throw SC_INVALID_DATA;

				}

			}

			// xfer the drawable data
			xfer->xferSnapshot( draw );

			// end block (not necessary since this is a no-op but symettrically nice)
			xfer->endBlock();

		}

	}

	// xfer the in-game mission briefing history list
	if (version >= 2)
	{
		if( xfer->getXferMode() == XFER_SAVE )
		{
			BriefingList *bList = GetBriefingTextList();
			Int numEntries = bList->size();
			xfer->xferInt(&numEntries);
			DEBUG_LOG(("Saving %d briefing lines", numEntries));
			for (BriefingList::const_iterator bIt = bList->begin(); bIt != bList->end(); ++bIt)
			{
				AsciiString tempStr = *bIt;
				DEBUG_LOG(("'%s'", tempStr.str()));
				xfer->xferAsciiString(&tempStr);
			}
		}
		else // XFER_LOAD
		{
			Int numEntries = 0;
			xfer->xferInt(&numEntries);
			DEBUG_LOG(("Loading %d briefing lines", numEntries));
			UpdateDiplomacyBriefingText(AsciiString::TheEmptyString, TRUE); // clear out briefing list first
			while (numEntries-- > 0)
			{
				AsciiString tempStr;
				xfer->xferAsciiString(&tempStr);
				DEBUG_LOG(("'%s'", tempStr.str()));
				UpdateDiplomacyBriefingText(tempStr, FALSE);
			}
		}
	}


	// GeneralsX @bugfix skybox-1 12/10/2026 (skybox r10 P2): a map script that switched the sky off keeps it off after a
	// load (W3DWater.cpp's latch; the engine reset before the load cleared it). Version 3 saves read as "not switched off".
	if (version >= 4)
	{
		Bool skyOff = gxSkyScriptOff();
		xfer->xferBool( &skyOff );
		if (xfer->getXferMode() == XFER_LOAD)
			gxSetSkyScriptOff( skyOff );
	}
}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void GameClient::loadPostProcess()
{

	//
	// due to the fact that during the load process we have called newDrawable for drawables
	// without objects, and then overwrote their ids with data from the save file, our allocator
	// id may be far higher than it needs to be.  We'll pull it back down as low as we can
	//
	Drawable *draw;
	for( draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
		if( draw->getID() >= m_nextDrawableID )
			m_nextDrawableID = (DrawableID)((UnsignedInt)draw->getID() + 1);

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void GameClient::crc( Xfer *xfer )
{

}
