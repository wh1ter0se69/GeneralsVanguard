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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LobbyUtils.cpp
// Author: Matthew D. Campbell, Sept 2002
// Description: GameSpy lobby utils
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdint>
#include "Common/GameEngine.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/version.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Image.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Mouse.h"
#include "GameNetwork/GameSpyOverlay.h"

#include "GameClient/LanguageFilter.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/LadderDefs.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/GSConfig.h"

#include "Common/STLTypedefs.h"


// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
// Note: if you add more columns, you must modify the .wnd files and change the listbox properties (yuck!)
enum {
	COLUMN_NAME = 0,
	COLUMN_MAP,
	COLUMN_LADDER,
	COLUMN_NUMPLAYERS,
	COLUMN_PASSWORD,
	COLUMN_OBSERVER,
#if !RTS_GENERALS
  COLUMN_USE_STATS,
#endif
	COLUMN_PING,
};

static NameKeyType buttonSortAlphaID = NAMEKEY_INVALID;
static NameKeyType buttonSortPingID = NAMEKEY_INVALID;
static NameKeyType buttonSortBuddiesID = NAMEKEY_INVALID;
static NameKeyType windowSortAlphaID = NAMEKEY_INVALID;
static NameKeyType windowSortPingID = NAMEKEY_INVALID;
static NameKeyType windowSortBuddiesID = NAMEKEY_INVALID;

static GameWindow *buttonSortAlpha = nullptr;
static GameWindow *buttonSortPing = nullptr;
static GameWindow *buttonSortBuddies = nullptr;
static GameWindow *windowSortAlpha = nullptr;
static GameWindow *windowSortPing = nullptr;
static GameWindow *windowSortBuddies = nullptr;

static GameSortType theGameSortType = GAMESORT_ALPHA_ASCENDING;
static Bool sortBuddies = TRUE;
static void showSortIcons()
{
	if (windowSortAlpha && windowSortPing)
	{
		switch(theGameSortType)
		{
			case GAMESORT_ALPHA_ASCENDING:
				windowSortAlpha->winHide(FALSE);
				windowSortAlpha->winEnable(TRUE);
				windowSortPing->winHide(TRUE);
				break;
			case GAMESORT_ALPHA_DESCENDING:
				windowSortAlpha->winHide(FALSE);
				windowSortAlpha->winEnable(FALSE);
				windowSortPing->winHide(TRUE);
				break;
			case GAMESORT_PING_ASCENDING:
				windowSortPing->winHide(FALSE);
				windowSortPing->winEnable(TRUE);
				windowSortAlpha->winHide(TRUE);
				break;
			case GAMESORT_PING_DESCENDING:
				windowSortPing->winHide(FALSE);
				windowSortPing->winEnable(FALSE);
				windowSortAlpha->winHide(TRUE);
				break;
		}
	}

	if (sortBuddies)
	{
		if (windowSortBuddies)
		{
			windowSortBuddies->winHide(FALSE);
		}
	}
	else
	{
		if (windowSortBuddies)
		{
			windowSortBuddies->winHide(TRUE);
		}
	}
}
void setSortMode( GameSortType sortType ) { theGameSortType = sortType; showSortIcons(); RefreshGameListBoxes(); }
void sortByBuddies( Bool doSort ) { sortBuddies = doSort; showSortIcons(); RefreshGameListBoxes(); }

Bool HandleSortButton( NameKeyType sortButton )
{
	if (sortButton == buttonSortBuddiesID)
	{
		sortByBuddies( !sortBuddies );
		return TRUE;
	}
	else if (sortButton == buttonSortAlphaID)
	{
		if (theGameSortType == GAMESORT_ALPHA_ASCENDING)
		{
			setSortMode(GAMESORT_ALPHA_DESCENDING);
		}
		else
		{
			setSortMode(GAMESORT_ALPHA_ASCENDING);
		}
		return TRUE;
	}
	else if (sortButton == buttonSortPingID)
	{
		if (theGameSortType == GAMESORT_PING_ASCENDING)
		{
			setSortMode(GAMESORT_PING_DESCENDING);
		}
		else
		{
			setSortMode(GAMESORT_PING_ASCENDING);
		}
		return TRUE;
	}
	return FALSE;
}

// window ids ------------------------------------------------------------------------------
static NameKeyType parentID = NAMEKEY_INVALID;
//static NameKeyType parentGameListSmallID = NAMEKEY_INVALID;
static NameKeyType parentGameListLargeID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyGamesSmallID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyGamesLargeID = NAMEKEY_INVALID;
//static NameKeyType listboxLobbyGameInfoID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parent = nullptr;
//static GameWindow *parentGameListSmall = nullptr;
static GameWindow *parentGameListLarge = nullptr;
       //GameWindow *listboxLobbyGamesSmall = nullptr;
       GameWindow *listboxLobbyGamesLarge = nullptr;
       //GameWindow *listboxLobbyGameInfo = nullptr;

static const Image *pingImages[3] = { nullptr, nullptr, nullptr };

