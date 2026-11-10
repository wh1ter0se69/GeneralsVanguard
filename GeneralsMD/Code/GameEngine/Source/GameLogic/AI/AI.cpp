/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// AI.cpp
// The Artificial Intelligence system
// Author: Michael S. Booth, November 2000
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDebug.h"
#include "Common/GameState.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"

#include "GameLogic/AI.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Weapon.h"

extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);


///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE CLASS ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
void TAiData::addSideInfo(AISideInfo *infoToAdd)
{
	infoToAdd->m_next = m_sideInfo;
	m_sideInfo = infoToAdd;
}

void TAiData::addFactionBuildList(AISideBuildList *buildList)
{

	AISideBuildList *info = m_sideBuildLists;
	while (info) {
		if (buildList->m_side == info->m_side) {
			deleteInstance(info->m_buildList);
			info->m_buildList = buildList->m_buildList;
			// GeneralsX 04/11/2026 base-layout: the old wrapper stays, so the replacing list's keys come with its entries (a list without them is retail again)
			info->m_gxTurnLayout = buildList->m_gxTurnLayout;
			info->m_gxFrontDoors = buildList->m_gxFrontDoors;
			info->m_gxRingRadius = buildList->m_gxRingRadius;
			buildList->m_buildList = nullptr;
			buildList->m_next = nullptr;
			deleteInstance(buildList);
			return;
		}
		info = info->m_next;
	}
	buildList->m_next = m_sideBuildLists;
	m_sideBuildLists = buildList;
}

TAiData::~TAiData()
{
	AISideInfo *info = m_sideInfo;
	m_sideInfo = nullptr;
	while (info) {
		AISideInfo *cur = info;
		info = info->m_next;
		deleteInstance(cur);
	}

	AISideBuildList *build = m_sideBuildLists;
	m_sideBuildLists = nullptr;
	while (build) {
		AISideBuildList *cur = build;
		build = build->m_next;
		deleteInstance(cur);
	}

}

///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE CLASS ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
AISideBuildList::AISideBuildList( AsciiString side ) :
	m_side(side),
	m_buildList(nullptr),
	m_next(nullptr),
	m_gxTurnLayout(FALSE),
	m_gxFrontDoors(FALSE),
	m_gxRingRadius(0.0f)
{
}

AISideBuildList::~AISideBuildList()
{
	deleteInstance(m_buildList); // note - deletes all in the list.
	m_buildList = nullptr;
}

