// GeneralsX: WS-B VkCtx v0: the native renderer's Vulkan device layer.
//
// Owns: the Vulkan library handle (the SAME one the process already uses - see VkCtxDesc::getInstanceProcAddr), the
// instance (+ core and synchronization validation when asked), the physical/logical device, one graphics queue and an
// optional dedicated transfer queue, a timeline semaphore per queue, the VMA allocator and a retirement list.
// Everything the frame ring, upload ring and GPU timer do is keyed by those timeline values, so "is the GPU done with X"
// always has one answer: completed(queue) >= the value of the submission that used X.
//
// Threading (v0): one thread owns the context and submits. completed() and the validation counters may be read from any
// thread. Render-only: nothing here is read by the simulation.
// Submit thread: with VkCtxDesc::submitThread (or
// GX_VK_SUBMIT_THREAD=1) a "gx-vk-submit" thread owns the queues: submit() and present() enqueue (a submit's timeline value
// is assigned at enqueue, in today's order) and the thread runs them FIFO with MoltenVK's own submits SYNCHRONOUS. A refused
// submit first DRAINS the device on the thread (every earlier accepted batch completes; not for VK_ERROR_DEVICE_LOST) and only
// then LATCHES the context: every op behind it is skipped, submit() returns 0, deviceLost() (= terminal) is true, and
// Timeline::wait() of a value that never ran returns false - by then nothing accepted still runs, so a caller acting on that
// false (a resource last used by the refused value) cannot race the GPU (A21 amendment 1 (1) / 2 (1)). A wait on an
// earlier value stays a real wait (only a DRIVER loss turns waits into counter reads). After a drained latch every retirement
// settles. Host waits first wait for the op to have executed. Every device-wide wait goes through waitIdle(), which drains first.
#pragma once

#include "VkCommon.h"

#include <atomic>
#include <cfenv>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GXRender { namespace Vk {

enum class LogLevel { Info, Warn, Error };
typedef void (*LogFn)(LogLevel level, const char* line, void* user);

enum class QueueKind { Graphics = 0, Transfer = 1 };

// R19 submit thread: Default = off unless GX_VK_SUBMIT_THREAD=1; On = on unless GX_VK_SUBMIT_THREAD=0 (the off switch);
// Off = never (a test whose refusal probe needs a context that survives a refused submit - gx_record_test order-units)
enum class SubmitThreadMode { Default, On, Off };

struct VkCtxDesc
{
	const char* appName = "GeneralsX";
	uint32_t appVersion = 0;

	// Khronos validation layer with core checks; syncValidation adds synchronization validation. init() FAILS when
	// validation is asked for and the layer is missing - a "clean" run without the layer would prove nothing.
	// GX_VK_VALIDATION=1 (core+sync) / =core (core only) in the environment also turns it on, =0 turns it off.
	// Debug builds (no NDEBUG) turn core+sync on by default when the layer is installed and only WARN when it is not.
	bool validation = false;
	bool syncValidation = true;
	// Return VK_TRUE from the messenger on a validation ERROR, so the layer skips the offending call. Only the fail-arm
	// tests set it: they plant real errors (a pool reset while its buffer is pending) that must not reach the driver.
	bool abortCallsOnValidationError = false;

	// The library the instance comes from. In the game pass SDL3's (SDL_Vulkan_GetVkGetInstanceProcAddr()) so the window
	// surface and the device share one implementation; null = load the platform loader with the DXVK fork's own list
	// (references/fbraz3-dxvk src/vulkan/vulkan_loader.cpp: macOS loader first and MoltenVK never by path, iOS bundle
	// Frameworks). Mixing two Vulkan implementations in one process crashed DXVK before (its vulkan_loader.cpp:35).
	PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
	// Load this file instead of searching (tests: a bogus path must make init() fail cleanly).
	const char* loaderPath = nullptr;

	std::vector<const char*> instanceExtensions; // extra, e.g. SDL_Vulkan_GetInstanceExtensions()
	std::vector<const char*> deviceExtensions;   // extra, e.g. VK_KHR_swapchain

