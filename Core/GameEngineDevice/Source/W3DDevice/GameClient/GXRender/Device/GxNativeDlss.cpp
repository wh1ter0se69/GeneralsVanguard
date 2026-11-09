// GeneralsX @feature B · dlss-streamline-1 13/10/2026 DLSS Super Resolution behind IGxWorldPass::Upscale (PREREG A36.2,
// modernize/render-dlss/DESIGN_dlss-streamline.md §3). Without GX_STREAMLINE, or without Streamline loaded (m_host.streamline
// null: not DLSS, not NVIDIA, no DLLs), every entry point is FALSE / E_NOTIMPL and the seam draws its own bilinear quad.
//
// Upscale records ONE outside-scope command (GxPacketWriter::outsideScope: the scope closes, fn runs, every cached binding is
// re-bound by the next draw - Streamline's manual "host restores state" is that): world colour, depth, motion vectors in the
// layouts they are tagged with -> DLSS into a device-owned RGBA8 storage image at output size -> blit (same size, format
// conversion only) into the seam's destination rect. Render-only: nothing here is read by the simulation.
#include "GxNativeDevice.h"
#include "GxFgUiAlpha.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(GX_STREAMLINE)
#include "GxStreamline.h"
#endif
#if defined(_WIN32)
#include <tlhelp32.h>
#endif

namespace GXRender { namespace Dev {

#if defined(GX_STREAMLINE)
namespace
{
// B · dlss-fg-1 (PREREG A39b): set by the Windows message loop, consumed by the device at its next frame (PCL guide 1: "set a flag for
// the next simulation to send the ping marker" - the frame id is what matters, not the instant).
std::atomic<uint32_t> g_pclMessage{ 0 };	// PCL Stats' ping message id, 0 = Reflex not running
std::atomic<bool> g_pclPingPending{ false };
std::atomic<bool> g_flashPending{ false };

uint64_t g_dlssEvaluations = 0;	// the whole run's DLSS evaluations, for the exit census (device thread writes, atexit reads)

// PREREG A36.2 gate (1): which NVIDIA Streamline / DLSS modules this process holds, printed at process exit in EVERY native
// session of a GX_STREAMLINE build (key off must say sl=0). At exit, not at device teardown: the game exits without destroying
// its device. Windows only; elsewhere nothing of Streamline can load.
void logStreamlineModules()
{
#if defined(_WIN32)
	std::string names;
	uint32_t n = 0;
	const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W me = {};
		me.dwSize = sizeof(me);
		for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
			const wchar_t* b = me.szModule;
			if (_wcsnicmp(b, L"sl.", 3) == 0 || _wcsnicmp(b, L"nvngx_dlss", 10) == 0) {
				char a[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, b, -1, a, MAX_PATH, nullptr, nullptr);
				names += std::string(n ? "," : "") + a;
				++n;
			}
		}
		CloseHandle(snap);
	}
	std::fprintf(stderr, "[GXSL] modules sl=%u%s%s evaluations=%llu\n", n, n ? " " : "", names.c_str(),
		(unsigned long long)g_dlssEvaluations);
	std::fflush(stderr);
#endif
}
} // namespace
#endif

#if defined(GX_STREAMLINE)

void GxNativeLoadStreamline(IDirect3D8* dxvk, GxNativeHost& host, bool wantDlss, int reflex, int frameGen)
{
	static bool s_census = false;	// A36.2 gate (1): the module census at exit, every native session of this build
	if (!s_census) {
		s_census = true;
		std::atexit(&logStreamlineModules);
	}
	// TEST-ONLY GX_STREAMLINE_FORCE_LOAD=1: the interposer as loader root with the upscaler off - the A36.2 overhead arm
	const char* force = std::getenv("GX_STREAMLINE_FORCE_LOAD");
	const bool forced = force && std::strcmp(force, "1") == 0;
	// TEST-ONLY GX_REFLEX_MEASURE_ONLY=1 (PREREG A39e's baseline arm): Reflex loads for its PCL markers - so PresentMon can read PC
	// latency - with low latency OFF, i.e. today's latency behaviour, measurable.
	const char* measure = std::getenv("GX_REFLEX_MEASURE_ONLY");
	const bool measureOnly = measure && std::strcmp(measure, "1") == 0;
	if (measureOnly && reflex <= 0)
		reflex = 1;
	if (!wantDlss && reflex <= 0 && frameGen <= 0 && !forced)
		return;	// every key off: nothing of Streamline loads (no sl.* module in the process)
	// Adapter 0: the one DX8Wrapper::Set_Any_Render_Device tries first. ponytail: a device made on another adapter that is not
	// NVIDIA would meet Streamline's device proxy and fail vkCreateDevice -> native refuses, classic runs (logged, no crash);
	// gate at CreateDevice(adapter) if multi-adapter selection ever ships (Codex dlss-sl-r1 P2).
	D3DADAPTER_IDENTIFIER8 id = {};
	if (!dxvk || FAILED(dxvk->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id)) || id.VendorId != 0x10DE) {
		// A36.1: on a non-NVIDIA device Streamline's device proxy makes vkCreateDevice fail (VK_NVX_binary_import)
		std::fprintf(stderr, "[GXSL] not loaded: adapter vendor 0x%04lx is not NVIDIA\n", (unsigned long)id.VendorId);
		std::fflush(stderr);
		return;
	}
	std::wstring dir;
	if (const wchar_t* e = _wgetenv(L"GX_STREAMLINE_DIR"))
		dir = e;
	else {
		wchar_t exe[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
		dir.assign(exe, n);
		const size_t slash = dir.find_last_of(L"\\/");
		dir = (slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash)) + L"\\Streamline";
	}
	// Never destroyed, like the device it serves (GxNativeDevice.cpp: the device lives to process exit by design): NO slShutdown at
	// a normal exit - NVIDIA's telemetry shutdown hangs outside a console session (A36.1) and the OS reclaims everything. slShutdown
	// runs only in a device teardown (dlssShutdown). A static object here ran slShutdown at exit (A36.2's census showed the
	// plugins unloaded); render-integration-5 + dlss-integrator ruled against that 13/10.
	static Upscale::Streamline* const s_streamline = new Upscale::Streamline();
	std::string why;
	Upscale::StreamlineFeatures want;
	want.dlss = wantDlss || forced;
	want.reflexMode = reflex > 0 ? uint32_t(reflex) : 0u;
	want.frameGen = frameGen > 0 ? uint32_t(frameGen) : 0u;
	want.reflexLowLatencyOff = measureOnly;
	if (!s_streamline->load(dir.c_str(), want, &why)) {
		std::fprintf(stderr, "[GXSL] not loaded: %s\n", why.c_str());
		std::fflush(stderr);
		return;
	}
	host.getInstanceProcAddr = s_streamline->getInstanceProcAddr();
	host.loaderInterposer = s_streamline->module();
	host.streamline = s_streamline;
	std::fprintf(stderr, "[GXSL] loaded: NVIDIA Streamline interposer is the Vulkan loader root (adapter \"%s\")\n", id.Description);
	std::fprintf(stderr, "[GXSL] features dlss=%d reflex=%u framegen=%u\n", want.dlss ? 1 : 0, s_streamline->features().reflexMode,
		s_streamline->features().frameGen);
	std::fflush(stderr);
}

