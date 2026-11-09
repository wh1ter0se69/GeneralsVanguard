// GeneralsX @feature B · dlss-streamline-1 13/10/2026 see GxStreamline.h. Built only with GX_STREAMLINE=ON on Windows x64.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "GxStreamline.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

// Streamline's own headers, from the SDK folder at build time (never vendored, see GxStreamline.h).
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>	// defines its functions: include in this one TU only

namespace GXRender { namespace Upscale {

struct Streamline::Api
{
	std::wstring dir;
	PFun_slInit* init = nullptr;
	PFun_slShutdown* shutdown = nullptr;
	PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
	PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
	PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
	PFun_slSetConstants* setConstants = nullptr;
	PFun_slSetTagForFrame* setTagForFrame = nullptr;
	PFun_slEvaluateFeature* evaluateFeature = nullptr;
	PFun_slGetFeatureVersion* getFeatureVersion = nullptr;
	PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;	// feature functions: resolved on first use
	PFun_slDLSSSetOptions* dlssSetOptions = nullptr;
	// dlss-fg-1: resolved after slInit, each only when its feature loaded
	PFun_slReflexSleep* reflexSleep = nullptr;
	PFun_slReflexSetOptions* reflexSetOptions = nullptr;
	PFun_slReflexGetState* reflexGetState = nullptr;
	PFun_slPCLSetMarker* pclSetMarker = nullptr;
	PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
	PFun_slDLSSGGetState* dlssgGetState = nullptr;
};

namespace
{
std::string narrow(const std::wstring& w)
{
	if (w.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
	return s;
}

std::string fail(const char* what, sl::Result r)
{
	return std::string(what) + " failed: sl::Result " + std::to_string(int(r));
}

// Streamline's log callback carries no user pointer: its lines go to stderr with our prefix, like every [GX...] line.
void logLine(sl::LogType type, const char* msg)
{
	std::fprintf(stderr, "[GXSL] %s %s", type == sl::LogType::eError ? "ERROR" : type == sl::LogType::eWarn ? "WARN" : "info",
		msg ? msg : "");
	const size_t n = msg ? std::strlen(msg) : 0;
	if (n == 0 || msg[n - 1] != '\n')
		std::fputc('\n', stderr);
}

sl::float4x4 rows(const float* m)
{
	sl::float4x4 r;
	for (uint32_t i = 0; i < 4; ++i)
		r.setRow(i, sl::float4(m[i * 4 + 0], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]));
	return r;
}

sl::Resource resource(const DlssImage& i, sl::SubresourceRange* range)
{
	sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(i.image), nullptr, reinterpret_cast<void*>(i.view),
		uint32_t(i.layout));
	r.width = i.width;
	r.height = i.height;
	r.nativeFormat = uint32_t(i.format);
	r.mipLevels = 1;
	r.arrayLayers = 1;
	r.flags = 0;
	r.usage = uint32_t(i.usage);
	range->aspectMask = uint32_t(i.aspect);
	range->baseMipLevel = 0;
	range->levelCount = 1;
	range->baseArrayLayer = 0;
	range->layerCount = 1;
	r.next = range;	// read by sl.dlss (findStruct<SubresourceRange>) for the NGX view description
	return r;
}

template <typename T> bool symbol(HMODULE m, const char* name, T*& out)
{
	out = reinterpret_cast<T*>(GetProcAddress(m, name));
	return out != nullptr;
}

// loaderRoot(): slShutdown before the device and the instance go, on every path (Codex dlss-sl-r2 P1). VkCtx::init destroys
// both itself when a step after vkCreateDevice fails (timeline semaphore, VMA) - code this module does not own - and VkCtx
// resolves every function through the root it is given (volk: vkDestroyInstance from vkGetInstanceProcAddr, vkDestroyDevice
// again from vkGetDeviceProcAddr). So the root hands out the interposer's functions with those destroys wrapped.
// ponytail: one Streamline device at a time (globals); per-device state if two ever coexist.
Streamline* g_owner = nullptr;	// the loaded Streamline; null once it shut down
PFN_vkGetInstanceProcAddr g_gipa = nullptr;	// the interposer's; kept after shutdown (the module stays mapped)
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;
PFN_vkDestroyDevice g_destroyDevice = nullptr;
PFN_vkDestroyInstance g_destroyInstance = nullptr;

void shutdownBefore(const char* what)
{
	if (!g_owner)
		return;	// the usual case: the device teardown already ran slShutdown
	std::fprintf(stderr, "[GXSL] slShutdown before %s (loader root)\n", what);
	g_owner->shutdown();
}

VKAPI_ATTR void VKAPI_CALL rootDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
	shutdownBefore("vkDestroyDevice");
	g_destroyDevice(device, allocator);
}

VKAPI_ATTR void VKAPI_CALL rootDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
	shutdownBefore("vkDestroyInstance");
	g_destroyInstance(instance, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rootGetDeviceProcAddr(VkDevice device, const char* name)
{
	const PFN_vkVoidFunction f = g_gdpa(device, name);
	if (f && std::strcmp(name, "vkDestroyDevice") == 0) {
		g_destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&rootDestroyDevice);
	}
	return f;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rootGetInstanceProcAddr(VkInstance instance, const char* name)
{
	const PFN_vkVoidFunction f = g_gipa(instance, name);
	if (!f)
		return f;
	if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&rootGetInstanceProcAddr);
	if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
		g_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&rootGetDeviceProcAddr);
	}
	if (std::strcmp(name, "vkDestroyDevice") == 0) {
		g_destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&rootDestroyDevice);
	}
	if (std::strcmp(name, "vkDestroyInstance") == 0) {
		g_destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(f);
		return reinterpret_cast<PFN_vkVoidFunction>(&rootDestroyInstance);
	}
	return f;
}
} // namespace

