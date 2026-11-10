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

// FILE: LoadScreen.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Mar 2002
//
//	Filename: 	LoadScreen.cpp
//
//	author:		Chris Huybregts
//
//	purpose:	Contains each of the different derived LoadClasses for each of the
//						Different kind of games we can have.
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "GameClient/GXTextColor.h"
#include "Common/NameKeyGenerator.h"
#include "Common/AudioAffect.h"
#include "Common/AudioEventRTS.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/GameLOD.h"
#include "Common/GameState.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/Display.h"
#include "GameClient/GadgetProgressBar.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Keyboard.h"
#include "GameClient/LoadScreen.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/Shell.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/WindowVideoManager.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameLogic/FPUControl.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/RankPointValue.h"
#include "GameClient/SaveLoadFeedback.h"	// notePreventedLoadCrash

//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
// PRIVATE TYPES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC DATA ////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
void positionStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void updateMapStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], Bool onLoadScreen = FALSE );
void positionAdditionalImages( MapMetaData *mmd, GameWindow *mapWindow, Bool force);

enum{
FRAME_TITLES_START = 20,
FRAME_TELETYPE_START = 24,
FRAME_FUDGE_ADD = 30,
FRAME_PORTRAITS_START = 35,
FRAME_OUTER_CIRCLE_LINE_SHOW = 50,
FRAME_INNER_CIRCLE_LINE_SHOW = 52,
FRAME_OUTER_CIRCLE_ALPHA_SHOW = 63,
FRAME_INNER_CIRCLE_ALPHA_SHOW = 74,
FRAME_OUTER_CIRCLE_LINE_HIDE = 75,
FRAME_INNER_BACKDROP_ALPHA_SHOW = 80,
FRAME_INNER_CIRCLE_LINE_HIDE = 81,
FRAME_VS_ANIM_START = 98,
FRAME_RIGHT_VOICE = 140,
};

static const Int TELETYPE_UPDATE_FREQ = 2; // how many frames between teletype updates



//-----------------------------------------------------------------------------
// GeneralsX: The row builder GeneralsMD registers. See LoadScreen.h for
// why this is a hook and not a direct call: this file is Core and CodeGuiRowsEnsure is not.
//
// Null until someone registers one, and null forever in the Generals twin - which is the point.
// Every call site below is therefore exactly as it was for any game that does not register.
//-----------------------------------------------------------------------------
static LoadScreenRowBuilder s_loadScreenRowBuilder = nullptr;

void LoadScreenSetRowBuilder( LoadScreenRowBuilder builder )
{
	s_loadScreenRowBuilder = builder;
}

Int LoadScreenEnsureRows( GameWindow *parent, const char *idFormat, Int rowCount, Bool placedByCaller )
{
	if( s_loadScreenRowBuilder == nullptr || parent == nullptr || idFormat == nullptr )
		return 0;
	return s_loadScreenRowBuilder( parent, idFormat, rowCount, placedByCaller );
}

// GeneralsX: The splitter GeneralsMD registers; see LoadScreen.h. Null in the twin.
static LoadScreenRowSplitter s_loadScreenRowSplitter = nullptr;

void LoadScreenSetRowSplitter( LoadScreenRowSplitter splitter )
{
	s_loadScreenRowSplitter = splitter;
}

void LoadScreenSplitRows( GameWindow *parent, const char * const *idFormats, Int columnCount,
													const char * const *titleNames, Int titleCount, Int usedRows )
{
	if( s_loadScreenRowSplitter == nullptr || parent == nullptr || idFormats == nullptr )
		return;
	s_loadScreenRowSplitter( parent, idFormats, columnCount, titleNames, titleCount, usedRows );
}

//-----------------------------------------------------------------------------
// LoadScreen Class
//-----------------------------------------------------------------------------

LoadScreen::LoadScreen()
{
	m_loadScreen = nullptr;
	m_gxWarmWaited = FALSE;
	m_gxWarmText = nullptr;
}

LoadScreen::~LoadScreen()
{
	if(m_loadScreen)
		TheWindowManager->winDestroy( m_loadScreen );
}

void LoadScreen::update( Int percent )
{
	TheGameEngine->serviceWindowsOS();
	if (TheGameEngine->getQuitting() || (TheGameLogic && TheGameLogic->isQuitToDesktopRequested()))
		return;	//don't bother with any of this if the player is exiting game.

	// GeneralsX: A2: the first call of a session starts the pipeline warm-up, so it compiles
	// while the map loads; later calls only read the pending count.
	TheDisplay->warmPipelines();

	TheWindowManager->update();
	TheDisplay->update();
	// redraw all views, update the GUI
	TheDisplay->draw();

	setFPMode();

	// 100 = LOAD_PROGRESS_END (GameLogic.cpp): the load is done, the barrier and the first frame are next. The single-
	// player screens rescale before calling here ((p + 30) / 1.3), which maps 100 - and the barrier's 101 - to 100, so
	// the flag, set before the wait, is what makes it once per screen and keeps the wait's own update(101) out.
	if (percent == 100 && !m_gxWarmWaited)
	{
		m_gxWarmWaited = TRUE;
		waitForPipelineWarmup();
	}
}

// GeneralsX: A2: GX_WARM_SHOT=1 saves one screenshot at the load's end - of the held screen with its
// message, or of a load that did not hold (no message). The proof shots. Unset: one cached getenv, nothing else. Off
// on a -headless peer, which never creates the device takeScreenShot reads (as gxShotCaptureOn).
// The screenshot reads back buffer 0 after a present, and DXVK ROTATES the back buffers at every present
// (D3D9SwapChainEx::Present, m_backBuffers[i]->Swap): buffer 0 is then one drawn one or two presents earlier. The first
// iPad shot (IT2, 01/10) showed the screen from BEFORE the message for that reason (had said so; declined
// then, wrongly). So three more barrier passes first - at most three buffers rotate (two back buffers plus the front
// one) - each drawing what the shot is to show. Test runs only: it lengthens a load by three draws.
static void gxWarmShot( LoadScreen *screen, Bool message )
{
	static const Bool s_on = getenv("GX_WARM_SHOT") != nullptr && !(TheGlobalData != nullptr && TheGlobalData->m_headless);
	if (!s_on)
		return;
	for (Int i = 0; i < 3; ++i)
		screen->update(101);
	TheDisplay->takeScreenShot(SCREENSHOT_PNG);
	fprintf(stderr, "[GXWARM] shot message=%d\n", message);
}

// GeneralsX: A2 (wh1ter0se 02/10: "show the message top left 'Optimizing shaders...' with a
// 0.00-100.00% info"): one left-aligned line in the screen's top-left corner, in the face and size of the render-fps/
// clock readout InGameUI draws there (Tahoma bold, its size, resolution-adjusted), white with a black border, one row below that
// readout. Not a .wnd text's font: the multiplayer screens' features font is so large the line ran into the general's
// name under it (rc-check RD3, 02/10). Nothing drawn behind it (defaultVisual FALSE, as MainMenu.cpp's credit label).
// Hidden until a hold shows it; a child of the screen, so it goes with it.
static GameWindow *gxWarmTextTopLeft( GameWindow *screen )
{
	// Not on a -headless peer: its window manager is the dummy, whose static text has no TextData for the gadget's own
	// destroy to free. Nothing is drawn there anyway.
	if (screen == nullptr || TheWindowManager == nullptr || TheFontLibrary == nullptr
		|| (TheGlobalData != nullptr && TheGlobalData->m_headless))
		return nullptr;
	Int screenW, screenH;
	screen->winGetSize(&screenW, &screenH);
	// The readout's own size (8 when the readout is off), capped at 24 as InGameUI caps it, so a player who enlarged the
	// readout still gets the line one of its rows below it, not over it (review P3, 02/10).
	Int pointSize = (TheGlobalData != nullptr && TheGlobalData->m_renderFpsFontSize > 0) ? TheGlobalData->m_renderFpsFontSize : 8;
	if (pointSize > 24)
		pointSize = 24;
	GameFont *font = TheFontLibrary->getFont("Tahoma",
		TheGlobalLanguageData != nullptr ? TheGlobalLanguageData->adjustFontSize(pointSize) : pointSize, TRUE);
	const Int lineH = (font != nullptr && font->height > 0) ? font->height : 16;
	WinInstanceData instData;
	TextData textData;
	instData.init();
	memset(&textData, 0, sizeof(textData));
	instData.m_style = GWS_STATIC_TEXT;
	GameWindow *text = TheWindowManager->gogoGadgetStaticText(screen, WIN_STATUS_ENABLED | WIN_STATUS_HIDDEN,
		lineH / 2, lineH * 2, screenW / 2, lineH * 3 / 2, &instData, &textData, font, FALSE);
	if (text != nullptr)
		text->winSetEnabledTextColors(GameMakeColor(255, 255, 255, 255), GameMakeColor(0, 0, 0, 255));
	return text;
}

// The warm-up line with its progress: the share of the pipelines pending at the hold's start that are compiled now.
static void gxWarmProgress( GameWindow *text, UnsignedInt pendingAtEntry, UnsignedInt pending )
{
	const Real percent = pending >= pendingAtEntry ? 0.0f : 100.0f * Real(pendingAtEntry - pending) / Real(pendingAtEntry);
	GadgetStaticTextSetText(text, TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:GXOptimizingShaders",
		L"Optimizing shaders... %.2f%%", percent));
}

// GeneralsX: A2: hold the load screen until every recorded graphics pipeline is compiled, so no
// first draw in the match compiles one (13 of the 16 stops over 100 ms in modernize/HANDBACK_s0-hitch-attribution.md).
// Before GameLogic reports the load complete, so a slow compile is load time, never a lockstep stop. Each pass is
// update(101) - the subclass's own barrier pass, the one GameLogic's barrier makes: network keep-alive, the LAN start
// resend, a redraw. Capped at 20 s, well inside the 60 s progress barrier a peer waits in.
void LoadScreen::waitForPipelineWarmup()
{
	const UnsignedInt pendingAtEntry = TheDisplay->warmPipelines();
	if (pendingAtEntry == 0)
	{
		fprintf(stderr, "[GXWARM] load-end pending=0 wait_ms=0\n");
		gxWarmShot(this, FALSE);
		return;
	}
	const UnsignedInt start = timeGetTime();
	UnsignedInt pending = pendingAtEntry;
	// GeneralsX: A2 (wh1ter0se 01/10: "with a clear message"): say what the hold is, in the screen's
	// own text element so it looks like the rest of the screen, once the hold has lasted long enough to notice - a shorter
	// one would only flash a sentence. Put back after: a LAN screen goes on to wait for the other players.
	UnicodeString savedText;
	Bool savedHidden = FALSE;
	Int messageMs = -1;
	while (pending > 0 && timeGetTime() - start < 20000 && !TheGameEngine->getQuitting()
		&& !(TheGameLogic && TheGameLogic->isQuitToDesktopRequested()))	// the same two exits update() honours
	{
		const Bool showNow = messageMs < 0 && m_gxWarmText != nullptr && timeGetTime() - start >= 250;
		if (showNow)
		{
			messageMs = timeGetTime() - start;
			savedText = GadgetStaticTextGetText(m_gxWarmText);
			savedHidden = m_gxWarmText->winIsHidden();
			m_gxWarmText->winHide(FALSE);
		}
		// Not "first launch ... happens once": macOS rebuilds its pipelines from the cache on every start, so a later
		// launch holds ~1.5 s too and shows this line (rc-check-1002b, Mini, 02/10).
		if (messageMs >= 0)
			gxWarmProgress(m_gxWarmText, pendingAtEntry, pending);
		update(101);
		if (showNow)
			gxWarmShot(this, TRUE);
		Sleep(10);	// the compile workers are the bottleneck; a present that does not block must not spin against them
		pending = TheDisplay->warmPipelines();
	}
	if (messageMs >= 0)
	{
		gxWarmProgress(m_gxWarmText, pendingAtEntry, pending);	// the final count: 100.00% unless the 20 s cap or a quit ended it
		gxWarmShot(this, TRUE);	// and as the hold ends: the startup screen fades in under the main menu's first-run fade
		GadgetStaticTextSetText(m_gxWarmText, savedText);
		m_gxWarmText->winHide(savedHidden);
	}
	else
		gxWarmShot(this, FALSE);	// a hold too short to say anything
	fprintf(stderr, "[GXWARM] load-end pending=%u wait_ms=%u left=%u message_ms=%d\n", pendingAtEntry, timeGetTime() - start,
		pending, messageMs);
}


