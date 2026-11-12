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

// See GxScene.h.

#include "W3DDevice/GameClient/GXRender/Scene/GxScene.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Dry.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Check.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Exec.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Model.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Seat.h"
#include "W3DDevice/GameClient/GxDrawRun.h"
#include "W3DDevice/GameClient/GxTestEnv.h"
#include "GameClient/GXGfxProbe.h"
#include "WW3D2/camera.h"
#include "WW3D2/decalmsh.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8polygonrenderer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/lightenvironment.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/shader.h"
#include "WW3D2/ww3d.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static_assert(DX8C_N <= GXCEN_MAX, "[GXCENSUS] would drop the last DX8C_ counters: raise GXCEN_MAX");

static std::vector<GxClassicDraw> s_list;	// the current Flush's classic draw list, taken at its start (reused)
static std::vector<GxClassicDraw> s_exec;	// GX_L2_CHECK: what the Flush then processed (GxL2Executed)
static bool s_census = false;				// GX_L2_CENSUS
static int s_check = 0;						// GX_L2_CHECK: 1 = check, 2 = check with the planted fail arm

#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
static std::vector<std::pair<const void *, const void *>> s_bins;	// (category, polygon renderer) of each r_elig task

// The census of one Flush's list (dx8wrapper.h DX8C_L2_*). The task tests are DX8TextureCategoryClass::Render's own
// branches (dx8renderer.cpp): SKIN -> identity world, SORT with sorting on -> the triangle sorter, the alpha / material
// override, ALIGNED / ORIENTED, object scale, a null light environment; "blend" is the category's shader as Render sets
// it (forced multiply included). r_elig is a PRE-CONTRACT PROXY for sizing only; decides eligibility.
static void gxL2Census()
{
	if (!DX8Wrapper::CensusOn || s_list.empty())
		return;
	unsigned long long *c = DX8Wrapper::Census;
	++c[DX8C_L2_FLUSH];
	s_bins.clear();
	for (const GxClassicDraw &d : s_list) {
		switch (d.kind) {
		case GxClassicDraw::SKIN: {
			MeshModelClass *mdl = d.mesh->Peek_Model();
			++c[DX8C_L2_SKIN];
			if (mdl->Get_Flag(MeshModelClass::SORT) && WW3D::Is_Sorting_Enabled() && mdl->Get_Sort_Level() == SORT_LEVEL_NONE)
				++c[DX8C_L2_SKIN_SORT];	// Register_Mesh_Type's container choice
			if (d.batch > 0)
				++c[DX8C_L2_SKIN_B1];
			break;
		}
		case GxClassicDraw::MATPASS:			++c[DX8C_L2_MATPASS]; break;
		case GxClassicDraw::DELAYED_MATPASS:	++c[DX8C_L2_DMATPASS]; break;
		case GxClassicDraw::DECAL:				++c[DX8C_L2_DECAL]; break;
		case GxClassicDraw::TASK: {
			MeshClass *mesh = d.mesh;
			MeshModelClass *mdl = mesh->Peek_Model();
			++c[DX8C_L2_TASK];
			if (mdl->Get_Flag(MeshModelClass::SKIN)) {
				++c[DX8C_L2_S_TASK];
				break;
			}
			++c[DX8C_L2_RIGID];
			if (mdl->Get_Flag(MeshGeometryClass::SORT) && WW3D::Is_Sorting_Enabled()) {
				++c[DX8C_L2_R_SORT];
				break;
			}
			const ShaderClass sh = d.cat->Get_Shader();
			const bool opaque_blend = sh.Get_Src_Blend_Func() == ShaderClass::SRCBLEND_ONE && sh.Get_Dst_Blend_Func() == ShaderClass::DSTBLEND_ZERO;
			const bool blend = !opaque_blend || sh.Get_Depth_Mask() != ShaderClass::DEPTH_WRITE_ENABLE
				|| (DX8TextureCategoryClass::GetForceMultiply() && sh.Get_Dst_Blend_Func() == ShaderClass::DSTBLEND_ZERO);
			const bool ovr = mesh->Get_Alpha_Override() != 1.0f
				|| (mesh->Get_User_Data() && *(int *)mesh->Get_User_Data() == RenderObjClass::USER_DATA_MATERIAL_OVERRIDE);
			const bool cam = mdl->Get_Flag(MeshModelClass::ALIGNED) || mdl->Get_Flag(MeshModelClass::ORIENTED);
			if (blend) ++c[DX8C_L2_R_BLEND];
			if (ovr) ++c[DX8C_L2_R_OVR];
			if (cam) ++c[DX8C_L2_R_CAM];
			if (mesh->Get_ObjectScale() != 1.0f) ++c[DX8C_L2_R_SCALE];
			if (mesh->Get_Lighting_Environment() == nullptr) ++c[DX8C_L2_R_LENV0];
			if (d.pr->Get_Pass() > 0) ++c[DX8C_L2_R_PASS1];
			if (!blend && !ovr && !cam) {
				++c[DX8C_L2_R_ELIG];
				s_bins.push_back(std::make_pair((const void *)d.cat, (const void *)d.pr));
			}
			break;
		}
		}
	}
	std::sort(s_bins.begin(), s_bins.end());
	c[DX8C_L2_R_BIN] += (unsigned long long)(std::unique(s_bins.begin(), s_bins.end()) - s_bins.begin());
}
#endif

// GX_L2_CHECK, the classic half of the state-check: the list taken at a Flush's start must equal, entry for entry and
// in order, what that Flush then processed (kind, category, polygon renderer, material pass, mesh, decal, skin batch).
// It proves Gx_List_Pending reproduces Flush before L2 builds on it; L2's own list joins this comparison under the
// contract's rules. A mismatch prints the first differing entry (at most 20 flushes in full) and every 10000 checked
// flushes a summary. The fail arm (GX_L2_CHECK=2) swaps the listed first two entries, so every Flush with two or more
// entries must report a mismatch.
static unsigned long long s_flushes = 0, s_entries = 0, s_bad = 0, s_b1 = 0;	// s_b1: checked entries in skin batch >= 1
static const char *const kGxFlushSiteName[] = { "UNSET", "MAIN_OPAQUE", "OCCL_TRANSL_PRE", "OCCL_TRANSL_STENCIL", "OCCL_BUCKET",
	"OCCL_OTHERS", "OCCL_OCCLUDERS", "OCCL_EMPTY", "OCCL_PLAIN", "TRANSLUCENT", "STATIC_SORT", "PROJ_SHADOW" };
static_assert(sizeof(kGxFlushSiteName) / sizeof(kGxFlushSiteName[0]) == GXFS_N, "dx8renderer.h GxFlushSite names");
static unsigned s_siteFrame = ~0u;	// the flush id: the site and its index within the WW3D frame
static int s_siteIndex[GXFS_N];
static int s_flushIndex = 0;

static void gxL2PrintEntry(const char *tag, const std::vector<GxClassicDraw> &v, size_t i)
{
	if (i >= v.size()) {
		fprintf(stderr, " %s=<end>", tag);
		return;
	}
	const GxClassicDraw &d = v[i];
	// Pointers only: by the end of the Flush a task's mesh, model and polygon renderers may be gone (a static-sort mesh whose last
	// reference was its render task), so nothing here is dereferenced.
	fprintf(stderr, " %s={k=%d cat=%p pr=%p pass=%p mesh=%p decal=%p b=%d}", tag, d.kind, (void *)d.cat, (void *)d.pr,
		(void *)d.pass, (void *)d.mesh, (void *)d.decal, d.batch);
}

// C9: at every GX_L2_C9_EVERY-th draw inside a mesh Flush (default 997, prime so the samples walk the
// sites), the wrapper's caches vs the device's own state - every RenderStates / TextureStageStates slot holding a value (not the
// 0x12345678 sentinel), the eight texture bindings, the lights the wrapper believes enabled (payload + enable), and WORLD/VIEW as
// last sent. A difference is state written behind the wrapper, i.e. E2's premise "a valid shadow is the device" failing there.
// Device reads take _Get_D3D_Device8(), which drains the d3d8 ring first (check mode only). Fail arm GX_L2_MUTANT=10: at the
// first draw of every 100th Flush a planted ZFUNC ALWAYS and a VIEW moved 1 unit are written behind the wrapper, sampled, and put
// back before the draw.
static const unsigned kGxSentinel = 0x12345678;
static int s_c9Every = 997;
static int s_mutant = 0;
static bool s_inFlush = false;
static unsigned long long s_flushDraws = 0, s_drawsSeen = 0, s_c9Samples = 0, s_c9DiffSamples = 0;	// s_drawsSeen: C9's cadence,
// counted across flushes so small flushes are sampled too and the samples walk the sites
static unsigned long long s_c9Rs[256], s_c9Tss[MAX_TEXTURE_STAGES][32], s_c9Tex[MAX_TEXTURE_STAGES], s_c9Light[4], s_c9World, s_c9View;
static unsigned long long s_c9Proj, s_c9Texm[8], s_c9Mat, s_c9Fvf, s_c9ViewSent, s_c9Planted;	// planted: M10 samples (X9's denominator)
static unsigned long long s_c9TruthRs[256], s_c9TruthTss[MAX_TEXTURE_STAGES][32];	// the device-truth table vs the device
// The sentinel report: the RS / stage-0..1 TSS slots DXVK's fixed-function pipeline reads for a triangle
// draw (d3d9_fixed_function / d3d9_state: depth, blend, alpha test, fog, stencil, cull, fill, lighting + material sources,
// colour write, wrap, texture factor; per stage colour/alpha ops and args, texcoord index, addressing, filters, transform flags),
// counted at every C9 sample where the wrapper holds the sentinel, with the device's value then - so E2's "known" rule can be argued
// from data (device default and constant, or not).
struct GxSentinelSeen { unsigned long long n; DWORD first; bool varies; };	// here: E2 slots whose device TRUTH is unknown
static GxSentinelSeen s_sentRs[256], s_sentTss[2][32];
static int s_c9Printed = 0;
static unsigned s_truthFrame0 = 0;	// the frame the device truth started (raw writer calls per frame: DX8Wrapper::GxTruthRawCalls)

static void gxC9Report(const char *field, unsigned shadow, unsigned device)
{
	if (++s_c9Printed > 20)
		return;
	fprintf(stderr, "[GXL2CHK] f=%u flush=%s#%d kind=SHADOW field=%s classic=0x%08X device=0x%08X\n", WW3D::Get_Frame_Count(),
		kGxFlushSiteName[GxFlushSite], s_flushIndex, field, shadow, device);
}

