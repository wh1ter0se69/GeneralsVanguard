// GeneralsX @feature D · rw3-X-2 13/10/2026 A23-B: the native device's command stream (GxNativeCs.h).
#include "GxNativeCs.h"

#include <cfenv>
#include <chrono>
#include <atomic>
#include <thread>
#include <string>
#include <memory>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace GXRender { namespace Dev {

namespace {

thread_local const GxNativeCs* t_worker = nullptr;

int64_t nowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void cpuPause()
{
#if defined(_WIN32)
	YieldProcessor();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}

// The worker spins this long after its last command before it sleeps (the next command of a frame is usually that close;
// the game ring's own spin, dx8wrapper.cpp Ring_Thread_Main). The producer spins this long before it sleeps on a drain / room.
constexpr int64_t kWorkerSpinNs = 50000;
constexpr int64_t kProducerSpinNs = 20000;
constexpr size_t kPublishBytes = 16u << 10;   // publish() at least every 16 KiB of records

} // namespace

bool GxNativeCs::onWorker() const { return t_worker == this; }

std::atomic<bool> GxNativeCs::testDefaultStack { false };

bool GxNativeCs::start(size_t capacity, const char* name)
{
	if (running() || capacity < (64u << 10) || (capacity & (capacity - 1)))
		return false;
	m_buf.reset(new (std::nothrow) uint8_t[capacity]);
	if (!m_buf)
		return false;
	m_cap = capacity;
	m_mask = capacity - 1;
	m_seq = 0;
	m_wr = m_rdSeen = m_pubLocal = 0;
	m_pub.store(0);
	m_rd.store(0);
	m_done.store(0);
	m_quit.store(false);
	fenv_t env;   // the worker computes GxPackFF's floats: the creating thread's FP environment, bit-identical (DESIGN §4.1)
	fegetenv(&env);
#if defined(__APPLE__)
	struct Start { GxNativeCs* cs; const char* name; fenv_t env; };
	Start* st = new (std::nothrow) Start { this, name, env };
	pthread_attr_t a;
	bool ok = st && pthread_attr_init(&a) == 0;
	if (ok) {
		size_t stack = 0;
		if (!testDefaultStack.load(std::memory_order_relaxed)) {   // the creating thread's, never less than the default
			const size_t page = size_t(getpagesize());
			size_t def = 0;
			pthread_attr_getstacksize(&a, &def);
			stack = (std::max(pthread_get_stacksize_np(pthread_self()), def) + page - 1) / page * page;
		}
		ok = (stack == 0 || pthread_attr_setstacksize(&a, stack) == 0) &&
			pthread_create(&m_thread.t, &a, [](void* p) -> void* {
				std::unique_ptr<Start> s(static_cast<Start*>(p));
				fesetenv(&s->env);
				s->cs->threadMain(s->name);
				return nullptr;
			}, st) == 0;
		pthread_attr_destroy(&a);
	}
	if (!ok) {
		delete st;
		m_buf.reset();
		m_cap = m_mask = 0;
		return false;
	}
	m_thread.live = true;
#else
	try {
		m_thread.t = std::thread([this, name, env] {
			fesetenv(&env);
			threadMain(name);
		});
	} catch (...) {
		m_buf.reset();
		m_cap = m_mask = 0;
		return false;
	}
#endif
	return true;
}

void GxNativeCs::stop()
{
	if (!running())
		return;
	drain();
	{
		std::lock_guard<std::mutex> g(m_lock);
		m_quit.store(true, std::memory_order_seq_cst);
		m_workerCv.notify_one();
	}
	m_thread.join();
	m_buf.reset();
	m_cap = m_mask = 0;
}

void GxNativeCs::wakeWorker()
{
	if (m_workerSleeping.load(std::memory_order_seq_cst)) {
		std::lock_guard<std::mutex> g(m_lock);
		m_workerCv.notify_one();
		m_stats.wakes++;
	}
}

