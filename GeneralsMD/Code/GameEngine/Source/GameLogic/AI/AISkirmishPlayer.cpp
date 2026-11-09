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

// Portions (the cash-hoard helpers) ported from Zero Hour Reforged, commit 87e007e:
// Modified 2025-2026 by Olcay Seygan for Zero Hour Reforged; see that project's git history.

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// AISkirmishPlayer.cpp
// Computerized opponent
// Author: Michael S. Booth, January 2002

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Lib/trig.h"	// GeneralsX: deterministic Sin/Cos/Tan - see WWMath/gamemath.h


#include "Common/GameMemory.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/GXOctant.h"	// GeneralsX 22/10/2026 ai-wh1ter0se-econ R
#if defined(GX_TEST_INSTRUMENTS)	// GeneralsX 23/10/2026 ai-wh1ter0se-econ R: [GXROT] defence events, test-instruments builds only
#define GX_ROT_LOG(...) \
	do { fprintf(stderr, "[GXROT] f=%u ", (unsigned)TheGameLogic->getFrame()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
		fflush(stderr); } while (0)
extern void gxDoorRuleFixture();	// GeneralsX 04/11/2026 base-layout: AIPlayer.cpp, GX_DOORRULE_FIXTURE
extern void gxLayoutFallbackFixture();	// GeneralsX 06/11/2026 base-layout: AIPlayer.cpp, GX_LAYOUTFB_FIXTURE
#else
#define GX_ROT_LOG(...) do { } while (0)
#endif
#include "Common/TunnelTracker.h"	// GeneralsX 22/10/2026 ai-wh1ter0se-econ T
#include "GameLogic/AITNGuard.h"	// GeneralsX 27/10/2026 ai-pro-rules-2 T2: gxAirEngageable, gxTunnelNearest
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/AIStateMachine.h"	// GeneralsX 27/10/2026 T2: AI_GUARD_TUNNEL_NETWORK
#include "GameLogic/Weapon.h"	// GeneralsX 27/10/2026 T2: NO_MAX_SHOTS_LIMIT
#include "Common/GameState.h"	// GeneralsX 27/10/2026 T2: SC_INVALID_DATA (xfer v4)
#include "GameLogic/Module/PhysicsUpdate.h"
#if defined(GX_TEST_INSTRUMENTS)	// GeneralsX 22/10/2026 ai-wh1ter0se-econ T: [GXTAA] events, test-instruments builds only
#include <cstdarg>
#include "GameLogic/GameLogic.h"	// TheGameLogic, read by the helper below
// GeneralsX @performance Claude 03/11/2026 t2-cheap: ONE write per [GXTAA] line - the same bytes the old fprintf + fprintf + fputc wrote
// (three writes on the unbuffered stderr), so the line, its order and every reader see nothing new; a line too long for the buffer
// takes the old three calls.
static void gxTaaLogLine(const char *fmt, ...)
{
	char buf[1024];
	const unsigned frame = (unsigned)TheGameLogic->getFrame();
	const int n = snprintf(buf, sizeof(buf), "[GXTAA] f=%u ", frame);
	va_list ap;
	va_start(ap, fmt);
	const int m = (n > 0 && n < (int)sizeof(buf)) ? vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap) : -1;
	va_end(ap);
	if (m >= 0 && n + m + 1 < (int)sizeof(buf))
	{
		buf[n + m] = '\n';
		fwrite(buf, 1, (size_t)(n + m + 1), stderr);
	}
	else
	{
		fprintf(stderr, "[GXTAA] f=%u ", frame);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}
	fflush(stderr);
}
#define GX_TAA_LOG(...) do { if (gxTaaDiagOn()) gxTaaLogLine(__VA_ARGS__); } while (0)	// GX_TAA_DIAG=0: no line (AIPlayer.h)
#else
#define GX_TAA_LOG(...) do { } while (0)
#endif
#include "GameLogic/SidesList.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/GXHookTimer.h"	// GeneralsX 22/10/2026 ai-wh1ter0se-econ: [GXHOOK] CPU timer (test-instruments builds)
#include "GameLogic/GXRules/GXRuleBank.h"	// GeneralsX 30/10/2026 rb-core: the AI rule bank
GX_HOOK_STATS_EXTERN(gxFaceFrontHook);	// AIPlayer.cpp: the face-front arm's one per-tick sum
// arm T's per-tick time: the air watch here AND the guard-side checks in AITNGuard.cpp, ONE sum (Codex ke-tunnel-r11 P2)
GX_HOOK_STATS_DEFINE(gxTunnelAAHook, "tunnel_air_watch");
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameClient/TerrainVisual.h"

// GeneralsX @bugfix E·wh1ter0se-bugs-1015 16/10/2026 The AI tests a structure spot as standing ON THE GROUND. Its spot searches move a
// candidate in x and y only, keeping the height of the spot they started from (the build-list spot, the warehouse, the base
// centre), and isLocationLegalToBuild sees an object only when the two boxes overlap in height too
// (PartitionManager::geomCollidesWithGeom). So from a spot on a ridge, every candidate on the low ground below was tested as a box
// floating far above it and passed through everything there: on Twin Ridges a GLA AI put two Barracks 20 units apart, the
// second tested 47 units above the first (wh1ter0se's 0.9.64 game, 14/10). The same code is in TheSuperHackers and Generals Online.
// This tests a grounded copy and leaves the caller's position untouched.
static LegalBuildCode gxLegalToBuildOnGround( const Coord3D *pos, const ThingTemplate *what, Real angle, UnsignedInt options,
	const Object *builder, Player *player )
{
	Coord3D ground = *pos;
	ground.z = TheTerrainLogic->getGroundHeight( ground.x, ground.y );
	return TheBuildAssistant->isLocationLegalToBuild( &ground, what, angle, options, builder, player );
}


#define USE_DOZER 1



///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

AISkirmishPlayer::AISkirmishPlayer( Player *p ) :	AIPlayer(p),
m_curFlankBaseDefense(0),
m_curFrontBaseDefense(0),
m_curFrontLeftDefenseAngle(0),
m_curFrontRightDefenseAngle(0),
m_curLeftFlankLeftDefenseAngle(0),
m_curLeftFlankRightDefenseAngle(0),
m_curRightFlankLeftDefenseAngle(0),
m_curRightFlankRightDefenseAngle(0),
m_frameToCheckEnemy(0),
m_frameEnemyReviewed(0),
m_currentEnemy(nullptr),
m_gxRules(nullptr),
m_gxStockCenterSet(FALSE)

{
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	p->setCanBuildUnits(true); // turn on ai production by default.
	m_gxRules = new GXRuleBank(p);	// GeneralsX 30/10/2026 rb-core: empty until an AI_PLAYER_GX_RULE action configures it
	m_gxStockCenter.zero();
}

AISkirmishPlayer::~AISkirmishPlayer()
{
	clearTeamsInQueue();
	delete m_gxRules;	// GeneralsX 30/10/2026 rb-core (test-instruments builds print its ev=cost / ev=activity first)
	m_gxRules = nullptr;
}


/**
 * Build our base.
 */
void AISkirmishPlayer::processBaseBuilding()
{
	//
	// Refresh base buildings. Scan through list, if a building is missing,
	// rebuild it, unless it's rebuild count is zero.
	//
	if (m_readyToBuildStructure)
	{
		const ThingTemplate *bldgPlan=nullptr;
		BuildListInfo	*bldgInfo = nullptr;
		Bool isPriority = false;
		Object *bldg = nullptr;
		const ThingTemplate *powerPlan=nullptr;
		BuildListInfo	*powerInfo = nullptr;
		Bool isUnderPowered = !m_player->getEnergy()->hasSufficientPower();
		Bool powerUnderConstruction = false;
		for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *curPlan = TheThingFactory->findTemplate( name );
			if (!curPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			// check for hole.
			if (info->getObjectID() != INVALID_ID) {
				// used to have a building.
				Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
				if (bldg==nullptr) {
					// got destroyed.
					ObjectID priorID;
					priorID = info->getObjectID();
					info->setObjectID(INVALID_ID);
					info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					// Scan for a GLA hole.	KINDOF_REBUILD_HOLE
					Object *obj;
					for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() ) {
						if (!obj->isKindOf(KINDOF_REBUILD_HOLE)) continue;
						RebuildHoleBehaviorInterface *rhbi = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject( obj );
						if( rhbi ) {
							ObjectID spawnerID = rhbi->getSpawnerID();
							if (priorID == spawnerID) {
								DEBUG_LOG(("AI Found hole to rebuild %s", curPlan->getName().str()));
								info->setObjectID(obj->getID());
							}
						}
 					}
				}	else {
					if (bldg->getControllingPlayer() == m_player) {
						// Check for built or dozer missing.
						if( bldg->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
						{
							if (bldg->isKindOf(KINDOF_FS_POWER) && !bldg->isKindOf(KINDOF_CASH_GENERATOR))
							{
								powerUnderConstruction = true;
							}
							// make sure dozer is working on him.
							ObjectID builder = bldg->getBuilderID();
							Object* myDozer = TheGameLogic->findObjectByID(builder);

              if (myDozer && ( myDozer->getControllingPlayer() != m_player || myDozer->isDisabledByType( DISABLED_UNMANNED ) ) )
              {//I don't expect this dozer to work well with me.
                myDozer = nullptr;
                bldg->setBuilder( nullptr );
              }

							if (myDozer==nullptr) {
								DEBUG_LOG(("AI's Dozer got killed (or captured).  Find another dozer."));
								queueDozer();
 								myDozer = findDozer(bldg->getPosition());
								if (myDozer==nullptr || myDozer->getAI()==nullptr) {
									continue;
								}
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}	else {
								// make sure he is building.
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}
						}
					} else {
						// oops, got captured.
						info->setObjectID(INVALID_ID);
						info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					}
				}
			}
			if (info->getObjectID()==INVALID_ID && info->getObjectTimestamp()>0) {
				// this object was built at some time, and got destroyed at or near objectTimestamp.
				// Wait a few seconds before initiating a rebuild.
				if (info->getObjectTimestamp()+TheAI->getAiData()->m_rebuildDelaySeconds*LOGICFRAMES_PER_SECOND > TheGameLogic->getFrame()) {
					continue;
				}	else {
					DEBUG_LOG(("Enabling rebuild for %s", info->getTemplateName().str()));
					info->setObjectTimestamp(0); // ready to build.
				}
			}
			if (bldg) {
				continue; // already built.
			}
			// Make sure it is safe to build here.
			// GeneralsX @feature Claude 01/10/2026 A supply centre whose dock-face spot is not safe moves to today's spot at once and
			// is tested there, instead of waiting for the face to clear (useFallbackSpot is one-shot; see AIPlayer::buildBySupplies).
			// GeneralsX @bugfix Claude 27/10/2026 ...but only when today's spot IS safe. Moving to a spot that is just as unsafe gave up
			// the face for nothing: the entry then waited there and the centre was built off the dock with 3 trucks (wh1ter0se's 27/10
			// 0.9.68 game: all 6 unsafe refusals had an unsafe today's spot, gx-handoff ai/china-fallback). Now the entry waits on
			// the face and moves the first time today's spot is safe and the face is not.
			if (!isLocationSafe(info->getLocation(), curPlan)) {
				const Coord3D *fallback = info->getFallbackLocation();
				if (fallback == nullptr || !isLocationSafe(fallback, curPlan)) {
					continue;
				}
				// GeneralsX 06/11/2026 base-layout (ADDENDUM_8 (c)): a keyed entry's layout fallback, the stock spot, is taken the same way, but only by
				// the entry this pass builds: it stays a candidate here, unmoved, and moves below once it is the pick (Codex bl-fb-r2 P2). One this
				// pass could not pick at all (not a priority request, not automatic, not a forced power plant: a tech building the scripts have not
				// asked for yet) waits on its layout spot as before (Codex bl-fb-r1 P2). Either way a passing enemy cannot spend its one move.
				if (info->hasLayoutFallback()) {
					const Bool gxCanPick = info->isPriorityBuild() || info->isAutomaticBuild()
						|| (isUnderPowered && curPlan->isKindOf(KINDOF_FS_POWER) && !curPlan->isKindOf(KINDOF_CASH_GENERATOR));
					if (!gxCanPick) {
						continue;
					}
				} else {
					info->useFallbackSpot();
				}
			}
			if (info->isPriorityBuild()) {
				// Always take priority build, unless we already have priority build.
				if (!isPriority) {
					bldgPlan = curPlan;
					bldgInfo = info;
					isPriority = true;
				}
			}
			if (curPlan->isKindOf(KINDOF_FS_POWER)) {
				if (powerPlan==nullptr && !curPlan->isKindOf(KINDOF_CASH_GENERATOR)) {
					if (isUnderPowered || info->isAutomaticBuild()) {
						powerPlan = curPlan;
						powerInfo = info;
					}
				}
			}
			if (!info->isAutomaticBuild()) {
				continue; // marked to not build automatically.
			}
			Object *dozer = findDozer(info->getLocation());
			if (dozer==nullptr) {
				if (isUnderPowered) {
					queueDozer();
				}
				continue;
			}
			if (TheBuildAssistant->canMakeUnit(dozer, bldgPlan)!=CANMAKE_OK) {
				if (info->isBuildable()) {
					AsciiString bldgName = info->getTemplateName();
					bldgName.concat(" - Dozer unable to build - money or technology missing.");
					TheScriptEngine->AppendDebugMessage(bldgName, false);
				}
				continue;
			}
			// check if this building has any "rebuilds" left
			if (info->isBuildable())
			{
				if (bldgPlan == nullptr) {
					bldgPlan = curPlan;
					bldgInfo = info;
				}
			}
		}
		if (powerInfo && powerPlan && !powerPlan->isEquivalentTo(bldgPlan)) {
			if (!powerUnderConstruction) {
				bldgPlan = powerPlan;
				bldgInfo = powerInfo;
				DEBUG_LOG(("Forcing build of power plant."));
			}
		}
		if (bldgPlan && bldgInfo) {
			// GeneralsX 06/11/2026 base-layout (ADDENDUM_8 (c), Codex bl-fb-r2 P2): the keyed entry this pass builds, kept a candidate above with its
			// layout spot unsafe and its stock spot safe (the same test, this same frame), moves to the stock spot now.
			if (bldgInfo->hasLayoutFallback() && !isLocationSafe(bldgInfo->getLocation(), bldgPlan)) {
				GX_ROT_LOG("p=%d ev=fallback why=unsafe tmpl=%s x=%.2f y=%.2f", (Int)m_player->getPlayerIndex(), bldgInfo->getTemplateName().str(),
					bldgInfo->getFallbackLocation()->x, bldgInfo->getFallbackLocation()->y);
				bldgInfo->useFallbackSpot();
			}
#ifdef USE_DOZER
			// dozer-construct the building
			bldg = buildStructureWithDozer(bldgPlan, bldgInfo);
			// store the object with the build order
			if (bldg)
			{
				bldgInfo->setObjectID( bldg->getID() );
				bldgInfo->decrementNumRebuilds();

				m_readyToBuildStructure = false;
				m_structureTimer = TheAI->getAiData()->m_structureSeconds*static_cast<float>(LOGICFRAMES_PER_SECOND);
				if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
				}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
					m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
				}
				m_frameLastBuildingBuilt = TheGameLogic->getFrame();
				// only build one building per delay loop
			}

#else
			// force delay between rebuilds
			Int framesToBuild = bldgPlan->calcTimeToBuild(m_player);
			if (TheGameLogic->getFrame() - m_frameLastBuildingBuilt < framesToBuild)
			{
				m_buildDelay = framesToBuild - (TheGameLogic->getFrame() - m_frameLastBuildingBuilt);
				return;
			}	else {
				// building is missing, (re)build it
				// deduct money to build, if we have it
				Int cost = bldgPlan->calcCostToBuild( m_player );
				if (m_player->getMoney()->countMoney() >= cost)
				{
					// we have the money, deduct it
					m_player->getMoney()->withdraw( cost );

					// inst-construct the building
					bldg = buildStructureNow(bldgPlan, bldgInfo);
					// store the object with the build order
					if (bldg)
					{
						bldgInfo->setObjectID( bldg->getID() );
						bldgInfo->decrementNumRebuilds();

						m_readyToBuildStructure = false;
						m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
						if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
						}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
						}
						m_frameLastBuildingBuilt = TheGameLogic->getFrame();
					}
				}
			}
