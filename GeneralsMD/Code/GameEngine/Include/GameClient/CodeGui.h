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
** CodeGui: build GUI screens in code, no .wnd required.
**
** - Never pass WIN_STATUS_IMAGE: a mapped image missing from shipped data draws nothing. Use colours.
** - Use defaultVisual == TRUE so every draw-data slot has a look; call WinInstanceData::init() before every gadget
**   and memset typed data structs to 0 (gogoGadget* memcpy's them). Callbacks are raw function pointers.
** - Never winSetSystemFunc() on a gadget (orphans GWM_DESTROY, leaks user data); messages arrive via the OWNER.
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

// Fonts: control text draws in Arial; the Debug overlay uses Tahoma Bold where the platform has it.
// Bold is a family, not a flag: the FreeType path (macOS/iOS/Linux) ignores IsBold, so the family name sets weight.
// codeGuiFontFamily in CodeGui.cpp holds the per-platform spelling.

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

// Retail-look factories: each builds its gadget the way the .wnd parser builds the named retail control (the shipped
// gadget art, the HEADERTEMPLATE font from the installed HeaderTemplate.ini, that control's text colours). The plain
// factories above keep the CodeGui palette, which only the Debug overlay should wear. Missing art falls back to the
// plain factory: the image draw funcs do not null-check every piece.

/// The face a .wnd control with HEADERTEMPLATE = `templateName` gets, as the parser picks it in the active
/// layout mode. nullptr if the template is not installed.
GameFont *CodeGuiTemplateFont( const char *templateName );

/// An 800x600 authored rect (the .wnd SCREENRECT corners) mapped to screen pixels the way the parser maps
/// a top-level control's, in the active layout mode.
void CodeGuiMapAuthoredRect( Int loX, Int loY, Int hiX, Int hiY, Int *x, Int *y, Int *width, Int *height );

/// A caption like PopupJoinGame.wnd's "GUI:Password" label: LabelRegular, left-justified with the
/// parser's 7-pixel margin, vertically centred, hover tint.
GameWindow *CodeGuiRetailLabel( GameWindow *parent,
																Int x, Int y, Int width, Int height,
																const char *idName,
																const WideChar *text );

/// A text box like PopupJoinGame.wnd:TextEntryGamePassword: the TextEntry art and template.
GameWindow *CodeGuiRetailTextEntry( GameWindow *parent,
																		Int x, Int y, Int width, Int height,
																		const char *idName,
																		Short maxTextLen );

/// A text button like MainMenu.wnd:ButtonUSARecentSave: the three-piece Buttons-* art (left end, repeating middle,
/// right end per state) and the ButtonSmall template.
GameWindow *CodeGuiRetailButton( GameWindow *parent,
																 Int x, Int y, Int width, Int height,
																 const char *idName,
																 const WideChar *label,
																 const WideChar *tooltip = nullptr );

/// EA's X button, as PopupJoinGame.wnd:ButtonCancel: the Xbutton art and ButtonSmall template.
GameWindow *CodeGuiRetailCloseButton( GameWindow *parent,
																			Int x, Int y, Int width, Int height,
																			const char *idName,
																			const WideChar *tooltip = nullptr );

// Per-index control columns authored in a .wnd. A binding loop that runs past the authored rows would null-crash
// (GadgetComboBoxGetEditBox has no null check). CodeGuiRowsEnsure creates the missing "File.wnd:Control%d" rows under
// the same parent, copying the authored look; call it BEFORE the binding loops.
// Nothing is touched when the .wnd already covers `rowCount`.
enum CodeGuiRowFit
{
	CODEGUI_ROWS_BAND,		///< the rows are a column in a fixed band; squeeze `rowCount` of them into it
	CODEGUI_ROWS_FREE			///< the rows are placed by other code later; create them, position nothing
};

/// Grow the per-index control column named by `idFormat` (one "%d") to `rowCount` rows; returns the number created.
/// `parent` is only the search root; each new row goes under its template row's own parent.
/// BAND re-pitches the column to stay in the .wnd's band; FREE is for controls placed elsewhere (map start markers).
/// `leadRow` stands in for an index the column does not author; it is adopted here, before the authored rects change.
Int CodeGuiRowsEnsure( GameWindow *parent, const char *idFormat, Int rowCount, CodeGuiRowFit fit,
											 GameWindow *leadRow = nullptr, Int leadIndex = 0 );

// Scrolling seat rows for game-setup screens (replaces BAND): CodeGuiRowsVisibleCap() rows show, with a scroll bar; the wheel
// over the rows or the bar scrolls them (an open drop-down keeps it for its own list).
// Rows outside the window are parked off screen, not hidden, so the screen's own show/hide logic stays intact.
// With `chatList`, the chat gives up to `chatRows` rows from its top (keeping `chatKeep` px); controls between shift.
// All columns share one parent; call before the binding loops. A second call on a live screen only scrolls to top.
/// Seat rows shown at once on a game-setup screen: 10 on iOS, 14 elsewhere.
Int CodeGuiRowsVisibleCap( void );

void CodeGuiRowsScrollSetup( GameWindow *parent, const char * const *idFormats, Int columnCount, Int rowCount,
														 GameWindow *chatList, Int chatRows, Int chatKeep, GameWindow *lookListbox,
														 GameWindow *leadRow = nullptr );

/// Show or hide the seat rows' scroll bar together with the per-player controls. The map pickers and the connection
/// screen hide the setup screen's controls by name (ShowUnderlyingGUIElements) and the bar has none of those names, so it
/// stayed drawn through the see-through popup. Pass the layout file and parent name that call got. While the bar is
/// hidden the wheel scrolls nothing. Does nothing on a screen without the bar.
void CodeGuiRowsScrollShow( Bool show, const char *layoutFilename, const char *parentName );

/// One more checkbox that is INDISTINGUISHABLE from the authored
/// checkbox `tmpl` (whose window name is `tmplName`): same art, font, text colours, height and
/// windowtransitions.ini animation, created under tmpl's own parent at parent-relative (x, y) and
/// `width` wide. The same adopt-the-template's-look mechanism CodeGuiRowsEnsure uses for slot rows,
/// so a new game option sits beside the retail ones with no .wnd change. Call before the screen's
/// setGroup, as for CodeGuiRowsEnsure. Returns nullptr (and builds nothing) if tmpl is null.
GameWindow *CodeGuiCheckboxLike( GameWindow *tmpl, const char *tmplName, const char *idName,
																 Int x, Int y, Int width,
																 const WideChar *label, const WideChar *tooltip );

/// GeneralsX @feature Twins of an authored control for a row a screen builds in code, so it wears the screen's own
/// art, draw function, text colours and face instead of the CodeGui theme. Each is created under `parent` at
/// parent-relative (x, y, width, height) and then takes `tmpl`'s look (a static text also takes its justification).
/// Each returns nullptr (and builds nothing) if tmpl is null, so a caller can fall back to the plain factory.
GameWindow *CodeGuiLabelLike( GameWindow *tmpl, GameWindow *parent, const char *idName,
															Int x, Int y, Int width, Int height, const WideChar *text );
GameWindow *CodeGuiTextEntryLike( GameWindow *tmpl, GameWindow *parent, const char *idName,
																	Int x, Int y, Int width, Int height, Short maxTextLen );
GameWindow *CodeGuiButtonLike( GameWindow *tmpl, GameWindow *parent, const char *idName,
															 Int x, Int y, Int width, Int height, const WideChar *label, const WideChar *tooltip );

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
