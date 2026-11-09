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

// FILE: GameWindowManagerScript.cpp //////////////////////////////////////////
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
// File name: GameWindowManagerScript.cpp
//
// Created:   Colin Day, June 2001
//						Dean Iverson, May 1998
//
// Desc:      Reading window definition files from disk for the window manager
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "Common/Debug.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameMemory.h"
#include "Common/NameKeyGenerator.h"
#include "Common/FunctionLexicon.h"
#include "GameClient/Display.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/LayoutScale.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowGlobal.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTabControl.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GameText.h"
#include "GameClient/HeaderTemplate.h"



// DEFINES ////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// PRIVATE TYPES //////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

enum
{
	WIN_BUFFER_LENGTH  = 2048,
	WIN_STACK_DEPTH    = 10,
};

//-------------------------------------------------------------------------------------------------
/** Layout parse structure ... these data items apply to the window file itself,
	* they are not associated with any window, but rather just a block of data in
	* every file */
//-------------------------------------------------------------------------------------------------
struct LayoutScriptParse
{

	const char *name;
	Bool (*parse)( const char *token, char *buffer, UnsignedInt version, WindowLayoutInfo *info );

};

// GameWindowParse ------------------------------------------------------------
/** used to match database fields to parsing functions */
//-----------------------------------------------------------------------------
struct GameWindowParse
{

	const char *name;
	Bool (*parse)( const char *token, WinInstanceData *, char *, void * );

};

///////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// window methods and their string representations
static GameWinSystemFunc		systemFunc = nullptr;
static GameWinInputFunc		inputFunc = nullptr;
static GameWinTooltipFunc	tooltipFunc = nullptr;
static GameWinDrawFunc			drawFunc = nullptr;
static AsciiString theSystemString;
static AsciiString theInputString;
static AsciiString theTooltipString;
static AsciiString theDrawString;

// default visual properties
static Color defEnabledColor		= 0;
static Color defDisabledColor		= 0;
static Color defBackgroundColor	= 0;
static Color defHiliteColor			= 0;
static Color defSelectedColor		= 0;
static Color defTextColor				= 0;
static GameFont  *defFont				= nullptr;

//
// These strings must be in the same order as they are in their definitions
// (see WIN_STATUS_* enums and GWS_* enums).
//
const char *const WindowStatusNames[] = { "ACTIVE", "TOGGLE", "DRAGABLE", "ENABLED", "HIDDEN",
														  "ABOVE", "BELOW", "IMAGE", "TABSTOP", "NOINPUT",
														  "NOFOCUS", "DESTROYED", "BORDER",
														  "SMOOTH_TEXT", "ONE_LINE", "NO_FLUSH", "SEE_THRU",
															"RIGHT_CLICK", "WRAP_CENTERED", "CHECK_LIKE","HOTKEY_TEXT",
															"USE_OVERLAY_STATES", "NOT_READY", "FLASHING", "ALWAYS_COLOR",
															"ON_MOUSE_DOWN", /*"SHORTCUT_BUTTON",*/
															nullptr };

const char *const WindowStyleNames[] = { "PUSHBUTTON",	"RADIOBUTTON",	"CHECKBOX",
														 "VERTSLIDER",	"HORZSLIDER",		"SCROLLLISTBOX",
														 "ENTRYFIELD",	"STATICTEXT",		"PROGRESSBAR",
														 "USER",				"MOUSETRACK",		"ANIMATED",
														 "TABSTOP",			"TABCONTROL",		"TABPANE",
														 "COMBOBOX",
														 nullptr };

// Implement a stack to keep track of parent/child nested window descriptions.
static GameWindow *windowStack[ WIN_STACK_DEPTH ];
static GameWindow **stackPtr;

// for parsing
static const char *seps = " =;\n\r\t";
WinDrawData enabledDropDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData disabledDropDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData hiliteDropDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes

WinDrawData enabledEditBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData disabledEditBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData hiliteEditBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes

WinDrawData enabledListBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData disabledListBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes
WinDrawData hiliteListBoxDrawData[ MAX_DRAW_DATA ];  ///< for combo boxes

WinDrawData enabledUpButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData disabledUpButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData hiliteUpButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData enabledDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData disabledDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData hiliteDownButtonDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData enabledSliderDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData disabledSliderDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData hiliteSliderDrawData[ MAX_DRAW_DATA ];  ///< for list boxes and combo boxes
WinDrawData enabledSliderThumbDrawData[ MAX_DRAW_DATA ];  ///< for sliders and list boxes and combo boxes
WinDrawData disabledSliderThumbDrawData[ MAX_DRAW_DATA ];  ///< for sliders and list boxes and combo boxes
WinDrawData hiliteSliderThumbDrawData[ MAX_DRAW_DATA ];  ///< for sliders and list boxes and combo boxes

// PUBLIC DATA ////////////////////////////////////////////////////////////////

// PRIVATE PROTOTYPES /////////////////////////////////////////////////////////
static GameWindow *parseWindow( File *inFile, char *buffer );

///////////////////////////////////////////////////////////////////////////////
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// parseBitFlag ===============================================================
/** Parse one of the "flags" referred to below in the header comment
	* for ParseBitString().  Sets the appropriate bit in the 'bits' arg,
	* if successful.  Returns TRUE on success, else FALSE. */
//=============================================================================
static Bool parseBitFlag( const char *flagString, UnsignedInt *bits,
													ConstCharPtrArray flagList )
{
	ConstCharPtrArray c;
	int i;

	for( i = 0, c = flagList; *c; i++, c++ )
	{

		if( stricmp( *c, flagString ) == 0 )
		{
			*bits |= (1 << i);
			return TRUE;
		}

	}

	return FALSE;

}

// parseBitString =============================================================
/** Given a character string of the form 'A+B+C+D', parse the
	* flags separated by the '+' symbols into a bitfield stored in the 'bits'
	* argument.
	* Note that this routine does not clear any bits, only sets them. */
//=============================================================================
static void parseBitString( const char *inBuffer, UnsignedInt *bits, ConstCharPtrArray flagList )
{
	char buffer[256];
	char *tok;

	// do not modify the inBuffer argument
	strlcpy(buffer, inBuffer, ARRAY_SIZE(buffer));

	if( strncmp( buffer, "NULL", 4 ) != 0 )
	{
		for( tok = strtok( buffer, "+" ); tok; tok = strtok( nullptr, "+" ) )
		{
			if ( !parseBitFlag( tok, bits, flagList ) )
			{
				DEBUG_LOG(( "ParseBitString: Invalid flag '%s'.", tok ));
			}
		}
	}

}

// readUntilSemicolon =========================================================
//=============================================================================
static void readUntilSemicolon( File *fp, char *buffer, int maxBufLen )
{
	int i = 0;
	Bool start = TRUE;

	while( i < maxBufLen )
	{

		// get next character
		fp->read(buffer + i, 1);

		// make all whitespace characters spaces
		if( isspace( buffer[ i ] ) )
		{

			if( start == FALSE )
				buffer[ i++ ] = ' ';

		}
		else
		{

			start = FALSE;

			if( buffer[ i ] == ';' )
			{

				// found end of data chunk
				buffer[ i ] = '\000';
				return;

			}

			i++;
		}

	}

	DEBUG_LOG(( "ReadUntilSemicolon: ERROR - Read buffer overflow - input truncated." ));

	buffer[ maxBufLen - 1 ] = '\000';

}

// scanBool ===================================================================
//=============================================================================
static Int scanBool( const char *source, Bool& val )
{
	Int temp = 0;
	Int ret = sscanf( source, "%d", &temp );
	val = (Bool)temp;

	return ret;
}

// scanShort ==================================================================
//=============================================================================
static Int scanShort( const char *source, Short& val )
{
	Int temp = 0;
	Int ret = sscanf( source, "%d", &temp );
	val = (Short)temp;

	return ret;
}

// scanInt ====================================================================
//=============================================================================
static Int scanInt( const char *source, Int& val )
{
	Int ret = sscanf( source, "%d", &val ); // not strictly necessary to wrap this, but it's more consistent

	return ret;
}

// scanUnsignedInt ============================================================
//=============================================================================
static Int scanUnsignedInt( const char *source, UnsignedInt& val )
{
	Int ret = sscanf( source, "%d", &val ); // not strictly necessary to wrap this, but it's more consistent

	return ret;
}

// resetWindowStack ===========================================================
//=============================================================================
static void resetWindowStack()
{

  memset( windowStack, 0, sizeof( windowStack ) );
  stackPtr = windowStack;

}

// resetWindowDefaults ========================================================
//=============================================================================
static void resetWindowDefaults()
{

	defEnabledColor = 0;
	defDisabledColor = 0;
	defBackgroundColor = 0;
	defHiliteColor = 0;
	defSelectedColor = 0;
	defTextColor = 0;
	defFont = nullptr;

}

// peekWindow =================================================================
//=============================================================================
static GameWindow *peekWindow()
{
  if (stackPtr == windowStack)
    return nullptr;

  return *(stackPtr - 1);

}

// popWindow ==================================================================
//=============================================================================
static GameWindow *popWindow()
{

  if( stackPtr == windowStack )
    return nullptr;
  stackPtr--;

  return *stackPtr;

}

// pushWindow =================================================================
//=============================================================================
static void pushWindow( GameWindow *window )
{

  if( stackPtr == &windowStack[ WIN_STACK_DEPTH - 1 ] )
	{

    DEBUG_LOG(( "pushWindow: Warning, stack overflow" ));
    return;

  }

  *stackPtr++ = window;

}

// parseColor =================================================================
/** Parse a color entry and store it in the value pointed to by the
	* 'color' parm. */
//=============================================================================
static Bool parseColor( Color *color, char *buffer )
{
  char *c;
  Byte red, green, blue;

	c = strtok( buffer, " \t\n\r" );
  red = atoi(c);

	c = strtok( nullptr, " \t\n\r" );
  green = atoi(c);

	c = strtok( nullptr, " \t\n\r" );
  blue = atoi(c);

	*color = TheWindowManager->winMakeColor( red, green, blue, 255 );

  return TRUE;

}

// parseDefaultColor ==========================================================
/** Parse a default color entry and store it in the value pointed to by
	* the 'color' parm. */
//=============================================================================
static Bool parseDefaultColor( Color *color, File *inFile, char *buffer )
{
	// eat '='
//	fscanf( inFile, "%*s" );
	AsciiString str;
	inFile->scanString(str);

  // Read the rest of the color definition
	readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

  if (strcmp( buffer, "TRANSPARENT" ) == 0)
	{

		*color = WIN_COLOR_UNDEFINED;

	}
  else
    parseColor( color, buffer );

  return TRUE;

}

// parseDefaultFont ===========================================================
/** Parse the default font */
//=============================================================================
static Bool parseDefaultFont( GameFont *font, File *inFile, char *buffer )
{

	// eat '='
//	fscanf( inFile, "%*s" );
	AsciiString str;
	inFile->scanString(str);

  // Read the rest of the color definition
	readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

	/// @todo font parsing for window files work needed here
//	*font = GetFont( buffer );
//	if( *font == nullptr )
//		return FALSE;

	return TRUE;

}

// parseTooltip ===============================================================
/** Parse the tooltip field */
//=============================================================================
static Bool parseTooltip( const char *token, WinInstanceData *instData,
													char *buffer, void *data )
{
	UnicodeString tooltip;
	tooltip.set(L"Need tooltip translation");
	/// @todo need to parse the tooltip in multibyte here

	instData->setTooltipText( tooltip );
	return TRUE;

}

