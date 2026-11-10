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

// AIPlayer.h
// Computerized opponent
// Author: Michael S. Booth, January 2002

#pragma once

#include "Common/GameMemory.h"
#include "Common/Snapshot.h"
#if defined(GX_TEST_INSTRUMENTS)
#include <cstdlib>
/// GeneralsX: t2-cheap (test-instruments builds only): T2's OWN diagnostics - every [GXTAA] line and the scans that
/// only feed them (census, plane_gone, engageable, auto_air, cover) - run unless GX_TAA_DIAG=0. Off changes no decision: those blocks
/// read the game and write stderr (and a log-only list, m_gxAirPrevRaid, that nothing saves, CRCs or reads); the engageable scan's
/// damage estimate also refreshes caches every reader refreshes itself (Weapon::getStatus's m_status, armor - not CRC'd;
/// ). The [GXHOOK] timer stays on either way. A release build has none of this code.
inline bool gxTaaDiagOn()
{
	static const bool on = []() { const char *v = getenv("GX_TAA_DIAG"); return !(v != nullptr && v[0] == '0' && v[1] == '\0'); }();
	return on;
}
#endif

enum { INVALID_SKILLSET_SELECTION = -1 };

class BuildListInfo;
class Script;
class GXRuleBank;

/**
 * When a team is selected for training, a list of these
 * "work orders" are created, one for each member of the team.
 * This pairs team members with production buildings to keep
 * track of who is building what, and allows us to track if
 * a building was destroyed while in the process of training a unit.
 */
class WorkOrder : public MemoryPoolObject,
									public Snapshot
{

	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( WorkOrder, "WorkOrder" )

public:

	WorkOrder():m_thing(nullptr), m_factoryID(INVALID_ID), m_isResourceGatherer(false), m_numCompleted(0), m_numRequired(1), m_next(nullptr) {};

	Bool isWaitingToBuild();		///< return true if nothing is yet building this unit
	void validateFactory( Player *thisPlayer );			///< verify factoryID still refers to an active object

public:

	const ThingTemplate *m_thing;			///< thing to build
	ObjectID m_factoryID;							///< ID of object that is building this, or zero if no-one is
	WorkOrder *m_next;
	Int			m_numCompleted;					  ///< Number built.
	Int			m_numRequired;					  ///< Number needed.
	Bool		m_required;								///< True if part of minimum requirement.
	Bool		m_isResourceGatherer;			///< True if resource gatherer.

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

};

inline Bool WorkOrder::isWaitingToBuild()
{
	if (m_factoryID!=INVALID_ID)
		return false;
	if (m_numCompleted >= m_numRequired)
		return false;
	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
class TeamInQueue : public MemoryPoolObject,
										public Snapshot
{

	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( TeamInQueue, "TeamInQueue"  )

private:

	MAKE_DLINK(TeamInQueue, TeamBuildQueue)				///< the instances of our prototype
	MAKE_DLINK(TeamInQueue, TeamReadyQueue)				///< the instances of our prototype

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

public:

	TeamInQueue() :
		m_workOrders(nullptr),
		m_team(nullptr),
		m_nextTeamInQueue(nullptr),
		m_sentToStartLocation(false),
		m_reinforcement(false),
		m_stopQueueing(false),
		m_reinforcementID(INVALID_ID),
		m_frameStarted(0),
		m_priorityBuild(FALSE),
		m_productionConditionScript(nullptr),
		m_productionConditionScriptValid(FALSE)
	{
	}

	Bool isAllBuilt();				///< Returns true if the team is finished building.
	Bool isBuildTimeExpired();///< Returns true if the team has run out of build time.
	Bool isMinimumBuilt();		///< Returns true if the team has started building at least the minimum number of units.
	Bool includesADozer();		///< Returns true if the team includes a dozer unit.
	Bool areBuildsComplete();	///< Returns true if all units in factories have finished building.
	void disband();						///< Disbands the team (moves units into the default team).
	void stopQueueing() {m_stopQueueing=true;} ///< Stops building new units, just finishes current.

public:

	WorkOrder *m_workOrders;				///< list of work orders
	Bool m_priorityBuild;						///< True if the team is specifically requested.
	Team *m_team;										///< the team that units built by the m_workOrders go into
	TeamInQueue *m_nextTeamInQueue; ///< next
	Int	m_frameStarted;							///< Frame we started building.
	Bool m_sentToStartLocation;			///< Has it been sent to it's start location?
	Bool m_stopQueueing;						///< True if we are to quit queueing units (usually because we ran out of build time.)
	Bool m_reinforcement;						///< True if it is a unit to reinforce an existing team.
	ObjectID m_reinforcementID;			///< True if it is a unit to reinforce an existing team.

	// GeneralsX: Memo of TheScriptEngine->findScriptByName(production condition), which is a linear
	// scan of every side's scripts and ran every frame per queued/ready team. Runtime-only: not xfer'd or CRC'd, re-resolved
	// when the condition name changes (m_team can be reassigned). Script lists are only replaced at map load/game setup.
	const Script *getProductionConditionScript();
private:
	const Script *m_productionConditionScript;
	AsciiString m_productionConditionScriptName;
	Bool m_productionConditionScriptValid;

};


/**
 * The computer-controlled opponent.
 */
class AIPlayer : public MemoryPoolObject,
								 public Snapshot
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AIPlayer, "AIPlayer"  )

