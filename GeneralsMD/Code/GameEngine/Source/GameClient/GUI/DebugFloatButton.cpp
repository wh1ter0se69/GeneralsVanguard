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

// FILE: DebugFloatButton.cpp //////////////////////////////////////////////////////////////////////
// Desc:   Always-on-top floating button that opens the Debug overlay
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/OptionPreferences.h"

#include "Common/GlobalData.h"
#include "GameClient/CodeGui.h"
#include "GameClient/DebugFloatButton.h"
#include "GameClient/DebugMenu.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/WinInstanceData.h"

//-------------------------------------------------------------------------------------------------
// The host panel is a parentless window sized EXACTLY to the visible widget. findWindowUnderMouse
// tests the ROOT window's own region before descending, so an oversized ABOVE root would swallow
// taps on the map and the control bar everywhere it overhung — which is also why retracting has to
// shrink the host and not merely hide the button inside it. SEE_THRU keeps it from painting a
// second box behind its children while still letting drawWindow recurse into it.
static GameWindow *s_hostPanel = nullptr;
static GameWindow *s_button    = nullptr;		// the DEBUG action, hidden while retracted
static GameWindow *s_tab       = nullptr;		// the collapse/expand handle, always present

// Latched by DebugFloatButtonDestroy so the per-frame keep-alive cannot resurrect the overlay into a
// window manager that ~GameClient is about to delete.
static Bool s_shutdown = FALSE;

// Display size the current rect was computed against, so a resolution change can be detected.
static Int s_builtForW = 0;
static Int s_builtForH = 0;

//-------------------------------------------------------------------------------------------------
// Widget state that must outlive the windows themselves.
//
// GameWindowManager::reset() calls winDestroyAll() on a match transition and frees the overlay
// without going through DebugFloatButtonDestroy; the keep-alive then rebuilds it. If the placement
// lived in the GameWindows, every transition would fling the button back to the default corner and
// re-open it. It lives here instead, and debugFloatBuild() reconstructs the layout from it.
//
// The position is stored as a FRACTION of the display rather than in device pixels so a resolution
// change moves the widget proportionally instead of dumping it in a corner or off the edge.
static Bool s_placed		= FALSE;		// FALSE => never touched, still in the default corner
static Real s_fracX			= 0.0f;			// host top-left as a fraction of the display
static Real s_fracY			= 0.0f;
static Bool s_retracted	= FALSE;
static Int  s_laidInset	= 0;			// the spectator HUD's top inset the default corner was last placed under
static Bool s_dockLeft	= FALSE;		// edge a retract snaps to; FALSE matches the top-RIGHT default

//-------------------------------------------------------------------------------------------------
// Live drag gesture. s_dragArmed means one of our gadgets is the window manager's grab window.
static Bool			s_dragArmed		= FALSE;
static Bool			s_dragMoved		= FALSE;		// ...and it cleared the dead zone, so it is a move
static ICoord2D	s_dragAnchor	= { 0, 0 };
static ICoord2D	s_dragOrigin	= { 0, 0 };

// Matches TAP_DEAD_ZONE_PX in SDL3GameEngine.cpp, and measured the same way (Manhattan distance in
// device pixels) so the two layers agree on what counts as a tap.
static const Int DEBUG_FLOAT_DEAD_ZONE_PX = 8;

// Defined in InGameUI.cpp (GXSpectatorHud.inl): where the spectator HUD's top strip ends (0 =
// no strip). The default corner sits below it - at y = margin the DEBUG button covered the last player card.
extern Int gxSpectatorHudTopInset();

static WindowMsgHandledType DebugFloatSystem( GameWindow *window, UnsignedInt msg,
																							WindowMsgData mData1, WindowMsgData mData2 );

