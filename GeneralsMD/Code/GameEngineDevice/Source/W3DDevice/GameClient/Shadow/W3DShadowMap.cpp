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

// GPU sun shadow map: the Zero Hour game glue. Render-only, outside the SimID digest; installed by
// W3DShadowManager::init. Per tactical frame it asks the native device for IGxShadowMap (absent on DXVK and
// classic: the old volumes draw), fits the cascades to 2003's point light and walks the casters.
// Not yet: tree casters and PROJECTION casters. Include order follows W3DShadow.cpp.
#include "WWLib/always.h"
#include "GameClient/View.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DVolumetricShadow.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "W3DDevice/GameClient/W3DShadow.h"
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "Common/MapObject.h"
#include "W3DDevice/GameClient/GXShadowMapHooks.h"
#include "W3DDevice/GameClient/GXShadowMapFit.h"
#include "W3DDevice/GameClient/GXShadowMapTagState.h"
#include "W3DDevice/GameClient/GxShadowMap.h"
#include "GameClient/GXGraphicsSettings.h"
#include "WW3D2/Renderer.h"   // restoreState goes through the Renderer seam
#include "WW3D2/dx8renderer.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/sortingrenderer.h"
#include "WW3D2/ww3d.h"
#include "WWMath/colmath.h"
#include "W3DDevice/GameClient/GXShadowMapOffscreen.h"
#include "WW3D2/hlod.h"
#include "GameLogic/GameLogic.h"
#include "WW3D2/part_buf.h"
#include "WW3D2/scene.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	IDirect3DDevice8 *s_dev = nullptr;
	IGxShadowMap *s_sm = nullptr;
	uint32_t s_tags = 0;
	uint64_t s_own = 0;          // A41: the own light the device holds (0 = none)
	W3DVolumetricShadowManager::GxShadowMapWalkOut s_walk;
	// A39: the (map, L) the image plane was made for, and whether that (map, L) fell back to today's volumes (latched: A42 #10)
	struct PointKey { const WorldHeightMap *map = nullptr; float L[3] = { 0.f, 0.f, 0.f }; bool latched = false; char why[96] = ""; } s_key;
	GXShadowFit::PointBasis s_pb;
	uint32_t s_ownCalls = 0, s_ownCallsPeriod = 0, s_latchedFrames = 0, s_paddingPeriod = 0;
	// TEST-ONLY GX_SHADOWMAP_LIGHT=sun: the sun light - ONE orthographic map along the map's sun, no own lights -
	// on this binary, so the run uses the same pictures and the same GX_SHADOW_DEBUG_COLOR as the other arms. Unset = the default light (always in play).
	// TEST-ONLY GX_SHADOWMAP_CAST=none: no draw is ever tagged CAST - the map stays clear
	// and, with the key on, nothing is shadowed at all (the volumes are off too): the no-shadow reference of the masks
	// TEST-ONLY GX_SHADOWMAP_OWN=0 forces every walk
	// root's own light to none (casters captured unscaled, receivers look up unscaled); GX_SHADOWMAP_BIAS_SCALE=<x> scales the step's
	// constant and slope depth biases (never the clamp). Both read once, printed; unset = no change.
	bool ownOff()
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_OWN");
			s = (e && !strcmp(e, "0")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_OWN=0: no own lights\n");
		}
		return s == 1;
	}
	float biasScale()
	{
		static float k = -1.f;
		if (k < 0.f)
		{
			const char *e = getenv("GX_SHADOWMAP_BIAS_SCALE");
			const float v = e ? float(atof(e)) : 1.f;
			k = (v > 0.f && v <= 16.f) ? v : 1.f;
			if (e)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_BIAS_SCALE=%s: depth biases x %.3f\n", e, k);
		}
		return k;
	}
	// A71/A73 TEST-ONLY (the D72 / D73 diagnostics): GX_SHADOWMAP_BIAS_CLAMP=<units> REPLACES the step's depth-bias clamp by units / 65536
	// (0..4096; 0 = unclamped, the pre-A73 bias). Unset or out of range = the step's clamp. The caster pass drops a non-zero clamp on a
	// device without depthBiasClamp (GxShadowCasters::record).
	float biasClamp(float stepClamp)
	{
		static float c = -2.f;
		if (c < -1.5f)
		{
			const char *e = getenv("GX_SHADOWMAP_BIAS_CLAMP");
			char *end = nullptr;
			const double v = e ? strtod(e, &end) : -1.0;   // validated as a double (4096.0001 rounds to 4096.f)
			c = (e && end != e && *end == '\0' && v >= 0.0 && v <= 4096.0) ? float(v / 65536.0) : -1.f;   // the whole value a number
			if (e)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_BIAS_CLAMP=%s: depth-bias clamp %.8f%s\n", e, double(c),
					c < 0.f ? " (out of range: the step's clamp)" : "");
		}
		return c >= 0.f ? c : stepClamp;
	}
	bool castNone()
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_CAST");
			s = (e && !strcmp(e, "none")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_CAST=none: no casters\n");
		}
		return s == 1;
	}
	bool sunMode()
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_LIGHT");
			s = (e && !strcmp(e, "sun")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_LIGHT=sun: the P2 orthographic light, no own lights\n");
		}
		return s == 1;
	}
	// The own-light pull's delta in world units. TEST-ONLY GX_SHADOWMAP_OWN_PULL=<wu> (0..256) sets it; the
	// default stays 0 (no pull) until a registered policy is committed, as with the clamp. Printed once.
	float ownPullDelta()
	{
		static float k = -1.f;
		if (k < 0.f)
		{
			const char *e = getenv("GX_SHADOWMAP_OWN_PULL");
			char *end = nullptr;
			const double v = e ? strtod(e, &end) : 0.0;
			const bool valid = e && end != e && *end == '\0' && v >= 0.0 && v <= 256.0;   // the whole value a number, in range
			k = valid ? float(v) : 0.f;
			fprintf(stderr, "[GXSHADOWMAP] own pull: delta=%g wu (%s%s%s) muMax=%.3f\n", double(k),
				e ? "TEST-ONLY GX_SHADOWMAP_OWN_PULL=" : "default: 0 until D79's policy", e ? e : "", (e && !valid) ? " - invalid: 0" : "",
				double(GXShadowFit::kOwnPullMuMax));
		}
		return k;
	}
	float s_pullEps = 0.f, s_pullD = 0.f;   // A79: this frame's eps and the distance it came from (ownPullEps)
	GXShadowFit::State s_fit[2];
	uint32_t s_lastStep = ~0u;
	uint32_t s_frames = 0;
	GxShadowFrame s_frame = {};   // BeginShadowFrame's argument, alive until the next frame (a ring replay reads it by pointer)

	// every IGxShadowMap call goes through DX8Wrapper::Device_Call, so it is recorded into DX8Ring with the
	// mesh draws it tags (GX_D3D8RING) and replayed in stream order on the ring's thread - never pushed past them from here. The tag
	// travels BY VALUE in the call's context word; the frame by a pointer to s_frame, which lives until the next tactical frame.
	void tagsCall(IDirect3DDevice8 *, void *ctx)
	{
		if (s_sm)
			s_sm->SetShadowTags(uint32_t(reinterpret_cast<uintptr_t>(ctx)));
	}
	void frameCall(IDirect3DDevice8 *, void *ctx)
	{
		if (s_sm)
			s_sm->BeginShadowFrame(*static_cast<const GxShadowFrame *>(ctx));
	}
	static_assert(sizeof(void *) == 8, "the own light travels by value in Device_Call's 64-bit context word");
	void ownCall(IDirect3DDevice8 *, void *ctx)
	{
		if (s_sm)
			s_sm->SetOwnLight(uint64_t(reinterpret_cast<uintptr_t>(ctx)));
	}
	// on change only; 0 = none
	void setOwn(uint64_t own)
	{
		if (own != s_own && s_sm)
		{
			DX8Wrapper::Device_Call(&ownCall, reinterpret_cast<void *>(uintptr_t(own)));
			s_own = own;
			++s_ownCalls;
		}
	}

	void setTags(uint32_t t)
	{
		if (t != s_tags && s_sm)
		{
			DX8Wrapper::Device_Call(&tagsCall, reinterpret_cast<void *>(uintptr_t(t)));
			s_tags = t;
		}
	}

	// the walk's index of the mesh's root (-1: not a caster root)
	int rootIndex(const MeshClass *mesh)
	{
		const RenderObjClass *root = mesh;
		while (root->Get_Container() != nullptr)
			root = root->Get_Container();
		const std::vector<RenderObjClass *> &r = s_walk.roots;
		const std::vector<RenderObjClass *>::const_iterator it = std::lower_bound(r.begin(), r.end(), root);
		return it != r.end() && *it == root ? int(it - r.begin()) : -1;
	}
	// the volume code's own mesh filter (W3DVolumetricShadow.cpp:807-811; skin never reaches here)
	bool casterFilter(const MeshClass *mesh)
	{
		// Peek_Model is not const in WW3D2 (read-only use here)
		return !((mesh->Is_Alpha() || mesh->Is_Translucent()) &&
			!const_cast<MeshClass *>(mesh)->Peek_Model()->Get_Flag(MeshGeometryClass::CAST_SHADOW));
	}

	// every pass of a non-skin mesh task: its root's own light (A41: every receiver draw of a clamped instance, caster or not), and
	// CAST on pass 0 when the mesh casts
	void meshBegin(const MeshClass *mesh, int pass)
	{
		const int i = rootIndex(mesh);
		const GXShadowTags::Want w = GXShadowTags::meshBegin(s_tags, i >= 0, i >= 0 ? s_walk.own[size_t(i)] : 0, pass, casterFilter(mesh));
		setOwn(w.own);
		// A60 TEST-ONLY GX_SHADOWMAP_DROP_CAST (the D60 diagnostic): this mesh's draws lose CAST - nothing else changes
		const bool drop = !castNone() && (w.tags & GX_SHADOW_TAG_CAST) != 0 &&   // A60b: no (cast) line under CAST=none
			W3DVolumetricShadowManager::gxShadowDropMesh(mesh->Get_Name(), TRUE);
		setTags(castNone() || drop ? (w.tags & ~uint32_t(GX_SHADOW_TAG_CAST)) : w.tags);
	}

	// a procedural or delayed material pass (dx8renderer.cpp Render_Procedural_Material_Passes / Render_Delayed_...: they bypass
	// meshBegin): the own light only, never CAST; a skin mesh has no caster copy, so none
	void passBegin(const MeshClass *mesh)
	{
		const bool skin = const_cast<MeshClass *>(mesh)->Peek_Model() && const_cast<MeshClass *>(mesh)->Peek_Model()->Get_Flag(MeshModelClass::SKIN);
		const int i = skin ? -1 : rootIndex(mesh);
		const GXShadowTags::Want w = GXShadowTags::passBegin(s_tags, s_own, skin, i >= 0, i >= 0 ? s_walk.own[size_t(i)] : 0);
		setOwn(w.own);
		setTags(w.tags);
	}

	// the end of a rigid container's draws and of every material-pass loop: nothing after it inherits CAST or an own light
	void containerEnd()
	{
		const GXShadowTags::Want w = GXShadowTags::containerEnd(s_tags);
		setTags(w.tags);
		setOwn(w.own);
	}

	static_assert(GXShadowTags::kCast == GX_SHADOW_TAG_CAST, "GXShadowMapTagState.h's CAST is the device's");
	void smSetTags(uint32_t t) { setTags(t); }   // A44: the offscreen ops' name for it (a member named setTags would hide it)
	GxShadowCastHooks s_castHooks = { meshBegin, containerEnd, passBegin };

	// steps: map size per cascade, cascades, guard texels (the step's filter reach + 1), depth bias { constant, clamp, slope }.
	// The clamp stays 0 (unclamped). A clamp of 8 D16 units made airborne casters' shadows solid but
	// dithered unit and building surfaces, and the sweep 64 / 256 / 1024 found no single pass-wide value
	// that does both: an own-light (squashed) airborne caster sits closer to the ground in depth than unit surfaces' slope bias allows.
	// The next step is per-caster bias or a receiver-side offset; GxShadowCasters keeps the depthBiasClamp guard for any clamp sent
	struct Step { uint32_t res, cascades, guard; float bias[3]; };
	const Step kSteps[3] = {
		{ 2048, 2, 5, { 4.0f, 0.0f, 2.0f } },
		{ 1536, 1, 3, { 4.0f, 0.0f, 2.0f } },
		{ 1024, 1, 2, { 4.0f, 0.0f, 2.0f } },
	};

	// the render object's min/max height are scanned once at load (BaseHeightMap initHeightData);
	// scripted craters lower samples later (setRawMapHeight + terrainHeightChanged, which never updates them). So the slab is re-measured
	// from the raw samples (a byte min/max over the map: tens of microseconds) on a new map and whenever W3DView::update rebuilt terrain
	// since the last frame (GxShadowMapTerrainChanged: the same frame the crater first draws), plus every 256 tactical frames as a
	// backstop for any other writer; it only ever WIDENS the load-time range
	struct TerrainScan { const WorldHeightMap *map = nullptr; uint32_t age = 0; float lo = 0.0f, hi = 0.0f; } s_scan;
	void terrainRange(float *lo, float *hi)
	{
		*lo = TheTerrainRenderObject->getMinHeight();
		*hi = TheTerrainRenderObject->getMaxHeight();
		WorldHeightMap *hm = TheTerrainRenderObject->getMap();
		if (hm == nullptr || hm->getDataPtr() == nullptr)
			return;
		if (hm != s_scan.map || GxShadowMapTerrainChanged || ++s_scan.age >= 256)
		{
			GxShadowMapTerrainChanged = false;
			const UnsignedByte *d = hm->getDataPtr();
			const size_t n = size_t(hm->getXExtent()) * size_t(hm->getYExtent());
			UnsignedByte mn = 255, mx = 0;
			for (size_t i = 0; i < n; ++i)
			{
				mn = d[i] < mn ? d[i] : mn;
				mx = d[i] > mx ? d[i] : mx;
			}
			s_scan.map = hm;
			s_scan.age = 0;
			s_scan.lo = n ? float(mn) * hm->getHeightScale() : *lo;
			s_scan.hi = n ? float(mx) * hm->getHeightScale() : *hi;
		}
		*lo = s_scan.lo < *lo ? s_scan.lo : *lo;
		*hi = s_scan.hi > *hi ? s_scan.hi : *hi;
	}

	bool queryDevice()
	{
		IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
		if (dev != s_dev)
		{
			if (s_sm)
				s_sm->Release();
			s_sm = nullptr;
			s_dev = dev;
			s_tags = 0;
			s_own = 0;
			if (dev == nullptr || dev->QueryInterface(IID_GxShadowMap, reinterpret_cast<void **>(&s_sm)) != S_OK)
				s_sm = nullptr;
		}
		return s_sm != nullptr;
	}

	// this frame runs today's volumes: a step-3 frame (no capture, no pass; renderShadows sees GxShadowMapActive false)
	void volumesFrame(GxShadowFrame &f, uint32_t step)
	{
		f.cascades = 0;
		f.step = 3;
		s_frame = f;
		DX8Wrapper::Device_Call(&frameCall, &s_frame);
		s_lastStep = step;
	}

	// A42 #10: a (map, L) whose map cannot be made validly runs today's volumes until the map or L changes - never per-frame popping
	void latch(const char *why, float value)
	{
		if (s_key.latched)
			return;
		s_key.latched = true;
		std::snprintf(s_key.why, sizeof(s_key.why), "%s %.1f", why, value);
		fprintf(stderr, "[GXSHADOWMAP] point light: %s - volumes for this map and time of day\n", s_key.why);
	}

	// ---- A44/A46/A47: the off-screen casters ---------------------------------------------------------------
	// Candidates = 2003's own Update pre-cull (the walk's cand flags, A47). Per caster MESH of a candidate (collected the way HLodClass::Render
	// picks models): dropped outside the light volume; skipped when the scene will draw its root and the mesh is inside the camera (the
	// scene captures it with CAST); else submitted. Only those MeshClass leaves are Render()ed - never a root - so the pose a candidate gets
	// is the lazy Get_Transform one, 2003's own trigger on the same candidate set (A47). The WIDE light camera is rinfo's camera (its only
	// reader on this path is MeshClass::Render's per-mesh cull + the decal rejection distance); the mesh renderer's camera stays the MAIN
	// camera, so ALIGNED / ORIENTED meshes build the scene's world transform.
	CameraClass *s_wideCam = nullptr;
	std::vector<MeshClass *> s_offMeshes;     // this frame's submissions (raw: used inside tacticalBegin only)
	std::vector<MeshClass *> s_skipped;       // TEST-ONLY check mode: the skipped meshes, each held by a reference until tacticalEnd
	std::vector<RenderObjClass *> s_leaves;
	std::vector<uint8_t> s_leafLodDiff;       // A63: parallel to s_leaves - an enclosing HLOD has > 1 LOD and draws one below the top
	std::vector<GxClassicDraw> s_pending;
	CameraClass *s_frameCam = nullptr;        // the tactical camera of this frame (check mode reads its frustum at tacticalEnd)
	uint32_t s_offCandPeriod = 0, s_offMeshesPeriod = 0, s_offInviewPeriod = 0, s_offDirtyPeriod = 0, s_skipWrongPeriod = 0,
		s_skipCheckedPeriod = 0, s_skipDirtyPeriod = 0, s_stateMovedPeriod = 0, s_stateMismatchPeriod = 0;
	uint32_t s_armedUnfired = 0;              // A50 (4): tactical frames armed by W3DView whose start the scene never fired
	bool s_started = false;                   // A50 (4): this frame's tacticalBegin ran (tacticalEnd is a no-op otherwise)
	double s_frameUsPeriod = 0.0;             // A50 (2): the whole tacticalBegin (charged to [GXGFXF] TERR)
	bool s_censusPrinted = false;             // A52: the census line went out this frame (frame_us follows it)
	unsigned s_emitN[2] = { ~0u, ~0u }, s_emitU[2] = { ~0u, ~0u };   // A50/A55/A56 (5): the last [GXEMIT] tuple printed (per ring)
	unsigned long long s_emitS[2] = { ~0ull, ~0ull }, s_emitM[2] = { ~0ull, ~0ull };
	double s_offUsPeriod = 0.0;
	// A61/A62/A63: the sorted CAST_SHADOW casters (rotor discs) - submitted in view / off view, own-light groups,
	// meshes under a non-top LOD (A63's guard: 2003's geometry is the top LOD), and the sorted phase's own time (A62 (5))
	uint32_t s_sortedInPeriod = 0, s_sortedOffPeriod = 0, s_sortedGroupsPeriod = 0, s_sortedLodDiffPeriod = 0;
	double s_sortedUsPeriod = 0.0;
	struct SortedCaster { MeshClass *mesh; uint64_t own; };
	std::vector<SortedCaster> s_sorted;       // this frame's sorted submissions, grouped by own light (raw: used inside tacticalBegin only)
	std::vector<size_t> s_groupStart;         // group g = s_sorted[s_groupStart[g], s_groupStart[g + 1])
	std::chrono::steady_clock::time_point s_sortedT0;

	// TEST-ONLY GX_SHADOWMAP_OFFSCREEN=0 (A47 M3 arm N0: the early flush disabled, everything else identical); GX_SHADOWMAP_OFFSCREEN_CHECK=1
	// (A47's residual measurement: skip_wrong)
	bool offscreenOff()
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_OFFSCREEN");
			s = (e && !strcmp(e, "0")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_OFFSCREEN=0: no off-screen casters\n");
		}
		return s == 1;
	}
	bool offscreenCheck()
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_OFFSCREEN_CHECK");
			s = (e && !strcmp(e, "1")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_OFFSCREEN_CHECK=1: skipped meshes re-checked after the scene\n");
		}
		return s == 1;
	}

	// the models HLodClass::Render would draw (hlod.cpp:2125: the current LOD, OBBOX skipped, then the additional models - at the parent's
	// LOD when it matches sub-object LODs, :2140, clamped as Set_LOD_Level clamps: GXShadowFit::effectiveLod, A48), nested HLODs and other
	// containers recursed (no depth limit: W3D containment has no cycles); leaves only - the caller keeps the casters. forcedLod < 0 = the
	// HLOD's own stored LOD. Nothing here sets a LOD or reads a transform.
	void collectLeaves(RenderObjClass *o, int forcedLod, bool lodDiff = false)
	{
		if (o == nullptr || !o->Is_Not_Hidden_At_All())
			return;
		const int id = o->Class_ID();
		if (id == RenderObjClass::CLASSID_MESH)
		{
			s_leaves.push_back(o);
			s_leafLodDiff.push_back(lodDiff ? 1 : 0);
			return;
		}
		if (id == RenderObjClass::CLASSID_HLOD)
		{
			HLodClass *h = static_cast<HLodClass *>(o);
			const int lod = forcedLod >= 0 ? forcedLod : h->Get_LOD_Level();
			const bool diff = lodDiff || (h->Get_LOD_Count() > 1 && lod != h->Get_LOD_Count() - 1);   // A63
			for (int i = 0; i < h->Get_Lod_Model_Count(lod); ++i)
			{
				RenderObjClass *m = h->Peek_Lod_Model(lod, i);
				if (m != nullptr && m->Class_ID() != RenderObjClass::CLASSID_OBBOX)
					collectLeaves(m, -1, diff);
			}
			const bool match = h->Is_Sub_Objects_Match_LOD_Enabled() != 0;
			for (int i = 0; i < h->Get_Additional_Model_Count(); ++i)
			{
				RenderObjClass *a = h->Peek_Additional_Model(i);
				if (a == nullptr)
					continue;
				const int childLod = (a->Class_ID() == RenderObjClass::CLASSID_HLOD && match)
					? GXShadowFit::effectiveLod(lod, true, a->Get_LOD_Level(), a->Get_LOD_Count()) : -1;
				collectLeaves(a, childLod, diff);
			}
			return;
		}
		const int n = o->Get_Num_Sub_Objects();
		for (int k = 0; k < n; ++k)
		{
			RenderObjClass *c = o->Get_Sub_Object(k);
			if (c == nullptr)
				continue;
			collectLeaves(c, -1, lodDiff);
			c->Release_Ref();
		}
	}

	// A48: would reading this object's transform evaluate a pose? RenderObjClass::Validate_Transform (rendobj.cpp:458) re-poses the top
	// container when ANY container up the chain has dirty sub-object transforms - the same predicate, read-only
	bool chainDirty(RenderObjClass *o)
	{
		for (RenderObjClass *c = o->Get_Container(); c != nullptr; c = c->Get_Container())
			if (c->Are_Sub_Object_Transforms_Dirty())
				return true;
		return false;
	}

	// A51: will the scene draw this root - RTS3DScene::Visibility_Check's own answer, READ: it sets every RenderList object's visible bit on
	// every outcome this pass (force-visible, hidden, culled, shrouded / effectively hidden, visible; W3DScene.cpp:974-1043) before the hook
	// fires. No sphere read (an HLOD sphere can re-pose), no drawable read.
	bool sceneDrawsRoot(RenderObjClass *root)
	{
		return GxShadowMapFrameScene != nullptr && root->Peek_Scene() == GxShadowMapFrameScene && root->Is_Visible() != 0;
	}

	// The start runs after cam->Apply, fill mode, Set_Ambient, the mesh renderer's Set_Camera and Set_Fog,
	// so whatever the off-screen stage moves must be put back (WORLD excluded). Snapshot -> stage -> restore what moved; lights and
	// DX8Wrapper's fog statics are compare-only (nothing on this path writes them). Check mode counts both sides.
	const D3DRENDERSTATETYPE kSnapRs[7] = { D3DRS_FILLMODE, D3DRS_AMBIENT, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGSTART,
		D3DRS_FOGEND };
	struct StateSnap
	{
		Matrix4x4 view, proj;
		D3DVIEWPORT8 vp;
		bool vpOk;
		unsigned rs[7];
		Vector3 ambient;
		bool fog;
		unsigned fogColor;
		bool light[4];
		// A64: the lights the DEVICE holds - the wrapper's sent-light cache (DX8Wrapper::Gx_Shadows: the native
		// device answers no GetLight). The sorting pool's Apply_Render_State sets them with Set_DX8_Light, past render_state's flags.
		bool sentOn[4], sentValid[4];
		D3DLIGHT8 sent[4];
	};
	void snapState(StateSnap &s)
	{
		DX8Wrapper::Get_Transform(D3DTS_VIEW, s.view);
		DX8Wrapper::Get_Transform(D3DTS_PROJECTION, s.proj);
		IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
		std::memset(&s.vp, 0, sizeof(s.vp));
		s.vpOk = dev != nullptr && dev->GetViewport(&s.vp) == D3D_OK;
		for (int i = 0; i < 7; ++i)
		{	// A52: the DEVICE's value - the wrapper cache can hold 0x12345678 = unknown (Invalidate_Cached_Render_States)
			DWORD v = 0;
			s.rs[i] = (dev != nullptr && dev->GetRenderState(kSnapRs[i], &v) == D3D_OK) ? unsigned(v) : DX8Wrapper::Get_DX8_Render_State(kSnapRs[i]);
		}
		s.ambient = DX8Wrapper::Get_Ambient();
		s.fog = DX8Wrapper::Get_Fog_Enable();
		s.fogColor = DX8Wrapper::Get_Fog_Color();
		for (int i = 0; i < 4; ++i)
			s.light[i] = DX8Wrapper::Is_Light_Enabled(i);
		const DX8Wrapper::GxShadowView v = DX8Wrapper::Gx_Shadows();
		for (int i = 0; i < 4; ++i)
		{
			s.sentOn[i] = v.lightEnables[i];
			s.sentValid[i] = v.lightValid[i];
			s.sent[i] = v.lights[i];
		}
	}
	// bit i set when item i differs: 0 view, 1 proj, 2 viewport, 3-9 the render states, 10 ambient, 11 fog enable, 12 fog colour, 13-16 lights
	// (render_state's enables), 17-20 the lights the device holds (enable, and the parameters of an enabled one) - A64
	uint32_t diffState(const StateSnap &a, const StateSnap &b)
	{
		uint32_t d = 0;
		if (std::memcmp(&a.view, &b.view, sizeof(a.view)) != 0) d |= 1u << 0;
		if (std::memcmp(&a.proj, &b.proj, sizeof(a.proj)) != 0) d |= 1u << 1;
		if (a.vpOk != b.vpOk || std::memcmp(&a.vp, &b.vp, sizeof(a.vp)) != 0) d |= 1u << 2;
		for (int i = 0; i < 7; ++i)
			if (a.rs[i] != b.rs[i]) d |= 1u << (3 + i);
		if (!(a.ambient == b.ambient)) d |= 1u << 10;
		if (a.fog != b.fog) d |= 1u << 11;
		if (a.fogColor != b.fogColor) d |= 1u << 12;
		for (int i = 0; i < 4; ++i)
			if (a.light[i] != b.light[i]) d |= 1u << (13 + i);
		for (int i = 0; i < 4; ++i)
			if (a.sentOn[i] != b.sentOn[i] || (a.sentOn[i] && (a.sentValid[i] != b.sentValid[i] ||
					std::memcmp(&a.sent[i], &b.sent[i], sizeof(D3DLIGHT8)) != 0)))
				d |= 1u << (17 + i);
		return d;
	}
	uint32_t popcount32(uint32_t v) { uint32_t n = 0; for (; v; v &= v - 1) ++n; return n; }
	bool xState()   // TEST-ONLY GX_SHADOWMAP_X_STATE=1 (A50 (3)'s X-): the stage perturbs D3DRS_AMBIENT and skips that restore
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_X_STATE");
			s = (e && !strcmp(e, "1")) ? 1 : 0;
			if (s)
				fprintf(stderr, "[GXSHADOWMAP] TEST-ONLY GX_SHADOWMAP_X_STATE=1: the off-screen stage leaves D3DRS_AMBIENT perturbed\n");
		}
		return s == 1;
	}
	bool emitterProbe()   // TEST-ONLY GX_SHADOWMAP_EMITTER_PROBE=1 (A50 (5))
	{
		static int s = -1;
		if (s < 0)
		{
			const char *e = getenv("GX_SHADOWMAP_EMITTER_PROBE");
			s = (e && !strcmp(e, "1")) ? 1 : 0;
		}
		return s == 1;
	}
	void restoreState(const StateSnap &was)
	{
		StateSnap now;
		snapState(now);
		const uint32_t moved = diffState(was, now);
		const bool x = xState();
		if (moved & (1u << 0))
			DX8Wrapper::Set_Transform(D3DTS_VIEW, was.view);
		if (moved & (1u << 1))
			DX8Wrapper::Set_Transform(D3DTS_PROJECTION, was.proj);
		if ((moved & (1u << 2)) && was.vpOk)
		{	// the PRODUCT off-screen stage restores through the Renderer seam (render-seam-lint DIRECT); the
			// snapshot's D3DVIEWPORT8 round-trips exactly (unsigned X / Y / Width / Height, float MinZ / MaxZ), as W3DWater's sky pass does
			const RenderViewport rv = { (unsigned int)was.vp.X, (unsigned int)was.vp.Y, (unsigned int)was.vp.Width, (unsigned int)was.vp.Height,
				was.vp.MinZ, was.vp.MaxZ };
			Renderer::Set_Viewport(rv);
		}
		if ((moved & (1u << 10)) && !x)
			Renderer::Set_Ambient(was.ambient);
		for (int i = 0; i < 7; ++i)
			if ((moved & (1u << (3 + i))) && !(x && kSnapRs[i] == D3DRS_AMBIENT))
				DX8Wrapper::Set_DX8_Render_State(kSnapRs[i], was.rs[i]);
		for (int i = 0; i < 4; ++i)   // A64: the device's lights back, through the wrapper (its cache stays the truth); unknown parameters
			if (moved & (1u << (17 + i)))   // (an enabled slot that was not valid) cannot be put back - they stay in state_mismatch
			{
				if (was.sentOn[i] && was.sentValid[i])
				{
					D3DLIGHT8 l = was.sent[i];
					DX8Wrapper::Set_DX8_Light(i, &l);
				}
				else if (!was.sentOn[i])
					DX8Wrapper::Set_DX8_Light(i, nullptr);
			}
		if (offscreenCheck())
		{
			StateSnap after;
			snapState(after);
			s_stateMovedPeriod += popcount32(moved);
			s_stateMismatchPeriod += popcount32(diffState(was, after));
		}
	}

	// GXShadowMapOffscreen.h's operations on the real engine (the order and its mutants are host-tested there)
	void smSetTags(uint32_t t);
	struct OffscreenOps
	{
		RenderInfoClass *rinfo;
		CameraClass *mainCam;
		bool queuesEmpty()
		{	// no pending drawable work, no sorted polygons, and no flush-site tag the scene set for ITS flush
			TheDX8MeshRenderer.Gx_List_Pending(s_pending);
			return s_pending.empty() && SortingRendererClass::Gx_Is_Empty() && GxFlushSiteNext == GXFS_UNSET;
		}
		void *suspendMotion() { GxMotionHooks *m = GxMotion; GxMotion = nullptr; return m; }
		void restoreMotion(void *m) { GxMotion = static_cast<GxMotionHooks *>(m); }
		void setTags(uint32_t t) { smSetTags(t); }
		bool staticSort() { return WW3D::Are_Static_Sort_Lists_Enabled(); }
		void setStaticSort(bool on) { WW3D::Enable_Static_Sort_Lists(on); }
		void render(size_t i) { s_offMeshes[i]->Render(*rinfo); }
		void *useCasterCamera() { CameraClass *prev = TheDX8MeshRenderer.Peek_Camera(); TheDX8MeshRenderer.Set_Camera(mainCam); return prev; }
		void restoreCamera(void *c) { TheDX8MeshRenderer.Set_Camera(static_cast<CameraClass *>(c)); }
		void flushMeshes() { TheDX8MeshRenderer.Flush(); }
		void flushSorted() { SortingRendererClass::Flush(); }
		void clearOwn() { setOwn(0); }
		void renderGroup(size_t g);    // A61: the sorted phase (defined with the groups below)
		void setGroupOwn(size_t g);
	};

	// A61: the sorted phase's operations (GXShadowMapOffscreen.h run): a group's meshes Render()ed (they queue into the sorting renderer at the
	// mesh flush), and its own light set for its sorting flush (containerEnd zeroed it at the mesh flush)
	void OffscreenOps::renderGroup(size_t g)
	{
		if (g == 0)
			s_sortedT0 = std::chrono::steady_clock::now();   // A62 (5): sorted_us = the first group's Render to the stage's end
		for (size_t i = s_groupStart[g]; i < s_groupStart[g + 1]; ++i)
			s_sorted[i].mesh->Render(*rinfo);
	}
	void OffscreenOps::setGroupOwn(size_t g) { setOwn(s_sorted[s_groupStart[g]].own); }

	// A52 phase 1, right after the walk and BEFORE the fit: the caster meshes of every posed candidate (as HLodClass::
	// Render picks them, the filters, the clean-chain transform read, the mesh sphere), stored; their union widens the walk's near distance
	// and slab top, so the fit encloses what S7 captures (not only classic's LOD-0 geometry)
	struct Collected { MeshClass *mesh; SphereClass sp; uint32_t root; bool sorted, lodDiff; };   // A61/A63: sorted, under a non-top LOD
	std::vector<Collected> s_collected;
	double s_collectUs = 0.0;
	void collectCasters(const Vector3 &L, const Vector3 &axis)
	{
		const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		s_collected.clear();
		for (size_t i = 0; i < s_walk.roots.size(); ++i)
		{
			if (!s_walk.cand[i] || !s_walk.posed[i])   // A48: classic casts nothing for a root none of whose geometry meshes is visible
				continue;
			++s_offCandPeriod;
			s_leaves.clear();
			s_leafLodDiff.clear();
			collectLeaves(s_walk.roots[i], -1);
			for (size_t li = 0; li < s_leaves.size(); ++li)
			{
				MeshClass *mesh = static_cast<MeshClass *>(s_leaves[li]);
				if (!mesh->Is_Not_Hidden_At_All())   // A48: right before its own transform read, after every earlier read (as classic)
					continue;
				MeshModelClass *model = mesh->Peek_Model();
				// what the visible path could tag CAST: no skin, no sort level (dx8renderer.cpp:2130 sends sorted meshes past the hook), the
				// volume code's filter
				if (model == nullptr || model->Get_Flag(MeshModelClass::SKIN) || model->Get_Sort_Level() != SORT_LEVEL_NONE || !casterFilter(mesh))
					continue;
				// A48: a read that would evaluate a pose classic's trigger (the walk) did not is never made - the mesh is not collected (and
				// has no current bound: the residual is skip_dirty itself)
				if (chainDirty(mesh))
				{
					++s_skipDirtyPeriod;
					continue;
				}
				(void)mesh->Get_Transform();   // clean chain: no pose evaluated; the cached sphere is current after it
				const SphereClass &sp = mesh->Get_Bounding_Sphere();
				// A61: a SORT mesh goes to the sorting renderer past the CAST hook (dx8renderer.cpp:2130) - the sorted phase captures it
				const bool sorted = model->Get_Flag(MeshModelClass::SORT) && WW3D::Is_Sorting_Enabled();
				Collected c = { mesh, sp, uint32_t(i), sorted, s_leafLodDiff[li] != 0 };
				s_collected.push_back(c);
				Real d = (sp.Center.X - L.X) * axis.X + (sp.Center.Y - L.Y) * axis.Y + (sp.Center.Z - L.Z) * axis.Z - sp.Radius;
				if (GXShadowFit::ownPullApplies(GXShadowFit::unpackOwnLight(s_walk.own[i]).mu, GXShadowFit::kOwnPullMuMax, s_pullEps))
					d = GXShadowFit::pulledNear(d, sp.Radius, s_pullEps);   // A79b #8: a pulled root's mesh, pulled as its copy replays
				const Real top = sp.Center.Z + sp.Radius;
				if (!s_walk.any || top > s_walk.topZ)
					s_walk.topZ = top;
				if (!s_walk.any || d < s_walk.nearDist)
					s_walk.nearDist = d;
				s_walk.any = TRUE;
			}
		}
		s_collectUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	}

	// phase 2, after the fit: the per-mesh decision on the stored spheres, then the isolated cast-only flush. f = this frame's map (the whole
	// cascade is the last one), axis = the (map, L) image plane's axis
	void offscreenCasters(CameraClass *cam, const GxShadowFrame &f, const GXShadowFit::Vec3 &L, const GXShadowFit::Vec3 &axis)
	{
		const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		const float *mWhole = f.lightViewProj[f.cascades - 1];
		const FrustumClass &mainF = cam->Get_Frustum();
		const bool check = offscreenCheck();
		s_offMeshes.clear();
		s_sorted.clear();
		s_groupStart.clear();
		uint32_t sortedIn = 0, sortedOff = 0, sortedLodDiff = 0;
		{
			for (const Collected &c : s_collected)
			{
				MeshClass *mesh = c.mesh;
				const SphereClass &sp = c.sp;
				const GXShadowFit::OwnLight o = GXShadowFit::unpackOwnLight(s_walk.own[c.root]);
				const bool drawsRoot = sceneDrawsRoot(s_walk.roots[c.root]);
				const bool lightOut = GXShadowFit::sphereOutsideClip(mWhole,
					GXShadowFit::ownSphereCentre(o, GXShadowFit::v3(sp.Center.X, sp.Center.Y, sp.Center.Z)), sp.Radius);
				const SphereClass grown(sp.Center, sp.Radius + GXShadowFit::insideMargin(sp.Radius));
				const bool grownInside = !lightOut && drawsRoot && CollisionMath::Overlap_Test(mainF, grown) == CollisionMath::INSIDE;
				const GXShadowFit::OffscreenPick pk = GXShadowFit::meshPick(lightOut, drawsRoot, grownInside, c.sorted);
				if (c.sorted)
				{	// A61/A62 (6): on or off screen (the scene's draw never captures it); N0 (GX_SHADOWMAP_OFFSCREEN=0) drops the off-view ones only;
					// GX_SHADOWMAP_CAST=none (M3's Z arm) captures nothing at all
					const bool inView = drawsRoot && grownInside;
					// A65: GX_SHADOWMAP_DROP_CAST reaches sorted casters too (they bypass meshBegin); collection - and so the
					// fit's bounds - keeps them
					if (pk == GXShadowFit::kSubmit && !castNone() && (inView || !offscreenOff()) &&
						!W3DVolumetricShadowManager::gxShadowDropMesh(mesh->Get_Name(), TRUE))
					{
						SortedCaster sc = { mesh, s_walk.own[c.root] };
						s_sorted.push_back(sc);
						++(inView ? sortedIn : sortedOff);   // A64: added to the census only if the stage accepts the frame
						sortedLodDiff += c.lodDiff ? 1u : 0u;
					}
				}
				else if (pk == GXShadowFit::kSubmit)
					s_offMeshes.push_back(mesh);
				else if (pk == GXShadowFit::kSkipInView)
				{
					++s_offInviewPeriod;
					if (check)
					{
						mesh->Add_Ref();
						s_skipped.push_back(mesh);
					}
				}
			}
		}
		if (offscreenOff())
			s_offMeshes.clear();
		// A61: own-light groups - the sorting renderer interleaves nodes of different meshes by depth, so one own light per sorting flush
		std::stable_sort(s_sorted.begin(), s_sorted.end(), [](const SortedCaster &a, const SortedCaster &b) { return a.own < b.own; });
		for (size_t i = 0; i < s_sorted.size(); ++i)
			if (i == 0 || s_sorted[i].own != s_sorted[i - 1].own)
				s_groupStart.push_back(i);
		const size_t groups = s_groupStart.size();
		s_groupStart.push_back(s_sorted.size());
		if (!s_offMeshes.empty() || groups > 0)
		{
			if (s_wideCam == nullptr)
				s_wideCam = new CameraClass();
			Matrix3D m(true);
			m.Look_At_Dir(Vector3(L.x, L.y, L.z), Vector3(axis.x, axis.y, axis.z), 0.0f);
			s_wideCam->Set_Transform(m);
			s_wideCam->Set_View_Plane(Vector2(-4.0f, -4.0f), Vector2(4.0f, 4.0f));
			s_wideCam->Set_Clip_Planes(1.0f, 1.0e6f);
			RenderInfoClass rinfo(*s_wideCam);
			OffscreenOps ops = { &rinfo, cam };
			StateSnap was;
			snapState(was);
			const GXShadowOffscreen::Result r = GXShadowOffscreen::run(ops, s_offMeshes.size(), groups, GX_SHADOW_TAG_CAST_ONLY,
				GX_SHADOW_TAG_CAST | GX_SHADOW_TAG_SOLID, GX_SHADOW_TAG_RECEIVE);
			if (r.groups > 0)
			{
				s_sortedUsPeriod += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s_sortedT0).count();
				s_sortedInPeriod += sortedIn;
				s_sortedOffPeriod += sortedOff;
				s_sortedLodDiffPeriod += sortedLodDiff;
			}
			s_sortedGroupsPeriod += r.groups;
			if (xState())	// TEST-ONLY plant (A50 (3)'s X-): a state the stage moved and does not put back
				DX8Wrapper::Set_DX8_Render_State(D3DRS_AMBIENT, was.rs[1] ^ 0x00010101u);
			restoreState(was);
			s_offMeshesPeriod += r.submitted;
			s_offDirtyPeriod += r.dirty;
		}
		s_offUsPeriod += s_collectUs + std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		s_collectUs = 0.0;
	}

	// TEST-ONLY check (A47): after the scene drew (and re-posed), a skipped mesh whose fresh box is OUTSIDE the camera was culled by the scene
	// after all - a gap the skip opened (skip_wrong)
	void offscreenCheckEnd()
	{
		if (s_skipped.empty())
			return;
		const FrustumClass *fr = s_frameCam != nullptr ? &s_frameCam->Get_Frustum() : nullptr;
		uint32_t wrong = 0;
		for (MeshClass *mesh : s_skipped)
		{
			if (fr != nullptr)
			{
				(void)mesh->Get_Transform();   // after the scene drew: its pose is the scene's (TEST-ONLY mode)
				++s_skipCheckedPeriod;
				if (CollisionMath::Overlap_Test(*fr, mesh->Get_Bounding_Box()) == CollisionMath::OUTSIDE)
					++wrong;
			}
			mesh->Release_Ref();
		}
		s_skipped.clear();
		s_skipWrongPeriod += wrong;
		if (wrong != 0)   // A48: a gap is printed the frame it is found, never only in a later period line
			fprintf(stderr, "[GXSHADOWMAP] skip_wrong frame=%u count=%u\n", s_frames, wrong);
	}

	void tacticalBeginImpl(CameraClass *cam)
	{
		GxShadowMapActive = false;
		GxShadowCast = nullptr;
		if (cam == nullptr || TheW3DShadowManager == nullptr || TheW3DVolumetricShadowManager == nullptr ||
			TheTerrainRenderObject == nullptr || !queryDevice())
			return;
		const Int pin = GXGraphics::shadowMapStepPin();
		const uint32_t step = pin >= 0 ? uint32_t(pin) : 0u;
		GxShadowFrame f = {};
		f.step = step;
		f.shadowColor = TheW3DShadowManager->getShadowColor();
		WorldHeightMap *hm = TheTerrainRenderObject->getMap();
		if (step >= 3 || hm == nullptr)
		{
			volumesFrame(f, step);   // off (or no terrain to place the image plane on): today's volumes draw
			return;
		}
		const Step &st = kSteps[step];
		if (step != s_lastStep)
		{
			s_fit[0] = GXShadowFit::State();
			s_fit[1] = GXShadowFit::State();
			s_lastStep = step;
		}
		// the terrain's world rectangle INCLUDING the border: vertex k sits at (k - border) x MAP_XY_FACTOR (BaseHeightMap.cpp:142
		// ADJUST_FROM_INDEX_TO_REAL); the render object's bounding box starts at 0 and would cut the negative border off
		const float border = float(hm->getBorderSizeInline());
		float clampXY[4];
		clampXY[0] = -border * MAP_XY_FACTOR;
		clampXY[1] = -border * MAP_XY_FACTOR;
		clampXY[2] = (float(hm->getXExtent() - 1) - border) * MAP_XY_FACTOR;
		clampXY[3] = (float(hm->getYExtent() - 1) - border) * MAP_XY_FACTOR;
		float zLo = 0.0f, zHi = 0.0f;
		terrainRange(&zLo, &zHi);
		// A39: 2003's point light. The image plane is fixed per (map, L); a new map or a time-of-day change makes it again and clears the
		// latch; the cone guard runs once per (map, L)
		const Vector3 &lp = TheW3DShadowManager->getLightPosWorld(0);
		const GXShadowFit::Vec3 L = GXShadowFit::v3(lp.X, lp.Y, lp.Z);
		if (hm != s_key.map || L.x != s_key.L[0] || L.y != s_key.L[1] || L.z != s_key.L[2])
		{
			s_key = PointKey();
			s_key.map = hm;
			s_key.L[0] = L.x;
			s_key.L[1] = L.y;
			s_key.L[2] = L.z;
			s_pb = GXShadowFit::pointBasis(L, GXShadowFit::v3(0.5f * (clampXY[0] + clampXY[2]), 0.5f * (clampXY[1] + clampXY[3]), 0.5f * (zLo + zHi)));
			s_fit[0] = GXShadowFit::State();
			s_fit[1] = GXShadowFit::State();
			const float cone = GXShadowFit::coneHalfAngleDeg(s_pb, clampXY, zLo, zHi + 200.0f);
			fprintf(stderr, "[GXSHADOWMAP] point light L=%.0f,%.0f,%.0f cone=%.1f deg (guard %.0f) axis=%.3f,%.3f,%.3f\n", L.x, L.y, L.z, cone,
				GXShadowFit::kConeMaxDeg, s_pb.b.f.x, s_pb.b.f.y, s_pb.b.f.z);
			if (!(cone <= GXShadowFit::kConeMaxDeg))
				latch("cone deg >", cone);
		}
		if (s_key.latched)
		{
			++s_latchedFrames;
			volumesFrame(f, step);
			return;
		}
		// the walk: casters, their own lights, the runtime clamps 2003 computes now (inside the visible-terrain box renderShadows uses)
		AABoxClass visBox;
		TheTerrainRenderObject->getMaximumVisibleBox(cam->Get_Frustum(), &visBox, TRUE);
		const bool sun = sunMode();
		const GXShadowFit::Vec3 sunDir = GXShadowFit::normalize(GXShadowFit::v3(-L.x, -L.y, -L.z));   // from the sun INTO the scene
		const GXShadowFit::Vec3 walkAxis = sun ? sunDir : s_pb.b.f;
		{	// A79a: eps from the camera's centre ray on the terrain's mid height, BEFORE the walk (nothing it computes feeds it); never in sun mode
			const Matrix3D &ct0 = cam->Get_Transform();
			const Vector3 e0 = ct0.Get_Translation(), cd = ct0.Rotate_Vector(Vector3(0.0f, 0.0f, -1.0f));
			const float delta = ownPullDelta();
			s_pullEps = sun ? 0.f : GXShadowFit::ownPullEps(L, GXShadowFit::v3(e0.X, e0.Y, e0.Z), GXShadowFit::v3(cd.X, cd.Y, cd.Z),
				0.5f * (zLo + zHi), s_pb.dCentre, delta, &s_pullD);
		}
		TheW3DVolumetricShadowManager->gxShadowMapWalk(s_walk, lp, Vector3(walkAxis.x, walkAxis.y, walkAxis.z), visBox, s_pullEps,
			GXShadowFit::kOwnPullMuMax);
		s_collected.clear();
		if (!castNone())   // A52 phase 1: S7's own caster meshes widen the walk's bounds before the fit (same axis and L as the walk)
			collectCasters(lp, Vector3(walkAxis.x, walkAxis.y, walkAxis.z));
		if (ownOff() && !sun)   // A69 TEST-ONLY: no own lights (the D69 arm No); anyOwn / minZ0 are KEPT so the fit is the control's
			std::fill(s_walk.own.begin(), s_walk.own.end(), uint64_t(0));
		if (sun)
		{	// Sun mode has no own lights. The walk's distance is dot(c - L, sun) - r; the near depth here is from the origin, dot(c, sun) - r
			std::fill(s_walk.own.begin(), s_walk.own.end(), uint64_t(0));
			s_walk.anyOwn = FALSE;
			s_walk.nearDist += GXShadowFit::dot(L, sunDir);
		}
		// the receiver slab: the terrain's height range (re-scanned: craters), its top raised to the highest caster - a roof, a bridge-top
		// unit or an aircraft between the eye and the bottom ray's ground hit is a visible receiver too - and its
		// bottom lowered to the lowest own-light plane, so every scaled lookup stays inside the prism
		if (s_walk.any && s_walk.topZ > zHi)
			zHi = s_walk.topZ;
		if (s_walk.anyOwn && s_walk.minZ0 < zLo)
			zLo = s_walk.minZ0;
		// L must stand above everything it lights
		if (!(L.z > zHi + 1.0f))
		{
			latch("light not above the highest caster: L.z - top =", L.z - zHi);
			++s_latchedFrames;
			volumesFrame(f, step);
			return;
		}
		// the camera's corner rays: W3D cameras look down -Z in camera space; the view plane at distance 1 is [vmin, vmax]
		Vector2 vmin, vmax;
		cam->Get_View_Plane(vmin, vmax);
		const Matrix3D &ct = cam->Get_Transform();
		const Vector3 eye = ct.Get_Translation();
		GXShadowFit::Vec3 dir[4];
		const float xs[2] = { vmin.X, vmax.X }, ys[2] = { vmin.Y, vmax.Y };
		for (int i = 0; i < 4; ++i)
		{
			// dir[0..1] = bottom corners (the near edge for a down-pitched RTS camera), dir[2..3] = top (GXShadowMapFit.h footprint)
			const Vector3 d = ct.Rotate_Vector(Vector3(xs[i & 1], ys[(i >> 1) & 1], -1.0f));
			dir[i] = GXShadowFit::v3(d.X, d.Y, d.Z);
		}
		const GXShadowFit::Vec3 e = GXShadowFit::v3(eye.X, eye.Y, eye.Z);
		GXShadowFit::Params pr;
		pr.res = st.res;
		pr.guard = st.guard;
		// the camera's own far clip DEPTH (the rays are not normalized: camera-space z = -1); a fixed 20000
		// along unit rays made cascade 0 10,752 wu wide
		float zNearCam = 0.0f, farClip = 0.0f;
		cam->Get_Clip_Planes(zNearCam, farClip);
		// cascade slots: step 0 = [0] near part (cut 0.4) + [1] the whole footprint; steps 1-2 = [0] the whole footprint (A12)
		const uint32_t n = st.cascades;
		for (uint32_t c = 0; c < n; ++c)
		{
			const bool whole = n == 1 || c == 1;
			const GXShadowFit::Footprint fp = GXShadowFit::footprint(e, dir, zLo, zHi, farClip, whole ? 1.0f : 0.4f, clampXY);
			if (sun)
			{	// TEST-ONLY: orthographic fit along the sun (the receivers' orthographic path: nearFar 0)
				const GXShadowFit::Result o = GXShadowFit::fit(GXShadowFit::basis(sunDir), fp, s_walk.nearDist, s_walk.any != FALSE, pr, &s_fit[c]);
				std::memcpy(f.lightViewProj[c], o.m, sizeof(o.m));
				f.depthRangeWU[c] = o.zFar - o.zNear;
				f.texelWU[c] = o.texel;
				f.rect[c][0] = c == 0 ? 0u : 2048u;
				f.rect[c][1] = 0u;
				f.rect[c][2] = st.res;
				f.rect[c][3] = st.res;
				continue;
			}
			const GXShadowFit::PointResult r = GXShadowFit::fitPoint(s_pb, fp, s_walk.nearDist, s_walk.any != FALSE, pr, &s_fit[c]);
			if (!r.ok)
			{
				latch("a receiver at or behind the light's plane, cascade", float(c));
				++s_latchedFrames;
				volumesFrame(f, step);
				return;
			}
			std::memcpy(f.lightViewProj[c], r.m, sizeof(r.m));
			f.tanTexel[c] = r.tanTexel;
			f.nearFar[c][0] = r.zNear;
			f.nearFar[c][1] = r.zFar;
			f.texelWU[c] = r.tanTexel * s_pb.dCentre;   // informational (the line): a texel at the axis centre
			f.depthRangeWU[c] = r.zFar - r.zNear;
			f.rect[c][0] = c == 0 ? 0u : 2048u;
			f.rect[c][1] = 0u;
			f.rect[c][2] = st.res;
			f.rect[c][3] = st.res;
		}
		f.cascades = n;
		f.guard = st.guard;
		f.depthBias[0] = st.bias[0] * biasScale();   // A69 TEST-ONLY scale (1 unless GX_SHADOWMAP_BIAS_SCALE)
		f.depthBias[1] = biasClamp(st.bias[1]);   // A73: the step's clamp, never scaled (TEST-ONLY override: GX_SHADOWMAP_BIAS_CLAMP)
		f.depthBias[2] = st.bias[2] * biasScale();
		if (s_pullEps > 0.f)
		{	// L, eps, muMax for the caster replay and the receivers; ownPullAux[1] (TEST-ONLY noRecv) stays 0 here ALWAYS
			f.ownPull[0] = L.x;
			f.ownPull[1] = L.y;
			f.ownPull[2] = L.z;
			f.ownPull[3] = s_pullEps;
			f.ownPullAux[0] = GXShadowFit::kOwnPullMuMax;
		}
		s_frame = f;
		DX8Wrapper::Device_Call(&frameCall, &s_frame);
		s_tags = 0;
		GxShadowMapActive = true;
		GxShadowCast = &s_castHooks;
		// A44/A46: the off-screen casters, captured in an isolated cast-only flush BEFORE the scene submits anything (it ends on RECEIVE)
		s_frameCam = cam;
		if (!castNone())
			offscreenCasters(cam, f, L, walkAxis);
		// 2003's receiver set = everything the main scene draws up to the stencil-shadow boundary
		// (W3DShadowMapStencilBoundary, DoShadows(rinfo, TRUE)); pre-transformed draws (the HUD, the team-colour quads) never
		// receive whatever the tag says (the translator's rule)
		setTags(GX_SHADOW_TAG_RECEIVE);
		s_ownCallsPeriod += s_ownCalls;
		s_ownCalls = 0;
		s_paddingPeriod += s_walk.paddingNow;
		if ((++s_frames % 120) == 1)
		{
			fprintf(stderr, "[GXSHADOWMAP] game step=%u cascades=%u roots=%u extent0=%.0f extent1=%.0f eye=%.0f,%.0f,%.0f far=%.0f "
				"L=%.0f,%.0f,%.0f near0=%.0f far0=%.0f near1=%.0f far1=%.0f color=%08x terrain=%.0f..%.0f xy=%.0f,%.0f..%.0f,%.0f "
				"clamped=%u/%u runtime_clamps_computed=%u conflicts=%u own_calls_per_frame=%.1f latched_frames=%u offscreen_cand=%u "
				"offscreen_meshes=%u offscreen_inview=%u castonly_dirty=%u offscreen_us=%.0f skip_checked=%u skip_wrong=%u skip_dirty=%u "
				"state_moved=%u state_mismatch=%u armed_unfired=%u gxgfxf_charge=terr hooks_l2=%d hooks_boundary=%d "
				"sorted_in=%u sorted_off=%u sorted_groups=%u sorted_lod_diff=%u sorted_us=%.0f pulled=%u/%u/%u pull_eps=%.7f pull_D=%.0f\n",
				step, n,
				(unsigned)s_walk.roots.size(), f.texelWU[0] * float(st.res), f.texelWU[1] * float(st.res), eye.X, eye.Y, eye.Z, farClip,
				L.x, L.y, L.z, f.nearFar[0][0], f.nearFar[0][1], f.nearFar[1][0], f.nearFar[1][1], (unsigned)f.shadowColor, zLo, zHi,
				clampXY[0], clampXY[1], clampXY[2], clampXY[3], s_walk.clampedTemplate, s_walk.clampedRuntime, s_paddingPeriod,
				s_walk.conflicts, s_frames > 1 ? double(s_ownCallsPeriod) / 120.0 : double(s_ownCallsPeriod), s_latchedFrames, s_offCandPeriod,
				s_offMeshesPeriod, s_offInviewPeriod, s_offDirtyPeriod, s_offUsPeriod, s_skipCheckedPeriod, s_skipWrongPeriod, s_skipDirtyPeriod,
				s_stateMovedPeriod, s_stateMismatchPeriod, s_armedUnfired,
				GxL2FlushHook != nullptr ? 1 : 0, DX8MeshRendererClass::GxBoundary != nullptr ? 1 : 0,
				s_sortedInPeriod, s_sortedOffPeriod, s_sortedGroupsPeriod, s_sortedLodDiffPeriod, s_sortedUsPeriod,
				s_walk.pulledAir, s_walk.pulledGround, s_walk.pulledRuntime, double(s_pullEps), double(s_pullD));
			s_sortedInPeriod = s_sortedOffPeriod = s_sortedGroupsPeriod = s_sortedLodDiffPeriod = 0;
			s_sortedUsPeriod = 0.0;
			s_stateMovedPeriod = s_stateMismatchPeriod = 0;
			s_censusPrinted = true;
			s_offCandPeriod = s_offMeshesPeriod = s_offInviewPeriod = s_offDirtyPeriod = s_skipCheckedPeriod = s_skipWrongPeriod = 0;
			s_skipDirtyPeriod = 0;
			s_offUsPeriod = 0.0;
			s_ownCallsPeriod = 0;
			s_paddingPeriod = 0;
		}
	}

	// A49/A50: fired by the scene after its On_Frame_Update loop (GxShadowMapFireArmed), key off too (it returns at once there): the emitter
	// probe reads first, then the frame, timed whole (frame_us)
	void tacticalBegin(CameraClass *cam)
	{
		if (emitterProbe() && GxShadowMapFrameScene != nullptr)
		{	// A52/A53: PARTICLES - live (Get_Particle_Count = NonNewNum + NewNum) plus those emitted this
			// frame still waiting in the new-particle queue (Gx_Pending_New_Count) - over every particle buffer of the scene that fired the frame;
			// read-only (no update is forced)
			// A55/A56: per ring (live, queued): count / unexpired / stamp sum / splitmix sum
			// (ParticleBufferClass::Gx_Probe, read-only)
			unsigned n[2] = { 0, 0 }, un[2] = { 0, 0 }, buffers = 0;
			unsigned long long ss[2] = { 0, 0 }, sm[2] = { 0, 0 };
			const unsigned int now = WW3D::Get_Sync_Time();
			SceneIterator *it = GxShadowMapFrameScene->Create_Iterator(false);
			for (it->First(); !it->Is_Done(); it->Next())
			{
				RenderObjClass *o = it->Current_Item();
				if (o != nullptr && o->Class_ID() == RenderObjClass::CLASSID_PARTICLEBUFFER)
				{
					unsigned int bn[2], bu[2];
					unsigned long long bs[2], bm[2];
					static_cast<const ParticleBufferClass *>(o)->Gx_Probe(now, bn, bu, bs, bm);
					for (int r = 0; r < 2; ++r)
					{
						n[r] += bn[r]; un[r] += bu[r]; ss[r] += bs[r]; sm[r] += bm[r];
					}
					++buffers;
				}
			}
			GxShadowMapFrameScene->Destroy_Iterator(it);
			if (std::memcmp(n, s_emitN, sizeof(n)) != 0 || std::memcmp(un, s_emitU, sizeof(un)) != 0 || std::memcmp(ss, s_emitS, sizeof(ss)) != 0 ||
				std::memcmp(sm, s_emitM, sizeof(sm)) != 0)
			{
				fprintf(stderr, "[GXEMIT] f=%u live=%u/%u/%016llx/%016llx queued=%u/%u/%016llx/%016llx buffers=%u\n",
					TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u, n[0], un[0], ss[0], sm[0], n[1], un[1], ss[1], sm[1], buffers);
				std::memcpy(s_emitN, n, sizeof(n));
				std::memcpy(s_emitU, un, sizeof(un));
				std::memcpy(s_emitS, ss, sizeof(ss));
				std::memcpy(s_emitM, sm, sizeof(sm));
			}
		}
		s_started = true;
		const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		tacticalBeginImpl(cam);
		s_frameUsPeriod += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		if (s_censusPrinted)
		{	// A52: printed AFTER this frame is accumulated - the period that ends with the census line above it
			fprintf(stderr, "[GXSHADOWMAP] frame_us=%.0f (the whole start, %s)\n", s_frameUsPeriod, "gxgfxf_charge=terr");
			s_frameUsPeriod = 0.0;
			s_censusPrinted = false;
		}
	}

	// the map belongs to the tactical scene ONLY - a later pass (a crossfade's extra render, another view)
	// must draw today's volumes, so the flag is cleared here as well as set per frame
	void tacticalEnd()
	{
		if (!s_started)
		{	// A50 (4): armed, never fired - nothing of this frame ran, so nothing to clear; counted
			++s_armedUnfired;
			if (s_armedUnfired == 1 || s_armedUnfired % 120 == 0)
				fprintf(stderr, "[GXSHADOWMAP] armed_unfired=%u (the scene never started the frame)\n", s_armedUnfired);
			return;
		}
		s_started = false;
		offscreenCheckEnd();   // A47 TEST-ONLY (empty unless GX_SHADOWMAP_OFFSCREEN_CHECK=1)
		s_frameCam = nullptr;
		setTags(0);
		setOwn(0);
		GxShadowCast = nullptr;
		GxShadowMapActive = false;
	}

	GxShadowMapFrameHooks s_frameHooks = { tacticalBegin, tacticalEnd };
}

