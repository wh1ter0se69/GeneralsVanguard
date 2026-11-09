// GxPipelineCache.h - the native renderer's pipeline cache (WS-D; render-seam/DESIGN_pipeline-cache.md §4).
//
// Holds every VkPipeline the session creates, keyed by GxPipelineKey, over one VkPipelineCache persisted per device + driver.
// The GENERIC set (census draw parts x the session's attachment configurations, plus every key this machine missed before)
// is pre-built on worker threads before gameplay; a lookup that finds no generic pipeline is a GENERIC MISS - counted, logged
// ([GXPIPE] MISS), compiled on the calling thread and recorded so the next launch prebuilds it. A SPECIALISED FF variant
// (getFF: the same ubershader with the FF key as specialisation constants) is compiled on background workers the first time
// its {key, FF key} is drawn; until it is published the generic pipeline draws (DESIGN §3, PLAN §1 cache-miss policy).
// Custom (WS-E) keys have no generic stand-in.
#pragma once

#include "VkCommon.h"
#include "GxPipelineKey.h"
#include "GxFFState.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GXRender {

namespace Vk { class VkCtx; }

// The shaders + layout every FF pipeline shares (DESIGN §3): the FF ubershader's vertex shader and its smooth and flat
// fragment shaders (shaders/gx_ff*), one layout (rw2-F1's GxRecordLayout). Generic and specialised pipelines use the SAME
// modules: a specialised one only adds VkSpecializationInfo.
struct GxGenericShaders {
	VkShaderModule   vs = VK_NULL_HANDLE;
	VkShaderModule   fs = VK_NULL_HANDLE;
	VkShaderModule   fsFlat = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	// B · a2c-2 (A108; the integrator's ruling after DLSS_INTEGRATION.md:3876): the alpha-to-coverage family (gx_ff[_flat][.sm].a2c.frag),
	// null unless the session runs A2C. createPipeline() uses it for a key with alphaToCoverage = 1 and ONLY for one: every other key
	// keeps fs / fsFlat (today's module), so a plain draw in an A2C session is today's pipeline exactly.
	VkShaderModule   fsA2c = VK_NULL_HANDLE;
	VkShaderModule   fsFlatA2c = VK_NULL_HANDLE;
};

// WS-E (rw2-E): the custom-program modules (GxCustomCatalog.h ids). `ps` is gx_cs_ps.frag (program = spec constant 40; the TEST-ONLY
// mutant build of it also reads constant 41 = the program to perturb), `vsTrees` is gx_cs_trees.vert, `vsWave` gx_cs_wave.vert. A custom pipeline uses the
// FF layout (set 0 b0 = VS block - a custom VS reads its constants c0-c95 from the first 1536 B of it, b1 = PS block, set 1 = 8
// samplers, push = the FF key), the FF vertex shader unless the key names a custom VS, and the FF generic fragment shader unless
// it names a custom PS. The FF stage that stays is the GENERIC one (it reads the pushed FF key at run time).
struct GxCustomShaders {
	VkShaderModule ps = VK_NULL_HANDLE;
	VkShaderModule vsTrees = VK_NULL_HANDLE;
	VkShaderModule vsWave = VK_NULL_HANDLE;
	// D · render-motion-units-1 (A35): the motion bridge's synthetic programs (GxSyntheticPrograms.h); null unless the host's
	// motionUnits key is on, and then a gx:vel-* draw has no module (refused like any custom program without one)
	VkShaderModule vsVelRigid = VK_NULL_HANDLE;   // gx_cs_vel.vert
	VkShaderModule vsVelSkin = VK_NULL_HANDLE;    // gx_cs_vel.vert -DGX_VEL_SKIN
	VkShaderModule psVel = VK_NULL_HANDLE;        // gx_cs_vel.frag (GxCsPsVelocity: its own module, not gx_cs_ps.frag)
	VkShaderModule vsShadow = VK_NULL_HANDLE;   // gx_cs_shadow.vert (C · rw3-R11): the GX_SHADOW_GPU shadow-volume vs
	VkShaderModule psA2c = VK_NULL_HANDLE;      // B · a2c-2: gx_cs_ps[.sm].a2c.frag - `ps` for an alphaToCoverage = 1 key only (null: no A2C)
	uint32_t       psMutate = 0;   // TEST-ONLY: program id passed as constant 41 (0 = none); ps must be the mutant module
};