bool Streamline::load(const wchar_t* dir, const StreamlineFeatures& want, std::string* why)
{
	shutdown();
	if (dir == nullptr || *dir == 0) {
		*why = "no Streamline folder given";
		return false;
	}
	// Absolute path: the signature checks refuse relative ones, and LoadLibraryEx must not search.
	wchar_t full[MAX_PATH] = {};
	const DWORD fullLen = GetFullPathNameW(dir, MAX_PATH, full, nullptr);
	if (fullLen == 0 || fullLen >= MAX_PATH) {	// >= MAX_PATH: the required size, `full` was not written (Codex dlss-sl-r1)
		*why = "bad Streamline folder";
		return false;
	}
	const std::wstring interposer = std::wstring(full) + L"\\sl.interposer.dll";
	if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
		*why = "no sl.interposer.dll in " + narrow(full);
		return false;
	}
	if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
		*why = narrow(interposer) + " is not signed by NVIDIA - refused";
		return false;
	}
	// slInit loads the plugins, and NGX later nvngx_dlss.dll, with plain LoadLibraryW: their dependencies would search the current
	// directory and PATH. From here on the process searches the exe's folder, System32 and this folder only (Codex dlss-sl-r2 P2).
	// Process-wide and for good (Windows has no undo) - only a session that loads Streamline (DLSS asked, NVIDIA adapter) gets it.
	// So ONE Streamline folder per process: a later, different folder would load against the first one's entry (Codex dlss-sl-r3 P2).
	static std::wstring s_searchDir;
	if (s_searchDir.empty()) {
		if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) ||
			!AddDllDirectory(full)) {
			*why = "cannot restrict the DLL search path: " + std::to_string(GetLastError());
			return false;
		}
		s_searchDir = full;
	} else if (_wcsicmp(s_searchDir.c_str(), full) != 0) {
		*why = "Streamline folder " + narrow(full) + " differs from this process's first one (" + narrow(s_searchDir) + "): restart to switch";
		return false;
	}
	// Its dependencies resolve from its own folder and System32 only - never the current directory or PATH (Codex dlss-sl-r1).
	HMODULE mod = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (mod == nullptr) {
		*why = "LoadLibrary(" + narrow(interposer) + ") failed: " + std::to_string(GetLastError());
		return false;
	}
	Api* api = new Api();
	api->dir = full;
	PFN_vkGetInstanceProcAddr gipa = nullptr;
	const bool all = symbol(mod, "slInit", api->init) && symbol(mod, "slShutdown", api->shutdown) &&
		symbol(mod, "slIsFeatureSupported", api->isFeatureSupported) && symbol(mod, "slGetFeatureFunction", api->getFeatureFunction) &&
		symbol(mod, "slGetNewFrameToken", api->getNewFrameToken) && symbol(mod, "slSetConstants", api->setConstants) &&
		symbol(mod, "slSetTagForFrame", api->setTagForFrame) && symbol(mod, "slEvaluateFeature", api->evaluateFeature) &&
		symbol(mod, "slGetFeatureVersion", api->getFeatureVersion) &&
		(gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"))) != nullptr;
	if (!all) {
		*why = "sl.interposer.dll lacks an entry point this build needs (SDK/DLL version mismatch?)";
		delete api;
		FreeLibrary(mod);
		return false;
	}

	const wchar_t* paths[] = { api->dir.c_str() };
	// dlss-fg-1: only the features this session asked for (PCL, like sl.common, loads with every Streamline).
	sl::Feature features[3] = {};
	uint32_t featureCount = 0;
	if (want.dlss)
		features[featureCount++] = sl::kFeatureDLSS;
	if (want.reflexMode || want.frameGen)
		features[featureCount++] = sl::kFeatureReflex;	// DLSS-G refuses to run without Reflex (eFailReflexNotDetectedAtRuntime)
	if (want.frameGen)
		features[featureCount++] = sl::kFeatureDLSS_G;
	sl::Preferences pref{};
	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathsToPlugins = paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = nullptr;
	pref.logMessageCallback = &logLine;
	// No OTA: Streamline would otherwise fetch and load plugins from NVIDIA's servers at run time.
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
		sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = featureCount;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "GeneralsX";
	pref.projectId = "d88d0b94-7339-4bbc-8ae2-7f3cd3ac1e87";	// GeneralsX's own id (no NVIDIA application id)
	pref.renderAPI = sl::RenderAPI::eVulkan;
	const sl::Result r = api->init(pref, sl::kSDKVersion);
	if (r != sl::Result::eOk) {
		*why = fail("slInit", r);
		delete api;
		FreeLibrary(mod);
		return false;
	}
	m_module = mod;
	m_api = api;
	m_gipa = gipa;
	g_owner = this;
	g_gipa = gipa;
	m_features = want;
	if (m_features.frameGen && !m_features.reflexMode)
		m_features.reflexMode = 1;
	return true;	// the feature entry points come after the device exists: resolveFeatures()
}

