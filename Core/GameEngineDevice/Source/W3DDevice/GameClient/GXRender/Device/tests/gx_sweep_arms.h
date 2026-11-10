// GeneralsX: A23-B merge gate item 2: the STRICT VALUE SWEEP (A23-B amendment 5 (4)).
// TEST-ONLY. Every IDirect3DDevice8 entry point with NON-default argument values - the input list is review
// rw3x-a23b-code-r10-oracle's "Separate checklist", taken whole - each followed by what depends on it (a draw, the whole back
// buffer read back), and every result (HRESULT, out-parameter, pixels) handed to `obs`. Only deterministic results are observed
// (no pointers, window handles, raster position or free memory). Self-contained on the d3d8 interface: compiled into the A23-B
// gx_native_test (obs = --strict's digest, CS=1 must equal CS=0) AND into a base (8244ef7bd) gx_native_test (obs = nothing: the
// A22 tap of the same call sequence carries rule (a), base == CS0).
#pragma once

#include "GxCustomCatalog.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// NOT SWEPT, by family and why (census of the r10/r11 checklist; each is named here, not silently absent):
// * Combinatorial families are SAMPLED, not exhausted: render-state wrap x material-source combinations; texture-stage per-axis
//   address / filter / argument-selector-modifier combinations; colour x depth x sample-count target pairings; texture / buffer /
//   target / depth / image usage x size x format boundaries beyond the rows below; Reset interval x swap-effect combinations. Each
//   family's members are forwarded by the same entry point the sampled members exercise (trin + EXEC + obs see the values).
// * Draws whose start / count / index arithmetic reaches past their buffers: undefined in D3D8 and in Vulkan (a read out of range) -
//   not a CS0-vs-CS1 question; DrawIndexedPrimitive / SetIndices extremes are swept without a draw that would read them.
// * DrawPrimitiveUP with count > 0 and null data, DrawIndexedPrimitiveUP with count > 0 and null pointers or a zero stride: today's
//   code records a draw with no vertex buffer (VUID-vkCmdDraw-None-04007, base and head) - pre-existing, named in the handback.
// * Declaration-driven multi-stream layouts and further vertex-shader declarations: the catalogue ports one declaration per program;
//   every catalogue program is created (res) and Trees.vso is drawn.
// * Stream stride vs the FVF's attribute extent (a stride below it: the translator refuses by name - skipStrideCheck's own arm in
//   gx_translate_test), CopyRects block-alignment / rectangle-arithmetic boundaries (DXT rectangles off the 4x4 grid: refused today
//   as DXVK does; arithmetic past the surfaces: covered by the out / far / negative rectangles below), successful UpdateTexture of
//   other formats / mip chains (one A8R8G8B8 3-level case below; the DXT1 device arms cover the block path), further vertex-buffer
//   FVF boundaries and FVF <-> shader binding transitions (sampled: XYZB2, PSIZE, TEX4 with sizes; FVF -> VS -> FVF and stale
//   handles below): sampled families, same entry points.
// * Shader token-length boundaries other than the oversize S path (gx_native_test strict-shader-oversize): a stream without its END
//   token makes funcLen read past the caller's memory in today's code as in DXVK's - not a value a caller can pass safely.
// * GetDisplayMode(nullptr): forwarded unchanged to the IDirect3D8 (the test stub dereferences it). GetRasterStatus's output,
//   creation-parameters' focus window and the parent's address: timing / process-dependent - only their HRESULTs and identity
//   relationships are observed.
namespace gxsweep {

using Obs = void (*)(const void*, size_t);
struct S;
// What a binary hands the sweep: its --strict obs sink (null in the base binary) and a device-counter observer called at every
// read-back (the device's own draw / clear / creation accounting). Every observation also folds into a
// file digest when GX_SWEEP_OBS=<file> is set - one line per arm - so gates2 compares the base binary's outputs with CS0's
// (r11-oracle P2-1: a front-description error shared by CS0 and CS1 moves neither digest nor tap).
struct Hooks
{
	Obs obs = nullptr;
	void (*counters)(void* ctx, S& s) = nullptr;
	void* ctx = nullptr;
};

struct S
{
	IDirect3DDevice8* dev = nullptr;
	Hooks h;
	const char* name = "";
	uint64_t fh = 1469598103934665603ull, fn = 0;   // the GX_SWEEP_OBS digest
	std::string* d = nullptr;
	bool ok = true;
	int fails = 0, readbacks = 0, draws = 0;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* img = nullptr;   // the read-back surface (the back buffer's size and format)
	FILE* trace = nullptr;   // GX_SWEEP_OBS_TRACE=<file>: one line per observation (localising a base vs CS0 difference)
	void raw(const void* p, size_t n)
	{
		if (h.obs)
			h.obs(p, n);
		if (trace) {
			std::fprintf(trace, "rb=%d fn=%llu n=%zu", readbacks, (unsigned long long)fn, n);
			for (size_t i = 0; i < n && i < 16; ++i)
				std::fprintf(trace, "%s%02x", i ? "" : " ", static_cast<const uint8_t*>(p)[i]);
			std::fprintf(trace, "\n");
		}
		for (int i = 0; i < 8; ++i) {
			fh ^= (uint64_t(n) >> (8 * i)) & 0xff;
			fh *= 1099511628211ull;
		}
		for (size_t i = 0; i < n; ++i) {
			fh ^= static_cast<const uint8_t*>(p)[i];
			fh *= 1099511628211ull;
		}
		++fn;
	}
	template <class T>
	void o(const T& v) { raw(&v, sizeof(v)); }
	void check(bool c, const std::string& what)
	{
		if (c)
			return;
		ok = false;
		if (fails++ < 12)
			*d += " FAIL: " + what + ";";
	}
	void dropImg()
	{
		if (img)
			img->Release();
		img = nullptr;
	}
	bool getBB()
	{
		if (bb)
			bb->Release();
		bb = nullptr;
		dropImg();
		return SUCCEEDED(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb;
	}
	void end()
	{
		dropImg();
		if (bb)
			bb->Release();
		bb = nullptr;
		*d += " readbacks " + std::to_string(readbacks) + ", draws " + std::to_string(draws) + ";";
		if (trace)
			std::fclose(trace);
		trace = nullptr;
		if (const char* f = std::getenv("GX_SWEEP_OBS"))
			if (FILE* fp = std::fopen(f, "a")) {
				std::fprintf(fp, "%s %016llx %llu\n", name, (unsigned long long)fh, (unsigned long long)fn);
				std::fclose(fp);
			}
	}
};
inline void begin(S& s, IDirect3DDevice8* dev, const Hooks& h, std::string* d, const char* name)
{
	s.dev = dev;
	s.h = h;
	s.d = d;
	s.name = name;
	if (const char* f = std::getenv("GX_SWEEP_OBS_TRACE"))
		s.trace = std::fopen((std::string(f) + name + ".txt").c_str(), "w");
}

inline D3DRENDERSTATETYPE RS(uint32_t n) { return static_cast<D3DRENDERSTATETYPE>(n); }
inline D3DTEXTURESTAGESTATETYPE TSS(uint32_t n) { return static_cast<D3DTEXTURESTAGESTATETYPE>(n); }
inline D3DTRANSFORMSTATETYPE TS(uint32_t n) { return static_cast<D3DTRANSFORMSTATETYPE>(n); }
inline DWORD F(float f)
{
	DWORD u;
	std::memcpy(&u, &f, 4);
	return u;
}

// the whole back buffer, read back through the game's path, into obs
// the whole back buffer, read back through the game's path, into obs - every HRESULT observed - then the device counters
inline bool readBack(S& s)
{
	D3DSURFACE_DESC sd = {};
	HRESULT h = s.bb ? s.bb->GetDesc(&sd) : E_FAIL;
	s.o(h);
	bool ok = SUCCEEDED(h);
	if (ok && !s.img) {
		h = s.dev->CreateImageSurface(sd.Width, sd.Height, sd.Format, &s.img);
		s.o(h);
		ok = SUCCEEDED(h) && s.img;
	}
	if (ok) {
		h = s.dev->CopyRects(s.bb, nullptr, 0, s.img, nullptr);
		s.o(h);
		ok = SUCCEEDED(h);
	}
	D3DLOCKED_RECT lr = {};
	if (ok) {
		h = s.img->LockRect(&lr, nullptr, D3DLOCK_READONLY);
		s.o(h);
		ok = SUCCEEDED(h);
	}
	if (ok) {
		for (UINT y = 0; y < sd.Height; ++y)
			s.raw(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(sd.Width) * 4);
		s.o(s.img->UnlockRect());
	}
	++s.readbacks;
	s.check(ok, "read-back " + std::to_string(s.readbacks));
	if (s.h.counters)
		s.h.counters(s.h.ctx, s);
	return ok;
}

// pre-transformed, diffuse, two coordinate sets (the second 0..1, the first -0.5..1.5: address modes matter)
struct TV { float x, y, z, rhw; DWORD c; float u0, v0, u1, v1; };
const DWORD kTV = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2;
struct CV { float x, y, z, rhw; DWORD c; };
const DWORD kCV = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

inline void drawQuad(S& s, float z = 0.5f)
{
	const TV q[4] = { { 2, 3, z, 1, 0xff2040c0, -0.5f, -0.5f, 0, 0 }, { 61, 1, z, 1, 0x80c04020, 1.5f, -0.5f, 1, 0 },
		{ 1, 62, z, 1, 0x40ff8000, -0.5f, 1.5f, 0, 1 }, { 63, 60, z, 1, 0xc000ff80, 1.5f, 1.5f, 1, 1 } };
	s.o(s.dev->BeginScene());
	s.o(s.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV)));
	s.o(s.dev->EndScene());
	++s.draws;
}

inline void clearAll(S& s, D3DCOLOR c, float z = 1.0f, DWORD st = 0x5a)
{
	s.o(s.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, c, z, st));
}

// bytes per block row for a level (block-compressed: 4x4 blocks)
inline void levelLayout(D3DFORMAT f, UINT w, UINT h, UINT* rowBytes, UINT* rows)
{
	switch (f) {
	case D3DFMT_DXT1: *rowBytes = ((w + 3) / 4) * 8; *rows = (h + 3) / 4; return;
	case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *rowBytes = ((w + 3) / 4) * 16; *rows = (h + 3) / 4; return;
	case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_X4R4G4B4:
		*rowBytes = w * 2; *rows = h; return;
	case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: *rowBytes = w; *rows = h; return;
	default: *rowBytes = w * 4; *rows = h; return;
	}
}
inline void fillRows(const D3DLOCKED_RECT& lr, UINT rowBytes, UINT rows, uint32_t seed)
{
	for (UINT y = 0; y < rows; ++y) {
		uint8_t* p = static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
		for (UINT x = 0; x < rowBytes; ++x)
			p[x] = uint8_t((x * 37 + y * 101 + seed * 53 + ((x ^ y) & 7) * 11) & 0xff);
	}
}
// every field of a surface description (r11-oracle P2-2)
inline void obsDesc(S& s, const D3DSURFACE_DESC& sd)
{
	s.o(sd.Format);
	s.o(sd.Type);
	s.o(sd.Usage);
	s.o(sd.Pool);
	s.o(sd.Size);
	s.o(sd.MultiSampleType);
	s.o(sd.Width);
	s.o(sd.Height);
}
// A creation's result: the HRESULT, and its output pointer - handed in POISONED, so a failure must clear it (r11-oracle P2-2).
// true = a usable object in p; a failure leaves p null.
template <class T>
T* poison() { return reinterpret_cast<T*>(uintptr_t(0x10)); }
template <class T>
bool created(S& s, HRESULT h, T*& p, const char* what)
{
	s.o(h);
	const bool kept = p == poison<T>();
	s.o(kept);
	s.o(p == nullptr);
	if (FAILED(h)) {   // observed, not asserted: base vs CS0 (amendment 5 (8)) judges it - today some refusals return before clearing
		p = nullptr;   // (CreateRenderTarget / CreateDepthStencilSurface with an invalid MSAA type: pre-existing, named in the handback)
		return false;
	}
	s.check(p && !kept, std::string(what) + " answered its object");
	if (kept)
		p = nullptr;
	return p != nullptr;
}
// a texture with every level written (MANAGED unless asked), or null; every result observed, the requested format / usage / pool
// asserted on each level (r11-oracle P2-1). *defined = every level was written (only such a texture may be sampled: P2-4).
inline IDirect3DTexture8* makeTex(S& s, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, uint32_t seed,
	bool* defined = nullptr)
{
	IDirect3DTexture8* t = poison<IDirect3DTexture8>();
	if (defined)
		*defined = false;
	if (!created(s, s.dev->CreateTexture(w, h, levels, usage, f, pool, &t), t, "CreateTexture"))
		return nullptr;
	const DWORD n = t->GetLevelCount();
	s.o(n);
	bool all = true;
	for (DWORD l = 0; l < n; ++l) {
		D3DSURFACE_DESC sd = {};
		s.o(t->GetLevelDesc(l, &sd));
		obsDesc(s, sd);
		s.check(sd.Format == f && sd.Usage == usage && sd.Pool == pool, "texture level " + std::to_string(l) + " describes the format / usage / pool requested");
		if (pool == D3DPOOL_DEFAULT && !(usage & D3DUSAGE_DYNAMIC)) {
			all = false;
			continue;   // not lockable
		}
		D3DLOCKED_RECT lr = {};
		const HRESULT hl = t->LockRect(l, &lr, nullptr, 0);
		s.o(hl);
		if (SUCCEEDED(hl)) {
			UINT rb = 0, rows = 0;
			levelLayout(sd.Format, sd.Width, sd.Height, &rb, &rows);
			fillRows(lr, rb, rows, seed + l);
			s.o(t->UnlockRect(l));
		} else
			all = false;
	}
	if (defined)
		*defined = all;
	return t;
}

