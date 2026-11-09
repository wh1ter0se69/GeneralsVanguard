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

// FILE: GadgetComboBox.cpp ///////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: ListBox.cpp
//
// Created:   Dean Iverson, March 1998
//						Colin Day, June 2001
//
// Desc:      ListBox GUI control
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Common/Language.h"
#include "Common/AudioEventRTS.h"
#include "Common/GameAudio.h"
#include "Common/Debug.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GameWindowGlobal.h"
#include "GameClient/Keyboard.h"
#include "GameClient/LobbySetupUi.h"

#include <vector>

// DEFINES ////////////////////////////////////////////////////////////////////

// PRIVATE TYPES //////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// PUBLIC DATA ////////////////////////////////////////////////////////////////

// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////

// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
void HideListBox(GameWindow * window);
static void ShowListBox(GameWindow * window);

// GeneralsX @feature Claude 25/09/2026 LETTER KEYS ON AN OPEN DROP-DOWN.
//
// Asked for on the game-setup slot lists: with Open / Closed / Easy Army / Medium Army / Hard Army
// showing, O / C / E / M / H picks that entry at once. Done here, for every NON-EDITABLE combo,
// rather than in each menu, because the key never reaches a menu: keyboard focus is usually on the
// lobby chat box, which eats the letter first. Clicking a combo does not move focus (stock), so the
// open drop-down is tracked here and offered the keystroke before the focus window gets it.
//
// A keystroke can arrive twice: as a CHARACTER (GWM_IME_CHAR to the focused text field - SDL's
// SDL_EVENT_TEXT_INPUT, Win32's WM_CHAR through IMEManager) and as a KEY (WindowXlat). With a text
// field focused THE CHARACTER PICKS, when it arrives (GadgetComboBoxDropDownOwnsText), and the key
// does nothing: only the character knows the keyboard layout, it needs no partner (a key and its
// character come in no fixed order - Win32's DirectInput can read the key a frame before WM_CHAR,
// and two letters typed inside one SDL pump pass both precede their key-downs), and it says whether
// a key typed it at all (GWM_IME_CHAR_COUNT_MASK: IME and dead-key composition output is chat, never a
// pick) and whether it is an auto-repeat (GWM_IME_CHAR_REPEAT: never a second pick - kept out of chat
// when its first press picked, chat when that press was chat). Only a character naming an entry of the
// open list picks. With no text field focused (Skirmish) no character comes, and the KEY picks, named
// by the active layout (Keyboard::getLayoutKey) rather than the key table - a fresh press only
// (ComboKeyPressPicks); its auto-repeats and key-ups go on to the focus window as they always did.
//
// Editable combos are excluded - typing into them is their point. On the key path letters with
// Ctrl/Alt pass through untouched, as do non-letter keys (Esc still backs out of the screen, arrows
// still tab); on the text path the character is the truth, whatever modifier made it. Only a combo
// that is enabled and actually on screen takes keys, so a list left open under a hidden shell layout
// cannot swallow in-game hotkeys - and only one in the UI that has the keyboard: not behind a modal,
// not while the keystroke goes to another layout (ComboListHasKeyboard).
static GameWindow *s_openDropDown = nullptr;	///< the combo whose list is showing, if any
static GameWindow *s_keyPickCombo = nullptr;	///< set only while a letter's GCM_SELECTED is being delivered
/// The text path's held-key memory: the folded letter whose key's first press picked (0 = the last press did
/// not), so its auto-repeats stay out of chat even after the list closed. The next non-repeated keystroke
/// replaces it - an OS repeats only the key pressed last, and every repeat follows its own first press.
static WideChar s_textPickHeld = 0;

static GameWindow *rootOf( GameWindow *w )
{
	while( w != nullptr && w->winGetParent() != nullptr )
		w = w->winGetParent();
	return w;
}

/// `target`: the window the keystroke is going to - the focused window, or the text field its character
/// arrived at (nullptr = none). The list must belong to the UI that has the keyboard (ComboListHasKeyboard).
static Bool dropDownTakesKeys( GameWindow *combo, GameWindow *target )
{
	if( combo == nullptr || !BitIsSet( combo->winGetStyle(), GWS_COMBO_BOX ) )
		return FALSE;
	ComboBoxData *comboData = (ComboBoxData *)combo->winGetUserData();
	if( comboData == nullptr || comboData->isEditable || comboData->listBox == nullptr )
		return FALSE;
	if( comboData->listBox->winIsHidden() || !combo->winGetEnabled() )
		return FALSE;
	GameWindow *root = combo;
	for( GameWindow *w = combo; w != nullptr; w = w->winGetParent() )
	{
		if( w->winIsHidden() )
			return FALSE;
		root = w;
	}
	return ComboListHasKeyboard( root, TheWindowManager->winGetModal(), rootOf( target ) );
}

static void dropDownLeads( GameWindow *combo, std::vector<WideChar> &leads )
{
	ComboBoxData *comboData = (ComboBoxData *)combo->winGetUserData();
	const Int count = GadgetListBoxGetNumEntries( comboData->listBox );
	leads.assign( count > 0 ? count : 0, 0 );
	for( Int i = 0; i < count; ++i )
		leads[ i ] = ComboEntryLead( GadgetListBoxGetText( comboData->listBox, i ).str() );
}