// Codex fg-p0-r1 P1: slGetFeatureFunction answers eErrorNotInitialized until the plugins initialised against a device, so this runs
// after VkCtx::init (GxNativeDevice::dlssInit). A missing plugin DLL (sl.reflex / sl.dlss_g / nvngx_dlssg: optional, the user's own
// copy) leaves its pointers null and the feature off - logged, never fatal.
void Streamline::resolveFeatures()
{
	if (!m_api)
		return;
	Api* api = m_api;
	auto feature = [api](sl::Feature f, const char* name, auto*& out) {
		void* fn = nullptr;
		const sl::Result r = api->getFeatureFunction(f, name, fn);
		if (r == sl::Result::eOk && fn)
			out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(fn);
		else
			std::fprintf(stderr, "[GXSL] %s unavailable: sl::Result %d (its Streamline plugin did not load or initialise)\n", name, int(r));
	};
	if (m_features.reflexMode) {
		feature(sl::kFeatureReflex, "slReflexSleep", api->reflexSleep);
		feature(sl::kFeatureReflex, "slReflexSetOptions", api->reflexSetOptions);
		feature(sl::kFeatureReflex, "slReflexGetState", api->reflexGetState);
		feature(sl::kFeaturePCL, "slPCLSetMarker", api->pclSetMarker);
	}
	if (m_features.frameGen) {
		feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api->dlssgSetOptions);
		feature(sl::kFeatureDLSS_G, "slDLSSGGetState", api->dlssgGetState);
	}
	std::fflush(stderr);
}

