// GeneralsX: The object-velocity bridge - see GxMotionUnits.h and
// (A35). Game side: it drives the device only through DX8Wrapper (whose caches
// and DX8Ring stay consistent) plus three direct calls after a ring drain (shader / texture creation, SetObjectVelocity).
#include "W3DDevice/GameClient/GXRender/Scene/GxMotionUnits.h"
#include "W3DDevice/GameClient/GXRender/Scene/GxMotionHistory.h"
#include "W3DDevice/GameClient/GxWorldPass.h"
#include "GxDxvkMath.h"            // GXRender/Pipeline: GxPackFFWorld's arithmetic, so the replay's depth is the FF draw's
#include "GxSyntheticPrograms.h"   // GXRender/Pipeline: the gx:vel-* blobs + declarations, GX_FOURCC_RGBA16F
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/Renderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace GXRender;

static_assert(sizeof(Matrix3D) == 12 * sizeof(float), "Matrix3D is 3 rows of 4 floats");
static_assert(sizeof(Matrix4x4) == 16 * sizeof(float), "Matrix4x4 is 4 rows of 4 floats");

const unsigned kSkinBufferVertices = 65535;   // DX8VertexBufferClass counts and Set_Index_Buffer's base are 16-bit

struct Rec
{
	VertexBufferClass *vb;     // rigid: the mesh's static VB; skin: the bridge's batch buffer. AddRef'd until the replay ends.
	IndexBufferClass *ib;      // static IB, AddRef'd
	unsigned short base;       // the base vertex the draw had (rigid: index_base_offset + vba_offset; skin: index_base_offset)
	unsigned prim, minVertex, vertexCount, startIndex, polyCount;
	D3DVIEWPORT8 vp;           // the draw's viewport and depth range (binding O resets them)
	unsigned cull, zbias;      // the draw's culling and z bias, so the replay rasterises what the draw did
	bool skin;
	float c[GX_VEL_CONSTANTS][4];
};

struct State
{
	float zEps = 0.0f;         // GX_MOTION_ZEPS: depth-tie calibration knob, default 0 (the position is FF-exact)
	bool zEpsRead = false;
	IGxWorldPass *wp = nullptr;
	bool open = false;
	uint32_t pass = 0;
	bool havePrevCam = false;
	float view[16], proj[16], prevView[16], prevProj[16];   // the world camera (D3D layout, unjittered): this pass / the previous
	float prevViewProj[16];    // previous view * projection (D3D layout), for skins
	float jitter[2] = { 0.0f, 0.0f };
	GxRigidHistory rigid;
	GxSkinHistory skin;
	// rigid: scoped to one mesh task (rigidBegin .. rigidEnd)
	bool pending = false;
	float pendingCur[16], pendingPrev[16];   // this task's world as Set_Transform stores it / its previous world (both D3D layout)
	// skin
	std::vector<float> staging;              // the batch being filled: (cur xyz, prev xyz) per vertex at its batch offset
	std::vector<float> prevTmp;
	unsigned batchVertices = 0;
	int skinBuffer = -1;                     // the batch buffer of the open skin batch (-1 = none open)
	std::vector<VertexBufferClass*> skinBuffers;
	size_t skinBuffersUsed = 0;
	std::vector<Rec> recs;
	void (*chained)(unsigned, unsigned, unsigned, unsigned, unsigned, bool) = nullptr;
	bool savedCapture = false;
	GxMotionHooks hooks = {};
	// O and the programs (created on first use, only with the key on)
	IDirect3DTexture8 *oTex = nullptr;
	IDirect3DSurface8 *o = nullptr;
	unsigned oW = 0, oH = 0;
	DWORD vsRigid = 0, vsSkin = 0, ps = 0;
	bool programs = false, programsFailed = false;
	// [GXMOTION] units, once per 600 passes
	uint64_t nPasses = 0, nRigid = 0, nSkin = 0, nSkipped = 0, nReplayed = 0;
};

