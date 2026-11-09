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

// FILE: CodeGui.h /////////////////////////////////////////////////////////////////////////////////
// Desc:   Build GUI screens in code, with no .wnd file and no art dependency
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 CodeGui: build GUI screens in code, no .wnd required.
**
** The .wnd editor is Windows-only and the stock layouts live inside WindowZH.big, so every new
** screen in this port has to be constructed at runtime. This module encapsulates the correct
** construction sequence so callers cannot repeat the mistakes that make a code-built screen
** render as an invisible, unexitable sheet:
**
**   - NEVER pass WIN_STATUS_IMAGE. gogoGadget* latches the IMAGE draw func at creation time,
**     and a mapped image that is missing from the shipped data then draws literally nothing.
**     Colours are the only safe path for new code.
**   - defaultVisual == TRUE so assignDefaultGadgetLook fills EVERY draw-data slot
**     (enabled/enabledSelected/disabled/disabledSelected/hilite/hiliteSelected plus all three
**     text colour pairs). Overriding slot 0 alone leaves a pressed button rendering
**     WIN_COLOR_UNDEFINED, which W3DGameWinDefaultDraw skips entirely.
**   - WinInstanceData::init() before EVERY gadget; typed data structs memset to 0 because
**     gogoGadget* memcpy's them wholesale into their heap copy.
**   - Callbacks are raw function pointers. FunctionLexicon is consulted ONLY by the .wnd parser
**     (GameWindowManagerScript.cpp); code-built screens register nothing.
**   - Never winSetSystemFunc() on a gadget: that orphans its GWM_DESTROY handler and leaks the
**     typed user data it allocated. Messages reach you via the OWNER, which gogoGadget* sets to
**     the parent you pass.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "GameClient/Color.h"
#include "GameClient/GameWindow.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////
class GameFont;
class GameWindow;
class WindowLayout;

//-------------------------------------------------------------------------------------------------
// Resolution helpers.
//
// Runtime-created windows get NO CREATIONRESOLUTION scaling; that happens only in
// GameWindowManagerScript::parseScreenRect. These map the 800x600 authoring space the stock
// .wnd files use onto the real backbuffer.
//
// NOTE the return type. TheDisplay->getWidth() is UnsignedInt, and Core/Libraries/Include/Lib/
// BaseType.h #undef's the min/max macros and replaces them with template<typename T> T max(T,T),
// so max( 16, TheDisplay->getHeight() * 22 / 600 ) fails template deduction ('int' vs
// 'unsigned int'). Everything below is Int so callers never hit that.
//-------------------------------------------------------------------------------------------------
Int CodeGuiScreenW( void );										///< (Int)TheDisplay->getWidth(), 1 if TheDisplay is null
Int CodeGuiScreenH( void );										///< (Int)TheDisplay->getHeight(), 1 if TheDisplay is null
Int CodeGuiScaleX( Int authoring800 );				///< authoring x/width -> device
Int CodeGuiScaleY( Int authoring600 );				///< authoring y/height -> device
Int CodeGuiAtLeast( Int value, Int floorValue );	///< Int-only max(), avoids the template collision

//-------------------------------------------------------------------------------------------------
// Palette. One place to retheme every code-built screen.
//-------------------------------------------------------------------------------------------------
struct CodeGuiTheme
{
	Color panelBg;
	Color panelBorder;
	Color buttonBg;
	Color buttonBorder;
	Color buttonHotBg;
	Color buttonHotBorder;
	Color buttonDownBg;
	Color buttonDownBorder;
	Color buttonDisabledBg;
	Color buttonDisabledBorder;
	Color fieldBg;
	Color fieldBorder;
	Color textNormal;
	Color textShadow;
	Color textDisabled;
	Color accent;
};
const CodeGuiTheme &CodeGuiGetTheme( void );

