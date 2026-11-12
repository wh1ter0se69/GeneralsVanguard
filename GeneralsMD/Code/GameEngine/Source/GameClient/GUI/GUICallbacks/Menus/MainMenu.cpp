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

// FILE: MainMenu.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, October 2001
// Description: Main menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "gamespy/ghttp/ghttp.h"

#include "Lib/BaseType.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/RandomValue.h"
#include "Common/OptionPreferences.h"
#include "Common/version.h"
#include "Common/GameLOD.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/ExtendedMessageBox.h"
#include "GameClient/MessageBox.h"
#include "GameClient/GXLoginFlow.h"
#include "GameClient/GXOnlineGate.h"
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/Display.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/Mouse.h"
#include "GameClient/WindowVideoManager.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/HotKey.h"
#include "GameClient/GameClient.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/ChallengeGenerals.h"

#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/BuddyThread.h"

#include "GameNetwork/DownloadManager.h"
#include "GameNetwork/GameSpy/MainMenuUtils.h"

#include "GameClient/InGameUI.h"

// GeneralsX: In-game update checker for tagged release builds
#ifdef SAGE_UPDATE_CHECK
#include "Common/UpdateChecker.h"
#include "GameClient/GadgetPushButton.h"
#include <SDL3/SDL.h>
#endif


// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////

enum
{
	DROPDOWN_NONE = 0,
	DROPDOWN_SINGLE,
	DROPDOWN_MULTIPLAYER,
	DROPDOWN_MAIN,
	DROPDOWN_LOADREPLAY,
	DROPDOWN_DIFFICULTY,

	DROPDOWN_COUNT
};

static Bool raiseMessageBoxes = TRUE;
static Bool campaignSelected = FALSE;
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
static NameKeyType campaignID = NAMEKEY_INVALID;
static GameWindow *buttonCampaign = nullptr;
#ifdef TEST_COMPRESSION
static GameWindow *buttonCompressTest = nullptr;
void DoCompressTest();
#endif // TEST_COMPRESSION
#endif


// window ids -------------------------------------------------------------------------------------
static NameKeyType mainMenuID = NAMEKEY_INVALID;
static NameKeyType skirmishID = NAMEKEY_INVALID;
static NameKeyType onlineID = NAMEKEY_INVALID;
static NameKeyType networkID = NAMEKEY_INVALID;
static NameKeyType optionsID = NAMEKEY_INVALID;
static NameKeyType exitID = NAMEKEY_INVALID;
static NameKeyType motdID = NAMEKEY_INVALID;
static NameKeyType worldBuilderID = NAMEKEY_INVALID;
static NameKeyType getUpdateID = NAMEKEY_INVALID;
static NameKeyType buttonTRAININGID = NAMEKEY_INVALID;
static NameKeyType buttonChallengeID = NAMEKEY_INVALID;
static NameKeyType buttonUSAID = NAMEKEY_INVALID;
static NameKeyType buttonGLAID = NAMEKEY_INVALID;
static NameKeyType buttonChinaID = NAMEKEY_INVALID;
static NameKeyType buttonUSARecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonUSALoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonGLARecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonGLALoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonChinaRecentSaveID = NAMEKEY_INVALID;
static NameKeyType buttonChinaLoadGameID = NAMEKEY_INVALID;
static NameKeyType buttonSinglePlayerID = NAMEKEY_INVALID;
static NameKeyType buttonMultiPlayerID = NAMEKEY_INVALID;
static NameKeyType buttonMultiBackID = NAMEKEY_INVALID;
static NameKeyType buttonSingleBackID = NAMEKEY_INVALID;
static NameKeyType buttonLoadReplayBackID = NAMEKEY_INVALID;
static NameKeyType buttonReplayID = NAMEKEY_INVALID;
static NameKeyType buttonLoadReplayID = NAMEKEY_INVALID;
static NameKeyType buttonLoadID = NAMEKEY_INVALID;
static NameKeyType buttonCreditsID = NAMEKEY_INVALID;
static NameKeyType buttonEasyID = NAMEKEY_INVALID;
static NameKeyType buttonMediumID = NAMEKEY_INVALID;
static NameKeyType buttonHardID = NAMEKEY_INVALID;
static NameKeyType buttonDiffBackID = NAMEKEY_INVALID;


// window pointers --------------------------------------------------------------------------------
static GameWindow *parentMainMenu = nullptr;
static GameWindow *buttonSinglePlayer = nullptr;
static GameWindow *buttonMultiPlayer = nullptr;
static GameWindow *buttonSkirmish = nullptr;
static GameWindow *buttonOnline = nullptr;
static GameWindow *buttonNetwork = nullptr;
static GameWindow *buttonOptions = nullptr;
static GameWindow *buttonExit = nullptr;
static GameWindow *buttonMOTD = nullptr;
static GameWindow *buttonWorldBuilder = nullptr;
static GameWindow *mainMenuMovie = nullptr;
static GameWindow *getUpdate = nullptr;
#ifdef SAGE_UPDATE_CHECK
static GameWindow *updateNotifyButton = nullptr;  // GeneralsX: Dynamically created update notification button
static AsciiString s_updateLatestTag;             // Tag of available update
#endif
static GameWindow *buttonTRAINING = nullptr;
static GameWindow *buttonChallenge = nullptr;
static GameWindow *buttonUSA = nullptr;
static GameWindow *buttonGLA = nullptr;
static GameWindow *buttonChina = nullptr;
static GameWindow *buttonUSARecentSave = nullptr;
static GameWindow *buttonUSALoadGame = nullptr;
static GameWindow *buttonGLARecentSave = nullptr;
static GameWindow *buttonGLALoadGame = nullptr;
static GameWindow *buttonChinaRecentSave = nullptr;
static GameWindow *buttonChinaLoadGame = nullptr;
static GameWindow *buttonReplay = nullptr;
static GameWindow *buttonLoadReplay = nullptr;
static GameWindow *buttonLoad = nullptr;
static GameWindow *buttonCredits = nullptr;
static GameWindow *buttonEasy = nullptr;
static GameWindow *buttonMedium = nullptr;
static GameWindow *buttonHard = nullptr;
static GameWindow *buttonDiffBack = nullptr;
static GameWindow *dropDownWindows[DROPDOWN_COUNT];

static Bool buttonPushed = FALSE;
static Bool isShuttingDown = FALSE;
static Bool startGame = FALSE;
static Int	initialGadgetDelay = 210;
// GeneralsX: Keep fallback credit label tied to main menu lifecycle so it does not leak into gameplay.
static GameWindow *fallbackCreditLabel = nullptr;

enum
{
	SHOW_NONE = 0,
	SHOW_TRAINING,
	SHOW_USA,
	SHOW_GLA,
	SHOW_CHINA,
	SHOW_SKIRMISH,
	SHOW_FRAMES_LIMIT = 20
};

static Int showFade = FALSE;
static Int dropDown = DROPDOWN_NONE;
static Int pendingDropDown = DROPDOWN_NONE;
static AnimateWindowManager *localAnimateWindowManager = nullptr;
static Bool notShown = TRUE;
static Bool FirstTimeRunningTheGame = TRUE;

static Bool showLogo = FALSE;
static Int  showFrames = 0;
static Int  showSide = SHOW_NONE;
static Bool logoIsShown = FALSE;
static Bool justEntered = FALSE;
static Bool launchChallengeMenu = FALSE;

static Bool dontAllowTransitions = FALSE;

const Int /*TIME_OUT = 15,*/ CORNER = 10;
void AcceptResolution();
void DeclineResolution();
GameWindow *resAcceptMenu = nullptr;
extern DisplaySettings oldDispSettings, newDispSettings;
extern Bool dispChanged;
//static time_t timeStarted = 0, currentTime = 0;

