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

// FILE: WOLMapSelectMenu.cpp ////////////////////////////////////////////////////////////////////////
// Author: Matt Campbell, December 2001
// Description: MapSelect menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CustomMatchPreferences.h"
#include "Common/GameEngine.h"
#include "Common/MessageStream.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/CodeGui.h"			// CodeGuiRowsEnsure - the .wnd authors 8 start markers
#include "GameClient/CodeGuiSearchField.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/Shell.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/GUIUtil.h"


// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
// GeneralsX: The shipped .wnd layouts carry exactly this many slot
// rows. MAX_SLOTS is now larger; lookups for rows past this legitimately return null until the
// dynamic-row lobby rebuild lands, so the per-row assert below tolerates them.
enum { LEGACY_WND_SLOT_ROWS = 8 };

static NameKeyType buttonBack = NAMEKEY_INVALID;
static NameKeyType buttonOK = NAMEKEY_INVALID;
static NameKeyType listboxMap = NAMEKEY_INVALID;
static GameWindow *parent = nullptr;
static Bool raiseMessageBoxes = FALSE;
static GameWindow *winMapPreview = nullptr;
static NameKeyType winMapPreviewID = NAMEKEY_INVALID;

static NameKeyType radioButtonSystemMapsID = NAMEKEY_INVALID;
static NameKeyType radioButtonUserMapsID = NAMEKEY_INVALID;

extern WindowLayout *WOLMapSelectLayout;			///< Map selection overlay
static GameWindow *mapList = nullptr;

static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {0};
static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static GameWindow *winMapWindow = nullptr;

// GeneralsX: Map search field, built in code. WOLMapSelectMenu.wnd lives
// inside WindowZH.big and the .wnd editor is Windows-only, so the row is created at runtime against
// ListboxMap's real rect. Its windows are children of WOLMapSelectMenuParent, so the Back/OK paths
// tear them down with the rest of the overlay.
// HOST ONLY: in a staging room only the host may change the map, which is why
// WOLGameSetupMenuInit's !TheGameSpyInfo->amIHost() branch disables ButtonSelectMap. A client's
// list is read-only, so it gets no field at all rather than a greyed one.
static CodeGuiSearchField wolMapSearchField;
static Bool wolMapSearchUsesSystemMaps = TRUE;

static void NullifyControls()
{
	parent = nullptr;
	mapList = nullptr;
	wolMapSearchField.reset();
	if (winMapPreview)
	{
		winMapPreview->winSetUserData(nullptr);
		winMapPreview = nullptr;
	}
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		buttonMapStartPosition[i] = nullptr;
	}
}

static const char *layoutFilename = "GameSpyGameOptionsMenu.wnd";
static const char *parentName = "GameSpyGameOptionsMenuParent";
static const char *gadgetsToHide[] =
{
	"MapWindow",
	//"StaticTextGameName",
	"StaticTextTeam",
	"StaticTextFaction",
	"StaticTextColor",
	"TextEntryMapDisplay",
	"ButtonSelectMap",
	"ButtonStart",
	"StaticTextMapPreview",

	nullptr
};
static const char *perPlayerGadgetsToHide[] =
{
	"ComboBoxTeam",
	"ComboBoxColor",
	"ComboBoxPlayerTemplate",
	//"ButtonStartPosition",
	nullptr
};
void positionStartSpots( AsciiString mapName, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);

static void showGameSpyGameOptionsUnderlyingGUIElements( Bool show )
{
	ShowUnderlyingGUIElements( show, layoutFilename, parentName, gadgetsToHide, perPlayerGadgetsToHide );
	// GeneralsX @bugfix The seat rows' scroll bar is not one of the named gadgets: it hides and shows with them.
	CodeGuiRowsScrollShow( show, layoutFilename, parentName );
	GameWindow *win	= TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:ButtonBack") );
	if(win)
		win->winEnable( show );
}