void GxNativeNoteWindowsMessage(unsigned message)
{
	const uint32_t pcl = g_pclMessage.load(std::memory_order_relaxed);
	if (pcl == 0)
		return;	// no Reflex in this session: nothing to do, one load per message
	if (message == pcl)
		g_pclPingPending.store(true);
	else if (message == 0x0201u)	// WM_LBUTTONDOWN: the Reflex flash indicator
		g_flashPending.store(true);
}

struct GxNativeDevice::Dlss
{
	Upscale::Streamline* sl = nullptr;
	bool supported = false;
	// RGBA8_UNORM: the format table maps A8R8G8B8 to BGRA8, whose storage use is optional; this one is checked at init.
	Res::FormatInfo outFormat;
	Res::GxImage out;
	VkImageView outView = VK_NULL_HANDLE;
	bool refused[6] = {};	// one log line per reason
	uint64_t evaluations = 0;
	uint64_t lastPresent = ~0ull;	// one evaluation per presented frame (Streamline's frame token, DLSS history)
	// B · dlss-fg-1 (PREREG A39): the per-frame contract + Reflex + DLSS Frame Generation
	bool frameBegun = false;	// a Streamline frame is open (frameBegin succeeded)
	bool sceneMarked = false;	// this frame's first BeginScene was marked
	bool frameFailLogged = false;
	uint32_t reflexMode = 0;	// set and accepted
	uint32_t fgWanted = 0;	// GXFrameGen: generated frames per real one
	bool fgSupported = false;
	bool fgProbe = false;	// TEST-ONLY GX_FG_PROBE=1: switch frame generation on before P3 lands (first light, never merged)
	bool fgSubrect = false;	// TEST-ONLY GX_FG_SUBRECT=1: frame generation on the camera viewport only
	bool fgOn = false;	// slDLSSGSetOptions(eOn) accepted
	bool fgRefused = false;	// tried and refused: never retried this session
	bool fgSubrectLogged = false;
	// FG P0 H3 (F8, DLSS_G guide §0 / §17 / §18): frame generation OFF before a swapchain recreation (resize, minimise, a dirty or
	// out-of-date swapchain) and the swapchain recreated on every on/off toggle. fgSuspended: off for a recreation, back on once
	// kFgStableFrames frames were presented with no recreation; fgToggleRecreate: the recreation that follows turning it back on (it does
	// not suspend again); fgStableFrom: m_stats.presents at the last recreation.
	uint32_t fgGenerated = 0;	// the generated count slDLSSGSetOptions(eOn) accepted (the resume asks for the same)
	bool fgSuspended = false;
	bool fgToggleRecreate = false;
	uint64_t fgStableFrom = 0;
	uint32_t fgSuspends = 0, fgResumes = 0;
	uint64_t testRecreateAt[4] = {};	// TEST-ONLY GX_TEST_FG_RECREATE_AT=<present>[,<present>...]: force a swapchain recreation there
	uint32_t fgLastStatus = ~0u;
	// PREREG A39c (TEST-ONLY GX_FG_HUDLESS=1): the scene without HUD. hudMS = a copy of the back buffer at HudBegin (its sample count);
	// hud1x = that copy resolved with the present's own shader (DXVK's linear-light resolve: the same pixels the final frame gets).
	bool hudlessWanted = false;
	bool hudCaptured = false;
	bool hudLogged = false;
	std::unique_ptr<Res::GxRenderTarget> hudMS, hud1x;
	std::unique_ptr<GxNativePresent> hudPresent;
	SeamSlots hudUse;	// its descriptor slots: one is reused only after the submission that last used it completed (Codex fg-p2-r1 P2)
	// PREREG A39c v1 (TEST-ONLY GX_FG_UIALPHA=1, with GX_FG_HUDLESS=1): UI alpha = where the final frame differs from the HUD-less one.
	// final1x = the back buffer (with HUD) resolved by the same shader on slot + N of hudPresent; alpha = GxFgUiAlpha::kAlphaFormat at back-buffer size.
	bool uiAlphaWanted = false;
	std::unique_ptr<Res::GxRenderTarget> final1x;
	std::unique_ptr<GxFgUiAlpha> uiAlpha;
	Res::FormatInfo alphaFormat;
	Res::GxImage alpha;
	VkImageView alphaView = VK_NULL_HANDLE;
	uint64_t winReal = 0, winShown = 0, totalReal = 0, totalShown = 0;
	uint32_t winHudTags = 0, winAlphaTags = 0;	// tags Streamline accepted this window: a run proves its own HUD-less / UI-alpha inputs
	std::chrono::steady_clock::time_point winStart{};
};

namespace
{
Upscale::DlssImage dlssImage(const Res::GxImage& i, VkImageView view, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	Upscale::DlssImage d;
	d.image = i.image;
	d.view = view;
	d.layout = i.layout;
	d.format = i.format ? i.format->vk : VK_FORMAT_UNDEFINED;
	d.width = i.width;
	d.height = i.height;
	d.usage = usage;
	d.aspect = aspect;
	return d;
}

// The DLSS mode whose scale is nearest the seam's (GXUpscaleQuality decided the render size; any size inside the mode's
// min..max is accepted). The mode picks NVIDIA's preset only.
// The published frame's depth / motion vectors (render-motion-1's GxMotionFrame: by value, in the layout the device keeps each in when
// this command runs - DEPTH_STENCIL_READ_ONLY / SHADER_READ_ONLY after its resolve). DLSS tags them as they are and transitions nothing
// of theirs (Streamline hands every tagged image back in its tagged layout).
Upscale::DlssImage motionImage(const GxMotionImage& m, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	Upscale::DlssImage d;
	d.image = m.image;
	d.view = m.view;
	d.layout = m.layout;
	d.format = m.format;
	d.width = m.width;
	d.height = m.height;
	d.usage = usage;
	d.aspect = aspect;
	return d;
}

Upscale::DlssMode modeFor(uint32_t renderW, uint32_t outputW)
{
	const float r = renderW ? float(outputW) / float(renderW) : 1.0f;
	if (r >= 2.5f)
		return Upscale::DlssMode::UltraPerformance;	// 3.0
	if (r >= 1.85f)
		return Upscale::DlssMode::MaxPerformance;	// 2.0
	if (r >= 1.62f)
		return Upscale::DlssMode::Balanced;	// 1.72
	if (r >= 1.2f)
		return Upscale::DlssMode::MaxQuality;	// 1.5
	return Upscale::DlssMode::DLAA;	// 1.0
}
} // namespace