void diffReverseSide();
void HandleCanceledDownload( Bool resetDropDown )
{
	buttonPushed = FALSE;
	if (resetDropDown)
	{
		dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
		TheTransitionHandler->setGroup("MainMenuDefaultMenuLogoFade");
	}
}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------

static void showSelectiveButtons( Int show )
{
	buttonUSARecentSave->winHide(!(show == SHOW_USA ));
	buttonUSALoadGame->winHide(!(show == SHOW_USA ));
	buttonGLARecentSave->winHide(!(show == SHOW_GLA ));
	buttonGLALoadGame->winHide(!(show == SHOW_GLA ));
	buttonChinaRecentSave->winHide(!(show == SHOW_CHINA ));
	buttonChinaLoadGame->winHide(!(show == SHOW_CHINA ));
}

// Sign-in prompt on Online: clicking Online while signed out opens the browser on the OAuth page; saying no
// does not open the lobby. The decision is GXOnlineGate.h (scripts/test/online-gate-test.cpp covers it).
// GameWinMsgBoxFunc is a bare void(*)() with no user data, so the intent lives in a file static.

// GXOnlineGate mirrors GXLoginState so its decision can be unit-tested without dragging the engine
// into the test TU. Pin the two together: adding or reordering a state in either is a COMPILE
// ERROR here rather than a gate that quietly decides the wrong thing.
static_assert((int)GXOnlineGate::LOGIN_IDLE      == (int)GX_LOGIN_IDLE,      "GXOnlineGate drifted from GXLoginState");
static_assert((int)GXOnlineGate::LOGIN_WAITING   == (int)GX_LOGIN_WAITING,   "GXOnlineGate drifted from GXLoginState");
static_assert((int)GXOnlineGate::LOGIN_OK        == (int)GX_LOGIN_OK,        "GXOnlineGate drifted from GXLoginState");
static_assert((int)GXOnlineGate::LOGIN_FAILED    == (int)GX_LOGIN_FAILED,    "GXOnlineGate drifted from GXLoginState");
static_assert((int)GXOnlineGate::LOGIN_TIMEOUT   == (int)GX_LOGIN_TIMEOUT,   "GXOnlineGate drifted from GXLoginState");
static_assert((int)GXOnlineGate::LOGIN_CANCELLED == (int)GX_LOGIN_CANCELLED, "GXOnlineGate drifted from GXLoginState");

static Bool s_openLobbyAfterLogin = FALSE;

// GeneralsX: The refusal box, deferred by one frame. See gxShowSignInRefusal.
static Bool          s_showRefusalPending = FALSE;
static UnicodeString s_pendingRefusalBody;
// GeneralsX: The deferral now carries more than one kind of refusal, so
// the title travels with the body instead of being hardcoded at the drain.
static UnicodeString s_pendingRefusalTitle;

// A refusal is recorded so the modal does not reappear on every Online press; declining forever is supported
// (/login in the lobby links an account later). Persisted so a daily player is not asked daily; cached in a static.
static const char * const GX_PREF_SIGN_IN_DECLINED = "GXSignInDeclined";
static Bool s_signInDeclinedLoaded = FALSE;
static Bool s_signInDeclined = FALSE;

static Bool gxSignInDeclined()
{
	if (!s_signInDeclinedLoaded)
	{
		OptionPreferences pref;
		s_signInDeclined = pref.getBool(GX_PREF_SIGN_IN_DECLINED, FALSE);
		s_signInDeclinedLoaded = TRUE;
	}
	return s_signInDeclined;
}

static void gxRememberSignInDeclined()
{
	s_signInDeclined = TRUE;
	s_signInDeclinedLoaded = TRUE;

	// OptionPreferences' constructor loads the file first, so this is a read-modify-write and
	// does not clobber anything else in Options.ini.
	OptionPreferences pref;
	pref.setBool(GX_PREF_SIGN_IN_DECLINED, TRUE);
	pref.write();
}

static void gxEnterOnlineLobby()
{
	TheShell->push( "Menus/WOLCustomLobby.wnd" );
}

// ONLINE IS SIGN-IN ONLY: other players see your name and add you as a friend. NETWORK is untouched.
// Show the specific reason and always drain the queue: only the lobby consumes GXLoginFlow's queued text,
// and an undrained line would survive into the next attempt.
static void gxShowSignInRefusal( const UnicodeString &fallbackBody )
{
	UnicodeString queued;
	Color queuedColor = 0;
	const Bool haveQueued = GXLoginFlow::takeMessage(&queued, &queuedColor);

	UnicodeString body;
	if (haveQueued && !queued.isEmpty())
	{
		// The specific reason, plus the one instruction it does not carry.
		body = queued;
		body.concat(L"\n");
		body.concat(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInRetryHint",
			L"NETWORK works without an account."));
	}
	else
	{
		body = fallbackBody;
	}

	// Queue the refusal rather than open a message box from inside one: the second box's winSetModal()
	// overwrites the shared MsgBoxCallbacks struct and destroying the first strands the modal stack.
	s_pendingRefusalBody = body;
	s_pendingRefusalTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInNeededTitle", L"Sign in needed");
	s_showRefusalPending = TRUE;
}

// No network game without the base Generals archives: Zero Hour's maps reference base-game art, and a peer that
// cannot load a bridge model computes a different footprint and desyncs. Single-peer modes do not ask.
static Bool gxNetworkPlayNeedsBaseAssets()
{
	return TheTerrainVisual != nullptr && !TheTerrainVisual->isEveryBridgeModelAvailable();
}

static void gxShowBaseAssetsRefusal()
{
	s_pendingRefusalTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBaseAssetsMissingTitle",
		L"Base game files missing");
	s_pendingRefusalBody = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBaseAssetsMissing",
		L"Network games need the original Generals data files, and this install cannot find them.\n"
		L"Copy Textures.big, W3D.big and Terrain.big from Command & Conquer Generals into a "
		L"ZH_Generals folder inside your Zero Hour folder, then restart.\n"
		L"Skirmish and the campaign still work.");
	s_showRefusalPending = TRUE;
}

static void gxSignInRefused()
{
	// The repeat wording is not decoration: somebody who already said no once is being told the
	// same thing again, and the useful sentence for them is where to go instead.
	gxShowSignInRefusal(gxSignInDeclined()
		? TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInStillNeeded",
			L"Online still needs an account. NETWORK works without one.")
		: TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInNeeded",
			L"Online needs an account so other players know who you are.\n"
			L"NETWORK works without one."));
}

// A login that got as far as opening a browser and then did not finish. Says which of the three
// things happened, because "it did not work" is the least useful sentence available and the player
// is the only one who can tell a cancel from a timeout.
static void gxSignInFailed( GXLoginState st )
{
	UnicodeString body;
	switch (st)
	{
		case GX_LOGIN_TIMEOUT:
			body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInTimedOut",
				L"That sign-in ran out of time.\nPress Online to try again.");
			break;
		case GX_LOGIN_CANCELLED:
			body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInCancelled",
				L"Sign-in was cancelled, so Online is still closed.\nNETWORK works without one.");
			break;
		default:
			body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInFailed",
				L"That sign-in did not complete, so Online is still closed.\n"
				L"Press Online to try again.");
			break;
	}

	gxShowSignInRefusal(body);
}

static void gxSignInYesCallback()
{
	// start() queues its own line on failure and returns FALSE.
	if (GXLoginFlow::start())
	{
		s_openLobbyAfterLogin = TRUE;
		return;
	}
	// A login that could not START is not a reason to let somebody into a lobby they cannot be
	// identified in. This used to fall through to gxEnterOnlineLobby().
	gxSignInRefused();
}