#endif
		}
	}
}

/**
 * Invoked when a unit I am training comes into existence
 */
void AISkirmishPlayer::onUnitProduced( Object *factory, Object *unit )
{
	AIPlayer::onUnitProduced(factory, unit);
}

/**
 * Search the computer player's buildings for one that can build the given request
 * and start training the unit.
 * If busyOK is true, it will queue a unit even if one is building.  This lets
 * script invoked teams "push" to the front of the queue.
 */
Bool AISkirmishPlayer::startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName)
{
	Object *factory = findFactory(order->m_thing, busyOK);
	if( factory )
	{
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu && pu->queueCreateUnit( order->m_thing, pu->requestUniqueUnitID() )) {
			order->m_factoryID = factory->getID();
			if (TheGlobalData->m_debugAI) {
				AsciiString teamStr = "Queuing ";
				teamStr.concat(order->m_thing->getName());
				teamStr.concat(" for ");
				teamStr.concat(teamName);
				TheScriptEngine->AppendDebugMessage(teamStr, false);
			}
			return true;
		}
	}

	return FALSE;

}


// GeneralsX @feature Claude 08/10/2026 Ported from Zero Hour Reforged (GPL-3.0, Olcay Seygan, commit 87e007e:
// GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AI.cpp aiHoardAdjustedDelay / aiHoardAllowedTeamInstances, AIPlayer.cpp
// scriptAttacks / aiTeamAttacks), unchanged except NULL -> nullptr, file-local linkage and integer
// arithmetic in aiHoardAdjustedDelay. The threshold is ours
// (AIPlayer::hoardThreshold), not Reforged's per-difficulty 4000/6000, so a $10,000-start match never reaches it.

/** Shorten a build delay when the money is piling up.  Twice the threshold halves the delay, four times quarters it,
	* and it stops there - this is a decision to spend faster, not a production multiplier. */
static Int aiHoardAdjustedDelay( Int frames, Int money, Int hoardAt )
{
	if( hoardAt <= 0 || money <= hoardAt || frames <= 1 )
		return frames;

	// GeneralsX: integer ceilings instead of Reforged's float scale - this runs in the lockstep simulation, and
	// REAL_TO_INT_CEIL has two platform variants (BaseType.h).
	Int out = (Int)(((Int64)frames * hoardAt + money - 1) / money);	// 2x the cash -> half the wait
	const Int floor = (frames + 3) / 4;																// and no faster than four times
	if( out < floor )
		out = floor;
	return (out < 1) ? 1 : out;
}

/** How many copies of one team a skirmish AI may field.  The skirmish scripts cap the army by their roster: late in a
	* match every attack team left to build allows one copy, so the AI replaces its losses and banks the rest.  Every two
	* thresholds of cash allows one more copy of the teams the script already permits, up to three more.  A team the data
	* never allows stays at zero, and no threshold leaves EA's limit alone. */
static Int aiHoardAllowedTeamInstances( Int maxInstances, Int money, Int hoardAt )
{
	const Int MAX_EXTRA_INSTANCES = 3;

	if( hoardAt <= 0 || maxInstances <= 0 )
		return maxInstances;

	Int extra = money / (2 * hoardAt);
	if( extra > MAX_EXTRA_INSTANCES )
		extra = MAX_EXTRA_INSTANCES;
	return maxInstances + extra;
}

/** A script, or one it hands the team on to, that sends the team at the enemy: an attack order, a wait on the flag the
	* skirmish scripts raise to launch a wave, or a count towards the next one. */
/** GeneralsX (Codex spend-r6): the action orders the team that runs the script, not some other team. Every retail
	* skirmish attack action names "<This Team>" (254 of 254); a mod's guard hook that sends ANOTHER team does not make
	* the guard an attacker. */
static Bool ordersThisTeam( ScriptAction *action )
{
	if( action->getNumParameters() < 1 )
		return FALSE;
	const Parameter *team = action->getParameter( 0 );
	return team->getParameterType() == Parameter::TEAM && team->getString() == THIS_TEAM;
}

static Bool scriptAttacks( const Script *script, Int depth, Bool falseToo )
{
	if( script == nullptr || depth > 4 )
		return FALSE;
	// GeneralsX (Codex spend-r10): conditions only where they are evaluated - a hook or generic script (depth 0); a
	// sequential script runs its action list and ignores its conditions. No retail sequential script attacks only by one.
	for( OrCondition *orCond = depth == 0 ? script->getOrCondition() : nullptr; orCond; orCond = orCond->getNextOrCondition() )
		for( Condition *cond = orCond->getFirstAndCondition(); cond; cond = cond->getNext() )
			if( cond->getConditionType() == Condition::FLAG && cond->getParameter( 0 )->getString().startsWith( "_LAUNCH_ATTACK" ) )
				return TRUE;
	// GeneralsX (Codex spend-r7, r8): only branches the engine can run - an OnCreate/OnIdle hook runs through
	// executeScript, which takes the false actions when the conditions fail; generic and sequential scripts run the true
	// actions only. Retail has no attack action in a false branch.
	for( Int branch = 0; branch < (falseToo ? 2 : 1); ++branch )
	{
		for( ScriptAction *action = branch ? script->getFalseAction() : script->getAction(); action; action = action->getNext() )
		{
			switch( action->getActionType() )
			{
				case ScriptAction::SKIRMISH_FOLLOW_APPROACH_PATH:
				case ScriptAction::SKIRMISH_MOVE_TO_APPROACH_PATH:
					if( ordersThisTeam( action ) )
						return TRUE;
					break;
				default:
					break;
			}
			for( Int i = 0; i < action->getNumParameters(); ++i )
			{
				const Parameter *param = action->getParameter( i );
				if( param->getParameterType() == Parameter::COUNTER && param->getString().startsWith( "_COUNTER_FOR_ATTACK" ) )
					return TRUE;
				// only a script the team is set to run; following every script a script names reached the
				// subroutine lists and called the garrison teams and the base expanders attackers
				const ScriptAction::ScriptActionType type = action->getActionType();
				if( (type == ScriptAction::TEAM_EXECUTE_SEQUENTIAL_SCRIPT || type == ScriptAction::TEAM_EXECUTE_SEQUENTIAL_SCRIPT_LOOPING) &&
						param->getParameterType() == Parameter::SCRIPT && ordersThisTeam( action ) &&
						scriptAttacks( TheScriptEngine->findScriptByName( param->getString() ), depth + 1, FALSE ) )
					return TRUE;
			}
		}
	}
	return FALSE;
}

/** A team the skirmish scripts send at the enemy at some point, as against one they keep at home on guard: the tunnel
	* guards, the fire base crew, the palace garrison. */
// GeneralsX (Codex spend-r13): the script group ScriptEngine::runScript would find for this name (ScriptEngine::findGroup,
// which is protected): it looks for a group first.
static ScriptGroup *findHookGroup( const AsciiString &name )
{
	for( Int i = 0; i < TheSidesList->getNumSides(); ++i )
	{
		ScriptList *list = TheSidesList->getSideInfo( i )->getScriptList();
		for( ScriptGroup *group = list ? list->getScriptGroup() : nullptr; group; group = group->getNext() )
			if( group->getName() == name )
				return group;
	}
	return nullptr;
}

/** An OnCreate/OnIdle hook as ScriptEngine::runScript resolves it - a subroutine group (each of its scripts) before a
	* subroutine script - and executeScript filters it: a script not flagged for the AI's difficulty is skipped, and only a
	* Hard AI hoards. Only fixed flags are read, never isActive(), which scripts change during the match and a save does not
	* keep in this cache (Codex spend-r9, r11, r12, r13). All 429 retail Hard hooks are Hard subroutine scripts. */
static Bool lifecycleHookAttacks( const AsciiString &name )
{
	if( name.isEmpty() )
		return FALSE;
	if( ScriptGroup *group = findHookGroup( name ) )
	{
		if( !group->isSubroutine() )
			return FALSE;
		for( Script *script = group->getScript(); script; script = script->getNext() )	// executeScripts skips subroutines (Codex spend-r14)
			if( !script->isSubroutine() && script->isHard() && scriptAttacks( script, 0, TRUE ) )
				return TRUE;
		return FALSE;
	}
	const Script *script = TheScriptEngine->findScriptByName( name );
	return script != nullptr && script->isSubroutine() && script->isHard() && scriptAttacks( script, 0, TRUE );
}

static Bool hookAttacks( const AsciiString &name )
{	// GeneralsX (Codex spend-r1): an empty hook names no script, and findScriptByName scans every side's list to say so
	return !name.isEmpty() && scriptAttacks( TheScriptEngine->findScriptByName( name ), 0, FALSE );
}

static Bool aiTeamAttacks( const TeamTemplateInfo *info )
{
	if( lifecycleHookAttacks( info->m_scriptOnCreate ) || lifecycleHookAttacks( info->m_scriptOnIdle ) )
		return TRUE;
	for( Int i = 0; i < MAX_GENERIC_SCRIPTS; ++i )
		if( hookAttacks( info->m_teamGenericScripts[ i ] ) )
			return TRUE;
	return FALSE;
}

/**
 * Check if this team is buildable, doesn't exceed maximum limits, meets conditions, and isn't under construction.
 */
Bool AISkirmishPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!gxAirTeamAllowed(proto, FALSE))	// GeneralsX 27/10/2026 ai-pro-rules-2 T2: the AA team only while a hostile airfield stands
		return false;
	// Check condition.
	if (!proto->evaluateProductionCondition()) {
		return false;
	}
	// check build limit, which a hoard raises for attack teams; defence teams keep the data's, and so do the guard teams
	// the scripts never send (GeneralsX: Reforged's rule, see aiHoardAllowedTeamInstances above). The script walk runs
	// only for a team already at its limit while hoarding.
	const TeamTemplateInfo *info = proto->getTemplateInfo();
	Int allowedInstances = info->m_maxInstances;
	// A singleton keeps its one copy: createInactiveTeam hands back the live team and an automatic build skips the
	// singleton guard, so a raised limit would only pour units into it (Codex spend-r1). The classification is cached
	// per team for this match - the scripts never change during it.
	if (proto->countTeamInstances() >= allowedInstances && !proto->getIsSingleton() && isHoarding()
			&& !info->m_isBaseDefense && !info->m_isPerimeterDefense) {
		const Int more = aiHoardAllowedTeamInstances( allowedInstances, m_player->getMoney()->countMoney(), hoardThreshold() );
		if (more > allowedInstances) {
			std::map<const TeamPrototype*, Bool>::iterator it = m_hoardAttackTeams.find(proto);
			if (it == m_hoardAttackTeams.end())
				it = m_hoardAttackTeams.insert(std::make_pair((const TeamPrototype*)proto, aiTeamAttacks(info))).first;
			if (it->second)
				allowedInstances = more;
		}
	}
	if (proto->countTeamInstances() >= allowedInstances){
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			str.format("Team %s not chosen - %d already exist.", proto->getName().str(), proto->countTeamInstances());
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;	// Max already built.
	}

	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		if (team->m_team->getPrototype() == proto) {
			return false; // currently building one of these.
		}
	}
	Bool needMoney;
	if (!isPossibleToBuildTeam( proto, true, needMoney)) {
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			if (needMoney) {
				str.format("Team %s not chosen - Not enough money.", proto->getName().str());
			} else {
				str.format("Team %s not chosen - Factory/tech missing or busy.", proto->getName().str());
			}
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;
	}
	return true;
}

/**
 * See if any existing teams need reinforcements, and have higher priority.
 */
Bool AISkirmishPlayer::selectTeamToReinforce( Int minPriority )
{
	return AIPlayer::selectTeamToReinforce(minPriority);
}

/**
 * Determine the next team to build.  Return true if one was selected.
 */
Bool AISkirmishPlayer::selectTeamToBuild()
{
	return AIPlayer::selectTeamToBuild();
}

/**
	Build a specific building.
	*/
void AISkirmishPlayer::buildSpecificAIBuilding(const AsciiString &thingName)
{
	//
	Bool found = false;
	Bool foundUnbuilt = false;
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->getTemplateName()==thingName)
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
			if (!bldgPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
			found = true;
			if (bldg) {
				continue; // already built.
			}
			if (info->isPriorityBuild()) {
				continue; // already marked for priority build.
			}
			foundUnbuilt = true;
			info->markPriorityBuild();
			break;
		}
	}
	if (foundUnbuilt) {
		m_buildDelay = 0;
		AsciiString buildingStr = "Queueing building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' for construction.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else if (found) {
		AsciiString buildingStr = "Warning - all instances of building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' are already built or queued for build, not queueing.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}	else {
		AsciiString buildingStr = "Error - could not find building '";
		buildingStr.concat(thingName);
		buildingStr.concat("' in the building template list.");
		TheScriptEngine->AppendDebugMessage(buildingStr, false);
	}
}



/**
	Gets the player index of my enemy.
	*/
Int AISkirmishPlayer::getMyEnemyPlayerIndex() {
	Int playerNdx;
	if (m_currentEnemy) {
		return m_currentEnemy->getPlayerIndex();
	}
	// For now, return first human player, as there should only be one. jba
	for (playerNdx=0; playerNdx<ThePlayerList->getPlayerCount(); playerNdx++) {
		if (ThePlayerList->getNthPlayer(playerNdx)->getPlayerType() == PLAYER_HUMAN) {
			break;
		}
	}
	return playerNdx;
}

