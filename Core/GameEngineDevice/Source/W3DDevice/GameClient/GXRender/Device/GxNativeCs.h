// GeneralsX @feature D · rw3-X-2 13/10/2026 A23-B: the native device's command stream and its worker thread `gx-native-cs`
// (perf-audit/DESIGN_A23_wide-ring.md v8 §2, §3.10, §5 - DXVK's CS model, dxvk_cs.h).
//
// One producer (the device's front end: the thread calling the device, one at a time) and one consumer (the worker). A command
// is a by-value functor plus an optional payload of bytes copied at the call, laid out in one SPSC byte ring:
//   [Rec 16 B: size, payload, queue seq][the functor object][payload][pad to 16]
// A record that would cross the end of the ring is preceded by a padding record (seq 0) that fills the rest. The producer waits
// for room when the ring is full (counted: fullWaits / fullNs) - never fails a call. Queue sequences are consecutive from 1;
// the worker checks seq == previous + 1 for every record (seqErrors; DESIGN §7 arm 14) and publishes done() after running it.
//
// Threading: push/drain/stop/testHoldAt/testRelease only on the producer; done()/queued() any thread; run() only on the worker.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace GXRender { namespace Dev {

struct GxNativeCsStats
{
	uint64_t cmds = 0, bytes = 0, payloadBytes = 0;   // producer: records pushed (padding excluded), ring bytes, payload bytes
	uint64_t fullWaits = 0, fullNs = 0;               // producer: pushes that found no room, and the time they waited
	uint64_t drains = 0, drainNs = 0;                 // producer: drains that had to wait, and the time
	uint64_t leadWaits = 0, leadNs = 0;               // producer: waitFor() calls that had to wait (the device's lead fence)
	uint64_t wakes = 0;                               // producer: worker wake-ups it signalled
	uint64_t producerWakes = 0;                       // producer: returns from its own sleep in a drain / lead / room wait (A55)
	uint64_t producerNotifies = 0;                    // worker: wake-ups it signalled to a sleeping producer (A55; exact after stop)
	uint64_t publishes = 0;                           // producer: write-position publications (batched: see publish())
	uint64_t busyNs = 0;                              // worker: time spent running commands (read after a drain)
	uint64_t seqErrors = 0;                           // worker: records whose queue seq was not previous + 1 (the worker then
	                                                  // stops running commands: the stream is corrupt - counted in bad)
};

class GxNativeCs
{
public:
	GxNativeCs() = default;
	~GxNativeCs() { stop(); }
	GxNativeCs(const GxNativeCs&) = delete;
	GxNativeCs& operator=(const GxNativeCs&) = delete;

	// capacity: a power of two >= 64 KiB. false = no thread (allocation or thread creation failed): the caller stays direct.
	bool start(size_t capacity, const char* name);
	// Drain, then end and join the worker. Idempotent.
	void stop();
	bool running() const { return m_thread.joinable(); }
	size_t capacity() const { return m_cap; }
	// The largest payload a single command may carry (DESIGN §3.2 kMaxCmd = min(8 MiB, capacity / 4)); larger ones are S.
	size_t maxPayload() const { return m_cap ? std::min<size_t>(8u << 20, m_cap / 4) - 256 : 0; }

	// fn() or fn(const uint8_t* payload, size_t bytes). src: `bytes` copied into the record now (null = zero-filled).
	template <class F>
	void push(F&& fn, const void* src = nullptr, size_t bytes = 0)
	{
		pushFill(std::forward<F>(fn), bytes, [src, bytes](uint8_t* dst) {
			if (bytes)
				src ? std::memcpy(dst, src, bytes) : std::memset(dst, 0, bytes);
		});
	}
	// fill(uint8_t* dst) writes exactly `bytes` payload bytes (e.g. rows gathered from a pitched source).
	template <class F, class Fill>
	void pushFill(F&& fn, size_t bytes, Fill&& fill)
	{
		using Op = OpT<typename std::decay<F>::type>;
		static_assert(alignof(Op) <= 16, "a command functor needs at most 16-byte alignment");
		const size_t head = align16(sizeof(Rec) + sizeof(Op));
		const size_t need = head + align16(bytes);
		uint8_t* p = reserve(need);
		Rec* r = reinterpret_cast<Rec*>(p);
		r->size = uint32_t(need);
		r->payload = uint32_t(bytes);
		r->seq = ++m_seq;
		new (p + sizeof(Rec)) Op(std::forward<F>(fn));
		if (bytes)
			fill(p + head);
		m_stats.cmds++;
		m_stats.payloadBytes += bytes;
		commit(need);
	}

