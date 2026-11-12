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

// FILE: GameClient.cpp ////////////////////////////////////////////////////
// Implementation of GameClient singleton
// Author: Michael S. Booth, March 2001
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <stdexcept>
#include <cstdio>
#include <algorithm>	// GX_TEST_CHEAT_SCRIPT: std::sort, std::min
#include <chrono>	// GX_REPLAY_PAUSE: the held window's wall clock
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "GameClient/GameClient.h"
#include "GameClient/GXOrderDriver.h"
#include "GameClient/GXGym.h"	// GeneralsX: ai-gym
#include "GameClient/GXMassOrder.h"
#include "GameClient/GXRenderSplit.h"	// GeneralsX: render-split step 1
#include "GameClient/GXPosePublish.h"	// GeneralsX: render-split step 4

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/ActionManager.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/Recorder.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "GameClient/GXNoShroud.h"
#include "GameClient/GXDebugIntelGate.h"	// GeneralsX: GX_CAM: who may have the camera find other players' armies
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "GameClient/Anim2D.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/DebugFloatButton.h"
#include "GameClient/Diplomacy.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Drawable.h"
#include "GameClient/MissileVisual.h"
#include "GameClient/DrawGroupInfo.h"
#include "GameClient/Eva.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/GXGraphicsSettings.h"
#include "GameClient/GXPerfReport.h"
#include "GameClient/GXReplayDiag.h"
#include "GameClient/GXGfxProbe.h"		// [GXGFX] render split
#include <stdarg.h>							// gxGfxAppend
#include "Common/GameLOD.h"				// [GXGFX] lod=
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
#include "GameClient/Snow.h"
#include "GameClient/TerrainVisual.h"
#include "GameClient/View.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/WindowXlat.h"
#include "Common/FramePacer.h"			// [GXPACE] probe: getLastHoldTime
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"	// the test quit hook: surrender is refused after a victory, as the menu refuses it
// GeneralsX: Discord Rich Presence. Outside the SimID digest, like
// everything else under GameClient - see GXDiscord.h.
#include "GameClient/GXDiscord.h"
#include "GameClient/GXLoginFlow.h"
#include "GameClient/GUICallbacks.h"	// GXBuddyOverlayPump
#include "GameClient/GXMatchChat.h"
#include "GameClient/GXKickPump.h"	// GeneralsX: the in-match kick's victim half
#include "GameClient/GXVoteKick.h"	// GeneralsX: the vote kick's client half
#include "GameNetwork/GameInfo.h"	// the test save hook: TheSkirmishGameInfo
#include "GameNetwork/NetworkInterface.h"	// [GXPACE] probe: isStalling/getRunAhead/getFrameRate
#include "GameLogic/GhostObject.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"	// GX_TEST_REVEAL_SCRIPT's probe
#include "GameLogic/TerrainLogic.h"		// GX_TEST_CHEAT_SCRIPT's spawn point
#if defined(GX_TEST_INSTRUMENTS)
#include "GameLogic/Module/AIUpdate.h"		// GX_TEST_ENTERQ's probe
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#endif
#include "GameLogic/ScriptEngine.h"		// For TheScriptEngine - jkmcd
// GeneralsX: The replay strip (InGameUI.cpp, GXReplayStrip.inl): its clicks, and the seek that runs between frames.
extern GameMessageTranslator *gxNewReplayStripTranslator();
extern void gxReplayStripUpdate();
#include "../../GameEngineDevice/Include/W3DDevice/GameClient/Module/W3DModelDraw.h"	// preloadAssets: the models of a template
#include "Common/RandomValue.h"	// [GXPRELOAD]: the random streams' state

// GeneralsX: Defined in InGameUI.cpp (GXSpectatorHud.inl).
extern GameMessageTranslator *gxNewSpectatorHudTranslator();
extern GameMessageTranslator *gxNewSpectatorHudBoardGate();
extern GameMessageTranslator *gxNewSpectatorHudOrderGate();
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

	delete TheChallengeGenerals;
	TheChallengeGenerals = nullptr;

	// delete the shell
	delete TheShell;
	TheShell = nullptr;

	delete TheIMEManager;
	TheIMEManager = nullptr;

	// GeneralsX: Drop the floating Debug overlay while its window manager
	// is still alive, otherwise its file statics outlive the windows they point at.
	DebugFloatButtonDestroy();
	GXDiscordDestroy();
	// GeneralsX: Abandon any login still polling. The worker thread is
	// detached, so without this it would keep waking up inside statics whose engine has gone.
	GXLoginFlow::cancel();

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

	delete TheSnowManager;
	TheSnowManager = nullptr;

}

static void gxNoShroud( void );		// defined above update(); see the GX_NOSHROUD note there

// GeneralsX: GX_REMASTERED_PROBE (GXGraphicsSettings.h): lets a run with no
// keyboard - a headless peer, the Linux VPS - prove GXGraphics.ini persists. =1 reads the value and logs it
// ([GXREMASTER]); =flip also flips it and writes the file, as the chord does. The value is render-only.
static void gxRemasteredProbe( void )
{
	const char *env = getenv( "GX_REMASTERED_PROBE" );
	if( env == nullptr || ( atoi( env ) <= 0 && strcmp( env, "flip" ) != 0 ) )
		return;
	const Bool on = GXGraphics::isRemastered();
	if( strcmp( env, "flip" ) == 0 )
		GXGraphics::setRemastered( !on );
	fflush( stderr );
}