//-------------------------------------------------------------------------------------------------
/** Refill the map listbox from the current source radio and the current search filter */
//-------------------------------------------------------------------------------------------------
// GeneralsX: One choke point for every repopulate, so the source radio
// and the search text can never drift apart.
static void repopulateWOLMapList( void )
{
	if( mapList == nullptr )
		return;

	// Deliberately NOT calling TheMapCache->updateCache() here. Each row's item data is a raw
	// const char* into a MapCache key (MapUtil.cpp), safe only while the cache is not modified
	// under a live listbox. Rebuilding it per keystroke would dangle every row, and both
	// GLM_SELECTED and buttonOK dereference that pointer.
	populateMapListbox( mapList, wolMapSearchUsesSystemMaps, TRUE, TheGameSpyGame->getMap(),
											wolMapSearchField.getFilter() );

	// populateMapListbox always asks for row 0 when the map it wanted is missing, and
	// GadgetListBoxSystem's GLM_SET_SELECTION bails out before notifying the owner when row 0 has
	// no cell — i.e. exactly when the filter matched nothing. Without this the preview and the
	// start spots would keep showing a map that is no longer in the list.
	// positionStartSpots dereferences its mapWindow unconditionally on the not-found path.
	Int selected = -1;
	GadgetListBoxGetSelected( mapList, &selected );
	if( selected < 0 && winMapPreview )
		positionStartSpots( AsciiString::TheEmptyString, buttonMapStartPosition, winMapPreview );
}

//-------------------------------------------------------------------------------------------------
/** The search field changed; refill the list through the one choke point */
//-------------------------------------------------------------------------------------------------
static void wolMapSearchChanged( CodeGuiSearchField *field )
{
	repopulateWOLMapList();
}

// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
/** Initialize the MapSelect menu */
//-------------------------------------------------------------------------------------------------
void WOLMapSelectMenuInit( WindowLayout *layout, void *userData )
{

	// set keyboard focus to main parent
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:WOLMapSelectMenuParent" );
	parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );

	TheWindowManager->winSetFocus( parent );

	CustomMatchPreferences pref;
	Bool usesSystemMapDir = pref.usesSystemMapDir();
	winMapPreviewID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:WinMapPreview" );
	winMapPreview = TheWindowManager->winGetWindowFromId(parent, winMapPreviewID);

	const MapMetaData *mmd = TheMapCache->findMap(TheGameSpyGame->getMap());
	if (mmd)
	{
		usesSystemMapDir = mmd->m_isOfficial;
	}

	//if stats are enabled, only official maps can be used
	if( TheGameSpyInfo->getCurrentStagingRoom()->getUseStats() )
		usesSystemMapDir = true;

	buttonBack = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:ButtonBack" );
	buttonOK = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:ButtonOK" );
	listboxMap = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:ListboxMap" );
	radioButtonSystemMapsID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:RadioButtonSystemMaps" );
	radioButtonUserMapsID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:RadioButtonUserMaps" );
	winMapWindow = TheWindowManager->winGetWindowFromId( parent, listboxMap );

	GameWindow *radioButtonSystemMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonSystemMapsID );
	GameWindow *radioButtonUserMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonUserMapsID );
	if( TheGameSpyInfo->getCurrentStagingRoom()->getUseStats() )
	{	//disable unofficial maps if stats are being recorded
		GadgetRadioSetSelection( radioButtonSystemMaps, FALSE );
		radioButtonUserMaps->winEnable( FALSE );
	}
	else if (usesSystemMapDir)
		GadgetRadioSetSelection( radioButtonSystemMaps, FALSE );
	else
		GadgetRadioSetSelection( radioButtonUserMaps, FALSE );

	// GeneralsX: A 12-PLAYER MAP SHOWED EIGHT START MARKERS.
	//
	// The shipped .wnd authors ButtonMapStartPosition0..7, so the loop that
	// follows found nothing for 8.., skipped it, and GXCircle12 / GXCauldron12 drew no marker on
	// the bases of players 9-12. The game-options screens already clone the missing markers from
	// marker 0 - same art, font and draw func - and this is the same call. FREE fit: the markers are
	// placed on the map's Player_N_Start waypoints by positionStartSpots, not laid out in a column.
	// Idempotent: once 0..MAX_SLOTS-1 exist it measures and returns.
	CodeGuiRowsEnsure( winMapPreview, "WOLMapSelectMenu.wnd:ButtonMapStartPosition%d", MAX_SLOTS, CODEGUI_ROWS_FREE );

	AsciiString tmpString;
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		tmpString.format("WOLMapSelectMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( winMapPreview, buttonMapStartPositionID[i] );
		// Slots 8+ have no .wnd control: the layouts author ButtonMapStartPosition0..7 only, and MAX_SLOTS is larger.
		// -headless swaps in GameWindowManagerDummy, whose winGetWindowFromId returns non-null, so only a real
		// window manager hits a null here.
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i] || i >= LEGACY_WND_SLOT_ROWS,
			("Could not find the ButtonMapStartPosition[%d]",i ));
		if (buttonMapStartPosition[i] == nullptr)
			continue;
		buttonMapStartPosition[i]->winHide(TRUE);
		buttonMapStartPosition[i]->winEnable(FALSE);
	}

	raiseMessageBoxes = TRUE;
	showGameSpyGameOptionsUnderlyingGUIElements( FALSE );

	// get the listbox window
	NameKeyType mapListID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:ListboxMap" );
	mapList = TheWindowManager->winGetWindowFromId( parent, mapListID );
	if( mapList )
	{
		// GeneralsX: Reset the search state up front rather than trusting
		// NullifyControls: winDestroy unlinks the windows immediately but GWM_DESTROY is delivered
		// later from processDestroyList, so a stale entry pointer can still be set when the overlay
		// is reopened in the frame it was closed.
		wolMapSearchField.reset();
		wolMapSearchUsesSystemMaps = usesSystemMapDir;

		// Only the host may change the map. This is the same predicate WOLGameSetupMenuInit uses to
		// leave ButtonSelectMap enabled; a client gets no field, not a disabled one.
		if( TheGameSpyInfo->amIHost() )
		{
			wolMapSearchField.create( mapList, wolMapSearchChanged,
																"WOLMapSelectMenu.wnd:TextEntryMapSearch",
																"WOLMapSelectMenu.wnd:ButtonMapSearchClear" );
		}

		if (TheMapCache)
			TheMapCache->updateCache();		// the only updateCache; never once per keystroke
		repopulateWOLMapList();
	}

}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLMapSelectMenuShutdown( WindowLayout *layout, void *userData )
{
	NullifyControls();

	// hide menu
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );

}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu update method */
//-------------------------------------------------------------------------------------------------
void WOLMapSelectMenuUpdate( WindowLayout *layout, void *userData )
{

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = false;
	}

	// No update because the game setup screen is up at the same
	// time and it does the update for us...
}