/// Pick the entry the folded `ch` names on `combo`'s open list. FALSE = no entry answers to it.
static Bool dropDownPick( GameWindow *combo, WideChar ch )
{
	ComboBoxData *comboData = (ComboBoxData *)combo->winGetUserData();
	std::vector<WideChar> leads;
	dropDownLeads( combo, leads );
	const Int count = (Int)leads.size();
	Int current = -1;
	GadgetListBoxGetSelected( comboData->listBox, &current );
	const Int pick = ComboKeyPickIndex( leads.empty() ? nullptr : &leads[ 0 ], count, current, ch );
	if( pick < 0 )
		return FALSE;

	// A letter several entries share (French "Armee facile / moyenne / difficile") keeps the list
	// OPEN, so the next press of it cycles on to the next one. Only a letter naming one entry closes
	// the list and marks the pick as a key pick, which is what lets the menu open the next slot.
	const Bool shared = ComboKeyMatchCount( &leads[ 0 ], count, ch ) > 1;

	s_keyPickCombo = shared ? nullptr : combo;
	comboData->dontHide = shared;
	// Exactly what a click on the row does: GLM_SELECTED hides the list (unless dontHide), sets the
	// edit text and sends GCM_SELECTED to the owner, whose own rules (host-only, etc.) apply unchanged.
	GadgetListBoxSetSelected( comboData->listBox, pick );
	s_keyPickCombo = nullptr;
	comboData->dontHide = FALSE;		// GLM_SELECTED clears it; this covers a row with no cell
	return TRUE;
}

Bool GadgetComboBoxKeyPick( UnsignedByte key, UnsignedShort state )
{
	if( !BitIsSet( state, KEY_STATE_DOWN ) || !ComboKeyPressPicks( BitIsSet( state, KEY_STATE_AUTOREPEAT ) ) )
		return FALSE;
	if( TheKeyboard == nullptr || TheWindowManager == nullptr )
		return FALSE;

	GameWindow *focus = TheWindowManager->winGetFocus();
	GameWindow *combo = s_openDropDown;
	if( !dropDownTakesKeys( combo, focus ) )
		return FALSE;
	if( BitIsSet( state, KEY_STATE_CONTROL | KEY_STATE_ALT ) )
		return FALSE;

	// A text field has the focus, so this keystroke's character comes by the text path and picks
	// there (see above); the key goes on to the field exactly as it always did.
	if( focus != nullptr && BitIsSet( focus->winGetStyle(), GWS_ENTRY_FIELD ) )
		return FALSE;

	const WideChar ch = ComboKeyFold( TheKeyboard->getLayoutKey( (KeyDefType)key ) );
	if( ch == 0 )
		return FALSE;

	// A letter no entry answers to is not ours: it goes on to the focus window exactly as it does
	// with a mouse-opened list.
	return dropDownPick( combo, ch );
}

Bool GadgetComboBoxTakeKeyPick( GameWindow *comboBox )
{
	if( comboBox == nullptr || comboBox != s_keyPickCombo )
		return FALSE;
	s_keyPickCombo = nullptr;		// once: the owner's own slot-list refresh re-sends GCM_SELECTED
	return TRUE;
}

Bool GadgetComboBoxDropDownOwnsText( GameWindow *textEntry, WideChar ch, WindowMsgData how )
{
	// Only a character that names an entry of the open list picks. Every other letter, and Enter,
	// Backspace, space and punctuation, reach the text field - so chat typed while a list is open
	// (the menu opens the next slot's list by itself) still arrives whole.
	const Bool keystroke = ( how & GWM_IME_CHAR_COUNT_MASK ) != 0;
	const Bool repeat = ( how & GWM_IME_CHAR_REPEAT ) != 0;
	const WideChar folded = ComboKeyFold( ch );
	Bool owned = FALSE;
	if( folded != 0 && dropDownTakesKeys( s_openDropDown, textEntry ) )
	{
		ComboBoxData *comboData = (ComboBoxData *)s_openDropDown->winGetUserData();
		if( textEntry != comboData->editBox )
		{
			std::vector<WideChar> leads;
			dropDownLeads( s_openDropDown, leads );
			owned = ComboListOwnsChar( leads.empty() ? nullptr : &leads[ 0 ], (Int)leads.size(), ch );
		}
	}
	const ComboTextAction act = ComboTextFor( owned, keystroke, repeat, folded != 0 && folded == s_textPickHeld );
	if( keystroke && !repeat )
		s_textPickHeld = ( act == COMBO_TEXT_PICK ) ? folded : 0;
	if( act == COMBO_TEXT_PICK )
		dropDownPick( s_openDropDown, folded );
	return act != COMBO_TEXT_PASS;
}

void GadgetComboBoxOpenList( GameWindow *comboBox )
{
	if( comboBox == nullptr || !BitIsSet( comboBox->winGetStyle(), GWS_COMBO_BOX ) || !comboBox->winGetEnabled() )
		return;
	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();
	if( comboData == nullptr || comboData->listBox == nullptr || !comboData->listBox->winIsHidden() )
		return;
	comboData->dontHide = FALSE;
	TheWindowManager->winSetLoneWindow( comboBox );
	ShowListBox( comboBox );
}