//-------------------------------------------------------------------------------------------------
/** Initialize resources for the game client */
//-------------------------------------------------------------------------------------------------
void GameClient::init()
{

	setFrameRate(MSEC_PER_LOGICFRAME_REAL);		// from GameCommon.h... tell W3D what our expected framerate is

	// GeneralsX: The perf reporter's socket, bound HERE and never from
	// update(). UDP::Bind fflush(stderr)es on failure and udp.cpp is inside the simulation digest,
	// so the only way that flush stays off the lockstep frame path is for the bind never to be
	// reached from one. tools/relay/test-perf.js [B2-4]/[B2-5] are the standing scan.
	gxPerfOpenSocket();

	// GX_NOSHROUD: before TheDisplay is created below, so W3DScene and the terrain build shroudless.
	gxNoShroud();
	gxRemasteredProbe();

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
		// GeneralsX: The vote-kick banner's clicks and taps (GXVoteKickHud.inl), BEFORE the window system:
		// the WindowTranslator eats every click while a map script disables input, and a ballot must still get through then.
		// Only a click on the banner itself is taken (never an order to the units under it); the banner steps aside under the
		// quit menu so no menu click is taken. Passes everything else through.
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( gxNewVoteKickTranslator(),     9 );
		// GeneralsX: The open player Tab board owns the mouse over itself, ahead of the windows
		// Passes everything when no board is up.
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( gxNewSpectatorHudBoardGate(),     9 );
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( MSGNEW("GameClientSubsystem") WindowTranslator,     10 );
	// GeneralsX: The replay strip draws over the spectator HUD, so it takes the mouse first: the same
	// priority as the HUD below, attached ahead of it (equal priorities keep attach order). After the windows, so chat still wins.
	m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( gxNewReplayStripTranslator(),     11 );
		// GeneralsX: The GeneralsX spectator HUD's clicks, drags and keys (InGameUI.cpp,
		// GXSpectatorHud.inl): after the windows, so a focused chat box or menu still wins; before the meta map, so a
		// spectator's Tab opens the scoreboard and not Diplomacy. Passes everything through unless that HUD is up.
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( gxNewSpectatorHudTranslator(),     11 );
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
		// GeneralsX: A spectator's selection orders stop here (GXSpectatorHud.inl OrderGate): after every
		// translator that can make one, before the dispatcher hands the stream to the network. Passes everything else.
		m_translators[ m_numTranslators++ ] =	TheMessageStream->attachTranslator( gxNewSpectatorHudOrderGate(),		990 );

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
		try {
			TheDisplay->init();
		} catch (const std::exception& e) {
			fprintf(stderr, "ERROR: TheDisplay->init() threw exception: %s\n", e.what());
			throw;
		} catch (...) {
			fprintf(stderr, "ERROR: TheDisplay->init() threw unknown exception\n");
			throw;
		}
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

 	TheChallengeGenerals = createChallengeGenerals();
 	if( TheChallengeGenerals ) {
 		TheChallengeGenerals->init();
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

	TheSnowManager = createSnowManager();
	if (TheSnowManager)
	{
		TheSnowManager->init();
		TheSnowManager->setName("TheSnowManager");
	}

	m_intro = NEW Intro;

	// GeneralsX: Floating Debug button. This position is load-bearing:
	// TheDisplay, TheFontLibrary, TheMappedImageCollection and TheWindowManager are all up by now,
	// and GameClient::init runs after GameEngine::init's verifyNameKeyID(2265) checkpoint, so the two
	// name keys minted here cannot shift any CRC-relevant ordinal. Do not hoist it earlier.
	DebugFloatButtonCreate();
	GXDiscordCreate();

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
	missileVisualShockReset();	// the nuke shockwave records name particle systems of the game that is ending

//	m_drawableHash.clear();
//	m_drawableHash.resize(DRAWABLE_HASH_SIZE);

	m_drawableVector.clear();
	m_drawableVector.resize(DRAWABLE_HASH_SIZE, nullptr);

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
	if (TheSnowManager)
		TheSnowManager->reset();

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
// ---------------------------------------------------------------------------
// GeneralsX: Display::draw, timed for the replay timeline's draw_ms (GXReplayDiag.h).
static void gxDrawTimed( void )
{
	const long long t0 = gxPerfMonoNs();
	TheDisplay->DRAW();
	gxReplayDiagDrawSpan( gxPerfMonoNs() - t0 );
}

// GeneralsX: [GXPACE], a 1 Hz frame-pacing probe, off unless GX_PACE is set (read once). It keeps its own clock:
// TheFramePacer->getUpdateTime() is frozen in headless -lanhost/-lanjoin runs and getAverageFPS() is constant under
// -headless. Own tag, not folded into [GXCRC] (harnesses diff the .crc streams across peers); 1 Hz because the iOS
// stderr sink is capped at 8 MB.
static Bool gxPaceSample( void )	// TRUE when it printed: [GXGFX] closes its window on the same iteration
{
	static const Bool s_enabled = (getenv("GX_PACE") != NULL);
	if (!s_enabled)
		return FALSE;

	// GeneralsX: the same native #ifdef pair was written out here and in
	// GXPerfReport.cpp. One definition now, in GXPerfReport.h (already included above), so the two
	// probes cannot drift apart about what a second is - which this file's own note required and
	// only copy-paste enforced. See gxPerfMonoNs for why std::chrono rather than the device layer.
	const int64_t now = (int64_t)gxPerfMonoNs();

	static int64_t     s_windowStart      = 0;
	static int64_t     s_lastIter         = 0;
	static UnsignedInt s_windowStartFrame = 0;
	static UnsignedInt s_lastLogicFrame   = 0;
	static UnsignedInt s_iters            = 0;
	static int64_t     s_stallNs          = 0;
	static int64_t     s_holdBaseNs       = 0; // limiter's cumulative hold at window start
	static Bool        s_frameReset       = FALSE; // logic counter went backwards inside this window
	static int64_t     s_lastHoldNs       = 0; // limiter's cumulative hold at the previous iteration
	static UnsignedInt s_cuBase           = 0; // tick catch-up's logic-only passes at window start (cu=)

	const UnsignedInt logicFrame = (TheGameLogic != NULL) ? TheGameLogic->getFrame() : 0;

	if (s_windowStart == 0)
	{
		s_windowStart      = now;
		s_lastIter         = now;
		s_windowStartFrame = logicFrame;
		s_lastLogicFrame   = logicFrame;
		s_holdBaseNs       = (TheFramePacer != NULL) ? (int64_t)TheFramePacer->getTotalHoldNs() : 0;
		s_lastHoldNs       = s_holdBaseNs;
		s_cuBase           = (TheFramePacer != NULL) ? TheFramePacer->getCatchUpPasses() : 0;
		return FALSE;
	}

	const int64_t sinceLastIter = now - s_lastIter;
	s_lastIter = now;
	++s_iters;

	// hold_ms is a measurement: FrameRateLimit::getTotalHoldNs is cumulative and read as a per-window delta, since the
	// pacer sleeps at the end of an advancing iteration. stall_ms counts only iterations where the frame did not
	// advance. The two must be disjoint: sinceLastIter already contains the previous limiter sleep.
	const int64_t holdNowNs   = (TheFramePacer != NULL) ? (int64_t)TheFramePacer->getTotalHoldNs() : 0;
	const int64_t holdDeltaNs = holdNowNs - s_lastHoldNs;
	s_lastHoldNs = holdNowNs;

	if (logicFrame == s_lastLogicFrame)
	{
		if (TheNetwork != NULL && TheNetwork->isStalling())
		{
			const int64_t blockedNs = sinceLastIter - holdDeltaNs;
			if (blockedNs > 0)
				s_stallNs += blockedNs;
		}
	}

	// Catch the reset WHERE IT HAPPENS. Comparing only the window endpoints misses a counter that
	// resets and then climbs back past s_windowStartFrame before the window is emitted - that
	// window would report a plausible, and wrong, rate. This sampler sees every iteration, so the
	// reset cannot hide from an iteration-level check.
	if (logicFrame < s_lastLogicFrame)
		s_frameReset = TRUE;

	s_lastLogicFrame = logicFrame;

	const int64_t windowNs = now - s_windowStart;
	if (windowNs < 1000000000LL)
		return FALSE;

	const double  secs        = (double)windowNs / 1000000000.0;
	const double  itersPerSec = (secs > 0.0) ? (double)s_iters / secs : 0.0;
	const double  engineFps   = (TheDisplay != NULL) ? (double)TheDisplay->getAverageFPS() : -1.0;

	// GeneralsX: The logic rate is a delta on UnsignedInt, so any frame
	// counter RESET - returning to the menu, starting a match - makes it negative and it prints as
	// ~3.2e9 or ~7e7 rather than a rate. Observed twice in one run, both times on the first sample
	// of a new match (carries logic=70032935.4).
	//
	// The rate genuinely IS unknown for the window the reset lands in: nothing here records WHERE
	// inside that second the counter went back to 0, so any number we computed would be invented.
	// Report -1.0 and re-baseline, matching the file's existing convention where runahead/netrate/
	// netfps use -1 for "not applicable". A wrong rate is worse than an absent one - the whole
	// point of this probe is to be believable.
	const double  logicPerSec = (s_frameReset || logicFrame < s_windowStartFrame)
	                            ? -1.0
	                            : ((secs > 0.0) ? (double)(logicFrame - s_windowStartFrame) / secs : 0.0);

	const int64_t holdTotalNs = (TheFramePacer != NULL) ? (int64_t)TheFramePacer->getTotalHoldNs() : 0;
	const int64_t holdNs      = holdTotalNs - s_holdBaseNs;

	const Int  netFps    = (TheNetwork != NULL) ? TheNetwork->getAverageFPS() : -1;
	const Int  runAhead  = (TheNetwork != NULL) ? (Int)TheNetwork->getRunAhead() : -1;
	const Int  netRate   = (TheNetwork != NULL) ? (Int)TheNetwork->getFrameRate() : -1;

	// GeneralsX: cap= is the limit the pacer enforced at the end of this window -
	// the HUD bracket's number, 0 = unlimited (InGameUI::drawRenderFps) - so a run proves which cap it ran
	// under, not which one it was launched with.
	const Int capFps = (TheFramePacer != NULL) ? TheFramePacer->getActualFramesPerSecondLimit() : -1;
	// GeneralsX: cu= the iterations of this window that skipped their draw for tick catch-up
	// (FramePacer::isCatchUpDrawSkip): frames DRAWN = iters - cu (fast forward's skips are not counted here).
	const UnsignedInt cuNow = (TheFramePacer != NULL) ? TheFramePacer->getCatchUpPasses() : 0;
	fprintf(stderr,
		"[GXPACE] frame=%u iters=%.1f logic=%.1f rfps_local=%.1f rfps_engine=%.1f "
		"stall_ms=%.0f hold_ms=%.0f runahead=%d netrate=%d netfps=%d rfps_max=%d cap=%d cu=%u\n",
		logicFrame, itersPerSec, logicPerSec, itersPerSec, engineFps,
		(double)s_stallNs / 1000000.0, (double)holdNs / 1000000.0,
		runAhead, netRate, netFps,
		(int)gxPerfPeakFps(),	// best whole second of the last 10, -1 none (on a -headless peer,
								// iterations - like rfps_local - not frames drawn)
		(capFps >= (Int)RenderFpsPreset::UncappedFpsValue) ? 0 : capFps, cuNow - s_cuBase);
	fflush(stderr);
	s_cuBase = cuNow;

	s_windowStart      = now;
	s_windowStartFrame = logicFrame;
	s_iters            = 0;
	s_stallNs          = 0;
	s_holdBaseNs       = holdTotalNs;
	s_frameReset       = FALSE;
	return TRUE;
}

// ---------------------------------------------------------------------------
// GeneralsX: [GXGFX], the 1 Hz render split. It prints what GameClient/GXGfxProbe.h accumulated (frames marked in
// W3DDisplay::draw, Present timed through DX8Wrapper::Present_Probe) and closes its window on the same iteration as
// [GXPACE] when GX_PACE is on. Times are ms means per completed frame; unknown values are omitted; GXGfxProbe.h
// names each span. pres is the whole DXVK downstream wait, not GPU time.
static int gxGfxAppend( char *buf, int len, int cap, const char *fmt, ... )
{
	if( len < 0 || len >= cap )
		return len;
	va_list ap;
	va_start( ap, fmt );
	const int wrote = vsnprintf( buf + len, (size_t)( cap - len ), fmt, ap );
	va_end( ap );
	return wrote < 0 ? len : len + wrote;
}

static void gxGfxSample( Bool paceEmitted )
{
	if( !gxGfxLineOn() )
		return;
	static const Bool s_paced = ( getenv( "GX_PACE" ) != NULL );
	GxGfxState &g = g_gxGfx;
	const int64_t now = gxGfxWallNs();
	const UnsignedInt frame = ( TheGameLogic != NULL ) ? TheGameLogic->getFrame() : 0;
	if( g.winStart == 0 )
	{
		g.winStart = now;
		g.winFrame0 = frame;
		return;
	}
	if( s_paced ? !paceEmitted : ( now - g.winStart < 1000000000LL ) )
		return;

	GxGfxWindow &w = g.win;
	static Bool s_initPrinted = FALSE;
	if( !s_initPrinted && g.inited )
	{
		s_initPrinted = TRUE;
#if defined(_WIN32)
		fprintf( stderr, "[GXGFX] init clk=2 cpn=%.4f rdw=%.1f rdc=%.1f w=%d h=%d", g.cyclesPerNs,
#else
		fprintf( stderr, "[GXGFX] init clk=1 rdw=%.1f rdc=%.1f w=%d h=%d",
#endif
			g.readWallNs, g.readCpuNs, TheDisplay ? (int)TheDisplay->getWidth() : 0, TheDisplay ? (int)TheDisplay->getHeight() : 0 );
		if( g.loadSpan >= 0 )
			fprintf( stderr, " lspan=%d lus=%d lsleep=%d", g.loadSpan, g.loadUs, g.loadSleep ? 1 : 0 );
		fprintf( stderr, "\n" );
	}

	// A window with no marked draw at all (every -headless window: its draw() returns before the marks) prints
	// nothing, so a headless capture holds no [GXGFX] line.
	if( w.n > 0 || w.skip > 0 )
	{
#if defined(_WIN32)
		const Bool cpuKnown = g.cyclesPerNs > 0.0;
#else
		const Bool cpuKnown = TRUE;
#endif
		const double ms = 1.0e-6;
		const double f = w.n > 0 ? (double)w.n : 1.0;
		char buf[ 1536 ];
		int len = 0;
		len = gxGfxAppend( buf, len, sizeof( buf ), "[GXGFX] frame=%u f0=%u win=%.0f n=%u skip=%u",
			(unsigned)frame, (unsigned)g.winFrame0, (double)( now - g.winStart ) * ms, (unsigned)w.n, (unsigned)w.skip );
		if( TheGameLODManager != NULL )
			len = gxGfxAppend( buf, len, sizeof( buf ), " lod=%d", (int)TheGameLODManager->getDynamicLODLevel() );
		if( w.pintN > 0 )
			len = gxGfxAppend( buf, len, sizeof( buf ), " np=%u pint=%.3f pint99=%.3f pintmax=%.3f", (unsigned)w.pintN,
				(double)w.pint * ms / (double)w.pintN, (double)gxGfxHistPct( w.histPint, w.pintHistN, 99.0 ) * ms,
				(double)w.pintMax * ms );
		if( w.n > 0 )
		{
			len = gxGfxAppend( buf, len, sizeof( buf ), " draw=%.3f d99=%.3f dmax=%.3f",
				(double)w.draw * ms / f, (double)gxGfxHistPct( w.histDraw, w.n, 99.0 ) * ms, (double)w.drawMax * ms );
			if( cpuKnown )
				len = gxGfxAppend( buf, len, sizeof( buf ), " dcpu=%.3f off=%.3f",
					(double)w.drawCpu * ms / f, (double)( w.draw - w.drawCpu ) * ms / f );
			for( int s = 0; s < GXGFX_SPANS; ++s )
			{
				len = gxGfxAppend( buf, len, sizeof( buf ), " %s=%.3f", kGxGfxSpanName[ s ], (double)w.span[ s ] * ms / f );
				if( cpuKnown )
					len = gxGfxAppend( buf, len, sizeof( buf ), " %sc=%.3f", kGxGfxSpanName[ s ], (double)w.spanCpu[ s ] * ms / f );
			}
			len = gxGfxAppend( buf, len, sizeof( buf ), " pmax=%.3f ovh=%.2f", (double)w.presMax * ms,
				(double)w.marks / f * ( g.readWallNs + g.readCpuNs ) * 1.0e-3 );
		}
		if( w.cliN > 0 )
			len = gxGfxAppend( buf, len, sizeof( buf ), " cli=%.3f", (double)w.cli * ms / (double)w.cliN );
		if( g.inited && g.loadSpan >= 0 )
			len = gxGfxAppend( buf, len, sizeof( buf ), " load=%d", g.loadActive ? g.loadUs : 0 );
#if defined(_WIN32)
		{	// cpnw: this window's cycles per ns by the kernel's thread accounting. Printed only when the thread ran at
			// least 250 ms of CPU (GetThreadTimes is quantised to the ~15.6 ms scheduler tick: +-6% at 250 ms)
			ULONG64 cyc = 0;
			QueryThreadCycleTime( GetCurrentThread(), &cyc );
			const ULONG64 tt = gxGfxThreadTime100ns();
			if( g.winCycles0 != 0 && tt > g.winThreadTime0 && tt - g.winThreadTime0 >= 2500000 )
				len = gxGfxAppend( buf, len, sizeof( buf ), " cpnw=%.3f",
					(double)( cyc - g.winCycles0 ) / ( (double)( tt - g.winThreadTime0 ) * 100.0 ) );
			g.winCycles0 = cyc;
			g.winThreadTime0 = tt;
		}
#endif
#if defined(__APPLE__)
		{	// i-08: read once per window, at print time; -1 (unknown) is omitted
			const int therm = gxAppleThermalState();
			const int lpm = gxAppleLowPowerMode();
			if( therm >= 0 )
				len = gxGfxAppend( buf, len, sizeof( buf ), " therm=%d", therm );
			if( lpm >= 0 )
				len = gxGfxAppend( buf, len, sizeof( buf ), " lpm=%d", lpm );
		}
#endif
		if( w.dcN > 0 )
			len = gxGfxAppend( buf, len, sizeof( buf ), " dc=%.1f dcmax=%u", (double)w.dc / (double)w.dcN, (unsigned)w.dcMax );
		if( w.n > 0 )
			len = gxGfxAppend( buf, len, sizeof( buf ), " dcw=%.1f tex=%.1f rs=%.1f",
				(double)w.dcw / f, (double)w.tex / f, (double)w.rs / f );
		if( len > 0 && len < (int)sizeof( buf ) )
			fprintf( stderr, "%s\n", buf );

		// [GXGFXF] (GXGfxProbe.h names every field): scene sub-spans and hmupd in ms, counters as
		// means, per completed frame; same window and frame= as [GXGFX].
		if( w.n > 0 )
		{
			len = gxGfxAppend( buf, 0, sizeof( buf ), "[GXGFXF] frame=%u n=%u", (unsigned)frame, (unsigned)w.n );
			for( int k = 0; k < GXSUB_N; ++k )
				len = gxGfxAppend( buf, len, sizeof( buf ), " %s=%.3f", kGxGfxSubName[ k ], (double)w.sub[ k ] * ms / f );
			for( int k = 0; k < GXF_N; ++k )
				len = gxGfxAppend( buf, len, sizeof( buf ), k == GXF_HMUPD ? " %s=%.3f" : " %s=%.1f", kGxGfxFeatName[ k ],
					k == GXF_HMUPD ? (double)w.feat[ k ] * ms / f : (double)w.feat[ k ] / f );
			if( len > 0 && len < (int)sizeof( buf ) )
				fprintf( stderr, "%s\n", buf );
		}

		// [GXSHADOW] (GX_SHADOWSTATS=1; GXGfxProbe.h names every field): per completed frame,
		// counts as means, times (upd..pcol) in ms, upkb in KB. Same window and frame= as the [GXGFX] line above.
		if( gxShadowOn() && w.n > 0 )
		{
			len = gxGfxAppend( buf, 0, sizeof( buf ), "[GXSHADOW] frame=%u n=%u", (unsigned)frame, (unsigned)w.n );
			for( int k = 0; k < GXS_N; ++k )
			{
				const double v = (double)w.shadow[ k ] / f;
				if( k >= GXS_T_UPD )
					len = gxGfxAppend( buf, len, sizeof( buf ), " %s=%.3f", kGxShadowName[ k ], v * ms );
				else if( k == GXS_UPB )
					len = gxGfxAppend( buf, len, sizeof( buf ), " %s=%.1f", kGxShadowName[ k ], v / 1024.0 );
				else
					len = gxGfxAppend( buf, len, sizeof( buf ), " %s=%.1f", kGxShadowName[ k ], v );
			}
			if( len > 0 && len < (int)sizeof( buf ) )
				fprintf( stderr, "%s\n", buf );
		}

		// GeneralsX: [GXCENSUS] (GX_DRAWCENSUS=1, R-CPU roadmap step 2; GXGfxProbe.h, and
		// dx8wrapper.h enum DX8CensusCounter names every field): one line per [GXGFXF] sub-span that moved any counter,
		// then span=all (the whole draw), each counter a mean per completed frame (*_ns in ns). Same window and frame=.
		if( gxCensusOn() && w.n > 0 )
		{
			GxCensus &c = g_gxCensus;
			char cbuf[ 2048 ];
			for( int sp = 0; sp <= GXSUB_N; ++sp )
			{
				bool any = false;
				for( int k = 0; k < c.n && !any; ++k )
					any = c.win[ sp ][ k ] != 0;
				if( !any )
					continue;
				int clen = gxGfxAppend( cbuf, 0, sizeof( cbuf ), "[GXCENSUS] frame=%u n=%u span=%s", (unsigned)frame,
					(unsigned)w.n, sp < GXSUB_N ? kGxGfxSubName[ sp ] : "all" );
				for( int k = 0; k < c.n; ++k )
					clen = gxGfxAppend( cbuf, clen, sizeof( cbuf ), " %s=%.1f", c.names[ k ], (double)c.win[ sp ][ k ] / f );
				if( clen > 0 && clen < (int)sizeof( cbuf ) )
					fprintf( stderr, "%s\n", cbuf );
			}
		}
		fflush( stderr );
	}

	memset( &w, 0, sizeof( w ) );
	memset( g_gxCensus.win, 0, sizeof( g_gxCensus.win ) );
	g.winStart = now;
	g.winFrame0 = frame;
	if( g.inited && g.loadSpan >= 0 )
		g.loadActive = !g.loadActive;	// GX_GFX_LOAD: every other window is loaded
}

// ---------------------------------------------------------------------------
// GeneralsX: the player's own lag verdict on the existing toast channel (TheInGameUI->message()): no .wnd edit,
// which would move dataID and refuse every join with DATA_DIFFERS. Built from this machine's measurements, never
// naming another player. Toasted on change only and at most once a minute, though the verdict is recomputed every
// second.
static void gxPerfVerdictToast( void )
{
	/* GX_PERF_VERDICT_MAX, never a literal: a copy shorter than the composer's truncates, a
	 * truncated copy never strcmp-equals the original, and the once-a-minute gate below silently
	 * stops holding. */
	static char        s_shown[ GX_PERF_VERDICT_MAX ] = "";
	static UnsignedInt s_lastMs       = 0;

	const char *verdict = gxPerfLocalVerdict();
	if( verdict == NULL || verdict[ 0 ] == '\0' )
	{
		// Recovered. Forget what was shown so the SAME verdict toasts again if it comes back -
		// otherwise a player who fixes a problem and hits it again an hour later is told nothing.
		s_shown[ 0 ] = '\0';
		return;
	}
	if( strcmp( verdict, s_shown ) == 0 )
		return;

	const UnsignedInt now = timeGetTime();
	if( s_lastMs != 0 && now - s_lastMs < 60000 )
		return;

	strncpy( s_shown, verdict, sizeof( s_shown ) - 1 );
	s_shown[ sizeof( s_shown ) - 1 ] = '\0';
	s_lastMs = now;

	if( TheInGameUI != NULL )
	{
		UnicodeString line;
		line.translate( AsciiString( verdict ) );
		// GeneralsX: The verdict is finished text ("... (worst 1%: ...") - message() used it as a printf
		// format, and "%:" is not a conversion.
		TheInGameUI->messageNoFormat( line );
	}
}

// GeneralsX: GX_OBSERVE_PLAYER=<player index> (R-CPU roadmap step 2): in a
// replay or as an observer, view the match as that player from logic frame 1 - the observer bar's "view as player",
// so the shroud, ghosts and fogged objects draw as that player sees them. The census needs it: the G0 replay is an
// observer view (nothing shrouded), and a player's view adds the shroud material pass to every object that is not
// fully clear (W3DScene renderOneObject). It needs PlayerObserverEnabled = yes in Options.ini; the [GXOBSERVE] line says
// whether it took. Client-side like GX_CAM: rts::changeObservedPlayer is what the observer bar calls.
static void gxObserveDrive( void )
{
	static Int s_index = -2;	// -2 unread, -1 off, else the player index; set to -1 once applied
	if (s_index == -2)
	{
		const char *env = getenv("GX_OBSERVE_PLAYER");
		s_index = (env != NULL && env[0] >= '0' && env[0] <= '9') ? atoi(env) : -1;
	}
	if (s_index < 0 || TheGameLogic == NULL || ThePlayerList == NULL || TheControlBar == NULL || TheGameLogic->getFrame() < 1)
		return;
	// Only a replay or an observer seat, never the shell map's own game (the shell reached frame 1 and
	// consumed the request before the replay loaded) and never a seated player's live view.
	if (TheGameLogic->isInShellGame())
		return;
	Player *local = ThePlayerList->getLocalPlayer();
	if (!(TheRecorder != NULL && TheRecorder->isPlaybackMode()) && !(local != NULL && local->isPlayerObserver()))
		return;
	Player *pl = (s_index < ThePlayerList->getPlayerCount()) ? ThePlayerList->getNthPlayer(s_index) : NULL;
	AsciiString name("(none)");
	if (pl != NULL)
	{
		rts::changeObservedPlayer(pl);
		name.translate(pl->getPlayerDisplayName());
	}
	fprintf(stderr, "[GXOBSERVE] f=%u index=%d player=%s observed=%d players=", (unsigned)TheGameLogic->getFrame(), s_index,
		name.str(),
		(pl != NULL && TheControlBar->getObservedPlayer() == pl) ? 1 : 0);
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)	// so the next run can pick its index
	{
		AsciiString n;
		n.translate(ThePlayerList->getNthPlayer(i)->getPlayerDisplayName());
		fprintf(stderr, "%s%d:%s", i ? "," : "", i, n.str());
	}
	fprintf(stderr, "\n");
	fflush(stderr);
	s_index = -1;
}

// GeneralsX: May a test probe that finds OTHER players' units steer this client now? In a
// GX_TEST_INSTRUMENTS build, always. Otherwise the Debug menu's rule (GXDebugIntelGate.h): a replay, an observer seat, a
// defeated player, a game that is not a network game, or a network game with no second human (observers counted).
static Bool gxProbeMaySeeOthers( void )
{
#if defined(GX_TEST_INSTRUMENTS)
	return TRUE;
#else
	const Player *local = (ThePlayerList != NULL) ? ThePlayerList->getLocalPlayer() : NULL;
	return GXDebugIntel::mayShowOthersFor( TheRecorder != NULL && TheRecorder->isPlaybackMode(),
		TheNetwork != NULL || (TheGameLogic != NULL && TheGameLogic->isInMultiplayerGame()),
		GameLogic::gxCheatsAllowed(), local );
#endif
}

// GeneralsX: GX_CAM - every N logic frames, look at the centroid of the mobile objects of one active playable
// player, cycling players by frame number, so render measurements see armies instead of empty terrain. Deterministic
// (frame number and object order only) and client-side. Emits [GXCAM] per move. Held where gxProbeMaySeeOthers()
// says no: [GXCAMLOCK] once per match; GX_CAMPAN holds too.
static Coord3D s_gxCamTarget;			// GX_CAM's last target, for GX_CAMPAN
static Bool    s_gxCamHaveTarget = FALSE;

static void gxCameraDrive( void )
{
	static Int  s_period  = -1;
	static UnsignedInt s_lastMoved = 0xFFFFFFFF;

	if (s_period < 0)
	{
		const char *env = getenv("GX_CAM");
		s_period = (env != NULL) ? atoi(env) : 0;
		if (s_period > 0)
		{
			fprintf(stderr, "[GXCAM] camera driver ON, every %d frame(s)\n", s_period);
			fflush(stderr);
		}
	}

	if (s_period <= 0 || TheGameLogic == NULL || TheTacticalView == NULL || ThePlayerList == NULL)
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame == 0 || (frame % (UnsignedInt)s_period) != 0 || frame == s_lastMoved)
		return;

	static UnsignedInt s_lockSaid = 0xFFFFFFFF;	// the frame of the last [GXCAMLOCK]; a lower frame is a new match
	if (!gxProbeMaySeeOthers())
	{
		if (s_lockSaid == 0xFFFFFFFF || frame < s_lockSaid)
		{
			fprintf(stderr, "[GXCAMLOCK] GX_CAM held at f=%u: another human is in this network match and you are playing "
				"(a GX_TEST_INSTRUMENTS build, a replay, an observer seat or a defeated player is not held)\n", (unsigned)frame);
			fflush(stderr);
		}
		s_lockSaid = frame;
		s_gxCamHaveTarget = FALSE;
		return;
	}

	// Collect mobile objects owned by playable players. Structures are excluded deliberately: they
	// sit at the base for the whole match, so including them drags the camera back to the start
	// position - the very failure this probe exists to remove.
	//
	// The cap is a cost bound, not a sample: the pass below is O(n^2), and it runs once every
	// GX_CAM frames rather than per frame, so 512 entries costs ~131k distance tests every two
	// seconds at the default period. Objects past the cap are ignored in LIST ORDER, which is
	// deterministic - not by any rule that could differ between two peers.
	enum { GXCAM_MAX_OBJS = 512 };
	Coord3D pos[GXCAM_MAX_OBJS];
	Int     owner[GXCAM_MAX_OBJS];
	Int     n = 0;
	Int     perPlayer[MAX_PLAYER_COUNT];
	for (Int p = 0; p < MAX_PLAYER_COUNT; ++p)
		perPlayer[p] = 0;

	for (Object *o = TheGameLogic->getFirstObject(); o != NULL && n < GXCAM_MAX_OBJS; o = o->getNextObject())
	{
		Player *pl = o->getControllingPlayer();
		if (pl == NULL)
			continue;
		const Int idx = (Int)pl->getPlayerIndex();
		if (idx < 0 || idx >= MAX_PLAYER_COUNT || !pl->isPlayerActive() || !pl->isPlayableSide())
			continue;
		if (o->isKindOf(KINDOF_STRUCTURE))
			continue;
		const Coord3D *p3 = o->getPosition();
		if (p3 == NULL)
			continue;
		pos[n]   = *p3;
		owner[n] = idx;
		++perPlayer[idx];
		++n;
	}
	if (n == 0)
		return;

	// WHERE THE ACTION IS. Combat is two players' units close together, so the closest cross-owner
	// pair is the front line - and that is a far better render workload than any one army, because
	// it is where the explosions, smudges and shadow casters actually are. Falling back to the
	// biggest army keeps the camera useful in the opening minutes, when the armies have not met and
	// there is no front line to find; the log says which of the two it used, because "the camera
	// moved" and "the camera found combat" are different claims.
	Real bestDistSqr = 0.0f;
	Int  bestA = -1, bestB = -1;
	for (Int i = 0; i < n; ++i)
	{
		for (Int j = i + 1; j < n; ++j)
		{
			if (owner[i] == owner[j])
				continue;
			const Real dx = pos[i].x - pos[j].x;
			const Real dy = pos[i].y - pos[j].y;
			const Real d2 = dx * dx + dy * dy;
			if (bestA < 0 || d2 < bestDistSqr)
			{
				bestDistSqr = d2;
				bestA = i;
				bestB = j;
			}
		}
	}

	Coord3D target;
	const char *mode;
	Int reported;

	if (bestA >= 0)
	{
		target.x = (pos[bestA].x + pos[bestB].x) * 0.5f;
		target.y = (pos[bestA].y + pos[bestB].y) * 0.5f;
		mode     = "front";
		reported = (Int)sqrtf(bestDistSqr);
	}
	else
	{
		Int biggest = -1;
		for (Int p = 0; p < MAX_PLAYER_COUNT; ++p)
			if (perPlayer[p] > 0 && (biggest < 0 || perPlayer[p] > perPlayer[biggest]))
				biggest = p;
		if (biggest < 0)
			return;

		Real sumX = 0.0f, sumY = 0.0f;
		Int  c = 0;
		for (Int i = 0; i < n; ++i)
			if (owner[i] == biggest)
			{
				sumX += pos[i].x;
				sumY += pos[i].y;
				++c;
			}
		target.x = sumX / (Real)c;
		target.y = sumY / (Real)c;
		mode     = "army";
		reported = c;
	}

	target.z = 0.0f;
	TheTacticalView->lookAt(&target);
	s_lastMoved = frame;
	s_gxCamTarget = target;
	s_gxCamHaveTarget = TRUE;

	fprintf(stderr, "[GXCAM] f=%u mode=%s mobile=%d val=%d x=%.0f y=%.0f\n",
		(unsigned)frame, mode, n, reported, target.x, target.y);
	fflush(stderr);
}

// GeneralsX: GX_CAMPAN=<amplitude>,<period> - a moving camera for render measurements.
//
// GX_CAM jump-cuts to the front line every N frames and then PARKS, so terrain/shadow/culling work that only happens while
// the view moves is never seen. This pans the view along x around GX_CAM's last target, target.x + amplitude * sin(2 pi frame / period), once
// per logic frame. It needs GX_CAM (no target, no pan) and does nothing until the first GX_CAM move.
//
// GeneralsX: GX_CAMPITCH=<degrees> holds the view pitch of this client (0.1..89.9, View::setPitch's own
// clamp) so a picture or a draw-cost run sits at the horizon without an admin session. Client-only; unset = untouched.
static void gxCameraPitch( void )
{
	static Real s_deg = -1.0f;
	if (s_deg < 0.0f)
	{
#if defined(GX_TEST_INSTRUMENTS)	// a test knob: a stray variable in a player's environment must not hold the camera
		const char *env = getenv("GX_CAMPITCH");
#else
		const char *env = NULL;
#endif
		s_deg = (env != NULL) ? (Real)atof(env) : 0.0f;
		if (s_deg > 0.0f)
		{
			fprintf(stderr, "[GXCAMPITCH] pitch held at %.1f deg\n", s_deg);
			fflush(stderr);
		}
	}
	if (s_deg > 0.0f && TheTacticalView != NULL)
		TheTacticalView->setPitch(DEG_TO_RADF(s_deg));

}

// Deterministic by logic frame and list order only (GX_CAM's target), so two runs of one seed pan identically, and
// client-only like GX_CAM: TheTacticalView->lookAt moves this client's camera and nothing else. sinf differing in
// the last bit across platforms moves a camera, never the simulation. Undigested file.
static void gxCameraPan( void )
{
	static Int s_amp = -1, s_period = 0;
	static UnsignedInt s_last = 0xFFFFFFFF;
	if (s_amp < 0)
	{
		s_amp = 0;
		const char *env = getenv("GX_CAMPAN");
		Int a = 0, p = 0;
		if (env != NULL && sscanf(env, "%d,%d", &a, &p) == 2 && a > 0 && p > 1)
		{
			s_amp = a;
			s_period = p;
			fprintf(stderr, "[GXCAMPAN] pan ON: amplitude %d, period %d frame(s), around the GX_CAM target\n", a, p);
		}
		else if (env != NULL)
			fprintf(stderr, "[GXCAMPAN] GX_CAMPAN='%s' rejected (want <amplitude>,<period>, both > 0) - pan off\n", env);
		fflush(stderr);
	}
	if (s_amp <= 0 || TheGameLogic == NULL || TheTacticalView == NULL)
		return;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (s_last != 0xFFFFFFFF && frame < s_last)
		s_gxCamHaveTarget = FALSE;	// the frame counter went back: a new match, and the old target is on another map
	if (frame == s_last || !s_gxCamHaveTarget)
	{
		s_last = frame;
		return;
	}
	s_last = frame;
	Coord3D p = s_gxCamTarget;
	p.x += (Real)s_amp * sinf(6.2831853f * (Real)(frame % (UnsignedInt)s_period) / (Real)s_period);
	TheTacticalView->lookAt(&p);
}

// GeneralsX: GX_SHOT=<frame>[,<frame>...] takes the screenshot-hotkey PNG (TheDisplay->takeScreenShot) at each
// listed logic frame of the first match only (the W3D capture clock counts from the first match), never in the
// shell. Logs [GXSHOT] n=<k> f=<frame> asked=<frame>. Off when unset or -headless. Strict parse: 1-16 ascending
// decimal frames, each 1..999999999, else off with the reason logged once.
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

// GeneralsX: the camera easing (W3DView zoomCameraToDesiredHeight, movePivotToGround) steps by min(speed * BaseFps /
// render fps, 1) per draw and STOPS once the remainder is under a threshold, so where it comes to rest depended on
// the render cadence: a constant sub-pixel camera offset for the rest of the match, seen as per-pixel noise on
// uneven ground. Under capture it steps by the same total per logic frame, BaseFps / tick rate, once per logic
// frame.
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

// GeneralsX: GX_STRESS used to live here as a client-side spawn through
// MSG_GX_CHEAT_SPAWN_UNIT, which gxCheatsAllowed refuses with two humans in the match. It is now a
// lobby option set by the HOST (HeadlessMatch hostStressFromEnv, "ST=") and applied by the
// simulation on every peer: GameLogic::gxStressSpawn / gxStressOrders, GameLogic/GXStress.h.

// GeneralsX: GX_NOSHROUD=1 draws the whole map on this client: every client shroud cull asks gxShroudDrawn()
// (GXNoShroud.h), false under the probe. It writes no global data, never touches the shroud grid in the frame CRC,
// and is set once from init(). Only in a GX_TEST_INSTRUMENTS build, since it would reveal enemies; otherwise it is
// ignored with a line lacking the [GXNOSHROUD] canary.
static void gxNoShroud( void )
{
#if ENABLE_CONFIGURABLE_SHROUD
	const char *env = getenv( "GX_NOSHROUD" );
	if( env == nullptr || atoi( env ) <= 0 )
		return;
#if defined(GX_TEST_INSTRUMENTS)
	gxNoShroudProbe() = TRUE;
	fprintf( stderr, "[GXNOSHROUD] shroud OFF on this client (display only; the simulation is untouched)\n" );
#else
	fprintf( stderr, "[GXSHROUDLOCK] GX_NOSHROUD ignored: honoured only in a GX_TEST_INSTRUMENTS build\n" );
#endif
	fflush( stderr );
#endif
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

// GeneralsX: GX_TEST_SAVE_AT_FRAME=N: save once at logic frame N of a match, the
// way PopupSaveLoad does (so it carries the build trailer). Test-only: headless has no save menu, and a
// real save cannot be committed as a fixture because it embeds the retail map. scripts/test/save-compat-test.sh.
// GeneralsX: GX_FAST_FORWARD="<speed>" or "<frame>:<speed>,<frame>:<speed>,..." - fast forward from
// the environment, through the same refusal as the Debug screen (setFastForwardSpeed, CommandXlat.cpp): the driver for a
// measured run nobody clicks in, e.g. "0:2,3600:10,72000:1". Each entry applies once, on the first client update at or
// past its frame and past frame 0 (a match is not started before it); a new match (its frame counter back below the last one seen) starts the list over. Not a way around
// the rule: a refused entry logs "[GXFF] refused" and the match stays at 1x.
static void gxFastForwardSchedule()
{
	static const char *const s_spec = getenv("GX_FAST_FORWARD");
	if (s_spec == nullptr || *s_spec == '\0' || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame())
		return;
	static const char *s_next = nullptr;
	static UnsignedInt s_lastFrame = 0;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (s_next == nullptr || frame < s_lastFrame)
		s_next = s_spec;
	s_lastFrame = frame;
	while (*s_next != '\0')
	{
		unsigned int at = 0;
		int speed = 0;
		int used = 0;
		if (sscanf(s_next, "%u:%d%n", &at, &speed, &used) != 2)
		{
			at = 0;
			if (sscanf(s_next, "%d%n", &speed, &used) != 1)
			{
				fprintf(stderr, "[GXFF] GX_FAST_FORWARD: cannot parse '%s'\n", s_next);
				s_next = "";
				return;
			}
		}
		if (frame < at || frame == 0)	// not before the first logic frame: gxFastForwardRefusal
			return;
		setFastForwardSpeed(speed);
		s_next += used;
		if (*s_next == ',')
			++s_next;
	}
}

// GeneralsX: GX_REPLAY_PAUSE="<frame>:<seconds>" - in a replay played through the client (-loadreplay), pause logic
// at <frame> for <seconds> of wall time while the client keeps drawing, then resume once: the draw-only ceiling of a
// scene. Prints [GXRPAUSE] on and off lines with the frame. Ignored outside a replay; one window per process; never
// with GX_REPLAY_SEEK. Run uncapped (-noFPSLimit); sound and per-drawable updates are paused too.
static void gxReplayPauseSchedule()
{
	static const char *const s_spec = getenv("GX_REPLAY_PAUSE");
	if (s_spec == nullptr || *s_spec == '\0' || TheGameLogic == nullptr || !TheGameLogic->isInGame() || !TheGameLogic->isInReplayGame())
		return;
	static Int s_state = -1;	// -1 unparsed, 0 waiting for the frame, 1 paused, 2 done
	static unsigned int s_at = 0;
	static double s_seconds = 0.0;
	static std::chrono::steady_clock::time_point s_since;
	if (s_state == -1)
	{
		s_state = (sscanf(s_spec, "%u:%lf", &s_at, &s_seconds) == 2 && s_seconds > 0.0) ? 0 : 2;
		if (s_state == 2)
			fprintf(stderr, "[GXRPAUSE] GX_REPLAY_PAUSE: cannot parse '%s' (want <frame>:<seconds>)\n", s_spec);
	}
	const UnsignedInt frame = TheGameLogic->getFrame();
	const auto now = std::chrono::steady_clock::now();
	if (s_state == 0 && frame >= s_at)
	{
		TheGameLogic->setGamePaused(TRUE, FALSE, FALSE);
		s_since = now;
		s_state = 1;
		// paused=1 is the proof the window holds the logic (a refused pause would label a running match "held")
		fprintf(stderr, "[GXRPAUSE] on f=%u paused=%d\n", (unsigned)frame, TheGameLogic->isGamePaused() ? 1 : 0);
	}
	else if (s_state == 1 && std::chrono::duration<double>(now - s_since).count() >= s_seconds)
	{
		TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);
		s_state = 2;
		fprintf(stderr, "[GXRPAUSE] off f=%u held_ms=%lld\n", (unsigned)frame,
			(long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - s_since).count());
	}
}

static void gxTestSaveAtFrame()
{
	static Int s_frame = -2;
	if (s_frame == -2)
	{
		const char *v = getenv("GX_TEST_SAVE_AT_FRAME");
		s_frame = v != nullptr ? atoi(v) : -1;
	}
	if (s_frame < 0 || TheGameLogic == nullptr || TheGameState == nullptr || !TheGameLogic->isInGame()
		|| TheGameLogic->getFrame() < (UnsignedInt)s_frame)
		return;
	const Int at = s_frame;
	s_frame = -1;
	// GeneralsX: Never in a network match with a second human in it. A save is every player's
	// whole state - bases, armies, cash - and a LAN one is written below as a skirmish so it loads past the gate, so a copy
	// loaded on a second machine scouted the whole map mid-match. Every harness that uses this is a solo host or an all-AI
	// match watched from one observer seat: GameLogic::gxCheatsAllowed's sandbox (observers counted), left alone.
	if ((TheNetwork != NULL || TheGameLogic->isInMultiplayerGame()) && !GameLogic::gxCheatsAllowed())
	{
		fprintf(stderr, "[SAVELOAD] test save at frame %d refused: another human is in this network match\n", at);
		fflush(stderr);
		return;
	}
	// The only headless match is a solo -lanhost, and a LAN save is refused on load (SaveLoadFeedback.cpp).
	// Write it as the skirmish it effectively is: GameStateMap::xfer saves the mode and, for a skirmish,
	// TheSkirmishGameInfo - built here from the LAN game's own slots, map and seed - then put it all back.
	const GameMode mode = TheGameLogic->getGameMode();
	const Bool asSkirmish = (mode == GAME_LAN && TheGameInfo != nullptr && TheSkirmishGameInfo == nullptr);
	if (asSkirmish)
	{
		TheSkirmishGameInfo = NEW SkirmishGameInfo;
		TheSkirmishGameInfo->init();
		TheSkirmishGameInfo->clearSlotList();
		TheSkirmishGameInfo->reset();
		// A skirmish matches its local slot by IP, and a skirmish slot's IP is 0 (SkirmishGameOptionsMenu).
		for (Int i = 0; i < MAX_SLOTS; ++i)
			if (TheGameInfo->getConstSlot(i) != nullptr)
			{
				TheSkirmishGameInfo->setSlot(i, *TheGameInfo->getConstSlot(i));
				TheSkirmishGameInfo->getSlot(i)->setIP(0);
			}
		TheSkirmishGameInfo->setMap(TheGameInfo->getMap());
		TheSkirmishGameInfo->setMapCRC(TheGameInfo->getMapCRC());
		TheSkirmishGameInfo->setMapSize(TheGameInfo->getMapSize());
		TheSkirmishGameInfo->setSeed(TheGameInfo->getSeed());
		TheSkirmishGameInfo->setStartingCash(TheGameInfo->getStartingCash());
		TheSkirmishGameInfo->setGxAiColours(TheGameInfo->getGxAiColours());
		TheSkirmishGameInfo->setLocalIP(0);
		TheSkirmishGameInfo->setInGame();
		TheSkirmishGameInfo->startGame(0);
		TheGameLogic->setGameMode(GAME_SKIRMISH);
	}
	const SaveResult r = TheGameState->saveGame(AsciiString::TheEmptyString, UnicodeString(L"gxtest"), SAVE_FILE_TYPE_NORMAL);
	if (asSkirmish)
	{
		TheGameLogic->setGameMode(mode);
		delete TheSkirmishGameInfo;
		TheSkirmishGameInfo = nullptr;
	}
	presentSaveResult(r);
	fprintf(stderr, "[SAVELOAD] test save at frame %d: SaveCode %d file '%s'\n", at, (Int)r.saveCode, r.filename.str());
}

// GeneralsX: GX_TEST_QUIT_AT=<frame>[:surrender] - at <frame> this peer quits through the in-game menu calls, so a
// headless run exercises the real quit path. Default: Esc > Exit Game > Yes. surrender: MSG_SELF_DESTRUCT(TRUE), the
// player stays. exitprocess: the default quit, then a normal engine shutdown. exitonly: GameLogic::exitGame alone,
// no self-destruct. Off unless set; fires once.
static void gxTestQuitAtFrame()
{
	static Int s_frame = -2;
	static Bool s_surrender = FALSE;
	static Bool s_exitOnly = FALSE;
	static Bool s_exitProcess = FALSE;
	if (s_frame == -2)
	{
		const char *v = getenv("GX_TEST_QUIT_AT");
		s_frame = v != nullptr ? atoi(v) : -1;
		s_surrender = v != nullptr && strstr(v, ":surrender") != nullptr;
		s_exitOnly = v != nullptr && strstr(v, ":exitonly") != nullptr;
		s_exitProcess = v != nullptr && strstr(v, ":exitprocess") != nullptr;
	}
	// A MATCH only: the menu's animated backdrop is a game too (GAME_SHELL), and quitting it would spend the one shot
	// before the match starts.
	if (s_frame < 0 || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
		|| TheGameLogic->isLoadingMap() || TheGameLogic->getFrame() < (UnsignedInt)s_frame || TheInGameUI == nullptr
		|| TheVictoryConditions == nullptr)
		return;
	const Int at = s_frame;
	s_frame = -1;
	fprintf(stderr, "[GXQUIT] %s at frame %u (GX_TEST_QUIT_AT=%d)\n", s_surrender ? "surrender" : s_exitOnly ? "exitonly" : "exit",
		TheGameLogic->getFrame(), at);
	if (s_exitOnly)
	{
		TheGameLogic->exitGame();
		return;
	}
	if (s_surrender)
	{
		if (TheVictoryConditions->isLocalAlliedVictory())
			return;
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
		msg->appendBooleanArgument(TRUE);
		TheInGameUI->setClientQuiet(TRUE);
		return;
	}
	TheInGameUI->setQuitMenuVisible(TRUE);
	TheGameLogic->quit(FALSE);
	TheInGameUI->setQuitMenuVisible(FALSE);
	if (s_exitProcess && TheGameEngine != nullptr)
		TheGameEngine->setQuitting(TRUE);
}

// GeneralsX: GX_TEST_SELECT=<frame>: at that logic frame of a match, select this player's
// Command Center on THIS client and log one "[GXSELECT]" line - so a GX_SHOT capture at a later frame shows its command
// bar, and with it the hotkey letters as drawHotKeyLabel draws them (the Keybinds page's corner, size and look). It
// does only what a player's own click does to their own selection, minus the message: InGameUI::selectDrawable alone
// posts nothing, and it acts only on an EMPTY selection, because clearing one (deselectAllDrawables) would post
// MSG_DESTROY_SELECTED_GROUP. So nothing reaches the simulation or the network, which is why it sits outside
// GX_TEST_INSTRUMENTS as GX_SHOT does, and the binary a proof runs is the one that ships. Once; off when unset.
static void gxTestSelect()
{
	static Int s_at = -1;
	if (s_at < 0)
	{
		const char *v = getenv("GX_TEST_SELECT");
		s_at = (v != nullptr) ? atoi(v) : 0;
		if (s_at < 0)
			s_at = 0;
	}
	if (s_at == 0 || TheGameLogic == nullptr || TheInGameUI == nullptr || ThePlayerList == nullptr
		|| !TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->isLoadingMap()
		|| TheGameLogic->getFrame() < (UnsignedInt)s_at)
		return;
	s_at = 0;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (TheInGameUI->getSelectCount() != 0)
	{
		fprintf(stderr, "[GXSELECT] frame %u: something is already selected - nothing done\n", frame);
		fflush(stderr);
		return;
	}
	const Player *local = ThePlayerList->getLocalPlayer();
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (o->getControllingPlayer() != local || !o->isKindOf(KINDOF_COMMANDCENTER) || o->isEffectivelyDead()
			|| o->getDrawable() == nullptr)
			continue;
		TheInGameUI->selectDrawable(o->getDrawable());
		fprintf(stderr, "[GXSELECT] frame %u selected %s id %u\n", frame, o->getTemplate()->getName().str(), (unsigned)o->getID());
		fflush(stderr);
		return;
	}
	fprintf(stderr, "[GXSELECT] frame %u: this player has no Command Center\n", frame);
	fflush(stderr);
}

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: GX_TEST_REVEAL_SCRIPT="<frame>:<0|1|p>,...": at each listed logic frame, print how
// many partition cells are CLEAR for the local player ([GXREVEAL] probe), then for 1 / 0 order Reveal Map / Re-shroud
// on the local player through the message stream, exactly as the Debug menu's buttons do. Test-only, like
// GX_TEST_FORGE: scripts/test/reveal-saveload-test.sh uses it to reveal before a save and toggle after the load.
static void gxTestRevealScript()
{
	static UnsignedInt s_frame[16];
	static char s_what[16];
	static Int s_count = -1, s_at = 0;
	if (s_count < 0)
	{
		s_count = 0;
		const char *v = getenv("GX_TEST_REVEAL_SCRIPT");
		unsigned f = 0; char w = 0; int n = 0;
		while (v != nullptr && s_count < 16 && sscanf(v, "%u:%c%n", &f, &w, &n) >= 2)
		{
			s_frame[s_count] = f; s_what[s_count] = w; ++s_count;
			v += n;
			if (*v != ',')
				break;
			++v;
		}
	}
	if (s_at >= s_count || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isLoadingMap()
		|| TheGameLogic->isLoadingSave() || TheGameLogic->getFrame() < s_frame[s_at]
		|| ThePlayerList == nullptr || ThePlayerList->getLocalPlayer() == nullptr || ThePartitionManager == nullptr)
		return;
	const Int idx = ThePlayerList->getLocalPlayer()->getPlayerIndex();
	Int clear = 0, total = 0;
	for (Int y = 0; y < ThePartitionManager->getCellCountY(); ++y)
		for (Int x = 0; x < ThePartitionManager->getCellCountX(); ++x, ++total)
			if (ThePartitionManager->getShroudStatusForPlayer(idx, x, y) == CELLSHROUD_CLEAR)
				++clear;
	fprintf(stderr, "[GXREVEAL] step %d frame %u player %d clear=%d/%d\n", s_at, (unsigned)TheGameLogic->getFrame(), idx, clear, total);
	const char w = s_what[s_at++];
	if (w == '0' || w == '1')
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_CHEAT_REVEAL_MAP);
		msg->appendIntegerArgument(idx);
		msg->appendBooleanArgument(w == '1');
		fprintf(stderr, "[GXREVEAL] ordered reveal=%c for player %d\n", w, idx);
	}
	fflush(stderr);
}