// GeneralsX @probe Claude 20/08/2026 Which layout the parser is inside. Set by winCreateFromScript,
// read by the GX_FONT traces below - a scale printed without the layout it belongs to is what made
// the previous session record SkirmishGameOptionsMenu.wnd as authored at 1920x1080 when it is
// authored at 800x600 like every other retail menu. The parser handles one file at a time.
static const char *s_gxParsingLayout = "(none)";

// GeneralsX @bugfix Claude 25/08/2026 The row/column groups for the layout being parsed.
//
// parseScreenRect maps one window at a time, and parent and sibling links are applied after the
// mapping, so at map time there is nothing to anchor a ROW on - which is why ExtrasMenu.wnd's
// label (100..350, anchors left) and its slider (370..650, anchors right) drifted 366px apart at
// 1920x1080. gxScanLayoutGroups fills this in a cheap first pass over the file text before the
// real parse starts; parseScreenRect then looks each authored rect up. See GameClient/LayoutScale.h.
//
// Empty means "no groups", which is byte-for-byte the behaviour that predates this. It is reset at
// the top of every winCreateFromScript so one layout's groups can never leak into the next.
static LayoutScaleGroupTable s_gxLayoutGroups;

// GeneralsX @bugfix Claude 25/08/2026 Pass one: collect the layout's authored rects and group them,
// before a single window exists.
//
// A SECOND OPEN of the same file rather than a rewind of the parse handle. The parse handle has
// already had its version header and its layout block consumed by the time the first SCREENRECT is
// read, so rewinding it would tie two passes together that otherwise need to know nothing about
// each other - and the version parse would have to be re-entrant. A .wnd is tens of kilobytes and
// a menu is loaded once, so the read is cheaper than the coupling.
//
// BINARY, like INI::prepFile: in text mode on Windows size() counts the CRLFs that the read then
// collapses, and readEntireAndClose leaves that many bytes of the buffer uninitialised.
//
// UNIFORM ONLY. In the shipped STRETCH mode the group is never consulted, so this does not run and
// the default path does exactly the I/O it has always done.
static void gxScanLayoutGroups( const char *filepath )
{
	LayoutScaleGroupsReset( &s_gxLayoutGroups );

	if( LayoutScaleActiveMode() != LAYOUT_SCALE_UNIFORM )
		return;

	File *scanFile = TheFileSystem->openFile( filepath, File::READ | File::BINARY );
	if( scanFile == nullptr )
		return;												// the real open below reports it; do not log it twice

	const Int len = scanFile->size();
	char *text = scanFile->readEntireAndClose();		// closes (and deletes) the handle; buffer is ours
	if( text == nullptr )
		return;

	if( len > 0 )
		LayoutScaleGroupsScanLayout( &s_gxLayoutGroups, text, len );

	delete [] text;
}

// GeneralsX @probe Claude 20/08/2026 GX_FONT level: 1 = per-string font at draw time,
// 2 = adds the layout-tree dump and the header-template override trace.
static Int gxFontDiagLevel(void)
{
	static Int s_level = -1;
	if( s_level < 0 )
	{
		const char *env = getenv("GX_FONT");
		s_level = (env != NULL) ? atoi(env) : 0;
		if( s_level < 0 )
			s_level = 0;
	}
	return s_level;
}

// parseScreenRect ============================================================
/** Parse the screen rect entry which tells us the position and size
	* of window.  Note we scale for the current resolution if needed
	* and adjust to make the screen rect coords relative to any parent
	* if present */
//=============================================================================
// GeneralsX @bugfix Claude 19/08/2026 The scale the current window's SCREENRECT was resized by, so
// parseFont can size the text to match the box. Set in parseScreenRect, read in parseFont, and the
// parser handles one window at a time in a single thread.
static Real s_gxLayoutFontScale = 1.0f;

// GeneralsX @feature Claude 20/08/2026 The factor text must share with the box it sits in.
//
// In UNIFORM the boxes are scaled by min(xScale,yScale) x the dial. Text that does not move by the
// SAME factor overflows a box that got narrower - OBSERVED on the skirmish screen, where the boxes
// shrank 33% horizontally while every header-template font stayed at adjustFontSize's width-driven
// 1.98 and "Starting Cash" wrapped and clipped.
//
// Zero in STRETCH: this returns 1.0 there, so the shipped look is untouched. That is the whole
// reason it is a factor and not a mode switch inside parseFont.
static Real gxLayoutTextFactor( void )
{
	Real f = s_gxLayoutFontScale;
	if( LayoutScaleActiveMode() == LAYOUT_SCALE_UNIFORM )
		f *= LayoutScaleClampDial( LayoutScaleActiveDial() );   // same clamp the rects use
	return f;
}

static Bool parseScreenRect( const char *token, char *buffer,
														 Int *x, Int *y, Int *width, Int *height )
{
	GameWindow *parent = peekWindow();
	IRegion2D screenRegion;
	ICoord2D createRes;  // creation resolution
	const char *seps = " ,:=\n\r\t";
	char *c;

	c = strtok( nullptr, seps );  // UPPERLEFT token
	c = strtok( nullptr, seps );  // x position
	scanInt( c, screenRegion.lo.x );
	c = strtok( nullptr, seps );  // y posotion
	scanInt( c, screenRegion.lo.y );

	c = strtok( nullptr, seps );  // BOTTOMRIGHT token
	c = strtok( nullptr, seps );  // x position
	scanInt( c, screenRegion.hi.x );
	c = strtok( nullptr, seps );  // y posotion
	scanInt( c, screenRegion.hi.y );

	c = strtok( nullptr, seps );  // CREATIONRESOLUTION token
	c = strtok( nullptr, seps );  // x creation resolution
	scanInt( c, createRes.x );
	c = strtok( nullptr, seps );  // y creation resolution
	scanInt( c, createRes.y );

	//
	// shrink or expand the screen region by the ratio of the current
	// resolution divided by the creation resolution
	//
	Real xScale = (Real)TheDisplay->getWidth() / (Real)createRes.x;
	Real yScale = (Real)TheDisplay->getHeight() / (Real)createRes.y;

	// GeneralsX @bugfix Claude 19/08/2026 Remember this window's scale so its FONT can use it too.
	//
	// Measured 1920x1080: a layout row's text is created at its raw authored size and drawn 1:1 (13pt
	// -> 18px glyphs, extent == raster height, no upscaling anywhere), while the box around it is
	// scaled by the ratio below. Small text in a big row, which is what the operator saw as "the font
	// is pixelized for those but not the buttons" - the headers escape only because they happen to be
	// authored at 33pt.
	//
	// Deliberately NOT GlobalLanguage::adjustFontSize(), which is what this looks like it wants: that
	// scales by display/DEFAULT_DISPLAY_WIDTH (a fixed 800) and CAPS at 2.0, while the coordinates
	// here scale by display/createRes with no cap. Two different denominators and a cap on one side
	// means the text still would not match its box. The only factor that keeps text proportional to
	// the box it sits in is the box's own factor, which is this one.
	//
	// min() rather than either axis alone: a layout stretched more horizontally than vertically would
	// otherwise get text too tall for its row. SCREENRECT is parsed before the field loop that reads
	// FONT (the assert above this call site guarantees the order), so this is always the current
	// window's scale and never a neighbour's.
	s_gxLayoutFontScale = (xScale < yScale) ? xScale : yScale;
	if( s_gxLayoutFontScale <= 0.0f )
		s_gxLayoutFontScale = 1.0f;

	// GeneralsX @probe Claude 19/08/2026 The number the whole theory rests on. Applying the scale to
	// the font changed NOTHING measurable, which means it is 1.0 - i.e. the layouts are authored at
	// the display resolution and the coordinates are not being scaled either. Print it rather than
	// infer it a third time.
	{
		static Int s_on = -1;
		if( s_on < 0 )
		{
			const char *env = getenv("GX_FONT");
			s_on = (env != NULL && atoi(env) > 0) ? 1 : 0;
		}
		if( s_on )
		{
			// Once per LAYOUT, and named. A global print cap reported only the first file parsed -
			// ControlBar.wnd, which 340_ControlBarProZH.big overrides with a 1920x1080-authored copy -
			// and every later layout inherited that reading by association.
			static std::set<std::string> s_seenLayout;
			if( s_seenLayout.insert(std::string(s_gxParsingLayout)).second )
			{
				fprintf(stderr, "[GXFONT] layout '%s' createRes=%dx%d display=%dx%d xScale=%.3f yScale=%.3f used=%.3f\n",
					s_gxParsingLayout,
					createRes.x, createRes.y, TheDisplay->getWidth(), TheDisplay->getHeight(),
					xScale, yScale, s_gxLayoutFontScale);
				fflush(stderr);
			}
		}
	}
	// GeneralsX @feature Claude 20/08/2026 See GameClient/LayoutScale.h. Identical arithmetic in the
	// default STRETCH mode - the four casts moved out of this function, they did not change.
	{
		LayoutScaleContext lsCtx;
		lsCtx.createW	 = createRes.x;
		lsCtx.createH	 = createRes.y;
		lsCtx.displayW = TheDisplay->getWidth();
		lsCtx.displayH = TheDisplay->getHeight();
		lsCtx.mode		 = LayoutScaleActiveMode();
		lsCtx.uiScale	 = LayoutScaleActiveDial();

		LayoutRect authoredRect, mappedRect;
		authoredRect.loX = screenRegion.lo.x;
		authoredRect.loY = screenRegion.lo.y;
		authoredRect.hiX = screenRegion.hi.x;
		authoredRect.hiY = screenRegion.hi.y;

		// GeneralsX @bugfix Claude 25/08/2026 Anchor on the ROW this control belongs to, not on the
		// control. The group came from the first pass in winCreateFromScript; a miss (a layout that
		// was not scanned, or one past the table's cap) passes NULL and keeps the previous behaviour.
		LayoutRect groupRect;
		const Bool haveGroup = LayoutScaleGroupFind( &s_gxLayoutGroups, lsCtx,
																								 authoredRect, &groupRect );

		LayoutScaleMapRectGrouped( lsCtx, authoredRect, haveGroup ? &groupRect : NULL, &mappedRect );

		screenRegion.lo.x = mappedRect.loX;
		screenRegion.lo.y = mappedRect.loY;
		screenRegion.hi.x = mappedRect.hiX;
		screenRegion.hi.y = mappedRect.hiY;
	}

	//
	// given the screen region upper left compute the upper left that we
	// will give this window, if we have a parent note that the position
	// is relative to the parent client area, if no parent is present
	// we're talking about the screen
	//
	if( parent )
	{
		ICoord2D parentScreenPos;

		// get parent position on screen
		parent->winGetScreenPosition( &parentScreenPos.x, &parentScreenPos.y );

		// save x and y with parent position as relative (0,0) location
		*x = screenRegion.lo.x - parentScreenPos.x;
		*y = screenRegion.lo.y - parentScreenPos.y;

	}
	else
	{

		*x = screenRegion.lo.x;
		*y = screenRegion.lo.y;

	}

	// save our width and height from the adjusted screen region locations
	*width = screenRegion.hi.x - screenRegion.lo.x;
	*height = screenRegion.hi.y - screenRegion.lo.y;

	return TRUE;

}