PFN_vkGetInstanceProcAddr Streamline::loaderRoot() const
{
	return m_gipa ? &rootGetInstanceProcAddr : nullptr;
}

bool Streamline::dlssSupported(VkPhysicalDevice physical, std::string* why)
{
	if (!m_api) {
		*why = "Streamline not loaded";
		return false;
	}
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = physical;
	const sl::Result r = m_api->isFeatureSupported(sl::kFeatureDLSS, adapter);
	if (r != sl::Result::eOk) {
		*why = fail("slIsFeatureSupported(DLSS)", r);
		return false;
	}
	return true;
}

bool Streamline::dlssRenderSize(DlssMode mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH,
	std::string* why, uint32_t* minW, uint32_t* minH, uint32_t* maxW, uint32_t* maxH)
{
	if (!m_api) {
		*why = "Streamline not loaded";
		return false;
	}
	if (!m_api->dlssGetOptimalSettings) {
		void* fn = nullptr;
		const sl::Result r = m_api->getFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn);
		if (r != sl::Result::eOk) {
			*why = fail("slGetFeatureFunction(slDLSSGetOptimalSettings)", r);
			return false;
		}
		m_api->dlssGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
	}
	sl::DLSSOptions options{};
	options.mode = sl::DLSSMode(uint32_t(mode));
	options.outputWidth = outW;
	options.outputHeight = outH;
	sl::DLSSOptimalSettings settings{};
	const sl::Result r = m_api->dlssGetOptimalSettings(options, settings);
	if (r != sl::Result::eOk) {
		*why = fail("slDLSSGetOptimalSettings", r);
		return false;
	}
	*renderW = settings.optimalRenderWidth;
	*renderH = settings.optimalRenderHeight;
	if (minW) *minW = settings.renderWidthMin;
	if (minH) *minH = settings.renderHeightMin;
	if (maxW) *maxW = settings.renderWidthMax;
	if (maxH) *maxH = settings.renderHeightMax;
	return true;
}