// GeneralsX: GX_TEST_CHEAT_SCRIPT="<frame>:<order>:<target>,...": at each listed logic frame run one step and print
// a [GXCHEATSCRIPT] line. Cheat orders go through the message stream as the Debug menu sends them (an admin's is
// signed in Network.cpp gxSealAdminOrder): spawn, cars, rangers, defeat, money, wipe. <target>: a slot number, me
// (this peer's Player) or obs (the ReplayObserver).
static void gxTestCheatScript()
{
	static UnsignedInt s_frame[16];
	static char s_order[16][8];
	static char s_target[16][4];
	static Int s_count = -1, s_at = 0;
	if (s_count < 0)
	{
		s_count = 0;
		const char *v = getenv("GX_TEST_CHEAT_SCRIPT");
		unsigned f = 0; int n = 0;
		while (v != nullptr && s_count < 16 && sscanf(v, "%u:%7[a-z]:%3[a-z0-9]%n", &f, s_order[s_count], s_target[s_count], &n) == 3)
		{
			s_frame[s_count++] = f;
			v += n;
			if (*v != ',')
				break;
			++v;
		}
	}
	if (s_at >= s_count || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isLoadingMap()
		|| TheGameLogic->getFrame() < s_frame[s_at] || ThePlayerList == nullptr || ThePlayerList->getLocalPlayer() == nullptr
		|| TheTerrainLogic == nullptr || TheThingFactory == nullptr)
		return;
	const Int step = s_at++;
	const char *order = s_order[step];
	const UnsignedInt frame = TheGameLogic->getFrame();
	const Player *local = ThePlayerList->getLocalPlayer();
	enum { HUMVEE, CAR, RANGER, KINDS };
	static const char *const s_kindName[KINDS] = { "humvee", "car", "ranger" };
	const ThingTemplate *kind[KINDS] = { TheThingFactory->findTemplate("AmericaVehicleHumvee"),
		TheThingFactory->findTemplate("CarSedan01"), TheThingFactory->findTemplate("AmericaInfantryRanger") };

	if (strcmp(order, "enter") == 0 || strcmp(order, "probe") == 0)
	{
		Int owned[KINDS][MAX_PLAYER_COUNT] = { { 0 } };
		Int unmanned[KINDS] = { 0 }, veteran[KINDS] = { 0 };
		std::vector<ObjectID> mine, seats;
		for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
		{
			if (o->isEffectivelyDead())
				continue;
			const Player *owner = o->getControllingPlayer();
			const Int idx = (owner != nullptr) ? (Int)owner->getPlayerIndex() : -1;
			for (Int k = 0; k < KINDS; ++k)
			{
				if (kind[k] == nullptr || o->getTemplate()->getTemplateID() != kind[k]->getTemplateID())
					continue;
				if (idx >= 0 && idx < MAX_PLAYER_COUNT)
					++owned[k][idx];
				if (o->isDisabledByType(DISABLED_UNMANNED))
					++unmanned[k];
				if (o->getVeterancyLevel() >= LEVEL_VETERAN)
					++veteran[k];
				if (k == RANGER && owner == local)
					mine.push_back(o->getID());
			}
			if (owner == ThePlayerList->getNeutralPlayer() && o->isDisabledByType(DISABLED_UNMANNED))
				seats.push_back(o->getID());
		}
		AsciiString line, part;
		line.format("[GXCHEATSCRIPT] step %d frame %u ", step, (unsigned)frame);
		if (order[0] == 'e')
		{
			std::sort(mine.begin(), mine.end());
			std::sort(seats.begin(), seats.end());
			const size_t pairs = std::min(mine.size(), seats.size());
			for (size_t i = 0; i < pairs; ++i)
			{
				GameMessage *pick = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
				pick->appendBooleanArgument(TRUE);
				pick->appendObjectIDArgument(mine[i]);
				GameMessage *go = TheMessageStream->appendMessage(GameMessage::MSG_ENTER);
				go->appendObjectIDArgument(INVALID_ID);	// the selection, as CommandTranslator::createEnterMessage sends it
				go->appendObjectIDArgument(seats[i]);
			}
			part.format("enter: %d of my Rangers sent into %d Neutral unmanned vehicle(s)", (Int)pairs, (Int)seats.size());
			line.concat(part);
		}
		else
		{
			line.concat("probe");
			for (Int k = 0; k < KINDS; ++k)
			{
				part.format(" %s", s_kindName[k]);
				line.concat(part);
				for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
					if (owned[k][i] > 0) { part.format(" p%d=%d", i, owned[k][i]); line.concat(part); }
				part.format(" unmanned=%d veteran=%d", unmanned[k], veteran[k]);
				line.concat(part);
			}
		}
		fprintf(stderr, "%s\n", line.str());
		fflush(stderr);
		return;
	}

	const Player *target = local;
	if (strcmp(s_target[step], "obs") == 0)
		target = ThePlayerList->findPlayerWithNameKey(NAMEKEY("ReplayObserver"));
	else if (strcmp(s_target[step], "me") != 0)
	{
		AsciiString name;
		name.format("player%s", s_target[step]);
		target = ThePlayerList->findPlayerWithNameKey(NAMEKEY(name));
	}
	const ThingTemplate *spawnWhat = (strcmp(order, "spawn") == 0) ? kind[HUMVEE]
		: (strcmp(order, "cars") == 0) ? kind[CAR] : (strcmp(order, "rangers") == 0) ? kind[RANGER] : nullptr;
	GameMessage::Type type = GameMessage::MSG_INVALID;
	if (spawnWhat != nullptr) type = GameMessage::MSG_GX_CHEAT_SPAWN_UNIT;
	else if (strcmp(order, "kill") == 0) type = GameMessage::MSG_GX_CHEAT_KILL_PLAYER;
	else if (strcmp(order, "money") == 0) type = GameMessage::MSG_GX_CHEAT_GIVE_MONEY;
	else if (strcmp(order, "wipe") == 0) type = GameMessage::MSG_GX_CHEAT_KILL_OBJECTS;
	if (target == nullptr || type == GameMessage::MSG_INVALID)
	{
		fprintf(stderr, "[GXCHEATSCRIPT] step %d: nothing sent for '%s:%s'\n", step, order, s_target[step]);
		fflush(stderr);
		return;
	}
	GameMessage *msg = TheMessageStream->appendMessage(type);
	msg->appendIntegerArgument(target->getPlayerIndex());
	if (type == GameMessage::MSG_GX_CHEAT_SPAWN_UNIT)
	{
		Region3D extent;
		TheTerrainLogic->getExtent(&extent);
		Coord3D centre;
		centre.set((extent.lo.x + extent.hi.x) / 2, (extent.lo.y + extent.hi.y) / 2, 0);
		msg->appendIntegerArgument((Int)spawnWhat->getTemplateID());
		msg->appendLocationArgument(centre);
		msg->appendIntegerArgument(7);
		msg->appendIntegerArgument((Int)LEVEL_VETERAN);
	}
	else if (type == GameMessage::MSG_GX_CHEAT_GIVE_MONEY)
		msg->appendIntegerArgument(1000);
	else
		msg->appendBooleanArgument(type == GameMessage::MSG_GX_CHEAT_KILL_OBJECTS);
	fprintf(stderr, "[GXCHEATSCRIPT] step %d frame %u sent %s (%d) at player %d\n", step, (unsigned)frame,
		order, (Int)type, (Int)target->getPlayerIndex());
	fflush(stderr);
}
#endif

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: GX_TEST_ENTERQ="<frame>:<step>,..." - the WAYPOINT ENTER test driver, a TEST INSTRUMENT
// like GX_TEST_REVEAL_SCRIPT. It acts as the local player's mouse and keyboard would: every order goes into
// TheMessageStream and crosses the network; the `q`, `x` and `c` steps go through the real
// CommandTranslator::evaluateContextCommand in waypoint mode. The probes (`p`, `v`) only read. Steps: see
// scripts/test/net-kill-gates.sh (the enterq arms) and the T6 plan, Task 4.
namespace
{
	std::vector<ObjectID> s_gxEnterqContainers;		///< the local player's containers, numbered at the first step
	Bool s_gxEnterqNumbered = FALSE;
	std::vector<std::pair<char, ObjectID> > s_gxEnterqWatched;	///< every `x` step's targets, for the `o` step