void AISideBuildList::addInfo(BuildListInfo *info)
{
	// Add to the end of the list.
	if (m_buildList == nullptr) {
		m_buildList = info;
	} else {
		BuildListInfo *cur = m_buildList;
		while (cur && cur->getNext()) {
			cur = cur->getNext();
		}
		DEBUG_ASSERTCRASH(cur && cur->getNext()==nullptr, ("Logic error."));
		cur->setNextBuildList(info);
	}
	info->setNextBuildList(nullptr); // should be at the end of the list.
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
static const FieldParse TheAIFieldParseTable[] =
{

	{ "StructureSeconds",				INI::parseReal,nullptr,		offsetof( TAiData, m_structureSeconds ) },
	{ "TeamSeconds",						INI::parseReal,nullptr,		offsetof( TAiData, m_teamSeconds ) },
	{ "Wealthy",								INI::parseInt,nullptr,			offsetof( TAiData, m_resourcesWealthy ) },
	{ "Poor",										INI::parseInt,nullptr,		  offsetof( TAiData, m_resourcesPoor ) },
	{ "ForceIdleMSEC",					GX_PARSE_GO_DURATION,nullptr,offsetof( TAiData, m_forceIdleFramesCount )	},
	{ "StructuresWealthyRate",	INI::parseReal,nullptr,		offsetof( TAiData, m_structuresWealthyMod ) },
	{ "TeamsWealthyRate",				INI::parseReal,nullptr,		offsetof( TAiData, m_teamWealthyMod ) },
	{ "StructuresPoorRate",			INI::parseReal,nullptr,		offsetof( TAiData, m_structuresPoorMod ) },
	{ "TeamsPoorRate",					INI::parseReal,nullptr,		offsetof( TAiData, m_teamPoorMod ) },
	{ "TeamResourcesToStart",		INI::parseReal,nullptr,		offsetof( TAiData, m_teamResourcesToBuild ) },
	{ "GuardInnerModifierAI",		INI::parseReal,nullptr,		offsetof( TAiData, m_guardInnerModifierAI ) },
	{ "GuardOuterModifierAI",		INI::parseReal,nullptr,		offsetof( TAiData, m_guardOuterModifierAI ) },
	{ "GuardInnerModifierHuman",INI::parseReal,nullptr,		offsetof( TAiData, m_guardInnerModifierHuman ) },
	{ "GuardOuterModifierHuman",INI::parseReal,nullptr,		offsetof( TAiData, m_guardOuterModifierHuman ) },
	{ "GuardChaseUnitsDuration",				INI::parseDurationUnsignedInt,nullptr,		offsetof( TAiData, m_guardChaseUnitFrames ) },
	{ "GuardEnemyScanRate",				INI::parseDurationUnsignedInt,nullptr,		offsetof( TAiData, m_guardEnemyScanRate ) },
	{ "GuardEnemyReturnScanRate",				INI::parseDurationUnsignedInt,nullptr,		offsetof( TAiData, m_guardEnemyReturnScanRate ) },
	{ "SkirmishGroupFudgeDistance",	INI::parseReal,nullptr,		offsetof( TAiData, m_skirmishGroupFudgeValue ) },

	{ "RepulsedDistance",				INI::parseReal,nullptr,		offsetof( TAiData, m_repulsedDistance ) },
	{ "EnableRepulsors",				INI::parseBool,nullptr,		offsetof( TAiData, m_enableRepulsors ) },

	{	"AlertRangeModifier",			INI::parseReal,nullptr,		offsetof( TAiData, m_alertRangeModifier)	},
	{	"AggressiveRangeModifier",INI::parseReal,nullptr,		offsetof( TAiData, m_aggressiveRangeModifier)	},

	{ "ForceSkirmishAI",				INI::parseBool,nullptr,		offsetof( TAiData, m_forceSkirmishAI ) },
	{ "RotateSkirmishBases",		INI::parseBool,nullptr,		offsetof( TAiData, m_rotateSkirmishBases ) },

	{ "AttackUsesLineOfSight",	INI::parseBool,nullptr,		offsetof( TAiData, m_attackUsesLineOfSight ) },
	{ "AttackIgnoreInsignificantBuildings",	INI::parseBool,nullptr,		offsetof( TAiData, m_attackIgnoreInsignificantBuildings ) },


	{ "AttackPriorityDistanceModifier", INI::parseReal,nullptr, offsetof( TAiData, m_attackPriorityDistanceModifier) },
 	{ "MaxRecruitRadius",				INI::parseReal,nullptr,		offsetof( TAiData, m_maxRecruitDistance ) },
	{ "SkirmishBaseDefenseExtraDistance",	INI::parseReal,nullptr,	offsetof( TAiData, m_skirmishBaseDefenseExtraDistance ) },

 	{ "WallHeight",							INI::parseReal,nullptr,		offsetof( TAiData, m_wallHeight ) },

	{ "SideInfo",			AI::parseSideInfo,			nullptr, 0 },


	{ "SkirmishBuildList",			AI::parseSkirmishBuildList,			nullptr, 0 },


 	{ "MinInfantryForGroup",		INI::parseInt,nullptr,			offsetof( TAiData, m_minInfantryForGroup ) },
 	{ "MinVehiclesForGroup",		INI::parseInt,nullptr,			offsetof( TAiData, m_minVehiclesForGroup ) },

 	{ "MinDistanceForGroup",		INI::parseReal,nullptr,			offsetof( TAiData, m_minDistanceForGroup ) },
 	{ "DistanceRequiresGroup",	INI::parseReal,nullptr,			offsetof( TAiData, m_distanceRequiresGroup ) },
 	{ "MinClumpDensity",				INI::parseReal,nullptr,			offsetof( TAiData, m_minClumpDensity ) },

 	{ "InfantryPathfindDiameter",		INI::parseInt,nullptr,			offsetof( TAiData, m_infantryPathfindDiameter ) },
 	{ "VehiclePathfindDiameter",		INI::parseInt,nullptr,			offsetof( TAiData, m_vehiclePathfindDiameter ) },
 	{ "RebuildDelayTimeSeconds",		INI::parseInt,nullptr,			offsetof( TAiData, m_rebuildDelaySeconds ) },
 	{ "SupplyCenterSafeRadius",			INI::parseReal,nullptr,			offsetof( TAiData, m_supplyCenterSafeRadius ) },

 	{ "AIDozerBoredRadiusModifier",	INI::parseReal,nullptr,			offsetof( TAiData, m_aiDozerBoredRadiusModifier ) },
 	{ "AICrushesInfantry",	INI::parseBool,nullptr,			offsetof( TAiData, m_aiCrushesInfantry ) },

 	{ "MaxRetaliationDistance",	INI::parseReal,nullptr,			offsetof( TAiData, m_maxRetaliateDistance ) },
 	{ "RetaliationFriendsRadius",	INI::parseReal,nullptr,			offsetof( TAiData, m_retaliateFriendsRadius ) },


	{ nullptr,					nullptr,						nullptr,						0 }

};

void AI::parseSideInfo(INI *ini, void *instance, void* /*store*/, const void* /*userData*/)
{
	const char* c = ini->getNextToken();
	AsciiString side(c);

	static const FieldParse myFieldParse[] =
		{
			{ "ResourceGatherersEasy",				INI::parseInt,						nullptr, offsetof( AISideInfo, m_easy ) },
			{ "ResourceGatherersNormal",			INI::parseInt,						nullptr, offsetof( AISideInfo, m_normal ) },
      { "ResourceGatherersHard",				INI::parseInt,						nullptr, offsetof( AISideInfo, m_hard ) },
      { "BaseDefenseStructure1",				INI::parseAsciiString,		nullptr, offsetof( AISideInfo, m_baseDefenseStructure1 ) },
			{ "SkillSet1",										AI::parseSkillSet,				nullptr, offsetof( AISideInfo, m_skillSet1 ) },
			{ "SkillSet2",										AI::parseSkillSet,				nullptr, offsetof( AISideInfo, m_skillSet2 ) },
			{ "SkillSet3",										AI::parseSkillSet,				nullptr, offsetof( AISideInfo, m_skillSet3 ) },
			{ "SkillSet4",										AI::parseSkillSet,				nullptr, offsetof( AISideInfo, m_skillSet4 ) },
			{ "SkillSet5",										AI::parseSkillSet,				nullptr, offsetof( AISideInfo, m_skillSet5 ) },
			{ nullptr,							nullptr,											nullptr, 0 }
		};

	AISideInfo *resourceInfo = ((TAiData*)instance)->m_sideInfo;
	while (resourceInfo) {
		if (side == resourceInfo->m_side) {
			break;
		}
		resourceInfo = resourceInfo->m_next;
	}
	if (resourceInfo==nullptr)
	{
		resourceInfo = newInstance(AISideInfo);
		((TAiData*)instance)->addSideInfo(resourceInfo);
	}
	resourceInfo->m_side = side;
	ini->initFromINI(resourceInfo, myFieldParse);

}

void AI::parseSkillSet(INI *ini, void *instance, void* store, const void* /*userData*/)
{
	static const FieldParse myFieldParse[] =
		{
			{ "Science",											AI::parseScience,					nullptr, 0 },
			{ nullptr,							nullptr,											nullptr, 0 }
		};

	TSkillSet *skillset = ((TSkillSet*)store);
	skillset->m_numSkills = 0;
	ini->initFromINI(store, myFieldParse);
}

void AI::parseScience(INI *ini, void *instance, void* /*store*/, const void* /*userData*/)
{
	TSkillSet *skillset = ((TSkillSet*)instance);
	if (skillset->m_numSkills>=MAX_AI_UPGRADES) {
#ifdef DEBUG_CRASHING
		const char* c = ini->getNextToken();
		DEBUG_CRASH(("Too many SCIENCE skills in skillset. Skill = %s, max is %d", c, MAX_AI_UPGRADES));
#endif
		return;
	}
	skillset->m_skills[skillset->m_numSkills] = SCIENCE_INVALID;
	INI::parseScience(ini, instance, skillset->m_skills+skillset->m_numSkills, nullptr);
	ScienceType science = skillset->m_skills[skillset->m_numSkills];
	if (science != SCIENCE_INVALID) {
		if (TheScienceStore->getSciencePurchaseCost(science)==0) {
			DEBUG_CRASH(("Science %s is not purchaseable, can't be bought.",
				TheScienceStore->getInternalNameForScience(science).str()));
			return;
		}
		skillset->m_numSkills++;
	}
}

void AI::parseSkirmishBuildList(INI *ini, void *instance, void* /*store*/, const void* /*userData*/)
{
	const char* c = ini->getNextToken();
	AsciiString faction(c);

	static const FieldParse myFieldParse[] =
		{
			{ "Structure",			BuildListInfo::parseStructure,			nullptr, 0 },
			{ "GXTurnLayout",		INI::parseBool,											nullptr, offsetof( AISideBuildList, m_gxTurnLayout ) },	// GeneralsX 04/11/2026 base-layout
			{ "GXFrontDoors",		INI::parseBool,											nullptr, offsetof( AISideBuildList, m_gxFrontDoors ) },	// GeneralsX 04/11/2026 base-layout
			{ "GXRingRadius",		INI::parseReal,											nullptr, offsetof( AISideBuildList, m_gxRingRadius ) },	// GeneralsX 05/11/2026 base-layout
			{ nullptr,							nullptr,											nullptr, 0 }
		};

	AISideBuildList *build = newInstance(AISideBuildList)(faction);
	ini->initFromINI(build, myFieldParse);
	((TAiData*)instance)->addFactionBuildList(build);

}

//--------------------------------------------------------------------------------------------------------

/// The AI system singleton
AI *TheAI = nullptr;


/**
 * Constructor for the AI system
 */
AI::AI()
{
	m_aiData = NEW TAiData;
	m_pathfinder = NEW Pathfinder;
	m_nextFormationID = NO_FORMATION_ID;
}

/**
 * Initialize the AI system
 */
void AI::init()
{
	m_nextGroupID = 0;
}

/**
 * Reset the AI system in preparation for a new map
 */
void AI::reset()
{
	m_pathfinder->reset();
	while (m_aiData && m_aiData->m_next) {
		TAiData *cur = m_aiData;
		m_aiData = m_aiData->m_next;
		delete cur;
	}

#if RETAIL_COMPATIBLE_AIGROUP
	while (!m_groupList.empty())
	{
		AIGroup *groupToRemove = m_groupList.front();
		if (groupToRemove)
		{
			destroyGroup(groupToRemove);
		}
		else
		{
			m_groupList.pop_front(); // nullptr group, just kill from list.  Shouldn't really happen, but just in case.
			m_groupIndex.erase(nullptr);
		}
	}
#else
	DEBUG_ASSERTCRASH(m_groupList.empty(), ("AI::m_groupList is expected empty already"));

	m_groupList.clear(); // Clear just in case...
	m_groupIndex.clear();
#endif

	m_nextGroupID = 0;
	m_nextFormationID = NO_FORMATION_ID;
	getNextFormationID(); // increment once past NO_FORMATION_ID.  jba.
}

/**
 * Update the AI system
 */
void AI::update()
{
	// Do pathfinding.
	m_pathfinder->processPathfindQueue();

	// run player updates
	{
		ThePlayerList->UPDATE();
	}

}

/**
 * Destroy the AI system
 */
AI::~AI()
{
	delete m_pathfinder;
	m_pathfinder = nullptr;

	while (m_aiData)
	{
		TAiData *cur = m_aiData;
		m_aiData = m_aiData->m_next;
		delete cur;
	}
}


void AI::newOverride()
{
	TAiData *cur = m_aiData;
	m_aiData = NEW TAiData;
	*m_aiData = *cur;
	m_aiData->m_sideInfo = nullptr;
	AISideInfo *info = cur->m_sideInfo;
	while (info) {
		AISideInfo *newInfo = newInstance(AISideInfo);
		*newInfo = *info;
		newInfo->m_next = nullptr;
		addSideInfo(newInfo);
		info = info->m_next;
	}
	m_aiData->m_sideBuildLists = nullptr;
	AISideBuildList *build = cur->m_sideBuildLists;
	while (build) {
		AISideBuildList *newbuild = newInstance(AISideBuildList)(build->m_side);
		newbuild->m_next = nullptr;
		newbuild->m_buildList = build->m_buildList->duplicate();
		newbuild->m_gxTurnLayout = build->m_gxTurnLayout;	// GeneralsX 04/11/2026 base-layout: a map's override keeps the keys
		newbuild->m_gxFrontDoors = build->m_gxFrontDoors;
		newbuild->m_gxRingRadius = build->m_gxRingRadius;
		m_aiData->addFactionBuildList(newbuild);
		build = build->m_next;
	}
	m_aiData->m_next = cur;
}

void AI::addSideInfo(AISideInfo *infoToAdd)
{
	m_aiData->addSideInfo(infoToAdd);

}

//-------------------------------------------------------------------------------------------------
/** Parse GameData entry */
//-------------------------------------------------------------------------------------------------
void AI::parseAiDataDefinition( INI* ini )
{
	if( TheAI )
	{

		//
		// if the type of loading we're doing creates override data, we need to
		// be loading into a new override item
		//
		if( ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES )
			TheAI->newOverride();

		// parse the ini weapon definition
		ini->initFromINI( TheAI->m_aiData, TheAIFieldParseTable );

	}
}




//--------------------------------------------------------------------------------------------------------
/**
 * Create a new AI Group
 */
AIGroupPtr AI::createGroup()
{
	// create a new instance
#if RETAIL_COMPATIBLE_AIGROUP
	AIGroup *group = newInstance(AIGroup);
#else
	AIGroupPtr group;
	group.Assign_No_Add_Ref(newInstance(AIGroup));
#endif

	// add it to the list
//	DEBUG_LOG(("***AIGROUP %x is being added to m_groupList.", group ));
#if RETAIL_COMPATIBLE_AIGROUP
	m_groupList.push_back( group );
	m_groupIndex[group] = std::prev( m_groupList.end() );
#else
	m_groupList.push_back( group.Peek() );
	m_groupIndex[group.Peek()] = std::prev( m_groupList.end() );
#endif

	return group;
}

/**
 * Destroy the given AI Group
 */
void AI::destroyGroup( AIGroup *group )
{
	// GeneralsX: was std::find over m_groupList; the index answers the same question in O(1)
	std::hash_map<AIGroup *, std::list<AIGroup *>::iterator>::iterator found = m_groupIndex.find( group );

	// make sure group is actually in the list
	if (found == m_groupIndex.end())
		return;
	std::list<AIGroup *>::iterator i = found->second;
	m_groupIndex.erase( found );

	DEBUG_ASSERTCRASH(group != nullptr, ("A null group made its way into the AIGroup list.. jkmcd"));

	// remove it
//	DEBUG_LOG(("***AIGROUP %x is being removed from m_groupList.", group ));
	m_groupList.erase( i );

	// destroy group
	deleteInstance(group);
}

/**
 * Given an ID, return the associated AIGroup
 */
AIGroup *AI::findGroup( UnsignedInt id )
{
	/// @todo Optimize this (MSB)
	std::list<AIGroup *>::iterator i;
	for( i=m_groupList.begin(); i!=m_groupList.end(); ++i )
		if ((*i)->getID() == id)
			return (*i);

	return nullptr;
}

//--------------------------------------------------------------------------------------------------------
/**
 * Get the next formation id.
 */
FormationID AI::getNextFormationID()
{
	FormationID nextVal = m_nextFormationID;
	m_nextFormationID = (FormationID) (nextVal+1);
	return nextVal;
}

//-----------------------------------------------------------------------------
// GeneralsX: internal linkage: SpectreGunshipUpdate.cpp defines another class of this name, and
// once this one overrides getCellOwnerMask the two differ - the linker would keep one vtable for both
namespace {
class PartitionFilterLiveMapEnemies : public PartitionFilter
{
private:
	const Object *m_obj;
public:
	PartitionFilterLiveMapEnemies(const Object *obj) : m_obj(obj) { }

	virtual Bool allow(Object *objOther) override
	{
		// this is way fast (bit test) so do it first.
		if (objOther->isEffectivelyDead())
			return false;

		// this is also way fast (bit test) so do it next.
		if (objOther->isOffMap() != m_obj->isOffMap())
			return false;

		Relationship r = m_obj->getRelationship(objOther);
		if (r != ENEMIES)
			return false;

		return true;
	}

	// GeneralsX: allow() passes ENEMIES only and has no side effects, so as the first filter
	// (findClosestEnemy) it lets getClosestObjects skip cells holding no enemy (PartitionFilter::getCellOwnerMask)
	virtual PartitionOwnerMask getCellOwnerMask() override
	{
		return PartitionFilterRelationship::cellOwnerMaskFor(m_obj, PartitionFilterRelationship::ALLOW_ENEMIES);
	}

#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterLiveMapEnemies"; }
#endif
};
} // namespace

//-----------------------------------------------------------------------------
class PartitionFilterWithinAttackRange : public PartitionFilter
{
private:
	const Object* m_obj;
public:
	PartitionFilterWithinAttackRange(const Object* obj) : m_obj(obj) { }

	virtual Bool allow(Object* objOther) override
	{
		for (Int i = 0; i < WEAPONSLOT_COUNT;	i++ )
		{
			// ignore empty slots.
			const Weapon* w = m_obj->getWeaponInWeaponSlot((WeaponSlotType)i);
			if (w == nullptr)
				continue;

			if (w->isWithinAttackRange(m_obj, objOther))
			{
				return true;
			}
		}
		return false;
	}

#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterWithinAttackRange"; }
#endif
};


typedef struct
{
	Int priority;
	const AttackPriorityInfo *info;
} TPriorityInfo;

static void priorityFunc(Object *obj, void *userData)
{
	TPriorityInfo *dp;
	dp = (TPriorityInfo*)userData;
	Int curPriority = dp->info->getPriority(obj->getTemplate());
	if (curPriority>dp->priority) {
		dp->priority = curPriority;
	}
}


#if defined(GX_TEST_INSTRUMENTS)
//-----------------------------------------------------------------------------
// GeneralsX: The retail attack-priority search, kept verbatim (the modifier passed in) as the
// reference for GX_HUNT_CHECK: every object that passes the filters, sorted near to far, scored in that order; the first
// object with the largest (modPriority, curPriority) wins.
static Object *findBestByPriorityRetail(const Object *me, Real range, PartitionFilter **filters,
	const AttackPriorityInfo *info, Real modifierDist, Bool *noCandidates)
{
	Object *bestEnemy = nullptr;
	Int			effectivePriority=0;
	Int			actualPriority=0;
	ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(me, range, FROM_BOUNDINGSPHERE_2D, filters, ITER_SORTED_NEAR_TO_FAR);
	MemoryPoolObjectHolder holder(iter);
	// GeneralsX: an empty iterator means no object passed the filters.
	*noCandidates = (iter->first() == nullptr);
	for (Object *theEnemy = iter->first(); theEnemy; theEnemy = iter->next())
	{
		Int curPriority = info->getPriority(theEnemy->getTemplate());
		if (curPriority == 0)
			continue; // don't attack 0 priority targets.

		/* check for garrisoned buildings/vehicles & see if a higher priority unit is inside. */
		ContainModuleInterface* contain = theEnemy->getContain();
		if (contain) {
			TPriorityInfo priorityInfo;
			priorityInfo.priority = curPriority;
			priorityInfo.info = info;
			contain->iterateContained( priorityFunc, &priorityInfo, false ) ;
			if (priorityInfo.priority > curPriority) {
				curPriority = priorityInfo.priority;
			}
		}

		Real distSqr = ThePartitionManager->getDistanceSquared(me, theEnemy, FROM_BOUNDINGSPHERE_2D);
		Real dist = sqrt(distSqr);
		Int modifier = dist/modifierDist;
		Int modPriority = curPriority-modifier;
		if (modPriority < 1)
			modPriority = 1;
		if (modPriority > effectivePriority)
		{
			effectivePriority = modPriority;
			actualPriority = curPriority;
			bestEnemy = theEnemy;
		}
		if (modPriority == effectivePriority && curPriority > actualPriority)
		{
			effectivePriority = modPriority;
			actualPriority = curPriority;
			bestEnemy = theEnemy;
		}
	}
	return bestEnemy;
}
#endif // GX_TEST_INSTRUMENTS

//-----------------------------------------------------------------------------
// GeneralsX: findBestByPriorityRetail's result from getClosestObjects' visitor mode. visit()
// gets the objects in the order the retail iterator INSERTED them; the iterator prepends and then merge-sorts stably by
// the same distSqr key, so in its sorted order a smaller key comes first and, among equal keys, the later-inserted
// object comes first. The retail loop keeps the first sorted object with the largest (modPriority, curPriority), so a
// newly visited object takes over when it beats the best on (modPriority, curPriority), or ties it and sorts first
// (key <= the best's key). Scoring arithmetic is the retail loop's, line for line.
// Early stop: an object not visited yet is farther than minCentreDist (centre) and has a radius <= maxRadius, so its
// boundary distance is >= minCentreDist - myRadius - maxRadius (the 1.0 absorbs float rounding), its modifier is at least
// Int(that / modifierDist), and its modPriority at most max(1, maxPriority - that modifier). Stop when that is below the
// best modPriority: it can neither beat nor tie the best. Never with a best of 1 (a clamped 1 ties and wins on priority),
// never unless the modifier is positive, and never when the caller says a filter has side effects (mayStop): a skipped
// object is a skipped filter call, and PartitionFilterFreeOfFog's getShroudedStatus updates seen state and ghost
// snapshots that scripts read later.
class HuntPriorityVisitor : public PartitionRingVisitor
{
public:
	const Object *m_me;
	const AttackPriorityInfo *m_info;
	Real m_modifierDist;
	Int m_maxPriority;
	Real m_myRadius;
	Object *m_best;
	Int m_bestMod;
	Int m_bestPrio;
	Real m_bestKey;
	Bool m_any;
	Bool m_stopped;
	Bool m_mayStop;

	HuntPriorityVisitor(const Object *me, const AttackPriorityInfo *info, Real modifierDist, Bool mayStop) :
		m_me(me), m_info(info), m_modifierDist(modifierDist), m_maxPriority(info->getMaxPriority()),
		m_myRadius(me->getGeometryInfo().getBoundingCircleRadius()), m_best(nullptr), m_bestMod(0), m_bestPrio(0),
		m_bestKey(0.0f), m_any(FALSE), m_stopped(FALSE), m_mayStop(mayStop) { }

	virtual void visit(Object *theEnemy, Real distSqrKey) override
	{
		m_any = TRUE;
		Int curPriority = m_info->getPriority(theEnemy->getTemplate());
		if (curPriority == 0)
			return; // don't attack 0 priority targets.

		/* check for garrisoned buildings/vehicles & see if a higher priority unit is inside. */
		ContainModuleInterface* contain = theEnemy->getContain();
		if (contain) {
			TPriorityInfo priorityInfo;
			priorityInfo.priority = curPriority;
			priorityInfo.info = m_info;
			contain->iterateContained( priorityFunc, &priorityInfo, false ) ;
			if (priorityInfo.priority > curPriority) {
				curPriority = priorityInfo.priority;
			}
		}

		Real distSqr = ThePartitionManager->getDistanceSquared(m_me, theEnemy, FROM_BOUNDINGSPHERE_2D);
		Real dist = sqrt(distSqr);
		Int modifier = dist/m_modifierDist;
		Int modPriority = curPriority-modifier;
		if (modPriority < 1)
			modPriority = 1;
		if (modPriority > m_bestMod
			|| (modPriority == m_bestMod && (curPriority > m_bestPrio || (curPriority == m_bestPrio && distSqrKey <= m_bestKey))))
		{
			m_best = theEnemy;
			m_bestMod = modPriority;
			m_bestPrio = curPriority;
			m_bestKey = distSqrKey;
		}
	}

	Int boundAt(Real minBoundaryDist) const
	{
		Int bound = m_maxPriority - (Int)(minBoundaryDist / m_modifierDist);
		return bound < 1 ? 1 : bound;
	}

	virtual Bool canStopAfter(Real minCentreDist, Real maxRadius) override
	{
		// a zero or negative AttackPriorityDistanceModifier (the INI accepts one) makes farther targets score the same or
		// higher, so distance bounds nothing: walk everything, as retail did
		if (!m_mayStop || m_bestMod <= 1 || !(m_modifierDist > 0.0f))
			return FALSE;
		const Real lb = minCentreDist - m_myRadius - maxRadius - 1.0f;
		return lb > 0.0f && boundAt(lb) < m_bestMod;
	}

	virtual void stoppedEarly() override { m_stopped = TRUE; }

	virtual Bool mayStillMatter(const Object *obj, Real centreDist) override
	{
		if (!(m_modifierDist > 0.0f))
			return TRUE;	// never asked then (canStopAfter refuses), kept consistent
		const Real lb = centreDist - m_myRadius - obj->getGeometryInfo().getBoundingCircleRadius() - 1.0f;
		return lb <= 0.0f || boundAt(lb) >= m_bestMod;
	}
};

#if defined(GX_TEST_INSTRUMENTS)
//-----------------------------------------------------------------------------
// GeneralsX: GX_HUNT_CHECK=1 (test builds only,
// -DGX_TEST_INSTRUMENTS=ON; 29/10/2026): the equivalence harness for HuntPriorityVisitor. After each
// attack-priority search it runs the retail search too and compares the winner and "no candidates"; prints cumulative
// "[GXHUNT] f= q= diff= stopped=" every 600 logic frames. Off (the default) it costs one static flag test per search.
// On, it walks the whole range again (the retail cost), so it is for audits, never for timing.
static Bool gxHuntCheckOn() { static const Bool b = getenv("GX_HUNT_CHECK") != nullptr && atoi(getenv("GX_HUNT_CHECK")) > 0; return b; }
static void gxHuntCheck(const Object *me, Real range, PartitionFilter **filters, const AttackPriorityInfo *info,
	Real modifierDist, const HuntPriorityVisitor &visitor)
{
	static Int s_queries = 0, s_diffs = 0, s_stopped = 0;
	static UnsignedInt s_lastPrint = 0;
	Bool retailNoCandidates = FALSE;
	Object *retail = findBestByPriorityRetail(me, range, filters, info, modifierDist, &retailNoCandidates);
	++s_queries;
	if (retail != visitor.m_best || retailNoCandidates != !visitor.m_any)
		++s_diffs;
	if (visitor.m_stopped)
		++s_stopped;
	const UnsignedInt f = TheGameLogic ? TheGameLogic->getFrame() : 0;
	if (f / 600 != s_lastPrint / 600)
	{
		s_lastPrint = f;
		fprintf(stderr, "[GXHUNT] f=%u q=%d diff=%d stopped=%d\n", f, s_queries, s_diffs, s_stopped);
		fflush(stderr);
	}
}
#endif // GX_TEST_INSTRUMENTS

//-----------------------------------------------------------------------------
/**
 * Return the closest enemy, according to the qualifiers.
 */
Object *AI::findClosestEnemy( const Object *me, Real range, UnsignedInt qualifiers,
														 const AttackPriorityInfo *info, PartitionFilter *optionalFilter, Bool *noCandidates)
{
	// GeneralsX: Report "no object passed the filters" so AIHuntState can skip an identical retry.
	if (noCandidates)
		*noCandidates = FALSE;

	if ((qualifiers & CAN_ATTACK) && !me->isAbleToAttack())
	{
		/*
			PartitionFilterPossibleToAttack would filter out everything anyway,
			so just punt here.
		*/
		// GeneralsX: nothing can pass PartitionFilterPossibleToAttack either.
		if (noCandidates)
			*noCandidates = TRUE;
		return nullptr;
	}

	// only consider live, on-map enemies.
	// since this gets called a ton, I made a special custom filter to
	// combine several canned ones, in the name of speed (srj)
	PartitionFilterLiveMapEnemies filterObvious(me);

	PartitionFilterWithinAttackRange filterWithinAttackRange(me);

	// never target buildings (unless they can attack)
	PartitionFilterRejectBuildings filterBldgs(me);

	// and only stuff that isn't stealthed (and not detected)
	// (note that stealthed allies aren't hidden from us, but we're only looking for enemies here)
	// *** This doesn't cut it anymore. Bombtrucks can be disguised as an enemy member meaning we
	//     still want to acquire it -- however this old filter fails because the unit is stealthed.
	//PartitionFilterRejectByObjectStatus filterStealth(OBJECT_STATUS_STEALTHED, OBJECT_STATUS_DETECTED);
	// *** Use this new filter
	PartitionFilterStealthedAndUndetected filterStealth( me, false );

	// (optional) only stuff we can see.
	PartitionFilterLineOfSight	filterLOS(me);

	// (optional) only stuff we can attack
	PartitionFilterPossibleToAttack filterAttack(ATTACK_NEW_TARGET, me, CMD_FROM_AI);

	// (optional) only stuff that is significant
	PartitionFilterInsignificantBuildings filterInsignificant(true, false);

	// (optional) only stuff clear of fog
	PartitionFilterFreeOfFog filterFogged(me->getControllingPlayer()->getPlayerIndex());

	PartitionFilter *filters[16];
	Int numFilters = 0;

	// Important note: the filters are called in order, and once one rejects an object,
	// the remaining ones are not called, so you should endeavor to
	// arrange them such that the most-coarse (ie, the ones that will usually REJECT
	// the most objects) should come first.
	//
	// srj sez: I actually did profiling on USA04 (enabling FILTER_PROFILING in partition mgr)
	// to determine the order of these. some observations:
	//
	// -- filterTeam is BY FAR the best to put first (since most things near you tend to be nonenemies).
	// -- filterLOS tends to reject a very large number, but is computationally expensive
	// -- filterStealth is BY FAR the least common to be useful, so it goes last.
	// GS Fog check used to be inside can attack, so it feels right to be right after it

	filters[numFilters++] = &filterObvious;

	if( !(qualifiers & ATTACK_BUILDINGS) )
		filters[numFilters++] = &filterBldgs;

	if (qualifiers & WITHIN_ATTACK_RANGE)
		filters[numFilters++] = &filterWithinAttackRange;

	if (qualifiers & CAN_SEE)
		filters[numFilters++] = &filterLOS;

	if (qualifiers & CAN_ATTACK)
		filters[numFilters++] = &filterAttack;

	if (qualifiers & UNFOGGED)
		filters[numFilters++] = &filterFogged;

	if (qualifiers & IGNORE_INSIGNIFICANT_BUILDINGS)
		filters[numFilters++] = &filterInsignificant;

	filters[numFilters++] = &filterStealth;

	if (optionalFilter)
	{
		filters[numFilters++] = optionalFilter;
	}

	filters[numFilters] = nullptr;

	if (info == nullptr || info == TheScriptEngine->getDefaultAttackInfo())
	{
		// No additional attack info, so just return the closest one.
		Object* o = ThePartitionManager->getClosestObject( me, range, FROM_BOUNDINGSPHERE_2D, filters );
		// GeneralsX: getClosestObject returns null only when no object passed the filters.
		if (noCandidates)
			*noCandidates = (o == nullptr);
		return o;
	}

	// GeneralsX: An attack-priority search used to collect EVERY object that passed the
	// filters across the whole range (a hunter's is 9999.9: the map) into a sorted iterator and then score all of them:
	// ~0.9 ms per hunt search with 12 Hard AIs, most of it visiting objects that could never win (ai-smartcheap P2/gco).
	// The visitor scores them as the walk finds them and stops the walk once nothing unvisited can win - the same
	// winner by construction (see HuntPriorityVisitor and getClosestObjects' visitor mode). GX_HUNT_CHECK=1 (test builds) runs the
	// retail search as well and counts disagreements ([GXHUNT]).
	// Every filter must still see every object the retail walk would have shown it when one has side effects: UNFOGGED's
	// fog filter does (see HuntPriorityVisitor), and a caller's own filter is not known not to - those walk the whole range.
	const Bool mayStop = !(qualifiers & UNFOGGED) && optionalFilter == nullptr;
	HuntPriorityVisitor visitor(me, info, getAiData()->m_attackPriorityDistanceModifier, mayStop);
	ThePartitionManager->visitObjectsNearToFar(me, range, FROM_BOUNDINGSPHERE_2D, filters, &visitor);
#if defined(GX_TEST_INSTRUMENTS)
	if (gxHuntCheckOn())
		gxHuntCheck(me, range, filters, info, getAiData()->m_attackPriorityDistanceModifier, visitor);
#endif
	if (noCandidates)
		*noCandidates = !visitor.m_any;
	return visitor.m_best;
}




 /////////////////////////////
/**
 * Return the closest ally, according to the qualifiers.
 */
Object *AI::findClosestAlly( const Object *me, Real range, UnsignedInt qualifiers)
{
	// never target buildings (unless they can attack)
	PartitionFilterRejectBuildings		filterBldgs(me);

	// only consider allies.
	PartitionFilterRelationship	filterTeam(me, PartitionFilterRelationship::ALLOW_ALLIES);

	// and only stuff that is not dead
	PartitionFilterAlive filterAlive;

	// and on map (or not)
	PartitionFilterSameMapStatus filterMapStatus(me);

	// (optional) only stuff we can see.
	PartitionFilterLineOfSight	filterLOS(me);

	PartitionFilter *filters[16];
	Int numFilters = 0;

	filters[numFilters++] = &filterBldgs;
	filters[numFilters++] = &filterTeam;
	filters[numFilters++] = &filterAlive;
	filters[numFilters++] = &filterMapStatus;

	if (qualifiers & CAN_SEE)
		filters[numFilters++] = &filterLOS;

	filters[numFilters] = nullptr;

	return ThePartitionManager->getClosestObject( me, range, FROM_BOUNDINGSPHERE_2D, filters );
}
/////////////////////////////



 /////////////////////////////
/**
 * Return the closest repulsor.
 */
Object *AI::findClosestRepulsor( const Object *me, Real range)
{

	if (!getAiData()->m_enableRepulsors) {
		return nullptr;
	}

	// never target buildings (unless they can attack)
	PartitionFilterRepulsor		filter(me);

	// and only stuff that isn't stealthed (and not detected)
	// (note that stealthed allies aren't hidden from us, but that's ok. jba.)
	PartitionFilterRejectByObjectStatus filterStealth( MAKE_OBJECT_STATUS_MASK( OBJECT_STATUS_STEALTHED ),
																										 MAKE_OBJECT_STATUS_MASK2( OBJECT_STATUS_DETECTED, OBJECT_STATUS_DISGUISED ) );

	PartitionFilter *filters[16];
	Int numFilters = 0;

	filters[numFilters++] = &filter;
	filters[numFilters++] = &filterStealth;
	filters[numFilters] = nullptr;

	return ThePartitionManager->getClosestObject( me, range, FROM_BOUNDINGSPHERE_2D, filters );
}
/////////////////////////////

Real AI::getAdjustedVisionRangeForObject(const Object *object, Int factorsToConsider)
{
	Real originalRange = object->getVisionRange();
	const AIUpdateInterface *ai = object->getAI();
	const TAiData *aiData = TheAI->getAiData();

	if (!ai)
	{
		DEBUG_CRASH(("Unit without AI ('%s') calling AI::getAdjustedVisionRangeForObject. Notify jkmcd.", object->getTemplate()->getName().str()));
		return 0.0f;
	}

	UnsignedInt moodMatrixVal = ai->getMoodMatrixValue();

	if (factorsToConsider & AI_VISIONFACTOR_OWNERTYPE)
	{
		Bool playerIsHuman = (moodMatrixVal & MM_Controller_Player) != 0;

		if (playerIsHuman)
		{
			if (factorsToConsider & AI_VISIONFACTOR_GUARDINNER)
				originalRange *= aiData->m_guardInnerModifierHuman;
			else
				originalRange *= aiData->m_guardOuterModifierHuman;
		}
		else
		{
			if (factorsToConsider & AI_VISIONFACTOR_GUARDINNER)
				originalRange *= aiData->m_guardInnerModifierAI;
			else
				originalRange *= aiData->m_guardOuterModifierAI;
		}
	}

	if (object->getContainedBy() != nullptr)
	{
		originalRange = object->getLargestWeaponRange();
	}
	else
	{
		if ((factorsToConsider & AI_VISIONFACTOR_MOOD) && ((moodMatrixVal & MM_Controller_Player) == 0) )
		{
			switch(moodMatrixVal & MM_Mood_Bitmask)
			{
				case MM_Mood_Sleep:
					return 0.0f;

				case MM_Mood_Passive:
				case MM_Mood_Normal:
					break;

				case MM_Mood_Alert:
					originalRange *= TheAI->getAiData()->m_alertRangeModifier;
					break;

				case MM_Mood_Aggressive:
					originalRange *= TheAI->getAiData()->m_aggressiveRangeModifier;
					break;
			}
		}
	}

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugVisibility)
	{
		// ICK. This really nasty statement is used so that we only initialize this color once.
		// It should be exactly double the intensity of its targettable brother.
		static RGBColor theAdjustedVisionColor = {
			(TheGlobalData->m_debugVisibilityTargettableColor.red * 2 <= 1.0f ?
				TheGlobalData->m_debugVisibilityTargettableColor.red * 2 :
				1.0f),
			(TheGlobalData->m_debugVisibilityTargettableColor.green * 2 <= 1.0f ?
				TheGlobalData->m_debugVisibilityTargettableColor.green * 2 :
				1.0f),
			(TheGlobalData->m_debugVisibilityTargettableColor.blue * 2 <= 1.0f ?
				TheGlobalData->m_debugVisibilityTargettableColor.blue * 2 :
				1.0f)
		};

		Vector3 pos(originalRange, 0, 0);
		for (int i = 0; i < TheGlobalData->m_debugVisibilityTileCount; ++i)
		{
			pos.Rotate_Z(1.0f * i / TheGlobalData->m_debugVisibilityTileCount * 2 * PI);
			Coord3D coord = { pos.X + object->getPosition()->x, pos.Y + object->getPosition()->y, pos.Z + object->getPosition()->z };

			addIcon(&coord, TheGlobalData->m_debugVisibilityTileWidth,
											TheGlobalData->m_debugVisibilityTileDuration,
											theAdjustedVisionColor);
		}
	}
#endif
	return originalRange;
}