static void gxC9Sample(bool sortingPath)
{
	DX8Wrapper::GxLogBody rd(gxlog::DEVICE_READ);	// a modelled flush's sample is a READ
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();	// drains the ring
	const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
	int diffs = 0;
	char name[32];
	for (unsigned i = 0; i < 256; ++i) {
		if (v.renderStates[i] == kGxSentinel)
			continue;
		DWORD d = 0;
		if (dev->GetRenderState((D3DRENDERSTATETYPE)i, &d) == D3D_OK && d != v.renderStates[i]) {
			++s_c9Rs[i]; ++diffs;
			snprintf(name, sizeof(name), "RS%u", i);
			gxC9Report(name, v.renderStates[i], d);
		}
	}
	for (unsigned st = 0; st < MAX_TEXTURE_STAGES; ++st) {
		for (unsigned i = 0; i < 32; ++i) {
			if (v.textureStageStates[st][i] == kGxSentinel)
				continue;
			DWORD d = 0;
			if (dev->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)i, &d) == D3D_OK && d != v.textureStageStates[st][i]) {
				++s_c9Tss[st][i]; ++diffs;
				snprintf(name, sizeof(name), "TSS%u.%u", st, i);
				gxC9Report(name, v.textureStageStates[st][i], d);
			}
		}
		IDirect3DBaseTexture8 *t = nullptr;
		if (dev->GetTexture(st, &t) == D3D_OK) {
			if (t != v.textures[st]) {
				++s_c9Tex[st]; ++diffs;
				snprintf(name, sizeof(name), "TEX%u", st);
				gxC9Report(name, (unsigned)(uintptr_t)v.textures[st], (unsigned)(uintptr_t)t);
			}
			if (t) t->Release();
		}
	}
	for (unsigned l = 0; l < 4; ++l) {
		if (!v.lightValid[l])
			continue;
		BOOL on = FALSE;
		D3DLIGHT8 dl;
		if (dev->GetLightEnable(l, &on) != D3D_OK)
			continue;
		const bool diff = (on != FALSE) != v.lightEnables[l]
			|| (on && (dev->GetLight(l, &dl) != D3D_OK || memcmp(&dl, &v.lights[l], sizeof(D3DLIGHT8)) != 0));
		if (diff) {
			++s_c9Light[l]; ++diffs;
			snprintf(name, sizeof(name), "LIGHT%u", l);
			gxC9Report(name, v.lightEnables[l], (unsigned)on);
		}
	}
	for (unsigned i = 0; i < 256; ++i) {	// the device-truth table (what E2 reads) vs the device
		if (!DX8Wrapper::GxTruthRSKnown[i])
			continue;
		DWORD d = 0;
		if (dev->GetRenderState((D3DRENDERSTATETYPE)i, &d) == D3D_OK && d != DX8Wrapper::GxTruthRS[i]) {
			++s_c9TruthRs[i]; ++diffs;
			snprintf(name, sizeof(name), "TRUTH_RS%u", i);
			gxC9Report(name, DX8Wrapper::GxTruthRS[i], d);
		}
	}
	for (unsigned st = 0; st < MAX_TEXTURE_STAGES; ++st)
		for (unsigned i = 0; i < 32; ++i) {
			if (!DX8Wrapper::GxTruthTSSKnown[st][i])
				continue;
			DWORD d = 0;
			if (dev->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)i, &d) == D3D_OK && d != DX8Wrapper::GxTruthTSS[st][i]) {
				++s_c9TruthTss[st][i]; ++diffs;
				snprintf(name, sizeof(name), "TRUTH_TSS%u.%u", st, i);
				gxC9Report(name, DX8Wrapper::GxTruthTSS[st][i], d);
			}
		}
	for (int k = 0; k < kGxFfRsN; ++k) {
		const unsigned i = kGxFfRs[k];
		if (DX8Wrapper::GxTruthRSKnown[i])
			continue;
		DWORD d = 0;
		if (dev->GetRenderState((D3DRENDERSTATETYPE)i, &d) != D3D_OK)
			continue;
		GxSentinelSeen &e = s_sentRs[i];
		if (e.n++ == 0) e.first = d; else if (d != e.first) e.varies = true;
	}
	for (unsigned st = 0; st < 2; ++st)
		for (int k = 0; k < kGxFfTssN; ++k) {
			const unsigned i = kGxFfTss[k];
			if (DX8Wrapper::GxTruthTSSKnown[st][i])
				continue;
			DWORD d = 0;
			if (dev->GetTextureStageState(st, (D3DTEXTURESTAGESTATETYPE)i, &d) != D3D_OK)
				continue;
			GxSentinelSeen &e = s_sentTss[st][i];
			if (e.n++ == 0) e.first = d; else if (d != e.first) e.varies = true;
		}
	DWORD fvf = 0;
	// Not on the sorting path: Draw_Sorting_IB_VB binds its dynamic VB's FVF directly (dx8wrapper.cpp), past Vertex_Shader, and a
	// sorted draw is never L2's (E3)
	if (!sortingPath && dev->GetVertexShader(&fvf) == D3D_OK && fvf != v.fvf) {
		++s_c9Fvf; ++diffs;
		gxC9Report("FVF", v.fvf, fvf);
	}
	D3DMATERIAL8 dm;
	if (v.materialValid && dev->GetMaterial(&dm) == D3D_OK && memcmp(&dm, v.material, sizeof(dm)) != 0) {
		++s_c9Mat; ++diffs;
		gxC9Report("MATERIAL", 0, 0);
	}
	D3DMATRIX m;
	if (v.projectionValid && dev->GetTransform(D3DTS_PROJECTION, &m) == D3D_OK && memcmp(&m, v.projection, sizeof(m)) != 0) {
		++s_c9Proj; ++diffs;
		gxC9Report("PROJECTION", 0, 0);
	}
	for (unsigned st = 0; st < 8; ++st)
		if ((v.texmValid >> st) & 1u) {
			if (dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + st), &m) == D3D_OK
				&& memcmp(&m, &v.texm[st], sizeof(m)) != 0) {
				++s_c9Texm[st]; ++diffs;
				snprintf(name, sizeof(name), "TEXM%u", st);
				gxC9Report(name, 0, 0);
			}
		}
	// WORLD/VIEW only while the cache holds them (dx8wrapper.h GxXformValid): a zeroed cache is "unknown", not a diff
	if ((v.xformValid & 1u) && dev->GetTransform(D3DTS_WORLD, &m) == D3D_OK && memcmp(&m, &v.transforms[D3DTS_WORLD], sizeof(m)) != 0) {
		++s_c9World; ++diffs;
		gxC9Report("WORLD", 0, 0);
	}
	if ((v.xformValid & 2u) && dev->GetTransform(D3DTS_VIEW, &m) == D3D_OK && memcmp(&m, &v.transforms[D3DTS_VIEW], sizeof(m)) != 0) {
		++s_c9View; ++diffs;
		gxC9Report("VIEW", 0, 0);
		if (s_c9View <= 3) {	// a real VIEW mismatch (the zeroed-cache one is excluded above): show both matrices
			const float *a = (const float *)&v.transforms[D3DTS_VIEW], *b = (const float *)&m;
			fprintf(stderr, "[GXL2C9V] f=%u flush=%s#%d cache=", WW3D::Get_Frame_Count(), kGxFlushSiteName[GxFlushSite], s_flushIndex);
			for (int k = 0; k < 16; ++k) fprintf(stderr, "%s%.6g", k ? "," : "", a[k]);
			fprintf(stderr, " device=");
			for (int k = 0; k < 16; ++k) fprintf(stderr, "%s%.6g", k ? "," : "", b[k]);
			fprintf(stderr, "\n");
		}
	}
	// the never-zeroed VIEW snapshot the L2 capture uses (dx8wrapper.h GxViewSent): any view written behind the wrapper shows here
	if (v.viewSentValid && dev->GetTransform(D3DTS_VIEW, &m) == D3D_OK && memcmp(&m, v.viewSent, sizeof(m)) != 0) {
		++s_c9ViewSent; ++diffs;
		gxC9Report("VIEWSENT", 0, 0);
	}
	++s_c9Samples;
	s_c9DiffSamples += diffs != 0;
}

// Every classic TASK is judged at its OWN device draw: its entry is recorded before its setters, so the
// first non-sorting device draw after it, while it is the newest entry, is its draw. There: static rules
// (E1, E3, E5-E9), E2 and E4 (GxL2Exec.h). C2 compares L2's prediction with the bin key's snapshot, C1 the
// run's packet, C4 the packet's flush tag. Dry: L2 draws nothing. Mutants: 1 (C2), 7 (C1), 8 (C4).
static const gxl2::Policy s_policy;	// phase 1a: no skins, tie epsilon 0 (classification)
static gxl2::Policy s_fpolicy;		// the flush's: tieEpsilon from its camera (Task 4, gxl2x::tieEps)
static const gxl2x::SlotList kGxSlots = { kGxFfRs, kGxFfRsN, kGxFfTss, kGxFfTssN };
static unsigned long long s_flushSerial = 0;
static size_t s_lastDrawn = (size_t)-1;	// the newest s_exec entry whose first draw was taken (any kind)
// ---- Task 4 (v4; CONTRACT_L2 §3.3, §4.3 C3): the flush's DRAWING sequence in classic order - every task's own draw and
// each procedural / delayed / decal entry's first draw - with conservative world bounds and screen rectangles; Mode B packets planned over
// it with those bounds; C3 = validateEmission over the whole emitted sequence. Mutants: 2 = each bin's members emitted in reverse (C3),
// 22 = every run of a flush merged without the planner's test (C3), 23 = a PLANTED OVERLAP: every unclaimed draw sensitive and full
// screen, so no merge may cross one and packetsB_bounded must equal the no-bounds packetsB.
static std::vector<gxl2::SeqDraw> s_seq;
static std::vector<gxl2::BinKey> s_seqKey;			// claimed entries: their bin key
static std::vector<std::pair<int, int>> s_runSeq;	// per run: first / last s_seq index (its claimed draws are contiguous)
static float s_vp[16];
static bool s_vpSet = false;
static unsigned long long s_xOrder = 0, s_xOrderFlushes = 0, s_xPacketsBB = 0, s_xPacketsBU = 0;	// BU: no-bounds packetsB on the same sampled flushes
static int s_modeBEvery = 8;	// GX_L2_MODEB_EVERY: Mode B + C3 on every Nth SCENE frame (~10 ns per pair test, millions per big flush)
// A scene frame = a WW3D frame with a flush at a claimable site. Counting WW3D frames could phase-lock: a projected-shadow texture's
// own End_Render advances the WW3D counter once more per displayed frame, so "frame % 8" might only ever land on shadow frames.
static unsigned s_sceneFrames = 0, s_lastSceneFrame = ~0u;
static bool s_sampleScene = false;
static const float kGxNdcGuard = 0.004f;	// ~2 px at 1080 lines: the contract's +1 px, doubled
// Trade-off: a fixed NDC guard; derive it from the viewport if a target smaller than ~500 lines ever runs the check

// World AABB of a mesh by pure reads: the model's object-space box (static data) through the mesh transform the classic draw has just
// validated - never RenderObjClass::Get_Bounding_Box, which recomputes (WRITES) its cache when stale. Skins (bind-pose box) and
// camera-aligned / oriented meshes (drawn with Obj_Look_At, not the transform) are unbounded: conservative.
static gxl2::Aabb gxWorldBounds(MeshClass *mesh)
{
	gxl2::Aabb b = { gxl2::Vec3{ -FLT_MAX, -FLT_MAX, -FLT_MAX }, gxl2::Vec3{ FLT_MAX, FLT_MAX, FLT_MAX } };
	MeshModelClass *mdl = mesh ? mesh->Peek_Model() : nullptr;
	if (mdl == nullptr || mdl->Get_Flag(MeshModelClass::SKIN) || mdl->Get_Flag(MeshModelClass::ALIGNED) || mdl->Get_Flag(MeshModelClass::ORIENTED))
		return b;
	AABoxClass ob;
	mdl->Get_Bounding_Box(&ob);
	const Matrix3D &tm = mesh->Get_Transform();
	const float o[3] = { ob.Center.X, ob.Center.Y, ob.Center.Z }, e[3] = { ob.Extent.X, ob.Extent.Y, ob.Extent.Z };
	float lo[3], hi[3];
	for (int r = 0; r < 3; ++r) {
		const float c = tm[r][0] * o[0] + tm[r][1] * o[1] + tm[r][2] * o[2] + tm[r][3];
		const float x = fabsf(tm[r][0]) * e[0] + fabsf(tm[r][1]) * e[1] + fabsf(tm[r][2]) * e[2];
		lo[r] = c - x;
		hi[r] = c + x;
	}
	b.lo = gxl2::Vec3{ lo[0], lo[1], lo[2] };
	b.hi = gxl2::Vec3{ hi[0], hi[1], hi[2] };
	return b;
}

// The ACTIVE depth buffer's precision in bits (Find_Z_Mode may fall back to D16 / D15S1), read from the device once per WW3D frame
// (check mode only: the read drains the d3d8 ring). D32 counts as 24: a float depth's step near 1.0 is 2^-24. Unknown: 0 = no reorder.
static int gxDepthBits()
{
	static unsigned s_frame = ~0u;
	static int s_bits = 0;
	if (WW3D::Get_Frame_Count() == s_frame)
		return s_bits;
	s_frame = WW3D::Get_Frame_Count();
	s_bits = 0;
	IDirect3DSurface8 *ds = nullptr;
	if (DX8Wrapper::_Get_D3D_Device8()->GetDepthStencilSurface(&ds) == D3D_OK && ds != nullptr) {
		D3DSURFACE_DESC desc;
		if (ds->GetDesc(&desc) == D3D_OK) {
			switch (desc.Format) {
			case D3DFMT_D16_LOCKABLE: case D3DFMT_D16: s_bits = 16; break;
			case D3DFMT_D15S1: s_bits = 15; break;
			case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32: s_bits = 24; break;
			default: break;
			}
		}
		ds->Release();
	}
	return s_bits;
}

