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

// GeneralsX @feature Claude 02/10/2026 ai-gym observation logger: the match state as NDJSON, schema v1
// (scripts/ai/gym/SCHEMA.md is the contract; bump GXGYM_SCHEMA_VERSION with any change to a line's fields).
//
// SELF-CONTAINED ON PURPOSE. The first learned AI will be trained on Generals Online replays decoded by GO's OWN client with
// this same logger patched in (lead C8, 02/10), so this file uses only engine concepts both forks share - Object, Player,
// ThePlayerList, ThePartitionManager, TheTerrainLogic, ThingTemplate, ScoreKeeper, TheVictoryConditions - and no GeneralsX helper. Its caller
// (GXGym.cpp here) decides when to call it and where the lines go.
//
// READS ONLY, AND ONLY GETTERS THAT CHANGE NOTHING. It runs on one peer (the bot's), so any write the simulation reads back
// would split that peer from the others. So, deliberately:
//  * never Object::getShroudedStatus - it recomputes a lazy cache the logic reads back (PartitionData::m_everSeenByPlayer).
//    Visibility is the const cell query ThePartitionManager->getShroudStatusForPlayer plus the stealth/detected status bits,
//    exactly GXOrderDriver::isVisibleTo; CLEAR only, never FOGGED vs SHROUDED (that distinction differs headless/rendered);
//  * never a name key: NAMEKEY/nameToKey on a string no peer interned yet mints a SEQUENTIAL key on this peer alone. KEYNAME
//    (keyToName) only reads, and the supply warehouse is found by dynamic_cast on its dock interface, not by module name;
//  * no random numbers, no AI group creation, no weapon or mood queries (TheSuperHackers #3328's unsafe-getters list).
// The [CRC] stream with this on and off on the same seed is the proof (ai/HANDBACK_ai-gym.md).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXGymObserve.h"

#include <vector>

#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ScoreKeeper.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/PhysicsUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"

namespace
{

std::vector<bool> s_described;	///< template id -> a "tmpl" line was written this match

enum ObjFlag
{
	OF_VISIBLE = 1,		///< the view player may see it (own, allied, or a clear cell and not an undetected stealther)
	OF_BUILDING = 2,	///< under construction
	OF_STEALTHED = 4,	///< stealthed and not detected
	OF_CONTAINED = 8,	///< inside a transport, building or tunnel
	OF_SOLD = 16,
	OF_BUSY = 32		///< a builder with a construction or repair task pending (what a player sees it doing)
};

void writeString(FILE *out, const char *s)
{
	fputc('"', out);
	for (; s != nullptr && *s != '\0'; ++s)
	{
		const unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			fprintf(out, "\\%c", c);
		else if (c < 0x20 || c >= 0x80)
			fprintf(out, "\\u%04x", c);	// one escape per BYTE: valid JSON whatever the encoding (SCHEMA.md: re-encode latin-1)
		else
			fputc(c, out);
	}
	fputc('"', out);
}

/// A number JSON can carry: a NaN or an infinity (none seen, nothing rules it out) is written as 0.
Real jsonNumber(Real v)
{
	return (v == v && v > -1.0e30f && v < 1.0e30f) ? v : 0.0f;
}

Bool mutualAllies(Player *a, Player *b)
{
	return a != nullptr && b != nullptr && a->getDefaultTeam() != nullptr && b->getDefaultTeam() != nullptr
		&& a->getRelationship(b->getDefaultTeam()) == ALLIES && b->getRelationship(a->getDefaultTeam()) == ALLIES;
}

Bool isPlayable(const Player *p)
{
	return p != nullptr && p->isPlayableSide();
}

/// Template class letters: S structure, I infantry, V vehicle, A aircraft, D builder, H gatherer, P supply source,
/// T tech building, C command center, W can attack, E power plant.
void writeKinds(FILE *out, const ThingTemplate *t)
{
	char k[16];
	Int n = 0;
	if (t->isKindOf(KINDOF_STRUCTURE)) k[n++] = 'S';
	if (t->isKindOf(KINDOF_INFANTRY)) k[n++] = 'I';
	if (t->isKindOf(KINDOF_VEHICLE)) k[n++] = 'V';
	if (t->isKindOf(KINDOF_AIRCRAFT)) k[n++] = 'A';
	if (t->isKindOf(KINDOF_DOZER)) k[n++] = 'D';
	if (t->isKindOf(KINDOF_HARVESTER)) k[n++] = 'H';
	if (t->isKindOf(KINDOF_SUPPLY_SOURCE)) k[n++] = 'P';
	if (t->isKindOf(KINDOF_TECH_BUILDING)) k[n++] = 'T';
	if (t->isKindOf(KINDOF_COMMANDCENTER)) k[n++] = 'C';
	if (t->isKindOf(KINDOF_CAN_ATTACK)) k[n++] = 'W';
	if (t->isKindOf(KINDOF_FS_POWER)) k[n++] = 'E';
	k[n] = '\0';
	writeString(out, k);
}

void describe(FILE *out, const char *prefix, const ThingTemplate *t)
{
	const UnsignedShort id = t->getTemplateID();
	if (s_described.size() <= id)
		s_described.resize((size_t)id + 1, false);
	if (s_described[id])
		return;
	s_described[id] = true;
	fprintf(out, "%s{\"v\":%d,\"t\":\"tmpl\",\"id\":%u,\"name\":", prefix, (Int)GXGYM_SCHEMA_VERSION, (unsigned)id);
	writeString(out, t->getName().str());
	fprintf(out, ",\"cost\":%d,\"k\":", t->friend_getBuildCost());
	writeKinds(out, t);
	fputs("}\n", out);
}

/// Worth a row: anything a playable player owns, and the neutral map features that matter to one (structures to garrison or
/// capture, supply piles, oil derricks). Not projectiles, not the dead, not scenery that moves by itself.
Bool worthARow(const Object *o)
{
	if (o->isEffectivelyDead() || o->isDestroyed() || o->isKindOf(KINDOF_PROJECTILE))
		return FALSE;
	return isPlayable(o->getControllingPlayer()) || o->isKindOf(KINDOF_STRUCTURE) || o->isKindOf(KINDOF_SUPPLY_SOURCE)
		|| o->isKindOf(KINDOF_TECH_BUILDING);
}

Bool visibleTo(const Object *o, Player *owner, Player *view)
{
	if (view == nullptr || owner == view || mutualAllies(owner, view))
		return TRUE;
	if (o->isContained())
		return FALSE;	// nobody outside sees into a transport or a building
	if (o->testStatus(OBJECT_STATUS_STEALTHED) && !o->testStatus(OBJECT_STATUS_DETECTED))
		return FALSE;
	return ThePartitionManager->getShroudStatusForPlayer(view->getPlayerIndex(), o->getPosition()) == CELLSHROUD_CLEAR;
}

} // namespace