//-------------------------------------------------------------------------------------------------
/** Control sizes, in device pixels. */
//-------------------------------------------------------------------------------------------------
static void debugFloatMetrics( Int *buttonW, Int *buttonH, Int *tabW )
{
	// Floored generously: SDL3GameEngine promotes more than TAP_DEAD_ZONE_PX (8 device px) of finger
	// travel to a drag, and a drag off a push button clears WIN_STATE_SELECTED so GBM_SELECTED never
	// fires. A small target on an iPad is a button that "sometimes does nothing".
	*buttonW = CodeGuiAtLeast( CodeGuiScaleX( 96 ), 96 );
	*buttonH = CodeGuiAtLeast( CodeGuiScaleY( 30 ), 44 );

	// The tab is the only thing on screen while retracted, so it is as narrow as a thumb allows
	// rather than as narrow as it could be drawn.
	*tabW = CodeGuiAtLeast( CodeGuiScaleX( 24 ), 36 );
}

//-------------------------------------------------------------------------------------------------
/** Footprint of the host for the current retract state. */
//-------------------------------------------------------------------------------------------------
static void debugFloatHostSize( Int *width, Int *height )
{
	Int buttonW, buttonH, tabW;
	debugFloatMetrics( &buttonW, &buttonH, &tabW );

	*width	= s_retracted ? tabW : tabW + buttonW;
	*height	= buttonH;
}

//-------------------------------------------------------------------------------------------------
/** Move the host, clamped so it can never end up wholly off-screen and unrecoverable, and record
	* where it landed. Uses the host's CURRENT size, so callers must resize before they place. */
//-------------------------------------------------------------------------------------------------
static void debugFloatPlaceHost( Int x, Int y )
{
	if( s_hostPanel == nullptr )
		return;

	Int width, height;
	s_hostPanel->winGetSize( &width, &height );

	const Int screenW = CodeGuiAtLeast( CodeGuiScreenW(), 1 );
	const Int screenH = CodeGuiAtLeast( CodeGuiScreenH(), 1 );

	// Fully on-screen, the same rule the engine's own WIN_STATUS_DRAGGABLE path enforces. A partly
	// off-screen widget would be legal, but the part left behind is the part you have to hit to get
	// it back, and on a touch screen there is no keyboard escape hatch.
	const Int maxX = CodeGuiAtLeast( screenW - width, 0 );
	const Int maxY = CodeGuiAtLeast( screenH - height, 0 );

	if( x < 0 )
		x = 0;
	else if( x > maxX )
		x = maxX;

	if( y < 0 )
		y = 0;
	else if( y > maxY )
		y = maxY;

	s_hostPanel->winSetPosition( x, y );

	s_fracX = (Real)x / (Real)screenW;
	s_fracY = (Real)y / (Real)screenH;
}

//-------------------------------------------------------------------------------------------------
/** Which screen edge a retract collapses toward. Recomputed only when the gesture ends, never
	* mid-drag: swapping the tab across the widget while the finger is still down moves the thing
	* the finger is holding. */
//-------------------------------------------------------------------------------------------------
static void debugFloatUpdateDockSide( void )
{
	if( s_hostPanel == nullptr )
		return;

	Int x, y, width, height;
	s_hostPanel->winGetPosition( &x, &y );
	s_hostPanel->winGetSize( &width, &height );

	s_dockLeft = ( x + width / 2 ) < ( CodeGuiScreenW() / 2 );
}

//-------------------------------------------------------------------------------------------------
/** Size, arrange and place everything from the persistent state. Idempotent, and the single path
	* that both a rebuild and a retract toggle go through, so the two can never disagree. */
