// GeneralsX: render-split step 1: the tick window
// and the deferred render-command queue.
//
// The simulation calls 272 render-side entry points (scripts/test/audit/render-split-inventory.py). In a two-thread build
// the simulation thread must not run them: they write state the draw reads. This header makes an entry point THREAD-AWARE
// AT THE CALLEE, in render code the SimID digest does not watch: called from inside the tick window, the entry point
// records the call - its inputs copied at call time, plus the logic frame it would have read - and returns; the queue is
// replayed in call order when the window closes.
//   * One thread (today): the window is the logic tick, GameClient::setFrame to GameClient::step. Time frozen skips step,
//     so the next GameClient::update closes it instead, before anything there reads what was queued.
//   * Two threads (later): the window is "on the simulation thread"; the replay runs in the sync phase, simulation parked.
// Replaying is invisible today because nothing reads a converted entry point's state between the call and the end of the
// window except the simulation itself, and an entry point is converted only after checking the simulation never reads
// that state back (2.3 rule 1). A replayed body reads the logic frame through logicFrame(), which returns the
// frame captured at the call, so timestamps and time-outs come out identical.
//
// RENDER-ONLY: no simulation code includes it. No engine includes, so scripts/test/audit/bench/render_split.cpp tests this
// exact code.
//
// GX_RSPLIT=0        the kill switch: every call runs directly, as before (the same-binary control arm of an A/B).
// GX_RSPLIT_PROBE=1  one "[GXRSPLIT]" line per second: ticks, deferred calls per category, the deepest queue. Since step 6
//                    also one "[GXRSPART]" line per tick: what the particle and FX entry points APPLIED in that tick's
//                    window (see traceOp), printed by both arms from the same point, so they can be compared line by line.
// GX_RSPLIT_TEST_ARM=lifo    test only, an ordering fail arm: the queue is replayed last in, first out. Its [GXRSPART]
//                    lines must differ from the default arm's, or the trace cannot see a broken order.
// GX_RSPLIT_TEST_ARM=bypass  test only, a deferral fail arm: ParticleSystem::setPosition applies at once even on a
//                    published system called from the tick. Its [GXRSPART] byp= must be > 0 (notePhase), or the probe
//                    cannot see a missing deferral.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#ifndef GX_RENDER_SPLIT_TEST_ARM
#define GX_RENDER_SPLIT_TEST_ARM 0	// the bench's fail arms; always 0 in the game
#endif

class GXRenderSplit
{
public:
	// Only state the DRAW reads needs a category here. State that only the sync phase reads (ControlBar's dirty flag, EVA's
	// play requests: ControlBar::update and Eva::update run there, with the simulation parked) stays a direct write - the
	// barrier already orders it.
	// Step 3 (07/10/2026): superweapon timer entries, script named timers and the replay/spectator feed banners.
	// Step 6 (08/10/2026): Particle = publishing a particle system created in the window and operations on published ones
	// (ParticleSys.cpp); Fx = camera shakes, terrain scorch marks and light pulses (W3DView, BaseHeightMap, W3DDisplay).
	enum Category { FloatingText, Message, Superweapon, NamedTimer, Banner, Particle, Fx, CategoryCount };

	// The game's instance, switched by GX_RSPLIT / GX_RSPLIT_PROBE.
	static GXRenderSplit& get()
	{
		static GXRenderSplit s(envOn("GX_RSPLIT", true), envOn("GX_RSPLIT_PROBE", false), envIs("GX_RSPLIT_TEST_ARM", "lifo"),
			envIs("GX_RSPLIT_TEST_ARM", "bypass"));
		return s;
	}
	GXRenderSplit(bool enabled, bool probe, bool testLifo = false, bool testBypass = false)
		: m_enabled(enabled), m_probe(probe), m_testLifo(testLifo), m_testBypass(testBypass) {}

