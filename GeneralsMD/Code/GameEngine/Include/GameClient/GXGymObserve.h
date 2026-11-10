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

// GeneralsX: ai-gym observation logger, schema v1. See GXGymObserve.cpp.

#pragma once

#include <stdio.h>

enum { GXGYM_SCHEMA_VERSION = 1 };

/// Forget which templates were described. Call at the start of every match, before gxGymObserveStart.
void gxGymObserveReset();

/// One "start" line: the map, the logic rate, the view player and every playable player.
void gxGymObserveStart(FILE *out, const char *prefix, Int viewPlayerIndex);

/// One "obs" line for the logic state as it stands now, preceded by a "tmpl" line for each template not described yet.
/// viewPlayerIndex < 0: no fog view (every object is marked visible).
void gxGymObserveFrame(FILE *out, const char *prefix, UnsignedInt frame, Int viewPlayerIndex);

/// One "ext" line (GX_OBS_EXT=1): per mobile object worth a row, height, heading, signed forward speed, AI state, goal,
/// a harvester's preferred dock and boxes carried, unmanned/moving/idle bits. Omniscient; for decoded-replay analysis.
void gxGymObserveExtFrame(FILE *out, const char *prefix, UnsignedInt frame);
