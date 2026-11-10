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

// FILE: CodeGui.cpp ///////////////////////////////////////////////////////////////////////////////
// Desc:   Build GUI screens in code, with no .wnd file and no art dependency
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

// GeneralsX: codeGuiFontFamily() has to tell iOS apart from macOS;
// TARGET_OS_IPHONE is only defined once this is included.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "Common/GameMemory.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/CodeGui.h"
#include "GameClient/Color.h"
#include "GameClient/LoadScreen.h"		// LoadScreenSetRowBuilder - Core asks, this file answers
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"		// a cloned row inherits its template's animation
#include "GameClient/WinInstanceData.h"
#include "GameClient/WindowLayout.h"

//-------------------------------------------------------------------------------------------------
Int CodeGuiScreenW( void ) { return TheDisplay ? (Int)TheDisplay->getWidth() : 1; }
Int CodeGuiScreenH( void ) { return TheDisplay ? (Int)TheDisplay->getHeight() : 1; }
Int CodeGuiScaleX( Int authoring800 ) { return ( authoring800 * CodeGuiScreenW() ) / 800; }
Int CodeGuiScaleY( Int authoring600 ) { return ( authoring600 * CodeGuiScreenH() ) / 600; }
Int CodeGuiAtLeast( Int value, Int floorValue ) { return value < floorValue ? floorValue : value; }

//-------------------------------------------------------------------------------------------------
static const CodeGuiTheme s_theme =
{
	// Deliberately translucent -- the owner reviewed the rendered result and prefers seeing the
	// screen underneath. Do not "fix" this to opaque: the click-through that made the overlay feel
	// broken was a separate defect (DebugMenuInput returned MSG_IGNORED for mouse messages, so
	// taps fell through to the tactical view), and translucency was never the cause.
	/* panelBg */							GameMakeColor(   6,   8,  14, 235 ),
	/* panelBorder */					GameMakeColor(  90, 110, 170, 255 ),
	/* buttonBg */						GameMakeColor(  26,  34,  56, 255 ),
	/* buttonBorder */				GameMakeColor( 120, 145, 205, 255 ),
	/* buttonHotBg */					GameMakeColor(  46,  62,  98, 255 ),
	/* buttonHotBorder */			GameMakeColor( 190, 210, 255, 255 ),
	/* buttonDownBg */				GameMakeColor( 235, 190,  60, 255 ),
	/* buttonDownBorder */		GameMakeColor( 255, 255, 255, 255 ),
	/* buttonDisabledBg */		GameMakeColor(  24,  24,  28, 200 ),
	/* buttonDisabledBorder */	GameMakeColor(  90,  90,  96, 255 ),
	/* fieldBg */							GameMakeColor(   0,   0,   0, 190 ),
	/* fieldBorder */					GameMakeColor(  49,  55, 168, 255 ),
	/* textNormal */					GameMakeColor( 254, 254, 254, 255 ),
	/* textShadow */					GameMakeColor(   0,   0,   0, 255 ),
	/* textDisabled */				GameMakeColor( 140, 140, 148, 255 ),
	/* accent */							GameMakeColor( 255, 220,  60, 255 )
};
const CodeGuiTheme &CodeGuiGetTheme( void ) { return s_theme; }

//-------------------------------------------------------------------------------------------------
/** Spell the Arial family the way THIS platform's font locator wants it.
	*
	* GeneralsX: Real bold on the FreeType platforms.
	*
	* The Bool that winFindFont -> FontLibrary::getFont -> Get_FontChars -> Initialize_GDI_Font
	* threads through only reaches FW_BOLD inside FontCharsClass::Create_GDI_Font. On macOS/iOS/Linux
	* the build takes Create_Freetype_Font( font_name ) instead, which never reads IsBold — so the
	* flag alone buys nothing but a duplicate GameFont with identical glyphs. The weight has to come
	* out of the family name, and the two locators disagree on how to spell it:
	*
	*   iOS      Locate_Font_FontConfig lowercases the name, strips spaces, and opens
	*            fonts/<name>.ttf under the CWD. "Arial Bold" -> fonts/arialbold.ttf, which
	*            scripts/build/ios/stage-fonts.sh stages as LiberationSans-Bold. VERIFIED present.
	*   macOS /  Locate_Font_FontConfig runs FcNameParse + FcFontMatch. FcNameParse treats
	*   Linux    "Arial Bold" as a FAMILY, finds no such family, and FcFontMatch — which never
	*            fails, it only degrades — hands back whatever it likes best (Verdana on the
	*            reference Mac). ":bold" is fontconfig's own weight syntax and is the only spelling
	*            that actually lands on Arial Bold.
	*   Win32    GDI honours the bold flag directly, and a face name with a colon in it would not
	*            resolve, so the plain family is correct there.
	*
	* Keep the bold flag set as well as the name: it is what keeps the bold and regular faces in
	* separate FontCharsClass slots (FontCharsClass::Is_Font compares name, size AND bold). */
