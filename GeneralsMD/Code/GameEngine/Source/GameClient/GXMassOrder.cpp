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

// GeneralsX: GX_MASSORDER - one player's mass order, scripted and timed (ROADMAP_2027 H03).
//
// WHY. H03 spreads the path work of a big group order over several ticks. Its gate needs the same order, at the same
// frame, with the same army, in both arms: "on matched 100- and 400-unit mass orders, the first and the LAST unit's
// order-to-movement delay" (ROADMAP_2027 M2), plus the cost of the tick that executes it. Nothing in a normal match
// gives that: AI orders are per team, and GX_STRESS orders each unit on its own (aiAttackMoveToPosition), never as a
// selected group through AIGroup.
//
// WHAT. At GX_MASSORDER_SPAWN the local player gets <n> units of one template through the sandbox's own spawn cheat
// (MSG_GX_CHEAT_SPAWN_UNIT, at most 50 per message - GameLogicDispatch.cpp GX_CHEAT_MAX_SPAWN_COUNT - so 400 is eight
// messages), around the centroid of its structures. From GX_MASSORDER_AT on, every GX_MASSORDER_EVERY frames, it
// selects every live unit of that template it owns (ascending ObjectID, at most <n>) and sends ONE group order - the
// MSG_CREATE_SELECTED_GROUP + MSG_DO_MOVETO (or MSG_DO_ATTACKMOVETO) pair a box-select and a right-click send - to a
// point GX_MASSORDER_DIST away: towards the map centre on even orders, back to the first centroid on odd ones.
// Then it watches each unit's logic position and prints, once every unit moved (or 600 frames passed):
//   [GXMASS] order=<k> n=<units> kind=<move|attack> f_issue=<frame> ra=<run-ahead> moved=<m> first=<+f> p50=<+f>
//            p90=<+f> last=<+f>
// the frames from the logic frame the order was issued at to each unit's first displacement (> 1 world unit). The
// tick that executed the order is f_issue + ra; its cost is the [GXTICK] line of that frame (GX_TICKMS=1).
//
//   GX_MASSORDER=<n>[:<template>]   1..5000 units; default template AmericaTankCrusader. Unset or 0 = off and inert.
//   GX_MASSORDER_SPAWN=<f>|none     default 300        GX_MASSORDER_AT=<f>     default SPAWN + 300 (none: 600)
//                                   none: no spawn - the army is already there (GX_STRESS / GX_STRESS_UNIT, which a
//                                   two-human match can have and the one-human spawn cheat cannot)
//   GX_MASSORDER_EVERY=<f>          default 1200       GX_MASSORDER_COUNT=<k>  default 6
//   GX_MASSORDER_DIST=<d>           default 600        GX_MASSORDER_KIND=move|attack  default move
//   GX_MASSORDER_SELECT=chunks|ui|team   default chunks: ids in messages of <= 254 (see issue()). ui: ONE message
//                                   holding every id, exactly what a box-select (SelectionXlat) or a select-across-map
//                                   (InGameUI) sends - the shape a player's own order has. team: ONE MSG_CREATE_TEAM1
//                                   with every id (Ctrl+1, SelectionTranslator::onMetaCreateTeam), then MSG_SELECT_TEAM1
//                                   (pressing 1), then the order.
//   GX_MASSORDER_WATCH=<f>          default 600: frames each order is watched (EVERY must be >= it)
//   GX_MASSORDER_TL=<f>             default 0 = off: every <f> frames of the watch, one "[GXMASS] tl" timeline line
//
// GeneralsX: (mass-order-breaks) Besides the delay line, each order now ends with
//   [GXMASS] end order=<k> n=<units> recv=<r> moved=<m> idle=<i> wait=<w> stuck=<s> d0=<p50 start distance>
//            d50=<p50> d90=<p90> dmax=<max end distance> near=<units within 300 of the target>
//            away=<units that ended further from the target than they started, by more than 50>
// recv = units that took the order (their AI goal changed, or they were idle and no longer are, two frames after the
// order executed), wait = units still waiting for a path at the end. Reads only: AIUpdateInterface::isIdle/isMoving/
// isWaitingForPath/isBlockedAndStuck/getGoalPosition, all plain field reads with no lazy state.
//
// HONEST. Every order is a message the UI or the Debug menu sends, crossing the network like a human's; nothing here
// calls into GameLogic to act. The only reads are object positions, templates and owners - never
// Object::getShroudedStatus or any lazy cache the simulation reads back (GXOrderDriver.cpp isVisibleTo). No random
// numbers. GameClient/ is outside the SimID digest. It runs only in a command-line harness match (-lanhost/-lanjoin) and
// the spawn cheat only in a one-human sandbox (GameLogic::gxCheatsAllowed); anywhere else one "[GXMASS] refused" line.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXMassOrder.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "Common/GameCommon.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameNetwork/NetworkInterface.h"