	// Makes every command pushed so far visible to the worker. A push only advances the producer's own position (a store to the
	// shared position the spinning worker reads costs a cross-core line transfer per command - 15-25x a setter's inline cost,
	// measured by the front bench): publication happens here - the device calls it after a draw, Clear, EndScene and Present -
	// once kPublishBytes are unpublished, and before every producer wait (room, drain, waitFor), so no wait depends on a command
	// the worker cannot see. As DXVK's CS chunks (EmitCsChunk). Order is unchanged: records run in push order.
	void publish();
	// Waits until the worker ran every pushed command (done() == queued()). Then the worker idles until the next push.
	void drain();
	// Waits until the worker ran queue seq `seq` (0 / already run: returns at once). Counted as leadWaits.
	void waitFor(uint64_t seq);
	bool testProducerWaiting() const { return m_producerWaiting.load(std::memory_order_acquire); }   // TEST: a blocked-state probe
	// TEST (the X-cs-full-fails plant): a record of `payload` bytes and functor size `fnBytes` would have to wait for room now
	bool testWouldWait(size_t fnBytes, size_t payload) const
	{
		const size_t need = align16(sizeof(Rec) + fnBytes) + align16(payload), off = m_wr & m_mask;
		const size_t pad = off + need > m_cap ? m_cap - off : 0;
		return m_wr + pad + need - m_rd.load(std::memory_order_acquire) > m_cap;
	}
	uint64_t queued() const { return m_seq; }                                  // producer: the last queue seq pushed
	uint64_t done() const { return m_done.load(std::memory_order_acquire); }   // the last queue seq the worker ran
	uint64_t runningSeq() const { return m_running; }   // WORKER only: the queue seq of the command it is running
	bool onWorker() const;   // the calling thread is this stream's worker
	GxNativeCsStats stats() const;   // worker fields are exact after a drain

	// TEST-ONLY (DESIGN §7 hold hook): the worker stops BEFORE running queue seq k until testRelease() (any thread);
	// testHeld() says it is there.
	void testHoldAt(uint64_t k) { m_holdAt.store(k, std::memory_order_release); }
	bool testWaitNoPublish = false;   // TEST (X-cs-wait-unpublished): a producer wait does not publish first (it can wait forever)
	void testRelease();
	// TEST-ONLY: the ring bytes one record of functor type F with `payload` bytes takes (arms that place records exactly).
	template <class F>
	static size_t testRecordBytes(size_t payload) { return align16(sizeof(Rec) + sizeof(OpT<typename std::decay<F>::type>)) + align16(payload); }
	bool testHeld() const { return m_held.load(std::memory_order_acquire); }
	// TEST-ONLY plant (X-cs-ring-overwrite): the next push skips its room check.
	bool testNoRoomCheck = false;
	// TEST-ONLY plants (A55, set before the first push): X-cs-wake - the worker notifies a waiting producer after every record (the
	// wake storm A55 removed); X-cs-wake-target - only once the last seq it ran is PAST the producer's target (off by one: a lost wake)
	bool testWakeEveryRecord = false;
	bool testWakeLate = false;
	// TEST-ONLY plant (X-cs-deep-clear, set before any stream starts): start() keeps the platform's default thread stack (Apple:
	// std::thread's 512 KiB) instead of the creating thread's
	static std::atomic<bool> testDefaultStack;

private:
	struct Rec
	{
		uint32_t size;      // whole record, padding included (a multiple of 16)
		uint32_t payload;   // payload bytes after the functor
		uint64_t seq;       // queue seq; 0 = padding
	};
	struct Cmd
	{
		virtual void run(const uint8_t* payload, size_t bytes) = 0;
		virtual ~Cmd() = default;
	};
	template <class F>
	struct OpT final : Cmd
	{
		F f;
		template <class G> explicit OpT(G&& g) : f(std::forward<G>(g)) {}
		void run(const uint8_t* p, size_t n) override
		{
			if constexpr (std::is_invocable<F&>::value) {
				(void)p;
				(void)n;
				f();
			} else
				f(p, n);
		}
	};
	static size_t align16(size_t n) { return (n + 15) & ~size_t(15); }

