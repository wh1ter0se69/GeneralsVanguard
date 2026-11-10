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

// GeneralsX: ai-gym - a training ground on our own engine (ai/PROMPT_ai-gym.md): OBSERVE a match as
// NDJSON (GXGymObserve.cpp) and ACT for this peer's player from lines a training process writes (GXGymProtocol.h).
//
// HOW ACTING STAYS HONEST. Every action becomes the GameMessages the UI appends for the same click - MSG_CREATE_SELECTED_GROUP
// and then one order - in TheMessageStream, exactly as GXOrderDriver (GX_ORDERS) does. So it crosses the network like a human's,
// every peer executes it from the command list, and the replay records it. Nothing here calls into GameLogic to act. Each
// action is checked first against what a human could click: only this player's own live units, an attack only on a visible
// enemy (GXOrderDriver's cell + stealth test, never Object::getShroudedStatus), a build only of a button the builder shows, that
// the player can afford, on a spot the PlaceEventTranslator's checks accept (behind GXOrderDriver's anyObjectNear guard), and no
// more than GX_ACT_APM actions a minute. A refused action writes a "rej" line with the reason and sends nothing.
//
// It lives in GameClient/, outside the SimID source digest: an observing or acting peer joins a stock peer of the same build.
//
//   GX_OBS=<n>               write an "obs" line every n logic frames (1..36000); unset or 0 = off.
//   GX_OBS_OUT=<file>        where the lines go (truncated per match); unset = stderr, each line prefixed "[GXOBS] ".
//   GX_OBS_VIEW=local|all|<i> whose fog marks the rows' visible bit: this peer's player (default with GX_ACT), nobody (default
//                            without; every row visible), or player index i.
//   GX_OBS_EXT=1             after each obs line also an "ext" line: height, heading, forward speed (negative = reversing),
//                            AI state, goal object/position, a harvester's preferred dock and boxes carried, unmanned/moving/
//                            idle bits, for every mobile object (scripts/ai/gym/SCHEMA.md). Analysis of decoded replays only.
//   GX_ACT=<file>            read action lines from this file as it grows (it must exist when the match starts).
//   GX_ACT_SYNC=1            lock-step with the trainer: after each obs line the match waits for a "step" line.
//   GX_ACT_APM=<n>           actions per minute of logic time, 1..6000, default 600 (GX_ORDERS' ceiling).
//   GX_ACT_TIMEOUT_MS=<n>    GX_ACT_SYNC: give up waiting after this long and stop acting, default 120000.
//   GX_GYM_ALLOW_HUMANS=1    allow both in a match with more than one human (the 2-peer lockstep proof). Honoured ONLY in a
//                            GX_TEST_INSTRUMENTS build (never shipped): in a player's binary it would be a maphack and a bot
//                            in a stranger's room through -lanjoin.
//
// WHERE IT MAY RUN. Observing is a full map reveal and acting is a bot, so neither runs in a player's game: only in a
// command-line harness match (-lanhost / -lanjoin) with at most one human - GameLogic::gxCheatsAllowed, the cheats' own
// sandbox rule, so the bot's own seat is that one human - or with GX_GYM_ALLOW_HUMANS=1 in a test-instruments build. Observing
// a REPLAY is allowed (a replay viewer sees everything anyway); acting in one is not.
//
// EVERY action line costs one unit of the GX_ACT_APM budget, refused or not, and one update reads at most MAX_LINES_PER_UPDATE
// of them: validation is real work (a build search walks the object list), and a refusal is information (review r1 P2).

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXGym.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#include "Common/ActionManager.h"
#include "Common/BuildAssistant.h"
#include "Common/GameCommon.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameClient/GXGymObserve.h"
#include "GameClient/GXGymProtocol.h"
#include "GameClient/GXOrderDriver.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/ProductionUpdate.h"