static void gxSignInNoCallback()
{
	// Still recorded, so the refusal above can tell "asked once" from "asked again". It no longer
	// grants entry: declining Online now means not playing Online.
	gxRememberSignInDeclined();
	gxSignInRefused();
}

static void quitCallback()
{
	buttonPushed = TRUE;
	TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_SELECTED]);
	TheShell->pop();
	TheGameEngine->setQuitting( TRUE );



	//if (!TheGameLODManager->didMemPass())
	{	//GIANT CRAPTACULAR HACK ALERT!!!!  On sytems with little memory, we skip all normal exit code
//		//and let Windows clean up the mess.  This reduces exit times from minutes to seconds.
//		//8-19-03. MW
//		delete TheGameClient;
//		_exit(EXIT_SUCCESS);

//  THE CRAP IS NOW EVEN MORE TACULAR
//  NOW WE PERSUADE THE MEMORYPOOLMANAGER TO RETURN STUPID FROM ITS FREE()
//    if (TheMemoryPoolFactory) TheMemoryPoolFactory->prepareForMinSpecShutDown();

	}
	if (TheGameLogic->isInGame())
		TheGameLogic->exitGame();
}


void setupGameStart(AsciiString mapName, GameDifficulty diff)
{
	TheCampaignManager->setGameDifficulty(diff);

	if (launchChallengeMenu)
	{
		if (TheChallengeGenerals)
			TheChallengeGenerals->setCurrentDifficulty(diff);

		campaignSelected = TRUE;
		TheShell->push( "Menus/ChallengeMenu.wnd" );
		TheTransitionHandler->reverse("MainMenuDifficultyMenuTraining");
	}
	else
	{
		startGame = TRUE;
		TheWritableGlobalData->m_pendingFile = mapName;
		TheShell->reverseAnimatewindow();
		TheTransitionHandler->setGroup("FadeWholeScreen");
	}
}

void prepareCampaignGame(GameDifficulty diff)
{
	dontAllowTransitions = TRUE;
	OptionPreferences pref;
	pref.setCampaignDifficulty(diff);
	pref.write();
	TheScriptEngine->setGlobalDifficulty(diff);

	buttonPushed = FALSE;
	TheTransitionHandler->reverse("MainMenuDifficultyMenuBack");
	setupGameStart(TheCampaignManager->getCurrentMap(), diff );
}
static void doGameStart()
{
	startGame = FALSE;

	if (TheGameLogic->isInGame())
		TheGameLogic->clearGameData();

	// send a message to the logic for a new game
	GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
	msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
	msg->appendIntegerArgument(TheCampaignManager->getGameDifficulty());
	msg->appendIntegerArgument(TheCampaignManager->getRankPoints());
	InitRandom(0);

	isShuttingDown = TRUE;
}

static void shutdownComplete( WindowLayout *layout )
{
	isShuttingDown = FALSE;
	if (fallbackCreditLabel)
	{
		TheWindowManager->winDestroy(fallbackCreditLabel);
		fallbackCreditLabel = nullptr;
	}

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );

}



/*
static void TimetToFileTime( time_t t, LPFILETIME pft )
{
	LONGLONG ll = Int32x32To64(t, 10000000) + 116444736000000000;
	pft->dwLowDateTime = (DWORD) ll;
	pft->dwHighDateTime = ll >>32;
}
*/

void initialHide()
{
GameWindow *win = nullptr;
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinGrowMarker"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTrainingSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTrainingMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmish"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmishSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionSkirmishMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUSSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUSMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLASmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLAMedium"));
	if(win)
		win->winHide(TRUE);

	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChinaSmall"));
	if(win)
		win->winHide(TRUE);
	win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChinaMedium"));
	if(win)
		win->winHide(TRUE);

}

// GeneralsX: Print fixed project watermark in optional main-menu LabelVersion widget.
static void initLabelVersion()
{
	NameKeyType versionID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:LabelVersion" );
	GameWindow *labelVersion = TheWindowManager->winGetWindowFromId( nullptr, versionID );
	UnicodeString creditText;
	creditText.translate("GeneralsX - Multiplatform C&C Generals");

	if (labelVersion)
	{
		GadgetStaticTextSetText( labelVersion, creditText );
		return;
	}

	if (!fallbackCreditLabel && TheWindowManager && parentMainMenu)
	{
		if (TheDisplay)
		{
			WinInstanceData instData;
			TextData textData;
			instData.init();
			memset(&textData, 0, sizeof(textData));
			textData.centered = 0;

			instData.m_style = GWS_STATIC_TEXT | GWS_MOUSE_TRACK;
			instData.m_textLabelString = "GeneralsXCreditLabel";

			const Int width = 560;
			const Int height = 28;
			const Int x = 8;
			const Int y = TheDisplay->getHeight() - height - 8;

			// GeneralsX: Fallback credit label when MainMenu.wnd lacks LabelVersion.
			fallbackCreditLabel = TheWindowManager->gogoGadgetStaticText(parentMainMenu,
				WIN_STATUS_ENABLED,
				x, y,
				width, height,
				&instData,
				&textData,
				nullptr,
				FALSE);

			if (fallbackCreditLabel)
			{
				// GeneralsX: Keep fallback watermark subtle and aligned to bottom-left target placement.
				fallbackCreditLabel->winSetFont(TheWindowManager->winFindFont("Arial", 12, FALSE));
				fallbackCreditLabel->winSetEnabledTextColors(GameMakeColor(255, 220, 60, 255), GameMakeColor(0, 0, 0, 0));
				GadgetStaticTextSetText(fallbackCreditLabel, creditText);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the main menu */
//-------------------------------------------------------------------------------------------------
void MainMenuInit( WindowLayout *layout, void *userData )
{
	TheWritableGlobalData->m_breakTheMovie = FALSE;

	TheShell->showShellMap(TRUE);
	TheMouse->setVisibility(TRUE);
	//winVidManager = NEW WindowVideoManager;
	buttonPushed = FALSE;
	isShuttingDown = FALSE;
	startGame = FALSE;
	dropDown = DROPDOWN_NONE;
	pendingDropDown = DROPDOWN_NONE;
	Int i = 0;
	for(; i < DROPDOWN_COUNT; ++i)
		dropDownWindows[i] = nullptr;

	// get ids for our windows
	mainMenuID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MainMenuParent" );
//	campaignID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonCampaign" );
	skirmishID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSkirmish" );
	onlineID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonOnline" );
	networkID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonNetwork" );
	optionsID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonOptions" );
	exitID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonExit" );
	motdID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMOTD" );
	worldBuilderID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonWorldBuilder" );
	getUpdateID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGetUpdate" );
//	buttonTRAININGID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonTRAINING" );
	buttonChallengeID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChallenge" );
	buttonUSAID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSA" );
	buttonGLAID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLA" );
	buttonChinaID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChina" );
	buttonUSARecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSARecentSave" );
	buttonUSALoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonUSALoadGame" );
	buttonGLARecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLARecentSave" );
	buttonGLALoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonGLALoadGame" );
	buttonChinaRecentSaveID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChinaRecentSave" );
	buttonChinaLoadGameID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonChinaLoadGame" );
	buttonSinglePlayerID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSinglePlayer" );
	buttonMultiPlayerID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMultiplayer" );
	buttonMultiBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMultiBack" );
	buttonSingleBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonSingleBack" );
	buttonLoadReplayBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadReplayBack" );
	buttonReplayID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonReplay" );
	buttonLoadReplayID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadReplay" );
	buttonLoadID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonLoadGame" );
	buttonCreditsID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonCredits" );

	buttonEasyID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonEasy" );
	buttonMediumID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonMedium" );
	buttonHardID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonHard" );
	buttonDiffBackID = TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonDiffBack" );

	// get pointers to the window buttons
	parentMainMenu = TheWindowManager->winGetWindowFromId( nullptr, mainMenuID );
	//buttonCampaign = TheWindowManager->winGetWindowFromId( parentMainMenu, campaignID );
	buttonSinglePlayer = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonSinglePlayerID );
	buttonMultiPlayer = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonMultiPlayerID );
	buttonSkirmish = TheWindowManager->winGetWindowFromId( parentMainMenu, skirmishID );
	buttonOnline = TheWindowManager->winGetWindowFromId( parentMainMenu, onlineID );
	buttonNetwork = TheWindowManager->winGetWindowFromId( parentMainMenu, networkID );
	buttonOptions = TheWindowManager->winGetWindowFromId( parentMainMenu, optionsID );
	buttonExit = TheWindowManager->winGetWindowFromId( parentMainMenu, exitID );
	buttonMOTD = TheWindowManager->winGetWindowFromId( parentMainMenu, motdID );
	buttonWorldBuilder = TheWindowManager->winGetWindowFromId( parentMainMenu, worldBuilderID );
	buttonReplay = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonReplayID );
	buttonLoadReplay = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonLoadReplayID );
	buttonLoad = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonLoadID );
	buttonCredits = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonCreditsID );

	// Hide the Steam re-release's "Custom Mission" button: this engine has no handler and never reads Patch.str,
	// so it would show MISSING:'GUI:CustomMission'. If the feature is implemented, delete this block.
	{
		GameWindow *buttonCustomMission = TheWindowManager->winGetWindowFromId(
			parentMainMenu,
			TheNameKeyGenerator->nameToKey( "MainMenu.wnd:ButtonCustomMission" ) );
		if( buttonCustomMission )
		{
			buttonCustomMission->winHide( TRUE );
		}
	}

	buttonEasy = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonEasyID );
	buttonMedium = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonMediumID );
	buttonHard = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonHardID );
	buttonDiffBack = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonDiffBackID );

	getUpdate = TheWindowManager->winGetWindowFromId( parentMainMenu, getUpdateID );