	uint8_t* reserve(size_t need);
	void commit(size_t need);
	void waitRoom(size_t need);
	bool waitDone(uint64_t seq);   // false: nothing to wait for
	void wakeWorker();
	void threadMain(const char* name);   // runs under the creating thread's FP environment (start)

	std::unique_ptr<uint8_t[]> m_buf;
	size_t m_cap = 0, m_mask = 0;
	uint64_t m_running = 0;       // worker
	// producer
	uint64_t m_seq = 0;
	size_t m_wr = 0;              // bytes written (monotone)
	size_t m_rdSeen = 0;          // the consumer position last read
	size_t m_pubLocal = 0;        // the position last published (m_pub's value, producer copy)
	GxNativeCsStats m_stats;
	// shared
	std::atomic<size_t> m_pub { 0 };     // bytes published (monotone)
	std::atomic<size_t> m_rd { 0 };      // bytes consumed (monotone)
	std::atomic<uint64_t> m_done { 0 };
	std::atomic<uint64_t> m_busyNs { 0 };
	std::atomic<uint64_t> m_seqErrors { 0 };
	std::atomic<uint64_t> m_prodNotifies { 0 };   // worker: stats().producerNotifies
	std::atomic<bool> m_quit { false };
	std::atomic<bool> m_workerSleeping { false };
	std::atomic<bool> m_producerWaiting { false };
	// A55: the queue seq a sleeping producer waits for (0 = room: any progress). Stored relaxed BEFORE the seq_cst store of
	// m_producerWaiting and read relaxed AFTER the worker's seq_cst load of it - that pair orders it (a stale value can only come with a
	// stale m_producerWaiting, whose episode's own m_done check already saw the record)
	std::atomic<uint64_t> m_wakeAt { 0 };
	std::atomic<uint64_t> m_holdAt { 0 };
	std::atomic<bool> m_held { false };
	std::mutex m_lock;
	std::condition_variable m_workerCv, m_producerCv, m_holdCv;
	// The worker. Apple: a pthread with at least the CREATING thread's stack - the exec halves it runs ran on the game's main thread
	// (8 MiB on macOS) before, and std::thread's 512 KiB overflowed under a deep driver encode (Codex rw3x-a23b-code-r12-core P1:
	// a 20000-rectangle Clear with GX_VK_SUBMIT_THREAD=0). Elsewhere std::thread already matches main (Windows: the exe's default
	// reserve; Linux: RLIMIT_STACK).
	struct Worker
	{
#if defined(__APPLE__)
		pthread_t t {};
		bool live = false;
		bool joinable() const { return live; }
		void join()
		{
			pthread_join(t, nullptr);
			live = false;
		}
#else
		std::thread t;
		bool joinable() const { return t.joinable(); }
		void join() { t.join(); }
#endif
	} m_thread;
};

// A60: ends the process at once - Windows TerminateProcess, POSIX _exit: no atexit handler, no static destructor, and nothing that takes
// the UCRT exit lock (UCRT's own _exit / abort / quick_exit do). For the CRT-exit watchdog only.
[[noreturn]] void gxTerminateProcessNow(int code);
// A60 (Codex rf2-a60-r2 P1): writes msg to the process's stderr through the OS (no FILE lock, no stdio buffer) on a helper thread and
// waits at most `ms` for it - a stalled pipe or a lock held by a blocked thread can delay the line, never what follows it
void gxWriteStderrBounded(const char* msg, int ms);

}} // namespace GXRender::Dev