void GxNativeDevice::dlssInit()
{
	if (!m_host.streamline)
		return;
	m_dlss = new Dlss();
	m_dlss->sl = m_host.streamline;
	std::string why;
	const Upscale::StreamlineFeatures& feat = m_dlss->sl->features();
	m_dlss->sl->resolveFeatures();	// Codex fg-p0-r1 P1: only now does Streamline answer feature-function queries
	if (feat.dlss)
		m_dlss->supported = m_dlss->sl->dlssSupported(m_ctx.physicalDevice(), &why);
	else
		why = "not asked (GXUpscaler is not dlss)";
	VkFormatProperties fp = {};
	vkGetPhysicalDeviceFormatProperties(m_ctx.physicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &fp);
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	if (m_dlss->supported && (fp.optimalTilingFeatures & need) != need) {
		m_dlss->supported = false;
		why = "RGBA8 has no storage + blit-src on this device";
	}
	m_dlss->outFormat.vk = VK_FORMAT_R8G8B8A8_UNORM;
	m_dlss->outFormat.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	m_dlss->outFormat.blockBytes = 4;
	m_dlss->outFormat.features = fp.optimalTilingFeatures;
	m_dlss->outFormat.name = "R8G8B8A8_UNORM (dlss out)";
	logLine("[GXSL] dlss=%s %s%s%s", m_dlss->supported ? "supported" : "unsupported", m_dlss->sl->versions().c_str(),
		why.empty() ? "" : " why=", why.c_str());
	if (m_dlss->supported) {
		setTemporalConsumer(true);	// render-motion-1: the world pass jitters only for a backend that consumes it
		m_motion.setMinJitterPhases(16);	// DLSS wants >= 16 phases
	}
	// B · dlss-fg-1: Reflex (mandatory with frame generation) and DLSS-G. Missing plugins = that feature off, logged.
	if (feat.reflexMode) {
		std::string w;
		const uint32_t mode = feat.reflexLowLatencyOff ? 0u : feat.reflexMode;
		const bool ok = m_dlss->sl->reflexSetMode(mode, &w);
		m_dlss->reflexMode = ok ? feat.reflexMode : 0u;	// markers + sleep run whenever Reflex is loaded (Reflex guide 4.0)
		logLine("[GXREFLEX] mode=%u%s %s%s", mode, feat.reflexLowLatencyOff ? " (TEST-ONLY GX_REFLEX_MEASURE_ONLY: markers only)" : "",
			ok ? "set (frame limit 0: FramePacer caps)" : "FAILED: ", ok ? "" : w.c_str());
		g_pclMessage.store(m_dlss->sl->statsWindowMessage());
		logLine("[GXREFLEX] pcl ping message=0x%x", g_pclMessage.load());
	}
	if (feat.frameGen) {
		std::string w;
		m_dlss->fgWanted = feat.frameGen;
		m_dlss->fgSupported = m_dlss->sl->frameGenSupported(m_ctx.physicalDevice(), &w);
		const char* probe = std::getenv("GX_FG_PROBE");
		const char* sub = std::getenv("GX_FG_SUBRECT");
		if (const char* rc = std::getenv("GX_TEST_FG_RECREATE_AT")) {	// TEST-ONLY (FG P0 H3 on wh1ter0se's PC): forced recreations
			uint32_t n = 0;
			for (const char* p = rc; *p && n < 4; ++n) {
				m_dlss->testRecreateAt[n] = std::strtoull(p, const_cast<char**>(&p), 10);
				if (*p == ',')
					++p;
				else
					break;
			}
		}
		m_dlss->fgProbe = probe && std::strcmp(probe, "1") == 0;
		m_dlss->fgSubrect = sub && std::strcmp(sub, "1") == 0;
		const char* hud = std::getenv("GX_FG_HUDLESS");
		m_dlss->hudlessWanted = hud && std::strcmp(hud, "1") == 0;
		const char* ua = std::getenv("GX_FG_UIALPHA");
		m_dlss->uiAlphaWanted = m_dlss->hudlessWanted && ua && std::strcmp(ua, "1") == 0;
		logLine("[GXFG] framegen=%u supported=%d probe=%d subrect=%d hudless=%d%s%s", feat.frameGen, m_dlss->fgSupported ? 1 : 0,
			m_dlss->fgProbe ? 1 : 0, m_dlss->fgSubrect ? 1 : 0, m_dlss->hudlessWanted ? 1 : 0, w.empty() ? "" : " why=", w.c_str());
	}
	// The frame contract starts with frame 0, NOW: every DLSS evaluate of this session then names a begun frame. Without it the first
	// evaluates took tokens from motion's frame index and the presents-based ids met them again - Streamline refused the second set of
	// constants for that frame (sl::Result 27) and DLSS switched itself off (first-light arm A, 14/10 04:09Z).
	if (m_dlss->reflexMode || m_dlss->fgWanted) {
		std::string w;
		m_dlss->frameBegun = m_dlss->sl->frameBegin(0u, &w);
		if (!m_dlss->frameBegun)
			logLine("[GXSL] frame begin failed (Reflex/DLSS-G get no frames): %s", w.c_str());
	}
}

void GxNativeDevice::slSceneBegin()
{
	if (!m_dlss || !m_dlss->frameBegun || m_dlss->sceneMarked)
		return;
	m_dlss->sceneMarked = true;
	m_dlss->sl->marker(Upscale::PclMarker::SimulationEnd);
	m_dlss->sl->marker(Upscale::PclMarker::RenderSubmitStart);
}

