// GeneralsX: L2 data contract, reference rules (render-seam/CONTRACT_L2.md, WS-G0).
//
// Which classic mesh-renderer draws the L2 scene renderer may take over in the HYBRID (L2 drawing inside DXVK's device at
// the original DX8MeshRendererClass::Flush points), and the bin key / per-instance split for the ones it takes. Header-only
// and engine-free on purpose: WS-G fills these structs from the classic objects at flush time (DX8TextureCategoryClass,
// PolyRenderTaskClass's MeshClass, the flush site and pass state); scripts/test/l2-binkey-test.cpp drives it on hand-made
// states. Nothing here is compiled into the game yet, and nothing the simulation reads is touched.
//
// The section numbers (§) are CONTRACT_L2.md's. Every rule names the classic line it mirrors (base render/integration
// 92a850536). GX_L2_MUTANT=<n> plants one defect per number, for the test's fail arms (l2-binkey-test.sh).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef GX_L2_MUTANT
#define GX_L2_MUTANT 0
#endif

namespace gxl2 {

struct Vec3 { float x, y, z; };
struct Aabb { Vec3 lo, hi; };

// Matrix3D's 3x4 row-major layout (WWMath matrix3d.h): rows 0..2 = [r0 r1 r2 t].
struct Mat34 { float m[3][4]; };

// §1.1 the flush a draw is in. Only the default scene pass's object flushes are claimable in phase 1.
enum FlushSite : uint8_t {
	SITE_UNSET = 0,			// a Flush nobody tagged - WW3D::Flush (ww3d.cpp:1068-1070, outside that file's digested regions) is left
							// untagged on purpose (render/ws-G's site tags are set by the callers): never claimed
	SITE_MAIN_OPAQUE,		// W3DScene.cpp:1083
	SITE_OCCL_TRANSL_PRE,	// :2687 - the bucket's opaque objects queued before a translucent occludee
	SITE_OCCL_TRANSL_STENCIL,	// :2694 - stencil-only (STENCILFUNC NEVER): never claimed
	SITE_OCCL_BUCKET,		// :2707
	SITE_OCCL_OTHERS,		// :2722
	SITE_OCCL_OCCLUDERS,	// :2746
	SITE_OCCL_EMPTY,		// :2753 (GX_OCCLSTATS only, draws nothing)
	SITE_OCCL_PLAIN,		// :2843
	SITE_TRANSLUCENT,		// :2948 flushTranslucentObjects (alpha-overridden objects that need no triangle sort)
	SITE_STATIC_SORT,		// static_sort_list.cpp:94
	SITE_PROJ_SHADOW,		// W3DProjectedShadow.cpp:1524 - inside DoShadows (W3DScene.cpp:1076), BEFORE the main flush: with
							// shadow decals on it drains the scene's queued base passes (rw1-G census, Mini G2: ~1967 tasks/frame)
};

inline bool siteClaimable(FlushSite s)
{
	switch (s) {
	case SITE_MAIN_OPAQUE: case SITE_OCCL_TRANSL_PRE: case SITE_OCCL_BUCKET:
	case SITE_OCCL_OTHERS: case SITE_OCCL_OCCLUDERS: case SITE_OCCL_PLAIN: case SITE_PROJ_SHADOW:
		return true;
	default:
		return false;
	}
}

// §1.1 / §2 E1: the state of the whole pass, read at the flush.
struct PassCtx {
	bool defaultPass = true;		// RTS3DScene m_customPassMode == SCENE_PASS_DEFAULT
	bool extraPassDisabled = true;	// Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE
	bool cullingInverted = false;	// ShaderClass::Is_Backface_Culling_Inverted() (water reflection, W3DWater.cpp:1553)
	bool texturingEnabled = true;	// WW3D::Is_Texturing_Enabled()
	bool coloringEnabled = false;	// WW3D::Is_Coloring_Enabled() (vertmaterial.cpp:953)
	bool debugSwitches = false;		// DX8RendererDebugger, Expose_Prelit, triangle draw off, DrawPolygonLowBoundLimit
	bool forceMultiply = false;		// DX8TextureCategoryClass::m_gForceMultiply (WorldBuilder only, dx8renderer.cpp:1744)
};

// The flush-level state the rules need. staticStateKnown is read at the claimed run's own position, after the boundary
// replay's Apply (§3.4): every RS/TSS slot the run's pipeline depends on is KNOWN in the device-truth table (§2 E2) - seeded from
// DXVK's default state right after a successful CreateDevice / D3D Reset (before the engine's own default writes; a failed
// Reset makes every slot UNKNOWN), followed through every wrapper write, untouched by
// Invalidate_Cached_Render_States (which only empties the wrapper's cache, dx8wrapper.cpp:1654-1670), made UNKNOWN by an
// audited raw writer behind the wrapper. NOT "the cache shadow is not the 0x12345678 sentinel": a slot nobody writes
// (NORMALIZENORMALS on an unscaled frame, CLIPPLANEENABLE always) keeps the sentinel forever (rw1-G, 10/10).
struct FlushCtx {
	FlushSite site = SITE_MAIN_OPAQUE;
	PassCtx pass;
	bool staticStateKnown = true;
	uint32_t zbias = 0;					// D3DRS_ZBIAS from the DEVICE-TRUTH table, never the wrapper cache (decals set 8, :2254)
	uint32_t clipPlaneEnable = 0;		// D3DRS_CLIPPLANEENABLE from the device-truth table (no live writer: DXVK's default 0)
	Mat34 camera = {};					// TheDX8MeshRenderer.Peek_Camera()->Get_Transform() AT THE FLUSH (dx8renderer.cpp:1856)
};

// The EFFECTIVE depth/blend/colour/stencil state of the run, decoded from the device-truth table (E2) after the replay (§3.4) - never
// from the wrapper's RenderStates cache (sentinels, H10), and never
// from the ShaderClass bits: ShaderClass::Apply leaves raw writes standing when the bits match its cache (shader.cpp:424), e.g.
// the shroud pass's ZFUNC EQUAL (W3DShaderManager.cpp:1304) or an RGB-only COLORWRITEENABLE (FlatHeightMap.cpp:539) - review
// contract r2 #3. The bits stay the key.
struct ShaderDesc {
	uint32_t bits = 0;			// ShaderClass bits, the key
	bool blendOff = true;		// ALPHABLENDENABLE FALSE, or src ONE / dst ZERO
	bool zEnable = true;		// ZENABLE
	bool depthWrite = true;		// ZWRITEENABLE
	bool depthCmpLess = true;	// ZFUNC LESS or LESSEQUAL
	bool colorWrite = true;		// COLORWRITEENABLE writes red, green and blue - RGB or RGBA (r9: the terrain leaves RGB standing, §3.3)
	uint8_t colorMask = 0xF;	// COLORWRITEENABLE & 0xF: in the key, and two draws with different masks never reorder where they overlap
	bool stencilProfileOk = true;	// stencil off, or ALWAYS / KEEP on z-fail / REPLACE (every claimable site's profile, §3.3)
};

enum MapperKind : uint8_t { MAP_NONE, MAP_SCALE, MAP_OTHER };	// mapper.h:61-83; only SCALE is stateless (mapper.cpp:88)

// §1.3 the category: static state shared by every task in it (DX8TextureCategoryClass, dx8renderer.cpp:106-152).
struct CatState {
	const void *container = nullptr;	// DX8FVFCategoryContainer
	uint32_t fvf = 0;
	bool sortingContainer = false;		// DX8FVFCategoryContainer::Is_Sorting
	bool skinContainer = false;
	int pass = 0;						// 0..3
	const void *tex[2] = {};			// MeshMatDescClass::MAX_TEX_STAGES = 2 (meshmatdesc.h:68)
	const void *material = nullptr;		// the category's VertexMaterialClass* (first CRC-equal one, dx8renderer.cpp:208)
	MapperKind mapper[2] = { MAP_NONE, MAP_NONE };
	float mapperScale[2][2] = {};		// ScaleTextureMapperClass U,V
	uint8_t uvSource[2] = { 0, 1 };
	ShaderDesc shader;
	uint64_t effectiveState = 0;		// interned id of the run's post-replay RS/TSS/sampler snapshot (§1.4, §3.4): two eligible
										// states that differ only in, e.g., ZFUNC LESS vs LESSEQUAL need two pipelines
};

// §1.3 one PolyRenderTaskClass: a mesh instance drawn through one polygon renderer of the category.
struct TaskState {
	const void *mesh = nullptr;				// MeshClass*
	const void *polyRenderer = nullptr;		// DX8PolygonRendererClass* (index range + vertex offset)
	const void *object = nullptr;			// the root render object (HLOD tree) the mesh belongs to
	bool skin = false, sort = false, aligned = false, oriented = false;	// MeshModelClass flags (meshgeometry.h:102-110)
	bool sortingEnabled = true;				// WW3D::Is_Sorting_Enabled()
	bool staticSortLevel = false;			// Get_Sort_Level() != SORT_LEVEL_NONE (mesh.cpp:669)
	bool strip = false;						// DX8PolygonRendererClass strip (ENABLE_STRIPING is off)
	Mat34 transform = {};					// mesh->Get_Transform()
	bool transformIdentity = false;			// mesh->Is_Transform_Identity()
	float objectScale = 1.0f;				// mesh->Get_ObjectScale()
	float alphaOverride = 1.0f;				// mesh->Get_Alpha_Override()
	bool materialOverride = false;			// user data == USER_DATA_MATERIAL_OVERRIDE (dx8renderer.cpp:1908)
	bool hasLenv = true;					// mesh->Get_Lighting_Environment() != nullptr
	uint8_t lightCount = 0;					// LightEnvironmentClass: at most 4 (lightenvironment.h:117)
	uint8_t lightPoint = 0;					// bit l: light l is a point light
	int proceduralPasses = 0;				// MatPassTask entries of this mesh this frame (visible + delayed)
	bool onlyVisibleTask = true;			// the mesh has exactly ONE visible task this flush - one pass AND one category (§2 E8):
											// a mesh's polygons can sit in several categories of one pass (dx8renderer.cpp:1248,
											// mesh.cpp:747), so anything less lets an opaque part be taken while a blended or
											// later-refused part stays classic (contract r6 #2)
	bool vbOverflow = false;				// skins only: Base_Vertex_Offset == VERTEX_BUFFER_OVERFLOW
	Aabb bounds = {};						// world-space, conservative
};

struct Policy {
	bool skins = false;				// phase 1b: skins drawn from their CLASSIC CPU-deformed range of DXVK's dynamic VB
	bool gpuSkinning = false;		// phase 2: L2 deforms on the GPU (and the CPU deformation is skipped) - needs:
	bool gpuSkinExact = false;		// §2 E9: this platform's GPU skinning is bit-identical to get_deformed_vertices
	float tieEpsilon = 0.0f;		// §3.3 delta: depth-quantum bound, world units, added to every AABB
};

enum Reason : uint8_t {
	OK = 0,
	R_SITE, R_PASS, R_DEBUG, R_FORCE_MULTIPLY, R_STATE_UNKNOWN, R_AMBIENT,
	R_SORTED, R_STATIC_SORT, R_STRIP,
	R_BLENDED, R_NO_DEPTH_WRITE, R_DEPTH_FUNC, R_COLOR_MASK,
	R_ALPHA_OVERRIDE, R_MATERIAL_OVERRIDE, R_MAPPER_STATEFUL,
	R_NULL_LENV, R_PROCEDURAL_PASS, R_MULTI_TASK,
	R_SKIN_PHASE, R_SKIN_SORTED, R_SKIN_NOT_EXACT, R_SKIN_OVERFLOW,
	R_CATEGORY_HISTORY,
};

// §2: may L2 draw this task, judged alone. STATIC rules first (E1, E3, E5-E9: properties of the task, the category and the
// flush), then the DRAW-TIME ones (E2, E4: the effective state at the task's own draw) - so the census names the cause, not the
// symptom: an alpha-override task draws with the alpha shader (dx8renderer.cpp:1936), which E4 would report as R_BLENDED
// (rw1-G, 10/10). Claimable or not does not depend on the order; the first failing reason does.
inline Reason classifyTask(const FlushCtx &f, const CatState &c, const TaskState &t, const Policy &p)
{
	// E1
	if (!siteClaimable(f.site)) return R_SITE;
	if (!f.pass.defaultPass || !f.pass.extraPassDisabled || f.pass.cullingInverted
		|| !f.pass.texturingEnabled || f.pass.coloringEnabled) return R_PASS;
	if (f.pass.debugSwitches) return R_DEBUG;
	if (f.pass.forceMultiply) return R_FORCE_MULTIPLY;
#if GX_L2_MUTANT == 17
	if (!f.staticStateKnown) return R_STATE_UNKNOWN;	// r4's order: a draw-time rule before the static ones
#endif
	// E3
	if (t.staticSortLevel) return R_STATIC_SORT;
	// Render_Sorted (dx8renderer.cpp:1903): the triangles go to the global sorter, not to this flush - and for a skin the
	// sorter reads the CPU-deformed vertices (:1338-1386), so a sorted skin keeps CPU deformation (§2 E9)
	if ((t.sort && t.sortingEnabled) || c.sortingContainer) {
#if GX_L2_MUTANT == 6
		if (!t.skin)
#endif
		return t.skin ? R_SKIN_SORTED : R_SORTED;
	}
	if (t.strip) return R_STRIP;
	// E5
#if GX_L2_MUTANT != 5
	if (t.alphaOverride != 1.0f) return R_ALPHA_OVERRIDE;	// switches to the alpha shader + opacity + ALPHAREF (:1927-1946)
#endif
	if (t.materialOverride) return R_MATERIAL_OVERRIDE;		// UV offset + forced material re-apply (:1917-1955)
	// E6
	for (int s = 0; s < 2; ++s)
		if (c.mapper[s] == MAP_OTHER) return R_MAPPER_STATEFUL;
	// E7
	if (!t.hasLenv) return R_NULL_LENV;					// inherits the previous draw's lights (:1834-1841): phase 1 keeps
														// per-instance lights a function of the task alone
	// E8
	if (t.proceduralPasses != 0) return R_PROCEDURAL_PASS;	// shroud depth EQUAL etc. over the same geometry (§2 E8)
#if GX_L2_MUTANT != 18
	if (!t.onlyVisibleTask) return R_MULTI_TASK;
#endif
	// E9
	if (t.skin || c.skinContainer) {
		if (!p.skins) return R_SKIN_PHASE;
		if (p.gpuSkinning && !p.gpuSkinExact) return R_SKIN_NOT_EXACT;
		if (t.vbOverflow) return R_SKIN_OVERFLOW;
	}
	// E2 (draw time)
	if (!f.staticStateKnown) return R_STATE_UNKNOWN;
	if (f.zbias != 0 || f.clipPlaneEnable != 0) return R_AMBIENT;
	// E4 (draw time, EFFECTIVE state)
	if (!c.shader.blendOff) return R_BLENDED;
	if (!c.shader.zEnable || !c.shader.depthWrite) return R_NO_DEPTH_WRITE;
#if GX_L2_MUTANT != 13
	if (!c.shader.depthCmpLess) return R_DEPTH_FUNC;
#endif
	if (!c.shader.colorWrite) return R_COLOR_MASK;
	if (!c.shader.stencilProfileOk) return R_AMBIENT;
	return OK;
}

// A task whose applied state depends on the task drawn before it (H1: the lazily re-applied material, dx8wrapper.cpp:3650;
// H4). Census only since r1: the boundary replay (§3.4) leaves such a classic neighbour the history it had.
inline bool historySensitive(const TaskState &t)
{
	return t.alphaOverride != 1.0f || t.materialOverride;
}

// §1.3 the world matrix the classic code sends for this task (dx8renderer.cpp:1846-1890): a recipe, evaluated by WS-G with
// the engine's own Matrix3D::Obj_Look_At so the bits are the classic ones.
enum WorldMode : uint8_t { WORLD_TRANSFORM, WORLD_IDENTITY, WORLD_LOOKAT };
struct WorldRecipe {
	WorldMode mode = WORLD_TRANSFORM;	// what the device receives: IDENTITY whenever Set_World_Identity is called
	Vec3 p = {}, t = {};		// WORLD_LOOKAT: Obj_Look_At(p, t, 0.0f)
	bool setIdentity = false;	// Set_World_Identity instead of Set_Transform (:1883-1890)
};

inline Vec3 translation(const Mat34 &m) { return Vec3{ m.m[0][3], m.m[1][3], m.m[2][3] }; }
inline Vec3 zVector(const Mat34 &m) { return Vec3{ m.m[0][2], m.m[1][2], m.m[2][2] }; }	// Matrix3D::Get_Z_Vector

inline WorldRecipe resolveWorld(const TaskState &t, const FlushCtx &f)
{
	WorldRecipe r;
	// :1847 identity starts as the mesh's own flag; the billboard branches compute a look-at matrix but leave the flag
	// alone, so a billboard whose transform is identity is still sent Set_World_Identity (:1883) - review contract r1 #1
	r.setIdentity = t.transformIdentity;
	// the classic if / else-if order: ALIGNED, then ORIENTED, then SKIN
#if GX_L2_MUTANT == 2
	if (t.skin) { r.mode = WORLD_IDENTITY; r.setIdentity = true; return r; }
#endif
	if (t.aligned) {
		r.mode = WORLD_LOOKAT;
		r.p = translation(t.transform);
		const Vec3 z = zVector(f.camera);
		r.t = Vec3{ r.p.x + z.x, r.p.y + z.y, r.p.z + z.z };	// mesh_position + camera_z_vector (:1859)
#if GX_L2_MUTANT == 9
		r.setIdentity = false;	// r0's defect
#endif
	} else if (t.oriented) {
		r.mode = WORLD_LOOKAT;
		r.p = translation(t.transform);
#if GX_L2_MUTANT == 3
		const Vec3 z = zVector(f.camera);
		r.t = Vec3{ r.p.x + z.x, r.p.y + z.y, r.p.z + z.z };
#else
		r.t = translation(f.camera);	// camera_position (:1868-1871)
#endif
#if GX_L2_MUTANT == 9
		r.setIdentity = false;
#endif
	} else if (t.skin) {
		r.setIdentity = true;	// :1877-1879
	}
	if (r.setIdentity)
		r.mode = WORLD_IDENTITY;
	return r;
}

// §1.4 the bin key: everything one instanced draw shares. Per-instance data (world, lights + ambient values) is not in it.
struct BinKey {
	const void *container = nullptr;
	uint32_t fvf = 0;
	int pass = 0;
	const void *tex[2] = {};
	const void *material = nullptr;
	uint8_t mapper[2] = {};
	float mapperScale[2][2] = {};
	uint8_t uvSource[2] = {};
	uint32_t shaderBits = 0;
	uint64_t effectiveState = 0;
	bool normalizeNormals = false;	// D3DRS_NORMALIZENORMALS TRUE when Get_ObjectScale() != 1 (:1894)
	uint8_t colorMask = 0xF;		// r9: the pipeline draws with the effective colour write mask; a bin never mixes masks
	uint8_t lightCount = 0;			// DXVK's FF VS key carries the light count (d3d9_fixed_function.h:128); the TYPE is
									// selected at run time (d3d9_fixed_function.cpp:1287-1326), so it is instance data
	bool skinPath = false;
	const void *polyRenderer = nullptr;	// one instanced draw = one index range
	const void *skinMesh = nullptr;		// skins: each instance has its own deformed range (dx8renderer.cpp:1424), so a skin
										// bin holds one mesh - phase 1b draws skins one task per draw (contract r1 #2)

