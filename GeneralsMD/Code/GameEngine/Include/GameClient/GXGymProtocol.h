/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// ai-gym: the action-line grammar a training bot writes (GXGym.cpp reads it); pure text, no engine types.
// One action per line; `step` ends a step. Token order: verb, name, x y, target, ids, where the verb takes them.
// move|amove <x> <y> <id>... ; attack <target> <id>... ; stop|evacuate|sell <id>... ; guard <x> <y> <id>...
// build <Template> <x> <y> <dozer> [@<deg>]: @deg = facing, world frame, -360..360, else PlacementViewAngle
// train <Template> <factory> ; enter <container> <id>... ; exit <unit> <container> ; rally <x> <y> <building>
// science <Science> ; power <Power> <source> ; powerat <Power> <x> <y> <source> ; poweron <Power> <target> <source>
// upgrade <Upgrade> <producer> ; source/producer 0 = GXGym.cpp picks one. Only well-formedness is checked here.

#pragma once

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum GXGymVerb { GXG_NONE, GXG_STEP, GXG_MOVE, GXG_AMOVE, GXG_ATTACK, GXG_STOP, GXG_BUILD, GXG_TRAIN, GXG_ENTER, GXG_RALLY,
	GXG_GUARD, GXG_POWER, GXG_POWERAT, GXG_POWERON, GXG_UPGRADE, GXG_EVACUATE, GXG_EXIT, GXG_SCIENCE, GXG_SELL,
	GXG_SPAWN, GXG_CLEAR };	// the last two: scenario-controller verbs (GX_GYM_SCENARIO=1, sandbox only), never a policy's

enum { GXGYM_MAX_IDS = 64, GXGYM_MAX_TMPL = 64 };
static const float GXGYM_MAX_COORD = 100000.0f;	// far past any map; only rejects garbage

struct GXGymAction
{
	int verb;
	float x, y;
	int target;
	char tmpl[GXGYM_MAX_TMPL];
	int ids[GXGYM_MAX_IDS];
	int nIds;
	bool hasAngle;		///< build only: a facing was given ...
	float angleDeg;		///< ... in degrees, world frame (MSG_DOZER_CONSTRUCT's angle); without it the template's own
};

namespace gxgym_detail
{
inline bool nextToken(const char *&p, char *tok, size_t cap)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;
	size_t n = 0;
	while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
	{
		if (n + 1 >= cap)
			return false;	// longer than any valid token
		tok[n++] = *p++;
	}
	tok[n] = '\0';
	return n > 0;
}

inline bool toCoord(const char *s, float *out)
{
	char *end = nullptr;
	const double v = strtod(s, &end);
	if (end == s || *end != '\0' || !(v > -GXGYM_MAX_COORD && v < GXGYM_MAX_COORD))	// NaN fails too
		return false;
	*out = (float)v;
	return true;
}

inline bool toId(const char *s, int *out)
{
	if (*s < '0' || *s > '9')
		return false;	// no sign, no blank
	char *end = nullptr;
	const long long v = strtoll(s, &end, 10);
	if (*end != '\0' || v <= 0 || v > 0x7fffffffLL)
		return false;
	*out = (int)v;
	return true;
}

inline bool isIdentifier(const char *s)
{
	for (; *s != '\0'; ++s)
		if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return false;
	return true;
}
} // namespace gxgym_detail

/// What each verb takes, in token order: a name, x y, a target id, then its ids.
struct GXGymShape
{
	const char *name;
	int verb;
	bool tmpl, xy, target;
	int ids;		///< 0 none, 1 exactly one, 2 one or more
	bool pickable;	///< the one id may be 0: GXGym.cpp picks it
};

inline const GXGymShape *gxGymShape(const char *verbName)
{
	static const GXGymShape shapes[] = {
		{ "step", GXG_STEP, false, false, false, 0, false },
		{ "move", GXG_MOVE, false, true, false, 2, false },
		{ "amove", GXG_AMOVE, false, true, false, 2, false },
		{ "attack", GXG_ATTACK, false, false, true, 2, false },
		{ "stop", GXG_STOP, false, false, false, 2, false },
		{ "build", GXG_BUILD, true, true, false, 1, false },
		{ "train", GXG_TRAIN, true, false, false, 1, false },
		{ "enter", GXG_ENTER, false, false, true, 2, false },
		{ "rally", GXG_RALLY, false, true, false, 1, false },
		{ "guard", GXG_GUARD, false, true, false, 2, false },
		{ "power", GXG_POWER, true, false, false, 1, true },
		{ "powerat", GXG_POWERAT, true, true, false, 1, true },
		{ "poweron", GXG_POWERON, true, false, true, 1, true },
		{ "upgrade", GXG_UPGRADE, true, false, false, 1, true },
		{ "evacuate", GXG_EVACUATE, false, false, false, 2, false },
		{ "exit", GXG_EXIT, false, false, true, 1, false },
		{ "science", GXG_SCIENCE, true, false, false, 0, false },
		{ "sell", GXG_SELL, false, false, false, 2, false },
		// Track B scenario drills: "spawn <Template> <x> <y> <player> <count> [vet 1-4]"
		// and "clear <Template> <player>". The trailing numbers are NOT unit ids (perform() routes these two first).
		{ "spawn", GXG_SPAWN, true, true, false, 2, false },
		{ "clear", GXG_CLEAR, true, false, false, 1, false } };
	for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i)
		if (strcmp(verbName, shapes[i].name) == 0)
			return &shapes[i];
	return nullptr;
}