//	buttonTRAINING = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonTRAININGID );
	buttonChallenge = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChallengeID );
	buttonUSA = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSAID );
	buttonGLA = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLAID );
	buttonChina = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaID );
	buttonUSARecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSARecentSaveID );
	buttonUSALoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonUSALoadGameID );
	buttonGLARecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLARecentSaveID );
	buttonGLALoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonGLALoadGameID );
	buttonChinaRecentSave = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaRecentSaveID );
	buttonChinaLoadGame = TheWindowManager->winGetWindowFromId( parentMainMenu, buttonChinaLoadGameID );

	dropDownWindows[DROPDOWN_SINGLE] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder" ));
	dropDownWindows[DROPDOWN_MULTIPLAYER] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder1" ) );
	dropDownWindows[DROPDOWN_MAIN] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder2" ) );
	dropDownWindows[DROPDOWN_LOADREPLAY] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder3" ) );
	dropDownWindows[DROPDOWN_DIFFICULTY] = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MapBorder4" ) );
	for(i = 1; i < DROPDOWN_COUNT; ++i)
		dropDownWindows[i]->winHide(TRUE);

	initialHide();

	showSelectiveButtons(SHOW_NONE);
	// Set up the version number
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
	WinInstanceData instData;
#ifdef TEST_COMPRESSION
	instData.init();
	BitSet( instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
	instData.m_textLabelString = "Debug: Compress/Decompress Maps";
	instData.setTooltipText(L"Only Used in Debug and Internal!");
	buttonCompressTest = TheWindowManager->gogoGadgetPushButton( parentMainMenu,
																									 WIN_STATUS_ENABLED | WIN_STATUS_IMAGE,
																									 25, 175,
																									 400, 400,
																									 &instData, nullptr, TRUE );
#endif // TEST_COMPRESSION

	instData.init();
	BitSet( instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
	instData.m_textLabelString = "Debug: Load Map";

	instData.setTooltipText(L"Only Used in Debug and Internal!");
	buttonCampaign = TheWindowManager->gogoGadgetPushButton( parentMainMenu,
																									 WIN_STATUS_ENABLED,
																									 25, 54,
																									 180, 26,
																									 &instData, nullptr, TRUE );
#endif

	initLabelVersion();

	//TheShell->registerWithAnimateManager(buttonCampaign, WIN_ANIMATION_SLIDE_LEFT, TRUE, 800);
	//TheShell->registerWithAnimateManager(buttonSkirmish, WIN_ANIMATION_SLIDE_LEFT, TRUE, 600);
//	TheShell->registerWithAnimateManager(buttonSinglePlayer, WIN_ANIMATION_SLIDE_LEFT, TRUE, 400);
//	TheShell->registerWithAnimateManager(buttonMultiPlayer, WIN_ANIMATION_SLIDE_LEFT, TRUE, 200);
//	TheShell->registerWithAnimateManager(buttonOptions, WIN_ANIMATION_SLIDE_LEFT, TRUE, 1);
//	TheShell->registerWithAnimateManager(buttonExit, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);
//
	layout->hide( FALSE );

	/*
	if (!checkedForUpdate)
	{
		DWORD state = 0;
		Bool isConnected = InternetGetConnectedState(&state, 0);
		if (isConnected && !(state & INTERNET_CONNECTION_MODEM_BUSY))
		{
			// wohoo - we're connected!  fire off a check for updates
			checkedForUpdate = TRUE;
			DEBUG_LOG(("Looking for a patch for productID=%d, versionStr=%s, distribution=%d",
				gameProductID, gameVersionUniqueIDStr, gameDistributionID));
			ptCheckForPatch( gameProductID, gameVersionUniqueIDStr, gameDistributionID, patchAvailableCallback, PTFalse, nullptr );
			//ptCheckForPatch( productID, versionUniqueIDStr, distributionID, mapPackAvailableCallback, PTFalse, nullptr );
		}
	}
	if (getUpdate != nullptr)
	{
		getUpdate->winHide( TRUE );
		//getUpdate->winEnable( FALSE );
	}
	/**/

	// GeneralsX: Start background GitHub release check for tagged builds
#ifdef SAGE_UPDATE_CHECK
	UpdateChecker::start();
#endif

	if (TheGameSpyPeerMessageQueue && !TheGameSpyPeerMessageQueue->isConnected())
	{
		DEBUG_LOG(("Tearing down GameSpy from MainMenuInit()"));
		TearDownGameSpy();
	}
	if (TheMapCache)
		TheMapCache->updateCache();

	/*
	if (MOTDBuffer && buttonMOTD)
	{
		buttonMOTD->winHide(FALSE);
	}
	*/

	TheShell->loadScheme("MainMenu");
	raiseMessageBoxes = TRUE;

//	if(!localAnimateWindowManager)
//		localAnimateWindowManager = NEW AnimateWindowManager;

	//pendingDropDown =DROPDOWN_MAIN;


	GameWindow *rule = TheWindowManager->winGetWindowFromId( parentMainMenu, TheNameKeyGenerator->nameToKey( "MainMenu.wnd:MainMenuRuler" ) );
	if(rule)
		rule->winHide(TRUE);
	campaignSelected = FALSE;
//	dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
	if(FirstTimeRunningTheGame)
	{
		TheMouse->setVisibility(FALSE);

		TheTransitionHandler->reverse("FadeWholeScreen");
		FirstTimeRunningTheGame  = FALSE;
	}
	else
	{
		showFade = TRUE;
		justEntered = TRUE;
		initialGadgetDelay = 2;
		if(rule)
		rule->winHide(FALSE);
	}

	layout->bringForward();
	// set keyboard focus to main parent
	TheWindowManager->winSetFocus( parentMainMenu );


}

//-------------------------------------------------------------------------------------------------
/** Main menu shutdown method */
//-------------------------------------------------------------------------------------------------
void MainMenuShutdown( WindowLayout *layout, void *userData )
{
	if (!startGame)
		isShuttingDown = TRUE;
	if (fallbackCreditLabel)
	{
		TheWindowManager->winDestroy(fallbackCreditLabel);
		fallbackCreditLabel = nullptr;
	}

#ifdef SAGE_UPDATE_CHECK
	// GeneralsX: Destroy the update notification button on
	// shutdown so the pointer cannot dangle if the main menu is reopened in the same session.
	if (updateNotifyButton)
	{
		TheWindowManager->winDestroy(updateNotifyButton);
		updateNotifyButton = nullptr;
	}
#endif

	CancelPatchCheckCallback();

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;

//	if(winVidManager)
	//		delete winVidManager;
	//	winVidManager = nullptr;


	if( popImmediate )
	{
//		if(localAnimateWindowManager)
//		{
//			delete localAnimateWindowManager;
//			localAnimateWindowManager = nullptr;
//		}
		shutdownComplete( layout );
		return;

	}

	if (!startGame)
		TheShell->reverseAnimatewindow();
	//TheShell->reverseAnimatewindow();
//	if(localAnimateWindowManager && dropDown != DROPDOWN_NONE)
//		localAnimateWindowManager->reverseAnimateWindow();
}

extern Bool DontShowMainMenu;

////////////////////////////////////////////////////////////////////////////
//Allows the user to confirm the change, goes back to the previous mode
//if the time to change expires.
////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
// Accept Resolution callback method
//-------------------------------------------------------------------------------------------------
void AcceptResolution()
{
	//Keep new settings and bail with setting the display changed flag
	//set to off
	oldDispSettings = newDispSettings;
	dispChanged = FALSE;
}

//-------------------------------------------------------------------------------------------------
// Decline Resolution callback method
//-------------------------------------------------------------------------------------------------
void DeclineResolution()
{
	//Revert back to old resolution and reset all necessary
	//parts of the shell

	if (TheDisplay->setDisplayMode(oldDispSettings.xRes, oldDispSettings.yRes,
										oldDispSettings.bitDepth, oldDispSettings.windowed))
	{
		dispChanged = FALSE;
		newDispSettings = oldDispSettings;

		// GeneralsX: Read the EFFECTIVE size back, same as
		// OptionsMenu::saveOptions. setDisplayMode publishes what it actually set into these two
		// globals and can reduce a request no attached display can present, so the value restored
		// by a decline is not guaranteed to be the value asked for. Writing the requested one back
		// over it - which is what these two lines used to do - would persist a resolution that is
		// not running straight into Options.ini a few lines below, and the next launch would read
		// it back. A no-op whenever nothing was reduced.
		newDispSettings.xRes = TheGlobalData->m_xResolution;
		newDispSettings.yRes = TheGlobalData->m_yResolution;

		TheHeaderTemplateManager->onResolutionChanged();
		TheMouse->onResolutionChanged();

		AsciiString prefString;
		prefString.format("%d %d", newDispSettings.xRes, newDispSettings.yRes);

		OptionPreferences optionPref;
		optionPref["Resolution"] = prefString;
		optionPref.write();

		TheShell->recreateWindowLayouts();

		TheInGameUI->recreateControlBar();
	}
}

//-------------------------------------------------------------------------------------------------
// Accept/Decline Resolution dialog method
//-------------------------------------------------------------------------------------------------
void DoResolutionDialog()
{
	//Bring up a dialog to accept the resolution chosen in the options menu
	UnicodeString resolutionNew;

	UnicodeString resTimerString = TheGameText->fetch("GUI:Resolution");

	resolutionNew.format(L": %dx%d\n", newDispSettings.xRes , newDispSettings.yRes);

	resTimerString.concat(resolutionNew);


	resAcceptMenu = TheWindowManager->gogoMessageBox( CORNER, CORNER, -1, -1,MSG_BOX_OK | MSG_BOX_CANCEL ,
																									 TheGameText->fetch("GUI:Resolution"),
																									 resTimerString, nullptr, nullptr, AcceptResolution,
																									 DeclineResolution);
}

/* This function is not being currently used because we do not need a timer on the
// dialog box.
//-------------------------------------------------------------------------------------------------
//ResolutionDialogUpdate() - if resolution dialog box is shown, this must count 10 seconds for
//	accepting resolution changes otherwise we go back to previous display settings
//-------------------------------------------------------------------------------------------------
void ResolutionDialogUpdate()
{
	if (timeStarted == 0 && currentTime == 0)
	{
		timeStarted = currentTime = time(nullptr);
	}
	else
	{
		currentTime = time(nullptr);
	}

	if ( ( currentTime - timeStarted ) >= TIME_OUT)
	{
		currentTime = timeStarted = 0;
		DeclineResolution();
	}
	//------------------------------------------------------------------------------------------------------
	// Used for debugging purposes
	//------------------------------------------------------------------------------------------------------
	DEBUG_LOG(("Resolution Timer :  started at %d,  current time at %d, frameTicker is %d", timeStarted,
							time(nullptr) , currentTime));
}
*/

//-------------------------------------------------------------------------------------------------
/** Main menu update method */
//-------------------------------------------------------------------------------------------------
void DownloadMenuUpdate( WindowLayout *layout, void *userData );
void MainMenuUpdate( WindowLayout *layout, void *userData )
{
	// GeneralsX: Drain the deferred sign-in refusal here, one frame after
	// the Yes/No box asked for it, so that box is gone before this one is created. See
	// gxShowSignInRefusal for what opening it inline did to the modal stack.
	if( s_showRefusalPending )
	{
		s_showRefusalPending = FALSE;
		MessageBoxOk(s_pendingRefusalTitle, s_pendingRefusalBody, nullptr);
		s_pendingRefusalBody.clear();
		s_pendingRefusalTitle.clear();
	}

	if( TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() )
	{
		// GeneralsX: A pending "open the lobby once the login lands" is
		// scoped to the menu the player asked from. If they started a match while the browser was
		// still open, that intent is stale, and honouring it when the match ends would drop them
		// on a screen they did not ask for minutes earlier. The login itself still completes and
		// still counts - only the navigation is abandoned.
		s_openLobbyAfterLogin = FALSE;
		return;
	}

	// After the player signs in via the browser, carry them into the lobby when the login lands. Watched here
	// because GXLoginFlow does not know which screen is on top.
	// Must stay BELOW the in-game guard: Shell::update() runs every layout on the stack,
	// so a mid-match login would push the lobby.
	if (s_openLobbyAfterLogin)
	{
		// GeneralsX: A FAILED LOGIN NO LONGER OPENS THE LOBBY.
		//
		// The button refused you and this let you through one click later. Found by cross-model
		// review; the comment that justified it was the most dangerous part, because it read as a
		// deliberate exemption a later reader would respect.
		//
		// The decision now comes from GXOnlineGate, which the ONLINE button also asks, so the two
		// cannot drift apart again - and it is covered by scripts/test/online-gate-test.cpp,
		// because nothing else in this repository can see a menu callback.
		const GXLoginState st = GXLoginFlow::state();
		const GXOnlineGate::Decision decision = GXOnlineGate::afterLogin(
			(GXOnlineGate::LoginState)st, GXPlayerIdentity::hasVerifiedIdentity() != FALSE);

		if (decision == GXOnlineGate::DECISION_ENTER)
		{
			s_openLobbyAfterLogin = FALSE;
			gxEnterOnlineLobby();
			return;
		}
		if (decision == GXOnlineGate::DECISION_REFUSE)
		{
			s_openLobbyAfterLogin = FALSE;
			gxSignInFailed(st);
			return;
		}
		// DECISION_WAIT: the browser is still open. Fall through and keep waiting.
	}

	if(DontShowMainMenu && justEntered)
		justEntered = FALSE;

	// GeneralsX: Poll background update check; create dynamic button when update found
#ifdef SAGE_UPDATE_CHECK
	if (updateNotifyButton == nullptr)
	{
		const char* latestTag = nullptr;
		if (UpdateChecker::poll(&latestTag) && latestTag != nullptr && TheDisplay && parentMainMenu)
		{
			s_updateLatestTag = latestTag;

			const Int btnW = 260;
			const Int btnH = 26;
			const Int margin = 8;
			const Int x = TheDisplay->getWidth()  - btnW - margin;
			const Int y = TheDisplay->getHeight() - btnH - margin;

			WinInstanceData instData;
			instData.init();
			BitSet(instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);

			UnicodeString btnText;
			btnText.format(L"Update available: %hs - click to download", latestTag);
			instData.setTooltipText(btnText);

			AsciiString labelKey = "GeneralsXUpdateNotify";
			instData.m_textLabelString = labelKey;

			updateNotifyButton = TheWindowManager->gogoGadgetPushButton(
				parentMainMenu,
				WIN_STATUS_ENABLED,
				x, y, btnW, btnH,
				&instData, nullptr, TRUE);

			if (updateNotifyButton)
			{
				GameFont* font = TheWindowManager->winFindFont("Arial", 10, FALSE);
				if (font)
					updateNotifyButton->winSetFont(font);

				UnicodeString displayText;
				displayText.format(L"Update available: %hs", latestTag);
				GadgetButtonSetText(updateNotifyButton, displayText);
			}
		}
	}
#endif

	if (TheDownloadManager && !TheDownloadManager->isDone())
	{
		TheDownloadManager->update();
		DownloadMenuUpdate(layout, userData);
	}

	/* This is also commented for the same reason as the top
	if (dispChanged)
	{
		ResolutionDialogUpdate();
		return;
	}
	*/

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->setGroup("MainMenuDefaultMenuLogoFade");
			TheWindowManager->winSetFocus( parentMainMenu );
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}

	if(dontAllowTransitions && TheTransitionHandler->isFinished())
		dontAllowTransitions = FALSE;

	if(showLogo && dontAllowTransitions == FALSE)
	{
//		if(showFrames == SHOW_FRAMES_LIMIT)
//		{
//			TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
			switch (showSide) {
			case SHOW_TRAINING:
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				break;
			case SHOW_CHINA:
				TheTransitionHandler->setGroup("MainMenuFactionChina");
				break;
			case SHOW_GLA:
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
				break;
			case SHOW_USA:
				TheTransitionHandler->setGroup("MainMenuFactionUS");
				break;
			case SHOW_SKIRMISH:
				TheTransitionHandler->setGroup("MainMenuFactionSkirmish");
				break;
			}
			showLogo = FALSE;
//			showFrames = 0;
//			logoIsShown = TRUE;
//		}
//		else
//			showFrames++;
	}

//	if(showFade)
//	{
//		showFade = FALSE;
//		TheTransitionHandler->reverse("FadeWholeScreen");
//	}
////
//	if (notShown)
//	{
//		if(initialGadgetDelay == 1)
//		{
//			dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
//			TheTransitionHandler->setGroup("MainMenuFade", TRUE);
//			TheTransitionHandler->setGroup("MainMenuDefaultMenu");
//			TheMouse->setVisibility(TRUE);
//			initialGadgetDelay = 2;
//			notShown = FALSE;
//		}
//		else
//			initialGadgetDelay--;
//	}

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = FALSE;
	}

	HTTPThinkWrapper();
	GameSpyUpdateOverlays();