// SinglePlayerLoadScreen Class ///////////////////////////////////////////////
//-----------------------------------------------------------------------------
SinglePlayerLoadScreen::SinglePlayerLoadScreen()
{
	m_currentObjectiveLine = 0;
	m_currentObjectiveLineCharacter = 0;
	m_finishedObjectiveText = FALSE;
	m_currentObjectiveWidthOffset = 0;
	m_progressBar = nullptr;
	m_percent = nullptr;
	m_videoStream = nullptr;
	m_videoBuffer = nullptr;
	m_objectiveWin = nullptr;
	for(Int i = 0; i < MAX_OBJECTIVE_LINES; ++i)
		m_objectiveLines[i] = nullptr;

}

SinglePlayerLoadScreen::~SinglePlayerLoadScreen()
{
	delete m_videoBuffer;

	if ( m_videoStream )
	{
		m_videoStream->close();
	}

	TheAudio->removeAudioEvent( m_ambientLoopHandle );
}

void SinglePlayerLoadScreen::moveWindows( Int frame )
{
	enum{
		STATE_BEGIN = 250,
		STATE_SHOW_LOCATION = 251,
		STATE_BEGIN_BRIEFING = 255,
//		STATE_BEGIN_ANIMATING_TEXT = 250,
		STATE_SHOW_CAMEO_1 = 434,
		STATE_BEGIN_ANIMATING_TEXT = 356,
		STATE_HIDE_CAMEO_1 = 459,
		STATE_SHOW_CAMEO_2 = 464,
		STATE_HIDE_CAMEO_2 = 492,
		STATE_SHOW_CAMEO_3 = 497,
		STATE_HIDE_CAMEO_3 = 524,
//		STATE_END_ANIM_HEAD = 450,
		STATE_END_ANIMATING_TEXT = 730,
		STATE_END = 730
	};
	if(frame < STATE_BEGIN || frame > STATE_END)
		return;

	if( frame == STATE_BEGIN_BRIEFING)
	{
		// add sound support here
		TheAudio->friend_forcePlayAudioEventRTS(&TheCampaignManager->getCurrentMission()->m_briefingVoice);
	}

	if( frame == STATE_BEGIN_ANIMATING_TEXT)
	{
		m_objectiveWin->winHide(FALSE);
		// animate the text and stuff
	}

	if( frame > STATE_BEGIN_ANIMATING_TEXT && frame <= STATE_END_ANIMATING_TEXT && !m_finishedObjectiveText)
	{
		if(m_currentObjectiveLineCharacter >= m_unicodeObjectiveLines[m_currentObjectiveLine].getLength() )
		{
			m_currentObjectiveLine++;
			m_currentObjectiveLineCharacter =0;
		}
		if(m_currentObjectiveLine >= MAX_OBJECTIVE_LINES || m_unicodeObjectiveLines[m_currentObjectiveLine].isEmpty())
		{
			m_finishedObjectiveText = TRUE;
		}
		else
		{
			WideChar wChar = m_unicodeObjectiveLines[m_currentObjectiveLine].getCharAt(m_currentObjectiveLineCharacter);
			UnicodeString text = GadgetStaticTextGetText(m_objectiveLines[m_currentObjectiveLine]);
			text.concat(wChar);
			GadgetStaticTextSetText(m_objectiveLines[m_currentObjectiveLine], text);

		}
		m_currentObjectiveLineCharacter++;
	}
	switch (frame) {

	case STATE_SHOW_LOCATION:
		m_location->winHide(FALSE);
		break;
	case STATE_SHOW_CAMEO_1:
		m_unitDesc[0]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_1:
		m_unitDesc[0]->winHide(TRUE);
		break;
	case STATE_SHOW_CAMEO_2:
		m_unitDesc[1]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_2:
		m_unitDesc[1]->winHide(TRUE);
		break;
	case STATE_SHOW_CAMEO_3:
		m_unitDesc[2]->winHide(FALSE);
		break;
	case STATE_HIDE_CAMEO_3:
		m_unitDesc[2]->winHide(TRUE);
		break;
	}

}
/*
	static Bool on = FALSE;
	static ICoord2D startPos, endPos;
	enum{
		STATE_BEGIN = 275,
		STATE_BEGIN_ANIM = 290,
		STATE_ANIM_CAMEO1 = 300,
		STATE_ANIM_CAMEO1_TRANSITION_CAMEO2 = 350,
		STATE_ANIM_CAMEO2 = 400,
		STATE_ANIM_CAMEO2_TRANSITION_CAMEO3 = 450,
		STATE_ANIM_CAMEO3 = 500,
		STATED_END_ANIM = 550,
		STATE_END = 800
	};
	if(frame < STATE_BEGIN)
		return;
	else if(frame == STATE_BEGIN )
	{
		m_cameoWindow1->winHide(FALSE);
		m_cameoWindow2->winHide(FALSE);
		m_cameoWindow3->winHide(FALSE);
		m_cameoFrame->winHide(FALSE);
	}
	else if( frame == STATE_ANIM_CAMEO1)
	{
		m_cameoWindow1->winEnable(TRUE);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[0]));
		//save of positions
	}
	else if( frame == STATE_ANIM_CAMEO1_TRANSITION_CAMEO2)
	{
		m_cameoWindow1->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		ICoord2D tempPos;
		Int xOffset;
		m_cameoFrame->winGetPosition(&startPos.x, &startPos.y);
		m_cameoWindow1->winGetPosition(&tempPos.x, &tempPos.y);
		xOffset = tempPos.x - startPos.x;
		m_cameoWindow2->winGetPosition(&endPos.x, &endPos.y);
		endPos.x = endPos.x - xOffset;
		endPos.y = startPos.y;

	}
	else if( frame > STATE_ANIM_CAMEO1_TRANSITION_CAMEO2 && frame < STATE_ANIM_CAMEO2)
	{

		//extrapolate between start and end pos
		Real percent = INT_TO_REAL((frame - STATE_ANIM_CAMEO1_TRANSITION_CAMEO2)) / (STATE_ANIM_CAMEO2 - STATE_ANIM_CAMEO1_TRANSITION_CAMEO2);
		m_cameoFrame->winSetPosition(startPos.x + (endPos.x - startPos.x) * percent, endPos.y);
	}
	else if( frame == STATE_ANIM_CAMEO2 )
	{
		m_cameoWindow2->winEnable(TRUE);
		m_cameoFrame->winSetPosition(endPos.x, endPos.y);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[1]));
	}
	else if( frame == STATE_ANIM_CAMEO2_TRANSITION_CAMEO3)
	{
		m_cameoWindow2->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		ICoord2D tempPos;
		Int xOffset;
		m_cameoFrame->winGetPosition(&startPos.x, &startPos.y);
		m_cameoWindow2->winGetPosition(&tempPos.x, &tempPos.y);
		xOffset = tempPos.x - startPos.x;
		m_cameoWindow3->winGetPosition(&endPos.x, &endPos.y);
		endPos.x = endPos.x - xOffset;
		endPos.y = startPos.y;

	}
	else if( frame > STATE_ANIM_CAMEO2_TRANSITION_CAMEO3 && frame < STATE_ANIM_CAMEO3)
	{

		//extrapolate between start and end pos
		Real percent = INT_TO_REAL((frame - STATE_ANIM_CAMEO2_TRANSITION_CAMEO3)) / (STATE_ANIM_CAMEO3 - STATE_ANIM_CAMEO2_TRANSITION_CAMEO3);
		m_cameoFrame->winSetPosition(startPos.x + (endPos.x - startPos.x) * percent, endPos.y);
	}
	else if( frame == STATE_ANIM_CAMEO3 )
	{
		m_cameoFrame->winSetPosition(endPos.x, endPos.y);
		m_cameoWindow3->winEnable(TRUE);
		GadgetStaticTextSetText(m_cameoText, TheGameText->fetch(TheCampaignManager->getCurrentMission()->m_cameoImageName[2]));
	}
	else if( frame ==STATED_END_ANIM)
	{
		m_cameoWindow3->winEnable(FALSE);
		GadgetStaticTextSetText(m_cameoText, UnicodeString::TheEmptyString);
		m_cameoFrame->winHide(TRUE);

	}
}*/

void SinglePlayerLoadScreen::init( GameInfo *game )
{
	//No music in SinglePlayerLoadScreen

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/SinglePlayerLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the single player loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();
//	Mission *mission = TheCampaignManager->getCurrentMission();
	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );

	m_percent = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:Percent" ));
	DEBUG_ASSERTCRASH(m_percent, ("Can't initialize the m_percent for the single player loadscreen"));
	GadgetStaticTextSetText(m_percent,L"0%");
	m_percent->winHide(TRUE);

	m_objectiveWin = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ObjectivesWin" ));
	DEBUG_ASSERTCRASH(m_objectiveWin, ("Can't initialize the m_objectiveWin for the single player loadscreen"));
	m_objectiveWin->winHide(TRUE);


	Mission *mission = TheCampaignManager->getCurrentMission();
	AsciiString lineName;
	Int i = 0;
	for(; i < MAX_OBJECTIVE_LINES; ++i)
	{
		lineName.format("SinglePlayerLoadScreen.wnd:StaticTextLine%d",i);
		m_objectiveLines[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( lineName ));
		DEBUG_ASSERTCRASH(m_objectiveLines[i], ("Can't initialize the m_objectiveLines[%d] for the single player loadscreen", i));
		GadgetStaticTextSetText(m_objectiveLines[i],UnicodeString::TheEmptyString);

		// translate the objective lines
		if(mission->m_missionObjectivesLabel[i].isNotEmpty())
			m_unicodeObjectiveLines[i] = TheGameText->fetch(mission->m_missionObjectivesLabel[i]);
	}
	m_gxWarmText = gxWarmTextTopLeft( m_loadScreen );	// GeneralsX A2

	for(i = 0; i < MAX_DISPLAYED_UNITS; ++i)
	{
		lineName.format("SinglePlayerLoadScreen.wnd:StaticTextCameoText%d",i);
		m_unitDesc[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( lineName ));
		DEBUG_ASSERTCRASH(m_unitDesc[i], ("Can't initialize the m_objectiveLines[%d] for the single player loadscreen", i));
		GadgetStaticTextSetText(m_unitDesc[i],TheGameText->fetch(mission->m_unitNames[i]));
		m_unitDesc[i]->winHide(TRUE);
	}
	m_location = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:StaticTextCameoText3" ));
	DEBUG_ASSERTCRASH(m_location, ("Can't initialize the m_objectiveWin for the single player loadscreen"));
	m_location->winHide(TRUE);
	GadgetStaticTextSetText(m_location, TheGameText->fetch(mission->m_locationNameLabel));



	m_currentObjectiveLine = 0;
	m_currentObjectiveWidthOffset = 0;
	m_currentObjectiveLineCharacter = 0;
	m_finishedObjectiveText = FALSE;