/// 1 and *out filled when the line is a well-formed action; 0 and *why (a static string) otherwise.
inline int gxGymParseAction(const char *line, GXGymAction *out, const char **why)
{
	using namespace gxgym_detail;
	memset(out, 0, sizeof(*out));
	const char *p = line;
	char tok[128];
	if (!nextToken(p, tok, sizeof(tok)))
	{
		*why = "empty";
		return 0;
	}
	const GXGymShape *shape = gxGymShape(tok);
	if (shape == nullptr)
	{
		*why = "unknown verb";
		return 0;
	}
	out->verb = shape->verb;

	if (shape->tmpl && (!nextToken(p, out->tmpl, sizeof(out->tmpl)) || !isIdentifier(out->tmpl)))
	{
		*why = "bad name";
		return 0;
	}
	if (shape->xy && (!nextToken(p, tok, sizeof(tok)) || !toCoord(tok, &out->x) || !nextToken(p, tok, sizeof(tok))
		|| !toCoord(tok, &out->y)))
	{
		*why = "bad coordinates";
		return 0;
	}
	if (shape->target && (!nextToken(p, tok, sizeof(tok)) || !toId(tok, &out->target)))
	{
		*why = "bad target id";
		return 0;
	}

	while (nextToken(p, tok, sizeof(tok)))
	{
		if (out->nIds >= GXGYM_MAX_IDS)
		{
			*why = "too many units";
			return 0;
		}
		if (tok[0] == '@')	// build's facing: after its one builder, and the last token
		{
			if (out->verb != GXG_BUILD || out->nIds != 1 || out->hasAngle)
			{
				*why = out->verb != GXG_BUILD ? "only build takes an angle" : "the angle goes once, after the builder";
				return 0;
			}
			float deg = 0.0f;
			if (!toCoord(tok + 1, &deg) || deg < -360.0f || deg > 360.0f)
			{
				*why = "bad angle";
				return 0;
			}
			out->hasAngle = true;
			out->angleDeg = deg;
			continue;
		}
		if (out->hasAngle)
		{
			*why = "the angle goes once, after the builder";
			return 0;
		}
		int *id = &out->ids[out->nIds++];
		if (!toId(tok, id) && !(shape->pickable && strcmp(tok, "0") == 0))
		{
			*why = "bad unit id";
			return 0;
		}
	}
	if (*p != '\0')
	{
		*why = "token too long";
		return 0;
	}

	if (out->verb == GXG_SPAWN && (out->nIds < 2 || out->nIds > 3 || (out->nIds == 3 && out->ids[2] > 4)))
	{
		*why = "spawn takes <player> <count> [veterancy 1-4]";
		return 0;
	}
	if (shape->ids == 0 ? out->nIds != 0 : shape->ids == 1 ? out->nIds != 1 : out->nIds < 1)
	{
		*why = shape->ids == 0 ? "takes no ids" : shape->ids == 1 ? "needs exactly one id" : "needs a unit";
		return 0;
	}
	return 1;
}

// Which build-spot search a match may use. The guarded
// search (GXOrderDriver anyObjectNear: no object within ~2 x the footprint) exists because BuildAssistant's legality check reads
// Object::getShroudedStatus, a lazy cache that must never be computed on one peer alone. A match with exactly one peer cannot
// diverge from another, so it may use the engine's own legality check - which lets a supply center stand ~130 from its dock as
// stock Hard's does, instead of ~260. "One peer" = at most one human or observer slot (GameLogic::gxCheatsAllowed) and not a
// replay. Everything else - a 2-peer lockstep proof, any network match - keeps the guard.
inline bool gxGymSoloPlacement(bool atMostOneHumanSlot, bool inPlayback)
{
	return atMostOneHumanSlot && !inPlayback;
}

