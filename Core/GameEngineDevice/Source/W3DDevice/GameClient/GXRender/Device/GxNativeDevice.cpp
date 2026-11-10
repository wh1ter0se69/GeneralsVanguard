// GeneralsX: L1: the native d3d8 device (GxNativeDevice.h).
// DXVK cites are references/fbraz3-dxvk 954c5c692, the CLASSIC path this device must be indistinguishable from.
#include "GxNativeDevice.h"

#include "GxD3D.h"
#include "GxNativeUuid.h"
#include "GxOrderCheck.h"
#include "gx_cs_ps.frag.h"
#include "gx_cs_trees.vert.h"
#include "gx_cs_wave.vert.h"
#include "gx_cs_vel.vert.h"
#include "gx_cs_vel.skin.vert.h"
#include "gx_cs_vel.frag.h"
#include "gx_cs_shadow.vert.h"
#include "gx_ff.frag.h"
#include "gx_ff.vert.h"
#include "gx_ff_flat.frag.h"
// S7 P2 (A18/A36): the key-on receiver family, loaded when the look mask has bit 0
#include "gx_cs_ps.sm.frag.h"
#include "gx_cs_trees.sm.vert.h"
#include "gx_cs_wave.sm.vert.h"
#include "gx_cs_shadow.sm.vert.h"
#include "gx_ff.sm.frag.h"
#include "gx_ff.sm.vert.h"
#include "gx_ff_flat.sm.frag.h"
#include "gx_cs_ps.a2c.frag.h"      // (A108): the alpha-to-coverage family (regen.sh @A2C / @SMA2C)
#include "gx_ff.a2c.frag.h"
#include "gx_ff_flat.a2c.frag.h"
#include "gx_cs_ps.sm.a2c.frag.h"
#include "gx_ff.sm.a2c.frag.h"
#include "gx_ff_flat.sm.a2c.frag.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace GXRender { namespace Dev {

namespace R = GXRender::Res;
namespace V = GXRender::Vk;

std::atomic<int> g_gxNativeLiveDevices { 0 };
std::atomic<GxCsExecSink*> g_csExecSink { nullptr };   // TEST-ONLY (A23-B §7.1): the EXEC digest sink

// {7E1B5F3C-4A2D-4C8E-9B61-3D5E0A9F2C71}
const GUID IID_GxNative = { 0x7e1b5f3c, 0x4a2d, 0x4c8e, { 0x9b, 0x61, 0x3d, 0x5e, 0x0a, 0x9f, 0x2c, 0x71 } };

namespace {

bool isIID(REFIID a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

// Shader handles as DXVK encodes them (d3d8_device.cpp:11-21): bit 0 (D3DFVF_RESERVED0) marks a shader, never an FVF.
DWORD shaderHandle(size_t index) { return DWORD(((index + 1) << 1) | 1u); }
bool isShaderHandle(DWORD h) { return (h & 1u) != 0; }
size_t shaderIndex(DWORD h) { return size_t(h >> 1) - 1; }

// TEST-ONLY (A23-B arm 6w): n bytes copied so that their last byte abuts a PROT_NONE page; a read past them faults. POSIX only
// (elsewhere the bytes are used where they are).
struct TestGuardCopy
{
	void* base = nullptr;
	size_t len = 0;
	const uint8_t* place(const uint8_t* src, size_t n)
	{
#if !defined(_WIN32)
		const size_t page = size_t(sysconf(_SC_PAGESIZE)), body = (n + page - 1) / page * page;
		len = body + page;
		base = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (base == MAP_FAILED) {
			base = nullptr;
			return src;
		}
		uint8_t* end = static_cast<uint8_t*>(base) + body;
		mprotect(end, page, PROT_NONE);
		std::memcpy(end - n, src, n);
		return end - n;
#else
		(void)n;
		return src;
#endif
	}
	~TestGuardCopy()
	{
#if !defined(_WIN32)
		if (base)
			munmap(base, len);
#endif
	}
};

// Lengths in DWORDs (GXCapRecorder.cpp Decl_Len / Func_Len: the same tokens the capture records and the catalogue keys).
size_t declLen(const DWORD* d)
{
	size_t i = 0;
	for (;;) {
		const DWORD t = d[i++];
		if (t == 0xFFFFFFFFu)
			return i;
		if (((t >> 29) & 7) == 4)   // D3DVSD_TOKEN_CONSTMEM
			i += ((t >> 25) & 0xF) * 4;
	}
}

size_t funcLen(const DWORD* f)
{
	size_t i = 1;
	for (;;) {
		const DWORD t = f[i++];
		if (t & 0x80000000u)
			continue;
		if (t == 0x0000FFFFu)
			return i;
		if ((t & 0xFFFF) == 0xFFFE)
			i += (t >> 16) & 0x7FFF;
		else if ((t & 0xFFFF) == 81)
			i += 5;
	}
}


const uint64_t kIdleUploadFlushBytes = 32ull << 20;

// A23-B: the worker's default - ON for the timing series (A23-B: X runs with GX_NATIVE_CS unset; GX_NATIVE_CS=0 is
// today's direct path). The branch MERGES with it ON only after step 1, step 1b, the wh1ter0se's-PC ABBA, the Mini default-camera guard,
// the iGPU + iPad arms and the Windows CRT-exit check (render-integration-5, 13/10). Its ring (A23-B amendment 1 (5):
// 32 MiB until step 1b sizes it; iOS capped at 64 MiB).
constexpr bool kNativeCsDefault = true;

// The Reflex request both FG-era guards act on (the submit-thread guard in initVulkan, the A23-B worker guard): the player's GXReflex,
// or 1 under TEST-ONLY GX_REFLEX_MEASURE_ONLY=1, which loads Reflex for its PCL markers with no GXReflex (GxNativeDlss.cpp, PREREG
// A39e) and then needs the same per-frame Streamline contract.
static int reflexRequested(const GxNativeHost& h)
{
	if (h.requestedReflex > 0)
		return h.requestedReflex;
	const char* m = std::getenv("GX_REFLEX_MEASURE_ONLY");
	return m && std::strcmp(m, "1") == 0 ? 1 : 0;
}
constexpr size_t kNativeCsRingBytes = 32u << 20;

// The census-derived generic FF set (GxGenericSet.inc, scripts/render/dxvk-census/census.py --emit-inc)
const GxDrawPart kGxGenericParts[] = {
#include "GxGenericSet.inc"
};
const size_t kGxGenericPartCount = sizeof(kGxGenericParts) / sizeof(kGxGenericParts[0]);

} // namespace

// ------------------------------------------------------------------------------------------------------------ lifetime

namespace {
std::mutex g_exitLock;
GxNativeDevice* g_exitDevice = nullptr;   // the live device that still owes its final stats line (one per process, DESIGN §3)
}

// atexit: the final line of a device the process never destroyed (end=exit). Also the game's FlushFinalStats before its _exit
// (SDL3Main.cpp), and called by gx_native_test. FIRST it quiesces gx-vk-submit (DESIGN v4 §3.7, A21 am. 4): no Vulkan call
// may run on that thread while the process ends - through exit()'s atexit handlers and static destructors (the Vulkan library's
// were registered when initVulkan loaded it, before init() registered this hook, so they run after it), or through the game's
// window teardown and memory-manager shutdown. The final line then counts every op the thread ran.
// LIMIT of that premise (dlss-integrator 18/10): a FUNCTION-LOCAL static first built after init() - e.g. on a
// renderer thread inside the validation layer's first vkCmdCopyImageToBuffer check - registers its destructor AFTER this hook, so
// exit() destroys it BEFORE the hook parks the worker (LIFO): a worker still recording at that moment uses a destroyed static
// (gx_native_test's child-exit-crt: SIGSEGV in VVL). The game never meets it (it parks, then _exit; the validation layer is debug-only).
namespace {
// A60 (A60 am. 2): UCRT runs every atexit handler under its exit lock (__acrt_exit_lock), and a renderer thread that meets a
// first-time static with a destructor (the validation layer's, a driver's, ours) blocks registering it on that same lock while this hook
// waits for it - a deadlock (Windows, dlss-fg-2: 2 in 22). The quiesce stays complete and
// unbounded: it is what keeps Vulkan calls off the process while exit() destroys the Vulkan library's statics (§3.7). At the CRT exit a
// watchdog thread watches it, and when no command completes and no stage passes for the limit it prints one line and ENDS the process -
// never a return into a CRT that would go on destroying statics under running renderer threads.
class ExitWatchdog
{
public:
	ExitWatchdog(GxNativeDevice* d, bool on) : m_d(d)
	{
		if (!on)
			return;
		const char* e = std::getenv("GX_NATIVE_EXIT_STALL_MS");   // TEST-ONLY: the arms' shorter limit
		const long ms = e ? std::strtol(e, nullptr, 10) : 0;
		m_stallMs = ms > 0 && ms < 600000 ? int(ms) : GxNativeDevice::kExitStallMs;
		m_ignoreProgress = d->dbg().csExitWatchNoProgress;
		try {
			m_t = std::thread([this] { run(); });
		} catch (...) {   // no thread at exit: the quiesce runs unwatched, as before A60 - never an exception out of an atexit handler
		}
	}
	~ExitWatchdog()
	{
		if (!m_t.joinable())
			return;
		{
			std::lock_guard<std::mutex> g(m_m);
			m_done = true;
		}
		m_cv.notify_one();
		m_t.join();
	}
	void stage(int s) { m_stage.store(s, std::memory_order_release); }

private:
	void run()
	{
		// progress = a stage passed or a command completed (done() is the stream's atomic; nothing producer-owned is read here)
		auto sig = [this] { return (uint64_t(m_stage.load(std::memory_order_acquire)) << 56) ^ m_d->csStream().done(); };
		uint64_t seen = sig();
		auto last = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> l(m_m);
		while (!m_done) {
			m_cv.wait_for(l, std::chrono::milliseconds(20));
			if (m_done)
				return;
			const auto now = std::chrono::steady_clock::now();
			const uint64_t s = sig();
			if (s != seen && !m_ignoreProgress) {
				seen = s;
				last = now;
			} else if (now - last >= std::chrono::milliseconds(m_stallMs)) {
				char line[320];   // snprintf takes no FILE lock; the line itself goes out bounded
				std::snprintf(line, sizeof(line), "[GXNATIVE] exit: the renderer's exit quiesce made no progress for %d ms in the CRT exit "
					"handler (stage %d, stream done=%llu) - the process ends here, without the rest of the CRT exit (A60)\n", m_stallMs,
					m_stage.load(std::memory_order_acquire), (unsigned long long)m_d->csStream().done());
				gxWriteStderrBounded(line, 500);
				gxTerminateProcessNow(GxNativeDevice::kExitTerminalCode);
			}
		}
	}
	GxNativeDevice* m_d;
	int m_stallMs = 0;
	bool m_ignoreProgress = false;
	bool m_done = false;
	std::atomic<int> m_stage { 0 };
	std::mutex m_m;
	std::condition_variable m_cv;
	std::thread m_t;
};

// crtExit: called by the CRT's exit (the atexit hook below) rather than explicitly (the game's own shutdown, tests) - A60
void flushFinalStatsAtExit(bool crtExit)
{
	std::lock_guard<std::mutex> g(g_exitLock);
	if (!g_exitDevice)
		return;
	GxNativeDevice* d = g_exitDevice;
	ExitWatchdog dog(d, crtExit && !d->dbg().csExitUnwatched);
	d->noteExitQuiesce();
	dog.stage(1);
	d->csPark();   // A23-B §3.9 / amendment 1 (7): every queued command ran and gx-native-cs is gone BEFORE lever 3's quiesce
	dog.stage(2);
	if (!d->dbg().csExitNoSettle)
		d->settleAtExit();
	dog.stage(3);
	if (!d->dbg().finalAtExitOff)
		d->logFinalStats("exit");
}
void gxNativeAtExitHook() { flushFinalStatsAtExit(true); }
} // namespace

void GxNativeFlushFinalStatsAtExit() { flushFinalStatsAtExit(false); }

GxNativeDevice::GxNativeDevice(IDirect3D8* parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
	const GxNativeHost& host)
	: m_parent(parent), m_adapter(adapter), m_type(type), m_focus(focus), m_behavior(behavior), m_host(host),
	  m_swvp((behavior & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0)
{
	if (m_parent)
		m_parent->AddRef();
	++g_gxNativeLiveDevices;
	for (uint32_t i = 0; i < 256; ++i)   // identity ramp, as a fresh D3D9 swap chain reports
		m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = WORD(i * 257);
}

GxNativeDevice::~GxNativeDevice()
{
	{   // first out of the exit hook's reach (under its lock): a final child release on another thread while the process exits
		// must never park the worker concurrently with the hook
		std::lock_guard<std::mutex> g(g_exitLock);
		if (g_exitDevice == this)
			g_exitDevice = nullptr;
	}
	csPark();   // A23-B: the worker drained and joined BEFORE anything below; the rest runs direct, as today
	if (m_haveSwapchain)
		applyPresentStatus(m_swapchain.takePresentStatus());   // R19: the last present's answer is counted
	logFinalStats("teardown");
	if (m_ctx.initialized()) {
		if (m_writer.frameOpen())
			m_writer.endFrame();
		m_ctx.waitIdle();
		for (const TestShot& t : m_testShots)   // TEST readback buffers (testSwapchainReadback)
			vmaDestroyBuffer(m_gx.allocator(), t.buf, t.alloc);
		m_testShots.clear();
	}
	for (uint32_t i = 0; i < 8; ++i)
		SetTexture(i, nullptr);
	for (uint32_t i = 0; i < 16; ++i)
		SetStreamSource(i, nullptr, 0);
	SetIndices(nullptr, 0);
	bindSurface(m_rt, nullptr);
	bindSurface(m_ds, nullptr);
	worldRelease(true);   // an open world pass's targets (render-motion-1)
	destroySwapchainObjects();
	drainDestroys();
	m_dropCandidates.clear();
	if (m_sceneInit)
		m_scene.shutdown(m_writer);   // disarm, copies and probe - before the writer and GXRES go
	m_sceneInit = false;
	shadowMapShutdown();   // S7: after the device idled above; before the writer that points at its caster list
	if (m_gpuDump) {   // S7 A92
		std::fclose(m_gpuDump);
		m_gpuDump = nullptr;
	}
	m_cache.shutdown();   // first: it joins the spec workers, which compile against the writer's pipeline layout
	m_writer.setGpuTimer(nullptr, 0);
	m_writer.shutdown();
	m_gpuTimer.shutdown();   // its query pool, after the writer's last frame and before the device
	m_present.shutdown();
	m_worldResolve.shutdown();   // render-hud-seam-1 ResolveLinear
	m_fsr1.shutdown();           // render-hud-seam-1 Upscale(FSR1)
	m_motion.shutdown();         // render-motion-1
	dlssShutdown();   // its output image and slShutdown, before GxResources and the device go
	m_motionInit = false;
	if (m_capBuf)
		vmaDestroyBuffer(m_ctx.allocator(), m_capBuf, m_capAlloc);
	m_capBuf = VK_NULL_HANDLE;
	if (m_ctx.initialized()) {
		for (VkShaderModule* m : { &m_ffShaders.vs, &m_ffShaders.fs, &m_ffShaders.fsFlat, &m_csShaders.ps, &m_csShaders.vsTrees,
			     &m_csShaders.vsWave, &m_csShaders.vsVelRigid, &m_csShaders.vsVelSkin, &m_csShaders.psVel, &m_csShaders.vsShadow,
			     &m_ffShaders.fsA2c, &m_ffShaders.fsFlatA2c, &m_csShaders.psA2c })   // the a2c family (null with A2C off)
			if (*m)
				vkDestroyShaderModule(m_ctx.device(), *m, nullptr);
	}
	m_gx.shutdown();
	if (m_surface && m_host.destroySurface)
		m_host.destroySurface(m_ctx.instance(), m_surface);
	// every object is gone under the checker's wrappers; restore volk's pointers before the device goes, or the next device's
	// install() is a no-op while its VkCtx::init overwrites the hooks - its checks silently stop
	if (m_orderCheckInstalled && !m_debug.orderCheckStaysInstalled)
		Rec::GxOrderCheck::uninstall();
	m_ctx.shutdown();
	if (m_parent)
		m_parent->Release();
	--g_gxNativeLiveDevices;
}

ULONG GxNativeDevice::Release()
{
	const ULONG n = --m_ref;
	if (n == 0)
		delete this;
	return n;
}

HRESULT GxNativeDevice::QueryInterface(REFIID riid, void** pp)
{
	if (!pp)
		return E_POINTER;
	if (isIID(riid, IID_GxNative)) {   // no AddRef: an identity query (GxNativeFromDevice)
		*pp = this;
		return S_OK;
	}
	if (isIID(riid, __uuidof(IUnknown)) || isIID(riid, __uuidof(IDirect3DDevice8))) {
		AddRef();
		*pp = static_cast<IDirect3DDevice8*>(this);
		return S_OK;
	}
	if (isIID(riid, IID_GxShadowMap)) {   // (S7): key ON only (PREREG A18) - key off answers as before
		if (!m_shadowMapOn) {
			*pp = nullptr;
			return E_NOINTERFACE;
		}
		m_shadowMapIf.dev = this;
		AddRef();
		*pp = static_cast<IGxShadowMap*>(&m_shadowMapIf);
		return S_OK;
	}
	if (isIID(riid, IID_GxWorldPass)) {   // render-hud-seam-1: the world/HUD seam (GxWorldPass.h)
		m_worldPass.dev = this;
		AddRef();
		*pp = static_cast<IGxWorldPass*>(&m_worldPass);
		return S_OK;
	}
	if (isIID(riid, IID_GxSceneTap) && m_sceneInit) {   // F-depth+ (GxSceneTap.h), seam on or off
		m_sceneTap.dev = this;
		AddRef();
		*pp = static_cast<IGxSceneTap*>(&m_sceneTap);
		return S_OK;
	}
	*pp = nullptr;
	return E_NOINTERFACE;
}

GxNativeDevice* GxNativeFromDevice(IDirect3DDevice8* dev)
{
	void* p = nullptr;
	if (!dev || FAILED(dev->QueryInterface(IID_GxNative, &p)))
		return nullptr;
	return static_cast<GxNativeDevice*>(p);
}

void GxNativeDevice::logLine(const char* fmt, ...)
{
	char buf[2048];   // the stats line with A23-B's cs fields passes 1 KiB
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> g(m_logLock);
	if (m_host.log)
		m_host.log(buf);
	else
		std::fprintf(stderr, "%s\n", buf);
}

HRESULT GxNativeDevice::neverCalled(const char* what)
{
	if (!m_debug.countNothing && m_stats.neverCalled++ == 0)
		logLine("[GXNATIVE] a method the census says the game never calls was called: %s (INVALIDCALL)", what);
	return D3DERR_INVALIDCALL;
}

// A draw the translator refused (named in its stats): counted, the first one logged with the reasons so far (DESIGN §2.3).
void GxNativeDevice::refusedDraw()
{
	if (m_stats.drawsRefused++ != 0)
		return;
	std::string why;
	for (const auto& kv : m_tr.stats().refusedWhy)
		why += (why.empty() ? "" : ", ") + kv.first;
	logLine("[GXNATIVE] REFUSED draw: %s", why.empty() ? "(no reason recorded)" : why.c_str());
}

// DESIGN §2.3: the session's counters, every kStatsEveryPresents presents and once at teardown. bad = the counters any of which
// fails G-3 (refused draws / creates / targets / copies, GXRES errors, present-shader errors); why = the translator's and the
// writer's refusal reasons with their counts.
// DESIGN §2.3: the final line, exactly once per session, by whichever comes first: the device's destruction (end=teardown) or a
// normal process exit with the device still alive (end=exit - the game never released it, or a leaked child still holds it, as
// DXVK's device children do). A run killed by a signal prints neither. integrator, rw3-H KG6: GX_TEST_QUIT_AT without
// :exitprocess quits the MATCH, the process stayed at the score screen until the driver killed it.
void GxNativeDevice::logFinalStats(const char* end)
{
	drainGpuDump();   // S7 A92: teardown and the exit hook both come here, the worker parked
	if (!m_initOk || (m_finalLogged && !m_debug.finalNotOnce))   // a device the session refused never drew: no stats line for it
		return;
	m_finalLogged = true;
	logStats("final", end);
}

// GX_GPUTIME: the frames read since the last line - GPU ms per frame from the start of the frame's command buffer to its end (MoltenVK:
// first encoder start to last encoder end), per frame ring slot. A frame is read 8 frames after it ran; an unavailable one is miss.
// The graphics timeline wait; with `us` (GX_NATIVE_WAITS, A30 [GXWAIT]) its host time is added there - without it no clock
// is read, as before.
void GxNativeDevice::waitTimed(uint64_t value, uint64_t* us)
{
	const auto t0 = us ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	m_ctx.queue(V::QueueKind::Graphics).timeline.wait(value);
	if (us)
		*us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

// S7 A92 TEST-ONLY GX_GPUTIME_DUMP: one line per sample of m_gpuWin, then one per frame counted missing (R1's
// completeness denominator keeps their logic frames)
void GxNativeDevice::writeGpuDump()
{
	if (!m_gpuDump)
		return;
	for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin) {
		if (g.shadow == Rec::GxPacketWriter::ShadowRead)
			std::fprintf(m_gpuDump, "%u %u %.4f %.4f\n", g.logicFrame, g.slot, double(g.ms), double(g.shadowMs));
		else
			std::fprintf(m_gpuDump, "%u %u %.4f %s\n", g.logicFrame, g.slot, double(g.ms),
				g.shadow == Rec::GxPacketWriter::ShadowMissing ? "miss" : "-1");
	}
	for (uint32_t lf : m_gpuMissLogic)
		std::fprintf(m_gpuDump, "%u - miss miss\n", lf);
	std::fflush(m_gpuDump);
}

// the dump's last batch - after the worker is parked (both callers), the GPU idled, every armed set read
void GxNativeDevice::drainGpuDump()
{
	// the terminal settlement runs ONCE (FlushFinalStats, the CRT exit hook and teardown can each come here) - a
	// second drain would append the open frame's missing row again
	if (!m_gpuDump || !m_gpuOn || m_gpuDumpDrained)
		return;
	m_gpuDumpDrained = true;
	m_ctx.waitIdle();
	uint32_t openLogic = 0;
	const bool open = m_writer.drainGpuSets(&openLogic);
	uint32_t miss = 0;
	m_gpuMissLogic.clear();
	m_writer.takeGpuSamples(&m_gpuWin, &miss, &m_gpuMissLogic);
	if (open)   // the frame still open at teardown / exit - never submitted: MISSING, with its logic frame
		m_gpuMissLogic.push_back(openLogic);
	writeGpuDump();
}

void GxNativeDevice::logGpu(uint64_t presents)
{
	uint32_t miss = 0, refused = 0;
	m_gpuMissLogic.clear();
	m_writer.takeGpuSamples(&m_gpuWin, &miss, &m_gpuMissLogic);
	for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin)
		refused += g.refused != 0;   // partial frames (A17 P2)
	auto at = [](std::vector<float>& v, double q) {
		return v.empty() ? 0.0 : double(v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]);
	};
	double sum = 0.0, slotMed[3] = {};
	size_t slotN[3] = {};
	for (uint32_t slot = 0; slot < m_fif; ++slot) {   // A30: every frame slot (s2 only at 3 in flight)
		m_gpuMs.clear();
		for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin)
			if (g.slot == slot)
				m_gpuMs.push_back(g.ms);
		std::sort(m_gpuMs.begin(), m_gpuMs.end());
		slotN[slot] = m_gpuMs.size();
		slotMed[slot] = at(m_gpuMs, 0.5);
	}
	m_gpuMs.clear();
	for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin) {
		m_gpuMs.push_back(g.ms);
		sum += g.ms;
	}
	std::sort(m_gpuMs.begin(), m_gpuMs.end());
	const double med = at(m_gpuMs, 0.5), mean = m_gpuMs.empty() ? 0.0 : sum / double(m_gpuMs.size()), p99 = at(m_gpuMs, 0.99),
		mx = at(m_gpuMs, 1.0);
	// A17: per-frame medians of the render passes summed, the present, the rest (frame - passes - present) and the pass count -
	// over the COMPLETE frames only: a frame with a refused scope has partial parts (its whole-frame ms above stays)
	double part[4] = {};
	size_t complete = 0;
	for (int k = 0; k < 4; ++k) {
		m_gpuMs.clear();
		for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin)
			if (!g.refused)
				m_gpuMs.push_back(k == 0 ? g.passMs : k == 1 ? g.presentMs : k == 2 ? g.ms - g.passMs - g.presentMs : float(g.passes));
		std::sort(m_gpuMs.begin(), m_gpuMs.end());
		part[k] = at(m_gpuMs, 0.5);
		complete = m_gpuMs.size();
	}
	// no complete frame in this line: the parts are UNKNOWN, printed as na - never as 0 ms (A17)
	char parts[128];
	if (complete)
		std::snprintf(parts, sizeof(parts), "pass=%.4f present=%.4f other=%.4f np=%.1f", part[0], part[1], part[2], part[3]);
	else
		std::snprintf(parts, sizeof(parts), "pass=na present=na other=na np=na");
	// the writer's own counters since the last line, per writer frame: why its scopes break
	const Rec::GxWriterStats& w = m_writer.stats();
	const uint64_t now[7] = { w.frames, w.scopes, w.splitUpload, w.splitSample, w.splitOutside, w.splitAttachments, w.handoffs };
	double per[7] = {};
	const double frames = double(now[0] - m_gpuPrev[0]);
	for (int k = 1; k < 7; ++k)
		per[k] = frames > 0 ? double(now[k] - m_gpuPrev[k]) / frames : 0.0;
	std::copy(std::begin(now), std::end(now), std::begin(m_gpuPrev));
	char s2[48] = "";
	if (m_fif == 3)
		std::snprintf(s2, sizeof(s2), " s2=%zu/%.4f", slotN[2], slotMed[2]);
	// S7 A92 (PREREG A27; the integrator's P4 ruling (3)): `frame` spans INIT + MAIN since this slice - the marker lets a parser refuse a
	// pair from either side of it. `shadow` = the caster pass's diagnostic scope over the frames that read one (never summed into med);
	// shadow_miss = frames that recorded a pass but read no shadow pair (refused or unavailable: MISSING, never 0 ms); na = no pass at all
	m_gpuMs.clear();
	uint32_t shadowMiss = 0;
	for (const Rec::GxPacketWriter::GpuSample& g : m_gpuWin) {
		if (g.shadow == Rec::GxPacketWriter::ShadowRead)
			m_gpuMs.push_back(g.shadowMs);
		else if (g.shadow == Rec::GxPacketWriter::ShadowMissing)
			++shadowMiss;
	}
	writeGpuDump();
	std::sort(m_gpuMs.begin(), m_gpuMs.end());
	char shadowF[64];
	if (m_gpuMs.empty())
		std::snprintf(shadowF, sizeof(shadowF), "shadow=na/0");
	else
		std::snprintf(shadowF, sizeof(shadowF), "shadow=%.4f/%zu", at(m_gpuMs, 0.5), m_gpuMs.size());
	logLine("[GXGPU] presents=%llu n=%zu miss=%u med=%.4f mean=%.4f p99=%.4f max=%.4f s0=%zu/%.4f s1=%zu/%.4f%s %s "
		"refused=%u scopes=%.2f up=%.2f samp=%.2f out=%.2f att=%.2f hand=%.2f frame=init+main %s shadow_miss=%u gpu_backwards=%llu",
		(unsigned long long)presents, m_gpuWin.size(), miss, med, mean, p99, mx, slotN[0], slotMed[0], slotN[1], slotMed[1], s2,
		parts, refused, per[1], per[2], per[3], per[4], per[5], per[6], shadowF, shadowMiss,
		(unsigned long long)(m_gpuTimer.backwardsPairs() - m_gpuBackPrev));
	m_gpuBackPrev = m_gpuTimer.backwardsPairs();
}

namespace {
// A23-B §3.1: who writes each counter outside an S body. The front (the caller) writes these ...
void copyFrontStats(GxNativeStats& d, const GxNativeStats& s)
{
	d.liveTextures = s.liveTextures;
	d.liveSurfaces = s.liveSurfaces;
	d.liveBuffers = s.liveBuffers;
	d.creates = s.creates;
	d.createsRefused = s.createsRefused;
	d.draws = s.draws;
	d.clears = s.clears;
	d.presents = s.presents;
	d.dropped = s.dropped;
	d.restores = s.restores;
	d.pinned = s.pinned;
	d.readbacks = s.readbacks;
	d.neverCalled = s.neverCalled;
	d.gammaIgnored = s.gammaIgnored;
	d.copiesRefused = s.copiesRefused;
	d.vsConstantsRefused = s.vsConstantsRefused;   // rw3-R11: SetVertexShaderConstant's refusal answers on the front
	d.targetSizeRefused = s.targetSizeRefused;
	d.surfacesRecreated = s.surfacesRecreated;
	d.swapchainsRecreated = s.swapchainsRecreated;
	d.losable = s.losable;
}
// ... and the worker these (exec halves; S bodies write either set with the worker idle). resourceErrors is an atomic of its own.
void copyWorkerStats(GxNativeStats& d, const GxNativeStats& s)
{
	d.drawsRefused = s.drawsRefused;
	d.targetsRefused = s.targetsRefused;
	d.presentsSkipped = s.presentsSkipped;
	d.acquireNotOk = s.acquireNotOk;
	d.presentErrors = s.presentErrors;
	d.presentFailures = s.presentFailures;
	d.stretchedPresents = s.stretchedPresents;
	d.presentsDirect = s.presentsDirect;   // rw3-W W-a: counted in presentToSwapchain (the worker's Present body)
	d.gpuCopies = s.gpuCopies;
	d.resolves = s.resolves;
	d.uploadFlushes = s.uploadFlushes;
}
} // namespace

void GxNativeDevice::logStats(const char* when, const char* end)
{
	logStatsLine(when, end, nullptr);
}

