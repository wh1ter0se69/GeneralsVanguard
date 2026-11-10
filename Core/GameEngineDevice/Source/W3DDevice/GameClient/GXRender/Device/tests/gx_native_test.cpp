// GeneralsX: gx_native_test: the native d3d8 device (GxNativeDevice) driven ONLY through the d3d8
// interfaces, offscreen (no window), on a stub IDirect3D8 (the device forwards caps/identity queries to its parent; the real
// parent is DXVK's interface, exercised by gxcap-replay --native). One case per behaviour of DESIGN_l1-device.md, each with an
// X-<case> arm that turns on one GxNativeDebug plant in the PRODUCT device and must make exactly that case fail.
//
//   gx_native_test [--arm NAME[,NAME...]|all] [--no-validation] [--device SEL]
// The present / present-msaa arms open a HIDDEN SDL3 window (the game's WSI: VkWsi.h's SDL3 host functions) and present
// through a real swapchain; they need an SDL3 build (GX_VKCTX_HAS_SDL3) and are skipped otherwise.
#include "GxNativeDevice.h"
#include "GxSyntheticPrograms.h"
#include "GxDxvkMath.h"
#include "GxNativeCs.h"
#include "GxNativeConfig.h"
#include "GxDecode.h"
#include "GxOrderCheck.h"
#include "gx_exec_digest.h"
#include "gx_sweep_arms.h"
#include "GxCustomCatalog.h"
#include "../../../../../../Include/W3DDevice/GameClient/GxShadowVolumeVS.h"
#include "../../../../../../Include/W3DDevice/GameClient/GxShadowGpuProj.h"   // R-DL6   // the bytes the game creates (rw3-R11)
#if defined(GX_VKCTX_HAS_SDL3)
#include "VkWsi.h"
#include <SDL3/SDL.h>
#endif

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>
#if defined(_WIN32)   // MSVC has no setenv / unsetenv (as gx_proxy_test's setEnvVar): an empty value removes the variable
static int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#if defined(__APPLE__)
#include <mach/mach.h>   // runSelf: a planted child's exception ports
#endif
#endif

using namespace GXRender::Dev;

namespace {

void say(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

// The parent interface: what the device may ask its IDirect3D8 (DXVK's in the game).
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
		c->VertexShaderVersion = D3DVS_VERSION(1, 1);   // DXVK d3d8's answer (rw3-R11: the device reports min(this, 96))
		c->MaxVertexShaderConst = 256;
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

// Every device log line of the current arm (still echoed to stderr, as the product's null log does). Validation messages arrive
// from the pipeline workers' threads too: every access holds g_logLock.
std::mutex g_logLock;
std::vector<std::string> g_log;
namespace exitobs {   // A23-B arm 16's post-line observer (defined with the arm)
extern std::atomic<bool> armed, afterFinal;
extern std::atomic<uint64_t> logs;
int watched();
}
void captureLog(const char* line)
{
	if (exitobs::afterFinal.load() && exitobs::watched() >= 0)
		++exitobs::logs;
	{
		std::lock_guard<std::mutex> g(g_logLock);
		g_log.emplace_back(line);
	}
	std::fprintf(stderr, "%s\n", line);
	if (exitobs::armed.load() && std::strstr(line, "[GXNATIVE] stats final"))
		exitobs::afterFinal = true;
}
std::vector<std::string> logSnapshot()
{
	std::lock_guard<std::mutex> g(g_logLock);
	return g_log;
}

// A23-B arm 10: every window arm's drawable-size callback goes through this - it counts calls made off the main thread
std::thread::id g_mainThread;
std::atomic<int> g_drawableOffMain { 0 };
#if defined(GX_VKCTX_HAS_SDL3)
void testDrawableSize(void* window, uint32_t* w, uint32_t* h)
{
	if (std::this_thread::get_id() != g_mainThread)
		++g_drawableOffMain;
	GXRender::Vk::sdl3DrawableSize(window, w, h);
}
#endif

struct Options
{
	bool validation = true;
	const char* device = nullptr;
	bool serial = false;   // A23-B --strict: the SERIAL schedule for every device the arm makes
	uint32_t fif = 2;   // --fif N (PREREG_T A30 step 2): every case's device runs N frames in flight
};
uint32_t g_fif = 2;   // the run's --fif, for the cases that make their own devices

struct Env
{
	IDirect3DDevice8* dev = nullptr;
	GxNativeDevice* nd = nullptr;
	void* window = nullptr;     // the hidden SDL3 window of a present arm
	bool devReleased = false;   // the case released the game's device reference itself
	bool keepWindow = false;    // the case leaks the window too: the process exits with it alive (exit-quiesce, as in the game)
	bool setupFailed = false;   // the case could not reach its gate (no detection, never a pass)
	bool a25Symptom = false, a25Other = false;   // A25 cases: a failure that is exactly the plant's stale value / any other failure
};

// A23-B: --strict runs ONE call sequence in both modes, so the CS=1 contract branches (rw3-X/CS1_ARM_CONTRACT.md) are off there
bool g_strictRun = false;
// E · look-shadowmaps-9 (S7 fold, integrator 02:1xZ 19/10: "every X- test-side - an existing TEST-ONLY switch or a BROKEN INPUT"): these
// arms' X- runs the case with g_brokenInput set - the case breaks its OWN input (no product plant); `--arm all` generates their X- too
bool g_brokenInput = false;
const char* const kBrokenInputArms[] = { "sm-castonly-noscope", "sm-cast-ordinary", "sm-step-compile", "sm-vel-rigid", "sm-vel-skin",
	"sm-vel-occluded", "sm-replay4", "sm-replay4-nodraw" };
bool brokenInputArm(const char* n)
{
	for (const char* a : kBrokenInputArms)
		if (std::strcmp(n, a) == 0)
			return true;
	return false;
}
const char* const kCsContractArms[] = { "present-surface-lost", "present-surface-lost-shown" /* its second plant */,
	"present-surface-lost-copy" /* its third (r2-oracle P2-1) */, "present-surface-lost-mirror" /* its fourth (r3-oracle P2-1) */,
	"present-surface-lost-clip" /* its fifth (r4-oracle P2-1) */, "present-surface-lost-linear" /* its sixth (r5-oracle P2-1) */, "present-submit-fail", "present-retry-frame", "present-side-lost",
	"present-error", "thread-present-skipped", "thread-present-deferred", "device-lost-mid-present", "stats-line" };
bool csBranch(Env& e) { return e.nd->csOn() && !g_strictRun; }
// PREREG_T A23-B "STRICT vs ASYNC" (registered before any data, by arm number; here by name): judged by pixels and named semantics
// only - the strict runners print them as ASYNC and compare no digest. Everything else (the existing arms, 6w cs-up-wrap, 21
// cs-exec-ledger, 24 cs-flush-front, 24b cs-flush-worker, 30 cs-create-fails, the strict-* plant arms) is STRICT.
const char* const kAsyncArms[] = { "cs-getter" /* 1 */, "cs-discard" /* 2 */, "cs-discard-gpu" /* 2g */, "cs-nooverwrite" /* 3 */,
	"cs-static-unlock" /* 4 */, "cs-tex-unlock" /* 5 */, "cs-up" /* 6 */, "cs-up-notarget" /* A-up-notarget */, "cs-copy-late" /* 7 */,
	"cs-lifetime" /* 8 */, "cs-lead" /* 9 */, "cs-drawable-thread" /* 10 */, "cs-reset-drain" /* 11 */, "cs-loss" /* 12 */,
	"cs-lost-fenced" /* 12b */, "cs-wsi-owner" /* 13 */, "cs-ring" /* 14 */, "cs-wake" /* A55 */, "cs-wake-target" /* A55 */, "cs-oversize" /* 15 */, "thread-cs-exit-midframe" /* 16 */,
	"cs-create-retry" /* 17 */, "cs-create-retry-count" /* 17 */, "cs-reconcile" /* 18 */, "cs-rt-view-fail" /* 19 */,
	"cs-rt-view-retire" /* 19 */, "cs-discard-pressure" /* 20 */, "cs-exact-transform" /* 22 */, "cs-exact-clear" /* 22 */,
	"cs-exact-up0" /* 22 */, "cs-reclaim" /* 23 */, "cs-reclaim-identity" /* 23 extended */, "cs-reclaim-busy" /* 23b */,
	"cs-ring-full" /* 25 */, "cs-site-classes" /* 27 */, "cs-worker-recover-lead" /* 28a */, "cs-worker-recover-room" /* 28b */,
	"cs-worker-selfdrain" /* 28a X */, "cs-reordered-failure" /* 29 */, "cs-reordered-failure-b" /* 29b */, "cs-sbody-alloc" /* 31a */,
	"cs-terminal-count" /* 31b */, "cs-reorder" /* the hold form of X-cs-reorder (its STRICT form: strict-reorder) */,
	"cs-tsan-probe" /* the TSan lane */, "cs-mirror-check" /* the mirror-check mode */,
	/* added after, before any of their data: */ "cs-constants" /* payload at the call, worker held (as 4-6) */,
	"cs-const-bounds" /* a guard-page fault in a child (as 22) */, "cs-wait-publish" /* a hang in a child (as 28a X) */,
	/* after, before their data: */ "cs-vs-payload" /* payload at the call, worker held (as 4-6) */,
	"cs-slice-terminal" /* a terminal allocation failure counted (as 31b) */,
	/* after, before its data: */ "thread-cs-recover-latched" /* recovery after a latched refusal (holds gx-vk-submit) */,
	/* after: */ "cs-ps-payload" /* payload at the call, worker held (as cs-vs-payload) */,
	/* after: */ "cs-ps-bind" /* its binding twin (worker held at creation) */, "thread-cs-recover-latched-worker" /* holds gx-vk-submit */,
	/* the integrator's exit condition: */ "cs-exit-crt" /* a normal (CRT) exit in a child (as 16; no thread- prefix: it runs with the submit thread off too) */, "cs-exit-crt-nowin" /* the same, windowless (rw3-X-3) */,
	"cs-exit-stuck" /* A60: a stuck worker at the CRT exit - the watchdog (child) */, "cs-exit-slow" /* A60: a slow but progressing one (child) */,
	/* after: */ "cs-ps-draw", "cs-vs-bind" /* the drawing twins of cs-ps-payload / cs-vs-payload (worker held) */,
	/* after: */ "cs-ps-bind-late" /* the same arm, its late-bind plant */,
	/* after: */ "cs-ps-delete-bound", "cs-vs-delete-bound" /* the same arms, their delete-while-bound plant */,
	/* after: */ "cs-clear-rects" /* payload at the call, worker held (as 4-6) */, "cs-vs-delete-exec" /* as cs-vs-payload */,
	/* after: */ "cs-ps-delete-exec" /* as cs-ps-payload */,
	/* rw3-X-3, after (amendment 5): */ "cs-ff-payload-material", "cs-ff-payload-light", "cs-ff-payload-viewport" /* as cs-constants */,
	/* rw3-X-3, after: */ "cs-deep-clear" /* a stack overflow in a child (as 22) */,
	/* rw3-X-3, after: */ "cs-shader-oversize-order" /* worker held, an order sink (as cs-constants) */,
	/* rw3-X-3, the same property for every other oversize S branch: */ "cs-oversize-order" /* worker held, an order sink */,
	/* rw3-X-3, after: */ "cs-copy-readback-s", "cs-managed-restore-write" /* worker held (as cs-constants) */,
	"cs-host-copy-refused" /* a front-decided refusal: no exec half at CS=1 by design (judged by HRESULT and contents) */,
	/* rw3-X-3, after: */ "cs-host-copy-refused-count" /* the same arm, its counter plant (judged by the counters too) */,
	/* rw3-X-3 X': */ "cs-getdesc-nodrain" /* worker held (as cs-constants) */, "cs-getdesc-race" /* a race schedule, the TSan lane */, "cs-drain-attribution" /* worker held (r20-core P2-1) */, "cs-drain-attribution-s" /* the same, its SScope plant (r21-core P2-1) */ };
bool isAsyncArm(const std::string& a)
{
	return std::find_if(std::begin(kAsyncArms), std::end(kAsyncArms), [&](const char* n) { return a == n; }) != std::end(kAsyncArms);
}
// rw3-X-3 (A23-B merge gate item 3, PREREG A23-B amendment 5): the TRANSLATOR-INPUT plants, one per class - each makes the exec
// half hand the translator something other than what it traced. --strict's plant pass requires EXEC + ledger EQUAL and the trin
// digest DIFFERENT for these (by name): scalar, object, payload, dropped call.
const char* const kTrinPlants[] = { "strict-light-off" /* scalar: LightEnable TRUE forwarded */, "strict-indexed-start" /* scalar */,
	"strict-indexed-base" /* scalar */, "strict-bb-rotation" /* object: a stale back buffer bound */, "strict-vs-code" /* payload */,
	"strict-light-rs" /* dropped call */ };
bool isTrinPlant(const std::string& a)
{
	return std::find_if(std::begin(kTrinPlants), std::end(kTrinPlants), [&](const char* n) { return a == n; }) != std::end(kTrinPlants);
}
// rw3-X-3 (PREREG A23-B am. 5 (15), registered AFTER gate fe908b581 measured it): STRICT plants the A22 command tap cannot see - the
// planted run records the same commands, arguments, descriptors and creations; the plant changes only bytes written into staging
// or a creation's HRESULT, which the tap does not digest (gx_cmd_tap.cpp). --strict detects each; --strict-tap's plant pass skips them.
// Measured: planted == clean tap, same command count (gate fe908b581 both submit modes; run/t66 for -ro / -plain: 340 commands each).
const char* const kTapBlindPlants[] = { "strict-shader-oversize" /* a refused creation, never drawn */,
	"strict-buffer-ranges-plain" /* the slice's bytes, not its binding */,
	"strict-buffer-ranges-ro" /* staging bytes counted; the range is rewritten and the buffer released before the next recording */,
	"strict-buffer-ranges-size0" /* a dynamic lock that fails: the mapped bytes are never written */,
	"strict-texture-partial" /* the staged rows' pitch: bytes, not copy regions */ };
bool isTapBlindPlant(const std::string& a)
{
	return std::find_if(std::begin(kTapBlindPlants), std::end(kTapBlindPlants), [&](const char* n) { return a == n; }) !=
		std::end(kTapBlindPlants);
}
// --strict's digests: the EXEC sink and the translator-input sink are one ExecDigest
void setSinks(ExecDigest* d)
{
	GXRender::Dev::g_csExecSink.store(d);
	GXRender::Rec::g_trInputSink.store(d);
}

#define CHECK(cond, what) do { if (!(cond)) { *d += std::string(" ") + (what); ok = false; } } while (0)
// a case's starting state not reached: no gate ran - the runner makes the verdict WRONG for a clean AND a fail arm (rw3V-code-r1 P2-5)
bool g_setupFailed = false;
#define SETUP(cond, what) do { if (!(cond)) { *d += std::string(" SETUP FAILED: ") + (what); g_setupFailed = true; return false; } } while (0)

D3DPRESENT_PARAMETERS presentParams(bool windowed, UINT backBuffers, D3DMULTISAMPLE_TYPE msaa = D3DMULTISAMPLE_NONE)
{
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	pp.BackBufferCount = backBuffers;
	pp.MultiSampleType = msaa;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.Windowed = windowed;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	return pp;
}

// Reads one pixel of a render-target-like surface through the game's own path: CreateImageSurface + CopyRects + LockRect.
bool readPixel(IDirect3DDevice8* dev, IDirect3DSurface8* src, UINT x, UINT y, uint32_t* out)
{
	D3DSURFACE_DESC sd;
	if (FAILED(src->GetDesc(&sd)))
		return false;
	IDirect3DSurface8* img = nullptr;
	if (FAILED(dev->CreateImageSurface(sd.Width, sd.Height, sd.Format, &img)))
		return false;
	bool ok = SUCCEEDED(dev->CopyRects(src, nullptr, 0, img, nullptr));
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		std::memcpy(out, static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch + x * 4, 4);
		img->UnlockRect();
	} else
		ok = false;
	img->Release();
	return ok;
}

bool clearSurface(IDirect3DDevice8* dev, IDirect3DSurface8* rt, D3DCOLOR c)
{
	IDirect3DSurface8* ds = nullptr;
	dev->GetDepthStencilSurface(&ds);
	const bool ok = SUCCEEDED(dev->SetRenderTarget(rt, nullptr)) && SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET, c, 1.f, 0));
	if (ds)
		ds->Release();
	return ok;
}

// ------------------------------------------------------------------------------------------------------------ cases

// DESIGN §8.2 / d3d9_swapchain.cpp:925-939 + :248-258: windowed DISCARD with ONE back buffer does not swap with the front buffer
// (no rotation); two back buffers fullscreen rotate three storages under stable surface identities.
bool caseRotation(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8 *bb0 = nullptr, *bb1 = nullptr;
	CHECK(SUCCEEDED(e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb0)) && bb0, "GetBackBuffer(0)");
	CHECK(SUCCEEDED(e.dev->GetBackBuffer(1, D3DBACKBUFFER_TYPE_MONO, &bb1)) && bb1, "GetBackBuffer(1)");
	if (!ok)
		return false;
	CHECK(clearSurface(e.dev, bb0, 0xff102030) && clearSurface(e.dev, bb1, 0xff405060), "clears");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	IDirect3DSurface8* again = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &again);
	CHECK(again == bb0, "back buffer 0 identity changed across Present");
	if (again)
		again->Release();
	uint32_t px = 0;
	CHECK(readPixel(e.dev, bb0, 1, 1, &px), "read bb0");
	CHECK((px & 0xffffff) == 0x405060, ("bb0 after Present shows " + std::to_string(px & 0xffffff) + ", want buffer 1's 0x405060").c_str());
	bb0->Release();
	bb1->Release();
	return ok;
}

// DESIGN §3: the device holds a private reference on what is bound - a texture the game Releases while bound lives until
// unbound; then it is destroyed.
bool caseBoundRef(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	if (!t)
		return false;
	const uint64_t live = e.nd->stats().liveTextures;
	e.dev->SetTexture(0, t);
	CHECK(t->Release() == 0, "public refcount after Release");
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);   // a device entry point: queued destructions run there (DESIGN §9)
	CHECK(e.nd->stats().liveTextures == live, "texture destroyed while bound");
	e.dev->SetTexture(0, nullptr);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().liveTextures == live - 1, "texture not destroyed after unbind");
	return ok;
}

// DESIGN §3 / d3d8_subresource.h: a level surface keeps its container alive.
bool caseLevelSurface(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	if (!t)
		return false;
	const uint64_t live = e.nd->stats().liveTextures;
	IDirect3DSurface8* s = nullptr;
	CHECK(SUCCEEDED(t->GetSurfaceLevel(1, &s)) && s, "GetSurfaceLevel");
	t->Release();
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);   // queued destructions run here (DESIGN §9)
	if (e.nd->stats().liveTextures != live) {   // destroyed under a held surface: never touch s again (it dangles)
		*d += " texture destroyed while its level surface is held";
		return false;
	}
	D3DSURFACE_DESC sd = {};
	CHECK(s && SUCCEEDED(s->GetDesc(&sd)) && sd.Width == 4 && sd.Height == 4, "level 1 desc");
	if (s)
		s->Release();
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().liveTextures == live - 1, "texture not destroyed after the surface's Release");
	return ok;
}

// DESIGN §7: the CopyRects rows the game uses. (a) SYSTEMMEM image -> MANAGED texture level (text pages) -> back to SYSTEMMEM;
// (b) a cleared render target -> SYSTEMMEM (GX_SHOT); (c) an MSAA render target -> RT texture (pillarbox resolve) -> SYSTEMMEM;
// (d) render target -> RT texture, a sub-rect at a destination point (smudge).
bool caseCopyRects(Env& e, std::string* d)
{
	bool ok = true;
	// (a)
	IDirect3DSurface8* img = nullptr;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8* lvl = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateImageSurface(8, 8, D3DFMT_A8R8G8B8, &img)), "image surface");
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)), "managed texture");
	if (!ok)
		return false;
	tex->GetSurfaceLevel(0, &lvl);
	D3DLOCKED_RECT lr = {};
	img->LockRect(&lr, nullptr, 0);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff000000u | uint32_t(y << 8 | x);
	img->UnlockRect();
	CHECK(SUCCEEDED(e.dev->CopyRects(img, nullptr, 0, lvl, nullptr)), "(a) image -> managed");
	uint32_t px = 0;
	CHECK(readPixel(e.dev, lvl, 5, 3, &px) && px == (0xff000000u | 3 << 8 | 5), "(a) managed level content");
	lvl->Release();
	tex->Release();
	img->Release();
	// (b)
	IDirect3DSurface8* rt = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &rt)), "render target");
	if (rt) {
		CHECK(clearSurface(e.dev, rt, 0xff336699) && readPixel(e.dev, rt, 7, 7, &px) && (px & 0xffffff) == 0x336699, "(b) RT readback");
	}
	// (c)
	IDirect3DSurface8* msaa = nullptr;
	IDirect3DTexture8* rtTex = nullptr;
	IDirect3DSurface8* rtLvl = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &msaa)), "MSAA RT");
	CHECK(SUCCEEDED(e.dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex)), "RT texture");
	if (msaa && rtTex) {
		rtTex->GetSurfaceLevel(0, &rtLvl);
		CHECK(clearSurface(e.dev, msaa, 0xffaa5511), "(c) clear MSAA");
		CHECK(SUCCEEDED(e.dev->CopyRects(msaa, nullptr, 0, rtLvl, nullptr)), "(c) resolve");
		CHECK(readPixel(e.dev, rtLvl, 3, 9, &px) && (px & 0xffffff) == 0xaa5511, "(c) resolved content");
	}
	// (d)
	if (rt && rtLvl) {
		CHECK(clearSurface(e.dev, rtLvl, 0xff000000) && clearSurface(e.dev, rt, 0xff00ff00), "(d) clears");
		RECT src = { 2, 2, 6, 6 };
		POINT dst = { 10, 10 };
		CHECK(SUCCEEDED(e.dev->CopyRects(rt, &src, 1, rtLvl, &dst)), "(d) sub-rect copy");
		uint32_t in = 0, out = 0;
		CHECK(readPixel(e.dev, rtLvl, 11, 11, &in) && (in & 0xffffff) == 0x00ff00, "(d) copied pixel");
		CHECK(readPixel(e.dev, rtLvl, 5, 5, &out) && (out & 0xffffff) == 0, "(d) pixel outside the destination rect");
	}
	for (IUnknown* u : { static_cast<IUnknown*>(rt), static_cast<IUnknown*>(msaa), static_cast<IUnknown*>(rtLvl),
		     static_cast<IUnknown*>(rtTex) })
		if (u)
			u->Release();
	return ok;
}

// DESIGN §6: a MANAGED texture drops its host copy at the Present after it was filled; a later lock restores it from the GPU
// and pins the texture (no second drop).
bool caseManagedDrop(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	if (!t)
		return false;
	D3DLOCKED_RECT lr = {};
	t->LockRect(0, &lr, nullptr, 0);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff000000u | uint32_t(x * 16 + y);
	t->UnlockRect(0);
	const GxNativeStats before = e.nd->stats();
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().dropped == before.dropped + 1, "not dropped at Present");
	CHECK(SUCCEEDED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)), "relock");
	const uint32_t v = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 7 * lr.Pitch)[9];
	t->UnlockRect(0);
	CHECK(v == (0xff000000u | (9 * 16 + 7)), "restored content");
	CHECK(e.nd->stats().restores == before.restores + 1, "restore not counted");
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().dropped == before.dropped + 1, "pinned texture dropped again");
	t->Release();
	return ok;
}

// DESIGN §4: a frame drawn through the device: the FF quad's colour lands in the back buffer.
bool caseDraw(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear back buffer");
	struct V { float x, y, z, rhw; DWORD c; };
	const V q[4] = { { 0, 0, 0.5f, 1, 0xffcc4422 }, { 64, 0, 0.5f, 1, 0xffcc4422 }, { 0, 64, 0.5f, 1, 0xffcc4422 },
		{ 64, 64, 0.5f, 1, 0xffcc4422 } };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))), "DrawPrimitiveUP");
	CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	uint32_t px = 0;
	CHECK(bb && readPixel(e.dev, bb, 20, 30, &px) && (px & 0xffffff) == 0xcc4422,
		("drawn pixel " + std::to_string(px & 0xffffff)).c_str());
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// world-only (PREREG_T A22 amendment 2: a native arm the A22 plant must move): two FF draws of one quad (D3DFVF_XYZ, identity view
// and projection) whose only difference is SetTransform(WORLD) - the translator reuses its pack and refreshes the world part alone
// (GxPackFFWorld), the path A22's VS-block generation skips. Both quads must land: x -0.5 and +0.5 of clip space, pixels 16 and 48.
// With GX_A22_PLANT=1 the second draw keeps the first one's world (its generation never moved), so pixel 48 stays black.
bool caseWorldOnly(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear back buffer");
	struct V { float x, y, z; DWORD c; };
	const V q[4] = { { -0.25f, -0.25f, 0.5f, 0xff22aa66 }, { 0.25f, -0.25f, 0.5f, 0xff22aa66 }, { -0.25f, 0.25f, 0.5f, 0xff22aa66 },
		{ 0.25f, 0.25f, 0.5f, 0xff22aa66 } };
	D3DMATRIX m = {};
	m._11 = m._22 = m._33 = m._44 = 1.f;
	e.dev->SetTransform(D3DTS_VIEW, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &m);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	for (float x : { -0.5f, 0.5f }) {
		m._41 = x;
		e.dev->SetTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))), "DrawPrimitiveUP");
	}
	CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	for (UINT x : { 16u, 48u }) {
		uint32_t px = 0;
		CHECK(bb && readPixel(e.dev, bb, x, 32, &px) && (px & 0xffffff) == 0x22aa66,
			("pixel " + std::to_string(x) + ",32 = " + std::to_string(px & 0xffffff)).c_str());
	}
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// DESIGN §6 (GxBuffer = DXVK LockBuffer): a dynamic VB locked DISCARD between two draws of one frame gives the second draw new
// storage - the first draw still reads its own vertices when the frame executes; NOOVERWRITE appends into the current storage
// without disturbing the draw before it. Three quads, three colours, one frame.
bool caseBufferLocks(Env& e, std::string* d)
{
	bool ok = true;
	struct V { float x, y, z, rhw; DWORD c; };
	auto quad = [](V* v, float x0, float y0, float x1, float y1, DWORD c) {
		const V q[4] = { { x0, y0, 0.5f, 1, c }, { x1, y0, 0.5f, 1, c }, { x0, y1, 0.5f, 1, c }, { x1, y1, 0.5f, 1, c } };
		std::memcpy(v, q, sizeof(q));
	};
	IDirect3DVertexBuffer8* vb = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(8 * sizeof(V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "CreateVertexBuffer dynamic");
	if (!vb)
		return false;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear back buffer");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	e.dev->SetStreamSource(0, vb, sizeof(V));
	e.dev->BeginScene();
	BYTE* p = nullptr;
	CHECK(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "lock DISCARD 1");
	if (p) {
		quad(reinterpret_cast<V*>(p), 0, 0, 32, 32, 0xff112233);
		vb->Unlock();
	}
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "draw 1");
	p = nullptr;
	CHECK(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "lock DISCARD 2");
	if (p) {
		quad(reinterpret_cast<V*>(p), 32, 0, 64, 32, 0xff445566);
		vb->Unlock();
	}
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "draw 2");
	p = nullptr;
	CHECK(SUCCEEDED(vb->Lock(4 * sizeof(V), 4 * sizeof(V), &p, D3DLOCK_NOOVERWRITE)) && p, "lock NOOVERWRITE");
	if (p) {
		quad(reinterpret_cast<V*>(p), 0, 40, 64, 64, 0xff778899);
		vb->Unlock();
	}
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2)), "draw 3");
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	uint32_t a = 0, b = 0, c = 0;
	CHECK(bb && readPixel(e.dev, bb, 10, 10, &a) && readPixel(e.dev, bb, 50, 10, &b) && readPixel(e.dev, bb, 30, 50, &c), "read back");
	CHECK((a & 0xffffff) == 0x112233, ("draw 1 (DISCARD then renamed) shows " + std::to_string(a & 0xffffff) + ", want 0x112233").c_str());
	CHECK((b & 0xffffff) == 0x445566, ("draw 2 shows " + std::to_string(b & 0xffffff)).c_str());
	CHECK((c & 0xffffff) == 0x778899, ("draw 3 (NOOVERWRITE) shows " + std::to_string(c & 0xffffff)).c_str());
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	vb->Release();
	return ok;
}

// DESIGN §8.2: Present on a WINDOWED device with a real swapchain (the game's path): acquire, the back buffer (resolved first
// when multisampled) copied into the swapchain image, present - frames in flight wrap, validation (core + sync) stays clean,
// no stretch (equal extents). The X- arm skips the copy source's layout transition: validation must report it.
bool casePresent(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	for (uint32_t f = 0; f < 6; ++f) {
		CHECK(bb && clearSurface(e.dev, bb, 0xff000000 | (f * 0x202020)), "clear");
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), ("Present " + std::to_string(f)).c_str());
	}
	if (bb)
		bb->Release();
	e.nd->ctx().waitIdle();
	e.nd->ctx().collect();
	uint32_t sw = 0, sh = 0;
	CHECK(e.nd->swapchainExtent(&sw, &sh) && sw == 64 && sh == 64, "swapchain extent == back buffer 64x64");
	CHECK(e.nd->stats().presents == 6, "6 presents counted");
	CHECK(e.nd->stats().stretchedPresents == 0, "equal extents: no stretched present");
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	return ok;
}

// d3d9_texture.h:60-71: SetLOD stores min(new, levels - 1) and returns the previous value.
bool caseLod(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture 4 levels");
	if (!t)
		return false;
	CHECK(t->SetLOD(1) == 0 && t->GetLOD() == 1, "SetLOD(1): returns 0, GetLOD 1");
	CHECK(t->SetLOD(9) == 1 && t->GetLOD() == 3, "SetLOD(9): returns 1, clamped to 3");
	t->Release();
	return ok;
}

// CopyRects DEFAULT -> DEFAULT between two levels of ONE texture: the copy reads the SOURCE's level,
// and the shared image keeps a layout valid for both ends (validation core + sync stays clean). The levels are filled the
// game's way (UpdateTexture from SYSTEMMEM). A render target bound at a level > 0 is refused by name (no attachment view).
bool caseCopyMips(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8 *sys = nullptr, *t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys)) && sys, "SYSTEMMEM 2 levels");
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t)) && t, "DEFAULT 2 levels");
	if (!sys || !t)
		return false;
	for (UINT l = 0; l < 2; ++l) {
		D3DLOCKED_RECT lr = {};
		const UINT n = 64u >> l;
		CHECK(SUCCEEDED(sys->LockRect(l, &lr, nullptr, 0)), "lock SYSTEMMEM level");
		for (UINT y = 0; y < n && lr.pBits; ++y)
			for (UINT x = 0; x < n; ++x)
				reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch)[x] = l ? 0xff505152u : 0xff0a0b0cu;
		sys->UnlockRect(l);
	}
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, t)), "UpdateTexture");
	IDirect3DSurface8 *l0 = nullptr, *l1 = nullptr;
	t->GetSurfaceLevel(0, &l0);
	t->GetSurfaceLevel(1, &l1);
	const RECT r = { 0, 0, 16, 16 };
	const POINT at = { 0, 0 };
	CHECK(l0 && l1 && SUCCEEDED(e.dev->CopyRects(l1, &r, 1, l0, &at)), "CopyRects level 1 -> level 0");
	uint32_t in = 0, out = 0;
	CHECK(l0 && readPixel(e.dev, l0, 5, 5, &in) && readPixel(e.dev, l0, 40, 40, &out), "read level 0");
	CHECK((in & 0xffffff) == 0x505152, ("copied texel " + std::to_string(in & 0xffffff) + ", want level 1's 0x505152").c_str());
	CHECK((out & 0xffffff) == 0x0a0b0c, "outside the rect untouched");
	e.nd->ctx().waitIdle();
	uint32_t sw = 0, sh = 0;
	CHECK(!e.nd->swapchainExtent(&sw, &sh), "offscreen: no swapchain extent");
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	IDirect3DTexture8* rt = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 2, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt)) && rt, "RT 2 levels");
	IDirect3DSurface8* rl1 = nullptr;
	if (rt)
		rt->GetSurfaceLevel(1, &rl1);
	const uint64_t before = e.nd->stats().targetsRefused;
	CHECK(rl1 && SUCCEEDED(e.dev->SetRenderTarget(rl1, nullptr)) && e.nd->stats().targetsRefused > before, "level-1 target refused by name");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->SetRenderTarget(bb, nullptr);
	for (IUnknown* u : { static_cast<IUnknown*>(bb), static_cast<IUnknown*>(rl1), static_cast<IUnknown*>(rt), static_cast<IUnknown*>(l0),
		     static_cast<IUnknown*>(l1), static_cast<IUnknown*>(t), static_cast<IUnknown*>(sys) })
		if (u)
			u->Release();
	return ok;
}

// UpdateTexture of a DXT1 texture after a partial lock: the level-0 dirty box {4,4,8,8} scales to
// offset 2,2 / extent 2x2 at mip 1, inside one 4x4 block - aligned to the block grid as DXVK's UpdateTextureFromBuffer aligns
// it, the update succeeds and every level holds the source's blocks.
bool caseUpdateDxt(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8 *sys = nullptr, *t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(16, 16, 2, 0, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &sys)) && sys, "SYSTEMMEM DXT1");
	CHECK(SUCCEEDED(e.dev->CreateTexture(16, 16, 2, 0, D3DFMT_DXT1, D3DPOOL_DEFAULT, &t)) && t, "DEFAULT DXT1");
	if (!sys || !t)
		return false;
	auto fill = [&](UINT level, const RECT* r, uint8_t seed) {
		D3DLOCKED_RECT lr = {};
		if (FAILED(sys->LockRect(level, &lr, r, 0)))
			return false;
		const UINT n = 16u >> level, blocksW = r ? UINT(r->right - r->left + 3) / 4 : (n + 3) / 4, rows = r ? UINT(r->bottom - r->top + 3) / 4 : (n + 3) / 4;
		for (UINT y = 0; y < rows; ++y)
			for (UINT b = 0; b < blocksW * 8; ++b)
				static_cast<uint8_t*>(lr.pBits)[size_t(y) * lr.Pitch + b] = uint8_t(seed + y * 31 + b);
		sys->UnlockRect(level);
		return true;
	};
	CHECK(fill(0, nullptr, 1) && fill(1, nullptr, 7), "fill both levels");
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, t)), "first UpdateTexture (whole dirty box)");
	const RECT r = { 4, 4, 8, 8 };
	CHECK(fill(0, &r, 90), "partial lock of level 0");
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, t)), "UpdateTexture with the box {4,4,8,8} must succeed");
	GXRender::Res::GxTexture& dt = static_cast<GxNativeTexture*>(t)->gx();
	for (UINT l = 0; l < 2; ++l) {
		std::vector<uint8_t> want, got;
		static_cast<GxNativeTexture*>(sys)->gx().readLevel(l, &want);
		dt.readLevel(l, &got);
		if (dt.decoded()) {   // no BC on this device: the GPU holds the blocks' decode (A8R8G8B8 = BGRA bytes) - compare that
			const uint32_t lw = dt.levelWidth(l), lh = dt.levelHeight(l), bw = (lw + 3) / 4;
			std::vector<uint8_t> dec(size_t(lw) * lh * 4);
			for (uint32_t by = 0; by * 4 < lh; ++by)
				for (uint32_t bx = 0; bx < bw; ++bx) {
					uint8_t texels[64];
					GXRender::Res::decodeBlockBGRA(D3DFMT_DXT1, &want[(size_t(by) * bw + bx) * 8], texels);
					for (uint32_t y = 0; y < 4 && by * 4 + y < lh; ++y)
						for (uint32_t x = 0; x < 4 && bx * 4 + x < lw; ++x)
							std::memcpy(&dec[((size_t(by) * 4 + y) * lw + bx * 4 + x) * 4], &texels[(y * 4 + x) * 4], 4);
				}
			want.swap(dec);
		}
		CHECK(!want.empty() && want == got, ("level " + std::to_string(l) + " holds the source's " + (dt.decoded() ? "decoded texels" : "blocks")).c_str());
	}
	// reading the DEFAULT level back into a DXT1 image surface: blocks on a BC device; a decoded texture has none to give
	// (its texels must never be written as blocks) - INVALIDCALL
	IDirect3DSurface8 *l0 = nullptr, *img = nullptr;
	t->GetSurfaceLevel(0, &l0);
	CHECK(l0 && SUCCEEDED(e.dev->CreateImageSurface(16, 16, D3DFMT_DXT1, &img)) && img, "DXT1 image surface");
	if (l0 && img) {
		const HRESULT hr = e.dev->CopyRects(l0, nullptr, 0, img, nullptr);
		CHECK(dt.decoded() ? hr == D3DERR_INVALIDCALL : SUCCEEDED(hr),
			dt.decoded() ? "a decoded level copied into DXT1 blocks must be refused" : "copy of DXT1 blocks");
	}
	if (img)
		img->Release();
	if (l0)
		l0->Release();
	t->Release();
	sys->Release();
	return ok;
}

// CopyRects into an MSAA surface: a single-sample source is refused by name (DXVK draws it - no game
// caller), not recorded as an invalid vkCmdCopyImage; validation stays clean.
bool caseCopyMsaaDst(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8 *ms = nullptr, *ss = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &ms)) && ms, "MSAA x4 target");
	CHECK(SUCCEEDED(e.dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &ss)) && ss, "single-sample target");
	if (ms && ss) {
		const uint64_t before = e.nd->stats().copiesRefused;
		CHECK(e.dev->CopyRects(ss, nullptr, 0, ms, nullptr) == D3DERR_INVALIDCALL && e.nd->stats().copiesRefused == before + 1,
			"x1 -> x4 refused by name");
	}
	e.nd->ctx().waitIdle();
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0, ("validation " + std::to_string(v.errors) + " " + v.first.substr(0, 120)).c_str());
	if (ms)
		ms->Release();
	if (ss)
		ss->Release();
	return ok;
}

// DXVK counts back buffers and the auto depth as losable until destroyed (d3d9_swapchain.cpp:1035, d3d9_device.cpp:8650):
// a back buffer the caller still holds blocks Reset (DEVICELOST, NotReset); released, Reset succeeds.
bool caseResetImplicit(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	CHECK(SUCCEEDED(e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb, "GetBackBuffer");
	CHECK(bb && clearSurface(e.dev, bb, 0xff3a5b7c), "clear it");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	CHECK(e.dev->Reset(&pp) == D3DERR_DEVICELOST, "Reset with a retained back buffer must be DEVICELOST");
	D3DSURFACE_DESC sd = {};
	uint32_t px = 0;   // the held surface is still the surface it was (DXVK keeps its resource alive)
	CHECK(bb && SUCCEEDED(bb->GetDesc(&sd)) && sd.Width == 64 && sd.Height == 64 && sd.Format == D3DFMT_A8R8G8B8,
		"the held back buffer keeps its description after the failed Reset");
	CHECK(bb && readPixel(e.dev, bb, 3, 3, &px) && (px & 0xffffff) == 0x3a5b7c, "the held back buffer keeps its content");
	if (bb)
		bb->Release();
	CHECK(SUCCEEDED(e.dev->Reset(&pp)), "Reset after the release");
	return ok;
}

// The auto depth is a DEFAULT-pool surface DXVK counts as losable until destroyed (d3d9_device.cpp:8650):
// GetDepthStencilSurface -> held across Reset -> DEVICELOST + NotReset; released -> Reset succeeds.
bool caseResetDepth(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* ds = nullptr;
	CHECK(SUCCEEDED(e.dev->GetDepthStencilSurface(&ds)) && ds, "GetDepthStencilSurface");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	CHECK(e.dev->Reset(&pp) == D3DERR_DEVICELOST, "Reset with the auto depth held must be DEVICELOST");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "NotReset");
	if (ds)
		ds->Release();
	CHECK(SUCCEEDED(e.dev->Reset(&pp)) && e.dev->TestCooperativeLevel() == D3D_OK, "Reset after the release");
	IDirect3DSurface8* ds2 = nullptr;
	CHECK(SUCCEEDED(e.dev->GetDepthStencilSurface(&ds2)) && ds2, "an auto depth after the Reset");   // (its address may be the old one's)
	if (ds2)
		ds2->Release();
	return ok;
}

// Reset's phases (DESIGN 8.4 = d3d8_device.cpp:236-247, d3d9_device.cpp:563-618):
// B - d3d9's early validation fails AFTER the lost state became Ok: a NotReset device is Ok again, bindings intact;
// D - a back-buffer format the swapchain cannot take fails AFTER the unbinding: NotReset, no render target bound.
bool caseResetPhases(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	e.dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t);
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	CHECK(t && e.dev->Reset(&pp) == D3DERR_DEVICELOST && e.dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "set up NotReset");
	DWORD z = 1;   // phase C's ResetState takes ZENABLE from the INCOMING parameters, also when the Reset fails (design-r3 #4)
	D3DPRESENT_PARAMETERS noDepth = pp;
	noDepth.EnableAutoDepthStencil = FALSE;
	CHECK(e.dev->Reset(&noDepth) == D3DERR_DEVICELOST && SUCCEEDED(e.dev->GetRenderState(D3DRS_ZENABLE, &z)) && z == D3DZB_FALSE,
		"a failed Reset without auto depth leaves ZENABLE FALSE");
	IDirect3DSurface8* rt = nullptr;
	pp.SwapEffect = D3DSWAPEFFECT(5);   // passes d3d8's checks, fails d3d9's - from NotReset (design-r3 #7)
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "phase B: INVALIDCALL");
	CHECK(e.dev->TestCooperativeLevel() == D3D_OK, "phase B: the lost state became Ok before the early validation");
	if (t)
		t->Release();
	pp = presentParams(true, 1);
	CHECK(SUCCEEDED(e.dev->Reset(&pp)), "Reset to a good state");
	// an UNKNOWN back buffer and no back-buffer count (the iOS game): DXVK converts both in the caller's struct (d3d8_d3d9_util.h:89-94)
	D3DPRESENT_PARAMETERS unk = presentParams(true, 0);
	unk.BackBufferFormat = D3DFMT_UNKNOWN;
	CHECK(SUCCEEDED(e.dev->Reset(&unk)) && unk.BackBufferFormat == D3DFMT_X8R8G8B8 && unk.BackBufferCount == 1,
		"Reset with an UNKNOWN back buffer / 0 back buffers is not converted as DXVK converts it");
	// the failure paths: (1) d3d8 refuses first (a windowed non-DEFAULT interval): INVALIDCALL, the caller's
	// struct untouched, the device still Ok with its render target bound; (2) d3d8 passes and d3d9's early validation fails (SwapEffect 5):
	// DXVK has already converted the caller's struct by then (d3d8_device.cpp:253, before GetD3D9()->Reset), so native must have too
	D3DPRESENT_PARAMETERS bad = presentParams(true, 0);
	bad.BackBufferFormat = D3DFMT_UNKNOWN;
	bad.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	IDirect3DSurface8* boundRt = nullptr;
	CHECK(e.dev->Reset(&bad) == D3DERR_INVALIDCALL && bad.BackBufferFormat == D3DFMT_UNKNOWN && bad.BackBufferCount == 0 &&
		e.dev->TestCooperativeLevel() == D3D_OK && SUCCEEDED(e.dev->GetRenderTarget(&boundRt)) && boundRt,
		"a Reset d3d8 refuses converted the caller's struct, or changed the device state or its binding");
	if (boundRt)
		boundRt->Release();
	bad.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
	bad.SwapEffect = D3DSWAPEFFECT(5);
	CHECK(e.dev->Reset(&bad) == D3DERR_INVALIDCALL && bad.BackBufferFormat == D3DFMT_X8R8G8B8 && bad.BackBufferCount == 1,
		"a Reset d3d9 refuses AFTER the conversion left the caller's struct unconverted");
	pp.SwapEffect = D3DSWAPEFFECT(5);
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "phase B again: INVALIDCALL");
	CHECK(SUCCEEDED(e.dev->GetRenderTarget(&rt)) && rt, "phase B: bindings intact");
	if (rt)
		rt->Release(), rt = nullptr;
	pp = presentParams(true, 1);
	pp.BackBufferFormat = D3DFMT_D24S8;
	pp.EnableAutoDepthStencil = FALSE;   // the late failure takes ZENABLE from these parameters too (design-r4 #4)
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "phase D: INVALIDCALL");
	CHECK(SUCCEEDED(e.dev->GetRenderState(D3DRS_ZENABLE, &z)) && z == D3DZB_FALSE, "phase D: ZENABLE from the incoming parameters");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "phase D: NotReset");
	CHECK(e.dev->GetRenderTarget(&rt) == D3DERR_NOTFOUND && !rt, "phase D: render target unbound");
	if (rt)
		rt->Release();
	pp = presentParams(true, 1);
	CHECK(SUCCEEDED(e.dev->Reset(&pp)) && e.dev->TestCooperativeLevel() == D3D_OK, "a good Reset recovers");
	return ok;
}

// DESIGN §9: a resource's last release may come from any thread (SurfaceClass releases directly while
// the d3d8 ring's render thread draws). It only queues the object; the device destroys it on ITS thread at the next entry
// point. (1) releases on another thread destroy nothing until the next Present; (2) releases racing draws on the device thread.
bool caseReleaseThread(Env& e, std::string* d)
{
	bool ok = true;
	auto make = [&](std::vector<IDirect3DTexture8*>* v, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			IDirect3DTexture8* t = nullptr;
			if (SUCCEEDED(e.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t)
				v->push_back(t);
		}
	};
	std::vector<IDirect3DTexture8*> a;
	make(&a, 32);
	const uint64_t live = e.nd->stats().liveTextures;
	std::thread rel([&] { for (IDirect3DTexture8* t : a) t->Release(); });
	rel.join();
	CHECK(e.nd->stats().liveTextures == live, ("released on another thread: " + std::to_string(live - e.nd->stats().liveTextures) +
		" destroyed there, want 0 until the device's next entry point").c_str());
	if (!ok)
		return false;   // (2) races the device with the defect present: never run it then
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().liveTextures == live - a.size(), "destroyed at Present");
	std::vector<IDirect3DTexture8*> b;
	make(&b, 64);
	struct V { float x, y, z, rhw; DWORD c; };
	const V q[4] = { { 0, 0, 0.5f, 1, 0xff203040 }, { 64, 0, 0.5f, 1, 0xff203040 }, { 0, 64, 0.5f, 1, 0xff203040 },
		{ 64, 64, 0.5f, 1, 0xff203040 } };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	std::atomic<bool> go { false };
	std::thread rel2([&] { while (!go.load()) {} for (IDirect3DTexture8* t : b) t->Release(); });
	go = true;
	for (int i = 0; i < 400; ++i)   // the device thread draws while the other one releases
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	rel2.join();
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	CHECK(e.nd->stats().liveTextures == live - a.size(), "all 64 destroyed at the Present after the race");
	CHECK(e.nd->stats().drawsRefused == 0, "draws refused during the race");
	return ok;
}

// DXVK presents an MSAA back buffer through dxvk_present_frag_ms.frag: every sample to linear light, averaged, back to sRGB
// A white triangle on black over MSAA x4: an edge pixel with k of 4 samples covered presents
// linear_to_srgb(k/4) = 0 / 137 / 188 / 225 / 255 - vkCmdResolveImage (the X- arm) gives 0 / 64 / 128 / 191 / 255.
bool caseMsaaPresentMath(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	struct V { float x, y, z, rhw; DWORD c; };
	const V tri[3] = { { 0, 0, 0.5f, 1, 0xffffffff }, { 64, 0, 0.5f, 1, 0xffffffff }, { 0, 61, 0.5f, 1, 0xffffffff } };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V))), "draw");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	CHECK(e.nd->testReadPresent(&px, &w, &h) && w == 64 && h == 64 && px.size() >= size_t(w) * h * 4, "read the presented image");
	const int dxvk[5] = { 0, 137, 188, 225, 255 };
	size_t edges = 0, foreign = 0;
	int firstForeign = -1;
	for (size_t i = 0; i + 3 < px.size(); i += 4) {
		const int v = px[i + 2];   // R of B8G8R8A8
		bool known = false;
		for (int k : dxvk)
			known = known || std::abs(v - k) <= 1;
		if (!known && foreign++ == 0)
			firstForeign = v;
		edges += known && v > 1 && v < 254;
	}
	CHECK(foreign == 0, ("pixels off DXVK's linear-light values: " + std::to_string(foreign) + " (first " +
		std::to_string(firstForeign) + ")").c_str());
	CHECK(edges > 0, "no partially covered edge pixel: the case proves nothing");
	if (bb)
		bb->Release();
	return ok;
}

// rw3-W lever (2) W-a (PREREG_T A20): the one-pass present (DXVK's present shader straight into the swapchain image) shows the
// SAME bytes as the old path (shader into a window-sized target, clear, 1:1 blit). One scene - a background, a triangle with three
// vertex colours (a gradient, and partially covered edge pixels under MSAA) - presented twice: one-pass first (no window-sized
// target may exist after it), then the old path (debug().presentDirectOff). Both read back from the swapchain image. expectDirect
// false (present-direct-shortfit, 72x50: the old path's float fit is 49 rows there): the first present must stay
// on the old path. Validation is asserted HERE: the harness does not count it for an X- arm.
bool presentDirectCompare(Env& e, std::string* d, bool expectDirect)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC desc = {};
	CHECK(bb && SUCCEEDED(bb->GetDesc(&desc)), "back buffer desc");
	struct V { float x, y, z, rhw; DWORD c; };
	const V tri[3] = { { 2.3f, 3.1f, 0.5f, 1, 0xffff2000 }, { 61.7f, 9.4f, 0.5f, 1, 0xff20ff40 }, { 17.2f, 58.6f, 0.5f, 1, 0xff3040ff } };
	std::vector<uint8_t> px[2];
	uint64_t direct[2] = {};
	for (int pass = 0; pass < 2; ++pass) {
		e.nd->debug().presentDirectOff = pass == 1;
		CHECK(bb && clearSurface(e.dev, bb, 0xff183050), "clear");
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V))), "draw");
		const uint64_t before = e.nd->stats().presentsDirect;
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
		direct[pass] = e.nd->stats().presentsDirect - before;
		uint32_t w = 0, h = 0;
		CHECK(e.nd->testReadPresent(&px[pass], &w, &h) && w == desc.Width && h == desc.Height && px[pass].size() == size_t(w) * h * 4,
			"read the presented image (the back buffer's size)");
		if (pass == 0 && expectDirect)
			CHECK(!e.nd->testHasPresentTarget(), "the one-pass present made a window-sized target");
	}
	e.nd->debug().presentDirectOff = false;
	CHECK(direct[0] == (expectDirect ? 1u : 0u) && direct[1] == 0, ("present_direct per present " + std::to_string(direct[0]) + " / " +
		std::to_string(direct[1]) + (expectDirect ? " (want 1 / 0)" : " (want 0 / 0)")).c_str());
	size_t differ = 0, colours = 0;
	std::vector<uint32_t> seen;
	for (size_t i = 0; i + 3 < px[0].size() && px[1].size() == px[0].size(); i += 4) {
		differ += std::memcmp(&px[0][i], &px[1][i], 4) != 0;
		uint32_t v;
		std::memcpy(&v, &px[0][i], 4);
		seen.push_back(v);
	}
	std::sort(seen.begin(), seen.end());
	colours = size_t(std::unique(seen.begin(), seen.end()) - seen.begin());
	CHECK(differ == 0, ("one-pass vs old path: " + std::to_string(differ) + " of " + std::to_string(px[0].size() / 4) +
		" pixels differ").c_str());
	CHECK(colours > 200, ("only " + std::to_string(colours) + " distinct colours: the scene proves nothing").c_str());
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	if (bb)
		bb->Release();
	return ok;
}

// present-direct-frames (the A30 coordination in MR_W-a.md; wa-r6 P2): six one-pass presents per phase. On a tree
// with A30 (render/integration 3e830ddae+, where the device, writer and swapchain all take --fif) run at --fif 3, that is every slot of
// three twice. On a 2-slot tree (this branch alone has no --fif), it is both slots three times.
// Each frame f has its own scene and its own clear colour, a marker read back at pixel (0,0), outside the triangle. Per path:
//  phase 1, captured: six frames in a row, each read back after its present. testReadPresent waits the device idle, so these never
//    overlap. Needs every one-pass present counted (6 / 0), each pair byte-equal, each frame carrying ITS marker and unlike EVERY
//    earlier frame (a stale slot or a stale capture fails);
//  phase 2, overlapped: six more frames with the capture OFF (no idle wait: they overlap in flight under core + sync validation) and ON
//    only for the last. The last frame's bytes must equal the old path's and carry its marker.
// Validation clean over both phases.
bool presentDirectFrames(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC desc = {};
	CHECK(bb && SUCCEEDED(bb->GetDesc(&desc)), "back buffer desc");
	struct V { float x, y, z, rhw; DWORD c; };
	const int kRun = 6;
	auto clearOf = [](int f) { return D3DCOLOR(0xff000000u | (DWORD(0x18 + 9 * f) << 16) | (DWORD(0x30 + 5 * f) << 8) | DWORD(0x50 + 11 * f)); };
	auto present = [&](int f, bool capture, uint64_t* direct) {
		const float o = 0.37f * float(f);
		const DWORD k = DWORD(f) * 0x00151515u;
		const V tri[3] = { { 2.3f + o, 3.1f, 0.5f, 1, 0xffff2000 ^ k }, { 61.7f - o, 9.4f + o, 0.5f, 1, 0xff20ff40 ^ k },
			{ 17.2f, 58.6f - o, 0.5f, 1, 0xff3040ff ^ k } };
		e.nd->debug().capturePresent = capture;
		CHECK(bb && clearSurface(e.dev, bb, clearOf(f)), "clear");
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V))), "draw");
		const uint64_t before = e.nd->stats().presentsDirect;
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
		*direct += e.nd->stats().presentsDirect - before;
	};
	auto read = [&](int f, std::vector<uint8_t>* px) {
		uint32_t w = 0, h = 0;
		CHECK(e.nd->testReadPresent(px, &w, &h) && w == desc.Width && h == desc.Height && px->size() == size_t(w) * h * 4,
			"read the presented image (the back buffer's size)");
		const D3DCOLOR c = clearOf(f);   // B8G8R8A8 rows: pixel (0,0) = B, G, R of frame f's clear colour
		CHECK(px->size() >= 4 && (*px)[0] == (c & 0xff) && (*px)[1] == ((c >> 8) & 0xff) && (*px)[2] == ((c >> 16) & 0xff),
			("frame " + std::to_string(f) + ": pixel (0,0) is not its own clear colour (a stale image)").c_str());
	};
	std::vector<uint8_t> px[2][kRun], last[2];
	uint64_t direct1[2] = {}, direct2[2] = {};
	for (int pass = 0; pass < 2; ++pass) {
		e.nd->debug().presentDirectOff = pass == 1;
		for (int f = 0; f < kRun; ++f) {
			present(f, true, &direct1[pass]);
			read(f, &px[pass][f]);
		}
		for (int f = kRun; f < 2 * kRun; ++f)
			present(f, f == 2 * kRun - 1, &direct2[pass]);
		read(2 * kRun - 1, &last[pass]);
	}
	e.nd->debug().presentDirectOff = false;
	e.nd->debug().capturePresent = true;
	CHECK(direct1[0] == uint64_t(kRun) && direct1[1] == 0 && direct2[0] == uint64_t(kRun) && direct2[1] == 0,
		("present_direct captured " + std::to_string(direct1[0]) + " / " + std::to_string(direct1[1]) + ", overlapped " +
		std::to_string(direct2[0]) + " / " + std::to_string(direct2[1]) + " (want 6 / 0 each)").c_str());
	auto differ = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
		size_t n = 0;
		for (size_t i = 0; i + 3 < a.size() && b.size() == a.size(); i += 4)
			n += std::memcmp(&a[i], &b[i], 4) != 0;
		return a.size() == b.size() ? n : a.size() / 4 + 1;
	};
	for (int f = 0; f < kRun; ++f) {
		const size_t n = differ(px[0][f], px[1][f]);
		CHECK(n == 0, ("one-pass vs old path: " + std::to_string(n) + " pixels differ (frame " + std::to_string(f) + ")").c_str());
		for (int g = 0; g < f; ++g)
			CHECK(px[0][f] != px[0][g], ("frame " + std::to_string(f) + " equals frame " + std::to_string(g) + ": a stale image would pass").c_str());
		std::vector<uint32_t> seen;
		for (size_t i = 0; i + 3 < px[0][f].size(); i += 4) {
			uint32_t v;
			std::memcpy(&v, &px[0][f][i], 4);
			seen.push_back(v);
		}
		std::sort(seen.begin(), seen.end());
		CHECK(size_t(std::unique(seen.begin(), seen.end()) - seen.begin()) > 200,
			("frame " + std::to_string(f) + ": too few distinct colours, the scene proves nothing").c_str());
	}
	const size_t nl = differ(last[0], last[1]);
	CHECK(nl == 0, ("overlapped phase, last frame: one-pass vs old path " + std::to_string(nl) + " pixels differ").c_str());
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	if (bb)
		bb->Release();
	return ok;
}

bool casePresentDirect(Env& e, std::string* d) { return presentDirectCompare(e, d, true); }
bool casePresentDirectShortFit(Env& e, std::string* d) { return presentDirectCompare(e, d, false); }

// FG P0 H3 (rw3-W ruling 1): W-a's one-pass present draws INTO the swapchain image, so it needs COLOR_ATTACHMENT there - which a
// Streamline proxy (DLSS-G loaded, DLSS_G guide §18) may lack, while vkCreateImageView would still succeed (viewsReady proves nothing).
// present-direct-usage: the swapchain is created WITHOUT COLOR_ATTACHMENT (the planted proxy, host.testSwapchainDropUsage): the old path
// must be taken (present_direct 0 / 0), its pixels, validation clean. X- (presentDirectIgnoresUsage) draws into it anyway: present_direct 1
// and the validation error.
bool casePresentDirectUsage(Env& e, std::string* d) { return presentDirectCompare(e, d, false); }

// present-direct-interposed: images that MAY be proxies (host.testSwapchainInterposed = what a loaded DLSS-G sets) prove no usage - the
// one-pass present is not taken and the present is not read back (TRANSFER_SRC unproven too), and the frame is still presented. X-
// (presentDirectIgnoresUsage) takes the one-pass present anyway.
bool casePresentDirectInterposed(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff183050), "clear");
	const GxNativeStats s0 = e.nd->stats();
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present 2");
	// under the submit thread a Present's driver answer is taken by the NEXT Present - a third one settles the second
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present 3");
	const GxNativeStats s1 = e.nd->stats();
	const uint64_t direct = s1.presentsDirect - s0.presentsDirect, presents = s1.presents - s0.presents;
	// the frames really SHOWN (a skip also answers D3D_OK and counts a present); no answer of the first two failed or
	// asked for a rebuild
	CHECK(presents == 3 && s1.presentsShown - s0.presentsShown == 3 && s1.presentsSkipped == s0.presentsSkipped &&
		s1.presentFailures == s0.presentFailures && s1.swapchainsRecreated == s0.swapchainsRecreated, ("presents " +
		std::to_string(presents) + " shown " + std::to_string(s1.presentsShown - s0.presentsShown) + " skipped " +
		std::to_string(s1.presentsSkipped - s0.presentsSkipped) + " failures " + std::to_string(s1.presentFailures - s0.presentFailures) +
		" recreated " + std::to_string(s1.swapchainsRecreated - s0.swapchainsRecreated) + " (want 3 / 3 / 0 / 0 / 0)").c_str());
	CHECK(direct == 0, ("present_direct " + std::to_string(direct) + " on images whose usage is unproven (want 0)").c_str());
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	CHECK(!e.nd->testReadPresent(&px, &w, &h), "the present was read back from images whose TRANSFER_SRC is unproven");
	CHECK(!e.nd->testSwapchainViewsMade(), "image views were made on images whose COLOR_ATTACHMENT is unproven");
	const GxNativeValidation v = e.nd->validation();
	CHECK(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	if (bb)
		bb->Release();
	return ok;
}
bool casePresentDirectFrames(Env& e, std::string* d) { return presentDirectFrames(e, d); }

// DXVK presents a back buffer smaller than the window with dxvk_present_frag_blit.frag's pixel-footprint rule: at 2x it is
// integer scaling - every window pixel is exactly the back-buffer pixel under it. A LINEAR blit
// (the X- arm) blends neighbours. Back buffer 64x64 with a distinct colour per pixel, window 128x128.
bool casePresentScaled(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	struct V { float x, y, z, rhw; DWORD c; };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	for (int y = 0; y < 64; ++y)
		for (int x = 0; x < 64; ++x) {   // one pixel each: the quad around D3D's pixel centre (x, y)
			const DWORD c = 0xff000000u | (DWORD(x * 4) << 16) | (DWORD(y * 4) << 8) | ((x ^ y) & 1 ? 0xffu : 0x00u);
			const float x0 = x - 0.5f, y0 = y - 0.5f;
			const V q[4] = { { x0, y0, 0.5f, 1, c }, { x0 + 1, y0, 0.5f, 1, c }, { x0, y0 + 1, 0.5f, 1, c }, { x0 + 1, y0 + 1, 0.5f, 1, c } };
			e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
		}
	std::vector<uint32_t> src(64 * 64);
	IDirect3DSurface8* img = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &img)) && img && SUCCEEDED(e.dev->CopyRects(bb, nullptr, 0, img, nullptr)),
		"read the back buffer");
	D3DLOCKED_RECT lr = {};
	if (img && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		for (int y = 0; y < 64; ++y)
			std::memcpy(&src[size_t(y) * 64], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 64 * 4);
		img->UnlockRect();
	}
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	CHECK(e.nd->testReadPresent(&px, &w, &h) && w == 128 && h == 128, "read the presented image (128x128)");
	size_t differ = 0;
	for (uint32_t y = 0; y < h && px.size() >= size_t(w) * h * 4; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			uint32_t v;
			std::memcpy(&v, &px[(size_t(y) * w + x) * 4], 4);
			differ += (v & 0xffffff) != (src[size_t(y / 2) * 64 + x / 2] & 0xffffff);
		}
	CHECK(differ == 0, ("window pixels not equal to the back-buffer pixel under them: " + std::to_string(differ)).c_str());
	CHECK(e.nd->stats().stretchedPresents >= 1, "counted as a scaled present");
	CHECK(src[1] != src[0], "the back buffer holds the per-pixel pattern");
	if (img)
		img->Release();
	if (bb)
		bb->Release();
	return ok;
}

// A GXRES failure the d3d8 call cannot report (it answers D3D_OK): GXRES's only report is an Error line through VkCtx's log
// ("[GXRES] staging ... failed", GxBuffer.cpp:198 / GxTexture.cpp:173 - a staging allocation failure under memory pressure,
// not inducible here), and the device must COUNT it (stats().resourceErrors) so the game's stats and gxcap-replay --native see
// it (C · rw3-P-2's ask). The lines go through VkCtx::logf, the path GXRES uses; Info lines and other prefixes are not counted.
bool caseResourceErrors(Env& e, std::string* d)
{
	bool ok = true;
	const uint64_t before = e.nd->stats().resourceErrors;
	e.nd->ctx().logf(GXRender::Vk::LogLevel::Error, "[GXRES] staging %u bytes for a buffer unlock failed (gx_native_test)", 64u);
	e.nd->ctx().logf(GXRender::Vk::LogLevel::Info, "[GXRES] an informational line (gx_native_test)");
	e.nd->ctx().logf(GXRender::Vk::LogLevel::Error, "[GXVK] another subsystem's error (gx_native_test)");
	CHECK(e.nd->stats().resourceErrors == before + 1, ("GXRES error lines counted: " + std::to_string(e.nd->stats().resourceErrors - before) +
		", want exactly 1").c_str());
	return ok;
}

// rw3-X-3 / R41 (both made this hoist; merged once): helpers used outside the window block (lever 3's thread and exit-quiesce arms,
// A23-B arms) sit above it, unchanged (none needs a window), so the standalone Windows suite, which has no SDL3, compiles them.
namespace V = GXRender::Vk;
bool presentClear(Env& e, D3DCOLOR c)
{
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	const bool ok = bb && clearSurface(e.dev, bb, c) && SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	if (bb)
		bb->Release();
	return ok;
}
bool rgbNear(uint32_t a, uint32_t b)
{
	for (int sh = 0; sh < 24; sh += 8)
		if (std::abs(int((a >> sh) & 0xff) - int((b >> sh) & 0xff)) > 1)   // r3-oracle: +-1 (a uniform region reads exact)
			return false;
	return true;
}
std::string validationText(Env& e)
{
	e.nd->ctx().waitIdle();
	const GxNativeValidation v = e.nd->validation();
	return std::to_string(v.errors) + "/" + std::to_string(v.hazards) + (v.first.empty() ? "" : " " + v.first.substr(0, 160));
}

#if defined(GX_VKCTX_HAS_SDL3)

// / r2-oracle P2-1: the recovery arms present DISTINGUISHABLE frames and read the WHOLE final
// swapchain image after each (TEST readback in recordPresent; a 2x window keeps the scaled present path too): uniform = that colour. Frame k is cleared to frameColour(k); a shown frame
// presents its own colour, an unshown one leaves the last shown colour. One "[TEST] SHOWN" line per Present (--strict-tap compares
// them across the modes). Returns false if the frame could not be presented or read.
uint32_t frameColour(int k) { return 0xff000000u | (uint32_t(40 + 20 * k) << 16) | (uint32_t(200 - 15 * k) << 8) | uint32_t(30 + 25 * k); }
uint32_t cornerColour(int k) { return 0xff000000u | (uint32_t(230 - 17 * k) << 16) | (uint32_t(20 + 23 * k) << 8) | uint32_t(240 - 11 * k); }
struct Shown { uint32_t want, got; };
bool presentShown(Env& e, int k, std::vector<Shown>* shown)
{
	e.nd->debug().testSwapchainReadback = true;   // the FINAL swapchain image, whole (set before the Present)
	// r3-oracle P2-1: an ASYMMETRIC frame - its colour, and a quad of a second colour in the top-left quarter: a mirrored, flipped,
	// partial or stale copy no longer reads as this frame
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC bd = {};
	const bool cleared = bb && SUCCEEDED(bb->GetDesc(&bd)) && clearSurface(e.dev, bb, frameColour(k));
	if (bb)
		bb->Release();
	if (!cleared)
		return false;
	struct CV { float x, y, z, rhw; DWORD c; };
	const float cw = float(bd.Width) / 2, ch = float(bd.Height) / 2;
	const DWORD cc = cornerColour(k);
	const CV corner[4] = { { 0, 0, 0.5f, 1, cc }, { cw, 0, 0.5f, 1, cc }, { 0, ch, 0.5f, 1, cc }, { cw, ch, 0.5f, 1, cc } };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, corner, sizeof(CV));
	e.dev->EndScene();
	if (FAILED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)))
		return false;
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	bool bgra = true;
	uint32_t got = 0xdeadbeefu;
	if (e.nd->testReadSwapchain(&px, &w, &h, &bgra) && w && h && px.size() >= size_t(w) * h * 4) {
		auto at = [&](size_t i) {
			const uint8_t* q = px.data() + i * 4;
			return bgra ? (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0] : (uint32_t(q[0]) << 16) | (uint32_t(q[1]) << 8) | q[2];
		};
		// EVERY pixel (r4-oracle P2-1), the corner quad's edge included (r5-oracle P2-1: the 2x present is point-sampled, so a
		// linear filter's blended edge is a wrong image): frame j is cornerColour(j) in the top-left quarter and frameColour(j)
		// elsewhere. got = frameColour(j) of the frame whose whole image this is, else 0xfeedfa (a mirrored / clipped / partial /
		// filtered / wrong copy, never a frame)
		got = 0xfeedfau;
		for (int j = k; j >= 0 && got == 0xfeedfau; --j) {
			bool all = true;
			for (uint32_t y = 0; y < h && all; ++y)
				for (uint32_t x = 0; x < w && all; ++x)
					all = rgbNear(at(size_t(y) * w + x), x < w / 2 && y < h / 2 ? cornerColour(j) : frameColour(j));
			if (all)
				got = frameColour(j) & 0xffffff;
		}
	}
	shown->push_back({ frameColour(k) & 0xffffff, got });
	std::fprintf(stderr, "[TEST] SHOWN k=%d want=%06x got=%06x\n", k, frameColour(k) & 0xffffffu, got);
	return true;
}
// every Present showed its own frame, or - unshown - the last shown frame again; *unshown = how many were unshown
bool shownValid(const std::vector<Shown>& s, int* unshown)
{
	*unshown = 0;
	for (size_t k = 0; k < s.size(); ++k) {
		if (rgbNear(s[k].got, s[k].want))
			continue;
		if (k == 0 || !rgbNear(s[k].got, s[k - 1].got))
			return false;
		++*unshown;
	}
	return true;
}


// DXVK reads the window's size every Present and recreates the swapchain when it changed (d3d9_swapchain.cpp:1325,
// dxvk_presenter.cpp:115): created equal (64), resized to 128 - the next Present recreates at 128 and
// presents the 64 back buffer scaled (DXVK's footprint shader), nothing skipped, validation clean.
bool casePresentResize(Env& e, std::string* d)
{
	bool ok = true;
	uint32_t w = 0, h = 0;
	CHECK(presentClear(e, 0xff204060) && e.nd->swapchainExtent(&w, &h) && w == 64, "a first frame at 64x64");
	SDL_SetWindowSize(static_cast<SDL_Window*>(e.window), 128, 128);
	SDL_SyncWindow(static_cast<SDL_Window*>(e.window));
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames after the resize");
	CHECK(e.nd->swapchainExtent(&w, &h) && w == 128 && h == 128, ("swapchain " + std::to_string(w) + "x" + std::to_string(h) + " after the window became 128x128").c_str());
	CHECK(e.nd->stats().stretchedPresents >= 1 && e.nd->stats().presentsSkipped == 0, "scaled presents, none skipped");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// A swapchain recreation that fails skips the frame and is retried at the next Present (DXVK: softError, then the next
// acquire recreates; the r2 device stopped presenting for good). The failure is injected.
bool casePresentRecreateRetry(Env& e, std::string* d)
{
	bool ok = true;
	uint32_t w = 0, h = 0;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	SDL_SetWindowSize(static_cast<SDL_Window*>(e.window), 128, 128);
	SDL_SyncWindow(static_cast<SDL_Window*>(e.window));
	e.nd->debug().failNextRecreate = true;
	CHECK(presentClear(e, 0xff204060) && e.nd->stats().presentsSkipped == 1, "the frame with the failed recreation is skipped");
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "later frames");
	CHECK(e.nd->swapchainExtent(&w, &h) && w == 128 && e.nd->stats().presentsSkipped == 1, "the next Present recreated the swapchain");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// Everything that can fail is made BEFORE the acquire: an injected present-target failure skips one
// frame and leaves no acquired image behind - four more frames reuse both acquire semaphores with validation clean.
bool casePresentTargetFail(Env& e, std::string* d)
{
	bool ok = true;
	e.nd->debug().presentDirectOff = true;   // the window-sized target exists only on the old path (rw3-W W-a: not on a 1:1 present)
	e.nd->debug().presentTargetFailsOnce = true;   // the first MSAA present must create its target: it fails
	CHECK(presentClear(e, 0xff204060) && e.nd->stats().presentsSkipped == 1, "the frame without a target is skipped");
	for (int i = 0; i < 4; ++i)
		CHECK(presentClear(e, 0xff204060), "later frames");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// VK_ERROR_SURFACE_LOST_KHR: a NEW VkSurfaceKHR and swapchain before the next acquire, as DXVK's presenter (dxvk_presenter.cpp:513;
// ). The loss is injected before any image is acquired; the frame is still shown, later frames too.
bool casePresentSurfaceLost(Env& e, std::string* d)
{
	bool ok = true;
	std::vector<Shown> shown;
	int k = 0, unshown = 0;
	CHECK(presentShown(e, k++, &shown), "a first frame");
	if (csBranch(e)) {   // A23-B CS=1 contract (rw3-X/CS1_ARM_CONTRACT.md, divergence b): the worker's acquire fails -> the frame is
		                  // not shown, the surface is marked lost; an S Present makes the new surface; <= 2 frames unshown, then all shown
		e.nd->debug().injectSurfaceLost = true;
		for (int i = 0; i < 3; ++i)
			CHECK(presentShown(e, k++, &shown), "frames around the surface loss answer D3D_OK");
		const GxNativeStats a = e.nd->stats();
		CHECK(presentShown(e, k++, &shown) && presentShown(e, k++, &shown), "later frames");
		const GxNativeStats b = e.nd->stats();
		*d += "surfacesRecreated " + std::to_string(b.surfacesRecreated) + ", skipped " + std::to_string(a.presentsSkipped) + " then +" +
			std::to_string(b.presentsSkipped - a.presentsSkipped);
		CHECK(b.surfacesRecreated == 1 && a.presentsSkipped >= 1 && a.presentsSkipped <= 2 && b.presentsSkipped == a.presentsSkipped,
			"a new surface by an S Present, 1-2 frames unshown, then every frame shown");
		CHECK(shownValid(shown, &unshown) && uint64_t(unshown) == b.presentsSkipped,
			"each Present showed its own frame or (unshown) the last shown one");
		const std::string v = validationText(e);
		CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
		return ok;
	}
	e.nd->debug().injectSurfaceLost = true;
	CHECK(presentShown(e, k++, &shown) && presentShown(e, k++, &shown), "frames after the surface loss");
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 0, "a new surface, nothing skipped");
	CHECK(shownValid(shown, &unshown) && unshown == 0, "every Present showed its own frame");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// a RECTANGULAR window (96x64 back buffer and window, 1:1). Six Presents: each shows its own whole frame,
// nothing is recreated or skipped - the width and height the worker's Present takes are the call's, in order.
// X-cs-present-transposed (the worker's Present swaps them: every frame looks like a resize) is caught. Both modes.
bool casePresentRect(Env& e, std::string* d)
{
	bool ok = true;
	std::vector<Shown> shown;
	const GxNativeStats a = e.nd->stats();
	for (int k = 0; k < 6; ++k)
		CHECK(presentShown(e, k, &shown), "a Present");
	const GxNativeStats b = e.nd->stats();
	int unshown = 0;
	const bool valid = shownValid(shown, &unshown);
	const uint64_t rebuilt = (b.swapchainsRecreated - a.swapchainsRecreated) + (b.surfacesRecreated - a.surfacesRecreated),
		skipped = b.presentsSkipped - a.presentsSkipped;
	*d += "recreated " + std::to_string(rebuilt) + ", skipped " + std::to_string(skipped) + ", unshown " + std::to_string(unshown);
	CHECK(rebuilt == 0 && skipped == 0 && valid && unshown == 0, "every Present showed its own frame, nothing recreated or skipped");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// A frame whose submission fails after its acquire is never presented (the present would wait on a semaphore no submission
// signals, VUID-vkQueuePresentKHR-pWaitSemaphores-03268): terminal - Present and TestCooperativeLevel answer DEVICELOST and no
// image is acquired again. No X- arm: presenting on the unsignalled semaphore can hang the driver.
bool casePresentSubmitFail(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	e.nd->debug().failNextSubmit = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence a): the worker latches it; DEVICELOST within <= 2 Presents, then for good
		HRESULT h[3] = {};
		CHECK(bb && clearSurface(e.dev, bb, 0xff204060), "the frame");
		for (HRESULT& x : h)
			x = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		int first = -1;
		for (int i = 0; i < 3; ++i)
			if (h[i] == D3DERR_DEVICELOST && first < 0)
				first = i;
		*d += "first DEVICELOST at Present N+" + std::to_string(first);
		CHECK(first >= 0 && (first == 2 || h[first + 1] == D3DERR_DEVICELOST) && (first >= 1 || h[2] == D3DERR_DEVICELOST),
			"the failed submission's loss answered within <= 2 Presents, then on every Present");
	} else
	CHECK(bb && clearSurface(e.dev, bb, 0xff204060) && e.dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST,
		"the failed submission's Present answers DEVICELOST");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICELOST && e.dev->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST,
		"terminal");
	if (bb)
		bb->Release();
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// the surface is lost while a recreation is due BEFORE the acquire (a resize): swapchain creation on it
// fails (VkSwapchain answers false; Vulkan answered SURFACE_LOST). As DXVK's recreateSwapChain (dxvk_presenter.cpp:507-523) the
// device asks the surface, finds it lost, makes a new one and its swapchain in the SAME recreation: the frame is shown.
// Injected. Planted: the creation failure is not classified - presentation never resumes.
bool casePresentCreateLost(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	e.nd->debug().injectCreateSurfaceLost = true;
	for (int i = 0; i < 3; ++i)
		CHECK(presentClear(e, 0xff204060), "frames after the loss");
	*d += "surfacesRecreated " + std::to_string(e.nd->stats().surfacesRecreated) + ", skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 0, "a new surface in the same recreation, nothing skipped");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// The same loss, and the REPLACEMENT surface's creation fails once (transient): that frame is skipped, the next Present makes
// the surface and recovers ('s closure). Planted: a lost surface is never made again.
bool casePresentCreateLostRetry(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	e.nd->debug().injectCreateSurfaceLost = true;
	e.nd->debug().failNextSurfaceCreate = true;
	for (int i = 0; i < 3; ++i)
		CHECK(presentClear(e, 0xff204060), "frames after the loss");
	*d += "surfacesRecreated " + std::to_string(e.nd->stats().surfacesRecreated) + ", skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 1, "one frame skipped, then a new surface");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// VK_ERROR_SURFACE_LOST_KHR from vkQueuePresentKHR (design-r5 #3: the r5 fixture covered the acquire side only): the next Present
// makes a new surface and swapchain before its acquire. Injected; the old surface stays lost. Planted: no new surface.
bool casePresentSideLost(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence b): the worker applies the SURFACE_LOST answer; the Present after next
		// at the latest recovers as S on a new surface - <= 2 frames unshown, then every frame shown
		e.nd->debug().injectPresentSurfaceLost = true;
		for (int i = 0; i < 4; ++i)
			CHECK(presentClear(e, 0xff204060), "frames around the loss answer D3D_OK");
		const GxNativeStats a = e.nd->stats();
		CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "later frames");
		const GxNativeStats b = e.nd->stats();
		*d += "surfacesRecreated " + std::to_string(b.surfacesRecreated) + ", skipped " + std::to_string(a.presentsSkipped) + " then +" +
			std::to_string(b.presentsSkipped - a.presentsSkipped);
		CHECK(b.surfacesRecreated == 1 && a.presentsSkipped <= 2 && b.presentsSkipped == a.presentsSkipped,
			"one new surface, <= 2 frames unshown, then every frame shown");
		const std::string v = validationText(e);
		CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
		return ok;
	}
	e.nd->debug().injectPresentSurfaceLost = true;
	for (int i = 0; i < 3; ++i)
		CHECK(presentClear(e, 0xff204060), "frames around the loss");
	*d += "surfacesRecreated " + std::to_string(e.nd->stats().surfacesRecreated) + ", skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 0, "a new surface before the next acquire");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// the acquire fails (surface lost), the recreation succeeds, but the writer frame after it cannot begin.
// The RETRY acquire must be checked like the first: the frame is skipped BEFORE acquiring (D3D_OK, counted) and the next Present
// recovers. Injected. Planted: the retry acquires without a frame - its submission is empty and Present goes DEVICELOST for good.
bool casePresentRetryFrame(Env& e, std::string* d)
{
	bool ok = true;
	std::vector<Shown> shown;
	int k = 0, unshown = 0;
	CHECK(presentShown(e, k++, &shown), "a first frame");
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence b): the retry acquire never runs on the worker. The first injected loss
		// ends a frame unshown there (the surface marked lost); the S Present that recovers then meets a SECOND injected acquire
		// failure, so today's retry path - recreation without a writer frame, the plant's place - runs inline in it
		e.nd->debug().injectSurfaceLost = true;
		CHECK(presentShown(e, k++, &shown), "the worker's unshown frame answers D3D_OK");
		e.nd->csSync();   // its flag is visible: the next Present is S
		e.nd->debug().injectSurfaceLost = true;
		e.nd->debug().failFrameInRecreateAfter = 2;   // the first recreation (the lost surface) succeeds, the retry one fails
		CHECK(presentShown(e, k++, &shown), "the S Present whose retry recreation left no writer frame answers D3D_OK");
		CHECK(presentShown(e, k++, &shown) && presentShown(e, k++, &shown), "later frames");
		CHECK(e.dev->TestCooperativeLevel() == D3D_OK, "not lost");
		*d += "skipped " + std::to_string(e.nd->stats().presentsSkipped);
		CHECK(e.nd->stats().presentsSkipped == 2, "exactly the worker's frame and the retry's frame skipped");
		CHECK(shownValid(shown, &unshown) && unshown == 2, "each Present showed its own frame or (unshown) the last shown one");
		const std::string v = validationText(e);
		CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
		return ok;
	}
	e.nd->debug().injectSurfaceLost = true;
	e.nd->debug().failFrameInRecreate = true;
	CHECK(presentShown(e, k++, &shown), "the frame whose recreation left no writer frame answers D3D_OK");
	CHECK(presentShown(e, k++, &shown) && presentShown(e, k++, &shown), "later frames");
	CHECK(e.dev->TestCooperativeLevel() == D3D_OK, "not lost");
	*d += "skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().presentsSkipped == 1, "exactly the one frame skipped");
	CHECK(shownValid(shown, &unshown) && unshown == 1, "each Present showed its own frame or (unshown) the last shown one");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// a Present reports SURFACE_LOST and the game Resets before the next Present. The Reset's swapchain is
// built on a NEW surface (the lost one would fail every retry, and a failed Reset leaves no swapchain for a Present to recover).
// Injected; the old surface stays lost. Planted: the lost surface is never made again - the Reset fails.
bool caseResetSurfaceLost(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	e.nd->debug().injectPresentSurfaceLost = true;
	CHECK(presentClear(e, 0xff204060), "the frame whose present reports the surface lost");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.hDeviceWindow = static_cast<HWND>(e.window);
	const HRESULT hr = e.dev->Reset(&pp);
	CHECK(hr == D3D_OK, ("Reset with the surface lost answered " + std::to_string(long(hr))).c_str());
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames after the Reset");
	*d += "surfacesRecreated " + std::to_string(e.nd->stats().surfacesRecreated) + ", skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 0, "the Reset made a new surface; nothing skipped");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}


// ---------------------------------------------------------------------------------------------- the submit thread (R19, rw3-V)
// Under the submit thread a present runs after the Present call that queued it: its answer is applied at the next Present (or at
// Reset) - DESIGN_submit-thread.md 3.5. Each arm queues ops behind VkCtx::testHoldAt and injects at the thread's choke point.

int fatalLines()
{
	int n = 0;
	for (const std::string& l : logSnapshot())
		n += l.find("FATAL") != std::string::npos;
	return n;
}

// The frame's submit is REFUSED at the thread while it and its present are queued: the present never reaches the driver, its
// answer is DeviceLost, the next Present answers DEVICELOST, ONE FATAL line names the refused frame submit (
// rw3V-design-r1 P2-4; integrator condition 3). X-: a skipped present reads Ok.
bool caseThreadPresentSkipped(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	ctx.testSet([&](V::TestMutants& m) { m.skippedPresentOk = e.nd->debug().threadSkippedPresentOk; });
	SETUP(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "two warm-up frames");
	e.nd->ctx().waitIdle();
	const int fatal0 = fatalLines();
	const uint64_t fv = e.nd->ctx().queue(V::QueueKind::Graphics).timeline.lastSubmitted() + 1;
	const V::SubmitThreadStats s0 = ctx.threadStats();
	ctx.testHoldAt(fv);
	ctx.testSet([&](V::TestMutants& m) { m.submitFailAt = fv; });
	const bool queuedOk = presentClear(e, 0xff204060);
	e.nd->csSync();   // A23-B CS=1: the frame's submit and present are queued by the worker (CS=0: a no-op)
	const V::SubmitThreadStats sq = ctx.threadStats();
	if (!queuedOk || sq.enqueued - s0.enqueued != 2 || sq.submits != s0.submits) {
		ctx.testRelease();
		SETUP(false, "the frame and its present were not queued behind the hold");
	}
	ctx.testRelease();
	e.nd->ctx().waitIdle();
	const V::SubmitThreadStats s1 = ctx.threadStats();
	const V::VkSwapchain::Status st = e.nd->swapchain().takePresentStatus();
	const V::SubmitRefusal r = ctx.refusal();
	const HRESULT next = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	e.nd->csSync();   // A23-B CS=1 (divergence a): the FATAL line is the worker's PresentLost - read after a drain (CS=0: a no-op)
	ctx.testSet([&](V::TestMutants& m) { m.submitFailAt = 0; });
	ctx.testSet([&](V::TestMutants& m) { m.skippedPresentOk = false; });
	*d += "driver presents +" + std::to_string(s1.driverPresents - s0.driverPresents) + ", skipped +" +
	      std::to_string(s1.skipped - s0.skipped) + ", status " + V::VkSwapchain::name(st) + ", refusal " + (r.label ? r.label : "-") +
	      " value " + std::to_string(r.value) + " (frame " + std::to_string(fv) + "), next Present " + std::to_string(long(next)) +
	      ", FATAL lines +" + std::to_string(fatalLines() - fatal0);
	CHECK(s1.driverPresents == s0.driverPresents && s1.skipped - s0.skipped >= 1, "the skipped present never reached the driver");
	CHECK(st == V::VkSwapchain::Status::DeviceLost, "the skipped present's answer is DeviceLost");
	CHECK(r.value == fv && r.label && std::string(r.label) == "frame", "the refusal names the frame submit");
	CHECK(next == D3DERR_DEVICELOST, "the next Present answers DEVICELOST");
	CHECK(fatalLines() - fatal0 == 1, "one FATAL line");
	return ok;
}

// A present's answer injected at the thread (OUT_OF_DATE, SURFACE_LOST, OUT_OF_HOST_MEMORY; the driver not called) is handled at
// the NEXT Present exactly as the inline answer was: rebuild / new surface / rebuild + presentFailures. X-: Present ignores it.
bool caseThreadPresentDeferred(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	SETUP(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "two warm-up frames");
	auto injectNext = [&](VkResult r) {
		e.nd->ctx().waitIdle();   // every present so far has run: the next one is presents + 1
		ctx.testSet([&](V::TestMutants& m) { m.presentResult = r; });
		const uint64_t nextPresent = ctx.threadStats().presents + 1;   // read OUTSIDE testSet: its callback runs under the op lock
		ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = nextPresent; });
	};
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence b): the answer is applied by the WORKER's next Present, which ends that
		// frame unshown and leaves the recovery to an S Present within <= 2 Presents; one rebuild / new surface per event
		struct Event { VkResult r; uint64_t GxNativeStats::*counter; const char* name; };
		const Event events[] = { { VK_ERROR_OUT_OF_DATE_KHR, &GxNativeStats::swapchainsRecreated, "out-of-date" },
			{ VK_ERROR_SURFACE_LOST_KHR, &GxNativeStats::surfacesRecreated, "surface-lost" },
			{ VK_ERROR_OUT_OF_HOST_MEMORY, &GxNativeStats::swapchainsRecreated, "out-of-memory" } };
		for (const Event& ev : events) {
			injectNext(ev.r);
			const GxNativeStats s0 = e.nd->stats();
			for (int i = 0; i < 4; ++i)
				CHECK(presentClear(e, 0xff204060), "frames around the injected present");
			e.nd->ctx().waitIdle();
			const GxNativeStats s1 = e.nd->stats();
			ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = 0; });
			*d += std::string(ev.name) + ": +" + std::to_string(s1.*ev.counter - s0.*ev.counter) + " skipped +" +
				std::to_string(s1.presentsSkipped - s0.presentsSkipped) + " failures +" + std::to_string(s1.presentFailures - s0.presentFailures) + "; ";
			CHECK(s1.*ev.counter - s0.*ev.counter == 1, (std::string(ev.name) + ": handled once").c_str());
			CHECK(s1.presentsSkipped - s0.presentsSkipped <= 2, (std::string(ev.name) + ": <= 2 frames unshown").c_str());
			CHECK(s1.presentFailures - s0.presentFailures == (ev.r == VK_ERROR_OUT_OF_HOST_MEMORY ? 1u : 0u),
				(std::string(ev.name) + ": present failures").c_str());
		}
		const std::string v = validationText(e);
		CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
		return ok;
	}
	const GxNativeStats a = e.nd->stats();
	injectNext(VK_ERROR_OUT_OF_DATE_KHR);
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames around the out-of-date present");
	const GxNativeStats b = e.nd->stats();
	injectNext(VK_ERROR_SURFACE_LOST_KHR);
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames around the surface-lost present");
	const GxNativeStats c = e.nd->stats();
	injectNext(VK_ERROR_OUT_OF_HOST_MEMORY);
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames around the failed present");
	e.nd->ctx().waitIdle();   // every queued present has run before the injection is cleared
	const GxNativeStats f = e.nd->stats();
	ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = 0; });
	*d += "out-of-date: swapchains +" + std::to_string(b.swapchainsRecreated - a.swapchainsRecreated) + "; surface-lost: surfaces +" +
	      std::to_string(c.surfacesRecreated - b.surfacesRecreated) + "; out-of-memory: swapchains +" +
	      std::to_string(f.swapchainsRecreated - c.swapchainsRecreated) + ", presentFailures +" + std::to_string(f.presentFailures - c.presentFailures) +
	      ", skipped " + std::to_string(f.presentsSkipped - a.presentsSkipped);
	CHECK(b.swapchainsRecreated - a.swapchainsRecreated == 1, "out-of-date: one rebuild at the next Present");
	CHECK(c.surfacesRecreated - b.surfacesRecreated == 1, "surface-lost: one new surface at the next Present");
	CHECK(f.swapchainsRecreated - c.swapchainsRecreated == 1 && f.presentFailures - c.presentFailures == 1, "out-of-memory: one rebuild, one failure");
	CHECK(f.presentsSkipped == a.presentsSkipped, "nothing skipped");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// SURFACE_LOST injected at the thread, the surface then behaving as lost, and the game Resets before the next Present: Reset takes
// the answer first and builds on a NEW surface, nothing stale is left for the next Present (the
// boundary of reset-surface-lost, unchanged). X-: Reset leaves the answer pending - it turns up stale at the next Present.
bool caseThreadResetDeferred(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	SETUP(presentClear(e, 0xff204060), "a first frame");
	e.nd->ctx().waitIdle();
	ctx.testSet([&](V::TestMutants& m) { m.presentResult = VK_ERROR_SURFACE_LOST_KHR; });
	const uint64_t nextPresent = ctx.threadStats().presents + 1;   // read OUTSIDE testSet: its callback runs under the op lock
	ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = nextPresent; });
	CHECK(presentClear(e, 0xff204060), "the frame whose present reports the surface lost (later)");
	e.nd->testMarkSurfaceLost();
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.hDeviceWindow = static_cast<HWND>(e.window);
	const HRESULT hr = e.dev->Reset(&pp);
	ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = 0; });
	CHECK(hr == D3D_OK, ("Reset answered " + std::to_string(long(hr))).c_str());
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames after the Reset");
	const uint64_t stale = e.nd->swapchain().staleStatuses();
	*d += "surfacesRecreated " + std::to_string(e.nd->stats().surfacesRecreated) + ", stale statuses " + std::to_string(stale) +
	      ", skipped " + std::to_string(e.nd->stats().presentsSkipped);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped == 0, "the Reset made a new surface; nothing skipped");
	CHECK(stale == 0, "no present answer left over for a replaced swapchain");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// the window reports a size the surface clamps away (MoltenVK's currentExtent wins: the swapchain stays
// 64x64 while 80x80 was asked). One size change = ONE recreation, not one per Present. Injected (the drawable size + 16).
// Planted: the resize test compares the window with the ACTUAL extent - every Present recreates.
bool casePresentClampedExtent(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	const uint64_t before = e.nd->stats().swapchainsRecreated;
	e.nd->debug().drawableSizeOffset = 16;
	for (int i = 0; i < 4; ++i)
		CHECK(presentClear(e, 0xff204060), "frames at the clamped size");
	e.nd->debug().drawableSizeOffset = 0;
	uint32_t w = 0, h = 0;
	CHECK(e.nd->swapchainExtent(&w, &h) && w == 64 && h == 64, ("the surface clamped 80x80 to " + std::to_string(w) + "x" +
		std::to_string(h) + " (want 64x64, else the case tests nothing)").c_str());
	const uint64_t rebuilt = e.nd->stats().swapchainsRecreated - before;
	*d += "recreations over 4 Presents: " + std::to_string(rebuilt);
	CHECK(rebuilt == 1, "one size change, one recreation");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// a Reset that builds the swapchain and then cannot allocate its back buffer leaves the device NotReset.
// Present then answers D3DERR_DEVICELOST and presents nothing, as DXVK's (d3d9_swapchain.cpp:116-117); a later good Reset
// recovers. Injected (the back-buffer allocation). Planted: Present runs while NotReset - it reaches the empty back-buffer
// storage (the guard skips the frame rather than dereferencing null) and answers D3D_OK.
bool caseResetFailPresent(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.hDeviceWindow = static_cast<HWND>(e.window);
	e.nd->debug().failNextBackBuffer = true;
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "the Reset whose back buffer cannot be made fails");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "NotReset");
	const HRESULT hr = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	*d += "Present while NotReset answered " + std::to_string(long(hr));
	CHECK(hr == D3DERR_DEVICELOST, "Present while NotReset answers DEVICELOST (DXVK)");
	CHECK(e.dev->Reset(&pp) == D3D_OK, "a good Reset");
	CHECK(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "frames after the good Reset");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// vkQueuePresentKHR fails with OUT_OF_*_MEMORY - the present is not enqueued, its image stays acquired and
// its semaphore unwaited. As DXVK (every status but VK_SUCCESS marks the swapchain dirty, dxvk_presenter.cpp:247-252) the next
// Present rebuilds it before acquiring, which retires the stranded image. Injected (the call is skipped, as such a failure leaves
// it). Planted: the failure is ignored - no rebuild, the image stays stranded.
bool casePresentError(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	const uint64_t before = e.nd->stats().swapchainsRecreated;
	e.nd->debug().injectPresentError = true;
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence b): one rebuild by an S Present within <= 2 Presents, <= 2 unshown
		const uint64_t skipped0 = e.nd->stats().presentsSkipped;
		for (int i = 0; i < 4; ++i)
			CHECK(presentClear(e, 0xff204060), "frames around the failed present");
		const GxNativeStats a = e.nd->stats();
		*d += "rebuilds " + std::to_string(a.swapchainsRecreated - before) + ", presentFailures " + std::to_string(a.presentFailures) +
			", skipped +" + std::to_string(a.presentsSkipped - skipped0);
		CHECK(a.swapchainsRecreated - before == 1 && a.presentFailures == 1 && a.presentsSkipped - skipped0 <= 2,
			"one rebuild after the failed present, <= 2 frames unshown");
		const std::string v = validationText(e);
		CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
		return ok;
	}
	for (int i = 0; i < 3; ++i)
		CHECK(presentClear(e, 0xff204060), "frames around the failed present");
	const uint64_t rebuilt = e.nd->stats().swapchainsRecreated - before;
	*d += "rebuilds " + std::to_string(rebuilt) + ", presentFailures " + std::to_string(e.nd->stats().presentFailures);
	CHECK(rebuilt == 1 && e.nd->stats().presentFailures == 1, "one rebuild after the failed present");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}

// A present shader that cannot draw shows BLACK, counted - never the target's previous content. The
// one-pass present (rw3-W W-a) has no target of its own: what it could show stale is the swapchain image, so four red frames
// first - every swapchain image last held red. present-shader-fail-old runs the same on the old path (window-sized target).
bool casePresentShaderFail(Env& e, std::string* d)
{
	bool ok = true;
	for (int i = 0; i < 4; ++i)
		CHECK(presentClear(e, 0xffff0000), "a red frame");
	e.nd->debug().presentPipelineFails = true;
	CHECK(presentClear(e, 0xff00ff00), "a green frame whose present shader fails");
	e.nd->debug().presentPipelineFails = false;
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	CHECK(e.nd->testReadPresent(&px, &w, &h) && px.size() >= 4, "read the presented image");
	uint32_t v = 0;
	if (px.size() >= 4)
		std::memcpy(&v, px.data(), 4);
	CHECK((v & 0xffffff) == 0, ("presented " + std::to_string(v & 0xffffff) + ", want black (not the stale red frame)").c_str());
	CHECK(e.nd->stats().presentErrors == 1, "counted");
	return ok;
}

bool casePresentShaderFailOld(Env& e, std::string* d)
{
	e.nd->debug().presentDirectOff = true;
	return casePresentShaderFail(e, d);
}
#endif

// DESIGN §4: methods the game never calls answer INVALIDCALL and are counted.
bool caseNeverCalled(Env& e, std::string* d)
{
	bool ok = true;
	const uint64_t before = e.nd->stats().neverCalled;
	IDirect3DCubeTexture8* cube = nullptr;
	CHECK(e.dev->CreateCubeTexture(8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube) == D3DERR_INVALIDCALL && !cube, "cube create");
	CHECK(e.nd->stats().neverCalled == before + 1, "not counted");
	return ok;
}


// DESIGN §3 / d3d8_device_child.h: a child's public reference retains the device - the game may release the device first.
bool caseDeviceRetained(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	if (!t)
		return false;
	const int before = g_gxNativeLiveDevices.load();
	e.dev->Release();
	e.devReleased = true;
	if (g_gxNativeLiveDevices.load() != before) {   // destroyed under a live child: t dangles, never touch it
		*d += " device destroyed while a texture holds a public reference";
		return false;
	}
	IDirect3DDevice8* back = nullptr;
	CHECK(SUCCEEDED(t->GetDevice(&back)) && back, "GetDevice from the child");
	if (back)
		back->Release();
	t->Release();
	CHECK(g_gxNativeLiveDevices.load() == before - 1, "device not destroyed with its last child");
	return ok;
}

// DESIGN §7.1 = d3d8_device.cpp:667-990's quirks: a non-null rect array with 0 rects is INVALIDCALL; only the FIRST rect is
// copied; the RT -> SYSTEMMEM fast path (same size, not asymmetric) overwrites the WHOLE destination whatever the rect.
bool caseCopyQuirks(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8 *rt = nullptr, *img = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &rt)), "RT");
	CHECK(SUCCEEDED(e.dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &img)), "image");
	if (!ok)
		return false;
	CHECK(clearSurface(e.dev, rt, 0xff11aa22), "clear RT");
	D3DLOCKED_RECT lr = {};
	img->LockRect(&lr, nullptr, 0);
	std::memset(lr.pBits, 0, size_t(lr.Pitch) * 16);
	img->UnlockRect();
	RECT rects[2] = { { 0, 0, 4, 4 }, { 8, 8, 12, 12 } };
	CHECK(e.dev->CopyRects(rt, rects, 0, img, nullptr) == D3DERR_INVALIDCALL, "0 rects must be INVALIDCALL");
	CHECK(SUCCEEDED(e.dev->CopyRects(rt, rects, 2, img, nullptr)), "fast-path copy");
	img->LockRect(&lr, nullptr, D3DLOCK_READONLY);
	const uint32_t inside = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 1 * lr.Pitch)[1];
	const uint32_t outside = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 14 * lr.Pitch)[14];
	img->UnlockRect();
	CHECK((inside & 0xffffff) == 0x11aa22, "rect pixel");
	CHECK((outside & 0xffffff) == 0x11aa22, "fast path must overwrite the whole surface (pixel outside both rects)");
	// first rect only: a host -> host copy with two rects at points
	IDirect3DSurface8* img2 = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &img2)), "image 2");
	if (img2) {
		img2->LockRect(&lr, nullptr, 0);
		std::memset(lr.pBits, 0, size_t(lr.Pitch) * 16);
		img2->UnlockRect();
		POINT pts[2] = { { 0, 0 }, { 8, 8 } };
		CHECK(SUCCEEDED(e.dev->CopyRects(img, rects, 2, img2, pts)), "host copy");
		img2->LockRect(&lr, nullptr, D3DLOCK_READONLY);
		const uint32_t first = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 1 * lr.Pitch)[1];
		const uint32_t second = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 9 * lr.Pitch)[9];
		img2->UnlockRect();
		CHECK((first & 0xffffff) == 0x11aa22, "first rect copied");
		CHECK(second == 0, "second rect must NOT be copied (DXVK returns inside the loop)");
		img2->Release();
	}
	rt->Release();
	img->Release();
	return ok;
}

// DESIGN §7.3 = d3d9_device.cpp:1100-1175: SYSTEMMEM -> DEFAULT only; only the source's dirty box is copied, then cleared.
bool caseUpdateTexture(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8 *sys = nullptr, *def = nullptr, *man = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys)), "sysmem");
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &def)), "default");
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &man)), "managed");
	if (!ok)
		return false;
	auto fill = [&](IDirect3DTexture8* t, const RECT* r, uint32_t v) {
		D3DLOCKED_RECT lr = {};
		t->LockRect(0, &lr, r, 0);
		const int w = r ? r->right - r->left : 8, h = r ? r->bottom - r->top : 8;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = v;
		t->UnlockRect(0);
	};
	CHECK(e.dev->UpdateTexture(sys, man) == D3DERR_INVALIDCALL, "SYSTEMMEM -> MANAGED must be INVALIDCALL");
	fill(sys, nullptr, 0xff0000ff);
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, def)), "first update (whole: full dirty box)");
	const RECT smallRect = { 2, 2, 4, 4 };   // not `small`: the Windows SDK #defines small (rpcndr.h)
	fill(sys, nullptr, 0xff00ff00);                                  // whole level written...
	sys->LockRect(0, nullptr, nullptr, D3DLOCK_NO_DIRTY_UPDATE);     // (a lock that does not dirty)
	sys->UnlockRect(0);
	// ...but the dirty box is the union of the WRITING locks since the last update: the whole level again. Clear it by an
	// update, then dirty only `smallRect`.
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, def)), "second update");
	D3DLOCKED_RECT lr = {};
	sys->LockRect(0, &lr, nullptr, D3DLOCK_NO_DIRTY_UPDATE);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xffff0000;
	sys->UnlockRect(0);
	sys->AddDirtyRect(&smallRect);
	CHECK(SUCCEEDED(e.dev->UpdateTexture(sys, def)), "dirty-box update");
	IDirect3DSurface8* lvl = nullptr;
	def->GetSurfaceLevel(0, &lvl);
	uint32_t in = 0, out = 0;
	CHECK(lvl && readPixel(e.dev, lvl, 3, 3, &in) && (in & 0xffffff) == 0xff0000, "pixel inside the dirty box");
	CHECK(lvl && readPixel(e.dev, lvl, 6, 6, &out) && (out & 0xffffff) == 0x00ff00, "pixel outside the dirty box must keep the last update");
	if (lvl)
		lvl->Release();
	sys->Release();
	def->Release();
	man->Release();
	return ok;
}

// DESIGN §8.4 = d3d9_device.cpp:560-626: Reset with a live DEFAULT-pool object fails with D3DERR_DEVICELOST and leaves the
// device NotReset (TestCooperativeLevel = DEVICENOTRESET); after the object is released Reset succeeds and the device draws.
bool caseReset(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t)), "DEFAULT texture");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.BackBufferWidth = pp.BackBufferHeight = 32;
	CHECK(e.dev->Reset(&pp) == D3DERR_DEVICELOST, "Reset with a live DEFAULT object must be DEVICELOST");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET, "NotReset state");
	D3DVIEWPORT8 vp = {};   // ResetState unbound render target 0: d3d9 makes the viewport 0x0 (d3d9_device.cpp:1704-1733)
	CHECK(SUCCEEDED(e.dev->GetViewport(&vp)) && vp.Width == 0 && vp.Height == 0, "viewport 0x0 after the failed Reset");
	if (t)
		t->Release();
	CHECK(SUCCEEDED(e.dev->Reset(&pp)), "Reset after release");
	CHECK(e.dev->TestCooperativeLevel() == D3D_OK, "Ok state");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC sd = {};
	CHECK(bb && SUCCEEDED(bb->GetDesc(&sd)) && sd.Width == 32, "new back buffer size");
	uint32_t px = 0;
	CHECK(bb && clearSurface(e.dev, bb, 0xff445566) && readPixel(e.dev, bb, 5, 5, &px) && (px & 0xffffff) == 0x445566, "draws after Reset");
	if (bb)
		bb->Release();
	return ok;
}

// d3d8's parameter checks before Reset touches anything (d3d8_device.cpp:236-247): a windowed FullScreen_PresentationInterval
// other than DEFAULT and COPY with two back buffers are INVALIDCALL, the device keeps its state and back buffer.
bool caseResetParams(Env& e, std::string* d)
{
	bool ok = true;
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.BackBufferWidth = pp.BackBufferHeight = 32;
	pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "windowed + interval IMMEDIATE must be INVALIDCALL");
	pp = presentParams(true, 2);
	pp.SwapEffect = D3DSWAPEFFECT_COPY;
	CHECK(e.dev->Reset(&pp) == D3DERR_INVALIDCALL, "COPY + 2 back buffers must be INVALIDCALL");
	CHECK(e.dev->TestCooperativeLevel() == D3D_OK, "state untouched");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC sd = {};
	CHECK(bb && SUCCEEDED(bb->GetDesc(&sd)) && sd.Width != 32, "back buffer untouched");
	if (bb)
		bb->Release();
	return ok;
}

// DESIGN §5.2 = DXVK getUserConfig + parseUserConfigLine: the file, then DXVK_CONFIG split at ';', the later line winning; quotes,
// comments, '-' signs, an unparsable int keeps -1; a key the device does not reproduce or a [section] refuses the session (S5).
GxNativeConfigDebug configDebug(const Env& e)
{
	GxNativeConfigDebug d;
	d.firstLineWins = e.nd->debug().configFirstLineWins;
	d.ignoreUnknownKeys = e.nd->debug().configIgnoresUnknown;
	d.ignorePresentKeys = e.nd->debug().configIgnoresPresentKeys;
	return d;
}

bool caseConfigOrder(Env& e, std::string* d)
{
	bool ok = true;
	const GxNativeConfigDebug dbg = configDebug(e);
	GxNativeConfig c = GxNativeParseConfig("# a comment\nd3d9.samplerAnisotropy = 4\n  d3d9.samplerAnisotropy=\"8\"  # trailing\n", true,
		"dxvk.logLevel = none;d3d9.samplerAnisotropy = 16", &dbg);
	CHECK(c.refused.empty() && c.samplerAnisotropy == 16, "env after file, later line wins: 16");
	c = GxNativeParseConfig("d3d9.samplerAnisotropy = 4\nd3d9.samplerAnisotropy = 8\n", true, "", &dbg);
	CHECK(c.samplerAnisotropy == 8, "file: later line wins: 8");
	c = GxNativeParseConfig("d3d9.samplerAnisotropy = 4\n", false, "d3d9.samplerAnisotropy = x2", &dbg);
	CHECK(c.samplerAnisotropy == -1, "no file read; unparsable env value keeps DXVK's default -1");
	c = GxNativeParseConfig("", false, "d3d9.samplerAnisotropy=-1", &dbg);
	CHECK(c.samplerAnisotropy == -1 && c.refused.empty(), "-1 parses");
	return ok;
}

bool caseConfigUnknown(Env& e, std::string* d)
{
	bool ok = true;
	const GxNativeConfigDebug dbg = configDebug(e);
	GxNativeConfig c = GxNativeParseConfig("d3d9.samplerLodBias = 1\n", true, "", &dbg);
	CHECK(c.refused.find("d3d9.samplerLodBias") != std::string::npos, "a key the device does not reproduce refuses");
	c = GxNativeParseConfig("", false, "d3d9.maxFrameLatency = 1;d3d9.samplerAnisotropy = 16", &dbg);
	CHECK(!c.refused.empty(), "an env key the device does not reproduce refuses");
	c = GxNativeParseConfig("[GeneralsXZH]\nd3d9.samplerAnisotropy = 2\n", true, "", &dbg);
	CHECK(c.refused.find("section") != std::string::npos, "a [section] refuses");
	c = GxNativeParseConfig("d3d9.samplerAnisotropy-x = 3\n= 5\n", true, "", &dbg);
	CHECK(c.refused.empty() && c.samplerAnisotropy == -1, "a malformed key line is ignored as DXVK ignores it");
	return ok;
}

// D · native-windows-1 13/10/2026: the present keys of the SHIPPED Windows dxvk.conf (setup-run-win64.ps1) are reproduced, parsed as
// DXVK parses them (int32 / Tristate, lowercased; an unparsable value keeps the default), and absent keys keep today's defaults.
bool caseConfigPresentKeys(Env& e, std::string* d)
{
	bool ok = true;
	const GxNativeConfigDebug dbg = configDebug(e);
	GxNativeConfig c = GxNativeParseConfig("dxvk.logLevel = info\nd3d9.presentInterval = 0\ndxvk.tearFree = True\nd3d9.samplerAnisotropy = 16\n", true, "", &dbg);
	CHECK(c.refused.empty() && c.presentInterval == 0 && c.tearFree == 1 && c.samplerAnisotropy == 16, "the shipped Windows conf is reproduced");
	c = GxNativeParseConfig("d3d9.samplerAnisotropy = 16\n", true, "", &dbg);
	CHECK(c.refused.empty() && c.presentInterval == -1 && c.tearFree == -1, "absent keys: DXVK's defaults (-1, Auto)");
	c = GxNativeParseConfig("d3d9.presentInterval = 1\ndxvk.tearFree = FALSE\n", true, "", &dbg);
	CHECK(c.presentInterval == 1 && c.tearFree == 0, "1 + FALSE (case-insensitive)");
	c = GxNativeParseConfig("dxvk.tearFree = True\n", true, "dxvk.tearFree = auto", &dbg);
	CHECK(c.tearFree == -1, "env after file: auto wins");
	c = GxNativeParseConfig("d3d9.presentInterval = 2\n", true, "", &dbg);
	CHECK(c.refused.find("presentInterval") != std::string::npos, "an interval >= 2 (a DXVK frame limit) refuses");
	c = GxNativeParseConfig("d3d9.presentInterval = x\ndxvk.tearFree = maybe\n", true, "", &dbg);
	CHECK(c.refused.empty() && c.presentInterval == -1 && c.tearFree == -1, "unparsable values keep the defaults");
	return ok;
}

// DESIGN S9: a device whose physical device's deviceUUID differs from the adapter's DeviceIdentifier is not
// created (why names it) and leaves nothing behind; the matching UUID creates it. One live device at a time: VkCtx loads the
// device functions into volk's process-global table (VkCtx.cpp volkLoadDevice), so the arm's own device goes first.
bool caseUuidGate(Env& e, std::string* d)
{
	bool ok = true;
	uint8_t mine[VK_UUID_SIZE], other[VK_UUID_SIZE];
	std::memcpy(mine, e.nd->deviceUuid(), VK_UUID_SIZE);
	std::memcpy(other, mine, VK_UUID_SIZE);
	other[0] ^= 0xFF;
	bool zero = true;
	for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
		zero = zero && mine[i] == 0;
	CHECK(!zero, "the device read its deviceUUID");
	const bool unwired = e.nd->debug().uuidGateUnwired;
	GxNativeHost host;
	host.validation = e.nd->ctx().validationEnabled();
	host.prebuildGenericSet = false;
	host.framesInFlight = g_fif;
	e.dev->Release();
	e.devReleased = true;
	StubD3D8 parent;
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	const int before = g_gxNativeLiveDevices.load();
	IDirect3DDevice8* dev = nullptr;
	std::string why;
	host.requireDeviceUuid = unwired ? nullptr : other;
	const HRESULT bad = GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_MIXED_VERTEXPROCESSING, pp, host, &dev, &why);
	CHECK(FAILED(bad) && !dev && why.find("uuid mismatch") != std::string::npos, "a different UUID refuses the device");
	if (dev)
		dev->Release(), dev = nullptr;
	CHECK(g_gxNativeLiveDevices.load() == before, "nothing left behind");
	host.requireDeviceUuid = mine;
	CHECK(SUCCEEDED(GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_MIXED_VERTEXPROCESSING, pp, host, &dev, &why)) && dev,
		"the adapter's own UUID creates the device");
	if (dev)
		dev->Release();
	return ok;
}

// (GxBuffer = DXVK's renaming): more DISCARD lock + draw cycles on one dynamic VB inside ONE frame than the
// slice cap (64). Every slice is then read by the frame still being recorded, so none can be waited for: the buffer grows past the
// cap (sliceOverflows) and every Lock succeeds. Planted: the capped buffer waits for the unsubmitted value and the lock fails.
bool caseBufferDiscardCap(Env& e, std::string* d)
{
	bool ok = true;
	struct V { float x, y, z, rhw; DWORD c; };
	IDirect3DVertexBuffer8* vb = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "CreateVertexBuffer dynamic");
	if (!vb)
		return false;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && clearSurface(e.dev, bb, 0xff000000), "clear back buffer");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	e.dev->SetStreamSource(0, vb, sizeof(V));
	e.dev->BeginScene();
	const int kCycles = 80;   // > the 64-slice cap
	int locked = 0;
	for (int i = 0; i < kCycles; ++i) {
		BYTE* p = nullptr;
		if (FAILED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) || !p)
			break;
		const DWORD c = 0xff000000u | DWORD(0x10 + i);   // the last quad's colour is the one read back
		const V q[4] = { { 0, 0, 0.5f, 1, c }, { 64, 0, 0.5f, 1, c }, { 0, 64, 0.5f, 1, c }, { 64, 64, 0.5f, 1, c } };
		std::memcpy(p, q, sizeof(q));
		vb->Unlock();
		++locked;
		e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	}
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	CHECK(locked == kCycles, ("DISCARD lock " + std::to_string(locked + 1) + " of " + std::to_string(kCycles) + " failed").c_str());
	uint32_t px = 0;
	CHECK(bb && readPixel(e.dev, bb, 20, 20, &px), "read back");
	CHECK(locked != kCycles || (px & 0xffffff) == DWORD(0x10 + kCycles - 1), ("last quad shows " + std::to_string(px & 0xffffff)).c_str());
	const uint64_t overflows = e.nd->resources().stats().sliceOverflows;
	*d += "locks " + std::to_string(locked) + "/" + std::to_string(kCycles) + ", sliceOverflows " + std::to_string(overflows);
	CHECK(locked != kCycles || overflows > 0, "the cap was never reached (the case tests nothing)");
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	vb->Release();
	return ok;
}

// a whole-target Clear waits in the writer's pass as its load op. Clear the back buffer with a temporary
// depth bound, unbind the depth, release it, then CreateImageSurface (which destroys the depth on the device thread, DESIGN §9):
// the clear must still reach the back buffer. Planted: forgetting the depth drops the pass with its pending clear.
bool caseForgetPendingClear(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	uint32_t px = 0;
	CHECK(bb && clearSurface(e.dev, bb, 0xffff0000) && readPixel(e.dev, bb, 8, 8, &px) && (px & 0xffffff) == 0xff0000, "red first");
	IDirect3DSurface8* ds = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &ds)) && ds, "temp depth");
	if (bb && ds) {
		CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ds)), "bind back buffer + temp depth");
		// every aspect (Z + stencil of the D24S8): only then is the whole clear deferred into the pass as its load op - a Z-only
		// clear of a stencil format is recorded at once and survives a forget (the first version of this case missed that)
		CHECK(SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff0000ff, 1.f, 0)),
			"clear blue + depth + stencil");
		CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, nullptr)), "unbind the depth");
		ds->Release();   // the last reference: queued, destroyed by the next Create*
		ds = nullptr;
		px = 0;
		CHECK(readPixel(e.dev, bb, 8, 8, &px), "read back");
		*d += "back buffer after the depth's destruction: " + std::to_string(px & 0xffffff);
		CHECK((px & 0xffffff) == 0x0000ff, "the clear was lost (want blue 255)");
	}
	if (ds)
		ds->Release();
	if (bb)
		bb->Release();
	return ok;
}

// (DXVK Clear -> ClearImageView on the bound depth, whatever the colour target): the water mirror binds a
// single-sample render-target texture over the MSAA depth (WaterRenderObjClass::renderMirror) and clears target + Z. The writer
// draws that pair without its depth, but the Z clear must still reach the depth: a later draw on the MSAA back buffer at z 0.5
// with ZFUNC LESS against the cleared 0.0 is rejected. Planted: the clear follows the depth drop (Z stays 1.0, the quad draws).
bool caseClearDroppedDepth(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8 *bb = nullptr, *ads = nullptr, *rtl = nullptr;
	IDirect3DTexture8* rt = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ads);
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt)) && rt, "RT texture");
	if (rt)
		rt->GetSurfaceLevel(0, &rtl);
	if (bb && ads && rtl) {
		D3DSURFACE_DESC bd = {}, dd = {};
		bb->GetDesc(&bd);
		ads->GetDesc(&dd);
		CHECK(bd.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES && dd.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES, "MSAA x4 back buffer + depth");
		CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ads)) &&
			SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "clear back buffer, Z 1");
		CHECK(SUCCEEDED(e.dev->SetRenderTarget(rtl, ads)) &&
			SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff00ff00, 0.f, 0)), "mirror clear, Z 0");
		CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ads)), "back to the back buffer");
		struct V { float x, y, z, rhw; DWORD c; };
		const V q[4] = { { 0, 0, 0.5f, 1, 0xffff0000 }, { 64, 0, 0.5f, 1, 0xffff0000 }, { 0, 64, 0.5f, 1, 0xffff0000 },
			{ 64, 64, 0.5f, 1, 0xffff0000 } };
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		e.dev->BeginScene();
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))), "draw at z 0.5");
		e.dev->EndScene();
		uint32_t a = 0, m = 0;
		CHECK(readPixel(e.dev, bb, 20, 30, &a) && readPixel(e.dev, rtl, 20, 30, &m), "read back");
		*d += "back buffer " + std::to_string(a & 0xffffff) + ", mirror " + std::to_string(m & 0xffffff);
		CHECK((m & 0xffffff) == 0x00ff00, "the mirror's colour clear");
		CHECK((a & 0xffffff) == 0x000000, "the quad passed Z LESS: the mirror's Z clear never reached the depth");
		CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	}
	if (rtl)
		rtl->Release();
	if (rt)
		rt->Release();
	if (ads)
		ads->Release();
	if (bb)
		bb->Release();
	return ok;
}

// two checked devices one after the other in one process (gxcap-replay --native over several windows).
// The first must uninstall GxOrderCheck before its VkDevice goes: else the second's install() is a no-op while its VkCtx::init
// overwrites volk's hooks, and its "0 violations" is vacuous. Both devices must have their draws seen by the checker. Planted:
// the first device is destroyed without uninstalling (the second sees nothing).
bool caseOrderCheckReinstall(Env& e, std::string* d)
{
	bool ok = true;
	const bool plant = e.nd->debug().orderCheckStaysInstalled;
	e.dev->Release();   // one live native device per process (volk): the case's own devices replace the arm's
	e.devReleased = true;
	for (int i = 0; i < 2; ++i) {
		StubD3D8 parent;
		GxNativeHost host;
		host.orderCheck = true;
		host.framesInFlight = g_fif;
		D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
		IDirect3DDevice8* dev = nullptr;
		std::string why;
		if (FAILED(GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_MIXED_VERTEXPROCESSING, pp, host, &dev, &why)) || !dev) {
			CHECK(false, ("checked device " + std::to_string(i) + ": " + why).c_str());
			break;
		}
		GxNativeFromDevice(dev)->debug().orderCheckStaysInstalled = plant;
		const uint64_t before = GXRender::Rec::GxOrderCheck::counts().draws;
		IDirect3DSurface8* bb = nullptr;
		dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
		struct V { float x, y, z, rhw; DWORD c; };
		const V q[4] = { { 0, 0, 0.5f, 1, 0xffcc4422 }, { 64, 0, 0.5f, 1, 0xffcc4422 }, { 0, 64, 0.5f, 1, 0xffcc4422 },
			{ 64, 64, 0.5f, 1, 0xffcc4422 } };
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		uint32_t px = 0;
		const bool drawn = bb && clearSurface(dev, bb, 0xff000000) && SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))) &&
			readPixel(dev, bb, 20, 30, &px) && (px & 0xffffff) == 0xcc4422;
		const uint64_t seen = GXRender::Rec::GxOrderCheck::counts().draws - before;
		*d += "device " + std::to_string(i) + ": checker saw " + std::to_string(seen) + " draws; ";
		CHECK(drawn, "the quad");
		CHECK(seen > 0, ("device " + std::to_string(i) + "'s draws went past the checker").c_str());
		if (bb)
			bb->Release();
		dev->Release();
	}
	GXRender::Rec::GxOrderCheck::uninstall();   // the planted arm leaves it installed: later arms start clean (no-op otherwise)
	return ok;
}

// GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING) answers the ACTIVE mode (d3d8_device.cpp: d3d9's
// GetSoftwareVertexProcessing), consistent with GetDeviceCaps' MaxVertexBlendMatrixIndex (255 software / 8 hardware). The arm's
// device is MIXED. Planted: the getter answers the translator's slot (always 0).
bool caseSwvpState(Env& e, std::string* d)
{
	bool ok = true;
	DWORD v = 7;
	D3DCAPS8 caps = {};
	CHECK(SUCCEEDED(e.dev->GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, &v)) && v == FALSE, "a MIXED device starts in hardware");
	e.dev->SetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, TRUE);
	e.dev->GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, &v);
	e.dev->GetDeviceCaps(&caps);
	*d += "after TRUE: state " + std::to_string(v) + ", MaxVertexBlendMatrixIndex " + std::to_string(caps.MaxVertexBlendMatrixIndex);
	CHECK(v == TRUE && caps.MaxVertexBlendMatrixIndex == 255, "software mode: the state and the caps agree");
	e.dev->SetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, FALSE);
	e.dev->GetRenderState(D3DRS_SOFTWAREVERTEXPROCESSING, &v);
	e.dev->GetDeviceCaps(&caps);
	CHECK(v == FALSE && caps.MaxVertexBlendMatrixIndex == 8, "hardware mode again");
	return ok;
}

// DESIGN §2.3 + §16f: the session's counters are printed as "[GXNATIVE] stats" every 120 presents and once at teardown ("final"),
// with bad = the counters that fail G-3. The integrator accepted the §16f residual (SetRenderTarget refused for a depth smaller than
// the target) on condition that it is counted there and stays 0 in the game: one such refusal must show as set_target_refused=1,
// bad=1 in both lines. Planted: the refusal is not counted.
bool caseStatsLine(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8 *big = nullptr, *ads = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex)) && tex, "128x128 RT");
	if (tex)
		tex->GetSurfaceLevel(0, &big);
	e.dev->GetDepthStencilSurface(&ads);   // 64x64
	CHECK(big && ads && e.dev->SetRenderTarget(big, ads) == D3DERR_INVALIDCALL, "a 64x64 depth under a 128x128 target is refused");
	for (IDirect3DSurface8* x : { big, ads })
		if (x)
			x->Release();
	if (tex)
		tex->Release();
	for (int i = 0; i < 120; ++i)
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	e.nd->csSync();   // A23-B CS=1 contract (divergence d): the worker prints Present 120's line - read after a drain (CS=0: no-op)
	auto find = [](const char* prefix) {
		const std::vector<std::string> log = logSnapshot();
		for (auto it = log.rbegin(); it != log.rend(); ++it)
			if (it->compare(0, std::strlen(prefix), prefix) == 0)
				return *it;
		return std::string();
	};
	const std::string periodic = find("[GXNATIVE] stats periodic presents=120 ");
	CHECK(!periodic.empty(), "a periodic stats line at 120 presents");
	CHECK(periodic.find(" set_target_refused=1 ") != std::string::npos && periodic.find(" bad=1 ") != std::string::npos,
		("periodic: " + periodic.substr(0, 200)).c_str());
	e.dev->Release();   // teardown prints the final line
	e.devReleased = true;
	const std::string fin = find("[GXNATIVE] stats final ");
	*d += fin.empty() ? std::string("no final line") : fin.substr(0, 160);
	CHECK(fin.find(" set_target_refused=1 ") != std::string::npos, "the final line carries the refusal");
	return ok;
}

// DESIGN §8.6 (-a): once VkCtx has latched VK_ERROR_DEVICE_LOST (at any submit or wait; simulated here with
// VkCtx's own test hook), every Present answers D3DERR_DEVICELOST - offscreen too - and TestCooperativeLevel agrees; the game's
// draws after the loss are not kept in an open frame; "[GXNATIVE] FATAL device lost" is printed once. Planted: Present ignores
// the latched loss (D3D_OK).
bool caseDeviceLost(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	e.nd->ctx().simulateDeviceLost();
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	HRESULT hr[3] = {};
	for (HRESULT& h : hr) {
		if (bb)
			clearSurface(e.dev, bb, 0xff204060);   // the game keeps drawing
		h = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	}
	if (bb)
		bb->Release();
	int fatal = 0;
	for (const std::string& l : logSnapshot())
		fatal += l.compare(0, 30, "[GXNATIVE] FATAL device lost: ") == 0;
	*d += "Present after the loss: " + std::to_string(long(hr[0])) + "/" + std::to_string(long(hr[1])) + "/" + std::to_string(long(hr[2])) +
		", FATAL lines " + std::to_string(fatal);
	CHECK(hr[0] == D3DERR_DEVICELOST && hr[1] == D3DERR_DEVICELOST && hr[2] == D3DERR_DEVICELOST, "every Present answers DEVICELOST");
	CHECK(e.dev->TestCooperativeLevel() == D3DERR_DEVICELOST, "TestCooperativeLevel agrees");
	CHECK(fatal == 1, "one FATAL line");
	CHECK(!e.nd->writer().frameOpen(), "no frame left open after the last Present");
	return ok;
}

// GetIndices = d3d8_device.cpp's: the bound index buffer (AddRef'd) and the SetIndices base vertex; a null out-pointer -> INVALIDCALL
// with *pp cleared. L2's whole-run fallback calls it, so it is not a never-called method any more (rw3-G-3,
// rw3G-l2design-r4 P3). Planted: still counted as never called.
bool caseGetIndices(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DIndexBuffer8* ib = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateIndexBuffer(64, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib)) && ib, "index buffer");
	if (!ib)
		return false;
	const uint64_t never = e.nd->stats().neverCalled;
	e.dev->SetIndices(ib, 7);
	IDirect3DIndexBuffer8* got = nullptr;
	UINT base = 0;
	CHECK(e.dev->GetIndices(&got, &base) == D3D_OK && got == ib && base == 7, "the binding and its base vertex");
	if (got)
		got->Release();
	got = reinterpret_cast<IDirect3DIndexBuffer8*>(1);
	CHECK(e.dev->GetIndices(&got, nullptr) == D3DERR_INVALIDCALL && got == nullptr, "a null base pointer: INVALIDCALL, *pp cleared");
	*d += "neverCalled +" + std::to_string(e.nd->stats().neverCalled - never);
	CHECK(e.nd->stats().neverCalled == never, "GetIndices is not a never-called method");
	e.dev->SetIndices(nullptr, 0);
	ib->Release();
	return ok;
}

// GetTexture / GetStreamSource / GetVertexShader / GetPixelShader answer the binding the matching Set* made: the world/HUD seam's
// composite saves and restores the device state with them, so they are not never-called methods any more (render-hud-seam-2,
// render-integration-5's ruling 13/10). One case, four arms: each planted flag puts its getter back on the never-called count.
bool caseStateGetters(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	IDirect3DVertexBuffer8* vb = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_MANAGED, &vb)) && vb,
		"CreateVertexBuffer");
	if (!t || !vb) {   // release whichever was made: a live resource keeps the device alive past the arm
		if (t)
			t->Release();
		if (vb)
			vb->Release();
		return false;
	}
	const uint64_t never = e.nd->stats().neverCalled;
	const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
	e.dev->SetTexture(1, t);
	e.dev->SetStreamSource(0, vb, 24);
	e.dev->SetVertexShader(fvf);
	e.dev->SetPixelShader(0);
	IDirect3DBaseTexture8* gt = nullptr;
	IDirect3DVertexBuffer8* gvb = nullptr;
	UINT stride = 0;
	DWORD vs = 0, ps = 1;
	CHECK(e.dev->GetTexture(1, &gt) == D3D_OK && gt == t, "GetTexture: the stage's binding");
	CHECK(e.dev->GetStreamSource(0, &gvb, &stride) == D3D_OK && gvb == vb && stride == 24, "GetStreamSource: the buffer and stride");
	CHECK(e.dev->GetVertexShader(&vs) == D3D_OK && vs == fvf, "GetVertexShader: the FVF");
	CHECK(e.dev->GetPixelShader(&ps) == D3D_OK && ps == 0, "GetPixelShader: the handle");
	if (gt)
		gt->Release();
	if (gvb)
		gvb->Release();
	*d += "neverCalled +" + std::to_string(e.nd->stats().neverCalled - never);
	CHECK(e.nd->stats().neverCalled == never, "the four state getters are not never-called methods");
	e.dev->SetTexture(1, nullptr);
	e.dev->SetStreamSource(0, nullptr, 0);
	vb->Release();
	t->Release();
	return ok;
}

// R-SL2 through A22 (render-integration-5 / dlss-integrator 13/10): the seam's D3DTSS_MIPMAPLODBIAS reaches the bound sampler past
// A22's per-draw set reuse. One textured quad drawn three times in one frame: bias 0, bias 0 again (no new sampler, no new texture
// set: A22 reuses), then bias -1 (a new sampler key, so a new set). Both directions in one arm. The key is the REQUESTED bias;
// MoltenVK then clamps the VkSampler's mipLodBias to 0 (GxSamplers.cpp, residual R29): this checks the plumbing, not the picture.
// What a draw BINDS (allocation counts alone do not prove a rebind): the arm wraps the two entry points
// the writer records through (volk globals, the same ones gx_cmd_tap swaps) and remembers each texture set's stage-0 sampler from
// its descriptor writes (binding i = stage i) and the sets last bound for graphics.
PFN_vkCmdBindDescriptorSets g_spyRealBind = nullptr;
PFN_vkUpdateDescriptorSets g_spyRealUpdate = nullptr;
std::map<VkDescriptorSet, VkSampler> g_spyStage0;
std::map<uint32_t, VkDescriptorSet> g_spyBound;
VKAPI_ATTR void VKAPI_CALL spyBindSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
	const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn)
{
	if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS)
		for (uint32_t i = 0; i < n; ++i)
			g_spyBound[first + i] = sets[i];
	g_spyRealBind(cb, bp, l, first, n, sets, nd, dyn);
}
VKAPI_ATTR void VKAPI_CALL spyUpdateSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c)
{
	for (uint32_t i = 0; i < nw; ++i)
		if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && w[i].dstBinding == 0 && w[i].pImageInfo)
			g_spyStage0[w[i].dstSet] = w[i].pImageInfo[0].sampler;
	g_spyRealUpdate(dev, nw, w, nc, c);
}
VkSampler spyBoundStage0()
{
	for (const auto& kv : g_spyBound) {
		const auto it = g_spyStage0.find(kv.second);
		if (it != g_spyStage0.end())
			return it->second;
	}
	return VK_NULL_HANDLE;
}

// shadow-gpu-jitter (R-DL6, D · dlss-shadow-jitter): the shadow volume VS's projection constants c5-c8 carry the world pass's jitter
// exactly as GxWorldPassJitterProjection builds it (what trees/water upload), and stay the clean matrix on classic (no pass) and
// on zero jitter. The X- run is this file built with -DGX_TEST_NO_SHADOW_JITTER (the call removed): the jittered arm must fail.
struct FakeJitterPass final : public IGxWorldPass
{
	float j[2];
	explicit FakeJitterPass(float x, float y) { j[0] = x; j[1] = y; }
	ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE NextTargetsAreWorld(uint32_t) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE Begin(const GxWorldPassBegin&) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE End() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResolveLinear(IDirect3DSurface8*, IDirect3DSurface8*) override { return E_NOTIMPL; }
	BOOL STDMETHODCALLTYPE Supports(uint32_t) override { return FALSE; }
	HRESULT STDMETHODCALLTYPE Upscale(const GxUpscaleCall&) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE HudBegin() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE SetObjectVelocity(IDirect3DSurface8*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetJitter(float ndc[2]) override { ndc[0] = j[0]; ndc[1] = j[1]; return S_OK; }
	BOOL STDMETHODCALLTYPE SharpensItself(uint32_t) override { return FALSE; }
};

bool caseShadowGpuJitter(Env&, std::string* d)
{
	bool ok = true;
	D3DMATRIX P = {};
	P._11 = 1.7f; P._22 = 2.3f; P._33 = 1.01f; P._34 = 1.0f; P._43 = -0.5f; P._13 = 0.02f; P._31 = 0.01f;
	const float jit[2] = { 2.f * 0.25f / 64.f, -2.f * 0.25f / 64.f };
	// the expected jittered matrix, written out independently of GxWorldPassJitterProjection
	D3DMATRIX Pj = P;
	float* pj = &Pj._11;
	for (int r = 0; r < 4; ++r) {
		pj[r * 4 + 0] += pj[r * 4 + 3] * jit[0];
		pj[r * 4 + 1] += pj[r * 4 + 3] * jit[1];
	}
	auto cols = [](const D3DMATRIX& m, float out[4][4]) {
		const float c[4][4] = { { m._11, m._21, m._31, m._41 }, { m._12, m._22, m._32, m._42 }, { m._13, m._23, m._33, m._43 }, { m._14, m._24, m._34, m._44 } };
		std::memcpy(out, c, sizeof(c));
	};
	float want[4][4], clean[4][4], got[4][4];
	cols(Pj, want);
	cols(P, clean);
	CHECK(std::memcmp(want, clean, sizeof(want)) != 0, "(setup) the jitter must move the matrix");
	FakeJitterPass jp(jit[0], jit[1]), zp(0.f, 0.f);
	GxShadowGpuProjConstants(&jp, P, got);
	CHECK(std::memcmp(got, want, sizeof(got)) == 0, "(a) temporal pass + nonzero jitter: c5-c8 are not the jittered projection");
	GxShadowGpuProjConstants(&zp, P, got);
	CHECK(std::memcmp(got, clean, sizeof(got)) == 0, "(b) zero jitter: c5-c8 changed");
	GxShadowGpuProjConstants(nullptr, P, got);
	CHECK(std::memcmp(got, clean, sizeof(got)) == 0, "(c) classic / outside the pass (no interface): c5-c8 changed");
	return ok;
}

bool caseLodBiasSampler(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(8, 8, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "CreateTexture");
	if (!t)
		return false;
	struct V { float x, y, z, rhw; float u, v; };
	const V q[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 0, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTexture(0, t);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);   // with no mip filter the bias is not part of the sampler
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	auto& w = e.nd->writer();
	uint64_t sets[3] = {};
	size_t samplers[3] = {};
	VkSampler bound[3] = {};
	const float bias[3] = { 0.0f, 0.0f, -1.0f };
	// E · seam-worker-2 (phase 2: this arm runs with the A23-B worker ON at CS1): the writer's stats/samplers and the spies are written by
	// the worker - drain (csSync, a no-op at CS0) before the spies are cleared and hooked, around each draw, and before they are unhooked
	e.nd->csSync();
	g_spyStage0.clear();
	g_spyBound.clear();
	g_spyRealBind = vkCmdBindDescriptorSets;
	g_spyRealUpdate = vkUpdateDescriptorSets;
	vkCmdBindDescriptorSets = spyBindSets;
	vkUpdateDescriptorSets = spyUpdateSets;
	CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	for (int i = 0; i < 3; ++i) {
		DWORD v;
		std::memcpy(&v, &bias[i], sizeof(v));
		e.dev->SetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, v);
		e.nd->csSync();
		const uint64_t s0 = w.stats().textureSets;
		const size_t k0 = w.samplers().count();
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))), "DrawPrimitiveUP");
		e.nd->csSync();
		sets[i] = w.stats().textureSets - s0;
		samplers[i] = w.samplers().count() - k0;
		bound[i] = spyBoundStage0();
	}
	CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	e.nd->csSync();
	vkCmdBindDescriptorSets = g_spyRealBind;
	vkUpdateDescriptorSets = g_spyRealUpdate;
	CHECK(bound[0] != VK_NULL_HANDLE, "no texture set seen bound for the first draw");
	CHECK(bound[1] == bound[0], "the same bias again bound another sampler");
	CHECK(bound[2] != VK_NULL_HANDLE && bound[2] != bound[0], "the new bias's draw is still bound to the bias-0 sampler");
	*d += "same bias: +" + std::to_string(samplers[1]) + " sampler +" + std::to_string(sets[1]) + " set; bias -1: +" +
		std::to_string(samplers[2]) + " sampler +" + std::to_string(sets[2]) + " set; bound stage-0 sampler " +
		(bound[1] == bound[0] ? "same" : "CHANGED") + " then " + (bound[2] != bound[0] ? "new" : "SAME");
	CHECK(samplers[1] == 0 && sets[1] == 0, "the same bias again made a new sampler or set");
	CHECK(samplers[2] == 1 && sets[2] == 1, "a new bias did not reach the bound sampler and set");
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	const DWORD zero = 0;
	e.dev->SetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, zero);
	e.dev->SetTexture(0, nullptr);
	t->Release();
	return ok;
}

// #5c: a loss latched DURING a Present (rw3-T's VkCtx latches it in the frame-ring slot wait inside
// ensureFrame) makes THAT Present answer DEVICELOST - not one more D3D_OK - with one FATAL line. Injected right after ensureFrame.
// Planted: Present does not re-check before it returns.
bool caseDeviceLostMidPresent(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	if (csBranch(e)) {   // A23-B CS=1 contract (divergence a), made deterministic with the hold hook: the worker stopped before
		// Present N's command; Present N answers before the loss exists; Present N+1 - its lead fence covers N, released while the
		// caller blocks in it - must answer DEVICELOST (the late check; X-: it does not re-check), then for good, one FATAL
		e.nd->csSync();
		e.nd->debug().loseDeviceMidPresent = true;
		GXRender::Dev::GxNativeCs& cs = e.nd->csStream();
		cs.testHoldAt(cs.queued() + 1);
		const HRESULT a = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		std::thread rel([&cs] {
			while (!cs.testHeld() || !cs.testProducerWaiting())
				std::this_thread::yield();
			cs.testRelease();
		});
		const HRESULT b = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		rel.join();
		const HRESULT c = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		e.nd->csSync();
		int fatal = 0;
		for (const std::string& l : logSnapshot())
			fatal += l.compare(0, 30, "[GXNATIVE] FATAL device lost: ") == 0;
		*d += "Present N: " + std::to_string(long(a)) + ", N+1: " + std::to_string(long(b)) + ", N+2: " + std::to_string(long(c)) +
			", FATAL " + std::to_string(fatal);
		CHECK(a == D3D_OK, "the Present whose command was held answers before the loss exists");
		CHECK(b == D3DERR_DEVICELOST, "the next Present - its fence covered the loss - answers DEVICELOST");
		CHECK(c == D3DERR_DEVICELOST && fatal == 1 && !e.nd->writer().frameOpen(), "terminal, one FATAL, no open frame");
		return ok;
	}
	e.nd->debug().loseDeviceMidPresent = true;
	const HRESULT a = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	const HRESULT b = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	int fatal = 0;
	for (const std::string& l : logSnapshot())
		fatal += l.compare(0, 30, "[GXNATIVE] FATAL device lost: ") == 0;
	*d += "the Present that lost the device: " + std::to_string(long(a)) + ", the next: " + std::to_string(long(b)) + ", FATAL " +
		std::to_string(fatal);
	CHECK(a == D3DERR_DEVICELOST, "the Present during which the loss was latched answers DEVICELOST");
	CHECK(b == D3DERR_DEVICELOST && fatal == 1 && !e.nd->writer().frameOpen(), "terminal, one FATAL, no open frame");
	return ok;
}

std::vector<std::string> finalLines()
{
	std::vector<std::string> out;
	for (const std::string& l : logSnapshot())
		if (l.compare(0, 23, "[GXNATIVE] stats final ") == 0)
			out.push_back(l);
	return out;
}

// DESIGN §2.3 (integrator, rw3-H KG6): the final stats line prints ONCE per session, by whichever comes first - a normal process
// exit with the device still alive (end=exit: the atexit flush, called directly here) or the device's destruction (end=teardown).
// The flush first, then the release: one line, end=exit, carrying the extents. Planted: the final line is not once.
bool caseFinalOnce(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a frame");
	GxNativeFlushFinalStatsAtExit();
	e.dev->Release();
	e.devReleased = true;
	const std::vector<std::string> f = finalLines();
	*d += std::to_string(f.size()) + " final line(s)" + (f.empty() ? std::string() : ": ..." + f[0].substr(f[0].find(" bb=")));
	CHECK(f.size() == 1, "exactly one final line");
	CHECK(!f.empty() && f[0].find(" end=exit ") != std::string::npos && f[0].find(" bb=64x64 swapchain=0x0 ") != std::string::npos,
		"end=exit with the back buffer and (offscreen) swapchain extents");
	GxNativeFlushFinalStatsAtExit();   // the device is gone: nothing more
	CHECK(finalLines().size() == 1, "a flush after the teardown prints nothing");
	return ok;
}

#if defined(GX_VKCTX_HAS_SDL3)   // D · native-windows-1: a window case (presentClear); the Windows standalone test has no SDL3 window
// The teardown alone prints end=teardown (the normal arm order: no flush). Planted: the exit flush prints nothing - here that is
// irrelevant, so this case's X- arm uses resetUnlogged instead: a successful Reset names the extents it made (rw3-H KG6: the
// device was created at 1024x768 and Reset to 1920x1080 with no trace in the log). Reset to 128x128 on the 64x64 window: the line
// names back buffer 128x128 and the swapchain the window gives.
bool caseResetLogged(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	pp.BackBufferWidth = pp.BackBufferHeight = 128;
	pp.hDeviceWindow = static_cast<HWND>(e.window);
	CHECK(e.dev->Reset(&pp) == D3D_OK, "Reset to 128x128");
	std::string line;
	for (const std::string& l : logSnapshot())
		if (l.compare(0, 18, "[GXNATIVE] Reset: ") == 0)
			line = l;
	*d += line.empty() ? std::string("no Reset line") : line;
	CHECK(line.find("back buffer 128x128 msaa=0 swapchain 64x64") != std::string::npos, "the Reset line names both extents");
	CHECK(presentClear(e, 0xff204060), "a frame after the Reset (scaled to the window)");
	e.dev->Release();
	e.devReleased = true;
	const std::vector<std::string> f = finalLines();
	CHECK(f.size() == 1 && f[0].find(" bb=128x128 swapchain=64x64 ") != std::string::npos && f[0].find(" end=teardown ") != std::string::npos,
		"the teardown's final line carries the current extents, end=teardown");
	return ok;
}
#endif

// The atexit wiring itself, end to end: RUN ONLY BY NAME, as its own process (it leaks the device on purpose, so no arm may follow):
//   gx_native_test --arm exit-final   -> stderr must hold "[GXNATIVE] stats final ... end=exit" after main returns
//   gx_native_test --arm X-exit-final -> the flush planted off: no final line at all
bool caseExitFinal(Env& e, std::string* d)
{
	e.devReleased = true;   // never released: the process exits with the device alive
	*d += "device leaked on purpose; the final line is printed at exit (check stderr)";
	return true;
}

// ------------------------------------------------------------------------------------------------------- motion bridge (A35)
// D · render-motion-units-1 13/10/2026 (render-dlss/DESIGN_motion-units.md §2.6-2.7, §9). The game's world pass draws a quad with depth
// into the back buffer; the bridge re-draws it into the object-velocity image O (GXRA = R16G16B16A16_SFLOAT, .xy = current -> previous
// in render pixels, .a = written) with gx:vel-rigid / gx:vel-skin + gx:vel-ps, LESSEQUAL against that depth, no depth write.
// Matrices are identity so clip = position: an NDC shift of (-0.125, +0.125) between the previous and the current frame is (-4, -4)
// pixels on a 64x64 target (+y down).

float halfToFloat(uint16_t h);   // render/ws-motion's (below): NaN reads as infinity, which every velocity check here also rejects

bool readVel(IDirect3DDevice8* dev, IDirect3DSurface8* src, UINT x, UINT y, float out[4])
{
	D3DSURFACE_DESC sd;
	if (FAILED(src->GetDesc(&sd)))
		return false;
	IDirect3DSurface8* img = nullptr;
	if (FAILED(dev->CreateImageSurface(sd.Width, sd.Height, sd.Format, &img)))
		return false;
	bool ok = SUCCEEDED(dev->CopyRects(src, nullptr, 0, img, nullptr));
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		uint16_t h[4];
		std::memcpy(h, static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch + x * 8, 8);
		for (int i = 0; i < 4; ++i)
			out[i] = halfToFloat(h[i]);
		img->UnlockRect();
	} else
		ok = false;
	img->Release();
	return ok;
}

bool velNear(const float v[4], float x, float y, float a) { return std::fabs(v[0] - x) < 0.01f && std::fabs(v[1] - y) < 0.01f && std::fabs(v[3] - a) < 0.01f; }

struct VelScene
{
	IDirect3DSurface8 *bb = nullptr, *ds = nullptr, *o = nullptr;
	IDirect3DTexture8* oTex = nullptr;
	IDirect3DVertexBuffer8* quad = nullptr;   // XYZ, triangle strip, x/y in [-0.5, 0.5], z = 0.5
	DWORD vs = 0, ps = 0;
	~VelScene()
	{
		for (IUnknown* u : { static_cast<IUnknown*>(bb), static_cast<IUnknown*>(ds), static_cast<IUnknown*>(o),
			     static_cast<IUnknown*>(oTex), static_cast<IUnknown*>(quad) })
			if (u)
				u->Release();
	}
};

// E · look-shadowmaps-9 (sm-vel-*, sm-seam-*): with g_velShadow set, the world draws are a CLAMPED OWN-LIGHT CASTER (CAST, the own light
// mu 0.5 / z0 0) in a shadow-map frame, followed by a receiver on the back buffer (velShadowWorld, with the S7 cases below)
IGxShadowMap* g_velShadow = nullptr;
bool g_velProbeRepacked = false;   // velShadowWorld's WORLD probe was drawn from a FULL repack (its precondition)
void velShadowWorld(Env& e, IGxShadowMap* sm, bool begin);

// The world pass: back buffer + depth cleared, the quad (and an optional occluder at z 0.25 over x < 0) drawn FF with depth write.
bool velWorld(Env& e, VelScene& v, bool occluder, std::string* d)
{
	bool ok = true;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &v.bb);
	e.dev->GetDepthStencilSurface(&v.ds);
	CHECK(v.bb && v.ds, "back buffer + depth");
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFORMAT(GXRender::GX_FOURCC_RGBA16F), D3DPOOL_DEFAULT,
		&v.oTex)) && v.oTex, "O texture (GXRA)");
	if (v.oTex)
		v.oTex->GetSurfaceLevel(0, &v.o);
	struct P { float x, y, z; };
	const P q[8] = { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f },
		{ -1.f, -1.f, 0.25f }, { 0.f, -1.f, 0.25f }, { -1.f, 1.f, 0.25f }, { 0.f, 1.f, 0.25f } };
	BYTE* p = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &v.quad)) && v.quad &&
		SUCCEEDED(v.quad->Lock(0, 0, &p, 0)) && p, "quad VB");
	if (p) {
		std::memcpy(p, q, sizeof(q));
		v.quad->Unlock();
	}
	if (!ok)
		return false;
	if (g_velShadow) {   // the quad VB's first upload carried by a frame of its own: written in the measured frame's MAIN, its caster would
		             // be skipped as a late dependency (S7 A1, gx_record_test sm-dep-new) and the world would cast nothing
		e.dev->SetRenderTarget(v.bb, v.ds);
		CHECK(SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)) &&
			SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "the upload frame");
	}
	D3DMATRIX id = {};
	id._11 = id._22 = id._33 = id._44 = 1.f;
	e.dev->SetTransform(D3DTS_WORLD, &id);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
	e.dev->SetTransform(D3DTS_VIEW, &id);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
	e.dev->SetTransform(D3DTS_PROJECTION, &id);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
	e.dev->SetRenderTarget(v.bb, v.ds);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ);
	e.dev->SetStreamSource(0, v.quad, sizeof(P));
	CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	if (g_velShadow)
		velShadowWorld(e, g_velShadow, true);
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "world quad");
	if (occluder)
		CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2)), "occluder");
	if (g_velShadow) {
		velShadowWorld(e, g_velShadow, false);
		e.dev->SetVertexShader(D3DFVF_XYZ);
		e.dev->SetStreamSource(0, v.quad, sizeof(P));   // the receiver's DrawPrimitiveUP left stream 0 unset
	}
	return ok;
}

// The bridge's replay setup: O bound with the world depth, cleared to 0 (A = 0 = unwritten), depth test only, the velocity programs.
void velBegin(Env& e, VelScene& v, const uint32_t* vsCode, const uint32_t* decl)
{
	e.dev->CreateVertexShader(reinterpret_cast<const DWORD*>(decl), reinterpret_cast<const DWORD*>(vsCode), &v.vs, 0);
	e.dev->CreatePixelShader(reinterpret_cast<const DWORD*>(GXRender::gxVelPs), &v.ps);
	e.dev->SetRenderTarget(v.o, v.ds);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.f, 0);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetVertexShader(v.vs);
	e.dev->SetPixelShader(v.ps);
}

// c0-3 world*view, c4-7 projection, c8-11 previous clip matrix (all identity, the previous one shifted by prevShift in NDC),
// c12 = (0.5 W, -0.5 H, zEps, 0), c13 = jitter (0)
void velConstants(Env& e, float prevShiftX, float prevShiftY)
{
	float c[GXRender::GX_VEL_CONSTANTS][4] = {};
	for (int m = 0; m < 3; ++m)
		for (int i = 0; i < 4; ++i)
			c[m * 4 + i][i] = 1.f;
	c[8][3] = prevShiftX;   // transposed: row i of the upload = column i of the d3d matrix, whose row 3 is the translation
	c[9][3] = prevShiftY;
	c[12][0] = 32.f;
	c[12][1] = -32.f;
	c[12][2] = 0.f;   // zEps 0: the FF-exact position passes LESSEQUAL by itself
	e.dev->SetVertexShaderConstant(0, c, GXRender::GX_VEL_CONSTANTS);
}

bool velEnd(Env& e, VelScene& v)
{
	e.dev->SetVertexShader(D3DFVF_XYZ);
	e.dev->SetPixelShader(0);
	e.dev->SetRenderTarget(v.bb, v.ds);
	return SUCCEEDED(e.dev->EndScene());
}

bool caseVelRigid(Env& e, std::string* d)
{
	bool ok = true;
	VelScene v;
	if (!velWorld(e, v, false, d))
		return false;
	velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
	velConstants(e, -0.125f, 0.125f);
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "velocity draw");
	CHECK(velEnd(e, v), "EndScene");
	float in[4] = {}, corner[4] = {}, out[4] = {};
	CHECK(readVel(e.dev, v.o, 32, 32, in) && velNear(in, -4.f, -4.f, 1.f), ("moved quad: O = (" + std::to_string(in[0]) + ", " +
		std::to_string(in[1]) + ", a " + std::to_string(in[3]) + "), want (-4, -4, a 1)").c_str());
	CHECK(readVel(e.dev, v.o, 17, 46, corner) && velNear(corner, -4.f, -4.f, 1.f), "edge pixel of the quad covered");   // LESSEQUAL tie
	CHECK(readVel(e.dev, v.o, 4, 4, out) && out[3] == 0.f, "outside the quad: unwritten (A = 0)");
	// control: the same draw with the previous matrix = the current one writes zero velocity - the result follows c8-11
	velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
	velConstants(e, 0.f, 0.f);
	e.dev->BeginScene();
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "control draw");
	velEnd(e, v);
	CHECK(readVel(e.dev, v.o, 32, 32, in) && velNear(in, 0.f, 0.f, 1.f), "unmoved quad: O = (0, 0, a 1)");
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	return ok;
}

bool caseVelSkin(Env& e, std::string* d)
{
	bool ok = true;
	VelScene v;
	if (!velWorld(e, v, false, d))
		return false;
	// the skin bridge's buffer: (current, previous) world-space positions, 24-B stride; the previous ones shifted (-0.125, +0.125)
	struct S { float x, y, z, px, py, pz; };
	const S moved[4] = { { -0.5f, -0.5f, 0.5f, -0.625f, -0.375f, 0.5f }, { 0.5f, -0.5f, 0.5f, 0.375f, -0.375f, 0.5f },
		{ -0.5f, 0.5f, 0.5f, -0.625f, 0.625f, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.375f, 0.625f, 0.5f } };
	S still[4];
	for (int i = 0; i < 4; ++i)
		still[i] = { moved[i].x, moved[i].y, moved[i].z, moved[i].x, moved[i].y, moved[i].z };
	IDirect3DVertexBuffer8* vb = nullptr;
	BYTE* p = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(moved) + sizeof(still), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb)) && vb &&
		SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "skin VB");
	if (!ok)
		return false;
	std::memcpy(p, moved, sizeof(moved));
	std::memcpy(p + sizeof(moved), still, sizeof(still));
	vb->Unlock();
	velBegin(e, v, GXRender::gxVelSkinVs, GXRender::gxVelSkinDecl);
	velConstants(e, 0.f, 0.f);   // skin: view, projection and the previous view*projection are all identity here
	e.dev->SetStreamSource(0, vb, sizeof(S));
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "skin velocity draw");
	velEnd(e, v);
	float in[4] = {};
	CHECK(readVel(e.dev, v.o, 32, 32, in) && velNear(in, -4.f, -4.f, 1.f), ("skinned: O = (" + std::to_string(in[0]) + ", " +
		std::to_string(in[1]) + "), want (-4, -4) from the previous-position stream").c_str());
	// control: previous = current positions in the stream: zero velocity
	velBegin(e, v, GXRender::gxVelSkinVs, GXRender::gxVelSkinDecl);
	velConstants(e, 0.f, 0.f);
	e.dev->SetStreamSource(0, vb, sizeof(S));
	e.dev->BeginScene();
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2)), "skin control draw");
	velEnd(e, v);
	CHECK(readVel(e.dev, v.o, 32, 32, in) && velNear(in, 0.f, 0.f, 1.f), "skinned, previous = current: O = (0, 0)");
	e.dev->SetStreamSource(0, nullptr, 0);
	vb->Release();
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	return ok;
}

// dlss-integrator ruling (a): the replay depth-tests against this frame's world depth - an occluded unit writes no velocity
bool caseVelOccluded(Env& e, std::string* d)
{
	bool ok = true;
	VelScene v;
	if (!velWorld(e, v, true, d))
		return false;
	velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
	velConstants(e, -0.125f, 0.125f);
	e.dev->SetStreamSource(0, v.quad, 12);
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "velocity draw");
	velEnd(e, v);
	float hidden[4] = {}, seen[4] = {};
	CHECK(readVel(e.dev, v.o, 24, 32, hidden) && hidden[3] == 0.f, ("behind the occluder: A = " + std::to_string(hidden[3]) + ", want 0").c_str());
	CHECK(readVel(e.dev, v.o, 40, 32, seen) && velNear(seen, -4.f, -4.f, 1.f), "the visible half is written");
	return ok;
}

// fdepth-vel-occluded (D · look-fdepth-plus; the integrator's condition b, 17/10): vel-occluded with F-depth+'s tap between the world's
// opaque draws (velWorld: the occluder) and the unit replay, as RTS3DScene::Flush calls it before the transparents: the occluded unit
// still writes no velocity, the visible half its own, and the depth copy holds the occluder (0.25 at x 16). Planted: the copy records
// nothing - the probe must read no occluder depth; the vectors must not care.
IGxSceneTap* sceneTap(Env& e);   // F-depth+ helpers (below, with the fdepth-* arms)
bool fdNear(float a, float b, float tol);

bool caseFdepthVelOccluded(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap && tap->Wanted(), "IGxSceneTap + the probe reader");
	e.nd->testScene().testProbeTexel(16, 32);
	VelScene v;
	if (!velWorld(e, v, true, d)) {
		tap->Release();
		return false;
	}
	CHECK(SUCCEEDED(tap->OpaqueDone()), "OpaqueDone");
	velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
	velConstants(e, -0.125f, 0.125f);
	e.dev->SetStreamSource(0, v.quad, 12);
	CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "velocity draw");
	velEnd(e, v);
	float hidden[4] = {}, seen[4] = {};
	CHECK(readVel(e.dev, v.o, 24, 32, hidden) && hidden[3] == 0.f, ("behind the occluder: A = " + std::to_string(hidden[3]) + ", want 0").c_str());
	CHECK(readVel(e.dev, v.o, 40, 32, seen) && velNear(seen, -4.f, -4.f, 1.f), "the visible half is written");
	float pr[4] = { -2.f, -2.f, -2.f, -2.f };
	CHECK(e.nd->testReadSceneProbe(pr) && fdNear(pr[0], 0.25f, 1e-3f), ("the depth copy at x 16: " + std::to_string(pr[0]) + ", want 0.25").c_str());
	tap->Release();
	return ok;
}

// + r2 #4/#6: the FF-exactness claim itself, with an EQUAL depth gate (a replay depth that is merely nearer fails).
// (a) parallel quad: a rotated + translated world, a translated view, a perspective projection; the replay builds its constants as the
//     bridge does (WorldView = GxPackFFWorld's dxm product, columns uploaded). Under ZFUNC EQUAL the replay must cover EXACTLY the FF
//     draw's pixels, with one uniform velocity: the quad's z = 0.5 + view 2 = depth 2.5, and the previous world shifted by dx = -0.1
//     moves it -0.1 * cot(30 deg) / 2.5 NDC x 32 = -2.217025 px.
// (b) tilted quad (z = 0.4 y: clip w varies across it) drawn by FF with a JITTERED projection (+0.25 px right/down, as render-motion-1's
//     device jitters FF draws through the projection); the replay gets that same projection in c4-7 and the jitter in c13, previous =
//     current. Under EQUAL it must cover exactly the FF pixels, and its velocity must be 0 (the jitter is excluded) on every one.
bool readAll(IDirect3DDevice8* dev, IDirect3DSurface8* src, size_t bpp, std::vector<uint8_t>* out, UINT* pitch)
{
	D3DSURFACE_DESC sd;
	if (FAILED(src->GetDesc(&sd)))
		return false;
	IDirect3DSurface8* img = nullptr;
	if (FAILED(dev->CreateImageSurface(sd.Width, sd.Height, sd.Format, &img)))
		return false;
	bool ok = SUCCEEDED(dev->CopyRects(src, nullptr, 0, img, nullptr));
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		out->resize(size_t(sd.Height) * size_t(lr.Pitch));
		std::memcpy(out->data(), lr.pBits, out->size());
		*pitch = UINT(lr.Pitch);
		img->UnlockRect();
	} else
		ok = false;
	img->Release();
	(void)bpp;
	return ok;
}

bool caseVelRigidPersp(Env& e, std::string* d)
{
	bool ok = true;
	VelScene v;
	if (!velWorld(e, v, false, d))   // targets + the parallel quad VB (the identity world pass it draws is overwritten below)
		return false;
	e.dev->EndScene();   // velWorld leaves its scene open; each arm below opens and closes its own
	struct Pv { float x, y, z; };
	const Pv tilt[4] = { { -0.5f, -0.5f, -0.2f }, { 0.5f, -0.5f, -0.2f }, { -0.5f, 0.5f, 0.2f }, { 0.5f, 0.5f, 0.2f } };
	IDirect3DVertexBuffer8* tvb = nullptr;
	BYTE* tp = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(tilt), D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &tvb)) && tvb &&
		SUCCEEDED(tvb->Lock(0, 0, &tp, 0)) && tp, "tilted quad VB");
	if (!ok)
		return false;
	std::memcpy(tp, tilt, sizeof(tilt));
	tvb->Unlock();
	const float c30 = 0.8660254f, s30 = 0.5f, ys = 1.7320508f, zn = 1.f, zf = 100.f;
	D3DMATRIX W = {}, V = {}, P = {};
	W._11 = c30; W._12 = s30; W._21 = -s30; W._22 = c30; W._33 = 1.f; W._41 = 0.2f; W._42 = -0.1f; W._44 = 1.f;
	V._11 = V._22 = V._33 = V._44 = 1.f; V._43 = 2.f;
	P._11 = ys; P._22 = ys; P._33 = zf / (zf - zn); P._34 = 1.f; P._43 = -zn * zf / (zf - zn);
	auto rows = [](const float m[16], float c[][4], int base) { for (int i = 0; i < 4; ++i) for (int r = 0; r < 4; ++r) c[base + i][r] = m[r * 4 + i]; };
	// One arm: FF draw of vb (world Wff, proj = Pff) then the replay under EQUAL with c8-11 = prevClip (D3D layout) - gx:vel-rigid
	// over vb, or gx:vel-skin over skinVb ((current, previous) world positions, 24-B stride) when given; returns the coverage
	// mismatch count and the worst |velocity - want(x, y)| over the replay's pixels.
	auto arm = [&](IDirect3DVertexBuffer8* vb, const D3DMATRIX& Wff, IDirect3DVertexBuffer8* skinVb, const D3DMATRIX& Pff, const float jit[2],
	               const float prevClip[16], const std::function<void(UINT, UINT, float*, float*)>& want, int* covered, int* mismatch,
	               int* badVel, float* worst) -> bool {
		bool good = true;
		e.dev->SetTransform(D3DTS_WORLD, &Wff);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
		e.dev->SetTransform(D3DTS_VIEW, &V);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
		e.dev->SetTransform(D3DTS_PROJECTION, &Pff);   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A35 velocity cases)
		e.dev->SetRenderTarget(v.bb, v.ds);
		e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		e.dev->SetVertexShader(D3DFVF_XYZ);
		e.dev->SetStreamSource(0, vb, 12);
		e.dev->BeginScene();
		good = good && SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
		float wv[16], pm[16];
		GXRender::dxm::store(GXRender::dxm::loadD3D(&V._11) * GXRender::dxm::loadD3D(&Wff._11), wv);   // GxPackFFWorld
		std::memcpy(pm, &Pff._11, 64);
		float c[GXRender::GX_VEL_CONSTANTS][4] = {};
		rows(wv, c, 0);
		rows(pm, c, 4);
		rows(prevClip, c, 8);
		c[12][0] = 32.f;
		c[12][1] = -32.f;
		c[13][0] = jit[0];
		c[13][1] = jit[1];
		if (skinVb)
			velBegin(e, v, GXRender::gxVelSkinVs, GXRender::gxVelSkinDecl);
		else
			velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);   // the exactness gate: only a bit-identical depth passes
		e.dev->SetVertexShaderConstant(0, c, GXRender::GX_VEL_CONSTANTS);
		e.dev->SetStreamSource(0, skinVb ? skinVb : vb, skinVb ? 24 : 12);
		good = good && SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
		velEnd(e, v);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		std::vector<uint8_t> bb, o;
		UINT bbPitch = 0, oPitch = 0;
		good = good && readAll(e.dev, v.bb, 4, &bb, &bbPitch) && readAll(e.dev, v.o, 8, &o, &oPitch);
		if (!good)
			return false;
		*covered = *mismatch = *badVel = 0;
		*worst = 0.f;
		for (UINT y = 0; y < 64; ++y)
			for (UINT x = 0; x < 64; ++x) {
				uint32_t px;
				std::memcpy(&px, bb.data() + y * bbPitch + x * 4, 4);
				uint16_t h[4];
				std::memcpy(h, o.data() + y * oPitch + x * 8, 8);
				const bool ff = (px & 0xffffff) != 0, written = halfToFloat(h[3]) > 0.5f;
				*covered += ff;
				*mismatch += ff != written;
				if (written) {
					float wx = 0.f, wy = 0.f;
					want(x, y, &wx, &wy);
					const float dx = std::fabs(halfToFloat(h[0]) - wx), dy = std::fabs(halfToFloat(h[1]) - wy);
					*worst = std::max(*worst, std::max(dx, dy));
					// a NaN component counts; so does any alpha but gx:vel-ps's exact 1 - the shared decoder
					// reads a half NaN as infinity, which passes the "written" test above
					*badVel += !(dx <= 0.01f && dy <= 0.01f && halfToFloat(h[3]) == 1.f);
				}
			}
		return true;
	};
	int covered = 0, mismatch = 0, badVel = 0;
	float worst = 0.f;
	const float noJit[2] = { 0.f, 0.f };
	auto prevClipOf = [&](const D3DMATRIX& Wp, const D3DMATRIX& Vp, float out[16]) {
		GXRender::dxm::store(GXRender::dxm::loadD3D(&P._11) * (GXRender::dxm::loadD3D(&Vp._11) * GXRender::dxm::loadD3D(&Wp._11)), out);
	};
	D3DMATRIX Wp = W;
	Wp._41 -= 0.1f;
	float pcA[16], pcB[16], pcC[16];
	prevClipOf(Wp, V, pcA);
	CHECK(arm(v.quad, W, nullptr, P, noJit, pcA, [](UINT, UINT, float* wx, float* wy) { *wx = -2.217025f; *wy = 0.f; }, &covered, &mismatch, &badVel,
		&worst), "(a) parallel quad arm");
	CHECK(covered > 200, ("(a) FF quad covers " + std::to_string(covered) + " px").c_str());
	CHECK(mismatch == 0, ("(a) EQUAL-depth replay coverage differs from the FF draw on " + std::to_string(mismatch) + " px").c_str());
	CHECK(badVel == 0, ("(a) velocity off (-2.217025, 0) by up to " + std::to_string(worst) + " px on " + std::to_string(badVel) + " px").c_str());
	// (b) jittered projection, exactly as GxWorldPassJitterProjection builds it, on FF and replay alike
	const float jit[2] = { 2.f * 0.25f / 64.f, -2.f * 0.25f / 64.f };
	D3DMATRIX Pj = P;
	float* pj = &Pj._11;
	for (int r = 0; r < 4; ++r) {
		pj[r * 4 + 0] += pj[r * 4 + 3] * jit[0];
		pj[r * 4 + 1] += pj[r * 4 + 3] * jit[1];
	}
	prevClipOf(W, V, pcB);
	CHECK(arm(tvb, W, nullptr, Pj, jit, pcB, [](UINT, UINT, float* wx, float* wy) { *wx = 0.f; *wy = 0.f; }, &covered, &mismatch, &badVel, &worst),
		"(b) tilted jittered arm");
	CHECK(covered > 200, ("(b) FF tilted quad covers " + std::to_string(covered) + " px").c_str());
	CHECK(mismatch == 0, ("(b) EQUAL-depth jittered replay coverage differs from the jittered FF draw on " + std::to_string(mismatch) + " px").c_str());
	CHECK(badVel == 0, ("(b) jitter leaked into the velocity: up to " + std::to_string(worst) + " px on " + std::to_string(badVel) + " px").c_str());
	// (c): the tilted quad (clip w varies) with the previous camera rotated 10 deg about its view axis: the
	// previous view-space position is (x c - y s, x s + y c, z), so every pixel's previous NDC is its current NDC rotated by 10 deg -
	// a per-pixel velocity that varies across the quad and does not depend on depth. A shader that divided the clip positions at the
	// vertices (then interpolated) would miss it wherever w varies.
	const float cr = 0.98480775f, sr = 0.17364818f;
	D3DMATRIX Rz = {}, Vr = {};
	Rz._11 = cr; Rz._12 = sr; Rz._21 = -sr; Rz._22 = cr; Rz._33 = 1.f; Rz._44 = 1.f;
	GXRender::dxm::store(GXRender::dxm::loadD3D(&Rz._11) * GXRender::dxm::loadD3D(&V._11), &Vr._11);   // V then Rz (row vectors)
	prevClipOf(W, Vr, pcC);
	const auto rotField = [&](UINT x, UINT y, float* wx, float* wy) {
		// D3D9 pixel centres sit on integer coordinates (the translator's half-pixel viewport offset, GxD3D8Translate.cpp:770-773):
		// pixel x samples NDC x/32 - 1, not (x + 0.5)/32 - 1
		const float nx = float(x) / 32.f - 1.f, ny = 1.f - float(y) / 32.f;
		*wx = (nx * cr - ny * sr - nx) * 32.f;
		*wy = (nx * sr + ny * cr - ny) * -32.f;
	};
	CHECK(arm(tvb, W, nullptr, P, noJit, pcC, rotField, &covered, &mismatch, &badVel, &worst), "(c) tilted rotating-camera arm");
	CHECK(mismatch == 0, ("(c) EQUAL-depth replay coverage differs from the FF draw on " + std::to_string(mismatch) + " px").c_str());
	CHECK(badVel == 0, ("(c) per-pixel velocity off the rotation's by up to " + std::to_string(worst) + " px on " + std::to_string(badVel) + " px").c_str());
	// Skins (vel-skin's identity matrices cannot see the previous-camera path). A skin draws CPU-skinned
	// WORLD-space positions with world = identity; the bridge copies them into its (current, previous) buffer and replays with
	// c0-3 = view * identity, c4-7 = the projection, c8-11 = the previous view * projection (GxMotionUnits.cpp onDraw).
	// (d) the tilted quad's world positions, previous = current in the stream, previous camera = arm (c)'s: EQUAL coverage and (c)'s
	//     per-pixel field - only c8-11 carries the motion (a skin program that ignored them would write 0).
	// (e) the parallel quad's world positions, previous = current shifted by world dx -0.1, camera unchanged: arm (a)'s uniform
	//     (-2.217025, 0) - only the previous-position stream carries the motion (a program that read v0 for the previous would write 0).
	D3DMATRIX I = {};
	I._11 = I._22 = I._33 = I._44 = 1.f;
	auto skinArm = [&](const Pv* obj, float prevDx, const float prevClip[16], const std::function<void(UINT, UINT, float*, float*)>& want,
	                   const char* name) -> bool {
		Pv wpos[4];
		float sk[4][6];
		for (int i = 0; i < 4; ++i) {   // object -> world by W (row vector), one rounding per value: FF and replay read the same floats
			wpos[i] = { obj[i].x * W._11 + obj[i].y * W._21 + W._41, obj[i].x * W._12 + obj[i].y * W._22 + W._42, obj[i].z };
			sk[i][0] = wpos[i].x; sk[i][1] = wpos[i].y; sk[i][2] = wpos[i].z;
			sk[i][3] = wpos[i].x + prevDx; sk[i][4] = wpos[i].y; sk[i][5] = wpos[i].z;
		}
		IDirect3DVertexBuffer8 *ffVb = nullptr, *skVb = nullptr;
		BYTE *a = nullptr, *b = nullptr;
		bool good = SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(wpos), D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &ffVb)) && ffVb &&
			SUCCEEDED(ffVb->Lock(0, 0, &a, 0)) && a && SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(sk), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
			&skVb)) && skVb && SUCCEEDED(skVb->Lock(0, 0, &b, 0)) && b;
		if (good) {
			std::memcpy(a, wpos, sizeof(wpos));
			std::memcpy(b, sk, sizeof(sk));
			ffVb->Unlock();
			skVb->Unlock();
			good = arm(ffVb, I, skVb, P, noJit, prevClip, want, &covered, &mismatch, &badVel, &worst);
		}
		e.dev->SetStreamSource(0, nullptr, 0);
		for (IDirect3DVertexBuffer8* u : { ffVb, skVb })
			if (u)
				u->Release();
		if (!good)
			*d += std::string(" ") + name + ": setup or draw failed;";
		return good;
	};
	float pcD[16], pcE[16];
	prevClipOf(I, Vr, pcD);
	CHECK(skinArm(tilt, 0.f, pcD, rotField, "(d)"), "(d) skinned tilted rotating-camera arm");
	CHECK(covered > 200, ("(d) FF skinned quad covers " + std::to_string(covered) + " px").c_str());
	CHECK(mismatch == 0, ("(d) EQUAL-depth skin replay coverage differs from the FF draw on " + std::to_string(mismatch) + " px").c_str());
	CHECK(badVel == 0, ("(d) skin per-pixel velocity off the rotation's by up to " + std::to_string(worst) + " px on " + std::to_string(badVel) + " px").c_str());
	const Pv flat[4] = { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f } };   // velWorld's quad
	prevClipOf(I, V, pcE);
	CHECK(skinArm(flat, -0.1f, pcE, [](UINT, UINT, float* wx, float* wy) { *wx = -2.217025f; *wy = 0.f; }, "(e)"), "(e) skinned moved-vertices arm");
	CHECK(covered > 200, ("(e) FF skinned quad covers " + std::to_string(covered) + " px").c_str());
	CHECK(mismatch == 0, ("(e) EQUAL-depth skin replay coverage differs from the FF draw on " + std::to_string(mismatch) + " px").c_str());
	CHECK(badVel == 0, ("(e) skin velocity off (-2.217025, 0) by up to " + std::to_string(worst) + " px on " + std::to_string(badVel) + " px").c_str());
	tvb->Release();
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	return ok;
}

// key off (host.motionUnits false): the GXRA format is not registered (O cannot even be created) and the velocity programs have no
// module, so a gx:vel draw is refused (key off probes and knows no new format)
bool caseVelOff(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* oTex = nullptr;
	CHECK(FAILED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFORMAT(GXRender::GX_FOURCC_RGBA16F), D3DPOOL_DEFAULT, &oTex)) &&
		!oTex, "key off: a GXRA texture must not be creatable");
	if (oTex)
		oTex->Release();
	IDirect3DSurface8 *bb = nullptr, *ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	struct P { float x, y, z; };
	const P q[4] = { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { -0.5f, 0.5f, 0.5f }, { 0.5f, 0.5f, 0.5f } };
	IDirect3DVertexBuffer8* vb = nullptr;
	BYTE* p = nullptr;
	CHECK(bb && ds && SUCCEEDED(e.dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &vb)) && vb &&
		SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "setup");
	if (p) {
		std::memcpy(p, q, sizeof(q));
		vb->Unlock();
	}
	const uint64_t refused0 = e.nd->stats().drawsRefused;
	DWORD vs = 0, ps = 0;
	e.dev->CreateVertexShader(reinterpret_cast<const DWORD*>(GXRender::gxVelRigidDecl), reinterpret_cast<const DWORD*>(GXRender::gxVelRigidVs), &vs, 0);
	e.dev->CreatePixelShader(reinterpret_cast<const DWORD*>(GXRender::gxVelPs), &ps);
	e.dev->SetRenderTarget(bb, ds);
	e.dev->SetVertexShader(vs);
	e.dev->SetPixelShader(ps);
	velConstants(e, -0.125f, 0.125f);
	e.dev->SetStreamSource(0, vb, sizeof(P));
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.dev->SetVertexShader(D3DFVF_XYZ);
	e.dev->SetPixelShader(0);
	e.dev->SetStreamSource(0, nullptr, 0);
	CHECK(e.nd->stats().drawsRefused == refused0 + 1, "key off: the gx:vel draw is refused (no module)");
	for (IUnknown* u : { static_cast<IUnknown*>(bb), static_cast<IUnknown*>(ds), static_cast<IUnknown*>(vb) })
		if (u)
			u->Release();
	return ok;
}

// ------------------------------------------------------------------------------------- lever (3)'s exit path (PREREG_T A21 am. 4)
// A device the game never destroys keeps gx-vk-submit running while the process ends, so the exit flush (the atexit hook, also
// the game's FlushFinalStats) quiesces it first. Both arms queue a frame (submit + present) behind the hold; a helper releases it
// only once the flush is blocked in VkCtx::quiesce (it gives up after 3 s).

// Queues one frame behind the hold. False (hold released) when it did not queue as exactly a held submit + present.
bool queueHeldFrame(Env& e, V::SubmitThreadStats* s0)
{
	V::VkCtx& ctx = e.nd->ctx();
	if (!presentClear(e, 0xff204060) || !presentClear(e, 0xff204060))
		return false;
	e.nd->ctx().waitIdle();
	*s0 = ctx.threadStats();
	ctx.testHoldAt(e.nd->ctx().queue(V::QueueKind::Graphics).timeline.lastSubmitted() + 1);
	const bool queuedOk = presentClear(e, 0xff204060);
	e.nd->csSync();   // A23-B CS=1: the frame's submit and present are queued by the worker (CS=0: a no-op)
	const V::SubmitThreadStats sq = ctx.threadStats();
	// windowed: the frame's submit + its present are two held ops; offscreen (-nowin arms) Present ends the frame, so it is one held submit
	const uint64_t heldOps = e.window ? 2 : 1;
	if (queuedOk && sq.enqueued - s0->enqueued == heldOps && sq.submits == s0->submits)
		return true;
	ctx.testRelease();
	return false;
}

// Releases the hold once a thread is blocked in quiesce (or after 3 s, or when *stop). *sawBlocked / *released record which;
// `announce`: a stderr line BEFORE the release (exit-quiesce's order check: it must precede the final line the flush then prints).
void releaseWhenQuiescing(V::VkCtx* ctx, std::atomic<bool>* sawBlocked, std::atomic<bool>* released, std::atomic<bool>* stop,
	bool announce = false)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!stop->load() && std::chrono::steady_clock::now() < end) {
		if (ctx->testQuiesceWaiters() > 0) {
			sawBlocked->store(true);
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (announce)
		std::fprintf(stderr, "[TEST] exit-quiesce: helper releasing the hold (%s)\n",
			sawBlocked->load() ? "the flush is blocked in quiesce" : "gave up: no quiesce seen in 3 s");
	released->store(true);
	ctx->testRelease();
}

// FG/Reflex submit-thread guard (FG P0 §2): a host that REQUESTS frame generation (GXFrameGen > 0) or Reflex (GXReflex > 0) gets the
// submit thread OFF and the log line naming why; a plain host keeps today's default (ON). X-: the guard skipped leaves the thread ON.
bool fgGuardCase(Env& e, std::string* d, bool expectOff)
{
	const bool threaded = e.nd->ctx().submitThreaded();
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXVK] submit thread OFF: FrameGen/Reflex requested") != std::string::npos;
	*d = std::string("submitThreaded=") + (threaded ? "1" : "0") + " guardLine=" + (line ? "1" : "0");
	return expectOff ? (!threaded && line) : (threaded && !line);
}
bool caseFgGuardFrameGen(Env& e, std::string* d) { return fgGuardCase(e, d, true); }
bool caseFgGuardReflex(Env& e, std::string* d) { return fgGuardCase(e, d, true); }
bool caseFgGuardOff(Env& e, std::string* d) { return fgGuardCase(e, d, false); }
// E · seam-worker-2 (dlss-integrator's ruling 18/10, seam2 design r1 P1): a LOADED Streamline alone (no FrameGen, no Reflex: DLSS SR
// or GX_STREAMLINE_FORCE_LOAD=1) turns the submit thread off too - its present hooks must not run on gx-vk-submit while the producer
// evaluates. The harness injects a fake host.streamline (fgguard-streamline): a non-Streamline gx_device never dereferences it (dlssInit and
// the loader root are stubs there), a Streamline build would - SKIP there. X- (fgGuardSkipped): the guard skipped leaves the thread ON.
bool caseFgGuardStreamline(Env& e, std::string* d)
{
	const bool threaded = e.nd->ctx().submitThreaded();
	bool line = false, req = false;
	for (const std::string& l : logSnapshot()) {
		line = line || (l.find("[GXVK] submit thread OFF: FrameGen/Reflex requested") != std::string::npos &&
			l.find("framegen=0 reflex=0 streamline=1") != std::string::npos);
		req = req || l.find("[GXVK] fg request framegen=0 reflex=0 submitThread=off streamline=1") != std::string::npos;
	}
	*d = std::string("submitThreaded=") + (threaded ? "1" : "0") + " guardLine(streamline=1)=" + (line ? "1" : "0") + " requestLine=" +
		(req ? "1" : "0");
	return !threaded && line && req;
}

// render/dlss x A23-B guard (the forward merge of c14f14881): a host that requests an upscaler (the world seam), motion units, frame
// generation or Reflex gets the A23-B worker OFF - their device calls record outside it - and the line naming why; a plain host keeps
// A23-B's default (csguard-off: needsCs, SKIP when the run is GX_NATIVE_CS=0). Under GX_NATIVE_CS=0 the worker is off by the env and
// no guard line is due. X-: the guard skipped leaves the worker ON (xNeedsCs: the X- run SKIPs when the device is direct).
bool csGuardCase(Env& e, std::string* d, bool expectOff)
{
	const bool on = e.nd->csOn();
	const char* env = std::getenv("GX_NATIVE_CS");
	const bool envDirect = env != nullptr && std::strcmp(env, "0") == 0;
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXNATIVE] cs=off forced: render/dlss feature requested") != std::string::npos;
	*d = std::string("csOn=") + (on ? "1" : "0") + " guardLine=" + (line ? "1" : "0") + (envDirect ? " (GX_NATIVE_CS=0)" : "");
	return expectOff ? (!on && (line || envDirect)) : (on && !line);
}
bool caseCsGuardOn(Env& e, std::string* d) { return csGuardCase(e, d, true); }

// E · seam-worker-1 (tier 1, DESIGN_seam-worker-1.md): a SPATIAL upscaler request (FSR1) keeps the A23-B worker ON - the seam's recording
// entry points are S bodies now - and says so in its own line (needsCs: SKIP under GX_NATIVE_CS=0). DLSS stays forced off: csguard-upscaler.
// E · seam-worker-2 (phase 2): the same for the temporal kinds a Mac reaches (csguard-temporal: MetalFX temporal, csguard-fsr3) and motion
// units (csguard-motion) - the line names the request and motion_units.
bool csGuardLiftedCase(Env& e, std::string* d, const char* needle)
{
	const bool on = e.nd->csOn();
	bool forced = false, lifted = false;
	for (const std::string& l : logSnapshot()) {
		forced = forced || l.find("[GXNATIVE] cs=off forced: render/dlss feature requested") != std::string::npos;
		lifted = lifted || l.find(std::string("[GXNATIVE] cs=on with the seam: ") + needle) != std::string::npos;
	}
	*d = std::string("csOn=") + (on ? "1" : "0") + " forcedLine=" + (forced ? "1" : "0") + " seamLine(" + needle + ")=" + (lifted ? "1" : "0");
	return on && !forced && lifted;
}
bool caseCsGuardLifted(Env& e, std::string* d) { return csGuardLiftedCase(e, d, "upscaler=2 motion_units=0 "); }
bool caseCsGuardTemporal(Env& e, std::string* d) { return csGuardLiftedCase(e, d, "upscaler=6 motion_units=0 "); }
bool caseCsGuardFsr3(Env& e, std::string* d) { return csGuardLiftedCase(e, d, "upscaler=3 motion_units=0 "); }
bool caseCsGuardMotion(Env& e, std::string* d) { return csGuardLiftedCase(e, d, "upscaler=0 motion_units=1 "); }

// E · seam-worker-1 (tier 1, DESIGN_seam-worker-1.md §5): the seam's recording entry points as A23-B S bodies, on a device whose request
// (FSR1, a lifted kind) keeps the worker ON at CS1. Each arm: draw A into the entry point's source, the entry point, draw B over the
// source, read the output = A (the entry point is stream-ordered after A and before B), with NO read-back between A and the entry point
// (a read-back is an S that would drain the backlog for it), and csOwnerViolations() unchanged. X- (xNeedsCs): the entry point's SScope
// dropped - its body reaches ensureFrame on the front with the worker running, which the ownership counter counts whatever the schedule.
// Under the plant the worker is drained first (an S accessor), so the planted front recording never races the worker's writer in this
// process; the counter does not depend on that.
IGxWorldPass* seamPass(Env& e)
{
	void* p = nullptr;
	return SUCCEEDED(e.dev->QueryInterface(IID_GxWorldPass, &p)) ? static_cast<IGxWorldPass*>(p) : nullptr;
}
bool seamNear(uint32_t got, uint32_t want, int tol)
{
	for (int sh = 0; sh < 32; sh += 8)
		if (std::abs(int((got >> sh) & 0xff) - int((want >> sh) & 0xff)) > tol)
			return false;
	return true;
}
std::string seamHex(uint32_t v)
{
	char b[16];
	std::snprintf(b, sizeof(b), "%08x", v);
	return b;
}
// the planted run (X-, the worker on): idle the worker before the entry point - see above
void seamQuiesceIfPlanted(Env& e, bool planted)
{
	if (planted && e.nd->csOn())
		(void)e.nd->stats();   // an S accessor: drains
}
IDirect3DSurface8* seamTexture(Env& e, UINT w, UINT h, IDirect3DTexture8** tex)
{
	IDirect3DSurface8* s = nullptr;
	if (SUCCEEDED(e.dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex)) && *tex)
		(*tex)->GetSurfaceLevel(0, &s);
	return s;
}
void seamRelease(std::initializer_list<IUnknown*> objs)
{
	for (IUnknown* o : objs)
		if (o)
			o->Release();
}

// seam-resolve: ResolveLinear (the MSAA world -> the seam texture, linear light)
bool caseSeamResolve(Env& e, std::string* d)
{
	bool ok = true;
	IGxWorldPass* wp = seamPass(e);
	SETUP(wp, "IGxWorldPass");
	IDirect3DSurface8* ms = nullptr;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8* bb = nullptr;
	wp->NextTargetsAreWorld(GXWP_MSAA_COLOUR);   // the next MSAA render target is the world colour: SAMPLED
	const bool made = SUCCEEDED(e.dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &ms)) && ms;
	IDirect3DSurface8* dst = made ? seamTexture(e, 64, 64, &tex) : nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	if (!made || !dst || !bb) {
		seamRelease({ ms, dst, tex, bb, wp });
		SETUP(false, "targets");
	}
	const bool planted = e.nd->debug().seamNoSResolve;
	const uint64_t own0 = e.nd->csOwnerViolations();
	e.dev->BeginScene();
	CHECK(clearSurface(e.dev, ms, 0xffff0000), "clear A");
	seamQuiesceIfPlanted(e, planted);
	CHECK(SUCCEEDED(wp->ResolveLinear(ms, dst)), "ResolveLinear declined");
	CHECK(clearSurface(e.dev, ms, 0xff00ff00), "clear B");
	e.dev->SetRenderTarget(bb, nullptr);
	e.dev->EndScene();
	uint32_t px = 0;
	CHECK(readPixel(e.dev, dst, 32, 32, &px), "read-back");
	CHECK(seamNear(px, 0xffff0000, 2), "resolved " + seamHex(px) + " is not A (ffff0000)");
	if (!planted)   // the planted (X-) run's symptom is judged by the runner: caught only by it, with every check above passing
		CHECK(e.nd->csOwnerViolations() == own0, "worker state touched on the front outside an S body");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	seamRelease({ ms, dst, tex, bb, wp });
	return ok;
}

// seam-fsr1 / seam-rcas: Upscale(FSR1) 32x32 -> 64x64 (EASU into the scratch, RCAS into dst), Upscale(RCAS) 64x64 -> 64x64. A uniform
// colour stays that colour through both passes (within 2 steps).
bool seamUpscaleCase(Env& e, std::string* d, bool rcas)
{
	bool ok = true;
	IGxWorldPass* wp = seamPass(e);
	SETUP(wp, "IGxWorldPass");
	const UINT sw = rcas ? 64 : 32;
	IDirect3DTexture8 *st = nullptr, *ct = nullptr, *dt = nullptr;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* src = seamTexture(e, sw, sw, &st);
	IDirect3DSurface8* scr = seamTexture(e, 64, 64, &ct);
	IDirect3DSurface8* dst = seamTexture(e, 64, 64, &dt);
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	const uint32_t kind = rcas ? uint32_t(GX_UPSCALE_RCAS) : uint32_t(GX_UPSCALE_FSR1);
	if (!src || !scr || !dst || !bb || !wp->Supports(kind)) {
		seamRelease({ src, scr, dst, st, ct, dt, bb, wp });
		SETUP(false, "targets / Supports");
	}
	const bool planted = e.nd->debug().seamNoSUpscale;
	const uint64_t own0 = e.nd->csOwnerViolations();
	const uint32_t A = 0xff3080c0u;
	GxUpscaleCall c = {};
	c.src = src;
	c.srcW = c.srcH = sw;
	c.srcRect = { 0, 0, LONG(sw), LONG(sw) };
	c.dst = dst;
	c.dstRect = { 0, 0, 64, 64 };
	c.scratch = rcas ? nullptr : scr;
	c.kind = kind;
	c.sharpness = 0.2f;
	e.dev->BeginScene();
	CHECK(clearSurface(e.dev, src, A), "clear A");
	seamQuiesceIfPlanted(e, planted);
	CHECK(SUCCEEDED(wp->Upscale(c)), "Upscale declined");
	CHECK(clearSurface(e.dev, src, 0xff00ff00), "clear B");
	e.dev->SetRenderTarget(bb, nullptr);
	e.dev->EndScene();
	uint32_t px = 0;
	CHECK(readPixel(e.dev, dst, 32, 32, &px), "read-back");
	CHECK(seamNear(px, A, 2), "upscaled " + seamHex(px) + " is not A (" + seamHex(A) + ")");
	if (!planted)   // the planted (X-) run's symptom is judged by the runner: caught only by it, with every check above passing
		CHECK(e.nd->csOwnerViolations() == own0, "worker state touched on the front outside an S body");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	seamRelease({ src, scr, dst, st, ct, dt, bb, wp });
	return ok;
}
bool caseSeamFsr1(Env& e, std::string* d) { return seamUpscaleCase(e, d, false); }
bool caseSeamRcas(Env& e, std::string* d) { return seamUpscaleCase(e, d, true); }
// FG P0 FIF rule: GX_NATIVE_FIF=3 with FrameGen requested -> frames in flight 2 + the line; without a request the knob's 3 stands. X-: kept 3.
bool fifGuardCase(Env& e, std::string* d, bool expect2)
{
	const uint32_t fif = e.nd->framesInFlight();
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXNATIVE] frames_in_flight=2 forced: FrameGen/Reflex requested") != std::string::npos;
	*d = "framesInFlight=" + std::to_string(fif) + " guardLine=" + (line ? "1" : "0");
	return expect2 ? (fif == 2 && line) : (fif == 3 && !line);
}
bool caseFifGuardFrameGen(Env& e, std::string* d) { return fifGuardCase(e, d, true); }
bool caseFifGuardOff(Env& e, std::string* d) { return fifGuardCase(e, d, false); }
bool caseCsGuardOff(Env& e, std::string* d) { return csGuardCase(e, d, false); }

// In process: the flush returns only after the helper released the held frame, with that frame's submit and present run, the
// thread joined, and the context refusing a later submit and present without a driver call. X-: the flush skips the quiesce - it
// returns at once with the thread alive and the frame still held.
bool caseThreadExitQuiesce(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	V::SubmitThreadStats s0;
	SETUP(queueHeldFrame(e, &s0), "two warm-up frames, then a frame and its present queued behind the hold");
	std::atomic<bool> sawBlocked { false }, released { false }, stop { false };
	std::thread helper(releaseWhenQuiescing, &ctx, &sawBlocked, &released, &stop, false);
	GxNativeFlushFinalStatsAtExit();
	const bool releasedAtReturn = released.load();
	const bool joinable = ctx.testThreadJoinable();
	const V::SubmitThreadStats s1 = ctx.threadStats();
	stop = true;
	helper.join();
	*d += std::string("flush blocked in quiesce: ") + (sawBlocked ? "yes" : "no") + ", released before it returned: " +
	      (releasedAtReturn ? "yes" : "no") + ", thread joined: " + (joinable ? "no" : "yes") + ", submits +" +
	      std::to_string(s1.submits - s0.submits) + ", driver presents +" + std::to_string(s1.driverPresents - s0.driverPresents);
	CHECK(sawBlocked && releasedAtReturn, "the flush waited in quiesce until the held frame was released");
	CHECK(s1.submits - s0.submits == 1 && s1.driverPresents - s0.driverPresents == (e.window ? 1u : 0u), "the held submit and present ran first");
	CHECK(!joinable, "gx-vk-submit is joined when the flush returns");
	if (joinable) {   // planted: the thread still runs the released frame - no stopped-context probe on it
		ctx.waitIdle();
		return ok;
	}
	const uint64_t v = ctx.submit(V::QueueKind::Graphics, V::SubmitDesc());
	VkResult pr = VK_SUCCESS;
	const uint64_t pseq = ctx.present(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, nullptr, &pr);
	const V::SubmitThreadStats s2 = ctx.threadStats();
	*d += ", later submit " + std::to_string(v) + ", later present " + std::to_string(pseq) + "/" + std::to_string(long(pr));
	CHECK(v == 0 && pseq == 0 && pr == VK_ERROR_DEVICE_LOST, "a later submit answers 0 and a later present is skipped (DEVICE_LOST)");
	CHECK(s2.enqueued == s1.enqueued && s2.submits == s1.submits && s2.driverPresents == s1.driverPresents,
		"neither was queued nor reached the driver");
	return ok;
}

void exitSentinel(const char* what)
{
	std::fprintf(stderr, "[TEST] exit-quiesce: atexit %s\n", what);
}
void exitSentinelBefore() { exitSentinel("registered before the device"); }
void exitSentinelAfter() { exitSentinel("registered after the device's init"); }

// The same, end to end through exit(): RUN ONLY BY NAME, as its own process (it leaks the device, as exit-final). main registered
// exitSentinelBefore before any device; this registers exitSentinelAfter. gates.sh reads its stderr after exit and requires, in
// order: the after-sentinel, the helper's release (the flush blocked in quiesce), "[GXNATIVE] stats final ... submit_thread_ops=<expected> ... end=exit", the
// before-sentinel - the hook's LIFO slot. X-exit-quiesce: the final line's submit_thread_ops falls short (the held ops had not run).
bool caseExitQuiesce(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	V::SubmitThreadStats s0;
	SETUP(queueHeldFrame(e, &s0), "two warm-up frames, then a frame and its present queued behind the hold");
	std::atexit(exitSentinelAfter);
	std::fprintf(stderr, "[TEST] exit-quiesce: expect submit_thread_ops=%llu\n",
		(unsigned long long)(s0.submits + s0.driverPresents + (e.window ? 2 : 1)));
	static std::atomic<bool> sawBlocked { false }, released { false }, stop { false };
	std::thread(releaseWhenQuiescing, &ctx, &sawBlocked, &released, &stop, true).detach();
	e.devReleased = true;   // never released: the process exits with the device alive and a frame held ...
	e.keepWindow = true;    // ... and its window alive, as the game flushes before SDL destroys it
	*d += "device leaked with a frame held; the order is checked in stderr after exit";
	return ok;
}

// ------------------------------------------------------------------------------------------------------------ PREREG_T A25
// amendment 1: the translator invalidates its FF pack only on a GxPackFF input, so a draw after a NON-input change reuses the pack
// and must still read that state live. Each domain arm draws 16x16 quads (one FVF, one stage setup, so one pack) at x 0 / 24 / 48,
// changes ONE non-input state between them and reads the pixels the change must move; every draw after the first must REUSE the
// pack (none may under GX_A25_SLOW_P1=1, where every change repacks). X- = nonInputStale (a reuse reads the last repack's copy of
// rs / tss): caught here under F, inert under GX_A25_SLOW_P1=1.
struct A25V { float x, y, z, rhw; DWORD c; float u, v; };
const DWORD kA25Fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// back buffer + the auto depth-stencil bound and cleared (colour c, depth 1, stencil s); z off, no lighting, CULL_NONE, stage 0 =
// the diffuse colour. The caller releases the returned back buffer.
IDirect3DSurface8* a25Begin(Env& e, D3DCOLOR c, DWORD s)
{
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	const bool ok = bb && ds && SUCCEEDED(e.dev->SetRenderTarget(bb, ds)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, c, 1.f, s));
	if (ds)
		ds->Release();
	if (!ok && bb) {
		bb->Release();
		bb = nullptr;
	}
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(kA25Fvf);
	return bb;
}

// a 16x16 quad at (x0, y0); reversed = the opposite winding; u runs 0 -> u1 across it
bool a25Quad(Env& e, float x0, D3DCOLOR c, bool reversed = false, float u1 = 1.f, float y0 = 8.f, float w = 16.f)
{
	A25V q[4] = { { x0, y0, 0.5f, 1, c, 0, 0 }, { x0 + w, y0, 0.5f, 1, c, u1, 0 }, { x0, y0 + 16, 0.5f, 1, c, 0, 1 },
		{ x0 + w, y0 + 16, 0.5f, 1, c, u1, 1 } };
	if (reversed) {
		std::swap(q[0], q[1]);
		std::swap(q[2], q[3]);
	}
	return SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(A25V)));
}

uint64_t a25Reused(Env& e) { return e.nd->translator().stats().packReused; }

// reads x,y (low 24 bits)
uint32_t a25Px(Env& e, IDirect3DSurface8* bb, UINT x, UINT y = 16)
{
	uint32_t px = 0xdeadbeefu;
	return bb && readPixel(e.dev, bb, x, y, &px) ? (px & 0xffffff) : 0xdeadbeefu;
}

std::string hex(uint32_t v)
{
	char b[16];
	std::snprintf(b, sizeof(b), "%06x", v);
	return b;
}

// An X- arm is a detection only when its failure IS the plant's stale value: A25_CHECK = an assertion the plant
// must NOT break (setup, the first draw, readback, reuse counts, refusals); A25_SYM(good, stale, what) = one it MUST break, whose
// failure counts as the symptom only when the observed value is exactly what the stale state produces - anything else is "other".
#define A25_CHECK(cond, what) do { if (!(cond)) { *d += std::string(" ") + (what); ok = false; e.a25Other = true; } } while (0)
#define A25_SYM(good, stale, what) do { if (!(good)) { *d += std::string(" ") + (what); ok = false; \
	if (stale) e.a25Symptom = true; else e.a25Other = true; } } while (0)

// the draws after the first: all reuse (F) or none (GX_A25_SLOW_P1=1)
#define A25_REUSE(r0, n) A25_CHECK(a25Reused(e) - (r0) == (GXRender::Rec::g_recTest.a25SlowP1 ? 0u : uint64_t(n)), \
	("pack reuses " + std::to_string(a25Reused(e) - (r0)) + ", want " + std::to_string(GXRender::Rec::g_recTest.a25SlowP1 ? 0 : (n))).c_str())

// SRCBLEND ONE -> ZERO (DESTBLEND ZERO, blend on): the second quad must come out black over the 336699 clear
bool caseA25Blend(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = a25Begin(e, 0xff336699, 0);
	if (!bb) {
		e.setupFailed = true;
		return false;
	}
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
	e.dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	A25_CHECK(a25Quad(e, 0, 0xffcc4422), "draw 1");
	const uint64_t r0 = a25Reused(e);
	e.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
	A25_CHECK(a25Quad(e, 24, 0xffcc4422), "draw 2");
	A25_REUSE(r0, 1);
	A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	const uint32_t p1 = a25Px(e, bb, 8), p2 = a25Px(e, bb, 32);
	A25_CHECK(p1 == 0xcc4422, ("SRCBLEND ONE pixel " + hex(p1)).c_str());
	A25_SYM(p2 == 0, p2 == 0xcc4422, ("SRCBLEND ZERO pixel " + hex(p2) + " (want 000000)").c_str());
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// CULL NONE, then CW, then CCW on the same (clockwise) winding: drawn, culled, drawn
bool caseA25Cull(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = a25Begin(e, 0xff000000, 0);
	if (!bb) {
		e.setupFailed = true;
		return false;
	}
	A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	A25_CHECK(a25Quad(e, 0, 0xff22aa66), "draw NONE");
	const uint64_t r0 = a25Reused(e);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
	A25_CHECK(a25Quad(e, 24, 0xff22aa66), "draw CW");
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
	A25_CHECK(a25Quad(e, 48, 0xff22aa66), "draw CCW");
	A25_REUSE(r0, 2);
	A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	const uint32_t p0 = a25Px(e, bb, 8), pcw = a25Px(e, bb, 32), pccw = a25Px(e, bb, 56);
	A25_CHECK(p0 == 0x22aa66, ("CULL NONE pixel " + hex(p0)).c_str());
	// the quad is clockwise on screen: D3D's front face (VK_FRONT_FACE_CLOCKWISE through the negative-height viewport), so CULL_CW
	// removes it and CULL_CCW keeps it (exact outcomes, not just "one of the two")
	A25_SYM(pcw == 0, pcw == 0x22aa66, ("CULL CW pixel " + hex(pcw) + " (want 000000: the clockwise quad is culled)").c_str());
	A25_CHECK(pccw == 0x22aa66, ("CULL CCW pixel " + hex(pccw) + " (want 22aa66)").c_str());
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// stencil cleared to 0x10; EQUAL/0x10 drawn, then STENCILREF 0x20 (EQUAL rejects), then STENCILFUNC GREATER (D3D compares ref OP
// stencil: 0x20 > 0x10 passes - a stale EQUAL with the live reference would reject)
bool caseA25Stencil(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = a25Begin(e, 0xff000000, 0x10);
	if (!bb) {
		e.setupFailed = true;
		return false;
	}
	e.dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
	e.dev->SetRenderState(D3DRS_STENCILREF, 0x10);
	A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	A25_CHECK(a25Quad(e, 0, 0xff8844cc), "draw 1");
	const uint64_t r0 = a25Reused(e);
	e.dev->SetRenderState(D3DRS_STENCILREF, 0x20);
	A25_CHECK(a25Quad(e, 24, 0xff8844cc), "draw 2");
	e.dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_GREATER);
	A25_CHECK(a25Quad(e, 48, 0xff8844cc), "draw 3");
	A25_REUSE(r0, 2);
	A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	const uint32_t p1 = a25Px(e, bb, 8), p2 = a25Px(e, bb, 32), p3 = a25Px(e, bb, 56);
	A25_CHECK(p1 == 0x8844cc, ("EQUAL 0x10 pixel " + hex(p1)).c_str());
	A25_SYM(p2 == 0, p2 == 0x8844cc, ("STENCILREF 0x20 pixel " + hex(p2) + " (want 000000)").c_str());
	A25_CHECK(p3 == 0x8844cc, ("STENCILFUNC GREATER pixel " + hex(p3) + " (want 8844cc: 0x20 > 0x10)").c_str());
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// a 2x1 texture (red, green), u 0 -> 2 across each quad. ADDRESSU CLAMP at u 1.25 = green; WRAP there = red; then MAGFILTER LINEAR
// at u ~0.4 = a red/green blend (POINT would give pure red)
bool caseA25Sampler(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* tex = nullptr;
	D3DLOCKED_RECT lr = {};
	const bool texOk = SUCCEEDED(e.dev->CreateTexture(2, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex &&
		SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0));
	if (texOk) {
		static_cast<uint32_t*>(lr.pBits)[0] = 0xffff0000u;
		static_cast<uint32_t*>(lr.pBits)[1] = 0xff00ff00u;
		tex->UnlockRect(0);
	}
	IDirect3DSurface8* bb = texOk ? a25Begin(e, 0xff000000, 0) : nullptr;
	if (!bb) {
		if (tex)
			tex->Release();
		e.setupFailed = true;
		return false;
	}
	e.dev->SetTexture(0, tex);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	A25_CHECK(a25Quad(e, 0, 0xffffffff, false, 2.f), "draw CLAMP");
	const uint64_t r0 = a25Reused(e);
	e.dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
	A25_CHECK(a25Quad(e, 24, 0xffffffff, false, 2.f), "draw WRAP");
	e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	A25_CHECK(a25Quad(e, 48, 0xffffffff, false, 2.f), "draw LINEAR");
	A25_REUSE(r0, 2);
	A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	const uint32_t pc = a25Px(e, bb, 10), pw = a25Px(e, bb, 34), pl = a25Px(e, bb, 51);
	A25_CHECK(pc == 0x00ff00, ("ADDRESSU CLAMP at u 1.25 = " + hex(pc) + " (want 00ff00)").c_str());
	A25_SYM(pw == 0xff0000, pw == 0x00ff00, ("ADDRESSU WRAP at u 1.25 = " + hex(pw) + " (want ff0000)").c_str());
	const uint32_t r = pl >> 16 & 0xff, g = pl >> 8 & 0xff;
	A25_SYM(pl != 0xdeadbeefu && r >= 0x20 && r <= 0xe0 && g >= 0x20 && g <= 0xe0, pl == 0xff0000,
		("MAGFILTER LINEAR at u ~0.4 = " + hex(pl) + " (want a red/green blend)").c_str());
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	e.dev->SetTexture(0, nullptr);
	bb->Release();
	tex->Release();
	return ok;
}

// (c) TEXTUREFACTOR IS a pack input: the second draw repacks and shows the new factor. X- = packTableShort (60 left out of the
// table): the pack is reused with the old factor (and, under GX_TR_PACK_CHECK=1, check mode counts the mismatch)
bool caseA25Tfactor(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = a25Begin(e, 0xff000000, 0);
	if (!bb) {
		e.setupFailed = true;
		return false;
	}
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff112233);
	A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
	A25_CHECK(a25Quad(e, 0, 0xffffffff), "draw 1");
	const uint64_t packed0 = e.nd->translator().stats().packed;
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff445566);
	A25_CHECK(a25Quad(e, 24, 0xffffffff), "draw 2");
	const uint64_t packed = e.nd->translator().stats().packed - packed0;
	A25_SYM(packed == 1, packed == 0, ("TEXTUREFACTOR change: " + std::to_string(packed) + " repacks, want 1").c_str());
	A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
	const uint32_t p1 = a25Px(e, bb, 8), p2 = a25Px(e, bb, 32);
	A25_CHECK(p1 == 0x112233, ("TFACTOR 112233 pixel " + hex(p1)).c_str());
	A25_SYM(p2 == 0x445566, p2 == 0x112233, ("TFACTOR 445566 pixel " + hex(p2)).c_str());
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

// P3 (one-call stencil): a front-winding quad at x 0 and a back-winding one at x 24, CULL_NONE; each step sets ONE stencil field,
// draws both (grey), then a front-winding probe over both (stencil EQUAL to the value the step must leave, ops KEEP) paints the
// step's colour - both pixels must show it. Every probe's readback ends the frame, so the next step's first stencil group is a
// shadow reset. Step 6 toggles backStencilDefault: unequal faces (two-face path; the back quad's NEVER keeps its stencil); step 7
// goes equal -> unequal -> equal inside one frame; step 8 is equal again after the frame end.
// X- = stencilFrontOnly: the back face keeps an old state.
bool caseA25StencilFaces(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = a25Begin(e, 0xff000000, 0x10);
	if (!bb) {
		e.setupFailed = true;
		return false;
	}
	const GXRender::Rec::GxWriterStats w0 = e.nd->writer().stats();
	e.dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
	auto st = [&](D3DCMPFUNC f, DWORD ref, DWORD fail, DWORD zfail, DWORD pass, DWORD mask, DWORD wmask) {
		e.dev->SetRenderState(D3DRS_STENCILFUNC, f);
		e.dev->SetRenderState(D3DRS_STENCILREF, ref);
		e.dev->SetRenderState(D3DRS_STENCILFAIL, fail);
		e.dev->SetRenderState(D3DRS_STENCILZFAIL, zfail);
		e.dev->SetRenderState(D3DRS_STENCILPASS, pass);
		e.dev->SetRenderState(D3DRS_STENCILMASK, mask);
		e.dev->SetRenderState(D3DRS_STENCILWRITEMASK, wmask);
	};
	const DWORD K = D3DSTENCILOP_KEEP;
	auto step = [&](const char* what, const std::function<void()>& set, DWORD want, D3DCOLOR probe, bool backKeeps = false,
		uint32_t backWant = 0) {
		A25_CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
		// the frame's first stencil group: a no-op state on both faces (ALWAYS, every op KEEP), so the step's own state is an
		// update inside the command buffer - the case X-a25-stencil-faces' plant (front face only) must miss on the back quad
		st(D3DCMP_ALWAYS, 0, K, K, K, 0xff, 0xff);
		A25_CHECK(a25Quad(e, 0, 0xff202020), (std::string(what) + ": seed").c_str());
		set();
		A25_CHECK(a25Quad(e, 0, 0xff202020) && a25Quad(e, 24, 0xff202020, true), (std::string(what) + ": draws").c_str());
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		st(D3DCMP_EQUAL, want, K, K, K, 0xff, 0xff);
		A25_CHECK(a25Quad(e, 0, probe, false, 1.f, 8.f, 40.f), (std::string(what) + ": probe").c_str());
		A25_CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
		const uint32_t pf = a25Px(e, bb, 8), pb = a25Px(e, bb, 32), c = probe & 0xffffff;
		// the plant acts on the back face only: the front pixel must hold; a stale back face leaves the back quad's own grey (its
		// stencil never took the step's value, so the probe skipped it)
		A25_CHECK(pf == c, (std::string(what) + ": front pixel " + hex(pf) + " want " + hex(c)).c_str());
		A25_SYM(pb == (backKeeps ? backWant : c), pb == 0x202020, (std::string(what) + ": back pixel " + hex(pb) + " want " +
			hex(backKeeps ? backWant : c)).c_str());
	};
	step("passOp REPLACE + reference", [&] { st(D3DCMP_ALWAYS, 0x33, K, K, D3DSTENCILOP_REPLACE, 0xff, 0xff); }, 0x33, 0xff0000c0);
	step("failOp INCRSAT", [&] { st(D3DCMP_NEVER, 0x33, D3DSTENCILOP_INCRSAT, K, K, 0xff, 0xff); }, 0x34, 0xff00c000);
	step("depthFailOp DECRSAT", [&] {
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
		st(D3DCMP_ALWAYS, 0, K, D3DSTENCILOP_DECRSAT, K, 0xff, 0xff);
	}, 0x33, 0xffc00000);
	step("compareMask 0x0f", [&] { st(D3DCMP_EQUAL, 0x03, K, K, D3DSTENCILOP_INCRSAT, 0x0f, 0xff); }, 0x34, 0xff00c0c0);
	step("writeMask 0x0f", [&] { st(D3DCMP_ALWAYS, 0xff, K, K, D3DSTENCILOP_REPLACE, 0xff, 0x0f); }, 0x3f, 0xffc000c0);
	const GXRender::Rec::GxWriterStats w1 = e.nd->writer().stats();
	e.nd->translateDebug().backStencilDefault = true;   // unequal faces: the back face NEVER passes, keeps 0x3f, shows c000c0
	step("unequal faces (back default)", [&] { st(D3DCMP_ALWAYS, 0x40, K, K, D3DSTENCILOP_REPLACE, 0xff, 0xff); }, 0x40, 0xffc0c000,
		true, 0xc000c0);
	e.nd->translateDebug().backStencilDefault = false;
	// equal -> unequal -> equal inside ONE command buffer (no frame end between: the shadow compare alone must re-set the back face)
	step("equal -> unequal -> equal in one frame", [&] {
		st(D3DCMP_ALWAYS, 0, K, K, K, 0xff, 0xff);   // equal, no stencil change
		A25_CHECK(a25Quad(e, 0, 0xff202020) && a25Quad(e, 24, 0xff202020, true), "transition: equal draws");
		e.nd->translateDebug().backStencilDefault = true;   // unequal: front 0x40 (already), back NEVER keeps 0x3f
		st(D3DCMP_ALWAYS, 0x40, K, K, D3DSTENCILOP_REPLACE, 0xff, 0xff);
		A25_CHECK(a25Quad(e, 0, 0xff202020) && a25Quad(e, 24, 0xff202020, true), "transition: unequal draws");
		e.nd->translateDebug().backStencilDefault = false;   // equal again: only a re-set back face takes 0x3f -> 0x40
		st(D3DCMP_EQUAL, 0x3f, K, K, D3DSTENCILOP_INCRSAT, 0xff, 0xff);
	}, 0x40, 0xff4080c0);
	const GXRender::Rec::GxWriterStats w2 = e.nd->writer().stats();
	step("equal again", [&] { st(D3DCMP_ALWAYS, 0x50, K, K, D3DSTENCILOP_REPLACE, 0xff, 0xff); }, 0x50, 0xff80c040);
	const GXRender::Rec::GxWriterStats w3 = e.nd->writer().stats();
	const bool slow = GXRender::Rec::g_recTest.a25SlowP3;
	const uint64_t g = w3.stencilGroups - w0.stencilGroups, sh = w3.stencilShared - w0.stencilShared;
	const uint64_t shUnequal = w2.stencilShared - w1.stencilShared, gUnequal = w2.stencilGroups - w1.stencilGroups;
	*d += " [stencil groups " + std::to_string(g) + ", one-call " + std::to_string(sh) + "; unequal steps " + std::to_string(gUnequal) +
		"/" + std::to_string(shUnequal) + "]";
	A25_CHECK(g > 0 && (slow ? sh == 0 : sh > 0 && sh < g), "stencil group paths: one-call groups must run (none under GX_A25_SLOW_P3=1)");
	A25_CHECK(gUnequal > 0 && shUnequal < gUnequal, "the unequal step never took the two-face path");
	A25_CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	bb->Release();
	return ok;
}

// S7 (D · look-shadowmaps 17/10/2026, PREREG_look-shadowmaps A18/A20, PLAN Task 4): the shadow map exists only with the look mask
// (GXShadowMap read once): sm-key-off answers E_NOINTERFACE and creates nothing; sm-key-on has the D16 4096x2048 atlas in its
// sampled layout and says so; sm-queued's BeginShadowFrame / SetShadowTags run in the WORKER's exec half (needsCs) and set the
// caster pass and the translator's tags, with no "cs=off forced" (S7 never turns A23-B off). X-sm-queued runs them on the caller.
bool caseSmKeyOff(Env& e, std::string* d)
{
	void* p = reinterpret_cast<void*>(uintptr_t(1));
	const HRESULT hr = e.dev->QueryInterface(IID_GxShadowMap, &p);
	const bool on = e.nd->testShadowMapOn();
	const bool atlas = e.nd->testAtlas().image != VK_NULL_HANDLE;
	char b[96];
	std::snprintf(b, sizeof(b), "QI=0x%08x ptr=%s on=%d atlas=%d", unsigned(hr), p ? "set" : "null", on ? 1 : 0, atlas ? 1 : 0);
	*d = b;
	return hr == E_NOINTERFACE && p == nullptr && !on && !atlas;
}

bool caseSmKeyOn(Env& e, std::string* d)
{
	IGxShadowMap* sm = nullptr;
	const HRESULT hr = e.dev->QueryInterface(IID_GxShadowMap, reinterpret_cast<void**>(&sm));
	const GXRender::Res::GxImage& a = e.nd->testAtlas();
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXSHADOWMAP] state=on atlas=4096x2048") != std::string::npos;
	char b[160];
	std::snprintf(b, sizeof(b), "QI=0x%08x atlas=%ux%u layout=%d line=%d", unsigned(hr), a.width, a.height, int(a.layout), line ? 1 : 0);
	*d = b;
	if (sm)
		sm->Release();
	return hr == S_OK && sm && a.width == 4096 && a.height == 2048 && a.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL && line;
}

bool caseSmQueued(Env& e, std::string* d)
{
	IGxShadowMap* sm = nullptr;
	if (e.dev->QueryInterface(IID_GxShadowMap, reinterpret_cast<void**>(&sm)) != S_OK || !sm) {
		*d = "no IGxShadowMap";
		e.setupFailed = true;
		return false;
	}
	GxShadowFrame f = {};
	f.step = 0;
	f.cascades = 1;
	f.rect[0][2] = 64;
	f.rect[0][3] = 64;
	for (int i = 0; i < 4; ++i)
		f.lightViewProj[0][i * 5] = f.lightViewProj[1][i * 5] = 1.f;
	sm->BeginShadowFrame(f);
	const bool frameOnWorker = (e.nd->csSync(), e.nd->testShadowLastOnWorker());
	sm->SetShadowTags(GX_SHADOW_TAG_CAST);
	e.nd->csSync();
	const bool tagsOnWorker = e.nd->testShadowLastOnWorker();
	const uint32_t tags = e.nd->testShadowTags();
	const uint32_t cascades = e.nd->testCasters().pass().cascades;
	bool forced = false;
	for (const std::string& l : logSnapshot())
		forced = forced || l.find("cs=off forced") != std::string::npos;
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	e.nd->csSync();
	sm->Release();
	char b[160];
	std::snprintf(b, sizeof(b), "frame_on_worker=%d tags_on_worker=%d tags=%u cascades=%u forced_line=%d", frameOnWorker ? 1 : 0,
		tagsOnWorker ? 1 : 0, tags, cascades, forced ? 1 : 0);
	*d = b;
	return frameOnWorker && tagsOnWorker && tags == GX_SHADOW_TAG_CAST && cascades == 1 && !forced;
}

// S7 P2 (D · look-shadowmaps-2 17/10/2026; PREREG_look-shadowmaps A2/A11/A26/A36, PLAN "P2 implementation" P2-E): the RECEIVER end to
// end on the device. One frame: a CAST-ONLY quad (world x -1..0, z 0.25: captured, never drawn) and a full-screen RECEIVER quad (z 0.5,
// white) under an identity light (light clip = world), shadow colour 0xff4080c0. The INIT replay draws the caster into c0; the receiver
// samples it after fog: its left half = white x shadowColor = (64, 128, 192), its right half white.
namespace smrcv {
struct V { float x, y, z; DWORD c; };
const DWORD kShadow = 0xff4080c0;
// A39/A41 (D · look-shadowmaps-3): persp = a PERSPECTIVE light at L = (2, 0, -1) looking along +z, the caster's edge at x = 0.8: the
// receivers' boundary lands at x = 0.56 (1.2 x 0.8 - 0.4); read WITHOUT the perspective divide it would land at 0.4 (
// an edge at clip x 0 cannot tell the two apart); own = 1: the caster is captured under an own light (mu 0.5, z0 0) and the
// receiver (z 0.2) drawn WITHOUT it, own = 2: the receiver drawn with it too (A41: see caseSmOwnLight)
// A44 (D · look-shadowmaps-4): drops = 1 adds two NON-casters over the right half under CAST_ONLY (no CAST; CAST with z write off),
// drops = 2 also leaves CAST_ONLY set over the receiver (A23's planted leftover: the receiver must be lost)
// A61/A62 (2)/A63 (D · look-shadowmaps-5): solid = 1..5 adds a sorted-style caster over the RIGHT half (x 0..0.8) drawn from a DYNAMIC VB +
// IB with z write OFF: 1 = CAST_ONLY|CAST|SOLID, then the refills after the capture - a NOOVERWRITE append, a DISCARD wrap over the
// captured slots, two more appends - all with OTHER bytes (a quad outside the receiver and DEGENERATE indices: replaying either the
// vertices or the indices shadows nothing); 2 = the other VERTICES written BEFORE the capture (sensitivity: the shadow must leave the
// right half); 3 = CAST_ONLY|SOLID without CAST (g); 4 = z test off (h); 5 = the drawn path, CAST|SOLID without CAST_ONLY (j: SOLID
// ignored, z write off -> not captured); 6 = the right quad with the degenerate INDICES written before the capture (A64: IB sensitivity).
// The worker is drained between the capture and the refills (A64): a deterministic schedule in which only the
// slice's lastUse stamp keeps a DISCARD off the captured slice, at CS0 and CS1 alike.
bool frame(Env& e, IGxShadowMap* sm, uint32_t step, bool copyMid, std::vector<uint8_t>* px, uint32_t* w, uint32_t* h, std::string* d,
	bool c0Away = false, bool realView = false, bool persp = false, int own = 0, int drops = 0, int solid = 0)
{
	// a draw the worker refuses still returns D3D_OK - every error counter is snapshotted by value and compared after
	// the synchronized readback, so a refusal is infrastructure, never a "detection"
	auto errors = [&] {
		const GxNativeStats s = e.nd->stats();
		return s.drawsRefused + s.createsRefused + s.resourceErrors + s.copiesRefused + s.targetsRefused + s.presentErrors +
			s.presentFailures + s.presentsSkipped + e.nd->translator().stats().refused + e.nd->testWriter().stats().refused;
	};
	e.nd->csSync();
	const uint64_t err0 = errors();
	const GXRender::Rec::GxCasterStats cs0 = e.nd->testCasters().stats();
	// the readback must be THIS frame's - a per-frame clear colour in the border the receiver leaves must come back
	// exactly (a refused late submission would hand back the previous capture)
	static uint32_t s_frameNo = 0;
	const DWORD clear = 0xff000000u | ((0x2a1b0cu * ++s_frameNo) & 0x7f7f7fu);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	bool ok = bb && clearSurface(e.dev, bb, clear);
	GxShadowFrame f = {};
	f.step = step;
	f.cascades = step >= 3 ? 0u : (step == 0 ? 2u : 1u);
	f.rect[0][2] = f.rect[0][3] = 2048;
	f.rect[1][0] = 2048;
	f.rect[1][2] = f.rect[1][3] = 2048;
	for (int i = 0; i < 4; ++i)
		f.lightViewProj[0][i * 5] = f.lightViewProj[1][i * 5] = 1.f;
	if (c0Away)   // c0's depth range moved off the receiver (z' = z + 1: the receiver at 1.5) - only c1 covers it
		f.lightViewProj[0][14] = 1.f;
	f.shadowColor = kShadow;
	f.depthRangeWU[0] = f.depthRangeWU[1] = 1.f;
	f.texelWU[0] = f.texelWU[1] = 2.f / 2048.f;
	f.guard = 5;
	f.depthBias[0] = 4.f;
	f.depthBias[2] = 2.f;
	if (persp) {
		// q = p - L, d = q.z; window u = q.x / d in [-2.6, -0.6] (cu = -1.6, S = 2), v = q.y / d in [-1, 1]; near 1, far 2:
		// clip.x = p.x + 1.6 p.z - 0.4, clip.y = p.y, clip.z = 2 p.z (z/w = 2 - 2/d), clip.w = p.z + 1 (D3D row-vector element order)
		for (int c = 0; c < 2; ++c) {
			float* m = f.lightViewProj[c];
			std::fill(m, m + 16, 0.f);
			m[0] = 1.f; m[8] = 1.6f; m[12] = -0.4f;
			m[5] = 1.f;
			m[10] = 2.f;
			m[11] = 1.f; m[15] = 1.f;
			f.nearFar[c][0] = 1.f;
			f.nearFar[c][1] = 2.f;
			f.tanTexel[c] = 2.f / 2048.f;
		}
	}
	sm->BeginShadowFrame(f);
	// realView: VIEW = a rigid transform M (a 30 deg turn about Z + a translation), PROJECTION = M^-1 - the screen is the identity case's,
	// but view space is not world space, so the receiver must really recover the world position from it (InverseView x viewPos)
	D3DMATRIX vm = {}, pm = {};
	{
		const float c = std::cos(0.5235988f), s = std::sin(0.5235988f), tx = 0.3f, ty = -0.2f, tz = 0.1f;
		const float r[3][3] = { { c, s, 0.f }, { -s, c, 0.f }, { 0.f, 0.f, 1.f } };   // rows: v' = v x R
		float* m = &vm._11;
		float* q = &pm._11;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j) {
				m[i * 4 + j] = r[i][j];
				q[i * 4 + j] = r[j][i];   // R^T
			}
		m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1.f;
		const float t[3] = { tx, ty, tz };
		for (int j = 0; j < 3; ++j)   // -t x R^T
			q[12 + j] = -(t[0] * r[j][0] + t[1] * r[j][1] + t[2] * r[j][2]);
		q[15] = 1.f;
	}
	D3DMATRIX ident = {};
	ident._11 = ident._22 = ident._33 = ident._44 = 1.f;
	e.dev->SetTransform(D3DTS_VIEW, realView ? &vm : &ident);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, realView ? &pm : &ident);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	const float ce = persp ? 0.8f : 0.f;   // the caster's right edge
	const V caster[4] = { { -1, -1, 0.25f, 0xffffffff }, { ce, -1, 0.25f, 0xffffffff }, { -1, 1, 0.25f, 0xffffffff }, { ce, 1, 0.25f, 0xffffffff } };
	const V recv[4] = { { -0.8f, -0.8f, 0.5f, 0xffffffff }, { 0.8f, -0.8f, 0.5f, 0xffffffff }, { -0.8f, 0.8f, 0.5f, 0xffffffff },
		{ 0.8f, 0.8f, 0.5f, 0xffffffff } };   // a border of the clear colour stays around it (the freshness check)
	const V recvLow[4] = { { -0.8f, -0.8f, 0.2f, 0xffffffff }, { 0.8f, -0.8f, 0.2f, 0xffffffff }, { -0.8f, 0.8f, 0.2f, 0xffffffff },
		{ 0.8f, 0.8f, 0.2f, 0xffffffff } };   // own frames: in FRONT of the caster (0.25), behind its scaled copy (0.125)
	uint64_t ownBits = 0;
	if (own) {   // mu 0.5, z0 0 (GXShadowMapFit.h ownLight packing: mu's float bits | z0's << 32)
		const float mu = 0.5f, z0 = 0.f;
		uint32_t mb, zb;
		std::memcpy(&mb, &mu, 4);
		std::memcpy(&zb, &z0, 4);
		ownBits = (uint64_t(zb) << 32) | mb;
		sm->SetOwnLight(ownBits);
	}
	// A44: a cast-only draw is captured only as a CASTER - CAST with z test + z write, the drawn path's rule; the receivers keep z off
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST);
	ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, caster, sizeof(V)));
	if (drops) {   // A44 sm-castonly-drop: neither may be captured (the right half would darken) nor drawn
		const V right[4] = { { 0.1f, -1, 0.25f, 0xffffffff }, { 1, -1, 0.25f, 0xffffffff }, { 0.1f, 1, 0.25f, 0xffffffff },
			{ 1, 1, 0.25f, 0xffffffff } };
		sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY);   // (a) not a caster: no CAST
		ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, right, sizeof(V)));
		sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST);   // (b) CAST, but z write off (the drawn path would not capture it)
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, right, sizeof(V)));
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	}
	IDirect3DVertexBuffer8* svb = nullptr;   // A61: held to the end of the frame (the capture replays at submit)
	IDirect3DIndexBuffer8* sib = nullptr;
	if (solid) {
		const V quad[4] = { { 0.f, -0.8f, 0.25f, 0xffffffff }, { 0.8f, -0.8f, 0.25f, 0xffffffff }, { 0.f, 0.8f, 0.25f, 0xffffffff },
			{ 0.8f, 0.8f, 0.25f, 0xffffffff } };
		const V other[4] = { { -3.f, -3.f, 0.25f, 0xffffffff }, { -2.f, -3.f, 0.25f, 0xffffffff }, { -3.f, -2.f, 0.25f, 0xffffffff },
			{ -2.f, -2.f, 0.25f, 0xffffffff } };
		const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
		ok = ok && SUCCEEDED(e.dev->CreateVertexBuffer(16 * sizeof(V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE,
				D3DPOOL_DEFAULT, &svb)) &&
			SUCCEEDED(e.dev->CreateIndexBuffer(24 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT,
				&sib));
		auto fill = [&](UINT slot, const V* q, DWORD flags, bool degenerate) {   // slot k: vertices [4k, 4k + 4), indices [6k, 6k + 6)
			// the INDEX buffer first: a VB DISCARD rename queues a worker command, after which an IB DISCARD must take a
			// fresh slice whatever its stamp - so an IB lock after it could never show an IB-only retention failure at CS1
			if (!svb || !sib)
				return false;
			BYTE* pv = nullptr;
			BYTE* pi = nullptr;
			if (FAILED(sib->Lock(slot * 6 * sizeof(uint16_t), 6 * sizeof(uint16_t), &pi, flags)) || !pi)
				return false;
			uint16_t* ii = reinterpret_cast<uint16_t*>(pi);
			for (int k = 0; k < 6; ++k)
				ii[k] = uint16_t((degenerate ? 0 : idx[k]) + slot * 4);   // degenerate: every triangle collapses to one vertex
			sib->Unlock();
			if (FAILED(svb->Lock(slot * 4 * sizeof(V), 4 * sizeof(V), &pv, flags)) || !pv)
				return false;
			std::memcpy(pv, q, 4 * sizeof(V));
			svb->Unlock();
			return true;
		};
		ok = ok && fill(0, solid == 2 ? other : quad, D3DLOCK_DISCARD, solid == 6);
		ok = ok && SUCCEEDED(e.dev->SetStreamSource(0, svb, sizeof(V))) && SUCCEEDED(e.dev->SetIndices(sib, 0));
		const DWORD tags = solid == 3 ? (GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_SOLID) :
			solid == 5 ? (GX_SHADOW_TAG_CAST | GX_SHADOW_TAG_SOLID) : (GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST | GX_SHADOW_TAG_SOLID);
		sm->SetShadowTags(tags);
		e.dev->SetRenderState(D3DRS_ZENABLE, solid == 4 ? FALSE : TRUE);
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		ok = ok && SUCCEEDED(e.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, 2));
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST);
		e.nd->csSync();   // A64: the worker has executed the capture before any refill (the frame is still unsubmitted)
		// the refills after the capture (integrator A62 (2)): (a) a NOOVERWRITE append, (b) a DISCARD wrap over the captured slots, (c) two
		// more appends - as another group's and the scene's sorting flushes would - other vertices AND degenerate indices
		ok = ok && fill(1, other, D3DLOCK_NOOVERWRITE, true) && fill(0, other, D3DLOCK_DISCARD, true) &&
			fill(1, other, D3DLOCK_NOOVERWRITE, true) && fill(2, other, D3DLOCK_NOOVERWRITE, true);
		e.dev->SetStreamSource(0, nullptr, 0);
		e.dev->SetIndices(nullptr, 0);
	}
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	if (own == 1)
		sm->SetOwnLight(0);
	sm->SetShadowTags(drops == 2 ? GX_SHADOW_TAG_CAST_ONLY : GX_SHADOW_TAG_RECEIVE);
	ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, own ? recvLow : recv, sizeof(V)));
	if (own)
		sm->SetOwnLight(0);
	if (copyMid) {   // a GPU copy (outsideScope) between two receiver draws: the second must rebind set 2 (A36 4a). Image surfaces would
		             // copy on the CPU and never leave the scope - the back buffer into a render-target texture is a GPU copy
		IDirect3DTexture8* rtTex = nullptr;
		IDirect3DSurface8* lvl = nullptr;
		RECT src = { 0, 0, 8, 8 };
		POINT dst = { 0, 0 };
		ok = ok && SUCCEEDED(e.dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rtTex)) &&
			SUCCEEDED(rtTex->GetSurfaceLevel(0, &lvl)) && SUCCEEDED(e.dev->CopyRects(bb, &src, 1, lvl, &dst));
		if (lvl)
			lvl->Release();
		if (rtTex)
			rtTex->Release();
		ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, recv, sizeof(V)));
	}
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	ok = ok && SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	ok = ok && e.nd->testReadPresent(px, w, h) && *w >= 64 && *h >= 64 && px->size() >= size_t(*w) * *h * 4;
	e.nd->csSync();
	if (svb)
		svb->Release();
	if (sib)
		sib->Release();
	const uint64_t err1 = errors();
	if (err1 != err0) {
		*d += " [" + std::to_string(err1 - err0) + " refusal/error(s) in the frame]";
		ok = false;
	}
	if (ok) {
		const uint8_t* p = &(*px)[(size_t(1) * *w + 1) * 4];   // B8G8R8A8
		if (p[2] != ((clear >> 16) & 0xff) || p[1] != ((clear >> 8) & 0xff) || p[0] != (clear & 0xff)) {
			*d += " [stale readback: the border is not this frame's clear colour]";
			ok = false;
		}
	}
	const GXRender::Rec::GxCasterStats cs1 = e.nd->testCasters().stats();
	if (ok && step < 3 && (cs1.passes == cs0.passes || cs1.replayedDraws == cs0.replayedDraws)) {   // the INIT replay ran (r3)
		*d += " [the caster pass did not run in the frame]";
		ok = false;
	}
	if (ok && e.dev->TestCooperativeLevel() != D3D_OK) {
		*d += " [device not OK after the frame]";
		ok = false;
	}
	if (bb)
		bb->Release();
	if (!ok) {   // infrastructure, never a detection (an X- arm must fail on its own symptom only)
		*d += " [frame setup/draw/copy/present/read failed]";
		e.setupFailed = true;
	}
	return ok;
}
// the pixels of columns [x0, x1) x rows [9, h - 9) farther than `tol` from (r, g, b) (B8G8R8A8) - inside the receiver quad (+/-0.8)
size_t off(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, uint32_t x0, uint32_t x1, int r, int g, int b, int tol, std::string* first)
{
	size_t n = 0;
	for (uint32_t y = 9; y + 9 < h; ++y)
		for (uint32_t x = x0; x < x1; ++x) {
			const uint8_t* p = &px[(size_t(y) * w + x) * 4];
			if (std::abs(p[2] - r) > tol || std::abs(p[1] - g) > tol || std::abs(p[0] - b) > tol) {
				if (n++ == 0 && first)
					*first = "(" + std::to_string(x) + "," + std::to_string(y) + ")=" + std::to_string(p[2]) + "," + std::to_string(p[1]) + "," +
						std::to_string(p[0]);
			}
		}
	return n;
}
IGxShadowMap* query(Env& e, std::string* d)
{
	IGxShadowMap* sm = nullptr;
	if (e.dev->QueryInterface(IID_GxShadowMap, reinterpret_cast<void**>(&sm)) != S_OK || !sm) {
		*d = "no IGxShadowMap";
		e.setupFailed = true;
		return nullptr;
	}
	return sm;
}
} // namespace smrcv

bool caseSmReceive(Env& e, std::string* d)
{
	bool ok = true;
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	for (uint32_t step : { 0u, 1u, 2u, 3u }) {
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		if (!smrcv::frame(e, sm, step, false, &px, &w, &h, d)) {
			ok = false;
			break;
		}
		std::string fl, fr;
		const bool off3 = step >= 3;   // step 3: no map, the whole receiver stays white
		const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, off3 ? 255 : 0x40, off3 ? 255 : 0x80, off3 ? 255 : 0xc0, 3, &fl);
		const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr);
		*d += " step" + std::to_string(step) + ": left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" +
			std::to_string(right) + (right ? " first " + fr : "");
		ok = ok && left == 0 && right == 0;
	}
	if (ok) {   // a real view matrix (view space != world space): the receiver must recover the world position
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		std::string fl, fr;
		ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d, false, true);
		const size_t left = ok ? smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl) : 1;
		const size_t right = ok ? smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr) : 1;
		*d += " real-view: left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" + std::to_string(right) +
			(right ? " first " + fr : "");
		ok = ok && left == 0 && right == 0;
	}
	if (ok) {   // step 0, the receiver inside c0's square but outside its depth range: c1 must shadow it
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		std::string fl;
		ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d, true);
		const size_t left = ok ? smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl) : 1;
		*d += " c0-depth-off: left_off=" + std::to_string(left) + (left ? " first " + fl : "");
		ok = ok && left == 0;
	}
	sm->Release();
	return ok;
}

// A44 (D · look-shadowmaps-4; integrator T5 (b)): under CAST_ONLY only a CASTER is captured - CAST with z test + z write, the drawn path's
// rule - and nothing reaches MAIN. sm-receive's step-0 frame plus two non-casters over the RIGHT half ((a) CAST_ONLY without CAST, (b) CAST
// with z write off). Pass: left shadowed, right white; the translator's castOnly +1 and castOnlyDropped +2 (= the 3 draws issued under
// CAST_ONLY), drawn +1 (the receiver), refused +0; the writer's MAIN draws +1. Built-in control (A23's X-): the same frame with CAST_ONLY
// left set over the receiver must LOSE the receiver (MAIN draws +0, drawn +0) - the check is seen to catch the mode leaking into the main
// flush. X-sm-castonly-drop (GxTranslateDebug::smCastOnlyIgnoresCast): both right-half quads are captured -> the right half darkens.
bool caseSmCastOnlyDrop(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	bool ok = true;
	for (int mode = 1; mode <= 2 && ok; ++mode) {
		e.nd->csSync();
		const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
		const uint64_t w0 = e.nd->testWriter().stats().draws;
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d, false, false, false, 0, mode);
		e.nd->csSync();
		const GXRender::Rec::GxTranslateStats t1 = e.nd->translator().stats();
		const uint64_t mainDraws = e.nd->testWriter().stats().draws - w0;
		const uint64_t cast = t1.castOnly - t0.castOnly, dropped = t1.castOnlyDropped - t0.castOnlyDropped, drawn = t1.drawn - t0.drawn,
			refused = t1.refused - t0.refused;
		char b[200];
		std::snprintf(b, sizeof(b), " mode%d: castOnly=%llu dropped=%llu drawn=%llu refused=%llu main_draws=%llu", mode,
			(unsigned long long)cast, (unsigned long long)dropped, (unsigned long long)drawn, (unsigned long long)refused,
			(unsigned long long)mainDraws);
		*d += b;
		if (!ok)
			break;
		if (mode == 1) {
			std::string fl, fr;
			const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl);
			const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr);
			*d += " left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" + std::to_string(right) +
				(right ? " first " + fr : "");
			ok = left == 0 && right == 0 && cast == 1 && dropped == 2 && drawn == 1 && refused == 0 && mainDraws == 1;
		} else {   // the control: the receiver under a leftover CAST_ONLY is dropped - main_draws +0 is what catches the leak
			const bool caught = mainDraws == 0 && drawn == 0 && dropped == 3 && refused == 0;
			*d += caught ? " (control: the leftover mode is caught)" : " [control NOT caught: the receiver reached MAIN under CAST_ONLY]";
			ok = caught;
		}
	}
	sm->Release();
	return ok;
}

// A61/A62 (2)/A63 (D · look-shadowmaps-5): a SOLID cast-only caster - the sorted rotor-disc path - from a DYNAMIC VB + IB with z write off,
// over the RIGHT half (smrcv::frame solid modes). Mode 1: captured with its draw-time bytes although the slots were then refilled (a
// NOOVERWRITE append, a DISCARD wrap, two more appends) with a quad outside the receiver -> the whole receiver is shadowed. Mode 2
// (sensitivity, A62 (2)): the same other bytes written BEFORE the capture -> the right half stays lit, so the check tells the byte sets
// apart; mode 6 does the same for the INDICES (A64). Modes 3 (no CAST), 4 (z test off), 5 (the drawn path) -> right lit. Exact counters per frame: the translator's castOnly /
// castOnlySolid / castOnlyDropped / drawn, refused 0. X-sm-castonly-solid (GxTranslateDebug::smCastOnlyIgnoresSolid): mode 1's caster is
// dropped -> right lit -> caught. X-sm-solid-retain (Res GxDebug::bindNoStamp): the DISCARD wrap may reuse the captured slice -> the
// replay reads the other quad -> right lit -> caught (an X- that is NOT caught means the retention check sees nothing: stop, A63).
// X-sm-solid-retain-ib (Res GxDebug::bindNoStampIndex, A64): only the INDEX slice may be reused -> degenerate indices -> right lit -> caught.
bool caseSmCastOnlySolid(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	struct Want { int mode; bool rightDark; uint64_t castOnly, solid, dropped, drawn; };
	const Want wants[] = { { 1, true, 2, 1, 0, 1 }, { 2, false, 2, 1, 0, 1 }, { 3, false, 1, 0, 1, 1 }, { 4, false, 1, 0, 1, 1 },
		{ 5, false, 1, 0, 0, 2 }, { 6, false, 2, 1, 0, 1 } };
	bool ok = true;
	for (const Want& want : wants) {
		e.nd->csSync();
		const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		// a failing mode ENDS the case (its own X- plants showed a later mode's frame() overwriting an earlier failure)
		ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d, false, false, false, 0, 0, want.mode);
		e.nd->csSync();
		const GXRender::Rec::GxTranslateStats t1 = e.nd->translator().stats();
		const uint64_t cast = t1.castOnly - t0.castOnly, solid = t1.castOnlySolid - t0.castOnlySolid,
			dropped = t1.castOnlyDropped - t0.castOnlyDropped, drawn = t1.drawn - t0.drawn, refused = t1.refused - t0.refused;
		char b[200];
		std::snprintf(b, sizeof(b), " mode%d: castOnly=%llu solid=%llu dropped=%llu drawn=%llu refused=%llu", want.mode,
			(unsigned long long)cast, (unsigned long long)solid, (unsigned long long)dropped, (unsigned long long)drawn,
			(unsigned long long)refused);
		*d += b;
		if (!ok)
			break;
		std::string fl, fr;
		const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl);
		const size_t right = want.rightDark ? smrcv::off(px, w, h, w / 2 + 6, w - 9, 0x40, 0x80, 0xc0, 3, &fr) :
			smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr);
		*d += " left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" + std::to_string(right) +
			(right ? " first " + fr : "");
		ok = left == 0 && right == 0 && cast == want.castOnly && solid == want.solid && dropped == want.dropped && drawn == want.drawn &&
			refused == 0;
		if (!ok)
			break;
	}
	sm->Release();
	return ok;
}

// A39 (D · look-shadowmaps-3): the receivers on a PERSPECTIVE map from a point light. The light at (2, 0, -1) puts the caster's edge
// (x = 0.8, z 0.25) on the receiver plane (z 0.5) at x = 0.56: the shadowed band runs to x 0.5 (column 0.75 w - 2), the lit band from x
// 0.65. Read without the divide (X-sm-persp-divide: the UBO's w row flattened) the edge lands at x = 0.4 - columns 0.7 w .. 0.78 w turn
// lit and the shadowed band fails. X-sm-persp (the look frame never written): nothing shadowed. Steps 0 and 1.
bool caseSmPersp(Env& e, std::string* d)
{
	bool ok = true;
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	for (uint32_t step : { 0u, 1u }) {
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		if (!smrcv::frame(e, sm, step, false, &px, &w, &h, d, false, false, true)) {
			ok = false;
			break;
		}
		// x = 0.56 sits at column 0.78 w (49.9 of 64); the shadowed band [9, 0.75 w) (to x 0.5), the lit band [0.82 w, w - 9) (from x 0.64)
		std::string fl, fr;
		const size_t left = smrcv::off(px, w, h, 9, uint32_t(0.75f * float(w)), 0x40, 0x80, 0xc0, 3, &fl);
		const size_t right = smrcv::off(px, w, h, uint32_t(0.82f * float(w)), w - 9, 255, 255, 255, 3, &fr);
		*d += " persp step" + std::to_string(step) + ": shadow_off=" + std::to_string(left) + (left ? " first " + fl : "") + " lit_off=" +
			std::to_string(right) + (right ? " first " + fr : "");
		ok = ok && left == 0 && right == 0;
	}
	sm->Release();
	return ok;
}

// A41 (D · look-shadowmaps-3): a clamped instance's own light (identity light: depth = world z). The caster (z 0.25) is captured under
// an own light mu 0.5 about z0 0 - its copy stands at 0.125. own-none: a receiver at z 0.2 drawn WITHOUT the own light is behind that
// copy, so its left half is shadowed (unscaled, the caster at 0.25 would be behind it: X-sm-ownlight-cast, the plain World captured,
// leaves it lit). own-self: the same receiver drawn WITH the own light looks up at 0.1, in front of the copy - lit everywhere, as its
// unscaled self is in front of the caster (X-sm-ownlight, the receiver never given bit 3, darkens the left half).
bool caseSmOwnLight(Env& e, std::string* d)
{
	bool ok = true;
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	for (int own : { 1, 2 }) {
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		if (!smrcv::frame(e, sm, 1, false, &px, &w, &h, d, false, false, false, own)) {
			ok = false;
			break;
		}
		std::string fl, fr;
		const bool dark = own == 1;
		const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, dark ? 0x40 : 255, dark ? 0x80 : 255, dark ? 0xc0 : 255, 3, &fl);
		const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr);
		*d += std::string(own == 1 ? " [own-none" : " [own-self") + ": left_off=" + std::to_string(left) + (left ? " first " + fl : "") +
			" right_off=" + std::to_string(right) + (right ? " first " + fr : "") + "]";
		ok = ok && left == 0 && right == 0;
	}
	// A79 / A79a #6 (E · look-shadowmaps-7; integrator 18:1xZ RULING A79a #6): the own-light PULL on the receiver side. A perspective light from
	// L = (2, 0, -1) (frame()'s persp matrices: depth = 2 - 2 / d, d = z + 1, near 1, far 2); one caster (x/y -1..1, z 0.25) captured under an
	// own light mu 0.02 about z0 0.5 - its copy stands at 0.495 (d 1.495) - and a pull eps 0.01 about L (the copy replays at d 1.48005).
	//   left = an OWN receiver (x -0.8..0, z 0.2) drawn with the same own light: it looks up at 0.494 (d 1.494), pulled to 1.47906 - still in
	//   front of its pulled copy: LIT (without the pull on the receiver it would sit behind the pulled copy);
	//   right = an ORDINARY receiver (x 0..0.8, z 0.49, d 1.49): in front of the unpulled copy (lit without the pull), behind the pulled one:
	//   SHADOWED - the pull's effect on foreign receivers.
	// Mutants (same arm, GX_NATIVE_TEST_SM_MUTANT, read here only): noRecvPull = the frame's TEST-ONLY receivers-unpulled flag
	// (ownPullAux[1] = 1): the own receiver darkens -> FAIL; pullOrdinary = the ordinary receiver drawn under an own light mu 0.02 about
	// z0 0.49 = its own z (the scale is the identity there, only the pull acts): it is pulled in front of the copy and lit -> FAIL. Any other
	// value FAILS the case, so a typo cannot pass as clean.
	if (ok) {
		const char* mut = std::getenv("GX_NATIVE_TEST_SM_MUTANT");
		const std::string mutant = mut ? mut : "";
		const bool noRecvPull = mutant == "noRecvPull", pullOrdinary = mutant == "pullOrdinary";
		if (!mutant.empty() && !noRecvPull && !pullOrdinary) {
			*d += " [own-pull: unknown GX_NATIVE_TEST_SM_MUTANT=" + mutant + " - FAIL]";
			sm->Release();
			return false;
		}
		auto errors = [&] {
			const GxNativeStats s = e.nd->stats();
			return s.drawsRefused + s.createsRefused + s.resourceErrors + s.copiesRefused + s.targetsRefused + s.presentErrors +
				s.presentFailures + s.presentsSkipped + e.nd->translator().stats().refused + e.nd->testWriter().stats().refused;
		};
		auto packOwn = [](float mu, float z0) {
			uint32_t mb, zb;
			std::memcpy(&mb, &mu, 4);
			std::memcpy(&zb, &z0, 4);
			return (uint64_t(zb) << 32) | mb;
		};
		e.nd->csSync();
		const uint64_t err0 = errors();
		const GXRender::Rec::GxCasterStats cs0 = e.nd->testCasters().stats();
		const DWORD clear = 0xff102030u;
		IDirect3DSurface8* bb = nullptr;
		e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
		bool fok = bb && clearSurface(e.dev, bb, clear);
		GxShadowFrame f = {};
		f.step = 1;
		f.cascades = 1;
		f.rect[0][2] = f.rect[0][3] = 2048;
		f.shadowColor = smrcv::kShadow;
		f.depthRangeWU[0] = f.depthRangeWU[1] = 1.f;
		f.texelWU[0] = f.texelWU[1] = 2.f / 2048.f;
		f.guard = 5;
		f.depthBias[0] = 4.f;
		f.depthBias[2] = 2.f;
		for (int c = 0; c < 2; ++c) {   // frame()'s persp light: L = (2, 0, -1)
			float* m = f.lightViewProj[c];
			std::fill(m, m + 16, 0.f);
			m[0] = 1.f; m[8] = 1.6f; m[12] = -0.4f;
			m[5] = 1.f;
			m[10] = 2.f;
			m[11] = 1.f; m[15] = 1.f;
			f.nearFar[c][0] = 1.f;
			f.nearFar[c][1] = 2.f;
			f.tanTexel[c] = 2.f / 2048.f;
		}
		f.ownPull[0] = 2.f;
		f.ownPull[1] = 0.f;
		f.ownPull[2] = -1.f;
		f.ownPull[3] = 0.01f;
		f.ownPullAux[0] = 0.03f;   // the game's class bound (GXShadowMapFit.h kOwnPullMuMax, A79b)
		f.ownPullAux[1] = noRecvPull ? 1.f : 0.f;
		sm->BeginShadowFrame(f);
		D3DMATRIX ident = {};
		ident._11 = ident._22 = ident._33 = ident._44 = 1.f;
		e.dev->SetTransform(D3DTS_VIEW, &ident);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		e.dev->SetTransform(D3DTS_PROJECTION, &ident);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
		using smrcv::V;
		const V caster[4] = { { -1, -1, 0.25f, 0xffffffff }, { 1, -1, 0.25f, 0xffffffff }, { -1, 1, 0.25f, 0xffffffff }, { 1, 1, 0.25f, 0xffffffff } };
		const V own[4] = { { -0.8f, -0.8f, 0.2f, 0xffffffff }, { 0.f, -0.8f, 0.2f, 0xffffffff }, { -0.8f, 0.8f, 0.2f, 0xffffffff },
			{ 0.f, 0.8f, 0.2f, 0xffffffff } };
		const V ord[4] = { { 0.f, -0.8f, 0.49f, 0xffffffff }, { 0.8f, -0.8f, 0.49f, 0xffffffff }, { 0.f, 0.8f, 0.49f, 0xffffffff },
			{ 0.8f, 0.8f, 0.49f, 0xffffffff } };
		const uint64_t ownBits = packOwn(0.02f, 0.5f);
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);   // a cast-only draw is captured with CAST + z test + z write (A44)
		sm->SetOwnLight(ownBits);
		sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST);
		fok = fok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, caster, sizeof(V)));
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
		fok = fok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, own, sizeof(V)));   // the own receiver, under its own light
		sm->SetOwnLight(pullOrdinary ? packOwn(0.02f, 0.49f) : 0);
		fok = fok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ord, sizeof(V)));
		sm->SetOwnLight(0);
		sm->SetShadowTags(GX_SHADOW_TAG_NONE);
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		fok = fok && SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
		fok = fok && e.nd->testReadPresent(&px, &w, &h) && w >= 64 && h >= 64 && px.size() >= size_t(w) * h * 4;
		e.nd->csSync();
		if (bb)
			bb->Release();
		const GXRender::Rec::GxCasterStats cs1 = e.nd->testCasters().stats();
		if (errors() != err0 || cs1.passes == cs0.passes || cs1.pulledDraws == cs0.pulledDraws) {   // infrastructure, never a detection
			*d += " [own-pull: frame setup failed - errors " + std::to_string(errors() - err0) + " passes " + std::to_string(cs1.passes - cs0.passes) +
				" pulled " + std::to_string(cs1.pulledDraws - cs0.pulledDraws) + "]";
			e.setupFailed = true;
			fok = false;
		}
		if (fok) {
			std::string fl, fr;
			const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, 255, 255, 255, 3, &fl);
			const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, 0x40, 0x80, 0xc0, 3, &fr);
			*d += " [own-pull" + (mutant.empty() ? std::string() : " MUTANT " + mutant) + ": own receiver lit_off=" + std::to_string(left) +
				(left ? " first " + fl + " - FAIL: the own receiver is darkened (its lookup is not pulled with its copy)" : "") +
				" ordinary receiver shadow_off=" + std::to_string(right) +
				(right ? " first " + fr + " - FAIL: the ordinary receiver is lit (pulled with the copy, or the copy not pulled)" : "") + "]";
			fok = left == 0 && right == 0;
		}
		ok = ok && fok;
	}
	sm->Release();
	return ok;
}

// A36 4a: set 2 is bound at the first draw of a command buffer and AGAIN after outsideScope's state reset (a foreign-layout pass may
// have disturbed it; the HUD draws after such a pass need it). Counted (lookBinds): a frame with one copy between two receiver draws
// binds it twice. X-: the rebind skipped -> once.
bool caseSmRebind(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	bool ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d);   // warm: the plant is handed over by this frame's BeginShadowFrame
	e.nd->csSync();
	const uint64_t b0 = e.nd->testWriter().stats().lookBinds;
	ok = ok && smrcv::frame(e, sm, 0, true, &px, &w, &h, d);
	e.nd->csSync();
	const uint64_t binds = e.nd->testWriter().stats().lookBinds - b0;
	std::string fl;
	const size_t left = ok ? smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl) : 1;
	*d += " set2_binds_in_frame=" + std::to_string(binds) + " left_off=" + std::to_string(left);
	sm->Release();
	return ok && binds == 2 && left == 0;
}

// A11: a step change compiles NOTHING (steps are UBO values: no pipeline, no reallocation). Frames at steps 0 3 0 1 2 0: every compile
// counter equal after the first frame. Positive control (the instrument can see a compile): one draw with a NEW pipeline key (alpha
// blending on) afterwards must move them.
bool caseSmStepCompile(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	bool ok = smrcv::frame(e, sm, 0, false, &px, &w, &h, d);
	e.nd->csSync();
	auto compiles = [&] {
		const GXRender::GxPipeStats s = e.nd->testCacheStats();
		return uint64_t(s.genericMiss) + s.customMiss + s.customBuilt + s.specQueued + s.failed + s.prebuildWait;
	};
	const uint64_t c0 = compiles();
	// E · look-shadowmaps-9 (PREREG 3.3 "0 image creations", integrator 02:1xZ 19/10; tc-r1 #1): the atlas image
	// itself, every device-level creation (GxNativeStats::creates) and every VMA allocation (images and buffers) stay put across the step
	// changes - snapshotted after EVERY step frame. Residual limit (integrator 2056): an image created and destroyed between two snapshots
	// is not seen; shadowMapCreate, S7's only image creation, runs at device init only.
	// X-sm-step-compile (BROKEN INPUT): one render-target texture created inside the step sequence - the creation checks must catch it
	auto allocs = [&] {
		VmaTotalStatistics t = {};
		vmaCalculateStatistics(e.nd->resources().allocator(), &t);
		return uint64_t(t.total.statistics.allocationCount);
	};
	const VkImage img0 = e.nd->testAtlas().image;
	const uint64_t creates0 = e.nd->stats().creates, allocs0 = allocs();
	IDirect3DTexture8* extra = nullptr;
	bool imagesOk = true;
	std::string moved;
	for (uint32_t step : { 3u, 0u, 1u, 2u, 0u }) {
		ok = ok && smrcv::frame(e, sm, step, false, &px, &w, &h, d);
		if (ok && g_brokenInput && !extra && step == 1 &&
			(FAILED(e.dev->CreateTexture(8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &extra)) || !extra)) {
			*d += " [X-: the planted texture could not be created]";   // tc-r1 #2: never a detection
			e.setupFailed = true;
			sm->Release();
			return false;
		}
		e.nd->csSync();
		const VkImage img = e.nd->testAtlas().image;
		const uint64_t cr = e.nd->stats().creates, al = allocs();
		if (img != img0 || cr != creates0 || al != allocs0) {
			if (imagesOk)
				moved = " first moved after the step-" + std::to_string(step) + " frame (creates +" + std::to_string(cr - creates0) +
					", VMA " + std::to_string(allocs0) + " -> " + std::to_string(al) + (img != img0 ? ", atlas RECREATED" : "") + ")";
			imagesOk = false;
		}
	}
	e.nd->csSync();
	const uint64_t c1 = compiles();
	const VkImage img1 = e.nd->testAtlas().image;
	const uint64_t creates1 = e.nd->stats().creates, allocs1 = allocs();
	if (extra)
		extra->Release();
	// a refused control draw (its new pipeline failed) would still move the compile counters - smrcv's refusal / error
	// counters around it: any increase is setup, never a pass nor a detection
	auto errors = [&] {
		const GxNativeStats s = e.nd->stats();
		return s.drawsRefused + s.createsRefused + s.resourceErrors + s.copiesRefused + s.targetsRefused + s.presentErrors +
			s.presentFailures + s.presentsSkipped + e.nd->translator().stats().refused + e.nd->testWriter().stats().refused;
	};
	const uint64_t errC0 = errors();
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);   // the positive control: a new pipeline key
	e.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	e.dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	const smrcv::V q[4] = { { -1, -1, 0.5f, 0x80ffffff }, { 1, -1, 0.5f, 0x80ffffff }, { -1, 1, 0.5f, 0x80ffffff }, { 1, 1, 0.5f, 0x80ffffff } };
	ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(smrcv::V))) &&
		SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.nd->csSync();
	const uint64_t c2 = compiles();
	if (errors() != errC0) {
		*d += " [SETUP: the positive control's draw or present was refused / errored]";
		e.setupFailed = true;
		sm->Release();
		return false;
	}
	*d += " compiles after frame 1=" + std::to_string(c0) + " after steps 3 0 1 2 0=" + std::to_string(c1) + " after the control=" +
		std::to_string(c2) + "; across the steps (6 snapshots): atlas image " + (img1 == img0 ? "same" : "RECREATED") + ", device creates +" +
		std::to_string(creates1 - creates0) + ", VMA allocations " + std::to_string(allocs0) + " -> " + std::to_string(allocs1) + moved;
	sm->Release();
	const bool rest = ok && c1 == c0 && c2 > c1 && img0 != VK_NULL_HANDLE;
	if (g_brokenInput && !rest) {   // tc-r1 #2: under X- only the image probe may fail - anything else is setup, never a detection
		*d += " [X-: a check other than the image probe failed]";
		e.setupFailed = true;
		return false;
	}
	return rest && imagesOk;
}

// ---- E · look-shadowmaps-9 (S7 fold TEST-ONLY commit; integrator rulings DLSS_INTEGRATION 02:1xZ / 02:2xZ / 02:3xZ 19/10) ----
namespace smx {
const DWORD kGrey = 0xff808080;
const smrcv::V kCasterL[4] = { { -1, -1, 0.25f, 0xffffffff }, { 0, -1, 0.25f, 0xffffffff }, { -1, 1, 0.25f, 0xffffffff }, { 0, 1, 0.25f, 0xffffffff } };
void recv(smrcv::V q[4], DWORD c, float z)
{
	q[0] = { -0.8f, -0.8f, z, c };
	q[1] = { 0.8f, -0.8f, z, c };
	q[2] = { -0.8f, 0.8f, z, c };
	q[3] = { 0.8f, 0.8f, z, c };
}
int mulc(int a, int b) { return (a * b + 127) / 255; }
// smrcv::frame's step frame: identity light matrices, two 2048 cascades at step 0
GxShadowFrame stepFrame(uint32_t step)
{
	GxShadowFrame f = {};
	f.step = step;
	f.cascades = step >= 3 ? 0u : (step == 0 ? 2u : 1u);
	f.rect[0][2] = f.rect[0][3] = 2048;
	f.rect[1][0] = 2048;
	f.rect[1][2] = f.rect[1][3] = 2048;
	for (int i = 0; i < 4; ++i)
		f.lightViewProj[0][i * 5] = f.lightViewProj[1][i * 5] = 1.f;
	f.shadowColor = smrcv::kShadow;
	f.depthRangeWU[0] = f.depthRangeWU[1] = 1.f;
	f.texelWU[0] = f.texelWU[1] = 2.f / 2048.f;
	f.guard = 5;
	f.depthBias[0] = 4.f;
	f.depthBias[2] = 2.f;
	return f;
}
// smrcv::frame's draw state: identity transforms, FF diffuse, no blend, z write on
void ffState(Env& e)
{
	D3DMATRIX id = {};
	id._11 = id._22 = id._33 = id._44 = 1.f;
	e.dev->SetTransform(D3DTS_WORLD, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
}
// smrcv::frame's infrastructure counters: a refusal or an error is never a detection
uint64_t errors(Env& e)
{
	const GxNativeStats s = e.nd->stats();
	return s.drawsRefused + s.createsRefused + s.resourceErrors + s.copiesRefused + s.targetsRefused + s.presentErrors + s.presentFailures +
		s.presentsSkipped + e.nd->translator().stats().refused + e.nd->testWriter().stats().refused;
}
bool presentRead(Env& e, std::vector<uint8_t>* px, uint32_t* w, uint32_t* h)
{
	bool ok = SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	ok = ok && e.nd->testReadPresent(px, w, h) && *w >= 64 && *h >= 64 && px->size() >= size_t(*w) * *h * 4;
	e.nd->csSync();
	return ok;
}
// pixels of [x0, x1) x [y0, y1) farther than tol from (r, g, b) (B8G8R8A8)
size_t offRect(const std::vector<uint8_t>& px, uint32_t w, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, int r, int g, int b, int tol,
	std::string* first)
{
	size_t n = 0;
	for (uint32_t y = y0; y < y1; ++y)
		for (uint32_t x = x0; x < x1; ++x) {
			const uint8_t* p = &px[(size_t(y) * w + x) * 4];
			if ((std::abs(p[2] - r) > tol || std::abs(p[1] - g) > tol || std::abs(p[0] - b) > tol) && n++ == 0 && first)
				*first = "(" + std::to_string(x) + "," + std::to_string(y) + ")=" + std::to_string(p[2]) + "," + std::to_string(p[1]) + "," +
					std::to_string(p[0]);
		}
	return n;
}
// the receiver halves of a frame whose caster covers x < 0: left = c x kShadow when shadowed (else c), right = c
bool halves(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, DWORD c, bool leftShadowed, const char* what, std::string* d)
{
	const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
	const int sr = leftShadowed ? mulc(r, 0x40) : r, sg = leftShadowed ? mulc(g, 0x80) : g, sb = leftShadowed ? mulc(b, 0xc0) : b;
	std::string fl, fr;
	const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, sr, sg, sb, 4, &fl);
	const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, r, g, b, 4, &fr);
	*d += std::string(" ") + what + ": left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" +
		std::to_string(right) + (right ? " first " + fr : "");
	return left == 0 && right == 0;
}
} // namespace smx

// A39 T3 + A20/P2 item 0 (sm-vel-*, sm-seam-*): the world draws as a clamped own-light caster, then a white receiver (z 0.5, z test off) on
// the back buffer. The own light (mu 0.5, z0 0) scales the CAPTURE RECORD only: its copy stands at z 0.25, so the receiver darkens inside
// the quad's footprint - and ONLY because of the own light (the unscaled 0.5 would not shadow a receiver at 0.5).
// g_brokenInput (X-sm-vel-*): the scale is applied to the MAIN draw's world as well (WORLD = S) - the leak A39 T3 forbids: the world depth
// moves to 0.25 and the velocity replay at 0.5 fails its LESSEQUAL test, so the vel case's own checks fail.
void velShadowWorld(Env& e, IGxShadowMap* sm, bool begin)
{
	D3DMATRIX m = {};
	m._11 = m._22 = m._33 = m._44 = 1.f;
	if (begin) {
		sm->BeginShadowFrame(smx::stepFrame(0));
		const float mu = 0.5f, z0 = 0.f;
		uint32_t mb, zb;
		std::memcpy(&mb, &mu, 4);
		std::memcpy(&zb, &z0, 4);
		sm->SetOwnLight((uint64_t(zb) << 32) | mb);
		sm->SetShadowTags(GX_SHADOW_TAG_CAST);
		if (g_brokenInput) {
			m._33 = 0.5f;
			e.dev->SetTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		}
		return;
	}
	sm->SetOwnLight(0);
	// the world quad again as a plain depth draw (own light 0, no tag) WITHOUT re-setting WORLD (the stream is still
	// velWorld's quad): a translator that scaled its STORED world in place while capturing would write depth 0.25 here, and the velocity
	// replay at 0.5 fails its LESSEQUAL test - WORLD is restored only after this probe. tc-r2 #1: own light and tags do not invalidate the
	// translator's cached FF pack, so TEXTUREFACTOR (a GxPackFF input this draw does not shade with: COLOROP SELECTARG2 diffuse) changes
	// first - the probe repacks from the STORED world; `packed` +1 across it is the probe's precondition (checked by the case)
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	DWORD tf = 0;
	e.dev->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf ^ 0x00010101u);
	e.nd->csSync();
	const uint64_t packed0 = e.nd->translator().stats().packed;
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.nd->csSync();
	g_velProbeRepacked = e.nd->translator().stats().packed > packed0;
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, tf);
	e.dev->SetTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	smrcv::V r[4];
	smx::recv(r, 0xffffffff, 0.5f);
	sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, r, sizeof(smrcv::V));
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
}

// the footprint check of velShadowWorld's receiver (or a later one of colour c): x in [0.56, 0.69) w (inside the quad, right of
// vel-occluded's occluder) shadowed, x in [0.8, 0.87) w (outside the quad) lit; rows [0.3, 0.7) h
bool velShadowCheck(const std::vector<uint8_t>& px, uint32_t w, uint32_t h, DWORD c, std::string* d)
{
	const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
	std::string fi, fo;
	const uint32_t y0 = h * 3 / 10, y1 = h * 7 / 10;
	const size_t in = smx::offRect(px, w, w * 56 / 100, w * 69 / 100, y0, y1, smx::mulc(r, 0x40), smx::mulc(g, 0x80), smx::mulc(b, 0xc0), 4, &fi);
	const size_t out = smx::offRect(px, w, w * 80 / 100, w * 87 / 100, y0, y1, r, g, b, 4, &fo);
	*d += " footprint_off=" + std::to_string(in) + (in ? " first " + fi : "") + " outside_off=" + std::to_string(out) +
		(out ? " first " + fo : "");
	return in == 0 && out == 0;
}

// A39 T3 (integrator 20:0xZ 17/10) + A20 / P2 item 0 "units + shadows" (16:3xZ 17/10), fold ruling 02:1xZ 19/10: vel-rigid / -skin /
// -occluded with motion units ON AND the shadow map ON in the same frame, the world quad (and occluder) being a clamped own-light caster:
// the vel case's own checks unchanged (the own light reaches the capture record only - never the main draw, its transform or the units'
// worlds), the world captured, and the receiver shadowed by the scaled copy (velShadowCheck). X- (BROKEN INPUT): the scale leaks into the
// main draw's world (velShadowWorld) - the vel checks fail.
bool smVelCase(Env& e, std::string* d, bool (*vel)(Env&, std::string*))
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	e.nd->csSync();
	const uint64_t err0 = smx::errors(e);
	const GXRender::Rec::GxCasterStats c0 = e.nd->testCasters().stats();
	g_velShadow = sm;
	g_velProbeRepacked = false;
	bool ok = vel(e, d);
	g_velShadow = nullptr;
	if (!g_velProbeRepacked) {   // tc-r2 #1: a probe drawn from a cached pack proves nothing - setup, never a detection
		*d += " [PRECONDITION: the WORLD probe did not repack]";
		e.setupFailed = true;
		sm->Release();
		return false;
	}
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	const bool read = smx::presentRead(e, &px, &w, &h);
	const GXRender::Rec::GxCasterStats c1 = e.nd->testCasters().stats();
	if (!read || smx::errors(e) != err0) {
		*d += " [present/read failed or a refusal in the frame]";
		e.setupFailed = true;
		sm->Release();
		return false;
	}
	*d += " captured=" + std::to_string(c1.captured - c0.captured);
	ok = ok && c1.captured > c0.captured;
	ok = velShadowCheck(px, w, h, 0xffffffff, d) && ok;
	sm->Release();
	return ok;
}
bool caseSmVelRigid(Env& e, std::string* d) { return smVelCase(e, d, caseVelRigid); }
bool caseSmVelSkin(Env& e, std::string* d) { return smVelCase(e, d, caseVelSkin); }
bool caseSmVelOccluded(Env& e, std::string* d) { return smVelCase(e, d, caseVelOccluded); }

// A44 T6 (f) (integrator 22:1xZ 17/10): the cast-only flush opens NO MAIN scope, so F-depth+'s tap still sees one world. After a frame
// boundary (a warm frame's Present), the game's order: BeginShadowFrame, the cast-only flush (a caster under CAST_ONLY | CAST), the world
// (a grey receiver): across the flush the writer's scopes and MAIN draws do not move; the world then opens exactly one scope; the caster was
// captured (the translator's castOnly +1) and shadows the receiver's left half. CS0 and CS1 (sm0 / sm1).
// X-sm-castonly-noscope (BROKEN INPUT): the flush's caster tagged CAST without CAST_ONLY (drawn) - it reaches MAIN and opens the scope.
bool caseSmCastOnlyNoScope(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	if (!smrcv::frame(e, sm, 0, false, &px, &w, &h, d)) {   // the warm frame (its own failures mark the setup)
		sm->Release();
		return false;
	}
	const uint64_t err0 = smx::errors(e);
	sm->BeginShadowFrame(smx::stepFrame(0));
	smx::ffState(e);
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.nd->csSync();
	const GXRender::Rec::GxWriterStats w0 = e.nd->testWriter().stats();
	const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
	sm->SetShadowTags(g_brokenInput ? GX_SHADOW_TAG_CAST : (GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST));
	bool ok = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, smx::kCasterL, sizeof(smrcv::V)));
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	e.nd->csSync();
	const GXRender::Rec::GxWriterStats w1 = e.nd->testWriter().stats();
	const GXRender::Rec::GxTranslateStats t1 = e.nd->translator().stats();
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	smrcv::V r[4];
	smx::recv(r, smx::kGrey, 0.5f);
	sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
	ok = ok && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, r, sizeof(smrcv::V)));
	sm->SetShadowTags(GX_SHADOW_TAG_NONE);
	e.nd->csSync();
	const GXRender::Rec::GxWriterStats w2 = e.nd->testWriter().stats();
	ok = ok && smx::presentRead(e, &px, &w, &h);
	if (!ok || smx::errors(e) != err0) {
		*d += " [frame draw/present/read failed or a refusal in the frame]";
		e.setupFailed = true;
		sm->Release();
		return false;
	}
	const uint64_t flushScopes = w1.scopes - w0.scopes, flushMain = w1.draws - w0.draws, castOnly = t1.castOnly - t0.castOnly,
		worldScopes = w2.scopes - w1.scopes;
	char b[200];
	std::snprintf(b, sizeof(b), " flush: scopes +%llu main_draws +%llu castOnly +%llu; world: scopes +%llu;", (unsigned long long)flushScopes,
		(unsigned long long)flushMain, (unsigned long long)castOnly, (unsigned long long)worldScopes);
	*d += b;
	const bool pixels = smx::halves(px, w, h, smx::kGrey, true, "receiver", d);
	sm->Release();
	return pixels && flushScopes == 0 && flushMain == 0 && castOnly == 1 && worldScopes == 1;
}

// (integrator 02:3xZ 19/10; PREREG 3.3 "caster captured only under cast=1"): an ORDINARY mesh (the drawn path, no
// CAST_ONLY: z test + z write) is captured iff its draw carries CAST - the translator's tag guard decides, not the test. Two frames, the same
// mesh over the left half: tag NONE -> captured +0 and the grey receiver unshadowed; tag CAST -> captured +1 and its left half shadowed.
// X-sm-cast-ordinary (BROKEN INPUT): the NONE frame's mesh tagged CAST - its "not captured, not shadowed" checks fail.
bool caseSmCastOrdinary(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	bool ok = true;
	for (int pass = 0; pass < 2 && ok; ++pass) {
		const bool want = pass == 1, tagged = want || g_brokenInput;
		e.nd->csSync();
		const uint64_t err0 = smx::errors(e);
		const GXRender::Rec::GxCasterStats c0 = e.nd->testCasters().stats();
		sm->BeginShadowFrame(smx::stepFrame(0));
		smx::ffState(e);
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		sm->SetShadowTags(tagged ? GX_SHADOW_TAG_CAST : GX_SHADOW_TAG_NONE);
		bool drawn = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, smx::kCasterL, sizeof(smrcv::V)));
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		smrcv::V r[4];
		smx::recv(r, smx::kGrey, 0.5f);
		sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
		drawn = drawn && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, r, sizeof(smrcv::V)));
		sm->SetShadowTags(GX_SHADOW_TAG_NONE);
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		drawn = drawn && smx::presentRead(e, &px, &w, &h);
		const GXRender::Rec::GxCasterStats c1 = e.nd->testCasters().stats();
		if (!drawn || smx::errors(e) != err0) {
			*d += " [frame draw/present/read failed or a refusal in the frame]";
			e.setupFailed = true;
			sm->Release();
			return false;
		}
		const uint64_t captured = c1.captured - c0.captured;
		*d += std::string(" tag ") + (want ? "CAST" : "NONE") + ": captured +" + std::to_string(captured);
		ok = smx::halves(px, w, h, smx::kGrey, want, want ? "shadowed" : "unshadowed", d) && captured == (want ? 1u : 0u);
	}
	sm->Release();
	return ok;
}

// (integrator 02:3xZ 19/10; PREREG 3.3 "the apply rule per blend mode"): the PIXEL outcome of a modulate pass over a
// shadowed receiver. A white receiver (left half shadowed: kShadow), then a modulate pass of grey M over it, RECEIVE-tagged as the game's
// receive window tags it: left = kShadow x M, right = M - the shadow applied ONCE (by the receiver), never again by the modulate pass. Both
// forms: DESTCOLOR / ZERO and ZERO / SRCCOLOR.
// X-sm-modulate (GxTranslateDebug::smBitsIgnoreBlend, X-sm-bits' plant): applyRgb on the modulate pass -> the left half darkened twice.
// A shader that ignored ApplyRgb on the modulate pass shows the same symptom.
bool caseSmModulate(Env& e, std::string* d)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	bool ok = true;
	for (int form = 0; form < 2 && ok; ++form) {
		e.nd->csSync();
		const uint64_t err0 = smx::errors(e);
		sm->BeginShadowFrame(smx::stepFrame(0));
		smx::ffState(e);
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		sm->SetShadowTags(GX_SHADOW_TAG_CAST_ONLY | GX_SHADOW_TAG_CAST);
		bool drawn = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, smx::kCasterL, sizeof(smrcv::V)));
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		smrcv::V base[4], mod[4];
		smx::recv(base, 0xffffffff, 0.5f);
		smx::recv(mod, smx::kGrey, 0.5f);
		sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
		drawn = drawn && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, base, sizeof(smrcv::V)));
		e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_SRCBLEND, form == 0 ? D3DBLEND_DESTCOLOR : D3DBLEND_ZERO);
		e.dev->SetRenderState(D3DRS_DESTBLEND, form == 0 ? D3DBLEND_ZERO : D3DBLEND_SRCCOLOR);
		drawn = drawn && SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, mod, sizeof(smrcv::V)));
		e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		e.dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
		sm->SetShadowTags(GX_SHADOW_TAG_NONE);
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		drawn = drawn && smx::presentRead(e, &px, &w, &h);
		if (!drawn || smx::errors(e) != err0) {
			*d += " [frame draw/present/read failed or a refusal in the frame]";
			e.setupFailed = true;
			sm->Release();
			return false;
		}
		ok = smx::halves(px, w, h, smx::kGrey, true, form == 0 ? "DESTCOLOR/ZERO" : "ZERO/SRCCOLOR", d);
	}
	sm->Release();
	return ok;
}

// P2 (4) (integrator 16:3xZ 17/10; fold ruling 02:1xZ 19/10): validation (core + sync: the runner fails a clean arm on any error or hazard)
// on ONE frame with the key ON, motion units ON (a velocity draw: the bridge's programs), the seam ON (seam-fsr1's FSR1 = EASU + RCAS, or
// seam-rcas's RCAS) and draws AFTER them, at CS0 and CS1 - a direct arm. The frame: the vel world with velShadowWorld (a clamped own-light
// caster + receiver 1), a velocity draw into O, seamUpscaleCase's sequence (clear A, Upscale, clear B), then receiver 2 (grey) on the back
// buffer AFTER the foreign-layout binds: set 2 must be rebound, so receiver 2 still shows the shadow. Checks: the velocity value, the
// upscaled colour, receiver 2's footprint, no refusal. X- = seamNoSUpscale (seam-fsr1's ownership plant, judged by the runner).
bool smSeamCase(Env& e, std::string* d, bool rcas)
{
	bool ok = true;
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	IGxWorldPass* wp = seamPass(e);
	if (!wp)
		sm->Release();
	SETUP(wp, "IGxWorldPass");
	const UINT sw = rcas ? 64 : 32;
	IDirect3DTexture8 *st = nullptr, *ct = nullptr, *dt = nullptr;
	IDirect3DSurface8* src = seamTexture(e, sw, sw, &st);
	IDirect3DSurface8* scr = seamTexture(e, 64, 64, &ct);
	IDirect3DSurface8* dst = seamTexture(e, 64, 64, &dt);
	const uint32_t kind = rcas ? uint32_t(GX_UPSCALE_RCAS) : uint32_t(GX_UPSCALE_FSR1);
	if (!src || !scr || !dst || !wp->Supports(kind)) {
		seamRelease({ src, scr, dst, st, ct, dt, wp, sm });
		SETUP(false, "targets / Supports");
	}
	const bool planted = e.nd->debug().seamNoSUpscale;
	e.nd->csSync();
	const uint64_t own0 = e.nd->csOwnerViolations(), err0 = smx::errors(e);
	const uint32_t A = 0xff3080c0u;
	GxUpscaleCall c = {};
	c.src = src;
	c.srcW = c.srcH = sw;
	c.srcRect = { 0, 0, LONG(sw), LONG(sw) };
	c.dst = dst;
	c.dstRect = { 0, 0, 64, 64 };
	c.scratch = rcas ? nullptr : scr;
	c.kind = kind;
	c.sharpness = 0.2f;
	{
		VelScene v;
		g_velShadow = sm;
		g_velProbeRepacked = false;
		const bool world = velWorld(e, v, false, d) && g_velProbeRepacked;   // BeginScene; the clamped caster + receiver 1
		g_velShadow = nullptr;
		if (!world) {
			seamRelease({ src, scr, dst, st, ct, dt, wp, sm });
			SETUP(false, "the vel world");
		}
		velBegin(e, v, GXRender::gxVelRigidVs, GXRender::gxVelRigidDecl);
		velConstants(e, -0.125f, 0.125f);
		CHECK(SUCCEEDED(e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2)), "velocity draw");
		e.dev->SetVertexShader(D3DFVF_XYZ);
		e.dev->SetPixelShader(0);
		CHECK(clearSurface(e.dev, src, A), "clear A");
		seamQuiesceIfPlanted(e, planted);
		CHECK(SUCCEEDED(wp->Upscale(c)), "Upscale declined");
		CHECK(clearSurface(e.dev, src, 0xff00ff00), "clear B");
		// the draws AFTER the motion and seam binds: receiver 2 on the back buffer, the same frame's map
		e.dev->SetRenderTarget(v.bb, v.ds);
		smx::ffState(e);
		e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		smrcv::V r[4];
		smx::recv(r, smx::kGrey, 0.5f);
		sm->SetShadowTags(GX_SHADOW_TAG_RECEIVE);
		CHECK(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, r, sizeof(smrcv::V))), "receiver 2 after the seam");
		sm->SetShadowTags(GX_SHADOW_TAG_NONE);
		CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
		float vel[4] = {};
		CHECK(readVel(e.dev, v.o, 32, 32, vel) && velNear(vel, -4.f, -4.f, 1.f), "the velocity draw: O(32,32) is not (-4, -4, a 1)");
	}
	uint32_t up = 0;
	CHECK(readPixel(e.dev, dst, 32, 32, &up), "read-back");
	CHECK(seamNear(up, A, 2), "upscaled " + seamHex(up) + " is not A (" + seamHex(A) + ")");
	if (!planted)   // as seamUpscaleCase: the planted run's symptom is judged by the runner
		CHECK(e.nd->csOwnerViolations() == own0, "worker state touched on the front outside an S body");
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	const bool read = smx::presentRead(e, &px, &w, &h);
	if (!read || smx::errors(e) != err0) {
		*d += " [present/read failed or a refusal in the frame]";
		e.setupFailed = true;
		ok = false;
	} else
		ok = velShadowCheck(px, w, h, smx::kGrey, d) && ok;
	seamRelease({ src, scr, dst, st, ct, dt, wp, sm });
	return ok;
}
bool caseSmSeamFsr1(Env& e, std::string* d) { return smSeamCase(e, d, false); }

// S7 A92 (the integrator's P4 ruling (5)): replay-k at k = 4 (GX_SHADOWMAP_DEBUG_REPLAY=4, set for these arms before the device exists):
// copies 2..4 render into the TEST-ONLY scratch image, never sampled - the receivers still read only the atlas, so sm-receive's step-0
// picture holds exactly (left shadowed, right lit); the caster pass replays 4 x (2 cascades x 1 record) draws in 4 x 2 rendering instances.
// -nodraw (GX_SHADOWMAP_DEBUG_REPLAY_NODRAW=1, the instrument check's fail arm): the copies keep their 6 instances, lose their draws (2).
// Validation (core + sync) at CS0 and CS1: the runner fails a clean arm on any error or hazard. X- (BROKEN INPUT): judged against k = 1's
// counts (2 draws, 2 instances) - the count checks fail.
bool smReplayCase(Env& e, std::string* d, bool noDraw)
{
	IGxShadowMap* sm = smrcv::query(e, d);
	if (!sm)
		return false;
	e.nd->csSync();
	const bool scratch = e.nd->testSmScratch().image != VK_NULL_HANDLE;
	const GXRender::Rec::GxCasterStats c0 = e.nd->testCasters().stats();
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	if (!scratch || !smrcv::frame(e, sm, 0, false, &px, &w, &h, d)) {
		*d += scratch ? "" : " [SETUP: no replay-k scratch image (GX_SHADOWMAP_DEBUG_REPLAY not seen at creation)]";
		e.setupFailed = true;
		sm->Release();
		return false;
	}
	const GXRender::Rec::GxCasterStats c1 = e.nd->testCasters().stats();
	const uint64_t draws = c1.replayedDraws - c0.replayedDraws, passes = c1.passes - c0.passes;
	const uint64_t wantDraws = g_brokenInput ? 2u : (noDraw ? 2u : 8u), wantPasses = g_brokenInput ? 2u : 8u;
	std::string fl, fr;
	const size_t left = smrcv::off(px, w, h, 9, w / 2 - 6, 0x40, 0x80, 0xc0, 3, &fl);
	const size_t right = smrcv::off(px, w, h, w / 2 + 6, w - 9, 255, 255, 255, 3, &fr);
	*d += " replayed draws +" + std::to_string(draws) + " (want " + std::to_string(wantDraws) + ") instances +" + std::to_string(passes) +
		" (want " + std::to_string(wantPasses) + "); left_off=" + std::to_string(left) + (left ? " first " + fl : "") + " right_off=" +
		std::to_string(right) + (right ? " first " + fr : "");
	sm->Release();
	if (g_brokenInput && (left || right)) {   // under X- only the count symptom may fail - a wrong picture is setup
		*d += " [X-: the picture failed too: not a detection]";
		e.setupFailed = true;
		return false;
	}
	return draws == wantDraws && passes == wantPasses && left == 0 && right == 0;
}
bool caseSmReplay4(Env& e, std::string* d) { return smReplayCase(e, d, false); }
bool caseSmReplay4NoDraw(Env& e, std::string* d) { return smReplayCase(e, d, true); }
bool caseSmSeamRcas(Env& e, std::string* d) { return smSeamCase(e, d, true); }

// A36 item 5: the atlas cannot be made (planted at creation) -> a key-off session exactly: the effective mask 0, no set 2 in the writer,
// IID_GxShadowMap unanswered, and the state=off line says why. X-: the bit kept after the failure.
bool caseSmCreateFail(Env& e, std::string* d)
{
	void* p = reinterpret_cast<void*>(uintptr_t(1));
	const HRESULT hr = e.dev->QueryInterface(IID_GxShadowMap, &p);
	// D · look-shadowmaps-3: the planted run GETS the interface - release it, or the device outlives the arm and is torn down from an atexit
	// path after the validation layer's statics (OBSERVED 17/10: rc 134 "VkDevice dispatch handle was not found" whenever X-sm-create-fail
	// ran after another arm in one process, on the rebased base a96905826 and on fad01250a alike)
	const bool gotIt = hr == S_OK && p != nullptr && p != reinterpret_cast<void*>(uintptr_t(1));
	if (gotIt)
		static_cast<IGxShadowMap*>(p)->Release();
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXSHADOWMAP] state=off reason=\"(test) atlas creation failed\"") != std::string::npos;
	const uint32_t mask = e.nd->testLookFeatures();
	const bool set2 = e.nd->testWriter().lookOn();
	char b[160];
	std::snprintf(b, sizeof(b), "QI=0x%08x mask=%u set2=%d line=%d", unsigned(hr), mask, set2 ? 1 : 0, line ? 1 : 0);
	*d = b;
	return hr == E_NOINTERFACE && p == nullptr && mask == 0 && !set2 && line;
}

// A36 item 1: the generic set prewarmed with the key-on family (prebuildGenericSet) - every FF VS x PS pipeline compiled at load under
// the harness' core + sync validation (a VUID-RuntimeSpirv-OpEntryPoint-08743 interface error there fails the arm), none failed.
bool caseSmPrewarm(Env& e, std::string* d)
{
	const GXRender::GxPipeStats s = e.nd->testCacheStats();
	char b[160];
	std::snprintf(b, sizeof(b), "mask=%u prebuild queued=%u built=%u failed=%u prebuildFailed=%u", e.nd->testLookFeatures(), s.prebuildQueued,
		s.prebuilt, s.failed, s.prebuildFailed);
	*d = b;
	return e.nd->testLookFeatures() == 1u && s.prebuildQueued > 0 && s.prebuilt == s.prebuildQueued && s.failed == 0 && s.prebuildFailed == 0;
}

// B · a2c-1 (A108, SCOPE_a2c.md): alpha-to-coverage through the real device. A full-screen quad textured with a horizontal alpha ramp
// (texel x: alpha 4x, white; POINT, so the test GE 0x80 flips between columns 31 and 32 and fwidth(alpha * 255) = 4 per pixel) at z 0.5
// with z writes, then a red full-screen quad at z 0.7 behind it. Where the ramp quad covered a sample its depth hides the red; where it
// discarded, the red shows - the picture reads colour AND depth. TODAY (key off; key on at 1x): every pixel is red or white, the edge
// at columns 31 | 32. NEW (key on at 4x): red through column 30, white from column 34, the resolved mix only at the edge columns (31-33),
// in every row. X- (a2cSpecBitOff): A2C in the pipeline without the ramp - raw alpha decides coverage, the white side mixes far past
// the edge. everything but that pixel rule is SETUP (draws, Present, the readback, the [GXA2C] line, the session state,
// validation) - a broken run is never counted as the plant detected. so are the draw counts (translator: 2 drawn, 0
// refused, 1 A2C draw iff the session has A2C; writer: 0 refused) and the picture's STRUCTURE, which holds with or without the ramp
// (red through column 30 in every row - the hard test and the ramp both drop alpha < 0x7c - and something white-ish from column 40):
// an all-red picture from a refused draw is SETUP, never "detected". The X- plant shows ONLY as mixing off the edge.
bool a2cRamp(Env& e, std::string* d, bool wantA2c, const char* wantLine)
{
	bool ok = true;
	const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
	const uint64_t w0 = e.nd->testWriter().stats().refused;
	IDirect3DSurface8* img = nullptr;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8* lvl = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(64, 4, D3DFMT_A8R8G8B8, &img)) && img &&
		SUCCEEDED(e.dev->CreateTexture(64, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex &&
		SUCCEEDED(tex->GetSurfaceLevel(0, &lvl)) && lvl, "the ramp texture");
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(img->LockRect(&lr, nullptr, 0)), "lock the ramp");
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 64; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = (uint32_t(4 * x) << 24) | 0x00ffffffu;
	img->UnlockRect();
	SETUP(SUCCEEDED(e.dev->CopyRects(img, nullptr, 0, lvl, nullptr)), "upload the ramp");
	lvl->Release();
	img->Release();
	SETUP(SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "clear colour + depth");
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ALPHAREF, 0x80);
	e.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
	e.dev->SetTexture(0, tex);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	e.dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	struct VT { float x, y, z, rhw, u, v; };
	const VT ramp[4] = { { -0.5f, -0.5f, 0.5f, 1, 0, 0 }, { 63.5f, -0.5f, 0.5f, 1, 1, 0 }, { -0.5f, 63.5f, 0.5f, 1, 0, 1 },
		{ 63.5f, 63.5f, 0.5f, 1, 1, 1 } };
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	SETUP(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ramp, sizeof(VT))), "draw the ramp");
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	e.dev->SetTexture(0, nullptr);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	struct VC { float x, y, z, rhw; DWORD c; };
	const VC red[4] = { { -0.5f, -0.5f, 0.7f, 1, 0xffff0000 }, { 63.5f, -0.5f, 0.7f, 1, 0xffff0000 }, { -0.5f, 63.5f, 0.7f, 1, 0xffff0000 },
		{ 63.5f, 63.5f, 0.7f, 1, 0xffff0000 } };
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	SETUP(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, red, sizeof(VC))), "draw the red quad behind");
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	tex->Release();
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	SETUP(e.nd->testReadPresent(&px, &w, &h) && w == 64 && h == 64 && px.size() >= size_t(w) * h * 4, "read the presented image");
	const GXRender::Rec::GxTranslateStats& t1 = e.nd->translator().stats();
	SETUP(t1.drawn - t0.drawn == 2 && t1.refused == t0.refused && e.nd->testWriter().stats().refused == w0,
		("draws: translator drawn " + std::to_string(t1.drawn - t0.drawn) + " refused " + std::to_string(t1.refused - t0.refused) +
		" (want 2 / 0), writer refused " + std::to_string(e.nd->testWriter().stats().refused - w0) + " (want 0)").c_str());
	SETUP(t1.a2cOn - t0.a2cOn == (wantA2c ? 1u : 0u), ("A2C draws " + std::to_string(t1.a2cOn - t0.a2cOn) + " (want " +
		(wantA2c ? "1" : "0") + ")").c_str());
	// per pixel: G (red 0, white 255) of B8G8R8A8; R must be 255 everywhere (both quads are full red)
	int badR = 0, mixedFar = 0, rowsNoMix = 0, hardWrong = 0, mixedAny = 0, structure = 0;
	for (uint32_t y = 2; y < 62; ++y)   // the SETUP structure (see above): both arms of the X- pair have it
		for (uint32_t x = 0; x < 64; ++x) {
			const uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
			structure += (x <= 30 && p[1] > 2) || (x >= 40 && p[1] <= 2) || p[2] < 250;
		}
	SETUP(structure == 0, ("picture structure (red through column 30, lit from column 40, R 255): " + std::to_string(structure) +
		" pixels off").c_str());
	for (uint32_t y = 2; y < 62; ++y) {
		bool rowMix = false;
		for (uint32_t x = 0; x < 64; ++x) {
			const uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
			const int g = p[1], r = p[2];
			badR += r < 250;
			const bool mixed = g > 2 && g < 253;
			mixedAny += mixed;
			if (wantA2c) {
				const bool edge = x >= 31 && x <= 33;
				rowMix = rowMix || (mixed && edge);
				mixedFar += mixed && !edge;
				hardWrong += (x <= 30 && g > 2) || (x >= 34 && g < 253);
			} else
				hardWrong += mixed || (x <= 31 && g > 2) || (x >= 32 && g < 253);
		}
		rowsNoMix += wantA2c && !rowMix;
	}
	char b[200];
	std::snprintf(b, sizeof(b), "a2c=%d R<250=%d mixed=%d mixed_off_edge=%d rows_without_edge_mix=%d wrong=%d row32 G=%u,%u,%u,%u,%u",
		int(e.nd->testA2COn()), badR, mixedAny, mixedFar, rowsNoMix, hardWrong, px[(32 * w + 30) * 4 + 1], px[(32 * w + 31) * 4 + 1],
		px[(32 * w + 32) * 4 + 1], px[(32 * w + 33) * 4 + 1], px[(32 * w + 34) * 4 + 1]);
	*d += b;
	bool line = wantLine == nullptr;
	bool anyLine = false;
	for (const std::string& l : logSnapshot()) {
		line = line || (wantLine && l.find(wantLine) != std::string::npos);
		anyLine = anyLine || l.find("[GXA2C] state=") != std::string::npos;
	}
	SETUP(wantLine ? line : !anyLine, wantLine ? (std::string("no '") + wantLine + "' line").c_str() : "a [GXA2C] state line with the key off");
	SETUP(e.nd->testA2COn() == wantA2c, "the session's A2C state");
	const GxNativeValidation v = e.nd->validation();
	SETUP(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	if (wantA2c)   // the plant's own symptom gets its own token; the runner counts X-a2c-ramp ONLY with it
		CHECK(mixedFar == 0, "[mixing off the edge: " + std::to_string(mixedFar) + " px]");
	CHECK(badR == 0 && hardWrong == 0 && mixedFar == 0 && rowsNoMix == 0, wantA2c ? "[the A2C edge]" : "[today's hard edge]");   // the ONLY rule
	return ok;
}
bool caseA2cRamp(Env& e, std::string* d) { return a2cRamp(e, d, true, "[GXA2C] state=on samples=4"); }

// B · a2c-1 (A108): a Reset to another MSAA. Key on (created at 4x, generic set prewarmed): the Reset to 2x prewarms
// the 2x configurations and their A2C twins before it returns (a "[GXA2C] reset prewarm samples=2" line, twins added, none failed); a Reset
// that keeps 4x prewarms nothing more. X- (a2cResetNoPrewarm): no prewarm at 2x - no twin added. Key off (a2c-reset-off): the Reset queues
// nothing new - today's Reset.
bool a2cReset(Env& e, std::string* d, bool keyOn)
{
	bool ok = true;
	const GXRender::GxPipeStats s0 = e.nd->testCacheStats();
	SETUP(e.nd->testA2COn() == keyOn && s0.prebuildQueued > 0, "the session (A2C state, generic set prewarmed)");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1, D3DMULTISAMPLE_4_SAMPLES);
	SETUP(SUCCEEDED(e.dev->Reset(&pp)), "Reset at the same 4x");
	const GXRender::GxPipeStats s1 = e.nd->testCacheStats();
	pp = presentParams(true, 1, D3DMULTISAMPLE_2_SAMPLES);
	SETUP(SUCCEEDED(e.dev->Reset(&pp)), "Reset to 2x");
	const GXRender::GxPipeStats s2 = e.nd->testCacheStats();
	bool line = false;
	for (const std::string& l : logSnapshot())
		line = line || l.find("[GXA2C] reset prewarm samples=2 ") != std::string::npos;
	char b[220];
	std::snprintf(b, sizeof(b), "a2c=%d queued %u -> %u (same 4x) -> %u (2x), twins %u -> %u -> %u, prebuild failed %u, line=%d, prebuild_runs=%u",
		int(keyOn), s0.prebuildQueued, s1.prebuildQueued, s2.prebuildQueued, s0.a2cTwins, s1.a2cTwins, s2.a2cTwins, s2.prebuildFailed, int(line),
		s2.prebuildRuns);
	*d += b;
	CHECK(s1.prebuildQueued == s0.prebuildQueued && s1.a2cTwins == s0.a2cTwins, " [a Reset that keeps 4x prewarmed again]");
	if (keyOn)
		CHECK(line && s2.a2cTwins > s1.a2cTwins && s2.prebuildQueued > s1.prebuildQueued && s2.prebuildFailed == 0,
			" [the 2x configurations and their A2C twins were not prewarmed]");
	else   // B · a2c-2 (the integrator's condition a): key off = ONE prebuild per session, both Resets included
		CHECK(!line && s2.prebuildQueued == s1.prebuildQueued && s2.a2cTwins == 0 && s2.prebuildRuns == 1,
			" [a key-off Reset queued pipelines, or the session ran prebuild() " + std::to_string(s2.prebuildRuns) + " times (want 1)]");
	return ok;
}
bool caseA2cReset(Env& e, std::string* d) { return a2cReset(e, d, true); }

// B · a2c-1: the keys a session already DREW survive an MSAA Reset. A draw state outside the census (pre-transformed
// diffuse + specular, no texture; MEASURED outside: its 4x draws miss) is drawn at 4x with the alpha test off, then on: key on = 2 misses (the plain key and its A2C key - SETUP:
// else the state is in the census and the arm proves nothing), key off = 1 (one shared key). Reset to 2x, the same two draws: key on = 0 new
// misses (the Reset retargeted both keys to 2x); key off = 1 (today: the 4x key does not serve 2x). X- (a2cResetNoRetarget): key on misses
// both again.
bool a2cResetDraw(Env& e, std::string* d, bool keyOn, D3DMULTISAMPLE_TYPE to = D3DMULTISAMPLE_2_SAMPLES)
{
	bool ok = true;
	struct V { float x, y, z, rhw; DWORD c, s; };
	const V q[4] = { { -0.5f, -0.5f, 0.5f, 1, 0x80ffffff, 0 }, { 63.5f, -0.5f, 0.5f, 1, 0x80ffffff, 0 }, { -0.5f, 63.5f, 0.5f, 1, 0x80ffffff, 0 },
		{ 63.5f, 63.5f, 0.5f, 1, 0x80ffffff, 0 } };
	// the cache counters are read on the front thread: the translator() accessor syncs the A23-B worker first (else a read races the draws)
	auto misses = [&]() { (void)e.nd->translator(); return e.nd->testCacheStats().genericMiss; };
	auto two = [&]() {
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
		e.dev->SetTexture(0, nullptr);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		e.dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_ALPHAREF, 0x60);
		e.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		const bool a = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
		e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		const bool b = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
		e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		return a && b && SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	};
	SETUP(e.nd->testA2COn() == keyOn, "the session's A2C state");
	const uint32_t m0 = misses();
	SETUP(two(), "the two draws at 4x");
	const uint32_t m1 = misses();
	SETUP(m1 - m0 == (keyOn ? 2u : 1u), ("misses at creation " + std::to_string(m1 - m0) + " (want " + (keyOn ? "2" : "1") +
		": the state must be outside the census)").c_str());
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1, to);
	SETUP(SUCCEEDED(e.dev->Reset(&pp)), "Reset to the new MSAA");
	const uint32_t m2 = misses();
	SETUP(two(), "the two draws at the new MSAA");
	const uint32_t m3 = misses();
	const GxNativeValidation v = e.nd->validation();
	SETUP(v.errors == 0 && v.hazards == 0, ("validation " + std::to_string(v.errors) + "/" + std::to_string(v.hazards) + " " + v.first).c_str());
	char b[200];
	std::snprintf(b, sizeof(b), "a2c=%d misses at creation %u, during the Reset %u, after it (%ux) %u, prebuild_runs=%u", int(keyOn), m1 - m0,
		m2 - m1, unsigned(to), m3 - m2, e.nd->testCacheStats().prebuildRuns);
	*d += b;
	CHECK(m3 - m2 == (keyOn ? 0u : 1u), keyOn ? " [the drawn keys were not retargeted: misses after the Reset]" :
		" [key off: not today's one miss after the Reset]");
	if (!keyOn) {   // B · a2c-2 (the integrator's condition a): key off = ONE prebuild per session, the Reset included
		const uint32_t runs = e.nd->testCacheStats().prebuildRuns;
		CHECK(runs == 1, " [key off: prebuild() ran " + std::to_string(runs) + " times (want 1)]");
	}
	return ok;
}
bool caseA2cResetDraw(Env& e, std::string* d) { return a2cResetDraw(e, d, true); }
bool caseA2cResetDrawOff(Env& e, std::string* d) { return a2cResetDraw(e, d, false); }
// an INCREASING MSAA (created 2x, Reset to 4x) - the default mask normalised to 0x3 at 2x must reach 4x as 0xf
bool caseA2cResetDrawUp(Env& e, std::string* d) { return a2cResetDraw(e, d, true, D3DMULTISAMPLE_4_SAMPLES); }
bool caseA2cResetOff(Env& e, std::string* d) { return a2cReset(e, d, false); }
bool caseA2cRampOff(Env& e, std::string* d) { return a2cRamp(e, d, false, nullptr); }
bool caseA2cRamp1x(Env& e, std::string* d) { return a2cRamp(e, d, false, "[GXA2C] state=off samples=1"); }

// B · a2c-2 (SCOPE_a2c.md "keep a discard only for alpha that is fully transparent"): a SHARP alpha step inside one
// 2x2 derivative quad - texel x <= 32 alpha 0, x >= 33 alpha 255, white, POINT, one texel per pixel - so fwidth(alpha * 255) = 255 at
// columns 32 | 33. At GE 0x60 the bare ramp gives the alpha-0 texel 12.5 % coverage, at GE 0x08 47 %. Drawn at z 0.5 with z writes, a red
// quad behind at z 0.7: column 32 must stay PURE red in every row (no sample took the transparent texel's colour or depth) and column 33
// white, at both refs. Today's hard test reads the same (a2c-sharp-edge-off, the control). SETUP: the draws, A2C draws on key on, the
// readback, the picture's structure (red through column 31, white from column 34).
bool a2cSharpEdge(Env& e, std::string* d, bool wantA2c)
{
	bool ok = true;
	IDirect3DSurface8* img = nullptr;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8* lvl = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(64, 4, D3DFMT_A8R8G8B8, &img)) && img &&
		SUCCEEDED(e.dev->CreateTexture(64, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex &&
		SUCCEEDED(tex->GetSurfaceLevel(0, &lvl)) && lvl, "the step texture");
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(img->LockRect(&lr, nullptr, 0)), "lock the step");
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 64; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = (x <= 32 ? 0u : 0xff000000u) | 0x00ffffffu;
	img->UnlockRect();
	SETUP(SUCCEEDED(e.dev->CopyRects(img, nullptr, 0, lvl, nullptr)), "upload the step");
	lvl->Release();
	img->Release();
	for (const DWORD ref : { DWORD(0x60), DWORD(0x08) }) {
		const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
		SETUP(SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "clear colour + depth");
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		e.dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		e.dev->SetRenderState(D3DRS_ALPHAREF, ref);
		e.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		e.dev->SetTexture(0, tex);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		e.dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
		e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		e.dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		struct VT { float x, y, z, rhw, u, v; };
		const VT step[4] = { { -0.5f, -0.5f, 0.5f, 1, 0, 0 }, { 63.5f, -0.5f, 0.5f, 1, 1, 0 }, { -0.5f, 63.5f, 0.5f, 1, 0, 1 },
			{ 63.5f, 63.5f, 0.5f, 1, 1, 1 } };
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
		SETUP(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, step, sizeof(VT))), "draw the step");
		e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		e.dev->SetTexture(0, nullptr);
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		struct VC { float x, y, z, rhw; DWORD c; };
		const VC red[4] = { { -0.5f, -0.5f, 0.7f, 1, 0xffff0000 }, { 63.5f, -0.5f, 0.7f, 1, 0xffff0000 }, { -0.5f, 63.5f, 0.7f, 1, 0xffff0000 },
			{ 63.5f, 63.5f, 0.7f, 1, 0xffff0000 } };
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		SETUP(SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, red, sizeof(VC))), "draw the red quad behind");
		SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
		std::vector<uint8_t> px;
		uint32_t w = 0, h = 0;
		SETUP(e.nd->testReadPresent(&px, &w, &h) && w == 64 && h == 64 && px.size() >= size_t(w) * h * 4, "read the presented image");
		const GXRender::Rec::GxTranslateStats& t1 = e.nd->translator().stats();
		SETUP(t1.drawn - t0.drawn == 2 && t1.refused == t0.refused, "draws: 2 drawn, 0 refused");
		SETUP(t1.a2cOn - t0.a2cOn == (wantA2c ? 1u : 0u), ("A2C draws " + std::to_string(t1.a2cOn - t0.a2cOn)).c_str());
		int structure = 0, col32 = 0, col33 = 0, maxG32 = 0;
		for (uint32_t y = 2; y < 62; ++y) {
			for (uint32_t x = 0; x < 64; ++x) {
				const uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
				structure += (x <= 31 && p[1] > 2) || (x >= 34 && p[1] < 250) || p[2] < 250;
			}
			const uint8_t g32 = px[(size_t(y) * w + 32) * 4 + 1], g33 = px[(size_t(y) * w + 33) * 4 + 1];
			col32 += g32 > 2;
			col33 += g33 < 250;
			maxG32 = std::max<int>(maxG32, g32);
		}
		SETUP(structure == 0, ("picture structure (red through column 31, white from column 34): " + std::to_string(structure) + " px off").c_str());
		char b[160];
		std::snprintf(b, sizeof(b), "GE 0x%02x: column 32 (alpha 0) non-red rows %d (max G %d), column 33 (alpha 255) non-white rows %d; ",
			unsigned(ref), col32, maxG32, col33);
		*d += b;
		char hx[8];
		std::snprintf(hx, sizeof(hx), "0x%02x", unsigned(ref));
		CHECK(col32 == 0, "[a fully transparent texel covered samples at GE " + std::string(hx) + "]");
		CHECK(col33 == 0, "[an opaque texel lost coverage at GE " + std::string(hx) + "]");
	}
	tex->Release();
	return ok;
}
bool caseA2cSharpEdge(Env& e, std::string* d) { return a2cSharpEdge(e, d, true); }
bool caseA2cSharpEdgeOff(Env& e, std::string* d) { return a2cSharpEdge(e, d, false); }

// B · a2c-2 (DLSS_INTEGRATION.md:3927 a, a stated limit): an A2C key that fails on FIRST use (outside the prewarm) - its draw is refused,
// and the periodic [GXA2C] line counts it as firstuse_fail. Key on at 4x: an alpha-tested draw state outside the census (pre-transformed
// diffuse + specular, no texture - a2c-reset-draw's state) is drawn ONCE with failA2cKeys (the driver refuses A2C keys), then the device
// presents until the next periodic line. a2c-firstuse-fail: that draw refused, the cache counts 1, the line reads firstuse_fail=1 (and every
// earlier/later window 0). a2c-firstuse: the same without the plant - drawn, 0 everywhere (the control).
bool a2cFirstUse(Env& e, std::string* d, bool plant)
{
	bool ok = true;
	struct V { float x, y, z, rhw; DWORD c, s; };
	const V q[4] = { { -0.5f, -0.5f, 0.5f, 1, 0x80ffffff, 0 }, { 63.5f, -0.5f, 0.5f, 1, 0x80ffffff, 0 }, { -0.5f, 63.5f, 0.5f, 1, 0x80ffffff, 0 },
		{ 63.5f, 63.5f, 0.5f, 1, 0x80ffffff, 0 } };
	SETUP(e.nd->testA2COn(), "the key-on 4x session");
	const size_t log0 = logSnapshot().size();
	const GXRender::Rec::GxTranslateStats t0 = e.nd->translator().stats();
	const uint32_t f0 = e.nd->testCacheStats().a2cFirstUseFailed;
	const unsigned h0 = GXRender::g_gxPipeTestMutants.failA2cHits.load();
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
	e.dev->SetTexture(0, nullptr);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHAREF, 0x60);
	e.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
	GXRender::g_gxPipeTestMutants.failA2cKeys = plant;
	const bool drew = SUCCEEDED(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
	(void)e.nd->translator();   // the A23-B worker has run the draw (and its miss) before the plant is cleared
	GXRender::g_gxPipeTestMutants.failA2cKeys = false;
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	SETUP(drew, "the draw call");
	bool presented = true;
	for (int i = 0; i < 2 * int(GxNativeDevice::kStatsEveryPresents) && presented; ++i)
		presented = SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr));
	SETUP(presented, "the Presents");
	(void)e.nd->translator();
	const GXRender::Rec::GxTranslateStats& t1 = e.nd->translator().stats();
	const uint32_t f1 = e.nd->testCacheStats().a2cFirstUseFailed;
	const unsigned hits = GXRender::g_gxPipeTestMutants.failA2cHits.load() - h0;
	uint32_t lines = 0, fuSum = 0, fuMissing = 0;
	const std::vector<std::string> lg = logSnapshot();
	for (size_t i = log0; i < lg.size(); ++i)
		if (lg[i].rfind("[GXA2C] presents=", 0) == 0) {
			++lines;
			const size_t at = lg[i].find(" firstuse_fail=");
			if (at == std::string::npos)
				++fuMissing;
			else
				fuSum += uint32_t(std::strtoul(lg[i].c_str() + at + std::strlen(" firstuse_fail="), nullptr, 10));
		}
	char b[220];
	std::snprintf(b, sizeof(b), "plant=%d plant_hits=%u a2c_draws=%llu refused=%llu cache_firstuse=%u [GXA2C] lines=%u (without the field %u) "
		"firstuse_fail sum=%u", int(plant), hits, (unsigned long long)(t1.a2cOn - t0.a2cOn), (unsigned long long)(t1.refused - t0.refused),
		f1 - f0, lines, fuMissing, fuSum);
	*d += b;
	SETUP(t1.a2cOn - t0.a2cOn == 1, "the draw is an A2C draw");
	SETUP(lines >= 1 && fuMissing == 0, "the periodic [GXA2C] lines, each with the firstuse_fail field");
	if (plant) {
		SETUP(hits >= 1, "the failA2cKeys plant was reached");
		CHECK(t1.refused - t0.refused == 1 && f1 - f0 == 1 && fuSum == 1, " [the first-use A2C failure is not counted exactly once]");
	} else
		CHECK(t1.refused == t0.refused && f1 == f0 && fuSum == 0, " [a first-use failure counted without one]");
	return ok;
}
bool caseA2cFirstUseFail(Env& e, std::string* d) { return a2cFirstUse(e, d, true); }
bool caseA2cFirstUse(Env& e, std::string* d) { return a2cFirstUse(e, d, false); }

// B · a2c-1 (A108; the integrator's ruling (5)): with the key on at 4x the generic set is prewarmed with its A2C twins (prebuildGenericSet):
// every twin compiles at load under the harness' core + sync validation, none fails, and the count is the eligible parts x the session's
// multisampled configs (> 0). The key-off count is pinned by gx_pipecache_test --arm a2c-off and the tap digests (== the parent).
bool caseA2cPrewarm(Env& e, std::string* d)
{
	const GXRender::GxPipeStats s = e.nd->testCacheStats();   // (the cache's own "[GXPIPE] prebuild ... a2c_twins=" line goes to stderr)
	char b[200];
	std::snprintf(b, sizeof(b), "a2c=%d prebuild queued=%u built=%u twins=%u failed=%u prebuildFailed=%u", int(e.nd->testA2COn()),
		s.prebuildQueued, s.prebuilt, s.a2cTwins, s.failed, s.prebuildFailed);
	*d = b;
	return e.nd->testA2COn() && s.a2cTwins > 0 && s.prebuildQueued > s.a2cTwins && s.prebuilt == s.prebuildQueued && s.failed == 0 &&
		s.prebuildFailed == 0;
}

// B · a2c-2 (the integrator's ruling after DLSS_INTEGRATION.md:3876: A2C only ever turns ITSELF off). One alpha-tested, unblended quad
// (in the A2C rule's "on" class) + a Present: true when every call succeeded. The caller compares the translator's counters around it.
bool a2cOneQuad(IDirect3DDevice8* dev)
{
	struct V { float x, y, z, rhw; DWORD c; };
	const V q[4] = { { -0.5f, -0.5f, 0.5f, 1, 0x80ffffff }, { 63.5f, -0.5f, 0.5f, 1, 0x80ffffff }, { -0.5f, 63.5f, 0.5f, 1, 0x80ffffff },
		{ 63.5f, 63.5f, 0.5f, 1, 0x80ffffff } };
	dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	dev->SetTexture(0, nullptr);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHAREF, 0x60);
	dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
	const bool a = SUCCEEDED(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.f, 0)) && SUCCEEDED(dev->BeginScene()) &&
		SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V))) && SUCCEEDED(dev->EndScene());
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	return a && SUCCEEDED(dev->Present(nullptr, nullptr, nullptr, nullptr));
}

// a2c-prewarm-fail (init): a key-on 4x device whose A2C keys cannot be created (g_gxPipeTestMutants.failA2cKeys during creation). The
// device is still created, A2C is off for the session with its "[GXA2C] state=off reason=prewarm-failed at=init" line, the failed keys are
// dropped, today's prebuild ran a second time and completed, and an alpha-tested draw is drawn on today's path (no A2C, nothing refused,
// validation clean). X- (a2cNoFallback -> host.testA2cNoFallback, the fallback removed): the creation fails on A2C's account.
bool caseA2cPrewarmFail(Env& e, std::string* d)
{
	bool ok = true;
	const bool noFallback = e.nd->debug().a2cNoFallback;
	GxNativeHost host;
	host.log = captureLog;
	host.validation = e.nd->ctx().validationEnabled();
	host.prebuildGenericSet = true;
	host.framesInFlight = g_fif;
	host.lookFeatures = 2u;   // GXAlphaToCoverage = yes
	host.testA2cNoFallback = noFallback;
	e.dev->Release();
	e.devReleased = true;
	StubD3D8 parent;
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1, D3DMULTISAMPLE_4_SAMPLES);
	IDirect3DDevice8* dev = nullptr;
	std::string why;
	const size_t log0 = logSnapshot().size();
	const unsigned hits0 = GXRender::g_gxPipeTestMutants.failA2cHits.load();
	GXRender::g_gxPipeTestMutants.failA2cKeys = true;
	const HRESULT hr = GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_MIXED_VERTEXPROCESSING, pp, host, &dev, &why);
	GXRender::g_gxPipeTestMutants.failA2cKeys = false;
	// the plant must have been REACHED (an A2C key refused by it during this creation), and a creation failure counts
	// as the planted symptom only when it is the generic-set closure's - anything else is a setup failure (no detection, never a pass)
	const unsigned hits = GXRender::g_gxPipeTestMutants.failA2cHits.load() - hits0;
	const bool planted = hits > 0;
	*d += "plant_hits=" + std::to_string(hits) + " ";
	if (dev && !planted)
		dev->Release(), dev = nullptr;
	SETUP(planted, ("the failA2cKeys plant was never reached (creation " + std::string(SUCCEEDED(hr) ? "ok" : why) + ")").c_str());
	SETUP(SUCCEEDED(hr) || why.find("the generic FF prebuild did not complete") != std::string::npos,
		("the creation failed for another reason: " + why).c_str());
	CHECK(SUCCEEDED(hr) && dev, "[A2C failed init: " + why + "]");
	if (!dev)
		return ok;
	GxNativeDevice* nd = GxNativeFromDevice(dev);
	const std::vector<std::string> lg = logSnapshot();
	bool offLine = false, onLine = false;
	size_t dropped = 0;
	for (size_t i = log0; i < lg.size(); ++i) {
		const size_t at = lg[i].find("[GXA2C] state=off reason=prewarm-failed at=init dropped=");
		if (at != std::string::npos) {
			offLine = true;
			dropped = size_t(std::strtoul(lg[i].c_str() + at + std::strlen("[GXA2C] state=off reason=prewarm-failed at=init dropped="), nullptr, 10));
		}
		onLine = onLine || lg[i].find("[GXA2C] state=on") != std::string::npos;
	}
	const GXRender::GxPipeStats st = nd->testCacheStats();
	const uint64_t on0 = nd->translator().stats().a2cOn, ref0 = nd->translator().stats().refused;
	const bool drew = a2cOneQuad(dev);
	const uint64_t on1 = nd->translator().stats().a2cOn, ref1 = nd->translator().stats().refused;
	const GxNativeValidation v = nd->validation();
	char b[260];
	std::snprintf(b, sizeof(b), "a2c=%d off_count=%u off_line=%d on_line=%d dropped=%zu prebuild_runs=%u failed=%u drew=%d a2c_draws=%llu refused=%llu "
		"validation %u/%u", int(nd->testA2COn()), nd->testA2COffCount(), int(offLine), int(onLine), dropped, st.prebuildRuns, st.failed, int(drew),
		(unsigned long long)(on1 - on0), (unsigned long long)(ref1 - ref0), v.errors, v.hazards);
	*d += b;
	CHECK(!nd->testA2COn() && nd->testA2COffCount() == 1 && offLine && !onLine, " [A2C did not turn itself off at init]");
	CHECK(dropped > 0 && st.failed > 0 && st.prebuildRuns == 2, " [the failed A2C keys were not dropped / today's prebuild did not run again]");
	CHECK(drew && on1 == on0 && ref1 == ref0, " [the alpha-tested draw was not drawn on today's path]");
	CHECK(v.errors == 0 && v.hazards == 0, " [validation: " + v.first + "]");
	dev->Release();
	return ok;
}

// a2c-prewarm-fail-reset: key on at 4x (generic set + twins prewarmed), then a Reset to 2x whose A2C keys cannot be created. The Reset
// succeeds, A2C is off for the session ("[GXA2C] state=off reason=prewarm-failed at=reset"), an alpha-tested draw at 2x is drawn on today's
// path (no A2C, nothing refused), and a later Reset back to 4x does no A2C work (no prewarm line, no prebuild). X- (a2cNoFallback): A2C
// stays on after the failed prewarm.
bool caseA2cPrewarmFailReset(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->testA2COn() && e.nd->testCacheStats().prebuildRuns == 1, "the key-on 4x session (generic set + A2C twins prewarmed)");
	const size_t log0 = logSnapshot().size();
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1, D3DMULTISAMPLE_2_SAMPLES);
	const unsigned hits0 = GXRender::g_gxPipeTestMutants.failA2cHits.load();
	GXRender::g_gxPipeTestMutants.failA2cKeys = true;
	const HRESULT hr = e.dev->Reset(&pp);
	GXRender::g_gxPipeTestMutants.failA2cKeys = false;
	const unsigned hits = GXRender::g_gxPipeTestMutants.failA2cHits.load() - hits0;
	*d += "plant_hits=" + std::to_string(hits) + " ";
	// the Reset itself succeeded and its prewarm reached the plant (an incomplete "reset prewarm samples=2" line and a
	// planted refusal after it) - else the arm never got to its gate (a setup failure, no detection)
	bool offLine = false, prewarmFailed = false;
	const bool planted = hits > 0;
	{
		const std::vector<std::string> lg0 = logSnapshot();
		for (size_t i = log0; i < lg0.size(); ++i) {
			offLine = offLine || lg0[i].find("[GXA2C] state=off reason=prewarm-failed at=reset dropped=") != std::string::npos;
			prewarmFailed = prewarmFailed || (lg0[i].find("[GXA2C] reset prewarm samples=2 ") != std::string::npos &&
				lg0[i].find(" complete=0") != std::string::npos);
		}
	}
	SETUP(hr == D3D_OK, "the Reset to 2x");
	SETUP(planted && prewarmFailed, "the Reset prewarm did not reach the failA2cKeys plant");
	CHECK(!e.nd->testA2COn() && e.nd->testA2COffCount() == 1 && offLine, " [A2C still on after a failed prewarm]");
	const GXRender::Rec::GxTranslateStats& ts = e.nd->translator().stats();
	const uint64_t on0 = ts.a2cOn, ref0 = ts.refused;
	const bool drew = a2cOneQuad(e.dev);
	const uint64_t on1 = e.nd->translator().stats().a2cOn, ref1 = e.nd->translator().stats().refused;
	const uint32_t runs1 = e.nd->testCacheStats().prebuildRuns;
	pp = presentParams(true, 1, D3DMULTISAMPLE_4_SAMPLES);
	const size_t log1 = logSnapshot().size();
	const bool back = SUCCEEDED(e.dev->Reset(&pp));
	const std::vector<std::string> lg = logSnapshot();
	bool prewarmAgain = false;
	for (size_t i = log1; i < lg.size(); ++i)
		prewarmAgain = prewarmAgain || lg[i].find("[GXA2C] reset prewarm") != std::string::npos;
	const uint32_t runs2 = e.nd->testCacheStats().prebuildRuns;
	const bool drewBack = a2cOneQuad(e.dev);
	const GxNativeValidation v = e.nd->validation();
	char b[240];
	std::snprintf(b, sizeof(b), "reset=0x%08x a2c=%d off_count=%u off_line=%d drew=%d a2c_draws=%llu refused=%llu prebuild_runs %u -> %u "
		"(back to 4x: ok=%d prewarm_line=%d drew=%d) validation %u/%u log_lines=%zu", unsigned(hr), int(e.nd->testA2COn()), e.nd->testA2COffCount(),
		int(offLine), int(drew), (unsigned long long)(on1 - on0), (unsigned long long)(ref1 - ref0), runs1, runs2, int(back), int(prewarmAgain),
		int(drewBack), v.errors, v.hazards, lg.size() - log0);
	*d += b;
	CHECK(drew && on1 == on0 && ref1 == ref0, " [the alpha-tested draw at 2x was not drawn on today's path]");
	CHECK(back && !prewarmAgain && runs2 == runs1 && drewBack, " [A2C work after it turned itself off]");
	CHECK(v.errors == 0 && v.hazards == 0, " [validation: " + v.first + "]");
	return ok;
}

struct Case
{
	const char* name;
	bool (*fn)(Env&, std::string*);
	bool GxNativeDebug::*plant;
	bool windowed;
	UINT backBuffers;
	bool window = false;   // a hidden SDL3 window + swapchain (present arms)
	D3DMULTISAMPLE_TYPE msaa = D3DMULTISAMPLE_NONE;
	int windowScale = 1;   // the window is the back buffer times this (a scaled present)
	bool noBC = false;     // forced GXRES capabilities: no BC (DXTn decoded on upload, the M3 iPad leg's fallback)
	bool GXRender::Res::GxDebug::*resPlant = nullptr;       // the X- arm's plant when it lives in GXRES ...
	bool GXRender::Rec::GxTranslateDebug::*trPlant = nullptr;   // ... or in the translator
	bool byNameOnly = false;   // not part of "all" (exit-final leaks its device; the verdict is read from stderr after exit)
	bool gpuTime = false;      // host.gpuTime (GX_GPUTIME)
	bool needsCs = false;      // A23-B: holds the worker - SKIP when the device runs direct (GX_NATIVE_CS=0)
	uint32_t ringKiB = 0;      // A23-B: GX_NATIVE_CS_RING_KIB for this arm's device (0 = the default ring)
	int strict = 0;            // A23-B --strict: 1 = its X- plant must also change the EXEC digest (runs normally too), 2 = only there,
	                           // 3 = a TAP plant (--strict-tap only: what it changes is not an exec input)
	bool xNeedsCs = false;     // A23-B: the clean arm runs in both modes, its X- plant acts only on the worker path (SKIP when direct)
	uint32_t stagingKiB = 0;   // A23-B: GX_NATIVE_STAGING_KIB for this arm's device (0 = the default 64 MiB staging ring)
	bool mirrorOnly = false;   // A23-B: runs only in the mirror-check mode (GX_NATIVE_CS_MIRRORCHECK=1)
	UINT bbWidth = 0;          // A23-B: a RECTANGULAR back buffer (and window) this wide; 0 = the fixture's 64 (square)
	// render/integration's fields (merge of 120c574b3): AFTER A23-B's, so A23-B's ~100 positional rows keep their meaning; the 9 rows
	// that set one of these carry A23-B's defaults (false, 0, 0, false, 0, false, 0) first (rw3-W/kcases-fields.py: 0 misfits)
	bool GXRender::Rec::GxRecDebug::*recPlant = nullptr;   // ... or in the packet writer (PREREG_T A25 P3)
	bool symptomGated = false;   // A25: the X- arm counts only if EVERY failure is the plant's stale value
	D3DFORMAT bbFormat = D3DFMT_A8R8G8B8;   // the back buffer's format (the native device admits A8R8G8B8 and X8R8G8B8 only, S4)
	UINT bbW = 64, bbH = 64;                 // the back buffer's size (the window is it times windowScale); bbWidth, when set, wins
	bool motionUnits = false;  // host.motionUnits (GX_MOTION_UNITS, PREREG_T A35); LAST: the vel-* rows carry A23-B's and W-a's defaults first
};

// The SERIAL schedule applies to every arm that holds no thread: never an ASYNC arm, a worker-hold arm (needsCs, arm 21's held
// worker) nor a thread- arm (it holds gx-vk-submit) - a drain / device idle after every call would wait for the hold forever.
bool serialArm(const Case& c)
{
	return !isAsyncArm(c.name) && !c.needsCs && std::strncmp(c.name, "thread-", 7) != 0 && std::strcmp(c.name, "cs-exec-ledger") != 0;
}

// idle-pressure (PREREG_T A26 amendment 1: the integrator's two-direction arm = through the D3D8 path).
// With no frame open (no draw, clear or present yet), 12 cycles of a 16 MiB MANAGED vertex buffer created, written, unlocked and
// released. Each unlock's staging waits for a submission nobody makes unless the idle flush fires. Every cycle must really stage its
// 16 MiB (GXRES stagingBytes, no GXRES error). Clean: the flushes fall EXACTLY where the model puts them (pressure
// starts at what device creation staged, +16 MiB per cycle, a flush past 32 MiB resets it - a26-r2 #3: an early or extra flush
// fails), the pressure after each cycle equals the model, and no staging leaves the 64 MiB ring. The X arm (stagingPressure() =
// pendingBytes(), A26 v1's trigger) is detected ONLY by its own symptom: every cycle staged, no flush at all, >= 4 overflow buffers.
bool caseIdlePressure(Env& e, std::string* d)
{
	GXRender::Res::GxResources& gx = e.nd->resources();
	const bool planted = gx.debug().pressureIsPending;
	const GXRender::Vk::VkUploadRing::Stats s0 = gx.ring().stats();
	const uint64_t f0 = e.nd->stats().uploadFlushes, err0 = e.nd->stats().resourceErrors;
	const uint64_t kBytes = 16ull << 20, kThreshold = 32ull << 20;
	uint64_t model = gx.stagingPressure();
	std::string want, got;
	bool setup = true, tracks = true;
	for (int i = 1; i <= 12 && setup; ++i) {
		const uint64_t staged0 = gx.stats().stagingBytes, fl0 = e.nd->stats().uploadFlushes;
		IDirect3DVertexBuffer8* vb = nullptr;
		BYTE* p = nullptr;
		setup = SUCCEEDED(e.dev->CreateVertexBuffer(UINT(kBytes), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb)) &&
			SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p;
		if (p) {
			std::memset(p, i, size_t(kBytes));
			vb->Unlock();
		}
		if (vb)
			vb->Release();
		setup = setup && gx.stats().stagingBytes == staged0 + kBytes;
		model += kBytes;
		if (model > kThreshold) {
			want += " " + std::to_string(i);
			model = 0;
		}
		for (uint64_t k = e.nd->stats().uploadFlushes - fl0; k > 0; --k)   // one entry per flush: two in one cycle read "i i" (a26-r3)
			got += " " + std::to_string(i);
		if (!planted && gx.stagingPressure() != model)
			tracks = false;
	}
	setup = setup && e.nd->stats().resourceErrors == err0;
	const GXRender::Vk::VkUploadRing::Stats& s1 = gx.ring().stats();
	const uint64_t overflow = s1.dedicated - s0.dedicated, flushes = e.nd->stats().uploadFlushes - f0;
	*d += std::string("setup ") + (setup ? "ok" : "FAILED") + "; idle flushes at cycles" + (got.empty() ? " none" : got) +
		" (model" + want + ")" + (tracks ? "" : ", pressure OFF the model") + "; dedicated overflow " + std::to_string(overflow) +
		" buffers / " + std::to_string((s1.dedicatedBytes - s0.dedicatedBytes) >> 20) + " MiB;";
	if (planted)
		return !(setup && flushes == 0 && overflow >= 4);   // false = detected, by the planted symptom only
	return setup && got == want && tracks && overflow == 0;
}

// gputime (B · rw3-T-3 12/10/2026, PREREG_T A16): host.gpuTime - the device validates its VkGpuTimer BEFORE GxResources (else GXRES
// contract #1 trips on the probes) and prints one [GXGPU] line per 120 presents. 250 presents paced at 1 ms: the provenance line says
// VALIDATED, and every window line read >= 100 frames with <= 2 missing and a median > 0 ms.
bool caseGpuTime(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	for (int i = 0; i < 250 && ok; ++i) {
		CHECK(bb && clearSurface(e.dev, bb, 0xff000000 | uint32_t(i)), "clear back buffer");
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (bb)
		bb->Release();
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> g(g_logLock);
		for (const std::string& l : g_log)
			if (l.compare(0, 8, "[GXGPU] ") == 0)
				lines.push_back(l);
	}
	int validated = 0, windows = 0, good = 0;
	for (const std::string& l : lines) {
		validated += l.compare(0, 29, "[GXGPU] provenance=VALIDATED ") == 0;
		unsigned long long presents = 0;
		size_t n = 0;
		unsigned miss = 0;
		double med = 0.0, np = 0.0, scopes = 0.0;
		const size_t at = l.find(" np=");
		const bool a17 = at != std::string::npos && std::sscanf(l.c_str() + at, " np=%lf", &np) == 1 &&
			l.find(" scopes=") != std::string::npos && std::sscanf(l.c_str() + l.find(" scopes="), " scopes=%lf", &scopes) == 1;
		// A30: the per-slot counts cover every slot of the run's --fif (s2 only at 3) and add up to n
		size_t sn[3] = {};
		const size_t s0 = l.find(" s0="), s1 = l.find(" s1="), s2 = l.find(" s2=");
		const bool slotsOk = s0 != std::string::npos && s1 != std::string::npos && std::sscanf(l.c_str() + s0, " s0=%zu/", &sn[0]) == 1 &&
			std::sscanf(l.c_str() + s1, " s1=%zu/", &sn[1]) == 1 && (s2 != std::string::npos) == (g_fif == 3) &&
			(s2 == std::string::npos || std::sscanf(l.c_str() + s2, " s2=%zu/", &sn[2]) == 1);
		if (std::sscanf(l.c_str(), "[GXGPU] presents=%llu n=%zu miss=%u med=%lf", &presents, &n, &miss, &med) == 4) {
			++windows;
			good += n >= 100 && miss <= 2 && med > 0.0 && a17 && np >= 1.0 && scopes >= 1.0   // A17: >= 1 pass per frame
				&& slotsOk && sn[0] + sn[1] + sn[2] == n && sn[0] && sn[1] && (g_fif == 2 || sn[2]);
			*d += l.substr(8, 60) + "; ";
		}
	}
	CHECK(validated == 1, "no single [GXGPU] provenance=VALIDATED line");
	CHECK(windows == 2 && good == windows, ("[GXGPU] windows " + std::to_string(good) + " good of " + std::to_string(windows)).c_str());
	return ok;
}

// cs-ring (A23-B, PREREG_T A23-B; DESIGN §5 + §7 arm 14 at the stream level): GxNativeCs alone, no device involved.
// (1) 4000 commands with payloads 0..3000 B through a 64 KiB ring (dozens of wraps, padding records): each runs once, in push
// order, and reads exactly the bytes pushed. (2) The worker held at seq 1, 200 x 1 KiB pushed: the producer WAITS for room (no
// failure, counted), every command runs once in order after the release. (3) Eight records of exactly capacity/8 fill the ring
// while the worker is held at seq 1; a ninth is pushed (clean: it waits for room; X-cs-ring-overwrite: no room check, it
// replaces the unread record 1) - the worker's sequence check must count the replacement and run nothing of a corrupt stream.
struct CsRingProbe
{
	uint32_t index;
	std::vector<uint32_t>* ran;
	void operator()(const uint8_t*, size_t) const { ran->push_back(index); }
};

bool caseCsRing(Env& e, std::string* d)
{
	bool ok = true;
	const bool plant = e.nd->debug().csRingOverwrite;
	using GXRender::Dev::GxNativeCs;
	auto inOrder = [](const std::vector<uint32_t>& v, uint32_t n) {
		if (v.size() != n)
			return false;
		for (uint32_t i = 0; i < n; ++i)
			if (v[i] != i)
				return false;
		return true;
	};
	{   // (1)
		GxNativeCs cs;
		SETUP(cs.start(64u << 10, "gx-cs-test"), "64 KiB stream start");
		std::vector<uint32_t> ran;
		uint32_t bad = 0;
		std::vector<uint8_t> src;
		for (uint32_t i = 0; i < 4000; ++i) {
			const size_t n = size_t(i * 2654435761u) % 3001;
			src.resize(n);
			for (size_t k = 0; k < n; ++k)
				src[k] = uint8_t(i + k * 7);
			cs.push([i, n, &ran, &bad](const uint8_t* p, size_t m) {
				if (m != n)
					++bad;
				for (size_t k = 0; k < m; ++k)
					if (p[k] != uint8_t(i + k * 7)) {
						++bad;
						break;
					}
				ran.push_back(i);
			}, src.data(), n);
		}
		cs.drain();
		CHECK(inOrder(ran, 4000), "(1) not every command ran once in push order");
		CHECK(bad == 0, "(1) a payload differed from the bytes pushed");
		CHECK(cs.stats().seqErrors == 0, "(1) sequence errors on a clean stream");
		CHECK(cs.stats().payloadBytes > 64u << 10, "(1) the payloads did not wrap the ring");
		cs.stop();
	}
	{   // (2)
		GxNativeCs cs;
		SETUP(cs.start(64u << 10, "gx-cs-test"), "64 KiB stream start");
		cs.testHoldAt(1);
		std::thread rel([&cs] {
			while (!cs.testHeld())
				std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			cs.testRelease();
		});
		std::vector<uint32_t> ran;
		std::vector<uint8_t> kib(1024, 0x5a);
		for (uint32_t i = 0; i < 200; ++i)
			cs.push(CsRingProbe { i, &ran }, kib.data(), kib.size());
		rel.join();
		cs.drain();
		const GXRender::Dev::GxNativeCsStats st = cs.stats();
		CHECK(inOrder(ran, 200), "(2) not every command ran once in push order after the full ring");
		CHECK(st.fullWaits > 0 && st.fullNs > 0, "(2) the producer never waited for room behind the held worker");
		CHECK(st.seqErrors == 0, "(2) sequence errors");
		cs.stop();
	}
	{   // (3)
		GxNativeCs cs;
		const size_t cap = 64u << 10, rec = cap / 8;
		const size_t payload = rec - GxNativeCs::testRecordBytes<CsRingProbe>(0);
		SETUP(GxNativeCs::testRecordBytes<CsRingProbe>(payload) == rec, "record size");
		SETUP(cs.start(cap, "gx-cs-test"), "64 KiB stream start");
		cs.testHoldAt(1);
		std::vector<uint32_t> ran;
		std::vector<uint8_t> bytes(payload, 0x33);
		for (uint32_t i = 0; i < 8; ++i)
			cs.push(CsRingProbe { i, &ran }, bytes.data(), bytes.size());
		while (!cs.testHeld())
			std::this_thread::yield();
		std::thread rel;
		if (plant)
			cs.testNoRoomCheck = true;
		else
			rel = std::thread([&cs] {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				cs.testRelease();
			});
		cs.push(CsRingProbe { 8, &ran }, bytes.data(), bytes.size());
		if (plant)
			cs.testRelease();
		else
			rel.join();
		cs.drain();
		const GXRender::Dev::GxNativeCsStats st = cs.stats();
		CHECK(st.seqErrors == 0, "(3) the sequence check counted " + std::to_string(st.seqErrors) + " replaced record(s)");
		CHECK(inOrder(ran, 9), "(3) the nine commands did not run once each in order (ran " + std::to_string(ran.size()) + ")");
		cs.stop();
	}
	return ok;
}

// A55 (PREREG_RF A55): a sleeping producer is woken when what it waits for is reached, not per record. cs-wake: the worker held at
// seq 1 with 20000 records queued, the producer waits for the last one and is asleep (past its 20 us spin) before the release: over
// the whole wait the worker notifies it at most twice (its wake count is printed); every record runs once, in order. X-cs-wake (the
// worker notifies after every record while the producer waits - the wake storm of 15/10: main RUNNING in this wait ~1.4 ms/frame at
// Wh1ter0se's camera, the worker ~0.5 ms/frame in notify_one) must fail the bound.
struct CsWakeProbe
{
	std::atomic<uint64_t>* sum;
	uint32_t spinNs;
	void operator()(const uint8_t*, size_t) const
	{
		if (spinNs) {
			const auto t0 = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - t0 < std::chrono::nanoseconds(spinNs)) {
			}
		}
		sum->fetch_add(1, std::memory_order_relaxed);
	}
};

bool caseCsWake(Env& e, std::string* d)
{
	bool ok = true;
	using GXRender::Dev::GxNativeCs;
	const uint32_t n = 20000;
	GxNativeCs cs;
	SETUP(cs.start(4u << 20, "gx-cs-test"), "4 MiB stream start");
	cs.testWakeEveryRecord = e.nd->debug().csWakeEveryRecord;
	cs.testHoldAt(1);
	std::vector<uint32_t> ran;
	ran.reserve(n);
	for (uint32_t i = 0; i < n; ++i)
		cs.push(CsRingProbe { i, &ran });
	std::thread rel([&cs] {
		while (!cs.testHeld() || !cs.testProducerWaiting())
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the producer is asleep in its condition variable by now
		cs.testRelease();
	});
	cs.waitFor(cs.queued());
	rel.join();
	const uint64_t done = cs.done(), queued = cs.queued();
	cs.stop();   // joins the worker: its notify count is exact after
	const GXRender::Dev::GxNativeCsStats st = cs.stats();
	*d += "producer wakes " + std::to_string(st.producerWakes) + ", worker notifies " + std::to_string(st.producerNotifies);
	bool order = ran.size() == n;
	for (uint32_t i = 0; order && i < n; ++i)
		order = ran[i] == i;
	CHECK(done == queued && order, "the records did not all run once, in push order, before the wait returned");
	CHECK(st.leadWaits == 1, "the wait was not one counted lead wait");
	// the bound is on the worker's notifies: a condition variable may also return spuriously (wakes stay a diagnostic)
	CHECK(st.producerNotifies <= 2, "the sleeping producer was woken per record (the wake storm)");
	CHECK(st.seqErrors == 0, "sequence errors");
	return ok;
}

// cs-wake-target (A55; in a child - the arm watchdog ends a hang): (1) 300 waits for the LAST queued seq made while the worker is held
// before the first of 1..17 new records, the producer asleep before the release - the case a wrong wake target can never wake;
// (2) 20000 waits for a random queued seq with the worker free; (3) the worker held at the next record while 400 x 1 KiB records go
// into the 64 KiB ring: the producer SLEEPS waiting for room (released once it is), and is woken by progress - its wakes counted
// before the final drain. X-cs-wake-target (the worker wakes only once PAST the target) must hang
// in (1).
bool caseChildCsWakeTarget(Env& e, std::string* d)
{
	bool ok = true;
	using GXRender::Dev::GxNativeCs;
	GxNativeCs cs;
	SETUP(cs.start(64u << 10, "gx-cs-test"), "64 KiB stream start");
	cs.testWakeLate = e.nd->debug().csWakeLate;
	std::atomic<uint64_t> sum { 0 };
	uint64_t pushed = 0;
	std::fprintf(stderr, "[TEST] WAKE-TARGET started\n");   // set up: what follows is the arm itself (a planted run hangs after this)
	for (uint32_t k = 0; k < 300; ++k) {   // (1)
		cs.testHoldAt(cs.queued() + 1);
		for (uint32_t i = 0, r = 1 + k % 17; i < r; ++i, ++pushed)
			cs.push(CsWakeProbe { &sum, 0 });
		std::thread rel([&cs] {
			while (!cs.testHeld() || !cs.testProducerWaiting())
				std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			cs.testRelease();
		});
		cs.waitFor(cs.queued());
		rel.join();
	}
	const uint64_t heldWakes = cs.stats().producerWakes;
	uint32_t rng = 12345;
	for (uint32_t k = 0; k < 20000; ++k) {   // (2)
		rng = rng * 1664525u + 1013904223u;
		const uint32_t r = 1 + (rng >> 8) % 8;
		for (uint32_t i = 0; i < r; ++i, ++pushed)
			cs.push(CsWakeProbe { &sum, 0 });
		cs.waitFor(cs.queued() - (rng >> 16) % r);
	}
	const GXRender::Dev::GxNativeCsStats before3 = cs.stats();
	std::vector<uint8_t> kib(1024, 0x6b);
	cs.testHoldAt(cs.queued() + 1);   // (3)
	std::thread rel3([&cs] {
		while (!cs.testHeld() || !cs.testProducerWaiting())
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		cs.testRelease();
	});
	for (uint32_t k = 0; k < 400; ++k, ++pushed)
		cs.push(CsWakeProbe { &sum, 0 }, kib.data(), kib.size());
	rel3.join();
	const uint64_t roomSleeps = cs.stats().producerWakes - before3.producerWakes;   // the room waits only: before the drain
	cs.drain();
	const GXRender::Dev::GxNativeCsStats st = cs.stats();
	cs.stop();
	*d += "held-wait wakes " + std::to_string(heldWakes) + ", room waits " + std::to_string(st.fullWaits - before3.fullWaits) +
		" (woken " + std::to_string(roomSleeps) + ")";
	CHECK(sum.load() == pushed && st.seqErrors == 0, "not every record ran once");
	CHECK(heldWakes >= 300, "(1) the producer did not sleep in the held waits (the arm tests nothing)");
	CHECK(st.fullWaits > before3.fullWaits && roomSleeps > 0, "(3) the producer never slept waiting for room");
	std::fprintf(stderr, "[TEST] WAKE-TARGET ok pushed=%llu held_wakes=%llu\n", (unsigned long long)pushed, (unsigned long long)heldWakes);
	return ok;
}

// ---------------------------------------------------------------------------------------------- A23-B hold-hook arms (§7)
// Each needs the worker (GX_NATIVE_CS=1; SKIP without it). The worker is stopped before the next command (GxNativeCs::testHoldAt),
// so a race becomes one deterministic state; each X- arm plants the defect DESIGN v8.1 §7 names and must fail for that reason.
namespace csarm {
struct QV { float x, y, z, rhw; DWORD c; };
struct TV { float x, y, z, rhw, u, v; };
void quad(QV* v, float x0, float y0, float x1, float y1, DWORD c)
{
	const QV q[4] = { { x0, y0, 0.5f, 1, c }, { x1, y0, 0.5f, 1, c }, { x0, y1, 0.5f, 1, c }, { x1, y1, 0.5f, 1, c } };
	std::memcpy(v, q, sizeof(q));
}
void ffDiffuse(Env& e)
{
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}
void hold(Env& e)
{
	GxNativeCs& s = e.nd->csStream();
	s.testHoldAt(s.queued() + 1);
}
void release(Env& e) { e.nd->csStream().testRelease(); }
uint32_t px(Env& e, IDirect3DSurface8* s, UINT x, UINT y)
{
	uint32_t p = 0;
	if (!readPixel(e.dev, s, x, y, &p))
		return 0xdeadbeefu;
	return p & 0xffffff;
}
std::string hex(uint32_t v)
{
	char b[16];
	std::snprintf(b, sizeof(b), "%06x", v);
	return b;
}
// Present / Present-like call on this thread while a helper waits for the producer to block in the worker's fence, then
// releases the hold. blocked = the producer was seen waiting; releasedFirst = the release happened before the call returned.
template <class Call>
void blockedCall(Env& e, Call call, bool* blocked, bool* releasedFirst)
{
	GxNativeCs& s = e.nd->csStream();
	std::atomic<bool> released { false }, sawBlocked { false };
	std::thread helper([&] {
		const auto t0 = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
			if (s.testHeld() && s.testProducerWaiting()) {
				sawBlocked = true;
				break;
			}
			std::this_thread::yield();
		}
		released = true;
		s.testRelease();
	});
	call();
	*releasedFirst = released.load();
	helper.join();
	*blocked = sawBlocked.load();
}
} // namespace csarm

// arm 1: a getter right after its setter with the worker held answers the value set (the front mirror); X: the translator's stale one
bool caseCsGetter(Env& e, std::string* d)
{
	bool ok = true;
	DWORD before = 0, got = 0;
	e.dev->GetRenderState(D3DRS_CULLMODE, &before);
	const DWORD want = before == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW;
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_CULLMODE, want);
	e.dev->GetRenderState(D3DRS_CULLMODE, &got);
	csarm::release(e);
	*d += "set " + std::to_string(want) + " got " + std::to_string(got);
	CHECK(got == want, "the getter answers the value just set while the worker has not run the setter");
	return ok;
}

// arm 2: DISCARD, draw, DISCARD, draw with the worker held - each draw shows its own bytes; X: the second DISCARD reuses the slice
// the first queued draw still binds (workerDone ignored) - both draws read the second quad
bool caseCsDiscard(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	csarm::hold(e);
	BYTE* p = nullptr;
	if (SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p) {
		csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 32, 64, 0xff112233);
		vb->Unlock();
	}
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	p = nullptr;
	if (SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p) {
		csarm::quad(reinterpret_cast<QV*>(p), 32, 0, 64, 64, 0xff445566);
		vb->Unlock();
	}
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	csarm::release(e);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0x112233 && b == 0x445566, "each queued draw shows the bytes of its own DISCARD");
	bb->Release();
	vb->Release();
	return ok;
}

// arm 3: NOOVERWRITE appends across queued draws (the contract case: the caller never touches bytes a pending draw reads)
bool caseCsNoOverwrite(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(8 * sizeof(QV), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	csarm::hold(e);
	BYTE* p = nullptr;
	if (SUCCEEDED(vb->Lock(0, 4 * sizeof(QV), &p, D3DLOCK_DISCARD)) && p) {
		csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 32, 64, 0xff216549);
		vb->Unlock();
	}
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	p = nullptr;
	if (SUCCEEDED(vb->Lock(4 * sizeof(QV), 4 * sizeof(QV), &p, D3DLOCK_NOOVERWRITE)) && p) {
		csarm::quad(reinterpret_cast<QV*>(p), 32, 0, 64, 64, 0xff7a3c11);
		vb->Unlock();
	}
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 4, 2);
	csarm::release(e);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0x216549 && b == 0x7a3c11, "both appended quads drawn");
	bb->Release();
	vb->Release();
	return ok;
}

// arm 4: a static (MANAGED) VB written A / unlocked / drawn, then written B at the same bytes / unlocked / drawn, worker held -
// A then B; X-cs-unlock-by-ref: the worker stages from the shadow when it runs, so A's draw shows B
bool caseCsStaticUnlock(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "static VB");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	csarm::hold(e);
	for (int i = 0; i < 2; ++i) {
		BYTE* p = nullptr;
		if (SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p) {
			csarm::quad(reinterpret_cast<QV*>(p), i ? 32.f : 0.f, 0, i ? 64.f : 32.f, 64, i ? 0xff3355aa : 0xffaa5533);
			vb->Unlock();
		}
		e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	}
	csarm::release(e);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0xaa5533 && b == 0x3355aa, "the first draw shows the first upload, the second the second");
	bb->Release();
	vb->Release();
	return ok;
}

// arm 5: the same with a MANAGED texture's LockRect / UnlockRect; X-cs-texunlock-by-ref
bool caseCsTexUnlock(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::TV;
	IDirect3DTexture8* tex = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "MANAGED texture");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	e.dev->SetTexture(0, tex);
	e.dev->BeginScene();
	csarm::hold(e);
	for (int i = 0; i < 2; ++i) {
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x)
					reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = i ? 0xff2244ccu : 0xffcc4422u;
			tex->UnlockRect(0);
		}
		const float x0 = i ? 32.f : 0.f, x1 = x0 + 32.f;
		const TV q[4] = { { x0, 0, 0.5f, 1, 0, 0 }, { x1, 0, 0.5f, 1, 1, 0 }, { x0, 64, 0.5f, 1, 0, 1 }, { x1, 64, 0.5f, 1, 1, 1 } };
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
	}
	csarm::release(e);
	e.dev->EndScene();
	e.dev->SetTexture(0, nullptr);
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0xcc4422 && b == 0x2244cc, "the first draw samples the first upload, the second the second");
	bb->Release();
	tex->Release();
	return ok;
}

// arm 6: DrawPrimitiveUP from a stack buffer overwritten right after the call, worker held - the original vertices are drawn;
// X-cs-up-by-ref: the worker reads the caller's buffer when it runs - the overwritten quad is drawn instead
bool caseCsUp(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->BeginScene();
	QV q[4];
	csarm::quad(q, 0, 0, 32, 64, 0xff5599dd);
	csarm::hold(e);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
	csarm::quad(q, 32, 0, 64, 64, 0xffdd9955);   // the caller's buffer, reused at once (the d3d8 contract allows it)
	csarm::release(e);
	e.dev->EndScene();
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0x5599dd && b == 0, "the vertices of the call, not the buffer's later content");
	bb->Release();
	return ok;
}

// A-up-notarget (§3.2 residual): an UP draw with no colour image bound is refused on the worker after the copy - counted, no crash
bool caseCsUpNoTarget(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8 *img = nullptr, *bb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &img)) && img, "image surface");
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	const uint64_t refused0 = e.nd->stats().drawsRefused;
	SETUP(SUCCEEDED(e.dev->SetRenderTarget(img, nullptr)), "an image surface as the target (no colour image)");
	csarm::ffDiffuse(e);
	QV q[4];
	csarm::quad(q, 0, 0, 32, 64, 0xff5599dd);
	CHECK(e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV)) == D3D_OK, "D3D_OK, as today");
	const uint64_t refused = e.nd->stats().drawsRefused - refused0;
	*d += "refused +" + std::to_string(refused);
	CHECK(refused == 1, "the draw refused on the worker, counted");
	if (bb) {
		e.dev->SetRenderTarget(bb, nullptr);
		bb->Release();
	}
	img->Release();
	return ok;
}

// arm 7: the shroud pattern - CopyRects SYSTEMMEM -> DEFAULT, then the source written through the pointer kept after UnlockRect,
// worker held: the copy holds the OLD texels; X-cs-copy-late-read: the worker reads the source when it runs - the new ones
bool caseCsCopyLateRead(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8 *src = nullptr, *dstL = nullptr;
	IDirect3DTexture8* dst = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(4, 4, D3DFMT_A8R8G8B8, &src)) && src, "SYSTEMMEM source");
	SETUP(SUCCEEDED(e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst)) && dst, "DEFAULT texture");
	dst->GetSurfaceLevel(0, &dstL);
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(src->LockRect(&lr, nullptr, 0)), "lock the source");
	auto fill = [&](uint32_t c) {
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 4; ++x)
				reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = c;
	};
	fill(0xff11aa33u);
	src->UnlockRect();
	csarm::hold(e);
	CHECK(SUCCEEDED(e.dev->CopyRects(src, nullptr, 0, dstL, nullptr)), "CopyRects SYSTEMMEM -> DEFAULT");
	fill(0xff33aa11u);   // through the kept pointer (W3DShroud, GxTexture.h:8-9)
	csarm::release(e);
	const uint32_t t = csarm::px(e, dstL, 1, 1);
	*d += "texel " + csarm::hex(t);
	CHECK(t == 0x11aa33, "the copy holds the source as it was at the call");
	dstL->Release();
	dst->Release();
	src->Release();
	return ok;
}

// arm 9: the lead - the worker held before Present 1 runs: Present 1 returns, Present 2 BLOCKS until the release (seen through the
// producer's blocked state, never a sleep); X-cs-no-lead: Present 2 returns at once
bool caseCsLead(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	e.nd->csSync();
	csarm::hold(e);
	const HRESULT p1 = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	bool blocked = false, releasedFirst = false;
	HRESULT p2 = E_FAIL;
	csarm::blockedCall(e, [&] { p2 = e.dev->Present(nullptr, nullptr, nullptr, nullptr); }, &blocked, &releasedFirst);
	*d += std::string("Present 2 blocked: ") + (blocked ? "yes" : "no") + ", returned after the release: " + (releasedFirst ? "yes" : "no");
	CHECK(SUCCEEDED(p1) && SUCCEEDED(p2), "both Presents succeed");
	CHECK(blocked && releasedFirst, "Present 2 waits for the worker to finish Present 1");
	return ok;
}

// arm 12: a loss latched on the WORKER (inside its Present) reaches the game within <= 2 Presents and TestCooperativeLevel;
// X-cs-loss-unpublished: the front never sees it - Presents keep answering D3D_OK
bool caseCsLoss(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	e.nd->debug().loseDeviceMidPresent = true;   // latched by the worker inside the next Present
	HRESULT h[3];
	for (HRESULT& x : h)
		x = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	e.nd->csSync();
	const HRESULT tcl = e.dev->TestCooperativeLevel();
	*d += "Presents " + std::to_string(long(h[0])) + " " + std::to_string(long(h[1])) + " " + std::to_string(long(h[2])) + ", TCL " +
		std::to_string(long(tcl));
	CHECK(h[1] == D3DERR_DEVICELOST || h[2] == D3DERR_DEVICELOST, "DEVICELOST within <= 2 Presents of the one that latched it");
	CHECK(tcl == D3DERR_DEVICELOST, "TestCooperativeLevel answers DEVICELOST");
	return ok;
}

// arm 12b: the loss visible, the worker held - Present 1 answers DEVICELOST, Present 2 BLOCKS until the release (a PresentLost is
// fenced like any Present); X-cs-lost-unfenced: Present 2 returns at once
bool caseCsLostFenced(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	e.nd->ctx().simulateDeviceLost();
	csarm::hold(e);
	const HRESULT p1 = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	bool blocked = false, releasedFirst = false;
	HRESULT p2 = E_FAIL;
	csarm::blockedCall(e, [&] { p2 = e.dev->Present(nullptr, nullptr, nullptr, nullptr); }, &blocked, &releasedFirst);
	*d += std::string("Present 2 blocked: ") + (blocked ? "yes" : "no") + ", returned after the release: " + (releasedFirst ? "yes" : "no");
	CHECK(p1 == D3DERR_DEVICELOST && p2 == D3DERR_DEVICELOST, "both answer DEVICELOST");
	CHECK(blocked && releasedFirst, "the second PresentLost waits for the first");
	return ok;
}

// arms 14 + 25 (a 64 KiB TEST ring): a burst that overfills it with the worker held - the caller WAITS for room (no call fails),
// every call answers D3D_OK, the frame is right; an UP draw and an unlock larger than kMaxCmd go S, complete and draw.
// X-cs-full-fails: a full ring fails the call (INVALIDCALL)
bool caseCsRingFull(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	SETUP(e.nd->csStream().capacity() == (64u << 10), "the 64 KiB TEST ring (GX_NATIVE_CS_RING_KIB=64)");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->BeginScene();
	const GxNativeCsStats s0 = e.nd->csStream().stats();
	csarm::hold(e);
	int bad = 0;
	bool blocked = false, releasedFirst = false;
	csarm::blockedCall(e, [&] {
		for (DWORD i = 0; i < 4000; ++i)
			bad += e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, i) != D3D_OK;
	}, &blocked, &releasedFirst);
	QV q[4];
	csarm::quad(q, 0, 0, 32, 64, 0xff40c0f0);
	bad += e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV)) != D3D_OK;
	// oversize UP (> kMaxCmd = 16 KiB - its header): 1200 triangles of the right half, S
	const uint64_t syncs0 = e.nd->csSyncCount();
	std::vector<QV> big(1202);
	for (size_t i = 0; i < big.size(); ++i)
		big[i] = { (i & 1) ? 64.f : 32.f, float((i / 2) % 2 ? 64 : 0), 0.5f, 1, 0xfff0c040 };
	bad += e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 1200, big.data(), sizeof(QV)) != D3D_OK;
	const uint64_t syncs = e.nd->csSyncCount() - syncs0;
	e.dev->EndScene();
	const GxNativeCsStats s1 = e.nd->csStream().stats();
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "calls failed " + std::to_string(bad) + ", producer blocked " + (blocked ? "yes" : "no") + ", full waits +" +
		std::to_string(s1.fullWaits - s0.fullWaits) + ", oversize S +" + std::to_string(syncs) + ", left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(bad == 0, "no call fails on a full ring");
	CHECK(blocked && s1.fullWaits > s0.fullWaits, "the caller waited for room");
	CHECK(syncs >= 1, "the oversize UP draw went S");
	CHECK(a == 0x40c0f0 && b == 0xf0c040, "both draws landed");
	bb->Release();
	return ok;
}

// arm 18: S reconciliation - a plain lock on a dynamic buffer (S, today's rename with its bytes kept) between queued draws, then
// DISCARD and NOOVERWRITE: exact pixels; X-cs-no-reconcile (GXRES: the worker's current slice left stale) - wrong bytes drawn
bool caseCsReconcile(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(8 * sizeof(QV), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	auto lockDraw = [&](DWORD flags, UINT first, float x0, float y0, float x1, float y1, DWORD c) {
		BYTE* p = nullptr;
		if (SUCCEEDED(vb->Lock(first * sizeof(QV), 4 * sizeof(QV), &p, flags)) && p) {
			csarm::quad(reinterpret_cast<QV*>(p), x0, y0, x1, y1, c);
			vb->Unlock();
		}
		e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, first, 2);
	};
	lockDraw(D3DLOCK_DISCARD, 0, 0, 0, 32, 32, 0xff102030);
	lockDraw(0, 4, 32, 0, 64, 32, 0xff405060);   // plain: S (the slice is read by the recorded frame: today renames it)
	lockDraw(D3DLOCK_DISCARD, 0, 0, 32, 32, 64, 0xff708090);
	lockDraw(D3DLOCK_NOOVERWRITE, 4, 32, 32, 64, 64, 0xffa0b0c0);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	const uint32_t p[4] = { csarm::px(e, bb, 10, 10), csarm::px(e, bb, 50, 10), csarm::px(e, bb, 10, 50), csarm::px(e, bb, 50, 50) };
	*d += csarm::hex(p[0]) + " " + csarm::hex(p[1]) + " " + csarm::hex(p[2]) + " " + csarm::hex(p[3]);
	CHECK(p[0] == 0x102030 && p[1] == 0x405060 && p[2] == 0x708090 && p[3] == 0xa0b0c0, "four quads, each its own bytes");
	bb->Release();
	vb->Release();
	return ok;
}

// TSan lane probe (§7 "TSan"): presents with rotating back buffers while the worker runs them. Clean: nothing for TSan; X-cs-race:
// the front reads the worker's rotation index - a TSan build must report a data race (the lane sees this code). Non-TSan builds:
// both pass (the verdict is TSan's report count, read from stderr by the lane script).
bool caseCsTsanProbe(Env& e, std::string* d)
{
	bool ok = true;
	for (int i = 0; i < 24; ++i)
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present");
	e.nd->csSync();
	*d += "24 presents";
	return ok;
}

// X-cs-reorder's pixel arm (§7.1 b): COLORWRITEENABLE 0 then a draw, worker held - nothing is written; X: the state runs after the
// draw - the quad lands
bool caseCsReorder(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	const bool plant = e.nd->dbg().csReorder;   // the one-shot plant must take THIS state, not the setup's first one
	e.nd->debug().csReorder = false;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->BeginScene();
	QV q[4];
	csarm::quad(q, 0, 0, 64, 64, 0xffffffff);
	e.nd->debug().csReorder = plant;
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
	e.dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
	csarm::release(e);
	e.dev->EndScene();
	const uint32_t a = csarm::px(e, bb, 30, 30);
	*d += "pixel " + csarm::hex(a);
	CHECK(a == 0, "the draw ran with colour writes off");
	bb->Release();
	return ok;
}

// arm 21 (STRICT, --strict compares the digests): worker held before a SetRenderState; then a draw, CreateTexture (on the front at
// once), SetTexture(new), a draw; release. Normal runs check the frame; --strict requires the EXEC digest and the creation ledger
// to equal CS=0's. X-cs-exec-physical (the creation also recorded into the EXEC stream at its physical time) only shows in --strict.
bool caseCsExecLedger(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::TV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->BeginScene();
	if (e.nd->csOn())
		csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00);
	csarm::QV q[4];
	csarm::quad(q, 0, 0, 32, 64, 0xff806040);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
	IDirect3DTexture8* tex = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "CreateTexture while held");
	if (tex) {
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x)
					reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff4060a0u;
			tex->UnlockRect(0);
		}
	}
	e.dev->SetTexture(0, tex);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const TV t[4] = { { 32, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 32, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, t, sizeof(TV));
	if (e.nd->csOn())
		csarm::release(e);
	e.dev->EndScene();
	e.dev->SetTexture(0, nullptr);
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(a == 0x806040 && b == 0x4060a0, "both draws");
	if (tex)
		tex->Release();
	bb->Release();
	return ok;
}

// ---------------------------------------------------------------------------------------------- A23-B pressure arms (§3.8, §7)
namespace csarm {
// the bytes of live VMA allocations - what the TEST budget (GxNativeDebug::allocBudget) is compared with
uint64_t vmaBytes(Env& e)
{
	VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
	vmaGetHeapBudgets(e.nd->ctx().allocator(), b);
	uint64_t used = 0;
	for (const VmaBudget& h : b)
		used += h.statistics.allocationBytes;
	return used;
}
} // namespace csarm

// arm 17: creation retry - a released 4 MiB texture's Destroy is queued (worker held) when a creation needs its memory under a TEST
// budget: the front attempt fails, the S retry drains (the Destroy runs), recovers, and today's body succeeds - creates +1,
// refused +0, alloc_failures +0, cs_create_retries +1. X-cs-no-retry: INVALIDCALL; X-cs-retry-double-count: creates +2
bool caseCsCreateRetry(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8 *a = nullptr, *b = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(1024, 1024, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &a)) && a, "texture A (4 MiB)");
	const GxNativeStats s0 = e.nd->stats();
	const uint64_t retries0 = e.nd->csCreateRetries(), fail0 = e.nd->allocFailures();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + (1u << 20);   // B fits only once A's storage is gone
	csarm::hold(e);
	a->Release();   // its Destroy goes out with the next creation's drainDestroys - behind the hold
	HRESULT hr = E_FAIL;
	bool blocked = false, releasedFirst = false;
	csarm::blockedCall(e, [&] { hr = e.dev->CreateTexture(1024, 1024, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &b); }, &blocked, &releasedFirst);
	e.nd->debug().allocBudget = 0;
	const GxNativeStats s1 = e.nd->stats();
	*d += "hr " + std::to_string(long(hr)) + ", creates +" + std::to_string(s1.creates - s0.creates) + ", refused +" +
		std::to_string(s1.createsRefused - s0.createsRefused) + ", retries +" + std::to_string(e.nd->csCreateRetries() - retries0) +
		", alloc_failures +" + std::to_string(e.nd->allocFailures() - fail0);
	CHECK(hr == D3D_OK && b, "the creation succeeds after the S retry");
	CHECK(s1.creates - s0.creates == 1 && s1.createsRefused == s0.createsRefused, "counted once, refused never");
	CHECK(e.nd->csCreateRetries() - retries0 == 1 && e.nd->allocFailures() == fail0, "one S retry, no terminal failure");
	if (b)
		b->Release();
	return ok;
}

// arm 20: DISCARD under pressure - the worker held behind an unrelated setter (a queued command may still bind the current slice
// as far as the front knows), the slice allocator failing once: the DISCARD still SUCCEEDS - drained, today's lock body reuses
// the GPU-idle slice. X-cs-discard-noretry: the failed attempt answers INVALIDCALL
bool caseCsDiscardPressure(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	const uint64_t slices0 = e.nd->resources().stats().slicesCreated;
	e.nd->debug().allocFailSite = "slice";   // before the hold: debug() drains
	e.nd->debug().allocFailCount = 1;
	const uint64_t syncs0 = e.nd->csSyncCount();   // after the test's own accessor drains (X': each counts as an S)
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);
	BYTE* p = nullptr;
	HRESULT hr = E_FAIL;
	bool blocked = false, releasedFirst = false;
	csarm::blockedCall(e, [&] { hr = vb->Lock(0, 0, &p, D3DLOCK_DISCARD); }, &blocked, &releasedFirst);
	if (hr == D3D_OK)
		vb->Unlock();
	const uint64_t syncs = e.nd->csSyncCount() - syncs0;   // before the next accessor drain
	e.nd->debug().allocFailCount = 0;
	const uint64_t slices = e.nd->resources().stats().slicesCreated - slices0;
	*d += "hr " + std::to_string(long(hr)) + ", new slices " + std::to_string(slices) + ", S +" + std::to_string(syncs);
	CHECK(hr == D3D_OK && p, "the DISCARD succeeds");
	CHECK(slices == 0 && syncs == 1, "retried as S, today's body reused the idle slice");
	vb->Release();
	return ok;
}

// arm 23: extra-slice pressure - a lagging DISCARD gave a 4 MiB dynamic buffer a second slice; a 4 MiB creation under a budget that
// fits only once that idle extra slice is gone: the S retry reclaims it and succeeds. X-cs-no-reclaim: INVALIDCALL
bool caseCsReclaim(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DTexture8* t = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb)) && vb,
		"4 MiB dynamic VB");
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 2);
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "a DISCARD while the worker lags (a second slice)");
	vb->Unlock();
	csarm::release(e);
	e.nd->csSync();
	const uint64_t extra = e.nd->resources().dynSliceBytes.load();
	const GxNativeStats s0 = e.nd->stats();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + (2u << 20);   // the texture fits only without the idle extra slice
	const HRESULT hr = e.dev->CreateTexture(1024, 1024, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t);
	e.nd->debug().allocBudget = 0;
	const GxNativeStats s1 = e.nd->stats();
	*d += "slice bytes " + std::to_string(extra) + " -> " + std::to_string(e.nd->resources().dynSliceBytes.load()) + ", hr " +
		std::to_string(long(hr)) + ", refused +" + std::to_string(s1.createsRefused - s0.createsRefused);
	CHECK(extra >= (8u << 20), "the lagging DISCARD made a second 4 MiB slice");
	CHECK(hr == D3D_OK && t && s1.createsRefused == s0.createsRefused, "the creation succeeds after the reclaim");
	if (t)
		t->Release();
	vb->Release();
	return ok;
}

// arm 27: every site class fails its FIRST attempt once - the recovery ran (per site) before today's body and the call's result is
// the clean run's; a second injected failure after the recovery -> today's failure body (INVALIDCALL, refused +1, alloc_failures
// +1, one line). X-cs-recovery-bypass: CreateTexture's failure skips the recovery
bool caseCsSiteClasses(Env& e, std::string* d)
{
	bool ok = true;
	auto inject = [&](const char* site, uint32_t n) {
		e.nd->debug().allocFailSite = site;
		e.nd->debug().allocFailCount = n;
	};
	struct Front { const char* entry; const char* site; std::function<HRESULT(IUnknown**)> make; };
	const Front classes[] = {
		{ "CreateTexture", "image", [&](IUnknown** o) { return e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, reinterpret_cast<IDirect3DTexture8**>(o)); } },
		{ "CreateVertexBuffer", "buffer", [&](IUnknown** o) { return e.dev->CreateVertexBuffer(4096, 0, D3DFVF_XYZ, D3DPOOL_MANAGED, reinterpret_cast<IDirect3DVertexBuffer8**>(o)); } },
		{ "CreateRenderTarget", "image", [&](IUnknown** o) { return e.dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, reinterpret_cast<IDirect3DSurface8**>(o)); } },
		{ "CreateDepthStencilSurface", "image", [&](IUnknown** o) { return e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, reinterpret_cast<IDirect3DSurface8**>(o)); } },
	};
	for (const Front& c : classes) {
		const uint64_t r0 = e.nd->recoveriesAt(c.entry);
		IUnknown* o = nullptr;
		inject(c.site, 1);
		const HRESULT hr = c.make(&o);
		inject(nullptr, 0);
		const uint64_t r = e.nd->recoveriesAt(c.entry) - r0;
		*d += std::string(c.entry) + " hr " + std::to_string(long(hr)) + " recovered " + std::to_string(r) + "; ";
		CHECK(hr == D3D_OK && o && r == 1, (std::string(c.entry) + ": recovered once, then today's body succeeded").c_str());
		if (o)
			o->Release();
	}
	{   // the DISCARD slice (a front attempt behind a lagging worker)
		IDirect3DVertexBuffer8* vb = nullptr;
		e.dev->CreateVertexBuffer(4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb);
		const uint64_t r0 = e.nd->recoveriesAt("slice");
		inject("slice", 1);   // before the hold: debug() drains
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 3);
		BYTE* p = nullptr;
		HRESULT hr = E_FAIL;
		bool blocked = false, releasedFirst = false;
		csarm::blockedCall(e, [&] { hr = vb ? vb->Lock(0, 0, &p, D3DLOCK_DISCARD) : E_FAIL; }, &blocked, &releasedFirst);
		inject(nullptr, 0);
		if (hr == D3D_OK)
			vb->Unlock();
		const uint64_t r = e.nd->recoveriesAt("slice") - r0;
		*d += "slice hr " + std::to_string(long(hr)) + " recovered " + std::to_string(r) + "; ";
		CHECK(hr == D3D_OK && r == 1, "DISCARD slice: recovered once, then today's body");
		if (vb)
			vb->Release();
	}
	{   // the read-back buffer, first met inside an S body (a GPU read of the back buffer)
		IDirect3DSurface8* bb = nullptr;
		e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
		clearSurface(e.dev, bb, 0xff336699);
		const uint64_t r0 = e.nd->recoveriesAt("readback");
		inject("readback", 1);
		uint32_t px = 0;
		const bool read = bb && readPixel(e.dev, bb, 3, 3, &px);
		inject(nullptr, 0);
		const uint64_t r = e.nd->recoveriesAt("readback") - r0;
		*d += "readback " + std::string(read ? "ok" : "failed") + " recovered " + std::to_string(r) + "; ";
		CHECK(read && (px & 0xffffff) == 0x336699 && r == 1, "read-back: recovered once inside the S body, then the read");
		if (bb)
			bb->Release();
	}
	{   // two failures: the second one after the recovery takes today's failure body exactly
		const GxNativeStats s0 = e.nd->stats();
		const uint64_t f0 = e.nd->allocFailures();
		IDirect3DTexture8* t = nullptr;
		inject("image", 2);
		const HRESULT hr = e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
		inject(nullptr, 0);
		const GxNativeStats s1 = e.nd->stats();
		*d += "twice: hr " + std::to_string(long(hr)) + " refused +" + std::to_string(s1.createsRefused - s0.createsRefused) +
			" alloc_failures +" + std::to_string(e.nd->allocFailures() - f0);
		CHECK(hr == D3DERR_INVALIDCALL && !t && s1.createsRefused - s0.createsRefused == 1 && e.nd->allocFailures() - f0 == 1,
			"today's failure body, counted once, the terminal failure counted once");
	}
	return ok;
}

// arm 30: a failed creation reaches the GAME as today - with no memory left (a TEST budget at the current use) CreateTexture,
// CreateVertexBuffer and CreateRenderTarget answer INVALIDCALL with *pp null, creates and creates_refused +1 each, one REFUSED line
// each - in BOTH modes. X-cs-create-defer: the front answers D3D_OK and leaves the failure for later
bool caseCsCreateFails(Env& e, std::string* d)
{
	bool ok = true;
	const GxNativeStats s0 = e.nd->stats();
	size_t lines0 = 0;
	for (const std::string& l : logSnapshot())
		lines0 += l.compare(0, 26, "[GXNATIVE] REFUSED create:") == 0;
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + 1;
	IDirect3DTexture8* t = reinterpret_cast<IDirect3DTexture8*>(uintptr_t(1));
	IDirect3DVertexBuffer8* v = reinterpret_cast<IDirect3DVertexBuffer8*>(uintptr_t(1));
	IDirect3DSurface8* r = reinterpret_cast<IDirect3DSurface8*>(uintptr_t(1));
	const HRESULT ht = e.dev->CreateTexture(256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t);
	const HRESULT hv = e.dev->CreateVertexBuffer(1u << 20, 0, D3DFVF_XYZ, D3DPOOL_MANAGED, &v);
	const HRESULT hrt = e.dev->CreateRenderTarget(256, 256, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &r);
	e.nd->debug().allocBudget = 0;
	const GxNativeStats s1 = e.nd->stats();
	size_t lines = 0;
	for (const std::string& l : logSnapshot())
		lines += l.compare(0, 26, "[GXNATIVE] REFUSED create:") == 0;
	*d += "hr " + std::to_string(long(ht)) + "/" + std::to_string(long(hv)) + "/" + std::to_string(long(hrt)) + ", creates +" +
		std::to_string(s1.creates - s0.creates) + ", refused +" + std::to_string(s1.createsRefused - s0.createsRefused) + ", lines +" +
		std::to_string(lines - lines0);
	CHECK(ht == D3DERR_INVALIDCALL && hv == D3DERR_INVALIDCALL && hrt == D3DERR_INVALIDCALL, "INVALIDCALL at the call");
	CHECK(!t && !v && !r, "*pp null");
	CHECK(s1.creates - s0.creates == 3 && s1.createsRefused - s0.createsRefused == 3 && lines - lines0 == 3, "counted and refused as today");
	return ok;
}

// arm 31: allocations first met inside an S body. (a) a lagging DISCARD's idle extra slice, then a dropped MANAGED texture's LockRect
// (S restore) under a budget that fits the read-back buffer only without that slice: the S body's first failure recovers (the
// slice reclaimed) and the restore succeeds with the right texels. X-cs-sbody-norecover: INVALIDCALL. (b) terminal: a budget nothing
// can satisfy - LockRect answers INVALIDCALL as today, alloc_failures +1 (bad > 0), one line. X-cs-terminal-silent: not counted
bool caseCsSbodyAlloc(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8* t = nullptr;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "MANAGED 512x512");
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb)) && vb, "VB");
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)), "fill");
	for (int y = 0; y < 512; ++y)
		for (int x = 0; x < 512; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff000000u | uint32_t(x * 131 + y);
	t->UnlockRect(0);
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present (the host copy drops)");
	SETUP(e.nd->stats().dropped >= 1, "the MANAGED host copy was dropped");
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 4);
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "a lagging DISCARD (the extra slice)");
	vb->Unlock();
	csarm::release(e);
	e.nd->csSync();
	const uint64_t f0 = e.nd->allocFailures();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + (512u << 10);   // the 1 MiB read-back fits only without the idle 4 MiB slice
	const HRESULT ha = t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
	e.nd->debug().allocBudget = 0;
	bool right = false;
	if (ha == D3D_OK) {
		right = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + 7 * lr.Pitch)[9] == (0xff000000u | uint32_t(9 * 131 + 7));
		t->UnlockRect(0);
	}
	*d += "(a) hr " + std::to_string(long(ha)) + (right ? " texels right" : " texels WRONG");
	CHECK(ha == D3D_OK && right, "(a) the restore succeeds after the S body's recovery");
	// (b) terminal: a second texture (the first is pinned now - a written level locked again), dropped, then nothing fits
	IDirect3DTexture8* t2 = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t2)) && t2, "a second MANAGED texture");
	SETUP(SUCCEEDED(t2->LockRect(0, &lr, nullptr, 0)), "fill 2");
	t2->UnlockRect(0);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	const uint64_t f1 = e.nd->allocFailures();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + 1;
	const HRESULT hb = t2->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
	e.nd->debug().allocBudget = 0;
	if (hb == D3D_OK)
		t2->UnlockRect(0);
	const uint64_t f = e.nd->allocFailures() - f1;
	*d += ", (b) hr " + std::to_string(long(hb)) + " alloc_failures +" + std::to_string(f) + " (a) +" + std::to_string(f1 - f0);
	CHECK(hb == D3DERR_INVALIDCALL && f == 1, "(b) INVALIDCALL as today, the terminal failure counted once");
	t2->Release();
	vb->Release();
	t->Release();
	return ok;
}

// arm 24: the FRONT's FlushIdle emission. An S body (UpdateTexture failing on its last mip, TEST knob) leaves > 32 MiB staged with no
// frame open; the next F unlock (a dynamic buffer) must flush at ITS call position, as direct does (upload_flushes +1 there, none
// at the SYSTEMMEM unlock after it); --strict compares the EXEC digests. X-cs-flush-dropped: no flush (CS=1 only)
bool caseCsFlushFront(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DTexture8 *src = nullptr, *dst = nullptr;
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DSurface8* img = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(3072, 3072, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src)) && src, "SYSTEMMEM 3072^2 x2 mips");
	SETUP(SUCCEEDED(e.dev->CreateTexture(3072, 3072, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst)) && dst, "DEFAULT 3072^2 x2 mips");
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &img)) && img, "image surface");
	e.nd->debug().updateFailLastMip = true;
	const HRESULT hu = e.dev->UpdateTexture(src, dst);   // S: mip 0 staged (36 MiB), mip 1 fails - no flush at its end
	e.nd->debug().updateFailLastMip = false;
	const uint64_t pressure = e.nd->resources().stagingPressure(), f0 = e.nd->stats().uploadFlushes;
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_NOOVERWRITE)) && p, "lock the dynamic VB");
	vb->Unlock();   // F: the flush belongs here
	const uint64_t f1 = e.nd->stats().uploadFlushes;
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(img->LockRect(&lr, nullptr, 0)), "lock the image surface");
	img->UnlockRect();
	const uint64_t f2 = e.nd->stats().uploadFlushes;
	*d += "UpdateTexture hr " + std::to_string(long(hu)) + ", pressure " + std::to_string(pressure >> 20) + " MiB, flushes at the VB unlock +" +
		std::to_string(f1 - f0) + ", at the image unlock +" + std::to_string(f2 - f1);
	CHECK(hu == D3DERR_INVALIDCALL && pressure > (32ull << 20), "the S body left > 32 MiB staged, no frame open");
	CHECK(f1 - f0 == 1 && f2 == f1, "one flush, at the first F unlock");
	img->Release();
	vb->Release();
	dst->Release();
	src->Release();
	return ok;
}

// arm 24b: the WORKER path of A26's trigger - 24 MANAGED create / 4 MiB lock / unlock / release cycles with no frame open: every
// unlock is A (its exec half stages and flushes on the worker); the flushes fall exactly where A26's model puts them, no dedicated
// overflow, in both modes; --strict compares the EXEC digests. X-cs-exec-noflush (CS=1 only): the worker's upload drops its flush
bool caseCsFlushWorker(Env& e, std::string* d)
{
	GXRender::Res::GxResources& gx0 = e.nd->resources();
	const GXRender::Vk::VkUploadRing::Stats s0 = gx0.ring().stats();
	const uint64_t f0 = e.nd->stats().uploadFlushes, err0 = e.nd->stats().resourceErrors;
	const uint64_t kBytes = 4ull << 20, kThreshold = 32ull << 20;
	uint64_t model = e.nd->resources().stagingPressure();
	std::string want, got;
	bool setup = true, tracks = true;
	for (int i = 1; i <= 24 && setup; ++i) {
		const uint64_t staged0 = e.nd->resources().stats().stagingBytes, fl0 = e.nd->stats().uploadFlushes;
		IDirect3DVertexBuffer8* vb = nullptr;
		BYTE* p = nullptr;
		setup = SUCCEEDED(e.dev->CreateVertexBuffer(UINT(kBytes), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb)) &&
			SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p;
		if (p) {
			std::memset(p, i, size_t(kBytes));
			vb->Unlock();
		}
		if (vb)
			vb->Release();
		GXRender::Res::GxResources& gx = e.nd->resources();   // drains: the cycle's exec halves ran
		setup = setup && gx.stats().stagingBytes == staged0 + kBytes;
		model += kBytes;
		if (model > kThreshold) {
			want += " " + std::to_string(i);
			model = 0;
		}
		for (uint64_t k = e.nd->stats().uploadFlushes - fl0; k > 0; --k)
			got += " " + std::to_string(i);
		if (gx.stagingPressure() != model)
			tracks = false;
	}
	setup = setup && e.nd->stats().resourceErrors == err0;
	const GXRender::Vk::VkUploadRing::Stats s1 = e.nd->resources().ring().stats();
	const uint64_t overflow = s1.dedicated - s0.dedicated;
	*d += std::string("setup ") + (setup ? "ok" : "FAILED") + "; flushes at cycles" + (got.empty() ? " none" : got) + " (model" + want + ")" +
		(tracks ? "" : ", pressure OFF the model") + "; dedicated " + std::to_string(overflow) + "; flushes +" +
		std::to_string(e.nd->stats().uploadFlushes - f0);
	return setup && got == want && tracks && overflow == 0;
}

// arm 8: lifetime - a draw sampling T queued (worker held), SetTexture(0, null), Release(T): T is dead on the front, its Destroy goes
// out with the next Present behind the draw; after the release the frame shows T's texels, validation clean. (X-cs-destroy-now -
// the storage destroyed on the releasing thread - is a use-after-free: not run in-process.)
bool caseCsLifetime(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::TV;
	IDirect3DTexture8* tex = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "texture");
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)), "fill");
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff13579bu;
	tex->UnlockRect(0);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	e.dev->SetTexture(0, tex);
	e.dev->BeginScene();
	csarm::hold(e);
	const TV q[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 0, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
	e.dev->SetTexture(0, nullptr);
	CHECK(tex->Release() == 0, "the game's last reference");
	e.dev->EndScene();
	csarm::release(e);
	const uint32_t a = csarm::px(e, bb, 30, 30);
	*d += "pixel " + csarm::hex(a);
	CHECK(a == 0x13579b, "the queued draw sampled the released texture's texels");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present (the Destroy goes out behind the draw)");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	bb->Release();
	return ok;
}

// arm 10: the drawable size is read on the CALLER (SDL wants main) - a test host callback counts every call made off the main
// thread: 0 after frames with the worker on. X-cs-worker-drawable: the worker calls it
bool caseCsDrawableThread(Env& e, std::string* d)
{
	bool ok = true;
	const int off0 = g_drawableOffMain.load();
	for (int i = 0; i < 6; ++i)
		CHECK(presentClear(e, 0xff204060), "frames");
	e.nd->csSync();
	const int off = g_drawableOffMain.load() - off0;
	*d += "drawable-size calls off the main thread: " + std::to_string(off);
	CHECK(off == 0, "never off the caller's thread");
	return ok;
}

// arm 11: Reset while the worker holds a recorded frame on the back buffer - Reset drains first (the caller blocks until the
// release), validation clean, no worker state touched by the front outside an S body. X-cs-reset-nodrain
// C · renderer-faster-2 15/10/2026 (lead C38): the PLANTED form runs in a child. Without the drain, the held draw runs on the worker
// against the back buffer Reset already replaced once the hold is released - a race the plant itself creates, which crashed the Windows
// suite in-process once in 12 runs (0xC0000005 after "vkCmdPipelineBarrier2 image VK_NULL_HANDLE", dlss-fg-3) and took every later arm
// with it. The child prints its verdict BEFORE it releases the worker; the parent credits the plant only for that verdict.
bool csResetDrainPlantedParent(Env& e, std::string* d);   // after runSelf (below)
bool caseCsResetDrain(Env& e, std::string* d)
{
	bool ok = true;
	if (e.nd->debug().csResetNoDrain)
		return csResetDrainPlantedParent(e, d);
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	bb->Release();
	csarm::ffDiffuse(e);
	const uint64_t own0 = e.nd->csOwnerViolations();
	csarm::hold(e);
	QV q[4];
	csarm::quad(q, 0, 0, 64, 64, 0xff446688);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	HRESULT hr = E_FAIL;
	bool blocked = false, releasedFirst = false;
	csarm::blockedCall(e, [&] { hr = e.dev->Reset(&pp); }, &blocked, &releasedFirst);
	const uint64_t own = e.nd->csOwnerViolations() - own0;
	*d += "Reset hr " + std::to_string(long(hr)) + ", drained first: " + (blocked && releasedFirst ? "yes" : "no") + ", ownership +" + std::to_string(own);
	CHECK(hr == D3D_OK, "Reset succeeds");
	CHECK(blocked && releasedFirst && own == 0, "Reset waited for the worker, touched nothing of it before");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}
// the planted child (X-child-cs-reset-drain, by name only): arm 11's sequence with the plant on; the verdict line is printed while the
// worker is still held, then the hold is released (the stale draw may then crash this process - that is the plant's race, contained here)
bool csResetDrainPlantedChild(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	bb->Release();
	csarm::ffDiffuse(e);
	const uint64_t own0 = e.nd->csOwnerViolations();
	csarm::hold(e);
	QV q[4];
	csarm::quad(q, 0, 0, 64, 64, 0xff446688);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1);
	GxNativeCs& s = e.nd->csStream();
	std::atomic<bool> released { false }, printed { false };
	// the release comes only AFTER the verdict is out (the plant: Reset returned with the worker held) or when Reset really waits for
	// the worker (a plant that changed nothing: the drain blocks until this release) - never on a timer that can beat the verdict
	// 30 s is the backstop for neither
	std::thread helper([&] {
		const auto t0 = std::chrono::steady_clock::now();
		while (!printed.load() && !(s.testHeld() && s.testProducerWaiting()) &&
			std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		released = true;
		s.testRelease();
	});
	const HRESULT hr = e.dev->Reset(&pp);
	const bool releasedFirst = released.load();
	const uint64_t own = e.nd->csOwnerViolations() - own0;
	std::fprintf(stderr, "[TEST] RESET-DRAIN hr=%ld released_first=%d own=%llu end\n", long(hr), releasedFirst ? 1 : 0, (unsigned long long)own);
	std::fflush(stderr);
	printed = true;
	helper.join();
	*d += "verdict printed before the release";
	e.devReleased = true;   // leaked: the process ends with the plant's stale draw wherever it got to
	e.keepWindow = true;
	return ok;
}
bool caseChildCsResetDrain(Env& e, std::string* d) { return csResetDrainPlantedChild(e, d); }

// arm 13: an acquire answering SURFACE_LOST on the worker - the frame is not shown (<= 2), the caller's next Present recreates as
// S, and no surface / swapchain work ever ran on the worker (its counter 0). X-cs-recreate-on-worker
bool caseCsWsiOwner(Env& e, std::string* d)
{
	bool ok = true;
	CHECK(presentClear(e, 0xff204060), "a first frame");
	e.nd->debug().injectSurfaceLost = true;
	for (int i = 0; i < 4; ++i)
		CHECK(presentClear(e, 0xff204060), "frames");
	e.nd->csSync();
	const uint64_t wsi = e.nd->csWsiOnWorker();
	*d += "surfaces " + std::to_string(e.nd->stats().surfacesRecreated) + ", skipped " + std::to_string(e.nd->stats().presentsSkipped) +
		", WSI on the worker " + std::to_string(wsi);
	CHECK(e.nd->stats().surfacesRecreated == 1 && e.nd->stats().presentsSkipped <= 2, "recovered by the caller, <= 2 unshown");
	CHECK(wsi == 0, "no surface / swapchain creation on the worker");
	return ok;
}

// arm 15 (64 KiB TEST ring, kMaxCmd 16 KiB): a static VB unlock and a MANAGED texture unlock larger than kMaxCmd go S (counted,
// decided before any bookkeeping - each lock opened once) and draw the right pixels
bool caseCsOversize(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	using csarm::TV;
	SETUP(e.nd->csStream().capacity() == (64u << 10), "the 64 KiB TEST ring");
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DTexture8* tex = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(32u << 10, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "32 KiB static VB");
	SETUP(SUCCEEDED(e.dev->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "64 KiB MANAGED texture");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	const uint64_t s0 = e.nd->csSyncCount();
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "lock the VB whole");
	std::memset(p, 0, 32u << 10);
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 32, 64, 0xff2a6b9c);
	vb->Unlock();
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)), "lock the texture whole");
	for (int y = 0; y < 128; ++y)
		for (int x = 0; x < 128; ++x)
			reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xff9c6b2au;
	tex->UnlockRect(0);
	const uint64_t syncs = e.nd->csSyncCount() - s0;
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->SetStreamSource(0, nullptr, 0);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	e.dev->SetTexture(0, tex);
	const TV q[4] = { { 32, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 32, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
	e.dev->SetTexture(0, nullptr);
	e.dev->EndScene();
	const uint32_t a = csarm::px(e, bb, 10, 30), b = csarm::px(e, bb, 50, 30);
	*d += "S +" + std::to_string(syncs) + ", left " + csarm::hex(a) + " right " + csarm::hex(b);
	CHECK(syncs == 2, "both oversize unlocks went S");
	CHECK(a == 0x2a6b9c && b == 0x9c6b2a, "both uploads drawn");
	bb->Release();
	tex->Release();
	vb->Release();
	return ok;
}

// A host-signalled timeline semaphore that holds a frame on the GPU (lever 3's gx_vkctx_test HostGate, trimmed): the device's next
// offscreen frame waits on it (GxNativeDebug::testGateSem); opened by a thread once a driver wait blocks, or explicitly
struct CsHostGate
{
	GXRender::Vk::VkCtx* ctx = nullptr;
	VkSemaphore sem = VK_NULL_HANDLE;
	std::thread thread;
	std::atomic<bool> opened { false };
	bool create(GXRender::Vk::VkCtx& c)
	{
		ctx = &c;
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		si.pNext = &type;
		return vkCreateSemaphore(c.device(), &si, nullptr, &sem) == VK_SUCCESS;
	}
	void open()
	{
		if (opened.exchange(true))
			return;
		VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
		info.semaphore = sem;
		info.value = 1;
		vkSignalSemaphore(ctx->device(), &info);
	}
	// opens once some thread blocks in one of ctx's driver waits (the recovery reached its REAL wait), or after `ms`
	void openWhenBlocked(int ms)
	{
		thread = std::thread([this, ms] {
			for (int i = 0; i < ms && ctx->testBlockedWaits() == 0 && !opened.load(); ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			open();
		});
	}
	void destroy()
	{
		open();
		if (thread.joinable())
			thread.join();
		ctx->waitIdle();
		vkDestroySemaphore(ctx->device(), sem, nullptr);
	}
};

// arm 2g: the completed() half of DISCARD reuse - frame 1's draw from slice S1 is held ON THE GPU behind a host gate, the worker has
// caught up; a DISCARD must take a new slice (S1's last use has not completed) - after the gate opens frame 1 shows its own quad.
// X-cs-discard-gpu (GXRES: reuse ignores completed()): the DISCARD rewrites S1 under the held draw - frame 1 shows the new bytes
bool caseCsDiscardGpu(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	CsHostGate gate;
	SETUP(gate.create(e.nd->ctx()), "host gate");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "DISCARD 1");
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, 0xff31c4a5);
	vb->Unlock();
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.nd->debug().testGateSem = gate.sem;
	e.nd->debug().testGateValue = 1;
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present frame 1 (held on the GPU)");
	e.nd->csSync();   // the worker caught up: only completed() can still say no
	p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "DISCARD 2 while frame 1 is held");
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, 0xffa5c431);
	vb->Unlock();
	gate.open();
	e.dev->SetStreamSource(0, nullptr, 0);
	const uint32_t a = csarm::px(e, bb, 30, 30);
	*d += "frame 1 shows " + csarm::hex(a);
	CHECK(a == 0x31c4a5, "the held frame drew its own bytes");
	gate.destroy();
	bb->Release();
	vb->Release();
	return ok;
}

// arm 23b: as arm 23, but the frame that last read the extra slice is held ON THE GPU (host gate, opened when the recovery's wait
// blocks): the S retry WAITS for submitted work, then reclaims and succeeds. X-cs-reclaim-nowait: no wait - the busy slice stays,
// INVALIDCALL
bool caseCsReclaimBusy(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DTexture8* t = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "4 MiB dynamic VB");
	CsHostGate gate;
	SETUP(gate.create(e.nd->ctx()), "host gate");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 4 * sizeof(QV), &p, D3DLOCK_DISCARD)) && p, "DISCARD 1");
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, 0xff8040c0);
	vb->Unlock();
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	e.nd->debug().testGateSem = gate.sem;
	e.nd->debug().testGateValue = 1;
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "Present (held on the GPU)");
	e.nd->csSync();
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "DISCARD 2: a second slice (the first is GPU-busy)");
	vb->Unlock();
	e.nd->csSync();
	const uint64_t extra = e.nd->resources().dynSliceBytes.load();
	const GxNativeStats s0 = e.nd->stats();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + (2u << 20);
	gate.openWhenBlocked(3000);
	const HRESULT hr = e.dev->CreateTexture(1024, 1024, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t);
	e.nd->debug().allocBudget = 0;
	gate.open();
	const GxNativeStats s1 = e.nd->stats();
	*d += "slice bytes " + std::to_string(extra) + " -> " + std::to_string(e.nd->resources().dynSliceBytes.load()) + ", hr " + std::to_string(long(hr)) +
		", refused +" + std::to_string(s1.createsRefused - s0.createsRefused);
	CHECK(extra >= (8u << 20), "two 4 MiB slices before the creation");
	CHECK(hr == D3D_OK && t && s1.createsRefused == s0.createsRefused, "the recovery waited for the held frame, reclaimed, succeeded");
	if (t)
		t->Release();
	gate.destroy();
	bb->Release();
	vb->Release();
	return ok;
}

namespace csarm {
// a 12 KiB MANAGED VB whose unlock is an A command (12 KiB < kMaxCmd) that must stage through a DEDICATED buffer: the TEST staging
// ring is 8 KiB (GX_NATIVE_STAGING_KIB=8)
struct RecoverWaits
{
	std::mutex m;
	std::vector<std::pair<uint64_t, uint64_t>> v;   // (value waited for, lastSubmitted then)
	static void hook(void* u, uint64_t value, uint64_t last)
	{
		RecoverWaits* r = static_cast<RecoverWaits*>(u);
		std::lock_guard<std::mutex> g(r->m);
		r->v.push_back({ value, last });
	}
};
} // namespace csarm

// arm 28a / 28b: recovery INSIDE a worker command (an upload exec half whose dedicated staging fails once), with the front blocked
// on the LEAD fence (a) / on ROOM in a full 64 KiB stream (b): the worker's recovery completes, the front unblocks, the upload lands;
// every value the recovery waited for was already submitted (none the open frame's). X-cs-worker-selfdrain (by name: the recovery
// drains its own stream - a real cycle; the arm watchdog ends the process)
bool caseCsWorkerRecover(Env& e, std::string* d, bool room)
{
	bool ok = true;
	using csarm::QV;
	SETUP(e.nd->resources().ring().capacity() <= (8u << 10), "the 8 KiB TEST staging ring");
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(12u << 10, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "12 KiB static VB");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame (something submitted)");
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	csarm::RecoverWaits waits;
	e.nd->testRecoverWait = &csarm::RecoverWaits::hook;
	e.nd->testRecoverWaitUser = &waits;
	e.nd->debug().allocFailSite = "upload-dedicated";
	e.nd->debug().allocFailCount = 1;
	const uint64_t r0 = e.nd->recoveriesAt("upload-dedicated"), f0 = e.nd->allocFailures();
	csarm::hold(e);
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "lock the VB");
	std::memset(p, 0, 12u << 10);
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, 0xff6699cc);
	vb->Unlock();   // A: the upload, held
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	bool blocked = false, releasedFirst = false;
	int bad = 0;
	if (room)
		csarm::blockedCall(e, [&] {
			for (DWORD i = 0; i < 4000; ++i)
				bad += e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, i) != D3D_OK;
		}, &blocked, &releasedFirst);
	else {
		bad += e.dev->Present(nullptr, nullptr, nullptr, nullptr) != D3D_OK;   // Present 1, queued behind the held upload
		csarm::blockedCall(e, [&] { bad += e.dev->Present(nullptr, nullptr, nullptr, nullptr) != D3D_OK; }, &blocked, &releasedFirst);
	}
	e.nd->csSync();
	e.nd->testRecoverWait = nullptr;
	const uint64_t r = e.nd->recoveriesAt("upload-dedicated") - r0;
	bool waitsOk = !waits.v.empty();
	for (const auto& w : waits.v)
		waitsOk = waitsOk && w.first <= w.second;
	if (room)
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	const uint32_t px = csarm::px(e, bb, 30, 30);
	*d += std::string(room ? "(b) room" : "(a) lead") + ": front blocked " + (blocked ? "yes" : "no") + ", recovered " + std::to_string(r) +
		", waits " + std::to_string(waits.v.size()) + (waitsOk ? " all submitted" : " NOT all submitted") + ", alloc_failures +" +
		std::to_string(e.nd->allocFailures() - f0) + ", pixel " + csarm::hex(px);
	CHECK(bad == 0 && blocked && releasedFirst, "the front blocked, then every call answered as without the failure");
	CHECK(r == 1 && waitsOk && e.nd->allocFailures() == f0, "the worker recovered once, waiting only for submitted work");
	CHECK(px == 0x6699cc, "the upload landed");
	bb->Release();
	vb->Release();
	return ok;
}
bool caseCsWorkerRecoverLead(Env& e, std::string* d) { return caseCsWorkerRecover(e, d, false); }
bool caseCsWorkerRecoverRoom(Env& e, std::string* d) { return caseCsWorkerRecover(e, d, true); }

// arm 29 / 29b (ASYNC - shows divergence f = R27): the worker held before an upload that needs dedicated staging; its draw and
// Present 1 queued; then CreateTexture(U) takes the rest of a TEST budget on the front; release - the upload's staging fails on the
// worker AFTER its recovery: counted (resource_errors +1, alloc_failures +1), logged, no crash, validation clean, U usable. 29b: U
// released at once - its Destroy queues behind the failing upload, same outcome. X-cs-failure-silent: nothing counted
bool caseCsReorderedFailure(Env& e, std::string* d, bool releaseU)
{
	bool ok = true;
	using csarm::QV;
	SETUP(e.nd->resources().ring().capacity() <= (8u << 10), "the 8 KiB TEST staging ring");
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DTexture8* u = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(12u << 10, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "12 KiB static VB");
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a first frame");
	csarm::ffDiffuse(e);
	const GxNativeStats s0 = e.nd->stats();
	const uint64_t f0 = e.nd->allocFailures();
	e.nd->debug().allocBudget = csarm::vmaBytes(e) + (256u << 10);   // U (256 KiB) takes exactly what is left
	csarm::hold(e);
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "lock the VB");
	std::memset(p, 0, 12u << 10);
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, 0xff6699cc);
	vb->Unlock();
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	const HRESULT hu = e.dev->CreateTexture(256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &u);   // the front takes the budget now
	if (releaseU && u) {
		u->Release();
		u = nullptr;
	}
	csarm::release(e);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);   // 29b: U's Destroy goes out here, behind the failing upload
	e.nd->csSync();
	e.nd->debug().allocBudget = 0;
	const GxNativeStats s1 = e.nd->stats();
	const uint64_t rerr = s1.resourceErrors - s0.resourceErrors, af = e.nd->allocFailures() - f0;
	bool usable = true;
	if (u) {
		D3DSURFACE_DESC sd;
		usable = SUCCEEDED(u->GetLevelDesc(0, &sd)) && sd.Width == 256;
		u->Release();
	}
	*d += std::string(releaseU ? "(29b) " : "(29) ") + "U hr " + std::to_string(long(hu)) + ", resource_errors +" + std::to_string(rerr) +
		", alloc_failures +" + std::to_string(af) + (usable ? ", U usable" : ", U NOT usable");
	CHECK(hu == D3D_OK && usable, "U created on the front and usable");
	CHECK(rerr == 1 && af == 1, "the reordered failure is counted and logged, not silent");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	vb->Release();
	return ok;
}
bool caseCsReorderedFailureKeep(Env& e, std::string* d) { return caseCsReorderedFailure(e, d, false); }
bool caseCsReorderedFailureRelease(Env& e, std::string* d) { return caseCsReorderedFailure(e, d, true); }

// ------------------------------------------------------------------------------- A23-B child-process arms (22, 6w, 16)
// The arm's verdict is how a CHILD run of this binary ended: a fault (arms 22, 6w) or the process exit itself (arm 16) is the
// behaviour under test. The child runs a by-name arm (child-*; its X- form plants the defect), stderr to a temp file.
const char* g_self = nullptr;
Options g_childOpt;
#if !defined(_WIN32)
struct ChildRun
{
	bool spawned = false, exited = false, signaled = false, timedOut = false;
	int code = -1, sig = 0;
	std::string err;
	std::string keptLog;   // a child that died by a signal or timed out: its whole output, kept here (the evidence)
	std::vector<std::string> lines(const char* tag) const
	{
		std::vector<std::string> v;
		for (size_t p = 0, q; p < err.size(); p = q + 1) {
			q = err.find('\n', p);
			if (q == std::string::npos)
				q = err.size();
			if (err.compare(p, std::strlen(tag), tag) == 0)
				v.push_back(err.substr(p, q - p));
		}
		return v;
	}
	std::string how() const
	{
		return (!spawned ? "not spawned" : timedOut ? "timed out" : signaled ? "died of signal " + std::to_string(sig) :
			"exited " + std::to_string(code)) + (keptLog.empty() ? "" : " (output: " + keptLog + ")");
	}
};
// env: overrides; an empty value unsets the variable
ChildRun runSelf(const std::string& arm, const std::vector<std::pair<std::string, std::string>>& env, int timeoutS = 120,
	const std::vector<std::string>& extra = {})
{
	ChildRun o;
	char path[] = "/tmp/gx-native-child-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0)
		return o;
	std::vector<std::string> args = { g_self, "--arm", arm };
	args.insert(args.end(), extra.begin(), extra.end());
	if (!g_childOpt.validation)
		args.push_back("--no-validation");
	if (g_childOpt.device) {
		args.push_back("--device");
		args.push_back(g_childOpt.device);
	}
	if (g_childOpt.fif != 2) {   // merge of render/integration 120c574b3: a child arm runs at the parent's --fif (A30)
		args.push_back("--fif");
		args.push_back(std::to_string(g_childOpt.fif));
	}
	std::vector<char*> argv;
	for (std::string& s : args)
		argv.push_back(&s[0]);
	argv.push_back(nullptr);
	std::vector<std::string> envs;
	for (char** p = environ; *p; ++p) {
		bool over = false;
		for (const auto& kv : env)
			over = over || (std::strncmp(*p, kv.first.c_str(), kv.first.size()) == 0 && (*p)[kv.first.size()] == '=');
		if (!over)
			envs.push_back(*p);
	}
	for (const auto& kv : env)
		if (!kv.second.empty())
			envs.push_back(kv.first + "=" + kv.second);
	std::vector<char*> envp;
	for (std::string& s : envs)
		envp.push_back(&s[0]);
	envp.push_back(nullptr);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fd, 1);   // the child's ARM / SUMMARY lines (stdout) and its logs (stderr), one file
	posix_spawn_file_actions_adddup2(&fa, fd, 2);
	posix_spawnattr_t sa;
	posix_spawnattr_init(&sa);
#if defined(__APPLE__)
	// a planted (X-) child dies on purpose: no exception port, so ReportCrash never sees it - no "quit unexpectedly" dialog on the
	// owner's desk, no .ips (lead C43 17/10). The kernel still delivers the signal: waitpid below reads it as before. Clean children
	// keep the system's ports, so an unexpected crash still leaves its report.
	if (arm.compare(0, 2, "X-") == 0)
		posix_spawnattr_setexceptionports_np(&sa, EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_CRASH |
			EXC_MASK_CORPSE_NOTIFY, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
#endif
	pid_t pid = 0;
	o.spawned = posix_spawn(&pid, g_self, &fa, &sa, argv.data(), envp.data()) == 0;
	posix_spawnattr_destroy(&sa);
	posix_spawn_file_actions_destroy(&fa);
	if (o.spawned) {
		int status = 0;
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
		for (;;) {
			if (waitpid(pid, &status, WNOHANG) == pid)
				break;
			if (std::chrono::steady_clock::now() > end) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				o.timedOut = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		o.exited = WIFEXITED(status);
		o.code = o.exited ? WEXITSTATUS(status) : -1;
		o.signaled = WIFSIGNALED(status);
		o.sig = o.signaled ? WTERMSIG(status) : 0;
	}
	lseek(fd, 0, SEEK_SET);
	char buf[4096];
	for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;)
		o.err.append(buf, size_t(n));
	close(fd);
	if (o.signaled || o.timedOut) {   // keep the evidence of an abnormal end
		o.keptLog = std::string("/tmp/gx-child-") + arm + "-" + std::to_string(int(pid)) + ".log";
		if (std::rename(path, o.keptLog.c_str()) != 0)
			o.keptLog.clear();
	}
	if (o.keptLog.empty())
		unlink(path);
	return o;
}
#endif
#if !defined(_WIN32)
std::string slurpAndUnlink(const char* path)
{
	std::string t;
	if (FILE* f = std::fopen(path, "rb")) {
		char b[4096];
		for (size_t n; (n = std::fread(b, 1, sizeof(b), f)) > 0;)
			t.append(b, n);
		std::fclose(f);
	}
	unlink(path);
	return t;
}
#endif
uint64_t fnv64(const void* p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < n; ++i) {
		h ^= static_cast<const uint8_t*>(p)[i];
		h *= 1099511628211ull;
	}
	return h;
}

// arm 22 (r3 P2-3) exact inputs. (a) SetTransform of an unmodelled type (D3DTS_WORLD1, 24) with a pointer to ONE readable byte
// before a PROT_NONE page: nothing read. In a child; X-cs-exact-transform (= X-cs-transform-copyall: 64 bytes read for every
// type) -> the child faults.
bool caseChildTransformEdge(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const size_t page = size_t(sysconf(_SC_PAGESIZE));
	uint8_t* base = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
	SETUP(base != MAP_FAILED && mprotect(base + page, page, PROT_NONE) == 0, "a page and a PROT_NONE guard page");
	uint8_t* one = base + page - 1;
	*one = 0x3f;
	const D3DMATRIX* m = reinterpret_cast<const D3DMATRIX*>(one);
	const HRESULT h1 = e.dev->SetTransform(D3DTRANSFORMSTATETYPE(257), m), h2 = e.dev->SetTransform(D3DTRANSFORMSTATETYPE(24), m);
	e.nd->csSync();
	std::fprintf(stderr, "[TEST] TRANSFORM-EDGE ok hr=%ld/%ld\n", long(h1), long(h2));
	*d += "no fault";
	CHECK(h1 == D3D_OK && h2 == D3D_OK, "an unmodelled type answers D3D_OK");
#else
	SETUP(false, "needs mmap");
#endif
	return ok;
}
bool caseCsExactTransform(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csTransformCopyAll;
	const ChildRun c = runSelf(planted ? "X-child-transform-edge" : "child-transform-edge", {});
	*d += "child " + c.how();
	CHECK(c.exited && c.code == 0 && c.lines("[TEST] TRANSFORM-EDGE ok").size() == 1, "an unmodelled SetTransform reads nothing");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// (b) Clear(0, non-null rects) clears nothing; Clear(0, null) clears the viewport. X-cs-clear-nullify -> the first one clears.
bool caseCsExactClear(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	const D3DRECT r = { 0, 0, 8, 8 };
	e.dev->Clear(0, &r, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
	const uint32_t a = csarm::px(e, bb, 30, 30), a0 = csarm::px(e, bb, 2, 2);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
	const uint32_t b = csarm::px(e, bb, 30, 30);
	*d += "Clear(0, rects) " + csarm::hex(a) + "/" + csarm::hex(a0) + ", Clear(0, null) " + csarm::hex(b);
	CHECK(a == 0 && a0 == 0, "Clear(0, non-null rects) clears nothing");
	CHECK(b == 0x00ff00, "Clear(0, null) clears the viewport");
	bb->Release();
	return ok;
}

// (c) an UP draw with stride 0 and a non-null pointer: the writer sees a non-null upVertices, as today (it refuses the 0-byte
// stream copy - that reason is the observable). X-cs-up-nullempty (the pointer sent as null) -> that refusal is missing.
bool caseCsExactUp0(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	auto count = [&] {
		const auto& w = e.nd->writer().stats().refusedWhy;
		const auto it = w.find("stream ring allocation failed (UP vertices)");
		return it == w.end() ? uint64_t(0) : it->second;
	};
	const uint64_t c0 = count();
	uint8_t buf[64] = {};
	e.dev->BeginScene();
	const HRESULT hr = e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, buf, 0);
	e.dev->EndScene();
	const uint64_t c1 = count();
	*d += "UP stride 0: hr " + std::to_string(long(hr)) + ", the writer's 0-byte copy refusals +" + std::to_string(c1 - c0);
	CHECK(hr == D3D_OK && c1 - c0 == 1, "the writer saw the non-null pointer (and refused its 0-byte copy)");
	bb->Release();
	return ok;
}

// arm 6w (integrator, R25's condition): an UP draw whose 32-bit size wraps - POINTLIST, (2^25 + 2) x 128 = 2^32 + 256 -> 256 bytes
// as today computes it. In the child the exec half reads its bytes from a copy that abuts a PROT_NONE page (csUpGuard) and the
// writer refuses the packet right after copying it (the GPU never fetches the out-of-range vertices). The child prints the exec
// entry and every stream copy; X-cs-up-shortcopy (one stride less copied) -> the writer's 256-byte read runs into the guard page.
bool caseChildUpWrap(Env& e, std::string* d)
{
	bool ok = true;
	struct UpSink final : GXRender::Dev::GxCsExecSink
	{
		void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg* a, size_t n, const GXRender::Dev::GxCsBytes* b) override
		{
			if (op != GXRender::Dev::GxCsOp::DrawUP || !b || n < 4)
				return;
			const size_t bytes = b->p ? size_t(b->rowBytes) * b->rows : 0;
			std::fprintf(stderr, "[TEST] UPEXEC t=%llu count=%llu stride=%llu present=%llu bytes=%zu fnv=%016llx\n",
				(unsigned long long)a[0].v, (unsigned long long)a[1].v, (unsigned long long)a[2].v, (unsigned long long)a[3].v, bytes,
				(unsigned long long)(b->p ? fnv64(b->p, bytes) : 0));
		}
		void destroyed(const void*) override {}
		void created(uint64_t, uint32_t, const GXRender::Dev::GxCsArg*, size_t) override {}
	};
	static UpSink sink;
	e.nd->debug().csUpGuard = true;
	GXRender::Rec::g_recTest.refuseUpAfterCopy = true;
	csarm::ffDiffuse(e);
	std::vector<uint8_t> v(256);
	for (size_t i = 0; i < v.size(); ++i)
		v[i] = uint8_t(i * 7 + 1);
	GXRender::Dev::g_csExecSink.store(&sink);
	e.dev->BeginScene();
	const HRESULT hr = e.dev->DrawPrimitiveUP(D3DPT_POINTLIST, (1u << 25) + 2, v.data(), 128);
	e.dev->EndScene();
	e.nd->csSync();
	GXRender::Dev::g_csExecSink.store(nullptr);
	const auto& w = e.nd->writer().stats().refusedWhy;
	const auto it = w.find("TEST: UP packet refused after its copy");
	const uint64_t refused = it == w.end() ? 0 : it->second;
	std::fprintf(stderr, "[TEST] UPDONE hr=%ld refused_after_copy=%llu\n", long(hr), (unsigned long long)refused);
	*d += "refused after its copy " + std::to_string(refused);
	CHECK(hr == D3D_OK && refused == 1, "the UP packet was copied, then refused by the TEST knob");
	return ok;
}
bool caseCsUpWrap(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csUpShortCopy;
	// the A22 recorded-stream tap (GX_CMDTAP) of each clean child, under the SERIAL schedule (the tap's recording thread includes
	// gx-native-cs; the two modes then record the same events in the same order)
	char tp0[] = "/tmp/gx-native-tap-XXXXXX", tp1[] = "/tmp/gx-native-tap-XXXXXX";
	const int f0 = mkstemp(tp0), f1 = mkstemp(tp1);
	SETUP(f0 >= 0 && f1 >= 0, "two temp files for the tap");
	close(f0);
	close(f1);
	const ChildRun c0 = runSelf("child-up-wrap", { { "GX_NATIVE_CS", "0" }, { "GX_CMDTAP", tp0 } }, 120, { "--serial" }),
		c1 = runSelf("child-up-wrap", { { "GX_NATIVE_CS", "1" }, { "GX_CMDTAP", tp1 } }, 120, { "--serial" });
	const std::string tap0 = slurpAndUnlink(tp0), tap1 = slurpAndUnlink(tp1);
	const bool cleanRan = c0.exited && c0.code == 0 && c1.exited && c1.code == 0 && c0.lines("[TEST] UPDONE").size() == 1 &&
		c1.lines("[TEST] UPDONE").size() == 1;
	const std::vector<std::string> x0 = c0.lines("[TEST] UPEXEC"), x1 = c1.lines("[TEST] UPEXEC");
	*d += "children cs0 " + c0.how() + ", cs1 " + c1.how() + (x0.empty() ? "" : "; " + x0[0].substr(7)) + "; tap " +
		(tap0.empty() ? "EMPTY" : tap0 == tap1 ? "equal" : "DIFFERS");
	const bool upBytes = x0.size() == 1 && x0[0].find(" bytes=256 ") != std::string::npos;
	if (planted) {
		SETUP(cleanRan && x0 == x1 && !tap0.empty() && tap0 == tap1 && upBytes, "the clean children ran equal");
		const ChildRun p = runSelf("X-child-up-wrap", { { "GX_NATIVE_CS", e.nd->csOn() ? "1" : "0" } });
		const std::vector<std::string> xp = p.lines("[TEST] UPEXEC");
		*d += "; planted child " + p.how() + (xp.empty() ? "" : ": " + xp[0].substr(7));
		CHECK(!p.signaled, "no fault at the guard page");
		CHECK(xp == (e.nd->csOn() ? x1 : x0), "the exec digest equal");
		return ok;
	}
	CHECK(cleanRan, "both children copied and refused the wrapped draw, no fault");
	CHECK(upBytes, "256 bytes (today's 32-bit size) reached the exec half");
	CHECK(x0 == x1, "the exec entry is equal with the worker on and off");
	CHECK(!tap0.empty() && tap0 == tap1, "the A22 tap (recorded stream + the writer's copies) is equal with the worker on and off");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// arm 16: exit with the worker MID-FRAME (child). The child queues a final frame whose submit gx-vk-submit holds (its present
// injected to fail as OUT_OF_HOST_MEMORY), opens the next frame on the worker and holds the worker before a draw, then leaks the
// device: the exit flush drains the worker (a helper releases it once the flush waits), lever 3 settles (quiesce: its helper releases
// the held submit once the flush blocks), the final line once - WITH the failed present - and after it no driver call (wrapped
// vk entry points, counted by thread name) and no log from gx-native-cs or gx-vk-submit. X-cs-exit-nosettle (the line before the
// settle) -> the failure missing from the line and/or a submission after it.
namespace exitobs {
std::atomic<bool> armed { false }, afterFinal { false };
std::atomic<uint64_t> calls[2], logs { 0 };
int watched()
{
#if !defined(_WIN32)
	char n[64] = {};
	pthread_getname_np(pthread_self(), n, sizeof(n));
	return std::strcmp(n, "gx-native-cs") == 0 ? 0 : std::strcmp(n, "gx-vk-submit") == 0 ? 1 : -1;
#else
	return -1;
#endif
}
void note()
{
	if (!afterFinal.load())
		return;
	const int w = watched();
	if (w >= 0)
		++calls[w];
}
#define GX_OBS(name, ret, params, args) \
	PFN_##name r_##name = nullptr; \
	ret VKAPI_CALL w_##name params { note(); return r_##name args; }
GX_OBS(vkQueueSubmit2, VkResult, (VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f), (q, n, s, f))
GX_OBS(vkQueueSubmit, VkResult, (VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f), (q, n, s, f))
GX_OBS(vkQueuePresentKHR, VkResult, (VkQueue q, const VkPresentInfoKHR* p), (q, p))
GX_OBS(vkAcquireNextImageKHR, VkResult, (VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore m, VkFence f, uint32_t* i), (d, s, t, m, f, i))
GX_OBS(vkBeginCommandBuffer, VkResult, (VkCommandBuffer c, const VkCommandBufferBeginInfo* b), (c, b))
GX_OBS(vkEndCommandBuffer, VkResult, (VkCommandBuffer c), (c))
GX_OBS(vkCmdDraw, void, (VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t x, uint32_t y), (c, a, b, x, y))
GX_OBS(vkCmdDrawIndexed, void, (VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t x, int32_t o, uint32_t y), (c, a, b, x, o, y))
GX_OBS(vkCmdBeginRendering, void, (VkCommandBuffer c, const VkRenderingInfo* r), (c, r))
GX_OBS(vkCmdCopyBuffer, void, (VkCommandBuffer c, VkBuffer s, VkBuffer t, uint32_t n, const VkBufferCopy* r), (c, s, t, n, r))
GX_OBS(vkWaitSemaphores, VkResult, (VkDevice d, const VkSemaphoreWaitInfo* w, uint64_t t), (d, w, t))
GX_OBS(vkGetSemaphoreCounterValue, VkResult, (VkDevice d, VkSemaphore s, uint64_t* v), (d, s, v))
#undef GX_OBS
void (*prevLoad)(VkDevice) = nullptr;
void onLoad(VkDevice dev)
{
	if (prevLoad)
		prevLoad(dev);
#define GX_SWAP(name) if (name && name != w_##name) { r_##name = name; name = w_##name; }
	GX_SWAP(vkQueueSubmit2) GX_SWAP(vkQueueSubmit) GX_SWAP(vkQueuePresentKHR) GX_SWAP(vkAcquireNextImageKHR)
	GX_SWAP(vkBeginCommandBuffer) GX_SWAP(vkEndCommandBuffer) GX_SWAP(vkCmdDraw) GX_SWAP(vkCmdDrawIndexed)
	GX_SWAP(vkCmdBeginRendering) GX_SWAP(vkCmdCopyBuffer) GX_SWAP(vkWaitSemaphores) GX_SWAP(vkGetSemaphoreCounterValue)
#undef GX_SWAP
}
void install()
{
	armed = true;
	prevLoad = GXRender::Vk::g_testAfterLoadDevice;
	GXRender::Vk::g_testAfterLoadDevice = onLoad;
}
void report()   // called by main AFTER the explicit flush (the child ends like the game); waits out lever 3's 3 s release fallback
{
	std::this_thread::sleep_for(std::chrono::seconds(4));
	std::fprintf(stderr, "[TEST] post-line: gx-native-cs=%llu gx-vk-submit=%llu logs=%llu\n", (unsigned long long)calls[0].load(),
		(unsigned long long)calls[1].load(), (unsigned long long)logs.load());
}
} // namespace exitobs

bool caseChildExitMidframe(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx = e.nd->ctx();
	SETUP(exitobs::armed.load(), "the observer is installed (main)");
	SETUP(ctx.submitThreaded(), "the submit thread is off");
	SETUP(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "two warm-up frames");
	e.nd->ctx().waitIdle();
	ctx.testSet([&](V::TestMutants& m) { m.presentResult = VK_ERROR_OUT_OF_HOST_MEMORY; });
	const uint64_t nextPresent = ctx.threadStats().presents + 1;
	ctx.testSet([&](V::TestMutants& m) { m.presentResultAt = nextPresent; });
	ctx.testHoldAt(ctx.queue(V::QueueKind::Graphics).timeline.lastSubmitted() + 1);
	SETUP(presentClear(e, 0xff204060), "the final frame");
	e.nd->csSync();   // the worker ran the final Present: its submit and present wait on gx-vk-submit's hold
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff102030, 1.0f, 0);   // the next frame, opened on the worker
	csarm::ffDiffuse(e);
	csarm::hold(e);
	csarm::QV q[4];
	csarm::quad(q, 0, 0, 32, 32, 0xff6699cc);
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));   // held: the worker is mid-frame when main returns
	static std::atomic<bool> sawBlocked { false }, released { false }, stop { false };
	std::thread(releaseWhenQuiescing, &ctx, &sawBlocked, &released, &stop, false).detach();
	GxNativeCs* cs = &e.nd->csStream();
	std::thread([cs] {
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool waited = false;
		while (!(waited = cs->testHeld() && cs->testProducerWaiting()) && std::chrono::steady_clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::fprintf(stderr, "[TEST] exit-midframe: worker released (%s)\n", waited ? "the exit flush waited for it" : "timeout");
		cs->testRelease();
	}).detach();
	std::fprintf(stderr, "[TEST] exit-midframe: ready\n");
	e.devReleased = true;   // never released: the process exits with the worker held mid-frame ...
	e.keepWindow = true;    // ... and its window alive
	*d += "device leaked with the worker held mid-frame; the parent reads the exit";
	return ok;
}
bool caseCsExitMidframe(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csExitNoSettle;
	const ChildRun c = runSelf(planted ? "X-child-exit-midframe" : "child-exit-midframe", { { "GX_NATIVE_CS", "1" } });
	const std::vector<std::string> f = c.lines("[GXNATIVE] stats final"), post = c.lines("[TEST] post-line:");
	const bool waited = c.lines("[TEST] exit-midframe: worker released (the exit flush waited for it)").size() == 1;
	*d += "child " + c.how() + ", worker held at exit: " + (waited ? "yes" : "no") + ", final lines " + std::to_string(f.size()) +
		(post.empty() ? ", no observer line" : ", " + post[0].substr(7));
	SETUP(c.lines("[TEST] exit-midframe: ready").size() == 1 && waited, "the child exited with the worker held mid-frame");
	CHECK(c.exited && !c.signaled && (planted || c.code == 0), "the process exits normally");
	CHECK(f.size() == 1 && f[0].find(" end=exit") != std::string::npos, "the final line once, end=exit");
	CHECK(f.size() == 1 && f[0].find(" present_failures=1 ") != std::string::npos, "the line carries the final present's failure");
	CHECK(post.size() == 1 && post[0] == "[TEST] post-line: gx-native-cs=0 gx-vk-submit=0 logs=0",
		"no driver call or log from gx-native-cs or gx-vk-submit after the line");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// arm 19 (r2): a PARTIAL creation failure - the render target's image made, its attachment view failed (TEST knob) - on the front
// while the worker has just recorded texture uploads (their pending copies in GXRES, no front sync since). The cleanup must touch
// nothing the worker owns: judged in the TSan lane (no report). CS=1: the S retry then makes it (cs_create_retries +1); CS=0: today's
// refusal. cs-rt-view-retire (BY NAME, TSan lane) with X-: the old destroy path (dropPendingFor + a retirement from the front) ->
// TSan reports the race on GxResources::m_pendingImages. Its verdict is TSan's, not the arm's (the arm itself still passes and the
// runner prints PLANT NOT DETECTED): DETECTED = a "data race" report whose first frame is GxImage::destroy and whose previous write
// is the worker's GxResources::queueImageCopy (measured 13/10: 3 such reports, rc 134); the clean arm under TSan: 0 reports.
bool caseCsRtViewFail(Env& e, std::string* d)
{
	bool ok = true;
	std::vector<IDirect3DTexture8*> tex;
	for (int i = 0; i < 4; ++i) {
		IDirect3DTexture8* t = nullptr;
		SETUP(SUCCEEDED(e.dev->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "a managed texture");
		tex.push_back(t);
	}
	e.nd->resources().debug().failRtViewOnce = true;   // set before the uploads: resources() drains
	const uint64_t retries0 = e.nd->csCreateRetries(), refused0 = e.nd->stats().createsRefused;
	for (IDirect3DTexture8* t : tex) {
		D3DLOCKED_RECT lr;
		SETUP(SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)), "lock");
		for (UINT y = 0; y < 128; ++y)
			std::memset(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, 0x5a, 128 * 4);
		t->UnlockRect(0);   // A: the worker queues the copy (GXRES pending images)
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the worker runs them; no front sync (a sleep orders nothing)
	IDirect3DSurface8* rt = nullptr;
	const HRESULT hr = e.dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &rt);
	const uint64_t retries = e.nd->csCreateRetries() - retries0, refused = e.nd->stats().createsRefused - refused0;
	*d += "CreateRenderTarget hr " + std::to_string(long(hr)) + ", retries +" + std::to_string(retries) + ", refused +" + std::to_string(refused);
	if (e.nd->csOn()) {
		CHECK(hr == D3D_OK && rt && retries == 1 && refused == 0, "the front attempt failed cleanly, the S retry made it");
		if (rt) {
			const uint32_t px0 = clearSurface(e.dev, rt, 0xff336699) ? csarm::px(e, rt, 10, 10) : 0xdeadbeefu;
			*d += ", cleared " + csarm::hex(px0);
			CHECK(px0 == 0x336699, "the render target works");
		}
	} else
		CHECK(hr == D3DERR_INVALIDCALL && !rt && refused == 1, "direct: today's refusal");
	if (rt)
		rt->Release();
	for (IDirect3DTexture8* t : tex)
		t->Release();
	return ok;
}

// §3.4 mirror-check mode (GX_NATIVE_CS_MIRRORCHECK=1; its own suite run, never with the worker-hold arms - a drain inside a hold
// would wait forever): every mirrored family set, then Presents and an S point (MultiplyTransform) - the device counts 0 fields
// that differ from the translator, and GetViewport answers what was set. X-cs-mirror-viewport (SetViewport leaves the mirror): the
// viewport counted AND the getter wrong.
bool caseCsMirrorCheck(Env& e, std::string* d)
{
	bool ok = true;
	const uint64_t m0 = e.nd->mirrorMismatches();
	D3DMATRIX m = {};
	for (int i = 0; i < 4; ++i)
		(&m._11)[i * 5] = 1.0f + float(i);
	m._41 = 3.0f;
	e.dev->SetTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_VIEW, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &m);
	e.dev->SetTransform(D3DTS_TEXTURE1, &m);
	const D3DRENDERSTATETYPE rs[] = { D3DRS_ZENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE, D3DRS_FOGCOLOR,
		D3DRS_TEXTUREFACTOR, D3DRS_ALPHAREF };
	for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i)
		e.dev->SetRenderState(rs[i], DWORD(i + 2));
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	e.dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
	D3DMATERIAL8 mat = {};
	mat.Diffuse.r = 0.25f;
	mat.Power = 7.0f;
	e.dev->SetMaterial(&mat);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	const D3DVIEWPORT8 vp = { 4, 6, 50, 40, 0.1f, 0.9f };
	e.dev->SetViewport(&vp);
	D3DVIEWPORT8 got = {};
	e.dev->GetViewport(&got);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);   // checked after it (no target change: the viewport stays as set)
	e.dev->MultiplyTransform(D3DTS_WORLD, &m);   // GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process; an S point (never called by the game): checked before its body
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	const uint64_t mm = e.nd->mirrorMismatches() - m0;
	*d += "mirror fields differing " + std::to_string(mm) + ", GetViewport " + std::to_string(got.X) + "," + std::to_string(got.Y) + " " +
		std::to_string(got.Width) + "x" + std::to_string(got.Height);
	CHECK(mm == 0, "the mirror equals the translator after each Present and before the S body");
	CHECK(got.X == vp.X && got.Y == vp.Y && got.Width == vp.Width && got.Height == vp.Height && got.MinZ == vp.MinZ && got.MaxZ == vp.MaxZ,
		"GetViewport answers the viewport just set");
	return ok;
}

// arm 23 extended (r5 P2-2, r6 P2-4): slices are identified by POINTER across a reclaim. A TEST observer records the slice record
// at every dynamic lock, every unlock's flush and every bind (the worker). Rules: each flush resolves to the record its lock took
// (LIFO); a NOOVERWRITE lock and the next bind resolve to the current record (the last DISCARD's). Two layouts, reclaimed by an S
// creation retry (one injected image failure: recoverStorage frees the idle non-current, unlocked slices):
//   P: [idle, open A, open A2, current] -> a stale OUTSTANDING-lock identity (in range);
//   Q: [idle, current, open A]          -> a stale CURRENT identity and an out-of-range lock index.
// X-cs-reclaim-index (identity by index, the vector compacted) -> the observer reports a mismatch in each layout.
namespace csarm {
struct SliceObs
{
	std::mutex m;
	std::map<const void*, std::vector<const void*>> open;
	std::map<const void*, int> ids;
	std::vector<const void*> binds;
	const void* lastLock = nullptr;
	int flushBad = 0;
	std::string log;
	int id(const void* s)
	{
		if (!s)
			return 0;
		const auto it = ids.find(s);
		return it != ids.end() ? it->second : (ids[s] = int(ids.size()) + 1);
	}
	static void ev(void* u, int e, const void* b, const void* s)
	{
		SliceObs* o = static_cast<SliceObs*>(u);
		std::lock_guard<std::mutex> g(o->m);
		if (e == 0) {
			o->open[b].push_back(s);
			o->lastLock = s;
			o->log += " L" + std::to_string(o->id(s));
		} else if (e == 1) {
			std::vector<const void*>& st = o->open[b];
			const void* want = st.empty() ? nullptr : st.back();
			if (!st.empty())
				st.pop_back();
			o->flushBad += want != s;
			o->log += (want != s ? " F!" : " F") + std::to_string(o->id(s));
		} else
			o->binds.push_back(s);
	}
};
} // namespace csarm
bool caseCsReclaimIdentity(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	static csarm::SliceObs obs;
	{
		std::lock_guard<std::mutex> g(obs.m);
		obs.open.clear();
		obs.ids.clear();
		obs.binds.clear();
		obs.lastLock = nullptr;
		obs.flushBad = 0;
		obs.log.clear();
	}
	GXRender::Res::GxDebug& rd = e.nd->resources().debug();   // drains (set before any hold)
	rd.sliceEvent = &csarm::SliceObs::ev;
	rd.sliceEventUser = &obs;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	csarm::ffDiffuse(e);
	auto lockOf = [&](IDirect3DVertexBuffer8* vb, DWORD flags, BYTE** p) {
		*p = nullptr;
		const bool locked = SUCCEEDED(vb->Lock(0, 0, p, flags)) && *p;
		std::lock_guard<std::mutex> g(obs.m);
		return locked ? obs.lastLock : nullptr;
	};
	auto reclaim = [&](const char* layout) {
		e.nd->csSync();
		const uint64_t bytes0 = e.nd->resources().dynSliceBytes.load(), r0 = e.nd->recoveriesAt("CreateTexture");
		e.nd->debug().allocFailSite = "image";
		e.nd->debug().allocFailCount = 1;
		IDirect3DTexture8* t = nullptr;
		const HRESULT hr = e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
		e.nd->debug().allocFailSite = nullptr;
		const uint64_t bytes1 = e.nd->resources().dynSliceBytes.load();
		*d += std::string(layout) + ": slices " + std::to_string(bytes0 >> 16) + " -> " + std::to_string(bytes1 >> 16) + ";";
		if (t)
			t->Release();
		return hr == D3D_OK && e.nd->recoveriesAt("CreateTexture") - r0 == 1 && bytes1 < bytes0;
	};
	// the NOOVERWRITE lock after the reclaim and its draw: both on the current record; the pixels drawn
	auto drawCheck = [&](IDirect3DVertexBuffer8* vb, const void* cur, DWORD colour, const char* layout) {
		BYTE* p = nullptr;
		const void* s = lockOf(vb, D3DLOCK_NOOVERWRITE, &p);
		if (p)
			csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 64, 64, colour);
		vb->Unlock();
		size_t b0;
		{
			std::lock_guard<std::mutex> g(obs.m);
			b0 = obs.binds.size();
		}
		e.dev->SetStreamSource(0, vb, sizeof(QV));
		e.dev->BeginScene();
		e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		e.dev->EndScene();
		e.dev->SetStreamSource(0, nullptr, 0);
		e.nd->csSync();
		const void* bound;
		{
			std::lock_guard<std::mutex> g(obs.m);
			bound = obs.binds.size() > b0 ? obs.binds[b0] : nullptr;
		}
		const uint32_t px = csarm::px(e, bb, 30, 30);
		*d += std::string(" ") + layout + " NOOVERWRITE " + (s == cur ? "current" : "STALE") + ", bind " + (bound == cur ? "current" : "STALE") +
			", pixel " + csarm::hex(px) + ";";
		return s == cur && bound == cur && px == (colour & 0xffffff);
	};
	// layout P: [idle, open A, open A2, current]
	IDirect3DVertexBuffer8* vp = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(64u << 10, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vp)) && vp, "dynamic VB (P)");
	BYTE* p = nullptr;
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);
	lockOf(vp, D3DLOCK_DISCARD, &p);
	vp->Unlock();
	lockOf(vp, D3DLOCK_NOOVERWRITE, &p);   // A (stays open)
	lockOf(vp, D3DLOCK_DISCARD, &p);
	vp->Unlock();
	lockOf(vp, D3DLOCK_NOOVERWRITE, &p);   // A2 (stays open)
	const void* curP = lockOf(vp, D3DLOCK_DISCARD, &p);
	vp->Unlock();
	csarm::release(e);
	SETUP(reclaim("P"), "P: the S retry reclaimed the idle slice");
	vp->Unlock();   // A2
	vp->Unlock();   // A
	int badP;
	{
		std::lock_guard<std::mutex> g(obs.m);
		badP = obs.flushBad;
	}
	const bool drawP = drawCheck(vp, curP, 0xff6699cc, "P");
	// layout Q: [idle, current, open A]
	IDirect3DVertexBuffer8* vq = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(64u << 10, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vq)) && vq, "dynamic VB (Q)");
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 2);
	lockOf(vq, D3DLOCK_DISCARD, &p);
	vq->Unlock();
	lockOf(vq, D3DLOCK_DISCARD, &p);
	vq->Unlock();
	lockOf(vq, D3DLOCK_NOOVERWRITE, &p);   // A on the third slice (stays open)
	csarm::release(e);
	e.nd->csSync();
	lockOf(vq, D3DLOCK_DISCARD, &p);   // reuses the first slice (the third is locked)
	vq->Unlock();
	lockOf(vq, D3DLOCK_NOOVERWRITE, &p);   // L1 on the first slice ...
	const void* curQ = lockOf(vq, D3DLOCK_DISCARD, &p);   // ... so this one takes the second
	vq->Unlock();
	vq->Unlock();   // L1: the first slice is idle again
	SETUP(reclaim("Q"), "Q: the S retry reclaimed the idle slice");
	vq->Unlock();   // A
	int badQ;
	{
		std::lock_guard<std::mutex> g(obs.m);
		badQ = obs.flushBad - badP;
	}
	const bool drawQ = drawCheck(vq, curQ, 0xff33cc66, "Q");
	{
		std::lock_guard<std::mutex> g(obs.m);
		*d += " trace" + obs.log;
	}
	rd.sliceEvent = nullptr;   // (the device is idle: csSync above)
	CHECK(badP == 0 && badQ == 0, "every unlock flushed the slice its lock took");
	CHECK(drawP && drawQ, "NOOVERWRITE and the draw stayed on the current slice; exact pixels");
	vp->Release();
	vq->Release();
	bb->Release();
	return ok;
}

// ---------------------------------------------------------------------- A23-B STRICT plant arms (PREREG A23-B (b), the STRICT list)
// Stable arms (the SERIAL schedule applies: no cs- prefix) that MOVE PIXELS for each digest plant; the strict runner's plant pass
// requires each plant (X-, at CS=1) to change the EXEC digest against the clean arm at CS=0, and the tap driver the tap. The plants
// act only on the worker path (xNeedsCs).
namespace csarm {
void fillQuad(IDirect3DVertexBuffer8* vb, float x0, DWORD c)
{
	BYTE* p = nullptr;
	if (SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p) {
		quad(reinterpret_cast<QV*>(p), x0, 0, x0 + 32, 64, c);
		vb->Unlock();
	}
}
void drawVb(Env& e, IDirect3DVertexBuffer8* vb, UINT stride)
{
	e.dev->SetStreamSource(0, vb, stride);
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
}
} // namespace csarm

// X-cs-reorder: a SetRenderState pushed after the next draw - the draw runs with the old TFACTOR colour
bool caseStrictReorder(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "VB");
	csarm::ffDiffuse(e);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff0000ff);
	csarm::fillQuad(vb, 0, 0xffffffff);
	csarm::drawVb(e, vb, sizeof(QV));
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff00ff00);   // the plant pushes this one after the next draw
	csarm::fillQuad(vb, 32, 0xffffffff);
	csarm::drawVb(e, vb, sizeof(QV));
	const uint32_t l = csarm::px(e, bb, 10, 30), r = csarm::px(e, bb, 40, 30);
	*d += "left " + csarm::hex(l) + ", right " + csarm::hex(r);
	CHECK(l == 0x0000ff && r == 0x00ff00, "each draw ran with the TFACTOR set before it");
	vb->Release();
	bb->Release();
	return ok;
}

// X-cs-payload-late: the first unlock's bytes are read when its (deferred) upload runs - after the second write
bool caseStrictPayload(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "VB");
	csarm::ffDiffuse(e);
	csarm::fillQuad(vb, 0, 0xffff0000);
	csarm::drawVb(e, vb, sizeof(QV));
	csarm::fillQuad(vb, 32, 0xff00ff00);
	csarm::drawVb(e, vb, sizeof(QV));
	const uint32_t l = csarm::px(e, bb, 10, 30), r = csarm::px(e, bb, 40, 30);
	*d += "left " + csarm::hex(l) + ", right " + csarm::hex(r);
	CHECK(l == 0xff0000 && r == 0x00ff00, "each draw shows the bytes written before it");
	vb->Release();
	bb->Release();
	return ok;
}

// X-tap-lod: a MANAGED texture whose level 0 is red and level 1 green, SetLOD(1), a 1:1 quad - green (the view starts at level 1)
bool caseStrictLod(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::TV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	IDirect3DTexture8* t = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(64, 64, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "a 2-level managed texture");
	for (UINT lv = 0; lv < 2; ++lv) {
		D3DLOCKED_RECT lr;
		SETUP(SUCCEEDED(t->LockRect(lv, &lr, nullptr, 0)), "lock a level");
		const UINT n = 64 >> lv;
		for (UINT y = 0; y < n; ++y)
			for (UINT x = 0; x < n; ++x)
				reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = lv ? 0xff00ff00u : 0xffff0000u;
		t->UnlockRect(lv);
	}
	t->SetLOD(1);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTexture(0, t);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const TV q[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 64, 0, 0.5f, 1, 1, 0 }, { 0, 64, 0.5f, 1, 0, 1 }, { 64, 64, 0.5f, 1, 1, 1 } };
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
	e.dev->EndScene();
	e.dev->SetTexture(0, nullptr);
	const uint32_t px = csarm::px(e, bb, 30, 30);
	*d += "SetLOD(1) quad " + csarm::hex(px);
	CHECK(px == 0x00ff00, "the quad samples level 1");
	t->Release();
	bb->Release();
	return ok;
}

// X-tap-substitute: two back buffers; after a Present (a rotation) a Clear of back buffer 0 must land in ITS storage
bool caseStrictBbRotation(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff112233), "clear back buffer 0");
	SETUP(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "a Present (rotation)");
	bb->Release();
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff446688), "clear the new back buffer 0");
	const uint32_t px = csarm::px(e, bb, 30, 30);
	*d += "back buffer 0 after the rotation " + csarm::hex(px);
	CHECK(px == 0x446688, "the Clear landed in back buffer 0's current storage");
	bb->Release();
	return ok;
}

// DESIGN §7 front-end cost bench (descriptive, BY NAME, run with --no-validation): ns per front-end call by class - the calling
// thread's cost (CS=1: the front half + the record copy; CS=0: the whole inline body) and front + drain (the worker's share too).
// One BENCH line per class; nothing is asserted but that every call answered as today.
bool caseBenchFront(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	using clk = std::chrono::steady_clock;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DVertexBuffer8* sv = nullptr;
	IDirect3DTexture8* tex = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "VB");
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4096, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &sv)) && sv, "4 KiB static VB");
	SETUP(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "64x64 texture");
	csarm::fillQuad(vb, 0, 0xff336699);
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	int bad = 0;
	auto bench = [&](const char* name, int n, const std::function<void(int)>& call) {
		e.nd->csSync();
		const uint64_t busy0 = e.nd->csStream().stats().busyNs;
		const clk::time_point t0 = clk::now();
		for (int i = 0; i < n; ++i)
			call(i);
		const clk::time_point t1 = clk::now();
		e.nd->csSync();
		const clk::time_point t2 = clk::now();
		const double busy = double(e.nd->csStream().stats().busyNs - busy0) / n;   // the worker running commands (exact after a drain)
		auto ns = [n](clk::time_point a, clk::time_point b) { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / n; };
		say("BENCH cs=%d %-26s n=%-6d caller %8.1f ns/call   caller+drain %8.1f ns/call   worker busy %8.1f ns/call", e.nd->csOn() ? 1 : 0,
			name, n, ns(t0, t1), ns(t0, t2), busy);
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	};
	DWORD v = 0;
	bench("F GetRenderState", 20000, [&](int) { bad += e.dev->GetRenderState(D3DRS_ZENABLE, &v) != D3D_OK; });
	bench("A SetRenderState", 20000, [&](int i) { bad += e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, DWORD(i)) != D3D_OK; });
	bench("A SetTextureStageState", 20000, [&](int i) { bad += e.dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, DWORD(i & 7)) != D3D_OK; });
	D3DMATRIX m = {};
	m._11 = m._22 = m._33 = m._44 = 1.0f;
	bench("A SetTransform(WORLD)", 20000, [&](int i) { m._41 = float(i); bad += e.dev->SetTransform(D3DTS_WORLD, &m) != D3D_OK; });	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	bench("A SetTexture", 20000, [&](int i) { bad += e.dev->SetTexture(0, (i & 1) ? tex : nullptr) != D3D_OK; });
	e.dev->SetTexture(0, nullptr);
	bench("A SetStreamSource", 20000, [&](int i) { bad += e.dev->SetStreamSource(0, (i & 1) ? vb : nullptr, sizeof(QV)) != D3D_OK; });
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	bench("A DrawPrimitive (2 tris)", 5000, [&](int) { bad += e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2) != D3D_OK; });
	QV up[4];
	csarm::quad(up, 0, 0, 8, 8, 0xff00ff00);
	bench("A DrawPrimitiveUP 80 B", 5000, [&](int) { bad += e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, up, sizeof(QV)) != D3D_OK; });
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	std::vector<QV> big(51);
	for (size_t i = 0; i < big.size(); ++i)
		big[i] = up[i & 3];
	bench("A DrawPrimitiveUP 1 KiB", 5000, [&](int) { bad += e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 17, big.data(), sizeof(QV)) != D3D_OK; });
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	bench("A VB Lock+Unlock 4 KiB", 2000, [&](int i) {
		BYTE* p = nullptr;
		if (FAILED(sv->Lock(0, 0, &p, 0)) || !p) {
			++bad;
			return;
		}
		std::memset(p, i, 4096);
		sv->Unlock();
	});
	bench("A Texture LockRect+Unlock 16 KiB", 2000, [&](int i) {
		D3DLOCKED_RECT lr;
		if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
			++bad;
			return;
		}
		for (UINT y = 0; y < 64; ++y)
			std::memset(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, i, 256);
		tex->UnlockRect(0);
	});
	bench("A Present", 500, [&](int) { bad += e.dev->Present(nullptr, nullptr, nullptr, nullptr) != D3D_OK; });
	*d += "calls answered as today: " + std::string(bad ? "NO (" + std::to_string(bad) + ")" : "yes");
	CHECK(bad == 0, "every call answered D3D_OK");
	e.dev->SetStreamSource(0, nullptr, 0);
	tex->Release();
	sv->Release();
	vb->Release();
	bb->Release();
	return ok;
}

// Batched publication's correctness condition (integrator 13/10): every producer wait publishes first, so no wait can depend on a
// command the worker cannot see. The child queues setters only (never published: no draw, Clear, EndScene or Present, < 16 KiB),
// then an S call (MultiplyTransform drains). X-cs-wait-unpublished (the waits skip the publish) -> the drain waits forever: the
// child's watchdog ends it (rc 124).
bool caseChildWaitPublish(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	e.nd->csStream().testWaitNoPublish = e.nd->debug().csWaitUnpublished;   // drains: set before the setters
	for (DWORD i = 0; i < 16; ++i)
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, i);
	D3DMATRIX m = {};
	m._11 = m._22 = m._33 = m._44 = 1.0f;
	const HRESULT hr = e.dev->MultiplyTransform(D3DTS_WORLD, &m);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	DWORD v = 0;
	e.dev->GetRenderState(D3DRS_TEXTUREFACTOR, &v);
	std::fprintf(stderr, "[TEST] WAIT-PUBLISH ok hr=%ld tfactor=%lu\n", long(hr), (unsigned long)v);
	*d += "the S call returned";
	CHECK(hr == D3D_OK && v == 15, "the drain returned with every setter run");
	return ok;
}
bool caseCsWaitPublish(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csWaitUnpublished;
	const ChildRun c = runSelf(planted ? "X-child-wait-publish" : "child-wait-publish", { { "GX_NATIVE_CS", "1" }, { "GX_ARM_WATCHDOG_S", "15" } }, 120);
	*d += "child " + c.how();
	CHECK(c.exited && c.code == 0 && c.lines("[TEST] WAIT-PUBLISH ok").size() == 1, "a wait never depends on an unpublished command");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}
// arm 11's PLANTED form, in a child (see caseCsResetDrain): judged by the verdict line the child prints while its worker is still held
bool csResetDrainPlantedParent(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const ChildRun c = runSelf("X-child-cs-reset-drain", { { "GX_NATIVE_CS", "1" } }, 60);
	const std::vector<std::string> v = c.lines("[TEST] RESET-DRAIN ");
	*d += "child " + c.how() + (v.empty() ? ", no verdict line" : ", " + v[0].substr(7));
	// judged by ITS symptom only, from a COMPLETE verdict line (every field, the end marker): Reset returned while
	// the worker was still held (no drain) or touched worker state
	long hr = 0;
	int rf = -1;
	unsigned long long own = 0;
	char tail[8] = {};
	const bool parsed = v.size() == 1 &&
		std::sscanf(v[0].c_str(), "[TEST] RESET-DRAIN hr=%ld released_first=%d own=%llu %7s", &hr, &rf, &own, tail) == 4 &&
		std::strcmp(tail, "end") == 0 && (rf == 0 || rf == 1);
	CHECK(!(parsed && (rf == 0 || own > 0)), "Reset waited for the worker, touched nothing of it before");
#else
	SETUP(false, "the planted form runs in a child process (posix_spawn)");
#endif
	return ok;
}
bool caseCsWakeTarget(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csWakeLate;
	const ChildRun c = runSelf(planted ? "X-child-cs-wake-target" : "child-cs-wake-target", { { "GX_ARM_WATCHDOG_S", "30" } }, 120);
	*d += "child " + c.how();
	// the X- child exits non-zero when its case HOLDS (the runner inverts a planted arm), so a planted run is judged by the hang alone:
	// broken iff the child started the arm, never reached its last line and was ended by the arm watchdog - a plant that changed
	// nothing, or a child that failed for any other reason (spawn, setup, a crash), fails this X- arm
	const bool started = c.lines("[TEST] WAKE-TARGET started").size() == 1, reached = c.lines("[TEST] WAKE-TARGET ok").size() == 1;
	const bool hung = started && !reached && !c.lines("WATCHDOG: arm X-child-cs-wake-target exceeded").empty();
	if (planted)
		CHECK(!hung, "a sleeping producer was never woken (the planted target hung: watchdog)");
	else
		CHECK(c.spawned && c.exited && c.code == 0 && started && reached, "a sleeping producer was never woken (lost wake-up)");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// Clear with RECTANGLES - two distinct ones, the worker held while the caller zeroes its array: exactly those
// rectangles take the colour, the rest (and the shared corner of the first) keeps the previous clear, AND the exec half received
// exactly the call's 32 bytes (EXEC sink: the pixels alone can pass a short copy when the reused ring memory still holds the same
// rectangles from an earlier identical call - measured: the plant passed in suite order, failed alone). X-cs-clear-rect-short (the
// payload copies count x 4 bytes, not count x 16). ASYNC (holds the worker).
namespace csarm {
struct ClearSink final : GXRender::Dev::GxCsExecSink
{
	std::mutex m;
	std::vector<std::pair<size_t, uint64_t>> seen;   // (bytes, fnv) of every Clear payload the exec halves saw
	void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg*, size_t, const GXRender::Dev::GxCsBytes* b) override
	{
		if (op != GXRender::Dev::GxCsOp::Clear || !b)
			return;
		const size_t n = size_t(b->rowBytes) * b->rows;
		std::lock_guard<std::mutex> g(m);
		seen.push_back({ n, b->p && n ? fnv64(b->p, n) : 0 });
	}
	void destroyed(const void*) override {}
	void created(uint64_t, uint32_t, const GXRender::Dev::GxCsArg*, size_t) override {}
};
} // namespace csarm
bool caseCsClearRects(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	D3DRECT r[2] = { { 4, 4, 20, 20 }, { 40, 30, 60, 50 } };
	const uint64_t want = fnv64(r, sizeof(r));
	static csarm::ClearSink sink;
	{
		std::lock_guard<std::mutex> g(sink.m);
		sink.seen.clear();
	}
	e.nd->csSync();   // the setup's own Clear has run: the sink sees this call's alone
	GXRender::Dev::g_csExecSink.store(&sink);
	csarm::hold(e);
	const HRESULT hr = e.dev->Clear(2, r, D3DCLEAR_TARGET, 0xff11aa55, 1.0f, 0);
	std::memset(r, 0, sizeof(r));   // the caller reuses its array at once
	csarm::release(e);
	e.nd->csSync();
	GXRender::Dev::g_csExecSink.store(nullptr);
	std::vector<std::pair<size_t, uint64_t>> seen;
	{
		std::lock_guard<std::mutex> g(sink.m);
		seen = sink.seen;
	}
	const bool bytesOk = seen.size() == 1 && seen[0].first == sizeof(r) && seen[0].second == want;
	const uint32_t in1 = csarm::px(e, bb, 10, 10), in2 = csarm::px(e, bb, 50, 40), out = csarm::px(e, bb, 30, 30),
		edge = csarm::px(e, bb, 20, 20), last = csarm::px(e, bb, 59, 49);
	*d += "exec payload " + (seen.empty() ? std::string("none") : std::to_string(seen[0].first) + " bytes") + (bytesOk ? " = the call's" : " DIFFERS") +
		", inside " + csarm::hex(in1) + " / " + csarm::hex(in2) + " / " + csarm::hex(last) + ", outside " + csarm::hex(out) + " / " + csarm::hex(edge);
	CHECK(bytesOk, "the exec half received the call's two rectangles, all 32 bytes");
	CHECK(hr == D3D_OK && in1 == 0x11aa55 && in2 == 0x11aa55 && last == 0x11aa55 && out == 0 && edge == 0,
		"exactly the two rectangles of the call were cleared");
	bb->Release();
	return ok;
}

// ---------------------------------------------: entry points no arm reached (each with its plant)
// DrawIndexedPrimitive -> pixels; X-cs-drop-indexed (answered, never sent) leaves the quad undrawn. STRICT (serial).
// r8-oracle P2-3 / r9-oracle P2-2: three colour groups (vertices 0-3, 4-7, 8-11), base vertex 4, and an IB of two index groups
// (0-3 and 4-7, relative to the base) drawn from StartIndex 6: the draw takes the THIRD group. X-cs-indices-base-zero (exec passes
// base 0) draws the second, X-cs-indexed-start-zero (exec passes start 0) the second, X-cs-drop-indexed nothing.
bool caseStrictIndexed(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DIndexBuffer8* ib = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(12 * sizeof(QV), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "VB");
	SETUP(SUCCEEDED(e.dev->CreateIndexBuffer(12 * 2, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib)) && ib, "IB");
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "lock the VB");
	csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 32, 64, 0xffcc3366);       // vertices 0-3
	csarm::quad(reinterpret_cast<QV*>(p) + 4, 0, 0, 32, 64, 0xff3366cc);   // vertices 4-7: base vertex 4, start index 0
	csarm::quad(reinterpret_cast<QV*>(p) + 8, 0, 0, 32, 64, 0xff66cc33);   // vertices 8-11: base vertex 4, start index 6
	vb->Unlock();
	p = nullptr;
	SETUP(SUCCEEDED(ib->Lock(0, 0, &p, 0)) && p, "lock the IB");
	const uint16_t idx[12] = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 };
	std::memcpy(p, idx, sizeof(idx));
	ib->Unlock();
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->SetIndices(ib, 4);
	e.dev->BeginScene();
	const HRESULT hr = e.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 4, 4, 6, 2);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	e.dev->SetIndices(nullptr, 0);
	const uint32_t px = csarm::px(e, bb, 10, 30);
	*d += "indexed quad (base vertex 4, start index 6) " + csarm::hex(px);
	CHECK(hr == D3D_OK && px == 0x66cc33, "the indexed draw drew its quad from its start index and base vertex");
	ib->Release();
	vb->Release();
	bb->Release();
	return ok;
}

// Shader constants with the worker held: the caller overwrites its arrays right after the calls - the translator must hold the
// values of the call (payload taken at the call). X-cs-const-by-ref (read when the worker runs them) -> the overwritten values. ASYNC.
bool caseCsConstants(Env& e, std::string* d)
{
	bool ok = true;
	float vs[3][4], ps[2][4], vs0[3][4], ps0[2][4];
	for (int i = 0; i < 12; ++i)
		(&vs[0][0])[i] = float(i) + 0.25f;
	for (int i = 0; i < 8; ++i)
		(&ps[0][0])[i] = -float(i) - 0.5f;
	std::memcpy(vs0, vs, sizeof(vs));
	std::memcpy(ps0, ps, sizeof(ps));
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);
	const HRESULT h1 = e.dev->SetVertexShaderConstant(4, vs, 3), h2 = e.dev->SetPixelShaderConstant(1, ps, 2);
	std::memset(vs, 0x7f, sizeof(vs));
	std::memset(ps, 0x7f, sizeof(ps));
	csarm::release(e);
	e.nd->csSync();
	const GXRender::Rec::GxD3D8Translate& tr = e.nd->translator();
	bool same = true;
	for (uint32_t r = 0; r < 3; ++r)
		same = same && std::memcmp(tr.testVsConst(4 + r), vs0[r], 16) == 0;
	for (uint32_t r = 0; r < 2; ++r)
		same = same && std::memcmp(tr.testPsConst(1 + r), ps0[r], 16) == 0;
	*d += std::string("constants as at the call: ") + (same ? "yes" : "NO");
	CHECK(h1 == D3D_OK && h2 == D3D_OK && same, "the translator holds the values of the call");
	return ok;
}

// Constant ranges past the register file (c96 / c8): the caller's memory ends exactly where today's read ends, a PROT_NONE page after
// it - no fault. In a child. Merge of render/integration 120c574b3: a VERTEX range past c95
// is now REFUSED on the front (INVALIDCALL, vsConstantsRefused, nothing read, nothing changed), so the vertex half proves the refusal
// reads nothing and the in-range boundary (c90-c95, ending at the guard page) is set; the PIXEL half (no refusal: c6 count 4 clipped to
// c6-c7) keeps the clipped read. X-cs-const-overread (count x 16 copied) faults on the pixel call.
bool caseChildConstBounds(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const size_t page = size_t(sysconf(_SC_PAGESIZE));
	uint8_t* base = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
	SETUP(base != MAP_FAILED && mprotect(base + page, page, PROT_NONE) == 0, "a page and a PROT_NONE guard page");
	float* v = reinterpret_cast<float*>(base + page - 6 * 16);   // c90-c95: the last 6 registers, ending at the guard page
	for (int i = 0; i < 24; ++i)
		v[i] = float(i) * 2.0f;
	float* q = reinterpret_cast<float*>(base + page - 2 * 16);   // c6-c7 of (6, count 4) - the same last 32 bytes as v's c94-c95
	for (int i = 0; i < 8; ++i)
		q[i] = float(i) + 100.0f;
	float v0[24], q0[8];   // the bytes each in-range call reads (q overlaps v's tail, so taken after both writes)
	std::memcpy(v0, v, sizeof(v0));
	std::memcpy(q0, q, sizeof(q0));
	const uint64_t refused0 = e.nd->stats().vsConstantsRefused;
	const HRESULT h1 = e.dev->SetVertexShaderConstant(90, v, 6), h2 = e.dev->SetPixelShaderConstant(6, q, 4);
	for (int i = 0; i < 24; ++i)   // the payload was taken at the call: new bytes the refused call below must neither read nor set
		v[i] = float(i) + 1000.0f;
	const HRESULT h1b = e.dev->SetVertexShaderConstant(90, v, 20);   // past c95: refused - reading count x 16 would fault
	const HRESULT h3 = e.dev->SetVertexShaderConstant(96, base + page, 4);   // refused; the pointer is the guard page itself
	e.nd->csSync();
	const GXRender::Rec::GxD3D8Translate& tr = e.nd->translator();
	const uint64_t refused = e.nd->stats().vsConstantsRefused - refused0;
	const bool set = std::memcmp(tr.testVsConst(90), v0, 6 * 16) == 0 && std::memcmp(tr.testPsConst(6), q0, 2 * 16) == 0;
	std::fprintf(stderr, "[TEST] CONST-BOUNDS ok hr=%ld/%ld/%ld/%ld refused=%llu set=%d\n", long(h1), long(h2), long(h1b), long(h3),
		(unsigned long long)refused, set ? 1 : 0);
	*d += "no fault";
	CHECK(h1 == D3D_OK && h2 == D3D_OK && h1b == D3DERR_INVALIDCALL && h3 == D3DERR_INVALIDCALL && refused == 2 && set,
		"c90-c95 and the clipped c6-c7 set, the two vertex ranges past c95 refused, nothing past the caller's bytes read");
#else
	SETUP(false, "needs mmap");
#endif
	return ok;
}
bool caseCsConstBounds(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csConstOverread;
	const ChildRun c = runSelf(planted ? "X-child-const-bounds" : "child-const-bounds", {});
	*d += "child " + c.how();
	CHECK(c.exited && c.code == 0 && c.lines("[TEST] CONST-BOUNDS ok").size() == 1 && c.err.find("set=1") != std::string::npos,
		"constant ranges past the register file read only today's bytes");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// FF lighting: a red directional light on a white material, a quad facing it - red. X-cs-drop-light / X-cs-drop-light-enable
// (the command not sent) -> unlit (black). STRICT (serial).
bool caseStrictLight(Env& e, std::string* d)
{
	bool ok = true;
	struct LV { float x, y, z, nx, ny, nz; };
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "clear");
	D3DMATRIX id = {};
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	e.dev->SetTransform(D3DTS_WORLD, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &id);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_LIGHTING, TRUE);
	e.dev->SetRenderState(D3DRS_AMBIENT, 0);
	D3DMATERIAL8 mat = {};
	mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
	e.dev->SetMaterial(&mat);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	D3DLIGHT8 l = {};
	l.Type = D3DLIGHT_DIRECTIONAL;
	l.Diffuse.r = 1.0f;
	l.Diffuse.a = 1.0f;
	l.Direction.z = 1.0f;
	e.dev->SetLight(0, &l);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->LightEnable(0, TRUE);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL);
	const LV q[4] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(LV));
	e.dev->EndScene();
	const uint32_t px = csarm::px(e, bb, 30, 30);
	// r9-oracle P2-3: LightEnable(FALSE), lighting still on, over a blue clear: the light is out - a black quad (ambient 0).
	// X-cs-light-enable-true (the exec half forwards TRUE, the trace keeps FALSE) draws it red again
	e.dev->LightEnable(0, FALSE);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	SETUP(clearSurface(e.dev, bb, 0xff0000ff), "the blue clear");
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(LV));
	e.dev->EndScene();
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	const uint32_t off = csarm::px(e, bb, 30, 30);
	*d += "lit quad " + csarm::hex(px) + ", light disabled " + csarm::hex(off);
	CHECK(px == 0xff0000, "the light reached the draw");
	CHECK(off == 0, "LightEnable(FALSE) put the light out (a black quad over the blue clear)");
	bb->Release();
	return ok;
}

namespace csarm {   // a programmable vs.1.1 (mov oPos, v0; mov oD0, v1) and its declaration (the r2-oracle P2-2 arms)
const DWORD kVsDecl[] = { 0x20000000u /* STREAM(0) */, 0x40030000u /* REG(v0, FLOAT4) */, 0x40040001u /* REG(v1, D3DCOLOR) */,
	0xFFFFFFFFu /* END */ };
const DWORD kVsCode[] = { 0xFFFE0101u /* vs.1.1 */, 0x00000001u, 0xC00F0000u, 0x90E40000u /* mov oPos, v0 */, 0x00000001u,
	0xD00F0000u, 0x90E40001u /* mov oD0, v1 */, 0x0000FFFFu };
} // namespace csarm

// CreatePixelShader and both constant setters reach the EXEC digest (STRICT; the bytecode is not a catalogue shader, so no pixels:
// X-cs-drop-create-ps / X-cs-drop-constants are judged by the --strict plant pass alone, strict level 2)
bool caseStrictShaderState(Env& e, std::string* d)
{
	bool ok = true;
	const DWORD code[] = { 0xFFFF0101u, 0x00000001u, 0x800F0000u, 0x90E40000u, 0x0000FFFFu };   // ps.1.1: mov r0, v0
	DWORD h = 0;
	const HRESULT hc = e.dev->CreatePixelShader(code, &h);
	float c[4][4];
	for (int i = 0; i < 16; ++i)
		(&c[0][0])[i] = float(i) * 0.5f;
	const HRESULT hv = e.dev->SetVertexShaderConstant(10, c, 4), hp = e.dev->SetPixelShaderConstant(2, c, 2);
	e.dev->SetPixelShader(h);
	e.dev->SetPixelShader(0);
	e.dev->DeletePixelShader(h);
	DWORD vh = 0;   // a programmable vertex shader with code, and a draw through it
	const HRESULT hvs = e.dev->CreateVertexShader(csarm::kVsDecl, csarm::kVsCode, &vh, 0);
	e.dev->SetVertexShader(vh);
	csarm::QV q[4];
	csarm::quad(q, 0, 0, 16, 16, 0xff00ff00);
	e.dev->BeginScene();
	const HRESULT hd = e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
	e.dev->EndScene();
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	e.dev->DeleteVertexShader(vh);
	*d += "handles " + std::to_string(h) + "/" + std::to_string(vh);
	CHECK(hc == D3D_OK && h != 0 && hv == D3D_OK && hp == D3D_OK && hvs == D3D_OK && vh != 0 && hd == D3D_OK, "answered as today");
	return ok;
}

// a PROGRAMMABLE vertex shader's payload (declaration + function code) is taken at the call.
// The worker held; CreateVertexShader; the caller overwrites both arrays; release - the bytes the exec half saw (EXEC trace) equal
// the originals; then a draw through it answers as today. X-cs-vs-by-ref (read from the caller when the worker runs it). ASYNC.
namespace csarm {
struct VsSink final : GXRender::Dev::GxCsExecSink
{
	std::mutex m;
	std::vector<uint64_t> seen;   // fnv of every CreateVs payload the exec halves saw (decl, then code)
	void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg*, size_t, const GXRender::Dev::GxCsBytes* b) override
	{
		if (op != GXRender::Dev::GxCsOp::CreateVs || !b)
			return;
		std::lock_guard<std::mutex> g(m);
		seen.push_back(b->p ? fnv64(b->p, size_t(b->rowBytes) * b->rows) : 0);
	}
	void destroyed(const void*) override {}
	void created(uint64_t, uint32_t, const GXRender::Dev::GxCsArg*, size_t) override {}
};
} // namespace csarm
// /P2-3: the payload of a SUPPORTED (catalogue, ported) shader - Trees.vso with its declaration / the first ported
// pixel shader - created with the worker held, the caller's copy overwritten at once; after the release the translator must have
// resolved the bytes of the call as PORTED (a by-reference read, or the function pointer taken from the wrong offset, resolves to
// UNKNOWN). For the VS the exec half's bytes are also checked against the call's. X-cs-vs-by-ref / X-cs-ps-by-ref. ASYNC.
bool caseCsShaderPayload(Env& e, std::string* d, bool ps)
{
	bool ok = true;
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	const GXRender::GxCsEntry* ent = nullptr;
	for (size_t i = 0; i < n && !ent; ++i)
		if (ps ? (all[i].pixel && all[i].program) : (!all[i].pixel && all[i].program && all[i].decl && std::strstr(all[i].files, "Trees.vso")))
			ent = &all[i];
	SETUP(ent, ps ? "a ported pixel shader in the catalogue" : "Trees.vso (ported, with its declaration) in the catalogue");
	std::vector<DWORD> code(ent->dwords, ent->dwords + ent->count), decl;
	if (!ps)
		decl.assign(ent->decl, ent->decl + ent->declCount);
	static csarm::VsSink sink;
	{
		std::lock_guard<std::mutex> g(sink.m);
		sink.seen.clear();
	}
	GXRender::Dev::g_csExecSink.store(&sink);
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 3);
	DWORD h = 0;
	const HRESULT hc = ps ? e.dev->CreatePixelShader(code.data(), &h) : e.dev->CreateVertexShader(decl.data(), code.data(), &h, 0);
	std::fill(code.begin(), code.end(), 0xEEEEEEEEu);   // the caller reuses its arrays at once
	std::fill(decl.begin(), decl.end(), 0xEEEEEEEEu);
	csarm::release(e);
	e.nd->csSync();
	GXRender::Dev::g_csExecSink.store(nullptr);
	std::vector<uint64_t> seen;
	{
		std::lock_guard<std::mutex> g(sink.m);
		seen = sink.seen;
	}
	const uint8_t verdict = ps ? e.nd->translator().testPsVerdict(h) : e.nd->translator().testVsVerdict(h);
	const bool bytes = ps || (seen.size() == 2 && seen[0] == fnv64(ent->decl, ent->declCount * 4) && seen[1] == fnv64(ent->dwords, ent->count * 4));
	*d += std::string(ent->files) + ": verdict " + std::to_string(int(verdict)) + (verdict == GXRender::GxCsPorted ? " (ported)" : "") +
		(ps ? "" : bytes ? ", exec bytes = the call's" : ", exec bytes DIFFER");
	CHECK(hc == D3D_OK && h != 0, "answered as today");
	CHECK(verdict == GXRender::GxCsPorted && bytes, "the translator resolved the bytes of the call as the ported catalogue shader");
	// then DRAW with it bound (r4-oracle P2-2, r5-oracle P2-2/P2-3): the translator draws with this handle, each draw is recorded
	// (drawn +1, refused +0) and writes its pixel (the same result over a black and a white clear). PS: the result is not fixed
	// function's. VS (Trees.vso, c4-c7 = identity, c8 = 0, shade 1): a full-target quad of the vertex colour, exactly.
	// X-cs-ps-bind-dropped / X-cs-ps-draw-dropped / X-cs-vs-bind-wrong
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	csarm::ffDiffuse(e);
	csarm::QV q[4];
	csarm::quad(q, 0, 0, 64, 64, 0xff3399cc);
	struct TreeV { float pos[3], normal[3]; DWORD c; float uv[2]; };
	const TreeV tq[4] = { { { -1, -1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 0, 0 } }, { { 1, -1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 1, 0 } },
		{ { -1, 1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 0, 1 } }, { { 1, 1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 1, 1 } } };
	auto drawQuad = [&](DWORD clear) {
		clearSurface(e.dev, bb, clear);
		e.dev->BeginScene();
		if (ps)
			e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
		else
			e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(TreeV));
		e.dev->EndScene();
		return csarm::px(e, bb, 30, 30);
	};
	const uint32_t ff = ps ? drawQuad(0xff000000) : 0;
	if (ps)
		e.dev->SetPixelShader(h);
	else {
		const float k[5][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 0 } };
		e.dev->SetVertexShaderConstant(4, k, 5);
		e.dev->SetVertexShader(h);
	}
	const auto s0 = e.nd->translator().stats();
	const uint32_t onBlack = drawQuad(0xff000000), onWhite = drawQuad(0xffffffff);
	const auto s1 = e.nd->translator().stats();
	const uint32_t bound = ps ? e.nd->translator().testBoundPs() : e.nd->translator().testBoundVs();
	if (ps)
		e.dev->SetPixelShader(0);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	uint32_t late = onBlack;
	if (ps) {   // r6-oracle P2-1: the worker held across bind -> draw -> unbind (no drain between): the draw executes after the front
		clearSurface(e.dev, bb, 0xffffffff);   // already unbound it and must still use the handle of ITS call. X-cs-ps-bind-front
		csarm::hold(e);
		e.dev->SetPixelShader(h);
		e.dev->BeginScene();
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
		e.dev->EndScene();
		e.dev->SetPixelShader(0);
		csarm::release(e);
		late = csarm::px(e, bb, 30, 30);
	}
	const uint64_t drawn = s1.drawn - s0.drawn, refused = s1.refused - s0.refused;
	*d += ", drawn " + std::to_string(drawn) + " refused " + std::to_string(refused) + ": " + (ps ? "fixed function " + csarm::hex(ff) + ", " : "") +
		"with the shader " + csarm::hex(onBlack) + " / " + csarm::hex(onWhite) + ", translator bound " + std::to_string(bound) +
		(ps ? ", bound late (held) " + csarm::hex(late) : "");
	CHECK(bound == h && drawn == 2 && refused == 0, "the bound shader is the one the translator draws with, and both draws were recorded");
	CHECK(onBlack == onWhite && (ps ? onBlack != ff : rgbNear(onBlack, 0x3399cc)),
		ps ? "the draw wrote its pixel (not the clear) and it is not fixed function's" : "the draw wrote the vertex colour through the shader");
	CHECK(late == onBlack, "a bind the worker runs after the front unbound it draws with the handle of its own call");
	// r7-oracle P2-2: deleted while BOUND, a shader keeps drawing until it is unbound (d3d8; the translator retains it): bind, delete,
	// draw over a white clear -> the shader's own pixel again. X-cs-delete-unbinds (the exec half unbinds instead of deleting)
	if (ps)
		e.dev->SetPixelShader(h);
	else
		e.dev->SetVertexShader(h);
	const HRESULT hdel = ps ? e.dev->DeletePixelShader(h) : e.dev->DeleteVertexShader(h);
	const uint32_t afterDelete = drawQuad(0xffffffff);
	// r8-oracle P2-2: a VS deleted while bound - SetVertexShader(0) then changes nothing (d3d8's current handle is already 0), so it
	// still draws; without the exec half's deletion the translator would take the 0 as unbinding a LIVE shader (declaration only:
	// refused). X-cs-delete-skips-exec (the exec half traces and never deletes)
	uint32_t afterZero = afterDelete;
	if (!ps) {
		e.dev->SetVertexShader(0);
		afterZero = drawQuad(0xffffffff);
	}
	if (ps)
		e.dev->SetPixelShader(0);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	// r9-oracle P2-1: once unbound, the deleted shader's record is gone (255 = no record); X-cs-delete-skips-exec keeps it
	const uint8_t gone = ps ? e.nd->translator().testPsVerdict(h) : e.nd->translator().testVsVerdict(h);
	*d += ", deleted while bound " + csarm::hex(afterDelete) + (ps ? "" : ", then SetVertexShader(0) " + csarm::hex(afterZero)) +
		", record after the unbind " + (gone == 255 ? std::string("gone") : "KEPT");
	CHECK(gone == 255, "the deleted shader's record is gone once it is unbound");
	CHECK(hdel == D3D_OK && afterDelete == onBlack, "a shader deleted while bound still draws until it is unbound");
	CHECK(afterZero == onBlack, "SetVertexShader(0) after deleting the bound shader changes nothing");
	if (bb)
		bb->Release();
	return ok;
}
bool caseCsVsPayload(Env& e, std::string* d) { return caseCsShaderPayload(e, d, false); }
bool caseCsPsPayload(Env& e, std::string* d) { return caseCsShaderPayload(e, d, true); }

// ------------------------------------------------------------------ E · water-live-native-port (R-W1 + R-G1, 21/10/2026)
// cs-live-programs: live water's two programs (water:glare, water:foam) and the remastered ground's three (gx:ground-base / -mult /
// -mult-lite), drawn by the NATIVE device over four solid textures, a solid vertex colour and chosen constants. Every pixel must
// equal what DXVK's compile of the same TOKENS computes. The oracle (livearm::run) executes the catalogue's bytes - gx_cs_test proves
// they are the game's own arrays - with DXVK's dxso semantics: it decodes the tokens and never reads gx_cs_ps.frag, so a port that
// misreads its program differs here. Two discrimination checks show the draws can tell: the glare draw is far from the result of
// a [-1,1] clamp after every instruction (real ps.1.1 hardware's range, which DXVK does not apply), and the foam draw is far from
// the program with r1.b's replicate read as r1.a. X-cs-live-programs: the translator plant psCatalogMiss = the refusal every one
// of these draws met before the ports (refused, the target keeps its clear).
namespace livearm {
struct V4 { float v[4]; };
V4 argb(uint32_t c) { return { { float((c >> 16) & 0xff) / 255.f, float((c >> 8) & 0xff) / 255.f, float(c & 0xff) / 255.f, float(c >> 24) / 255.f } }; }
uint32_t pack(const V4& x)
{
	auto u = [](float f) { return uint32_t(std::lround(std::min(1.f, std::max(0.f, f)) * 255.f)); };
	return (u(x.v[3]) << 24) | (u(x.v[0]) << 16) | (u(x.v[1]) << 8) | u(x.v[2]);
}
// One ps.1.1 program on constant inputs, as references/fbraz3-dxvk src/dxso compiles it: constants clamped to [-1,1]
// (dxso_compiler.cpp:1721); the swizzle, then the source modifier (neg, bias = x - 0.5, comp = 1 - x, bx2 = x*2 - 1;
// emitSrcOperandPostSwizzleModifiers); add / sub / mul component-wise, mad = a*b + c (emitMad), lrp = s0*(s1 - s2) + s2 (emitMix),
// dp3 = the three products summed in order, replicated (emitDot; D · water-ra3-2 24/10/2026 for water:ra3);
// the result x 2^shift, then _sat, then the write mask (dxso_compiler.h emitDstStore) - no other clamp between instructions;
// co-issued instructions in token order. hwClamp = the semantics DXVK does NOT have (every result clamped to [-1,1]).
bool run(const uint32_t* tok, size_t n, const V4 tex[4], const V4& v0, const V4 c[8], bool hwClamp, V4* out, std::string* why)
{
	V4 r[2] = {}, t[4] = {};
	if (n < 2 || tok[0] != 0xFFFF0101u) {
		*why = "not ps.1.1";
		return false;
	}
	auto load = [&](uint32_t sTok, V4* o) {
		const uint32_t type = (sTok >> 28) & 7, num = sTok & 0x7FF, sw = (sTok >> 16) & 0xFF, mod = (sTok >> 24) & 0xF;
		V4 x;
		if (type == 0 && num < 2)
			x = r[num];
		else if (type == 1 && num == 0)
			x = v0;
		else if (type == 2 && num < 8) {
			x = c[num];
			for (float& f : x.v)
				f = std::min(1.f, std::max(-1.f, f));
		} else if (type == 3 && num < 4)
			x = t[num];
		else {
			*why = "source register type " + std::to_string(type) + " #" + std::to_string(num);
			return false;
		}
		for (int k = 0; k < 4; ++k) {
			float f = x.v[(sw >> (2 * k)) & 3];
			if (mod == 1)
				f = -f;
			else if (mod == 2)
				f = f - 0.5f;
			else if (mod == 6)
				f = 1.f - f;
			else if (mod == 4)
				f = f * 2.f - 1.f;
			else if (mod != 0) {
				*why = "source modifier " + std::to_string(mod);
				return false;
			}
			o->v[k] = f;
		}
		return true;
	};
	size_t i = 1;
	while (i < n && tok[i] != 0x0000FFFFu) {
		const uint32_t op = tok[i] & 0xFFFF;
		const int srcs = op == 0x42 ? 0 : op == 0x01 ? 1 : (op == 0x02 || op == 0x03 || op == 0x05 || op == 0x08) ? 2 : (op == 0x04 || op == 0x12) ? 3 : -1;
		if (srcs < 0 || i + 2 + size_t(srcs) > n) {
			*why = "opcode " + std::to_string(op);
			return false;
		}
		const uint32_t dst = tok[i + 1], dtype = (dst >> 28) & 7, dnum = dst & 0x7FF;
		V4 sv[3] = {};
		for (int k = 0; k < srcs; ++k)
			if (!load(tok[i + 2 + k], &sv[k]))
				return false;
		i += 2 + size_t(srcs);
		if (op == 0x42) {   // tex tN: stage N's texel (solid textures)
			if (dtype != 3 || dnum >= 4) {
				*why = "tex destination";
				return false;
			}
			t[dnum] = tex[dnum];
			continue;
		}
		if (dtype != 0 || dnum >= 2) {
			*why = "destination register";
			return false;
		}
		int shift = int((dst >> 24) & 0xF);
		if (shift & 8)
			shift -= 16;
		const float scale = shift >= 0 ? float(1 << shift) : 1.f / float(1 << -shift);
		const bool sat = ((dst >> 20) & 1) != 0;
		const float dot3 = sv[0].v[0] * sv[1].v[0] + sv[0].v[1] * sv[1].v[1] + sv[0].v[2] * sv[1].v[2];
		for (int k = 0; k < 4; ++k) {
			if (!((dst >> (16 + k)) & 1))
				continue;
			const float a = sv[0].v[k], b = sv[1].v[k], cc = sv[2].v[k];
			float res = op == 0x01 ? a : op == 0x02 ? a + b : op == 0x03 ? a - b : op == 0x05 ? a * b : op == 0x04 ? a * b + cc : op == 0x08 ? dot3 : a * (b - cc) + cc;
			res *= scale;
			if (sat)
				res = std::min(1.f, std::max(0.f, res));
			if (hwClamp)
				res = std::min(1.f, std::max(-1.f, res));
			r[dnum].v[k] = res;
		}
	}
	if (i >= n) {
		*why = "no END token";
		return false;
	}
	*out = r[0];
	return true;
}
bool far(uint32_t a, uint32_t b, int tol)
{
	for (int sh = 0; sh < 32; sh += 8)
		if (std::abs(int((a >> sh) & 0xff) - int((b >> sh) & 0xff)) > tol)
			return true;
	return false;
}
} // namespace livearm

bool caseCsLivePrograms(Env& e, std::string* d)
{
	bool ok = true;
	using livearm::V4;
	// the solid inputs (ARGB): t1 is not white, so every lrp and mul reads it; no channel of a texture repeats another's
	const uint32_t kTex[4] = { 0xC04080F0u, 0xD0F0E0C8u, 0x60FFC080u, 0xA0E0C0FFu }, kDiffuse = 0x9080C0A0u;
	struct Draw { const char* file; float c[5][4]; };
	const Draw draws[] = {
		// water:glare as the game sets it (c1 = glint colour x strength, c2 = sheen): t1*t2 squared x4 exceeds 1 in r and g
		{ "water:glare", { { 0, 0, 0, 0 }, { 0.5f, 0.25f, 0.125f, 0.5f }, { 0.1f, 0.1f, 0.1f, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
		// ...and with the glint colour above 1 (a tune > 1): DXVK clamps the constant to 1
		{ "water:glare", { { 0, 0, 0, 0 }, { 1.5f, 0.6f, 0.3f, 1.0f }, { 0.2f, 0.2f, 0.2f, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
		// water:foam: breakup scale / floor different per channel (and a low alpha floor), so reading r1.b's neighbours shows
		{ "water:foam", { { 0, 0, 0, 0 }, { 0.3f, 0.4f, 0.45f, 0.2f }, { 0.5f, 0.52f, 0.55f, 0.2f }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
		// gx:ground-base: k, s/4, 0.125, shade strength, the encoded neutral (TerrainTex.cpp GXGround_Base: every component alike)
		{ "gx:ground-base", { { 0.3f, 0.3f, 0.3f, 0.3f }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.125f, 0.125f, 0.125f, 0.125f }, { 0.8f, 0.8f, 0.8f, 0.8f },
		                      { 0.45f, 0.45f, 0.45f, 0.45f } } },
		// gx:ground-mult / -mult-lite: noise depth, 1, detail, 0.5, macro (GXGround_MultBegin)
		{ "gx:ground-mult", { { 0.7f, 0.7f, 0.7f, 0.7f }, { 1, 1, 1, 1 }, { 0.6f, 0.6f, 0.6f, 0.6f }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f, 0.4f } } },
		{ "gx:ground-mult-lite", { { 0.7f, 0.7f, 0.7f, 0.7f }, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
		// water:ra3 (RA3 water's sheen, D · water-ra3-2): c0 = a sun half vector, c1 = the sun's colour x strength; v0 = the sky x Fresnel
		{ "water:ra3", { { 0.3f, 0.2f, 0.93f, 0 }, { 0.9f, 0.8f, 0.6f, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
		// water:ra3river (its river program, C · water-ra3-3 25/10/2026): the same constants; v0.a (0x90 here) = the bank fade x the shroud
		{ "water:ra3river", { { 0.3f, 0.2f, 0.93f, 0 }, { 0.9f, 0.8f, 0.6f, 1 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } },
	};
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	auto entry = [&](const char* file) -> const GXRender::GxCsEntry* {
		for (size_t i = 0; i < n; ++i)
			if (all[i].pixel && std::strcmp(all[i].files, file) == 0)
				return &all[i];
		return nullptr;
	};
	for (const Draw& dr : draws)
		SETUP(entry(dr.file), (std::string(dr.file) + " in the catalogue").c_str());
	IDirect3DTexture8* tex[4] = {};
	for (int s = 0; s < 4; ++s) {
		SETUP(SUCCEEDED(e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex[s])) && tex[s], "CreateTexture 4x4");
		D3DLOCKED_RECT lr = {};
		SETUP(SUCCEEDED(tex[s]->LockRect(0, &lr, nullptr, 0)), "LockRect");
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 4; ++x)
				std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4, &kTex[s], 4);
		tex[s]->UnlockRect(0);
	}
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb, "back buffer");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	for (DWORD s = 0; s < 4; ++s) {
		e.dev->SetTexture(s, tex[s]);
		e.dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, 0);
		e.dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		e.dev->SetTextureStageState(s, D3DTSS_MINFILTER, D3DTEXF_POINT);
		e.dev->SetTextureStageState(s, D3DTSS_MAGFILTER, D3DTEXF_POINT);
		e.dev->SetTextureStageState(s, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		e.dev->SetTextureStageState(s, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		e.dev->SetTextureStageState(s, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	// fixed function would stop at stage 1 (the game's alpha preset leaves it so): a bound ps still reads all four stages
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	e.dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	struct TQ { float x, y, z, rhw; DWORD c; float u, v; };
	const TQ q[4] = { { 0, 0, 0.5f, 1, kDiffuse, 0, 0 }, { 64, 0, 0.5f, 1, kDiffuse, 1, 0 }, { 0, 64, 0.5f, 1, kDiffuse, 0, 1 },
	                  { 64, 64, 0.5f, 1, kDiffuse, 1, 1 } };
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	V4 tv[4];
	for (int s = 0; s < 4; ++s)
		tv[s] = livearm::argb(kTex[s]);
	const V4 v0 = livearm::argb(kDiffuse);
	const auto s0 = e.nd->translator().stats();
	uint32_t drawsMade = 0;
	DWORD handle[sizeof(draws) / sizeof(draws[0])] = {};
	int k = 0;
	for (const Draw& dr : draws) {
		const GXRender::GxCsEntry* ent = entry(dr.file);
		CHECK(SUCCEEDED(e.dev->CreatePixelShader(reinterpret_cast<const DWORD*>(ent->dwords), &handle[k])) && handle[k], std::string("[CreatePixelShader ") + dr.file + "]");
		V4 c[8] = {};
		for (int r = 0; r < 5; ++r) {
			for (int j = 0; j < 4; ++j)
				c[r].v[j] = dr.c[r][j];
			e.dev->SetPixelShaderConstant(DWORD(r), dr.c[r], 1);
		}
		V4 want, wrong;
		std::string why;
		CHECK(livearm::run(ent->dwords, ent->count, tv, v0, c, false, &want, &why), std::string("[oracle ") + dr.file + ": " + why + "]");
		e.dev->SetPixelShader(handle[k]);
		clearSurface(e.dev, bb, 0x00000000u);
		e.dev->BeginScene();
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TQ));
		e.dev->EndScene();
		++drawsMade;
		e.dev->SetPixelShader(0);
		uint32_t got = 0xdeadbeefu;
		readPixel(e.dev, bb, 31, 29, &got);
		const uint32_t exp = livearm::pack(want);
		*d += std::string(" ") + dr.file + " " + csarm::hex(got) + " want " + csarm::hex(exp);   // ARGB
		CHECK(!livearm::far(got, exp, 2), std::string("[") + dr.file + ": native pixel != DXVK's (oracle) within 2/255]");
		if (k == 0) {   // the glare draw discriminates: a [-1,1] clamp after every instruction gives another pixel
			CHECK(livearm::run(ent->dwords, ent->count, tv, v0, c, true, &wrong, &why) && livearm::far(livearm::pack(wrong), exp, 8),
			      "[the glare draw does not tell DXVK's semantics from a clamping port]");
			CHECK(livearm::far(got, livearm::pack(wrong), 8), "[native glare = the clamping semantics]");
		}
		if (std::strcmp(dr.file, "water:foam") == 0) {   // ...and the foam draw: r1.b's replicate read as r1.a
			std::vector<uint32_t> alt(ent->dwords, ent->dwords + ent->count);
			bool swapped = false;
			for (uint32_t& w : alt)
				if (w == 0x80aa0001u && !swapped) {
					w = 0x80ff0001u;
					swapped = true;
				}
			CHECK(swapped && livearm::run(alt.data(), alt.size(), tv, v0, c, false, &wrong, &why) && livearm::far(livearm::pack(wrong), exp, 8) &&
			      livearm::far(got, livearm::pack(wrong), 8), "[the foam draw does not tell r1.b from r1.a]");
		}
		if (std::strcmp(dr.file, "water:ra3river") == 0) {   // ...and the river sheen draw: v0 read instead of v0.a (the fade lost)
			std::vector<uint32_t> alt(ent->dwords, ent->dwords + ent->count);
			int swz = 0;
			for (uint32_t& w : alt)
				if (w == 0x90FF0000u) {
					w = 0x90E40000u;
					++swz;
				}
			CHECK(swz == 1 && livearm::run(alt.data(), alt.size(), tv, v0, c, false, &wrong, &why) && livearm::far(livearm::pack(wrong), exp, 8) &&
			      livearm::far(got, livearm::pack(wrong), 8), "[the river sheen draw does not tell v0.a from v0]");
		}
		if (std::strcmp(dr.file, "water:ra3") == 0) {   // ...and the sheen draw: the normals read without _bx2 (never centred)
			std::vector<uint32_t> alt(ent->dwords, ent->dwords + ent->count);
			int plain = 0;
			for (uint32_t& w : alt)
				if ((w & 0xFFFF0000u) == 0xB4E40000u) {
					w = (w & 0x0000FFFFu) | 0xB0E40000u;
					++plain;
				}
			CHECK(plain == 2 && livearm::run(alt.data(), alt.size(), tv, v0, c, false, &wrong, &why) && livearm::far(livearm::pack(wrong), exp, 8) &&
			      livearm::far(got, livearm::pack(wrong), 8), "[the sheen draw does not tell t_bx2 from t]");
		}
		++k;
	}
	const auto s1 = e.nd->translator().stats();
	const uint64_t drawn = s1.drawn - s0.drawn, refused = s1.refused - s0.refused;
	*d += ", drawn " + std::to_string(drawn) + " refused " + std::to_string(refused);
	CHECK(drawn == drawsMade && refused == 0, "[every draw recorded, none refused]");
	for (int s = 0; s < 4; ++s) {
		e.dev->SetTexture(s, nullptr);
		tex[s]->Release();
	}
	for (DWORD h : handle)
		if (h)
			e.dev->DeletePixelShader(h);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	bb->Release();
	return ok;
}

// ------------------------------------------------------------------ rw3-X-3: the A23-B merge gate's arms (PREREG A23-B amendment 5)
namespace csarm {
// --strict's "obs" digest: what a STRICT arm saw come back (nothing without --strict). g_trInputSink is only ever an ExecDigest.
void obs(const void* p, size_t n)
{
	if (GXRender::Rec::GxTranslateInputSink* s = GXRender::Rec::g_trInputSink.load())
		static_cast<ExecDigest*>(s)->observe(p, n);
}
template <class T>
void obsv(const T& v) { obs(&v, sizeof(v)); }
// the whole surface, read back through the game's own path, into obs (rows tight); false = the read-back failed (also observed)
bool obsSurface(Env& e, IDirect3DSurface8* src)
{
	if (!GXRender::Rec::g_trInputSink.load())
		return true;
	D3DSURFACE_DESC sd = {};
	IDirect3DSurface8* img = nullptr;
	bool ok = src && SUCCEEDED(src->GetDesc(&sd)) && SUCCEEDED(e.dev->CreateImageSurface(sd.Width, sd.Height, sd.Format, &img)) &&
		SUCCEEDED(e.dev->CopyRects(src, nullptr, 0, img, nullptr));
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		for (UINT y = 0; y < sd.Height; ++y)
			obs(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(sd.Width) * 4);
		img->UnlockRect();
	} else
		ok = false;
	if (img)
		img->Release();
	obsv(ok);
	return ok;
}
} // namespace csarm

// The STRICT value sweep (A23-B merge gate item 2; gx_sweep_arms.h, PREREG A23-B amendment 5 (4)): four serial arms, every result
// into --strict's obs digest. The shader arms take the catalogue's Trees.vso (ported, with its declaration) and a ported PS.
bool sweepShaders(const DWORD** decl, const DWORD** vs, const DWORD** ps)
{
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	*decl = *vs = *ps = nullptr;
	for (size_t i = 0; i < n; ++i) {
		if (!*vs && !all[i].pixel && all[i].program && all[i].decl && std::strstr(all[i].files, "Trees.vso")) {
			*vs = reinterpret_cast<const DWORD*>(all[i].dwords);
			*decl = reinterpret_cast<const DWORD*>(all[i].decl);
		}
		if (!*ps && all[i].pixel && all[i].program)
			*ps = reinterpret_cast<const DWORD*>(all[i].dwords);
	}
	return *vs && *ps;
}
// the device's own accounting at every sweep read-back: the deterministic counters only (present
// skips / acquires and idle flushes follow timing). The same list is in the base binary's wrapper (base-sweep-arms.patch).
void sweepCounters(void* ctx, gxsweep::S& s)
{
	const GxNativeStats& st = static_cast<GxNativeDevice*>(ctx)->stats();
	for (uint64_t v : { st.creates, st.createsRefused, st.draws, st.drawsRefused, st.clears, st.presents, st.neverCalled, st.gammaIgnored,
			 st.resourceErrors, st.copiesRefused, st.targetsRefused, st.targetSizeRefused, st.liveTextures, st.liveSurfaces, st.liveBuffers,
			 st.readbacks, st.gpuCopies, st.resolves })
		s.o(v);
}
gxsweep::Hooks sweepHooks(Env& e)
{
	gxsweep::Hooks h;
	h.obs = csarm::obs;
	h.counters = sweepCounters;
	h.ctx = e.nd;
	return h;
}
bool caseStrictSweepState(Env& e, std::string* d) { return gxsweep::sweepState(e.dev, sweepHooks(e), d); }
bool caseStrictSweepDraw(Env& e, std::string* d) { return gxsweep::sweepDraw(e.dev, sweepHooks(e), d); }
bool caseStrictSweepRes(Env& e, std::string* d)
{
	bool ok = true;
	const DWORD *decl, *vs, *ps;
	SETUP(sweepShaders(&decl, &vs, &ps), "Trees.vso and a ported PS in the catalogue");
	return gxsweep::sweepRes(e.dev, sweepHooks(e), d, decl, vs, ps) && ok;
}
bool caseStrictSweepMisc(Env& e, std::string* d)
{
	bool ok = true;
	const DWORD *decl, *vs, *ps;
	SETUP(sweepShaders(&decl, &vs, &ps), "Trees.vso and a ported PS in the catalogue");
	return gxsweep::sweepMisc(e.dev, sweepHooks(e), d, decl, vs, ps) && ok;
}

// CreateVertexShader / CreatePixelShader over the worker's largest command (the arm's ring is 64 KiB: 16 KiB -
// 256) take their S path. Trees.vso and a ported PS, each enlarged by a 4100-DWORD comment after the version token (valid: funcLen
// and the parsers skip comments), answer D3D_OK with a handle as today; at CS=1 each creation is an S transition; the translator
// holds a record for each (its catalogue verdict exists) - and --strict compares the received token bytes through trin.
// X-strict-shader-oversize: the oversize branch refuses the creation (INVALIDCALL). STRICT (serial).
bool caseStrictShaderOversize(Env& e, std::string* d)
{
	bool ok = true;
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	const GXRender::GxCsEntry *vsE = nullptr, *psE = nullptr;
	for (size_t i = 0; i < n; ++i) {
		if (!vsE && !all[i].pixel && all[i].program && all[i].decl && std::strstr(all[i].files, "Trees.vso"))
			vsE = &all[i];
		if (!psE && all[i].pixel && all[i].program)
			psE = &all[i];
	}
	SETUP(vsE && psE, "Trees.vso and a ported PS in the catalogue");
	auto enlarge = [](const GXRender::GxCsEntry* x) {
		std::vector<DWORD> v;
		v.push_back(x->dwords[0]);   // the version token
		v.push_back(0x0000FFFEu | (4100u << 16));   // a comment of 4100 DWORDs
		for (uint32_t i = 0; i < 4100; ++i)
			v.push_back(0x41414100u + (i & 0xff));
		v.insert(v.end(), x->dwords + 1, x->dwords + x->count);
		return v;
	};
	const std::vector<DWORD> vsCode = enlarge(vsE), psCode = enlarge(psE);
	const std::vector<DWORD> decl(vsE->decl, vsE->decl + vsE->declCount);
	SETUP(vsCode.size() * 4 > 16384 && psCode.size() * 4 > 16384, "both streams over the 64 KiB ring's largest command");
	const uint64_t s0 = e.nd->csSyncCount();
	DWORD hv = 0, hp = 0;
	const HRESULT rv = e.dev->CreateVertexShader(decl.data(), vsCode.data(), &hv, 0);
	const uint64_t s1 = e.nd->csSyncCount();
	const HRESULT rp = e.dev->CreatePixelShader(psCode.data(), &hp);
	const uint64_t s2 = e.nd->csSyncCount();
	e.nd->csSync();
	const uint8_t verdictV = e.nd->translator().testVsVerdict(hv), verdictP = e.nd->translator().testPsVerdict(hp);
	csarm::obsv(rv);
	csarm::obsv(rp);
	csarm::obsv(hv);
	csarm::obsv(hp);
	csarm::obsv(verdictV);
	csarm::obsv(verdictP);
	*d += "VS " + std::to_string(vsCode.size()) + " DWORDs hr " + std::to_string(long(rv)) + " S +" + std::to_string(s1 - s0) + " verdict " +
		std::to_string(int(verdictV)) + "; PS " + std::to_string(psCode.size()) + " DWORDs hr " + std::to_string(long(rp)) + " S +" +
		std::to_string(s2 - s1) + " verdict " + std::to_string(int(verdictP));
	CHECK(rv == D3D_OK && hv != 0 && rp == D3D_OK && hp != 0, "both oversize creations answered D3D_OK and a handle, as today");
	CHECK(verdictV != 255 && verdictP != 255, "the translator received both shaders");
	if (e.nd->csOn())
		CHECK(s1 - s0 >= 1 && s2 - s1 >= 1, "at CS=1 each oversize creation went S");
	if (rv == D3D_OK)
		e.dev->DeleteVertexShader(hv);
	if (rp == D3D_OK)
		e.dev->DeletePixelShader(hp);
	return ok;
}

// ============================ rw3-X-3: Lock/Unlock + CopyRects census (PREREG A23-B amendment 5 (13))
namespace csarm2 {
// the 64x64 back buffer read back whole into px (and --strict's obs)
bool readBB(Env& e, std::vector<uint32_t>* px)
{
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* img = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	bool ok = bb && SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &img)) && img && SUCCEEDED(e.dev->CopyRects(bb, nullptr, 0, img, nullptr));
	px->assign(64 * 64, 0);
	D3DLOCKED_RECT lr = {};
	if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		for (int y = 0; y < 64; ++y)
			std::memcpy(&(*px)[size_t(y) * 64], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 256);
		img->UnlockRect();
	} else
		ok = false;
	if (img)
		img->Release();
	if (bb)
		bb->Release();
	csarm::obs(px->data(), px->size() * 4);
	return ok;
}
// texels [u0, u0+64) x [v0, v0+64) of level `level` (lw x lh) drawn 1:1 with point sampling over a black back buffer
void drawTex(Env& e, IDirect3DBaseTexture8* t, UINT lw, UINT lh, UINT u0, UINT v0, DWORD level = 0)
{
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	clearSurface(e.dev, bb, 0xff000000);
	if (bb)
		bb->Release();
	IDirect3DDevice8* d = e.dev;
	d->SetRenderState(D3DRS_ZENABLE, FALSE);
	d->SetRenderState(D3DRS_LIGHTING, FALSE);
	d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	d->SetTextureStageState(0, D3DTSS_MAXMIPLEVEL, level);
	d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	d->SetTexture(0, t);
	d->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const UINT w = std::min<UINT>(64, lw - u0), h = std::min<UINT>(64, lh - v0);
	struct V { float x, y, z, rhw, u, v; };
	const float ua = float(u0) / lw, ub = float(u0 + w) / lw, va = float(v0) / lh, vb = float(v0 + h) / lh;
	const V q[4] = { { -0.5f, -0.5f, 0.5f, 1, ua, va }, { w - 0.5f, -0.5f, 0.5f, 1, ub, va }, { -0.5f, h - 0.5f, 0.5f, 1, ua, vb },
		{ w - 0.5f, h - 0.5f, 0.5f, 1, ub, vb } };
	d->BeginScene();
	d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	d->EndScene();
	d->SetTexture(0, nullptr);
	d->SetTextureStageState(0, D3DTSS_MAXMIPLEVEL, 0);
}
// a level's bytes in its own layout: pixels (A8R8G8B8 4, R5G6B5 2 bytes) or DXT1 4x4 blocks (8 bytes, solid colour: c0 > c1, indices 0)
struct Lvl
{
	UINT w = 0, h = 0, bpe = 4, ew = 0, eh = 0;
	D3DFORMAT f = D3DFMT_A8R8G8B8;
	std::vector<uint8_t> b;
	Lvl(UINT w_, UINT h_, D3DFORMAT f_) : w(w_), h(h_), f(f_)
	{
		const bool dxt = f == D3DFMT_DXT1;
		bpe = dxt ? 8 : f == D3DFMT_R5G6B5 ? 2 : 4;
		ew = dxt ? (w + 3) / 4 : w;
		eh = dxt ? (h + 3) / 4 : h;
		b.assign(size_t(ew) * eh * bpe, 0);
	}
	uint8_t* at(UINT ex, UINT ey) { return &b[(size_t(ey) * ew + ex) * bpe]; }
	void fill(uint32_t seed)
	{
		for (UINT y = 0; y < eh; ++y)
			for (UINT x = 0; x < ew; ++x) {
				uint8_t* e = at(x, y);
				const uint32_t v = (x * 2654435761u) ^ (y * 40503u) ^ (seed * 977u);
				if (f == D3DFMT_DXT1) {
					uint16_t c0 = uint16_t((v >> 3) | 0x0841);
					const uint16_t c1 = uint16_t(c0 - 1);
					std::memcpy(e, &c0, 2);
					std::memcpy(e + 2, &c1, 2);
					std::memset(e + 4, 0, 4);
				} else if (bpe == 2) {
					const uint16_t c = uint16_t(v >> 7);
					std::memcpy(e, &c, 2);
				} else {
					const uint32_t c = 0xff000000u | (v >> 8);
					std::memcpy(e, &c, 4);
				}
			}
	}
	// elements of src's rect (DXT1: block granularity, the source offset truncated as today's host snapshot does) to (dx, dy)
	void copyFrom(Lvl& src, RECT r, POINT pt)
	{
		const UINT g = f == D3DFMT_DXT1 ? 4 : 1;
		const UINT sx = UINT(r.left) / g, sy = UINT(r.top) / g, nw = (UINT(r.right - r.left) + g - 1) / g, nh = (UINT(r.bottom - r.top) + g - 1) / g;
		for (UINT j = 0; j < nh; ++j)
			for (UINT i = 0; i < nw; ++i)
				std::memcpy(at(UINT(pt.x) / g + i, UINT(pt.y) / g + j), src.at(sx + i, sy + j), bpe);
	}
	bool write(const D3DLOCKED_RECT& lr) const
	{
		for (UINT y = 0; y < eh; ++y)
			std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, &b[size_t(y) * ew * bpe], size_t(ew) * bpe);
		return true;
	}
};
IDirect3DSurface8* image(Env& e, const Lvl& l)
{
	IDirect3DSurface8* s = nullptr;
	if (FAILED(e.dev->CreateImageSurface(l.w, l.h, l.f, &s)) || !s)
		return nullptr;
	D3DLOCKED_RECT lr = {};
	if (SUCCEEDED(s->LockRect(&lr, nullptr, 0))) {
		l.write(lr);
		s->UnlockRect();
	}
	return s;
}
// level `lvl` of t and of a MANAGED reference holding `want` drawn 1:1 tile by tile: the number of pixels that differ (-1 = no draw)
int compareDrawn(Env& e, IDirect3DTexture8* t, UINT levels, UINT lvl, const Lvl& want)
{
	IDirect3DTexture8* ref = nullptr;
	if (FAILED(e.dev->CreateTexture(want.w << lvl, want.h << lvl, levels, 0, want.f, D3DPOOL_MANAGED, &ref)) || !ref)
		return -1;
	D3DLOCKED_RECT lr = {};
	if (SUCCEEDED(ref->LockRect(lvl, &lr, nullptr, 0))) {
		want.write(lr);
		ref->UnlockRect(lvl);
	}
	int bad = 0;
	std::vector<uint32_t> a, b;
	for (UINT v0 = 0; v0 < want.h; v0 += 64)
		for (UINT u0 = 0; u0 < want.w; u0 += 64) {
			drawTex(e, t, want.w, want.h, u0, v0, lvl);
			readBB(e, &a);
			drawTex(e, ref, want.w, want.h, u0, v0, lvl);
			readBB(e, &b);
			for (size_t i = 0; i < a.size(); ++i)
				bad += a[i] != b[i];
		}
	ref->Release();
	return bad;
}
} // namespace csarm2

// + P2-8: buffer ranges. Static MANAGED VB / IB partial uploads at NONZERO offsets, small (queued) and over the
// 64 KiB ring's largest command (S), each drawn; a static READONLY lock / unlock uploads nothing (staging unchanged) while the same
// writing cycle uploads; a dynamic Lock(offset, 0) covers the bytes after the offset; DISCARD | NOOVERWRITE appends (the earlier
// quad survives); the plain-lock S path keeps the bytes it did not write. Plants (one Case row each): csUploadOffsetZero,
// csPlainLockForwardsDiscard, GxDebug readonlyUploads / lockSizeWhole / keepDiscardWithNoOverwrite. STRICT (serial).
bool caseStrictBufferRanges(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb, "the back buffer");
	csarm::ffDiffuse(e);
	auto quadAt = [](QV* v, int cell, DWORD c) {   // a 16x16 cell of the 4x4 grid; cell < 0 = off screen
		const float x0 = cell < 0 ? -100.f : float((cell % 4) * 16), y0 = cell < 0 ? -100.f : float((cell / 4) * 16);
		csarm::quad(v, x0, y0, x0 + 16, y0 + 16, c);
	};
	std::vector<uint32_t> px;
	auto cell = [&](int c) { return px[size_t((c / 4) * 16 + 8) * 64 + size_t((c % 4) * 16 + 8)] & 0xffffff; };
	BYTE* p = nullptr;
	// (1) static VB: 800 quads off screen; quad 5 (80 bytes) and quads 50-299 (20 000 bytes)
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(64000, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "a 64 000-byte static VB");
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "the VB locked whole");
	for (int q = 0; q < 800; ++q)
		quadAt(reinterpret_cast<QV*>(p) + q * 4, -1, 0xff000000);
	vb->Unlock();
	const uint64_t s0 = e.nd->csSyncCount();
	const HRESULT h1 = vb->Lock(80 * 5, 80, &p, 0);
	if (SUCCEEDED(h1) && p)
		quadAt(reinterpret_cast<QV*>(p), 0, 0xffff0000);
	vb->Unlock();
	const uint64_t s1 = e.nd->csSyncCount();
	const HRESULT h2 = vb->Lock(80 * 50, 80 * 250, &p, 0);
	if (SUCCEEDED(h2) && p)
		for (int q = 0; q < 250; ++q)
			quadAt(reinterpret_cast<QV*>(p) + q * 4, q == 0 ? 2 : q == 249 ? 1 : -1, q == 0 ? 0xff0000ff : 0xff00ff00);
	vb->Unlock();
	const uint64_t s2 = e.nd->csSyncCount();
	clearSurface(e.dev, bb, 0xff000000);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 5 * 4, 2);
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 299 * 4, 2);
	e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 50 * 4, 2);
	e.dev->EndScene();
	csarm2::readBB(e, &px);
	*d += "static VB: cells " + csarm::hex(cell(0)) + "/" + csarm::hex(cell(1)) + "/" + csarm::hex(cell(2)) + " S " + std::to_string(s1 - s0) + "+" +
		std::to_string(s2 - s1) + ";";
	CHECK(SUCCEEDED(h1) && SUCCEEDED(h2) && cell(0) == 0xff0000 && cell(1) == 0x00ff00 && cell(2) == 0x0000ff && cell(5) == 0,
		"static VB partial uploads land at their offsets (small and oversize)");
	if (e.nd->csOn())
		CHECK(s1 == s0 && s2 > s1, "the small static upload queued, the oversize one went S");
	// (2) static IB: zeros; indices at 600 (12 bytes) -> quad 5, at 1000 (20 000 bytes) -> quad 50
	IDirect3DIndexBuffer8* ib = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateIndexBuffer(64000, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib)) && ib, "a 64 000-byte static IB");
	SETUP(SUCCEEDED(ib->Lock(0, 0, &p, 0)) && p, "the IB locked whole");
	std::memset(p, 0, 64000);
	ib->Unlock();
	const uint16_t q5[6] = { 20, 21, 22, 22, 21, 23 }, q50[6] = { 200, 201, 202, 202, 201, 203 };
	const HRESULT h3 = ib->Lock(2 * 600, 12, &p, 0);
	if (SUCCEEDED(h3) && p)
		std::memcpy(p, q5, 12);
	ib->Unlock();
	const HRESULT h4 = ib->Lock(2 * 1000, 20000, &p, 0);
	if (SUCCEEDED(h4) && p) {
		std::memset(p, 0, 20000);
		std::memcpy(p, q50, 12);
	}
	ib->Unlock();
	clearSurface(e.dev, bb, 0xff000000);
	e.dev->SetIndices(ib, 0);
	e.dev->BeginScene();
	e.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3200, 600, 2);
	e.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3200, 1000, 2);
	e.dev->EndScene();
	e.dev->SetIndices(nullptr, 0);
	csarm2::readBB(e, &px);
	*d += " static IB: cells " + csarm::hex(cell(0)) + "/" + csarm::hex(cell(1)) + "/" + csarm::hex(cell(2)) + ";";
	CHECK(SUCCEEDED(h3) && SUCCEEDED(h4) && cell(0) == 0xff0000 && cell(2) == 0x0000ff && cell(1) == 0, "static IB partial uploads land at their offsets");
	// (3) a READONLY cycle uploads nothing; the same writing cycle does
	e.nd->csSync();
	const uint64_t st0 = e.nd->resources().stats().stagingBytes;
	vb->Lock(80 * 7, 80, &p, D3DLOCK_READONLY);
	vb->Unlock();
	e.nd->csSync();
	const uint64_t st1 = e.nd->resources().stats().stagingBytes;
	if (SUCCEEDED(vb->Lock(80 * 7, 80, &p, 0)) && p)
		quadAt(reinterpret_cast<QV*>(p), -1, 0xff000000);
	vb->Unlock();
	e.nd->csSync();
	const uint64_t st2 = e.nd->resources().stats().stagingBytes;
	csarm::obsv(st1 - st0);
	csarm::obsv(st2 - st1);
	*d += " READONLY staged +" + std::to_string(st1 - st0) + ", writing +" + std::to_string(st2 - st1) + ";";
	CHECK(st1 == st0 && st2 > st1, "a static READONLY unlock uploads nothing, a writing one does");
	e.dev->SetStreamSource(0, nullptr, 0);
	vb->Release();
	ib->Release();
	// (4)-(6) a dynamic VB of 100 quads
	IDirect3DVertexBuffer8* dv = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(8000, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &dv)) && dv, "a dynamic VB");
	auto fillOff = [&](BYTE* q, int n) { for (int i = 0; i < n; ++i) quadAt(reinterpret_cast<QV*>(q) + i * 4, -1, 0xff000000); };
	// halve = the second draw of a frame: diffuse x TEXTUREFACTOR 0x80 - its pixels differ from the first draw's, so a whole-target
	// Clear between the two is not needed (a Clear after a draw in one frame also removes later draws: pre-existing, base the same)
	auto drawQuads = [&](std::initializer_list<int> qs, bool halve = false) {
		e.dev->SetStreamSource(0, dv, sizeof(QV));
		if (halve) {
			e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xff808080);
			e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
		}
		e.dev->BeginScene();
		for (int q : qs)
			e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, UINT(q) * 4, 2);
		e.dev->EndScene();
		if (halve) {
			e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		}
	};
	SETUP(SUCCEEDED(dv->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "the dynamic VB discarded");
	fillOff(p, 100);
	dv->Unlock();
	// (4) Lock(800, 0): the 7200 bytes after the offset - quad 10 (cell 3) and quad 99 (cell 4)
	const HRESULT h5 = dv->Lock(800, 0, &p, D3DLOCK_NOOVERWRITE);
	if (SUCCEEDED(h5) && p) {
		quadAt(reinterpret_cast<QV*>(p), 3, 0xffffff00);
		quadAt(reinterpret_cast<QV*>(p) + 89 * 4, 4, 0xff00ffff);
	}
	dv->Unlock();
	clearSurface(e.dev, bb, 0xff000000);
	drawQuads({ 10, 99 });
	csarm2::readBB(e, &px);
	*d += " Lock(800,0) hr " + std::to_string(long(h5)) + " cells " + csarm::hex(cell(3)) + "/" + csarm::hex(cell(4)) + ";";
	CHECK(SUCCEEDED(h5) && cell(3) == 0xffff00 && cell(4) == 0x00ffff, "Lock(offset, 0) covers the bytes after the offset");
	// (5) DISCARD then DISCARD | NOOVERWRITE: an append - quad 0 (cell 5) survives next to quad 1 (cell 6)
	SETUP(SUCCEEDED(dv->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "discard");
	fillOff(p, 100);
	quadAt(reinterpret_cast<QV*>(p), 5, 0xffff00ff);
	dv->Unlock();
	clearSurface(e.dev, bb, 0xff000000);
	drawQuads({ 0 });
	e.nd->csSync();
	const GXRender::Res::GxStats g5 = e.nd->resources().stats();
	const HRESULT h6 = dv->Lock(80, 80, &p, D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
	if (SUCCEEDED(h6) && p)
		quadAt(reinterpret_cast<QV*>(p), 6, 0xffffffff);
	dv->Unlock();
	drawQuads({ 0, 1 }, true);
	csarm2::readBB(e, &px);
	const GXRender::Res::GxStats g6 = e.nd->resources().stats();
	*d += " DISCARD|NOOVERWRITE cells " + csarm::hex(cell(5)) + "/" + csarm::hex(cell(6)) + " slices +" + std::to_string(g6.slicesCreated - g5.slicesCreated) + " reused +" +
		std::to_string(g6.sliceReuses - g5.sliceReuses) + ";";
	CHECK(SUCCEEDED(h6) && cell(5) == 0x800080 && cell(6) == 0x808080, "DISCARD | NOOVERWRITE appends: the earlier quad survives (drawn again, halved)");
	// (6) the plain-lock S path: quads 0 (cell 7) and 1 (cell 8) in flight, a plain lock rewrites quad 1 - quad 0 must survive
	SETUP(SUCCEEDED(dv->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "discard");
	fillOff(p, 100);
	quadAt(reinterpret_cast<QV*>(p), 7, 0xffff8000);
	quadAt(reinterpret_cast<QV*>(p) + 4, 8, 0xff808080);
	dv->Unlock();
	clearSurface(e.dev, bb, 0xff000000);
	drawQuads({ 0, 1 });
	const uint64_t s3 = e.nd->csSyncCount();
	e.nd->csSync();
	const GXRender::Res::GxStats g7 = e.nd->resources().stats();
	const HRESULT h7 = dv->Lock(80, 80, &p, 0);
	if (SUCCEEDED(h7) && p)
		quadAt(reinterpret_cast<QV*>(p), 8, 0xff808000);
	dv->Unlock();
	const uint64_t s4 = e.nd->csSyncCount();
	drawQuads({ 0, 1 }, true);
	csarm2::readBB(e, &px);
	const GXRender::Res::GxStats g8 = e.nd->resources().stats();
	*d += " plain lock S +" + std::to_string(s4 - s3) + " cells " + csarm::hex(cell(7)) + "/" + csarm::hex(cell(8)) + " slices +" +
		std::to_string(g8.slicesCreated - g7.slicesCreated) + " reused +" + std::to_string(g8.sliceReuses - g7.sliceReuses) + ";";
	CHECK(SUCCEEDED(h7) && cell(7) == 0x804000 && cell(8) == 0x404000, "the plain lock keeps the bytes it did not write (drawn again, halved)");
	if (e.nd->csOn())
		CHECK(s4 > s3, "the plain lock of a dynamic buffer in flight went S");
	e.dev->SetStreamSource(0, nullptr, 0);
	dv->Release();
	bb->Release();
	return ok;
}

// partial MANAGED texture uploads at NONZERO rectangles - a 32x32 level's (5,3)-(13,9) (192 bytes: queued), a
// 128x128 level's (16,8)-(112,72) (24 576 bytes: S), a 64x64 DXT1 level's block rectangle (8,4)-(24,20) - read through the GPU (drawn
// 1:1): the patch holds the new texels and every other texel the old ones. Plant csRectPitchSwap (each path takes the other's pitch).
// strict-texture-partial-nobc runs it with BC unavailable (DXT1 decoded on upload). STRICT (serial).
bool caseStrictTexturePartial(Env& e, std::string* d)
{
	bool ok = true;
	auto run = [&](UINT tw, D3DFORMAT f, RECT pr, const char* name) {
		IDirect3DTexture8* t = nullptr;
		if (FAILED(e.dev->CreateTexture(tw, tw, 1, 0, f, D3DPOOL_MANAGED, &t)) || !t) {
			CHECK(false, std::string(name) + ": the texture");
			return uint64_t(0);
		}
		csarm2::Lvl old(tw, tw, f), want(tw, tw, f), patch(UINT(pr.right - pr.left), UINT(pr.bottom - pr.top), f);
		old.fill(1);
		patch.fill(2);
		want = old;
		want.copyFrom(patch, { 0, 0, pr.right - pr.left, pr.bottom - pr.top }, { pr.left, pr.top });
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
			old.write(lr);
			t->UnlockRect(0);
		}
		const uint64_t s0 = e.nd->csSyncCount();
		const HRESULT hl = t->LockRect(0, &lr, &pr, 0);
		if (SUCCEEDED(hl)) {
			patch.write(lr);
			t->UnlockRect(0);
		}
		const uint64_t syncs = e.nd->csSyncCount() - s0;
		const int bad = csarm2::compareDrawn(e, t, 1, 0, want);
		*d += std::string(" ") + name + ": hr " + std::to_string(long(hl)) + " S +" + std::to_string(syncs) + " texels wrong " + std::to_string(bad) + ";";
		CHECK(SUCCEEDED(hl) && bad == 0, std::string(name) + ": the patch holds the new texels, the rest the old ones");
		t->Release();
		return syncs;
	};
	run(32, D3DFMT_A8R8G8B8, { 5, 3, 13, 9 }, "32x32 partial");
	const uint64_t sy = run(128, D3DFMT_A8R8G8B8, { 16, 8, 112, 72 }, "128x128 partial (oversize)");
	run(64, D3DFMT_DXT1, { 8, 4, 24, 20 }, "DXT1 64x64 block partial");
	if (e.nd->csOn())
		CHECK(sy >= 1, "the oversize partial upload went S");
	return ok;
}

// + P2-4: host -> DEFAULT copies beyond square A8R8G8B8 images into level 0 - R5G6B5, a 4x2 extent, a
// destination mip 1, SYSTEMMEM and MANAGED texture-level sources (queued), and R5G6B5 / 128x32 / mip-1 copies over the arm's
// largest command (S); DXT1: a destination at (2,4) is refused (the level unchanged), an off-grid source rect (2,0)-(6,4) is
// accepted with today's truncated block. Each destination is filled first (a whole-level host copy), then compared texel for texel
// with a MANAGED reference holding the expected level, both drawn 1:1. Plants (one Case row each): csHostCopyTwinA8 / Square /
// Level0, csCopySkipDestBlockCheck, csCopyValidateSource. STRICT (serial, 64 KiB ring).
// DXT1 MANAGED level sources too (8x8 queued, 256x128 over the largest command: S), and the whole table again with BC
// unavailable (strict-host-copy-nobc: a DXT1 destination is decoded A8R8G8B8 underneath; X-: csHostCopyTwinPhysFmt). The refused row
// also asserts that no device counter moves (X-cs-host-copy-refused-count: csFrontRefusalCounts).
bool hostCopyCases(Env& e, std::string* d, bool refusedOnly)
{
	bool ok = true;
	struct HC { const char* name; D3DFORMAT f; UINT tw, th, levels, lvl; int src; UINT sw, sh; RECT r; POINT pt; bool refused; bool big; };
	// src: 0 image surface, 1 SYSTEMMEM texture level, 2 MANAGED texture level
	const HC cases[] = {
		{ "R5G6B5", D3DFMT_R5G6B5, 16, 16, 1, 0, 0, 8, 8, { 1, 1, 7, 5 }, { 4, 9 }, false, false },
		{ "4x2 extent", D3DFMT_A8R8G8B8, 16, 16, 1, 0, 0, 8, 8, { 1, 2, 5, 4 }, { 6, 9 }, false, false },
		{ "mip 1", D3DFMT_A8R8G8B8, 16, 16, 2, 1, 0, 4, 4, { 0, 0, 4, 4 }, { 2, 1 }, false, false },
		{ "SYSTEMMEM level source", D3DFMT_A8R8G8B8, 16, 16, 1, 0, 1, 8, 8, { 0, 0, 4, 4 }, { 8, 8 }, false, false },
		{ "MANAGED level source", D3DFMT_A8R8G8B8, 16, 16, 1, 0, 2, 8, 8, { 2, 2, 6, 6 }, { 2, 10 }, false, false },
		{ "R5G6B5 oversize", D3DFMT_R5G6B5, 128, 128, 1, 0, 0, 128, 64, { 0, 0, 128, 64 }, { 0, 32 }, false, true },
		{ "128x32 oversize", D3DFMT_A8R8G8B8, 128, 64, 1, 0, 0, 128, 32, { 0, 0, 128, 32 }, { 0, 16 }, false, true },
		{ "mip 1 oversize", D3DFMT_A8R8G8B8, 256, 256, 2, 1, 0, 64, 64, { 0, 0, 64, 64 }, { 32, 32 }, false, true },
		{ "DXT1 destination (2,4)", D3DFMT_DXT1, 16, 16, 1, 0, 0, 8, 8, { 0, 0, 8, 8 }, { 2, 4 }, true, false },
		{ "DXT1 off-grid source", D3DFMT_DXT1, 16, 16, 1, 0, 0, 8, 8, { 2, 0, 6, 4 }, { 4, 4 }, false, false },
		{ "DXT1 MANAGED level source", D3DFMT_DXT1, 16, 16, 1, 0, 2, 8, 8, { 0, 0, 8, 8 }, { 4, 8 }, false, false },
		{ "DXT1 MANAGED level oversize", D3DFMT_DXT1, 256, 256, 1, 0, 2, 256, 128, { 0, 0, 256, 128 }, { 0, 128 }, false, true },
	};
	for (const HC& c : cases) {
		if (c.refused != refusedOnly)
			continue;
		const UINT lw = c.tw >> c.lvl, lh = c.th >> c.lvl;
		IDirect3DTexture8* t = nullptr;
		IDirect3DSurface8* lv = nullptr;
		if (FAILED(e.dev->CreateTexture(c.tw, c.th, c.levels, 0, c.f, D3DPOOL_DEFAULT, &t)) || !t || FAILED(t->GetSurfaceLevel(c.lvl, &lv)) || !lv) {
			CHECK(false, std::string(c.name) + ": the destination");
			if (t)
				t->Release();
			continue;
		}
		csarm2::Lvl bg(lw, lh, c.f), src(c.sw, c.sh, c.f);
		bg.fill(3);
		src.fill(4);
		IDirect3DSurface8* bgImg = csarm2::image(e, bg);
		const HRESULT hb = bgImg ? e.dev->CopyRects(bgImg, nullptr, 0, lv, nullptr) : E_FAIL;
		IDirect3DSurface8* srcSurf = nullptr;
		IDirect3DTexture8* srcTex = nullptr;
		if (c.src == 0)
			srcSurf = csarm2::image(e, src);
		else if (SUCCEEDED(e.dev->CreateTexture(c.sw, c.sh, 1, 0, c.f, c.src == 1 ? D3DPOOL_SYSTEMMEM : D3DPOOL_MANAGED, &srcTex)) && srcTex) {
			D3DLOCKED_RECT lr = {};
			if (SUCCEEDED(srcTex->LockRect(0, &lr, nullptr, 0))) {
				src.write(lr);
				srcTex->UnlockRect(0);
			}
			srcTex->GetSurfaceLevel(0, &srcSurf);
		}
		GxNativeStats st0 = {};
		if (c.refused)
			st0 = e.nd->stats();   // a drain (S): the refused row only (an ASYNC arm, no digest)
		const uint64_t s0 = e.nd->csSyncCount();
		const HRESULT hc = srcSurf ? e.dev->CopyRects(srcSurf, &c.r, 1, lv, &c.pt) : E_FAIL;
		const uint64_t syncs = e.nd->csSyncCount() - s0;
		if (c.refused) {   // today (CS=0) the refusal counts nothing: every device counter unchanged, in both modes
			const GxNativeStats st1 = e.nd->stats();
			static_assert(sizeof(GxNativeStats) % sizeof(uint64_t) == 0, "GxNativeStats: 64-bit counters only");
			uint64_t a[sizeof(GxNativeStats) / sizeof(uint64_t)], b[sizeof(a) / sizeof(uint64_t)];
			std::memcpy(a, &st0, sizeof(a));
			std::memcpy(b, &st1, sizeof(b));
			std::string moved;
			for (size_t i = 0; i < sizeof(a) / sizeof(uint64_t); ++i)
				if (a[i] != b[i])
					moved += " #" + std::to_string(i) + " " + std::to_string(a[i]) + "->" + std::to_string(b[i]);
			*d += " counters moved:" + (moved.empty() ? std::string(" none") : moved) + ";";
			CHECK(moved.empty(), std::string(c.name) + ": no device counter moved (copiesRefused " + std::to_string(st1.copiesRefused) + ")");
		}
		csarm2::Lvl want = bg;
		if (!c.refused)
			want.copyFrom(src, c.r, c.pt);
		const int bad = csarm2::compareDrawn(e, t, c.levels, c.lvl, want);
		csarm::obsv(hc);
		*d += std::string(" ") + c.name + ": hr " + std::to_string(long(hc)) + " S +" + std::to_string(syncs) + " wrong " + std::to_string(bad) + ";";
		CHECK(SUCCEEDED(hb), std::string(c.name) + ": the destination filled");
		CHECK(c.refused ? hc == D3DERR_INVALIDCALL : hc == D3D_OK, std::string(c.name) + (c.refused ? ": refused, as today" : ": answered D3D_OK"));
		CHECK(bad == 0, std::string(c.name) + ": the destination level holds exactly the expected texels");
		if (c.big && e.nd->csOn())
			CHECK(syncs >= 1, std::string(c.name) + ": went S");
		for (IUnknown* u : { static_cast<IUnknown*>(srcSurf), static_cast<IUnknown*>(srcTex), static_cast<IUnknown*>(bgImg), static_cast<IUnknown*>(lv), static_cast<IUnknown*>(t) })
			if (u)
				u->Release();
	}
	return ok;
}
bool caseStrictHostCopy(Env& e, std::string* d) { return hostCopyCases(e, d, false); }
// The refused DXT1 destination (2,4) alone: the CS=1 front block check refuses before any exec half exists, while CS=0 runs today's
// body (it traces a CopyHost, then copyFrom refuses) - the EXEC streams differ by design for this refusal (measured, --strict dump
// 13/10: the only difference). Judged by HRESULT, contents and every device counter in both modes, not by the digest: ASYNC-classified.
bool caseHostCopyRefused(Env& e, std::string* d) { return hostCopyCases(e, d, true); }

// CopyRects' read-back S for the MANAGED routes - a dropped MANAGED source into a DEFAULT target and into a host
// image, and a DEFAULT target into a MANAGED texture - each behind a held, queued predecessor: the producer blocks (S drains first),
// no ownership violation, D3D_OK, and the copied texels. X-cs-copy-readback-s: the S only for DEFAULT -> SYSTEMMEM. ASYNC (held).
// X' (rw3-X-3, render-integration-5 conditions 1-2; gate 2 found GetDesc draining the worker on every game call). cs-getdesc-nodrain:
// with the worker HELD, GetLevelDesc (a texture level) and GetDesc (the back buffer) return at once - before a helper releases the
// hold 500 ms later - with no S counted, and answer the descriptions they gave with the worker free. X-: GetDesc reads the format
// table through the draining resources() (46bd619c2's code) - it blocks until the release and counts an S. ASYNC (worker held).
bool caseCsGetDescNoDrain(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	IDirect3DTexture8* t = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(64, 32, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t)) && t, "a texture");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	D3DSURFACE_DESC f0 = {}, f1 = {}, h0 = {}, h1 = {};
	const HRESULT a0 = t->GetLevelDesc(1, &f0), b0 = bb ? bb->GetDesc(&f1) : E_FAIL;
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);   // queued behind the hold: the worker now has work it cannot finish
	std::atomic<bool> released { false };
	GxNativeCs* cs = &e.nd->csStream();
	std::thread rel([cs, &released] {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		released.store(true);
		cs->testRelease();
	});
	const uint64_t s0 = e.nd->csSyncCount();
	const auto t0 = std::chrono::steady_clock::now();
	const HRESULT a1 = t->GetLevelDesc(1, &h0), b1 = bb ? bb->GetDesc(&h1) : E_FAIL;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	const bool beforeRelease = !released.load();
	const uint64_t syncs = e.nd->csSyncCount() - s0;
	rel.join();
	char buf[160];
	std::snprintf(buf, sizeof(buf), "held calls %.1f ms, before the release %d, S +%llu; level 1 %ux%u fmt %u size %u", ms, int(beforeRelease),
		(unsigned long long)syncs, h0.Width, h0.Height, unsigned(h0.Format), h0.Size);
	*d += buf;
	CHECK(SUCCEEDED(a0) && SUCCEEDED(b0) && SUCCEEDED(a1) && SUCCEEDED(b1), "GetLevelDesc / GetDesc answer D3D_OK");
	CHECK(beforeRelease && syncs == 0, "with the worker held they return at once and count no S (no drain)");
	CHECK(std::memcmp(&f0, &h0, sizeof(f0)) == 0 && std::memcmp(&f1, &h1, sizeof(f1)) == 0, "the same descriptions as with the worker free");
	CHECK(h0.Width == 32 && h0.Height == 16 && h0.Format == D3DFMT_A8R8G8B8 && h0.Size == 32 * 16 * 4, "level 1 of a 64x32 A8R8G8B8 texture");
	if (bb)
		bb->Release();
	t->Release();
	return ok;
}

// X' (r21-core P2-1): drain ATTRIBUTION is observed at BOTH recording sites - two front drains, each held ~200 ms by
// the worker: an accessor (resources(): csSync, "accessor resources") and an S body (MultiplyTransform: SScope, "MultiplyTransform").
// The next periodic stats line names both in cs_sync_top and gives each >= 150 ms in cs_drain_top. X-cs-drain-attribution: the csSync
// site records 0 ns (csDrainTimeZero); X-cs-drain-attribution-s: the SScope site records 0 ns (csDrainTimeZeroS). ASYNC (worker held).
bool caseCsDrainAttribution(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	GxNativeCs* cs = &e.nd->csStream();
	auto heldDrain = [&](const std::function<void()>& drain) {
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);   // queued behind the hold
		std::thread rel([cs] {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			cs->testRelease();
		});
		drain();   // blocked until the release (~200 ms)
		rel.join();
	};
	heldDrain([&] { (void)e.nd->resources(); });   // csSync: "accessor resources"
	const D3DMATRIX id = { { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } } };
	heldDrain([&] { e.dev->MultiplyTransform(D3DTS_WORLD, &id); });   // SScope: "MultiplyTransform" GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	for (int i = 0; i < 120; ++i)
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	e.nd->csSync();   // the worker prints Present 120's line (divergence d) - read after a drain
	std::string line;
	const std::vector<std::string> log = logSnapshot();
	for (auto it = log.rbegin(); it != log.rend() && line.empty(); ++it)
		if (it->compare(0, 39, "[GXNATIVE] stats periodic presents=120 ") == 0)
			line = *it;
	auto field = [&](const char* key, const char* next) {
		const size_t a = line.find(std::string(" ") + key + "="), b = a == std::string::npos ? a : line.find(std::string(" ") + next + "=", a);
		return a == std::string::npos || b == std::string::npos ? std::string() : line.substr(a + std::strlen(key) + 2, b - a - std::strlen(key) - 2);
	};
	const std::string top = field("cs_drain_top", "cs_full_waits"), syncTop = field("cs_sync_top", "cs_drain_top");
	auto msOf = [&](const char* why) {
		const size_t at = top.find(std::string(why) + ":");
		return at == std::string::npos ? -1.0 : std::atof(top.c_str() + at + std::strlen(why) + 1);
	};
	const double acc = msOf("accessor resources"), sc = msOf("MultiplyTransform");
	*d += "cs_sync_top=" + syncTop + " cs_drain_top=" + top;
	CHECK(!line.empty(), "a periodic stats line at 120 presents");
	CHECK(syncTop.find("accessor resources:") != std::string::npos && syncTop.find("MultiplyTransform:") != std::string::npos,
		"cs_sync_top names both held drains' reasons");
	CHECK(acc >= 150, "cs_drain_top: the held accessor drain (csSync) carries its time");
	CHECK(sc >= 150, "cs_drain_top: the held S body's drain (SScope) carries its time");
	return ok;
}

// cs-getdesc-race (BY NAME, the TSan lane, both submit modes): GetDesc / GetLevelDesc / GetSurfaceLevel on the main thread WHILE the
// worker runs (not held) what 300 rounds queue - a MANAGED texture's upload (LockRect / UnlockRect), a draw that binds a long-lived
// texture (the worker marks its image used), the texture's destroy - and the descriptions are right every time; TSan must report no
// race. X-: GetDesc of a texture level also reads the image's worker-written lastUse on the front - TSan must report it (the arm's
// own checks still pass: TSan judges it). ASYNC (a race schedule).
bool caseCsGetDescRace(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	IDirect3DTexture8* keep = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(128, 64, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &keep)) && keep, "a long-lived texture");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	IDirect3DSurface8* keepLevel = nullptr;
	keep->GetSurfaceLevel(0, &keepLevel);
	csarm::ffDiffuse(e);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	const csarm::TV q[4] = { { 0, 0, 0.5f, 1, 0, 0 }, { 16, 0, 0.5f, 1, 1, 0 }, { 0, 16, 0.5f, 1, 0, 1 }, { 16, 16, 0.5f, 1, 1, 1 } };
	int wrong = 0;
	for (int i = 0; i < 300; ++i) {
		const D3DFORMAT f = (i & 1) ? D3DFMT_A8R8G8B8 : D3DFMT_R5G6B5;
		IDirect3DTexture8* t = nullptr;
		if (FAILED(e.dev->CreateTexture(32, 32, 2, 0, f, D3DPOOL_MANAGED, &t)) || !t) {
			++wrong;
			continue;
		}
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
			for (int y = 0; y < 32; ++y)
				std::memset(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, i & 0xff, f == D3DFMT_R5G6B5 ? 64 : 128);
			t->UnlockRect(0);   // the upload runs on the worker
		}
		e.dev->SetTexture(0, keep);
		e.dev->BeginScene();
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));   // the worker binds keep (marks its image used)
		e.dev->EndScene();
		D3DSURFACE_DESC a = {}, b = {}, c = {}, k = {}, x = {};
		t->GetLevelDesc(1, &a);
		IDirect3DSurface8* s0 = nullptr;
		if (SUCCEEDED(t->GetSurfaceLevel(0, &s0)) && s0) {
			s0->GetDesc(&b);
			s0->Release();
		}
		keep->GetLevelDesc(UINT(i % 3), &c);
		if (keepLevel)
			keepLevel->GetDesc(&k);
		if (bb)
			bb->GetDesc(&x);
		wrong += !(a.Width == 16 && a.Height == 16 && a.Format == f) + !(b.Width == 32 && b.Height == 32 && b.Format == f) +
			!(c.Format == D3DFMT_A8R8G8B8 && c.Width == (128u >> (i % 3))) + !(k.Width == 128 && k.Height == 64) + !(x.Width == 64);
		e.dev->SetTexture(0, nullptr);
		t->Release();   // the destroy runs on the worker
		if (i % 50 == 49)
			e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	}
	*d += "300 rounds of upload / draw / desc / release with the worker running; wrong descriptions " + std::to_string(wrong);
	CHECK(wrong == 0, "every description right while the worker runs");
	if (keepLevel)
		keepLevel->Release();
	if (bb)
		bb->Release();
	keep->Release();
	return ok;
}

bool caseCsCopyReadbackS(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb, "the back buffer");
	const uint64_t own0 = e.nd->csOwnerViolations();
	auto dropped = [&](uint32_t seed, IDirect3DTexture8** out, csarm2::Lvl* l) {   // a MANAGED 16x16 filled, then dropped at a Present
		*out = nullptr;
		if (FAILED(e.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out)) || !*out)
			return false;
		l->fill(seed);
		D3DLOCKED_RECT lr = {};
		if (FAILED((*out)->LockRect(0, &lr, nullptr, 0)))
			return false;
		l->write(lr);
		(*out)->UnlockRect(0);
		const uint64_t before = e.nd->stats().dropped;
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		return e.nd->stats().dropped == before + 1;
	};
	auto heldCall = [&](const char* name, std::function<HRESULT()> call) {
		const uint64_t s0 = e.nd->csSyncCount();
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x0a0b0c0du);   // queued behind the hold
		HRESULT hr = E_FAIL;
		bool blocked = false, releasedFirst = false;
		csarm::blockedCall(e, [&] { hr = call(); }, &blocked, &releasedFirst);
		e.nd->csSync();
		const uint64_t syncs = e.nd->csSyncCount() - s0;
		*d += std::string(" ") + name + ": hr " + std::to_string(long(hr)) + " blocked " + (blocked ? "yes" : "no") + " S +" + std::to_string(syncs) + ";";
		CHECK(hr == D3D_OK, std::string(name) + " answered D3D_OK");
		CHECK(blocked && syncs >= 1, std::string(name) + ": the S body drained the queued command first (the producer blocked)");
	};
	// read a 16x16 level / surface back through the host
	auto readHost = [&](IDirect3DSurface8* s, std::vector<uint32_t>* px) {
		px->assign(256, 0);
		IDirect3DSurface8* img = nullptr;
		D3DSURFACE_DESC sd = {};
		s->GetDesc(&sd);
		IDirect3DSurface8* rd = s;
		if (sd.Pool != D3DPOOL_SYSTEMMEM && sd.Pool != D3DPOOL_MANAGED) {
			if (FAILED(e.dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &img)) || FAILED(e.dev->CopyRects(s, nullptr, 0, img, nullptr)))
				return false;
			rd = img;
		}
		D3DLOCKED_RECT lr = {};
		const bool okl = SUCCEEDED(rd->LockRect(&lr, nullptr, D3DLOCK_READONLY));
		if (okl) {
			for (int y = 0; y < 16; ++y)
				std::memcpy(&(*px)[size_t(y) * 16], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 64);
			rd->UnlockRect();
		}
		if (img)
			img->Release();
		return okl;
	};
	auto same = [](const std::vector<uint32_t>& px, const csarm2::Lvl& l) {
		int n = 0;
		for (size_t i = 0; i < 256; ++i) {
			uint32_t w;
			std::memcpy(&w, &l.b[i * 4], 4);
			n += (px[i] & 0xffffff) == (w & 0xffffff);
		}
		return n;
	};
	std::vector<uint32_t> px;
	// (a) a dropped MANAGED source into a DEFAULT render-target texture
	csarm2::Lvl la(16, 16, D3DFMT_A8R8G8B8), lb(16, 16, D3DFMT_A8R8G8B8);
	IDirect3DTexture8* ma = nullptr;
	IDirect3DTexture8* dt = nullptr;
	IDirect3DSurface8* mal = nullptr;
	IDirect3DSurface8* dtl = nullptr;
	SETUP(dropped(5, &ma, &la) && SUCCEEDED(ma->GetSurfaceLevel(0, &mal)), "a dropped MANAGED texture");
	SETUP(SUCCEEDED(e.dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dt)) && dt && SUCCEEDED(dt->GetSurfaceLevel(0, &dtl)), "a DEFAULT target");
	heldCall("dropped MANAGED -> DEFAULT", [&] { return e.dev->CopyRects(mal, nullptr, 0, dtl, nullptr); });
	const int na = readHost(dtl, &px) ? same(px, la) : -1;
	// (b) a dropped MANAGED source into a host image
	IDirect3DTexture8* mb = nullptr;
	IDirect3DSurface8* mbl = nullptr;
	IDirect3DSurface8* img = nullptr;
	SETUP(dropped(6, &mb, &lb) && SUCCEEDED(mb->GetSurfaceLevel(0, &mbl)) && SUCCEEDED(e.dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &img)), "a dropped MANAGED texture + an image");
	heldCall("dropped MANAGED -> host", [&] { return e.dev->CopyRects(mbl, nullptr, 0, img, nullptr); });
	const int nb = readHost(img, &px) ? same(px, lb) : -1;
	// (c) a DEFAULT target (cleared) into a MANAGED texture
	SETUP(SUCCEEDED(e.dev->SetRenderTarget(dtl, nullptr)), "the DEFAULT target bound");
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff3a5c7e, 1.0f, 0);
	e.dev->SetRenderTarget(bb, ds);
	IDirect3DTexture8* mc = nullptr;
	IDirect3DSurface8* mcl = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &mc)) && mc && SUCCEEDED(mc->GetSurfaceLevel(0, &mcl)), "a MANAGED target");
	heldCall("DEFAULT -> MANAGED", [&] { return e.dev->CopyRects(dtl, nullptr, 0, mcl, nullptr); });
	int nc = -1;
	D3DLOCKED_RECT lr = {};
	if (SUCCEEDED(mc->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
		nc = 0;
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x)
				nc += (reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch)[x] & 0xffffff) == 0x3a5c7e;
		mc->UnlockRect(0);
	}
	const uint64_t own = e.nd->csOwnerViolations() - own0;
	*d += " texels right " + std::to_string(na) + "/" + std::to_string(nb) + "/" + std::to_string(nc) + " (of 256), ownership violations " + std::to_string(own);
	CHECK(na == 256 && nb == 256 && nc == 256, "every route copied the right texels");
	CHECK(own == 0, "no worker state touched on the front outside an S body");
	for (IUnknown* u : { static_cast<IUnknown*>(mcl), static_cast<IUnknown*>(mc), static_cast<IUnknown*>(img), static_cast<IUnknown*>(mbl), static_cast<IUnknown*>(mb),
			 static_cast<IUnknown*>(dtl), static_cast<IUnknown*>(dt), static_cast<IUnknown*>(mal), static_cast<IUnknown*>(ma), static_cast<IUnknown*>(ds), static_cast<IUnknown*>(bb) })
		if (u)
			u->Release();
	return ok;
}

// a dropped MANAGED level restored for WRITING - a partial writing LockRect, and a host CopyRects into it - each
// behind a held, queued predecessor: the producer blocks (the restore's S drains first), the new texels land, the rest is restored
// from the GPU (drawn 1:1), restores counted, no ownership violation. X-cs-managed-restore-write: the restore enters S only for
// READONLY locks. ASYNC (held).
bool caseCsManagedRestoreWrite(Env& e, std::string* d)
{
	bool ok = true;
	const uint64_t own0 = e.nd->csOwnerViolations(), r0 = e.nd->stats().restores;
	for (int k = 0; k < 2; ++k) {
		IDirect3DTexture8* m = nullptr;
		SETUP(SUCCEEDED(e.dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m)) && m, "a MANAGED texture");
		csarm2::Lvl old(16, 16, D3DFMT_A8R8G8B8), patch(4, 4, D3DFMT_A8R8G8B8);
		old.fill(7 + uint32_t(k));
		patch.fill(9 + uint32_t(k));
		D3DLOCKED_RECT lr = {};
		SETUP(SUCCEEDED(m->LockRect(0, &lr, nullptr, 0)), "filled");
		old.write(lr);
		m->UnlockRect(0);
		const uint64_t dr0 = e.nd->stats().dropped;
		e.dev->Present(nullptr, nullptr, nullptr, nullptr);
		SETUP(e.nd->stats().dropped == dr0 + 1, "dropped at the Present");
		const RECT pr = { 4, 6, 8, 10 };
		const POINT pt = { 4, 6 };
		IDirect3DSurface8* img = k ? csarm2::image(e, patch) : nullptr;
		IDirect3DSurface8* ml = nullptr;
		m->GetSurfaceLevel(0, &ml);
		const uint64_t s0 = e.nd->csSyncCount();
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x01010101u * uint32_t(k + 1));
		HRESULT hr = E_FAIL;
		bool blocked = false, releasedFirst = false;
		csarm::blockedCall(e, [&] {
			if (k == 0) {
				hr = m->LockRect(0, &lr, &pr, 0);
				if (SUCCEEDED(hr)) {
					patch.write(lr);
					m->UnlockRect(0);
				}
			} else {
				const RECT all4 = { 0, 0, 4, 4 };
				hr = e.dev->CopyRects(img, &all4, 1, ml, &pt);
			}
		}, &blocked, &releasedFirst);
		e.nd->csSync();
		const uint64_t syncs = e.nd->csSyncCount() - s0;
		csarm2::Lvl want = old;
		want.copyFrom(patch, { 0, 0, 4, 4 }, pt);
		const int bad = csarm2::compareDrawn(e, m, 1, 0, want);
		*d += std::string(k ? " host CopyRects" : " writing LockRect") + ": hr " + std::to_string(long(hr)) + " blocked " + (blocked ? "yes" : "no") + " S +" +
			std::to_string(syncs) + " texels wrong " + std::to_string(bad) + ";";
		CHECK(hr == D3D_OK, "answered D3D_OK");
		CHECK(blocked && syncs >= 1, "the restore's S drained the queued command first (the producer blocked)");
		CHECK(bad == 0, "the new texels landed, the rest restored");
		for (IUnknown* u : { static_cast<IUnknown*>(ml), static_cast<IUnknown*>(img), static_cast<IUnknown*>(m) })
			if (u)
				u->Release();
	}
	const uint64_t own = e.nd->csOwnerViolations() - own0, restores = e.nd->stats().restores - r0;
	*d += " restores +" + std::to_string(restores) + ", ownership violations " + std::to_string(own);
	CHECK(restores >= 2, "both restores counted");
	CHECK(own == 0, "no worker state touched on the front outside an S body");
	return ok;
}

// GPU copies beyond square regions - a rectangular (3,2)-(13,7) sub-copy between DEFAULT render targets at
// (5,9) (the region and the untouched surroundings read back), and an equal-count 4x MSAA -> MSAA copy, then resolved and read back.
// Plants (one Case row each): csCopyGpuSquareExtent, csCopyGpuMsaaFromSrc. STRICT (serial).
bool caseStrictGpuCopyShapes(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb, "the back buffer");
	auto readRt = [&](IDirect3DSurface8* s, UINT w, UINT h, std::vector<uint32_t>* px) {
		px->assign(size_t(w) * h, 0);
		IDirect3DSurface8* img = nullptr;
		bool okr = SUCCEEDED(e.dev->CreateImageSurface(w, h, D3DFMT_A8R8G8B8, &img)) && img && SUCCEEDED(e.dev->CopyRects(s, nullptr, 0, img, nullptr));
		D3DLOCKED_RECT lr = {};
		if (okr && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			for (UINT y = 0; y < h; ++y)
				std::memcpy(&(*px)[size_t(y) * w], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(w) * 4);
			img->UnlockRect();
		} else
			okr = false;
		if (img)
			img->Release();
		csarm::obs(px->data(), px->size() * 4);
		return okr;
	};
	auto clearQuads = [&](IDirect3DSurface8* s, UINT w, UINT h) {   // four quadrants, four colours
		e.dev->SetRenderTarget(s, nullptr);
		const D3DCOLOR c[4] = { 0xffc03010, 0xff10c030, 0xff3010c0, 0xffc0c010 };
		for (int i = 0; i < 4; ++i) {
			const D3DRECT r = { LONG((i % 2) * (w / 2)), LONG((i / 2) * (h / 2)), LONG((i % 2 + 1) * (w / 2)), LONG((i / 2 + 1) * (h / 2)) };
			e.dev->Clear(1, &r, D3DCLEAR_TARGET, c[i], 1.0f, 0);
		}
		e.dev->SetRenderTarget(bb, ds);
	};
	// (1) the rectangular sub-copy
	IDirect3DTexture8* st = nullptr;
	IDirect3DTexture8* dt = nullptr;
	IDirect3DSurface8* sl = nullptr;
	IDirect3DSurface8* dl = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &st)) && st && SUCCEEDED(st->GetSurfaceLevel(0, &sl)), "the source target");
	SETUP(SUCCEEDED(e.dev->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dt)) && dt && SUCCEEDED(dt->GetSurfaceLevel(0, &dl)), "the destination target");
	clearQuads(sl, 32, 32);
	e.dev->SetRenderTarget(dl, nullptr);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
	e.dev->SetRenderTarget(bb, ds);
	const RECT r = { 3, 2, 13, 7 };
	const POINT pt = { 5, 9 };
	const HRESULT h1 = e.dev->CopyRects(sl, &r, 1, dl, &pt);
	std::vector<uint32_t> sp, dp;
	readRt(sl, 32, 32, &sp);
	readRt(dl, 32, 32, &dp);
	int bad = 0;
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x) {
			const bool in = x >= pt.x && x < pt.x + 10 && y >= pt.y && y < pt.y + 5;
			const uint32_t want = in ? sp[size_t(y - pt.y + r.top) * 32 + size_t(x - pt.x + r.left)] : 0xff202020u;
			bad += (dp[size_t(y) * 32 + x] & 0xffffff) != (want & 0xffffff);
		}
	// (2) equal-count MSAA -> MSAA, then a resolve
	IDirect3DSurface8* ma = nullptr;
	IDirect3DSurface8* mb = nullptr;
	IDirect3DSurface8* one = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &ma)) && ma &&
		SUCCEEDED(e.dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &mb)) && mb &&
		SUCCEEDED(e.dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, FALSE, &one)) && one, "two 4x MSAA targets and a single-sample one");
	clearQuads(ma, 32, 32);
	e.dev->SetRenderTarget(mb, nullptr);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
	e.dev->SetRenderTarget(bb, ds);
	const HRESULT h2 = e.dev->CopyRects(ma, nullptr, 0, mb, nullptr);
	const HRESULT h3 = e.dev->CopyRects(mb, nullptr, 0, one, nullptr);
	std::vector<uint32_t> rp;
	readRt(one, 32, 32, &rp);
	const GxNativeValidation val = e.nd->validation();   // a resolve into the MSAA destination is invalid Vulkan: the arm sees it
	CHECK(val.errors == 0 && val.hazards == 0, "no validation error (the equal-count copy is a copy, not a resolve)");
	const uint32_t q[4] = { rp[size_t(8) * 32 + 8], rp[size_t(8) * 32 + 24], rp[size_t(24) * 32 + 8], rp[size_t(24) * 32 + 24] };
	*d += "rect copy hr " + std::to_string(long(h1)) + " wrong " + std::to_string(bad) + "; MSAA copy hr " + std::to_string(long(h2)) + " resolve hr " +
		std::to_string(long(h3)) + " quadrants " + csarm::hex(q[0] & 0xffffff) + "/" + csarm::hex(q[1] & 0xffffff) + "/" + csarm::hex(q[2] & 0xffffff) + "/" + csarm::hex(q[3] & 0xffffff);
	CHECK(h1 == D3D_OK && bad == 0, "the rectangular sub-copy: the region and the untouched surroundings");
	CHECK(h2 == D3D_OK && h3 == D3D_OK && (q[0] & 0xffffff) == 0xc03010 && (q[1] & 0xffffff) == 0x10c030 && (q[2] & 0xffffff) == 0x3010c0 &&
		(q[3] & 0xffffff) == 0xc0c010, "the equal-count MSAA copy carries all four quadrants through the resolve");
	for (IUnknown* u : { static_cast<IUnknown*>(one), static_cast<IUnknown*>(mb), static_cast<IUnknown*>(ma), static_cast<IUnknown*>(dl), static_cast<IUnknown*>(dt),
			 static_cast<IUnknown*>(sl), static_cast<IUnknown*>(st), static_cast<IUnknown*>(ds), static_cast<IUnknown*>(bb) })
		if (u)
			u->Release();
	return ok;
}

// host -> DEFAULT copies at NONZERO destination points land where the call says. (1) the queued path: an 8x8
// image's rectangles (2,1)-(6,5) and (0,0)-(2,2) into a 16x16 DEFAULT render-target texture at (9,7) and (1,12) - only the FIRST
// lands, as DXVK's d3d8 CopyRects does (measured both modes, base the same); (2) the oversize S
// path (64 KiB ring): a 64x64 image into a 128x128 one at (32,40). Each target is cleared first; read back through the game's own
// path, every pixel must be the image's in the copied regions and the clear's elsewhere. X-strict-copy-offsets: the queued exec
// half writes at the origin. STRICT (serial).
bool caseStrictCopyOffsets(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb, "the back buffer");
	auto pattern = [](UINT w, UINT h, uint32_t seed) {
		std::vector<uint32_t> v(size_t(w) * h);
		for (size_t i = 0; i < v.size(); ++i)
			v[i] = 0xff000000u | (uint32_t((i + seed) * 2654435761u) >> 8);
		return v;
	};
	auto makeImg = [&](UINT w, UINT h, const std::vector<uint32_t>& px) {
		IDirect3DSurface8* img = nullptr;
		if (FAILED(e.dev->CreateImageSurface(w, h, D3DFMT_A8R8G8B8, &img)) || !img)
			return img;
		D3DLOCKED_RECT lr = {};
		if (SUCCEEDED(img->LockRect(&lr, nullptr, 0))) {
			for (UINT y = 0; y < h; ++y)
				std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, &px[size_t(y) * w], size_t(w) * 4);
			img->UnlockRect();
		}
		return img;
	};
	// a cleared DEFAULT render-target texture's level 0, read back whole after the copy
	auto run = [&](UINT tw, const char* name, const std::vector<uint32_t>& src, UINT sw, UINT nr, const RECT* rs, const POINT* ps,
		std::vector<uint32_t>* want, uint64_t* syncs) {
		IDirect3DTexture8* t = nullptr;
		IDirect3DSurface8* lv = nullptr;
		HRESULT hc = E_FAIL;
		std::vector<uint32_t> got(size_t(tw) * tw, 0);
		if (SUCCEEDED(e.dev->CreateTexture(tw, tw, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t)) && t &&
			SUCCEEDED(t->GetSurfaceLevel(0, &lv)) && lv && SUCCEEDED(e.dev->SetRenderTarget(lv, nullptr))) {
			e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
			e.dev->SetRenderTarget(bb, ds);
			IDirect3DSurface8* img = makeImg(sw, sw, src);
			const uint64_t s0 = e.nd->csSyncCount();
			if (img)
				hc = e.dev->CopyRects(img, rs, nr, lv, ps);
			*syncs = e.nd->csSyncCount() - s0;
			IDirect3DSurface8* rb = nullptr;
			if (SUCCEEDED(e.dev->CreateImageSurface(tw, tw, D3DFMT_A8R8G8B8, &rb)) && rb && SUCCEEDED(e.dev->CopyRects(lv, nullptr, 0, rb, nullptr))) {
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(rb->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
					for (UINT y = 0; y < tw; ++y)
						std::memcpy(&got[size_t(y) * tw], static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(tw) * 4);
					rb->UnlockRect();
				}
			}
			if (rb)
				rb->Release();
			if (img)
				img->Release();
		}
		e.dev->SetRenderTarget(bb, ds);
		if (lv)
			lv->Release();
		if (t)
			t->Release();
		want->assign(size_t(tw) * tw, 0xff202020u);
		for (UINT i = 0; i < 1 && i < nr; ++i)   // ONLY the first rectangle: DXVK's d3d8 CopyRects returns inside its loop (as today)
			for (LONG y = rs[i].top; y < rs[i].bottom; ++y)
				for (LONG x = rs[i].left; x < rs[i].right; ++x)
					(*want)[size_t(ps[i].y + (y - rs[i].top)) * tw + size_t(ps[i].x + (x - rs[i].left))] = src[size_t(y) * sw + size_t(x)];
		int same = 0;
		std::string bad;
		for (size_t i = 0; i < got.size(); ++i) {
			const bool eq = (got[i] & 0xffffff) == ((*want)[i] & 0xffffff);
			same += eq;
			if (!eq && bad.size() < 160)
				bad += " (" + std::to_string(i % tw) + "," + std::to_string(i / tw) + ") " + csarm::hex(got[i] & 0xffffff) + "!=" + csarm::hex((*want)[i] & 0xffffff);
		}
		if (!bad.empty())
			*d += " first wrong:" + bad + ";";
		csarm::obsv(hc);
		csarm::obs(got.data(), got.size() * 4);
		*d += std::string(" ") + name + ": hr " + std::to_string(long(hc)) + " S +" + std::to_string(*syncs) + " pixels right " +
			std::to_string(same) + "/" + std::to_string(got.size()) + ";";
		CHECK(hc == D3D_OK, std::string(name) + " answered D3D_OK");
		CHECK(same == int(got.size()), std::string(name) + ": the copied regions hold the image, the rest the clear");
	};
	const std::vector<uint32_t> smallImg = pattern(8, 8, 11), big = pattern(64, 64, 29);   // not `small`: rpcndr.h #defines it (MSVC)
	const RECT rs[2] = { { 2, 1, 6, 5 }, { 0, 0, 2, 2 } };
	const POINT ps[2] = { { 9, 7 }, { 1, 12 } };
	std::vector<uint32_t> want;
	uint64_t syncs = 0;
	run(16, "queued 2 rects", smallImg, 8, 2, rs, ps, &want, &syncs);
	const RECT whole = { 0, 0, 64, 64 };
	const POINT at = { 32, 40 };
	run(128, "oversize at (32,40)", big, 64, 1, &whole, &at, &want, &syncs);
	if (e.nd->csOn())
		CHECK(syncs >= 1, "the 64x64 copy took the oversize S path");
	if (ds)
		ds->Release();
	bb->Release();
	return ok;
}

// CopyRects host -> a DEFAULT texture level over the worker's largest command (the arm's ring is 64 KiB: 16 KiB
// - 256) takes its S path: a defined 64x64 A8R8G8B8 image into a matching DEFAULT texture's level 0 (16 384 bytes) answers D3D_OK as
// today, enters S at CS=1, and the texels arrive - the texture drawn 1:1 (point filter) reads back exactly the image's pixels.
// X-strict-copy-oversize: the oversize branch refuses (INVALIDCALL). STRICT (serial).
bool caseStrictCopyOversize(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* img = nullptr;
	IDirect3DTexture8* tex = nullptr;
	IDirect3DSurface8* lv = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &img)) && img, "a 64x64 image surface");
	SETUP(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex)) && tex, "a 64x64 DEFAULT texture");
	SETUP(SUCCEEDED(tex->GetSurfaceLevel(0, &lv)) && lv, "its level 0");
	std::vector<uint32_t> src(64 * 64);
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = 0xff000000u | (uint32_t(i * 2654435761u) >> 8);
	D3DLOCKED_RECT lr = {};
	SETUP(SUCCEEDED(img->LockRect(&lr, nullptr, 0)), "the image locked");
	for (int y = 0; y < 64; ++y)
		std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, &src[size_t(y) * 64], 256);
	img->UnlockRect();
	const uint64_t s0 = e.nd->csSyncCount();
	const HRESULT hc = e.dev->CopyRects(img, nullptr, 0, lv, nullptr);
	const uint64_t s1 = e.nd->csSyncCount();
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "the back buffer cleared");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	e.dev->SetTexture(0, tex);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	e.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	e.dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
	e.dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);
	struct V { float x, y, z, rhw, u, v; };
	const V q[4] = { { -0.5f, -0.5f, 0.5f, 1, 0, 0 }, { 63.5f, -0.5f, 0.5f, 1, 1, 0 }, { -0.5f, 63.5f, 0.5f, 1, 0, 1 }, { 63.5f, 63.5f, 0.5f, 1, 1, 1 } };
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
	e.dev->EndScene();
	e.dev->SetTexture(0, nullptr);
	// the whole back buffer read back, compared with the image
	IDirect3DSurface8* rb = nullptr;
	int same = -1;
	if (SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &rb)) && rb && SUCCEEDED(e.dev->CopyRects(bb, nullptr, 0, rb, nullptr))) {
		D3DLOCKED_RECT r2 = {};
		if (SUCCEEDED(rb->LockRect(&r2, nullptr, D3DLOCK_READONLY))) {
			same = 0;
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x) {
					uint32_t p = 0;
					std::memcpy(&p, static_cast<const uint8_t*>(r2.pBits) + size_t(y) * r2.Pitch + size_t(x) * 4, 4);
					same += (p & 0xffffff) == (src[size_t(y) * 64 + x] & 0xffffff);
				}
			rb->UnlockRect();
		}
	}
	csarm::obsv(hc);
	csarm::obsSurface(e, bb);
	*d += "hr " + std::to_string(long(hc)) + ", S +" + std::to_string(s1 - s0) + ", texels matching the image " + std::to_string(same) + "/4096";
	CHECK(hc == D3D_OK, "the oversize host copy answered D3D_OK, as today");
	if (e.nd->csOn())
		CHECK(s1 - s0 >= 1, "at CS=1 the oversize host copy went S");
	CHECK(same == 4096, "the texels arrived: the texture drawn 1:1 reads back the image exactly");
	if (rb)
		rb->Release();
	bb->Release();
	lv->Release();
	tex->Release();
	img->Release();
	return ok;
}

// an oversize shader creation enters S - drains the worker - BEFORE it translates. With the worker HELD at a
// queued SetRenderState(TEXTUREFACTOR), each oversize creation (as strict-shader-oversize's) runs while a helper releases the hold
// once the producer blocks; an order sink (EXEC + translator-input events in one sequence) must show the queued RenderState's EXEC
// and translator events BEFORE the shader's translator event. X-cs-shader-oversize-order (S entered after the translation): the
// shader is translated first. ASYNC (worker held, 64 KiB ring).
namespace csarm {
struct OrderSink final : GXRender::Dev::GxCsExecSink, GXRender::Rec::GxTranslateInputSink
{
	std::mutex m;
	std::vector<std::pair<char, int>> seq;   // ('E', GxCsOp) / ('T', GxTrIn)
	void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg*, size_t, const GXRender::Dev::GxCsBytes*) override
	{
		std::lock_guard<std::mutex> g(m);
		seq.emplace_back('E', int(op));
	}
	void destroyed(const void*) override {}
	void created(uint64_t, uint32_t, const GXRender::Dev::GxCsArg*, size_t) override {}
	void input(GXRender::Rec::GxTrIn e, const GXRender::Rec::GxTrArg*, size_t, const GXRender::Rec::GxTrBytes*, size_t) override
	{
		std::lock_guard<std::mutex> g(m);
		seq.emplace_back('T', int(e));
	}
	int find(char k, int v) const
	{
		for (size_t i = 0; i < seq.size(); ++i)
			if (seq[i].first == k && seq[i].second == v)
				return int(i);
		return -1;
	}
};
} // namespace csarm
bool caseCsShaderOversizeOrder(Env& e, std::string* d)
{
	bool ok = true;
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	const GXRender::GxCsEntry *vsE = nullptr, *psE = nullptr;
	for (size_t i = 0; i < n; ++i) {
		if (!vsE && !all[i].pixel && all[i].program && all[i].decl && std::strstr(all[i].files, "Trees.vso"))
			vsE = &all[i];
		if (!psE && all[i].pixel && all[i].program)
			psE = &all[i];
	}
	SETUP(vsE && psE, "Trees.vso and a ported PS in the catalogue");
	auto enlarge = [](const GXRender::GxCsEntry* x) {
		std::vector<DWORD> v;
		v.push_back(x->dwords[0]);
		v.push_back(0x0000FFFEu | (4100u << 16));
		for (uint32_t i = 0; i < 4100; ++i)
			v.push_back(0x41414100u + (i & 0xff));
		v.insert(v.end(), x->dwords + 1, x->dwords + x->count);
		return v;
	};
	const std::vector<DWORD> vsCode = enlarge(vsE), psCode = enlarge(psE);
	const std::vector<DWORD> decl(vsE->decl, vsE->decl + vsE->declCount);
	GXRender::Dev::GxCsExecSink* prevE = GXRender::Dev::g_csExecSink.load();
	GXRender::Rec::GxTranslateInputSink* prevT = GXRender::Rec::g_trInputSink.load();
	for (int ps = 0; ps < 2; ++ps) {
		csarm::OrderSink sink;
		GXRender::Dev::g_csExecSink.store(&sink);
		GXRender::Rec::g_trInputSink.store(&sink);
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x11223344u + DWORD(ps));   // queued behind the hold
		DWORD h = 0;
		HRESULT hr = E_FAIL;
		bool blocked = false, releasedFirst = false;
		csarm::blockedCall(e, [&] {
			hr = ps ? e.dev->CreatePixelShader(psCode.data(), &h) : e.dev->CreateVertexShader(decl.data(), vsCode.data(), &h, 0);
		}, &blocked, &releasedFirst);
		e.nd->csSync();
		GXRender::Dev::g_csExecSink.store(prevE);
		GXRender::Rec::g_trInputSink.store(prevT);
		const int eRs = sink.find('E', int(GXRender::Dev::GxCsOp::RenderState)), tRs = sink.find('T', int(GXRender::Rec::GxTrIn::RenderState));
		const int tSh = sink.find('T', int(ps ? GXRender::Rec::GxTrIn::CreatePs : GXRender::Rec::GxTrIn::CreateVs));
		*d += std::string(ps ? " PS" : "VS") + ": hr " + std::to_string(long(hr)) + " blocked " + (blocked ? "yes" : "no") + ", order E(RS)@" +
			std::to_string(eRs) + " T(RS)@" + std::to_string(tRs) + " T(create)@" + std::to_string(tSh) + ";";
		CHECK(hr == D3D_OK && h != 0, "the oversize creation answered D3D_OK and a handle, as today");
		CHECK(eRs >= 0 && tRs >= 0 && tSh >= 0 && eRs < tSh && tRs < tSh, "the queued command ran (EXEC and translator) BEFORE the shader was translated");
		if (hr == D3D_OK)
			ps ? e.dev->DeletePixelShader(h) : e.dev->DeleteVertexShader(h);
	}
	return ok;
}

// The same order property for the other oversize S branches (rw3-X-3, ahead of the oracle: r13 found it for the shaders): with the
// worker HELD at a queued SetRenderState, each oversize call - texture UnlockRect (64 KiB MANAGED), CopyRects host -> DEFAULT (64x64),
// buffer Unlock (32 KiB MANAGED VB), Clear (1100 rects), DrawPrimitiveUP (1200 triangles) - must EXECUTE (its EXEC event) only after
// the queued command's EXEC event, and no A23-B ownership violation is logged. X-cs-oversize-order (csOversizeLateDrain: UnlockRect and
// CopyRects run their inline exec before entering S) reverses those two. ASYNC (worker held, 64 KiB ring).
bool caseCsOversizeOrder(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	SETUP(e.nd->csStream().capacity() == (64u << 10), "the 64 KiB TEST ring");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "the back buffer");
	csarm::ffDiffuse(e);
	IDirect3DTexture8* tex = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex)) && tex, "a 64 KiB MANAGED texture");
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(32u << 10, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "a 32 KiB VB");
	IDirect3DSurface8* img = nullptr;
	IDirect3DTexture8* dtex = nullptr;
	IDirect3DSurface8* dlv = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, &img)) && img, "a 64x64 image");
	SETUP(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dtex)) && dtex && SUCCEEDED(dtex->GetSurfaceLevel(0, &dlv)), "a DEFAULT 64x64 level");
	D3DLOCKED_RECT ilr = {};
	SETUP(SUCCEEDED(img->LockRect(&ilr, nullptr, 0)), "the image locked");
	for (int y = 0; y < 64; ++y)
		std::memset(static_cast<uint8_t*>(ilr.pBits) + size_t(y) * ilr.Pitch, 0x40 + y, 256);
	img->UnlockRect();
	std::vector<D3DRECT> rects(1100);
	for (size_t i = 0; i < rects.size(); ++i)
		rects[i] = { LONG(i % 64), LONG((i / 64) % 64), LONG(i % 64) + 1, LONG((i / 64) % 64) + 1 };
	std::vector<QV> tris(3600);
	for (size_t i = 0; i < 1200; ++i) {
		const float x = float(i % 60), y = float((i / 60) % 60);
		tris[i * 3] = { x, y, 0.5f, 1, 0xff336699 };
		tris[i * 3 + 1] = { x + 2, y, 0.5f, 1, 0xff336699 };
		tris[i * 3 + 2] = { x, y + 2, 0.5f, 1, 0xff336699 };
	}
	struct Step { const char* name; GXRender::Dev::GxCsOp op; bool plantable; std::function<HRESULT()> prep, call; };
	BYTE* vp = nullptr;
	D3DLOCKED_RECT tlr = {};
	const Step steps[] = {
		{ "UnlockRect", GXRender::Dev::GxCsOp::UploadRect, true,
			[&] { const HRESULT h = tex->LockRect(0, &tlr, nullptr, 0); if (SUCCEEDED(h)) for (int y = 0; y < 128; ++y) std::memset(static_cast<uint8_t*>(tlr.pBits) + size_t(y) * tlr.Pitch, 0x5a, 512); return h; },
			[&] { return tex->UnlockRect(0); } },
		{ "CopyRects", GXRender::Dev::GxCsOp::CopyHost, true, [&] { return D3D_OK; }, [&] { return e.dev->CopyRects(img, nullptr, 0, dlv, nullptr); } },
		{ "Unlock", GXRender::Dev::GxCsOp::Upload, false,
			[&] { const HRESULT h = vb->Lock(0, 0, &vp, 0); if (SUCCEEDED(h) && vp) std::memset(vp, 0x11, 32u << 10); return h; }, [&] { return vb->Unlock(); } },
		{ "Clear", GXRender::Dev::GxCsOp::Clear, false, [&] { return D3D_OK; },
			[&] { return e.dev->Clear(DWORD(rects.size()), rects.data(), D3DCLEAR_TARGET, 0xff00c0ff, 1.0f, 0); } },
		{ "DrawPrimitiveUP", GXRender::Dev::GxCsOp::DrawUP, false, [&] { return D3D_OK; },
			[&] { return e.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1200, tris.data(), sizeof(QV)); } },
	};
	GXRender::Dev::GxCsExecSink* prevE = GXRender::Dev::g_csExecSink.load();
	GXRender::Rec::GxTranslateInputSink* prevT = GXRender::Rec::g_trInputSink.load();
	const bool planted = e.nd->debug().csOversizeLateDrain;
	int reversed = 0;
	const uint64_t own0 = e.nd->csOwnerViolations();
	e.dev->BeginScene();
	for (const Step& st : steps) {
		const HRESULT hp = st.prep();
		e.nd->csSync();
		csarm::OrderSink sink;
		GXRender::Dev::g_csExecSink.store(&sink);
		GXRender::Rec::g_trInputSink.store(&sink);
		const uint64_t s0 = e.nd->csSyncCount();
		csarm::hold(e);
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x01020304u);   // queued behind the hold
		HRESULT hr = E_FAIL;
		bool blocked = false, releasedFirst = false;
		csarm::blockedCall(e, [&] { hr = st.call(); }, &blocked, &releasedFirst);
		e.nd->csSync();
		const uint64_t syncs = e.nd->csSyncCount() - s0;
		GXRender::Dev::g_csExecSink.store(prevE);
		GXRender::Rec::g_trInputSink.store(prevT);
		const int eRs = sink.find('E', int(GXRender::Dev::GxCsOp::RenderState)), eOp = sink.find('E', int(st.op));
		const bool inOrder = eRs >= 0 && eOp >= 0 && eRs < eOp;
		reversed += st.plantable && !inOrder;
		*d += std::string(" ") + st.name + ": hr " + std::to_string(long(hr)) + " S +" + std::to_string(syncs) + " E(RS)@" + std::to_string(eRs) +
			" E(op)@" + std::to_string(eOp) + ";";
		CHECK(SUCCEEDED(hp) && hr == D3D_OK, std::string(st.name) + " answered D3D_OK, as today");
		CHECK(syncs >= 1, std::string(st.name) + " went S (oversize)");
		CHECK(inOrder, std::string(st.name) + ": the queued command executed BEFORE the oversize call's own exec");
	}
	e.dev->EndScene();
	const uint64_t own = e.nd->csOwnerViolations() - own0;
	if (planted)
		*d += " plantable steps reversed " + std::to_string(reversed) + "/2;";
	*d += " ownership violations " + std::to_string(own);
	CHECK(own == 0, "no worker state touched on the front outside an S body");
	dlv->Release();
	dtex->Release();
	img->Release();
	vb->Release();
	tex->Release();
	bb->Release();
	return ok;
}

// the worker's stack. In a child with the submit thread OFF and CS=1, the review's input: a 20000-rectangle Clear
// ({1,1,2,2} each: all survive clipping - one vkCmdClearAttachments of 20000 rects, whose MoltenVK encode stack-allocates per
// rectangle) then Present, so the submission and its encode run on gx-native-cs. It must survive as it does on main (8 MiB).
// X-cs-deep-clear: the stream keeps std::thread's 512 KiB stack - the child dies (measured: ___chkstk_darwin). ASYNC (as 22).
bool caseChildCsDeepClear(Env& e, std::string* d)
{
	bool ok = true;
	std::vector<D3DRECT> r(20000, D3DRECT { 1, 1, 2, 2 });
	const HRESULT hc = e.dev->Clear(DWORD(r.size()), r.data(), D3DCLEAR_TARGET, 0xff336699, 1.0f, 0);
	const HRESULT hp = e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	e.nd->csSync();
	std::fprintf(stderr, "[TEST] DEEP-CLEAR ok cs=%d hr=%ld/%ld\n", e.nd->csOn() ? 1 : 0, long(hc), long(hp));
	*d += "cleared + presented";
	CHECK(hc == D3D_OK && hp == D3D_OK, "answered as today");
	return ok;
}
bool caseCsDeepClear(Env& e, std::string* d)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csWorkerDefaultStack;
	const ChildRun c = runSelf(planted ? "X-child-cs-deep-clear" : "child-cs-deep-clear", { { "GX_VK_SUBMIT_THREAD", "0" }, { "GX_NATIVE_CS", "1" } });
	*d += "child " + c.how();
	CHECK(c.exited && c.code == 0 && c.lines("[TEST] DEEP-CLEAR ok cs=1").size() == 1,
		"a 20000-rectangle Clear + Present encoded on the worker (submit thread off) without overflowing its stack");
#else
	SETUP(false, "needs posix_spawn");
#endif
	return ok;
}

// SetMaterial, SetLight(1) and SetViewport with the worker HELD - the caller overwrites its three structs
// before the release (a stack object reused, as the game does). The translator must hold the values of the CALL (its state:
// material, light 1, viewport) and draw with them: white material x green light 1 -> green inside the left-half viewport, the blue
// clear outside it. X-cs-ff-payload-{material,light,viewport} (the command keeps the caller's pointer): black / red / green past
// the half. ASYNC (worker held, as cs-constants).
bool caseCsFfPayload(Env& e, std::string* d)
{
	bool ok = true;
	struct LV { float x, y, z, nx, ny, nz; };
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff0000ff), "the blue clear (full viewport)");
	D3DMATRIX id = {};
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	e.dev->SetTransform(D3DTS_WORLD, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &id);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetRenderState(D3DRS_LIGHTING, TRUE);
	e.dev->SetRenderState(D3DRS_AMBIENT, 0);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL);
	D3DMATERIAL8 mat = {};
	mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
	mat.Power = 7.0f;
	D3DLIGHT8 l = {};
	l.Type = D3DLIGHT_DIRECTIONAL;
	l.Diffuse.g = 1.0f;
	l.Diffuse.a = 1.0f;
	l.Direction.z = 1.0f;
	l.Range = 50.0f;
	D3DVIEWPORT8 vp = { 0, 0, 32, 64, 0.0f, 1.0f };
	const D3DMATERIAL8 mat0 = mat;
	const D3DLIGHT8 l0 = l;
	const D3DVIEWPORT8 vp0 = vp;
	csarm::hold(e);
	const HRESULT h1 = e.dev->SetMaterial(&mat);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	const HRESULT h2 = e.dev->SetLight(1, &l);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->LightEnable(1, TRUE);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	const HRESULT h3 = e.dev->SetViewport(&vp);
	mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = 0.0f;   // the caller reuses its structs at once
	mat.Power = 0.0f;
	l.Diffuse.r = 1.0f;
	l.Diffuse.g = 0.0f;
	vp.Width = 64;
	csarm::release(e);
	e.nd->csSync();
	const GXRender::Rec::GxD3D8Translate& tr = e.nd->translator();
	const GXRender::GxD3D8Light* tl = tr.testLight(1);
	const bool mOk = std::memcmp(&tr.state().material, &mat0, sizeof(mat0)) == 0;
	const bool lOk = tl && std::memcmp(tl, &l0, sizeof(l0)) == 0;
	const GXRender::GxD3D8Viewport& tv = tr.state().viewport;
	const bool vOk = tv.x == vp0.X && tv.y == vp0.Y && tv.width == vp0.Width && tv.height == vp0.Height && tv.minZ == vp0.MinZ &&
		tv.maxZ == vp0.MaxZ;
	const LV q[4] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(LV));
	e.dev->EndScene();
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	const uint32_t in = csarm::px(e, bb, 16, 30), out = csarm::px(e, bb, 48, 30);
	*d += std::string("translator state as at the call: material ") + (mOk ? "yes" : "NO") + ", light 1 " + (lOk ? "yes" : "NO") +
		", viewport " + (vOk ? "yes" : "NO") + "; inside " + csarm::hex(in) + ", outside " + csarm::hex(out);
	CHECK(h1 == D3D_OK && h2 == D3D_OK && h3 == D3D_OK, "answered as today");
	CHECK(mOk && lOk && vOk, "the translator holds the material, light and viewport of the call");
	CHECK(in == 0x00ff00, "white material x green light 1 inside the viewport");
	CHECK(out == 0x0000ff, "the blue clear outside the call's viewport");
	bb->Release();
	return ok;
}

// an already-UNBOUND supported shader deleted - its record goes at once (DXVK erases an unbound one), so a
// later bind of the stale handle draws nothing (refused, the clear stays) and the verdict is 255. For Trees.vso and a ported PS:
// create, bind, draw (the shader's pixel), unbind, delete. X-strict-shader-unbound-delete (the exec half deletes only a BOUND
// shader). STRICT (serial).
bool caseStrictShaderUnboundDelete(Env& e, std::string* d)
{
	bool ok = true;
	size_t n = 0;
	const GXRender::GxCsEntry* all = GXRender::GxCsEntries(&n);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb, "the back buffer");
	csarm::ffDiffuse(e);
	for (int ps = 0; ps < 2; ++ps) {
		const GXRender::GxCsEntry* ent = nullptr;
		for (size_t i = 0; i < n && !ent; ++i)
			if (ps ? (all[i].pixel && all[i].program) : (!all[i].pixel && all[i].program && all[i].decl && std::strstr(all[i].files, "Trees.vso")))
				ent = &all[i];
		SETUP(ent, ps ? "a ported pixel shader in the catalogue" : "Trees.vso (ported, with its declaration) in the catalogue");
		std::vector<DWORD> code(ent->dwords, ent->dwords + ent->count), decl;
		if (!ps)
			decl.assign(ent->decl, ent->decl + ent->declCount);
		DWORD h = 0;
		const HRESULT hc = ps ? e.dev->CreatePixelShader(code.data(), &h) : e.dev->CreateVertexShader(decl.data(), code.data(), &h, 0);
		csarm::QV q[4];
		csarm::quad(q, 0, 0, 64, 64, 0xff3399cc);
		struct TreeV { float pos[3], normal[3]; DWORD c; float uv[2]; };
		const TreeV tq[4] = { { { -1, -1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 0, 0 } }, { { 1, -1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 1, 0 } },
			{ { -1, 1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 0, 1 } }, { { 1, 1, 0.5f }, { 0, 1, 0 }, 0xff3399cc, { 1, 1 } } };
		auto drawQuad = [&](DWORD clear) {
			clearSurface(e.dev, bb, clear);
			e.dev->BeginScene();
			if (ps)
				e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
			else
				e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(TreeV));
			e.dev->EndScene();
			csarm::obsSurface(e, bb);
			return csarm::px(e, bb, 30, 30);
		};
		auto unbind = [&] {
			if (ps)
				e.dev->SetPixelShader(0);
			e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		};
		auto bind = [&] {
			if (ps)
				e.dev->SetPixelShader(h);
			else {
				const float k[5][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 0, 0, 0, 0 } };
				e.dev->SetVertexShaderConstant(4, k, 5);
				e.dev->SetVertexShader(h);
			}
		};
		bind();
		const uint32_t drawn = drawQuad(0xff000000), drawnW = ps ? drawQuad(0xffffffff) : drawn;
		unbind();
		const HRESULT hd = ps ? e.dev->DeletePixelShader(h) : e.dev->DeleteVertexShader(h);
		e.nd->csSync();
		const uint8_t gone = ps ? e.nd->translator().testPsVerdict(h) : e.nd->translator().testVsVerdict(h);
		const auto s0 = e.nd->translator().stats();
		bind();   // the stale handle: nothing of the deleted shader may draw
		const uint32_t stale = drawQuad(0xffffffff);
		const auto s1 = e.nd->translator().stats();
		unbind();
		csarm::obsv(hc);
		csarm::obsv(hd);
		csarm::obsv(gone);
		*d += std::string(ps ? " PS" : "VS") + ": drawn " + csarm::hex(drawn) + ", record after unbind+delete " +
			(gone == 255 ? std::string("gone") : "KEPT") + ", stale bind " + csarm::hex(stale) + " (drawn +" + std::to_string(s1.drawn - s0.drawn) +
			" refused +" + std::to_string(s1.refused - s0.refused) + ");";
		CHECK(hc == D3D_OK && hd == D3D_OK, "answered as today");
		CHECK(ps ? drawn == drawnW : rgbNear(drawn, 0x3399cc), "the shader drew (its own pixel over black and white) before it was unbound");
		CHECK(gone == 255, "an unbound shader's record goes at its deletion");
		CHECK(stale == 0xffffff && s1.drawn == s0.drawn && s1.refused == s0.refused + 1, "a stale handle draws nothing (refused, the clear stays)");
	}
	bb->Release();
	return ok;
}

// MultiplyTransform (an S body: the census never sees the game call it) really multiplies - WORLD, VIEW and
// TEXTURE2 set to a known matrix A, multiplied by a distinct non-commuting B: all 16 GetTransform floats bit-equal to the product
// in the translator's own order (m x current in this row-major storage, k ascending). X-strict-multiply (the CS=1 S body keeps the
// drain, the accounting and D3D_OK, and skips the multiplication). STRICT (serial).
bool caseStrictMultiply(Env& e, std::string* d)
{
	bool ok = true;
	D3DMATRIX a = {}, b = {};
	for (int i = 0; i < 16; ++i) {
		(&a._11)[i] = 0.5f + float(i) * 0.25f;
		(&b._11)[i] = float((i * 7) % 5) - 1.5f + (i == 3 ? 0.125f : 0.0f);
	}
	float want[16];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k)
				s += (&b._11)[i * 4 + k] * (&a._11)[k * 4 + j];
			want[i * 4 + j] = s;
		}
	float other[16];   // a x b: what the reversed order would give - must differ, or the arm proves nothing
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			float s = 0.f;
			for (int k = 0; k < 4; ++k)
				s += (&a._11)[i * 4 + k] * (&b._11)[k * 4 + j];
			other[i * 4 + j] = s;
		}
	SETUP(std::memcmp(want, other, 64) != 0 && std::memcmp(want, &a._11, 64) != 0, "A and B do not commute and B x A != A");
	const D3DTRANSFORMSTATETYPE types[3] = { D3DTS_WORLD, D3DTS_VIEW, D3DTS_TEXTURE2 };
	for (const D3DTRANSFORMSTATETYPE t : types) {
		e.dev->SetTransform(t, &a);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		const HRESULT hm = e.dev->MultiplyTransform(t, &b);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
		D3DMATRIX got = {};
		const HRESULT hg = e.dev->GetTransform(t, &got);
		e.nd->csSync();
		const bool trOk = std::memcmp(t == D3DTS_WORLD ? e.nd->translator().state().world : t == D3DTS_VIEW ? e.nd->translator().state().view :
			e.nd->translator().state().texture[2], want, 64) == 0;
		csarm::obsv(hm);
		csarm::obsv(hg);
		csarm::obsv(got);
		const bool gOk = std::memcmp(&got._11, want, 64) == 0;
		*d += " type " + std::to_string(int(t)) + (gOk ? " product" : " NOT the product") + (trOk ? "" : " (translator differs)") + ";";
		CHECK(hm == D3D_OK && hg == D3D_OK, "answered as today");
		CHECK(gOk && trOk, "GetTransform and the translator hold B x A, bit-exact");
	}
	// then DRAW through a multiplied WORLD (the tap and the pixels depend on it - gate f95565396: X- moved neither): WORLD = translate
	// x +0.5, MultiplyTransform(scale x 0.5) -> x' = 0.5x + 0.5 (scale first: m x current). A quad over x in [-0.5, 0.5] lands on
	// pixels 40-56; unmultiplied (translate only) on 32-64 - pixel 34 tells them apart, 48 is inside both
	D3DMATRIX id = {}, tr = {}, sc = {};
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	tr = id;
	tr._41 = 0.5f;
	sc = id;
	sc._11 = 0.5f;
	e.dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	e.dev->SetTransform(D3DTS_PROJECTION, &id);
	e.dev->SetTransform(D3DTS_WORLD, &tr);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	const HRESULT hw = e.dev->MultiplyTransform(D3DTS_WORLD, &sc);	// GX-WRAPSKIP-OK: a test device, no DX8Wrapper in this process
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	SETUP(bb && clearSurface(e.dev, bb, 0xff000000), "the black clear");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	struct XV { float x, y, z; DWORD c; };
	const XV q[4] = { { -0.5f, 0.5f, 0.5f, 0xff00ff00 }, { 0.5f, 0.5f, 0.5f, 0xff00ff00 }, { -0.5f, -0.5f, 0.5f, 0xff00ff00 }, { 0.5f, -0.5f, 0.5f, 0xff00ff00 } };
	e.dev->BeginScene();
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(XV));
	e.dev->EndScene();
	const uint32_t out = csarm::px(e, bb, 34, 32), in = csarm::px(e, bb, 48, 32);
	csarm::obsSurface(e, bb);
	bb->Release();
	*d += " drawn: x34 " + csarm::hex(out) + ", x48 " + csarm::hex(in);
	CHECK(hw == D3D_OK && out == 0 && in == 0x00ff00, "the draw goes through the multiplied WORLD (scaled, then translated)");
	return ok;
}

// a TERMINAL dynamic-slice failure is counted and logged. Every slice of the buffer is read by the open frame
// (none can be reused), then a DISCARD whose slice allocations all fail (3 armed; measured: 2 used - the front attempt escapes to S,
// whose retry recovers storage first and then fails terminally) answers INVALIDCALL with no pointer, alloc_failures +1, one
// "allocation failed: slice" line.
// X-cs-slice-terminal-silent: not counted, no line. ASYNC (31-class).
bool caseCsSliceTerminal(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(QV), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
		D3DPOOL_DEFAULT, &vb)) && vb, "dynamic VB");
	csarm::ffDiffuse(e);
	e.dev->SetStreamSource(0, vb, sizeof(QV));
	e.dev->BeginScene();
	BYTE* p = nullptr;
	// slices read by the frame still being recorded: a DISCARD reuses an idle slice first, so cycle (draining each time) until a
	// cycle had to CREATE one - then every slice of the buffer is read by the open frame and none can be reused
	for (int i = 0; i < 16; ++i) {
		const uint64_t created0 = e.nd->resources().stats().slicesCreated;
		SETUP(SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_DISCARD)) && p, "a DISCARD in the open frame");
		csarm::quad(reinterpret_cast<QV*>(p), 0, 0, 8, 8, 0xff112233);
		vb->Unlock();
		e.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		e.nd->csSync();
		if (i > 0 && e.nd->resources().stats().slicesCreated > created0)
			break;
	}
	const uint64_t f0 = e.nd->allocFailures();
	const size_t log0 = logSnapshot().size();
	e.nd->debug().allocFailSite = "slice";
	e.nd->debug().allocFailCount = 3;
	p = reinterpret_cast<BYTE*>(1);
	const HRESULT hr = vb->Lock(0, 0, &p, D3DLOCK_DISCARD);
	e.dev->EndScene();
	e.dev->SetStreamSource(0, nullptr, 0);
	const std::vector<std::string> log = logSnapshot();
	int lines = 0;
	for (size_t i = log0; i < log.size(); ++i)
		lines += log[i].find("[GXNATIVE] allocation failed: slice") != std::string::npos;
	const uint64_t af = e.nd->allocFailures() - f0;
	const GXRender::Res::GxStats rs = e.nd->resources().stats();
	*d += "Lock hr " + std::to_string(long(hr)) + ", alloc_failures +" + std::to_string(af) + ", lines " + std::to_string(lines) +
		" [diag: injections left " + std::to_string(e.nd->debug().allocFailCount) + ", slices created " + std::to_string(rs.slicesCreated) +
		", reuses " + std::to_string(rs.sliceReuses) + ", overflows " + std::to_string(rs.sliceOverflows) + ", syncs " +
		std::to_string(e.nd->csSyncCount()) + "]";
	CHECK(hr == D3DERR_INVALIDCALL && p == nullptr, "INVALIDCALL with no pointer, as today");
	CHECK(af == 1 && lines == 1, "the terminal slice failure is counted and logged once");
	e.nd->debug().allocFailSite = nullptr;
	if (hr == D3D_OK)
		vb->Unlock();
	vb->Release();
	return ok;
}

// a recovery after a LATCHED refusal must free nothing - collect() would settle every retirement,
// the open frame's included. Frame N's submit held on gx-vk-submit; Present N; frame N+1 opened (Clear) with a retirement filed under
// its unsubmitted value; N's submit refused (latch + drain); a creation whose first attempt fails -> the S retry's recovery. The
// open frame's retirement must not have run. X-cs-recover-ignores-latch (collect regardless) runs it. ASYNC (holds gx-vk-submit).
bool caseCsRecoverLatched(Env& e, std::string* d)
{
	bool ok = true;
	V::VkCtx& ctx0 = e.nd->ctx();
	SETUP(ctx0.submitThreaded(), "the submit thread is on");
	SETUP(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "two warm-up frames");
	V::VkCtx& ctx = e.nd->ctx();   // re-taken: drains
	ctx.waitIdle();
	const uint64_t fv = ctx.queue(V::QueueKind::Graphics).timeline.lastSubmitted() + 1;
	ctx.testHoldAt(fv);
	SETUP(presentClear(e, 0xff204060), "frame N, its submit held");
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff336699, 1.0f, 0);   // frame N+1 opens on the worker
	V::VkCtx& c2 = e.nd->ctx();   // drains: N+1 is open, N's submit still held
	static std::atomic<bool> ran { false };
	ran = false;
	const uint64_t openValue = c2.queue(V::QueueKind::Graphics).timeline.pending();
	c2.retire(V::QueueKind::Graphics, openValue, [] { ran = true; });
	c2.testSet([&](V::TestMutants& m) { m.submitFailAt = fv; });
	c2.testRelease();
	for (int i = 0; i < 2000 && !c2.deviceLost(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	SETUP(c2.deviceLost(), "frame N's submit refused and latched");
	e.nd->debug().allocFailSite = "image";
	e.nd->debug().allocFailCount = 1;
	IDirect3DTexture8* t = nullptr;
	const HRESULT hr = e.dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
	e.nd->debug().allocFailSite = nullptr;
	*d += "open-frame retirement ran: " + std::string(ran ? "YES" : "no") + ", CreateTexture hr " + std::to_string(long(hr)) +
		", recoveries at CreateTexture " + std::to_string(e.nd->recoveriesAt("CreateTexture"));
	CHECK(e.nd->recoveriesAt("CreateTexture") == 1, "the creation's S retry entered recovery");
	CHECK(!ran, "a recovery after a latched refusal freed nothing of the open frame");
	c2.testSet([&](V::TestMutants& m) { m.submitFailAt = 0; });
	if (t)
		t->Release();
	if (bb)
		bb->Release();
	return ok;
}

// the WORKER's recovery after a latched refusal frees nothing either. As thread-cs-recover-latched, but the recovery is
// forced on the worker: a queued 12 KiB static unlock must stage through a DEDICATED buffer (8 KiB TEST staging ring) whose first
// allocation fails. X-cs-recover-ignores-latch-worker (the guard bypassed in the worker's context only) runs the open frame's
// retirement. ASYNC (holds gx-vk-submit).
bool caseCsRecoverLatchedWorker(Env& e, std::string* d)
{
	bool ok = true;
	using csarm::QV;
	SETUP(e.nd->resources().ring().capacity() <= (8u << 10), "the 8 KiB TEST staging ring");
	V::VkCtx& ctx0 = e.nd->ctx();
	SETUP(ctx0.submitThreaded(), "the submit thread is on");
	IDirect3DVertexBuffer8* vb = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateVertexBuffer(12u << 10, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb)) && vb, "12 KiB static VB");
	SETUP(presentClear(e, 0xff204060) && presentClear(e, 0xff204060), "two warm-up frames");
	V::VkCtx& ctx = e.nd->ctx();
	ctx.waitIdle();
	const uint64_t fv = ctx.queue(V::QueueKind::Graphics).timeline.lastSubmitted() + 1;
	ctx.testHoldAt(fv);
	SETUP(presentClear(e, 0xff204060), "frame N, its submit held");
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff336699, 1.0f, 0);   // frame N+1 opens on the worker
	V::VkCtx& c2 = e.nd->ctx();
	static std::atomic<bool> ran { false };
	ran = false;
	c2.retire(V::QueueKind::Graphics, c2.queue(V::QueueKind::Graphics).timeline.pending(), [] { ran = true; });
	c2.testSet([&](V::TestMutants& m) { m.submitFailAt = fv; });
	c2.testRelease();
	for (int i = 0; i < 2000 && !c2.deviceLost(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	SETUP(c2.deviceLost(), "frame N's submit refused and latched");
	const uint64_t r0 = e.nd->recoveriesAt("upload-dedicated");
	e.nd->debug().allocFailSite = "upload-dedicated";
	e.nd->debug().allocFailCount = 1;
	BYTE* p = nullptr;
	SETUP(SUCCEEDED(vb->Lock(0, 0, &p, 0)) && p, "lock the VB");
	std::memset(p, 0x5a, 12u << 10);
	vb->Unlock();   // A: the worker stages it - a dedicated buffer, first attempt fails -> the WORKER's recovery
	e.nd->csSync();
	e.nd->debug().allocFailSite = nullptr;
	const uint64_t r = e.nd->recoveriesAt("upload-dedicated") - r0;
	*d += "worker recoveries " + std::to_string(r) + ", open-frame retirement ran: " + (ran ? "YES" : "no");
	CHECK(r == 1, "the upload's staging failure entered the worker's recovery");
	CHECK(!ran, "a worker recovery after a latched refusal freed nothing of the open frame");
	c2.testSet([&](V::TestMutants& m) { m.submitFailAt = 0; });
	vb->Release();
	return ok;
}

// render-integration-5's condition before default ON (13/10 ~10:5xZ): a process that ends through the NORMAL exit - atexit hooks, then
// static destruction (Windows: WinMain returns -> CRT exit) - right after a frame whose queued draws need NEW pipelines. The child
// holds the worker before draws in states no prebuilt pipeline has (they miss at the exit flush and queue specialised compiles),
// leaks the device and returns from main. After the flush an atexit observer (registered before any device) prints the cache's
// live spec workers - 0 required - then static destruction runs: exit 0, the final line once, no abort. X-cs-exit-spec-running
// (the settle leaves the workers running) -> workers alive after the line. On Windows: run `--arm child-exit-crt` DIRECTLY (no
// posix_spawn there) and check exit 0, one final line and "spec workers alive=0".
// A60: a static constructed before main, so its destructor runs AFTER the device's atexit hook (registered later - LIFO): its line proves
// the CRT exit went on past the hook (static destruction ran); a process the A60 watchdog ended never prints it
namespace csexit {
bool g_sentinelOn = false;
struct Sentinel
{
	~Sentinel()
	{
		if (g_sentinelOn) {
			std::fprintf(stderr, "[TEST] cs-exit: static destruction ran\n");
			std::fflush(stderr);
		}
	}
} g_sentinel;
} // namespace csexit
GxNativeDevice* g_crtDevice = nullptr;
void crtReport()   // atexit, registered in main before any device: runs after the device's flush hook
{
	std::fprintf(stderr, "[TEST] exit-crt: spec workers alive=%zu\n", g_crtDevice ? g_crtDevice->testSpecWorkers() : size_t(999));
	std::fflush(stderr);
}
bool caseChildExitCrt(Env& e, std::string* d)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	SETUP(presentClear(e, 0xff204060), "a first frame");
	csarm::ffDiffuse(e);
	csarm::hold(e);
	e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 1);   // the hold: everything below runs in the exit flush's drain
	const DWORD ops[] = { D3DTOP_MODULATE, D3DTOP_ADD, D3DTOP_SUBTRACT, D3DTOP_MODULATE2X, D3DTOP_ADDSIGNED, D3DTOP_BLENDDIFFUSEALPHA };
	csarm::QV q[4];
	csarm::quad(q, 0, 0, 16, 16, 0xff6699cc);
	e.dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
	e.dev->BeginScene();
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
		e.dev->SetRenderState(D3DRS_FOGCOLOR, 0xff000000u | DWORD(i * 40));
		e.dev->SetTextureStageState(0, D3DTSS_COLOROP, ops[i]);
		e.dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(csarm::QV));
	}
	e.dev->EndScene();
	GxNativeCs* cs = &e.nd->csStream();
	GxNativeDevice* nd = e.nd;
	std::thread([cs, nd] {   // the exit flush drains the held worker: release it once the flush waits (A60: the CRT-exit drain polls -
		// testExitDraining; an explicit flush still sleeps on the producer condition variable)
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!(cs->testHeld() && (cs->testProducerWaiting() || nd->testExitDraining())) && std::chrono::steady_clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		cs->testRelease();
	}).detach();
	// E · seam-worker-2 (dlss-integrator's ruling 18/10): main returns only once the worker has reached the hold - everything before it
	// (the first frame's Present included) has run, so the exit flush starts from the state this arm means. Returning earlier let exit()
	// destroy the validation layer's function-local statics first built ON THE WORKER during that Present (registered after the device's
	// atexit hook, so destroyed BEFORE it - LIFO) while the worker still used them: SIGSEGV in VVL's GetCopyBufferImageDeviceVUID, ~1 run
	// in 10 on some builds (seam2 logs: base 0/60, C 8/60, B'' 4/40; --no-validation 0/40; this wait 0/60).
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!cs->testHeld() && std::chrono::steady_clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(cs->testHeld(), "the worker never reached the hold before main returned");
	}
	g_crtDevice = e.nd;
	csexit::g_sentinelOn = true;
	std::fprintf(stderr, "[TEST] exit-crt: ready\n");
	e.devReleased = true;   // leaked: the process ends through its normal exit with the draws still queued
	e.keepWindow = true;
	*d += "device leaked with draws that miss the pipeline cache queued behind the hold";
	return ok;
}
bool exitCrtParent(Env& e, std::string* d, const std::string& child)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = e.nd->debug().csExitNoSpecQuiesce;
	const ChildRun c = runSelf(planted ? "X-" + child : child, { { "GX_NATIVE_CS", "1" } });
	const std::vector<std::string> f = c.lines("[GXNATIVE] stats final"), w = c.lines("[TEST] exit-crt: spec workers alive=");
	*d += "child " + c.how() + ", final lines " + std::to_string(f.size()) + (w.empty() ? ", no worker report" : ", " + w[0].substr(7));
	SETUP(c.lines("[TEST] exit-crt: ready").size() == 1, "the child left draws queued that miss the cache");
	CHECK(f.size() == 1 && f[0].find(" end=exit") != std::string::npos, "the final line once, end=exit");
	CHECK(w.size() == 1 && w[0] == "[TEST] exit-crt: spec workers alive=0", "no pipeline worker runs after the line");
	CHECK(c.exited && !c.signaled && (planted || c.code == 0), "the normal exit (static destruction) ends with exit 0, no abort");
	CHECK(planted || c.lines("[TEST] cs-exit: static destruction ran").size() == 1, "the CRT exit went on past the hook (A60: no early end)");
#else
	SETUP(false, "needs posix_spawn - on Windows run --arm child-exit-crt-nowin directly");
#endif
	return ok;
}
bool caseCsExitCrt(Env& e, std::string* d) { return exitCrtParent(e, d, "child-exit-crt"); }
// rw3-X-3: the same exit without a window (the Windows standalone test has no SDL3 window, so child-exit-crt SKIPs there): the
// first frame is an offscreen Present. Windows runs `--arm child-exit-crt-nowin` DIRECTLY with GX_NATIVE_CS=1.
bool caseCsExitCrtNowin(Env& e, std::string* d) { return exitCrtParent(e, d, "child-exit-crt-nowin"); }

// A60 (PREREG_RF A60 am. 2): the CRT-exit quiesce and its watchdog. Each child holds the worker before gated commands, leaks the device and
// returns from main; a helper releases the hold once the hook's quiesce started (testExitDraining). The parent sets the watchdog's limit to
// 1500 ms (GX_NATIVE_EXIT_STALL_MS, TEST-ONLY). A TEST-ONLY exec sink gates SetRenderState (TEXTUREFACTOR, 0x5EED0000 + i) on the worker:
//  child-cs-exit-stuck: i = 0 prints "worker at the gate" and blocks on a mutex main holds through exit - the portable form of the UCRT
//    exit lock (the hook's caller holds what the worker needs): the watchdog ends the process - the give-up line once, code 3, no final
//    line, and NO static destruction after it (the sentinel). X-cs-exit-stuck (no watchdog) hangs at the gate.
//  child-cs-exit-slow: i = 0..3 each sleep 1000 ms (each < 1500, together > 1500): drained in full - code 0, the final line once, no
//    give-up line, the sentinel line. X-cs-exit-slow (the watchdog ignores progress) = the give-up line.
//  child-cs-exit-onexit (the real shape, Windows): i = 0 calls std::atexit FROM THE WORKER during the exit quiesce - UCRT blocks it on the
//    exit lock the hook runs under. No parent (no posix_spawn there): Windows runs it directly - code 0 + the final line, or code 3 + the
//    give-up line, never a hang; its X- (no watchdog) hangs.
namespace csexit {
enum class Mode { Stuck, Slow, OnExit };
std::mutex* g_gate = nullptr;   // leaked: main holds it through exit
void noop() {}
struct Sink final : GXRender::Dev::GxCsExecSink
{
	Mode mode;
	explicit Sink(Mode m) : mode(m) {}
	void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg* a, size_t n, const GXRender::Dev::GxCsBytes*) override
	{
		if (op != GXRender::Dev::GxCsOp::RenderState || n < 2 || a[0].v != D3DRS_TEXTUREFACTOR || (a[1].v & 0xffff0000u) != 0x5EED0000u)
			return;
		std::fprintf(stderr, "[TEST] cs-exit: worker at the gate %u\n", unsigned(a[1].v & 0xffffu));
		std::fflush(stderr);
		if (mode == Mode::Stuck)
			g_gate->lock();   // never returns: main holds it
		else if (mode == Mode::Slow)
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		else
			std::atexit(noop);
	}
	void destroyed(const void*) override {}
	void created(uint64_t, uint32_t, const GXRender::Dev::GxCsArg*, size_t) override {}
};
bool child(Env& e, std::string* d, Mode mode)
{
	bool ok = true;
	SETUP(e.nd->csOn(), "the worker is on");
	SETUP(presentClear(e, 0xff204060), "a first frame");
	// the worker idle BEFORE the sink appears: it reads g_csExecSink relaxed, so a sink stored while it still runs the frame above is a
	// vptr race (TSan, 15/10); the publish of the gated commands below orders the construction before any worker read
	e.nd->csSync();
	GXRender::Dev::g_csExecSink.store(new Sink(mode), std::memory_order_release);   // leaked: the worker may be inside it at exit
	if (mode == Mode::Stuck) {
		g_gate = new std::mutex;
		g_gate->lock();
	}
	csarm::hold(e);
	for (DWORD i = 0, n = mode == Mode::Slow ? 4 : 1; i < n; ++i)
		e.dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x5EED0000u + i);
	GxNativeDevice* nd = e.nd;
	std::thread([nd] {   // the CRT-exit quiesce started: release the hold - the gated commands run under it
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!(nd->csStream().testHeld() && nd->testExitDraining()) && std::chrono::steady_clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		nd->csStream().testRelease();
	}).detach();
	g_sentinelOn = true;
	std::fprintf(stderr, "[TEST] cs-exit: ready\n");
	e.devReleased = true;   // leaked: the process ends through its normal exit with the gated commands queued
	e.keepWindow = true;
	*d += "device leaked with gated commands behind the hold";
	return ok;
}
bool parent(Env& e, std::string* d, const char* arm, bool slow)
{
	bool ok = true;
#if !defined(_WIN32)
	const bool planted = slow ? e.nd->debug().csExitWatchNoProgress : e.nd->debug().csExitUnwatched;
	const ChildRun c = runSelf(planted ? std::string("X-") + arm : std::string(arm), { { "GX_NATIVE_CS", "1" }, { "GX_NATIVE_EXIT_STALL_MS", "1500" } },
		40);
	const std::vector<std::string> gave = c.lines("[GXNATIVE] exit:"), fin = c.lines("[GXNATIVE] stats final"),
		gate = c.lines("[TEST] cs-exit: worker at the gate"), sen = c.lines("[TEST] cs-exit: static destruction ran");
	*d += "child " + c.how() + ", gate " + std::to_string(gate.size()) + ", give-up " + std::to_string(gave.size()) + ", final " +
		std::to_string(fin.size()) + ", static destruction " + std::to_string(sen.size());
	SETUP(c.lines("[TEST] cs-exit: ready").size() == 1, "the child left gated commands behind the hold");
	if (slow && planted)   // judged by ITS symptom only: the watchdog fired DURING the drain (stage 1) after a command completed and the
		// next one started (gate >= 2) - anything else fails this X- arm
		CHECK(!(gave.size() == 1 && gave[0].find("(stage 1,") != std::string::npos && gate.size() >= 2 && c.exited &&
				  c.code == GxNativeDevice::kExitTerminalCode),
			"a slow but progressing exit drain was abandoned");
	else if (slow)
		CHECK(gate.size() == 4 && gave.empty() && fin.size() == 1 && fin[0].find(" end=exit") != std::string::npos && sen.size() == 1 &&
				c.exited && !c.signaled && c.code == 0,
			"a slow but progressing exit drain: in full - the final line once, no give-up, static destruction after it, exit 0");
	else if (planted)   // judged by ITS symptom only: a hang with the worker at the gate
		CHECK(!(c.timedOut && gate.size() == 1 && gave.empty()), "the CRT exit hung waiting for a stuck worker");
	else
		CHECK(gate.size() == 1 && gave.size() == 1 && fin.empty() && sen.empty() && c.exited && !c.signaled &&
				c.code == GxNativeDevice::kExitTerminalCode,
			"a stuck worker at the CRT exit: the watchdog's line once, exit code 3, no final line, no static destruction after it");
#else
	SETUP(false, "needs posix_spawn - on Windows run --arm child-cs-exit-stuck / child-cs-exit-slow / child-cs-exit-onexit directly");
#endif
	return ok;
}
} // namespace csexit
bool caseChildCsExitStuck(Env& e, std::string* d) { return csexit::child(e, d, csexit::Mode::Stuck); }
bool caseChildCsExitSlow(Env& e, std::string* d) { return csexit::child(e, d, csexit::Mode::Slow); }
bool caseChildCsExitOnExit(Env& e, std::string* d) { return csexit::child(e, d, csexit::Mode::OnExit); }
bool caseCsExitStuck(Env& e, std::string* d) { return csexit::parent(e, d, "child-cs-exit-stuck", false); }
bool caseCsExitSlow(Env& e, std::string* d) { return csexit::parent(e, d, "child-cs-exit-slow", true); }

// ---------------------------------------------------------------------------------------------- A23-B EXEC digest (§7.1)
// The sink (ExecDigest) lives in gx_exec_digest.h, shared with gx_proxy_test.

// ------------------------------------------------------------------------------------------------ motion (E · render-motion-1)
// DESIGN_motion.md rev 2 §12: sampleable depth, camera motion vectors from the resolve pass, front-side projection jitter. The
// arms drive GxNativeDevice::worldBegin/worldEnd (the IGxWorldPass bodies) directly, on explicitly created 64x64 depth surfaces (the temporal arms through worldDepth(), below).

// The world depth the seam creates in temporal mode: NextTargetsAreWorld(GXWP_TEMPORAL), then CreateDepthStencilSurface.
IDirect3DSurface8* worldDepth(Env& e, bool temporal = true)
{
	if (temporal)
		e.nd->worldNextTargets(GXWP_TEMPORAL);
	IDirect3DSurface8* ds = nullptr;
	e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &ds);
	return ds;
}

void mIdentity(float* m)
{
	std::memset(m, 0, 64);
	m[0] = m[5] = m[10] = m[15] = 1.f;
}

// D3D left-handed perspective (w = view z), x/y scale sx/sy, planes n/f
void mPerspective(float* p, float sx, float sy, float n, float f)
{
	std::memset(p, 0, 64);
	p[0] = sx;
	p[5] = sy;
	p[10] = f / (f - n);
	p[11] = 1.f;
	p[14] = -n * f / (f - n);
}

// world -> view for a camera at (x, y, z) looking down +z
void mCamera(float* v, float x, float y, float z)
{
	mIdentity(v);
	v[12] = -x;
	v[13] = -y;
	v[14] = -z;
}

struct MQuad { float x0, x1, y0, y1, z; DWORD c; };

void drawWorldQuad(IDirect3DDevice8* dev, const MQuad& q)
{
	struct V { float x, y, z; DWORD c; };
	const V v[4] = { { q.x0, q.y1, q.z, q.c }, { q.x1, q.y1, q.z, q.c }, { q.x0, q.y0, q.z, q.c }, { q.x1, q.y0, q.z, q.c } };
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

// One frame: clear, world pass (camera view/proj, the world quads), then the 'after' quads with the projection left as it is
// (what the HUD meets after End). present = end the frame with Present.
bool motionFrame(Env& e, IDirect3DSurface8* ds, const float* view, const float* proj, bool temporal, const std::vector<MQuad>& world,
	const std::vector<MQuad>& after, bool present, std::string* d, D3DMATRIX* projInWorld = nullptr,
	IDirect3DSurface8* object = nullptr, bool quiesce = false)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && ds && SUCCEEDED(e.dev->SetRenderTarget(bb, ds)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "clear");
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	float world4[16];
	mIdentity(world4);
	e.dev->BeginScene();
	GxMotionBegin mb;
	mb.renderW = mb.outputW = 64;
	mb.renderH = mb.outputH = 64;
	mb.temporal = temporal;
	std::memcpy(mb.view, view, 64);
	std::memcpy(mb.proj, proj, 64);
	mb.nearZ = 1.f;
	mb.farZ = 100.f;
	mb.fovYRad = 1.5707963f;
	if (quiesce)
		(void)e.nd->stats();   // E · seam-worker-2: a planted (X-) run idles the worker before the entry point (an S accessor: drains)
	if (bb && ds)
		e.nd->worldBegin(mb, bb, ds);
	e.dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(world4));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
	if (projInWorld)
		e.dev->GetTransform(D3DTS_PROJECTION, projInWorld);
	for (const MQuad& q : world)
		drawWorldQuad(e.dev, q);
	if (object)
		e.nd->worldObjectVelocity(object);   // what IGxWorldPass::SetObjectVelocity does, right before End (units-1's order)
	if (quiesce)
		(void)e.nd->stats();
	e.nd->worldEnd();
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	for (const MQuad& q : after)
		drawWorldQuad(e.dev, q);
	e.dev->EndScene();
	if (present)
		CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	if (bb)
		bb->Release();
	return ok;
}

float halfToFloat(uint16_t h)
{
	const uint32_t s = (h >> 15) & 1, ex = (h >> 10) & 31, m = h & 1023;
	float v = ex == 0 ? std::ldexp(float(m), -24) : ex == 31 ? INFINITY : std::ldexp(float(m | 1024), int(ex) - 25);
	return s ? -v : v;
}

// Max |mv - expect(x, y)| over the whole image; false when the read-back failed.
template <class F>
bool mvError(Env& e, F expect, float* maxErr, std::string* d)
{
	std::vector<uint8_t> px;
	uint32_t w = 0, h = 0;
	if (!e.nd->testReadMotion(&px, &w, &h) || w != 64 || h != 64 || px.size() < size_t(w) * h * 4) {
		*d += " no motion vectors read back";
		return false;
	}
	*maxErr = 0.f;
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x) {
			uint16_t r, g;
			std::memcpy(&r, &px[(size_t(y) * w + x) * 4], 2);
			std::memcpy(&g, &px[(size_t(y) * w + x) * 4 + 2], 2);
			float ex, ey;
			expect(float(x) + 0.5f, float(y) + 0.5f, &ex, &ey);
			*maxErr = std::max(*maxErr, std::max(std::fabs(halfToFloat(r) - ex), std::fabs(halfToFloat(g) - ey)));
		}
	return true;
}

const MQuad kPlane = { -50.f, 50.f, -50.f, 50.f, 10.f, 0xff808080 };   // fills the view at z 10 (sx = 1: half-width 10)

// motion-off: with no NextTargetsAreWorld(GXWP_TEMPORAL) announcement the depth is created exactly as before (no SAMPLED, no sample view), a world pass
// changes nothing and GetTransform answers what was set. Planted: SAMPLED regardless of the key.
bool caseMotionOff(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetDepthStencilSurface(&ds);
	GxNativeSurface* n = static_cast<GxNativeSurface*>(ds);
	CHECK(n && n->renderTarget() && n->renderTarget()->depthSampleView() == VK_NULL_HANDLE, "auto depth got a sample view with the key off");
	IDirect3DSurface8* ds2 = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &ds2)) && ds2, "create depth");
	GxNativeSurface* n2 = static_cast<GxNativeSurface*>(ds2);
	CHECK(n2 && n2->renderTarget() && n2->renderTarget()->depthSampleView() == VK_NULL_HANDLE, "created depth got a sample view");
	float v[16], p[16];
	mCamera(v, 0.f, 0.f, 0.f);
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	D3DMATRIX got = {};
	ok &= motionFrame(e, ds2, v, p, true, { kPlane }, {}, true, d, &got);
	CHECK(std::memcmp(&got, p, 64) == 0, "GetTransform(PROJECTION) differs from what was set");
	CHECK(e.nd->motionFrame() == nullptr, "motion ran with the key off");
	if (ds2)
		ds2->Release();
	if (ds)
		ds->Release();
	return ok;
}

// mv-camera: a camera pan of (+0.9375, +0.625) world units at a plane 10 ahead (sx = sy = 1, 64 px) moves the scene 3 px left
// and 2 px down: every pixel's vector (current -> previous, +y down) is (+3, -2). Planted: clipToPrevClip = identity.
bool caseMvCamera(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	float v1[16], v2[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 0.9375f, 0.625f, 0.f);
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d);
	ok &= motionFrame(e, wd, v2, p, true, { kPlane }, {}, false, d);
	float err = 1e9f;
	CHECK(mvError(e, [](float, float, float* x, float* y) { *x = 3.f; *y = -2.f; }, &err, d), "read back");
	*d += " max error " + std::to_string(err);
	CHECK(err < 0.02f, "vectors are not (+3, -2)");
	const GxMotionFrame* f = e.nd->motionFrame();
	CHECK(f && !(f->flags & GXMF_RESET) && f->frameIndex == 2, "frame flags / index");
	// a spatial pass (not GXWP_TEMPORAL) publishes nothing; the next temporal pass restarts history
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	ok &= motionFrame(e, wd, v2, p, false, { kPlane }, {}, true, d);
	CHECK(e.nd->motionFrame() == nullptr, "a spatial pass published motion");
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, false, d);
	f = e.nd->motionFrame();
	CHECK(f && (f->flags & GXMF_RESET), "the temporal pass after a spatial one kept its history");
	if (wd)
		wd->Release();
	return ok;
}

// mv-zoom-jitter: the camera moves 2 forward toward the plane (10 -> 8 ahead): the field is radial, mv(u) = -0.2 * (u - c) at the
// UN-jittered position u = p - jitter, c = the world viewport's centre (32.5, 32.5: D3D9's 0.5 offset). Frame 2's jitter is
// Halton index 2 = (-0.25, +1/6). Planted: the resolve keeps the jitter (u = p): off by 0.2 * jitter = 0.05 px.
bool caseMvZoomJitter(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	e.nd->setTemporalConsumer(true);   // a temporal backend consumes the jitter
	float v1[16], v2[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 0.f, 0.f, 2.f);
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d);
	ok &= motionFrame(e, wd, v2, p, true, { kPlane }, {}, false, d);
	const GxMotionFrame* f = e.nd->motionFrame();
	CHECK(f && std::fabs(f->jitterPx[0] + 0.25f) < 1e-6f && std::fabs(f->jitterPx[1] - 1.f / 6.f) < 1e-6f, "frame 2 jitter is not Halton 2");
	const float jx = f ? f->jitterPx[0] : 0.f, jy = f ? f->jitterPx[1] : 0.f;
	float err = 1e9f;
	CHECK(mvError(e, [&](float px, float py, float* x, float* y) {
		*x = -0.2f * (px - jx - 32.5f);
		*y = -0.2f * (py - jy - 32.5f);
	}, &err, d), "read back");
	*d += " max error " + std::to_string(err);
	CHECK(err < 0.015f, "radial field at the un-jittered position");
	if (wd)
		wd->Release();
	return ok;
}

// jitter-sign: a quad covering fb x > 32.6, y > 32.4 (the world viewport maps NDC 0 to 32.5). Frame 1 jitter (0, -1/6): column 32
// (centre 32.5) uncovered, row 32 covered - this also pins the 0.5 centre (with centre 32.0 column 32 would be covered). Frame 2
// jitter (-0.25, +1/6) moves the raster left/down: column 32 covered, row 32 uncovered. GetTransform in the pass = the clean matrix.
// Planted: the offset's sign flipped.
bool caseJitterSign(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	e.nd->setTemporalConsumer(true);   // a temporal backend consumes the jitter
	float v[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v, 0.f, 0.f, 0.f);
	const MQuad edge = { 0.1f / 32.f * 10.f, 50.f, -50.f, 0.1f / 32.f * 10.f, 10.f, 0xffffffff };
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	uint32_t c32 = 0, c33 = 0, r31 = 0, r32 = 0;
	D3DMATRIX got = {};
	ok &= motionFrame(e, wd, v, p, true, { edge }, {}, false, d, &got);
	CHECK(bb && readPixel(e.dev, bb, 32, 40, &c32) && readPixel(e.dev, bb, 33, 40, &c33) && readPixel(e.dev, bb, 40, 31, &r31) &&
		readPixel(e.dev, bb, 40, 32, &r32), "read frame 1");
	*d += " f1 c32=" + std::to_string(c32 & 0xff) + " c33=" + std::to_string(c33 & 0xff) + " r31=" + std::to_string(r31 & 0xff) +
		" r32=" + std::to_string(r32 & 0xff);
	CHECK((c32 & 0xff) == 0 && (c33 & 0xff) == 0xff && (r31 & 0xff) == 0 && (r32 & 0xff) == 0xff, "frame 1 coverage (jitter 0, -1/6)");
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	ok &= motionFrame(e, wd, v, p, true, { edge }, {}, false, d, &got);
	CHECK(std::memcmp(&got, p, 64) == 0, "GetTransform(PROJECTION) answered the jittered matrix");
	uint32_t r33 = 0;
	CHECK(bb && readPixel(e.dev, bb, 32, 40, &c32) && readPixel(e.dev, bb, 40, 32, &r32) && readPixel(e.dev, bb, 40, 33, &r33), "read frame 2");
	*d += " f2 c32=" + std::to_string(c32 & 0xff) + " r32=" + std::to_string(r32 & 0xff) + " r33=" + std::to_string(r33 & 0xff);
	CHECK((c32 & 0xff) == 0xff && (r32 & 0xff) == 0 && (r33 & 0xff) == 0xff, "frame 2 coverage (jitter -0.25, +1/6)");
	if (bb)
		bb->Release();
	if (wd)
		wd->Release();
	return ok;
}

// jitter-scope: the same edge quad drawn AFTER End (the HUD's place) with the projection the world pass set: never jittered, so
// in frame 2 (jitter -0.25) column 32 stays uncovered. Planted: End leaves the jittered projection in force.
bool caseJitterScope(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	e.nd->setTemporalConsumer(true);   // a temporal backend consumes the jitter
	float v[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v, 0.f, 0.f, 0.f);
	const MQuad edge = { 0.1f / 32.f * 10.f, 50.f, -50.f, 50.f, 10.f, 0xffffffff };
	ok &= motionFrame(e, wd, v, p, true, { kPlane }, {}, true, d);
	ok &= motionFrame(e, wd, v, p, true, { kPlane }, { edge }, false, d);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	uint32_t c32 = 0, c33 = 0;
	CHECK(bb && readPixel(e.dev, bb, 32, 40, &c32) && readPixel(e.dev, bb, 33, 40, &c33), "read");
	*d += " c32=" + std::to_string(c32 & 0xff) + " c33=" + std::to_string(c33 & 0xff);
	CHECK((c32 & 0xff) == 0x80 && (c33 & 0xff) == 0xff, "a draw after End was jittered");
	if (bb)
		bb->Release();
	if (wd)
		wd->Release();
	return ok;
}


// mv-object: render-motion-units-1's object vectors win where their target's alpha > 0.5. O (a RENDERTARGET texture) is cleared
// to (r 128, g 64, a 255) on the left half and to alpha 0 on the right; frame 2 pans the camera as mv-camera does. Left half =
// (128/255, 64/255) exactly as stored, right half = the camera's (+3, -2), and GXMF_HAS_OBJECT_MV is set. A pass that does NOT
// register O is camera-only again (frame 3). Planted: the resolve never reads O.
bool caseMvObject(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	IDirect3DTexture8* ot = nullptr;
	IDirect3DSurface8* os = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ot)) && ot, "O texture");
	if (ot)
		ot->GetSurfaceLevel(0, &os);
	float v1[16], v2[16], v3[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 0.9375f, 0.625f, 0.f);
	mCamera(v3, 1.875f, 1.25f, 0.f);
	const D3DRECT left = { 0, 0, 32, 64 }, right = { 32, 0, 64, 64 };
	CHECK(os && SUCCEEDED(e.dev->SetRenderTarget(os, nullptr)) && SUCCEEDED(e.dev->Clear(1, &left, D3DCLEAR_TARGET, 0xff804000, 1.f, 0)) &&
		SUCCEEDED(e.dev->Clear(1, &right, D3DCLEAR_TARGET, 0x00000000, 1.f, 0)), "clear O");
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d);
	ok &= motionFrame(e, wd, v2, p, true, { kPlane }, {}, false, d, nullptr, os);
	float err = 1e9f;
	CHECK(mvError(e, [](float x, float, float* mx, float* my) {
		*mx = x < 32.f ? 128.f / 255.f : 3.f;
		*my = x < 32.f ? 64.f / 255.f : -2.f;
	}, &err, d), "read back");
	*d += " object max error " + std::to_string(err);
	CHECK(err < 0.002f, "left half is not O's vectors / right half not the camera's");
	const GxMotionFrame* f = e.nd->motionFrame();
	CHECK(f && (f->flags & GXMF_HAS_OBJECT_MV), "GXMF_HAS_OBJECT_MV not set");
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	ok &= motionFrame(e, wd, v3, p, true, { kPlane }, {}, false, d);   // O not registered this pass
	CHECK(mvError(e, [](float, float, float* mx, float* my) { *mx = 3.f; *my = -2.f; }, &err, d), "read back 3");
	*d += ", unregistered max error " + std::to_string(err);
	CHECK(err < 0.02f, "a pass without SetObjectVelocity still used O");
	f = e.nd->motionFrame();
	CHECK(f && !(f->flags & GXMF_HAS_OBJECT_MV), "GXMF_HAS_OBJECT_MV left set");
	// registered then cancelled (SetObjectVelocity(nullptr)) = camera-only; a wrong-size target is refused
	IDirect3DTexture8* smallTex = nullptr;
	IDirect3DSurface8* ss = nullptr;
	if (SUCCEEDED(e.dev->CreateTexture(32, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &smallTex)) && smallTex)
		smallTex->GetSurfaceLevel(0, &ss);
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);
	{
		IDirect3DSurface8* bb = nullptr;
		e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
		e.dev->SetRenderTarget(bb, wd);
		e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
		GxMotionBegin mb;
		mb.renderW = mb.outputW = mb.renderH = mb.outputH = 64;
		mb.temporal = true;
		std::memcpy(mb.view, v1, 64);   // camera back by (-1.875, -1.25): (-6, +4)
		std::memcpy(mb.proj, p, 64);
		mb.nearZ = 1.f;
		mb.farZ = 100.f;
		e.dev->BeginScene();
		e.nd->worldBegin(mb, bb, wd);
		e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v1));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
		e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));
		e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);   // motionFrame leaves Z off after its End
		drawWorldQuad(e.dev, kPlane);
		CHECK(SUCCEEDED(e.nd->worldObjectVelocity(os)), "register O");
		CHECK(SUCCEEDED(e.nd->worldObjectVelocity(nullptr)), "cancel O");
		CHECK(ss == nullptr || FAILED(e.nd->worldObjectVelocity(ss)), "a 32x32 target accepted for a 64x64 world");
		IDirect3DTexture8* al = nullptr;   // a 2-channel target samples alpha 1: refused
		IDirect3DSurface8* als = nullptr;
		if (SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8L8, D3DPOOL_DEFAULT, &al)) && al)
			al->GetSurfaceLevel(0, &als);
		CHECK(als == nullptr || FAILED(e.nd->worldObjectVelocity(als)), "a 2-channel A8L8 target accepted");
		if (als)
			als->Release();
		if (al)
			al->Release();
		e.nd->worldEnd();
		e.dev->EndScene();
		if (bb)
			bb->Release();
	}
	CHECK(mvError(e, [](float, float, float* mx, float* my) { *mx = -6.f; *my = 4.f; }, &err, d), "read back 4");
	*d += ", cancelled max error " + std::to_string(err);
	CHECK(err < 0.02f, "a cancelled registration was still merged");
	f = e.nd->motionFrame();
	CHECK(f && !(f->flags & GXMF_HAS_OBJECT_MV), "GXMF_HAS_OBJECT_MV set after a cancel");
	if (ss)
		ss->Release();
	if (smallTex)
		smallTex->Release();
	if (os)
		os->Release();
	if (ot)
		ot->Release();
	if (wd)
		wd->Release();
	return ok;
}


// E · seam-worker-2 (phase 2, DESIGN_seam-worker-2.md §5): worldBegin / worldEnd as A23-B S bodies with the worker ON (request 6, MetalFX
// temporal - a lifted kind). mv-camera's pan (two world passes; (+3, -2) everywhere), no read-back between a Begin and its End, and the
// ownership counter unchanged. X- (xNeedsCs): seamNoSBegin (X-seam-begin) / seamNoSEnd (X-seam-end) drop that S - its body runs on the
// front with the worker on, which the ownership counter counts whatever the schedule (the runner judges it: every check here must still
// pass); the planted run idles the worker right before the
// entry point (motionFrame's quiesce) so the front recording never races the worker's writer in this process (phase 1's pattern).
bool caseSeamMotion(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	const bool planted = e.nd->debug().seamNoSBegin || e.nd->debug().seamNoSEnd;
	const uint64_t own0 = e.nd->csOwnerViolations();
	float v1[16], v2[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 0.9375f, 0.625f, 0.f);
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d, nullptr, nullptr, planted);
	ok &= motionFrame(e, wd, v2, p, true, { kPlane }, {}, false, d, nullptr, nullptr, planted);
	float err = 1e9f;
	CHECK(mvError(e, [](float, float, float* x, float* y) { *x = 3.f; *y = -2.f; }, &err, d), "read back");
	*d += " max error " + std::to_string(err) + " own " + std::to_string(e.nd->csOwnerViolations() - own0) + " cs=" +
		(e.nd->csOn() ? "on" : "off");
	CHECK(err < 0.02f, "vectors are not (+3, -2)");
	const GxMotionFrame* f = e.nd->motionFrame();
	CHECK(f && !(f->flags & GXMF_RESET) && f->frameIndex == 2, "frame flags / index");
	if (!planted)   // the planted (X-) run's symptom is judged by the runner: caught only by it, with every check above passing
		CHECK(e.nd->csOwnerViolations() == own0, "worker state touched on the front outside an S body");
	if (wd)
		wd->Release();
	return ok;
}

// mv-object-late (seam-worker-1 DESIGN §5, its design r1 P2-2): the motion bridge's order - O is created before the frame but first
// BOUND inside the world pass (a queued SetRenderTarget: the worker makes O's lazy attachment view), cleared there, the world target bound
// back, a world draw, then worldObjectVelocity(O) at once and End. mv-object's numbers: O's vectors on the left half (alpha 1), the camera's
// (+3, -2) on the right (alpha 0). With the worker ON (request 6). X- (xNeedsCs): seamNoSVelocity drops worldObjectVelocity's S - the
// ownership counter counts it; the planted run idles the worker right before the call.
bool caseMvObjectLate(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	IDirect3DTexture8* ot = nullptr;
	IDirect3DSurface8* os = nullptr;
	CHECK(SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ot)) && ot, "O texture");
	if (ot)
		ot->GetSurfaceLevel(0, &os);
	const bool planted = e.nd->debug().seamNoSVelocity;
	const uint64_t own0 = e.nd->csOwnerViolations();
	float v1[16], v2[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 0.9375f, 0.625f, 0.f);
	ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d);   // frame 1: the history
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(bb && os && SUCCEEDED(e.dev->SetRenderTarget(bb, wd)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "clear");
	GxMotionBegin mb;
	mb.renderW = mb.outputW = mb.renderH = mb.outputH = 64;
	mb.temporal = true;
	std::memcpy(mb.view, v2, 64);
	std::memcpy(mb.proj, p, 64);
	mb.nearZ = 1.f;
	mb.farZ = 100.f;
	mb.fovYRad = 1.5707963f;
	e.dev->BeginScene();
	if (bb)
		e.nd->worldBegin(mb, bb, wd);
	e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v2));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));
	const D3DRECT left = { 0, 0, 32, 64 }, right = { 32, 0, 64, 64 };
	CHECK(os && SUCCEEDED(e.dev->SetRenderTarget(os, nullptr)) && SUCCEEDED(e.dev->Clear(1, &left, D3DCLEAR_TARGET, 0xff804000, 1.f, 0)) &&
		SUCCEEDED(e.dev->Clear(1, &right, D3DCLEAR_TARGET, 0x00000000, 1.f, 0)), "O first bound and cleared inside the pass");
	CHECK(bb && SUCCEEDED(e.dev->SetRenderTarget(bb, wd)), "the world target back");
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);   // motionFrame leaves Z off after its End
	drawWorldQuad(e.dev, kPlane);
	seamQuiesceIfPlanted(e, planted);
	CHECK(SUCCEEDED(e.nd->worldObjectVelocity(os)), "register O right after its first queued use");
	e.nd->worldEnd();
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->EndScene();
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	float err = 1e9f;
	CHECK(mvError(e, [](float x, float, float* mx, float* my) {
		*mx = x < 32.f ? 128.f / 255.f : 3.f;
		*my = x < 32.f ? 64.f / 255.f : -2.f;
	}, &err, d), "read back");
	*d += " object max error " + std::to_string(err) + " own " + std::to_string(e.nd->csOwnerViolations() - own0) + " cs=" +
		(e.nd->csOn() ? "on" : "off");
	CHECK(err < 0.002f, "left half is not O's vectors / right half not the camera's");
	const GxMotionFrame* f = e.nd->motionFrame();
	CHECK(f && (f->flags & GXMF_HAS_OBJECT_MV), "GXMF_HAS_OBJECT_MV not set");
	if (!planted)   // the planted (X-) run's symptom is judged by the runner: caught only by it, with every check above passing
		CHECK(e.nd->csOwnerViolations() == own0, "worker state touched on the front outside an S body");
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	if (bb)
		bb->Release();
	if (os)
		os->Release();
	if (ot)
		ot->Release();
	if (wd)
		wd->Release();
	return ok;
}


// world-ref: the game releases the world depth and colour between Begin and End (and a Create* drains deferred destroys): the
// device's own references keep both alive until End records from them. Validation must stay clean.
bool caseWorldRef(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* wd = worldDepth(e);
	IDirect3DTexture8* ct = nullptr;
	IDirect3DSurface8* cs = nullptr;
	CHECK(wd && SUCCEEDED(e.dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ct)) && ct,
		"targets");
	if (ct)
		ct->GetSurfaceLevel(0, &cs);
	float v[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v, 0.f, 0.f, 0.f);
	CHECK(cs && SUCCEEDED(e.dev->SetRenderTarget(cs, wd)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "bind + clear");
	GxMotionBegin mb;
	mb.renderW = mb.outputW = mb.renderH = mb.outputH = 64;
	mb.temporal = true;
	std::memcpy(mb.view, v, 64);
	std::memcpy(mb.proj, p, 64);
	mb.nearZ = 1.f;
	mb.farZ = 100.f;
	e.dev->BeginScene();
	e.nd->worldBegin(mb, cs, wd);
	e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	drawWorldQuad(e.dev, kPlane);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->SetRenderTarget(bb, nullptr);   // the game moves off the world targets ...
	if (bb)
		bb->Release();
	cs->Release();                          // ... and drops every reference it had
	ct->Release();
	wd->Release();
	cs = nullptr;
	ct = nullptr;
	wd = nullptr;
	IDirect3DTexture8* t = nullptr;         // a Create* runs drainDestroys
	e.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
	if (t)
		t->Release();
	e.nd->worldEnd();
	e.dev->EndScene();
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	CHECK(e.nd->motionFrame() != nullptr, "the resolve did not record");
	return ok;
}


// jitter-abandon: a jittering world pass replaced by a second Begin (no End) must not leave its jittered projection for the
// draws that follow. Frame 2 jitter is (-0.25, +1/6); the replacement pass is spatial (no jitter);
// the edge quad drawn in it must be unjittered: column 32 uncovered. Planted: the abandon keeps the jittered projection.
bool caseJitterAbandon(Env& e, std::string* d)
{
	bool ok = true;
	e.nd->setTemporalConsumer(true);
	IDirect3DSurface8* wd = worldDepth(e);
	CHECK(wd != nullptr, "world depth");
	float v[16], p[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v, 0.f, 0.f, 0.f);
	const MQuad edge = { 0.1f / 32.f * 10.f, 50.f, -50.f, 50.f, 10.f, 0xffffffff };
	ok &= motionFrame(e, wd, v, p, true, { kPlane }, {}, true, d);   // frame 1 (Halton 1)
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->SetRenderTarget(bb, wd);
	e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
	GxMotionBegin mb;
	mb.renderW = mb.outputW = mb.renderH = mb.outputH = 64;
	mb.temporal = true;
	std::memcpy(mb.view, v, 64);
	std::memcpy(mb.proj, p, 64);
	mb.nearZ = 1.f;
	mb.farZ = 100.f;
	e.dev->BeginScene();
	e.nd->worldBegin(mb, bb, wd);   // frame 2: jitter (-0.25, +1/6) ...
	e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));
	mb.temporal = false;
	e.nd->worldBegin(mb, bb, wd);   // ... abandoned by a second Begin, a spatial pass
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	drawWorldQuad(e.dev, edge);
	e.nd->worldEnd();
	e.dev->EndScene();
	uint32_t c32 = 0, c33 = 0;
	CHECK(bb && readPixel(e.dev, bb, 32, 40, &c32) && readPixel(e.dev, bb, 33, 40, &c33), "read");
	*d += " c32=" + std::to_string(c32 & 0xff) + " c33=" + std::to_string(c33 & 0xff);
	CHECK((c32 & 0xff) == 0 && (c33 & 0xff) == 0xff, "the abandoned pass's jitter stayed installed");
	if (bb)
		bb->Release();
	if (wd)
		wd->Release();
	return ok;
}


// jitter-inherit: the projection is set BEFORE the first world pass and never again: the pass must still jitter it.
// Edge at fb y 32.6 (quad below it): unjittered row 32 (centre 32.5) is uncovered; frame 1's Halton (0, -1/6)
// moves the raster up to 32.433, covering row 32. Planted: Begin does not seed the clean copy.
bool caseJitterInherit(Env& e, std::string* d)
{
	bool ok = true;
	e.nd->setTemporalConsumer(true);
	IDirect3DSurface8* wd = worldDepth(e);
	IDirect3DSurface8* bb = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	float v[16], p[16], w[16];
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	mCamera(v, 0.f, 0.f, 0.f);
	mIdentity(w);
	CHECK(wd && bb && SUCCEEDED(e.dev->SetRenderTarget(bb, wd)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0)), "bind");
	e.dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(w));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (A31 motion cases)
	e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));   // before motion ever ran
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	GxMotionBegin mb;
	mb.renderW = mb.outputW = mb.renderH = mb.outputH = 64;
	mb.temporal = true;
	std::memcpy(mb.view, v, 64);
	std::memcpy(mb.proj, p, 64);
	mb.nearZ = 1.f;
	mb.farZ = 100.f;
	e.dev->BeginScene();
	e.nd->worldBegin(mb, bb, wd);
	const MQuad edge = { -50.f, 50.f, -50.f, -0.1f / 32.f * 10.f, 10.f, 0xffffffff };
	drawWorldQuad(e.dev, edge);
	e.nd->worldEnd();
	e.dev->EndScene();
	uint32_t r32 = 0, r33 = 0;
	CHECK(bb && readPixel(e.dev, bb, 40, 32, &r32) && readPixel(e.dev, bb, 40, 33, &r33), "read");
	*d += " r32=" + std::to_string(r32 & 0xff) + " r33=" + std::to_string(r33 & 0xff);
	CHECK((r32 & 0xff) == 0xff && (r33 & 0xff) == 0xff, "the inherited projection was not jittered");
	if (bb)
		bb->Release();
	if (wd)
		wd->Release();
	return ok;
}

// ---------------------------------------------------------------------------------------------- F-depth+ (D · look-fdepth-plus)
// look-director/d2/PLAN_fdepth-plus.md: a reader's mid-frame copies of the world depth and opaque colour (GxSceneTap.h). Every frame
// draws a far quad A over the whole target, calls IGxSceneTap::OpaqueDone, then a near quad B over it, and Presents; the device's
// TEST-ONLY probe (host.fdepthProbe) reads the centre texel of each copy. The copies must hold THIS frame's A - never B (drawn after
// the tap), never an earlier frame's A. A changes depth and colour every frame, so a stale copy cannot pass.

IGxSceneTap* sceneTap(Env& e)
{
	void* p = nullptr;
	return SUCCEEDED(e.dev->QueryInterface(IID_GxSceneTap, &p)) ? static_cast<IGxSceneTap*>(p) : nullptr;
}

struct FdQuad { float z; DWORD c; };
const FdQuad kFdA[3] = { { 0.25f, 0xffff0000 }, { 0.5f, 0xff0000ff }, { 0.75f, 0xffff00ff } };
const FdQuad kFdB = { 0.1f, 0xff00ff00 };

void fdQuad(Env& e, IDirect3DSurface8* rt, const FdQuad& q)
{
	D3DSURFACE_DESC sd = {};
	rt->GetDesc(&sd);
	const float w = float(sd.Width), h = float(sd.Height);
	struct V { float x, y, z, rhw; DWORD c; };
	const V v[4] = { { 0, 0, q.z, 1, q.c }, { w, 0, q.z, 1, q.c }, { 0, h, q.z, 1, q.c }, { w, h, q.z, 1, q.c } };
	e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

// One frame on (rt, ds): A, OpaqueDone (taps times), B, Present. taps 0 = the reader-less control (no call at all).
bool fdFrame(Env& e, IGxSceneTap* tap, IDirect3DSurface8* rt, IDirect3DSurface8* ds, const FdQuad& a, int taps, std::string* d)
{
	bool ok = true;
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(rt, ds)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.f, 0)), "clear");
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	e.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	e.dev->BeginScene();
	fdQuad(e, rt, a);
	for (int i = 0; i < taps; ++i) {
		CHECK(tap && SUCCEEDED(tap->OpaqueDone()), "OpaqueDone");
		if (i == 0)
			fdQuad(e, rt, kFdB);   // after the (first) tap: never in the copies
	}
	if (taps == 0)
		fdQuad(e, rt, kFdB);
	e.dev->EndScene();
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	CHECK(e.nd->stats().drawsRefused == 0, "a draw was refused");
	return ok;
}

bool fdNear(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// The probe's last read against A: depth (D24 quantisation on Windows: 1e-3) and colour (8-bit: 1/255 + slack).
bool fdProbeIs(Env& e, const FdQuad& a, bool depth, bool colour, int frame, std::string* d)
{
	bool ok = true;
	float p[4] = { -2.f, -2.f, -2.f, -2.f };
	CHECK(e.nd->testReadSceneProbe(p), "probe read-back");
	const float r = float((a.c >> 16) & 0xff) / 255.f, g = float((a.c >> 8) & 0xff) / 255.f, b = float(a.c & 0xff) / 255.f;
	char what[200];
	std::snprintf(what, sizeof(what), "frame %d probe (%.4f, %.3f %.3f %.3f) != A (%.4f, %.3f %.3f %.3f)", frame, p[0], p[1], p[2], p[3],
		a.z, r, g, b);
	if (depth)
		CHECK(fdNear(p[0], a.z, 1e-3f), what);
	if (colour)
		CHECK(fdNear(p[1], r, 0.01f) && fdNear(p[2], g, 0.01f) && fdNear(p[3], b, 0.01f), what);
	return ok;
}

uint32_t fdDepthUsage(IDirect3DSurface8* ds)
{
	GxNativeSurface* n = static_cast<GxNativeSurface*>(ds);
	return n && n->image() ? uint32_t(n->image()->usage) : 0xffffffffu;
}

// fdepth-off: no reader. Wanted() is FALSE and an OpaqueDone call (the game makes none then) records, creates and arms nothing:
// the frame's scopes, splits and pixels equal a frame without the call; the world targets' usage words stay what they were made with.
// Planted: the copies are taken with no reader on.
bool caseFdepthOff(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap, "IGxSceneTap");
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds, "back buffer + auto depth");
	CHECK(!tap->Wanted(), "Wanted() with no reader");
	CHECK(!e.nd->depthReadAfterPass(), "depthReadAfterPass() with no reader and no motion");
	const uint32_t usage0 = fdDepthUsage(ds);
	auto counts = [&](uint64_t* scopes, uint64_t* splits, uint64_t* resolves) {
		const GXRender::Rec::GxWriterStats& s = e.nd->writer().stats();   // writer(): the worker drained first
		*scopes = s.scopes;
		*splits = s.splitOutside + s.splitAttachments + s.splitSample + s.splitUpload;
		*resolves = s.scopeResolves;
	};
	uint64_t s0, p0, r0, s1, p1, r1, s2, p2, r2;
	counts(&s0, &p0, &r0);
	for (int f = 0; f < 2; ++f)
		ok &= fdFrame(e, nullptr, bb, ds, kFdA[f], 0, d);   // the control: no call
	counts(&s1, &p1, &r1);
	uint32_t ctl = 0;
	CHECK(readPixel(e.dev, bb, 32, 32, &ctl), "control pixel");
	for (int f = 0; f < 2; ++f)
		ok &= fdFrame(e, tap, bb, ds, kFdA[f], 1, d);       // the same frames with the call
	counts(&s2, &p2, &r2);
	uint32_t got = 0;
	CHECK(readPixel(e.dev, bb, 32, 32, &got) && got == ctl, "pixel differs from the frame without the call");
	CHECK(s2 - s1 == s1 - s0, ("scopes " + std::to_string(s2 - s1) + " vs " + std::to_string(s1 - s0) + " without the call").c_str());
	CHECK(p2 - p1 == p1 - p0, ("splits " + std::to_string(p2 - p1) + " vs " + std::to_string(p1 - p0) + " without the call").c_str());
	CHECK(r2 == r0, "a scope carried a resolve");
	CHECK(e.nd->testScene().stats().taps == 0, "the copies were taken");
	CHECK(!e.nd->writer().scopeResolveArmed(), "the writer is armed");
	CHECK(fdDepthUsage(ds) == usage0, "the world depth's usage moved");
	tap->Release();
	ds->Release();
	bb->Release();
	return ok;
}

// The arms with the probe on: three frames, each A_i / tap / B, the probe read after each Present. expectPath per frame (0 = any).
bool fdRun(Env& e, IDirect3DSurface8* rt, IDirect3DSurface8* ds, const uint8_t (&depthPath)[3], const uint8_t (&colourPath)[3],
	int taps, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap, "IGxSceneTap");
	SETUP(tap->Wanted(), "the probe reader is not registered (host.fdepthProbe)");
	CHECK(e.nd->depthReadAfterPass(), "depthReadAfterPass() false with a depth reader on");
	const uint32_t usage0 = fdDepthUsage(ds);
	for (int f = 0; f < 3; ++f) {
		ok &= fdFrame(e, tap, rt, ds, kFdA[f], taps, d);
		ok &= fdProbeIs(e, kFdA[f], true, true, f, d);
		const GxSceneFrame& fr = e.nd->testScene().frame();   // after the read-back's drain
		if (depthPath[f])
			CHECK(fr.depthPath == depthPath[f], ("frame " + std::to_string(f) + " depth path " + std::to_string(fr.depthPath) + " expected " +
				std::to_string(depthPath[f])).c_str());
		if (colourPath[f])
			CHECK(fr.colourPath == colourPath[f], ("frame " + std::to_string(f) + " colour path " + std::to_string(fr.colourPath) +
				" expected " + std::to_string(colourPath[f])).c_str());
	}
	CHECK(fdDepthUsage(ds) == usage0, "the world depth's usage moved");
	CHECK(e.nd->testScene().stats().taps == 3, ("taps " + std::to_string(e.nd->testScene().stats().taps) + " != 3").c_str());
	tap->Release();
	return ok;
}

// fdepth-1x: a single-sample back buffer + auto depth: vkCmdCopyImage every frame. Planted: the copy records nothing.
bool caseFdepth1x(Env& e, std::string* d)
{
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds && fdDepthUsage(ds) != 0xffffffffu, "back buffer + auto depth");
	const uint8_t c3[3] = { GXSP_COPY, GXSP_COPY, GXSP_COPY };
	const bool ok = fdRun(e, bb, ds, c3, c3, 1, d);
	ds->Release();
	bb->Release();
	return ok;
}

// fdepth-msaa: Wh1ter0se's shape (GXUpscaler off, MSAA x4 back buffer + auto depth). Frame 0 resolves in its own pass (nothing was armed
// yet), frames 1-2 in the world's own scope. Planted (writer): the scope resolve is recorded as attached but never attached.
bool caseFdepthMsaa(Env& e, std::string* d)
{
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds, "back buffer + auto depth");
	D3DSURFACE_DESC dd = {};
	ds->GetDesc(&dd);
	SETUP(dd.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES, "MSAA x4 auto depth");
	const uint8_t p3[3] = { GXSP_PASS, GXSP_IN_SCOPE, GXSP_IN_SCOPE };
	const bool ok = fdRun(e, bb, ds, p3, p3, 1, d);
	ds->Release();
	bb->Release();
	return ok;
}

// fdepth-rt: the world/HUD seam's shape - its own MSAA x4 render target + depth (GXUpscaler on). Same paths, same plant.
bool caseFdepthRt(Env& e, std::string* d)
{
	IDirect3DSurface8* rt = nullptr;
	IDirect3DSurface8* ds = nullptr;
	SETUP(SUCCEEDED(e.dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_4_SAMPLES, FALSE, &rt)) && rt &&
		SUCCEEDED(e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, &ds)) && ds, "MSAA x4 world targets");
	const uint8_t p3[3] = { GXSP_PASS, GXSP_IN_SCOPE, GXSP_IN_SCOPE };
	const bool ok = fdRun(e, rt, ds, p3, p3, 1, d);
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ads = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ads);
	e.dev->SetRenderTarget(bb, ads);   // the world targets unbound before they go
	for (IDirect3DSurface8* s : { bb, ads, ds, rt })
		if (s)
			s->Release();
	return ok;
}

// fdepth-pass: GX_FDEPTH_RESOLVE=pass (host.fdepthForcePass): every frame resolves in its own pass, nothing is ever armed.
// Planted: the pass records nothing.
bool caseFdepthPass(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds, "back buffer + auto depth");
	const uint8_t p3[3] = { GXSP_PASS, GXSP_PASS, GXSP_PASS };
	ok &= fdRun(e, bb, ds, p3, p3, 1, d);
	CHECK(!e.nd->writer().scopeResolveArmed(), "armed although the pass is forced");
	CHECK(e.nd->writer().stats().scopeResolves == 0, "a scope carried a resolve although the pass is forced");
	ds->Release();
	bb->Release();
	return ok;
}

// fdepth-order: the probe reads at Present - after B was drawn on top of the tap. The world's scopes after the tap must not resolve
// again (the copies hold the tap's A). Planted (writer): suspendScopeResolve does not suspend.
bool caseFdepthOrder(Env& e, std::string* d)
{
	e.nd->testScene().testProbeAtPresent(true);
	return caseFdepthMsaa(e, d);
}

// fdepth-once: OpaqueDone twice in a frame (A, tap, B, tap): the second is ignored - the copies hold A. Planted: it runs.
bool caseFdepthOnce(Env& e, std::string* d)
{
	e.nd->testScene().testProbeAtPresent(true);
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds, "back buffer + auto depth");
	const uint8_t any[3] = { 0, 0, 0 };
	const bool ok = fdRun(e, bb, ds, any, any, 2, d);
	ds->Release();
	bb->Release();
	return ok;
}

// fdepth-forget: a world depth the writer is armed with is released - the writer disarms at once (never attaches a freed image), and
// the next frame on another depth takes its copies from scratch. Planted (writer): forget() keeps the GxScopeResolve.
bool caseFdepthForget(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap && tap->Wanted(), "IGxSceneTap + the probe reader");
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ads = nullptr;
	IDirect3DSurface8* ds2 = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ads);
	SETUP(bb && ads && SUCCEEDED(e.dev->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES, &ds2)) && ds2,
		"a second MSAA x4 depth");
	ok &= fdFrame(e, tap, bb, ds2, kFdA[0], 1, d);
	CHECK(e.nd->writer().scopeResolveArmed(), "not armed after a tap on an MSAA world");
	e.dev->SetRenderTarget(bb, ads);
	ds2->Release();   // the last reference: the surface and its image go (forgetImage -> the writer)
	ds2 = nullptr;
	e.dev->Present(nullptr, nullptr, nullptr, nullptr);   // the destruction runs behind every command that named it
	const bool armed = e.nd->writer().scopeResolveArmed();
	CHECK(!armed, "still armed with a released depth");
	if (!armed) {   // the plant leaves a freed image in the writer: never draw on with it
		ok &= fdFrame(e, tap, bb, ads, kFdA[1], 1, d);
		ok &= fdProbeIs(e, kFdA[1], true, true, 1, d);
	}
	tap->Release();
	ads->Release();
	bb->Release();
	return ok;
}

// fdepth-source: the world's depth also serves another MSAA colour target C2 between the world's last draw and
// the tap (A1 on the back buffer, then X on (C2, the same depth) with depth writes off, then the back buffer bound again and the tap
// with no draw). The writer resolved C2's colour into the copy at the end of C2's scope; the copy must still hold the BACK BUFFER's
// colour (A1) - the tap resolves it again - and the depth A1's. Frame 0 arms. Planted (writer): a held colour bit is taken whatever
// colour target its scope resolved.
bool caseFdepthSource(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap && tap->Wanted(), "IGxSceneTap + the probe reader");
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	IDirect3DSurface8* c2 = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	D3DSURFACE_DESC bd = {};
	SETUP(bb && ds && SUCCEEDED(bb->GetDesc(&bd)) &&
		SUCCEEDED(e.dev->CreateRenderTarget(bd.Width, bd.Height, bd.Format, D3DMULTISAMPLE_4_SAMPLES, FALSE, &c2)) && c2,
		"MSAA x4 back buffer + a second MSAA target of its size and format");
	ok &= fdFrame(e, tap, bb, ds, kFdA[0], 1, d);   // arms the world's scopes on ds
	const FdQuad x = { 0.3f, 0xff00ffff };
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ds)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.f, 0)), "clear");
	e.dev->BeginScene();
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	fdQuad(e, bb, kFdA[1]);
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(c2, ds)), "bind C2 + the world depth");
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	fdQuad(e, c2, x);   // C2's scope carries the armed resolve: the colour copy gets X here
	e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ds)), "bind the back buffer again");
	CHECK(SUCCEEDED(tap->OpaqueDone()), "OpaqueDone");
	e.dev->EndScene();
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	ok &= fdProbeIs(e, kFdA[1], true, true, 1, d);
	CHECK(e.nd->testScene().frame().depthPath == GXSP_IN_SCOPE, "the depth was not the world scope's own resolve");
	CHECK(e.nd->testScene().frame().colourPath == GXSP_PASS, "the colour was not taken again from the back buffer");
	tap->Release();
	for (IDirect3DSurface8* sf : { c2, ds, bb })
		sf->Release();
	return ok;
}

// fdepth-repaint: after the world scope resolved A1's colour, the back buffer is drawn again WITHOUT the world
// depth (X, a scope the armed resolve does not follow), the world pair is bound again and the tap comes with no draw. The colour copy
// must be X - the back buffer as it is at the tap - and the depth still A1's. Planted (writer): that scope keeps the held colour.
bool caseFdepthRepaint(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap && tap->Wanted(), "IGxSceneTap + the probe reader");
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	e.dev->GetDepthStencilSurface(&ds);
	SETUP(bb && ds, "MSAA x4 back buffer + auto depth");
	ok &= fdFrame(e, tap, bb, ds, kFdA[0], 1, d);   // arms the world's scopes on ds
	const FdQuad x = { 0.3f, 0xff00ffff };
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ds)) &&
		SUCCEEDED(e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.f, 0)), "clear");
	e.dev->BeginScene();
	fdQuad(e, bb, kFdA[1]);   // the world scope: its resolve leaves A1 in both copies
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, nullptr)), "the back buffer without depth");
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	fdQuad(e, bb, x);
	e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	CHECK(SUCCEEDED(e.dev->SetRenderTarget(bb, ds)), "the world pair again");
	CHECK(SUCCEEDED(tap->OpaqueDone()), "OpaqueDone");
	e.dev->EndScene();
	CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
	const FdQuad want = { kFdA[1].z, x.c };   // depth A1, colour X
	ok &= fdProbeIs(e, want, true, true, 1, d);
	tap->Release();
	ds->Release();
	bb->Release();
	return ok;
}

// fdepth-motion: the integrator's condition (DLSS_INTEGRATION.md 17/10): a world pass with motion on (temporal, sampleable world depth)
// and a reader on. The tap inside the pass leaves motion-1's vectors exactly as without it, and the copies hold A.
// Planted: the copy records nothing - the probe must miss the plane depth.
bool caseFdepthMotion(Env& e, std::string* d)
{
	bool ok = true;
	IGxSceneTap* tap = sceneTap(e);
	SETUP(tap && tap->Wanted(), "IGxSceneTap + the probe reader");
	IDirect3DSurface8* wd = worldDepth(e);
	SETUP(wd, "temporal world depth");
	float v1[16], v2[16], p[16];
	mCamera(v1, 0.f, 0.f, 0.f);
	mCamera(v2, 1.f, 0.f, 0.f);
	mPerspective(p, 1.f, 1.f, 1.f, 100.f);
	auto run = [&](bool withTap, std::vector<uint8_t>* mv) {
		ok &= motionFrame(e, wd, v1, p, true, { kPlane }, {}, true, d);
		if (withTap) {   // the second frame: A, tap, B inside the world pass, as RTS3DScene::Flush calls it
			IDirect3DSurface8* bb = nullptr;
			e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
			GxMotionBegin mb;
			mb.renderW = mb.outputW = 64;
			mb.renderH = mb.outputH = 64;
			mb.temporal = true;
			std::memcpy(mb.view, v2, 64);
			std::memcpy(mb.proj, p, 64);
			mb.nearZ = 1.f;
			mb.farZ = 100.f;
			mb.fovYRad = 1.5707963f;
			e.dev->SetRenderTarget(bb, wd);
			e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.f, 0);
			e.dev->BeginScene();
			e.nd->worldBegin(mb, bb, wd);
			float world4[16];
			mIdentity(world4);
			e.dev->SetRenderState(D3DRS_ZENABLE, TRUE);
			e.dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			e.dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
			e.dev->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(world4));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (F-depth+ motion arm)
			e.dev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(v2));   // GX-WRAPSKIP-OK: test device, no DX8Wrapper in the process (F-depth+ motion arm)
			e.dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(p));
			drawWorldQuad(e.dev, kPlane);
			CHECK(SUCCEEDED(tap->OpaqueDone()), "OpaqueDone");
			e.nd->worldEnd();
			e.dev->EndScene();
			CHECK(SUCCEEDED(e.dev->Present(nullptr, nullptr, nullptr, nullptr)), "present");
			bb->Release();
		} else
			ok &= motionFrame(e, wd, v2, p, true, { kPlane }, {}, true, d);
		uint32_t w = 0, h = 0;
		CHECK(e.nd->testReadMotion(mv, &w, &h) && w == 64 && h == 64, "motion read-back");
	};
	std::vector<uint8_t> without, with;
	run(false, &without);
	run(true, &with);
	CHECK(without.size() == with.size() && without == with, "the motion vectors moved with a tap inside the world pass");
	float pr[4] = { -2.f, -2.f, -2.f, -2.f };
	// the plane at view z 10 under n 1 / f 100: z_ndc = f/(f-n) - n*f/((f-n)*10) = 900/990 everywhere
	CHECK(e.nd->testReadSceneProbe(pr) && fdNear(pr[0], 900.f / 990.f, 1e-3f), ("the probe saw no plane depth: " + std::to_string(pr[0])).c_str());
	CHECK(e.nd->testScene().frame().depthPath == GXSP_COPY, "a single-sample temporal world copies its depth");
	tap->Release();
	wd->Release();
	return ok;
}


// rw3-R11: the GX_SHADOW_GPU shadow-volume vs (GxShadowVolumeVS.h = the bytes the game creates) through the d3d8 calls the game makes
// (CreateVertexShader, SetVertexShaderConstant, a MANAGED VB/IB, DrawIndexedPrimitive). Colour writes stay ON here (the game turns them
// off) so the extrusion is visible; view = projection = identity, so oPos = the volume transform * the corner. One edge {pA, qA, pB, qB}
// from (-0.5, -0.5) to (0.5, -0.5): face A sees the light and stays, face B does not and moves to (v - L) * s + v with L = (0, -1.5, 0.5),
// s = 1, so qA = (-1, 0.5) and qB = (1, 0.5): a trapezoid, narrow at y = -0.5, wide at y = 0.5 (pixel row 19 is y = +0.39 in d3d).
// Arms: the caps say 96; the volume in slot 1 (a0 addressing, slot 0 zeroed) draws the trapezoid and nothing outside it; both faces lit
// draws nothing; every slot zeroed draws nothing; no draw refused.
bool caseShadowVs(Env& e, std::string* d)
{
	bool ok = true;
	D3DCAPS8 caps = {};
	CHECK(SUCCEEDED(e.dev->GetDeviceCaps(&caps)) && caps.MaxVertexShaderConst == kGxNativeMaxVsConst && kGxNativeMaxVsConst == 96,
		("MaxVertexShaderConst " + std::to_string(caps.MaxVertexShaderConst) + ", want 96 (what the translator holds)").c_str());
	struct V { float p[3], n[3], r[3], base; };
	static_assert(sizeof(V) == 40, "GxShadowEdgeVertex is 40 B");
	IDirect3DSurface8* bb = nullptr;
	IDirect3DVertexBuffer8* vb = nullptr;
	IDirect3DIndexBuffer8* ib = nullptr;
	DWORD vs = 0;
	e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
	CHECK(SUCCEEDED(e.dev->CreateVertexShader((const DWORD*)kGxShadowDecl, (const DWORD*)kGxShadowVS, &vs, 0)) && vs, "CreateVertexShader");
	CHECK(SUCCEEDED(e.dev->CreateVertexBuffer(4 * sizeof(V), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb)) && vb, "CreateVertexBuffer");
	CHECK(SUCCEEDED(e.dev->CreateIndexBuffer(6 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib)) && ib, "CreateIndexBuffer");
	BYTE* p = nullptr;
	if (ib && SUCCEEDED(ib->Lock(0, 6 * sizeof(WORD), &p, 0)) && p) {
		const WORD x[6] = { 0, 2, 1, 1, 2, 3 };   // (pA, pB, qA), (qA, pB, qB)
		std::memcpy(p, x, sizeof(x));
		ib->Unlock();
	}
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	// one draw: the edge with base register `base`, face B's normal y = nBy (+1 unlit, -1 lit), slot 1 filled unless `zero`
	auto drawEdge = [&](float base, float nBy, bool zero, const char* what, bool wantTrapezoid) {
		if (!bb || !vb || !ib || !vs)
			return;
		const V v[4] = {
			{ { -0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 0, -0.5f, 0.5f }, base },     // pA (face A)
			{ { -0.5f, -0.5f, 0.5f }, { 0, nBy, 0 }, { 0, -0.5f, 0.5f }, base },    // qA (face B)
			{ { 0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 0, -0.5f, 0.5f }, base },      // pB
			{ { 0.5f, -0.5f, 0.5f }, { 0, nBy, 0 }, { 0, -0.5f, 0.5f }, base },     // qB
		};
		BYTE* q = nullptr;
		if (SUCCEEDED(vb->Lock(0, sizeof(v), &q, 0)) && q) {
			std::memcpy(q, v, sizeof(v));
			vb->Unlock();
		}
		float c[17][4] = {};
		c[0][1] = 1;                                           // c0 = (0, 1, 0, 0)
		for (int i = 0; i < 4; ++i)
			c[1 + i][i] = c[5 + i][i] = 1;                     // view and projection columns: identity
		if (!zero) {                                           // slot 1 (c13..c16); slot 0 (c9..c12) stays zero
			c[13][0] = c[14][1] = c[15][2] = 1;                // the volume's Matrix3D rows: identity
			c[16][0] = 0; c[16][1] = -1.5f; c[16][2] = 0.5f; c[16][3] = 1;   // (L, s)
		}
		CHECK(clearSurface(e.dev, bb, 0xff000000), "clear back buffer");
		e.dev->SetVertexShaderConstant(0, c, 17);
		e.dev->SetVertexShader(vs);
		e.dev->SetStreamSource(0, vb, sizeof(V));
		e.dev->SetIndices(ib, 0);
		CHECK(SUCCEEDED(e.dev->BeginScene()), "BeginScene");
		CHECK(SUCCEEDED(e.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, 2)), "DrawIndexedPrimitive");
		CHECK(SUCCEEDED(e.dev->EndScene()), "EndScene");
		e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		const struct { UINT x, y; bool in; } at[4] = { { 32, 32, true }, { 51, 19, true }, { 51, 44, false }, { 32, 4, false } };
		for (const auto& a : at) {
			uint32_t px = 0;
			const bool lit = readPixel(e.dev, bb, a.x, a.y, &px) && (px & 0xffffff) == 0xffffff;
			const bool want = wantTrapezoid && a.in;
			if (lit != want || (!want && (px & 0xffffff) != 0)) {
				*d += std::string(" [") + what + " (" + std::to_string(a.x) + "," + std::to_string(a.y) + ") = " + std::to_string(px & 0xffffff) +
					(want ? ", want white]" : ", want black]");
				ok = false;
			}
		}
	};
	drawEdge(13.0f, 1.0f, false, "slot 1, face B unlit", true);
	drawEdge(13.0f, -1.0f, false, "slot 1, both faces lit", false);
	drawEdge(9.0f, 1.0f, false, "slot 0 (zeroed)", false);
	drawEdge(13.0f, 1.0f, true, "every slot zeroed", false);
	CHECK(e.nd->stats().drawsRefused == 0, "a shadow draw was refused");
	// c93-c95 are the device's last three registers; one past them is the d3d8 answer for a register past MaxVertexShaderConst, counted
	// (a 61-slot classic capture's c96+ used to be dropped silently by the translator)
	const float k[4][4] = {};
	CHECK(SUCCEEDED(e.dev->SetVertexShaderConstant(93, k, 3)), "SetVertexShaderConstant c93-c95 refused");
	CHECK(e.dev->SetVertexShaderConstant(94, k, 3) == D3DERR_INVALIDCALL && e.nd->stats().vsConstantsRefused == 1,
		"SetVertexShaderConstant c94-c96 not refused / not counted");
	if (vs)
		e.dev->DeleteVertexShader(vs);
	if (ib)
		ib->Release();
	if (vb)
		vb->Release();
	if (bb)
		bb->Release();
	return ok;
}

// R38: a clear INSIDE a pass (the pass already has a draw) must not take the draws after it with it. MoltenVK records an
// in-pass vkCmdClearAttachments as a Metal draw of its own and disturbs bound state, so the writer re-sends its state after one.
// Covers a colour-only, a depth-only and an all-aspects clear (memory gxpacketwriter-zonly-clear-on-stencil-is-not-deferred):
// draw A (red, cell 0) -> Clear(kind) -> draw B (green, cell 1), one frame, no read-back between. B must always be green; A is
// black when the clear has TARGET, red when it is depth-only. Both arms set the GxRecDebug::disturbAfterClear hook (an empty scissor
// after every in-pass clear, on every driver), so the clean arm needs the production re-send on drivers that keep bound state across
// the clear too. X- = GxRecDebug::noRebindAfterClear (no re-send), symptom-gated:
// a detection only when B's cell keeps its pre-B black on EVERY kind while setup, read-back, A and the
// clear count all hold; any other failure, or the symptom on some kinds only, makes the X- arm INVALID.
bool caseClearMidframeRebind(Env& e, std::string* d)
{
	bool ok = true;
	IDirect3DSurface8* bb = nullptr;
	IDirect3DSurface8* ds = nullptr;
	if (FAILED(e.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(e.dev->GetDepthStencilSurface(&ds)) || !ds) {
		if (bb)
			bb->Release();
		e.setupFailed = true;
		return false;
	}
	e.dev->SetRenderTarget(bb, ds);
	e.dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	e.dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	e.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	e.dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	e.dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	e.dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	struct QV { float x, y, z, rhw; DWORD c; };
	const QV a[4] = { { 0, 0, 0.5f, 1, 0xffff0000 }, { 16, 0, 0.5f, 1, 0xffff0000 }, { 0, 16, 0.5f, 1, 0xffff0000 }, { 16, 16, 0.5f, 1, 0xffff0000 } };
	const QV b[4] = { { 16, 0, 0.5f, 1, 0xff00ff00 }, { 32, 0, 0.5f, 1, 0xff00ff00 }, { 16, 16, 0.5f, 1, 0xff00ff00 }, { 32, 16, 0.5f, 1, 0xff00ff00 } };
	struct Kind { const char* name; DWORD flags; uint32_t cell0; };
	const Kind kinds[3] = { { "colour", D3DCLEAR_TARGET, 0x000000 }, { "depth", D3DCLEAR_ZBUFFER, 0xff0000 },
		{ "all", D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x000000 } };
	e.nd->writer().debug().disturbAfterClear = true;
	const GXRender::Rec::GxWriterStats w0 = e.nd->writer().stats();
	int staleKinds = 0;
	for (const Kind& k : kinds) {
		e.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.f, 0);
		e.dev->BeginScene();
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, a, sizeof(QV));
		e.dev->Clear(0, nullptr, k.flags, 0xff000000, 1.f, 0);   // the pass is open: an in-pass clear
		e.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, b, sizeof(QV));
		e.dev->EndScene();
		uint32_t c0 = 0, c1 = 0;
		const bool read = readPixel(e.dev, bb, 8, 8, &c0) && readPixel(e.dev, bb, 24, 8, &c1);
		char buf[96];
		std::snprintf(buf, sizeof(buf), " %s: cell0 %06x cell1 %06x;", k.name, c0 & 0xffffff, c1 & 0xffffff);
		*d += buf;
		A25_CHECK(read, "read-back");
		const bool stale = read && (c1 & 0xffffff) == 0x000000;   // B never drawn: the cell keeps the clears' black
		staleKinds += stale;
		A25_SYM(read && (c1 & 0xffffff) == 0x00ff00, stale, "the draw after the in-pass clear is present");
		A25_CHECK((c0 & 0xffffff) == k.cell0, "the draw before the clear: erased by a colour clear, kept by a depth-only one");
	}
	A25_CHECK(staleKinds == 0 || staleKinds == 3, ("B lost on " + std::to_string(staleKinds) + " of 3 kinds, not all or none").c_str());
	const GXRender::Rec::GxWriterStats w1 = e.nd->writer().stats();
	A25_CHECK(w1.clears >= w0.clears + 6, "every clear reached the writer");
	e.nd->writer().debug().disturbAfterClear = false;
	ds->Release();
	bb->Release();
	return ok;
}

#if defined(GX_VKCTX_HAS_SDL3)   // a present arm (presentClear, validationText)
// PREREG_T A30 step 2 (amendment 1 (5)): the EFFECTIVE frames in flight equal the run's --fif. The device's count, the writer's ring,
// the descriptor-pool slots the writer cycles and the swapchain's present slots are all exactly g_fif, before and after a surface-lost
// Reset (the swapchain rebuilt on a new surface), no slot's pools are reset before its own retirement, validation clean. The back buffer
// is 4x MSAA so every present goes through GxNativePresent::record, which refuses a slot without its descriptor set (a black frame,
// counted in present_errors): no present error over every slot. Planted (before creation): the
// writer keeps 2 while the knob says 3 (the X- arm runs only at --fif 3; at 2 the plant changes nothing).
bool caseFifEffective(Env& e, std::string* d)
{
	bool ok = true;
	auto cycle = [&](const std::string& when) {
		uint32_t writerSlots = 0, presentSlots = 0;   // bit masks of the slots seen
		for (uint32_t i = 0; i < 2 * g_fif + 2; ++i) {
			CHECK(presentClear(e, 0xff204060), (when + ": a frame").c_str());
			writerSlots |= 1u << e.nd->writer().frameSlot();
			presentSlots |= 1u << e.nd->presentSlot();
		}
		const uint32_t all = (1u << g_fif) - 1;   // exactly slots 0..g_fif-1 (portable: no popcount builtin)
		*d += " " + when + ": device " + std::to_string(e.nd->framesInFlight()) + ", ring " + std::to_string(e.nd->writer().framesInFlight()) +
			", writer slot mask " + std::to_string(writerSlots) + ", present slot mask " + std::to_string(presentSlots) + " of " +
			std::to_string(e.nd->presentSlots()) + ", present errors " + std::to_string(e.nd->stats().presentErrors) + ";";
		CHECK(e.nd->framesInFlight() == g_fif && e.nd->writer().framesInFlight() == g_fif, (when + ": the count is --fif").c_str());
		CHECK(writerSlots == all && presentSlots == all && e.nd->presentSlots() == g_fif, (when + ": every slot cycles, no other").c_str());
		CHECK(e.nd->stats().presentErrors == 0 && e.nd->stats().presentFailures == 0, (when + ": every slot's present drew").c_str());
	};
	cycle("before");
	e.nd->debug().injectPresentSurfaceLost = true;
	CHECK(presentClear(e, 0xff204060), "the frame whose present reports the surface lost");
	D3DPRESENT_PARAMETERS pp = presentParams(true, 1, D3DMULTISAMPLE_4_SAMPLES);   // the same MSAA: the shader present again
	pp.hDeviceWindow = static_cast<HWND>(e.window);
	CHECK(e.dev->Reset(&pp) == D3D_OK, "Reset on a new surface");
	cycle("after the Reset");
	CHECK(e.nd->writer().retireViolations() == 0, "no slot's pools reset before its own retirement");
	const std::string v = validationText(e);
	CHECK(v.compare(0, 3, "0/0") == 0, ("validation " + v).c_str());
	return ok;
}
#endif

const Case kCases[] = {
	{ "rotation", caseRotation, &GxNativeDebug::noRotate, false, 2 },
	{ "cs-ring", caseCsRing, &GxNativeDebug::csRingOverwrite, true, 1 },
	{ "cs-wake", caseCsWake, &GxNativeDebug::csWakeEveryRecord, true, 1 },   // A55
	{ "cs-wake-target", caseCsWakeTarget, &GxNativeDebug::csWakeLate, true, 1 },   // A55 (a hang in a child)
	{ "cs-getter", caseCsGetter, &GxNativeDebug::csGetterReadsWorker, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-discard", caseCsDiscard, &GxNativeDebug::csDiscardEarly, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-nooverwrite", caseCsNoOverwrite, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-static-unlock", caseCsStaticUnlock, &GxNativeDebug::csUnlockByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 1 },
	{ "cs-tex-unlock", caseCsTexUnlock, &GxNativeDebug::csTexUnlockByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-up", caseCsUp, &GxNativeDebug::csUpByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-up-notarget", caseCsUpNoTarget, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-copy-late", caseCsCopyLateRead, &GxNativeDebug::csCopyLateRead, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-lead", caseCsLead, &GxNativeDebug::csNoLead, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-loss", caseCsLoss, &GxNativeDebug::csLossUnpublished, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-lost-fenced", caseCsLostFenced, &GxNativeDebug::csLostUnfenced, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ring-full", caseCsRingFull, &GxNativeDebug::csFullFails, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 64 },
	{ "cs-reconcile", caseCsReconcile, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::sliceWorkerStale, nullptr, false, false, true },
	{ "cs-reorder", caseCsReorder, &GxNativeDebug::csReorder, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 1 },
	{ "cs-tsan-probe", caseCsTsanProbe, &GxNativeDebug::csRaceRot, false, 2, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-create-retry", caseCsCreateRetry, &GxNativeDebug::csNoRetry, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-create-retry-count", caseCsCreateRetry, &GxNativeDebug::csRetryDoubleCount, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-discard-pressure", caseCsDiscardPressure, &GxNativeDebug::csDiscardNoRetry, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-reclaim", caseCsReclaim, &GxNativeDebug::csNoReclaim, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-site-classes", caseCsSiteClasses, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-create-fails", caseCsCreateFails, &GxNativeDebug::csCreateDefer, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "cs-sbody-alloc", caseCsSbodyAlloc, &GxNativeDebug::csSbodyNoRecover, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-terminal-count", caseCsSbodyAlloc, &GxNativeDebug::csTerminalSilent, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-flush-front", caseCsFlushFront, &GxNativeDebug::csFlushDropped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "cs-flush-worker", caseCsFlushWorker, &GxNativeDebug::csExecNoFlush, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "cs-lifetime", caseCsLifetime, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-drawable-thread", caseCsDrawableThread, &GxNativeDebug::csWorkerDrawable, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "cs-reset-drain", caseCsResetDrain, &GxNativeDebug::csResetNoDrain, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "child-cs-reset-drain", caseChildCsResetDrain, &GxNativeDebug::csResetNoDrain, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-wsi-owner", caseCsWsiOwner, &GxNativeDebug::csRecreateOnWorker, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-oversize", caseCsOversize, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 64 },
	{ "cs-discard-gpu", caseCsDiscardGpu, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::discardIgnoresCompleted, nullptr, false, false, true },
	{ "cs-reclaim-busy", caseCsReclaimBusy, &GxNativeDebug::csReclaimNoWait, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-rt-view-fail", caseCsRtViewFail, nullptr, true, 1 },
	{ "strict-light-off", caseStrictLight, &GxNativeDebug::csLightEnableTrue, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },   // strict 1: rw3-X-3 trin plant
	{ "strict-indexed-start", caseStrictIndexed, &GxNativeDebug::csIndexedStartZero, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },   // strict 1: rw3-X-3 trin plant
	{ "strict-indexed-base", caseStrictIndexed, &GxNativeDebug::csIndicesBaseZero, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },   // strict 1: rw3-X-3 trin plant
	{ "strict-indexed", caseStrictIndexed, &GxNativeDebug::csDropIndexed, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "cs-constants", caseCsConstants, &GxNativeDebug::csConstByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-const-bounds", caseCsConstBounds, &GxNativeDebug::csConstOverread, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "child-const-bounds", caseChildConstBounds, &GxNativeDebug::csConstOverread, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "strict-light", caseStrictLight, &GxNativeDebug::csDropLight, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-light-enable", caseStrictLight, &GxNativeDebug::csDropLightEnable, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-ps", caseStrictShaderState, &GxNativeDebug::csDropCreatePs, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2, true },
	{ "strict-constants", caseStrictShaderState, &GxNativeDebug::csDropConstants, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2, true },
	{ "strict-vs", caseStrictShaderState, &GxNativeDebug::csDropCreateVs, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2, true },
	// rw3-X-3 (A23-B merge gate item 3): two more translator-input plants (kTrinPlants) - payload and dropped call
	{ "strict-vs-code", caseStrictShaderState, &GxNativeDebug::csTrVsCodeAtDecl, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2, true },
	// strict 2: dropping every render state leaves this arm's pixels as they are (D3D defaults) - only trin sees it (gate f95565396)
	{ "strict-light-rs", caseStrictLight, &GxNativeDebug::csTrSkipRenderState, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2, true },
	// the worker's stack (a child: a 20000-rectangle Clear + Present, the submit thread off, CS=1)
	{ "cs-deep-clear", caseCsDeepClear, &GxNativeDebug::csWorkerDefaultStack, true, 1 },
	{ "child-cs-deep-clear", caseChildCsDeepClear, &GxNativeDebug::csWorkerDefaultStack, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	// rw3-X-3: the arms (PREREG A23-B amendment 5 (3))
	{ "cs-ff-payload-material", caseCsFfPayload, &GxNativeDebug::csMaterialByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ff-payload-light", caseCsFfPayload, &GxNativeDebug::csLightByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ff-payload-viewport", caseCsFfPayload, &GxNativeDebug::csViewportByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "strict-shader-unbound-delete", caseStrictShaderUnboundDelete, &GxNativeDebug::csDeleteSkipsUnbound, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-multiply", caseStrictMultiply, &GxNativeDebug::csMultiplySkip, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-shader-oversize", caseStrictShaderOversize, &GxNativeDebug::csShaderOversizeRefused, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "cs-shader-oversize-order", caseCsShaderOversizeOrder, &GxNativeDebug::csOversizeLateDrain, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 64 },
	{ "strict-copy-oversize", caseStrictCopyOversize, &GxNativeDebug::csCopyOversizeRefused, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "cs-oversize-order", caseCsOversizeOrder, &GxNativeDebug::csOversizeLateDrain, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 64 },
	{ "strict-copy-offsets", caseStrictCopyOffsets, &GxNativeDebug::csCopyDestOrigin, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	// rw3-X-3: Lock/Unlock + CopyRects census (amendment 5 (13)) - one Case row per plant
	{ "strict-buffer-ranges", caseStrictBufferRanges, &GxNativeDebug::csUploadOffsetZero, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "strict-buffer-ranges-plain", caseStrictBufferRanges, &GxNativeDebug::csPlainLockForwardsDiscard, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "strict-buffer-ranges-ro", caseStrictBufferRanges, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::readonlyUploads, nullptr, false, false, false, 64, 1, false },
	{ "strict-buffer-ranges-size0", caseStrictBufferRanges, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::lockSizeWhole, nullptr, false, false, false, 64, 1, true },
	{ "strict-buffer-ranges-dno", caseStrictBufferRanges, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::keepDiscardWithNoOverwrite, nullptr, false, false, false, 64, 1, true },
	{ "strict-texture-partial", caseStrictTexturePartial, &GxNativeDebug::csRectPitchSwap, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "strict-texture-partial-nobc", caseStrictTexturePartial, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, true, nullptr, nullptr, false, false, false, 64 },
	{ "strict-host-copy-fmt", caseStrictHostCopy, &GxNativeDebug::csHostCopyTwinA8, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, false },
	{ "strict-host-copy-shape", caseStrictHostCopy, &GxNativeDebug::csHostCopyTwinSquare, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, false },
	{ "strict-host-copy-mip", caseStrictHostCopy, &GxNativeDebug::csHostCopyTwinLevel0, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, false },
	{ "cs-host-copy-refused", caseHostCopyRefused, &GxNativeDebug::csCopySkipDestBlockCheck, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 0, true },
	{ "strict-host-copy-dxt-src", caseStrictHostCopy, &GxNativeDebug::csCopyValidateSource, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 1, true },
	{ "strict-host-copy-nobc", caseStrictHostCopy, &GxNativeDebug::csHostCopyTwinPhysFmt, true, 1, false, D3DMULTISAMPLE_NONE, 1, true, nullptr, nullptr, false, false, false, 64, 1, false },
	{ "cs-host-copy-refused-count", caseHostCopyRefused, &GxNativeDebug::csFrontRefusalCounts, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64, 0, true },
	{ "cs-getdesc-nodrain", caseCsGetDescNoDrain, &GxNativeDebug::csGetDescDrains, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, true },
	{ "cs-getdesc-race", caseCsGetDescRace, &GxNativeDebug::csGetDescRace, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true, 0, 0, true },
	{ "cs-drain-attribution", caseCsDrainAttribution, &GxNativeDebug::csDrainTimeZero, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, true },
	{ "cs-drain-attribution-s", caseCsDrainAttribution, &GxNativeDebug::csDrainTimeZeroS, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, true },
	{ "cs-copy-readback-s", caseCsCopyReadbackS, &GxNativeDebug::csReadbackNarrowS, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-managed-restore-write", caseCsManagedRestoreWrite, &GxNativeDebug::csRestoreSOnlyReadonly, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "strict-gpu-copy-rect", caseStrictGpuCopyShapes, &GxNativeDebug::csCopyGpuSquareExtent, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	// strict 0: the forwarded msaa flag is not traced and the pixels agree - the arm's own validation check detects it (normal runs)
	{ "strict-gpu-copy-msaa", caseStrictGpuCopyShapes, &GxNativeDebug::csCopyGpuMsaaFromSrc, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	// rw3-X-3: the STRICT value sweep (amendment 5 (4)); -draw / -res run on a 64 KiB ring so their big UP draw, Clear and host copy
	// take the S path
	{ "strict-sweep-state", caseStrictSweepState, nullptr, true, 1 },
	{ "strict-sweep-draw", caseStrictSweepDraw, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64 },
	{ "strict-sweep-res", caseStrictSweepRes, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 64 },
	{ "strict-sweep-misc", caseStrictSweepMisc, nullptr, true, 2, true },
	{ "cs-vs-payload", caseCsVsPayload, &GxNativeDebug::csVsByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ps-payload", caseCsPsPayload, &GxNativeDebug::csPsByRef, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	// E · water-live-native-port: the five catalogue programs live water and the remastered ground create; X- = the pre-port refusal
	{ "cs-live-programs", caseCsLivePrograms, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::psCatalogMiss },
	{ "cs-ps-bind", caseCsPsPayload, &GxNativeDebug::csPsBindDropped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-clear-rects", caseCsClearRects, &GxNativeDebug::csClearRectShort, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ps-delete-exec", caseCsPsPayload, &GxNativeDebug::csDeleteSkipsExec, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-vs-delete-exec", caseCsVsPayload, &GxNativeDebug::csDeleteSkipsExec, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ps-delete-bound", caseCsPsPayload, &GxNativeDebug::csDeleteUnbinds, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-vs-delete-bound", caseCsVsPayload, &GxNativeDebug::csDeleteUnbinds, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ps-bind-late", caseCsPsPayload, &GxNativeDebug::csPsBindFront, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-ps-draw", caseCsPsPayload, &GxNativeDebug::csPsDrawDropped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-vs-bind", caseCsVsPayload, &GxNativeDebug::csVsBindWrong, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "thread-cs-recover-latched-worker", caseCsRecoverLatchedWorker, &GxNativeDebug::csRecoverIgnoresLatchWorker, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, false, 8 },
	{ "thread-cs-recover-latched", caseCsRecoverLatched, &GxNativeDebug::csRecoverIgnoresLatch, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-slice-terminal", caseCsSliceTerminal, &GxNativeDebug::csSliceTerminalSilent, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-wait-publish", caseCsWaitPublish, &GxNativeDebug::csWaitUnpublished, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "child-wait-publish", caseChildWaitPublish, &GxNativeDebug::csWaitUnpublished, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "child-cs-wake-target", caseChildCsWakeTarget, &GxNativeDebug::csWakeLate, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "bench-front", caseBenchFront, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "strict-reorder", caseStrictReorder, &GxNativeDebug::csReorder, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-payload", caseStrictPayload, &GxNativeDebug::csPayloadLate, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-lod", caseStrictLod, &GxNativeDebug::csDropLod, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },
	{ "strict-bb-rotation", caseStrictBbRotation, &GxNativeDebug::csBbStale, false, 2, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 1, true },   // strict 1 (was 3): rw3-X-3 trin plant
	{ "cs-reclaim-identity", caseCsReclaimIdentity, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::reclaimByIndex, nullptr, false, false, true },
	{ "cs-mirror-check", caseCsMirrorCheck, &GxNativeDebug::csMirrorViewport, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, true },
	{ "cs-rt-view-retire", caseCsRtViewFail, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::unpublishedRetire, nullptr, true },
	{ "cs-exact-transform", caseCsExactTransform, &GxNativeDebug::csTransformCopyAll, true, 1 },
	{ "child-transform-edge", caseChildTransformEdge, &GxNativeDebug::csTransformCopyAll, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "cs-exact-clear", caseCsExactClear, &GxNativeDebug::csClearNullify, true, 1 },
	{ "cs-exact-up0", caseCsExactUp0, &GxNativeDebug::csUpNullEmpty, true, 1 },
	{ "cs-up-wrap", caseCsUpWrap, &GxNativeDebug::csUpShortCopy, true, 1 },
	{ "child-up-wrap", caseChildUpWrap, &GxNativeDebug::csUpShortCopy, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "thread-cs-exit-midframe", caseCsExitMidframe, &GxNativeDebug::csExitNoSettle, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "cs-exit-crt", caseCsExitCrt, &GxNativeDebug::csExitNoSpecQuiesce, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "child-exit-crt", caseChildExitCrt, &GxNativeDebug::csExitNoSpecQuiesce, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-exit-crt-nowin", caseCsExitCrtNowin, &GxNativeDebug::csExitNoSpecQuiesce, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "child-exit-crt-nowin", caseChildExitCrt, &GxNativeDebug::csExitNoSpecQuiesce, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-exit-stuck", caseCsExitStuck, &GxNativeDebug::csExitUnwatched, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },   // A60
	{ "child-cs-exit-stuck", caseChildCsExitStuck, &GxNativeDebug::csExitUnwatched, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-exit-slow", caseCsExitSlow, &GxNativeDebug::csExitWatchNoProgress, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },   // A60
	{ "child-cs-exit-slow", caseChildCsExitSlow, &GxNativeDebug::csExitWatchNoProgress, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "child-cs-exit-onexit", caseChildCsExitOnExit, &GxNativeDebug::csExitUnwatched, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },   // A60, Windows
	{ "child-exit-midframe", caseChildExitMidframe, &GxNativeDebug::csExitNoSettle, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true },
	{ "cs-worker-recover-lead", caseCsWorkerRecoverLead, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, false, 8 },
	{ "cs-worker-recover-room", caseCsWorkerRecoverRoom, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 64, 0, false, 8 },
	{ "cs-worker-selfdrain", caseCsWorkerRecoverLead, &GxNativeDebug::csWorkerSelfDrain, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true, false, true, 0, 0, false, 8 },
	{ "cs-reordered-failure", caseCsReorderedFailureKeep, &GxNativeDebug::csFailureSilent, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, false, 8 },
	{ "cs-reordered-failure-b", caseCsReorderedFailureRelease, &GxNativeDebug::csFailureSilent, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true, 0, 0, false, 8 },
	{ "cs-exec-ledger", caseCsExecLedger, &GxNativeDebug::csExecPhysical, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 2 },
	{ "bound-ref", caseBoundRef, &GxNativeDebug::noPrivateRefs, true, 1 },
	{ "level-surface", caseLevelSurface, &GxNativeDebug::levelSurfaceOwnRef, true, 1 },
	{ "copyrects", caseCopyRects, &GxNativeDebug::copySkipsResolve, true, 1 },
	{ "managed-drop", caseManagedDrop, &GxNativeDebug::dropIgnoresPin, true, 1 },
	{ "draw", caseDraw, &GxNativeDebug::dropDraws, true, 1 },
	{ "shadow-vs", caseShadowVs, &GxNativeDebug::vsConstCapsFromDxvk, true, 1 },   // rw3-R11
	{ "buffer-locks", caseBufferLocks, &GxNativeDebug::lockDropsDiscard, true, 1 },
	{ "resource-errors", caseResourceErrors, &GxNativeDebug::resourceErrorsUncounted, true, 1 },
	{ "release-thread", caseReleaseThread, &GxNativeDebug::destroyImmediately, true, 1 },
	{ "present", casePresent, &GxNativeDebug::presentSkipsTransition, true, 1, true },
	{ "gputime", caseGpuTime, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, true },
	{ "present-msaa", casePresent, &GxNativeDebug::presentSkipsTransition, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-msaa-math", caseMsaaPresentMath, &GxNativeDebug::presentResolveAverages, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	// E · seam-worker-2: the runner's ownership gate - the same case, its X- with testReadPresent's bare csSync back (xNeedsCs)
	{ "own-present-read", caseMsaaPresentMath, &GxNativeDebug::accessorBareSync, true, 1, true, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr,
		nullptr, false, false, false, 0, 0, true },
	{ "present-scaled", casePresentScaled, &GxNativeDebug::presentBlitLinear, true, 1, true, D3DMULTISAMPLE_NONE, 2 },
	{ "present-direct", casePresentDirect, &GxNativeDebug::presentDirectAverage, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-direct-frames", casePresentDirectFrames, &GxNativeDebug::presentDirectAverage, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-direct-1x", casePresentDirect, &GxNativeDebug::presentSkipsTransition, true, 1, true },
	{ "present-direct-x8", casePresentDirect, &GxNativeDebug::presentDirectAverage, true, 1, true, D3DMULTISAMPLE_4_SAMPLES, 1, false,
		nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_X8R8G8B8 },
	{ "present-direct-shortfit", casePresentDirectShortFit, &GxNativeDebug::presentDirectIgnoresFit, true, 1, true,
		D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 72, 50 },
	{ "present-scaled-direct", casePresentScaled, &GxNativeDebug::presentDirectAnyExtent, true, 1, true, D3DMULTISAMPLE_NONE, 2 },
	{ "present-direct-usage", casePresentDirectUsage, &GxNativeDebug::presentDirectIgnoresUsage, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-direct-interposed", casePresentDirectInterposed, &GxNativeDebug::presentDirectIgnoresUsage, true, 1, true },
	{ "present-direct-interposed-views", casePresentDirectInterposed, &GxNativeDebug::swapchainViewsWithoutUsage, true, 1, true },
#if defined(GX_VKCTX_HAS_SDL3)
	{ "present-resize", casePresentResize, &GxNativeDebug::ignoreDrawableSize, true, 1, true },
	{ "present-recreate-retry", casePresentRecreateRetry, &GxNativeDebug::noRecreateRetry, true, 1, true },
	{ "present-target-fail", casePresentTargetFail, &GxNativeDebug::targetCheckAfterAcquire, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-shader-fail", casePresentShaderFail, &GxNativeDebug::presentFailNoClear, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-shader-fail-old", casePresentShaderFailOld, &GxNativeDebug::presentFailNoClear, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-surface-lost", casePresentSurfaceLost, &GxNativeDebug::surfaceLostNotRecovered, true, 1, true, D3DMULTISAMPLE_NONE, 2 },
	{ "present-surface-lost-shown", casePresentSurfaceLost, &GxNativeDebug::csRecoveryStaleSource, true, 1, true, D3DMULTISAMPLE_NONE, 2, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "present-surface-lost-copy", casePresentSurfaceLost, &GxNativeDebug::csRecoveryHalfCopy, true, 1, true, D3DMULTISAMPLE_NONE, 2, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "present-surface-lost-mirror", casePresentSurfaceLost, &GxNativeDebug::csRecoveryMirror, true, 1, true, D3DMULTISAMPLE_NONE, 2, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "present-surface-lost-clip", casePresentSurfaceLost, &GxNativeDebug::csRecoveryClip, true, 1, true, D3DMULTISAMPLE_NONE, 2, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "present-rect", casePresentRect, &GxNativeDebug::csPresentTransposed, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true, 0, false, 96 },
	{ "present-surface-lost-linear", casePresentSurfaceLost, &GxNativeDebug::csRecoveryLinear, true, 1, true, D3DMULTISAMPLE_NONE, 2, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "present-submit-fail", casePresentSubmitFail, nullptr, true, 1, true },
	{ "present-create-lost", casePresentCreateLost, &GxNativeDebug::createLossUnclassified, true, 1, true },
	{ "present-create-lost-retry", casePresentCreateLostRetry, &GxNativeDebug::surfaceLostNotRecovered, true, 1, true },
	{ "present-side-lost", casePresentSideLost, &GxNativeDebug::surfaceLostNotRecovered, true, 1, true },
	{ "present-retry-frame", casePresentRetryFrame, &GxNativeDebug::retryAcquireSkipsFrameCheck, true, 1, true, D3DMULTISAMPLE_NONE, 2 },
	{ "reset-surface-lost", caseResetSurfaceLost, &GxNativeDebug::surfaceLostNotRecovered, true, 1, true },
	{ "fif-effective", caseFifEffective, &GxNativeDebug::fifWriterStays2, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "present-clamped-extent", casePresentClampedExtent, &GxNativeDebug::resizeComparesActualExtent, true, 1, true },
	{ "reset-fail-present", caseResetFailPresent, &GxNativeDebug::presentWhileNotReset, true, 1, true },
	{ "present-error", casePresentError, &GxNativeDebug::presentErrorIgnored, true, 1, true },
	{ "thread-present-skipped", caseThreadPresentSkipped, &GxNativeDebug::threadSkippedPresentOk, true, 1, true },
	{ "thread-present-deferred", caseThreadPresentDeferred, &GxNativeDebug::presentDropsPresentStatus, true, 1, true },
	{ "thread-reset-deferred", caseThreadResetDeferred, &GxNativeDebug::resetDropsPresentStatus, true, 1, true },
#endif
	{ "never-called", caseNeverCalled, &GxNativeDebug::countNothing, true, 1 },
	{ "device-retained", caseDeviceRetained, &GxNativeDebug::childrenDontRetain, true, 1 },
	{ "copy-quirks", caseCopyQuirks, &GxNativeDebug::fastPathRectOnly, true, 1 },
	{ "update-texture", caseUpdateTexture, &GxNativeDebug::updateIgnoresDirty, true, 1 },
	{ "reset", caseReset, &GxNativeDebug::resetIgnoresLosable, true, 1 },
	{ "reset-params", caseResetParams, &GxNativeDebug::resetSkipsD3D8Checks, true, 1 },
	{ "reset-implicit", caseResetImplicit, &GxNativeDebug::implicitNotLosable, true, 1 },
	{ "reset-held-storage", caseResetImplicit, &GxNativeDebug::resetDropsRetainedStorage, true, 1 },
	{ "reset-depth", caseResetDepth, &GxNativeDebug::implicitNotLosable, true, 1 },
	{ "reset-phases", caseResetPhases, &GxNativeDebug::resetPhasesCollapsed, true, 1 },
	{ "reset-phases-z", caseResetPhases, &GxNativeDebug::resetZFromOldParams, true, 1 },
	{ "reset-phases-b", caseResetPhases, &GxNativeDebug::resetBKeepsLost, true, 1 },
	{ "lod", caseLod, &GxNativeDebug::lodStoresPrevious, true, 1 },
	{ "update-dxt", caseUpdateDxt, &GxNativeDebug::updateNoBlockAlign, true, 1 },
	{ "update-dxt-nobc", caseUpdateDxt, &GxNativeDebug::copyDecodedAsBlocks, true, 1, false, D3DMULTISAMPLE_NONE, 1, true },
	{ "copy-mips", caseCopyMips, &GxNativeDebug::copySrcMipZero, true, 1 },
	{ "copy-msaa-dst", caseCopyMsaaDst, nullptr, true, 1 },   // no X- arm: the defect (the invalid copy recorded) crashed
	                                                        // MoltenVK + the validation layer on the M5 (OBSERVED 18:08Z 11/10)
	{ "copy-mips-alias", caseCopyMips, &GxNativeDebug::copyNoAliasLayout, true, 1 },
	{ "config-order", caseConfigOrder, &GxNativeDebug::configFirstLineWins, true, 1 },
	{ "uuid-gate", caseUuidGate, &GxNativeDebug::uuidGateUnwired, true, 1 },
	{ "config-unknown", caseConfigUnknown, &GxNativeDebug::configIgnoresUnknown, true, 1 },
	{ "config-present-keys", caseConfigPresentKeys, &GxNativeDebug::configIgnoresPresentKeys, true, 1 },
	{ "buffer-discard-cap", caseBufferDiscardCap, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false,
		&GXRender::Res::GxDebug::sliceCapWaitsUnsubmitted },
	{ "forget-pending-clear", caseForgetPendingClear, &GxNativeDebug::forgetDropsPendingClear, true, 1 },
	{ "order-check-reinstall", caseOrderCheckReinstall, &GxNativeDebug::orderCheckStaysInstalled, true, 1 },
	{ "swvp-state", caseSwvpState, &GxNativeDebug::swvpStateFromTranslator, true, 1 },
	{ "stats-line", caseStatsLine, &GxNativeDebug::targetSizeRefusalUncounted, true, 1 },
	{ "device-lost", caseDeviceLost, &GxNativeDebug::presentIgnoresDeviceLoss, true, 1 },
	{ "final-once", caseFinalOnce, &GxNativeDebug::finalNotOnce, true, 1 },
#if defined(GX_VKCTX_HAS_SDL3)
	{ "reset-logged", caseResetLogged, &GxNativeDebug::resetUnlogged, true, 1, true },
#endif
	{ "exit-final", caseExitFinal, &GxNativeDebug::finalAtExitOff, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "thread-exit-quiesce", caseThreadExitQuiesce, &GxNativeDebug::exitNoQuiesce, true, 1, true },
	{ "fgguard-framegen", caseFgGuardFrameGen, &GxNativeDebug::fgGuardSkipped, true, 1 },
	{ "fgguard-reflex", caseFgGuardReflex, &GxNativeDebug::fgGuardSkipped, true, 1 },
	{ "thread-fgguard-off", caseFgGuardOff, nullptr, true, 1 },
	{ "csguard-upscaler", caseCsGuardOn, &GxNativeDebug::csDlssGuardSkipped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	// E · seam-worker-2 (phase 2): motion units keep the worker on now - the lift arm (needsCs), like csguard-fsr1 / -temporal / -fsr3
	{ "csguard-motion", caseCsGuardMotion, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "csguard-temporal", caseCsGuardTemporal, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "csguard-fsr3", caseCsGuardFsr3, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "csguard-framegen", caseCsGuardOn, &GxNativeDebug::csDlssGuardSkipped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	// E · seam-worker-1 (tier 1): FSR1 keeps the worker on; the seam's S bodies, each with its omission plant (X-: xNeedsCs)
	{ "csguard-fsr1", caseCsGuardLifted, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },
	{ "seam-resolve", caseSeamResolve, &GxNativeDebug::seamNoSResolve, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "seam-fsr1", caseSeamFsr1, &GxNativeDebug::seamNoSUpscale, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "seam-rcas", caseSeamRcas, &GxNativeDebug::seamNoSUpscale, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "csguard-measure", caseCsGuardOn, &GxNativeDebug::csDlssGuardSkipped, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },   // GX_REFLEX_MEASURE_ONLY=1 alone
	{ "fgguard-measure", caseFgGuardReflex, &GxNativeDebug::fgGuardSkipped, true, 1 },
	{ "fgguard-streamline", caseFgGuardStreamline, &GxNativeDebug::fgGuardSkipped, true, 1 },   // E · seam-worker-2: a loaded Streamline alone   // GX_REFLEX_MEASURE_ONLY=1 alone
	{ "fifguard-framegen", caseFifGuardFrameGen, &GxNativeDebug::fifFgGuardSkipped, true, 1 },
	{ "fifguard-off", caseFifGuardOff, nullptr, true, 1 },
	{ "fifguard-reflex", caseFifGuardFrameGen, &GxNativeDebug::fifFgGuardSkipped, true, 1 },   // GXReflex alone (uncovered)
	{ "csguard-off", caseCsGuardOff, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },   // needsCs
	// S7 (D · look-shadowmaps): the look mask is set by name below (every sm-* arm but sm-key-off)
	{ "sm-key-off", caseSmKeyOff, nullptr, true, 1 },
	// B · a2c-1 (A108): alpha-to-coverage - the ramp at 4x (X-: no ramp in the shader), key off at 4x and key on at 1x = today's edge,
	// the A2C twins prewarmed
	{ "a2c-ramp", caseA2cRamp, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::a2cSpecBitOff },
	{ "a2c-ramp-off", caseA2cRampOff, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-ramp-1x", caseA2cRamp1x, nullptr, true, 1, true },
	// B · a2c-2: a fully transparent texel next to an opaque one keeps NO coverage; the off arm is today's control.
	// No X- arm: the product has no switch for it - the fail side is a one-time mutation run on the pre-fix shader headers (gates/)
	{ "a2c-sharp-edge", caseA2cSharpEdge, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-sharp-edge-off", caseA2cSharpEdgeOff, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	// B · a2c-2 (DLSS_INTEGRATION.md:3927 a): a first-use A2C failure is counted per window (firstuse_fail=); the plain arm is the control
	{ "a2c-firstuse-fail", caseA2cFirstUseFail, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-firstuse", caseA2cFirstUse, nullptr, true, 1, true, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-prewarm", caseA2cPrewarm, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-reset", caseA2cReset, &GxNativeDebug::a2cResetNoPrewarm, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-reset-off", caseA2cResetOff, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-reset-draw", caseA2cResetDraw, &GxNativeDebug::a2cResetNoRetarget, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-reset-draw-off", caseA2cResetDrawOff, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-reset-draw-up", caseA2cResetDrawUp, &GxNativeDebug::a2cResetNoRetarget, true, 1, false, D3DMULTISAMPLE_2_SAMPLES },
	// B · a2c-2 (the integrator's ruling after DLSS_INTEGRATION.md:3876): a failed A2C prewarm at init / at a Reset turns A2C off, never the device
	{ "a2c-prewarm-fail", caseA2cPrewarmFail, &GxNativeDebug::a2cNoFallback, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "a2c-prewarm-fail-reset", caseA2cPrewarmFailReset, &GxNativeDebug::a2cNoFallback, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "sm-key-on", caseSmKeyOn, nullptr, true, 1 },
	{ "sm-queued", caseSmQueued, &GxNativeDebug::shadowCmdDirect, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, true },   // needsCs
	// S7 P2 (D · look-shadowmaps-2): the receivers (present arms: they read the swapchain image)
	{ "sm-receive", caseSmReceive, &GxNativeDebug::smLookFrameStale, true, 1, true },
	// A39/A41 (D · look-shadowmaps-3): the perspective map (X-: the look frame never written - nothing shadowed) and the own light (X-: the
	// receiver unscaled; X-sm-ownlight-cast: the caster captured with its plain World)
	{ "sm-persp", caseSmPersp, &GxNativeDebug::smLookFrameStale, true, 1, true },
	{ "sm-persp-divide", caseSmPersp, &GxNativeDebug::smNoDivide, true, 1, true },   // the missing-divide mutant
	{ "sm-ownlight", caseSmOwnLight, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::smOwnNoReceiver },
	{ "sm-ownlight-cast", caseSmOwnLight, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::smOwnNoCaster },
	{ "sm-castonly-drop", caseSmCastOnlyDrop, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::smCastOnlyIgnoresCast },   // A44
	{ "sm-castonly-solid", caseSmCastOnlySolid, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::smCastOnlyIgnoresSolid },   // A61
	{ "sm-solid-retain", caseSmCastOnlySolid, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::bindNoStamp, nullptr },   // A63
	{ "sm-solid-retain-ib", caseSmCastOnlySolid, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, &GXRender::Res::GxDebug::bindNoStampIndex, nullptr },   // A64
	{ "sm-rebind", caseSmRebind, &GxNativeDebug::smNoRebind, true, 1, true },
	{ "sm-step-compile", caseSmStepCompile, nullptr, true, 1, true },
	// E · look-shadowmaps-9 (S7 fold TEST-ONLY commit): X- of the BROKEN-INPUT arms come from kBrokenInputArms (sm-castonly-noscope,
	// sm-cast-ordinary, sm-step-compile, sm-vel-*); sm-modulate's is X-sm-bits' plant; sm-seam-*'s is seam-fsr1's ownership plant
	{ "sm-castonly-noscope", caseSmCastOnlyNoScope, nullptr, true, 1, true },
	{ "sm-cast-ordinary", caseSmCastOrdinary, nullptr, true, 1, true },
	{ "sm-modulate", caseSmModulate, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, &GXRender::Rec::GxTranslateDebug::smBitsIgnoreBlend },
	{ "sm-vel-rigid", caseSmVelRigid, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "sm-vel-skin", caseSmVelSkin, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "sm-vel-occluded", caseSmVelOccluded, nullptr, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "sm-seam-fsr1", caseSmSeamFsr1, &GxNativeDebug::seamNoSUpscale, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "sm-seam-rcas", caseSmSeamRcas, &GxNativeDebug::seamNoSUpscale, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	// S7 A92 (P4 instrumentation): replay-k at k = 4 and its NODRAW fail arm (the env is set for them by name, before the device exists)
	{ "sm-replay4", caseSmReplay4, nullptr, true, 1, true },
	{ "sm-replay4-nodraw", caseSmReplay4NoDraw, nullptr, true, 1, true },
	{ "sm-create-fail", caseSmCreateFail, &GxNativeDebug::smCreateFailKeepsBit, true, 1 },
	{ "sm-prewarm", caseSmPrewarm, nullptr, true, 1 },
	{ "exit-quiesce", caseExitQuiesce, &GxNativeDebug::exitNoQuiesce, true, 1, true, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	// R41 follow-up: the same two exit arms OFFSCREEN (the first frame is an offscreen Present), so the Windows standalone suite - which has no
	// SDL3 window - RUNS them instead of skipping (rw3-X-3's -nowin idiom). exit-quiesce-nowin is by name only, as exit-quiesce.
	{ "thread-exit-quiesce-nowin", caseThreadExitQuiesce, &GxNativeDebug::exitNoQuiesce, true, 1, false },
	{ "exit-quiesce-nowin", caseExitQuiesce, &GxNativeDebug::exitNoQuiesce, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, true },
	{ "device-lost-mid-present", caseDeviceLostMidPresent, &GxNativeDebug::presentLateLossOk, true, 1 },
	{ "get-indices", caseGetIndices, &GxNativeDebug::getIndicesNeverCalled, true, 1 },
	{ "get-texture", caseStateGetters, &GxNativeDebug::getTextureNeverCalled, true, 1 },
	{ "get-stream-source", caseStateGetters, &GxNativeDebug::getStreamSourceNeverCalled, true, 1 },
	{ "get-vertex-shader", caseStateGetters, &GxNativeDebug::getVertexShaderNeverCalled, true, 1 },
	{ "get-pixel-shader", caseStateGetters, &GxNativeDebug::getPixelShaderNeverCalled, true, 1 },
	{ "shadow-gpu-jitter", caseShadowGpuJitter, nullptr, true, 1 },   // both directions in-arm; the X- run is the -DGX_TEST_NO_SHADOW_JITTER build
	{ "lodbias-sampler", caseLodBiasSampler, nullptr, true, 1 },   // both directions in-arm; a plant would be a translator edit (A22 busy)
	{ "idle-pressure", caseIdlePressure, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false,
		&GXRender::Res::GxDebug::pressureIsPending },
	{ "world-only", caseWorldOnly, nullptr, true, 1 },
	{ "clear-dropped-depth", caseClearDroppedDepth, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::depthClearFollowsDrop },
	{ "motion-off", caseMotionOff, &GxNativeDebug::motionSampledAlways, true, 1 },
	{ "mv-camera", caseMvCamera, &GxNativeDebug::motionIdentityReprojection, true, 1 },
	{ "mv-zoom-jitter", caseMvZoomJitter, &GxNativeDebug::motionJitterInVelocity, true, 1 },
	{ "jitter-sign", caseJitterSign, &GxNativeDebug::motionFlipJitterSign, true, 1 },
	{ "jitter-scope", caseJitterScope, &GxNativeDebug::motionJitterAfterEnd, true, 1 },
	{ "mv-object", caseMvObject, &GxNativeDebug::motionIgnoreObject, true, 1 },
	// E · seam-worker-2 (phase 2): the motion entry points' S bodies with the worker on, each with its omission plant (X-: xNeedsCs)
	{ "seam-begin", caseSeamMotion, &GxNativeDebug::seamNoSBegin, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "seam-end", caseSeamMotion, &GxNativeDebug::seamNoSEnd, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	{ "mv-object-late", caseMvObjectLate, &GxNativeDebug::seamNoSVelocity, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, true },
	// D · look-fdepth-plus (F-depth+): the probe (host.fdepthProbe) is set by name in the runner; fdepth-off runs without it
	{ "fdepth-off", caseFdepthOff, &GxNativeDebug::fdepthTapWithoutReader, true, 1 },
	{ "fdepth-1x", caseFdepth1x, &GxNativeDebug::fdepthFallbackNothing, true, 1 },
	{ "fdepth-msaa", caseFdepthMsaa, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::scopeResolveDropped },
	{ "fdepth-rt", caseFdepthRt, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::scopeResolveDropped },
	{ "fdepth-pass", caseFdepthPass, &GxNativeDebug::fdepthFallbackNothing, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "fdepth-order", caseFdepthOrder, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::resolveNotSuspended },
	{ "fdepth-once", caseFdepthOnce, &GxNativeDebug::fdepthSecondTap, true, 1, false, D3DMULTISAMPLE_4_SAMPLES },
	{ "fdepth-forget", caseFdepthForget, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::forgetKeepsResolve },
	{ "fdepth-motion", caseFdepthMotion, &GxNativeDebug::fdepthFallbackNothing, true, 1 },
	{ "fdepth-repaint", caseFdepthRepaint, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::resolveKeepsRepaintedColour },
	{ "fdepth-source", caseFdepthSource, nullptr, true, 1, false, D3DMULTISAMPLE_4_SAMPLES, 1, false, nullptr, nullptr, false, false, false, 0,
		0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::resolveAnyColourSource },
	{ "world-ref", caseWorldRef, nullptr, true, 1 },
	{ "jitter-abandon", caseJitterAbandon, &GxNativeDebug::motionAbandonKeepsJitter, true, 1 },
	{ "jitter-inherit", caseJitterInherit, &GxNativeDebug::motionNoSeed, true, 1 },
	// PREREG_T A35 (D · render-motion-units-1): the motion bridge's gx:vel-* programs; the arms are in-case controls (velocity
	// follows the previous-position input; occluded pixels stay unwritten) and vel-off (key off: no module, the draw is refused)
	{ "vel-rigid", caseVelRigid, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "vel-rigid-persp", caseVelRigidPersp, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "vel-skin", caseVelSkin, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "vel-occluded", caseVelOccluded, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	{ "vel-off", caseVelOff, nullptr, true, 1 },
	{ "fdepth-vel-occluded", caseFdepthVelOccluded, &GxNativeDebug::fdepthFallbackNothing, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		nullptr, false, false, false, 0, 0, false, 0, false, 0, nullptr, false, D3DFMT_A8R8G8B8, 64, 64, true },
	// PREREG_T A25 amendment 1
	{ "a25-blend", caseA25Blend, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::nonInputStale, false, false, false, 0, 0, false, 0, false, 0, nullptr, true },
	{ "a25-cull", caseA25Cull, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::nonInputStale, false, false, false, 0, 0, false, 0, false, 0, nullptr, true },
	{ "a25-stencil", caseA25Stencil, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::nonInputStale, false, false, false, 0, 0, false, 0, false, 0, nullptr, true },
	{ "a25-sampler", caseA25Sampler, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::nonInputStale, false, false, false, 0, 0, false, 0, false, 0, nullptr, true },
	{ "a25-tfactor", caseA25Tfactor, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr,
		&GXRender::Rec::GxTranslateDebug::packTableShort, false, false, false, 0, 0, false, 0, false, 0, nullptr, true },
	{ "a25-stencil-faces", caseA25StencilFaces, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false, false,
		false, 0, 0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::stencilFrontOnly, true },
	{ "clear-midframe-rebind", caseClearMidframeRebind, nullptr, true, 1, false, D3DMULTISAMPLE_NONE, 1, false, nullptr, nullptr, false,
		false, false, 0, 0, false, 0, false, 0, &GXRender::Rec::GxRecDebug::noRebindAfterClear, true },
};

struct ArmResult
{
	std::string name, detail;
	bool expectFail = false, failed = false, skipped = false;
	GXRender::Rec::GxTranslateStats pack;   // the arm's translator pack counters (PREREG_T A25 amendment 1: suite totals)
	bool ownCounters = false;               // pack reused / packed / checks were read from the arm's own device
	bool invalid = false;                   // A25 X- arm that failed by something other than its plant: always a FAIL
};

ArmResult runArmBody(const std::string& arm, const Options& opt);

// A23-B: a hung arm (a deadlock) ends the process with its name after GX_ARM_WATCHDOG_S seconds (default 300) instead of eating the
// caller's time budget silently
std::atomic<GxNativeDevice*> g_armDevice { nullptr };   // the running arm's device: the watchdog's hang report reads its stream
ArmResult runArm(const std::string& arm, const Options& opt)
{
	const char* w = std::getenv("GX_ARM_WATCHDOG_S");
	const long limit = w ? std::strtol(w, nullptr, 10) : 300;
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	std::thread dog([&] {
		std::unique_lock<std::mutex> g(m);
		if (!cv.wait_for(g, std::chrono::seconds(limit), [&] { return done; })) {
			std::fprintf(stderr, "WATCHDOG: arm %s exceeded %ld s - a hang; ending the process\n", arm.c_str(), limit);
			if (GxNativeDevice* nd = g_armDevice.load()) {   // racy reads on purpose: the process is hung, nothing else will run
				GxNativeCs& cs = nd->csStream();
				std::fprintf(stderr, "WATCHDOG: cs on=%d queued=%llu done=%llu held=%d producer_waiting=%d\n", nd->csOn() ? 1 : 0,
					(unsigned long long)cs.queued(), (unsigned long long)cs.done(), cs.testHeld() ? 1 : 0, cs.testProducerWaiting() ? 1 : 0);
			}
#if !defined(_WIN32)
			char cmd[160];   // every thread's stack (-free evidence for an intermittent hang)
			std::snprintf(cmd, sizeof(cmd), "/usr/bin/sample %d 2 -file /tmp/gx-hang-%d.txt > /dev/null 2>&1", int(getpid()), int(getpid()));
			const int rc = std::system(cmd);
			std::fprintf(stderr, "WATCHDOG: stacks (sample rc %d) in /tmp/gx-hang-%d.txt\n", rc, int(getpid()));
#endif
			std::fflush(stderr);
			say("ARM %-16s FAIL (WATCHDOG: hung > %ld s)", arm.c_str(), limit);
			std::_Exit(124);
		}
	});
	ArmResult r = runArmBody(arm, opt);
	{
		std::lock_guard<std::mutex> g(m);
		done = true;
	}
	cv.notify_one();
	dog.join();
	return r;
}

ArmResult runArmBody(const std::string& arm, const Options& opt)
{
	ArmResult r;
	r.name = arm;
	r.expectFail = arm.compare(0, 2, "X-") == 0;
	const std::string cn = r.expectFail ? arm.substr(2) : arm;
	const Case* c = nullptr;
	for (const Case& k : kCases)
		if (cn == k.name)
			c = &k;
	if (!c) {
		r.detail = "no such case";
		r.failed = !r.expectFail;
		return r;
	}
#if defined(_WIN32)
	if (r.expectFail && cn == "cs-reset-drain") {   // its plant runs in a child process (posix_spawn: POSIX only) - see caseCsResetDrain
		r.detail = "the planted form runs in a child process (POSIX only): in-process its own race crashed the Windows suite";
		r.skipped = true;   // counted as SKIP, never as a pass
		return r;
	}
#endif
	if (cn == "fgguard-streamline" && GXRender::Dev::GxNativeDevice::streamlineBuilt()) {   // E · seam-worker-2: see caseFgGuardStreamline
		r.detail = "this gx_device is a GX_STREAMLINE build: the fake host.streamline would be dereferenced (dlssInit, the loader root)";
		r.skipped = true;   // counted as SKIP, never as a pass
		return r;
	}
	if (r.expectFail && cn == "fgguard-streamline") {   // Review seam2 c-r1 P3: the plant's symptom is the thread left ON - an env that
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");   // forces it off would "detect" it by the missing line alone
		if (st && std::strcmp(st, "0") == 0) {
			r.detail = "GX_VK_SUBMIT_THREAD=0 forces the thread off: the plant (guard skipped -> thread ON) cannot show";
			r.skipped = true;   // counted as SKIP, never as a pass
			return r;
		}
	}
	if (r.expectFail && cn == "fif-effective" && opt.fif == 2) {   // by name: the case exists only with SDL3
		r.detail = "the plant (writer keeps 2) is invisible at --fif 2";
		r.skipped = true;   // counted as SKIP, never as a pass
		return r;
	}
#if defined(_WIN32)
	// A23-B's child-process arms spawn this exe (posix_spawn): POSIX only. render/integration has no Windows leg, so on Windows they
	// FAILED "needs posix_spawn" instead of skipping (render/dlss's Windows rule-0 run, D · dlss-fg-2 15/10). The CRT-exit check runs
	// on Windows as `--arm child-exit-crt-nowin`, directly (the case's own hint).
	static const char* const kPosixSpawnArms[] = { "cs-const-bounds", "cs-deep-clear", "cs-wait-publish", "cs-exact-transform",
		"cs-up-wrap", "thread-cs-exit-midframe", "cs-exit-crt", "cs-exit-crt-nowin",
		"cs-wake-target", "cs-exit-stuck", "cs-exit-slow" /* A55 / A60 parents: their children run directly on Windows */ };
	for (const char* a : kPosixSpawnArms)
		if (cn == a) {
			r.detail = "needs posix_spawn (a POSIX child process); on Windows the CRT-exit check is --arm child-exit-crt-nowin";
			r.skipped = true;   // counted as SKIP, never as a pass
			return r;
		}
#endif
	StubD3D8 parent;
	GxNativeHost host;
	{
		std::lock_guard<std::mutex> g(g_logLock);
		g_log.clear();
	}
	host.log = captureLog;
	host.validation = opt.validation;
	host.gpuTime = c->gpuTime;
	host.motionUnits = c->motionUnits;
	host.deviceSelect = opt.device;
	host.framesInFlight = opt.fif;
	if (cn == "fgguard-framegen")
		host.requestedFrameGen = 2;   // the player's GXFrameGen (the request, not a loaded Streamline)
	// the world-pass arms drive worldBegin/worldEnd: in the game a world pass exists only under GXUpscaler != off, which the guard keys on
	// E · seam-worker-2 (phase 2): their request is MetalFX temporal (6, lifted: the worker stays ON at CS1); csguard-upscaler keeps DLSS (4,
	// still forced off - it is that guard's test)
	static const char* const kWorldPassArms[] = { "motion-off", "mv-camera", "mv-zoom-jitter", "jitter-sign", "jitter-scope", "mv-object",
		"mv-object-late", "world-ref", "jitter-abandon", "jitter-inherit", "csguard-upscaler",
		// and the arms of render/dlss features that exist only under an upscaler: R-SL2 mip bias at render scale, R-DL6 shadow jitter -
		// with the A23-B worker on, lodbias-sampler counted its sampler/set before the worker ran the draw (+0/+0, Windows 15/10)
		"lodbias-sampler", "shadow-gpu-jitter", "fdepth-motion" /* D · look-fdepth-plus: a tap inside a motion world pass */ };
	for (const char* a : kWorldPassArms)
		if (cn == a)
			host.requestedUpscaler = cn == "csguard-upscaler" ? 4 : 6;   // GXGraphics::UPSCALER_DLSS / _METALFX_TEMPORAL (requests; no backend)
	if (cn.compare(0, 3, "sm-") == 0 && cn != "sm-key-off")
		host.lookFeatures = 1u;   // S7: GXGraphics::LOOK_SHADOWMAP
	// S7 P2: GX_NATIVE_TEST_LOOK=1 runs EVERY arm with the key-on family (set 2 + the receiver modules) - the A20/A36 "suites with the key
	// ON" gate under core + sync validation; sm-key-off keeps its key off
	if (const char* lk = std::getenv("GX_NATIVE_TEST_LOOK"))
		if (lk[0] == '1' && cn != "sm-key-off")
			host.lookFeatures = 1u;
	// B · a2c-1 (A108): the a2c-* arms run with GXAlphaToCoverage on (look bit 1), except the key-off one; GX_NATIVE_TEST_A2C=1 runs EVERY
	// arm with it (the "suites with the key on" gate: only the multisampled arms load the family)
	const bool a2cOffArm = cn == "a2c-ramp-off" || cn == "a2c-reset-off" || cn == "a2c-reset-draw-off" || cn == "a2c-sharp-edge-off";
	if (cn.compare(0, 4, "a2c-") == 0 && !a2cOffArm)
		host.lookFeatures |= 2u;
	if (const char* a2 = std::getenv("GX_NATIVE_TEST_A2C"))
		if (a2[0] == '1' && !a2cOffArm)
			host.lookFeatures |= 2u;
	if (cn.compare(0, 11, "a2c-prewarm") == 0 || cn.compare(0, 9, "a2c-reset") == 0)   // a2c-prewarm, -prewarm-fail[-reset], a2c-reset*
		host.prebuildGenericSet = true;
	host.testShadowMapCreateFails = cn == "sm-create-fail";
	host.testShadowMapFailKeepsBit = r.expectFail && c->plant == &GxNativeDebug::smCreateFailKeepsBit;   // creation-time plant
	if (cn == "sm-prewarm")
		host.prebuildGenericSet = true;
	// S7 A92: replay-k is read once at the atlas's creation - set before the device exists; both variables set
	// explicitly for every arm and restored on EVERY return path of this function (a skipped arm included), never leaking into the next
	struct ReplayEnv {
		explicit ReplayEnv(const std::string& n)
		{
			const bool on = n == "sm-replay4" || n == "sm-replay4-nodraw";
			if (on)
				setenv("GX_SHADOWMAP_DEBUG_REPLAY", "4", 1);
			else
				unsetenv("GX_SHADOWMAP_DEBUG_REPLAY");
			if (n == "sm-replay4-nodraw")
				setenv("GX_SHADOWMAP_DEBUG_REPLAY_NODRAW", "1", 1);
			else
				unsetenv("GX_SHADOWMAP_DEBUG_REPLAY_NODRAW");
		}
		~ReplayEnv()
		{
			unsetenv("GX_SHADOWMAP_DEBUG_REPLAY");
			unsetenv("GX_SHADOWMAP_DEBUG_REPLAY_NODRAW");
		}
	} replayEnv(cn);
	if (cn == "csguard-motion")
		host.motionUnits = true;
	// E · seam-worker-1: the seam arms' request is a LIFTED (spatial) kind - the worker stays on at CS1
	if (cn == "csguard-fsr1" || cn.compare(0, 5, "seam-") == 0 || cn == "sm-seam-fsr1" || cn == "sm-seam-rcas")
		host.requestedUpscaler = 2;   // GXGraphics::UPSCALER_FSR1
	// E · seam-worker-2 (phase 2): the temporal lifts, and the motion seam arms' request (a world pass exists only under an upscaler)
	if (cn == "csguard-temporal" || cn == "seam-begin" || cn == "seam-end")
		host.requestedUpscaler = 6;   // GXGraphics::UPSCALER_METALFX_TEMPORAL
	if (cn == "csguard-fsr3")
		host.requestedUpscaler = 3;   // GXGraphics::UPSCALER_FSR3
	if (cn.compare(0, 7, "fdepth-") == 0 && cn != "fdepth-off")   // D · look-fdepth-plus: the TEST-ONLY dummy reader, depth + colour
		host.fdepthProbe = GX_SCENE_DEPTH | GX_SCENE_COLOUR;
	host.fdepthForcePass = cn == "fdepth-pass";
	if (cn == "csguard-framegen")
		host.requestedFrameGen = 1;
	if (cn == "fifguard-framegen" || cn == "fifguard-off" || cn == "fifguard-reflex")
		host.framesInFlight = 3;   // the A30 knob asks for 3
	if (cn == "fifguard-reflex")
		host.requestedReflex = 1;   // Reflex alone, no FrameGen
	if (cn == "fifguard-framegen")
		host.requestedFrameGen = 1;
	host.testNoFifGuard = r.expectFail && c->plant == &GxNativeDebug::fifFgGuardSkipped;   // creation-time plant: FIF 3 kept
	host.testPresentDirectIgnoresUsage = r.expectFail && c->plant == &GxNativeDebug::presentDirectIgnoresUsage;   // creation-time plant
	if (cn == "present-direct-usage")   // FG P0 H3: the planted proxy - swapchain images without COLOR_ATTACHMENT
		host.testSwapchainDropUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	host.testSwapchainViewsWithoutUsage = r.expectFail && c->plant == &GxNativeDebug::swapchainViewsWithoutUsage;   // creation-time plant
	if (cn == "present-direct-interposed" || cn == "present-direct-interposed-views")   // FG P0 H3: what a loaded DLSS-G sets
		host.testSwapchainInterposed = true;
	host.testNoCsDlssGuard = r.expectFail && c->plant == &GxNativeDebug::csDlssGuardSkipped;   // creation-time plant: the guard skipped
	if (cn == "fgguard-reflex")
		host.requestedReflex = 1;
	// E · seam-worker-2: a fake loaded Streamline (never dereferenced by a non-Streamline gx_device; the case SKIPs in a Streamline build)
	static int s_fakeStreamline = 0;
	if (cn == "fgguard-streamline" && !GXRender::Dev::GxNativeDevice::streamlineBuilt())
		host.streamline = reinterpret_cast<GXRender::Upscale::Streamline*>(&s_fakeStreamline);
	host.testNoFgThreadGuard = r.expectFail && c->plant == &GxNativeDebug::fgGuardSkipped;   // creation-time plant: the guard skipped
	host.testFifWriterStays2 = r.expectFail && c->plant == &GxNativeDebug::fifWriterStays2;   // a creation-time plant
	D3DPRESENT_PARAMETERS pp = presentParams(c->windowed, c->backBuffers, c->msaa);
	pp.BackBufferFormat = c->bbFormat;
	pp.BackBufferWidth = c->bbW;
	pp.BackBufferHeight = c->bbH;
	if (c->bbWidth)
		pp.BackBufferWidth = c->bbWidth;
	static GXRender::Res::FormatCaps noBC;
	noBC.noBC = true;
	host.testFormatCaps = c->noBC ? &noBC : nullptr;
	void* window = nullptr;
	if (c->window) {
#if defined(GX_VKCTX_HAS_SDL3)
		static bool sdlUp = false;
		if (!sdlUp) {
			GXRender::Vk::sdl3PreferVulkanLoader();   // the loader, never MoltenVK by path (the game's rule, SDL3Main)
			sdlUp = SDL_Init(SDL_INIT_VIDEO);
		}
		window = sdlUp ? SDL_CreateWindow("gx_native_test", int(pp.BackBufferWidth) * c->windowScale, int(pp.BackBufferHeight) * c->windowScale,
			SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN) : nullptr;
		static std::vector<const char*> exts;   // outlives device creation
		exts.clear();
		host.getInstanceProcAddr = window ? GXRender::Vk::sdl3VulkanLoader(&exts) : nullptr;
		host.instanceExtensions = exts;
		host.createSurface = GXRender::Vk::sdl3CreateSurface;
		host.destroySurface = GXRender::Vk::sdl3DestroySurface;
		host.drawableSize = testDrawableSize;   // = sdl3DrawableSize + an off-main-thread count (A23-B arm 10)
		host.presentSupport = GXRender::Vk::sdl3PresentSupport;
		pp.hDeviceWindow = static_cast<HWND>(window);
#endif
		if (!window || !host.getInstanceProcAddr) {
			r.detail = "no SDL3 window/Vulkan in this build";
			r.skipped = true;   // counted as SKIP, never as a pass
			return r;
		}
	}
	Env e;
	std::string why;
	if (c->ringKiB)
		setenv("GX_NATIVE_CS_RING_KIB", std::to_string(c->ringKiB).c_str(), 1);
	if (c->stagingKiB)
		setenv("GX_NATIVE_STAGING_KIB", std::to_string(c->stagingKiB).c_str(), 1);
	const bool measureArm = cn == "csguard-measure" || cn == "fgguard-measure";   // TEST-ONLY Reflex with no GXReflex request
	if (measureArm)
		setenv("GX_REFLEX_MEASURE_ONLY", "1", 1);
	const HRESULT created = GxNativeCreateDevice(&parent, 0, D3DDEVTYPE_HAL, static_cast<HWND>(window), D3DCREATE_MIXED_VERTEXPROCESSING,
		pp, host, &e.dev, &why);
	if (measureArm)
		unsetenv("GX_REFLEX_MEASURE_ONLY");
	if (c->ringKiB)
		unsetenv("GX_NATIVE_CS_RING_KIB");
	if (c->stagingKiB)
		unsetenv("GX_NATIVE_STAGING_KIB");
	if (FAILED(created)) {
		r.detail = "device creation failed: " + why;
		r.failed = !r.expectFail;   // an infrastructure failure never counts as a detection
		if (r.expectFail)
			r.failed = false, r.detail += " [arm cannot run]";
		return r;
	}
	e.nd = GxNativeFromDevice(e.dev);
	g_armDevice.store(e.nd);
	e.window = window;
	e.nd->debug().capturePresent = c->window;   // testReadPresent reads the swapchain image the last Present showed (W-a)
	const bool contractArm = std::find_if(std::begin(kCsContractArms), std::end(kCsContractArms),
		[&](const char* n) { return std::strcmp(c->name, n) == 0; }) != std::end(kCsContractArms);
	// the mirror-check mode never runs an arm that holds the worker (needsCs, a tiny ring, a CS=1 contract branch): its drains
	// would wait for the hold forever (§3.4: never combined with the async race arms)
	const bool skipMirror = c->mirrorOnly != e.nd->mirrorCheckOn() && (c->mirrorOnly || c->needsCs || c->ringKiB || (contractArm && e.nd->csOn()));
	if (((c->needsCs || (c->xNeedsCs && r.expectFail)) && !e.nd->csOn()) || skipMirror) {   // A23-B: proves nothing without the worker
		r.detail = skipMirror ? (c->mirrorOnly ? "runs in the mirror-check mode only" : "mirror-check mode: no worker-hold arm") :
			"needs the worker (GX_NATIVE_CS=1)";
		r.skipped = true;
		e.dev->Release();
#if defined(GX_VKCTX_HAS_SDL3)
		if (window)
			SDL_DestroyWindow(static_cast<SDL_Window*>(window));
#endif
		return r;
	}
	// A23-B §7 STRICT: the SERIAL schedule (getFF synchronous: set before the device) - never for the cs- arms (they hold the worker)
	// nor the thread- arms (they hold gx-vk-submit): a drain / device idle after every dispatch would wait for the held thread
	// forever, and the hold already fixes the schedule those arms test
	if (opt.serial && serialArm(*c))
		e.nd->debug().csSerial = true;
	if (r.expectFail && c->plant)
		e.nd->debug().*(c->plant) = true;
	if (r.expectFail && c->resPlant)
		e.nd->resources().debug().*(c->resPlant) = true;
	if (r.expectFail && c->trPlant)
		e.nd->translateDebug().*(c->trPlant) = true;
	if (r.expectFail && c->recPlant)
		e.nd->writer().debug().*(c->recPlant) = true;
	// A planted A23-B race (an X- arm on the worker, e.g. X-cs-copy-readback-s: a read-back on the front without its S) can submit inside
	// the worker's open frame; the writer's guard (checkForeignSubmit) then ABORTED the whole suite at the next endFrame - the device
	// release, after the arm's own verdict, intermittently (look-shadowmaps-5 18/10 05:04Z). For these arms the guard COUNTS it ("[GXREC]
	// RETIRE VIOLATION" in the log): a second detector of the same plant, never a suite abort. Clean arms keep production's abort; the
	// guard's own count-not-abort path is proven by gx_record_test's foreign-submit arm. (lead C43 18/10)
	if (r.expectFail && (c->needsCs || c->xNeedsCs))
		e.nd->writer().debug().foreignSubmitNoAbort = true;
	g_setupFailed = false;
	const uint64_t mismatches0 = GXRender::Rec::g_recTest.packCheckMismatches.load(std::memory_order_relaxed);
	const uint64_t tfOnly0 = GXRender::Rec::g_recTest.packCheckTFactorOnly.load(std::memory_order_relaxed);
	g_brokenInput = r.expectFail && brokenInputArm(c->name);   // E · look-shadowmaps-9: the case breaks its own input
	r.failed = !c->fn(e, &r.detail);
	g_brokenInput = false;
	{   // B · a2c-1: an X- arm whose plant has ONE named symptom counts as detected only with that symptom's token
		static const std::pair<const char*, const char*> kXSymptom[] = { { "a2c-ramp", "[mixing off the edge" },
			{ "a2c-prewarm-fail", "[A2C failed init" }, { "a2c-prewarm-fail-reset", "[A2C still on after a failed prewarm]" } };
		for (const auto& xs : kXSymptom)
			if (r.expectFail && r.failed && cn == xs.first && r.detail.find(xs.second) == std::string::npos) {
				r.failed = false;   // -> PLANT NOT DETECTED
				r.detail += std::string(" [failed, but without the planted symptom '") + xs.second + "']";
			}
	}
	if (g_setupFailed)
		r.failed = !r.expectFail;   // no gate ran: never a pass and never a detection
	if (e.setupFailed) {   // no gate ran: never a pass, never a detection
		r.failed = !r.expectFail;
		r.detail += " [setup failed: the case never reached its gate]";
	}
	// A25: with GX_TR_PACK_CHECK=1 a reused pack must equal a fresh one - on EVERY device the arm opened (the process-wide tally
	// also sees a device the case replaced or released itself)
	r.pack.packCheckMismatches = GXRender::Rec::g_recTest.packCheckMismatches.load(std::memory_order_relaxed) - mismatches0;
	if (r.pack.packCheckMismatches && !r.expectFail)
		r.failed = true;
	if (!e.devReleased) {   // the arm's own device's reuse counters (descriptive; a released / replaced device is not in them)
		const GXRender::Rec::GxTranslateStats& ts = e.nd->translator().stats();
		r.pack.packReused = ts.packReused;
		r.pack.packed = ts.packed;
		r.pack.packChecks = ts.packChecks;
		r.ownCounters = true;
	}
	r.detail += " [pack reused=" + std::to_string(r.pack.packReused) + " packed=" + std::to_string(r.pack.packed) + " checks=" +
		std::to_string(r.pack.packChecks) + (r.ownCounters ? "" : " (device released by the case)") + " mismatches(all devices)=" +
		std::to_string(r.pack.packCheckMismatches) + "]";
	// E · seam-worker-2 (seam2 design r1 P2, dlss-integrator's ruling 18/10): with the worker on, EVERY clean arm is an ownership
	// check - a front recording outside an S body on the arm's own device fails it, whatever the case itself checked. The X- arm of this
	// gate is X-own-present-read (accessorBareSync: a test accessor drains with a bare csSync again, the case's own checks still pass).
	// the case's own verdict (setup included) before the ownership gate below adds its own
	const bool caseFailed = r.failed || e.setupFailed || g_setupFailed;
	// An ownership plant (an S dropped, or an inspection accessor's bare csSync) is caught ONLY by its symptom: the cases skip their own
	// ownership check when planted, every functional check must still pass, and the arm's device counted a front recording outside an S
	// (seam2 code r1 P2-2, r2): the verdict is taken after validation, below.
	const bool ownPlant = c->plant == &GxNativeDebug::accessorBareSync || c->plant == &GxNativeDebug::seamNoSResolve ||
		c->plant == &GxNativeDebug::seamNoSUpscale || c->plant == &GxNativeDebug::seamNoSBegin || c->plant == &GxNativeDebug::seamNoSEnd ||
		c->plant == &GxNativeDebug::seamNoSVelocity;
	const uint64_t ownViolations = e.devReleased ? 0 : e.nd->csOwnerViolations();
	if (ownViolations && e.nd->csOn()) {
		r.detail += " [OWNERSHIP: " + std::to_string(ownViolations) + " front recordings outside an S body]";
		if (!r.expectFail || ownPlant)
			r.failed = true;
	}
	if (!e.devReleased && e.nd->mirrorCheckOn() && e.nd->mirrorMismatches()) {   // A23-B §3.4: every arm is a mirror check in that mode
		r.detail += " [MIRROR: " + std::to_string(e.nd->mirrorMismatches()) + " fields differed]";
		if (!r.expectFail)
			r.failed = true;
	}
	const GxNativeValidation v = e.devReleased ? GxNativeValidation() : e.nd->validation();
	if (v.errors || v.hazards) {
		r.detail += " [validation: " + std::to_string(v.errors) + " errors, " + std::to_string(v.hazards) + " hazards; first: " + v.first + "]";
		if (!r.expectFail)
			r.failed = true;
	}
	// E · look-shadowmaps-9: the broken-input X- arms and X-sm-modulate fail on their own probe ONLY - with any validation
	// error or hazard the arm is INVALID, never a detection
	if (r.expectFail && (brokenInputArm(c->name) || cn == "sm-modulate") && (v.errors || v.hazards)) {
		r.invalid = true;
		r.detail += " [INVALID: a validation error / hazard in an X- that must fail on its own probe only]";
	}
	// A25 X- arms: a detection only when the case failed AND every failure was the plant's stale value, with no
	// validation error and no check-mode mismatch (except packTableShort's own: a stale pack IS its symptom). A failure of any other
	// kind is neither a detection nor a clean non-detection: the arm is INVALID (a FAIL that no verdict may read as "not detected").
	if (r.expectFail && ownPlant) {   // E · seam-worker-2: see the ownership gate above
		const bool mirrorBad = !e.devReleased && e.nd->mirrorCheckOn() && e.nd->mirrorMismatches();
		const bool other = caseFailed || r.pack.packCheckMismatches || mirrorBad || v.errors || v.hazards;
		if (!ownViolations || other) {
			r.invalid = true;
			r.detail += other ? " [INVALID: a failure other than the ownership symptom (a check, setup, validation, mirror or pack)]" :
				" [INVALID: no ownership violation counted]";
		}
	}
	if (r.expectFail && c->symptomGated && !e.setupFailed) {
		// packTableShort's own symptom: EVERY mismatch differs from a fresh pack in the TextureFactor word alone
		const uint64_t tfOnly = GXRender::Rec::g_recTest.packCheckTFactorOnly.load(std::memory_order_relaxed) - tfOnly0;
		const bool mismatchIsSymptom = c->trPlant == &GXRender::Rec::GxTranslateDebug::packTableShort && tfOnly == r.pack.packCheckMismatches;
		const bool clean = !e.a25Other && !v.errors && !v.hazards && (!r.pack.packCheckMismatches || mismatchIsSymptom);
		if (!clean) {
			r.invalid = true;
			r.detail += " [INVALID: a failure other than the plant's stale value]";
		} else if (r.failed && !e.a25Symptom) {
			r.invalid = true;
			r.detail += " [INVALID: failed without the stale value]";
		}
	}
	g_armDevice.store(nullptr);
	if (!e.devReleased)
		e.dev->Release();
#if defined(GX_VKCTX_HAS_SDL3)
	if (window && !e.keepWindow)
		SDL_DestroyWindow(static_cast<SDL_Window*>(window));
#endif
	return r;
}

} // namespace

int main(int argc, char** argv)
{
	g_mainThread = std::this_thread::get_id();
	Options opt;
	std::string armList = "all";
	bool strict = false, strictTap = false;
	bool list = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			armList = argv[++i];
		else if (!std::strcmp(argv[i], "--list"))   // print the arms "all" runs, one per line, and exit (A25 eq gate's arm list)
			list = true;
		else if (!std::strcmp(argv[i], "--strict"))
			strict = true;
		else if (!std::strcmp(argv[i], "--serial")) {   // A23-B: the SERIAL schedule without the strict runner (tap children)
			opt.serial = true;
			g_strictRun = true;   // one call sequence in both modes (the CS=1 contract branches off), as --strict
			GXRender::g_gxPipeTestMutants.syncSpec = true;
		}
		else if (!std::strcmp(argv[i], "--strict-tap"))
			strictTap = true;
		else if (!std::strcmp(argv[i], "--no-validation"))
			opt.validation = false;
		else if (!std::strcmp(argv[i], "--device") && i + 1 < argc)
			opt.device = argv[++i];
		else if (!std::strcmp(argv[i], "--fif") && i + 1 < argc && (!std::strcmp(argv[i + 1], "2") || !std::strcmp(argv[i + 1], "3")))
			opt.fif = uint32_t(std::atoi(argv[++i]));
		else {
			std::fprintf(stderr, "usage: %s [--arm NAME[,NAME...]|all] [--list] [--no-validation] [--device SEL] [--fif 2|3] "
				"[--strict|--strict-tap|--serial]\n", argv[0]);
			return 2;
		}
	}
	g_fif = opt.fif;
	std::vector<std::string> arms;
	if (armList == "all") {
		// the submit thread switched off (GX_VK_SUBMIT_THREAD=0): its arms would only report that it is off
		const char* st = std::getenv("GX_VK_SUBMIT_THREAD");
		const bool threadOff = st && std::strcmp(st, "0") == 0;
		auto wanted = [threadOff](const Case& c) { return !c.byNameOnly && !(threadOff && std::strncmp(c.name, "thread-", 7) == 0); };
		for (const Case& c : kCases)
			if (wanted(c))
				arms.push_back(c.name);
		for (const Case& c : kCases)
			if (wanted(c) && (c.plant || c.resPlant || c.trPlant || c.recPlant || brokenInputArm(c.name)) && c.strict != 2)
				arms.push_back(std::string("X-") + c.name);
	} else
		for (size_t p = 0, q; p <= armList.size(); p = q + 1) {
			q = armList.find(',', p);
			if (q == std::string::npos)
				q = armList.size();
			if (q > p)
				arms.push_back(armList.substr(p, q - p));
		}
	if (list) {   // after the selection, before any atexit registration or device
		for (const std::string& a : arms)
			std::printf("%s\n", a.c_str());
		return 0;
	}
	g_self = argv[0];
	g_childOpt = opt;
	for (const std::string& a : arms) {
		if (a == "exit-quiesce" || a == "X-exit-quiesce" || a == "exit-quiesce-nowin" || a == "X-exit-quiesce-nowin")
			std::atexit(exitSentinelBefore);
		if (a == "child-exit-midframe" || a == "X-child-exit-midframe")   // A23-B arm 16: the vk wrappers before any device
			exitobs::install();
		if (a == "child-exit-crt" || a == "X-child-exit-crt" || a == "child-exit-crt-nowin" || a == "X-child-exit-crt-nowin")   // before any device: runs after the device's flush hook
			std::atexit(crtReport);
		if (a == "X-child-cs-deep-clear")   // before any device: its stream starts on the default (512 KiB) stack
			GXRender::Dev::GxNativeCs::testDefaultStack = true;
	}   // before any device: its init registers the flush after this (PREREG_T A21 am. 4)
	if (arms.empty()) {   // an empty selection proves nothing
		std::fprintf(stderr, "no arm selected\n");
		return 2;
	}
	if (strict) {
		// A23-B §7 (PREREG_T A23-B, rules written before any CS=1 data): every clean arm under the SERIAL schedule, three times -
		// GX_NATIVE_CS=0, again =0 (the A/A floor), =1. (a) an arm whose floor differs is EXCLUDED by this rule, never after CS=1
		// is seen; (c) every other arm's EXEC digest and creation ledger must be EQUAL CS=1 vs CS=0. Plants are X- arms (later).
		opt.serial = true;
		g_strictRun = true;
		GXRender::g_gxPipeTestMutants.syncSpec = true;
		int equal = 0, differ = 0, excluded = 0, armFail = 0;
		int async = 0;
		for (const std::string& a : arms) {
			if (a.compare(0, 2, "X-") == 0)
				continue;
			if (isAsyncArm(a)) {
				say("STRICT %-24s ASYNC (PREREG A23-B list: pixels and named semantics, no digest)", a.c_str());
				++async;
				continue;
			}
			ExecDigest dg[3];
			ArmResult rr[3];
			const char* mode[3] = { "0", "0", "1" };
			for (int k = 0; k < 3; ++k) {
				setenv("GX_NATIVE_CS", mode[k], 1);
				if (const char* dump = std::getenv("GX_STRICT_DUMP"))
					dg[k].m_dump = std::fopen((std::string(dump) + a + "-" + std::to_string(k) + ".txt").c_str(), "w");
				setSinks(&dg[k]);
				rr[k] = runArm(a, opt);
				setSinks(nullptr);
				if (dg[k].m_dump)
					std::fclose(dg[k].m_dump);
				dg[k].m_dump = nullptr;
			}
			const bool skipped = rr[0].skipped || rr[2].skipped;
			const bool floorOk = dg[0].equal(dg[1]);
			const bool same = floorOk && dg[2].equal(dg[0]);
			// a CS=1 contract arm's own verdict belongs to the normal runs (its CS=1 assertions); here only its digests count
			const bool contract = std::find_if(std::begin(kCsContractArms), std::end(kCsContractArms), [&](const char* n) { return a == n; }) !=
				std::end(kCsContractArms);
			const bool armsOk = contract || (!rr[0].failed && !rr[1].failed && !rr[2].failed);
			if (skipped)
				;
			else if (!floorOk)
				++excluded;
			else
				(same ? equal : differ)++;
			armFail += !skipped && !armsOk;
			say("STRICT %-24s %s cs0 %s | cs1 %s%s", a.c_str(), skipped ? "SKIP" : !floorOk ? ("EXCLUDED(floor differs: " + dg[0].parts(dg[1]) + ")").c_str() :
				same ? "EQUAL" : ("DIFFERS(" + dg[2].parts(dg[0]) + ")").c_str(), dg[0].text().c_str(), dg[2].text().c_str(), armsOk ? "" : (" [arm verdict: cs0 " + std::string(rr[0].failed ? "FAIL" : "ok") +
				" cs0b " + (rr[1].failed ? "FAIL" : "ok") + " cs1 " + (rr[2].failed ? "FAIL " + rr[2].detail.substr(0, 160) : "ok") + "]").c_str());
		}
		// §7.1 (b): every digest plant must CHANGE the EXEC digest - the clean arm at CS=0 against its X- arm at CS=1
		int detected = 0, missed = 0;
		for (const Case& c : kCases) {
			if (!c.strict || c.strict == 3 || (armList != "all" && std::find(arms.begin(), arms.end(), std::string(c.name)) == arms.end() &&
				std::find(arms.begin(), arms.end(), "X-" + std::string(c.name)) == arms.end()))
				continue;
			ExecDigest clean, planted;
			setenv("GX_NATIVE_CS", "0", 1);
			setSinks(&clean);
			const ArmResult rc = runArm(c.name, opt);
			setenv("GX_NATIVE_CS", "1", 1);
			setSinks(&planted);
			const ArmResult rp = runArm(std::string("X-") + c.name, opt);
			setSinks(nullptr);
			const bool skipped = rc.skipped || rp.skipped;
			// a TRANSLATOR-INPUT plant (kTrinPlants) proves the trin digest: EXEC + ledger EQUAL, trin DIFFERS - nothing else counts
			const bool trinOnly = isTrinPlant(c.name);
			const bool caught = !skipped && (trinOnly ? clean.execEqual(planted) && !clean.trinEqual(planted) : !clean.equal(planted));
			if (!skipped)
				(caught ? detected : missed)++;
			say("STRICT X-%-22s %s (%s) cs0 clean %s | cs1 planted %s", c.name, skipped ? "SKIP" : caught ? (trinOnly ? "DIFFERS (trin only: plant detected)" :
				"DIFFERS (plant detected)") : trinOnly ? "NOT TRIN-ONLY (PLANT NOT DETECTED)" : "EQUAL (PLANT NOT DETECTED)", clean.parts(planted).c_str(),
				clean.text().c_str(), planted.text().c_str());
		}
		unsetenv("GX_NATIVE_CS");
		say("STRICT SUMMARY equal=%d differ=%d excluded=%d async=%d arm_failures=%d plants_detected=%d plants_missed=%d %s", equal, differ,
			excluded, async, armFail, detected, missed, differ == 0 && armFail == 0 && missed == 0 ? "PASS" : "FAIL");
		return differ == 0 && armFail == 0 && missed == 0 ? 0 : 1;
	}
	if (strictTap) {
#if !defined(_WIN32)
		// A23-B §7.2 / PREREG A23-B: the SECONDARY digest - A22's GX_CMDTAP tap (gx-native-cs counts as recording), each run a CHILD
		// under --serial with its own tap file: CS=0, CS=0 again (the A/A floor: an arm whose floor differs is EXCLUDED by this rule),
		// CS=1 - EQUAL required. Then every tap plant (strict 1 or 3), X- at CS=1, must change the tap against the clean arm at CS=0.
		auto tapRun = [](const std::string& arm, const char* cs, ChildRun* cr) {
			char path[] = "/tmp/gx-native-tap-XXXXXX";
			const int fd = mkstemp(path);
			if (fd < 0)
				return std::string();
			close(fd);
			*cr = runSelf(arm, { { "GX_NATIVE_CS", cs }, { "GX_CMDTAP", path } }, 600, { "--serial" });
			return slurpAndUnlink(path);
		};
		int equal = 0, differ = 0, excluded = 0, armFail = 0, detected = 0, missed = 0, async = 0, notSerial = 0, divergenceB = 0;
		for (const std::string& a : arms) {
			if (a.compare(0, 2, "X-") == 0 || a.compare(0, 6, "child-") == 0)
				continue;
			const Case* ca = nullptr;
			for (const Case& k : kCases)
				if (a == k.name)
					ca = &k;
			if (isAsyncArm(a)) {
				say("TAP %-24s ASYNC (PREREG A23-B list)", a.c_str());
				++async;
				continue;
			}
			if (ca && !serialArm(*ca)) {   // integrator R32: a tap equality claim only under the SERIAL schedule (or thread-off)
				say("TAP %-24s n/a (holds a thread: not under the SERIAL schedule - R32)", a.c_str());
				++notSerial;
				continue;
			}
			ChildRun cr[3];
			const std::string t0 = tapRun(a, "0", &cr[0]), t1 = tapRun(a, "0", &cr[1]), t2 = tapRun(a, "1", &cr[2]);
			const bool skipped = cr[0].err.find(" SKIP (") != std::string::npos || cr[2].err.find(" SKIP (") != std::string::npos;
			const bool floorOk = !t0.empty() && t0 == t1;
			const bool same = floorOk && t2 == t0;
			// integrator ruling 13/10 (rw3-X/CS1_ARM_CONTRACT.md "tap"): these arms' tap differs by named divergence (b) and stays
			// OUTSIDE the tap claim ONLY while the bound holds - CS1 presents_skipped = CS0's + exactly one per recovery event, the
			// submissions, presents and recreations EQUAL (the EXEC digest EQUAL is --strict's); anything else FAILS
			const int events = a == "present-surface-lost" || a == "present-surface-lost-shown" || a == "present-surface-lost-copy" ||
				a == "present-surface-lost-mirror" || a == "present-surface-lost-clip" || a == "present-surface-lost-linear" ||
				a == "present-retry-frame" ? 1 : 0;
			bool divB = false;
			std::string why;
			if (events && floorOk && !same) {
				auto stat = [](const std::string& err, const char* key) {
					const size_t f = err.rfind("[GXNATIVE] stats final");
					const size_t k = f == std::string::npos ? f : err.find(std::string(" ") + key + "=", f);
					return k == std::string::npos ? -1LL : std::atoll(err.c_str() + k + std::strlen(key) + 2);
				};
				auto submits = [](const std::string& t) {
					long long n = 0;
					for (size_t q = t.find(" S"); q != std::string::npos; q = t.find(" S", q + 2))
						n += std::isdigit(static_cast<unsigned char>(t[q + 2])) != 0;
					return n;
				};
				const long long sk0 = stat(cr[0].err, "presents_skipped"), sk1 = stat(cr[2].err, "presents_skipped");
				const bool eq = stat(cr[0].err, "presents") == stat(cr[2].err, "presents") &&
					stat(cr[0].err, "surfaces_recreated") == stat(cr[2].err, "surfaces_recreated") &&
					stat(cr[0].err, "swapchains_recreated") == stat(cr[2].err, "swapchains_recreated") && submits(t0) == submits(t2);
				// what was SHOWN must differ only by the permitted unshown frames - each Present's presented
				// colour equal across the modes, except exactly `events` frames where CS1 still shows its previous frame
				auto shownSeq = [](const ChildRun& c) {
					std::vector<uint32_t> g;
					for (const std::string& l : c.lines("[TEST] SHOWN "))
						g.push_back(uint32_t(std::strtoul(l.c_str() + l.find("got=") + 4, nullptr, 16)));
					return g;
				};
				const std::vector<uint32_t> g0 = shownSeq(cr[0]), g2 = shownSeq(cr[2]);
				int moved = 0;
				bool shownOk = !g0.empty() && g0.size() == g2.size();
				for (size_t k = 0; shownOk && k < g0.size(); ++k)
					if (!rgbNear(g0[k], g2[k])) {
						shownOk = k > 0 && rgbNear(g2[k], g2[k - 1]);
						++moved;
					}
				shownOk = shownOk && moved == events;
				divB = sk0 >= 0 && sk1 == sk0 + events && eq && shownOk;
				why = " presents_skipped " + std::to_string(sk0) + " -> " + std::to_string(sk1) + (eq ? ", the rest equal" : ", OTHER COUNTERS DIFFER") +
					", shown frames " + (shownOk ? "differ only by the unshown one" : "DIFFER OTHERWISE (" + std::to_string(moved) + " moved)");
			}
			const bool contract = std::find_if(std::begin(kCsContractArms), std::end(kCsContractArms), [&](const char* n) { return a == n; }) !=
				std::end(kCsContractArms);
			const bool armsOk = contract || (cr[0].exited && cr[0].code == 0 && cr[1].exited && cr[1].code == 0 && cr[2].exited && cr[2].code == 0);
			if (skipped)
				;
			else if (!floorOk)
				++excluded;
			else if (divB)
				++divergenceB;
			else
				(same ? equal : differ)++;
			armFail += !skipped && !armsOk;
			say("TAP %-24s %s%s cs0 %016llx | cs1 %016llx%s", a.c_str(), skipped ? "SKIP" : !floorOk ? "EXCLUDED(floor differs)" : same ? "EQUAL" :
				divB ? "DIFFERS by named divergence (b), bound holds:" : "DIFFERS", why.c_str(),
				(unsigned long long)fnv64(t0.data(), t0.size()), (unsigned long long)fnv64(t2.data(), t2.size()),
				armsOk ? "" : (" [arm: cs0 " + cr[0].how() + " cs0b " + cr[1].how() + " cs1 " + cr[2].how() + "]").c_str());
		}
		for (const Case& c : kCases) {
			if (!(c.strict == 1 || c.strict == 3) || (armList != "all" && std::find(arms.begin(), arms.end(), std::string(c.name)) == arms.end() &&
				std::find(arms.begin(), arms.end(), "X-" + std::string(c.name)) == arms.end()))
				continue;
			if (isTapBlindPlant(c.name)) {
				say("TAP X-%-22s SKIP (tap-blind, registered: --strict detects it)", c.name);
				continue;
			}
			ChildRun rc, rp;
			const std::string clean = tapRun(c.name, "0", &rc), planted = tapRun(std::string("X-") + c.name, "1", &rp);
			const bool caught = !clean.empty() && !planted.empty() && clean != planted;
			(caught ? detected : missed)++;
			say("TAP X-%-22s %s cs0 clean %016llx | cs1 planted %016llx (child %s)", c.name, caught ? "DIFFERS (plant detected)" :
				"EQUAL (PLANT NOT DETECTED)", (unsigned long long)fnv64(clean.data(), clean.size()),
				(unsigned long long)fnv64(planted.data(), planted.size()), rp.how().c_str());
		}
		say("TAP SUMMARY equal=%d differ=%d divergence_b=%d excluded=%d async=%d not_serial=%d arm_failures=%d plants_detected=%d plants_missed=%d %s",
			equal, differ, divergenceB, excluded, async, notSerial, armFail, detected, missed, differ == 0 && armFail == 0 && missed == 0 ? "PASS" : "FAIL");
		return differ == 0 && armFail == 0 && missed == 0 ? 0 : 1;
#else
		std::fprintf(stderr, "--strict-tap needs posix_spawn\n");
		return 2;
#endif
	}
	int pass = 0, fail = 0, skip = 0;
	GXRender::Rec::GxTranslateStats pack;
	int uncounted = 0;
	for (const std::string& a : arms) {
		const ArmResult r = runArm(a, opt);
		uncounted += !r.ownCounters;
		pack.packReused += r.pack.packReused;
		pack.packed += r.pack.packed;
		pack.packChecks += r.pack.packChecks;
		pack.packCheckMismatches += r.pack.packCheckMismatches;
		if (r.skipped) {
			say("ARM %-16s SKIP (%s)", r.name.c_str(), r.detail.c_str());
			++skip;
			continue;
		}
		const bool armOk = !r.invalid && (r.expectFail ? r.failed : !r.failed);
		say("ARM %-16s %s (%s)%s", r.name.c_str(), armOk ? "PASS" : "FAIL",
			r.invalid ? "INVALID" : r.expectFail ? (r.failed ? "case broken as planted" : "PLANT NOT DETECTED") : (r.failed ? "BROKEN" : "holds"),
			r.detail.c_str());
		armOk ? ++pass : ++fail;
	}
	say("PACKSTATS reused=%llu packed=%llu checks=%llu (own devices; %d arm(s) released theirs: not in these) mismatches(all devices)=%llu "
		"a25SlowP1=%d a25SlowP3=%d", (unsigned long long)pack.packReused, (unsigned long long)pack.packed,
		(unsigned long long)pack.packChecks, uncounted, (unsigned long long)pack.packCheckMismatches,
		GXRender::Rec::g_recTest.a25SlowP1 ? 1 : 0, GXRender::Rec::g_recTest.a25SlowP3 ? 1 : 0);
	say("SUMMARY arms=%d pass=%d fail=%d skipped=%d validation=%s fif=%u %s", pass + fail, pass, fail, skip,
		opt.validation ? "core+sync" : "off", opt.fif, fail ? "FAIL" : "PASS");
	if (exitobs::armed.load()) {
		// A23-B arm 16's child ends the way the GAME does (SDL3Main: GxNative::FlushFinalStats(), then _exit): the flush, the observer,
		// then _exit - no static destruction. Measured: through a normal exit the test process's static destructors could abort
		// ("mutex lock failed") while a pipeline worker still compiled after the line - a teardown the game never performs.
		GxNativeFlushFinalStatsAtExit();
		exitobs::report();
		std::fflush(stdout);
		std::fflush(stderr);
		_exit(fail ? 1 : 0);
	}
	return fail ? 1 : 0;
}