/*
	m_cameoWindow1 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo1" ));
	DEBUG_ASSERTCRASH(m_cameoWindow1, ("Can't initialize the m_cameoWindow1 for the single player loadscreen"));
	m_cameoWindow1->winHide(TRUE);
	m_cameoWindow1->winEnable(FALSE);
	m_cameoWindow1->winSetEnabledImage(0, mission->m_cameoImage[0]);
	m_cameoWindow1->winSetDisabledImage(0, mission->m_cameoDisabledImage[0]);

	m_cameoWindow2 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo2" ));
	DEBUG_ASSERTCRASH(m_cameoWindow2, ("Can't initialize the m_cameoWindow2 for the single player loadscreen"));
	m_cameoWindow2->winHide(TRUE);
	m_cameoWindow2->winEnable(FALSE);
	m_cameoWindow2->winSetEnabledImage(0, mission->m_cameoImage[1]);
	m_cameoWindow2->winSetDisabledImage(0, mission->m_cameoDisabledImage[1]);

	m_cameoWindow3 = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowCameo3" ));
	DEBUG_ASSERTCRASH(m_cameoWindow3, ("Can't initialize the m_cameoWindow3 for the single player loadscreen"));
	m_cameoWindow3->winHide(TRUE);
	m_cameoWindow3->winEnable(FALSE);
	m_cameoWindow3->winSetEnabledImage(0, mission->m_cameoImage[2]);
	m_cameoWindow3->winSetDisabledImage(0, mission->m_cameoDisabledImage[2]);

	m_headMovie = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowHead" ));
	DEBUG_ASSERTCRASH(m_headMovie, ("Can't initialize the m_headMovie for the single player loadscreen"));
	m_headMovie->winHide(TRUE);
	m_cameoFrame = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:WindowHiliteCameo" ));
	DEBUG_ASSERTCRASH(m_cameoFrame, ("Can't initialize the m_cameoFrame for the single player loadscreen"));
	m_cameoFrame->winHide(TRUE);
	m_cameoText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:StaticTextCameoText" ));
	DEBUG_ASSERTCRASH(m_cameoText, ("Can't initialize the m_cameoText for the single player loadscreen"));

*/
	m_ambientLoop.setEventName("LoadScreenAmbient");
	// create the new stream
	// GeneralsX: No current mission (bad campaign data): treat it like a movie that cannot open. Idea from Generals Online 3e6a7186c, reimplemented.
	const Mission *currentMission = TheCampaignManager->getCurrentMission();
	m_videoStream = currentMission ? TheVideoPlayer->open( currentMission->m_movieLabel ) : nullptr;
	if ( m_videoStream == nullptr )
	{
		m_percent->winHide(TRUE);
		return;
	}

	// Create the new buffer
	m_videoBuffer = TheDisplay->createVideoBuffer();
	if (	m_videoBuffer == nullptr ||
				!m_videoBuffer->allocate(	m_videoStream->width(),
													m_videoStream->height())
		)
	{
		delete m_videoBuffer;
		m_videoBuffer = nullptr;

		if ( m_videoStream )
		{
			m_videoStream->close();
			m_videoStream = nullptr;
		}

		return;
	}

	// format the progress bar: USA to blue, GLA to green, China to red
	// and set the background image
	AsciiString campaignName = TheCampaignManager->getCurrentCampaign()->m_name;
	GameWindow *backgroundWin = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "SinglePlayerLoadScreen.wnd:ParentSinglePlayerLoadScreen" ));
	if (campaignName.compareNoCase("USA") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_USA"))
		{
			backgroundWin->winSetEnabledImage( 0, image);
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter2"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	else if (campaignName.compareNoCase("GLA") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_GLA"))
		{
			backgroundWin->winSetEnabledImage( 0, image );
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter3"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	else if (campaignName.compareNoCase("China") == 0)
	{
		if (const Image *image = TheMappedImageCollection->findImageByName("MissionLoad_China"))
		{
			backgroundWin->winSetEnabledImage( 0, image );
		}
		if (const Image *image = TheMappedImageCollection->findImageByName("LoadingBar_ProgressCenter1"))
		{
			m_progressBar->winSetEnabledImage( 6, image );
		}
	}
	// else leave the default background screen


	if(TheGameLODManager && TheGameLODManager->didMemPass())
	{
		// TheSuperHackers @bugfix Originally this movie render loop stopped rendering when the game window was inactive.
		// This either skipped the movie or caused decompression artifacts. Now the video just keeps playing until it done.

		Int progressUpdateCount = m_videoStream->frameCount() / FRAME_FUDGE_ADD;
		Int shiftedPercent = -FRAME_FUDGE_ADD + 1;
		while (m_videoStream->frameIndex() < m_videoStream->frameCount() - 1 )
		{
			if (GameClient::isMovieAbortRequested())
			{
				break;
			}

			if(!m_videoStream->isFrameReady())
			{
				Sleep(1);
				continue;
			}

			m_videoStream->frameDecompress();
			m_videoStream->frameRender(m_videoBuffer);

#if RTS_GENERALS
			moveWindows( m_videoStream->frameIndex());
#endif

			m_videoStream->frameNext();

			// GeneralsX @bugfix fbraz3 20/04/2026 The LoadScreen video loop is a blocking loop
			// that never reaches TheVideoPlayer->update(). Without an explicit update() call here,
			// the OpenAL audio source can underrun (AL_STOPPED) or stay in AL_INITIAL and never
			// restart. update() calls audioStream->play() if the source is not already playing.
			// Issue: https://github.com/fbraz3/GeneralsX/issues/38
			if (TheVideoPlayer)
				TheVideoPlayer->update();

			if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);
			if(m_videoStream->frameIndex() % progressUpdateCount == 0)
			{
				shiftedPercent++;
				if(shiftedPercent >0)
					shiftedPercent = 0;
				Int percent = (shiftedPercent + FRAME_FUDGE_ADD)/1.3;
				UnicodeString per;
				per.format(L"%d%%",percent);
				TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
				GadgetProgressBarSetProgress(m_progressBar, percent);
				GadgetStaticTextSetText(m_percent, per);

			}
			TheWindowManager->update();

			// redraw all views, update the GUI
			TheDisplay->draw();
		}

#if !RTS_GENERALS
		// let the background image show through
		m_videoStream->close();
		m_videoStream = nullptr;
		m_loadScreen->winGetInstanceData()->setVideoBuffer( nullptr );
		TheDisplay->draw();
#endif
	}
	else
	{
#if RTS_GENERALS
		// if we're min speced
		m_videoStream->frameGoto(m_videoStream->frameCount()); // zero based
		while(!m_videoStream->isFrameReady())
			Sleep(1);
		m_videoStream->frameDecompress();
		m_videoStream->frameRender(m_videoBuffer);
		if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

		m_objectiveWin->winHide(FALSE);
		for(i = 0; i < MAX_DISPLAYED_UNITS; ++i)
			m_unitDesc[i]->winHide(FALSE);
		m_location->winHide(FALSE);

		// Audio was choppy so, I chopped it out!
		TheAudio->friend_forcePlayAudioEventRTS(&TheCampaignManager->getCurrentMission()->m_briefingVoice);

		for(Int i = 0; i < MAX_OBJECTIVE_LINES; ++i)
		{
			GadgetStaticTextSetText(m_objectiveLines[i], m_unicodeObjectiveLines[i]);
		}
#else
		// if we're min spec'ed don't play a movie
#endif

		Int delay = mission->m_voiceLength * 1000;
		Int begin = timeGetTime();
		Int currTime = begin;
		Int fudgeFactor = 0;
		while(begin + delay > currTime )
		{
			fudgeFactor = 30 * ((currTime - begin)/ INT_TO_REAL(delay ));
			GadgetProgressBarSetProgress(m_progressBar, fudgeFactor);

			if (GameClient::isMovieAbortRequested())
			{
				break;
			}

			TheWindowManager->update();
			TheDisplay->draw();
			Sleep(100);
			currTime = timeGetTime();
		}


		TheWindowManager->update();
		TheDisplay->draw();

	}
	setFPMode();
	m_percent->winHide(TRUE);
	m_ambientLoopHandle = TheAudio->addAudioEvent(&m_ambientLoop);

}

void SinglePlayerLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void SinglePlayerLoadScreen::update( Int percent )
{
	percent = (percent + FRAME_FUDGE_ADD)/1.3;
	UnicodeString per;
	per.format(L"%d%%",percent);
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);
	GadgetStaticTextSetText(m_percent, per);

	// Do this last!
	LoadScreen::update( percent );
}

void SinglePlayerLoadScreen::setProgressRange( Int min, Int max )
{

}

// ChallengeLoadScreen Class ///////////////////////////////////////////////
//-----------------------------------------------------------------------------
ChallengeLoadScreen::ChallengeLoadScreen()
{
	m_progressBar = nullptr;
	m_videoStream = nullptr;
	m_videoBuffer = nullptr;

	m_bioNameLeft = nullptr;
	m_bioAgeLeft = nullptr;
	m_bioBirthplaceLeft = nullptr;
	m_bioStrategyLeft = nullptr;
	m_bioBigNameEntryLeft = nullptr;
	m_bioNameEntryLeft = nullptr;
	m_bioAgeEntryLeft = nullptr;
	m_bioBirthplaceEntryLeft = nullptr;
	m_bioStrategyEntryLeft = nullptr;
	m_bioBigNameEntryRight = nullptr;
	m_bioNameRight = nullptr;
	m_bioAgeRight = nullptr;
	m_bioBirthplaceRight = nullptr;
	m_bioStrategyRight = nullptr;
	m_bioNameEntryRight = nullptr;
	m_bioAgeEntryRight = nullptr;
	m_bioBirthplaceEntryRight = nullptr;
	m_bioStrategyEntryRight = nullptr;

	m_portraitLeft = nullptr;
	m_portraitRight = nullptr;
	m_portraitMovieLeft = nullptr;
	m_portraitMovieRight = nullptr;

//	m_overlayReticleCrosshairs = nullptr;
//	m_overlayReticleCircleLineOuter = nullptr;
//	m_overlayReticleCircleLineInner = nullptr;
	m_overlayReticleCircleAlphaOuter = nullptr;
	m_overlayReticleCircleAlphaInner = nullptr;
	m_overlayVsBackdrop = nullptr;
	m_overlayVs = nullptr;
	m_wndVideoManager = nullptr;
}

ChallengeLoadScreen::~ChallengeLoadScreen()
{
	delete m_videoBuffer;

	if ( m_videoStream )
	{
		m_videoStream->close();
	}

	delete m_wndVideoManager;

	TheAudio->removeAudioEvent( m_ambientLoopHandle );
}

// accepts the number of chars to advance, the window we're concerned with, the total text for final display, and the current position of the readout
// returns the updated position of the readout
Int updateTeletypeText( Int num_chars, GameWindow* window, UnicodeString full_text, Int current_text_pos )
{
	DEBUG_ASSERTCRASH(window, ("No window for teletype text update"));
	UnicodeString currentText = GadgetStaticTextGetText(window);
	WideChar wChar;
	for (Int i = 0; i < num_chars; i++)
	{
		if (current_text_pos < full_text.getLength())
		{
			wChar = full_text.getCharAt(current_text_pos);
			currentText.concat(wChar);
			current_text_pos++;
		}
	}
	GadgetStaticTextSetText(window, currentText);
	return current_text_pos;
}

void ChallengeLoadScreen::activatePieces( Int frame, const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent )
{
	static Int textPosBigNameRight = 0;
	static Int textPosNameRight = 0;
	static Int textPosAgeRight = 0;
	static Int textPosBirthplaceRight = 0;
	static Int textPosStrategyRight = 0;
	static Int textPosBigNameLeft = 0;
	static Int textPosNameLeft = 0;
	static Int textPosAgeLeft = 0;
	static Int textPosBirthplaceLeft = 0;
	static Int textPosStrategyLeft = 0;

	AudioEventRTS eventLeftGeneral( generalPlayer->getNameSound() );
	AudioEventRTS eventVS("Taunts_GCAnnouncer12");
	AudioEventRTS eventRightGeneral( generalOpponent->getNameSound() );

	switch (frame)
	{
		case FRAME_TITLES_START:
			m_bioNameLeft->winHide(FALSE);
//			m_bioAgeLeft->winHide(FALSE);
			m_bioBirthplaceLeft->winHide(FALSE);
			m_bioStrategyLeft->winHide(FALSE);
			m_bioNameRight->winHide(FALSE);
//			m_bioAgeRight->winHide(FALSE);
			m_bioBirthplaceRight->winHide(FALSE);
			m_bioStrategyRight->winHide(FALSE);

			break;
		case FRAME_TELETYPE_START:
			// reinit the statics for each new load screen
			textPosBigNameRight = 0;
			textPosNameRight = 0;
			textPosAgeRight = 0;
			textPosBirthplaceRight = 0;
			textPosStrategyRight = 0;
			textPosBigNameLeft = 0;
			textPosNameLeft = 0;
			textPosAgeLeft = 0;
			textPosBirthplaceLeft = 0;
			textPosStrategyLeft = 0;

			m_bioBigNameEntryLeft->winHide(FALSE);
			m_bioNameEntryLeft->winHide(FALSE);
//			m_bioAgeEntryLeft->winHide(FALSE);
			m_bioBirthplaceEntryLeft->winHide(FALSE);
			m_bioStrategyEntryLeft->winHide(FALSE);
			GadgetStaticTextSetText( m_bioBigNameEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioNameEntryLeft, UnicodeString::TheEmptyString );
//			GadgetStaticTextSetText( m_bioAgeEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioBirthplaceEntryLeft, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioStrategyEntryLeft, UnicodeString::TheEmptyString );

			m_bioBigNameEntryRight->winHide(FALSE);
			m_bioNameEntryRight->winHide(FALSE);
//			m_bioAgeEntryRight->winHide(FALSE);
			m_bioBirthplaceEntryRight->winHide(FALSE);
			m_bioStrategyEntryRight->winHide(FALSE);
			GadgetStaticTextSetText( m_bioBigNameEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioNameEntryRight, UnicodeString::TheEmptyString );
//			GadgetStaticTextSetText( m_bioAgeEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioBirthplaceEntryRight, UnicodeString::TheEmptyString );
			GadgetStaticTextSetText( m_bioStrategyEntryRight, UnicodeString::TheEmptyString );
			break;
		case FRAME_PORTRAITS_START:

			m_wndVideoManager->playMovie( m_portraitMovieLeft, generalPlayer->getPortraitMovieLeftName(), WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
			m_wndVideoManager->playMovie( m_portraitMovieRight, generalOpponent->getPortraitMovieRightName(), WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
			m_portraitMovieLeft->winHide(FALSE);
			m_portraitMovieRight->winHide(FALSE);

			TheAudio->addAudioEvent( &eventLeftGeneral );

			break;
		case FRAME_OUTER_CIRCLE_LINE_SHOW:
//			m_overlayReticleCircleLineOuter->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_LINE_SHOW:
//			m_overlayReticleCircleLineInner->winHide(FALSE);
			break;
		case FRAME_OUTER_CIRCLE_ALPHA_SHOW:
			m_overlayReticleCircleAlphaOuter->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_ALPHA_SHOW:
			m_overlayReticleCircleAlphaInner->winHide(FALSE);
			break;
		case FRAME_OUTER_CIRCLE_LINE_HIDE:
//			m_overlayReticleCircleLineOuter->winHide(TRUE);
			break;
		case FRAME_INNER_BACKDROP_ALPHA_SHOW:
			m_overlayVsBackdrop->winHide(FALSE);
			break;
		case FRAME_INNER_CIRCLE_LINE_HIDE:
//			m_overlayReticleCircleLineInner->winHide(TRUE);
			break;
		case FRAME_VS_ANIM_START:
			// it's time to start the overlay movie
//					m_overlayVsBackdrop->winSetEnabledImage( 0, TheMappedImageCollection->findImageByFilename("))
			m_overlayVsBackdrop->winHide(FALSE);
			m_overlayVs->winHide(FALSE);
			m_wndVideoManager->playMovie( m_overlayVs, "VSSmall", WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);

			// "Verses"
			TheAudio->addAudioEvent( &eventVS );

			break;
		case FRAME_RIGHT_VOICE:
			TheAudio->addAudioEvent( &eventRightGeneral );

			break;
	}

	// update the teletype readout
	if (frame > FRAME_TELETYPE_START && (frame % TELETYPE_UPDATE_FREQ) == 0)
	{
		textPosNameLeft = updateTeletypeText( 1, m_bioNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()), textPosNameLeft);
		textPosBigNameLeft = updateTeletypeText( 1, m_bioBigNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()), textPosBigNameLeft);
//		textPosAgeLeft = updateTeletypeText( 1, m_bioAgeEntryLeft, TheGameText->fetch(generalPlayer->getBioDOB()), textPosAgeLeft);
		textPosBirthplaceLeft = updateTeletypeText( 1, m_bioBirthplaceEntryLeft, TheGameText->fetch(generalPlayer->getBioRank()), textPosBirthplaceLeft);
		textPosStrategyLeft = updateTeletypeText( 1, m_bioStrategyEntryLeft, TheGameText->fetch(generalPlayer->getBioStrategy()), textPosStrategyLeft);

		textPosNameRight = updateTeletypeText( 1, m_bioNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()), textPosNameRight);
		textPosBigNameRight = updateTeletypeText( 1, m_bioBigNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()), textPosBigNameRight);
