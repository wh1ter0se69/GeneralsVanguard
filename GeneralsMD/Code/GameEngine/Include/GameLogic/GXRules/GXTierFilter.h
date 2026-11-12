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

// AI rule bank tier filter: Hard, Sentient and Brutal share one script list; tier entries carry a GXT.<T>.<name> mark.
// A mark whose tiers include the player tier replaces the stock entry of that name in the same parent; others drop.
// A malformed mark is dropped for every player. A player with no tier keeps no mark, so Hard lists are exactly stock.
// Pure, no engine type; scripts/test/gx-rulebank-test.cpp tests it.
#pragma once

#include <string>
#include <vector>
#include "GXRuleSpec.h"

namespace gxrb
{

struct Mark
{
	bool marked;		///< the name starts with "GXT."
	unsigned tiers;		///< 0 = malformed (dropped for everyone)
	std::string base;	///< the stock name it stands for
};

inline Mark parseMark(const std::string &name)
{
	Mark m = { false, 0u, name };
	if (name.compare(0, 4, "GXT.") != 0)
		return m;
	m.marked = true;
	const size_t dot = name.find('.', 4);
	if (dot == std::string::npos)
	{
		m.base.clear();
		return m;
	}
	const std::string tok = name.substr(4, dot - 4);
	m.base = name.substr(dot + 1);
	if (tok == "SENTIENT")
		m.tiers = TIER_SENTIENT;
	else if (tok == "BRUTAL")
		m.tiers = TIER_BRUTAL;
	else if (tok == "SENTIENT+BRUTAL")
		m.tiers = TIER_SENTIENT | TIER_BRUTAL;
	if (m.base.empty() || m.base.compare(0, 4, "GXT.") == 0)
		m.tiers = 0u;	// no name, or a mark inside a mark: malformed
	return m;
}

/// any marked name at all (the copy sites take the stock path untouched when there is none)
inline bool anyMarked(const std::vector<std::string> &names)
{
	for (size_t i = 0; i < names.size(); ++i)
		if (names[i].compare(0, 4, "GXT.") == 0)
			return true;
	return false;
}

struct PlanRow
{
	int src;			///< the input entry emitted here
	std::string name;	///< under this name
	int at;				///< the input position this row occupies: the stock twin's for a replacement, else src. Rows come in
						///< increasing `at`, so a container can be edited in place: entry at <- content of src, renamed; the rest deleted
};
struct PlanStats { int kept; int replaced; int dropped; int malformed; int dups; };

/// One parent's entries (names in order) -> what this player gets, in order. Unmarked entries stay, except the stock twin of a kept
/// mark, which the mark replaces in place (only the FIRST stock entry of that name). Pure and deterministic.
inline std::vector<PlanRow> tierPlan(const std::vector<std::string> &names, unsigned tier, PlanStats *stats)
{
	PlanStats st = { 0, 0, 0, 0, 0 };
	std::vector<PlanRow> out;
	std::vector<Mark> marks(names.size());
	std::vector<int> keptFor;		// per entry: for a stock entry, the index of the kept mark replacing it, else -1
	std::vector<bool> keep(names.size(), false);
	keptFor.assign(names.size(), -1);
	for (size_t i = 0; i < names.size(); ++i)
	{
		marks[i] = parseMark(names[i]);
		if (!marks[i].marked)
			continue;
		if (marks[i].tiers == 0u)
		{
			++st.malformed;
			continue;
		}
		if ((marks[i].tiers & tier) == 0u)
			continue;
		bool dup = false;
		for (size_t j = 0; j < i && !dup; ++j)
			dup = keep[j] && marks[j].base == marks[i].base;
		if (dup)
		{
			++st.dups;
			continue;
		}
		keep[i] = true;
	}
	// each kept mark replaces the first unmarked entry of its base name
	std::vector<bool> placed(names.size(), false);
	for (size_t i = 0; i < names.size(); ++i)
	{
		if (!keep[i])
			continue;
		for (size_t j = 0; j < names.size(); ++j)
			if (names[j] == marks[i].base)	// a kept mark's base never starts with GXT., so names[j] is unmarked
			{
				keptFor[j] = (int)i;
				placed[i] = true;
				break;
			}
	}
	for (size_t i = 0; i < names.size(); ++i)
	{
		if (!marks[i].marked)
		{
			if (keptFor[i] >= 0)
			{
				PlanRow r = { keptFor[i], names[i], (int)i };
				out.push_back(r);
				++st.replaced;
			}
			else
			{
				PlanRow r = { (int)i, names[i], (int)i };
				out.push_back(r);
			}
			continue;
		}
		if (!keep[i])
		{
			++st.dropped;
			continue;
		}
		++st.kept;
		if (!placed[i])
		{
			PlanRow r = { (int)i, marks[i].base, (int)i };
			out.push_back(r);
		}
	}
	if (stats != nullptr)
		*stats = st;
	return out;
}


/// The plan for a player's QUALIFIED copy: duplicateAndQualify appends the player's start index to every script
/// and group name, which would turn a malformed empty-name mark "GXT.SENTIENT." into "GXT.SENTIENT.3" (base "3"). So every name that ends
/// with the qualifier is judged WITHOUT it, and the qualifier is appended to the plan's output name again; a name that does not end with
/// it (never, for a copy) is judged and emitted as it is.
inline std::vector<PlanRow> tierPlanQualified(const std::vector<std::string> &names, const std::string &qualifier, unsigned tier,
	PlanStats *stats)
{
	std::vector<std::string> bare(names.size());
	std::vector<bool> had(names.size(), false);
	for (size_t i = 0; i < names.size(); ++i)
	{
		const std::string &n = names[i];
		had[i] = !qualifier.empty() && n.size() >= qualifier.size() && n.compare(n.size() - qualifier.size(), qualifier.size(), qualifier) == 0;
		bare[i] = had[i] ? n.substr(0, n.size() - qualifier.size()) : n;
	}
	std::vector<PlanRow> plan = tierPlan(bare, tier, stats);
	for (size_t r = 0; r < plan.size(); ++r)
		if (had[(size_t)plan[r].at])	// the name is the entry's at `at` (the stock twin's for a replacement)
			plan[r].name += qualifier;
	return plan;
}

}