public:

	AIPlayer( Player *p );							///< constructor

	virtual Bool computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *pos, Int playerNdx, Real weaponRadius); ///< Calculates best pos for weapon given radius.
	/// GeneralsX: how a power is aimed (AIPlayer.cpp gxStrategicCost): 2003's build cost, or economic/strategic value
	enum SuperweaponAim { AIM_STOCK, AIM_STRATEGIC, AIM_STRATEGIC_SUPERWEAPON };
	static SuperweaponAim gxSuperweaponAim( const Player *aimer, const SpecialPowerTemplate *power );	///< GeneralsX: which aim this player uses for this power
	static Int getPlayerSuperweaponValue( Coord3D *center, Int playerNdx, Real radius, Bool includeMilitaryUnits = TRUE, SuperweaponAim aim = AIM_STOCK );

public: // AIPlayer interface, may be overridden by AISkirmishPlayer.  jba.

	virtual void update();											///< simulates the behavior of a player

	virtual void newMap();											///< New map loaded call.

	/// Invoked when a unit I am training comes into existence
	virtual void onUnitProduced( Object *factory, Object *unit );

	/// Invoked when a structure I am building comes into existence
	virtual void onStructureProduced( Object *factory, Object *structure );

	virtual void buildSpecificAITeam(TeamPrototype *teamProto, Bool priorityBuild); ///< Builds this team immediately.

	virtual void buildAIBaseDefense(Bool flank); ///< Builds base defense on front or flank of base.

	virtual void buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank); ///< Builds base defense on front or flank of base.

	virtual void buildSpecificAIBuilding(const AsciiString &thingName); ///< Builds this building as soon as possible.


	virtual void recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius); ///< Builds this team immediately.

	virtual Bool isSkirmishAI() {return false;}
	/// GeneralsX 22/10/2026 (ai-wh1ter0se-econ R, 6): AI_PLAYER_FACE_FRONT turned on; the point the doors face (FALSE: none).
	void gxSetFaceFront(Bool on) { m_gxFaceFront = on; }
	Bool gxFaceFront() const { return m_gxFaceFront; }
	virtual Bool gxFrontPoint(Coord3D *out) { (void)out; return FALSE; }
	/// GeneralsX 22/10/2026 (ai-wh1ter0se-econ T, 7): AI_PLAYER_TUNNEL_AIR_WATCH's radius; 0 = off (stock).
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2): a NEGATIVE radius is the observe-only watch of the ablation arm T0 - the same AA team,
	/// lifetime rule and [GXTAA] engageable lines, and no assignment, exit, attack or return order. gxAirWatchRadius() is the ACTING
	/// radius (0 in T0, so every "watch on" test elsewhere stays T's); gxAirWatchScanRadius() is the radius both arms scan with.
	void gxSetAirWatchRadius(Int r) { m_gxAirWatchRadius = r; }
	Int gxAirWatchRadius() const { return m_gxAirWatchRadius > 0 ? m_gxAirWatchRadius : 0; }
	Int gxAirWatchScanRadius() const { return m_gxAirWatchRadius < 0 ? -m_gxAirWatchRadius : m_gxAirWatchRadius; }
	void gxSetAirWatchTeam(const AsciiString& team) { m_gxAirWatchTeam = team; }
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2 lifetime): may this AI build (reinforce = FALSE) or reinforce the air-watch team now?
	/// Every other team: TRUE. Build needs C's airfield term, reinforcement needs C (AIPlayer.cpp).
	Bool gxAirTeamAllowed(const TeamPrototype *proto, Bool reinforce) const;
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2): an AA guard on its way back into the tunnels after its assigned aircraft (the tunnel
	/// guard's Return state then turns back for nothing).
	Bool gxAirReturning(ObjectID unit) const;
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2): with the ACTING watch on, a member of the air-watch team never
	/// takes an aircraft as its own idle-mood attack target (AIIdleState) - every attack order at an aircraft is an assignment
	Bool gxAirWatchTeamMemberSkips(const Object *unit, const Object *target) const;
	Bool gxAirAttackAuthorized(const Object *unit, const Object *target) const;	///< T2 r8: may this unit attack this aircraft (its own row's plane only)
	void gxAirRecruitEndsAirAttack(Object *unit) const;	///< t2-fix cause N: a recruit into the watch team drops an aircraft attack the watch refuses
	void gxAirLogTeam(const TeamPrototype *proto, const char *state);	///< [GXTAA] ev=team (test-instruments builds; no-op otherwise)
	/// GeneralsX 28/10/2026: TRUE when gxFrontPoint's answer cannot change for the rest of the game (AIPlayer.cpp gxBuildAngles).
	virtual Bool gxFrontPointFixed() { return TRUE; }
	/// GeneralsX 04/11/2026 base-layout: this side's list carries GXFrontDoors (FALSE for a campaign AI)
	virtual Bool gxFrontDoors() { return FALSE; }
	/// GeneralsX 06/11/2026 base-layout (ADDENDUM_8 -> ADDENDUM_9): the point supply depots are ranked from and a supply centre is offset towards
	/// (findSupplyCenter, buildBySupplies): the base centre, or a keyed skirmish list's STOCK centre (AISkirmishPlayer).
	virtual const Coord3D &gxSupplyRef() const { return m_baseCenter; }
	/// GeneralsX 30/10/2026 rb-core: this AI's rule bank - AISkirmishPlayer's; nullptr for a campaign AI
	virtual GXRuleBank *gxRuleBank() { return nullptr; }
	Int hoardThreshold();	///< GeneralsX: the bank above which this AI spends faster (0 = never; AIPlayer.cpp)
	Bool isHoarding();		///< GeneralsX: the bank is above hoardThreshold()
	Int unitsPerOrder(const ThingTemplate *thing);	///< GeneralsX: the largest batch any of our factories builds this unit in
	virtual Player *getAiEnemy() {return nullptr;}	///< Solo AI attacks based on scripting.  Only skirmish auto-acquires an enemy at this point.  jba.
	virtual Bool checkBridges(Object *unit, Waypoint *way) {return false;}
	virtual void repairStructure(ObjectID structure);

	virtual void selectSkillset(Int skillset);

