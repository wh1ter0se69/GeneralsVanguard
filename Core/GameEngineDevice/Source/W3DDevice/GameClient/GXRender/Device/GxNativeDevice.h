// GeneralsX: L1 in the game: the native d3d8 device.
//
// GxNativeDevice implements IDirect3DDevice8 over GXRender: VkCtx (+ VkSwapchain when there is a window), GXRES resources,
// the FF/custom pipeline cache, GxPacketWriter and GxD3D8Translate (which holds the d3d8 state). Textures, surfaces and
// vertex/index buffers are d3d8 objects over GxTexture / GxRenderTarget / GxBuffer. Everything the game can observe follows
// the DXVK d3d8 path it replaces (the CLASSIC reference, references/fbraz3-dxvk 954c5c692): back-buffer rotation, CopyRects'
// pool dispatch, reference counting, the present interval.
//
// Threading: the device is called by one thread at a time (the d3d8 ring hands it between main and its render thread with
// its own synchronisation - DESIGN §9); nothing here is thread-affine. Render-only: nothing the simulation reads.
// A23-B (v8.1): every entry point is a FRONT half (validation, bindings, refcounts, the
// getter mirror, host copies, creation) run on the caller and an EXEC half (translator, writer, GXRES uploads, recording,
// acquire, present) run on the device's own worker `gx-native-cs` in call order (GX_NATIVE_CS=1, the default), or inline right
// after the front half (GX_NATIVE_CS=0 = today's path, and inside every S point - §3.10).
#pragma once

#include <d3d8.h>
#if defined(_MSC_VER)
typedef BOOL WINBOOL;   // Wine/DXVK's spelling of BOOL (DXVK windows_base.h:38); the Windows SDK has only BOOL
#endif

#include "GxBuffer.h"
#include "GxD3D.h"
#include "GxCustomCatalog.h"
#include "GxNativeCs.h"
#include "GxNativeCsTrace.h"
#include "GxD3D8Translate.h"
#include "GxMotion.h"
#include "GxFsr1.h"
#include "GxNativePresent.h"
#include "GxPacketWriter.h"
#include "GxPipelineCache.h"
#include "GxRenderTarget.h"
#include "GxSceneCopy.h"   // F-depth+ (GxSceneTap.h, the game's side)
#include "GxTexture.h"
#include "GxWorldPass.h"   // render-hud-seam-1: Core/GameEngineDevice/Include/W3DDevice/GameClient (the seam's device interface)
#include "GxShadowMap.h"   // (S7): the shadow-map device interface (same folder)
#include "GxShadowCasters.h"
#include "VkCtx.h"
#include "VkGpuTimer.h"
#include "VkSwapchain.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

namespace GXRender { namespace Upscale { class Streamline; } }   // (GXRender/Upscale/GxStreamline.h)

