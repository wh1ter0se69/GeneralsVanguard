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

// Sense layer of the AI rule bank: the only way a rule reads other players objects. BRUTAL sees the whole map.
// Use partition iteration only, never an object-list walk: getShroudedStatus answers CLEAR for hidden cargo.
// Fair viewers see what their own units see, minus undetected stealth; a disguise counts as its disguise.
// Values are integers (build cost); no RNG, no state, no allocation the simulation can see.
#pragma once

#include <string>
#include <vector>
#include "Common/GameType.h"
#include "Lib/BaseType.h"	// Coord3D (rb-sense-2: Seen::pos)

class Player;
class Object;
class ThingTemplate;
namespace gxrb { struct Sighting; }

class GXRuleSense
{
public:
	GXRuleSense(Player *viewer, unsigned (*tierOf)(const Player *));
	/// build cost of the armed ENEMY objects the viewer sees within r of an OWN object: alive, on the map, seen (fog per tier,
	/// stealth), owner (as seen) an enemy of the viewer, template (as seen) able to carry a weapon and not a dozer or harvester.
	/// 0 for an anchor the viewer does not own.
	Int enemyValueNear(const Object *ownAnchor, Real r) const;
	/// SENSE.ENEMY_FACTION: ONE pass over every object the viewer fairly sees on the map - the filters
	/// of enemyValueNear (alive, on the map, fog per tier, stealth as seen from the lowest-id own object of the viewer) and identity as a human
	/// sees it. For every player index set in `track`, (*out)[index] gets {base side, template name} of each STRUCTURE / INFANTRY /
	/// VEHICLE / AIRCRAFT (no projectile) it owns AS SEEN whose template's side is a playable faction's. out holds one entry per player
	/// index. Returns the objects that reached the fog test (the pass's cost counter); 0 when the viewer owns nothing (it sees nothing).
	Int collectSightings(PlayerMaskType track, std::vector<std::vector<gxrb::Sighting> > *out) const;
	/// the base side (PlayerTemplate BaseSide: USA, China, GLA) of a template's own side - its FINAL override's (a map.ini may change a
	/// Side) - when that side is a playable faction, else ""
	std::string baseSideOf(const ThingTemplate *t) const;
	/// the template the sighting pass records for t (its final override) when it is a unit or building, else nullptr
	static const ThingTemplate *sightable(const ThingTemplate *t);
	/// baseSideOf(sightable(t)): "" for anything the pass never records - the save check's catalog
	std::string sightBaseOf(const ThingTemplate *t) const;
	/// OWN SIGHT: mark in *mark (one byte
	/// per partition cell, row-major) every cell the CURRENT look of one of the viewer's own units or buildings reveals - an object it
	/// controls, alive, whose template is sightable() and whose look (Object::friend_getPartitionLastLook) reveals for it, marked as
	/// doShroudReveal touches cells (PartitionManager::markLookCells). A satellite ping, a parachute, a reveal marker, an ally's or a spied
	/// unit's look, and a reveal with no object (map, script) mark nothing. Returns the eyes counted. FAIR sight (enemyValueNear,
	/// collectSightings and the FOG_CONDITIONS snapshot) needs one of a candidate's touched cells marked AND clear; BRUTAL never asks.
	Int markOwnEyes(std::vector<UnsignedByte> *mark) const;
	/// SENSE.FOG_CONDITIONS: one object the viewer sees now, as a human in its seat sees it
	struct Seen
	{
		Int owner;					///< the owner's player index AS SEEN (a disguise shows its disguise's player)
		const ThingTemplate *tmpl;	///< the template AS SEEN, its final override (sightable())
		Coord3D pos;
		Int value;					///< its LIST price when armed and neither a dozer nor a harvester (countsAsThreat), else 0
	};
	/// every unit and building the viewer sees NOW whose owner AS SEEN is player `ownerIndex` (never the viewer itself) - the filters of
	/// collectSightings (own sight and stealth for a FAIR viewer; BRUTAL: the whole map, stealth kept) and identity as seen. Computed at
	/// every call from the world at that moment - no snapshot is kept, so no answer depends on when or by whom it was
	/// first asked. Empty when the viewer owns nothing.
	std::vector<Seen> seenOf(Int ownerIndex) const;
#if defined(GX_TEST_INSTRUMENTS)
	/// fixtures only: the same value computed WRONGLY by walking each enemy player's object list with the fog test - the red arm that
	/// proves the cargo fixture can see the difference (never call it from a rule; the lint refuses it in rule files)
	Int enemyValueNearByListWalkForTest(const Object *ownAnchor, Real r) const;
	/// evidence for the judge only (rb-sense-2): per template name, the Euclidean distance from the enemy's nearest object of it to the
	/// viewer's nearest own unit or building, that eye's vision range and template - "tmpl:dist/range@eye,..." - independent of the cells
	std::string eyeEvidenceForTest(const Player *enemy, const std::vector<std::string> &tmpls) const;
#endif
private:
	Player *m_viewer;
	unsigned (*m_tierOf)(const Player *);
	mutable std::vector<signed char> m_baseIdx;	///< baseSideOf's cache by template id: -2 not looked up, -1 none, else m_bases[i]
	mutable std::vector<std::string> m_bases;
#if defined(GX_TEST_INSTRUMENTS)
	mutable std::vector<Seen> m_fc;		///< the frame_cache mutant's (red arm) per-frame cache - test builds only
	mutable UnsignedInt m_fcFrame = 0u;
	mutable Int m_fcOwner = -1;
	mutable Bool m_fcValid = FALSE;
public:
	mutable unsigned long long m_seenBuildsForTest = 0, m_seenObjsForTest = 0;	///< seenOf's computations and the objects they returned (ev=seen_cost)
#endif
};