void GxNativeCs::waitRoom(size_t need)
{
	if (m_wr + need - m_rdSeen <= m_cap)
		return;
	m_rdSeen = m_rd.load(std::memory_order_acquire);
	if (m_wr + need - m_rdSeen <= m_cap || testNoRoomCheck)
		return;
	if (!testWaitNoPublish)
		publish();   // the worker frees room only by running what it can see
	// full: the caller waits for the worker (DESIGN §5 - back-pressure, never a failure; the worker never waits for us)
	m_stats.fullWaits++;
	const int64_t t0 = nowNs();
	auto room = [&] { return m_wr + need - (m_rdSeen = m_rd.load(std::memory_order_acquire)) <= m_cap; };
	// the sleeping check is a Dekker pair with the worker (it stores m_rd, then loads m_producerWaiting - both seq_cst): this side
	// must load m_rd seq_cst AFTER its seq_cst store of m_producerWaiting, or an acquire (RCpc on arm64) load can pass the store and
	// both sides miss each other - a lost wake-up (measured: 1 hang in 30 under load, queued == done, producer asleep)
	auto roomSc = [&] { return m_wr + need - (m_rdSeen = m_rd.load(std::memory_order_seq_cst)) <= m_cap; };
	while (!room() && nowNs() - t0 < kProducerSpinNs)
		cpuPause();
	if (!room()) {
		std::unique_lock<std::mutex> g(m_lock);
		m_wakeAt.store(0, std::memory_order_relaxed);   // any progress frees room
		m_producerWaiting.store(true, std::memory_order_seq_cst);
		while (!roomSc()) {
			m_producerCv.wait(g);
			m_stats.producerWakes++;
		}
		m_producerWaiting.store(false, std::memory_order_seq_cst);
	}
	m_stats.fullNs += uint64_t(nowNs() - t0);
}

uint8_t* GxNativeCs::reserve(size_t need)
{
	size_t off = m_wr & m_mask;
	if (off + need > m_cap) {   // fill the rest of the ring with a padding record, start at 0
		const size_t pad = m_cap - off;
		waitRoom(pad + need);
		Rec* r = reinterpret_cast<Rec*>(m_buf.get() + off);
		r->size = uint32_t(pad);
		r->payload = 0;
		r->seq = 0;
		m_wr += pad;
		m_stats.bytes += pad;
		off = 0;
	} else
		waitRoom(need);
	testNoRoomCheck = false;
	return m_buf.get() + off;
}

void GxNativeCs::commit(size_t need)
{
	m_wr += need;
	m_stats.bytes += need;
	if (m_wr - m_pubLocal >= kPublishBytes)
		publish();
}

void GxNativeCs::publish()
{
	if (m_wr == m_pubLocal)
		return;
	m_pubLocal = m_wr;
	m_pub.store(m_wr, std::memory_order_seq_cst);
	m_stats.publishes++;
	wakeWorker();
}

// done() only grows on a clean stream (a poisoned one stops running commands but still publishes their seqs)
bool GxNativeCs::waitDone(uint64_t seq)
{
	if (running() && !testWaitNoPublish)
		publish();   // every wait first makes all pushed commands visible (the correctness condition of batched publication)
	if (!running() || seq == 0 || done() >= seq)
		return false;
	auto caught = [&] { return m_done.load(std::memory_order_acquire) >= seq; };
	auto caughtSc = [&] { return m_done.load(std::memory_order_seq_cst) >= seq; };   // the Dekker side of the sleep (waitRoom)
	const int64_t t0 = nowNs();
	while (!caught() && nowNs() - t0 < kProducerSpinNs)
		cpuPause();
	if (!caught()) {
		std::unique_lock<std::mutex> g(m_lock);
		m_wakeAt.store(seq, std::memory_order_relaxed);   // A55: only the record that reaches seq wakes this wait
		m_producerWaiting.store(true, std::memory_order_seq_cst);
		while (!caughtSc()) {
			m_producerCv.wait(g);
			m_stats.producerWakes++;
		}
		m_producerWaiting.store(false, std::memory_order_seq_cst);
	}
	return true;
}

void GxNativeCs::drain()
{
	const int64_t t0 = nowNs();
	if (waitDone(m_seq)) {
		m_stats.drains++;
		m_stats.drainNs += uint64_t(nowNs() - t0);
	}
}

void GxNativeCs::waitFor(uint64_t seq)
{
	const int64_t t0 = nowNs();
	if (waitDone(seq)) {
		m_stats.leadWaits++;
		m_stats.leadNs += uint64_t(nowNs() - t0);
	}
}

GxNativeCsStats GxNativeCs::stats() const
{
	GxNativeCsStats s = m_stats;
	s.busyNs = m_busyNs.load(std::memory_order_acquire);
	s.seqErrors = m_seqErrors.load(std::memory_order_acquire);
	s.producerNotifies = m_prodNotifies.load(std::memory_order_acquire);
	return s;
}

void GxNativeCs::testRelease()
{
	std::lock_guard<std::mutex> g(m_lock);
	m_holdAt.store(0, std::memory_order_seq_cst);
	m_holdCv.notify_all();
}

