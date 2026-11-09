// GeneralsX @feature Claude 30/10/2026 AI RULE BANK sense layer - see GXRuleSense.h for the rules it keeps (partition only, fog per tier,
// stealth, identity as a human would see it). Read only: no order, no random draw, no state.
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/GXRules/GXRuleSense.h"
#include "GameLogic/GXRules/GXRuleCore.h"

GXRuleSense::GXRuleSense(Player *viewer, unsigned (*tierOf)(const Player *)) : m_viewer(viewer), m_tierOf(tierOf)
{
}

namespace
{
/// test-instruments builds only: GX_TEST_SENSE_MUTANT=pre_r1 restores the sense layer before Codex rbs2-r1's fixes - the owner's
/// discounted price, the stock TARGETING stealth filter, the fog filter beside the eyes, the real controller of a hiding garrison - the
/// red arm of the seen_value discount, seen_count disguise_neutral and own_sight edge subs. Always FALSE in a release build.
Bool gxSensePreR1ForTest()
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool on = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "pre_r1") == 0;
	return on;
#else
	return FALSE;
#endif
}
/// what the viewer would see of o: its owner and template as a human in the viewer's seat sees them (InGameUI.cpp's disguise rule)
struct GXSeen
{
	Player *owner;
	const ThingTemplate *tmpl;
};
GXSeen gxSeen(Object *o, Player *viewer)
{
	GXSeen s = { o->getControllingPlayer(), o->getTemplate() };
	if (s.owner == nullptr)
		return s;
	// a garrison that hides itself from non-allies (GarrisonContain::getApparentControllingPlayer - a stealth garrison in a civilian
	// building) shows the building's ORIGINAL owner to them, as InGameUI shows it (Codex rbs2-r1 P1)
	const ContainModuleInterface *contain = o->getContain();
	const Player *apparent = (contain != nullptr && !gxSensePreR1ForTest()) ? contain->getApparentControllingPlayer(viewer) : nullptr;
	if (apparent != nullptr)
		s.owner = const_cast<Player *>(apparent);
	StealthUpdate *su = o->getStealth();
	if (su != nullptr && su->isDisguised())
	{
		const Relationship toViewer = s.owner->getRelationship(viewer->getDefaultTeam());
		const Int owner = gxrb::seenOwner(s.owner->getPlayerIndex(), TRUE, su->getDisguisedPlayerIndex(), (Int)toViewer);
		if (gxrb::seenAsDisguise(TRUE, su->getDisguisedPlayerIndex(), (Int)toViewer))
		{
			Player *p = ThePlayerList->getNthPlayer(owner);
			if (p != nullptr)
				s.owner = p;
			if (su->getDisguisedTemplate() != nullptr)
				s.tmpl = su->getDisguisedTemplate();
		}
	}
	return s;
}
/// what a human knows a seen template costs: its LIST price (the final override's BuildCost) - never calcCostToBuild(owner), which
/// applies the owner's hidden production discounts and handicap (an unseen Oil Refinery makes every unit 10% cheaper; Codex rbs2-r1 P1)
Int gxListPrice(const ThingTemplate *t, const Player *owner = nullptr)
{
	if (t != nullptr && owner != nullptr && gxSensePreR1ForTest())
		return t->calcCostToBuild(owner);
	return t != nullptr ? static_cast<const ThingTemplate *>(t->getFinalOverride())->friend_getBuildCost() : 0;
}
/// what is hidden from a viewer by stealth: a stealthed, undetected object - except a disguised disguiser, which every viewer sees AS ITS
/// DISGUISE (gxSeen) whoever it imitates. The stock PartitionFilterStealthedAndUndetected is a TARGETING rule (a disguise as a friend
/// counts as stealthed; a neutral building holding undetected stealth units counts as stealthed) and so undercounted what a human sees
/// (Codex rbs2-r1 P2 + the P1 garrison case: the building stays seen, owned as gxSeen shows it)
class GXSightStealthFilter : public PartitionFilter
{
public:
	virtual Bool allow(Object *o) override
	{
		if (!o->testStatus(OBJECT_STATUS_STEALTHED) || o->testStatus(OBJECT_STATUS_DETECTED))
			return TRUE;
		StealthUpdate *su = o->getStealth();
		return o->isKindOf(KINDOF_DISGUISER) && su != nullptr && su->isDisguised();
	}
#if defined(RTS_DEBUG)
	virtual const char *debugGetName() override { return "GXSightStealthFilter"; }
#endif
};
/// the value of one seen object to the viewer: its list price if it is an armed enemy that is not a dozer or harvester, else 0
Int gxSeenValue(Object *o, Player *viewer, const Object *anchor)
{
	if (o == anchor || o->isEffectivelyDead() || o->isDestroyed())
		return 0;	// a unit a script deleted this frame is still in the partition (Codex sell-engine-r3 #1)
	const GXSeen s = gxSeen(o, viewer);
	if (s.owner == nullptr || s.tmpl == nullptr || s.owner == viewer)
		return 0;
	if (viewer->getRelationship(s.owner->getDefaultTeam()) != ENEMIES)
		return 0;
	if (!gxrb::countsAsThreat(s.tmpl->canPossiblyHaveAnyWeapon() != FALSE, s.tmpl->isKindOf(KINDOF_DOZER) != FALSE,
			s.tmpl->isKindOf(KINDOF_HARVESTER) != FALSE))
		return 0;
	return gxListPrice(s.tmpl, s.owner);
}
}

namespace
{
/// test-instruments builds only: GX_TEST_SENSE_MUTANT=no_owner_guard drops the own-anchor guard - the BROKEN sense layer the filters
/// fixture's foreign_anchor case must catch (Codex core-r4-lensB P1). Always FALSE in a release build.
Bool gxSenseNoOwnerGuardForTest()
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool on = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "no_owner_guard") == 0;
	return on;
#else
	return FALSE;
#endif
}
/// test-instruments builds only: GX_TEST_SENSE_MUTANT=brutal_no_stealth drops the stealth filter from BRUTAL's whole-map view - the
/// BROKEN sense layer fx-filters-brutal-nostealth must catch (Codex core-r6-lensB P2: Brutal drops ONLY the fog filter, D2)
Bool gxSenseBrutalNoStealthForTest()
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool on = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "brutal_no_stealth") == 0;
	return on;
#else
	return FALSE;
#endif
}
/// test-instruments builds only: GX_TEST_SENSE_MUTANT=any_reveal drops the own-eyes test - the sense layer before rb-sense-2, which any
/// reveal fed (a granted one, a satellite ping, an ally's unit); the own_sight fixture must FAIL on it. Always FALSE in a release build.
Bool gxSenseAnyRevealForTest()
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool on = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "any_reveal") == 0;
	return on;
#else
	return FALSE;
#endif
}

/// test-instruments builds only: GX_TEST_SENSE_MUTANT=no_self_reveal drops gxShowsItselfTo - the sense layer before the lead's ruling, where an
/// enemy superweapon counted only under an own eye; the seen_self_reveal fixture must FAIL on it. Always FALSE in a release build.
Bool gxSenseNoSelfRevealForTest()
{
#if defined(GX_TEST_INSTRUMENTS)
	static const Bool on = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "no_self_reveal") == 0;
	return on;
#else
	return FALSE;
#endif
}

/// an object that shows ITSELF to the viewer wherever the viewer's units are: its current ShroudRevealToAllRange look (a superweapon, Object::look:
/// finished, and not stealthed unless detected) names the viewer. If the game shows the player, the player knows (lead's ruling 02/11/2026,
/// rb-sense-3). This object only: what stands beside it, in the same small disc, still needs an own eye.
Bool gxShowsItselfTo(const Object *o, const Player *viewer)
{
	const SightingInfo *look = o->friend_getPartitionRevealAllLastLook();
	return !gxSenseNoSelfRevealForTest() && look != nullptr && !look->isInvalid() && (look->m_forWhom & viewer->getPlayerMask()) != 0;
}

/// one OWN eye of the viewer (rb-sense-2): an object it controls, alive, whose template is a unit or a building (GXRuleSense::sightable -
/// never a satellite ping, a parachute, a reveal marker or a projectile) and whose current look reveals for the viewer
struct GXEyeWalk
{
	const Player *viewer;
	std::vector<UnsignedByte> *mark;
	Int eyes;
};
void gxMarkEye(Object *o, void *userData)
{
	GXEyeWalk &w = *static_cast<GXEyeWalk *>(userData);
	if (o == nullptr || o->isDestroyed() || o->isEffectivelyDead() || o->getControllingPlayer() != w.viewer)
		return;
	if (GXRuleSense::sightable(o->getTemplate()) == nullptr)
		return;
	const SightingInfo *look = o->friend_getPartitionLastLook();
	if (look == nullptr || look->isInvalid() || (look->m_forWhom & w.viewer->getPlayerMask()) == 0)
		return;
	ThePartitionManager->markLookCells(*look, *w.mark);
	++w.eyes;
}

/// what the viewer's own eyes see (rb-sense-2): an object one of whose touched cells an own eye reveals and is CLEAR for the viewer, or one
/// that shows itself to the viewer (gxShowsItselfTo);
/// the eye grid is built (GXRuleSense::markOwnEyes) when the first candidate reaches this test, so a pass that has no candidate - every
/// other filter refused everything - pays nothing for it; the grid is the same whichever candidate comes first
class GXOwnEyesFilter : public PartitionFilter
{
public:
	GXOwnEyesFilter(const GXRuleSense *sense, const Player *viewer) : m_sense(sense), m_viewer(viewer), m_viewerIndex(viewer->getPlayerIndex()), m_built(FALSE) {}
	virtual Bool allow(Object *o) override
	{
		if (gxShowsItselfTo(o, m_viewer))
			return TRUE;
		if (!m_built)
		{
			m_sense->markOwnEyes(&m_eyes);
			m_built = TRUE;
		}
		const PartitionData *pd = o->friend_getConstPartitionData();
		return pd != nullptr && pd->touchesMarkedClearCell(m_viewerIndex, m_eyes);
	}
#if defined(RTS_DEBUG)
	virtual const char *debugGetName() override { return "GXOwnEyesFilter"; }
#endif
private:
	const GXRuleSense *m_sense;
	const Player *m_viewer;
	Int m_viewerIndex;
	Bool m_built;
	std::vector<UnsignedByte> m_eyes;
};
}

Int GXRuleSense::markOwnEyes(std::vector<UnsignedByte> *mark) const
{
	if (mark == nullptr || m_viewer == nullptr || ThePartitionManager == nullptr)
		return 0;
	const size_t n = (size_t)ThePartitionManager->getCellCountX() * (size_t)ThePartitionManager->getCellCountY();
	mark->assign(n, 0);
	GXEyeWalk w = { m_viewer, mark, 0 };
	m_viewer->iterateObjects(gxMarkEye, &w);
	return w.eyes;
}

Int GXRuleSense::enemyValueNear(const Object *ownAnchor, Real r) const
{
	if (m_viewer == nullptr || ownAnchor == nullptr || ThePartitionManager == nullptr)
		return 0;
	if (ownAnchor->getControllingPlayer() != m_viewer && !gxSenseNoOwnerGuardForTest())
		return 0;
	const Bool brutal = m_tierOf != nullptr && m_tierOf(m_viewer) == gxrb::TIER_BRUTAL;
	PartitionFilterAlive fAlive;
	PartitionFilterOnMap fMap;
	PartitionFilterFreeOfFog fFog(m_viewer->getPlayerIndex());
	GXSightStealthFilter fStealth;
	// own sight only (rb-sense-2): the viewer's own units and buildings, never a granted reveal, a satellite ping or an ally's eyes. The
	// eyes test REPLACES the fog filter: it needs a marked cell that is CLEAR, and also sees a building an eye shows only partly
	// (OBJECTSHROUD_PARTIAL_CLEAR, which PartitionFilterFreeOfFog refuses; Codex rbs2-r1 P2). The any_reveal mutant is the old path.
	GXOwnEyesFilter fEyes(this, m_viewer);
	PartitionFilterStealthedAndUndetected fStockStealth(ownAnchor, false);
	PartitionFilter *pre[] = { &fAlive, &fMap, &fFog, &fStockStealth, &fEyes, nullptr };	// the pre_r1 mutant
	PartitionFilter *fair[] = { &fAlive, &fMap, &fStealth, gxSenseAnyRevealForTest() ? static_cast<PartitionFilter *>(&fFog) : &fEyes, nullptr };
	PartitionFilter *whole[] = { &fAlive, &fMap, gxSenseBrutalNoStealthForTest() ? nullptr : &fStealth, nullptr };	// Brutal: only the fog filter goes (D2)
	SimpleObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(ownAnchor->getPosition(), r, FROM_BOUNDINGSPHERE_2D,
		brutal ? whole : (gxSensePreR1ForTest() ? pre : fair));
	MemoryPoolObjectHolder hold(iter);
	Int value = 0;
	for (Object *o = iter->first(); o != nullptr; o = iter->next())
		value += gxSeenValue(o, m_viewer, ownAnchor);
	return value;
}