//		textPosAgeRight = updateTeletypeText( 1, m_bioAgeEntryRight, TheGameText->fetch(generalOpponent->getBioDOB()), textPosAgeRight);
		textPosBirthplaceRight = updateTeletypeText( 1, m_bioBirthplaceEntryRight, TheGameText->fetch(generalOpponent->getBioRank()), textPosBirthplaceRight);
		textPosStrategyRight = updateTeletypeText( 1, m_bioStrategyEntryRight, TheGameText->fetch(generalOpponent->getBioStrategy()), textPosStrategyRight);
	}
}

void ChallengeLoadScreen::activatePiecesMinSpec(const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent)
{
	m_bioNameLeft->winHide(FALSE);
//	m_bioAgeLeft->winHide(FALSE);
	m_bioBirthplaceLeft->winHide(FALSE);
	m_bioStrategyLeft->winHide(FALSE);
	m_bioNameRight->winHide(FALSE);
//	m_bioAgeRight->winHide(FALSE);
	m_bioBirthplaceRight->winHide(FALSE);
	m_bioStrategyRight->winHide(FALSE);
	m_bioBigNameEntryLeft->winHide(FALSE);
	m_bioNameEntryLeft->winHide(FALSE);
//	m_bioAgeEntryLeft->winHide(FALSE);
	m_bioBirthplaceEntryLeft->winHide(FALSE);
	m_bioStrategyEntryLeft->winHide(FALSE);
	GadgetStaticTextSetText( m_bioBigNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()) );
	GadgetStaticTextSetText( m_bioNameEntryLeft, TheGameText->fetch(generalPlayer->getBioName()) );
//	GadgetStaticTextSetText( m_bioAgeEntryLeft, TheGameText->fetch(generalPlayer->getBioDOB()) );
	GadgetStaticTextSetText( m_bioBirthplaceEntryLeft, TheGameText->fetch(generalPlayer->getBioRank()) );
	GadgetStaticTextSetText( m_bioStrategyEntryLeft, TheGameText->fetch(generalPlayer->getBioStrategy()) );
	m_bioBigNameEntryRight->winHide(FALSE);
	m_bioNameEntryRight->winHide(FALSE);
//	m_bioAgeEntryRight->winHide(FALSE);
	m_bioBirthplaceEntryRight->winHide(FALSE);
	m_bioStrategyEntryRight->winHide(FALSE);
	GadgetStaticTextSetText( m_bioBigNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()) );
	GadgetStaticTextSetText( m_bioNameEntryRight, TheGameText->fetch(generalOpponent->getBioName()) );
//	GadgetStaticTextSetText( m_bioAgeEntryRight, TheGameText->fetch(generalOpponent->getBioDOB()) );
	GadgetStaticTextSetText( m_bioBirthplaceEntryRight, TheGameText->fetch(generalOpponent->getBioRank()) );
	GadgetStaticTextSetText( m_bioStrategyEntryRight, TheGameText->fetch(generalOpponent->getBioStrategy()) );
	m_portraitLeft->winSetEnabledImage(0, generalPlayer->getBioPortraitLarge() );
	m_portraitRight->winSetEnabledImage(0, generalOpponent->getBioPortraitLarge() );
	m_portraitLeft->winHide(FALSE);
	m_portraitRight->winHide(FALSE);
	m_overlayReticleCircleAlphaOuter->winHide(FALSE);
	m_overlayReticleCircleAlphaInner->winHide(FALSE);
	m_overlayVsBackdrop->winHide(FALSE);
	m_overlayVsBackdrop->winHide(FALSE);
	m_overlayVs->winHide(FALSE);
	m_wndVideoManager->playMovie( m_overlayVs, "VSSmall", WINDOW_PLAY_MOVIE_SHOW_LAST_FRAME);
}


void ChallengeLoadScreen::init( GameInfo *game )
{
	const Campaign *campaign = TheCampaignManager->getCurrentCampaign();
	const Mission *mission = TheCampaignManager->getCurrentMission();

	// the player general is tied to the campaign
	// GeneralsX: An unresolved campaign, mission or persona (bad/modded Campaign.ini) was dereferenced. Keep building the window and progress bar
	// (update() drives it), then leave before the persona, voice-delay and taunt pieces, like the failed-video path below does. Idea from Generals Online 3e6a7186c, reimplemented.
	const GeneralPersona* generalPlayer = campaign ? TheChallengeGenerals->getPlayerGeneralByCampaignName( campaign->m_name ) : nullptr;

	// the opponent general is tied to the mission
	DEBUG_ASSERTCRASH(mission && mission->m_generalName.isNotEmpty(), ("No GeneralName associated with this mission, check Campaign.ini"));
	const GeneralPersona* generalOpponent = mission ? TheChallengeGenerals->getGeneralByGeneralName( mission->m_generalName ) : nullptr;

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/ChallengeLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the single player loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );
	// GeneralsX A2: in the bios' style; here, before the early returns below (no mission, no movie, no video buffer), which
	// still load
	m_gxWarmText = gxWarmTextTopLeft( m_loadScreen );

	m_ambientLoop.setEventName("LoadScreenAmbient");

	if (!mission || !generalPlayer || !generalOpponent)
	{
		return;
	}

	// create the new background video stream
	m_videoStream = TheVideoPlayer->open( mission->m_movieLabel );
	if (m_videoStream == nullptr)
	{
		return;
	}

	// Create the new buffer
	m_videoBuffer = TheDisplay->createVideoBuffer();
	if (m_videoBuffer == nullptr || !m_videoBuffer->allocate(	m_videoStream->width(), m_videoStream->height() ))
	{
		delete m_videoBuffer;
		m_videoBuffer = nullptr;

		if ( m_videoStream )
		{
			m_videoStream->close();
			m_videoStream = nullptr;
		}

		return;
	}

	// init overlays
	NameKeyType namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitLeft");
	m_portraitLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitRight");
	m_portraitRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitMovieLeft");
	m_portraitMovieLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:PortraitMovieRight");
	m_portraitMovieRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:ReticleCrosshairs");
//	m_overlayReticleCrosshairs = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
/*
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:OuterCircleLine");
	m_overlayReticleCircleLineOuter = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:InnerCircleLine");
	m_overlayReticleCircleLineInner = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
*/
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:CircleAlphaOuter");
	m_overlayReticleCircleAlphaOuter = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:CircleAlphaInner");
	m_overlayReticleCircleAlphaInner = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:VersusBackdrop");
	m_overlayVsBackdrop = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:OverlayVs");
	m_overlayVs = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );

	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameLeft");
	m_bioNameLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBLeft");
//	m_bioAgeLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceLeft");
	m_bioBirthplaceLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyLeft");
	m_bioStrategyLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BigNameEntryLeft");
	m_bioBigNameEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameEntryLeft");
	m_bioNameEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBEntryLeft");
//	m_bioAgeEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceEntryLeft");
	m_bioBirthplaceEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyEntryLeft");
	m_bioStrategyEntryLeft = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameRight");
	m_bioNameRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBRight");
//	m_bioAgeRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceRight");
	m_bioBirthplaceRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyRight");
	m_bioStrategyRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BigNameEntryRight");
	m_bioBigNameEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioNameEntryRight");
	m_bioNameEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
//	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioDOBEntryRight");
//	m_bioAgeEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioBirthplaceEntryRight");
	m_bioBirthplaceEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );
	namekey = TheNameKeyGenerator->nameToKey( "ChallengeLoadScreen.wnd:BioStrategyEntryRight");
	m_bioStrategyEntryRight = TheWindowManager->winGetWindowFromId( m_loadScreen, namekey );


	// make sure reticle stuff starts out hidden
//	m_overlayReticleCircleLineOuter->winHide(TRUE);
//	m_overlayReticleCircleLineInner->winHide(TRUE);
	m_overlayReticleCircleAlphaOuter->winHide(TRUE);
	m_overlayReticleCircleAlphaInner->winHide(TRUE);
	m_overlayVsBackdrop->winHide(TRUE);
	m_overlayVs->winHide(TRUE);

	m_wndVideoManager = NEW WindowVideoManager;
	m_wndVideoManager->init();

	if(TheGameLODManager && TheGameLODManager->didMemPass())
	{
		// TheSuperHackers @bugfix Originally this movie render loop stopped rendering when the game window was inactive.
		// This either skipped the movie or caused decompression artifacts. Now the video just keeps playing until it done.

		Int progressUpdateCount = m_videoStream->frameCount() / FRAME_FUDGE_ADD;
		Int shiftedPercent = -FRAME_FUDGE_ADD + 1;
		while (m_videoStream->frameIndex() < m_videoStream->frameCount() - 1 )
		{
			if (GameClient::isMovieAbortRequested())
			{
				break;
			}

			if(!m_videoStream->isFrameReady())
			{
				Sleep(1);
				continue;
			}

			m_videoStream->frameDecompress();
			m_videoStream->frameRender(m_videoBuffer);
			m_videoStream->frameNext();

			// GeneralsX @bugfix fbraz3 20/04/2026 Same blocking loop fix as SinglePlayerLoadScreen.
			// TheVideoPlayer->update() must be called explicitly here since this loop never returns
			// to the main game loop. Without it, OpenAL audio source stays AL_INITIAL/AL_STOPPED.
			// Issue: https://github.com/fbraz3/GeneralsX/issues/38
			if (TheVideoPlayer)
				TheVideoPlayer->update();

			if(m_videoBuffer)
				m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

			Int frame = m_videoStream->frameIndex();
			if(frame % progressUpdateCount == 0)
			{
				shiftedPercent++;
				if(shiftedPercent >0)
					shiftedPercent = 0;
				Int percent = (shiftedPercent + FRAME_FUDGE_ADD)/1.3;
				UnicodeString per;
				per.format(L"%d%%",percent);
				TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
				GadgetProgressBarSetProgress(m_progressBar, percent);
			}
			TheWindowManager->update();

			activatePieces(frame, generalPlayer, generalOpponent);
			m_wndVideoManager->update();

			// redraw all views, update the GUI
			TheDisplay->draw();

			TheAudio->update();
		}
	}
	else
	{
		// if we're min speced
		m_videoStream->frameGoto(m_videoStream->frameCount()); // zero based
		while(!m_videoStream->isFrameReady())
		{
			if (GameClient::isMovieAbortRequested())
			{
				return;
			}
			Sleep(1);
		}
		m_videoStream->frameDecompress();
		m_videoStream->frameRender(m_videoBuffer);
		if(m_videoBuffer)
			m_loadScreen->winGetInstanceData()->setVideoBuffer(m_videoBuffer);

		activatePiecesMinSpec(generalPlayer, generalOpponent);

		Int delay = mission->m_voiceLength * 1000;
		Int begin = timeGetTime();
		Int currTime = begin;
		Int fudgeFactor = 0;
		while(begin + delay > currTime )
		{
			fudgeFactor = 30 * ((currTime - begin)/ INT_TO_REAL(delay ));
			GadgetProgressBarSetProgress(m_progressBar, fudgeFactor);

			if (GameClient::isMovieAbortRequested())
			{
				break;
			}

			TheWindowManager->update();
			TheDisplay->draw();
			Sleep(100);
			currTime = timeGetTime();
		}

		m_wndVideoManager->update();
		TheWindowManager->update();
		TheDisplay->draw();
	}
	setFPMode();


	AudioEventRTS event( generalOpponent->getRandomTauntSound() );
	TheAudio->addAudioEvent( &event );

	m_ambientLoopHandle = TheAudio->addAudioEvent(&m_ambientLoop);
	TheAudio->update();
}

void ChallengeLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void ChallengeLoadScreen::update( Int percent )
{
	percent = (percent + FRAME_FUDGE_ADD)/1.3;
	UnicodeString per;
	per.format(L"%d%%",percent);
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);

	// Do this last!
	LoadScreen::update( percent );
}

void ChallengeLoadScreen::setProgressRange( Int min, Int max )
{

}

// ShellGameLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
ShellGameLoadScreen::ShellGameLoadScreen()
{
	m_progressBar = nullptr;
}

ShellGameLoadScreen::~ShellGameLoadScreen()
{
}

void ShellGameLoadScreen::init( GameInfo *game )
{
	static BOOL firstLoad = TRUE;


	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/ShellGameLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the ShellGame loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	// Store the pointer to the progress bar on the loadscreen
	m_progressBar = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ShellGameLoadScreen.wnd:ProgressLoad" ));
	DEBUG_ASSERTCRASH(m_progressBar, ("Can't initialize the progressbar for the single player loadscreen"));
	GadgetProgressBarSetProgress(m_progressBar, 0 );
	m_progressBar->winHide(TRUE);

	// GeneralsX A2: the startup screen, so with the shell map on this is where a first launch holds (2.8 s on the Mini,
	// MB1 01/10) - and on macOS every launch (rc-check-1002b, 02/10).
	m_gxWarmText = gxWarmTextTopLeft( m_loadScreen );

	if(m_loadScreen && firstLoad && TheGameLODManager && TheGameLODManager->didMemPass())
	{
		m_loadScreen->winSetEnabledImage(0, TheMappedImageCollection->findImageByName("TitleScreen"));
		TheWritableGlobalData->m_breakTheMovie = FALSE;

		GameWindow *win = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "ShellGameLoadScreen.wnd:StaticTextLegal" ));
		if(win)
			win->winHide(FALSE);
		firstLoad = FALSE;
	}
	m_progressBar->winHide(FALSE);
}

void ShellGameLoadScreen::reset()
{
 setLoadScreen(nullptr);
 m_progressBar = nullptr;
}

void ShellGameLoadScreen::update( Int percent )
{
	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);
	GadgetProgressBarSetProgress(m_progressBar, percent);

	// Do this last!
	LoadScreen::update( percent );
}

// MultiPlayerLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
MultiPlayerLoadScreen::MultiPlayerLoadScreen()
{
	m_mapPreview = nullptr;
	m_portraitLocalGeneral = nullptr;
	m_featuresLocalGeneral = nullptr;
	m_nameLocalGeneral = nullptr;

	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_buttonMapStartPosition[i] = nullptr;
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
		m_playerLookup[i] = -1;
	}
}

MultiPlayerLoadScreen::~MultiPlayerLoadScreen()
{
	if(m_mapPreview)
	{
		m_mapPreview->winSetUserData(nullptr);
	}

	TheAudio->removeAudioEvent( AHSV_StopTheMusicFade );
//	TheAudio->stopAudio( AudioAffect_Music );
}

// GeneralsX: init() ran before the start positions were picked. m_mapPreview is
// null until init() has built the window, which is also the headless case.
void MultiPlayerLoadScreen::refreshStartSpots( GameInfo *game )
{
	if (game && m_mapPreview)
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
}

// GeneralsX: A multiplayer load screen with no game is a save from a LAN or
// online match being loaded outside one: GameStateMap::xfer restores the saved mode and nothing supplies
// TheGameInfo. Both inits below dereferenced it - a SIGSEGV. Throw instead. On a save load this runs
// inside GameState::loadGame's catch, which turns it into SC_INVALID_DATA and the normal error; any
// other start with no game was already a crash and now reaches the engine's catch instead of a signal.
// Only GameLogic creates these, always as its m_loadScreen, so the caller IS that screen: drop it, or the
// next load would skip building a screen and reuse this half-built one (whose update() dereferences
// TheGameInfo). Nothing touches the deleted object after that line. GameLogic also hid the cursor just
// before init(); give it back, as this load never reaches the code that would.
static void refuseMultiplayerLoadScreenWithoutGame( const char *which )
{
	fprintf( stderr, "[SAVELOAD] %s has no game (a LAN/online save loaded outside a match?); aborting the load\n", which );
	if( TheGameLogic )
		TheGameLogic->deleteLoadScreen();
	if( TheMouse )
		TheMouse->setVisibility( TRUE );
	throw SC_INVALID_DATA;
}

// GeneralsX: A game with no local player slot: both inits below dereferenced
// getSlot(getLocalSlotNum()) = getSlot(-1) = null. Measured with a skirmish save from a build with a
// different MAX_SLOTS (retail and older GeneralsX wrote 8): SkirmishGameInfo::xfer misreads the slots and
// none is local. The save gate refuses that save before loading (SaveLoadFeedback.cpp); this is the same
// throw as above for any route the gate does not cover, noted as the crash it would have been.
static void refuseMultiplayerLoadScreenWithoutLocalSlot( GameInfo *game, const char *which )
{
	const Int local = game->getLocalSlotNum();	// checked first: getSlot(-1) is a DEBUG_ASSERTCRASH before it is null
	if( local >= 0 && local < MAX_SLOTS && game->getSlot( local ) != nullptr )
		return;
	fprintf( stderr, "[SAVELOAD] %s has no local player slot; aborting the load\n", which );
	notePreventedLoadCrash( "the game it restores has no local player slot" );
	if( TheGameLogic )
		TheGameLogic->deleteLoadScreen();
	if( TheMouse )
		TheMouse->setVisibility( TRUE );
	throw SC_INVALID_DATA;
}

void MultiPlayerLoadScreen::init( GameInfo *game )
{
	if( game == nullptr )
		refuseMultiplayerLoadScreenWithoutGame( "MultiPlayerLoadScreen" );
	refuseMultiplayerLoadScreenWithoutLocalSlot( game, "MultiPlayerLoadScreen" );

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/MultiplayerLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the Multiplayer loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();
	m_mapPreview = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:WinMapPreview"));
	GameSlot *lSlot = game->getSlot(game->getLocalSlotNum());
	const PlayerTemplate* pt;
	if (lSlot->getPlayerTemplate() >= 0)
		pt = ThePlayerTemplateStore->getNthPlayerTemplate(lSlot->getPlayerTemplate());
	else
		pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );

#if RTS_GENERALS
	const Image *loadScreenImage = TheMappedImageCollection->findImageByName(pt->getLoadScreen());
	if(loadScreenImage)
		m_loadScreen->winSetEnabledImage(0, loadScreenImage);
#else
	// add portrait, features, and name for the local player's general
	const GeneralPersona *localGeneral = TheChallengeGenerals->getGeneralByTemplateName( pt->getName() );
	const Image *portrait = nullptr;
	UnicodeString localName;
	if (localGeneral)
	{
		portrait = localGeneral->getBioPortraitLarge();
		localName = TheGameText->fetch( localGeneral->getBioName() );
	}
	else
	{
		// the main original factions don't have associated generals
		AsciiString imageName;
		if (pt->getName() == "FactionAmerica")
			portrait = TheMappedImageCollection->findImageByName("SAFactionLogoLg_US");
		else if (pt->getName() == "FactionGLA")
			portrait = TheMappedImageCollection->findImageByName("SUFactionLogoLg_GLA");
		else if (pt->getName() == "FactionChina")
			portrait = TheMappedImageCollection->findImageByName("SNFactionLogoLg_China");
		else
			DEBUG_CRASH(("Unexpected player template"));

		localName = pt->getDisplayName();
	}
	m_portraitLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralPortrait"));
	m_portraitLocalGeneral->winSetEnabledImage( 0, portrait);
	m_featuresLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralFeatures"));
	AsciiString features = pt->getGeneralFeatures();
	GadgetStaticTextSetText( m_featuresLocalGeneral, TheGameText->fetch( features.isEmpty() ? "GUI:PlayerObserver" : pt->getGeneralFeatures() ) );
	m_gxWarmText = gxWarmTextTopLeft( m_loadScreen );	// GeneralsX A2
	m_nameLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MultiplayerLoadScreen.wnd:LocalGeneralName"));
	GadgetStaticTextSetText( m_nameLocalGeneral, localName );
#endif

	AsciiString musicName = pt->getLoadScreenMusic();
	if ( ! musicName.isEmpty() )
	{
		TheAudio->removeAudioEvent( AHSV_StopTheMusicFade );
		AudioEventRTS event( musicName );
		event.setShouldFade( TRUE );

		TheAudio->addAudioEvent( &event );
		TheAudio->update();//Since GameEngine::update() is suspended until after I am gone...

	}

	//DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network!!!!"));
	//DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));

	GameWindow *teamWin[MAX_SLOTS];
	Int i = 0;
	for (; i < MAX_SLOTS; ++i)
	{
		teamWin[i] = nullptr;
	}

	// GeneralsX: BUILD THE ROWS THE .WND DOES NOT AUTHOR.
	//
	// MultiplayerLoadScreen.wnd authors ProgressLoad0..7, StaticTextPlayer0..7, StaticTextSide0..7,
	// StaticTextTeam0..7 and ButtonMapStartPosition0..7 - verified by extracting it from
	// WindowZH.big and listing the indices on each column. MAX_SLOTS is 12, so four rows never
	// existed and the load screen showed eight of twelve players.
	//
	// The guard further down - which skips an occupied slot whose row is null - STAYS. It is what
	// stopped this crashing, and it stays correct if a builder is never registered or a column is
	// dropped from a future .wnd. It is a safety net, not the mechanism; a guard that hides a
	// missing row looks exactly like a fix, which is precisely how these rows stayed invisible
	// after the crash was fixed.
	//
	// LoadScreenEnsureRows is a null check when no builder is registered, so the Generals twin -
	// which has no CodeGui and registers nothing - behaves exactly as it does today.
	// GeneralsX: Created, NOT fitted (TRUE): LoadScreenSplitRows below places them once the
	// number of filled rows is known - the authored eight as EA laid them out, more as two groups that stay inside the
	// frame. Fitting all MAX_SLOTS into the band here ran 34 rows off the bottom of the screen.
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:ProgressLoad%d",           MAX_SLOTS, TRUE );
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:StaticTextPlayer%d",       MAX_SLOTS, TRUE );
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:StaticTextSide%d",         MAX_SLOTS, TRUE );
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:StaticTextTeam%d",         MAX_SLOTS, TRUE );
	// GeneralsX: The start markers too - this list stopped at the player
	// columns, so a 12-player map loaded with markers on eight of its twelve bases. Placed by
	// positionStartSpots below, so they are created, not fitted.
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:ButtonMapStartPosition%d", MAX_SLOTS, TRUE );

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		AsciiString winName;
		winName.format( "MultiplayerLoadScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the Multiplayer loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load MapStart Positions
		winName.format( "MultiplayerLoadScreen.wnd:ButtonMapStartPosition%d",i);
		m_buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_buttonMapStartPosition[i], ("Can't initialize the MapStart Positions for the MultiplayerLoadScreen loadscreen"));


		// Load the Player's name
		winName.format( "MultiplayerLoadScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the Multiplayer loadscreen"));

		// Load the Player's Side
		winName.format( "MultiplayerLoadScreen.wnd:StaticTextSide%d",i);
		m_playerSide[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerSide[i], ("Can't initialize the Sides for the Multiplayer loadscreen"));

		winName.format( "MultiplayerLoadScreen.wnd:StaticTextTeam%d",i);
		teamWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));

		// get the slot man!
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		// GeneralsX: THE OCCUPIED BRANCH HAD THE SAME NULL ROW AS THE TAIL.
		//
		// The guard below the loop already covers rows netSlot..MAX_SLOTS-1. It did not cover THIS
		// branch, which indexes the widget arrays by netSlot - the count of OCCUPIED slots - and
		// dereferences m_progressBars/m_playerNames/m_playerSide with no check at all.
		// MultiplayerLoadScreen.wnd authors ProgressLoad0..7 and StaticTextPlayer0..7 (verified by
		// extracting it from WindowZH.big), so with nine or more occupied slots netSlot reaches 8,
		// winGetWindowFromId returns null, and winSetEnabledImage writes through it.
		//
		// isOccupied() is human OR AI, so one human plus nine AI is enough - which is exactly a
		// twelve-slot lobby filled with AI, the thing MAX_SLOTS 8 -> 12 was raised to allow.
		// Reproduced on both platforms at the same three frames, writing to address 0xb8:
		//   winSetEnabledImage <- MultiPlayerLoadScreen::init <- GameLogic::tryStartNewGame
		//
		// SKIP the slot rather than null-check fifteen dereferences, and do NOT advance netSlot:
		// there is no row here to consume, so the next occupied slot must not be pushed further
		// out of range. m_playerLookup[i] therefore stays -1 (set at construction), and update()
		// already returns early on -1 - so a player with no row simply shows no progress bar
		// instead of taking the process down.
		//
		// Cloning the missing rows the way the lobby does would be better, and is deliberately NOT
		// done here: CodeGuiRowsEnsure lives in GeneralsMD and this file is Core, shared with the
		// Generals build, which must not grow #if gates for it.
		if (m_progressBars[netSlot] == nullptr || m_playerNames[netSlot] == nullptr ||
				m_playerSide[netSlot] == nullptr)
			continue;

		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