	// A logic tick starts (GameClient::setFrame, with the frame it runs). A -headless peer defers too: it draws nothing,
	// but then a headless [CRC] run exercises the same path a rendered peer does.
	void tickBegin(unsigned frame = 0)
	{
		// A second logic pass while the window is still open (time frozen: GameLogic::update opens it and returns before
		// GameClient::step, and nothing stops another pass on the same frame before the next client update) closes the
		// first window here: its calls replay before the second pass runs, as they would have applied in it directly,
		// and its [GXRSPART] line keeps its own serial.
		const bool secondPass = m_window;
		if (secondPass)
			tickEnd();
		m_windowFrame = frame;
#if GX_RENDER_SPLIT_TEST_ARM == 1
		m_inTick = false;	// fail arm: nothing is ever deferred
#else
		m_inTick = m_enabled;
#endif
		m_window = true;
		++m_ticks;
#if GX_RENDER_SPLIT_TEST_ARM == 5	// fail arm: a frozen second pass is not counted as a logic update
		if (!secondPass)
#endif
		++m_serial;
	}

	// Logic updates begun so far, frozen-time ones included (GameLogic::update calls GameClient::setFrame every time). One
	// tickBegin is one logic update and opens one window, so this is also the window serial: step 6 stamps a queued write
	// and its [GXRSPART] line with it, and step 4's pose publish (GXPosePublish.h) is due again when it moves.
	// The rule where the two steps meet: a frozen-time second pass on the same frame IS a new logic update - its scripts
	// can move what the publish reads (step 4's frozen-time class) - so it moves this serial; closing the first window
	// early (tickEnd above) only replays that window's render calls and moves nothing. The publish gates on "moved", not on
	// how far, so two passes before one client update give one walk there, after both, which is what the draw then reads.
	unsigned long tickSerial() const { return m_serial; }

	// The tick ended (GameClient::step), or the client is about to update with the window still open (time frozen;
	// GameClient::update, or updateHeadless on a -headless peer): replay the queue in call order. Idempotent; costs a branch when nothing was queued.
	void tickEnd()
	{
		m_inTick = false;
		const bool hadWindow = m_window;
		m_window = false;
		if (!m_queue.empty())
		{
			// A private list: a body may discard() (a reset) or, the window being closed, call another entry point directly.
			std::vector<Command> run;
			run.swap(m_queue);
			++m_drain;
#if GX_RENDER_SPLIT_TEST_ARM == 2
			std::reverse(run.begin(), run.end());	// fail arm: last in, first out
#endif
			if (m_testLifo)
				std::reverse(run.begin(), run.end());	// GX_RSPLIT_TEST_ARM=lifo, the in-game fail arm
			struct Replaying	// cleared on every way out of the loop
			{
				bool& flag;
				explicit Replaying(bool& f) : flag(f) { flag = true; }
				~Replaying() { flag = false; }
			} replaying(m_replaying);
			for (Command& c : run)
			{
				m_replayFrame = c.frame;
				c.body();
			}
			// Keep the storage for the next tick (a body may have discarded or, the window closed, queued nothing).
			if (m_queue.empty())
			{
				run.clear();
				m_queue.swap(run);
			}
		}
		if (m_probe)
		{
			if (hadWindow)
				traceTick();
			probe();
		}
	}

	// TRUE: the caller is the simulation side, so a converted entry point defers instead of running.
	bool simSide() const { return m_inTick; }

	// Record one call. frame: the logic frame the call would have read now.
	void defer(Category cat, unsigned frame, std::function<void()> body)
	{
		m_queue.push_back(Command{ frame, cat, std::move(body) });
		++m_counts[cat];
		if (m_queue.size() > m_maxQueue)
			m_maxQueue = m_queue.size();
	}

	// What a converted body uses instead of TheGameLogic->getFrame(): the frame captured at the call while replaying.
	unsigned logicFrame(unsigned live) const
	{
#if GX_RENDER_SPLIT_TEST_ARM == 3
		return live;	// fail arm: the replay reads the frame at the end of the tick
#else
		return m_replaying ? m_replayFrame : live;
#endif
	}
	bool replaying() const { return m_replaying; }
	// GX_RSPLIT_PROBE is on: converted entry points may print a trace of each change they commit, so the deferred and the
	// direct arm of one match can be compared line by line (the client state the [CRC] stream cannot see).
	bool probing() const { return m_probe; }

