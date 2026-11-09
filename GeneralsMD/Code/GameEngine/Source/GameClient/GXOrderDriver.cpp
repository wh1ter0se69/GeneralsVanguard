/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX @test Claude 01/10/2026 GX_ORDERS - a scripted human, so a multiplayer test match is not an idle one.
//
// WHY. A headless human peer issues no orders, and skirmish AI orders are simulated locally on every peer rather than
// sent. So a 12-peer loss match or a 34-peer scale match put almost nothing on the wire (ROADMAP_2027 M2 Smooth-1 and
// M6; scripts/test/relay-npeer.sh "headless humans issue no orders"). This gives each human peer a seeded order stream
// at a stated rate, so the wire carries what real players send.
//
// HOW IT STAYS HONEST. Every order is the GameMessage the UI appends for the same click - a MSG_CREATE_SELECTED_GROUP
// and then one action - put into TheMessageStream, so it crosses the network like a human's and every peer executes it
// from the command list. Nothing here calls into GameLogic to act. The only queries are the ones the UI itself makes
// before sending the same order (canMakeUnit and requestUniqueUnitID as ControlBarCommandProcessing does,
// isLocationLegalToBuild with the PlaceEventTranslator's flags, findCommandSet), so a one-peer call here is exactly as
// safe as a human hovering a building. Its random numbers come from a private splitmix64, NEVER the logic or client
// streams: a logic-RNG draw on the issuing peer alone would be a desync.
//
// It lives in GameClient/, outside the SimID source digest, so a driver-on peer joins a stock peer of the same build.
//
//   GX_ORDERS=<n>         orders per minute (of logic time) for this peer's player, 0 < n <= 600; unset or 0 = off.
//                         An "order" is one replay command that is not housekeeping - the unit scripts/test/order-rate.py
//                         counts - so each action costs two: its selection and itself. GO 1v1/2v2 median: 66.
//   GX_ORDERS_SEED=<n>    default 1; the stream is this mixed with the player index, so peers differ.
//   GX_ORDERS_MIX=b:p:a:m relative weights of build, produce, attack, move. Default 7:32:20:41 (GO corpus, 5 matches).
//   GX_ORDERS_FROM=<f>    first logic frame that may issue an order, default 1.
//
// A bad value refuses the whole driver with one "[GXORDERS] refused" line. It never runs half-configured. It runs only in
// a command-line harness match (-lanhost / -lanjoin, headless or rendered): any other start - a GUI lobby, skirmish or
// the shell - refuses it the same way, so a shipped client carries no bot switch a player could turn on.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXOrderDriver.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "Common/BuildAssistant.h"
#include "Common/GameCommon.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/ThingTemplate.h"
#include "GameClient/ControlBar.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"

