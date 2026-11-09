// GeneralsX @feature D · rw3-X-2 13/10/2026 A23-B arm 26 (DESIGN_A23_wide-ring.md v8.1 §7, r5 P2-6 / r6 P2-6): the front-only
// results. Every F entry point of the native device and its resources is called with valid, null and out-of-range arguments,
// between setters that make the state non-default; each call's HRESULT and outputs are hashed - COM pointers as the object's
// identity (first-seen order; aliases therefore compare), lock pointers as (allocation identity, offset), every other byte
// literally. Built twice: against the BASE device (8244ef7bd, no A23-B) and against the A23-B device (GX_A23B_FRONT defined:
// run with GX_NATIVE_CS=0 and =1, the SERIAL schedule on). STRICT: the three digests must be EQUAL.
// Plant (A23-B build only): GX_FRONT_PLANT=getmaterial-ok = X-cs-getmaterial-ok (GetMaterial answers D3D_OK) - must differ.
#include "GxNativeDevice.h"
#include "GxNativeUuid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace GXRender::Dev;

namespace {

class StubD3D8 final : public IDirect3D8
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** pp) override { *pp = nullptr; return E_NOINTERFACE; }
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
	ULONG STDMETHODCALLTYPE Release() override { return --m_ref; }
	HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3DERR_INVALIDCALL; }
	UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
	HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER8* id) override
	{
		std::memset(id, 0, sizeof(*id));
		std::snprintf(id->Description, sizeof(id->Description), "stub");
		return D3D_OK;
	}
	UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT) override { return 1; }
	HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT, UINT, D3DDISPLAYMODE* m) override { return GetAdapterDisplayMode(0, m); }
	HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* m) override
	{
		m->Width = 640;
		m->Height = 480;
		m->RefreshRate = 60;
		m->Format = D3DFMT_X8R8G8B8;
		return D3D_OK;
	}
	HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS8* c) override
	{
		std::memset(c, 0, sizeof(*c));
		c->MaxTextureBlendStages = 8;
		c->MaxSimultaneousTextures = 8;
		c->MaxStreams = 16;
		return D3D_OK;
	}
	HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override { return nullptr; }
	HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8**) override
	{
		return D3DERR_INVALIDCALL;
	}

private:
	ULONG m_ref = 1;
};

struct Recorder
{
	uint64_t h = 1469598103934665603ull;
	uint64_t calls = 0;
	std::map<const void*, uint64_t> objects;              // COM identity, first-seen
	std::map<std::pair<int, const void*>, uint64_t> bases;   // (resource, allocation base) -> identity
	FILE* dump = nullptr;
	std::string line;

	void mix(uint64_t v)
	{
		for (int i = 0; i < 8; ++i) {
			h ^= (v >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
		if (dump)
			line += " " + std::to_string(v);
	}
	void bytes(const void* p, size_t n)
	{
		const uint8_t* b = static_cast<const uint8_t*>(p);
		for (size_t i = 0; i < n; ++i) {
			h ^= b[i];
			h *= 1099511628211ull;
		}
		if (dump)
			line += " b" + std::to_string(n);
	}
	void begin(const char* name)
	{
		++calls;
		for (const char* c = name; *c; ++c)
			mix(uint8_t(*c));
		if (dump)
			line = name;
	}
	void end()
	{
		if (dump)
			std::fprintf(dump, "%s\n", line.c_str());
	}
	void hr(HRESULT r) { mix(uint64_t(uint32_t(r))); }
	void object(const void* p)
	{
		if (!p) {
			mix(0);
			return;
		}
		auto it = objects.find(p);
		if (it == objects.end())
			it = objects.emplace(p, objects.size() + 1).first;
		mix(1000 + it->second);
	}
	// a lock pointer: the allocation it points into (per resource, first-seen) and the offset in it
	void lockPtr(int resource, const void* p, size_t offset)
	{
		if (!p) {
			mix(0);
			return;
		}
		const void* base = static_cast<const uint8_t*>(p) - offset;
		auto key = std::make_pair(resource, base);
		auto it = bases.find(key);
		if (it == bases.end())
			it = bases.emplace(key, bases.size() + 1).first;
		mix(2000 + it->second);
		mix(offset);
	}
};

// GetRenderState etc. write their output before an error in some paths: every output is pre-filled with a pattern and hashed
template <class T>
void poison(T* t) { std::memset(t, 0xa5, sizeof(T)); }

} // namespace

int main(int argc, char** argv)
{
	const char* label = argc > 1 ? argv[1] : "run";
	Recorder rec;
	if (const char* d = std::getenv("GX_FRONT_DUMP"))
		rec.dump = std::fopen((std::string(d) + label + ".txt").c_str(), "w");
	StubD3D8 parent;
	GxNativeHost host;
	host.validation = true;
	host.prebuildGenericSet = false;
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice8* dev = nullptr;
	std::string why;
	if (FAILED(GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_MIXED_VERTEXPROCESSING, pp, host, &dev, &why))) {
		std::fprintf(stderr, "device creation failed: %s\n", why.c_str());
		return 2;
	}
	GxNativeDevice* nd = GxNativeFromDevice(dev);
#if defined(GX_A23B_FRONT)
	nd->debug().csSerial = true;   // the SERIAL schedule: a DISCARD's slice choice is the same in every mode
	GXRender::g_gxPipeTestMutants.syncSpec = true;
	const char* plant = std::getenv("GX_FRONT_PLANT");
	const bool plantGetMaterial = plant && !std::strcmp(plant, "getmaterial-ok");
	std::printf("FRONTMODE %s cs=%s\n", label, nd->csOn() ? "on" : "off");
#else
	const bool plantGetMaterial = false;
	std::printf("FRONTMODE %s base\n", label);
#endif
	(void)nd;

