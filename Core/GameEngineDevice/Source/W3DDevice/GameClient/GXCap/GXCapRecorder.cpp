// GeneralsX: GXCAP recorder core (WS-I).
// A recording IDirect3DDevice8 proxy installed on DX8Wrapper::D3DDevice right after CreateDevice (DX8Wrapper::
// Device_Wrap_Hook, set by GXCap.cpp), plus wrappers for the textures, surfaces and buffers it hands out. Everything that
// reaches DXVK goes through that one pointer - the raw _Get_D3D_Device8() holders, DX8CALL users, the DX8Wrapper statics,
// the lock classes, the d3d8 ring's render thread, the D3DX compat code - so the proxy sees every call. Outside a window it
// forwards and keeps the object maps, the setter shadows (render/stage states, strides, light slots) and dirty rects; a
// window starts with a snapshot (every live or still-bound object, its content read back, the device state), then records
// every call until it closes. Format: GXCapFormat.h. Game-independent (gxcap-selftest links it). Not built on Windows yet.
#include "GXCapRecorder.h"

#ifndef _WIN32

#include "GXCapFormat.h"

#include <algorithm>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

extern char **environ;

namespace
{
using namespace gxcap;
typedef std::lock_guard<std::recursive_mutex> Guard;

struct CapObj;
struct CapDevice;

struct ShaderBytes { std::vector<uint8_t> decl, func; uint32_t usage = 0; };

struct LockInfo
{
	uint32_t sub, face, level, x0, y0, x1, y1, z0, z1, flags;
	const uint8_t *ptr;
	uint32_t pitch, slicePitch;
};

const uint32_t NO_LIGHT = 0xFFFFFFFFu;

struct State
{
	std::recursive_mutex mx;
	std::string dir, sha;
	CapDevice *dev = nullptr;
	uint32_t nextId = 1;
	std::unordered_map<void *, CapObj *> byReal, byWrap;
	std::set<std::pair<uint64_t, uint64_t>> blobs;
	std::map<uint32_t, ShaderBytes> vs, ps;
	std::set<uint32_t> lights;
	std::set<std::string> warned;
	bool unsupportedEver = false;
	std::string d3d8Lib;
	// GXCAP_TEST_* knobs (gxcap-selftest's negative arms only): each disables one mechanism the replay depends on
	bool testNoRefresh = false, testNoZombies = false, testGetterState = false, testNoGpuMark = false, testAllowNested = false;
	// the application's bound shaders, and whether it deleted them while bound: DXVK's d3d8 then clears only its handle and the
	// d3d9 binding stays, which a snapshot (handle 0) cannot restore - a draw before the next set is UNSUPPORTED (branch-r6 #2)
	uint32_t setVS = 0, setPS = 0;
	bool delVS = false, delPS = false, staleVS = false, stalePS = false;
	// setter shadows: values the application SET (the getters are lossy, e.g. d3d8 ZBIAS)
	uint32_t rs[256] = {};
	uint8_t rsSet[256] = {};
	uint32_t tss[8][32] = {};
	uint8_t tssSet[8][32] = {};
	uint32_t stride[16] = {};
	uint32_t lightSlots[8];		// DXVK's enabledLightIndices, emulated (d3d9_device.cpp LightEnable): the packing order
	// the open window
	bool open = false, inShot = false, shotRequested = false, shotSeen = false;
	std::thread::id shotThread;	// the thread that opened the shot bracket: only its calls are the screenshot's (r11 #1)
	uint32_t winFrame = 0, drawFrame = 0, span = 1, flags = 0;
	Buf rec;
	uint64_t records = 0, newBlobBytes = 0, newBlobs = 0;
	std::chrono::steady_clock::time_point t0;
};
State g;

void Log(const char *fmt, const std::string &a = std::string())
{
	fprintf(stderr, "[GXCAP] ");
	fprintf(stderr, fmt, a.c_str());
	fprintf(stderr, "\n");
	fflush(stderr);
}

void Warn(const std::string &what)
{
	if (g.warned.insert(what).second)
		Log("WARNING %s", what);
}

BlobRef Write_Blob(const void *d, size_t n)
{
	BlobRef r = { Hash64(d, n), uint64_t(n) };
	if (g.blobs.insert(std::make_pair(r.hash, r.len)).second) {
		const std::string path = Blob_Path(g.dir, r.hash, r.len);
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			const std::string tmp = path + ".tmp";
			FILE *f = fopen(tmp.c_str(), "wb");
			bool ok = f != nullptr && (n == 0 || fwrite(d, 1, n, f) == n);
			if (f != nullptr && fclose(f) != 0)
				ok = false;
			if (ok)
				ok = rename(tmp.c_str(), path.c_str()) == 0;
			if (!ok) {
				Warn("blob write failed: " + path);
				g.flags |= FH_TRUNCATED;
			}
			g.newBlobBytes += n;
			++g.newBlobs;
		}
	}
	return r;
}

void Put_Bytes(Buf &p, const void *d, size_t n)
{
	if (n <= INLINE_MAX) {
		p.v<uint8_t>(0);
		p.u32(uint32_t(n));
		p.put(d, n);
	} else {
		p.v<uint8_t>(1);
		p.v(Write_Blob(d, n));
	}
}

// One record of the open window: build the payload in p, then emit with the call's result.
struct Rec
{
	Buf p;
	uint16_t op, fl;
	explicit Rec(uint16_t o, uint16_t f = 0) : op(o), fl(f) {}
	void emit(int32_t hr = 0)
	{
		RecHeader h = { op, fl, uint32_t(p.b.size()), hr };
		g.rec.v(h);
		g.rec.put(p.b.data(), p.b.size());
		++g.records;
	}
};

// A call belongs to the open screenshot only when it comes from the thread that opened the bracket: the mutex is released
// between calls, so another thread's copy can land inside the bracket
bool In_Shot()
{
	return g.inShot && std::this_thread::get_id() == g.shotThread;
}

void Check_Stale_Shader()
{
	if (g.open && (g.staleVS || g.stalePS) && (g.flags & FH_UNSUPPORTED) == 0) {
		Warn("a draw with a shader deleted before the window still bound (the snapshot cannot restore it): window UNSUPPORTED");
		g.flags |= FH_UNSUPPORTED;
	}
}

void Unsupported(const char *name)
{
	g.unsupportedEver = true;
	Warn(std::string("unsupported call: ") + name + " (every window of this run is flagged UNSUPPORTED)");
	if (g.open) {
		g.flags |= FH_UNSUPPORTED;
		Rec r(OP_UNSUPPORTED);
		r.p.put(name, strlen(name));
		r.emit();
	}
}

// ---- objects ----

struct CapObj
{
	uint32_t id = 0;
	Kind kind = K_NONE;
	Origin origin = O_CREATE;
	uint16_t createOp = 0;	// surfaces made by the device: OP_CREATE_RT / _DS / _IMAGE
	CapObj *parent = nullptr;
	uint32_t level = 0, face = 0;
	uint32_t width = 0, height = 0, levels = 0, usage = 0, format = 0, pool = 0, msaa = 0, length = 0, fvf = 0, lockable = 0;
	// Public refcount reached 0 = !alive. The object can still live privately (bound to the device) and come back through
	// a getter, so the wrapper and its id stay until a new object reuses the address.
	bool alive = true, zombie = false;
	bool dirtyAny = true;	// SYSTEMMEM textures: DXVK starts every texture fully dirty (d3d9_common_texture.cpp:30)
	// (root) a CPU-pool object the GPU wrote inside the open window (CopyRects from a DEFAULT surface, or from a CPU-pool
	// object already marked - any number of CPU hops): its content is the replay's own to reproduce, so it is never
	// refreshed - a refresh would paste the game's pixels over it and hide a rendering difference upstream.
	// Cleared when a window opens.
	bool gpuWritten = false;
	RECT dirty = {};
	std::vector<CapObj *> children;
	std::vector<LockInfo> locks;
	virtual ~CapObj() {}
	virtual void *Real() = 0;
	virtual void *Wrap() = 0;
};

bool Is_Subresource(const CapObj *o)
{
	return o->parent != nullptr && (o->origin == O_TEX_LEVEL || o->origin == O_CUBE_FACE || o->origin == O_VOL_LEVEL);
}

CapObj *Root(CapObj *o)
{
	while (Is_Subresource(o))
		o = o->parent;
	return o;
}

void Set_Alive(CapObj *o, bool a)
{
	o->alive = a;
	for (CapObj *c : o->children)
		Set_Alive(c, a);
}

void Register(CapObj *o)
{
	o->id = g.nextId++;
	g.byReal[o->Real()] = o;
	g.byWrap[o->Wrap()] = o;
}

void Unregister_Tree(CapObj *o)
{
	for (CapObj *c : o->children)
		Unregister_Tree(c);
	g.byReal.erase(o->Real());
	g.byWrap.erase(o->Wrap());
	delete o;
}

CapObj *Find_Wrap(const void *w)
{
	if (w == nullptr)
		return nullptr;
	auto it = g.byWrap.find(const_cast<void *>(w));
	return it == g.byWrap.end() ? nullptr : it->second;
}

CapObj *Find_Real(const void *r)
{
	if (r == nullptr)
		return nullptr;
	auto it = g.byReal.find(const_cast<void *>(r));
	return it == g.byReal.end() ? nullptr : it->second;
}

// A new object at an address the maps still hold: the old one is gone (only creation reuses an address).
void Evict(void *real)
{
	CapObj *o = Find_Real(real);
	if (o == nullptr)
		return;
	o = Root(o);
	if (o->alive)
		Warn("a new object reused the address of one still counted alive (wrapper dropped)");
	if (o->parent != nullptr) {
		std::vector<CapObj *> &sib = o->parent->children;
		sib.erase(std::remove(sib.begin(), sib.end(), o), sib.end());
	}
	Unregister_Tree(o);
}

// The public refcount reached 0. A subresource shares its container's count (DXVK D3D8Subresource::Release), so the whole
// container's tree goes publicly dead together. Nothing is deleted: a bound object can come back through a getter.
void Died(CapObj *o)
{
	CapObj *root = Root(o);
	if (!root->alive)
		return;
	Set_Alive(root, false);
	if (g.open) {
		Rec r(OP_DESTROY);
		r.p.u32(root->id);
		r.emit();
	}
}

uint32_t Id(const void *w)
{
	if (w == nullptr)
		return 0;
	CapObj *o = Find_Wrap(w);
	if (o == nullptr) {
		Warn("an object the proxy did not create reached the device (capture incomplete)");
		if (g.open)
			g.flags |= FH_UNSUPPORTED;
		return 0xFFFFFFFFu;
	}
	return o->id;
}

// The real object behind a wrapper. Every d3d8 interface inherits singly from the one below it, so the requested base
// interface sits at the same address as the concrete interface the wrapper stores. A foreign pointer passes through.
template <class I> I *Real_Of(I *w)
{
	if (w == nullptr)
		return nullptr;
	CapObj *o = Find_Wrap(w);
	return o != nullptr ? static_cast<I *>(o->Real()) : w;
}

HRESULT Get_Device(IDirect3DDevice8 **pp);

template <class W, class I> HRESULT Query_Obj(W *w, I *real, REFIID riid, void **pp)
{
	if (pp == nullptr)
		return E_POINTER;
	void *q = nullptr;
	HRESULT hr = real->QueryInterface(riid, &q);
	if (FAILED(hr))
		return hr;
	if (q == static_cast<void *>(real)) {
		*pp = w->Wrap();	// the real AddRef from QueryInterface is the reference the caller now holds
		return hr;
	}
	static_cast<IUnknown *>(q)->Release();
	*pp = nullptr;
	Warn("QueryInterface for a non-d3d8 interface refused under GXCAP");
	return E_NOINTERFACE;
}