namespace
{

const Real MAP_MARGIN = 50.0f;		///< GXOrderDriver's: a click lands inside the playable map
const Real APM_BURST = 20.0f;		///< actions that may be banked
const Int MAX_LINE = 1024;
const Int MAX_LINES_PER_UPDATE = 64;

struct Config
{
	Int obsPeriod;
	AsciiString obsOut;
	AsciiString view;		///< "local", "all" or a player index
	AsciiString actPath;
	Bool sync;
	Real apm;
	UnsignedInt timeoutMs;
	Bool allowHumans;
	Bool scenario;		///< GX_GYM_SCENARIO=1: the spawn / clear controller verbs (Track B drills)
	Bool obsExt;		///< GX_OBS_EXT=1: an "ext" line after each obs line
};

Config s_cfg;
Bool s_configRead = FALSE;
Bool s_on = FALSE;

// Per match. arm() sets them when a game is entered, disarm() when it is left.
Bool s_armed = FALSE;
Bool s_soloPlacement = FALSE;	///< decided in arm(): this match has one peer, so build spots use the engine's own check
Bool s_acting = FALSE;
FILE *s_obs = nullptr;
const char *s_prefix = "";
FILE *s_act = nullptr;
std::string s_pending;		///< the start of a line whose end the trainer has not written yet
Bool s_overlong = FALSE;	///< the pending line passed MAX_LINE: the whole line is refused at its end
Bool s_hasNul = FALSE;		///< the pending line holds a NUL byte
Int s_lineNo = 0;
Int s_viewIndex = -1;
UnsignedInt s_lastObs = 0xffffffffu;
UnsignedInt s_lastFrame = 0;
Real s_budget = 0.0f;
Int s_sent = 0, s_refused = 0;

void say(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[GXGYM] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	fflush(stderr);
}

Bool readInt(const char *name, Int lo, Int hi, Int def, Int *out)
{
	const char *v = getenv(name);
	if (v == nullptr || v[0] == '\0')
	{
		*out = def;
		return TRUE;
	}
	char *end = nullptr;
	const long n = strtol(v, &end, 10);
	if (end == v || *end != '\0' || n < lo || n > hi)
	{
		say("refused: %s='%s' is not a whole number in %d..%d", name, v, lo, hi);
		return FALSE;
	}
	*out = (Int)n;
	return TRUE;
}

Bool readConfig()
{
	const char *act = getenv("GX_ACT");
	if (!readInt("GX_OBS", 0, 36000, 0, &s_cfg.obsPeriod))
		return FALSE;
	Int ext = 0;
	if (!readInt("GX_OBS_EXT", 0, 1, 0, &ext))
		return FALSE;
	s_cfg.obsExt = (ext == 1);
	if (s_cfg.obsPeriod == 0 && (act == nullptr || act[0] == '\0'))
		return FALSE;		// neither asked for: off, silently, like every other GX_ switch

	const char *v = getenv("GX_OBS_OUT");
	s_cfg.obsOut = (v != nullptr) ? v : "";
	v = getenv("GX_OBS_VIEW");
	s_cfg.view = (v != nullptr && v[0] != '\0') ? v : ((act != nullptr && act[0] != '\0') ? "local" : "all");
	if (s_cfg.view != "local" && s_cfg.view != "all"
		&& (s_cfg.view.getLength() > 2 || s_cfg.view.str()[strspn(s_cfg.view.str(), "0123456789")] != '\0'))
	{
		say("refused: GX_OBS_VIEW='%s' is not local, all or a player index", s_cfg.view.str());
		return FALSE;
	}
	s_cfg.actPath = (act != nullptr) ? act : "";
	Int n = 0;
	if (!readInt("GX_ACT_SYNC", 0, 1, 0, &n))
		return FALSE;
	s_cfg.sync = (n == 1);
	if (!readInt("GX_ACT_APM", 1, 6000, 600, &n))
		return FALSE;
	s_cfg.apm = (Real)n;
	if (!readInt("GX_ACT_TIMEOUT_MS", 1, 3600000, 120000, &n))
		return FALSE;
	s_cfg.timeoutMs = (UnsignedInt)n;
	if (!readInt("GX_GYM_SCENARIO", 0, 1, 0, &n))
		return FALSE;
	s_cfg.scenario = (n == 1);
	if (!readInt("GX_GYM_ALLOW_HUMANS", 0, 1, 0, &n))
		return FALSE;
#if defined(GX_TEST_INSTRUMENTS)
	s_cfg.allowHumans = (n == 1);
#else
	s_cfg.allowHumans = FALSE;
	if (n == 1)
		say("GX_GYM_ALLOW_HUMANS ignored: honoured only in a GX_TEST_INSTRUMENTS build");
#endif
	if (s_cfg.sync && (s_cfg.actPath.isEmpty() || s_cfg.obsPeriod == 0))
	{
		say("refused: GX_ACT_SYNC=1 needs GX_ACT and GX_OBS (a step ends at each obs line)");
		return FALSE;
	}
	return TRUE;
}

Bool inPlayback()
{
	return TheRecorder != nullptr && TheRecorder->isPlaybackMode();
}

// ------------------------------------------------------------------------------------------------
// Output lines besides the observer's: acknowledgements, refusals, the end.

void writeAck(UnsignedInt frame, const Coord3D *at)
{
	if (s_obs == nullptr)
		return;
	if (at != nullptr)
		fprintf(s_obs, "%s{\"v\":%d,\"t\":\"ack\",\"f\":%u,\"n\":%d,\"x\":%.0f,\"y\":%.0f}\n", s_prefix,
			(Int)GXGYM_SCHEMA_VERSION, (unsigned)frame, s_lineNo, at->x, at->y);
	else
		fprintf(s_obs, "%s{\"v\":%d,\"t\":\"ack\",\"f\":%u,\"n\":%d}\n", s_prefix, (Int)GXGYM_SCHEMA_VERSION, (unsigned)frame,
			s_lineNo);
}

void writeRej(UnsignedInt frame, const char *why)
{
	++s_refused;
	if (s_obs != nullptr)
		fprintf(s_obs, "%s{\"v\":%d,\"t\":\"rej\",\"f\":%u,\"n\":%d,\"why\":\"%s\"}\n", s_prefix, (Int)GXGYM_SCHEMA_VERSION,
			(unsigned)frame, s_lineNo, why);	// every reason is a fixed string literal: nothing to escape
}

// ------------------------------------------------------------------------------------------------
// Validation: could a human click this?

/// A live, finished unit or structure of mine that a click could select (not riding in a transport or a building, and not
/// one SelectionXlat refuses to select: UNSELECTABLE, MASKED, !isSelectable).
Object *myObject(Int id, const Player *me)
{
	Object *o = TheGameLogic->findObjectByID((ObjectID)id);
	if (o == nullptr || o->getControllingPlayer() != me || o->isEffectivelyDead() || o->isDestroyed() || o->isContained()
		|| o->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || o->testStatus(OBJECT_STATUS_SOLD)
		|| o->testStatus(OBJECT_STATUS_UNSELECTABLE) || o->testStatus(OBJECT_STATUS_MASKED) || !o->isSelectable())
		return nullptr;
	return o;
}

Bool onMap(Real x, Real y, Coord3D *out)
{
	Region3D ext;
	TheTerrainLogic->getExtent(&ext);
	if (x < ext.lo.x + MAP_MARGIN || x > ext.hi.x - MAP_MARGIN || y < ext.lo.y + MAP_MARGIN || y > ext.hi.y - MAP_MARGIN)
		return FALSE;
	out->x = x;
	out->y = y;
	out->z = TheTerrainLogic->getGroundHeight(x, y);	// a click lands on the terrain
	return TRUE;
}

/// The first button on this object's command bar that `match` accepts and that the bar shows: never a SCRIPT_ONLY button, never
/// a general's power whose science the player lacks (ControlBarCommand.cpp populates the bar with the same two rules - but
/// here whatever the button's NEED_SPECIAL_POWER_SCIENCE bit says, as the simulation gates the power (AIGroup): asking a
/// SharedSyncedTimer power that is not owned yet whether it is ready would start its timer on this peer alone (review r2 P3).
template <typename Match>
const CommandButton *findButton(const Object *o, const Player *me, Match match)
{
	const CommandSet *set = TheControlBar->findCommandSet(o->getCommandSetString());
	if (set == nullptr)
		return nullptr;
	for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *b = set->getCommandButton(i);
		if (b == nullptr || BitIsSet(b->getOptions(), SCRIPT_ONLY) || !match(b))
			continue;
		const SpecialPowerTemplate *sp = b->getSpecialPowerTemplate();
		const GUICommandType t = b->getCommandType();
		if (sp != nullptr && sp->getRequiredScience() != SCIENCE_INVALID && t != GUI_COMMAND_PURCHASE_SCIENCE
			&& t != GUI_COMMAND_PLAYER_UPGRADE && t != GUI_COMMAND_OBJECT_UPGRADE && !me->hasScience(sp->getRequiredScience()))
			continue;
		return b;
	}
	return nullptr;
}

