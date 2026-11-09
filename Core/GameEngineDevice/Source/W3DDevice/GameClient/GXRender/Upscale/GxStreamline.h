// GeneralsX @feature B · dlss-streamline-1 13/10/2026 NVIDIA Streamline (DLSS Super Resolution) for the native renderer.
// Windows x64 only, behind GX_STREAMLINE (default OFF); PREREG A36 (render-seam/rw3-T/PREREG_T.md).
//
// Licence (render-dlss research R8, binding): nothing of NVIDIA's is in this tree or in our zip. The Streamline headers (MIT) are
// read at BUILD time from the SDK folder named by GX_STREAMLINE_SDK. At RUN time the user's own copy of NVIDIA's signed DLLs
// (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll from NVIDIA's Streamline release) is loaded from ONE folder,
// after the Windows + NVIDIA signature checks on sl.interposer.dll (sl_security.h); the interposer checks its plugins itself.
// No folder or no DLLs = DLSS absent, everything else runs as before.
//
// How it sits under VkCtx: Streamline's interposer becomes the Vulkan loader root (VkCtxDesc::getInstanceProcAddr). Its
// vkGetInstanceProcAddr / vkGetDeviceProcAddr hand out vulkan-1.dll's own functions except the few Streamline intercepts
// (sl_hooks.h): vkCreateInstance / vkCreateDevice add Streamline's extensions, features and queues and tell it the device;
// the swapchain + vkQueuePresentKHR proxies run its per-frame bookkeeping (presentCommon). So VkCtx, the swapchain and the
// present path are not edited: with Streamline loaded the same calls resolve to the proxies.
//
// Render-only: nothing here is read by the simulation.
#pragma once

#include "VkCommon.h"

#include <cstdint>
#include <string>