// The view x projection the device received (row-vector D3D convention), taken at the flush's first draw.
static void gxSetViewProj()
{
	s_vpSet = false;
	const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
	if (!v.viewSentValid || !v.projectionValid)
		return;
	const float *a = (const float *)v.viewSent, *b = (const float *)v.projection;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			s_vp[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
	s_vpSet = true;
}

static int gxSeqAdd(const gxl2::TaskState &t, bool claimed, bool sensitive, bool biased, unsigned colorMask)
{
	gxl2::SeqDraw sd;
	sd.task = t;
	sd.claimed = claimed;
	sd.sensitive = sensitive;
	sd.biased = biased;
	sd.colorMask = (uint8_t)colorMask;
	if (s_vpSet)
		gxl2x::ndcRect(s_vp, t.bounds, kGxNdcGuard, sd.screen);
	else
		sd.screen[0] = sd.screen[1] = -2.0f, sd.screen[2] = sd.screen[3] = 2.0f;	// no view: full screen
	s_seq.push_back(sd);
	s_seqKey.push_back(gxl2::BinKey());
	return (int)s_seq.size() - 1;
}

// A procedural / delayed / decal entry's first draw: always sensitive (§3.3), a decal also biased (ZBIAS).
static void gxSeqOther(const GxClassicDraw &d)
{
	MeshClass *mesh = d.kind == GxClassicDraw::DECAL ? (d.decal ? d.decal->Peek_Parent() : nullptr) : d.mesh;
	gxl2::TaskState t;
	t.mesh = mesh;
	t.object = mesh;
	t.bounds = gxWorldBounds(mesh);
	gxSeqAdd(t, false, true, d.kind == GxClassicDraw::DECAL, 0xF);
}

// Mode B (§3.3): runs in classic order; a packet absorbs the next run iff every claimed draw already in it commutes with every draw it
// would newly pass (those between its current emission point and that run). Emission: unclaimed draws in place, each packet's draws at its
// last run's end, planBins over them. Returns the packet count; fills the emitted order.
static int gxModeB(std::vector<int> &emitted)
{
	const float eps = s_fpolicy.tieEpsilon;
	std::vector<std::vector<int>> packets;
	int emitAt = -1;	// the open packet's emission point (its last run's last draw)
	for (int r = 0; r < (int)s_runSeq.size(); ++r) {
		const int a = s_runSeq[r].first;
		bool merge = !packets.empty();
		if (merge && s_mutant != 22) {
			for (int q : packets.back()) {
				for (int x = s_runSeq[q].first; x <= s_runSeq[q].second && merge; ++x)
					for (int y = emitAt + 1; y < a && merge; ++y) {
						gxl2::SeqDraw ys = s_seq[y];
						if (s_mutant == 23 && !ys.claimed) {	// the planted overlap
							ys.sensitive = true;
							ys.screen[0] = ys.screen[1] = -2.0f; ys.screen[2] = ys.screen[3] = 2.0f;
						}
						if (!gxl2::commutes(s_seq[x], ys, eps)) merge = false;
					}
				if (!merge) break;
			}
		}
		if (merge) packets.back().push_back(r);
		else packets.push_back(std::vector<int>(1, r));
		emitAt = s_runSeq[r].second;
	}
	std::vector<int> packetAt(s_seq.size(), -1);	// seq index -> the packet emitted right after it
	for (int pk = 0; pk < (int)packets.size(); ++pk)
		packetAt[s_runSeq[packets[pk].back()].second] = pk;
	emitted.clear();
	std::vector<gxl2::ClaimedDraw> in;
	for (int k = 0; k < (int)s_seq.size(); ++k) {
		if (!s_seq[k].claimed) emitted.push_back(k);
		if (packetAt[k] < 0) continue;
		in.clear();
		for (int r : packets[packetAt[k]])
			for (int x = s_runSeq[r].first; x <= s_runSeq[r].second; ++x) {
				gxl2::ClaimedDraw cd;
				cd.key = s_seqKey[x];
				cd.task = s_seq[x].task;
				cd.classicSeq = x;
				in.push_back(cd);
			}
		for (const gxl2::Bin &b : gxl2::planBins(in, s_fpolicy)) {
			if (s_mutant == 2)	// M2: a bin's members in reverse
				for (int j = (int)b.draws.size() - 1; j >= 0; --j) emitted.push_back(in[b.draws[j]].classicSeq);
			else
				for (int j : b.draws) emitted.push_back(in[j].classicSeq);
		}
	}
	return (int)packets.size();
}
static std::vector<gxl2::Reason> s_xreason;		// per s_exec entry (kGxL2NotTask: not a TASK, or not judged yet)
static GxL2FlushPlan s_xplan;					// the flush-start static plan over s_list (= s_exec, gate K)
static std::unordered_map<const void *, int> s_meshTasks, s_procPasses;
static gxl2::FlushCtx s_fctx;
// One task's lights as Set_Light_Environment sends them - a VALUE: M1 keeps the bin's first one, whose mesh (and its embedded
// m_localLightEnv) its render task may release before the next draw.
struct GxLights { int n; D3DLIGHT8 l[4]; unsigned ambient; };
struct GxKeySnap { gxl2::BinKey key; GxDrawState snap; GxLights lights; };	// lights: M1's - the key's first task in the run
static void gxLightsOf(const LightEnvironmentClass &le, GxLights &o)
{
	o.n = le.Get_Light_Count() < 4 ? le.Get_Light_Count() : 4;
	for (int l = 0; l < o.n; ++l)
		GxL2Check_Light(le, l, o.l[l]);
	o.ambient = DX8Wrapper::Convert_Color(le.Get_Equivalent_Ambient(), 0.0f);	// Set_Light_Environment
}
static struct GxRunState
{
	bool open = false;
	size_t first = 0, last = 0;
	std::vector<gxl2::ClaimedDraw> claimed;
	std::vector<GxKeySnap> keys;
} s_run;
static std::vector<GxL2Run> s_xruns;
static GxDrawState s_cap, s_pred;
static unsigned long long s_xTasks = 0, s_xClaimed = 0, s_xRuns = 0, s_xBins = 0, s_xCov = 0, s_xState = 0, s_xBound = 0, s_xNoDraw = 0;
static int s_xPrinted = 0;
static const int kGxXPrintMax = 40;
static unsigned long long s_cmask[16];	// COLORWRITEENABLE (low 4 bits) of every task refused R_COLOR_MASK

// GX_L2_MODEL=1 (GX_L2_CHECK=1 only).
// Every flush of a modelled scene frame is logged (the `=2` call log, dx8wrapper.h GxLogOn) from its start to its close point (after
// the null-buffer setters, dx8renderer.h GxL2FlushClosedHook), every device draw in it captured (GxDrawState), and the log replayed
// through the device-less model (GxL2Model.h) seeded from the flush-start snapshot: MODEL_CALL = the model's predicted invocations vs
// the logged ones, MODEL = the model's device vs the capture at each draw (model_cache: the fields the wrapper's caches / logical
// state hold, not the device). Off, nothing here runs.
static bool s_model = false;
static int s_modelEvery = 8;					// GX_L2_MODEL_EVERY (default: the Mode B cadence)
static bool s_modelScene = false;				// the current scene frame is modelled
static bool s_logOpen = false;
static gxlog::Log s_log;
static gxlog::Snapshot *s_snap = nullptr;		// heap, once: large
static gxl2m::State s_mstate, s_mscratch;		// the replay's model; shader_stale_draws' scratch copy
static gxl2m::State s_mstateB;					// T4 C5: the boundary-replay arm (CONTRACT_L2 §3.4 under DrawRun)
static std::vector<gxl2m::Run> s_c5runs;		// the flush's claimed runs (s_xruns) as C5 replaces them
static std::vector<GxDrawState> s_mcap;			// the modelled flush's device draws, in order (sorting-path draws too)
static std::unordered_set<const void *> s_claimedMeshes;	// meshes with a CLAIMED task this WW3D frame (procpass_claimed_xflush)
static unsigned s_claimedFrame = ~0u;
static unsigned long long s_mFlushes = 0, s_mDraws = 0, s_mRecords = 0, s_mSends = 0, s_mCall = 0, s_mState = 0, s_mCache = 0,
	s_mUnknown = 0, s_mSorting = 0, s_mProcX = 0, s_mStale = 0, s_mSortq = 0, s_mSortqDiff = 0, s_mIdxUnknown = 0;
static unsigned s_mPrintFrame = ~0u;
static int s_mCallPrinted = 0, s_mStatePrinted = 0, s_mUnknownPrinted = 0;
static unsigned long long s_c5Runs = 0, s_c5Scaled = 0, s_c5Compares = 0, s_c5Mism = 0, s_c5Unsourced = 0, s_c5Unstable = 0, s_c5BadRanges = 0;
static int s_c5Printed = 0;
static_assert(gxl2m::kEntryTask == (uint32_t)GxClassicDraw::TASK, "the C5 run ranges match TASK entries");

static void gxClaimedFrame()
{
	if (WW3D::Get_Frame_Count() != s_claimedFrame) {
		s_claimedFrame = WW3D::Get_Frame_Count();
		s_claimedMeshes.clear();
	}
}

static_assert(gxl2x::RS_ZENABLE == D3DRS_ZENABLE && gxl2x::RS_ZWRITEENABLE == D3DRS_ZWRITEENABLE && gxl2x::RS_SRCBLEND == D3DRS_SRCBLEND
	&& gxl2x::RS_DESTBLEND == D3DRS_DESTBLEND && gxl2x::RS_ZFUNC == D3DRS_ZFUNC && gxl2x::RS_ALPHABLENDENABLE == D3DRS_ALPHABLENDENABLE
	&& gxl2x::RS_ZBIAS == D3DRS_ZBIAS && gxl2x::RS_STENCILENABLE == D3DRS_STENCILENABLE && gxl2x::RS_STENCILZFAIL == D3DRS_STENCILZFAIL
	&& gxl2x::RS_STENCILPASS == D3DRS_STENCILPASS && gxl2x::RS_STENCILFUNC == D3DRS_STENCILFUNC
	&& gxl2x::RS_CLIPPLANEENABLE == D3DRS_CLIPPLANEENABLE && gxl2x::RS_COLORWRITEENABLE == D3DRS_COLORWRITEENABLE
	&& gxl2x::RS_AMBIENT == D3DRS_AMBIENT
	&& gxl2x::BLEND_ZERO == D3DBLEND_ZERO && gxl2x::BLEND_ONE == D3DBLEND_ONE && gxl2x::CMP_LESS == D3DCMP_LESS
	&& gxl2x::CMP_LESSEQUAL == D3DCMP_LESSEQUAL && gxl2x::CMP_ALWAYS == D3DCMP_ALWAYS && gxl2x::STENCILOP_KEEP == D3DSTENCILOP_KEEP
	&& gxl2x::STENCILOP_REPLACE == D3DSTENCILOP_REPLACE && gxl2x::TSS_COLOROP == D3DTSS_COLOROP && gxl2x::TSS_ALPHAOP == D3DTSS_ALPHAOP
	&& gxl2x::TOP_DISABLE == D3DTOP_DISABLE && gxl2x::kStages <= MAX_TEXTURE_STAGES, "GxL2Exec.h's D3D8 numbers");

static void gxXReport(const char *kind, size_t seq, const char *field, unsigned classic, unsigned l2)
{
	if (s_xPrinted++ >= kGxXPrintMax)
		return;
	fprintf(stderr, "[GXL2CHK] f=%u flush=%s#%d seq=%zu kind=%s field=%s classic=0x%08X l2=0x%08X\n", WW3D::Get_Frame_Count(),
		kGxFlushSiteName[GxFlushSite], s_flushIndex, seq, kind, field, classic, l2);
}

// L2's own prediction of a claimed task's draw state: its key's snapshot + the per-instance data L2 computes itself.
// A task's pose as the classic draw reads it - the mesh transform and its identity flag (GxL2Dry_FillTask leaves both to the caller):
// gxL2TaskDraw after the classic draw has validated the transform, the S1 seat at its run's position.
static void gxL2TaskPose(MeshClass *mesh, gxl2::TaskState &t)
{
	const Matrix3D &tm = mesh->Get_Transform();
	for (int r = 0; r < 3; ++r)
		for (int k = 0; k < 4; ++k)
			t.transform.m[r][k] = tm[r][k];
	t.transformIdentity = mesh->Is_Transform_Identity();
}

// The world matrix the device receives for a task: the resolved recipe (identity / billboard look-at / the mesh transform),
// as Set_Transform(D3DTS_WORLD, Matrix3D) stores and sends it (transposed). The C2 predictor's (C2 = 0 over 37.6 M claimed draws)
// and the S1 seat's packet world - one function. t needs its pose (gxL2TaskPose).
static Matrix4x4 gxL2World(MeshClass *mesh, const gxl2::TaskState &t)
{
	const gxl2::WorldRecipe w = gxl2::resolveWorld(t, s_fctx);
	Matrix4x4 world;
	if (w.mode == gxl2::WORLD_IDENTITY) {
		world.Make_Identity();
	} else if (w.mode == gxl2::WORLD_LOOKAT) {
		Matrix3D tmp;
		tmp.Obj_Look_At(Vector3(w.p.x, w.p.y, w.p.z), Vector3(w.t.x, w.t.y, w.t.z), 0.0f);
		world = Matrix4x4(tmp).Transpose();
	} else {
		world = Matrix4x4(mesh->Get_Transform()).Transpose();	// Set_Transform(D3DTS_WORLD, Matrix3D) (dx8wrapper.h)
	}
	return world;
}

static void gxL2Predict(const GxDrawState &snap, const GxClassicDraw &d, const gxl2::TaskState &t, const GxLights &lights,
	GxDrawState &p)
{
	p = snap;
	const Matrix4x4 world = gxL2World(d.mesh, t);
	memcpy(p.world, &world, sizeof(p.world));
	p.worldKnown = true;
	for (int l = 0; l < 4; ++l) {
		p.lightOn[l] = l < lights.n;
		if (l < lights.n)
			p.light[l] = lights.l[l];
	}
	p.rs[D3DRS_AMBIENT] = lights.ambient;
	p.rsKnown[D3DRS_AMBIENT] = true;
	p.indexBase = (unsigned)d.mesh->Get_Base_Vertex_Offset();
	p.baseVertex = p.indexBase + snap.vbaOffset;
	p.prim = D3DPT_TRIANGLELIST;	// ENABLE_STRIPING is off (E3 R_STRIP)
	// Render -> Draw_Triangles -> Draw take unsigned shorts, and Draw replaces a range below 3 by the whole buffer past the base
	// (dx8wrapper.cpp Draw) - mirrored, so the prediction is the parameters the device receives
	unsigned short minIndex = (unsigned short)d.pr->Get_Min_Vertex_Index(), numVerts = (unsigned short)d.pr->Get_Vertex_Index_Range();
	if (numVerts < 3) {
		minIndex = 0;
		const VertexBufferClass *vb = (const VertexBufferClass *)snap.vb;
		if (vb != nullptr && (vb->Type() == BUFFER_TYPE_DX8 || vb->Type() == BUFFER_TYPE_SORTING))
			numVerts = (unsigned short)(vb->Get_Vertex_Count() - p.indexBase - snap.vbaOffset - minIndex);
	}
	p.startIndex = (unsigned short)d.pr->Get_Index_Offset() + snap.ibaOffset;
	p.primCount = (unsigned short)(d.pr->Get_Index_Count() / 3);
	p.minIndex = minIndex;
	p.numVerts = numVerts;
}

// The draw-time half's inputs read from the device truth as it stands (E2 known slots, ZBIAS / clip plane, E4's effective shader, the
// state id): gxL2TaskDraw at a classic draw, the S1 seat after its PREPARE (D2).
static void gxL2TruthCtx(gxl2::FlushCtx &f, gxl2::CatState &c)
{
	f.staticStateKnown = gxl2x::e2Known(DX8Wrapper::GxTruthRSKnown, DX8Wrapper::GxTruthTSS, DX8Wrapper::GxTruthTSSKnown, kGxSlots);
	f.zbias = DX8Wrapper::GxTruthRS[D3DRS_ZBIAS];
	f.clipPlaneEnable = DX8Wrapper::GxTruthRS[D3DRS_CLIPPLANEENABLE];
	c.shader = gxl2x::e4Decode(DX8Wrapper::GxTruthRS, c.shader.bits);
	c.effectiveState = gxl2x::effectiveId(DX8Wrapper::GxTruthRS, DX8Wrapper::GxTruthTSS, DX8Wrapper::GxTruthTSSKnown, kGxSlots);
}

static void gxL2CloseRun()
{
	if (!s_run.open)
		return;
	s_run.open = false;
	++s_xRuns;
	const std::vector<gxl2::Bin> bins = gxl2::planBins(s_run.claimed, s_fpolicy);
	s_xBins += bins.size();
	// C1: the packet's draws, one-to-one with the run's claimed entries
	std::vector<int> want, got;
	for (const gxl2::ClaimedDraw &cd : s_run.claimed) want.push_back(cd.classicSeq);
	for (const gxl2::Bin &b : bins)
		for (int j : b.draws) got.push_back(s_run.claimed[j].classicSeq);
	if (s_mutant == 7 && !got.empty())
		got.erase(std::find(got.begin(), got.end(), want.back()));	// M7: the run's last task dropped from its packet
	std::sort(got.begin(), got.end());
	if (got != want) {
		++s_xCov;
		size_t k = 0;
		while (k < got.size() && k < want.size() && got[k] == want[k]) ++k;
		gxXReport("COVERAGE", k < want.size() ? (size_t)want[k] : (size_t)got[k], got.size() < want.size() ? "missing" : "extra_or_dup",
			(unsigned)want.size(), (unsigned)got.size());
	}
	// C4: the packet's flush tag vs its classic twins' flush
	const unsigned long long tag = s_mutant == 8 ? s_flushSerial - 1 : s_flushSerial;
	if (tag != s_flushSerial) {
		++s_xBound;
		gxXReport("BOUNDARY", s_run.first, "flush", (unsigned)s_flushSerial, (unsigned)tag);
	}
	GxL2Run r;
	r.first = (int)s_run.first;
	r.last = (int)s_run.last;
	r.keys = (int)s_run.keys.size();
	r.bins = (int)bins.size();
	s_xruns.push_back(r);
}

static void gxL2SetReason(size_t idx, gxl2::Reason r)
{
	if (s_xreason.size() <= idx)
		s_xreason.resize(idx + 1, kGxL2NotTask);
	s_xreason[idx] = r;
}

static void gxL2TaskDraw(size_t idx, unsigned prim, unsigned minIndex, unsigned numVerts, unsigned startIndex, unsigned primCount)
{
	const GxClassicDraw d = s_exec[idx];	// a copy: s_exec may grow
	gxl2::CatState c;
	gxl2::TaskState t;
	GxL2Dry_FillCat(d.cat, d.pr, c);
	GxL2Dry_FillTask(d.mesh, d.pr, d.batch, t);
	const auto pp = s_procPasses.find(d.mesh);
	t.proceduralPasses = pp == s_procPasses.end() ? 0 : pp->second;
	const auto mt = s_meshTasks.find(d.mesh);
	t.onlyVisibleTask = mt != s_meshTasks.end() && mt->second == 1;
	gxL2TaskPose(d.mesh, t);	// the classic draw has just validated the transform
	gxl2::FlushCtx f = s_fctx;
	gxL2TruthCtx(f, c);
	const unsigned cmask = c.shader.colorMask;	// RGB or RGBA passes E4, the mask is in the key
	const gxl2::Reason reason = gxl2::classifyTask(f, c, t, s_policy);
	gxL2SetReason(idx, reason);
	++s_xTasks;
	if (reason == gxl2::R_COLOR_MASK)
		++s_cmask[cmask];
	t.bounds = gxWorldBounds(d.mesh);	// Task 4: real bounds (planBins, Mode B, C3)
	const bool e4ok = f.staticStateKnown && c.shader.blendOff && c.shader.zEnable && c.shader.depthWrite && c.shader.depthCmpLess
		&& c.shader.colorWrite && c.shader.stencilProfileOk;
	const int seq = gxSeqAdd(t, reason == gxl2::OK, !e4ok, f.zbias != 0, cmask);	// sensitive = fails E4 (§3.3)
	if (reason != gxl2::OK)
		return;
	++s_xClaimed;
	if (s_model) {	// procpass_claimed_xflush's set
		gxClaimedFrame();
		s_claimedMeshes.insert(d.mesh);
	}
	if (!(s_run.open && idx == s_run.last + 1 && s_exec[s_run.last].cat == d.cat)) {
		gxL2CloseRun();
		s_run.open = true;
		s_run.first = idx;
		s_run.claimed.clear();
		s_run.keys.clear();
		s_runSeq.push_back(std::make_pair(seq, seq));
	}
	s_run.last = idx;
	s_runSeq.back().second = seq;
	gxl2::ClaimedDraw cd;
	cd.key = gxl2::binKey(c, t);
	s_seqKey[seq] = cd.key;
	cd.task = t;
	cd.classicSeq = (int)idx;
	s_run.claimed.push_back(cd);
	GxL2Check_Capture(s_cap, prim, minIndex, numVerts, startIndex, primCount);
	GxLights lights;
	gxLightsOf(*d.mesh->Get_Lighting_Environment(), lights);	// non-null: E7
	const GxKeySnap *ks = nullptr;
	for (const GxKeySnap &k : s_run.keys)
		if (k.key == cd.key) { ks = &k; break; }
	if (ks == nullptr) {
		s_run.keys.push_back(GxKeySnap{ cd.key, s_cap, lights });
		ks = &s_run.keys.back();
	}
	const GxDrawState *snap = &ks->snap;
	// M1: the lights of the key's FIRST task in the run - every bin of a key starts there or later, so this is at least as wrong as the
	// bin's first task for every draw (with bounds, planBins can bin A,B,A as A[0,2] B[1])
	gxL2Predict(*snap, d, t, s_mutant == 1 ? ks->lights : lights, s_pred);
	char prefix[160];
	snprintf(prefix, sizeof(prefix), "[GXL2CHK] f=%u flush=%s#%d seq=%zu mesh=%s", WW3D::Get_Frame_Count(), kGxFlushSiteName[GxFlushSite],
		s_flushIndex, idx, d.mesh->Get_Name());
	const int budget = s_xPrinted < kGxXPrintMax ? kGxXPrintMax - s_xPrinted : 0;
	const int n = GxL2Check_CompareStateM(s_cap, s_pred, prefix, budget, s_mutant);
	if (n) {
		++s_xState;
		s_xPrinted += n < budget ? n : budget;
	}
}

static void gxL2ExecStart()
{
	++s_flushSerial;
	s_lastDrawn = (size_t)-1;
	s_seq.clear();
	s_seqKey.clear();
	s_runSeq.clear();
	s_vpSet = false;
	s_fpolicy = s_policy;
	if (CameraClass *cam = TheDX8MeshRenderer.Peek_Camera()) {
		float zn = 0.0f, zf = 0.0f;
		cam->Get_Clip_Planes(zn, zf);
		s_fpolicy.tieEpsilon = gxl2x::tieEps(zn, zf, gxDepthBits());
	} else {
		s_fpolicy.tieEpsilon = FLT_MAX;
	}
	s_xreason.clear();
	s_xruns.clear();
	s_run.open = false;
	s_meshTasks.clear();
	s_procPasses.clear();
	for (const GxClassicDraw &d : s_list) {
		if (d.kind == GxClassicDraw::TASK) ++s_meshTasks[d.mesh];
		else if (d.kind == GxClassicDraw::MATPASS || d.kind == GxClassicDraw::DELAYED_MATPASS) ++s_procPasses[d.mesh];
	}
	GxL2Dry_FillFlush(GxFlushSite, s_fctx);
	// the scene-frame cadence: a claimable site in the default, non-mirrored pass (the water reflection also flushes at claimable sites,
	// with inverted culling, and has its own End_Render)
	const gxl2::PassCtx &pc = s_fctx.pass;
	if (gxl2::siteClaimable((gxl2::FlushSite)GxFlushSite) && pc.defaultPass && pc.extraPassDisabled && !pc.cullingInverted
		&& pc.texturingEnabled && !pc.coloringEnabled && !pc.debugSwitches && WW3D::Get_Frame_Count() != s_lastSceneFrame) {
		s_lastSceneFrame = WW3D::Get_Frame_Count();
		const unsigned sf = s_sceneFrames++;
		s_sampleScene = sf % (unsigned)s_modeBEvery == 0;
		s_modelScene = s_model && sf % (unsigned)s_modelEvery == 0;	// its own modulus
	}
	if (CameraClass *cam = TheDX8MeshRenderer.Peek_Camera()) {
		const Matrix3D &ct = cam->Get_Transform();
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 4; ++k)
				s_fctx.camera.m[r][k] = ct[r][k];
	}
}