static void gameTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	// GeneralsX @bugfix Claude 30/07/2026 A tooltip must not assume the GameSpy backend is alive.
	//
	// This function calls TheGameSpyInfo->findStagingRoomByID() below, and TheGameSpyInfo is
	// legitimately null for a whole run of frames on the ordinary join path:
	//
	//   Shell::push("Menus/LanGameOptionsMenu.wnd")
	//     -> WOLLobbyMenuShutdown (Shell::push shuts the current screen down first)
	//        -> TearDownGameSpy(), which does `delete TheGameSpyInfo; TheGameSpyInfo = nullptr`
	//   ... and then, because popImmediate is FALSE, the screen ANIMATES OUT over several more
	//   frames with ListboxGamesLarge still alive and still carrying this tooltip func.
	//
	// Any pointer motion over the list in that window went straight through a null vtable load.
	// Measured on the iPad, which hits it every single time: a tap on Join emits a move event at
	// the same coordinates, so the crash lands immediately after "recv Join Accept". SIGSEGV at
	// gameTooltip+188, `ldr x8, [x0]` with x0 = 0.
	//
	// Guarding here rather than unregistering the func in the shutdown: the screen is on its way
	// out, so having no tooltip is the correct behaviour, and a tooltip that silently does nothing
	// when there is no backend is right for every other caller too - including the relay-driven
	// lobby, which runs with no GameSpy session at all by design.
	if (TheGameSpyInfo == nullptr)
	{
		if (TheMouse != nullptr)
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}

	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);//TheGameText->fetch("TOOLTIP:GamesBeingFormed") );
		return;
	}

	Int gameID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(window, row, 0)));
	GameSpyStagingRoom *room = TheGameSpyInfo->findStagingRoomByID(gameID);
	if (!room)
	{
		TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UnknownGame") );
		return;
	}

	if (col == COLUMN_PING)
	{
		// GeneralsX @feature Claude 07/08/2026 Say what the dot actually measured.
		//
		// The stock tooltip is the fixed string "Connection speed quality to host", which tells a
		// player nothing they cannot see from the colour. We now have a real number and, more
		// usefully, we know which half of the path it came from - so show the breakdown. A player
		// whose own leg is the slow one can act on that; "quality to host" hides it.
		//
		// Both printed halves come from the SAME instant the total was formed - the host's leg is
		// the number the relay actually gave us, kept on the room, and our leg is the remainder of
		// that total. Reading TheLAN->getRelayRttMs() live here instead would subtract a fresh
		// sample from a frozen total and label the residue "Host to relay", which is arithmetic
		// dressed up as a measurement. Falls back to the stock string when nothing was measured.
		const Int totalMs = room->getPingAsInt();
		const Int hostLegMs = room->getHostPingMs();

		if (totalMs > 0 && hostLegMs > 0 && hostLegMs < totalMs)
		{
			const UnicodeString s = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("TOOLTIP:RelayPingInfo",
				L"Latency: %d ms\nYou to relay: %d ms\nHost to relay: %d ms",
				totalMs, totalMs - hostLegMs, hostLegMs);
			TheMouse->setCursorTooltip( s, 10, nullptr, 2.0f );
		}
		else
		{
			TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:PingInfo"), 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
		}
		return;
	}
	if (col == COLUMN_NUMPLAYERS)
	{
		TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:NumberOfPlayers"), 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
		return;
	}
	if (col == COLUMN_PASSWORD)
	{
		if (room->getHasPassword())
		{
			UnicodeString checkTooltip =TheGameText->fetch("TOOTIP:Password");
			if(!checkTooltip.compare(L"Password required to joing game"))
				checkTooltip.set(L"Password required to join game");
			TheMouse->setCursorTooltip( checkTooltip, 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
		}
		else
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}
#if !RTS_GENERALS
  if (col == COLUMN_USE_STATS)
  {
    if ( room->getUseStats() )
    {
      TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UseStatsOn") );
    }
    else
    {
      TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UseStatsOff") );
    }
    return;
  }
#endif

	UnicodeString tooltip;

	UnicodeString mapName;
	const MapMetaData *md = TheMapCache->findMap(room->getMap());
	if (md)
	{
		mapName = md->m_displayName;
	}
	else
	{
		const char *start = room->getMap().reverseFind('\\');
		if (start)
		{
			++start;
		}
		else
		{
			start = room->getMap().str();
		}
		mapName.translate( start );
	}
	UnicodeString tmp;
	tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:GameInfoGameName"), room->getGameName().str());
	if (room->getLadderPort() != 0)
	{
		const LadderInfo *linfo = TheLadderList->findLadder(room->getLadderIP(), room->getLadderPort());
		if (linfo)
		{
			tmp.format(TheGameText->fetchGameOnly("TOOLTIP:GameInfoLadderName"), linfo->name.str());
			tooltip.concat(tmp);
		}
	}
	if (room->getExeCRC() != TheGlobalData->m_exeCRC || room->getIniCRC() != TheGlobalData->m_iniCRC)
	{
		tmp.format(TheGameText->fetchGameOnly("TOOLTIP:InvalidGameVersion"), mapName.str());
		tooltip.concat(tmp);
	}
	tmp.format(TheGameText->fetchGameOnly("TOOLTIP:GameInfoMap"), mapName.str());
	tooltip.concat(tmp);

	AsciiString aPlayer;
	UnicodeString player;
	Int numPlayers = 0;
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		GameSpyGameSlot *slot = room->getGameSpySlot(i);
		if (i == 0 && (!slot || !slot->isHuman()))
		{
			DEBUG_CRASH(("About to tooltip a non-hosted game!"));
		}
		if (slot && slot->isHuman())
		{
			tmp.format(TheGameText->fetchGameOnly("TOOLTIP:GameInfoPlayer"), slot->getName().str(), slot->getWins(), slot->getLosses());
			tooltip.concat(tmp);
			++numPlayers;
		}
		else if (slot && slot->isAI())
		{
			++numPlayers;
			switch(slot->getState())
			{
			case SLOT_EASY_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:EasyAI"));
				break;
			case SLOT_MED_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:MediumAI"));
				break;
			case SLOT_BRUTAL_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:HardAI"));
				break;
			}
		}
	}
	DEBUG_ASSERTCRASH(numPlayers, ("Tooltipping a 0-player game!"));

	TheMouse->setCursorTooltip( tooltip, 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
}