/// Does this object's command bar show a button of this type for this thing? Never a line-built wall (the UI sends
/// MSG_DOZER_CONSTRUCT_LINE with an end point for it).
Bool offers(Object *o, const ThingTemplate *what, GUICommandType type)
{
	const CommandButton *b = findButton(o, ThePlayerList->getLocalPlayer(), [&](const CommandButton *c) {
		return c->getCommandType() == type && c->getThingTemplate() != nullptr && c->getThingTemplate()->isEquivalentTo(what); });
	return b != nullptr && !what->isKindOf(KINDOF_LINEBUILD);
}

/// The ControlBar's own verdict on pressing this button for this object (prerequisites, money, sciences, a power's timer, a
/// full queue, a disabled object), as a fixed phrase; NULL when a human could press it now. A pure read: no window is passed,
/// so not even the button's clock overlay is drawn.
const char *unavailable(const CommandButton *b, Object *o)
{
	switch (TheControlBar->getCommandAvailability(b, o, nullptr))
	{
		case COMMAND_AVAILABLE: return nullptr;
		case COMMAND_NOT_READY: return "not ready";
		case COMMAND_CANT_AFFORD: return "already has it or is making it";	// only the upgrade buttons say this
		case COMMAND_ACTIVE: return "already active";
		case COMMAND_HIDDEN: return "button not shown";
		default: return "button disabled";
	}
}

void collectObject(Object *o, void *out)
{
	static_cast<std::vector<Object *> *>(out)->push_back(o);
}

/// The object whose button `match` accepts and a human could press now: `*who` when the trainer named one, else (id 0) the
/// first of the player's objects that qualifies. NULL and *who/*button set, or why not - for a pick, the first reason met.
template <typename Match>
const char *pickButton(const Player *me, Match match, Object **who, const CommandButton **button)
{
	std::vector<Object *> candidates;
	if (*who != nullptr)
		candidates.push_back(*who);
	else
		me->iterateObjects(collectObject, &candidates);
	const char *firstReason = nullptr;
	std::vector<AsciiString> noButton;		///< command sets already seen without the button: one search per set, not per object
	for (size_t i = 0; i < candidates.size(); ++i)
	{
		Object *o = myObject((Int)candidates[i]->getID(), me);
		if (o == nullptr || std::find(noButton.begin(), noButton.end(), o->getCommandSetString()) != noButton.end())
			continue;
		const CommandButton *b = findButton(o, me, match);
		if (b == nullptr)
		{
			noButton.push_back(o->getCommandSetString());
			continue;
		}
		const char *no = unavailable(b, o);
		if (no == nullptr)
		{
			*who = o;
			*button = b;
			return nullptr;
		}
		if (firstReason == nullptr)
			firstReason = no;
	}
	if (firstReason != nullptr)
		return firstReason;
	return (*who != nullptr) ? "no such button" : "none of your objects has that button";
}

/// What a power button waits for after it is pressed - an object, a location, or nothing - as issueSpecialPowerCommand decides.
UnsignedInt targeting(const CommandButton *b)
{
	if (BitIsSet(b->getOptions(), COMMAND_OPTION_NEED_OBJECT_TARGET))
		return COMMAND_OPTION_NEED_OBJECT_TARGET;
	return BitIsSet(b->getOptions(), NEED_TARGET_POS) ? NEED_TARGET_POS : 0;
}

/// A target a click could land on: mine, or a cell CLEAR for me and not an undetected stealther.
Bool clickable(const Object *o, const Player *me)
{
	return o != nullptr && !o->isEffectivelyDead() && !o->isDestroyed() && !o->isContained()
		&& (o->getControllingPlayer() == me || gxOrderIsVisibleTo(o, me));
}