namespace
{

enum Kind { K_BUILD, K_PRODUCE, K_ATTACK, K_MOVE, K_COUNT };

const Int MAX_GROUP = 8;				///< units per move/attack selection
const Real MOVE_RANGE = 350.0f;			///< a move lands within this of the group, most of the time
const Real ATTACK_RANGE = 600.0f;		///< the nearest enemy this close is attacked directly
const Real MAP_MARGIN = 50.0f;
const Int MAX_BUILDERS = 3;				///< no more builders past this many: humans keep 2-3, and the Command Center's only
										///< button is a builder, so without a cap the starting cash all went on builders and
										///< no barracks - hence no unit to attack with - was ever afforded (g1-on run, 01/10)

struct Config
{
	Bool on;
	Real opm;
	uint64_t seed;
	Int weight[K_COUNT];
	UnsignedInt from;
};

Config s_cfg;
Bool s_configRead = FALSE;

// Per match. Reset by arm() when a game starts.
Bool s_armed = FALSE;
UnsignedInt s_lastFrame = 0;
Real s_budget = 0.0f;
uint64_t s_rng = 0;
Int s_actions[K_COUNT];
Int s_messages = 0;
Int s_starved = 0;

void say(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[GXORDERS] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	fflush(stderr);
}

Bool readConfig()
{
	const char *v = getenv("GX_ORDERS");
	if (v == nullptr || v[0] == '\0')
		return FALSE;
	char *end = nullptr;
	const double opm = strtod(v, &end);
	if (end == v || *end != '\0' || !(opm >= 0.0 && opm <= 600.0))	// written so NaN fails too
	{
		say("refused: GX_ORDERS='%s' is not an order rate in 0..600 per minute", v);
		return FALSE;
	}
	if (opm == 0.0)
		return FALSE;		// 0 means off, like GX_STRESS=0
	// A test-harness tool, never a bot switch in a player's game (lead C6, 01/10): only a command-line match - a
	// -lanhost or -lanjoin role, which the harnesses always set (headless or rendered) and a GUI lobby never does.
	if (TheGlobalData == nullptr || TheGlobalData->m_lanRole == LANROLE_NONE)
	{
		say("refused: not a harness match (GX_ORDERS needs a -lanhost or -lanjoin start)");
		return FALSE;
	}
	s_cfg.opm = (Real)opm;

	s_cfg.seed = 1;
	v = getenv("GX_ORDERS_SEED");
	if (v != nullptr && v[0] != '\0')
	{
		s_cfg.seed = strtoull(v, &end, 10);
		if (end == v || *end != '\0')
		{
			say("refused: GX_ORDERS_SEED='%s' is not a whole number", v);
			return FALSE;
		}
	}

	static const Int s_defaultMix[K_COUNT] = { 7, 32, 20, 41 };
	for (Int k = 0; k < K_COUNT; ++k)
		s_cfg.weight[k] = s_defaultMix[k];
	v = getenv("GX_ORDERS_MIX");
	if (v != nullptr && v[0] != '\0')
	{
		Int w[K_COUNT];
		int used = 0;
		// Each weight <= 1000000, so their sum (summed again per action in issueOne) cannot overflow an Int.
		if (sscanf(v, "%d:%d:%d:%d%n", &w[0], &w[1], &w[2], &w[3], &used) != 4 || v[used] != '\0'
			|| w[0] < 0 || w[1] < 0 || w[2] < 0 || w[3] < 0 || w[0] > 1000000 || w[1] > 1000000 || w[2] > 1000000
			|| w[3] > 1000000 || w[0] + w[1] + w[2] + w[3] <= 0)
		{
			say("refused: GX_ORDERS_MIX='%s' is not build:produce:attack:move weights (0..1000000 each, not all 0)", v);
			return FALSE;
		}
		for (Int k = 0; k < K_COUNT; ++k)
			s_cfg.weight[k] = w[k];
	}

	s_cfg.from = 1;
	v = getenv("GX_ORDERS_FROM");
	if (v != nullptr && v[0] != '\0')
	{
		const long f = strtol(v, &end, 10);
		if (end == v || *end != '\0' || f < 1 || f > 100000000)	// <= ~19 days at 60 Hz: never wraps an UnsignedInt
		{
			say("refused: GX_ORDERS_FROM='%s' is not a logic frame in 1..100000000", v);
			return FALSE;
		}
		s_cfg.from = (UnsignedInt)f;
	}
	return TRUE;
}

// splitmix64 (Steele, Lea, Flood 2014). Private on purpose - see the header comment.
UnsignedInt nextU32()
{
	uint64_t z = (s_rng += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return (UnsignedInt)((z ^ (z >> 31)) >> 32);
}

Int pick(Int n)
{
	return (n > 0) ? (Int)(nextU32() % (UnsignedInt)n) : 0;
}

Real unit01()
{
	return (Real)(nextU32() >> 8) * (1.0f / 16777216.0f);
}

template <typename T>
T takeRandom(std::vector<T> &v)
{
	const Int i = pick((Int)v.size());
	T t = v[i];
	v[i] = v.back();
	v.pop_back();
	return t;
}

// ------------------------------------------------------------------------------------------------
// What this player has, read once per action.

struct Mine
{
	std::vector<Object *> producers;	///< finished structures with a production queue
	std::vector<Object *> dozers;		///< anything that builds (a GLA Worker too) and has no build or repair task
	std::vector<Object *> movers;		///< mobile units that are neither dozers nor harvesters
	std::vector<Object *> fighters;		///< movers that can attack
	Int builders;						///< every builder this player owns, busy or not, plus those queued for production
	std::vector<Object *> spareWorkers;	///< builders not building or repairing, gatherers not gathering: moved only when there are no movers
	std::vector<Object *> gatherers;	///< busy gatherers: moved only when there is nothing else at all (a GLA player whose
										///< Workers are all hauling had nothing to command and starved - Mini m12-on, 01/10)
};

Bool isFinished(const Object *o)
{
	return !o->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) && !o->testStatus(OBJECT_STATUS_SOLD);
}

void gather(const Player *me, Mine &m)
{
	m.builders = 0;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (o->getControllingPlayer() != me || o->isEffectivelyDead() || !isFinished(o))
			continue;
		if (o->isKindOf(KINDOF_STRUCTURE))
		{
			const ProductionUpdateInterface *pu = o->getProductionUpdateInterface();
			if (pu != nullptr)
			{
				m.producers.push_back(o);
				// Builders already queued count toward the cap: their cost is withdrawn at queue time.
				for (const ProductionEntry *e = pu->firstProduction(); e != nullptr; e = pu->nextProduction(e))
					if (e->getProductionType() == PRODUCTION_UNIT && e->getProductionObject() != nullptr
						&& e->getProductionObject()->isKindOf(KINDOF_DOZER))
						++m.builders;
			}
			continue;
		}
		if (o->getAIUpdateInterface() == nullptr || o->isKindOf(KINDOF_IMMOBILE) || o->isKindOf(KINDOF_PROJECTILE)
			|| o->isContained())
			continue;
		// A builder with a build or repair task pending is never picked for a new build or a move: a second build makes
		// it abandon the first site (BuildAssistant::buildObjectNow idles it). Same read the ControlBar makes.
		AIUpdateInterface *ai = o->getAIUpdateInterface();
		DozerAIInterface *dozer = ai->getDozerAIInterface();
		// A builder that also gathers (a GLA Worker) is free only when idle: isAnyTaskPending knows nothing of ferrying.
		const Bool freeBuilder = o->isKindOf(KINDOF_DOZER) && dozer != nullptr && !dozer->isAnyTaskPending()
			&& (!o->isKindOf(KINDOF_HARVESTER) || ai->isIdle());
		if (o->isKindOf(KINDOF_DOZER) || o->isKindOf(KINDOF_HARVESTER))
		{
			// Spare: idle, or a pure builder (not a gatherer) that is only walking. A human re-clicks a walking builder;
			// one that is constructing, repairing or gathering is left at its job.
			if (ai->isIdle() || (freeBuilder && !o->isKindOf(KINDOF_HARVESTER)))
				m.spareWorkers.push_back(o);
			else if (o->isKindOf(KINDOF_HARVESTER) && (dozer == nullptr || !dozer->isAnyTaskPending()))
				m.gatherers.push_back(o);	// a Worker that is building or repairing is not a gatherer here (Codex r14)
		}
		if (o->isKindOf(KINDOF_DOZER))
		{
			++m.builders;
			if (freeBuilder)
				m.dozers.push_back(o);
		}
		else if (!o->isKindOf(KINDOF_HARVESTER))
		{
			m.movers.push_back(o);
			if (o->isKindOf(KINDOF_CAN_ATTACK))
				m.fighters.push_back(o);
		}
	}
}