// ---- SENSE.ENEMY_FACTION (GeneralsX 01/11/2026 rb-sense-1, gx-handoff ai/rulebank/core/NEXT_rb-sense-1.md) ----
/// what the sighting pass may record (one rule for the pass and the save check, Codex sense-r6 P1): a unit or building - STRUCTURE,
/// INFANTRY, VEHICLE or AIRCRAFT, never a PROJECTILE - judged on the template's FINAL override (StealthUpdate::xfer restores a disguise as the
/// template before its map.ini override; the uninterrupted game held the override)
const ThingTemplate *GXRuleSense::sightable(const ThingTemplate *t)
{
	if (t == nullptr)
		return nullptr;
	t = static_cast<const ThingTemplate *>(t->getFinalOverride());
	if (t->isKindOf(KINDOF_PROJECTILE) || !(t->isKindOf(KINDOF_STRUCTURE) || t->isKindOf(KINDOF_INFANTRY) || t->isKindOf(KINDOF_VEHICLE)
			|| t->isKindOf(KINDOF_AIRCRAFT)))
		return nullptr;
	return t;
}
std::string GXRuleSense::sightBaseOf(const ThingTemplate *t) const
{
	const ThingTemplate *s = sightable(t);
	return s != nullptr ? baseSideOf(s) : std::string();
}

namespace
{
void gxSenseLowestOwn(Object *o, void *userData)
{
	Object **best = static_cast<Object **>(userData);
	if (o != nullptr && !o->isDestroyed() && (*best == nullptr || o->getID() < (*best)->getID()))
		*best = o;
}
/// the fair-sighting test, cheapest first: a unit or building (as seen) of a tracked player (as seen), then enemyValueNear's own filters -
/// alive, on the map, fog for the viewer (none for BRUTAL), stealthed and undetected not seen. Only the objects that reach the fog test
/// pay for it (getShroudedStatus caches seen state per player).
class GXSightFilter : public PartitionFilter
{
public:
	GXSightFilter(Player *viewer, PlayerMaskType track, PartitionFilter *alive, PartitionFilter *onMap, PartitionFilter *fog,
		PartitionFilter *eyes, PartitionFilter *stealth) :
		m_viewer(viewer), m_track(track), m_alive(alive), m_onMap(onMap), m_fog(fog), m_eyes(eyes), m_stealth(stealth), m_fogTests(0) {}
	virtual Bool allow(Object *o) override
	{
		if (o->isDestroyed())
			return FALSE;	// a unit a script deleted this frame is still in the partition (gxSeenValue's guard)
		const GXSeen s = gxSeen(o, m_viewer);
		if (s.owner == nullptr || s.tmpl == nullptr || s.owner == m_viewer)
			return FALSE;
		const Int pi = s.owner->getPlayerIndex();
		if (pi < 0 || pi >= MAX_PLAYER_COUNT || (m_track & gxPlayerBit(pi)) == 0u)
			return FALSE;
		if (GXRuleSense::sightable(s.tmpl) == nullptr)
			return FALSE;
		if (!m_alive->allow(o) || !m_onMap->allow(o))
			return FALSE;
		++m_fogTests;
		if (m_fog != nullptr && !m_fog->allow(o))
			return FALSE;
		if (m_eyes != nullptr && !m_eyes->allow(o))
			return FALSE;
		return m_stealth == nullptr || m_stealth->allow(o);
	}
#if defined(RTS_DEBUG)
	virtual const char *debugGetName() override { return "GXSightFilter"; }
#endif
	Int fogTests() const { return m_fogTests; }
private:
	Player *m_viewer;
	PlayerMaskType m_track;
	PartitionFilter *m_alive, *m_onMap, *m_fog, *m_eyes, *m_stealth;
	Int m_fogTests;
};
}

std::string GXRuleSense::baseSideOf(const ThingTemplate *t) const
{
	if (t == nullptr || ThePlayerTemplateStore == nullptr)
		return std::string();
	// the FINAL override, whoever asks: an object is made from it (a map.ini can override a unit's Side) while findTemplate - the save
	// check's lookup by name - returns the template before it; both must classify the same thing (Codex sense-r5 P1)
	t = static_cast<const ThingTemplate *>(t->getFinalOverride());
	const size_t id = (size_t)t->getTemplateID();
	if (id >= m_baseIdx.size())
		m_baseIdx.resize(id + 1, (signed char)-2);
	if (m_baseIdx[id] == -2)
	{
		m_baseIdx[id] = -1;
		const AsciiString &side = t->getDefaultOwningSide();
		for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount() && side.isNotEmpty(); ++i)
		{
			const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
			if (pt == nullptr || !pt->isPlayableSide() || pt->getSide() != side)
				continue;
			const std::string base = pt->getBaseSide().isNotEmpty() ? pt->getBaseSide().str() : pt->getSide().str();
			size_t k = 0;
			while (k < m_bases.size() && m_bases[k] != base)
				++k;
			if (k == m_bases.size() && k < 127u)
				m_bases.push_back(base);
			if (k < m_bases.size())
				m_baseIdx[id] = (signed char)k;
			break;
		}
	}
	return m_baseIdx[id] >= 0 ? m_bases[(size_t)m_baseIdx[id]] : std::string();
}

Int GXRuleSense::collectSightings(PlayerMaskType track, std::vector<std::vector<gxrb::Sighting> > *out) const
{
	if (m_viewer == nullptr || ThePartitionManager == nullptr || out == nullptr || track == 0u)
		return 0;
	Object *anchor = nullptr;	// "our" object of the stealth filter: the disguise and garrison tests read its player and team
	m_viewer->iterateObjects(gxSenseLowestOwn, &anchor);
	if (anchor == nullptr)
		return 0;
	const Bool brutal = m_tierOf != nullptr && m_tierOf(m_viewer) == gxrb::TIER_BRUTAL;
	PartitionFilterAlive fAlive;
	PartitionFilterOnMap fMap;
	PartitionFilterFreeOfFog fFog(m_viewer->getPlayerIndex());
	GXSightStealthFilter fStealth;
	GXOwnEyesFilter fEyes(this, m_viewer);	// own sight only (rb-sense-2), in place of the fog filter (enemyValueNear's note); BRUTAL sees the whole map
	const Bool any = gxSenseAnyRevealForTest(), pre = gxSensePreR1ForTest();
	PartitionFilterStealthedAndUndetected fStockStealth(anchor, false);
	GXSightFilter f(m_viewer, track, &fAlive, &fMap, (!brutal && (any || pre)) ? &fFog : nullptr, (brutal || any) ? nullptr : &fEyes,
		(brutal && gxSenseBrutalNoStealthForTest()) ? nullptr : (pre ? static_cast<PartitionFilter *>(&fStockStealth) : &fStealth));
	PartitionFilter *filters[] = { &f, nullptr };
	SimpleObjectIterator *iter = ThePartitionManager->iterateAllObjects(filters);
	MemoryPoolObjectHolder hold(iter);
	for (Object *o = iter->first(); o != nullptr; o = iter->next())
	{
		const GXSeen s = gxSeen(o, m_viewer);
		const Int pi = s.owner->getPlayerIndex();
		if (pi < 0 || (size_t)pi >= out->size())
			continue;
		const ThingTemplate *t = sightable(s.tmpl);	// the final override, as the save check names it
		if (t == nullptr)
			continue;
		gxrb::Sighting g;
		g.base = baseSideOf(t);
		g.tmpl = t->getName().str();
		if (!g.base.empty())
			(*out)[(size_t)pi].push_back(g);
	}
	return f.fogTests();
}

// ---- SENSE.FOG_CONDITIONS (GeneralsX 02/11/2026 rb-sense-2, gx-handoff ai/rulebank/core/PLAN_rb-sense-2.md) ----
namespace
{
/// a unit or building of another player, as the viewer sees its owner and template (the snapshot's own cheap test; the partition filters
/// after it decide whether it is seen at all)
class GXSeenFilter : public PartitionFilter
{
public:
	GXSeenFilter(Player *viewer, Int ownerIndex) : m_viewer(viewer), m_owner(ownerIndex) {}
	virtual Bool allow(Object *o) override
	{
		if (o->isDestroyed())
			return FALSE;	// a unit a script deleted this frame is still in the partition (gxSeenValue's guard)
		const GXSeen s = gxSeen(o, m_viewer);
		return s.owner != nullptr && s.tmpl != nullptr && s.owner != m_viewer && s.owner->getPlayerIndex() == m_owner
			&& GXRuleSense::sightable(s.tmpl) != nullptr;
	}
#if defined(RTS_DEBUG)
	virtual const char *debugGetName() override { return "GXSeenFilter"; }
#endif
private:
	Player *m_viewer;
	Int m_owner;
};
}

// Computed at EVERY ask, never cached (Codex rbs2-r2 P1): a snapshot built at the first ask of a pass and reused depends on WHEN that
// ask came, and a client-local condition earlier in an AND (one naming <Local Player>, say) makes that differ between peers - one peer
// builds before a shared script deletes a unit, the other after, and their answers split. Computed per ask, an answer is a function of the
// world at that moment alone, the same on every peer, in a replay and after a load; no state, nothing to save.
std::vector<GXRuleSense::Seen> GXRuleSense::seenOf(Int ownerIndex) const
{
	std::vector<Seen> out;
#if defined(GX_TEST_INSTRUMENTS)
	// GX_TEST_SENSE_MUTANT=frame_cache: the BROKEN per-frame cache this replaced - the seen_stale fixture's red arm
	static const Bool frameCache = getenv("GX_TEST_SENSE_MUTANT") != nullptr && strcmp(getenv("GX_TEST_SENSE_MUTANT"), "frame_cache") == 0;
	const UnsignedInt nowFrame = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0u;
	if (frameCache && m_fcValid && m_fcFrame == nowFrame && m_fcOwner == ownerIndex)
		return m_fc;
#endif
	if (m_viewer == nullptr || ThePartitionManager == nullptr || ownerIndex < 0)
		return out;
	Object *anchor = nullptr;	// the viewer owns nothing: it sees nothing (and the pre_r1 mutant's stock stealth filter needs "our" object)
	m_viewer->iterateObjects(gxSenseLowestOwn, &anchor);
	if (anchor == nullptr)
		return out;
	const Bool brutal = m_tierOf != nullptr && m_tierOf(m_viewer) == gxrb::TIER_BRUTAL;
	GXSeenFilter fSeen(m_viewer, ownerIndex);
	PartitionFilterAlive fAlive;
	PartitionFilterOnMap fMap;
	PartitionFilterFreeOfFog fFog(m_viewer->getPlayerIndex());
	GXSightStealthFilter fStealth;
	GXOwnEyesFilter fEyes(this, m_viewer);	// own sight in place of the fog filter (enemyValueNear's note)
	PartitionFilterStealthedAndUndetected fStockStealth(anchor, false);
	PartitionFilter *pre[] = { &fSeen, &fAlive, &fMap, &fFog, &fStockStealth, &fEyes, nullptr };	// the pre_r1 mutant
	PartitionFilter *fair[] = { &fSeen, &fAlive, &fMap, &fStealth, gxSenseAnyRevealForTest() ? static_cast<PartitionFilter *>(&fFog) : &fEyes, nullptr };
	PartitionFilter *whole[] = { &fSeen, &fAlive, &fMap, gxSenseBrutalNoStealthForTest() ? nullptr : &fStealth, nullptr };
	SimpleObjectIterator *iter = ThePartitionManager->iterateAllObjects(brutal ? whole : (gxSensePreR1ForTest() ? pre : fair));
	MemoryPoolObjectHolder hold(iter);
	for (Object *o = iter->first(); o != nullptr; o = iter->next())
	{
		const GXSeen s = gxSeen(o, m_viewer);
		const ThingTemplate *t = sightable(s.tmpl);
		Seen e;
		e.owner = s.owner->getPlayerIndex();
		e.tmpl = t;
		e.pos = *o->getPosition();
		e.value = gxrb::countsAsThreat(t->canPossiblyHaveAnyWeapon() != FALSE, t->isKindOf(KINDOF_DOZER) != FALSE,
			t->isKindOf(KINDOF_HARVESTER) != FALSE) ? gxListPrice(t, s.owner) : 0;
		out.push_back(e);
	}
#if defined(GX_TEST_INSTRUMENTS)
	++m_seenBuildsForTest;
	m_seenObjsForTest += out.size();
	if (frameCache)
	{
		m_fc = out;
		m_fcValid = TRUE;
		m_fcFrame = nowFrame;
		m_fcOwner = ownerIndex;
	}
#endif
	return out;
}