	bool operator==(const BinKey &o) const
	{
		return container == o.container && fvf == o.fvf && pass == o.pass && tex[0] == o.tex[0] && tex[1] == o.tex[1]
			&& material == o.material && mapper[0] == o.mapper[0] && mapper[1] == o.mapper[1]
			&& std::memcmp(mapperScale, o.mapperScale, sizeof(mapperScale)) == 0
			&& uvSource[0] == o.uvSource[0] && uvSource[1] == o.uvSource[1] && shaderBits == o.shaderBits
			&& effectiveState == o.effectiveState && normalizeNormals == o.normalizeNormals && colorMask == o.colorMask
			&& lightCount == o.lightCount
			&& skinPath == o.skinPath && polyRenderer == o.polyRenderer && skinMesh == o.skinMesh;
	}
	bool operator!=(const BinKey &o) const { return !(*this == o); }
};

inline BinKey binKey(const CatState &c, const TaskState &t)
{
	BinKey k;
	k.container = c.container;
	k.fvf = c.fvf;
	k.pass = c.pass;
	k.tex[0] = c.tex[0];
	k.tex[1] = c.tex[1];
	k.material = c.material;
	for (int s = 0; s < 2; ++s) {
		k.mapper[s] = c.mapper[s];
		k.uvSource[s] = c.uvSource[s];
		if (c.mapper[s] == MAP_SCALE) {
			k.mapperScale[s][0] = c.mapperScale[s][0];
			k.mapperScale[s][1] = c.mapperScale[s][1];
		}
	}
	k.shaderBits = c.shader.bits;
#if GX_L2_MUTANT != 16
	k.effectiveState = c.effectiveState;
#endif
#if GX_L2_MUTANT != 1
	k.normalizeNormals = t.objectScale != 1.0f;
#endif
#if GX_L2_MUTANT != 19
	k.colorMask = c.shader.colorMask;
#endif
#if GX_L2_MUTANT != 7
	k.lightCount = t.lightCount;
#endif
	k.skinPath = t.skin || c.skinContainer;
	k.polyRenderer = t.polyRenderer;
#if GX_L2_MUTANT != 11
	if (k.skinPath)
		k.skinMesh = t.mesh;
#endif
	return k;
}

// §3.3 two draws whose relative order may decide a pixel: opaque, depth-writing, LEQUAL draws commute except on an exact
// depth tie, which needs the two surfaces within a depth quantum of each other - so disjoint AABBs grown by epsilon cannot.
// Two meshes of one object are always treated as able to tie (coplanar overlays, multi-mesh art).
inline bool tieCapable(const TaskState &a, const TaskState &b, float eps)
{
	// a billboard's copied bounds were built from its own transform, but it is drawn with Obj_Look_At (dx8renderer.cpp:1859),
	// so an off-centre model sphere can leave them; never reorder one (contract r2 #4; none in G2, rw1-G census)
#if GX_L2_MUTANT != 14
	if (a.aligned || a.oriented || b.aligned || b.oriented) return true;
#endif
#if GX_L2_MUTANT != 8
	if (a.object != nullptr && a.object == b.object) return true;
#endif
#if GX_L2_MUTANT == 4
	return false;
#endif
	return a.bounds.lo.x - eps <= b.bounds.hi.x + eps && b.bounds.lo.x - eps <= a.bounds.hi.x + eps
		&& a.bounds.lo.y - eps <= b.bounds.hi.y + eps && b.bounds.lo.y - eps <= a.bounds.hi.y + eps
		&& a.bounds.lo.z - eps <= b.bounds.hi.z + eps && b.bounds.lo.z - eps <= a.bounds.hi.z + eps;
}

struct ClaimedDraw {
	BinKey key;
	TaskState task;
	int classicSeq = 0;		// position in the flush's classic draw order
};
struct Bin {
	BinKey key;
	std::vector<int> draws;	// indices into the input, in emission order
};

// §3.3 instancing without a forbidden reorder: draws arrive in classic order; a draw may join an earlier bin of its key only
// if it would not overtake a tie-capable draw, or a draw with another colour write mask (r9 - conservatively, as a ClaimedDraw carries
// no screen rectangle), already placed in a later bin. The emission order is the bins' order, each
// bin's members in classic order (Vulkan rasterises instances in order).
// Trade-off: O(draws x bins) bounding-box tests; a grid over the bounds when a flush's claimed count makes it show up.
inline std::vector<Bin> planBins(const std::vector<ClaimedDraw> &in, const Policy &p)
{
	std::vector<Bin> bins;
	for (int j = 0; j < (int)in.size(); ++j) {
		int target = -1;
		for (int b = (int)bins.size() - 1; b >= 0; --b) {
			if (bins[b].key != in[j].key) continue;
			bool blocked = false;
			for (int later = b + 1; later < (int)bins.size() && !blocked; ++later)
				for (int x : bins[later].draws)
					if (tieCapable(in[x].task, in[j].task, p.tieEpsilon)
#if GX_L2_MUTANT != 21
						|| in[x].key.colorMask != in[j].key.colorMask
#endif
						) { blocked = true; break; }
			if (!blocked) target = b;
			break;	// only the LAST bin of this key is a candidate: joining an older one overtakes that newer one
		}
		if (target < 0) {
			bins.push_back(Bin{ in[j].key, {} });
			target = (int)bins.size() - 1;
		}
		bins[target].draws.push_back(j);
	}
	return bins;
}

// §3.3 / §4.3 C3: the WHOLE emitted sequence of a flush - classic draws at their places, every packet's draws where the packet
// runs - checked against the classic order (contract r1 #6: packets checked one by one can still invert a pair that
// sits in two different packets). Every pair whose emitted order is the reverse of the classic order must commute.
struct SeqDraw {
	TaskState task;
	bool claimed = false;
	bool sensitive = false;		// blends, writes no depth, other depth func or colour mask (fails E4) - decals, procedural passes
	bool biased = false;		// a nonzero ZBIAS / depth bias (Render_Decal_Meshes, dx8renderer.cpp:2254): its depth is not its geometry's
	uint8_t colorMask = 0xF;	// COLORWRITEENABLE & 0xF (r9): with different masks the channel one draw leaves alone keeps an order-dependent value
	float screen[4] = {};		// conservative screen rectangle x0 y0 x1 y1 at the flush camera, +1 px guard
};

inline bool screenOverlap(const SeqDraw &a, const SeqDraw &b)
{
	return a.screen[0] <= b.screen[2] && b.screen[0] <= a.screen[2] && a.screen[1] <= b.screen[3] && b.screen[1] <= a.screen[3];
}

// Lemma (§3.3): two opaque depth-writing LEQUAL/LESS draws with the SAME colour write mask commute except on an exact depth tie; a
// sensitive or biased draw, or a pair with different masks (r9: destination alpha, read later by the soft water edge), commutes with
// another draw only where they cover no common pixel.
inline bool commutes(const SeqDraw &a, const SeqDraw &b, float eps)
{
	const bool billboard = a.task.aligned || a.task.oriented || b.task.aligned || b.task.oriented;
#if GX_L2_MUTANT != 14
	if (billboard) return false;	// its screen rectangle comes from the same non-conservative bounds
#else
	(void)billboard;
#endif
#if GX_L2_MUTANT == 10
	if (false)
#else
	if (a.sensitive || b.sensitive || a.biased || b.biased
#if GX_L2_MUTANT != 20
		|| a.colorMask != b.colorMask
#endif
		)
#endif
		return !screenOverlap(a, b);
	return !tieCapable(a.task, b.task, eps);
}

// classicOrder[i] = the flush's draws in classic order; emitted = indices into it in emission order, which must be a
// permutation: every draw exactly once. Returns -1 if it is not (a missing, duplicated or out-of-range draw - contract
// r2 #6), else the number of non-commuting pairs the emission inverts (0 = the emission is CLASSIC-equivalent).
// Trade-off: O(n^2) pairs - a check-mode tool; sort by screen tiles if a flush's draw count makes it slow.
inline int validateEmission(const std::vector<SeqDraw> &classicOrder, const std::vector<int> &emitted, float eps)
{
	std::vector<int> pos(classicOrder.size(), -1);
#if GX_L2_MUTANT != 15
	if (emitted.size() != classicOrder.size()) return -1;
#endif
	for (int k = 0; k < (int)emitted.size(); ++k) {
		const int e = emitted[k];
		if (e < 0 || e >= (int)classicOrder.size() || pos[e] >= 0) return -1;
		pos[e] = k;
	}
	int bad = 0;
	for (int i = 0; i < (int)classicOrder.size(); ++i)
		for (int j = i + 1; j < (int)classicOrder.size(); ++j) {
			if (pos[i] < pos[j]) continue;
#if GX_L2_MUTANT == 12
			if (classicOrder[i].claimed && classicOrder[j].claimed) continue;	// r0's per-packet view
#endif
			if (!commutes(classicOrder[i], classicOrder[j], eps)) ++bad;
		}
	return bad;
}

} // namespace gxl2