// GeneralsX @bugfix Claude 07/08/2026 Re-proportion the game browser's columns.
//
// WOLCustomLobby.wnd ships COLUMNS: 8 with widths 48/30/0/6/6/0/4/6, and two of those are
// literally ZERO: COLUMN_LADDER and COLUMN_OBSERVER. A zero-width column still contributes a
// header label, so the header ran together as "Slots Key Re Ping" - and COLUMN_USE_STATS at 4%
// (~19px at the 800x600 creation size) clipped its own header to "Re". The ping cell then had to
// fit a quality dot AND up to four digits in 6%.
//
// Done here rather than by shipping a corrected .wnd. A loose Data/Window/Menus override does
// work (GeneralsZH/Data/Window/Menus/ExtrasMenu.wnd is the precedent, and .wnd files are not
// scanned by dataID so it would not refuse a join), but it would have to be copied by four
// separate deploy paths and could then drift per peer. Percentages live in ListboxData and are
// turned into pixels by a single formula, so setting them in code is one change that cannot get
// out of step.
//
// The recompute mirrors GadgetListBox.cpp's own GWM_CREATE arithmetic exactly, including
// subtracting the slider, because that is the value it will be re-derived from on any later
// resize.
void RelayFixGameListColumns( GameWindow *listbox )
{
	if( listbox == nullptr )
		return;

	ListboxData *list = (ListboxData *)listbox->winGetUserData();
	if( list == nullptr || list->columnWidth == nullptr || list->columnWidthPercentage == nullptr )
		return;

	// Only touch the layout this was measured against. A modded .wnd with a different column
	// count must keep whatever its author intended.
	if( list->columns != 8 )
		return;

	//                              name map ladder slots locked obs stats ping
	static const Int widths[8] = {   30,  28,     0,    10,     10,   0,    9,   13 };

	Int i;
	for( i = 0; i < 8; ++i )
		list->columnWidthPercentage[i] = widths[i];

	Int totalWidth, height;
	listbox->winGetSize( &totalWidth, &height );
	if( list->slider )
	{
		ICoord2D sliderSize;
		list->slider->winGetSize( &sliderSize.x, &sliderSize.y );
		totalWidth -= sliderSize.x;
	}

	for( i = 0; i < 8; ++i )
		list->columnWidth[i] = list->columnWidthPercentage[i] * totalWidth / 100;
}

static Bool isSmall = TRUE;

GameWindow *GetGameListBox()
{
	return listboxLobbyGamesLarge;
}

GameWindow *GetGameInfoListBox()
{
	return nullptr;
}

NameKeyType GetGameListBoxID()
{
	return listboxLobbyGamesLargeID;
}

NameKeyType GetGameInfoListBoxID()
{
	return NAMEKEY_INVALID;
}

void GrabWindowInfo()
{
	isSmall = TRUE;
	parentID = NAMEKEY( "WOLCustomLobby.wnd:WOLLobbyMenuParent" );
	parent = TheWindowManager->winGetWindowFromId(nullptr, parentID);

	pingImages[0] = TheMappedImageCollection->findImageByName("Ping03");
	pingImages[1] = TheMappedImageCollection->findImageByName("Ping02");
	pingImages[2] = TheMappedImageCollection->findImageByName("Ping01");
	DEBUG_ASSERTCRASH(pingImages[0], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[1], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[2], ("Can't find ping image!"));

//	parentGameListSmallID = NAMEKEY( "WOLCustomLobby.wnd:ParentGameListSmall" );
//	parentGameListSmall = TheWindowManager->winGetWindowFromId(nullptr, parentGameListSmallID);

	parentGameListLargeID = NAMEKEY( "WOLCustomLobby.wnd:ParentGameListLarge" );
	parentGameListLarge = TheWindowManager->winGetWindowFromId(nullptr, parentGameListLargeID);

	listboxLobbyGamesSmallID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGames" );
//	listboxLobbyGamesSmall = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGamesSmallID);
//	listboxLobbyGamesSmall->winSetTooltipFunc(gameTooltip);

	listboxLobbyGamesLargeID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGamesLarge" );
	listboxLobbyGamesLarge = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGamesLargeID);
	listboxLobbyGamesLarge->winSetTooltipFunc(gameTooltip);
	RelayFixGameListColumns(listboxLobbyGamesLarge);