/**
	Get the AI's enemy.  Recalc if it has been a while (5 seconds.)
*/
void AISkirmishPlayer::acquireEnemy()
{
	Player *bestEnemy = nullptr;
	Real bestDistanceSqr = HUGE_DIST*HUGE_DIST;

	// GeneralsX @feature Claude 29/09/2026 Smarter target picking for Normal and Hard skirmish AIs (wh1ter0se: "yes, build
	// a test"). Retail kept the current enemy until it had no units or no build facility, and its two tie-breakers below
	// (+500^2 when another AI already targets him, -25^2 when he targets me) are negligible against squared base
	// distances of ~3.6-6.8 M on a 12-spawn map - so every AI fought its nearest neighbour to the end and never answered
	// whoever was actually in its base. Normal and Hard now re-score every enemy: squared distance x (1 + 0.5 per OTHER
	// AI already on him, for a candidate I would switch TO) x 0.25 if he has >= 3 combat units inside my base, and switch
	// only when the best is clearly better (score < 0.7 x the current enemy's) or the current enemy is gone or crippled.
	// A healthy current enemy is re-scored at most once a minute, counted from when I last chose or re-scored him (so
	// a new pick is held a minute): re-scoring on every 5 s check made about half of all switches flip back within
	// 10-25 s (measured, 12-AI A/B b4), and the choice steers attack waves, defence placement and expansion. A crippled
	// one is still dropped on the next 5 s check. The minute is per AI, not a global window, because getAiEnemy() runs
	// on demand and a caller could keep missing a window (Codex r4/r5). No new saved state (m_frameEnemyReviewed is an
	// unsaved timer, like m_frameToCheckEnemy) and deterministic (players by index; per-player counts from one
	// partition scan, which are order-independent; logic frames only, no wall clock, no per-peer state). Easy keeps the
	// retail rule so it does not get harder.
	const Bool smart = (m_difficulty != DIFFICULTY_EASY);
	const UnsignedInt now = TheGameLogic->getFrame();

	if (m_currentEnemy) {
		Bool inBadShape = !m_currentEnemy->hasAnyUnits() || !m_currentEnemy->hasAnyBuildFacility();
		if (!inBadShape && !smart) return;
		if (!inBadShape && now - m_frameEnemyReviewed < 60*LOGICFRAMES_PER_SECOND) return;
	}
	if (smart) m_frameEnemyReviewed = now;

	Int invaders[MAX_PLAYER_COUNT];
	for (Int p = 0; p < MAX_PLAYER_COUNT; ++p) invaders[p] = 0;
	if (smart && m_baseCenterSet) {
		const Real range = (m_baseRadius > 300.0f ? m_baseRadius : 300.0f) + 300.0f;
		// Undetected stealth units are skipped, as in AIPlayer::isLocationSafe (Codex r5). Fog is deliberately NOT
		// applied (Codex r6 asked for it): retail computer players ignore fog - AIUpdate sets AI::UNFOGGED only for
		// human-owned units, and every enemy base is located through fog by its structure bounds - so stealth is the one
		// visibility rule the retail AI respects, and this keeps to it.
		PartitionFilterRejectByObjectStatus filterStealth( MAKE_OBJECT_STATUS_MASK( OBJECT_STATUS_STEALTHED ),
			MAKE_OBJECT_STATUS_MASK2( OBJECT_STATUS_DETECTED, OBJECT_STATUS_DISGUISED ) );
		PartitionFilter *filters[] = { &filterStealth, nullptr };
		ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(&m_baseCenter, range, FROM_CENTER_2D, filters, ITER_FASTEST);
		MemoryPoolObjectHolder holder(iter);
		for (Object *obj = iter->first(); obj; obj = iter->next()) {
			if (obj->isEffectivelyDead() || obj->isKindOf(KINDOF_STRUCTURE) || obj->isKindOf(KINDOF_PROJECTILE) || obj->isKindOf(KINDOF_DRONE)) continue;
			if (!obj->isKindOf(KINDOF_INFANTRY) && !obj->isKindOf(KINDOF_VEHICLE) && !obj->isKindOf(KINDOF_AIRCRAFT)) continue;
			// Workers, dozers and harvesters are not an attack (Codex r1): only units that can fight count.
			if (obj->isKindOf(KINDOF_HARVESTER) || obj->isKindOf(KINDOF_DOZER) || !obj->isAbleToAttack()) continue;
			const Player *owner = obj->getControllingPlayer();
			if (owner == nullptr || owner == m_player) continue;
			const Int ndx = owner->getPlayerIndex();
			if (ndx >= 0 && ndx < MAX_PLAYER_COUNT) ++invaders[ndx];
		}
	}
	Real currentScore = HUGE_DIST*HUGE_DIST;
	Bool currentValid = FALSE;

	// look for the closest enemy.
	Int i;
	for (i=0; i<ThePlayerList->getPlayerCount(); i++) {
		Player *curPlayer = ThePlayerList->getNthPlayer(i);
		if (m_player->getRelationship(curPlayer->getDefaultTeam()) == ENEMIES) {
			if (curPlayer->hasAnyObjects()==false) continue; // not much of an enemy.
			// ok, we got an enemy;
			// If a player is out of units, or out of build facilities, we can lower his priority.
			Bool inBadShape = !curPlayer->hasAnyUnits() || !curPlayer->hasAnyBuildFacility();

			Coord3D enemyPos = m_baseCenter;
			Region2D bounds;
			getPlayerStructureBounds(&bounds, i);
			enemyPos.x = bounds.lo.x + bounds.width()/2;
			enemyPos.y = bounds.lo.y + bounds.height()/2;
			Real curDistSqr = sqr(enemyPos.x-m_baseCenter.x) + sqr(enemyPos.y-m_baseCenter.y);

			//Fudge for in bad shape.	 If an enemy is crippled, concentrate on the other ones.
			if (inBadShape) {
				curDistSqr = HUGE_DIST*HUGE_DIST*0.5f;
			}
			if (smart && !inBadShape) {
				// A crippled enemy keeps its fixed "last resort" score: multiplying it could push it past the initial
				// best and leave an AI locked on an eliminated player (Codex r1).
				// Do not gang up - but only as a reason not to JOIN: leaving my own enemy because an ally joined me
				// was the other half of the flip-flopping.
				if (curPlayer != m_currentEnemy) {
					Int others = 0;
					for (Int k=0; k<ThePlayerList->getPlayerCount(); k++) {
						if (k==i) continue;
						Player *somePlayer = ThePlayerList->getNthPlayer(k);
						if (somePlayer == m_player) continue; // my own choice is not "someone else already on him"
						// An eliminated AI keeps its AI and its last enemy (killPlayer leaves both), so it would
						// count forever (Codex r3).
						if (!somePlayer->isPlayerActive()) continue;
						if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==curPlayer)) {
							others++;
						}
					}
					curDistSqr *= 1.0f + 0.5f*others;
				}
				const Int ndx = curPlayer->getPlayerIndex();
				if (ndx >= 0 && ndx < MAX_PLAYER_COUNT && invaders[ndx] >= 3) {
					curDistSqr *= 0.25f;
				}
				if (curPlayer == m_currentEnemy) {
					currentScore = curDistSqr;
					currentValid = TRUE;
				}
			} else if (!smart) {
				// See if other ai's are attacking this target.
				// We don't want the ai's to gang up on one enemy.
				Int k;
				for (k=0; k<ThePlayerList->getPlayerCount(); k++) {
					if (k==i) continue;  // don't count self.
					Player *somePlayer = ThePlayerList->getNthPlayer(k);
					if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==curPlayer)) {
						// Some ai is already targeting this guy.  Add a distance penalty.
						curDistSqr += (500*500);
					}
					if (somePlayer->isSkirmishAIPlayer() && (somePlayer->getCurrentEnemy()==m_player)) {
						// he is attacking me.  So I will (gently) prefer to attack him.
						curDistSqr -= (25*25);
						if (curDistSqr<0) curDistSqr = 0;
					}
				}
			}

			// Ai enemy - will take if we don't get a better offer.
			if (curDistSqr<bestDistanceSqr || (smart && bestEnemy == nullptr)) {
				bestEnemy = curPlayer;
				bestDistanceSqr = curDistSqr;
			}
		}
	}
	// Hysteresis: a healthy current enemy is kept unless the best alternative is clearly better.
	if (smart && currentValid && bestEnemy != m_currentEnemy && !(bestDistanceSqr < 0.7f*currentScore)) {
		return;
	}
	if (bestEnemy!=nullptr && (bestEnemy!=m_currentEnemy)) {
		m_currentEnemy = bestEnemy;
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" acquiring target enemy player: ");
		msg.concat(TheNameKeyGenerator->keyToName(m_currentEnemy->getPlayerNameKey()));
		TheScriptEngine->AppendDebugMessage( msg, false);
	}
}



/**
	Get the AI's enemy.  Recalc if it has been a while (20 seconds.)
*/
Player *AISkirmishPlayer::getAiEnemy()
{
	if (TheGameLogic->getFrame()>=m_frameToCheckEnemy) {
		m_frameToCheckEnemy = TheGameLogic->getFrame() + 5*LOGICFRAMES_PER_SECOND;
		acquireEnemy();
	}
	return m_currentEnemy;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ R (gx-handoff ai/wh1ter0se-econ/DESIGN.md 6): the base FRONT - what front base defences
// aim at (buildAIBaseDefenseStructure, !flank): the waypoint of this player's own Center path closest to its base centre, else the
// centre of its enemy's structures. FALSE before the base centre is known.
// ------------------------------------------------------------------------------------------------
Bool AISkirmishPlayer::gxFrontPoint(Coord3D *out)
{
	if (!m_baseCenterSet)
		return FALSE;
	// GeneralsX 04/11/2026 base-layout: GXFrontDoors reads the one fixed point the layout turn and the door rule share
	if (gxFrontDoors()) {
		// the start, as the layout turn resolves it (Codex bl-engine-r2): from the list's centre the two could fall either side of GX_FRONT_MIN_DIST and
		// disagree about the front. The start waypoint, not the list's command centre: gxRelocateQueued can move a destroyed one's entry (Codex bl-engine-r3).
		Coord3D anchor = m_baseCenter;
		gxStartPoint(&anchor);
		gxBaseFrontFrom(anchor, out);
		return TRUE;
	}
	Waypoint *way = gxFrontWaypoint();
	if (way) {
		*out = *way->getLocation();
		return TRUE;
	}
	Region2D bounds;
	getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
	// no enemy structure leaves the bounds zeroed (getPlayerStructureBounds); ONE structure gives zero width and height at its centre,
	// which is a valid front (Codex ke-rotate-r1 P2)
	if (bounds.lo.x == 0 && bounds.lo.y == 0 && bounds.hi.x == 0 && bounds.hi.y == 0)
		return FALSE;
	out->x = bounds.lo.x + bounds.width()/2;
	out->y = bounds.lo.y + bounds.height()/2;
	out->z = 0;
	return TRUE;
}

// GeneralsX @feature Claude 28/10/2026 gxFrontPoint's first answer: the waypoint of this player's own Center path closest to its base
// centre (moved out of gxFrontPoint unchanged).
Waypoint *AISkirmishPlayer::gxFrontWaypoint()
{
	AsciiString pathLabel;
	pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
	Coord3D goalPos = m_baseCenter;
	return TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
}

// GeneralsX @feature Claude 28/10/2026 TRUE when gxFrontPoint's answer cannot change for the rest of the game: the base centre is set once
// (computeCenterAndRadiusOfBase, newMap), so an unset one never gives a front, and a set one with a Center path always gives that path's
// same waypoint. Only the other answer - the centre of the enemy's structures - moves (AIPlayer.cpp gxBuildAngles). Reads only.
Bool AISkirmishPlayer::gxFrontPointFixed()
{
	return !m_baseCenterSet || gxFrontDoors() || gxFrontWaypoint() != nullptr;
}

// GeneralsX @feature Claude 04/11/2026 base-layout (gx-handoff ai/base-layout/PLAN_base-layout.md, wh1ter0se 03/11 "look at the rotation facing back of the
// base"): this side's list in the AI data. Looked up by side each time: nothing is saved, and the same data gives the same answer after a load
// (dataID guarantees the data). A side with no list has no keys.
const AISideBuildList *AISkirmishPlayer::gxSideList() const
{
	const AsciiString &side = m_player->getSide();
	for (const AISideBuildList *build = TheAI->getAiData()->m_sideBuildLists; build; build = build->m_next) {
		if (build->m_side == side)
			return build;
	}
	return nullptr;
}

// GeneralsX @feature Claude 06/11/2026 base-layout (gx-handoff ai/base-layout/ADDENDUM_9.md, lead C70): a keyed layout moves the base centre, and with
// it which supply depot findSupplyCenter ranks nearest: measured, the keyed AI expanded to the far depot first and found the near one enemy-held
// when it turned to it (Arctic: keyed China $10k had 71 supply centres by 10:00 against stock's 116). A keyed list ranks depots, and offsets a
// supply centre, from the STOCK centre instead: computeCenterAndRadiusOfBase's own sum, in the same order, over the stock spots the entries fall
// back to (placed by adjustBuildList with the retail transform), so it is bit for bit the m_baseCenter the stock list gives. Only when every entry
// carries a layout fallback; any other list keeps m_baseCenter.
void AISkirmishPlayer::gxComputeStockCenter()
{
	m_gxStockCenterSet = FALSE;
	m_gxStockCenter.zero();
	const AISideBuildList *gxList = gxSideList();
	if (gxList == nullptr || !gxList->m_gxTurnLayout)
		return;
	Coord2D total;
	total.x = 0;
	total.y = 0;
	Int n = 0;
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext())
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		if (TheThingFactory->findTemplate(name) == nullptr) continue;
		if (!info->hasLayoutFallback())
			return;	// an entry with no stock spot: not the stock list's centre, keep m_baseCenter
		const Coord3D *fb = info->getFallbackLocation();
		total.x += fb->x;
		total.y += fb->y;
		n++;
	}
	if (n == 0)
		return;
	m_gxStockCenter.x = total.x / n;
	m_gxStockCenter.y = total.y / n;
	m_gxStockCenterSet = TRUE;
}

Bool AISkirmishPlayer::gxFrontDoors()
{
	const AISideBuildList *build = gxSideList();
	return build != nullptr && build->m_gxFrontDoors;
}

// GeneralsX @feature Claude 04/11/2026 base-layout: the base front seen from a point, for a keyed list: the closest waypoint of this player's
// Center path when it is at least GX_FRONT_MIN_DIST away (map-centers.py: 20 of 311 starts sit under 150 wu, down to 22, where the direction
// means nothing), else the centre of the map's largest pathfind extent (14 of 311 starts have no Center path). It never depends on the enemy's
// structures, so it cannot move once the base centre is set. Returns TRUE when the answer is the waypoint.
static const Real GX_FRONT_MIN_DIST = 150.0f;