//-------------------------------------------------------------------------------------------------
TAiData::TAiData() :
m_next(nullptr),
m_sideInfo(nullptr),
m_attackIgnoreInsignificantBuildings(false),
m_skirmishGroupFudgeValue(0.0f),
m_structureSeconds(0),
m_teamSeconds(0),
m_resourcesWealthy(0),
m_resourcesPoor(0),
m_forceIdleFramesCount(1),
m_structuresWealthyMod(0),
m_teamPoorMod(0),
m_teamResourcesToBuild(0),
m_guardInnerModifierAI(0),
m_guardOuterModifierAI(0),
m_guardInnerModifierHuman(0),
m_guardOuterModifierHuman(0),
m_guardChaseUnitFrames(0),
m_guardEnemyScanRate(LOGICFRAMES_PER_SECOND/2),
m_guardEnemyReturnScanRate(LOGICFRAMES_PER_SECOND),
m_wallHeight(0),
m_alertRangeModifier(0),
m_aggressiveRangeModifier(0),
m_attackPriorityDistanceModifier(0),
m_maxRecruitDistance(0),
m_skirmishBaseDefenseExtraDistance(0),
m_repulsedDistance(0),
m_enableRepulsors(false),
m_forceSkirmishAI(false),
m_rotateSkirmishBases(false),
m_attackUsesLineOfSight(true),
m_minInfantryForGroup(3),
m_minVehiclesForGroup(4),
m_minDistanceForGroup(100),
m_minClumpDensity(0.5f),
m_infantryPathfindDiameter(6),
m_vehiclePathfindDiameter(6),
m_supplyCenterSafeRadius(250),
m_rebuildDelaySeconds(10),
m_distanceRequiresGroup(0.0f),
m_sideBuildLists(nullptr),
m_structuresPoorMod(0.0f),
m_teamWealthyMod(0.0f),
m_aiDozerBoredRadiusModifier(2.0),
m_aiCrushesInfantry(true),
m_maxRetaliateDistance(210.0f),
m_retaliateFriendsRadius(120.0f)
{
}

