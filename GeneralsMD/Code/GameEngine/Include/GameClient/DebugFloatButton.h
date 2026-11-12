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
** Floating DEBUG button: a parentless, off-stack GameWindow with WIN_STATUS_ABOVE, present in the shell and in-game.
** GameWindowManager::reset() frees it on match transitions; DebugFloatButtonUpdate rebuilds it on the next frame.
** Drag moves it, tapping DEBUG opens the debug screen, the chevron tab collapses it to an edge.
** Position (as a display fraction) and retract state live in file statics so the rebuild restores them.
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
