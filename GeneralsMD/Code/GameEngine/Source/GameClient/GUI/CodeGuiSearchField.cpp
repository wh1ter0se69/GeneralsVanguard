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
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "Common/GlobalData.h"
#include "Common/UserPreferences.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GXMapSources.h"
#include "GameClient/GXMatchChat.h"			// gxMatchWideFromUtf8: the catalogue's titles are UTF-8
#include "GameClient/GXUserDataDir.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/NetworkDefs.h"			// MAX_SLOTS: the most players a listed map has
#include "WWLib/gxproductdir.h"				// GeneralsVanguard or GeneralsX, the launcher's own rule

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#endif

//-------------------------------------------------------------------------------------------------
static const Short CODEGUI_SEARCH_MAX_LEN = 32;

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
// See the note on the declaration: restoring a search needs
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
	// The row height and the X button's width come from ONE authored 21x21 square, mapped as the parser
	// maps EA's own X (PopupJoinGame.wnd:ButtonCancel): stretched per axis in the default layout like
	// every retail control, square in the uniform one. The row also never gets shorter than the entry's
	// own face needs - the image draw starts the text 5 px below the top edge - and the X keeps the
	// square's mapped aspect at whatever height the row ends up.
	Int squareX = 0, squareY = 0, squareW = 0, squareH = 0;
	CodeGuiMapAuthoredRect( 0, 0, 21, 21, &squareX, &squareY, &squareW, &squareH );
	GameFont *entryFont = CodeGuiTemplateFont( "TextEntry" );
	const Int entryTextH = ( entryFont != nullptr ) ? TheWindowManager->winFontHeight( entryFont ) : 0;
	const Int fieldH = CodeGuiAtLeast( CodeGuiAtLeast( squareH, 21 ), entryTextH + 2 * 5 );
	const Int gap    = CodeGuiAtLeast( CodeGuiScaleY( 4 ), 3 );
	const Int clearW = CodeGuiAtLeast( CodeGuiAtLeast( squareW, 21 ), ( fieldH * squareW ) / CodeGuiAtLeast( squareH, 1 ) );

	// Refuse to shrink the list into uselessness on a tiny backbuffer.
	if( lh <= ( fieldH + gap ) * 2 || lw <= ( clearW + gap ) * 3 )
		return FALSE;

	Int entryX = lx;
	Int entryW = lw - clearW - gap;

	// A bare box with no caption reads as broken, so label it when there is room to spare. Wide
	// enough for the caption in its own face plus the static text's 7 px margin at each end.
	Int labelW = CodeGuiAtLeast( CodeGuiScaleX( 48 ), 40 );
	GameFont *labelFont = CodeGuiTemplateFont( "LabelRegular" );
	if( labelFont != nullptr && labelText != nullptr && TheDisplayStringManager != nullptr )
	{
		DisplayString *probe = TheDisplayStringManager->newDisplayString();
		if( probe != nullptr )
		{
			Int textW = 0, textH = 0;
			probe->setFont( labelFont );
			probe->setText( UnicodeString( labelText ) );
			probe->getSize( &textW, &textH );
			TheDisplayStringManager->freeDisplayString( probe );
			labelW = CodeGuiAtLeast( labelW, textW + 2 * 7 );
		}
	}
	if( entryW - labelW - gap >= CodeGuiAtLeast( CodeGuiScaleX( 120 ), 80 ) )
	{
		m_label = CodeGuiRetailLabel( listParent, lx, ly, labelW, fieldH, nullptr, labelText );
		entryX += labelW + gap;
		entryW -= labelW + gap;
	}

	m_entry = CodeGuiRetailTextEntry( listParent, entryX, ly, entryW, fieldH,
																		entryIdName, CODEGUI_SEARCH_MAX_LEN );
	if( m_entry == nullptr )
		return FALSE;

	m_onChanged = onChanged;

	// The iOS soft keyboard's backspace has not been proven to reach KEY_BACKSPACE, so give the
	// field a way to empty itself that only needs a tap.
	m_clear = CodeGuiRetailCloseButton( listParent, lx + lw - clearW, ly, clearW, fieldH,
																			clearIdName, L"Clear the search" );

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