namespace GXRender { namespace Upscale {

// Same order as sl::DLSSMode.
enum class DlssMode : uint32_t { Off, MaxPerformance, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA };

struct DlssImage
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;	// colour: the whole image; depth: DEPTH aspect only
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;	// the layout the image is in when the recorded evaluate runs
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0, height = 0;	// the whole image
	VkImageUsageFlags usage = 0;	// as created (Streamline cannot query it)
	// The view's aspect. DEPTH for a depth-stencil image: tagged with an explicit sl::SubresourceRange, else the DLSS plugin
	// assumes COLOR (dlssEntry.cpp cacheState) and NGX barriers a D24S8 image with the wrong aspect (Codex motion-design-r1 #18).
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

// GeneralsX @feature B · dlss-fg-1 14/10/2026 (PREREG A39, modernize/render-dlss/DESIGN_dlss-fg-P0.md H1): which Streamline features
// this session loads. PCL (the latency markers) loads with every Streamline. NVIDIA's frame-generation DLLs (sl.dlss_g.dll,
// nvngx_dlssg.dll) stay OPTIONAL like every other: absent = that feature unavailable, logged, the rest unchanged (wh1ter0se 14/10 via
// the lead: the game builds, runs and ships without them; no link-time dependency).
struct StreamlineFeatures
{
	bool dlss = false;	// sl.dlss: Super Resolution / DLAA (GXUpscaler=dlss)
	uint32_t reflexMode = 0;	// sl.reflex: 0 = not loaded, 1 = low latency, 2 = low latency + boost (GXReflex; >= 1 with frame generation)
	bool reflexLowLatencyOff = false;	// TEST-ONLY GX_REFLEX_MEASURE_ONLY: Reflex loaded (markers, sleep), low latency mode Off
	uint32_t frameGen = 0;	// sl.dlss_g: generated frames per real frame (GXFrameGen 1-4), 0 = not loaded. Loaded, every swapchain
	                    	// becomes a Streamline proxy (DLSS_G guide §18): load it only when frame generation is asked for
};

// Same values as sl::PCLMarker.
enum class PclMarker : uint32_t { SimulationStart = 0, SimulationEnd = 1, RenderSubmitStart = 2, RenderSubmitEnd = 3, PresentStart = 4,
	PresentEnd = 5, TriggerFlash = 7, PCLatencyPing = 8 };

// slDLSSGGetState, per call (call it once per presented frame, on the presenting thread).
struct FrameGenState
{
	uint32_t status = 0;	// sl::DLSSGStatus bits, 0 = OK
	uint32_t presented = 0;	// frames presented since the previous call (real + generated)
	uint32_t maxGenerated = 0;	// numFramesToGenerateMax on this system
	uint32_t minWidthOrHeight = 0;
};

// The newest complete Reflex latency report (slReflexGetState), in the Reflex SDK's own time unit (microseconds) - deltas only.
struct ReflexLatency
{
	uint64_t frameId = 0;
	uint64_t simToRenderSubmitEnd = 0, simToPresentEnd = 0, simToGpuEnd = 0;
	uint32_t gpuFrameUs = 0, gpuActiveUs = 0;
};

// One frame's inputs, in render/ws-motion's GxMotionFrame conventions: motion = current -> previous in render pixels, +y down,
// camera motion included, unjittered, not dilated; jitter in render pixels, +x right / +y down; depth 0 near / 1 far.
// Matrices row-major as sl::Constants wants them, without jitter.
struct DlssFrame
{
	DlssImage colorIn, depth, motion, colorOut;
	uint32_t renderX = 0, renderY = 0;	// used part of colorIn / depth / motion: the camera viewport (the seam's srcRect)
	uint32_t renderW = 0, renderH = 0;
	uint32_t outputW = 0, outputH = 0;	// used part of colorOut
	float jitterPx[2] = {};
	float mvScale[2] = {};	// multiplies motion into [-1,1]: {1/renderW, 1/renderH} for render pixels
	float viewToClip[16] = {}, clipToView[16] = {}, clipToPrevClip[16] = {}, prevClipToClip[16] = {};
	float cameraPos[3] = {}, cameraUp[3] = {}, cameraRight[3] = {}, cameraFwd[3] = {};
	float nearZ = 1.0f, farZ = 1000.0f, fovY = 1.0f, aspect = 1.0f;
	bool reset = false;	// history invalid (first frame, size change, camera cut)
	uint32_t frameIndex = 0;	// +1 per evaluated frame (used only when no frame was begun: see Streamline::frameBegin)
	// dlss-fg-1: the part of the PRESENTED image frame generation runs on (the world's camera viewport in back-buffer pixels); zero
	// width = the whole image. Outside it DLSS-G copies the real frame (the control bar is never interpolated).
	uint32_t fgSubrectX = 0, fgSubrectY = 0, fgSubrectW = 0, fgSubrectH = 0;
};

class Streamline
{
public:
	Streamline() = default;
	~Streamline() { shutdown(); }
	Streamline(const Streamline&) = delete;
	Streamline& operator=(const Streamline&) = delete;

	// dir = absolute folder holding the Streamline DLLs. False + why when the folder or a DLL is missing, a signature fails, or
	// slInit refuses; nothing stays loaded then. Call before VkCtx::init (Streamline must see vkCreateInstance/vkCreateDevice).
	bool load(const wchar_t* dir, const StreamlineFeatures& want, std::string* why);
	const StreamlineFeatures& features() const { return m_features; }
	// After VkCtx::init on the loader root (the plugins initialise against the device): the Reflex / PCL / DLSS-G entry points.
	void resolveFeatures();
	bool loaded() const { return m_module != nullptr; }
	void* module() const { return m_module; }	// the interposer's HMODULE (session row S8 accepts exactly this one)
	// The interposer's own vkGetInstanceProcAddr (S8 attributes it to module()); null until load() succeeded.
	PFN_vkGetInstanceProcAddr getInstanceProcAddr() const { return m_gipa; }
	// The loader root for VkCtxDesc::getInstanceProcAddr: the interposer's, except that vkDestroyDevice and vkDestroyInstance run
	// shutdown() first - so slShutdown precedes the device on every path, VkCtx::init's own failure cleanup included (Codex
	// dlss-sl-r2 P1). Null until load() succeeded.
	PFN_vkGetInstanceProcAddr loaderRoot() const;