#define CAP_IUNKNOWN(R) \
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **pp) override { Guard k(g.mx); return Query_Obj(this, R, riid, pp); } \
	ULONG STDMETHODCALLTYPE AddRef() override { Guard k(g.mx); return R->AddRef(); } \
	ULONG STDMETHODCALLTYPE Release() override \
	{ \
		Guard k(g.mx); \
		const ULONG n = R->Release(); \
		if (n == 0) \
			Died(this); \
		return n; \
	}

#define CAP_PRIVATE_DATA(R) \
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8 **pp) override { return Get_Device(pp); } \
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID gd, const void *d, DWORD n, DWORD f) override { return R->SetPrivateData(gd, d, n, f); } \
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID gd, void *d, DWORD *n) override { return R->GetPrivateData(gd, d, n); } \
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID gd) override { return R->FreePrivateData(gd); }

#define CAP_RESOURCE(R) \
	CAP_PRIVATE_DATA(R) \
	DWORD STDMETHODCALLTYPE SetPriority(DWORD v) override \
	{ \
		Guard k(g.mx); \
		const DWORD o = R->SetPriority(v); \
		if (g.open) { Rec x(OP_SET_PRIORITY); x.p.u32(id); x.p.u32(v); x.emit(int32_t(o)); } \
		return o; \
	} \
	DWORD STDMETHODCALLTYPE GetPriority() override { return R->GetPriority(); } \
	void STDMETHODCALLTYPE PreLoad() override \
	{ \
		Guard k(g.mx); \
		R->PreLoad(); \
		if (g.open) { Rec x(OP_PRELOAD); x.p.u32(id); x.emit(); } \
	} \
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return R->GetType(); }

// A lock is noted when it succeeds and written out when it is released: the bytes at Unlock are the bytes the GPU gets.
// Locks are noted outside windows too, so a lock opened before a window (the texture loader thread fills mapped memory
// between a foreground Lock and Unlock, textureloader.cpp) is skipped by the snapshot and recorded at its Unlock.
void Note_Lock(CapObj *o, uint32_t sub, uint32_t face, uint32_t level, uint32_t w, uint32_t h, const RECT *rc,
	uint32_t flags, const void *ptr, uint32_t pitch)
{
	LockInfo li = { sub, face, level, 0, 0, w, h, 0, 1, flags, static_cast<const uint8_t *>(ptr), pitch, 0 };
	if (rc != nullptr) {
		li.x0 = uint32_t(rc->left); li.y0 = uint32_t(rc->top); li.x1 = uint32_t(rc->right); li.y1 = uint32_t(rc->bottom);
	}
	for (const LockInfo &other : o->locks)
		if (other.sub == sub)
			Warn("nested lock on one subresource (only the newest lock's bytes are recorded)");
	o->locks.push_back(li);
}

void Flush_Lock(CapObj *o, uint32_t sub, bool buffer)
{
	for (size_t i = o->locks.size(); i-- > 0;) {
		if (o->locks[i].sub != sub)
			continue;
		const LockInfo li = o->locks[i];
		o->locks.erase(o->locks.begin() + long(i));
		if (!g.open)
			return;
		const bool readOnly = (li.flags & D3DLOCK_READONLY) != 0;
		Region rg = { o->id, li.sub, li.face, li.level, li.x0, li.y0, li.x1, li.y1, li.z0, li.z1, li.flags, 0, 0, 0 };
		std::vector<uint8_t> bytes;
		if (buffer) {
			rg.rowBytes = li.x1 - li.x0;
			rg.rows = 1;
			rg.slices = 1;
			if (!readOnly)
				bytes.assign(li.ptr, li.ptr + rg.rowBytes);
		} else {
			const Layout L = Format_Layout(o->format, li.x1 - li.x0, li.y1 - li.y0);
			if (!L.ok) {
				Warn("lock of an unknown format: its bytes are not recorded (window UNSUPPORTED)");
				g.flags |= FH_UNSUPPORTED;
			} else {
				rg.rowBytes = L.rowBytes;
				rg.rows = L.rows;
				rg.slices = 1;
				if (!readOnly || In_Shot()) {
					bytes.resize(size_t(rg.rowBytes) * rg.rows);
					for (uint32_t y = 0; y < rg.rows; ++y)
						memcpy(&bytes[size_t(y) * rg.rowBytes], li.ptr + size_t(y) * li.pitch, rg.rowBytes);
				}
			}
		}
		// Inside a screenshot readback the bytes are the reference picture: kept even for a read-only lock.
		Rec r(readOnly ? OP_LOCK_READ : OP_LOCK_WRITE, In_Shot() ? F_SHOT : 0);
		r.p.v(rg);
		if (!readOnly || In_Shot())
			Put_Bytes(r.p, bytes.data(), bytes.size());
		r.emit();
		return;
	}
}

void Dirty_Union(CapObj *t, const RECT *rc, uint32_t level)
{
	RECT r = { 0, 0, LONG(t->width), LONG(t->height) };
	if (rc != nullptr) {
		r.left = rc->left << level; r.top = rc->top << level; r.right = rc->right << level; r.bottom = rc->bottom << level;
	}
	if (!t->dirtyAny) {
		t->dirty = r;
		t->dirtyAny = true;
		return;
	}
	t->dirty.left = std::min(t->dirty.left, r.left); t->dirty.top = std::min(t->dirty.top, r.top);
	t->dirty.right = std::max(t->dirty.right, r.right); t->dirty.bottom = std::max(t->dirty.bottom, r.bottom);
}

struct CapSurface;
CapSurface *Wrap_Surface(IDirect3DSurface8 *r, Origin origin, CapObj *parent, uint32_t level, uint32_t face, bool *isNew);

struct CapSurface final : IDirect3DSurface8, CapObj
{
	IDirect3DSurface8 *r;
	explicit CapSurface(IDirect3DSurface8 *real) : r(real) { kind = K_SURFACE; }
	void *Real() override { return r; }
	void *Wrap() override { return static_cast<IDirect3DSurface8 *>(this); }
	CAP_IUNKNOWN(r)
	CAP_PRIVATE_DATA(r)
	HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **pp) override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *d) override { return r->GetDesc(d); }
	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->LockRect(lr, rc, f);
		if (SUCCEEDED(hr) && lr != nullptr) {
			Note_Lock(this, 0, 0, 0, width, height, rc, f, lr->pBits, uint32_t(lr->Pitch));
			if (origin == O_TEX_LEVEL && parent != nullptr && (f & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) == 0)
				Dirty_Union(parent, rc, level);	// a level's surface dirties its texture, as a texture lock does
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect() override
	{
		Guard k(g.mx);
		Flush_Lock(this, 0, false);
		return r->UnlockRect();
	}
};