namespace GXRender { namespace Dev {

// What the game (or a tool) gives the device. Every callback may be null.
struct GxNativeHost
{
	PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;  // null: VkCtx's own loader search (tests, tools)
	std::vector<const char*> instanceExtensions;              // the window system's (SDL_Vulkan_GetInstanceExtensions)
	// null: OFFSCREEN - no swapchain, Present ends the frame (tests, gxcap-replay --native)
	bool (*createSurface)(void* window, VkInstance instance, VkSurfaceKHR* surface) = nullptr;
	void (*destroySurface)(VkInstance instance, VkSurfaceKHR surface) = nullptr;
	void (*drawableSize)(void* window, uint32_t* width, uint32_t* height) = nullptr;
	bool (*presentSupport)(VkInstance, VkPhysicalDevice, uint32_t family) = nullptr;
	const char* deviceSelect = nullptr;   // VkCtxDesc::deviceSelect (the game passes DXVK's adapter description)
	bool validation = false;              // core + sync validation (fails creation when the layer is missing)
	int samplerAnisotropy = -1;           // dxvk.conf d3d9.samplerAnisotropy (GxNativeConfig)
	int presentInterval = -1;             // dxvk.conf d3d9.presentInterval (GxNativeConfig; >= 0 replaces the d3d8 interval)
	int tearFree = -1;                    // dxvk.conf dxvk.tearFree (GxNativeConfig; -1 Auto, 0 False, 1 True)
	bool prebuildGenericSet = true;       // DESIGN S10 (tests turn it off: every arm creates a device)
	const char* pipelineCacheDir = nullptr;   // per-machine VkPipelineCache + recorded keys ("" / null = none)
	void (*log)(const char* line) = nullptr;   // null: stderr
	bool orderCheck = false;              // TOOLS/TESTS: GxOrderCheck installed before any pipeline + the writer's check mode
	bool gpuTime = false;                 // GX_GPUTIME=1 (A16, default OFF): GPU time per frame on a VALIDATED VkGpuTimer
	bool motionUnits = false;             // GX_MOTION_UNITS=1 (A35, default OFF): create the motion bridge's gx:vel-* modules
	// S7 (A18/A20(b)): the look feature MASK every host fills once from GXGraphics - bit 0 = GXShadowMap.
	// 0 = today's device exactly: nothing of S7 is created, IID_GxShadowMap is not answered, the writer has no caster list.
	// (A108): bit 1 = GXAlphaToCoverage - the a2c shader family + prebuilt A2C twins, only when the creation MSAA is >= 2.
	uint32_t lookFeatures = 0;
	// GX_NATIVE_FIF=2|3 (A30 step 2, TEST knob): frames in flight, ONE value for the writer's ring and the swapchain's slots.
	// Default 2 = today. 0 = a refused value (anything but 2 or 3): the device logs it and keeps 2.
	uint32_t framesInFlight = 2;
	bool waitStats = false;               // GX_NATIVE_WAITS=1 (A30): a [GXWAIT] line (frame-ring + present-slot wait) per 120 presents
	// FG/Reflex submit-thread guard (FG P0 §2): what the PLAYER REQUESTED (GXFrameGen 0-4 / GXReflex 0-2), filled by BOTH hosts from
	// GXGraphics, not whether Streamline loaded. Either > 0 turns the gx-vk-submit thread OFF (wins over GX_VK_SUBMIT_THREAD=1) until
	// frame generation's census has run at thread=1.
	int requestedFrameGen = 0;
	int requestedReflex = 0;
	int requestedUpscaler = 0;           // GXGraphics::upscaler(): non-zero = the world/HUD seam runs (render/dlss x A23-B guard below)
	bool testNoFifGuard = false;         // TESTS: X-fifguard-*'s plant - FIF 3 kept although FG/Reflex is requested (creation-time)
	bool testShadowMapCreateFails = false;   // TESTS (S7 P2, A36 item 5): the atlas creation fails -> a key-off session (sm-create-fail)
	bool testShadowMapFailKeepsBit = false;  // TESTS: X-sm-create-fail's plant - the look bit is kept after that failure (creation-time)
	bool testA2cNoFallback = false;          // TESTS: X-a2c-prewarm-fail's plant - a failed init prewarm with A2C on fails init
	                                         // (the "A2C turns itself off" fallback removed; creation-time)
	bool testSwapchainInterposed = false;  // TESTS (FG P0 H3): the swapchain images may be proxies, as with DLSS-G loaded (creation-time)
	uint32_t testSwapchainDropUsage = 0;   // TESTS (FG P0 H3): VkImageUsageFlags the swapchain does NOT request - the planted proxy
	bool testSwapchainViewsWithoutUsage = false; // TESTS: X-present-direct-interposed-views' plant - swapchain views made without proven
	                                             // COLOR_ATTACHMENT, the one-pass present still refused (creation-time)
	bool testPresentDirectIgnoresUsage = false;  // TESTS: X-present-direct-usage / -interposed's plant - swapchain views made and the
	                                             // one-pass present taken without proven COLOR_ATTACHMENT (creation-time)
	bool testNoCsDlssGuard = false;      // TESTS: X-csguard-* plant - the render/dlss guard on the A23-B worker is skipped (creation-time)
	bool testNoFgThreadGuard = false;   // TESTS: X-fgguard-* plant - the guard is skipped (a creation-time plant, like testFifWriterStays2)
	// DESIGN S9: when set (16 bytes = DXVK's D3DADAPTER_IDENTIFIER8::DeviceIdentifier), the picked physical device's
	// VkPhysicalDeviceIDProperties::deviceUUID must equal it, checked right after VkCtx::init (before any surface or swapchain)
	const uint8_t* requireDeviceUuid = nullptr;
	const Res::FormatCaps* testFormatCaps = nullptr;   // TESTS: forced GXRES capabilities (e.g. no BC: DXT decoded on upload)
	// GeneralsX: (PREREG A36.2): the HMODULE of a vetted interposer that owns
	// getInstanceProcAddr (NVIDIA Streamline's, loaded only for DLSS on NVIDIA); session row S8 accepts exactly that module.
	void* loaderInterposer = nullptr;
	// GeneralsX: (PREREG A36.2): NVIDIA Streamline, set by the install code ONLY in a
	// GX_STREAMLINE build when the player chose DLSS on an NVIDIA adapter - otherwise null and nothing of it is in the process.
	// getInstanceProcAddr is then its interposer's (S8 accepts loaderInterposer = its HMODULE).
	Upscale::Streamline* streamline = nullptr;
	bool testFifWriterStays2 = false;   // TESTS: X-fif-effective's plant - the writer keeps 2 while the knob says 3 (read at creation)
	// (F-depth+, GxSceneCopy.h), TEST-ONLY, read at creation and OR'd with the environment:
	uint32_t fdepthProbe = 0;            // GX_FDEPTH_PROBE=depth|colour|both: the dummy reader's GxSceneNeed bits (0 = none)
	bool fdepthForcePass = false;        // GX_FDEPTH_RESOLVE=pass: never the in-scope resolve (the A/B's other arm)
};

// (GxNativeDlss.cpp, PREREG A36.2): fills host.streamline / getInstanceProcAddr / loaderInterposer when
// wantDlss (the player's GXUpscaler=dlss), this is a GX_STREAMLINE build, DXVK's default adapter is NVIDIA and NVIDIA's signed
// Streamline loads from GX_STREAMLINE_DIR or <exe dir>\Streamline. Otherwise the host is untouched and nothing of it loads.
// One line on stderr either way when wantDlss.
// (PREREG A39): reflex = GXReflex 0-2, frameGen = GXFrameGen 0-4 (generated frames per real one); either one
// loads Streamline too (Reflex / DLSS Frame Generation), with the same NVIDIA-adapter + signed-DLL gates.
void GxNativeLoadStreamline(IDirect3D8* dxvk, GxNativeHost& host, bool wantDlss, int reflex = 0, int frameGen = 0);
// (PREREG A39b): the game's Windows message loop reports every message here (any thread may call it: atomics only).
// PCL Stats' latency ping and a left click (the Reflex flash) become PCL markers on the device's next frame. No-op without Reflex.
void GxNativeNoteWindowsMessage(unsigned message);

// TEST-ONLY plants (gx_native_test's X- arms); all false in the product.
struct GxNativeDebug
{
	bool a2cResetNoPrewarm = false;          // (A108): an A2C session's Reset to another MSAA skips the re-prewarm (a2c-reset must fail)
	bool a2cResetNoRetarget = false;         // ...prewarms the census parts but not the keys the session already drew (a2c-reset-draw)
	bool a2cNoFallback = false;              // a failed A2C prewarm does NOT turn A2C off (X-a2c-prewarm-fail[-reset]; the init arm
	                                         // hands it to host.testA2cNoFallback for the device it creates)
	// GxMotion's plants, copied into it at every world Begin
	bool motionSampledAlways = false;        // depth gets SAMPLED with the key off (motion-off must fail)
	bool motionIdentityReprojection = false; // clipToPrevClip = identity (mv-camera must fail)
	bool motionJitterInVelocity = false;     // the resolve keeps the jitter in the vectors (mv-zoom-jitter must fail)
	bool motionFlipJitterSign = false;       // the projection offset has the wrong sign (jitter-sign must fail)
	bool motionJitterAfterEnd = false;       // End leaves the jittered projection in force (jitter-scope must fail)
	bool motionIgnoreObject = false;         // the resolve never reads the object vectors (mv-object must fail)
	bool motionAbandonKeepsJitter = false;   // an abandoned pass leaves its jittered projection (jitter-abandon must fail)
	// (F-depth+): GxSceneCopy's plants, copied into it at every OpaqueDone
	bool fdepthTapWithoutReader = false;     // OpaqueDone takes the copies with no reader on (fdepth-off must fail)
	bool fdepthSecondTap = false;            // a second OpaqueDone in one frame takes them again (fdepth-once must fail)
	bool fdepthFallbackNothing = false;      // the pass / copy path records nothing but publishes (fdepth-1x / -pass must fail)
	bool motionNoSeed = false;               // Begin does not seed the clean projection set before motion ran (jitter-inherit must fail)
	bool shadowCmdDirect = false;     // S7 X-sm-queued: IGxShadowMap's commands run on the CALLER instead of the worker's exec half
	bool smLookFrameStale = false;    // S7 P2 X-sm-receive: the writer never writes set 2's UBO (handed to it by BeginShadowFrame)
	bool smNoRebind = false;          // S7 P2 X-sm-rebind: set 2 not rebound after outsideScope (A36 4a; handed over by BeginShadowFrame)
	bool smNoDivide = false;          // S7 A39 X-sm-persp-divide: the receivers read the perspective map undivided (handed over likewise)
	bool smCreateFailKeepsBit = false;// S7 P2 X-sm-create-fail's marker: the harness hands it to host.testShadowMapFailKeepsBit (creation-time)
	bool noRotate = false;            // Present does not rotate the back buffers
	bool fifFgGuardSkipped = false;   // gx_native_test X-fifguard-*'s marker: the harness hands it to host.testNoFifGuard BEFORE creation
	bool csDlssGuardSkipped = false;  // gx_native_test X-csguard-*'s marker: the harness hands it to host.testNoCsDlssGuard BEFORE creation
	bool fgGuardSkipped = false;      // gx_native_test X-fgguard-*'s marker: the harness hands it to host.testNoFgThreadGuard BEFORE creation
	bool fifWriterStays2 = false;     // gx_native_test X-fif-effective's marker: the harness hands it to host.testFifWriterStays2 BEFORE
	                                  // creation (the writer is made once; set afterwards it would change nothing)
	bool noPrivateRefs = false;       // binding a texture / buffer takes no private reference
	bool levelSurfaceOwnRef = false;  // a level surface counts its own references instead of its container's
	bool copySkipsResolve = false;    // CopyRects from an MSAA surface copies nothing
	bool dropIgnoresPin = false;      // a pinned MANAGED texture is dropped again
	bool dropDraws = false;           // draws are not recorded
	bool countNothing = false;        // never-called methods are not counted
	bool childrenDontRetain = false;  // a child's first public reference does not AddRef the device
	bool fastPathRectOnly = false;    // CopyRects' RT -> SYSTEMMEM fast path copies only the rect
	bool updateIgnoresDirty = false;  // UpdateTexture copies whole levels, not the dirty box
	bool resetIgnoresLosable = false; // Reset succeeds with live DEFAULT-pool objects
	bool resetSkipsD3D8Checks = false; // Reset accepts parameters d3d8 refuses (windowed interval, COPY + 2 buffers)
	bool configFirstLineWins = false;  // GxNativeConfigDebug::firstLineWins (gx_native_test's config cases)
	bool configIgnoresUnknown = false; // GxNativeConfigDebug::ignoreUnknownKeys
	bool configIgnoresPresentKeys = false; // GxNativeConfigDebug::ignorePresentKeys
	bool uuidGateUnwired = false;      // gx_native_test: the S9 UUID is not handed to the device (the gate never runs)
	bool lockDropsDiscard = false;     // a buffer Lock passes DISCARD on as NOOVERWRITE (writes into the slice a draw reads)
	bool presentSkipsTransition = false; // the present copy reads the back buffer without moving it to TRANSFER_SRC (the one-pass
	                                     // present: renders into the swapchain image without moving it to the attachment layout)
	bool ignoreDrawableSize = false;  // a resize is not followed: no window-size check, a Suboptimal status not acted on
	bool failNextRecreate = false;    // the next swapchain recreation fails (TEST fault injection, not a defect)
	bool noRecreateRetry = false;     // after a failed recreation the device never recreates again
	bool presentTargetFailsOnce = false; // the next present-target allocation fails (TEST fault injection, not a defect)
	bool targetCheckAfterAcquire = false; // the present target is made after the acquire (a failure strands the acquired image)
	bool presentPipelineFails = false; // the present shader reports failure (TEST fault injection, not a defect)
	bool injectSurfaceLost = false;   // the next acquire reports SURFACE_LOST before acquiring (TEST fault injection)
	bool injectCreateSurfaceLost = false; // the surface is lost and a recreation is due before the acquire (as after a resize): swapchain
	                                  // creation on it fails until a new surface exists (TEST fault injection, design-r5 #1)
	bool injectPresentSurfaceLost = false; // the next present reports SURFACE_LOST and the surface stays lost (TEST fault injection)
	bool failNextSurfaceCreate = false; // the next VkSurfaceKHR creation fails (TEST fault injection)
	bool failFrameInRecreate = false; // the writer frame after the next recreation cannot begin (TEST fault injection, design-r5 #2)
	uint32_t failFrameInRecreateAfter = 0; // A23-B: the same for the Nth recreation from now (1 = the next) - an S Present's retry
	bool createLossUnclassified = false; // a swapchain creation that failed on a lost surface is not recognised (no new surface)
	bool retryAcquireSkipsFrameCheck = false; // the retry acquire after a recreation does not check that a writer frame is open
	bool resizeComparesActualExtent = false; // a resize is the window vs the swapchain's ACTUAL extent (a clamped one recreates forever)
	uint32_t drawableSizeOffset = 0;  // added to the host's drawable size (TEST fault injection: a size the surface clamps away)
	bool failNextBackBuffer = false;  // the next back-buffer allocation fails, after the swapchain was built (TEST fault injection)
	bool presentWhileNotReset = false; // Present runs on a NotReset device (DXVK answers DEVICELOST and presents nothing)
	bool injectPresentError = false;  // the next vkQueuePresentKHR fails as OUT_OF_*_MEMORY: not enqueued (TEST fault injection)
	bool presentErrorIgnored = false; // a generic present failure is not followed by a rebuild (the image stays stranded)
	bool resetDropsPresentStatus = false; // R19: Reset leaves the submit thread's pending present status untaken (stale at the next Present)
	bool presentDropsPresentStatus = false; // R19: Present takes the previous present's deferred status and ignores it
	bool threadSkippedPresentOk = false;  // R19: (forwarded to Vk::g_testMutants by the arm) a present skipped behind a refusal reads Ok
	bool orderCheckStaysInstalled = false; // the device is destroyed without GxOrderCheck::uninstall (the next device goes unchecked)
	bool swvpStateFromTranslator = false; // GetRenderState(SOFTWAREVERTEXPROCESSING) answers the translator's slot, not the mode
	bool getIndicesNeverCalled = false; // GetIndices still counted as a never-called method (L2's fallback calls it)
	bool getTextureNeverCalled = false;      // these four still counted as never-called methods (the world/HUD seam's composite
	bool getStreamSourceNeverCalled = false; // saves and restores the device state with them: render-hud-seam-2)
	bool getVertexShaderNeverCalled = false;
	bool getPixelShaderNeverCalled = false;
	bool presentIgnoresDeviceLoss = false;
	bool finalNotOnce = false;        // the final stats line may print twice (exit flush, then teardown)
	bool finalAtExitOff = false;      // the exit flush prints nothing (a device the game never destroys has no final line)
	bool exitNoQuiesce = false;       // the exit flush leaves gx-vk-submit running (skips VkCtx::quiesce) while the process ends
	bool resetUnlogged = false;       // a successful Reset logs nothing (the log keeps only the creation extent)
	bool loseDeviceMidPresent = false; // the next Present latches a device loss right after its ensureFrame (TEST fault injection)
	bool presentLateLossOk = false;   // a loss latched during a Present is not re-checked before it returns (one more D3D_OK) // Present does not check the latched device loss (answers D3D_OK, the frame skipped)
	bool targetSizeRefusalUncounted = false; // the §16f SetRenderTarget refusal is not counted (the stats line shows 0)
	bool surfaceLostNotRecovered = false; // a lost surface is not made again (presentation stays stopped)
	bool failNextSubmit = false;      // the next present frame's submission is lost (TEST fault injection)
	bool presentAfterFailedSubmit = false; // presents although the frame's submission failed (waits on an unsignalled semaphore)
	bool presentFailNoClear = false;  // a failed present shader leaves the target as it was (a stale frame is shown)
	bool resetZFromOldParams = false; // a Reset's ZENABLE comes from the previous parameters
	bool resetBKeepsLost = false;     // Reset leaves a NotReset device NotReset when d3d9's early validation fails (design-r3 #7)
	bool presentResolveAverages = false; // an MSAA back buffer presented through vkCmdResolveImage (averages the encoded values)
	bool presentBlitLinear = false;   // a scaled present through vkCmdBlitImage LINEAR instead of DXVK's footprint rule
	bool copySrcMipZero = false;      // a GPU CopyRects reads level 0 of the source texture whatever its level
	bool implicitNotLosable = false;  // a back buffer / the auto depth the caller holds is not counted as losable (Reset ignores it)
	bool lodStoresPrevious = false;   // SetLOD stores the previous LOD
	bool updateNoBlockAlign = false;  // UpdateTexture's scaled dirty box is not aligned to compression blocks
	bool destroyImmediately = false;  // the last private release destroys on the releasing thread
	bool resetPhasesCollapsed = false; // Reset's late (recreation) checks run before the unbinding
	bool copyDecodedAsBlocks = false; // a decoded DXT level read back from the GPU is copied as if it were blocks
	bool resetDropsRetainedStorage = false; // Reset destroys the storage of a back buffer the caller still holds (code-r6 #2)
	bool resourceErrorsUncounted = false; // GXRES error lines are printed but not counted (ask)
	bool copyNoAliasLayout = false;   // a GPU copy between two levels of one texture transitions the image twice
	bool forgetDropsPendingClear = false; // destroying an attachment of the writer's pass drops its pending clear
	bool csRingOverwrite = false;     // A23-B: one command stream push skips its room check (overwrites an unread record)
	// A23-B §7 plants (each inert while the device runs direct; an X- arm with the worker held must catch it):
	bool csGetterReadsWorker = false; // X-cs-getter-reads-worker: GetRenderState reads the translator, not the front mirror
	bool csDiscardEarly = false;      // X-cs-discard-early: a DISCARD reuses a slice queued commands may still bind (workerDone ignored)
	bool csUnlockByRef = false;       // X-cs-unlock-by-ref: a static unlock's bytes are read when the worker runs it, not at the call
	bool csTexUnlockByRef = false;    // X-cs-texunlock-by-ref: the same for a MANAGED texture's UnlockRect
	bool csUpByRef = false;           // X-cs-up-by-ref: DrawPrimitiveUP reads the caller's vertices when the worker runs it
	bool csCopyLateRead = false;      // X-cs-copy-late-read: CopyRects host -> DEFAULT reads the source when the worker runs it
	bool csNoLead = false;            // X-cs-no-lead: Present never waits for the worker to finish the Present before it
	bool csWakeEveryRecord = false;   // X-cs-wake (A55): the stream's worker wakes a waiting producer after every record
	bool csWakeLate = false;          // X-cs-wake-target (A55): ... only once it ran PAST the awaited seq (a lost wake-up)
	bool csLostUnfenced = false;      // X-cs-lost-unfenced: a PresentLost is not fenced
	bool csLossUnpublished = false;   // X-cs-loss-unpublished: the front never sees a loss the worker latched
	bool csReorder = false;           // X-cs-reorder: the next SetRenderState runs after the draw that follows it
	bool csFullFails = false;         // X-cs-full-fails: a SetRenderState that finds the ring full answers INVALIDCALL instead of waiting
	bool csExecPhysical = false;      // X-cs-exec-physical: a front creation is also recorded into the EXEC stream at its physical time
	bool csRaceRot = false;           // X-cs-race (the TSan lane's proof it sees this code): the front reads the worker's m_rot
	// A23-B §3.8 TEST-ONLY allocation knobs and plants (arms 17, 20, 23, 27-31)
	const char* allocFailSite = nullptr; // the next allocFailCount in-scope allocations at this site ("*" = any) fail as OUT_OF_DEVICE_MEMORY
	uint32_t allocFailCount = 0;
	uint64_t allocBudget = 0;         // a TEST VMA budget (bytes of VMA usage over all heaps): an allocation past it fails
	bool csNoRetry = false;           // X-cs-no-retry: a failed front creation answers INVALIDCALL without the S retry
	bool csRetryDoubleCount = false;  // X-cs-retry-double-count: the failed attempt counts a creation too
	bool csDiscardNoRetry = false;    // X-cs-discard-noretry: a failed DISCARD slice answers INVALIDCALL without the S retry
	bool csNoReclaim = false;         // X-cs-no-reclaim: recovery never frees idle dynamic slices
	bool csReclaimNoWait = false;     // X-cs-reclaim-nowait: recovery does not wait for submitted work
	const char* csRecoveryBypass = nullptr; // X-cs-recovery-bypass: a failure at this site skips recovery
	bool csTerminalSilent = false;    // X-cs-terminal-silent / X-cs-failure-silent: a terminal failure is not counted
	bool csSbodyNoRecover = false;    // X-cs-sbody-norecover: an allocation first met inside an S body never recovers
	bool csCreateDefer = false;       // X-cs-create-defer: a failed front creation answers D3D_OK (the failure deferred past the call)
	bool updateFailLastMip = false;   // TEST (arm 24): UpdateTexture fails on its LAST mip, after staging the others (skips its flush)
	bool csFlushDropped = false;      // X-cs-flush-dropped: an F unlock never emits its FlushIdle
	bool csExecNoFlush = false;       // X-cs-exec-noflush: an upload exec half on the worker drops its flushIdleUploads
	bool csRecreateOnWorker = false;  // X-cs-recreate-on-worker: the worker's Present runs the swapchain recreation itself
	bool csWorkerDrawable = false;    // X-cs-worker-drawable: the worker reads the drawable size through the host callback
	bool csResetNoDrain = false;      // X-cs-reset-nodrain: Reset runs its body without draining the worker first
	bool testSwapchainReadback = false; // TEST: every present also copies the finished swapchain image to a host buffer (testReadSwapchain)
	bool csRecoveryClip = false;        // X-cs-recovery-clip: the first present after a recreation (CS=1) leaves the right eighth unwritten
	bool csRecoveryMirror = false;      // X-cs-recovery-mirror: the first present after a recreation (CS=1) is blitted mirrored
	bool csRecoveryHalfCopy = false;    // X-cs-recovery-half-copy: the first present after a recreation (CS=1) blits half the image
	bool csPsByRef = false;             // X-cs-ps-by-ref: CreatePixelShader's code read from the caller when the worker runs it
	bool csVsByRef = false;             // X-cs-vs-by-ref: CreateVertexShader's decl + code read from the caller when the worker runs it
	bool csDropCreateVs = false;        // X-cs-drop-create-vs: CreateVertexShader answers a handle, the command is not sent (CS=1)
	bool csRecoverIgnoresLatch = false;   // X-cs-recover-ignores-latch: recovery collects after a failed wait / a latched refusal
	bool csRecoverIgnoresLatchWorker = false; // X-cs-recover-ignores-latch-worker: the same, in the WORKER's recovery only
	bool csPsBindDropped = false;       // X-cs-ps-bind-dropped: SetPixelShader's exec half (CS=1) binds 0, the trace names the handle
	bool csLightEnableTrue = false;     // X-cs-light-enable-true: LightEnable's exec half (CS=1) forwards TRUE, the trace keeps the call's
	                                    //
	bool csIndexedStartZero = false;    // X-cs-indexed-start-zero: DrawIndexedPrimitive's exec half (CS=1) passes start index 0, the trace
	                                    // keeps the call's
	bool csClearRectShort = false;      // X-cs-clear-rect-short: Clear's rectangle payload copies count x 4 bytes (CS=1)
	bool csDeleteSkipsExec = false;     // X-cs-delete-skips-exec: Delete{Pixel,Vertex}Shader's exec half (CS=1) traces and never deletes
	                                    //
	bool csIndicesBaseZero = false;     // X-cs-indices-base-zero: SetIndices' exec half (CS=1) passes base vertex 0, the trace keeps the
	                                    // call's
	// rw3-X-3 (A23-B merge gate): the translator-input plants (item 3) and the plants
	bool csTrVsCodeAtDecl = false;      // X-strict-vs-code: CreateVertexShader's exec half (CS=1) hands the translator the declaration's
	                                    // words as the code (a ring offset slip); the trace hashes the right bytes - payload class
	bool csTrSkipRenderState = false;   // X-strict-light-rs: SetRenderState's exec half (CS=1) traces and never reaches the translator
	                                    // - dropped-call class
	bool csMaterialByRef = false;       // X-cs-ff-payload-material: SetMaterial's command keeps the caller's pointer, read when the
	                                    // worker runs it (r10-oracle P2-1)
	bool csLightByRef = false;          // X-cs-ff-payload-light: the same for SetLight
	bool csViewportByRef = false;       // X-cs-ff-payload-viewport: the same for SetViewport
	bool csDeleteSkipsUnbound = false;  // X-strict-shader-unbound-delete: Delete{Vertex,Pixel}Shader's exec half (CS=1) deletes only a
	                                    // BOUND shader - an unbound one keeps its record (r10-oracle P2-2)
	bool csShaderOversizeRefused = false; // X-strict-shader-oversize: Create{Vertex,Pixel}Shader's oversize S branch (CS=1) answers
	                                      // INVALIDCALL instead of today's body
	bool csOversizeLateDrain = false;   // X-cs-shader-oversize-order / X-cs-oversize-order: an oversize S body that calls its inline
	                                    // exec explicitly (Create{Vertex,Pixel}Shader, UnlockRect, CopyRects host -> DEFAULT) runs it
	                                    // BEFORE entering S - an older queued command can still run
	bool csCopyOversizeRefused = false; // X-strict-copy-oversize: CopyRects' host -> DEFAULT oversize branch (CS=1) answers INVALIDCALL
	                                    // instead of today's body
	bool csCopyDestOrigin = false;      // X-strict-copy-offsets: the queued host -> DEFAULT copy's exec half (CS=1) writes at the
	                                    // destination's origin instead of the call's point
	// rw3-X-3, Lock/Unlock + CopyRects census (one plant per forwarding error it names; inert when false)
	bool csUploadOffsetZero = false;    // X-strict-buffer-ranges-offset: a static unlock's upload (CS=1) lands at offset 0
	bool csPlainLockForwardsDiscard = false; // X-strict-buffer-ranges-plain: the plain-lock S fallback locks with DISCARD
	bool csRectPitchSwap = false;       // X-strict-texture-partial: a partial MANAGED upload (CS=1) passes the level pitch to the
	                                    // queued payload and the payload pitch to the oversize S body (each the other's)
	bool csHostCopyTwinA8 = false;      // X-strict-host-copy-fmt: the host-copy twin is A8R8G8B8 whatever the source format
	bool csHostCopyTwinSquare = false;  // X-strict-host-copy-shape: the twin's height is the copied width
	bool csHostCopyTwinLevel0 = false;  // X-strict-host-copy-mip: the twin is copied into destination level 0
	bool csCopySkipDestBlockCheck = false; // X-strict-host-copy-dxt-dest: the queued host copy skips the front block-rect check
	bool csCopyValidateSource = false;  // X-strict-host-copy-dxt-src: the queued host copy refuses an off-grid block source rect
	bool csReadbackNarrowS = false;     // X-cs-copy-readback-s: CopyRects' read-back S only for DEFAULT -> SYSTEMMEM
	bool csRestoreSOnlyReadonly = false; // X-cs-managed-restore-write: a dropped level's restore enters S only for READONLY locks
	bool csCopyGpuSquareExtent = false; // X-strict-gpu-copy-rect: the queued GPU copy (CS=1) takes the width as the height
	bool csCopyGpuMsaaFromSrc = false;  // X-strict-gpu-copy-msaa: the queued GPU copy resolves whenever the source is multisampled
	bool csGetDescDrains = false;       // X-cs-getdesc-nodrain (X'): GetDesc reads the format table through the DRAINING resources()
	bool csGetDescRace = false;         // X-cs-getdesc-race (X', TSan-judged): GetDesc of a texture level also reads the image's
	                                    // worker-written lastUse on the front - a real race the TSan lane must report
	bool csDrainTimeZero = false;       // X-cs-drain-attribution (X'): a csSync (accessor) drain is recorded with 0 ns
	bool csDrainTimeZeroS = false;      // X-cs-drain-attribution-s: an SScope drain is recorded with 0 ns
	// rw3-X-3,
	bool csHostCopyTwinPhysFmt = false; // X-strict-host-copy-nobc: the twin takes the destination's PHYSICAL format (A8R8G8B8 decoded)
	bool csFrontRefusalCounts = false;  // X-cs-host-copy-refused-count: the front block-check refusal (CS=1) counts a copiesRefused
	bool csWorkerDefaultStack = false;  // X-cs-deep-clear: only selects the X- child (which sets GxNativeCs::testDefaultStack before its
	                                    // device); no effect on the device itself
	bool csMultiplySkip = false;        // X-strict-multiply: MultiplyTransform's S body (CS=1) skips the multiplication, keeping the
	                                    // drain, the accounting and D3D_OK (r10-oracle P2-3)
	bool csPresentTransposed = false;   // X-cs-present-transposed: the worker's Present (CS=1) takes the call's width and height swapped
	                                    //
	bool csDeleteUnbinds = false;       // X-cs-delete-unbinds: Delete{Pixel,Vertex}Shader's exec half (CS=1) unbinds instead of
	                                    // deleting - a shader deleted while bound stops drawing
	bool csPsBindFront = false;         // X-cs-ps-bind-front: SetPixelShader's exec half (CS=1) binds the FRONT's current m_ps, not
	                                    // its call's handle
	bool csPsDrawDropped = false;       // X-cs-ps-draw-dropped: DrawPrimitiveUP's exec half (CS=1) traces, opens the frame and draws
	                                    // nothing while a pixel shader is bound
	bool csVsBindWrong = false;         // X-cs-vs-bind-wrong: SetVertexShader's exec half (CS=1) binds handle + 2 for a programmable
	                                    // shader, the trace names the handle
	bool csRecoveryLinear = false;      // X-cs-recovery-linear: the first scaled present after a recreation (CS=1) resizes through
	                                    // vkCmdBlitImage LINEAR instead of the present shader
	bool csSliceTerminalSilent = false; // X-cs-slice-terminal-silent: a terminal "slice" failure is not counted or logged (CS=1)
	bool csRecoveryStaleSource = false; // X-cs-recovery-stale-source: the first scaled present after a recreation (CS=1) shows the
	                                    // present target's old content - counters unchanged, the shown frame wrong
	bool csDropIndexed = false;       // X-cs-drop-indexed: an indexed draw is answered and never sent (CS=1)
	bool csConstByRef = false;        // X-cs-const-by-ref: shader constants read from the caller's memory when the worker runs them
	bool csConstOverread = false;     // X-cs-const-overread: the front copies count x 16 bytes, not today's clipped range
	bool csDropConstants = false;     // X-cs-drop-constants: shader-constant commands not sent (CS=1)
	bool csDropLight = false;         // X-cs-drop-light: SetLight not sent (CS=1)
	bool csDropLightEnable = false;   // X-cs-drop-light-enable: LightEnable not sent (CS=1)
	bool csDropCreatePs = false;      // X-cs-drop-create-ps: CreatePixelShader answers a handle, the command is not sent (CS=1)
	bool csWaitUnpublished = false;   // X-cs-wait-unpublished: producer waits skip the publish (the arm's child hangs)
	bool csPayloadLate = false;       // X-cs-payload-late: one static unlock's bytes read at execution, pushed only at the next unlock
	bool csDropLod = false;           // X-tap-lod: the SetLod command is not sent (the worker's view keeps the old mip range)
	bool csBbStale = false;           // X-tap-substitute: the worker resolves the back buffer's storage one rotation stale
	bool csMirrorViewport = false;    // X-cs-mirror-viewport: SetViewport leaves the getter mirror as it was
	bool csTransformCopyAll = false;  // X-cs-transform-copyall: SetTransform copies 64 bytes for every type (an unmodelled one too)
	bool csClearNullify = false;      // X-cs-clear-nullify: Clear(0, non-null rects) sends the rects as null (the viewport cleared)
	bool csUpNullEmpty = false;       // X-cs-up-nullempty: an UP draw that reads 0 bytes sends its non-null pointer as null
	bool csUpShortCopy = false;       // X-cs-up-shortcopy: the front copies one stride less than today's size
	bool csUpGuard = false;           // TEST knob (arm 6w): the UP exec half reads its bytes from a copy that abuts a PROT_NONE page
	bool csExitNoSpecQuiesce = false; // X-cs-exit-spec-running: the exit settle leaves the pipeline cache's spec workers running
	bool csExitNoSettle = false;      // X-cs-exit-nosettle: the exit flush prints the final line before lever 3's drain and status
	bool csExitUnwatched = false;        // X-cs-exit-stuck (A60): the CRT-exit quiesce runs without its watchdog
	bool csExitWatchNoProgress = false;  // X-cs-exit-slow (A60): the watchdog counts its limit from the start, progress or not
	bool csWorkerSelfDrain = false;   // X-cs-worker-selfdrain: the worker's recovery drains the stream (waits for its own command)
	bool csFailureSilent = false;     // X-cs-failure-silent: a worker-side terminal failure counts nothing (alloc_failures, resource_errors)
	VkSemaphore testGateSem = VK_NULL_HANDLE; // TEST (arms 2g, 23b): the next OFFSCREEN frame submission also waits on this timeline
	uint64_t testGateValue = 0;               // semaphore value (a host gate holds the frame on the GPU); consumed by that submission
	bool csSerial = false;            // A23-B §7 STRICT (TEST-ONLY, not a plant): the SERIAL schedule - every exec half dispatched is
	                                  // run (the worker drained) and the device idled before the caller goes on (both modes); the
	                                  // caller also sets g_gxPipeTestMutants.syncSpec (getFF waits for its specialised build)
	bool vsConstCapsFromDxvk = false; // GetDeviceCaps passes DXVK's 256 vertex-shader constants through (the translator holds 96; rw3-R11)
	// rw3-W lever (2) W-a, the one-pass present (A20)
	bool presentDirectOff = false;    // TEST control, not a plant: the one-pass present is not taken (the old path, inside an arm)
	bool presentDirectAverage = false; // the one-pass present averages MSAA samples as stored values, not in linear light
	bool presentDirectAnyExtent = false; // the one-pass present is taken although the window is not the back buffer's size
	bool presentDirectIgnoresFit = false; // the one-pass present is taken where the old path's 1:1 fit does not cover the image
	bool swapchainViewsWithoutUsage = false; // FG P0 H3: X-present-direct-interposed-views' marker (host.testSwapchainViewsWithoutUsage)
	bool presentDirectIgnoresUsage = false; // FG P0 H3: X-present-direct-usage / -interposed's marker - the harness hands it to
	                                        // host.testPresentDirectIgnoresUsage BEFORE creation (the views are made with the swapchain)
	bool capturePresent = false;      // TESTS: every present also copies the swapchain image into a host buffer (testReadPresent)
	// (tier 1): each drops ONE seam entry point's SScope - its body then records on the front
	// while the A23-B worker runs (the ownership counter must catch it)
	bool seamNoSResolve = false;      // X-seam-resolve: WorldPass::ResolveLinear without its S
	bool seamNoSUpscale = false;      // X-seam-fsr1 / X-seam-rcas: WorldPass::Upscale without its S
	// X-own-present-read - testReadPresent / testReadSceneProbe drain with a bare csSync again (m_inline stays 0, so
	// their endFrameForHost counts as a front recording at CS1): the gx_native_test runner's ownership gate must fail the clean case
	bool accessorBareSync = false;
	// (phase 2): the motion entry points' S, one each
	bool seamNoSBegin = false;        // X-seam-begin: worldBegin without its S
	bool seamNoSEnd = false;          // X-seam-end: worldEnd without its S
	bool seamNoSVelocity = false;     // X-mv-object-late: worldObjectVelocity without its S
};

// Live native devices in the process (tests: device retention by its children).
extern std::atomic<int> g_gxNativeLiveDevices;

struct GxNativeStats
{
	uint64_t liveTextures = 0, liveSurfaces = 0, liveBuffers = 0;
	uint64_t creates = 0, createsRefused = 0;
	uint64_t draws = 0, drawsRefused = 0, clears = 0, presents = 0;
	uint64_t dropped = 0, restores = 0, pinned = 0, readbacks = 0, gpuCopies = 0, resolves = 0, uploadFlushes = 0;
	uint64_t neverCalled = 0;
	uint64_t gammaIgnored = 0;
	uint64_t resourceErrors = 0;      // GXRES errors (a staging / upload failure): the d3d8 call itself answered D3D_OK
	uint64_t copiesRefused = 0;       // a CopyRects the device cannot do as DXVK does (a decoded DXT level read back as blocks)
	uint64_t vsConstantsRefused = 0;  // SetVertexShaderConstant past kGxNativeMaxVsConst (rw3-R11): INVALIDCALL, counted in bad
	uint64_t targetsRefused = 0;      // a render target the device cannot attach (a texture level > 0): its clears/draws refused
	uint64_t targetSizeRefused = 0;   // SetRenderTarget refused for a depth smaller than the target (DESIGN 16f residual: must stay 0)
	uint64_t presentsSkipped = 0;     // frames not shown (swapchain recreation / acquire / present target failed; retried next Present)
	uint64_t acquireNotOk = 0;        // acquires answering Suboptimal / OutOfDate / ... (the first ones logged)
	uint64_t surfacesRecreated = 0;   // VkSurfaceKHRs made again after a surface loss
	uint64_t swapchainsRecreated = 0; // swapchains rebuilt at a Present (resize, Suboptimal, loss, a failed earlier recreation)
	uint64_t presentErrors = 0;       // the present shader could not draw: the frame was shown black
	uint64_t presentFailures = 0;     // vkQueuePresentKHR failed otherwise (out of memory): the swapchain is rebuilt
	uint64_t stretchedPresents = 0;   // back buffer and window extents differed: DXVK's scaled present (DESIGN 8.2)
	uint64_t presentsDirect = 0;      // rw3-W W-a: presents drawn straight into the swapchain image (no window-sized target)
	uint64_t presentsShown = 0;       // FG P0 H3: presents the swapchain accepted (Ok / Suboptimal; enqueue-time under the submit thread)
	int64_t losable = 0;              // live DEFAULT-pool objects the game created (DXVK's m_losableResourceCounter)
};

struct GxNativeValidation
{
	uint32_t errors = 0, hazards = 0;
	std::string first;
};

// The vertex-shader constants this device implements (GxCustomCatalog.h kGxCsVsConstCount): GetDeviceCaps reports MaxVertexShaderConst
// = min(DXVK's, this) and SetVertexShaderConstant past it is refused (rw3-R11); gxcap-replay --native's caps check (e) applies the same min.
constexpr DWORD kGxNativeMaxVsConst = kGxCsVsConstCount;

class GxNativeDevice;
class GxNativeTexture;

// ---------------------------------------------------------------------------------------------------------- resource objects

// DXVK's d3d8 device-child protocol (d3d8_device_child.h:23-53): an atomic PUBLIC count (the game's) and an
// atomic PRIVATE count (bindings, device ownership). Public 0 -> 1 takes one private reference and AddRefs the device; public
// 1 -> 0 (compare-exchange, clamped at 0) drops it and Releases the device. The object is destroyed only when PRIVATE reaches 0:
// one counter decides, so the main thread's AddRef/Release and the d3d8 ring thread's queued Release cannot race a destruction.
// Created objects start at 0/0: the creator calls addPublic() (a game object) or addPrivate() (a device-owned one).
class GxNativeRefs
{
public:
	explicit GxNativeRefs(GxNativeDevice& dev) : m_owner(dev) {}
	virtual ~GxNativeRefs() = default;
	ULONG addPublic();
	ULONG releasePublic();
	void addPrivate() { ++m_private; }
	void releasePrivate();
	ULONG publicCount() const { return m_public.load(); }

protected:
	friend class GxNativeDevice;   // drainDestroys(): the accounting on the front, the storage as a command (A23-B §3.3)
	virtual void destroyFront() = 0;     // live counts, losable, drop candidates: front
	virtual void destroyStorage() = 0;   // the storage and the object itself (delete): the worker, or direct
	void destroy() { destroyFront(); destroyStorage(); }
	virtual bool containerOwned() const { return false; }   // a texture level: never queued, its texture owns it
	// A23-B §7.1 TEST-ONLY: retire every address the exec traces name this object by (the most-derived object, its storage, a
	// texture's level surfaces) - the base subobject alone is not one of them (rw3-X-3: a freed buffer's id stayed live and a reused
	// address aliased differently per run)
	virtual void traceRetire(GxCsExecSink&) const {}
	GxNativeDevice& m_owner;

private:
	std::atomic<ULONG> m_public { 0 };
	std::atomic<ULONG> m_private { 0 };
};

class GxNativeSurface final : public IDirect3DSurface8, public GxNativeRefs
{
public:
	enum class Kind { TextureLevel, RenderTarget, Depth, BackBuffer, Image };
	// TextureLevel: owned by its texture, references forward to it. BackBuffer: owned by the device (never destroyed by Release).
	GxNativeSurface(GxNativeDevice& dev, Kind kind, GxNativeTexture* tex, uint32_t level);
	GxNativeSurface(GxNativeDevice& dev, Kind kind, std::unique_ptr<Res::GxRenderTarget> rt, uint32_t d3dFormat, uint32_t usage);   // usage: implied by kind
	GxNativeSurface(GxNativeDevice& dev, uint32_t backBufferIndex);
	GxNativeSurface(GxNativeDevice& dev, std::unique_ptr<Res::GxTexture> image);   // CreateImageSurface (SYSTEMMEM)
	~GxNativeSurface() override;