//-------------------------------------------------------------------------------------------------
static void debugFloatApplyLayout( void )
{
	if( s_hostPanel == nullptr || s_button == nullptr || s_tab == nullptr )
		return;

	Int buttonW, buttonH, tabW;
	debugFloatMetrics( &buttonW, &buttonH, &tabW );

	Int hostW, hostH;
	debugFloatHostSize( &hostW, &hostH );
	s_hostPanel->winSetSize( hostW, hostH );

	//
	// The tab sits on the docked side of the widget in BOTH states, which is what makes it hold
	// still while the drawer opens and closes: docked right, expanded puts the host at
	// screenW-(tabW+buttonW) with the tab at +buttonW, and retracted puts the host at screenW-tabW
	// with the tab at 0 — the same absolute pixel either way. The handle you just tapped is still
	// under your finger, so a mis-tap cannot happen on the way back.
	//
	if( s_retracted )
	{
		s_button->winHide( TRUE );
		s_tab->winSetPosition( 0, 0 );
	}
	else
	{
		s_button->winHide( FALSE );
		s_button->winSetPosition( s_dockLeft ? tabW : 0, 0 );
		s_tab->winSetPosition( s_dockLeft ? 0 : buttonW, 0 );
	}

	// Chevrons name the outcome, not the state: outward while open (this collapses toward that
	// edge), inward while collapsed (this pulls the drawer back out). ASCII only — the shipped
	// fonts are not guaranteed to carry guillemets or arrows.
	const Bool pointRight = s_retracted ? s_dockLeft : !s_dockLeft;
	GadgetButtonSetText( s_tab, UnicodeString( pointRight ? L">>" : L"<<" ) );

	Int x, y;
	if( s_placed )
	{
		x = (Int)( s_fracX * (Real)CodeGuiScreenW() + 0.5f );
		y = (Int)( s_fracY * (Real)CodeGuiScreenH() + 0.5f );
	}
	else
	{
		const Int margin = CodeGuiAtLeast( CodeGuiScaleX( 10 ), 12 );
		x = CodeGuiScreenW() - hostW - margin;
		s_laidInset = gxSpectatorHudTopInset();
		y = CodeGuiAtLeast( margin, s_laidInset );
	}

	// Retracted means flush with the edge, whatever the stored fraction says — that is the whole
	// point of the state, and the clamp below would otherwise leave a margin-wide dead strip.
	if( s_retracted )
		x = s_dockLeft ? 0 : CodeGuiScreenW() - hostW;

	debugFloatPlaceHost( x, y );
}

//-------------------------------------------------------------------------------------------------
/** Un-press a gadget by hand.
	*
	* GadgetPushButtonInput clears WIN_STATE_SELECTED from its own GWM_LEFT_UP, and
	* winProcessMouseEvent only delivers that when the release lands inside the grabbed gadget — so a
	* drag that ran into the screen-edge clamp leaves the button drawn permanently pushed. On a mouse
	* the next GWM_MOUSE_LEAVING would tidy it up; on touch nothing moves again until the next tap. */
//-------------------------------------------------------------------------------------------------
static void debugFloatClearPressed( GameWindow *gadget )
{
	if( gadget == nullptr )
		return;

	WinInstanceData *instData = gadget->winGetInstanceData();
	if( instData )
		BitClear( instData->m_state, WIN_STATE_SELECTED );
}

//-------------------------------------------------------------------------------------------------
/** Finish the current gesture. Safe to call when no gesture is running. */
//-------------------------------------------------------------------------------------------------
static void debugFloatEndDrag( void )
{
	const Bool moved = s_dragMoved;

	s_dragArmed = FALSE;
	s_dragMoved = FALSE;

	debugFloatClearPressed( s_button );
	debugFloatClearPressed( s_tab );

	if( moved == FALSE )
		return;

	// Re-home only now that the finger is off. Snapping a retracted tab back to an edge it is
	// actively being pulled away from would make it impossible to move.
	debugFloatUpdateDockSide();
	debugFloatApplyLayout();
}