#if defined(GX_TEST_INSTRUMENTS)
namespace
{
struct GXEyePos
{
	Coord3D pos;
	Real range;
	const char *tmpl;
};
void gxCollectEyePos(Object *o, void *userData)
{
	std::vector<GXEyePos> &v = *static_cast<std::vector<GXEyePos> *>(userData);
	if (o == nullptr || o->isDestroyed() || o->isEffectivelyDead() || GXRuleSense::sightable(o->getTemplate()) == nullptr)
		return;
	GXEyePos e = { *o->getPosition(), o->getShroudClearingRange(), o->getTemplate()->getName().str() };
	v.push_back(e);
}
struct GXEvidenceWalk
{
	const std::vector<GXEyePos> *eyes;
	std::string tmpl;
	Real best, range;
	const char *eyeTmpl;
};
void gxEvidenceOne(Object *o, void *userData)
{
	GXEvidenceWalk &w = *static_cast<GXEvidenceWalk *>(userData);
	if (o == nullptr || o->isDestroyed() || GXRuleSense::sightable(o->getTemplate()) == nullptr
		|| GXRuleSense::sightable(o->getTemplate())->getName().str() != w.tmpl)
		return;
	const Coord3D *p = o->getPosition();
	for (size_t k = 0; k < w.eyes->size(); ++k)
	{
		const Real dx = p->x - (*w.eyes)[k].pos.x, dy = p->y - (*w.eyes)[k].pos.y;
		const Real d = sqrtf(dx * dx + dy * dy);
		if (w.best < 0.0f || d < w.best)
		{
			w.best = d;
			w.range = (*w.eyes)[k].range;
			w.eyeTmpl = (*w.eyes)[k].tmpl;
		}
	}
}
}

std::string GXRuleSense::eyeEvidenceForTest(const Player *enemy, const std::vector<std::string> &tmpls) const
{
	if (m_viewer == nullptr || enemy == nullptr)
		return "-";
	std::vector<GXEyePos> eyes;
	m_viewer->iterateObjects(gxCollectEyePos, &eyes);
	std::string out;
	for (size_t i = 0; i < tmpls.size(); ++i)
	{
		GXEvidenceWalk w = { &eyes, tmpls[i], -1.0f, 0.0f, "-" };
		const_cast<Player *>(enemy)->iterateObjects(gxEvidenceOne, &w);
		char one[160];
		snprintf(one, sizeof(one), "%s%s:%d/%d@%s", i ? "," : "", tmpls[i].c_str(), (int)w.best, (int)w.range, w.eyeTmpl);
		out += one;
	}
	return out.empty() ? "-" : out;
}

namespace
{
struct GXWalk
{
	Player *viewer;
	const Object *anchor;
	Real r2;
	Int value;
	Bool brutal;
};
void gxWalkOne(Object *o, void *userData)
{
	GXWalk &w = *static_cast<GXWalk *>(userData);
	const Coord3D *a = w.anchor->getPosition();
	const Coord3D *p = o->getPosition();
	const Real dx = p->x - a->x, dy = p->y - a->y;
	if (dx * dx + dy * dy > w.r2)
		return;
	if (!w.brutal && o->getShroudedStatus(w.viewer->getPlayerIndex()) != OBJECTSHROUD_CLEAR)
		return;
	PartitionFilterStealthedAndUndetected st(w.anchor, false);
	if (!st.allow(o))
		return;
	w.value += gxSeenValue(o, w.viewer, w.anchor);
}
}

Int GXRuleSense::enemyValueNearByListWalkForTest(const Object *ownAnchor, Real r) const
{
	if (m_viewer == nullptr || ownAnchor == nullptr || ownAnchor->getControllingPlayer() != m_viewer)
		return 0;
	GXWalk w = { m_viewer, ownAnchor, r * r, 0, m_tierOf != nullptr && m_tierOf(m_viewer) == gxrb::TIER_BRUTAL };
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (p != nullptr && p != m_viewer)
			p->iterateObjects(gxWalkOne, &w);
	}
	return w.value;
}
#endif

#if defined(GX_TEST_INSTRUMENTS)
// ---- the sense layer's in-engine fixtures (GX_RULE_SENSE_TEST, run once by PROBE's first tick; rb-core plan Task 7). They SPAWN objects,
// so a fixture run is never a CRC-identity run. Each prints [GXRULE] ... ev=sense_test case=<c> ... result=PASS|FAIL|SKIP why=.
#include <cmath>
#include "Common/ThingFactory.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/GXRules/GXRuleBank.h"
#include "GameLogic/Module/ContainModule.h"
namespace
{
Object *gxFxSpawn(const char *tmpl, Player *owner, const Coord3D &at)
{
	const ThingTemplate *t = TheThingFactory->findTemplate(AsciiString(tmpl), FALSE);
	if (t == nullptr || owner == nullptr || owner->getDefaultTeam() == nullptr)
		return nullptr;
	Object *o = TheThingFactory->newObject(t, owner->getDefaultTeam());
	if (o != nullptr)
	{
		o->setPosition(&at);
		// into its partition cells NOW, as this frame's PartitionManager::update() would (it only marked the object dirty): a fixture
		// senses in the same tick it spawns (Codex core-r4-lensA P2 - without this every positive case saw an empty cell)
		if (o->friend_getPartitionData() != nullptr)
			o->friend_getPartitionData()->friend_updateCellsTouched();
	}
	return o;
}
void gxFxFirstStructurePos(Object *o, void *userData)
{
	Coord3D *c = static_cast<Coord3D *>(userData);
	if (c->z < 0.0f && o->isKindOf(KINDOF_STRUCTURE) && !o->isEffectivelyDead())
		*c = *o->getPosition();
}
}

namespace
{
/// the n-th player (n = 0, 1, ...) with relation `rel` to the viewer, among the computer players
Player *gxFxPlayer(Player *viewer, Relationship rel, int n)
{
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (p == nullptr || p == viewer || p->getDefaultTeam() == nullptr || p->getPlayerType() != PLAYER_COMPUTER)
			continue;
		if (viewer->getRelationship(p->getDefaultTeam()) == rel && n-- == 0)
			return p;
	}
	return nullptr;
}
Int gxFxCost(const char *tmpl, Player *owner)
{
	const ThingTemplate *t = TheThingFactory->findTemplate(AsciiString(tmpl), FALSE);
	return (t != nullptr && owner != nullptr) ? gxListPrice(t) : -1;	// the list price, as the sense layer counts (Codex rbs2-r1 P1)
}
void gxFxLine(GXRuleCtx &ctx, const char *cs, const char *sub, Int got, Int want, const char *extra = "")
{
	char kv[256];
	// every line names its viewer's tier (a Brutal-only red arm fails for that player alone), unless the case already does
	const char *tier = strstr(extra, "tier=") != nullptr ? "" : gxRuleTierOf(ctx.player()) == gxrb::TIER_BRUTAL ? " tier=BRUTAL" : " tier=FAIR";
	snprintf(kv, sizeof(kv), "case=%s sub=%s got=%d want=%d%s%s%s result=%s", cs, sub, (int)got, (int)want, *extra ? " " : "", extra, tier,
		got == want && want >= 0 ? "PASS" : "FAIL");
	ctx.log("sense_test", kv);
}
}