//-------------------------------------------------------------------------------------------------
/** Map select menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLMapSelectMenuInput( GameWindow *window, UnsignedInt msg,
																				 WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

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
						NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "WOLMapSelectMenu.wnd:ButtonBack" );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}
void WOLPositionStartSpots();

//-------------------------------------------------------------------------------------------------
/** MapSelect menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLMapSelectMenuSystem( GameWindow *window, UnsignedInt msg,
																				  WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
			break;

		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			NullifyControls();
			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;

		}

		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxMap )
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						GadgetListBoxSetSelected( control, rowSelected );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonOK );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonOK );
					}
				}
				break;
			}
		//---------------------------------------------------------------------------------------------



			case GLM_SELECTED:
			{

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxMap )
				{
					int rowSelected = mData2;
					if( rowSelected < 0 )
					{
						positionStartSpots( AsciiString::TheEmptyString, buttonMapStartPosition, winMapPreview);
//						winMapPreview->winClearStatus(WIN_STATUS_IMAGE);
						break;
					}
					winMapPreview->winSetStatus(WIN_STATUS_IMAGE);
					UnicodeString map;
					// get text of the map to load
					map = GadgetListBoxGetText( winMapWindow, rowSelected, 0 );

					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( winMapWindow, rowSelected );
					DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
					if (mapFname)
						asciiMap = mapFname;
					else
						asciiMap.translate( map );
					asciiMap.toLower();
					Image *image = getMapPreviewImage(asciiMap);
					winMapPreview->winSetUserData((void *)TheMapCache->findMap(asciiMap));
					if(image)
					{
						winMapPreview->winSetEnabledImage(0, image);
					}
					else
					{
						winMapPreview->winClearStatus(WIN_STATUS_IMAGE);
					}
					positionStartSpots( asciiMap, buttonMapStartPosition, winMapPreview);
				}
				break;
			}
		//---------------------------------------------------------------------------------------------
		// GeneralsX: Live map search. GadgetTextEntryInput sends
		// GEM_UPDATE_TEXT to the entry's OWNER on every accepted character and every backspace, and
		// GEM_EDIT_DONE on Return. The field parents itself to the listbox's parent, which is
		// WOLMapSelectMenuParent — the window this callback is bound to. No .wnd change involved.
		case GEM_UPDATE_TEXT:
		case GEM_EDIT_DONE:
		{
			wolMapSearchField.handleSystemMsg( msg, (GameWindow *)mData1 );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;

			// GeneralsX: The clear button is code-created and belongs to no
			// .wnd, so let the search field claim it by pointer before anything reads an id off
			// `control`.
			if ( wolMapSearchField.handleSystemMsg( msg, control ) )
				break;

			Int controlID = control->winGetWindowId();

			if( controlID == buttonBack )
			{
				showGameSpyGameOptionsUnderlyingGUIElements( TRUE );

				if (WOLMapSelectLayout)
				{
					WOLMapSelectLayout->destroyWindows();
					deleteInstance(WOLMapSelectLayout);
					WOLMapSelectLayout = nullptr;
				}

				WOLPositionStartSpots();
			}
			else if ( controlID == radioButtonSystemMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				wolMapSearchUsesSystemMaps = TRUE;
				repopulateWOLMapList();		// keeps any active search text applied
				CustomMatchPreferences pref;
				pref.setUsesSystemMapDir(TRUE);
				pref.write();
			}
			else if ( controlID == radioButtonUserMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				wolMapSearchUsesSystemMaps = FALSE;
				repopulateWOLMapList();
				CustomMatchPreferences pref;
				pref.setUsesSystemMapDir(FALSE);
				pref.write();
			}
			else if( controlID == buttonOK )
			{
				Int selected;
				UnicodeString map;

				// get the selected index
				GadgetListBoxGetSelected( winMapWindow, &selected );

				if( selected != -1 )
				{

					// get text of the map to load
					map = GadgetListBoxGetText( winMapWindow, selected, 0 );


					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( winMapWindow, selected );
					DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
					if (mapFname)
						asciiMap = mapFname;
					else
						asciiMap.translate( map );
					TheGameSpyGame->setMap(asciiMap);
					asciiMap.toLower();
					std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(asciiMap);
					if (it != TheMapCache->end())
					{
						TheGameSpyGame->getGameSpySlot(0)->setMapAvailability(TRUE);
						TheGameSpyGame->setMapCRC( it->second.m_CRC );
						TheGameSpyGame->setMapSize( it->second.m_filesize );
					}

					TheGameSpyGame->adjustSlotsForMap(); // BGC- adjust the slots for the new map.
					TheGameSpyGame->resetAccepted();
					TheGameSpyGame->resetStartSpots();
					TheGameSpyInfo->setGameOptions();

					WOLDisplaySlotList();
					WOLDisplayGameOptions();

					if (WOLMapSelectLayout)
					{
						WOLMapSelectLayout->destroyWindows();
						deleteInstance(WOLMapSelectLayout);
						WOLMapSelectLayout = nullptr;
					}

					showGameSpyGameOptionsUnderlyingGUIElements( TRUE );

					WOLPositionStartSpots();

				}

			}

			break;

		}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}
