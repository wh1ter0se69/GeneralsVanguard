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

// FILE: AITNGuard.cpp
/*---------------------------------------------------------------------------*/
/* EA Pacific                                                                */
/* Confidential Information	                                                 */
/* Copyright (C) 2001 - All Rights Reserved                                  */
/* DO NOT DISTRIBUTE                                                         */
/*---------------------------------------------------------------------------*/
/* Project:    RTS3                                                          */
/* File name:  AITNGuard.cpp                                                   */
/* Created:    John Ahlquist., 12/20/2002                              */
/* Desc:       // Set up guard tunnel network states for AI                                 */
/* Revision History:                                                         */
/*		12/20/2002 : Initial creation - modified from AIGuard.cpp                                           */
/*---------------------------------------------------------------------------*/
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/PerfTimer.h"
#include "Common/Team.h"
#include "Common/Player.h"
#include "Common/Xfer.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/AITNGuard.h"
#include "GameLogic/AIPlayer.h"	// GeneralsX 22/10/2026 ai-wh1ter0se-econ T: gxAirWatchRadius (Codex ke-tunnel-r1 P1)
#if defined(GX_TEST_INSTRUMENTS)	// GeneralsX 23/10/2026 ai-wh1ter0se-econ T: a guard's pop / turn / hide, for the box readers (tools/box/mech.py)
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
static Bool gxAirWatchOn(const Player *p)
{
	return p && p->getAIPlayer() && p->getAIPlayer()->gxAirWatchRadius() > 0;
}
// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (wh1ter0se 27/10: "they need to target them manually"): with the owner's ACTING watch on, an aircraft
// is attacked from the tunnels only by the watch's assignments - every other way a guard picks a target (team target, the tunnel's
// attacker, the network nemesis, its own scan, retaliation) skips aircraft
static Bool gxAirByAssignmentOnly(const Object *guard, const Object *target)
{
	return target != nullptr && target->isKindOf(KINDOF_AIRCRAFT) && guard != nullptr && gxAirWatchOn(guard->getControllingPlayer());
}
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/CollideModule.h"
#include "GameLogic/Module/PhysicsUpdate.h"	// GeneralsX 23/10/2026 ai-wh1ter0se-econ T: PhysicsBehavior::getVelocity (Codex ke-tunnel-r10 P1)
#include "GameLogic/GXHookTimer.h"	// GeneralsX 22/10/2026 ai-wh1ter0se-econ: [GXHOOK] CPU timer (test-instruments builds)
GX_HOOK_STATS_EXTERN(gxTunnelAAHook);	// AISkirmishPlayer.cpp: arm T's one per-tick sum (the watch + these guard-side checks)
#include "Common/TunnelTracker.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/PolygonTrigger.h"

const Real CLOSE_ENOUGH = (25.0f);


static Bool gxAirReturningGuard(const Object *owner);	// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (below)

static Bool hasAttackedMeAndICanReturnFire( State *thisState, void* /*userData*/ )
{
	Object *obj = thisState->getMachineOwner();
	BodyModuleInterface *bmi = obj ? obj->getBodyModule() : nullptr;

	if (!(obj && bmi)) {
		return FALSE;
	}
	// GeneralsX 27/10/2026 ai-pro-rules-2 T2: an AA guard the air watch is bringing back in does not stop to retaliate (wh1ter0se 27/10)
	if (gxAirReturningGuard(obj)) {
		return FALSE;
	}

	if (bmi->getClearableLastAttacker() == INVALID_ID) {
		return FALSE;
	}

	// K. It appears we have a valid aggressor. Find it, and determine if we can attack it, etc.
	Object *target = TheGameLogic->findObjectByID(bmi->getClearableLastAttacker());
	bmi->clearLastAttacker();
	if (gxAirByAssignmentOnly(obj, target)) {
		return FALSE;	// GeneralsX 27/10/2026 ai-pro-rules-2 T2: no retaliation at an aircraft
	}

	// We use the clearable last attacker because we should continue attacking the guy. But if he
	// stops attacking us, then we want our timer to kick us off of him and make us go attack
	// other units instead.


	if (!target) {
		return FALSE;
	}

	if (obj->getRelationship(target) != ENEMIES) {
		return FALSE;
	}

	// This is a quick test on the target. It will be duplicated in getAbleToAttackSpecificObject,
	// but the payoff is worth the duplication.
	if (target->isEffectivelyDead()) {
		return FALSE;
	}

	CanAttackResult result = obj->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, target, CMD_FROM_AI);
	if( result == ATTACKRESULT_POSSIBLE || result == ATTACKRESULT_POSSIBLE_AFTER_MOVING )
	{
		return TRUE;
	}
	return FALSE;
}

static Object *findBestTunnel(Player *ownerPlayer, const Coord3D *pos)
{
	if (!ownerPlayer) return nullptr; // should never happen, but hey.  jba.
	TunnelTracker *tunnels = ownerPlayer->getTunnelSystem();
	Object *bestTunnel = nullptr;
	Real bestDistSqr = 0;
	const std::list<ObjectID> *allTunnels = tunnels->getContainerList();
	for( std::list<ObjectID>::const_iterator iter = allTunnels->begin(); iter != allTunnels->end(); iter++ ) {
		// For each ID, look it up and change its team.  We all get captured together.
		Object *currentTunnel = TheGameLogic->findObjectByID( *iter );
		if( currentTunnel ) {
			Real dx = currentTunnel->getPosition()->x-pos->x;
			Real dy = currentTunnel->getPosition()->y-pos->y;
			Real distSqr = dx*dx+dy*dy;
			if (bestTunnel==nullptr || distSqr<bestDistSqr) {
				bestDistSqr = distSqr;
				bestTunnel = currentTunnel;
			}
		}
	}
	return bestTunnel;
}