State s;

int readKey()
{
	const char *k = std::getenv("GX_MOTION_UNITS");
	return k && std::strcmp(k, "1") == 0 ? 1 : 0;
}

void setViewport(const D3DVIEWPORT8 &v)   // through the Renderer seam (render-seam-lint: Set_Viewport is a routed method)
{
	RenderViewport r;
	r.x = v.X;
	r.y = v.Y;
	r.width = v.Width;
	r.height = v.Height;
	r.min_z = v.MinZ;
	r.max_z = v.MaxZ;
	Renderer::Set_Viewport(r);
}

void d3dOf(const Matrix3D &m, float out[16])   // exactly what Set_Transform(D3DTS_WORLD, m) stores in render_state.world
{
	const Matrix4x4 t = Matrix4x4(m).Transpose();
	std::memcpy(out, &t, 64);
}

void mul(const float a[16], const float b[16], float out[16])   // D3D row-vector a * b (velocity xy only: no bit-exactness needed)
{
	float r[16];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
	std::memcpy(out, r, sizeof(r));
}

void upload(float c[][4], int base, const float m[16])   // transposed: c[base + i] = column i (gx_cs_vel.vert)
{
	for (int i = 0; i < 4; ++i)
		for (int r = 0; r < 4; ++r)
			c[base + i][r] = m[r * 4 + i];
}

// The current position's two matrices as the FF draw got them: WorldView = GxPackFFWorld's dxm product of the view and world the
// wrapper applied, projection = the one it sent (GxProjSent, captured while the bridge is open) with this pass's jitter.
bool currentConstants(const float world[16], Rec &r)
{
	const RenderStateStruct &rs = DX8Wrapper::Gx_Render_State();
	if (!DX8Wrapper::GxProjSentValid || !DX8Wrapper::GxViewportSentValid)
		return false;
	float wv[16], p[16];
	dxm::store(dxm::loadD3D(reinterpret_cast<const float*>(&rs.view)) * dxm::loadD3D(world), wv);
	std::memcpy(p, &DX8Wrapper::GxProjSent, 64);
	if (s.jitter[0] != 0.0f || s.jitter[1] != 0.0f)
		for (int row = 0; row < 4; ++row) {   // GxWorldPassJitterProjection's arithmetic, on our copy
			p[row * 4 + 0] += p[row * 4 + 3] * s.jitter[0];
			p[row * 4 + 1] += p[row * 4 + 3] * s.jitter[1];
		}
	upload(r.c, 0, wv);
	upload(r.c, 4, p);
	r.vp = DX8Wrapper::GxViewportSent;
	r.c[12][0] = 0.5f * float(r.vp.Width);
	r.c[12][1] = -0.5f * float(r.vp.Height);
	r.c[12][2] = s.zEps;
	r.c[12][3] = 0.0f;
	r.c[13][0] = s.jitter[0];
	r.c[13][1] = s.jitter[1];
	r.c[13][2] = r.c[13][3] = 0.0f;
	const unsigned *rsCache = DX8Wrapper::Gx_Shadows().renderStates;   // 0x12345678 = unknown since an Invalidate (dx8wrapper.cpp)
	r.cull = rsCache[D3DRS_CULLMODE] == 0x12345678u ? unsigned(D3DCULL_NONE) : rsCache[D3DRS_CULLMODE];
	r.zbias = rsCache[D3DRS_ZBIAS] == 0x12345678u ? 0u : rsCache[D3DRS_ZBIAS];
	return true;
}

