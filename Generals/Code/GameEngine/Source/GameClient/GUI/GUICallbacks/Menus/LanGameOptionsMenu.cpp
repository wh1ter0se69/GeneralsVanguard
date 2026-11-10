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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LanGameOptionsMenu.cpp
// Author: Chris Huybregts, October 2001
// Description: Lan Game Options Menu
///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine


#include "Common/PlayerTemplate.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/UserPreferences.h"
#include "Common/QuotedPrintable.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/GameWindowTransitions.h"

#include "GameNetwork/FirewallHelper.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/GameText.h"
#include "GameNetwork/GUIUtil.h"


extern char *LANnextScreen;
extern Bool LANisShuttingDown;
extern Bool LANbuttonPushed;
extern void MapSelectorTooltip(GameWindow *window, WinInstanceData *instData,	UnsignedInt mouse);
extern void gameAcceptTooltip(GameWindow *window, WinInstanceData *instData, UnsignedInt mouse);
Color white = GameMakeColor( 255, 255, 255, 255 );
static bool s_isIniting = FALSE;
// window ids ------------------------------------------------------------------------------
static NameKeyType parentLanGameOptionsID = NAMEKEY_INVALID;

// GeneralsX: The shipped .wnd layouts carry exactly this many slot
// rows. MAX_SLOTS is now larger; lookups for rows past this legitimately return null until the
// dynamic-row lobby rebuild lands, so the per-row asserts below tolerate them.
enum { LEGACY_WND_SLOT_ROWS = 8 };

static NameKeyType comboBoxPlayerID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonAcceptID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxColorID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxPlayerTemplateID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxTeamID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

//static NameKeyType buttonStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType textEntryMapDisplayID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonStartID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType buttonSelectMapID = NAMEKEY_INVALID;
static NameKeyType windowMapID = NAMEKEY_INVALID;
// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentLanGameOptions = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonStart = nullptr;
static GameWindow *buttonSelectMap = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *textEntryMapDisplay = nullptr;
static GameWindow *windowMap = nullptr;

static GameWindow *comboBoxPlayer[MAX_SLOTS] = {0};
static GameWindow *buttonAccept[MAX_SLOTS] = {0};

static GameWindow *comboBoxColor[MAX_SLOTS] = {0};

static GameWindow *comboBoxPlayerTemplate[MAX_SLOTS] = {0};

static GameWindow *comboBoxTeam[MAX_SLOTS] = {0};

//static GameWindow *buttonStartPosition[MAX_SLOTS] = {0};
//
static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {0};

//external declarations of the Gadgets the callbacks can use
GameWindow *listboxChatWindowLanGame = nullptr;
NameKeyType listboxChatWindowLanGameID = NAMEKEY_INVALID;
WindowLayout *mapSelectLayout = nullptr;

static Int getNextSelectablePlayer(Int start)
{
	LANGameInfo *game = TheLAN->GetMyGame();
	if (!game->amIHost())
		return -1;
	for (Int j=start; j<MAX_SLOTS; ++j)
	{
		LANGameSlot *slot = game->getLANSlot(j);
		if (slot && slot->getStartPos() == -1 &&
			( (j==game->getLocalSlotNum() && game->getConstSlot(j)->getPlayerTemplate()!=PLAYERTEMPLATE_OBSERVER)
			|| slot->isAI()))
		{
			return j;
		}
	}
	return -1;
}

static Int getFirstSelectablePlayer(const GameInfo *game)
{
	const GameSlot *slot = game->getConstSlot(game->getLocalSlotNum());
	if (!game->amIHost() || (slot && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER))
		return game->getLocalSlotNum();

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		slot = game->getConstSlot(i);
		if (slot && slot->isAI())
			return i;
	}

	return game->getLocalSlotNum();
}

void updateMapStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], Bool onLoadScreen = FALSE );
void positionStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void LanPositionStartSpots()
{

	positionStartSpots( TheLAN->GetMyGame(), buttonMapStartPosition, windowMap);
}
static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int idx = -1;
	Int i=0;
	for (; i<MAX_SLOTS; ++i)
	{
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (window && comboBoxPlayer[i] && window == GadgetComboBoxGetEditBox(comboBoxPlayer[i]))
		{
			idx = i;
			break;
		}
	}
	if (idx == -1)
		return;

	// TheSuperHackers @bugfix Okladnoj 17/10/2026 Check that the game still exists as it is deleted while the setup screen fades out after the host left.
	LANGameInfo *myGame = TheLAN->GetMyGame();
	if (!myGame)
		return;

	LANGameSlot *slot = myGame->getLANSlot(i);
	if (!slot)
		return;

	LANPlayer *player = slot->getUser();
	if (!player)
	{
		DEBUG_ASSERTCRASH(myGame->getIP(i) == 0, ("No player info in listbox!"));
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}

	setLANPlayerTooltip(player);
}