struct GxPipeStats {
	uint32_t prebuildQueued = 0;   // keys handed to prebuild() (incl. recorded misses)
	uint32_t prebuilt = 0;         // created by the prebuild workers
	uint32_t recordedLoaded = 0;   // keys this machine missed (or built custom) in earlier sessions, loaded from the keys file
	uint32_t recordedSkipped = 0;  // recorded CUSTOM keys this session's module set cannot build (an older build's): not queued
	uint32_t a2cTwins = 0;         // B · a2c-1 (A108): A2C twins prebuild() added (setA2CTwins on)
	uint32_t a2cRecordedSkipped = 0; // B · a2c-1: recorded A2C keys a session WITHOUT A2C skipped (an earlier key-on session's): not queued
	uint32_t a2cPlainTwins = 0;    // B · a2c-2 (DLSS_INTEGRATION.md:3857 (1)): ...of them, the PLAIN twins queued instead (new candidates only)
	uint32_t prebuildRuns = 0;     // B · a2c-2 (the integrator's condition a): prebuild() calls - a key-off session makes exactly one
	uint32_t a2cFirstUseFailed = 0; // B · a2c-2 (DLSS_INTEGRATION.md:3927 a): an alphaToCoverage = 1 key whose pipeline failed OUTSIDE a
	                                // prebuild (a draw-time get() miss, buildCustom): its draw is refused - a stated limit, counted
	uint32_t failed = 0;           // vkCreateGraphicsPipelines errors or refused keys (any path)
	uint32_t prebuildFailed = 0;   // of them, prebuild keys: the session must not start native rendering with any
	uint32_t genericMiss = 0;      // get() found no generic pipeline and compiled one on the caller's thread
	uint32_t prebuildWait = 0;     // get() waited for a key in flight (gated at 0 like a miss)
	uint32_t specRequested = 0;    // get() called with an FF-specialised key (served generic: getFF() is the specialised path)
	uint32_t specQueued = 0;       // getFF(): {key, FF key} pairs seen for the first time, queued for a background compile
	uint32_t specCompiled = 0;     // ...built and published (later getFF() calls return them)
	uint32_t specFailed = 0;       // ...failed: the generic pipeline keeps drawing that pair, never retried per draw
	uint64_t specFallbackDraws = 0;// getFF() answered with the generic pipeline because the specialised one was not ready
	double   specMaxMs = 0.0;      // the slowest specialised compile (background)
	uint64_t specHandoverDeferred = 0; // getFF() found a worker holding the handover lock and moved on (never waits)
	double   specTotalMs = 0.0;
	uint32_t customBuilt = 0;      // buildCustom(): custom pipelines built (WS-E)
	double   customMs = 0.0;       // ...their total build time
	uint32_t customMiss = 0;       // Custom (WS-E) keys not prebuilt: no generic fallback exists for them
	double   prebuildMs = 0.0;     // wall time of the last prebuild, first key to last
	double   missMs = 0.0;         // total time get() spent compiling misses
	bool     cacheLoaded = false;  // a persisted VkPipelineCache passed every check and was handed to the driver
	bool     driverRefused = false;// ...and the driver still refused it (retried empty)
};

struct GxKeyHasher { size_t operator()(const GxPipelineKey& k) const { return size_t(GxFastHash64(&k, sizeof(k))); } };
struct GxKeyEq { bool operator()(const GxPipelineKey& a, const GxPipelineKey& b) const { return GxKeyEqual(a, b); } };