// parseImageOffset ===========================================================
/** Parse the image draw offset */
//=============================================================================
static Bool parseImageOffset( const char *token, WinInstanceData *instData,
															char *buffer, void *data )
{
  char *c;

	c = strtok( buffer, " \t\n\r" );
	instData->m_imageOffset.x = atoi( c );

	c = strtok( nullptr, " \t\n\r" );
	instData->m_imageOffset.y = atoi( c );

  return TRUE;

}

// parseFont ==================================================================
/** Parse the font field */
//=============================================================================
static Bool parseFont( const char *token, WinInstanceData *instData,
											 char *buffer, void *data )
{
	char *c, *ptr;
	const char *seps = " ,\n\r\t";
	const char *stringSeps = ":,\n\r\t\"";
	char fontName[ 256 ];
	Int fontSize;
	Int fontBold;

	// "NAME"
	c = strtok( buffer, seps );  // label
	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the "
	c = strtok( ptr, stringSeps );  // value
	strlcpy(fontName, c, ARRAY_SIZE(fontName));

	// "SIZE"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, fontSize );

	// "BOLD"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, fontBold );

	// GeneralsX @bugfix Claude 19/08/2026 Scale the authored size by the same factor this window's
	// SCREENRECT was scaled by, so text stays proportional to the box whatever resolution the layout
	// was authored at. See the note in parseScreenRect for why adjustFontSize() is not what is wanted.
	// GeneralsX @tweak Claude 19/08/2026 GX_UI_TEXT_SCALE - a dial for the operator to pick a size by
	// eye. The layouts pair 13pt list rows with 35pt buttons on one 1920 canvas; at 13pt a lowercase
	// curve is about one pixel across a slope, which is why 'E' renders clean and 'a'/'s'/'y' do not
	// at any alpha depth or hinting setting. The fix is size, and the right number is a judgement
	// call, so this exists to find it rather than to guess it. Default 1.0 = today's behaviour.
	static Real s_gxUiTextScale = -1.0f;
	if( s_gxUiTextScale < 0.0f )
	{
		const char *env = getenv("GX_UI_TEXT_SCALE");
		s_gxUiTextScale = (env != NULL) ? (Real)atof(env) : 1.0f;
		if( s_gxUiTextScale <= 0.0f )
			s_gxUiTextScale = 1.0f;
	}

	// GeneralsX @bugfix Claude 20/08/2026 gxLayoutTextFactor rather than s_gxLayoutFontScale, so the
	// GX_UI_SCALE dial moves this text with its box. Identical in STRETCH, where the factor is just
	// s_gxLayoutFontScale.
	const Int scaledFontSize = REAL_TO_INT_FLOOR( (Real)fontSize * gxLayoutTextFactor() * s_gxUiTextScale );
	fontSize = (scaledFontSize < 1) ? 1 : scaledFontSize;

	if( TheFontLibrary )
	{
		GameFont *font = TheFontLibrary->getFont( AsciiString(fontName), fontSize, fontBold );
		if( font )
			instData->m_font = font;
	}

	return TRUE;

}

// parseName =================================================================
/** Parse the NAME field */
//=============================================================================
static Bool parseName( const char *token, WinInstanceData *instData,
											 char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value
	instData->m_decoratedNameString = c;

	// given the name assign a window ID from the
	assert( TheNameKeyGenerator );
	if( TheNameKeyGenerator )
		instData->m_id = (Int)TheNameKeyGenerator->nameToKey( instData->m_decoratedNameString );

	return TRUE;

}

// parseStatus ================================================================
/** Parse the STATUS field */
//=============================================================================
static Bool parseStatus( const char *token, WinInstanceData *instData,
												 char *buffer, void *data )
{

  instData->m_status = 0;
  parseBitString( buffer, &instData->m_status, WindowStatusNames );

  return TRUE;

}

// parseStyle =================================================================
/** Parse the STYLE field */
//=============================================================================
static Bool parseStyle( const char *token, WinInstanceData *instData,
												char *buffer, void *data )
{

  instData->m_style = 0;
  parseBitString( buffer, &instData->m_style, WindowStyleNames );

  return TRUE;

}

// parseSystemCallback ========================================================
/** Parse the system method callback for a window */
//=============================================================================
static Bool parseSystemCallback( const char *token, WinInstanceData *instData,
																 char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value

	// save a pointer of the function address
	DEBUG_ASSERTCRASH( TheNameKeyGenerator && TheFunctionLexicon, ("Invalid singletons") );
	theSystemString = c;
	NameKeyType key = TheNameKeyGenerator->nameToKey( theSystemString );
	systemFunc = TheFunctionLexicon->gameWinSystemFunc( key );

	return TRUE;

}

// parseInputCallback =========================================================
/** Parse the Input method callback for a window */
//=============================================================================
static Bool parseInputCallback( const char *token, WinInstanceData *instData,
																char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value

	// save a pointer of the function address
	DEBUG_ASSERTCRASH( TheNameKeyGenerator && TheFunctionLexicon, ("Invalid singletons") );
	theInputString = c;
	NameKeyType key = TheNameKeyGenerator->nameToKey( theInputString );
	inputFunc = TheFunctionLexicon->gameWinInputFunc( key );

	return TRUE;

}

// parseTooltipCallback =======================================================
/** Parse the Tooltip method callback for a window */
//=============================================================================
static Bool parseTooltipCallback( const char *token, WinInstanceData *instData,
																  char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value

	// save a pointer of the function address
	DEBUG_ASSERTCRASH( TheNameKeyGenerator && TheFunctionLexicon, ("Invalid singletons") );
	theTooltipString = c;
	NameKeyType key = TheNameKeyGenerator->nameToKey( theTooltipString );
	tooltipFunc = TheFunctionLexicon->gameWinTooltipFunc( key );

	return TRUE;

}

// parseDrawCallback ==========================================================
/** Parse the Draw method callback for a window */
//=============================================================================
static Bool parseDrawCallback( const char *token, WinInstanceData *instData,
															 char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value

	// save a pointer of the function address
	DEBUG_ASSERTCRASH( TheNameKeyGenerator && TheFunctionLexicon, ("Invalid singletons") );
	theDrawString = c;
	NameKeyType key = TheNameKeyGenerator->nameToKey( theDrawString );
	drawFunc = TheFunctionLexicon->gameWinDrawFunc( key );

	return TRUE;

}

// parseHeaderTemplate ==========================================================
/** Parse the Draw method callback for a window */
//=============================================================================
static Bool parseHeaderTemplate( const char *token, WinInstanceData *instData,
															 char *buffer, void *data )
{
	char *c, *ptr;
//	const char *seps = " ,\n\r\t";
	const char *stringSeps = "\"";

	// scan to the first " mark
	ptr = buffer;
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the first "
	c = strtok( ptr, stringSeps );  // name value

	// save a pointer of the function address
	DEBUG_ASSERTCRASH( TheNameKeyGenerator && TheFunctionLexicon, ("Invalid singletons") );

	instData->m_headerTemplateName = c;

	return TRUE;

}

// parseListboxData ===========================================================
/** Parse listbox data entry */
//=============================================================================
static Bool parseListboxData( const char *token, WinInstanceData *instData,
															char *buffer, void *data )
{
	ListboxData *listData = (ListboxData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "LENGTH"
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );
	scanShort( c, listData->listLength );

	// "AUTOSCROLL"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, listData->autoScroll );

	// "SCROLLIFATEND" (optional)
	c = strtok( nullptr, seps );  // label
	if ( stricmp(c, "ScrollIfAtEnd") == 0 )
	{
		c = strtok( nullptr, seps );  // value
		scanBool( c, listData->scrollIfAtEnd );
		c = strtok( nullptr, seps );  // label
	}
	else
	{
		listData->scrollIfAtEnd = FALSE;
	}

	// "AUTOPURGE"
	c = strtok( nullptr, seps );  // value
	scanBool( c, listData->autoPurge );

	// "SCROLLBAR"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, listData->scrollBar );

	// "MULTISELECT"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, listData->multiSelect );

	// "COLUMNS"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanShort( c, listData->columns );
	if(listData->columns > 1)
	{
		listData->columnWidthPercentage = NEW Int[listData->columns];
		for(Int i = 0; i < listData->columns; i++ )
		{
			// "COLUMNS"
			c = strtok( nullptr, seps );  // label
			c = strtok( nullptr, seps );  // value
			scanInt( c, listData->columnWidthPercentage[i] );
		}
	}
	else
		listData->columnWidthPercentage = nullptr;
	listData->columnWidth = nullptr;
	// "FORCESELECT"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, listData->forceSelect );


	// "
	return TRUE;

}

// parseComboBoxData ===========================================================
/** Parse Combo Box data entry */
//=============================================================================
static Bool parseComboBoxData( const char *token, WinInstanceData *instData,
															char *buffer, void *data )
{
	ComboBoxData *comboData = (ComboBoxData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );	// value
  scanBool( c, comboData->isEditable );

	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );	// value
  scanInt( c, comboData->maxChars );

	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );	// value
  scanInt( c, comboData->maxDisplay );

	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );	// value
  scanBool( c, comboData->asciiOnly );

	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );	// value
  scanBool( c, comboData->lettersAndNumbersOnly );


  return TRUE;
}

// parseSliderData ============================================================
/** Parse slider data entry */
//=============================================================================
static Bool parseSliderData( const char *token, WinInstanceData *instData,
														 char *buffer, void *data )
{
	SliderData *sliderData = (SliderData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "MINVALUE"
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, sliderData->minVal );

	// "MAXVALUE"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, sliderData->maxVal );

	return TRUE;

}

// parseRadioButtonData =======================================================
/** Parse radio button data entry */
//=============================================================================
static Bool parseRadioButtonData( const char *token, WinInstanceData *instData,
																	char *buffer, void *data )
{
	RadioButtonData *radioData = (RadioButtonData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "GROUP"
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, radioData->group );

	return TRUE;

}


// parseTooltipText ===========================================================
/** Parse the TOOLTIPTEXT field */
//=============================================================================
static Bool parseTooltipText( const char *token, WinInstanceData *instData,
											 char *buffer, void *data )
{
	char *ptr = buffer;
	char *c;
	const char *stringSeps = "\n\r\t\"";

	// scan to the first " mark
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the "
	if(strlen( ptr ) == 1 )
		return TRUE;
	c = strtok( ptr, stringSeps );  // value
	if( strlen( c ) >= MAX_TEXT_LABEL )
	{

		DEBUG_LOG(( "TextTooltip label '%s' is too long, max is '%d'", c, MAX_TEXT_LABEL ));
		assert( 0 );
		return FALSE;

	}
	instData->m_tooltipString.set(c);
	instData->setTooltipText(TheGameText->fetch(c));


  return TRUE;

}

// parseTooltipDelay =======================================================
/** Parse the tooltip delay */
//=============================================================================
static Bool parseTooltipDelay( const char *token, WinInstanceData *instData,
																	char *buffer, void *data )
{
	//RadioButtonData *radioData = (RadioButtonData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "getvalue"
	c = strtok( buffer, seps );  // value
	scanInt( c, instData->m_tooltipDelay );

	return TRUE;

}