struct CapTexture final : IDirect3DTexture8, CapObj
{
	IDirect3DTexture8 *r;
	explicit CapTexture(IDirect3DTexture8 *real) : r(real) { kind = K_TEXTURE; }
	void *Real() override { return r; }
	void *Wrap() override { return static_cast<IDirect3DTexture8 *>(this); }
	CAP_IUNKNOWN(r)
	CAP_RESOURCE(r)
	DWORD STDMETHODCALLTYPE SetLOD(DWORD v) override
	{
		Guard k(g.mx);
		const DWORD o = r->SetLOD(v);
		if (g.open) { Rec x(OP_SET_LOD); x.p.u32(id); x.p.u32(v); x.emit(int32_t(o)); }
		return o;
	}
	DWORD STDMETHODCALLTYPE GetLOD() override { return r->GetLOD(); }
	DWORD STDMETHODCALLTYPE GetLevelCount() override { return r->GetLevelCount(); }
	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l, D3DSURFACE_DESC *d) override { return r->GetLevelDesc(l, d); }
	HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT l, IDirect3DSurface8 **pp) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->GetSurfaceLevel(l, pp != nullptr ? &s : nullptr);
		if (FAILED(hr) || pp == nullptr)
			return hr;
		bool isNew = false;
		CapSurface *w = Wrap_Surface(s, O_TEX_LEVEL, this, l, 0, &isNew);
		*pp = w;
		if (g.open) { Rec x(OP_GET_SURFACE_LEVEL, isNew ? F_NEW : 0); x.p.u32(w->id); x.p.u32(id); x.p.u32(l); x.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockRect(UINT l, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->LockRect(l, lr, rc, f);
		if (SUCCEEDED(hr) && lr != nullptr) {
			Note_Lock(this, l, 0, l, std::max(1u, width >> l), std::max(1u, height >> l), rc, f, lr->pBits, uint32_t(lr->Pitch));
			if ((f & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) == 0)
				Dirty_Union(this, rc, l);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect(UINT l) override
	{
		Guard k(g.mx);
		Flush_Lock(this, l, false);
		return r->UnlockRect(l);
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *rc) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->AddDirtyRect(rc);
		if (SUCCEEDED(hr))
			Dirty_Union(this, rc, 0);
		if (g.open) {
			Rec x(OP_ADD_DIRTY_RECT);
			x.p.u32(id); x.p.u32(0); x.p.v<uint8_t>(rc != nullptr);
			RECT z = {}; x.p.v(rc != nullptr ? *rc : z);
			x.emit(hr);
		}
		return hr;
	}
};

template <class B> struct CapBuffer final : B, CapObj
{
	B *r;
	CapBuffer(B *real, Kind k) : r(real) { kind = k; }
	void *Real() override { return r; }
	void *Wrap() override { return static_cast<B *>(this); }
	CAP_IUNKNOWN(r)
	CAP_RESOURCE(r)
	HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, BYTE **pp, DWORD f) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->Lock(off, size, pp, f);
		if (SUCCEEDED(hr) && pp != nullptr && *pp != nullptr) {
			const uint32_t end = size == 0 ? length : std::min(length, off + size);
			LockInfo li = { 0, 0, 0, off, 0, end, 1, 0, 1, f, *pp, 0, 0 };
			if (!locks.empty())
				Warn("nested buffer lock (only the newest lock's bytes are recorded)");
			locks.push_back(li);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE Unlock() override
	{
		Guard k(g.mx);
		Flush_Lock(this, 0, true);
		return r->Unlock();
	}
	HRESULT STDMETHODCALLTYPE GetDesc(typename std::conditional<std::is_same<B, IDirect3DVertexBuffer8>::value, D3DVERTEXBUFFER_DESC,
		D3DINDEXBUFFER_DESC>::type *d) override
	{
		return r->GetDesc(d);
	}
};
typedef CapBuffer<IDirect3DVertexBuffer8> CapVB;
typedef CapBuffer<IDirect3DIndexBuffer8> CapIB;

// A surface a getter handed out. Known and alive: the same wrapper. Known but publicly dead: the object came back (it was
// still bound), so its tree is alive again and the getter's record carries F_NEW for the replayer to re-bind the id.
CapSurface *Wrap_Surface(IDirect3DSurface8 *s, Origin origin, CapObj *parent, uint32_t level, uint32_t face, bool *isNew)
{
	if (CapObj *o = Find_Real(s)) {
		const bool sameIdentity = origin != O_TEX_LEVEL || (o->parent == parent && o->level == level);
		if (sameIdentity) {
			CapObj *root = Root(o);
			*isNew = !root->alive;
			if (!root->alive)
				Set_Alive(root, true);
			return static_cast<CapSurface *>(o);
		}
		Evict(s);
	}
	*isNew = true;
	CapSurface *w = new CapSurface(s);
	D3DSURFACE_DESC d = {};
	s->GetDesc(&d);
	w->origin = origin;
	w->parent = parent;
	w->level = level;
	w->face = face;
	w->width = d.Width;
	w->height = d.Height;
	w->format = uint32_t(d.Format);
	w->usage = d.Usage;
	w->pool = uint32_t(d.Pool);
	w->msaa = uint32_t(d.MultiSampleType);
	Register(w);
	if (parent != nullptr)
		parent->children.push_back(w);
	return w;
}

HRESULT CapSurface::GetContainer(REFIID riid, void **pp)
{
	Guard k(g.mx);
	void *q = nullptr;
	const HRESULT hr = r->GetContainer(riid, &q);
	if (FAILED(hr) || pp == nullptr) {
		if (pp != nullptr)
			*pp = q;
		return hr;
	}
	if (CapObj *o = Find_Real(q)) {
		const bool back = !Root(o)->alive;
		Set_Alive(Root(o), true);
		*pp = o->Wrap();
		if (g.open) { Rec x(OP_GET_CONTAINER, back ? F_NEW : 0); x.p.u32(o->id); x.p.u32(id); x.emit(hr); }
		return hr;
	}
	static_cast<IUnknown *>(q)->Release();
	*pp = nullptr;
	if (parent == nullptr)	// the device: hand back the proxy
		return Get_Device(reinterpret_cast<IDirect3DDevice8 **>(pp));
	Warn("GetContainer returned an object the proxy does not know");
	return E_NOINTERFACE;
}

// A CPU-pool copy source is re-read right before the copy consumes it: the shroud writes its SYSTEMMEM surface through a
// pointer kept after UnlockRect (W3DShroud.cpp:174-183, 356) and uploads it with CopyRects.
void Refresh_Source(CapObj *o);

// ---- the device ----

uint32_t Prim_Verts(D3DPRIMITIVETYPE t, UINT n)
{
	switch (t) {
	case D3DPT_POINTLIST: return n;
	case D3DPT_LINELIST: return 2 * n;
	case D3DPT_LINESTRIP: return n + 1;
	case D3DPT_TRIANGLELIST: return 3 * n;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_TRIANGLEFAN: return n + 2;
	default: return 0;
	}
}

// Length in DWORDs of a d3d8 vertex declaration (through D3DVSD_END, skipping D3DVSD_CONST data).
size_t Decl_Len(const DWORD *d)
{
	size_t i = 0;
	for (;;) {
		const DWORD t = d[i++];
		if (t == 0xFFFFFFFFu)
			return i;
		if (((t >> 29) & 7) == 4)	// D3DVSD_TOKEN_CONSTMEM
			i += ((t >> 25) & 0xF) * 4;
	}
}

// Length in DWORDs of a shader function (through the 0x0000FFFF end token, skipping comments and def constants).
// Shader model 1.x: an instruction token has bit 31 clear and is followed by its parameter tokens (bit 31 set); only
// instruction tokens are interpreted (an operand like c81 = 0xA0E40051 must not read as def).
size_t Func_Len(const DWORD *f)
{
	size_t i = 1;	// version token
	for (;;) {
		const DWORD t = f[i++];
		if (t & 0x80000000u)
			continue;	// a parameter token
		if (t == 0x0000FFFFu)
			return i;
		if ((t & 0xFFFF) == 0xFFFE)	// comment: its length in DWORDs follows in bits 16-30
			i += (t >> 16) & 0x7FFF;
		else if ((t & 0xFFFF) == 81)	// def: destination + 4 raw values (any bit pattern)
			i += 5;
	}
}

void Close_Window(bool truncated);

struct CapDevice final : IDirect3DDevice8
{
	IDirect3DDevice8 *r;
	void *implicitBB = nullptr, *implicitDS = nullptr;	// real pointers, no reference held
	UINT adapter;
	D3DDEVTYPE type;
	DWORD behavior;
	D3DPRESENT_PARAMETERS pp;

	CapDevice(IDirect3DDevice8 *real, UINT a, D3DDEVTYPE t, DWORD b, const D3DPRESENT_PARAMETERS *p)
		: r(real), adapter(a), type(t), behavior(b), pp(*p)
	{
		Note_Implicit();
	}

	void Note_Implicit()
	{
		IDirect3DSurface8 *s = nullptr;
		implicitBB = implicitDS = nullptr;
		if (SUCCEEDED(r->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &s)) && s != nullptr) { implicitBB = s; s->Release(); }
		s = nullptr;
		if (SUCCEEDED(r->GetDepthStencilSurface(&s)) && s != nullptr) { implicitDS = s; s->Release(); }
	}

	CapSurface *Classify(IDirect3DSurface8 *s, bool *isNew)
	{
		// A reset-created implicit surface can reuse the address of a released custom surface (a pillarbox texture level
		// freed at Reset): that dead wrapper is not this surface - resurrecting it would mark its freed parent alive for the
		// snapshot to read. Evict it, then classify as implicit.
		if (CapObj *known = Find_Real(s))
			if ((s == implicitBB && known->origin != O_BACKBUFFER) || (s == implicitDS && known->origin != O_AUTODEPTH))
				if (!Root(known)->alive)
					Evict(s);
		if (Find_Real(s) != nullptr)
			return Wrap_Surface(s, O_UNKNOWN, nullptr, 0, 0, isNew);	// known: same wrapper (resurrected if it was dead)
		if (s == implicitBB)
			return Wrap_Surface(s, O_BACKBUFFER, nullptr, 0, 0, isNew);
		if (s == implicitDS)
			return Wrap_Surface(s, O_AUTODEPTH, nullptr, 0, 0, isNew);
		Warn("a surface of unknown origin was handed out (a window that uses it is refused by the replayer)");
		return Wrap_Surface(s, O_UNKNOWN, nullptr, 0, 0, isNew);
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		void *q = nullptr;
		const HRESULT hr = r->QueryInterface(riid, &q);
		if (FAILED(hr))
			return hr;
		if (q == static_cast<void *>(r)) {
			*ppv = static_cast<IDirect3DDevice8 *>(this);
			return hr;
		}
		static_cast<IUnknown *>(q)->Release();
		*ppv = nullptr;
		Warn("QueryInterface for a non-d3d8 device interface refused under GXCAP (CLASSIC-only capture)");
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return r->AddRef(); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		Guard k(g.mx);
		const ULONG n = r->Release();
		if (n == 0) {
			if (g.open)
				Close_Window(true);
			g.dev = nullptr;
			Log("device released: recorder off%s", "");
			delete this;
		}
		return n;
	}

	HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return r->TestCooperativeLevel(); }
	UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return r->GetAvailableTextureMem(); }
	HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD b) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->ResourceManagerDiscardBytes(b);
		if (g.open) { Rec x(OP_RESOURCE_DISCARD); x.p.u32(b); x.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8 **p) override { return r->GetDirect3D(p); }
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8 *c) override { return r->GetDeviceCaps(c); }
	HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *m) override { return r->GetDisplayMode(m); }
	HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p) override { return r->GetCreationParameters(p); }
	HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface8 *s) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetCursorProperties(x, y, Real_Of(s));
		if (g.open) { Rec q(OP_SET_CURSOR_PROPS); q.p.u32(x); q.p.u32(y); q.p.u32(Id(s)); q.emit(hr); }
		return hr;
	}
	void STDMETHODCALLTYPE SetCursorPosition(UINT x, UINT y, DWORD f) override
	{
		Guard k(g.mx);
		r->SetCursorPosition(x, y, f);
		if (g.open) { Rec q(OP_SET_CURSOR_POS); q.p.u32(x); q.p.u32(y); q.p.u32(f); q.emit(); }
	}
	WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL b) override
	{
		Guard k(g.mx);
		const WINBOOL o = r->ShowCursor(b);
		if (g.open) { Rec q(OP_SHOW_CURSOR); q.p.u32(uint32_t(b)); q.emit(int32_t(o)); }
		return o;
	}
	HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *p, IDirect3DSwapChain8 **sc) override
	{
		Guard k(g.mx);
		Unsupported("CreateAdditionalSwapChain");
		return r->CreateAdditionalSwapChain(p, sc);
	}
	HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *p) override
	{
		Guard k(g.mx);
		if (g.open) {
			Rec q(OP_RESET);
			if (p != nullptr)
				q.p.v(*p);
			q.emit();
			Close_Window(true);
		}
		const HRESULT hr = r->Reset(p);
		if (SUCCEEDED(hr)) {
			if (p != nullptr)
				pp = *p;
			// DXVK Reset: every state back to its default, lights gone, implicit back buffers/depth recreated
			memset(g.rsSet, 0, sizeof(g.rsSet));
			memset(g.tssSet, 0, sizeof(g.tssSet));
			memset(g.stride, 0, sizeof(g.stride));
			for (uint32_t &s : g.lightSlots)
				s = NO_LIGHT;
			g.lights.clear();
			std::vector<void *> stale;
			for (auto &kv : g.byReal)
				if (kv.second->origin == O_BACKBUFFER || kv.second->origin == O_AUTODEPTH)
					stale.push_back(kv.first);
			for (void *r0 : stale)
				Evict(r0);
		}
		Note_Implicit();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE Present(const RECT *src, const RECT *dst, HWND hwnd, const RGNDATA *dirty) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->Present(src, dst, hwnd, dirty);
		if (g.open) {
			Rec q(OP_PRESENT);
			RECT z = {};
			q.p.v<uint8_t>(src != nullptr); q.p.v(src != nullptr ? *src : z);
			q.p.v<uint8_t>(dst != nullptr); q.p.v(dst != nullptr ? *dst : z);
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->GetBackBuffer(i, t, ps != nullptr ? &s : nullptr);
		if (FAILED(hr) || ps == nullptr)
			return hr;
		bool isNew = false;
		CapSurface *w = (i == 0 && s == implicitBB) ? Classify(s, &isNew) : Wrap_Surface(s, O_BACKBUFFER, nullptr, i, 0, &isNew);
		if (isNew)
			w->level = i;
		*ps = w;
		if (g.open) { Rec q(OP_GET_BACKBUFFER, isNew ? F_NEW : 0); q.p.u32(w->id); q.p.u32(i); q.p.u32(uint32_t(t)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *s) override { return r->GetRasterStatus(s); }
	void STDMETHODCALLTYPE SetGammaRamp(DWORD f, const D3DGAMMARAMP *ramp) override
	{
		Guard k(g.mx);
		r->SetGammaRamp(f, ramp);
		if (g.open && ramp != nullptr) { Rec q(OP_SET_GAMMA_RAMP); q.p.u32(f); q.p.v(*ramp); q.emit(); }
	}
	void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP *ramp) override { r->GetGammaRamp(ramp); }

	HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
		IDirect3DTexture8 **pt) override
	{
		Guard k(g.mx);
		IDirect3DTexture8 *t = nullptr;
		const HRESULT hr = r->CreateTexture(w, h, levels, usage, fmt, pool, pt != nullptr ? &t : nullptr);
		if (FAILED(hr) || pt == nullptr)
			return hr;
		Evict(t);
		CapTexture *c = new CapTexture(t);
		c->width = w; c->height = h; c->levels = t->GetLevelCount(); c->usage = usage; c->format = uint32_t(fmt); c->pool = uint32_t(pool);
		c->dirty = { 0, 0, LONG(w), LONG(h) };	// fully dirty at creation, as DXVK starts it
		Register(c);
		*pt = c;
		if (g.open) {
			Rec q(OP_CREATE_TEXTURE);
			q.p.u32(c->id); q.p.u32(w); q.p.u32(h); q.p.u32(levels); q.p.u32(usage); q.p.u32(uint32_t(fmt)); q.p.u32(uint32_t(pool));
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
		IDirect3DVolumeTexture8 **pt) override
	{
		Guard k(g.mx);
		Unsupported("CreateVolumeTexture");
		return r->CreateVolumeTexture(w, h, d, levels, usage, fmt, pool, pt);
	}
	HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT e, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
		IDirect3DCubeTexture8 **pt) override
	{
		Guard k(g.mx);
		Unsupported("CreateCubeTexture");
		return r->CreateCubeTexture(e, levels, usage, fmt, pool, pt);
	}
	template <class B> HRESULT Make_Buffer(HRESULT hr, B *b, B **out, Kind kind, uint16_t op, UINT len, DWORD usage, DWORD fvfOrFmt,
		D3DPOOL pool)
	{
		if (FAILED(hr) || out == nullptr)
			return hr;
		Evict(b);
		CapBuffer<B> *c = new CapBuffer<B>(b, kind);
		c->length = len; c->usage = usage; c->fvf = fvfOrFmt; c->format = fvfOrFmt; c->pool = uint32_t(pool);
		Register(c);
		*out = c;
		if (g.open) {
			Rec q(op);
			q.p.u32(c->id); q.p.u32(len); q.p.u32(usage); q.p.u32(fvfOrFmt); q.p.u32(uint32_t(pool));
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8 **pb) override
	{
		Guard k(g.mx);
		IDirect3DVertexBuffer8 *b = nullptr;
		const HRESULT hr = r->CreateVertexBuffer(len, usage, fvf, pool, pb != nullptr ? &b : nullptr);
		return Make_Buffer(hr, b, pb, K_VB, OP_CREATE_VB, len, usage, fvf, pool);
	}
	HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer8 **pb) override
	{
		Guard k(g.mx);
		IDirect3DIndexBuffer8 *b = nullptr;
		const HRESULT hr = r->CreateIndexBuffer(len, usage, fmt, pool, pb != nullptr ? &b : nullptr);
		return Make_Buffer(hr, b, pb, K_IB, OP_CREATE_IB, len, usage, uint32_t(fmt), pool);
	}
	CapSurface *Made_Surface(IDirect3DSurface8 *s, uint16_t op, uint32_t lockable)
	{
		bool isNew = false;
		Evict(s);
		CapSurface *w = Wrap_Surface(s, O_CREATE, nullptr, 0, 0, &isNew);
		w->createOp = op;
		w->lockable = lockable;
		return w;
	}
	HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, WINBOOL lockable,
		IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->CreateRenderTarget(w, h, fmt, ms, lockable, ps != nullptr ? &s : nullptr);
		if (FAILED(hr) || ps == nullptr)
			return hr;
		CapSurface *c = Made_Surface(s, OP_CREATE_RT, uint32_t(lockable));
		*ps = c;
		if (g.open) { Rec q(OP_CREATE_RT); q.p.u32(c->id); q.p.u32(w); q.p.u32(h); q.p.u32(uint32_t(fmt)); q.p.u32(uint32_t(ms)); q.p.u32(uint32_t(lockable)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->CreateDepthStencilSurface(w, h, fmt, ms, ps != nullptr ? &s : nullptr);
		if (FAILED(hr) || ps == nullptr)
			return hr;
		CapSurface *c = Made_Surface(s, OP_CREATE_DS, 0);
		*ps = c;
		if (g.open) { Rec q(OP_CREATE_DS); q.p.u32(c->id); q.p.u32(w); q.p.u32(h); q.p.u32(uint32_t(fmt)); q.p.u32(uint32_t(ms)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->CreateImageSurface(w, h, fmt, ps != nullptr ? &s : nullptr);
		if (FAILED(hr) || ps == nullptr)
			return hr;
		CapSurface *c = Made_Surface(s, OP_CREATE_IMAGE, 1);
		*ps = c;
		if (g.open) { Rec q(OP_CREATE_IMAGE); q.p.u32(c->id); q.p.u32(w); q.p.u32(h); q.p.u32(uint32_t(fmt)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8 *src, const RECT *rects, UINT n, IDirect3DSurface8 *dst, const POINT *pts) override
	{
		Guard k(g.mx);
		if (g.open)
			Refresh_Source(Find_Wrap(src));
		const HRESULT hr = r->CopyRects(Real_Of(src), rects, n, Real_Of(dst), pts);
		if (g.open && SUCCEEDED(hr)) {
			CapObj *so = Find_Wrap(src), *dob = Find_Wrap(dst);
			if (so != nullptr && dob != nullptr && Root(dob)->pool != D3DPOOL_DEFAULT && !g.testNoGpuMark &&
				(Root(so)->pool == D3DPOOL_DEFAULT || Root(so)->gpuWritten))
				Root(dob)->gpuWritten = true;
		}
		if (g.open) {
			Rec q(OP_COPY_RECTS, In_Shot() ? F_SHOT : 0);
			q.p.u32(Id(src)); q.p.u32(Id(dst)); q.p.u32(n);
			q.p.v<uint8_t>(rects != nullptr); q.p.v<uint8_t>(pts != nullptr);
			if (rects != nullptr) q.p.put(rects, sizeof(RECT) * n);
			if (pts != nullptr) q.p.put(pts, sizeof(POINT) * n);
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8 *src, IDirect3DBaseTexture8 *dst) override
	{
		Guard k(g.mx);
		CapObj *so = Find_Wrap(src);
		if (g.open)
			Refresh_Source(so);
		const HRESULT hr = r->UpdateTexture(Real_Of(src), Real_Of(dst));
		if (SUCCEEDED(hr) && so != nullptr)
			so->dirtyAny = false;	// UpdateTexture consumes the source's dirty region
		if (g.open) { Rec q(OP_UPDATE_TEXTURE); q.p.u32(Id(src)); q.p.u32(Id(dst)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8 *dst) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->GetFrontBuffer(Real_Of(dst));
		if (g.open) { Rec q(OP_GET_FRONT_BUFFER); q.p.u32(Id(dst)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8 *rt, IDirect3DSurface8 *ds) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetRenderTarget(Real_Of(rt), Real_Of(ds));
		if (g.open) { Rec q(OP_SET_RENDER_TARGET); q.p.u32(Id(rt)); q.p.u32(Id(ds)); q.emit(hr); }
		return hr;
	}
	HRESULT Get_Surface(HRESULT hr, IDirect3DSurface8 *s, IDirect3DSurface8 **ps, uint16_t op)
	{
		if (FAILED(hr) || ps == nullptr || s == nullptr) {
			if (ps != nullptr)
				*ps = s;
			return hr;
		}
		bool isNew = false;
		CapSurface *w = Classify(s, &isNew);
		*ps = w;
		if (g.open) { Rec q(op, isNew ? F_NEW : 0); q.p.u32(w->id); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->GetRenderTarget(ps != nullptr ? &s : nullptr);
		return Get_Surface(hr, s, ps, OP_GET_RENDERTARGET);
	}
	HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8 **ps) override
	{
		Guard k(g.mx);
		IDirect3DSurface8 *s = nullptr;
		const HRESULT hr = r->GetDepthStencilSurface(ps != nullptr ? &s : nullptr);
		return Get_Surface(hr, s, ps, OP_GET_DEPTHSTENCIL);
	}
	HRESULT STDMETHODCALLTYPE BeginScene() override
	{
		Guard k(g.mx);
		const HRESULT hr = r->BeginScene();
		if (g.open) { Rec q(OP_BEGIN_SCENE); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE EndScene() override
	{
		Guard k(g.mx);
		const HRESULT hr = r->EndScene();
		if (g.open) { Rec q(OP_END_SCENE); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE Clear(DWORD n, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->Clear(n, rects, flags, color, z, stencil);
		if (g.open) {
			Rec q(OP_CLEAR);
			q.p.u32(n); q.p.u32(flags); q.p.u32(color); q.p.v(z); q.p.u32(stencil); q.p.v<uint8_t>(rects != nullptr);
			if (rects != nullptr) q.p.put(rects, sizeof(D3DRECT) * n);
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetTransform(s, m);
		if (g.open && m != nullptr) { Rec q(OP_SET_TRANSFORM); q.p.u32(uint32_t(s)); q.p.v(*m); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX *m) override { return r->GetTransform(s, m); }
	HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->MultiplyTransform(s, m);	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
		if (g.open && m != nullptr) { Rec q(OP_MULTIPLY_TRANSFORM); q.p.u32(uint32_t(s)); q.p.v(*m); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8 *v) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetViewport(v);
		if (g.open && v != nullptr) { Rec q(OP_SET_VIEWPORT); q.p.v(*v); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8 *v) override { return r->GetViewport(v); }
	HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8 *m) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetMaterial(m);	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
		if (g.open && m != nullptr) { Rec q(OP_SET_MATERIAL); q.p.v(*m); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8 *m) override { return r->GetMaterial(m); }
	HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT8 *l) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetLight(i, l);	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
		if (SUCCEEDED(hr) && l != nullptr)
			g.lights.insert(i);
		if (g.open && l != nullptr) { Rec q(OP_SET_LIGHT); q.p.u32(i); q.p.v(*l); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT8 *l) override { return r->GetLight(i, l); }
	HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, WINBOOL e) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->LightEnable(i, e);	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
		if (SUCCEEDED(hr)) {
			g.lights.insert(i);
			// DXVK LightEnable: enabling takes the first free slot, disabling frees the light's slot, a no-op otherwise
			bool on = false;
			for (uint32_t s : g.lightSlots)
				on = on || s == i;
			if (on != (e != 0)) {
				const uint32_t from = e ? NO_LIGHT : i, to = e ? i : NO_LIGHT;
				for (uint32_t &s : g.lightSlots)
					if (s == from) { s = to; break; }
			}
		}
		if (g.open) { Rec q(OP_LIGHT_ENABLE); q.p.u32(i); q.p.u32(uint32_t(e)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, WINBOOL *e) override { return r->GetLightEnable(i, e); }
	HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float *p) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetClipPlane(i, p);
		if (g.open && p != nullptr) { Rec q(OP_SET_CLIP_PLANE); q.p.u32(i); q.p.put(p, sizeof(float) * 4); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float *p) override { return r->GetClipPlane(i, p); }
	HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetRenderState(s, v);
		if (SUCCEEDED(hr) && uint32_t(s) < 256) { g.rs[s] = v; g.rsSet[s] = 1; }
		if (g.open) { Rec q(OP_SET_RENDER_STATE); q.p.u32(uint32_t(s)); q.p.u32(v); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, DWORD *v) override { return r->GetRenderState(s, v); }
	HRESULT STDMETHODCALLTYPE BeginStateBlock() override { Guard k(g.mx); Unsupported("BeginStateBlock"); return r->BeginStateBlock(); }	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
	HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD *t) override { Guard k(g.mx); Unsupported("EndStateBlock"); return r->EndStateBlock(t); }	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
	HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD t) override { Guard k(g.mx); Unsupported("ApplyStateBlock"); return r->ApplyStateBlock(t); }	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
	HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD t) override { Guard k(g.mx); Unsupported("CaptureStateBlock"); return r->CaptureStateBlock(t); }	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
	HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD t) override { Guard k(g.mx); Unsupported("DeleteStateBlock"); return r->DeleteStateBlock(t); }
	HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE ty, DWORD *t) override
	{
		Guard k(g.mx);
		Unsupported("CreateStateBlock");
		return r->CreateStateBlock(ty, t);	// GX-WRAPSKIP-OK: capture proxy: forwards the wrapper's own call to the real device 1:1
	}
	HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8 *c) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetClipStatus(c);
		if (g.open && c != nullptr) { Rec q(OP_SET_CLIP_STATUS); q.p.v(*c); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8 *c) override { return r->GetClipStatus(c); }
	HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture8 **pt) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->GetTexture(stage, pt);
		if (SUCCEEDED(hr) && pt != nullptr && *pt != nullptr) {
			if (CapObj *o = Find_Real(*pt)) {
				const bool back = !Root(o)->alive;
				Set_Alive(Root(o), true);
				*pt = static_cast<IDirect3DBaseTexture8 *>(static_cast<IDirect3DTexture8 *>(static_cast<CapTexture *>(o)));
				if (g.open) { Rec q(OP_GET_TEXTURE, back ? F_NEW : 0); q.p.u32(o->id); q.p.u32(stage); q.emit(hr); }
			}
			else
				Warn("GetTexture returned a texture the proxy does not know");
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8 *t) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetTexture(stage, Real_Of(t));
		if (g.open) { Rec q(OP_SET_TEXTURE); q.p.u32(stage); q.p.u32(Id(t)); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD *v) override
	{
		return r->GetTextureStageState(stage, t, v);
	}
	HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetTextureStageState(stage, t, v);
		if (SUCCEEDED(hr) && stage < 8 && uint32_t(t) < 32) { g.tss[stage][t] = v; g.tssSet[stage][t] = 1; }
		if (g.open) { Rec q(OP_SET_TSS); q.p.u32(stage); q.p.u32(uint32_t(t)); q.p.u32(v); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *n) override { return r->ValidateDevice(n); }
	HRESULT STDMETHODCALLTYPE GetInfo(DWORD id, void *p, DWORD n) override { return r->GetInfo(id, p, n); }
	HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT i, const PALETTEENTRY *e) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetPaletteEntries(i, e);
		if (g.open && e != nullptr) { Rec q(OP_SET_PALETTE_ENTRIES); q.p.u32(i); q.p.put(e, sizeof(PALETTEENTRY) * 256); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT i, PALETTEENTRY *e) override { return r->GetPaletteEntries(i, e); }
	HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT i) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetCurrentTexturePalette(i);
		if (g.open) { Rec q(OP_SET_CURRENT_PALETTE); q.p.u32(i); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *i) override { return r->GetCurrentTexturePalette(i); }
	HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT n) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DrawPrimitive(t, start, n);
		Check_Stale_Shader();
		if (g.open) { Rec q(OP_DRAW_PRIMITIVE); q.p.u32(uint32_t(t)); q.p.u32(start); q.p.u32(n); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIdx, UINT nv, UINT start, UINT n) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DrawIndexedPrimitive(t, minIdx, nv, start, n);
		Check_Stale_Shader();
		if (g.open) { Rec q(OP_DRAW_INDEXED_PRIMITIVE); q.p.u32(uint32_t(t)); q.p.u32(minIdx); q.p.u32(nv); q.p.u32(start); q.p.u32(n); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT n, const void *data, UINT stride) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DrawPrimitiveUP(t, n, data, stride);
		Check_Stale_Shader();
		g.stride[0] = 0;	// DXVK sets stream 0 to {null, 0} (d3d8_device.cpp DrawPrimitiveUP)
		if (g.open) {
			Rec q(OP_DRAW_PRIMITIVE_UP);
			q.p.u32(uint32_t(t)); q.p.u32(n); q.p.u32(stride);
			Put_Bytes(q.p, data, data != nullptr ? size_t(Prim_Verts(t, n)) * stride : 0);
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minIdx, UINT nv, UINT n, const void *idx, D3DFORMAT ifmt,
		const void *data, UINT stride) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DrawIndexedPrimitiveUP(t, minIdx, nv, n, idx, ifmt, data, stride);
		Check_Stale_Shader();
		g.stride[0] = 0;	// and stream 0 and the indices to null
		if (g.open) {
			Rec q(OP_DRAW_INDEXED_PRIMITIVE_UP);
			q.p.u32(uint32_t(t)); q.p.u32(minIdx); q.p.u32(nv); q.p.u32(n); q.p.u32(uint32_t(ifmt)); q.p.u32(stride);
			const size_t isz = ifmt == D3DFMT_INDEX32 ? 4 : 2;
			Put_Bytes(q.p, idx, idx != nullptr ? size_t(Prim_Verts(t, n)) * isz : 0);
			Put_Bytes(q.p, data, data != nullptr ? size_t(minIdx + nv) * stride : 0);
			q.emit(hr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE ProcessVertices(UINT src, UINT dst, UINT n, IDirect3DVertexBuffer8 *vb, DWORD f) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->ProcessVertices(src, dst, n, Real_Of(vb), f);
		if (g.open) { Rec q(OP_PROCESS_VERTICES); q.p.u32(src); q.p.u32(dst); q.p.u32(n); q.p.u32(Id(vb)); q.p.u32(f); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *decl, const DWORD *func, DWORD *h, DWORD usage) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->CreateVertexShader(decl, func, h, usage);
		if (SUCCEEDED(hr) && h != nullptr) {
			ShaderBytes &s = g.vs[*h];
			s.decl.clear(); s.func.clear(); s.usage = usage;
			if (decl != nullptr) { const uint8_t *b = reinterpret_cast<const uint8_t *>(decl); s.decl.assign(b, b + Decl_Len(decl) * 4); }
			if (func != nullptr) { const uint8_t *b = reinterpret_cast<const uint8_t *>(func); s.func.assign(b, b + Func_Len(func) * 4); }
			if (g.open) Emit_Create_VS(*h, s, 0);
		}
		return hr;
	}
	static void Emit_Create_VS(uint32_t h, const ShaderBytes &s, uint16_t fl)
	{
		Rec q(OP_CREATE_VS, fl);
		q.p.u32(h); q.p.u32(s.usage);
		Put_Bytes(q.p, s.decl.data(), s.decl.size());
		Put_Bytes(q.p, s.func.data(), s.func.size());
		q.emit();
	}
	static void Emit_Create_PS(uint32_t h, const ShaderBytes &s, uint16_t fl)
	{
		Rec q(OP_CREATE_PS, fl);
		q.p.u32(h);
		Put_Bytes(q.p, s.func.data(), s.func.size());
		q.emit();
	}
	HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD h) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetVertexShader(h);
		// DXVK's d3d8 treats a set of its current handle as a no-op, and a deletion leaves that at 0: SetVertexShader(0) then
		// keeps the deleted shader bound (d3d8_device.cpp SetVertexShader / DeleteVertexShader; gxcap-design-r11 #4)
		if (SUCCEEDED(hr) && !(h == 0 && g.delVS)) { g.setVS = h; g.delVS = g.staleVS = false; }
		if (g.open) { Rec q(OP_SET_VS); q.p.u32(h); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD *h) override { return r->GetVertexShader(h); }
	HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD h) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DeleteVertexShader(h);
		if (SUCCEEDED(hr) && h == g.setVS && g.vs.count(h) != 0)
			g.delVS = true;	// a shader handle (not an FVF) deleted while bound
		g.vs.erase(h);
		if (g.open) { Rec q(OP_DELETE_VS); q.p.u32(h); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void *d, DWORD n) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetVertexShaderConstant(reg, d, n);
		if (g.open) { Rec q(OP_SET_VS_CONST); q.p.u32(reg); q.p.u32(n); Put_Bytes(q.p, d, d != nullptr ? size_t(n) * 16 : 0); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD reg, void *d, DWORD n) override { return r->GetVertexShaderConstant(reg, d, n); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD h, void *d, DWORD *n) override { return r->GetVertexShaderDeclaration(h, d, n); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD h, void *d, DWORD *n) override { return r->GetVertexShaderFunction(h, d, n); }
	HRESULT STDMETHODCALLTYPE SetStreamSource(UINT i, IDirect3DVertexBuffer8 *vb, UINT stride) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetStreamSource(i, Real_Of(vb), stride);
		if (SUCCEEDED(hr) && i < 16 && vb != nullptr)
			g.stride[i] = stride;	// DXVK keeps the previous stride for a null buffer (d3d8_device.cpp SetStreamSource)
		if (g.open) { Rec q(OP_SET_STREAM_SOURCE); q.p.u32(i); q.p.u32(Id(vb)); q.p.u32(stride); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetStreamSource(UINT i, IDirect3DVertexBuffer8 **pvb, UINT *stride) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->GetStreamSource(i, pvb, stride);
		if (SUCCEEDED(hr) && pvb != nullptr && *pvb != nullptr) {
			if (CapObj *o = Find_Real(*pvb)) {
				const bool back = !o->alive;
				o->alive = true;
				*pvb = static_cast<CapVB *>(o);
				if (g.open) { Rec q(OP_GET_STREAM_SOURCE, back ? F_NEW : 0); q.p.u32(o->id); q.p.u32(i); q.emit(hr); }
			}
			else Warn("GetStreamSource returned a buffer the proxy does not know");
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8 *ib, UINT base) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetIndices(Real_Of(ib), base);
		if (g.open) { Rec q(OP_SET_INDICES); q.p.u32(Id(ib)); q.p.u32(base); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8 **pib, UINT *base) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->GetIndices(pib, base);
		if (SUCCEEDED(hr) && pib != nullptr && *pib != nullptr) {
			if (CapObj *o = Find_Real(*pib)) {
				const bool back = !o->alive;
				o->alive = true;
				*pib = static_cast<CapIB *>(o);
				if (g.open) { Rec q(OP_GET_INDICES, back ? F_NEW : 0); q.p.u32(o->id); q.emit(hr); }
			}
			else Warn("GetIndices returned a buffer the proxy does not know");
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *func, DWORD *h) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->CreatePixelShader(func, h);
		if (SUCCEEDED(hr) && h != nullptr) {
			ShaderBytes &s = g.ps[*h];
			s.decl.clear(); s.func.clear();
			if (func != nullptr) { const uint8_t *b = reinterpret_cast<const uint8_t *>(func); s.func.assign(b, b + Func_Len(func) * 4); }
			if (g.open) Emit_Create_PS(*h, s, 0);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD h) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetPixelShader(h);
		if (SUCCEEDED(hr)) { g.setPS = h; g.delPS = g.stalePS = false; }
		if (g.open) { Rec q(OP_SET_PS); q.p.u32(h); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD *h) override { return r->GetPixelShader(h); }
	HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD h) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->DeletePixelShader(h);
		if (SUCCEEDED(hr) && h == g.setPS && h != 0)
			g.delPS = true;
		g.ps.erase(h);
		if (g.open) { Rec q(OP_DELETE_PS); q.p.u32(h); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void *d, DWORD n) override
	{
		Guard k(g.mx);
		const HRESULT hr = r->SetPixelShaderConstant(reg, d, n);
		if (g.open) { Rec q(OP_SET_PS_CONST); q.p.u32(reg); q.p.u32(n); Put_Bytes(q.p, d, d != nullptr ? size_t(n) * 16 : 0); q.emit(hr); }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD reg, void *d, DWORD n) override { return r->GetPixelShaderConstant(reg, d, n); }
	HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD h, void *d, DWORD *n) override { return r->GetPixelShaderFunction(h, d, n); }
	HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT h, const float *s, const D3DRECTPATCH_INFO *i) override
	{
		Guard k(g.mx);
		Unsupported("DrawRectPatch");
		return r->DrawRectPatch(h, s, i);
	}
	HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT h, const float *s, const D3DTRIPATCH_INFO *i) override
	{
		Guard k(g.mx);
		Unsupported("DrawTriPatch");
		return r->DrawTriPatch(h, s, i);
	}
	HRESULT STDMETHODCALLTYPE DeletePatch(UINT h) override { Guard k(g.mx); Unsupported("DeletePatch"); return r->DeletePatch(h); }
};