// GxDrawHook (dx8wrapper.h:249), installed between Begin and End: the draw has been applied (wrapper state = what it ran with).
void onDraw(unsigned prim, unsigned minVertex, unsigned vertexCount, unsigned startIndex, unsigned polyCount, bool sorting)
{
	if (s.chained)
		s.chained(prim, minVertex, vertexCount, startIndex, polyCount, sorting);
	if (sorting || !s.havePrevCam || (prim != D3DPT_TRIANGLELIST && prim != D3DPT_TRIANGLESTRIP))
		return;
	const RenderStateStruct &rs = DX8Wrapper::Gx_Render_State();
	const bool skinDraw = s.skinBuffer >= 0 && rs.vertex_buffer_types[0] == BUFFER_TYPE_DYNAMIC_DX8;
	if (!skinDraw && !(s.pending && std::memcmp(&rs.world, s.pendingCur, 64) == 0))
		return;   // not a candidate: nothing else is read or done for this draw
	// Only draws that test AND write depth carry a surface's motion: a no-depth-write pass (blended, or the
	// heat-vision-only pass W3DScene.cpp:920 draws instead of a stealthed unit's base pass) would overwrite what is behind it.
	// The wrapper cache can say "unknown" (0x12345678) after Invalidate_Cached_Render_States: ShaderClass re-sends ZWRITEENABLE
	// before every shaded draw, so an unknown ZWRITE is not recorded; ZENABLE is never re-sent (the game leaves it on), so unknown
	// counts as on (r3 #4: no device query).
	const unsigned *rsCache = DX8Wrapper::Gx_Shadows().renderStates;
	if (rsCache[D3DRS_ZWRITEENABLE] != TRUE || rsCache[D3DRS_ZENABLE] == FALSE)
		return;
	const bool staticIB = rs.index_buffer && rs.index_buffer_type == BUFFER_TYPE_DX8 && rs.iba_offset == 0;
	Rec r = {};
	if (skinDraw) {
		if (!staticIB) {
			++s.nSkipped;
			return;
		}
		float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };   // a skin draws with world = identity
		if (!currentConstants(identity, r)) {
			++s.nSkipped;
			return;
		}
		upload(r.c, 8, s.prevViewProj);
		r.vb = s.skinBuffers[size_t(s.skinBuffer)];
		r.base = rs.index_base_offset;   // the mesh's offset in the batch; the bridge's buffer starts at the batch's vertex 0
		r.skin = true;
		++s.nSkin;
	} else {   // the pending rigid mesh's draw (its applied world matched above)
		if (!staticIB || rs.vertex_buffer_types[0] != BUFFER_TYPE_DX8) {   // dynamic contents are gone by End
			++s.nSkipped;
			return;
		}
		if (!currentConstants(s.pendingCur, r)) {
			++s.nSkipped;
			return;
		}
		float prevClip[16];
		mul(s.pendingPrev, s.prevView, prevClip);
		mul(prevClip, s.prevProj, prevClip);
		upload(r.c, 8, prevClip);
		r.vb = rs.vertex_buffers[0];
		r.base = (unsigned short)(rs.index_base_offset + rs.vba_offset);
		r.skin = false;
		++s.nRigid;
	}
	r.ib = rs.index_buffer;
	r.prim = prim;
	r.minVertex = minVertex;
	r.vertexCount = vertexCount;
	r.startIndex = startIndex;
	r.polyCount = polyCount;
	r.vb->Add_Ref();
	r.ib->Add_Ref();
	s.recs.push_back(r);
}

void rigidBegin(const MeshClass *mesh, const void *model, const Matrix3D &world)
{
	float cur[12], prev[12];
	std::memcpy(cur, &world, sizeof(cur));
	const bool moved = s.rigid.update(mesh, model, cur, s.pass, prev);
	s.pending = moved && s.havePrevCam;
	if (s.pending) {
		d3dOf(world, s.pendingCur);
		Matrix3D p;
		std::memcpy(&p, prev, sizeof(prev));
		d3dOf(p, s.pendingPrev);
	}
}

void rigidEnd()
{
	s.pending = false;
}

