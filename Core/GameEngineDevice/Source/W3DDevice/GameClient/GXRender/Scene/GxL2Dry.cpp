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

// GeneralsX: WS-G: see GxL2Dry.h.

#include "W3DDevice/GameClient/GXRender/Scene/GxL2Dry.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/dx8polygonrenderer.h"
#include "WW3D2/dx8rendererdebugger.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/lightenvironment.h"
#include "WW3D2/mapper.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/shader.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/ww3d.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// GxL2Contract.h's FlushSite and dx8renderer.h's GxFlushSite are one list (CONTRACT_L2.md §1.1).
static_assert(gxl2::SITE_UNSET == GXFS_UNSET && gxl2::SITE_MAIN_OPAQUE == GXFS_MAIN_OPAQUE
	&& gxl2::SITE_OCCL_TRANSL_PRE == GXFS_OCCL_TRANSL_PRE && gxl2::SITE_OCCL_TRANSL_STENCIL == GXFS_OCCL_TRANSL_STENCIL
	&& gxl2::SITE_OCCL_BUCKET == GXFS_OCCL_BUCKET && gxl2::SITE_OCCL_OTHERS == GXFS_OCCL_OTHERS
	&& gxl2::SITE_OCCL_OCCLUDERS == GXFS_OCCL_OCCLUDERS && gxl2::SITE_OCCL_EMPTY == GXFS_OCCL_EMPTY
	&& gxl2::SITE_OCCL_PLAIN == GXFS_OCCL_PLAIN && gxl2::SITE_TRANSLUCENT == GXFS_TRANSLUCENT
	&& gxl2::SITE_STATIC_SORT == GXFS_STATIC_SORT && gxl2::SITE_PROJ_SHADOW == GXFS_PROJ_SHADOW && GXFS_N == 12,
	"GxFlushSite and gxl2::FlushSite diverged");

static bool s_plainPass = false;

void GxL2Dry_SetPass(bool plainScenePass) { s_plainPass = plainScenePass; }

static gxl2::MapperKind gxMapperKind(VertexMaterialClass *m, int stage)
{
	TextureMapperClass *mp = m ? m->Peek_Mapper(stage) : nullptr;
	if (mp == nullptr)
		return gxl2::MAP_NONE;
	return mp->Mapper_ID() == TextureMapperClass::MAPPER_ID_SCALE ? gxl2::MAP_SCALE : gxl2::MAP_OTHER;
}

void GxL2Dry_FillCat(DX8TextureCategoryClass *cat, DX8PolygonRendererClass *pr, gxl2::CatState &c)
{
	DX8FVFCategoryContainer *k = cat->Get_Container();
	c.container = k;
	c.fvf = k->Get_FVF();
	c.sortingContainer = k->Is_Sorting();
	c.skinContainer = k->Is_Skin();
	c.pass = (int)pr->Get_Pass();
	VertexMaterialClass *m = (VertexMaterialClass *)cat->Peek_Material();
	c.material = m;
	for (int s = 0; s < 2; ++s) {
		c.tex[s] = cat->Peek_Texture(s);
		c.mapper[s] = gxMapperKind(m, s);
		c.uvSource[s] = (uint8_t)(m ? m->Get_UV_Source(s) : s);
	}
	// mapperScale stays 0: the material pointer is in the key and one material holds one mapper per stage, so equal pointers
	// mean equal scales (ScaleTextureMapperClass::Scale is protected).
	const ShaderClass sh = cat->Get_Shader();
	c.shader.bits = sh.Get_Bits();
	// PROXY from the bits, for the static census only: E4 proper is decoded from the effective post-Apply state at the run
	// (CONTRACT_L2.md §2 E4, contract r2 #3), which a raw write can make differ from these bits.
	c.shader.blendOff = sh.Get_Src_Blend_Func() == ShaderClass::SRCBLEND_ONE && sh.Get_Dst_Blend_Func() == ShaderClass::DSTBLEND_ZERO;
	c.shader.zEnable = true;
	c.shader.depthWrite = sh.Get_Depth_Mask() == ShaderClass::DEPTH_WRITE_ENABLE;
	c.shader.depthCmpLess = sh.Get_Depth_Compare() == ShaderClass::PASS_LESS || sh.Get_Depth_Compare() == ShaderClass::PASS_LEQUAL;
	c.shader.colorWrite = sh.Get_Color_Mask() == ShaderClass::COLOR_WRITE_ENABLE;
	c.shader.stencilProfileOk = true;	// ambient: judged at the run
}