//
//	listboxLobbyGameInfoID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGameInfo" );
//	listboxLobbyGameInfo = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGameInfoID);

	buttonSortAlphaID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortAlpha");
	buttonSortPingID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortPing");
	buttonSortBuddiesID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortBuddies");
	windowSortAlphaID = NAMEKEY("WOLCustomLobby.wnd:WindowSortAlpha");
	windowSortPingID = NAMEKEY("WOLCustomLobby.wnd:WindowSortPing");
	windowSortBuddiesID = NAMEKEY("WOLCustomLobby.wnd:WindowSortBuddies");

	buttonSortAlpha = TheWindowManager->winGetWindowFromId(parent, buttonSortAlphaID);
	buttonSortPing = TheWindowManager->winGetWindowFromId(parent, buttonSortPingID);
	buttonSortBuddies = TheWindowManager->winGetWindowFromId(parent, buttonSortBuddiesID);
	windowSortAlpha = TheWindowManager->winGetWindowFromId(parent, windowSortAlphaID);
	windowSortPing = TheWindowManager->winGetWindowFromId(parent, windowSortPingID);
	windowSortBuddies = TheWindowManager->winGetWindowFromId(parent, windowSortBuddiesID);

	showSortIcons();
}

void ReleaseWindowInfo()
{
	isSmall = TRUE;
	parent = nullptr;
//	parentGameListSmall = nullptr;
	parentGameListLarge = nullptr;
//	listboxLobbyGamesSmall = nullptr;
	listboxLobbyGamesLarge = nullptr;
//	listboxLobbyGameInfo = nullptr;

	buttonSortAlpha = nullptr;
	buttonSortPing = nullptr;
	buttonSortBuddies = nullptr;
	windowSortAlpha = nullptr;
	windowSortPing = nullptr;
	windowSortBuddies = nullptr;
}

typedef std::set<GameSpyStagingRoom *> BuddyGameSet;
static BuddyGameSet *theBuddyGames = nullptr;
static void populateBuddyGames()
{
	BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
	theBuddyGames = NEW BuddyGameSet;
	if (!m)
	{
		return;
	}
	for (BuddyInfoMap::const_iterator bit = m->begin(); bit != m->end(); ++bit)
	{
		BuddyInfo info = bit->second;
		if (info.m_status == GP_STAGING)
		{
			StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
			for (StagingRoomMap::iterator srmIt = srm->begin(); srmIt != srm->end(); ++srmIt)
			{
				GameSpyStagingRoom *game = srmIt->second;
				game->cleanUpSlotPointers();
				const GameSpyGameSlot *slot = game->getGameSpySlot(0);
				if (slot && slot->getName() == info.m_locationString)
				{
					theBuddyGames->insert(game);
					break;
				}
			}
		}
	}
}

static void clearBuddyGames()
{
	delete theBuddyGames;
	theBuddyGames = nullptr;
}

struct GameSortStruct
{
	bool operator()(GameSpyStagingRoom *g1, GameSpyStagingRoom *g2) const
	{
		// sort CRC mismatches to the bottom
		Bool g1Good = (g1->getExeCRC() == TheGlobalData->m_exeCRC && g1->getIniCRC() == TheGlobalData->m_iniCRC);
		Bool g2Good = (g2->getExeCRC() == TheGlobalData->m_exeCRC && g2->getIniCRC() == TheGlobalData->m_iniCRC);
		if ( g1Good ^ g2Good )
		{
			return g1Good;
		}

		// sort games with private ladders to the bottom
		Bool g1UnknownLadder = (g1->getLadderPort() && TheLadderList->findLadder(g1->getLadderIP(), g1->getLadderPort()) == nullptr);
		Bool g2UnknownLadder = (g2->getLadderPort() && TheLadderList->findLadder(g2->getLadderIP(), g2->getLadderPort()) == nullptr);
		if ( g1UnknownLadder ^ g2UnknownLadder )
		{
			return g2UnknownLadder;
		}

		// sort full games to the bottom
		Bool g1Full = (g1->getNumNonObserverPlayers() == g1->getMaxPlayers() || g1->getNumPlayers() == MAX_SLOTS);
		Bool g2Full = (g2->getNumNonObserverPlayers() == g2->getMaxPlayers() || g2->getNumPlayers() == MAX_SLOTS);
		if ( g1Full ^ g2Full )
		{
			return g2Full;
		}

		if (sortBuddies)
		{
			Bool g1HasBuddies = (theBuddyGames->find(g1) != theBuddyGames->end());
			Bool g2HasBuddies = (theBuddyGames->find(g2) != theBuddyGames->end());
			if ( g1HasBuddies ^ g2HasBuddies )
			{
				return g1HasBuddies;
			}
		}

		switch(theGameSortType)
		{
		case GAMESORT_ALPHA_ASCENDING:
			return wcsicmp(g1->getGameName().str(), g2->getGameName().str()) < 0;
			break;
		case GAMESORT_ALPHA_DESCENDING:
			return wcsicmp(g1->getGameName().str(),g2->getGameName().str()) > 0;
			break;
		// GeneralsX @bugfix Claude 07/08/2026 An UNMEASURED game must not sort as the fastest one.
		//
		// getPingAsInt() is 0 when there is no measurement, so a plain numeric compare put every
		// unmeasured game at the top of a fastest-first sort - the most prominent position in the
		// browser given to the rows we know least about. It did not arise before because the only
		// producer was GameSpy, whose "no data" value was getPingTimeoutInMs(), a large number that
		// already sorted last. setPingMs(0) reintroduced a real zero.
		//
		// Keyed on the ping STRING being empty, which is the same "no measurement exists" test
		// insertGame uses to leave the cell blank, so the sort and the display cannot disagree.
		// Unmeasured games sort last in BOTH directions: they are not fast, and they are not slow.
		case GAMESORT_PING_ASCENDING:
		case GAMESORT_PING_DESCENDING:
		{
			const Bool g1Unknown = g1->getPingString().isEmpty();
			const Bool g2Unknown = g2->getPingString().isEmpty();
			if (g1Unknown != g2Unknown)
				return g2Unknown;			// the one we DO know about comes first
			if (g1Unknown)
				return false;				// neither is measured - leave their order alone

			if (theGameSortType == GAMESORT_PING_ASCENDING)
				return g1->getPingAsInt() < g2->getPingAsInt();
			return g1->getPingAsInt() > g2->getPingAsInt();
		}
		}
		return false;
	}
};