	// Between tickBegin and tickEnd, deferring or not (GX_RSPLIT=0 still opens the window): the step-6 trace records what
	// is applied here, and what the replay applies, so both arms trace the same set of calls.
	bool inWindow() const { return m_window; }
	bool traced() const { return m_probe && (m_window || m_replaying); }
	// Replays begun so far: a replayed command can tell an object published earlier in the SAME replay (it was still
	// private to the simulation side at the call) from one that was already published when it was called.
	unsigned long drain() const { return m_drain; }

	// The step-6 trace ([GXRSPART], probe only). An entry point records each change it APPLIES while traced(): the
	// object's ID, an op code and the op's argument bytes. Per window, the entries are stable-sorted by ID and hashed: equal
	// hashes mean every object got the same changes with the same arguments in the same order - between two runs whose
	// draws and particle emission ran on the same schedule (render-time creations take IDs and client-random draws too).
	// Publication order and the FX writes are hashed separately in apply order.
	static unsigned hashBytes(unsigned h, const void* data, size_t n)
	{
		const unsigned char* p = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < n; ++i)
			h = (h ^ p[i]) * 16777619u;
		return h;
	}
	void traceOp(unsigned label, unsigned op, const void* args, size_t n)
	{
		m_traceOps.push_back(TraceEntry{ label, hashBytes(hashBytes(2166136261u, &op, sizeof(op)), args, n) });
	}
	void tracePublish(unsigned label) { m_tracePub = hashBytes(m_tracePub, &label, sizeof(label)); ++m_tracePubs; }
	void traceFx(unsigned op, const void* args, size_t n)
	{
		m_traceFx = hashBytes(hashBytes(m_traceFx, &op, sizeof(op)), args, n);
		++m_traceFxs;
	}
	// A simulation-side read of a value an operation still queued in this window will change (DESIGN D4): expected 0.
	void noteStale() { ++m_stale; }

	// Where a converted write was APPLIED (probe only), per sink: dir = directly inside the window on a target still
	// private to the simulation side (a staged system), or anything with GX_RSPLIT=0; rep = replayed from the queue;
	// byp = directly on a target the draw already sees, called from the simulation side - a deferral that did not happen.
	// Expected byp=0 in every arm but GX_RSPLIT_TEST_ARM=bypass. The applied-op hash alone cannot see a bypass: nothing
	// in today's single-thread tick reads between the call and the replay, so an immediate write leaves the same trace.
	enum { PhaseSinks = 32 };
	void notePhase(unsigned sink, bool targetPublished)
	{
		if (!m_probe || !(m_window || m_replaying))
			return;
		const unsigned phase = m_replaying ? 1u : ((m_inTick && targetPublished) ? 2u : 0u);
		++m_phase[sink % PhaseSinks][phase];
		++m_phaseTick[phase];
	}
	// GX_RSPLIT_TEST_ARM=bypass (see the top of the file).
	bool testBypass() const { return m_testBypass; }

	// A game is being reset (InGameUI::reset): calls queued for the old game must not reach the next one.
	void discard()
	{
#if GX_RENDER_SPLIT_TEST_ARM != 4	// fail arm: the old game's calls survive a reset
		m_queue.clear();
#endif
		++m_resets;
	}
	// Resets seen so far (GameClient::reset -> InGameUI::reset: a new game, a save load, a replay restart). Render-side
	// bookkeeping keyed by drawable ID starts over when this moves (render-split step 4's GX_POSEPUB_CHECK shadows).
	unsigned long resetSerial() const { return m_resets; }
	size_t pending() const { return m_queue.size(); }