static void gxL2ExecEnd(unsigned frame)
{
	gxL2CloseRun();
	if (s_xreason.size() < s_exec.size())
		s_xreason.resize(s_exec.size(), kGxL2NotTask);
	for (size_t i = 0; i < s_exec.size(); ++i) {
		if (s_exec[i].kind != GxClassicDraw::TASK || s_xreason[i] != kGxL2NotTask)
			continue;
		// no draw: the static plan's reason (pointers only - by now the mesh may be gone)
		const gxl2::Reason r = i < s_xplan.reason.size() ? s_xplan.reason[i] : gxl2::R_SITE;
		s_xreason[i] = r;
		++s_xTasks;
		const bool staticPass = r == gxl2::OK || r == gxl2::R_STATE_UNKNOWN || r == gxl2::R_AMBIENT || r == gxl2::R_BLENDED
			|| r == gxl2::R_NO_DEPTH_WRITE || r == gxl2::R_DEPTH_FUNC || r == gxl2::R_COLOR_MASK;	// only draw-time rules left
		if (staticPass) {
			++s_xNoDraw;
			++s_xCov;	// a missing draw is a COVERAGE mismatch in every total
			gxXReport("COVERAGE", i, "nodraw", (unsigned)r, 0);
		}
	}
	static std::vector<int> emitted;
	int packetsBB = -1;	// -1: not a sampled frame (the census divides by sampled frames)
	if (frame == s_lastSceneFrame && s_sampleScene) {	// every flush of a sampled scene frame
		packetsBB = gxModeB(emitted);
		s_xPacketsBB += packetsBB;
		s_xPacketsBU += GxL2Dry_PacketsB(s_exec, s_xreason, s_xruns);	// M23's exact comparison
		const int bad = gxl2::validateEmission(s_seq, emitted, s_fpolicy.tieEpsilon);	// C3, independent of gxModeB's own test
		if (bad != 0) {
			++s_xOrderFlushes;
			s_xOrder += bad > 0 ? bad : 1;
			gxXReport("ORDER", 0, bad < 0 ? "not_a_permutation" : "inverted_pairs", (unsigned)bad, (unsigned)s_seq.size());
		}
	}
	if (s_census) {
		static GxL2FlushPlan p;
		p.site = (gxl2::FlushSite)GxFlushSite;
		p.packetsBBounded = packetsBB;
		p.reason = s_xreason;
		p.runs = s_xruns;
		p.packetsB = GxL2Dry_PacketsB(s_exec, p.reason, p.runs);
		p.multiTaskMeshes = s_xplan.multiTaskMeshes;
		p.multiTaskTasks = s_xplan.multiTaskTasks;
		GxL2Dry_RunCounters(s_exec, p.reason, s_xplan.taskFlags, p);	// s_list = s_exec entry for entry (gate K)
		GxL2Dry_CensusExec(p, frame);
	}
}

static void gxL2ExecPrint()
{
	char model[512] = "", c5[256] = "";	// the model's counters, GX_L2_MODEL=1 only
	if (s_model)
		snprintf(c5, sizeof(c5), " invariance=%llu c5_runs=%llu c5_scaled_runs=%llu c5_compares=%llu c5_unsourced=%llu payload_stable=%llu "
			"c5_bad_ranges=%llu", s_c5Mism, s_c5Runs, s_c5Scaled, s_c5Compares, s_c5Unsourced, s_c5Unstable, s_c5BadRanges);
	if (s_model)
		snprintf(model, sizeof(model), " model_flushes=%llu model_draws=%llu model_records=%llu model_sends=%llu model_call=%llu model=%llu "
			"model_cache=%llu model_unknown=%llu sorting_draws=%llu procpass_claimed_xflush=%llu shader_stale_draws=%llu model_sortq=%llu model_sortq_diff=%llu model_indices_unknown=%llu",
			s_mFlushes, s_mDraws, s_mRecords, s_mSends, s_mCall, s_mState, s_mCache, s_mUnknown, s_mSorting, s_mProcX, s_mStale, s_mSortq,
			s_mSortqDiff, s_mIdxUnknown);
	fprintf(stderr, "[GXL2CHK] sum frame=%u flushes=%llu tasks=%llu claimed=%llu runs=%llu bins=%llu packetsB_bounded=%llu packetsB_unbounded=%llu mismatch=%llu "
		"by_kind=COVERAGE:%llu,STATE:%llu,BOUNDARY:%llu,ORDER:%llu order_flushes=%llu nodraw=%llu%s%s%s\n", WW3D::Get_Frame_Count(), s_flushSerial,
		s_xTasks, s_xClaimed, s_xRuns, s_xBins, s_xPacketsBB, s_xPacketsBU, s_xCov + s_xState + s_xBound + s_xOrder, s_xCov, s_xState, s_xBound, s_xOrder,
		s_xOrderFlushes, s_xNoDraw, model, c5,
		s_mutant == 1 ? " MUTANT 1" : s_mutant == 7 ? " MUTANT 7" : s_mutant == 8 ? " MUTANT 8" : s_mutant == 2 ? " MUTANT 2"
		: s_mutant == 22 ? " MUTANT 22" : s_mutant == 23 ? " MUTANT 23" : "");
	fprintf(stderr, "[GXL2CHK] color_mask_refusals=");
	for (int k = 0; k < 16; ++k)
		if (s_cmask[k]) fprintf(stderr, "%X:%llu,", k, s_cmask[k]);
	fprintf(stderr, "\n");
	GxL2Check_PrintFieldHist();
}