	// Device choice: an index or a case-insensitive name substring; GX_VK_DEVICE overrides. Default = best type
	// (discrete > integrated > virtual > cpu) among devices that meet the floor (Vulkan 1.3, timeline, sync2, graphics).
	const char* deviceSelect = nullptr;
	// Optional present check: the graphics family must be able to present to this surface (WSI).
	std::function<bool(VkInstance, VkPhysicalDevice, uint32_t family)> presentSupport;

	LogFn log = nullptr;
	void* logUser = nullptr;

	SubmitThreadMode submitThread = SubmitThreadMode::Default;   // R19: a gx-vk-submit thread owns every queue op (header comment)
};

struct ValidationCounters
{
	uint32_t errors = 0;
	uint32_t warnings = 0;
	uint32_t syncHazards = 0; // messages whose id name starts with SYNC-HAZARD
	std::string firstError;   // id name + message of the first error, for the report
};

// A queue's monotone timeline. Every submission through VkCtx::submit() signals lastSubmitted()+1.
class Timeline
{
public:
	VkSemaphore semaphore() const { return m_semaphore; }
	uint64_t lastSubmitted() const { return m_lastSubmitted; }
	uint64_t pending() const { return m_lastSubmitted + 1; } // the value the NEXT submission will signal
	uint64_t completed() const;
	// Waits until completed() >= value. Returns false on timeout or device loss. A value never submitted is refused
	// (returns false at once) instead of hanging forever. A wait (or counter read) that sees a device error latches it on the
	// owning VkCtx (noteResult: VK_ERROR_DEVICE_LOST -> deviceLost(), which Present answers as DEVICELOST); once lost, a wait
	// only reads the counter. Under the submit thread it first waits for the op to have executed,
	// and a value refused or skipped behind a refusal returns false at once. A value already reached returns without
	// vkWaitSemaphores (MoltenVK builds an MTLSharedEventListener per call: render-perf-audit, ~0.036 ms of main per frame).
	bool wait(uint64_t value, uint64_t timeoutNs = UINT64_MAX) const;

private:
	friend class VkCtx;
	const class VkCtx* m_owner = nullptr;
	int m_index = 0;   // 0 graphics, 1 the dedicated transfer queue
	VkDevice m_device = VK_NULL_HANDLE;
	VkSemaphore m_semaphore = VK_NULL_HANDLE;
	uint64_t m_lastSubmitted = 0;
};

struct QueueInfo
{
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = UINT32_MAX;
	uint32_t timestampValidBits = 0;
	Timeline timeline;
	bool valid() const { return queue != VK_NULL_HANDLE; }
};

struct SemaphoreWait
{
	VkSemaphore semaphore = VK_NULL_HANDLE;
	uint64_t value = 0; // 0 for a binary semaphore
	VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
};

struct SubmitDesc
{
	std::vector<VkCommandBuffer> commandBuffers;
	std::vector<SemaphoreWait> waits;
	std::vector<SemaphoreWait> signals; // extra signals (binary: value 0); the queue's timeline is always appended
	VkFence fence = VK_NULL_HANDLE;
	const char* label = "submit";       // what the submission is (frame / immediate / pending / gputimer): named by a refusal
};

// The first refused submission under the submit thread (VkCtx::refusal()); value 0 = none.
struct SubmitRefusal
{
	const char* label = nullptr;
	uint64_t value = 0;
	VkResult result = VK_SUCCESS;
};