//	if(localAnimateWindowManager)
//		localAnimateWindowManager->update();
//	if(localAnimateWindowManager && pendingDropDown != DROPDOWN_NONE && localAnimateWindowManager->isFinished())
//	{
//		localAnimateWindowManager->reset();
//		if(dropDown != DROPDOWN_NONE)
//			dropDownWindows[dropDown]->winHide(TRUE);
//		dropDown = pendingDropDown;
//		dropDownWindows[dropDown]->winHide(FALSE);
//		localAnimateWindowManager->registerGameWindow(dropDownWindows[dropDown],WIN_ANIMATION_SLIDE_TOP_FAST,TRUE,1,1);
//		//buttonPushed = FALSE;
//		pendingDropDown = DROPDOWN_NONE;
//	}
//	else if(localAnimateWindowManager && dropDown == DROPDOWN_NONE && pendingDropDown == DROPDOWN_NONE && localAnimateWindowManager->isReversed() && localAnimateWindowManager->isFinished())
//	{
//		localAnimateWindowManager->reset();
//		for(Int i = 1; i < DROPDOWN_COUNT; ++i)
//			dropDownWindows[i]->winHide(TRUE);
//	}





	if (startGame && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		doGameStart();
	}

	// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		shutdownComplete(layout);
	}


	// We'll only be successful if we've requested to