//-- ExitConditions -------------------------------------------------------------------------------
/**
 * This returns true if the conditions specified have been met, false otherwise.
 */
Bool TunnelNetworkExitConditions::shouldExit(const StateMachine* machine) const
{

		if (TheGameLogic->getFrame() >= m_attackGiveUpFrame)
		{
			return true;
		}

	return false;
}


//-- AITNGuardMachine -------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------
AITNGuardMachine::AITNGuardMachine( Object *owner ) :
	StateMachine(owner, "AITNGuardMachine"),
	m_nemesisToAttack(INVALID_ID),
	m_guardMode(GUARDMODE_NORMAL)
{
	m_positionToGuard.zero();

	static const StateConditionInfo attackAggressors[] =
	{
		StateConditionInfo(hasAttackedMeAndICanReturnFire, AI_TN_GUARD_ATTACK_AGGRESSOR, nullptr),
		StateConditionInfo(nullptr, INVALID_STATE_ID, nullptr)
	};

	// order matters: first state is the default state.
// srj sez: I made "return" the start state, so that if ordered to guard a position
// that isn't the unit's current position, it moves to that position first.
	defineState( AI_TN_GUARD_RETURN,						newInstance(AITNGuardReturnState)( this ), AI_TN_GUARD_IDLE, AI_TN_GUARD_INNER, attackAggressors );
	defineState( AI_TN_GUARD_IDLE,							newInstance(AITNGuardIdleState)( this ), AI_TN_GUARD_INNER, AI_TN_GUARD_RETURN );
	defineState( AI_TN_GUARD_INNER,						newInstance(AITNGuardInnerState)( this ), AI_TN_GUARD_OUTER, AI_TN_GUARD_OUTER , attackAggressors);
	defineState( AI_TN_GUARD_OUTER,						newInstance(AITNGuardOuterState)( this ), AI_TN_GUARD_GET_CRATE, AI_TN_GUARD_GET_CRATE );
	defineState( AI_TN_GUARD_GET_CRATE,				newInstance(AITNGuardPickUpCrateState)( this ), AI_TN_GUARD_RETURN, AI_TN_GUARD_RETURN );
	defineState( AI_TN_GUARD_ATTACK_AGGRESSOR, newInstance(AITNGuardAttackAggressorState)( this ), AI_TN_GUARD_RETURN, AI_TN_GUARD_RETURN  );
#ifdef STATE_MACHINE_DEBUG
	setDebugOutput(true);
#endif
}

//--------------------------------------------------------------------------------------
AITNGuardMachine::~AITNGuardMachine()
{
}

//--------------------------------------------------------------------------------------
/*static*/ Real AITNGuardMachine::getStdGuardRange(const Object* obj)
{
	Real visionRange = TheAI->getAdjustedVisionRangeForObject(obj,
		AI_VISIONFACTOR_OWNERTYPE | AI_VISIONFACTOR_MOOD | AI_VISIONFACTOR_GUARDINNER);

	return visionRange;
}

//--------------------------------------------------------------------------------------
// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ T (gx-handoff ai/wh1ter0se-econ/DESIGN.md 7): with the owner's air watch on, an AIRCRAFT
// network nemesis is taken only by a guard that can attack it - the test the damaged-tunnel path of lookForInnerTarget already makes - so
// a Scorpion stays in (or keeps going in) for a MiG. Watch off, or any other nemesis: stock (taken).
// GeneralsX 23/10/2026 ai-wh1ter0se-econ T: is a watched plane ENGAGEABLE by this guard - within its weapon range + 50, or within 250 more and
// closing on it? ONE rule for popping out, turning back while returning, and giving up (inner and retaliation states): with separate
// rules a guard that the return state turned back for an out-of-reach plane was given up at once by the inner state, every frame
// (smoke A-T-7: 32,728 turn lines) - it never hid. Stateless, comparisons only.
Bool gxAirEngageable(const Object *guard, const Object *plane)	// GeneralsX 27/10/2026 T2: shared with the air watch (AITNGuard.h)
{
	const Real reach = guard->getLargestWeaponRange() + 50.0f;
	// a guard INSIDE the network deploys through the exit nearest the plane (AITNGuardIdleState picks it after lookForInnerTarget), so its
	// distance and closing are measured from that tunnel, not its stored position (Codex ke-tunnel-r10 P2); ties to the earlier tunnel
	Coord3D from = *guard->getPosition();
	if (guard->getContainedBy())
	{
		Player *owner = guard->getControllingPlayer();
		TunnelTracker *tunnels = owner ? owner->getTunnelSystem() : nullptr;
		if (tunnels)
		{
			Real best = -1.0f;
			const std::list<ObjectID> *all = tunnels->getContainerList();
			for (std::list<ObjectID>::const_iterator it = all->begin(); it != all->end(); ++it)
			{
				const Object *t = TheGameLogic->findObjectByID(*it);
				if (t == nullptr)
					continue;
				const Real d = ThePartitionManager->getDistanceSquared(t, plane, FROM_CENTER_2D);
				if (best < 0.0f || d < best)
				{
					best = d;
					from = *t->getPosition();
				}
			}
		}
	}
	const Real dx = from.x - plane->getPosition()->x, dy = from.y - plane->getPosition()->y;
	const Real d2 = dx * dx + dy * dy;
	if (d2 <= reach * reach)
		return TRUE;
	const Real lead = reach + 250.0f;
	if (d2 > lead * lead)
		return FALSE;
	const PhysicsBehavior *pp = plane->getPhysics();
	const Coord3D *v = pp ? pp->getVelocity() : nullptr;
	return v != nullptr && (v->x * dx + v->y * dy) > 0.0f;
}