// GadgetComboBoxInput =========================================================
/** Handle input for Combo box */
//=============================================================================
WindowMsgHandledType GadgetComboBoxInput( GameWindow *window, UnsignedInt msg,
												 WindowMsgData mData1, WindowMsgData mData2 )
{
//	ComboBoxData *combo = (ComboBoxData *)window->winGetUserData();
	WinInstanceData *instData = window->winGetInstanceData();
	GameWindow *editBox = GadgetComboBoxGetEditBox(window);
	switch (msg)
	{

		// ------------------------------------------------------------------------
		case GWM_CHAR:
		{

			switch (mData1)
			{

				// --------------------------------------------------------------------
				case KEY_DOWN:
				case KEY_RIGHT:
				case KEY_TAB:

					if( BitIsSet( mData2, KEY_STATE_DOWN ) )
						TheWindowManager->winNextTab(window);
					break;

				// --------------------------------------------------------------------
				case KEY_UP:
				case KEY_LEFT:

					if( BitIsSet( mData2, KEY_STATE_DOWN ) )
						TheWindowManager->winPrevTab(window);
					break;

				// --------------------------------------------------------------------
				default:
					return TheWindowManager->winSendInputMsg( editBox, GWM_CHAR, mData1, mData2 );

			}

			break;

		}

		// ------------------------------------------------------------------------
		case GWM_WHEEL_DOWN:
		{

			break;

		}

		// ------------------------------------------------------------------------
		case GWM_WHEEL_UP:
		{
			break;

		}

		// ------------------------------------------------------------------------
		case GWM_LEFT_UP:
		{
			ComboBoxData *comboData = (ComboBoxData *)window->winGetUserData();
				comboData->dontHide = FALSE;
				AudioEventRTS buttonClick("GUIClick");

				if( TheAudio )
				{
					TheAudio->addAudioEvent( &buttonClick );
				}

				GameWindow *listBox = GadgetComboBoxGetListBox(window);
				if (listBox)
				{
					TheWindowManager->winSetLoneWindow(window);
					// If the Listbox isn't showing, Show it.
					if(listBox->winIsHidden())
					{
						ShowListBox(window);
					}
					// if the Listbox was showing, hide it.
					else
					{
						HideListBox(window);
					}
				}			break;

		}

		// ------------------------------------------------------------------------
		case GWM_RIGHT_UP:
		{
			break;

		}

/*
		// ------------------------------------------------------------------------
		case GWM_MOUSE_ENTERING:
		{

			if( BitIsSet( instData->getStyle(), GWS_MOUSE_TRACK ) )
			{

				BitSet( instData->m_state, WIN_STATE_HILITED );
				TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																						GBM_MOUSE_ENTERING,
																						(WindowMsgData)window,
																						0 );
				TheWindowManager->winSetFocus( window );

			}

			break;

		}

		// ------------------------------------------------------------------------
		case GWM_MOUSE_LEAVING:
		{

			if( BitIsSet( instData->getStyle(), GWS_MOUSE_TRACK ))
			{

				BitClear( instData->m_state, WIN_STATE_HILITED );
				TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																						GBM_MOUSE_LEAVING,
																						(WindowMsgData)window,
																						0 );
			}

			break;

		}
*/

		// ------------------------------------------------------------------------
		case GWM_LEFT_DRAG:

			if (BitIsSet( instData->getStyle(), GWS_MOUSE_TRACK ) )
				TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																						GGM_LEFT_DRAG,
																						(WindowMsgData)window,
																						0 );
			break;

		// ------------------------------------------------------------------------
//		case GWM_LEFT_DOWN:

			// we want to eat the down... so we may receive the up.
//			return MSG_HANDLED;

		//-------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

// HideListBox ================================================================
/** Called to close the listbox if it is opened */
//=============================================================================
void HideListBox(GameWindow * window)
{
	ICoord2D winSize;
	ICoord2D newSize;
	if( s_openDropDown == window )
		s_openDropDown = nullptr;

	GameWindow *listBox = GadgetComboBoxGetListBox(window);
	if (!listBox)
		return;

	if(!listBox->winIsHidden())
	{
		listBox->winHide(TRUE);
		GameWindow *editBox = GadgetComboBoxGetEditBox(window);
		editBox->winGetSize(&winSize.x, &winSize.y);
		window->winGetSize(&newSize.x, &newSize.y);
		window->winSetSize(newSize.x, winSize.y );
	}
}

// ShowListBox ================================================================
/** Open the listbox. GeneralsX @refactor Claude 25/09/2026 This body used to be written out twice,
	* in the click handler and the drop-down-button handler; GadgetComboBoxOpenList is its third
	* caller, and it is also the one place that records which drop-down is open. */
//=============================================================================
static void ShowListBox(GameWindow * window)
{
	ComboBoxData *comboData = (ComboBoxData *)window->winGetUserData();
	GameWindow *listBox = GadgetComboBoxGetListBox(window);
	if (!listBox || !comboData)
		return;

	ICoord2D winSize;
	ICoord2D newSize;
	Int listX;
	Int multiplier;

	listBox->winHide(FALSE);
	window->winGetSize(&winSize.x, &winSize.y);
	WinInstanceData *listInstData = listBox->winGetInstanceData();
	ListboxData *listData = (ListboxData *)listBox->winGetUserData();
	// If we have less entries then our max display is set to, only show
	// those entries and not additional blank lines.  Also, just so it looks
	// pretty, hide the list box's sliders if we don't need to scroll.
	if(comboData->entryCount <= comboData->maxDisplay)
	{
		multiplier = comboData->entryCount;
		listX = winSize.x;// + 16;

		if(listData->upButton)
			listData->upButton->winHide(TRUE);
		if(listData->downButton)
			listData->downButton->winHide(TRUE);
		if(listData->slider)
			listData->slider->winHide(TRUE);
	}
	else
	{
		multiplier = comboData->maxDisplay;
		listX = winSize.x;
		if(listData->upButton)
			listData->upButton->winHide(FALSE);
		if(listData->downButton)
			listData->downButton->winHide(FALSE);
		if(listData->slider)
			listData->slider->winHide(FALSE);

	}

	newSize.y = ((TheWindowManager->winFontHeight( listInstData->getFont() ) ) * multiplier) + multiplier * 2 + 4;
	window->winSetSize(winSize.x , winSize.y + newSize.y );
	listBox->winSetPosition(0, winSize.y);

	listBox->winSetSize(listX , newSize.y);

	s_openDropDown = window;
}

// GadgetListBoxSystem ========================================================
/** Handle system messages for list box */
//=============================================================================
WindowMsgHandledType GadgetComboBoxSystem( GameWindow *window, UnsignedInt msg,
													WindowMsgData mData1, WindowMsgData mData2 )
{
//	ListboxData *list = (ListboxData *)window->winGetUserData();
	WinInstanceData *instData = window->winGetInstanceData();
	ComboBoxData *comboData = (ComboBoxData *)window->winGetUserData();
	switch( msg )
	{
		// ------------------------------------------------------------------------
		case GGM_SET_LABEL:
		{
			instData->setText(*(UnicodeString*)mData1);
			break;

		}

		// ------------------------------------------------------------------------
		case GCM_GET_TEXT:
		{
			if(comboData->editBox)
				*(UnicodeString*)mData2 = GadgetTextEntryGetText(comboData->editBox);
			break;
		}

		// ------------------------------------------------------------------------
		case GCM_SET_TEXT:
		{
			if (comboData->editBox)
				GadgetTextEntrySetText(comboData->editBox,*(const UnicodeString*)mData1);
			break;

		}

		case GEM_UPDATE_TEXT:
		{
			TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																	GCM_UPDATE_TEXT,
																	(WindowMsgData)window,
																	0 );
			if (comboData->listBox)
			{
				GadgetListBoxSetSelected(comboData->listBox, -1);
				HideListBox(window);
			}
			break;
		}

		// ------------------------------------------------------------------------
		// if we get sent an edit done message from the text box, lets notify the parent
		case GEM_EDIT_DONE:
		{
			if ((GameWindow *)mData1 == comboData->editBox)
			{
				HideListBox(window);
				TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																		GCM_SELECTED,
																		(WindowMsgData)window,
																		0 );
			}
			break;
		}
		// ------------------------------------------------------------------------
		// Set the selection in the listbox, this will trigger the listbox selected message
		// which will set the edit box.
		case GCM_SET_SELECTION:
		{
			GameWindow *listBox = GadgetComboBoxGetListBox(window);
			if(listBox)
			{
				if( !listBox->winIsHidden() && mData2 == TRUE )
					comboData->dontHide = TRUE;

				GadgetListBoxSetSelected(listBox, (Int)mData1);
			}
			break;
		}
		// ------------------------------------------------------------------------
		// Get what the listbox has selected.
		case GCM_GET_SELECTION:
		{
			if(comboData->listBox)
				GadgetListBoxGetSelected(comboData->listBox, (Int *)mData2);
			else
			{
				DEBUG_CRASH(("We don't have a listbox as part of the combo box"));
				*(Int *)mData2 = -1;
			}
			break;
		}
		// ------------------------------------------------------------------------
		// Set the User Data for the specified listbox element
		case GCM_SET_ITEM_DATA:
		{
			if(comboData->listBox)
			{
				GadgetListBoxSetItemData(comboData->listBox, (void *)mData2, (Int)mData1 );
			}

			break;
		}
		// ------------------------------------------------------------------------
		// Get the user Data for the specified listbox element
		case GCM_GET_ITEM_DATA:
		{
			if(comboData->listBox)
			{
				*(void **)mData2 = GadgetListBoxGetItemData(comboData->listBox, (Int) mData1, 0);
			}
			break;
		}
		// ------------------------------------------------------------------------
		// Pass onto the parent window the selection the listbox just made
		case GLM_SELECTED:
		{
				if((GameWindow *) mData1 == comboData->listBox)
				{
					if( comboData->dontHide == TRUE )
					{
						comboData->dontHide = FALSE;
					}
					else
						HideListBox(window);

					// Nothing was actually selected, so we just want to
					if( mData2 == -1)
					{
						break;
					}

					//Grab the text that was selected
					UnicodeString tempUString;
					Color color;
					tempUString = GadgetListBoxGetTextAndColor( comboData->listBox, &color, mData2, 0 );

					GadgetTextEntrySetTextColor(comboData->editBox, color);

					GadgetTextEntrySetText(comboData->editBox, tempUString);

					TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																		GCM_SELECTED,
																		(WindowMsgData)window,
																		0 );
				}
			break;
		}

		// ------------------------------------------------------------------------
		case GGM_LEFT_DRAG:
		{

			break;

		}

		// ------------------------------------------------------------------------
		case GCM_DEL_ALL:
		{
			if ( comboData->listBox )
				GadgetListBoxReset(comboData->listBox);
			if ( comboData->editBox )
				GadgetTextEntrySetText(comboData->editBox, UnicodeString::TheEmptyString );
			comboData->entryCount = 0;
			//HideListBox(window);
			break;

		}

		// ------------------------------------------------------------------------
		case GCM_DEL_ENTRY:
		{

			break;

		}
		// ------------------------------------------------------------------------
		case GGM_CLOSE:
		{
			HideListBox(window);
			break;

		}

		// ------------------------------------------------------------------------
		case GCM_ADD_ENTRY:
		{
			GameWindow *listBox = GadgetComboBoxGetListBox(window);
			ListboxData *listData = (ListboxData *)listBox->winGetUserData();

			Int addedIndex = -1;
			if( listBox )
			{
				//Increase our internal entry count
				comboData->entryCount++;
				//If we've exceeded the set listlength, resize it to twice the size
				if(comboData->entryCount >= listData->listLength)
					GadgetListBoxSetListLength(listBox,listData->listLength * 2);
				//Add the entry to the Listbox
				addedIndex = GadgetListBoxAddEntryText( listBox, *(UnicodeString*)mData1, mData2, -1, 0 );

				// Now resize the list box
				ICoord2D winSize;
				ICoord2D newSize;
				ICoord2D editBoxSize;
				Int listX;
				Int multiplier;
				WinInstanceData *listInstData = listBox->winGetInstanceData();
				GameWindow *editBox = GadgetComboBoxGetEditBox(window);
				window->winGetSize(&winSize.x, &winSize.y);
				editBox->winGetSize(&editBoxSize.x, &editBoxSize.y);
				// If the listbox has less entries then the MaxDisplay, size it smaller
				if(comboData->entryCount <= comboData->maxDisplay)
				{
					multiplier = comboData->entryCount;
					listX = winSize.x + 16;
					if(listData->upButton)
						listData->upButton->winHide(TRUE);
					if(listData->downButton)
						listData->downButton->winHide(TRUE);
					if(listData->slider)
						listData->slider->winHide(TRUE);
				}
				else
				{
					//Else size it to the MaxDisplay Size
					multiplier = comboData->maxDisplay;
					listX = winSize.x;
					if(listData->upButton)
						listData->upButton->winHide(FALSE);
					if(listData->downButton)
						listData->downButton->winHide(FALSE);
					if(listData->slider)
						listData->slider->winHide(FALSE);
				}
				newSize.y = ((TheWindowManager->winFontHeight( listInstData->getFont() ) ) * multiplier) + multiplier * 2 + 4;
				listBox->winSetPosition(0, editBoxSize.y);
				listBox->winSetSize(listX , newSize.y);
			}

			return( (WindowMsgHandledType) addedIndex );
		}


		// ------------------------------------------------------------------------
		case GWM_CREATE:
			break;

		// ------------------------------------------------------------------------
		case GGM_RESIZED:
		{
			Int width = (Int)mData1;
			Int height = (Int)mData2;
			ICoord2D dropDownSize;

			// get needed window sizes

			comboData->dropDownButton->winGetSize( &dropDownSize.x, &dropDownSize.y );

			GameWindow *listBox = GadgetComboBoxGetListBox(window);
			if (listBox->winIsHidden())
			{
				if (listBox)
					listBox->winSetSize(width,height);

				if( comboData->dropDownButton )
				{
					comboData->dropDownButton->winSetPosition( width - dropDownSize.x, 0 );
				}

				if( comboData->editBox )
				{
					comboData->editBox->winSetPosition(  0,  0 );
					comboData->editBox->winSetSize( width - dropDownSize.x, height );
				}
			}
			break;

		}

		// ------------------------------------------------------------------------

		// ------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			TheWindowManager->winSetLoneWindow(nullptr); // if we are transitioning screens, close all combo boxes
			if( s_openDropDown == window )
				s_openDropDown = nullptr;
			if( s_keyPickCombo == window )
				s_keyPickCombo = nullptr;
			delete (ComboBoxData *)window->winGetUserData();
			window->winSetUserData(nullptr);
			comboData = nullptr;
			break;
		}

		// ------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// If we're losing focus
			if( mData1 == FALSE )
			{
				BitClear( instData->m_state, WIN_STATE_HILITED );
			}
			else
			{
				BitSet( instData->m_state, WIN_STATE_HILITED );
			}

			TheWindowManager->winSendSystemMsg( window->winGetOwner(),
																					GGM_FOCUS_CHANGE,
																					mData1,
																					window->winGetWindowId() );

			Bool wantsFocus = FALSE;
			GameWindow *editBox = GadgetComboBoxGetEditBox(window);
			// we need to tell the text entry box to take the focus.
			TheWindowManager->winSendSystemMsg( editBox, GWM_INPUT_FOCUS, mData1, (WindowMsgData)&wantsFocus );

			*(Bool*)mData2 = TRUE;
			break;

		}


		case GBM_SELECTED:
		{
			// See if the drop down button was selected
			if( (GameWindow *)mData1 == comboData->dropDownButton )
			{
				comboData->dontHide = FALSE;
				GameWindow *listBox = GadgetComboBoxGetListBox(window);
				if (listBox)
				{
					TheWindowManager->winSetLoneWindow(window);
					// If the Listbox isn't showing, Show it.
					if(listBox->winIsHidden())
					{
						ShowListBox(window);
					}
					// if the Listbox was showing, hide it.
					else
					{
						HideListBox(window);
					}
				}
			}
			break;
		}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