//-------------------------------------------------------------------------------------------------
void TAiData::crc( Xfer *xfer )
{
	xfer->xferReal( &m_structureSeconds );
	xfer->xferReal( &m_teamSeconds );
	xfer->xferInt( &m_resourcesWealthy );
	xfer->xferInt( &m_resourcesPoor );
	xfer->xferUnsignedInt( &m_forceIdleFramesCount );
	xfer->xferReal( &m_structuresWealthyMod );
	xfer->xferReal( &m_teamWealthyMod );
	xfer->xferReal( &m_structuresPoorMod );
	xfer->xferReal( &m_teamPoorMod );
	xfer->xferReal( &m_teamResourcesToBuild );
	xfer->xferReal( &m_guardInnerModifierAI );
	xfer->xferReal( &m_guardOuterModifierAI );
	xfer->xferReal( &m_guardInnerModifierHuman );
	xfer->xferReal( &m_guardOuterModifierHuman );
	xfer->xferUnsignedInt( &m_guardChaseUnitFrames );
	xfer->xferUnsignedInt( &m_guardEnemyScanRate );
	xfer->xferUnsignedInt( &m_guardEnemyReturnScanRate );
	xfer->xferReal( &m_alertRangeModifier );
	xfer->xferReal( &m_aggressiveRangeModifier );
	xfer->xferReal( &m_attackPriorityDistanceModifier );
	xfer->xferReal( &m_maxRecruitDistance );
	xfer->xferReal( &m_skirmishBaseDefenseExtraDistance );
	xfer->xferReal( &m_repulsedDistance );
	xfer->xferBool( &m_enableRepulsors );
	CRCGEN_LOG(("CRC after AI TAiData for frame %d is 0x%8.8X", TheGameLogic->getFrame(), ((XferCRC *)xfer)->getCRC()));

}

//-----------------------------------------------------------------------------
void TAiData::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

}

//-----------------------------------------------------------------------------
void TAiData::loadPostProcess()
{

}

//-----------------------------------------------------------------------------
void AI::crc( Xfer *xfer )
{

	xfer->xferSnapshot( m_pathfinder );
	CRCGEN_LOG(("CRC after AI pathfinder for frame %d is 0x%8.8X", TheGameLogic->getFrame(), ((XferCRC *)xfer)->getCRC()));

	AsciiString marker;
	TAiData *aiData = m_aiData;
	while (aiData)
	{
		marker = "MARKER:TAiData";
		xfer->xferAsciiString(&marker);
		xfer->xferSnapshot( aiData );
		aiData = aiData->m_next;
	}

	for (std::list<AIGroup *>::iterator groupIt = m_groupList.begin(); groupIt != m_groupList.end(); ++groupIt)
	{
		if (*groupIt)
		{
			marker = "MARKER:AIGroup";
			xfer->xferAsciiString(&marker);
			xfer->xferSnapshot( (*groupIt) );
		}
	}

}

//-----------------------------------------------------------------------------
void AI::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

}

//-----------------------------------------------------------------------------
void AI::loadPostProcess()
{

}