HRESULT Get_Device(IDirect3DDevice8 **pp)
{
	if (pp == nullptr)
		return D3DERR_INVALIDCALL;
	*pp = g.dev;
	if (g.dev == nullptr)
		return D3DERR_INVALIDCALL;
	g.dev->AddRef();
	return D3D_OK;
}

bool Read_Rows(const uint8_t *src, uint32_t pitch, const Layout &L, std::vector<uint8_t> &out)
{
	if (src == nullptr)
		return false;
	out.resize(size_t(L.rowBytes) * L.rows);
	for (uint32_t y = 0; y < L.rows; ++y)
		memcpy(&out[size_t(y) * L.rowBytes], src + size_t(y) * pitch, L.rowBytes);
	return true;
}

void Emit_Upload(const CapObj *o, uint32_t sub, uint32_t w, uint32_t h, const Layout &L, const std::vector<uint8_t> &bytes,
	uint16_t flags)
{
	Region rg = { o->id, sub, 0, sub, 0, 0, w, h, 0, 1, 0, L.rowBytes, L.rows, 1 };
	Rec q(OP_UPLOAD, flags);
	q.p.v(rg);
	Put_Bytes(q.p, bytes.data(), bytes.size());
	q.emit();
}

bool Has_Lock(const CapObj *o, uint32_t sub)
{
	for (const LockInfo &li : o->locks)
		if (li.sub == sub)
			return true;
	for (const CapObj *c : o->children)
		if (c->level == sub && !c->locks.empty())
			return true;
	return false;
}