//	if(TheShell->isAnimReversed() && TheShell->isAnimFinished())
//		shutdownComplete( layout );

//	if(winVidManager)
//		winVidManager->update();

}

//-------------------------------------------------------------------------------------------------
/** Main menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MainMenuInput( GameWindow *window, UnsignedInt msg,
																		WindowMsgData mData1, WindowMsgData mData2 )
{
	if(!notShown)
		return MSG_IGNORED;

	switch( msg )
	{
		// --------------------------------------------------------------------------------------------
		case GWM_MOUSE_POS:
		{
			// TheSuperHackers @tweak 26/02/2026 Show mouse and menu immediately when shellmap is disabled.
			Bool doShow = !TheGlobalData->m_shellMapOn;

			if (!doShow)
			{
				ICoord2D mouse;
				mouse.x = mData1 & 0xFFFF;
				mouse.y = mData1 >> 16;

				static Int mousePosX = mouse.x;
				static Int mousePosY = mouse.y;

				doShow = abs(mouse.x - mousePosX) > 20 || abs(mouse.y - mousePosY) > 20;
			}

			if (doShow)
			{
				initialGadgetDelay = 1;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->setGroup("MainMenuFade", TRUE);
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
				TheMouse->setVisibility(TRUE);
				notShown = FALSE;
				return MSG_HANDLED;
			}

			break;
		}

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			initialGadgetDelay = 1;
			dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
			TheTransitionHandler->setGroup("MainMenuFade", TRUE);
			TheTransitionHandler->setGroup("MainMenuDefaultMenu");
			TheMouse->setVisibility(TRUE);
			notShown = FALSE;
			return MSG_HANDLED;
		}
	}

	return MSG_IGNORED;
}

void PrintOffsetsFromControlBarParent();
//-------------------------------------------------------------------------------------------------
/** Main menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MainMenuSystem( GameWindow *window, UnsignedInt msg,
										 WindowMsgData mData1, WindowMsgData mData2 )
{
	static Bool triedToInitWOLAPI = FALSE;
	static Bool canInitWOLAPI = FALSE;

	switch( msg )
	{

		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
			ghttpStartup();
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			ghttpCleanup();
			DEBUG_LOG(("Tearing down GameSpy from MainMenuSystem(GWM_DESTROY)"));
			TearDownGameSpy();
			StopAsyncDNSCheck(); // kill off the async DNS check thread in case it is still running
			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			break;

		}
		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_ENTERING:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			if(controlID == onlineID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_ONLINE_HIGHLIGHTED]);
			}
			else if(controlID == networkID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_HIGHLIGHTED]);
			}
			else if(controlID == optionsID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_HIGHLIGHTED]);
			}
			else if(controlID == exitID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_HIGHLIGHTED]);
			}
			else if(controlID == buttonChallengeID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_TRAINING;
				}

				if(campaignSelected || dontAllowTransitions)
					break;

				TheTransitionHandler->setGroup("MainMenuFactionTraining");
			}
/*			else if(controlID == buttonTRAININGID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_TRAINING;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionTraining");

				//showSelectiveButtons(SHOW_NONE);
			}
*/			else if(controlID == skirmishID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_SKIRMISH;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionSkirmish");
				//showSelectiveButtons(SHOW_NONE);
			}

			else if(controlID == buttonUSAID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_USA;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");

				TheTransitionHandler->setGroup("MainMenuFactionUS");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_USA;

			}
			else if(controlID == buttonGLAID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_GLA;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_GLA;
			}
			else if(controlID == buttonChinaID)
			{
				if(dontAllowTransitions && !campaignSelected)
				{
					showLogo = TRUE;
					showSide = SHOW_CHINA;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				//TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->setGroup("MainMenuFactionChina");
//				showLogo = TRUE;
//				showFrames = 0;
//				showSide = SHOW_CHINA;
			}

		break;
		}
		//---------------------------------------------------------------------------------------------
		case GBM_MOUSE_LEAVING:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if(controlID == onlineID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_ONLINE_UNHIGHLIGHTED]);
			}
			else if(controlID == networkID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_UNHIGHLIGHTED]);
			}
			else if(controlID == optionsID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_UNHIGHLIGHTED]);
			}
			else if(controlID == exitID)
			{
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_EXIT_UNHIGHLIGHTED]);
			}
			else if(controlID == buttonChallengeID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}

				if(campaignSelected || dontAllowTransitions)
					break;

				// we'll just use the training logo anim for now
				TheTransitionHandler->reverse("MainMenuFactionTraining");
			}