/// The first spot at or around `want`, nearest first, where this player could place it facing `angle`. GXOrderDriver's guard
/// comes first: isLocationLegalToBuild asks Object::getShroudedStatus of every object it examines, so only spots where it
/// examines nothing are tried.
Bool findBuildSpot(const Coord3D &want, const ThingTemplate *what, Real angle, Object *builder, Coord3D *out)
{
	const UnsignedInt flags = BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS
		| BuildAssistant::CLEAR_PATH | BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED
		| BuildAssistant::IGNORE_STEALTHED | BuildAssistant::FAIL_STEALTHED_WITHOUT_FEEDBACK;	// PlaceEventTranslator's
	for (Int ring = 0; ring <= 10; ++ring)
	{
		const Int steps = (ring == 0) ? 1 : 8 * ring;
		for (Int s = 0; s < steps; ++s)
		{
			const Real a = 6.2831853f * (Real)s / (Real)steps;
			Coord3D at;
			if (!onMap(want.x + 40.0f * (Real)ring * cosf(a), want.y + 40.0f * (Real)ring * sinf(a), &at))
				continue;
			if ((!s_soloPlacement && gxOrderAnyObjectNear(at, what))	// the guard only where another peer exists (arm())
				|| TheBuildAssistant->isLocationLegalToBuild(&at, what, angle, flags, builder, nullptr) != LBC_OK)
				continue;
			*out = at;
			return TRUE;
		}
	}
	return FALSE;
}

/// The engine's own reason, as a fixed phrase: a trainer learns more from "missing prerequisite" than from "no".
const char *cannotMake(CanMakeType why)
{
	switch (why)
	{
		case CANMAKE_NO_PREREQ: return "missing prerequisite";
		case CANMAKE_NO_MONEY: return "not enough money";
		case CANMAKE_FACTORY_IS_DISABLED: return "factory disabled";
		case CANMAKE_QUEUE_FULL: return "queue full";
		case CANMAKE_PARKING_PLACES_FULL: return "no parking place";
		case CANMAKE_MAXED_OUT_FOR_PLAYER: return "at the build limit";
		default: return "cannot make it now";
	}
}

void selectGroup(const std::vector<Object *> &group)
{
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	msg->appendBooleanArgument(TRUE);	// a new group, as a click does
	for (size_t i = 0; i < group.size(); ++i)
		msg->appendObjectIDArgument(group[i]->getID());
}

/// NULL when the action was sent; otherwise why it was refused. `at` is set for a build: the spot chosen.
/// GeneralsX: Track B scenario drills (PLAN_neural.md S2): the TRUSTED controller sets up a
/// battle with the existing sandbox cheat orders - MSG_GX_CHEAT_SPAWN_UNIT / MSG_GX_CHEAT_DELETE_OBJECTS, whose
/// handlers (GameLogicDispatch.cpp) refuse them in any match with more than one human. Off unless GX_GYM_SCENARIO=1;
/// the Python side never lets a policy emit these two verbs.
const char *scenario(const GXGymAction &a)
{
	if (!s_cfg.scenario)
		return "scenario verbs need GX_GYM_SCENARIO=1";
	if (!GameLogic::gxCheatsAllowed())
		return "scenario verbs: sandbox only";
	if (a.ids[0] <= 0 || a.ids[0] >= ThePlayerList->getPlayerCount())
		return "no such player";
	// findTemplate looks the name up in a string map: no name key is created (see GXGymObserve.cpp).
	const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(a.tmpl), FALSE);
	if (what == nullptr)
		return "no such template";
	if (a.verb == GXG_CLEAR)
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS);
		msg->appendIntegerArgument(a.ids[0]);
		msg->appendIntegerArgument((Int)what->getTemplateID());
		msg->appendIntegerArgument(0);		// every match of that type (the handler's sentinel)
		return nullptr;
	}
	Coord3D where;
	if (!onMap(a.x, a.y, &where))
		return "off the map";
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_CHEAT_SPAWN_UNIT);
	msg->appendIntegerArgument(a.ids[0]);
	msg->appendIntegerArgument((Int)what->getTemplateID());
	msg->appendLocationArgument(where);
	msg->appendIntegerArgument(a.ids[1]);		// the handler clamps to 1 .. GX_CHEAT_MAX_SPAWN_COUNT
	msg->appendIntegerArgument(a.nIds == 3 ? a.ids[2] - 1 : (Int)LEVEL_REGULAR);	// 1-4 on the wire of GX_ACT
	return nullptr;
}

