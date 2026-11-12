/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// AI rule bank registry; a generator script produces rule_ids.json from these rows.
// Ids are explicit, append-only, never reused (they live in scb data and saves); param and ref keys start at 1.
// Rows sit in ONE macro: each row ends in a line splice, no comment inside; the blank line after the last ends it.
// GX_RULE_CLAIM is an exclusive value-hook claim: two enabled rules never share one.
#pragma once

#define GX_RULE_TABLE(GX_RULE, GX_RULE_PARAM, GX_RULE_REF, GX_RULE_CLAIM, GX_CAPABILITY) \
	GX_CAPABILITY("TIER.FILTER") \
	GX_CAPABILITY("SENSE.ENEMY_FACTION") \
	GX_CAPABILITY("SENSE.FOG_CONDITIONS") \
	GX_RULE(1, Probe, "PROBE", GX_TIER_HARD | GX_TIER_SENTIENT | GX_TIER_BRUTAL, 0, 2, 0) \
	GX_RULE_PARAM(1, 1, "period_s", 10, 1, 600, "s") \
	GX_RULE_REF(1, 1, "unit_template", GX_REF_TEMPLATE) \
	GX_RULE_REF(1, 2, "unit_team", GX_REF_TEAM) \