void GxNativeDevice::fgHudBegin()
{
	if (!m_dlss || !m_dlss->fgOn || !m_dlss->hudlessWanted || !m_haveSwapchain || m_dlss->hudCaptured)
		return;
	Dlss& d = *m_dlss;
	Res::GxRenderTarget* bb = backBufferStorage(0);
	if (!bb)
		return;
	// The seam composites into the BOUND target: under pillarboxing that is an off-screen surface copied to the back buffer only after
	// the HUD (Pillarbox_End) - the back buffer would hold last frame's picture (Codex fg-p2-r1 P2). Capture only a back-buffer target.
	if (!m_rt || m_rt->kind() != GxNativeSurface::Kind::BackBuffer) {
		if (!d.hudLogged) {
			d.hudLogged = true;
			logLine("[GXFG] hudless off this frame: the bound target is not the back buffer (pillarbox) - not supported in v0");
		}
		return;
	}
	Res::GxImage& src = bb->image();
	// DLSS-G wants the HUD-less colour at the PRESENTED image's size: only when the present is the back buffer 1:1 (a scaled or
	// pillarboxed present would need the same mapping - not in v0)
	const VkExtent2D ext = m_swapchain.extent();
	if (ext.width != src.width || ext.height != src.height) {
		if (!d.hudLogged) {
			d.hudLogged = true;
			logLine("[GXFG] hudless off: presented %ux%u is not the back buffer %ux%u 1:1", ext.width, ext.height, src.width, src.height);
		}
		return;
	}
	const bool ms = src.samples != VK_SAMPLE_COUNT_1_BIT;
	auto fits = [&](const std::unique_ptr<Res::GxRenderTarget>& t, bool multi) {
		return t && t->image().width == src.width && t->image().height == src.height && t->image().format == src.format &&
			t->image().samples == (multi ? src.samples : VK_SAMPLE_COUNT_1_BIT);
	};
	std::string why;
	auto make = [&](std::unique_ptr<Res::GxRenderTarget>& t, bool multi) {
		if (fits(t, multi))
			return true;
		if (t) {
			forgetImage(&t->image());
			t.reset();	// ponytail: a resize mid-run frees under the GPU only if a frame still uses it - v0 runs at one size
		}
		auto n = std::make_unique<Res::GxRenderTarget>();
		Res::RenderTargetDesc rd;
		rd.width = src.width;
		rd.height = src.height;
		rd.format = m_pp.BackBufferFormat;
		rd.samples = multi ? uint32_t(src.samples) : 0u;
		rd.sampled = multi;	// the present shader samples it
		if (!n->init(m_gx, rd, &why))
			return false;
		t = std::move(n);
		return true;
	};
	if (!make(d.hudMS, ms) || (ms && !make(d.hud1x, false)) || (d.uiAlphaWanted && ms && !make(d.final1x, false))) {
		if (!d.hudLogged) {
			d.hudLogged = true;
			logLine("[GXFG] hudless off: target %s", why.c_str());
		}
		d.hudlessWanted = false;
		return;
	}
	if (d.uiAlphaWanted && (!d.alpha.image || d.alpha.width != src.width || d.alpha.height != src.height)) {
		if (d.alpha.image) {	// ponytail: v0/v1 run at one size; a mid-run resize frees under the GPU only if a frame still uses it
			forgetImage(&d.alpha);
			if (d.alphaView)
				vkDestroyImageView(m_ctx.device(), d.alphaView, nullptr);
			d.alpha.destroy(m_gx);
			d.alpha = Res::GxImage();
			d.alphaView = VK_NULL_HANDLE;
		}
		VkFormatProperties fp = {};
		vkGetPhysicalDeviceFormatProperties(m_ctx.physicalDevice(), GxFgUiAlpha::kAlphaFormat, &fp);
		d.alphaFormat.vk = GxFgUiAlpha::kAlphaFormat;
		d.alphaFormat.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		d.alphaFormat.blockBytes = 2;
		d.alphaFormat.features = fp.optimalTilingFeatures;
		d.alphaFormat.name = "R16_SFLOAT (DLSS-G ui alpha)";
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		const bool made = d.alpha.create(m_gx, &d.alphaFormat, src.width, src.height, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &why);
		if (made) {
			vi.image = d.alpha.image;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = GxFgUiAlpha::kAlphaFormat;
			vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		}
		if (!made || vkCreateImageView(m_ctx.device(), &vi, nullptr, &d.alphaView) != VK_SUCCESS) {
			logLine("[GXFG] ui alpha off: alpha target %s", why.c_str());
			d.uiAlphaWanted = false;
		}
	}
	if (d.uiAlphaWanted && !d.uiAlpha) {
		auto a = std::make_unique<GxFgUiAlpha>();
		if (!a->init(m_ctx, uint32_t(m_presentWaits.size()), &why)) {
			a->shutdown();
			logLine("[GXFG] ui alpha off: %s", why.c_str());
			d.uiAlphaWanted = false;
		} else
			d.uiAlpha = std::move(a);
	}
	if (d.hudUse.lastUse.empty())	// the slot ring for the resolve AND the alpha pass - single-sample alpha needs it too (Codex fg-p2-r3 P2)
		d.hudUse.reset(uint32_t(m_presentWaits.size()));
	if (ms && !d.hudPresent) {
		auto p = std::make_unique<GxNativePresent>();
		// 2 x the slots: slot = the HUD-less resolve, slot + N = the final frame's (UI alpha)
		if (!p->init(m_ctx, 2u * uint32_t(m_presentWaits.size()), &why) || !p->prepare(d.hud1x->image().format->vk, src.samples, &why)) {
			p->shutdown();	// what init made (Codex fg-p2-r1 P3)
			logLine("[GXFG] hudless off: resolve pipeline %s", why.c_str());
			d.hudlessWanted = false;
			return;
		}
		d.hudPresent = std::move(p);
	}
	ensureFrame();
	Res::GxImage& dst = d.hudMS->image();
	Res::GxImage* touched[2] = { &src, &dst };
	m_writer.outsideScope([&](VkCommandBuffer cmd) {
		src.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		dst.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkImageCopy c = {};
		c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		c.extent = { src.width, src.height, 1 };
		vkCmdCopyImage(cmd, src.image, src.layout, dst.image, dst.layout, 1, &c);
	}, touched, 2);
	d.hudCaptured = true;
	if (!d.hudLogged) {
		d.hudLogged = true;
		logLine("[GXFG] hudless on: %ux%u samples=%u (copied at HudBegin, %s)", src.width, src.height, unsigned(src.samples),
			ms ? "resolved with the present's shader" : "single-sample");
	}
}