const char *perform(const GXGymAction &a, const Player *me, Coord3D *at, Bool *placed)
{
	*placed = FALSE;
	if (a.verb == GXG_SPAWN || a.verb == GXG_CLEAR)
		return scenario(a);
	std::vector<Object *> group;
	for (Int i = 0; i < a.nIds; ++i)
	{
		if (a.ids[i] == 0)
			continue;		// power* / upgrade: pick one below (the parser accepts 0 nowhere else)
		Object *o = myObject(a.ids[i], me);
		if (o == nullptr)
			return "not your live unit";
		if (std::find(group.begin(), group.end(), o) == group.end())
			group.push_back(o);		// once each: a click never selects one unit twice (review r1 P2)
	}

	switch (a.verb)
	{
		case GXG_MOVE:
		case GXG_AMOVE:
		case GXG_GUARD:
		{
			for (size_t i = 0; i < group.size(); ++i)
				if (group[i]->getAIUpdateInterface() == nullptr || group[i]->isKindOf(KINDOF_IMMOBILE))
					return "cannot move";
			if (a.verb == GXG_GUARD)
				for (size_t i = 0; i < group.size(); ++i)
				{
					// Guarding is a button (Command_Guard, a position to click), not a right click.
					const CommandButton *b = findButton(group[i], me, [](const CommandButton *c) {
						return c->getCommandType() == GUI_COMMAND_GUARD && BitIsSet(c->getOptions(), NEED_TARGET_POS); });
					if (b == nullptr)
						return "cannot guard";
					const char *no = unavailable(b, group[i]);
					if (no != nullptr)
						return no;
				}
			Coord3D dest;
			if (!onMap(a.x, a.y, &dest))
				return "off the map";
			selectGroup(group);
			if (a.verb == GXG_GUARD)
			{
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_POSITION);
				msg->appendLocationArgument(dest);
				msg->appendIntegerArgument(GUARDMODE_NORMAL);	// the Guard button (GUICommandTranslator.cpp)
				return nullptr;
			}
			TheMessageStream->appendMessage(a.verb == GXG_MOVE ? GameMessage::MSG_DO_MOVETO : GameMessage::MSG_DO_ATTACKMOVETO)
				->appendLocationArgument(dest);
			return nullptr;
		}
		case GXG_ENTER:
		{
			// ActionManager's own test for a click on a container, asked as CMD_FROM_SCRIPT: as CMD_FROM_PLAYER it would call
			// Object::getShroudedStatus (isObjectShroudedForAction), the lazy cache this file must never fill on one peer. The fog
			// half is clickable() instead - a CLEAR cell, stricter than the UI's "not fogged". One reason for every failure, so a
			// refusal tells nothing about a hidden object.
			const Object *box = TheGameLogic->findObjectByID((ObjectID)a.target);
			Bool anyCan = FALSE;
			if (clickable(box, me))
				for (size_t i = 0; i < group.size() && !anyCan; ++i)
					anyCan = TheActionManager->canEnterObject(group[i], box, CMD_FROM_SCRIPT, CHECK_CAPACITY);
			if (!anyCan)
				return "cannot enter that";
			selectGroup(group);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_ENTER);
			msg->appendObjectIDArgument(INVALID_ID);	// "the selection", as CommandXlat's createEnterMessage
			msg->appendObjectIDArgument(box->getID());
			return nullptr;
		}
		case GXG_RALLY:
		{
			if (!group[0]->isKindOf(KINDOF_AUTO_RALLYPOINT))
				return "cannot rally";		// InGameUI's ACTIONTYPE_SET_RALLY_POINT test
			Coord3D dest;
			if (!onMap(a.x, a.y, &dest))
				return "off the map";
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
			msg->appendObjectIDArgument(group[0]->getID());
			msg->appendLocationArgument(dest);
			return nullptr;
		}
		case GXG_POWER:
		case GXG_POWERAT:
		case GXG_POWERON:
		{
			// findSpecialPowerTemplate compares names: no name key is created.
			const SpecialPowerTemplate *sp = TheSpecialPowerStore->findSpecialPowerTemplate(AsciiString(a.tmpl));
			if (sp == nullptr)
				return "no such power";
			// Not offered, because asking whether they may be used would change this peer alone (review ai-gym-2 r1 P2-1):
			// battle plans (getCommandAvailability mints name keys: NAMEKEY + findUpdateModule), the vehicle disguise
			// (canDoSpecialPowerAtObject, the same), a sneak attack (canDoSpecialPowerAtLocation -> isLocationLegalToBuild ->
			// Object::getShroudedStatus).
			const SpecialPowerType spType = sp->getSpecialPowerType();
			if (spType == SPECIAL_CHANGE_BATTLE_PLANS || spType == SPECIAL_DISGUISE_AS_VEHICLE || spType == SPECIAL_SNEAK_ATTACK)
				return "power not offered to a bot";
			Object *src = group.empty() ? nullptr : group[0];
			const CommandButton *b = nullptr;
			const char *why = pickButton(me, [&](const CommandButton *c) {
				return c->getCommandType() == GUI_COMMAND_SPECIAL_POWER && c->getSpecialPowerTemplate() == sp; }, &src, &b);
			if (why != nullptr)
				return why;
			const UnsignedInt want = (a.verb == GXG_POWERON) ? COMMAND_OPTION_NEED_OBJECT_TARGET
				: (a.verb == GXG_POWERAT) ? NEED_TARGET_POS : 0;
			const UnsignedInt has = targeting(b);
			if (has != want)
				return has == COMMAND_OPTION_NEED_OBJECT_TARGET ? "that power needs a target object (poweron)"
					: has == NEED_TARGET_POS ? "that power needs a location (powerat)" : "that power takes no target (power)";
			// Pressed with the source selected, as CommandXlat's issueSpecialPowerCommand: no specific source in the message.
			GameMessage *msg = nullptr;
			if (a.verb == GXG_POWER)
			{
				selectGroup(std::vector<Object *>(1, src));
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
				msg->appendIntegerArgument(sp->getID());
			}
			else if (a.verb == GXG_POWERAT)
			{
				Coord3D dest;
				if (!onMap(a.x, a.y, &dest))
					return "off the map";
				// ActionManager's test, as CommandXlat asks it before a location power is sent (and as the simulation asks it
				// again): no paradrop into water, no damaging power into a SHROUDED cell (a cell read, not the object cache).
				if (!TheActionManager->canDoSpecialPowerAtLocation(src, &dest, CMD_FROM_PLAYER, sp, nullptr, b->getOptions()))
					return "cannot use it there";
				selectGroup(std::vector<Object *>(1, src));
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
				msg->appendIntegerArgument(sp->getID());
				msg->appendLocationArgument(dest);
				msg->appendRealArgument(INVALID_ANGLE);
				msg->appendObjectIDArgument(INVALID_ID);	// no object under the cursor
			}
			else
			{
				// ActionManager's per-power rules as CMD_FROM_SCRIPT: as CMD_FROM_PLAYER its isObjectShroudedForAction would call
				// Object::getShroudedStatus. The fog half is clickable(). One reason for every failure: nothing is learned
				// about a hidden object.
				const Object *target = TheGameLogic->findObjectByID((ObjectID)a.target);
				if (!clickable(target, me) || !b->isValidObjectTarget(src, target)
					|| !TheActionManager->canDoSpecialPowerAtObject(src, target, CMD_FROM_SCRIPT, sp, b->getOptions()))
					return "not a valid visible target";
				selectGroup(std::vector<Object *>(1, src));
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
				msg->appendIntegerArgument(sp->getID());
				msg->appendObjectIDArgument(target->getID());
			}
			msg->appendIntegerArgument(b->getOptions());
			msg->appendObjectIDArgument(INVALID_ID);
			return nullptr;
		}
		case GXG_UPGRADE:
		{
			const UpgradeTemplate *u = nullptr;	// a walk of the list by name: no name key is created (findUpgrade would)
			for (const UpgradeTemplate *t = TheUpgradeCenter->firstUpgradeTemplate(); t != nullptr && u == nullptr;
				t = t->friend_getNext())
				if (t->getUpgradeName().compare(a.tmpl) == 0)
					u = t;
			if (u == nullptr)
				return "no such upgrade";
			if (!TheUpgradeCenter->canAffordUpgrade(ThePlayerList->getLocalPlayer(), u, FALSE))
				return "not enough money";
			Object *producer = group.empty() ? nullptr : group[0];
			const CommandButton *b = nullptr;
			const char *why = pickButton(me, [&](const CommandButton *c) {
				return (c->getCommandType() == GUI_COMMAND_PLAYER_UPGRADE || c->getCommandType() == GUI_COMMAND_OBJECT_UPGRADE)
					&& c->getUpgradeTemplate() == u; }, &producer, &b);
			if (why != nullptr)
				return why;
			selectGroup(std::vector<Object *>(1, producer));	// onQueueUpgrade queues on the selection
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
			msg->appendObjectIDArgument(producer->getID());
			msg->appendIntegerArgument((Int)u->getWireId());	// GeneralsX: the name's id, not the NameKey (GXWireId.h)
			return nullptr;
		}
		case GXG_EVACUATE:
		case GXG_SELL:
		{
			const GUICommandType type = (a.verb == GXG_SELL) ? GUI_COMMAND_SELL : GUI_COMMAND_EVACUATE;
			for (size_t i = 0; i < group.size(); ++i)
			{
				// The plain button only: an Evacuate that wants a spot to drop at is not offered here.
				const CommandButton *b = findButton(group[i], me, [&](const CommandButton *c) {
					return c->getCommandType() == type && !BitIsSet(c->getOptions(), NEED_TARGET_POS); });
				if (b == nullptr)
					return (a.verb == GXG_SELL) ? "cannot sell" : "cannot evacuate";
				const char *no = unavailable(b, group[i]);
				if (no != nullptr)
					return no;
			}
			selectGroup(group);
			TheMessageStream->appendMessage(a.verb == GXG_SELL ? GameMessage::MSG_SELL : GameMessage::MSG_EVACUATE);
			return nullptr;
		}
		case GXG_EXIT:
		{
			// One passenger out, as its inventory button on the container's bar does (doTransportInventoryUI): only a container
			// that shows its contents has such buttons. myObject() refuses a contained unit, so the passenger is checked here.
			Object *box = group[0];
			const Object *who = TheGameLogic->findObjectByID((ObjectID)a.target);
			if (who == nullptr || who->getControllingPlayer() != me || who->getContainedBy() != box || box->getContain() == nullptr
				|| !box->getContain()->isDisplayedOnControlBar())
				return "not your passenger there";
			if (box->isDisabledByType(DISABLED_SUBDUED))
				return "button disabled";
			selectGroup(group);
			TheMessageStream->appendMessage(GameMessage::MSG_EXIT)->appendObjectIDArgument(who->getID());
			return nullptr;
		}
		case GXG_SCIENCE:
		{
			Player *p = ThePlayerList->getLocalPlayer();
			ScienceVec can, later;
			TheScienceStore->getPurchasableSciences(p, can, later);
			for (size_t i = 0; i < can.size(); ++i)
			{
				if (TheScienceStore->getInternalNameForScience(can[i]).compare(a.tmpl) != 0)
					continue;		// keyToName: no name key is created
				if (TheScienceStore->getSciencePurchaseCost(can[i]) > p->getSciencePurchasePoints())
					return "not enough science points";
				TheMessageStream->appendMessage(GameMessage::MSG_PURCHASE_SCIENCE)->appendIntegerArgument(
					(Int)TheScienceStore->getWireIdForScience(can[i]));	// GeneralsX: the name's id, not the NameKey (GXWireId.h)
				return nullptr;
			}
			return "cannot buy it now";
		}
		case GXG_ATTACK:
		{
			// ONE reason for "no such object", "not an enemy" and "not visible": separate ones would let a bot probe ids
			// and learn that a hidden enemy exists - a fog leak through the refusal text.
			const Object *target = TheGameLogic->findObjectByID((ObjectID)a.target);
			if (target == nullptr || !gxOrderIsEnemyTarget(target, me) || !gxOrderIsVisibleTo(target, me))
				return "not a visible enemy";
			for (size_t i = 0; i < group.size(); ++i)
				if (!group[i]->isKindOf(KINDOF_CAN_ATTACK))
					return "cannot attack";
			selectGroup(group);
			TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACK_OBJECT)->appendObjectIDArgument(target->getID());
			return nullptr;
		}
		case GXG_STOP:
			selectGroup(group);
			TheMessageStream->appendMessage(GameMessage::MSG_DO_STOP);
			return nullptr;
		case GXG_BUILD:
		{
			Object *dozer = group[0];
			// findTemplate looks the name up in a string map: no name key is created (see GXGymObserve.cpp).
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(a.tmpl), FALSE);
			if (what == nullptr)
				return "no such template";
			if (!dozer->isKindOf(KINDOF_DOZER) || !offers(dozer, what, GUI_COMMAND_DOZER_CONSTRUCT))
				return "builder has no such button";
			const CanMakeType can = TheBuildAssistant->canMakeUnit(dozer, what);
			if (can != CANMAKE_OK)
				return cannotMake(can);
			Coord3D want;
			if (!onMap(a.x, a.y, &want))
				return "off the map";
			// GeneralsX: ai-train-8: a facing from the bot (wh1ter0se's live watch #15), else the template's -
			// what a player who clicks without dragging gets (InGameUI). It travels in the message, so every peer builds the same.
			const Real angle = a.hasAngle ? (Real)a.angleDeg * (PI / 180.0f) : what->getPlacementViewAngle();
			if (!findBuildSpot(want, what, angle, dozer, at))
				return "no legal spot near";
			selectGroup(group);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
			msg->appendIntegerArgument(what->getTemplateID());
			msg->appendLocationArgument(*at);
			msg->appendRealArgument(angle);
			*placed = TRUE;
			return nullptr;
		}
		case GXG_TRAIN:
		{
			Object *factory = group[0];
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(a.tmpl), FALSE);
			if (what == nullptr)
				return "no such template";
			ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
			if (pu == nullptr || !offers(factory, what, GUI_COMMAND_UNIT_BUILD))
				return "factory has no such button";
			const CanMakeType can = TheBuildAssistant->canMakeUnit(factory, what);
			if (can != CANMAKE_OK)
				return cannotMake(can);
			selectGroup(group);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
			msg->appendIntegerArgument(what->getTemplateID());
			msg->appendIntegerArgument(0);	// as the ControlBar: dispatch allocates the production ID on every peer (sweep c066-04)
			return nullptr;
		}
		default:
			return "not an order";
	}
}