	Bool gxEnterqIsContainer( const Object *o )
	{
		return o->getContain() != nullptr && o->getContain()->getContainMax() > 0;
	}

	// "Infantry" for these tests: combat infantry. A GLA start's Worker is INFANTRY and DOZER, and it would board too.
	Bool gxEnterqIsInfantry( const Object *o )
	{
		return o->isKindOf( KINDOF_INFANTRY ) && !o->isKindOf( KINDOF_DOZER );
	}

	// The local player's living objects of one kind, ascending ObjectID.
	void gxEnterqLocal( Int kind, std::vector<Object *> &out )		// 0 infantry out, 1 containers, 2 vehicle without contain
	{
		out.clear();
		const Player *local = ThePlayerList->getLocalPlayer();
		for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
		{
			if( o->isEffectivelyDead() || o->getControllingPlayer() != local )
				continue;
			if( ( kind == 0 && gxEnterqIsInfantry( o ) && o->getContainedBy() == nullptr )
				|| ( kind == 1 && gxEnterqIsContainer( o ) )
				|| ( kind == 2 && o->isKindOf( KINDOF_VEHICLE ) && o->getContain() == nullptr ) )
				out.push_back( o );
		}
		for( size_t i = 1; i < out.size(); ++i )		// insertion sort by id: small lists
			for( size_t j = i; j > 0 && out[ j ]->getID() < out[ j - 1 ]->getID(); --j )
				std::swap( out[ j ], out[ j - 1 ] );
	}

