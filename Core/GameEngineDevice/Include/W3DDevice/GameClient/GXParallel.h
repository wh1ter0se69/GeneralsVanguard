/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @performance Claude 07/10/2026 A render-side fork-join pool (modernize/render-split PLAN_pose-prepass,
// smooth/MT_DESIGN's "fixed-partition fork-join on std::thread"). gxParallelFor(count, chunk, fn, ctx) runs
// fn(ctx, begin, end) over [0, count) in chunks of `chunk`, on persistent workers plus the calling thread, and returns
// once every chunk has run; everything the jobs wrote is then visible to the caller (each finished chunk is counted
// with a release add, the caller waits for the count with acquire loads). The caller does not wait for a worker that
// woke too late to find a chunk: the NEXT dispatch waits for every worker to check in before it hands out new chunks,
// so a late worker can never run old work on new indices. One dispatch at a time (one calling thread); a dispatch from
// inside a job runs inline on that thread.
// RENDER SIDE ONLY: a job must never touch simulation state, and the order the chunks run in is not fixed.
// Workers: min(hardware threads - 1, 7), or GX_PARALLEL_WORKERS (0-15); 0 runs everything on the caller.
// Test: scripts/test/gx-parallel-test.sh.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

typedef void (*GxParallelFn)(void *ctx, int begin, int end);

namespace gxparallel_detail
{
inline thread_local bool t_inJob = false;

struct Pool
{
	std::mutex m;
	std::condition_variable wake;	// workers wait for a new generation
	std::condition_variable idle;	// the caller waits for busy == 0
	unsigned generation = 0;
	int busy = 0;
	GxParallelFn fn = nullptr;
	void *ctx = nullptr;
	int count = 0;
	int chunk = 1;
	std::atomic<int> next{ 0 };
	std::atomic<int> done{ 0 };		// chunks finished in the current generation
	int workers = 0;

	void runChunks(GxParallelFn f, void *c, int n, int ch)
	{
		for (;;)
		{
			const int begin = next.fetch_add(ch, std::memory_order_relaxed);
			if (begin >= n)
				return;
			f(c, begin, std::min(n, begin + ch));
			done.fetch_add(1, std::memory_order_release);
		}
	}

	void workerLoop()
	{
		t_inJob = true;
		unsigned seen = 0;
		for (;;)
		{
			GxParallelFn f;
			void *c;
			int n, ch;
			{
				std::unique_lock<std::mutex> lk(m);
				wake.wait(lk, [&] { return generation != seen; });
				seen = generation;
				f = fn; c = ctx; n = count; ch = chunk;
			}
			runChunks(f, c, n, ch);
			{
				std::lock_guard<std::mutex> lk(m);
				if (--busy == 0)
					idle.notify_one();
			}
		}
	}

	// Never throws (Codex 0.9.60 r10 P2): a worker that cannot start (std::system_error at a thread or memory limit) ends
	// the start-up, and the pool runs with the workers that did start - 0 means every dispatch runs on the caller. A throw
	// out of here would have freed the Pool under the workers already started, and reached the fatal handler.
	Pool()
	{
		const char *v = getenv("GX_PARALLEL_WORKERS");
		const unsigned hw = std::thread::hardware_concurrency();
		const int want = v != nullptr ? std::max(0, std::min(15, atoi(v))) : (hw > 1 ? (int)std::min(hw - 1, 7u) : 0);
		for (int i = 0; i < want; ++i)
		{
			try
			{
#ifdef GX_PARALLEL_TEST_THROW_AT	// scripts/test/gx-parallel-test.sh: the start-up failure, forced
				if (i == GX_PARALLEL_TEST_THROW_AT)
					throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
				std::thread([this] { workerLoop(); }).detach();
			}
			catch (...)
			{
				break;
			}
			workers = i + 1;
		}
	}
};

// ponytail: the pool is never destroyed - its workers park on a condition variable until the process exits, so no
// join can hang in a static destructor at exit (Windows ends other threads before those run). nullptr when even the
// Pool could not be allocated: every dispatch then runs on the caller.
inline Pool *pool()
{
	static Pool *p = new (std::nothrow) Pool;
	return p;
}
} // namespace gxparallel_detail

inline int gxParallelWorkers()
{
	gxparallel_detail::Pool *p = gxparallel_detail::pool();
	return p != nullptr ? p->workers : 0;
}

inline void gxParallelFor(int count, int chunk, GxParallelFn fn, void *ctx)
{
	using namespace gxparallel_detail;
	if (count <= 0)
		return;
	if (chunk < 1)
		chunk = 1;
	Pool *pp = pool();
	if (t_inJob || pp == nullptr || pp->workers == 0 || count <= chunk)
	{
		for (int begin = 0; begin < count; begin += chunk)
			fn(ctx, begin, std::min(count, begin + chunk));
		return;
	}
	Pool &p = *pp;
	{
		std::unique_lock<std::mutex> lk(p.m);
		p.idle.wait(lk, [&] { return p.busy == 0; });	// the previous dispatch's late workers have checked in
		p.fn = fn;
		p.ctx = ctx;
		p.count = count;
		p.chunk = chunk;
		p.next.store(0, std::memory_order_relaxed);
		p.done.store(0, std::memory_order_relaxed);
		p.busy = p.workers;
		++p.generation;
	}
	p.wake.notify_all();
	t_inJob = true;
	p.runChunks(fn, ctx, count, chunk);
	t_inJob = false;
	const int chunks = (count + chunk - 1) / chunk;
	while (p.done.load(std::memory_order_acquire) < chunks)	// only chunks other workers are still running
		std::this_thread::yield();
}
