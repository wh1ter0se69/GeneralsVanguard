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
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/MapListFilter.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////
class GameWindow;
class CodeGuiSearchField;
class MapMetaData;
struct GXMapSources;

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

//-------------------------------------------------------------------------------------------------
/** GeneralsX @feature 14/11/2026 The map pickers' filters: Players, Source and Sort drop-downs on the "Select Map" title
	* band (above the search row when a layout has no room there), the search row above the map list, and "No maps match."
	* with a Clear filters button inside the list when nothing passes.
	* One object per picker (Skirmish, LAN, Online); each remembers its own choice in <userdata>/MapFilters.ini.
	*
	* The drop-downs wear the art of `comboTemplate`, an authored retail combo box (the setup screen's hidden team box);
	* with no template only the search row is built and no remembered filter applies. Source refines UNOFFICIAL maps
	* (Generals Vanguard, Downloaded and each launcher category, My maps, GXMapSources.h): under the Official radio it reads
	* "Official maps" and is disabled. Display only: nothing here touches what a map is or how it joins. */
//-------------------------------------------------------------------------------------------------
class CodeGuiMapFilters
{

public:

	/// `prefsKey` names this picker in MapFilters.ini ("Skirmish", "LAN", "Online"); a string literal.
	explicit CodeGuiMapFilters( const char *prefsKey );
	~CodeGuiMapFilters();

	/** Build the filter row and the search row, after TheMapCache->updateCache(). `idPrefix` is the layout's
		* ("SkirmishMapSelectMenu.wnd:"); the row finds the title and ButtonOK by it. `userMaps` is the radio's state. Remembered filters that would hide
		* `mapToShow` (the game's current map) are relaxed for this opening, so the picker always shows it. */
	Bool create( GameWindow *listbox, GameWindow *comboTemplate, CodeGuiSearchFieldChangedFunc onChanged,
							 const char *idPrefix, Bool userMaps, const AsciiString &mapToShow );

	/// Forget the windows and the search text, as CodeGuiSearchField::reset. Remembered filters stay.
	void reset( void );

	/// GEM_*, GBM_SELECTED and GCM_SELECTED from the OWNER's system func; TRUE when it was ours. A change calls `onChanged`.
	Bool handleSystemMsg( UnsignedInt msg, GameWindow *control );

	/// The Official / Unofficial radio moved: refill the Players and Source drop-downs. Does not repopulate.
	void setUserMaps( Bool userMaps );

	/** populateMapListbox with every filter applied; updates the "No maps match." state. `bothKinds` lists solo and
		* multiplayer maps together (Skirmish's unofficial list). The row picked before a filter changed stays selected
		* when it is still listed, else `mapToSelect`. */
	Int populate( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, const AsciiString &mapToSelect,
								Bool bothKinds = FALSE );

private:

	enum Source { SOURCE_ANY = 0, SOURCE_VANGUARD, SOURCE_DOWNLOADED, SOURCE_MINE, SOURCE_CATEGORY };

	static Bool acceptRow( const AsciiString &mapPath, const MapMetaData &md, void *ctx );
	Bool accepts( const AsciiString &lowerPath, const MapMetaData &md ) const;
	Source classify( const AsciiString &lowerPath, AsciiString *category ) const;
	Bool isFiltering( void ) const;
	void fillPlayers( void );
	void fillSource( void );
	void fillSort( void );
	void selectByData( GameWindow *combo, Int data );
	void readCombos( void );
	void loadPrefs( void );
	void savePrefs( void ) const;
	void clearFilters( void );
	void updateEmptyState( GameWindow *listbox );
	void keepSelection( void );

	CodeGuiSearchField							 m_search;
	const char											*m_prefsKey;
	CodeGuiSearchFieldChangedFunc		 m_onChanged;
	GameWindow											*m_players;			///< the three drop-downs; nullptr when there was no template
	GameWindow											*m_source;
	GameWindow											*m_sort;
	GameWindow											*m_emptyLabel;	///< "No maps match."
	GameWindow											*m_emptyClear;	///< Clear filters
	GameWindow											*m_listbox;			///< the map list create() was given
	AsciiString											 m_keep;				///< its picked row, copied before a change repopulates it
	Bool														 m_quiet;				///< TRUE while code moves a drop-down
	Bool														 m_userMaps;
	Int															 m_wantPlayers;	///< 0 = any
	Source													 m_wantSource;
	AsciiString											 m_wantCategory;	///< SOURCE_CATEGORY's catalogue id
	MapListSort											 m_sortOrder;
	GXMapSources										*m_sources;			///< loaded at create()
	AsciiString											 m_userMapDir;		///< lowercased, as populateMapListbox builds it

};