void gxGymObserveReset()
{
	s_described.clear();
}

void gxGymObserveStart(FILE *out, const char *prefix, Int viewPlayerIndex)
{
	if (out == nullptr || ThePlayerList == nullptr)
		return;
	fprintf(out, "%s{\"v\":%d,\"t\":\"start\",\"f\":%u,\"hz\":%d,\"view\":%d,\"map\":", prefix, (Int)GXGYM_SCHEMA_VERSION,
		TheGameLogic != nullptr ? (unsigned)TheGameLogic->getFrame() : 0u, (Int)LOGICFRAMES_PER_SECOND, viewPlayerIndex);
	writeString(out, TheGlobalData != nullptr ? TheGlobalData->m_mapName.str() : "");
	Region3D ext;
	ext.lo.x = ext.lo.y = ext.hi.x = ext.hi.y = 0.0f;
	if (TheTerrainLogic != nullptr)
		TheTerrainLogic->getExtent(&ext);
	fprintf(out, ",\"ext\":[%.0f,%.0f,%.0f,%.0f],\"players\":[", jsonNumber(ext.lo.x), jsonNumber(ext.lo.y), jsonNumber(ext.hi.x),
		jsonNumber(ext.hi.y));
	Bool first = TRUE;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (!isPlayable(p))
			continue;
		// GeneralsX @bugfix Claude 07/11/2026 scale32: 64 bits and every Player, as the frame rows' `vis` below - at MAX_PLAYER_COUNT 40
		// an ally at index 32+ was dropped from the start record, so the readers split that alliance (Codex vnext1107-r1 P2).
		UnsignedInt64 allies = 0;
		for (Int j = 0; j < ThePlayerList->getPlayerCount() && j < MAX_PLAYER_COUNT; ++j)
			if (j != i && mutualAllies(p, ThePlayerList->getNthPlayer(j)))
				allies |= (UnsignedInt64)1 << j;
		fprintf(out, "%s{\"i\":%d,\"name\":", first ? "" : ",", (Int)p->getPlayerIndex());
		writeString(out, KEYNAME(p->getPlayerNameKey()).str());
		fputs(",\"side\":", out);
		writeString(out, p->getSide().str());
		fprintf(out, ",\"ai\":%d,\"diff\":%d,\"allies\":%llu}", p->isSkirmishAIPlayer() ? 1 : 0, (Int)p->getPlayerDifficulty(),
			(unsigned long long)allies);
		first = FALSE;
	}
	fputs("]}\n", out);
	fflush(out);
}

