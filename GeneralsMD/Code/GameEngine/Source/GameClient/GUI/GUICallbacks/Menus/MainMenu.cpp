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

// GeneralsX @feature BenderAI 21/04/2026 In-game update checker for tagged release builds
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
static GameWindow *updateNotifyButton = nullptr;  // GeneralsX @feature BenderAI 21/04/2026 Dynamically created update notification button
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
// GeneralsX @bugfix BenderAI 31/03/2026 Keep fallback credit label tied to main menu lifecycle so it does not leak into gameplay.
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

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 11/08/2026 Sign-in prompt on Online.
//
// The owner's requirement, verbatim: "when a player clicks online, if he isn't signed in it will
// prompt him to sign in and open the browser with the oAuth directly on discord where he accepts;
// and he's now logged in, that's it."
//
// SAYING NO DOES NOT OPEN THE LOBBY. See "ONLINE IS SIGN-IN ONLY" further down this file, which is
// the paragraph that governs.
//
// GeneralsX @bugfix Claude 20/09/2026 This block used to say the opposite - "SAYING NO STILL OPENS
// THE LOBBY. That is deliberate..." - and it had been false since 11/08/2026, when gxSignInNoCallback
// was changed to call gxSignInRefused() instead of gxEnterOnlineLobby(). It sat 1600 lines above the
// code that contradicts it, read as a design invariant, and cost a planning round on 20/09/2026: a
// brief was written around "declining Online still opens the lobby" and the owner had to correct it.
// The mechanism is at GXOnlineGate.h:79-88 (DECISION_ENTER only when hasVerifiedIdentity); the call
// site is the ButtonOnline branch of MainMenuSystem below, which takes the refusal path on anything
// but DECISION_ENTER; and scripts/test/online-gate-test.cpp:112 asserts that no login state with
// hasVerifiedIdentity=false ever enters, from either entry point.
//
// GameWinMsgBoxFunc is a bare void(*)() with no user data (GameWindow.h:85), which is why the
// intent lives in a file static - the same shape quitCallback below already uses.
// ------------------------------------------------------------------------------------------------

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

// GeneralsX @bugfix Claude 20/08/2026 The refusal box, deferred by one frame. See gxShowSignInRefusal.
static Bool          s_showRefusalPending = FALSE;
static UnicodeString s_pendingRefusalBody;
// GeneralsX @feature Claude 20/09/2026 The deferral now carries more than one kind of refusal, so
// the title travels with the body instead of being hardcoded at the drain.
static UnicodeString s_pendingRefusalTitle;

// GeneralsX @bugfix Claude 11/08/2026 A refusal has to be recorded somewhere, or it is not a
// refusal - it is a delay until the next button press.
//
// The gate in MainMenuSystem re-evaluates hasVerifiedIdentity() every time Online is pressed, so
// with nothing remembering the "No" a player who never intends to link an account gets the modal
// on every single press. GXIdentityAssertion.h makes an absent assertion a normal permanent
// state, which makes declining forever a supported answer rather than something to nag out of.
//
// Persisted rather than held for the session because a daily player would otherwise be asked
// daily, which is the same defect on a slower clock. It is not a dead end: /login in the lobby
// (GXLobbyCommands.cpp) still links an account at any time, and once one is linked
// hasVerifiedIdentity() short-circuits the gate permanently anyway.
//
// The value is cached in a static because the common path - already declined - would otherwise
// re-parse Options.ini on every press. Named constant rather than a repeated literal so a typo
// cannot leave the writer and the reader disagreeing, which would silently restore the nagging.
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

// GeneralsX @feature Claude 11/08/2026 ONLINE IS SIGN-IN ONLY.
//
// Both roads that used to end in the lobby without an identity end here instead. Online is the
// screen where other players see your name, add you as a friend and swap codes with you; every one
// of those is meaningless for somebody nobody can identify, and the card exchange has nothing to
// bind to. NETWORK is deliberately untouched - a LAN game needs no account and never will.
// GeneralsX @bugfix Claude 11/08/2026 SHOW THE SPECIFIC REASON, AND ALWAYS DRAIN THE QUEUE.
//
// GXLoginFlow queues the actionable text - "the account could not be saved" and friends - and its
// ONLY consumer in the tree is the lobby (WOLLobbyMenu.cpp:4539, unbounded grep). While a failed
// login still entered the lobby that was fine. Now that a refusal keeps the player on the main
// menu, showing only a generic "try again" would hide the one sentence that matters: a player
// whose assertion could not be PERSISTED is told to retry, and the retry fails identically for
// ever.
//
// Draining here also matters even when the queued line is not shown, because an undrained line
// survives into the NEXT attempt - so every refusal path goes through this one function.
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

	// GeneralsX @bugfix Claude 20/08/2026 QUEUE IT - DO NOT OPEN A MESSAGE BOX FROM INSIDE ONE.
	//
	// Reported by the operator: answer No to "Sign in?" and every main-menu button stops working.
	// Reproduced here by driving the UI - after No -> OK, ONLINE, NETWORK and BACK all do nothing.
	//
	// The cause is that this function used to run inside the Yes/No box's own noCallback:
	//   ExtendedMessageBox.cpp:284  noCallback() runs        <- opens a SECOND box here
	//   ExtendedMessageBox.cpp:66   the second box calls winSetModal() and overwrites the SHARED
	//                               global MsgBoxCallbacks struct that the first box is still using
	//   ExtendedMessageBox.cpp:294  winDestroy(window)       <- tears the FIRST box down underneath
	//                               the second one, with the modal stack now belonging to the second
	// The menu is left with a stranded modal, so input never returns to it.
	//
	// Showing it one frame later from MainMenuUpdate costs nothing visually - the player sees the
	// same box - and the first box is fully destroyed before the second is created.
	s_pendingRefusalBody = body;
	s_pendingRefusalTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignInNeededTitle", L"Sign in needed");
	s_showRefusalPending = TRUE;
}