// CPU content of a lockable (non-DEFAULT) texture level or surface, read through the REAL object.
bool Read_Level_CPU(CapObj *o, uint32_t level, std::vector<uint8_t> &buf, Layout &L, uint32_t &w, uint32_t &h)
{
	D3DLOCKED_RECT lr = {};
	bool ok = false;
	if (o->kind == K_TEXTURE) {
		IDirect3DTexture8 *t = static_cast<CapTexture *>(o)->r;
		D3DSURFACE_DESC d = {};
		if (FAILED(t->GetLevelDesc(level, &d)))
			return false;
		w = d.Width;
		h = d.Height;
		L = Format_Layout(uint32_t(d.Format), w, h);
		if (L.ok && SUCCEEDED(t->LockRect(level, &lr, nullptr, D3DLOCK_READONLY))) {
			ok = Read_Rows(static_cast<const uint8_t *>(lr.pBits), uint32_t(lr.Pitch), L, buf);
			t->UnlockRect(level);
		}
	} else if (o->kind == K_SURFACE) {
		IDirect3DSurface8 *s = static_cast<CapSurface *>(o)->r;
		w = o->width;
		h = o->height;
		L = Format_Layout(o->format, w, h);
		if (L.ok && SUCCEEDED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			ok = Read_Rows(static_cast<const uint8_t *>(lr.pBits), uint32_t(lr.Pitch), L, buf);
			s->UnlockRect();
		}
	}
	return ok;
}