// TEST-ONLY fault injection (gx_pipecache_test's fail arms); production code never sets it.
struct GxPipeTestMutants {
	bool skipNullStream = false;      // createPipeline() leaves out the zero-stream inputs: the VS reads unprovided locations
	bool allowBadOffset = false;      // createPipeline() passes an attribute offset above maxVertexInputAttributeOffset on
	bool failLastPrebuildKey = false; // the worker that takes the LAST queued key reports a failure (completion-gate arm)
	unsigned lastWorkerDelayMs = 0;   // ...and waits this long between counting it and decrementing (the race window)
	bool failMisses = false;          // get() discards what it compiled: the key is published as failed (caller-fail-first)
	void (*afterMissClaim)() = nullptr; // get() calls it after claiming a miss, before compiling (caller-in-flight)
	bool syncCustom = false;          // get() compiles a custom (WS-E) miss on the CALLER (replay tools only: production prebuilds)
	bool syncSpec = false;            // getFF() compiles a new specialised variant on the CALLER (the cold-cache fail arm)
	VkShaderModule specFs = VK_NULL_HANDLE; // every specialised pipeline uses this FS (the identity gate's mutant arm)
	VkShaderModule specFsFlat = VK_NULL_HANDLE; // B · a2c-1: ...and a FLAT-shaded key this one when set (gx_ff_test --a2c-family)
	bool specCorrupt = false;         // the specialised pipeline gets stage 0's colour op with bit 0 flipped while the pushed key
	                                  // stays right: if it still matched, the specialisation constants would not be in force
	bool specCorruptVs = false;       // ...the same for a VS-only word (HasColor0): proves the VERTEX stage's constants apply
	bool failKeysRewrite = false;     // rewriteKeysFile() fails as if no replacement could be created (keys-repair-fail)
	bool noCustomRecord = false;      // buildCustom() does not record its key (custom-record's fail arm)
	unsigned keysAppendShortBy = 0;   // recordMiss() appends a record this many bytes short and sees the failure (keys-append-short)
	bool noA2cTwins = false;          // B · a2c-1: prebuild() adds no A2C twin although setA2CTwins is on (a2c-twins-none: A2C draws miss)
	bool noA2cPlainTwins = false;     // B · a2c-2: a session WITHOUT A2C queues no plain twin of a recorded A2C key (a2c-plain-twin-none: 1 miss)
	std::atomic<bool> failA2cKeys { false };   // B · a2c-2: createPipeline() fails every alphaToCoverage = 1 key (the A2C prewarm-failure
	                                  // plant; atomic - a specialised compile worker may read it while a test clears it: Codex a2c-code-r11 #5)
	std::atomic<unsigned> failA2cHits { 0 };   // ...and counts every key it refused (the native arms' evidence that the plant was reached)
	bool a2cModuleSwapOne = false;    // B · a2c-2: the FIRST plain FF key created takes the a2c module (a2c-modules-swap must catch it)
	bool cumulativePrebuildFailed = false; // B · a2c-2: prebuildComplete() reads every prebuild's failures again (prebuild-again's X-)
	bool plainTwinNoCustomCheck = false; // B · a2c-2: a recorded custom A2C key's plain twin is queued WITHOUT the custom-module check
	bool retargetNoBuildCheck = false;   // B · a2c-2: retargetedKeys() returns keys this session cannot build (a2c-retarget-unbuildable-nocheck)
	                                     // (a2c-recorded-custom-nocheck: an unbuildable twin then fails the prebuild)
};
extern GxPipeTestMutants g_gxPipeTestMutants;

// The attachment configurations a session can bind (DESIGN §5.2): colour B8G8R8A8 x {device depth-stencil, none} x
// {MSAA, 1}. d3dBackBufferFormat is the back buffer's D3DFORMAT value: a 16-bit session (R5G6B5 / X1R5G5B5 / A1R5G5B5 -
// dx8wrapper.cpp:2382's fallback) returns false and renders classic. msaa is clamped to what the device supports.
bool GxSessionAttachConfigs(Vk::VkCtx& ctx, uint32_t d3dBackBufferFormat, uint8_t msaa, std::vector<GxAttachConfig>& out,
                            std::string* why);

class GxPipelineCache {
public:
	GxPipelineCache() = default;
	~GxPipelineCache() { shutdown(); }
	GxPipelineCache(const GxPipelineCache&) = delete;
	GxPipelineCache& operator=(const GxPipelineCache&) = delete;