	// IUnknown / IDirect3DSurface8
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** pp) override;
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
	HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** pp) override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* desc) override;
	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags) override;
	HRESULT STDMETHODCALLTYPE UnlockRect() override;

	Kind kind() const { return m_kind; }
	GxNativeTexture* texture() const { return m_tex; }
	uint32_t level() const { return m_level; }
	// WORKER (or direct): the image a draw/copy uses NOW (a back buffer's storage rotates with the presents); null for an image
	// surface. A23-B §3.1: never called by the front while the worker runs.
	Res::GxImage* image() const;
	Res::GxRenderTarget* renderTarget() const;   // RenderTarget / Depth / BackBuffer
	VkImageView attachmentView() const;
	// Any thread: the immutable description fixed at construction (A23-B §3.1 front desc; requested AND actual samples)
	uint32_t d3dFormat() const { return m_front.format; }
	uint32_t width() const { return m_front.width; }
	uint32_t height() const { return m_front.height; }
	uint32_t pool() const { return m_front.pool; }
	uint32_t usage() const { return m_front.usage; }
	bool multisampled() const { return m_front.multisampled; }
	uint32_t requestedSamples() const { return m_front.requestedSamples; }
	uint32_t samples() const { return m_front.samples; }   // the ACTUAL Vulkan count (copy validation, §3.1)
	// FRONT: the surface has a colour/depth image a target binding hands the translator (a back buffer: storage exists - the
	// swapchain objects change only inside S bodies)
	bool frontHasImage() const;
	Res::GxTexture* imageTexture() const { return m_image.get(); }   // Kind::Image

	void addPrivateRef();
	void releasePrivateRef();
	void testUncountLosable();   // gx_native_test's implicitNotLosable plant only
	// Reset tore the swapchain down while the caller still holds this back buffer: it keeps the storage it shows (DXVK keeps
	// the resource alive while references remain)
	void detachStorage(std::unique_ptr<Res::GxRenderTarget> rt) { m_rt = std::move(rt); }

