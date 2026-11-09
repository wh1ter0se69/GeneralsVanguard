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

// FILE: DebugMenu.h ///////////////////////////////////////////////////////////////////////////////
// Desc:   Code-built Debug overlay: stats, live player panel, LAN-safe cheats, camera tuning,
//         config and saves
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 Debug overlay reachable from the shell AND from in-game.
**
** This is deliberately NOT a Shell screen. Shell::doPush hardcodes winCreateLayout(filename) and
** would run MainMenuShutdown on whatever is underneath, which is wrong for something that also has
** to open on top of a running match. It is an off-stack WindowLayout built with CodeGui, exactly
** the pattern Shell::getOptionsLayout already uses, so the same instance serves both entry points
** and Back is a plain destroy with no pop/shutdownComplete handshake to stall on.
**
** GeneralsX @feature Claude 27/07/2026 The cheats work in a LAN game.
**
** They are appended to TheMessageStream as MSG_GX_CHEAT_* orders, which sit in the 1000..1999
** network band and are therefore shipped to every peer, deleted locally, and re-delivered - to the
** sender too - on one stamped logic frame in fixed slot order. Every peer runs the identical
** handler on the identical frame, so there is nothing left to diverge and the old
** single-player-only gate is gone. Each order carries an explicit TARGET PLAYER INDEX, so a cheat
** can be aimed at any player rather than always the local one. Two consequences worth knowing:
** the effect lands a few frames after the tap, on the sender's screen as well, and both peers must
** be running the same binary because the enum values ARE the protocol.
**
** GeneralsX @security Claude 18/09/2026 Only while at most one human is in the match. With a second
** human every peer's simulation refuses the orders (GameLogic::gxCheatsAllowed) - a cheat any peer
** can send is a way to remove any other player.
**
** GeneralsX @security Claude 25/09/2026 ...or from a player the RELAY proved an admin for the match
** (GameLogic::gxCheatAllowedFor), hosting or not, and every player then sees who used which cheat on whom.
**
** GeneralsX @feature Claude 27/07/2026 Spawn and delete get their own tab.
**
** Spawning used to mean typing an exact INI Object name into a text box, which is not something
** anyone can do on a touch device and gave nothing back but "no such template" when it went wrong.
** It is now a searchable list of every template the ThingFactory knows, with a count stepper, a
** veterancy cycle and three named delete actions - so the DELETE_OBJECTS order's "every type" and
** "all of them" sentinels are buttons rather than zeroes the user has to know to type.
**
** It is a SEPARATE PANE from Cheats because the panes are a fixed height and Cheats was already
** near the bottom of it; a control laid out past its parent's rect is invisible and unhittable, not
** merely clipped. The two panes share one target player - the Spawn pane reads the Cheats pane's
** selection and says so - because two target lists on one screen is a bug waiting to be filed.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"

void DebugMenuOpen  ( void );		///< build (if needed) and show the overlay
void DebugMenuClose ( void );		///< destroy it; safe to call from inside its own button handler
void DebugMenuToggle( void );		///< open if closed, close if open — for the floating button
Bool DebugMenuIsOpen( void );

/// Refresh the live readouts. Driven by the overlay's own draw func so the screen is self-contained,
/// but exposed so a per-frame owner (GameClient::update) can drive it too. Idempotent and throttled.
void DebugMenuUpdate( void );