//-------------------------------------------------------------------------------------------------
/** One drag report, in absolute device pixels. */
//-------------------------------------------------------------------------------------------------
static void debugFloatDragTo( Int mouseX, Int mouseY )
{
	if( s_hostPanel == nullptr )
		return;

	if( s_dragArmed == FALSE )
	{
		//
		// The mouse-down itself never reaches this window: GadgetPushButtonInput consumes it and
		// winProcessMouseEvent makes the gadget the grab window, which is what routes every
		// subsequent drag here no matter where the finger wanders. So the first drag report is the
		// only anchor available. On touch that costs one extra dead zone, because SDL3GameEngine has
		// already spent TAP_DEAD_ZONE_PX deciding this was a drag at all — cheap, and it keeps a real
		// mouse honest too, where the press and the first motion genuinely are the same point.
		//
		s_dragArmed			= TRUE;
		s_dragMoved			= FALSE;
		s_dragAnchor.x	= mouseX;
		s_dragAnchor.y	= mouseY;
		s_hostPanel->winGetPosition( &s_dragOrigin.x, &s_dragOrigin.y );
		return;
	}

	const Int dx = mouseX - s_dragAnchor.x;
	const Int dy = mouseY - s_dragAnchor.y;

	if( s_dragMoved == FALSE )
	{
		const Int travel = ( dx < 0 ? -dx : dx ) + ( dy < 0 ? -dy : dy );
		if( travel < DEBUG_FLOAT_DEAD_ZONE_PX )
			return;			// still a tap as far as anyone is concerned

		s_dragMoved	= TRUE;
		s_placed		= TRUE;
	}

	// Anchored, not incremental: accumulating per-frame deltas would drift by exactly the amount the
	// clamp eats every time the widget is pushed into a screen edge and pulled back out.
	debugFloatPlaceHost( s_dragOrigin.x + dx, s_dragOrigin.y + dy );
}

//-------------------------------------------------------------------------------------------------
/** Do the actual construction. Separate from the public Create so the keep-alive can rebuild
	* without clearing the shutdown latch. */
//-------------------------------------------------------------------------------------------------
static void debugFloatBuild( void )
{
	if( s_hostPanel != nullptr || s_shutdown )
		return;

	// TheDisplay is required, not optional: CodeGuiScreenW/H fall back to 1 when it is absent, which
	// would silently produce a 1-pixel-wide button anchored off-screen.
	if( TheWindowManager == nullptr || TheDisplay == nullptr )
		return;

	// A headless client runs a GameWindowManagerDummy whose draw funcs are all null. Nothing would
	// ever render, and there is no one to tap it.
	if( TheGlobalData && TheGlobalData->m_headless )
		return;

	Int buttonW, buttonH, tabW;
	debugFloatMetrics( &buttonW, &buttonH, &tabW );

	Int hostW, hostH;
	debugFloatHostSize( &hostW, &hostH );

	// NO_FOCUS as well as ABOVE: winSetFocus bails on a NO_FOCUS window, so a floating overlay can
	// never take the keyboard away from the screen underneath it.
	s_hostPanel = CodeGuiPanel( nullptr, 0, 0, hostW, hostH, DebugFloatSystem, "DebugFloat:Host",
															WIN_STATUS_ABOVE | WIN_STATUS_SEE_THRU | WIN_STATUS_NO_FOCUS );
	if( s_hostPanel == nullptr )
		return;

	// Child rects are parent-relative (winPointInChild accumulates parent origins), so the children
	// ride along with every winSetPosition on the host and a drag only has to move the host.
	// debugFloatApplyLayout gives both of them their real positions below.
	s_button = CodeGuiButton( s_hostPanel, 0, 0, buttonW, buttonH,
														"DebugFloat:Button", L"DEBUG", L"Open the debug screen" );
	s_tab		 = CodeGuiButton( s_hostPanel, 0, 0, tabW, buttonH,
														"DebugFloat:Tab", L"<<", L"Collapse or expand the debug button" );
	if( s_button == nullptr || s_tab == nullptr )
	{
		TheWindowManager->winDestroy( s_hostPanel );		// recurses into whichever child did get made
		s_hostPanel	= nullptr;
		s_button		= nullptr;
		s_tab				= nullptr;
		return;
	}

	// Everything visible is derived from the persistent statics, so a rebuild after reset or a
	// resolution change comes back retracted-or-not and where the user left it.
	debugFloatApplyLayout();

	s_builtForW = CodeGuiScreenW();
	s_builtForH = CodeGuiScreenH();

	// Fresh windows cannot inherit a gesture that belonged to the destroyed ones.
	s_dragArmed = FALSE;
	s_dragMoved = FALSE;
}