protected:
	void destroyFront() override;
	void destroyStorage() override;
	void traceRetire(GxCsExecSink& s) const override { s.destroyed(this); }
	bool containerOwned() const override { return m_kind == Kind::TextureLevel; }

private:
	GxNativeDevice& m_dev;
	Kind m_kind;
	GxNativeTexture* m_tex = nullptr;
	uint32_t m_level = 0;
	uint32_t m_bb = 0;
	std::unique_ptr<Res::GxRenderTarget> m_rt;
	std::unique_ptr<Res::GxTexture> m_image;
	uint32_t m_format = 0;
	bool m_notLosable = false;
	struct FrontDesc
	{
		uint32_t format = 0, width = 0, height = 0, pool = 0, usage = 0, requestedSamples = 0, samples = 1;
		bool multisampled = false, hasImage = false;
	} m_front;
	void initFront();   // every constructor, last: from what the surface was made over (fixed for its life)
};

class GxNativeTexture final : public IDirect3DTexture8, public GxNativeRefs
{
public:
	GxNativeTexture(GxNativeDevice& dev, std::unique_ptr<Res::GxTexture> tex);
	~GxNativeTexture() override;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** pp) override;
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
	DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override;
	DWORD STDMETHODCALLTYPE GetPriority() override { return m_priority; }
	void STDMETHODCALLTYPE PreLoad() override {}
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
	DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override;
	DWORD STDMETHODCALLTYPE GetLOD() override { return m_lod; }
	DWORD STDMETHODCALLTYPE GetLevelCount() override { return m_tex->levels(); }
	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override;
	HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags) override;
	HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
	HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* r) override { addDirty(r, 0); return D3D_OK; }

	Res::GxTexture& gx() { return *m_tex; }
	bool managed() const { return m_tex->desc().pool == Res::d3d::POOL_MANAGED; }
	// DESIGN §6: the host-copy policy. written(level) after any write; a lock of a level written before pins the texture.
	void noteWritten(uint32_t level);
	bool pinned() const { return m_pinned; }
	void pin();
	bool dropCandidate() const;
	void addPrivateRef() { addPrivate(); }
	void releasePrivateRef() { releasePrivate(); }
	// D3D9's level-0 dirty box (d3d9_common_texture.h:405-440): full at creation, grown by writing locks and AddDirtyRect,
	// consumed and cleared by UpdateTexture (DESIGN 7.3). {left, top, right, bottom}; right == left = empty.
	void addDirty(const RECT* r, uint32_t level);
	RECT dirty() const { return m_dirty; }
	void clearDirty() { m_dirty = RECT { 0, 0, 0, 0 }; }