// GadgetComboBoxSetColors ====================================================
/** Set the colors for a Combo box, note that this will also automatically
	* change the colors of any attached slider, slider thumb, and slider
	* buttons */
//=============================================================================
void GadgetComboBoxSetColors( GameWindow *comboBox,
														 Color enabledColor,
														 Color enabledBorderColor,
														 Color enabledSelectedItemColor,
														 Color enabledSelectedItemBorderColor,
														 Color disabledColor,
														 Color disabledBorderColor,
														 Color disabledSelectedItemColor,
														 Color disabledSelectedItemBorderColor,
														 Color hiliteColor,
														 Color hiliteBorderColor,
														 Color hiliteSelectedItemColor,
														 Color hiliteSelectedItemBorderColor )
{
//	ComboBoxData *comboBoxData = (ComboBoxData *)comboBox->winGetUserData();
	// enabled

// enabled
		GadgetComboBoxSetEnabledColor( comboBox, enabledColor);
		GadgetComboBoxSetEnabledBorderColor( comboBox, enabledBorderColor );
		GadgetComboBoxSetEnabledSelectedItemColor( comboBox, enabledSelectedItemColor );
		GadgetComboBoxSetEnabledSelectedItemBorderColor( comboBox, enabledSelectedItemBorderColor );
		// disabled
		GadgetComboBoxSetDisabledColor( comboBox, disabledColor );
		GadgetComboBoxSetDisabledBorderColor( comboBox, disabledBorderColor );
		GadgetComboBoxSetDisabledSelectedItemColor( comboBox, disabledSelectedItemColor );
		GadgetComboBoxSetDisabledSelectedItemBorderColor( comboBox, disabledSelectedItemBorderColor );
		// hilite
		GadgetComboBoxSetHiliteColor( comboBox, hiliteColor );
		GadgetComboBoxSetHiliteBorderColor( comboBox,hiliteBorderColor );
		GadgetComboBoxSetHiliteSelectedItemColor( comboBox, hiliteSelectedItemColor );
		GadgetComboBoxSetHiliteSelectedItemBorderColor( comboBox, hiliteSelectedItemBorderColor );

	GameWindow *editBox = GadgetComboBoxGetEditBox(comboBox);
	if (editBox)
	{
		// enabled
		GadgetButtonSetEnabledColor( editBox, enabledColor );
		GadgetButtonSetEnabledBorderColor( editBox, enabledBorderColor );
		GadgetButtonSetEnabledSelectedColor( editBox, enabledSelectedItemColor );
		GadgetButtonSetEnabledSelectedBorderColor( editBox, enabledSelectedItemBorderColor );
		// disabled
		GadgetButtonSetDisabledColor( editBox, disabledColor );
		GadgetButtonSetDisabledBorderColor( editBox, disabledBorderColor );
		GadgetButtonSetDisabledSelectedColor( editBox, disabledSelectedItemColor );
		GadgetButtonSetDisabledSelectedBorderColor( editBox, disabledSelectedItemBorderColor );
		// hilite
		GadgetButtonSetHiliteColor( editBox,hiliteColor );
		GadgetButtonSetHiliteBorderColor( editBox, hiliteBorderColor );
		GadgetButtonSetHiliteSelectedColor( editBox, hiliteSelectedItemColor );
		GadgetButtonSetHiliteSelectedBorderColor( editBox, hiliteSelectedItemBorderColor );
	}

	GameWindow *dropDownButton = GadgetComboBoxGetDropDownButton(comboBox);
	if (dropDownButton)
	{
		// enabled
		GadgetButtonSetEnabledColor( dropDownButton, enabledColor );
		GadgetButtonSetEnabledBorderColor( dropDownButton, enabledBorderColor );
		GadgetButtonSetEnabledSelectedColor( dropDownButton, enabledSelectedItemColor );
		GadgetButtonSetEnabledSelectedBorderColor( dropDownButton, enabledSelectedItemBorderColor );
		// disabled
		GadgetButtonSetDisabledColor( dropDownButton, disabledColor );
		GadgetButtonSetDisabledBorderColor( dropDownButton, disabledBorderColor );
		GadgetButtonSetDisabledSelectedColor( dropDownButton, disabledSelectedItemColor );
		GadgetButtonSetDisabledSelectedBorderColor( dropDownButton, disabledSelectedItemBorderColor );
		// hilite
		GadgetButtonSetHiliteColor( dropDownButton,hiliteColor );
		GadgetButtonSetHiliteBorderColor( dropDownButton, hiliteBorderColor );
		GadgetButtonSetHiliteSelectedColor( dropDownButton, hiliteSelectedItemColor );
		GadgetButtonSetHiliteSelectedBorderColor( dropDownButton, hiliteSelectedItemBorderColor );
	}

	GameWindow * listBox = GadgetComboBoxGetListBox( comboBox );
	if ( listBox )
	{
		GadgetListBoxSetColors(listBox,
													enabledColor,
													enabledBorderColor,
													enabledSelectedItemColor,
													enabledSelectedItemBorderColor,
													disabledColor,
													disabledBorderColor,
													disabledSelectedItemColor,
													disabledSelectedItemBorderColor,
													hiliteColor,
													hiliteBorderColor,
													hiliteSelectedItemColor,
													hiliteSelectedItemBorderColor );
	}
}