void Refresh_Source(CapObj *o)
{
	if (o == nullptr || o->pool == D3DPOOL_DEFAULT || (o->kind != K_TEXTURE && o->kind != K_SURFACE) || g.testNoRefresh)
		return;
	if (Root(o)->gpuWritten)
		return;	// GPU-written inside this window: the replay produces it itself (see CapObj::gpuWritten)
	const uint32_t levels = o->kind == K_TEXTURE ? o->levels : 1;
	std::vector<uint8_t> buf;
	for (uint32_t l = 0; l < levels; ++l) {
		if (Has_Lock(o, l)) {
			Warn("a copy source is locked while it is copied: its bytes are not refreshed");
			continue;
		}
		Layout L = {};
		uint32_t w = 0, h = 0;
		if (!Read_Level_CPU(o, l, buf, L, w, h)) {
			Warn("a copy source could not be re-read before its copy (window UNSUPPORTED)");
			g.flags |= FH_UNSUPPORTED;
			continue;
		}
		Emit_Upload(o, l, w, h, L, buf, F_REFRESH);
	}
}

// ---- the snapshot (window start) ----

uint32_t Id_Of_Real(void *real, const char *what)
{
	if (real == nullptr)
		return 0;
	if (CapObj *o = Find_Real(real))
		if (Root(o)->alive || Root(o)->zombie)
			return o->id;
	if (g.testNoZombies && Find_Real(real) != nullptr)
		return 0;
	Warn(std::string("snapshot: a bound ") + what + " the proxy never saw - recorded as unbound (window UNSUPPORTED)");
	g.flags |= FH_UNSUPPORTED;
	return 0;
}

// GPU content of a single-sample DEFAULT-pool surface: CreateImageSurface + CopyRects (the path GX_SHOT uses) on the
// REAL device. A multisampled surface would come back resolved, which is not restorable: the caller leaves it undefined.
bool Read_Surface_GPU(IDirect3DSurface8 *s, std::vector<uint8_t> &out, Layout &L)
{
	D3DSURFACE_DESC d = {};
	if (FAILED(s->GetDesc(&d)) || d.MultiSampleType != D3DMULTISAMPLE_NONE)
		return false;
	L = Format_Layout(uint32_t(d.Format), d.Width, d.Height);
	if (!L.ok || Is_Depth_Format(uint32_t(d.Format)))
		return false;
	IDirect3DSurface8 *tmp = nullptr;
	if (FAILED(g.dev->r->CreateImageSurface(d.Width, d.Height, d.Format, &tmp)) || tmp == nullptr)
		return false;
	bool ok = SUCCEEDED(g.dev->r->CopyRects(s, nullptr, 0, tmp, nullptr));
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(tmp->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		ok = Read_Rows(static_cast<const uint8_t *>(lr.pBits), uint32_t(lr.Pitch), L, out);
		tmp->UnlockRect();
	} else {
		ok = false;
	}
	tmp->Release();
	return ok;
}

// Content of every subresource the snapshot can restore. Not restored, so UNDEFINED for the replayer's definedness check:
// back buffers (the gate must read what the window rendered, never pre-window pixels), depth/stencil (not readable in
// d3d8), multisampled surfaces (no restorable readback), and anything locked at the snapshot (recorded at its Unlock).
void Snapshot_Content(CapObj *o, uint64_t &bytes, uint32_t &undefinedSubs)
{
	std::vector<uint8_t> buf;
	if (o->kind == K_TEXTURE) {
		IDirect3DTexture8 *t = static_cast<CapTexture *>(o)->r;
		for (uint32_t l = 0; l < o->levels; ++l) {
			D3DSURFACE_DESC d = {};
			if (FAILED(t->GetLevelDesc(l, &d)) || Is_Depth_Format(uint32_t(d.Format)) || Has_Lock(o, l)) { ++undefinedSubs; continue; }
			Layout L = {};
			uint32_t w = d.Width, h = d.Height;
			bool ok = false, gpu = false;
			if (d.Pool != D3DPOOL_DEFAULT) {
				ok = Read_Level_CPU(o, l, buf, L, w, h);
			} else {
				IDirect3DSurface8 *s = nullptr;
				if (SUCCEEDED(t->GetSurfaceLevel(l, &s)) && s != nullptr) {
					ok = gpu = Read_Surface_GPU(s, buf, L);
					s->Release();
				}
			}
			if (!ok) { ++undefinedSubs; continue; }
			Emit_Upload(o, l, w, h, L, buf, uint16_t(F_SNAPSHOT | (gpu ? F_GPU : 0)));
			bytes += buf.size();
		}
		const DWORD lod = o->pool == D3DPOOL_MANAGED ? t->GetLOD() : 0;
		if (lod != 0) { Rec q(OP_SET_LOD, F_SNAPSHOT); q.p.u32(o->id); q.p.u32(lod); q.emit(); }
		if (o->pool == D3DPOOL_SYSTEMMEM) {
			Rec q(OP_DIRTY, F_SNAPSHOT);
			q.p.u32(o->id); q.p.v<uint8_t>(o->dirtyAny); q.p.v(o->dirty);
			q.emit();
		}
	} else if (o->kind == K_SURFACE && !Is_Subresource(o)) {
		if (o->origin == O_BACKBUFFER || Is_Depth_Format(o->format) || o->msaa != 0 || Has_Lock(o, 0)) { ++undefinedSubs; return; }
		Layout L = {};
		uint32_t w = o->width, h = o->height;
		bool ok = false, gpu = false;
		if (o->pool != D3DPOOL_DEFAULT)
			ok = Read_Level_CPU(o, 0, buf, L, w, h);
		else
			ok = gpu = Read_Surface_GPU(static_cast<CapSurface *>(o)->r, buf, L);
		if (!ok) { ++undefinedSubs; return; }
		Emit_Upload(o, 0, w, h, L, buf, uint16_t(F_SNAPSHOT | (gpu ? F_GPU : 0)));
		bytes += buf.size();
	} else if (o->kind == K_VB || o->kind == K_IB) {
		if (Has_Lock(o, 0)) { ++undefinedSubs; return; }
		BYTE *p = nullptr;
		const HRESULT hr = o->kind == K_VB ? static_cast<CapVB *>(o)->r->Lock(0, 0, &p, D3DLOCK_READONLY)
			: static_cast<CapIB *>(o)->r->Lock(0, 0, &p, D3DLOCK_READONLY);
		if (FAILED(hr) || p == nullptr) { ++undefinedSubs; return; }
		buf.assign(p, p + o->length);
		if (o->kind == K_VB) static_cast<CapVB *>(o)->r->Unlock();
		else static_cast<CapIB *>(o)->r->Unlock();
		const Layout L = { o->length, 1, true };
		Emit_Upload(o, 0, o->length, 1, L, buf, F_SNAPSHOT);
		bytes += buf.size();
	}
}