static Int insertGame( GameWindow *win, GameSpyStagingRoom *game, Bool showMap )
{
	game->cleanUpSlotPointers();
	// GeneralsX @info Claude 06/08/2026 The row colour is a STATE and there are three of them. This
	// was undocumented and had to be inferred from the screen, which is why it is written down here
	// and shown to the player as a legend when the lobby opens:
	//   GSCOLOR_GAME          - joinable
	//   GSCOLOR_GAME_FULL     - no free slot
	//   GSCOLOR_GAME_CRCMISMATCH - a DIFFERENT BUILD. This is the one worth knowing: the row is not
	//                           unjoinable because it is busy, it is unjoinable because that host is
	//                           running other code and the join would be refused.
	Color gameColor = GameSpyColor[GSCOLOR_GAME];
	if (game->getNumNonObserverPlayers() == game->getMaxPlayers() || game->getNumPlayers() == MAX_SLOTS)
	{
		gameColor = GameSpyColor[GSCOLOR_GAME_FULL];
	}
	if (game->getExeCRC() != TheGlobalData->m_exeCRC || game->getIniCRC() != TheGlobalData->m_iniCRC)
	{
		gameColor = GameSpyColor[GSCOLOR_GAME_CRCMISMATCH];
	}
	UnicodeString gameName = game->getGameName();

	if(TheGameSpyInfo->getDisallowAsianText())
	{
		const WideChar *buff = gameName.str();
		Int length =  gameName.getLength();
		for(Int i = 0; i < length; ++i)
		{
			if(buff[i] >= 256)
				return -1;
		}
	}
	else if(TheGameSpyInfo->getDisallowNonAsianText())
	{
		const WideChar *buff = gameName.str();
		Int length =  gameName.getLength();
		Bool hasUnicode = FALSE;
		for(Int i = 0; i < length; ++i)
		{
			if(buff[i] >= 256)
			{
				hasUnicode = TRUE;
				break;
			}
		}
		if(!hasUnicode)
			return -1;
	}



	Int index = GadgetListBoxAddEntryText(win, game->getGameName(), gameColor, -1, COLUMN_NAME);
	GadgetListBoxSetItemData(win, reinterpret_cast<void*>(std::uintptr_t(game->getID())), index);

	UnicodeString s;

	if (showMap)
	{
		UnicodeString mapName;
		const MapMetaData *md = TheMapCache->findMap(game->getMap());
		if (md)
		{
			mapName = md->m_displayName;
		}
		else
		{
			const char *start = game->getMap().reverseFind('\\');
			if (start)
			{
				++start;
			}
			else
			{
				start = game->getMap().str();
			}
			mapName.translate( start );
		}
		GadgetListBoxAddEntryText(win, mapName, gameColor, index, COLUMN_MAP);

		const LadderInfo * li = TheLadderList->findLadder(game->getLadderIP(), game->getLadderPort());
		if (li)
		{
			GadgetListBoxAddEntryText(win, li->name, gameColor, index, COLUMN_LADDER);
		}
		else if (game->getLadderPort())
		{
			GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:UnknownLadder"), gameColor, index, COLUMN_LADDER);
		}
		else
		{
			GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:NoLadder"), gameColor, index, COLUMN_LADDER);
		}
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_MAP);
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_LADDER);
	}

	s.format(L"%d/%d", game->getReportedNumPlayers(), game->getReportedMaxPlayers());
	GadgetListBoxAddEntryText(win, s, gameColor, index, COLUMN_NUMPLAYERS);

	if (game->getHasPassword())
	{
		const Image *img = TheMappedImageCollection->findImageByName("Password");
		Int width = 10, height = 10;
		if (img)
		{
			width = img->getImageWidth();
			height = img->getImageHeight();
		}
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_PASSWORD, width, height);
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_PASSWORD);
	}

	if (game->getAllowObservers())
	{
		const Image *img = TheMappedImageCollection->findImageByName("Observer");
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_OBSERVER);
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_OBSERVER);
	}