// GadgetComboBoxSetIsEditable ================================================
/** Sets up the Text Entry gadget as editable or not */
//=============================================================================
void GadgetComboBoxSetIsEditable(GameWindow *comboBox, Bool isEditable  )
{
	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();
	GameWindow *editBox = GadgetComboBoxGetEditBox(comboBox);
	UnsignedInt status ;
	if(!editBox)
	 return;

	comboData->isEditable = isEditable;
	if (isEditable)
	{
		status = editBox->winGetStatus();

		BitClear(status, WIN_STATUS_NO_INPUT);
//		BitClear(status, WIN_STATUS_NO_FOCUS);
		editBox->winSetStatus(status);
	}
	else
	{
		status = editBox->winGetStatus();

		BitSet(status, WIN_STATUS_NO_INPUT);
//		BitSet(status, WIN_STATUS_NO_FOCUS);
		editBox->winSetStatus(status);
	}
}

// GadgetComboBoxSetIsAsciiOnly ==================================================
/** Get the text the Combo Box */
//=============================================================================
void GadgetComboBoxSetLettersAndNumbersOnly(GameWindow *comboBox, Bool isLettersAndNumbersOnly)
{
	//sanity
	if(comboBox == nullptr)
		return;
	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();

	comboData->lettersAndNumbersOnly = isLettersAndNumbersOnly;
	if(comboData->entryData)
		comboData->entryData->alphaNumericalOnly = isLettersAndNumbersOnly;

}

