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

// FILE: CodeGuiSearchField.h //////////////////////////////////////////////////////////////////////
// Desc:   Reusable "filter this listbox" row, built in code above an existing listbox
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** Reusable search field (caption, text entry, clear button) that shrinks a listbox to make room; state is per object.
** The three controls are EA's own from PopupJoinGame.wnd (the password caption, its text entry, the X button), built
** by the CodeGuiRetail* factories so the row matches the retail controls around it.
** The object does NOT own its windows: they are children of the listbox's parent and die with the layout.
** Call reset() from GWM_DESTROY and again before create(); the engine can free the windows behind its back.
** Both id names must be unique process-wide (e.g. "SkirmishMapSelectMenu.wnd:TextEntryMapSearch").
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////
class GameWindow;
class CodeGuiSearchField;

/// Invoked after the filter string has already been updated. The field is passed in so one
/// callback can serve several screens; read the new text with getFilter().
typedef void (*CodeGuiSearchFieldChangedFunc)( CodeGuiSearchField *field );

//-------------------------------------------------------------------------------------------------
/** A search row that filters a listbox sitting below it. */
//-------------------------------------------------------------------------------------------------
class CodeGuiSearchField
{

public:

	CodeGuiSearchField();

	//-----------------------------------------------------------------------------------------------
	/** Build the row directly above `listbox` and shrink the listbox by its height.
		* Coordinates are read back off the listbox (already scaled for CREATIONRESOLUTION). Call BEFORE the first populate.
		* Returns FALSE and builds nothing when there is no room, no parent, or the field is already built; the screen then
		* runs unfiltered. `entryIdName`/`clearIdName` must be unique process-wide; `onChanged` may be nullptr. */
	Bool create( GameWindow *listbox,
							 CodeGuiSearchFieldChangedFunc onChanged,
							 const char *entryIdName,
							 const char *clearIdName,
							 const WideChar *labelText = L"Search" );

	/// Forget the windows and empty the filter. Does NOT destroy anything — see the file header.
	void reset( void );

	//-----------------------------------------------------------------------------------------------
	/** Offer one of the OWNER window's system messages to this field; returns TRUE when it was handled.
		* Handles only GEM_UPDATE_TEXT, GEM_EDIT_DONE and GBM_SELECTED; anything else returns FALSE untouched.
		* Call from GBM_SELECTED before anything reads an id off the control: the clear button is code-created and is
		* matched by pointer. */
	Bool handleSystemMsg( UnsignedInt msg, GameWindow *control );

	/// The current filter text. Empty when the field was never built, so it is always safe to pass
	/// straight into populateMapListbox's trailing `filter` parameter.
	const UnicodeString &getFilter( void ) const { return m_filter; }

	/// TRUE once create() has produced a live entry field.
	Bool exists( void ) const { return m_entry != nullptr; }

	/// The entry gadget, for winSetFocus. nullptr until create() succeeds.
	GameWindow *getEntry( void ) const { return m_entry; }

	/**
		* Seed the field with text, as if the user had typed it.
		*
		* Exists so a screen can RESTORE a search it was showing
		* before it was torn down. Writing the entry text directly is not enough and is the trap this
		* replaces: m_filter is private and is only refreshed from GEM_EDIT_DONE, so a caller that set
		* the gadget text by hand would leave the visible box and the live filter disagreeing - the
		* list would show everything while the box read "raptor".
		*
		* Deliberately does NOT fire the changed callback. The caller is restoring state it already
		* knows about and is about to repopulate its own list anyway; calling back would make this a
		* path that takes an action, and a restore must never do that.
		*/
	void setFilter( const UnicodeString &text );

private:

	GameWindow										*m_label;			///< the "Search" caption, or nullptr when it did not fit
	GameWindow										*m_entry;			///< the text entry; also the "am I built?" flag
	GameWindow										*m_clear;			///< the one-tap clear button
	UnicodeString									 m_filter;		///< live copy of the entry text
	CodeGuiSearchFieldChangedFunc	 m_onChanged;

};