bool Streamline::dlssEvaluate(VkCommandBuffer cmd, DlssMode mode, const DlssFrame& f, std::string* why)
{
	if (!m_api) {
		*why = "Streamline not loaded";
		return false;
	}
	const sl::ViewportHandle viewport(0u);
	// dlss-fg-1: the frame begun by frameBegin (one token per rendered frame, shared with Reflex and DLSS-G); a session that never
	// begins frames (DLSS SR alone, as before) still gets its own token per evaluate.
	sl::FrameToken* token = static_cast<sl::FrameToken*>(m_frameToken);
	sl::Result r = sl::Result::eOk;
	if (!token) {
		const uint32_t index = f.frameIndex;
		r = m_api->getNewFrameToken(token, &index);
		if (r != sl::Result::eOk || token == nullptr) {
			*why = fail("slGetNewFrameToken", r);
			return false;
		}
	}

	// TEST-ONLY GX_DLSS_JITTER_FOLD=<x>,<y> (PREREG A36.2k): a constant added to the jitter DLSS is told, in render pixels - does it
	// take out the scaled world's D3D9 half-pixel offset (hud-seam-2: 0.5 * (1/r - 1) output px right + down)? Unset or malformed = 0,0.
	struct JitterFold { float x = 0.f, y = 0.f; };
	static const JitterFold fold = [] {
		JitterFold j;
		if (const char* v = std::getenv("GX_DLSS_JITTER_FOLD"))
			if (int n = 0; std::sscanf(v, "%f,%f %n", &j.x, &j.y, &n) != 2 || v[n] || !std::isfinite(j.x) || !std::isfinite(j.y))
				j = JitterFold();
		return j;
	}();

	if (mode != m_lastMode || f.outputW != m_lastOutW || f.outputH != m_lastOutH) {
		if (!m_api->dlssSetOptions) {
			void* fn = nullptr;
			r = m_api->getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn);
			if (r != sl::Result::eOk) {
				*why = fail("slGetFeatureFunction(slDLSSSetOptions)", r);
				return false;
			}
			m_api->dlssSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
		}
		sl::DLSSOptions options{};
		options.mode = sl::DLSSMode(uint32_t(mode));
		options.outputWidth = f.outputW;
		options.outputHeight = f.outputH;
		options.colorBuffersHDR = sl::Boolean::eFalse;	// the game's world colour is LDR (RGBA8)
		options.useAutoExposure = sl::Boolean::eTrue;	// no exposure texture
		// The model: E at EVERY mode, never eDefault (wh1ter0se's ruling 16/10/2026, modernize/render-dlss/RESULT_wh1ter0se-fg-live.md: at DLAA on his
		// PC E cost 0.42 ms of GPU per frame against L's 2.32, and kept 95% / 86% of native's fine detail at f4800 / f3500; it was L since
		// dlss-integrator's ruling 13/10). Streamline 2.14.1's sl_dlss.h marks ePresetE "Deprecated": it runs on today's DLSS DLLs (measured
		// 16/10), a later DLL may drop it. eDefault = K at DLAA/Balanced/Quality. TEST-ONLY GX_DLSS_PRESET=<E..O> overrides it for every
		// mode (picture A/Bs).
		static const char* const presetEnv = std::getenv("GX_DLSS_PRESET");
		const bool forced = presetEnv && presetEnv[0] >= 'E' && presetEnv[0] <= 'O' && presetEnv[1] == 0;
		options.dlaaPreset = options.qualityPreset = options.balancedPreset = options.performancePreset =
			options.ultraPerformancePreset = options.ultraQualityPreset =
			forced ? sl::DLSSPreset(uint32_t(presetEnv[0] - 'A' + 1)) : sl::DLSSPreset::ePresetE;
		const char* jitterEnv = std::getenv("GX_DLSS_JITTER");
		std::fprintf(stderr, "[GXSL] options mode=%u out=%ux%u preset=%s jitter=%s fold=%g,%g\n", unsigned(mode), f.outputW,
			f.outputH, forced ? presetEnv : "E", jitterEnv && *jitterEnv ? jitterEnv : "as-is", double(fold.x), double(fold.y));
		r = m_api->dlssSetOptions(viewport, options);
		if (r != sl::Result::eOk) {
			*why = fail("slDLSSSetOptions", r);
			return false;
		}
		m_lastMode = mode;
		m_lastOutW = f.outputW;
		m_lastOutH = f.outputH;
	}

	sl::Constants c{};
	c.cameraViewToClip = rows(f.viewToClip);
	c.clipToCameraView = rows(f.clipToView);
	static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	c.clipToLensClip = rows(kIdentity);
	c.clipToPrevClip = rows(f.clipToPrevClip);
	c.prevClipToClip = rows(f.prevClipToClip);
	// TEST-ONLY GX_DLSS_JITTER=neg|zero|negx|negy (PREREG A36.2i plant): the jitter DLSS is told - negated (both axes or one), or zeroed -
	// while the projection keeps the real one: a mismatch on purpose, to see how far the picture moves. Unset = the frame's jitter as is.
	struct JitterPlant { float x = 1.f, y = 1.f; };
	static const JitterPlant plant = [] {
		JitterPlant p;
		const char* v = std::getenv("GX_DLSS_JITTER");
		if (!v) return p;
		if (!std::strcmp(v, "neg") || !std::strcmp(v, "negx")) p.x = -1.f;
		if (!std::strcmp(v, "neg") || !std::strcmp(v, "negy")) p.y = -1.f;
		if (!std::strcmp(v, "zero")) p.x = p.y = 0.f;
		return p;
	}();
	c.jitterOffset = sl::float2(f.jitterPx[0] * plant.x + fold.x, f.jitterPx[1] * plant.y + fold.y);
	c.mvecScale = sl::float2(f.mvScale[0], f.mvScale[1]);
	c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
	c.cameraPos = sl::float3(f.cameraPos[0], f.cameraPos[1], f.cameraPos[2]);
	c.cameraUp = sl::float3(f.cameraUp[0], f.cameraUp[1], f.cameraUp[2]);
	c.cameraRight = sl::float3(f.cameraRight[0], f.cameraRight[1], f.cameraRight[2]);
	c.cameraFwd = sl::float3(f.cameraFwd[0], f.cameraFwd[1], f.cameraFwd[2]);
	c.cameraNear = f.nearZ;
	c.cameraFar = f.farZ;
	c.cameraFOV = f.fovY;
	c.cameraAspectRatio = f.aspect;
	c.depthInverted = sl::Boolean::eFalse;
	c.cameraMotionIncluded = sl::Boolean::eTrue;
	c.motionVectors3D = sl::Boolean::eFalse;
	c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = sl::Boolean::eFalse;
	c.motionVectorsDilated = sl::Boolean::eFalse;
	c.motionVectorsJittered = sl::Boolean::eFalse;
	// Once per frame (DLSS_G guide §17: never twice for one frame): a second evaluate in a begun frame keeps the first constants.
	if (!m_frameToken || m_constantsFrame != m_frameId) {
		r = m_api->setConstants(c, *token, viewport);
		if (r != sl::Result::eOk) {
			*why = fail("slSetConstants", r);
			return false;
		}
		m_constantsFrame = m_frameId;
	}

	sl::SubresourceRange ranges[4];
	sl::Resource colorIn = resource(f.colorIn, &ranges[0]), depth = resource(f.depth, &ranges[1]),
		motion = resource(f.motion, &ranges[2]), colorOut = resource(f.colorOut, &ranges[3]);
	const sl::Extent renderExtent{ f.renderY, f.renderX, f.renderW, f.renderH };	// top, left, width, height
	const sl::Extent outputExtent{ 0, 0, f.outputW, f.outputH };
	// eValidUntilEvaluate (DLSS's own required lifecycle, dlssEntry.cpp requiredTags): the evaluate follows in this command
	// buffer, so Streamline uses the images as given. eOnlyValidNow made it CLONE them - an extra copy each, and the depth clone
	// lost the DEPTH subresource range (Codex dlss-sl-r1 P1).
	const sl::ResourceLifecycle life = sl::ResourceLifecycle::eValidUntilEvaluate;
	// dlss-fg-1: with frame generation the depth and motion vectors are read again at the PRESENT (DLSS_G guide §5.2: tag them
	// eValidUntilPresent first). They are motion's own images, rewritten only by the next frame's world pass - which DLSS-G's blocking
	// wait on our queue orders after its read (DESIGN_dlss-fg-P0.md §3).
	const sl::ResourceLifecycle fgLife = m_api->dlssgSetOptions ? sl::ResourceLifecycle::eValidUntilPresent : life;
	// The part of the presented image frame generation runs on (the camera viewport): the control bar outside it is copied as is.
	const sl::Extent fgSubrect{ f.fgSubrectY, f.fgSubrectX, f.fgSubrectW, f.fgSubrectH };
	const sl::ResourceTag tags[] = {
		sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, life, &renderExtent),
		sl::ResourceTag(&depth, sl::kBufferTypeDepth, fgLife, &renderExtent),
		sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, fgLife, &renderExtent),
		sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, life, &outputExtent),
		sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &fgSubrect),
	};
	const uint32_t tagCount = (m_api->dlssgSetOptions && f.fgSubrectW && f.fgSubrectH) ? 5u : 4u;
	r = m_api->setTagForFrame(*token, viewport, tags, tagCount, reinterpret_cast<sl::CommandBuffer*>(cmd));
	if (r != sl::Result::eOk) {
		*why = fail("slSetTagForFrame", r);
		return false;
	}
	const sl::BaseStructure* inputs[] = { &viewport };
	r = m_api->evaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, reinterpret_cast<sl::CommandBuffer*>(cmd));
	if (r != sl::Result::eOk) {
		*why = fail("slEvaluateFeature(DLSS)", r);
		return false;
	}
	return true;
}