Bool isEnemyTarget(const Object *o, const Player *me)
{
	const Player *owner = o->getControllingPlayer();
	return owner != nullptr && owner != me && owner->isPlayableSide() && owner->isPlayerActive()
		&& !o->isEffectivelyDead() && !o->isKindOf(KINDOF_PROJECTILE) && !o->isContained()
		&& o->getTeam() != nullptr && me->getRelationship(o->getTeam()) == ENEMIES;
}

/// Could a human click this enemy? Its cell is CLEAR for us and it is not an undetected stealther. Deliberately NOT
/// Object::getShroudedStatus: that recomputes a lazy cache the simulation reads back (PartitionData::m_everSeenByPlayer,
/// see FXList::doFXObj), so a call on one peer alone could split the peers. This is a const read of the cell.
Bool isVisibleTo(const Object *o, const Player *me)
{
	if (o->testStatus(OBJECT_STATUS_STEALTHED) && !o->testStatus(OBJECT_STATUS_DETECTED))
		return FALSE;
	return ThePartitionManager->getShroudStatusForPlayer(me->getPlayerIndex(), o->getPosition()) == CELLSHROUD_CLEAR;
}

void clampToMap(Coord3D *p)
{
	Region3D ext;
	TheTerrainLogic->getExtent(&ext);
	if (p->x < ext.lo.x + MAP_MARGIN) p->x = ext.lo.x + MAP_MARGIN;
	if (p->x > ext.hi.x - MAP_MARGIN) p->x = ext.hi.x - MAP_MARGIN;
	if (p->y < ext.lo.y + MAP_MARGIN) p->y = ext.lo.y + MAP_MARGIN;
	if (p->y > ext.hi.y - MAP_MARGIN) p->y = ext.hi.y - MAP_MARGIN;
	p->z = TheTerrainLogic->getGroundHeight(p->x, p->y);	// a click lands on the terrain
}