namespace
{

const Int SPAWN_PER_MESSAGE = 50;	// GameLogicDispatch.cpp GX_CHEAT_MAX_SPAWN_COUNT
const UnsignedInt WATCH_FRAMES = 600;	// the default and the floor of GX_MASSORDER_EVERY
const Real NEAR_DIST = 300.0f;
const Real MAP_MARGIN = 60.0f;

struct Config
{
	Bool on = FALSE;
	Int units = 0;
	const ThingTemplate *tmpl = nullptr;
	UnsignedInt spawn = 300, at = 600, every = 1200;
	Int count = 6;
	Real dist = 600.0f;
	Bool attack = FALSE;
	Bool selectUi = FALSE;
	Bool selectTeam = FALSE;
	Bool noSpawn = FALSE;
	UnsignedInt watch = WATCH_FRAMES, tl = 0;
};

Config s_cfg;
Bool s_configRead = FALSE;
Bool s_spawned = FALSE;
Int s_issued = 0;
UnsignedInt s_lastFrame = 0;
Coord3D s_home;		// the first order's centroid: odd orders return here
struct Watch { ObjectID id; Coord3D from; UnsignedInt moved; Bool wasIdle; Coord3D goal; Bool received; };
std::vector<Watch> s_watch;
UnsignedInt s_issueFrame = 0;
Int s_issueRunAhead = -1;
Coord3D s_to;
Bool s_recvDone = FALSE, s_delayDone = FALSE;

void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[GXMASS] %s\n", line);
	fflush(stderr);
}

Bool envFrame(const char *name, UnsignedInt dflt, UnsignedInt *out)
{
	const char *v = getenv(name);
	*out = dflt;
	if (v == nullptr || *v == '\0')
		return TRUE;
	char *end = nullptr;
	const long n = strtol(v, &end, 10);
	if (end == v || *end != '\0' || n < 1 || n > 100000000)
		return FALSE;
	*out = (UnsignedInt)n;
	return TRUE;
}

Bool readConfig()
{
	const char *v = getenv("GX_MASSORDER");
	if (v == nullptr || *v == '\0' || strcmp(v, "0") == 0)
		return FALSE;
	char name[128] = "AmericaTankCrusader";
	int n = 0;
	const char *colon = strchr(v, ':');
	if (sscanf(v, "%d", &n) != 1 || n < 1 || n > 5000
		|| (colon != nullptr && (strlen(colon + 1) == 0 || strlen(colon + 1) >= sizeof(name))))
	{
		say("refused: GX_MASSORDER='%s' is not <units 1..5000>[:<template>]", v);
		return FALSE;
	}
	if (colon != nullptr)
		strcpy(name, colon + 1);
	if (TheGlobalData == nullptr || TheGlobalData->m_lanRole == LANROLE_NONE)
	{
		say("refused: not a harness match (GX_MASSORDER needs a -lanhost or -lanjoin start)");
		return FALSE;
	}
	s_cfg.tmpl = (TheThingFactory != nullptr) ? TheThingFactory->findTemplate(AsciiString(name), FALSE) : nullptr;
	if (s_cfg.tmpl == nullptr)
	{
		say("refused: no unit template '%s'", name);
		return FALSE;
	}
	s_cfg.units = n;
	UnsignedInt every = 0, count = 0, dist = 0, watchFrames = 0, tl = 0;
	const char *kind = getenv("GX_MASSORDER_KIND");
	const char *sel = getenv("GX_MASSORDER_SELECT");
	const char *tlv = getenv("GX_MASSORDER_TL");
	const char *sp = getenv("GX_MASSORDER_SPAWN");
	s_cfg.noSpawn = sp != nullptr && strcmp(sp, "none") == 0;
	if (s_cfg.noSpawn)
		s_cfg.spawn = 0;
	if ((!s_cfg.noSpawn && !envFrame("GX_MASSORDER_SPAWN", 300, &s_cfg.spawn))
		|| !envFrame("GX_MASSORDER_AT", s_cfg.noSpawn ? 600 : s_cfg.spawn + 300, &s_cfg.at)
		|| !envFrame("GX_MASSORDER_EVERY", 1200, &every) || !envFrame("GX_MASSORDER_COUNT", 6, &count)
		|| !envFrame("GX_MASSORDER_DIST", 600, &dist) || !envFrame("GX_MASSORDER_WATCH", WATCH_FRAMES, &watchFrames)
		|| !(tlv == nullptr || strcmp(tlv, "0") == 0 || envFrame("GX_MASSORDER_TL", 0, &tl))
		|| s_cfg.at <= s_cfg.spawn || every < watchFrames
		|| (kind != nullptr && strcmp(kind, "move") != 0 && strcmp(kind, "attack") != 0)
		|| (sel != nullptr && strcmp(sel, "chunks") != 0 && strcmp(sel, "ui") != 0 && strcmp(sel, "team") != 0))
	{
		say("refused: GX_MASSORDER_SPAWN/AT/EVERY/COUNT/DIST/KIND/SELECT/WATCH/TL not understood (AT > SPAWN, EVERY >= WATCH,"
			" KIND move|attack, SELECT chunks|ui|team)");
		return FALSE;
	}
	s_cfg.selectUi = sel != nullptr && strcmp(sel, "ui") == 0;
	s_cfg.selectTeam = sel != nullptr && strcmp(sel, "team") == 0;
	s_cfg.watch = watchFrames;
	s_cfg.tl = tl;
	s_cfg.every = every;
	s_cfg.count = (Int)count;
	s_cfg.dist = (Real)dist;
	s_cfg.attack = kind != nullptr && strcmp(kind, "attack") == 0;
	say("on: units=%d template=%s spawn=%s%u at=%u every=%u count=%d dist=%.0f kind=%s select=%s watch=%u tl=%u", s_cfg.units,
		name, s_cfg.noSpawn ? "none/" : "", s_cfg.spawn, s_cfg.at, s_cfg.every, s_cfg.count, s_cfg.dist, s_cfg.attack ? "attack" : "move",
		s_cfg.selectTeam ? "team" : s_cfg.selectUi ? "ui" : "chunks", s_cfg.watch, s_cfg.tl);
	return TRUE;
}