//-------------------------------------------------------------------------------------------------
// Fonts.
//
// Every code-built control draws in Arial, because that is the only face the port can rely on
// (scripts/build/ios/stage-fonts.sh installs Liberation Sans as fonts/arial.ttf) - except the Debug
// overlay's own windows, which draw in Tahoma Bold where the platform has it (26/09/2026, see
// codeGuiInDebugOverlay in CodeGui.cpp).
//
// BOLD IS A FAMILY, NOT A FLAG, on every platform this port ships to. The Bool that
// GameWindowManager::winFindFont / FontLibrary::getFont / WW3DAssetManager::Get_FontChars pass
// around only reaches FW_BOLD on the Win32 GDI path (render2dsentence.cpp,
// FontCharsClass::Create_GDI_Font). The FreeType path used on macOS/iOS/Linux is
// Create_Freetype_Font( font_name ) — it never looks at IsBold at all, so
// winFindFont( "Arial", 12, TRUE ) yields a SECOND GameFont with byte-identical glyphs. The only
// thing that changes the weight is the family name handed to the font locator, and the correct
// spelling of that name differs per platform, so it lives in one place: codeGuiFontFamily in CodeGui.cpp.

//-------------------------------------------------------------------------------------------------
// Control factories.
//
// `idName` is hashed with TheNameKeyGenerator->nameToKey() and installed with winSetWindowId().
// Always pass one for anything you intend to look up: an id-less window gets id 0 ==
// NAMEKEY_INVALID, and winGetWindowFromId( nullptr, 0 ) would then match it from anywhere in the
// process. Pass nullptr only for decorative controls you never address again.
//
// Every child's OWNER is `parent`, so all of a screen's GBM_/GLM_/GSM_ messages arrive at the
// parent's single system func. Pass nullptr for `parent` only for a screen ROOT.
//
// All coordinates are device pixels and PARENT-RELATIVE. Run authoring-space numbers through
// CodeGuiScaleX / CodeGuiScaleY first.
//-------------------------------------------------------------------------------------------------

/// Plain (non-gadget) container. Pass parent==nullptr for a top-level screen root.
/// `extraStatus` is OR'd into the status word: WIN_STATUS_ABOVE for an overlay that must beat the
/// in-game control bar, WIN_STATUS_SEE_THRU for an invisible grouping container.
GameWindow *CodeGuiPanel( GameWindow *parent,
													Int x, Int y, Int width, Int height,
													GameWinSystemFunc systemFunc,
													const char *idName,
													UnsignedInt extraStatus = 0 );

GameWindow *CodeGuiButton( GameWindow *parent,
													 Int x, Int y, Int width, Int height,
													 const char *idName,
													 const WideChar *label,
													 const WideChar *tooltip = nullptr );

GameWindow *CodeGuiLabel( GameWindow *parent,
													Int x, Int y, Int width, Int height,
													const char *idName,
													const WideChar *text,
													Bool centered = FALSE,
													Int pointSize = 12,
													Bool bold = FALSE );

GameWindow *CodeGuiCheckbox( GameWindow *parent,
														 Int x, Int y, Int width, Int height,
														 const char *idName,
														 const WideChar *label,
														 Bool checked = FALSE );

GameWindow *CodeGuiTextEntry( GameWindow *parent,
															Int x, Int y, Int width, Int height,
															const char *idName,
															Short maxTextLen,
															Int pointSize = 12,
															Bool bold = FALSE );

GameWindow *CodeGuiHSlider( GameWindow *parent,
														Int x, Int y, Int width, Int height,
														const char *idName,
														Int minVal, Int maxVal );

/// Scrolling listbox. `listLength` is the row capacity, not the pixel height; grow it later with
/// GadgetListBoxSetListLength. The scrollbar children are the one place the engine force-ORs
/// WIN_STATUS_IMAGE on our behalf (GadgetListboxCreateScrollbar), so this factory strips it back
/// off and rebinds the colour draw funcs — otherwise the arrows and thumb are invisible whenever
/// the VSliderLarge* mapped images are absent from the shipped data.
GameWindow *CodeGuiListbox( GameWindow *parent,
														Int x, Int y, Int width, Int height,
														const char *idName,
														Int listLength,
														Bool multiSelect = FALSE,
														Bool scrollBar = TRUE,
														Int pointSize = 12 );