// Before presentToSwapchain (its slot): the HUD-less copy becomes what DLSS-G reads at the present.
void GxNativeDevice::fgHudlessTag()
{
	if (!m_dlss || !m_dlss->hudCaptured)
		return;
	Dlss& d = *m_dlss;
	d.hudCaptured = false;
	const bool ms = d.hudMS->image().samples != VK_SAMPLE_COUNT_1_BIT;
	const bool alpha = d.uiAlphaWanted && d.uiAlpha && d.alpha.image && (!ms || d.final1x);
	Res::GxRenderTarget* bb = backBufferStorage(0);
	uint32_t slot = 0;
	ensureFrame();
	if (ms || alpha) {
		slot = d.hudUse.take(m_ctx, m_gx.nextUseValue());	// waits for the slot's last submission; ~0u = decline this frame
		if (slot == ~0u)
			return;	// no tag this frame: DLSS-G interpolates without the HUD-less input once
	}
	const uint32_t n = uint32_t(m_presentWaits.size());
	Res::GxImage& in = d.hudMS->image();
	Res::GxImage& out = ms ? d.hud1x->image() : in;
	const VkImageView outView = ms ? d.hud1x->attachmentView() : d.hudMS->attachmentView();
	// UI alpha's "final": the back buffer (HUD drawn) resolved by the same shader - or the back buffer itself when single-sample
	Res::GxImage* fin = alpha && bb ? (ms ? &d.final1x->image() : &bb->image()) : nullptr;
	const VkImageView finView = fin ? (ms ? d.final1x->attachmentView() : bb->attachmentView()) : VK_NULL_HANDLE;
	Res::GxImage* touched[5] = { &in, ms ? &out : nullptr, fin, fin && ms ? &bb->image() : nullptr, fin ? &d.alpha : nullptr };
	Res::GxImage* list[5];
	uint32_t count = 0;
	for (Res::GxImage* t : touched)
		if (t)
			list[count++] = t;
	bool ok = true, alphaOk = fin != nullptr;
	bool hudTagged = false, alphaTagged = false;	// set only by a tagging call that ran: outsideScope skips fn when no frame is open (Codex fg-p2-r6 P2)
	std::string why;
	m_writer.outsideScope([&](VkCommandBuffer cmd) {
		if (ms)
			ok = d.hudPresent->record(cmd, slot, in, d.hudMS->attachmentView(), out, outView, false);	// leaves out in TRANSFER_SRC
		if (ok && fin && ms)
			alphaOk = d.hudPresent->record(cmd, slot + n, bb->image(), bb->attachmentView(), *fin, finView, false);
		if (ok && fin && alphaOk)	// leaves final, hudless and alpha in SHADER_READ_ONLY
			alphaOk = d.uiAlpha->record(cmd, slot, *fin, finView, out, outView, d.alpha, d.alphaView);
		if (ok && !(fin && alphaOk) && !ms)
			out.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		if (ok)
			hudTagged = ok = d.sl->tagHudless(cmd, dlssImage(out, outView, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT), &why);
		if (ok && fin && alphaOk)
			alphaTagged = alphaOk = d.sl->tagUiAlpha(cmd, dlssImage(d.alpha, d.alphaView, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT), &why);
	}, list, count);
	if (hudTagged)
		++d.winHudTags;
	if (alphaTagged)
		++d.winAlphaTags;
	if (fin && !alphaOk) {
		logLine("[GXFG] ui alpha off: %s", why.empty() ? "pass failed" : why.c_str());
		d.uiAlphaWanted = false;
	}
	if (!ok) {
		logLine("[GXFG] hudless off: %s", why.empty() ? "resolve failed" : why.c_str());
		d.hudlessWanted = false;
	}
}

void GxNativeDevice::slPresentBegin()
{
	fgHudlessTag();
	if (!m_dlss || !m_dlss->frameBegun)
		return;
	m_dlss->sl->marker(Upscale::PclMarker::RenderSubmitEnd);
}

namespace
{
// FG P0 H3: ePresentStart / ePresentEnd bracket vkQueuePresentKHR ALONE (they bracketed the whole presentToSwapchain - acquire, the frame's
// submission, the present - before), on the thread that calls it, for the frame whose token the hook carries (F9: the presented frame's
// id). ctx = the Streamline; token = its frame handle. They call Streamline only - never back into the VkCtx (the hook's contract).
void slHookStart(void* ctx, uint64_t token)
{
	static_cast<Upscale::Streamline*>(ctx)->markerOn(Upscale::PclMarker::PresentStart, reinterpret_cast<void*>(uintptr_t(token)));
}
void slHookEnd(void* ctx, uint64_t token, VkResult)
{
	static_cast<Upscale::Streamline*>(ctx)->markerOn(Upscale::PclMarker::PresentEnd, reinterpret_cast<void*>(uintptr_t(token)));
}
}

bool GxNativeDevice::slPresentHook(Vk::PresentHook* out)
{
	// the A23-B worker never presents with Streamline on (d6417fe48's guard keeps it off): a defence, not a path
	if (!m_dlss || !m_dlss->frameBegun || m_cs.onWorker() || !m_dlss->sl->frameHandle())
		return false;
	out->start = &slHookStart;
	out->end = &slHookEnd;
	out->ctx = m_dlss->sl;
	out->token = uint64_t(reinterpret_cast<uintptr_t>(m_dlss->sl->frameHandle()));
	return true;
}

// After the present is counted (thread 0: this IS the presenting thread - DLSS_G guide §6.0 / §15.1 want DLSS-G's options and state
// there). Then the next frame begins: its id is the number of frames presented so far.
// FG P0 H3 (F8): before presentToSwapchain recreates the swapchain - frame generation off (eOff keeps its resources:
// eRetainResourcesWhenOff), unless this recreation is the one that follows turning it back on
void GxNativeDevice::slBeforeRecreate()
{
	if (!m_dlss)
		return;
	Dlss& d = *m_dlss;
	if (d.fgToggleRecreate) {
		d.fgToggleRecreate = false;
		return;
	}
	if (!d.fgOn)
		return;
	std::string w;
	const bool off = d.sl->frameGenSetOptions(false, d.fgGenerated, &w);
	if (!off) {   // Codex fg3-h3-r1 P2: not off - say so, keep fgOn (the next recreation tries again), never resume
		d.fgRefused = true;
		logLine("[GXFG] slDLSSGSetOptions(eOff) before a swapchain recreation FAILED: %s - frame generation state unchanged (tried "
			"again before the next recreation), no resume this session", w.c_str());
		return;
	}
	d.fgOn = false;
	d.fgSuspended = true;
	++d.fgSuspends;
	logLine("[GXFG] suspended for a swapchain recreation (#%u): slDLSSGSetOptions(eOff) %s%s - back on after %u presented frames with no "
		"recreation", d.fgSuspends, "accepted", "", kFgStableFrames);
}

// after it (ok or not): the stable window starts again
void GxNativeDevice::slAfterRecreate()
{
	if (m_dlss)
		m_dlss->fgStableFrom = m_stats.presentsShown;   // shown presents only (Codex fg3-h3-r1 P2: a skipped Present is no stable frame)
}