bool Streamline::frameBegin(uint32_t id, std::string* why)
{
	if (!m_api) {
		*why = "Streamline not loaded";
		return false;
	}
	sl::FrameToken* token = nullptr;
	const sl::Result r = m_api->getNewFrameToken(token, &id);
	if (r != sl::Result::eOk || token == nullptr) {
		*why = fail("slGetNewFrameToken", r);
		m_frameToken = nullptr;
		return false;
	}
	m_frameToken = token;
	m_frameId = id;
	// Reflex: sleep, then the simulation starts (Reflex guide 5.0; called whether Reflex low latency is on or off).
	if (m_api->reflexSleep)
		m_api->reflexSleep(*token);
	marker(PclMarker::SimulationStart);
	return true;
}

void Streamline::marker(PclMarker m)
{
	markerOn(m, m_frameToken);
}

void Streamline::markerOn(PclMarker m, void* frame)
{
	if (m_api && m_api->pclSetMarker && frame)
		m_api->pclSetMarker(sl::PCLMarker(uint32_t(m)), *static_cast<sl::FrameToken*>(frame));
}

bool Streamline::reflexSetMode(uint32_t mode, std::string* why)
{
	if (!m_api || !m_api->reflexSetOptions) {
		*why = "Reflex not loaded";
		return false;
	}
	sl::ReflexOptions o{};
	o.mode = mode >= 2 ? sl::ReflexMode::eLowLatencyWithBoost : mode == 1 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
	o.frameLimitUs = 0;	// the game's FramePacer is the cap (DESIGN_dlss-fg-P0.md §4)
	const sl::Result r = m_api->reflexSetOptions(o);
	if (r != sl::Result::eOk) {
		*why = fail("slReflexSetOptions", r);
		return false;
	}
	return true;
}