#if RTS_GENERALS
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot],houseColor );
#else
		// format the progress bar to house colors
		AsciiString imageName;
		imageName.format("LoadingBar_ProgressCenter%d", slot->getApparentColor());
		const Image *houseImage = TheMappedImageCollection->findImageByName(imageName);
		if (! houseImage)
			houseImage = TheMappedImageCollection->findImageByName("LoadingBar_Progress");
		m_progressBars[netSlot]->winSetEnabledImage( 6, houseImage );
#endif

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		GadgetStaticTextSetText(m_playerSide[netSlot], slot->getApparentPlayerTemplateDisplayName() );
		m_playerSide[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerSide[netSlot]->winGetEnabledTextBorderColor());

		if (slot->isAI() && m_progressBars[netSlot])
			m_progressBars[netSlot]->winHide(TRUE);

		if (teamWin[netSlot])
		{
			AsciiString teamStr;
			teamStr.format("Team:%d", slot->getTeamNumber() + 1);
			GadgetStaticTextSetText(teamWin[netSlot], TheGameText->fetch(teamStr));
			teamWin[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerNames[netSlot]->winGetEnabledTextBorderColor());
		}

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	// GeneralsX: SLOTS 8+ HAVE NO .WND CONTROL, AND THIS CRASHED.
	//
	// MultiplayerLoadScreen.wnd and GameSpyLoadScreen.wnd author rows 0..7 - read out of
	// WindowZH.big. MAX_SLOTS became 12, so winGetWindowFromId returns null for 8..11
	// (GameWindowManager.cpp:719) and this tail loop dereferenced it unguarded. It runs from
	// netSlot, the number of OCCUPIED slots, so the fewer players there are the more of it
	// executes: hosting alone means netSlot == 1 and the loop reaches i == 8 every single time.
	// That is "crashes when loading into a match in the lobby, alone", exactly as reported.
	// Note the asymmetry this replaces - teamWin[netSlot] was already null-checked a dozen
	// lines above, in the occupied branch. Only the tail was left bare.
	//
	// WHY NO TEST CAUGHT IT: -headless installs GameWindowManagerDummy (GameClient.cpp:367),
	// whose winGetWindowFromId override returns m_windowList rather than null "because callers
	// expect this to be non-null" (GameWindowManager.cpp:4214-4223). Every headless 12-slot
	// soak was structurally incapable of reaching this fault.
	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		if (m_progressBars[i]) m_progressBars[i]->winHide(TRUE);
		if (m_playerNames[i])  m_playerNames[i]->winHide(TRUE);
		if (m_playerSide[i])   m_playerSide[i]->winHide(TRUE);
		if (teamWin[i])        teamWin[i]->winHide(TRUE);
	}

	// GeneralsX: Place the filled rows: unchanged up to the authored eight, two side-by-side
	// groups past that, so 32 players stay inside the frame. See LoadScreenSplitRows.
	{
		static const char * const mpColumns[] =
		{
			"MultiplayerLoadScreen.wnd:StaticTextPlayer%d",
			"MultiplayerLoadScreen.wnd:StaticTextSide%d",
			"MultiplayerLoadScreen.wnd:StaticTextTeam%d",
			"MultiplayerLoadScreen.wnd:ProgressLoad%d",
		};
		static const char * const mpTitles[] =
		{
			"MultiplayerLoadScreen.wnd:StaticTextPlayerTitle",
			"MultiplayerLoadScreen.wnd:StaticTextSideTitle",
			"MultiplayerLoadScreen.wnd:StaticTextTeamTitle",
			"MultiplayerLoadScreen.wnd:StaticTextProgressTitle",
		};
		LoadScreenSplitRows( m_loadScreen, mpColumns, (Int)( sizeof( mpColumns ) / sizeof( mpColumns[ 0 ] ) ),
												 mpTitles, (Int)( sizeof( mpTitles ) / sizeof( mpTitles[ 0 ] ) ), netSlot );
	}

	if(m_mapPreview)
	{
		const MapMetaData *mmd = TheMapCache->findMap(game->getMap());
		Image *image = getMapPreviewImage(game->getMap());
		m_mapPreview->winSetUserData((void *)mmd);

		positionStartSpots( game, m_buttonMapStartPosition, m_mapPreview);
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
		//positionAdditionalImages((MapMetaData *)mmd, m_mapPreview, TRUE);
		if(image)
		{
			m_mapPreview->winSetStatus(WIN_STATUS_IMAGE);
			m_mapPreview->winSetEnabledImage(0, image);
		}
		else
		{
			m_mapPreview->winClearStatus(WIN_STATUS_IMAGE);
		}
	}


	TheGameLogic->initTimeOutValues();
}

void MultiPlayerLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
	}
}

// GeneralsX: GX_TEST_LOAD_STALL_MS=<ms> blocks ONCE, for <ms>, at the first load step
// at or past GX_TEST_LOAD_STALL_AT percent (default 50) - a synchronous load step that long, which is what a
// slow machine does inside startNewGame. Nothing services a socket until it returns (this function is the
// only pump the load has), so it reproduces a relay membership going silent mid-load on any machine.
// Unset: one cached getenv, never a stall. GameClient/, so outside the simulation digest.
static void gxTestLoadStall( Int percent )
{
	static Int s_ms = -1;
	static Int s_at = 50;
	if (s_ms < 0)
	{
		const char *ms = getenv("GX_TEST_LOAD_STALL_MS");
		const char *at = getenv("GX_TEST_LOAD_STALL_AT");
		s_ms = ms != nullptr ? atoi(ms) : 0;
		if (at != nullptr)
			s_at = atoi(at);
	}
	if (s_ms <= 0 || percent < s_at || percent > 100)
		return;
	fprintf(stderr, "[GXLOADSTALL] stalling %d ms at load step %d%%\n", s_ms, percent);
	fflush(stderr);
	Sleep(s_ms);
	fprintf(stderr, "[GXLOADSTALL] resumed at load step %d%%\n", percent);
	fflush(stderr);
	s_ms = 0;
}

void MultiPlayerLoadScreen::update( Int percent )
{
	gxTestLoadStall(percent);

	// GeneralsX: Keep delivering the host's Game Start while it loads and
	// waits in startNewGame's progress barrier. The start's 5-copy burst fires only from
	// LANAPI::update, which the engine pumps once per main-loop iteration - and this whole load,
	// barrier (PROGRESS_COMPLETE_TIMEOUT = 60 s) and fade is ONE iteration. A peer that lost both
	// immediate copies was therefore reached only after the barrier timed out, ~62 s later, into a
	// match that had started without it. Every barrier pass comes through here (updateLoadProgress
	// (101)), on both engines and headless.
	//
	// resendPendingGameStart is the one doFileTransfer already uses for the same blocked-host shape:
	// host only (a no-op on joiners), the stored message verbatim so the nonce makes it inert at any
	// peer that already ran this start, and self-throttled to one datagram per 2 s. It cannot outlive
	// the start: this screen is deleted as the barrier releases, and nothing calls it after.
	if (TheLAN != nullptr && TheGameLogic->isInLanGame())
		TheLAN->resendPendingGameStart();

	if (TheNetwork)
	{
		if(percent <= 100)
			TheNetwork->updateLoadProgress( percent );
		TheNetwork->liteupdate();
	}
	else
	{
		if (percent <= 100)
			TheGameLogic->processProgress( TheGameInfo->getLocalSlotNum(), percent );
	}

	//GadgetProgressBarSetProgress(m_progressBars[TheNetwork->getLocalPlayerID()], percent );

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

	// Do this last!
	LoadScreen::update( percent );
}

void MultiPlayerLoadScreen::processProgress(Int playerId, Int percentage)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}
	//DEBUG_LOG(("Percentage %d was passed in for Player %d (in loadscreen position %d)", percentage, playerId, m_playerLookup[playerId]));
	if(m_progressBars[m_playerLookup[playerId]])
		GadgetProgressBarSetProgress(m_progressBars[m_playerLookup[playerId]], percentage );
}

// GameSpyLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
GameSpyLoadScreen::GameSpyLoadScreen()
{

	m_mapPreview = nullptr;
	m_portraitLocalGeneral = nullptr;
	m_featuresLocalGeneral = nullptr;
	m_nameLocalGeneral = nullptr;

	for(Int i = 0; i < MAX_SLOTS; ++i)
	{

		m_buttonMapStartPosition[i] = nullptr;
		m_playerRank[i] = nullptr;

		m_playerOfficerMedal[i] = nullptr;
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
		m_playerLookup[i] = -1;
		m_playerFavoriteFactions[i]= nullptr;
		m_playerTotalDisconnects[i]= nullptr;
		m_playerWin[i]= nullptr;
		m_playerWinLosses[i]= nullptr;
	}
}

GameSpyLoadScreen::~GameSpyLoadScreen()
{
	if(m_mapPreview)
	{
		m_mapPreview->winSetUserData(nullptr);
	}
}

extern Int GetAdditionalDisconnectsFromUserFile(Int playerID);

// GeneralsX: init() ran before the start positions were picked. m_mapPreview is
// null until init() has built the window, which is also the headless case.
void GameSpyLoadScreen::refreshStartSpots( GameInfo *game )
{
	if (game && m_mapPreview)
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
}

void GameSpyLoadScreen::init( GameInfo *game )
{
	if( game == nullptr )
		refuseMultiplayerLoadScreenWithoutGame( "GameSpyLoadScreen" );
	refuseMultiplayerLoadScreenWithoutLocalSlot( game, "GameSpyLoadScreen" );

	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/GameSpyLoadScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the Multiplayer loadscreen"));
	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();
	m_mapPreview = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:WinMapPreview"));
	DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network!!!!"));
	DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));
GameSlot *lSlot = game->getSlot(game->getLocalSlotNum());
	const PlayerTemplate* pt;
	if (lSlot->getPlayerTemplate() >= 0)
		pt = ThePlayerTemplateStore->getNthPlayerTemplate(lSlot->getPlayerTemplate());
	else
		pt = ThePlayerTemplateStore->findPlayerTemplate( TheNameKeyGenerator->nameToKey("FactionObserver") );

#if RTS_GENERALS
	const Image *loadScreenImage = TheMappedImageCollection->findImageByName(pt->getLoadScreen());
	if(loadScreenImage)
		m_loadScreen->winSetEnabledImage(0, loadScreenImage);