// FG P0 H2 (rw3-T-7's shape): called just before and just after vkQueuePresentKHR, on the thread
// that calls it - the caller with the submit thread off (no lock held there, rw3-T-7 correction (a)), gx-vk-submit with it on (under
// hostLock and the queue mutex). Only a present that reaches the driver calls it: a latched, stopped, skipped or TEST-injected present
// calls neither. end gets the driver's answer. Neither may call into this VkCtx (submit / present / waitOp / waitIdle / quiesce) or take
// its locks - nor block on ANYTHING a thread waiting for the submit thread may hold (a caller that holds lock L and
// then waits in waitOp / waitIdle / quiesce / takePresentStatus deadlocks a hook that takes L). Copied BY VALUE into the op: ctx must stay
// valid until the present ran (waitOp). Streamline's PCL ePresentStart / ePresentEnd go here (H3), so they bracket vkQueuePresentKHR
// alone, on the presenting thread, with the presented frame's token.
struct PresentHook
{
	void (*start)(void* ctx, uint64_t token) = nullptr;
	void (*end)(void* ctx, uint64_t token, VkResult result) = nullptr;
	void* ctx = nullptr;
	uint64_t token = 0;
};

// One queue op for the submit thread.
struct SubmitOp
{
	enum Kind : uint8_t { Submit, Present } kind = Submit;
	QueueKind queue = QueueKind::Graphics;
	SubmitDesc desc;          // Submit
	uint64_t value = 0;       // Submit: its timeline value
	uint64_t seq = 0;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;   // Present
	uint32_t image = 0;
	VkSemaphore wait = VK_NULL_HANDLE;
	std::mutex* hostLock = nullptr;
	VkResult* result = nullptr;
	PresentHook hook;         // Present: FG P0 H2, copied at enqueue (hasHook = one was passed)
	bool hasHook = false;
	struct TestCtl   // the thread-read TestMutants fields, taken under the op lock when the thread takes this op
	{
		uint64_t failAt = 0, presentAt = 0;
		VkResult failResult = VK_SUCCESS, presentResult = VK_SUCCESS;
		bool skippedOk = false, noLatch = false, latchLost = false, latchNoDrain = false, initLast = false, hookOnInjected = false;
		uint64_t resultAfterDriverAt = 0;
		VkResult resultAfterDriver = VK_SUCCESS;
		VkResult idleResult = VK_SUCCESS;
	} test;
};

struct SubmitThreadStats
{
	uint64_t enqueued = 0;        // ops accepted (submits + presents)
	uint64_t submits = 0;         // submits the driver accepted on the thread
	uint64_t presents = 0;        // present ops the thread ran (driver or injected answer; skipped ones not counted)
	uint64_t driverPresents = 0;  // vkQueuePresentKHR calls the thread made
	uint64_t skipped = 0;         // ops not run because the context was latched
	uint64_t waitFastPath = 0;    // Timeline::wait returns that skipped vkWaitSemaphores (both modes)
};

// Device facts the other streams branch on. Filled from what the device SUPPORTS and was ENABLED with.
struct DeviceFeatures
{
	bool samplerAnisotropy = false;
	bool textureCompressionBC = false;
	bool textureCompressionASTC_LDR = false;
	bool textureCompressionETC2 = false;
	bool fillModeNonSolid = false;
	bool independentBlend = false;
	bool depthClamp = false;
	bool depthBiasClamp = false;
	bool dualSrcBlend = false;
	bool multiDrawIndirect = false;
	bool drawIndirectFirstInstance = false;
	bool shaderClipDistance = false;
	bool timelineSemaphore = false;
	bool synchronization2 = false;
	bool dynamicRendering = false;
	bool hostQueryReset = false;
	bool descriptorIndexing = false;
	bool scalarBlockLayout = false;
	bool maintenance4 = false;
	bool portabilitySubset = false; // MoltenVK (and other layered drivers): VK_KHR_portability_subset enabled
	bool calibratedTimestamps = false;