/*			else if(controlID == buttonTRAININGID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}

				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionTraining");

				//showSelectiveButtons(SHOW_NONE);
			}
*/			else if(controlID == skirmishID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionSkirmish");
				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonUSAID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionUS");

				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonGLAID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionGLA");
				//showSelectiveButtons(SHOW_NONE);
			}
			else if(controlID == buttonChinaID)
			{
				if(dontAllowTransitions && !campaignSelected && showLogo)
				{
					showLogo = FALSE;
					showSide = SHOW_NONE;
				}
				if(campaignSelected || dontAllowTransitions)
					break;
				TheTransitionHandler->reverse("MainMenuFactionChina");
				//showSelectiveButtons(SHOW_NONE);
			}
		break;
		}
		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{

			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if(buttonPushed)
				break;
#if defined(RTS_DEBUG) || defined RTS_PROFILE_LEGACY
			if( control == buttonCampaign )
			{
				buttonPushed = TRUE;
				TheShell->push("Menus/MapSelectMenu.wnd");
				// As soon as we have a campaign, add it in here!;
			}
#ifdef TEST_COMPRESSION
			else if( control == buttonCompressTest )
			{
				DoCompressTest();
			}
#endif // TEST_COMPRESSION
			else
#endif

			// don't allow mouse click slop that occurs during transitions to unset this flag
			if (TheTransitionHandler->isFinished()
				&& controlID != buttonEasyID && controlID != buttonMediumID && controlID != buttonHardID)
			{
				// this toggle must only be reset if one of these buttons have not been pressed
				// ...the difficulty selection behavior must have a chance to act upon this toggle
				launchChallengeMenu = FALSE;
			}


			if( controlID == buttonSinglePlayerID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_SINGLE]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuSinglePlayerMenu");
			}
			else if( controlID == buttonSingleBackID )
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuSinglePlayerMenu");
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBack");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
				dontAllowTransitions = TRUE;
			}
			else if( controlID == buttonMultiBackID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuMultiPlayerMenu");
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuReverse");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
			}
			else if( controlID == buttonLoadReplayBackID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuLoadReplayMenu");
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBack");
				TheTransitionHandler->setGroup("MainMenuDefaultMenu");
			}

			else if( control == buttonCredits )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				TheShell->push("Menus/CreditsMenu.wnd" );
				dropDownWindows[DROPDOWN_MAIN]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuDefaultMenu");
			}
			else if( controlID == buttonMultiPlayerID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuMultiPlayerMenu");
			}
			else if( controlID == buttonLoadReplayID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				buttonPushed = FALSE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuDefaultMenu");
				TheTransitionHandler->reverse("MainMenuDefaultMenuBack");
				TheTransitionHandler->setGroup("MainMenuLoadReplayMenu");
			}
			else if( controlID == buttonLoadID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
//				SaveLoadLayoutType layoutType = SLLT_LOAD_ONLY;
//        WindowLayout *saveLoadMenuLayout = TheShell->getSaveLoadMenuLayout();
//				DEBUG_ASSERTCRASH( saveLoadMenuLayout, ("Unable to get save load menu layout.") );
//				saveLoadMenuLayout->runInit( &layoutType );
//				saveLoadMenuLayout->hide( FALSE );
//				saveLoadMenuLayout->bringForward();
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBackTransition");
				TheShell->push("Menus/SaveLoad.wnd");

			}
			else if( controlID == buttonReplayID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_LOADREPLAY]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuLoadReplayMenuBackTransition");
				TheShell->push("Menus/ReplayMenu.wnd");
			}
			else if( controlID == skirmishID )
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				buttonPushed = TRUE;
				campaignSelected = TRUE;
				dropDownWindows[DROPDOWN_SINGLE]->winHide(FALSE);
				TheTransitionHandler->remove("MainMenuFactionSkirmish");

				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackSkirmish");
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_CAMPEA_DEMO" );
/*
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuUS");
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_USA;
*/
				prepareCampaignGame(DIFFICULTY_NORMAL);
				break;
#endif
				TheShell->push( "Menus/SkirmishGameOptionsMenu.wnd" );
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_SKIRMISH_SELECTED]);
			}
			else if( controlID == onlineID )
			{
				if(dontAllowTransitions)
					break;

				if( gxNetworkPlayNeedsBaseAssets() )
				{
					gxShowBaseAssetsRefusal();
					break;
				}

				// Decide before touching transition state: the retail preamble is one-way (buttonPushed = TRUE, transition
				// reversal ends with winHide(TRUE)), and the sign-in gate has no recovery, so a refusal would leave the menu hidden.
				// Ask once if there is no verified identity; the gate ignores gxSignInDeclined() so a refusal is no bypass.
				const GXOnlineGate::Decision gate = GXOnlineGate::onOnlinePressed(
					(GXOnlineGate::LoginState)GXLoginFlow::state(),
					GXPlayerIdentity::hasVerifiedIdentity() != FALSE);

				if (gate != GXOnlineGate::DECISION_ENTER)
				{
					if (gate == GXOnlineGate::DECISION_WAIT)
					{
						// Their browser is already open. Asking again would start a second flow and
						// invalidate the first one's nonce.
						MessageBoxOk(
							TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInWaitTitle", L"Signing in"),
							TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInWait",
								L"Your browser is open. Finish there, then come back."),
							nullptr);
					}
					else
					{
						// GeneralsX: THIS COPY MUST FIT THE STOCK PANEL.
						// MessageBoxYesNo passes -1,-1,-1,-1 to gogoMessageBox (MessageBox.cpp:61)
						// and the retail box does not grow, so the previous five-line body was
						// drawn with its first line under the title and its last line - the one
						// saying you could play without signing in - never drawn at all.
						MessageBoxYesNo(
							TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInTitle", L"Sign in?"),
							TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInPrompt",
								L"Online needs an account so players know who you are.\n"
								L"Your browser will open - approve it, then come back."),
							gxSignInYesCallback, gxSignInNoCallback);
					}
					dropDown = DROPDOWN_NONE;
					break;
				}

				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuTransitionToNext");

				// Online opens the relay lobby instead of the GameSpy chain, which can only end in "CANNOT CONNECT".
				// WOLCustomLobby.wnd is Zero Hour's own Online lobby, driven from our relay (see WOLLobbyMenu.cpp).
				// NetworkDirectConnect.wnd is still reachable from the LAN lobby.

				// GeneralsX: Drop any pending post-login navigation before
				// entering, or the lobby opens twice. Falling through here with a login still in
				// flight is the GX_LOGIN_WAITING arm of the gate above - the player pressed Online
				// again while their browser was open - and s_openLobbyAfterLogin is still set from
				// the "Yes" that started it. Without this clear, MainMenuUpdate pushes
				// WOLCustomLobby.wnd a second time the moment the login lands, and Shell::push()
				// has no duplicate check that would swallow it.
				s_openLobbyAfterLogin = FALSE;
				gxEnterOnlineLobby();