// GadgetComboBoxSetAsciiOnly ==================================================
/** Get the text the Combo Box */
//=============================================================================
void GadgetComboBoxSetAsciiOnly(GameWindow *comboBox, Bool isAsciiOnly  )
{
	//sanity
	if(comboBox == nullptr)
		return;
	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();

	comboData->asciiOnly = isAsciiOnly;
	if(comboData->entryData)
		comboData->entryData->aSCIIOnly = isAsciiOnly;

}

// GadgetComboBoxSetMaxChars ==================================================
/** Get the text the Combo Box */
//=============================================================================
void GadgetComboBoxSetMaxChars( GameWindow *comboBox, Int maxChars )
{
	//sanity
	if(comboBox == nullptr)
		return;

	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();
	comboData->maxChars = maxChars;
	comboData->entryData->maxTextLen = maxChars;

}

// GadgetComboBoxSetMaxDisplay ================================================
/** Sets the MaxDisplay variable to the new Max Display */
//=============================================================================
void GadgetComboBoxSetMaxDisplay( GameWindow *comboBox, Int maxDisplay )
{
	ComboBoxData *comboData = (ComboBoxData *)comboBox->winGetUserData();
	comboData->maxDisplay = maxDisplay;

}

// GadgetComboBoxGetText =======================================================
/** Get the text the Combo Box */
//=============================================================================
UnicodeString GadgetComboBoxGetText( GameWindow *comboBox )
{

	// sanity
	if( comboBox == nullptr )
		return UnicodeString::TheEmptyString;

	// verify that this is a combo box
	if( BitIsSet( comboBox->winGetStyle(), GWS_COMBO_BOX ) == FALSE )
		return UnicodeString::TheEmptyString;

	return GadgetTextEntryGetText( GadgetComboBoxGetEditBox(comboBox) );
}