#if !RTS_GENERALS
  {
    if (game->getUseStats())
    {
      if (const Image *img = TheMappedImageCollection->findImageByName("GoodStatsIcon"))
      {
        GadgetListBoxAddEntryImage(win, img, index, COLUMN_USE_STATS, img->getImageHeight(), img->getImageWidth());
      }
    }
  }
#endif

	// GeneralsX @feature Matchmaking. A game with no ping measurement gets an EMPTY ping cell.
	//
	// getPingValue() answers the full ping timeout when it has nothing to compare, so an unmeasured
	// game scores worse than the "bad" cutoff and is drawn with the red one-bar icon. That is not a
	// pessimistic reading of a real measurement, it is the absence of one being rendered as a
	// verdict - and every game listed by our relay is in that position, because the relay's GXGAME
	// reply carries no ping. Showing a bar there would tell the player, falsely and about every
	// single row, that the game is unplayable. Blank says what we actually know.
	if (game->getPingString().isEmpty())
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_PING);
		return index;
	}

	s.format(L"%d", game->getPingAsInt());
	GadgetListBoxAddEntryText(win, s, gameColor, index, COLUMN_PING);
	Int ping = game->getPingAsInt();
	Int width = 10, height = 10;
	if (pingImages[0])
	{
		width = pingImages[0]->getImageWidth();
		height = pingImages[0]->getImageHeight();
	}

	// GeneralsX @bugfix Claude 07/08/2026 The cutoffs below used to come from TheGameSpyConfig,
	// dereferenced unconditionally. Two problems, and the second one is why the config is gone
	// entirely rather than merely null-guarded.
	//
	// First, the deref was safe only because it was UNREACHABLE: every relay-listed game had an
	// empty ping string and returned at the guard above. Publishing a real measurement puts this
	// on a screen whose backend is torn down while the layout animates out - the same window that
	// already produced two null-vtable crashes in this file (see the notes on gameTooltip).
	//
	// Second, and worse, a null guard alone would have been DEAD CODE. SetUpGameSpyForRelayLobby()
	// does `if (TheGameSpyConfig == nullptr) TheGameSpyConfig = GameSpyConfigInterface::create("")`,
	// so on the relay path the config always exists - and, built from an empty string, it carries
	// GameSpy's own defaults of 300 ms and 600 ms. Those describe a 2003 dial-up matchmaker, not
	// this relay: a 250 ms game would have been drawn with the green three-bar icon.
	//
	// The bands are now the ones the in-game HUD already uses (InGameUI's kLatencyMsGood /
	// kLatencyMsFair), so the lobby dot and the millisecond readout beside the FPS counter agree
	// about what "good" means instead of each having its own opinion. Both values are milliseconds
	// on every path that can still reach here - GameSpy's own m_pingInt was milliseconds too.
	const Int cutoffGood = 80;
	const Int cutoffBad = 180;

	// CLH picking an arbitrary number for our ping display
	// GeneralsX @info pingImages[0] is the GOOD (three-bar) icon and [2] is the BAD one - the
	// numbering runs opposite to quality. Do not "tidy" this by reversing the indices.
	if (ping < cutoffGood)
	{
		GadgetListBoxAddEntryImage(win, pingImages[0], index, COLUMN_PING, width, height);
	}
	else if (ping < cutoffBad)
	{
		GadgetListBoxAddEntryImage(win, pingImages[1], index, COLUMN_PING, width, height);
	}
	else
	{
		GadgetListBoxAddEntryImage(win, pingImages[2], index, COLUMN_PING, width, height);
	}

	return index;
}