	// A step's target letter: a container digit, i (first own infantry), T (own vehicle with no contain module), E (the
	// highest-ID container of another human player), V (the highest-ID vehicle with NO contain module of another human
	// player) or G (the highest-ID NON-FACTION structure with a contain module - a civilian building - any owner).
	Object *gxEnterqTarget( char c )
	{
		std::vector<Object *> v;
		if( c >= '1' && c <= '9' )
		{
			const size_t d = (size_t)( c - '1' );
			return d < s_gxEnterqContainers.size() ? TheGameLogic->findObjectByID( s_gxEnterqContainers[ d ] ) : nullptr;
		}
		if( c == 'i' || c == 'T' )
		{
			gxEnterqLocal( c == 'i' ? 0 : 2, v );
			return v.empty() ? nullptr : v[ 0 ];
		}
		if( c == 'G' )
		{
			Object *best = nullptr;
			for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
				if( !o->isEffectivelyDead() && gxEnterqIsContainer( o ) && o->isKindOf( KINDOF_STRUCTURE ) && !o->isFactionStructure()
					&& ( best == nullptr || o->getID() > best->getID() ) )
					best = o;
			return best;
		}
		if( c == 'E' || c == 'V' )
		{
			Object *best = nullptr;
			const Player *local = ThePlayerList->getLocalPlayer();
			for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
			{
				const Player *owner = o->getControllingPlayer();
				const Bool kind = c == 'E' ? gxEnterqIsContainer( o ) : ( o->isKindOf( KINDOF_VEHICLE ) && o->getContain() == nullptr );
				if( !o->isEffectivelyDead() && kind && owner && owner != local
					&& owner->getPlayerType() == PLAYER_HUMAN && ( best == nullptr || o->getID() > best->getID() ) )
					best = o;
			}
			return best;
		}
		return nullptr;
	}