void skinMesh(const MeshClass *mesh, const void *model, const Vector3 *pos, int count, unsigned batchOffset)
{
	const size_t n = size_t(count) * 3;
	if (s.prevTmp.size() < n)
		s.prevTmp.resize(n);
	s.skin.exchange(mesh, model, reinterpret_cast<const float*>(pos), count, s.pass, s.prevTmp.data());
	const unsigned end = batchOffset + unsigned(count);
	if (end > kSkinBufferVertices)
		return;   // the container clamps a batch to 65535 vertices; never reached
	if (s.staging.size() < size_t(end) * 6)
		s.staging.resize(size_t(end) * 6);
	float *d = s.staging.data() + size_t(batchOffset) * 6;
	const float *c = reinterpret_cast<const float*>(pos), *p = s.prevTmp.data();
	for (int v = 0; v < count; ++v, d += 6, c += 3, p += 3) {
		d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
		d[3] = p[0]; d[4] = p[1]; d[5] = p[2];
	}
	if (end > s.batchVertices)
		s.batchVertices = end;
}

void skinBatch(bool open)
{
	if (!open) {
		s.skinBuffer = -1;
		return;
	}
	const unsigned n = s.batchVertices;
	s.batchVertices = 0;
	if (n == 0 || !s.havePrevCam)
		return;
	// one retained buffer per batch per pass, each <= 65535 vertices, never grown mid-frame
	if (s.skinBuffersUsed == s.skinBuffers.size())
		s.skinBuffers.push_back(NEW_REF(DX8VertexBufferClass, (D3DFVF_XYZ | D3DFVF_NORMAL, (unsigned short)kSkinBufferVertices,
			DX8VertexBufferClass::USAGE_DYNAMIC)));
	VertexBufferClass *vb = s.skinBuffers[s.skinBuffersUsed];
	{
		VertexBufferClass::WriteLockClass lock(vb, D3DLOCK_DISCARD);
		if (!lock.Get_Vertex_Array())
			return;   // a failed lock: this batch stays camera-only
		std::memcpy(lock.Get_Vertex_Array(), s.staging.data(), size_t(n) * 6 * sizeof(float));
	}
	s.skinBuffer = int(s.skinBuffersUsed++);
}

bool ensureProgramsAndTarget(IDirect3DSurface8 *depth)
{
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();   // drains/falls back the ring before a direct call
	if (!s.programs && !s.programsFailed) {
		const bool ok = SUCCEEDED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(gxVelRigidDecl),
			reinterpret_cast<const DWORD*>(gxVelRigidVs), &s.vsRigid, 0)) &&
			SUCCEEDED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(gxVelSkinDecl), reinterpret_cast<const DWORD*>(gxVelSkinVs),
				&s.vsSkin, 0)) &&
			SUCCEEDED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(gxVelPs), &s.ps));
		s.programs = ok;
		s.programsFailed = !ok;
		if (!ok)
			std::fprintf(stderr, "[GXMOTION] units: the gx:vel programs could not be created - the bridge stays off\n");
	}
	if (!s.programs)
		return false;
	D3DSURFACE_DESC dd = {};
	if (!depth || FAILED(depth->GetDesc(&dd)))
		return false;
	if (s.oTex && (s.oW != dd.Width || s.oH != dd.Height)) {   // O's size is the world depth's (the device refuses a larger colour)
		s.o->Release();
		s.oTex->Release();
		s.o = nullptr;
		s.oTex = nullptr;
	}
	if (!s.oTex) {
		if (FAILED(dev->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFORMAT(GX_FOURCC_RGBA16F), D3DPOOL_DEFAULT,
			    &s.oTex)) || !s.oTex || FAILED(s.oTex->GetSurfaceLevel(0, &s.o))) {
			if (s.oTex)
				s.oTex->Release();
			s.oTex = nullptr;
			s.o = nullptr;
			std::fprintf(stderr, "[GXMOTION] units: O (%ux%u GXRA) could not be created\n", dd.Width, dd.Height);
			return false;
		}
		s.oW = dd.Width;
		s.oH = dd.Height;
	}
	return true;
}