void StartPressed()
{
	LANGameInfo *myGame = TheLAN->GetMyGame();

	Bool isReady = true;
	Bool allHaveMap = true;
	Int playerCount = 0;
	if (!myGame)
	{
		return;
	}
	myGame->getLANSlot(0)->setAccept(); // cause we are, of course!

	int i;

	int numUsers = 0;
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = myGame->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			numUsers++;
		}
	}

	// Check for too many players
	const MapMetaData *md = TheMapCache->findMap( myGame->getMap() );
	if (!md || md->m_numPlayers < numUsers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:TooManyPlayers"), (md)?md->m_numPlayers:0);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// GeneralsX: The map must also have room for every slot next to the
	// civilian sides it keeps (Common/PlayerCapacity.h). Refused here, never trimmed: owner decision.
	const Int slotCapacity = TheMapCache->getPlayerSlotCapacity( myGame->getMap() );
	if (slotCapacity >= 0 && myGame->countSlotsTakingPlayers() > slotCapacity)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:TooManyPlayers"), slotCapacity);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// GeneralsX: THE START RULE is GameInfo::getStartVerdict, one function for every
	// setup screen and inside the SimID digest. An all-AI lobby (humans only as observers) may start: retail
	// refused it here with GUI:NeedHumanPlayers. It starts as a sandbox, counted in nobody's stats (owner,
	// 27/09/2026), and is announced below.
	const GameInfo::StartVerdict startVerdict = myGame->getStartVerdict();

	// Check for too few players
	if (startVerdict == GameInfo::START_NOBODY_PLAYS || numUsers < TheGlobalData->m_netMinPlayers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:NeedMorePlayers"),numUsers);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// Check for too few teams
	int numRandom = 0;
	std::set<Int> teams;
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = myGame->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			if (slot->getTeamNumber() >= 0)
			{
				teams.insert(slot->getTeamNumber());
			}
			else
			{
				++numRandom;
			}
		}
	}
	if (numRandom + teams.size() < TheGlobalData->m_netMinPlayers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:NeedMoreTeams"));
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	if (startVerdict == GameInfo::START_SANDBOX)
	{
		UnicodeString text;
		text.format(TheGameText->fetchGameOnly("GUI:SandboxMode"));
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
	}

	// see if everyone's accepted and count the number of players in the game
	UnicodeString mapDisplayName;
	const MapMetaData *mapData = TheMapCache->findMap( myGame->getMap() );
	Bool willTransfer = TRUE;
	if (mapData)
	{
		mapDisplayName.format(L"%ls", mapData->m_displayName.str());
		willTransfer = !mapData->m_isOfficial;
	}
	else
	{
		// GeneralsX: The map's NAME, never its path (GeneralsMD mirror).
		mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(myGame->getMap()).str());
		willTransfer = WouldMapTransfer(myGame->getMap());
	}
	for( i = 0; i < MAX_SLOTS; i++ )
	{
		LANGameSlot *slot = myGame->getLANSlot(i);
		if( slot->isHuman() && !slot->isAccepted())
			isReady = false;
		// GeneralsX: EVERY human, accepted or not. Accepting never checks
		// the map, so an accepted slot without an official map passed here and the start ran into a transfer that cannot
		// happen (DoAnyMapTransfers refuses it). Since M1 2b that includes a slot whose map.ini/solo.ini differ from ours.
		if( slot->isHuman() && !willTransfer && !slot->hasMap())
		{
			UnicodeString msg;
			msg.format(TheGameText->fetchGameOnly("GUI:PlayerNoMap"), slot->getName().str(), mapDisplayName.str());
			GadgetListBoxAddEntryText(listboxChatWindowLanGame, msg , chatSystemColor, -1, 0);
			allHaveMap = false;
		}
		if( slot->isHuman() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER )
			playerCount++;
	}

	if(isReady && allHaveMap)
	{
		for( i = 0; i < MAX_SLOTS; i++ )
		{
			LANGameSlot *slot = myGame->getLANSlot(i);
			if (slot && slot->isOpen())
			{
				slot->setState( SLOT_CLOSED );
				GadgetComboBoxSetSelectedPos(comboBoxPlayer[i], SLOT_CLOSED);
			}
		}
		Int seconds = TheMultiplayerSettings->getStartCountdownTimerSeconds();
		if (seconds)
		{
			TheLAN->RequestGameStartTimer(seconds);
			// Disabled for the countdown; a transfer-only start re-enables it via
			// lanNotifyTransferOnlyStart when the lobby comes back.
			LANEnableStartButton(false);
		}
		else
		{
			// GeneralsX: (cross-model review) With no countdown,
			// RequestGameStart runs OnGameStart SYNCHRONOUSLY - and a transfer-only first start
			// has already restored the lobby by the time it returns, so disabling the button
			// here would lock the host out of the required second start. Only disable when the
			// game genuinely started.
			TheLAN->RequestGameStart();
			if (TheLAN->GetMyGame() != nullptr && TheLAN->GetMyGame()->isGameInProgress())
				LANEnableStartButton(false);
		}
	}
	else
	{
		// Does everyone have the map?
		if (allHaveMap)
		{
			GadgetListBoxAddEntryText(listboxChatWindowLanGame, TheGameText->fetch("GUI:NotifiedStartIntent") , chatSystemColor, -1, 0);
			TheLAN->RequestAccept();
		}
	}

}