/// One line from the trainer. Returns TRUE for a "step" line.
Bool handleLine(const char *line, UnsignedInt frame)
{
	++s_lineNo;
	if (line[0] == '#' || line[strspn(line, " \t\r\n")] == '\0')
		return FALSE;		// a comment or a blank line is not an action, and not numbered as a refusal either
	GXGymAction a;
	const char *why = "";
	if (!gxGymParseAction(line, &a, &why))
	{
		writeRej(frame, why);
		return FALSE;
	}
	if (a.verb == GXG_STEP)
		return TRUE;
	if (!s_acting)
	{
		writeRej(frame, "acting is off");
		return FALSE;
	}
	const Player *me = ThePlayerList->getLocalPlayer();
	if (me == nullptr || !me->isPlayableSide() || !me->isPlayerActive() || TheControlBar == nullptr
		|| TheActionManager == nullptr || TheBuildAssistant == nullptr || TheSpecialPowerStore == nullptr
		|| TheUpgradeCenter == nullptr || TheScienceStore == nullptr || TheThingFactory == nullptr)
	{
		writeRej(frame, "no player to act for");
		return FALSE;
	}
	if (s_budget < 1.0f)
	{
		writeRej(frame, "over GX_ACT_APM");
		return FALSE;
	}
	s_budget -= 1.0f;		// before the checks: a refusal costs the same as an order (review r1 P2)
	Coord3D at;
	Bool placed = FALSE;
	const char *refused = perform(a, me, &at, &placed);
	if (refused != nullptr)
	{
		writeRej(frame, refused);
		return FALSE;
	}
	++s_sent;
	writeAck(frame, placed ? &at : nullptr);
	return FALSE;
}