protected:
	void destroyFront() override;
	void destroyStorage() override;
	void traceRetire(GxCsExecSink& s) const override
	{
		s.destroyed(this);
		for (const auto& l : m_levels)   // its level surfaces die with it (SetTarget names them)
			if (l)
				s.destroyed(l.get());
	}

private:
	friend class GxNativeSurface;
	GxNativeDevice& m_dev;
	std::unique_ptr<Res::GxTexture> m_tex;
	std::vector<std::unique_ptr<GxNativeSurface>> m_levels;   // created on demand
	std::vector<uint32_t> m_writes;
	bool m_pinned = false;
	RECT m_dirty = {};
	DWORD m_priority = 0, m_lod = 0;
};

template <class I, class Desc>
class GxNativeBuffer final : public I, public GxNativeRefs
{
public:
	GxNativeBuffer(GxNativeDevice& dev, std::unique_ptr<Res::GxBuffer> buf, DWORD usage, DWORD fvfOrFormat, D3DPOOL pool);
	~GxNativeBuffer() override;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
	ULONG STDMETHODCALLTYPE AddRef() override { return addPublic(); }
	ULONG STDMETHODCALLTYPE Release() override { return releasePublic(); }
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** pp) override;
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
	DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override { const DWORD o = m_priority; m_priority = p; return o; }
	bool losable() const { return m_pool == D3DPOOL_DEFAULT; }
	DWORD STDMETHODCALLTYPE GetPriority() override { return m_priority; }
	void STDMETHODCALLTYPE PreLoad() override {}
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
	HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override;
	HRESULT STDMETHODCALLTYPE Unlock() override;
	HRESULT STDMETHODCALLTYPE GetDesc(Desc* desc) override;

	Res::GxBuffer& gx() { return *m_buf; }
	void traceRetire(GxCsExecSink& s) const override
	{
		s.destroyed(this);
		s.destroyed(m_buf.get());
	}
	void addPrivateRef() { addPrivate(); }
	void releasePrivateRef() { releasePrivate(); }

protected:
	void destroyFront() override;
	void destroyStorage() override;

private:
	GxNativeDevice& m_dev;
	std::unique_ptr<Res::GxBuffer> m_buf;
	DWORD m_usage, m_fvfOrFormat, m_priority = 0;
	D3DPOOL m_pool;
};
using GxNativeVertexBuffer = GxNativeBuffer<IDirect3DVertexBuffer8, D3DVERTEXBUFFER_DESC>;
using GxNativeIndexBuffer = GxNativeBuffer<IDirect3DIndexBuffer8, D3DINDEXBUFFER_DESC>;

// ---------------------------------------------------------------------------------------------------------- the device

class GxNativeDevice final : public IDirect3DDevice8
{
public:
	GxNativeDevice(IDirect3D8* parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, const GxNativeHost& host);
	~GxNativeDevice();
	// false + why: the session must be CLASSIC (nothing created survives).
	bool init(const D3DPRESENT_PARAMETERS& pp, std::string* why);

	// IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
	ULONG STDMETHODCALLTYPE Release() override;
	// IDirect3DDevice8
	HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
	UINT STDMETHODCALLTYPE GetAvailableTextureMem() override;
	HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** pp) override;
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* caps) override;
	HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* mode) override;
	HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override;
	HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, IDirect3DSurface8*) override { return D3D_OK; }
	void STDMETHODCALLTYPE SetCursorPosition(UINT, UINT, DWORD) override {}
	WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL show) override { const WINBOOL o = m_cursorShown; m_cursorShown = show; return o; }
	HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8**) override;
	HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override;
	HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) override;
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS*) override;
	void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) override;
	void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP* ramp) override;
	HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
		IDirect3DTexture8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, WINBOOL lockable,
		IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8* src, const RECT* srcRects, UINT count, IDirect3DSurface8* dst,
		const POINT* dstPoints) override;
	HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8* src, IDirect3DBaseTexture8* dst) override;
	HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8*) override;
	HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8* rt, IDirect3DSurface8* ds) override;
	HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** pp) override;
	HRESULT STDMETHODCALLTYPE BeginScene() override;
	HRESULT STDMETHODCALLTYPE EndScene() override;
	HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
	HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) override;
	HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* m) override;
	HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) override;
	HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8* vp) override;
	HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8* vp) override;
	HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8* m) override;
	HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8* m) override;
	HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT8* l) override;
	HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3DLIGHT8* l) override;
	HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, WINBOOL on) override;
	HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, WINBOOL* on) override;
	HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float* plane) override;
	HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD index, float* plane) override;
	HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override;
	HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) override;
	HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return neverCalled("BeginStateBlock"); }
	HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD*) override { return neverCalled("EndStateBlock"); }
	HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD) override { return neverCalled("ApplyStateBlock"); }
	HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD) override { return neverCalled("CaptureStateBlock"); }
	HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD) override { return neverCalled("DeleteStateBlock"); }
	HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE, DWORD*) override { return neverCalled("CreateStateBlock"); }
	HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8*) override { return neverCalled("SetClipStatus"); }
	HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8*) override { return neverCalled("GetClipStatus"); }
	HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture8** pp) override;
	HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8* tex) override;
	HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
	HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
	HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override;
	HRESULT STDMETHODCALLTYPE GetInfo(DWORD, void*, DWORD) override { return neverCalled("GetInfo"); }
	HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY*) override { return neverCalled("SetPaletteEntries"); }
	HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override { return neverCalled("GetPaletteEntries"); }
	HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override { return neverCalled("SetCurrentTexturePalette"); }
	HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT*) override { return neverCalled("GetCurrentTexturePalette"); }
	HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) override;
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT startIndex,
		UINT count) override;
	HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override;
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count,
		const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride) override;
	HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) override { return neverCalled("ProcessVertices"); }
	HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* decl, const DWORD* code, DWORD* handle, DWORD usage) override;
	HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override;
	HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) override;
	HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) override;
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) override;
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD, void*, DWORD) override { return neverCalled("GetVertexShaderConstant"); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD, void*, DWORD*) override { return neverCalled("GetVertexShaderDeclaration"); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD, void*, DWORD*) override { return neverCalled("GetVertexShaderFunction"); }
	HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, IDirect3DVertexBuffer8* vb, UINT stride) override;
	HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, IDirect3DVertexBuffer8** pp, UINT* stride) override;
	HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* ib, UINT baseVertex) override;
	HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** pp, UINT* baseVertex) override;
	HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* code, DWORD* handle) override;
	HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) override;
	HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) override;
	HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) override;
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) override;
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD, void*, DWORD) override { return neverCalled("GetPixelShaderConstant"); }
	HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD, void*, DWORD*) override { return neverCalled("GetPixelShaderFunction"); }
	HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override { return neverCalled("DrawRectPatch"); }
	HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override { return neverCalled("DrawTriPatch"); }
	HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return neverCalled("DeletePatch"); }

	// --- for the resource objects, L2 (DESIGN §11) and tools
	// A23-B §3.1: inspection accessors are S points - the worker is drained first and the reference is valid only until the
	// caller's next call into the device. Product code inside the device uses frontStats() / dbg(), which never drain.
	GxNativeStats& stats()
	{
		csSync("accessor stats");
		m_stats.resourceErrors = m_resourceErrors.load(std::memory_order_relaxed);
		return m_stats;
	}
	GxNativeStats& frontStats() { return m_stats; }
	const GxNativeDebug& dbg() const { return m_debug; }
	// DESIGN §2.3: "[GXNATIVE] stats final ... end=<teardown|exit>", once per session (a refused session prints none)
	void logFinalStats(const char* end);
	void logGpu(uint64_t presents);
	void writeGpuDump();   // S7 A92 TEST-ONLY: m_gpuWin + m_gpuMissLogic into GX_GPUTIME_DUMP
	void drainGpuDump();   // S7 A92 TEST-ONLY: the dump's last batch at teardown / exit   // presents: the Present count the line is labelled with (the worker: its command's capture)
	void waitTimed(uint64_t value, uint64_t* us);   // the graphics timeline wait, timed into *us when us is set (A30 [GXWAIT])
	GxNativeDebug& debug() { csSync("accessor debug"); return m_debug; }
	GxNativeValidation validation();
	Res::GxResources& resources() { csSync("accessor resources"); return m_gx; }
	// rw3-X-3 (A23-B X'): the format table, immutable after GxResources::init (device creation, before the worker starts) - a
	// front read that NEVER drains (GetDesc / GetLevelDesc used resources() and drained the worker on every call)
	const Res::FormatTable& formats() const { return m_gx.formats(); }
	Rec::GxPacketWriter& writer() { csSync("accessor writer"); return m_writer; }
	uint32_t framesInFlight() const { return m_fif; }   // A30: the resolved GX_NATIVE_FIF (2 or 3) - set in initVulkan before the
	                                                   // worker starts and never written again: a front read that never drains
	// TESTS (A30): the swapchain's frame slots (0 offscreen) and the slot the next Present uses - the worker's Present advances
	// m_presentSlot, so both are inspection accessors (A23-B §3.1: S)
	size_t presentSlots() { csSync("accessor presentSlots"); return m_presentWaits.size(); }
	uint32_t presentSlot() { csSync("accessor presentSlot"); return m_presentSlot; }
	const Rec::GxD3D8Translate& translator() { csSync("accessor translator"); return m_tr; }
	Rec::GxTranslateDebug& translateDebug() { csSync("accessor translateDebug"); return m_tr.debug(); }   // TEST/TOOL plants only (fail arms)
	Vk::VkCtx& ctx() { csSync("accessor ctx"); return m_ctx; }
	Vk::VkSwapchain& swapchain() { csSync("accessor swapchain"); return m_swapchain; }   // TEST: the submit thread's arms read the deferred present status
	// A23-B: the worker is running (GX_NATIVE_CS=1 and it started); the stream itself (TEST hooks: hold, ring stats)
	bool csOn() const { return m_csOn; }
	uint64_t csSyncCount() const { return m_csSyncs; }   // S transitions that drained (front)
	uint64_t csCreateRetries() const { return m_csCreateRetries; }
	size_t testSpecWorkers() const { return m_cache.testSpecWorkers(); }   // TEST-ONLY
	bool mirrorCheckOn() const { return m_mirrorCheck; }
	uint64_t mirrorMismatches() const { return m_mirrorMismatches; }   // GX_NATIVE_CS_MIRRORCHECK=1: fields found different, total
	uint64_t csOwnerViolations() const { return m_ownerViolations.load(); }   // worker state touched by the front outside an S body
	uint64_t csWsiOnWorker() const { return m_wsiOnWorker.load(); }          // surface / swapchain (re)creation run by the worker
	uint64_t allocFailures() const { return m_allocFailures.load(); }
	uint64_t recoveries() const { return m_recoveries.load(); }
	uint64_t recoveriesAt(const char* site)
	{
		std::lock_guard<std::mutex> g(m_recoverLock);
		auto it = m_recoverBySite.find(site);
		return it == m_recoverBySite.end() ? 0 : it->second;
	}
	// TEST (arm 28): every value recoverStorage waits for, with lastSubmitted when it waited
	void (*testRecoverWait)(void* user, uint64_t value, uint64_t lastSubmitted) = nullptr;
	void* testRecoverWaitUser = nullptr;
	GxNativeCs& csStream() { return m_cs; }
	// Drain the worker (no-op without one, or on the worker itself): what an inspection accessor needs.
	void csSync(const char* why = "test sync");   // a front drain = an S: counted under `why` with its drain time (X')
	// Drain and end the worker for good; the device runs direct from now (teardown, the exit flush - §3.9).
	void csPark();
	void noteExitQuiesce() { m_csExitDraining.store(true, std::memory_order_release); }   // A60: the CRT-exit quiesce started
	bool testExitDraining() const { return m_csExitDraining.load(std::memory_order_acquire); }   // TEST: ... (the arms' release signal)
	static constexpr int kExitStallMs = 10000;   // A60: the CRT-exit watchdog's no-progress limit
	static constexpr int kExitTerminalCode = 3;  // A60: its process exit code (VkCtx's terminal path uses 3 too)
	void settleAtExit();   // §3.9 exit: lever 3 settled as teardown does (drain gx-vk-submit, the last present's status applied)
	void testMarkSurfaceLost() { m_testSurfaceLost = true; }   // TEST: the surface behaves as lost until a new one is made
	const uint8_t* deviceUuid() const { return m_uuid; }   // the physical device's deviceUUID (valid after init)
	// TESTS: the swapchain image the last Present showed (its own format's rows, B8G8R8A8 on every surface met so far), captured
	// when debug().capturePresent was set before that Present; false when it was not captured (no readback usage, no capture)
	bool testReadPresent(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height);
	// TESTS (FG P0 H3): the swapchain has at least one image view
	bool testSwapchainViewsMade() const
	{
		for (uint32_t i = 0; i < m_swapchain.imageCount(); ++i)
			if (m_swapchain.view(i))
				return true;
		return false;
	}
	// TEST: the finished swapchain image of the last SHOWN present (debug().testSwapchainReadback set before it); drains and idles
	bool testReadSwapchain(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height, bool* bgra);
	// TESTS (W-a): the window-sized present target exists (the worker's Present makes / keeps it: S)
	bool testHasPresentTarget() { csSync("accessor testHasPresentTarget"); return m_presentImg != nullptr; }
	// The swapchain's ACTUAL extent (Vulkan may clamp the drawable size); false offscreen / without a swapchain (DESIGN S9)
	bool swapchainExtent(uint32_t* w, uint32_t* h) const
	{
		if (!m_haveSwapchain)
			return false;
		*w = m_swapchain.extent().width;
		*h = m_swapchain.extent().height;
		return true;
	}
	// the present mode VkSwapchain actually chose (VkPresentModeKHR), -1 without a swapchain
	int swapchainPresentMode() const { return m_haveSwapchain ? int(m_swapchain.presentMode()) : -1; }
	// Ends the writer frame if one is open: what a synchronous GXRES read-back or restore needs (GxPacketWriter header).
	void endFrameForHost();
	// Staging that only a submission can free (queued uploads, dropped ones' staging) rides on a frame of its own once it passes a
	// threshold with no frame open (load time; GxResources::stagingPressure).
	void flushIdleUploads();
	void forgetImage(const Res::GxImage* img);
	// DESIGN §9: the last private release may come from ANY thread (SurfaceClass releases directly while
	// the d3d8 ring's render thread draws), so it only queues the object; the device destroys the queue on the thread inside
	// the device - at Present, Reset, every Create* and teardown. Destruction touches the writer and GXRES, which one thread owns.
	void deferDestroy(GxNativeRefs* r);
	void drainDestroys();
	void noteManaged(GxNativeTexture* t);   // a MANAGED texture became a drop candidate (DESIGN §6)
	// A23-B §3.6: the lock / unlock paths of the resource objects (front halves; exec halves as commands, or direct)
	void* lockBuffer(Res::GxBuffer& b, uint32_t offset, uint32_t bytes, uint32_t flags);
	void* lockBufferInline(Res::GxBuffer& b, uint32_t offset, uint32_t bytes, uint32_t flags);
	void unlockBuffer(Res::GxBuffer& b);
	void unlockTextureLevel(GxNativeTexture* t, uint32_t level);
	void setTextureLod(GxNativeTexture* t, uint32_t lod);
	// The IDLE-FLUSH rule (§3.6): an F unlock's flushIdleUploads at its call position - a FlushIdle command whenever the front
	// cannot prove it a no-op (the published staging pressure + the staging of every upload command queued after it > 32 MiB).
	void flushIdleFront();
	void unnoteManaged(GxNativeTexture* t);
	Res::GxRenderTarget* backBufferStorage(uint32_t index) const;   // worker (or direct): reads the rotation
	size_t backBufferCount() const { return m_bbStore.size(); }   // any: changes only inside S bodies
	// (rev 2): the world pass. colour/depth = the bound world targets (GxNativeSurface).
	void worldBegin(const GxMotionBegin& b, IDirect3DSurface8* colour, IDirect3DSurface8* depth);
	void worldEnd();
	void worldJitter(float ndc[2]) const { m_motion.jitterNdc(ndc); }
	HRESULT worldObjectVelocity(IDirect3DSurface8* o);   // IGxWorldPass::SetObjectVelocity (render-motion-units-1's target; null cancels)
	// IGxWorldPass::NextTargetsAreWorld: under GXWP_TEMPORAL the NEXT depth-stencil surface created gets SAMPLED + a DEPTH-aspect
	// view (one-shot: only the world depth). Without it every depth image is created exactly as before.
	void worldNextTargets(uint32_t flags) { m_nextDepthSampled = (flags & GXWP_TEMPORAL) != 0; }
	// A temporal upscaler backend that will consume the jitter (GxMotion::setJitterConsumer); call before the first world pass.
	void setTemporalConsumer(bool on) { m_motion.setJitterConsumer(on); }
	// The upscalers' inputs of the last world pass; null while motion never ran (GxMotionFrame.h).
	const GxMotionFrame* motionFrame() const { return m_motionInit && m_motion.isPublished() ? &m_motion.frame() : nullptr; }
	// (GxNativeDlss.cpp): IGxWorldPass::Supports / Upscale for GX_UPSCALE_DLSS. FALSE / E_NOTIMPL without
	// Streamline (the seam then draws bilinear).
	bool dlssSupported() const;
	static bool streamlineBuilt();   // TESTS: gx_device compiled with GX_STREAMLINE
	HRESULT dlssUpscale(const GxUpscaleCall& c);
	// (F-depth+, GxSceneCopy.h): IGxSceneTap's bodies. Front side; OpaqueDone's work is one async command.
	HRESULT sceneSetReader(const char* name, uint32_t needs);
	bool sceneWanted() const { return m_sceneInit && m_scene.readers().needs() != 0; }
	HRESULT sceneOpaqueDone();
	// FRONT: something will sample the world depth after this point of the frame - motion-1's End-of-pass resolve, or an F-depth+
	// depth reader (whose mid-frame copy needs the depth stored at its split). Lever 2b (W-b, depth DONT_CARE at the last scope)
	// must keep the depth while this is true (A20s note 2; the integrator 17/10: W-b wires it, whichever lands second).
	bool depthReadAfterPass() const { return m_motionInit || (m_scene.readers().needs() & GX_SCENE_DEPTH) != 0; }
	// TESTS: F-depth+'s state (probe texel / timing, frame(), stats()) and the probe's last value (depth, r, g, b)
	GxSceneCopy& testScene() { return m_scene; }
	bool testReadSceneProbe(float out[4]);
	void logSceneStats();   // the periodic [GXFDEPTH] line (worker or direct), only once a tap ran
	// TESTS: the last world pass's motion vectors (RG16F rows, 4 bytes a pixel); false while motion never ran
	bool testReadMotion(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height);
	void logLine(const char* fmt, ...);
	HRESULT neverCalled(const char* what);
	void refuseCreate(const char* what);
	void refusedDraw();
	void logStats(const char* when, const char* end = nullptr);
	static constexpr uint64_t kStatsEveryPresents = 120;   // DESIGN §2.3

	// A23-B §3.10: an S point. Drains the worker (cs_syncs counts it by reason), runs the body inline (every entry point it reaches
	// runs its exec half directly), and on the outermost exit reconciles what the worker keeps its own copy of before production
	// goes back to it: the getter mirror, the worker's target binding.
	class SScope
	{
	public:
		SScope(GxNativeDevice& d, const char* why);
		~SScope();
		SScope(const SScope&) = delete;
		SScope& operator=(const SScope&) = delete;

	private:
		GxNativeDevice& m_d;
	};