/// Is any object close enough to this footprint that BuildAssistant::isLocationClearOfObjects would examine it? That
/// function asks Object::getShroudedStatus of every object it examines (its potential collisions, and structures within
/// 2*(major+minor) for the exit check) - the lazy cache the simulation reads back, which must never be computed on one
/// peer alone (see isVisibleTo). So the driver only tries spots where it examines nothing. A pure read of positions; the
/// bounds cover iteratePotentialCollisions' 1.1 x bounding-sphere slop and the exit search, plus each object's own radius.
Bool anyObjectNear(const Coord3D &at, const ThingTemplate *what)
{
	const GeometryInfo &g = what->getTemplateGeometryInfo();
	Real reach = 2.0f * (g.getMajorRadius() + g.getMinorRadius());
	if (1.1f * g.getBoundingSphereRadius() > reach)
		reach = 1.1f * g.getBoundingSphereRadius();
	reach += 1.0f;
	for (const Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		const Real r = reach + o->getGeometryInfo().getBoundingSphereRadius();
		const Real dx = o->getPosition()->x - at.x;
		const Real dy = o->getPosition()->y - at.y;
		if (dx * dx + dy * dy < r * r)
			return TRUE;
	}
	return FALSE;
}

/// A random buildable thing from this object's command bar, of one button type, that the player can make right now. Only
/// what a human can click: never a SCRIPT_ONLY button (the ControlBar hides those), never a line-built wall (the UI sends
/// MSG_DOZER_CONSTRUCT_LINE with an end point for it, which this driver does not generate).
const ThingTemplate *pickButton(Object *o, GUICommandType type, Bool noBuilders = FALSE)
{
	const CommandSet *set = TheControlBar->findCommandSet(o->getCommandSetString());
	if (set == nullptr)
		return nullptr;
	const ThingTemplate *ok[MAX_COMMANDS_PER_SET];
	Int n = 0;
	for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *b = set->getCommandButton(i);
		if (b == nullptr || b->getCommandType() != type || BitIsSet(b->getOptions(), SCRIPT_ONLY))
			continue;
		const ThingTemplate *t = b->getThingTemplate();
		if (t != nullptr && !t->isKindOf(KINDOF_LINEBUILD) && !(noBuilders && t->isKindOf(KINDOF_DOZER))
			&& TheBuildAssistant->canMakeUnit(o, t) == CANMAKE_OK)
			ok[n++] = t;
	}
	return (n > 0) ? ok[pick(n)] : nullptr;
}