// GadgetComboBoxSetText =======================================================
/** Set the text the Combo Box */
//=============================================================================
void GadgetComboBoxSetText( GameWindow *comboBox, UnicodeString text )
{
	if( comboBox == nullptr )
		return;

	GadgetTextEntrySetText(GadgetComboBoxGetEditBox(comboBox), text);
}

// GadgetComboBoxAddEntry =======================================================
/** Convenience wrapper function for adding an entry */
//=============================================================================
Int GadgetComboBoxAddEntry( GameWindow *comboBox, UnicodeString text, Color color )
{
	// sanity
	if( comboBox == nullptr )
		return -1;
	return (Int)TheWindowManager->winSendSystemMsg( comboBox, GCM_ADD_ENTRY, (WindowMsgData)&text, color );
}
// GadgetComboBoxReset =======================================================
/** Convenience wrapper function for resetting the Combo Box entries */
//=============================================================================
void GadgetComboBoxReset( GameWindow *comboBox )
{
	// sanity
	if( comboBox == nullptr )
		return;
	// reset via system message
	TheWindowManager->winSendSystemMsg( comboBox, GCM_DEL_ALL, 0, 0 );
}
// GadgetComboBoxHideList =======================================================
/** Convenience wrapper function hiding the list */
//=============================================================================
void GadgetComboBoxHideList( GameWindow *comboBox )
{
	// sanity
	if( comboBox == nullptr )
		return;
	// reset via system message
	TheWindowManager->winSendSystemMsg( comboBox, GGM_CLOSE, 0, 0 );
}
// GadgetComboBoxSetFont =======================================================
/** Function used to set the Font of the combo box and all sub gadgets */
//=============================================================================
void GadgetComboBoxSetFont( GameWindow *comboBox, GameFont *font )
{
	// sanity
	if( comboBox == nullptr )
		return;

	// set the ListBox gadget's font
	GameWindow *listBox = GadgetComboBoxGetListBox(comboBox);
	if(listBox)
		listBox->winSetFont( font);

	// set the Text Entry gadget's font
	GameWindow *editBox = GadgetComboBoxGetEditBox(comboBox);
	if(editBox)
		editBox->winSetFont(font);

	//Need to setup the default window font
	DisplayString *dString;

		// set the font for the display strings all windows have
	dString = comboBox->winGetInstanceData()->getTextDisplayString();
	if( dString )
		dString->setFont( font );
	dString = comboBox->winGetInstanceData()->getTooltipDisplayString();
	if( dString )
		dString->setFont( font );
}

