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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: FileTransfer.h
// Author: Matthew D. Campbell, December 2002
// Description: File Transfer wrapper using TheNetwork
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

class GameInfo;

// Convenience functions
AsciiString GetBasePathFromPath( AsciiString path );
AsciiString GetFileFromPath( AsciiString path );
AsciiString GetExtensionFromFile( AsciiString fname );
AsciiString GetBaseFileFromFile( AsciiString fname );
AsciiString GetPreviewFromMap( AsciiString path );
AsciiString GetINIFromMap( AsciiString path );
AsciiString GetStrFileFromMap( AsciiString path );
AsciiString GetSoloINIFromMap( AsciiString path );
AsciiString GetAssetUsageFromMap( AsciiString path );
AsciiString GetReadmeFromMap( AsciiString path );

// The meat of file (map) transfers
// GeneralsX @bugfix Claude 10/08/2026 keepShellUp: run the transfer as an OVERLAY over the live
// room screen instead of shutting the shell down around it. Shell::hideShell runs the top
// screen's SHUTDOWN and the matching showShell re-runs its INIT - and the room screen's init is
// written for entering a fresh room: on the host it rebuilds the game from preferences, which
// emptied the slot list, duplicated the CodeGui password row and swapped the background after a
// transfer-only start (measured live, 2026-08-10, three peers). A delivery run must return to
// the SAME room, so its caller passes TRUE and the shell is never touched on success.
Bool DoAnyMapTransfers(GameInfo *game, Bool keepShellUp = FALSE);