// parseText ==================================================================
/** Parse the TEXT field */
//=============================================================================
static Bool parseText( const char *token, WinInstanceData *instData,
											 char *buffer, void *data )
{
	char *ptr = buffer;
	char *c;
	const char *stringSeps = "\n\r\t\"";

	// scan to the first " mark
	while( *ptr != '"' )
		ptr++;
	ptr++;  // skip the "
	c = strtok( ptr, stringSeps );  // value
	if( strlen( c ) >= MAX_TEXT_LABEL )
	{

		DEBUG_LOG(( "Text label '%s' is too long, max is '%d'", c, MAX_TEXT_LABEL ));
		assert( 0 );
		return FALSE;

	}
	instData->m_textLabelString = c;


  return TRUE;

}

// parseTextColor =============================================================
/** Parse text color entries for enabled, disabled, and hilite with
	* drop shadow colors */
//=============================================================================
static Bool parseTextColor( const char *token, WinInstanceData *instData,
														char *buffer, void *data )
{
	char *c;
	const char *seps = " :,\n\r\t";
	UnsignedInt r, g, b, a;
	Int i, states = 3;
	TextDrawData *textData;
	Bool first = TRUE;


	for( i = 0; i < states; i++ )
	{

		if( i == 0 )
			textData = &instData->m_enabledText;
		else if( i == 1 )
			textData = &instData->m_disabledText;
		else if( i == 2 )
			textData = &instData->m_hiliteText;
		else
		{

			DEBUG_LOG(( "Undefined state for text color" ));
			assert( 0 );
			return FALSE;

		}

		// color
		if( first == TRUE )
			c = strtok( buffer, seps );  // label
		else
			c = strtok( nullptr, seps );  // label
		first = FALSE;
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, r );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, g );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, b );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, a );
		textData->color = GameMakeColor( r, g, b, a );

		// border color
		c = strtok( nullptr, seps );  // label
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, r );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, g );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, b );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, a );
		textData->borderColor = GameMakeColor( r, g, b, a );

	}

	return TRUE;

}

// parseStaticTextData ========================================================
/** Parse static text data entry */
//=============================================================================
static Bool parseStaticTextData( const char *token, WinInstanceData *instData,
																 char *buffer, void *data )
{
	TextData *textData = (TextData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "CENTERED"
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, textData->centered );

	// @todo: add these  to GUIEdit options and output
	// These are initialized here because any TextData constructor would never get called.
	// The behavior with these defaults is the same as it was before these members were added.
	textData->centeredVertically = TRUE;
	textData->leftMargin = 7;
	textData->topMargin = 7;

	return TRUE;

}

// parseTextEntryData =========================================================
/** Parse text entry data entry */
//=============================================================================
static Bool parseTextEntryData( const char *token, WinInstanceData *instData,
																char *buffer, void *data )
{
	EntryData *entryData = (EntryData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	// "MAXLEN"
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanShort( c, entryData->maxTextLen );

	// "SECRETTEXT"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, entryData->secretText );

	// "NUMERICALONLY"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, entryData->numericalOnly );

	// "ALPHANUMERICALONLY"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, entryData->alphaNumericalOnly );

	// "ASCIIONLY"
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanBool( c, entryData->aSCIIOnly );

	return TRUE;

}

// parseTabControlData =========================================================
/** Parse tab control data entry */
//=============================================================================
static Bool parseTabControlData( const char *token, WinInstanceData *instData,
																char *buffer, void *data )
{
	TabControlData *tabControlData = (TabControlData *)data;
	char *c;
	const char *seps = " :,\n\r\t";

	//TABORIENTATION
	c = strtok( buffer, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->tabOrientation );

	//TABEDGE
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->tabEdge );

	//TABWIDTH
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->tabWidth );

	//TABHEIGHT
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->tabHeight );

	//TABCOUNT
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->tabCount );

	//PANEBORDER
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, tabControlData->paneBorder );

	//PANEDISABLED
	Int entryCount = 0;
	c = strtok( nullptr, seps );  // label
	c = strtok( nullptr, seps );  // value
	scanInt( c, entryCount );

	for( Int paneIndex = 0; paneIndex < entryCount; paneIndex ++ )
	{
		c = strtok( nullptr, seps );  // value
		scanBool( c, tabControlData->subPaneDisabled[paneIndex] );
	}

	return TRUE;
}

// parseDrawData ==============================================================
/** Parse set of draw data elements */
//=============================================================================
static Bool parseDrawData( const char *token, WinInstanceData *instData,
													 char *buffer, void *data )
{
	Int i;
	UnsignedInt r, g, b, a;
	WinDrawData *drawData;
	Bool first = TRUE;
	char *c;
	const char *seps = " :,\n\r\t";

	for( i = 0; i < MAX_DRAW_DATA; i++ )
	{

		// get the right draw data
		if( strcmp( token, "ENABLEDDRAWDATA" ) == 0 )
			drawData = &instData->m_enabledDrawData[ i ];
		else if( strcmp( token, "DISABLEDDRAWDATA" ) == 0 )
			drawData = &instData->m_disabledDrawData[ i ];
		else if( strcmp( token, "HILITEDRAWDATA" ) == 0 )
			drawData = &instData->m_hiliteDrawData[ i ];
		else if( strcmp( token, "LISTBOXENABLEDUPBUTTONDRAWDATA" ) == 0 )
			drawData = &enabledUpButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXDISABLEDUPBUTTONDRAWDATA" ) == 0 )
			drawData = &disabledUpButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXHILITEUPBUTTONDRAWDATA" ) == 0 )
			drawData = &hiliteUpButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXENABLEDDOWNBUTTONDRAWDATA" ) == 0 )
			drawData = &enabledDownButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXDISABLEDDOWNBUTTONDRAWDATA" ) == 0 )
			drawData = &disabledDownButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXHILITEDOWNBUTTONDRAWDATA" ) == 0 )
			drawData = &hiliteDownButtonDrawData[ i ];
		else if( strcmp( token, "LISTBOXENABLEDSLIDERDRAWDATA" ) == 0 )
			drawData = &enabledSliderDrawData[ i ];
		else if( strcmp( token, "LISTBOXDISABLEDSLIDERDRAWDATA" ) == 0 )
			drawData = &disabledSliderDrawData[ i ];
		else if( strcmp( token, "LISTBOXHILITESLIDERDRAWDATA" ) == 0 )
			drawData = &hiliteSliderDrawData[ i ];
		else if( strcmp( token, "SLIDERTHUMBENABLEDDRAWDATA" ) == 0 )
			drawData = &enabledSliderThumbDrawData[ i ];
		else if( strcmp( token, "SLIDERTHUMBDISABLEDDRAWDATA" ) == 0 )
			drawData = &disabledSliderThumbDrawData[ i ];
		else if( strcmp( token, "SLIDERTHUMBHILITEDRAWDATA" ) == 0 )
			drawData = &hiliteSliderThumbDrawData[ i ];
		else if( strcmp( token, "COMBOBOXDROPDOWNBUTTONENABLEDDRAWDATA" ) == 0 )
			drawData = &enabledDropDownButtonDrawData[ i ];
		else if( strcmp( token, "COMBOBOXDROPDOWNBUTTONDISABLEDDRAWDATA" ) == 0 )
			drawData = &disabledDropDownButtonDrawData[ i ];
		else if( strcmp( token, "COMBOBOXDROPDOWNBUTTONHILITEDRAWDATA" ) == 0 )
			drawData = &hiliteDropDownButtonDrawData[ i ];
		else if( strcmp( token, "COMBOBOXEDITBOXENABLEDDRAWDATA" ) == 0 )
			drawData = &enabledEditBoxDrawData[ i ];
		else if( strcmp( token, "COMBOBOXEDITBOXDISABLEDDRAWDATA" ) == 0 )
			drawData = &disabledEditBoxDrawData[ i ];
		else if( strcmp( token, "COMBOBOXEDITBOXHILITEDRAWDATA" ) == 0 )
			drawData = &hiliteEditBoxDrawData[ i ];
		else if( strcmp( token, "COMBOBOXLISTBOXENABLEDDRAWDATA" ) == 0 )
			drawData = &enabledListBoxDrawData[ i ];
		else if( strcmp( token, "COMBOBOXLISTBOXDISABLEDDRAWDATA" ) == 0 )
			drawData = &disabledListBoxDrawData[ i ];
		else if( strcmp( token, "COMBOBOXLISTBOXHILITEDRAWDATA" ) == 0 )
			drawData = &hiliteListBoxDrawData[ i ];
		else
		{

			DEBUG_LOG(( "ParseDrawData, undefined token '%s'", token ));
			assert( 0 );
			return FALSE;

		}

		// IMAGE: X
		if( first == TRUE )
			c = strtok( buffer, seps );  // label
		else
			c = strtok( nullptr, seps );  // label
		first = FALSE;

		c = strtok( nullptr, seps );  // value
		if( strcmp( c, "NoImage" ) != 0 )
			drawData->image = TheMappedImageCollection->findImageByName( AsciiString( c ) );
		else
			drawData->image = nullptr;
		// COLOR: R G B A
		c = strtok( nullptr, seps );  // label
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, r );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, g );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, b );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, a );
		drawData->color = GameMakeColor( r, g, b, a );

		// BORDERCOLOR: R G B A
		c = strtok( nullptr, seps );  // label
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, r );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, g );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, b );
		c = strtok( nullptr, seps );  // value
		scanUnsignedInt( c, a );
		drawData->borderColor = GameMakeColor( r, g, b, a );

	}

	return TRUE;

}

// getDataTemplate ============================================================
/** Given a window type style string return the address of a static
	* gadget data type used for the generic data pointers in the
	* GUI gadget controls */
//=============================================================================
void *getDataTemplate( char *type )
{
  static EntryData eData;
  static SliderData sData;
  static ListboxData lData;
  static TextData tData;
	static RadioButtonData rData;
	static TabControlData tcData;
	static ComboBoxData	cData;
	void *data;

  if( strcmp( type, "VERTSLIDER" ) == 0 || strcmp( type, "HORZSLIDER" ) == 0 )
	{

    memset( &sData, 0, sizeof( SliderData ) );
    data = &sData;

  }
	else if( strcmp( type, "SCROLLLISTBOX" ) == 0 )
	{

    memset( &lData, 0, sizeof( ListboxData ) );
    data = &lData;

  }
	else if( strcmp( type, "TABCONTROL" ) == 0 )
	{
    memset( &tcData, 0, sizeof( TabControlData ) );
    data = &tcData;
  }
	else if( strcmp( type, "ENTRYFIELD" ) == 0 )
	{

    memset( &eData, 0, sizeof( EntryData ) );
    data = &eData;

  }
	else if( strcmp( type, "STATICTEXT" ) == 0 )
	{

		memset( &tData, 0, sizeof( TextData ) );
		data = &tData;

  }
	else if( strcmp( type, "RADIOBUTTON" ) == 0 )
	{

		memset( &rData, 0, sizeof( RadioButtonData ) );
		data = &rData;
	}
	else if( strcmp( type, "COMBOBOX" ) == 0 )
	{

		memset( &cData, 0, sizeof( ComboBoxData ) );
		data = &cData;
	}
	else
    data = nullptr;

  return data;

}