/// Is the debug overlay wanted at all? ON in a debug build, OFF otherwise, unless Options.ini says DebugMenu = yes.
/// Read once: it is a launch-time choice.
static Bool debugFloatWanted( void )
{
	static Int s_wanted = -1;
	if( s_wanted >= 0 )
		return s_wanted != 0;

#if defined(RTS_DEBUG)
	s_wanted = 1;
#else
	s_wanted = 0;
	{
		OptionPreferences prefs;
		OptionPreferences::const_iterator it = prefs.find( "DebugMenu" );
		if( it != prefs.end() && stricmp( it->second.str(), "yes" ) == 0 )
			s_wanted = 1;
	}
#endif
	return s_wanted != 0;
}

void DebugFloatButtonCreate( void )
{
	if( !debugFloatWanted() )
		return;

	s_shutdown = FALSE;			// an explicit create re-arms the overlay after a Destroy
	debugFloatBuild();
}

//-------------------------------------------------------------------------------------------------
void DebugFloatButtonDestroy( void )
{
	// Latch BEFORE destroying: the keep-alive rebuilds on a null host, and teardown must not hand a
	// fresh window to a manager that is one statement away from being deleted.
	s_shutdown = TRUE;

	if( s_hostPanel != nullptr && TheWindowManager != nullptr )
		TheWindowManager->winDestroy( s_hostPanel );		// recurses into the children

	s_hostPanel	= nullptr;
	s_button		= nullptr;
	s_tab				= nullptr;
	s_builtForW	= 0;
	s_builtForH	= 0;
	s_dragArmed	= FALSE;
	s_dragMoved	= FALSE;

	// Placement and retract state deliberately survive: a Create after this must restore the widget
	// where the user put it, exactly as a rebuild does.
}

//-------------------------------------------------------------------------------------------------
void DebugFloatButtonUpdate( void )
{
	// The same gate as Create(), and it is NOT redundant: the branch below rebuilds the overlay
	// whenever the host panel is null, which after a suppressed Create is every single frame.
	// Gating only the constructor would have put the button straight back.
	if( !debugFloatWanted() )
		return;

	if( s_hostPanel == nullptr )
	{
		// Covers a Create that ran before TheDisplay was usable, and a rebuild after anything that
		// walked the whole window list out from under us (see GWM_DESTROY below).
		debugFloatBuild();
		return;
	}

	//
	// Authoritative end of a drag. GBM_SELECTED is not enough on its own: winProcessMouseEvent only
	// forwards the closing GWM_LEFT_UP to the grabbed gadget when the release lands INSIDE it, and a
	// drag that ran into the screen-edge clamp leaves the finger outside. Without this the stale
	// s_dragMoved would swallow the next genuine tap. The grab window is cleared by
	// winProcessMouseEvent on mouse-up and by winDestroy, so "no longer ours" covers every ending.
	//
	if( s_dragArmed && TheWindowManager )
	{
		GameWindow *grabbed = TheWindowManager->winGetGrabWindow();
		if( grabbed != s_button && grabbed != s_tab )
			debugFloatEndDrag();
	}

	// Runtime-created windows get none of the .wnd parser's CREATIONRESOLUTION rescaling, and
	// Shell::recreateWindowLayouts() re-pushes by filename so it does not know about us either.
	// Nothing else re-anchors this on a resolution change.
	//
	// Rebuild rather than reposition: the control sizes were fixed at creation, so a bare
	// winSetPosition on the host would leave a correctly-anchored widget of the wrong size.
	// winDestroy unlinks from m_windowList immediately and only frees on the next
	// processDestroyList, so the old window stops drawing and hit-testing before the new one exists.
	// The strip's height is known only once the HUD has drawn (and moves with the window size and the PLAYERS tab): a button
	// still in its default corner follows it. One the player dragged stays where he put it.
	if( !s_placed && s_hostPanel != nullptr && gxSpectatorHudTopInset() != s_laidInset )
		debugFloatApplyLayout();

	if( CodeGuiScreenW() != s_builtForW || CodeGuiScreenH() != s_builtForH )
	{
		if( TheWindowManager )
			TheWindowManager->winDestroy( s_hostPanel );

		s_hostPanel	= nullptr;
		s_button		= nullptr;
		s_tab				= nullptr;
		debugFloatBuild();
	}
}