bool Streamline::reflexLatest(ReflexLatency* out)
{
	if (!m_api || !m_api->reflexGetState)
		return false;
	sl::ReflexState st{};
	if (m_api->reflexGetState(st) != sl::Result::eOk || !st.latencyReportAvailable)
		return false;
	// the newest report with every stamp present (the array runs oldest -> newest)
	for (int i = sl::kReflexFrameReportCount - 1; i >= 0; --i) {
		const sl::ReflexReport& rep = st.frameReport[i];
		if (!rep.frameID || !rep.simStartTime || rep.gpuRenderEndTime < rep.simStartTime || rep.presentEndTime < rep.simStartTime)
			continue;
		out->frameId = rep.frameID;
		out->simToRenderSubmitEnd = rep.renderSubmitEndTime >= rep.simStartTime ? rep.renderSubmitEndTime - rep.simStartTime : 0;
		out->simToPresentEnd = rep.presentEndTime - rep.simStartTime;
		out->simToGpuEnd = rep.gpuRenderEndTime - rep.simStartTime;
		out->gpuFrameUs = rep.gpuFrameTimeUs;
		out->gpuActiveUs = rep.gpuActiveRenderTimeUs;
		return true;
	}
	return false;
}

uint32_t Streamline::statsWindowMessage()
{
	if (!m_api || !m_api->reflexGetState)
		return 0;
	sl::ReflexState st{};
	return m_api->reflexGetState(st) == sl::Result::eOk ? st.statsWindowMessage : 0u;
}

bool Streamline::frameGenSupported(VkPhysicalDevice physical, std::string* why)
{
	if (!m_api || !m_api->dlssgSetOptions || !m_api->dlssgGetState) {
		*why = "sl.dlss_g not loaded";
		return false;
	}
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = physical;
	const sl::Result r = m_api->isFeatureSupported(sl::kFeatureDLSS_G, adapter);
	if (r != sl::Result::eOk) {
		*why = fail("slIsFeatureSupported(DLSS_G)", r);	// e.g. hardware-accelerated GPU scheduling off, OS too old
		return false;
	}
	return true;
}