// parseData ==================================================================
//
// Parse the data for gadgets.  For sliders the data is in the format:
// <Minimum Value> <Maximum Value>
//
// For listboxes the data is formatted as:
// <Max number of entries> <Height of an entry> <AutoScroll> <AutoPurge> <ScrollBar> <MultiSelect> <ForceSelect>
//
// For Combo Boxes the data is formatted as:
// <is Editable> <Max Characters> <Max Entries before scroll appaears>
//
// For entry fields the data is as follows:
// <Max text length> <entry box width> <SecretText> <1 = NumericalOnly, 2 = AlphaNumericOnly>
//
// For text the data is as follows:
// <Centered> <Outlined> <StringManagerLabel>
//
// 1/2/2003: THIS FUNCTION IS NEVER REACHED; IT IS OBSOLETE -MDC
//
//=============================================================================
static Bool parseData( void **data, char *type, char *buffer )
{
  char *c;
  static EntryData eData;
  static SliderData sData;
  static ListboxData lData;
  static TextData tData;
	static RadioButtonData rData;
	static ComboBoxData cData;

  if( strcmp( type, "VERTSLIDER" ) == 0 || strcmp( type, "HORZSLIDER" ) == 0 )
	{

    memset( &sData, 0, sizeof( SliderData ) );

	  c = strtok( buffer, " \t\n\r" );
    sData.minVal = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
    sData.maxVal = atoi(c);

    *data = &sData;

  }
	else if( strcmp( type, "SCROLLLISTBOX" ) == 0 )
	{

    memset( &lData, 0, sizeof( ListboxData ) );

	  c = strtok( buffer, " \t\n\r" );
    lData.listLength = atoi(c);

//	  c = strtok( nullptr, " \t\n\r" );
//    lData.entryHeight = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
    lData.autoScroll = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
    lData.autoPurge = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
    lData.scrollBar = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
    lData.multiSelect = atoi(c);

		c = strtok( nullptr, " \t\n\r" );
		lData.forceSelect = atoi(c);

    *data = &lData;

  }
	else if( strcmp( type, "ENTRYFIELD" ) == 0 )
	{

    memset( &eData, 0, sizeof( EntryData ) );

	  c = strtok( buffer, " \t\n\r" );
    eData.maxTextLen = atoi(c);

	  c = strtok( nullptr, " \t\n\r" );
//    if (c)
//      eData.entryWidth = atoi(c);
//    else
//      eData.entryWidth = -1;

		c = strtok( nullptr, " \t\n\r" );
		if (c)
		{
			eData.secretText = atoi(c);

			if( eData.secretText != FALSE )
				eData.secretText = TRUE;
		}
		else
			eData.secretText = FALSE;

		c = strtok( nullptr, " \t\n\r" );
		if (c)
		{
			eData.numericalOnly = ( atoi(c) == 1 );
			eData.alphaNumericalOnly = ( atoi(c) == 2 );
			eData.aSCIIOnly = ( atoi(c) == 3 );
		}
		else
		{
			eData.numericalOnly = FALSE;
			eData.alphaNumericalOnly = FALSE;
			eData.aSCIIOnly = FALSE;
		}
    *data = &eData;

  }
	else if( strcmp( type, "STATICTEXT" ) == 0 )
	{

	  c = strtok( buffer, " \t\n\r" );
    tData.centered = atoi(c);

		if( tData.centered != FALSE )
			tData.centered = TRUE;

	  c = strtok( nullptr, " \t\n\r" );

		/** @todo need to get a label from the translation manager, uncomment
		the following line and remove the WideChar assignment when
		we have it */
//		text = StringManagerFetch( c );
//		text = L"Need StrManager, Remove me!";
//		TheWindowManager->winStrcpy( tData.text, text );

		*data = &tData;

  }
	else if( strcmp( type, "RADIOBUTTON" ) == 0 )
	{

		c = strtok( buffer, " \t\n\r" );
		rData.group = atoi(c);
/// @todo Colin: Why was this here???
//		if( tData.centered != FALSE )
//		{
//			tData.centered = TRUE;
//		}
		*data = &rData;
	}
	else
    *data = nullptr;

  return TRUE;

}

// setWindowText ==============================================================
/** Set the default text for a window or gadget control */
//=============================================================================
static void setWindowText( GameWindow *window, AsciiString textLabel )
{

	// sanity
	if (textLabel.isEmpty())
		return;

	UnicodeString theText, entryText;
	//Translate the text
	theText = TheGameText->fetch( (char *)textLabel.str());
	// set the text in the window based on what it is
	if( BitIsSet( window->winGetStyle(), GWS_PUSH_BUTTON ) )
		GadgetButtonSetText( window, theText );
	else if( BitIsSet( window->winGetStyle(), GWS_RADIO_BUTTON ) )
		GadgetRadioSetText( window, theText );
	else if( BitIsSet( window->winGetStyle(), GWS_CHECK_BOX ) )
		GadgetCheckBoxSetText( window, theText );
	else if( BitIsSet( window->winGetStyle(), GWS_STATIC_TEXT ) )
		GadgetStaticTextSetText( window, theText );
	else if( BitIsSet( window->winGetStyle(), GWS_ENTRY_FIELD ) )
	{
		entryText.translate(textLabel);
		GadgetTextEntrySetText( window, entryText );
	}
	else
		window->winSetText( theText );

}

// createGadget ===============================================================
/** Create a gadget based on the 'type' parm */
//=============================================================================
static GameWindow *createGadget( char *type,
																 GameWindow *parent,
																 Int status,
																 Int x, Int y,
																 Int width, Int height,
																 WinInstanceData *instData,
																 void *data )
{
  GameWindow *window = nullptr;

  instData->m_owner = parent;

  if( strcmp( type, "PUSHBUTTON" ) == 0 )
	{

    instData->m_style |= GWS_PUSH_BUTTON;
    window = TheWindowManager->gogoGadgetPushButton( parent, status, x, y,
																										 width, height,
																										 instData,
																										 instData->m_font, FALSE );

  }
	else if( strcmp( type, "RADIOBUTTON" ) == 0 )
	{
		RadioButtonData *rData = (RadioButtonData *)data;
		char filename[ MAX_WINDOW_NAME_LEN ];
		char *c;

		//
		// assign a screen identifier to the radio button based on the
		// filename the radio button was saved in
		//

		strlcpy(filename, instData->m_decoratedNameString.str(), ARRAY_SIZE(filename));
		c = strchr( filename, ':' );
		if( c )
			*c = 0;  // terminate after filename (format is filename:gadgetname)
		assert( TheNameKeyGenerator );
		if( TheNameKeyGenerator )
			rData->screen = (Int)(TheNameKeyGenerator->nameToKey( filename ));

    instData->m_style |= GWS_RADIO_BUTTON;
    window = TheWindowManager->gogoGadgetRadioButton( parent, status, x, y,
																											width, height,
																											instData, rData,
																											instData->m_font, FALSE );

  }
	else if( strcmp( type, "CHECKBOX" ) == 0 )
	{

    instData->m_style |= GWS_CHECK_BOX;
    window = TheWindowManager->gogoGadgetCheckbox( parent, status, x, y,
																									 width, height,
																									 instData,
																									 instData->m_font, FALSE );

  }
	else if( strcmp( type, "TABCONTROL" ) == 0 )
	{
		TabControlData *tcData = (TabControlData *)data;
    instData->m_style |= GWS_TAB_CONTROL;
    window = TheWindowManager->gogoGadgetTabControl( parent, status, x, y,
																										width, height,
																										instData, tcData,
																										instData->m_font, FALSE );
	}
	else if( strcmp( type, "VERTSLIDER" ) == 0 )
	{
    SliderData *sData = (SliderData *)data;

    instData->m_style |= GWS_VERT_SLIDER;
    window = TheWindowManager->gogoGadgetSlider( parent, status, x, y,
																								 width, height,
																								 instData, sData,
																								 instData->m_font, FALSE );

		//
		// we know we've read in the slider thumb data in the definition file
		// (it's guaranteed to be generated by the editor) so place that
		// draw data into the thumb of the slider
		//
		GameWindow *thumb = window->winGetChild();
		if( thumb )
		{
			WinInstanceData *instData = thumb->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledSliderThumbDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledSliderThumbDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteSliderThumbDrawData[ i ];

			}

		}

  }
	else if( strcmp( type, "HORZSLIDER" ) == 0 )
	{
    SliderData *sData = (SliderData *)data;

    instData->m_style |= GWS_HORZ_SLIDER;
    window = TheWindowManager->gogoGadgetSlider( parent, status, x, y,
																								 width, height,
																								 instData, sData,
																								 instData->m_font, FALSE );

		//
		// we know we've read in the slider thumb data in the definition file
		// (it's guaranteed to be generated by the editor) so place that
		// draw data into the thumb of the slider
		//
		GameWindow *thumb = window->winGetChild();
		if( thumb )
		{
			WinInstanceData *instData = thumb->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledSliderThumbDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledSliderThumbDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteSliderThumbDrawData[ i ];

			}

		}

  }
	else if( strcmp( type, "SCROLLLISTBOX" ) == 0 )
	{

    ListboxData *lData = (ListboxData *)data;

    instData->m_style |= GWS_SCROLL_LISTBOX;
    window = TheWindowManager->gogoGadgetListBox( parent, status, x, y,
																									width, height,
																									instData, lData,
																									instData->m_font, FALSE );

		//
		// we know that in the file we have read the draw data for the listbox
		// parts (up button, down button, and slider), now that we have those
		// parts actually created we must assign that data to them
		//
		GameWindow *upButton = GadgetListBoxGetUpButton( window );
		if( upButton )
		{
			WinInstanceData *instData = upButton->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledUpButtonDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledUpButtonDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteUpButtonDrawData[ i ];

			}

		}

		GameWindow *downButton = GadgetListBoxGetDownButton( window );
		if( downButton )
		{
			WinInstanceData *instData = downButton->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledDownButtonDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledDownButtonDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteDownButtonDrawData[ i ];

			}

		}

		GameWindow *slider = GadgetListBoxGetSlider( window );
		if( slider )
		{
			WinInstanceData *instData = slider->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledSliderDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledSliderDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteSliderDrawData[ i ];

			}

			// do the slider thumb
			GameWindow *thumb = slider->winGetChild();
			if( thumb )
			{
				WinInstanceData *instData = thumb->winGetInstanceData();

				for( Int i = 0; i < MAX_DRAW_DATA; i++ )
				{

					instData->m_enabledDrawData[ i ] = enabledSliderThumbDrawData[ i ];
					instData->m_disabledDrawData[ i ] = disabledSliderThumbDrawData[ i ];
					instData->m_hiliteDrawData[ i ] = hiliteSliderThumbDrawData[ i ];

				}

			}

		}

  }
	else if( strcmp( type, "COMBOBOX" ) == 0 )
	{
		ComboBoxData *cData = (ComboBoxData *)data;
		cData->entryData = NEW EntryData;
		memset ( cData->entryData, 0, sizeof(EntryData));
		cData->listboxData = NEW ListboxData;
		memset ( cData->listboxData, 0, sizeof(ListboxData));

		//initialize combo box data
		//cData->isEditable = TRUE;
		//cData->maxChars = 16;
		//cData->maxDisplay = 5;
		cData->entryCount = 0;
		//initialize entry data
		cData->entryData->aSCIIOnly = cData->asciiOnly;
		cData->entryData->alphaNumericalOnly = cData->lettersAndNumbersOnly;
		cData->entryData->maxTextLen = cData->maxChars;

		//initialize listbox data
		cData->listboxData->listLength = 10;
		cData->listboxData->autoScroll = 0;
		cData->listboxData->scrollIfAtEnd = FALSE;
		cData->listboxData->autoPurge = 0;
		cData->listboxData->scrollBar = 1;
		cData->listboxData->multiSelect = 0;
		cData->listboxData->forceSelect = 1;
		cData->listboxData->columns = 1;
		cData->listboxData->columnWidth = nullptr;
		cData->listboxData->columnWidthPercentage = nullptr;

    instData->m_style |= GWS_COMBO_BOX;
    window = TheWindowManager->gogoGadgetComboBox( parent, status, x, y,
																									width, height,
																									instData, cData,
																									instData->m_font, FALSE );

		GameWindow *dropDownButton = GadgetComboBoxGetDropDownButton( window );
		if( dropDownButton )
		{
			WinInstanceData *instData = dropDownButton->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledDropDownButtonDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledDropDownButtonDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteDropDownButtonDrawData[ i ];

			}
		}

		GameWindow *editBox = GadgetComboBoxGetEditBox( window );
		if( editBox )
		{
			WinInstanceData *instData = editBox->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledEditBoxDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledEditBoxDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteEditBoxDrawData[ i ];

			}
		}


		GameWindow *listBox = GadgetComboBoxGetListBox( window );
		if( listBox )
		{
			WinInstanceData *instData = listBox->winGetInstanceData();

			for( Int i = 0; i < MAX_DRAW_DATA; i++ )
			{

				instData->m_enabledDrawData[ i ] = enabledListBoxDrawData[ i ];
				instData->m_disabledDrawData[ i ] = disabledListBoxDrawData[ i ];
				instData->m_hiliteDrawData[ i ] = hiliteListBoxDrawData[ i ];

			}


			GameWindow *upButton = GadgetListBoxGetUpButton( listBox );
			if( upButton )
			{
				WinInstanceData *instData = upButton->winGetInstanceData();

				for( Int i = 0; i < MAX_DRAW_DATA; i++ )
				{

					instData->m_enabledDrawData[ i ] = enabledUpButtonDrawData[ i ];
					instData->m_disabledDrawData[ i ] = disabledUpButtonDrawData[ i ];
					instData->m_hiliteDrawData[ i ] = hiliteUpButtonDrawData[ i ];

				}

			}

			GameWindow *downButton = GadgetListBoxGetDownButton( listBox );
			if( downButton )
			{
				WinInstanceData *instData = downButton->winGetInstanceData();

				for( Int i = 0; i < MAX_DRAW_DATA; i++ )
				{

					instData->m_enabledDrawData[ i ] = enabledDownButtonDrawData[ i ];
					instData->m_disabledDrawData[ i ] = disabledDownButtonDrawData[ i ];
					instData->m_hiliteDrawData[ i ] = hiliteDownButtonDrawData[ i ];

				}

			}

			GameWindow *slider = GadgetListBoxGetSlider( listBox );
			if( slider )
			{
				WinInstanceData *instData = slider->winGetInstanceData();

				for( Int i = 0; i < MAX_DRAW_DATA; i++ )
				{

					instData->m_enabledDrawData[ i ] = enabledSliderDrawData[ i ];
					instData->m_disabledDrawData[ i ] = disabledSliderDrawData[ i ];
					instData->m_hiliteDrawData[ i ] = hiliteSliderDrawData[ i ];

				}

				// do the slider thumb
				GameWindow *thumb = slider->winGetChild();
				if( thumb )
				{
					WinInstanceData *instData = thumb->winGetInstanceData();

					for( Int i = 0; i < MAX_DRAW_DATA; i++ )
					{

						instData->m_enabledDrawData[ i ] = enabledSliderThumbDrawData[ i ];
						instData->m_disabledDrawData[ i ] = disabledSliderThumbDrawData[ i ];
						instData->m_hiliteDrawData[ i ] = hiliteSliderThumbDrawData[ i ];

					}

				}

			}
		}
  }
	else if( strcmp( type, "ENTRYFIELD" ) == 0 )
	{
    EntryData *eData = (EntryData *)data;

    instData->m_style |= GWS_ENTRY_FIELD;
    window = TheWindowManager->gogoGadgetTextEntry( parent, status, x, y,
																										width, height,
																										instData, eData,
																										instData->m_font, FALSE );

  }
	else if( strcmp( type, "STATICTEXT" ) == 0 )
	{
    TextData *tData = (TextData *)data;

    instData->m_style |= GWS_STATIC_TEXT;
    window = TheWindowManager->gogoGadgetStaticText( parent, status, x, y,
																										 width, height,
																										 instData, tData,
																										 instData->m_font, FALSE );


  }
	else if( strcmp( type, "PROGRESSBAR" ) == 0 )
	{

    instData->m_style |= GWS_PROGRESS_BAR;
    window = TheWindowManager->gogoGadgetProgressBar( parent, status, x, y,
																											width, height,
																											instData,
																											instData->m_font, FALSE );

  }

  return window;

}

