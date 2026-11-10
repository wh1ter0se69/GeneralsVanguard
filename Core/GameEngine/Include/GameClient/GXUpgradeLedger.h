/*
**	GeneralsX: which upgrade completions have already been announced.
**
**	WHY THIS IS A HEADER OF ITS OWN, AND PURE.
**
**	The same reason as GXOnlineGate.h next door: the belief it replaces was WRONG in a way that no
**	compile and no four-platform verify could ever see, and the only way a wrong belief does not
**	come back silently is if it is testable without an engine. scripts/test/upgrade-dedupe-test.cpp
**	drives this class with `c++ -std=c++17 -I Core/GameEngine/Include` - no window, no game data,
**	no replay file.
**
**	WHAT WAS WRONG. InGameUI::announceUpgrade carried a comment asserting that "an upgrade completes
**	once per player by construction - Player::onUpgradeCompleted is called from the single place
**	that sets the mask bit". It is not one place and it is not once. Player::addUpgrade sets the
**	completed mask and calls onUpgradeCompleted with NO already-complete guard, and three call sites
**	reach it with UPGRADE_STATUS_COMPLETE: ProductionUpdate.cpp (research finished at a building)
**	and GrantUpgradeCreate.cpp twice (an object granting an upgrade as it is built). The grant path
**	is deduped only per MODULE INSTANCE - that is, per object - so an upgrade granted by three
**	different buildings runs onUpgradeCompleted three times and announces three times.
**
**	WHY A BITMASK AND NOT A RING. The sibling m_announcedSkills is a 16-entry ring with a
**	two-second window, and that shape is wrong here twice over. The repeats are MINUTES apart -
**	Upgrade_Nationalism is granted by the Infantry General's barracks, war factory and command
**	centers, which are built across a whole match - so no window short enough to be safe would
**	catch them; and a 16-entry ring shared by up to MAX_PLAYER_COUNT players evicts the first
**	Nationalism entry long before the third building goes up. One bit per allowlisted upgrade per
**	player is exact, and it is smaller than the ring it replaces.
**
**	It takes no engine types on purpose. The caller maps its own player index and its own allowlist
**	position onto the two ints below, and static_asserts its own bounds against the two limits.
*/

#pragma once

#ifndef GX_UPGRADE_LEDGER_H
#define GX_UPGRADE_LEDGER_H

namespace GXUpgradeLedger
{

/// Mirrors MAX_PLAYER_COUNT. Duplicated rather than included so a standalone test needs no engine;
/// InGameUI.cpp static_asserts the two against each other, so a change to either is a COMPILE
/// ERROR rather than a silently truncated ledger.
enum { MAX_PLAYERS = 40 };	// GeneralsX 06/11/2026 16 -> 40 with MAX_PLAYER_COUNT

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