// The replay hooks, and the log's open / close points.
static void gxMReport(bool cache, unsigned seq, const char *field, unsigned classic, unsigned l2)
{
	if (cache)
		++s_mCache;
	else
		++s_mState;
	if (s_mStatePrinted++ >= 20)
		return;
	fprintf(stderr, "[GXL2CHK] f=%u flush=%s#%d seq=%u kind=MODEL field=%s classic=0x%08X l2=0x%08X\n", WW3D::Get_Frame_Count(),
		kGxFlushSiteName[GxFlushSite], s_flushIndex, seq, field, classic, l2);
}

static_assert(sizeof(gxlog::Material) == sizeof(D3DMATERIAL8) && sizeof(gxlog::Light) == sizeof(D3DLIGHT8)
	&& sizeof(gxlog::Mat4) == sizeof(GxDrawState::world) && gxl2m::kStages == MAX_TEXTURE_STAGES, "C6 compares bytes");

// The model's view at its k-th device draw vs the k-th captured draw, field by field.
static void gxModelDrawn(const gxlog::Record &r, const gxl2m::DrawArgs &a, const gxl2m::State &st, const GxDrawState &c, bool sorting)
{
	const gxl2m::Device &d = st.dev;
	const unsigned seq = r.entry;
	char f[32];
	for (int i = 0; i < 256; ++i)
		if (c.rsKnown[i] && (!d.rsKnown[i] || d.rs[i] != c.rs[i])) {
			snprintf(f, sizeof(f), d.rsKnown[i] ? "RS%d" : "RS%d_UNKNOWN", i);
			gxMReport(false, seq, f, c.rs[i], d.rs[i]);
		}
	for (int s8 = 0; s8 < gxl2m::kStages; ++s8) {
		for (int i = 0; i < 32; ++i)
			if (c.tssKnown[s8][i] && (!d.tssKnown[s8][i] || d.tss[s8][i] != c.tss[s8][i])) {
				snprintf(f, sizeof(f), d.tssKnown[s8][i] ? "TSS%d.%d" : "TSS%d.%d_UNKNOWN", s8, i);
				gxMReport(false, seq, f, c.tss[s8][i], d.tss[s8][i]);
			}
		if ((uint64_t)(uintptr_t)c.tex[s8] != d.tex[s8]) {
			snprintf(f, sizeof(f), "TEX%d", s8);
			gxMReport(false, seq, f, (unsigned)(uintptr_t)c.tex[s8], (unsigned)d.tex[s8]);
		}
		const bool tv = ((d.texmValid >> s8) & 1u) != 0;
		if (c.texmKnown[s8] && (!tv || memcmp(&d.texm[s8], c.texm[s8], sizeof(c.texm[s8])) != 0)) {
			snprintf(f, sizeof(f), "TEXM%d", s8);
			gxMReport(false, seq, f, 1, tv);
		}
	}
	if (c.matKnown && (!d.matValid || memcmp(&d.mat, &c.mat, sizeof(c.mat)) != 0))
		gxMReport(false, seq, "MATERIAL", 1, d.matValid);
	for (int l = 0; l < 4; ++l) {
		if (!c.lightValid[l])
			continue;
		if (c.lightOn[l] != d.lightOn[l]) {
			snprintf(f, sizeof(f), "LIGHT%d_ON", l);
			gxMReport(false, seq, f, c.lightOn[l], d.lightOn[l]);
		} else if (c.lightOn[l] && (!d.lightValid[l] || memcmp(&d.light[l], &c.light[l], sizeof(c.light[l])) != 0)) {
			snprintf(f, sizeof(f), "LIGHT%d", l);
			gxMReport(false, seq, f, 1, d.lightValid[l]);
		}
	}
	if (c.worldKnown && (!(d.xformValid & 1u) || memcmp(&d.world, c.world, sizeof(c.world)) != 0))
		gxMReport(false, seq, "WORLD", 1, d.xformValid & 1u);
	if (c.viewKnown && (!d.viewSentValid || memcmp(&d.viewSent, c.view, sizeof(c.view)) != 0))
		gxMReport(false, seq, "VIEW", 1, d.viewSentValid);
	if (c.projKnown && (!d.projValid || memcmp(&d.proj, c.proj, sizeof(c.proj)) != 0))
		gxMReport(false, seq, "PROJ", 1, d.projValid);
	// The device's SetIndices base vertex: Apply sends index_base_offset + vba_offset, which is the capture's baseVertex after Apply.
	// Device state, so MODEL - where the index offset replayed as a no-op must fire; it showed only in the cache column
	// (INDEXBASE) and MODEL_CALL. A sorting draw binds its own indices raw, so only an indexed draw is compared.
	if (!sorting) {
		if (!d.indicesKnown)
			++s_mIdxUnknown;
		else if (c.baseVertex != d.indexBase)
			gxMReport(false, seq, "BASEVERTEX", c.baseVertex, d.indexBase);
	}
	// cache-derived: the FVF the wrapper's Vertex_Shader holds, and the logical bindings (render_state)
	const gxlog::Logical &rs = st.rs;
	if (c.fvf != st.vsCache) gxMReport(true, seq, "FVF", c.fvf, st.vsCache);
	if ((uint64_t)(uintptr_t)c.vb != rs.vb[0].obj) gxMReport(true, seq, "VB", (unsigned)(uintptr_t)c.vb, (unsigned)rs.vb[0].obj);
	if ((uint64_t)(uintptr_t)c.ib != rs.ib.obj) gxMReport(true, seq, "IB", (unsigned)(uintptr_t)c.ib, (unsigned)rs.ib.obj);
	if (c.indexBase != rs.indexBaseOffset) gxMReport(true, seq, "INDEXBASE", c.indexBase, rs.indexBaseOffset);
	if (c.vbaOffset != rs.vbaOffset) gxMReport(true, seq, "VBAOFFSET", c.vbaOffset, rs.vbaOffset);
	if (c.ibaOffset != rs.ibaOffset) gxMReport(true, seq, "IBAOFFSET", c.ibaOffset, rs.ibaOffset);
	// the draw's own arguments
	if (c.prim != a.prim) gxMReport(false, seq, "PRIM", c.prim, a.prim);
	if (c.minIndex != a.minIndex) gxMReport(false, seq, "MININDEX", c.minIndex, a.minIndex);
	if (c.numVerts != a.vertexCount) gxMReport(false, seq, "NUMVERTS", c.numVerts, a.vertexCount);
	if (c.startIndex != a.startIndex) gxMReport(false, seq, "STARTINDEX", c.startIndex, a.startIndex);
	if (c.primCount != a.polyCount) gxMReport(false, seq, "PRIMCOUNT", c.primCount, a.polyCount);
	// shader_stale_draws: a full Apply of CurrentShader from empty caches - a slot it sends whose device value differs
	// from the model's device is a shader-owned slot that is not what CurrentShader implies
	s_mscratch = st;
	for (uint32_t &v : s_mscratch.cacheRS) v = gxl2m::kSentinel;
	for (auto &row : s_mscratch.cacheTSS) for (uint32_t &v : row) v = gxl2m::kSentinel;
	s_mscratch.shaderDirty = true;
	s_mscratch.sink = nullptr;
	gxl2m::shaderApply(s_mscratch, st.curShader);
	bool stale = false;
	for (int i = 0; i < 256 && !stale; ++i)
		stale = s_mscratch.cacheRS[i] != gxl2m::kSentinel && s_mscratch.dev.rs[i] != d.rs[i];
	for (int s8 = 0; s8 < gxl2m::kStages && !stale; ++s8)
		for (int i = 0; i < 32 && !stale; ++i)
			stale = s_mscratch.cacheTSS[s8][i] != gxl2m::kSentinel && s_mscratch.dev.tss[s8][i] != d.tss[s8][i];
	s_mStale += stale;
}

struct GxModelHooks : gxl2m::ReplayHooks
{
	size_t draws = 0;	// the model's device draws so far
	void callMismatch(const gxlog::Record &r, const char *why) override
	{
		++s_mCall;
		if (s_mCallPrinted++ < 20)
			fprintf(stderr, "[GXL2CHK] f=%u flush=%s#%d seq=%u kind=MODEL_CALL field=%u classic=0x%X,0x%X l2=%s\n", WW3D::Get_Frame_Count(),
				kGxFlushSiteName[GxFlushSite], s_flushIndex, r.entry, (unsigned)r.op, r.a, r.b, why);
	}
	void drawn(const gxlog::Record &r, const gxl2m::DrawArgs &a, gxl2m::DrawOut o, const gxl2m::State &st) override
	{
		if (o != gxl2m::DRAW_INDEXED && o != gxl2m::DRAW_SORTING)
			return;
		++s_mDraws;
		if (o == gxl2m::DRAW_SORTING)
			++s_mSorting;
		const size_t k = draws++;
		if (k < s_mcap.size())	// the k-th model draw is the k-th device draw; a count difference is reported at the end
			gxModelDrawn(r, a, st, s_mcap[k], o == gxl2m::DRAW_SORTING);
	}
	// a sorted task's queued state (Insert_Triangles' copy) is drawn after the flush, outside the capture window, so
	// the model's copy is compared here or nowhere - each differing field is a MODEL mismatch SORTQ_<field> (counted in `model`)
	void sortq(uint32_t seq, const gxlog::Logical &pred, const gxlog::Logical &logged) override
	{
		struct Out : gxl2m::FieldSink {
			unsigned seq = 0;
			void field(const char *f, uint64_t classic, uint64_t l2) override
			{
				char n[40];
				snprintf(n, sizeof(n), "SORTQ_%s", f);
				gxMReport(false, seq, n, (unsigned)classic, (unsigned)l2);
			}
		} o;
		o.seq = seq;
		++s_mSortq;
		if (gxl2m::logicalDiff(logged, pred, &o))
			++s_mSortqDiff;
	}
	void unknown(uint64_t addr) override
	{
		++s_mUnknown;
		if (s_mUnknownPrinted++ < 5)
			fprintf(stderr, "[GXL2MDL] raw_access caller=0x%llx\n", (unsigned long long)addr);
	}
};

// C5 INVARIANCE - the boundary-replay arm vs the as-recorded arm.
struct GxC5Hooks : gxl2m::C5Sink
{
	void invariance(uint32_t seq, const char *point, const char *field, uint64_t classic, uint64_t l2) override
	{
		if (s_c5Printed++ < 20)
			fprintf(stderr, "[GXL2CHK] f=%u flush=%s#%d seq=%u kind=INVARIANCE field=%s:%s classic=0x%llX l2=0x%llX\n", WW3D::Get_Frame_Count(),
				kGxFlushSiteName[GxFlushSite], s_flushIndex, seq, point, field, (unsigned long long)classic, (unsigned long long)l2);
	}
};

// At every checked flush's start (after gxL2ExecStart): procpass_claimed_xflush, then the log opens on a modelled scene frame.
static void gxL2ModelStart(unsigned frame)
{
	gxClaimedFrame();
	for (const GxClassicDraw &d : s_list)
		if ((d.kind == GxClassicDraw::MATPASS || d.kind == GxClassicDraw::DELAYED_MATPASS) && s_claimedMeshes.count(d.mesh))
			++s_mProcX;
	if (frame != s_lastSceneFrame || !s_modelScene)
		return;
	if (s_snap == nullptr)
		s_snap = new gxlog::Snapshot;
	s_log.clear();
	DX8Wrapper::Gx_Log_Snapshot(*s_snap);
	DX8Wrapper::GxLogParent = DX8Wrapper::GxLogScope = 0;
	DX8Wrapper::GxLogBracket = gxlog::BR_NONE;
	DX8Wrapper::GxLogDrawRec = -1;
	DX8Wrapper::GxLogEntry = 0;	// a record before the flush's first ENTRY names no previous flush's entry
	DX8Wrapper::GxLog = &s_log;
	DX8Wrapper::GxLogOn = true;
	s_mcap.clear();
	s_logOpen = true;
}

// dx8renderer.h GxL2FlushClosedHook: after the flush's null-buffer setters. The log closes, then the model replays it.
static void gxL2FlushClosed()
{
	if (!s_logOpen)
		return;
	s_logOpen = false;
	DX8Wrapper::GxLogOn = false;
	if (WW3D::Get_Frame_Count() != s_mPrintFrame) {
		s_mPrintFrame = WW3D::Get_Frame_Count();
		s_mCallPrinted = s_mStatePrinted = s_c5Printed = 0;
	}
	gxl2m::seed(s_mstate, *s_snap);
	s_mstate.mutant = s_mutant;	// 4 / 24: the model's runtime mutants
	gxl2m::seed(s_mstateB, *s_snap);
	s_mstateB.mutant = s_mutant;	// 5: M5, the carry-out replays the run's FIRST task (T4)
	s_c5runs.clear();
	for (const GxL2Run &r : s_xruns)	// the flush's claimed runs, as Mode A / B / DrawRun would take them
		s_c5runs.push_back(gxl2m::Run{ (uint32_t)r.first, (uint32_t)r.last });
	GxModelHooks h;
	GxC5Hooks h5;
	gxl2m::C5Counts n;
	memset(&n, 0, sizeof(n));
	gxl2m::replayC5(s_mstate, s_mstateB, s_log, s_c5runs.data(), (unsigned)s_c5runs.size(), h, h5, n);	// C6 on the as-recorded arm, as replay()
	s_c5Runs += n.runs;
	s_c5Scaled += n.scaledRuns;
	s_c5Compares += n.compares;
	s_c5Mism += n.mismatches;
	s_c5Unsourced += n.unsourced;
	s_c5Unstable += n.payloadUnstable;
	s_c5BadRanges += n.badRanges;
	if (h.draws != s_mcap.size())
		gxMReport(false, s_log.rec.empty() ? 0u : s_log.rec.back().entry, "draw_count", (unsigned)s_mcap.size(), (unsigned)h.draws);
	++s_mFlushes;
	s_mRecords += s_log.rec.size();
	for (const gxlog::Record &r : s_log.rec)
		s_mSends += r.op >= gxlog::RS && r.op < gxlog::ENTRY;
}