	// non-default state through the setters (A at CS=1: the getters below must answer from the front)
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
	dev->SetRenderState(D3DRS_ALPHAREF, 0x55);
	dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x12345678);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
	dev->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, 2);
	D3DMATRIX m = {};
	for (int i = 0; i < 16; ++i)
		(&m._11)[i] = float(i) * 0.25f - 1.f;
	dev->SetTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	m._44 = 3.f;
	dev->SetTransform(D3DTS_PROJECTION, &m);
	m._11 = 7.f;
	dev->SetTransform(D3DTS_TEXTURE3, &m);
	D3DVIEWPORT8 vp = { 3, 4, 50, 40, 0.25f, 0.75f };
	dev->SetViewport(&vp);
	D3DMATERIAL8 mat;
	for (int i = 0; i < int(sizeof(mat) / 4); ++i)
		reinterpret_cast<float*>(&mat)[i] = float(i) / 3.f;
	dev->SetMaterial(&mat);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	DWORD vsDecl[] = { D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_END() };
	DWORD vsh = 0;
	dev->CreateVertexShader(vsDecl, nullptr, &vsh, 0);
	dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);

	IDirect3DTexture8* tex = nullptr;
	dev->CreateTexture(16, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
	IDirect3DVertexBuffer8 *vbd = nullptr, *vbs = nullptr;
	dev->CreateVertexBuffer(1024, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vbd);
	dev->CreateVertexBuffer(512, 0, D3DFVF_XYZ, D3DPOOL_MANAGED, &vbs);
	IDirect3DIndexBuffer8* ib = nullptr;
	dev->CreateIndexBuffer(256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
	IDirect3DSurface8* img = nullptr;
	dev->CreateImageSurface(8, 8, D3DFMT_A8R8G8B8, &img);
	dev->SetTexture(2, tex);
	dev->SetStreamSource(1, vbs, 12);
	dev->SetIndices(ib, 5);

	// --- device getters
	for (DWORD s = 0; s <= 256; ++s) {
		DWORD v;
		poison(&v);
		rec.begin("GetRenderState");
		rec.mix(s);
		rec.hr(dev->GetRenderState(D3DRENDERSTATETYPE(s), &v));
		rec.mix(v);
		rec.end();
	}
	rec.begin("GetRenderState-null");
	rec.hr(dev->GetRenderState(D3DRS_CULLMODE, nullptr));
	rec.end();
	for (DWORD st = 0; st <= 8; ++st)
		for (DWORD t = 0; t <= 32; ++t) {
			DWORD v;
			poison(&v);
			rec.begin("GetTextureStageState");
			rec.mix(st);
			rec.mix(t);
			rec.hr(dev->GetTextureStageState(st, D3DTEXTURESTAGESTATETYPE(t), &v));
			rec.mix(v);
			rec.end();
		}
	for (DWORD t : { DWORD(D3DTS_WORLD), DWORD(D3DTS_VIEW), DWORD(D3DTS_PROJECTION), DWORD(D3DTS_TEXTURE0), DWORD(D3DTS_TEXTURE3),
		     DWORD(D3DTS_TEXTURE7), DWORD(4), DWORD(257) }) {
		D3DMATRIX o;
		poison(&o);
		rec.begin("GetTransform");
		rec.mix(t);
		rec.hr(dev->GetTransform(D3DTRANSFORMSTATETYPE(t), &o));
		rec.bytes(&o, sizeof(o));
		rec.end();
	}
	rec.begin("GetTransform-null");
	rec.hr(dev->GetTransform(D3DTS_WORLD, nullptr));
	rec.end();
	{
		D3DVIEWPORT8 o;
		poison(&o);
		rec.begin("GetViewport");
		rec.hr(dev->GetViewport(&o));
		rec.bytes(&o, sizeof(o));
		rec.end();
		rec.begin("GetViewport-null");
		rec.hr(dev->GetViewport(nullptr));
		rec.end();
	}
	{
		D3DMATERIAL8 o;
		poison(&o);
		rec.begin("GetMaterial");
		HRESULT r = dev->GetMaterial(&o);
		if (plantGetMaterial)
			r = D3D_OK;
		rec.hr(r);
		rec.bytes(&o, sizeof(o));
		rec.end();
		rec.begin("GetMaterial-null");
		rec.hr(dev->GetMaterial(nullptr));
		rec.end();
	}
	{
		D3DLIGHT8 l;
		poison(&l);
		BOOL b = 7;
		float plane[4];
		poison(&plane);
		rec.begin("GetLight");
		rec.hr(dev->GetLight(0, &l));
		rec.bytes(&l, sizeof(l));
		rec.hr(dev->GetLightEnable(0, &b));
		rec.mix(uint64_t(b));
		rec.hr(dev->GetClipPlane(0, plane));
		rec.bytes(plane, sizeof(plane));
		rec.end();
	}
	for (DWORD st = 0; st <= 8; ++st) {
		IDirect3DBaseTexture8* t = reinterpret_cast<IDirect3DBaseTexture8*>(uintptr_t(0x1));
		rec.begin("GetTexture");
		rec.mix(st);
		const HRESULT r = dev->GetTexture(st, &t);
		rec.hr(r);
		rec.object(r == D3D_OK ? t : nullptr);
		if (r == D3D_OK && t)
			rec.mix(t->Release());
		rec.end();
	}
	rec.begin("GetTexture-null");
	rec.hr(dev->GetTexture(0, nullptr));
	rec.end();
	for (UINT s : { 0u, 1u, 15u, 16u }) {
		IDirect3DVertexBuffer8* v = nullptr;
		UINT stride = 0xa5a5a5a5u;
		rec.begin("GetStreamSource");
		rec.mix(s);
		const HRESULT r = dev->GetStreamSource(s, &v, &stride);
		rec.hr(r);
		rec.object(r == D3D_OK ? v : nullptr);
		rec.mix(stride);
		if (r == D3D_OK && v)
			rec.mix(v->Release());
		rec.end();
	}
	{
		IDirect3DIndexBuffer8* o = reinterpret_cast<IDirect3DIndexBuffer8*>(uintptr_t(0x1));
		UINT bv = 0xa5a5a5a5u;
		rec.begin("GetIndices");
		const HRESULT r = dev->GetIndices(&o, &bv);
		rec.hr(r);
		rec.object(o);
		rec.mix(bv);
		if (r == D3D_OK && o)
			rec.mix(o->Release());
		o = reinterpret_cast<IDirect3DIndexBuffer8*>(uintptr_t(0x1));
		rec.hr(dev->GetIndices(&o, nullptr));
		rec.object(o);
		rec.hr(dev->GetIndices(nullptr, &bv));
		rec.end();
	}
	{
		DWORD h1 = 0xa5a5a5a5u, h2 = 0xa5a5a5a5u;
		rec.begin("GetShaders");
		rec.hr(dev->GetVertexShader(&h1));
		rec.mix(h1);
		rec.hr(dev->GetPixelShader(&h2));
		rec.mix(h2);
		rec.hr(dev->GetVertexShader(nullptr));
		rec.hr(dev->GetPixelShader(nullptr));
		rec.end();
	}
	{
		IDirect3DSurface8 *rt = nullptr, *ds = nullptr, *b0 = nullptr, *b0b = nullptr, *b5 = nullptr;
		rec.begin("GetTargets");
		rec.hr(dev->GetRenderTarget(&rt));
		rec.object(rt);
		rec.hr(dev->GetDepthStencilSurface(&ds));
		rec.object(ds);
		rec.hr(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &b0));
		rec.object(b0);
		rec.hr(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &b0b));
		rec.object(b0b);
		rec.hr(dev->GetBackBuffer(5, D3DBACKBUFFER_TYPE_MONO, &b5));
		rec.object(b5);
		rec.hr(dev->GetRenderTarget(nullptr));
		for (IDirect3DSurface8* s : { rt, ds, b0 })
			if (s) {
				D3DSURFACE_DESC sd;
				poison(&sd);
				rec.hr(s->GetDesc(&sd));
				rec.bytes(&sd, sizeof(sd));
				void* c = reinterpret_cast<void*>(uintptr_t(0x1));
				const HRESULT r = s->GetContainer(__uuidof(IDirect3DDevice8), &c);
				rec.hr(r);
				rec.object(r == D3D_OK ? c : nullptr);
				if (r == D3D_OK && c)
					rec.mix(static_cast<IUnknown*>(c)->Release());
			}
		for (IDirect3DSurface8* s : { rt, ds, b0, b0b })
			if (s)
				rec.mix(s->Release());
		rec.end();
	}
	{
		rec.begin("Scene+misc");
		rec.hr(dev->TestCooperativeLevel());
		rec.hr(dev->BeginScene());
		rec.hr(dev->BeginScene());
		rec.hr(dev->EndScene());
		rec.hr(dev->EndScene());
		rec.mix(uint64_t(dev->ShowCursor(TRUE)));
		rec.mix(uint64_t(dev->ShowCursor(FALSE)));
		D3DGAMMARAMP g;
		poison(&g);
		dev->GetGammaRamp(&g);
		rec.bytes(&g, sizeof(g));
		DWORD passes = 0xa5;
		rec.hr(dev->ValidateDevice(&passes));
		rec.mix(passes);
		D3DCAPS8 caps;
		poison(&caps);
		rec.hr(dev->GetDeviceCaps(&caps));
		rec.bytes(&caps, sizeof(caps));
		D3DDISPLAYMODE dm;
		poison(&dm);
		rec.hr(dev->GetDisplayMode(&dm));
		rec.bytes(&dm, sizeof(dm));
		D3DDEVICE_CREATION_PARAMETERS cp;
		poison(&cp);
		rec.hr(dev->GetCreationParameters(&cp));
		rec.bytes(&cp, sizeof(cp));
		rec.mix(dev->GetAvailableTextureMem());
		rec.hr(dev->ResourceManagerDiscardBytes(0));
		D3DRASTER_STATUS rs;
		rec.hr(dev->GetRasterStatus(&rs));
		IDirect3DSwapChain8* sc = reinterpret_cast<IDirect3DSwapChain8*>(uintptr_t(0x1));
		rec.hr(dev->CreateAdditionalSwapChain(&pp, &sc));
		rec.object(sc);
		rec.hr(dev->GetFrontBuffer(nullptr));
		DWORD tok = 0;
		rec.hr(dev->BeginStateBlock());	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		rec.hr(dev->EndStateBlock(&tok));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		rec.hr(dev->CreateStateBlock(D3DSBT_ALL, &tok));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		rec.hr(dev->GetInfo(0, nullptr, 0));
		rec.hr(dev->SetPaletteEntries(0, nullptr));
		rec.hr(dev->GetVertexShaderConstant(0, nullptr, 1));
		rec.hr(dev->SetClipPlane(0, nullptr));
		rec.hr(dev->DeleteVertexShader(vsh));
		rec.hr(dev->DeleteVertexShader(vsh));
		rec.hr(dev->DeletePixelShader(12345));
		rec.end();
	}
	// --- resources
	if (tex) {
		rec.begin("Texture");
		rec.mix(tex->GetLevelCount());
		for (UINT l = 0; l <= tex->GetLevelCount(); ++l) {
			D3DSURFACE_DESC sd;
			poison(&sd);
			rec.hr(tex->GetLevelDesc(l, &sd));
			rec.bytes(&sd, sizeof(sd));
		}
		IDirect3DSurface8 *s0 = nullptr, *s0b = nullptr, *sOut = nullptr;
		rec.hr(tex->GetSurfaceLevel(0, &s0));
		rec.object(s0);
		rec.hr(tex->GetSurfaceLevel(0, &s0b));
		rec.object(s0b);
		rec.hr(tex->GetSurfaceLevel(99, &sOut));
		rec.object(sOut);
		if (s0) {
			void* c = nullptr;
			const HRESULT r = s0->GetContainer(__uuidof(IDirect3DTexture8), &c);
			rec.hr(r);
			rec.object(c);
			if (r == D3D_OK && c)
				rec.mix(static_cast<IUnknown*>(c)->Release());
			rec.mix(s0->Release());
		}
		if (s0b)
			rec.mix(s0b->Release());
		rec.mix(tex->SetPriority(7));
		rec.mix(tex->GetPriority());
		rec.mix(tex->SetLOD(9));
		rec.mix(tex->GetLOD());
		rec.mix(tex->SetLOD(1));
		rec.mix(tex->GetLOD());
		rec.mix(uint64_t(tex->GetType()));
		rec.hr(tex->GetPrivateData(IID_GxNative, nullptr, nullptr));
		rec.hr(tex->FreePrivateData(IID_GxNative));
		rec.mix(tex->AddRef());
		rec.mix(tex->Release());
		D3DLOCKED_RECT lr;
		poison(&lr);
		const RECT r = { 4, 4, 8, 8 };
		const HRESULT hl = tex->LockRect(0, &lr, &r, D3DLOCK_READONLY);
		rec.hr(hl);
		if (hl == D3D_OK) {
			rec.lockPtr(1, lr.pBits, size_t(4) * lr.Pitch + 16);
			rec.mix(uint64_t(lr.Pitch));
			rec.hr(tex->UnlockRect(0));
		}
		rec.hr(tex->LockRect(99, &lr, nullptr, 0));
		rec.hr(tex->LockRect(0, nullptr, nullptr, 0));
		rec.hr(tex->UnlockRect(99));
		rec.hr(tex->AddDirtyRect(nullptr));
		IDirect3DDevice8* gd = nullptr;
		rec.hr(tex->GetDevice(&gd));
		rec.object(gd);
		if (gd)
			rec.mix(gd->Release());
		rec.end();
	}
	auto bufferCalls = [&](const char* name, int id, IDirect3DVertexBuffer8* vb, DWORD flags) {
		if (!vb)
			return;
		rec.begin(name);
		D3DVERTEXBUFFER_DESC bd;
		poison(&bd);
		rec.hr(vb->GetDesc(&bd));
		rec.bytes(&bd, sizeof(bd));
		rec.hr(vb->GetDesc(nullptr));
		rec.mix(uint64_t(vb->GetType()));
		rec.mix(vb->SetPriority(3));
		rec.mix(vb->GetPriority());
		for (UINT off : { 0u, 64u, 256u }) {
			BYTE* p = nullptr;
			const HRESULT r = vb->Lock(off, 64, &p, flags);
			rec.hr(r);
			if (r == D3D_OK) {
				rec.lockPtr(id, p, off);
				rec.hr(vb->Unlock());
			}
		}
		BYTE* p = nullptr;
		rec.hr(vb->Lock(2000, 64, &p, flags));   // out of range
		rec.hr(vb->Lock(0, 0, nullptr, flags));
		rec.end();
	};
	bufferCalls("VB-dynamic-nooverwrite", 2, vbd, D3DLOCK_NOOVERWRITE);
	bufferCalls("VB-dynamic-readonly", 2, vbd, D3DLOCK_READONLY);
	bufferCalls("VB-static", 3, vbs, D3DLOCK_READONLY);
	if (ib) {
		rec.begin("IB");
		D3DINDEXBUFFER_DESC bd;
		poison(&bd);
		rec.hr(ib->GetDesc(&bd));
		rec.bytes(&bd, sizeof(bd));
		rec.end();
	}
	if (img) {
		rec.begin("ImageSurface");
		D3DSURFACE_DESC sd;
		poison(&sd);
		rec.hr(img->GetDesc(&sd));
		rec.bytes(&sd, sizeof(sd));
		D3DLOCKED_RECT lr;
		poison(&lr);
		const HRESULT r = img->LockRect(&lr, nullptr, D3DLOCK_READONLY);
		rec.hr(r);
		if (r == D3D_OK) {
			rec.lockPtr(4, lr.pBits, 0);
			rec.mix(uint64_t(lr.Pitch));
			rec.hr(img->UnlockRect());
		}
		rec.hr(img->LockRect(nullptr, nullptr, 0));
		rec.end();
	}
	// never-called counter and its one log line are part of the result
	rec.begin("stats");
	rec.mix(nd->stats().neverCalled);
	rec.mix(nd->stats().creates);
	rec.end();
	dev->SetTexture(2, nullptr);
	dev->SetStreamSource(1, nullptr, 0);
	dev->SetIndices(nullptr, 0);
	for (IUnknown* u : std::vector<IUnknown*> { tex, vbd, vbs, ib, img })
		if (u)
			u->Release();
	dev->Release();
	if (rec.dump)
		std::fclose(rec.dump);
	std::printf("FRONTDIGEST %s %016llx calls=%llu\n", label, (unsigned long long)rec.h, (unsigned long long)rec.calls);
	return 0;
}