	// Dynamic state (WS-D decides which pipeline-state dimensions may be dynamic per backend). EDS1 and the base of EDS2
	// are core in Vulkan 1.3 and always present on a qualifying device; the rest are extensions, enabled when listed.
	bool extendedDynamicState2LogicOp = false;            // VK_EXT_extended_dynamic_state2
	bool extendedDynamicState2PatchControlPoints = false; // VK_EXT_extended_dynamic_state2
	bool eds3ColorBlendEnable = false;                    // VK_EXT_extended_dynamic_state3 ...
	bool eds3ColorBlendEquation = false;
	bool eds3ColorWriteMask = false;
	bool eds3RasterizationSamples = false;
	bool eds3AlphaToCoverageEnable = false;
	bool eds3PolygonMode = false;
	bool eds3DepthClampEnable = false;
	bool vertexInputDynamicState = false;                 // VK_EXT_vertex_input_dynamic_state
	bool graphicsPipelineLibrary = false;                 // VK_EXT_graphics_pipeline_library (+ VK_KHR_pipeline_library)
};

class VkCtx
{
public:
	VkCtx() = default;
	~VkCtx() { shutdown(); }
	VkCtx(const VkCtx&) = delete;
	VkCtx& operator=(const VkCtx&) = delete;

	// Returns false with a reason; never throws, never aborts. Safe to call shutdown() after a failed init().
	bool init(const VkCtxDesc& desc, std::string* why);
	// Waits for the device to go idle, runs every pending retirement, destroys in reverse order.
	void shutdown();
	bool initialized() const { return m_device != VK_NULL_HANDLE; }

	VkInstance instance() const { return m_instance; }
	VkPhysicalDevice physicalDevice() const { return m_physical; }
	VkDevice device() const { return m_device; }
	VmaAllocator allocator() const { return m_allocator; }
	uint32_t apiVersion() const { return m_apiVersion; }
	const VkPhysicalDeviceProperties& properties() const { return m_props; } // vendorID, deviceID, driverVersion, pipelineCacheUUID
	const VkPhysicalDeviceDriverProperties& driverProperties() const { return m_driverProps; } // driverID, driverName, driverInfo
	bool deviceExtensionEnabled(const char* name) const;
	const VkPhysicalDeviceLimits& limits() const { return m_props.limits; }
	const DeviceFeatures& features() const { return m_features; }
	const std::string& loaderPath() const { return m_loaderPath; }
	const std::string& deviceLine() const { return m_deviceLine; }

	QueueInfo& queue(QueueKind kind) { return kind == QueueKind::Transfer && m_transfer.valid() ? m_transfer : m_graphics; }
	const QueueInfo& queue(QueueKind kind) const { return kind == QueueKind::Transfer && m_transfer.valid() ? m_transfer : m_graphics; }
	bool hasDedicatedTransfer() const { return m_transfer.valid(); }

