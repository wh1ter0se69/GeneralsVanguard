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
**	Which general's-skill PURCHASES get a replay banner; pure (scripts/test/skill-purchase-banner-test.cpp).
**	A use banner exists only for a science some SpecialPowerTemplate returns from getRequiredScience(); a passive or
**	unit-unlock science is never "used", so its purchase is its only event. An empty set announces every purchase.
**	InGameUI::reset() clears the cached set. The caller static_asserts SCIENCE_INVALID against the mirror below.
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