private:
	struct Shader
	{
		bool live = false;
		bool pixel = false;
	};
	// A23-B §3.4: the front's copy of exactly the d3d8 state the getters return, written with the translator setters' own stores
	struct Mirror
	{
		float world[16], view[16], projection[16], texture[8][16];
		uint32_t rs[256];
		uint32_t tss[8][32];
		GxD3D8Material material;
		GxD3D8Viewport viewport;
	};
	void reseedMirror();
	void mirrorCheck(const char* where);   // §3.4 mirror-check mode: the mirror vs m_tr.state(), the worker drained
	void recoverStorage(bool producer, const char* site);   // §3.8 (a)-(d) by context
	void publishPressure();                      // WORKER: (stagingPressure, this command's queue seq) for flushIdleFront
	void noteStagingCmd(uint64_t bytes);         // FRONT: an upload command just pushed stages `bytes`
	void pruneStaged(uint64_t publishedSeq);     // FRONT: forget the upload commands the worker published past
	void resetPressureBound();                   // FRONT, worker idle: the exact value, nothing queued
	bool csDirect() const { return !m_csOn || m_inline > 0; }
	// §7.1 TEST-ONLY: the EXEC digest point at the top of every exec half (worker, inline or inside an S body)
	static void csTrace(GxCsOp op, std::initializer_list<GxCsArg> a, const GxCsBytes* b = nullptr)
	{
		if (GxCsExecSink* s = g_csExecSink.load(std::memory_order_relaxed))
			s->exec(op, a.begin(), a.size(), b);
	}
	static GxCsBytes csFlat(const void* p, size_t n) { GxCsBytes b; b.p = p; b.rowBytes = n; b.rows = 1; b.pitch = n; return b; }
	// The exec half: now (direct) or as a command, in call order. f captures its arguments BY VALUE.
	template <class F>
	void cs(F&& f)
	{
		++m_csIssued;
		if (csDirect())
			f();
		else
			m_cs.push(std::forward<F>(f));
		csAfter();
	}
	void csAfter()
	{
		if (m_debug.csSerial)
			csSerialPoint();
	}
	void csSerialPoint();
	// The same with `bytes` of caller memory the exec half reads (f(const uint8_t*, size_t)): direct = the caller's own bytes,
	// queued = a copy taken now (DESIGN §3.2: exactly the bytes today's body reads).
	template <class F>
	void csCopy(F&& f, const void* src, size_t bytes)
	{
		++m_csIssued;
		if (csDirect())
			f(static_cast<const uint8_t*>(src), bytes);
		else
			m_cs.push(std::forward<F>(f), src, bytes);
		csAfter();
	}
	bool frontColourHasImage() const;   // the colour target bound on the FRONT has an image (Clear reads its rects only then)
	void noteSync(const char* why, uint64_t ns);
	void syncTopInto(char* out, size_t cap, bool byNs) const;   // the top 3 S reasons by count (cs_sync_top) or drain ns (cs_drain_top)
	void logSeamDrains(uint64_t presents);   // [GXSEAM] cs drains - the seam S reasons since the last line, if any
	void logMotionCounters(uint64_t presents);   // [GXMOTION] device - m_motionCount, while motion is initialised
	void csFlushReorder();   // the csReorder plant only
	void csDrawn();          // after a draw: the csReorder plant, then publish the stream
	void csLedger(uint64_t position, uint32_t kind, std::initializer_list<GxCsArg> a);   // §7.1 TEST-ONLY: the creation ledger
	bool initVulkan(std::string* why);
	static void vkLog(Vk::LogLevel level, const char* line, void* user);
	bool createSwapchainObjects(const D3DPRESENT_PARAMETERS& pp, std::string* why);
	void destroySwapchainObjects();
	void ensureFrame();
	void syncTarget(bool bindViewport = false);
	void bindSurface(GxNativeSurface*& slot, GxNativeSurface* s);
	template <class T> void bindPrivate(T*& slot, T* obj);
	HRESULT copyRect(GxNativeSurface* src, const RECT& sr, GxNativeSurface* dst, const RECT& dr, bool asymmetric, bool stretch);
	// A23-B §3.7 exec halves (worker, or direct)
	void execCopyGpu(GxNativeSurface* src, GxNativeSurface* dst, const VkRect2D& r, const RECT& dr, bool msaa);
	// today's twin sequence: -1 = the twin could not be made (today returns before noteWritten), 0 = copyFrom refused, 1 = done
	int execCopyHostToDefault(GxNativeSurface* dst, uint32_t fmt, const VkRect2D& r, const RECT& dr, const uint8_t* bytes);
	HRESULT copyHostToDefaultInline(GxNativeSurface* dst, uint32_t fmt, const VkRect2D& r, const RECT& dr, const uint8_t* bytes);
	bool writeHost(GxNativeSurface* dst, const RECT& dr, const std::vector<uint8_t>& bytes);
	void resetState(const D3DPRESENT_PARAMETERS& pp);
	bool readSurface(GxNativeSurface* src, const VkRect2D& r, std::vector<uint8_t>* out);
	void dropManaged();
	void rotateBackBuffers();
	// w, h: the drawable size the CALLER read (A23-B §3.9 step 3); on the worker the execution-time guard skips the frame instead
	// of recreating (step 5) - recovery is the caller's next Present, as S
	HRESULT presentToSwapchain(uint32_t w, uint32_t h);
	HRESULT presentDirect();      // today's Present body after the NotReset check (direct, or the S recovery)
	void endOffscreenFrame();
	// the front's counters as of the Present that carries them (the periodic line's front half, §3.1 GxNativeStats split)
	struct FrontCounters
	{
		GxNativeStats s;
		GxNativeCsStats cs;
		uint64_t syncs = 0, createRetries = 0, frameBytesMax = 0;
		char syncTop[96] = {}, drainTop[96] = {};
	};
	void execPresent(uint32_t w, uint32_t h, const FrontCounters& f);
	FrontCounters frontCounters();
	void logStatsLine(const char* when, const char* end, const FrontCounters* f);
	void applyPresentStatus(Vk::VkSwapchain::Status st);   // today's post-present handling (rebuild flags, failure count)
	bool surfaceIsLost() const;
	bool lostTerminal();          // DESIGN §8.6: true = Present answers DEVICELOST (FATAL logged once, open frame ended)   // the window surface answers VK_ERROR_SURFACE_LOST_KHR (or a test injection says so)
	bool replaceSurface(std::string* why);
	bool buildOnSurface(uint32_t w, uint32_t h, bool fifo, uint32_t frames, std::string* why);
	bool ensurePresentTarget(const Res::GxImage& src, bool* shader, bool* direct, std::string* why);
	bool ensureCapture();   // TESTS (debug().capturePresent): the host buffer a present copies the swapchain image into

	std::atomic<ULONG> m_ref { 1 };
	IDirect3D8* m_parent;
	UINT m_adapter;
	D3DDEVTYPE m_type;
	HWND m_focus;
	DWORD m_behavior;
	GxNativeHost m_host;
	D3DPRESENT_PARAMETERS m_pp = {};
	GxNativeStats m_stats;
	GxNativeDebug m_debug;

	Vk::VkCtx m_ctx;
	static constexpr uint32_t kGpuSets = 8;   // GX_GPUTIME query sets: a frame is read 8 frames after it ran (MoltenVK resolves late)
	Vk::VkGpuTimer m_gpuTimer;
	bool m_gpuOn = false;
	uint32_t m_fif = 2;   // frames in flight, resolved once from m_host.framesInFlight (A30)
	uint64_t m_presentWaitUs = 0;   // GX_NATIVE_WAITS: host time presentToSwapchain spent in its present-slot wait
	std::vector<Rec::GxPacketWriter::GpuSample> m_gpuWin;
	std::vector<float> m_gpuMs;
	static constexpr uint32_t kGpuScopes = 64;   // per frame: the frame + its render passes + the present (A17)
	uint64_t m_gpuPrev[7] = {};   // the writer's frames, scopes, splitUpload, splitSample, splitOutside, splitAttachments, handoffs
	VkSurfaceKHR m_surface = VK_NULL_HANDLE;
	Vk::VkSwapchain m_swapchain;
	bool m_haveSwapchain = false;
	Res::GxResources m_gx;
	GxPipelineCache m_cache;
	GxGenericShaders m_ffShaders;
	GxCustomShaders m_csShaders;
	Rec::GxPacketWriter m_writer;
	Rec::GxD3D8Translate m_tr;
	bool m_inScene = false;
	bool m_swvp = false;          // DXVK m_isSWVP: SOFTWARE_VERTEXPROCESSING, or MIXED + D3DRS_SOFTWAREVERTEXPROCESSING
	bool m_notReset = false;      // a Reset failed (DXVK D3D9DeviceLostState::NotReset)

	std::vector<std::unique_ptr<Res::GxRenderTarget>> m_bbStore;
	std::vector<GxNativeSurface*> m_bbSurfaces;   // standalone device children, held privately
	uint32_t m_rot = 0;
	GxNativePresent m_present;                          // DXVK's present shaders (GxNativePresent.h)
	struct Dlss;                                        // (GxNativeDlss.cpp); null without Streamline
	Dlss* m_dlss = nullptr;
	PFN_vkGetInstanceProcAddr dlssLoaderRoot() const;   // initVulkan's VkCtx root: m_host's, or Streamline::loaderRoot()
	// FG P0 H3: DLSS-G is loaded (or TESTS say so): every swapchain is a Streamline proxy whose images' usage is unknowable
	bool swapchainInterposed() const;
	bool m_interposedLogged = false;
	void dlssInit();                                    // end of initVulkan
	void dlssShutdown();                                // destructor: before GxResources and the device go (slShutdown first)
	// (GxNativeDlss.cpp): the per-frame contract - one Streamline frame per presented frame.
	// No-ops without Streamline. slSceneBegin: the frame's first BeginScene (simulation ends, rendering starts); slPresentBegin: before
	// presentToSwapchain (the HUD-less tag, eRenderSubmitEnd); slPresentHook: the present hook (FG P0 H3) - ePresentStart / ePresentEnd
	// around vkQueuePresentKHR alone, on the presenting thread, for this frame's token (false: no hook); slFrameEnd: after the present is
	// counted - DLSS-G state, then the next frame begins.
	void slSceneBegin();
	void fgHudBegin();      // (PREREG A39c, TEST-ONLY GX_FG_HUDLESS=1): WorldPass::HudBegin - the scene without HUD, copied
	void fgHudlessTag();    // slPresentBegin: that copy resolved like the present and tagged as DLSS-G's HUD-less colour
	void slPresentBegin();
	bool slPresentHook(Vk::PresentHook* out);
	// FG P0 H3 (F8): around presentToSwapchain's swapchain recreation - frame generation off before it, back on (with its own
	// recreation) after kFgStableFrames presented frames with no recreation. No-ops without Streamline / frame generation.
	void slBeforeRecreate();
	void slAfterRecreate();
	static constexpr uint32_t kFgStableFrames = 30;
	void slFrameEnd();
	GxMotion m_motion;                                  // depth / motion vectors / jitter (GxMotion.h); initialised at the first
	bool m_motionInit = false;                          // world pass whose depth is sampleable, never with the key off
	bool m_motionOffLogged = false;
	bool m_nextDepthSampled = false;                    // worldNextTargets: the next CreateDepthStencilSurface only
	GxNativeSurface* m_worldColour = nullptr;           // the world pass's targets, held (private refs) Begin..End
	GxNativeSurface* m_worldDepth = nullptr;            // ... and kept until the next Begin: the published frame names it
	GxNativeSurface* m_worldObject = nullptr;           // SetObjectVelocity's target, held until End records from it
	GxSceneCopy m_scene;                                // F-depth+ (front registry + exec copies)
	bool m_sceneInit = false;
	bool m_sceneTapped = false;                         // FRONT: OpaqueDone ran in this frame (cleared at Present)
	void worldRelease(bool depthToo);                   // drops those refs (End: colour + object; Begin/Reset/teardown: all)
	void worldAbandon();                                // an open pass dies (Reset, a second Begin): clean projection back
	bool m_presentInit = false;
	// A23-B §3.1: written by the worker's present path, read by the front's Present / TestCooperativeLevel - atomics
	std::atomic<bool> m_swapchainDirty { false };   // a Suboptimal acquire / present: recreate before the next acquire
	bool m_recreateDead = false;     // the noRecreateRetry plant only
	std::atomic<bool> m_surfaceLost { false };      // VK_ERROR_SURFACE_LOST_KHR seen: a new VkSurfaceKHR before the next acquire
	std::atomic<bool> m_testSurfaceLost { false };  // TEST fault injection: the current surface behaves as lost until a new one is made
	std::atomic<bool> m_presentLost { false };      // A23-B: a present answered DeviceLost on the worker (the front answers DEVICELOST)
	bool m_finalLogged = false;       // the final stats line was printed (once per session)
	std::atomic<bool> m_lostLogged { false };       // DESIGN §8.6: the FATAL line was printed (once per session)
	bool m_initOk = false;            // init completed: a device the session refused prints no stats line
	bool m_orderCheckInstalled = false;   // this device installed GxOrderCheck: it uninstalls before its VkDevice goes
	uint32_t m_swapReqW = 0, m_swapReqH = 0;   // the size the swapchain was last built at (requested; Vulkan may clamp the extent)
	std::atomic<bool> m_presentDead { false };      // a frame's submission failed after its acquire: Present answers DEVICELOST
	std::unique_ptr<Res::GxRenderTarget> m_presentImg;   // their window-sized target, copied 1:1 into the swapchain image
	VkBuffer m_capBuf = VK_NULL_HANDLE;   // TESTS: testReadPresent's copy of the last presented swapchain image
	VmaAllocation m_capAlloc = nullptr;
	void* m_capPtr = nullptr;
	VkDeviceSize m_capSize = 0;
	uint32_t m_capW = 0, m_capH = 0;   // the extent the last capture holds (0: none)
	GxNativeSurface* m_autoDepth = nullptr;              // a device-owned surface (public ref held by the device)
	GxNativeSurface* m_rt = nullptr;
	GxNativeSurface* m_ds = nullptr;
	IDirect3DBaseTexture8* m_textures[8] = {};
	GxNativeVertexBuffer* m_streams[16] = {};
	UINT m_strides[16] = {};
	GxNativeIndexBuffer* m_indices = nullptr;
	UINT m_baseVertex = 0;
	DWORD m_vs = 0, m_ps = 0;
	std::vector<Shader> m_shaders;   // index = DXVK handle encoding (d3d8_device.cpp:11-21)
	D3DGAMMARAMP m_gamma = {};
	WINBOOL m_cursorShown = FALSE;
	std::vector<GxNativeTexture*> m_dropCandidates;
	std::vector<uint64_t> m_presentWaits;   // per acquire-semaphore slot: the submission that waited on it
	uint32_t m_presentSlot = 0;
	uint8_t m_uuid[VK_UUID_SIZE] = {};
	std::mutex m_deadLock;
	std::vector<GxNativeRefs*> m_dead;   // released, not yet destroyed (deferDestroy)

	// GeneralsX: the world/HUD seam's interface (GxWorldPass.h), answered by
	// QueryInterface(IID_GxWorldPass); its reference count is the device's. Bodies in GxNativeWorldPass.cpp: each stream adds its
	// work as one call there (render-motion-1 Begin/End, the upscaler backends Supports/Upscale). Front-end contract: GxWorldPass.h.
	struct WorldPass final : public IGxWorldPass
	{
		GxNativeDevice* dev = nullptr;
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override { return dev->QueryInterface(riid, pp); }
		ULONG STDMETHODCALLTYPE AddRef() override { return dev->AddRef(); }
		ULONG STDMETHODCALLTYPE Release() override { return dev->Release(); }
		HRESULT STDMETHODCALLTYPE NextTargetsAreWorld(uint32_t flags) override;
		HRESULT STDMETHODCALLTYPE Begin(const GxWorldPassBegin& b) override;
		HRESULT STDMETHODCALLTYPE End() override;
		HRESULT STDMETHODCALLTYPE ResolveLinear(IDirect3DSurface8* msaaSrc, IDirect3DSurface8* dst) override;
		BOOL STDMETHODCALLTYPE Supports(uint32_t kind) override;
		HRESULT STDMETHODCALLTYPE Upscale(const GxUpscaleCall& c) override;
		HRESULT STDMETHODCALLTYPE HudBegin() override;
		HRESULT STDMETHODCALLTYPE SetObjectVelocity(IDirect3DSurface8* o) override;
		HRESULT STDMETHODCALLTYPE GetJitter(float ndc[2]) override;
		BOOL STDMETHODCALLTYPE SharpensItself(uint32_t kind) override;
		bool fsr1Ready();                          // FSR 1's pipelines and sets, made on first use (declined for the session on failure)
		HRESULT sharpen(const GxUpscaleCall& c);   // Upscale(GX_UPSCALE_RCAS)
	};
	WorldPass m_worldPass;
	// GeneralsX: F-depth+'s interface (GxSceneTap.h), answered by QueryInterface(IID_GxSceneTap)
	// whatever the seam does; its reference count is the device's.
	struct SceneTap final : public IGxSceneTap
	{
		GxNativeDevice* dev = nullptr;
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override { return dev->QueryInterface(riid, pp); }
		ULONG STDMETHODCALLTYPE AddRef() override { return dev->AddRef(); }
		ULONG STDMETHODCALLTYPE Release() override { return dev->Release(); }
		HRESULT STDMETHODCALLTYPE SetReader(const char* name, uint32_t needs) override { return dev->sceneSetReader(name, needs); }
		BOOL STDMETHODCALLTYPE Wanted() override { return dev->sceneWanted() ? TRUE : FALSE; }
		HRESULT STDMETHODCALLTYPE OpaqueDone() override { return dev->sceneOpaqueDone(); }
	};
	SceneTap m_sceneTap;
	// S7: the shadow-map interface (QueryInterface(IID_GxShadowMap), key on only), its atlas and caster list
	struct ShadowMap final : public IGxShadowMap
	{
		GxNativeDevice* dev = nullptr;
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override { return dev->QueryInterface(riid, pp); }
		ULONG STDMETHODCALLTYPE AddRef() override { return dev->AddRef(); }
		ULONG STDMETHODCALLTYPE Release() override { return dev->Release(); }
		HRESULT STDMETHODCALLTYPE BeginShadowFrame(const GxShadowFrame& f) override;
		HRESULT STDMETHODCALLTYPE SetShadowTags(uint32_t tags) override;
		HRESULT STDMETHODCALLTYPE SetOwnLight(uint64_t packed) override;
	};
	ShadowMap m_shadowMapIf;
	bool m_shadowMapOn = false;
	bool m_shadowLastOnWorker = false;   // TESTS: the last IGxShadowMap command ran in the worker's exec half
	Rec::GxShadowCasters m_casters;
	Res::GxImage m_atlas;
	VkImageView m_atlasView = VK_NULL_HANDLE;   // DEPTH aspect: the pass's attachment view and (P2) the receivers' sampled view
	Res::GxImage m_smScratch;                    // S7 A92 TEST-ONLY replay-k: copies 2..k render here (k > 1 only; never sampled)
	VkImageView m_smScratchView = VK_NULL_HANDLE;
	FILE* m_gpuDump = nullptr;                   // S7 A92 TEST-ONLY GX_GPUTIME_DUMP: one line per GPU sample (logGpu's drain)
	uint64_t m_gpuBackPrev = 0;                  // S7 A92: the timer's backwards-pair total at the last [GXGPU] line
	bool m_gpuDumpDrained = false;               // S7 A92: the dump's terminal drain ran (exactly once per device)
	std::vector<uint32_t> m_gpuMissLogic;        // S7 A92: logGpu's drain of the missing frames' logic frames (the dump)
	bool shadowMapCreate(std::string* why);   // the atlas, its view, the caster list: BEFORE the writer (P2: set 2 needs the view)
	void shadowMapAttach();                   // after the writer: set 2's atlas, the caster list, the interface on
	void shadowMapShutdown();
	void shadowMapReport(uint64_t presents);   // the periodic [GXSHADOWMAP] line + the TEST-ONLY atlas dump (GX_SHADOWMAP_DUMP_EVERY)
	Rec::GxCasterStats m_smPrev;
	uint32_t m_smDumpEvery = 0, m_smDumps = 0;
	uint32_t m_lookFeatures = 0;   // S7 P2: the effective look mask (the host's, bit 0 cleared when the atlas could not be made)
	uint32_t m_smFilter = 0;       // GX_SHADOWMAP_FILTER=hard (TEST-ONLY, the cost table): one compare tap
	uint64_t m_smRcvPrev[7] = {};  // the translator's receiver class counts (+ A44 castOnlyDropped, A61 castOnlySolid) at the last periodic line
	// (A108): alpha-to-coverage for the session = look bit 1 AND a multisampled creation (init's present parameters); then the
	// a2c module family is loaded, the translator decides per draw and the cache prebuilds A2C twins. Off = today's device exactly.
	uint32_t m_createSamples = 1;  // init(pp)'s MultiSampleType (>= 1), read by initVulkan's family choice
	uint32_t m_a2cPrewarmedSamples = 0;   // the MSAA the generic set + its A2C twins were last prebuilt for
	bool m_a2cOn = false;
	uint32_t m_a2cOffCount = 0;   // times A2C turned itself off (a failed prewarm or module; at most once per session in practice)
	void a2cOff(const char* reason, const char* at);   // A2C off for the rest of the session (init or a Reset's S body)
	void a2cReport(uint64_t presents);   // the periodic [GXA2C] line: the translator's draws on / off by reason since the last one
	uint64_t m_a2cPrev[1 + GXRender::GxA2C_WhyCount] = {};
	uint32_t m_a2cFirstUsePrev = 0;   // the cache's a2cFirstUseFailed at the last periodic line