///////////////////////////////////////////////////////////////////////////////////////////////////
// CodeGuiMapFilters
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
// <GeneralsXPatch>, resolved as StdBIGFileSystem / Win32BIGFileSystem resolvePatchOverlayDirectory resolve it:
// GENERALSX_PATCH_PATH, else the folder beside the executable. The launcher copies <it>/ShippedMaps into <userdata>/Maps.
static std::filesystem::path codeGuiPatchOverlayDir( void )
{
	const char *env = getenv( "GENERALSX_PATCH_PATH" );
	if( env != nullptr && *env != '\0' )
		return std::filesystem::path( env );
#if defined(_WIN32)
	wchar_t exe[ 4096 ];
	const DWORD n = GetModuleFileNameW( nullptr, exe, 4096 );
	if( n == 0 || n >= 4096 )
		return std::filesystem::path();
	return std::filesystem::path( exe ).parent_path() / "GeneralsXPatch";
#elif defined(__APPLE__)
	char exe[ 4096 ];
	uint32_t size = (uint32_t)sizeof( exe );
	if( _NSGetExecutablePath( exe, &size ) != 0 )
		return std::filesystem::path();
	return std::filesystem::path( exe ).parent_path() / "GeneralsXPatch";
#else
	std::error_code ec;
	const std::filesystem::path exe = std::filesystem::read_symlink( "/proc/self/exe", ec );
	if( ec )
		return std::filesystem::path();
	return exe.parent_path() / "GeneralsXPatch";
#endif
}

//-------------------------------------------------------------------------------------------------
// The Rust launcher's state folder (gxio state.rs state_dir): <GX_USERDATA_DIR>/launcher on a test profile, else
// %APPDATA% / Application Support / $XDG_DATA_HOME or ~/.local/share, plus GeneralsVanguard or GeneralsX. iOS has none.
static std::filesystem::path codeGuiLauncherStateDir( void )
{
#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	return std::filesystem::path();
#else
	std::string ovr;
	if( gxUserDataDirOverride( ovr ) )
		return std::filesystem::path( ovr ) / "launcher";
#if defined(_WIN32)
	const wchar_t *appData = _wgetenv( L"APPDATA" );
	if( appData == nullptr || *appData == L'\0' )
		return std::filesystem::path();
	const std::filesystem::path base( appData );
#elif defined(__APPLE__)
	const char *home = getenv( "HOME" );
	if( home == nullptr || *home == '\0' )
		return std::filesystem::path();
	const std::filesystem::path base = std::filesystem::path( home ) / "Library" / "Application Support";
#else
	std::filesystem::path base;
	const char *xdg = getenv( "XDG_DATA_HOME" );
	if( xdg != nullptr && xdg[ 0 ] == '/' )
		base = xdg;
	else if( const char *home = getenv( "HOME" ) )
		base = std::filesystem::path( home ) / ".local" / "share";
	else
		return std::filesystem::path();
#endif
	return base / gxProductDirName( base );
#endif
}

//-------------------------------------------------------------------------------------------------
static const char *const MAP_FILTERS_PREFS = "MapFilters.ini";
static const Int MAP_FILTER_CATEGORY_DATA = 100;	///< Source item data: 100 + index into GXMapSources::categories
static const Int MAP_FILTER_TITLE_MAX = 24;				///< catalogue titles may be 64 bytes; the drop-down is narrow

//-------------------------------------------------------------------------------------------------
CodeGuiMapFilters::CodeGuiMapFilters( const char *prefsKey )
	: m_prefsKey( prefsKey ),
		m_onChanged( nullptr ),
		m_players( nullptr ),
		m_source( nullptr ),
		m_sort( nullptr ),
		m_emptyLabel( nullptr ),
		m_emptyClear( nullptr ),
		m_listbox( nullptr ),
		m_quiet( FALSE ),
		m_userMaps( FALSE ),
		m_wantPlayers( 0 ),
		m_wantSource( SOURCE_ANY ),
		m_sortOrder( MAPLIST_SORT_PLAYERS ),
		m_sources( nullptr )
{
}