//-------------------------------------------------------------------------------------------------
/** Host panel system callback. gogoGadgetPushButton set both buttons' owner to this panel, so every
	* GBM_ and GGM_ message arrives here — the panel is never given to winSetSystemFunc as a gadget,
	* which is what would orphan a gadget's own GWM_DESTROY handler. */
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugFloatSystem( GameWindow *window, UnsignedInt msg,
																							WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// ------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// Refuse focus. WIN_STATUS_NO_FOCUS already makes winSetFocus bail on us directly, but
			// GadgetPushButtonSystem never answers TRUE for itself, so a winSetFocus aimed at a
			// button walks up to this panel — and whatever it answers here is final.
			if( mData1 == TRUE )
				*(Bool *)mData2 = FALSE;
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		case GGM_LEFT_DRAG:
		{
			//
			// GadgetPushButtonInput forwards its own GWM_LEFT_DRAG here as
			// (mData1 = the gadget, mData2 = packed mouse position). Both children forward it, so the
			// whole widget is a drag handle — the tab HAS to be one, or a retracted widget could
			// never be moved off the edge it snapped to.
			//
			GameWindow *from = (GameWindow *)mData1;
			if( from != s_button && from != s_tab )
				break;

			debugFloatDragTo( LOLONGTOSHORT( mData2 ), HILONGTOSHORT( mData2 ) );
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *from = (GameWindow *)mData1;
			if( from != s_button && from != s_tab )
				break;

			//
			// A finished drag usually arrives here too: the gadget rides under the finger for the
			// whole gesture, so the release almost always lands inside it and GadgetPushButtonInput
			// reports a click. The dead zone is what separates the two, exactly as the touch layer
			// separates a tap from a drag-box.
			//
			const Bool wasDrag = s_dragMoved;
			debugFloatEndDrag();
			if( wasDrag )
				return MSG_HANDLED;

			if( from == s_tab )
			{
				s_retracted	= !s_retracted;
				s_placed		= TRUE;		// the collapsed edge position is now the widget's own

				// Pick the edge before collapsing, so it goes to the one it is already nearest.
				if( s_retracted )
					debugFloatUpdateDockSide();

				debugFloatApplyLayout();
			}
			else
			{
				DebugMenuToggle();
			}

			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			// GameWindowManager::reset() -> winDestroyAll() takes the overlay on a match transition
			// without going through DebugFloatButtonDestroy, and shutdown does the same. Drop ALL
			// three pointers or the next DebugFloatButtonUpdate dereferences freed memory. The
			// identity test matters: on a resolution rebuild this fires for the OLD panel one frame
			// after s_hostPanel already points at the new one.
			if( window == s_hostPanel )
			{
				s_hostPanel	= nullptr;
				s_button		= nullptr;
				s_tab				= nullptr;
				s_dragArmed	= FALSE;
				s_dragMoved	= FALSE;
			}
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		default:
			break;

	}

	return MSG_IGNORED;
}
