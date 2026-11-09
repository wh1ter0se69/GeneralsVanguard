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

// FILE: CodeGuiSearchField.cpp ////////////////////////////////////////////////////////////////////
// Desc:   Reusable "filter this listbox" row, built in code above an existing listbox
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameClient/CodeGui.h"
#include "GameClient/CodeGuiSearchField.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetTextEntry.h"

//-------------------------------------------------------------------------------------------------
// Carried over verbatim from the SkirmishMapSelectMenu implementation this was extracted from, so
// that screen keeps the geometry it was verified with. See the note in create() for why they are
// not the stock lobby-label sizes.
static const Int CODEGUI_SEARCH_LABEL_POINTS = 10;
static const Int CODEGUI_SEARCH_ENTRY_POINTS = 12;
static const Short CODEGUI_SEARCH_MAX_LEN		 = 32;

//-------------------------------------------------------------------------------------------------
CodeGuiSearchField::CodeGuiSearchField()
	: m_label( nullptr ),
		m_entry( nullptr ),
		m_clear( nullptr ),
		m_onChanged( nullptr )
{
}

//-------------------------------------------------------------------------------------------------
void CodeGuiSearchField::reset( void )
{
	m_label			= nullptr;
	m_entry			= nullptr;
	m_clear			= nullptr;
	m_filter.clear();
	// m_onChanged deliberately survives: it is compile-time screen wiring, not window state.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/08/2026 See the note on the declaration: restoring a search needs
// BOTH halves moved together, or the box and the filter disagree.
void CodeGuiSearchField::setFilter( const UnicodeString &text )
{
	m_filter = text;

	if( m_entry != nullptr )
	{
		// GEM_SET_TEXT does not echo GEM_UPDATE_TEXT back at the owner, so this cannot re-enter
		// handleSystemMsg - the same property the clear button relies on above.
		GadgetTextEntrySetText( m_entry, text );
	}
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiSearchField::create( GameWindow *listbox, CodeGuiSearchFieldChangedFunc onChanged,
																 const char *entryIdName, const char *clearIdName,
																 const WideChar *labelText )
{
	if( listbox == nullptr || m_entry != nullptr || TheWindowManager == nullptr )
		return FALSE;

	// Parent to the listbox's parent, never to nullptr: a gadget with no parent becomes a top-level
	// window that owns itself, so its GEM_/GBM_ messages would go nowhere and the screen's layout
	// would not destroy it.
	GameWindow *listParent = listbox->winGetParent();
	if( listParent == nullptr )
		return FALSE;

	Int lx = 0, ly = 0, lw = 0, lh = 0;
	listbox->winGetPosition( &lx, &ly );		// parent-relative device pixels
	listbox->winGetSize( &lw, &lh );

	// Int-only arithmetic throughout: TheDisplay->getWidth() is UnsignedInt and Lib/BaseType.h
	// replaces the min/max macros with template<typename T> T max(T,T), so mixing Int and
	// UnsignedInt is a hard compile error. CodeGuiScale*/CodeGuiAtLeast are Int-only for this.
	const Int fieldH = CodeGuiAtLeast( CodeGuiScaleY( 21 ), 21 );
	const Int gap    = CodeGuiAtLeast( CodeGuiScaleY( 4 ), 3 );
	const Int clearW = CodeGuiAtLeast( CodeGuiScaleX( 21 ), 21 );

	// Refuse to shrink the list into uselessness on a tiny backbuffer.
	if( lh <= ( fieldH + gap ) * 2 || lw <= ( clearW + gap ) * 3 )
		return FALSE;

	Int entryX = lx;
	Int entryW = lw - clearW - gap;

	// A bare box with no caption reads as broken, so label it when there is room to spare.
	//
	// GeneralsX @feature Claude 27/07/2026 Caption and typed text are BOLD by request. Note this is
	// heavier than the stock lobby labels it sits near: "GUI:Players" carries
	// HEADERTEMPLATE = "MinorTitle", and HeaderTemplate.ini defines MinorTitle as Arial 14 Bold=No
	// (the template overrides the window's own FONT= line in GameWindowManagerScript.cpp). Its heft
	// on screen is point size plus the black text border, not weight. Flip the two flags below to
	// FALSE to sit flush with the stock look.
	const Int labelW = CodeGuiAtLeast( CodeGuiScaleX( 48 ), 40 );
	if( entryW - labelW - gap >= CodeGuiAtLeast( CodeGuiScaleX( 120 ), 80 ) )
	{
		m_label = CodeGuiLabel( listParent, lx, ly, labelW, fieldH, nullptr, labelText, FALSE,
														CODEGUI_SEARCH_LABEL_POINTS, TRUE );
		entryX += labelW + gap;
		entryW -= labelW + gap;
	}

	m_entry = CodeGuiTextEntry( listParent, entryX, ly, entryW, fieldH,
															entryIdName, CODEGUI_SEARCH_MAX_LEN,
															CODEGUI_SEARCH_ENTRY_POINTS, TRUE );
	if( m_entry == nullptr )
		return FALSE;

	m_onChanged = onChanged;

	// The iOS soft keyboard's backspace has not been proven to reach KEY_BACKSPACE, so give the
	// field a way to empty itself that only needs a tap.
	m_clear = CodeGuiButton( listParent, lx + lw - clearW, ly, clearW, fieldH,
													 clearIdName, L"X", L"Clear the search" );

	// Take the row's space out of the listbox. winSetSize fires GGM_RESIZED, which
	// GadgetListBoxSystem handles by repositioning the up/down buttons and the slider and
	// recomputing the column widths, so a runtime resize is fully supported.
	listbox->winSetPosition( lx, ly + fieldH + gap );
	listbox->winSetSize( lw, lh - fieldH - gap );

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiSearchField::handleSystemMsg( UnsignedInt msg, GameWindow *control )
{
	if( control == nullptr )
		return FALSE;

	switch( msg )
	{

		// GadgetTextEntryInput sends GEM_UPDATE_TEXT to the entry's OWNER on every accepted character
		// and every backspace, and GEM_EDIT_DONE on Return. gogoGadgetTextEntry set that owner to the
		// window we passed as parent, so both land on the screen's existing system func with no .wnd
		// change and no FunctionLexicon row.
		case GEM_UPDATE_TEXT:
		case GEM_EDIT_DONE:
		{
			if( m_entry == nullptr || control != m_entry )
				return FALSE;

			m_filter = GadgetTextEntryGetText( m_entry );
			break;
		}

		case GBM_SELECTED:
		{
			if( m_clear == nullptr || control != m_clear )
				return FALSE;

			if( m_entry )
			{
				// GEM_SET_TEXT does not echo GEM_UPDATE_TEXT back at the owner, so this cannot
				// re-enter; the callback below is the single notification.
				GadgetTextEntrySetText( m_entry, UnicodeString::TheEmptyString );

				// Hand focus back to the field. GadgetPushButtonInput never takes focus itself (the
				// winSetFocus calls in it are commented out), but re-asserting it is what keeps
				// SDL3GameEngine::updateTextInputState holding the iOS keyboard up.
				TheWindowManager->winSetFocus( m_entry );
			}
			m_filter.clear();
			break;
		}

		default:
			return FALSE;

	}

	if( m_onChanged )
		m_onChanged( this );

	return TRUE;
}
