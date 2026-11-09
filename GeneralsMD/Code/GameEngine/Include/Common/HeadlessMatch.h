/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX @feature Claude 28/07/2026 Headless LAN host/join driver.
//
// Drives the lobby lifecycle through TheLAN with no UI, so a match can be hosted and joined
// from the command line. This is what makes cross-platform determinism soak-testable without
// two humans clicking through menus, and it is the same primitive the relay server needs:
// RequestGameJoinDirectConnect finds a peer by explicit IP rather than UDP broadcast, so it
// is not confined to one LAN.
//
// Modelled on ReplaySimulation, which is the proven headless entry point: GameMain branches
// to it INSTEAD of TheGameEngine->execute().

#pragma once

#include "Common/AsciiString.h"
#include "Common/GlobalData.h"	// LanRole, LanAiSpec
#include "Common/UnicodeString.h"

#include <vector>

class HeadlessMatch
{
public:

	/// Entry point called from GameMain when a headless LAN role was requested.
	/// Returns a process exit code: 0 on a clean run, non-zero on any failure.
	static int run();

	/// True when the command line asked for a headless host or join.
	static Bool isRequested();

private:

	static Bool setUpLan();
	static Bool driveLobby();
	static Bool runMatch();

	/// Builds the host's slot list (local player + AI fills) and pushes it to the peers.
	static Bool publishGameOptions();

	/// Pumps TheLAN until predicate() is true or timeoutMs elapses.
	/// Returns FALSE on timeout. This is the only thing pumping TheLAN in a headless
	/// process - nothing in the engine does it outside the lobby UI.
	template <typename Predicate>
	static Bool pumpUntil(Predicate predicate, UnsignedInt timeoutMs, const char *what);
};