	// cacheDir: where gxpipe-<vendor>-<device>-<driver>-<uuid>.{bin,keys} live ("" = no persistence). dynamicMask: the
	// GxDynamicMask features supported AND enabled on this device (0 = Vulkan 1.3 core only).
	bool init(Vk::VkCtx& ctx, const GxGenericShaders& shaders, const std::string& cacheDir, uint32_t dynamicMask,
	          std::string* why);
	// PRECONDITION: the caller has stopped submitting. Waits for the device to go idle (so no submitted draw can still use a
	// pipeline), waits for the prebuild, saves, destroys every pipeline and the VkPipelineCache.
	void shutdown();
	// Stops the specialisation workers (queued compiles dropped, a running one finishes; later requests fall back to the generic
	// pipeline) and waits for the prebuild: no worker of this cache runs after it. The native device's exit flush (A23-B §3.9) - the
	// process may end through static destruction right after it (the Windows CRT exit). Idempotent; shutdown() starts with it.
	void quiesceWorkers();
	size_t testSpecWorkers() const { return m_specWorkers.size(); }   // TEST-ONLY: live specialisation workers

	// Queues parts x configs (normalised, de-duplicated) + this machine's recorded misses for `workers` threads; returns the
	// number queued. Returns at once. A key already built or claimed (in flight) is skipped.
	// B · a2c-1 (Codex a2c-code-r3 #4): every key this cache has published (non-null) or recorded at `fromSamples`, re-made for each of
	// `configs` drawn at `toSamples` with the same depth presence (custom shader identity kept) - what an A2C session's Reset to another MSAA
	// hands prebuild() as `extra` (whose A2C twins follow), so a key the session already drew is never compiled on first use again.
	// B · a2c-2 (Codex a2c-code-r9 #4): a recorded key this session cannot build (recordedBuildable) is not re-made - startup skipped it too.
	std::vector<GxPipelineKey> retargetedKeys(uint8_t fromSamples, uint8_t toSamples, const GxAttachConfig* configs, size_t configCount);
	size_t prebuild(const GxDrawPart* parts, size_t partCount, const GxAttachConfig* configs, size_t configCount,
	                unsigned workers, const std::vector<GxPipelineKey>* extra = nullptr);
	bool prebuildDone() const { return m_prebuildRemaining.load(std::memory_order_acquire) == 0; }
	// After the prebuild: every key of the last prebuild's WHOLE closure published and non-null - incl. keys it did not queue
	// because a caller's get() had already failed them or was still compiling them (Codex r5 #1). False = do not start native
	// rendering (DESIGN §4). A worker counts a failure BEFORE its decrement, so once the remaining count reads 0 (acquire) the
	// failure count is final. Latched once true (pipelines are never removed before shutdown); prebuild() clears the latch.
	// B · a2c-2 (Codex a2c-code-r7 #6): it reads the failures of the LAST prebuild() only (a session with one prebuild - every key-off
	// session - reads exactly what it read before; a later prebuild is not failed forever by an earlier one's key).
	bool prebuildComplete() const;
	void waitPrebuild();
	// B · a2c-2 (the integrator's ruling after DLSS_INTEGRATION.md:3876): after waitPrebuild(), un-publish the keys the LAST prebuild's
	// workers failed (still null), so nothing of a failed A2C prewarm stays behind: a later draw of such a key takes today's first-use path
	// (get() compiles it) instead of a stale null. Returns how many were dropped. Never called in a key-off session.
	size_t dropPrebuildFailures();