// The task's facts the static rules need, from pure field reads only (dx8renderer.cpp:1830-1908): no Get_Transform /
// Is_Transform_Identity here - they validate transforms (rendobj.h:598) and are read at the task's execution instead.
// Get_Container is a plain pointer read.
void GxL2Dry_FillTask(MeshClass *mesh, DX8PolygonRendererClass *pr, int batch, gxl2::TaskState &t)
{
	MeshModelClass *mdl = mesh->Peek_Model();
	t.mesh = mesh;
	t.polyRenderer = pr;
	const RenderObjClass *root = mesh;
	while (root->Get_Container() != nullptr)
		root = root->Get_Container();
	t.object = root;
	t.skin = mdl->Get_Flag(MeshModelClass::SKIN) != 0;
	t.sort = mdl->Get_Flag(MeshModelClass::SORT) != 0;
	t.aligned = mdl->Get_Flag(MeshModelClass::ALIGNED) != 0;
	t.oriented = mdl->Get_Flag(MeshModelClass::ORIENTED) != 0;
	t.sortingEnabled = WW3D::Is_Sorting_Enabled();
	t.staticSortLevel = false;	// a static-sort mesh never reaches these lists (mesh.cpp:669-677)
	t.strip = false;			// ENABLE_STRIPING is off (dx8renderer.cpp:41): every polygon renderer is a triangle list
	t.objectScale = mesh->Get_ObjectScale();
	t.alphaOverride = mesh->Get_Alpha_Override();
	t.materialOverride = mesh->Get_User_Data() && *(int *)mesh->Get_User_Data() == RenderObjClass::USER_DATA_MATERIAL_OVERRIDE;
	LightEnvironmentClass *le = mesh->Get_Lighting_Environment();
	t.hasLenv = le != nullptr;
	t.lightCount = 0;
	t.lightPoint = 0;
	if (le != nullptr) {
		t.lightCount = (uint8_t)le->Get_Light_Count();
		for (int l = 0; l < t.lightCount; ++l)
			if (le->isPointLight(l)) t.lightPoint |= (uint8_t)(1u << l);
	}
	t.vbOverflow = batch > 0;
	// No bounds are copied at collection yet (CONTRACT_L2.md §3.6), so every task is tie-capable with every other one.
	t.bounds.lo = gxl2::Vec3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	t.bounds.hi = gxl2::Vec3{ FLT_MAX, FLT_MAX, FLT_MAX };
}

void GxL2Dry_FillFlush(int site, gxl2::FlushCtx &f)
{
	f.site = (gxl2::FlushSite)site;
	f.pass.defaultPass = s_plainPass;
	f.pass.extraPassDisabled = s_plainPass;
	f.pass.cullingInverted = ShaderClass::Is_Backface_Culling_Inverted();
	f.pass.texturingEnabled = WW3D::Is_Texturing_Enabled();
	f.pass.coloringEnabled = WW3D::Is_Coloring_Enabled();
	f.pass.debugSwitches = DX8RendererDebugger::Is_Enabled() || !DX8Wrapper::_Is_Triangle_Draw_Enabled()
		|| DX8Wrapper::Get_Draw_Polygon_Low_Bound_Limit() != 0;	// GeneralsX: WS-G seat: Draw's low bound skips draws
#ifdef WWDEBUG
	f.pass.debugSwitches = f.pass.debugSwitches || WW3D::Expose_Prelit();	// the classic loop's prelit texture override (dx8renderer.cpp Render)
#endif
	f.pass.forceMultiply = DX8TextureCategoryClass::GetForceMultiply();
}

// Mode B without bounds: a packet ends at the first unclaimed entry that DRAWS in this flush between two runs.
int GxL2Dry_PacketsB(const std::vector<GxClassicDraw> &list, const std::vector<gxl2::Reason> &reason, const std::vector<GxL2Run> &runs)
{
	int packets = 0;
	bool open = false;
	size_t r = 0;
	for (int k = 0; k < (int)list.size(); ++k) {
		if (r < runs.size() && k == runs[r].first) {
			if (!open) { ++packets; open = true; }
			k = runs[r].last;
			++r;
			continue;
		}
		const GxClassicDraw &d = list[k];
		const bool draws = d.kind == GxClassicDraw::MATPASS || d.kind == GxClassicDraw::DELAYED_MATPASS || d.kind == GxClassicDraw::DECAL
			|| (d.kind == GxClassicDraw::TASK && reason[k] != gxl2::R_SORTED && reason[k] != gxl2::R_SKIN_SORTED);
		if (draws) open = false;
	}
	return packets;
}