	// Select these objects as a player would: in the client, and through the simulation's group message.
	void gxEnterqSelect( const std::vector<Object *> &objs )
	{
		TheInGameUI->deselectAllDrawables();
		GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_CREATE_SELECTED_GROUP );
		msg->appendBooleanArgument( TRUE );
		for( size_t i = 0; i < objs.size(); ++i )
		{
			msg->appendObjectIDArgument( objs[ i ]->getID() );
			if( objs[ i ]->getDrawable() )
				TheInGameUI->selectDrawable( objs[ i ]->getDrawable() );
		}
	}

	void gxEnterqProbe()
	{
		for( Int p = 0; p < ThePlayerList->getPlayerCount(); ++p )
		{
			const Player *player = ThePlayerList->getNthPlayer( p );
			std::vector<Object *> containers;
			Int out = 0, queued = 0;
			for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
			{
				if( o->isEffectivelyDead() || o->getControllingPlayer() != player )
					continue;
				if( gxEnterqIsContainer( o ) )
					containers.push_back( o );
				else if( gxEnterqIsInfantry( o ) )
				{
					if( o->getContainedBy() == nullptr )
						++out;
					if( o->getAI() )
						queued += o->getAI()->gxTestQueueCount();
				}
			}
			if( containers.empty() )
				continue;
			for( size_t i = 1; i < containers.size(); ++i )
				for( size_t j = i; j > 0 && containers[ j ]->getID() < containers[ j - 1 ]->getID(); --j )
					std::swap( containers[ j ], containers[ j - 1 ] );
			AsciiString line, item;
			line.format( "[GXENTERQ] f=%u p=%d", (unsigned)TheGameLogic->getFrame(), p );
			for( size_t i = 0; i < containers.size(); ++i )
			{
				Object *c = containers[ i ];
				Int heading = 0;
				for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
					if( gxEnterqIsInfantry( o ) && o->getAI() && o->getAI()->getGoalObject() == c )
						++heading;
				const BodyModuleInterface *body = c->getBodyModule();
				const Int health = ( body && body->getMaxHealth() > 0 ) ? (Int)( 100.0f * body->getHealth() / body->getMaxHealth() ) : -1;
				item.format( " %u:%d/%d@%d,%d%sg%dh%d", (unsigned)c->getID(), (Int)c->getContain()->getContainCount(),
					c->getContain()->getContainMax(), (Int)c->getPosition()->x, (Int)c->getPosition()->y,
					c->isDisabledByType( DISABLED_UNMANNED ) ? "u" : "", heading, health );
				line.concat( item );
			}
			item.format( " out=%d q=%d", out, queued );
			line.concat( item );
			fprintf( stderr, "%s\n", line.str() );
		}
		fflush( stderr );
	}

	// Is the read-only visibility test equivalent to the lazy status? getShroudedStatus() may recompute a lazy cache that
	// simulation code reads back, so the probe asks the SAME questions on every peer - every object, every player index,
	// in object-list order - never "my local player" alone, which differs per peer. Every peer that runs it therefore
	// makes identical cache updates from identical state; the harness gives it to both peers at the same frame.
	void gxEnterqVisibility()
	{
		Int checks = 0, partial = 0, mismatchAny = 0, mismatchCentre = 0;
		for( Int idx = 0; idx < ThePlayerList->getPlayerCount(); ++idx )
		{
			for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
			{
				++checks;
				const ObjectShroudStatus st = o->getShroudedStatus( idx );
				if( st == OBJECTSHROUD_PARTIAL_CLEAR )
					++partial;
				const Bool truth = st < OBJECTSHROUD_FOGGED;
				const PartitionData *cells = o->friend_getPartitionData();
				const Bool any = o->isKindOf( KINDOF_ALWAYS_VISIBLE ) || cells == nullptr || cells->isAnyCellClearForPlayer( idx );
				const Bool centre = ThePartitionManager->getShroudStatusForPlayer( idx, o->getPosition() ) == CELLSHROUD_CLEAR;
				if( any != truth )
					++mismatchAny;
				if( centre != truth )
					++mismatchCentre;
			}
		}
		fprintf( stderr, "[GXENTERQ] vis f=%u checks=%d partial=%d mismatch_any=%d mismatch_centre=%d\n",
			(unsigned)TheGameLogic->getFrame(), checks, partial, mismatchAny, mismatchCentre );
		fflush( stderr );
	}

	GameMessage::Type gxEnterqClick( CommandTranslator *xlat, Object *target, Bool waypoint, CommandTranslator::CommandEvaluateType type )
	{
		TheInGameUI->setWaypointMode( waypoint );
		const GameMessage::Type t = xlat->evaluateContextCommand( target->getDrawable(), target->getPosition(), type );
		TheInGameUI->setWaypointMode( FALSE );
		return t;
	}

	void gxEnterqMoveBy( Real dx, Real dy, const Coord3D &from )
	{
		Coord3D to = from;
		to.x += dx;
		to.y += dy;
		TheMessageStream->appendMessage( GameMessage::MSG_DO_MOVETO )->appendLocationArgument( to );
	}

	void gxEnterqStep( CommandTranslator *xlat, const AsciiString &step )
	{
		const char kind = step.getCharAt( 0 );
		const char *args = step.str() + 1;
		std::vector<Object *> v;
		if( !s_gxEnterqNumbered )
		{
			gxEnterqLocal( 1, v );
			for( size_t i = 0; i < v.size(); ++i )
				s_gxEnterqContainers.push_back( v[ i ]->getID() );
			s_gxEnterqNumbered = TRUE;
			AsciiString ids, one;
			for( size_t i = 0; i < s_gxEnterqContainers.size(); ++i )
			{
				one.format( " %u", (unsigned)s_gxEnterqContainers[ i ] );
				ids.concat( one );
			}
			fprintf( stderr, "[GXENTERQ] containers%s\n", ids.str() );
			v.clear();
		}
		switch( kind )
		{
			case 'e':
			case 'm':
				for( const char *a = args; *a; ++a )
					if( Object *o = gxEnterqTarget( *a ) )
						v.push_back( o );
				gxEnterqSelect( v );
				if( kind == 'e' )
					TheMessageStream->appendMessage( GameMessage::MSG_EVACUATE );
				else if( !v.empty() )
					gxEnterqMoveBy( 400.0f, 0.0f, *v[ 0 ]->getPosition() );
				break;
			case 'h':
			case 'S':
				gxEnterqLocal( 0, v );
				gxEnterqSelect( v );
				if( kind == 'h' )
					TheMessageStream->appendMessage( GameMessage::MSG_INTERNET_HACK );
				break;
			case 'q':
			case 'c':
			case 'n':	// a PLAIN click (waypoint mode off): what the same clicks give without the waypoint key
			case 'x':
				if( xlat == nullptr )
				{
					fprintf( stderr, "[GXENTERQ] %c unavailable: no command translator\n", kind );
					break;
				}
				for( const char *a = args; *a; ++a )
				{
					Object *o = gxEnterqTarget( *a );
					if( o == nullptr || o->getDrawable() == nullptr )
					{
						fprintf( stderr, "[GXENTERQ] %c unavailable: target %c has %s\n", kind, *a, o ? "no drawable" : "no object" );
						continue;
					}
					if( kind == 'x' )
					{
						s_gxEnterqWatched.push_back( std::make_pair( *a, o->getID() ) );
						fprintf( stderr, "[GXENTERQ] x %c off=%s on=%s id=%u\n", *a,
							GameMessage::getCommandTypeAsString( gxEnterqClick( xlat, o, FALSE, CommandTranslator::EVALUATE_ONLY ) ),
							GameMessage::getCommandTypeAsString( gxEnterqClick( xlat, o, TRUE, CommandTranslator::EVALUATE_ONLY ) ),
							(unsigned)o->getID() );
					}
					else
						fprintf( stderr, "[GXENTERQ] %c %c -> %s id=%u\n", kind, *a,
							GameMessage::getCommandTypeAsString( gxEnterqClick( xlat, o, kind != 'n', CommandTranslator::DO_COMMAND ) ),
							(unsigned)o->getID() );
				}
				break;
			case 'Q':
				for( const char *a = args; *a; ++a )
					if( Object *o = gxEnterqTarget( *a ) )
						TheMessageStream->appendMessage( GameMessage::MSG_GX_QUEUE_ENTER )->appendObjectIDArgument( o->getID() );
				break;
			case 'w':
				for( const char *a = args; *a; ++a )
					if( Object *o = gxEnterqTarget( *a ) )
						TheMessageStream->appendMessage( GameMessage::MSG_ADD_WAYPOINT )->appendLocationArgument( *o->getPosition() );
				break;
			case 'P':
			case 's':
			{
				gxEnterqLocal( 0, v );
				if( v.empty() )
					break;
				Coord3D c;
				c.zero();
				for( size_t i = 0; i < v.size(); ++i )
				{
					c.x += v[ i ]->getPosition()->x / v.size();
					c.y += v[ i ]->getPosition()->y / v.size();
				}
				c.x += ( kind == 's' ) ? 150.0f : 0.0f;
				c.y += ( kind == 'P' ) ? 400.0f : 0.0f;
				c.z = TheTerrainLogic ? TheTerrainLogic->getGroundHeight( c.x, c.y ) : 0.0f;
				TheMessageStream->appendMessage( kind == 'P' ? GameMessage::MSG_ADD_WAYPOINT : GameMessage::MSG_DO_MOVETO )->appendLocationArgument( c );
				break;
			}
			case 'p':
				gxEnterqProbe();
				break;
			case 'o':	// the owner of every `x` target, by the id it had then (a hijack moves it out of its letter)
				for( size_t i = 0; i < s_gxEnterqWatched.size(); ++i )
				{
					const Object *o = TheGameLogic->findObjectByID( s_gxEnterqWatched[ i ].second );
					const Player *owner = ( o && !o->isEffectivelyDead() ) ? o->getControllingPlayer() : nullptr;
					fprintf( stderr, "[GXENTERQ] own f=%u %c id=%u owner=%d\n", (unsigned)TheGameLogic->getFrame(),
						s_gxEnterqWatched[ i ].first, (unsigned)s_gxEnterqWatched[ i ].second, owner ? owner->getPlayerIndex() : -1 );
				}
				fflush( stderr );
				break;
			case 'u':	// per-unit detail, for diagnosis: id, AI state, goal, container, queued entries
				gxEnterqLocal( 0, v );
				{
					std::vector<Object *> inside;
					for( Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject() )
						if( o->getControllingPlayer() == ThePlayerList->getLocalPlayer() && gxEnterqIsInfantry( o ) && o->getContainedBy() )
							inside.push_back( o );
					v.insert( v.end(), inside.begin(), inside.end() );
				}
				for( size_t i = 0; i < v.size(); ++i )
				{
					AIUpdateInterface *ai = v[ i ]->getAI();
					Object *goal = ai ? ai->getGoalObject() : nullptr;
					fprintf( stderr, "[GXENTERQ] unit f=%u id=%u state=%d goal=%u in=%u q=%d\n", (unsigned)TheGameLogic->getFrame(),
						(unsigned)v[ i ]->getID(), ai ? (Int)ai->getCurrentStateID() : -1, (unsigned)( goal ? goal->getID() : 0 ),
						(unsigned)( v[ i ]->getContainedBy() ? v[ i ]->getContainedBy()->getID() : 0 ), ai ? ai->gxTestQueueCount() : -1 );
				}
				break;
			case 'v':
				gxEnterqVisibility();
				break;
			default:
				fprintf( stderr, "[GXENTERQ] unknown step '%s'\n", step.str() );
				break;
		}
		// input=0 means the CommandTranslator destroys every order this step sent (a scripted no-input window, a pause)
		fprintf( stderr, "[GXENTERQ] step %s at f=%u input=%d\n", step.str(), (unsigned)TheGameLogic->getFrame(),
			TheInGameUI->getInputEnabled() ? 1 : 0 );
		fflush( stderr );
	}
}