	// vkQueueSubmit2 on the queue; appends the signal of the queue's timeline (lastSubmitted()+1) and returns that value,
	// or 0 on failure (logged). All submissions to a queue must go through here or the timeline stops being monotone.
	// Under the submit thread: enqueued (0 once latched); the refusal, if any, is seen by the next wait of its value.
	uint64_t submit(QueueKind kind, const SubmitDesc& desc);
	// vkQueuePresentKHR on the graphics queue, holding hostLock (the swapchain's host-access mutex). Synchronous: *result is
	// written now and 0 returned. Submit thread: enqueued after every earlier op; *result is written when it ran (read it after
	// waitOp(the returned sequence number)); a present queued behind a refusal is skipped and reads VK_ERROR_DEVICE_LOST.
	// hook (FG P0 H2, may be null = the present exactly as before): see PresentHook.
	uint64_t present(VkSwapchainKHR swapchain, uint32_t image, VkSemaphore wait, std::mutex* hostLock, VkResult* result,
		const PresentHook* hook = nullptr);
	// Submit thread: blocks until the op with this sequence number ran or was skipped (0 / synchronous: returns at once).
	void waitOp(uint64_t seq) const;
	// Submit thread: blocks until the submit carrying value has executed; false when it was refused or skipped. Synchronous:
	// true for any value submit() returned.
	bool waitExecuted(QueueKind kind, uint64_t value) const;
	// Every queued op runs (or is skipped), then vkDeviceWaitIdle until it answers VK_SUCCESS or VK_ERROR_DEVICE_LOST (completion
	// for the call returning it): on return the device is PROVEN idle, so an owner may destroy what its submissions used. Failing
	// answers are retried for 2 s of wall time; then the process ENDS with a FATAL line (idleDevice) - never a hang, never a free
	// under the GPU. It also settles the retirements a latch or a loss left, while the pools they free into still exist. Use it for
	// EVERY device-wide wait.
	void waitIdle();
	// The process is ending with the device alive (DESIGN v4 §3.7, A21 am. 4): stops accepting, runs (or skips, once
	// latched) every queued op, then JOINS gx-vk-submit. The context stays stopped: every later submit() answers 0 and every
	// present() is skipped (DEVICE_LOST), neither reaching the driver. No device idle - nothing is freed. Idempotent; a no-op
	// without the thread. Teardown stays shutdown().
	void quiesce();
	bool submitThreaded() const { return m_threaded; }
	SubmitRefusal refusal() const;
	SubmitThreadStats threadStats() const;
	// TEST-ONLY (the thread arms' controlled starting state): the thread stops BEFORE running the submit carrying this
	// graphics value until testRelease() (0 = no hold). Ops behind it wait too.
	void testHoldAt(uint64_t value);
	void testRelease();
	// TEST-ONLY: changes g_testMutants under the op lock (the thread reads its fields under it) - for every field the thread reads.
	// The callback runs under that lock: it must not call back into this context.
	void testSet(const std::function<void(TestMutants&)>& set);
	// TEST-ONLY (FG P0 H2 arm): whether some thread holds the queue mutex NOW, probed by a helper thread's try_lock (a try_lock by the
	// holder itself would be undefined). Never blocks; safe inside a present hook.
	bool testQueueMutexHeld();
	// TEST-ONLY (FG P0 H2 arm): gx-vk-submit's id (a default id without the thread) - the hook arms compare the
	// thread their callbacks ran on with it, not with each other
	std::thread::id testSubmitThreadId() const { return m_thread.get_id(); }
	// TEST-ONLY boundaries: threads now inside a blocking driver wait (Timeline::wait's vkWaitSemaphores,
	// waitIdle's vkDeviceWaitIdle), and threads now blocked in waitIdle's drain of the submit thread.
	int testBlockedWaits() const { return m_blockedWaits.load(); }
	// TEST-ONLY: how many times any thread has ENTERED Timeline::wait's vkWaitSemaphores (monotone: an entry is never missed by a
	// late sample)
	uint64_t testSemWaitEntries() const { return m_semWaitEntries.load(); }
	int testDrainWaiters() const;
	// TEST-ONLY: threads now blocked in quiesce()'s join; and whether the submit thread is still joinable (read it only from the
	// thread that would join it)
	int testQuiesceWaiters() const;
	bool testThreadJoinable() const { return m_thread.joinable(); }

	// Runs fn once completed(kind) >= value - at collect() or shutdown(). For resources whose last use is a submission.
	void retire(QueueKind kind, uint64_t value, std::function<void()> fn);
	void collect();

	bool validationEnabled() const { return m_validation; }
	bool syncValidationEnabled() const { return m_syncValidation; }
	ValidationCounters validation() const;
	void resetValidationCounters();

	void logf(LogLevel level, const char* fmt, ...) const;
	// Terminal: a driver loss, or a refused submission under the submit thread (Present answers DEVICELOST either way).
	bool deviceLost() const { return m_deviceLost.load() || m_latchedFlag.load(); }
	// Test hook (WSI recovery gate): behave as if the device was lost - submit() refuses, the swapchain reports
	// DeviceLost. The only recovery is the real one: shutdown() everything and init() again.
	void simulateDeviceLost() { m_deviceLost.store(true); }
	void noteResult(VkResult r, const char* what) const; // logs failures, latches VK_ERROR_DEVICE_LOST

private:
	friend class Timeline;   // its wait() syncs with the submit thread
	static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