// createWindow ===============================================================
// Create a user window or a gadget depending on the 'type' parm
//=============================================================================
static GameWindow *createWindow( char *type,
																 Int id,
																 Int status,
																 Int x, Int y,
																 Int width, Int height,
																 WinInstanceData *instData,
																 void *data,
																 GameWinSystemFunc system,
																 GameWinInputFunc input,
																 GameWinTooltipFunc tooltip,
																 GameWinDrawFunc draw )
{
  GameWindow *window, *parent;

	// Check to see if this window has a parent
  parent = peekWindow();

  // If this is a regular window just create it
  if( strcmp( type, "USER" ) == 0 )
	{

    window = TheWindowManager->winCreate( parent,
																					status, x, y,
																					width, height,
																					system );
		if( window )
		{

			instData->m_style |= GWS_USER_WINDOW;
			window->winSetInstanceData( instData );
			window->winSetWindowId( id );

		}

  }
	else if( strcmp( type, "TABPANE" ) == 0 )
	{

    window = TheWindowManager->winCreate( parent,
																					status, x, y,
																					width, height,
																					system );
		if( window )
		{

			instData->m_style |= GWS_TAB_PANE;
			window->winSetInstanceData( instData );
			window->winSetWindowId( id );

		}

  }

	else
	{

    // Else parse the type and create the gadget
    window = createGadget( type,
													 parent,
													 status,
													 x, y,
													 width, height,
                           instData,
													 data );
		if( window )
		{

			// set id
			window->winSetWindowId( id );

		}

  }

	// assign the callbacks if they are not empty/nullptr, that means they were read
	// in and parsed from the window definition file
	if( window )
	{

		if( system )
			window->winSetSystemFunc( system );
		if( input )
			window->winSetInputFunc( input );
		if( tooltip )
			window->winSetTooltipFunc( tooltip );
		if( draw )
			window->winSetDrawFunc( draw );

		// save strings for edit data if present
		GameWindowEditData *editData = window->winGetEditData();
		if( editData )
		{

			editData->systemCallbackString = theSystemString;
			editData->inputCallbackString = theInputString;
			editData->tooltipCallbackString = theTooltipString;
			editData->drawCallbackString = theDrawString;

		}

	}

	if( window )
	{

		// set any text read from the textLabel
		setWindowText( window, instData->m_textLabelString );

	}

  // If there is a parent window, send it the SCRIPT_CREATE message
  if( window && parent )
		TheWindowManager->winSendInputMsg( parent, GWM_SCRIPT_CREATE, id, 0 );

  return window;

}

// parseChildWindows ==========================================================
/** Parse window descriptions until an extra end is encountered indicating
	* the end of this block of child window descriptions. */