	// After VkCtx::init on that loader root: can DLSS run on this physical device? why = Streamline's answer when not.
	bool dlssSupported(VkPhysicalDevice physical, std::string* why);
	// The render size DLSS wants for mode at this output size, and the range it accepts.
	bool dlssRenderSize(DlssMode mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH, std::string* why,
		uint32_t* minW = nullptr, uint32_t* minH = nullptr, uint32_t* maxW = nullptr, uint32_t* maxH = nullptr);
	// Records DLSS into cmd (outside any render pass). Streamline leaves the bound pipeline/descriptor state undefined
	// (manual hooking): the caller re-binds before its next draw or dispatch.
	bool dlssEvaluate(VkCommandBuffer cmd, DlssMode mode, const DlssFrame& f, std::string* why);
	// "sl=2.14.1 ngx=x.y.z" for the run's log line ("" before load).
	std::string versions();

	// dlss-fg-1 (DESIGN_dlss-fg-P0.md §4): ONE id per rendered frame, ONE sl::FrameToken for it. Reflex's sleep and markers, the
	// common constants, the tags and DLSS's evaluate all name the frame begun here, so DLSS-G can match its inputs to the present.
	// frameBegin: a new token for id; with Reflex loaded, slReflexSleep then eSimulationStart. False + why when Streamline refuses.
	bool frameBegin(uint32_t id, std::string* why);
	bool hasFrame() const { return m_frameToken != nullptr; }
	uint32_t frameId() const { return m_frameId; }
	void marker(PclMarker m);	// on the current frame; no-op before the first frameBegin or without Reflex
	// FG P0 H3 (the present hook, VkCtx.h PresentHook): the current frame's sl::FrameToken as an opaque handle, and a marker on the frame
	// that handle names - so ePresentStart / ePresentEnd carry the PRESENTED frame's id wherever the present runs (F9)
	void* frameHandle() const { return m_frameToken; }
	void markerOn(PclMarker m, void* frame);
	// Reflex: mode 0 off / 1 low latency / 2 + boost; frameLimitUs 0 - the game's FramePacer stays the only cap.
	bool reflexSetMode(uint32_t mode, std::string* why);
	bool reflexLatest(ReflexLatency* out);	// false: no complete report yet
	// The Windows message id PCL Stats pings the game with (ReflexState::statsWindowMessage); 0 without Reflex.
	uint32_t statsWindowMessage();
	// DLSS Frame Generation (sl.dlss_g).
	bool frameGenSupported(VkPhysicalDevice physical, std::string* why);
	// on = DLSSGMode::eOn with `generated` frames per real one (capped by the state's max). Takes effect at the next present; call it
	// on the presenting thread (DLSS_G guide §6.0).
	bool frameGenSetOptions(bool on, uint32_t generated, std::string* why);
	bool frameGenState(FrameGenState* out, std::string* why);
	// DLSS-G's HUD-less colour for the current frame (kBufferTypeHUDLessColor, eValidUntilPresent): the scene without HUD at back-buffer
	// size, in the layout `img` names when the present runs. Recorded into cmd (PREREG A39c).
	bool tagHudless(VkCommandBuffer cmd, const DlssImage& img, std::string* why);
	// The same for DLSS-G's UI alpha (kBufferTypeUIAlpha: R8, 1 = UI, 0 = none).
	bool tagUiAlpha(VkCommandBuffer cmd, const DlssImage& img, std::string* why);

	// slShutdown: before VkCtx::shutdown (Streamline releases its device objects first). loaderRoot() also calls it. Idempotent.
	void shutdown();

private:
	struct Api;	// the resolved Streamline entry points (GxStreamline.cpp)
	void* m_module = nullptr;	// HMODULE of sl.interposer.dll
	PFN_vkGetInstanceProcAddr m_gipa = nullptr;
	Api* m_api = nullptr;
	DlssMode m_lastMode = DlssMode::Off;
	uint32_t m_lastOutW = 0, m_lastOutH = 0;
	StreamlineFeatures m_features;
	void* m_frameToken = nullptr;	// sl::FrameToken* of the current frame (frameBegin)
	uint32_t m_frameId = 0;
	uint32_t m_constantsFrame = ~0u;	// the frame id the common constants were last set for: once per frame (DLSS_G §17)
};

}} // namespace GXRender::Upscale
