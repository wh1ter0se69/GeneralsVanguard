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

#pragma once

#include "Common/GameState.h"
#include "GameClient/GameWindow.h"	// GameWinMsgBoxFunc

void presentSaveResult( const SaveResult &result );
void presentLoadResult( SaveCode result, const AsciiString &filename );

// Call before ANY load route tears anything down. TRUE means the caller must stop now: the save was
// refused (LAN/online), its load was stopped because it would have crashed (the player has been told, and
// a crash log written where the build has a writer), or a "SAVE FROM ANOTHER BUILD" Yes/No box is up. Yes
// has runPendingSaveLoad call retry(), which must re-enter the caller; the gate then lets exactly that save
// past the build warning once (it is scanned again: the refusal and crash checks still apply). No leaves
// the player where they were. FALSE: load it.
Bool gateSaveLoad( const AsciiString &filename, GameWinMsgBoxFunc retry );
// GameClient::update, every frame: runs the retry a Yes asked for, once the box that asked is destroyed,
// and shows a load's message box once the reset after a failed load has been and gone.
void runPendingSaveLoad();

// A guard inside a load turned what would have been a crash into a throw: record why. The next failed
// load reported through presentLoadResult or reportPreventedLoadCrash then says it would have crashed,
// and writes the crash log, instead of the plain "cannot load" box.
void notePreventedLoadCrash( const char *cause );
// TRUE (and the player told, the crash log written if it can be) when a guard noted a prevented crash for this load.
Bool reportPreventedLoadCrash( const AsciiString &filename );

// GXCrashLog_Write, set by the ZH main loop once CrashLogs/ exists. A pointer, not a call: this file is
// also compiled into Generals, whose main has no GXCrashLog. Null = no crash log is written.
extern bool (*TheGXCrashLogWriter)( const char *reason, void *const *frames, int n );

// TRUE when the current match is LAN or online, whose saves gateSaveLoad refuses. The player has
// then been told why and the caller must not save.
Bool refuseNetworkMatchSave();