// f: the front's counters carried by the Present the worker prints this line for (null: the worker is idle or gone - final
// line, S body, direct - and every counter is read live)
void GxNativeDevice::logStatsLine(const char* when, const char* end, const FrontCounters* f)
{
	GxNativeStats s;
	FrontCounters live;
	if (f) {
		copyFrontStats(s, f->s);
		copyWorkerStats(s, m_stats);
	} else {
		s = m_stats;
		live.cs = m_cs.stats();
		live.syncs = m_csSyncs;
		live.createRetries = m_csCreateRetries;
		live.frameBytesMax = m_frameBytesMax;
		syncTopInto(live.syncTop, sizeof(live.syncTop), false);
		syncTopInto(live.drainTop, sizeof(live.drainTop), true);
		f = &live;
	}
	s.resourceErrors = m_resourceErrors.load(std::memory_order_relaxed);
	const GxNativeCsStats& c = f->cs;
	const Rec::GxTranslateStats& t = m_tr.stats();
	const Rec::GxWriterStats& w = m_writer.stats();
	const uint64_t allocFailures = m_allocFailures.load(std::memory_order_relaxed);
	const uint64_t bad = s.drawsRefused + s.createsRefused + s.targetsRefused + s.targetSizeRefused + s.copiesRefused +
		s.resourceErrors + s.presentErrors + w.refused + s.vsConstantsRefused + c.seqErrors + allocFailures;
	std::string why;
	if (s.vsConstantsRefused)   // rw3-R11: in why, so the line's fields stay as every parser knows them
		why = "vs constants past c" + std::to_string(kGxNativeMaxVsConst - 1) + ":" + std::to_string(s.vsConstantsRefused);
	for (const auto& kv : t.refusedWhy)
		why += (why.empty() ? "" : ",") + kv.first + ":" + std::to_string(kv.second);
	for (const auto& kv : w.refusedWhy)
		why += (why.empty() ? "" : ",") + std::string("writer ") + kv.first + ":" + std::to_string(kv.second);
	auto u = [](uint64_t v) { return (unsigned long long)v; };
	logLine("[GXNATIVE] stats %s presents=%llu bad=%llu draws=%llu refused=%llu writer_refused=%llu clears=%llu creates=%llu "
		"creates_refused=%llu targets_refused=%llu set_target_refused=%llu copies_refused=%llu resource_errors=%llu "
		"present_errors=%llu present_failures=%llu presents_skipped=%llu surfaces_recreated=%llu swapchains_recreated=%llu "
		"stretched=%llu never_called=%llu dropped=%llu restores=%llu pinned=%llu readbacks=%llu upload_flushes=%llu "
		"slice_overflows=%llu custom_builds=%llu split_upload=%llu hoisted=%llu submit_thread_ops=%llu wait_fastpath=%llu "
		"cs=%s cs_cmds=%llu cs_bytes=%llu cs_payload_bytes=%llu cs_frame_bytes_max=%llu cs_syncs=%llu cs_sync_top=%s cs_drain_top=%s "
		"cs_full_waits=%llu cs_full_ns=%llu cs_lead_waits=%llu cs_lead_ns=%llu cs_drain_ns=%llu cs_present_recovery=%llu "
		"cs_create_retries=%llu cs_wakes=%llu cs_prod_wakes=%llu cs_busy_ns=%llu cs_seq_errors=%llu cs_recoveries=%llu alloc_failures=%llu "
		"dyn_slice_bytes=%llu "
		"present_direct=%llu bb=%ux%u swapchain=%ux%u%s%s why=%s",
		when, u(s.presents), u(bad), u(s.draws), u(s.drawsRefused), u(w.refused), u(s.clears), u(s.creates), u(s.createsRefused),
		u(s.targetsRefused), u(s.targetSizeRefused), u(s.copiesRefused), u(s.resourceErrors), u(s.presentErrors),
		u(s.presentFailures), u(s.presentsSkipped), u(s.surfacesRecreated), u(s.swapchainsRecreated), u(s.stretchedPresents),
		u(s.neverCalled), u(s.dropped), u(s.restores), u(s.pinned), u(s.readbacks), u(s.uploadFlushes),
		u(m_gx.stats().sliceOverflows), u(t.customBuilds), u(w.splitUpload), u(w.hoistedUploads),
		u(m_ctx.threadStats().submits + m_ctx.threadStats().driverPresents), u(m_ctx.threadStats().waitFastPath),
		m_cs.running() ? "on" : "off", u(c.cmds), u(c.bytes), u(c.payloadBytes), u(f->frameBytesMax), u(f->syncs),
		f->syncTop[0] ? f->syncTop : "-", f->drainTop[0] ? f->drainTop : "-", u(c.fullWaits), u(c.fullNs), u(c.leadWaits), u(c.leadNs), u(c.drainNs),
		u(m_csPresentRecovery), u(f->createRetries), u(c.wakes), u(c.producerWakes), u(c.busyNs), u(c.seqErrors), u(m_recoveries.load()), u(allocFailures),
		u(m_gx.dynSliceBytes.load()),
		u(s.presentsDirect), m_pp.BackBufferWidth, m_pp.BackBufferHeight,
		m_haveSwapchain ? m_swapchain.extent().width : 0u, m_haveSwapchain ? m_swapchain.extent().height : 0u, end ? " end=" : "",
		end ? end : "", why.empty() ? "-" : why.c_str());
}

void GxNativeDevice::refuseCreate(const char* what)
{
	if (m_stats.createsRefused++ < 16)
		logLine("[GXNATIVE] REFUSED create: %s", what);
}

GxNativeValidation GxNativeDevice::validation()
{
	csSync("accessor validation");
	GxNativeValidation v;
	const V::ValidationCounters c = m_ctx.validation();
	v.errors = c.errors;
	v.hazards = c.syncHazards;
	v.first = c.firstError;
	return v;
}

// ------------------------------------------------------------------------------------------------------------ init

// VkCtx's / GXRES's log lines: printed as before (host.log or stderr), and a GXRES ERROR counted - GXRES reports a failed operation
// (a staging or upload failure under memory pressure) only by this line while the d3d8 call returns D3D_OK, so the count is what
// makes it visible to the game's stats and to gxcap-replay --native (ask; gxcap-replay-vk counts the same).
// Called on the device's thread for GXRES (its calls are device calls).
void GxNativeDevice::vkLog(V::LogLevel level, const char* line, void* user)
{
	GxNativeDevice* d = static_cast<GxNativeDevice*>(user);
	if (level == V::LogLevel::Error && std::strncmp(line, "[GXRES]", 7) == 0 && !d->m_debug.resourceErrorsUncounted &&
		!(d->m_debug.csFailureSilent && d->m_cs.onWorker()))
		++d->m_resourceErrors;   // any thread: the front's creations log through here too (A23-B §3.1)
	std::lock_guard<std::mutex> g(d->m_logLock);
	if (d->m_host.log)
		d->m_host.log(line);
	else
		std::fprintf(stderr, "%s\n", line);
}

// TEST: the FINAL swapchain image, not the intermediate present target. One buffer per extent,
// rewritten by every shown present; the reader drains the worker and idles the device first.
VkBuffer GxNativeDevice::testShotBuffer()
{
	const VkExtent2D ext = m_swapchain.extent();
	if (m_testShots.empty() || m_testShots.back().w != ext.width || m_testShots.back().h != ext.height) {
		TestShot t;
		t.w = ext.width;
		t.h = ext.height;
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = VkDeviceSize(t.w) * t.h * 4;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (V::gxCreateBuffer(m_gx.allocator(), nullptr, "test-shot", &bi, &ai, &t.buf, &t.alloc, &info) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		t.ptr = info.pMappedData;
		m_testShots.push_back(t);
	}
	return m_testShots.back().buf;
}

bool GxNativeDevice::testReadSwapchain(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height, bool* bgra)
{
	csSync("test read-back");
	m_ctx.waitIdle();
	if (m_testShots.empty() || !m_testShots.back().ptr)
		return false;
	const TestShot& t = m_testShots.back();
	vmaInvalidateAllocation(m_gx.allocator(), t.alloc, 0, VK_WHOLE_SIZE);
	const uint8_t* p = static_cast<const uint8_t*>(t.ptr);
	out->assign(p, p + size_t(t.w) * t.h * 4);
	*width = t.w;
	*height = t.h;
	*bgra = m_swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM || m_swapchain.format() == VK_FORMAT_B8G8R8A8_SRGB;
	return true;
}

bool GxNativeDevice::testReadPresent(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height)
{
	// A23-B §3.1: an inspection accessor - S (the worker wrote m_capW / m_capH in its Present). an S body, not a bare
	// csSync - endFrameForHost below is an ownership boundary, and a drain alone (m_inline 0) counted it as a front recording at CS1
	std::optional<SScope> sc;
	if (m_debug.accessorBareSync)
		csSync("test read-back");   // (the plant: X-own-present-read)
	else
		sc.emplace(*this, "test read-back");
	if (!m_capBuf || !m_capW || !m_capH)
		return false;
	endFrameForHost();
	m_ctx.waitIdle();   // the copy rode in the last Present's submission; drains the submit thread first (lever 3: it may still hold that submission)
	vmaInvalidateAllocation(m_ctx.allocator(), m_capAlloc, 0, VK_WHOLE_SIZE);
	*width = m_capW;
	*height = m_capH;
	const uint8_t* p = static_cast<const uint8_t*>(m_capPtr);
	out->assign(p, p + size_t(m_capW) * m_capH * 4);
	return true;
}

// TESTS: a host-visible buffer for one swapchain image (4 bytes per pixel: VkSwapchain admits only 8-bit RGBA layouts), made before
// the acquire like the present target (nothing that can fail sits between an acquire and its submit).
bool GxNativeDevice::ensureCapture()
{
	const VkExtent2D e = m_swapchain.extent();
	const VkDeviceSize need = VkDeviceSize(e.width) * e.height * 4;
	if (m_capBuf && m_capSize >= need)
		return true;
	if (m_capBuf)
		vmaDestroyBuffer(m_ctx.allocator(), m_capBuf, m_capAlloc);
	m_capBuf = VK_NULL_HANDLE;
	m_capW = m_capH = 0;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = need;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo info = {};
	// A23-B §3.8: through gxCreateBuffer like every Device/ allocation (the render-seam lint's RAWVMA rule); no policy = the raw
	// call, as testShotBuffer's (a TEST buffer is never recovered nor counted)
	if (V::gxCreateBuffer(m_ctx.allocator(), nullptr, "test-capture", &bi, &ai, &m_capBuf, &m_capAlloc, &info) != VK_SUCCESS) {
		m_capBuf = VK_NULL_HANDLE;
		return false;
	}
	m_capPtr = info.pMappedData;
	m_capSize = need;
	return true;
}

bool GxNativeDevice::initVulkan(std::string* why)
{
	V::VkCtxDesc d;
	d.getInstanceProcAddr = dlssLoaderRoot();   // m_host's, or with Streamline its slShutdown-first root
	d.instanceExtensions = m_host.instanceExtensions;
	if (m_host.createSurface)
		d.deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	if (m_host.presentSupport)
		d.presentSupport = m_host.presentSupport;
	d.deviceSelect = m_host.deviceSelect;
	d.validation = m_host.validation;
	d.syncValidation = true;
	d.log = vkLog;
	d.logUser = this;
	d.submitThread = V::SubmitThreadMode::On;   // R19 (rw3-V): gx-vk-submit owns the queue; GX_VK_SUBMIT_THREAD=0 turns it off
	if ((m_host.requestedFrameGen > 0 || reflexRequested(m_host) > 0 || m_host.streamline) && !m_host.testNoFgThreadGuard) {
		// FG P0 §2: its PCL present markers sit around presentToSwapchain on the producer thread; the thread stays off until the
		// thread=1 census. VkCtx lets Off beat an explicit GX_VK_SUBMIT_THREAD=1.
		// (dlss-integrator's ruling 18/10, seam2 design): ANY loaded Streamline too - DLSS SR alone and
		// GX_STREAMLINE_FORCE_LOAD=1 included. Streamline 2.14.1's sl.common plugin hooks Vulkan present (presentCommon: the non-atomic
		// ctx.currentFrame, garbage collection); on gx-vk-submit those hooks ran unordered with slEvaluateFeature (documented
		// non-thread-safe) on the producer. With the thread off, present and evaluate are on one thread, in stream order.
		d.submitThread = V::SubmitThreadMode::Off;
		char off[192];
		std::snprintf(off, sizeof(off), "[GXVK] submit thread OFF: FrameGen/Reflex requested (FG P0 §2, until the thread=1 census); framegen=%d "
			"reflex=%d streamline=%d", m_host.requestedFrameGen, reflexRequested(m_host), m_host.streamline ? 1 : 0);
		vkLog(V::LogLevel::Info, off, this);
	}
	if (!m_ctx.init(d, why))
		return false;
	// FG P0 §2: the request and the outcome, unconditionally - one line proves both inputs (the player's GXFrameGen / GXReflex) and the result.
	{
		char req[128];
		std::snprintf(req, sizeof(req), "[GXVK] fg request framegen=%d reflex=%d submitThread=%s streamline=%d", m_host.requestedFrameGen,
			reflexRequested(m_host), m_ctx.submitThreaded() ? "on" : "off", m_host.streamline ? 1 : 0);
		vkLog(V::LogLevel::Info, req, this);
	}
	{
		VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		p2.pNext = &id;
		vkGetPhysicalDeviceProperties2(m_ctx.physicalDevice(), &p2);
		std::memcpy(m_uuid, id.deviceUUID, VK_UUID_SIZE);
		if (m_host.requireDeviceUuid && std::memcmp(m_uuid, m_host.requireDeviceUuid, VK_UUID_SIZE) != 0) {
			auto hex = [](const uint8_t* u) {
				char b[2 * VK_UUID_SIZE + 1];
				for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
					std::snprintf(b + 2 * i, 3, "%02x", u[i]);
				return std::string(b);
			};
			*why = "uuid mismatch: device " + hex(m_uuid) + " != adapter " + hex(m_host.requireDeviceUuid);
			return false;
		}
	}
	if (m_host.orderCheck) {   // before any pipeline, view or command buffer exists
		if (!Rec::GxOrderCheck::install()) {
			*why = "GxOrderCheck could not install";
			return false;
		}
		m_orderCheckInstalled = true;
	}
	if (m_host.gpuTime) {   // GX_GPUTIME: validate() submits ~31 probes - before GxResources tags any staging for "the next submission"
		V::VkGpuTimer::Report r;
		// S7 A101: the writer resets each query set on the HOST (VkGpuTimer::beginFrameHost) - no hostQueryReset, no timer
		const bool hostReset = m_ctx.features().hostQueryReset;
		const bool up = hostReset && m_gpuTimer.init(m_ctx, kGpuSets, kGpuScopes);
		const V::VkGpuTimer::Provenance p = up ? m_gpuTimer.validate(&r) : V::VkGpuTimer::Provenance::Unsupported;
		logLine("[GXGPU] provenance=%s sets=%u %s", V::VkGpuTimer::name(p), kGpuSets,
			up ? r.line.c_str() : hostReset ? "init failed" : "no hostQueryReset (the query sets are reset on the host, S7 A101)");
		m_gpuOn = p == V::VkGpuTimer::Provenance::Validated;   // no number is ever printed from a timer that did not validate
		// S7 A92 TEST-ONLY: GX_GPUTIME_DUMP=<file> - every per-frame sample, with its logic frame (P4's D2 / T2 are computed from these)
		if (const char* dp = std::getenv("GX_GPUTIME_DUMP"))
			if (m_gpuOn && dp[0] && (m_gpuDump = std::fopen(dp, "w")) != nullptr)
				logLine("[GXGPU] TEST-ONLY dump=%s (logic slot ms shadow_ms|-1|miss; a missing frame: logic - miss miss)", dp);
	}
	Res::FormatCaps caps = m_host.testFormatCaps ? *m_host.testFormatCaps : Res::FormatCaps();
	caps.rgba16f = m_host.motionUnits;   // A35: the motion bridge's O format exists only with its key on
	VkDeviceSize staging = 64ull << 20;   // GX_NATIVE_STAGING_KIB: TEST only (A23-B arms 28, 29 force a dedicated staging buffer)
	if (const char* k = std::getenv("GX_NATIVE_STAGING_KIB"))
		if (const unsigned long v = std::strtoul(k, nullptr, 10))
			staging = VkDeviceSize(v) << 10;
	if (!m_gx.init(m_ctx, staging, why, caps))
		return false;
	m_gx.setAllocPolicy(&m_allocPolicy);   // A23-B §3.8 (the staging ring's dedicated overflow included)
	// A30 step 2: frames in flight, ONE value for the writer here and the swapchain's slots (buildOnSurface's frames)
	// FG P0 (dlss-integrator + render-integration-5, 14/10): while FrameGen / Reflex is requested, frames in flight stay 2 and the line says
	// so, until a census runs at FIF 3 - the rule that keeps gx-vk-submit off (the knob's 3 is refused then, like any other value)
	const bool fgFif = (m_host.requestedFrameGen > 0 || reflexRequested(m_host) > 0) && !m_host.testNoFifGuard;
	m_fif = m_host.framesInFlight == 3 && !fgFif ? 3u : 2u;
	if (m_host.framesInFlight == 3 && fgFif)
		logLine("[GXNATIVE] frames_in_flight=2 forced: FrameGen/Reflex requested (FG P0, until the FIF 3 census)");
	else if (m_host.framesInFlight != m_fif)
		logLine("[GXNATIVE] GX_NATIVE_FIF refused (2 or 3 only): 2 kept");
	logLine("[GXNATIVE] frames_in_flight=%u", m_fif);
	// S7 P2 (A36 item 5): the atlas exists BEFORE the writer, whose layout gains set 2 (and whose modules sample it) only when it does;
	// a failure gives a key-off session exactly (no set 2, today's modules, IID_GxShadowMap unanswered) and says why
	m_lookFeatures = m_host.lookFeatures;
	if (m_lookFeatures & 1u) {
		std::string sw;
		if (!shadowMapCreate(&sw)) {
			shadowMapShutdown();
			if (!m_host.testShadowMapFailKeepsBit)
				m_lookFeatures &= ~1u;
			logLine("[GXSHADOWMAP] state=off reason=\"%s\" (the game keeps today's stencil shadows)", sw.c_str());
		}
	}
	// (A108): bit 1 (GXAlphaToCoverage) acts only on a multisampled creation - else it is cleared here and the session is today's
	if (m_lookFeatures & 2u) {
		m_a2cOn = m_createSamples >= 2;
		if (!m_a2cOn) {
			m_lookFeatures &= ~2u;
			logLine("[GXA2C] state=off samples=%u reason=\"no MSAA at creation (the key acts at 2x and above)\"", m_createSamples);
		}   // on: init() prints the state line with the GRANTED count once the back buffer exists
	}
	Rec::GxWriterDesc wd;
	wd.framesInFlight = m_host.testFifWriterStays2 ? 2u : m_fif;   // the plant (TESTS): the writer ignores the knob
	wd.lookFeatures = m_lookFeatures & 1u;   // the writer's layout reads bit 0 only (set 2); A2C changes no layout
	wd.vsBlockBytes = sizeof(GxFFVsBlock);
	wd.psBlockBytes = sizeof(GxFFPsBlock);
	wd.streamBytes = 64ull << 20;   // gxcap-replay-vk's size: the FF blocks of a 52k-draw G2 frame fit
	wd.check = m_host.orderCheck;
	if (!m_writer.init(m_ctx, m_gx, wd, why))
		return false;
	m_writer.setAllocPolicy(&m_allocPolicy);
	if (m_gpuOn)
		m_writer.setGpuTimer(&m_gpuTimer, kGpuSets);
	dlssInit();   // a no-op without Streamline (m_host.streamline null)
	{   // F-depth+ - nothing is made until a reader asks, except the TEST-ONLY probe's pipeline
		uint32_t probe = m_host.fdepthProbe;
		if (const char* e = std::getenv("GX_FDEPTH_PROBE"))
			probe |= !std::strcmp(e, "depth") ? GX_SCENE_DEPTH : !std::strcmp(e, "colour") ? GX_SCENE_COLOUR :
				!std::strcmp(e, "both") ? (GX_SCENE_DEPTH | GX_SCENE_COLOUR) : 0u;
		const char* rp = std::getenv("GX_FDEPTH_RESOLVE");
		const bool forcePass = m_host.fdepthForcePass || (rp && !std::strcmp(rp, "pass"));
		std::string fwhy;
		m_sceneInit = m_scene.init(m_ctx, m_gx, probe, forcePass, &fwhy);
		if (!m_sceneInit) {
			m_scene.shutdown(m_writer);   // whatever the probe made before it failed
			logLine("[GXFDEPTH] off for the session: %s", fwhy.c_str());
		}
		else if (probe || forcePass)
			logLine("[GXFDEPTH] TEST-ONLY probe=%s%s resolve=%s", (probe & GX_SCENE_DEPTH) ? "d" : "", (probe & GX_SCENE_COLOUR) ? "c" : "",
				forcePass ? "pass" : "in-scope");
	}
	auto module = [&](const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(m_ctx.device(), &ci, nullptr, &m);
		return m;
	};
	// S7 P2: the family is chosen ONCE here from the effective mask (A18/A20 b): key off = today's modules byte for byte
	const bool sm = (m_lookFeatures & 1u) != 0;
	m_ffShaders.vs = sm ? module(spv_gx_ff_sm_vert, sizeof(spv_gx_ff_sm_vert)) : module(spv_gx_ff_vert, sizeof(spv_gx_ff_vert));
	m_ffShaders.fs = sm ? module(spv_gx_ff_sm_frag, sizeof(spv_gx_ff_sm_frag)) : module(spv_gx_ff_frag, sizeof(spv_gx_ff_frag));
	m_ffShaders.fsFlat = sm ? module(spv_gx_ff_flat_sm_frag, sizeof(spv_gx_ff_flat_sm_frag)) :
		module(spv_gx_ff_flat_frag, sizeof(spv_gx_ff_flat_frag));
	m_ffShaders.layout = m_writer.layout().pipeline;
	m_csShaders.ps = sm ? module(spv_gx_cs_ps_sm_frag, sizeof(spv_gx_cs_ps_sm_frag)) : module(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag));
	// (A108; the integrator's ruling after): today's fragment modules above serve every plain key; with
	// A2C on the a2c family is made IN ADDITION, and the cache uses it for an alphaToCoverage = 1 key only (vertex shaders are the same
	// either way). A module that cannot be made turns A2C off for the session - never the device.
	if (m_a2cOn) {
		m_ffShaders.fsA2c = sm ? module(spv_gx_ff_sm_a2c_frag, sizeof(spv_gx_ff_sm_a2c_frag)) : module(spv_gx_ff_a2c_frag, sizeof(spv_gx_ff_a2c_frag));
		m_ffShaders.fsFlatA2c = sm ? module(spv_gx_ff_flat_sm_a2c_frag, sizeof(spv_gx_ff_flat_sm_a2c_frag)) :
			module(spv_gx_ff_flat_a2c_frag, sizeof(spv_gx_ff_flat_a2c_frag));
		m_csShaders.psA2c = sm ? module(spv_gx_cs_ps_sm_a2c_frag, sizeof(spv_gx_cs_ps_sm_a2c_frag)) :
			module(spv_gx_cs_ps_a2c_frag, sizeof(spv_gx_cs_ps_a2c_frag));
		if (!m_ffShaders.fsA2c || !m_ffShaders.fsFlatA2c || !m_csShaders.psA2c) {
			for (VkShaderModule* m : { &m_ffShaders.fsA2c, &m_ffShaders.fsFlatA2c, &m_csShaders.psA2c }) {
				if (*m)
					vkDestroyShaderModule(m_ctx.device(), *m, nullptr);
				*m = VK_NULL_HANDLE;
			}
			m_a2cOn = false;   // the cache and the translator below are told the session's state (off)
			m_lookFeatures &= ~2u;
			++m_a2cOffCount;
			logLine("[GXA2C] state=off reason=modules at=init (the a2c shader family could not be made: today's modules and path)");
		}
	}
	m_csShaders.vsTrees = sm ? module(spv_gx_cs_trees_sm_vert, sizeof(spv_gx_cs_trees_sm_vert)) :
		module(spv_gx_cs_trees_vert, sizeof(spv_gx_cs_trees_vert));
	m_csShaders.vsWave = sm ? module(spv_gx_cs_wave_sm_vert, sizeof(spv_gx_cs_wave_sm_vert)) :
		module(spv_gx_cs_wave_vert, sizeof(spv_gx_cs_wave_vert));
	m_csShaders.vsShadow = sm ? module(spv_gx_cs_shadow_sm_vert, sizeof(spv_gx_cs_shadow_sm_vert)) :   // GX_SHADOW_GPU
		module(spv_gx_cs_shadow_vert, sizeof(spv_gx_cs_shadow_vert));
	if (!m_ffShaders.vs || !m_ffShaders.fs || !m_ffShaders.fsFlat || !m_csShaders.ps || !m_csShaders.vsTrees || !m_csShaders.vsWave ||
		!m_csShaders.vsShadow) {
		*why = "shader module creation failed";
		return false;
	}
	if (m_host.motionUnits) {   // A35: key off creates none of them
		m_csShaders.vsVelRigid = module(spv_gx_cs_vel_vert, sizeof(spv_gx_cs_vel_vert));
		m_csShaders.vsVelSkin = module(spv_gx_cs_vel_skin_vert, sizeof(spv_gx_cs_vel_skin_vert));
		m_csShaders.psVel = module(spv_gx_cs_vel_frag, sizeof(spv_gx_cs_vel_frag));
		if (!m_csShaders.vsVelRigid || !m_csShaders.vsVelSkin || !m_csShaders.psVel) {
			*why = "motion-units shader module creation failed";
			return false;
		}
	}
	if (!m_cache.init(m_ctx, m_ffShaders, m_host.pipelineCacheDir ? m_host.pipelineCacheDir : "", 0, why)) {
		*why = "pipeline set: " + *why;   // S10 = the pipeline cache + the generic prebuild (rw3-S classifies by this prefix)
		return false;
	}
	m_cache.setCustomShaders(m_csShaders);
	m_cache.setA2CTwins(m_a2cOn);   // (A108): prebuild() adds the A2C twin of every eligible key (no first-use compile)
	m_writer.setPipelineCache(&m_cache);
	// S7 (PREREG A18): the shadow map (created above, before the writer) goes live only with the effective mask
	if (m_lookFeatures & 1u)
		shadowMapAttach();
	// MoltenVK reports no triangle fans (portability subset); the game draws none (census) - a fan is refused by name.
	m_tr.init(m_ctx, m_writer, m_cache, !m_ctx.features().portabilitySubset);
	m_tr.setBuildCustomOnMiss(true);
	m_tr.setAnisotropyOverride(m_host.samplerAnisotropy);
	m_tr.setReceivers((m_lookFeatures & 1u) != 0);   // S7 P2: DrawBits per draw (key off: the word is never written)
	m_tr.setAlphaToCoverage(m_a2cOn);   // (A108): key off = no draw examined
	return true;
}

bool GxNativeDevice::createSwapchainObjects(const D3DPRESENT_PARAMETERS& pp, std::string* why)
{
	m_pp = pp;
	HWND window = pp.hDeviceWindow ? pp.hDeviceWindow : m_focus;
	if (m_host.createSurface && window) {
		// a lost surface (also one whose replacement failed: m_surface is null then) is made by buildOnSurface's replaceSurface,
		// once (it was made here and replaced again at once)
		if (!m_surface && !m_surfaceLost && !m_host.createSurface(window, m_ctx.instance(), &m_surface)) {
			*why = "window surface creation failed";
			return false;
		}
		uint32_t w = pp.BackBufferWidth, h = pp.BackBufferHeight;
		if (m_host.drawableSize)
			m_host.drawableSize(window, &w, &h);
		// the d3d8 interval (d3d8_d3d9_util.h:108-124): windowed = IMMEDIATE unless COPY_VSYNC (= ONE). `fullscreen` only picks
		// VkSwapchain's mode (IMMEDIATE > MAILBOX > FIFO vs FIFO), so it carries "interval >= 1".
		// dxvk.conf d3d9.presentInterval >= 0 replaces the interval (d3d9_swapchain.cpp:139-140)
		const bool fifo = m_host.presentInterval >= 0 ? m_host.presentInterval > 0 : (!pp.Windowed || pp.SwapEffect == D3DSWAPEFFECT_COPY_VSYNC);
		const uint32_t frames = m_fif;   // A30: the writer's count (was V::SwapchainDesc()'s default 2)
		// a Reset after a SURFACE_LOST that no Present has recovered yet replaces the surface too
		if (!buildOnSurface(w, h, fifo, frames, why))
			return false;
		m_haveSwapchain = true;
		m_presentWaits.assign(frames, 0);
		m_presentSlot = 0;
	}
	// DXVK's buffer count (d3d9_swapchain.cpp:1002-1012 with :248-258; COPY_VSYNC is COPY in d3d9, d3d8_d3d9_util.h:117-119)
	const bool swapFront = pp.SwapEffect != D3DSWAPEFFECT_COPY && pp.SwapEffect != D3DSWAPEFFECT_COPY_VSYNC &&
		!(pp.SwapEffect == D3DSWAPEFFECT_DISCARD && pp.BackBufferCount <= 1 && pp.Windowed);
	const uint32_t count = std::max<uint32_t>(1u, pp.BackBufferCount) + (swapFront ? 1u : 0u);
	for (uint32_t i = 0; i < count; ++i) {
		if (m_debug.failNextBackBuffer) {   // TEST fault injection: the back buffer cannot be allocated (after the swapchain exists)
			m_debug.failNextBackBuffer = false;
			*why = "(test) back buffer allocation failed";
			return false;
		}
		auto rt = std::make_unique<R::GxRenderTarget>();
		R::RenderTargetDesc rd;
		rd.width = pp.BackBufferWidth;
		rd.height = pp.BackBufferHeight;
		rd.format = pp.BackBufferFormat;
		rd.samples = pp.MultiSampleType;
		rd.sampled = true;   // the present reads an MSAA back buffer's samples (DXVK's linear-light resolve, GxNativePresent)
		if (!rt->init(m_gx, rd, why))
			return false;
		m_bbStore.push_back(std::move(rt));
	}
	for (uint32_t i = 0; i < std::max<uint32_t>(1u, pp.BackBufferCount); ++i) {
		GxNativeSurface* bb = new GxNativeSurface(*this, i);
		bb->addPrivateRef();   // the device's (DXVK Com<D3D8Surface, false>)
		m_bbSurfaces.push_back(bb);
	}
	m_rot = 0;
	if (m_haveSwapchain && m_presentWaits.size() && !m_presentInit) {
		if (!m_present.init(m_ctx, uint32_t(m_presentWaits.size()), why))
			return false;
		m_presentInit = true;
	}
	// the present pipelines this back buffer can need, built now: a failure refuses the session at creation (S9) instead of a
	// present that cannot draw
	if (m_haveSwapchain && !m_present.prepare(m_bbStore[0]->image().format->vk, m_bbStore[0]->samples(), why))
		return false;
	if (pp.EnableAutoDepthStencil) {
		auto ds = std::make_unique<R::GxRenderTarget>();
		R::RenderTargetDesc rd;
		rd.width = pp.BackBufferWidth;
		rd.height = pp.BackBufferHeight;
		rd.format = pp.AutoDepthStencilFormat;
		rd.samples = pp.MultiSampleType;
		rd.depthSampled = m_debug.motionSampledAlways;   // the auto depth is never the world's; plant only
		if (!ds->init(m_gx, rd, why))
			return false;
		m_autoDepth = new GxNativeSurface(*this, GxNativeSurface::Kind::Depth, std::move(ds), pp.AutoDepthStencilFormat,
			D3DUSAGE_DEPTHSTENCIL);
		m_autoDepth->addPrivateRef();
	}
	bindSurface(m_rt, m_bbSurfaces[0]);
	bindSurface(m_ds, m_autoDepth);
	syncTarget();
	GxD3D8Viewport vp = { 0, 0, pp.BackBufferWidth, pp.BackBufferHeight, 0.f, 1.f };
	m_tr.viewport(vp);
	return true;
}

void GxNativeDevice::destroySwapchainObjects()
{
	bindSurface(m_rt, nullptr);
	bindSurface(m_ds, nullptr);
	if (m_autoDepth) {
		m_autoDepth->releasePrivateRef();   // the device's own reference
		m_autoDepth = nullptr;
	}
	for (uint32_t i = 0; i < m_bbSurfaces.size(); ++i)   // still held by the caller: it keeps the storage it shows
		if (m_bbSurfaces[i]->publicCount() > 0 && !m_debug.resetDropsRetainedStorage && !m_bbStore.empty())
			m_bbSurfaces[i]->detachStorage(std::move(m_bbStore[(i + m_rot) % m_bbStore.size()]));
	for (GxNativeSurface* bb : m_bbSurfaces)
		bb->releasePrivateRef();
	m_bbSurfaces.clear();
	for (auto& rt : m_bbStore)
		if (rt)
			forgetImage(&rt->image());
	m_bbStore.clear();
	if (m_presentImg) {
		forgetImage(&m_presentImg->image());
		m_presentImg.reset();
	}
	if (m_haveSwapchain) {
		m_swapchain.shutdown();
		m_haveSwapchain = false;
	}
}