// GeneralsX: Null-guard the widgets. Mirrors the GeneralsMD fix.
// These are only assigned by LanGameOptionsMenuInit, but the caller is
// LANGameInfo::resetAccepted() - a STATE operation on the slot list that LANAPI::OnPlayerJoin
// performs for every join. Any path that resets accepts without the lobby screen being up
// therefore dereferences a null window. Not reachable from the UI flow, where the widgets
// always exist, which is why it went unnoticed; it crashed a headless host in GeneralsMD the
// instant a peer joined.
void LANEnableStartButton(Bool enabled)
{
	if (buttonStart != nullptr)
		buttonStart->winEnable(enabled);
	if (buttonSelectMap != nullptr)
		buttonSelectMap->winEnable(enabled);
}

static void handleColorSelection(int index)
{
	GameWindow *combo = comboBoxColor[index];
	Int color, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	color = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));

	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (color == slot->getColor())
			return;

		if (color >= -1 && color < TheMultiplayerSettings->getNumColors())
		{
			Bool colorAvailable = TRUE;
			if(color != -1 )
			{
				for(Int i=0; i <MAX_SLOTS; i++)
				{
					LANGameSlot *checkSlot = myGame->getLANSlot(i);
					if(color == checkSlot->getColor() && slot != checkSlot)
					{
						colorAvailable = FALSE;
						break;
					}
				}
			}
			if(!colorAvailable)
				return;
		}

		slot->setColor(color);

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the color from the host
			if (!slot->isLocalPlayer() || !AreSlotListUpdatesEnabled())
				return;

			AsciiString options;
			options.format("Color=%d", color);
			TheLAN->RequestGameOptions(options, true);
		}
	}
}

static void handlePlayerTemplateSelection(int index)
{
	GameWindow *combo = comboBoxPlayerTemplate[index];
	Int playerTemplate, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	playerTemplate = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (playerTemplate == slot->getPlayerTemplate())
			return;

		Int oldTemplate = slot->getPlayerTemplate();
		slot->setPlayerTemplate(playerTemplate);

		if (oldTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// was observer, so populate color & team with all, and enable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
		}
		else if (playerTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// is becoming observer, so populate color & team with random only, and disable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
		}

		myGame->resetAccepted();

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the playerTemplate from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("PlayerTemplate=%d", playerTemplate);
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}

static void handleStartPositionSelection(Int player, int startPos)
{
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(player);
		if (startPos == slot->getStartPos())
			return;
		Bool skip = FALSE;
		if (startPos < 0)
		{
			skip = TRUE;
		}

		if(!skip)
		{
			Bool isAvailable = TRUE;
			for(Int i = 0; i < MAX_SLOTS; ++i)
			{
				if(i != player && myGame->getSlot(i)->getStartPos() == startPos)
				{
					isAvailable = FALSE;
					break;
				}
			}
			if( !isAvailable )
				return;
		}
		slot->setStartPos(startPos);

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				myGame->resetAccepted();
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the color from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("StartPos=%d", slot->getStartPos());
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}



static void handleTeamSelection(int index)
{
	GameWindow *combo = comboBoxTeam[index];
	Int team, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	team = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (team == slot->getTeamNumber())
			return;

		slot->setTeamNumber(team);
		myGame->resetAccepted();

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				myGame->resetAccepted();
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the team from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("Team=%d", team);
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}

void lanUpdateSlotList()
{
	if(!AreSlotListUpdatesEnabled() || s_isIniting)
		return;
	UpdateSlotList( TheLAN->GetMyGame(), comboBoxPlayer, comboBoxColor,
		comboBoxPlayerTemplate, comboBoxTeam, buttonAccept, buttonStart, buttonMapStartPosition);

	updateMapStartSpots(TheLAN->GetMyGame(), buttonMapStartPosition);
}

// GeneralsX: See the GeneralsMD twin for the full account. Generals
// has no start-phase machine; restoring the button flag and state and redrawing is the whole
// job. The explicit LANEnableStartButton(TRUE) matters for the countdown path, which disabled
// the button when the countdown armed (cross-model review finding).
void lanNotifyTransferOnlyStart( void )
{
	LANbuttonPushed = FALSE;
	LANEnableStartButton(TRUE);
	lanUpdateSlotList();
	updateGameOptions();
}