void GxL2Dry_Plan(const std::vector<GxClassicDraw> &list, int site, GxL2FlushPlan &out)
{
	const int n = (int)list.size();
	out.site = (gxl2::FlushSite)site;
	out.reason.assign(n, kGxL2NotTask);
	out.runs.clear();
	out.historyNext = out.nullLenvAfter = out.decalAfter = out.multiTaskMeshes = out.multiTaskTasks = out.packetsB = 0;

	gxl2::FlushCtx f;
	GxL2Dry_FillFlush(site, f);
	// staticStateKnown, zbias, clipPlaneEnable stay at their defaults here: E2 is judged at the run (state-check, Task 3).
	const gxl2::Policy p;	// phase 1a: no skins, tieEpsilon 0

	static std::vector<gxl2::CatState> cats;
	static std::vector<gxl2::TaskState> tasks;
	cats.resize(n);
	tasks.resize(n);
	std::unordered_map<const void *, int> procPasses;	// E8: procedural passes of each mesh this flush (visible + delayed)
	for (const GxClassicDraw &d : list)
		if (d.kind == GxClassicDraw::MATPASS || d.kind == GxClassicDraw::DELAYED_MATPASS)
			++procPasses[d.mesh];
	std::unordered_map<const void *, bool> meshRefused;	// E8: a mesh with any refused task is claimed in no pass
	// E8 (CONTRACT_L2 r6: onlyVisibleTask): a part refused at ITS run (E2/E4) cannot un-take another part of the same mesh drawn
	// elsewhere in the flush - another pass, or another category of the same pass (registration splits a mesh's polygons by
	// texture/material/shader, dx8renderer.cpp:1248) - so phase 1 claims only meshes with exactly one visible task in the flush.
	std::unordered_map<const void *, int> meshTasks;
	for (const GxClassicDraw &d : list)
		if (d.kind == GxClassicDraw::TASK)
			++meshTasks[d.mesh];
	for (const auto &mt : meshTasks)
		if (mt.second > 1) {
			++out.multiTaskMeshes;
			out.multiTaskTasks += mt.second;
		}
	for (int i = 0; i < n; ++i) {
		const GxClassicDraw &d = list[i];
		if (d.kind != GxClassicDraw::TASK)
			continue;
		GxL2Dry_FillCat(d.cat, d.pr, cats[i]);
		GxL2Dry_FillTask(d.mesh, d.pr, d.batch, tasks[i]);
		auto pp = procPasses.find(d.mesh);
		tasks[i].proceduralPasses = pp == procPasses.end() ? 0 : pp->second;
		tasks[i].onlyVisibleTask = meshTasks[d.mesh] == 1;
		out.reason[i] = gxl2::classifyTask(f, cats[i], tasks[i], p);
		if (out.reason[i] != gxl2::OK)
			meshRefused[d.mesh] = true;
	}
	for (int i = 0; i < n; ++i)
		if (out.reason[i] == gxl2::OK && meshRefused.count(list[i].mesh)) {
			tasks[i].onlyVisibleTask = false;
			out.reason[i] = gxl2::classifyTask(f, cats[i], tasks[i], p);
		}

	// Runs: maximal stretches of claimed tasks inside one category body (a body's tasks are contiguous in the list).
	std::vector<gxl2::ClaimedDraw> claimed;
	std::vector<gxl2::BinKey> seen;
	for (int i = 0; i < n;) {
		if (out.reason[i] != gxl2::OK) { ++i; continue; }
		int j = i;
		while (j + 1 < n && out.reason[j + 1] == gxl2::OK && list[j + 1].cat == list[i].cat)
			++j;
		claimed.clear();
		seen.clear();
		for (int k = i; k <= j; ++k) {
			gxl2::ClaimedDraw cd;
			cd.key = gxl2::binKey(cats[k], tasks[k]);
			cd.task = tasks[k];
			cd.classicSeq = k;
			claimed.push_back(cd);
			bool known = false;
			for (const gxl2::BinKey &s : seen)
				if (s == cd.key) { known = true; break; }
			if (!known) seen.push_back(cd.key);
		}
		GxL2Run r;
		r.first = i;
		r.last = j;
		r.keys = (int)seen.size();
		r.bins = (int)gxl2::planBins(claimed, p).size();
		out.runs.push_back(r);
		i = j + 1;
	}

	out.packetsB = GxL2Dry_PacketsB(list, out.reason, out.runs);

	out.taskFlags.assign(n, 0);
	for (int i = 0; i < n; ++i)
		if (list[i].kind == GxClassicDraw::TASK)
			out.taskFlags[i] = (uint8_t)((gxl2::historySensitive(tasks[i]) ? 1u : 0u) | (tasks[i].hasLenv ? 0u : 2u));
	GxL2Dry_RunCounters(list, out.reason, out.taskFlags, out);
}