void clampToMap(Coord3D *p)
{
	Region3D ext;
	TheTerrainLogic->getExtent(&ext);
	p->x = std::min(std::max(p->x, ext.lo.x + MAP_MARGIN), ext.hi.x - MAP_MARGIN);
	p->y = std::min(std::max(p->y, ext.lo.y + MAP_MARGIN), ext.hi.y - MAP_MARGIN);
	p->z = TheTerrainLogic->getGroundHeight(p->x, p->y);
}

Bool structureCentroid(const Player *me, Coord3D *out)
{
	Int n = 0;
	out->x = out->y = out->z = 0.0f;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (o->getControllingPlayer() != me || !o->isKindOf(KINDOF_STRUCTURE) || o->isEffectivelyDead())
			continue;
		out->x += o->getPosition()->x;
		out->y += o->getPosition()->y;
		++n;
	}
	if (n == 0)
		return FALSE;
	out->x /= (Real)n;
	out->y /= (Real)n;
	return TRUE;
}

void spawn(const Player *me)
{
	Coord3D at;
	if (!structureCentroid(me, &at))
	{
		say("spawn skipped: the local player owns no structure");
		return;
	}
	clampToMap(&at);
	for (Int left = s_cfg.units; left > 0; left -= SPAWN_PER_MESSAGE)
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_GX_CHEAT_SPAWN_UNIT);
		msg->appendIntegerArgument(me->getPlayerIndex());
		msg->appendIntegerArgument((Int)s_cfg.tmpl->getTemplateID());
		msg->appendLocationArgument(at);
		msg->appendIntegerArgument(std::min(left, SPAWN_PER_MESSAGE));
		msg->appendIntegerArgument((Int)LEVEL_REGULAR);
	}
	say("spawn f=%u units=%d at=%.0f,%.0f", TheGameLogic->getFrame(), s_cfg.units, at.x, at.y);
}

