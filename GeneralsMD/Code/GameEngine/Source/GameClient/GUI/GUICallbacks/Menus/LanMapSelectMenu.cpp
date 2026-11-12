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

// FILE: LanMapSelectMenu.cpp ////////////////////////////////////////////////////////////////////////
// Author: Colin Day, October 2001
// Description: MapSelect menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/NameKeyGenerator.h"
#include "Common/MessageStream.h"
#include "Common/UserPreferences.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/CodeGui.h"			// CodeGuiRowsEnsure - the .wnd authors 8 start markers
#include "GameClient/CodeGuiSearchField.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameNetwork/LANAPICallbacks.h"
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
static NameKeyType winMapPreviewID = NAMEKEY_INVALID;
static GameWindow *parent = nullptr;
static GameWindow *mapList = nullptr;
static GameWindow *winMapPreview = nullptr;
static NameKeyType radioButtonSystemMapsID = NAMEKEY_INVALID;
static NameKeyType radioButtonUserMapsID = NAMEKEY_INVALID;

static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {0};
static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

// GeneralsX: Map search field, built in code. LanMapSelectMenu.wnd lives
// inside WindowZH.big and the .wnd editor is Windows-only, so the row is created at runtime against
// ListboxMap's real rect. Its windows are children of LanMapSelectMenuParent, so the Back/OK paths
// tear them down with the rest of the overlay.
// HOST ONLY: in a LAN lobby only the host may change the map, which is why
// LanGameOptionsMenuInit's !TheLAN->AmIHost() branch disables ButtonSelectMap. A client's list is
// read-only, so it gets no field at all rather than a greyed one.
static CodeGuiSearchField lanMapSearchField;
static Bool lanMapSearchUsesSystemMaps = TRUE;


// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
void positionStartSpots( AsciiString mapName, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
static const char *layoutFilename = "LanGameOptionsMenu.wnd";
static const char *parentName = "LanGameOptionsMenuParent";
static const char *gadgetsToHide[] =
{
	"MapWindow",
	//"StaticTextTitle",
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
	nullptr
};

static void showLANGameOptionsUnderlyingGUIElements( Bool show )
{
	ShowUnderlyingGUIElements( show, layoutFilename, parentName, gadgetsToHide, perPlayerGadgetsToHide );
	// GeneralsX @bugfix The seat rows' scroll bar is not one of the named gadgets: it hides and shows with them.
	CodeGuiRowsScrollShow( show, layoutFilename, parentName );
	GameWindow *win	= TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey("LanGameOptionsMenu.wnd:ButtonBack") );
	if(win)
		win->winEnable( show );

}

static void NullifyControls()
{
	parent = nullptr;
	mapList = nullptr;
	lanMapSearchField.reset();
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

//-------------------------------------------------------------------------------------------------
/** Refill the map listbox from the current source radio and the current search filter */
//-------------------------------------------------------------------------------------------------
// GeneralsX: One choke point for every repopulate, so the source radio
// and the search text can never drift apart.
static void repopulateLanMapList( void )
{
	if( mapList == nullptr )
		return;

	// Deliberately NOT calling TheMapCache->updateCache() here. Each row's item data is a raw
	// const char* into a MapCache key (MapUtil.cpp), safe only while the cache is not modified
	// under a live listbox. Rebuilding it per keystroke would dangle every row, and both
	// GLM_SELECTED and buttonOK dereference that pointer.
	populateMapListbox( mapList, lanMapSearchUsesSystemMaps, TRUE, TheLAN->GetMyGame()->getMap(),
											lanMapSearchField.getFilter() );

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
static void lanMapSearchChanged( CodeGuiSearchField *field )
{
	repopulateLanMapList();
}

//-------------------------------------------------------------------------------------------------
/** Initialize the MapSelect menu */
//-------------------------------------------------------------------------------------------------
void LanMapSelectMenuInit( WindowLayout *layout, void *userData )
{
	showLANGameOptionsUnderlyingGUIElements(FALSE);

	// set keyboard focus to main parent
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:LanMapSelectMenuParent" );
	parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );

	TheWindowManager->winSetFocus( parent );

	LANPreferences pref;
	Bool usesSystemMapDir = pref.usesSystemMapDir();

	const MapMetaData *mmd = TheMapCache->findMap(TheLAN->GetMyGame()->getMap());
	if (mmd)
	{
		usesSystemMapDir = mmd->m_isOfficial;
	}


	buttonBack = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:ButtonBack" );
	buttonOK = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:ButtonOK" );
	listboxMap = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:ListboxMap" );
	winMapPreviewID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:WinMapPreview" );

	radioButtonSystemMapsID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:RadioButtonSystemMaps" );
	radioButtonUserMapsID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:RadioButtonUserMaps" );
	GameWindow *radioButtonSystemMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonSystemMapsID );
	GameWindow *radioButtonUserMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonUserMapsID );
	winMapPreview = TheWindowManager->winGetWindowFromId(parent, winMapPreviewID);
	if (usesSystemMapDir)
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
	CodeGuiRowsEnsure( winMapPreview, "LanMapSelectMenu.wnd:ButtonMapStartPosition%d", MAX_SLOTS, CODEGUI_ROWS_FREE );

	AsciiString tmpString;
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		tmpString.format("LanMapSelectMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( winMapPreview, buttonMapStartPositionID[i] );
		// Layouts author ButtonMapStartPosition0..7 only, so winGetWindowFromId returns null for slots 8 and up.
		// The headless dummy window manager returns non-null, so headless runs never reach this.
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i] || i >= LEGACY_WND_SLOT_ROWS,
			("Could not find the ButtonMapStartPosition[%d]",i ));
		if (buttonMapStartPosition[i] == nullptr)
			continue;
		buttonMapStartPosition[i]->winHide(TRUE);
		buttonMapStartPosition[i]->winEnable(FALSE);
	}

	// get the listbox window
	NameKeyType mapListID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:ListboxMap" );
	mapList = TheWindowManager->winGetWindowFromId( parent, mapListID );
	if( mapList )
	{
		// GeneralsX: Reset the search state up front rather than trusting
		// NullifyControls: winDestroy unlinks the windows immediately but GWM_DESTROY is delivered
		// later from processDestroyList, so a stale entry pointer can still be set when the overlay
		// is reopened in the frame it was closed.
		lanMapSearchField.reset();
		lanMapSearchUsesSystemMaps = usesSystemMapDir;

		// Only the host may change the map. This is the same predicate LanGameOptionsMenuInit uses
		// to leave ButtonSelectMap enabled; a client gets no field, not a disabled one.
		if( TheLAN->AmIHost() )
		{
			lanMapSearchField.create( mapList, lanMapSearchChanged,
																"LanMapSelectMenu.wnd:TextEntryMapSearch",
																"LanMapSelectMenu.wnd:ButtonMapSearchClear" );
		}

		if (TheMapCache)
			TheMapCache->updateCache();		// the only updateCache; never once per keystroke
		repopulateLanMapList();
	}
}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu shutdown method */
//-------------------------------------------------------------------------------------------------
void LanMapSelectMenuShutdown( WindowLayout *layout, void *userData )
{

	// hide menu
	layout->hide( TRUE );

	NullifyControls();

	// our shutdown is complete
	TheShell->shutdownComplete( layout );
}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu update method */
//-------------------------------------------------------------------------------------------------
void LanMapSelectMenuUpdate( WindowLayout *layout, void *userData )
{

}