public:
	Bool getBaseCenter(Coord3D *pos) const {*pos = m_baseCenter; return m_baseCenterSet;}
	/// Difficulty level for this player.
	GameDifficulty getAIDifficulty() const;
	void setAIDifficulty(GameDifficulty difficulty) {m_difficulty = difficulty;}
	void buildBySupplies(Int minimumCash, const AsciiString &thingName ); ///< Builds a building by supplies.
	void buildSpecificBuildingNearestTeam( const AsciiString &thingName, const Team *team );
	void buildUpgrade(const AsciiString &upgrade ); ///< Builds an upgrade.
	/// A team is about to be destroyed.
	void aiPreTeamDestroy( const Team *team );
	/// Is the nearest supply source safe?
 	Bool isSupplySourceSafe( Int minSupplies );
	/// Is a supply source attacked?
	Bool isSupplySourceAttacked();

	Bool isLocationSafe( const Coord3D *pos, const ThingTemplate *tthing);

	/// Have the team guard a supply center.
	void guardSupplyCenter( Team *team, Int minSupplies );

	void setTeamDelaySeconds(Int delay) {m_teamSeconds = delay;}

	/// Calculates the closest construction zone location based on a template.
	Bool calcClosestConstructionZoneLocation( const ThingTemplate *constructTemplate, Coord3D *location );

protected:

	// snapshot methods
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	virtual void doBaseBuilding();
	virtual void checkReadyTeams();
	virtual void checkQueuedTeams();
	virtual void doTeamBuilding();
	virtual void doUpgradesAndSkills();
	virtual Object *findDozer(const Coord3D *pos);
	virtual void queueDozer();
	virtual Bool selectTeamToBuild();			///< determine the next team to build
	virtual Bool selectTeamToReinforce( Int minPriority );			///< determine the next team to reinforce
	virtual Bool startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName);	///< find a production building that can handle the order, and start building
	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto );		///< return true if team should be built
	virtual void processBaseBuilding();		///< do base-building behaviors
	virtual void processTeamBuilding();		///< do team-building behaviors
// End of aiplayer interface.

protected:

	MAKE_DLINK_HEAD(TeamInQueue, TeamBuildQueue);		///< List of teams being build
	MAKE_DLINK_HEAD(TeamInQueue, TeamReadyQueue);		///< List of teams built, waiting to reach rally point.