// GeneralsX @feature Claude 04/11/2026 base-layout: this player's start, which nothing moves: the waypoint its command centre was placed on (GameLogic.cpp
// placeNetworkBuildingsForPlayer). FALSE, out untouched, when the map has none.
Bool AISkirmishPlayer::gxStartPoint(Coord3D *out)
{
	AsciiString name;
	name.format("Player_%d_Start", m_player->getMpStartIndex()+1);
	Waypoint *way = TheTerrainLogic->getWaypointByName(name);
	if (way == nullptr)
		return FALSE;
	*out = *way->getLocation();
	return TRUE;
}

Bool AISkirmishPlayer::gxBaseFrontFrom(const Coord3D &from, Coord3D *out)
{
	AsciiString pathLabel;
	pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
	Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &from, pathLabel );
	if (way) {
		const Coord3D *at = way->getLocation();
		const Real dx = at->x - from.x;
		const Real dy = at->y - from.y;
		if (dx*dx + dy*dy >= GX_FRONT_MIN_DIST*GX_FRONT_MIN_DIST) {
			*out = *at;
			return TRUE;
		}
	}
	Region3D bounds;
	TheTerrainLogic->getMaximumPathfindExtent(&bounds);
	out->x = bounds.lo.x + bounds.width()/2;
	out->y = bounds.lo.y + bounds.height()/2;
	out->z = 0;
	return FALSE;
}

/**
	Build base defense structures on the front or flank of the base.
*/
void AISkirmishPlayer::buildAIBaseDefense(Bool flank)
{
	const AISideInfo *resInfo = TheAI->getAiData()->m_sideInfo;
	AsciiString defenseTemplateName;
	while (resInfo) {
		if (resInfo->m_side == m_player->getSide()) {
			defenseTemplateName = resInfo->m_baseDefenseStructure1;
			break;
		}
		resInfo = resInfo->m_next;
	}
	if (resInfo) {
		buildAIBaseDefenseStructure(resInfo->m_baseDefenseStructure1, flank);
	}
}

/**
	Build base defense structures on the front or flank of the base.
	Base defenses are placed as follows:
	m_baseCenter and m_baseRadius are calculated on map load.
	Defenses are placed along the this circle.
	Front defenses (!flank) are placed starting at the "Center" approach path.
	The first front defense is placed towards th Center path.  Number 2 is placed
	to the left of #1, #3 is placed to the right of #1, #4 is placed to the left of
	#2 and so on.  So it looks like:

												#1
									 #2 			#3
					#6  #4								 #5	  #7
		  #8																	#9

	The flank base defenses cover the "Flank" approach, and the "Backdoor" approach.
	They alternate between these two, so the first flank defense covers flank, and the second
	covers backdoor, and continue to alternate.  They cover the approach using the same
	pattern as front above.
	John A.

	*/
void AISkirmishPlayer::buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank)
{
	const ThingTemplate *tTemplate = TheThingFactory->findTemplate(thingName);
	if (tTemplate==nullptr) {
		DEBUG_CRASH(("Couldn't find base defense structure '%s' for side %s", thingName.str(), m_player->getSide().str()));
		return;
	}
	do {
		AsciiString pathLabel;
		if (flank) {
			if (m_curFlankBaseDefense&1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
			}	else {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
			}
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			if (flank) return;
			Region2D bounds;
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		Real defenseDistance = m_baseRadius;
		// GeneralsX @feature Claude 05/11/2026 base-layout (ADDENDUM_3): a redrawn list is bigger than stock, and the ring followed it out onto ground a base was never meant
		// to reach: a try refused on terrain still spends the arc below, so the requests after it were dropped. GXRingRadius keeps the ring as near stock as the list's
		// own buildings allow on the whole circle, since flank and backdoor requests land all around it. A list without the key keeps the base radius.
		const AISideBuildList *gxRingList = gxSideList();
		const Bool gxRingKeyed = gxRingList != nullptr && gxRingList->m_gxRingRadius > 0.0f;
		if (gxRingKeyed) {
			defenseDistance = gxRingList->m_gxRingRadius;
		}
		defenseDistance += TheAI->getAiData()->m_skirmishBaseDefenseExtraDistance;
		offset.x *= defenseDistance;
		offset.y *= defenseDistance;

		Real structureRadius = tTemplate->getTemplateGeometryInfo().getBoundingCircleRadius();
		Real baseCircumference = 2*PI*defenseDistance;
		Real angleOffset = 2*PI*(structureRadius*4/baseCircumference);

		Int selector;
		Real angle;
		if (flank) {
			selector = m_curFlankBaseDefense>>1;
			if (m_curFlankBaseDefense&1) {
				if (selector&1) {
					m_curLeftFlankRightDefenseAngle -= angleOffset;
					angle = m_curLeftFlankRightDefenseAngle;
				}	else {
					angle = m_curLeftFlankLeftDefenseAngle;
					m_curLeftFlankLeftDefenseAngle += angleOffset;
				}
			}	else {
				if (selector&1) {
					m_curRightFlankRightDefenseAngle -= angleOffset;
					angle = m_curRightFlankRightDefenseAngle;
				}	else {
					angle = m_curRightFlankLeftDefenseAngle;
					m_curRightFlankLeftDefenseAngle += angleOffset;
				}
			}

		} else {
			selector = m_curFrontBaseDefense;
			if (selector&1) {
				m_curFrontRightDefenseAngle -= angleOffset;
				angle = m_curFrontRightDefenseAngle;
			}	else {
				angle = m_curFrontLeftDefenseAngle;
				m_curFrontLeftDefenseAngle += angleOffset;
			}
		}

		if (angle > PI/3) break;
		Real s = Sin(angle);
		Real c = Cos(angle);

// TheSuperHackers @info helmutbuhler 21/04/2025 This debug mutates the code to become CRC incompatible
#if defined(RTS_DEBUG) || !RETAIL_COMPATIBLE_CRC
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Angle is %f sin %f, cos %f", 180*angle/PI, s, c));
		DEBUG_LOG(("buildAIBaseDefenseStructure -- Offset is %f  %f, Final Position is %f, %f",
			offset.x, offset.y,
			offset.x*c - offset.y*s,
			offset.y*c + offset.x*s
			));
#endif
		// GeneralsX @feature Claude 05/11/2026 base-layout (ADDENDUM_4): the arc below is spent once per bearing, so a request whose tries stand on ground past the base's edge
		// uses up the arc and the next request is dropped untried. A keyed list looks further in on the same bearing before it moves on, never further out. Any other list
		// makes the one pass it always made.
		Coord3D buildPos = m_baseCenter;
		Bool canBuild = FALSE;
		Real placeAngle = 0.0f;
		static const Real GX_RING_INWARD_STEP = 25.0f;
		const Int gxRingSteps = gxRingKeyed ? 4 : 0;
		for (Int gxStep = 0; gxStep <= gxRingSteps && !canBuild; ++gxStep) {
			Coord2D gxStepOffset = offset;
			if (gxStep > 0) {
				const Real gxStepDistance = defenseDistance - gxStep * GX_RING_INWARD_STEP;
				gxStepOffset.x = offset.x * gxStepDistance / defenseDistance;
				gxStepOffset.y = offset.y * gxStepDistance / defenseDistance;
			}
			buildPos = m_baseCenter;
			buildPos.x += gxStepOffset.x*c - gxStepOffset.y*s;
			buildPos.y += gxStepOffset.y*c + gxStepOffset.x*s;

			/* See if we can build there. */
			placeAngle = tTemplate->getPlacementViewAngle();
			// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ R (DESIGN.md 6): with AI_PLAYER_FACE_FRONT on, a base defence faces away from the
			// base centre - toward the approach it covers - when that angle is legal here; else the template's view angle, as before.
			if (m_gxFaceFront) {
				GX_HOOK_TIMER_ON(gxFaceFrontHook);
				const Real outward = gxOctantAngle(buildPos.x - m_baseCenter.x, buildPos.y - m_baseCenter.y);
				if (LBC_OK == gxLegalToBuildOnGround(&buildPos, tTemplate, outward,
						BuildAssistant::TERRAIN_RESTRICTIONS|BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player)) {
					placeAngle = outward;
				}
				TheTerrainVisual->removeAllBibs();
				GX_ROT_LOG("p=%d ev=defence tmpl=%s view=%d outward=%d turned=%d", (Int)m_player->getPlayerIndex(), tTemplate->getName().str(),
					REAL_TO_INT(tTemplate->getPlacementViewAngle() * 180.0f / PI), REAL_TO_INT(outward * 180.0f / PI), (Int)(placeAngle == outward));
			}
			canBuild = LBC_OK == gxLegalToBuildOnGround(&buildPos, tTemplate, placeAngle,
				BuildAssistant::TERRAIN_RESTRICTIONS|BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player);
			TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
		}
		if (flank) {
			m_curFlankBaseDefense++;
		} else {
			m_curFrontBaseDefense++;
		}
		if (canBuild) {
			m_player->addToPriorityBuildList(thingName, &buildPos, placeAngle);
			break;
		}
	}	while (true);

}


/**
	Checks bridges along a waypoint path.  If any are destroyed, sends a dozer to fix, and returns true.
	If there is no bridge problem, returns false.
	*/
Bool AISkirmishPlayer::checkBridges(Object *unit, Waypoint *way)
{
	Coord3D unitPos = *unit->getPosition();
	AIUpdateInterface *ai = unit->getAI();
	if (!ai) return false; // no ai
	const LocomotorSet& locoSet = ai->getLocomotorSet();
	Waypoint *curWay;
	for (curWay = way; curWay; curWay = curWay->getNext()) {
		if (TheAI->pathfinder()->clientSafeQuickDoesPathExist(locoSet, &unitPos, curWay->getLocation())) {
			continue;
		}
		ObjectID brokenBridge = INVALID_ID;
		if (TheAI->pathfinder()->findBrokenBridge(locoSet, &unitPos, curWay->getLocation(), &brokenBridge)) {
			repairStructure(brokenBridge);
			return true;
		}
	}
	return false;

}


/**
	Build a specific team.  If priorityBuild, put at front of queue with priority set.
	*/
void AISkirmishPlayer::buildSpecificAITeam( TeamPrototype *teamProto, Bool priorityBuild)
{
	gxAirLogTeam(teamProto, "produce");	// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (test-instruments log; the air-watch team only)
	AIPlayer::buildSpecificAITeam(teamProto, priorityBuild);
}


/**
	Recruit a specific team, within the specific radius of the home position.
	*/
void AISkirmishPlayer::recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius)
{
	if (recruitRadius < 1) recruitRadius = 99999.0f;
	//
	// Create "Team in queue" based on team population
	//
	if (teamProto)
	{
		if (teamProto->getIsSingleton()) {
			Team *singletonTeam = TheTeamFactory->findTeam( teamProto->getName() );
			if (singletonTeam && singletonTeam->hasAnyObjects()) {
				AsciiString teamStr = "Unable to recruit singleton team '";
				teamStr.concat("' because team already exists.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		if (!teamProto->getTemplateInfo()->m_hasHomeLocation)
		{
			AsciiString teamStr = "Error : team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' has no Home Position (or Origin).");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
		}
		// create inactive team to place members into as they are built
		// when team is complete, the team is activated
		Team *theTeam = TheTeamFactory->createInactiveTeam( teamProto->getName() );
		AsciiString teamName = teamProto->getName();
		teamName.concat(" - Recruiting.");
		TheScriptEngine->AppendDebugMessage(teamName, false);
		const TCreateUnitsInfo *unitInfo = &teamProto->getTemplateInfo()->m_unitsInfo[0];
//		WorkOrder *orders = nullptr;
		Int i;
		Int unitsRecruited = 0;
		// Recruit.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].maxUnits;
				while (count>0) {
					Object *unit = theTeam->tryToRecruit(thing, &teamProto->getTemplateInfo()->m_homeLocation, recruitRadius);
					if (unit)
					{
						unitsRecruited++;

						AsciiString teamStr = "Team '";
						teamStr.concat(theTeam->getPrototype()->getName());
						teamStr.concat("' recruits ");
						teamStr.concat(thing->getName());
						teamStr.concat(" from team '");
						teamStr.concat(unit->getTeam()->getPrototype()->getName());
						teamStr.concat("'");
						TheScriptEngine->AppendDebugMessage(teamStr, false);

						unit->setTeam(theTeam);
						gxAirRecruitEndsAirAttack(unit);	// GeneralsX 01/11/2026 t2-fix cause N (AIPlayer.cpp)

						AIUpdateInterface *ai = unit->getAIUpdateInterface();
						if (ai)
						{
#ifdef DEBUG_LOGGING
							Coord3D pos = *unit->getPosition();
							Coord3D to = teamProto->getTemplateInfo()->m_homeLocation;
							DEBUG_LOG(("Moving unit from %f,%f to %f,%f", pos.x, pos.y , to.x, to.y ));
#endif
							ai->aiMoveToPosition( &teamProto->getTemplateInfo()->m_homeLocation, CMD_FROM_AI);
						}
					} else {
						break;
					}
					count--;
				}
			}
		}
		if (unitsRecruited>0)
		{
			/* We have something to build. */
			TeamInQueue *team = newInstance(TeamInQueue);
			// Put in front of queue.
			prependTo_TeamReadyQueue(team);
			team->m_priorityBuild = false;
			team->m_workOrders = nullptr;
			team->m_frameStarted = TheGameLogic->getFrame();
			team->m_team = theTeam;
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Finished recruiting.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}	else {
			//disband.
			if (!theTeam->getPrototype()->getIsSingleton()) {
				deleteInstance(theTeam);
				theTeam = nullptr;
			}
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Recruited 0 units, disbanding.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}
	}
}




/**
 * Train our teams.
 */