// Every expectation below is an INDEPENDENT number (a template's build cost, or 0) written by hand from what a human in the viewer's
// seat would see - never recomputed through the classifier under test (Codex core-r1-lensB P1/P2). Each sub-case prints its own
// "case= sub= got= want= result=" line; a case also needs every template it spawns (else result=SKIP why=template).
void gxRuleSenseFactionFixture(GXRuleCtx &ctx, const char *which);
void gxRuleSenseFixture(GXRuleCtx &ctx, const char *which)
{
	if (strncmp(which, "faction", 7) == 0 || strcmp(which, "own_sight") == 0 || strncmp(which, "seen_", 5) == 0)
	{
		gxRuleSenseFactionFixture(ctx, which);
		return;
	}
	Player *viewer = ctx.player();
	std::vector<Object *> own;
	ctx.ownObjects(&own);
	const Object *anchor = nullptr;
	for (size_t i = 0; i < own.size() && anchor == nullptr; ++i)
		if (own[i]->isKindOf(KINDOF_STRUCTURE))
			anchor = own[i];
	Player *enemy = gxFxPlayer(viewer, ENEMIES, 0);
	if (anchor == nullptr || enemy == nullptr)
	{
		ctx.log("sense_test", "result=SKIP why=no_anchor_or_enemy");
		return;
	}
	const GXRuleSense &sense = ctx.sense();
	const Real R = 600.0f;
	Coord3D at = *anchor->getPosition();
	at.x += 80.0f;
	if (strcmp(which, "cargo") == 0)
	{
		// an EMPTY enclosing transport (a supply Chinook: no initial payload; Codex core-r2-lensB P2 - the Troop Crawler is armed and
		// starts with 8 passengers) and 4 armed Red Guards beside it, then inside it. The transport's own worth is MEASURED (it cancels
		// out); the Red Guards' is an independent number: 4 x their build cost outside, 0 inside.
		const Int v0 = sense.enemyValueNear(anchor, R);
		const Int w0 = sense.enemyValueNearByListWalkForTest(anchor, R);
		Object *tr = gxFxSpawn("AmericaVehicleChinook", enemy, at);
		const Int guard = gxFxCost("ChinaInfantryRedguard", enemy);
		if (tr == nullptr || tr->getContain() == nullptr || tr->getContain()->getContainCount() != 0u || guard <= 0
			|| (tr->getContain()->getContainMax() >= 0 && tr->getContain()->getContainMax() < 4))
		{
			ctx.log("sense_test", "case=cargo result=SKIP why=transport");
			return;
		}
		const Int vT = sense.enemyValueNear(anchor, R) - v0;				// the empty transport alone
		const Int wT = sense.enemyValueNearByListWalkForTest(anchor, R) - w0;
		std::vector<Object *> inf;
		for (int k = 0; k < 4; ++k)
		{
			Coord3D p = at;
			p.y += 15.0f * (Real)(k + 1);
			Object *o = gxFxSpawn("ChinaInfantryRedguard", enemy, p);
			if (o != nullptr)
				inf.push_back(o);
		}
		if (inf.size() != 4u)
		{
			ctx.log("sense_test", "case=cargo result=SKIP why=template");
			return;
		}
		gxFxLine(ctx, "cargo", "outside", sense.enemyValueNear(anchor, R) - v0 - vT, 4 * guard);
		Bool enclosed = TRUE;
		for (size_t k = 0; k < inf.size(); ++k)
		{
			enclosed = enclosed && tr->getContain()->isEnclosingContainerFor(inf[k]);
			tr->getContain()->addToContain(inf[k]);
		}
		char ex[96];
		snprintf(ex, sizeof(ex), "enclosed=%d contained=%u", (int)enclosed, (unsigned)tr->getContain()->getContainCount());
		gxFxLine(ctx, "cargo", "loaded", sense.enemyValueNear(anchor, R) - v0 - vT,
			(enclosed && tr->getContain()->getContainCount() == 4u) ? 0 : -1, ex);
		// the RED ARM, by an independent number: a list walk with the fog test still counts the 4 riders (they left the partition and
		// getShroudedStatus says CLEAR for them) - exactly 4 x their cost over the walk's own empty-transport value
		gxFxLine(ctx, "cargo", "listwalk_redarm", sense.enemyValueNearByListWalkForTest(anchor, R) - w0 - wT, 4 * guard);
		TheGameLogic->destroyObject(tr);
		return;
	}
	if (strcmp(which, "disguise") == 0)
	{
		// needs a 2v2 (GX_TEAMS): an ALLY of the viewer, and TWO enemies. A GLA Bomb Truck disguised as a Crusader of a THIRD player is
		// worth that Crusader's cost to the viewer - never the truck's - and nothing when the Crusader's owner is not hostile
		Player *enemy2 = gxFxPlayer(viewer, ENEMIES, 1);
		Player *ally = gxFxPlayer(viewer, ALLIES, 0);
		Player *neutral = ThePlayerList->getNeutralPlayer();
		const Int truckB = gxFxCost("GLAVehicleBombTruck", enemy);
		const Int crusaderC = gxFxCost("AmericaTankCrusader", enemy2);
		if (enemy2 == nullptr || ally == nullptr || truckB <= 0 || crusaderC <= 0 || truckB == crusaderC)
		{
			ctx.log("sense_test", "case=disguise result=SKIP why=needs_2v2_and_templates");
			return;
		}
		Coord3D farPos = at;	// GeneralsX 01/11/2026 t2-fix-2: not "far" - <windows.h> defines far (and near) as empty macros, so MSVC read "Coord3D = at;"
		farPos.x += 5000.0f;	// the disguise models, off the sensing range
		Object *modelC = gxFxSpawn("AmericaTankCrusader", enemy2, farPos);
		farPos.y += 100.0f;
		Object *modelN = gxFxSpawn("AmericaVehicleHumvee", neutral, farPos);
		const Int v0 = sense.enemyValueNear(anchor, R);
		Object *t1 = gxFxSpawn("GLAVehicleBombTruck", enemy, at);
		if (modelC == nullptr || modelN == nullptr || t1 == nullptr || t1->getStealth() == nullptr)
		{
			ctx.log("sense_test", "case=disguise result=SKIP why=template");
			return;
		}
		gxFxLine(ctx, "disguise", "plain_enemy_truck", sense.enemyValueNear(anchor, R) - v0, truckB);
		t1->getStealth()->disguiseAsObject(modelC);
		gxFxLine(ctx, "disguise", "as_third_enemy_crusader", sense.enemyValueNear(anchor, R) - v0, crusaderC);
		t1->getStealth()->disguiseAsObject(modelN);
		gxFxLine(ctx, "disguise", "as_neutral_humvee", sense.enemyValueNear(anchor, R) - v0, 0);
		TheGameLogic->destroyObject(t1);
		// an ALLY's truck disguised as the enemy's Crusader: the viewer sees through it (its real owner is allied) - worth 0
		Object *t2 = gxFxSpawn("GLAVehicleBombTruck", ally, at);
		if (t2 != nullptr && t2->getStealth() != nullptr)
		{
			t2->getStealth()->disguiseAsObject(modelC);
			gxFxLine(ctx, "disguise", "ally_truck_as_enemy_crusader", sense.enemyValueNear(anchor, R) - v0, 0);
			TheGameLogic->destroyObject(t2);
		}
		TheGameLogic->destroyObject(modelC);
		TheGameLogic->destroyObject(modelN);
		return;
	}
	if (strcmp(which, "filters") == 0)
	{
		// each exclusion its OWN sub-case against an independent number, measured as the step it adds (Codex core-r3-lensB P2: one
		// gate over all of them meant an armed "unarmed" template skipped every case): a live tank adds its cost; a dozer, a harvester,
		// a dead tank and an UNARMED ordinary unit add 0 - a US Pilot (retail AmericaInfantryPilot: no Weapon line, no payload; the
		// Listening Outpost has a dummy weapon and 2 Tank Hunters aboard); stealthed and undetected 0, detected again its cost; an anchor
		// the viewer does not own 0; an object in the map's border (off the playable map) 0. (countsAsThreat's flags are unit-tested one
		// by one in the pure core.)
		const Int crusader = gxFxCost("AmericaTankCrusader", enemy);
		Coord3D p = at;
		std::vector<Object *> spawned;
		Object *tank = nullptr;
		struct Step { const char *sub, *tmpl; Int want; Bool killIt, mustBeUnarmed; };
		const Step steps[] = {{"tank", "AmericaTankCrusader", crusader, FALSE, FALSE}, {"dozer", "ChinaVehicleDozer", 0, FALSE, FALSE},
			{"harvester", "ChinaVehicleSupplyTruck", 0, FALSE, FALSE}, {"dead", "AmericaTankCrusader", 0, TRUE, FALSE},
			{"unarmed", "AmericaInfantryPilot", 0, FALSE, TRUE}};
		for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); ++k)
		{
			const Int before = sense.enemyValueNear(anchor, R);
			Object *o = gxFxSpawn(steps[k].tmpl, enemy, p);
			p.y += 40.0f;
			char ex[96];
			if (o == nullptr || crusader <= 0 || (steps[k].mustBeUnarmed && o->getTemplate()->canPossiblyHaveAnyWeapon())
				|| (o->getContain() != nullptr && o->getContain()->getContainCount() != 0u))
			{
				snprintf(ex, sizeof(ex), "case=filters sub=%s result=SKIP why=template", steps[k].sub);
				ctx.log("sense_test", ex);
				continue;
			}
			spawned.push_back(o);
			if (steps[k].killIt)
				o->kill();
			if (k == 0)
				tank = o;
			snprintf(ex, sizeof(ex), "tmpl=%s", steps[k].tmpl);
			gxFxLine(ctx, "filters", steps[k].sub, sense.enemyValueNear(anchor, R) - before, steps[k].want, ex);
		}
		if (tank != nullptr)
		{
			// the foreign anchor against a NON-ZERO control (Codex core-r4-lensB P1): a second visible armed enemy tank beside the first
			// counts from the viewer's own anchor (second_tank = its cost), so a sense layer without the own-anchor guard would count it
			// around the first tank too (foreign_anchor = that cost, not 0) - the GX_TEST_SENSE_MUTANT=no_owner_guard red arm
			Coord3D p2 = *tank->getPosition();
			p2.x += 25.0f;
			const Int before2 = sense.enemyValueNear(anchor, R);
			Object *tank2 = gxFxSpawn("AmericaTankCrusader", enemy, p2);
			if (tank2 == nullptr)
				ctx.log("sense_test", "case=filters sub=second_tank result=SKIP why=template");
			else
			{
				spawned.push_back(tank2);
				gxFxLine(ctx, "filters", "second_tank", sense.enemyValueNear(anchor, R) - before2, crusader);
				gxFxLine(ctx, "filters", "foreign_anchor", sense.enemyValueNear(tank, R), 0,
					gxSenseNoOwnerGuardForTest() ? "mutant=no_owner_guard" : "");
			}
			const Int v0 = sense.enemyValueNear(anchor, R) - crusader;		// everything but the (visible, live) first tank
			tank->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), TRUE);
			gxFxLine(ctx, "filters", "stealthed_undetected", sense.enemyValueNear(anchor, R) - v0, 0);
			tank->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), TRUE);
			gxFxLine(ctx, "filters", "stealthed_detected", sense.enemyValueNear(anchor, R) - v0, crusader);
		}
		for (size_t k = 0; k < spawned.size(); ++k)
			TheGameLogic->destroyObject(spawned[k]);
		// off the playable map: the border corner of the map, with a radius that reaches it
		Region3D ext;
		TheTerrainLogic->getExtentIncludingBorder(&ext);
		Coord3D corner;
		corner.x = ext.lo.x + 5.0f;
		corner.y = ext.lo.y + 5.0f;
		corner.z = 0.0f;
		const Coord3D *a = anchor->getPosition();
		const Real dx = corner.x - a->x, dy = corner.y - a->y;
		const Real rr = sqrtf(dx * dx + dy * dy) + 100.0f;
		const Int vb = sense.enemyValueNear(anchor, rr);
		Object *off = gxFxSpawn("AmericaTankCrusader", enemy, corner);
		if (off != nullptr)
		{
			char ex[64];
			snprintf(ex, sizeof(ex), "offmap=%d", (int)off->isOffMap());
			gxFxLine(ctx, "filters", "off_map", sense.enemyValueNear(anchor, rr) - vb, off->isOffMap() ? 0 : -1, ex);
			TheGameLogic->destroyObject(off);
		}
		return;
	}
	if (strcmp(which, "fog") == 0)
	{
		Coord3D base;
		base.x = base.y = 0.0f;
		base.z = -1.0f;
		enemy->iterateObjects(gxFxFirstStructurePos, &base);
		if (base.z < 0.0f)
		{
			ctx.log("sense_test", "case=fog result=SKIP why=no_enemy_base");
			return;
		}
		// a spawn point the VIEWER provably cannot see (Mini fx run 30/10: in the observer-hosted headless games one AI saw the other's
		// base at frame 118, so "beside the enemy base" was no fog for it): the enemy base first, then a 9 x 9 grid inside the map, the
		// first point where the spawned tank is on the map and shrouded for the viewer. fogpts= counts the grid points fogged for it
		// (0 = it sees the whole map: the case cannot run, and that is itself worth knowing)
		const Int vi = viewer->getPlayerIndex();
		Region3D ext;
		TheTerrainLogic->getExtentIncludingBorder(&ext);
		std::vector<Coord3D> cand;
		Coord3D b0 = base;
		b0.x += 60.0f;
		cand.push_back(b0);
		Int fogpts = 0;
		for (int gy = 1; gy <= 9; ++gy)
			for (int gx = 1; gx <= 9; ++gx)
			{
				Coord3D c;
				c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
				c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
				c.z = 0.0f;
				if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) != CELLSHROUD_CLEAR)
				{
					++fogpts;
					cand.push_back(c);
				}
			}
		const Int cost = gxFxCost("AmericaTankCrusader", enemy);
		const Bool brutal = gxRuleTierOf(viewer) == gxrb::TIER_BRUTAL;
		const Coord3D *a = anchor->getPosition();
		for (size_t k = 0; k < cand.size() && cost > 0; ++k)
		{
			const Real dx = cand[k].x - a->x, dy = cand[k].y - a->y;
			const Real r = sqrtf(dx * dx + dy * dy) + 200.0f;
			const Int v0 = sense.enemyValueNear(anchor, r);
			Object *tank = gxFxSpawn("AmericaTankCrusader", enemy, cand[k]);
			if (tank == nullptr)
				break;
			if (tank->isOffMap() || tank->getShroudedStatus(vi) == OBJECTSHROUD_CLEAR)
			{
				TheGameLogic->destroyObject(tank);
				continue;
			}
			char ex[96];
			snprintf(ex, sizeof(ex), "tier=%s fogged=1 at=%d fogpts=%d", brutal ? "BRUTAL" : "FAIR", (int)k, (int)fogpts);
			gxFxLine(ctx, "fog", "enemy_tank_in_its_base", sense.enemyValueNear(anchor, r) - v0, brutal ? cost : 0, ex);
			TheGameLogic->destroyObject(tank);
			return;
		}
		char kv[96];
		snprintf(kv, sizeof(kv), "case=fog sub=enemy_tank_in_its_base result=SKIP why=nothing_fogged fogpts=%d", (int)fogpts);
		ctx.log("sense_test", kv);
		return;
	}
	char kv[96];
	snprintf(kv, sizeof(kv), "result=SKIP why=unknown_case case=%s", which);
	ctx.log("sense_test", kv);
}