static void gxTestEnterQueueScript( CommandTranslator *xlat )
{
	struct Step { UnsignedInt frame; AsciiString what; };
	static std::vector<Step> s_steps;
	static Int s_at = -1;
	if( s_at < 0 )
	{
		s_at = 0;
		AsciiString list( getenv( "GX_TEST_ENTERQ" ) ? getenv( "GX_TEST_ENTERQ" ) : "" ), entry, frame;
		while( list.nextToken( &entry, "," ) )
		{
			Step s;
			if( entry.nextToken( &frame, ":" ) && entry.isNotEmpty() )
			{
				s.frame = (UnsignedInt)atoi( frame.str() );
				s.what = entry.str() + ( entry.getCharAt( 0 ) == ':' ? 1 : 0 );	// nextToken leaves the separator on the rest
				s.what.trim();
				s_steps.push_back( s );
			}
		}
	}
	if( TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isLoadingMap() || ThePlayerList == nullptr
		|| ThePlayerList->getLocalPlayer() == nullptr || TheInGameUI == nullptr )
		return;
	while( s_at < (Int)s_steps.size() && TheGameLogic->getFrame() >= s_steps[ s_at ].frame )
		gxEnterqStep( xlat, s_steps[ s_at++ ].what );
}
#endif

// GeneralsX: translate this update's input and queue the orders on the network BEFORE the draw and its blocking
// Present, so an order is not queued a whole draw late. Same execution frame: no logic tick runs between here and
// GameEngine::update's own translation. The frame-ready decision stays after the draw. Rendered network matches
// only; if a GUI callback ends the match this returns FALSE. GX_H02_OFF=1 restores the old timing.
static Bool gxSendOrdersBeforeDraw( void )
{
	static const Bool s_off = getenv( "GX_H02_OFF" ) != nullptr;
	struct Gate { static Bool open() {
		return TheNetwork != nullptr && TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()
			&& !TheGameLogic->isLoadingMap(); } };
	if( s_off || TheGlobalData->m_headless || !Gate::open() )
		return TRUE;
	const int64_t t0 = gxGfxWallNs();
	TheMessageStream->propagateMessages();
	if( !Gate::open() )
		return FALSE;
	TheNetwork->sendLocalCommands();
	gxGfxClientExclude( gxGfxWallNs() - t0 );	// [GXGFX] cli= stays "client work outside the renderer and the orders"
	return TRUE;
}