/// Every complete line the trainer has written so far, at most MAX_LINES_PER_UPDATE of them. A line still being written stays
/// pending. Read byte by byte: a line longer than MAX_LINE is refused WHOLE at its end (its tail is never run as a line of its
/// own), and a NUL byte refuses its line (review r1 P3). TRUE when one was "step".
Bool drain(UnsignedInt frame)
{
	if (s_act == nullptr)
		return FALSE;
	Bool stepped = FALSE;
	Int lines = 0, bytes = 0;
	clearerr(s_act);	// the trainer may have appended since we last met the end of the file
	Int c;
	// Bytes are capped too: a file with no newline would otherwise be read whole in one call (review r2 P3).
	while (!stepped && lines < MAX_LINES_PER_UPDATE && bytes++ < MAX_LINES_PER_UPDATE * (MAX_LINE + 2)
		&& (c = fgetc(s_act)) != EOF)
	{
		if (c != '\n')
		{
			if (c == '\0')
				s_hasNul = TRUE;
			else if ((Int)s_pending.size() < MAX_LINE)
				s_pending += (char)c;
			else
				s_overlong = TRUE;
			continue;
		}
		++lines;
		if (s_overlong || s_hasNul)
		{
			++s_lineNo;
			if (s_budget >= 1.0f)
				s_budget -= 1.0f;
			writeRej(frame, s_overlong ? "line too long" : "NUL byte in line");
		}
		else
			stepped = handleLine(s_pending.c_str(), frame);
		s_pending.clear();
		s_overlong = s_hasNul = FALSE;
	}
	if (s_obs != nullptr)
		fflush(s_obs);
	return stepped;
}

Int resolveView()
{
	if (s_cfg.view == "all")
		return -1;
	const Player *local = ThePlayerList->getLocalPlayer();
	if (s_cfg.view == "local")
		return (local != nullptr && local->isPlayableSide()) ? (Int)local->getPlayerIndex() : -1;	// an observer sees all
	const Int i = atoi(s_cfg.view.str());
	return (i >= 0 && i < ThePlayerList->getPlayerCount()) ? i : -1;
}

void disarm()
{
	if (s_obs != nullptr)
	{
		fprintf(s_obs, "%s{\"v\":%d,\"t\":\"end\",\"f\":%u,\"sent\":%d,\"refused\":%d}\n", s_prefix, (Int)GXGYM_SCHEMA_VERSION,
			(unsigned)s_lastFrame, s_sent, s_refused);
		fflush(s_obs);
		if (s_obs != stderr)
			fclose(s_obs);
	}
	if (s_act != nullptr)
		fclose(s_act);
	say("match over at frame %u: %d action(s) sent, %d refused", (unsigned)s_lastFrame, s_sent, s_refused);
	s_obs = nullptr;
	s_act = nullptr;
	s_armed = FALSE;
}