bool GxNativeDevice::init(const D3DPRESENT_PARAMETERS& pp, std::string* why)
{
	m_createSamples = std::max<uint32_t>(1u, uint32_t(pp.MultiSampleType));   // initVulkan's A2C family choice
	if (!initVulkan(why) || !createSwapchainObjects(pp, why))
		return false;
	m_tr.renderState(D3DRS_ZENABLE, pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE);   // d3d9 ResetState at creation
	if (m_host.prebuildGenericSet) {   // DESIGN §2 S9: the generic FF set for this session's attachments, before any frame
		std::vector<GxAttachConfig> configs;
		if (!GxSessionAttachConfigs(m_ctx, pp.BackBufferFormat, uint8_t(std::max<uint32_t>(1u, pp.MultiSampleType)), configs, why)) {
			*why = "pipeline set: " + *why;   // every S10 failure carries this prefix (the proxy classifies by it)
			return false;
		}
		m_cache.prebuild(kGxGenericParts, kGxGenericPartCount, configs.data(), configs.size(), 2);
		m_cache.waitPrebuild();
		// (the integrator's ruling after): A2C only ever turns ITSELF off. A prewarm that failed with A2C
		// on (an A2C twin, or a plain key) switches A2C off for the session and runs today's prebuild: the failed keys are dropped, so a
		// plain key the first pass failed gets today's one attempt here. Only that plain-only closure can fail init - the parent's failure.
		// Key off: this branch is never taken (one prebuild, as today).
		if (m_a2cOn && !m_cache.prebuildComplete() && !m_host.testA2cNoFallback) {
			a2cOff("prewarm-failed", "init");
			m_cache.prebuild(kGxGenericParts, kGxGenericPartCount, configs.data(), configs.size(), 2);
			m_cache.waitPrebuild();
		}
		// the GRANTED count the configurations carry (8 asked on the Mini = 4 granted), what keys are made at
		m_a2cPrewarmedSamples = 1;
		for (const GxAttachConfig& c : configs)
			m_a2cPrewarmedSamples = std::max<uint32_t>(m_a2cPrewarmedSamples, c.samples);
		if (!m_cache.prebuildComplete()) {
			*why = "pipeline set: the generic FF prebuild did not complete";
			return false;
		}
	}
	if (m_a2cOn)   // the state line names the back buffer's GRANTED samples (and the request)
		logLine("[GXA2C] state=on samples=%u requested=%u", m_bbStore.empty() ? 0u : uint32_t(m_bbStore.front()->image().samples),
			m_createSamples);
	m_initOk = true;
	// A23-B: the getter mirror from the translator's state as creation left it, then the worker (GX_NATIVE_CS=0: none - today's
	// path). A worker that cannot start leaves the device direct, logged once (the game ring's rule, dx8wrapper.cpp).
	reseedMirror();
	m_wRt = m_rt;
	m_wDs = m_ds;
	resetPressureBound();   // §3.6 seed: GxPacketWriter::init staged its dummy texture
	{
		const char* e = std::getenv("GX_NATIVE_CS");
		bool want = e ? std::strcmp(e, "0") != 0 : kNativeCsDefault;
		// render/dlss x A23-B (the forward merge of render/integration c14f14881): the world/HUD seam (GXUpscaler not off),
		// motion units, DLSS and Streamline's per-frame Reflex / frame-generation calls record from the CALLER through device entry
		// points that A23-B never routed through the worker (WorldPass, worldBegin/End, slPresentBegin and the present hook live in presentDirect / presentToSwapchain only).
		// While any of them is requested the worker stays off - today's render/dlss device, exactly - and the line says why; it beats
		// GX_NATIVE_CS=1 like the FG guard beats GX_VK_SUBMIT_THREAD=1. Lifted per feature once its entry points are S bodies or queued.
		// (tier 1): the seam's recording entry points (worldBegin/End/ObjectVelocity,
		// ResolveLinear, Upscale, HudBegin) are S bodies now, so the worker stays ON for the SPATIAL kinds a reachable device runs: the
		// request u maps to kind u - 1 (W3DWorldSeam: c.kind = s.upscaler - 1). Still forced off: DLSS and XeSS (Windows-only, no Windows
		// arm yet), the temporal kinds and motion units (phase 2 lifted them: below), frame generation and Reflex (present path),
		// and ANY loaded Streamline (its present hooks and slFrameEnd run in the Present body: GX_STREAMLINE_FORCE_LOAD=1 with nothing
		// requested used to leave the worker on under them - dlss-integrator's ruling 17/10 23:3xZ; Windows only, owed a Windows run).
		// (phase 2): motion units and the temporal kinds a Mac reaches (FSR3, MetalFX temporal)
		// keep the worker ON too - worldBegin/End/ObjectVelocity are S bodies, the bridge's other calls are ordinary queued d3d8 calls, and a
		// temporal backend dispatches inside Upscale's S. DLSS stays forced: with SR alone the submit thread stays on and Streamline's
		// sl.common present hooks run on gx-vk-submit, unordered with a front-side evaluate (seam2 design) - its own fix first.
		const int upKind = m_host.requestedUpscaler - 1;
		const bool upLifted = m_host.requestedUpscaler == 0 || upKind == GX_UPSCALE_BILINEAR || upKind == GX_UPSCALE_FSR1 ||
		                      upKind == GX_UPSCALE_METALFX || upKind == GX_UPSCALE_FSR3 || upKind == GX_UPSCALE_METALFX_TEMPORAL;
		if (want && !m_host.testNoCsDlssGuard &&
		    (!upLifted || m_host.requestedFrameGen > 0 || reflexRequested(m_host) > 0 || m_host.streamline)) {
			want = false;
			logLine("[GXNATIVE] cs=off forced: render/dlss feature requested (upscaler=%d motion_units=%d framegen=%d reflex=%d) - they record "
			        "outside the A23-B worker; streamline=%d", m_host.requestedUpscaler, m_host.motionUnits ? 1 : 0, m_host.requestedFrameGen,
			        reflexRequested(m_host), m_host.streamline ? 1 : 0);
		} else if (want && (m_host.requestedUpscaler != 0 || m_host.motionUnits))
			logLine("[GXNATIVE] cs=on with the seam: upscaler=%d motion_units=%d - its recording entry points are S bodies (SScope)",
			        m_host.requestedUpscaler, m_host.motionUnits ? 1 : 0);
		// GX_NATIVE_CS_RING_KIB: a TEST / tuning override (a power of two >= 64), never set in a measured run (PREREG A23-B am. 1 (5))
		size_t ring = kNativeCsRingBytes;
		if (const char* k = std::getenv("GX_NATIVE_CS_RING_KIB")) {
			const unsigned long v = std::strtoul(k, nullptr, 10);
			if (v >= 64 && (v & (v - 1)) == 0)
				ring = size_t(v) << 10;
		}
		if (want) {
			m_csOn = m_cs.start(ring, "gx-native-cs");
			if (!m_csOn)
				logLine("[GXNATIVE] gx-native-cs could not start: the device runs direct (GX_NATIVE_CS=0 path)");
		}
		logLine("[GXNATIVE] cs=%s ring=%zuKiB", m_csOn ? "on" : "off", m_csOn ? ring >> 10 : size_t(0));
		const char* mc = std::getenv("GX_NATIVE_CS_MIRRORCHECK");
		m_mirrorCheck = mc && std::strcmp(mc, "1") == 0;
	}
	{   // the final stats line is owed even if the game exits without destroying the device (a leaked child keeps it alive).
		// Registered HERE, after initVulkan loaded the Vulkan library: exit() runs it before that load's destructors (LIFO, §3.7).
		std::lock_guard<std::mutex> g(g_exitLock);
		g_exitDevice = this;
		static const bool s_registered = (std::atexit(gxNativeAtExitHook), true);   // A60: the CRT-exit form (bounded drain)
		(void)s_registered;
	}
	return true;
}

namespace {

// d3d8's own checks (d3d8_interface.cpp:127-135 for CreateDevice, d3d8_device.cpp:236-247 for Reset)
bool d3d8FrontValid(const D3DPRESENT_PARAMETERS& pp)
{
	if (pp.SwapEffect == D3DSWAPEFFECT_COPY && pp.BackBufferCount > 1)
		return false;
	return !(pp.Windowed && pp.FullScreen_PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT);
}

// d3d9_interface.cpp ValidatePresentationParameters on the converted parameters (d3d8_d3d9_util.h:115-128: COPY_VSYNC
// becomes COPY; a windowed interval is DEFAULT, or ONE for COPY_VSYNC - both valid there)
bool d3d9EarlyValid(const D3DPRESENT_PARAMETERS& pp)
{
	if (pp.SwapEffect == 0 || pp.SwapEffect > D3DSWAPEFFECT_COPY_VSYNC || pp.BackBufferCount > 3)   // D3DPRESENT_BACK_BUFFERS_MAX
		return false;
	const UINT i = pp.FullScreen_PresentationInterval;
	return pp.Windowed || i == D3DPRESENT_INTERVAL_DEFAULT || i == D3DPRESENT_INTERVAL_ONE || i == D3DPRESENT_INTERVAL_TWO ||
		i == D3DPRESENT_INTERVAL_THREE || i == D3DPRESENT_INTERVAL_FOUR || i == D3DPRESENT_INTERVAL_IMMEDIATE;
}

// What the native device can build at Reset (DESIGN S4): windowed, an A8R8G8B8 / X8R8G8B8 back buffer, power-of-two MSAA.
// DXVK fails the same parameters only partly (ResetSwapChain: format, sample count); fullscreen and its other back-buffer
// formats it would accept - the named divergence (no v1 session can be fullscreen or use another format, S4).
bool nativeBuildable(const D3DPRESENT_PARAMETERS& pp, const char** why)
{
	if (!pp.Windowed)
		return *why = "fullscreen", false;
	if (pp.BackBufferFormat != D3DFMT_A8R8G8B8 && pp.BackBufferFormat != D3DFMT_X8R8G8B8)
		return *why = "back-buffer format", false;
	if (pp.MultiSampleType > 1 && (pp.MultiSampleType & (pp.MultiSampleType - 1)))
		return *why = "multisample type", false;
	return true;
}

} // namespace

// (TESTS): whether this gx_device was compiled with GX_STREAMLINE (a define private to gx_device) - fgguard-streamline
// injects a fake host.streamline, which only the stubs of a non-Streamline build never dereference
bool GxNativeDevice::streamlineBuilt()
{
#if defined(GX_STREAMLINE)
	return true;
#else
	return false;
#endif
}

bool GxD3D8PresentParamsValid(const D3DPRESENT_PARAMETERS& pp)
{
	return d3d8FrontValid(pp) && d3d9EarlyValid(pp);
}