protected:
	Bool isPossibleToBuildTeam( TeamPrototype *proto, Bool requireIdleFactory, Bool &needMoney );		///< return true if team can be considered for building
	Object *buildStructureNow(const ThingTemplate *bldgPlan, BuildListInfo *info );		///< Build a base buiding.
	Object *buildStructureWithDozer(const ThingTemplate *bldgPlan, BuildListInfo *info );		///< Build a base buiding.
	void clearTeamsInQueue();			///< Delete all teams in the build queue.
	void computeCenterAndRadiusOfBase(Coord3D *center, Real *radius);
	Object *findFactory(const ThingTemplate *thing, Bool busyOK); ///< Find a factory to build a unit.  If force is true, may return a busy factory.
	void queueUnits();						///< Check the team build list, & queue up units at any idle factories.
	void checkForSupplyCenter( BuildListInfo *info, Object *bldg);
 	void queueSupplyTruck();
	void updateBridgeRepair();
	Bool dozerInQueue();
	Object *findSupplyCenter(Int minSupplies, Bool skipUnsafe = false);
	static void getPlayerStructureBounds(Region2D *bounds, Int playerNdx, Bool conservative = FALSE );

protected:

	Player *m_player;									///< the Player we represent

	Bool		m_readyToBuildTeam;				///< True if the team select timer has expired.
	Bool		m_readyToBuildStructure;	///< True if the buildDelay timer has expired.
	Int			m_teamTimer;							///< Counts out the time between teams, as specified by ini.
	Int			m_structureTimer;					///< Counts out the time between structures, as specified by ini.
	Int			m_teamSeconds;						///< How many seconds to delay between teams.

	Int			m_buildDelay;							///< Delay for building in case we are resource or prereq. limited.
	Int			m_teamDelay;							///< Delay for teams in case we are resource or factory prereq. limited.

	Int			m_frameLastBuildingBuilt;	///< When we built the last building.

	GameDifficulty m_difficulty;

	Int			m_skillsetSelector;

	Coord3D m_baseCenter; // Center of the initial build list of structures.
	Bool m_gxFaceFront;	///< GeneralsX 22/10/2026: AI_PLAYER_FACE_FRONT (ai-wh1ter0se-econ R; AIPlayer xfer v2)
	Int m_gxAirWatchRadius;	///< GeneralsX 22/10/2026: AI_PLAYER_TUNNEL_AIR_WATCH (ai-wh1ter0se-econ T; AISkirmishPlayer xfer AIR_WATCH = 4, was 3 before the bank); < 0 = T0
	AsciiString m_gxAirWatchTeam;	///< GeneralsX 22/10/2026: the AA team whose idle members go back into the tunnels (xfer AIR_WATCH = 4)
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2; AISkirmishPlayer xfer AIR_GUARDS = 5, was 4 before the bank): the AA guards the watch has sent at an aircraft, and those
	/// on their way back in. One row per unit; a unit is in at most one row.
	enum { GX_AIR_GUARDS_MAX = 32 };
	enum { GX_AIR_ASSIGNED = 1, GX_AIR_ATTACKING = 2, GX_AIR_RETURNING = 3 };
	struct GXAirGuard { ObjectID unit; ObjectID plane; Int state; };
	GXAirGuard m_gxAirGuards[GX_AIR_GUARDS_MAX];
	Int m_gxAirGuardCount;
	Bool m_gxAirC;					///< the lifetime condition C at its last evaluation (airfield term OR an aircraft seen in 180 s)
	Bool m_gxAirCAirfield;	///< C's airfield term at its last evaluation
	Bool m_gxAirCKnown;			///< C has been evaluated (its first value is logged too)
	UnsignedInt m_gxAirSeenFrame;	///< the last frame a hostile aircraft was visible to this player; 0 = never
	/// GeneralsX 27/10/2026 (ai-pro-rules-2 T2, test-instruments LOGS only - nothing reads them, not saved): the raid planes of the
	/// previous pass, so a plane that dies or leaves is logged ([GXTAA] ev=plane_gone); after a load the first pass has none.
	ObjectID m_gxAirPrevRaid[16];
	Int m_gxAirPrevRaidCount;
	Bool		m_baseCenterSet; // True if baseCenter is valid.
	Real m_baseRadius; // Radius of the initial build list of structures.

	// Bridge repair info.
	enum {MAX_STRUCTURES_TO_REPAIR = 2};
	ObjectID m_structuresToRepair[MAX_STRUCTURES_TO_REPAIR];
	ObjectID m_repairDozer;
	Coord3D  m_repairDozerOrigin;
	Int			 m_structuresInQueue;
	Bool		 m_dozerQueuedForRepair;
	Bool		 m_dozerIsRepairing;			///< the repair dozer is trying to repair the bridge.
	Int			 m_bridgeTimer;

	UnsignedInt	m_supplySourceAttackCheckFrame;
	ObjectID m_attackedSupplyCenter;

	ObjectID m_curWarehouseID;
};
