/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @feature Claude 25/09/2026 WHERE THE FRIENDS KEY BELONGS TO FRIENDS.
//
// Owner decision, 25/09/2026, verbatim: "F5 = Friends ONLINE only; offline (skirmish/campaign) keeps camera
// view 5." So the Friends key (MetaEvent.cpp, F5 unless the launcher chose another) is caught only while a
// relay session is live; everywhere else it falls through to the meta map untouched, and a bare F5 in a
// skirmish, a campaign mission or a replay recalls View 5 exactly as it did before the Friends key existed.
//
// "ONLINE" IS A LIVE RELAY SESSION, measured from the transport rather than from which menu was used:
//   - shell:  the relay lobby screen, or a game room on a relaying transport (TheLAN is in a game). A
//             relaying TheLAN is NOT deleted when the player backs out to the main menu, so "the
//             transport is relaying" alone would hand F5 to Friends in the skirmish setup screen too.
//   - match:  a multiplayer game mode, with TheNetwork up, over a relaying transport - the same test
//             GXPerfReport's relayMatchTransport() uses. Skirmish, campaign and replay are other game
//             modes, so a relay transport left over from an earlier session cannot claim the key there;
//             a plain LAN match (no relay) keeps View 5 too.
//
// Pure and dependency-free so scripts/test/friends-key-online-test.cpp can drive every context; the
// caller (GXFriendsKeyHere, WOLLobbyMenu.cpp) only gathers the facts.
#pragma once

inline bool GXFriendsKeyOnline(bool shellActive, bool relayLobby, bool relaying, bool inRelayGameRoom,
	bool multiplayerMode, bool networkUp)
{
	if (shellActive)
		return relayLobby || (relaying && inRelayGameRoom);
	return multiplayerMode && networkUp && relaying;
}

// GeneralsX @bugfix Claude 25/09/2026 PRESENCE HAS A SHELF LIFE (codex-B P2 on 7a6121659).
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
