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

/*
**	Who may read OTHER players' state off the Debug menu (DebugMenu = yes). It may when ANY holds: the spectator
**	feed's gate (replay, observer seat, defeated player); it is not a network game; or it is a network game with
**	no second human (GameLogic::gxCheatsAllowed). Otherwise NO, including no local player outside a replay: fail
**	closed. Display-only, never read back into the simulation. Pure: scripts/test/debugmenu-intel-gate-test.cpp.
*/

#pragma once

#ifndef GX_DEBUG_INTEL_GATE_H
#define GX_DEBUG_INTEL_GATE_H

#include "GameClient/GXSpectatorFeed.h"

namespace GXDebugIntel
{

inline bool mayShowOthers( bool isPlayback, bool isNetworkGame, bool isSandbox, bool hasLocalPlayer, bool localPlayerIsActive )
{
	if (GXSpectatorFeed::viewerMaySeeFeed( isPlayback, hasLocalPlayer, localPlayerIsActive ))
		return true;
	if (!isNetworkGame)
		return true;
	return isSandbox && hasLocalPlayer;
}

/*
 * The engine glue, here so the test drives it too: PlayerT is the engine's Player in DebugMenu.cpp and a stand-in in the
 * test; all it needs is isPlayerActive().
 */
template <class PlayerT>
inline bool mayShowOthersFor( bool isPlayback, bool isNetworkGame, bool isSandbox, const PlayerT *localPlayer )
{
	return mayShowOthers( isPlayback, isNetworkGame, isSandbox, localPlayer != nullptr,
		localPlayer != nullptr && localPlayer->isPlayerActive() );
}

} // namespace GXDebugIntel

#endif // GX_DEBUG_INTEL_GATE_H
