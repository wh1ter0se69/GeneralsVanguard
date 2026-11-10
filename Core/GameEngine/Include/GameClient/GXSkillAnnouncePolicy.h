/*
**	GeneralsX: which general's-skill PURCHASES are worth a replay banner.
**
**	WHY THIS IS A HEADER OF ITS OWN, AND PURE.
**
**	Same reason as GXUpgradeLedger.h and GXOnlineGate.h next door: the belief it replaces was WRONG
**	in a way no compile and no four-platform verify could see, and a wrong belief only stays dead if
**	it is testable without an engine. scripts/test/skill-purchase-banner-test.cpp drives this class
**	with `c++ -std=c++17 -I Core/GameEngine/Include` - no window, no game data, no replay file.
**
**	WHAT WAS WRONG. InGameUI::announceGeneralSkill grew a blanket `if( !used ) return;` so that the
**	"X purchased Y" line would stop duplicating the "X used Y" line that follows it seconds later.
**	For an ACTIVATABLE skill that is exactly right. For a skill with no activation it is a
**	regression, because the purchase is the ONLY event that science will ever generate:
**
**	  * SpecialPowerModule::triggerSpecialPower announces used=TRUE, and only for a power whose
**	    template sets RequiredScience (SpecialPowerModule.cpp:496). Nothing else ever passes TRUE.
**	  * A general's point spent on a passive or unit-unlock science - Paladin Tank, Pathfinder,
**	    Marauder Tank, every rank-2 and rank-3 tier - never reaches triggerSpecialPower at all.
**	  * So the blanket return made those point spends completely invisible in a replay, and
**	    measured against the shipped INIZH.big that is 45 of the 78 purchasable sciences.
**
**	THE PREDICATE, and why it is exact rather than a heuristic. A use banner can only ever name a
**	science that some SpecialPowerTemplate in TheSpecialPowerStore returns from getRequiredScience()
**	- that value IS the argument triggerSpecialPower passes. So the set of sciences that will get a
**	use banner is exactly
**
**	    { t->getRequiredScience() : t in TheSpecialPowerStore, != SCIENCE_INVALID }
**
**	and "will this purchase be followed by a use banner" is set membership, not an estimate. That
**	makes the store walk the definition of the question rather than the cheapest approximation to
**	it, which is why no cheaper signal was hunted for: a cheaper one could only be a proxy.
**
**	WHY A CACHE. The walk is O(number of special power templates) - 79 in the shipped data - and a
**	purchase announcement is rare, so the naive version would be affordable. It is cached anyway
**	because the answer cannot change while a game is running: the store is populated at INI parse
**	time and only SpecialPowerStore::reset() disturbs it. InGameUI::reset() therefore clears this,
**	for the same load-bearing reason GXUpgradeLedger::clear() is called there - a second replay
**	watched in the same process must not inherit the first one's map overrides.
**
**	FAILURE DIRECTION. An unbuilt or empty set answers "no use banner is coming", so every purchase
**	announces. That degrades toward the pre-regression behaviour of two banners, never toward the
**	regression of silence. A store that is genuinely empty also cannot fire a power, so for that
**	case the answer is not merely safe, it is correct.
**
**	It takes no engine types on purpose. The caller passes ScienceType through as an int and
**	static_asserts SCIENCE_INVALID against the mirror below, so a change to either is a COMPILE
**	ERROR rather than a silently mis-classified science.
*/

#pragma once

#ifndef GX_SKILL_ANNOUNCE_POLICY_H
#define GX_SKILL_ANNOUNCE_POLICY_H

#include <vector>

namespace GXSkillAnnouncePolicy
{

/// Mirrors SCIENCE_INVALID (Common/Science.h). Duplicated rather than included so a standalone test
/// needs no engine; InGameUI.cpp static_asserts the two against each other.
enum { INVALID_SCIENCE = -1 };

/*
 * The set of sciences that some special power template requires - i.e. the sciences a "used"
 * banner can name. Built once per game from TheSpecialPowerStore, consulted per purchase.
 */
class UseAnnouncingSciences
{
public:

	UseAnnouncingSciences() : m_built(false) {}

	/*
	 * Forget everything, including the built flag. Called from InGameUI::reset(). Not tidy-up: the
	 * store's contents change when a map's INI overrides are loaded and dropped, so a set carried
	 * from the previous game would classify this game's sciences against the previous game's data.
	 */
	void clear()
	{
		m_sciences.clear();
		m_built = false;
	}

	/// FALSE until markBuilt(), so the caller knows to walk the store. A walk that finds no store
	/// must NOT mark, so that the next announcement retries.
	bool isBuilt() const { return m_built; }

	void markBuilt() { m_built = true; }

	/*
	 * Record one special power template's RequiredScience. SCIENCE_INVALID means the power is not
	 * gated on a science at all - the overwhelming majority, every ordinary unit ability - and
	 * SpecialPowerModule's own gate refuses to announce those, so they contribute nothing here.
	 * Duplicates are dropped: three different powers naming SCIENCE_DaisyCutter is one science.
	 */
	void noteRequiredScience( int science )
	{
		if (science == INVALID_SCIENCE)
			return;
		for (size_t i = 0; i < m_sciences.size(); ++i)
			if (m_sciences[i] == science)
				return;
		m_sciences.push_back(science);
	}

	/// TRUE if a "used" banner can name this science, i.e. if some special power requires it.
	bool announcesOnUse( int science ) const
	{
		if (science == INVALID_SCIENCE)
			return false;
		for (size_t i = 0; i < m_sciences.size(); ++i)
			if (m_sciences[i] == science)
				return true;
		return false;
	}

	/// For tests and diagnostics only.
	size_t count() const { return m_sciences.size(); }

private:

	std::vector<int> m_sciences;
	bool m_built;
};

/*
 * THE DECISION, in one place so the test drives the same expression the engine does.
 *
 * A use event announces unconditionally - that path was never the defect and is not touched here.
 * A purchase announces only when no use event can follow it, which is precisely when no special
 * power requires the science.
 */
inline bool shouldAnnounce( const UseAnnouncingSciences &useAnnouncing, int science, bool used )
{
	if (science == INVALID_SCIENCE)
		return false;
	if (used)
		return true;
	return !useAnnouncing.announcesOnUse( science );
}

} // namespace GXSkillAnnouncePolicy

#endif // GX_SKILL_ANNOUNCE_POLICY_H
