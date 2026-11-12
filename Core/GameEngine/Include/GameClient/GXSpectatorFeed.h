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
**	Who may see the opponent feed (skill/upgrade banners, observer intel columns): replay playback, or a local player
**	who is NOT ACTIVE (!Player::isPlayerActive(): an observer seat or a defeated player).
**	Playback is its own clause: a replay may fall back to an ACTIVE local player. No local player outside playback: NO.
**	Only decides what this client DRAWS, never read by the simulation. Pure: scripts/test/spectator-feed-gate-test.cpp.
*/

#pragma once

#ifndef GX_SPECTATOR_FEED_H
#define GX_SPECTATOR_FEED_H

namespace GXSpectatorFeed
{

inline bool viewerMaySeeFeed( bool isPlayback, bool hasLocalPlayer, bool localPlayerIsActive )
{
	if (isPlayback)
		return true;
	return hasLocalPlayer && !localPlayerIsActive;
}

/*
 * The engine glue, here rather than in InGameUI.cpp so the test drives it too: it
 * is the one place a dropped or doubled `!` would turn the gate inside out while the predicate above stayed green.
 * PlayerT is the engine's Player in InGameUI.cpp and a stand-in in the test; all it needs is isPlayerActive().
 */
template <class PlayerT>
inline bool viewerMaySeeFeedFor( bool isPlayback, const PlayerT *localPlayer )
{
	return viewerMaySeeFeed( isPlayback, localPlayer != nullptr, localPlayer != nullptr && localPlayer->isPlayerActive() );
}

} // namespace GXSpectatorFeed

#endif // GX_SPECTATOR_FEED_H