public:
	// TEST-ONLY (gx_native_test sm-*): the S7 state as the device holds it
	bool testShadowMapOn() const { return m_shadowMapOn; }
	const Rec::GxShadowCasters& testCasters() const { return m_casters; }
	const Res::GxImage& testAtlas() const { return m_atlas; }
	// S7 A92 TEST-ONLY (GX_GPUTIME_DUMP; the integrator's P4 ruling (4)): the logic frame the game draws, queued on the cs stream like
	// SetShadowTags and stored with the writer frame it lands in - each per-frame sample of the dump then names its game frame
	void testSetLogicFrame(uint32_t frame);
	const Res::GxImage& testSmScratch() const { return m_smScratch; }   // S7 A92: replay-k's scratch image (null image when k = 1)
	uint32_t testLookFeatures() const { return m_lookFeatures; }   // S7 P2: the EFFECTIVE look mask (a create failure clears bit 0)
	bool testA2COn() const { return m_a2cOn; }   // (A108): the a2c family is loaded and the translator decides per draw
	uint32_t testA2COffCount() const { return m_a2cOffCount; }   // [GXA2C] state=off reason=... lines printed by the fallback
	GxPipeStats testCacheStats() const { return m_cache.stats(); }   // S7 P2 A11: every compile counter
	const Rec::GxPacketWriter& testWriter() const { return m_writer; }
	bool testShadowLastOnWorker() const { return m_shadowLastOnWorker; }
	uint32_t testShadowTags() const { return m_tr.shadowTags(); }
	uint64_t testShadowOwn() const { return m_tr.shadowOwn(); }
