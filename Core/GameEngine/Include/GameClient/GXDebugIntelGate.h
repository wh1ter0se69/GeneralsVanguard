/*
**	GeneralsX: who may read OTHER players' state off the Debug menu.
**
**	THE HOLE. A release build opens the Debug menu when Options.ini says `DebugMenu = yes`
**	(DebugFloatButton.cpp debugFloatWanted). Its Players tab listed every player's cash, income, power, unit and
**	building counts and real general, and its Force radar button gave the local player a minimap he never built - both
**	behind debugClientToolsAllowed(), which asked only "is a match running". So any player in a live LAN or online
**	match could read every opponent's economy. The Players tab's own comment claimed InGameUI::drawPlayerInfoList
**	already shows every player's money to everyone; it does not - InGameUI draws that table only behind
**	TheControlBar->isObserverControlBarOn(), the bar ControlBar::setControlBarSchemeByPlayer hands a player who is NOT
**	ACTIVE.
**
**	THE PREDICATE: the menu may show other players' state when ANY of these holds -
**	  * the spectator feed's own gate (GXSpectatorFeed::viewerMaySeeFeed): replay playback, an observer seat, or a
**	    defeated player. Those viewers already hold the observer table and a forced radar; see GXSpectatorFeed.h for
**	    why a defeated player is included.
**	  * it is not a network game: single player, skirmish, campaign, Generals Challenge, the shell map.
**	  * it is a network game with no second human in it (GameLogic::gxCheatsAllowed, observers counted): the one-human
**	    harness matches and a player alone against AI. There is nobody to gain on, and the synced Reveal cheat is
**	    already open to that player, which shows more than this menu does.
**	A player who is still playing a network match against another human gets NO. Without a local player that is a
**	no as well, unless it is a replay or not a network game. Fail closed.
**
**	"Network game" is TheNetwork up OR a LAN / internet game mode, so either signal alone refuses.
**
**	DETERMINISM. Every input is recorder mode, a network/mode flag, or simulation state read without writing. The
**	answer only decides what this client DRAWS and whether it forces its OWN radar (display-only: Radar::crc is empty,
**	TheRadar is not in GameLogic::getCRC). Nothing reads it back into the simulation.
**
**	Pure and engine-free on purpose, like GXSpectatorFeed.h: scripts/test/debugmenu-intel-gate-test.cpp drives it with
**	`c++ -std=c++17 -I Core/GameEngine/Include`, and reads DebugMenu.cpp to prove every reader asks it.
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
