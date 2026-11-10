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
** GeneralsX: Reusable search field, extracted from SkirmishMapSelectMenu.
**
** Four map screens want the same row: a bold "Search" caption, a bold text entry, a one-tap clear
** button, and a listbox that shrinks to make room. The original implementation was four file
** statics on one screen; this is the same code with the state moved into an object so several
** screens can each hold their own, with no shared filter string between them.
**
** LIFETIME. This object does NOT own its windows. They are created as children of the listbox's
** parent, so the screen's WindowLayout::destroyWindows() tears them down with everything else and
** GadgetTextEntrySystem's own GWM_DESTROY frees the EntryData and its DisplayStrings. What the
** object holds is three raw pointers that MUST be forgotten the moment the engine frees the
** windows behind its back — GameWindowManager::reset() calls winDestroyAll() on match transitions.
** So: call reset() from the screen's GWM_DESTROY handler, and again before create() on the way in,
** because winDestroy only QUEUES a window and GWM_DESTROY arrives later from processDestroyList
** (reopening a screen in the frame it closed would otherwise see a stale entry pointer and skip
** construction entirely).
**
** IDS. Both id names are caller-supplied and must be unique process-wide: an id-less window gets
** NAMEKEY_INVALID and winGetWindowFromId( nullptr, id ) would match the wrong screen's field. Use
** the owning .wnd's prefix, e.g. "SkirmishMapSelectMenu.wnd:TextEntryMapSearch".
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
		*
		* Every coordinate is read back off the listbox rather than hardcoded, because the .wnd parser
		* has already applied CREATIONRESOLUTION scaling and its rect is therefore the only correct
		* source of device pixels. Call this BEFORE the first populate so the listbox's displayHeight
		* is already correct.
		*
		* Returns FALSE and builds nothing when there is no room (a tiny backbuffer), when the listbox
		* has no parent, or when this field is already built. A FALSE return is not an error: the
		* screen simply runs unfiltered, which is why getFilter() stays empty and every caller can go
		* on calling it unconditionally.
		*
		* `entryIdName` / `clearIdName` must be unique process-wide; see the file header.
		* `onChanged` may be nullptr for a field nobody listens to. */
	Bool create( GameWindow *listbox,
							 CodeGuiSearchFieldChangedFunc onChanged,
							 const char *entryIdName,
							 const char *clearIdName,
							 const WideChar *labelText = L"Search" );

	/// Forget the windows and empty the filter. Does NOT destroy anything — see the file header.
	void reset( void );

	//-----------------------------------------------------------------------------------------------
	/** Offer one of the OWNER window's system messages to this field.
		*
		* Returns TRUE when the message belonged to this field and has been fully handled, so a screen
		* system func reads:
		*
		*   case GEM_UPDATE_TEXT:
		*   case GEM_EDIT_DONE:
		*     s_search.handleSystemMsg( msg, (GameWindow *)mData1 );
		*     break;
		*
		*   case GBM_SELECTED:
		*     if( s_search.handleSystemMsg( msg, (GameWindow *)mData1 ) )
		*       break;
		*     ...
		*
		* Call it from GBM_SELECTED BEFORE anything reads an id off the control: the clear button is
		* code-created and carries only the id you gave it, so matching by pointer is what keeps it
		* out of the screen's own id comparisons.
		*
		* Only GEM_UPDATE_TEXT, GEM_EDIT_DONE and GBM_SELECTED are recognised; anything else returns
		* FALSE untouched. */
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
		* GeneralsX: Exists so a screen can RESTORE a search it was showing
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