// fixed function over one textured stage, no depth, no blend, no lighting
inline void ffBase(S& s, IDirect3DBaseTexture8* tex0)
{
	IDirect3DDevice8* d = s.dev;
	d->SetRenderState(D3DRS_ZENABLE, FALSE);
	d->SetRenderState(D3DRS_LIGHTING, FALSE);
	d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	d->SetRenderState(D3DRS_FOGENABLE, FALSE);
	d->SetVertexShader(kTV);
	d->SetTexture(0, tex0);
	d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
	d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}

struct KV { uint32_t k; DWORD v; };

// one render-state group: the defaults read, the values set (getter = value), clear + quad + read-back, the defaults back
inline void rsGroupN(S& s, const KV* gb, size_t gn, bool points = false)
{
	struct Range { const KV *b, *e; const KV* begin() const { return b; } const KV* end() const { return e; } } g { gb, gb + gn };
	std::vector<DWORD> old;
	for (const KV& kv : g) {
		DWORD v = 0xdeadbeefu;
		s.o(s.dev->GetRenderState(RS(kv.k), &v));
		s.o(v);
		old.push_back(v);
	}
	for (const KV& kv : g) {
		s.o(s.dev->SetRenderState(RS(kv.k), kv.v));
		DWORD v = 0xdeadbeefu;
		const HRESULT h = s.dev->GetRenderState(RS(kv.k), &v);
		s.o(h);
		s.o(v);
		if (kv.k < 256 && kv.k)
			s.check(SUCCEEDED(h) && v == kv.v, "GetRenderState(" + std::to_string(kv.k) + ") answers the value set");
	}
	clearAll(s, 0xff304050);
	drawQuad(s);
	if (points) {
		const CV p[4] = { { 10, 10, 0.5f, 1, 0xffff0000 }, { 50, 12, 0.5f, 1, 0xff00ff00 }, { 12, 50, 0.5f, 1, 0xff0000ff }, { 40, 40, 0.5f, 1, 0xffffffff } };
		s.dev->SetVertexShader(kCV);
		s.o(s.dev->BeginScene());
		s.o(s.dev->DrawPrimitiveUP(D3DPT_POINTLIST, 4, p, sizeof(CV)));
		s.o(s.dev->EndScene());
		s.dev->SetVertexShader(kTV);
		++s.draws;
	}
	readBack(s);
	size_t i = 0;
	for (const KV& kv : g)
		s.o(s.dev->SetRenderState(RS(kv.k), old[i++]));
}
inline void rsGroup(S& s, std::initializer_list<KV> g, bool points = false) { rsGroupN(s, g.begin(), g.size(), points); }

// one texture-stage group on `stage`, as rsGroup
inline void tssGroup(S& s, DWORD stage, std::initializer_list<KV> g)
{
	std::vector<DWORD> old;
	for (const KV& kv : g) {
		DWORD v = 0xdeadbeefu;
		s.o(s.dev->GetTextureStageState(stage, TSS(kv.k), &v));
		s.o(v);
		old.push_back(v);
	}
	for (const KV& kv : g) {
		s.o(s.dev->SetTextureStageState(stage, TSS(kv.k), kv.v));
		DWORD v = 0xdeadbeefu;
		const HRESULT h = s.dev->GetTextureStageState(stage, TSS(kv.k), &v);
		s.o(h);
		s.o(v);
		s.check(SUCCEEDED(h) && v == kv.v, "GetTextureStageState(" + std::to_string(stage) + "," + std::to_string(kv.k) + ") answers the value set");
	}
	clearAll(s, 0xff102030);
	drawQuad(s);
	readBack(s);
	size_t i = 0;
	for (const KV& kv : g)
		s.o(s.dev->SetTextureStageState(stage, TSS(kv.k), old[i++]));
}

inline D3DMATRIX mat(float a, float b)
{
	D3DMATRIX m;
	for (int i = 0; i < 16; ++i)
		(&m._11)[i] = a * float(i + 1) + ((i % 5) == 0 ? b : 0.0f);
	return m;
}