static void gxL2Draw(unsigned prim, unsigned minIndex, unsigned numVerts, unsigned startIndex, unsigned primCount, bool sortingPath)
{
	if (!s_inFlush)
		return;
	++s_flushDraws;
	if (s_logOpen) {	// every device draw of a modelled flush, in order
		s_mcap.resize(s_mcap.size() + 1);
		GxL2Check_Capture(s_mcap.back(), prim, minIndex, numVerts, startIndex, primCount);
	}
	// Task 3/4: the newest entry's own first draw. A sorting-path draw counts too: inside a mesh flush it is IMMEDIATE (a sorting
	// container's task, a procedural pass on a sorted mesh); the sorter's deferred flush runs after the mesh flush
	// returns (RTS3DScene::Flush, W3DScene.cpp: SortingRendererClass::Flush after TheDX8MeshRenderer.Flush), outside this hook's window.
	if (!s_exec.empty()) {
		const size_t idx = s_exec.size() - 1;
		if (idx != s_lastDrawn) {
			s_lastDrawn = idx;
			if (!s_vpSet)
				gxSetViewProj();
			if (s_exec[idx].kind == GxClassicDraw::TASK)
				gxL2TaskDraw(idx, prim, minIndex, numVerts, startIndex, primCount);
			else if (s_exec[idx].kind != GxClassicDraw::SKIN)
				gxSeqOther(s_exec[idx]);
		}
	}
	if (s_mutant == 10 && s_flushDraws == 1 && s_flushes % 100 == 99) {	// the fail arm (comment above)
		IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
		DWORD was = 0;
		D3DMATRIX view, planted;
		dev->GetRenderState(D3DRS_ZFUNC, &was);
		dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
		++s_c9Planted;
		const bool viewOk = dev->GetTransform(D3DTS_VIEW, &view) == D3D_OK;	// and a VIEW: the validity gate must not hide it
		if (viewOk) { planted = view; planted._41 += 1.0f; dev->SetTransform(D3DTS_VIEW, &planted); }	// GX-WRAPSKIP-OK: C9 fail arm (mutant 10): plants, then restores, the device's own VIEW
		gxC9Sample(sortingPath);
		if (viewOk) dev->SetTransform(D3DTS_VIEW, &view);	// GX-WRAPSKIP-OK: C9 fail arm (mutant 10): plants, then restores, the device's own VIEW
		dev->SetRenderState(D3DRS_ZFUNC, was);
		return;
	}
	if (++s_drawsSeen % (unsigned long long)s_c9Every == 0)
		gxC9Sample(sortingPath);
}

// The per-slot C9 table, with every summary: only the slots that ever differed.
static void gxC9PrintSlots()
{
	fprintf(stderr, "[GXL2C9] samples=%llu with_diffs=%llu raw_calls=%llu raw_frames=%u planted=%llu", s_c9Samples, s_c9DiffSamples,
		DX8Wrapper::GxTruthRawCalls, WW3D::Get_Frame_Count() - s_truthFrame0, s_c9Planted);
	for (unsigned i = 0; i < 256; ++i)
		if (s_c9Rs[i]) fprintf(stderr, " RS%u=%llu", i, s_c9Rs[i]);
	for (unsigned st = 0; st < MAX_TEXTURE_STAGES; ++st) {
		for (unsigned i = 0; i < 32; ++i)
			if (s_c9Tss[st][i]) fprintf(stderr, " TSS%u.%u=%llu", st, i, s_c9Tss[st][i]);
		if (s_c9Tex[st]) fprintf(stderr, " TEX%u=%llu", st, s_c9Tex[st]);
	}
	for (unsigned l = 0; l < 4; ++l)
		if (s_c9Light[l]) fprintf(stderr, " LIGHT%u=%llu", l, s_c9Light[l]);
	if (s_c9World) fprintf(stderr, " WORLD=%llu", s_c9World);
	if (s_c9View) fprintf(stderr, " VIEW=%llu", s_c9View);
	if (s_c9ViewSent) fprintf(stderr, " VIEWSENT=%llu", s_c9ViewSent);
	if (s_c9Proj) fprintf(stderr, " PROJ=%llu", s_c9Proj);
	for (unsigned st = 0; st < 8; ++st)
		if (s_c9Texm[st]) fprintf(stderr, " TEXM%u=%llu", st, s_c9Texm[st]);
	if (s_c9Mat) fprintf(stderr, " MAT=%llu", s_c9Mat);
	if (s_c9Fvf) fprintf(stderr, " FVF=%llu", s_c9Fvf);
	for (unsigned i = 0; i < 256; ++i)
		if (s_c9TruthRs[i]) fprintf(stderr, " TRUTH_RS%u=%llu", i, s_c9TruthRs[i]);
	for (unsigned st = 0; st < MAX_TEXTURE_STAGES; ++st)
		for (unsigned i = 0; i < 32; ++i)
			if (s_c9TruthTss[st][i]) fprintf(stderr, " TRUTH_TSS%u.%u=%llu", st, i, s_c9TruthTss[st][i]);
	fprintf(stderr, "\n[GXL2C9S] E2 slots whose device truth is UNKNOWN at C9 samples (count, device value, stable/varies):");
	for (int k = 0; k < kGxFfRsN; ++k)
		if (const unsigned i = kGxFfRs[k]; s_sentRs[i].n) fprintf(stderr, " RS%u=%llu,0x%X,%s", i, s_sentRs[i].n, (unsigned)s_sentRs[i].first, s_sentRs[i].varies ? "varies" : "stable");
	for (unsigned st = 0; st < 2; ++st)
		for (int k = 0; k < kGxFfTssN; ++k)
			if (const unsigned i = kGxFfTss[k]; s_sentTss[st][i].n) fprintf(stderr, " TSS%u.%u=%llu,0x%X,%s", st, i, s_sentTss[st][i].n, (unsigned)s_sentTss[st][i].first,
				s_sentTss[st][i].varies ? "varies" : "stable");
	fprintf(stderr, "\n");
}

static bool gxL2Same(const GxClassicDraw &a, const GxClassicDraw &b)	// field by field: the struct has padding
{
	return a.kind == b.kind && a.cat == b.cat && a.pr == b.pr && a.pass == b.pass && a.mesh == b.mesh && a.decal == b.decal
		&& a.batch == b.batch;
}

static void gxL2Compare()
{
	++s_flushes;
	s_entries += s_exec.size();
	for (const GxClassicDraw &d : s_exec)
		s_b1 += d.batch > 0;
	size_t i = 0;
	const size_t n = std::min(s_list.size(), s_exec.size());
	while (i < n && gxL2Same(s_list[i], s_exec[i]))
		++i;
	const bool bad = i < n || s_list.size() != s_exec.size();
	if (bad) {
		if (++s_bad <= 20) {
			fprintf(stderr, "[GXL2CHK] MISMATCH flush=%llu frame=%u site=%s#%d at=%zu listed=%zu executed=%zu", s_flushes,
				WW3D::Get_Frame_Count(), kGxFlushSiteName[GxFlushSite], s_flushIndex, i, s_list.size(), s_exec.size());
			gxL2PrintEntry("listed", s_list, i);
			gxL2PrintEntry("executed", s_exec, i);
			fprintf(stderr, "\n");
		}
	}
	if (s_flushes % 10000 == 0 || (bad && s_bad == 1)) {	// and once at the first mismatch
		fprintf(stderr, "[GXL2CHK] flushes=%llu entries=%llu b1=%llu mismatched=%llu frame=%u%s\n", s_flushes, s_entries, s_b1,
			s_bad, WW3D::Get_Frame_Count(), s_check == 2 ? " FAILARM" : "");
		gxC9PrintSlots();
		gxL2ExecPrint();
	}
}

// ---- The S1 DrawRun seat ---
// GX_L2_DRAWRUN=1 outside check mode (=1 is DRY by definition): at each claimed run's first task in the category loop
// (dx8renderer.h GxL2SeatHook) the seat forms the run, runs PREPARE - the first task's setters exactly as the classic block calls
// them, then one Apply - and judges the draw-time rules once on the device truth. ON (GxDrawRun::S1_On) and admitted: the run's
// packets go to GxDrawRun::Draw_Run, the wrapper's NORMALIZENORMALS FALSE follows if any task is scaled, then CARRY-OUT - the last
// task's setters (Set_Index_Buffer_Index_Offset through the setter: REQUIRED) and one Apply - and the loop only unlinks
// the run's tasks. OFF or refused: the classic loop draws the whole run from its first task. With
// GX_DRAWRUN_DIGEST=2 every run, in both states, sits in a C2b digest bracket from before PREPARE to the run's last own draw.
static bool s_seat = false;
static GxL2FlushPlan s_seatPlan;					// the flush-start static plan over s_list (the census / check's own)
static gxseat::Digest s_seatDigest;
static bool s_seatDigestOn = false;
static std::vector<PolyRenderTaskClass *> s_seatTaken;	// the run just formed, tasks 2..n, met as the loop reaches them
static size_t s_seatNext = 0;
static bool s_seatTakenClassic = false;			// that run is drawn by the classic loop (OFF / refused): never re-formed from a member
static std::vector<gxseat::Cand> s_seatCand;
static std::vector<PolyRenderTaskClass *> s_seatScan, s_seatMembers;
static std::vector<gxseat::LightKey> s_seatKeys;
static std::vector<GxLights> s_seatLights;
static std::vector<unsigned> s_seatSetIndex, s_seatSetFirst;
static std::vector<GxDrawRun::Task> s_seatTasks;
static std::vector<GxDrawRun::LightSet> s_seatSets;
static unsigned s_seatFrame = ~0u, s_seatRunIndex = 0, s_seatPrintFrame = 0;
static bool s_seatOnFrame = false;				// GxDrawRun::S1_On() for this WW3D frame (read once per frame: its frame clock)
static unsigned long long s_seatRuns = 0, s_seatTaskN = 0, s_seatOn = 0, s_seatOff = 0, s_seatScaled = 0, s_seatSetN = 0, s_seatK = 0;
static unsigned long long s_seatRefused[256];

static void gxSeatMark(void *, bool begin, unsigned long long tag)
{
	GxDrawRun::Digest_Mark(begin, tag);
}

static unsigned s_seatTruthOwn = 0;	// OFF / refused run (GX_DRAWRUN_TRUTH=1): its own wrapper draws still to come; the check fires at the last
static unsigned long long s_seatTruthTag = 0;
static IDirect3DIndexBuffer8 *s_seatRunIb = nullptr;	// PREPARE's index buffer for the current run (GX_DRAWRUN_TRUTH=1)
struct SeatOracle { IDirect3DIndexBuffer8 *runIb; const GxDrawRun::Task *last; const GxDrawRun::LightSet *lastSet; bool anyScaled; };
static void gxSeatTruth(unsigned long long tag, const SeatOracle *oracle = nullptr);

// GxDrawHook with GX_DRAWRUN_DIGEST=2 or GX_DRAWRUN_TRUTH=1: every wrapper draw, before it is recorded - the bracket's clock (D5).
static void gxSeatDraw(unsigned, unsigned, unsigned, unsigned, unsigned, bool)
{
	if (s_seatTruthOwn > 0 && --s_seatTruthOwn == 0)
		gxSeatTruth(s_seatTruthTag);	// the OFF arm's A/A control: after the last task's setters + Apply, before its draw is recorded
	if (s_seatDigestOn)
		s_seatDigest.draw(WW3D::Get_Frame_Count());
}

// GxPostDrawHook with GX_DRAWRUN_DIGEST=2: the same draw, just recorded - an OFF bracket closes after its last own draw, not at the next hooked one.
static void gxSeatPostDraw()
{
	s_seatDigest.drawDone(WW3D::Get_Frame_Count());
}

// DX8Wrapper::GxPrePresentHook with GX_DRAWRUN_DIGEST=2: an open bracket closes in its own frame, before Present and before the depth
// shot GameClient takes after the draw.
static void gxSeatPresent()
{
	s_seatDigest.frameEnd();
}

// A task's per-task setters exactly as DX8TextureCategoryClass::Render's classic block calls them (dx8renderer.cpp: the light
// environment, the ALIGNED / ORIENTED / SKIN / identity world, the object-scale NORMALIZENORMALS TRUE, and DX8PolygonRendererClass::
// Render's Set_Index_Buffer_Index_Offset) - PREPARE with the TRUE, CARRY-OUT without it (the pair is classic's per draw).
static void gxSeatSetters(MeshClass *mesh, bool withNormalize, bool pokeBase = false)
{
	LightEnvironmentClass *lenv = mesh->Get_Lighting_Environment();
	if (lenv != nullptr)
		DX8Wrapper::Set_Light_Environment(lenv);
	const Matrix3D *world_transform = &mesh->Get_Transform();
	bool identity = mesh->Is_Transform_Identity();
	Matrix3D tmp_world;
	if (mesh->Peek_Model()->Get_Flag(MeshModelClass::ALIGNED)) {
		Vector3 mesh_position;
		Vector3 camera_z_vector;
		TheDX8MeshRenderer.Peek_Camera()->Get_Transform().Get_Z_Vector(&camera_z_vector);
		mesh->Get_Transform().Get_Translation(&mesh_position);
		tmp_world.Obj_Look_At(mesh_position, mesh_position + camera_z_vector, 0.0f);
		world_transform = &tmp_world;
	} else if (mesh->Peek_Model()->Get_Flag(MeshModelClass::ORIENTED)) {
		Vector3 mesh_position;
		Vector3 camera_position;
		TheDX8MeshRenderer.Peek_Camera()->Get_Transform().Get_Translation(&camera_position);
		mesh->Get_Transform().Get_Translation(&mesh_position);
		tmp_world.Obj_Look_At(mesh_position, camera_position, 0.0f);
		world_transform = &tmp_world;
	} else if (mesh->Peek_Model()->Get_Flag(MeshModelClass::SKIN)) {
		tmp_world.Make_Identity();
		world_transform = &tmp_world;
		identity = true;
	}
	if (identity)
		DX8Wrapper::Set_World_Identity();
	else
		DX8Wrapper::Set_Transform(D3DTS_WORLD, *world_transform);
	if (withNormalize && mesh->Get_ObjectScale() != 1.0f)
		DX8Wrapper::Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, TRUE);
	if (pokeBase)	// GX_SEAT_MUTANT=2 only: the wrapper's belief moves without the setter, so nothing is sent
		DX8Wrapper::Gx_Poke_Index_Base((unsigned short)mesh->Get_Base_Vertex_Offset());
	else
		DX8Wrapper::Set_Index_Buffer_Index_Offset(mesh->Get_Base_Vertex_Offset());
}

