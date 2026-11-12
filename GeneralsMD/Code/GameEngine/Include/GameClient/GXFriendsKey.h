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

// The Friends key is caught only in a live relay session; elsewhere it falls through to the meta map (F5 = view 5).
// Shell: the relay lobby, or a game room on a relaying transport (a relaying TheLAN survives backing out to the menu).
// Match: multiplayer mode, TheNetwork up, relaying transport; skirmish, campaign, replay and plain LAN do not count.
// Pure and dependency-free so scripts/test/friends-key-online-test.cpp can drive every context.
#pragma once

inline bool GXFriendsKeyOnline(bool shellActive, bool relayLobby, bool relaying, bool inRelayGameRoom,
	bool multiplayerMode, bool networkUp)
{
	if (shellActive)
		return relayLobby || (relaying && inRelayGameRoom);
	return multiplayerMode && networkUp && relaying;
}

// PRESENCE HAS A SHELF LIFE.
//
// Once F5 opens the Friends overlay in a game room or a match, a presence answer the lobby cached can be
// shown long after the lobby stopped renewing it - a friend who signed off 20 minutes ago read "Online".
// The overlay now polls while it is open outside the lobby (GXBuddyOverlayPump), but a lost answer, a
// relay that stops replying, or a transport that is gone would still leave the last one on screen. So an
// answer older than this reads as unknown ("checking..."), never as a fact. 2.5 poll intervals (30 s,
// WOLLobbyMenu.cpp relayUpdatePresence): one lost reply in the lobby does not flicker the column, two do.
// Unsigned subtraction, so timeGetTime()'s 49-day wrap is harmless.
static const unsigned int GX_PRESENCE_MAX_AGE_MS = 75000;

inline bool GXPresenceAnswerCurrent(unsigned int nowMs, unsigned int answeredMs)
{
	return nowMs - answeredMs <= GX_PRESENCE_MAX_AGE_MS;
}