// ---- the SENSE.ENEMY_FACTION fixtures (GeneralsX 01/11/2026 rb-sense-1), GX_RULE_SENSE_TEST=faction | faction_disguise |
// faction_captured | faction_fog. Every viewer asks its first enemy E through the condition's own entry (GXRuleBank::senseEnemyFaction)
// for every playable side and base side; got = that answer as a bit mask over those strings (gxFxFactionNames' order), want = an
// INDEPENDENT number: the lobby slot's ORIGINAL template's side + base side when the lobby showed it (TheGameInfo, read here), else 0
// until the sightings the case plants say otherwise. A Random enemy is the case under test; a lobby-known one proves sightings never
// override the lobby. The pass is run at once (senseSampleNowForTest) after each plant, as its half-second cadence would.
#include "Common/PlayerTemplate.h"
#include "Common/NameKeyGenerator.h"
#include "GameNetwork/GameInfo.h"
#include "GameLogic/ObjectTypes.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/Scripts.h"
#include "GameLogic/ScriptConditions.h"
#include "GameLogic/ScriptEngine.h"
namespace
{
std::vector<std::string> gxFxFactionNames()
{
	std::vector<std::string> out;
	for (Int i = 0; ThePlayerTemplateStore != nullptr && i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (pt == nullptr || !pt->isPlayableSide())
			continue;
		const std::string two[2] = { pt->getSide().str(), pt->getBaseSide().str() };
		for (int k = 0; k < 2; ++k)
			if (!two[k].empty() && std::find(out.begin(), out.end(), two[k]) == out.end() && out.size() < 31u)
				out.push_back(two[k]);
	}
	return out;
}
Int gxFxBit(const std::vector<std::string> &names, const std::string &n)
{
	for (size_t i = 0; i < names.size(); ++i)
		if (names[i] == n)
			return 1 << (Int)i;
	return 0;
}
/// what the lobby shows of p: its slot's ORIGINAL template's side + base side (bits), 0 for a Random or unknown slot
Int gxFxLobbyBits(const Player *p, const std::vector<std::string> &names, Bool *random)
{
	*random = TRUE;
	if (p == nullptr || TheGameInfo == nullptr || TheNameKeyGenerator == nullptr)
		return 0;
	const int slot = gxrb::slotOfPlayerName(TheNameKeyGenerator->keyToName(p->getPlayerNameKey()).str());
	const GameSlot *gs = (slot >= 0 && slot < MAX_SLOTS) ? TheGameInfo->getConstSlot(slot) : nullptr;
	if (gs == nullptr || !gs->hasSavedOriginalSetup() || gs->getOriginalPlayerTemplate() < 0)
		return 0;
	const PlayerTemplate *pt = ThePlayerTemplateStore->getNthPlayerTemplate(gs->getOriginalPlayerTemplate());
	if (pt == nullptr)
		return 0;
	*random = FALSE;
	return gxFxBit(names, pt->getSide().str()) | gxFxBit(names, pt->getBaseSide().str());
}
Int gxFxAsk(GXRuleBank &bank, const Player *e, const std::vector<std::string> &names)
{
	Int m = 0;
	for (size_t i = 0; i < names.size(); ++i)
		if (bank.senseEnemyFaction(e, names[i].c_str()))
			m |= 1 << (Int)i;
	return m;
}
void gxFxFactionLine(GXRuleCtx &ctx, const char *cs, const char *sub, Int got, Int want, Bool random)
{
	char ex[48];
	snprintf(ex, sizeof(ex), "enemy_lobby=%s", random ? "random" : "known");
	gxFxLine(ctx, cs, sub, got, want, ex);
}
}