//				localAnimateWindowManager->reverseAnimateWindow();
				dropDown = DROPDOWN_NONE;

			}
			else if( controlID == networkID )
			{
				if(dontAllowTransitions)
					break;

				// Refuse BEFORE latching anything. dontAllowTransitions and buttonPushed are both
				// one-way for this menu group - see the long note in the ONLINE branch above, which
				// was written after a gate that latched them first left the main menu hidden with
				// its input switched off and nothing on the shell stack to restore it.
				if( gxNetworkPlayNeedsBaseAssets() )
				{
					gxShowBaseAssetsRefusal();
					break;
				}

				dontAllowTransitions = TRUE;
				buttonPushed = TRUE;
				dropDownWindows[DROPDOWN_MULTIPLAYER]->winHide(FALSE);
				TheTransitionHandler->reverse("MainMenuMultiPlayerMenuTransitionToNext");
				TheShell->push( "Menus/LanLobbyMenu.wnd" );

				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_NETWORK_SELECTED]);
			}
			else if( controlID == optionsID )
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				//buttonPushed = TRUE;
				TheScriptEngine->signalUIInteract(TheShellHookNames[SHELL_SCRIPT_HOOK_MAIN_MENU_OPTIONS_SELECTED]);

				// load the options menu
				WindowLayout *optLayout = TheShell->getOptionsLayout(TRUE);
				DEBUG_ASSERTCRASH(optLayout != nullptr, ("unable to get options menu layout"));
				optLayout->runInit();
				optLayout->hide(FALSE);
				optLayout->bringForward();
			}
			else if( controlID == worldBuilderID )
			{
#if defined RTS_DEBUG
				if(_spawnl(_P_NOWAIT,"WorldBuilderD.exe","WorldBuilderD.exe", nullptr) < 0)
					MessageBoxOk(TheGameText->fetch("GUI:WorldBuilder"), TheGameText->fetch("GUI:WorldBuilderLoadFailed"),nullptr);
#else
				if(_spawnl(_P_NOWAIT,"WorldBuilder.exe","WorldBuilder.exe", nullptr) < 0)
					MessageBoxOk(TheGameText->fetch("GUI:WorldBuilder"), TheGameText->fetch("GUI:WorldBuilderLoadFailed"),nullptr);
#endif
			}
			else if( controlID == getUpdateID )
			{
#ifdef SAGE_UPDATE_CHECK
				// GeneralsX: Open GitHub releases page instead of legacy GameSpy patch download
				SDL_OpenURL("https://github.com/fbraz3/GeneralsX/releases");
#else
				StartDownloadingPatches();
#endif
			}
#ifdef SAGE_UPDATE_CHECK
			else if( updateNotifyButton != nullptr && control == updateNotifyButton )
			{
				// GeneralsX: Dynamic update button click -> open releases page
				SDL_OpenURL("https://github.com/fbraz3/GeneralsX/releases");
			}
#endif
			else if( controlID == exitID )
			{
				// If we ever want to add a dialog before we exit out of the game, uncomment this line and kill the quitCallback() line below.
//#if defined(RTS_DEBUG)
				if (TheGlobalData->m_windowed)
				{
					quitCallback();
//#else
				}
				else
				{
					QuitMessageBoxYesNo(TheGameText->fetch("GUI:QuitPopupTitle"), TheGameText->fetch("GUI:QuitPopupMessage"),quitCallback,nullptr);
				}
//#endif

			}
			else if(controlID == buttonChallengeID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;

				// set up for the difficulty select into challenge menu
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackTraining");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuTraining");
				campaignSelected = TRUE;
				showLogo = FALSE;
				showSide = SHOW_TRAINING;
				launchChallengeMenu = TRUE;
			}


// This button has been removed for the mission disk -June 2003
/*			else if(controlID == buttonTRAININGID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "TRAINING" );
				TheTransitionHandler->setGroup("MainMenuFactionTraining");
				TheTransitionHandler->remove("MainMenuFactionTraining", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionTraining"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackTraining");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuTraining");
				campaignSelected = TRUE;
				showLogo = FALSE;
				showSide = SHOW_TRAINING;

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
*/			else if(controlID == buttonUSAID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "USA" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_USA_1_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionUS");
				TheTransitionHandler->remove("MainMenuFactionUS", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionUS"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackUS");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuUS");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_USA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonGLAID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "GLA" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_USA_2_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionGLA");
				TheTransitionHandler->remove("MainMenuFactionGLA", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionGLA"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackGLA");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuGLA");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_GLA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonChinaID)
			{
				if(campaignSelected || dontAllowTransitions)
					break;
				TheCampaignManager->setCampaign( "China" );
#ifdef _CAMPEA_DEMO
				TheCampaignManager->setCampaign( "MD_GLA_3_DEMO" );
#endif
				TheTransitionHandler->setGroup("MainMenuFactionChina");
				TheTransitionHandler->remove("MainMenuFactionChina", TRUE);
				GameWindow *win = TheWindowManager->winGetWindowFromId(parentMainMenu, TheNameKeyGenerator->nameToKey("MainMenu.wnd:WinFactionChina"));
				if(win)
					win->winHide(TRUE);
				TheTransitionHandler->reverse("MainMenuSinglePlayerMenuBackChina");
				TheTransitionHandler->setGroup("MainMenuDifficultyMenuChina");
				campaignSelected = TRUE;
				logoIsShown = FALSE;
				showLogo = FALSE;
				showSide = SHOW_CHINA;
//				launchChallengeMenu = FALSE;
//				WindowLayout *layout = nullptr;
//				layout = TheWindowManager->winCreateLayout( "Menus/DifficultySelect.wnd" );
//				layout->runInit();
//				layout->hide( FALSE );
//				layout->bringForward();

//				setupGameStart(TheCampaignManager->getCurrentMap());
			}
			else if(controlID == buttonEasyID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_EASY);
			}
			else if(controlID == buttonMediumID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_NORMAL);
			}
			else if(controlID == buttonHardID)
			{
				if(dontAllowTransitions)
					break;

				prepareCampaignGame(DIFFICULTY_HARD);
			}
			else if(controlID == buttonDiffBackID)
			{
				if(dontAllowTransitions)
					break;
				dontAllowTransitions = TRUE;
				TheCampaignManager->setCampaign( AsciiString::TheEmptyString );
				diffReverseSide();
				campaignSelected = FALSE;
			}


			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

void diffReverseSide()
{
	switch (showSide) {
	case SHOW_TRAINING:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuTrainingBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerTrainingMenuFromDiff");
		break;
	case SHOW_USA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuUSBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerUSAMenuFromDiff");
		break;
	case SHOW_GLA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuGLABack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerGLAMenuFromDiff");
		break;
	case SHOW_CHINA:
		TheTransitionHandler->reverse("MainMenuDifficultyMenuChinaBack");
		TheTransitionHandler->setGroup("MainMenuSinglePlayerChinaMenuFromDiff");

		break;
	}
}