// ================================================================== arm strict-sweep-state
// transforms (every type incl. TEXTURE0-7 and unmodelled ones), MultiplyTransform on non-WORLD types, every render state with a
// non-default value (dependent groups together), every texture-stage type on stages 0-7 (+ the invalid stage 8), textures on every
// stage, lights 1-3 POINT / SPOT / DIRECTIONAL with every member set, a never-set light enabled, clip planes.
inline bool sweepState(IDirect3DDevice8* dev, const Hooks& hk, std::string* d)
{
	S s;
	begin(s, dev, hk, d, "strict-sweep-state");
	if (!s.getBB()) {
		*d += " SETUP: no back buffer";
		return false;
	}
	// ---- transforms
	for (uint32_t t = D3DTS_TEXTURE0; t <= D3DTS_TEXTURE7; ++t) {
		D3DMATRIX m = mat(0.05f, 1.0f);
		m._41 = 0.01f * float(t);
		s.o(dev->SetTransform(TS(t), &m));
		D3DMATRIX g;
		std::memset(&g, 0x5a, sizeof(g));
		const HRESULT h = dev->GetTransform(TS(t), &g);
		s.o(h);
		s.o(g);
		s.check(SUCCEEDED(h) && std::memcmp(&g, &m, sizeof(m)) == 0, "GetTransform(TEXTURE" + std::to_string(t - D3DTS_TEXTURE0) + ") round trip");
	}
	for (uint32_t t : { 0u, 1u, 4u, 15u, 24u, 255u, 257u, 258u, 511u, 512u }) {   // invalid / unmodelled (WORLDMATRIX(1..))
		const D3DMATRIX m = mat(0.25f, -2.0f);
		s.o(dev->SetTransform(TS(t), &m));
		D3DMATRIX g;
		std::memset(&g, 0x5a, sizeof(g));
		s.o(dev->GetTransform(TS(t), &g));
		s.o(g);
	}
	for (uint32_t t : { uint32_t(D3DTS_WORLD), uint32_t(D3DTS_VIEW), uint32_t(D3DTS_PROJECTION), uint32_t(D3DTS_TEXTURE0), uint32_t(D3DTS_TEXTURE1),
			 uint32_t(D3DTS_TEXTURE2), uint32_t(D3DTS_TEXTURE3), uint32_t(D3DTS_TEXTURE4), uint32_t(D3DTS_TEXTURE5), uint32_t(D3DTS_TEXTURE6),
			 uint32_t(D3DTS_TEXTURE7), 257u }) {
		const D3DMATRIX a = mat(0.125f, 1.0f), b = mat(-0.0625f, 0.5f);
		s.o(dev->SetTransform(TS(t), &a));
		s.o(dev->MultiplyTransform(TS(t), &b));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		D3DMATRIX g;
		std::memset(&g, 0x5a, sizeof(g));
		s.o(dev->GetTransform(TS(t), &g));
		s.o(g);
	}
	D3DMATRIX id = {};
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	dev->SetTransform(D3DTS_PROJECTION, &id);

	// ---- textures: eight, mipmapped, distinct; one per stage
	IDirect3DTexture8* tex[8] = {};
	for (int i = 0; i < 8; ++i)
		tex[i] = makeTex(s, 16, 16, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, 7u + uint32_t(i) * 3u);
	ffBase(s, tex[0]);
	clearAll(s, 0xff000000);
	drawQuad(s);
	readBack(s);

	// ---- render states (D3D8 numbers; groups whose states only act together)
	rsGroup(s, { { 8, 2 } });                                        // FILLMODE wireframe
	rsGroup(s, { { 8, 1 } }, true);                                  // FILLMODE point
	rsGroup(s, { { 9, 1 } });                                        // SHADEMODE flat
	rsGroup(s, { { 9, 3 } });                                        // SHADEMODE phong
	rsGroup(s, { { 10, 0x00010003 } });                              // LINEPATTERN
	rsGroup(s, { { 7, 1 }, { 14, 0 }, { 23, 5 } });                 // ZENABLE, ZWRITEENABLE off, ZFUNC GREATER
	rsGroup(s, { { 7, 1 }, { 23, 2 }, { 47, 8 } });                  // ZFUNC LESS, ZBIAS 8
	rsGroup(s, { { 7, 1 }, { 23, 8 }, { 47, 15 } });                 // ZFUNC ALWAYS, ZBIAS 15
	rsGroup(s, { { 7, 2 } });                                        // ZENABLE USEW
	rsGroup(s, { { 15, 1 }, { 24, 0x80 }, { 25, 5 } });              // ALPHATEST GREATER 0x80
	rsGroup(s, { { 15, 1 }, { 24, 0x40 }, { 25, 2 } });              // ALPHATEST LESS 0x40
	rsGroup(s, { { 15, 1 }, { 24, 0xff }, { 25, 7 } });              // ALPHATEST NOTEQUAL 0xff
	rsGroup(s, { { 16, 0 } });                                       // LASTPIXEL off
	rsGroup(s, { { 27, 1 }, { 19, 5 }, { 20, 6 } });                 // blend SRCALPHA / INVSRCALPHA
	rsGroup(s, { { 27, 1 }, { 19, 12 } });                           // BOTHSRCALPHA
	rsGroup(s, { { 27, 1 }, { 19, 13 } });                           // BOTHINVSRCALPHA
	rsGroup(s, { { 27, 1 }, { 19, 9 }, { 20, 3 }, { 171, 2 } });     // DESTCOLOR, SRCCOLOR, SUBTRACT
	rsGroup(s, { { 27, 1 }, { 19, 2 }, { 20, 2 }, { 171, 4 } });     // ONE, ONE, MIN
	rsGroup(s, { { 27, 1 }, { 19, 7 }, { 20, 8 }, { 171, 3 } });     // DESTALPHA, INVDESTALPHA, REVSUBTRACT
	rsGroup(s, { { 27, 1 }, { 19, 11 }, { 20, 10 }, { 171, 5 } });   // SRCALPHASAT, INVDESTCOLOR, MAX
	rsGroup(s, { { 27, 1 }, { 19, 4 }, { 20, 1 } });                 // INVSRCCOLOR, ZERO
	rsGroup(s, { { 22, 2 } });                                       // CULLMODE CW
	rsGroup(s, { { 22, 3 } });                                       // CULLMODE CCW
	rsGroup(s, { { 26, 1 } });                                       // DITHERENABLE
	rsGroup(s, { { 28, 1 }, { 34, 0x00ff8040 }, { 35, 3 }, { 36, F(0.2f) }, { 37, F(0.8f) } });   // table fog LINEAR
	rsGroup(s, { { 28, 1 }, { 34, 0x000080ff }, { 35, 1 }, { 38, F(1.5f) } });                  // table fog EXP
	rsGroup(s, { { 28, 1 }, { 35, 2 }, { 38, F(0.7f) } });                                      // table fog EXP2
	rsGroup(s, { { 28, 1 }, { 35, 0 }, { 140, 3 }, { 48, 1 }, { 36, F(0.0f) }, { 37, F(1.0f) } });   // vertex fog, range fog
	rsGroup(s, { { 29, 1 } });                                       // SPECULARENABLE
	rsGroup(s, { { 30, 1 } });                                       // ZVISIBLE
	rsGroup(s, { { 40, 1 } });                                       // EDGEANTIALIAS
	rsGroup(s, { { 52, 1 }, { 56, 3 }, { 57, 0x5a }, { 58, 0xf0 }, { 59, 0x0f }, { 55, 7 }, { 53, 2 }, { 54, 6 } });   // stencil EQUAL
	rsGroup(s, { { 52, 1 }, { 56, 1 }, { 53, 3 }, { 57, 0x33 } });   // stencil NEVER, fail REPLACE
	rsGroup(s, { { 52, 1 }, { 56, 6 }, { 57, 0x10 }, { 55, 4 }, { 54, 5 } });   // stencil NOTEQUAL, INCRSAT / DECRSAT
	rsGroup(s, { { 52, 1 }, { 56, 4 }, { 57, 0x80 }, { 55, 8 }, { 58, 0x7f } });   // LESSEQUAL, DECR, mask
	rsGroup(s, { { 60, 0x80402010 } });                              // TEXTUREFACTOR
	rsGroup(s, { { 128, 1 }, { 129, 3 }, { 130, 2 }, { 135, 4 } });  // WRAP0/1/2/7
	rsGroup(s, { { 136, 0 } });                                      // CLIPPING off
	rsGroup(s, { { 137, 1 }, { 139, 0x00406080 } });                 // LIGHTING + AMBIENT on pre-transformed vertices
	rsGroup(s, { { 141, 0 }, { 142, 0 }, { 143, 1 } });              // COLORVERTEX off, LOCALVIEWER off, NORMALIZENORMALS
	rsGroup(s, { { 145, 1 }, { 146, 2 }, { 147, 1 }, { 148, 2 } });  // material sources
	rsGroup(s, { { 151, 1 } });                                      // VERTEXBLEND 1WEIGHTS
	rsGroup(s, { { 151, 0xff } });                                   // VERTEXBLEND TWEENING
	rsGroup(s, { { 152, 1 } });                                      // CLIPPLANEENABLE
	rsGroup(s, { { 153, 1 } });                                      // SOFTWAREVERTEXPROCESSING
	rsGroup(s, { { 154, F(4.0f) }, { 155, F(1.0f) }, { 156, 1 }, { 157, 1 }, { 158, F(0.0f) }, { 159, F(1.0f) }, { 160, F(0.0f) },
		{ 166, F(8.0f) } }, true);                                   // point size / sprites / scale
	rsGroup(s, { { 161, 0 }, { 162, 0x0 } });                        // MULTISAMPLEANTIALIAS off, MULTISAMPLEMASK 0
	rsGroup(s, { { 163, 1 }, { 164, F(2.0f) } });                    // patches
	rsGroup(s, { { 165, 1 } });                                      // DEBUGMONITORTOKEN
	rsGroup(s, { { 167, 1 } });                                      // INDEXEDVERTEXBLENDENABLE
	rsGroup(s, { { 168, 0x5 } });                                    // COLORWRITEENABLE R|B
	rsGroup(s, { { 168, 0x0 } });                                    // COLORWRITEENABLE none
	rsGroup(s, { { 170, F(0.5f) } });                                // TWEENFACTOR
	rsGroup(s, { { 172, 2 }, { 173, 1 } });                          // POSITIONORDER, NORMALORDER
	rsGroup(s, { { 6, 1 }, { 200, 7 }, { 255, 1 } });                // ids no state uses
	// r11-oracle remainder: every comparison for depth and alpha, WRAP3-6, vertex fog EXP / EXP2, the other vertex-blend modes
	for (DWORD f = 1; f <= 8; ++f) {
		rsGroup(s, { { 7, 1 }, { 23, f } });
		rsGroup(s, { { 15, 1 }, { 24, 0x60 }, { 25, f } });
	}
	rsGroup(s, { { 131, 1 }, { 132, 2 }, { 133, 4 }, { 134, 8 } });
	rsGroup(s, { { 28, 1 }, { 140, 1 }, { 38, F(0.9f) }, { 34, 0x00102030 } });
	rsGroup(s, { { 28, 1 }, { 140, 2 }, { 38, F(0.4f) } });
	rsGroup(s, { { 151, 2 } });
	rsGroup(s, { { 151, 3 } });
	// ... and EVERY raw slot 0-255, sixteen at a time, each at its default ^ 1 (the getter's value before)
	for (uint32_t b = 0; b < 256; b += 16) {
		KV g[16];
		for (uint32_t i = 0; i < 16; ++i) {
			DWORD v = 0;
			dev->GetRenderState(RS(b + i), &v);
			g[i] = { b + i, v ^ 1u };
		}
		rsGroupN(s, g, 16);
	}
	for (uint32_t k : { 0u, 256u, 1000u }) {                         // out of range
		s.o(dev->SetRenderState(RS(k), 3));
		DWORD v = 0xdeadbeefu;
		s.o(dev->GetRenderState(RS(k), &v));
		s.o(v);
	}

	// ---- stages: every stage bound and chained (MODULATE over 0..k), coordinate sets alternate
	for (DWORD st = 0; st < 8; ++st) {
		s.o(dev->SetTexture(st, tex[st]));
		IDirect3DBaseTexture8* g = nullptr;
		const HRESULT h = dev->GetTexture(st, &g);
		s.o(h);
		s.o(g == tex[st]);
		s.check(SUCCEEDED(h) && g == tex[st], "GetTexture(" + std::to_string(st) + ") answers the texture set");
		if (g)
			g->Release();
	}
	s.o(dev->SetTexture(8, tex[0]));   // invalid stage
	{
		IDirect3DBaseTexture8* g = nullptr;
		s.o(dev->GetTexture(8, &g));
		s.o(g != nullptr);
		if (g)
			g->Release();
	}
	for (DWORD k = 0; k < 8; ++k) {
		for (DWORD st = 0; st <= k; ++st) {
			dev->SetTextureStageState(st, D3DTSS_COLOROP, st == 0 ? D3DTOP_MODULATE : (st % 2 ? D3DTOP_ADDSIGNED : D3DTOP_MODULATE2X));
			dev->SetTextureStageState(st, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(st, D3DTSS_COLORARG2, st == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
			dev->SetTextureStageState(st, D3DTSS_ALPHAOP, st == 0 ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
			dev->SetTextureStageState(st, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(st, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(st, D3DTSS_TEXCOORDINDEX, st % 2);
		}
		if (k < 7)
			dev->SetTextureStageState(k + 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		clearAll(s, 0xff000000);
		drawQuad(s);
		readBack(s);
	}
	// every stage type outside the exercised set, on stages 0, 1, 5 and 7 (all eight chained)
	for (DWORD st = 0; st < 8; ++st) {   // every stage (r11-oracle remainder: 2/3/4/6 too)
		tssGroup(s, st, { { 4, D3DTOP_MODULATE }, { 5, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE }, { 6, D3DTA_DIFFUSE | D3DTA_COMPLEMENT } });
		tssGroup(s, st, { { 1, D3DTOP_MULTIPLYADD }, { 26, D3DTA_TFACTOR }, { 2, D3DTA_TEXTURE }, { 3, D3DTA_CURRENT }, { 28, D3DTA_TEMP } });
		tssGroup(s, st, { { 1, D3DTOP_LERP }, { 26, D3DTA_DIFFUSE | D3DTA_ALPHAREPLICATE }, { 4, D3DTOP_LERP }, { 27, D3DTA_TFACTOR } });
		tssGroup(s, st, { { 7, F(0.5f) }, { 8, F(-0.25f) }, { 9, F(0.25f) }, { 10, F(0.75f) }, { 22, F(2.0f) }, { 23, F(0.125f) },
			{ 1, D3DTOP_BUMPENVMAP } });
		tssGroup(s, st, { { 1, D3DTOP_BUMPENVMAPLUMINANCE } });
		tssGroup(s, st, { { 1, D3DTOP_DOTPRODUCT3 } });
		tssGroup(s, st, { { 13, D3DTADDRESS_CLAMP }, { 14, D3DTADDRESS_MIRROR }, { 25, D3DTADDRESS_BORDER }, { 15, 0xff00ff00 } });
		tssGroup(s, st, { { 13, D3DTADDRESS_BORDER }, { 14, D3DTADDRESS_MIRRORONCE }, { 15, 0x80ff0000 } });
		tssGroup(s, st, { { 19, F(1.5f) }, { 20, 2 }, { 21, 8 }, { 17, D3DTEXF_ANISOTROPIC }, { 16, D3DTEXF_ANISOTROPIC } });
		tssGroup(s, st, { { 17, D3DTEXF_POINT }, { 16, D3DTEXF_LINEAR }, { 18, D3DTEXF_POINT }, { 19, F(-1.0f) } });
		tssGroup(s, st, { { 17, D3DTEXF_FLATCUBIC }, { 16, D3DTEXF_GAUSSIANCUBIC }, { 18, D3DTEXF_NONE } });
		{
			D3DMATRIX m = id;
			m._11 = 2.0f;
			m._22 = 0.5f;
			m._31 = 0.25f;
			m._32 = -0.125f;
			s.o(dev->SetTransform(TS(D3DTS_TEXTURE0 + st), &m));
			tssGroup(s, st, { { 24, D3DTTFF_COUNT2 } });
			m._13 = 0.5f;
			m._33 = 2.0f;
			s.o(dev->SetTransform(TS(D3DTS_TEXTURE0 + st), &m));
			tssGroup(s, st, { { 24, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED } });
			m._14 = 0.25f;
			m._44 = 1.5f;
			s.o(dev->SetTransform(TS(D3DTS_TEXTURE0 + st), &m));
			tssGroup(s, st, { { 24, D3DTTFF_COUNT1 } });
			tssGroup(s, st, { { 24, D3DTTFF_COUNT4 } });
			tssGroup(s, st, { { 24, D3DTTFF_COUNT4 | D3DTTFF_PROJECTED } });
			tssGroup(s, st, { { 24, D3DTTFF_COUNT2 | D3DTTFF_PROJECTED } });
			s.o(dev->SetTransform(TS(D3DTS_TEXTURE0 + st), &id));
		}
		tssGroup(s, st, { { 11, D3DTSS_TCI_CAMERASPACENORMAL | 1 } });
		tssGroup(s, st, { { 11, D3DTSS_TCI_CAMERASPACEPOSITION } });
		tssGroup(s, st, { { 11, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR | 1 } });
		tssGroup(s, st, { { 1, D3DTOP_DISABLE } });   // the chain cut here
	}
	for (DWORD st : { 0u, 1u })   // every colour op on the first two stages
		for (DWORD op = D3DTOP_SELECTARG1; op <= D3DTOP_LERP; ++op)
			tssGroup(s, st, { { 1, op }, { 4, op <= D3DTOP_MODULATEINVCOLOR_ADDALPHA ? op : DWORD(D3DTOP_MODULATE) } });
	for (uint32_t k : { 0u, 12u, 29u, 30u, 31u, 32u }) {   // types no stage uses / out of range
		s.o(dev->SetTextureStageState(0, TSS(k), 5));
		DWORD v = 0xdeadbeefu;
		s.o(dev->GetTextureStageState(0, TSS(k), &v));
		s.o(v);
	}
	s.o(dev->SetTextureStageState(8, D3DTSS_COLOROP, D3DTOP_MODULATE));   // invalid stage
	{
		DWORD v = 0xdeadbeefu;
		s.o(dev->GetTextureStageState(8, D3DTSS_COLOROP, &v));
		s.o(v);
	}
	for (DWORD st = 1; st < 9; ++st)
		dev->SetTexture(st, nullptr);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

	// ---- lights: a lit grid, every light member set; POINT 1, SPOT 2, DIRECTIONAL 3, a never-set 5, a ninth index
	struct LV { float x, y, z, nx, ny, nz; DWORD c; };
	std::vector<LV> grid;
	for (int gy = 0; gy < 4; ++gy)
		for (int gx = 0; gx < 4; ++gx) {
			const float x0 = -1.0f + gx * 0.5f, y0 = -1.0f + gy * 0.5f, x1 = x0 + 0.5f, y1 = y0 + 0.5f;
			auto v = [&](float x, float y) {
				const float nx = x * 0.4f, ny = y * 0.3f;
				return LV { x, y, 0.5f, nx, ny, -1.0f, 0xff000000u | (uint32_t(gx * 60) << 16) | (uint32_t(gy * 60) << 8) | 0x80u };
			};
			const LV t[6] = { v(x0, y0), v(x1, y0), v(x0, y1), v(x0, y1), v(x1, y0), v(x1, y1) };
			grid.insert(grid.end(), t, t + 6);
		}
	D3DMATERIAL8 mtl = {};
	mtl.Diffuse = { 0.9f, 0.8f, 0.7f, 1.0f };
	mtl.Ambient = { 0.2f, 0.3f, 0.4f, 1.0f };
	mtl.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
	mtl.Emissive = { 0.05f, 0.0f, 0.1f, 0.0f };
	mtl.Power = 12.0f;
	s.o(dev->SetMaterial(&mtl));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	{
		D3DMATERIAL8 g;
		std::memset(&g, 0x5a, sizeof(g));
		s.o(dev->GetMaterial(&g));
		s.o(g);
	}
	dev->SetTexture(0, nullptr);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
	dev->SetRenderState(D3DRS_LIGHTING, TRUE);
	dev->SetRenderState(D3DRS_AMBIENT, 0x00202020);
	dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
	dev->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
	dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
	auto drawGrid = [&] {
		clearAll(s, 0xff000000);
		s.o(dev->BeginScene());
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(grid.size() / 3), grid.data(), sizeof(LV)));
		s.o(dev->EndScene());
		++s.draws;
		readBack(s);
	};
	D3DLIGHT8 l1 = {}, l2 = {}, l3 = {};
	l1.Type = D3DLIGHT_POINT;
	l1.Diffuse = { 1.0f, 0.5f, 0.25f, 1.0f };
	l1.Specular = { 0.5f, 0.5f, 1.0f, 1.0f };
	l1.Ambient = { 0.1f, 0.1f, 0.1f, 1.0f };
	l1.Position = { 0.0f, 0.0f, -0.5f };
	l1.Range = 3.0f;
	l1.Falloff = 1.0f;
	l1.Attenuation0 = 0.5f;
	l1.Attenuation1 = 0.25f;
	l1.Attenuation2 = 0.125f;
	l2.Type = D3DLIGHT_SPOT;
	l2.Diffuse = { 0.2f, 1.0f, 0.3f, 1.0f };
	l2.Specular = { 0.3f, 0.3f, 0.3f, 1.0f };
	l2.Position = { 0.5f, 0.5f, -1.0f };
	l2.Direction = { -0.2f, -0.2f, 1.0f };
	l2.Range = 5.0f;
	l2.Falloff = 1.5f;
	l2.Attenuation0 = 1.0f;
	l2.Theta = 0.3f;
	l2.Phi = 0.9f;
	l3.Type = D3DLIGHT_DIRECTIONAL;
	l3.Diffuse = { 0.1f, 0.2f, 0.9f, 1.0f };
	l3.Ambient = { 0.05f, 0.05f, 0.0f, 1.0f };
	l3.Direction = { 0.3f, -0.4f, 0.8f };
	s.o(dev->SetLight(1, &l1));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	s.o(dev->SetLight(2, &l2));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	s.o(dev->SetLight(3, &l3));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	for (DWORD i : { 1u, 2u, 3u })
		s.o(dev->LightEnable(i, TRUE));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	s.o(dev->LightEnable(5, TRUE));   // never set: the default light	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	s.o(dev->LightEnable(1, FALSE));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	s.o(dev->LightEnable(9, 7));   // a ninth index, a non-canonical TRUE	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	l2.Range = 0.1f;
	l2.Theta = 0.05f;
	s.o(dev->SetLight(2, &l2));   // changed while enabled	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
	dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR1);
	drawGrid();
	dev->SetRenderState(D3DRS_LOCALVIEWER, FALSE);
	mtl.Power = 0.0f;
	s.o(dev->SetMaterial(&mtl));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	drawGrid();
	{
		D3DLIGHT8 g;
		std::memset(&g, 0x5a, sizeof(g));
		s.o(dev->GetLight(1, &g));
		s.o(g);
		WINBOOL on = 0x5a;
		s.o(dev->GetLightEnable(1, &on));
		s.o(on);
	}
	const float plane[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
	for (DWORD i : { 0u, 1u, 5u }) {
		s.o(dev->SetClipPlane(i, plane));
		float g[4] = { 9, 9, 9, 9 };
		s.o(dev->GetClipPlane(i, g));
		s.o(g);
	}
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	for (IDirect3DTexture8* t : tex)
		if (t)
			t->Release();
	s.end();
	return s.ok;
}

// ================================================================== arm strict-sweep-draw
// streams 1-15 (+16), every primitive type through DrawPrimitive / DrawIndexedPrimitive / DrawPrimitiveUP /
// DrawIndexedPrimitiveUP with non-default start / min / count tuples (zeros and invalid types included), index bases incl. a null
// index buffer, INDEX32, strides with padding, an UP payload and a Clear over the worker's largest command (S), Clear's flags,
// depth, stencil, alpha and rectangle cases (negative, clipped, inverted, empty, under a smaller viewport).
inline bool sweepDraw(IDirect3DDevice8* dev, const Hooks& hk, std::string* d)
{
	S s;
	begin(s, dev, hk, d, "strict-sweep-draw");
	if (!s.getBB()) {
		*d += " SETUP: no back buffer";
		return false;
	}
	ffBase(s, nullptr);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	dev->SetRenderState(D3DRS_POINTSIZE, F(3.0f));
	CV verts[16];
	for (int i = 0; i < 16; ++i)
		verts[i] = { 4.0f + float(i % 4) * 18.0f + float(i / 4) * 2.0f, 3.0f + float(i / 4) * 19.0f + float(i % 3), 0.5f, 1.0f,
			0xff000000u | (uint32_t(i * 15) << 16) | (uint32_t(255 - i * 13) << 8) | uint32_t((i * 71) & 0xff) };
	IDirect3DVertexBuffer8* vb = nullptr;
	s.o(dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_WRITEONLY, kCV, D3DPOOL_MANAGED, &vb));
	if (!vb) {
		*d += " SETUP: vertex buffer";
		return false;
	}
	BYTE* p = nullptr;
	s.o(vb->Lock(0, 0, &p, 0));
	if (p)
		std::memcpy(p, verts, sizeof(verts));
	s.o(vb->Unlock());
	uint16_t idx16[48];
	uint32_t idx32[48];
	for (int i = 0; i < 48; ++i)
		idx16[i] = uint16_t((i * 5 + i / 3) % 11), idx32[i] = uint32_t((i * 7 + 2) % 10);
	IDirect3DIndexBuffer8* ib = nullptr;
	IDirect3DIndexBuffer8* ib32 = nullptr;
	s.o(dev->CreateIndexBuffer(sizeof(idx16), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib));
	s.o(dev->CreateIndexBuffer(sizeof(idx32), D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &ib32));
	if (!ib || !ib32) {
		*d += " SETUP: index buffers";
		return false;
	}
	s.o(ib->Lock(0, 0, &p, 0));
	if (p)
		std::memcpy(p, idx16, sizeof(idx16));
	s.o(ib->Unlock());
	s.o(ib32->Lock(0, 0, &p, 0));
	if (p)
		std::memcpy(p, idx32, sizeof(idx32));
	s.o(ib32->Unlock());

	// ---- streams 1..15 bound (stride 20 / 36 / 4), read back, unbound; 16 is out of range
	for (UINT st = 1; st < 17; ++st) {
		const UINT stride = st % 3 == 0 ? 36 : st % 3 == 1 ? 20 : 4;
		const HRESULT h = dev->SetStreamSource(st, vb, stride);
		s.o(h);
		IDirect3DVertexBuffer8* g = nullptr;
		UINT gs = 0xdead;
		const HRESULT hg = dev->GetStreamSource(st, &g, &gs);
		s.o(hg);
		s.o(g == vb);
		s.o(gs);
		if (st < 16)
			s.check(SUCCEEDED(h) && SUCCEEDED(hg) && g == vb && gs == stride, "GetStreamSource(" + std::to_string(st) + ") answers the binding");
		if (g)
			g->Release();
	}
	struct SS { UINT st, stride; };
	for (const SS& x : { SS { 1, 0 }, SS { 2, 1 }, SS { 3, 0xFFFF }, SS { 15, 0 } }) {   // stride boundaries (r11-oracle remainder)
		s.o(dev->SetStreamSource(x.st, vb, x.stride));
		IDirect3DVertexBuffer8* g = nullptr;
		UINT gs = 0xdead;
		s.o(dev->GetStreamSource(x.st, &g, &gs));
		s.o(gs);
		if (g)
			g->Release();
	}
	for (UINT st = 1; st < 16; ++st)
		s.o(dev->SetStreamSource(st, nullptr, 0));
	s.o(dev->SetStreamSource(0, vb, sizeof(CV)));
	dev->SetVertexShader(kCV);

	auto frame = [&](auto&& draws) {
		clearAll(s, 0xff202020);
		s.o(dev->BeginScene());
		draws();
		s.o(dev->EndScene());
		++s.draws;
		readBack(s);
	};
	// ---- DrawPrimitive: every type, two (start, count) each, zero, invalid types
	struct PD { D3DPRIMITIVETYPE t; UINT s0, c0, s1, c1; };
	const PD pd[6] = { { D3DPT_POINTLIST, 0, 16, 7, 5 }, { D3DPT_LINELIST, 0, 8, 5, 3 }, { D3DPT_LINESTRIP, 0, 15, 9, 4 },
		{ D3DPT_TRIANGLELIST, 0, 5, 6, 2 }, { D3DPT_TRIANGLESTRIP, 0, 14, 8, 3 }, { D3DPT_TRIANGLEFAN, 0, 14, 2, 4 } };
	for (const PD& x : pd)
		frame([&] {
			s.o(dev->DrawPrimitive(x.t, x.s0, x.c0));
			s.o(dev->DrawPrimitive(x.t, x.s1, x.c1));
		});
	frame([&] {
		s.o(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 0));
		s.o(dev->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(0), 0, 2));
		s.o(dev->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(7), 0, 2));
	});

	// ---- SetIndices / GetIndices: bases, a null buffer with a base, a huge base (no draw)
	for (UINT base : { 0u, 2u, 5u }) {
		s.o(dev->SetIndices(ib, base));
		IDirect3DIndexBuffer8* g = nullptr;
		UINT gb = 0xdead;
		const HRESULT h = dev->GetIndices(&g, &gb);
		s.o(h);
		s.o(g == ib);
		s.o(gb);
		s.check(SUCCEEDED(h) && g == ib && gb == base, "GetIndices answers the binding");
		if (g)
			g->Release();
		struct ID { D3DPRIMITIVETYPE t; UINT mn, nv, si, c; };
		const ID id[] = { { D3DPT_TRIANGLELIST, 0, 11, 0, 4 }, { D3DPT_TRIANGLELIST, 2, 9, 6, 2 }, { D3DPT_TRIANGLESTRIP, 0, 11, 3, 5 },
			{ D3DPT_TRIANGLEFAN, 1, 8, 0, 3 }, { D3DPT_LINELIST, 0, 11, 2, 4 }, { D3DPT_LINESTRIP, 0, 11, 1, 6 },
			{ D3DPT_POINTLIST, 0, 11, 0, 10 }, { D3DPT_TRIANGLELIST, 0, 0, 0, 0 }, { static_cast<D3DPRIMITIVETYPE>(9), 0, 11, 0, 2 },
			/* r11-oracle remainder: a count with a zero vertex range, extreme min / range (not read by the draw) */
			{ D3DPT_TRIANGLELIST, 0, 0, 0, 2 }, { D3DPT_TRIANGLESTRIP, 0x7fffffffu, 0xffffffffu, 0, 2 } };
		frame([&] {
			for (const ID& x : id)
				s.o(dev->DrawIndexedPrimitive(x.t, x.mn, x.nv, x.si, x.c));
		});
	}
	s.o(dev->SetIndices(ib32, 1));
	frame([&] {
		s.o(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 11, 0, 3));
		s.o(dev->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP, 1, 10, 9, 4));
	});
	s.o(dev->SetIndices(nullptr, 7));
	{
		IDirect3DIndexBuffer8* g = nullptr;
		UINT gb = 0xdead;
		s.o(dev->GetIndices(&g, &gb));
		s.o(g == nullptr);
		s.o(gb);
		if (g)
			g->Release();
	}
	for (UINT big : { 0x7fffffffu, 0x80000000u, 0xffffffffu }) {   // base boundaries (no draw)
		s.o(dev->SetIndices(ib, big));
		IDirect3DIndexBuffer8* g = nullptr;
		UINT gb = 0xdead;
		s.o(dev->GetIndices(&g, &gb));
		s.o(gb);
		if (g)
			g->Release();
	}
	s.o(dev->SetIndices(ib, 0));

	// ---- DrawPrimitiveUP: every type, zero, null (no bytes), an invalid type, a padded stride
	struct PV { CV v; float pad[4]; };
	PV padded[6];
	for (int i = 0; i < 6; ++i)
		padded[i] = { verts[i * 2], { 1, 2, 3, 4 } };
	frame([&] {
		s.o(dev->DrawPrimitiveUP(D3DPT_LINELIST, 3, verts, sizeof(CV)));
		s.o(dev->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, verts + 5, sizeof(CV)));
	});
	frame([&] {
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 3, verts + 1, sizeof(CV)));
		s.o(dev->DrawPrimitiveUP(D3DPT_POINTLIST, 6, verts + 9, sizeof(CV)));
	});
	frame([&] {
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0, verts, sizeof(CV)));
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0, nullptr, sizeof(CV)));
		// not swept: DrawPrimitiveUP(count > 0, nullptr) records a draw with no vertex buffer bound in BOTH modes and in the base
		// (VUID-vkCmdDraw-None-04007, measured 13/10) - pre-existing, named in the handback
		s.o(dev->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(0), 2, verts, sizeof(CV)));
		s.o(dev->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(8), 2, verts, sizeof(CV)));
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, padded, sizeof(PV)));
	});
	// ---- an UP payload over the worker's largest command (the arm's ring is 64 KiB: 16 KiB - 256): the S path
	{
		std::vector<CV> big;
		big.reserve(18000);
		for (int i = 0; i < 6000; ++i) {
			const float x = float(i % 60), y = float((i / 60) % 60);
			const DWORD c = 0xff000000u | uint32_t(i * 2654435761u >> 8 & 0xffffff);
			big.push_back({ x, y, 0.5f, 1, c });
			big.push_back({ x + 3, y, 0.5f, 1, c });
			big.push_back({ x, y + 3, 0.5f, 1, c });
		}
		frame([&] { s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 6000, big.data(), sizeof(CV))); });
	}
	// ---- DrawIndexedPrimitiveUP: INDEX16 and INDEX32, three types, zero, an invalid type
	{
		const uint16_t i16[12] = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 };
		const uint32_t i32[8] = { 1, 2, 3, 4, 5, 6, 7, 2 };
		frame([&] {
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 8, 4, i16, D3DFMT_INDEX16, verts, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLESTRIP, 1, 7, 3, i32, D3DFMT_INDEX32, verts + 4, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_POINTLIST, 0, 8, 8, i16, D3DFMT_INDEX16, verts + 8, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_LINESTRIP, 0, 8, 5, i32, D3DFMT_INDEX32, verts, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 8, 0, i16, D3DFMT_INDEX16, verts, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(11), 0, 8, 2, i16, D3DFMT_INDEX16, verts, sizeof(CV)));
			// r11-oracle remainder: LINELIST, TRIANGLEFAN, a padded stride, zero-count null pointers
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, 8, 4, i16, D3DFMT_INDEX16, verts, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLEFAN, 1, 7, 3, i32, D3DFMT_INDEX32, verts + 2, sizeof(CV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 6, 2, i16, D3DFMT_INDEX16, padded, sizeof(PV)));
			s.o(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 0, 0, nullptr, D3DFMT_INDEX16, nullptr, sizeof(CV)));
		});
		dev->SetStreamSource(0, vb, sizeof(CV));   // the UP draws unbound it
		dev->SetIndices(ib, 0);
	}
	// ---- Clear: flags, depth, stencil, alpha; each followed by a depth- / stencil-tested quad where it matters
	struct CL { DWORD flags; D3DCOLOR c; float z; DWORD st; };
	const CL cl[] = { { 0, 0xff00ff00, 0.0f, 0 }, { D3DCLEAR_ZBUFFER, 0xffff0000, 0.25f, 0 }, { D3DCLEAR_STENCIL, 0xffff0000, 1.0f, 0x5a },
		{ D3DCLEAR_TARGET | D3DCLEAR_STENCIL, 0x80ff0000, 1.0f, 0x33 }, { D3DCLEAR_TARGET, 0x80123456, 1.0f, 0 },
		{ D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00abcdef, 0.75f, 0 }, { D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.6f, 0xff } };
	for (const CL& c : cl) {
		clearAll(s, 0xff404040, 1.0f, 0);
		s.o(dev->Clear(0, nullptr, c.flags, c.c, c.z, c.st));
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
		dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
		dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		dev->SetRenderState(D3DRS_STENCILREF, c.st);
		s.o(dev->BeginScene());
		s.o(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 6));
		s.o(dev->EndScene());
		++s.draws;
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		readBack(s);
	}
	// rectangles: negative, clipped, inverted, empty, many; then under a smaller viewport
	const D3DRECT rr[5] = { { -10, -10, 20, 20 }, { 50, 50, 100, 100 }, { 30, 30, 10, 10 }, { 5, 5, 5, 9 }, { 24, 8, 40, 56 } };
	clearAll(s, 0xff000000);
	s.o(dev->Clear(5, rr, D3DCLEAR_TARGET, 0xffc08040, 1.0f, 0));
	readBack(s);
	D3DVIEWPORT8 vp = { 8, 8, 40, 40, 0.0f, 1.0f };
	s.o(dev->SetViewport(&vp));
	{
		D3DVIEWPORT8 g;
		std::memset(&g, 0x5a, sizeof(g));
		s.o(dev->GetViewport(&g));
		s.o(g);
		s.check(std::memcmp(&g, &vp, sizeof(vp)) == 0, "GetViewport answers the viewport set");
	}
	clearAll(s, 0xff000000);
	s.o(dev->Clear(2, rr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff4080c0, 0.5f, 0));
	s.o(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff800080, 1.0f, 0));   // whole viewport
	frame([&] { s.o(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 6)); });
	D3DVIEWPORT8 vp2 = { 3, 5, 50, 30, 0.25f, 0.75f };
	s.o(dev->SetViewport(&vp2));
	frame([&] { s.o(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 6)); });
	D3DVIEWPORT8 full = { 0, 0, 64, 64, 0.0f, 1.0f };
	s.o(dev->SetViewport(&full));
	// a rectangle payload over the worker's largest command (S; the arm's ring is 64 KiB: 16 KiB - 256): 1100 one-pixel rectangles
	// (17 600 bytes). Not 20000: MoltenVK's ClearAttachments encode stack-allocates per rectangle and ~20000 overflow a 512 KiB
	// std::thread stack (gx-vk-submit, lever 3 - measured 13/10, ___chkstk_darwin, both CS modes: pre-existing, residual R37).
	// 1100 = ~105 KB of encode stack against 512 KiB (~5x headroom). A ring-knob change that raises this count must be checked
	// against R37 first; it is the only arm reaching Clear's oversize S path (integrator 13/10).
	{
		std::vector<D3DRECT> many(1100);
		for (size_t i = 0; i < many.size(); ++i) {
			const LONG x = LONG((i * 7) % 64), y = LONG((i * 13 / 64) % 64);
			many[i] = { x, y, x + 1, y + 1 };
		}
		clearAll(s, 0xff000000);
		s.o(dev->Clear(DWORD(many.size()), many.data(), D3DCLEAR_TARGET, 0xff00c0ff, 1.0f, 0));
		readBack(s);
	}
	s.o(dev->SetStreamSource(0, nullptr, 0));
	s.o(dev->SetIndices(nullptr, 0));
	vb->Release();
	ib->Release();
	ib32->Release();
	s.end();
	return s.ok;
}