	// Render thread. `frame` only labels the miss line.
	VkPipeline get(const GxPipelineKey& key, uint32_t frame);
	// Render thread ONLY (one thread), every FF draw: the SPECIALISED pipeline of {key, spec} once its background compile
	// has been collected, else the generic pipeline for key (get(), same miss rules) - queued for the workers at first
	// sight, never compiled here. It never waits for a worker: the specialised map is this thread's own, and keys/results
	// cross to/from the workers under a try_lock (deferred to the next call when a worker holds it). *specialised says which
	// came back. The FF key must also be pushed (GxFFState.h): the generic pipeline reads it.
	// WS-E. The module set createPipeline() uses for a GxShaderCustom key (key.shader.reserved[0] = GxCsVsId, [1] = GxCsPsId).
	void setCustomShaders(const GxCustomShaders& c) { m_custom = c; }
	// B · a2c-1 (A108): the session runs alpha-to-coverage (the device's a2c family is loaded). Then prebuild() also queues the A2C twin
	// (alphaToCoverage = 1) of every candidate a GxAlphaToCoverage.h draw can turn on - multisampled, blending off, colour writes on, not a
	// gx:vel program - so no A2C draw compiles on first use. Off (the default): recorded A2C keys are skipped and counted, never built.
	void setA2CTwins(bool on) { m_a2cTwins = on; }
	static bool a2cTwinEligible(const GxPipelineKey& k);
	// Blocking build + publish of one custom key (the sanctioned way to get a custom pipeline: at load, before the first draw).
	// Returns the published pipeline, NULL on failure. A custom key never has a generic stand-in, so get() on a key nobody built
	// is a counted miss that draws nothing. A key built here is RECORDED in the keys file, so the next session's prebuild builds
	// it before the first frame (DESIGN_l1-device §10.2 item 1); a recorded custom key this session's modules cannot build is
	// skipped there (recordedSkipped), never queued, so it can never fail the prebuild.
	VkPipeline buildCustom(const GxPipelineKey& key);
	VkPipeline getFF(const GxPipelineKey& key, const GxFFSpecBlock& spec, uint32_t frame, bool* specialised = nullptr);
	void waitSpecIdle();   // tests, render thread: hand over, wait until no specialised compile is queued or running, collect
	bool contains(const GxPipelineKey& key) const;

	bool save();
	GxPipeStats stats() const;
	// B · a2c-2 (DLSS_INTEGRATION.md:3927 a): stats().a2cFirstUseFailed alone - the device's periodic [GXA2C] line reads it on the present
	// path (a plain locked getter, like stats(); its own name so the render-seam lint never reads it as a draining device accessor)
	uint32_t a2cFirstUseFailed() const;
	const std::string& cachePath() const { return m_path; }
	const std::string& keysPath() const { return m_keysPath; }
	const std::string& loadNote() const { return m_loadNote; }   // why the persisted cache was or was not used

	// Every published key and its pipeline (NULL = failed). Tests only; holds the reader lock while it runs.
	template<typename Fn> void visit(Fn fn) const {
		std::shared_lock<std::shared_mutex> r(m_lock);
		for (const auto& kv : m_map)
			fn(kv.first, kv.second);
	}

	// The Vulkan pipeline for a key - exposed for tests that only create (no lookup bookkeeping). spec != NULL builds the
	// SPECIALISED variant: specialisation constants 0-31 = spec's words, 32 (gxSpecialised) = true.
	VkPipeline createPipeline(const GxPipelineKey& key, const GxFFSpecBlock* spec = nullptr);

private:
	struct GxSpecKey { GxPipelineKey key; GxFFSpecBlock spec; };   // the WHOLE FF key: equality never rests on the hash
	struct GxSpecHasher { size_t operator()(const GxSpecKey& k) const { return size_t(GxFastHash64(&k.key, sizeof(k.key))); } };
	struct GxSpecEq {
		bool operator()(const GxSpecKey& a, const GxSpecKey& b) const {
			return GxKeyEqual(a.key, b.key) && std::memcmp(&a.spec, &b.spec, sizeof(a.spec)) == 0;
		}
	};
	struct GxSpecEntry { VkPipeline p = VK_NULL_HANDLE; bool done = false; };   // done && !p = failed

