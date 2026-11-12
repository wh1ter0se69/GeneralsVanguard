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
**	Which upgrade completions have already been announced; pure (scripts/test/upgrade-dedupe-test.cpp).
**	Player::addUpgrade calls onUpgradeCompleted with no already-complete guard and the grant path dedupes per module
**	instance, so an upgrade granted by three buildings announced three times. A bitmask: repeats are MINUTES apart.
**	No engine types: the caller maps its indices onto the two ints and static_asserts its bounds against the limits.
*/

#pragma once

#ifndef GX_UPGRADE_LEDGER_H
#define GX_UPGRADE_LEDGER_H

namespace GXUpgradeLedger
{

/// Mirrors MAX_PLAYER_COUNT. Duplicated rather than included so a standalone test needs no engine;
/// InGameUI.cpp static_asserts the two against each other, so a change to either is a COMPILE
/// ERROR rather than a silently truncated ledger.
enum { MAX_PLAYERS = 40 };	// tracks MAX_PLAYER_COUNT

/// The width of the mask below, so it is a consequence rather than a free parameter. The caller
/// static_asserts its allowlist length against it.
enum { MAX_KEYS = 32 };

class Ledger
{
public:

	Ledger() { clear(); }

	/*
	 * Forget everything. Called from InGameUI::reset(), and that call is load-bearing rather than
	 * tidy-up: there is deliberately no time window here, so without a reset the SECOND replay
	 * watched in one process would have every one of its banners suppressed as a duplicate of the
	 * first.
	 */
	void clear()
	{
		for (int p = 0; p < MAX_PLAYERS; ++p)
			m_seen[p] = 0u;
	}

	/*
	 * Out-of-range inputs answer "not seen" and record nothing. A bad index therefore degrades to
	 * exactly today's behaviour - one extra banner - rather than to a read or write past the end of
	 * the array. Announcements are cosmetic; the array is not.
	 */
	bool alreadyAnnounced( int playerIndex, int keyIndex ) const
	{
		if (!inRange(playerIndex, keyIndex))
			return false;
		return (m_seen[playerIndex] & bitFor(keyIndex)) != 0u;
	}

	void record( int playerIndex, int keyIndex )
	{
		if (!inRange(playerIndex, keyIndex))
			return;
		m_seen[playerIndex] |= bitFor(keyIndex);
	}

private:

	static bool inRange( int playerIndex, int keyIndex )
	{
		return playerIndex >= 0 && playerIndex < MAX_PLAYERS
			&& keyIndex    >= 0 && keyIndex    < MAX_KEYS;
	}

	static unsigned int bitFor( int keyIndex ) { return 1u << keyIndex; }

	unsigned int m_seen[MAX_PLAYERS];
};

}	// namespace GXUpgradeLedger

#endif	// GX_UPGRADE_LEDGER_H