// GadgetComboBoxSetEnabledTextColors =========================================
/** Set the Enabled Text Colors for the Sub Gadgets*/
//=============================================================================
void GadgetComboBoxSetEnabledTextColors(GameWindow *comboBox, Color color, Color borderColor )
{
	// sanity
	if( comboBox == nullptr )
		return;

	ComboBoxData *comboBoxData = (ComboBoxData *)comboBox->winGetUserData();
	if(comboBoxData->listBox)
		comboBoxData->listBox->winSetEnabledTextColors( color,borderColor);
	if(comboBoxData->editBox)
		comboBoxData->editBox->winSetEnabledTextColors(color,borderColor);
}
// GadgetComboBoxSetDisabledTextColors ========================================
/** Set the Disabled Text Colors for the Sub Gadgets */
//=============================================================================
void GadgetComboBoxSetDisabledTextColors(GameWindow *comboBox, Color color, Color borderColor )
{
	ComboBoxData *comboBoxData = (ComboBoxData *)comboBox->winGetUserData();
	// sanity
	if( comboBox == nullptr )
		return;

	if(comboBoxData->listBox)
		comboBoxData->listBox->winSetDisabledTextColors( color,borderColor);
	if(comboBoxData->editBox)
		comboBoxData->editBox->winSetDisabledTextColors(color,borderColor);
}
// GadgetComboBoxSetHiliteTextColors ==========================================
/** Set the Hilite Text Colors for the Sub Gadgets */
//=============================================================================
void GadgetComboBoxSetHiliteTextColors( GameWindow *comboBox,Color color, Color borderColor )
{
	// sanity
	if( comboBox == nullptr )
		return;

	ComboBoxData *comboBoxData = (ComboBoxData *)comboBox->winGetUserData();

	if(comboBoxData->listBox)
		comboBoxData->listBox->winSetHiliteTextColors( color,borderColor);
	if(comboBoxData->editBox)
		comboBoxData->editBox->winSetHiliteTextColors(color,borderColor);
}
// GadgetComboBoxSetIMECompositeTextColors ====================================
/** Set the IME Composite Text Colors Text Colors for the Sub Gadgets */
//=============================================================================
void GadgetComboBoxSetIMECompositeTextColors(GameWindow *comboBox, Color color, Color borderColor )
{
	// sanity
	if( comboBox == nullptr )
		return;

	ComboBoxData *comboBoxData = (ComboBoxData *)comboBox->winGetUserData();

	if(comboBoxData->listBox)
		comboBoxData->listBox->winSetIMECompositeTextColors( color,borderColor);
	if(comboBoxData->editBox)
		comboBoxData->editBox->winSetIMECompositeTextColors(color,borderColor);
}

// GadgetComboBoxGetSelectedPos ===============================================
/** Convenience wrapper function for getting the selected Position */
//=============================================================================
void GadgetComboBoxGetSelectedPos( GameWindow *comboBox, Int *selectedIndex )
{
	// sanity
	if( comboBox == nullptr )
		return;

	// get selected indices via system message
	TheWindowManager->winSendSystemMsg( comboBox, GCM_GET_SELECTION, 0, (WindowMsgData)selectedIndex );
}

// GadgetComboBoxSetSelectedPos ===============================================
/** Convenience wrapper function for setting the selected Position, if don't hide
		is set to true, the listbox won't be forced to hide when the Selected call is
		passed back */
//=============================================================================
void GadgetComboBoxSetSelectedPos( GameWindow *comboBox, Int selectedIndex, Bool dontHide )
{
	// sanity
	if( comboBox == nullptr )
		return;

	// get selected indices via system message
	TheWindowManager->winSendSystemMsg( comboBox, GCM_SET_SELECTION, selectedIndex, dontHide );
}
// GadgetComboBoxSetItemData ==================================================
/** Convenience wrapper function for setting the Item data for the listbox under the combo box */
//=============================================================================
void GadgetComboBoxSetItemData( GameWindow *comboBox, Int index, void *data )
{
	if (comboBox)
		TheWindowManager->winSendSystemMsg( comboBox, GCM_SET_ITEM_DATA, index, (WindowMsgData)data);
}
// GadgetComboBoxGetItemData ==================================================
/** Convenience wrapper function for getting the Item data from the listbox under the combo Box */
//=============================================================================
void *GadgetComboBoxGetItemData( GameWindow *comboBox, Int index )
{
	void *data = nullptr;

	if (comboBox)
	{
		TheWindowManager->winSendSystemMsg( comboBox, GCM_GET_ITEM_DATA, index, (WindowMsgData)&data);
	}
	return (data);
}

// GadgetComboBoxGetLength =================================================
/** Get the list length data contained in the listboxData
	* parameter. */
//=============================================================================
Int GadgetComboBoxGetLength( GameWindow *combobox )
{
	ComboBoxData *comboboxData = (ComboBoxData *)combobox->winGetUserData();
	if (comboboxData)
		return comboboxData->entryCount;

	return 0;
}

