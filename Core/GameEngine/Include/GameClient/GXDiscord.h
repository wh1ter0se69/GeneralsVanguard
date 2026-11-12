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

/* Discord Rich Presence. Under GameClient, outside the SimID source digest: it writes nothing the simulation observes.
 * NO LIBRARY, NO NETWORK: a local IPC protocol (JSON over a named pipe on Windows, a unix socket elsewhere).
 * Desktop-only (iOS has no Discord client). IT FAILS SILENT: any failure ends in "no presence", never a per-frame
 * log line. */

#pragma once

#include "Common/GameCommon.h"

/// Called once from GameClient::init(). Safe to call when Discord is absent.
void GXDiscordCreate(void);

/// Called every client frame from GameClient::update(). Cheap: it early-outs unless the presence
/// it would publish has actually changed, and it rate-limits the wire regardless - Discord asks
/// for at most one update every 15 seconds and drops the rest.
void GXDiscordUpdate(void);

/// Called from ~GameClient(). Clears the presence rather than leaving a stale "In a match" on
/// somebody's profile after the game has exited.
void GXDiscordDestroy(void);