void gxRuleSenseFactionFixture(GXRuleCtx &ctx, const char *which)
{
	Player *viewer = ctx.player();
	GXRuleBank &bank = ctx.bankForTest();
	std::vector<Object *> own;
	ctx.ownObjects(&own);
	const Object *anchor = nullptr;
	for (size_t i = 0; i < own.size() && anchor == nullptr; ++i)
		if (own[i]->isKindOf(KINDOF_STRUCTURE))
			anchor = own[i];
	Player *enemy = gxFxPlayer(viewer, ENEMIES, 0);
	const std::vector<std::string> names = gxFxFactionNames();
	const char *gla[4] = { "GLAInfantryRebel", "GLAVehicleTechnical", "GLATankScorpion", "GLAVehicleQuadCannon" };
	Bool tmplOk = TheThingFactory->findTemplate(AsciiString("AmericaTankCrusader"), FALSE) != nullptr
		&& TheThingFactory->findTemplate(AsciiString("GLAVehicleBombTruck"), FALSE) != nullptr;
	for (int k = 0; k < 4; ++k)
		tmplOk = tmplOk && TheThingFactory->findTemplate(AsciiString(gla[k]), FALSE) != nullptr;
	const Int bGLA = gxFxBit(names, "GLA");
	char kv[128];
	if (anchor == nullptr || enemy == nullptr || !tmplOk || bGLA == 0)
	{
		snprintf(kv, sizeof(kv), "case=%s result=SKIP why=%s", which, anchor == nullptr || enemy == nullptr ? "no_anchor_or_enemy" : "template");
		ctx.log("sense_test", kv);
		return;
	}
	Bool random = TRUE;
	const Int lobby = gxFxLobbyBits(enemy, names, &random);
	const Int known = random ? bGLA : lobby;		// what three fair GLA sightings make of it: GLA for a Random enemy, the lobby's otherwise
	// ISOLATION (Mini fx run 01/11: in these small observer-hosted games a viewer may already see the enemy's own base at frame 118, and
	// those real sightings mixed with - or pre-empted - the planted ones): the enemy's REAL objects are stealthed and undetected for the
	// fixture's one tick, and restored on every way out. Then a baseline pass must leave a Random enemy unseen, else the case is SKIPPED
	// (missing evidence, never a pass). Fixture games are never CRC-identity runs (they spawn objects).
	struct GXFxHide
	{
		struct Bits { Object *o; Bool stealthed, detected; };
		std::vector<Bits> bits;
		static void take(Object *o, void *userData)
		{
			if (o == nullptr || o->isDestroyed())
				return;
			Bits b = { o, o->testStatus(OBJECT_STATUS_STEALTHED), o->testStatus(OBJECT_STATUS_DETECTED) };
			static_cast<GXFxHide *>(userData)->bits.push_back(b);
		}
		~GXFxHide()
		{
			for (size_t k = 0; k < bits.size(); ++k)
			{
				bits[k].o->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), bits[k].stealthed);
				bits[k].o->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), bits[k].detected);
			}
		}
	} hide;
	enemy->iterateObjects(GXFxHide::take, &hide);
	for (size_t k = 0; k < hide.bits.size(); ++k)
	{
		hide.bits[k].o->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), TRUE);
		hide.bits[k].o->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), FALSE);
	}
	bank.senseSampleNowForTest();
	const gxrb::FactionSense *base0 = bank.senseStateForTest(enemy);
	if (random && base0 != nullptr && (!base0->seenBase.empty() || base0->mixed || !base0->known.empty()))
	{
		snprintf(kv, sizeof(kv), "case=%s result=SKIP why=enemy_seen_before_plant seen=%s", which, base0->seenBase.c_str());
		ctx.log("sense_test", kv);
		return;
	}
	Coord3D at = *anchor->getPosition();
	at.x += 80.0f;
	std::vector<Object *> spawned;
	auto put = [&](const char *t, Player *owner, Real dy) -> Object *
	{
		Coord3D p = at;
		p.y += dy;
		Object *o = gxFxSpawn(t, owner, p);
		if (o != nullptr)
			spawned.push_back(o);
		return o;
	};
	auto done = [&]()
	{
		for (size_t k = 0; k < spawned.size(); ++k)
			TheGameLogic->destroyObject(spawned[k]);
		spawned.clear();
	};
	gxFxFactionLine(ctx, which, "start", gxFxAsk(bank, enemy, names), lobby, random);	// before anything is seen: the lobby alone
	if (strcmp(which, "faction") == 0)
	{
		put(gla[0], enemy, 0.0f);
		put(gla[1], enemy, 30.0f);
		put(gla[0], enemy, 60.0f);	// the same template again is not a third
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "two", gxFxAsk(bank, enemy, names), lobby, random);
		Object *sc = put(gla[2], enemy, 90.0f);
		if (sc == nullptr)
			return;
		sc->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), TRUE);
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "stealthed", gxFxAsk(bank, enemy, names), lobby, random);	// stealthed and undetected: not seen
		sc->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), TRUE);
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "third", gxFxAsk(bank, enemy, names), known, random);
		done();
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "kept", gxFxAsk(bank, enemy, names), known, random);	// once known it stays known
		return;
	}
	if (strcmp(which, "faction_disguise") == 0)
	{
		Object *rebel = put(gla[0], enemy, 0.0f);
		put(gla[1], enemy, 30.0f);
		Coord3D farPos = at;
		farPos.x += 5000.0f;
		Object *modelN = gxFxSpawn("AmericaVehicleHumvee", ThePlayerList->getNeutralPlayer(), farPos);
		if (modelN != nullptr)
			spawned.push_back(modelN);
		if (rebel == nullptr || modelN == nullptr)
		{
			ctx.log("sense_test", "case=faction_disguise result=SKIP why=template");
			done();
			return;
		}
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "two", gxFxAsk(bank, enemy, names), lobby, random);
		// the truck is GLA's own 3rd template - shown as something else it must never count as one (spawned only now, and disguised
		// before the next pass: undisguised it IS a third GLA template)
		Object *truck = put("GLAVehicleBombTruck", enemy, 60.0f);
		if (truck == nullptr || truck->getStealth() == nullptr)
		{
			ctx.log("sense_test", "case=faction_disguise result=SKIP why=template");
			done();
			return;
		}
		truck->getStealth()->disguiseAsObject(rebel);			// as E's own Rebel: seen as a Rebel (a template already seen)
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "as_own_rebel", gxFxAsk(bank, enemy, names), lobby, random);
		truck->getStealth()->disguiseAsObject(modelN);			// as a neutral Humvee: not E's at all
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "as_neutral", gxFxAsk(bank, enemy, names), lobby, random);
		put(gla[2], enemy, 90.0f);									// a genuine third template: now it is known
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "third_genuine", gxFxAsk(bank, enemy, names), known, random);
		done();
		return;
	}
	if (strcmp(which, "faction_captured") == 0)
	{
		Object *cap = put("AmericaTankCrusader", ThePlayerList->getNeutralPlayer(), 0.0f);
		if (cap == nullptr || enemy->getDefaultTeam() == nullptr)
		{
			ctx.log("sense_test", "case=faction_captured result=SKIP why=template");
			done();
			return;
		}
		cap->setTeam(enemy->getDefaultTeam());					// captured / hijacked: a USA template under E
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "captured_alone", gxFxAsk(bank, enemy, names), lobby, random);
		put(gla[0], enemy, 30.0f);
		put(gla[1], enemy, 60.0f);
		put(gla[2], enemy, 90.0f);
		bank.senseSampleNowForTest();
		gxFxFactionLine(ctx, which, "mixed", gxFxAsk(bank, enemy, names), lobby, random);	// a mixed picture stays unknown
		done();
		return;
	}
	if (strcmp(which, "faction_fog") == 0)
	{
		// three GLA templates of E at the first map point the VIEWER provably does not see (the fog case's grid): FAIR stays unknown,
		// BRUTAL (whole map) knows; then the same three beside the viewer's base: known for both tiers
		const Int vi = viewer->getPlayerIndex();
		const Bool brutal = gxRuleTierOf(viewer) == gxrb::TIER_BRUTAL;
		Region3D ext;
		TheTerrainLogic->getExtentIncludingBorder(&ext);
		Bool placed = FALSE;
		for (int gy = 1; gy <= 9 && !placed; ++gy)
			for (int gx = 1; gx <= 9 && !placed; ++gx)
			{
				Coord3D c;
				c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
				c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
				c.z = 0.0f;
				if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) == CELLSHROUD_CLEAR)
					continue;
				Bool fogged = TRUE;
				for (int k = 0; k < 3; ++k)
				{
					Coord3D p = c;
					p.y += 20.0f * (Real)k;
					Object *o = gxFxSpawn(gla[k], enemy, p);
					if (o == nullptr)
						fogged = FALSE;
					else
					{
						spawned.push_back(o);
						const ObjectShroudStatus st = o->getShroudedStatus(vi);
						fogged = fogged && !o->isOffMap() && st != OBJECTSHROUD_CLEAR && st != OBJECTSHROUD_PARTIAL_CLEAR;
					}
				}
				if (fogged)
					placed = TRUE;
				else
					done();
			}
		if (!placed)
		{
			ctx.log("sense_test", "case=faction_fog sub=fogged result=SKIP why=nothing_fogged");
			return;
		}
		bank.senseSampleNowForTest();
		char ex[64];
		snprintf(ex, sizeof(ex), "enemy_lobby=%s tier=%s", random ? "random" : "known", brutal ? "BRUTAL" : "FAIR");
		gxFxLine(ctx, which, "fogged", gxFxAsk(bank, enemy, names), brutal ? known : lobby, ex);
		done();
		put(gla[0], enemy, 0.0f);
		put(gla[1], enemy, 30.0f);
		put(gla[2], enemy, 60.0f);
		bank.senseSampleNowForTest();
		gxFxLine(ctx, which, "visible", gxFxAsk(bank, enemy, names), known, ex);
		done();
		return;
	}
	if (strcmp(which, "seen_stale") == 0)
	{
		// GeneralsX 02/11/2026 rb-sense-2 (Codex rbs2-r2 P1): an answer is the world at the moment of the ask, never a snapshot an earlier
		// ask made - two visible Scorpions of E are counted (that ask would prime any cache), one is destroyed, the next ask in the SAME
		// tick counts one. Red arm: GX_TEST_SENSE_MUTANT=frame_cache answers 2 again.
		ObjectTypes *scorp = newInstance(ObjectTypes);
		scorp->addObjectType(AsciiString(gla[2]));
		gxFxLine(ctx, which, "none", bank.seenCount(enemy, *scorp), 0);
		put(gla[2], enemy, 0.0f);
		Object *s2 = put(gla[2], enemy, 30.0f);
		gxFxLine(ctx, which, "primed", bank.seenCount(enemy, *scorp), s2 != nullptr ? 2 : -1);
		if (s2 != nullptr)
		{
			spawned.erase(std::find(spawned.begin(), spawned.end(), s2));
			TheGameLogic->destroyObject(s2);
		}
		gxFxLine(ctx, which, "after_delete", bank.seenCount(enemy, *scorp), 1);
		deleteInstance(scorp);
		done();
		return;
	}
	if (strcmp(which, "seen_self_reveal") == 0)
	{
		// GeneralsX 02/11/2026 rb-sense-3 (lead's ruling 1): an enemy object that reveals itself to all counts as seen. E's Scud Storm far from anything
		// of the viewer's: fogged while under construction, then the game shows it to every enemy (ShroudRevealToAllRange, Object::look) and the
		// viewer's count and sighting pass have it. The game's own answer (getShroudedStatus, not the sensor's) is checked for each state. Not while
		// under construction, not while stealthed and undetected (the stealth general's), again once detected; what stands beside it needs an eye.
		const char *sw = "GLAScudStorm";
		const Int vi = viewer->getPlayerIndex();
		const Bool brutal = gxRuleTierOf(viewer) == gxrb::TIER_BRUTAL;
		const GXRuleSense &sense = ctx.sense();
		ObjectTypes *types = newInstance(ObjectTypes);
		types->addObjectType(AsciiString(sw));
		ObjectTypes *scorp = newInstance(ObjectTypes);
		scorp->addObjectType(AsciiString(gla[2]));
		auto count = [&]() -> Int { return bank.seenCount(enemy, *types); };
		auto sighted = [&]() -> Int
		{
			std::vector<std::vector<gxrb::Sighting> > out((size_t)ThePlayerList->getPlayerCount());
			const Int ei = enemy->getPlayerIndex();
			if (ei < 0 || ei >= MAX_PLAYER_COUNT || (size_t)ei >= out.size())
				return -1;
			sense.collectSightings(gxPlayerBit(ei), &out);
			Int n = 0;
			for (size_t k = 0; k < out[(size_t)ei].size(); ++k)
				n += out[(size_t)ei][k].tmpl == sw ? 1 : 0;
			return n;
		};
		auto shown = [&](Object *o) -> Bool
		{
			const ObjectShroudStatus st = o->getShroudedStatus(vi);
			return st == OBJECTSHROUD_CLEAR || st == OBJECTSHROUD_PARTIAL_CLEAR;
		};
		auto relook = [&](Object *o)
		{
			o->handlePartitionCellMaintenance();
			ThePartitionManager->processEntirePendingUndoShroudRevealQueue();
		};
		Region3D ext;
		TheTerrainLogic->getExtentIncludingBorder(&ext);
		Object *s = nullptr;
		Object *beside = nullptr;
		for (int gy = 1; gy <= 9 && s == nullptr; ++gy)
			for (int gx = 1; gx <= 9 && s == nullptr; ++gx)
			{
				Coord3D c;
				c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
				c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
				c.z = 0.0f;
				if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) == CELLSHROUD_CLEAR)
					continue;
				Object *o = gxFxSpawn(sw, enemy, c);
				if (o == nullptr)
					break;
				spawned.push_back(o);
				// a new object has already looked (Mini F3a 02/11: every spot read "shown"), so start it as a dozer would: under construction shows nothing
				o->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_UNDER_CONSTRUCTION), TRUE);
				relook(o);
				if (o->isOffMap() || shown(o))
				{
					done();
					continue;
				}
				s = o;
				Coord3D b = c;
				b.x += 20.0f;	// inside the Scud Storm's 60-unit disc
				beside = put(gla[2], enemy, 0.0f);
				if (beside != nullptr)
				{
					beside->setPosition(&b);
					if (beside->friend_getPartitionData() != nullptr)
						beside->friend_getPartitionData()->friend_updateCellsTouched();
				}
			}
		if (s == nullptr || beside == nullptr)
		{
			ctx.log("sense_test", "case=seen_self_reveal sub=fogged result=SKIP why=nothing_fogged");
			deleteInstance(types);
			deleteInstance(scorp);
			done();
			return;
		}
		gxFxLine(ctx, which, "fogged", count(), brutal ? 1 : 0);	// under construction: nothing shows it
		gxFxLine(ctx, which, "fogged_sighting", sighted(), brutal ? 1 : 0);
		s->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_UNDER_CONSTRUCTION), FALSE);
		relook(s);
		if (!shown(s) || !s->friend_getPartitionRevealAllLastLook() || s->friend_getPartitionRevealAllLastLook()->isInvalid())
			ctx.log("sense_test", "case=seen_self_reveal sub=shown result=SKIP why=game_does_not_show_it");
		else
		{
			gxFxLine(ctx, which, "shown", count(), 1);
			gxFxLine(ctx, which, "shown_sighting", sighted(), 1);
			gxFxLine(ctx, which, "beside", bank.seenCount(enemy, *scorp), brutal ? 1 : 0);	// inside the disc: a human sees it too; the ruling covers the object itself
		}
		// under construction: Object::look shows nothing yet (setStatus re-looks: "a refresh occurs")
		s->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_UNDER_CONSTRUCTION), TRUE);
		relook(s);
		gxFxLine(ctx, which, "under_construction", count(), brutal ? 1 : 0);
		s->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_UNDER_CONSTRUCTION), FALSE);
		relook(s);
		gxFxLine(ctx, which, "finished", count(), 1);
		// stealthed and undetected: no reveal at all, and even BRUTAL does not see stealth; detected: shown again
		s->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), TRUE);
		relook(s);
		gxFxLine(ctx, which, "stealthed", count(), 0);
		s->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), TRUE);
		relook(s);
		gxFxLine(ctx, which, "detected", count(), 1);
		deleteInstance(types);
		deleteInstance(scorp);
		s->setStatus(MAKE_OBJECT_STATUS_MASK2(OBJECT_STATUS_STEALTHED, OBJECT_STATUS_DETECTED), FALSE);
		s->setShroudClearingRange(0.0f);
		relook(s);
		done();
		return;
	}
	if (strcmp(which, "seen_count") == 0 || strcmp(which, "seen_value") == 0)
	{
		// GeneralsX 02/11/2026 rb-sense-2: SENSE.FOG_CONDITIONS through the conditions' own entries (GXRuleBank::seenCount / seenValueIn),
		// with E's real objects hidden (above). Every want is an independent number: how many planted objects - or their build costs - a
		// human in the viewer's seat sees, by hand. Every ask computes from the world at that moment (no snapshot is kept).
		const Bool brutal = gxRuleTierOf(viewer) == gxrb::TIER_BRUTAL;
		const Int vi = viewer->getPlayerIndex();
		auto blind = [&](Object *o)
		{
			if (o == nullptr)
				return;
			o->setShroudClearingRange(0.0f);
			o->handlePartitionCellMaintenance();
			ThePartitionManager->processEntirePendingUndoShroudRevealQueue();
			TheGameLogic->destroyObject(o);
		};
		// one object of template t for E at the first grid point the viewer does not see where it lands ON the map and fogged for the viewer
		// (the faction_fog grid; Mini F piece 02/11: the first fogged point can be in the border, off the map - unseen even by BRUTAL)
		auto spawnFogged = [&](const char *t, Coord3D *out) -> Object *
		{
			Region3D ext;
			TheTerrainLogic->getExtentIncludingBorder(&ext);
			for (int gy = 1; gy <= 9; ++gy)
				for (int gx = 1; gx <= 9; ++gx)
				{
					Coord3D c;
					c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
					c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
					c.z = 0.0f;
					if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) == CELLSHROUD_CLEAR)
						continue;
					Object *o = gxFxSpawn(t, enemy, c);
					if (o == nullptr)
						return nullptr;
					const ObjectShroudStatus st = o->getShroudedStatus(vi);
					if (!o->isOffMap() && st != OBJECTSHROUD_CLEAR && st != OBJECTSHROUD_PARTIAL_CLEAR)	// nothing of it shows (Codex rbs3-r1 P2: a footprint an own eye touches is seen)
					{
						*out = c;
						return o;
					}
					TheGameLogic->destroyObject(o);
				}
			return nullptr;
		};
		if (strcmp(which, "seen_count") == 0)
		{
			ObjectTypes *scorp = newInstance(ObjectTypes);
			scorp->addObjectType(AsciiString(gla[2]));
			ObjectTypes *truck = newInstance(ObjectTypes);
			truck->addObjectType(AsciiString("GLAVehicleBombTruck"));
			ObjectTypes *guard = newInstance(ObjectTypes);
			guard->addObjectType(AsciiString("ChinaInfantryRedguard"));
			auto count = [&](ObjectTypes *t) -> Int { return bank.seenCount(enemy, *t); };
			gxFxLine(ctx, which, "none", count(scorp), 0);			// nothing planted yet (E's own objects hidden)
			Object *s1 = put(gla[2], enemy, 0.0f);
			put(gla[2], enemy, 30.0f);
			gxFxLine(ctx, which, "visible", count(scorp), 2);			// two beside the viewer's own building
			Coord3D spot;
			Object *fogS = spawnFogged(gla[2], &spot);
			if (fogS != nullptr)
				spawned.push_back(fogS);
			if (fogS == nullptr || fogS->getShroudedStatus(vi) == OBJECTSHROUD_CLEAR)
				ctx.log("sense_test", "case=seen_count sub=fogged result=SKIP why=nothing_fogged");
			else
			{
				gxFxLine(ctx, which, "fogged", count(scorp), brutal ? 3 : 2);
				ThePartitionManager->doShroudReveal(spot.x, spot.y, 200.0f, viewer->getPlayerMask());	// granted: not an own look
				gxFxLine(ctx, which, "granted", count(scorp), brutal ? 3 : 2);
				ThePartitionManager->undoShroudReveal(spot.x, spot.y, 200.0f, viewer->getPlayerMask());
				Coord3D b = spot;
				b.y -= 30.0f;
				Object *eye = gxFxSpawn("AmericaVehicleHumvee", viewer, b);
				if (eye != nullptr)
					eye->handlePartitionCellMaintenance();
				gxFxLine(ctx, which, "own_eye", eye != nullptr ? count(scorp) : -1, 3);
				blind(eye);
			}
			const Int base = (fogS != nullptr && brutal) ? 3 : 2;		// what the viewer sees of the three now
			s1->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_STEALTHED), TRUE);
			gxFxLine(ctx, which, "stealthed", count(scorp), base - 1);
			s1->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), TRUE);
			gxFxLine(ctx, which, "detected", count(scorp), base);
			// a rider of an enclosing transport is out of the partition: nobody sees it (an EMPTY supply Chinook of E: the cargo case's)
			Object *tr = put("AmericaVehicleChinook", enemy, 60.0f);
			std::vector<Object *> inf;
			for (int k = 0; k < 3; ++k)
			{
				Object *o = put("ChinaInfantryRedguard", enemy, 90.0f + 15.0f * (Real)k);
				if (o != nullptr)
					inf.push_back(o);
			}
			if (tr == nullptr || tr->getContain() == nullptr || tr->getContain()->getContainCount() != 0u || inf.size() != 3u)
				ctx.log("sense_test", "case=seen_count sub=cargo_out result=SKIP why=template");
			else
			{
				gxFxLine(ctx, which, "cargo_out", count(guard), 3);
				for (size_t k = 0; k < inf.size(); ++k)
					tr->getContain()->addToContain(inf[k]);
				char ex[64];
				snprintf(ex, sizeof(ex), "contained=%u", (unsigned)tr->getContain()->getContainCount());
				gxFxLine(ctx, which, "cargo_in", count(guard), tr->getContain()->getContainCount() == 3u ? 0 : -1, ex);
			}
			// identity as seen: E's Bomb Truck shown as E's own Scorpion is a Scorpion to the viewer, never a truck; a NEUTRAL truck shown
			// as E's Scorpion is E's Scorpion to the viewer
			Object *t1 = put("GLAVehicleBombTruck", enemy, -30.0f);
			if (t1 == nullptr || t1->getStealth() == nullptr)
				ctx.log("sense_test", "case=seen_count sub=disguise_own result=SKIP why=template");
			else
			{
				t1->getStealth()->disguiseAsObject(s1);
				gxFxLine(ctx, which, "disguise_own", count(scorp), base + 1);
				gxFxLine(ctx, which, "disguise_own_truck", count(truck), 0);
				Coord3D p = at;
				p.y -= 60.0f;
				Object *t2 = gxFxSpawn("GLAVehicleBombTruck", ThePlayerList->getNeutralPlayer(), p);
				if (t2 == nullptr || t2->getStealth() == nullptr)
					ctx.log("sense_test", "case=seen_count sub=disguise_third result=SKIP why=template");
				else
				{
					spawned.push_back(t2);
					t2->getStealth()->disguiseAsObject(s1);
					gxFxLine(ctx, which, "disguise_third", count(scorp), base + 2);
				}
			}
			// a disguise is SEEN as its disguise whoever it imitates (Codex rbs2-r1 P2): E's truck shown as a NEUTRAL Humvee is one more
			// neutral Humvee to the viewer (the stock targeting filter called a disguise as a non-enemy "stealthed" and hid it)
			Player *neutral = ThePlayerList->getNeutralPlayer();
			ObjectTypes *hum = newInstance(ObjectTypes);
			hum->addObjectType(AsciiString("AmericaVehicleHumvee"));
			Coord3D pm = at;
			pm.x += 5000.0f;
			Object *model = neutral != nullptr ? gxFxSpawn("AmericaVehicleHumvee", neutral, pm) : nullptr;	// the disguise model, out of sight
			Object *t3 = put("GLAVehicleBombTruck", enemy, -90.0f);
			if (model == nullptr || t3 == nullptr || t3->getStealth() == nullptr)
				ctx.log("sense_test", "case=seen_count sub=disguise_neutral result=SKIP why=template");
			else
			{
				spawned.push_back(model);
				const Int h0 = bank.seenCount(neutral, *hum);
				t3->getStealth()->disguiseAsObject(model);
				// as a disguised Bomb Truck is in a game: StealthUpdate sets DISGUISED at the transition's half-way point and it stays until the reveal.
				// Without it the stock filter's disguise branch is never reached and the pre_r1 red arm could not bite (Mini F2 02/11)
				t3->setStatus(MAKE_OBJECT_STATUS_MASK2(OBJECT_STATUS_STEALTHED, OBJECT_STATUS_DISGUISED), TRUE);
				char exn[48];
				snprintf(exn, sizeof(exn), "before=%d", (int)h0);
				gxFxLine(ctx, which, "disguise_neutral", bank.seenCount(neutral, *hum) - h0, 1, exn);
			}
			deleteInstance(hum);
			// a garrison that hides itself (Codex rbs2-r1 P1; GarrisonContain::getApparentControllingPlayer): a civilian house holding only E's
			// undetected Jarmen Kell is still the neutral house it was to a non-ally, as InGameUI draws it. A second house with E's Rebel, no
			// stealth garrison, shows E. Then Kell is detected: the first house shows E too. Red arm: pre_r1 counted the real controller.
			const char *houseT = "GermanCotage01";
			ObjectTypes *houses = newInstance(ObjectTypes);
			houses->addObjectType(AsciiString(houseT));
			const Int h0 = bank.seenCount(neutral, *houses);	// any house of that type the map shows the viewer already
			Object *hide1 = put(houseT, neutral, 130.0f);
			Object *open1 = put(houseT, neutral, 190.0f);
			Object *kell = put("GLAInfantryJarmenKell", enemy, -150.0f);
			Object *rebel = put("GLAInfantryRebel", enemy, -180.0f);
			if (hide1 == nullptr || open1 == nullptr || kell == nullptr || rebel == nullptr || hide1->getContain() == nullptr || open1->getContain() == nullptr
				|| hide1->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR || open1->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR)
				ctx.log("sense_test", "case=seen_count sub=garrison result=SKIP why=template_or_not_visible");
			else
			{
				char exh[48];
				snprintf(exh, sizeof(exh), "before=%d", (int)h0);
				gxFxLine(ctx, which, "garrison_before", bank.seenCount(neutral, *houses) - h0, 2, exh);	// both houses, neutral and seen
				hide1->getContain()->addToContain(kell);
				open1->getContain()->addToContain(rebel);
				gxFxLine(ctx, which, "garrison_hidden_e", bank.seenCount(enemy, *houses), 1, exh);			// E shows only in the open house
				gxFxLine(ctx, which, "garrison_hidden_neutral", bank.seenCount(neutral, *houses) - h0, 1, exh);	// the hiding house is still neutral
				kell->setStatus(MAKE_OBJECT_STATUS_MASK(OBJECT_STATUS_DETECTED), TRUE);
				hide1->getContain()->recalcApparentControllingPlayer();
				gxFxLine(ctx, which, "garrison_detected_e", bank.seenCount(enemy, *houses), 2, exh);
				gxFxLine(ctx, which, "garrison_detected_neutral", bank.seenCount(neutral, *houses) - h0, 0, exh);
			}
			deleteInstance(houses);
			// "<Local Player>" names a different player on every peer, so the conditions refuse it in either seat (Codex sense-r1 P1) - asked through the
			// real condition entry with local moved to the very player the seat would then name, the split a real game would have. The control proves
			// the same call answers TRUE when no seat says "<Local Player>". Red arm: GX_TEST_SENSE_MUTANT=local_player_ok.
			auto askCond = [&](const char *aiName, UnsignedInt aiMask, const char *whoName, UnsignedInt whoMask) -> Int
			{
				Condition *c = newInstance(Condition)(Condition::AI_PLAYER_GX_SEEN_COUNT);
				c->getParameter(0)->friend_setString(AsciiString(aiName));
				c->getParameter(0)->friend_setInt((Int)aiMask);
				c->getParameter(1)->friend_setString(AsciiString(whoName));
				c->getParameter(1)->friend_setInt((Int)whoMask);
				c->getParameter(2)->friend_setInt(Parameter::GREATER_EQUAL);
				c->getParameter(3)->friend_setInt(1);
				c->getParameter(4)->friend_setString(AsciiString(gla[2]));	// E's two visible Scorpions
				const Bool answer = TheScriptConditions->evaluateCondition(c);
				deleteInstance(c);
				return answer ? 1 : 0;
			};
			Player *localWas = ThePlayerList->getLocalPlayer();
			gxFxLine(ctx, which, "local_control", askCond("viewer", (UnsignedInt)viewer->getPlayerMask(), "enemy", (UnsignedInt)enemy->getPlayerMask()), 1);
			ThePlayerList->setLocalPlayer(enemy);
			gxFxLine(ctx, which, "local_who", askCond("viewer", (UnsignedInt)viewer->getPlayerMask(), LOCAL_PLAYER, 0u), 0);
			ThePlayerList->setLocalPlayer(viewer);
			gxFxLine(ctx, which, "local_ai", askCond(LOCAL_PLAYER, 0u, "enemy", (UnsignedInt)enemy->getPlayerMask()), 0);
			ThePlayerList->setLocalPlayer(localWas);
			gxFxLine(ctx, which, "self", bank.seenCount(viewer, *scorp) == -1 ? 1 : 0, 1);		// refused (-1): the AI asking about itself
			deleteInstance(scorp);
			deleteInstance(truck);
			deleteInstance(guard);
			done();
			return;
		}
		// seen_value: the viewer's own inner perimeter (the stock skirmish area of its start, InnerPerimeter<start+1>)
		AsciiString an;
		an.format("%s%d", INNER_PERIMETER, viewer->getMpStartIndex() + 1);
		PolygonTrigger *area = TheTerrainLogic->getTriggerAreaByName(an);
		if (area == nullptr)
		{
			ctx.log("sense_test", "case=seen_value result=SKIP why=no_area");
			return;
		}
		auto value = [&]() -> Int { return bank.seenValueIn(enemy, area); };
		auto inArea = [&](const Object *o) -> Bool
		{
			ICoord3D ip;
			ip.x = REAL_TO_INT(o->getPosition()->x);
			ip.y = REAL_TO_INT(o->getPosition()->y);
			ip.z = 0;
			return area->pointInTrigger(ip);
		};
		gxFxLine(ctx, which, "none", value(), 0);
		const Int crus = gxFxCost("AmericaTankCrusader", enemy);
		Object *c1 = put("AmericaTankCrusader", enemy, 0.0f);
		Object *c2 = put("AmericaTankCrusader", enemy, 30.0f);
		if (c1 == nullptr || c2 == nullptr || crus <= 0 || !inArea(c1) || !inArea(c2))
		{
			ctx.log("sense_test", "case=seen_value sub=inside result=SKIP why=not_in_area");
			done();
			return;
		}
		gxFxLine(ctx, which, "inside", value(), 2 * crus);
		Object *dz = put("ChinaVehicleDozer", enemy, 60.0f);
		gxFxLine(ctx, which, "unarmed", dz != nullptr && inArea(dz) ? value() : -1, 2 * crus);		// a dozer is no threat
		// E's hidden production discount (an unseen Oil Refinery: -10% on vehicles) changes nothing the viewer sees: the LIST price counts
		// (Codex rbs2-r1 P1 - calcCostToBuild(owner) moved the value with no visible change)
		KindOfMaskType veh;
		veh.set(KINDOF_VEHICLE);
		enemy->addKindOfProductionCostChange(veh, -0.10f);
		gxFxLine(ctx, which, "discount", value(), 2 * crus);
		enemy->removeKindOfProductionCostChange(veh, -0.10f);
		Coord3D spot;
		Object *far1 = spawnFogged("AmericaTankCrusader", &spot);
		if (far1 != nullptr)
			spawned.push_back(far1);
		if (far1 == nullptr || inArea(far1))
			ctx.log("sense_test", "case=seen_value sub=outside result=SKIP why=no_spot_outside");
		else
			gxFxLine(ctx, which, "outside", value(), 2 * crus);			// fogged for FAIR, outside the area for BRUTAL: neither counts
		gxFxLine(ctx, which, "self", bank.seenValueIn(viewer, area) == -1 ? 1 : 0, 1);	// refused (-1)
		gxFxLine(ctx, which, "no_area", bank.seenValueIn(enemy, nullptr) == -1 ? 1 : 0, 1);	// refused (-1)
		done();
		return;
	}
	if (strcmp(which, "own_sight") == 0)
	{
		// GeneralsX 02/11/2026 rb-sense-2 (gx-handoff ai/rulebank/data/HANDBACK_rb-data-15.md section 5: stock USA's Spy Satellite fed the
		// sensor at f28): the sight path counts only what the viewer's OWN units and buildings see. Three GLA templates of E at a spot the
		// viewer does not see; then each reveal that is NOT an own unit's look - a granted reveal (doShroudReveal for the viewer, as a map or
		// script reveal does), a viewer-owned Spy Satellite ping, an ALLY's unit - must add no sighting and no value for a FAIR viewer (want
		// 0: nothing of the viewer's own looks there) and leave a Random E unknown; then a viewer-owned unit beside them: 3 sightings, their
		// build costs, E known. BRUTAL sees the whole map, so every sub counts for it. Every reveal is taken back before the next sub (the
		// granted one undone, the others' looks dropped and the queued undos flushed), so each starts from the fogged spot.
		const Int vi = viewer->getPlayerIndex();
		const Bool brutal = gxRuleTierOf(viewer) == gxrb::TIER_BRUTAL;
		const GXRuleSense &sense = ctx.sense();
		auto sightings = [&](const char *onlyTmpl) -> Int
		{
			std::vector<std::vector<gxrb::Sighting> > out((size_t)ThePlayerList->getPlayerCount());
			const Int ei = enemy->getPlayerIndex();
			if (ei < 0 || ei >= MAX_PLAYER_COUNT || (size_t)ei >= out.size())
				return -1;
			sense.collectSightings(gxPlayerBit(ei), &out);
			Int n = 0;
			for (size_t k = 0; k < out[(size_t)ei].size(); ++k)
				if (onlyTmpl == nullptr || out[(size_t)ei][k].tmpl == onlyTmpl)
					++n;
			return n;
		};
		auto blind = [&](Object *o)
		{
			if (o == nullptr)
				return;
			o->setShroudClearingRange(0.0f);
			o->handlePartitionCellMaintenance();		// unlook (queued) and no new look
			ThePartitionManager->processEntirePendingUndoShroudRevealQueue();
			TheGameLogic->destroyObject(o);
		};
		auto eye = [&](const char *t, Player *owner, const Coord3D &p, Real range) -> Object *
		{
			Object *o = gxFxSpawn(t, owner, p);
			if (o != nullptr)
			{
				if (range > 0.0f)
					o->setShroudClearingRange(range);
				o->handlePartitionCellMaintenance();	// look now, as its first partition update would
				// its spawn-time look (full range) was unlooked by that call, but an unlook is QUEUED: flush it, so the cells clear now are
				// exactly this look's (Mini F piece 02/11: cells_exact_min read 78 of 81 cells wrong, and pre_r1's edge saw a whole-CC reveal)
				ThePartitionManager->processEntirePendingUndoShroudRevealQueue();
			}
			return o;
		};
		// CELLS EXACT: markLookCells must mark exactly the cells the eye's look turned CLEAR (doShroudReveal's), measured around the eye:
		// the cells within its reach + 2 that were not CLEAR before it looked; got = how many of them disagree (marked but still not CLEAR,
		// or CLEAR now but not marked), -1 when no such cell exists (no evidence)
		struct GXCellBox { Int x0, y0, x1, y1; std::vector<UnsignedByte> before; };
		auto boxBefore = [&](const Coord3D &c, Real range) -> GXCellBox
		{
			GXCellBox b;
			Int cx, cy;
			ThePartitionManager->worldToCell(c.x, c.y, &cx, &cy);
			const Int rr = ThePartitionManager->worldToCellDist(range) + 2;
			b.x0 = std::max(0, cx - rr); b.y0 = std::max(0, cy - rr);
			b.x1 = std::min(ThePartitionManager->getCellCountX() - 1, cx + rr); b.y1 = std::min(ThePartitionManager->getCellCountY() - 1, cy + rr);
			for (Int y = b.y0; y <= b.y1; ++y)
				for (Int x = b.x0; x <= b.x1; ++x)
					b.before.push_back(ThePartitionManager->getShroudStatusForPlayer(vi, x, y) == CELLSHROUD_CLEAR ? 1 : 0);
			return b;
		};
		auto cellsExact = [&](const GXCellBox &b, const Object *eyeObj, const char *sub)
		{
			const SightingInfo *look = eyeObj != nullptr ? eyeObj->friend_getPartitionLastLook() : nullptr;
			if (look == nullptr || look->isInvalid())
			{
				gxFxLine(ctx, which, sub, -1, 0, "why=no_look");
				return;
			}
			std::vector<UnsignedByte> mark;
			ThePartitionManager->markLookCells(*look, mark);
			Int cells = 0, bad = 0, k = 0;
			const Int cxN = ThePartitionManager->getCellCountX();
			for (Int y = b.y0; y <= b.y1; ++y)
				for (Int x = b.x0; x <= b.x1; ++x, ++k)
				{
					if (b.before[(size_t)k] != 0)
						continue;		// already clear: this look cannot show on it
					++cells;
					const Bool nowClear = ThePartitionManager->getShroudStatusForPlayer(vi, x, y) == CELLSHROUD_CLEAR;
					const Bool marked = mark[(size_t)(y * cxN + x)] != 0;
					bad += nowClear != marked ? 1 : 0;
				}
			char ex2[96];
			snprintf(ex2, sizeof(ex2), "cells=%d range=%d", (int)cells, (int)look->m_howFar);
			gxFxLine(ctx, which, sub, cells > 0 ? bad : -1, 0, ex2);
		};
		Region3D ext;
		TheTerrainLogic->getExtentIncludingBorder(&ext);
		const Coord3D *a = anchor->getPosition();
		Coord3D spot;
		Real r = 0.0f;
		Int v0 = 0;
		Bool placed = FALSE;
		std::vector<Object *> planted;
		for (int gy = 1; gy <= 9 && !placed; ++gy)
			for (int gx = 1; gx <= 9 && !placed; ++gx)
			{
				Coord3D c;
				c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
				c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
				c.z = 0.0f;
				if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) == CELLSHROUD_CLEAR)
					continue;
				const Real dx = c.x - a->x, dy = c.y - a->y;
				r = sqrtf(dx * dx + dy * dy) + 300.0f;
				v0 = sense.enemyValueNear(anchor, r);		// before the three exist
				Bool fogged = TRUE;
				for (int k = 0; k < 3; ++k)
				{
					Coord3D p = c;
					p.y += 20.0f * (Real)k;
					Object *o = gxFxSpawn(gla[k], enemy, p);
					if (o == nullptr)
						fogged = FALSE;
					else
					{
						spawned.push_back(o);
						planted.push_back(o);
						const ObjectShroudStatus st = o->getShroudedStatus(vi);
						fogged = fogged && !o->isOffMap() && st != OBJECTSHROUD_CLEAR && st != OBJECTSHROUD_PARTIAL_CLEAR;
					}
				}
				if (fogged)
				{
					placed = TRUE;
					spot = c;
				}
				else
				{
					done();
					planted.clear();
				}
			}
		if (!placed)
		{
			ctx.log("sense_test", "case=own_sight sub=fogged result=SKIP why=nothing_fogged");
			return;
		}
		// the want: the list prices of the ARMED ones only - an unarmed template is worth 0 to enemyValueNear (Mini F piece 02/11: the base
		// GLAVehicleTechnical carries no weapon of its own, 750 not 1250)
		Int sum = 0;
		for (int k = 0; k < 3; ++k)
		{
			const ThingTemplate *tt = TheThingFactory->findTemplate(AsciiString(gla[k]), FALSE);
			if (tt != nullptr && tt->canPossiblyHaveAnyWeapon())
				sum += gxFxCost(gla[k], enemy);
		}
		char ex[96];
		snprintf(ex, sizeof(ex), "enemy_lobby=%s tier=%s", random ? "random" : "known", brutal ? "BRUTAL" : "FAIR");
		auto lines = [&](const char *sub, Bool ownLook)
		{
			char s[48];
			snprintf(s, sizeof(s), "%s_sightings", sub);
			gxFxLine(ctx, which, s, sightings(nullptr), (brutal || ownLook) ? 3 : 0, ex);
			snprintf(s, sizeof(s), "%s_value", sub);
			gxFxLine(ctx, which, s, sense.enemyValueNear(anchor, r) - v0, (brutal || ownLook) ? sum : 0, ex);
		};
		lines("fogged", FALSE);
		Coord3D beside = spot;
		beside.y -= 30.0f;
		// a granted reveal: cells cleared for the viewer with no object of its own looking
		ThePartitionManager->doShroudReveal(spot.x, spot.y + 20.0f, 200.0f, viewer->getPlayerMask());
		if (planted[0]->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR)
			ctx.log("sense_test", "case=own_sight sub=granted result=SKIP why=not_revealed");
		else
			lines("granted", FALSE);
		ThePartitionManager->undoShroudReveal(spot.x, spot.y + 20.0f, 200.0f, viewer->getPlayerMask());
		// the viewer's own Spy Satellite ping (system.ini SpySatellitePing: NO_COLLIDE IMMOBILE UNATTACKABLE INERT - not a unit)
		Object *ping = eye("SpySatellitePing", viewer, spot, 300.0f);
		if (ping == nullptr)
			ctx.log("sense_test", "case=own_sight sub=ping result=SKIP why=template");
		else if (planted[0]->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR)
			ctx.log("sense_test", "case=own_sight sub=ping result=SKIP why=not_revealed");
		else
			lines("ping", FALSE);
		blind(ping);
		// an ALLY's unit: its look clears the cells for the viewer too (Object::look's allies mask); needs a 2v2 (GX_TEAMS)
		Player *ally = gxFxPlayer(viewer, ALLIES, 0);
		Object *allyEye = ally != nullptr ? eye("AmericaVehicleHumvee", ally, beside, 0.0f) : nullptr;
		if (ally == nullptr)
			ctx.log("sense_test", "case=own_sight sub=ally result=SKIP why=no_ally");
		else if (allyEye == nullptr || planted[0]->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR)
			ctx.log("sense_test", "case=own_sight sub=ally result=SKIP why=not_revealed");
		else
			lines("ally", FALSE);
		blind(allyEye);
		bank.senseSampleNowForTest();
		gxFxLine(ctx, which, "faction_after_reveals", gxFxAsk(bank, enemy, names), brutal ? known : lobby, ex);
		// the viewer's own unit beside them: what a human with that unit sees
		const GXCellBox ownBox = boxBefore(beside, 600.0f);
		Object *ownEye = eye("AmericaVehicleHumvee", viewer, beside, 0.0f);
		cellsExact(ownBox, ownEye, "cells_exact");
		if (ownEye == nullptr || planted[0]->getShroudedStatus(vi) != OBJECTSHROUD_CLEAR)
			ctx.log("sense_test", "case=own_sight sub=own result=SKIP why=not_revealed");
		else
		{
			lines("own", TRUE);
			bank.senseSampleNowForTest();
			gxFxLine(ctx, which, "faction_own", gxFxAsk(bank, enemy, names), known, ex);
		}
		blind(ownEye);
		done();
		// EDGE: an own eye whose look clears only cells at the edge of a big enemy building - two or more cells from the building's own
		// centre cell - still sees the building (a human sees it: the engine draws an object when ANY cell it touches is clear)
		if (brutal)
		{
			ctx.log("sense_test", "case=own_sight sub=edge result=SKIP why=brutal_whole_map");
			return;
		}
		const char *big[3] = { "GLACommandCenter", "AmericaCommandCenter", "ChinaCommandCenter" };
		std::vector<Coord3D> spots(1, spot);		// the planted spot first, then every other grid point the viewer does not see
		for (int gy = 1; gy <= 9; ++gy)
			for (int gx = 1; gx <= 9; ++gx)
			{
				Coord3D c;
				c.x = ext.lo.x + (ext.hi.x - ext.lo.x) * (Real)gx / 10.0f;
				c.y = ext.lo.y + (ext.hi.y - ext.lo.y) * (Real)gy / 10.0f;
				c.z = 0.0f;
				if (ThePartitionManager->getShroudStatusForPlayer(vi, &c) != CELLSHROUD_CLEAR)
					spots.push_back(c);
			}
		Int unshownAlignments = 0;
		for (int k = 0; k < 3 * (int)spots.size(); ++k)
		{
			Object *cc = gxFxSpawn(big[k % 3], enemy, spots[(size_t)(k / 3)]);
			if (cc == nullptr)
				continue;
			spawned.push_back(cc);
			Coord3D ep = *cc->getPosition();
			ep.x += cc->getGeometryInfo().getMajorRadius() - 3.0f;	// inside its footprint, at the +x edge
			Int ccx, ccy, ecx, ecy;
			ThePartitionManager->worldToCell(cc->getPosition()->x, cc->getPosition()->y, &ccx, &ccy);
			ThePartitionManager->worldToCell(ep.x, ep.y, &ecx, &ecy);
			const Int dcell = std::max(std::abs(ecx - ccx), std::abs(ecy - ccy));
			const Int before = sightings(big[k % 3]);
			if (dcell < 2 || before != 0 || cc->isOffMap())
			{
				done();
				continue;
			}
			const GXCellBox edgeBox = boxBefore(ep, 40.0f);
			Object *edgeEye = eye("AmericaInfantryRanger", viewer, ep, 1.0f);	// the smallest look: one cell and its neighbours
			// the want is the GAME's answer, not a placement assumption (w-own-2v2 02/11 read 0 at an alignment where this look may clear no cell
			// the building touches): getShroudedStatus is what draws it for a human. Where the game does not show it the sensor must say 0 too, and
			// the next alignment is tried; where it does, the sensor must see it.
			const ObjectShroudStatus st = cc->getShroudedStatus(vi);
			const Bool gameShows = st == OBJECTSHROUD_CLEAR || st == OBJECTSHROUD_PARTIAL_CLEAR;
			const Int sensed = edgeEye != nullptr ? sightings(big[k % 3]) : -1;
			char ex2[160];
			snprintf(ex2, sizeof(ex2), "%s tmpl=%s cells_from_centre=%d shroud=%d unshown_before=%d", ex, big[k % 3], (int)dcell, (int)st, (int)unshownAlignments);
			if (!gameShows)
			{
				if (sensed != 0)
					gxFxLine(ctx, which, "edge_leak", sensed, 0, ex2);	// the sensor saw what the game does not show: never allowed, so never in a judge's want
				++unshownAlignments;
				blind(edgeEye);
				done();
				continue;
			}
			cellsExact(edgeBox, edgeEye, "cells_exact_min");
			gxFxLine(ctx, which, "edge", sensed, 1, ex2);
			blind(edgeEye);
			done();
			return;
		}
		ctx.log("sense_test", "case=own_sight sub=edge result=SKIP why=footprint_small");
		return;
	}
	snprintf(kv, sizeof(kv), "result=SKIP why=unknown_case case=%s", which);
	ctx.log("sense_test", kv);
}
#endif