// GeneralsX @feature Claude 27/10/2026 ai-pro-rules-2 T2 (wh1ter0se 27/10: "they need to target them manually"): with the owner's ACTING air
// watch on, a guard never takes an aircraft as its nemesis by itself - every shot at an aircraft from the tunnels is an assignment of
// the watch (AISkirmishPlayer::gxTunnelAirWatch: an explicit attack order on one plane, back in once it is gone). T's own rule (an
// aircraft taken only by a guard that can engage it) is what this replaces. Watch off or T0, or any other nemesis: stock (taken).
static Bool gxGuardTakesNemesis(const Object *owner, const Player *ownerPlayer, Object *nemesis)
{
	const AIPlayer *aip = ownerPlayer ? ownerPlayer->getAIPlayer() : nullptr;
	(void)owner;
	if (aip == nullptr || aip->gxAirWatchRadius() <= 0 || !nemesis->isKindOf(KINDOF_AIRCRAFT))
		return TRUE;
	return FALSE;
}

// GeneralsX 27/10/2026 ai-pro-rules-2 T2: a guard the air watch is bringing back in after its aircraft (AIPlayer::gxAirReturning)
static Bool gxAirReturningGuard(const Object *owner)
{
	const Player *p = owner ? owner->getControllingPlayer() : nullptr;
	const AIPlayer *aip = p ? p->getAIPlayer() : nullptr;
	return aip != nullptr && aip->gxAirWatchRadius() > 0 && aip->gxAirReturning(owner->getID());
}

Object *gxTunnelNearest(Player *ownerPlayer, const Coord3D *pos)	// GeneralsX 27/10/2026 T2: findBestTunnel for the air watch
{
	return findBestTunnel(ownerPlayer, pos);
}