void AISkirmishPlayer::processTeamBuilding()
{
	// select a new team
	if (selectTeamToBuild()) {
		queueUnits();
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it's time to build another base building.
 */
void AISkirmishPlayer::doBaseBuilding()
{
	if (m_player->getCanBuildBase()) {
		// See if we are ready to start trying a structure.
		if (!m_readyToBuildStructure) {
			m_structureTimer--;
			if (m_structureTimer<=0) {
				m_readyToBuildStructure = true;
				m_buildDelay = 0;
			}
			if (m_structureTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_structureTimer = 3*LOGICFRAMES_PER_SECOND;
			}
			// GeneralsX @feature Claude 08/10/2026 Hoarding: the 3 s beat shrinks with the bank (aiHoardAdjustedDelay, Reforged).
			const Int structureCap = aiHoardAdjustedDelay(3*LOGICFRAMES_PER_SECOND, m_player->getMoney()->countMoney(), hoardThreshold());
			if (m_structureTimer > structureCap) {
				m_structureTimer = structureCap;
			}
		}
		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_buildDelay--;
		if (m_buildDelay<1) {
			if (m_readyToBuildStructure) {
				processBaseBuilding();
			}
			if (m_buildDelay<1) {	// processBaseBuilding may reset m_buildDelay.
				m_buildDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 2 seconds.
			}
			// Note that this timer gets shortcut when a building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any ready teams have finished moving to the rally point.
 */
void AISkirmishPlayer::checkReadyTeams()
{
	AIPlayer::checkReadyTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any queued teams have finished building, or have run out of time.
 */
void AISkirmishPlayer::checkQueuedTeams()
{
	AIPlayer::checkQueuedTeams();
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it is time to start another ai team building.
 */
void AISkirmishPlayer::doTeamBuilding()
{
	// See if any teams are expired.
	if (m_player->getCanBuildUnits()) {
		// See if we are ready to start trying a team.
		if (!m_readyToBuildTeam) {
			m_teamTimer--;
			if (m_teamTimer<=0) {
				m_readyToBuildTeam = true;
				m_teamDelay = 0;
			}
			if (m_teamTimer > 3*LOGICFRAMES_PER_SECOND) {
				m_teamTimer = 3*LOGICFRAMES_PER_SECOND;
			}
			// GeneralsX @feature Claude 08/10/2026 Hoarding: the 3 s beat shrinks with the bank (aiHoardAdjustedDelay, Reforged).
			const Int teamCap = aiHoardAdjustedDelay(3*LOGICFRAMES_PER_SECOND, m_player->getMoney()->countMoney(), hoardThreshold());
			if (m_teamTimer > teamCap) {
				m_teamTimer = teamCap;
			}
		}

		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_teamDelay--;
		if (m_teamDelay<1) {
			queueUnits(); // update the queues.
			if (m_readyToBuildTeam) {
				processTeamBuilding();
			}
			m_teamDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 5 seconds.
			// Note that this timer gets shortcut when a unit or building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Perform computer-controlled player AI
 */
void AISkirmishPlayer::update()
{
	AIPlayer::update();
	m_gxRules->update();	// GeneralsX 30/10/2026 rb-core: the AI rule bank; returns at once while no rule is enabled
	gxTunnelAirWatch();	// GeneralsX 22/10/2026 ai-wh1ter0se-econ T: a no-op unless the scb turned the watch on
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ T (gx-handoff ai/wh1ter0se-econ/DESIGN.md 7; wh1ter0se 22/10: "keeping gla troops like 4-6 in the
// tunnel mostly ... anti air units like quads to pop when it detects incoming airplanes ... and then hide back instantly").
// GeneralsX @feature Claude 27/10/2026 ai-pro-rules-2 T2 (gx-handoff ai/pro-rules/PREREG_T2.md; wh1ter0se 27/10 via lead C54: "pop and then
// instantly go back in after like instantly shooting stuff down, and they need to target them manually"). Every 6 frames:
//  * RAID PLANES: every enemy aircraft this player can see (free of fog, not stealthed-undetected, not UNATTACKABLE) within the watch
//    radius of an own tunnel - beyond GX_AIR_HOLD only while closing on that tunnel (T's rule) - nearest to its nearest tunnel first.
//  * ASSIGNMENT (acting watch only): a tunnel GUARD inside the network that can shoot a raid plane (the tunnel guard's attack test and
//    gxAirEngageable from the exit nearest the plane) is assigned to it, vehicles (Quad Cannons) before infantry, then by id, at most
//    GX_AIR_FOCUS per plane: it leaves through the tunnel nearest the plane and gets an explicit attack order on that plane.
//  * RETURN: an assignment whose plane is dead, gone or no longer engageable by that unit ends at the next pass; a unit outside is sent
//    back into the tunnels at once (aiGuardTunnelNetwork; its Return state turns for nothing while gxAirReturning holds).
//  * LIFETIME (both arms): C = a hostile player owns an airfield OR a hostile aircraft was visible in the last 180 s, every 30 frames;
//    the team is built only on the airfield term, reinforced only while C (AIPlayer::gxAirTeamAllowed), released to the default team
//    when C turns false.
//  * T0 (radius < 0): the same team, lifetime and [GXTAA] engageable lines; no assignment.
// A guard never takes an aircraft as a nemesis on its own with the acting watch on (AITNGuard gxGuardTakesNemesis), so every shot at an
// aircraft from a tunnel is one of these assignments. Rows, C and the last sighting are saved (AISkirmishPlayer::xfer AIR_GUARDS = 5).
// ------------------------------------------------------------------------------------------------
static const Real GX_AIR_HOLD = 400.0f;	///< Quad Cannon AA range 350 + 50
static const Int GX_AIR_FOCUS = 3;			///< T2: at most this many tunnel guards are assigned to one aircraft (focus fire)
static const UnsignedInt GX_AIR_SEEN_FRAMES = 180 * LOGICFRAMES_PER_SECOND;	///< T2 lifetime: a sighting keeps C for 180 s

// a tunnel guard of this player, inside the network or out of it, that can shoot this plane now (the tunnel guard's own attack test
// and the engageable rule; Quad Cannon, Tunnel Defender, Stinger site soldiers - a Scorpion cannot)
static Bool gxCanShootPlane(const Object *u, const Object *plane)
{
	const CanAttackResult r = u->getAbleToAttackSpecificObject(ATTACK_TUNNEL_NETWORK_GUARD, plane, CMD_FROM_AI);
	return (r == ATTACKRESULT_POSSIBLE || r == ATTACKRESULT_POSSIBLE_AFTER_MOVING) && gxAirEngageable(u, plane);
}

static Bool gxIsTunnelGuard(const Object *u)
{
	return u != nullptr && !u->isEffectivelyDead() && u->getAI() != nullptr && u->getAI()->getAIStateType() == AI_GUARD_TUNNEL_NETWORK;
}

struct GXAirScan { const Player *me; Int meIndex; Bool airfield; Bool seen; };
static void gxAirScanObject(Object *o, void *userData)
{
	GXAirScan *sc = (GXAirScan *)userData;
	if (o == nullptr || o->isEffectivelyDead())
		return;
	if (o->isKindOf(KINDOF_FS_AIRFIELD))
		sc->airfield = TRUE;
	if (!sc->seen && o->isKindOf(KINDOF_AIRCRAFT) && !o->isKindOf(KINDOF_UNATTACKABLE) && o->getContainedBy() == nullptr
			&& !(o->testStatus(OBJECT_STATUS_STEALTHED) && !o->testStatus(OBJECT_STATUS_DETECTED)))
	{
		// visible = PartitionFilterFreeOfFog's own test (its allow() is not public)
		if (o->getShroudedStatus(sc->meIndex) == OBJECTSHROUD_CLEAR)
			sc->seen = TRUE;
	}
}

void AISkirmishPlayer::gxAirLifetime(TeamPrototype *proto, UnsignedInt now)
{
	GXAirScan sc = { m_player, m_player->getPlayerIndex(), FALSE, FALSE };
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *o = ThePlayerList->getNthPlayer(i);
		if (o == nullptr || o == m_player || o->getDefaultTeam() == nullptr || m_player->getRelationship(o->getDefaultTeam()) != ENEMIES)
			continue;
		o->iterateObjects(gxAirScanObject, &sc);
	}
	if (sc.seen)
		m_gxAirSeenFrame = now;
	const Bool c = sc.airfield || (m_gxAirSeenFrame != 0 && now - m_gxAirSeenFrame <= GX_AIR_SEEN_FRAMES);
	const Bool was = m_gxAirC, known = m_gxAirCKnown;
	m_gxAirCAirfield = sc.airfield;
	m_gxAirC = c;
	m_gxAirCKnown = TRUE;
	if (!known || c != was)
		GX_TAA_LOG("p=%d ev=c_state value=%d airfield=%d seen_f=%d seen_s=%d", (Int)m_player->getPlayerIndex(), (Int)c, (Int)sc.airfield,
			m_gxAirSeenFrame ? (Int)(now - m_gxAirSeenFrame) : -1,
			m_gxAirSeenFrame ? (Int)((now - m_gxAirSeenFrame + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND) : -1);
	// release on a true -> false edge, AND on the first evaluation when it is false (a save from before C was saved, or a loaded team -
	// Codex t2-engine-r1 #5); a pass with nothing allocated logs members=0 queued=0
	if (c || (known && !was) || proto == nullptr)
		return;
	// C true -> false: the team goes to the AI's stock duties - a queued build or reinforcement of it is cancelled the way an expired
	// build is (TeamInQueue::disband: its units to the default team), every member of every instance joins the default team (never
	// sold); the emptied team is removed by the stock cleanup (TeamPrototype::updateState). It is built again only on the airfield term.
	// the allocation snapshot of the release (Codex t2-readers-r1 #7): alive members and queued builds of the team, BEFORE it is released
	Int moved = 0, queued = 0;
	for (DLINK_ITERATOR<Team> ti = proto->iterate_TeamInstanceList(); !ti.done(); ti.advance())
		for (DLINK_ITERATOR<Object> mi = ti.cur()->iterate_TeamMemberList(); !mi.done(); mi.advance())
			if (!mi.cur()->isEffectivelyDead())
				++moved;
	for (DLINK_ITERATOR<TeamInQueue> qi = iterate_TeamBuildQueue(); !qi.done(); qi.advance())
		if (qi.cur()->m_team && qi.cur()->m_team->getPrototype() == proto)
			++queued;
	// every fall (and a first false evaluation) prints the snapshot, released or not (Codex t2-readers-r2 #4)
	GX_TAA_LOG("p=%d ev=c_fall members=%d queued=%d", (Int)m_player->getPlayerIndex(), moved, queued);
	if (moved == 0 && queued == 0)
		return;	// nothing allocated: nothing to release
	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); )
	{
		TeamInQueue *q = iter.cur();
		if (q->m_team && q->m_team->getPrototype() == proto)
		{
			removeFrom_TeamBuildQueue(q);
			q->disband();
			deleteInstance(q);
			iter = iterate_TeamBuildQueue();
		}
		else
			iter.advance();
	}
	Team *def = m_player->getDefaultTeam();
	for (DLINK_ITERATOR<Team> ti = proto->iterate_TeamInstanceList(); !ti.done(); ti.advance())
		if (def != nullptr && ti.cur() != def)
			ti.cur()->transferUnitsTo(def);
	gxAirLogTeam(proto, "release");
	GX_TAA_LOG("p=%d ev=released members=%d queued=%d", (Int)m_player->getPlayerIndex(), moved, queued);
}

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX @test Claude 27/10/2026 ai-pro-rules-2 T2 (PREREG_T2.md activation block L): GX_TAA_TEST_NOAIR=<on_s>:<off_s> - TEST-INSTRUMENTS
// builds only, a SCENARIO for the lifetime gate, never set in a main-grid game. From game second 0 the game alternates OFF windows
// (nothing happens) and ON windows (every airfield and every aircraft of every player hostile to this air-watching AI is destroyed at
// each of its passes), OFF first: the enemy builds its airfield, the AA team is built, the ON window removes the air threat, and C turns
// false 180 s after the last sighting. Each window change is logged ([GXTAA] ev=test_noair state=on|off).
static void gxNoAirCollect(Object *o, void *userData)
{
	if (o != nullptr && !o->isEffectivelyDead() && (o->isKindOf(KINDOF_FS_AIRFIELD)
			|| (o->isKindOf(KINDOF_AIRCRAFT) && !o->isKindOf(KINDOF_UNATTACKABLE))))
		((std::vector<Object *> *)userData)->push_back(o);
}

void AISkirmishPlayer::gxAirTestNoAir(UnsignedInt now)
{
	static Int s_on = -1, s_off = -1;
	if (s_on < 0)
	{
		const char *v = getenv("GX_TAA_TEST_NOAIR");
		s_on = s_off = 0;
		if (v == nullptr || sscanf(v, "%d:%d", &s_on, &s_off) != 2 || s_on <= 0 || s_off <= 0)
			s_on = s_off = 0;
	}
	if (s_on <= 0)
		return;
	const UnsignedInt period = (UnsignedInt)(s_on + s_off) * LOGICFRAMES_PER_SECOND;
	const Bool onWin = (now % period) >= (UnsignedInt)s_off * LOGICFRAMES_PER_SECOND;
	const Bool wasOn = now >= 6 && ((now - 6) % period) >= (UnsignedInt)s_off * LOGICFRAMES_PER_SECOND;
	if (onWin != wasOn)
		GX_TAA_LOG("p=%d ev=test_noair state=%s", (Int)m_player->getPlayerIndex(), onWin ? "on" : "off");
	if (!onWin)
		return;
	std::vector<Object *> doomed;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *o = ThePlayerList->getNthPlayer(i);
		if (o == nullptr || o == m_player || o->getDefaultTeam() == nullptr || m_player->getRelationship(o->getDefaultTeam()) != ENEMIES)
			continue;
		o->iterateObjects(gxNoAirCollect, &doomed);
	}
	for (size_t k = 0; k < doomed.size(); ++k)
		doomed[k]->kill();
}
#endif

void AISkirmishPlayer::gxTunnelAirWatch()
{
	if (m_gxAirWatchRadius == 0)
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	const Int me = m_player->getPlayerIndex();
	if ((now + (UnsignedInt)me) % 6 != 0)
		return;
	GX_HOOK_TIMER_ON(gxTunnelAAHook);
	const Bool acts = m_gxAirWatchRadius > 0;
	TeamPrototype *proto = m_gxAirWatchTeam.isNotEmpty() ? TheTeamFactory->findTeamPrototype(m_gxAirWatchTeam) : nullptr;
#if defined(GX_TEST_INSTRUMENTS)
	gxAirTestNoAir(now);	// the activation block's scenario hook (GX_TAA_TEST_NOAIR); nothing without the variable
#endif
	if ((now + (UnsignedInt)me) % 30 == 0)
		gxAirLifetime(proto, now);
#if defined(GX_TEST_INSTRUMENTS)
	if (gxTaaDiagOn() && proto && (now + (UnsignedInt)me) % (10 * LOGICFRAMES_PER_SECOND) == 0)	// t2-cheap: T2's diagnostics (AIPlayer.h)
	{
		Int members = 0;
		for (DLINK_ITERATOR<Team> ti = proto->iterate_TeamInstanceList(); !ti.done(); ti.advance())
			for (DLINK_ITERATOR<Object> mi = ti.cur()->iterate_TeamMemberList(); !mi.done(); mi.advance())
				if (!mi.cur()->isEffectivelyDead())
					++members;
		Bool queued = FALSE;
		for (DLINK_ITERATOR<TeamInQueue> qi = iterate_TeamBuildQueue(); !qi.done(); qi.advance())
			if (qi.cur()->m_team && qi.cur()->m_team->getPrototype() == proto)
				queued = TRUE;
		GX_TAA_LOG("p=%d ev=team_census members=%d allocated=%d queued=%d rows=%d c=%d", me, members, (Int)(members > 0 || queued), (Int)queued,
			m_gxAirGuardCount, (Int)m_gxAirC);
	}
#endif
	TunnelTracker *tunnels = m_player->getTunnelSystem();
	if (tunnels == nullptr || tunnels->getContainerList()->empty())
	{
		// no tunnel left: nothing to come back to - the rows end (their units keep whatever order they have)
		for (Int i = 0; i < m_gxAirGuardCount; ++i)
			GX_TAA_LOG("p=%d ev=network_lost unit=%u", me, (unsigned)m_gxAirGuards[i].unit);
		m_gxAirGuardCount = 0;
		return;
	}
	// every 5 s: the AA team's members that are out and idle - a reinforcement just built, or a guard whose fight ended oddly - are sent
	// back into the tunnels (the team auto-reinforces: Codex ke-tunnel-r1 P2; its on-create order reaches only the first six)
	if (proto && (now + (UnsignedInt)me) % (5 * LOGICFRAMES_PER_SECOND) == 0)
	{
		for (DLINK_ITERATOR<Team> ti = proto->iterate_TeamInstanceList(); !ti.done(); ti.advance())
		{
			for (DLINK_ITERATOR<Object> mi = ti.cur()->iterate_TeamMemberList(); !mi.done(); mi.advance())
			{
				Object *m = mi.cur();
				if (m && !m->isEffectivelyDead() && !m->getContainedBy() && m->getAI() && m->getAI()->isIdle())
				{
					m->getAI()->aiGuardTunnelNetwork(GUARDMODE_NORMAL, CMD_FROM_AI);
					GX_TAA_LOG("p=%d ev=reguard unit=%u", me, (unsigned)m->getID());
				}
			}
		}
	}
	const Real radius = (Real)gxAirWatchScanRadius();
	const std::list<ObjectID> *all = tunnels->getContainerList();

	// --- the raid planes, nearest (to its nearest own tunnel) first, then by id
	// every eligible plane, deduplicated, THEN sorted - nothing is dropped before the sort (Codex t2-engine-r1 #4)
	std::vector<Object *> raid;
	std::vector<Real> raidD;
	for (std::list<ObjectID>::const_iterator it = all->begin(); it != all->end(); ++it)
	{
		Object *t = TheGameLogic->findObjectByID(*it);
		if (t == nullptr)
			continue;
		// GeneralsX @performance Claude 03/11/2026 t2-cheap: first in the list, so the scan skips every cell holding no enemy object (getCellOwnerMask;
		// the same objects pass in the same order - AIGuard.cpp's scan does the same since 06/10/2026)
		PartitionFilterRelationship fEnemy(t, PartitionFilterRelationship::ALLOW_ENEMIES | PartitionFilterRelationship::SKIP_CELLS_BY_OWNER);
		// UNATTACKABLE "aircraft" (the China artillery barrage's shells) never count (Codex ke-tunnel-r6 P2)
		PartitionFilterAcceptByKindOf fAir(MAKE_KINDOF_MASK(KINDOF_AIRCRAFT), MAKE_KINDOF_MASK(KINDOF_UNATTACKABLE));
		PartitionFilterAlive fAlive;
		PartitionFilterSameMapStatus fMap(t);
		PartitionFilterFreeOfFog fSeen(me);
		PartitionFilterStealthedAndUndetected fStealth(t, FALSE);
		PartitionFilter *filters[] = { &fEnemy, &fAir, &fAlive, &fMap, &fSeen, &fStealth, nullptr };
		ObjectIterator *iter = ThePartitionManager->iterateObjectsInRange(t, radius, FROM_CENTER_2D, filters, ITER_SORTED_NEAR_TO_FAR);
		MemoryPoolObjectHolder holder(iter);
		for (Object *p = iter->first(); p; p = iter->next())
		{
			const Real dp = ThePartitionManager->getDistanceSquared(t, p, FROM_CENTER_2D);
			if (dp > GX_AIR_HOLD * GX_AIR_HOLD)
			{
				// beyond the hold radius only a plane closing on this tunnel (Codex ke-tunnel-r1 P2: a plane circling at 500)
				const PhysicsBehavior *pp = p->getPhysics();
				const Coord3D *v = pp ? pp->getVelocity() : nullptr;
				if (v == nullptr || (v->x * (t->getPosition()->x - p->getPosition()->x) + v->y * (t->getPosition()->y - p->getPosition()->y)) <= 0.0f)
					continue;
			}
			size_t k = 0;
			while (k < raid.size() && raid[k] != p)
				++k;
			if (k < raid.size())
			{
				if (dp < raidD[k])
					raidD[k] = dp;
			}
			else
			{
				raid.push_back(p);
				raidD.push_back(dp);
			}
		}
	}
	const Int nRaid = (Int)raid.size();
	for (Int i = 1; i < nRaid; ++i)	// insertion sort: (distance, id)
	{
		Object *p = raid[i];
		const Real d = raidD[i];
		Int j = i - 1;
		while (j >= 0 && (raidD[j] > d || (raidD[j] == d && raid[j]->getID() > p->getID())))
		{
			raid[j + 1] = raid[j];
			raidD[j + 1] = raidD[j];
			--j;
		}
		raid[j + 1] = p;
		raidD[j + 1] = d;
	}

#if defined(GX_TEST_INSTRUMENTS)
	// --- a raid plane of the previous pass that is not one now: dead (destroyed - D.pop's "aircraft destroyed") or left (logs only)
	if (gxTaaDiagOn())	// t2-cheap: T2's diagnostics (AIPlayer.h); m_gxAirPrevRaid is this block's own log list
	{
		for (Int k = 0; k < m_gxAirPrevRaidCount; ++k)
		{
			Bool still = FALSE;
			for (Int j = 0; j < nRaid && !still; ++j)
				still = raid[j]->getID() == m_gxAirPrevRaid[k];
			if (still)
				continue;
			const Object *gone = TheGameLogic->findObjectByID(m_gxAirPrevRaid[k]);
			GX_TAA_LOG("p=%d ev=plane_gone plane=%u why=%s", me, (unsigned)m_gxAirPrevRaid[k],
				(gone == nullptr || gone->isEffectivelyDead()) ? "dead" : "left");
		}
		m_gxAirPrevRaidCount = 0;
		for (Int j = 0; j < nRaid && m_gxAirPrevRaidCount < 16; ++j)	// a log list: 16 is plenty (planes beyond it lose their gone line)
			m_gxAirPrevRaid[m_gxAirPrevRaidCount++] = raid[j]->getID();
	}
#endif
	// --- [GXTAA] ev=engageable (both arms, BEFORE any assignment of this pass, so a prompt exit still has its opportunity line):
	// every tunnel guard inside the network with no row and a raid plane it can shoot, the first such plane
#if defined(GX_TEST_INSTRUMENTS)
	if (nRaid > 0 && gxTaaDiagOn())	// t2-cheap: T2's diagnostics (AIPlayer.h)
	{
		const ContainedItemsList *in = tunnels->getContainedItemsList();
		for (ContainedItemsList::const_iterator ci = in->begin(); ci != in->end(); ++ci)
		{
			const Object *u = *ci;
			if (!gxIsTunnelGuard(u))
				continue;
			Bool hasRow = FALSE;
			for (Int r = 0; r < m_gxAirGuardCount && !hasRow; ++r)
				hasRow = m_gxAirGuards[r].unit == u->getID();
			if (hasRow)
				continue;
			for (Int k = 0; k < nRaid; ++k)
			{
				if (gxCanShootPlane(u, raid[k]))
				{
					const Object *exitT = gxTunnelNearest(m_player, raid[k]->getPosition());
					GX_TAA_LOG("p=%d ev=engageable unit=%u plane=%u tunnel=%u", me, (unsigned)u->getID(), (unsigned)raid[k]->getID(),
						(unsigned)(exitT ? exitT->getID() : INVALID_ID));
					break;
				}
			}
		}
	}
#endif
	if (!acts)
		return;

#if defined(GX_TEST_INSTRUMENTS)
	// --- [GXTAA] ev=auto_air (Codex t2-readers-r1 #1): an own tunnel guard outside (or a unit of a row) whose AI victim is an aircraft the
	// watch did not assign to it - every pass. D.aim allows none. (A turret's own fire while its unit walks is not an AI victim: out of
	// scope, PREREG_T2.md section 1.)
	if (gxTaaDiagOn())	// t2-cheap: T2's diagnostics (AIPlayer.h)
	for (Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject())
	{
		// GeneralsX @performance Claude 03/11/2026 t2-cheap: the same pure tests, the object's own fields first and its owner (team -> player)
		// last - most objects have no victim, so most never chase their team
		if (o->getAI() == nullptr || o->getContainedBy() != nullptr || o->isEffectivelyDead())
			continue;
		const Object *v = o->getAI()->getCurrentVictim();
		if (v == nullptr || !v->isKindOf(KINDOF_AIRCRAFT) || o->getControllingPlayer() != m_player)
			continue;
		Int r = 0;
		while (r < m_gxAirGuardCount && m_gxAirGuards[r].unit != o->getID())
			++r;
		const Bool ours = r < m_gxAirGuardCount && m_gxAirGuards[r].state == GX_AIR_ATTACKING && m_gxAirGuards[r].plane == v->getID();
		const Bool teamMember = proto != nullptr && o->getTeam() != nullptr && o->getTeam()->getPrototype() == proto;
		if (!ours && (r < m_gxAirGuardCount || gxIsTunnelGuard(o) || teamMember))
			GX_TAA_LOG("p=%d ev=auto_air unit=%u plane=%u row=%d", me, (unsigned)o->getID(), (unsigned)v->getID(), (Int)(r < m_gxAirGuardCount));
	}
#endif

	// --- every EXISTING row is reconciled BEFORE this pass assigns (T2 r15, Codex t2-engine-r14 #25 #26 / t2-readers-r11): dead or captured
	// units, rows back inside, planes destroyed or out of reach (target_lost + the return order), returns, re-asserts, exits - so an expiring
	// row never counts as coverage and a replacement never makes a 4th open row
	gxAirRows(0, now, me);
	const Int firstNew = m_gxAirGuardCount;

	// --- new assignments (T2 r12/r13, lead C55 28/10: "a guard must never stand idle while a plane bombs the tunnel because nobody was
	// assigned to it"), from the FREE tunnel guards of this pass: inside the network with no row, or OUTSIDE with no row and not attacking
	// (no victim, not OBJECT_STATUS_IS_ATTACKING - an attack being entered has no victim yet: Codex r12 #23). Preference: outside (no pop),
	// then vehicles, then id. LEVEL 1 is a MAXIMUM matching of the planes with no row to the free guards (augmenting paths, planes nearest
	// first - Codex r12 #22: greedy can leave a coverable plane bare); levels 2 and 3 then go round-robin. Nothing else is ever assigned.
	std::vector<Object *> cand;	// the free guards at the pass start, in preference order
	{
		std::vector<Object *> all;
		const ContainedItemsList *in = tunnels->getContainedItemsList();
		for (ContainedItemsList::const_iterator ci = in->begin(); ci != in->end(); ++ci)
			all.push_back(*ci);
		if (nRaid > 0)
			for (Object *o = TheGameLogic->getFirstObject(); o; o = o->getNextObject())
				if (o->getControllingPlayer() == m_player && o->getContainedBy() == nullptr && gxIsTunnelGuard(o)
						&& o->getAI()->getCurrentVictim() == nullptr && !o->testStatus(OBJECT_STATUS_IS_ATTACKING))
					all.push_back(o);
		for (size_t c = 0; c < all.size(); ++c)
		{
			Object *u = all[c];
			if (!gxIsTunnelGuard(u))
				continue;
			Bool hasRow = FALSE;
			for (Int r = 0; r < m_gxAirGuardCount && !hasRow; ++r)
				hasRow = m_gxAirGuards[r].unit == u->getID();
			if (hasRow)
				continue;
			// insertion by preference (outside, vehicle, id) - stable and deterministic
			size_t at = cand.size();
			while (at > 0)
			{
				const Object *b2 = cand[at - 1];
				const Bool uo = u->getContainedBy() == nullptr, bo = b2->getContainedBy() == nullptr;
				const Bool uv = u->isKindOf(KINDOF_VEHICLE), bv = b2->isKindOf(KINDOF_VEHICLE);
				const Bool before = (uo && !bo) || (uo == bo && uv && !bv) || (uo == bo && uv == bv && u->getID() < b2->getID());
				if (!before)
					break;
				--at;
			}
			cand.insert(cand.begin() + at, u);
		}
	}
	const Int nC = (Int)cand.size();
	std::vector<Bool> ableKC((size_t)nRaid * (size_t)nC, FALSE);	// can free guard c shoot raid plane k
	for (Int k = 0; k < nRaid; ++k)
		for (Int c = 0; c < nC; ++c)
			ableKC[(size_t)k * nC + c] = gxCanShootPlane(cand[c], raid[k]);
	// a row counts as coverage only if the rows loop below will keep it: its unit can still shoot the plane - the same gxCanShootPlane the
	// loop ends it by (Codex t2-engine-r13 #24: three guards out of reach must not hide a fourth free one)
	std::vector<Int> rowsOn(nRaid, 0);
	for (Int k = 0; k < nRaid; ++k)
		for (Int r = 0; r < m_gxAirGuardCount; ++r)
			if (m_gxAirGuards[r].plane == raid[k]->getID() && m_gxAirGuards[r].state != GX_AIR_RETURNING)
			{
				const Object *ru = TheGameLogic->findObjectByID(m_gxAirGuards[r].unit);
				if (ru != nullptr && !raid[k]->isEffectivelyDead() && gxCanShootPlane(ru, raid[k]))
					++rowsOn[k];
			}
	std::vector<Int> matchC(nC, -1);	// level 1: the plane each free guard covers
	{
		std::vector<Bool> seen(nC, FALSE);
		struct GXAug
		{
			static Bool run(Int k, Int nC, const std::vector<Bool> &able, std::vector<Int> &matchC, std::vector<Bool> &seen)
			{
				for (Int c = 0; c < nC; ++c)
				{
					if (!able[(size_t)k * nC + c] || seen[c])
						continue;
					seen[c] = TRUE;
					if (matchC[c] < 0 || run(matchC[c], nC, able, matchC, seen))
					{
						matchC[c] = k;
						return TRUE;
					}
				}
				return FALSE;
			}
		};
		for (Int k = 0; k < nRaid; ++k)
		{
			if (rowsOn[k] > 0)
				continue;	// already covered by an earlier row
			for (Int c = 0; c < nC; ++c)
				seen[c] = FALSE;
			GXAug::run(k, nC, ableKC, matchC, seen);
		}
	}
	std::vector<Bool> used(nC, FALSE);
	for (Int k = 0; k < nRaid && m_gxAirGuardCount < GX_AIR_GUARDS_MAX; ++k)	// level 1 rows, plane order
		for (Int c = 0; c < nC; ++c)
			if (matchC[c] == k && m_gxAirGuardCount < GX_AIR_GUARDS_MAX)
			{
				used[c] = TRUE;
				GXAirGuard &row = m_gxAirGuards[m_gxAirGuardCount++];
				row.unit = cand[c]->getID();
				row.plane = raid[k]->getID();
				row.state = GX_AIR_ASSIGNED;
				++rowsOn[k];
				GX_TAA_LOG("p=%d ev=assign unit=%u utmpl=%s plane=%u ptmpl=%s n=%d from=%s lvl=1", me, (unsigned)cand[c]->getID(),
					cand[c]->getTemplate()->getName().str(), (unsigned)raid[k]->getID(), raid[k]->getTemplate()->getName().str(), rowsOn[k],
					cand[c]->getContainedBy() == nullptr ? "out" : "in");
			}
	for (Int level = 2; level <= GX_AIR_FOCUS; ++level)	// levels 2, 3: round-robin over the planes, candidates in preference order
		for (Int k = 0; k < nRaid && m_gxAirGuardCount < GX_AIR_GUARDS_MAX; ++k)
		{
			if (rowsOn[k] >= level || rowsOn[k] == 0)
				continue;	// (a plane level 1 could not cover gets no 2nd before a 1st)
			for (Int c = 0; c < nC; ++c)
			{
				if (used[c] || !ableKC[(size_t)k * nC + c])
					continue;
				used[c] = TRUE;
				GXAirGuard &row = m_gxAirGuards[m_gxAirGuardCount++];
				row.unit = cand[c]->getID();
				row.plane = raid[k]->getID();
				row.state = GX_AIR_ASSIGNED;
				++rowsOn[k];
				GX_TAA_LOG("p=%d ev=assign unit=%u utmpl=%s plane=%u ptmpl=%s n=%d from=%s lvl=%d", me, (unsigned)cand[c]->getID(),
					cand[c]->getTemplate()->getName().str(), (unsigned)raid[k]->getID(), raid[k]->getTemplate()->getName().str(), rowsOn[k],
					cand[c]->getContainedBy() == nullptr ? "out" : "in", level);
				break;
			}
		}
#if defined(GX_TEST_INSTRUMENTS)
	// --- [GXTAA] ev=cover (REPORTED, lead C55's coverage check): per raid plane after this pass's assignment - its rows, the free guards
	// that could shoot it at the pass start (able, and their ids so the reader can recompute the maximum matching), and how many of those
	// are still free (free_able)
	if (gxTaaDiagOn())	// t2-cheap: T2's diagnostics (AIPlayer.h)
	for (Int k = 0; k < nRaid; ++k)
	{
		Int able = 0, freeAble = 0;
		AsciiString ids;
		for (Int c = 0; c < nC; ++c)
			if (ableKC[(size_t)k * nC + c])
			{
				++able;
				freeAble += !used[c];
				AsciiString one;
				one.format("%s%u", ids.isEmpty() ? "" : ",", (unsigned)cand[c]->getID());
				ids.concat(one);
			}
		GX_TAA_LOG("p=%d ev=cover plane=%u rows=%d able=%d free_able=%d guards=%s", me, (unsigned)raid[k]->getID(), rowsOn[k], able, freeAble,
			ids.isEmpty() ? "-" : ids.str());
	}
#endif

	// --- the rows assigned in this pass: out and attack at once (T2 r15: the existing rows were reconciled before the assignment)
	gxAirRows(firstNew, now, me);
}

//----------------------------------------------------------------------------------------------------------
// GeneralsX 01/11/2026 t2-fix cause A (gx-handoff ai/t2-fix/DIAG_t2-auto-air-217.md: 363 of the 408 auto_air episodes of the T2 grid): a
// return order the unit REFUSED - AIUpdateInterface::privateGuardTunnelNetwork does nothing while the unit is not mobile (an EMP, subdual or
// paralysis disable) - left its attack on the plane standing, AI victim included, with its AI update suspended, so the watch saw an
// aircraft victim (ev=auto_air) until the disable lapsed. Such a unit is idled: privateIdle clears the machine whatever the unit's
// mobility, and the attack state's exit clears the victim and the turret targets. The RETURNING row orders it back once a second, as
// before. A unit whose return order took holds no aircraft victim after it, and nothing changes for it.
static void gxAirEndRefusedAttack(Object *u, Int me)
{
	const Object *v = u->getAI()->getCurrentVictim();
	if (v == nullptr || !v->isKindOf(KINDOF_AIRCRAFT))
		return;
	GX_TAA_LOG("p=%d ev=air_drop unit=%u plane=%u mob=%d why=return", me, (unsigned)u->getID(), (unsigned)v->getID(), (Int)u->isMobile());
	u->getAI()->aiIdle(CMD_FROM_AI);
}

//----------------------------------------------------------------------------------------------------------
// GeneralsX 28/10/2026 ai-pro-rules-2 T2 r15: the watch's row loop (out and attack, or back in), from row <from> on - called for every
// existing row before the pass assigns, and for the rows assigned in the pass after it
void AISkirmishPlayer::gxAirRows(Int from, UnsignedInt now, Int me)
{
	for (Int r = from; r < m_gxAirGuardCount; )
	{
		GXAirGuard &row = m_gxAirGuards[r];
		Object *u = TheGameLogic->findObjectByID(row.unit);
		Bool drop = FALSE;
		if (u == nullptr || u->isEffectivelyDead() || u->getAI() == nullptr)
		{
			GX_TAA_LOG("p=%d ev=row_end unit=%u why=unit_dead state=%d", me, (unsigned)row.unit, row.state);
			drop = TRUE;
		}
		else if (u->getControllingPlayer() != m_player)
		{
			GX_TAA_LOG("p=%d ev=row_end unit=%u why=captured state=%d", me, (unsigned)row.unit, row.state);	// Codex t2-engine-r2 #11
			drop = TRUE;
		}
		else if (row.state == GX_AIR_RETURNING)
		{
			drop = u->getContainedBy() != nullptr;	// back in (its [GXTAA] ev=in is TunnelContain's line)
			// still out and no longer in the tunnel-guard machine (an entry failed and something else took it): ordered back again, once a
			// second (Codex t2-engine-r2 #10)
			if (!drop && u->getAI()->getAIStateType() != AI_GUARD_TUNNEL_NETWORK && (now + (UnsignedInt)me) % LOGICFRAMES_PER_SECOND < 6)
			{
				u->getAI()->aiGuardTunnelNetwork(GUARDMODE_NORMAL, CMD_FROM_AI);
				GX_TAA_LOG("p=%d ev=return_order unit=%u again=1", me, (unsigned)row.unit);
				gxAirEndRefusedAttack(u, me);	// GeneralsX 01/11/2026 t2-fix cause A
			}
		}
		else if (row.state == GX_AIR_ATTACKING && u->getContainedBy() != nullptr)
		{
			GX_TAA_LOG("p=%d ev=row_end unit=%u why=inside state=%d", me, (unsigned)row.unit, row.state);	// something else put it back in
			drop = TRUE;
		}
		else
		{
			Object *p = TheGameLogic->findObjectByID(row.plane);
			const char *why = nullptr;
			if (p == nullptr || p->isEffectivelyDead())
				why = "dead";
			else if (!gxCanShootPlane(u, p))
				why = "range";
			// the row's own predicate, this pass, before it is acted on (Codex t2-readers-r2 #2 #7): eng=0 must end the row in this pass
			// held = its MAIN state is the attack on ITS plane (Codex t2-readers-r15 #1: an engageable outside row with held=0 must be ordered
			// at it again in this pass - the reader checks that continuity)
			GX_TAA_LOG("p=%d ev=row_tick unit=%u plane=%u eng=%d out=%d held=%d", me, (unsigned)row.unit, (unsigned)row.plane, (Int)(why == nullptr),
				(Int)(u->getContainedBy() == nullptr),
				(Int)(p != nullptr && u->getAI()->getAIStateType() == AI_ATTACK_OBJECT && u->getAI()->getGoalObject() == p));
			if (why)
			{
				GX_TAA_LOG("p=%d ev=target_lost unit=%u plane=%u why=%s out=%d", me, (unsigned)row.unit, (unsigned)row.plane, why,
					(Int)(u->getContainedBy() == nullptr));
				if (u->getContainedBy() != nullptr)
					drop = TRUE;	// never left (its exit was busy): nothing to come back from
				else
				{
					row.state = GX_AIR_RETURNING;
					row.plane = INVALID_ID;
					u->getAI()->aiGuardTunnelNetwork(GUARDMODE_NORMAL, CMD_FROM_AI);
					GX_TAA_LOG("p=%d ev=return_order unit=%u", me, (unsigned)row.unit);
					gxAirEndRefusedAttack(u, me);	// GeneralsX 01/11/2026 t2-fix cause A
				}
			}
			else if (row.state == GX_AIR_ASSIGNED)
			{
				if (u->getContainedBy() != nullptr)
				{
					// out through the tunnel nearest the plane, the way a guard deploys (AITNGuardIdleState); a busy exit retries next pass
					Object *exitT = gxTunnelNearest(m_player, p->getPosition());
					ContainModuleInterface *c = exitT ? exitT->getContain() : nullptr;
					ExitInterface *x = (c != nullptr && c->isContained(u)) ? c->getContainExitInterface() : nullptr;
					if (x != nullptr && !x->isExitBusy())
						x->exitObjectInAHurry(u);
				}
				if (u->getContainedBy() == nullptr)
				{
					u->getAI()->aiAttackObject(p, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);	// the explicit order on ITS plane
					row.state = GX_AIR_ATTACKING;
					GX_TAA_LOG("p=%d ev=attack unit=%u plane=%u ptmpl=%s", me, (unsigned)row.unit, (unsigned)row.plane,
						p->getTemplate()->getName().str());
				}
			}
			else if (row.state == GX_AIR_ATTACKING && u->getContainedBy() == nullptr
					&& (u->getAI()->getAIStateType() != AI_ATTACK_OBJECT || u->getAI()->getGoalObject() != p))
			{
				// GeneralsX 28/10/2026 T2 (diag T2d1; lead C55 "re-assert the per-plane order"): its MAIN state is no longer the attack on
				// ITS plane (an AI move the mood matrix turned into an attack-move, or a stock attack that gave up on an airborne target -
				// a computer player does not chase one) - given again. (The tunnel exit is a temporary state: the main state is kept.)
				u->getAI()->aiAttackObject(p, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
				GX_TAA_LOG("p=%d ev=attack unit=%u plane=%u ptmpl=%s again=1", me, (unsigned)row.unit, (unsigned)row.plane,
					p->getTemplate()->getName().str());
			}
		}
		if (drop)
		{
			for (Int j = r + 1; j < m_gxAirGuardCount; ++j)	// keep the order (deterministic)
				m_gxAirGuards[j - 1] = m_gxAirGuards[j];
			--m_gxAirGuardCount;
		}
		else
			++r;
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Adjusts the build list to match the starting position.
 */
void AISkirmishPlayer::adjustBuildList(BuildListInfo *list)
{
	Bool foundStart = false;
	Coord3D startPos;

	// Find our command center location.
	Object *obj;
	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{

		Player *owner = obj->getControllingPlayer();
		if (owner==m_player) {
			// See if it's a command center.
			if (obj->isKindOf(KINDOF_COMMANDCENTER)) {
				foundStart = true;
				startPos = *obj->getPosition();
				m_player->onStructureUndone(obj);
				TheAI->pathfinder()->removeObjectFromPathfindMap(obj);
				TheGameLogic->destroyObject(obj);
				break;
			}
		}
	}
	if (!foundStart) {
		DEBUG_LOG(("Couldn't find starting command center for ai player."));
		return;
	}
	// Find the location of the command center in the build list.
	Bool foundInBuildList = false;
	Coord3D buildPos;
	BuildListInfo *cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(cur->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			buildPos = *cur->getLocation();
			cur->setInitiallyBuilt(true);
		}
		cur = cur->getNext();
	}
	Region3D bounds;
	TheTerrainLogic->getMaximumPathfindExtent(&bounds);
	/* calculate section of 3x3 grid:
		6 7 8
		3 4 5
		0 1 2 */

	Int gridIndex = 0;
	if (startPos.x > bounds.lo.x + bounds.width()/3) {
		gridIndex++;
	}
	if (startPos.x > bounds.lo.x + 2*bounds.width()/3) {
		gridIndex++;
	}

	if (startPos.y > bounds.lo.y + bounds.height()/3) {
		gridIndex+=3;
	}
	if (startPos.y > bounds.lo.y + 2*bounds.height()/3) {
		gridIndex+=3;
	}

	Real angle = 0;
	if (TheAI->getAiData()->m_rotateSkirmishBases) {
		switch (gridIndex) {
			case 0 : angle = 0; break;
			case 1 : angle = PI/4; break;// 45 degrees.
			case 2 : angle = PI/2; break; // 90 degrees;
			case 3 : angle = -PI/4; break; // -45 degrees.
			case 4 : angle = 0; break;
			case 5 : angle = 3*PI/4; break; // 135 degrees.
			case 6 : angle = -PI/2; break; // -90 degrees;
			case 7 : angle = -3*PI/4; break; // -135 degrees.
			case 8 : angle = PI; break; // 180 degrees.
		}
	}

	angle += 3*PI/4;
	const Real gxRetailAngle = angle;	// GeneralsX 06/11/2026 base-layout (ADDENDUM_8): where the stock list's spots go, for a keyed list's fallbacks

	// GeneralsX @feature Claude 04/11/2026 base-layout: GXTurnLayout places the list rigidly, the template's +X at the base front: the turn is the
	// octant of the front direction seen from the start, and it reaches the angles too (the retail line below keeps each entry's angle, so a
	// spot moves and its door does not). Every other list keeps the retail 3*PI/4.
	const AISideBuildList *gxList = gxSideList();
	const Bool gxTurn = gxList != nullptr && gxList->m_gxTurnLayout;
	Coord3D gxFront;
	gxFront.zero();
	Bool gxFrontIsWay = FALSE;
	if (gxTurn) {
		Coord3D gxAnchor = startPos;
		gxStartPoint(&gxAnchor);	// the point gxFrontPoint reads later
		gxFrontIsWay = gxBaseFrontFrom(gxAnchor, &gxFront);
		angle = gxOctantAngle(gxFront.x - gxAnchor.x, gxFront.y - gxAnchor.y);
	}

	Real s = Sin(angle);
	Real c = Cos(angle);
	const Real gxRs = Sin(gxRetailAngle);
	const Real gxRc = Cos(gxRetailAngle);

	cur = list;
	while (cur) {
		const ThingTemplate *tTemplate = TheThingFactory->findTemplate(list->getTemplateName());
		if (tTemplate && tTemplate->isKindOf(KINDOF_COMMANDCENTER)) {
			foundInBuildList = true;
			Coord3D curPos = *cur->getLocation();
			// Transform to new coords.
			curPos.x -= buildPos.x;
			curPos.y -= buildPos.y;
			Real newX = curPos.x*c - curPos.y*s;
			Real newY = curPos.y*c + curPos.x*s;
			curPos.x = newX + startPos.x;
			curPos.y = newY + startPos.y;
			cur->setLocation(curPos);
			cur->setAngle(gxTurn ? normalizeAngle(cur->getAngle() + angle) : cur->getAngle());
			// GeneralsX @feature Claude 06/11/2026 base-layout (gx-handoff ai/base-layout/ADDENDUM_8.md, wh1ter0se's option 2): a keyed entry's fallback is the
			// stock list's spot for it, placed as retail places that list: the retail turn about the command centre, its angle as written.
			if (gxTurn && cur->hasLayoutFallback()) {
				Coord3D fb = *cur->getFallbackLocation();
				fb.x -= buildPos.x;
				fb.y -= buildPos.y;
				const Real fbX = fb.x*gxRc - fb.y*gxRs;
				const Real fbY = fb.y*gxRc + fb.x*gxRs;
				fb.x = fbX + startPos.x;
				fb.y = fbY + startPos.y;
				cur->gxSetLayoutFallback(fb, cur->getFallbackAngle());
			}
		}
		// GeneralsX 06/11/2026 base-layout: a fallback is read on a GXTurnLayout list only; any other list (a stock one cannot carry it) loses it here.
		if (!gxTurn) {
			cur->gxDropLayoutFallback();
		}
		cur = cur->getNext();
	}
#if defined(GX_TEST_INSTRUMENTS)
	// GeneralsX 04/11/2026 base-layout: the placed list, for the rigid-transform oracle (PLAN S0.3)
	for (cur = list; cur; cur = cur->getNext()) {
		GX_ROT_LOG("p=%d ev=layout side=%s keyed=%d doors=%d tmpl=%s x=%.2f y=%.2f ang=%.3f sx=%.2f sy=%.2f bx=%.2f by=%.2f turn=%.3f fx=%.2f fy=%.2f way=%d",
			(Int)m_player->getPlayerIndex(), m_player->getSide().str(), (Int)gxTurn, (Int)(gxList != nullptr && gxList->m_gxFrontDoors), cur->getTemplateName().str(),
			cur->getLocation()->x, cur->getLocation()->y, cur->getAngle(), startPos.x, startPos.y, buildPos.x, buildPos.y, angle, gxFront.x, gxFront.y, (Int)gxFrontIsWay);
		if (cur->hasLayoutFallback())	// GeneralsX 06/11/2026 base-layout (ADDENDUM_8): the placed fallback, for the same oracle against the stock arm's spots
			GX_ROT_LOG("p=%d ev=layoutfb tmpl=%s x=%.2f y=%.2f ang=%.3f turn=%.3f", (Int)m_player->getPlayerIndex(), cur->getTemplateName().str(),
				cur->getFallbackLocation()->x, cur->getFallbackLocation()->y, cur->getFallbackAngle(), gxRetailAngle);
	}
	gxDoorRuleFixture();
	gxLayoutFallbackFixture();
#endif

}



//----------------------------------------------------------------------------------------------------------
/**
 * Find any things that build stuff & add them to the build list.  Then build any initially built
 * buildings.
 */
void AISkirmishPlayer::newMap()
{

	/* Get our proper build list. */
	AsciiString mySide = m_player->getSide();
	DEBUG_LOG(("AI Player side is %s", mySide.str()));
	const AISideBuildList *build = TheAI->getAiData()->m_sideBuildLists;
	while (build) {
		if (build->m_side == mySide) {
			BuildListInfo *buildList = build->m_buildList->duplicate();
			adjustBuildList(buildList); // adjust to  our start position.
			m_player->setBuildList(buildList);
			computeCenterAndRadiusOfBase(&m_baseCenter, &m_baseRadius);
			gxComputeStockCenter();
			// GeneralsX 06/11/2026 base-layout (ADDENDUM_9): every skirmish AI's centres, for ORACLE-SC (a keyed seat's stock centre = its stock partner's base)
			GX_ROT_LOG("p=%d ev=centers base=%.2f,%.2f stock=%d,%.2f,%.2f", (Int)m_player->getPlayerIndex(), m_baseCenter.x, m_baseCenter.y,
				(Int)m_gxStockCenterSet, m_gxStockCenter.x, m_gxStockCenter.y);
			break;
		}
		build = build->m_next;
	}
	DEBUG_ASSERTLOG(build!=nullptr, ("Couldn't find build list for skirmish player."));

	// Build any with the initially built flag.
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
			continue;
		}
		if (info->isInitiallyBuilt()) {
			buildStructureNow(bldgPlan, info);
		} else {
			info->incrementNumRebuilds(); // the initial build in the normal build list consumes a rebuild, so add one.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * Queues up a dozer.
 */
void AISkirmishPlayer::queueDozer()
{
	AIPlayer::queueDozer();
}

//----------------------------------------------------------------------------------------------------------
/**
 * Finds a dozer that isn't building or collecting resources.
 */
Object * AISkirmishPlayer::findDozer( const Coord3D *pos )
{
	return AIPlayer::findDozer(pos);
}


//----------------------------------------------------------------------------------------------------------
/**
 * Find a good spot to fire a superweapon.
 */
Bool AISkirmishPlayer::computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *retPos, Int playerNdx, Real weaponRadius)
{

	Region2D bounds;
	getPlayerStructureBounds(&bounds, playerNdx);

	if( power->getSpecialPowerType() == SPECIAL_CLUSTER_MINES || power->getSpecialPowerType() == NUKE_SPECIAL_CLUSTER_MINES )
	{
		// hackus brutus - mine the entrances to our base.
		AsciiString pathLabel;
		Int mode = GameLogicRandomValue(0, 2);
		if (mode==1) {
				pathLabel.format("%s%d", SKIRMISH_FLANK, m_player->getMpStartIndex()+1);
		}	else if (mode==2) {
				pathLabel.format("%s%d", SKIRMISH_BACKDOOR, m_player->getMpStartIndex()+1);
		}	else {
			pathLabel.format("%s%d", SKIRMISH_CENTER, m_player->getMpStartIndex()+1);
		}

		Coord3D goalPos = m_baseCenter;
		Waypoint *way = TheTerrainLogic->getClosestWaypointOnPath( &goalPos, pathLabel );
		if (way) {
			goalPos = *way->getLocation();
		} else {
			Region2D bounds;
			getPlayerStructureBounds(&bounds, getMyEnemyPlayerIndex());
			goalPos.x = bounds.lo.x + bounds.width()/2;
			goalPos.y = bounds.lo.y + bounds.height()/2;
		}
		Coord2D offset;
		offset.x = goalPos.x-m_baseCenter.x;
		offset.y = goalPos.y-m_baseCenter.y;
		offset.normalize();
		offset.x *= m_baseRadius;
		offset.y *= m_baseRadius;
		*retPos = m_baseCenter;
		retPos->x += offset.x;
		retPos->y += offset.y;
		retPos->z = TheTerrainLogic->getGroundHeight(retPos->x, retPos->y);
		return TRUE;
	}

	return AIPlayer::computeSuperweaponTarget(power, retPos, playerNdx, weaponRadius);

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info;
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
static const XferVersion GX_SKIRMISH_XFER_FACE_FRONT = 2;	///< GeneralsX 22/10/2026 ai-wh1ter0se-econ R: m_gxFaceFront
static const XferVersion GX_SKIRMISH_XFER_RULEBANK = 3;		///< GeneralsX 30/10/2026 rb-core: the AI rule bank's block (3 = integration's next free number after FACE_FRONT, checked at the core-land merge 01/11/2026; an AI branch landing after it takes 4)
static const XferVersion GX_SKIRMISH_XFER_AIR_WATCH = 4;	///< GeneralsX 01/11/2026 t2-fix-2: ai-wh1ter0se-econ T, the tunnel air watch's radius + team (3 on feat/ai-ke-tunnel-aa3, renumbered after the bank)
static const XferVersion GX_SKIRMISH_XFER_AIR_GUARDS = 5;	///< GeneralsX 01/11/2026 t2-fix-2: ai-pro-rules-2 T2, the air watch's guard rows, C and the last sighting (4 on feat/ai-ke-tunnel-aa3)
static const XferVersion GX_SKIRMISH_XFER_STOCKCENTER = 6;	///< GeneralsX 06/11/2026 base-layout ADDENDUM_9: m_gxStockCenter / Set (4 on feat/ai-base-layout; renumbered after AIR_GUARDS at the 0.9.70 rebase, 08/11/2026)
void AISkirmishPlayer::xfer( Xfer *xfer )
{

	// version
	// GeneralsX 22/10/2026: 2 = AI_PLAYER_FACE_FRONT (ai-wh1ter0se-econ R); a retail-compatible save stays version 1, which retail's reader
	// accepts (Codex ke-tunnel-r1 P2, the rule of the neighbouring serializers)
	// GeneralsX 30/10/2026: GX_SKIRMISH_XFER_RULEBANK = the rule bank's block (rb-core). The versions are named so that a renumber when
	// several AI branches merge is one line here (lead C57); the loader accepts every earlier version. Once the bank exists a new rule
	// never bumps this version again: it versions its OWN state inside the bank block (GXRuleCore.h bankXfer).
	// GeneralsX 01/11/2026 t2-fix-2: GX_SKIRMISH_XFER_AIR_WATCH (ai-wh1ter0se-econ T: AI_PLAYER_TUNNEL_AIR_WATCH's radius + team) and
	// GX_SKIRMISH_XFER_AIR_GUARDS (ai-pro-rules-2 T2: the air watch's guard rows, C and the last sighting) come after the bank. On the
	// unmerged T2 branches they were 3 and 4: a save written there must not be loaded here - its version-3 bytes are read as the bank
	// block, which refuses them on its format byte unless the watch radius's low byte happens to be 1 (then it misreads them).
	// GeneralsX 08/11/2026 base-layout: GX_SKIRMISH_XFER_STOCKCENTER (ADDENDUM_9's stock base centre) comes after the air guards. On the
	// unmerged feat/ai-base-layout it was 4: a save written there must not be loaded here - its stock-centre bytes are read as the air watch.
#if RETAIL_COMPATIBLE_XFER_SAVE
	XferVersion currentVersion = 1;
#else
	XferVersion currentVersion = GX_SKIRMISH_XFER_STOCKCENTER;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// xfer base class info
	AIPlayer::xfer( xfer );

	// front base defense
	xfer->xferInt( &m_curFrontBaseDefense );

	// flank base defense
	xfer->xferInt( &m_curFlankBaseDefense );

	// front left defense angle
	xfer->xferReal( &m_curFrontLeftDefenseAngle );

	// front right defense angle
	xfer->xferReal( &m_curFrontRightDefenseAngle );

	// left flank left defense angle
	xfer->xferReal( &m_curLeftFlankLeftDefenseAngle );

	// left flank right defense angle
	xfer->xferReal( &m_curLeftFlankRightDefenseAngle );

	// right flank left defense angle
	xfer->xferReal( &m_curRightFlankLeftDefenseAngle );

	// right flank right defense angle
	xfer->xferReal( &m_curRightFlankRightDefenseAngle );

	// GeneralsX 22/10/2026 (ai-wh1ter0se-econ R): the AI_PLAYER_FACE_FRONT flag (AIPlayer::m_gxFaceFront); off in a version-1 save
	if( version >= GX_SKIRMISH_XFER_FACE_FRONT )
		xfer->xferBool( &m_gxFaceFront );
	else if( xfer->getXferMode() == XFER_LOAD )
		m_gxFaceFront = FALSE;	// an older save starts with it off; on LOAD only - a retail-compatible save WRITES v1 too (Codex ke-rotate-r2 P2)

	// GeneralsX 30/10/2026 rb-core: the rule bank (configuration, unit claims, money reserves, each rule's own state block). An older save
	// has none: nothing configured. A retail-compatible build writes version 1 and refuses every enable, so it never holds rule state.
	if( version >= GX_SKIRMISH_XFER_RULEBANK )
		m_gxRules->xfer( xfer );
	else if( xfer->getXferMode() == XFER_LOAD )
		m_gxRules->reset();

	// GeneralsX 22/10/2026 (ai-wh1ter0se-econ T): the air watch radius (AIPlayer::m_gxAirWatchRadius); off in a save older than AIR_WATCH
	if( version >= GX_SKIRMISH_XFER_AIR_WATCH )
	{
		xfer->xferInt( &m_gxAirWatchRadius );
		xfer->xferAsciiString( &m_gxAirWatchTeam );
	}
	else if( xfer->getXferMode() == XFER_LOAD )
	{
		// an older save: the watch starts off. Only on LOAD - a retail-compatible save WRITES version 1 too, and must leave the live AI's
		// watch on (Codex ke-tunnel-r2 P2)
		m_gxAirWatchRadius = 0;
		m_gxAirWatchTeam.clear();
	}

	// GeneralsX 27/10/2026 (ai-pro-rules-2 T2): the AA guards out at (or coming back from) an aircraft, C and the last sighting - all
	// read by the simulation (return orders, the team's build / reinforce / release). Older saves: none, C not yet evaluated.
	if( version >= GX_SKIRMISH_XFER_AIR_GUARDS )
	{
		xfer->xferInt( &m_gxAirGuardCount );
		if( m_gxAirGuardCount < 0 || m_gxAirGuardCount > GX_AIR_GUARDS_MAX )
		{
			DEBUG_CRASH(( "AISkirmishPlayer::xfer - air guard rows %d out of range", m_gxAirGuardCount ));
			throw SC_INVALID_DATA;
		}
		for( Int i = 0; i < m_gxAirGuardCount; ++i )
		{
			xfer->xferObjectID( &m_gxAirGuards[i].unit );
			xfer->xferObjectID( &m_gxAirGuards[i].plane );
			xfer->xferInt( &m_gxAirGuards[i].state );
		}
		xfer->xferBool( &m_gxAirC );
		xfer->xferBool( &m_gxAirCAirfield );
		xfer->xferBool( &m_gxAirCKnown );
		xfer->xferUnsignedInt( &m_gxAirSeenFrame );
	}
	else if( xfer->getXferMode() == XFER_LOAD )
	{
		m_gxAirGuardCount = 0;
		m_gxAirC = FALSE;
		m_gxAirCAirfield = FALSE;
		m_gxAirCKnown = FALSE;
		m_gxAirSeenFrame = 0;
	}

	// GeneralsX 06/11/2026 base-layout ADDENDUM_9: a keyed list's stock centre. An older save has none: it ranks from m_baseCenter, as it did.
	if( version >= GX_SKIRMISH_XFER_STOCKCENTER )
	{
		xfer->xferCoord3D( &m_gxStockCenter );
		xfer->xferBool( &m_gxStockCenterSet );
	}
	else if( xfer->getXferMode() == XFER_LOAD )
	{
		m_gxStockCenter.zero();
		m_gxStockCenterSet = FALSE;
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AISkirmishPlayer::loadPostProcess()
{
	// GeneralsX @bugfix Claude 01/10/2026 Chain to the base: it drops build-queue entries whose team no longer
	// exists (tsh batch2 #417).
	AIPlayer::loadPostProcess();
}