void RefreshGameListBox( GameWindow *win, Bool showMap )
{
	if (!win)
		return;

	// save off selection
	Int selectedIndex = -1;
	Int indexToSelect = -1;
	Int selectedID = 0;
	GadgetListBoxGetSelected(win, &selectedIndex);
	if (selectedIndex != -1 )
	{
	// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
	selectedID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(win, selectedIndex)));
	}
	int prevPos = GadgetListBoxGetTopVisibleEntry( win );

	// empty listbox
	GadgetListBoxReset(win);

	// GeneralsX @feature Claude 06/08/2026 A header row naming the columns.
	//
	// The list has always BEEN a table - COLUMN_NAME/MAP/NUMPLAYERS/PASSWORD/PING are real columns -
	// with nothing anywhere saying so, so the reader had to infer from context what the number pair,
	// the padlock and the coloured dot meant. The sort buttons above the list name three sort orders,
	// not the columns, and they do not line up with them.
	//
	// Inserted as the first ROW rather than as .wnd chrome because the layouts live inside a .big
	// archive; a row costs nothing and travels with the code. It carries no item data, and every
	// join path guards on `selectedID > 0` (WOLLobbyMenu.cpp), so it cannot be selected into a join.
	//
	// Deliberately placed before the populate loop and unconditionally, so an empty list still shows
	// the shape of what is missing rather than a blank box.
	{
		// EVERY column gets a label, including the two narrow icon columns. The first version of
		// this labelled only five of them, so "Pwd" sat over whichever icon column happened to
		// follow and the operator reasonably asked what the tick meant. Labels are kept SHORT
		// because these columns are icon-width in the .wnd and a long word wraps inside the cell
		// rather than widening it.
		const Color headerColor = GameMakeColor( 150, 190, 255, 255 );
		const Int hdr = GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColGame", L"Game (host)"),
			headerColor, -1, COLUMN_NAME);
		if (showMap)
			GadgetListBoxAddEntryText(win,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColMap", L"Map"), headerColor, hdr, COLUMN_MAP);
		GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColPlayers", L"Slots"), headerColor, hdr, COLUMN_NUMPLAYERS);
		GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColLock", L"Locked"), headerColor, hdr, COLUMN_PASSWORD);
		// COLUMN_OBSERVER is zero-width in the shipped .wnd and RelayFixGameListColumns leaves it
		// that way, because the relay path never allows observers. Labelling a zero-width cell puts
		// text nowhere useful and only crowds its neighbour, so it is deliberately unlabelled -
		// as COLUMN_LADDER already was, for the same reason.
#if !RTS_GENERALS
		GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColStats", L"Rec"), headerColor, hdr, COLUMN_USE_STATS);
#endif
		GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:LobbyColPing", L"Ping"), headerColor, hdr, COLUMN_PING);
	}

	// sort our games
	typedef std::multiset<GameSpyStagingRoom *, GameSortStruct> SortedGameList;
	SortedGameList sgl;
	StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
	populateBuddyGames();
	for (StagingRoomMap::iterator srmIt = srm->begin(); srmIt != srm->end(); ++srmIt)
	{
		sgl.insert(srmIt->second);
	}

	// populate listbox
	for (SortedGameList::iterator sglIt = sgl.begin(); sglIt != sgl.end(); ++sglIt)
	{
		GameSpyStagingRoom *game = *sglIt;
		if (game)
		{
			Int index = insertGame(win, game, showMap);
			if (game->getID() == selectedID)
			{
				indexToSelect = index;
			}
		}
	}

	clearBuddyGames();

	// GeneralsX @feature Claude 06/08/2026 Say so when the list is empty.
	//
	// An empty listbox is indistinguishable from a broken one: the operator could not tell "nobody
	// is hosting" from "the browser is not working" from "my relay is unreachable", and the honest
	// answer matters because all three have happened here. Red rather than the normal entry colour
	// so it reads as a STATE and not as a joinable row, and inserted only when the list is truly
	// empty so it can never be selected alongside a real game.
	//
	// GeneralsX @bugfix Claude 07/08/2026 "the browser ran and found nothing" was not true yet.
	//
	// The claim above - that reaching here with an empty sgl means the browser ran and found
	// nothing - holds between sweeps but NOT on entry. Nothing has been asked yet the first time
	// this screen draws, so the notice fired over a lobby that simply had not answered, which is
	// why it flashed for split seconds against a Linux host that never dropped its lobby.
	//
	// Transport::hasPublishedGameList() is the honest test: it is TRUE only once a sweep has
	// actually completed - a game arrived, or the relay explicitly answered "no games". Until then
	// the truthful state is "asking", not "none", and saying nothing beats saying something false.
	Bool sweepCompleted = TRUE;
	if (TheLAN != nullptr)
	{
		const Transport *t = TheLAN->getTransport();
		if (t != nullptr && t->isRelayEnabled())
			sweepCompleted = t->hasPublishedGameList();
	}

	if (sgl.empty() && sweepCompleted)
	{
		const Color noGamesColor = GameMakeColor( 255, 60, 60, 255 );
		GadgetListBoxAddEntryText(win,
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:NoLobbiesAvailable",
				L"No lobbies available at this moment"),
			noGamesColor, -1, 0);
	}

	// restore selection
	GadgetListBoxSetSelected(win, indexToSelect); // even for -1, so we can disable the 'Join Game' button
//	if(prevPos > 10)
		GadgetListBoxSetTopVisibleEntry( win, prevPos  );//+ 1

	if (indexToSelect < 0 && selectedID)
	{
		TheWindowManager->winSetLoneWindow(nullptr);
	}
}