void select(const std::vector<Object *> &group)
{
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	msg->appendBooleanArgument(TRUE);	// a new group, as a click does
	for (size_t i = 0; i < group.size(); ++i)
		msg->appendObjectIDArgument(group[i]->getID());
	++s_messages;
}

GameMessage *order(GameMessage::Type type)
{
	++s_messages;
	return TheMessageStream->appendMessage(type);
}

std::vector<Object *> takeGroup(std::vector<Object *> &from)
{
	const Int n = (Int)from.size() < MAX_GROUP ? (Int)from.size() : MAX_GROUP;
	const Int want = 1 + pick(n);
	std::vector<Object *> group;
	for (Int i = 0; i < want; ++i)
		group.push_back(takeRandom(from));
	return group;
}

// ------------------------------------------------------------------------------------------------
// The four actions. Each returns TRUE once it has appended its selection and its order.

Bool doProduce(Mine &m)
{
	while (!m.producers.empty())
	{
		Object *factory = takeRandom(m.producers);
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		const ThingTemplate *what = pickButton(factory, GUI_COMMAND_UNIT_BUILD, m.builders >= MAX_BUILDERS);
		if (pu == nullptr || what == nullptr)
			continue;
		select(std::vector<Object *>(1, factory));
		GameMessage *msg = order(GameMessage::MSG_QUEUE_UNIT_CREATE);
		msg->appendIntegerArgument(what->getTemplateID());
		msg->appendIntegerArgument(0);	// as the ControlBar: dispatch allocates the production ID on every peer (sweep c066-04)
		return TRUE;
	}
	return FALSE;
}

Bool doBuild(Mine &m)
{
	// The PlaceEventTranslator's flags for a confirmed placement.
	const UnsignedInt flags = BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS
		| BuildAssistant::CLEAR_PATH | BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED
		| BuildAssistant::IGNORE_STEALTHED | BuildAssistant::FAIL_STEALTHED_WITHOUT_FEEDBACK;
	while (!m.dozers.empty())
	{
		Object *dozer = takeRandom(m.dozers);
		const ThingTemplate *what = pickButton(dozer, GUI_COMMAND_DOZER_CONSTRUCT);
		if (what == nullptr)
			continue;
		const Real angle = what->getPlacementViewAngle();
		for (Int attempt = 0; attempt < 8; ++attempt)
		{
			const Real r = 60.0f + 260.0f * unit01();
			const Real a = 6.2831853f * unit01();
			Coord3D at = *dozer->getPosition();
			at.x += r * cosf(a);
			at.y += r * sinf(a);
			clampToMap(&at);
			if (anyObjectNear(at, what)		// first: see anyObjectNear - never let the legality check touch the shroud cache
				|| TheBuildAssistant->isLocationLegalToBuild(&at, what, angle, flags, dozer, nullptr) != LBC_OK)
				continue;
			select(std::vector<Object *>(1, dozer));
			GameMessage *msg = order(GameMessage::MSG_DOZER_CONSTRUCT);
			msg->appendIntegerArgument(what->getTemplateID());
			msg->appendLocationArgument(at);
			msg->appendRealArgument(angle);
			return TRUE;
		}
	}
	return FALSE;
}