void GxNativeCs::threadMain(const char* name)
{
	t_worker = this;
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	pthread_setname_np(name);
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	(void)name;
#elif defined(__linux__)
	pthread_setname_np(pthread_self(), name);
#endif
	size_t rd = 0;
	uint64_t last = 0;
	bool poisoned = false;
	for (;;) {
		size_t pub = m_pub.load(std::memory_order_acquire);
		if (pub == rd) {
			const int64_t t0 = nowNs();
			while ((pub = m_pub.load(std::memory_order_acquire)) == rd && nowNs() - t0 < kWorkerSpinNs)
				cpuPause();
			if (pub == rd) {
				std::unique_lock<std::mutex> g(m_lock);
				m_workerSleeping.store(true, std::memory_order_seq_cst);
				m_workerCv.wait(g, [&] {
					return (pub = m_pub.load(std::memory_order_seq_cst)) != rd || m_quit.load(std::memory_order_seq_cst);
				});
				m_workerSleeping.store(false, std::memory_order_seq_cst);
				if (pub == rd)
					break;   // quit with nothing left (stop() drains first)
			}
		}
		const int64_t b0 = nowNs();
		while (rd != pub) {
			uint8_t* p = m_buf.get() + (rd & m_mask);
			Rec* r = reinterpret_cast<Rec*>(p);
			const size_t size = r->size;
			if (r->seq) {
				const uint64_t hold = m_holdAt.load(std::memory_order_acquire);
				if (hold && hold == r->seq) {
					std::unique_lock<std::mutex> g(m_lock);
					m_held.store(true, std::memory_order_seq_cst);
					m_holdCv.wait(g, [&] { return m_holdAt.load(std::memory_order_seq_cst) != r->seq; });
					m_held.store(false, std::memory_order_seq_cst);
				}
				if (r->seq != last + 1) {   // a record replaced or lost: never run a corrupt stream (DESIGN §7 arm 14)
					m_seqErrors.fetch_add(1, std::memory_order_relaxed);
					poisoned = true;
				}
				last = r->seq;
				if (!poisoned) {
					m_running = r->seq;
					Cmd* c = reinterpret_cast<Cmd*>(p + sizeof(Rec));
					// the payload follows the functor at the record's 16-aligned head (pushFill's layout)
					c->run(p + (size - align16(r->payload)), r->payload);
					c->~Cmd();
				}
				m_done.store(r->seq, std::memory_order_seq_cst);
			}
			rd += size;
			m_rd.store(rd, std::memory_order_seq_cst);
			// A55: a sleeping producer is woken by the record it waits for (m_wakeAt; 0 = a room wait: any record), not by every
			// record - per record, main ran ~1.4 ms/frame of wake-ups inside Present's lead fence at wh1ter0se's camera (15/10)
			if (m_producerWaiting.load(std::memory_order_seq_cst)) {
				const uint64_t at = m_wakeAt.load(std::memory_order_relaxed);
				if (testWakeEveryRecord || (testWakeLate ? last > at : last >= at)) {
					std::lock_guard<std::mutex> g(m_lock);
					m_producerCv.notify_one();
					m_prodNotifies.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		m_busyNs.fetch_add(uint64_t(nowNs() - b0), std::memory_order_relaxed);
	}
	t_worker = nullptr;
}

void gxWriteStderrBounded(const char* msg, int ms)
{
	struct Job
	{
		std::string text;
		std::atomic<bool> done { false };
	};
	std::shared_ptr<Job> job;
	try {   // allocation or thread creation may fail at exit: then no line - never an exception out of the watchdog (Codex rf2-a60-r3)
		job = std::make_shared<Job>();
		job->text = msg;
		std::thread([job] {
#if defined(_WIN32)
			DWORD n = 0;
			WriteFile(GetStdHandle(STD_ERROR_HANDLE), job->text.data(), DWORD(job->text.size()), &n, nullptr);
#else
			for (size_t off = 0; off < job->text.size();) {
				const ssize_t n = ::write(2, job->text.data() + off, job->text.size() - off);
				if (n <= 0)
					break;
				off += size_t(n);
			}
#endif
			job->done.store(true, std::memory_order_release);
		}).detach();
	} catch (...) {
		return;
	}
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!job->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void gxTerminateProcessNow(int code)
{
#if defined(_WIN32)
	TerminateProcess(GetCurrentProcess(), UINT(code));
	for (;;)
		Sleep(INFINITE);   // not reached
#else
	_exit(code);
#endif
}

}} // namespace GXRender::Dev