void GxNativeDevice::slFrameEnd()
{
	if (!m_dlss || !(m_dlss->reflexMode || m_dlss->fgWanted))
		return;	// DLSS SR alone: no frame contract, as before (its evaluate takes its own token)
	Dlss& d = *m_dlss;
	for (uint64_t at : d.testRecreateAt)
		if (at && m_stats.presents == at) {	// TEST-ONLY: the next present recreates the swapchain, as a resize would
			m_swapchainDirty = true;
			logLine("[GXFG] TEST GX_TEST_FG_RECREATE_AT: swapchain recreation forced at present %llu", (unsigned long long)at);
		}
	if (d.fgSuspended && !d.fgRefused && m_haveSwapchain && m_stats.presentsShown >= d.fgStableFrom + kFgStableFrames) {
		std::string w;
		d.fgOn = d.sl->frameGenSetOptions(true, d.fgGenerated, &w);
		d.fgSuspended = false;
		d.fgRefused = !d.fgOn;
		d.fgToggleRecreate = d.fgOn;
		if (d.fgOn)
			m_swapchainDirty = true;	// §18: recreated when it turns back on, too - a failed eOn leaves a pending request alone (Codex fg3-h3-r1 P2)
		++d.fgResumes;
		logLine("[GXFG] resumed after %u presented frames with no recreation: slDLSSGSetOptions(eOn, generated=%u) %s%s%s", kFgStableFrames,
			d.fgGenerated, d.fgOn ? "accepted" : "FAILED: ", d.fgOn ? "" : w.c_str(), d.fgOn ? " - swapchain recreated for the toggle" :
			" - frame generation stays off");
	}
	if (d.fgWanted && d.fgSupported && d.fgProbe && !d.fgOn && !d.fgRefused && !d.fgSuspended && m_haveSwapchain) {
		// Codex fg-p0-r1 P2: the system's maximum first (slDLSSGGetState answers before frame generation is on); never ask for more
		std::string w;
		Upscale::FrameGenState pre;
		uint32_t generated = d.fgWanted;
		if (!d.sl->frameGenState(&pre, &w) || pre.maxGenerated == 0) {
			d.fgRefused = true;
			logLine("[GXFG] refused: no frame-generation maximum from slDLSSGGetState (%s) - frame generation stays off",
				w.empty() ? "max 0" : w.c_str());
		} else {
			if (generated > pre.maxGenerated) {
				logLine("[GXFG] GXFrameGen=%u > this system's max_generated=%u: clamped", generated, pre.maxGenerated);
				generated = pre.maxGenerated;
			}
			d.fgOn = d.sl->frameGenSetOptions(true, generated, &w);
			d.fgGenerated = d.fgOn ? generated : 0;
			d.fgRefused = !d.fgOn;
			logLine("[GXFG] FIRST LIGHT (TEST-ONLY GX_FG_PROBE, not the product): slDLSSGSetOptions(eOn, generated=%u of max %u) %s%s",
				generated, pre.maxGenerated, d.fgOn ? "accepted" : "FAILED: ", d.fgOn ? "" : w.c_str());
		}
	}
	if (d.fgOn) {
		Upscale::FrameGenState st;
		std::string w;
		if (d.sl->frameGenState(&st, &w)) {
			d.winShown += st.presented;
			d.totalShown += st.presented;
			if (st.status != d.fgLastStatus) {
				logLine("[GXFG] status=0x%x max_generated=%u min_size=%u (0 = OK; 2 = Reflex not detected; 8 = constants invalid)",
					st.status, st.maxGenerated, st.minWidthOrHeight);
				d.fgLastStatus = st.status;
			}
		}
	}
	if (d.frameBegun) {
		++d.winReal;
		++d.totalReal;
	}
	// once a second: REAL rendered fps, SHOWN fps and Reflex's latency side by side (never shown fps alone - ri5's §7 condition)
	const auto now = std::chrono::steady_clock::now();
	if (d.winStart == std::chrono::steady_clock::time_point{})
		d.winStart = now;
	const double secs = std::chrono::duration<double>(now - d.winStart).count();
	if (secs >= 1.0) {
		Upscale::ReflexLatency lat;
		const bool haveLat = d.sl->reflexLatest(&lat);
		logLine("[GXFG] t=%.2f real_fps=%.1f shown_fps=%.1f fg=%d reflex=%u lat_frame=%llu sim_to_submit_end_us=%llu "
			"sim_to_present_end_us=%llu sim_to_gpu_end_us=%llu gpu_frame_us=%u gpu_active_us=%u hudless_tags=%u ui_alpha_tags=%u", secs, double(d.winReal) / secs,
			d.fgOn ? double(d.winShown) / secs : double(d.winReal) / secs, d.fgOn ? 1 : 0, d.reflexMode,
			haveLat ? (unsigned long long)lat.frameId : 0ull, haveLat ? (unsigned long long)lat.simToRenderSubmitEnd : 0ull,
			haveLat ? (unsigned long long)lat.simToPresentEnd : 0ull, haveLat ? (unsigned long long)lat.simToGpuEnd : 0ull,
			haveLat ? lat.gpuFrameUs : 0u, haveLat ? lat.gpuActiveUs : 0u, d.winHudTags, d.winAlphaTags);
		d.winStart = now;
		d.winReal = d.winShown = 0;
		d.winHudTags = d.winAlphaTags = 0;
	}
	std::string w;
	d.frameBegun = d.sl->frameBegin(uint32_t(m_stats.presents), &w);	// Reflex sleep + simulation start
	d.sceneMarked = false;
	if (d.frameBegun && g_pclPingPending.exchange(false))
		d.sl->marker(Upscale::PclMarker::PCLatencyPing);
	if (d.frameBegun && g_flashPending.exchange(false))
		d.sl->marker(Upscale::PclMarker::TriggerFlash);
	if (!d.frameBegun && !d.frameFailLogged) {
		d.frameFailLogged = true;
		logLine("[GXSL] frame begin failed (Reflex/DLSS-G get no frames): %s", w.c_str());
	}
}

bool GxNativeDevice::swapchainInterposed() const
{
	// Requested is enough (StreamlineFeatures::frameGen: loaded only when asked for): the safe side - the old present path - when unsure
	return m_host.testSwapchainInterposed || (m_host.streamline && m_host.streamline->loaded() && m_host.streamline->features().frameGen > 0);
}

PFN_vkGetInstanceProcAddr GxNativeDevice::dlssLoaderRoot() const
{
	return m_host.streamline && m_host.streamline->loaded() ? m_host.streamline->loaderRoot() : m_host.getInstanceProcAddr;
}