void GameClient::update()
{
	USE_PERF_TIMER(GameClient_update)
	// GeneralsX: render-split step 1: render calls a time-frozen logic update queued (it skips
	// step) are replayed before anything below reads them. Nothing queued: a branch (GXRenderSplit.h).
	GXRenderSplit::get().tickEnd();
	// GeneralsX: First, so its clock read is as close to the loop's wake as it can be: the
	// render interpolation phase every drawable of this frame is drawn at (GXLogicPhase.h). One sample per update, on
	// purpose: camera tracking, the drawables' client updates, attached emitters and the draw all read this one phase
	// and so agree with each other; a second sample just before the draw would split them.
	// The client work before the draw adds its variable time as latency jitter; with interpolation, judder is cv
	// 0.037-0.038 at ~600 fps uncapped, against 2.9-3.9 without.
	TheDisplay->updateLogicPhase();
	gxShotClockTick();	// GeneralsX: before anything this update animates or draws
	gxTestSelect();	// GeneralsX: GX_TEST_SELECT: a Command Center's command bar for GX_SHOT
	gxOrderDriverUpdate();	// GeneralsX: GX_ORDERS: scripted human orders (GXOrderDriver.cpp); off when unset
	gxGymUpdate();	// GeneralsX: GX_OBS / GX_ACT: the ai-gym observer and bot input (GXGym.cpp); off when unset
	gxMassOrderUpdate();	// GeneralsX: GX_MASSORDER: a timed group order (GXMassOrder.cpp); off when unset

#if defined(GX_TEST_INSTRUMENTS)
	gxTestRevealScript();
	gxTestEnterQueueScript( m_commandTranslator );
	gxTestCheatScript();
#endif
	const Bool paceEmitted = gxPaceSample();
	gxGfxSample( paceEmitted );
	gxGfxClientBegin();		// [GXGFX] cli=: everything after the window cut, camera drivers included, to the end of DRAW
	// GeneralsX: render-split step 4: the sync phase publishes the drawables' interpolation
	// snapshots (GX_POSEPUB=1; off by default, Drawable.cpp gxPosePubOn), once per logic update, before anything below reads them (the camera drivers first); the draw only reads
	// them (GXPosePublish.h). Inside cli=, so the moved work stays in the client span.
	gxPublishDrawnPoses();
	gxPerfSample();
	gxReplayDiagSample();
	gxStatSample();
	gxPerfVerdictToast();
	gxCameraDrive();
	gxObserveDrive();
	gxCameraPan();
	gxCameraPitch();
	gxTestSaveAtFrame();
	gxTestQuitAtFrame();
	gxFastForwardSchedule();
	gxReplayStripUpdate();	// GeneralsX: a replay seek stops BETWEEN frames, never inside a message callback
	gxReplayPauseSchedule();	// GeneralsX: GX_REPLAY_PAUSE: render-split gate G0
	runPendingSaveLoad();	// a "load it anyway" Yes from the save gate, now that its box is gone

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

	//Update snow particles.
	if (TheSnowManager)
		TheSnowManager->UPDATE();

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


  if (TheInGameUI->isCameraTrackingDrawable())
  {
    Drawable *draw = TheInGameUI->getFirstSelectedDrawable();
    if ( draw )
    {
      // GeneralsX: Centre on the body as it is drawn this frame (render interpolation), or the
      // tracked unit judders against a camera stepping at the tick rate. Its logic position when
      // it is not blended.
      Coord3D pos = *draw->getPosition();
      pos.add(draw->getInterpolationOffset());
      TheTacticalView->userLookAt( &pos );
    }
    else
      TheInGameUI->setCameraTrackingDrawable( FALSE );
  }

	if (m_intro != nullptr)
	{
		// redraw all views, update the GUI
		TheDisplay->UPDATE();
		gxDrawTimed();
		return;
	}

	// GeneralsX: draws operator lines (GXSAY <room> 0.0.0.0 <text>) inside a match; the grey sender-zero line is drawn
	// only in the senderIp == 0 branch of WOLLobbyMenu.cpp. Runs ahead of TheShell->UPDATE() because the shell lobby
	// drain consumes the same 16-slot chat ring and the first to run takes the line, and above the RTS_DEBUG m_noDraw
	// return. No engine-class call site: it would render each line twice.
	gxMatchDrainOperatorLines();
	// GeneralsX: Out of the Online lobby for the match, back in after it
	// (GXMatchChat.h gxMatchPresenceGate) - a player in a game was also listed in the main lobby. And the
	// harness's one scripted in-match line (gxMatchTestSayPump, inert unless GX_TEST_MATCH_SAY is set).
	gxMatchPresenceGate();
	gxMatchTestSayPump();
	// GeneralsX: In-match kick, the kicked client's half (GXKickPump.h). Beside the
	// drain for the drain's reason: this is the one engine-driven per-frame GameClient call above every
	// early return, and it runs in a -headless peer too, which is what the harness drives.
	gxKickPump();
	// GeneralsX: The vote kick's lines, and a client kicked by a vote leaving 5 s later through the
	// moderator kick's own exit (a synchronised PLAYERLEAVE). Same place, same reason: it must run in a -headless peer too.
	if( gxVoteKickPump() )
		gxKickedExit();

	// update the window system itself
	{
		TheWindowManager->UPDATE();
	}

	// GeneralsX: Keep-alive for the floating Debug button: rebuilds it if
	// the display resolution changed, since a runtime-created window gets none of the .wnd parser's
	// resolution scaling and no layout owns it. Two integer compares in the steady state.
	DebugFloatButtonUpdate();
	GXDiscordUpdate();
	// GeneralsX: Consume a finished Discord login: verify it, write it,
	// refresh the identity. Ticked from the engine rather than from the lobby screen that started
	// it, because a login outlives that screen - the player can walk back to the main menu while
	// the browser is still open, and a pump hung off a UI callback goes deaf the moment they do.
	// That is the same defect the LANAPI pump had, and the lesson is.
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
			// GeneralsX: retail gate, with or without GX_NOSHROUD. getShroudedStatus() is a lazy cache
			// (PartitionData::m_shroudedness / m_everSeenByPlayer) that logic reads back, so it must recompute on the same
			// frames as on every other peer; the probe changes drawing only. No logic decision reads m_everSeenByPlayer now
			// (ActionManager.cpp tests the cells), so the schedule matches retail but decides nothing another peer can see.
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
//		TheParticleSystemManager->update();

	}

	// update the terrain visuals
	{
		// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
		TheTerrainVisual->UPDATE();
		// GX-SIMID-END
	}

	// GeneralsX: The nuke shockwave's radius for this logic frame, before the display updates and
	// draws the particle systems it steers.
	missileVisualShockTick();

	// update display
	{
		TheDisplay->UPDATE();
	}

	if( !gxSendOrdersBeforeDraw() )	// GeneralsX: H02: orders out before the draw + Present
		return;		// a translation ended or replaced the match (see gxSendOrdersBeforeDraw)

	{
		USE_PERF_TIMER(GameClient_draw)

	// redraw all views, update the GUI
	//if(TheGameLogic->getFrame() >= 2)

		gxDrawTimed();
		gxGfxClientEnd();
	}

	// GeneralsX: GX_SHOT capture, right after this frame was drawn and presented: the image is
	// the state at the logged logic frame. It reads the back buffer after Present, as the screenshot hotkey does
	// (CommandXlat runs in propagateMessages: after this update, or in a rendered network match before
	// this draw, reading the previous frame's presented buffer). D3D leaves a DISCARD back buffer undefined
	// after Present; under DXVK the last rendered image stays there, and the hotkey
	// has always relied on the same.
	gxShotDrive();
	missileVisualTestFire();	// GX_TEST_FIRESW: test input only

	{
		// let display string factory handle its update
		TheDisplayStringManager->update();
	}

	{
		// update the shell
		TheShell->UPDATE();
	}

	// GeneralsX: The Friends overlay, wherever F5 opened it. Right after the shell
	// because that is where the lobby screen used to pump it from.
	GXBuddyOverlayPump();

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
	// GeneralsX: render-split step 1: the logic tick ended, replay the render calls it queued
	// (GXRenderSplit.h). Before Display::step, so a step reads the same UI as before.
	GXRenderSplit::get().tickEnd();
	TheDisplay->step();
}

// GeneralsX: GameLogic::update calls this at the start of every logic update: the tick-start
// stamp the render interpolation phase estimates the lockstep grid from (GXLogicPhase.h).
void GameClient::setFrame( UnsignedInt frame )
{
	m_frame = frame;
	if (TheDisplay != nullptr)
		TheDisplay->noteLogicTick(frame);
	// GeneralsX: render-split step 1: from here to step() is the tick window - the converted
	// render entry points defer what the simulation calls (GXRenderSplit.h). The frame labels step 6's [GXRSPART] trace.
	GXRenderSplit::get().tickBegin(frame);
}

void GameClient::updateHeadless()
{
	// GeneralsX: render-split step 1: what GameClient::update's first line does for a rendered
	// peer - replay what a time-frozen logic update left queued (GXRenderSplit.h).
	GXRenderSplit::get().tickEnd();
	// GeneralsX: GX_MISSILEVIS=1 runs the superweapon missiles' visual flight on a headless peer
	// too - it normally runs only from updateDrawable(), which headless never calls. Two uses: its [GXMVIS] trace records
	// the real logic flights, and a headless CRC A/B with it on proves the code that reads logic state (NeutronMissileUpdate
	// through its own save path included) leaves the simulation byte-identical.
	if( missileVisualTraceOn() )
	{
		for( Drawable *draw = m_drawableList; draw; draw = draw->getNextDrawable() )
			draw->updateVisualTransform();
	}

	// TheSuperHackers @info helmutbuhler 03/05/2025 bobtista 02/02/2026
	// Update particles to prevent accumulation in headless mode. Particles are generated
	// during GameLogic and only cleaned up during rendering. update() lets particles finish
	// their lifecycle naturally instead of abruptly removing them with reset().
	TheParticleSystemManager->update();

	gxGymUpdate();	// GeneralsX: GX_OBS on a -headless -replay: the state stream of a recorded match
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
		// GeneralsX: A missile drawn along its visual flight (MissileVisual.h) is always walked:
		// the region is the view's ground footprint tested against the LOGIC position, and a missile thousands of units
		// up is on screen while neither position is inside it. Skipped, W3DModelDraw::reactToTransformChange would leave
		// the model at the logic transform - the missile would jump back onto the logic path for those frames.
		if( region == nullptr || draw->getVisualTransform() != nullptr ||
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
	for( Drawable *draw = getDrawableList(); draw; draw = draw->getNextDrawable() )
	{
		const Object *object=draw->getObject();

		if( object && object->isKindOf( KINDOF_FS_FAKE ) )
		{
			Relationship rel = rts::getObservedOrLocalPlayer()->getRelationship(object->getTeam());
			if (rel == ALLIES || rel == NEUTRAL)
				draw->setTerrainDecal(TERRAIN_DECAL_SHADOW_TEXTURE);
			else
				draw->setTerrainDecal(TERRAIN_DECAL_NONE);
		}
	}
}

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
//	m_drawableHash[ draw->getID() ] = draw;
	DrawableID newID = draw->getID();
	while( newID >= m_drawableVector.size() ) // Fail case is hella rare, so faster to double up on size() call
		m_drawableVector.resize(m_drawableVector.size() * 2, nullptr);

	m_drawableVector[ newID ] = draw;

}
// GX-SIMID-END

// ------------------------------------------------------------------------------------------------
/** Remove drawable from lookup table of fast id searching */
// ------------------------------------------------------------------------------------------------
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void GameClient::removeDrawableFromLookupTable( Drawable *draw )
{

	// sanity
	// TheSuperHackers @fix Mauller/Xezon 24/04/2025 Prevent out of range access to vector lookup table
	if( draw == nullptr || static_cast<size_t>(draw->getID()) >= m_drawableVector.size() )
		return;

	// remove from table
//	m_drawableHash.erase( draw->getID() );
	m_drawableVector[ draw->getID() ] = nullptr;

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

	// GeneralsX: [GXPRELOAD]: one line per preload, the canary that it ran on this peer, what it cost,
	// and that it left the DrawableID counter and both random streams exactly where they were.
	const long long gxT0 = gxPerfMonoNs();
	const DrawableID gxIdBefore = m_nextDrawableID;
	const UnsignedInt gxClientRngBefore = GetGameClientRandomSeedCRC();
	const UnsignedInt gxLogicRngBefore = GetGameLogicRandomSeedCRC();
	Int gxTemplates = 0, gxModules = 0;

	// first, for every drawable in the map load the assets for all states we care about
	Drawable *draw;
	for( draw = firstDrawable(); draw; draw = draw->getNextDrawable() )
		draw->preloadAssets( timeOfDay );

	//
	// now load the models of each of the faction things we can create
	//
	// GeneralsX: From each template's draw module data, NOT from a temporary Drawable as retail did.
	// Creating one takes a DrawableID and can draw the client random stream (W3DModelDraw picks an initial animation), and
	// both reach saves that the CRC never sees (PrisonBehavior saves a DrawableID; StealthUpdate saves a client random draw).
	// The models are the same: Drawable::preloadAssets reaches only W3DModelDraw::preloadAssets, which reads nothing but its
	// module data - and only the modules the Drawable constructor would have made (the draw-module LOD filter).
	const ThingTemplate *tTemplate;
	for( tTemplate = TheThingFactory->firstTemplate();
			 tTemplate;
			 tTemplate = tTemplate->friend_getNextTemplate() )
	{

		// if this isn't one of the objects that can be preloaded ignore it
		if( tTemplate->isKindOf( KINDOF_PRELOAD ) == FALSE && !TheGlobalData->m_preloadEverything )
			continue;

		++gxTemplates;
		const ModuleInfo& drawMI = tTemplate->getDrawModuleInfo();
		for( Int i = 0; i < drawMI.getCount(); ++i )
		{
			const ModuleData* md = drawMI.getNthData( i );
			if( md == nullptr || (TheGlobalData->m_useDrawModuleLOD &&
					md->getMinimumRequiredGameLOD() > TheGameLODManager->getStaticLODLevel()) )
				continue;
			const W3DModelDrawModuleData* w3dMD = md->getAsW3DModelDrawModuleData();
			if( w3dMD )
			{
				w3dMD->preloadAssets( timeOfDay, tTemplate->getAssetScale() );
				++gxModules;
			}
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
	extern std::vector<AsciiString>	debrisModelNamesMapHack;
	size_t i=0;
	for (; i<debrisModelNamesGlobalHack.size(); ++i)
	{
		TheDisplay->preloadModelAssets(debrisModelNamesGlobalHack[i]);
	}
	for (i=0; i<debrisModelNamesMapHack.size(); ++i)
		TheDisplay->preloadModelAssets(debrisModelNamesMapHack[i]);
	GlobalMemoryStatus(&after);
	// GeneralsX: Not cleared any more: the base list is filled once, when the INI is parsed, and every
	// map load frees the models again (W3DDisplay::doSmartAssetPurgeAndPreload), so clearing it left every match after the
	// first of a session without them. This map's own override names are in debrisModelNamesMapHack, which
	// ObjectCreationListStore::reset clears with the overrides.

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

	fprintf(stderr, "[GXPRELOAD] tod=%d templates=%d models=%d debris=%d ms=%.1f drawableID=%u->%u clientRNG=%08X->%08X logicRNG=%08X->%08X\n",
		(Int)timeOfDay, gxTemplates, gxModules, (Int)(debrisModelNamesGlobalHack.size() + debrisModelNamesMapHack.size()), (gxPerfMonoNs() - gxT0) / 1e6,
		(UnsignedInt)gxIdBefore, (UnsignedInt)m_nextDrawableID, gxClientRngBefore, GetGameClientRandomSeedCRC(),
		gxLogicRngBefore, GetGameLogicRandomSeedCRC());

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


	// GeneralsX: a map script that switched the sky off keeps it off after a
	// load (W3DWater.cpp's latch; the engine reset before the load cleared it). Version 3 saves read as not switched off.
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