void arm()
{
	s_armed = TRUE;
	s_pending.clear();
	s_overlong = s_hasNul = FALSE;
	s_lineNo = 0;
	s_lastObs = 0xffffffffu;
	s_lastFrame = TheGameLogic->getFrame();
	s_budget = APM_BURST;
	s_sent = s_refused = 0;

	// Where it may run, decided once the match exists (a replay is known to be one only now): a replay, or a command-line harness
	// match with at most one human - the bot's own seat - unless a test says otherwise. Refused for the rest of the process.
	const char *refusal = nullptr;
	if (!inPlayback() && (TheGlobalData == nullptr || TheGlobalData->m_lanRole == LANROLE_NONE))
		refusal = "not a harness match or a replay (GX_OBS / GX_ACT need a -lanhost, -lanjoin or -replay start)";
	else if (!inPlayback() && !GameLogic::gxCheatsAllowed() && !s_cfg.allowHumans)
		refusal = "this match has more than one human (GX_GYM_ALLOW_HUMANS=1 is for tests)";
	if (refusal != nullptr)
	{
		say("refused: %s", refusal);
		s_on = FALSE;
		s_armed = FALSE;
		return;
	}

	s_soloPlacement = gxGymSoloPlacement(GameLogic::gxCheatsAllowed() ? true : false, inPlayback() ? true : false);
	say("build spots: %s", s_soloPlacement ? "solo match - the engine's own legality check"
		: "more than one peer or a replay - guarded search (no object near the footprint)");

	if (s_cfg.obsOut.isEmpty())
	{
		s_obs = stderr;
		s_prefix = "[GXOBS] ";
	}
	else
	{
		s_obs = fopen(s_cfg.obsOut.str(), "w");
		s_prefix = "";
		if (s_obs == nullptr)
			say("cannot write GX_OBS_OUT '%s' - no observation lines this match", s_cfg.obsOut.str());
	}
	s_viewIndex = resolveView();

	s_acting = FALSE;
	if (s_cfg.actPath.isNotEmpty())
	{
		const Player *me = ThePlayerList->getLocalPlayer();
		if (inPlayback())
			say("not acting: a replay's orders are the recording's");
		else if (me == nullptr || !me->isPlayableSide())
			say("not acting: this peer has no player of its own (an observer seat)");
		else if ((s_act = fopen(s_cfg.actPath.str(), "r")) == nullptr)
			say("not acting: cannot read GX_ACT '%s'", s_cfg.actPath.str());
		else if (s_cfg.sync && s_obs == nullptr)
		{
			say("not acting: GX_ACT_SYNC needs the observation lines it steps on, and GX_OBS_OUT cannot be written");
			fclose(s_act);
			s_act = nullptr;
		}
		else
			s_acting = TRUE;
	}

	gxGymObserveReset();
	gxGymObserveStart(s_obs, s_prefix, s_viewIndex);
	say("on: obs every %d frame(s) to %s, view %d, acting %s%s for player %d, %.0f apm", s_cfg.obsPeriod,
		s_cfg.obsOut.isEmpty() ? "stderr" : s_cfg.obsOut.str(), s_viewIndex, s_acting ? "on" : "off",
		s_cfg.sync ? " (sync)" : "", ThePlayerList->getLocalPlayer() ? (Int)ThePlayerList->getLocalPlayer()->getPlayerIndex() : -1,
		s_cfg.apm);
}

} // namespace

void gxGymUpdate()
{
	if (!s_configRead)
	{
		s_configRead = TRUE;
		s_on = readConfig();
	}
	if (!s_on)
		return;

	const Bool inGame = TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame()
		&& !TheGameLogic->isLoadingMap() && ThePlayerList != nullptr;
	if (!inGame)
	{
		if (s_armed)
			disarm();
		return;
	}
	if (!s_armed)
	{
		arm();
		if (!s_on)
			return;
	}

	UnsignedInt frame = TheGameLogic->getFrame();
	if (frame + 1 < s_lastFrame)
	{
		disarm();		// the frame went back: a new match without a pass through the shell (review r1 P3)
		arm();
		if (!s_on)
			return;
		frame = TheGameLogic->getFrame();
	}
	if (frame > s_lastFrame)
	{
		s_budget += (Real)(frame - s_lastFrame) * s_cfg.apm / (60.0f * (Real)LOGICFRAMES_PER_SECOND);
		if (s_budget > APM_BURST)
			s_budget = APM_BURST;
	}
	s_lastFrame = frame;

	const Bool obsDue = s_cfg.obsPeriod > 0 && frame % (UnsignedInt)s_cfg.obsPeriod == 0 && frame != s_lastObs;
	if (obsDue)
	{
		s_lastObs = frame;
		gxGymObserveFrame(s_obs, s_prefix, frame, s_viewIndex);
		if (s_cfg.obsExt)
			gxGymObserveExtFrame(s_obs, s_prefix, frame);
	}

	if (!s_cfg.sync || !s_acting)
	{
		drain(frame);
		return;
	}
	if (!obsDue)
		return;		// lock-step: actions are read only right after an observation, until the trainer says "step"

	const UnsignedInt start = timeGetTime();
	while (!drain(frame))
	{
		if (timeGetTime() - start > s_cfg.timeoutMs)
		{
			say("no \"step\" from the trainer within %u ms at frame %u - acting stops, the match plays on", s_cfg.timeoutMs,
				(unsigned)frame);
			s_acting = FALSE;
			fclose(s_act);
			s_act = nullptr;
			return;
		}
		Sleep(1);
	}
}