// GX_DRAWRUN_TRUTH=1: the wrapper's BELIEF handed to GxDrawRun::Check_Truth, which asks the d3d8
// device for the same fields on the device thread in stream order. Called IMMEDIATELY after the CARRY-OUT's Apply in ON runs and, as the
// A/A control, at the first defined point after a classic run (the next hook call that is not a member of it, or the flush's end) in
// OFF / refused runs. Belief that the wrapper itself calls unknown (sentinel render state, transform cache cleared, no DX8 index buffer,
// an enabled light of unknown value) is not passed - counted as truth_unknown, never as a pass.
static unsigned long long s_seatTruthN = 0, s_seatTruthUnknown = 0, s_seatBaseDiff = 0;
static unsigned long long s_seatUnkWhy[4] = {0, 0, 0, 0}, s_seatNnAssumed = 0;	// unknown by: no DX8 IB / world cache cleared / ambient sentinel / light of unknown value
static int s_seatMutant = 0;	// GX_SEAT_MUTANT: 1 skips the CARRY-OUT, 2 moves the base vertex behind the setter (truth_mismatch must rise)

// The A/A control is checked from the draw hook at the run's LAST classic draw (gxSeatDraw), where the wrapper's setters and Apply for that
// task are done and nothing else has been recorded: a later point sees the next container's / a decal's buffers (seat-c4 #2, c5 #1).
static void gxSeatTruth(unsigned long long tag, const SeatOracle *oracle)
{
	const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
	const IndexBufferClass *ibc = (const IndexBufferClass *)v.ib;
	const unsigned kUnknown = 0x12345678u;	// Invalidate_Cached_Render_States' sentinel
	GxDrawRun::Truth t;
	memset(&t, 0, sizeof(t));
	// NORMALIZENORMALS: the wrapper sets it only around scaled tasks, and Invalidate_Cached_Render_States leaves its slot at the sentinel, so a
	// run without a scaled task finds it unknown. Classic leaves the DEVICE at FALSE after every task, so that is what is asserted then
	// (the same invariant the oracle requires after every admitted run) and counted as nn_assumed (S33 measured: every run was unknown).
	const bool nnKnown = v.renderStates[D3DRS_NORMALIZENORMALS] != kUnknown;
	const unsigned why = !(ibc != nullptr && ibc->Type() == BUFFER_TYPE_DX8) ? 1u : (v.xformValid & 1u) == 0 ? 2u
		: v.renderStates[D3DRS_AMBIENT] == kUnknown ? 3u : 0u;
	bool known = why == 0;
	if (why != 0)
		++s_seatUnkWhy[why - 1];
	if (known) {
		t.ib = ((DX8IndexBufferClass *)ibc)->Get_DX8_Index_Buffer();
		t.baseVertex = v.indexBaseOffset + v.vbaOffset;
		memcpy(&t.world, &v.transforms[D3DTS_WORLD], sizeof(t.world));
		t.ambient = v.renderStates[D3DRS_AMBIENT];
		t.normalizeNormals = nnKnown ? v.renderStates[D3DRS_NORMALIZENORMALS] : 0u;
		s_seatNnAssumed += nnKnown ? 0 : 1;
		for (int l = 0; l < 4 && known; ++l) {
			if (!v.lightEnables[l])
				continue;
			if (!v.lightValid[l]) {
				known = false;
				++s_seatUnkWhy[3];
			}
			else {
				t.lightEnableMask |= 1u << l;
				t.lights[l] = v.lights[l];
			}
		}
	}
	if (!known) {
		++s_seatTruthUnknown;
		return;
	}
	t.tag = tag;
	t.wrapperDirty = DX8Wrapper::Gx_Pending_Changes();	// right after the Apply: must be 0
	if (oracle != nullptr) {	// the run's classic final state, from the claim: a coherent wrong carry-out
		t.hasOracle = 1;
		t.runIb = oracle->runIb;
		t.last = *oracle->last;
		t.lastSet = *oracle->lastSet;
		t.anyScaled = oracle->anyScaled ? 1u : 0u;
	}
	++s_seatTruthN;
	GxDrawRun::Check_Truth(t);
}

// A classic run whose own wrapper draws did not all arrive before the next hook call / the flush's end: not checked, said so.
static void gxSeatTruthPending()
{
	if (s_seatTruthOwn > 0) {
		s_seatTruthOwn = 0;
		++s_seatTruthUnknown;
	}
}

static void gxSeatPrint()
{
	char refused[512] = "";
	size_t o = 0;
	for (int r = 0; r < 256; ++r)
		if (s_seatRefused[r] && o + 24 < sizeof(refused))
			o += (size_t)snprintf(refused + o, sizeof(refused) - o, "%s%d:%llu", o ? "," : "", r, s_seatRefused[r]);
	fprintf(stderr, "[GXSEAT] f=%u runs=%llu tasks=%llu on_runs=%llu off_runs=%llu scaled_runs=%llu sets=%llu k_mismatch=%llu "
		"truth_calls=%llu truth_unknown=%llu unk_ib=%llu unk_xform=%llu unk_ambient=%llu unk_light=%llu nn_assumed=%llu base_diff_runs=%llu mutant=%d "
		"refused=%s\n", WW3D::Get_Frame_Count(), s_seatRuns, s_seatTaskN, s_seatOn, s_seatOff, s_seatScaled, s_seatSetN, s_seatK,
		s_seatTruthN, s_seatTruthUnknown, s_seatUnkWhy[0], s_seatUnkWhy[1], s_seatUnkWhy[2], s_seatUnkWhy[3], s_seatNnAssumed, s_seatBaseDiff,
		s_seatMutant, refused);
}

// A flush-start reason that leaves the task to the draw-time rules: OK, or one of the draw-time reasons (E2, E4), which the plan judged
// on proxies - the dry check's static pass set (gxL2ExecEnd).
static bool gxSeatStaticPass(gxl2::Reason r)
{
	return r == gxl2::OK || r == gxl2::R_STATE_UNKNOWN || r == gxl2::R_AMBIENT || r == gxl2::R_BLENDED || r == gxl2::R_NO_DEPTH_WRITE
		|| r == gxl2::R_DEPTH_FUNC || r == gxl2::R_COLOR_MASK;
}

// dx8renderer.h GxL2SeatHook: asked once per task the category loop draws, right after its list entry.
static bool gxSeatTask(DX8TextureCategoryClass *cat, PolyRenderTaskClass *prt)
{
	if (s_seatNext < s_seatTaken.size()) {
		if (s_seatTaken[s_seatNext] == prt) {	// a member of the run just formed: drawn by DrawRun, or left to the classic loop as a whole
			++s_seatNext;
			return !s_seatTakenClassic;
		}
		++s_seatK;	// the loop left the run's order: never consume a task the seat did not draw
	}
	gxSeatTruthPending();	// the previous (classic) run's last task has drawn: this call is not one of its members
	s_seatTaken.clear();
	s_seatNext = 0;
	const unsigned frame = s_seatFrame;	// gxSeatFlushStart's: this hook is installed only in a frame the seat is active in
	// D1: the run, from the flush-start plan, in the loop's order (a skipped overflow task has no list entry)
	const size_t idx = s_exec.size() - 1;	// Gx_Executed has just appended this task's entry
	s_seatCand.clear();
	s_seatScan.clear();
	size_t k = idx;
	for (PolyRenderTaskClass *p = prt; p != nullptr; p = Gx_Task_Next(p)) {
		MeshClass *mesh = Gx_Task_Mesh(p);
		gxseat::Cand c = { Gx_Task_Skipped(p), false, false };
		if (!c.overflow) {
			if (k < s_list.size()) {
				const GxClassicDraw &e = s_list[k];
				c.matches = e.kind == GxClassicDraw::TASK && e.cat == cat && e.pr == Gx_Task_Renderer(p) && e.mesh == mesh
					&& (k != idx || (s_exec[idx].mesh == mesh && s_exec[idx].pr == e.pr));
				// the plan's static rules; its draw-time reasons come from shader-bit PROXIES (GxL2Dry_Plan), so they pass here and the
				// device truth after PREPARE decides them - as the dry check's static pass does
				c.staticOk = k < s_seatPlan.reason.size() && gxSeatStaticPass(s_seatPlan.reason[k]);
			}
			++k;
		}
		s_seatCand.push_back(c);
		s_seatScan.push_back(p);
		if (!c.overflow && !(c.matches && c.staticOk))
			break;	// the run ends at or before this one
	}
	if (!s_seatCand.empty() && !s_seatCand[0].matches)
		++s_seatK;
	const size_t end = gxseat::runEnd(s_seatCand.data(), s_seatCand.size());
	if (end == 0)
		return false;
	s_seatMembers.clear();
	for (size_t i = 0; i < end; ++i)
		if (!s_seatCand[i].overflow)
			s_seatMembers.push_back(s_seatScan[i]);
	const size_t n = s_seatMembers.size();
	const bool on = s_seatOnFrame;
	const unsigned runIndex = s_seatRunIndex++;
	if (s_seatDigestOn)
		s_seatDigest.runBegin(frame);	// D5: before PREPARE
	// PREPARE (D2): the first task's setters + one Apply, then the draw-time rules once on the device truth
	MeshClass *first = Gx_Task_Mesh(s_seatMembers[0]);
	DX8PolygonRendererClass *pr0 = Gx_Task_Renderer(s_seatMembers[0]);
	gxSeatSetters(first, true);
	DX8Wrapper::Apply_Render_State_Changes();
	if (GxDrawRun::Truth_On()) {	// the run's container IB as PREPARE bound it - nothing is pending here (the oracle's runIb)
		const DX8Wrapper::GxShadowView sv = DX8Wrapper::Gx_Shadows();
		const IndexBufferClass *sib = (const IndexBufferClass *)sv.ib;
		s_seatRunIb = sib != nullptr && sib->Type() == BUFFER_TYPE_DX8 ? ((DX8IndexBufferClass *)sib)->Get_DX8_Index_Buffer() : nullptr;
	}
	gxl2::CatState c;
	gxl2::TaskState t;
	GxL2Dry_FillCat(cat, pr0, c);
	GxL2Dry_FillTask(first, pr0, 0, t);	// its static rules passed in the plan; proceduralPasses / onlyVisibleTask as the plan judged them
	gxL2TaskPose(first, t);
	gxl2::FlushCtx f = s_fctx;
	gxL2TruthCtx(f, c);
	const gxl2::Reason verdict = gxl2::classifyTask(f, c, t, s_policy);
	++s_seatRuns;
	s_seatTaskN += n;
	if (!on || verdict != gxl2::OK) {	// D3: the classic loop draws the whole run from its first task
		if (verdict != gxl2::OK)
			++s_seatRefused[(unsigned)verdict & 255u];
		else
			++s_seatOff;
		if (s_seatDigestOn)
			s_seatDigest.runPath((unsigned long long)runIndex, (unsigned)n);	// path 0: the run's own n wrapper draws
		s_seatTaken.assign(s_seatMembers.begin() + 1, s_seatMembers.end());	// one run, one index, one bracket - as in ON
		s_seatTakenClassic = true;
		if (GxDrawRun::Truth_On()) {	// the A/A control: checked at the run's last own wrapper draw (gxSeatDraw)
			s_seatTruthOwn = (unsigned)n;
			s_seatTruthTag = (unsigned long long)runIndex;
		}
		return false;
	}
	// D4: the packets, from L2's own path (the C2 predictor's computation), with the vertex-buffer state PREPARE applied
	const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
	s_seatTasks.resize(n);
	s_seatKeys.resize(n);
	s_seatLights.resize(n);
	s_seatSetIndex.resize(n);
	s_seatSetFirst.resize(n);
	bool scaled = false;
	for (size_t i = 0; i < n; ++i) {
		MeshClass *mesh = Gx_Task_Mesh(s_seatMembers[i]);
		DX8PolygonRendererClass *pr = Gx_Task_Renderer(s_seatMembers[i]);
		gxl2::TaskState ti;
		GxL2Dry_FillTask(mesh, pr, 0, ti);
		gxL2TaskPose(mesh, ti);	// FillTask leaves the pose to the caller: a billboard's look-at reads it
		gxLightsOf(*mesh->Get_Lighting_Environment(), s_seatLights[i]);	// non-null: E7
		gxseat::LightKey &key = s_seatKeys[i];
		memset(&key, 0, sizeof(key));
		key.count = s_seatLights[i].n;
		static_assert(sizeof(D3DLIGHT8) == gxseat::kLightBytes, "the light-set key holds D3DLIGHT8 payloads");
		memcpy(key.bytes, s_seatLights[i].l, (size_t)key.count * sizeof(D3DLIGHT8));
		GxDrawRun::Task &task = s_seatTasks[i];
		const Matrix4x4 world = gxL2World(mesh, ti);	// the C2 predictor's own function
		static_assert(sizeof(task.world) == sizeof(world), "D3DMATRIX = Matrix4x4 as the wrapper sends it");
		memcpy(&task.world, &world, sizeof(task.world));
		task.ambient = s_seatLights[i].ambient;
		task.normalizeNormals = ti.objectScale != 1.0f ? 1u : 0u;
		scaled |= task.normalizeNormals != 0;
		const unsigned indexBase = (unsigned)mesh->Get_Base_Vertex_Offset();
		task.baseVertex = (INT)(indexBase + v.vbaOffset);
		unsigned short minIndex = (unsigned short)pr->Get_Min_Vertex_Index(), numVerts = (unsigned short)pr->Get_Vertex_Index_Range();
		if (numVerts < 3) {	// Draw's fallback (dx8wrapper.cpp Draw), as gxL2Predict mirrors it
			minIndex = 0;
			const VertexBufferClass *vb = (const VertexBufferClass *)v.vb;
			if (vb != nullptr && (vb->Type() == BUFFER_TYPE_DX8 || vb->Type() == BUFFER_TYPE_SORTING))
				numVerts = (unsigned short)(vb->Get_Vertex_Count() - indexBase - v.vbaOffset - minIndex);
		}
		task.minIndex = minIndex;
		task.numVertices = numVerts;
		task.startIndex = (unsigned short)pr->Get_Index_Offset() + v.ibaOffset;
		task.primCount = (unsigned short)(pr->Get_Index_Count() / 3);
	}
	const unsigned sets = gxseat::lightSets(s_seatKeys.data(), n, s_seatSetIndex.data(), s_seatSetFirst.data());
	s_seatSets.resize(sets);
	for (unsigned sIdx = 0; sIdx < sets; ++sIdx) {
		const GxLights &l = s_seatLights[s_seatSetFirst[sIdx]];
		GxDrawRun::LightSet &ls = s_seatSets[sIdx];
		memset(&ls, 0, sizeof(ls));
		ls.enableMask = (1u << l.n) - 1u;
		for (int li = 0; li < l.n; ++li)
			ls.lights[li] = l.l[li];
	}
	for (size_t i = 0; i < n; ++i)
		s_seatTasks[i].lightSet = s_seatSetIndex[i];
	if (s_seatDigestOn)
		s_seatDigest.runPath(1ull << 32 | runIndex, 0);	// path 1: DrawRun's draws are not wrapper draws
	GxDrawRun::Draw_Run(D3DPT_TRIANGLELIST, s_seatTasks.data(), (unsigned)n, s_seatSets.data(), sets);
	for (size_t i = 0; i < n; ++i)	// the run's draws in the draw census ([GXGFX] dc=, GxBoundary) as the classic draws counted them
		DX8Wrapper::Record_Direct_Draw();	// main thread, at submission, fallback or not
	if (scaled)	// classic leaves the cached NORMALIZENORMALS FALSE after a scaled task
		DX8Wrapper::Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, FALSE);
	if (s_seatMutant != 1) {	// GX_SEAT_MUTANT=1 skips the CARRY-OUT entirely
		// =3: the carry-out applies the PENULTIMATE task coherently - wrapper and device agree, only the claim's last task tells
		MeshClass *co = Gx_Task_Mesh(s_seatMembers[s_seatMutant == 3 && n >= 2 ? n - 2 : n - 1]);
		gxSeatSetters(co, false, s_seatMutant == 2);	// CARRY-OUT
		DX8Wrapper::Apply_Render_State_Changes();
	}
	if (s_seatDigestOn)
		s_seatDigest.runEnd();	// D5: the bracket holds the run's own draws only - closed right after the carry-out's Apply
	if (s_seatTasks[0].baseVertex != s_seatTasks[n - 1].baseVertex)
		++s_seatBaseDiff;
	if (GxDrawRun::Truth_On()) {	// IMMEDIATELY after the carry-out's Apply: an intervening draw would hide the mutants
		const SeatOracle oracle = { s_seatRunIb, &s_seatTasks[n - 1], &s_seatSets[s_seatTasks[n - 1].lightSet], scaled };
		gxSeatTruth(1ull << 32 | runIndex, &oracle);
	}
	++s_seatOn;
	s_seatScaled += scaled;
	s_seatSetN += sets;
	s_seatTaken.assign(s_seatMembers.begin() + 1, s_seatMembers.end());
	s_seatTakenClassic = false;
	return true;
}