void issue(const Player *me)
{
	std::vector<Object *> group;
	for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
	{
		if (o->getControllingPlayer() == me && o->getTemplate()->isEquivalentTo(s_cfg.tmpl) && !o->isEffectivelyDead()
			&& !o->isContained())
			group.push_back(o);
	}
	std::sort(group.begin(), group.end(), [](const Object *a, const Object *b) { return a->getID() < b->getID(); });
	if ((Int)group.size() > s_cfg.units)
		group.resize((size_t)s_cfg.units);
	if (group.empty())
	{
		say("order=%d skipped: no live unit of the template", s_issued);
		++s_issued;
		return;
	}
	Coord3D c = { 0.0f, 0.0f, 0.0f };
	for (size_t i = 0; i < group.size(); ++i)
	{
		c.x += group[i]->getPosition()->x;
		c.y += group[i]->getPosition()->y;
	}
	c.x /= (Real)group.size();
	c.y /= (Real)group.size();
	if (s_issued == 0)
		s_home = c;
	Coord3D to = s_home;
	if (s_issued % 2 == 0)
	{
		Region3D ext;
		TheTerrainLogic->getExtent(&ext);
		const Real mx = (ext.lo.x + ext.hi.x) * 0.5f - c.x, my = (ext.lo.y + ext.hi.y) * 0.5f - c.y;
		const Real len = sqrtf(mx * mx + my * my);
		to.x = c.x + (len > 1.0f ? mx / len : 1.0f) * s_cfg.dist;
		to.y = c.y + (len > 1.0f ? my / len : 0.0f) * s_cfg.dist;
	}
	clampToMap(&to);

	// A GameMessage carries at most 255 arguments: getArgumentCount() is an UnsignedByte (MessageStream.h), so one
	// message with 400 ids reported 401 % 256 = 145 and selected only the first 144. Chunks of 254 ids: the first
	// makes a new group, as a box-select does, the rest add to it. GX_MASSORDER_SELECT=ui sends the one big message a
	// player's box-select sends instead.
	const size_t kIdsPerMessage = s_cfg.selectUi ? group.size() : 254;
	Int selMessages = 0;
	if (s_cfg.selectTeam)
	{
		GameMessage *team = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_TEAM1);
		for (size_t i = 0; i < group.size(); ++i)
			team->appendObjectIDArgument(group[i]->getID());
		TheMessageStream->appendMessage(GameMessage::MSG_SELECT_TEAM1);
		selMessages = 2;
	}
	for (size_t start = 0; !s_cfg.selectTeam && start < group.size(); start += kIdsPerMessage, ++selMessages)
	{
		GameMessage *sel = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
		sel->appendBooleanArgument(start == 0 ? TRUE : FALSE);
		for (size_t i = start; i < group.size() && i < start + kIdsPerMessage; ++i)
			sel->appendObjectIDArgument(group[i]->getID());
	}
	TheMessageStream->appendMessage(s_cfg.attack ? GameMessage::MSG_DO_ATTACKMOVETO : GameMessage::MSG_DO_MOVETO)
		->appendLocationArgument(to);

	s_watch.clear();
	for (size_t i = 0; i < group.size(); ++i)
	{
		const AIUpdateInterface *ai = group[i]->getAI();
		const Coord3D *g = ai ? ai->getGoalPosition() : nullptr;
		s_watch.push_back(Watch{ group[i]->getID(), *group[i]->getPosition(), 0, (Bool)(ai == nullptr || ai->isIdle()),
			g ? *g : Coord3D{ 0.0f, 0.0f, 0.0f }, FALSE });
	}
	s_to = to;
	s_recvDone = s_delayDone = FALSE;
	s_issueFrame = TheGameLogic->getFrame();
	s_issueRunAhead = (TheNetwork != nullptr) ? (Int)TheNetwork->getRunAhead() : -1;
	say("issue order=%d f=%u n=%d sel_msgs=%d from=%.0f,%.0f to=%.0f,%.0f", s_issued, s_issueFrame, (Int)group.size(), selMessages, c.x, c.y, to.x, to.y);
	++s_issued;
}

UnsignedInt pct(std::vector<Real> &v, Int num, Int den)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	size_t i = (v.size() * (size_t)num) / (size_t)den;
	return (UnsignedInt)v[i < v.size() ? i : v.size() - 1];
}