Bool doAttack(Mine &m, const Player *me)
{
	if (m.fighters.empty())
		return FALSE;
	std::vector<Object *> group = takeGroup(m.fighters);
	const Coord3D *from = group[0]->getPosition();
	const Object *nearest = nullptr;
	Real bestD2 = ATTACK_RANGE * ATTACK_RANGE;
	std::vector<const Object *> structures;
	for (const Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (!isEnemyTarget(o, me))
			continue;
		if (o->isKindOf(KINDOF_STRUCTURE))
			structures.push_back(o);
		const Real dx = o->getPosition()->x - from->x;
		const Real dy = o->getPosition()->y - from->y;
		if (dx * dx + dy * dy < bestD2 && isVisibleTo(o, me))	// a direct attack needs a target a human could click
		{
			bestD2 = dx * dx + dy * dy;
			nearest = o;
		}
	}
	if (nearest != nullptr)
	{
		select(group);
		order(GameMessage::MSG_DO_ATTACK_OBJECT)->appendObjectIDArgument(nearest->getID());
		return TRUE;
	}
	if (structures.empty())
		return FALSE;
	Coord3D at = *structures[pick((Int)structures.size())]->getPosition();
	clampToMap(&at);
	select(group);
	order(GameMessage::MSG_DO_ATTACKMOVETO)->appendLocationArgument(at);
	return TRUE;
}

Bool doMove(Mine &m)
{
	// Early in a match a player owns only its Command Center and its builders, and once the starting cash is spent
	// nothing can be produced or built either - so with no units to move, a spare builder walks instead, as a human's
	// does. A busy one (constructing, repairing, gathering) is never pulled off its job.
	// Last resort, a gatherer: a human with nothing else to click moves a worker, income or not. A builder that is
	// constructing or repairing is never moved.
	std::vector<Object *> &pool = !m.movers.empty() ? m.movers : !m.spareWorkers.empty() ? m.spareWorkers : m.gatherers;
	if (pool.empty())
		return FALSE;
	std::vector<Object *> group = takeGroup(pool);
	Coord3D at;
	if (unit01() < 0.25f)
	{
		Region3D ext;
		TheTerrainLogic->getExtent(&ext);
		at.x = ext.lo.x + (ext.hi.x - ext.lo.x) * unit01();
		at.y = ext.lo.y + (ext.hi.y - ext.lo.y) * unit01();
	}
	else
	{
		at.x = at.y = 0.0f;
		for (size_t i = 0; i < group.size(); ++i)
		{
			at.x += group[i]->getPosition()->x;
			at.y += group[i]->getPosition()->y;
		}
		at.x = at.x / (Real)group.size() + MOVE_RANGE * (2.0f * unit01() - 1.0f);
		at.y = at.y / (Real)group.size() + MOVE_RANGE * (2.0f * unit01() - 1.0f);
	}
	at.z = 0.0f;
	clampToMap(&at);
	select(group);
	order(GameMessage::MSG_DO_MOVETO)->appendLocationArgument(at);
	return TRUE;
}

/// One action: a kind drawn by the mix, then the other kinds by weight if that one has nothing to act on.
Bool issueOne(const Player *me)
{
	Int total = 0;
	for (Int k = 0; k < K_COUNT; ++k)
		total += s_cfg.weight[k];
	Int roll = pick(total);
	Int first = 0;
	while (roll >= s_cfg.weight[first])
		roll -= s_cfg.weight[first++];

	Int tryOrder[K_COUNT];
	Int n = 0;
	tryOrder[n++] = first;
	for (Int pass = 0; pass < K_COUNT; ++pass)		// the rest, heaviest first
	{
		Int best = -1;
		for (Int k = 0; k < K_COUNT; ++k)
		{
			Bool taken = FALSE;
			for (Int j = 0; j < n; ++j)
				taken = taken || tryOrder[j] == k;
			if (!taken && s_cfg.weight[k] > 0 && (best < 0 || s_cfg.weight[k] > s_cfg.weight[best]))
				best = k;
		}
		if (best >= 0)
			tryOrder[n++] = best;
	}

	Mine m;
	gather(me, m);
	for (Int i = 0; i < n; ++i)
	{
		Bool done = FALSE;
		switch (tryOrder[i])
		{
			case K_BUILD:   done = doBuild(m); break;
			case K_PRODUCE: done = doProduce(m); break;
			case K_ATTACK:  done = doAttack(m, me); break;
			default:        done = doMove(m); break;
		}
		if (done)
		{
			++s_actions[tryOrder[i]];
			return TRUE;
		}
	}
	return FALSE;
}