// ================================================================== arm strict-sweep-res
// creation: texture / buffer / target / depth / image formats, pools, usages, rectangular and zero sizes, MSAA and lockable
// targets; CopyRects / UpdateTexture / SetRenderTarget boundaries; shaders: usage, unknown / stale / cross-kind handles, constant
// counts and ranges (zero, null, past the end, a wrapping register).
inline bool sweepRes(IDirect3DDevice8* dev, const Hooks& hk, std::string* d, const DWORD* vsDecl, const DWORD* vsCode, const DWORD* psCode)
{
	S s;
	begin(s, dev, hk, d, "strict-sweep-res");
	if (!s.getBB()) {
		*d += " SETUP: no back buffer";
		return false;
	}
	ffBase(s, nullptr);
	IDirect3DSurface8* ds = nullptr;
	s.o(dev->GetDepthStencilSurface(&ds));
	// ---- textures: each created, every level written, sampled over the quad (stage 0, SELECTARG1)
	struct TX { UINT w, h, levels; DWORD usage; D3DFORMAT f; D3DPOOL pool; };
	const TX tx[] = { { 32, 8, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 8, 32, 3, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED },
		{ 16, 16, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED }, { 16, 16, 1, 0, D3DFMT_A1R5G5B5, D3DPOOL_MANAGED },
		{ 16, 16, 1, 0, D3DFMT_A4R4G4B4, D3DPOOL_MANAGED }, { 16, 16, 1, 0, D3DFMT_X1R5G5B5, D3DPOOL_MANAGED },
		{ 16, 16, 1, 0, D3DFMT_A8, D3DPOOL_MANAGED }, { 16, 16, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED }, { 16, 16, 1, 0, D3DFMT_A8L8, D3DPOOL_MANAGED },
		{ 16, 16, 0, 0, D3DFMT_DXT3, D3DPOOL_MANAGED }, { 16, 16, 0, 0, D3DFMT_DXT5, D3DPOOL_MANAGED }, { 16, 8, 2, 0, D3DFMT_DXT1, D3DPOOL_MANAGED },
		{ 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT }, { 16, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT },
		{ 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM }, { 16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH },
		{ 0, 0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 16, 0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED },
		{ 16, 16, 1, 0, D3DFMT_P8, D3DPOOL_MANAGED }, { 16, 16, 20, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED },
		{ 64, 1, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 16, 16, 1, D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT },
		/* r11-oracle remainder: */ { 0, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 16, 16, 0, 0, D3DFMT_DXT2, D3DPOOL_MANAGED },
		{ 16, 16, 0, 0, D3DFMT_DXT4, D3DPOOL_MANAGED }, { 16, 16, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT },
		{ 16, 16, 1, 0, D3DFMT_A8R8G8B8, static_cast<D3DPOOL>(7) }, { 8, 8, 0xFFFFFFFFu, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED },
		{ 4096, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 1, 4096, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED },
		{ 16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R5G6B5, D3DPOOL_DEFAULT } };
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	uint32_t seed = 3;
	IDirect3DSurface8* rds = nullptr;
	s.o(dev->GetDepthStencilSurface(&rds));
	for (const TX& t : tx) {
		bool defined = false;
		IDirect3DTexture8* tex = makeTex(s, t.w, t.h, t.levels, t.usage, t.f, t.pool, seed++, &defined);
		if (!tex)
			continue;
		if (t.usage & D3DUSAGE_RENDERTARGET) {   // an RT texture is defined by a clear through its level-0 surface (P2-4)
			IDirect3DSurface8* lv = nullptr;
			s.o(tex->GetSurfaceLevel(0, &lv));
			if (lv) {
				D3DSURFACE_DESC sd = {};
				s.o(lv->GetDesc(&sd));
				obsDesc(s, sd);
				const HRESULT hs = dev->SetRenderTarget(lv, nullptr);
				s.o(hs);
				if (SUCCEEDED(hs)) {
					const HRESULT hc = dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff40c080, 1.0f, 0);
					s.o(hc);
					defined = SUCCEEDED(hc);
				}
				s.o(dev->SetRenderTarget(s.bb, rds));
				lv->Release();
			}
		}
		if (!defined) {   // undefined texels are never sampled into obs (r11-oracle P2-4): the description alone is the result
			tex->Release();
			continue;
		}
		s.o(dev->SetTexture(0, tex));
		clearAll(s, 0xff000000);
		drawQuad(s);
		readBack(s);
		s.o(dev->SetTexture(0, nullptr));
		tex->Release();
	}
	if (rds)
		rds->Release();   // (a leaked reference keeps the device alive into the exit hook: measured, SIGABRT at exit)
	// ---- vertex / index buffers: sizes, usages, pools, FVFs; one of each drawn
	struct VB { UINT len; DWORD usage, fvf; D3DPOOL pool; };
	const VB vbs[] = { { 0, 0, kCV, D3DPOOL_MANAGED }, { 1, 0, kCV, D3DPOOL_MANAGED }, { 100, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, kCV, D3DPOOL_DEFAULT },
		{ 80, 0, kCV, D3DPOOL_SYSTEMMEM }, { 96, D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX2, D3DPOOL_MANAGED },
		{ 81, D3DUSAGE_POINTS, 0, D3DPOOL_MANAGED }, { 120, D3DUSAGE_DYNAMIC, kCV, D3DPOOL_SYSTEMMEM },
		/* r11-oracle remainder: */ { 64, 0, kCV, D3DPOOL_SCRATCH }, { 80, D3DUSAGE_SOFTWAREPROCESSING, kCV, D3DPOOL_MANAGED },
		{ 80, D3DUSAGE_DONOTCLIP, kCV, D3DPOOL_MANAGED }, { 80, D3DUSAGE_RTPATCHES | D3DUSAGE_NPATCHES, kCV, D3DPOOL_MANAGED },
		{ 128, 0, D3DFVF_XYZB2 | D3DFVF_DIFFUSE, D3DPOOL_MANAGED }, { 128, 0, D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_SPECULAR, D3DPOOL_MANAGED },
		{ 256, 0, D3DFVF_XYZ | D3DFVF_TEX4 | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1) | D3DFVF_TEXCOORDSIZE4(3), D3DPOOL_MANAGED },
		{ 0xFFFFFFF0u, 0, kCV, D3DPOOL_MANAGED } };
	const CV quad[6] = { { 4, 4, 0.5f, 1, 0xffff0000 }, { 60, 4, 0.5f, 1, 0xff00ff00 }, { 4, 60, 0.5f, 1, 0xff0000ff }, { 60, 60, 0.5f, 1, 0xffffffff },
		{ 32, 2, 0.5f, 1, 0xff808080 }, { 2, 32, 0.5f, 1, 0xff404040 } };
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	for (const VB& v : vbs) {
		IDirect3DVertexBuffer8* b = poison<IDirect3DVertexBuffer8>();
		if (!created(s, dev->CreateVertexBuffer(v.len, v.usage, v.fvf, v.pool, &b), b, "CreateVertexBuffer"))
			continue;
		D3DVERTEXBUFFER_DESC bd = {};
		s.o(b->GetDesc(&bd));
		s.o(bd.Format);
		s.o(bd.Type);
		s.o(bd.Size);
		s.o(bd.Usage);
		s.o(bd.FVF);
		s.o(bd.Pool);
		s.check(bd.Usage == v.usage && bd.FVF == v.fvf && bd.Pool == v.pool && bd.Size >= v.len, "the vertex buffer describes the usage / FVF / pool / size requested");
		BYTE* p = nullptr;
		const HRESULT hl = b->Lock(0, 0, &p, v.usage & D3DUSAGE_DYNAMIC ? D3DLOCK_DISCARD : 0);
		s.o(hl);
		if (SUCCEEDED(hl) && p) {
			std::memcpy(p, quad, std::min<size_t>(v.len, sizeof(quad)));
			s.o(b->Unlock());
		}
		if (v.fvf == kCV && v.len >= 80 && v.pool != D3DPOOL_SYSTEMMEM) {
			dev->SetVertexShader(kCV);
			s.o(dev->SetStreamSource(0, b, sizeof(CV)));
			clearAll(s, 0xff000000);
			s.o(dev->BeginScene());
			s.o(dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
			s.o(dev->EndScene());
			++s.draws;
			readBack(s);
			s.o(dev->SetStreamSource(0, nullptr, 0));
		}
		b->Release();
	}
	struct IB { UINT len; DWORD usage; D3DFORMAT f; D3DPOOL pool; };
	const IB ibs[] = { { 24, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED }, { 12, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT },
		{ 12, 0, D3DFMT_INDEX16, D3DPOOL_SYSTEMMEM }, { 12, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED }, { 0, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED },
		{ 13, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED },
		/* r11-oracle remainder: */ { 12, 0, D3DFMT_INDEX16, D3DPOOL_SCRATCH }, { 24, 0, D3DFMT_INDEX32, D3DPOOL_DEFAULT },
		{ 12, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT }, { 12, D3DUSAGE_DYNAMIC, D3DFMT_INDEX32, D3DPOOL_SYSTEMMEM } };
	IDirect3DVertexBuffer8* qvb = nullptr;
	s.o(dev->CreateVertexBuffer(sizeof(quad), 0, kCV, D3DPOOL_MANAGED, &qvb));
	if (qvb) {
		BYTE* p = nullptr;
		const HRESULT hl = qvb->Lock(0, 0, &p, 0);
		s.o(hl);
		if (SUCCEEDED(hl) && p) {
			std::memcpy(p, quad, sizeof(quad));
			s.o(qvb->Unlock());
		}
	}
	for (const IB& x : ibs) {
		IDirect3DIndexBuffer8* b = poison<IDirect3DIndexBuffer8>();
		if (!created(s, dev->CreateIndexBuffer(x.len, x.usage, x.f, x.pool, &b), b, "CreateIndexBuffer"))
			continue;
		D3DINDEXBUFFER_DESC bd = {};
		s.o(b->GetDesc(&bd));
		s.o(bd.Format);
		s.o(bd.Type);
		s.o(bd.Usage);
		s.o(bd.Size);
		s.o(bd.Pool);
		s.check(bd.Format == x.f && bd.Usage == x.usage && bd.Pool == x.pool && bd.Size >= x.len, "the index buffer describes the format / usage / pool / size requested");
		BYTE* p = nullptr;
		const HRESULT hl = b->Lock(0, 0, &p, x.usage & D3DUSAGE_DYNAMIC ? D3DLOCK_DISCARD : 0);
		s.o(hl);
		if (SUCCEEDED(hl) && p) {
			const uint16_t i16[6] = { 0, 1, 2, 2, 1, 3 };
			const uint32_t i32[6] = { 3, 1, 2, 2, 1, 0 };
			std::memcpy(p, x.f == D3DFMT_INDEX32 ? static_cast<const void*>(i32) : static_cast<const void*>(i16), std::min<size_t>(x.len, x.f == D3DFMT_INDEX32 ? 24 : 12));
			s.o(b->Unlock());
		}
		if (qvb && x.pool != D3DPOOL_SYSTEMMEM && x.len >= 12 && (x.f == D3DFMT_INDEX16 || x.f == D3DFMT_INDEX32)) {
			dev->SetVertexShader(kCV);
			dev->SetStreamSource(0, qvb, sizeof(CV));
			s.o(dev->SetIndices(b, 1));
			clearAll(s, 0xff000000);
			s.o(dev->BeginScene());
			s.o(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, x.f == D3DFMT_INDEX32 ? 2 : 1));
			s.o(dev->EndScene());
			++s.draws;
			readBack(s);
			dev->SetIndices(nullptr, 0);
			dev->SetStreamSource(0, nullptr, 0);
		}
		b->Release();
	}
	// ---- render targets: formats, MSAA, lockable, rectangular, zero; each bound, cleared, drawn and copied back
	struct RT { UINT w, h; D3DFORMAT f; D3DMULTISAMPLE_TYPE ms; WINBOOL lockable; };
	const RT rts[] = { { 32, 16, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, TRUE }, { 16, 16, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, FALSE },
		{ 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, FALSE }, { 16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_3_SAMPLES, FALSE },
		{ 0, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE }, { 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, TRUE },
		/* r11-oracle remainder: */ { 16, 0, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE }, { 16, 16, D3DFMT_A1R5G5B5, D3DMULTISAMPLE_NONE, FALSE },
		{ 16, 16, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, FALSE }, { 16, 16, D3DFMT_A8R8G8B8, static_cast<D3DMULTISAMPLE_TYPE>(17), FALSE } };
	dev->SetVertexShader(kCV);
	for (const RT& r : rts) {
		IDirect3DSurface8* rt = poison<IDirect3DSurface8>();
		if (!created(s, dev->CreateRenderTarget(r.w, r.h, r.f, r.ms, r.lockable, &rt), rt, "CreateRenderTarget"))
			continue;
		D3DSURFACE_DESC sd = {};
		s.o(rt->GetDesc(&sd));
		obsDesc(s, sd);
		s.check(sd.Format == r.f && sd.Width == r.w && sd.Height == r.h && sd.Pool == D3DPOOL_DEFAULT && (sd.Usage & D3DUSAGE_RENDERTARGET),
			"the render target describes the format / size / pool / usage requested");
		const HRESULT hs = dev->SetRenderTarget(rt, nullptr);
		s.o(hs);
		if (SUCCEEDED(hs)) {
			s.o(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff336699, 1.0f, 0));
			s.o(dev->BeginScene());
			s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(CV)));
			s.o(dev->EndScene());
			++s.draws;
			IDirect3DSurface8* g = nullptr;
			s.o(dev->GetRenderTarget(&g));
			s.o(g == rt);
			if (g)
				g->Release();
			// copied into the back buffer's corner (a resolve for the MSAA one), then the back buffer read back
			s.o(dev->SetRenderTarget(s.bb, ds));
			const RECT src = { 0, 0, LONG(std::min<UINT>(r.w, 16)), LONG(std::min<UINT>(r.h, 16)) };
			const POINT at = { 8, 8 };
			s.o(dev->CopyRects(rt, &src, 1, s.bb, &at));
			readBack(s);
		}
		s.o(dev->SetRenderTarget(s.bb, ds));
		rt->Release();
	}
	// ---- depth surfaces: formats, MSAA, rectangular, zero; each bound with the back buffer, cleared, depth-tested
	struct DS { UINT w, h; D3DFORMAT f; D3DMULTISAMPLE_TYPE ms; };
	const DS dss[] = { { 64, 64, D3DFMT_D16, D3DMULTISAMPLE_NONE }, { 64, 64, D3DFMT_D24X8, D3DMULTISAMPLE_NONE },
		{ 64, 64, D3DFMT_D32, D3DMULTISAMPLE_NONE }, { 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES }, { 96, 80, D3DFMT_D24S8, D3DMULTISAMPLE_NONE },
		{ 0, 0, D3DFMT_D24S8, D3DMULTISAMPLE_NONE }, { 64, 64, D3DFMT_D15S1, D3DMULTISAMPLE_NONE },
		/* r11-oracle remainder: */ { 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_3_SAMPLES }, { 64, 64, D3DFMT_D16_LOCKABLE, D3DMULTISAMPLE_NONE },
		{ 64, 64, D3DFMT_D24X4S4, D3DMULTISAMPLE_NONE }, { 0, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE },
		{ 32, 32, D3DFMT_D24S8, D3DMULTISAMPLE_NONE } /* smaller than the 64x64 target: refused as a pair (targetSizeRefused) */ };
	for (const DS& x : dss) {
		IDirect3DSurface8* z = poison<IDirect3DSurface8>();
		if (!created(s, dev->CreateDepthStencilSurface(x.w, x.h, x.f, x.ms, &z), z, "CreateDepthStencilSurface"))
			continue;
		{
			D3DSURFACE_DESC sd = {};
			s.o(z->GetDesc(&sd));
			obsDesc(s, sd);
			s.check(sd.Format == x.f && sd.Width == x.w && sd.Height == x.h && (sd.Usage & D3DUSAGE_DEPTHSTENCIL),
				"the depth surface describes the format / size / usage requested");
		}
		const HRESULT hs = dev->SetRenderTarget(s.bb, z);
		s.o(hs);
		if (SUCCEEDED(hs)) {
			IDirect3DSurface8* g = nullptr;
			s.o(dev->GetDepthStencilSurface(&g));
			s.o(g == z);
			if (g)
				g->Release();
			s.o(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 0.4f, 0));
			dev->SetRenderState(D3DRS_ZENABLE, TRUE);
			dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
			const CV q2[4] = { { 4, 4, 0.2f, 1, 0xffff0000 }, { 60, 4, 0.6f, 1, 0xff00ff00 }, { 4, 60, 0.3f, 1, 0xff0000ff }, { 60, 60, 0.7f, 1, 0xffffffff } };
			s.o(dev->BeginScene());
			s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q2, sizeof(CV)));
			s.o(dev->EndScene());
			++s.draws;
			dev->SetRenderState(D3DRS_ZENABLE, FALSE);
			readBack(s);
		}
		s.o(dev->SetRenderTarget(s.bb, ds));
		z->Release();
	}
	// ---- image surfaces + CopyRects / UpdateTexture boundaries
	const D3DFORMAT imf[] = { D3DFMT_R5G6B5, D3DFMT_DXT1, D3DFMT_A8, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_L8, D3DFMT_A8L8, D3DFMT_DXT5 };
	for (D3DFORMAT f : imf) {
		IDirect3DSurface8* im = poison<IDirect3DSurface8>();
		if (!created(s, dev->CreateImageSurface(16, 16, f, &im), im, "CreateImageSurface"))
			continue;
		{
			D3DSURFACE_DESC sd = {};
			s.o(im->GetDesc(&sd));
			obsDesc(s, sd);
			s.check(sd.Format == f && sd.Width == 16 && sd.Height == 16, "the image surface describes the format / size requested");
			D3DLOCKED_RECT lw = {};   // defined contents before any copy (r11-oracle P2-4: never observe undefined bytes)
			const HRESULT hw = im->LockRect(&lw, nullptr, 0);
			s.o(hw);
			if (SUCCEEDED(hw)) {
				UINT rb = 0, rows = 0;
				levelLayout(f, 16, 16, &rb, &rows);
				fillRows(lw, rb, rows, 77);
				s.o(im->UnlockRect());
			}
		}
		s.o(dev->CopyRects(s.bb, nullptr, 0, im, nullptr));   // whole surface: a size / format mismatch is refused
		const RECT r1 = { 4, 4, 12, 12 };
		const POINT p1 = { 2, 3 };
		s.o(dev->CopyRects(s.bb, &r1, 1, im, &p1));
		D3DLOCKED_RECT lr = {};
		const HRESULT hl = im->LockRect(&lr, nullptr, D3DLOCK_READONLY);
		s.o(hl);
		if (SUCCEEDED(hl)) {
			UINT rb = 0, rows = 0;
			levelLayout(f, 16, 16, &rb, &rows);
			for (UINT y = 0; y < rows; ++y)
				s.raw(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, rb);
			s.o(im->UnlockRect());
		}
		im->Release();
	}
	{
		struct ID { UINT w, h; };
		for (const ID& x : { ID { 0, 0 }, ID { 0, 16 }, ID { 16, 0 }, ID { 16, 8 } }) {
			IDirect3DSurface8* im = poison<IDirect3DSurface8>();
			if (created(s, dev->CreateImageSurface(x.w, x.h, D3DFMT_A8R8G8B8, &im), im, "CreateImageSurface")) {
				D3DSURFACE_DESC sd = {};
				s.o(im->GetDesc(&sd));
				obsDesc(s, sd);
				im->Release();
			}
		}
		IDirect3DSurface8* im = poison<IDirect3DSurface8>();
		if (created(s, dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &im), im, "CreateImageSurface")) {
			D3DLOCKED_RECT lw = {};
			const HRESULT hw = im->LockRect(&lw, nullptr, 0);
			s.o(hw);
			if (SUCCEEDED(hw)) {
				fillRows(lw, 256, 64, 91);
				s.o(im->UnlockRect());
			}
			const RECT rs[3] = { { 0, 0, 8, 8 }, { 8, 8, 24, 16 }, { 60, 60, 64, 64 } };
			const POINT ps[3] = { { 40, 40 }, { 0, 48 }, { 1, 1 } };
			s.o(dev->CopyRects(s.bb, rs, 3, im, ps));
			s.o(dev->CopyRects(s.bb, rs, 3, im, nullptr));
			const RECT out = { 50, 50, 80, 80 };   // partly outside the source
			s.o(dev->CopyRects(s.bb, &out, 1, im, nullptr));
			const POINT offEnd = { 60, 60 };       // runs off the destination (not `far`: an empty macro in the Windows headers)
			s.o(dev->CopyRects(s.bb, rs + 1, 1, im, &offEnd));
			s.o(dev->CopyRects(s.bb, rs, 0, im, nullptr));   // count 0 with rectangles
			s.o(dev->CopyRects(nullptr, nullptr, 0, im, nullptr));
			s.o(dev->CopyRects(s.bb, nullptr, 0, nullptr, nullptr));
			s.o(dev->CopyRects(im, rs, 1, im, ps));   // source = destination
			D3DLOCKED_RECT lr = {};
			const HRESULT hl = im->LockRect(&lr, nullptr, D3DLOCK_READONLY);
			s.o(hl);
			if (SUCCEEDED(hl)) {
				for (UINT y = 0; y < 64; ++y)
					s.raw(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 256);
				s.o(im->UnlockRect());
			}
			s.o(dev->CopyRects(im, rs, 2, s.bb, ps));   // host -> the back buffer
			readBack(s);
			// r11-oracle remainder: empty / inverted / negative rectangles, and a whole-surface host copy INTO THE BACK BUFFER - refused
			// before any payload routing (a host copy needs a texture-level destination); the oversize host -> DEFAULT S path is
			// gx_native_test's strict-copy-oversize
			const RECT odd[3] = { { 10, 10, 10, 20 }, { 20, 20, 12, 12 }, { -4, -4, 8, 8 } };
			s.o(dev->CopyRects(im, odd, 3, s.bb, ps));
			s.o(dev->CopyRects(im, nullptr, 0, s.bb, nullptr));
			readBack(s);
			im->Release();
		}
	}
	{
		IDirect3DTexture8* sys = makeTex(s, 16, 16, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, 41);
		IDirect3DTexture8* def = nullptr;
		s.o(dev->CreateTexture(16, 16, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &def));
		IDirect3DTexture8* one = nullptr;
		s.o(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &one));
		IDirect3DTexture8* fmt = nullptr;
		s.o(dev->CreateTexture(16, 16, 3, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &fmt));
		s.o(dev->UpdateTexture(nullptr, def));
		s.o(dev->UpdateTexture(sys, nullptr));
		s.o(dev->UpdateTexture(sys, sys));
		s.o(dev->UpdateTexture(sys, one));   // level count mismatch
		s.o(dev->UpdateTexture(sys, fmt));   // format mismatch
		const HRESULT hu = dev->UpdateTexture(sys, def);
		s.o(hu);
		if (def && SUCCEEDED(hu)) {   // only a texture the update defined is sampled (P2-4)
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			s.o(dev->SetTexture(0, def));
			dev->SetVertexShader(kTV);
			clearAll(s, 0xff000000);
			drawQuad(s);
			readBack(s);
			dev->SetTexture(0, nullptr);
		}
		for (IDirect3DTexture8* t : { sys, def, one, fmt })
			if (t)
				t->Release();
	}
	// ---- host -> DEFAULT copies at NONZERO destination points (the base binary runs them too, so base == CS0
	// covers a mode-independent offset error): two rectangles of an 8x8 image into a cleared 16x16 render-target texture at (9,7) and
	// (1,12); a 64x64 image into a cleared 128x128 one at (32,40) (over the arm's largest command: S). Each read back whole.
	for (int k = 0; k < 2; ++k) {
		const UINT tw = k ? 128 : 16, sw = k ? 64 : 8;
		IDirect3DTexture8* t = poison<IDirect3DTexture8>();
		if (!created(s, dev->CreateTexture(tw, tw, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t), t, "CreateTexture"))
			continue;
		IDirect3DSurface8* lv = nullptr;
		IDirect3DSurface8* img = nullptr;
		IDirect3DSurface8* rb = nullptr;
		s.o(t->GetSurfaceLevel(0, &lv));
		if (lv && SUCCEEDED(dev->SetRenderTarget(lv, nullptr))) {
			s.o(dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0));
			s.o(dev->SetRenderTarget(s.bb, ds));
			s.o(dev->CreateImageSurface(sw, sw, D3DFMT_A8R8G8B8, &img));
			D3DLOCKED_RECT lw = {};
			if (img && SUCCEEDED(img->LockRect(&lw, nullptr, 0))) {
				fillRows(lw, sw * 4, sw, 51 + uint32_t(k));
				s.o(img->UnlockRect());
			}
			const RECT rs2[2] = { { 2, 1, 6, 5 }, { 0, 0, 2, 2 } }, whole = { 0, 0, 64, 64 };
			const POINT ps2[2] = { { 9, 7 }, { 1, 12 } }, at = { 32, 40 };
			if (img)
				s.o(k ? dev->CopyRects(img, &whole, 1, lv, &at) : dev->CopyRects(img, rs2, 2, lv, ps2));
			s.o(dev->CreateImageSurface(tw, tw, D3DFMT_A8R8G8B8, &rb));
			if (rb && SUCCEEDED(dev->CopyRects(lv, nullptr, 0, rb, nullptr))) {
				D3DLOCKED_RECT lr = {};
				const HRESULT hl = rb->LockRect(&lr, nullptr, D3DLOCK_READONLY);
				s.o(hl);
				if (SUCCEEDED(hl)) {
					for (UINT y = 0; y < tw; ++y)
						s.raw(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(tw) * 4);
					s.o(rb->UnlockRect());
				}
			}
		}
		s.o(dev->SetRenderTarget(s.bb, ds));
		for (IDirect3DSurface8* x : { rb, img, lv })
			if (x)
				x->Release();
		t->Release();
	}
	// ---- SetRenderTarget: a null colour target, then the depth alone changed
	s.o(dev->SetRenderTarget(nullptr, ds));
	{
		IDirect3DSurface8* g = nullptr;
		s.o(dev->GetRenderTarget(&g));
		s.o(g == s.bb);
		if (g)
			g->Release();
	}
	s.o(dev->SetRenderTarget(s.bb, nullptr));
	{
		IDirect3DSurface8* g = nullptr;
		s.o(dev->GetDepthStencilSurface(&g));
		s.o(g == nullptr);
		if (g)
			g->Release();
	}
	s.o(dev->SetRenderTarget(s.bb, ds));
	// ---- shaders: usage, declaration only, unknown / stale / cross-kind handles, constants
	DWORD vs = 0, vsDeclOnly = 0, ps = 0, junk = 0;
	s.o(dev->CreateVertexShader(vsDecl, vsCode, &vs, D3DUSAGE_SOFTWAREPROCESSING));
	s.o(dev->CreateVertexShader(vsDecl, nullptr, &vsDeclOnly, 0));
	s.o(dev->CreatePixelShader(psCode, &ps));
	const DWORD tinyPs[] = { 0xFFFF0101u /* ps.1.1 */, 0x00000001u, 0x800F0000u, 0x90E40000u /* mov r0, v0 */, 0x0000FFFFu };
	s.o(dev->CreatePixelShader(tinyPs, &junk));
	s.o(vs);
	s.o(vsDeclOnly);
	s.o(ps);
	s.o(junk);
	const float k[6][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0.5f, 0.25f, 0.125f, 1 }, { 2, 3, 4, 5 } };
	s.o(dev->SetVertexShaderConstant(4, k, 0));
	s.o(dev->SetVertexShaderConstant(4, nullptr, 1));
	s.o(dev->SetVertexShaderConstant(100, k, 4));
	s.o(dev->SetVertexShaderConstant(95, k, 3));
	s.o(dev->SetVertexShaderConstant(0xFFFFFFFFu, k, 2));
	s.o(dev->SetVertexShaderConstant(4, k, 5));
	s.o(dev->SetPixelShaderConstant(7, k, 3));
	s.o(dev->SetPixelShaderConstant(8, k, 1));
	s.o(dev->SetPixelShaderConstant(0, k, 0));
	s.o(dev->SetPixelShaderConstant(0, nullptr, 2));
	s.o(dev->SetPixelShaderConstant(0xFFFFFFFEu, k, 4));
	{   // r11-oracle remainder: every writable vertex register and pixel register, and huge counts from a valid register
		std::vector<float> big(96 * 4);
		for (size_t i = 0; i < big.size(); ++i)
			big[i] = float(i % 37) * 0.125f - 2.0f;
		s.o(dev->SetVertexShaderConstant(0, big.data(), 4));
		s.o(dev->SetVertexShaderConstant(9, big.data() + 36, 1));
		s.o(dev->SetVertexShaderConstant(14, big.data() + 56, 76));
		s.o(dev->SetVertexShaderConstant(10, big.data(), 0xFFFFFFFFu));   // reads c10-c95 only
		s.o(dev->SetPixelShaderConstant(0, big.data(), 1));
		s.o(dev->SetPixelShaderConstant(4, big.data() + 16, 2));
		s.o(dev->SetPixelShaderConstant(3, big.data(), 0xFFFFFFFFu));      // reads c3-c7 only
		s.o(dev->SetVertexShaderConstant(4, k, 5));                          // the Trees.vso draws below read c4-c8
	}
	{   // every catalogue program created (declaration + code for a vertex shader), its handle observed, deleted
		size_t n = 0;
		const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
		for (size_t i = 0; i < n; ++i) {
			if (!all[i].program)
				continue;
			DWORD h = 0x5a5a;
			const DWORD* code = reinterpret_cast<const DWORD*>(all[i].dwords);
			const HRESULT hc = all[i].pixel ? dev->CreatePixelShader(code, &h) :
				dev->CreateVertexShader(all[i].decl ? reinterpret_cast<const DWORD*>(all[i].decl) : vsDecl, code, &h, 0);
			s.o(hc);
			s.o(h);
			if (SUCCEEDED(hc))
				s.o(all[i].pixel ? dev->DeletePixelShader(h) : dev->DeleteVertexShader(h));
		}
	}
	struct TreeV { float pos[3], normal[3]; DWORD c; float uv[2]; };
	const TreeV tq[4] = { { { -1, -1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 0, 0 } }, { { 1, -1, 0.5f }, { 0, 1, 0 }, 0xffcc9933, { 1, 0 } },
		{ { -1, 1, 0.5f }, { 0, 1, 0 }, 0xff33cc99, { 0, 1 } }, { { 1, 1, 0.5f }, { 0, 1, 0 }, 0xff9933cc, { 1, 1 } } };
	auto drawVs = [&](DWORD h) {
		s.o(dev->SetVertexShader(h));
		clearAll(s, 0xffffffff);
		s.o(dev->BeginScene());
		s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(TreeV)));
		s.o(dev->EndScene());
		++s.draws;
		readBack(s);
	};
	drawVs(vs);
	drawVs(vsDeclOnly);
	drawVs(0x7ffffff1u);   // an unknown handle (shader-handle shaped)
	s.o(dev->DeletePixelShader(vs));   // cross-kind
	s.o(dev->DeleteVertexShader(ps));
	s.o(dev->DeleteVertexShader(0x12345u));
	s.o(dev->DeletePixelShader(0));
	s.o(dev->SetVertexShader(kCV));
	s.o(dev->SetPixelShader(ps));
	clearAll(s, 0xff000000);
	s.o(dev->BeginScene());
	s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(CV)));
	s.o(dev->EndScene());
	++s.draws;
	readBack(s);
	s.o(dev->SetPixelShader(0x7ffffff3u));   // unknown
	clearAll(s, 0xff000000);
	s.o(dev->BeginScene());
	s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(CV)));
	s.o(dev->EndScene());
	readBack(s);
	s.o(dev->SetPixelShader(0));
	s.o(dev->DeleteVertexShader(vs));
	s.o(dev->DeleteVertexShader(vs));   // twice: stale
	s.o(dev->DeleteVertexShader(vsDeclOnly));
	s.o(dev->DeletePixelShader(ps));
	s.o(dev->DeletePixelShader(junk));
	s.o(dev->SetPixelShader(ps));   // r11-oracle remainder: a stale PS bound - its draw refused, the clear stays
	clearAll(s, 0xff000000);
	s.o(dev->BeginScene());
	s.o(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(CV)));
	s.o(dev->EndScene());
	++s.draws;
	readBack(s);
	s.o(dev->SetPixelShader(0));
	for (DWORD h : { 0xFFFFFFFFu, 1u, 3u, 0xFFFFFFFEu }) {   // invalid-handle boundaries
		s.o(dev->DeleteVertexShader(h));
		s.o(dev->DeletePixelShader(h));
	}
	drawVs(vs);   // stale handle bound
	s.o(dev->SetVertexShader(kCV));
	if (qvb)
		qvb->Release();
	if (ds)
		ds->Release();
	s.end();
	return s.ok;
}