// DoShadows(rinfo, TRUE) calls this first (W3DShadow.cpp, ZH): the 2003 stencil-shadow boundary - nothing drawn after it (water,
// translucent meshes, particles) is a receiver. Through Device_Call like every tag (the cs stream, A36 item 7).
void W3DShadowMapStencilBoundary()
{
	if (GxShadowMapActive)
		setTags(s_tags & ~uint32_t(GX_SHADOW_TAG_RECEIVE));
}

// W3DShadowManager::init calls this (ZH). Installing is cheap and harmless when the device has no IGxShadowMap: tacticalBegin finds
// none and returns before touching anything.
void W3DShadowMapInstall()
{
	GxShadowMapFrame = &s_frameHooks;
}

// the device reference QueryInterface took is dropped with the device's resources (W3DShadowManager::
// ReleaseResources - device reset / loss) and at the manager's destruction, so the native device can be torn down; the next tactical
// frame queries again. Nothing is pending in DX8Ring here: the release paths run outside a frame.
void W3DShadowMapRelease()
{
	GxShadowCast = nullptr;
	GxShadowMapActive = false;
	for (MeshClass *m : s_skipped)	// A47 check mode: references held across a frame that a release cut short
		m->Release_Ref();
	s_skipped.clear();
	if (offscreenCheck())	// A48: the partial period's check totals are never lost
		fprintf(stderr, "[GXSHADOWMAP] release: partial period skip_checked=%u skip_wrong=%u skip_dirty=%u offscreen_meshes=%u\n",
			s_skipCheckedPeriod, s_skipWrongPeriod, s_skipDirtyPeriod, s_offMeshesPeriod);
	s_frameCam = nullptr;
	if (s_wideCam)
		s_wideCam->Release_Ref();   // A44's wide light camera
	s_wideCam = nullptr;
	if (s_sm)
		s_sm->Release();
	s_sm = nullptr;
	s_dev = nullptr;
	s_tags = 0;
	s_own = 0;
}