void Emit_Object(CapObj *o)
{
	Rec q(OP_END, F_SNAPSHOT);
	switch (o->origin) {
	case O_CREATE:
		if (o->kind == K_TEXTURE) {
			q.op = OP_CREATE_TEXTURE;
			q.p.u32(o->id); q.p.u32(o->width); q.p.u32(o->height); q.p.u32(o->levels); q.p.u32(o->usage); q.p.u32(o->format); q.p.u32(o->pool);
		} else if (o->kind == K_VB || o->kind == K_IB) {
			q.op = o->kind == K_VB ? OP_CREATE_VB : OP_CREATE_IB;
			q.p.u32(o->id); q.p.u32(o->length); q.p.u32(o->usage); q.p.u32(o->fvf); q.p.u32(o->pool);
		} else if (o->kind == K_SURFACE && o->createOp != 0) {
			q.op = o->createOp;
			q.p.u32(o->id); q.p.u32(o->width); q.p.u32(o->height); q.p.u32(o->format);
			if (o->createOp == OP_CREATE_RT || o->createOp == OP_CREATE_DS)
				q.p.u32(o->msaa);
			if (o->createOp == OP_CREATE_RT)
				q.p.u32(o->lockable);
		}
		break;
	case O_TEX_LEVEL:
		q.op = OP_GET_SURFACE_LEVEL;
		q.fl |= F_NEW;
		q.p.u32(o->id); q.p.u32(o->parent->id); q.p.u32(o->level);
		break;
	case O_BACKBUFFER:
		q.op = OP_GET_BACKBUFFER;
		q.fl |= F_NEW;
		q.p.u32(o->id); q.p.u32(o->level); q.p.u32(uint32_t(D3DBACKBUFFER_TYPE_MONO));
		break;
	case O_AUTODEPTH:
		q.op = OP_GET_DEPTHSTENCIL;
		q.fl |= F_NEW;
		q.p.u32(o->id);
		break;
	default:
		break;
	}
	if (q.op == OP_END) {
		q.op = OP_UNSUPPORTED;
		const char why[] = "snapshot: an object of unknown origin";
		q.p.put(why, sizeof(why) - 1);
		g.flags |= FH_UNSUPPORTED;
	}
	q.emit();
}

void Emit_Env()
{
	Rec q(OP_ENV, F_SNAPSHOT);
	char arch[16] = {};
	strncpy(arch, Arch_Name(), sizeof(arch) - 1);
	q.p.put(arch, sizeof(arch));
	fenv_t fe;
	fegetenv(&fe);
	q.p.u32(uint32_t(sizeof(fe)));
	q.p.put(&fe, sizeof(fe));
	D3DADAPTER_IDENTIFIER8 id = {};
	IDirect3D8 *d3d = nullptr;
	bool haveId = false;
	if (SUCCEEDED(g.dev->r->GetDirect3D(&d3d)) && d3d != nullptr) {
		haveId = SUCCEEDED(d3d->GetAdapterIdentifier(g.dev->adapter, 0, &id));
		d3d->Release();
	}
	q.p.v<uint8_t>(haveId);
	q.p.v(id);
	std::string env;
	for (char **e = environ; e != nullptr && *e != nullptr; ++e) {
		const std::string kv = *e;
		for (const char *pre : { "DXVK_", "MVK_", "VK_", "MESA_", "LP_", "GALLIUM_", "GX_D3D8RING", "GX_RENDER" })
			if (kv.compare(0, strlen(pre), pre) == 0) { env += kv; env.push_back('\0'); break; }
	}
	Put_Bytes(q.p, env.data(), env.size());
	Put_Bytes(q.p, g.d3d8Lib.data(), g.d3d8Lib.size());
	std::string conf;
	const char *cf = getenv("DXVK_CONFIG_FILE");
	if (FILE *f = fopen(cf != nullptr ? cf : "dxvk.conf", "rb")) {
		char b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof(b), f)) > 0)
			conf.append(b, n);
		fclose(f);
	}
	Put_Bytes(q.p, conf.data(), conf.size());
	// v2 tail: the executable name (DXVK picks an application profile by it) and the loaded d3d8/d3d9 fingerprints
	std::string exe;
#if defined(__APPLE__)
	exe = getprogname();
#else
	exe = program_invocation_short_name;
#endif
	Put_Bytes(q.p, exe.data(), exe.size());
	for (int lib = 0; lib < 2; ++lib) {
		std::string path = lib == 0 ? g.d3d8Lib : std::string();
		if (lib == 1) {
			for (const char *name : { "libdxvk_d3d9.so.0", "libdxvk_d3d9.0.dylib", "libdxvk_d3d9.so", "libdxvk_d3d9.dylib" }) {
				void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
				void *sym = h != nullptr ? dlsym(h, "Direct3DCreate9") : nullptr;
				Dl_info di = {};
				if (sym != nullptr && dladdr(sym, &di) != 0 && di.dli_fname != nullptr)
					path = di.dli_fname;
				if (h != nullptr)
					dlclose(h);
				if (!path.empty())
					break;
			}
		}
		std::vector<uint8_t> bytes;
		if (FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb")) {
			char b[65536];
			size_t n;
			while ((n = fread(b, 1, sizeof(b), f)) > 0)
				bytes.insert(bytes.end(), b, b + n);
			fclose(f);
		}
		Put_Bytes(q.p, path.data(), path.size());
		q.p.v<uint64_t>(bytes.size());
		q.p.v<uint64_t>(Hash64(bytes.data(), bytes.size()));
	}
	q.emit();
}

const uint32_t TRANSFORMS_FIXED[] = { 2, 3, 16, 17, 18, 19, 20, 21, 22, 23 };

void Snapshot_State(const std::vector<CapObj *> &zombies)
{
	IDirect3DDevice8 *d = g.dev->r;
	D3DCAPS8 caps = {};
	d->GetDeviceCaps(&caps);
	IDirect3DSurface8 *rt = nullptr, *ds = nullptr;
	d->GetRenderTarget(&rt);
	d->GetDepthStencilSurface(&ds);
	{ Rec q(OP_SET_RENDER_TARGET, F_SNAPSHOT); q.p.u32(Id_Of_Real(rt, "render target")); q.p.u32(Id_Of_Real(ds, "depth surface")); q.emit(); }
	if (rt) rt->Release();
	if (ds) ds->Release();
	D3DVIEWPORT8 vp = {};
	if (SUCCEEDED(d->GetViewport(&vp))) { Rec q(OP_SET_VIEWPORT, F_SNAPSHOT); q.p.v(vp); q.emit(); }
	for (uint32_t s = 0; s < 256; ++s) {
		if (!g.rsSet[s])
			continue;
		DWORD v = g.rs[s];
		if (g.testGetterState)
			d->GetRenderState(D3DRENDERSTATETYPE(s), &v);
		Rec q(OP_SET_RENDER_STATE, F_SNAPSHOT); q.p.u32(s); q.p.u32(v); q.emit();
	}
	for (uint32_t st = 0; st < 8; ++st) {
		for (uint32_t t = 0; t < 32; ++t)
			if (g.tssSet[st][t]) { Rec q(OP_SET_TSS, F_SNAPSHOT); q.p.u32(st); q.p.u32(t); q.p.u32(g.tss[st][t]); q.emit(); }
		IDirect3DBaseTexture8 *tex = nullptr;
		d->GetTexture(st, &tex);
		{ Rec q(OP_SET_TEXTURE, F_SNAPSHOT); q.p.u32(st); q.p.u32(Id_Of_Real(tex, "texture")); q.emit(); }
		if (tex) tex->Release();
	}
	std::vector<uint32_t> xf(TRANSFORMS_FIXED, TRANSFORMS_FIXED + sizeof(TRANSFORMS_FIXED) / sizeof(TRANSFORMS_FIXED[0]));
	for (uint32_t i = 0; i < 256; ++i)
		xf.push_back(256 + i);
	for (uint32_t s : xf) {
		D3DMATRIX m = {};
		if (SUCCEEDED(d->GetTransform(D3DTRANSFORMSTATETYPE(s), &m))) { Rec q(OP_SET_TRANSFORM, F_SNAPSHOT); q.p.u32(s); q.p.v(m); q.emit(); }
	}
	D3DMATERIAL8 mat = {};
	if (SUCCEEDED(d->GetMaterial(&mat))) { Rec q(OP_SET_MATERIAL, F_SNAPSHOT); q.p.v(mat); q.emit(); }
	for (uint32_t i : g.lights) {
		D3DLIGHT8 l = {};
		if (SUCCEEDED(d->GetLight(i, &l))) { Rec q(OP_SET_LIGHT, F_SNAPSHOT); q.p.u32(i); q.p.v(l); q.emit(); }
	}
	// Enable in slot order on the fresh replay device; a hole is held by a placeholder light, released afterwards.
	int last = -1;
	for (int k = 0; k < 8; ++k)
		if (g.lightSlots[k] != NO_LIGHT)
			last = k;
	const uint32_t dummyBase = g.lights.empty() ? 0 : *g.lights.rbegin() + 1;
	std::vector<uint32_t> holes;
	for (int k = 0; k <= last; ++k) {
		const uint32_t idx = g.lightSlots[k] != NO_LIGHT ? g.lightSlots[k] : dummyBase + uint32_t(k);
		if (g.lightSlots[k] == NO_LIGHT)
			holes.push_back(idx);
		Rec q(OP_LIGHT_ENABLE, F_SNAPSHOT); q.p.u32(idx); q.p.u32(1); q.emit();
	}
	for (uint32_t idx : holes) { Rec q(OP_LIGHT_ENABLE, F_SNAPSHOT); q.p.u32(idx); q.p.u32(0); q.emit(); }
	for (uint32_t i = 0; i < std::min<uint32_t>(caps.MaxUserClipPlanes, 6); ++i) {
		float p[4] = {};
		if (SUCCEEDED(d->GetClipPlane(i, p))) { Rec q(OP_SET_CLIP_PLANE, F_SNAPSHOT); q.p.u32(i); q.p.put(p, sizeof(p)); q.emit(); }
	}
	for (uint32_t i = 0; i < 16; ++i) {
		IDirect3DVertexBuffer8 *vb = nullptr;
		UINT stride = 0;
		if (FAILED(d->GetStreamSource(i, &vb, &stride)))
			continue;
		if (vb != nullptr || g.stride[i] != 0) {
			Rec q(OP_SET_STREAM_SOURCE, F_SNAPSHOT); q.p.u32(i); q.p.u32(Id_Of_Real(vb, "vertex buffer")); q.p.u32(g.stride[i]); q.emit();
		}
		if (vb) vb->Release();
	}
	IDirect3DIndexBuffer8 *ib = nullptr;
	UINT base = 0;
	if (SUCCEEDED(d->GetIndices(&ib, &base))) { Rec q(OP_SET_INDICES, F_SNAPSHOT); q.p.u32(Id_Of_Real(ib, "index buffer")); q.p.u32(base); q.emit(); }
	if (ib) ib->Release();
	DWORD h = 0;
	if (SUCCEEDED(d->GetVertexShader(&h))) { Rec q(OP_SET_VS, F_SNAPSHOT); q.p.u32(h); q.emit(); }
	h = 0;
	if (SUCCEEDED(d->GetPixelShader(&h))) { Rec q(OP_SET_PS, F_SNAPSHOT); q.p.u32(h); q.emit(); }
	const uint32_t nvc = std::min<uint32_t>(caps.MaxVertexShaderConst, 256);
	if (nvc > 0) {
		std::vector<float> c(size_t(nvc) * 4);
		if (SUCCEEDED(d->GetVertexShaderConstant(0, c.data(), nvc))) {
			Rec q(OP_SET_VS_CONST, F_SNAPSHOT); q.p.u32(0); q.p.u32(nvc); Put_Bytes(q.p, c.data(), c.size() * 4); q.emit();
		}
	}
	float pc[8 * 4] = {};
	if (SUCCEEDED(d->GetPixelShaderConstant(0, pc, 8))) {
		Rec q(OP_SET_PS_CONST, F_SNAPSHOT); q.p.u32(0); q.p.u32(8); Put_Bytes(q.p, pc, sizeof(pc)); q.emit();
	}
	// Bound but publicly released objects: created, filled and bound above, now released so the replay holds them only
	// through the binding - as the game does.
	for (CapObj *z : zombies) { Rec q(OP_DESTROY, F_SNAPSHOT); q.p.u32(z->id); q.emit(); }
}

