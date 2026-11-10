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

// FILE: Diplomacy.cpp ///////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell - August 2002
// Desc: GUI callbacks for the diplomacy menu
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXTextColor.h"
#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Recorder.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/CodeGui.h"		// CodeGuiRowsEnsure - the .wnd authors 8 rows, MAX_SLOTS is 12
#include "GameClient/Diplomacy.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"


//-------------------------------------------------------------------------------------------------

static NameKeyType staticTextPlayerID[MAX_SLOTS];
static NameKeyType staticTextSideID[MAX_SLOTS];
static NameKeyType staticTextTeamID[MAX_SLOTS];
static NameKeyType staticTextStatusID[MAX_SLOTS];
static NameKeyType buttonMuteID[MAX_SLOTS];
static NameKeyType buttonUnMuteID[MAX_SLOTS];
static NameKeyType radioButtonInGameID = NAMEKEY_INVALID;
static NameKeyType radioButtonBuddiesID = NAMEKEY_INVALID;
static GameWindow *radioButtonInGame = nullptr;
static GameWindow *radioButtonBuddies = nullptr;
static NameKeyType winInGameID = NAMEKEY_INVALID;
static NameKeyType winBuddiesID = NAMEKEY_INVALID;
static NameKeyType winSoloID = NAMEKEY_INVALID;
static GameWindow *winInGame = nullptr;
static GameWindow *winBuddies = nullptr;
static GameWindow *winSolo = nullptr;
static GameWindow *staticTextPlayer[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextSide[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextTeam[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextStatus[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *buttonMute[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *buttonUnMute[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static Int slotNumInRow[MAX_SLOTS];

//-------------------------------------------------------------------------------------------------

static WindowLayout *theLayout = nullptr;
static GameWindow *theWindow = nullptr;
static AnimateWindowManager *theAnimateWindowManager = nullptr;
WindowMsgHandledType BuddyControlSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2);
void InitBuddyControls(Int type);
void updateBuddyInfo();
static void grabWindowPointers()
{
	// GeneralsX: SHOW ALL TWELVE, NOT THE EIGHT THE .WND AUTHORS.
	//
	// Diplomacy.wnd authors StaticTextPlayer0..7, StaticTextSide0..7, StaticTextTeam0..7,
	// StaticTextStatus0..7, ButtonMute0..7 and ButtonUnMute0..7 - verified by extracting it from
	// WindowZH.big, all six columns stop at 7. MAX_SLOTS became 12, so the loop below asks for
	// rows 8..11, winGetWindowFromId returns null for every one of them, and the in-game player
	// list silently shows eight of twelve players. Reported live from a running 12-slot match.
	//
	// Same fix and same helper as the three lobby screens: extrapolate the column's own pitch and
	// clone the missing rows into the band the authored ones occupy. CodeGuiRowsEnsure returns 0
	// without touching a rect when the layout already covers rowCount, so at the shipped 8-slot
	// layout this is a measurement and nothing else - and it returns 0 for a column it cannot find
	// two rows to read a pitch from, so a future .wnd that drops one of these is not a crash.
	//
	// It must run BEFORE the binding loop: the loop is what caches the pointers this creates.
	if (theWindow != nullptr)
	{
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:StaticTextPlayer%d", MAX_SLOTS, CODEGUI_ROWS_BAND );
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:StaticTextSide%d",   MAX_SLOTS, CODEGUI_ROWS_BAND );
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:StaticTextTeam%d",   MAX_SLOTS, CODEGUI_ROWS_BAND );
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:StaticTextStatus%d", MAX_SLOTS, CODEGUI_ROWS_BAND );
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:ButtonMute%d",       MAX_SLOTS, CODEGUI_ROWS_BAND );
		CodeGuiRowsEnsure( theWindow, "Diplomacy.wnd:ButtonUnMute%d",     MAX_SLOTS, CODEGUI_ROWS_BAND );
	}

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		AsciiString temp;
		temp.format("Diplomacy.wnd:StaticTextPlayer%d", i);
		staticTextPlayerID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextSide%d", i);
		staticTextSideID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextTeam%d", i);
		staticTextTeamID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextStatus%d", i);
		staticTextStatusID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:ButtonMute%d", i);
		buttonMuteID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:ButtonUnMute%d", i);
		buttonUnMuteID[i] = NAMEKEY(temp);

		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextPlayerID[i]);
		staticTextSide[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextSideID[i]);
		staticTextTeam[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextTeamID[i]);
		staticTextStatus[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextStatusID[i]);
		buttonMute[i] = TheWindowManager->winGetWindowFromId(theWindow, buttonMuteID[i]);
		buttonUnMute[i] = TheWindowManager->winGetWindowFromId(theWindow, buttonUnMuteID[i]);

		slotNumInRow[i] = -1;
	}
}

static void releaseWindowPointers()
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		staticTextPlayer[i] = nullptr;
		staticTextSide[i] = nullptr;
		staticTextTeam[i] = nullptr;
		staticTextStatus[i] = nullptr;
		buttonMute[i] = nullptr;
		buttonUnMute[i] = nullptr;

		slotNumInRow[i] = -1;
	}
}


//-------------------------------------------------------------------------------------------------

static void updateFunc( WindowLayout *layout, void *param )
{
	if (theAnimateWindowManager && TheGlobalData->m_animateWindows)
	{
		Bool wasFinished = theAnimateWindowManager->isFinished();
		theAnimateWindowManager->update();
		if (theAnimateWindowManager->isFinished() && !wasFinished && theAnimateWindowManager->isReversed())
			theWindow->winHide( TRUE );
	}

	// GeneralsX: The panel was populated ONCE, when it opened.
	//
	// Every figure in it moves - ping most obviously, but also alive/defeated and the mute state -
	// and none of them were refreshed while it was on screen. The only way to see a current ping
	// was to close the panel and reopen it, which is also the only reason the numbers ever appeared
	// to change: you were not watching them update, you were watching a second snapshot.
	//
	// PopulateInGameDiplomacyPopup only calls winSetText/winHide on windows that already exist - it
	// builds no rows and owns no listbox - so re-running it holds no selection or scroll state to
	// lose, which is what makes a periodic refresh safe here at all.
	//
	// Throttled rather than per-frame because the underlying figure cannot move faster than the
	// router publishes it (m_networkRunAheadMetricsTime apart, not once per frame), so a per-frame
	// rebuild would be pure cost for a value that is identical 59 times out of 60.
	//
	// timeGetTime, not the logic frame: this is client-side furniture and must keep updating while
	// the simulation is blocked waiting on a peer - which is exactly when somebody opens this panel
	// to find out whose connection is the problem.
	if (theWindow != nullptr && !theWindow->winIsHidden())
	{
		static UnsignedInt s_lastRepopulate = 0;
		const UnsignedInt now = timeGetTime();
		if (now - s_lastRepopulate >= 250)
		{
			s_lastRepopulate = now;
			PopulateInGameDiplomacyPopup();
		}
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
static BriefingList theBriefingList;

//-------------------------------------------------------------------------------------------------
BriefingList* GetBriefingTextList()
{
	return &theBriefingList;
}

//-------------------------------------------------------------------------------------------------
void UpdateDiplomacyBriefingText(AsciiString newText, Bool clear)
{
	GameWindow *listboxSolo = TheWindowManager->winGetWindowFromId(theWindow, NAMEKEY("Diplomacy.wnd:ListboxSolo"));

	if (clear)
	{
		theBriefingList.clear();
		if (listboxSolo)
			GadgetListBoxReset(listboxSolo);
	}

	if (newText.isEmpty())
		return;

	if (std::find(theBriefingList.begin(), theBriefingList.end(), newText) != theBriefingList.end())
		return;

	theBriefingList.push_back(newText);
	if (!listboxSolo)
		return;

	UnicodeString translated = TheGameText->fetch(newText);

	Int numEntries = GadgetListBoxGetNumEntries(listboxSolo);
	GadgetListBoxAddEntryText(listboxSolo, translated, TheInGameUI->getMessageColor(numEntries%2), -1);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ShowDiplomacy( Bool immediate )
{
	if (!TheInGameUI->getInputEnabled() || TheGameLogic->isIntroMoviePlaying() ||
			TheGameLogic->isLoadingMap())
		return;


	if (TheInGameUI->isQuitMenuVisible())
		return;

	if (TheDisconnectMenu && TheDisconnectMenu->isScreenVisible())
		return;

	if (theWindow)
	{
		theWindow->winHide(FALSE);
		theWindow->winEnable(TRUE);
	}
	else
	{
		theLayout = TheWindowManager->winCreateLayout( "Diplomacy.wnd" );
		theWindow = theLayout->getFirstWindow();
		theLayout->setUpdate(updateFunc);
		theAnimateWindowManager = NEW AnimateWindowManager;
		radioButtonInGameID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:RadioButtonInGame");
		radioButtonBuddiesID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:RadioButtonBuddies");
		radioButtonInGame = TheWindowManager->winGetWindowFromId(nullptr, radioButtonInGameID);
		radioButtonBuddies = TheWindowManager->winGetWindowFromId(nullptr, radioButtonBuddiesID);
		winInGameID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:InGameParent");
		winBuddiesID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:BuddiesParent");
		winSoloID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:SoloParent");
		winInGame = TheWindowManager->winGetWindowFromId(nullptr, winInGameID);
		winBuddies = TheWindowManager->winGetWindowFromId(nullptr, winBuddiesID);
		winSolo = TheWindowManager->winGetWindowFromId(nullptr, winSoloID);

		if (!TheRecorder->isMultiplayer())
		{
			GameWindow *listboxSolo = TheWindowManager->winGetWindowFromId(theWindow, NAMEKEY("Diplomacy.wnd:ListboxSolo"));
			if (listboxSolo)
			{
				for (BriefingList::iterator it = theBriefingList.begin(); it != theBriefingList.end(); ++it)
				{
					UnicodeString translated = TheGameText->fetch(*it);
					Int numEntries = GadgetListBoxGetNumEntries(listboxSolo);
					GadgetListBoxAddEntryText(listboxSolo, translated, TheInGameUI->getMessageColor(numEntries%2), -1);
				}
			}
		}
	}
	theLayout->hide(FALSE);

	radioButtonInGame->winHide(TRUE);
	radioButtonBuddies->winHide(TRUE);
	GadgetRadioSetSelection(radioButtonInGame, FALSE);
	if (TheRecorder->isMultiplayer())
	{
		winInGame->winHide(FALSE);
		winBuddies->winHide(TRUE);
		winSolo->winHide(TRUE);
	}
	else
	{
		winInGame->winHide(TRUE);
		winBuddies->winHide(TRUE);
		winSolo->winHide(FALSE);
	}

	theAnimateWindowManager->reset();
	if (!immediate && TheGlobalData->m_animateWindows)
		theAnimateWindowManager->registerGameWindow( theWindow, WIN_ANIMATION_SLIDE_TOP, TRUE, 200 );

	TheInGameUI->registerWindowLayout(theLayout);
	grabWindowPointers();
	PopulateInGameDiplomacyPopup();

	if(TheGameSpyInfo && TheGameSpyInfo->getLocalProfileID() != 0)
	{
		radioButtonInGame->winHide(FALSE);
		radioButtonBuddies->winHide(FALSE);
		InitBuddyControls(1);
		PopulateOldBuddyMessages();
		updateBuddyInfo();
	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ResetDiplomacy()
{
	if(theLayout)
	{
		TheInGameUI->unregisterWindowLayout(theLayout);
		theLayout->destroyWindows();
		deleteInstance(theLayout);
		InitBuddyControls(-1);
		theLayout = nullptr;
	}
	theWindow = nullptr;

	delete theAnimateWindowManager;
	theAnimateWindowManager = nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void HideDiplomacy( Bool immediate )
{
	releaseWindowPointers();
	if (theWindow)
	{
		if (immediate || !TheGlobalData->m_animateWindows)
		{
			theWindow->winHide(TRUE);
			theWindow->winEnable(FALSE);
		}
		else
		{
			if (theAnimateWindowManager->isFinished())
				theAnimateWindowManager->reverseAnimateWindow();
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ToggleDiplomacy( Bool immediate )
{
	// If we bring this up, let's hide the quit menu
	HideQuitMenu();

	if (theWindow)
	{
		Bool show = theWindow->winIsHidden();
		if (show)
			ShowDiplomacy( immediate );
		else
			HideDiplomacy( immediate );
	}
	else
	{
		ShowDiplomacy( immediate );
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType DiplomacyInput( GameWindow *window, UnsignedInt msg,
																			WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
//			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					HideDiplomacy();
					return MSG_HANDLED;
					//return MSG_IGNORED;
				}

			}

			return MSG_HANDLED;

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType DiplomacySystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	if(BuddyControlSystem(window, msg, mData1, mData2) == MSG_HANDLED)
	{
		return MSG_HANDLED;
	}
	switch( msg )
	{
		//---------------------------------------------------------------------------------------------
		case GGM_FOCUS_CHANGE:
		{
//			Bool focus = (Bool) mData1;
			//if (focus)
				//TheWindowManager->winSetGrabWindow( chatTextEntry );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// if we're given the opportunity to take the keyboard focus we must say we don't want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = FALSE;

			return MSG_HANDLED;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			NameKeyType controlID = (NameKeyType)control->winGetWindowId();
			static NameKeyType buttonHideID = NAMEKEY( "Diplomacy.wnd:ButtonHide" );
			if (controlID == buttonHideID)
			{
				HideDiplomacy( FALSE );
			}
			else if( controlID == radioButtonInGameID)
			{
				winInGame->winHide(FALSE);
				winBuddies->winHide(TRUE);
			}
			else if( controlID == radioButtonBuddiesID)
			{
				winInGame->winHide(TRUE);
				winBuddies->winHide(FALSE);
			}

			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (controlID == buttonMuteID[i] && slotNumInRow[i] >= 0)
				{
					TheGameInfo->getSlot(slotNumInRow[i])->mute(TRUE);
					PopulateInGameDiplomacyPopup();
					break;
				}
				if (controlID == buttonUnMuteID[i] && slotNumInRow[i] >= 0)
				{
					TheGameInfo->getSlot(slotNumInRow[i])->mute(FALSE);
					PopulateInGameDiplomacyPopup();
					break;
				}
			}
			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

void PopulateInGameDiplomacyPopup()
{
	if (!TheGameInfo)
		return;

	Int rowNum = 0;
	for (Int slotNum=0; slotNum<MAX_SLOTS; ++slotNum)
	{
		const GameSlot *slot = TheGameInfo->getConstSlot(slotNum);
		if (slot && slot->isOccupied())
		{
			Bool isInGame = false;
			// Note - for skirmish, TheNetwork == nullptr.  jba.
			if (TheNetwork &&	TheNetwork->isPlayerConnected(slotNum)) {
				isInGame = true;
			} else if ((TheNetwork == nullptr) && slot->isHuman()) {
				// this is a skirmish game and it is the human player.
				isInGame = true;
			}
			if (slot->isAI())
				isInGame = true;
			Player *player = ThePlayerList->getPlayerFromSlotIndex(slotNum);
			// GeneralsX: isPlayerObserver() below was always an unguarded
			// deref (docs/WORKDIR/PLAYER_CEILING_SWEEP_FINDINGS.md:334); the slot-index lookup makes
			// a miss return nullptr rather than whatever the name lookup happened to find.
			if (!player)
				continue;
			Bool isAlive = !TheVictoryConditions->hasSinglePlayerBeenDefeated(player);
			Bool isObserver = player->isPlayerObserver();

			if (slot->isHuman() && TheGameInfo->getLocalSlotNum() != slotNum && isInGame)
			{
				// show mute button
				if (buttonMute[rowNum])
				{
					buttonMute[rowNum]->winHide(slot->isMuted());
				}
				if (buttonUnMute[rowNum])
				{
					buttonUnMute[rowNum]->winHide(!slot->isMuted());
				}
			}
			else
			{
				// can't mute self, AI players, or MIA humans
				if (buttonMute[rowNum])
					buttonMute[rowNum]->winHide(TRUE);
				if (buttonUnMute[rowNum])
					buttonUnMute[rowNum]->winHide(TRUE);
			}

			Color playerColor = gxPlayerTextColor( TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor() );	// GeneralsX: text only, near-black lift
			Color backColor = GameMakeColor(0, 0, 0, 255);
			Color aliveColor = GameMakeColor(0, 255, 0, 255);
			Color deadColor = GameMakeColor(255, 0, 0, 255);
			Color observerInGameColor = GameMakeColor(255, 255, 255, 255);
			Color goneColor = GameMakeColor(196, 0, 0, 255);
			Color observerGoneColor = GameMakeColor(196, 196, 196, 255);

			if (staticTextPlayer[rowNum])
			{
				staticTextPlayer[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				GadgetStaticTextSetText(staticTextPlayer[rowNum], slot->getName());
			}
			if (staticTextSide[rowNum])
			{
				staticTextSide[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				GadgetStaticTextSetText(staticTextSide[rowNum], slot->getApparentPlayerTemplateDisplayName() );
			}
			if (staticTextTeam[rowNum])
			{
				staticTextTeam[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				AsciiString teamStr;
				teamStr.format("Team:%d", slot->getTeamNumber() + 1);
				if (slot->isAI() && slot->getTeamNumber() == -1)
					teamStr = "Team:AI";
				GadgetStaticTextSetText(staticTextTeam[rowNum], TheGameText->fetch(teamStr) );
			}
			if (staticTextStatus[rowNum])
			{
				UnicodeString statusText;

				staticTextStatus[rowNum]->winHide(FALSE);
				if (isInGame)
				{
					if (isAlive)
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( aliveColor, backColor );
						statusText = TheGameText->fetch("GUI:PlayerAlive");
					}
					else
					{
						if (isObserver)
						{
							staticTextStatus[rowNum]->winSetEnabledTextColors( observerInGameColor, backColor );
							statusText = TheGameText->fetch("GUI:PlayerObserver");
						}
						else
						{
							staticTextStatus[rowNum]->winSetEnabledTextColors( deadColor, backColor );
							statusText = TheGameText->fetch("GUI:PlayerDead");
						}
					}
				}
				else
				{
					// not in game
					if (isObserver)
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( observerGoneColor, backColor );
						statusText = TheGameText->fetch("GUI:PlayerObserverGone");
					}
					else
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( goneColor, backColor );
						statusText = TheGameText->fetch("GUI:PlayerGone");
					}
				}

				// GeneralsX: PING PER PLAYER, on the screen the player
				// already opens with TAB (CommandMap.ini binds DIPLOMACY to KEY_TAB in GAME, and
				// MSG_META_DIPLOMACY calls ToggleDiplomacy - verified by extracting
				// Data\English\CommandMap.ini from EnglishZH.big, so no retail key is rebound).
				//
				// APPENDED TO THE STATUS CELL RATHER THAN GIVEN ITS OWN COLUMN, deliberately.
				// Diplomacy.wnd authors no ping column, and CodeGuiRowsEnsure grows an EXISTING
				// column - it cannot conjure a new one - so a real column would mean placing
				// widgets at coordinates chosen without being able to see the screen. That is the
				// exact failure this area has already produced (a cloned row landing at the wrong
				// x). Reusing a cell that already exists inherits the authored geometry, works
				// unchanged on the rows CodeGuiRowsEnsure clones for slots 8..11, and needs no
				// .wnd edit - which also means no dataID move, so this ships with the engine.
				//
				// Status is the right cell rather than a lucky one: its longest strings ("gone",
				// "observer gone") occur exactly when there is no live figure to show, so the
				// number and the long words are never competing for the same width.
				//
				// A DASH IS NOT A ZERO. -1 comes back for every honest "I do not know" - an AI or
				// empty slot, a router that has not published yet, a lost report, or a peer that
				// went quiet and aged out - and every one of them must read as "--ms" rather than
				// as a perfect connection. This readout has been fixed four times for printing a
				// confident number it could not support.
				if (TheNetwork != nullptr && slot->isHuman())
				{
					Int pingMs = TheNetwork->getPeerPingMs(slotNum);

					// GeneralsX: THE HOST USED TO SEE "--ms" FOR ITSELF
					// while every client saw a figure for its own row, which reads as a broken
					// readout on exactly one machine in the lobby.
					//
					// It is not a missing measurement, it is an absent one. This column means "your
					// round trip to the packet router", and the router has no round trip to itself:
					// publishLatencyReport (ConnectionManager.cpp:827-829) deliberately leaves the
					// router's own slot at 0, and 0 is the "no figure" sentinel everywhere in that
					// path. A client's own row works because a client really does measure its leg
					// to the router.
					//
					// So the fix belongs HERE and not in the network layer. Storing a zero upstream
					// would collide with the sentinel and make "the router knows nothing about this
					// slot" indistinguishable from "this slot is the router". Rendering it here
					// states the one case the sentinel cannot: your latency to the host, when you
					// ARE the host, is zero. That is a definition rather than a measurement, which
					// is why it is the only place a confident 0 is honest - every other zero in
					// this readout still renders as a dash.
					// GeneralsX: AND EVERY CLIENT USED TO SEE "--ms" FOR
					// THE HOST, which is the same absence seen from the other side.
					//
					// The fix above only covered "I am the router, looking at my own row". A client
					// looking at the ROUTER's row hits the identical sentinel for the identical
					// reason - the router publishes its own slot as 0 because it has no round trip
					// to itself - so the one row every player most wants a figure for was the one
					// row nobody ever got one for.
					//
					// A client does not need the router to measure itself, because the leg is
					// symmetric and the client already measured it: "my ping to the host" and "the
					// host's row, as seen by me" are the same wire. So render the client's own
					// measurement in the router's row. It is the same number the router publishes
					// back as that client's own row, which is why the two rows now agree instead of
					// one of them being a dash.
					//
					// Naming the router's slot is what needed getPacketRouterSlot() on
					// NetworkInterface; isPacketRouter() can only answer for the local machine.
					if (pingMs < 0)
					{
						const Int routerSlot = TheNetwork->getPacketRouterSlot();
						const Int localSlot  = (Int)TheNetwork->getLocalPlayerID();
						if (routerSlot >= 0 && slotNum == routerSlot)
						{
							// Your latency to the host, when you ARE the host, is zero. A
							// definition rather than a measurement, and the only confident 0 this
							// readout is entitled to - every other zero still renders as a dash.
							pingMs = (slotNum == localSlot) ? 0 : TheNetwork->getPeerPingMs(localSlot);
						}
					}

					UnicodeString pingText;
					if (pingMs < 0)
						pingText.format(L"  --ms");
					else
						pingText.format(L"  %dms", pingMs);
					statusText.concat(pingText);
				}

				GadgetStaticTextSetText(staticTextStatus[rowNum], statusText);
			}

			slotNumInRow[rowNum++] = slotNum;
		}
	}

	while (rowNum < MAX_SLOTS)
	{
		slotNumInRow[rowNum] = -1;
		if (staticTextPlayer[rowNum])
			staticTextPlayer[rowNum]->winHide(TRUE);
		if (staticTextSide[rowNum])
			staticTextSide[rowNum]->winHide(TRUE);
		if (staticTextTeam[rowNum])
			staticTextTeam[rowNum]->winHide(TRUE);
		if (staticTextStatus[rowNum])
			staticTextStatus[rowNum]->winHide(TRUE);
		if (buttonMute[rowNum])
			buttonMute[rowNum]->winHide(TRUE);
		if (buttonUnMute[rowNum])
			buttonUnMute[rowNum]->winHide(TRUE);

		++rowNum;
	}
}