//-------------------------------------------------------------------------------------------------
CodeGuiMapFilters::~CodeGuiMapFilters()
{
	delete m_sources;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::reset( void )
{
	m_search.reset();
	m_players = m_source = m_sort = nullptr;
	m_emptyLabel = m_emptyClear = nullptr;
	m_listbox = nullptr;
	m_keep.clear();
	m_quiet = FALSE;
	delete m_sources;
	m_sources = nullptr;
	// A screen that never calls create() (a LAN client) must list exactly what it always did; create() reloads the
	// remembered choice from MapFilters.ini.
	m_wantPlayers = 0;
	m_wantSource = SOURCE_ANY;
	m_wantCategory.clear();
	m_sortOrder = MAPLIST_SORT_PLAYERS;
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiMapFilters::create( GameWindow *listbox, GameWindow *comboTemplate, CodeGuiSearchFieldChangedFunc onChanged,
																const char *idPrefix, Bool userMaps, const AsciiString &mapToShow )
{
	if( listbox == nullptr || TheWindowManager == nullptr || m_search.exists() || idPrefix == nullptr )
		return FALSE;
	GameWindow *parent = listbox->winGetParent();
	if( parent == nullptr )
		return FALSE;

	m_onChanged = onChanged;
	m_listbox = listbox;
	m_keep.clear();
	m_userMaps = userMaps;
	m_wantPlayers = 0;
	m_wantSource = SOURCE_ANY;
	m_wantCategory.clear();
	m_sortOrder = MAPLIST_SORT_PLAYERS;

	Int lx = 0, ly = 0, lw = 0, lh = 0;
	listbox->winGetPosition( &lx, &ly );
	listbox->winGetSize( &lw, &lh );
	const Int gap = CodeGuiAtLeast( CodeGuiScaleY( 4 ), 3 );

	// The filter row: as tall as the authored box it copies.
	const Int minComboW = CodeGuiAtLeast( CodeGuiScaleX( 60 ), 48 );
	Int rowH = 0;
	if( comboTemplate != nullptr && BitIsSet( comboTemplate->winGetStyle(), GWS_COMBO_BOX ) )
	{
		Int tw = 0;
		comboTemplate->winGetSize( &tw, &rowH );
		if( rowH < 0 )
			rowH = 0;
	}

	// It sits on the title band, right of the "Select Map" caption (the layout's one window named just idPrefix), so the
	// list keeps its height: from past the caption's text to ACCEPT's right edge, centred on that text, never below the
	// caption's own rect nor into the list. Each retail layout puts the caption above the Official / Unofficial pair.
	Int rowX = lx, rowY = ly, rowW = lw;
	Bool onTitle = FALSE;
	GameWindow *title = TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey( idPrefix ) );
	if( rowH > 0 && title != nullptr && title->winGetParent() == parent && BitIsSet( title->winGetStyle(), GWS_STATIC_TEXT ) )
	{
		const TextData *td = (const TextData *)title->winGetUserData();
		Int tx = 0, ty = 0, tw = 0, th = 0, textW = 0, textH = 0;
		title->winGetPosition( &tx, &ty );
		title->winGetSize( &tw, &th );
		if( td != nullptr && td->text != nullptr )
			td->text->getSize( &textW, &textH );
		const Int textRight = ( td != nullptr && td->centered ) ? tx + ( tw + textW ) / 2 : tx + ( td ? td->leftMargin : 0 ) + textW;
		const Int textMidY = ( td != nullptr && td->centeredVertically ) ? ty + th / 2 : ty + ( td ? td->topMargin : 0 ) + textH / 2;

		AsciiString okId;
		okId.format( "%sButtonOK", idPrefix );
		GameWindow *ok = TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey( okId ) );
		Int right = lx + lw;
		if( ok != nullptr && ok->winGetParent() == parent )
		{
			Int ox = 0, oy = 0, ow = 0, oh = 0;
			ok->winGetPosition( &ox, &oy );
			ok->winGetSize( &ow, &oh );
			right = CodeGuiAtLeast( right, ox + ow );
		}

		const Int left = textRight + CodeGuiAtLeast( CodeGuiScaleX( 12 ), 8 );
		Int top = textMidY - rowH / 2;
		if( top + rowH > ty + th )
			top = ty + th - rowH;
		if( textW > 0 && top >= 0 && top + rowH <= ly && right - left >= 3 * minComboW + 2 * gap )
		{
			rowX = left;
			rowY = top;
			rowW = right - left;
			onTitle = TRUE;
		}
	}

	// No such band: the row goes above the search box instead, where the list keeps most of its height.
	if( rowH > 0 && !onTitle )
	{
		if( lh <= ( rowH + gap ) * 4 || lw <= 3 * minComboW + 2 * gap )
			rowH = 0;
		else
		{
			listbox->winSetPosition( lx, ly + rowH + gap );
			listbox->winSetSize( lw, lh - rowH - gap );
		}
	}

	AsciiString entryId, clearId;
	entryId.format( "%sTextEntryMapSearch", idPrefix );
	clearId.format( "%sButtonMapSearchClear", idPrefix );
	m_search.create( listbox, onChanged, entryId.str(), clearId.str() );

	// "No maps match." and its way out, centred in the list as it now stands. Built BEFORE the drop-downs: winCreate
	// prepends and the head of the child list paints last, so an open drop-down must be created after anything it covers.
	Int fx = 0, fy = 0, fw = 0, fh = 0;
	listbox->winGetPosition( &fx, &fy );
	listbox->winGetSize( &fw, &fh );
	Int bx = 0, by = 0, bw = 0, bh = 0;
	CodeGuiMapAuthoredRect( 0, 0, 128, 26, &bx, &by, &bw, &bh );		// the size of the screen's own OK / Back
	bw = CodeGuiAtLeast( bw, 96 );
	bh = CodeGuiAtLeast( bh, 22 );
	const Int labelH = bh;
	if( fh > labelH + gap + bh && fw > bw )
	{
		const Int top = fy + ( fh - ( labelH + gap + bh ) ) / 2;
		AsciiString id;
		id.format( "%sStaticTextNoMapsMatch", idPrefix );
		m_emptyLabel = CodeGuiRetailLabel( parent, fx, top, fw, labelH, id.str(), L"No maps match." );
		if( m_emptyLabel != nullptr )
		{
			TextData *td = (TextData *)m_emptyLabel->winGetUserData();
			if( td != nullptr )
				td->centered = TRUE;
			m_emptyLabel->winSetStatus( WIN_STATUS_NO_INPUT );
			m_emptyLabel->winHide( TRUE );
		}
		id.format( "%sButtonClearMapFilters", idPrefix );
		m_emptyClear = CodeGuiRetailButton( parent, fx + ( fw - bw ) / 2, top + labelH + gap, bw, bh, id.str(),
																				L"Clear filters", L"Show every map again" );
		if( m_emptyClear != nullptr )
			m_emptyClear->winHide( TRUE );
	}

	m_sources = new GXMapSources( gxMapSourcesLoad( codeGuiLauncherStateDir(), codeGuiPatchOverlayDir() / "ShippedMaps" ) );
	m_userMapDir = TheGlobalData->getPath_UserData();
	if( TheMapCache != nullptr )
		m_userMapDir.concat( TheMapCache->getMapDir() );
	m_userMapDir.toLower();

	if( rowH > 0 )
	{
		const Int third = ( rowW - 2 * gap ) / 3;
		AsciiString id;
		id.format( "%sComboBoxMapPlayers", idPrefix );
		m_players = CodeGuiComboBoxLike( comboTemplate, parent, id.str(), rowX, rowY, third, rowH, 64 );
		id.format( "%sComboBoxMapSource", idPrefix );
		m_source = CodeGuiComboBoxLike( comboTemplate, parent, id.str(), rowX + third + gap, rowY, third, rowH, 64 );
		id.format( "%sComboBoxMapSort", idPrefix );
		m_sort = CodeGuiComboBoxLike( comboTemplate, parent, id.str(), rowX + 2 * ( third + gap ), rowY,
																	rowW - 2 * ( third + gap ), rowH, 8 );

		if( m_players != nullptr && m_source != nullptr && m_sort != nullptr )
		{
			loadPrefs();

			// Never open on a list that hides the map the game is set to: drop the remembered filters for this opening
			// (not from the file - they come back the next time they would not hide it). Sort hides nothing.
			const MapMetaData *md = ( TheMapCache != nullptr && mapToShow.isNotEmpty() ) ? TheMapCache->findMap( mapToShow ) : nullptr;
			if( md != nullptr )
			{
				AsciiString lowerMap = mapToShow;
				lowerMap.toLower();
				if( !accepts( lowerMap, *md ) )
				{
					m_wantPlayers = 0;
					m_wantSource = SOURCE_ANY;
					m_wantCategory.clear();
				}
			}

			fillPlayers();
			fillSource();
			fillSort();
		}
		else
		{
			// Half a row is worse than none; the windows die with the layout.
			if( m_players ) m_players->winHide( TRUE );
			if( m_source ) m_source->winHide( TRUE );
			if( m_sort ) m_sort->winHide( TRUE );
			m_players = m_source = m_sort = nullptr;
		}
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
CodeGuiMapFilters::Source CodeGuiMapFilters::classify( const AsciiString &lowerPath, AsciiString *category ) const
{
	if( m_sources == nullptr )
		return SOURCE_MINE;
	const std::string folder = gxMapSourcesFolderOf( lowerPath.str(), m_userMapDir.str() );
	if( folder.empty() )
		return SOURCE_ANY;		// not a user map
	if( m_sources->vanguard.count( folder ) )
		return SOURCE_VANGUARD;
	const std::map<std::string, std::string>::const_iterator d = m_sources->downloaded.find( folder );
	if( d != m_sources->downloaded.end() )
	{
		if( category != nullptr )
			*category = d->second.c_str();
		return SOURCE_DOWNLOADED;
	}
	return SOURCE_MINE;
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiMapFilters::accepts( const AsciiString &lowerPath, const MapMetaData &md ) const
{
	if( m_wantPlayers > 0 && md.m_numPlayers != m_wantPlayers )
		return FALSE;
	if( !m_userMaps || m_wantSource == SOURCE_ANY )
		return TRUE;

	AsciiString category;
	const Source source = classify( lowerPath, &category );
	switch( m_wantSource )
	{
		case SOURCE_CATEGORY:		return source == SOURCE_DOWNLOADED && category == m_wantCategory;
		default:								return source == m_wantSource;
	}
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiMapFilters::acceptRow( const AsciiString &mapPath, const MapMetaData &md, void *ctx )
{
	return static_cast<const CodeGuiMapFilters *>( ctx )->accepts( mapPath, md );
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiMapFilters::isFiltering( void ) const
{
	return m_wantPlayers > 0 || ( m_userMaps && m_wantSource != SOURCE_ANY ) || !m_search.getFilter().isEmpty();
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::selectByData( GameWindow *combo, Int data )
{
	if( combo == nullptr )
		return;
	Int pick = 0;
	const Int n = GadgetComboBoxGetLength( combo );
	for( Int i = 0; i < n; ++i )
	{
		if( (Int)(intptr_t)GadgetComboBoxGetItemData( combo, i ) == data )
		{
			pick = i;
			break;
		}
	}
	m_quiet = TRUE;		// SetSelectedPos sends GCM_SELECTED straight back to the owner
	GadgetComboBoxSetSelectedPos( combo, pick );
	m_quiet = FALSE;
}

//-------------------------------------------------------------------------------------------------
// Only counts a multiplayer map on the radio's side (Official / Unofficial) has, smallest first: a choice that always
// lists nothing is not offered. Solo maps (campaign, 1 player) stay under "Any player count" where a list shows them.
void CodeGuiMapFilters::fillPlayers( void )
{
	if( m_players == nullptr )
		return;
	std::set<Int> counts;
	if( TheMapCache != nullptr )
	{
		for( MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it )
		{
			const MapMetaData &md = it->second;
			if( !md.m_isMultiplayer || md.m_numPlayers < 1 || md.m_numPlayers > MAX_SLOTS || md.m_displayName.isEmpty() )
				continue;		// populate lists 1..MAX_SLOTS and nothing else
			const Bool userMap = !gxMapSourcesFolderOf( it->first.str(), m_userMapDir.str() ).empty();
			if( userMap == m_userMaps )
				counts.insert( md.m_numPlayers );
		}
	}
	if( m_wantPlayers > 0 && !counts.count( m_wantPlayers ) )
		m_wantPlayers = 0;

	const Color color = m_players->winGetEnabledTextColor();
	m_quiet = TRUE;
	GadgetComboBoxReset( m_players );
	GadgetComboBoxSetItemData( m_players, GadgetComboBoxAddEntry( m_players, UnicodeString( L"Any player count" ), color ), (void *)(intptr_t)0 );
	for( std::set<Int>::const_iterator c = counts.begin(); c != counts.end(); ++c )
	{
		UnicodeString text;
		text.format( ( *c == 1 ) ? L"%d player" : L"%d players", *c );
		GadgetComboBoxSetItemData( m_players, GadgetComboBoxAddEntry( m_players, text, color ), (void *)(intptr_t)*c );
	}
	m_quiet = FALSE;
	selectByData( m_players, m_wantPlayers );
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::fillSource( void )
{
	if( m_source == nullptr )
		return;
	const Color color = m_source->winGetEnabledTextColor();
	m_quiet = TRUE;
	GadgetComboBoxReset( m_source );
	m_quiet = FALSE;

	if( !m_userMaps )
	{
		m_quiet = TRUE;
		GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, UnicodeString( L"Official maps" ), color ), (void *)(intptr_t)SOURCE_ANY );
		m_quiet = FALSE;
		selectByData( m_source, SOURCE_ANY );
		m_source->winEnable( FALSE );
		return;
	}
	m_source->winEnable( TRUE );

	// What the user maps on this machine hold, so no item is a dead end.
	Bool haveVanguard = FALSE, haveDownloaded = FALSE, haveMine = FALSE;
	std::set<std::string> haveCategory;
	if( TheMapCache != nullptr )
	{
		for( MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it )
		{
			AsciiString category;
			switch( classify( it->first, &category ) )
			{
				case SOURCE_VANGUARD:		haveVanguard = TRUE; break;
				case SOURCE_DOWNLOADED:	haveDownloaded = TRUE; haveCategory.insert( category.str() ); break;
				case SOURCE_MINE:				haveMine = TRUE; break;
				default:								break;
			}
		}
	}

	Int wantData = SOURCE_ANY;
	m_quiet = TRUE;
	GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, UnicodeString( L"Any source" ), color ), (void *)(intptr_t)SOURCE_ANY );
	if( haveVanguard )
	{
		GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, UnicodeString( L"Generals Vanguard" ), color ), (void *)(intptr_t)SOURCE_VANGUARD );
		if( m_wantSource == SOURCE_VANGUARD )
			wantData = SOURCE_VANGUARD;
	}
	if( haveDownloaded )
	{
		GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, UnicodeString( L"Downloaded" ), color ), (void *)(intptr_t)SOURCE_DOWNLOADED );
		if( m_wantSource == SOURCE_DOWNLOADED )
			wantData = SOURCE_DOWNLOADED;
		// Each launcher category, indented under Downloaded, in the catalogue's order.
		for( size_t i = 0; m_sources != nullptr && i < m_sources->categories.size(); ++i )
		{
			if( !haveCategory.count( m_sources->categories[ i ].first ) )
				continue;
			UnicodeString title = gxMatchWideFromUtf8( m_sources->categories[ i ].second.c_str() );
			if( title.getLength() > MAP_FILTER_TITLE_MAX )
			{
				UnicodeString cut;
				for( Int k = 0; k < MAP_FILTER_TITLE_MAX - 3; ++k )
					cut.concat( title.getCharAt( k ) );
				cut.concat( L"..." );
				title = cut;
			}
			UnicodeString text( L"   " );
			text.concat( title );
			const Int data = MAP_FILTER_CATEGORY_DATA + (Int)i;
			GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, text, color ), (void *)(intptr_t)data );
			if( m_wantSource == SOURCE_CATEGORY && m_wantCategory == m_sources->categories[ i ].first.c_str() )
				wantData = data;
		}
	}
	if( haveMine )
	{
		GadgetComboBoxSetItemData( m_source, GadgetComboBoxAddEntry( m_source, UnicodeString( L"My maps" ), color ), (void *)(intptr_t)SOURCE_MINE );
		if( m_wantSource == SOURCE_MINE )
			wantData = SOURCE_MINE;
	}
	m_quiet = FALSE;

	// A remembered source this machine no longer has falls back to Any.
	if( wantData == SOURCE_ANY )
	{
		m_wantSource = SOURCE_ANY;
		m_wantCategory.clear();
	}
	selectByData( m_source, wantData );
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::fillSort( void )
{
	if( m_sort == nullptr )
		return;
	const Color color = m_sort->winGetEnabledTextColor();
	m_quiet = TRUE;
	GadgetComboBoxReset( m_sort );
	GadgetComboBoxSetItemData( m_sort, GadgetComboBoxAddEntry( m_sort, UnicodeString( L"Sort by players" ), color ), (void *)(intptr_t)MAPLIST_SORT_PLAYERS );
	GadgetComboBoxSetItemData( m_sort, GadgetComboBoxAddEntry( m_sort, UnicodeString( L"Sort by name" ), color ), (void *)(intptr_t)MAPLIST_SORT_NAME );
	GadgetComboBoxSetItemData( m_sort, GadgetComboBoxAddEntry( m_sort, UnicodeString( L"Sort by newest" ), color ), (void *)(intptr_t)MAPLIST_SORT_NEWEST );
	m_quiet = FALSE;
	selectByData( m_sort, m_sortOrder );
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::setUserMaps( Bool userMaps )
{
	m_userMaps = userMaps;
	fillPlayers();
	fillSource();
}

//-------------------------------------------------------------------------------------------------
static Int codeGuiComboData( GameWindow *combo, Int fallback )
{
	Int pos = -1;
	GadgetComboBoxGetSelectedPos( combo, &pos );
	return ( pos >= 0 ) ? (Int)(intptr_t)GadgetComboBoxGetItemData( combo, pos ) : fallback;
}

void CodeGuiMapFilters::readCombos( void )
{
	m_wantPlayers = CodeGuiAtLeast( codeGuiComboData( m_players, 0 ), 0 );

	const Int sort = codeGuiComboData( m_sort, MAPLIST_SORT_PLAYERS );
	m_sortOrder = ( sort == MAPLIST_SORT_NAME || sort == MAPLIST_SORT_NEWEST ) ? (MapListSort)sort : MAPLIST_SORT_PLAYERS;

	if( !m_userMaps )
		return;		// the Source box reads "Official maps"; the remembered source waits for the Unofficial radio
	const Int source = codeGuiComboData( m_source, SOURCE_ANY );
	m_wantCategory.clear();
	if( source >= MAP_FILTER_CATEGORY_DATA && m_sources != nullptr
			&& (size_t)( source - MAP_FILTER_CATEGORY_DATA ) < m_sources->categories.size() )
	{
		m_wantSource = SOURCE_CATEGORY;
		m_wantCategory = m_sources->categories[ source - MAP_FILTER_CATEGORY_DATA ].first.c_str();
	}
	else if( source == SOURCE_VANGUARD || source == SOURCE_DOWNLOADED || source == SOURCE_MINE )
		m_wantSource = (Source)source;
	else
		m_wantSource = SOURCE_ANY;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::loadPrefs( void )
{
	UserPreferences pref;
	pref.load( MAP_FILTERS_PREFS );
	AsciiString key;

	key.format( "%sPlayers", m_prefsKey );
	m_wantPlayers = CodeGuiAtLeast( pref.getInt( key, 0 ), 0 );

	key.format( "%sSort", m_prefsKey );
	const AsciiString sort = pref.getAsciiString( key, "players" );
	m_sortOrder = ( sort == "name" ) ? MAPLIST_SORT_NAME : ( sort == "newest" ) ? MAPLIST_SORT_NEWEST : MAPLIST_SORT_PLAYERS;

	key.format( "%sSource", m_prefsKey );
	const AsciiString source = pref.getAsciiString( key, "any" );
	m_wantCategory.clear();
	if( source == "vanguard" )						m_wantSource = SOURCE_VANGUARD;
	else if( source == "downloaded" )			m_wantSource = SOURCE_DOWNLOADED;
	else if( source == "mine" )						m_wantSource = SOURCE_MINE;
	else if( source.startsWith( "cat:" ) && source.getLength() > 4 )
	{
		m_wantSource = SOURCE_CATEGORY;
		m_wantCategory = source.str() + 4;
	}
	else																	m_wantSource = SOURCE_ANY;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::savePrefs( void ) const
{
	UserPreferences pref;
	pref.load( MAP_FILTERS_PREFS );		// keeps the other pickers' lines
	AsciiString key, value;

	key.format( "%sPlayers", m_prefsKey );
	pref.setInt( key, m_wantPlayers );

	key.format( "%sSort", m_prefsKey );
	pref[ key ] = ( m_sortOrder == MAPLIST_SORT_NAME ) ? "name" : ( m_sortOrder == MAPLIST_SORT_NEWEST ) ? "newest" : "players";

	key.format( "%sSource", m_prefsKey );
	switch( m_wantSource )
	{
		case SOURCE_VANGUARD:		value = "vanguard"; break;
		case SOURCE_DOWNLOADED:	value = "downloaded"; break;
		case SOURCE_MINE:				value = "mine"; break;
		case SOURCE_CATEGORY:		value.format( "cat:%s", m_wantCategory.str() ); break;
		default:								value = "any"; break;
	}
	pref[ key ] = value;
	pref.write();
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::clearFilters( void )
{
	m_wantPlayers = 0;
	m_wantSource = SOURCE_ANY;
	m_wantCategory.clear();
	selectByData( m_players, 0 );
	if( m_userMaps )
		selectByData( m_source, SOURCE_ANY );
	m_search.setFilter( UnicodeString::TheEmptyString );
	if( m_players != nullptr )
		savePrefs();
	if( m_onChanged )
		m_onChanged( &m_search );
}

//-------------------------------------------------------------------------------------------------
// The picked row as an owned copy, while its item data still points into the map cache: every path that repopulates
// arrives here first, before a radio's updateCache() can move the cache under the list.
void CodeGuiMapFilters::keepSelection( void )
{
	if( m_listbox == nullptr )
		return;
	Int selected = -1;
	GadgetListBoxGetSelected( m_listbox, &selected );
	const char *map = ( selected >= 0 ) ? (const char *)GadgetListBoxGetItemData( m_listbox, selected ) : nullptr;
	if( map != nullptr )
		m_keep = map;
}

//-------------------------------------------------------------------------------------------------
Bool CodeGuiMapFilters::handleSystemMsg( UnsignedInt msg, GameWindow *control )
{
	// Our own SetSelectedPos echoing back. It arrives inside setUserMaps, AFTER a radio's updateCache() may have freed
	// rows the list still points at, so it must not reach keepSelection (the radio click itself was captured before).
	if( msg == GCM_SELECTED && m_quiet && control != nullptr
			&& ( control == m_players || control == m_source || control == m_sort ) )
		return TRUE;

	if( msg == GEM_UPDATE_TEXT || msg == GEM_EDIT_DONE || msg == GBM_SELECTED || msg == GCM_SELECTED )
		keepSelection();

	if( m_search.handleSystemMsg( msg, control ) )
		return TRUE;
	if( control == nullptr )
		return FALSE;

	if( msg == GBM_SELECTED && control == m_emptyClear )
	{
		clearFilters();
		return TRUE;
	}

	if( msg == GCM_SELECTED && ( control == m_players || control == m_source || control == m_sort ) )
	{
		readCombos();
		savePrefs();
		if( m_onChanged )
			m_onChanged( &m_search );
		return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiMapFilters::updateEmptyState( GameWindow *listbox )
{
	const Bool show = listbox != nullptr && GadgetListBoxGetNumEntries( listbox ) == 0 && isFiltering();
	if( m_emptyLabel != nullptr )
		m_emptyLabel->winHide( !show );
	if( m_emptyClear != nullptr )
		m_emptyClear->winHide( !show );
}

//-------------------------------------------------------------------------------------------------
Int CodeGuiMapFilters::populate( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer,
																 const AsciiString &mapToSelect, Bool bothKinds )
{
	MapListFilter filter;
	filter.search = m_search.getFilter();
	filter.accept = acceptRow;
	filter.ctx = this;
	filter.sort = m_sortOrder;
	filter.prefer = m_keep;
	filter.bothKinds = bothKinds;
	m_keep.clear();
	const Int selected = populateMapListboxFiltered( listbox, useSystemMaps, isMultiplayer, mapToSelect, filter, TRUE );
	updateEmptyState( listbox );
	return selected;
}