// ================================================================== arm strict-sweep-misc (a window + swapchain, two back buffers)
// Present's ignored arguments, GetBackBuffer types / indices, gamma, cursor, discard bytes, QueryInterface / GetDirect3D, every
// inert API with arguments, null-pointer validation, device / display / creation queries, cube / volume / additional swap chain
// creation, and - LAST - Reset with alternative parameters (fullscreen, MSAA, flags, depth format, a refused back-buffer format).
inline bool sweepMisc(IDirect3DDevice8* dev, const Hooks& hk, std::string* d, const DWORD* vsDecl, const DWORD* vsCode, const DWORD* psCode)
{
	S s;
	begin(s, dev, hk, d, "strict-sweep-misc");
	if (!s.getBB()) {
		*d += " SETUP: no back buffer";
		return false;
	}
	ffBase(s, nullptr);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	// ---- Present's ignored arguments (preservation)
	const RECT src = { 0, 0, 32, 32 }, dst = { 10, 10, 50, 50 };
	int fakeWindow = 0;
	struct { RGNDATAHEADER h; RECT r[1]; } rgn = {};
	rgn.h.dwSize = sizeof(RGNDATAHEADER);
	rgn.h.iType = 1;   // RDH_RECTANGLES
	rgn.h.nCount = 1;
	rgn.h.nRgnSize = sizeof(RECT);
	rgn.h.rcBound = src;
	rgn.r[0] = src;
	clearAll(s, 0xff102030);
	drawQuad(s);
	s.o(dev->Present(&src, &dst, reinterpret_cast<HWND>(&fakeWindow), reinterpret_cast<const RGNDATA*>(&rgn)));
	s.getBB();
	clearAll(s, 0xff302010);
	drawQuad(s);
	readBack(s);
	s.o(dev->Present(nullptr, &dst, nullptr, nullptr));
	s.getBB();
	// ---- GetBackBuffer: types and indices
	for (UINT i : { 0u, 1u, 2u, 0xFFFFFFFFu })
		for (D3DBACKBUFFER_TYPE t : { D3DBACKBUFFER_TYPE_MONO, D3DBACKBUFFER_TYPE_LEFT, D3DBACKBUFFER_TYPE_RIGHT }) {
			IDirect3DSurface8* g = nullptr;
			s.o(dev->GetBackBuffer(i, t, &g));
			s.o(g != nullptr);
			if (g)
				g->Release();
		}
	// ---- gamma, cursor, discard bytes
	D3DGAMMARAMP ramp;
	for (int i = 0; i < 256; ++i) {
		ramp.red[i] = WORD(i * 257);
		ramp.green[i] = WORD((std::min)(65535, i * 300));
		ramp.blue[i] = WORD(65535 - i * 257);
	}
	dev->SetGammaRamp(0, &ramp);
	dev->SetGammaRamp(1 /* D3DSGR_CALIBRATE */, &ramp);
	{
		D3DGAMMARAMP g;
		std::memset(&g, 0x5a, sizeof(g));
		dev->GetGammaRamp(&g);
		s.o(g);
	}
	for (int i = 0; i < 256; ++i)
		ramp.red[i] = ramp.green[i] = ramp.blue[i] = WORD(i * 257);
	dev->SetGammaRamp(0, &ramp);
	{
		D3DGAMMARAMP g;
		std::memset(&g, 0x5a, sizeof(g));
		dev->GetGammaRamp(&g);   // after the identity ramp (r11-oracle remainder)
		s.o(g);
		dev->SetGammaRamp(0, nullptr);
		dev->GetGammaRamp(nullptr);
	}
	s.o(dev->ShowCursor(2));
	s.o(dev->ShowCursor(-1));
	s.o(dev->ShowCursor(0));
	s.o(dev->ShowCursor(TRUE));
	s.o(dev->ResourceManagerDiscardBytes(12345));
	s.o(dev->ResourceManagerDiscardBytes(0));
	{
		IDirect3DSurface8* cur = nullptr;
		s.o(dev->CreateImageSurface(32, 32, D3DFMT_A8R8G8B8, &cur));
		s.o(dev->SetCursorProperties(3, 4, cur));
		s.o(dev->SetCursorProperties(0, 0, nullptr));
		if (cur)
			cur->Release();
	}
	dev->SetCursorPosition(10, 20, 1 /* D3DCURSOR_IMMEDIATE_UPDATE */);
	dev->SetCursorPosition(0xFFFFFFFFu, 7, 0);
	// ---- QueryInterface / GetDirect3D
	{
		// the IIDs as values (GxNativeUuid.h's helpers belong to the device's own files)
		const GUID iidDevice8 = { 0x7385E5DF, 0x8FE8, 0x41D5, { 0x86, 0xB6, 0xD7, 0xB4, 0x85, 0x47, 0xB6, 0xCF } };
		const GUID iidUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
		void* p = nullptr;
		s.o(dev->QueryInterface(iidDevice8, &p));
		s.o(p == static_cast<void*>(dev));
		if (p)
			static_cast<IUnknown*>(p)->Release();
		p = nullptr;
		s.o(dev->QueryInterface(iidUnknown, &p));
		s.o(p != nullptr);
		if (p)
			static_cast<IUnknown*>(p)->Release();
		p = reinterpret_cast<void*>(uintptr_t(1));
		const GUID other = { 0x12345678, 0x1234, 0x5678, { 1, 2, 3, 4, 5, 6, 7, 8 } };
		s.o(dev->QueryInterface(other, &p));
		s.o(p == nullptr);
		s.o(dev->QueryInterface(iidDevice8, nullptr));
		IDirect3D8* d3d = nullptr;
		IDirect3D8* d3d2 = nullptr;
		s.o(dev->GetDirect3D(&d3d));
		s.o(dev->GetDirect3D(&d3d2));
		s.o(d3d != nullptr);
		s.o(d3d == d3d2);   // the parent's identity is stable (its address is not observed)
		if (d3d)
			d3d->Release();
		if (d3d2)
			d3d2->Release();
		s.o(dev->GetDirect3D(nullptr));
	}
	// ---- the inert / never-called APIs, with arguments (outputs pre-filled: untouched must stay so)
	{
		DWORD tok = 0x5a5a5a5au;
		s.o(dev->BeginStateBlock());	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->EndStateBlock(&tok));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(tok);
		s.o(dev->EndStateBlock(nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->ApplyStateBlock(0x55));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->CaptureStateBlock(0x55));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->DeleteStateBlock(0x55));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		for (D3DSTATEBLOCKTYPE t : { D3DSBT_ALL, D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE, static_cast<D3DSTATEBLOCKTYPE>(9) }) {
			tok = 0x5a5a5a5au;
			s.o(dev->CreateStateBlock(t, &tok));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
			s.o(tok);
		}
		s.o(dev->CreateStateBlock(D3DSBT_ALL, nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		D3DCLIPSTATUS8 cs = { 0x11, 0x22 };
		s.o(dev->SetClipStatus(&cs));
		s.o(dev->SetClipStatus(nullptr));
		std::memset(&cs, 0x5a, sizeof(cs));
		s.o(dev->GetClipStatus(&cs));
		s.o(cs);
		s.o(dev->GetClipStatus(nullptr));
		uint8_t buf[64];
		std::memset(buf, 0x5a, sizeof(buf));
		s.o(dev->GetInfo(1, buf, 16));
		s.o(buf);   // right away (r11-oracle: it was overwritten before being observed)
		s.o(dev->GetInfo(0, nullptr, 0));
		PALETTEENTRY pal[256];
		for (int i = 0; i < 256; ++i)
			pal[i] = { BYTE(i), BYTE(255 - i), BYTE(i * 3), 255 };
		s.o(dev->SetPaletteEntries(3, pal));
		s.o(dev->SetPaletteEntries(0, nullptr));
		std::memset(pal, 0x5a, sizeof(pal));
		s.o(dev->GetPaletteEntries(3, pal));
		s.o(pal);
		s.o(dev->SetCurrentTexturePalette(2));
		UINT cp = 0x5a5a;
		s.o(dev->GetCurrentTexturePalette(&cp));
		s.o(cp);
		s.o(dev->GetCurrentTexturePalette(nullptr));
		IDirect3DVertexBuffer8* vb = nullptr;
		s.o(dev->CreateVertexBuffer(256, 0, kCV, D3DPOOL_SYSTEMMEM, &vb));
		s.o(dev->ProcessVertices(1, 2, 3, vb, 0));
		s.o(dev->ProcessVertices(0, 0, 0, nullptr, 1));
		if (vb)
			vb->Release();
		std::memset(buf, 0x5a, sizeof(buf));
		s.o(dev->GetVertexShaderConstant(4, buf, 2));
		s.o(dev->GetPixelShaderConstant(1, buf, 1));
		DWORD vs = 0, ps = 0;
		s.o(dev->CreateVertexShader(vsDecl, vsCode, &vs, 0));
		s.o(dev->CreatePixelShader(psCode, &ps));
		DWORD sz = 64;
		s.o(dev->GetVertexShaderDeclaration(vs, buf, &sz));
		s.o(sz);
		s.o(dev->GetVertexShaderDeclaration(vs, nullptr, &sz));
		s.o(sz);
		s.o(dev->GetVertexShaderFunction(vs, buf, &sz));
		s.o(sz);
		s.o(dev->GetPixelShaderFunction(ps, buf, &sz));
		s.o(sz);
		s.o(buf);
		DWORD h = 0x5a5a;
		s.o(dev->GetVertexShader(&h));
		s.o(h);
		h = 0x5a5a;
		s.o(dev->GetPixelShader(&h));
		s.o(h);
		s.o(dev->DeleteVertexShader(vs));
		s.o(dev->DeletePixelShader(ps));
		IDirect3DSurface8* fb = nullptr;
		s.o(dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &fb));
		s.o(dev->GetFrontBuffer(fb));
		s.o(dev->GetFrontBuffer(nullptr));
		if (fb)
			fb->Release();
		const float segs[4] = { 2, 3, 4, 5 };
		D3DRECTPATCH_INFO rp = { 0, 0, 4, 4, 4, D3DBASIS_BEZIER, D3DORDER_CUBIC };
		D3DTRIPATCH_INFO tp = { 0, 10, D3DBASIS_BEZIER, D3DORDER_CUBIC };
		s.o(dev->DrawRectPatch(1, segs, &rp));
		s.o(dev->DrawRectPatch(0, nullptr, nullptr));
		s.o(dev->DrawTriPatch(2, segs, &tp));
		s.o(dev->DeletePatch(3));
		s.o(dev->DeletePatch(0));
	}
	// ---- null-pointer validation on the supported methods
	{
		s.o(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, nullptr));
		s.o(dev->CreateVertexBuffer(64, 0, kCV, D3DPOOL_MANAGED, nullptr));
		s.o(dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, nullptr));
		s.o(dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, nullptr));
		s.o(dev->CreateDepthStencilSurface(16, 16, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, nullptr));
		s.o(dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, nullptr));
		s.o(dev->CreateVertexShader(vsDecl, vsCode, nullptr, 0));
		s.o(dev->CreateVertexShader(nullptr, vsCode, nullptr, 0));
		s.o(dev->CreatePixelShader(psCode, nullptr));
		s.o(dev->CreatePixelShader(nullptr, nullptr));
		s.o(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, nullptr));
		s.o(dev->CreateVolumeTexture(8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, nullptr));
		s.o(dev->SetTransform(D3DTS_WORLD, nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->MultiplyTransform(D3DTS_WORLD, nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->GetTransform(D3DTS_WORLD, nullptr));
		s.o(dev->SetViewport(nullptr));
		s.o(dev->GetViewport(nullptr));
		s.o(dev->SetMaterial(nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->GetMaterial(nullptr));
		s.o(dev->SetLight(0, nullptr));	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		s.o(dev->GetLight(0, nullptr));
		s.o(dev->GetLightEnable(0, nullptr));
		s.o(dev->SetClipPlane(0, nullptr));
		s.o(dev->GetClipPlane(0, nullptr));
		s.o(dev->GetRenderState(D3DRS_ZENABLE, nullptr));
		s.o(dev->GetTextureStageState(0, D3DTSS_COLOROP, nullptr));
		s.o(dev->GetTexture(0, nullptr));
		s.o(dev->GetDeviceCaps(nullptr));
		// GetDisplayMode(nullptr): not swept - the device forwards it to its IDirect3D8 unchanged (A23-B splits nothing there) and the
		// test's stub IDirect3D8 dereferences it (measured 13/10: SIGSEGV in StubD3D8::GetAdapterDisplayMode, both CS modes)
		s.o(dev->GetCreationParameters(nullptr));
		s.o(dev->GetDepthStencilSurface(nullptr));
		s.o(dev->GetRenderTarget(nullptr));
		s.o(dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, nullptr));
		s.o(dev->GetStreamSource(0, nullptr, nullptr));
		s.o(dev->GetIndices(nullptr, nullptr));
		s.o(dev->ValidateDevice(nullptr));
		s.o(dev->GetVertexShader(nullptr));
		s.o(dev->GetPixelShader(nullptr));
		s.o(dev->GetRasterStatus(nullptr));
		s.o(dev->CreateAdditionalSwapChain(nullptr, nullptr));
		s.o(dev->Reset(nullptr));
		s.o(dev->SetVertexShaderConstant(0, nullptr, 0));
	}
	// ---- queries (deterministic fields only)
	{
		D3DCAPS8 caps;
		std::memset(&caps, 0x5a, sizeof(caps));
		s.o(dev->GetDeviceCaps(&caps));
		s.o(caps);
		D3DDISPLAYMODE dm;
		std::memset(&dm, 0x5a, sizeof(dm));
		s.o(dev->GetDisplayMode(&dm));
		s.o(dm.Format);
		D3DDEVICE_CREATION_PARAMETERS cp = {};
		s.o(dev->GetCreationParameters(&cp));
		s.o(cp.AdapterOrdinal);
		s.o(cp.DeviceType);
		s.o(cp.BehaviorFlags);
		DWORD passes = 0x5a;
		s.o(dev->ValidateDevice(&passes));
		s.o(passes);
		s.o(dev->TestCooperativeLevel());
		D3DRASTER_STATUS rs = {};
		s.o(dev->GetRasterStatus(&rs));
		(void)dev->GetAvailableTextureMem();   // free memory: called, never observed
	}
	// ---- cube / volume textures, an additional swap chain
	{
		IDirect3DCubeTexture8* c = nullptr;
		s.o(dev->CreateCubeTexture(16, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &c));
		if (c)
			c->Release();
		c = nullptr;
		s.o(dev->CreateCubeTexture(8, 0, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c));
		if (c)
			c->Release();
		IDirect3DVolumeTexture8* v = nullptr;
		s.o(dev->CreateVolumeTexture(8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &v));
		if (v)
			v->Release();
		D3DPRESENT_PARAMETERS pp = {};
		pp.BackBufferWidth = 32;
		pp.BackBufferHeight = 32;
		pp.BackBufferFormat = D3DFMT_A8R8G8B8;
		pp.BackBufferCount = 1;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.Windowed = TRUE;
		IDirect3DSwapChain8* sc = nullptr;
		s.o(dev->CreateAdditionalSwapChain(&pp, &sc));
		if (sc)
			sc->Release();
	}
	// ---- Reset, LAST: alternatives, each followed by TestCooperativeLevel, a draw and a read-back when it took
	auto base = [] {
		D3DPRESENT_PARAMETERS pp = {};
		pp.BackBufferWidth = 64;
		pp.BackBufferHeight = 64;
		pp.BackBufferFormat = D3DFMT_A8R8G8B8;
		pp.BackBufferCount = 2;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.Windowed = TRUE;
		pp.EnableAutoDepthStencil = TRUE;
		pp.AutoDepthStencilFormat = D3DFMT_D24S8;
		return pp;
	};
	auto tryReset = [&](D3DPRESENT_PARAMETERS pp) {
		if (s.bb)
			s.bb->Release();
		s.bb = nullptr;
		s.dropImg();
		const HRESULT h = dev->Reset(&pp);
		s.o(h);
		s.o(dev->TestCooperativeLevel());
		if (SUCCEEDED(h) && s.getBB()) {
			ffBase(s, nullptr);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			clearAll(s, 0xff405060);
			drawQuad(s);
			readBack(s);
		}
	};
	{
		D3DPRESENT_PARAMETERS pp = base();
		pp.Flags = 1;   // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
		pp.AutoDepthStencilFormat = D3DFMT_D16;
		pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
		tryReset(pp);
		pp = base();
		pp.BackBufferWidth = 48;
		pp.BackBufferHeight = 32;
		pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.BackBufferCount = 3;
		tryReset(pp);
		pp = base();
		pp.Windowed = FALSE;
		pp.FullScreen_RefreshRateInHz = 60;
		pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		tryReset(pp);   // fullscreen: refused (the native device's named divergence S4)
		tryReset(base());
		pp = base();
		pp.BackBufferFormat = D3DFMT_R5G6B5;
		tryReset(pp);   // a back-buffer format the native device cannot build: refused
		tryReset(base());
		pp = base();
		pp.EnableAutoDepthStencil = FALSE;
		pp.SwapEffect = D3DSWAPEFFECT_COPY;
		pp.BackBufferCount = 1;
		tryReset(pp);
		tryReset(base());
		// r11-oracle remainder: zero (automatic) extents and count, an out-of-range count, FLIP / COPY_VSYNC, a windowed interval
		pp = base();
		pp.BackBufferWidth = pp.BackBufferHeight = 0;
		pp.BackBufferCount = 0;
		tryReset(pp);
		pp = base();
		pp.BackBufferCount = 4;
		tryReset(pp);
		pp = base();
		pp.SwapEffect = D3DSWAPEFFECT_FLIP;
		tryReset(pp);
		pp = base();
		pp.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;
		pp.BackBufferCount = 1;
		tryReset(pp);
		pp = base();
		pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
		tryReset(pp);
		tryReset(base());
	}
	s.end();
	return s.ok;
}

} // namespace gxsweep