void GxD3D8NormalizePresentParams(D3DPRESENT_PARAMETERS* pp)
{
	if (!pp || !d3d8FrontValid(*pp))
		return;   // DXVK answers INVALIDCALL before it converts: the caller's struct is untouched
	pp->BackBufferCount = std::max<UINT>(pp->BackBufferCount, 1u);
	if (pp->BackBufferFormat == D3DFMT_UNKNOWN)
		pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

HRESULT GxNativeCreateDevice(IDirect3D8* parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
	const D3DPRESENT_PARAMETERS& pp, const GxNativeHost& host, IDirect3DDevice8** out, std::string* why)
{
	if (!out)
		return D3DERR_INVALIDCALL;
	*out = nullptr;
	GxNativeDevice* d = new GxNativeDevice(parent, adapter, type, focus, behavior, host);
	if (!d->init(pp, why)) {
		d->Release();
		return D3DERR_NOTAVAILABLE;
	}
	*out = d;
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ frames

void GxNativeDevice::ensureFrame()
{
	csOwnerCheck("ensureFrame");
	if (!m_writer.frameOpen())
		m_writer.beginFrame();
}

void GxNativeDevice::endFrameForHost()
{
	csOwnerCheck("endFrameForHost");
	if (m_writer.frameOpen()) {
		const uint64_t v = m_writer.endFrame();
		if (v)
			m_ctx.queue(V::QueueKind::Graphics).timeline.wait(v);
	}
}

// An offscreen Present ends the writer frame (no swapchain). TEST: a host gate the frame's submission also waits on (arms 2g, 23b)
void GxNativeDevice::endOffscreenFrame()
{
	if (m_debug.testGateSem) {
		V::SubmitDesc extra;
		V::SemaphoreWait w;
		w.semaphore = m_debug.testGateSem;
		w.value = m_debug.testGateValue;
		extra.waits.push_back(w);
		m_debug.testGateSem = VK_NULL_HANDLE;
		m_writer.endFrame(extra);
		return;
	}
	m_writer.endFrame();
}

void GxNativeDevice::flushIdleUploads()
{
	if (!m_writer.frameOpen() && m_gx.stagingPressure() > kIdleUploadFlushBytes) {   // A26 amendment 1: dropped staging counts
		csTrace(GxCsOp::FlushActed, {});   // a flush that acts is an exec event; a no-op one is not (both modes reach the same acts)
		m_writer.beginFrame();   // carries the queued uploads (beginFrame/endFrame record them)
		m_writer.endFrame();
		++m_stats.uploadFlushes;
	}
}

void GxNativeDevice::forgetImage(const R::GxImage* img)
{
	if (!img)
		return;
	csOwnerCheck("forgetImage");
	if (GxCsExecSink* s = g_csExecSink.load(std::memory_order_relaxed))
		s->retiredImage(img);   // TEST-ONLY: the translator-input digest names images by address - a reused one is a new image
	// A whole-target Clear waits in the writer's pass as its load op and forget() resets that pass: run the clear first (
	// rw3L-code-r8 #2: clear the back buffer with a temp depth bound, unbind and release the depth -> the clear was lost).
	if (m_writer.frameOpen() && m_tr.inWriterPass(img) && !m_debug.forgetDropsPendingClear)
		m_writer.outsideScope([](VkCommandBuffer) {}, nullptr, 0);
	m_writer.forget(img);
}

void GxNativeDevice::deferDestroy(GxNativeRefs* r)
{
	std::lock_guard<std::mutex> g(m_deadLock);
	m_dead.push_back(r);
}

void GxNativeDevice::drainDestroys()
{
	for (;;) {   // a destruction can release further objects (a texture's levels): loop until nothing is queued
		std::vector<GxNativeRefs*> dead;
		{
			std::lock_guard<std::mutex> g(m_deadLock);
			dead.swap(m_dead);
		}
		if (dead.empty())
			return;
		// A23-B §3.3: the accounting now, on the front; the storage as a Destroy command behind every command that names the object
		for (GxNativeRefs* r : dead) {
			r->destroyFront();
			cs([r] {
				csTrace(GxCsOp::Destroy, { static_cast<const void*>(r) });
				if (GxCsExecSink* s = g_csExecSink.load(std::memory_order_relaxed)) {   // before destroyStorage deletes r: nothing can
					s->destroyed(r);                                                  // reuse an address still allocated
					r->traceRetire(*s);   // the addresses the traces name it by: a reused one is a new object in both modes
				}
				r->destroyStorage();
			});
		}
	}
}

void GxNativeDevice::noteManaged(GxNativeTexture* t)
{
	if (std::find(m_dropCandidates.begin(), m_dropCandidates.end(), t) == m_dropCandidates.end())
		m_dropCandidates.push_back(t);
}

void GxNativeDevice::unnoteManaged(GxNativeTexture* t)
{
	m_dropCandidates.erase(std::remove(m_dropCandidates.begin(), m_dropCandidates.end(), t), m_dropCandidates.end());
}

// DESIGN §6 (2): at Present, every filled, unpinned MANAGED texture drops its host copy. A pinned one stays off the list.
void GxNativeDevice::dropManaged()
{
	std::vector<GxNativeTexture*> keep;
	for (GxNativeTexture* t : m_dropCandidates) {
		if (t->dropCandidate() && t->gx().dropHostCopy())
			++m_stats.dropped;
		else if (!t->pinned() || m_debug.dropIgnoresPin)
			keep.push_back(t);   // not filled yet: try again at the next Present
	}
	m_dropCandidates.swap(keep);
}

R::GxRenderTarget* GxNativeDevice::backBufferStorage(uint32_t index) const
{
	if (m_bbStore.empty())
		return nullptr;
	const uint32_t stale = m_debug.csBbStale && m_cs.onWorker() ? uint32_t(m_bbStore.size()) - 1 : 0;   // the X-tap-substitute plant
	return m_bbStore[(index + m_rot + stale) % m_bbStore.size()].get();
}

// d3d9_swapchain.cpp:925-939: surface i shows what surface i+1 held; the front buffer rotates in when it exists.
void GxNativeDevice::rotateBackBuffers()
{
	if (m_debug.noRotate || m_bbStore.size() < 2)
		return;
	m_rot = (m_rot + 1) % uint32_t(m_bbStore.size());
}

// Runs on the worker (or direct): reads the WORKER's copy of the binding (A23-B §3.5). Direct - CS=0, or an S body with the worker
// idle - the binding is the front's own.
void GxNativeDevice::syncTarget(bool bindViewport)
{
	if (!m_cs.onWorker()) {
		m_wRt = m_rt;
		m_wDs = m_ds;
	}
	GxNativeSurface* const rt = m_wRt;
	GxNativeSurface* const ds = m_wDs;
	Rec::GxD3D8Target c, d;
	if (rt && rt->kind() == GxNativeSurface::Kind::TextureLevel && rt->level() > 0) {
		// DXVK renders into any level; GXRES gives a texture an attachment view for level 0 only. No caller is known to bind a
		// lower level (INFERRED): no target = its clears and draws refused by name, counted, first one logged.
		if (m_stats.targetsRefused++ == 0)
			logLine("[GXNATIVE] REFUSED render target: texture level %u > 0 has no attachment view", rt->level());
	} else if (rt) {
		if (R::GxImage* img = rt->image()) {
			c.image = img;
			c.view = rt->attachmentView();
			c.texture = rt->kind() == GxNativeSurface::Kind::TextureLevel;
			c.d3dFormat = rt->d3dFormat();
		}
	}
	if (ds) {
		if (R::GxImage* img = ds->image()) {
			d.image = img;
			d.view = ds->attachmentView();
			d.d3dFormat = ds->d3dFormat();
		}
	}
	m_tr.renderTarget(c, d, bindViewport);
}

void GxNativeDevice::bindSurface(GxNativeSurface*& slot, GxNativeSurface* s)
{
	if (slot == s)
		return;
	if (s)
		s->addPrivateRef();
	GxNativeSurface* old = slot;
	slot = s;
	if (old)
		old->releasePrivateRef();
}

template <class T>
void GxNativeDevice::bindPrivate(T*& slot, T* obj)
{
	if (slot == obj)
		return;
	if (obj && !m_debug.noPrivateRefs)
		obj->addPrivateRef();
	T* old = slot;
	slot = obj;
	if (old && !m_debug.noPrivateRefs)
		old->releasePrivateRef();
}

// ------------------------------------------------------------------------------------------------------------ A23-B front end

// §3.1 / §4.2: the writer, the translator's target, the swapchain objects are the WORKER's while it runs; the front may touch them
// only inside an S body (the worker drained) or direct. Counted and logged once - a cheap check at a few chokepoints, also in the
// product (it is what X-cs-reset-nodrain trips)
void GxNativeDevice::csOwnerCheck(const char* where)
{
	// onWorker() first: m_inline is FRONT state - the worker never reads it (a worker draw
	// racing an SScope entry on the front)
	if (m_cs.onWorker() || !m_csOn || m_inline != 0)
		return;
	if (m_ownerViolations++ == 0)
		logLine("[GXNATIVE] A23-B ownership: %s reached on the front outside an S body", where);
}

void GxNativeDevice::csSync(const char* why)
{
	if (m_csOn && !m_cs.onWorker()) {
		const auto t0 = std::chrono::steady_clock::now();
		m_cs.drain();
		const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
		noteSync(why, m_debug.csDrainTimeZero ? 0 : ns);   // (the plant)
	}
}

// The SERIAL schedule's point (TEST-ONLY): everything dispatched has run and the GPU finished it - so every timing-dependent
// choice that follows (a DISCARD's slice, upload-ring offsets, an idle flush) sees one state in every mode. The caller is the
// producer here: the worker is drained first.
void GxNativeDevice::csSerialPoint()
{
	if (m_csOn && !m_cs.onWorker())
		m_cs.drain();
	if (!m_cs.onWorker())
		m_ctx.waitIdle();
}

void GxNativeDevice::settleAtExit()
{
	if (m_debug.exitNoQuiesce)
		return;   // gx-vk-submit still runs: its present status is not the hook's to take (rw3-T-6)
	m_ctx.quiesce();
	if (m_haveSwapchain)   // the thread is joined: the hook is the producer - as teardown, the last present's answer is in the final line
		applyPresentStatus(m_swapchain.takePresentStatus());
	// and, as teardown (DESIGN §3.9), the device idle: no GPU work of ours is still completing after the line. Enqueues
	// nothing (the thread is joined).
	m_ctx.waitIdle();
	// and no pipeline worker after the line: the drained commands can miss the cache and queue specialised compiles, and a process
	// that ends through static destruction right after (Windows: WinMain returns -> CRT exit) would run them against destroyed
	// statics - measured on the Mac test child: "mutex lock failed" aborts (render-integration-5's condition before default ON)
	if (!m_debug.csExitNoSpecQuiesce)
		m_cache.quiesceWorkers();
}

void GxNativeDevice::csPark()
{
	if (!m_csOn)
		return;
	m_cs.stop();   // drains first
	m_csOn = false;
}

void GxNativeDevice::noteSync(const char* why, uint64_t ns)
{
	++m_csSyncs;
	for (auto& w : m_csSyncWhy)
		if (w.why == why || std::strcmp(w.why, why) == 0) {
			++w.n;
			w.ns += ns;
			return;
		}
	m_csSyncWhy.push_back({ why, 1, ns });
}

void GxNativeDevice::syncTopInto(char* out, size_t cap, bool byNs) const
{
	std::vector<SyncWhy> top = m_csSyncWhy;
	std::sort(top.begin(), top.end(), [byNs](const SyncWhy& a, const SyncWhy& b) { return byNs ? a.ns > b.ns : a.n > b.n; });
	size_t at = 0;
	for (size_t i = 0; i < top.size() && i < 3; ++i) {
		const int n = byNs ? std::snprintf(out + at, cap - at, "%s%s:%.1fms", i ? "," : "", top[i].why, double(top[i].ns) / 1e6)
		                   : std::snprintf(out + at, cap - at, "%s%s:%llu", i ? "," : "", top[i].why, (unsigned long long)top[i].n);
		if (n < 0 || size_t(n) >= cap - at)
			break;
		at += size_t(n);
	}
}

// (seam2 design): what the motion entry points did, cumulative since the device started - printed only while
// motion is initialised (key off: no line). A refused object registration (the bridge ignores SetObjectVelocity's return) shows here.
void GxNativeDevice::logMotionCounters(uint64_t presents)
{
	if (!m_motionInit)
		return;
	const MotionCounters& c = m_motionCount;
	logLine("[GXMOTION] device presents=%llu passes=%llu published=%llu with_object=%llu obj_accepted=%llu obj_refused=%llu",
	        (unsigned long long)presents, (unsigned long long)c.passes, (unsigned long long)c.published, (unsigned long long)c.withObject,
	        (unsigned long long)c.objAccepted, (unsigned long long)c.objRefused);
}

// the seam's S reasons ("seam <entry point>") since the last such line, per reason count + drain ms - printed only in a
// stats period where one was noted (key-off logs unchanged; the dlss-integrator's condition), from the front at the period's Present
void GxNativeDevice::logSeamDrains(uint64_t presents)
{
	char line[512];
	size_t at = 0;
	for (const SyncWhy& w : m_csSyncWhy) {
		if (std::strncmp(w.why, "seam ", 5) != 0)
			continue;
		SyncWhy* last = nullptr;
		for (SyncWhy& a : m_seamSyncAt)
			if (std::strcmp(a.why, w.why) == 0)
				last = &a;
		if (!last) {
			m_seamSyncAt.push_back({ w.why, 0, 0 });
			last = &m_seamSyncAt.back();
		}
		const uint64_t n = w.n - last->n, ns = w.ns - last->ns;
		*last = w;
		if (!n)
			continue;
		const int k = std::snprintf(line + at, sizeof(line) - at, " %s:n=%llu,ms=%.2f", w.why + 5, (unsigned long long)n, double(ns) / 1e6);
		if (k < 0 || size_t(k) >= sizeof(line) - at)
			break;
		at += size_t(k);
	}
	if (at)
		logLine("[GXSEAM] cs drains presents=%llu%s", (unsigned long long)presents, line);
}

GxNativeDevice::SScope::SScope(GxNativeDevice& d, const char* why) : m_d(d)
{
	if (m_d.m_inline == 0)
		m_d.m_sRecovered = false;   // a new S transition: it may recover storage once (§3.8)
	if (m_d.m_inline++ == 0 && m_d.m_csOn) {
		const auto t0 = std::chrono::steady_clock::now();
		m_d.m_cs.drain();
		const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
		m_d.noteSync(why, m_d.m_debug.csDrainTimeZeroS ? 0 : ns);   // (the plant)
	}
	if (m_d.m_inline == 1)
		m_d.mirrorCheck(why);   // before this S body (and its re-seed): the mirror must already equal the translator
}

GxNativeDevice::SScope::~SScope()
{
	if (--m_d.m_inline == 0) {   // §3.10 step 4: reconcile before production goes back to the worker
		m_d.reseedMirror();
		m_d.m_wRt = m_d.m_rt;
		m_d.m_wDs = m_d.m_ds;
		m_d.resetPressureBound();
	}
}

// §3.8: what a failed allocation returns before today's code runs again, by context. PRODUCER (an S body: the worker drained):
// wait for every SUBMITTED value (never the open frame - it is not submitted, so its storage stays, as today), collect, free idle
// dynamic slices. WORKER: only the wait for what it submitted itself and collect - it never drains itself, never touches the
// front's slice pools. No cycle: the worker waits only for submitted GPU work; the producer waited for the worker before this.
void GxNativeDevice::recoverStorage(bool producer, const char* site)
{
	++m_recoveries;
	{
		std::lock_guard<std::mutex> g(m_recoverLock);
		++m_recoverBySite[site ? site : "?"];
	}
	if (!producer && m_debug.csWorkerSelfDrain)
		m_cs.drain();   // the plant: the worker waits for the stream it is running - a real cycle
	V::Timeline& tl = m_ctx.queue(V::QueueKind::Graphics).timeline;
	const uint64_t last = tl.lastSubmitted();
	if (testRecoverWait)
		testRecoverWait(testRecoverWaitUser, last, last);
	const bool settled = !last || m_debug.csReclaimNoWait || tl.wait(last);
	// a failed wait or a latched refusal: free NOTHING - after a refusal's drain collect() settles every retirement, the OPEN frame's
	// (filed under its unsubmitted value) included, and this runs mid-frame (a freed uniform buffer
	// used by the same draw). The caller's retry then fails as today's body does (counted).
	if ((!settled || m_ctx.deviceLost()) && !m_debug.csRecoverIgnoresLatch && !(m_debug.csRecoverIgnoresLatchWorker && !producer))
		return;
	m_ctx.collect();
	if (producer && !m_debug.csNoReclaim)
		m_gx.reclaimIdleSlices();
}

bool GxNativeDevice::AllocPolicy::inject(const char* site, VkDeviceSize bytes)
{
	GxNativeDebug& d = m_d.m_debug;
	if (d.allocFailCount && d.allocFailSite && (!std::strcmp(d.allocFailSite, "*") || !std::strcmp(d.allocFailSite, site))) {
		--d.allocFailCount;
		return true;
	}
	if (d.allocBudget) {
		VmaBudget b[VK_MAX_MEMORY_HEAPS] = {};
		vmaGetHeapBudgets(m_d.m_ctx.allocator(), b);
		VkDeviceSize used = 0;   // the bytes of live VMA allocations (not blocks: a small image lives inside a big block)
		for (const VmaBudget& h : b)
			used += h.statistics.allocationBytes;
		if (used + bytes > d.allocBudget)
			return true;
	}
	return false;
}

GxNativeDevice::AllocPolicy::Decision GxNativeDevice::AllocPolicy::failed(const char* site, VkDeviceSize)
{
	GxNativeDevice& d = m_d;
	if (!d.m_csOn || (d.m_debug.csRecoveryBypass && !std::strcmp(d.m_debug.csRecoveryBypass, site)))
		return Decision::Fail;   // direct: nothing more is held than today - today's failure (now counted)
	if (d.m_cs.onWorker()) {
		d.recoverStorage(false, site);
		return Decision::Retry;
	}
	if (d.m_inline > 0) {   // inside an S body: its first in-scope failure recovers, once per S transition
		if (d.m_sRecovered || d.m_debug.csSbodyNoRecover)
			return Decision::Fail;
		d.m_sRecovered = true;
		d.recoverStorage(true, site);
		return Decision::Retry;
	}
	return Decision::Escape;   // a front ATTEMPT (Create*, a DISCARD's slice): the entry point's single S retry owns it
}

void GxNativeDevice::AllocPolicy::terminal(const char* site, VkDeviceSize bytes)
{
	if (m_d.m_debug.csSliceTerminalSilent && m_d.m_csOn && !std::strcmp(site, "slice"))
		return;   // the plant
	if (m_d.m_debug.csTerminalSilent || (m_d.m_debug.csFailureSilent && m_d.m_cs.onWorker()))
		return;
	if (++m_d.m_allocFailures <= 16)
		m_d.logLine("[GXNATIVE] allocation failed: %s %llu after recovery", site, (unsigned long long)bytes);
}

void GxNativeDevice::publishPressure()
{
	if (!m_cs.onWorker())
		return;   // direct: the front reads the exact value after the S body (resetPressureBound)
	m_pubPressure.store(m_gx.stagingPressure(), std::memory_order_relaxed);
	m_pubSeq.store(m_cs.runningSeq(), std::memory_order_release);
}

void GxNativeDevice::noteStagingCmd(uint64_t bytes)
{
	pruneStaged(m_pubSeq.load(std::memory_order_acquire));   // bounded: only commands the worker has not yet published past
	m_stagedQ.push_back({ m_cs.queued(), bytes });
	m_stagedSum += bytes;
}

// drops the entries the worker has published past (their staging is in the published pressure) and compacts the consumed prefix
// - on every upload command, not only in the idle flush (completed history grew without bound)
void GxNativeDevice::pruneStaged(uint64_t seq)
{
	while (m_stagedHead < m_stagedQ.size() && m_stagedQ[m_stagedHead].first <= seq)
		m_stagedSum -= m_stagedQ[m_stagedHead++].second;
	if (m_stagedHead == m_stagedQ.size()) {
		m_stagedQ.clear();
		m_stagedHead = 0;
	} else if (m_stagedHead >= 1024 && m_stagedHead * 2 >= m_stagedQ.size()) {
		m_stagedQ.erase(m_stagedQ.begin(), m_stagedQ.begin() + std::ptrdiff_t(m_stagedHead));
		m_stagedHead = 0;
	}
}

void GxNativeDevice::resetPressureBound()
{
	m_pubPressure.store(m_gx.stagingPressure(), std::memory_order_relaxed);
	m_pubSeq.store(m_cs.queued(), std::memory_order_release);
	m_stagedQ.clear();
	m_stagedHead = 0;
	m_stagedSum = 0;
}

// §3.6 IDLE-FLUSH: the bound U = the published pressure + the staging of every upload command queued after the command that
// published it. U is an upper bound: pressure grows only in upload exec halves and S bodies, and a submission only lowers it; a
// pressure read newer than its seq only over-counts (the worker stores the pressure, then the seq - release; read in reverse).
void GxNativeDevice::flushIdleFront()
{
	if (csDirect()) {
		flushIdleUploads();
		return;
	}
	if (m_debug.csFlushDropped)
		return;   // the plant
	const uint64_t seq = m_pubSeq.load(std::memory_order_acquire);
	const uint64_t pressure = m_pubPressure.load(std::memory_order_relaxed);
	pruneStaged(seq);
	if (pressure + m_stagedSum > kIdleUploadFlushBytes)
		cs([this] {
			flushIdleUploads();
			publishPressure();
		});
}

// today's whole lock body (direct, or inside an S body): its rename, if it made one, is the same exec event as a Rename command
void* GxNativeDevice::lockBufferInline(R::GxBuffer& b, uint32_t offset, uint32_t bytes, uint32_t flags)
{
	const R::GxBuffer::Slice* before = b.currentSlice();
	void* p = b.lock(offset, bytes, flags);
	if (b.currentSlice() != before) {
		++m_csIssued;
		csTrace(GxCsOp::Rename, { static_cast<const void*>(&b), uint64_t(b.currentSlice()->ordinal) });
		csAfter();
	}
	return p;
}

void* GxNativeDevice::lockBuffer(R::GxBuffer& b, uint32_t offset, uint32_t bytes, uint32_t flags)
{
	if (csDirect())
		return lockBufferInline(b, offset, bytes, flags);
	R::GxBuffer::Slice* renamed = nullptr;
	bool sync = false, allocFailed = false;
	void* p = b.lockFront(offset, bytes, flags, m_debug.csDiscardEarly ? UINT64_MAX : m_cs.done(), m_cs.queued(), &renamed, &sync,
		&allocFailed);
	if (sync) {   // §3.6 / §3.10 1b: today's lock body on the producer, the worker idle; then the worker's slice follows the front's
		if (allocFailed && m_debug.csDiscardNoRetry)
			return nullptr;   // the plant: the failed attempt answers INVALIDCALL
		SScope sc(*this, allocFailed ? "buffer DISCARD slice retry" : (flags & D3DLOCK_DISCARD) ? "buffer DISCARD" : "buffer plain lock");
		if (allocFailed) {   // / §3.8: a failed slice allocation returns what it can first, then today's lock body
			m_sRecovered = true;
			recoverStorage(true, "slice");
		}
		p = lockBufferInline(b, offset, bytes,
			m_debug.csPlainLockForwardsDiscard && !allocFailed && !(flags & D3DLOCK_DISCARD) ? flags | D3DLOCK_DISCARD : flags);   // (the plant)
		b.reconcile(m_cs.queued());
		return p;
	}
	if (renamed) {
		R::GxBuffer* bp = &b;
		++m_csIssued;
		m_cs.push([bp, renamed] {
			csTrace(GxCsOp::Rename, { static_cast<const void*>(bp), uint64_t(renamed->ordinal) });
			bp->execRename(renamed);
		});
		csAfter();
	}
	return p;
}

void GxNativeDevice::unlockBuffer(R::GxBuffer& b)
{
	uint32_t offset = 0, n = 0;
	if (!b.unlockFront(&offset, &n)) {   // dynamic (flushed on the front) or READONLY: F
		flushIdleFront();
		return;
	}
	if (m_payloadLate) {   // the X-cs-payload-late plant: the deferred upload goes out now, reading the shadow as it is NOW
		std::function<void()> late = std::move(m_payloadLate);
		m_payloadLate = nullptr;
		cs(std::move(late));
	}
	R::GxBuffer* bp = &b;
	auto exec = [this, bp, offset](const uint8_t* p, size_t bytes) {   // today's second half of unlock, then its idle flush
		const GxCsBytes tb = csFlat(p, bytes);
		csTrace(GxCsOp::Upload, { static_cast<const void*>(bp), uint64_t(offset) }, &tb);
		bp->execUpload(p, m_debug.csUploadOffsetZero && m_csOn ? 0u : offset, uint32_t(bytes));   // (the plant)
		if (!(m_debug.csExecNoFlush && m_cs.onWorker()))
			flushIdleUploads();
		publishPressure();
	};
	std::unique_ptr<SScope> sync;
	if (!csDirect() && n > m_cs.maxPayload())   // §3.2 kMaxCmd: S (the load-time 16 MiB unlocks)
		sync.reset(new SScope(*this, "Unlock oversize"));
	const bool queued = !csDirect();
	if (queued && !sync && m_debug.csPayloadLate && !m_payloadLateDone) {
		m_payloadLateDone = true;
		const uint32_t n32 = n;
		m_payloadLate = [exec, bp, offset, n32] { exec(bp->shadow() + offset, n32); };
		noteStagingCmd(n);
		return;
	}
	if (queued && m_debug.csUnlockByRef) {   // the plant: the shadow read when the worker runs it
		const uint32_t n32 = n;
		cs([exec, bp, offset, n32] { exec(bp->shadow() + offset, n32); });
		noteStagingCmd(n);
		return;
	}
	csCopy(exec, b.shadow() + offset, n);
	if (queued)
		noteStagingCmd(n);
}

void GxNativeDevice::unlockTextureLevel(GxNativeTexture* t, uint32_t level)
{
	R::GxTexture& g = t->gx();
	VkRect2D r;
	const bool upload = g.unlockFront(level, &r);
	t->noteWritten(level);
	if (!upload) {   // SYSTEMMEM / READONLY: nothing uploaded today
		flushIdleFront();
		return;
	}
	// the locked rect's rows, tight, as today's uploadRect hands them to stageRect (a DECODED texture decodes them there)
	const uint32_t rowBytes = g.rectRowBytes(r), rows = g.rectRows(r), pitch = g.levelPitch(level);
	const uint8_t* src = g.rectHostStart(level, r);
	const size_t bytes = size_t(rowBytes) * rows;
	R::GxTexture* gp = &g;
	auto exec = [this, gp, t, level, r, rowBytes, rows](const uint8_t* p, uint32_t srcPitch) {
		GxCsBytes tb;
		tb.p = p;
		tb.rowBytes = rowBytes;
		tb.rows = rows;
		tb.pitch = srcPitch;
		csTrace(GxCsOp::UploadRect, { static_cast<const void*>(t), uint64_t(level), uint64_t(r.offset.x), uint64_t(r.offset.y),
			uint64_t(r.extent.width), uint64_t(r.extent.height) }, &tb);
		gp->execUploadRect(level, r, p, srcPitch);
		flushIdleUploads();
		publishPressure();
	};
	if (csDirect() || bytes > m_cs.maxPayload()) {
		std::unique_ptr<SScope> sync;
		const bool late = m_debug.csOversizeLateDrain && !csDirect();   // (the plant: S entered after the inline exec)
		if (!csDirect() && !late)
			sync.reset(new SScope(*this, "UnlockRect oversize"));
		++m_csIssued;
		exec(src, m_debug.csRectPitchSwap && m_csOn ? rowBytes : pitch);   // (the plant)
		if (late)
			sync.reset(new SScope(*this, "UnlockRect oversize"));
		csAfter();
		return;
	}
	++m_csIssued;
	if (m_debug.csTexUnlockByRef) {   // the plant: the host rows read when the worker runs it
		m_cs.push([exec, gp, level, r, pitch] { exec(gp->rectHostStart(level, r), pitch); });
		noteStagingCmd(g.rectStagingBytes(r));
		csAfter();
		return;
	}
	m_cs.pushFill([this, exec, rowBytes, pitch](const uint8_t* p, size_t) { exec(p, m_debug.csRectPitchSwap ? pitch : rowBytes); }, bytes, [&](uint8_t* dst) {   // (the plant)
		for (uint32_t y = 0; y < rows; ++y)
			std::memcpy(dst + size_t(y) * rowBytes, src + size_t(y) * pitch, rowBytes);
	});
	noteStagingCmd(g.rectStagingBytes(r));
	csAfter();
}

void GxNativeDevice::setTextureLod(GxNativeTexture* t, uint32_t lod)
{
	if (m_debug.csDropLod && !csDirect())
		return;   // the plant
	cs([t, lod] {
		csTrace(GxCsOp::SetLod, { static_cast<const void*>(t), uint64_t(lod) });
		t->gx().setLod(lod);
	});
}

void GxNativeDevice::reseedMirror()
{
	const GxD3D8FFState& f = m_tr.state();
	std::memcpy(m_mirror.world, f.world, sizeof(f.world));
	std::memcpy(m_mirror.view, f.view, sizeof(f.view));
	// render/dlss motion: the translator may hold the JITTERED projection inside a world pass; the mirror keeps what the caller set
	// (motion's clean copy), as SetTransform does
	const float* cleanProj = m_motionInit ? m_motion.cleanProjection() : nullptr;
	std::memcpy(m_mirror.projection, cleanProj ? cleanProj : f.projection, sizeof(f.projection));
	std::memcpy(m_mirror.texture, f.texture, sizeof(f.texture));
	std::memcpy(m_mirror.rs, f.rs, sizeof(f.rs));
	std::memcpy(m_mirror.tss, f.tss, sizeof(f.tss));
	m_mirror.material = f.material;
	m_mirror.viewport = f.viewport;
}

// §3.4 mirror-check mode (GX_NATIVE_CS_MIRRORCHECK=1, its own test mode): after each Present and at each S point (before
// the re-seed) the worker is drained and every mirrored field compared with the translator's; each field that differs counts.
void GxNativeDevice::mirrorCheck(const char* where)
{
	if (!m_mirrorCheck)
		return;
	if (m_csOn && m_inline == 0)
		m_cs.drain();
	const GxD3D8FFState& f = m_tr.state();
	uint32_t n = 0;
	std::string first;
	auto field = [&](bool same, const std::string& name) {
		if (!same && n++ == 0)
			first = name;
	};
	field(!std::memcmp(m_mirror.world, f.world, sizeof(f.world)), "world");
	field(!std::memcmp(m_mirror.view, f.view, sizeof(f.view)), "view");
	const float* cleanProj = m_motionInit ? m_motion.cleanProjection() : nullptr;   // the caller's projection (motion: never the jittered one)
	field(!std::memcmp(m_mirror.projection, cleanProj ? cleanProj : f.projection, sizeof(f.projection)), "projection");
	for (uint32_t i = 0; i < 8; ++i)
		field(!std::memcmp(m_mirror.texture[i], f.texture[i], sizeof(f.texture[i])), "texture" + std::to_string(i));
	for (uint32_t i = 0; i < 256; ++i)
		field(m_mirror.rs[i] == f.rs[i], "rs" + std::to_string(i));
	for (uint32_t s = 0; s < 8; ++s)
		for (uint32_t i = 0; i < 32; ++i)
			field(m_mirror.tss[s][i] == f.tss[s][i], "tss" + std::to_string(s) + "." + std::to_string(i));
	field(!std::memcmp(&m_mirror.material, &f.material, sizeof(f.material)), "material");
	field(!std::memcmp(&m_mirror.viewport, &f.viewport, sizeof(f.viewport)), "viewport");
	if (!n)
		return;
	m_mirrorMismatches += n;
	if (m_mirrorLogged++ < 8)
		logLine("[GXNATIVE] mirror check (%s): %u fields differ, first %s", where, n, first.c_str());
}

// What the worker's syncTarget will hand the translator as the colour image, decided from FRONT state: no target, a texture
// level > 0 (refused), an image surface or a SYSTEMMEM level (no image), a back buffer with no storage -> none.
bool GxNativeDevice::frontColourHasImage() const
{
	if (!m_rt || (m_rt->kind() == GxNativeSurface::Kind::TextureLevel && m_rt->level() > 0))
		return false;
	return m_rt->frontHasImage();
}

// ------------------------------------------------------------------------------------------------------------ queries

HRESULT GxNativeDevice::TestCooperativeLevel()
{
	// A23-B §3.5: F - the latched atomics; lags the worker (a loss it latches reaches here within <= 2 Presents, divergence a)
	if ((m_ctx.deviceLost() || m_presentDead || m_presentLost) && !m_debug.csLossUnpublished)
		return D3DERR_DEVICELOST;
	return m_notReset ? D3DERR_DEVICENOTRESET : D3D_OK;   // d3d9_device.cpp TestCooperativeLevel
}

UINT GxNativeDevice::GetAvailableTextureMem()
{
	// Only the dead Get_Free_Texture_RAM calls this (census); DXVK's value is "not meant to be accurate" (d3d9_device.cpp:365).
	return neverCalled("GetAvailableTextureMem"), 0x7ff00000u;
}

HRESULT GxNativeDevice::GetDirect3D(IDirect3D8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = m_parent;
	if (m_parent)
		m_parent->AddRef();
	return m_parent ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT GxNativeDevice::GetDeviceCaps(D3DCAPS8* caps)
{
	if (!m_parent || !caps)
		return D3DERR_INVALIDCALL;
	const HRESULT hr = m_parent->GetDeviceCaps(m_adapter, m_type, caps);
	if (SUCCEEDED(hr)) {   // the DEVICE's answer (d3d9_device.cpp:391-397): 256 blend matrices under software vertex processing
		caps->MaxVertexBlendMatrixIndex = m_swvp ? 255 : 8;
		// the vertex-shader constants this device implements - the translator holds c0-c95 and a custom
		// vs reads them from the first 1536 B of the VS block (GxD3D8Translate m_vsConst). DXVK says 256; d3d8's vs.1.1 floor is
		// 96. The one reader that sizes by it, GX_SHADOW_GPU (W3DVolumetricShadow.cpp gxShadowGpuSlots), then packs 21 volumes a
		// draw instead of 61: same triangles in the same order, about 3x its draws.
		// Trade-off: 96 constants; 256 (61 volumes a draw) needs m_vsConst[256] + a 4 KiB custom VS block in GxPacketWriter
		// (R11 option A) - only if the shadow draws ever show up in the heavy main-thread split.
		if (!m_debug.vsConstCapsFromDxvk)
			caps->MaxVertexShaderConst = std::min<DWORD>(caps->MaxVertexShaderConst, kGxNativeMaxVsConst);
	}
	return hr;
}

HRESULT GxNativeDevice::GetDisplayMode(D3DDISPLAYMODE* mode)
{
	return m_parent ? m_parent->GetAdapterDisplayMode(m_adapter, mode) : D3DERR_INVALIDCALL;
}

HRESULT GxNativeDevice::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p)
{
	if (!p)
		return D3DERR_INVALIDCALL;
	p->AdapterOrdinal = m_adapter;
	p->DeviceType = m_type;
	p->hFocusWindow = m_focus;
	p->BehaviorFlags = m_behavior;
	return D3D_OK;
}

HRESULT GxNativeDevice::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8** pp)
{
	if (pp)
		*pp = nullptr;
	return neverCalled("CreateAdditionalSwapChain");
}

HRESULT GxNativeDevice::GetRasterStatus(D3DRASTER_STATUS*) { return neverCalled("GetRasterStatus"); }

void GxNativeDevice::SetGammaRamp(DWORD, const D3DGAMMARAMP* ramp)
{
	if (!ramp)
		return;
	m_gamma = *ramp;
	// DXVK applies a ramp only fullscreen and only when it is not the identity (d3d9_swapchain.cpp:712-715). Fullscreen is not
	// applied in v1 (DESIGN §8.5): counted, logged once.
	bool identity = true;
	for (uint32_t i = 0; i < 256 && identity; ++i)
		identity = ramp->red[i] == WORD(i * 257) && ramp->green[i] == WORD(i * 257) && ramp->blue[i] == WORD(i * 257);
	if (!identity && !m_pp.Windowed && m_stats.gammaIgnored++ == 0)
		logLine("[GXNATIVE] gamma not applied (fullscreen non-identity ramp; DESIGN 8.5 owed)");
}

void GxNativeDevice::GetGammaRamp(D3DGAMMARAMP* ramp)
{
	if (ramp)
		*ramp = m_gamma;
	neverCalled("GetGammaRamp");
}

HRESULT GxNativeDevice::ValidateDevice(DWORD* passes)
{
	if (passes)
		*passes = 1;
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ creation

// A23-B §3.8: creation runs on the front. With the worker running, an allocation ATTEMPT comes first - GXRES init only: no
// `creates` count, no refusal, no log, nothing left behind on failure (destroyUnpublished). It succeeds: today's bookkeeping
// once. It fails: the call becomes S - the queued Destroys run, what the GPU finished is freed - and runs today's WHOLE create
// body, which counts and refuses exactly as today. Direct: today's body.
#define GX_CREATE_ATTEMPT(name, initCall, onSuccess)                                                                          \
	std::unique_ptr<SScope> retry;                                                                                               \
	if (!csDirect()) {                                                                                                           \
		if (initCall)                                                                                                            \
			return ++m_stats.creates, onSuccess;                                                                                 \
		if (m_debug.csRetryDoubleCount)                                                                                          \
			++m_stats.creates;                                                                                                   \
		if (m_debug.csNoRetry)                                                                                                   \
			return D3DERR_INVALIDCALL;                                                                                           \
		if (m_debug.csCreateDefer)                                                                                               \
			return D3D_OK;                                                                                                       \
		retry.reset(new SScope(*this, name " retry"));                                                                           \
		++m_csCreateRetries;                                                                                                     \
		m_sRecovered = true;   /* this S transition recovers here, once: the inline body's failures are terminal */             \
		if (!m_debug.csRecoveryBypass || std::strcmp(m_debug.csRecoveryBypass, name))                                           \
			recoverStorage(true, name);                                                                                          \
	}

// §7.1 TEST-ONLY: the creation ledger - a front creation, in front order, at its position among the exec halves dispatched
void GxNativeDevice::csLedger(uint64_t position, uint32_t kind, std::initializer_list<GxCsArg> a)
{
	if (GxCsExecSink* s = g_csExecSink.load(std::memory_order_relaxed)) {
		s->created(position, kind, a.begin(), a.size());
		if (m_debug.csExecPhysical)   // the plant: the creation lands in the EXEC stream at its physical time (front, now)
			s->exec(GxCsOp::Destroy, a.begin(), a.size(), nullptr);
	}
}

HRESULT GxNativeDevice::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8** pp)
{
	drainDestroys();
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto t = std::make_unique<R::GxTexture>();
	R::TextureDesc td;
	td.width = w;
	td.height = h;
	td.levels = levels;
	td.format = uint32_t(fmt);
	td.usage = usage;
	td.pool = uint32_t(pool);
	auto made = [&] {
		csLedger(m_csIssued, 1, { uint64_t(w), uint64_t(h), uint64_t(levels), uint64_t(usage), uint64_t(fmt), uint64_t(pool) });
		GxNativeTexture* n = new GxNativeTexture(*this, std::move(t));
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateTexture", t->init(m_gx, td, nullptr), made())
	++m_stats.creates;
	std::string why;
	if (!t->init(m_gx, td, &why)) {
		refuseCreate(("texture: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}

HRESULT GxNativeDevice::CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture8** pp)
{
	if (pp)
		*pp = nullptr;
	return neverCalled("CreateVolumeTexture");
}

HRESULT GxNativeDevice::CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture8** pp)
{
	if (pp)
		*pp = nullptr;
	return neverCalled("CreateCubeTexture");
}

HRESULT GxNativeDevice::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** pp)
{
	drainDestroys();
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto b = std::make_unique<R::GxBuffer>();
	R::BufferDesc bd;
	bd.bytes = length;
	bd.usage = usage;
	bd.pool = uint32_t(pool);
	auto made = [&] {
		csLedger(m_csIssued, 2, { uint64_t(length), uint64_t(usage), uint64_t(fvf), uint64_t(pool) });
		GxNativeVertexBuffer* n = new GxNativeVertexBuffer(*this, std::move(b), usage, fvf, pool);
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateVertexBuffer", b->init(m_gx, bd, nullptr), made())
	++m_stats.creates;
	std::string why;
	if (!b->init(m_gx, bd, &why)) {
		refuseCreate(("vertex buffer: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}

HRESULT GxNativeDevice::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer8** pp)
{
	drainDestroys();
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto b = std::make_unique<R::GxBuffer>();
	R::BufferDesc bd;
	bd.bytes = length;
	bd.usage = usage;
	bd.pool = uint32_t(pool);
	bd.index = true;
	bd.indexFormat = uint32_t(fmt);
	auto made = [&] {
		csLedger(m_csIssued, 3, { uint64_t(length), uint64_t(usage), uint64_t(fmt), uint64_t(pool) });
		GxNativeIndexBuffer* n = new GxNativeIndexBuffer(*this, std::move(b), usage, DWORD(fmt), pool);
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateIndexBuffer", b->init(m_gx, bd, nullptr), made())
	++m_stats.creates;
	std::string why;
	if (!b->init(m_gx, bd, &why)) {
		refuseCreate(("index buffer: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}

namespace {
// DecodeMultiSampleType (d3d9_util.cpp:41-65): 0/1 = none; anything else must be a power of two (else INVALIDCALL)
bool msaaOk(uint32_t t) { return t <= 1 || (t <= 16 && (t & (t - 1)) == 0); }
}

HRESULT GxNativeDevice::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, WINBOOL, IDirect3DSurface8** pp)
{
	drainDestroys();
	if (!pp || !msaaOk(uint32_t(ms)))
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto rt = std::make_unique<R::GxRenderTarget>();
	R::RenderTargetDesc rd;
	rd.width = w;
	rd.height = h;
	rd.format = uint32_t(fmt);
	rd.samples = uint32_t(ms);
	// render-hud-seam-1: the world/HUD seam's MSAA world colour is read by ResolveLinear; a device that cannot sample that count
	// gets the plain target (ResolveLinear then declines and the seam resolves with CopyRects)
	rd.sampled = m_worldNextColour && uint32_t(ms) > 1;
	if (rd.sampled)
		m_worldNextColour = false;
	auto made = [&] {
		csLedger(m_csIssued, 4, { uint64_t(w), uint64_t(h), uint64_t(fmt), uint64_t(ms) });
		GxNativeSurface* n = new GxNativeSurface(*this, GxNativeSurface::Kind::RenderTarget, std::move(rt), uint32_t(fmt), D3DUSAGE_RENDERTARGET);
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateRenderTarget", rt->init(m_gx, rd, nullptr), made())
	++m_stats.creates;
	std::string why;
	// render-hud-seam-1's fallback, inside A23-B's inline body: a sampled world colour the device cannot make -> the plain target
	bool ok = rt->init(m_gx, rd, &why);
	if (!ok && rd.sampled) {
		rt = std::make_unique<R::GxRenderTarget>();
		rd.sampled = false;
		ok = rt->init(m_gx, rd, &why);
	}
	if (!ok) {
		refuseCreate(("render target: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}

HRESULT GxNativeDevice::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8** pp)
{
	drainDestroys();
	if (!pp || !msaaOk(uint32_t(ms)))
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto rt = std::make_unique<R::GxRenderTarget>();
	R::RenderTargetDesc rd;
	rd.width = w;
	rd.height = h;
	rd.format = uint32_t(fmt);
	rd.samples = uint32_t(ms);
	rd.depthSampled = m_nextDepthSampled || m_debug.motionSampledAlways;   // the world depth only
	m_nextDepthSampled = false;
	auto made = [&] {
		csLedger(m_csIssued, 5, { uint64_t(w), uint64_t(h), uint64_t(fmt), uint64_t(ms) });
		GxNativeSurface* n = new GxNativeSurface(*this, GxNativeSurface::Kind::Depth, std::move(rt), uint32_t(fmt), D3DUSAGE_DEPTHSTENCIL);
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateDepthStencilSurface", rt->init(m_gx, rd, nullptr), made())
	++m_stats.creates;
	std::string why;
	if (!rt->init(m_gx, rd, &why)) {
		refuseCreate(("depth stencil: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}

HRESULT GxNativeDevice::CreateImageSurface(UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8** pp)
{
	drainDestroys();
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = nullptr;
	auto t = std::make_unique<R::GxTexture>();
	R::TextureDesc td;
	td.width = w;
	td.height = h;
	td.levels = 1;
	td.format = uint32_t(fmt);
	td.pool = R::d3d::POOL_SYSTEMMEM;
	auto made = [&] {
		csLedger(m_csIssued, 6, { uint64_t(w), uint64_t(h), uint64_t(fmt) });
		GxNativeSurface* n = new GxNativeSurface(*this, std::move(t));
		n->AddRef();
		*pp = n;
		return D3D_OK;
	};
	GX_CREATE_ATTEMPT("CreateImageSurface", t->init(m_gx, td, nullptr), made())
	++m_stats.creates;
	std::string why;
	if (!t->init(m_gx, td, &why)) {
		refuseCreate(("image surface: " + why).c_str());
		return D3DERR_INVALIDCALL;
	}
	return made();
}
#undef GX_CREATE_ATTEMPT

// ------------------------------------------------------------------------------------------------------------ copies

// Reads a rect of a surface's CURRENT content as tight rows (a GPU read-back for render targets, back buffers and dropped
// texture levels; the host bytes otherwise). An MSAA source is resolved first (DXVK resolves in GetRenderTargetData's blit).
bool GxNativeDevice::readSurface(GxNativeSurface* src, const VkRect2D& r, std::vector<uint8_t>* out)
{
	const R::FormatTable& ft = m_gx.formats();
	const uint32_t fmt = src->d3dFormat();
	if (src->kind() == GxNativeSurface::Kind::Image || (src->kind() == GxNativeSurface::Kind::TextureLevel &&
		src->texture()->gx().hasHostCopy(src->level()))) {
		const R::GxTexture& t = src->kind() == GxNativeSurface::Kind::Image ? *src->imageTexture() : src->texture()->gx();
		const uint32_t level = src->kind() == GxNativeSurface::Kind::Image ? 0 : src->level();
		const uint8_t* host = t.hostLevel(level);
		if (!host)
			return false;
		const uint32_t pitch = t.levelPitch(level), rowBytes = ft.rowBytes(fmt, r.extent.width), rows = ft.rows(fmt, r.extent.height);
		const R::FormatInfo* fi = ft.get(fmt);
		out->resize(size_t(rowBytes) * rows);
		for (uint32_t y = 0; y < rows; ++y)
			std::memcpy(out->data() + size_t(y) * rowBytes,
				host + size_t(uint32_t(r.offset.y) / fi->blockH + y) * pitch + size_t(uint32_t(r.offset.x) / fi->blockW) * fi->blockBytes,
				rowBytes);
		return true;
	}
	endFrameForHost();
	++m_stats.readbacks;
	if (src->kind() == GxNativeSurface::Kind::TextureLevel) {
		// a DXTn texture decoded on upload (no BC on this device) holds A8R8G8B8 texels on the GPU, not blocks: there is nothing
		// to copy into a block-layout destination - refused, as GxTexture::copyFrom refuses it
		if (src->texture()->gx().decoded() && !m_debug.copyDecodedAsBlocks) {
			if (m_stats.copiesRefused++ == 0)
				logLine("[GXNATIVE] REFUSED copy: a decoded DXT level has no blocks to read back");
			return false;
		}
		if (src->texture()->managed())
			src->texture()->pin();
		std::vector<uint8_t> level;
		if (!src->texture()->gx().readLevel(src->level(), &level))
			return false;
		const uint32_t w = src->width(), rowFull = ft.rowBytes(fmt, w), rowBytes = ft.rowBytes(fmt, r.extent.width);
		const uint32_t rows = ft.rows(fmt, r.extent.height);
		const R::FormatInfo* fi = ft.get(fmt);
		out->resize(size_t(rowBytes) * rows);
		for (uint32_t y = 0; y < rows; ++y)
			std::memcpy(out->data() + size_t(y) * rowBytes,
				level.data() + size_t(uint32_t(r.offset.y) / fi->blockH + y) * rowFull + size_t(uint32_t(r.offset.x) / fi->blockW) * fi->blockBytes,
				rowBytes);
		return true;
	}
	R::GxRenderTarget* rt = src->renderTarget();
	if (!rt)
		return false;
	R::GxImage* img = &rt->image();
	std::unique_ptr<R::GxRenderTarget> tmp;
	if (rt->samples() != VK_SAMPLE_COUNT_1_BIT) {
		tmp = std::make_unique<R::GxRenderTarget>();
		R::RenderTargetDesc rd;
		rd.width = img->width;
		rd.height = img->height;
		rd.format = fmt;
		std::string why;
		if (!tmp->init(m_gx, rd, &why))
			return false;
		const bool ok = m_gx.immediate([&](VkCommandBuffer cmd) { rt->resolveTo(cmd, tmp->image()); });
		++m_stats.resolves;
		if (!ok)
			return false;
		img = &tmp->image();
	}
	const bool ok = m_gx.readImage(*img, 0, &r, out);
	if (tmp) {
		forgetImage(&tmp->image());
		tmp->destroy();
	}
	return ok;
}

// Writes tight rows into a host-backed destination (MANAGED / SYSTEMMEM texture level or image surface) through its own lock,
// so the MANAGED upload, the pin rule and the dirty box behave as for any write.
bool GxNativeDevice::writeHost(GxNativeSurface* dst, const RECT& dr, const std::vector<uint8_t>& bytes)
{
	const R::FormatTable& ft = m_gx.formats();
	const uint32_t fmt = dst->d3dFormat(), rowBytes = ft.rowBytes(fmt, uint32_t(dr.right - dr.left));
	const uint32_t rows = ft.rows(fmt, uint32_t(dr.bottom - dr.top));
	if (bytes.size() < size_t(rowBytes) * rows)
		return false;
	D3DLOCKED_RECT lr = {};
	if (FAILED(dst->LockRect(&lr, &dr, 0)))
		return false;
	for (uint32_t y = 0; y < rows; ++y)
		std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, bytes.data() + size_t(y) * rowBytes, rowBytes);
	dst->UnlockRect();
	return true;
}

// One CopyRects rect, d3d8_device.cpp:740-980 by (dst pool, src pool) - DESIGN 7.1.
// A23-B §3.7: the case is chosen on the FRONT from the pools, kinds and front-owned host state. DEFAULT -> DEFAULT is A (the GPU
// copy recorded by the worker); a host source -> a DEFAULT texture level is A (today's readSurface host branch is the snapshot,
// taken now; the worker stages it); host -> host runs on the front through the destination's own Lock/Unlock; every read-back
// (a DEFAULT source into host memory, a dropped MANAGED source) is S - today's body inline with the worker idle.
HRESULT GxNativeDevice::copyRect(GxNativeSurface* src, const RECT& sr, GxNativeSurface* dst, const RECT& dr, bool asymmetric, bool stretch)
{
	const VkRect2D r = { { sr.left, sr.top }, { uint32_t(sr.right - sr.left), uint32_t(sr.bottom - sr.top) } };
	if (sr.left < 0 || sr.top < 0 || sr.right <= sr.left || sr.bottom <= sr.top || uint32_t(sr.right) > src->width() ||
		uint32_t(sr.bottom) > src->height() || dr.left < 0 || dr.top < 0 || uint32_t(dr.right) > dst->width() ||
		uint32_t(dr.bottom) > dst->height())
		return D3DERR_INVALIDCALL;
	const uint32_t dstPool = dst->pool(), srcPool = src->pool();
	const bool srcHost = src->kind() == GxNativeSurface::Kind::Image || (src->kind() == GxNativeSurface::Kind::TextureLevel &&
		src->texture()->gx().hasHostCopy(src->level()));
	std::unique_ptr<SScope> sync;
	if (dstPool == R::d3d::POOL_DEFAULT && srcPool == R::d3d::POOL_DEFAULT) {
		// StretchRect(src rect -> dst rect, D3DTEXF_NONE): CopyRects never stretches, so a copy, or a resolve of an MSAA source
		if (!src->frontHasImage() || !dst->frontHasImage() || stretch)
			return D3DERR_INVALIDCALL;
		// an MSAA destination: DXVK copies through a draw (StretchRect fbBlit, d3d9_device.cpp:1317); the same sample count is
		// a plain copy here, any other count is refused by name - no game caller copies INTO an MSAA surface (the pillarbox
		// resolves the MSAA scene into a single-sample texture and DRAWS it, dx8wrapper.cpp:483)
		if (dst->samples() != VK_SAMPLE_COUNT_1_BIT && dst->samples() != src->samples()) {
			if (m_stats.copiesRefused++ == 0)
				logLine("[GXNATIVE] REFUSED copy into an MSAA x%u surface from x%u", dst->samples(), src->samples());
			return D3DERR_INVALIDCALL;
		}
		const bool msaa = src->multisampled() && dst->samples() == VK_SAMPLE_COUNT_1_BIT;   // a resolve; equal counts copy
		if (msaa && m_debug.copySkipsResolve)
			return D3D_OK;
		cs([this, src, dst, r, dr, msaa] {
			csTrace(GxCsOp::CopyGpu, { static_cast<const void*>(src), static_cast<const void*>(dst), uint64_t(r.offset.x), uint64_t(r.offset.y),
				uint64_t(r.extent.width), uint64_t(r.extent.height), uint64_t(dr.left), uint64_t(dr.top), uint64_t(msaa) });
			VkRect2D rr = r;
			if (m_debug.csCopyGpuSquareExtent && m_cs.onWorker())   // the plant
				rr.extent.height = rr.extent.width;
			execCopyGpu(src, dst, rr, dr, m_debug.csCopyGpuMsaaFromSrc && m_cs.onWorker() ? src->multisampled() : msaa);   // (the plant)
		});
		if (dst->kind() == GxNativeSurface::Kind::TextureLevel)
			dst->texture()->noteWritten(dst->level());
		return D3D_OK;
	}
	if (dstPool == R::d3d::POOL_DEFAULT) {
		// MANAGED -> DEFAULT ("UpdateTextureFromBuffer") / SYSTEMMEM -> DEFAULT (UpdateSurface): the source's bytes uploaded
		if (dst->kind() != GxNativeSurface::Kind::TextureLevel)
			return D3DERR_INVALIDCALL;   // into a render-target surface: no live caller (census)
		if (srcHost) {
			std::vector<uint8_t> bytes;   // today's readSurface host branch, at the call (W3DShroud writes through pBits later)
			if (!readSurface(src, r, &bytes))
				return D3DERR_INVALIDCALL;
			const uint32_t fmt = src->d3dFormat();
			if (csDirect())
				return copyHostToDefaultInline(dst, fmt, r, dr, bytes.data());
			// copyFrom's remaining checks with the twin as the source: the twin is the whole SYSTEMMEM level of the
			// source's format at origin 0 - always made, always valid - so the destination rect decides; refused as today:
			// the level still noted written, the idle flush still at its position, INVALIDCALL
			R::GxTexture& t = dst->texture()->gx();
			if (m_debug.csCopyValidateSource && (r.offset.x % 4 || r.offset.y % 4) &&
				(fmt == uint32_t(D3DFMT_DXT1) || fmt == uint32_t(D3DFMT_DXT2) || fmt == uint32_t(D3DFMT_DXT3) || fmt == uint32_t(D3DFMT_DXT4) ||
					fmt == uint32_t(D3DFMT_DXT5)))
				return D3DERR_INVALIDCALL;   // the plant: the host source validated like a LockRect
			if (!m_debug.csCopySkipDestBlockCheck && !t.rectValid(dst->level(), { { int32_t(dr.left), int32_t(dr.top) }, r.extent })) {   // (the plant)
				if (m_debug.csFrontRefusalCounts)
					++m_stats.copiesRefused;   // the plant
				dst->texture()->noteWritten(dst->level());
				flushIdleFront();
				return D3DERR_INVALIDCALL;
			}
			if (bytes.size() > m_cs.maxPayload()) {
				if (m_debug.csCopyOversizeRefused)
					return D3DERR_INVALIDCALL;   // the plant
				if (m_debug.csOversizeLateDrain) {   // the plant: S entered after the inline exec
					const HRESULT hr = copyHostToDefaultInline(dst, fmt, r, dr, bytes.data());
					SScope late(*this, "CopyRects oversize");
					return hr;
				}
				SScope sc(*this, "CopyRects oversize");
				return copyHostToDefaultInline(dst, fmt, r, dr, bytes.data());
			}
			// a worker-side staging failure answers D3D_OK + resource_errors (named divergence c, §6)
			if (m_debug.csCopyLateRead) {   // the plant: the source's host bytes read when the worker runs it
				++m_csIssued;
				m_cs.push([this, src, dst, fmt, r, dr] {
					std::vector<uint8_t> late;
					readSurface(src, r, &late);
					execCopyHostToDefault(dst, fmt, r, dr, late.data());
					flushIdleUploads();
				});
				dst->texture()->noteWritten(dst->level());
				csAfter();
				return D3D_OK;
			}
			++m_csIssued;
			m_cs.push([this, dst, fmt, r, dr](const uint8_t* p, size_t) {
				RECT d = dr;
				if (m_debug.csCopyDestOrigin && m_cs.onWorker()) {   // the plant
					d.right -= d.left;
					d.bottom -= d.top;
					d.left = d.top = 0;
				}
				execCopyHostToDefault(dst, fmt, r, d, p);
				flushIdleUploads();
				publishPressure();
			}, bytes.data(), bytes.size());
			noteStagingCmd(t.rectStagingBytes({ { int32_t(dr.left), int32_t(dr.top) }, r.extent }));
			csAfter();
			dst->texture()->noteWritten(dst->level());
			return D3D_OK;
		}
	}
	// the rest reads the GPU unless the source has a host copy (host -> host runs on the front, through the destination's own
	// Lock/Unlock, whose halves are split already)
	if (!srcHost && !csDirect() && !(m_debug.csReadbackNarrowS && !(srcPool == R::d3d::POOL_DEFAULT && dstPool == R::d3d::POOL_SYSTEMMEM)))   // (plant)
		sync.reset(new SScope(*this, "CopyRects read-back"));
	if (dstPool == R::d3d::POOL_DEFAULT) {
		std::vector<uint8_t> bytes;
		if (!readSurface(src, r, &bytes))
			return D3DERR_INVALIDCALL;
		return copyHostToDefaultInline(dst, src->d3dFormat(), r, dr, bytes.data());
	}
	// dst MANAGED or SYSTEMMEM
	if (dstPool == R::d3d::POOL_SYSTEMMEM && srcPool == R::d3d::POOL_DEFAULT &&
		((src->usage() & D3DUSAGE_RENDERTARGET) || src == m_rt) && !src->multisampled() && src->width() == dst->width() &&
		src->height() == dst->height() && !asymmetric) {
		// d3d8_device.cpp:876-886 fast path: GetRenderTargetData of the WHOLE surface, whatever the rect
		if (m_debug.fastPathRectOnly) {
			std::vector<uint8_t> part;
			return readSurface(src, r, &part) && writeHost(dst, dr, part) ? D3D_OK : D3DERR_INVALIDCALL;
		}
		std::vector<uint8_t> bytes;
		const VkRect2D whole = { { 0, 0 }, { src->width(), src->height() } };
		const RECT wholeDst = { 0, 0, LONG(dst->width()), LONG(dst->height()) };
		return readSurface(src, whole, &bytes) && writeHost(dst, wholeDst, bytes) ? D3D_OK : D3DERR_INVALIDCALL;
	}
	if (srcPool != R::d3d::POOL_DEFAULT && stretch)
		return D3DERR_INVALIDCALL;   // copyTextureBuffers cannot stretch
	if (src->multisampled() && m_debug.copySkipsResolve)
		return D3D_OK;
	// DEFAULT -> host: StretchRect into the temp blit image + GetRenderTargetData (DXVK overwrites the dst outside the rect with
	// the temp's content; the device writes only the rect - DESIGN 7.1's named divergence). Host -> host: copyTextureBuffers.
	std::vector<uint8_t> bytes;
	if (!readSurface(src, r, &bytes))
		return D3DERR_INVALIDCALL;
	return writeHost(dst, dr, bytes) ? D3D_OK : D3DERR_INVALIDCALL;
}

// DEFAULT -> DEFAULT: today's copy / resolve, recorded outside the writer's scope (worker, or direct)
void GxNativeDevice::execCopyGpu(GxNativeSurface* src, GxNativeSurface* dst, const VkRect2D& r, const RECT& dr, bool msaa)
{
	R::GxImage* srcImg = src->image();
	R::GxImage* dstImg = dst->image();
	if (!srcImg || !dstImg)
		return;   // the front saw images (frontHasImage): a guard, not a path
	ensureFrame();
	// two levels of ONE texture share an image: one layout for both ends, GENERAL
	const bool alias = srcImg == dstImg && !m_debug.copyNoAliasLayout;
	R::GxImage* touched[2] = { srcImg, dstImg };
	m_writer.outsideScope([&](VkCommandBuffer cmd) {
		if (alias) {
			srcImg->transition(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
				VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
		} else {
			srcImg->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			dstImg->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		}
		// the source's own level: a level surface's image() is the whole texture
		const VkImageSubresourceLayers srcSub = { srcImg->aspect(), m_debug.copySrcMipZero ? 0u : src->level(), 0, 1 };
		const VkImageSubresourceLayers dstSub = { dstImg->aspect(), dst->level(), 0, 1 };
		if (msaa) {
			VkImageResolve rr = {};
			rr.srcSubresource = srcSub;
			rr.srcOffset = { r.offset.x, r.offset.y, 0 };
			rr.dstSubresource = dstSub;
			rr.dstOffset = { int32_t(dr.left), int32_t(dr.top), 0 };
			rr.extent = { r.extent.width, r.extent.height, 1 };
			vkCmdResolveImage(cmd, srcImg->image, srcImg->layout, dstImg->image, dstImg->layout, 1, &rr);
		} else {
			VkImageCopy c = {};
			c.srcSubresource = srcSub;
			c.srcOffset = { r.offset.x, r.offset.y, 0 };
			c.dstSubresource = dstSub;
			c.dstOffset = { int32_t(dr.left), int32_t(dr.top), 0 };
			c.extent = { r.extent.width, r.extent.height, 1 };
			vkCmdCopyImage(cmd, srcImg->image, srcImg->layout, dstImg->image, dstImg->layout, 1, &c);
		}
	}, touched, alias ? 1u : 2u);
	++(msaa ? m_stats.resolves : m_stats.gpuCopies);
}

// host bytes (tight rows of the source rect) -> a DEFAULT texture level through a SYSTEMMEM twin, today's copy (worker, or direct)
HRESULT GxNativeDevice::copyHostToDefaultInline(GxNativeSurface* dst, uint32_t fmt, const VkRect2D& r, const RECT& dr, const uint8_t* bytes)
{
	++m_csIssued;
	const int done = execCopyHostToDefault(dst, fmt, r, dr, bytes);
	csAfter();
	if (done < 0)
		return D3DERR_INVALIDCALL;
	dst->texture()->noteWritten(dst->level());
	flushIdleUploads();
	return done ? D3D_OK : D3DERR_INVALIDCALL;
}

int GxNativeDevice::execCopyHostToDefault(GxNativeSurface* dst, uint32_t fmt, const VkRect2D& r, const RECT& dr, const uint8_t* bytes)
{
	{
		const R::FormatTable& f = m_gx.formats();
		GxCsBytes tb;
		tb.p = bytes;
		tb.rowBytes = f.rowBytes(fmt, r.extent.width);
		tb.rows = f.rows(fmt, r.extent.height);
		tb.pitch = tb.rowBytes;
		csTrace(GxCsOp::CopyHost, { static_cast<const void*>(dst), uint64_t(fmt), uint64_t(r.extent.width), uint64_t(r.extent.height),
			uint64_t(dr.left), uint64_t(dr.top) }, &tb);
	}
	R::GxTexture twin;
	R::TextureDesc td;
	td.width = r.extent.width;
	td.height = m_debug.csHostCopyTwinSquare ? r.extent.width : r.extent.height;   // (the plant)
	const bool twinA8 = m_debug.csHostCopyTwinA8 || (m_debug.csHostCopyTwinPhysFmt && dst->texture()->gx().decoded());   // (the plants)
	td.format = twinA8 ? uint32_t(D3DFMT_A8R8G8B8) : fmt;
	td.pool = R::d3d::POOL_SYSTEMMEM;
	std::string why;
	R::LockedRect lr;
	const VkRect2D all = { { 0, 0 }, { td.width, td.height } };   // = r.extent unless planted
	if (!twin.init(m_gx, td, &why) || !twin.lockRect(0, &all, 0, &lr))
		return -1;
	const R::FormatTable& ft = m_gx.formats();
	// the source's rows (unplanted: the twin's are the same; a plant reads no further than the source holds)
	const uint32_t srcRowBytes = ft.rowBytes(fmt, r.extent.width), srcRows = ft.rows(fmt, r.extent.height);
	const uint32_t rowBytes = std::min(ft.rowBytes(td.format, td.width), srcRowBytes), rows = std::min(ft.rows(td.format, td.height), srcRows);
	for (uint32_t y = 0; y < rows; ++y)
		std::memcpy(static_cast<uint8_t*>(lr.bits) + size_t(y) * lr.pitch, bytes + size_t(y) * srcRowBytes, rowBytes);
	twin.unlockRect(0);
	return dst->texture()->gx().copyFrom(twin, 0, all, m_debug.csHostCopyTwinLevel0 ? 0u : dst->level(), { int32_t(dr.left), int32_t(dr.top) }) ? 1 : 0;   // (plant)
}

HRESULT GxNativeDevice::CopyRects(IDirect3DSurface8* srcI, const RECT* srcRects, UINT count, IDirect3DSurface8* dstI, const POINT* dstPoints)
{
	// d3d8_device.cpp:675-700: null or identical surfaces, a format mismatch and depth formats are INVALIDCALL
	if (!srcI || !dstI || srcI == dstI)
		return D3DERR_INVALIDCALL;
	GxNativeSurface* src = static_cast<GxNativeSurface*>(srcI);
	GxNativeSurface* dst = static_cast<GxNativeSurface*>(dstI);
	if (src->d3dFormat() != dst->d3dFormat())
		return D3DERR_INVALIDCALL;
	const R::FormatInfo* fi = m_gx.formats().get(src->d3dFormat());
	if (!fi || fi->depth)
		return D3DERR_INVALIDCALL;
	RECT whole;
	const POINT origin = { 0, 0 };
	if (!srcRects) {   // :702-711 the whole source at (0,0)
		whole = { 0, 0, LONG(src->width()), LONG(src->height()) };
		srcRects = &whole;
		dstPoints = &origin;
		count = 1;
	}
	if (count == 0)
		return D3DERR_INVALIDCALL;   // the loop never runs: :983
	// :713-738, and ONLY the first rect: every pool branch returns inside the loop
	const RECT sr = srcRects[0];
	RECT dr = sr;
	bool asymmetric = false, stretch = false;
	if (dstPoints) {
		dr = { dstPoints[0].x, dstPoints[0].y, dstPoints[0].x + (sr.right - sr.left), dstPoints[0].y + (sr.bottom - sr.top) };
		asymmetric = dr.left != sr.left || dr.top != sr.top || dr.right != sr.right || dr.bottom != sr.bottom;
		stretch = (dr.right - dr.left) != (sr.right - sr.left) || (dr.bottom - dr.top) != (sr.bottom - sr.top);
	}
	return copyRect(src, sr, dst, dr, asymmetric, stretch);
}

// d3d9_device.cpp:1100-1175 (D3D8 forwards): SYSTEMMEM -> DEFAULT only; mip alignment; the source's level-0 dirty box per mip.
// A23-B §3.5: S - no live caller (CENSUS §3c), today's body inline keeps its retry-on-failure dirty box.
HRESULT GxNativeDevice::UpdateTexture(IDirect3DBaseTexture8* srcB, IDirect3DBaseTexture8* dstB)
{
	SScope sc(*this, "UpdateTexture");
	if (!srcB || !dstB || srcB->GetType() != D3DRTYPE_TEXTURE || dstB->GetType() != D3DRTYPE_TEXTURE)
		return D3DERR_INVALIDCALL;
	GxNativeTexture* src = static_cast<GxNativeTexture*>(static_cast<IDirect3DTexture8*>(srcB));
	GxNativeTexture* dst = static_cast<GxNativeTexture*>(static_cast<IDirect3DTexture8*>(dstB));
	R::GxTexture& s = src->gx();
	R::GxTexture& t = dst->gx();
	if (s.desc().pool != R::d3d::POOL_SYSTEMMEM || t.desc().pool != R::d3d::POOL_DEFAULT || s.levels() < t.levels() ||
		s.desc().format != t.desc().format)
		return D3DERR_INVALIDCALL;
	const uint32_t mips = t.levels();
	uint32_t srcMipOffset = 0;
	if (s.desc().width != t.desc().width || s.desc().height != t.desc().height)
		srcMipOffset = s.levels() - mips;   // match the source's LOWER levels to the destination's top
	if (s.levelWidth(srcMipOffset) != t.desc().width || s.levelHeight(srcMipOffset) != t.desc().height)
		return D3DERR_INVALIDCALL;
	const RECT box = m_debug.updateIgnoresDirty ? RECT { 0, 0, LONG(s.desc().width), LONG(s.desc().height) } : src->dirty();
	if (box.right > box.left && box.bottom > box.top) {
		for (uint32_t dm = 0; dm < mips; ++dm) {
			const uint32_t sm = dm + srcMipOffset;
			// util::computeMipLevelOffset / computeMipLevelExtent on the level-0 box at the SOURCE mip, then
			// UpdateTextureFromBuffer's alignment (d3d9_device.cpp:5248-5264): offset down to the block grid, extent up to whole
			// blocks, snapped to the level (a DXT box {4,4,8,8} is offset 2,2 at mip 1)
			uint32_t x = uint32_t(box.left) >> sm, y = uint32_t(box.top) >> sm;
			uint32_t w = std::max(1u, uint32_t(box.right - box.left) >> sm), h = std::max(1u, uint32_t(box.bottom - box.top) >> sm);
			if (!m_debug.updateNoBlockAlign) {
				const R::FormatInfo* fi = m_gx.formats().get(s.desc().format);
				const uint32_t bw = fi ? fi->blockW : 1, bh = fi ? fi->blockH : 1;
				const uint32_t ax = x / bw * bw, ay = y / bh * bh;
				w = (w + (x - ax) + bw - 1) / bw * bw;
				h = (h + (y - ay) + bh - 1) / bh * bh;
				x = ax;
				y = ay;
			}
			const VkRect2D rr = { { int32_t(x), int32_t(y) }, { std::min(w, s.levelWidth(sm) - x), std::min(h, s.levelHeight(sm) - y) } };
			if (m_debug.updateFailLastMip && dm + 1 == mips)
				return D3DERR_INVALIDCALL;   // TEST: the last mip fails after the others were staged (today's failure return)
			if (!t.copyFrom(s, sm, rr, dm, { int32_t(x), int32_t(y) }))
				return D3DERR_INVALIDCALL;
			dst->noteWritten(dm);
		}
	}
	src->clearDirty();
	flushIdleUploads();
	return D3D_OK;
}

HRESULT GxNativeDevice::GetFrontBuffer(IDirect3DSurface8*) { return neverCalled("GetFrontBuffer"); }

// ------------------------------------------------------------------------------------------------------------ targets

HRESULT GxNativeDevice::SetRenderTarget(IDirect3DSurface8* rtI, IDirect3DSurface8* dsI)
{
	// d3d8_device.cpp SetRenderTarget: a null colour target keeps the current one; the depth surface is always replaced and
	// must not be smaller than the colour target. D3D9's SetRenderTarget(0) resets the viewport to the whole target.
	GxNativeSurface* rt = static_cast<GxNativeSurface*>(rtI);
	GxNativeSurface* ds = static_cast<GxNativeSurface*>(dsI);
	GxNativeSurface* colour = rt ? rt : m_rt;
	if (colour && ds && (ds->width() < colour->width() || ds->height() < colour->height())) {
		// DESIGN §16f residual (accepted on condition that it is counted and stays 0 in the game): DXVK binds the new colour target
		// before this refusal, the device changes nothing
		if (!m_debug.targetSizeRefusalUncounted && m_stats.targetSizeRefused++ < 4)
			logLine("[GXNATIVE] REFUSED SetRenderTarget: depth %ux%u under a %ux%u target (DXVK would bind the target first; DESIGN 16f)",
				ds->width(), ds->height(), colour->width(), colour->height());
		return D3DERR_INVALIDCALL;
	}
	if (rt)
		bindSurface(m_rt, rt);
	bindSurface(m_ds, ds);
	if (rt) {   // a colour target resets the viewport to the whole colour IMAGE, 0x0 without one (GxD3D8Translate::renderTarget)
		const bool img = frontColourHasImage();
		m_mirror.viewport = { 0, 0, img ? m_rt->width() : 0u, img ? m_rt->height() : 0u, 0.f, 1.f };
	}
	// the worker keeps its own copy of the binding: the command names the surfaces, the front's private references keep them
	// alive until a later command (§3.3)
	cs([this, rt, ds] {
		csTrace(GxCsOp::SetTarget, { static_cast<const void*>(rt), static_cast<const void*>(ds) });
		if (rt)
			m_wRt = rt;
		m_wDs = ds;
		syncTarget(rt != nullptr);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetRenderTarget(IDirect3DSurface8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	*pp = m_rt;
	if (m_rt)
		m_rt->AddRef();
	return m_rt ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT GxNativeDevice::GetDepthStencilSurface(IDirect3DSurface8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	if (m_ds && m_ds == m_autoDepth && m_debug.implicitNotLosable)
		m_ds->testUncountLosable();
	*pp = m_ds;
	if (m_ds)
		m_ds->AddRef();
	return m_ds ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT GxNativeDevice::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface8** pp)
{
	if (!pp || index >= m_bbSurfaces.size())
		return D3DERR_INVALIDCALL;
	if (m_debug.implicitNotLosable)
		m_bbSurfaces[index]->testUncountLosable();
	*pp = m_bbSurfaces[index];
	(*pp)->AddRef();
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ scene

HRESULT GxNativeDevice::BeginScene()
{
	if (m_inScene)
		return D3DERR_INVALIDCALL;
	m_inScene = true;
	slSceneBegin();   // no-op without Streamline
	return D3D_OK;
}

HRESULT GxNativeDevice::EndScene()
{
	if (!m_inScene)
		return D3DERR_INVALIDCALL;
	m_inScene = false;
	if (m_csOn)
		m_cs.publish();
	return D3D_OK;
}

HRESULT GxNativeDevice::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	++m_stats.clears;
	static_assert(sizeof(D3DRECT) == 16, "D3DRECT = 4 x LONG (32-bit)");
	// §3.2: today reads the rects only with a colour image bound and !(rects && count == 0); pointer presence travels apart
	const bool present = rects != nullptr && !(m_debug.csClearNullify && count == 0);
	const size_t bytes = present && count > 0 && frontColourHasImage() ? size_t(count) * (m_debug.csClearRectShort && !csDirect() ? 4 : 16) : 0;
	std::unique_ptr<SScope> sync;
	if (!csDirect() && bytes > m_cs.maxPayload())
		sync.reset(new SScope(*this, "Clear oversize"));
	csCopy([this, count, present, flags, color, z, stencil](const uint8_t* p, size_t n) {
		const GxCsBytes b = csFlat(p, n);
		csTrace(GxCsOp::Clear, { uint64_t(count), uint64_t(present), uint64_t(flags), uint64_t(color), csBits(z), uint64_t(stencil) }, &b);
		ensureFrame();
		m_tr.clear(count, present ? reinterpret_cast<const int32_t*>(p) : nullptr, flags, color, z, stencil);
	}, rects, bytes);
	if (m_csOn)
		m_cs.publish();
	return D3D_OK;
}

HRESULT GxNativeDevice::Present(const RECT*, const RECT*, HWND, const RGNDATA*)
{
	m_nextDepthSampled = false;   // NextTargetsAreWorld lives for the frame that announced it
	m_sceneTapped = false;        // OpaqueDone may run once in the next frame
	struct AfterPresent   // §3.4 mirror-check mode: after each Present, whichever way it returns
	{
		GxNativeDevice& d;
		~AfterPresent() { d.mirrorCheck("Present"); }
	} after { *this };
	// d3d9_swapchain.cpp:116-117: a lost / NotReset device presents nothing (a failed Reset can leave a swapchain without back
	// buffers)
	if (m_notReset && !m_debug.presentWhileNotReset)
		return D3DERR_DEVICELOST;
	if (csDirect()) {
		const HRESULT hr = presentDirect();
		csAfter();
		return hr;
	}
	// A23-B §3.9, in today's order. 1: lost -> PresentLost (the worker's lostTerminal: the open frame ended, FATAL once), fenced
	// like any Present, DEVICELOST
	const bool lost = (m_presentDead || m_presentLost || m_ctx.deviceLost()) && !m_debug.presentIgnoresDeviceLoss &&
		!m_debug.csLossUnpublished;
	if (lost) {
		const uint64_t prev = m_lastPresentSeq;
		++m_csIssued;
		m_cs.push([this] {
			csTrace(GxCsOp::PresentLost, {});
			lostTerminal();
			publishPressure();
		});
		m_lastPresentSeq = m_cs.queued();
		if (!m_debug.csLostUnfenced)
			m_cs.waitFor(prev);
		csAfter();
		return D3DERR_DEVICELOST;
	}
	// 3-4: the drawable size on the CALLER (SDL wants main); any recovery condition -> S: today's whole Present inline, the caller
	// the producer (recreation, surfaces and the retry acquire never run on the worker)
	uint32_t w = m_pp.BackBufferWidth, h = m_pp.BackBufferHeight;
	if (m_haveSwapchain) {
		if (m_host.drawableSize && !m_debug.ignoreDrawableSize) {
			m_host.drawableSize(m_pp.hDeviceWindow ? m_pp.hDeviceWindow : m_focus, &w, &h);
			w += m_debug.drawableSizeOffset;
			h += m_debug.drawableSizeOffset;
		}
		const bool resized = m_debug.resizeComparesActualExtent ? (m_swapchain.extent().width != w || m_swapchain.extent().height != h)
		                                                        : (w != m_swapReqW || h != m_swapReqH);
		if (!m_swapchain.handle() || m_swapchainDirty || m_surfaceLost || resized || m_debug.injectCreateSurfaceLost) {
			HRESULT hr;
			{
				SScope sc(*this, "Present recovery");
				hr = presentDirect();
			}
			csAfter();
			return hr;
		}
	}
	// 2 + 5: the front's part, then the Present command (the worker runs today's body from ensureFrame to collect)
	if (m_debug.csRaceRot)
		m_raceSink = m_raceSink + m_rot;   // the plant: worker state read on the front (TSan must report it)
	drainDestroys();
	dropManaged();
	++m_stats.presents;
	const FrontCounters fc = frontCounters();
	const uint64_t prev = m_lastPresentSeq;
	++m_csIssued;
	m_cs.push([this, w, h, fc] {
		if (m_debug.csPresentTransposed)   // the plant
			return execPresent(h, w, fc);
		execPresent(w, h, fc);
	});
	m_cs.publish();
	m_lastPresentSeq = m_cs.queued();
	// 6: the lead fence - Present N returns once the worker FINISHED Present N-1 (a frame identity)
	if (!m_debug.csNoLead)
		m_cs.waitFor(prev);
	csAfter();
	// 7: a loss latched by now is answered now (one seen later reaches the game at the next Present: divergence a, §6)
	if (!m_debug.presentLateLossOk && !m_debug.presentIgnoresDeviceLoss && !m_debug.csLossUnpublished &&
		(m_presentDead || m_presentLost || m_ctx.deviceLost()))
		return D3DERR_DEVICELOST;
	return D3D_OK;
}

GxNativeDevice::FrontCounters GxNativeDevice::frontCounters()
{
	FrontCounters f;
	copyFrontStats(f.s, m_stats);   // never the worker's fields: it writes them now
	f.cs = m_cs.stats();
	f.syncs = m_csSyncs;
	f.createRetries = m_csCreateRetries;
	const uint64_t frame = f.cs.bytes - m_bytesAtPresent;
	m_bytesAtPresent = f.cs.bytes;
	m_frameBytesMax = std::max(m_frameBytesMax, frame);
	f.frameBytesMax = m_frameBytesMax;
	if (m_stats.presents % kStatsEveryPresents == 0) {   // this Present's line prints: its S reasons and the window's max
		syncTopInto(f.syncTop, sizeof(f.syncTop), false);
		syncTopInto(f.drainTop, sizeof(f.drainTop), true);
		m_frameBytesMax = 0;
		logSeamDrains(m_stats.presents);   // front state, printed here (the worker prints the stats line later)
		logMotionCounters(m_stats.presents);   // front state too; only while motion is initialised
	}
	return f;
}

// WORKER: today's Present body from ensureFrame to collect (§3.9 step 5)
void GxNativeDevice::execPresent(uint32_t w, uint32_t h, const FrontCounters& f)
{
	// today's Present starts with the terminal-loss check: a loss latched by the Present queued before this one (a lost
	// submission after its acquire, a device loss) stops this one too - it never acquires on a slot whose semaphore is still
	// signalled (VUID-vkAcquireNextImageKHR-semaphore-01286, found by present-submit-fail under GX_NATIVE_CS=1)
	if (lostTerminal()) {
		csTrace(GxCsOp::PresentLost, {});
		publishPressure();
		return;
	}
	csTrace(GxCsOp::Present, {});
	ensureFrame();
	if (m_debug.loseDeviceMidPresent) {   // TEST fault injection: the loss latched by the frame-ring slot wait inside ensureFrame
		m_debug.loseDeviceMidPresent = false;
		m_ctx.simulateDeviceLost();
	}
	if (m_sceneInit)
		m_scene.presented(m_writer);   // this frame's copies end; the next frame's world scopes resolve again
	if (m_haveSwapchain)
		presentToSwapchain(w, h);   // its DEVICELOST answers are latched (m_presentDead / m_presentLost / the context)
	else
		endOffscreenFrame();
	rotateBackBuffers();
	syncTarget();   // a back buffer bound as the target now has other storage
	if (f.s.presents % kStatsEveryPresents == 0) {   // printed when the worker reaches it (divergence d, §6)
		logStatsLine("periodic", nullptr, &f);
		logSceneStats();
		if (m_gpuOn)
			logGpu(f.s.presents);   // the count the front captured (never the live front counter)
		if (m_shadowMapOn)
			shadowMapReport(f.s.presents);   // S7
		if (m_a2cOn)
			a2cReport(f.s.presents);   // (A108)
		if (m_host.waitStats)   // A30 [GXWAIT] under the worker: the ring and present-slot waits are the worker's own (it runs
			// ensureFrame and presentToSwapchain), the presents label is the front's captured count, as the lines above
			logLine("[GXWAIT] presents=%llu fif=%u ring_us=%llu present_us=%llu", (unsigned long long)f.s.presents, m_fif,
				(unsigned long long)m_writer.ringWaitMicros(), (unsigned long long)m_presentWaitUs);
	}
	m_ctx.collect();
	publishPressure();
}

// Today's Present body: direct (CS=0), or the S recovery with the worker idle
HRESULT GxNativeDevice::presentDirect()
{
	if (lostTerminal()) {
		++m_csIssued;
		csTrace(GxCsOp::PresentLost, {});
		return D3DERR_DEVICELOST;
	}
	drainDestroys();
	++m_csIssued;
	csTrace(GxCsOp::Present, {});
	ensureFrame();
	if (m_debug.loseDeviceMidPresent) {   // TEST fault injection: the loss latched by the frame-ring slot wait inside ensureFrame
		m_debug.loseDeviceMidPresent = false;
		m_ctx.simulateDeviceLost();
	}
	dropManaged();
	if (m_sceneInit)
		m_scene.presented(m_writer);   // this frame's copies end; the next frame's world scopes resolve again
	HRESULT hr = D3D_OK;
	slPresentBegin();   // the HUD-less tag + PCL render-submit end (no-op without Streamline)
	if (m_haveSwapchain)
		hr = presentToSwapchain(0, 0);   // PCL present start / end: in its present hook (FG P0 H3)
	else
		endOffscreenFrame();
	rotateBackBuffers();
	syncTarget();   // a back buffer bound as the target now has other storage
	++m_stats.presents;
	slFrameEnd();   // DLSS-G state for the frame just presented, then the next frame's Reflex sleep
	if (m_stats.presents % kStatsEveryPresents == 0) {
		logStats("periodic");
		logSceneStats();
		logMotionCounters(m_stats.presents);   // the direct path too (GX_NATIVE_CS=0: arm X) - review seam2 code
		if (m_gpuOn)
			logGpu(m_stats.presents);
		if (m_shadowMapOn)
			shadowMapReport(m_stats.presents);   // S7
		if (m_a2cOn)
			a2cReport(m_stats.presents);   // (A108)
		if (m_host.waitStats)   // A30: its own line (the integrator: the [GXNATIVE] stats line is shared by other branches)
			logLine("[GXWAIT] presents=%llu fif=%u ring_us=%llu present_us=%llu", (unsigned long long)m_stats.presents, m_fif,
				(unsigned long long)m_writer.ringWaitMicros(), (unsigned long long)m_presentWaitUs);
	}
	m_ctx.collect();
	// a loss latched DURING this Present (the slot wait in ensureFrame, a submission, a present-slot wait) is terminal at once - never
	// one more D3D_OK from the "frame not shown" skip or the offscreen path (#5c)
	if (!m_debug.presentLateLossOk && lostTerminal())
		return D3DERR_DEVICELOST;
	return hr;
}

// DESIGN §8.6: a lost Vulkan device (VkCtx latched VK_ERROR_DEVICE_LOST at a submit or wait) or a present submission lost after its
// acquire is terminal - every Present answers DEVICELOST, offscreen too, "[GXNATIVE] FATAL" once. The open writer frame is ended
// each time, so the game's draws after the loss never grow one command buffer forever (-a). Checked when
// Present starts AND before it returns.
bool GxNativeDevice::lostTerminal()
{
	if (!(m_presentDead || m_presentLost || m_ctx.deviceLost()) || m_debug.presentIgnoresDeviceLoss)
		return false;
	if (!m_lostLogged.exchange(true)) {
		const V::SubmitRefusal rf = m_ctx.refusal();   // R19: the refused submit, named (integrator condition 3)
		if (rf.value)
			logLine("[GXNATIVE] FATAL device lost: presentation stopped (DEVICELOST) - refused submit %s value=%llu VkResult %d",
				rf.label ? rf.label : "?", (unsigned long long)rf.value, int(rf.result));
		else
			logLine("[GXNATIVE] FATAL device lost: presentation stopped (DEVICELOST)");
	}
	if (m_writer.frameOpen())
		m_writer.endFrame();
	return true;
}

// The present target for this swapchain extent, made BEFORE the acquire: an acquired image must reach the submit that waits on
// its semaphore, so nothing that can fail may sit between them. shader: DXVK's present shader needed.
bool GxNativeDevice::ensurePresentTarget(const R::GxImage& src, bool* shader, bool* direct, std::string* why)
{
	const VkExtent2D ext = m_swapchain.extent();
	*shader = src.samples != VK_SAMPLE_COUNT_1_BIT || ext.width != src.width || ext.height != src.height;
	// rw3-W lever (2) W-a (A20): a 1:1 present draws DXVK's present shader straight into the swapchain image - no
	// window-sized target, no clear, no blit. Same shader, same spec constants, same format: the same bytes. The format test is
	// defensive (nativeBuildable admits only 8888 back buffers, VkSwapchain only 8-bit RGBA layouts): its one reachable else, a BGRA
	// back buffer on a non-BGRA swapchain (MoltenVK offers BGRA first), takes the old path. The vkCmdResolveImage / LINEAR-blit test
	// plants keep the old path too; so does anything not 1:1 or without a view and pipeline. And the old path's own 1:1 fit must
	// cover the image: pillarboxFit's float aspect truncates one row at ~6% of sizes (3840x1600 -> 1599: the old path squashes the
	// frame by a row and leaves a black one); there the old path stays, byte for byte.
	const V::FitRect fit = V::pillarboxFit(ext.width, ext.height, ext.width, ext.height);
	const bool fitCovers = fit.x == 0 && fit.y == 0 && fit.w == ext.width && fit.h == ext.height;
	*direct = (m_debug.presentDirectAnyExtent || (ext.width == src.width && ext.height == src.height && (fitCovers ||
		m_debug.presentDirectIgnoresFit))) && !m_debug.presentDirectOff &&
		!m_debug.presentResolveAverages && !m_debug.presentBlitLinear && src.format && src.format->vk == m_swapchain.format() &&
		// FG P0 H3 (rw3-W ruling 1): it draws INTO the swapchain image - COLOR_ATTACHMENT must be PROVEN (viewsReady alone is not: a
		// Streamline proxy's view is made whatever its usage); with DLSS-G loaded it never is, so FG keeps the old (blit) path
		(m_swapchain.hasUsage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || m_host.testPresentDirectIgnoresUsage) &&
		m_swapchain.viewsReady() && m_present.directReady(m_swapchain.format(), src.samples);
	if (*direct)
		return true;
	if (!*shader || (m_presentImg && m_presentImg->image().width == ext.width && m_presentImg->image().height == ext.height))
		return true;
	if (m_presentImg) {
		forgetImage(&m_presentImg->image());
		m_presentImg.reset();
	}
	if (m_debug.presentTargetFailsOnce) {
		m_debug.presentTargetFailsOnce = false;
		*why = "planted allocation failure";
		return false;
	}
	auto t = std::make_unique<R::GxRenderTarget>();
	R::RenderTargetDesc rd;
	rd.width = ext.width;
	rd.height = ext.height;
	rd.format = m_pp.BackBufferFormat;
	if (!t->init(m_gx, rd, why))
		return false;
	m_presentImg = std::move(t);
	return true;
}

// DXVK's sequence (d3d9_swapchain.cpp:1318-1352, dxvk_presenter.cpp:80-135): the window's size is read
// every Present and a change - or no swapchain after a failed recreation - recreates it BEFORE acquiring; an acquire that fails
// (OutOfDate / Error) recreates and retries once; a recreation that fails skips the frame and is retried at the next Present.
// A Suboptimal acquire is presented (its image is acquired) and the swapchain recreated before the next acquire.
// A lost VkSurfaceKHR is destroyed with its swapchain and made again on the same window (DXVK's presenter,
// dxvk_presenter.cpp:507-523). false: the new surface could not be made - m_surfaceLost stays set, the next attempt retries.
bool GxNativeDevice::replaceSurface(std::string* why)
{
	if (m_cs.onWorker())
		++m_wsiOnWorker;   // §3.9: never on the worker (arm 13 asserts 0)
	m_swapchain.shutdown();
	if (m_surface && m_host.destroySurface)
		m_host.destroySurface(m_ctx.instance(), m_surface);
	m_surface = VK_NULL_HANDLE;
	bool ok = false;
	if (m_debug.failNextSurfaceCreate)   // TEST fault injection: a transient surface creation failure
		m_debug.failNextSurfaceCreate = false;
	else
		ok = m_host.createSurface && m_host.createSurface(m_pp.hDeviceWindow ? m_pp.hDeviceWindow : m_focus, m_ctx.instance(), &m_surface);
	if (!ok) {
		*why = "window surface recreation failed";
		return false;
	}
	++m_stats.surfacesRecreated;
	m_surfaceLost = false;
	m_testSurfaceLost = false;
	return true;
}

// The swapchain on the window surface (created, or recreated when one exists) at the requested size. A lost surface is replaced
// first; a creation that fails on a surface that turns out LOST (its capability query / vkCreateSwapchainKHR answered
// SURFACE_LOST, which VkSwapchain reports as false) replaces it and builds once more, as DXVK's recreateSwapChain
// (dxvk_presenter.cpp:507-523). A lost surface stays lost, so asking it classifies. Device creation,
// Reset and the per-Present recreation all come here (Reset kept the lost surface).
bool GxNativeDevice::buildOnSurface(uint32_t w, uint32_t h, bool fifo, uint32_t frames, std::string* why)
{
	if (m_cs.onWorker())
		++m_wsiOnWorker;
	bool ok = false;
	for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
		if (m_surfaceLost && !m_debug.surfaceLostNotRecovered && !replaceSurface(why))
			return false;
		if (m_testSurfaceLost) {   // TEST fault injection: creation on the lost surface fails, the old swapchain gone
			m_swapchain.shutdown();
			*why = "(test) swapchain creation on a lost surface";
		} else if (m_swapchain.handle())
			ok = m_swapchain.recreate(w, h, fifo, why);
		else {
			V::SwapchainDesc sd;
			sd.surface = m_surface;
			sd.width = w;
			sd.height = h;
			sd.fullscreen = fifo;
			sd.tearFree = int8_t(m_host.tearFree);
			sd.framesInFlight = frames;
			sd.interposed = swapchainInterposed();   // FG P0 H3: with DLSS-G loaded the images are Streamline proxies - no usage proven
			sd.testDropUsage = m_host.testSwapchainDropUsage;
			sd.testViewsWithoutUsage = m_host.testPresentDirectIgnoresUsage || m_host.testSwapchainViewsWithoutUsage;
			ok = m_swapchain.init(m_ctx, sd, why);
			if (ok && sd.interposed && !m_interposedLogged) {
				m_interposedLogged = true;
				logLine("[GXNATIVE] swapchain images may be Streamline proxies (DLSS-G loaded): one-pass present and present read-back off "
					"- their usage is unproven (FG P0 H3, rw3-W ruling 1)");
			}
		}
		if (ok || m_surfaceLost || m_debug.createLossUnclassified || !surfaceIsLost())
			break;
		m_surfaceLost = true;
	}
	if (ok) {
		m_swapReqW = w;
		m_swapReqH = h;
	}
	return ok;
}

bool GxNativeDevice::surfaceIsLost() const
{
	if (m_testSurfaceLost)
		return true;
	VkSurfaceCapabilitiesKHR caps;
	return m_surface && vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.physicalDevice(), m_surface, &caps) == VK_ERROR_SURFACE_LOST_KHR;
}

HRESULT GxNativeDevice::presentToSwapchain(uint32_t cmdW, uint32_t cmdH)
{
	const bool guard = m_cs.onWorker();   // A23-B §3.9 step 5: the worker never recreates - it ends the frame unshown
	// R19: the previous present ran on the submit thread - its answer is applied here, before anything touches the swapchain,
	// where today's inline handling left its state
	V::VkSwapchain::Status prev = m_swapchain.takePresentStatus();
	if (m_debug.presentDropsPresentStatus)
		prev = V::VkSwapchain::Status::Ok;   // the plant: the answer is taken and ignored
	applyPresentStatus(prev);
	if (prev == V::VkSwapchain::Status::DeviceLost) {
		if (guard)
			m_presentLost = true;
		m_writer.endFrame();
		return D3DERR_DEVICELOST;
	}
	const uint32_t slot = m_presentSlot;
	m_capW = m_capH = 0;   // TESTS: set again only by a present that records its capture
	if (m_presentWaits[slot])
		waitTimed(m_presentWaits[slot], m_host.waitStats ? &m_presentWaitUs : nullptr);
	auto skip = [&](const char* what, const std::string& why) {
		if (m_stats.presentsSkipped++ < 16)
			logLine("[GXNATIVE] frame not shown: %s%s%s", what, why.empty() ? "" : ": ", why.c_str());
		m_writer.endFrame();   // the frame still runs (its uploads and readers), it is just not shown
		return D3D_OK;
	};
	const bool fifo = m_host.presentInterval >= 0 ? m_host.presentInterval > 0 : (!m_pp.Windowed || m_pp.SwapEffect == D3DSWAPEFFECT_COPY_VSYNC);
	uint32_t w = m_pp.BackBufferWidth, h = m_pp.BackBufferHeight;
	if (guard && !m_debug.csWorkerDrawable) {   // the caller read it (the worker calls no host callback but the log)
		w = cmdW;
		h = cmdH;
	} else if (m_host.drawableSize && !m_debug.ignoreDrawableSize) {
		m_host.drawableSize(m_pp.hDeviceWindow ? m_pp.hDeviceWindow : m_focus, &w, &h);
		w += m_debug.drawableSizeOffset;   // TEST fault injection (0 in the product)
		h += m_debug.drawableSizeOffset;
	}
	// a swapchain that is gone (a failed recreation destroys the old one) or a lost surface is rebuilt from scratch - with a new
	// VkSurfaceKHR for a lost one, as DXVK's presenter does (dxvk_presenter.cpp:513)
	auto recreate = [&](std::string* why) {
		if (guard && !m_debug.csRecreateOnWorker) {   // the execution-time guard: flags stay set, the frame is not shown, the caller's next Present recovers (S)
			++m_csPresentRecovery;
			m_swapchainDirty = true;   // an OutOfDate / Error acquire sets no flag of its own
			*why = "recovery left to the next Present";
			return false;
		}
		if (m_recreateDead)
			return false;
		slBeforeRecreate();   // FG P0 H3: frame generation off before the swapchain changes (no-op unless it is on)
		endFrameForHost();   // recreation waits for the device; the frame continues in a new writer frame
		bool ok = true;
		if (m_debug.failNextRecreate) {   // a real failure's state: the old swapchain destroyed, none created
			m_debug.failNextRecreate = false;
			m_swapchain.shutdown();
			*why = "planted recreation failure";
			ok = false;
		} else
			ok = buildOnSurface(w, h, fifo, uint32_t(m_presentWaits.size()), why);
		slAfterRecreate();   // FG P0 H3: frame generation comes back only after a stable window
		if (ok) {
			++m_stats.swapchainsRecreated;
			m_testRecreated = m_csOn;
		}
		if (!ok && m_debug.noRecreateRetry)
			m_recreateDead = true;
		if (m_debug.failFrameInRecreate)   // TEST fault injection: the replacement writer frame cannot begin (an allocation error)
			m_debug.failFrameInRecreate = false;
		else if (m_debug.failFrameInRecreateAfter && --m_debug.failFrameInRecreateAfter == 0)
			;   // the same injection on a later recreation (A23-B: the retry recreation inside an S Present)
		else
			ensureFrame();
		return ok;
	};
	if (m_debug.injectCreateSurfaceLost) {
		m_debug.injectCreateSurfaceLost = false;
		m_testSurfaceLost = true;
		m_swapchainDirty = true;
	}
	std::string why;
	// a resize = the window's size differs from the size the swapchain was last REQUESTED at: Vulkan may clamp the actual extent
	// (currentExtent, min/max image extent), and comparing with that recreated on every Present
	const bool resized = m_debug.resizeComparesActualExtent ? (m_swapchain.extent().width != w || m_swapchain.extent().height != h)
	                                                        : (w != m_swapReqW || h != m_swapReqH);
	if (!m_swapchain.handle() || m_swapchainDirty || m_surfaceLost || resized) {
		if (!recreate(&why))
			return skip("swapchain recreation", why);
		m_swapchainDirty = false;
	}
	if (!m_writer.frameOpen())
		return skip("frame", "the writer could not begin a frame");   // before the acquire
	R::GxRenderTarget* bb = backBufferStorage(0);
	if (!bb)   // never reached while NotReset (Present answers DEVICELOST first); a guard, not a path
		return skip("back buffer", "no storage");
	R::GxImage* src = &bb->image();
	bool shader = false, direct = false;
	bool capture = m_debug.capturePresent && m_swapchain.canReadBack() && ensureCapture();   // TESTS, before the acquire
	if (!m_debug.targetCheckAfterAcquire && !ensurePresentTarget(*src, &shader, &direct, &why))
		return skip("present target", why);
	uint32_t image = 0;
	V::VkSwapchain::Status st = V::VkSwapchain::Status::SurfaceLost;
	if (m_debug.injectSurfaceLost)   // TEST fault injection: reported before any image is acquired
		m_debug.injectSurfaceLost = false;
	else
		st = m_swapchain.acquire(slot, &image);
	if (st == V::VkSwapchain::Status::SurfaceLost)
		m_surfaceLost = true;
	if (st == V::VkSwapchain::Status::OutOfDate || st == V::VkSwapchain::Status::Error || st == V::VkSwapchain::Status::SurfaceLost) {
		if (!recreate(&why))
			return skip("swapchain recreation", why);
		if (!m_writer.frameOpen() && !m_debug.retryAcquireSkipsFrameCheck)
			return skip("frame", "the writer could not begin a frame");   // before EVERY acquire
		if (!m_debug.targetCheckAfterAcquire && !ensurePresentTarget(*src, &shader, &direct, &why))
			return skip("present target", why);
		capture = m_debug.capturePresent && m_swapchain.canReadBack() && ensureCapture();   // the new extent
		st = m_swapchain.acquire(slot, &image);
	}
	if (st == V::VkSwapchain::Status::DeviceLost) {
		if (guard)
			m_presentLost = true;
		m_writer.endFrame();
		return D3DERR_DEVICELOST;
	}
	if (st != V::VkSwapchain::Status::Ok && st != V::VkSwapchain::Status::Suboptimal)
		return skip("acquire", V::VkSwapchain::name(st));
	if (st == V::VkSwapchain::Status::Suboptimal && !m_debug.ignoreDrawableSize)
		m_swapchainDirty = true;
	if (st != V::VkSwapchain::Status::Ok && m_stats.acquireNotOk++ < 4)
		logLine("[GXNATIVE] acquire: %s", V::VkSwapchain::name(st));
	if (m_debug.targetCheckAfterAcquire && !ensurePresentTarget(*src, &shader, &direct, &why))
		return skip("present target", why);   // the plant: the acquired image's semaphore is left signalled
	const VkExtent2D ext = m_swapchain.extent();
	const bool scaled = ext.width != src->width || ext.height != src->height;
	if (scaled)
		++m_stats.stretchedPresents;
	R::GxImage* touched[2] = { src, shader && !direct ? &m_presentImg->image() : nullptr };
	const VkBuffer shot = m_debug.testSwapchainReadback && m_swapchain.canReadBack() ? testShotBuffer() : VK_NULL_HANDLE;   // TEST
	const bool lostSubmit = m_debug.failNextSubmit;   // TEST fault injection: as a failed vkQueueSubmit2 - nothing of the present
	m_debug.failNextSubmit = false;                   // reaches the GPU (the swapchain image is never touched)
	if (!lostSubmit) m_writer.outsideScope([&](VkCommandBuffer cmd) {
		const int gpuPresent = m_writer.gpuBegin("present");   // GX_GPUTIME A17: the shader resolve + the swapchain blit
		// TESTS: one read-back per present, of the FINISHED swapchain image, on the one-pass path (finishImage) as on the blit path
		// (recordPresent): A23-B's shot (testReadSwapchain: the last SHOWN image persists across an unshown present) when an arm
		// asked for it, else W-a's capture (testReadPresent: none after a present that recorded none - m_capW / m_capH stay 0)
		const bool cap = capture && !shot;
		const VkBuffer readback = shot ? shot : cap ? m_capBuf : VK_NULL_HANDLE;
		m_capW = cap ? m_swapchain.extent().width : 0;
		m_capH = cap ? m_swapchain.extent().height : 0;
		if (direct) {   // W-a: one pass, the present shader writes the swapchain image
			const VkPipelineStageFlags2 att = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			if (!m_debug.presentSkipsTransition)
				m_swapchain.beginImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, att, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
			const bool drawn = !m_debug.presentPipelineFails && m_present.recordDirect(cmd, slot, *src, bb->attachmentView(),
				m_swapchain.format(), m_swapchain.view(image), m_debug.presentDirectAverage);
			VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			VkPipelineStageFlags2 stage = att;
			VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			if (!drawn && !m_debug.presentFailNoClear) {   // never present an unwritten image: black, counted
				if (m_stats.presentErrors++ == 0)
					logLine("[GXNATIVE] present shader failed: the frame is shown black");
				VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				b.srcStageMask = att;   // the attachment transition above; nothing was drawn
				b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
				b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				b.oldLayout = layout;
				b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = m_swapchain.image(image);
				b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dep.imageMemoryBarrierCount = 1;
				dep.pImageMemoryBarriers = &b;
				vkCmdPipelineBarrier2(cmd, &dep);
				const VkClearColorValue black = {};
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				vkCmdClearColorImage(cmd, m_swapchain.image(image), layout, &black, 1, &range);
				stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
				access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			}
			m_swapchain.finishImage(cmd, image, layout, stage, access, readback);
			++m_stats.presentsDirect;
			m_writer.gpuEnd(gpuPresent);
			return;
		}
		R::GxImage* from = src;
		if (shader) {
			R::GxImage& to = m_presentImg->image();
			bool drawn = true;
			if (m_debug.csRecoveryStaleSource && m_testRecreated) {
				m_testRecreated = false;   // the plant: the target is not rewritten - its old content is shown as this frame
			} else if (m_debug.presentResolveAverages && !scaled) {
				bb->resolveTo(cmd, to);
			} else if ((m_debug.presentBlitLinear || (m_debug.csRecoveryLinear && m_testRecreated && scaled)) &&
				src->samples == VK_SAMPLE_COUNT_1_BIT) {
				m_testRecreated = m_testRecreated && !m_debug.csRecoveryLinear;   // the plant is one-shot
				src->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
				to.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
				VkImageBlit b = {};
				b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				b.srcOffsets[1] = { int32_t(src->width), int32_t(src->height), 1 };
				b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				b.dstOffsets[1] = { int32_t(to.width), int32_t(to.height), 1 };
				vkCmdBlitImage(cmd, src->image, src->layout, to.image, to.layout, 1, &b, VK_FILTER_LINEAR);
			} else {
				drawn = !m_debug.presentPipelineFails &&
					m_present.record(cmd, slot, *src, bb->attachmentView(), to, m_presentImg->attachmentView(), m_debug.presentSkipsTransition);
			}
			if (!drawn && !m_debug.presentFailNoClear) {   // never present an unwritten target: black, counted
				if (m_stats.presentErrors++ == 0)
					logLine("[GXNATIVE] present shader failed: the frame is shown black");
				to.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
				const VkClearColorValue black = {};
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(cmd, to.image, to.layout, &black, 1, &range);
			}
			from = &to;
		}
		if (!m_debug.presentSkipsTransition)
			from->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		if ((m_debug.csRecoveryHalfCopy || m_debug.csRecoveryMirror || m_debug.csRecoveryClip) && m_testRecreated) {   // the plants: the
			m_testRecreated = false;   // first present after a recreation copies half / mirrored / clipped
			(m_debug.csRecoveryHalfCopy ? V::g_testMutants.presentHalfBlit : m_debug.csRecoveryMirror ? V::g_testMutants.presentMirrorBlit :
				V::g_testMutants.presentClipBlit) = true;
		}
		m_swapchain.recordPresent(cmd, image, from->image, from->width, from->height, readback);   // equal extents: a 1:1 copy
		m_writer.gpuEnd(gpuPresent);
	}, touched, shader && !direct ? 2u : 1u);
	V::SubmitDesc extra;
	m_swapchain.addSubmitSync(slot, image, &extra);
	uint64_t submitted = 0;
	if (lostSubmit)
		m_writer.endFrame();   // the game's own work of the frame still runs
	else
		submitted = m_writer.endFrame(extra);
	if (!submitted && !m_debug.presentAfterFailedSubmit) {
		// the present would wait on a semaphore no submission signals (VUID-vkQueuePresentKHR-pWaitSemaphores-03268) and the
		// acquired image's wait is gone: terminal, as a device loss
		logLine("[GXNATIVE] FATAL the frame's submission failed after its acquire: presentation stopped (DEVICELOST)");
		m_presentDead = true;
		m_lostLogged = true;   // one FATAL line per session
		return D3DERR_DEVICELOST;
	}
	m_presentWaits[slot] = submitted;
	m_presentSlot = (slot + 1) % uint32_t(m_presentWaits.size());
	const bool presentError = m_debug.injectPresentError;   // TEST fault injection: as VK_ERROR_OUT_OF_*_MEMORY - nothing enqueued,
	m_debug.injectPresentError = false;                     // the image stays acquired and its present semaphore unwaited
	V::PresentHook hook;   // FG P0 H3: Streamline's PCL present markers around vkQueuePresentKHR alone (null without Streamline)
	const bool hooked = !presentError && slPresentHook(&hook);
	st = presentError ? V::VkSwapchain::Status::Error : m_swapchain.present(image, hooked ? &hook : nullptr);
	if (m_debug.injectPresentSurfaceLost) {   // TEST fault injection: the present reports the surface lost, and it stays lost
		m_debug.injectPresentSurfaceLost = false;
		m_testSurfaceLost = true;
		st = V::VkSwapchain::Status::SurfaceLost;
	}
	applyPresentStatus(st);   // under the submit thread st is Ok here (or a fake above): the answer comes at the next Present
	if (st == V::VkSwapchain::Status::Ok || st == V::VkSwapchain::Status::Suboptimal)
		++m_stats.presentsShown;   // FG P0 H3: what frame generation's stable window counts (not skipped Presents)
	if (st == V::VkSwapchain::Status::DeviceLost && guard)
		m_presentLost = true;
	return st == V::VkSwapchain::Status::DeviceLost ? D3DERR_DEVICELOST : D3D_OK;
}

void GxNativeDevice::applyPresentStatus(V::VkSwapchain::Status st)
{
	if (st == V::VkSwapchain::Status::SurfaceLost)
		m_surfaceLost = true;
	if ((st == V::VkSwapchain::Status::Suboptimal || st == V::VkSwapchain::Status::OutOfDate) && !m_debug.ignoreDrawableSize)
		m_swapchainDirty = true;
	// any other failure (VK_ERROR_OUT_OF_*_MEMORY: the present was not enqueued, its image stays acquired - repeated, every image
	// would be stranded): rebuild before the next acquire, as DXVK's presenter does for every status but VK_SUCCESS
	// (dxvk_presenter.cpp:247-252). The rebuild waits for the device and retires the old swapchain with its images and semaphores.
	if (st == V::VkSwapchain::Status::Error && !m_debug.presentErrorIgnored) {
		m_swapchainDirty = true;
		if (m_stats.presentFailures++ == 0)
			logLine("[GXNATIVE] present failed: the swapchain is rebuilt before the next acquire");
	}
}

// DXVK's ResetState (d3d9_device.cpp ResetState): render / stage / sampler states to the device defaults, textures, streams,
// indices, render target and depth unbound. Transforms, lights, material, shaders and their constants persist.
void GxNativeDevice::resetState(const D3DPRESENT_PARAMETERS& pp)
{
	for (uint32_t i = 0; i < 8; ++i)
		SetTexture(i, nullptr);
	for (uint32_t i = 0; i < 16; ++i)
		SetStreamSource(i, nullptr, 0);
	SetIndices(nullptr, 0);
	bindSurface(m_rt, nullptr);
	bindSurface(m_ds, nullptr);
	m_tr.resetStates();
	// ZENABLE from the INCOMING parameters (d3d9_device.cpp:581 ResetState(pPresentationParameters), :8354), also when the Reset
	// then fails
	const D3DPRESENT_PARAMETERS& z = m_debug.resetZFromOldParams ? m_pp : pp;
	m_tr.renderState(D3DRS_ZENABLE, z.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE);
	syncTarget(true);   // render target 0 unbound: the viewport is 0x0 until the back buffer is bound again
}

// d3d9_device.cpp:560-626 for a non-extended device (DESIGN 8.4).
HRESULT GxNativeDevice::Reset(D3DPRESENT_PARAMETERS* pp)
{
	// DXVK's phases, in order: each failure leaves exactly the state DXVK leaves.
	if (!pp)
		return D3DERR_INVALIDCALL;
	std::unique_ptr<SScope> sc;   // A23-B §3.9: S - today's body inline, the mirror and the worker's binding re-seeded after it
	if (!m_debug.csResetNoDrain)
		sc.reset(new SScope(*this, "Reset"));
	// A. d3d8's own checks (d3d8_device.cpp:236-247): nothing changes, the lost state included
	if (!m_debug.resetSkipsD3D8Checks && !d3d8FrontValid(*pp))
		return D3DERR_INVALIDCALL;
	GxD3D8NormalizePresentParams(pp);   // DXVK converts here (d3d8_device.cpp:253), visible to the caller
	// B. (d3d8 purges its binding cache - no separate cache here) d3d9 Reset: the lost state becomes Ok, then the early
	//    validation (d3d9_device.cpp:566-576) - bindings untouched on failure
	if (!m_debug.resetBKeepsLost)
		m_notReset = false;
	const char* unbuildable = nullptr;
	if (m_debug.resetPhasesCollapsed && !nativeBuildable(*pp, &unbuildable))
		return D3DERR_INVALIDCALL;   // the plant: the late checks moved before the unbinding
	if (!m_debug.resetSkipsD3D8Checks && !d3d9EarlyValid(*pp))
		return D3DERR_INVALIDCALL;
	// render-motion-1: past every check that leaves the bindings untouched, no announcement or open world pass survives
	//
	m_nextDepthSampled = false;
	worldAbandon();
	if (m_motionInit)
		m_motion.published(false);   // the depth the published frame names is released just below
	worldRelease(true);
	// C. ResetState, back buffers and auto depth destroyed, InScene cleared, ALWAYS (:579-591); live losable objects ->
	//    DEVICELOST + NotReset (:604-608)
	endFrameForHost();
	m_ctx.waitIdle();
	// F-depth+'s copies, its arming and its once-per-frame flag go with the targets they came from; a format
	// that failed before is tried again. S body: the worker is idle, the GPU too.
	m_sceneTapped = false;
	if (m_sceneInit)
		m_scene.dropTargets(m_writer);
	// R19: the last present's answer belongs to the swapchain about to go - applied first, so a SurfaceLost makes this Reset build
	// on a new surface exactly as today's inline handling did
	if (m_haveSwapchain && !m_debug.resetDropsPresentStatus)
		applyPresentStatus(m_swapchain.takePresentStatus());
	resetState(*pp);
	VkSurfaceKHR keepSurface = m_surface;
	slBeforeRecreate();   // FG P0 H3: a Reset replaces the swapchain too - frame generation off first
	destroySwapchainObjects();
	m_surface = keepSurface;
	m_inScene = false;
	drainDestroys();   // what ResetState / the back buffers released is gone before the losable count is read
	if (m_stats.losable != 0 && !m_debug.resetIgnoresLosable) {
		logLine("[GXNATIVE] Reset refused: %lld live DEFAULT-pool objects (NotReset)", (long long)m_stats.losable);
		m_notReset = true;
		return D3DERR_DEVICELOST;
	}
	// D. recreation (ResetSwapChain, :611-618): a failure leaves NotReset
	std::string why;
	if (!nativeBuildable(*pp, &unbuildable))
		why = std::string("not buildable natively: ") + unbuildable;
	if (!unbuildable && !createSwapchainObjects(*pp, &why))
		unbuildable = "swapchain";
	slAfterRecreate();   // FG P0 H3: the stable window starts again (ok or not)
	if (unbuildable) {
		logLine("[GXNATIVE] Reset failed: %s (NotReset)", why.c_str());
		m_notReset = true;
		return D3DERR_INVALIDCALL;
	}
	m_tr.renderState(D3DRS_ZENABLE, pp->EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE);
	// (A108): an A2C session Reset to ANOTHER MSAA prewarms the new attachment configurations and their A2C
	// twins before Reset returns - else an alpha-tested draw, whose A2C key differs from the opaque draw's, would compile on first use. This
	// is an S body (the worker and the GPU are idle). A session without A2C (key off, or created at 1x) does nothing here: today's Reset.
	if (m_a2cOn && m_host.prebuildGenericSet && m_a2cPrewarmedSamples != 0) {
		// compared and retargeted by the GRANTED counts the configurations carry, never the requested ones
		std::vector<GxAttachConfig> configs;
		std::string cw;
		const bool cfgOk = GxSessionAttachConfigs(m_ctx, pp->BackBufferFormat, uint8_t(std::max<uint32_t>(1u, uint32_t(pp->MultiSampleType))),
			configs, &cw);
		uint32_t s = 1;
		for (const GxAttachConfig& c : configs)
			s = std::max<uint32_t>(s, c.samples);
		if (s != m_a2cPrewarmedSamples && !m_debug.a2cResetNoPrewarm) {
			if (cfgOk) {
				// the keys this session (or an earlier one, recorded) drew at the old MSAA, re-made for the new one
				std::vector<GxPipelineKey> extra;
				if (!m_debug.a2cResetNoRetarget)
					extra = m_cache.retargetedKeys(uint8_t(m_a2cPrewarmedSamples), uint8_t(s), configs.data(), configs.size());
				const size_t q = m_cache.prebuild(kGxGenericParts, kGxGenericPartCount, configs.data(), configs.size(), 2, &extra);
				m_cache.waitPrebuild();
				const bool complete = m_cache.prebuildComplete();
				if (complete)
					m_a2cPrewarmedSamples = s;
				logLine("[GXA2C] reset prewarm samples=%u queued=%zu complete=%d", s, q, int(complete));
				// (the integrator's ruling after): a failed prewarm turns A2C off for
				// the session - the Reset itself succeeds, the failed keys are dropped (a later draw takes today's first-use path)
				if (!complete && !m_debug.a2cNoFallback)
					a2cOff("prewarm-failed", "reset");
			} else {
				logLine("[GXA2C] reset prewarm samples=%u refused: %s", s, cw.c_str());
				if (!m_debug.a2cNoFallback)
					a2cOff("prewarm-refused", "reset");
			}
		}
	}
	// every successful Reset names the extents it made: the session line is the CREATION snapshot (rw3-H KG6: the game created
	// the device at 1024x768 and Reset it to 1920x1080 at the match - the log showed no trace of the switch)
	if (!m_debug.resetUnlogged)
		logLine("[GXNATIVE] Reset: back buffer %ux%u msaa=%u swapchain %ux%u", pp->BackBufferWidth, pp->BackBufferHeight,
			unsigned(pp->MultiSampleType), m_haveSwapchain ? m_swapchain.extent().width : 0u,
			m_haveSwapchain ? m_swapchain.extent().height : 0u);
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ state
// A23-B §3.2 / §3.5: a setter is A - INVALIDCALL where today checks a pointer (front), the mirror written with the translator's
// own store, exactly the bytes today's body reads copied into the command; a getter is F - today's body with m_tr.state()
// replaced by the mirror.

HRESULT GxNativeDevice::SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m)
{
	if (!m)
		return D3DERR_INVALIDCALL;
	const uint32_t t = uint32_t(s);
	if (m_debug.csTransformCopyAll) {   // the plant: 64 bytes read before the type is known
		volatile float sink = 0.0f;
		for (int i = 0; i < 16; ++i)
			sink = (&m->_11)[i];
		(void)sink;
	}
	float* slot = t == D3DTS_WORLD ? m_mirror.world : t == D3DTS_VIEW ? m_mirror.view : t == D3DTS_PROJECTION ? m_mirror.projection :
		t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7 ? m_mirror.texture[t - D3DTS_TEXTURE0] : nullptr;
	if (!slot)
		return D3D_OK;   // a type the translator does not model: today's body returns before reading the matrix (§3.2)
	struct Matrix { float f[16]; } mat;
	std::memcpy(mat.f, &m->_11, 64);
	std::memcpy(slot, mat.f, 64);   // the front mirror keeps what the caller set: the CLEAN matrix (GetTransform)
	// inside a jittering world pass the translator gets the jittered projection; the clean one stays front state
	// (rev 2). Motion never initialised (key off) = exactly as before. With the A23-B worker on (seam-worker-1) the
	// jittered matrix travels BY VALUE in the queued command: m_motion is front state, changed only here and in the S bodies of
	// worldBegin/End, so the worker applies exactly what an inline body would have.
	if (t == D3DTS_PROJECTION && m_motionInit)
		std::memcpy(mat.f, m_motion.projection(&m->_11), 64);
	cs([this, t, mat] {
		const GxCsBytes b = csFlat(mat.f, 64);
		csTrace(GxCsOp::Transform, { uint64_t(t) }, &b);
		m_tr.transform(t, mat.f, false);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m)
{
	if (!m)
		return D3DERR_INVALIDCALL;
	SScope sc(*this, "MultiplyTransform");   // never called (census): today's body inline, the mirror re-seeded after it
	neverCalled("MultiplyTransform");
	if (m_debug.csMultiplySkip && m_csOn)
		return D3D_OK;   // the plant (CS=1; inside the S body csDirect() is true)
	// render-motion-1: the projection multiplies the CLEAN front copy and goes through SetTransform's jitter;
	// the product order is the translator's (GxD3D8Translate::transform multiply: M * current)
	if (s == D3DTS_PROJECTION && m_motionInit && m_motion.cleanProjection()) {
		D3DMATRIX prod;
		gxMat4Mul(&m->_11, m_motion.cleanProjection(), &prod._11);
		return SetTransform(s, &prod);
	}
	m_tr.transform(uint32_t(s), &m->_11, true);
	return D3D_OK;
}

HRESULT GxNativeDevice::GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m)
{
	if (!m)
		return D3DERR_INVALIDCALL;
	const Mirror& ff = m_mirror;
	const float* clean = s == D3DTS_PROJECTION && m_motionInit ? m_motion.cleanProjection() : nullptr;   // never the jittered one
	const float* src = clean ? clean : s == D3DTS_WORLD ? ff.world : s == D3DTS_VIEW ? ff.view : s == D3DTS_PROJECTION ? ff.projection :
		s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7 ? ff.texture[s - D3DTS_TEXTURE0] : nullptr;
	if (!src)
		return D3DERR_INVALIDCALL;
	std::memcpy(&m->_11, src, 64);
	return D3D_OK;
}

HRESULT GxNativeDevice::SetViewport(const D3DVIEWPORT8* vp)
{
	if (!vp)
		return D3DERR_INVALIDCALL;
	const GxD3D8Viewport v = { vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ };
	if (!m_debug.csMirrorViewport)
		m_mirror.viewport = v;
	if (m_debug.csViewportByRef && !csDirect()) {   // the plant: the caller's struct, read when the worker runs it
		cs([this, vp] {
			const GxD3D8Viewport r = { vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ };
			csTrace(GxCsOp::Viewport, { uint64_t(r.x), uint64_t(r.y), uint64_t(r.width), uint64_t(r.height), csBits(r.minZ), csBits(r.maxZ) });
			m_tr.viewport(r);
		});
		return D3D_OK;
	}
	cs([this, v] {
		csTrace(GxCsOp::Viewport, { uint64_t(v.x), uint64_t(v.y), uint64_t(v.width), uint64_t(v.height), csBits(v.minZ), csBits(v.maxZ) });
		m_tr.viewport(v);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetViewport(D3DVIEWPORT8* vp)
{
	if (!vp)
		return D3DERR_INVALIDCALL;
	const GxD3D8Viewport& v = m_mirror.viewport;
	vp->X = v.x;
	vp->Y = v.y;
	vp->Width = v.width;
	vp->Height = v.height;
	vp->MinZ = v.minZ;
	vp->MaxZ = v.maxZ;
	return D3D_OK;
}

HRESULT GxNativeDevice::SetMaterial(const D3DMATERIAL8* m)
{
	if (!m)
		return D3DERR_INVALIDCALL;
	static_assert(sizeof(GxD3D8Material) == sizeof(D3DMATERIAL8), "GxD3D8Material mirrors D3DMATERIAL8");
	GxD3D8Material g;
	std::memcpy(&g, m, sizeof(g));
	m_mirror.material = g;
	if (m_debug.csMaterialByRef && !csDirect()) {   // the plant: the caller's struct, read when the worker runs it
		cs([this, m] {
			GxD3D8Material r;
			std::memcpy(&r, m, sizeof(r));
			const GxCsBytes b = csFlat(&r, sizeof(r));
			csTrace(GxCsOp::Material, {}, &b);
			m_tr.material(r);
		});
		return D3D_OK;
	}
	cs([this, g] {
		const GxCsBytes b = csFlat(&g, sizeof(g));
		csTrace(GxCsOp::Material, {}, &b);
		m_tr.material(g);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetMaterial(D3DMATERIAL8* m)
{
	if (m)
		std::memcpy(m, &m_mirror.material, sizeof(*m));
	return neverCalled("GetMaterial");
}

HRESULT GxNativeDevice::SetLight(DWORD index, const D3DLIGHT8* l)
{
	if (!l)
		return D3DERR_INVALIDCALL;
	static_assert(sizeof(GxD3D8Light) == sizeof(D3DLIGHT8), "GxD3D8Light mirrors D3DLIGHT8");
	GxD3D8Light g;
	std::memcpy(&g, l, sizeof(g));
	if (m_debug.csDropLight && !csDirect())
		return D3D_OK;   // the plant
	if (m_debug.csLightByRef && !csDirect()) {   // the plant: the caller's struct, read when the worker runs it
		cs([this, index, l] {
			GxD3D8Light r;
			std::memcpy(&r, l, sizeof(r));
			const GxCsBytes b = csFlat(&r, sizeof(r));
			csTrace(GxCsOp::Light, { uint64_t(index) }, &b);
			m_tr.light(index, r);
		});
		return D3D_OK;
	}
	cs([this, index, g] {
		const GxCsBytes b = csFlat(&g, sizeof(g));
		csTrace(GxCsOp::Light, { uint64_t(index) }, &b);
		m_tr.light(index, g);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetLight(DWORD, D3DLIGHT8*) { return neverCalled("GetLight"); }

HRESULT GxNativeDevice::LightEnable(DWORD index, WINBOOL on)
{
	const bool b = on != FALSE;
	if (m_debug.csDropLightEnable && !csDirect())
		return D3D_OK;   // the plant
	cs([this, index, b] {
		csTrace(GxCsOp::LightEnable, { uint64_t(index), uint64_t(b) });
		m_tr.lightEnable(index, (m_debug.csLightEnableTrue && m_cs.onWorker()) || b);   // (the plant)
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetLightEnable(DWORD, WINBOOL*) { return neverCalled("GetLightEnable"); }

HRESULT GxNativeDevice::SetClipPlane(DWORD, const float*)
{
	return D3D_OK;   // stored nowhere: CLIPPLANEENABLE != 0 at a draw is refused by GxPackFF (no live caller, census)
}

HRESULT GxNativeDevice::GetClipPlane(DWORD, float*) { return neverCalled("GetClipPlane"); }

HRESULT GxNativeDevice::SetRenderState(D3DRENDERSTATETYPE s, DWORD v)
{
	if (s == D3DRS_SOFTWAREVERTEXPROCESSING) {   // d3d8_device.cpp SetRenderState: only a MIXED device switches
		if (m_behavior & D3DCREATE_MIXED_VERTEXPROCESSING)
			m_swvp = v != FALSE;
		return D3D_OK;
	}
	const uint32_t st = uint32_t(s);
	if (m_debug.csFullFails && !csDirect() && m_cs.testWouldWait(64, 0))
		return D3DERR_INVALIDCALL;   // the plant: a full ring fails the call
	if (st < 256)
		m_mirror.rs[st] = v;
	if (m_debug.csReorder && !csDirect() && !m_reorderPending) {   // the plant: this state is pushed after the next draw
		m_reorderPending = true;
		m_reorderState = st;
		m_reorderValue = v;
		return D3D_OK;
	}
	cs([this, st, v] {
		csTrace(GxCsOp::RenderState, { uint64_t(st), uint64_t(v) });
		if (m_debug.csTrSkipRenderState && m_cs.onWorker())
			return;   // the plant
		m_tr.renderState(st, v);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v)
{
	if (!v || uint32_t(s) >= 256)
		return D3DERR_INVALIDCALL;
	// d3d8_device.cpp GetRenderState: the ACTIVE mode (d3d9 GetSoftwareVertexProcessing), not a stored state - the setter keeps it
	// in m_swvp, never in the translator
	if (s == D3DRS_SOFTWAREVERTEXPROCESSING && !m_debug.swvpStateFromTranslator) {
		*v = m_swvp ? TRUE : FALSE;
		return D3D_OK;
	}
	*v = m_debug.csGetterReadsWorker ? m_tr.state().rs[s] : m_mirror.rs[s];
	return D3D_OK;
}

HRESULT GxNativeDevice::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
	const uint32_t tt = uint32_t(t);
	if (stage < 8 && tt < 32)
		m_mirror.tss[stage][tt] = v;
	cs([this, stage, tt, v] {
		csTrace(GxCsOp::TextureStageState, { uint64_t(stage), uint64_t(tt), uint64_t(v) });
		m_tr.textureStageState(stage, tt, v);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD* v)
{
	if (!v || stage >= 8 || uint32_t(t) >= 32)
		return D3DERR_INVALIDCALL;
	*v = m_mirror.tss[stage][t];
	return D3D_OK;
}

HRESULT GxNativeDevice::SetTexture(DWORD stage, IDirect3DBaseTexture8* t)
{
	if (stage >= 8)
		return D3DERR_INVALIDCALL;
	GxNativeTexture* n = t ? static_cast<GxNativeTexture*>(static_cast<IDirect3DTexture8*>(t)) : nullptr;
	GxNativeTexture* old = m_textures[stage] ? static_cast<GxNativeTexture*>(static_cast<IDirect3DTexture8*>(m_textures[stage])) : nullptr;
	bindPrivate(old, n);
	m_textures[stage] = t;
	cs([this, stage, n] {
		csTrace(GxCsOp::Texture, { uint64_t(stage), static_cast<const void*>(n) });
		m_tr.texture(stage, n ? &n->gx() : nullptr);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetTexture(DWORD stage, IDirect3DBaseTexture8** pp)
{
	if (!pp || stage >= 8)
		return D3DERR_INVALIDCALL;
	*pp = m_textures[stage];
	if (*pp)
		(*pp)->AddRef();
	if (m_debug.getTextureNeverCalled)
		neverCalled("GetTexture");
	return D3D_OK;
}

HRESULT GxNativeDevice::SetStreamSource(UINT stream, IDirect3DVertexBuffer8* vb, UINT stride)
{
	if (stream >= 16)
		return D3DERR_INVALIDCALL;
	GxNativeVertexBuffer* n = static_cast<GxNativeVertexBuffer*>(vb);
	bindPrivate(m_streams[stream], n);
	m_strides[stream] = stride;
	cs([this, stream, n, stride] {
		csTrace(GxCsOp::Stream, { uint64_t(stream), static_cast<const void*>(n), uint64_t(stride) });
		m_tr.stream(stream, n ? &n->gx() : nullptr, stride);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetStreamSource(UINT stream, IDirect3DVertexBuffer8** pp, UINT* stride)
{
	if (!pp || stream >= 16)
		return D3DERR_INVALIDCALL;
	*pp = m_streams[stream];
	if (*pp)
		(*pp)->AddRef();
	if (stride)
		*stride = m_strides[stream];
	if (m_debug.getStreamSourceNeverCalled)
		neverCalled("GetStreamSource");
	return D3D_OK;
}

HRESULT GxNativeDevice::SetIndices(IDirect3DIndexBuffer8* ib, UINT baseVertex)
{
	GxNativeIndexBuffer* n = static_cast<GxNativeIndexBuffer*>(ib);
	bindPrivate(m_indices, n);
	m_baseVertex = baseVertex;
	cs([this, n, baseVertex] {
		csTrace(GxCsOp::Indices, { static_cast<const void*>(n), uint64_t(baseVertex) });
		m_tr.indices(n ? &n->gx() : nullptr, m_debug.csIndicesBaseZero && m_cs.onWorker() ? 0 : baseVertex);   // (the plant)
	});
	return D3D_OK;
}

// d3d8_device.cpp GetIndices, exactly. A supported getter since L2's whole-run fallback reads the binding (rw3-G-3,
// it was counted and logged as a method the game never calls).
HRESULT GxNativeDevice::GetIndices(IDirect3DIndexBuffer8** pp, UINT* baseVertex)
{
	if (pp)
		*pp = nullptr;
	if (!pp || !baseVertex)
		return D3DERR_INVALIDCALL;
	*pp = m_indices;
	if (*pp)
		(*pp)->AddRef();
	*baseVertex = m_baseVertex;
	if (m_debug.getIndicesNeverCalled)
		neverCalled("GetIndices");
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ shaders
// Handles are allocated and validated on the front (§3.5); the tokens today's body hands the translator (declLen / funcLen, read
// on the caller as today) are copied into the command.

HRESULT GxNativeDevice::CreateVertexShader(const DWORD* decl, const DWORD* code, DWORD* handle, DWORD)
{
	if (!decl || !handle)
		return D3DERR_INVALIDCALL;
	m_shaders.push_back({ true, false });
	*handle = shaderHandle(m_shaders.size() - 1);
	const size_t dl = declLen(decl), fl = code ? funcLen(code) : 0;
	const DWORD h = *handle;
	const bool hasCode = code != nullptr;
	if (m_debug.csDropCreateVs && !csDirect())
		return D3D_OK;   // the plant
	if (m_debug.csVsByRef && !csDirect()) {   // the plant: decl + code read from the caller's memory when the worker runs it
		++m_csIssued;
		m_cs.push([this, h, dl, fl, hasCode, decl, code] {
			const GxCsBytes bd = csFlat(decl, dl * 4), bc = csFlat(code, fl * 4);
			csTrace(GxCsOp::CreateVs, { uint64_t(h), uint64_t(dl), uint64_t(fl), uint64_t(hasCode) }, &bd);
			csTrace(GxCsOp::CreateVs, {}, &bc);
			m_tr.createVertexShader(h, reinterpret_cast<const uint32_t*>(decl), dl, reinterpret_cast<const uint32_t*>(code), fl);
		});
		csAfter();
		return D3D_OK;
	}
	if (csDirect() || (dl + fl) * 4 > m_cs.maxPayload()) {
		if (m_debug.csShaderOversizeRefused && !csDirect())
			return D3DERR_INVALIDCALL;   // the plant
		const bool late = m_debug.csOversizeLateDrain && !csDirect();   // the plant: S entered after the translation
		std::unique_ptr<SScope> sc;   // oversize: S
		if (!csDirect() && !late)
			sc.reset(new SScope(*this, "CreateVertexShader oversize"));
		++m_csIssued;
		const GxCsBytes bd = csFlat(decl, dl * 4), bc = csFlat(code, fl * 4);
		csTrace(GxCsOp::CreateVs, { uint64_t(h), uint64_t(dl), uint64_t(fl), uint64_t(hasCode) }, &bd);
		csTrace(GxCsOp::CreateVs, {}, &bc);
		m_tr.createVertexShader(h, reinterpret_cast<const uint32_t*>(decl), dl, reinterpret_cast<const uint32_t*>(code), fl);
		if (late)
			sc.reset(new SScope(*this, "CreateVertexShader oversize"));
		return D3D_OK;
	}
	++m_csIssued;
	m_cs.pushFill([this, h, dl, fl, hasCode](const uint8_t* p, size_t) {
		const uint32_t* w = reinterpret_cast<const uint32_t*>(p);
		const GxCsBytes bd = csFlat(w, dl * 4), bc = csFlat(w + dl, fl * 4);
		csTrace(GxCsOp::CreateVs, { uint64_t(h), uint64_t(dl), uint64_t(fl), uint64_t(hasCode) }, &bd);
		csTrace(GxCsOp::CreateVs, {}, &bc);
		m_tr.createVertexShader(h, w, dl, hasCode ? (m_debug.csTrVsCodeAtDecl && m_cs.onWorker() ? w : w + dl) : nullptr, fl);   // (the plant)
	}, (dl + fl) * 4, [&](uint8_t* dst) {
		std::memcpy(dst, decl, dl * 4);
		if (fl)
			std::memcpy(dst + dl * 4, code, fl * 4);
	});
	csAfter();
	return D3D_OK;
}

HRESULT GxNativeDevice::SetVertexShader(DWORD h)
{
	m_vs = h;
	const bool prog = isShaderHandle(h);
	cs([this, h, prog] {
		csTrace(GxCsOp::SetVs, { uint64_t(h), uint64_t(prog) });
		m_tr.vertexShader(m_debug.csVsBindWrong && prog && m_cs.onWorker() ? h + 2 : h, prog);   // (the plant)
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetVertexShader(DWORD* h)
{
	if (h)
		*h = m_vs;
	if (m_debug.getVertexShaderNeverCalled)
		neverCalled("GetVertexShader");
	return D3D_OK;
}

HRESULT GxNativeDevice::DeleteVertexShader(DWORD h)
{
	if (!isShaderHandle(h) || shaderIndex(h) >= m_shaders.size() || !m_shaders[shaderIndex(h)].live)
		return D3DERR_INVALIDCALL;
	m_shaders[shaderIndex(h)].live = false;
	cs([this, h] {
		csTrace(GxCsOp::DeleteVs, { uint64_t(h) });
		if (m_debug.csDeleteUnbinds && m_cs.onWorker())   // the plants
			return m_tr.vertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE, false);
		if (m_debug.csDeleteSkipsExec && m_cs.onWorker())
			return;
		if (m_debug.csDeleteSkipsUnbound && m_cs.onWorker() && m_tr.testBoundVs() != h)
			return;
		m_tr.deleteVertexShader(h);
	});
	return D3D_OK;
}

// §3.2: the translator reads registers below 96 (vertex) / 8 (pixel) only - exactly those vectors are copied
HRESULT GxNativeDevice::SetVertexShaderConstant(DWORD reg, const void* data, DWORD count)
{
	if (!data)
		return D3DERR_INVALIDCALL;
	if (uint64_t(reg) + count > kGxNativeMaxVsConst) {   // rw3-R11: past what GetDeviceCaps reports and the translator holds - the
		// d3d8 answer for a register past MaxVertexShaderConst, counted (bad) instead of the translator dropping it silently.
		// A23-B: answered on the FRONT, before anything is queued (a front counter, copyFrontStats) - nothing reaches the worker
		if (m_stats.vsConstantsRefused++ == 0)
			logLine("[GXNATIVE] REFUSED SetVertexShaderConstant c%u + %u: this device holds %u (a classic GX_SHADOW_GPU capture packs "
				"61 volumes a draw; record it with GX_SHADOW_GPU_SLOTS=21)", unsigned(reg), unsigned(count), unsigned(kGxNativeMaxVsConst));
		return D3DERR_INVALIDCALL;
	}
	const size_t bytes = m_debug.csConstOverread ? size_t(count) * 16 : reg < 96 ? size_t(std::min<DWORD>(count, 96 - reg)) * 16 : 0;
	if (m_debug.csDropConstants && !csDirect())
		return D3D_OK;   // the plant
	auto exec = [this, reg, count](const uint8_t* p, size_t n) {
		const GxCsBytes b = csFlat(p, n);
		csTrace(GxCsOp::VsConst, { uint64_t(reg), uint64_t(count) }, &b);
		m_tr.vsConstants(reg, count, reinterpret_cast<const float*>(p));
	};
	if (m_debug.csConstByRef && !csDirect()) {   // the plant: the caller's memory, read when the worker runs it
		const uint8_t* src = static_cast<const uint8_t*>(data);
		cs([exec, src, bytes] { exec(src, bytes); });
		return D3D_OK;
	}
	csCopy(exec, data, bytes);
	return D3D_OK;
}

HRESULT GxNativeDevice::CreatePixelShader(const DWORD* code, DWORD* handle)
{
	if (!code || !handle)
		return D3DERR_INVALIDCALL;
	m_shaders.push_back({ true, true });
	*handle = shaderHandle(m_shaders.size() - 1);
	const size_t fl = funcLen(code);
	const DWORD h = *handle;
	if (m_debug.csDropCreatePs && !csDirect())
		return D3D_OK;   // the plant
	if (m_debug.csPsByRef && !csDirect()) {   // the plant: the code read from the caller's memory when the worker runs it
		++m_csIssued;
		m_cs.push([this, h, fl, code] {
			const GxCsBytes b = csFlat(code, fl * 4);
			csTrace(GxCsOp::CreatePs, { uint64_t(h), uint64_t(fl) }, &b);
			m_tr.createPixelShader(h, reinterpret_cast<const uint32_t*>(code), fl);
		});
		csAfter();
		return D3D_OK;
	}
	if (!csDirect() && fl * 4 > m_cs.maxPayload()) {
		if (m_debug.csShaderOversizeRefused)
			return D3DERR_INVALIDCALL;   // the plant
		std::unique_ptr<SScope> sc;
		if (!m_debug.csOversizeLateDrain)   // (the plant: S entered after the translation)
			sc.reset(new SScope(*this, "CreatePixelShader oversize"));
		++m_csIssued;
		const GxCsBytes b = csFlat(code, fl * 4);
		csTrace(GxCsOp::CreatePs, { uint64_t(h), uint64_t(fl) }, &b);
		m_tr.createPixelShader(h, reinterpret_cast<const uint32_t*>(code), fl);
		if (!sc)
			sc.reset(new SScope(*this, "CreatePixelShader oversize"));
		return D3D_OK;
	}
	csCopy([this, h, fl](const uint8_t* p, size_t n) {
		const GxCsBytes b = csFlat(p, n);
		csTrace(GxCsOp::CreatePs, { uint64_t(h), uint64_t(fl) }, &b);
		m_tr.createPixelShader(h, reinterpret_cast<const uint32_t*>(p), fl);
	}, code, fl * 4);
	return D3D_OK;
}

HRESULT GxNativeDevice::SetPixelShader(DWORD h)
{
	m_ps = h;
	cs([this, h] {
		csTrace(GxCsOp::SetPs, { uint64_t(h) });
		m_tr.pixelShader(m_debug.csPsBindDropped && m_cs.onWorker() ? 0 : m_debug.csPsBindFront && m_cs.onWorker() ? m_ps : h);   // (plants)
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::GetPixelShader(DWORD* h)
{
	if (h)
		*h = m_ps;
	if (m_debug.getPixelShaderNeverCalled)
		neverCalled("GetPixelShader");
	return D3D_OK;
}

HRESULT GxNativeDevice::DeletePixelShader(DWORD h)
{
	if (!isShaderHandle(h) || shaderIndex(h) >= m_shaders.size() || !m_shaders[shaderIndex(h)].live)
		return D3DERR_INVALIDCALL;
	m_shaders[shaderIndex(h)].live = false;
	cs([this, h] {
		csTrace(GxCsOp::DeletePs, { uint64_t(h) });
		if (m_debug.csDeleteUnbinds && m_cs.onWorker())   // the plants
			return m_tr.pixelShader(0);
		if (m_debug.csDeleteSkipsExec && m_cs.onWorker())
			return;
		if (m_debug.csDeleteSkipsUnbound && m_cs.onWorker() && m_tr.testBoundPs() != h)
			return;
		m_tr.deletePixelShader(h);
	});
	return D3D_OK;
}

HRESULT GxNativeDevice::SetPixelShaderConstant(DWORD reg, const void* data, DWORD count)
{
	if (!data)
		return D3DERR_INVALIDCALL;
	const size_t bytes = m_debug.csConstOverread ? size_t(count) * 16 : reg < 8 ? size_t(std::min<DWORD>(count, 8 - reg)) * 16 : 0;
	if (m_debug.csDropConstants && !csDirect())
		return D3D_OK;   // the plant
	auto exec = [this, reg, count](const uint8_t* p, size_t n) {
		const GxCsBytes b = csFlat(p, n);
		csTrace(GxCsOp::PsConst, { uint64_t(reg), uint64_t(count) }, &b);
		m_tr.psConstants(reg, count, reinterpret_cast<const float*>(p));
	};
	if (m_debug.csConstByRef && !csDirect()) {   // the plant
		const uint8_t* src = static_cast<const uint8_t*>(data);
		cs([exec, src, bytes] { exec(src, bytes); });
		return D3D_OK;
	}
	csCopy(exec, data, bytes);
	return D3D_OK;
}

// the csReorder plant (TEST-ONLY): the held-back SetRenderState goes out now, after the draw it should have preceded
void GxNativeDevice::csDrawn()
{
	csFlushReorder();
	if (m_csOn)
		m_cs.publish();   // a draw is where the worker's work is: publish the setters before it with it
}

void GxNativeDevice::csFlushReorder()
{
	if (!m_reorderPending)
		return;
	m_reorderPending = false;
	const uint32_t st = m_reorderState, v = m_reorderValue;
	cs([this, st, v] {
		csTrace(GxCsOp::RenderState, { uint64_t(st), uint64_t(v) });
		m_tr.renderState(st, v);
	});
}

// ------------------------------------------------------------------------------------------------------------ draws
// D3D_OK always, as today; a refusal is the translator's (the worker's), counted and logged once (bad > 0).

HRESULT GxNativeDevice::DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count)
{
	++m_stats.draws;
	if (m_debug.dropDraws)
		return D3D_OK;
	cs([this, t, start, count] {
		csTrace(GxCsOp::Draw, { uint64_t(t), uint64_t(start), uint64_t(count) });
		ensureFrame();
		if (!m_tr.draw(uint32_t(t), start, count))
			refusedDraw();
	});
	csDrawn();
	return D3D_OK;
}

HRESULT GxNativeDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVertices, UINT startIndex, UINT count)
{
	++m_stats.draws;
	if (m_debug.dropDraws || (m_debug.csDropIndexed && !csDirect()))
		return D3D_OK;
	cs([this, t, minIndex, numVertices, startIndex, count] {
		csTrace(GxCsOp::DrawIndexed, { uint64_t(t), uint64_t(minIndex), uint64_t(numVertices), uint64_t(startIndex), uint64_t(count) });
		ensureFrame();
		if (!m_tr.drawIndexed(uint32_t(t), minIndex, numVertices, m_debug.csIndexedStartZero && m_cs.onWorker() ? 0 : startIndex, count))
			refusedDraw();
	});
	csDrawn();
	return D3D_OK;
}

HRESULT GxNativeDevice::DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
{
	++m_stats.draws;
	// d3d8/d3d9: an UP draw leaves stream 0 unbound - the device drops its reference too (the translator forgets the buffer)
	bindPrivate(m_streams[0], static_cast<GxNativeVertexBuffer*>(nullptr));
	m_strides[0] = 0;
	if (m_debug.dropDraws) {
		cs([this] {
			csTrace(GxCsOp::StreamUnbind, {});
			m_tr.stream(0, nullptr, 0);
		});
		return D3D_OK;
	}
	// §3.2: exactly the bytes today reads - primVertices x stride in today's 32-bit arithmetic (a wrap included: residual R25),
	// none for a draw the translator refuses before reading (0 primitives, an unknown type). A null pointer with bytes to read
	// and a payload over kMaxCmd are S: today's body inline.
	const uint32_t verts = Rec::GxD3D8Translate::primVertices(uint32_t(t), count);
	const uint32_t bytes = count == 0 || verts == 0 ? 0u : verts * stride;
	// a non-null pointer to 0 bytes reaches the writer as non-null, as today
	const bool present = data != nullptr && !(m_debug.csUpNullEmpty && bytes == 0);
	auto exec = [this, t, count, stride, present](const uint8_t* p, size_t n) {
		const GxCsBytes b = csFlat(p, n);
		csTrace(GxCsOp::DrawUP, { uint64_t(t), uint64_t(count), uint64_t(stride), uint64_t(present) }, &b);
		ensureFrame();
		TestGuardCopy guard;   // arm 6w: the bytes end at a PROT_NONE page - a read past them faults
		const uint8_t* v = present ? p : nullptr;
		if (m_debug.csUpGuard && v && n)
			v = guard.place(v, n);
		if (m_debug.csPsDrawDropped && m_cs.onWorker() && m_tr.testBoundPs())   // the plant
			return;
		if (!m_tr.drawUP(uint32_t(t), count, v, stride))
			refusedDraw();
	};
	std::unique_ptr<SScope> sync;   // the same exec half, inline with the worker idle
	if (!data && bytes)
		sync.reset(new SScope(*this, "DrawPrimitiveUP null data"));
	else if (!csDirect() && bytes > m_cs.maxPayload())
		sync.reset(new SScope(*this, "DrawPrimitiveUP oversize"));
	if (m_debug.csUpByRef && !csDirect() && !sync && data) {   // the plant: the caller's pointer, read when the worker runs it
		cs([exec, data, bytes] { exec(static_cast<const uint8_t*>(data), bytes); });
		csDrawn();
		return D3D_OK;
	}
	csCopy(exec, data, m_debug.csUpShortCopy && stride && bytes >= stride ? bytes - stride : bytes);
	csDrawn();
	return D3D_OK;
}

HRESULT GxNativeDevice::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVertices, UINT count, const void* indices,
	D3DFORMAT indexFormat, const void* data, UINT stride)
{
	SScope sc(*this, "DrawIndexedPrimitiveUP");   // never called (census): today's body inline
	neverCalled("DrawIndexedPrimitiveUP");
	++m_stats.draws;
	bindPrivate(m_streams[0], static_cast<GxNativeVertexBuffer*>(nullptr));
	bindPrivate(m_indices, static_cast<GxNativeIndexBuffer*>(nullptr));
	ensureFrame();
	if (!m_tr.drawIndexedUP(uint32_t(t), minIndex, numVertices, count, indices, indexFormat == D3DFMT_INDEX32, data, stride))
		refusedDraw();
	return D3D_OK;
}

}} // namespace GXRender::Dev

namespace GXRender { namespace Dev {

// (rev 2): IGxWorldPass::Begin/End bodies. Front side only, except the resolve,
// which is one outside-scope command at End (its arguments are this frame's front state, carried by value in GxMotion).
// (tier 1): with motion running, Begin/End/ObjectVelocity are A23-B S bodies (drain, inline, re-seed) - they move the
// translator's projection, record outside a scope, use GxMotion's GPU objects and read a texture's lazy attachment view, all worker
// state. Their key-off tests stay first and read front state only (m_motionInit; a standalone depth's sample view, made at creation).
void GxNativeDevice::worldRelease(bool depthToo)
{
	for (GxNativeSurface** s : { &m_worldColour, &m_worldObject, depthToo ? &m_worldDepth : nullptr })
		if (s && *s) {
			(*s)->releasePrivateRef();
			*s = nullptr;
		}
}

void GxNativeDevice::worldAbandon()
{
	if (!m_motionInit)
		return;
	const float* clean = m_motion.abandon();
	if (clean && !m_debug.motionAbandonKeepsJitter)
		m_tr.transform(D3DTS_PROJECTION, clean, false);
}

void GxNativeDevice::worldBegin(const GxMotionBegin& b, IDirect3DSurface8* colourI, IDirect3DSurface8* depthI)
{
	GxNativeSurface* depth = static_cast<GxNativeSurface*>(depthI);
	R::GxRenderTarget* drt = depth && depth->kind() == GxNativeSurface::Kind::Depth ? depth->renderTarget() : nullptr;
	const VkImageView view = drt ? drt->depthSampleView() : VK_NULL_HANDLE;
	if (!view && !m_motionInit)
		return;   // key off (the depth was never made sampleable): nothing changes
	std::optional<SScope> sc;
	if (!m_debug.seamNoSBegin)   // (the plant: X-seam-begin)
		sc.emplace(*this, "seam worldBegin");
	csOwnerCheck("worldBegin");   // the boundary the S protects (no ensureFrame below): an S dropped here counts
	if (!m_motionInit) {
		const char* d = std::getenv("GX_MOTION_DEBUG");
		const GxMotionDebugView dv = d && std::strcmp(d, "depth") == 0 ? GxMotionDebugView::Depth :
			d && std::strcmp(d, "velocity") == 0 ? GxMotionDebugView::Velocity : GxMotionDebugView::None;
		std::string why;
		if (!m_motion.init(m_ctx, m_gx, dv, &why)) {
			if (!m_motionOffLogged)
				logLine("[GXMOTION] off: %s", why.c_str());
			m_motionOffLogged = true;
			m_motion.shutdown();
			return;
		}
		m_motionInit = true;
		logLine("[GXMOTION] on: depth %s, debug=%s", drt && drt->image().format ? drt->image().format->name : "?", d ? d : "none");
	}
	worldAbandon();       // a Begin without its End: that pass's jitter goes ...
	worldRelease(true);   // ... and so do its refs, and the previous frame's published depth
	// the targets stay alive until End records from them, whatever the game releases meanwhile
	m_worldColour = static_cast<GxNativeSurface*>(colourI);
	m_worldDepth = depth;
	for (GxNativeSurface* s : { m_worldColour, m_worldDepth })
		if (s)
			s->addPrivateRef();
	GxMotionPlants& pl = m_motion.plants();
	pl.identityReprojection = m_debug.motionIdentityReprojection;
	pl.jitterInVelocity = m_debug.motionJitterInVelocity;
	pl.flipJitterSign = m_debug.motionFlipJitterSign;
	pl.jitterAfterEnd = m_debug.motionJitterAfterEnd;
	pl.ignoreObject = m_debug.motionIgnoreObject;
	// a projection set before motion first ran is the clean copy too: jitter it like a later one
	if (!m_motion.cleanProjection() && !m_debug.motionNoSeed)
		m_motion.projection(m_tr.state().projection);
	const char* whyOff = nullptr;   // a literal: the inactive path allocates nothing
	const float* p = m_motion.begin(b, drt ? &drt->image() : nullptr, view, &whyOff);
	if (whyOff && !m_motionOffLogged) {
		logLine("[GXMOTION] world pass without motion: %s", whyOff);
		m_motionOffLogged = true;
	}
	if (p)
		m_tr.transform(D3DTS_PROJECTION, p, false);
}

void GxNativeDevice::worldEnd()
{
	if (!m_motionInit)
		return;
	std::optional<SScope> sc;   // worldEnd WHOLE in one S: the outsideScope's &ok needs it synchronous (render-seam 09:13Z (2))
	if (!m_debug.seamNoSEnd)    // (the plant: X-seam-end)
		sc.emplace(*this, "seam worldEnd");
	csOwnerCheck("worldEnd");
	bool rec = false;
	GxMotionResolve cmd;
	R::GxImage* colour = m_worldColour ? m_worldColour->image() : nullptr;
	const VkImageView colourView = m_worldColour ? m_worldColour->attachmentView() : VK_NULL_HANDLE;
	const float* clean = m_motion.end(&rec, &cmd, colour, colourView);
	if (rec) {
		R::GxImage* touched[4];
		const uint32_t n = GxMotion::touched(cmd, touched);
		bool ok = false;
		// one outside-scope command with owned arguments (cmd by value): the same region as the seam's End work
		m_writer.outsideScope([this, cmd, &ok](VkCommandBuffer c) {
			const int gpu = m_writer.gpuBegin("motion");
			ok = m_motion.record(c, cmd);
			m_writer.gpuEnd(gpu);
		}, touched, n);
		m_motion.published(ok);
		++m_motionCount.passes;   // front state, inside this S
		if (ok) {
			++m_motionCount.published;
			if (m_motion.isPublished() && (m_motion.frame().flags & GXMF_HAS_OBJECT_MV))
				++m_motionCount.withObject;
		}
		if (!ok && !m_motionOffLogged) {
			logLine("[GXMOTION] resolve not recorded: the frame is not published, history restarts");
			m_motionOffLogged = true;
		}
	}
	worldRelease(false);   // the depth stays held: the published frame names it until the next Begin
	if (clean)
		m_tr.transform(D3DTS_PROJECTION, clean, false);
}

HRESULT GxNativeDevice::worldObjectVelocity(IDirect3DSurface8* oI)
{
	if (!m_motionInit)
		return S_OK;   // key off: nothing to merge into
	std::optional<SScope> sc;   // o->attachmentView() is created lazily, also by the worker (SetRenderTarget -> syncTarget)
	if (!m_debug.seamNoSVelocity)   // (the plant: X-mv-object-late)
		sc.emplace(*this, "seam ObjectVelocity");
	csOwnerCheck("worldObjectVelocity");
	if (m_worldObject) {   // a replacement or a cancellation drops the previous registration
		m_worldObject->releasePrivateRef();
		m_worldObject = nullptr;
	}
	m_motion.setObject(nullptr, VK_NULL_HANDLE);
	GxNativeSurface* o = static_cast<GxNativeSurface*>(oI);
	if (!o)
		return S_OK;
	// a single-sample COLOUR render-target texture level of the world's size: sampleable, one aspect
	R::GxImage* img = o->kind() == GxNativeSurface::Kind::TextureLevel && o->level() == 0 ? o->image() : nullptr;
	const VkImageView view = img ? o->attachmentView() : VK_NULL_HANDLE;
	const R::GxRenderTarget* depth = m_worldDepth ? m_worldDepth->renderTarget() : nullptr;
	const R::GxImage* d = depth ? &const_cast<R::GxRenderTarget*>(depth)->image() : nullptr;
	// 4 channels: the alpha is the 'written' flag (a 2-channel A8L8 view samples alpha 1 everywhere). RGBA16F is
	// units-1's target (signed vectors); an RGBA8 UNORM target carries only [0,1] vectors (the plumbing tests)
	const VkFormat f = img && img->format ? img->format->vk : VK_FORMAT_UNDEFINED;
	const bool fourChannels = f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
	if (!img || !view || !fourChannels || img->samples != VK_SAMPLE_COUNT_1_BIT || !d || img->width != d->width ||
		img->height != d->height) {
		++m_motionCount.objRefused;   // [GXMOTION] device obj_refused
		return E_INVALIDARG;
	}
	++m_motionCount.objAccepted;
	m_worldObject = o;
	o->addPrivateRef();   // held until End records from it
	m_motion.setObject(img, view);
	return S_OK;
}

bool GxNativeDevice::testReadMotion(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height)
{
	// (phase 2: the motion arms run with the worker ON at CS1): an inspection accessor - an S body (endFrameForHost and
	// readImage are worker state; a bare csSync would leave endFrameForHost's ownership check counting)
	SScope sc(*this, "test read-back");
	if (!m_motionInit || !m_motion.mvImage().image)
		return false;
	endFrameForHost();
	*width = m_motion.mvImage().width;
	*height = m_motion.mvImage().height;
	return m_gx.readImage(m_motion.mvImage(), 0, nullptr, out);
}

}} // namespace GXRender::Dev

// ------------------------------------------------------------------------------------------------- S7 shadow map
// (A18, A20, A21, A23, A30); PLAN Task 4. The atlas: one D16 4096x2048 image (two 2048^2 cascades side by
// side), created once at device init with the key on - a step change only moves the cascades' sub-rectangles (no reallocation,
// S14). Cleared to 1.0 and left in DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout every pass returns it to.
static_assert(GX_SHADOW_TAG_RECEIVE == GXRender::Rec::kShadowTagReceive && GX_SHADOW_TAG_CAST == GXRender::Rec::kShadowTagCast &&
	GX_SHADOW_TAG_CAST_ONLY == GXRender::Rec::kShadowTagCastOnly && GX_SHADOW_TAG_SOLID == GXRender::Rec::kShadowTagSolid,
	"GxShadowMap.h and GxShadowCasters.h tag values");

namespace GXRender { namespace Dev {

namespace { const uint32_t kAtlasW = 4096, kAtlasH = 2048, kCasterCapacity = 12288; }   // A30: 2.5x the measured 4,927 peak

// S7 A92 TEST-ONLY (the integrator's P4 ruling (4)): queued with its owned argument like SetShadowTags, executed in the worker's exec half
void GxNativeDevice::testSetLogicFrame(uint32_t frame)
{
	GxNativeDevice* d = this;
	cs([d, frame] {
		csTrace(GxCsOp::LogicFrame, { uint64_t(frame) }, nullptr);
		d->m_writer.setLogicFrame(frame);
	});
}

bool GxNativeDevice::shadowMapCreate(std::string* why)
{
	if (m_host.testShadowMapCreateFails) {   // TESTS (sm-create-fail)
		*why = "(test) atlas creation failed";
		return false;
	}
	const Res::FormatInfo* f = m_gx.formats().imageFormat(Res::d3d::D16);
	if (!f || !f->depth) {
		*why = "no D16 format";
		return false;
	}
	if (!m_atlas.create(m_gx, f, kAtlasW, kAtlasH, 1, VK_SAMPLE_COUNT_1_BIT,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT, why))   // TRANSFER_SRC: the TEST-ONLY dump's copy
		return false;
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = m_atlas.image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = f->vk;
	vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
	if (vkCreateImageView(m_ctx.device(), &vi, nullptr, &m_atlasView) != VK_SUCCESS) {
		*why = "atlas view";
		return false;
	}
	if (!m_casters.init(m_ctx, f->vk, kCasterCapacity, why))
		return false;
	{   // S7 A92 TEST-ONLY replay-k (P4's instrument check, PREREG A15 / A92): GX_SHADOWMAP_DEBUG_REPLAY=k (1, 2, 4) replays the casters k
	    // times, copies 2..k into a scratch depth image of the atlas's size that is never bound to a descriptor (receivers sample only the
	    // atlas) and dies with it; GX_SHADOWMAP_DEBUG_REPLAY_NODRAW=1 (the check's fail arm) records the copies' clears without their draws
		const char* rk = std::getenv("GX_SHADOWMAP_DEBUG_REPLAY");
		uint32_t k = rk ? uint32_t(std::strtoul(rk, nullptr, 10)) : 1u;
		if (k != 1 && k != 2 && k != 4) {
			if (rk)
				logLine("[GXSHADOWMAP] TEST-ONLY debug_replay=%s refused (1, 2 or 4): 1", rk);
			k = 1;
		}
		const char* nd = std::getenv("GX_SHADOWMAP_DEBUG_REPLAY_NODRAW");
		const bool noDraw = nd && nd[0] == '1';
		if (k > 1) {
			if (!m_smScratch.create(m_gx, f, kAtlasW, kAtlasH, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, why))
				return false;
			VkImageViewCreateInfo si = vi;
			si.image = m_smScratch.image;
			if (vkCreateImageView(m_ctx.device(), &si, nullptr, &m_smScratchView) != VK_SUCCESS) {
				*why = "replay-k scratch view";
				return false;
			}
			m_casters.setDebugReplay(k, &m_smScratch, m_smScratchView, noDraw);
			logLine("[GXSHADOWMAP] TEST-ONLY debug_replay=%u scratch=%ux%u nodraw=%d", k, kAtlasW, kAtlasH, noDraw ? 1 : 0);
		}
	}
	Res::GxImage& a = m_atlas;
	if (!m_gx.immediate([&](VkCommandBuffer cmd) {
			a.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkClearDepthStencilValue one = { 1.f, 0 };
			VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
			vkCmdClearDepthStencilImage(cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
			a.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		})) {
		*why = "atlas clear";
		return false;
	}
	if (const char* e = std::getenv("GX_SHADOWMAP_DUMP_EVERY"))   // TEST-ONLY: dump the atlas every N periodic lines (at most 6)
		m_smDumpEvery = uint32_t(std::strtoul(e, nullptr, 10));
	const char* fe = std::getenv("GX_SHADOWMAP_FILTER");   // TEST-ONLY (PREREG 2.1): hard = one compare tap, the cost table only
	m_smFilter = fe && std::strcmp(fe, "hard") == 0 ? 1u : 0u;
	logLine("[GXSHADOWMAP] state=on atlas=%ux%u %s capacity=%u depthClamp=%d receivers=1 filter=%s", kAtlasW, kAtlasH, f->name,
		kCasterCapacity, m_ctx.features().depthClamp ? 1 : 0, m_smFilter ? "hard" : "step");
	return true;
}

void GxNativeDevice::shadowMapAttach()
{
	m_writer.setLookAtlas(m_atlasView);
	m_writer.setShadowCasters(&m_casters);
	m_shadowMapOn = true;
}

void GxNativeDevice::shadowMapShutdown()
{
	m_writer.setShadowCasters(nullptr);
	m_casters.shutdown();
	if (m_smScratchView) {   // S7 A92: replay-k's scratch dies with the atlas
		vkDestroyImageView(m_ctx.device(), m_smScratchView, nullptr);
		m_smScratchView = VK_NULL_HANDLE;
	}
	if (m_smScratch.image)
		m_smScratch.destroy(m_gx);
	if (m_atlasView) {
		vkDestroyImageView(m_ctx.device(), m_atlasView, nullptr);
		m_atlasView = VK_NULL_HANDLE;
	}
	if (m_atlas.image)
		m_atlas.destroy(m_gx);
	m_shadowMapOn = false;
}

// Both commands are queued (cs): executed in the worker's exec half with owned arguments - S7 never records from the caller.
HRESULT GxNativeDevice::ShadowMap::BeginShadowFrame(const GxShadowFrame& f)
{
	GxNativeDevice* d = dev;
	const GxShadowFrame c = f;
	auto body = [d, c] {
		csTrace(GxCsOp::ShadowFrame, { uint64_t(c.step), uint64_t(c.cascades) }, nullptr);
		d->m_shadowLastOnWorker = d->m_cs.onWorker();
		d->m_writer.debug().lookFrameStale = d->m_debug.smLookFrameStale;   // TEST-ONLY plants (sm-receive / sm-rebind X- arms)
		d->m_writer.debug().lookNoRebind = d->m_debug.smNoRebind;
		d->m_writer.debug().lookNoDivide = d->m_debug.smNoDivide;
		Rec::GxShadowPassDesc p;
		p.atlas = &d->m_atlas;
		p.attachmentView = d->m_atlasView;
		p.cascades = c.step >= 3 ? 0u : std::min<uint32_t>(c.cascades, 2u);
		for (uint32_t i = 0; i < 2; ++i) {
			// a rectangle outside the atlas is clamped into it (never a render area past the image)
			const uint32_t x = std::min(c.rect[i][0], kAtlasW - 1), y = std::min(c.rect[i][1], kAtlasH - 1);
			const uint32_t w = std::min(c.rect[i][2], kAtlasW - x), h = std::min(c.rect[i][3], kAtlasH - y);
			p.rect[i] = { { int32_t(x), int32_t(y) }, { std::max(w, 1u), std::max(h, 1u) } };
			std::memcpy(p.lightViewProj[i], c.lightViewProj[i], sizeof(p.lightViewProj[i]));
		}
		std::memcpy(p.depthBias, c.depthBias, sizeof(p.depthBias));
		std::memcpy(p.ownPull, c.ownPull, sizeof(p.ownPull));   // A79: the own-light pull (L, eps; muMax, TEST-ONLY noRecv)
		std::memcpy(p.ownPullAux, c.ownPullAux, sizeof(p.ownPullAux));
		// P2: the receivers' inputs (the writer packs them into set 2's UBO at endFrame, GxLookFrame.h)
		p.step = c.step >= 3 ? 3u : c.step;
		p.shadowColor = c.shadowColor;
		std::memcpy(p.depthRangeWU, c.depthRangeWU, sizeof(p.depthRangeWU));
		std::memcpy(p.texelWU, c.texelWU, sizeof(p.texelWU));
		std::memcpy(p.tanTexel, c.tanTexel, sizeof(p.tanTexel));   // A39: the perspective map's tangent texel and near / far
		std::memcpy(p.nearFar, c.nearFar, sizeof(p.nearFar));
		p.guard = c.guard;
		p.filter = d->m_smFilter;
		d->m_casters.setPass(p);
	};
	if (d->m_debug.shadowCmdDirect)
		body();   // the plant (X-sm-queued): on the caller
	else
		d->cs(body);
	return D3D_OK;
}

HRESULT GxNativeDevice::ShadowMap::SetShadowTags(uint32_t tags)
{
	GxNativeDevice* d = dev;
	auto body = [d, tags] {
		csTrace(GxCsOp::ShadowTags, { uint64_t(tags) }, nullptr);
		d->m_shadowLastOnWorker = d->m_cs.onWorker();
		d->m_tr.shadowTags(tags);
	};
	if (d->m_debug.shadowCmdDirect)
		body();   // the plant (X-sm-queued): on the caller
	else
		d->cs(body);
	return D3D_OK;
}

// S7 A41 (integrator A40 T4): a clamped instance's own light, queued with an owned 8-byte argument exactly like
// the tags, executed in the worker's exec half in stream order with the draws it applies to
HRESULT GxNativeDevice::ShadowMap::SetOwnLight(uint64_t packed)
{
	GxNativeDevice* d = dev;
	auto body = [d, packed] {
		csTrace(GxCsOp::ShadowOwn, { packed }, nullptr);
		d->m_shadowLastOnWorker = d->m_cs.onWorker();
		d->m_tr.shadowOwn(packed);
	};
	if (d->m_debug.shadowCmdDirect)
		body();
	else
		d->cs(body);
	return D3D_OK;
}

// (the integrator's ruling after): A2C turns ITSELF off for the rest of the session - at init or in a
// Reset (an S body: no worker, no GPU work, no prebuild running). What the failed prewarm left (null keys) is dropped, the cache queues no
// more twins and the translator examines no draw: every later draw requests a plain key - today's modules (GxGenericShaders) and path.
void GxNativeDevice::a2cOff(const char* reason, const char* at)
{
	const size_t dropped = m_cache.dropPrebuildFailures();
	m_a2cOn = false;
	m_lookFeatures &= ~2u;
	m_cache.setA2CTwins(false);
	m_tr.setAlphaToCoverage(false);
	m_a2cPrewarmedSamples = 0;
	++m_a2cOffCount;
	logLine("[GXA2C] state=off reason=%s at=%s dropped=%zu (today's modules and path for the rest of the session)", reason, at, dropped);
}

// (A108; the integrator's ruling (1)): the translator's draws since the last line, A2C on and off by
// reason (GxAlphaToCoverage.h), so every gate run shows which guard excludes what - e.g. whether the trees are "on".
void GxNativeDevice::a2cReport(uint64_t presents)
{
	const Rec::GxTranslateStats& t = m_tr.stats();
	char buf[512];
	int n = std::snprintf(buf, sizeof(buf), "[GXA2C] presents=%llu on=%llu", (unsigned long long)presents,
		(unsigned long long)(t.a2cOn - m_a2cPrev[0]));
	m_a2cPrev[0] = t.a2cOn;
	for (uint32_t i = 1; i < GXRender::GxA2C_WhyCount && n > 0 && size_t(n) < sizeof(buf); ++i) {
		n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " off_%s=%llu", GXRender::GxA2CWhyName(GXRender::GxA2CWhy(i)),
			(unsigned long long)(t.a2cOff[i] - m_a2cPrev[i]));
		m_a2cPrev[i] = t.a2cOff[i];
	}
	// (a): A2C keys that failed on FIRST USE in this window (their draws were refused - a stated limit;
	// any key-on gate / picture / A108 run with one goes to the integrator before it is scored or shown)
	const uint32_t fu = m_cache.a2cFirstUseFailed();
	if (n > 0 && size_t(n) < sizeof(buf))
		std::snprintf(buf + n, sizeof(buf) - size_t(n), " firstuse_fail=%u", fu - m_a2cFirstUsePrev);
	m_a2cFirstUsePrev = fu;
	logLine("%s", buf);
}

// The periodic line: what the caster list did since the last line (deltas), so a run log reads like [GXGPU]'s. The TEST-ONLY dump
// (GX_SHADOWMAP_DUMP_EVERY=N: at every Nth line, at most 6) waits for the GPU, copies the whole D16 atlas to host memory and writes
// it as a 16-bit binary PGM (P5, maxval 65535, big-endian) into GX_SHADOWMAP_DUMP_DIR (default "."): the casters' silhouettes as the
// receivers will see them, before any receiver exists.
void GxNativeDevice::shadowMapReport(uint64_t presents)
{
	const Rec::GxCasterStats& s = m_casters.stats();
	const Rec::GxCasterStats& p = m_smPrev;
	logLine("[GXSHADOWMAP] presents=%llu frames=%llu captured=%llu cast_only=%llu late_buf=%llu late_tex=%llu format=%llu unsupported=%llu "
		"overflow=%llu replayed=%llu passes=%llu", (unsigned long long)presents, (unsigned long long)(s.frames - p.frames),
		(unsigned long long)(s.captured - p.captured), (unsigned long long)(s.castOnly - p.castOnly),
		(unsigned long long)(s.skippedLateBuf - p.skippedLateBuf), (unsigned long long)(s.skippedLateTex - p.skippedLateTex),
		(unsigned long long)(s.skippedFormat - p.skippedFormat), (unsigned long long)(s.unsupported - p.unsupported),
		(unsigned long long)(s.overflow - p.overflow), (unsigned long long)(s.replayedDraws - p.replayedDraws),
		(unsigned long long)(s.passes - p.passes));
	m_smPrev = s;
	// P2 (PREREG A2/A3): the receiver draws per composition class since the last line - the approximate share is measured, not argued
	const Rec::GxTranslateStats& t = m_tr.stats();
	logLine("[GXSHADOWMAP] presents=%llu rcv_exact=%llu rcv_additive=%llu rcv_nonlinear=%llu rcv_modulate=%llu rcv_alpha_skip=%llu "
		"castonly_dropped=%llu castonly_solid=%llu", (unsigned long long)presents, (unsigned long long)(t.rcvExact - m_smRcvPrev[0]),
		(unsigned long long)(t.rcvAdditive - m_smRcvPrev[1]), (unsigned long long)(t.rcvNonlinear - m_smRcvPrev[2]),
		(unsigned long long)(t.rcvModulate - m_smRcvPrev[3]), (unsigned long long)(t.rcvAlphaSkip - m_smRcvPrev[4]),
		(unsigned long long)(t.castOnlyDropped - m_smRcvPrev[5]), (unsigned long long)(t.castOnlySolid - m_smRcvPrev[6]));
	m_smRcvPrev[0] = t.rcvExact;
	m_smRcvPrev[1] = t.rcvAdditive;
	m_smRcvPrev[2] = t.rcvNonlinear;
	m_smRcvPrev[3] = t.rcvModulate;
	m_smRcvPrev[4] = t.rcvAlphaSkip;
	m_smRcvPrev[5] = t.castOnlyDropped;   // A44
	m_smRcvPrev[6] = t.castOnlySolid;     // A61
	if (m_smDumpEvery == 0 || m_smDumps >= 6 || (presents / kStatsEveryPresents) % m_smDumpEvery != 0)
		return;
	++m_smDumps;
	m_ctx.waitIdle();
	const VkDeviceSize bytes = VkDeviceSize(kAtlasW) * kAtlasH * 2;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = bytes;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation mem = nullptr;
	VmaAllocationInfo info = {};
	// integrator L2 (20:1xZ 18/10): through the allocation policy like every other host buffer (render-seam-lint RAWVMA), a site name of its own
	if (Vk::gxCreateBuffer(m_gx.allocator(), m_gx.allocPolicy(), "sm-atlas-dump", &bi, &ai, &buf, &mem, &info) != VK_SUCCESS ||
		!info.pMappedData) {
		logLine("[GXSHADOWMAP] dump: no host buffer");
		return;
	}
	Res::GxImage& a = m_atlas;
	const bool ok = m_gx.immediate([&](VkCommandBuffer cmd) {
		a.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
		c.imageExtent = { kAtlasW, kAtlasH, 1 };
		vkCmdCopyImageToBuffer(cmd, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
		a.transition(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	});
	if (ok) {
		vmaInvalidateAllocation(m_gx.allocator(), mem, 0, VK_WHOLE_SIZE);
		const char* dir = std::getenv("GX_SHADOWMAP_DUMP_DIR");
		char path[512];
		std::snprintf(path, sizeof(path), "%s/shadowatlas-%llu.pgm", dir ? dir : ".", (unsigned long long)presents);
		if (FILE* fp = std::fopen(path, "wb")) {
			std::fprintf(fp, "P5\n%u %u\n65535\n", kAtlasW, kAtlasH);
			const uint16_t* z = static_cast<const uint16_t*>(info.pMappedData);
			std::vector<uint8_t> row(size_t(kAtlasW) * 2);
			for (uint32_t y = 0; y < kAtlasH; ++y) {
				for (uint32_t x = 0; x < kAtlasW; ++x) {
					const uint16_t v = z[size_t(y) * kAtlasW + x];
					row[x * 2] = uint8_t(v >> 8);
					row[x * 2 + 1] = uint8_t(v & 0xff);
				}
				std::fwrite(row.data(), 1, row.size(), fp);
			}
			std::fclose(fp);
			logLine("[GXSHADOWMAP] dump %s", path);
		}
	} else
		logLine("[GXSHADOWMAP] dump: copy failed");
	vmaDestroyBuffer(m_gx.allocator(), buf, mem);
}

}} // namespace GXRender::Dev