//=============================================================================
static Bool parseChildWindows( GameWindow *window,
															 File *inFile,
															 char *buffer )
{
  GameWindow *lastWindow;
	AsciiString asciibuf;

	//The gadget with children needs to delete its default created children in favor
	//of the ones from the script file.  So kill them before reading.
	if( BitIsSet( window->winGetStyle(), GWS_TAB_CONTROL ) )
	{
		GameWindow *nextWindow = nullptr;
		for( GameWindow *myChild = window->winGetChild(); myChild; myChild = nextWindow )
		{
			nextWindow = myChild->winGetNext();
			TheWindowManager->winDestroy( myChild );
		}
	}

		// Push the current window onto the stack so we know it's the parent
  pushWindow( window );

	while( TRUE )
	{

		if (inFile->scanString(asciibuf) == FALSE) {
			break;
		}

		if (asciibuf.compare("ENDALLCHILDREN") == 0) {
			break;
		}

		if (asciibuf.compare("END") == 0) {
      break;
		}

		if (asciibuf.compare("ENABLEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defEnabledColor, inFile, buffer ) == FALSE )
			{
				return FALSE;
			}

		}
		else if (asciibuf.compare("DISABLEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defDisabledColor, inFile, buffer ) == FALSE )
			{
				return FALSE;
			}

		}
		else if (asciibuf.compare("HILITECOLOR") == 0)
		{

			if( parseDefaultColor( &defHiliteColor, inFile, buffer ) == FALSE )
			{
				return FALSE;
			}

		}
		else if (asciibuf.compare("SELECTEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defSelectedColor, inFile, buffer ) == FALSE )
			{
				return FALSE;
			}

		}
		else if (asciibuf.compare("TEXTCOLOR") == 0)
		{

			if( parseDefaultColor( &defTextColor, inFile, buffer ) == FALSE )
			{
				return FALSE;
			}

		}
		else if (asciibuf.compare("WINDOW") == 0)
		{

      // Parse window descriptions until the last END is read
			if( parseWindow( inFile, buffer ) == nullptr )
			{
				return FALSE;
			}

		}

	}

  // Pop the current window off the stack
  lastWindow = popWindow();

  if( lastWindow != window )
	{

    DEBUG_LOG(( "parseChildWindows: unmatched window on stack.  Corrupt stack or bad source" ));
    return FALSE;

  }

	if( BitIsSet( window->winGetStyle(), GWS_TAB_CONTROL ) )
		GadgetTabControlFixupSubPaneList( window );//all children created, so re-fill SubPane array with children

  return TRUE;

}

// lookup table for parsing functions
static GameWindowParse gameWindowFieldList[] =
{
	{ "NAME", parseName },
	{ "STATUS", parseStatus },
	{ "STYLE", parseStyle },
	{ "SYSTEMCALLBACK", parseSystemCallback },
	{ "INPUTCALLBACK", parseInputCallback },
	{ "TOOLTIPCALLBACK", parseTooltipCallback },
	{ "DRAWCALLBACK", parseDrawCallback },
	{ "FONT", parseFont },
	{ "HEADERTEMPLATE", parseHeaderTemplate },
	{ "LISTBOXDATA", parseListboxData },
	{ "COMBOBOXDATA", parseComboBoxData },
	{ "SLIDERDATA", parseSliderData },
	{ "RADIOBUTTONDATA", parseRadioButtonData },
	{	"TOOLTIPTEXT", parseTooltipText },
  { "TOOLTIPDELAY", parseTooltipDelay },
	{ "TEXT", parseText },
	{ "TEXTCOLOR", parseTextColor },
	{ "STATICTEXTDATA", parseStaticTextData },
	{ "TEXTENTRYDATA", parseTextEntryData },
	{ "TABCONTROLDATA", parseTabControlData },

	{ "ENABLEDDRAWDATA", parseDrawData },
	{ "DISABLEDDRAWDATA", parseDrawData },
	{ "HILITEDRAWDATA", parseDrawData },
	{ "LISTBOXENABLEDUPBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXENABLEDDOWNBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXENABLEDSLIDERDRAWDATA", parseDrawData },
	{ "LISTBOXDISABLEDUPBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXDISABLEDDOWNBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXDISABLEDSLIDERDRAWDATA", parseDrawData },
	{ "LISTBOXHILITEUPBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXHILITEDOWNBUTTONDRAWDATA", parseDrawData },
	{ "LISTBOXHILITESLIDERDRAWDATA", parseDrawData },
	{ "SLIDERTHUMBENABLEDDRAWDATA", parseDrawData },
	{ "SLIDERTHUMBDISABLEDDRAWDATA", parseDrawData },
	{ "SLIDERTHUMBHILITEDRAWDATA", parseDrawData },

	{ "COMBOBOXDROPDOWNBUTTONENABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXDROPDOWNBUTTONDISABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXDROPDOWNBUTTONHILITEDRAWDATA", parseDrawData },
	{ "COMBOBOXEDITBOXENABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXEDITBOXDISABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXEDITBOXHILITEDRAWDATA", parseDrawData },
	{ "COMBOBOXLISTBOXENABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXLISTBOXDISABLEDDRAWDATA", parseDrawData },
	{ "COMBOBOXLISTBOXHILITEDRAWDATA", parseDrawData },

	{ "IMAGEOFFSET", parseImageOffset },
	{ "TOOLTIP", parseTooltip },

	{ nullptr, nullptr }
};

// parseWindow ================================================================
/** Parse a WINDOW entry in the script. */
//=============================================================================
static GameWindow *parseWindow( File *inFile, char *buffer )
{
	GameWindowParse *parse;
	GameWindow *window = nullptr;
	GameWindow *parent = peekWindow();
	WinInstanceData instData;
	char type[64];
	char token[ 256 ];
	char *c;
	Int x, y, width, height;
	void *data = nullptr;
	ICoord2D parentSize;
	AsciiString asciibuf;

	//
	// reset our 'static globals' that house the current parsed window callback
	// definitions to empty
	//
	systemFunc = nullptr;
	inputFunc = nullptr;
	tooltipFunc = nullptr;
	drawFunc = nullptr;
	theSystemString.clear();
	theInputString.clear();
	theTooltipString.clear();
	theDrawString.clear();

	// get the size of the parent, or if no parent present the screen
	if( parent )
	{
		parent->winGetSize( &parentSize.x, &parentSize.y );
	}
	else
	{
		parentSize.x = TheDisplay->getWidth();
		parentSize.y = TheDisplay->getHeight();
	}

	// Initialize the instance data to the defaults
	/// @todo need to support enabled/disabled/hilite text colors here
	instData.init();
	instData.m_enabledText.color = defTextColor;
	instData.m_enabledText.borderColor = defTextColor;
	instData.m_disabledText.color = defTextColor;
	instData.m_disabledText.borderColor = defTextColor;
	instData.m_hiliteText.color = defTextColor;
	instData.m_hiliteText.borderColor = defTextColor;

	/// @todo need real font support here
	instData.m_font = defFont;

	//
	// read the first few lines that are required to be first in a
	// window definition file including position, size, type, and id
	//

	// window type
	readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );
	c = strtok( buffer, seps );
	assert( strcmp( c, "WINDOWTYPE" ) == 0 );
	c = strtok( nullptr, seps );  // get data to right of = sign
	strlcpy(type, c, ARRAY_SIZE(type));

	//
	// based on the window type get a pointer for any specific data
	// for the gadget controls needed
	//
	data = getDataTemplate( type );

	// position
	readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );
	c = strtok( buffer, seps );
	assert( strcmp( c, "SCREENRECT" ) == 0 );
	if( parseScreenRect( c, buffer, &x, &y, &width, &height ) == FALSE )
		goto cleanupAndExit;

	// parse all the field definitions
	while( TRUE )
	{

		// get token
		inFile->scanString(asciibuf);

		// parse field
		for( parse = gameWindowFieldList; parse->parse; parse++ )
		{

			if (asciibuf.compare(parse->name) == 0)
			{

				strlcpy(token, asciibuf.str(), ARRAY_SIZE(token));

				// eat '='
				inFile->scanString(asciibuf);

				readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

				if (parse->parse( token, &instData, buffer, data ) == FALSE )
				{
					DEBUG_LOG(( "parseGameObject: Error parsing %s", parse->name ));
					goto cleanupAndExit;
				}

				break;
			}

		}

		if( parse->parse == nullptr )
		{

			// If it's the END keyword
			if (asciibuf.compare("DATA") == 0)
			{

				// eat '='
				inFile->scanString(asciibuf);

				readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

				if( parseData( &data, type, buffer ) == FALSE )
				{
					DEBUG_LOG(( "parseGameWindow: Error parsing %s", parse->name ));
					goto cleanupAndExit;
				}

			}
			else if (asciibuf.compare("END") == 0)
			{
				// Check to see if we have a header template, if so, set the font equal to that.
				// GeneralsX @probe Claude 20/08/2026 GX_FONT=2 - this line silently discards the font
				// parsed from FONT = ... . Print both sides of the assignment so the winner is a
				// measurement rather than a reading of the source.
				if( gxFontDiagLevel() >= 2 )
				{
					GameFont *htf = TheHeaderTemplateManager->getFontFromTemplate(instData.m_headerTemplateName);
					fprintf(stderr,
						"[GXFONT2] parse layout='%s' name='%s' hdr='%s' parsedFont='%s' pt=%d -> hdrFont='%s' pt=%d %s\n",
						s_gxParsingLayout,
						instData.m_decoratedNameString.str(),
						instData.m_headerTemplateName.str(),
						instData.m_font ? instData.m_font->nameString.str() : "(none)",
						instData.m_font ? instData.m_font->pointSize : -1,
						htf ? htf->nameString.str() : "(unresolved)",
						htf ? htf->pointSize : -1,
						htf ? "HEADER TEMPLATE WINS" : "parsed font kept");
					fflush(stderr);
				}
				// GeneralsX @bugfix Claude 20/08/2026 In UNIFORM, rebuild the header template's font at the
				// layout's own factor instead of taking the one adjustFontSize sized.
				//
				// A HEADERTEMPLATE discards the FONT this .wnd asked for - 134 of the 290 controls loaded
				// at startup, and every text-drawing control on the main menu - and replaces it with a
				// size derived from display WIDTH alone (ClassicNoCeiling: 1 + (W/800 - 1) * 0.7 = 1.98 at
				// 1920). Boxes that scale uniformly by 1.80 and text that scales by 1.98 disagree by 10%,
				// and the text is the half that overflows.
				//
				// Rebuilt from the template's AUTHORED point size, not resized from its resolved font: the
				// resolved one already has adjustFontSize's factor baked in and scaling that again would
				// compound the two.
				GameFont *gxHeaderFont = TheHeaderTemplateManager->getFontFromTemplate( instData.m_headerTemplateName );

				if( gxHeaderFont != NULL && LayoutScaleActiveMode() == LAYOUT_SCALE_UNIFORM && TheFontLibrary != NULL )
				{
					HeaderTemplate *ht = TheHeaderTemplateManager->findHeaderTemplate( instData.m_headerTemplateName );
					if( ht != NULL )
					{
						Int pt = REAL_TO_INT_FLOOR( (Real)ht->m_point * gxLayoutTextFactor() );
						if( pt < 1 )
							pt = 1;
						// GeneralsX @bugfix Claude 24/09/2026 FontLibrary::getFont no longer refuses above 100
						// (upstream #3051, 620a173c6); it clamps at 512. This clamp stays as a fork-local
						// bound: above about 170 pt a glyph is taller than the 256-pixel glyph atlas and
						// Blit_Char clips it, so a bigger header would draw cut-off text.
						if( pt > 100 )
							pt = 100;

						// Keep the template's own font if the library refuses this size. An overflowing
						// caption is a cosmetic defect; winSetFont(nullptr) draws no glyphs at all.
						GameFont *scaled = TheFontLibrary->getFont( ht->m_fontName, pt, ht->m_bold );
						if( scaled != NULL )
							gxHeaderFont = scaled;
					}
				}

				if( gxHeaderFont != NULL )
					instData.m_font = gxHeaderFont;

				// Create a window using the current description
				if( window == nullptr )
					window = createWindow( type, instData.m_id, instData.getStatus(), x, y,
																 width, height, &instData, data,
																 systemFunc, inputFunc, tooltipFunc, drawFunc );


				goto cleanupAndExit;

			}
			else if (asciibuf.compare("CHILD") == 0)
			{

				// Create a window using the current description
				window = createWindow( type, instData.m_id, instData.getStatus(), x, y,
															 width, height, &instData, data,
															 systemFunc, inputFunc, tooltipFunc, drawFunc );

				if (window == nullptr)
					goto cleanupAndExit;

				// Parses the CHILD's window info.
				if( parseChildWindows( window, inFile, buffer ) == FALSE )
				{

					TheWindowManager->winDestroy( window );
					window = nullptr;
					goto cleanupAndExit;

				}

			}
			else
			{

				// Else it is unrecognized so eat associated data
				readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

			}

		}

	}

cleanupAndExit:

	//
	// this should be true since we should never set the text in
	// display strings in a instance data that is not inside a
	// window ... it's for sanity checking
	//
	// I am commenting this out to get tooltips working, If for
	// some reason we start having displayString problems... CLH
//	assert( instData.m_text == nullptr && instData.m_tooltip == nullptr );

	return window;

}

//=================================================================================================
//=================================================================================================
//=================================================================================================

//-------------------------------------------------------------------------------------------------
/** Parse init for layout file */
//-------------------------------------------------------------------------------------------------
Bool parseInit( const char *token, char *buffer, UnsignedInt version, WindowLayoutInfo *info )
{
	char *c;
	const char *seps = " \n\r\t";

	// get string
	c = strtok( buffer, seps );

	// translate string to function address
	info->initNameString = c;
	info->init = TheFunctionLexicon->winLayoutInitFunc( TheNameKeyGenerator->nameToKey( info->initNameString ) );

	return TRUE;  // success

}

//-------------------------------------------------------------------------------------------------
/** Parse update for layout file */
//-------------------------------------------------------------------------------------------------
Bool parseUpdate( const char *token, char *buffer, UnsignedInt version, WindowLayoutInfo *info )
{
	char *c;
	const char *seps = " \n\r\t";

	// get string
	c = strtok( buffer, seps );

	// translate string to function address
	info->updateNameString = c;
	info->update = TheFunctionLexicon->winLayoutUpdateFunc( TheNameKeyGenerator->nameToKey( info->updateNameString ) );

	return TRUE;  // success

}

//-------------------------------------------------------------------------------------------------
/** Parse shutdown for layout file */
//-------------------------------------------------------------------------------------------------
Bool parseShutdown( const char *token, char *buffer, UnsignedInt version, WindowLayoutInfo *info )
{
	char *c;
	const char *seps = " \n\r\t";

	// get string
	c = strtok( buffer, seps );

	// translate string to function address
	info->shutdownNameString = c;
	info->shutdown = TheFunctionLexicon->winLayoutShutdownFunc( TheNameKeyGenerator->nameToKey( info->shutdownNameString ) );

	return TRUE;  // success

}

static LayoutScriptParse layoutScriptTable[] =
{
	{ "LAYOUTINIT",										parseInit },
	{ "LAYOUTUPDATE",									parseUpdate },
	{ "LAYOUTSHUTDOWN",								parseShutdown },

	{ nullptr,															nullptr },

};

//-------------------------------------------------------------------------------------------------
/** Parse the layout block which MUST be present in every window file */
//-------------------------------------------------------------------------------------------------
Bool parseLayoutBlock( File *inFile, char *buffer, UnsignedInt version, WindowLayoutInfo *info )
{
	LayoutScriptParse *parse;
	char token[ 256 ];

	AsciiString asciitoken;
	if (inFile->scanString(asciitoken) == FALSE) {
		return FALSE;
	}

	// better be the layout block
	if (asciitoken.compare("STARTLAYOUTBLOCK") != 0) {
		return FALSE;
	}

	while( TRUE )
	{

		// get next token
		inFile->scanString(asciitoken);

		// check for end
		if (asciitoken.compare("ENDLAYOUTBLOCK") == 0) {
			break;
		}

		// search for token in the table
		for( parse = layoutScriptTable; parse && parse->name; parse++ )
		{

			if (asciitoken.compare(parse->name) == 0)
			{
				char *c;

				// read from file
				readUntilSemicolon( inFile, buffer, WIN_BUFFER_LENGTH );

				// eat equals separator " = "
				c = strtok( buffer, " =" );

				strlcpy(token, asciitoken.str(), ARRAY_SIZE(token));

				// parse it
				if( parse->parse( token, c, version, info ) == FALSE )
					return FALSE;

				break;  // exit for

			}

		}

	}

	return TRUE;

}

///////////////////////////////////////////////////////////////////////////////
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// GameWindowManager::winCreateLayout =========================================
/** Load window(s) from a .wnd definition file and wrap within a
	* new window layout */
//=============================================================================
WindowLayout *GameWindowManager::winCreateLayout( AsciiString filename )
{
	WindowLayout *layout;

	// allocate a new window layout
	layout = newInstance(WindowLayout);

	// load windows into layout
	if( layout->load( filename ) == FALSE )
	{

		deleteInstance(layout);
		return nullptr;

	}

	// return loaded layout
	return layout;

}

/** Free up the memory used by static strings.  Normally this memory
is freed by the string destructor but we do it here to make the
memory leak detection code happy.*/
void GameWindowManager::freeStaticStrings()
{
	theSystemString.clear();
	theInputString.clear();
	theTooltipString.clear();
	theDrawString.clear();
}

WindowLayoutInfo::WindowLayoutInfo() :
	version(0),
	init(nullptr),
	update(nullptr),
	shutdown(nullptr),
	initNameString(AsciiString::TheEmptyString),
	updateNameString(AsciiString::TheEmptyString),
	shutdownNameString(AsciiString::TheEmptyString)
{
		windows.clear();
}

// GeneralsX @probe Claude 20/08/2026 GX_FONT=2 - name the control that owns each font.
//
// The previous session measured font sizes per DRAWN STRING and could not attribute them to a
// control, so "the first row is 23pt and the rows below are 13pt" had no owner and the fix was
// aimed by guesswork - a clamp in createWindow that never fired. This walks the layout AFTER it is
// fully built and prints, for every window, the decorated name next to the font that window will
// actually draw with. A combo box draws through child windows, so those are printed too: if a
// combo's edit box carries a different font from the combo itself, the number the operator sees on
// rows 1..7 was never the combo's own font at all.
static const char *gxWinTypeName( UnsignedInt style )
{
	if( BitIsSet( style, GWS_COMBO_BOX ) )      return "COMBOBOX";
	if( BitIsSet( style, GWS_SCROLL_LISTBOX ) ) return "LISTBOX";
	if( BitIsSet( style, GWS_ENTRY_FIELD ) )    return "ENTRYFIELD";
	if( BitIsSet( style, GWS_STATIC_TEXT ) )    return "STATICTEXT";
	if( BitIsSet( style, GWS_PUSH_BUTTON ) )    return "PUSHBUTTON";
	if( BitIsSet( style, GWS_CHECK_BOX ) )      return "CHECKBOX";
	if( BitIsSet( style, GWS_RADIO_BUTTON ) )   return "RADIOBUTTON";
	if( BitIsSet( style, GWS_TAB_CONTROL ) )    return "TABCONTROL";
	if( BitIsSet( style, GWS_USER_WINDOW ) )    return "USER";
	return "?";
}

static void gxDumpOneWindow( GameWindow *win, const char *layout, const char *lineage, Int depth )
{
	if( win == nullptr )
		return;

	WinInstanceData *inst = win->winGetInstanceData();
	GameFont *f = win->winGetFont();
	Int w = 0, h = 0, sx = 0, sy = 0;
	win->winGetSize( &w, &h );
	win->winGetScreenPosition( &sx, &sy );

	fprintf(stderr,
		"[GXFONT2] %s d=%d %-10s win=%p name='%s' hdr='%s' rect=%d,%d %dx%d font='%s' pt=%d rasterH=%d bold=%d %s\n",
		layout, depth, gxWinTypeName( win->winGetStyle() ), (void *)win,
		inst ? inst->m_decoratedNameString.str() : "(none)",
		inst ? inst->m_headerTemplateName.str() : "(none)",
		sx, sy, w, h,
		f ? f->nameString.str() : "(NULL FONT)",
		f ? f->pointSize : -1,
		f ? f->height : -1,
		f ? (Int)f->bold : -1,
		lineage );
}

// Deliberately NOT a winGetNext() walk from the top: m_next chains the manager's GLOBAL window
// list, so following it from a layout's root would dump every other screen's windows too. The
// caller iterates the layout's own window list; m_next is only followed inside a child chain,
// where it does mean "sibling".
static void gxDumpWindowSubtree( GameWindow *win, const char *layout, Int depth )
{
	if( win == nullptr )
		return;

	gxDumpOneWindow( win, layout, "", depth );

	// A combo box's visible glyphs come from its edit box, and its drop-down rows from its list
	// box. Those two are created inside gogoGadgetComboBox from a freshly init()ed
	// WinInstanceData - whose m_font is nullptr - so name them explicitly rather than trust that
	// the combo's own font describes what is on screen. They also appear in the child walk below;
	// the win=%p on every line is what ties the two together.
	if( BitIsSet( win->winGetStyle(), GWS_COMBO_BOX ) )
	{
		gxDumpOneWindow( GadgetComboBoxGetEditBox( win ), layout, "<- combo.editBox", depth + 1 );
		gxDumpOneWindow( GadgetComboBoxGetListBox( win ), layout, "<- combo.listBox", depth + 1 );
	}

	for( GameWindow *child = win->winGetChild(); child != nullptr; child = child->winGetNext() )
		gxDumpWindowSubtree( child, layout, depth + 1 );
}


// GameWindowManager::winCreateFromScript =====================================
/** Parse through a window .wnd file and create all the windows
	* within it.
	*
	* NOTE: The FIRST window created from the script is returned, this
	* way if you want to know ALL of the windows created from this
	* layout file you must iterate over info->windows, since the windows will
	* not be at the head of the window list if there is a modal window active.
	*/
//=============================================================================
GameWindow *GameWindowManager::winCreateFromScript( AsciiString filenameString,
																										WindowLayoutInfo *info )
{
	const char* filename = filenameString.str();
	s_gxParsingLayout = filename;								// @probe: see the note on s_gxParsingLayout
	static char buffer[ WIN_BUFFER_LENGTH ]; 		// input buffer for reading
	GameWindow *firstWindow = nullptr;
  GameWindow *window;
  char filepath[ _MAX_PATH ] = "Window\\";
  File *inFile;
	WindowLayoutInfo scriptInfo;
	AsciiString asciibuf;

	// zero info struct
	//memset( &scriptInfo, 0, sizeof( WindowLayoutInfo ) ); // it's a class - use a constructor

  // Reset the window stack
  resetWindowStack();
	resetWindowDefaults();

	//
	// get the filename from the parameter, if it doesn't contain a '\' it is
	// a it is assumed to be a filename only, which we will prefix a "window\"
	// directory to, otherwise it is assumed to be an absolute path.  When using
	// a filename only make sure the current directory is set to the right
	// place for the window files subdirectory
	//
	if( strchr( filename, '\\' ) == nullptr )
		snprintf( filepath, ARRAY_SIZE(filepath), "Window\\%s", filename );
	else
		strlcpy(filepath, filename, ARRAY_SIZE(filepath));

	// GeneralsX @bugfix Claude 25/08/2026 Pass one, before any window is created, so parseScreenRect
	// has a ROW to anchor on. Resets the table unconditionally - a layout that is not scanned must
	// not inherit the previous layout's groups. See gxScanLayoutGroups above.
	gxScanLayoutGroups( filepath );

  // Open the input file
	inFile = TheFileSystem->openFile(filepath, File::READ);
	if (inFile == nullptr)
	{
		DEBUG_LOG(( "WinCreateFromScript: Cannot access file '%s'.", filename ));
		return nullptr;
	}

  // read into memory
  inFile=inFile->convertToRAMFile();

	// read the file version
	Int version;
	inFile->read(nullptr, strlen("FILE_VERSION = "));
	inFile->scanInt(version);
	inFile->nextLine();

	// version 2+ have a special block called the layout block
	if( version >= 2 )
	{

		if( parseLayoutBlock( inFile, buffer, version, &scriptInfo ) == FALSE )
		{

			DEBUG_LOG(( "WinCreateFromScript: Error parsing layout block" ));
			inFile->close();
			inFile = nullptr;
			return FALSE;

		}

	}
	else
	{

		// default none names
		scriptInfo.initNameString = "[None]";
		scriptInfo.updateNameString = "[None]";
		scriptInfo.shutdownNameString = "[None]";

	}

	while( TRUE )
	{

		if (inFile->scanString(asciibuf) == FALSE) {
			break;
		}

		if (asciibuf.compare("END") == 0) {
      continue;
		}

		if (asciibuf.compare("ENABLEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defEnabledColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("DISABLEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defDisabledColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("HILITECOLOR") == 0)
		{

			if( parseDefaultColor( &defHiliteColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("SELECTEDCOLOR") == 0)
		{

			if( parseDefaultColor( &defSelectedColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("TEXTCOLOR") == 0)
		{

			if( parseDefaultColor( &defTextColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("BACKGROUNDCOLOR") == 0)
		{

			if( parseDefaultColor( &defBackgroundColor, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("FONT") == 0)
		{

			if( parseDefaultFont( defFont, inFile, buffer ) == FALSE )
			{
				inFile->close();
				inFile = nullptr;
				return nullptr;
			}

		}
		else if (asciibuf.compare("WINDOW") == 0)
		{

      // Parse window descriptions until the last END is read
      window = parseWindow( inFile, buffer );

			// save first window created
			if( firstWindow == nullptr )
				firstWindow = window;

			scriptInfo.windows.push_back(window);

    }

	}

	// close the file
	inFile->close();
	inFile = nullptr;

	// if info parameter is provided, copy info to the param
	if( info )
		*info = scriptInfo;

	// GeneralsX @probe Claude 20/08/2026 GX_FONT=2 - dump the built layout, so every font printed
	// here carries the name of the control that will draw with it.
	if( gxFontDiagLevel() >= 2 )
	{
		fprintf(stderr, "[GXFONT2] ===== layout '%s' built, %d top-level windows =====\n",
			filename, (Int)scriptInfo.windows.size());
		for( GameWindowList::const_iterator it = scriptInfo.windows.begin();
				 it != scriptInfo.windows.end(); ++it )
			gxDumpWindowSubtree( *it, filename, 0 );
		fflush(stderr);
	}

	// return the first window created
	return firstWindow;

}