//-------------------------------------------------------------------------------------------------
/** Initialize the Gadgets Options Menu */
//-------------------------------------------------------------------------------------------------
void InitLanGameGadgets()
{
	//Initialize the gadget IDs
	parentLanGameOptionsID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:LanGameOptionsMenuParent" );
	buttonBackID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonBack" );
	buttonStartID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonStart" );
	textEntryChatID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:TextEntryChat" );
	textEntryMapDisplayID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:TextEntryMapDisplay" );
	listboxChatWindowLanGameID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ListboxChatWindowLanGame" );
	buttonEmoteID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonEmote" );
	buttonSelectMapID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonSelectMap" );
	windowMapID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:MapWindow" );

	// Initialize the pointers to our gadgets
	parentLanGameOptions = TheWindowManager->winGetWindowFromId( nullptr, parentLanGameOptionsID );
	DEBUG_ASSERTCRASH(parentLanGameOptions, ("Could not find the parentLanGameOptions"));
	buttonEmote = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonEmoteID  );
	DEBUG_ASSERTCRASH(buttonEmote, ("Could not find the buttonEmote"));
	buttonSelectMap = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonSelectMapID  );
	DEBUG_ASSERTCRASH(buttonSelectMap, ("Could not find the buttonSelectMap"));
	buttonStart = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonStartID  );
	DEBUG_ASSERTCRASH(buttonStart, ("Could not find the buttonStart"));
	buttonBack = TheWindowManager->winGetWindowFromId( parentLanGameOptions,  buttonBackID);
	DEBUG_ASSERTCRASH(buttonBack, ("Could not find the buttonBack"));
	listboxChatWindowLanGame = TheWindowManager->winGetWindowFromId( parentLanGameOptions, listboxChatWindowLanGameID );
	DEBUG_ASSERTCRASH(listboxChatWindowLanGame, ("Could not find the listboxChatWindowLanGame"));
	textEntryChat = TheWindowManager->winGetWindowFromId( parentLanGameOptions, textEntryChatID );
	DEBUG_ASSERTCRASH(textEntryChat, ("Could not find the textEntryChat"));
	textEntryMapDisplay = TheWindowManager->winGetWindowFromId( parentLanGameOptions, textEntryMapDisplayID );
	DEBUG_ASSERTCRASH(textEntryMapDisplay, ("Could not find the textEntryMapDisplay"));

	windowMap = TheWindowManager->winGetWindowFromId( parentLanGameOptions,windowMapID  );
	DEBUG_ASSERTCRASH(windowMap, ("Could not find the LanGameOptionsMenu.wnd:MapWindow" ));

	Int localSlotNum = TheLAN->GetMyGame()->getLocalSlotNum();
	DEBUG_ASSERTCRASH(localSlotNum >= 0, ("Bad slot number!"));

	//Tooltip function is being set for techBuildings, and supplyDocks
	windowMap->winSetTooltipFunc(MapSelectorTooltip);

	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		AsciiString tmpString;
		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxPlayer%d", i);
		comboBoxPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayer[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxPlayerID[i] );
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (comboBoxPlayer[i])
		{
			GadgetComboBoxReset(comboBoxPlayer[i]);
			GadgetComboBoxGetEditBox(comboBoxPlayer[i])->winSetTooltipFunc(playerTooltip);

			if(localSlotNum != i)
			{
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Open"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Closed"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:EasyAI"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:MediumAI"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:HardAI"),white);
				GadgetComboBoxSetSelectedPos(comboBoxPlayer[i],0);
			}
		}
		/*
		if(i != 0)
		{
			TheLAN->GetMyGame()->getLANSlot(i)->setState(SLOT_OPEN);
		}
		*/

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxColor%d", i);
		comboBoxColorID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxColor[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxColorID[i] );
		DEBUG_ASSERTCRASH(comboBoxColor[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxColor[%d]",i ));
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands (PopulateColorComboBox calls GadgetComboBoxGetLength, which derefs)
		if (comboBoxColor[i])
		{
			PopulateColorComboBox(i, comboBoxColor, TheLAN->GetMyGame());
			GadgetComboBoxSetSelectedPos(comboBoxColor[i], 0);
		}

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d", i);
		comboBoxPlayerTemplateID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayerTemplate[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxPlayerTemplateID[i] );
		DEBUG_ASSERTCRASH(comboBoxPlayerTemplate[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxPlayerTemplate[%d]",i ));
		PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, TheLAN->GetMyGame(), TRUE);

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxTeam%d", i);
		comboBoxTeamID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxTeam[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxTeamID[i] );
		DEBUG_ASSERTCRASH(comboBoxTeam[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxTeam[%d]",i ));
		PopulateTeamComboBox(i, comboBoxTeam, TheLAN->GetMyGame());

		tmpString.clear();
		tmpString.format("LanGameOptionsMenu.wnd:ButtonAccept%d", i);
		buttonAcceptID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonAccept[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonAcceptID[i] );
		DEBUG_ASSERTCRASH(buttonAccept[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the buttonAccept[%d]",i ));
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (buttonAccept[i])
			buttonAccept[i]->winSetTooltipFunc(gameAcceptTooltip);
//
//		tmpString.format("LanGameOptionsMenu.wnd:ButtonStartPosition%d", i);
//		buttonStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
//		buttonStartPosition[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonStartPositionID[i] );
//		DEBUG_ASSERTCRASH(buttonStartPosition[i], ("Could not find the ButtonStartPosition[%d]",i ));

		tmpString.format("LanGameOptionsMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonMapStartPositionID[i] );
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the ButtonMapStartPosition[%d]",i ));

		if(i !=0 && buttonAccept[i])
			buttonAccept[i]->winHide(TRUE);
	}
	if( buttonAccept[0] )
		GadgetButtonSetEnabledColor(buttonAccept[0], acceptTrueColor );

}

void DeinitLanGameGadgets()
{
	parentLanGameOptions = nullptr;
	buttonEmote = nullptr;
	buttonSelectMap = nullptr;
	buttonStart = nullptr;
	buttonBack = nullptr;
	listboxChatWindowLanGame = nullptr;
	textEntryChat = nullptr;
	textEntryMapDisplay = nullptr;
	if (windowMap)
	{
		windowMap->winSetUserData(nullptr);
		windowMap = nullptr;
	}
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		comboBoxPlayer[i] = nullptr;
		comboBoxColor[i] = nullptr;
		comboBoxPlayerTemplate[i] = nullptr;
		comboBoxTeam[i] = nullptr;
		buttonAccept[i] = nullptr;
//		buttonStartPosition[i] = nullptr;
		buttonMapStartPosition[i] = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the Lan Game Options Menu */
//-------------------------------------------------------------------------------------------------
void LanGameOptionsMenuInit( WindowLayout *layout, void *userData )
{
	if (TheLAN->GetMyGame() && TheLAN->GetMyGame()->isGameInProgress())
	{
		// If we init while the game is in progress, we are really returning to the menu
		// after the game.  So, we pop the menu and go back to the lobby.  Whee!
		DEBUG_LOG(("Popping to lobby after a game!"));
		TheShell->popImmediate();
		return;
	}
	s_isIniting = TRUE;

	LANbuttonPushed = false;
	LANisShuttingDown = false;

	//initialize the gadgets
	EnableSlotListUpdates(FALSE);
	InitLanGameGadgets();
	EnableSlotListUpdates(TRUE);
	Int start = 0;

	// Make sure the text fields are clear
	GadgetListBoxReset( listboxChatWindowLanGame );
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	//The dialog needs to react differently depending on whether it's the host or not.
	TheMapCache->updateCache();
	if (TheLAN->AmIHost())
	{
		// read in some prefs
		LANGameInfo *game = TheLAN->GetMyGame();
		LANGameSlot *slot = game->getLANSlot(0);
		LANPreferences pref;
		slot->setColor( pref.getPreferredColor() );
		slot->setPlayerTemplate( pref.getPreferredFaction() );
		slot->setNATBehavior(FirewallHelperClass::FIREWALL_TYPE_SIMPLE);
		game->setMap( pref.getPreferredMap() );
		AsciiString lowerMap = pref.getPreferredMap();
		lowerMap.toLower();
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(lowerMap);
		if (it != TheMapCache->end())
		{
			TheLAN->GetMyGame()->getSlot(0)->setMapAvailability(true);
			TheLAN->GetMyGame()->setMapCRC( it->second.m_CRC );
			TheLAN->GetMyGame()->setMapSize( it->second.m_filesize );

			TheLAN->GetMyGame()->adjustSlotsForMap(); // BGC- adjust the slots for the selected map.
		}

		//GadgetTextEntrySetText(comboBoxPlayer[0], TheLAN->GetMyName());
		lanUpdateSlotList();
		updateGameOptions();
		start = 1; // leave my combo boxes usable

		// TheSuperHackers @tweak disable the combo box for the host's player name
		comboBoxPlayer[0]->winEnable(FALSE);
	}
	else
	{

		//DEBUG_LOG(("LanGameOptionsMenuInit(): map is %s", TheLAN->GetMyGame()->getMap().str()));
		buttonStart->winSetText(TheGameText->fetch("GUI:Accept"));
		buttonSelectMap->winEnable( FALSE );
		TheLAN->GetMyGame()->setMapCRC( TheLAN->GetMyGame()->getMapCRC() );		// force a recheck
		TheLAN->GetMyGame()->setMapSize( TheLAN->GetMyGame()->getMapSize() ); // of if we have the map
		TheLAN->RequestHasMap();
		lanUpdateSlotList();
		updateGameOptions();
	}
	for (Int i = start; i < MAX_SLOTS; ++i)
	{
		//I'm a client, disable the controls I can't touch.
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (!TheLAN->AmIHost() && comboBoxPlayer[i])
			comboBoxPlayer[i]->winEnable(FALSE);

		if (comboBoxColor[i])
			comboBoxColor[i]->winEnable(FALSE);
		if (comboBoxPlayerTemplate[i])
			comboBoxPlayerTemplate[i]->winEnable(FALSE);
		if (comboBoxTeam[i])
			comboBoxTeam[i]->winEnable(FALSE);
//		buttonStartPosition[i]->winEnable(FALSE);
	}

//	for (i = 0; i < MAX_SLOTS; ++i)
//	{
//		if (buttonStartPosition[i])
//			buttonStartPosition[i]->winHide(TRUE); // not picking start spots this way any more
//	}
//
	// Show the Menu
	layout->hide( FALSE );

	// Set Keyboard to Main Parent
	TheWindowManager->winSetFocus( parentLanGameOptions );

	s_isIniting = FALSE;

	if (TheLAN->AmIHost())
	{
		TheLAN->RequestGameOptions(GenerateGameOptionsString(),true);
		TheLAN->RequestGameAnnounce();
	}
	lanUpdateSlotList();
	LanPositionStartSpots();
	TheTransitionHandler->setGroup("LanGameOptionsFade");

	// animate controls
	//TheShell->registerWithAnimateManager(buttonBack, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);

}

//-------------------------------------------------------------------------------------------------
/** Update options on screen */
//-------------------------------------------------------------------------------------------------
void updateGameOptions()
{
	LANGameInfo *theGame = TheLAN->GetMyGame();
	UnicodeString mapDisplayName;
	if (theGame && AreSlotListUpdatesEnabled())
	{
		const GameSlot *localSlot = theGame->getConstSlot(theGame->getLocalSlotNum());
		const MapMetaData *mapData = TheMapCache->findMap( TheLAN->GetMyGame()->getMap() );
		if (mapData && localSlot && localSlot->hasMap())
		{
			mapDisplayName.format(L"%ls", mapData->m_displayName.str());
		}
		else
		{
			// GeneralsX: GeneralsMD mirror: the old leaf hack only knew
			// backslashes, so a POSIX joiner's preview label showed the map's full path.
			mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(TheLAN->GetMyGame()->getMap()).str());
		}
		UnicodeString old = GadgetStaticTextGetText(textEntryMapDisplay);
		if(old.compare(mapDisplayName) != 0)
			LanPositionStartSpots();
		GadgetStaticTextSetText(textEntryMapDisplay, mapDisplayName);
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void setLANPlayerTooltip(LANPlayer* player)
{
	UnicodeString tooltip;

	if (!player->getLogin().isEmpty() || !player->getHost().isEmpty())
	{
		tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:LANPlayer"), player->getLogin().str(), player->getHost().str());
	}

#if defined(RTS_DEBUG)
	UnicodeString ip;
	ip.format(L" - %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(player->getIP()));
	tooltip.concat(ip);
#endif

	if (!tooltip.isEmpty())
	{
		TheMouse->setCursorTooltip( tooltip );
	}
}


//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{
	DeinitLanGameGadgets();
	textEntryMapDisplay = nullptr;
	LANisShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (LANnextScreen != nullptr) );

	if (LANnextScreen != nullptr)
	{
		TheShell->push(LANnextScreen);
	}

	LANnextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu shutdown method */
//-------------------------------------------------------------------------------------------------
void LanGameOptionsMenuShutdown( WindowLayout *layout, void *userData )
{
	TheMouse->setCursor(Mouse::ARROW);
	TheMouse->setMouseText(UnicodeString::TheEmptyString,nullptr,nullptr);
	EnableSlotListUpdates(FALSE);
	LANisShuttingDown = true;

	// GeneralsX: Backport of the Zero Hour twin (GeneralsMD's
	// LanGameOptionsMenuShutdown carries the full account). mapSelectLayout (:146) is a top-level
	// layout the shell does not own, so Shell::doPop's destroyWindows() on this screen never reaches
	// it, and only the Back/OK button handlers destroyed it. The pop-driven exits - OnHostLeave and
	// OnPlayerLeave in Core's LANAPICallbacks.cpp, and the popImmediate in LanGameOptionsMenuInit
	// (:807) - would leave its whole window tree drawn and hit-tested for the rest of the process.
	// Above the popImmediate early return so it runs on both branches: the two pop() exits take the
	// animated one (Shell::pop passes immediatePop FALSE), only the post-match popImmediate returns
	// early. Defensive - no repro, and no known path reaches these exits with the selector open.
	// Same four lines as WOLGameSetupMenuShutdown.
	if( mapSelectLayout )
	{
		mapSelectLayout->destroyWindows();
		deleteInstance(mapSelectLayout);
		mapSelectLayout = nullptr;
	}

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	TheTransitionHandler->reverse("LanGameOptionsFade");
	if (TheLAN)
		TheLAN->ResetGameStartTimer();

	/*
	// hide menu
	layout->hide( TRUE );

	// Reset the LAN singleton
//	TheLAN->reset();

	// our shutdown is complete
	TheShell->shutdownComplete( layout );
	*/
}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu update method */
//-------------------------------------------------------------------------------------------------
void LanGameOptionsMenuUpdate( WindowLayout * layout, void *userData)
{
	if(LANisShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);
	//TheLAN->update(); // this is handled in the lobby
}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanGameOptionsMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (LANbuttonPushed)
				break;

			switch( key )
			{
				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonBack, buttonBackID );
					}
					// don't let key fall through anywhere else
					return MSG_HANDLED;
				}
			}
		}
	}
	return MSG_IGNORED;
}


//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu window system callback */
//-------------------------------------------------------------------------------------------------

// GeneralsX: The Generals twin's seam into the shared lobby command
// table. The Zero Hour file has carried a gxHandleLobbySlashCommand since 07/08/2026; this side
// never had one, so /help, /clear and the retired-verb answers were unreachable here.
//
// /nick is deliberately absent: GXApplyNickname is defined in GeneralsMD/.../WOLLobbyMenu.cpp and
// declared in the ZERO HOUR GUICallbacks.h, so there is nothing here to call. The shared table
// gates that row on RTS_ZEROHOUR for the same reason, which is what keeps /help honest on this
// side rather than advertising a command that would fall through and be broadcast.
//
// Two passes, and the order matters. GXLobbyChatCommand runs FIRST because the verbs it retires
// (/pass, /nopassword, /lobbyhelp) are still recognised DOWNSTREAM by LANAPI::applyLobbyCommand,
// which this branch deliberately does not edit - LANAPI.cpp is inside the SimID source digest.
// GXLobbyGuardUnknownCommand runs LAST and lets /password, /private and /public through to
// RequestChat while refusing anything else that begins with '/'.
static void lanGameSay( const UnicodeString &line, Color color )
{
	// A headless run has no listbox, and silently dropping the line is what made the lobby
	// untestable without a GUI. [GXLOBBY] is what a harness greps for.
	if (listboxChatWindowLanGame == NULL)
	{
		AsciiString plain;
		plain.translate(line);
		fprintf(stderr, "[GXLOBBY] %s\n", plain.str());
		fflush(stderr);
		return;
	}
	GadgetListBoxAddEntryText(listboxChatWindowLanGame, line, color, -1, 0);
}

/// TRUE when the line was consumed and must NOT be sent. txtInput is non-const because a "//"
/// escape rewrites the line in place.
static Bool gxHandleLobbySlashCommand( UnicodeString &txtInput )
{
	AsciiString ascii;
	ascii.translate(txtInput);

	if (GXLobbyChatCommand(ascii, GX_LOBBY_SCREEN_GAME, listboxChatWindowLanGame, lanGameSay))
		return TRUE;

	Bool escaped = FALSE;
	if (GXLobbyGuardUnknownCommand(ascii, GX_LOBBY_SCREEN_GAME, lanGameSay, escaped))
		return TRUE;

	// Only the // escape may change what is sent, and then only by one leading character. Trimmed
	// on the WIDE string, never re-derived from `ascii`: RequestChat puts these wide characters
	// straight on the wire and AsciiString::translate is 7-bit only (its own @todo), so an
	// unconditional round trip would mangle every non-ASCII line typed on this screen.
	if (escaped)
		txtInput = UnicodeString(txtInput.str() + 1);
	return FALSE;
}

WindowMsgHandledType LanGameOptionsMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	switch( msg )
	{
		//-------------------------------------------------------------------------------------------------
		case GWM_CREATE:
			{
				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				if (windowMap)
					windowMap->winSetUserData(nullptr);

				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}
		//-------------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{

				// GeneralsX: Idea of upstream PR #2676: at match start hideShell()
				// runs DeinitLanGameGadgets(), which nulls every gadget pointer, but the layout is only
				// hidden - a mouse-down held across the start still delivers its mouse-up to the grab
				// window (hiding does not clear m_grabWindow), and ESC sends GBM_SELECTED with the
				// nulled buttonBack. parentLanGameOptions is null exactly while the gadgets are, so
				// every gadget case below drops the message then.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				LANGameInfo *myGame = TheLAN->GetMyGame();
				for (Int i = 0; i < MAX_SLOTS; i++)
				{
					if (controlID == comboBoxColorID[i])
					{
						handleColorSelection(i);
					}
					else if (controlID == comboBoxPlayerTemplateID[i])
					{
						handlePlayerTemplateSelection(i);
					}
					else if (controlID == comboBoxTeamID[i])
					{
						handleTeamSelection(i);
					}
					else if( controlID == comboBoxPlayerID[i] && myGame->amIHost() )
					{
						// We don't have anything that'll happen if we click on ourselves
						if(i == myGame->getLocalSlotNum())
						 break;
						// Get
						Int pos = -1;
						GadgetComboBoxGetSelectedPos(comboBoxPlayer[i], &pos);
						if( pos != SLOT_PLAYER && pos >= 0)
						{
							if( myGame->getLANSlot(i)->getState() == SLOT_PLAYER )
							{
								UnicodeString name = myGame->getPlayerName(i);
								myGame->getLANSlot(i)->setState(SlotState(pos));
								myGame->resetAccepted();
								TheLAN->OnPlayerLeave(name);
							}
							else if( myGame->getLANSlot(i)->getState() != pos )
							{
								Bool wasAI = (myGame->getLANSlot(i)->isAI());
								myGame->getLANSlot(i)->setState(SlotState(pos));
								Bool isAI = (myGame->getLANSlot(i)->isAI());
								if (wasAI || isAI)
									myGame->resetAccepted();
								if (wasAI ^ isAI)
									PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, myGame, wasAI);
								if (!s_isIniting)
								{
									TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
									lanUpdateSlotList();
								}
							}
						}
						break;
					}
				}
			}
		//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				// GeneralsX: Same guard as GCM_SELECTED above.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if ( controlID == buttonBackID )
				{
					if( mapSelectLayout )
						{
							mapSelectLayout->destroyWindows();
							deleteInstance(mapSelectLayout);
							mapSelectLayout = nullptr;
						}
					TheLAN->RequestGameLeave();
					//TheShell->pop();

				}
				else if ( controlID == buttonEmoteID )
				{
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && !gxHandleLobbySlashCommand(txtInput))
						TheLAN->RequestPlayerChat(txtInput);
				}
				else if ( controlID == buttonSelectMapID )
				{
					//buttonBack->winEnable( false );

					mapSelectLayout = TheWindowManager->winCreateLayout( "Menus/LanMapSelectMenu.wnd" );
					mapSelectLayout->runInit();
					mapSelectLayout->hide( FALSE );
					mapSelectLayout->bringForward();

				}
				else if ( controlID == buttonStartID )
				{
					if (TheLAN->AmIHost())
					{
						StartPressed();
						//TheLAN->RequestGameStart();
					}
					else
					{
						//I'm the Client... send an accept message to the host.
						TheLAN->RequestAccept();

						// Disable the accept button
						EnableAcceptControls(TRUE, TheLAN->GetMyGame(), comboBoxPlayer, comboBoxColor, comboBoxPlayerTemplate,
							comboBoxTeam, buttonAccept, buttonStart, buttonMapStartPosition);

					}
				}
				else
				{
					for (Int i = 0; i < MAX_SLOTS; i++)
					{
						if (controlID == buttonMapStartPositionID[i])
						{
							LANGameInfo *game = TheLAN->GetMyGame();
							Int playerIdxInPos = -1;
							for (Int j=0; j<MAX_SLOTS; ++j)
							{
								LANGameSlot *slot = game->getLANSlot(j);
								if (slot && slot->getStartPos() == i)
								{
									playerIdxInPos = j;
									break;
								}
							}
							if (playerIdxInPos >= 0)
							{
								LANGameSlot *slot = game->getLANSlot(playerIdxInPos);
								if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
								{
									// it's one of my type.  Try to change it.
									Int nextPlayer = getNextSelectablePlayer(playerIdxInPos+1);
									handleStartPositionSelection(playerIdxInPos, -1);
									if (nextPlayer >= 0)
									{
										handleStartPositionSelection(nextPlayer, i);
									}
								}
							}
							else
							{
								// nobody in the slot - put us in
								Int nextPlayer = getNextSelectablePlayer(0);
								if (nextPlayer < 0)
									nextPlayer = getFirstSelectablePlayer(game);
								handleStartPositionSelection(nextPlayer, i);
							}
						}
					}
				}

				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED_RIGHT:
		{
			// GeneralsX: Same guard as GCM_SELECTED above.
			// Upstream reference: Caball009, PR #2676
			// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
			if (LANbuttonPushed || !parentLanGameOptions)
				break;

			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			for (Int i = 0; i < MAX_SLOTS; i++)
			{
				if (controlID == buttonMapStartPositionID[i])
				{
					LANGameInfo *game = TheLAN->GetMyGame();
					Int playerIdxInPos = -1;
					for (Int j=0; j<MAX_SLOTS; ++j)
					{
						LANGameSlot *slot = game->getLANSlot(j);
						if (slot && slot->getStartPos() == i)
						{
							playerIdxInPos = j;
							break;
						}
					}
					if (playerIdxInPos >= 0)
					{
						LANGameSlot *slot = game->getLANSlot(playerIdxInPos);
						if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
						{
							// it's one of my type.  Remove it.
							handleStartPositionSelection(playerIdxInPos, -1);
						}
					}
				}
			}
			break;
		}
		//-------------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				// GeneralsX: Same guard as GCM_SELECTED above.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				// Take the user's input and echo it into the chat window as well as
				// send it to the other clients on the lan
				if ( controlID == textEntryChatID )
				{

					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && !gxHandleLobbySlashCommand(txtInput))
						TheLAN->RequestPlayerChat(txtInput);

				}
				break;
			}
		//-------------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;
	}
	return MSG_HANDLED;
}

//-------------------------------------------------------------------------------------------------
/** Utility FUnction used as a bridge from other windows to this one */
//-------------------------------------------------------------------------------------------------
void PostToLanGameOptions( PostToLanGameType post )
{
	if (post >= POST_TO_LAN_GAME_TYPE_COUNT)
		return;
	LanPositionStartSpots();
	switch (post)
	{
			//-------------------------------------------------------------------------------------------------
		case SEND_GAME_OPTS:
		{
			LANGameInfo *game = TheLAN->GetMyGame();
			game->resetAccepted();
			updateGameOptions();
			lanUpdateSlotList();

			//buttonBack->winEnable( true );

			for(Int i = 0; i < MAX_SLOTS; ++i)
			{
				game->getSlot(i)->setStartPos(-1);
			}

			TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
			break;
		}
		case MAP_BACK:
		{
				//buttonBack->winEnable( true );
		}
		default:
			return;
	}
}