// GeneralsX @bugfix Claude 20/09/2026 NO NETWORK GAME WITHOUT THE BASE GENERALS ARCHIVES.
//
// Zero Hour is an expansion and its maps reference base-game art. An install that cannot reach
// Textures.big/W3D.big/Terrain.big is missing the bridge models, and a peer that cannot load a
// bridge model computes a different bridge footprint from everybody else in the lobby - it would
// play a subtly different game and desync. Before 2026-09-20 it was worse still: the bridge
// SIMULATION objects were not created at all and the match desynced from frame 0, with one line
// on stderr as the only symptom.
//
// Both roads into a network game ask this, so ONLINE and NETWORK refuse identically. Skirmish,
// the campaign, replays and the map editor are untouched - they have exactly one peer, so a
// different footprint is not a disagreement with anybody. The question is the one that matters,
// asked directly: can every bridge model load (TerrainVisual::isEveryBridgeModelAvailable).
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

// GeneralsX @tweak BenderAI 31/03/2026 Print fixed project watermark in optional main-menu LabelVersion widget.
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

			// GeneralsX @feature BenderAI 31/03/2026 Fallback credit label when MainMenu.wnd lacks LabelVersion.
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
				// GeneralsX @tweak BenderAI 31/03/2026 Keep fallback watermark subtle and aligned to bottom-left target placement.
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

	// GeneralsX @bugfix Hide the Steam re-release's "Custom Mission" button.
	//
	// The Steam depot adds it in PatchWindow.big (MainMenu.wnd) with its strings
	// in PatchData.big (Data/Patch.str), and Steam's own executable implements the
	// feature. This engine descends from EA's 2003 GPL source, which predates it:
	// there is no handler for the control anywhere in the codebase, and the string
	// table loader never reads Patch.str, so the button renders as
	// MISSING:'GUI:CustomMission' and does nothing when pressed. Reproduced on
	// GeneralsOnline for Windows as well, so this is not port-specific.
	//
	// Hide it rather than label it: a correctly-labelled button that silently does
	// nothing is worse than no button. Should the feature ever be implemented,
	// delete this block and load Patch.str alongside the main string table.
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

	// GeneralsX @feature BenderAI 21/04/2026 Start background GitHub release check for tagged builds
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
	// GeneralsX @bugfix BenderAI 21/04/2026 Destroy the update notification button on
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

		// GeneralsX @bugfix Claude 14/09/2026 Read the EFFECTIVE size back, same as
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
	// GeneralsX @bugfix Claude 20/08/2026 Drain the deferred sign-in refusal here, one frame after
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
		// GeneralsX @bugfix Claude 11/08/2026 A pending "open the lobby once the login lands" is
		// scoped to the menu the player asked from. If they started a match while the browser was
		// still open, that intent is stale, and honouring it when the match ends would drop them
		// on a screen they did not ask for minutes earlier. The login itself still completes and
		// still counts - only the navigation is abandoned.
		s_openLobbyAfterLogin = FALSE;
		return;
	}

	// GeneralsX @feature Claude 11/08/2026 The player said yes, went to their browser, and came
	// back. Carry them into the lobby the moment the login lands.
	//
	// Watched here rather than signalled from GXLoginFlow because the flow lives below the GUI and
	// has no idea which screen is on top - the player may well have walked somewhere else while
	// the browser was open, and in that case this simply never fires and the login still took
	// effect. GXLoginFlow::update() is ticked from GameClient::update, so the state is already
	// current by the time this runs.
	//
	// GeneralsX @bugfix Claude 11/08/2026 This sits BELOW the in-game guard above, and must stay
	// there. Shell::update() calls runUpdate() on every layout still on the stack - there is no
	// hidden check and no top-of-stack check (Shell.cpp) - and GameClient::update() ticks the
	// shell every frame, so this function keeps running throughout a live match. Above the guard,
	// a login landing mid-match pushed WOLCustomLobby.wnd straight over the game; Shell::push()
	// has no duplicate or in-game check that would have caught it.
	if (s_openLobbyAfterLogin)
	{
		// GeneralsX @bugfix Claude 11/08/2026 A FAILED LOGIN NO LONGER OPENS THE LOBBY.
		//
		// This branch used to read "failed, timed out or cancelled -> into the lobby anyway,
		// unauthenticated", which was true of the old design and became a BYPASS the moment Online
		// required an identity: press Online, answer Yes, cancel in the browser, and you were in.
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

	// GeneralsX @feature BenderAI 21/04/2026 Poll background update check; create dynamic button when update found
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

				// GeneralsX @bugfix Claude 12/08/2026 DECIDE BEFORE TOUCHING ANY TRANSITION STATE.
				//
				// Reported by an external tester: begin the sign-in, close the browser without
				// finishing, and the game becomes a shell map with no UI that has to be force-quit.
				//
				// The four lines that used to sit here are retail's "the player is leaving the main
				// menu" preamble, and both halves of it are one-way. buttonPushed = TRUE makes
				// GBM_SELECTED break on EVERY later button (line ~1630), and reversing a FLASH or
				// WINFADE transition ENDS with winHide(TRUE) on each of its windows
				// (GameWindowTransitionsStyles.cpp:125 and :654) - for this group that is MapBorder1,
				// ButtonOnline, ButtonNetwork, ButtonMultiBack and the Logo. Retail could set both
				// unconditionally because the next statement was StartPatchCheck(), whose every abort
				// path calls HandleCanceledDownload() to put the menu back - the function is even
				// named CancelPatchCheckCallbackAndReopenDropdown (MainMenuUtils.cpp:560). Replacing
				// the patch chain with the sign-in gate removed the only caller of that recovery, so a
				// gate that refuses left the menu hidden with its input latched off and nothing on the
				// shell stack to restore it. An abandoned browser holds GX_LOGIN_WAITING for
				// GIVE_UP_MS - ten minutes (GXLoginFlow.cpp) - so DECISION_WAIT parks the player on
				// that dead screen for the whole of it.
				//
				// The Generals twin hit the identical wedge one day earlier and answered it this way,
				// in its own words: "Deliberately BEFORE any transition state is touched ... a message
				// shown from inside that half-transitioned state left the menu wedged behind the
				// dialog. Nothing here to restore means nothing to wedge." Zero Hour now agrees with
				// it. Calling HandleCanceledDownload() after the fact would also un-wedge the menu,
				// but it drops the player back to the top-level menu they did not ask to leave, and it
				// only helps a path that got as far as refusing.
				//
				// Answering Yes still navigates: MainMenuUpdate pushes the lobby when the login lands,
				// over a menu that is intact rather than latched.
				//
				// GeneralsX @feature Claude 11/08/2026 Ask about signing in first, once, if there is no
				// verified identity. hasVerifiedIdentity() is compiled on all four platforms (unlike the
				// flow itself used to be), and it is the same predicate /login uses.
				//
				// GeneralsX @bugfix Claude 11/08/2026 The gate is the identity ALONE. It deliberately
				// does not consult gxSignInDeclined(): a previous refusal must not become a bypass,
				// which is what it would be the moment declining still let you through.
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
						// GeneralsX @bugfix Claude 11/08/2026 THIS COPY MUST FIT THE STOCK PANEL.
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

				// GeneralsX @feature Matchmaking. Online now opens the RELAY lobby instead of
				// starting the GameSpy chain.
				//
				// StartPatchCheck() began patch-check -> login -> peerchat against
				// gamestats.gamespy.com and peerchat.gamespy.com. GameSpy was shut down in 2014,
				// so that path can only ever end in "CANNOT CONNECT ... please check your internet
				// connection" - which blames the player's connection for a service that no longer
				// exists. There is nothing to reconnect to and nothing to fix.
				//
				// The relay lobby does what Online used to: it reaches a server on startup, lists
				// every game being advertised, and joining one puts the match on the relay. Same
				// three steps, a backend we run.
				//
				// WOLCustomLobby.wnd is Zero Hour's OWN Online lobby - the multi-column browser
				// with a player list and sort buttons - and it has been sitting unused in our
				// archives the whole time. It is driven from our relay rather than from GameSpy;
				// see the "relay lobby" block in WOLLobbyMenu.cpp. This replaces the stopgap that
				// pointed Online at Menus/NetworkDirectConnect.wnd, which is a text field and a
				// combo box narrower than one of its own rows.
				//
				// NetworkDirectConnect.wnd is NOT retired: it is still reachable from the LAN
				// lobby, and it is the browser that has actually carried real matches. It stays
				// until this one has too.

				// GeneralsX @bugfix Claude 11/08/2026 Drop any pending post-login navigation before
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
				// GeneralsX @feature BenderAI 21/04/2026 Open GitHub releases page instead of legacy GameSpy patch download
				SDL_OpenURL("https://github.com/fbraz3/GeneralsX/releases");
#else
				StartDownloadingPatches();
#endif
			}
#ifdef SAGE_UPDATE_CHECK
			else if( updateNotifyButton != nullptr && control == updateNotifyButton )
			{
				// GeneralsX @feature BenderAI 21/04/2026 Dynamic update button click -> open releases page
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