void gxGymObserveFrame(FILE *out, const char *prefix, UnsignedInt frame, Int viewPlayerIndex)
{
	if (out == nullptr || ThePlayerList == nullptr || TheGameLogic == nullptr || ThePartitionManager == nullptr)
		return;
	Player *view = (viewPlayerIndex >= 0) ? ThePlayerList->getNthPlayer(viewPlayerIndex) : nullptr;

	// Templates first, so a reader always knows a row's template before it meets the row.
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
		if (worthARow(o))
			describe(out, prefix, o->getTemplate());

	fprintf(out, "%s{\"v\":%d,\"t\":\"obs\",\"f\":%u,\"end\":%u,\"pl\":[", prefix, (Int)GXGYM_SCHEMA_VERSION, (unsigned)frame,
		TheVictoryConditions != nullptr ? (unsigned)TheVictoryConditions->getEndFrame() : 0u);
	Bool first = TRUE;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *p = ThePlayerList->getNthPlayer(i);
		if (!isPlayable(p))
			continue;
		ScoreKeeper *s = p->getScoreKeeper();
		const Bool alive = TheVictoryConditions == nullptr || !TheVictoryConditions->hasSinglePlayerBeenDefeated(p);
		fprintf(out, "%s[%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]", first ? "" : ",", (Int)p->getPlayerIndex(),
			alive ? 1 : 0, (unsigned)p->getMoney()->countMoney(), p->getEnergy()->getProduction(),
			p->getEnergy()->getConsumption(), p->getSciencePurchasePoints(), p->getRankLevel(), s->getTotalMoneyEarned(),
			s->getTotalMoneySpent(), s->getTotalUnitsBuilt(), s->getTotalUnitsLost(), s->getTotalUnitsDestroyed(),
			s->getTotalBuildingsBuilt(), s->getTotalBuildingsLost(), s->getTotalBuildingsDestroyed());
		first = FALSE;
	}

	// Everyone who could see a row, as a bit per player index - so one stream serves every player's fog view (a replay
	// decoded once gives each player's (what it saw, what it did) pairs).
	std::vector<Player *> seers;
	for (Int i = 0; i < ThePlayerList->getPlayerCount() && i < MAX_PLAYER_COUNT; ++i)	// GeneralsX 06/11/2026 every Player (was < 32; Codex scale32-r1 P2)
		if (isPlayable(ThePlayerList->getNthPlayer(i)))
			seers.push_back(ThePlayerList->getNthPlayer(i));

	fputs("],\"o\":[", out);
	first = TRUE;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (!worthARow(o))
			continue;
		Player *owner = o->getControllingPlayer();
		UnsignedInt64 vis = 0;	// GeneralsX 06/11/2026 64-bit: a Player index (and view's below) can pass 31 at MAX_PLAYER_COUNT 40
		for (size_t i = 0; i < seers.size(); ++i)
			if (visibleTo(o, owner, seers[i]))
				vis |= (UnsignedInt64)1 << seers[i]->getPlayerIndex();
		Int flags = 0;
		if (view == nullptr || (vis >> view->getPlayerIndex()) & 1u) flags |= OF_VISIBLE;
		if (o->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) flags |= OF_BUILDING;
		if (o->testStatus(OBJECT_STATUS_STEALTHED) && !o->testStatus(OBJECT_STATUS_DETECTED)) flags |= OF_STEALTHED;
		if (o->isContained()) flags |= OF_CONTAINED;
		if (o->testStatus(OBJECT_STATUS_SOLD)) flags |= OF_SOLD;
		AIUpdateInterface *ai = o->getAIUpdateInterface();
		DozerAIInterface *dozer = (ai != nullptr) ? ai->getDozerAIInterface() : nullptr;
		if (dozer != nullptr && dozer->isAnyTaskPending()) flags |= OF_BUSY;
		Int queue = -1;		// production entries waiting or in progress; -1 = not a producer
		if (const ProductionUpdateInterface *pu = o->getProductionUpdateInterface())
		{
			queue = 0;
			for (const ProductionEntry *e = pu->firstProduction(); e != nullptr; e = pu->nextProduction(e))
				++queue;
		}
		Int boxes = -1;
		if (o->isKindOf(KINDOF_SUPPLY_SOURCE))
		{
			// By type, not by module name: findUpdateModule wants a name key, and asking for one can mint it (see the top).
			const SupplyWarehouseDockUpdate *w = dynamic_cast<SupplyWarehouseDockUpdate *>(o->getDockUpdateInterface());
			if (w != nullptr)
				boxes = w->getBoxesStored();
		}
		const BodyModuleInterface *body = o->getBodyModule();
		const Coord3D *pos = o->getPosition();
		fprintf(out, "%s[%u,%d,%u,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%d,%llu,%d]", first ? "" : ",", (unsigned)o->getID(),
			isPlayable(owner) ? (Int)owner->getPlayerIndex() : -1, (unsigned)o->getTemplate()->getTemplateID(), jsonNumber(pos->x),
			jsonNumber(pos->y), body != nullptr ? jsonNumber(body->getHealth()) : 0.0f, body != nullptr ? jsonNumber(body->getMaxHealth()) : 0.0f,
			(Int)o->getVeterancyLevel(), flags, boxes,
			(flags & OF_BUILDING) != 0 ? (Int)o->getConstructionPercent() : 100, (unsigned long long)vis, queue);
		first = FALSE;
	}
	fputs("]}\n", out);
	fflush(out);
}