//-------------------------------------------------------------------------------------------------
/** Map select menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanMapSelectMenuInput( GameWindow *window, UnsignedInt msg,
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
						NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "LanMapSelectMenu.wnd:ButtonBack" );
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

//-------------------------------------------------------------------------------------------------
/** MapSelect menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanMapSelectMenuSystem( GameWindow *window, UnsignedInt msg,
																				  WindowMsgData mData1, WindowMsgData mData2 )
{
	GameWindow *mapWindow = nullptr;
	if (listboxMap != NAMEKEY_INVALID)
	{
		mapWindow = TheWindowManager->winGetWindowFromId( parent, listboxMap );
	}

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
		// GeneralsX: Live map search. GadgetTextEntryInput sends
		// GEM_UPDATE_TEXT to the entry's OWNER on every accepted character and every backspace, and
		// GEM_EDIT_DONE on Return. The field parents itself to the listbox's parent, which is
		// LanMapSelectMenuParent — the window this callback is bound to. No .wnd change involved.
		case GEM_UPDATE_TEXT:
		case GEM_EDIT_DONE:
		{
			lanMapSearchField.handleSystemMsg( msg, (GameWindow *)mData1 );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;

			// GeneralsX: The clear button is code-created and belongs to no
			// .wnd, so let the search field claim it by pointer before anything reads an id off
			// `control`.
			if ( lanMapSearchField.handleSystemMsg( msg, control ) )
				break;

			Int controlID = control->winGetWindowId();

			if ( controlID == radioButtonSystemMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				lanMapSearchUsesSystemMaps = TRUE;
				repopulateLanMapList();		// keeps any active search text applied
				LANPreferences pref;
				pref["UseSystemMapDir"] = "yes";
				pref.write();
			}
			else if ( controlID == radioButtonUserMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				lanMapSearchUsesSystemMaps = FALSE;
				repopulateLanMapList();
				LANPreferences pref;
				pref["UseSystemMapDir"] = "no";
				pref.write();
			}
			else if ( controlID == buttonBack )
			{

				if (mapSelectLayout)
				{
					mapSelectLayout->destroyWindows();
					deleteInstance(mapSelectLayout);
					mapSelectLayout = nullptr;
				}

				// set the controls to nullptr since they've been destroyed.
				NullifyControls();
				showLANGameOptionsUnderlyingGUIElements(TRUE);
				PostToLanGameOptions( MAP_BACK );
			}
			else if ( controlID == buttonOK )
			{
				Int selected = -1;
				UnicodeString map;

				// get the selected index
				if (mapWindow != nullptr)
				{
					GadgetListBoxGetSelected( mapWindow, &selected );
				}

				if( selected != -1 )
				{
					// get text of the map to load
					map = GadgetListBoxGetText( mapWindow, selected, 0 );

					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( mapWindow, selected );
					DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
					if (mapFname)
						asciiMap = mapFname;
					else
						asciiMap.translate( map );
					TheLAN->GetMyGame()->setMap( asciiMap );
					asciiMap.toLower();
					std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(asciiMap);
					if (it != TheMapCache->end())
					{
						TheLAN->GetMyGame()->getSlot(0)->setMapAvailability(true);
						TheLAN->GetMyGame()->setMapCRC( it->second.m_CRC );
						TheLAN->GetMyGame()->setMapSize( it->second.m_filesize );

						TheLAN->GetMyGame()->resetStartSpots();
						TheLAN->GetMyGame()->adjustSlotsForMap(); // BGC- adjust the slots for the new map.
					}


					if (mapSelectLayout)
					{
						mapSelectLayout->destroyWindows();
						deleteInstance(mapSelectLayout);
						mapSelectLayout = nullptr;
					}

					// set the controls to nullptr since they've been destroyed.
					NullifyControls();

					showLANGameOptionsUnderlyingGUIElements(TRUE);
					PostToLanGameOptions(SEND_GAME_OPTS);

				}
			}

			break;

		}

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
					map = GadgetListBoxGetText( mapWindow, rowSelected, 0 );

					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( mapWindow, rowSelected );
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

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}
