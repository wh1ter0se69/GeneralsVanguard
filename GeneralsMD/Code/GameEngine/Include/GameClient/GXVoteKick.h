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

// IN-MATCH VOTE KICK - the client's half: the banner, its keys and clicks, the lines in
// the message list, the kicked client leaving, and the two calls the Tab board's KICK button makes.
//
// Everything here READS the simulation's vote (GameLogic::getGxVoteKick, rules in Common/GXVoteKick.h) and SENDS orders
// (MSG_GX_VOTEKICK_START / _CAST); nothing here decides anything. Defined in GXVoteKickHud.inl, included at the end of
// InGameUI.cpp for the reason GXSpectatorHud.inl is (a new .cpp would have to be named in a digested CMakeLists.txt).
#pragma once

#include "Common/UnicodeString.h"

class GameMessageTranslator;

/// May the local player start a vote to kick @p playerIndex (Player::getPlayerIndex) right now? FALSE + a short reason for
/// the button's tip: "A vote is already running", "You can start another vote in 3:12", "Needs at least 3 players", ...
/// The simulation checks the same rules again when the order executes; this only keeps a dead button from being offered.
Bool gxVoteKickCanStart( Int playerIndex, UnicodeString *whyNot );

/// Start that vote (appends MSG_GX_VOTEKICK_START). Does nothing when gxVoteKickCanStart says no.
void gxVoteKickStart( Int playerIndex );

/// Once per client frame, from GameClient::update (headless peers included): the lines and the GX_TEST_VOTEKICK instrument
/// in instrumented builds. TRUE once, 5 s after this client was kicked by a vote: the caller leaves the match then, through
/// gxKickedExit (GXKickPump.h), the moderator kick's own exit.
Bool gxVoteKickPump();

/// Clicks and taps on the banner: YES, NO, HIDE, and every other click on it swallowed so none becomes an order to the units
/// under it. Attached BEFORE the WindowTranslator, which eats every click while a map script disables input - a ballot must
/// still get through then; the banner steps aside under the quit menu so no menu click is taken. No keys.
GameMessageTranslator *gxNewVoteKickTranslator();