void GxNativeDevice::dlssShutdown()
{
	// No m_dlss (dlssInit never ran: VkCtx::init or a step after it failed): slShutdown still precedes the device - the loader
	// root's vkDestroyDevice runs it (Codex dlss-sl-r1 P1, r2 P1).
	if (!m_dlss)
		return;
	if (m_dlss->out.image) {
		forgetImage(&m_dlss->out);
		if (m_dlss->outView)
			vkDestroyImageView(m_ctx.device(), m_dlss->outView, nullptr);	// the device is idle (destructor)
		m_dlss->out.destroy(m_gx);
	}
	// PREREG A39c's HUD-less targets and their resolve pipeline (the device is idle here: destructor)
	if (m_dlss->hudPresent)
		m_dlss->hudPresent->shutdown();
	if (m_dlss->uiAlpha)
		m_dlss->uiAlpha->shutdown();
	if (m_dlss->alpha.image) {
		forgetImage(&m_dlss->alpha);
		if (m_dlss->alphaView)
			vkDestroyImageView(m_ctx.device(), m_dlss->alphaView, nullptr);
		m_dlss->alpha.destroy(m_gx);
	}
	for (std::unique_ptr<Res::GxRenderTarget>* t : { &m_dlss->hudMS, &m_dlss->hud1x, &m_dlss->final1x })
		if (*t) {
			forgetImage(&(*t)->image());
			t->reset();
		}
	logLine("[GXSL] shutdown evaluations=%llu", (unsigned long long)m_dlss->evaluations);
	m_dlss->sl->shutdown();	// slShutdown before the device goes (Streamline's order)
	delete m_dlss;
	m_dlss = nullptr;
}

bool GxNativeDevice::dlssSupported() const
{
	return m_dlss && m_dlss->supported;
}