// §6 census asks: history-sensitive tasks in a body that has a run (each body once); null-light tasks and decals
// after the first run. The task facts are static, so the execution-judged runs (GxScene.cpp) reuse the flush-start plan's flags
//
void GxL2Dry_RunCounters(const std::vector<GxClassicDraw> &list, const std::vector<gxl2::Reason> &reason,
	const std::vector<uint8_t> &taskFlags, GxL2FlushPlan &out)
{
	const int n = (int)std::min(list.size(), std::min(reason.size(), taskFlags.size()));
	out.historyNext = out.nullLenvAfter = out.decalAfter = 0;
	for (int b0 = 0; b0 < n;) {
		if (list[b0].kind != GxClassicDraw::TASK) { ++b0; continue; }
		int b1 = b0;
		while (b1 + 1 < n && list[b1 + 1].kind == GxClassicDraw::TASK && list[b1 + 1].cat == list[b0].cat) ++b1;
		bool hasRun = false;	// a body with an actual run, not merely an OK reason (a no-draw task keeps OK)
		for (const GxL2Run &r : out.runs) hasRun = hasRun || (r.first >= b0 && r.first <= b1);
		if (hasRun)
			for (int k = b0; k <= b1; ++k)
				if (reason[k] != gxl2::OK && (taskFlags[k] & 1u)) ++out.historyNext;
		b0 = b1 + 1;
	}
	if (!out.runs.empty())
		for (int k = out.runs.front().first; k < n; ++k) {
			if (list[k].kind == GxClassicDraw::DECAL) ++out.decalAfter;
			else if (list[k].kind == GxClassicDraw::TASK && (taskFlags[k] & 2u)) ++out.nullLenvAfter;
		}
}

// ---- census v2 ([GXL2CEN]): per flush site, means per drawn (WW3D) frame over 600-frame windows ---------------------------
static const char *const kGxReasonName[] = { "OK", "R_SITE", "R_PASS", "R_DEBUG", "R_FORCE_MULTIPLY", "R_STATE_UNKNOWN", "R_AMBIENT",
	"R_SORTED", "R_STATIC_SORT", "R_STRIP", "R_BLENDED", "R_NO_DEPTH_WRITE", "R_DEPTH_FUNC", "R_COLOR_MASK", "R_ALPHA_OVERRIDE",
	"R_MATERIAL_OVERRIDE", "R_MAPPER_STATEFUL", "R_NULL_LENV", "R_PROCEDURAL_PASS", "R_MULTI_TASK", "R_SKIN_PHASE",
	"R_SKIN_SORTED", "R_SKIN_NOT_EXACT", "R_SKIN_OVERFLOW", "R_CATEGORY_HISTORY" };
static const int kGxReasons = (int)(sizeof(kGxReasonName) / sizeof(kGxReasonName[0]));
static_assert(gxl2::R_CATEGORY_HISTORY == 24, "kGxReasonName follows gxl2::Reason");
static const char *const kGxSiteName[] = { "UNSET", "MAIN_OPAQUE", "OCCL_TRANSL_PRE", "OCCL_TRANSL_STENCIL", "OCCL_BUCKET",
	"OCCL_OTHERS", "OCCL_OCCLUDERS", "OCCL_EMPTY", "OCCL_PLAIN", "TRANSLUCENT", "STATIC_SORT", "PROJ_SHADOW" };