void arm(const Player *me, UnsignedInt frame)
{
	s_armed = TRUE;
	s_lastFrame = frame;
	s_budget = 0.0f;
	s_messages = 0;
	s_starved = 0;
	for (Int k = 0; k < K_COUNT; ++k)
		s_actions[k] = 0;
	const UnsignedInt index = (UnsignedInt)me->getPlayerIndex();
	s_rng = s_cfg.seed * 0x9E3779B97F4A7C15ull ^ ((uint64_t)index + 1) * 0xD1B54A32D192ED03ull;
	say("on: %.1f orders/min for player %u, seed %llu, mix build %d produce %d attack %d move %d, from frame %u",
		s_cfg.opm, index, (unsigned long long)s_cfg.seed, s_cfg.weight[K_BUILD], s_cfg.weight[K_PRODUCE],
		s_cfg.weight[K_ATTACK], s_cfg.weight[K_MOVE], s_cfg.from);
}

} // namespace

Bool gxOrderIsVisibleTo(const Object *o, const Player *me) { return isVisibleTo(o, me); }
Bool gxOrderIsEnemyTarget(const Object *o, const Player *me) { return isEnemyTarget(o, me); }
Bool gxOrderAnyObjectNear(const Coord3D &at, const ThingTemplate *what) { return anyObjectNear(at, what); }

void gxOrderDriverUpdate()
{
	if (!s_configRead)
	{
		s_configRead = TRUE;
		s_cfg.on = readConfig();
	}
	if (!s_cfg.on)
		return;

	if (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
		|| TheGameLogic->isLoadingMap())
	{
		s_armed = FALSE;
		return;
	}
	if (TheRecorder != nullptr && TheRecorder->isPlaybackMode())
		return;		// a replay's orders are the recording's, not ours
	const Player *me = (ThePlayerList != nullptr) ? ThePlayerList->getLocalPlayer() : nullptr;
	if (me == nullptr || !me->isPlayableSide() || !me->isPlayerActive() || TheControlBar == nullptr
		|| TheBuildAssistant == nullptr || TheTerrainLogic == nullptr || TheMessageStream == nullptr
		|| ThePartitionManager == nullptr)
		return;		// an observer, a defeated player, or a client without a command bar

	const UnsignedInt frame = TheGameLogic->getFrame();
	if (!s_armed || frame < s_lastFrame)
		arm(me, frame);
	if (frame == s_lastFrame)
		return;		// one decision per logic frame, however often the client updates
	const UnsignedInt steps = frame - s_lastFrame;
	s_lastFrame = frame;
	if (frame < s_cfg.from)
		return;

	// Two orders per action (selection + order). The cap stops a stretch with nothing to command from turning into a
	// burst later: a human with no units does not bank clicks.
	s_budget += (Real)steps * s_cfg.opm / (60.0f * (Real)LOGICFRAMES_PER_SECOND);
	if (s_budget > 4.0f)
		s_budget = 4.0f;
	// A failed attempt spends its budget too (Codex r11): attempts, and the object scans they cost, stay at the order rate
	// instead of repeating every frame while a player has nothing it can do.
	while (s_budget >= 2.0f)
	{
		s_budget -= 2.0f;
		if (!issueOne(me))
		{
			++s_starved;
			break;
		}
	}

	if (frame % 600 == 0)
		say("f=%u sent=%d build=%d produce=%d attack=%d move=%d starved=%d (driver's own count; the gate counts the replay)",
			frame, s_messages, s_actions[K_BUILD], s_actions[K_PRODUCE], s_actions[K_ATTACK], s_actions[K_MOVE], s_starved);
}