Bool AITNGuardMachine::lookForInnerTarget()
{
	Object* owner = getOwner();

	// Check if team auto targets same victim.
	Object *teamVictim = nullptr;
	if (owner->getTeam()->getPrototype()->getTemplateInfo()->m_attackCommonTarget)
	{
		teamVictim = owner->getTeam()->getTeamTargetObject();
		if (teamVictim && !gxAirByAssignmentOnly(owner, teamVictim))	// GeneralsX 27/10/2026 T2
		{
			setNemesisID(teamVictim->getID());
			return true;	// Transitions to AITNGuardInnerState.
		}
	}

	// Find tunnel network to defend.
	// Scan my tunnels.
	Player *ownerPlayer = getOwner()->getControllingPlayer();
	if (!ownerPlayer) return false; // should never happen, but hey.  jba.
	TunnelTracker *tunnels = ownerPlayer->getTunnelSystem();
	if (tunnels==nullptr) return false;
	if (tunnels->getCurNemesis()) {
		Object *n = tunnels->getCurNemesis();
		if (gxGuardTakesNemesis(owner, ownerPlayer, n)) {
			if (n->isKindOf(KINDOF_AIRCRAFT) && gxAirWatchOn(ownerPlayer))
				GX_TAA_LOG("p=%d ev=pop unit=%u plane=%u", (Int)ownerPlayer->getPlayerIndex(), (unsigned)owner->getID(), (unsigned)n->getID());
			setNemesisID(n->getID());
			return true;	// Transitions to AITNGuardInnerState.
		}
	}
	const std::list<ObjectID> *allTunnels = tunnels->getContainerList();
	for( std::list<ObjectID>::const_iterator iter = allTunnels->begin(); iter != allTunnels->end(); iter++ ) {
		Object *currentTunnel = TheGameLogic->findObjectByID( *iter );
		if( currentTunnel ) {
			// Check for attacking.
			if (currentTunnel->getAI()) {
				Object *victim = currentTunnel->getAI()->getGoalObject();
				if (owner->getRelationship(victim) == ENEMIES && !gxAirByAssignmentOnly(owner, victim)) {	// GeneralsX 27/10/2026 T2 (Codex t2-engine-r1 #2)
					setNemesisID(victim->getID());
					return true;
				}
			}
			// check for attacked.
			BodyModuleInterface *body = currentTunnel->getBodyModule();
			if (body) {
				const DamageInfo *info = body->getLastDamageInfo();
				if (info) {
					if (info->out.m_noEffect) {
						continue;
					}
					const UnsignedInt *lastDamageTimestamp = body->getLastDamageTimestamp();
					if (lastDamageTimestamp && *lastDamageTimestamp + TheAI->getAiData()->m_guardEnemyScanRate > TheGameLogic->getFrame()) {
						// winner.
						ObjectID attackerID = info->in.m_sourceID;
						Object *attacker = TheGameLogic->findObjectByID(attackerID);

						if( attacker )
						{
							if (owner->getRelationship(attacker) != ENEMIES || gxAirByAssignmentOnly(owner, attacker)) {	// GeneralsX 27/10/2026 T2
								continue;
							}
							CanAttackResult result = getOwner()->getAbleToAttackSpecificObject(ATTACK_TUNNEL_NETWORK_GUARD, attacker, CMD_FROM_AI);
							if( result == ATTACKRESULT_POSSIBLE || result == ATTACKRESULT_POSSIBLE_AFTER_MOVING )
							{
								setNemesisID(attackerID);
								owner->getTeam()->setTeamTargetObject(attacker);
								tunnels->updateNemesis(attacker);
								return true;	// Transitions to AITNGuardInnerState.
							}
						}
					}
				}
			}
		}
	}

	return false;
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AITNGuardMachine::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void AITNGuardMachine::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 2;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

	if (version>=2) {
		StateMachine::xfer(xfer);	// Forgot this in initial implementation.  jba.
	}

	xfer->xferObjectID(&m_nemesisToAttack);
	xfer->xferCoord3D(&m_positionToGuard);

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AITNGuardMachine::loadPostProcess()
{
}

//-- AITNGuardInnerState ----------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AITNGuardInnerState::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void AITNGuardInnerState::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AITNGuardInnerState::loadPostProcess()
{
	onEnter();
}

// ------------------------------------------------------------------------------------------------
AITNGuardInnerState::~AITNGuardInnerState()
{
	deleteInstance(m_attackState);
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardInnerState::onEnter()
{
	Object* nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID()) ;
	if (nemesis == nullptr)
	{
		DEBUG_LOG(("Unexpected null nemesis in AITNGuardInnerState."));
		return STATE_SUCCESS;
	}
	m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;

	m_attackState = newInstance(AIAttackState)(getMachine(), false, true, false, &m_exitConditions);

	m_attackState->getMachine()->setGoalObject(nemesis);

	StateReturnType returnVal = m_attackState->onEnter();
	if (returnVal == STATE_CONTINUE) {
		return STATE_CONTINUE;
	}

	// if we had no one to attack, we were successful, so go to the next state.
	return STATE_SUCCESS;
}

static Object *TunnelNetworkScan(Object *owner)
{
		PartitionFilterRelationship					f1(owner, PartitionFilterRelationship::ALLOW_ENEMIES);
		PartitionFilterPossibleToAttack			f2(ATTACK_NEW_TARGET, owner, CMD_FROM_AI);
		PartitionFilterSameMapStatus				filterMapStatus(owner);

		PartitionFilter *filters[16];
		Int count = 0;

		filters[count++] = &f1;
		filters[count++] = &f2;
		filters[count++] = &filterMapStatus;

		Real visionRange = AITNGuardMachine::getStdGuardRange(owner);

		filters[count++] = nullptr;

		Object* target = ThePartitionManager->getClosestObject(owner->getPosition(), visionRange, FROM_CENTER_2D, filters);
		return target;
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardInnerState::update()
{
	Object* nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID()) ;
	Player *ownerPlayer = getMachineOwner()->getControllingPlayer();
	TunnelTracker *tunnels = nullptr;
	if (ownerPlayer) {
		tunnels = ownerPlayer->getTunnelSystem();
	}

	Object* owner = getMachineOwner();
	// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ T (DESIGN.md 7, "hide back instantly"): with the owner's air watch on, a guard
	// whose nemesis is an aircraft it cannot engage (gxAirEngageable - the rule the return state turns back by) gives up at once - a
	// stateless test each update (nothing to save) - and the Return state takes it back into the nearest tunnel.
	if (nemesis && nemesis->isKindOf(KINDOF_AIRCRAFT) && gxAirWatchOn(ownerPlayer))
	{
		GX_HOOK_TIMER_ON(gxTunnelAAHook);
		if (!gxAirEngageable(owner, nemesis))
			m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame();
	}
	// killed him.
	Object *teamVictim = owner->getTeam()->getTeamTargetObject();
	if (gxAirByAssignmentOnly(owner, teamVictim))	// GeneralsX 27/10/2026 ai-pro-rules-2 T2
		teamVictim = nullptr;
	if (nemesis == nullptr)
	{
		if (teamVictim)
		{
			getGuardMachine()->setNemesisID(teamVictim->getID());
			m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
			return STATE_CONTINUE;
		}

		// Check tunnel.
		if (tunnels) {
			nemesis = tunnels->getCurNemesis();
			if (gxAirByAssignmentOnly(owner, nemesis))	// GeneralsX 27/10/2026 ai-pro-rules-2 T2
				nemesis = nullptr;
			if (nemesis) {
				getGuardMachine()->setNemesisID(nemesis->getID());
				m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
				return STATE_CONTINUE;
			}
		}

		if (m_scanForEnemy) {
			m_scanForEnemy = false; // we just do 1 scan.
			nemesis = TunnelNetworkScan(owner);
			if (gxAirByAssignmentOnly(owner, nemesis))	// GeneralsX 27/10/2026 ai-pro-rules-2 T2
				nemesis = nullptr;
			if (nemesis) {
				// GeneralsX @bugfix m_attackState is only ever built by onEnter(), and this state can be
				// resident with a null one (see the guard at the end of this function), so resetting it
				// here could dereference null. Build the sub-state when it is missing instead of
				// resetting it, which leaves the re-target below working on a valid object either way.
				if (m_attackState == nullptr) {
					m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
					m_attackState = newInstance(AIAttackState)(getMachine(), false, true, false, &m_exitConditions);
				} else {
					m_attackState->onExit(EXIT_RESET);
				}
				m_attackState->getMachine()->setGoalObject(nemesis);
				if (tunnels) {
					tunnels->updateNemesis(nemesis);
				}
				StateReturnType returnVal = m_attackState->onEnter();
				return returnVal;
			}
		}
	} else {
		if (nemesis != teamVictim && teamVictim != nullptr) {
			tunnels->updateNemesis(nemesis);
			getGuardMachine()->setNemesisID(teamVictim->getID());
		}
	}

	// GeneralsX @bugfix m_attackState is only created by onEnter(), which bails out early when the
	// nemesis ID no longer resolves to a live object. loadPostProcess() calls onEnter() and throws the
	// return value away, so unlike a normal state entry - where STATE_SUCCESS would immediately
	// transition us out - the machine can sit in AI_TN_GUARD_INNER after a load with no sub-state at
	// all; the save only has to have been taken while the guard's nemesis was already dead. The
	// unguarded update() below then dereferenced null (EXC_BAD_ACCESS); the sibling guard states all
	// check first. Rebuild the sub-state when we still have something to attack, the same way
	// AITNGuardOuterState::update() does, and otherwise report success - which is exactly what
	// onEnter() returns when there is no nemesis, so the machine advances to the outer state.
	if (m_attackState == nullptr)
	{
		if (nemesis == nullptr)
		{
			return STATE_SUCCESS;
		}

		m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
		m_attackState = newInstance(AIAttackState)(getMachine(), false, true, false, &m_exitConditions);
		m_attackState->getMachine()->setGoalObject(nemesis);
		return m_attackState->onEnter();
	}

	return m_attackState->update();
}

//--------------------------------------------------------------------------------------
void AITNGuardInnerState::onExit( StateExitType status )
{
	if (m_attackState)
	{
		m_attackState->onExit(status);
		deleteInstance(m_attackState);
		m_attackState = nullptr;
	}
}

//-- AITNGuardOuterState ----------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AITNGuardOuterState::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void AITNGuardOuterState::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AITNGuardOuterState::loadPostProcess()
{
	// GeneralsX @bugfix A stray 'AITNGuardOuterState' sat here, which made the next line
	// parse as a declaration — 'AITNGuardOuterState onEnter();' — rather than a call. The
	// state was therefore never re-entered after loading a save, so guarding
	// units abandoned their guard orders on load.
	onEnter();
}

// ------------------------------------------------------------------------------------------------
AITNGuardOuterState::~AITNGuardOuterState()
{
	deleteInstance(m_attackState);
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardOuterState::onEnter()
{
	if (getGuardMachine()->getGuardMode() == GUARDMODE_GUARD_WITHOUT_PURSUIT)
	{
		// "patrol" mode does not follow targets outside the guard area.
		return STATE_SUCCESS;
	}

	Object* nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID()) ;
	if (nemesis == nullptr)
	{
		DEBUG_LOG(("Unexpected null nemesis in AITNGuardOuterState."));
		return STATE_SUCCESS;
	}
	// GeneralsX @feature Claude 22/10/2026 ai-wh1ter0se-econ T (DESIGN.md 7, "hide back instantly"): with the owner's air watch on, an
	// aircraft is never chased outside - the inner state gave it up because it left weapon range, and both of the inner state's exits
	// lead here (Codex ke-tunnel-r2 P2: this pursued it again with a fresh chase timer). On to the crate check and the Return state.
	if (nemesis->isKindOf(KINDOF_AIRCRAFT) && getMachineOwner()->getControllingPlayer()
		&& getMachineOwner()->getControllingPlayer()->getAIPlayer()
		&& getMachineOwner()->getControllingPlayer()->getAIPlayer()->gxAirWatchRadius() > 0)
		return STATE_SUCCESS;

	m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
	m_attackState = newInstance(AIAttackState)(getMachine(), false, true, false, &m_exitConditions);
	m_attackState->getMachine()->setGoalObject(nemesis);

	StateReturnType returnVal = m_attackState->onEnter();
	if (returnVal == STATE_CONTINUE) {
		return STATE_CONTINUE;
	}

	// if we had no one to attack, we were successful, so go to the next state.
	return STATE_SUCCESS;
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardOuterState::update()
{
	Object *owner = getMachineOwner();
	if (m_attackState == nullptr)
	{
		// GeneralsX @bugfix fbraz3 16/04/2026 Recreate missing attack sub-state to avoid null dereference in guard update.
		Object* nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID());
		if (nemesis == nullptr)
		{
			DEBUG_LOG(("AITNGuardOuterState::update has no attack state and no nemesis."));
			return STATE_SUCCESS;
		}
		// GeneralsX 22/10/2026 ai-wh1ter0se-econ T: the onEnter rule above (Codex ke-tunnel-r2 P2)
		if (nemesis->isKindOf(KINDOF_AIRCRAFT) && owner->getControllingPlayer() && owner->getControllingPlayer()->getAIPlayer()
			&& owner->getControllingPlayer()->getAIPlayer()->gxAirWatchRadius() > 0)
			return STATE_SUCCESS;

		m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
		m_attackState = newInstance(AIAttackState)(getMachine(), false, true, false, &m_exitConditions);
		m_attackState->getMachine()->setGoalObject(nemesis);

		StateReturnType returnVal = m_attackState->onEnter();
		if (returnVal != STATE_CONTINUE)
		{
			return returnVal;
		}
	}

	Object* goalObj = m_attackState->getMachineGoalObject();
	if (goalObj)
	{
	}	else {
		Object* nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID()) ;
		if (nemesis) {
			goalObj = nemesis;
		}
		// Check if team auto targets same victim.
		Object *teamVictim = nullptr;
		if (goalObj == nullptr
			&& owner->getTeam()
			&& owner->getTeam()->getPrototype()
			&& owner->getTeam()->getPrototype()->getTemplateInfo()->m_attackCommonTarget)
		{
			teamVictim = owner->getTeam()->getTeamTargetObject();
			if (teamVictim && !gxAirByAssignmentOnly(owner, teamVictim))	// GeneralsX 27/10/2026 T2 (Codex t2-engine-r1 #2)
			{
				goalObj = teamVictim;
			}
			m_attackState->getMachine()->setGoalObject(goalObj);
			return m_attackState->onEnter();
		}
	}

	return m_attackState->update();
}