HRESULT GxNativeDevice::dlssUpscale(const GxUpscaleCall& c)
{
	if (!dlssSupported())
		return E_NOTIMPL;
	// A refusal is for the rest of the session: Supports turns FALSE and the jitter stops, so the seam's bilinear never shows a
	// jittered world (Codex dlss-sl-r1 P2).
	auto refuse = [this](int k, const std::string& why) {
		if (!m_dlss->refused[k])
			logLine("[GXSL] upscale refused, DLSS off for this session (bilinear instead): %s", why.c_str());
		m_dlss->refused[k] = true;
		m_dlss->supported = false;
		setTemporalConsumer(false);
		return E_NOTIMPL;
	};
	const GxMotionFrame* mf = motionFrame();
	if (!mf || !mf->depth.image || !mf->depth.view || !mf->mv.image || !mf->mv.view)
		return refuse(0, "no motion frame (world depth not sampleable or motion off)");
	GxNativeSurface* srcS = static_cast<GxNativeSurface*>(c.src);
	GxNativeSurface* dstS = static_cast<GxNativeSurface*>(c.dst);
	Res::GxImage* src = srcS ? srcS->image() : nullptr;
	Res::GxImage* dst = dstS ? dstS->image() : nullptr;
	const VkImageView srcView = srcS ? srcS->attachmentView() : VK_NULL_HANDLE;
	// The seam's contract (GxWorldPass.h, render/ws-hud-seam 0bab79e69): src = the 1x world texture, srcRect = the camera viewport in
	// it; dst = the seam's own 1x output-size texture and dstRect its whole size (the seam point-copies it into the real target).
	if (!src || !dst || !srcView || src->samples != VK_SAMPLE_COUNT_1_BIT || dst->samples != VK_SAMPLE_COUNT_1_BIT)
		return refuse(1, "source or destination is not a single-sample device image");
	const RECT& sr = c.srcRect;
	if (sr.left < 0 || sr.top < 0 || sr.right <= sr.left || sr.bottom <= sr.top || uint32_t(sr.right) > src->width ||
		uint32_t(sr.bottom) > src->height || uint32_t(sr.right) > mf->depth.width || uint32_t(sr.bottom) > mf->depth.height ||
		uint32_t(sr.right) > mf->mv.width || uint32_t(sr.bottom) > mf->mv.height || c.dstRect.left != 0 || c.dstRect.top != 0 ||
		uint32_t(c.dstRect.right) != dst->width || uint32_t(c.dstRect.bottom) != dst->height)
		return refuse(5, "source/destination rectangles outside the seam's contract");
	if (m_dlss->lastPresent == m_stats.presents)
		return E_NOTIMPL;	// a second world pass in one frame: DLSS history is per frame (bilinear for this one)
	const uint32_t renderW = uint32_t(sr.right - sr.left), renderH = uint32_t(sr.bottom - sr.top);
	const uint32_t outW = dst->width, outH = dst->height;
	const Upscale::DlssMode mode = modeFor(renderW, outW);
	uint32_t optW = 0, optH = 0, minW = 0, minH = 0, maxW = 0, maxH = 0;
	std::string why;
	if (!m_dlss->sl->dlssRenderSize(mode, outW, outH, &optW, &optH, &why, &minW, &minH, &maxW, &maxH))
		return refuse(2, why);
	if (renderW < minW || renderH < minH || renderW > maxW || renderH > maxH)
		return refuse(3, "render " + std::to_string(renderW) + "x" + std::to_string(renderH) + " outside DLSS's " +
			std::to_string(minW) + "x" + std::to_string(minH) + ".." + std::to_string(maxW) + "x" + std::to_string(maxH));

	if (m_dlss->out.image && (m_dlss->out.width != outW || m_dlss->out.height != outH)) {
		forgetImage(&m_dlss->out);
		const VkImageView old = m_dlss->outView;
		const VkDevice dev = m_ctx.device();
		m_ctx.retire(Vk::QueueKind::Graphics, m_dlss->out.lastUse, [dev, old]() { vkDestroyImageView(dev, old, nullptr); });
		m_dlss->out.destroy(m_gx);
		m_dlss->out = Res::GxImage();
		m_dlss->outView = VK_NULL_HANDLE;
	}
	const VkImageUsageFlags outUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!m_dlss->out.image) {
		if (!m_dlss->out.create(m_gx, &m_dlss->outFormat, outW, outH, 1, VK_SAMPLE_COUNT_1_BIT, outUsage, &why))
			return refuse(4, "output image: " + why);
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = m_dlss->out.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (vkCreateImageView(m_ctx.device(), &vi, nullptr, &m_dlss->outView) != VK_SUCCESS) {
			m_dlss->out.destroy(m_gx);
			m_dlss->out = Res::GxImage();
			return refuse(4, "output image view");
		}
	}

	ensureFrame();
	Upscale::DlssFrame f;
	f.renderX = uint32_t(sr.left);
	f.renderY = uint32_t(sr.top);
	f.renderW = renderW;
	f.renderH = renderH;
	f.outputW = outW;
	f.outputH = outH;
	f.jitterPx[0] = mf->jitterPx[0];
	f.jitterPx[1] = mf->jitterPx[1];
	f.mvScale[0] = mf->mvScale[0] / float(renderW);	// render pixels -> [-1,1]
	f.mvScale[1] = mf->mvScale[1] / float(renderH);
	std::memcpy(f.viewToClip, mf->viewToClip, sizeof f.viewToClip);	// D3D row-vector, row-major = sl::float4x4 as is
	std::memcpy(f.clipToView, mf->clipToView, sizeof f.clipToView);
	std::memcpy(f.clipToPrevClip, mf->clipToPrevClip, sizeof f.clipToPrevClip);
	std::memcpy(f.prevClipToClip, mf->prevClipToClip, sizeof f.prevClipToClip);
	std::memcpy(f.cameraPos, mf->cameraPos, sizeof f.cameraPos);
	std::memcpy(f.cameraUp, mf->cameraUp, sizeof f.cameraUp);
	std::memcpy(f.cameraRight, mf->cameraRight, sizeof f.cameraRight);
	std::memcpy(f.cameraFwd, mf->cameraFwd, sizeof f.cameraFwd);
	f.nearZ = mf->nearZ;
	f.farZ = mf->farZ;
	f.fovY = mf->fovYRad;
	f.aspect = mf->aspect;
	f.reset = (mf->flags & GXMF_RESET) != 0 || m_dlss->evaluations == 0;
	f.frameIndex = uint32_t(mf->frameIndex);
	// TEST-ONLY GX_FG_SUBRECT=1 (first light): frame generation on the camera viewport only, the control bar copied as is. The world
	// viewport starts at the back buffer's origin in this game; only when the presented image is the back buffer 1:1 (no pillarbox).
	if (m_dlss->fgOn && m_dlss->fgSubrect && m_haveSwapchain && m_swapchain.extent().width == m_pp.BackBufferWidth &&
		m_swapchain.extent().height == m_pp.BackBufferHeight && outW <= m_pp.BackBufferWidth && outH <= m_pp.BackBufferHeight) {
		f.fgSubrectW = outW;
		f.fgSubrectH = outH;
		if (!m_dlss->fgSubrectLogged) {
			m_dlss->fgSubrectLogged = true;
			logLine("[GXFG] subrect 0,0 %ux%u of the presented %ux%u", outW, outH, m_pp.BackBufferWidth, m_pp.BackBufferHeight);
		}
	}

	// depth and mv are not in this list: the motion resolve of this world pass, recorded earlier in the same writer frame, already
	// marked them used (GxNativeDevice::worldEnd), and the device holds the depth until the next Begin.
	Res::GxImage* touched[3] = { src, &m_dlss->out, dst };
	bool ok = false;
	m_writer.outsideScope([&](VkCommandBuffer cmd) {
		const int gpu = m_writer.gpuBegin("dlss");
		const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			src->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cs, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		m_dlss->out.transition(cmd, VK_IMAGE_LAYOUT_GENERAL, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		f.colorIn = dlssImage(*src, srcView, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		f.depth = motionImage(mf->depth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT);
		f.motion = motionImage(mf->mv, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		f.colorOut = dlssImage(m_dlss->out, m_dlss->outView, outUsage, VK_IMAGE_ASPECT_COLOR_BIT);
		ok = m_dlss->sl->dlssEvaluate(cmd, mode, f, &why);	// Streamline returns every image in the layout it was tagged with
		if (ok) {
			m_dlss->out.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
			dst->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkImageBlit blit = {};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.srcOffsets[1] = { int32_t(outW), int32_t(outH), 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.dstOffsets[0] = { int32_t(c.dstRect.left), int32_t(c.dstRect.top), 0 };
			blit.dstOffsets[1] = { int32_t(c.dstRect.right), int32_t(c.dstRect.bottom), 1 };
			vkCmdBlitImage(cmd, m_dlss->out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
		}
		m_writer.gpuEnd(gpu);
	}, touched, 3);
	if (!ok) {
		logLine("[GXSL] ERROR evaluate failed, DLSS off for this session (bilinear instead): %s", why.c_str());
		m_dlss->supported = false;
		setTemporalConsumer(false);
		return E_FAIL;
	}
	++m_dlss->evaluations;
	++g_dlssEvaluations;
	if (m_dlss->evaluations == 1 || m_dlss->evaluations % 1000 == 0)	// the evidence line (the game exits without a device teardown)
		logLine("[GXSL] evaluations=%llu render=%ux%u+%u+%u out=%ux%u mode=%u jitter=(%.3f,%.3f)",
			(unsigned long long)m_dlss->evaluations, renderW, renderH, f.renderX, f.renderY, outW, outH, unsigned(mode),
			f.jitterPx[0], f.jitterPx[1]);
	m_dlss->lastPresent = m_stats.presents;
	if (dstS->kind() == GxNativeSurface::Kind::TextureLevel)
		dstS->texture()->noteWritten(dstS->level());	// as the seam's FSR 1 does: the texture's own write bookkeeping
	return S_OK;
}

#else	// !GX_STREAMLINE

void GxNativeLoadStreamline(IDirect3D8*, GxNativeHost&, bool wantDlss, int reflex, int frameGen)
{
	if (wantDlss || reflex > 0 || frameGen > 0) {
		std::fprintf(stderr, "[GXSL] not loaded: this build has no Streamline (GX_STREAMLINE=OFF)\n");
		std::fflush(stderr);
	}
}

struct GxNativeDevice::Dlss {};
PFN_vkGetInstanceProcAddr GxNativeDevice::dlssLoaderRoot() const { return m_host.getInstanceProcAddr; }
bool GxNativeDevice::swapchainInterposed() const { return m_host.testSwapchainInterposed; }
void GxNativeDevice::dlssInit() {}
void GxNativeDevice::dlssShutdown() {}
void GxNativeNoteWindowsMessage(unsigned) {}
void GxNativeDevice::fgHudBegin() {}
void GxNativeDevice::fgHudlessTag() {}
void GxNativeDevice::slSceneBegin() {}
void GxNativeDevice::slPresentBegin() {}
bool GxNativeDevice::slPresentHook(Vk::PresentHook*) { return false; }
void GxNativeDevice::slBeforeRecreate() {}
void GxNativeDevice::slAfterRecreate() {}
void GxNativeDevice::slFrameEnd() {}
bool GxNativeDevice::dlssSupported() const { return false; }
HRESULT GxNativeDevice::dlssUpscale(const GxUpscaleCall&) { return E_NOTIMPL; }

#endif

}} // namespace GXRender::Dev