// GeneralsX @feature Claude 15/10/2026 pro-macro-data: the "ext" line - how each mobile object stands and moves, for
// analysis of decoded replays (which way a truck faces, whether it drives in reverse, which dock it uses, what it is
// ordered at). Same rules as above: const getters only, no name keys, no random numbers; heading and the forward speed
// are computed here from getOrientation and the physics velocity, not from Object::getUnitDirectionVector2D.
// Rows are self-contained (owner, template, x, y) because they also cover what the obs line leaves out: a vehicle whose
// crew was sniped belongs to the NEUTRAL player while it is unmanned (ActiveBody.cpp: DISABLED_UNMANNED + setTeam(neutral)),
// and worthARow keeps no neutral mobile object (Codex pro-macro-r1 P2).
namespace
{
Bool worthAnExtRow(const Object *o)
{
	if (o->isKindOf(KINDOF_STRUCTURE) || o->isEffectivelyDead() || o->isDestroyed() || o->isKindOf(KINDOF_PROJECTILE))
		return FALSE;
	return worthARow(o) || o->isDisabledByType(DISABLED_UNMANNED);
}
} // namespace

void gxGymObserveExtFrame(FILE *out, const char *prefix, UnsignedInt frame)
{
	if (out == nullptr || TheGameLogic == nullptr)
		return;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
		if (worthAnExtRow(o))
			describe(out, prefix, o->getTemplate());	// a neutral unmanned vehicle's template may not be described yet
	fprintf(out, "%s{\"v\":%d,\"t\":\"ext\",\"f\":%u,\"o\":[", prefix, (Int)GXGYM_SCHEMA_VERSION, (unsigned)frame);
	Bool first = TRUE;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (!worthAnExtRow(o))
			continue;
		const Player *owner = o->getControllingPlayer();
		const Coord3D *pos = o->getPosition();
		const Real heading = o->getOrientation();
		Real fwd = 0.0f;	// world units per second along the facing; negative = driving in reverse
		if (const PhysicsBehavior *phys = o->getPhysics())
		{
			const Coord3D *vel = phys->getVelocity();
			fwd = (vel->x * cosf(heading) + vel->y * sinf(heading)) * (Real)LOGICFRAMES_PER_SECOND;
		}
		Int state = -1, flags = 0;
		ObjectID goal = INVALID_ID, dock = INVALID_ID;
		Real gx = 0.0f, gy = 0.0f;
		Int carried = -1;
		if (o->isDisabledByType(DISABLED_UNMANNED)) flags |= 1;
		if (AIUpdateInterface *ai = o->getAIUpdateInterface())
		{
			state = (Int)ai->getAIStateType();
			if (ai->isMoving()) flags |= 2;
			if (ai->isIdle()) flags |= 4;
			if (const Object *g = ai->getGoalObject())	// non-const only by declaration: TheGameLogic->findObjectByID
				goal = g->getID();
			if (const Coord3D *gp = ai->getGoalPosition())
			{
				gx = gp->x;
				gy = gp->y;
			}
			if (const SupplyTruckAIInterface *truck = ai->getSupplyTruckAIInterface())
			{
				dock = truck->getPreferredDockID();
				carried = truck->getNumberBoxes();
			}
		}
		fprintf(out, "%s[%u,%d,%u,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%u,%.0f,%.0f,%u,%d,%d]", first ? "" : ",", (unsigned)o->getID(),
			isPlayable(owner) ? (Int)owner->getPlayerIndex() : -1, (unsigned)o->getTemplate()->getTemplateID(), jsonNumber(pos->x),
			jsonNumber(pos->y), jsonNumber(pos->z), jsonNumber(heading * 180.0f / PI), jsonNumber(fwd), state, (unsigned)goal,
			jsonNumber(gx), jsonNumber(gy), (unsigned)dock, carried, flags);
		first = FALSE;
	}
	fputs("]}\n", out);
	fflush(out);
}