private:
	bool m_worldNextColour = false;      // NextTargetsAreWorld: the next MSAA CreateRenderTarget is the world colour (SAMPLED)
	// A seam pass's own descriptor-slot rotation: each slot keeps the timeline value of the submission that last recorded into it
	// (GxResources::nextUseValue() inside the open frame), and is reused only once THAT submission completed. Not the present's
	// m_presentWaits: a frame whose present is skipped still submits, without updating it.
	struct SeamSlots
	{
		std::vector<uint64_t> lastUse;
		uint32_t next = 0;
		void reset(uint32_t n) { lastUse.assign(n, 0); next = 0; }
		// the slot to record into, after waiting for its last submission; ~0u = decline (already in the open submission, or the wait failed)
		uint32_t take(Vk::VkCtx& ctx, uint64_t now);
	};
	GxNativePresent m_worldResolve;      // the present's own linear-light MSAA resolve, for the world target (ResolveLinear)
	uint32_t m_worldResolveSlots = 0;     // its descriptor slot count (= the present's); 0 = not initialised, ~0u = init failed
	SeamSlots m_worldResolveUse;
	GxFsr1 m_fsr1;                        // IGxWorldPass::Upscale(GX_UPSCALE_FSR1)
	uint32_t m_fsr1Slots = 0;             // 0 = not initialised, ~0u = init failed (declined for the session)
	SeamSlots m_fsr1Use;

	// A23-B §3.8: the ONE allocation-failure owner (installed into GXRES, its staging ring and the writer's stream ring)
	class AllocPolicy final : public Vk::VkAllocPolicy
	{
	public:
		explicit AllocPolicy(GxNativeDevice& d) : m_d(d) {}
		bool inject(const char* site, VkDeviceSize bytes) override;
		Decision failed(const char* site, VkDeviceSize bytes) override;
		void terminal(const char* site, VkDeviceSize bytes) override;

	private:
		GxNativeDevice& m_d;
	};
	AllocPolicy m_allocPolicy { *this };
	bool m_sRecovered = false;                    // front: this S transition already ran recoverStorage (once per S, §3.8)
	std::atomic<uint64_t> m_allocFailures { 0 };  // terminal in-scope allocation failures (part of bad, both modes)
	std::atomic<uint64_t> m_recoveries { 0 };
	std::atomic<uint64_t> m_ownerViolations { 0 }, m_wsiOnWorker { 0 };
	void csOwnerCheck(const char* where);   // §3.1 / §4.2: a worker-state chokepoint reached on the front outside an S body
	std::mutex m_recoverLock;
	std::map<std::string, uint64_t> m_recoverBySite;   // TEST inspection (arm 27): recoveries per site
	// A23-B
	GxNativeCs m_cs;
	bool m_csOn = false;                  // the worker runs (front; changes only at creation and csPark)
	std::atomic<bool> m_csExitDraining { false };   // A60: the CRT-exit quiesce started (TEST-visible)
	int m_inline = 0;                     // S depth (front)
	Mirror m_mirror = {};                 // front
	GxNativeSurface* m_wRt = nullptr;     // the WORKER's copy of the target binding (syncTarget reads it; §3.5 SetRenderTarget)
	GxNativeSurface* m_wDs = nullptr;
	std::mutex m_logLock;                 // logLine / vkLog run on the front, the worker and gx-vk-submit (§3.1)
	uint64_t m_csSyncs = 0;               // front: S transitions that drained
	volatile uint32_t m_raceSink = 0;     // the csRaceRot plant only
	bool m_reorderPending = false;        // the csReorder plant: a SetRenderState held back until the next draw
	std::function<void()> m_payloadLate;   // the X-cs-payload-late plant's deferred upload
	bool m_payloadLateDone = false;
	uint32_t m_reorderState = 0, m_reorderValue = 0;
	uint64_t m_csIssued = 0;              // front: exec halves dispatched (queued or inline) - the creation ledger's position
	uint64_t m_csCreateRetries = 0;       // front: front creations retried as S (§3.8)
	bool m_testRecreated = false;
	struct TestShot { VkBuffer buf = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; void* ptr = nullptr; uint32_t w = 0, h = 0; };
	std::vector<TestShot> m_testShots;      // testSwapchainReadback: back() = the current extent's; older ones freed at teardown
	VkBuffer testShotBuffer();         // the csRecoveryStaleSource plant: a recreation happened (worker on)
	bool m_mirrorCheck = false;           // GX_NATIVE_CS_MIRRORCHECK=1 (TEST mode, never in a measured run)
	uint64_t m_mirrorMismatches = 0;
	uint32_t m_mirrorLogged = 0;
	uint64_t m_lastPresentSeq = 0;        // front: the queue seq of the last Present / PresentLost pushed (the lead fence, §5)
	uint64_t m_bytesAtPresent = 0, m_frameBytesMax = 0;   // front: cs bytes at the last Present; the largest frame since the last line
	uint64_t m_csPresentRecovery = 0;     // worker: Presents its execution-time guard ended unshown (§3.9 step 5)
	std::atomic<uint64_t> m_resourceErrors { 0 };   // vkLog counts GXRES errors from any thread (the front's creations too)
	std::atomic<uint64_t> m_pubPressure { 0 }, m_pubSeq { 0 };   // worker -> front (§3.6 IDLE-FLUSH)
	std::vector<std::pair<uint64_t, uint64_t>> m_stagedQ;        // front: (queue seq, staging bytes) of queued upload commands
	size_t m_stagedHead = 0;
	uint64_t m_stagedSum = 0;
	struct SyncWhy { const char* why; uint64_t n, ns; };
	std::vector<SyncWhy> m_csSyncWhy;   // front: per reason, count + drain ns (S is rare; a short list)
	std::vector<SyncWhy> m_seamSyncAt;  // front: the "seam *" reasons' totals at the last [GXSEAM] cs drains line
	// front, bumped inside the world* S bodies: resolves recorded / published / with object vectors; registrations
	struct MotionCounters { uint64_t passes = 0, published = 0, withObject = 0, objAccepted = 0, objRefused = 0; };
	MotionCounters m_motionCount;
};

// d3d8's parameter checks for CreateDevice and Reset (and d3d9's on the converted parameters): false = DXVK answers INVALIDCALL.
bool GxD3D8PresentParamsValid(const D3DPRESENT_PARAMETERS& pp);
// What DXVK writes back into the CALLER's parameters once d3d8's front checks pass (d3d8_d3d9_util.h:89-94, from CreateDevice
// and Reset): BackBufferCount at least 1, an UNKNOWN back-buffer format becomes X8R8G8B8. The game sees the same struct on either
// path, and S4 judges what DXVK would build (the SDL3 platforms present Windowed=TRUE; iOS finds no exclusive mode, so UNKNOWN).
void GxD3D8NormalizePresentParams(D3DPRESENT_PARAMETERS* pp);
// Creates the native device (DESIGN §2: VkCtx, swapchain, pipeline set). FAILED + why = the caller takes the classic path.
HRESULT GxNativeCreateDevice(IDirect3D8* parent, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
	const D3DPRESENT_PARAMETERS& pp, const GxNativeHost& host, IDirect3DDevice8** out, std::string* why);
// The native device behind a d3d8 device pointer; null for any other device (DXVK's).
GxNativeDevice* GxNativeFromDevice(IDirect3DDevice8* dev);
// Registered with atexit by the first device that completes init: quiesces that device's submit thread (VkCtx::quiesce), then
// prints its final stats line (end=exit) - for a device the process never destroyed. No-op once that device is gone (it printed
// end=teardown). The game calls it before its _exit (which runs no atexit handler); tests call it directly.
void GxNativeFlushFinalStatsAtExit();
// {7E1B5F3C-4A2D-4C8E-9B61-3D5E0A9F2C71}: QueryInterface on a native device returns the GxNativeDevice.
extern const GUID IID_GxNative;

}} // namespace GXRender::Dev