private:
	struct Command
	{
		unsigned frame;
		Category cat;
		std::function<void()> body;
	};

	struct TraceEntry
	{
		unsigned label;
		unsigned hash;
	};

	static bool envOn(const char* name, bool dflt)
	{
		const char* v = getenv(name);
		return (v == nullptr || *v == '\0') ? dflt : strcmp(v, "0") != 0;
	}
	static bool envIs(const char* name, const char* value)
	{
		const char* v = getenv(name);
		return v != nullptr && strcmp(v, value) == 0;
	}

	// One [GXRSPART] line for the window that just closed (probe only).
	void traceTick()
	{
		std::stable_sort(m_traceOps.begin(), m_traceOps.end(),
			[](const TraceEntry& a, const TraceEntry& b) { return a.label < b.label; });
		unsigned h = 2166136261u;
		for (const TraceEntry& e : m_traceOps)
		{
			h = hashBytes(h, &e.label, sizeof(e.label));
			h = hashBytes(h, &e.hash, sizeof(e.hash));
		}
		fprintf(stderr, "[GXRSPART] w=%lu f=%u ops=%lu pub=%lu h=%08x hp=%08x fx=%lu hf=%08x stale=%lu dir=%lu rep=%lu byp=%lu\n",
			m_serial, m_windowFrame, (unsigned long)m_traceOps.size(), m_tracePubs, h, m_tracePub, m_traceFxs, m_traceFx,
			m_stale, m_phaseTick[0], m_phaseTick[1], m_phaseTick[2]);
		m_traceOps.clear();
		m_tracePub = m_traceFx = 2166136261u;
		m_tracePubs = m_traceFxs = 0;
		m_stale = 0;
		m_phaseTick[0] = m_phaseTick[1] = m_phaseTick[2] = 0;
	}
	void probe()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now < m_nextProbe)
			return;
		m_nextProbe = now + std::chrono::seconds(1);
		fprintf(stderr, "[GXRSPLIT] ticks=%lu float=%lu msg=%lu sw=%lu timer=%lu banner=%lu part=%lu fx=%lu maxq=%lu\n", m_ticks,
			m_counts[FloatingText], m_counts[Message], m_counts[Superweapon], m_counts[NamedTimer], m_counts[Banner],
			m_counts[Particle], m_counts[Fx], (unsigned long)m_maxQueue);
		// Per-sink coverage since the start, dir/rep/byp (notePhase): which converted writes a match actually exercised.
		char line[1024];
		int n = snprintf(line, sizeof(line), "[GXRSPHASE]");
		for (unsigned s = 0; s < PhaseSinks && n > 0 && n < (int)sizeof(line) - 48; ++s)
		{
			if (m_phase[s][0] | m_phase[s][1] | m_phase[s][2])
				n += snprintf(line + n, sizeof(line) - n, " %u:%lu/%lu/%lu", s, m_phase[s][0], m_phase[s][1], m_phase[s][2]);
		}
		fprintf(stderr, "%s\n", line);
		m_ticks = 0;
		m_maxQueue = 0;
		for (unsigned long& c : m_counts)
			c = 0;
	}

	const bool m_enabled;
	const bool m_probe;
	const bool m_testLifo;
	const bool m_testBypass;
	bool m_inTick = false;
	bool m_window = false;
	bool m_replaying = false;
	unsigned m_replayFrame = 0;
	unsigned m_windowFrame = 0;
	unsigned long m_drain = 0;
	std::vector<Command> m_queue;
	unsigned long m_ticks = 0;
	unsigned long m_serial = 0;
	unsigned long m_resets = 0;
	unsigned long m_counts[CategoryCount] = {};
	size_t m_maxQueue = 0;
	std::chrono::steady_clock::time_point m_nextProbe{};
	// the step-6 trace (probe only)
	std::vector<TraceEntry> m_traceOps;
	unsigned m_tracePub = 2166136261u;
	unsigned m_traceFx = 2166136261u;
	unsigned long m_tracePubs = 0;
	unsigned long m_traceFxs = 0;
	unsigned long m_stale = 0;
	unsigned long m_phase[PhaseSinks][3] = {};
	unsigned long m_phaseTick[3] = {};
};
