/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// In-match kick: the red line every peer draws, and the ticket's packing
// into MSG_SELF_DESTRUCT. The decision itself is Common/GXKickTicket.h (digested); this is GameClient and
// only gathers the inputs and draws.

#pragma once

#include "Lib/BaseType.h"

class GameMessage;
class Player;

/// Called by GameLogic::onSelfDestruct, and only when the self-destruct took a LIVING player out. Draws
/// "<name> was kicked by <label>" (or "You were kicked by <label>") in red when the order's own ticket, or a
/// GXKNOT copy the relay sent this peer, is a genuine ticket for that player's slot. DISPLAY ONLY: it writes
/// nothing the simulation reads and returns nothing. Logs one [GXKICK] line either way.
void gxKickShowLine( Player *victim, const GameMessage *msg );

/// Append the ticket to a MSG_SELF_DESTRUCT that already carries its BOOLEAN: INTEGER len, then ceil(len/4)
/// little-endian INTEGER words. INTEGER and never WIDECHAR, which is sizeof(wchar_t) on the wire.
void gxKickPackOrder( GameMessage *msg, const unsigned char *bytes, Int len );

/// The ticket a MSG_SELF_DESTRUCT carries, or -1 when it carries none or anything malformed.
Int gxKickUnpackOrder( const GameMessage *msg, unsigned char *out, Int outMax );

/// The reason= the [GXKICK] exec line prints. LOG TEXT ONLY: nothing reads
/// it back but people and scripts/test. `argCount` is the MSG_SELF_DESTRUCT's argument count. Every ordinary
/// departure - the Surrender button, Network::quitGame, the disconnect path - appends its BOOLEAN and nothing
/// else, so one argument means the order carried no ticket; with no relay copy deciding either
/// (`cacheDecided` false) that is a plain departure, "leave". Anything else is the verdict's own name, so
/// "malformed" is left meaning what GXKickTicket.h says: a ticket was there and is not one. Until this, a plain
/// quit logged "malformed" and three ordinary departures were reported as kicks.
inline const char *gxKickExecReason( int argCount, bool cacheDecided, const char *verdictName )
{
	return ( argCount > 1 || cacheDecided ) ? verdictName : "leave";
}