//--------------------------------------------------------------------------------------
void AITNGuardOuterState::onExit( StateExitType status )
{
	if (m_attackState)
	{
		m_attackState->onExit(status);
		deleteInstance(m_attackState);
		m_attackState = nullptr;
	}
}

//-- AITNGuardReturnState ----------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AITNGuardReturnState::crc( Xfer *xfer )
{
	AIEnterState::crc(xfer);
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void AITNGuardReturnState::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

	AIEnterState::xfer(xfer);

	xfer->xferUnsignedInt(&m_nextReturnScanTime);
}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AITNGuardReturnState::loadPostProcess()
{
	AIEnterState::loadPostProcess();
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardReturnState::onEnter()
{
	UnsignedInt now = TheGameLogic->getFrame();
	m_nextReturnScanTime = now + GameLogicRandomValue(0, TheAI->getAiData()->m_guardEnemyReturnScanRate);

	if (getMachineOwner()->getContainedBy()) {
		// we are already inside our tunnel.  Return success. jba [8/24/2003]
		return STATE_SUCCESS;
	}
	// GeneralsX 27/10/2026 ai-pro-rules-2 T2: an AA guard the air watch is bringing back in goes straight in (wh1ter0se 27/10: "instantly go
	// back in") - no team target, no nemesis turns it round (update below); once inside it is an ordinary guard again
	if (getMachineOwner()->getTeam() && !gxAirReturningGuard(getMachineOwner())) {
		Object *teamVictim = getMachineOwner()->getTeam()->getTeamTargetObject();
		if (teamVictim && !gxAirByAssignmentOnly(getMachineOwner(), teamVictim))	{
			// We have a team target.  Go attack it rather than returning to the tunel. jba [8/24/2003]
			getGuardMachine()->setNemesisID(teamVictim->getID());
			return STATE_FAILURE; // Fail to return goes to inner attack state.
		}
	}
// no, no, no, don't do this in onEnter, unless you like really slow maps. (srj)
//	if (getGuardMachine()->lookForInnerTarget())
//		return STATE_FAILURE; // early termination because we found a target.

	// Find tunnel network to enter.
	// Scan my tunnels.
	Object *bestTunnel = findBestTunnel(getMachineOwner()->getControllingPlayer(), getMachineOwner()->getPosition());
	// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (Codex t2-engine-r4 #10): an AA guard coming back from its aircraft never fails into the
	// inner (attack) state - a failed entry goes to the idle state, whose next scan sends it back here
	const Bool gxBack = gxAirReturningGuard(getMachineOwner());
	if (bestTunnel==nullptr) return gxBack ? STATE_SUCCESS : STATE_FAILURE;

	getMachine()->setGoalObject(bestTunnel);
	getMachineOwner()->getAI()->friend_setGoalObject(bestTunnel);

	const StateReturnType gxEnter = AIEnterState::onEnter();
	return (gxBack && gxEnter == STATE_FAILURE) ? STATE_SUCCESS : gxEnter;
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardReturnState::update()
{
	Player *ownerPlayer = getMachineOwner()->getControllingPlayer();
	const Bool gxBack = gxAirReturningGuard(getMachineOwner());	// GeneralsX 27/10/2026 T2: straight in (onEnter)
	if (getMachineOwner()->getTeam() && !gxBack) {
		Object *teamVictim = getMachineOwner()->getTeam()->getTeamTargetObject();
		if (teamVictim && !gxAirByAssignmentOnly(getMachineOwner(), teamVictim))	{
			getGuardMachine()->setNemesisID(teamVictim->getID());
			return STATE_FAILURE; // Fail to return goes to inner attack state.
		}
	}
	// Check tunnel for target.
	TunnelTracker *tunnels = nullptr;
	if (ownerPlayer) {
		tunnels = ownerPlayer->getTunnelSystem();
	}

	if (tunnels && !gxBack) {
		Object *nemesis = tunnels->getCurNemesis();
		// GeneralsX 22/10/2026 ai-wh1ter0se-econ T: a guard that cannot attack the watch's aircraft keeps going in (Codex ke-tunnel-r3 P2: a
		// Scorpion on its way back was turned round again and again while a plane passed)
		if (nemesis && gxGuardTakesNemesis(getMachineOwner(), ownerPlayer, nemesis)) {
			if (nemesis->isKindOf(KINDOF_AIRCRAFT) && gxAirWatchOn(ownerPlayer))
				GX_TAA_LOG("p=%d ev=turn unit=%u plane=%u", (Int)ownerPlayer->getPlayerIndex(), (unsigned)getMachineOwner()->getID(),
					(unsigned)nemesis->getID());
			// Check distance.
			//Coord3D dist;
			//Coord3D curPos;
			//dist.set()
			getGuardMachine()->setNemesisID(nemesis->getID());
			return STATE_FAILURE; // Fail to return goes to inner attack state.
		}
	}

	// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (Codex t2-engine-r3 #10): an AA guard coming back from its aircraft whose destination tunnel
	// was captured or lost picks the best own tunnel again, instead of AIEnterState's attack on a hostile container
	if (gxBack)
	{
		const Object *goal = getMachineGoalObject();
		if (goal == nullptr || goal->isEffectivelyDead() || goal->getControllingPlayer() != ownerPlayer)
			return onEnter();
	}
	// Just let the return movement finish.
	StateReturnType ret = AIEnterState::update();
	if (ret==STATE_CONTINUE) return STATE_CONTINUE;
	if (gxAirWatchOn(ownerPlayer))
		GX_TAA_LOG("p=%d ev=hide unit=%u in=%d", (Int)ownerPlayer->getPlayerIndex(), (unsigned)getMachineOwner()->getID(),
			(Int)(getMachineOwner()->getContainedBy() != nullptr));
	return STATE_SUCCESS;

}

//--------------------------------------------------------------------------------------
void AITNGuardReturnState::onExit( StateExitType status )
{
	AIEnterState::onExit( status );
}

//-- AITNGuardIdleState ----------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AITNGuardIdleState::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void AITNGuardIdleState::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

	xfer->xferUnsignedInt(&m_nextEnemyScanTime);
}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AITNGuardIdleState::loadPostProcess()
{
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardIdleState::onEnter()
{
	// first time thru, use a random amount so that everyone doesn't scan on the same frame,
	// to avoid "spikes".
	UnsignedInt now = TheGameLogic->getFrame();
	m_nextEnemyScanTime = now + GameLogicRandomValue(0, TheAI->getAiData()->m_guardEnemyScanRate);

	getMachineOwner()->getAI()->friend_setGoalObject(nullptr);
	return STATE_CONTINUE;
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardIdleState::update()
{
	//DEBUG_LOG(("AITNGuardIdleState frame %d: %08lx",TheGameLogic->getFrame(),getMachineOwner()));

	UnsignedInt now = TheGameLogic->getFrame();
	if (now < m_nextEnemyScanTime)
		return STATE_SLEEP(m_nextEnemyScanTime - now);

	m_nextEnemyScanTime = now + TheAI->getAiData()->m_guardEnemyScanRate;

	getMachineOwner()->getAI()->friend_setGoalObject(nullptr);

#ifdef STATE_MACHINE_DEBUG
	//getMachine()->setDebugOutput(true);
#endif
	Object *owner = getMachineOwner();
	AIUpdateInterface *ai = owner->getAIUpdateInterface();
	// GeneralsX 27/10/2026 ai-pro-rules-2 T2 (Codex t2-engine-r2 #10): an AA guard the air watch is bringing back in, left outside by a
	// failed entry (its tunnel gone or full), goes back to the Return state at once - no target scan, no crate detour
	if (gxAirReturningGuard(owner) && !owner->getContainedBy() && findBestTunnel(owner->getControllingPlayer(), owner->getPosition()))
		return STATE_FAILURE;	// go to AITNGuardReturnState, & enter a tunnel.
	// Check to see if we have created a crate we need to pick up.
	if (ai->getCrateID() != INVALID_ID)
	{
		getMachine()->setState(AI_TN_GUARD_GET_CRATE);
		return STATE_SLEEP(m_nextEnemyScanTime - now);
	}

	// if anyone is in the inner area, return success.
	if (getGuardMachine()->lookForInnerTarget())
	{
		Object *nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID());
		if (nemesis == nullptr)
		{
			DEBUG_LOG(("Unexpected null nemesis in AITNGuardAttackAggressorState."));
			return STATE_SLEEP(0);
		}
		if (getMachineOwner()->getContainedBy()) {
			Object *bestTunnel = findBestTunnel(owner->getControllingPlayer(), nemesis->getPosition());
#if RETAIL_COMPATIBLE_CRC
			ExitInterface* goalExitInterface = bestTunnel->getContain() ? bestTunnel->getContain()->getContainExitInterface() : nullptr;
#else
			// GeneralsX @bugfix Claude 09/10/2026 "Contained by something" is not "inside this tunnel network": a guard
			// riding in a transport was thrown out through a tunnel, which cleared its containment and left it on the
			// transport's list; and with no tunnel left bestTunnel is null (Codex tsh-port-1008-sec-r1 P2). Only a
			// passenger of the network leaves through one of its tunnels.
			ContainModuleInterface *bestContain = bestTunnel != nullptr ? bestTunnel->getContain() : nullptr;
			if (bestContain == nullptr || !bestContain->isContained(getMachineOwner()))
				return STATE_FAILURE;
			ExitInterface* goalExitInterface = bestContain->getContainExitInterface();
#endif
			if( goalExitInterface == nullptr )
				return STATE_FAILURE;

			if( goalExitInterface->isExitBusy() )
				return STATE_SLEEP(0);// Just wait a sec.
			goalExitInterface->exitObjectInAHurry(getMachineOwner());
			return STATE_SLEEP(0);
		}
		return STATE_SUCCESS;	// Transitions to AITNGuardInnerState.
	}

	if (!owner->getContainedBy() && findBestTunnel(owner->getControllingPlayer(), owner->getPosition())) {
		return STATE_FAILURE;	 // go to AITNGuardReturnState, & enter a tunnel.
	}

	return STATE_SLEEP(m_nextEnemyScanTime - now);
}

//--------------------------------------------------------------------------------------
void AITNGuardIdleState::onExit( StateExitType status )
{
}

//-- AITNGuardPickUpCrateState ----------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
AITNGuardPickUpCrateState::AITNGuardPickUpCrateState( StateMachine *machine )	: AIPickUpCrateState(machine)
{
#ifdef STATE_MACHINE_DEBUG
		setName("AITNGuardPickUpCrateState");
#endif
}
//--------------------------------------------------------------------------------------
StateReturnType AITNGuardPickUpCrateState::onEnter()
{
	Object *owner = getMachineOwner();
	AIUpdateInterface *ai = owner->getAIUpdateInterface();

	// Check to see if we have created a crate we need to pick up.
	Object* crate = ai->checkForCrateToPickup();
	if (crate)
	{
		getMachine()->setGoalObject(crate);
		return AIPickUpCrateState::onEnter();
	}

	return STATE_SUCCESS; // no crate, so we're done.
}

//--------------------------------------------------------------------------------------
StateReturnType AITNGuardPickUpCrateState::update()
{
	return AIPickUpCrateState::update();
}

//--------------------------------------------------------------------------------------
void AITNGuardPickUpCrateState::onExit( StateExitType status )
{

}

//-- AITNGuardAttackAggressorState ------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
AITNGuardAttackAggressorState::AITNGuardAttackAggressorState( StateMachine *machine ) :
	State( machine, "AITNGuardAttackAggressorState" )
{
	m_attackState = nullptr;
}

// ------------------------------------------------------------------------------------------------
AITNGuardAttackAggressorState::~AITNGuardAttackAggressorState()
{
	deleteInstance(m_attackState);
}

//-------------------------------------------------------------------------------------------------
StateReturnType AITNGuardAttackAggressorState::onEnter()
{
	Object *obj = getMachineOwner();
	ObjectID nemID = INVALID_ID;

	if (obj->getBodyModule() && obj->getBodyModule()->getLastDamageInfo()->in.m_sourceID) {
		nemID = obj->getBodyModule()->getLastDamageInfo()->in.m_sourceID;
		getGuardMachine()->setNemesisID(nemID);

	}

	Object *nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID());
	if (nemesis == nullptr)
	{
		DEBUG_LOG(("Unexpected null nemesis in AITNGuardAttackAggressorState."));
		return STATE_SUCCESS;
	}

	Player *ownerPlayer = getMachineOwner()->getControllingPlayer();
	TunnelTracker *tunnels = nullptr;
	if (ownerPlayer) {
		tunnels = ownerPlayer->getTunnelSystem();
	}
	if (tunnels) tunnels->updateNemesis(nemesis);

	m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame() + TheAI->getAiData()->m_guardChaseUnitFrames;
	m_attackState = newInstance(AIAttackState)(getMachine(), true, true, false, &m_exitConditions);
	m_attackState->getMachine()->setGoalObject(nemesis);

	StateReturnType returnVal = m_attackState->onEnter();
	if (returnVal == STATE_CONTINUE) {
		return STATE_CONTINUE;
	}

	// if we had no one to attack, we were successful, so go to the next state.
	return STATE_SUCCESS;
}

//-------------------------------------------------------------------------------------------------
StateReturnType AITNGuardAttackAggressorState::update()
{
	// GeneralsX @bugfix Claude 06/10/2026 loadPostProcess() calls onEnter() and drops its result, and onEnter()
	// builds no m_attackState when the aggressor no longer resolves - so a save taken while a tunnel guard attacked a
	// unit that had just died left this state resident with null, dereferenced below on the first tick. Report what
	// onEnter() reported (SUCCESS: no one to attack), which moves the machine on as a normal entry would. The Inner and
	// Outer guard states already check this (sweep c009-01).
	if (m_attackState == nullptr)
	{
		return STATE_SUCCESS;
	}

	if (m_attackState->getMachine()->getCurrentStateID() == AttackStateMachine::FIRE_WEAPON) {
		Object *nemesis = TheGameLogic->findObjectByID(getGuardMachine()->getNemesisID());
		Player *ownerPlayer = getMachineOwner()->getControllingPlayer();
		TunnelTracker *tunnels = nullptr;
		if (ownerPlayer) {
			tunnels = ownerPlayer->getTunnelSystem();
		}
		if (tunnels) tunnels->updateNemesis(nemesis);
	}
	// GeneralsX 23/10/2026 ai-wh1ter0se-econ T: the inner state's air give-up here too - a guard hit by a strafing plane retaliates through this
	// state, which chased it for the full guardChaseUnitFrames after the watch had cleared it (Codex ke-tunnel-r7 P2). Stateless, per update.
	{
		Object *owner = getMachineOwner();
		Object *target = m_attackState->getMachineGoalObject();
		if (target && target->isKindOf(KINDOF_AIRCRAFT) && gxAirWatchOn(owner->getControllingPlayer()))
		{
			GX_HOOK_TIMER_ON(gxTunnelAAHook);
			if (!gxAirEngageable(owner, target))
				m_exitConditions.m_attackGiveUpFrame = TheGameLogic->getFrame();
		}
	}
	return m_attackState->update();
}

//-------------------------------------------------------------------------------------------------
void AITNGuardAttackAggressorState::onExit( StateExitType status )
{
	Object *obj = getMachineOwner();
	if (m_attackState)
	{
		m_attackState->onExit(status);
		deleteInstance(m_attackState);
		m_attackState = nullptr;
	}

	if (obj->getTeam())
	{
		obj->getTeam()->setTeamTargetObject(nullptr); // clear the target.
	}
}

//-------------------------------------------------------------------------------------------------
void AITNGuardAttackAggressorState::crc( Xfer *xfer )
{

}

//-------------------------------------------------------------------------------------------------
void AITNGuardAttackAggressorState::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

}

//-------------------------------------------------------------------------------------------------
void AITNGuardAttackAggressorState::loadPostProcess()
{
	onEnter();
}