void releaseRecords()
{
	for (Rec &r : s.recs) {
		r.vb->Release_Ref();
		r.ib->Release_Ref();
	}
	s.recs.clear();
}

// Uninstalls the hooks and forgets the pass's records without replaying them (a pass that never reached End, or a device reset).
void dropPass()
{
	s.open = false;
	GxMotion = nullptr;
	GxDrawHook = s.chained;
	s.chained = nullptr;
	DX8Wrapper::GxCaptureOn = s.savedCapture;
	DX8Wrapper::GxViewportCaptureOn = false;
	releaseRecords();
	s.wp = nullptr;
	// the dropped pass already stamped the pose histories with its own poses: its camera becomes the previous one too, so the
	// next pass pairs poses and camera from the same pass
	std::memcpy(s.prevView, s.view, 64);
	std::memcpy(s.prevProj, s.proj, 64);
}

void replay()
{
	IDirect3DSurface8 *rt = nullptr, *ds = nullptr;
	DX8Wrapper::Apply_Render_State_Changes();   // the game's pending state lands first, so it cannot undo ours
	DX8Wrapper::Get_Render_Target_Pair(rt, ds);   // both AddRef'd: released below
	const D3DVIEWPORT8 vpIn = DX8Wrapper::GxViewportSent;
	const bool vpInValid = DX8Wrapper::GxViewportSentValid;
	// what the replay changes, saved as the DEVICE holds it (the wrapper cache may say "unknown") and restored exactly afterwards,
	// so neither the device nor the wrapper's / ShaderClass's caches change
	static const D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE,
		D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SHADEMODE, D3DRS_COLORWRITEENABLE, D3DRS_CULLMODE,
		D3DRS_ZBIAS };
	DWORD saved[sizeof(kStates) / sizeof(kStates[0])];
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();   // drains the ring: the device's values are current
	for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i)
		if (FAILED(dev->GetRenderState(kStates[i], &saved[i])))
			saved[i] = DX8Wrapper::Get_DX8_Render_State(kStates[i]);
	// Shader handles: the DEVICE's binding is read here and restored through DX8Wrapper, exactly as the seam's composite does after End
	// (W3DWorldSeam.cpp saveState / restoreState; dlss-integrator ruling on). A raw device binder (water's
	// SetVertexShader(FVF), W3DWater.cpp:2031; W3DShaderManager.cpp, HeightMap.cpp) can leave the device and the wrapper's cache apart at
	// End; key off, the composite collapses them to the device's value, so restoring that value here makes key on end in the same state.
	// Dropping the VB binding below makes the next wrapper draw re-apply its own FVF. Arm: the HUD drawn
	// after End is 0 px between key on and key off (units-pics.sh on vs offt). Every programmable draw sets its own handles AND
	// constants right before it draws - trees W3DTreeBuffer.cpp:1787-1822, waves W3DWater.cpp:1948-1996 - and turns them off after,
	// :1853-1854 / :2030-2031; only HUD draws follow world End, so c0-c13 are not restored: no later draw reads them before
	// re-uploading.
	DWORD vsIn = 0, psIn = 0;
	dev->GetVertexShader(&vsIn);
	dev->GetPixelShader(&psIn);
	if (ensureProgramsAndTarget(ds) && DX8Wrapper::Set_Render_Target_Pair(s.o, ds)) {   // pair setter keeps the wrapper's books (#14)
		Renderer::Clear(true, false, Vector3(0.0f, 0.0f, 0.0f), 0.0f);   // A = 0: unwritten
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, D3DZB_TRUE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xF);
		DX8Wrapper::Set_Pixel_Shader(s.ps);
		const D3DVIEWPORT8 *vpLast = nullptr;
		for (const Rec &r : s.recs) {
			DX8Wrapper::Set_Vertex_Buffer(r.vb);
			DX8Wrapper::Set_Index_Buffer(r.ib, r.base);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, r.cull);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ZBIAS, r.zbias);
			if (!vpLast || std::memcmp(vpLast, &r.vp, sizeof(r.vp)) != 0) {
				setViewport(r.vp);
				vpLast = &r.vp;
			}
			DX8Wrapper::Apply_Render_State_Changes();   // binds the VB, which also sets its FVF as the vertex shader ...
			DX8Wrapper::Set_Vertex_Shader(r.skin ? s.vsSkin : s.vsRigid);   // ... so ours goes after (W3DTreeBuffer.cpp:1843-1850)
			DX8Wrapper::Gx_Forget_VS_Constants(0, GX_VEL_CONSTANTS);   // the cache may claim what the device no longer holds (#9)
			DX8Wrapper::Set_Vertex_Shader_Constant(0, r.c, GX_VEL_CONSTANTS);
			if (r.prim == D3DPT_TRIANGLESTRIP)
				DX8Wrapper::Draw_Strip((unsigned short)r.startIndex, (unsigned short)r.polyCount, (unsigned short)r.minVertex,
					(unsigned short)r.vertexCount);
			else
				DX8Wrapper::Draw_Triangles((unsigned short)r.startIndex, (unsigned short)r.polyCount, (unsigned short)r.minVertex,
					(unsigned short)r.vertexCount);
			++s.nReplayed;
		}
		DX8Wrapper::Gx_Forget_VS_Constants(0, GX_VEL_CONSTANTS);   // the device keeps ours: the next wrapper upload must not be skipped
		DX8Wrapper::Set_Vertex_Buffer(nullptr);   // drop the wrapper's engine refs: a skin buffer is locked again next pass
		DX8Wrapper::Set_Index_Buffer(nullptr, 0);
		DX8Wrapper::Apply_Render_State_Changes();
		for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); ++i)
			DX8Wrapper::Set_DX8_Render_State(kStates[i], saved[i]);
		DX8Wrapper::Set_Pixel_Shader(psIn);
		DX8Wrapper::Set_Vertex_Shader(vsIn);
		DX8Wrapper::Set_Render_Target_Pair(rt, ds);
		if (vpInValid)
			setViewport(vpIn);
		DX8Ring::Drain();
		s.wp->SetObjectVelocity(s.o);
	}
	if (rt)
		rt->Release();
	if (ds)
		ds->Release();
}