// A seat flush's start. The toggle state is read once per WW3D frame (GxDrawRun::S1_On, its frame clock). The toggle's OFF arm is the
// ORDINARY classic path - no hook, no list, no plan: the default path's one pointer test per task - unless GX_DRAWRUN_DIGEST=2 needs
// the same runs bracketed there; every cost the seat adds is then in the ON arm, as (d) measures it.
// Active: the list, the static plan, the flush context, the K list the hook indexes, the device truth.
static void gxSeatFlushStart()
{
	const unsigned frame = WW3D::Get_Frame_Count();
	if (frame != s_seatFrame) {
		s_seatFrame = frame;
		s_seatRunIndex = 0;
		s_seatOnFrame = GxDrawRun::S1_On();	// false while the fork's DrawRun has not answered on this device (Available)
		if (frame / 600 != s_seatPrintFrame) {
			s_seatPrintFrame = frame / 600;
			gxSeatPrint();
		}
	}
	s_seatTaken.clear();
	s_seatNext = 0;
	const bool active = s_seatOnFrame || ((s_seatDigestOn || GxDrawRun::Truth_On()) && GxDrawRun::Available());
	GxL2SeatHook = active ? &gxSeatTask : nullptr;
	if (!active && !s_census) {
		GxL2Executed = nullptr;
		return;
	}
	// the census reads this Flush's list and plan in every frame it runs, OFF frames included (never another
	// Flush's - their meshes may be gone); a census run is not a timing run
	TheDX8MeshRenderer.Gx_List_Pending(s_list);
	GxL2Dry_Plan(s_list, GxFlushSite, s_seatPlan);
	if (!active) {
		GxL2Executed = nullptr;
		return;
	}
	if (!DX8Wrapper::GxTruthOn)
		DX8Wrapper::Gx_Truth_Enable();	// E2 judges on the device truth (as the check's first Flush does)
	GxL2Dry_FillFlush(GxFlushSite, s_fctx);
	if (CameraClass *cam = TheDX8MeshRenderer.Peek_Camera()) {
		const Matrix3D &ct = cam->Get_Transform();
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 4; ++k)
				s_fctx.camera.m[r][k] = ct[r][k];
	}
	s_exec.clear();
	GxL2Executed = &s_exec;
}

static void gxL2Flush(bool end)
{
	if (end) {
		if (s_seat) {	// seat: a Flush without a camera runs no hooks, so nothing of this
			gxSeatTruthPending();	// the flush's last classic run, before the bracket state is dropped
			GxL2SeatHook = nullptr;	// one may reach it (its list, its plan)
			GxL2Executed = nullptr;
			s_seatTaken.clear();
			s_seatNext = 0;
		}
		if (s_check) {
			GxL2Executed = nullptr;
			s_inFlush = false;
			gxL2ExecEnd(WW3D::Get_Frame_Count());
			gxL2Compare();
		}
		return;
	}
	const unsigned frame = WW3D::Get_Frame_Count();
	if (frame != s_siteFrame) {
		s_siteFrame = frame;
		memset(s_siteIndex, 0, sizeof(s_siteIndex));
	}
	s_flushIndex = s_siteIndex[GxFlushSite]++;
	if (s_seat) {	// the seat (never with check: DRY by definition)
		gxSeatFlushStart();
		if (s_census) {	// the census reads the seat's own plan
			GxL2Dry_Census(s_seatPlan, frame);
#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
			gxL2Census();
#endif
		}
		return;
	}
	if (s_check && !DX8Wrapper::GxTruthOn) {
		DX8Wrapper::Gx_Truth_Enable();	// at the first checked Flush: the device exists and is the truth from here on
		s_truthFrame0 = frame;
	}
	TheDX8MeshRenderer.Gx_List_Pending(s_list);
	if (s_census || s_check)
		GxL2Dry_Plan(s_list, GxFlushSite, s_xplan);	// the census, and the static reasons of tasks that make no draw (Task 3)
	if (s_census) {
		GxL2Dry_Census(s_xplan, frame);
#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
		gxL2Census();
#endif
	}
	if (s_check) {
		if (s_check == 2 && s_list.size() >= 2)
			std::swap(s_list[0], s_list[1]);
		s_exec.clear();
		GxL2Executed = &s_exec;
		s_inFlush = true;
		s_flushDraws = 0;
		gxL2ExecStart();
		if (s_model)
			gxL2ModelStart(frame);
	}
}

void GxScene_Install()
{
	static bool s_done = false;
	if (s_done)
		return;
	s_done = true;
	const char *chk = GxTestEnv("GX_L2_CHECK");
	s_check = chk ? atoi(chk) : 0;
	if (s_check != 1 && s_check != 2)
		s_check = 0;
	if (getenv("GX_L2_CENSUS") != nullptr) {
		s_census = true;
#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
		fprintf(stderr, "[GXL2] census on: [GXL2CEN] per site; [GXCENSUS] l2_* with GX_DRAWCENSUS=1 GX_GFX=1\n");
#else
		fprintf(stderr, "[GXL2] census on: [GXL2CEN] per site ([GXCENSUS] l2_* need a GX_RENDER_CENSUS build)\n");
#endif
	}
	if (s_check)
		fprintf(stderr, "[GXL2] check on: the classic list vs what each Flush processes%s\n", s_check == 2 ? " - FAIL ARM (first two entries swapped)" : "");
	if (s_check) {
		const char *every = GxTestEnv("GX_L2_C9_EVERY");
		if (every && atoi(every) > 0)
			s_c9Every = atoi(every);
		const char *mut = GxTestEnv("GX_L2_MUTANT");
		s_mutant = mut ? atoi(mut) : 0;
		const char *mbe = GxTestEnv("GX_L2_MODEB_EVERY");
		if (mbe && atoi(mbe) > 0)
			s_modeBEvery = atoi(mbe);
		fprintf(stderr, "[GXL2] Mode B + C3 on every %d scene frame(s)\n", s_modeBEvery);
		const char *mdl = GxTestEnv("GX_L2_MODEL");
		if (s_check == 1 && mdl && atoi(mdl) == 1) {	// off under the K fail arm, =2
			s_model = true;
			s_modelEvery = s_modeBEvery;
			const char *me = GxTestEnv("GX_L2_MODEL_EVERY");
			if (me && atoi(me) > 0)
				s_modelEvery = atoi(me);
			fprintf(stderr, "[GXL2] model on: every %d scene frame(s)\n", s_modelEvery);
		}
		GxDrawHook = &gxL2Draw;
		const int c8 = GxL2Check_MaskOracle(s_mutant);	// CONTRACT_L2 §4.3 C8, once at start-up
		fprintf(stderr, "[GXL2CHK] C8 mask oracle: %d disagreeing slot(s) - %s%s\n", c8, c8 ? "FAIL" : "PASS",
			s_mutant == 6 ? " (MUTANT 6: stage 0 masked - must FAIL)" : "");
		DX8Wrapper::GxCaptureOn = true;	// before the first frame: material / projection / texture-matrix capture (dx8wrapper.h)
		fprintf(stderr, "[GXL2] C9 on: shadows vs device every %d draws in a Flush%s\n", s_c9Every,
			s_mutant == 10 ? " - MUTANT 10 (planted ZFUNC behind the wrapper every 100th Flush)" : "");
	}
	// The seat: GX_L2_DRAWRUN=1, never in check mode (DRY by definition)
	// the seat's own opt-in: GxDrawRun::Enabled() is also true for its synthetic test and the overhead arm
	const char *dr = getenv("GX_L2_DRAWRUN");
	s_seat = !s_check && dr != nullptr && atoi(dr) == 1 && GxDrawRun::Enabled();
	if (s_seat) {
		memset(s_seatRefused, 0, sizeof(s_seatRefused));	// GxL2SeatHook: installed per frame by gxSeatFlushStart
		const char *sm = GxTestEnv("GX_SEAT_MUTANT");
		s_seatMutant = sm != nullptr ? atoi(sm) : 0;
		if (GxDrawRun::Truth_On())
			DX8Wrapper::GxCaptureOn = true;	// GxXformValid (the world the device holds) is kept only under capture (seat-c4 #1)
		const char *dg = GxTestEnv("GX_DRAWRUN_DIGEST");
		s_seatDigestOn = dg != nullptr && atoi(dg) == 2;
		if (s_seatDigestOn) {	// s_seat excludes check mode, so the check's own GxDrawHook is never replaced
			s_seatDigest.mark = &gxSeatMark;
			GxDrawHook = &gxSeatDraw;
			DX8Wrapper::GxPrePresentHook = &gxSeatPresent;
			GxPostDrawHook = &gxSeatPostDraw;
		} else if (GxDrawRun::Truth_On()) {
			GxDrawHook = &gxSeatDraw;	// the A/A control's clock
		}
		fprintf(stderr, "[GXSEAT] on: claimed runs -> PREPARE, GxDrawRun::Draw_Run, CARRY-OUT (S1_On toggle%s)%s\n",
			s_seatDigestOn ? "; digest brackets at the seat, GX_DRAWRUN_DIGEST=2" : "",
			s_seatMutant == 1 ? " - MUTANT 1 (carry-out skipped)" : s_seatMutant == 2 ? " - MUTANT 2 (base vertex moved behind the setter)"
				: s_seatMutant == 3 ? " - MUTANT 3 (carry-out applies the penultimate task)" : "");
	}
	if (s_census || s_check || s_seat)
		GxL2FlushHook = &gxL2Flush;
	if (s_model)
		GxL2FlushClosedHook = &gxL2FlushClosed;
}