void RefreshGameInfoListBox( GameWindow *mainWin, GameWindow *win )
{
//	if (!mainWin || !win)
//		return;
//
//	GadgetListBoxReset(win);
//
//	Int selected = -1;
//	GadgetListBoxGetSelected(mainWin, &selected);
//	if (selected < 0)
//	{
//		return;
//	}
//
//	Int selectedID = (Int)GadgetListBoxGetItemData(mainWin, selected);
//	if (selectedID < 0)
//	{
//		return;
//	}
//
//	StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
//	StagingRoomMap::iterator srmIt = srm->find(selectedID);
//	if (srmIt != srm->end())
//	{
//		GameSpyStagingRoom *theRoom = srmIt->second;
//		theRoom->cleanUpSlotPointers();
//
//		// game name
////		GadgetListBoxAddEntryText(listboxLobbyGameInfo, theRoom->getGameName(), GameSpyColor[GSCOLOR_DEFAULT], -1);
//
//		const LadderInfo * li = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
//		if (li)
//		{
//			UnicodeString tmp;
//			tmp.format(TheGameText->fetch("TOOLTIP:LadderName"), li->name.str());
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, tmp, GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//		else if (theRoom->getLadderPort())
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:UnknownLadder"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//		else
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:NoLadder"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//
//		if (theRoom->getExeCRC() != TheGlobalData->m_exeCRC || theRoom->getIniCRC() != TheGlobalData->m_iniCRC)
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:InvalidGameVersionSingleLine"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//
//		// map name
//		UnicodeString mapName;
//		const MapMetaData *md = TheMapCache->findMap(theRoom->getMap());
//		if (md)
//		{
//			mapName = md->m_displayName;
//		}
//		else
//		{
//			const char *start = theRoom->getMap().reverseFind('\\');
//			if (start)
//			{
//				++start;
//			}
//			else
//			{
//				start = theRoom->getMap().str();
//			}
//			mapName.translate( start );
//		}
//
//		GadgetListBoxAddEntryText(listboxLobbyGameInfo, mapName, GameSpyColor[GSCOLOR_DEFAULT], -1);
//
//		// player list (rank, win/loss, side)
//		for (Int i=0; i<MAX_SLOTS; ++i)
//		{
//			const GameSpyGameSlot *slot = theRoom->getGameSpySlot(i);
//			if (slot && slot->isHuman())
//			{
//				UnicodeString theName, theRating, thePlayerTemplate;
//				Int colorIdx = slot->getColor();
//				theName = slot->getName();
//				theRating.format(L" (%d-%d)", slot->getWins(), slot->getLosses());
//				const PlayerTemplate * pt = ThePlayerTemplateStore->getNthPlayerTemplate(slot->getPlayerTemplate());
//				if (pt)
//				{
//					thePlayerTemplate = pt->getDisplayName();
//				}
//				else
//				{
//					thePlayerTemplate = TheGameText->fetch("GUI:Random");
//				}
//
//				UnicodeString theText;
//				theText.format(L"%ls - %ls - %ls", theName.str(), thePlayerTemplate.str(), theRating.str());
//
//				Int theColor = GameSpyColor[GSCOLOR_DEFAULT];
//				const MultiplayerColorDefinition *mcd = TheMultiplayerSettings->getColor(colorIdx);
//				if (mcd)
//				{
//					theColor = mcd->getColor();
//				}
//
//				GadgetListBoxAddEntryText(listboxLobbyGameInfo, theText, theColor, -1);
//			}
//		}
//	}

}

void RefreshGameListBoxes()
{
	GameWindow *main = GetGameListBox();
	GameWindow *info = GetGameInfoListBox();

	RefreshGameListBox( main, (info == nullptr) );

	if (info)
	{
		RefreshGameInfoListBox( main, info );
	}
}

void ToggleGameListType()
{
	isSmall = !isSmall;
	if(isSmall)
	{
		parentGameListLarge->winHide(TRUE);
//		parentGameListSmall->winHide(FALSE);
	}
	else
	{
		parentGameListLarge->winHide(FALSE);
//		parentGameListSmall->winHide(TRUE);
	}

	RefreshGameListBoxes();
}

// for use by GameWindow::winSetTooltipFunc
// displays the Army Tooltip for the player templates
void playerTemplateComboBoxTooltip(GameWindow *wndComboBox, WinInstanceData *instData, UnsignedInt mouse)
{
	Int index = 0;
	GadgetComboBoxGetSelectedPos(wndComboBox, &index);
	// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
	Int templateNum = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(wndComboBox, index)));
	UnicodeString ustringTooltip;
	if (templateNum == -1)
	{
			// the "Random" template is always first
			ustringTooltip = TheGameText->fetch("TOOLTIP:BioStrategyLong_Random");
	}
	else
	{
		const PlayerTemplate *playerTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(templateNum);
		if (playerTemplate)
		{
			ustringTooltip = TheGameText->fetch(playerTemplate->getTooltip());
		}
	}
	TheMouse->setCursorTooltip(ustringTooltip);
}

// -----------------------------------------------------------------------------

// for use by GameWindow::winSetTooltipFunc
// displays the Army Tooltip for the player templates
void playerTemplateListBoxTooltip(GameWindow *wndListBox, WinInstanceData *instData, UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);
	GadgetListBoxGetEntryBasedOnXY(wndListBox, x, y, row, col);
	if (row == -1 || col == -1)
		return;

// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
Int templateNum = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(wndListBox, row, col)));
	UnicodeString ustringTooltip;
	if (templateNum == -1)
	{
			// the "Random" template is always first
			ustringTooltip = TheGameText->fetch("TOOLTIP:BioStrategyLong_Random");
	}
	else
	{
		const PlayerTemplate *playerTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(templateNum);
		if (playerTemplate)
		{
			ustringTooltip = TheGameText->fetch(playerTemplate->getTooltip());
		}
	}

	// use no tooltip delay here
	TheMouse->setCursorTooltip(ustringTooltip, 0);
}