void stats()
{
	if (++s.nPasses % 600 != 0)
		return;
	std::fprintf(stderr, "[GXMOTION] units passes=%llu rigid=%llu skin=%llu skipped=%llu replayed=%llu meshes=%zu skins=%zu skinMB=%.1f "
		"buffers=%zu\n", (unsigned long long)s.nPasses, (unsigned long long)s.nRigid, (unsigned long long)s.nSkin,
		(unsigned long long)s.nSkipped, (unsigned long long)s.nReplayed, s.rigid.size(), s.skin.size(),
		(s.skin.bytes() + s.staging.capacity() * sizeof(float) + s.skinBuffers.size() * size_t(kSkinBufferVertices) * 24) / 1048576.0,
		s.skinBuffers.size());
	s.nRigid = s.nSkin = s.nSkipped = s.nReplayed = 0;
}

} // namespace

const int GxMotionUnits::Key = readKey();

void GxMotionUnits::Begin(IGxWorldPass *wp, const GxWorldPassBegin &b)
{
	if (!s.zEpsRead) {
		const char *z = std::getenv("GX_MOTION_ZEPS");
		s.zEps = z ? float(std::atof(z)) : 0.0f;
		s.zEpsRead = true;
	}
	if (s.open)
		dropPass();   // the previous pass never reached End: its records are stale
	if (Key == 0 || wp == nullptr || !(b.flags & GXWP_TEMPORAL))
		return;
	s.wp = wp;
	s.open = true;
	++s.pass;
	static bool said = false;   // the key-off evidence (A35 key OFF (c)): a key-off run never prints this; a key-on run does at once
	if (!said) {
		std::fprintf(stderr, "[GXMOTION] units on (GX_MOTION_UNITS=1, temporal world pass): the object-velocity bridge is active\n");
		said = true;
	}
	if (b.flags & GXWP_RESET_HISTORY)
		s.havePrevCam = false;   // no records this pass: camera-only vectors after a cut or a target change
	std::memcpy(s.view, &b.view, 64);
	std::memcpy(s.proj, &b.proj, 64);
	if (s.havePrevCam)
		mul(s.prevView, s.prevProj, s.prevViewProj);
	s.jitter[0] = s.jitter[1] = 0.0f;
	wp->GetJitter(s.jitter);
	if ((s.pass & 255) == 0) {
		s.rigid.sweep(s.pass);
		s.skin.sweep(s.pass);
	}
	s.recs.clear();
	s.recs.reserve(1024);
	s.skinBuffersUsed = 0;
	s.skinBuffer = -1;
	s.batchVertices = 0;
	s.pending = false;
	// the sent projection and viewport are captured while the bridge is open (dx8wrapper.h GxProjSent / GxViewportSent)
	s.savedCapture = DX8Wrapper::GxCaptureOn;
	if (!s.savedCapture)
		DX8Wrapper::Gx_Capture_Forget();   // nothing else is reading them: no stale value from an earlier pass survives
	DX8Wrapper::GxCaptureOn = true;
	DX8Wrapper::GxViewportSentValid = false;
	DX8Wrapper::GxViewportCaptureOn = true;
	s.hooks.rigidBegin = &rigidBegin;
	s.hooks.rigidEnd = &rigidEnd;
	s.hooks.skinMesh = &skinMesh;
	s.hooks.skinBatch = &skinBatch;
	GxMotion = &s.hooks;
	s.chained = GxDrawHook;
	GxDrawHook = &onDraw;
}