	void worker();
	void specWorker();
	VkPipeline compileSpec(const GxSpecKey& k);   // creates + counts; the caller publishes
	VkPipeline insert(const GxPipelineKey& key, VkPipeline p);   // returns the published (winning) handle
	VkPipeline find(const GxPipelineKey& key, bool* found) const;
	bool loadCacheData(std::vector<char>& data, std::string* note) const;
	void loadRecordedKeys();
	bool rewriteKeysFile();   // m_recordLock held; false = the file on disk may still be damaged (m_keysDirty set)
	void recordMiss(const GxPipelineKey& key);
	// The modules a GxShaderCustom key names (unchanged for a stage it does not name); false + why = this session cannot build it.
	bool customModules(const GxPipelineKey& key, VkShaderModule* vs, VkShaderModule* fs, std::string* why) const;
	// B · a2c-2 (Codex a2c-code-r9 #4): what prebuild() checks of a RECORDED key before queuing it - a velocity key needs this session's
	// gx:vel modules, a custom key its program's modules. False = queuing it would fail the closure.
	bool recordedBuildable(const GxPipelineKey& key) const;
	bool limitsOk(std::string* why) const;

	Vk::VkCtx* m_ctx = nullptr;
	GxGenericShaders m_shaders;
	GxCustomShaders m_custom;
	uint32_t m_dynamicMask = 0;
	bool m_a2cTwins = false;   // B · a2c-1 (A108)
	uint32_t m_prebuildFailedBase = 0;           // B · a2c-2: m_stats.prebuildFailed when the last prebuild() started (m_statsLock)
	std::vector<GxPipelineKey> m_prebuildFailedKeys;   // B · a2c-2: the keys the last prebuild's workers failed (m_statsLock)
	std::string m_path, m_keysPath, m_loadNote;
	VkPipelineCache m_cache = VK_NULL_HANDLE;

	// Lock order: m_queueLock may be held while taking m_lock (shared), never the reverse.
	mutable std::shared_mutex m_lock;          // m_map
	std::unordered_map<GxPipelineKey, VkPipeline, GxKeyHasher, GxKeyEq> m_map;   // a failed key maps to VK_NULL_HANDLE

	mutable std::mutex m_queueLock;            // m_queue, m_queueNext, m_inFlight, m_required
	std::condition_variable m_queueCv;         // a key left m_inFlight
	std::vector<GxPipelineKey> m_queue;
	size_t m_queueNext = 0;
	std::unordered_set<GxPipelineKey, GxKeyHasher, GxKeyEq> m_inFlight;   // claimed: queued, being built, or a miss compiling
	std::atomic<size_t> m_prebuildRemaining { 0 };
	std::vector<GxPipelineKey> m_required;     // the last prebuild's whole closure (normalised)
	mutable std::atomic<bool> m_requiredReady { false };
	std::vector<std::thread> m_workers;
	std::chrono::steady_clock::time_point m_prebuildStart;

	std::mutex m_recordLock;                   // m_recorded + the keys file
	std::vector<GxPipelineKey> m_recorded;     // misses of earlier sessions (loaded) and of this one (appended)
	bool m_keysDirty = false;                  // the file may not be whole: never append until a whole rewrite succeeds

	mutable std::mutex m_statsLock;
	GxPipeStats m_stats;

	// Specialised FF variants. The render thread owns m_spec and m_specOutbox (no lock); m_specLock guards only the
	// hand-over lists, is never held while compiling or while taking another lock, and the render thread only try_locks it.
	std::unordered_map<GxSpecKey, GxSpecEntry, GxSpecHasher, GxSpecEq> m_spec;   // render thread (and shutdown)
	std::vector<GxSpecKey> m_specOutbox;       // render thread: new keys not handed over yet
	std::mutex m_specLock;
	std::condition_variable m_specCv;          // queue gained a key, a compile finished, or shutdown
	std::deque<GxSpecKey> m_specQueue;         // -> workers
	std::vector<std::pair<GxSpecKey, VkPipeline>> m_specDone;   // <- workers (NULL = failed)
	std::atomic<bool> m_specDoneReady { false };
	unsigned m_specBusy = 0;
	bool m_specStop = false;
	std::vector<std::thread> m_specWorkers;
	std::atomic<uint64_t> m_specFallbackDraws { 0 }, m_specHandoverDeferred { 0 };   // render-thread counters, no lock
	std::atomic<uint32_t> m_specQueued { 0 };
	bool trySpecHandover(bool wait);           // render thread: outbox -> queue, done -> m_spec
};

} // namespace GXRender