#else
	// add portrait, features, and name for the local player's general
	const GeneralPersona *localGeneral = TheChallengeGenerals->getGeneralByTemplateName( pt->getName() );
	const Image *portrait = nullptr;
	UnicodeString localName;
	if (localGeneral)
	{
		portrait = localGeneral->getBioPortraitLarge();
		localName = TheGameText->fetch( localGeneral->getBioName() );
	}
	else
	{
		// the main original factions don't have associated generals
		AsciiString imageName;
		if (pt->getName() == "FactionAmerica")
			portrait = TheMappedImageCollection->findImageByName("SAFactionLogo144_US");
		else if (pt->getName() == "FactionGLA")
			portrait = TheMappedImageCollection->findImageByName("SUFactionLogo144_GLA");
		else if (pt->getName() == "FactionChina")
			portrait = TheMappedImageCollection->findImageByName("SNFactionLogo144_China");
		else
			DEBUG_CRASH(("Unexpected player template"));

		localName = pt->getDisplayName();
	}
	m_portraitLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralPortrait"));
	m_portraitLocalGeneral->winSetEnabledImage( 0, portrait);
	m_featuresLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralFeatures"));
	AsciiString features = pt->getGeneralFeatures();
	GadgetStaticTextSetText( m_featuresLocalGeneral, TheGameText->fetch( features.isEmpty() ? "GUI:PlayerObserver" : pt->getGeneralFeatures() ) );
	m_gxWarmText = gxWarmTextTopLeft( m_loadScreen );	// GeneralsX A2
	m_nameLocalGeneral = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "GameSpyLoadScreen.wnd:LocalGeneralName"));
	GadgetStaticTextSetText( m_nameLocalGeneral, localName );
#endif

	GameWindow *teamWin[MAX_SLOTS];
	Int i = 0;
	for (; i < MAX_SLOTS; ++i)
	{
		teamWin[i] = nullptr;
	}

	// GeneralsX: Same as MultiPlayerLoadScreen::init above - see the note
	// there. GameSpyLoadScreen.wnd authors the same eight rows across more columns, so the online
	// load screen has the identical eight-of-twelve defect.
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:ProgressLoad%d",             MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:StaticTextPlayer%d",         MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:StaticTextSide%d",           MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:StaticTextWinLoss%d",        MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:StaticTextTotalDisconnects%d", MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:WinRank%d",                  MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:WinOfficer%d",               MAX_SLOTS );
	// The team column is authored under the MULTIPLAYER prefix even on this screen - the binding
	// loop below reads "MultiplayerLoadScreen.wnd:StaticTextTeam%d" here, which is not a typo.
	LoadScreenEnsureRows( m_loadScreen, "MultiplayerLoadScreen.wnd:StaticTextTeam%d",       MAX_SLOTS );
	// GeneralsX: Start markers, as on MultiPlayerLoadScreen::init.
	LoadScreenEnsureRows( m_loadScreen, "GameSpyLoadScreen.wnd:ButtonMapStartPosition%d",   MAX_SLOTS, TRUE );

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		AsciiString winName;
		winName.format( "GameSpyLoadScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the GameSpyLoadScreen loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load the Player's name
		winName.format( "GameSpyLoadScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the GameSpyLoadScreen loadscreen"));

		// Load MapStart Positions
		winName.format( "GameSpyLoadScreen.wnd:ButtonMapStartPosition%d",i);
		m_buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_buttonMapStartPosition[i], ("Can't initialize the MapStart Positions for the GameSpyLoadScreen loadscreen"));


		// Load the Player's Side
		winName.format( "GameSpyLoadScreen.wnd:StaticTextSide%d",i);
		m_playerSide[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerSide[i], ("Can't initialize the Sides for the GameSpyLoadScreen loadscreen"));

		// Load the Player's window
		winName.format( "GameSpyLoadScreen.wnd:WinPlayer%d",i);
		m_playerWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerWin[i], ("Can't initialize the WinPlayer for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerTotalDisconnects
		winName.format( "GameSpyLoadScreen.wnd:StaticTextTotalDisconnects%d",i);
		m_playerTotalDisconnects[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerTotalDisconnects[i], ("Can't initialize the m_playerTotalDisconnects for the GameSpyLoadScreen loadscreen"));

//		// Load the Player's m_playerFavoriteFactions
//		winName.format( "GameSpyLoadScreen.wnd:StaticTextFavoriteFaction%d",i);
//		m_playerFavoriteFactions[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
//		DEBUG_ASSERTCRASH(m_playerFavoriteFactions[i], ("Can't initialize the StaticTextFavoriteFaction for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerWinLosses
		winName.format( "GameSpyLoadScreen.wnd:StaticTextWinLoss%d",i);
		m_playerWinLosses[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerWinLosses[i], ("Can't initialize the m_playerWinLosses for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerWinLosses
		winName.format( "GameSpyLoadScreen.wnd:WinRank%d",i);
		m_playerRank[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerRank[i], ("Can't initialize the m_playerRank for the GameSpyLoadScreen loadscreen"));

		// Load the Player's m_playerOfficerMedal
		winName.format( "GameSpyLoadScreen.wnd:WinOfficer%d",i);
		m_playerOfficerMedal[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerOfficerMedal[i], ("Can't initialize the m_playerOfficerMedal for the GameSpyLoadScreen loadscreen"));

		winName.format( "MultiplayerLoadScreen.wnd:StaticTextTeam%d",i);
		teamWin[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));

		// get the slot man!
		GameSpyGameSlot *slot = (GameSpyGameSlot *)game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		// GeneralsX: Same null row as MultiPlayerLoadScreen::init above -
		// see the long note there for the mechanism. GameSpyLoadScreen.wnd authors the same eight
		// rows, and this branch dereferences seven widget arrays by netSlot, so it faults on the
		// ninth occupied slot exactly as the LAN screen did. Not yet reachable in practice - the
		// online path caps humans lower - but it is the identical defect and leaving one of a pair
		// fixed is how the first one survived a fix in the first place.
		if (m_progressBars[netSlot] == nullptr || m_playerNames[netSlot] == nullptr ||
				m_playerSide[netSlot] == nullptr || m_playerRank[netSlot] == nullptr ||
				m_playerOfficerMedal[netSlot] == nullptr || m_playerWinLosses[netSlot] == nullptr ||
				m_playerTotalDisconnects[netSlot] == nullptr)
			continue;

		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
#if RTS_GENERALS
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot],houseColor );
#else
		// format the progress bar to house colors
		AsciiString imageName;
		imageName.format("LoadingBar_ProgressCenter%d", slot->getApparentColor());
		const Image *houseImage = TheMappedImageCollection->findImageByName(imageName);
		if (! houseImage)
			houseImage = TheMappedImageCollection->findImageByName("LoadingBar_Progress");
		m_progressBars[netSlot]->winSetEnabledImage( 6, houseImage );
#endif

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		// Get the stats for the player
		PSPlayerStats stats = TheGameSpyPSMessageQueue->findPlayerStatsByID(slot->getProfileID());
		DEBUG_LOG(("LoadScreen - populating info for %ls(%d) - stats returned id %d",
			slot->getName().str(), slot->getProfileID(), stats.id));

		Bool isPreorder = TheGameSpyInfo->didPlayerPreorder(stats.id);
		Int rankPoints = CalculateRank(stats);
		Int favSide = GetFavoriteSide(stats);
		const Image *preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
		if (!isPreorder)
			preorderImg = nullptr;
		const Image *rankImg = LookupSmallRankImage(favSide, rankPoints);
		// GeneralsX @bugfix: these two lines indexed the widget arrays with the raw slot
		// index 'i', while every other widget populated in this loop uses the compacted
		// 'netSlot' - including the isAI() block below, which hides m_playerRank[netSlot]
		// and m_playerOfficerMedal[netSlot]. With a gap in the slot list (an empty/closed
		// slot ahead of an occupied one) i != netSlot, so a player's name, side and W/L
		// record landed in row netSlot while their rank badge and officer medal were
		// painted into row i - a row belonging to someone else, or one of the trailing
		// rows hidden after the loop. It also meant the AI hide covered a row that was
		// never drawn into. Cosmetic, GameSpy/online only, but plainly unintended.
		m_playerOfficerMedal[netSlot]->winSetEnabledImage(0, preorderImg);
		m_playerRank[netSlot]->winSetEnabledImage(0, rankImg);

		UnicodeString formatString;

		// pop wins and losses
		Int numLosses = 0;
		PerGeneralMap::iterator it;
		for(it = stats.losses.begin(); it != stats.losses.end(); ++it)
		{
			numLosses += it->second;
		}
		Int numWins = 0;
		for(it =stats.wins.begin(); it != stats.wins.end(); ++it)
		{
			numWins += it->second;
		}
		formatString.format(L"%d/%d", numWins, numLosses);
		GadgetStaticTextSetText(m_playerWinLosses[netSlot], formatString);
		m_playerWinLosses[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerWinLosses[netSlot]->winGetEnabledTextBorderColor());
		// favoriteFaction
			Int numGames = 0;
		Int favorite = 0;
		for(it =stats.games.begin(); it != stats.games.end(); ++it)
		{
			if(it->second >= numGames)
			{
				numGames = it->second;
				favorite = it->first;
			}
		}
//		if(numGames == 0)
//			GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch("GUI:None"));
//		else if( stats.gamesAsRandom > numGames )
//			GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch("GUI:Random"));
//		else
//		{
//			const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(favorite);
//			if (fac)
//			{
//				AsciiString side;
//				side.format("SIDE:%s", fac->getSide().str());
//
//				GadgetStaticTextSetText(m_playerFavoriteFactions[netSlot], TheGameText->fetch(side));
//			}
//		}
//		m_playerFavoriteFactions[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerFavoriteFactions[netSlot]->winGetEnabledTextBorderColor());
		// disconnects
		numGames = 0;
		for(it =stats.discons.begin(); it != stats.discons.end(); ++it)
		{
			numGames += it->second;
		}
		for(it =stats.desyncs.begin(); it != stats.desyncs.end(); ++it)
		{
			numGames += it->second;
		}
		numGames += GetAdditionalDisconnectsFromUserFile(stats.id);

		formatString.format(L"%d", numGames);
		GadgetStaticTextSetText(m_playerTotalDisconnects[netSlot], formatString);
		m_playerTotalDisconnects[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerTotalDisconnects[netSlot]->winGetEnabledTextBorderColor());
		GadgetStaticTextSetText(m_playerSide[netSlot], slot->getApparentPlayerTemplateDisplayName() );
		m_playerSide[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerSide[netSlot]->winGetEnabledTextBorderColor());

		if (slot->isAI())
		{
			if (m_progressBars[netSlot])
				m_progressBars[netSlot]->winHide(TRUE);
			if (m_playerTotalDisconnects[netSlot])
				m_playerTotalDisconnects[netSlot]->winHide(TRUE);
//			if (m_playerFavoriteFactions[netSlot])
//				m_playerFavoriteFactions[netSlot]->winHide(TRUE);
			if (m_playerWinLosses[netSlot])
				m_playerWinLosses[netSlot]->winHide(TRUE);
			if (m_playerRank[netSlot])
				m_playerRank[netSlot]->winHide(TRUE);
			if (m_playerOfficerMedal[netSlot])
				m_playerOfficerMedal[netSlot]->winHide(TRUE);
		}

		if (teamWin[netSlot])
		{
			AsciiString teamStr;
			teamStr.format("Team:%d", slot->getTeamNumber() + 1);
			if (slot->isAI() && slot->getTeamNumber() == -1)
				teamStr = "Team:AI";
			GadgetStaticTextSetText(teamWin[netSlot], TheGameText->fetch(teamStr));
			teamWin[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerNames[netSlot]->winGetEnabledTextBorderColor());
		}

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	// GeneralsX: SLOTS 8+ HAVE NO .WND CONTROL, AND THIS CRASHED.
	//
	// MultiplayerLoadScreen.wnd and GameSpyLoadScreen.wnd author rows 0..7 - read out of
	// WindowZH.big. MAX_SLOTS became 12, so winGetWindowFromId returns null for 8..11
	// (GameWindowManager.cpp:719) and this tail loop dereferenced it unguarded. It runs from
	// netSlot, the number of OCCUPIED slots, so the fewer players there are the more of it
	// executes: hosting alone means netSlot == 1 and the loop reaches i == 8 every single time.
	// That is "crashes when loading into a match in the lobby, alone", exactly as reported.
	// Note the asymmetry this replaces - teamWin[netSlot] was already null-checked a dozen
	// lines above, in the occupied branch. Only the tail was left bare.
	//
	// WHY NO TEST CAUGHT IT: -headless installs GameWindowManagerDummy (GameClient.cpp:367),
	// whose winGetWindowFromId override returns m_windowList rather than null "because callers
	// expect this to be non-null" (GameWindowManager.cpp:4214-4223). Every headless 12-slot
	// soak was structurally incapable of reaching this fault.
	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		if (m_playerWin[i]) m_playerWin[i]->winHide(TRUE);
		//m_playerNames[i]->winHide(TRUE);
		//m_playerSide[i]->winHide(TRUE);
	}

	if(m_mapPreview)
	{
		const MapMetaData *mmd = TheMapCache->findMap(game->getMap());
		Image *image = getMapPreviewImage(game->getMap());
		m_mapPreview->winSetUserData((void *)mmd);

		positionStartSpots( game, m_buttonMapStartPosition, m_mapPreview);
		updateMapStartSpots( game, m_buttonMapStartPosition, TRUE );
		//positionAdditionalImages((MapMetaData *)mmd, m_mapPreview, TRUE);
		if(image)
		{
			m_mapPreview->winSetStatus(WIN_STATUS_IMAGE);
			m_mapPreview->winSetEnabledImage(0, image);
		}
		else
		{
			m_mapPreview->winClearStatus(WIN_STATUS_IMAGE);
		}
	}

	TheGameLogic->initTimeOutValues();
}

void GameSpyLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_playerSide[i]= nullptr;
	}
}

void GameSpyLoadScreen::update( Int percent )
{
	if(percent <= 100)
		TheNetwork->updateLoadProgress( percent );
	TheNetwork->liteupdate();

	//GadgetProgressBarSetProgress(m_progressBars[TheNetwork->getLocalPlayerID()], percent );

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

	// Do this last!
	LoadScreen::update( percent );
}

void GameSpyLoadScreen::processProgress(Int playerId, Int percentage)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}
	//DEBUG_LOG(("Percentage %d was passed in for Player %d (in loadscreen position %d)", percentage, playerId, m_playerLookup[playerId]));
	if(m_progressBars[m_playerLookup[playerId]])
		GadgetProgressBarSetProgress(m_progressBars[m_playerLookup[playerId]], percentage );
}

// MapTransferLoadScreen Class //////////////////////////////////////////////////
//-----------------------------------------------------------------------------
MapTransferLoadScreen::MapTransferLoadScreen()
{
	m_gxWarmWaited = TRUE;	// GeneralsX A2: its 100% is a finished file transfer, not a finished load
	m_oldTimeout = 0;
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_progressText[i]= nullptr;
		m_playerLookup[i] = -1;
		m_oldProgress[i] = -1;
	}
	m_fileNameText = nullptr;
	m_timeoutText = nullptr;
	m_gatedSceneRender = FALSE;
	m_prevLoadScreenRender = FALSE;
	m_drawViewsAtStart = Display::getDrawViewsCount();
}

MapTransferLoadScreen::~MapTransferLoadScreen()
{
	if (m_gatedSceneRender)
	{
		TheWritableGlobalData->m_loadScreenRender = m_prevLoadScreenRender;
		fprintf(stderr, "[GXXFERSCREEN] down: scene render restored to %d\n", m_prevLoadScreenRender ? 1 : 0);
	}
	// The decidable line: 0 means the world was never rendered while this screen was up. A leak
	// (the gate off, e.g. GX_XFER_UNGATED=1) prints one per doFileTransfer draw on a rendered peer.
	// A -headless peer always prints 0 (W3DDisplay::draw returns before drawViews), so read it rendered.
	fprintf(stderr, "[GXXFERSCREEN] drawViews ran %u times while the transfer screen was up\n",
		Display::getDrawViewsCount() - m_drawViewsAtStart);
}

void MapTransferLoadScreen::init( GameInfo *game )
{
	// create the layout of the load screen
	m_loadScreen = TheWindowManager->winCreateFromScript( "Menus/MapTransferScreen.wnd" );
	DEBUG_ASSERTCRASH(m_loadScreen, ("Can't initialize the map transfer loadscreen"));
	if (!m_loadScreen)
		return;

	m_loadScreen->winHide(FALSE);
	m_loadScreen->winBringToTop();

	// GeneralsX: Stop drawing the world behind the transfer screen, as every
	// other load screen already does (GameLogic::startNewGame, ScoreScreen set the same flag).
	//
	// This is the one load-type screen that can come up while a GAME_SHELL game is still loaded: the
	// LAN delivery run keeps the shell map alive so the room survives the transfer (LANAPICallbacks.cpp,
	// clearGameData moved after it), and doFileTransfer calls update() - a full TheDisplay->draw() -
	// in an unthrottled loop. With m_loadScreenRender FALSE that draw ran drawViews(), rendering the
	// whole shell map every iteration underneath this layout, which is how the shell map could reach
	// the screen during a transfer at all (owner report: it shows in the bottom-right). With it TRUE
	// W3DDisplay::draw repaints only the windows (W3DInGameUI::draw -> winRepaint) and the mouse, so
	// the room below and this layout still draw. The old value is put back in the destructor: a
	// delivery run returns to the room, which needs the scene drawn again.
	// GX_XFER_UNGATED=1 skips the gate: the red arm of the rendered A/B, from the same binary.
	static const Bool s_ungated = (getenv("GX_XFER_UNGATED") != nullptr);
	m_prevLoadScreenRender = TheGlobalData->m_loadScreenRender;
	if (!s_ungated)
	{
		TheWritableGlobalData->m_loadScreenRender = TRUE;
		m_gatedSceneRender = TRUE;
	}
	fprintf(stderr, "[GXXFERSCREEN] up: display=%dx%d, scene render %s (was %d)\n",
		TheDisplay ? (Int)TheDisplay->getWidth() : -1, TheDisplay ? (Int)TheDisplay->getHeight() : -1,
		s_ungated ? "NOT gated (GX_XFER_UNGATED)" : "gated", m_prevLoadScreenRender ? 1 : 0);

	// GeneralsX: MapTransferScreen.wnd authors its per-row controls 0..7
	// and MAX_SLOTS is 12, so a 9th-12th human transferring a map had NO control to be drawn on and
	// was simply invisible on this screen - no name, no bar, no percentage - while the transfer
	// itself worked. The sibling load screens both build their missing rows already:
	// MultiplayerLoadScreen::init and GameSpyLoadScreen::init directly above call this same helper.
	// This one never did.
	//
	// LoadScreenEnsureRows is a null check when no builder is registered, so the Generals twin -
	// which has no CodeGui and registers nothing - behaves exactly as it does today.
	LoadScreenEnsureRows( m_loadScreen, "MapTransferScreen.wnd:ProgressLoad%d",       MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "MapTransferScreen.wnd:StaticTextPlayer%d",   MAX_SLOTS );
	LoadScreenEnsureRows( m_loadScreen, "MapTransferScreen.wnd:StaticTextProgress%d", MAX_SLOTS );

	DEBUG_ASSERTCRASH(TheNetwork, ("Where the Heck is the Network?!!!!"));
	DEBUG_LOG(("NumPlayers %d", TheNetwork->getNumPlayers()));

	AsciiString winName;
	Int i;

	// Load the Filename Text
	m_fileNameText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MapTransferScreen.wnd:StaticTextCurrentFile" ));
	DEBUG_ASSERTCRASH(m_fileNameText, ("Can't initialize the filename for the map transfer loadscreen"));

	// Load the Timeout Text
	m_timeoutText = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( "MapTransferScreen.wnd:StaticTextTimeout" ));
	DEBUG_ASSERTCRASH(m_timeoutText, ("Can't initialize the timeout for the map transfer loadscreen"));

	Int netSlot = 0;
	// Loop through and make the loadscreen look all good.
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		// Load the Progress Bar
		winName.format( "MapTransferScreen.wnd:ProgressLoad%d",i);
		m_progressBars[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressBars[i], ("Can't initialize the progressbars for the map transfer loadscreen"));
		// set the progressbar to zero
		GadgetProgressBarSetProgress(m_progressBars[i], 0 );

		// Load the Player's name
		winName.format( "MapTransferScreen.wnd:StaticTextPlayer%d",i);
		m_playerNames[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_playerNames[i], ("Can't initialize the Names for the map transfer loadscreen"));

		// Load the Progress Text
		winName.format( "MapTransferScreen.wnd:StaticTextProgress%d",i);
		m_progressText[i] = TheWindowManager->winGetWindowFromId( m_loadScreen,TheNameKeyGenerator->nameToKey( winName ));
		DEBUG_ASSERTCRASH(m_progressText[i], ("Can't initialize the progress text for the map transfer loadscreen"));

		// get the slot man!
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isHuman())
			continue;
		Color houseColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
		GadgetProgressBarSetEnabledBarColor(m_progressBars[netSlot], houseColor );

		UnicodeString name = slot->getName();
		GadgetStaticTextSetText(m_playerNames[netSlot], name );
		m_playerNames[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_playerNames[netSlot]->winGetEnabledTextBorderColor());

		GadgetStaticTextSetText(m_progressText[netSlot], UnicodeString::TheEmptyString );
		m_progressText[netSlot]->winSetEnabledTextColors(gxPlayerTextColor(houseColor), m_progressText[netSlot]->winGetEnabledTextBorderColor());

		if ((i == 0 || (TheGameInfo->getConstSlot(i)->isHuman() && TheGameInfo->getConstSlot(i)->hasMap())) && m_progressBars[netSlot])
			m_progressBars[netSlot]->winHide(TRUE);

		m_playerLookup[i] = netSlot; // save our mapping so we can update progress correctly

		netSlot++;
	}

	// GeneralsX: SLOTS 8+ HAVE NO .WND CONTROL, AND THIS CRASHED.
	//
	// MultiplayerLoadScreen.wnd and GameSpyLoadScreen.wnd author rows 0..7 - read out of
	// WindowZH.big. MAX_SLOTS became 12, so winGetWindowFromId returns null for 8..11
	// (GameWindowManager.cpp:719) and this tail loop dereferenced it unguarded. It runs from
	// netSlot, the number of OCCUPIED slots, so the fewer players there are the more of it
	// executes: hosting alone means netSlot == 1 and the loop reaches i == 8 every single time.
	// That is "crashes when loading into a match in the lobby, alone", exactly as reported.
	// Note the asymmetry this replaces - teamWin[netSlot] was already null-checked a dozen
	// lines above, in the occupied branch. Only the tail was left bare.
	//
	// WHY NO TEST CAUGHT IT: -headless installs GameWindowManagerDummy (GameClient.cpp:367),
	// whose winGetWindowFromId override returns m_windowList rather than null "because callers
	// expect this to be non-null" (GameWindowManager.cpp:4214-4223). Every headless 12-slot
	// soak was structurally incapable of reaching this fault.
	for(i = netSlot; i < MAX_SLOTS; ++i)
	{
		if (m_progressBars[i]) m_progressBars[i]->winHide(TRUE);
		if (m_playerNames[i])  m_playerNames[i]->winHide(TRUE);
		if (m_progressText[i]) m_progressText[i]->winHide(TRUE);
	}
}

void MapTransferLoadScreen::reset()
{
	setLoadScreen(nullptr);
	for(Int i = 0; i < MAX_SLOTS; ++i)
	{
		m_progressBars[i] = nullptr;
		m_playerNames[i] = nullptr;
		m_progressText[i]= nullptr;
		m_playerLookup[i] = -1;
		m_oldProgress[i] = -1;
	}
	m_fileNameText = nullptr;
	m_timeoutText = nullptr;
}

void MapTransferLoadScreen::update( Int percent )
{
	if (TheNetwork)
	{
		TheNetwork->liteupdate();
	}

	TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);

	// Do this last!
	LoadScreen::update( percent );
}

void MapTransferLoadScreen::processProgress(Int playerId, Int percentage, AsciiString stateStr)
{

	if( percentage < 0 || percentage > 100 || playerId >= MAX_SLOTS || playerId < 0 || m_playerLookup[playerId] == -1)
	{
		DEBUG_CRASH(("Percentage %d was passed in for Player %d", percentage, playerId));
		return;
	}

	if (m_oldProgress[playerId] == percentage)
		return;
	m_oldProgress[playerId] = percentage;

	Int translatedSlot = m_playerLookup[playerId];
	if(m_progressBars[translatedSlot])
		GadgetProgressBarSetProgress(m_progressBars[translatedSlot], percentage );
	if (m_progressText[translatedSlot])
		GadgetStaticTextSetText(m_progressText[translatedSlot], TheGameText->fetch(stateStr));
}

void MapTransferLoadScreen::processTimeout(Int secondsLeft)
{
	if (m_oldTimeout == secondsLeft)
		return;
	m_oldTimeout = secondsLeft;

	if (m_timeoutText)
	{
		UnicodeString txt;
		txt.format(TheGameText->fetchGameOnly("MapTransfer:Timeout"), (secondsLeft/60), (secondsLeft%60));
		GadgetStaticTextSetText(m_timeoutText, txt);
	}
}

void MapTransferLoadScreen::setCurrentFilename(AsciiString filename)
{
	if (m_fileNameText)
	{
		UnicodeString txt;
		txt.translate(TheGameState->getMapLeafName(filename));
		txt.format(TheGameText->fetchGameOnly("MapTransfer:CurrentFile"), txt.str());
		GadgetStaticTextSetText(m_fileNameText, txt);
	}
}