void GxMotionUnits::End()
{
	if (!s.open)
		return;
	s.open = false;
	GxMotion = nullptr;
	GxDrawHook = s.chained;   // before the replay: its own draws must not record themselves
	s.chained = nullptr;
	DX8Wrapper::GxViewportCaptureOn = false;   // the replay's own viewports are not captured
	if (!s.recs.empty())
		replay();
	DX8Wrapper::GxCaptureOn = s.savedCapture;
	releaseRecords();
	std::memcpy(s.prevView, s.view, 64);
	std::memcpy(s.prevProj, s.proj, 64);
	s.havePrevCam = true;
	stats();
	s.wp = nullptr;
}

void GxMotionUnits::Release()
{
	if (!s.open && !s.programs && s.skinBuffers.empty() && !s.oTex && s.rigid.size() == 0 && s.skin.size() == 0)
		return;   // never active (e.g. GX_MOTION_UNITS=1 on the classic renderer): nothing to release, no device call
	if (s.open)
		dropPass();   // a reset inside a world pass: never replay into a device about to reset
	for (VertexBufferClass *vb : s.skinBuffers)
		vb->Release_Ref();
	s.skinBuffers.clear();
	s.skinBuffersUsed = 0;
	if (s.o)
		s.o->Release();
	if (s.oTex)
		s.oTex->Release();
	s.o = nullptr;
	s.oTex = nullptr;
	s.oW = s.oH = 0;
	// The seam calls this before a Reset AND from Release_Device / failOff (W3DWorldSeam.cpp): delete the shader handles while the
	// device still exists, so a new device never sees stale ones (they are re-created on the next replay). Safe with nothing made.
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev && s.programs) {
		dev->DeleteVertexShader(s.vsRigid);
		dev->DeleteVertexShader(s.vsSkin);
		dev->DeletePixelShader(s.ps);
	}
	s.programs = s.programsFailed = false;
	s.vsRigid = s.vsSkin = s.ps = 0;
	s.rigid = GxRigidHistory();
	s.skin = GxSkinHistory();
	s.havePrevCam = false;   // after dropPass: a reset forgets the camera as well
}