/// Drop-down / combo box. `maxDisplay` is a ROW COUNT, not a pixel height: it is how many rows the
/// OPEN list shows before the scrollbar appears, and GadgetComboBoxSystem recomputes the open list's
/// pixel height from the font at the moment it opens. `maxChars` is the edit field's capacity, and
/// `isEditable` decides whether the player may type into the box or only pick from the list.
///
/// The ComboBoxData handed to gogoGadgetComboBox must carry a HEAP EntryData and ListboxData: that
/// factory passes both straight into gogoGadgetTextEntry / gogoGadgetListBox and then deletes them,
/// re-pointing the fields at the sub-gadgets' own copies. Stack structs would be passed to delete.
/// This factory owns that detail so callers cannot get it wrong.
GameWindow *CodeGuiComboBox( GameWindow *parent,
														 Int x, Int y, Int width, Int height,
														 const char *idName,
														 Int maxDisplay,
														 Int maxChars,
														 Bool isEditable );

//-------------------------------------------------------------------------------------------------
// Per-index control columns authored in a .wnd.
//
// A stock lobby screen hard-codes one WINDOW block per row - "File.wnd:ComboBoxPlayer0" through
// "...7" - while the C++ that binds them loops to a constant. The moment that constant grows past
// what the .wnd contains, the binding loop finds nothing and the very next line dereferences it:
// GadgetComboBoxGetEditBox() has no null check, so the screen NULL-CRASHES at init rather than
// simply drawing short. The same is true of the tooltip wiring on every other column.
//
// CodeGuiRowsEnsure closes that gap from the code side with no .wnd edit and no data rollout: it
// measures what the layout actually authored, creates the missing indices under the same parent
// with the same "File.wnd:Control%d" ids, and copies the authored row's look onto them so a created
// row is indistinguishable from a retail one. Call it BEFORE the binding loops - winGetWindowFromId
// searches the parent's children recursively, so those loops then find code-built rows unchanged.
//
// NOTHING is touched when the .wnd already covers `rowCount`. At the shipped MAX_SLOTS this is a
// measurement and an early return, so retail geometry is bit-for-bit what EA authored.
//-------------------------------------------------------------------------------------------------
enum CodeGuiRowFit
{
	CODEGUI_ROWS_BAND,		///< the rows are a column in a fixed band; squeeze `rowCount` of them into it
	CODEGUI_ROWS_FREE			///< the rows are placed by other code later; create them, position nothing
};