	bool loadLibrary(const VkCtxDesc& desc, std::string* why);
	bool createInstance(const VkCtxDesc& desc, std::string* why);
	bool pickDevice(const VkCtxDesc& desc, std::string* why);
	bool createDevice(const VkCtxDesc& desc, std::string* why);
	void buildDeviceLine();
	VkResult submitDriver(QueueInfo& q, const SubmitDesc& desc, uint64_t value);
	void startThread();
	void stopThread();
	void threadMain();
	void runOp(SubmitOp& op);
	enum class Sync { Executed, Refused, Timeout };
	Sync syncExecuted(int index, uint64_t value, uint64_t timeoutNs) const;
	void settleRetirements();
	void idleDevice(VkResult inject);
	void noteFastPath() const { m_waitFastPath.fetch_add(1, std::memory_order_relaxed); }
	int queueIndex(QueueKind kind) const { return kind == QueueKind::Transfer && m_transfer.valid() ? 1 : 0; }

	VkCtxDesc m_desc;
	void* m_library = nullptr;
	std::string m_loaderPath;
	VkInstance m_instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
	VkPhysicalDevice m_physical = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VmaAllocator m_allocator = nullptr;
	uint32_t m_apiVersion = 0;
	VkPhysicalDeviceProperties m_props = {};
	VkPhysicalDeviceDriverProperties m_driverProps = {};
	DeviceFeatures m_features;
	std::vector<std::string> m_enabledDeviceExtensions;
	QueueInfo m_graphics;
	QueueInfo m_transfer;
	std::string m_deviceLine;

	bool m_validation = false;
	bool m_syncValidation = false;
	bool m_validationOptional = false; // the debug-build default: a missing layer warns instead of failing init
	mutable std::mutex m_counterMutex;
	ValidationCounters m_counters;
	mutable std::atomic<bool> m_deviceLost { false };

	struct Retirement { QueueKind kind; uint64_t value; std::function<void()> fn; };
	std::vector<Retirement> m_retirements;

	// The submit thread (R19). m_opMutex guards the op queue, m_executed, the latch, the stats and the hold; m_queueMutex is host
	// access to the VkQueue(s) (the thread's driver calls, waitIdle's vkDeviceWaitIdle).
	bool m_threaded = false;
	std::thread m_thread;
	mutable std::mutex m_opMutex;
	std::condition_variable m_opCv;            // the thread: work, a release, or stop
	mutable std::condition_variable m_doneCv;  // waiters: an op finished
	std::mutex m_queueMutex;
	std::deque<SubmitOp> m_ops;
	uint64_t m_seqNext = 0;
	uint64_t m_busySeq = 0;
	bool m_busy = false;
	bool m_stop = false;
	uint64_t m_executed[2] = { 0, 0 };
	bool m_latched = false;
	std::atomic<bool> m_latchedFlag { false };   // m_latched for lock-free readers (deviceLost())
	bool m_drained = false;   // the latch fell after a drain that PROVED the device idle: no retirement's resource is still in use
	SubmitRefusal m_refusal;
	SubmitThreadStats m_threadStats;
	mutable std::atomic<uint64_t> m_waitFastPath { 0 };
	uint64_t m_holdAt = 0;
	int m_drainWaiters = 0;
	int m_quiesceWaiters = 0;
	mutable std::atomic<int> m_blockedWaits { 0 };
	mutable std::atomic<uint64_t> m_semWaitEntries { 0 };
	uint64_t m_presentCount = 0;
	// FG P0 H2: the one vkQueuePresentKHR call site, inline and thread path alike - hook start, the driver, hook end
	// overrideAt / overrideResult: TestMutants::presentResultAfterDriver* (the thread's snapshot, or the producer's own read inline)
	VkResult doPresent(const VkPresentInfoKHR& info, const PresentHook* hook, uint64_t overrideAt, VkResult overrideResult);
	std::atomic<uint64_t> m_drivenPresents { 0 };   // TEST bookkeeping: presents that reached the driver (doPresent)
	std::fenv_t m_fenv;
};

}} // namespace GXRender::Vk