void Mark_Zombie(void *real, std::vector<CapObj *> &zombies)
{
	CapObj *o = Find_Real(real);
	if (o == nullptr || g.testNoZombies)
		return;
	o = Root(o);
	if (!o->alive && !o->zombie) {
		o->zombie = true;
		zombies.push_back(o);
	}
}

void Snapshot()
{
	IDirect3DDevice8 *d = g.dev->r;
	// Publicly released objects still bound to the device: they must exist in the replay.
	std::vector<CapObj *> zombies;
	IDirect3DSurface8 *s = nullptr;
	if (SUCCEEDED(d->GetRenderTarget(&s)) && s) { Mark_Zombie(s, zombies); s->Release(); }
	s = nullptr;
	if (SUCCEEDED(d->GetDepthStencilSurface(&s)) && s) { Mark_Zombie(s, zombies); s->Release(); }
	for (uint32_t st = 0; st < 8; ++st) {
		IDirect3DBaseTexture8 *t = nullptr;
		if (SUCCEEDED(d->GetTexture(st, &t)) && t) { Mark_Zombie(t, zombies); t->Release(); }
	}
	for (uint32_t i = 0; i < 16; ++i) {
		IDirect3DVertexBuffer8 *vb = nullptr;
		UINT stride = 0;
		if (SUCCEEDED(d->GetStreamSource(i, &vb, &stride)) && vb) { Mark_Zombie(vb, zombies); vb->Release(); }
	}
	IDirect3DIndexBuffer8 *ib = nullptr;
	UINT base = 0;
	if (SUCCEEDED(d->GetIndices(&ib, &base)) && ib) { Mark_Zombie(ib, zombies); ib->Release(); }

	std::vector<CapObj *> objs;
	objs.reserve(g.byWrap.size());
	for (auto &kv : g.byWrap) {
		CapObj *root = Root(kv.second);
		if (root->alive || root->zombie)
			objs.push_back(kv.second);
	}
	std::sort(objs.begin(), objs.end(), [](const CapObj *a, const CapObj *b) { return a->id < b->id; });
	Emit_Env();
	for (CapObj *o : objs)
		Emit_Object(o);
	for (auto &kv : g.vs)
		CapDevice::Emit_Create_VS(kv.first, kv.second, F_SNAPSHOT);
	for (auto &kv : g.ps)
		CapDevice::Emit_Create_PS(kv.first, kv.second, F_SNAPSHOT);
	uint64_t bytes = 0;
	uint32_t undefinedSubs = 0;
	for (CapObj *o : objs)
		Snapshot_Content(o, bytes, undefinedSubs);
	Snapshot_State(zombies);
	for (CapObj *z : zombies)
		z->zombie = false;
	Rec(OP_SNAPSHOT_END, F_SNAPSHOT).emit();
	char line[320];
	snprintf(line, sizeof(line), "snapshot: %zu objects (%zu bound after release), %zu vs + %zu ps, %llu content bytes, %u subresources "
		"left undefined (back buffers, depth, multisampled, locked or unreadable)", objs.size(), zombies.size(), g.vs.size(), g.ps.size(),
		(unsigned long long)bytes, undefinedSubs);
	Log("%s", line);
}

void Close_Window(bool truncated)
{
	if (g.inShot)
		Warn("window closed inside a screenshot readback");
	Rec(OP_END).emit();
	if (truncated)
		g.flags |= FH_TRUNCATED;
	FileHeader h = {};
	memcpy(h.magic, MAGIC, sizeof(MAGIC));
	h.version = VERSION;
	h.logicFrame = g.winFrame;
	h.drawFrame = g.drawFrame;
	h.span = g.span;
	h.flags = g.flags;
	const D3DPRESENT_PARAMETERS &p = g.dev->pp;
	h.adapter = g.dev->adapter;
	h.deviceType = uint32_t(g.dev->type);
	h.behavior = g.dev->behavior;
	h.ppBackBufferWidth = p.BackBufferWidth;
	h.ppBackBufferHeight = p.BackBufferHeight;
	h.ppBackBufferFormat = uint32_t(p.BackBufferFormat);
	h.ppBackBufferCount = p.BackBufferCount;
	h.ppMultiSampleType = uint32_t(p.MultiSampleType);
	h.ppSwapEffect = uint32_t(p.SwapEffect);
	h.ppWindowed = uint32_t(p.Windowed);
	h.ppEnableAutoDepthStencil = uint32_t(p.EnableAutoDepthStencil);
	h.ppAutoDepthStencilFormat = uint32_t(p.AutoDepthStencilFormat);
	h.ppFlags = p.Flags;
	h.ppRefreshRate = p.FullScreen_RefreshRateInHz;
	h.ppPresentationInterval = p.FullScreen_PresentationInterval;
	strncpy(h.buildSha, g.sha.c_str(), sizeof(h.buildSha) - 1);
	strncpy(h.arch, Arch_Name(), sizeof(h.arch) - 1);
	char name[64];
	snprintf(name, sizeof(name), "/f%u.gxcap", g.winFrame);
	const std::string path = g.dir + name;
	FILE *f = fopen(path.c_str(), "wb");
	bool ok = f != nullptr && fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(g.rec.b.data(), 1, g.rec.b.size(), f) == g.rec.b.size();
	if (f != nullptr && fclose(f) != 0)
		ok = false;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.t0).count();
	char line[512];
	snprintf(line, sizeof(line), "window f=%u draw_frame=%u file=%s %s records=%llu bytes=%zu new_blobs=%llu new_blob_bytes=%llu shot=%s "
		"flags=%s%s%s wall_ms=%.0f", g.winFrame, g.drawFrame, path.c_str(), ok ? "written" : "WRITE-FAILED",
		(unsigned long long)g.records, g.rec.b.size(), (unsigned long long)g.newBlobs, (unsigned long long)g.newBlobBytes,
		(g.flags & FH_SHOT) ? "yes" : "no", (g.flags & FH_TRUNCATED) ? "TRUNCATED " : "", (g.flags & FH_UNSUPPORTED) ? "UNSUPPORTED " : "",
		(g.flags & (FH_TRUNCATED | FH_UNSUPPORTED)) ? "" : "clean", ms);
	Log("%s", line);
	if (FILE *m = fopen((g.dir + "/manifest.txt").c_str(), "a")) {
		fprintf(m, "%s\n", line);
		fclose(m);
	}
	g.open = false;
	g.inShot = false;
	g.rec.b.clear();
	g.rec.b.shrink_to_fit();
}

} // namespace

namespace gxcap
{

bool Rec_Configure(const std::string &dir, const std::string &buildSha)
{
	Guard k(g.mx);
	std::error_code ec;
	std::filesystem::create_directories(dir + "/blobs", ec);
	if (ec)
		return false;
	g.dir = dir;
	g.sha = buildSha;
	g.testNoRefresh = getenv("GXCAP_TEST_NO_REFRESH") != nullptr;
	g.testNoZombies = getenv("GXCAP_TEST_NO_ZOMBIES") != nullptr;
	g.testGetterState = getenv("GXCAP_TEST_GETTER_STATE") != nullptr;
	g.testNoGpuMark = getenv("GXCAP_TEST_NO_GPU_MARK") != nullptr;
	g.testAllowNested = getenv("GXCAP_TEST_ALLOW_NESTED") != nullptr;
	if (g.testNoRefresh || g.testNoZombies || g.testGetterState || g.testNoGpuMark || g.testAllowNested)
		Log("TEST KNOB active (a negative arm): the capture is deliberately incomplete%s", "");
	return true;
}

IDirect3DDevice8 *Rec_Wrap_Device(IDirect3DDevice8 *real, UINT adapter, D3DDEVTYPE type, DWORD behavior, const D3DPRESENT_PARAMETERS *pp)
{
	Guard k(g.mx);
	if (real == nullptr || pp == nullptr || g.dir.empty())
		return real;
	if (g.dev != nullptr) {
		Warn("a second device was created while the first lives: not wrapped");
		return real;
	}
	g.dev = new CapDevice(real, adapter, type, behavior, pp);
	for (uint32_t &s : g.lightSlots)
		s = NO_LIGHT;
	Dl_info info = {};
	if (dladdr(*reinterpret_cast<void **>(real), &info) != 0 && info.dli_fname != nullptr)
		g.d3d8Lib = info.dli_fname;
	char line[1024];
	snprintf(line, sizeof(line), "device wrapped: %ux%u fmt=%u backbuffers=%u msaa=%u windowed=%u swap=%u d3d8=%s", pp->BackBufferWidth,
		pp->BackBufferHeight, unsigned(pp->BackBufferFormat), unsigned(pp->BackBufferCount), unsigned(pp->MultiSampleType),
		unsigned(pp->Windowed), unsigned(pp->SwapEffect), g.d3d8Lib.c_str());
	Log("%s", line);
	if (FILE *m = fopen((g.dir + "/manifest.txt").c_str(), "a")) {
		fprintf(m, "build=%s %s\n", g.sha.c_str(), line);
		fclose(m);
	}
	return g.dev;
}

bool Rec_Has_Device()
{
	return g.dev != nullptr;
}

bool Rec_Is_Open()
{
	return g.open;
}

bool Rec_Shot_Seen()
{
	return g.shotSeen;
}

void Rec_Open(uint32_t logicFrame, uint32_t drawFrame, uint32_t span)
{
	Guard k(g.mx);
	if (g.dev == nullptr || g.open)
		return;
	g.rec.b.clear();
	g.records = 0;
	g.newBlobs = 0;
	g.newBlobBytes = 0;
	g.flags = g.unsupportedEver ? FH_UNSUPPORTED : 0;
	g.winFrame = logicFrame;
	g.drawFrame = drawFrame;
	g.span = span;
	for (auto &kv : g.byWrap)
		kv.second->gpuWritten = false;
	g.staleVS = g.delVS;	// a deletion INSIDE the window replays as itself; only one before it is beyond the snapshot
	g.stalePS = g.delPS;
	g.inShot = false;
	g.shotSeen = false;
	g.open = true;
	g.t0 = std::chrono::steady_clock::now();
	Snapshot();
}

void Rec_Close(bool truncated)
{
	Guard k(g.mx);
	if (g.open && g.dev != nullptr)
		Close_Window(truncated);
}

void Rec_Shot_Begin(uint32_t logicFrame, bool requested, const char *name)
{
	Guard k(g.mx);
	if (!g.open)
		return;
	if (g.inShot && !g.testAllowNested) {	// overlapping brackets: the inner end closes the outer
		Warn("overlapping screenshot brackets: window UNSUPPORTED");
		g.flags |= FH_UNSUPPORTED;
	}
	g.inShot = true;
	g.shotThread = std::this_thread::get_id();
	g.shotRequested = requested;
	Rec q(OP_SHOT_BEGIN);
	q.p.u32(logicFrame);
	q.p.v<uint8_t>(requested);
	q.p.put(name, strlen(name));
	q.emit();
}

// Only a completed readback that GX_SHOT asked for counts as the window's shot (a hotkey screenshot, or one that
// returned early, does not close the window).
void Rec_Shot_End(bool completed)
{
	Guard k(g.mx);
	if (!g.open || !g.inShot)
		return;
	g.inShot = false;
	if (completed && g.shotRequested) {
		g.shotSeen = true;
		g.flags |= FH_SHOT;
	}
	Rec q(OP_SHOT_END);
	q.p.v<uint8_t>(completed);
	q.emit();
}

} // namespace gxcap

#endif