//-------------------------------------------------------------------------------------------------
static const char *codeGuiFontFamily( Bool bold )
{
	if( bold == FALSE )
		return "Arial";

#if defined(_WIN32)
	return "Arial";
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	return "Arial Bold";
#else
	return "Arial:bold";
#endif
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX: THE DEBUG OVERLAY DRAWS IN TAHOMA BOLD (owner: "try a different, sharper,
	* more beautiful font").
	*
	* Weight, not family, is what makes it sharper here, and that was measured, not assumed: rendered the way
	* render2dsentence.cpp renders (FreeType v35, 96 dpi, 8-bit alpha) at the overlay's 10/12 pt, a line of its text
	* is 38% solid ink in Arial and 38% in Tahoma - both spindly grey on a dark panel - and 55-61% in the bold faces.
	* Tahoma is the different face: drawn pixel-first for exactly these sizes, and the Windows UI face of the game's
	* own era, so it still reads as Zero Hour rather than a web font. It ships with Windows and macOS; Linux's
	* fontconfig degrades it to the nearest hinted sans; iOS stages only the Liberation fonts, so there the overlay
	* keeps Arial, in bold.
	*
	* Scoped to the overlay's own windows - the Debug menu and its floating button - by their root window id. Other
	* code-built controls sit inside retail .wnd screens (the LAN password row) and stay in the game's Arial. */
//-------------------------------------------------------------------------------------------------
static Bool codeGuiInDebugOverlay( GameWindow *parent )
{
	GameWindow *root = parent;
	while( root != nullptr && root->winGetParent() != nullptr )
		root = root->winGetParent();
	if( root == nullptr || TheNameKeyGenerator == nullptr )
		return FALSE;
	const Int id = root->winGetWindowId();
	return id == (Int)TheNameKeyGenerator->nameToKey( "DebugMenu:Root" )
			|| id == (Int)TheNameKeyGenerator->nameToKey( "DebugFloat:Host" );
}

static const char *codeGuiOverlayFamily( void )
{
#if defined(_WIN32)
	return "Tahoma";					// GDI takes the weight from the bold flag
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	return "Arial Bold";			// fonts/arialbold.ttf; no Tahoma is staged
#else
	return "Tahoma:bold";			// fontconfig's weight syntax, as for Arial:bold above
#endif
}

//-------------------------------------------------------------------------------------------------
/** Resolve the font every code-built control uses. `parent` is the window the control is created in. */
//-------------------------------------------------------------------------------------------------
static GameFont *codeGuiFont( GameWindow *parent, Int pointSize, Bool bold = FALSE )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	if( codeGuiInDebugOverlay( parent ) )
	{
		GameFont *overlayFont = TheWindowManager->winFindFont( codeGuiOverlayFamily(), pointSize, TRUE );
		if( overlayFont != nullptr )
			return overlayFont;
		bold = TRUE;				// no overlay face: the bold Arial below, still the sharper weight
	}

	GameFont *font = TheWindowManager->winFindFont( codeGuiFontFamily( bold ), pointSize, bold );

	// A gadget with a null font draws no glyphs at all, so a missing bold face must degrade to the
	// regular one rather than to nothing. getFont() only returns null on a genuine load failure —
	// on the fontconfig path that essentially cannot happen, but on iOS it can if fonts/ was never
	// staged into the bundle.
	if( font == nullptr && bold )
		font = TheWindowManager->winFindFont( "Arial", pointSize, FALSE );

	return font;
}

//-------------------------------------------------------------------------------------------------
/** Stamp an id onto a freshly created window.
	*
	* winCreate copies instData wholesale (m_instData = *data) so the id is already in place; this
	* just keeps the call sites honest about which windows are addressable, and covers the gadget
	* factories that rebuild parts of instData behind our back. */
//-------------------------------------------------------------------------------------------------
static void codeGuiSetId( GameWindow *win, const char *idName )
{
	if( win == nullptr || idName == nullptr )
		return;

	win->winSetWindowId( (Int)TheNameKeyGenerator->nameToKey( idName ) );
}

//-------------------------------------------------------------------------------------------------
/** Draw code-built controls with Zero Hour's OWN art instead of flat theme colours.
	*
	* GeneralsX:
	*
	* assignDefaultGadgetLook ALREADY installs the retail skin on every gadget this file creates -
	* GameWindowManager.cpp calls winFindImage("PushButtonEnabled"), "PushButtonHilite",
	* "RadioButtonEnabledBoxSelected" and the rest, and CodeGui passes defaultVisual=TRUE for buttons
	* and checkboxes, so the images are attached and paid for already. They never draw for exactly one
	* reason: gogoGadget* picks the image variant or the colour variant AT CREATION from
	* WIN_STATUS_IMAGE, and we do not pass it. So this is a switch, not a port.
	*
	* Verified present in the shipped data rather than assumed: "PushButtonEnabled" appears in
	* WindowZH.big and the MappedImages definitions are in INIZH.big, on a stock Zero Hour install.
	* That was the open worry in the scrollbar note below - if the mapped images were absent the
	* scrollbar would draw nothing at all - and it is answered.
	*
	* OFF BY DEFAULT AND ENV-GATED WHILE IT IS BEING JUDGED. EA's control art is authored at fixed
	* sizes, and CodeGui lays controls out at arbitrary rects scaled from an 800x600 authoring space,
	* so a stretched 9-slice may well look worse than the flat theme on some of them. That is a
	* judgement to make by looking, per screen, and an env gate makes it an A/B with no rebuild
	* between the two. Flip the default here once the look is settled and delete the gate. */
//-------------------------------------------------------------------------------------------------
static Bool codeGuiSkinned( void )
{
	// Read once. getenv per control would be a syscall per gadget on every screen build.
	static Int cached = -1;
	if( cached < 0 )
	{
		const char *v = getenv( "GX_CODEGUI_SKIN" );
		cached = ( v != nullptr && v[ 0 ] != '\0' && v[ 0 ] != '0' ) ? 1 : 0;
	}
	return cached != 0;
}

//-------------------------------------------------------------------------------------------------
/** WIN_STATUS_IMAGE when skinning is on, nothing when it is off.
	*
	* Both the creation status and WinInstanceData::m_status have to carry it: GameWindow::m_status
	* and the instance data are separate fields that the draw path and the gadget code read
	* independently - the same reason codeGuiForceColorDraw below has to clear both. */
//-------------------------------------------------------------------------------------------------
static UnsignedInt codeGuiImageBit( void )
{
	return codeGuiSkinned() ? WIN_STATUS_IMAGE : 0;
}

//-------------------------------------------------------------------------------------------------
/** Undo a forced WIN_STATUS_IMAGE on a gadget the engine built for us.
	*
	* GameWindow::m_status and WinInstanceData::m_status are separate fields that the draw path and
	* the gadget code read independently, so both have to be cleared. Rebinding the draw func is the
	* part that actually matters: gogoGadget* chose the image variant at creation and nothing else
	* ever revisits that choice. */
//-------------------------------------------------------------------------------------------------
static void codeGuiForceColorDraw( GameWindow *win, GameWinDrawFunc colorDraw )
{
	if( win == nullptr )
		return;

	win->winClearStatus( WIN_STATUS_IMAGE );
	BitClear( win->winGetInstanceData()->m_status, WIN_STATUS_IMAGE );

	if( colorDraw )
		win->winSetDrawFunc( colorDraw );
}

//-------------------------------------------------------------------------------------------------
/** Paint a push button (or a slider thumb, or a scrollbar arrow) in theme colours.
	*
	* Slots 0 AND 1 both matter: W3DGadgetPushButtonDraw selects index 1 whenever WIN_STATE_SELECTED
	* is set, and assignDefaultGadgetLook leaves index 1 on its debug yellow. */
//-------------------------------------------------------------------------------------------------
static void codeGuiThemeButton( GameWindow *btn, Color bg, Color border, Color downBg, Color downBorder )
{
	if( btn == nullptr )
		return;

	GadgetButtonSetEnabledColor								( btn, bg );
	GadgetButtonSetEnabledBorderColor					( btn, border );
	GadgetButtonSetEnabledSelectedColor				( btn, downBg );
	GadgetButtonSetEnabledSelectedBorderColor	( btn, downBorder );
	GadgetButtonSetHiliteColor								( btn, s_theme.buttonHotBg );
	GadgetButtonSetHiliteBorderColor					( btn, s_theme.buttonHotBorder );
	GadgetButtonSetHiliteSelectedColor				( btn, downBg );
	GadgetButtonSetHiliteSelectedBorderColor	( btn, downBorder );
	GadgetButtonSetDisabledColor							( btn, s_theme.buttonDisabledBg );
	GadgetButtonSetDisabledBorderColor				( btn, s_theme.buttonDisabledBorder );
	GadgetButtonSetDisabledSelectedColor			( btn, s_theme.buttonDisabledBg );
	GadgetButtonSetDisabledSelectedBorderColor( btn, s_theme.buttonDisabledBorder );

	btn->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	btn->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	btn->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiPanel( GameWindow *parent, Int x, Int y, Int width, Int height,
													GameWinSystemFunc systemFunc, const char *idName,
													UnsignedInt extraStatus )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	const UnsignedInt status = WIN_STATUS_ENABLED | extraStatus;	// never WIN_STATUS_IMAGE

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_USER_WINDOW;		// matches the .wnd "USER" window type
	instData.m_status = status;							// GameWindow::m_status and WinInstanceData::m_status are
																					// DIFFERENT fields; keep them in sync
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// winCreate sends GWM_CREATE BEFORE it copies instData, so id/style/status are not yet visible
	// inside a GWM_CREATE handler. Do all setup on the returned pointer.
	GameWindow *win = TheWindowManager->winCreate( parent, status, x, y, width, height,
																								 systemFunc, &instData );
	if( win == nullptr )
		return nullptr;

	codeGuiSetId( win, idName );

	if( BitIsSet( status, WIN_STATUS_SEE_THRU ) == FALSE )
	{
		// W3DGameWinDefaultDraw's colour branch, which skips any slot still holding
		// WIN_COLOR_UNDEFINED — and WinInstanceData::init() leaves every slot exactly there.
		// All three states get filled: a panel with GWS_MOUSE_TRACK children can still pick up
		// WIN_STATE_HILITED, and a disabled panel must not vanish.
		win->winSetEnabledColor				( 0, s_theme.panelBg );
		win->winSetEnabledBorderColor	( 0, s_theme.panelBorder );
		win->winSetHiliteColor				( 0, s_theme.panelBg );
		win->winSetHiliteBorderColor	( 0, s_theme.panelBorder );
		win->winSetDisabledColor			( 0, s_theme.panelBg );
		win->winSetDisabledBorderColor( 0, s_theme.panelBorder );
	}

	return win;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiButton( GameWindow *parent, Int x, Int y, Int width, Int height,
													 const char *idName, const WideChar *label, const WideChar *tooltip )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_PUSH_BUTTON | GWS_MOUSE_TRACK;	// GWS_PUSH_BUTTON is asserted
	instData.m_status = WIN_STATUS_ENABLED | codeGuiImageBit();
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );
	if( tooltip )
		instData.setTooltipText( UnicodeString( tooltip ) );

	// defaultVisual = TRUE: assignDefaultGadgetLook fills EVERY colour slot and all three text
	// colour pairs. It also installs mapped images by name - PushButtonEnabled, PushButtonHilite and
	// the rest - and whether those or the colours draw is decided HERE, by WIN_STATUS_IMAGE, latched
	// at creation and never revisited. See codeGuiSkinned().
	GameWindow *btn = TheWindowManager->gogoGadgetPushButton(
												parent,
												WIN_STATUS_ENABLED | codeGuiImageBit(),
												x, y, width, height,
												&instData,
												codeGuiFont( parent, 12 ),
												TRUE );
	if( btn == nullptr )
		return nullptr;

	codeGuiSetId( btn, idName );
	// Skipped when skinned: these overwrite the colour slots the image draw func never reads, so
	// applying them would be dead work - and leaving them out keeps the two looks honestly separate
	// rather than half-themed.
	if( !codeGuiSkinned() )
	{
		codeGuiThemeButton( btn, s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
	}

	// LITERAL text. GadgetButtonSetText does not go through TheGameText->fetch(), unlike the .wnd
	// TEXT= path — which is why a .wnd built outside the shipped CSF shows "MISSING: 'Back'".
	if( label )
		GadgetButtonSetText( btn, UnicodeString( label ) );

	return btn;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiLabel( GameWindow *parent, Int x, Int y, Int width, Int height,
													const char *idName, const WideChar *text, Bool centered, Int pointSize,
													Bool bold )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	TextData				textData;
	instData.init();
	memset( &textData, 0, sizeof( textData ) );		// memcpy'd wholesale into a fresh TextData;
																								// textData.text is overwritten for us
	textData.centered = centered;

	instData.m_style	= GWS_STATIC_TEXT | GWS_MOUSE_TRACK;
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// defaultVisual = FALSE here (unlike buttons) so no background box is painted behind the text:
	// every slot stays WIN_COLOR_UNDEFINED and only the glyphs draw. This is the in-tree precedent
	// from MainMenu.cpp's version/credit labels.
	// The font still lands: assignDefaultGadgetLook applies defaultFont BEFORE it checks
	// assignVisual and returns, so FALSE suppresses the colours and images only.
	// NOTE: gogoGadgetStaticText BitClears GWS_TAB_STOP on OUR struct — harmless, but it is why
	// instData must never be reused without another init().
	GameWindow *lbl = TheWindowManager->gogoGadgetStaticText(
												parent, WIN_STATUS_ENABLED,
												x, y, width, height,
												&instData, &textData,
												codeGuiFont( parent, pointSize, bold ),
												FALSE );
	if( lbl == nullptr )
		return nullptr;

	codeGuiSetId( lbl, idName );

	lbl->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	lbl->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	lbl->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	if( text )
		GadgetStaticTextSetText( lbl, UnicodeString( text ) );	// do this LAST, it needs the font

	return lbl;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiCheckbox( GameWindow *parent, Int x, Int y, Int width, Int height,
														 const char *idName, const WideChar *label, Bool checked )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_CHECK_BOX | GWS_MOUSE_TRACK;		// GWS_CHECK_BOX is asserted
	instData.m_status = WIN_STATUS_ENABLED | codeGuiImageBit();
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	GameWindow *box = TheWindowManager->gogoGadgetCheckbox(
												parent, WIN_STATUS_ENABLED | codeGuiImageBit(),
												x, y, width, height,
												&instData,
												codeGuiFont( parent, 12 ),
												TRUE );
	if( box == nullptr )
		return nullptr;

	codeGuiSetId( box, idName );

	// Skinned: CheckBoxEnabled / CheckBoxEnabledBoxSelected and friends draw, and every colour slot
	// below is dead work the image draw func never reads. See codeGuiSkinned().
	if( codeGuiSkinned() )
	{
		box->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
		box->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
		box->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

		if( label )
			GadgetCheckBoxSetText( box, UnicodeString( label ) );

		// The state bit by hand, NOT GadgetCheckBoxSetChecked - same reason as the unskinned path
		// below: the helper winSendSystemMsg's GBM_SELECTED at the owner, which during construction
		// reaches the screen's handler before it has assigned its control pointers.
		if( checked )
			BitSet( box->winGetInstanceData()->m_state, WIN_STATE_SELECTED );

		return box;
	}

	// Slot 0 is the row background, 1 the unchecked box, 2 the checked box. assignDefaultGadgetLook
	// deliberately leaves the unchecked FILL at WIN_COLOR_UNDEFINED so the box reads as hollow;
	// we keep that and only supply the border, otherwise checked and unchecked look identical.
	GadgetCheckBoxSetEnabledColor										( box, s_theme.fieldBg );
	GadgetCheckBoxSetEnabledBorderColor							( box, s_theme.fieldBorder );
	GadgetCheckBoxSetEnabledUncheckedBoxColor				( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetEnabledUncheckedBoxBorderColor	( box, s_theme.buttonBorder );
	GadgetCheckBoxSetEnabledCheckedBoxColor					( box, s_theme.accent );
	GadgetCheckBoxSetEnabledCheckedBoxBorderColor		( box, s_theme.textNormal );

	GadgetCheckBoxSetHiliteColor										( box, s_theme.buttonHotBg );
	GadgetCheckBoxSetHiliteBorderColor							( box, s_theme.buttonHotBorder );
	GadgetCheckBoxSetHiliteUncheckedBoxColor				( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetHiliteUncheckedBoxBorderColor	( box, s_theme.textNormal );
	GadgetCheckBoxSetHiliteCheckedBoxColor					( box, s_theme.accent );
	GadgetCheckBoxSetHiliteCheckedBoxBorderColor		( box, s_theme.textNormal );

	GadgetCheckBoxSetDisabledColor									( box, s_theme.buttonDisabledBg );
	GadgetCheckBoxSetDisabledBorderColor						( box, s_theme.buttonDisabledBorder );
	GadgetCheckBoxSetDisabledUncheckedBoxColor			( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetDisabledUncheckedBoxBorderColor( box, s_theme.buttonDisabledBorder );
	GadgetCheckBoxSetDisabledCheckedBoxColor				( box, s_theme.textDisabled );
	GadgetCheckBoxSetDisabledCheckedBoxBorderColor	( box, s_theme.buttonDisabledBorder );

	box->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	box->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	box->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	if( label )
		GadgetCheckBoxSetText( box, UnicodeString( label ) );

	// Set the state bit by hand rather than through GadgetCheckBoxSetChecked: that helper also
	// winSendSystemMsg's GBM_SELECTED at the owner, which during construction reaches the screen's
	// handler before the screen has finished assigning its control pointers. Use the helper for
	// every LATER change; here it would fire a click nobody made.
	if( checked )
		BitSet( box->winGetInstanceData()->m_state, WIN_STATE_SELECTED );

	return box;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiTextEntry( GameWindow *parent, Int x, Int y, Int width, Int height,
															const char *idName, Short maxTextLen, Int pointSize, Bool bold )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	EntryData				entryData;
	instData.init();
	memset( &entryData, 0, sizeof( entryData ) );		// MANDATORY: gogoGadgetTextEntry inspects
																									// entryData->text before the memcpy and
																									// allocates the DisplayStrings from it
	// maxTextLen MUST be > 1. The accept test is `charPos < maxTextLen - 1`, so 0 means `0 < -1`
	// and every keystroke is silently swallowed with the keyboard still up.
	entryData.maxTextLen = ( maxTextLen > 1 ) ? maxTextLen : (Short)32;		// clamped to 256 inside
	entryData.aSCIIOnly	 = FALSE;

	instData.m_style	= GWS_ENTRY_FIELD | GWS_MOUSE_TRACK;	// GWS_ENTRY_FIELD is also the exact bit
																												// SDL3GameEngine::updateTextInputState tests
																												// to raise the iOS keyboard
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	GameWindow *entry = TheWindowManager->gogoGadgetTextEntry(
													parent, WIN_STATUS_ENABLED,
													x, y, width, height,
													&instData, &entryData,
													codeGuiFont( parent, pointSize, bold ),
													TRUE );
	if( entry == nullptr )
		return nullptr;

	codeGuiSetId( entry, idName );

	GadgetTextEntrySetEnabledColor			 ( entry, s_theme.fieldBg );
	GadgetTextEntrySetEnabledBorderColor ( entry, s_theme.fieldBorder );
	GadgetTextEntrySetHiliteColor				 ( entry, s_theme.fieldBg );
	GadgetTextEntrySetHiliteBorderColor	 ( entry, s_theme.accent );
	GadgetTextEntrySetDisabledColor			 ( entry, s_theme.buttonDisabledBg );
	GadgetTextEntrySetDisabledBorderColor( entry, s_theme.buttonDisabledBorder );
	GadgetTextEntrySetTextColor					 ( entry, s_theme.textNormal );

	// winSetFont already dispatched to GadgetTextEntrySetFont (GameWindow::winSetFont switches on
	// the style bits) because gogoGadgetTextEntry does winSetUserData() BEFORE
	// assignDefaultGadgetLook(). Re-asserting is idempotent and documents the dependency: this is
	// the call that puts the face on entryData->text, i.e. on the glyphs the user types.
	GameFont *font = codeGuiFont( parent, pointSize, bold );
	if( font )
		GadgetTextEntrySetFont( entry, font );

	GadgetTextEntrySetText( entry, UnicodeString::TheEmptyString );

	return entry;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiHSlider( GameWindow *parent, Int x, Int y, Int width, Int height,
														const char *idName, Int minVal, Int maxVal )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	SliderData			sliderData;
	instData.init();
	memset( &sliderData, 0, sizeof( sliderData ) );		// memcpy'd; numTicks/position are internal
	sliderData.minVal = minVal;
	sliderData.maxVal = maxVal;

	instData.m_style	= GWS_HORZ_SLIDER | GWS_MOUSE_TRACK;
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// gogoGadgetSlider force-ORs WIN_STATUS_TAB_STOP and builds the thumb itself with
	// gogoGadgetPushButton( slider, status|ENABLED|DRAGGABLE, …, nullptr, TRUE ). Because our status
	// has no WIN_STATUS_IMAGE the thumb takes the COLOUR draw func and gets a full palette from
	// assignDefaultGadgetLook — i.e. the thumb is visible with no art.
	//
	// The size floors are load-bearing, not cosmetic: numTicks is
	// (width - HORIZONTAL_SLIDER_THUMB_WIDTH) / (maxVal - minVal), so a track narrower than the
	// thumb yields a zero or negative tick size and the thumb never tracks the value.
	const Int trackW = CodeGuiAtLeast( width, HORIZONTAL_SLIDER_THUMB_WIDTH * 3 );
	const Int trackH = CodeGuiAtLeast( height, HORIZONTAL_SLIDER_THUMB_HEIGHT );

	GameWindow *slider = TheWindowManager->gogoGadgetSlider(
													 parent, WIN_STATUS_ENABLED,
													 x, y, trackW, trackH,
													 &instData, &sliderData,
													 codeGuiFont( parent, 10 ),
													 TRUE );
	if( slider == nullptr )
		return nullptr;

	codeGuiSetId( slider, idName );

	GadgetSliderSetEnabledColor				( slider, s_theme.fieldBg );
	GadgetSliderSetEnabledBorderColor	( slider, s_theme.fieldBorder );
	GadgetSliderSetHiliteColor				( slider, s_theme.fieldBg );
	GadgetSliderSetHiliteBorderColor	( slider, s_theme.accent );
	GadgetSliderSetDisabledColor			( slider, s_theme.buttonDisabledBg );
	GadgetSliderSetDisabledBorderColor( slider, s_theme.buttonDisabledBorder );

	codeGuiThemeButton( GadgetSliderGetThumb( slider ), s_theme.accent, s_theme.textShadow,
											s_theme.textNormal, s_theme.textShadow );

	return slider;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiListbox( GameWindow *parent, Int x, Int y, Int width, Int height,
														const char *idName, Int listLength, Bool multiSelect, Bool scrollBar,
														Int pointSize )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	ListboxData			listboxData;
	instData.init();
	memset( &listboxData, 0, sizeof( listboxData ) );		// memcpy'd; every field after columnWidth
																											// is engine-internal bookkeeping
	// columns MUST be >= 1: gogoGadgetListBox takes the multi-column branch for anything else and
	// bails out with a half-built listbox when columnWidthPercentage is null. listLength MUST be
	// >= 1 for the same class of reason — GadgetListBoxSetListLength memcpy's from listData.
	listboxData.columns			= 1;
	listboxData.listLength	= (Short)CodeGuiAtLeast( listLength, 1 );
	listboxData.multiSelect = multiSelect;
	listboxData.scrollBar		= scrollBar;
	listboxData.autoScroll	= FALSE;
	listboxData.autoPurge		= FALSE;
	listboxData.forceSelect = FALSE;

	instData.m_style	= GWS_SCROLL_LISTBOX | GWS_MOUSE_TRACK;		// GWS_SCROLL_LISTBOX is asserted
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// No instance text: a listbox with text is treated as having a TITLE, which shrinks
	// displayHeight by a font height and offsets every scrollbar child.
	GameWindow *list = TheWindowManager->gogoGadgetListBox(
											 parent, WIN_STATUS_ENABLED,
											 x, y, width, height,
											 &instData, &listboxData,
											 codeGuiFont( parent, pointSize ),
											 TRUE );
	if( list == nullptr )
		return nullptr;

	codeGuiSetId( list, idName );

	// Propagates through to the scrollbar slider and both arrows, but NOT to the slider thumb.
	GadgetListBoxSetColors( list,
													s_theme.fieldBg,				// enabled
													s_theme.fieldBorder,		// enabled border
													s_theme.buttonHotBg,		// enabled selected item
													s_theme.accent,					// enabled selected item border
													s_theme.buttonDisabledBg,			// disabled
													s_theme.buttonDisabledBorder,	// disabled border
													s_theme.buttonDisabledBg,			// disabled selected item
													s_theme.buttonDisabledBorder,	// disabled selected item border
													s_theme.fieldBg,				// hilite
													s_theme.accent,					// hilite border
													s_theme.buttonHotBg,		// hilite selected item
													s_theme.accent );				// hilite selected item border

	list->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	list->winSetHiliteTextColors	( s_theme.textNormal,	 s_theme.textShadow );
	list->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	// GadgetListboxCreateScrollbar is the one place the engine force-ORs WIN_STATUS_IMAGE behind
	// our back, and assignDefaultGadgetLook then points the arrows at VSliderLarge*ButtonEnabled
	// and friends. If those mapped images are missing from the shipped data the whole scrollbar
	// draws nothing at all, so strip the bit and rebind the colour draw funcs. The scrollbar's own
	// vertical slider already escapes this — gogoGadgetSlider special-cases a listbox parent — but
	// its thumb is a plain push button and does not.
	//
	// When skinning is ON this is exactly the state we want, so leave the engine's bit alone and
	// let VSliderLarge*ButtonEnabled draw. The "might be missing from the shipped data" worry above
	// was checked rather than left standing: those images are in WindowZH.big. See codeGuiSkinned().
	GameWindow *scroll = GadgetListBoxGetSlider( list );
	if( scroll && !codeGuiSkinned() )
	{
		GameWindow *thumb = GadgetSliderGetThumb( scroll );

		codeGuiForceColorDraw( GadgetListBoxGetUpButton( list ),
													 TheWindowManager->getPushButtonDrawFunc() );
		codeGuiForceColorDraw( GadgetListBoxGetDownButton( list ),
													 TheWindowManager->getPushButtonDrawFunc() );
		codeGuiForceColorDraw( scroll, TheWindowManager->getVerticalSliderDrawFunc() );
		codeGuiForceColorDraw( thumb,	 TheWindowManager->getPushButtonDrawFunc() );

		codeGuiThemeButton( GadgetListBoxGetUpButton( list ), s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
		codeGuiThemeButton( GadgetListBoxGetDownButton( list ), s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
		codeGuiThemeButton( thumb, s_theme.accent, s_theme.textShadow,
												s_theme.textNormal, s_theme.textShadow );
	}

	return list;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiComboBox( GameWindow *parent, Int x, Int y, Int width, Int height,
														 const char *idName, Int maxDisplay, Int maxChars, Bool isEditable )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	ComboBoxData		comboData;
	instData.init();
	memset( &comboData, 0, sizeof( comboData ) );		// memcpy'd wholesale; everything from dontHide
																									// down is engine-internal bookkeeping

	// maxChars feeds EntryData::maxTextLen, whose accept test is `charPos < maxTextLen - 1`: at 0
	// that reads `0 < -1` and every keystroke is silently swallowed. maxDisplay is a row count, so a
	// zero would open a list with no visible rows and no way to pick anything out of it.
	comboData.isEditable						= isEditable;
	comboData.maxDisplay						= CodeGuiAtLeast( maxDisplay, 1 );
	comboData.maxChars							= CodeGuiAtLeast( maxChars, 2 );
	comboData.asciiOnly							= FALSE;
	comboData.lettersAndNumbersOnly	= FALSE;

	// HEAP, and never freed here. gogoGadgetComboBox hands these two straight to gogoGadgetTextEntry
	// and gogoGadgetListBox, then DELETES them and re-points the fields at the sub-gadgets' own
	// copies. This mirrors what the .wnd parser does for a COMBOBOX block; a stack struct here would
	// be passed to delete.
	comboData.entryData		= NEW EntryData;
	comboData.listboxData	= NEW ListboxData;
	memset( comboData.entryData,	 0, sizeof( EntryData ) );
	memset( comboData.listboxData, 0, sizeof( ListboxData ) );

	comboData.entryData->maxTextLen				= (Short)comboData.maxChars;
	comboData.entryData->aSCIIOnly				= comboData.asciiOnly;
	comboData.entryData->alphaNumericalOnly	= comboData.lettersAndNumbersOnly;

	// columns MUST be >= 1 and listLength >= 1, for the reasons spelled out on CodeGuiListbox.
	// listLength is the number of entries the list can HOLD, not the number it shows at once: when
	// it is reached GadgetListBoxAddEntry silently purges the top entry, so a list sized to
	// maxDisplay would quietly drop the first half of a long colour or faction list. forceSelect is
	// what the .wnd parser sets for a combo's list, and it is what stops a second click on the
	// already-selected row from clearing the selection.
	comboData.listboxData->columns			= 1;
	comboData.listboxData->listLength		= (Short)CodeGuiAtLeast( comboData.maxDisplay * 4, 64 );
	comboData.listboxData->scrollBar		= TRUE;
	comboData.listboxData->forceSelect	= TRUE;

	instData.m_style	= GWS_COMBO_BOX | GWS_MOUSE_TRACK;		// GWS_COMBO_BOX is asserted
	instData.m_status = WIN_STATUS_ENABLED | codeGuiImageBit();
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// defaultVisual = TRUE, same reasoning as CodeGuiButton: assignDefaultGadgetLook fills every
	// colour slot AND attaches the ListBox*/PushButton*/TextEntry* mapped images to the box, the
	// drop-down button and the edit field. Which of the two draws is decided here, by
	// WIN_STATUS_IMAGE, latched at creation and never revisited. See codeGuiSkinned().
	//
	// Font size 10 is not arbitrary: it is what every stock lobby COMBOBOX block asks for, and the
	// open list's pixel height is winFontHeight * rows, so a larger face makes the drop-down taller
	// than the row it hangs off.
	GameWindow *combo = TheWindowManager->gogoGadgetComboBox(
												parent,
												WIN_STATUS_ENABLED | codeGuiImageBit(),
												x, y, width, height,
												&instData, &comboData,
												codeGuiFont( parent, 10 ),
												TRUE );
	if( combo == nullptr )
		return nullptr;

	codeGuiSetId( combo, idName );

	// Skipped when skinned: these overwrite colour slots the image draw funcs never read. See
	// CodeGuiButton for the same split.
	if( !codeGuiSkinned() )
	{
		GadgetComboBoxSetColors( combo,
														 s_theme.fieldBg,								// enabled
														 s_theme.fieldBorder,						// enabled border
														 s_theme.buttonHotBg,						// enabled selected item
														 s_theme.accent,								// enabled selected item border
														 s_theme.buttonDisabledBg,			// disabled
														 s_theme.buttonDisabledBorder,	// disabled border
														 s_theme.buttonDisabledBg,			// disabled selected item
														 s_theme.buttonDisabledBorder,	// disabled selected item border
														 s_theme.fieldBg,								// hilite
														 s_theme.accent,								// hilite border
														 s_theme.buttonHotBg,						// hilite selected item
														 s_theme.accent );							// hilite selected item border

		// GadgetComboBoxSetColors reaches the list, the edit box and the drop-down button, but the
		// drop-down is a plain push button whose SELECTED slot assignDefaultGadgetLook leaves on its
		// debug yellow - the same slot-1 hole codeGuiThemeButton exists to fill.
		codeGuiThemeButton( GadgetComboBoxGetDropDownButton( combo ),
												s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
	}

	// winSetEnabledTextColors on a combo box forwards to the list and the edit box
	// (GameWindow::winSetEnabledTextColors switches on GWS_COMBO_BOX), which is what puts readable
	// glyphs in the closed box as well as in the open list.
	combo->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	combo->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	combo->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	return combo;
}

//-------------------------------------------------------------------------------------------------
// Per-index control columns. See the block comment in CodeGui.h for why this exists.
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** Look up row `index` of the column named by `idFormat`, which carries exactly one "%d". */
//-------------------------------------------------------------------------------------------------
static GameWindow *codeGuiFindRow( GameWindow *parent, const char *idFormat, Int index )
{
	AsciiString name;
	name.format( idFormat, index );

	// Recursive over the parent's children, which is what lets a created row be parented to the
	// same sub-window its template lives under and still be found by the screen's binding loops.
	return TheWindowManager->winGetWindowFromId( parent, (Int)TheNameKeyGenerator->nameToKey( name ) );
}

//-------------------------------------------------------------------------------------------------
/** Give `win` the look the .wnd gave `tmpl`.
	*
	* Not a theme application: the point is that a created row must be INDISTINGUISHABLE from the
	* authored ones beside it, and the authored ones carry EA's own art. Copying the three draw-data
	* arrays is exactly what GameWindowManagerScript does after each gogoGadget* call, so this is the
	* parser's own mechanism rather than a new one.
	*
	* The draw func has to come across too. gogoGadget* picks the image variant or the colour variant
	* AT CREATION from WIN_STATUS_IMAGE and nothing ever revisits that choice, so a row created with
	* skinning off would keep drawing flat colours over art it now owns. Taking the template's own
	* function pointer sidesteps having to re-derive the right one per gadget type. */
//-------------------------------------------------------------------------------------------------
static void codeGuiAdoptLook( GameWindow *win, GameWindow *tmpl )
{
	if( win == nullptr || tmpl == nullptr )
		return;

	WinInstanceData *dst = win->winGetInstanceData();
	WinInstanceData *src = tmpl->winGetInstanceData();
	if( dst == nullptr || src == nullptr )
		return;

	for( Int i = 0; i < MAX_DRAW_DATA; i++ )
	{
		dst->m_enabledDrawData[ i ]	 = src->m_enabledDrawData[ i ];
		dst->m_disabledDrawData[ i ] = src->m_disabledDrawData[ i ];
		dst->m_hiliteDrawData[ i ]	 = src->m_hiliteDrawData[ i ];
	}
	dst->m_imageOffset = src->m_imageOffset;

	// GameWindow::m_status and WinInstanceData::m_status are separate fields the draw path and the
	// gadget code read independently - the same reason codeGuiForceColorDraw clears both.
	if( BitIsSet( tmpl->winGetStatus(), WIN_STATUS_IMAGE ) )
	{
		win->winSetStatus( WIN_STATUS_IMAGE );
		BitSet( dst->m_status, WIN_STATUS_IMAGE );
	}
	else
	{
		win->winClearStatus( WIN_STATUS_IMAGE );
		BitClear( dst->m_status, WIN_STATUS_IMAGE );
	}
	win->winSetDrawFunc( tmpl->winGetDrawFunc() );

	// Input behaviour, by NAMED bits rather than by copying the status word wholesale. The accept
	// marker is authored NO_INPUT+NO_FOCUS precisely so it cannot swallow a click meant for the row
	// it decorates, and the map start markers are authored RIGHT_CLICK because cycling a start
	// position is a right-click gesture. Copying the whole word would drag HIDDEN and IMAGE across
	// as well, and both of those are decided elsewhere - HIDDEN by the screen's own bind loop.
	const UnsignedInt behaviourBits = WIN_STATUS_NO_INPUT | WIN_STATUS_NO_FOCUS |
																		WIN_STATUS_RIGHT_CLICK | WIN_STATUS_TAB_STOP;
	win->winClearStatus( behaviourBits );
	BitClear( dst->m_status, behaviourBits );
	const UnsignedInt inherited = tmpl->winGetStatus() & behaviourBits;
	if( inherited != 0 )
	{
		win->winSetStatus( inherited );
		BitSet( dst->m_status, inherited );
	}

	// Through the accessors, not the fields: on a combo box each of these forwards to the list and
	// the edit box, and setting m_enabledText by hand would leave those two on the debug palette.
	win->winSetEnabledTextColors ( tmpl->winGetEnabledTextColor(),	tmpl->winGetEnabledTextBorderColor() );
	win->winSetDisabledTextColors( tmpl->winGetDisabledTextColor(), tmpl->winGetDisabledTextBorderColor() );
	win->winSetHiliteTextColors	 ( tmpl->winGetHiliteTextColor(),		tmpl->winGetHiliteTextBorderColor() );

	if( tmpl->winGetFont() )
		win->winSetFont( tmpl->winGetFont() );

	// GeneralsX: A cloned static-text row must inherit its template's
	// JUSTIFICATION, not just its palette. Without this the caption is drawn hard against the left
	// edge of a box whose authored neighbours are centred, which is the "cloned rows sit at a
	// different x" the owner photographed on the Diplomacy screen.
	//
	// This is not a no-op copy: the two creation paths disagree on the DEFAULTS. Every static text
	// parsed from a .wnd goes through parseStaticTextData (GameWindowManagerScript.cpp), which sets
	//     centeredVertically = TRUE;  leftMargin = 7;  topMargin = 7;
	// unconditionally - they are hardcoded, there is no .wnd token for any of the three - on top of
	// the CENTERED flag the layout does author. A row built here instead comes from CodeGuiLabel,
	// which memsets its TextData and so starts at centered/centeredVertically FALSE and both margins
	// 0. W3DStaticText.cpp::drawStaticText then places the glyphs at
	//     centred:     origin.x + width/2 - textWidth/2
	//     otherwise:   origin.x + leftMargin
	// so the gap is (width - textWidth)/2 on a centred column and exactly 7 px on a left-justified
	// one - always leftward, and WIDER THE WIDER THE COLUMN, which is why it reads as every column
	// having its own wrong x rather than as one uniform shift.
	//
	// The rect is NOT the problem and is deliberately not touched here: codeGuiCloneRow already
	// creates the row at the template's own parent-relative x under the template's own parent, and
	// neither addWindowToParent nor codeGuiRowResize rebases or re-widens a static text. Only the
	// glyph placement inside that identical rect was wrong.
	//
	// `text` is pointedly not copied. gogoGadgetStaticText allocates a DisplayString per window and
	// the clone already owns its own; assigning the template's would alias it into two gadgets and
	// double-free it. Only the four justification fields cross over.
	//
	// Both null checks are load bearing rather than defensive habit: GameWindowDummy (headless mode)
	// overrides winGetUserData to return nullptr, and the harnesses run headless.
	if( BitIsSet( tmpl->winGetStyle(), GWS_STATIC_TEXT ) &&
			BitIsSet( win->winGetStyle(), GWS_STATIC_TEXT ) )
	{
		const TextData *srcText = (const TextData *)tmpl->winGetUserData();
		TextData			 *dstText = (TextData *)win->winGetUserData();

		if( srcText != nullptr && dstText != nullptr )
		{
			dstText->centered						= srcText->centered;
			dstText->centeredVertically = srcText->centeredVertically;
			dstText->leftMargin					= srcText->leftMargin;
			dstText->topMargin					= srcText->topMargin;
		}
	}

	// A combo box's three sub-gadgets are separate windows with their own draw data, and the .wnd
	// parser skins them individually. Without this the box would wear EA's art while its drop-down
	// arrow and edit field stayed on the default look.
	if( BitIsSet( tmpl->winGetStyle(), GWS_COMBO_BOX ) )
	{
		codeGuiAdoptLook( GadgetComboBoxGetDropDownButton( win ),
											GadgetComboBoxGetDropDownButton( tmpl ) );
		codeGuiAdoptLook( GadgetComboBoxGetEditBox( win ), GadgetComboBoxGetEditBox( tmpl ) );
		codeGuiAdoptLook( GadgetComboBoxGetListBox( win ), GadgetComboBoxGetListBox( tmpl ) );
	}
}

//-------------------------------------------------------------------------------------------------
/** Give the row at `index` whatever windowtransitions.ini gives the row at `tmplIndex`.
	*
	* Same reason as codeGuiAdoptLook, one layer out: a created row has to be INDISTINGUISHABLE from
	* the authored ones beside it, and "how it arrives on screen" is part of that. A transition group
	* is a list of window NAMES, so it can only ever name what a .wnd authored - the rows created
	* here carry names no artist wrote and therefore animate not at all, appearing fully formed next
	* to neighbours that count up. On the score screen that is the twelve-slot symptom exactly:
	* players 9-12 pop in while 1-8 sweep left to right.
	*
	* BY NAME, not by window. TheTransitionHandler resolves a name to a window when its group starts
	* (TransitionWindow::init), so nothing here needs the clone to be findable yet, and the registration
	* outlives this particular instance of the screen - which is right, because it is a statement about
	* the LAYOUT rather than about one GameWindow. mirrorWindowTransitions is idempotent for exactly
	* that reason; see the note on it.
	*
	* The template's name is rebuilt from idFormat rather than read back off the window, deliberately:
	* NameKeyGenerator::keyToName is a linear scan of every name key in the game, and this would run it
	* once per created row per column - forty full scans on one score screen. The format string is
	* right there and produces the same string by construction.
	*
	* ORDER MATTERS AND IT IS THE CALLER'S. This must run before the screen calls setGroup, because
	* setGroup is what init()s the group's windows. Every caller does - CodeGuiRowsEnsure is called
	* where a screen binds its parent, and the score screen's six setGroup calls are all below that.
	* mirrorWindowTransitions handles the other order safely rather than correctly; see it. */
//-------------------------------------------------------------------------------------------------
static void codeGuiAdoptTransitions( const char *idFormat, Int tmplIndex, Int index )
{
	if( TheTransitionHandler == nullptr || idFormat == nullptr || tmplIndex == index )
		return;

	AsciiString tmplName, cloneName;
	tmplName.format( idFormat, tmplIndex );
	cloneName.format( idFormat, index );

	TheTransitionHandler->mirrorWindowTransitions( tmplName, cloneName );
}

//-------------------------------------------------------------------------------------------------
/** Build one more row of the column `tmpl` belongs to, at `y`, `height` pixels tall.
	*
	* Parented to the TEMPLATE's parent rather than to the search root: a .wnd nests columns
	* differently (the slot controls hang off GadgetParent, the map start markers off MapWindow) and
	* every coordinate in play is parent-relative, so anything else silently offsets the new row. */
//-------------------------------------------------------------------------------------------------
static GameWindow *codeGuiCloneRow( GameWindow *tmpl, const char *idFormat, Int tmplIndex, Int index,
																		Int y, Int height )
{
	Int tx, ty, tw, th;
	tmpl->winGetPosition( &tx, &ty );
	tmpl->winGetSize( &tw, &th );

	AsciiString name;
	name.format( idFormat, index );

	GameWindow				*parent = tmpl->winGetParent();
	const UnsignedInt	 style	= tmpl->winGetStyle();
	GameWindow				*win		= nullptr;

	if( BitIsSet( style, GWS_COMBO_BOX ) )
	{
		// Read the box's real parameters off the template rather than guessing: MAXDISPLAY and
		// MAXCHARS are authored per WINDOW block and differ between the colour, faction and team
		// columns, and a created row that opened a differently sized list would be obvious.
		const ComboBoxData *tmplData = (const ComboBoxData *)tmpl->winGetUserData();

		win = CodeGuiComboBox( parent, tx, y, tw, height, name.str(),
													 tmplData ? tmplData->maxDisplay : 5,
													 tmplData ? tmplData->maxChars : 16,
													 tmplData ? tmplData->isEditable : FALSE );

		// The .wnd parser hardcodes the combo list's capacity at 10 entries for every authored box,
		// while CodeGuiComboBox sizes it generously. Match the template exactly: a created row that
		// could hold MORE colours than the retail rows beside it would show a different list per row,
		// which reads as a data bug rather than as the engine cap it is.
		GameWindow *tmplList = GadgetComboBoxGetListBox( tmpl );
		GameWindow *winList	 = GadgetComboBoxGetListBox( win );
		if( tmplList && winList )
		{
			const ListboxData *tmplListData = (const ListboxData *)tmplList->winGetUserData();
			if( tmplListData && tmplListData->listLength > 0 )
				GadgetListBoxSetListLength( winList, (Int)tmplListData->listLength );
		}
	}
	else if( BitIsSet( style, GWS_PUSH_BUTTON ) )
	{
		win = CodeGuiButton( parent, tx, y, tw, height, name.str(), nullptr );
	}
	else if( BitIsSet( style, GWS_STATIC_TEXT ) )
	{
		win = CodeGuiLabel( parent, tx, y, tw, height, name.str(), nullptr );
	}
	else
	{
		// A plain window - the ping indicator is one, a USER block that only ever carries an image.
		// The system func comes across because winSetSystemFunc substitutes the default for null, so
		// copying it is right whether the template declared one or not.
		win = CodeGuiPanel( parent, tx, y, tw, height, tmpl->winGetSystemFunc(), name.str() );
	}

	if( win == nullptr )
		return nullptr;

	codeGuiAdoptLook( win, tmpl );
	codeGuiAdoptTransitions( idFormat, tmplIndex, index );

	// The template's own hidden/enabled state is deliberately NOT copied. Every caller re-decides
	// both in the loop that binds the row - hiding the accept marker for slots other than zero, for
	// one - and a row created already hidden would stay hidden if the screen only ever calls
	// winHide(FALSE) on the slots it fills.
	return win;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiCheckboxLike( GameWindow *tmpl, const char *tmplName, const char *idName,
																 Int x, Int y, Int width,
																 const WideChar *label, const WideChar *tooltip )
{
	if( tmpl == nullptr )
		return nullptr;

	Int tw = 0, th = 0;
	tmpl->winGetSize( &tw, &th );

	GameWindow *box = CodeGuiCheckbox( tmpl->winGetParent(), x, y, width, th, idName, label );
	if( box == nullptr )
		return nullptr;

	codeGuiAdoptLook( box, tmpl );

	// Same depth as the template, not on top of the whole screen: winCreate PREPENDS, and the head of
	// the child list paints last and wins the click, so a fresh gadget would sit over every open
	// combo box drop-down (the bug lanPasswordRowSendBehind documents). Just ahead of its twin is
	// exactly where an authored sibling would be.
	TheWindowManager->unlinkChildWindow( box );
	TheWindowManager->insertWindowAheadOf( box, tmpl );

	if( tooltip != nullptr )
		box->winSetTooltip( UnicodeString( tooltip ) );
	if( TheTransitionHandler != nullptr && tmplName != nullptr && idName != nullptr )
		TheTransitionHandler->mirrorWindowTransitions( AsciiString( tmplName ), AsciiString( idName ) );
	return box;
}

//-------------------------------------------------------------------------------------------------
/** The smallest face this will ever step a row down to. Below this the glyphs stop being readable,
	* and a row whose text is too small to read is no better than one whose text overflows. */
//-------------------------------------------------------------------------------------------------
static const Int CODEGUI_ROW_MIN_POINT_SIZE = 6;

//-------------------------------------------------------------------------------------------------
/** How far below a row's top edge the gadget starts drawing glyphs, and therefore how much of the
	* row height the face can NOT have.
	*
	* This is read off the draw code rather than chosen, and choosing it instead is what made this
	* whole step-down miss. Two gadget families differ:
	*
	*  - A text entry - and therefore a COMBO BOX, whose visible text is drawn by its child edit box -
	*    puts the first glyph row at origin.y + 5. That is `startOffset` in BOTH of W3DTextEntry.cpp's
	*    draw functions, and it is a FIXED pixel count: it does not scale with the control, so a row
	*    compressed from 43 px to 25 px still loses the same 5. A non-editable combo (ISEDITABLE: 0,
	*    which is what every lobby player row is) additionally clips to its OWN rect rather than to the
	*    glyph box - drawTextEntryText's drawTextFromStart branch sets the clip to
	*    origin.y .. origin.y + size.y - so a face taller than height-5 does not visibly overflow into
	*    the row below, it gets its descenders sliced off inside its own row. Budgeting height-2 for it
	*    is 3 px too generous at 12 slots on 1080p, and 3 px is a whole descender.
	*  - Static text parsed from a .wnd is ALWAYS centeredVertically - parseStaticTextData
	*    (GameWindowManagerScript.cpp) sets it unconditionally, there is no .wnd token for it - and it
	*    also clips to its own rect. Centred text needs only height >= the face, so the two pixels are
	*    a margin there rather than a requirement, and they stay as they were.
	*
	* Keep the 5 in step with `startOffset` in W3DTextEntry.cpp. It lives in the device layer and
	* cannot be read from here, which is precisely why it is named and explained rather than folded
	* silently into the arithmetic. */
//-------------------------------------------------------------------------------------------------
static const Int CODEGUI_ROW_ENTRY_TOP_INSET = 5;		// W3DTextEntry.cpp startOffset
static const Int CODEGUI_ROW_DEFAULT_LEADING = 2;		// hairline, as before

static Int codeGuiRowTextTopInset( GameWindow *win )
{
	const UnsignedInt style = win->winGetStyle();

	if( BitIsSet( style, GWS_COMBO_BOX ) || BitIsSet( style, GWS_ENTRY_FIELD ) )
		return CODEGUI_ROW_ENTRY_TOP_INSET;

	return CODEGUI_ROW_DEFAULT_LEADING;
}

//-------------------------------------------------------------------------------------------------
/** `GX_ROWFIT=1` traces every decision the row font step-down makes.
	*
	* This exists because the step-down is INVISIBLE when it misbehaves. Every way it can fail -
	* a null font, a candidate the library refused, a budget that does not match what the draw code
	* needs - looks identical from outside: no crash, no warning, just text that is the wrong size.
	* It was assumed working for the whole of its life on that evidence.
	*
	* So the read-back is the point. `winSetFont` on a combo box does not set the combo's own glyphs;
	* it dispatches to GadgetComboBoxSetFont, which re-fonts the child edit box, and the EDIT BOX is
	* what draws the visible text. Reporting only what we assigned would prove nothing about what will
	* be drawn - so both are read back off the live windows AFTER assignment, and the edit box's own
	* height is used to state the exact glyph extent the draw will produce against its clip. */
//-------------------------------------------------------------------------------------------------
static Bool codeGuiRowFitTraceEnabled( void )
{
	static Int enabled = -1;
	if( enabled < 0 )
	{
		const char *env = getenv( "GX_ROWFIT" );
		enabled = ( env != nullptr && env[ 0 ] != '\0' && env[ 0 ] != '0' ) ? 1 : 0;
	}
	return enabled != 0;
}

//-------------------------------------------------------------------------------------------------
static void codeGuiRowFitTraceCandidate( Int pointSize, GameFont *cand )
{
	if( cand == nullptr )
	{
		fprintf( stderr, "[GXROWFIT]   cand pt=%d -> (null: out of range or load failed)\n", pointSize );
		return;
	}

	fprintf( stderr, "[GXROWFIT]   cand pt=%d height=%d\n",
					 cand->pointSize, TheWindowManager->winFontHeight( cand ) );
}

//-------------------------------------------------------------------------------------------------
static void codeGuiRowFitTrace( GameWindow *win, Int height, Int budget,
																GameFont *incoming, GameFont *assigned, const char *outcome )
{
	if( codeGuiRowFitTraceEnabled() == FALSE )
		return;

	const WinInstanceData *inst = win->winGetInstanceData();
	const char *rowName = ( inst != nullptr ) ? inst->m_decoratedNameString.str() : "<no-inst>";

	// Read the face back off the live window rather than reporting what we handed in. pointSize is
	// NOT pixel height, so both are printed - a reader comparing a point size against a row height
	// is the mistake this whole trace exists to catch.
	GameFont *readBack = win->winGetFont();
	const Int readBackPt = ( readBack != nullptr ) ? readBack->pointSize : -1;
	const Int readBackH  = TheWindowManager->winFontHeight( readBack );

	// The window that actually draws the glyphs. For a combo box that is its edit box, never the
	// combo itself, and they are only in step if winSetFont dispatched correctly.
	GameWindow *drawer = win;
	if( BitIsSet( win->winGetStyle(), GWS_COMBO_BOX ) )
	{
		GameWindow *editBox = GadgetComboBoxGetEditBox( win );
		if( editBox != nullptr )
			drawer = editBox;
	}

	GameFont *drawFont = drawer->winGetFont();
	const Int drawPt = ( drawFont != nullptr ) ? drawFont->pointSize : -1;
	const Int drawH  = TheWindowManager->winFontHeight( drawFont );

	Int drawerW, drawerH;
	drawer->winGetSize( &drawerW, &drawerH );

	const Int inset  = codeGuiRowTextTopInset( win );
	const Int extent = inset + drawH;						// first glyph row .. last, in drawer-local pixels

	fprintf( stderr,
		"[GXROWFIT] row=%s outcome=%s rowH=%d inset=%d budget=%d"
		" in={%s pt=%d h=%d} assigned={pt=%d h=%d} readback={pt=%d h=%d}"
		" draws={pt=%d h=%d boxH=%d top=%d bottom=%d} %s\n",
		rowName, outcome, height, inset, budget,
		( incoming != nullptr ) ? incoming->nameString.str() : "<null>",
		( incoming != nullptr ) ? incoming->pointSize : -1,
		TheWindowManager->winFontHeight( incoming ),
		( assigned != nullptr ) ? assigned->pointSize : -1,
		TheWindowManager->winFontHeight( assigned ),
		readBackPt, readBackH,
		drawPt, drawH, drawerH, inset, extent,
		( extent > drawerH ) ? "OVERFLOWS-CLIP" : "fits" );
}

//-------------------------------------------------------------------------------------------------
/** Step `win`'s font down until its glyphs fit inside a row `height` pixels tall.
	*
	* codeGuiRowResize used to move the rect and leave the face alone, so a band compressed from
	* 24 px to 16 px kept drawing a 10 pt face and every caption overflowed its box.
	*
	* Four things about fonts here are not obvious and each one can turn this into a silent no-op:
	*
	*  - A GameFont is an INTERNED, process-wide immutable object (FontLibrary::getFont walks
	*    m_fontList and returns the existing element on a name/size/bold match). Mutating one in
	*    place would resize that face for every window in the game, so only the POINTER is ever
	*    reassigned here.
	*  - getFont returns null for pointSize < 1 (it clamps above 512, upstream #3051 / #3268) and on
	*    a genuine load failure, and
	*    winSetFont( nullptr ) draws no glyphs at all. A missing candidate is therefore skipped
	*    rather than accepted, and if nothing in the range fits, the original face is kept: an
	*    overflowing caption is a cosmetic defect, an empty one is a broken screen.
	*  - pointSize is NOT pixel height. The step is gated on winFontHeight (GameFont::height, filled
	*    by loadFontData) rather than on arithmetic over the point size.
	*  - On a combo box, winSetFont dispatches through GameWindow::winSetFont's style switch to
	*    GadgetComboBoxSetFont, which re-fonts the list and the edit box in the same call. The list
	*    then takes GadgetListBox's WIN_STATUS_ONE_LINE branch, which reads winFontHeight live, so a
	*    combo's open drop-down re-measures its rows from the new face; only the glyphs already
	*    rasterised into existing entries keep their old size, which is why callers set the font
	*    BEFORE populating a box. */
//-------------------------------------------------------------------------------------------------
static void codeGuiRowFitFont( GameWindow *win, Int height )
{
	if( win == nullptr || TheWindowManager == nullptr )
		return;

	// NOT a style choice: this is the height the gadget's own draw code leaves for glyphs once its
	// fixed top inset is spent. See codeGuiRowTextTopInset.
	const Int budget = height - codeGuiRowTextTopInset( win );
	const Bool trace = codeGuiRowFitTraceEnabled();

	// Both of the following are silent no-ops in a shipping build and both look exactly like
	// "the step-down ran and was happy". They are traced for that reason and no other.
	GameFont *font = win->winGetFont();
	if( font == nullptr )
	{
		codeGuiRowFitTrace( win, height, budget, nullptr, nullptr, "no-font-on-window" );
		return;
	}

	if( budget <= 0 )
	{
		codeGuiRowFitTrace( win, height, budget, font, nullptr, "row-too-short-for-any-text" );
		return;
	}

	if( TheWindowManager->winFontHeight( font ) <= budget )
	{
		codeGuiRowFitTrace( win, height, budget, font, nullptr, "already-fits" );
		return;																			// already fits - do not shrink a row that is fine
	}

	const AsciiString name = font->nameString;		// by value: the candidate lookup can allocate
	const Bool				bold = font->bold;

	for( Int size = font->pointSize - 1; size >= CODEGUI_ROW_MIN_POINT_SIZE; --size )
	{
		GameFont *cand = TheWindowManager->winFindFont( name, size, bold );
		if( cand == nullptr )
		{
			if( trace )																// out of range, or this face failed to load
				codeGuiRowFitTraceCandidate( size, nullptr );
			continue;
		}

		if( trace )
			codeGuiRowFitTraceCandidate( size, cand );

		if( TheWindowManager->winFontHeight( cand ) <= budget )
		{
			win->winSetFont( cand );
			codeGuiRowFitTrace( win, height, budget, font, cand, "assigned" );
			return;
		}
	}

	// Nothing in range fits. Keep the face we have; see the third note above.
	codeGuiRowFitTrace( win, height, budget, font, nullptr, "no-candidate-fits" );
}

//-------------------------------------------------------------------------------------------------
/** The width a row should have once its height becomes `newH`.
	*
	* A row is compressed VERTICALLY, so most controls keep their width - a combo box owns its
	* column's width, and narrowing one would leave a ragged column and clip the faction names.
	* An ICON does not: the accept marker is a 16x16 image and the ping indicator a small square
	* panel, and squashing one to 16x10 is exactly the "no longer square" the owner reported.
	*
	* The test is the gadget's STYLE rather than its proportions, because the .wnd parser scales x
	* and y independently (GameWindowManagerScript.cpp: xScale = width/createW, yScale =
	* height/createH), so an authored 16x16 arrives as 38x29 at 1920x1080 and a "is it square"
	* test would answer no on exactly the resolutions the owner plays at. What is preserved is the
	* control's ASPECT, which is what makes it look untouched.
	*
	* Never widens: a row only ever gets shorter here, and a control that grew sideways would run
	* into the column beside it. */
//-------------------------------------------------------------------------------------------------
static Int codeGuiRowFitWidth( GameWindow *win, Int curW, Int curH, Int newH )
{
	const UnsignedInt style = win->winGetStyle();

	if( BitIsSet( style, GWS_COMBO_BOX ) || BitIsSet( style, GWS_ENTRY_FIELD ) ||
			BitIsSet( style, GWS_STATIC_TEXT ) || BitIsSet( style, GWS_SCROLL_LISTBOX ) )
		return curW;																// text-bearing: it owns the column's width

	if( curH <= 0 || newH >= curH || curW <= 0 )
		return curW;

	return CodeGuiAtLeast( ( curW * newH ) / curH, 1 );
}

//-------------------------------------------------------------------------------------------------
/** Resize a row to `height`, keeping its proportions, its sub-gadgets and its font consistent.
	*
	* The width is derived rather than passed: the caller used to hand back the row's OLD width with
	* the new height, which is what turned the square accept marker into a letterbox.
	*
	* winSetSize fires GGM_RESIZED, and GadgetComboBoxSystem handles it by repositioning the drop-down
	* button in x and resizing the edit box and the list - but it never touches the button's HEIGHT.
	* Left alone, a shortened row would keep a full-height arrow hanging below it. */
//-------------------------------------------------------------------------------------------------
static void codeGuiRowResize( GameWindow *win, Int height )
{
	Int curW, curH;
	win->winGetSize( &curW, &curH );

	win->winSetSize( codeGuiRowFitWidth( win, curW, curH, height ), height );

	// After the rect, because GGM_RESIZED re-measures the edit box and the list from the size, and
	// the font then re-measures the open list's rows from the face. Doing it the other way round
	// works too but reads as if the order did not matter.
	codeGuiRowFitFont( win, height );

	if( BitIsSet( win->winGetStyle(), GWS_COMBO_BOX ) == FALSE )
		return;

	GameWindow *drop = GadgetComboBoxGetDropDownButton( win );
	if( drop == nullptr )
		return;

	Int dropW, dropH;
	drop->winGetSize( &dropW, &dropH );
	drop->winSetSize( dropW, height );
}

//-------------------------------------------------------------------------------------------------
/** Is `win` one of the `count` rows in `rows`? */
//-------------------------------------------------------------------------------------------------
static Bool codeGuiRowsContains( GameWindow * const *rows, Int count, const GameWindow *win )
{
	for( Int i = 0; i < count; ++i )
		if( rows[ i ] == win )
			return TRUE;

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Restore a column's front-to-back order after rows have been added to it.
	*
	* THE CHILD LIST IS FRONT TO BACK, HEAD FIRST. GameWindowManager::drawWindow walks to the TAIL
	* and paints back through m_prev, so the head is painted LAST and is on top; GameWindow::
	* winPointInChild walks from the HEAD and returns the first hit, so the head also wins the click.
	* A covered row is therefore not merely hidden behind another - it is unclickable.
	*
	* Every stock lobby .wnd authors its combo columns DESCENDING (LanGameOptionsMenu.wnd lists
	* ComboBoxPlayer7 first and ComboBoxPlayer0 last, verified against WindowZH.big) and winCreate
	* PREPENDS each child as it is parsed, so the list comes out ASCENDING: row 0 at the head. That
	* is not decoration. A combo's drop-down opens DOWNWARD - GadgetComboBox grows the box itself by
	* the list's height and hangs the list below the edit box - so row i's open list lies over rows
	* i+1.., and only an ascending list paints it over them instead of under them.
	*
	* CodeGuiRowsEnsure creates rows in ascending index order and each one is prepended, so rows
	* 8..11 arrive in the list as 11,10,9,8 AHEAD of 0..7 - inverted exactly across the band where
	* the extra rows live. Hence this pass.
	*
	* Only the column's own rows move, and only among the sibling POSITIONS the column already
	* occupies: every other window keeps its neighbours, so nothing outside the column changes
	* depth relative to anything else. */
//-------------------------------------------------------------------------------------------------
static void codeGuiRowsRestoreZOrder( GameWindow *parent, const char *idFormat, Int rowCount )
{
	if( TheWindowManager == nullptr )
		return;

	enum { CODEGUI_MAX_ROWS = 64 };
	if( rowCount > CODEGUI_MAX_ROWS )
		return;

	GameWindow *rows[ CODEGUI_MAX_ROWS ];
	GameWindow *owner = nullptr;
	Int					count = 0;

	// The sibling list is the ROW's own parent, not the search root: codeGuiFindRow searches
	// recursively and a .wnd nests columns a level or two down (the slot controls hang off
	// GadgetParent, the map start markers off MapWindow). Rows that somehow ended up under a
	// different parent are skipped - they are not in the same list and cannot be ordered against it.
	for( Int i = 0; i < rowCount; ++i )
	{
		GameWindow *win = codeGuiFindRow( parent, idFormat, i );
		if( win == nullptr )
			continue;

		if( owner == nullptr )
			owner = win->winGetParent();

		if( owner == nullptr || win->winGetParent() != owner )
			continue;

		rows[ count++ ] = win;			// ascending index: this is the order we want, head first
	}

	if( owner == nullptr || count < 2 )
		return;

	//
	// Where the column sits today. For each position it occupies, remember the first sibling after
	// it that is NOT one of ours - that anchor survives the unlink below, while the next member
	// would not. A null anchor means the position was past the last non-member, i.e. the tail.
	//
	GameWindow *anchor[ CODEGUI_MAX_ROWS ];
	Int					slots = 0;
	Bool				inOrder = TRUE;

	for( GameWindow *child = owner->winGetChild(); child != nullptr; child = child->winGetNext() )
	{
		if( codeGuiRowsContains( rows, count, child ) == FALSE )
			continue;

		if( slots >= count )
			break;											// cannot happen; refuse to write past the array if it does

		if( child != rows[ slots ] )
			inOrder = FALSE;

		GameWindow *after = child->winGetNext();
		while( after != nullptr && codeGuiRowsContains( rows, count, after ) )
			after = after->winGetNext();

		anchor[ slots++ ] = after;
	}

	// The column is not laid out the way we just measured it, so we do not know what we would be
	// permuting. Change nothing.
	if( slots != count )
		return;

	// Already ascending - the authored case, and the case where this has run once already. Returning
	// here is what makes the pass idempotent, and it keeps the untouched screens untouched.
	if( inOrder )
		return;

	// Take every member out. unlinkChildWindow leaves m_prev/m_next dangling and clears m_parent, so
	// nothing may read them between here and the relink; both relink paths below overwrite all three.
	for( Int k = 0; k < count; ++k )
		TheWindowManager->unlinkChildWindow( rows[ k ] );

	// Put them back ascending, each into the position its slot held. Ascending k is what keeps two
	// members that shared an anchor in the right order: insertWindowAheadOf drops the window
	// immediately before the anchor, so the later one lands between the earlier one and the anchor.
	for( Int k = 0; k < count; ++k )
	{
		if( anchor[ k ] != nullptr )
			TheWindowManager->insertWindowAheadOf( rows[ k ], anchor[ k ] );
		else
			TheWindowManager->addWindowToParentAtEnd( rows[ k ], owner );
	}
}

//-------------------------------------------------------------------------------------------------
Int CodeGuiRowsEnsure( GameWindow *parent, const char *idFormat, Int rowCount, CodeGuiRowFit fit,
											 GameWindow *leadRow, Int leadIndex )
{
	if( TheWindowManager == nullptr || TheNameKeyGenerator == nullptr || idFormat == nullptr )
		return 0;
	if( rowCount <= 0 )
		return 0;

	// What did the .wnd actually author? Scanning rather than assuming index 0 exists is load
	// bearing: SkirmishGameOptionsMenu.wnd has ComboBoxPlayer1..7 and no ComboBoxPlayer0, because
	// the local player's row is a text entry instead.
	Int					firstIdx = -1;
	Int					lastIdx	 = -1;
	GameWindow *firstWin = nullptr;
	GameWindow *lastWin	 = nullptr;

	for( Int i = 0; i < rowCount; ++i )
	{
		GameWindow *win = codeGuiFindRow( parent, idFormat, i );
		if( win == nullptr )
			continue;

		if( firstWin == nullptr )
		{
			firstIdx = i;
			firstWin = win;
		}
		lastIdx = i;
		lastWin = win;
	}

	// No such column on this screen: nothing to copy the look FROM.
	if( firstWin == nullptr || lastWin == nullptr )
		return 0;

	// The layout already covers every index the caller asked for. Return before touching a single
	// rect so the shipped MAX_SLOTS keeps EA's geometry bit for bit, and so a second init of the
	// same screen is a no-op rather than a second compression.
	const Int authoredRows = lastIdx + 1;
	if( rowCount <= authoredRows )
		return 0;

	Int firstX, firstY, firstW, firstH;
	firstWin->winGetPosition( &firstX, &firstY );
	firstWin->winGetSize( &firstW, &firstH );

	// GeneralsX: FREE fit needs a template, never a pitch.
	//
	// A free-fit column is positioned by its owner afterwards - positionStartSpots puts the map start
	// markers on the map's Player_N_Start waypoints - so the authored rects carry no layout this
	// function uses. Gating it on the band arithmetic below meant a modded .wnd whose markers sit on
	// one horizontal line, or with marker 7 above marker 0, or with one marker, got no clones at all.
	// Each missing row is cut from the first authored one, at its rect: the owner moves it, and the
	// template's geometry rather than a compressed one keeps it the same size as its neighbours, which
	// is what the anti-overlap pass in positionStartSpotControls measures against. Indices below the
	// first authored row stay absent, as in the band loop below.
	if( fit == CODEGUI_ROWS_FREE )
	{
		Int created = 0;
		for( Int i = firstIdx + 1; i < rowCount; ++i )
		{
			if( codeGuiFindRow( parent, idFormat, i ) == nullptr &&
					codeGuiCloneRow( firstWin, idFormat, firstIdx, i, firstY, firstH ) != nullptr )
				++created;
		}
		return created;
	}

	// BAND fit: a single row has no second one to read a pitch from, and inventing a pitch is exactly
	// what step 1 of the plan forbids.
	if( lastIdx <= firstIdx )
		return 0;

	Int lastX, lastY;
	lastWin->winGetPosition( &lastX, &lastY );

	const Int span = lastIdx - firstIdx;
	if( lastY <= firstY )
		return 0;

	// ROUNDED, not truncated. GameSpyGameOptionsMenu.wnd authors GenericPing0..7 at y 113..280, i.e.
	// 23.857 px apart, while every other column on that screen is a clean 24. Truncating would give
	// the ping column a pitch of 23, a band 8 px shorter than its neighbours', and every compressed
	// row would then drift out of line with the combo box it belongs to.
	const Int pitch = ( lastY - firstY + span / 2 ) / span;
	if( pitch <= 0 )
		return 0;

	// The virtual top of row 0, which is what makes separate columns agree. Each column anchors on
	// its OWN first row, so a control the artist nudged down inside its row - the accept marker sits
	// 3 px below the combo box beside it - keeps that offset instead of being re-centred.
	const Int anchorY = firstY - ( firstIdx * pitch );

	// The band is a whole number of row slots, NOT the distance between the first and last authored
	// rects. Measuring it as rows*pitch is what makes every column on the screen compute the same
	// band and therefore the same new pitch, whatever height its own control happens to be.
	const Int bandHeight = authoredRows * pitch;

	Int newPitch = bandHeight / rowCount;
	Int newH		 = ( firstH * newPitch ) / pitch;		// scale the control with the row it sits in

	// Leave a hairline between rows. The authored layout has firstH == pitch (24 px controls on a
	// 24 px pitch), so the scaled height comes out EQUAL to the new pitch and twelve rows would
	// stack with no seam at all - each row's border touching the next one's. This is a CAP, not an
	// assignment: a control the artist drew shorter than its pitch (the 16 px accept marker on a
	// 24 px pitch) must keep its own proportion rather than be inflated to fill the gap.
	const Int rowGap = CodeGuiAtLeast( CodeGuiScaleY( 2 ), 1 );
	if( newH > newPitch - rowGap )
		newH = newPitch - rowGap;

	// Last resort, and it should never fire at a sane slot count: a row too short to draw a glyph in
	// is worse than a column that overruns its band by a few pixels, so the floor wins and the
	// overrun is accepted rather than shipping invisible controls.
	const Int floorH = CodeGuiAtLeast( CodeGuiScaleY( 8 ), 6 );
	if( newH < floorH )
		newH = floorH;

	// THE ROWS MUST NEVER OVERLAP, and after the floor they can: a floor taller than the pitch would
	// put each row's bottom inside the next row's rect, and since the head of the child list wins
	// the click, the row above would swallow the row below's drop-down arrow. Spending the band
	// instead is the lesser evil, and it is the same overrun the floor comment already accepts.
	// Note this can only fire once the floor has won, i.e. never at a sane slot count - the gap cap
	// above guarantees newH < newPitch otherwise.
	if( newPitch < newH )
		newPitch = newH;

	// The row the column does not author, re-pitched onto the SAME band as the rows it does.
	//
	// Skipping it is what produced the defect this parameter exists for: every ComboBoxPlayer row
	// was compressed 43 px -> 25 px and its face stepped 23 pt -> 13 pt to fit, while
	// TextEntryPlayerName - which is not a ComboBoxPlayer and so never entered the loop below -
	// kept both, drew at nearly twice the height of every row under it, and overhung the next row
	// by the difference. The face was never the odd one out; it was the only one left alone.
	//
	// leadIndex must sit BELOW firstIdx or it would fight a real row for the same slot. Position
	// comes from the band like any other row, so at leadIndex 0 it lands on anchorY, which is
	// exactly where the .wnd authored it - the rect and the face change, the placement does not.
	// A column that does not start at 0 and was given no stand-in has a row this pass will not
	// touch, which is the defect above waiting to happen again on some other screen. It is silent by
	// construction - the missing row simply keeps its authored rect - so say so in the trace rather
	// than let the next reader discover it from a screenshot. Every other ensured column in the game
	// authors index 0 (checked against WindowZH.big, 2026-08-20), so this is expected never to fire.
	if( fit == CODEGUI_ROWS_BAND && firstIdx > 0 && leadRow == nullptr && codeGuiRowFitTraceEnabled() )
	{
		fprintf( stderr, "[GXROWFIT] column '%s' starts at index %d with no leadRow -"
										 " rows 0..%d keep their authored rect and face\n",
						 idFormat, firstIdx, firstIdx - 1 );
	}

	if( fit == CODEGUI_ROWS_BAND && leadRow != nullptr && leadIndex >= 0 && leadIndex < firstIdx )
	{
		Int leadX, leadY;
		leadRow->winGetPosition( &leadX, &leadY );
		leadRow->winSetPosition( leadX, anchorY + ( leadIndex * newPitch ) );
		codeGuiRowResize( leadRow, newH );
	}

	Int created = 0;

	// Start at firstIdx, never at 0. An index BELOW the first authored row is missing on purpose -
	// SkirmishGameOptionsMenu puts a text entry where ComboBoxPlayer0 would be, and creating one
	// there would drop an unbound, unpopulated combo box straight on top of the player's name field.
	for( Int i = firstIdx; i < rowCount; ++i )
	{
		GameWindow *win	 = codeGuiFindRow( parent, idFormat, i );
		const Int		rowY = anchorY + ( i * newPitch );

		if( win == nullptr )
		{
			if( codeGuiCloneRow( firstWin, idFormat, firstIdx, i, rowY, newH ) != nullptr )
				++created;
			continue;
		}

		// Re-pitch the AUTHORED rows too. Creating rows 8..N-1 below an untouched 0..7 would put them
		// straight through the map preview and the chat panel, which is the overrun this whole
		// compression exists to avoid.
		Int winX, winY;
		win->winGetPosition( &winX, &winY );

		win->winSetPosition( winX, rowY );			// keep each row's own x: a column is not always
																						// perfectly aligned in the authored layout
		codeGuiRowResize( win, newH );					// derives the width itself; see the note there
	}

	// The template is the FIRST row and the loop above reaches it first (it starts at firstIdx), so
	// every cloned row was cut from an already-resized, already-re-fonted template and needs no
	// second pass. Say it out loud rather than leave it as a coincidence of loop order.

	// Front-to-back order last, once every row exists. BAND only - the FREE path returned above: a
	// free-fit column is repositioned by its owner afterwards (positionStartSpotControls moves the map
	// markers off the map's own waypoints and has its own anti-overlap pass), so its depth is not a
	// band question and the authored order there is left exactly as the artist wrote it.
	codeGuiRowsRestoreZOrder( parent, idFormat, rowCount );

	return created;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: Scrolling seat rows - see CodeGuiRowsScrollSetup in CodeGui.h.
//-------------------------------------------------------------------------------------------------
enum
{
	CODEGUI_SCROLL_MAX_COLUMNS = 8,
	CODEGUI_SCROLL_MAX_ROWS		 = 64,
	CODEGUI_SCROLL_BAR_W			 = 21,		// GadgetListboxCreateScrollbar: buttonWidth, device pixels at every resolution
	CODEGUI_SCROLL_BUTTON_H		 = 22			// ...and buttonHeight
};

struct CodeGuiRowScroll
{
	GameWindow			 *parent;																// the screen's search root, for the live-screen check
	GameWindow			 *bar;																	// container: owner of the arrows and the slider
	GameWindow			 *slider;
	Int								columns;
	Int								rows;
	Int								visible;
	Int								top;
	Int								pitch;
	GameWindow			 *win[ CODEGUI_SCROLL_MAX_COLUMNS ][ CODEGUI_SCROLL_MAX_ROWS ];
	Int								x[ CODEGUI_SCROLL_MAX_COLUMNS ][ CODEGUI_SCROLL_MAX_ROWS ];
	Int								maxDisplay[ CODEGUI_SCROLL_MAX_COLUMNS ][ CODEGUI_SCROLL_MAX_ROWS ];
	GameWinInputFunc	input[ CODEGUI_SCROLL_MAX_COLUMNS ][ CODEGUI_SCROLL_MAX_ROWS ];
	Int								rowY[ CODEGUI_SCROLL_MAX_COLUMNS ];		// parent-relative y of the first visible row
};

static CodeGuiRowScroll s_rowScroll;
static const char * const CODEGUI_ROW_SCROLL_BAR_ID = "CodeGui:RowScrollBar";

//-------------------------------------------------------------------------------------------------
Int CodeGuiRowsVisibleCap( void )
{
	// GX_LOBBY_ROWS=<4..14> stands in for the platform's cap, so a desktop build can show the iOS layout for a picture.
	// Read once; unset in every shipped launch.
	static Int s_override = -1;
	if( s_override < 0 )
	{
		const char *env = getenv( "GX_LOBBY_ROWS" );
		const Int value = ( env != nullptr ) ? atoi( env ) : 0;
		s_override = ( value >= 4 && value <= 14 ) ? value : 0;
	}
	if( s_override > 0 )
		return s_override;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	return 10;
#else
	return 14;
#endif
}

//-------------------------------------------------------------------------------------------------
/** Put every row where scroll position `top` wants it. Off-window rows are PARKED rather than hidden: the screens
	* hide and show rows themselves (accept markers, the host's own row), and a winHide here would be undone by them. */
//-------------------------------------------------------------------------------------------------
static void codeGuiRowScrollLayout( void )
{
	CodeGuiRowScroll &s = s_rowScroll;
	const Int parkY = -4 * CodeGuiAtLeast( CodeGuiScreenH(), 600 );

	// A list left open on a row that is about to move would hang in the wrong place or vanish with its parked row.
	// A no-op when nothing is open (winSetLoneWindow returns early on an unchanged null).
	TheWindowManager->winSetLoneWindow( nullptr );

	for( Int c = 0; c < s.columns; ++c )
	{
		for( Int i = 0; i < s.rows; ++i )
		{
			GameWindow *win = s.win[ c ][ i ];
			if( win == nullptr )
				continue;

			const Bool shown = ( i >= s.top && i < s.top + s.visible );
			const Int	 y		 = shown ? s.rowY[ c ] + ( i - s.top ) * s.pitch : parkY;
			win->winSetPosition( s.x[ c ][ i ], y );

			// No combo box here knows to open upward, so a list that would run off the bottom of the screen is cut to
			// the entries that fit and scrolls instead (ShowListBox reads maxDisplay each time it opens).
			if( shown && BitIsSet( win->winGetStyle(), GWS_COMBO_BOX ) && s.maxDisplay[ c ][ i ] > 0 )
			{
				GameWindow *list = GadgetComboBoxGetListBox( win );
				Int sx, sy, w, h;
				win->winGetScreenPosition( &sx, &sy );
				win->winGetSize( &w, &h );
				const Int line	= TheWindowManager->winFontHeight( list ? list->winGetFont() : win->winGetFont() ) + 2;
				const Int room	= CodeGuiScreenH() - ( sy + h ) - 4;
				Int fit = ( line > 0 ) ? room / line : s.maxDisplay[ c ][ i ];
				if( fit < 3 )
					fit = 3;
				GadgetComboBoxSetMaxDisplay( win, fit < s.maxDisplay[ c ][ i ] ? fit : s.maxDisplay[ c ][ i ] );
			}
		}
	}

	// Moving a window does not move the keyboard focus (lobby-scroll r1 P2): a focused row parked off screen -
	// the skirmish name field, say - would keep taking keys while invisible. Hand the focus back to the screen.
	for( GameWindow *focus = TheWindowManager->winGetFocus(); focus != nullptr; focus = focus->winGetParent() )
	{
		Bool parked = FALSE;
		for( Int c = 0; c < s.columns && !parked; ++c )
			for( Int i = 0; i < s.rows; ++i )
				if( s.win[ c ][ i ] == focus && ( i < s.top || i >= s.top + s.visible ) )
				{
					parked = TRUE;
					break;
				}
		if( parked )
		{
			TheWindowManager->winSetFocus( nullptr );
			break;
		}
	}

	if( s.slider != nullptr )
		GadgetSliderSetPosition( s.slider, ( s.rows - s.visible ) - s.top );		// the vertical slider's max is its top
}

//-------------------------------------------------------------------------------------------------
static void codeGuiRowScrollTo( Int top )
{
	CodeGuiRowScroll &s = s_rowScroll;
	const Int maxTop = s.rows - s.visible;
	if( top > maxTop )
		top = maxTop;
	if( top < 0 )
		top = 0;
	if( top == s.top )
		return;

	s.top = top;
	codeGuiRowScrollLayout();
}

//-------------------------------------------------------------------------------------------------
/** The scroll bar container's system function: the arrows and the slider report to their owner, which is this. */
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType codeGuiRowScrollSystem( GameWindow *window, UnsignedInt msg,
																										 WindowMsgData mData1, WindowMsgData mData2 )
{
	CodeGuiRowScroll &s = s_rowScroll;
	if( window != s.bar || s.bar == nullptr )
		return MSG_IGNORED;

	switch( msg )
	{
		case GBM_SELECTED:
		{
			GameWindow *button = (GameWindow *)mData1;
			if( button != nullptr && button->winGetParent() == s.bar && button != s.slider )
			{
				Int bx, by;
				button->winGetPosition( &bx, &by );
				codeGuiRowScrollTo( s.top + ( by == 0 ? -1 : 1 ) );		// the up arrow is the one at the top
			}
			return MSG_HANDLED;
		}

		case GSM_SLIDER_TRACK:
			codeGuiRowScrollTo( ( s.rows - s.visible ) - (Int)mData2 );
			return MSG_HANDLED;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
/** Wheel over a seat row scrolls the rows; everything else goes to the row's own input function. An open drop-down
	* keeps the wheel for its own list. */
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType codeGuiRowScrollInput( GameWindow *window, UnsignedInt msg,
																									WindowMsgData mData1, WindowMsgData mData2 )
{
	CodeGuiRowScroll &s = s_rowScroll;

	GameWinInputFunc original = nullptr;
	for( Int c = 0; c < s.columns && original == nullptr; ++c )
		for( Int i = 0; i < s.rows; ++i )
			if( s.win[ c ][ i ] == window )
			{
				original = s.input[ c ][ i ];
				break;
			}

	if( msg == GWM_WHEEL_UP || msg == GWM_WHEEL_DOWN )
	{
		GameWindow *list = BitIsSet( window->winGetStyle(), GWS_COMBO_BOX ) ? GadgetComboBoxGetListBox( window ) : nullptr;
		if( list == nullptr || list->winIsHidden() )
		{
			codeGuiRowScrollTo( s.top + ( msg == GWM_WHEEL_UP ? -1 : 1 ) );
			return MSG_HANDLED;
		}
	}

	return original != nullptr ? original( window, msg, mData1, mData2 ) : MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
/** Step a row's face UP towards `maxPoint` while it still fits a row `height` tall - the inverse of
	* codeGuiRowFitFont, for a band that grew. Only the pointer is reassigned; see codeGuiRowFitFont for why. */
//-------------------------------------------------------------------------------------------------
static void codeGuiRowGrowFont( GameWindow *win, Int height, Int maxPoint )
{
	GameFont *font = win->winGetFont();
	if( font == nullptr )
		return;

	const Int budget = height - codeGuiRowTextTopInset( win );
	const AsciiString name = font->nameString;
	const Bool bold = font->bold;

	for( Int size = maxPoint; size > font->pointSize; --size )
	{
		GameFont *cand = TheWindowManager->winFindFont( name, size, bold );
		if( cand != nullptr && TheWindowManager->winFontHeight( cand ) <= budget )
		{
			win->winSetFont( cand );
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** One part of the scroll bar, built the way GadgetListboxCreateScrollbar builds a listbox's, then dressed in
	* `tmpl`'s own art so it is the same bar the map list and the chat list wear. */
//-------------------------------------------------------------------------------------------------
static GameWindow *codeGuiRowScrollPart( GameWindow *bar, Bool slider, Int y, Int height, Int maxVal, GameWindow *tmpl )
{
	WinInstanceData instData;
	instData.init();
	instData.m_style = ( slider ? GWS_VERT_SLIDER : GWS_PUSH_BUTTON ) | GWS_MOUSE_TRACK;
	const UnsignedInt status = WIN_STATUS_ENABLED | WIN_STATUS_ACTIVE | WIN_STATUS_IMAGE;

	GameWindow *win;
	if( slider )
	{
		SliderData sData;
		memset( &sData, 0, sizeof( sData ) );
		sData.minVal = 0;
		sData.maxVal = maxVal;
		win = TheWindowManager->gogoGadgetSlider( bar, status, 0, y, CODEGUI_SCROLL_BAR_W, height,
																							&instData, &sData, nullptr, TRUE );
		if( win != nullptr && tmpl != nullptr )
			codeGuiAdoptLook( GadgetSliderGetThumb( win ), GadgetSliderGetThumb( tmpl ) );
	}
	else
	{
		win = TheWindowManager->gogoGadgetPushButton( bar, status, 0, y, CODEGUI_SCROLL_BAR_W, height,
																									&instData, nullptr, TRUE );
	}

	if( win != nullptr && tmpl != nullptr )
		codeGuiAdoptLook( win, tmpl );
	return win;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiRowsScrollSetup( GameWindow *parent, const char * const *idFormats, Int columnCount, Int rowCount,
														 GameWindow *chatList, Int chatRows, Int chatKeep, GameWindow *lookListbox, GameWindow *leadRow )
{
	if( TheWindowManager == nullptr || TheNameKeyGenerator == nullptr || parent == nullptr || idFormats == nullptr )
		return;
	if( columnCount <= 0 || columnCount > CODEGUI_SCROLL_MAX_COLUMNS || rowCount <= 0 || rowCount > CODEGUI_SCROLL_MAX_ROWS )
		return;

	CodeGuiRowScroll &s = s_rowScroll;

	// The same live screen again: everything below has already happened to these windows. Just go back to the top.
	if( s.parent == parent && s.bar != nullptr &&
			TheWindowManager->winGetWindowFromId( parent, (Int)TheNameKeyGenerator->nameToKey( CODEGUI_ROW_SCROLL_BAR_ID ) ) == s.bar )
	{
		s.top = 0;
		codeGuiRowScrollLayout();
		return;
	}

	memset( &s, 0, sizeof( s ) );

	//
	// 1. Measure what the .wnd authored, BEFORE any row is created: per column the first authored row, its rect and
	//    the pitch between authored rows.
	//
	Int firstIdx[ CODEGUI_SCROLL_MAX_COLUMNS ], firstY[ CODEGUI_SCROLL_MAX_COLUMNS ], firstH[ CODEGUI_SCROLL_MAX_COLUMNS ];
	Int pitch[ CODEGUI_SCROLL_MAX_COLUMNS ], lastBottom = 0;
	GameWindow *owner = nullptr;

	for( Int c = 0; c < columnCount; ++c )
	{
		firstIdx[ c ] = -1;
		pitch[ c ] = 0;
		Int lastIdx = -1, lastY = 0;
		for( Int i = 0; i < rowCount; ++i )
		{
			GameWindow *win = codeGuiFindRow( parent, idFormats[ c ], i );
			if( win == nullptr )
				continue;
			Int wx, wy, ww, wh;
			win->winGetPosition( &wx, &wy );
			win->winGetSize( &ww, &wh );
			if( firstIdx[ c ] < 0 )
			{
				firstIdx[ c ] = i;
				firstY[ c ] = wy;
				firstH[ c ] = wh;
				if( owner == nullptr )
					owner = win->winGetParent();
			}
			lastIdx = i;
			lastY = wy;
			if( wy + wh > lastBottom )
				lastBottom = wy + wh;
		}
		if( lastIdx > firstIdx[ c ] )
		{
			const Int span = lastIdx - firstIdx[ c ];
			pitch[ c ] = ( lastY - firstY[ c ] + span / 2 ) / span;		// rounded, as CodeGuiRowsEnsure does
		}
	}

	// Column 0 sets the band and the pitch; without two authored rows of it there is nothing to measure.
	if( owner == nullptr || firstIdx[ 0 ] < 0 || pitch[ 0 ] <= 0 )
		return;

	const Int authoredPitch = pitch[ 0 ];
	const Int bandTop = firstY[ 0 ] - firstIdx[ 0 ] * authoredPitch;
	Int bandBottom = bandTop + ( ( lastBottom - bandTop + authoredPitch / 2 ) / authoredPitch ) * authoredPitch;

	//
	// 2. Take up to `chatRows` whole rows off the top of the chat history, as long as it stays at least `chatKeep` tall.
	//    Everything is measured in SCREEN pixels and moved in parent pixels, because the chat and the options can sit
	//    under different sub-windows than the rows. Whole rows at the authored pitch, so the rows keep their authored size.
	//
	Int ownerSX = 0, ownerSY = 0;
	owner->winGetScreenPosition( &ownerSX, &ownerSY );

	if( chatList != nullptr && chatRows > 0 )
	{
		Int lx = 0, chatTop = 0, lw = 0, lh = 0;
		chatList->winGetScreenPosition( &lx, &chatTop );
		chatList->winGetSize( &lw, &lh );
		Int rows = ( lh > chatKeep ) ? ( lh - chatKeep ) / authoredPitch : 0;
		if( rows > chatRows )
			rows = chatRows;
		const Int take = rows * authoredPitch;

		if( take > 0 )
		{
			// The block between the last seat row and the chat (the owner's own children whose top lies in that strip)
			// moves down by what the chat gives up; the chat's top moves down with it and its bottom stays put.
			const Int rowsBottomS = ownerSY + bandBottom;
			for( GameWindow *child = owner->winGetChild(); child != nullptr; child = child->winGetNext() )
			{
				if( child == chatList )
					continue;
				Int cx, cy, px, py;
				child->winGetScreenPosition( &cx, &cy );
				if( cy < rowsBottomS || cy >= chatTop )
					continue;
				child->winGetPosition( &px, &py );
				child->winSetPosition( px, py + take );
			}
			bandBottom += take;

			Int lpx, lpy;
			chatList->winGetPosition( &lpx, &lpy );
			chatList->winSetPosition( lpx, lpy + take );
			chatList->winSetSize( lw, lh - take );
		}
	}

	//
	// 3. How many rows, how tall.
	//
	const Int bandH = bandBottom - bandTop;
	Int visible = bandH / authoredPitch;
	if( visible > CodeGuiRowsVisibleCap() )
		visible = CodeGuiRowsVisibleCap();
	if( visible > rowCount )
		visible = rowCount;
	if( visible < 1 )
		visible = 1;
	const Int newPitch = CodeGuiAtLeast( bandH / visible, authoredPitch );

	//
	// 4. Create the missing rows at their template's rect and look (the FREE fit positions nothing), then collect.
	//
	for( Int c = 0; c < columnCount; ++c )
		if( firstIdx[ c ] >= 0 )
			CodeGuiRowsEnsure( parent, idFormats[ c ], rowCount, CODEGUI_ROWS_FREE );

	s.parent	= parent;
	s.columns = columnCount;
	s.rows		= rowCount;
	s.visible = visible;
	s.pitch		= newPitch;

	// The scroll bar takes its width from the right edge of the rightmost column.
	Int barRight = 0;
	for( Int c = 0; c < columnCount; ++c )
	{
		GameWindow *win = ( firstIdx[ c ] >= 0 ) ? codeGuiFindRow( parent, idFormats[ c ], firstIdx[ c ] ) : nullptr;
		if( win == nullptr || win->winGetParent() != owner )
			continue;
		Int wx, wy, ww, wh;
		win->winGetPosition( &wx, &wy );
		win->winGetSize( &ww, &wh );
		if( wx + ww > barRight )
			barRight = wx + ww;
	}
	const Int barGap = CodeGuiAtLeast( CodeGuiScaleX( 2 ), 2 );
	const Int barX	 = barRight - CODEGUI_SCROLL_BAR_W;
	const Bool scrolls = ( rowCount > visible );

	for( Int c = 0; c < columnCount; ++c )
	{
		if( firstIdx[ c ] < 0 )
			continue;

		const Int colPitch = pitch[ c ] > 0 ? pitch[ c ] : authoredPitch;
		const Int anchorY	 = firstY[ c ] - firstIdx[ c ] * colPitch;

		for( Int i = 0; i < rowCount; ++i )
		{
			GameWindow *win = codeGuiFindRow( parent, idFormats[ c ], i );
			if( win == nullptr && c == 0 && i == 0 )
				win = leadRow;
			if( win == nullptr || win->winGetParent() != owner )
				continue;

			Int wx, wy, ww, wh;
			win->winGetPosition( &wx, &wy );
			win->winGetSize( &ww, &wh );

			const UnsignedInt style = win->winGetStyle();
			const Bool text = BitIsSet( style, GWS_COMBO_BOX ) || BitIsSet( style, GWS_ENTRY_FIELD ) ||
												BitIsSet( style, GWS_STATIC_TEXT );

			if( text )
			{
				// Grow with the row, then step the face up in the same proportion (never past what fits).
				const Int newH = ( firstH[ c ] * newPitch ) / authoredPitch;
				codeGuiRowResize( win, newH );
				GameFont *font = win->winGetFont();
				if( font != nullptr && newPitch > authoredPitch )
					codeGuiRowGrowFont( win, newH, ( font->pointSize * newPitch + authoredPitch / 2 ) / authoredPitch );

				// Make room for the scroll bar.
				if( scrolls && wx + ww > barX - barGap )
				{
					Int nw, nh;
					win->winGetSize( &nw, &nh );
					win->winSetSize( CodeGuiAtLeast( barX - barGap - wx, 8 ), nh );
				}
			}

			s.win[ c ][ i ] = win;
			s.x[ c ][ i ]		= wx;
			if( BitIsSet( style, GWS_COMBO_BOX ) )
			{
				const ComboBoxData *data = (const ComboBoxData *)win->winGetUserData();
				s.maxDisplay[ c ][ i ] = data ? data->maxDisplay : 0;
			}

			// The wheel, unless the window takes no input at all (the accept marker).
			if( BitIsSet( win->winGetStatus(), WIN_STATUS_NO_INPUT ) == FALSE )
			{
				s.input[ c ][ i ] = win->winGetInputFunc();
				win->winSetInputFunc( codeGuiRowScrollInput );
			}
		}

		// Text controls start at the row's top; anything shorter (icons) keeps its authored offset, centred in the
		// taller row.
		GameWindow *first = s.win[ c ][ firstIdx[ c ] ];
		const Bool text = first != nullptr && ( BitIsSet( first->winGetStyle(), GWS_COMBO_BOX ) ||
																						BitIsSet( first->winGetStyle(), GWS_ENTRY_FIELD ) ||
																						BitIsSet( first->winGetStyle(), GWS_STATIC_TEXT ) );
		s.rowY[ c ] = text ? anchorY : anchorY + ( newPitch - authoredPitch ) / 2;
	}

	// Depth: ascending rows, so an open list paints over the rows below it.
	for( Int c = 0; c < columnCount; ++c )
		codeGuiRowsRestoreZOrder( parent, idFormats[ c ], rowCount );

	//
	// 5. The scroll bar: arrows at both ends, the slider between, inside a see-through container that owns them.
	//
	if( scrolls )
	{
		const Int barH = visible * newPitch;
		s.bar = CodeGuiPanel( owner, barX, bandTop, CODEGUI_SCROLL_BAR_W, barH, codeGuiRowScrollSystem,
													CODEGUI_ROW_SCROLL_BAR_ID, WIN_STATUS_SEE_THRU );
		// A listbox authored without a scroll bar has no art to lend (SkirmishGameOptionsMenu's ListboxInfo), and the
		// default VSliderLarge* images are not in the shipped data, so a bar dressed by neither is INVISIBLE - measured on
		// the Mini 08/11, where the skirmish bar scrolled but could not be seen. Every seat combo box's drop-down list
		// carries authored scroll-bar art; borrow the first one's.
		if( lookListbox == nullptr || GadgetListBoxGetSlider( lookListbox ) == nullptr )
		{
			lookListbox = nullptr;
			for( Int c = 0; c < columnCount && lookListbox == nullptr; ++c )
				for( Int i = 0; i < rowCount; ++i )
				{
					GameWindow *row = s.win[ c ][ i ];
					GameWindow *list = ( row != nullptr && BitIsSet( row->winGetStyle(), GWS_COMBO_BOX ) ) ?
															 GadgetComboBoxGetListBox( row ) : nullptr;
					if( list != nullptr && GadgetListBoxGetSlider( list ) != nullptr )
					{
						lookListbox = list;
						break;
					}
				}
		}

		if( s.bar != nullptr )
		{
			GameWindow *upLook = lookListbox ? GadgetListBoxGetUpButton( lookListbox ) : nullptr;
			GameWindow *downLook = lookListbox ? GadgetListBoxGetDownButton( lookListbox ) : nullptr;
			GameWindow *sliderLook = lookListbox ? GadgetListBoxGetSlider( lookListbox ) : nullptr;

			codeGuiRowScrollPart( s.bar, FALSE, 0, CODEGUI_SCROLL_BUTTON_H, 0, upLook );
			codeGuiRowScrollPart( s.bar, FALSE, barH - CODEGUI_SCROLL_BUTTON_H, CODEGUI_SCROLL_BUTTON_H, 0, downLook );
			s.slider = codeGuiRowScrollPart( s.bar, TRUE, CODEGUI_SCROLL_BUTTON_H + 1,
																			 CodeGuiAtLeast( barH - 2 * CODEGUI_SCROLL_BUTTON_H - 2, GADGET_SIZE ),
																			 rowCount - visible, sliderLook );
		}
	}

	codeGuiRowScrollLayout();
}

//-------------------------------------------------------------------------------------------------
WindowLayout *CodeGuiMakeLayout( GameWindow *root )
{
	if( root == nullptr )
		return nullptr;

	// Public ctor + MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE, so newInstance works from any TU.
	// We never call load(), so nothing touches a .wnd and the filename stays empty. That is safe
	// ONLY because this layout is never put on m_screenStack — Shell::recreateWindowLayouts()
	// re-pushes by filename and would fail on it.
	WindowLayout *layout = newInstance(WindowLayout);
	layout->addWindow( root );

	return layout;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiDestroyLayout( WindowLayout *&layout )
{
	if( layout == nullptr )
		return;

	layout->destroyWindows();		// deferred winDestroy, recurses into children -> safe in a handler
	deleteInstance(layout);
	layout = nullptr;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: Lend the row builder to the load screens, which are in Core.
//
// LoadScreen.cpp is shared with the Generals twin and cannot call CodeGuiRowsEnsure - that function
// reaches the widget factories above it in this file, and the twin has no CodeGui at all. So Core
// declares a hook (LoadScreen.h) and this registers the implementation.
//
// SELF-REGISTERING, on purpose. A call from some init function is a line somebody has to remember
// to add, and the failure mode of forgetting it is silent: the load screen simply keeps showing
// eight of twelve, which is exactly the bug this fixes and exactly how it survived the crash fix.
// A file-scope object cannot be forgotten as long as this translation unit is linked, and it is -
// every lobby screen calls into it.
//
// Static-initialisation order is not a hazard here: s_loadScreenRowBuilder is a raw pointer with
// static storage duration, so it is ZERO-initialised before any dynamic initialiser in the program
// runs. This constructor can therefore only ever overwrite a null, never race a later zeroing.
//
// BAND fit, matching the lobby: the load screen's player rows are a column that must stay inside
// the band the authored rows occupy. The map start markers come through with placedByCaller and
// get FREE fit instead - they are placed from the map's own waypoints rather than laid out in a
// column, so band-fitting them would move them off the preview.
static Int codeGuiLoadScreenRowBuilder( GameWindow *parent, const char *idFormat, Int rowCount,
																				Bool placedByCaller )
{
	return CodeGuiRowsEnsure( parent, idFormat, rowCount,
														placedByCaller ? CODEGUI_ROWS_FREE : CODEGUI_ROWS_BAND );
}

//-------------------------------------------------------------------------------------------------
/** Step a static text's face down until its text fits `maxWidth` pixels - a header narrowed into half the span would
	* otherwise run into the one beside it ("Team" under "Progress" read "Tear Progress", Mini picture 08/11). */
//-------------------------------------------------------------------------------------------------
static void codeGuiFitTextWidth( GameWindow *win, Int maxWidth )
{
	if( win == nullptr || TheDisplayStringManager == nullptr || maxWidth <= 0 )
		return;
	GameFont *font = win->winGetFont();
	const UnicodeString text = GadgetStaticTextGetText( win );
	if( font == nullptr || text.isEmpty() )
		return;
	DisplayString *ds = TheDisplayStringManager->newDisplayString();
	if( ds == nullptr )
		return;
	ds->setText( text );

	const AsciiString name = font->nameString;
	const Bool bold = font->bold;
	for( Int size = font->pointSize; size >= CODEGUI_ROW_MIN_POINT_SIZE; --size )
	{
		GameFont *cand = ( size == font->pointSize ) ? font : TheWindowManager->winFindFont( name, size, bold );
		if( cand == nullptr )
			continue;
		ds->setFont( cand );
		if( ds->getWidth() <= maxWidth )
		{
			if( cand != font )
				win->winSetFont( cand );
			break;
		}
	}
	TheDisplayStringManager->freeDisplayString( ds );
}

//-------------------------------------------------------------------------------------------------
/** GeneralsX: The load screen's player list, once it knows how many rows it filled (see
	* LoadScreenSplitRows in LoadScreen.h). Up to the authored row count it touches nothing - the rows are EA's. Past it
	* the columns become two side-by-side groups over the same horizontal span, each with its own copy of the header row,
	* row i in group i / R at row i % R, R = half the filled rows rounded up. The groups share the authored band: at the
	* authored pitch while R fits it, compressed (and the face stepped down) past that, so every row stays in the frame. */
//-------------------------------------------------------------------------------------------------
static void codeGuiLoadScreenRowSplitter( GameWindow *parent, const char * const *idFormats, Int columnCount,
																					const char * const *titleNames, Int titleCount, Int usedRows )
{
	if( TheWindowManager == nullptr || parent == nullptr || idFormats == nullptr || columnCount <= 0 )
		return;

	// The authored rows of the first column: consecutive rows that step DOWN. Rows created placedByCaller sit on row 0's
	// rect, so the run stops at the first of them.
	GameWindow *first = codeGuiFindRow( parent, idFormats[ 0 ], 0 );
	if( first == nullptr )
		return;
	Int authored = 1, prevY = 0, y0 = 0, lastY = 0, tmp = 0;
	first->winGetPosition( &tmp, &y0 );
	prevY = y0;
	lastY = y0;
	for( Int i = 1; ; ++i )
	{
		GameWindow *win = codeGuiFindRow( parent, idFormats[ 0 ], i );
		if( win == nullptr )
			break;
		Int wy;
		win->winGetPosition( &tmp, &wy );
		if( wy <= prevY )
			break;
		prevY = wy;
		lastY = wy;
		authored = i + 1;
	}
	if( usedRows <= authored || authored < 2 )
		return;

	const Int pitch		 = ( lastY - y0 + ( authored - 1 ) / 2 ) / ( authored - 1 );
	const Int perGroup = ( usedRows + 1 ) / 2;
	const Int newPitch = ( perGroup <= authored ) ? pitch : ( authored * pitch ) / perGroup;
	if( pitch <= 0 || newPitch <= 0 )
		return;

	// The horizontal span every column and header shares.
	Int left = 0x7fffffff, right = 0;
	for( Int c = 0; c < columnCount; ++c )
	{
		GameWindow *win = codeGuiFindRow( parent, idFormats[ c ], 0 );
		if( win == nullptr )
			continue;
		Int x, y, w, h;
		win->winGetPosition( &x, &y );
		win->winGetSize( &w, &h );
		if( x < left )
			left = x;
		if( x + w > right )
			right = x + w;
	}
	if( right <= left )
		return;
	const Int span		= right - left;
	const Int gap			= CodeGuiAtLeast( CodeGuiScaleX( 12 ), 8 );
	const Int groupW	= ( span - gap ) / 2;
	const Int groupDx = groupW + gap;

	for( Int c = 0; c < columnCount; ++c )
	{
		GameWindow *win0 = codeGuiFindRow( parent, idFormats[ c ], 0 );
		if( win0 == nullptr )
			continue;
		Int x0, cy0, w0, h0;
		win0->winGetPosition( &x0, &cy0 );
		win0->winGetSize( &w0, &h0 );
		const Int nx = left + ( ( x0 - left ) * groupW ) / span;
		const Int nw = CodeGuiAtLeast( ( w0 * groupW ) / span, 8 );
		const Int nh = CodeGuiAtLeast( ( h0 * newPitch ) / pitch, 6 );

		for( Int i = 0; i < usedRows; ++i )
		{
			GameWindow *win = codeGuiFindRow( parent, idFormats[ c ], i );
			if( win == nullptr )
				continue;
			const Int g = i / perGroup;
			const Int r = i % perGroup;
			win->winSetPosition( nx + g * groupDx, cy0 + r * newPitch );
			win->winSetSize( nw, nh );
			if( BitIsSet( win->winGetStyle(), GWS_STATIC_TEXT ) )
				codeGuiRowFitFont( win, nh );
		}
	}

	// The header row: narrowed into the first group, and a copy of each header over the second.
	for( Int t = 0; titleNames != nullptr && t < titleCount; ++t )
	{
		if( titleNames[ t ] == nullptr )
			continue;
		GameWindow *title = TheWindowManager->winGetWindowFromId( parent,
			(Int)TheNameKeyGenerator->nameToKey( titleNames[ t ] ) );
		if( title == nullptr )
			continue;
		Int x, y, w, h;
		title->winGetPosition( &x, &y );
		title->winGetSize( &w, &h );
		const Int nx = left + ( ( x - left ) * groupW ) / span;
		const Int nw = CodeGuiAtLeast( ( w * groupW ) / span, 8 );
		title->winSetPosition( nx, y );
		title->winSetSize( nw, h );
		const Int textRoom = nw - CodeGuiAtLeast( CodeGuiScaleX( 4 ), 2 );
		codeGuiFitTextWidth( title, textRoom );

		// codeGuiCloneRow names the copy "<title>GX1"; tmplIndex == index skips the transition mirror, which would look
		// for a "<title>GX" group entry nobody wrote.
		AsciiString cloneFormat( titleNames[ t ] );
		cloneFormat.concat( "GX%d" );
		GameWindow *copy = codeGuiCloneRow( title, cloneFormat.str(), 1, 1, y, h );
		if( copy == nullptr )
			continue;
		copy->winSetPosition( nx + groupDx, y );
		copy->winSetSize( nw, h );
		if( BitIsSet( title->winGetStyle(), GWS_STATIC_TEXT ) )
		{
			GadgetStaticTextSetText( copy, GadgetStaticTextGetText( title ) );
			if( title->winGetFont() != nullptr )
				copy->winSetFont( title->winGetFont() );		// the face the narrowed original settled on
		}
	}
}

namespace
{
	struct CodeGuiLoadScreenRegistrar
	{
		CodeGuiLoadScreenRegistrar()
		{
			LoadScreenSetRowBuilder( codeGuiLoadScreenRowBuilder );
			LoadScreenSetRowSplitter( codeGuiLoadScreenRowSplitter );		// GeneralsX 08/11/2026
		}
	};
	CodeGuiLoadScreenRegistrar s_codeGuiLoadScreenRegistrar;
}
