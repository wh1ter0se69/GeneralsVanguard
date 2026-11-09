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

// FILE: DebugFloatButton.h ////////////////////////////////////////////////////////////////////////
// Desc:   Always-on-top floating button that opens the Debug overlay
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 Floating DEBUG button, reachable from the shell AND in-game.
**
** This is a parentless, off-stack GameWindow carrying WIN_STATUS_ABOVE, not a control on any screen.
** That is the only construction that survives everything the game does to its GUI: Shell::push and
** Shell::pop only walk m_screenStack, WindowLayout::hide only walks its own layout, and
** GameClient::reset never touches the window manager. The overlay is therefore present from
** GameClient::init to ~GameClient, across every menu, every load screen and every match.
** WIN_STATUS_ABOVE puts it in winRepaint's third and last draw pass and in winProcessMouseEvent's
** first hit-test pass, so it draws over and takes taps ahead of the in-game control bar.
**
** The one thing that does take it away is GameWindowManager::reset() -> winDestroyAll(), which frees
** every window without going through DebugFloatButtonDestroy. That is survivable rather than fatal:
** the GWM_DESTROY handler drops the pointers and DebugFloatButtonUpdate rebuilds on the next frame.
** Nothing here may assume the windows outlive a match transition.
**
** Four cases where it is present but NOT usable. All are structural, none is a regression, and all
** are acceptable for a debug affordance — see DebugFloatButton.cpp for the mechanism of each:
**   1. While a modal popup is up, taps go to the modal only.
**   2. While the tactical view holds the mouse (camera drag), the window system is bypassed.
**   3. Anchored structure placement lets the button's own mouse-up ALSO reach the world.
**   4. Full-screen video, letterbox and cinematic text render after winRepaint and cover it.
**
** GeneralsX @feature Claude 26/07/2026 Movable and retractable.
**
** A fixed overlay always covers something — on the in-game HUD that is the control bar, the minimap
** or the unit you are trying to select — and on a touch-only device there is no keyboard to get it
** out of the way. So the widget is a two-part drawer:
**
**   [<<][DEBUG]   drag either part to move it anywhere; tap DEBUG to open the debug screen;
**                 tap the chevron tab to collapse the drawer to the nearest vertical edge.
**   [>>]          collapsed: a thumb-sized tab and nothing else. One tap brings it back.
**
** The tab is laid out on the docked side of the widget in both states, so it occupies the same
** absolute pixel open or closed and never runs away from the finger that just tapped it.
**
** Dragging rides GameWindowManager's grab window (the same mechanism WIN_STATUS_DRAGGABLE uses),
** not winCapture: the gadget takes the grab on mouse-down for free, whereas a capture would have to
** be taken and released by hand and leaking one hijacks the mouse for the entire game. A move is
** told from a tap by the same dead zone the touch layer uses (TAP_DEAD_ZONE_PX), and the host is
** clamped fully on-screen so it can never be flung somewhere it cannot be retrieved from.
**
** Position and retract state live in file statics, NOT in the GameWindows, because
** GameWindowManager::reset() -> winDestroyAll() frees the overlay on a match transition and the
** keep-alive rebuilds it; debugFloatBuild() reconstructs the layout from those statics. The
** position is kept as a fraction of the display so a resolution change moves it proportionally.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"

/// Build the overlay. Idempotent, and safe before TheDisplay exists (it simply does nothing).
/// Call once from GameClient::init(), after TheWindowManager->init().
void DebugFloatButtonCreate( void );

/// Tear it down and stop DebugFloatButtonUpdate from rebuilding it. Placement and retract state are
/// kept, so a later Create restores the widget where the user left it.
/// Call from ~GameClient before `delete TheWindowManager`.
void DebugFloatButtonDestroy( void );

/// Per-frame keep-alive: rebuilds after a resolution change or a window manager reset, covers a
/// Create that ran too early, and closes out a drag whose mouse-up landed off the widget.
/// Cheap — two integer compares in the steady state. Call from GameClient::update().
void DebugFloatButtonUpdate( void );