Real dist2d(const Coord3D &a, const Coord3D &b)
{
	return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

void watch(UnsignedInt frame)
{
	const UnsignedInt executed = s_issueFrame + (UnsignedInt)(s_issueRunAhead > 0 ? s_issueRunAhead : 0);
	const Bool checkRecv = !s_recvDone && frame >= executed + 2;
	Int moved = 0, recv = 0, idle = 0, wait = 0, stuck = 0, moving = 0, nearTo = 0, away = 0;
	std::vector<Real> d0, d;
	for (size_t i = 0; i < s_watch.size(); ++i)
	{
		Watch &w = s_watch[i];
		const Object *o = TheGameLogic->findObjectByID(w.id);
		const AIUpdateInterface *ai = o ? o->getAI() : nullptr;
		if (o != nullptr)
		{
			const Real dx = o->getPosition()->x - w.from.x, dy = o->getPosition()->y - w.from.y;
			if (w.moved == 0 && dx * dx + dy * dy > 1.0f)
				w.moved = frame;
			d.push_back(dist2d(*o->getPosition(), s_to));
			if (d.back() <= NEAR_DIST)
				++nearTo;
			if (d.back() > dist2d(w.from, s_to) + 50.0f)
				++away;
		}
		d0.push_back(dist2d(w.from, s_to));
		if (checkRecv && ai != nullptr)
		{
			const Coord3D *g = ai->getGoalPosition();
			w.received = (g != nullptr && dist2d(*g, w.goal) > 1.0f) || (w.wasIdle && !ai->isIdle());
		}
		if (w.moved != 0)
			++moved;
		if (w.received)
			++recv;
		if (ai != nullptr)
		{
			idle += ai->isIdle() ? 1 : 0;
			wait += ai->isWaitingForPath() ? 1 : 0;
			stuck += ai->isBlockedAndStuck() ? 1 : 0;
			moving += ai->isMoving() ? 1 : 0;
		}
	}
	if (checkRecv)
		s_recvDone = TRUE;
	const UnsignedInt t = frame - s_issueFrame;
	if (s_cfg.tl != 0 && t % s_cfg.tl == 0)
	{
		std::vector<Real> dd = d;
		say("tl order=%d t=%u recv=%d moved=%d moving=%d idle=%d wait=%d stuck=%d near=%d away=%d d50=%u", s_issued - 1, t,
			recv, moved, moving, idle, wait, stuck, nearTo, away, pct(dd, 1, 2));
	}
	const Bool windowOver = frame >= s_issueFrame + s_cfg.watch;
	if (!s_delayDone && (moved == (Int)s_watch.size() || windowOver))
	{
		s_delayDone = TRUE;
		std::vector<UnsignedInt> dl;
		for (size_t i = 0; i < s_watch.size(); ++i)
			if (s_watch[i].moved != 0)
				dl.push_back(s_watch[i].moved - s_issueFrame);
		std::sort(dl.begin(), dl.end());
		const Int m = (Int)dl.size();
		say("order=%d n=%d kind=%s f_issue=%u ra=%d moved=%d first=%+d p50=%+d p90=%+d last=%+d", s_issued - 1,
			(Int)s_watch.size(), s_cfg.attack ? "attack" : "move", s_issueFrame, s_issueRunAhead, m,
			m ? (Int)dl[0] : -1, m ? (Int)dl[m / 2] : -1, m ? (Int)dl[(m * 9) / 10 < m ? (m * 9) / 10 : m - 1] : -1,
			m ? (Int)dl[m - 1] : -1);
	}
	if (!windowOver)
		return;
	const UnsignedInt dmax = d.empty() ? 0 : (UnsignedInt)*std::max_element(d.begin(), d.end());
	const UnsignedInt d50 = pct(d, 1, 2), d90 = pct(d, 9, 10);
	say("end order=%d n=%d f=%u recv=%d moved=%d idle=%d wait=%d stuck=%d d0=%u d50=%u d90=%u dmax=%u near=%d away=%d",
		s_issued - 1, (Int)s_watch.size(), frame, recv, moved, idle, wait, stuck, pct(d0, 1, 2), d50, d90, dmax, nearTo, away);
	s_watch.clear();
	if (s_issued >= s_cfg.count)
		say("done");
}

} // namespace

void gxMassOrderUpdate()
{
	if (!s_configRead)
	{
		s_configRead = TRUE;
		s_cfg.on = readConfig();
	}
	if (!s_cfg.on || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
		|| TheGameLogic->isLoadingMap() || TheMessageStream == nullptr || TheTerrainLogic == nullptr)
		return;
	if (TheRecorder != nullptr && TheRecorder->isPlaybackMode())
		return;		// a replay's orders are the recording's
	const Player *me = (ThePlayerList != nullptr) ? ThePlayerList->getLocalPlayer() : nullptr;
	if (me == nullptr || !me->isPlayableSide())
		return;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame == s_lastFrame)
		return;		// one step per logic frame, however often the client updates
	s_lastFrame = frame;
	if (!s_watch.empty())
		watch(frame);
	if (!s_spawned && s_cfg.noSpawn)
		s_spawned = TRUE;
	if (!s_spawned && frame >= s_cfg.spawn)
	{
		s_spawned = TRUE;
		if (!GameLogic::gxCheatsAllowed())
		{
			say("refused: the spawn cheat needs a one-human sandbox match (GameLogic::gxCheatsAllowed)");
			s_cfg.on = FALSE;
			return;
		}
		spawn(me);
	}
	if (s_watch.empty() && s_issued < s_cfg.count && frame >= s_cfg.at + (UnsignedInt)s_issued * s_cfg.every)
		issue(me);
}