/// Grow the per-index control column named by `idFormat` (one "%d") to `rowCount` rows.
///
/// `parent` is only the search root - each created row is parented to the window its own template
/// row lives under, because a .wnd nests differently per column (on LanGameOptionsMenu the slot
/// controls hang off GadgetParent while ButtonMapStartPosition* hang off MapWindow, and every
/// coordinate involved is parent-relative).
///
/// CODEGUI_ROWS_BAND re-pitches the whole column to keep it inside the band the .wnd gave it, so a
/// twelfth row cannot land on top of the map preview or the chat panel below. CODEGUI_ROWS_FREE is
/// for controls some other code positions afterwards - the map start-position markers are laid out
/// over the preview image from the map's own waypoints, so their authored rects are not a column at
/// all and re-pitching them would be nonsense.
///
/// `leadRow` is a control standing in for an index the column does NOT author. SkirmishGameOptions
/// Menu.wnd has ComboBoxPlayer1..7 and puts TextEntryPlayerName where ComboBoxPlayer0 would be, so
/// the compression pass would otherwise re-pitch rows 1..N and leave row 0 at its authored rect and
/// its authored face - measured 2026-08-20 at 1920x1080: rows 1..11 became 25 px tall at Arial 13,
/// row 0 stayed 43 px tall at Arial 23 and overhung the row below it by 15 px.
///
/// It must be adopted HERE rather than fitted by the caller afterwards, because the band is derived
/// from the authored rects and this function has already overwritten them by the time it returns.
///
/// Returns the number of rows created.
Int CodeGuiRowsEnsure( GameWindow *parent, const char *idFormat, Int rowCount, CodeGuiRowFit fit,
											 GameWindow *leadRow = nullptr, Int leadIndex = 0 );

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/11/2026 SCROLLING SEAT ROWS (owner, 08/11: "make this max hold 12-14 players with a
// scroll bar downwards like maps"; "for ipad show maximum 10 players then scroll"). The chat box was removed the same
// day and put back on 08/11 ("add back the lobby chat asap"): the rows take whole rows from the top of the chat history
// instead of the whole chat.
//
// With 34 seats the BAND fit squeezed every row into the 8-row band the .wnd authored, its floor height won, and the
// column ran a few pixels per row over the chat panel. This replaces BAND for the seat columns of a game-setup screen:
//
//  - if `chatList` is given, the chat history gives up to `chatRows` whole rows at the authored pitch from its TOP, as
//    long as it keeps at least `chatKeep` pixels of its height; every control that sat between the last seat row and
//    the chat (starting cash, superweapons, Limit Armies, ...) moves down by the same amount. The chat input and
//    anything else below the history stay where they are. A row a screen builds later from the listbox's rect (the
//    LAN password row) lands at the history's new top, so `chatKeep` must include it;
//  - the seat band becomes the authored top of row 0 down to where that block now starts;
//  - CodeGuiRowsVisibleCap() rows are shown at a time (fewer if the band cannot hold that many at the AUTHORED pitch),
//    stretched to fill the band - text controls grow with the row and step their face up in proportion, icons keep
//    their authored size and stay centred in the row;
//  - a retail scroll bar (the up / down buttons, track and thumb, wearing `lookListbox`'s own scroll bar art) sits to
//    the right of the rows and scrolls whole rows; the mouse wheel over a row scrolls too. Rows outside the window are
//    parked far off screen rather than hidden, so the screen's own show / hide decisions stay intact and a parked row
//    can neither draw nor take a click. A drop-down's open list is capped to the rows that fit below it on screen.
//
// Every column must hang off the same parent window. `leadRow` stands in for index 0 of the FIRST column (the skirmish
// name field). Call where the screen used to call CodeGuiRowsEnsure, BEFORE the binding loops populate the combo boxes,
// so the stepped-up face is the one the entries are rasterised in. A second call on the same live screen only scrolls
// back to the top.
//-------------------------------------------------------------------------------------------------
/// How many seat rows a game-setup screen shows at once on this platform: 10 on iOS (smaller screen), 14 elsewhere.
Int CodeGuiRowsVisibleCap( void );

void CodeGuiRowsScrollSetup( GameWindow *parent, const char * const *idFormats, Int columnCount, Int rowCount,
														 GameWindow *chatList, Int chatRows, Int chatKeep, GameWindow *lookListbox,
														 GameWindow *leadRow = nullptr );

/// GeneralsX @feature Claude 25/09/2026 One more checkbox that is INDISTINGUISHABLE from the authored
/// checkbox `tmpl` (whose window name is `tmplName`): same art, font, text colours, height and
/// windowtransitions.ini animation, created under tmpl's own parent at parent-relative (x, y) and
/// `width` wide. The same adopt-the-template's-look mechanism CodeGuiRowsEnsure uses for slot rows,
/// so a new game option sits beside the retail ones with no .wnd change. Call before the screen's
/// setGroup, as for CodeGuiRowsEnsure. Returns nullptr (and builds nothing) if tmpl is null.
GameWindow *CodeGuiCheckboxLike( GameWindow *tmpl, const char *tmplName, const char *idName,
																 Int x, Int y, Int width,
																 const WideChar *label, const WideChar *tooltip );

//-------------------------------------------------------------------------------------------------
// Off-stack layout lifecycle. Mirrors Shell::getOptionsLayout / destroyOptionsLayout (Shell.cpp),
// which is the engine's own pattern for a screen that is deliberately NOT on m_screenStack. A
// WindowLayout with no load() call behind it is fully supported: only load() touches a file, and
// it is the only thing that sets m_filenameString.
//-------------------------------------------------------------------------------------------------

/// newInstance(WindowLayout) + addWindow(root). Add ROOTS only; children ride along.
WindowLayout *CodeGuiMakeLayout( GameWindow *root );

/// destroyWindows() + deleteInstance() + nulls your pointer. Uses the DEFERRED winDestroy, so it
/// is safe to call from inside a button handler.
void CodeGuiDestroyLayout( WindowLayout *&layout );
