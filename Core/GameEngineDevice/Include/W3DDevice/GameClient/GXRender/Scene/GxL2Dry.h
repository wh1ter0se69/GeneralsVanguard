/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @feature Claude 10/10/2026 WS-G (gx-handoff render-seam/rw1-G/PLAN_statecheck.md Task 2): L2's DRY plan of one Flush -
// the classic list (dx8renderer.h GxClassicDraw) turned into GxL2Contract.h's structs, classified (CONTRACT_L2.md §2), cut into runs
// (§3.1) and keys (§1.4), binned (planBins) - nothing taken, nothing drawn. At the flush start only the STATIC rules can be judged:
// E2 (state known) and E4 (effective depth/blend/colour/stencil) are judged at each run in the state-check (Task 3); here E4 uses a
// proxy decoded from the category's shader bits, and the census says so.

#pragma once

#include "W3DDevice/GameClient/GxL2Contract.h"
#include <vector>

struct GxClassicDraw;

struct GxL2Run
{
	int first, last;				// indices into the flush's classic list, both TASK entries of one category body
	int keys, bins;					// distinct bin keys, planBins bins (S1: consecutive draws of one pipeline)
};

static const gxl2::Reason kGxL2NotTask = (gxl2::Reason)255;	// GxL2FlushPlan::reason of a non-TASK entry

struct GxL2FlushPlan
{
	gxl2::FlushSite site = gxl2::SITE_UNSET;
	std::vector<gxl2::Reason> reason;		// per list entry (OK only for TASKs that pass the static rules); non-TASKs = R_SITE
	std::vector<GxL2Run> runs;
	std::vector<uint8_t> taskFlags;			// per list entry: bit 0 history-sensitive (gxl2::historySensitive), bit 1 no light environment
	int historyNext = 0;					// history-sensitive tasks in a category body that has a run (§6 ask)
	int nullLenvAfter = 0, decalAfter = 0;	// null-light tasks and decal meshes after the flush's first run (what the carry-out serves)
	int multiTaskMeshes = 0, multiTaskTasks = 0;	// meshes with more than one visible task, and their tasks (E8's coverage cost)
	int packetsBBounded = -1;	// Task 4 (-1 = not planned this flush): Mode B packets WITH the copied bounds (GxScene.cpp gxModeB; exec census only)
	int packetsB = 0;	// Mode B packets WITHOUT bounds (CONTRACT_L2 §3.3): runs merged across everything that draws nothing here
						// (category prologues, sorted tasks - their triangles go to the sorter - skin deforms); any unclaimed draw in
						// between ends a packet, because without bounds every pair is tie-capable / overlapping (validateEmission)
};

void GxL2Dry_SetPass(bool plainScenePass);	// RTS3DScene::Flush: default pass and no extra-pass polygon mode (§1.1)
void GxL2Dry_Plan(const std::vector<GxClassicDraw> &list, int site, GxL2FlushPlan &out);
// The pieces GX_L2_CHECK's execution-driven judgement (GxScene.cpp) shares with the plan: the flush context, a TASK's static facts
// (pure field reads), the Mode B packet count over judged reasons, and the census line stage=exec.
void GxL2Dry_FillFlush(int site, gxl2::FlushCtx &f);
void GxL2Dry_FillCat(class DX8TextureCategoryClass *cat, class DX8PolygonRendererClass *pr, gxl2::CatState &c);
void GxL2Dry_FillTask(class MeshClass *mesh, class DX8PolygonRendererClass *pr, int batch, gxl2::TaskState &t);
void GxL2Dry_RunCounters(const std::vector<GxClassicDraw> &list, const std::vector<gxl2::Reason> &reason,
	const std::vector<uint8_t> &taskFlags, GxL2FlushPlan &out);	// historyNext / nullLenvAfter / decalAfter against out.runs
int GxL2Dry_PacketsB(const std::vector<GxClassicDraw> &list, const std::vector<gxl2::Reason> &reason, const std::vector<GxL2Run> &runs);
void GxL2Dry_CensusExec(const GxL2FlushPlan &plan, unsigned frame);
void GxL2Dry_Census(const GxL2FlushPlan &plan, unsigned frame);	// accumulates; prints [GXL2CEN] every 600 WW3D frames