bool Streamline::frameGenSetOptions(bool on, uint32_t generated, std::string* why)
{
	if (!m_api || !m_api->dlssgSetOptions) {
		*why = "sl.dlss_g not loaded";
		return false;
	}
	sl::DLSSGOptions o{};
	o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	o.numFramesToGenerate = generated ? generated : 1;
	o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
	o.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;	// DESIGN_dlss-fg-P0.md §3
	const sl::Result r = m_api->dlssgSetOptions(sl::ViewportHandle(0u), o);
	if (r != sl::Result::eOk) {
		*why = fail("slDLSSGSetOptions", r);
		return false;
	}
	return true;
}

bool Streamline::frameGenState(FrameGenState* out, std::string* why)
{
	if (!m_api || !m_api->dlssgGetState) {
		*why = "sl.dlss_g not loaded";
		return false;
	}
	sl::DLSSGState st{};
	const sl::Result r = m_api->dlssgGetState(sl::ViewportHandle(0u), st, nullptr);
	if (r != sl::Result::eOk) {
		*why = fail("slDLSSGGetState", r);
		return false;
	}
	out->status = uint32_t(st.status);
	out->presented = st.numFramesActuallyPresented;
	out->maxGenerated = st.numFramesToGenerateMax;
	out->minWidthOrHeight = st.minWidthOrHeight;
	return true;
}

bool Streamline::tagUiAlpha(VkCommandBuffer cmd, const DlssImage& img, std::string* why)
{
	if (!m_api || !m_frameToken) {
		*why = "no Streamline frame";
		return false;
	}
	sl::SubresourceRange range;
	sl::Resource r = resource(img, &range);
	const sl::Extent ext{ 0, 0, img.width, img.height };
	const sl::ResourceTag tag(&r, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &ext);
	const sl::Result res = m_api->setTagForFrame(*static_cast<sl::FrameToken*>(m_frameToken), sl::ViewportHandle(0u), &tag, 1,
		reinterpret_cast<sl::CommandBuffer*>(cmd));
	if (res != sl::Result::eOk) {
		*why = fail("slSetTagForFrame(UIAlpha)", res);
		return false;
	}
	return true;
}

bool Streamline::tagHudless(VkCommandBuffer cmd, const DlssImage& img, std::string* why)
{
	if (!m_api || !m_frameToken) {
		*why = "no Streamline frame";
		return false;
	}
	sl::SubresourceRange range;
	sl::Resource r = resource(img, &range);
	const sl::Extent ext{ 0, 0, img.width, img.height };
	const sl::ResourceTag tag(&r, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &ext);
	const sl::Result res = m_api->setTagForFrame(*static_cast<sl::FrameToken*>(m_frameToken), sl::ViewportHandle(0u), &tag, 1,
		reinterpret_cast<sl::CommandBuffer*>(cmd));
	if (res != sl::Result::eOk) {
		*why = fail("slSetTagForFrame(HUDLessColor)", res);
		return false;
	}
	return true;
}

std::string Streamline::versions()
{
	if (!m_api)
		return std::string();
	sl::FeatureVersion v{};
	if (m_api->getFeatureVersion(sl::kFeatureDLSS, v) != sl::Result::eOk)
		return "sl=? ngx=?";
	return "sl=" + v.versionSL.toStr() + " ngx=" + v.versionNGX.toStr();
}

void Streamline::shutdown()
{
	if (g_owner == this)
		g_owner = nullptr;
	if (!m_api)
		return;
	m_api->shutdown();
	delete m_api;
	m_api = nullptr;
	m_gipa = nullptr;
	// ponytail: the module stays mapped for the process lifetime. VkCtx (shut down after us) still calls the interposer's
	// proxies (vkDeviceWaitIdle, vkDestroySwapchainKHR) through volk; unmapping first would crash. One DLL per process.
	m_module = nullptr;
	m_lastMode = DlssMode::Off;
	m_lastOutW = m_lastOutH = 0;
	m_frameToken = nullptr;
	m_constantsFrame = ~0u;
}

}} // namespace GXRender::Upscale