struct GxSiteAcc
{
	unsigned long long flushes, tasks, runs, keys, bins, claimed, historyNext, nullLenvAfter, decalAfter, multiTaskMeshes, multiTaskTasks,
		packetsB, packetsBB, packetsBS, flushesWithRun;	// packetsBB: Mode B with bounds (exec, sampled flushes); packetsBS: packetsB on the SAME
		// sampled flushes; flushesWithRun: the LOWER bound of Mode B packets (one per flush if bounds allowed every merge)
	unsigned long long reason[32];
};
// [0] stage=static: the flush-start plan (E2/E4 from the shader bits, a PROXY); [1] stage=exec: every task judged at its own draw
// (GX_L2_CHECK, GxScene.cpp) - E2 from the device truth, E4 decoded from it (GxL2Exec.h).
struct GxCensusSet
{
	GxSiteAcc acc[GXFS_N];
	unsigned winFrame0 = ~0u, lastFrame = ~0u, frames = 0;
	unsigned bbLastFrame = ~0u, bbFrames = 0;	// frames Mode B was planned on (GX_L2_MODEB_EVERY samples them)
};
static GxCensusSet s_cen[2];

static void gxCensus(GxCensusSet &cs, const char *stage, const GxL2FlushPlan &plan, unsigned frame)
{
	GxSiteAcc *const s_acc = cs.acc;
	unsigned &s_winFrame0 = cs.winFrame0, &s_lastFrame = cs.lastFrame, &s_frames = cs.frames;
	if (s_winFrame0 == ~0u)
		s_winFrame0 = frame;
	if (frame != s_lastFrame) {
		s_lastFrame = frame;
		++s_frames;
	}
	GxSiteAcc &a = s_acc[plan.site < GXFS_N ? plan.site : 0];
	++a.flushes;
	for (gxl2::Reason r : plan.reason)
		if (r != kGxL2NotTask) {
			++a.tasks;
			++a.reason[r < 32 ? r : 31];
		}
	for (const GxL2Run &r : plan.runs) {
		++a.runs;
		a.keys += r.keys;
		a.bins += r.bins;
		a.claimed += r.last - r.first + 1;
	}
	a.historyNext += plan.historyNext;
	a.nullLenvAfter += plan.nullLenvAfter;
	a.decalAfter += plan.decalAfter;
	a.multiTaskMeshes += plan.multiTaskMeshes;
	a.multiTaskTasks += plan.multiTaskTasks;
	a.packetsB += plan.packetsB;
	if (plan.packetsBBounded >= 0) {
		a.packetsBB += plan.packetsBBounded;
		a.packetsBS += plan.packetsB;
		if (frame != cs.bbLastFrame) { cs.bbLastFrame = frame; ++cs.bbFrames; }
	}
	a.flushesWithRun += plan.runs.empty() ? 0 : 1;
	if (frame - s_winFrame0 < 600)
		return;
	const double fr = s_frames ? (double)s_frames : 1.0;
	for (int s = 0; s < GXFS_N; ++s) {
		const GxSiteAcc &x = s_acc[s];
		if (x.flushes == 0)
			continue;
		char buf[1536];
		int len = snprintf(buf, sizeof(buf), "[GXL2CEN] f=%u frames=%u stage=%s site=%s flushes=%.1f tasks=%.1f claimed=%.1f "
			"packetsB=%.1f packetsB_min=%.1f packetsB_bounded=%.1f packetsB_sampled=%.1f runs=%.1f keys=%.1f bins=%.1f hist_next=%.1f nolenv_after=%.1f decal_after=%.1f multitask_meshes=%.1f multitask_tasks=%.1f "
			"reasons=", frame, s_frames, stage, kGxSiteName[s], x.flushes / fr, x.tasks / fr, x.claimed / fr, x.packetsB / fr, x.flushesWithRun / fr, cs.bbFrames ? (double)x.packetsBB / cs.bbFrames : 0.0, cs.bbFrames ? (double)x.packetsBS / cs.bbFrames : 0.0, x.runs / fr, x.keys / fr, x.bins / fr,
			x.historyNext / fr, x.nullLenvAfter / fr, x.decalAfter / fr, x.multiTaskMeshes / fr, x.multiTaskTasks / fr);
		for (int r = 0; r < kGxReasons && len > 0 && len < (int)sizeof(buf); ++r)
			if (x.reason[r])
				len += snprintf(buf + len, sizeof(buf) - len, "%s:%.1f,", kGxReasonName[r], x.reason[r] / fr);
		fprintf(stderr, "%s\n", buf);
	}
	memset(s_acc, 0, sizeof(cs.acc));
	s_winFrame0 = frame;
	s_frames = 0;
	cs.bbFrames = 0;
}

void GxL2Dry_Census(const GxL2FlushPlan &plan, unsigned frame) { gxCensus(s_cen[0], "static", plan, frame); }
void GxL2Dry_CensusExec(const GxL2FlushPlan &plan, unsigned frame) { gxCensus(s_cen[1], "exec", plan, frame); }
