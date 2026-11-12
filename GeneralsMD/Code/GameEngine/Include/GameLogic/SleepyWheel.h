/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// FILE: SleepyWheel.h ////////////////////////////////////////////////////////////////////////////
// The sleepy-update scheduler as a timing wheel.
//
// Replaces GameLogic's binary heap of sleepy update modules. Modules run in the TOTAL order (key, seq):
// key = frame<<2 | phase (UpdateModule::friend_getPriority, unchanged), seq = a counter stamped on each
// scheduling event. So every module wakes on exactly the frame and phase its key says, and modules with
// equal keys run first-scheduled-first. The heap ran equal keys in heap-layout order, which depends on
// every queued module: no wheel can reproduce it, so
// this is a one-time change of intra-frame order, deterministic on every platform (integers only; no
// address, pointer or hash ever decides an order).
//
// Containers (a scheduled item is in exactly one; Ops::where names it, Ops::index is its slot):
//   WHEEL   frames [base, base+WHEEL_FRAMES): one FIFO bucket per (frame slot, phase), a list of chunks.
//           FIFO == seq order inside a bucket: a frame only takes direct appends once it is inside the
//           window, and FAR entries move in (in (key, seq) order) the moment it enters.
//   LATE    frames < base (scheduled for an already drained frame): a (key, seq) min-heap; always due.
//   FAR     frames >= base+WHEEL_FRAMES, below FOREVER: a (key, seq) min-heap.
//   PARKED  frame == FOREVER (never due): an unordered list; a wake re-stamps seq, so its place is unused.
//   RUNNING the item popDue just returned, until the caller schedules or unschedules it.
//
// Header-only and engine-free (the engine's integer typedefs and Ops are the only inputs) so that
// tests/sleepywheel can drive it against a reference ordered set.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(GX_SLEEPY_ORACLE)
#include <map>
#include <set>
#endif

enum SleepyWhere
{
	SLEEPY_NONE = 0,
	SLEEPY_WHEEL,
	SLEEPY_LATE,
	SLEEPY_FAR,
	SLEEPY_PARKED,
	SLEEPY_RUNNING
};

// Ops supplies, as static functions on Item*:
//   UnsignedInt key(const Item*)       frame<<2 | phase
//   Int& index(Item*)                  -1 = not scheduled
//   void*& chunk(Item*)                the wheel chunk holding the item (WHEEL only)
//   UnsignedByte& where(Item*)         a SleepyWhere
//   UnsignedInt64& seq(Item*)
template <class Item, class Ops>
class SleepyWheel
{
public:
	enum
	{
		WHEEL_FRAMES = 2048,	// power of two; 34 s at 60 Hz
		PHASES = 4,
		CHUNK_CAP = 30				// 256-byte chunks on 64-bit
	};
	static const UnsignedInt FOREVER_FRAME = 0x3fffffff;	// UPDATE_SLEEP_FOREVER
	static const UnsignedInt NO_FRAME = 0xffffffff;

	struct Stats
	{
		UnsignedInt64 pops, schedules, tombstones, lateIn, farIn, farMigrated, leftovers, rewinds, compactions;
	};

	SleepyWheel() :
		m_buckets(new Bucket[WHEEL_FRAMES * PHASES]),
		m_free(nullptr),
		m_base(0),
		m_drainFrame(NO_FRAME),
		m_drainPhase(PHASES),
		m_seq(0),
		m_size(0),
		m_running(nullptr),
		m_chunksInUse(0),
		m_wheelSlots(0),
		m_wheelLive(0),
		m_compactSlack(16384)
	{
		memset(m_buckets, 0, sizeof(Bucket) * WHEEL_FRAMES * PHASES);
		memset(&m_stats, 0, sizeof(m_stats));
		memset(m_cursorChunk, 0, sizeof(m_cursorChunk));
		memset(m_cursorIdx, 0, sizeof(m_cursorIdx));
#if defined(GX_SLEEPY_ORACLE)
		m_oracleOn = false;
		m_breakFired = false;
		m_breakAt = 0;
		m_oraclePops = 0;
		m_oracleFails = 0;
		m_oracleChecks = 0;
#endif
	}

	// Frees memory only: never touches the items (they may already be gone at shutdown), as the old vector did not.
	~SleepyWheel()
	{
		for (size_t i = 0; i < m_allChunks.size(); ++i)
			delete m_allChunks[i];
		delete [] m_buckets;
	}

	Int size() const { return m_size; }
	UnsignedInt64 lastSeq() const { return m_seq; }
	const Stats& stats() const { return m_stats; }
	void resetStats() { memset(&m_stats, 0, sizeof(m_stats)); }
	Int lateSize() const { return (Int)m_late.size(); }
	Int farSize() const { return (Int)m_far.size(); }
	Int parkedSize() const { return (Int)m_parked.size(); }
	Int chunksInUse() const { return m_chunksInUse; }
	Int deadSlots() const { return m_wheelSlots - m_wheelLive; }
	// Bucket slots that hold no live item (tombstones, consumed entries) may exceed the live items by this much before
	// every bucket is compacted. Tests lower it to make compaction frequent.
	void setCompactSlack(Int slack) { m_compactSlack = slack; }

	// Puts u in the queue at its current key with a fresh seq; an item already queued (or RUNNING) moves.
	void schedule(Item* u)
	{
		if (Ops::where(u) == SLEEPY_NONE)
			++m_size;
		else
			detach(u);
		Ops::seq(u) = ++m_seq;
		++m_stats.schedules;
#if defined(GX_SLEEPY_ORACLE)
		oracleInsert(u);
#endif
		place(u);
		if (m_wheelSlots - m_wheelLive > m_wheelLive + m_compactSlack)
			compact();
	}

	// Takes u out of the queue (an item not queued is left alone).
	void unschedule(Item* u)
	{
		if (Ops::where(u) == SLEEPY_NONE)
			return;
		detach(u);
		--m_size;
		Ops::where(u) = SLEEPY_NONE;
		Ops::index(u) = -1;
		Ops::chunk(u) = nullptr;
	}

	// True iff u's recorded place holds u (the heap's "index mismatch" fatal check).
	bool isAtItsPlace(Item* u) const
	{
		const Int i = Ops::index(u);
		switch (Ops::where(u))
		{
			case SLEEPY_WHEEL: return i >= 0 && i < CHUNK_CAP && static_cast<Chunk*>(Ops::chunk(u))->e[i] == u;
			case SLEEPY_LATE: return i >= 0 && i < (Int)m_late.size() && m_late[i].u == u;
			case SLEEPY_FAR: return i >= 0 && i < (Int)m_far.size() && m_far[i].u == u;
			case SLEEPY_PARKED: return i >= 0 && i < (Int)m_parked.size() && m_parked[i] == u;
			case SLEEPY_RUNNING: return u == m_running;
			default: return false;
		}
	}

	// The next item due at 'now' in (key, seq) order, marked RUNNING; nullptr when nothing is due.
	// The caller then schedules it (its new key) or unschedules it.
	Item* popDue(UnsignedInt now)
	{
		if (now != m_drainFrame)
			beginFrame(now);

		Item* u = nullptr;
		if (!m_late.empty() && (m_late[0].key >> 2) <= now)
		{
			u = m_late[0].u;
			heapErase(m_late, 0);
		}
		else
		{
			for (; m_drainPhase < PHASES; ++m_drainPhase)
			{
				u = takeFromBucket(m_drainPhase);
				if (u)
					break;
			}
		}

		if (u)
		{
			Ops::where(u) = SLEEPY_RUNNING;
			Ops::index(u) = 0;
			Ops::chunk(u) = nullptr;
			m_running = u;
			++m_stats.pops;
		}
#if defined(GX_SLEEPY_ORACLE)
		if (u)
			++m_oraclePops;
		oraclePop(u, now);
#endif
		return u;
	}

	// Empties the queue: every item becomes unscheduled (index -1). seq restarts.
	void clear()
	{
		for (Int b = 0; b < WHEEL_FRAMES * PHASES; ++b)
		{
			for (Chunk* c = m_buckets[b].head; c; c = c->next)
				for (UnsignedInt i = 0; i < c->count; ++i)
					if (c->e[i])
						forget(c->e[i]);
			releaseBucket(m_buckets[b]);
		}
		for (size_t i = 0; i < m_late.size(); ++i)
			forget(m_late[i].u);
		for (size_t i = 0; i < m_far.size(); ++i)
			forget(m_far[i].u);
		for (size_t i = 0; i < m_parked.size(); ++i)
			forget(m_parked[i]);
		if (m_running)
			forget(m_running);
		m_late.clear();
		m_far.clear();
		m_parked.clear();
		m_running = nullptr;
		m_size = 0;
		m_seq = 0;
		m_wheelSlots = 0;
		m_wheelLive = 0;
		m_drainFrame = NO_FRAME;
		m_drainPhase = PHASES;
		memset(m_cursorChunk, 0, sizeof(m_cursorChunk));
		memset(m_cursorIdx, 0, sizeof(m_cursorIdx));
#if defined(GX_SLEEPY_ORACLE)
		m_oracle.clear();
		m_oracleOf.clear();
#endif
	}

	// Sets the window to start at 'now'. The queue must be empty (after clear()).
	void rebase(UnsignedInt now)
	{
		m_base = now;
		m_drainFrame = NO_FRAME;
		m_drainPhase = PHASES;
		memset(m_cursorChunk, 0, sizeof(m_cursorChunk));
		memset(m_cursorIdx, 0, sizeof(m_cursorIdx));
	}

	// Calls f(item) for every queued item, in no particular order (reset, debugging).
	template <class F> void forEach(F f) const
	{
		for (Int b = 0; b < WHEEL_FRAMES * PHASES; ++b)
			for (const Chunk* c = m_buckets[b].head; c; c = c->next)
				for (UnsignedInt i = 0; i < c->count; ++i)
					if (c->e[i])
						f(c->e[i]);
		for (size_t i = 0; i < m_late.size(); ++i)
			f(m_late[i].u);
		for (size_t i = 0; i < m_far.size(); ++i)
			f(m_far[i].u);
		for (size_t i = 0; i < m_parked.size(); ++i)
			f(m_parked[i]);
		if (m_running)
			f(m_running);
	}

	// Checks every invariant; returns the number of violations (prints the first few). Slow: tests and debug only.
	Int validate() const
	{
		Int bad = 0;
		Int count = 0;
		for (Int b = 0; b < WHEEL_FRAMES * PHASES; ++b)
		{
			const UnsignedInt slot = (UnsignedInt)b / PHASES;
			const UnsignedInt phase = (UnsignedInt)b % PHASES;
			UnsignedInt64 lastSeq = 0;
			const Chunk* last = nullptr;
			for (const Chunk* c = m_buckets[b].head; c; c = c->next)
			{
				last = c;
				for (UnsignedInt i = 0; i < c->count; ++i)
				{
					Item* u = c->e[i];
					if (!u)
						continue;
					++count;
					const UnsignedInt k = Ops::key(u);
					const UnsignedInt f = k >> 2;
					bad += check(Ops::where(u) == SLEEPY_WHEEL && Ops::chunk(u) == c && Ops::index(u) == (Int)i, "wheel place");
					bad += check((k & 3) == phase && (f & (WHEEL_FRAMES - 1)) == slot, "wheel bucket");
					bad += check(f >= m_base && f - m_base < (UnsignedInt)WHEEL_FRAMES, "wheel window");
					bad += check(Ops::seq(u) > lastSeq, "wheel fifo");
					lastSeq = Ops::seq(u);
				}
			}
			bad += check(m_buckets[b].tail == last, "bucket tail");
		}
		for (size_t i = 0; i < m_late.size(); ++i)
		{
			++count;
			bad += check(Ops::where(m_late[i].u) == SLEEPY_LATE && Ops::index(m_late[i].u) == (Int)i, "late place");
			bad += check(m_late[i].key == Ops::key(m_late[i].u) && m_late[i].seq == Ops::seq(m_late[i].u), "late mirror");
			bad += check((m_late[i].key >> 2) < m_base || m_drainFrame == NO_FRAME, "late frame");
			bad += check(i == 0 || !less(m_late[i], m_late[(i - 1) / 2]), "late heap");
		}
		for (size_t i = 0; i < m_far.size(); ++i)
		{
			++count;
			bad += check(Ops::where(m_far[i].u) == SLEEPY_FAR && Ops::index(m_far[i].u) == (Int)i, "far place");
			bad += check(m_far[i].key == Ops::key(m_far[i].u) && m_far[i].seq == Ops::seq(m_far[i].u), "far mirror");
			bad += check((m_far[i].key >> 2) >= m_base + WHEEL_FRAMES && (m_far[i].key >> 2) < FOREVER_FRAME, "far frame");
			bad += check(i == 0 || !less(m_far[i], m_far[(i - 1) / 2]), "far heap");
		}
		for (size_t i = 0; i < m_parked.size(); ++i)
		{
			++count;
			bad += check(Ops::where(m_parked[i]) == SLEEPY_PARKED && Ops::index(m_parked[i]) == (Int)i, "parked place");
			bad += check((Ops::key(m_parked[i]) >> 2) >= FOREVER_FRAME, "parked frame");
		}
		if (m_running)
			++count;
		bad += check(count == m_size, "size");
		Int slots = 0, live = 0;
		for (Int b = 0; b < WHEEL_FRAMES * PHASES; ++b)
			for (const Chunk* c = m_buckets[b].head; c; c = c->next)
			{
				slots += (Int)c->count;
				bad += check(c->next == nullptr || c->count == CHUNK_CAP, "chunk full");
				for (UnsignedInt i = 0; i < c->count; ++i)
					live += c->e[i] ? 1 : 0;
			}
		bad += check(slots == m_wheelSlots && live == m_wheelLive, "slot accounting");
		return bad;
	}

#if defined(GX_SLEEPY_ORACLE)
	// Test instrument: mirror every operation into an ordered set on (key, seq) and check each pop against it.
	void setOracle(bool on, UnsignedInt64 breakAtPop) { m_oracleOn = on; m_breakAt = breakAtPop; }
	bool oracleOn() const { return m_oracleOn; }
	UnsignedInt64 oracleFails() const { return m_oracleFails; }
	UnsignedInt64 oracleChecks() const { return m_oracleChecks; }
#endif

private:
	struct Chunk
	{
		Chunk* next;
		UnsignedInt count;
		UnsignedInt pad;
		Item* e[CHUNK_CAP];
	};
	struct Bucket
	{
		Chunk* head;
		Chunk* tail;
	};
	struct HeapEntry
	{
		UnsignedInt key;
		UnsignedInt64 seq;
		Item* u;
	};

	static bool less(const HeapEntry& a, const HeapEntry& b)
	{
		return a.key < b.key || (a.key == b.key && a.seq < b.seq);
	}

	static Int check(bool ok, const char* what)
	{
		if (!ok)
			fprintf(stderr, "[SLEEPYWHEEL] invariant broken: %s\n", what);
		return ok ? 0 : 1;
	}

	Bucket& bucketOf(UnsignedInt key) { return m_buckets[((key >> 2) & (WHEEL_FRAMES - 1)) * PHASES + (key & 3)]; }

	// Marks u unscheduled without touching its container (clear() drops the containers wholesale).
	static void forget(Item* u)
	{
		Ops::where(u) = SLEEPY_NONE;
		Ops::index(u) = -1;
		Ops::chunk(u) = nullptr;
	}

	// Removes u from its container; leaves where/index for the caller to overwrite.
	void detach(Item* u)
	{
#if defined(GX_SLEEPY_ORACLE)
		oracleErase(u);
#endif
		const Int i = Ops::index(u);
		switch (Ops::where(u))
		{
			case SLEEPY_WHEEL:
				static_cast<Chunk*>(Ops::chunk(u))->e[i] = nullptr;
				--m_wheelLive;
				++m_stats.tombstones;
				break;
			case SLEEPY_LATE:
				heapErase(m_late, i);
				break;
			case SLEEPY_FAR:
				heapErase(m_far, i);
				break;
			case SLEEPY_PARKED:
			{
				Item* last = m_parked.back();
				m_parked[i] = last;
				Ops::index(last) = i;
				m_parked.pop_back();
				break;
			}
			case SLEEPY_RUNNING:
				m_running = nullptr;
				break;
			default:
				break;
		}
	}

	// Puts u (seq already stamped) into the container its key belongs to.
	void place(Item* u)
	{
		const UnsignedInt k = Ops::key(u);
		const UnsignedInt f = k >> 2;
		if (f >= FOREVER_FRAME)
		{
			Ops::where(u) = SLEEPY_PARKED;
			Ops::index(u) = (Int)m_parked.size();
			Ops::chunk(u) = nullptr;
			m_parked.push_back(u);
		}
		else if (f < m_base)
		{
			heapPush(m_late, SLEEPY_LATE, u);
			++m_stats.lateIn;
		}
		else if (f - m_base >= (UnsignedInt)WHEEL_FRAMES)
		{
			heapPush(m_far, SLEEPY_FAR, u);
			++m_stats.farIn;
		}
		else
		{
			append(u, k);
		}
	}

	void append(Item* u, UnsignedInt k)
	{
		Bucket& b = bucketOf(k);
		Chunk* t = b.tail;
		if (!t || t->count == CHUNK_CAP)
		{
			Chunk* n = allocChunk();
			if (t)
				t->next = n;
			else
				b.head = n;
			b.tail = t = n;
		}
		const UnsignedInt i = t->count++;
		t->e[i] = u;
		Ops::where(u) = SLEEPY_WHEEL;
		Ops::index(u) = (Int)i;
		Ops::chunk(u) = t;
		++m_wheelSlots;
		++m_wheelLive;
		// Phase rewind: an item due now in a phase the drain already passed runs next (its key is the smallest).
		if ((k >> 2) == m_drainFrame && (Int)(k & 3) < m_drainPhase)
		{
			m_drainPhase = (Int)(k & 3);
			++m_stats.rewinds;
		}
	}

	// The next live item of bucket (m_drainFrame, phase) after the drain cursor; consumes its slot.
	Item* takeFromBucket(Int phase)
	{
		Bucket& b = m_buckets[(m_drainFrame & (WHEEL_FRAMES - 1)) * PHASES + phase];
		Chunk* c = m_cursorChunk[phase] ? m_cursorChunk[phase] : b.head;
		UnsignedInt i = m_cursorChunk[phase] ? m_cursorIdx[phase] : 0;
#if defined(GX_SLEEPY_ORACLE)
		// Red arm (GX_HEAP_ORACLE_BREAK=n): at the first pop >= n whose bucket holds two live items, hand out the
		// second and leave the first in place, once. The oracle must report it.
		// m_oraclePops, not m_stats.pops: GX_HEAP_STATS resets the stats every 600 frames.
		const bool breakHere = m_breakAt != 0 && !m_breakFired && m_oraclePops + 1 >= m_breakAt;
		Chunk* firstC = nullptr;
		UnsignedInt firstI = 0;
#endif
		while (c)
		{
			for (; i < c->count; ++i)
			{
				Item* u = c->e[i];
				if (!u)
					continue;
#if defined(GX_SLEEPY_ORACLE)
				if (breakHere && !firstC)
				{
					firstC = c;
					firstI = i;
					continue;
				}
				if (firstC)
				{
					c->e[i] = nullptr;
					--m_wheelLive;
					m_cursorChunk[phase] = firstC;
					m_cursorIdx[phase] = firstI;
					m_breakFired = true;
					return u;
				}
#endif
				c->e[i] = nullptr;
				--m_wheelLive;
				m_cursorChunk[phase] = c;
				m_cursorIdx[phase] = i + 1;
				return u;
			}
			if (!c->next)
				break;
			c = c->next;
			i = 0;
		}
#if defined(GX_SLEEPY_ORACLE)
		if (firstC)
		{
			// One live item only: nothing to swap it with; take it.
			Item* u = firstC->e[firstI];
			firstC->e[firstI] = nullptr;
			--m_wheelLive;
			m_cursorChunk[phase] = firstC;
			m_cursorIdx[phase] = firstI + 1;
			return u;
		}
#endif
		m_cursorChunk[phase] = c;
		m_cursorIdx[phase] = i;
		return nullptr;
	}

	// Drops every dead slot (tombstone, consumed entry) from every bucket, keeping each bucket's order; rewrites the
	// moved items' places. Dead slots before a drain cursor are all that lies before it, so the cursors restart at
	// the heads. Amortised O(1): it runs only after the dead slots outnumber the live ones by m_compactSlack.
	void compact()
	{
		for (Int b = 0; b < WHEEL_FRAMES * PHASES; ++b)
		{
			Bucket& bk = m_buckets[b];
			if (!bk.head)
				continue;
			Chunk* w = bk.head;
			UnsignedInt wi = 0;
			Int written = 0;
			for (Chunk* c = bk.head; c; c = c->next)
			{
				for (UnsignedInt i = 0; i < c->count; ++i)
				{
					Item* u = c->e[i];
					if (!u)
						continue;
					if (wi == CHUNK_CAP)
					{
						w = w->next;	// never passes c: at most as many items written as read
						wi = 0;
					}
					w->e[wi] = u;
					Ops::chunk(u) = w;
					Ops::index(u) = (Int)wi;
					++wi;
					++written;
				}
			}
			if (written == 0)
			{
				releaseBucket(bk);
				continue;
			}
			Chunk* rest = w->next;
			w->next = nullptr;
			m_wheelSlots -= (Int)w->count - (Int)wi;	// the chunks before w were full and stay full
			w->count = wi;
			bk.tail = w;
			while (rest)
			{
				Chunk* n = rest->next;
				m_wheelSlots -= (Int)rest->count;
				rest->next = m_free;
				m_free = rest;
				--m_chunksInUse;
				rest = n;
			}
		}
		memset(m_cursorChunk, 0, sizeof(m_cursorChunk));
		memset(m_cursorIdx, 0, sizeof(m_cursorIdx));
		++m_stats.compactions;
	}

	// Advances the window to 'now': undrained items of the frames it leaves go to LATE (they are overdue),
	// FAR items whose frame enters the window go to their buckets in (key, seq) order.
	void beginFrame(UnsignedInt now)
	{
		while (m_base < now)
		{
			const UnsignedInt slot = m_base & (WHEEL_FRAMES - 1);
			for (Int p = 0; p < PHASES; ++p)
			{
				Bucket& b = m_buckets[slot * PHASES + p];
				for (Chunk* c = b.head; c; c = c->next)
				{
					for (UnsignedInt i = 0; i < c->count; ++i)
					{
						Item* u = c->e[i];
						if (!u)
							continue;
						heapPush(m_late, SLEEPY_LATE, u);
						--m_wheelLive;
						++m_stats.leftovers;
					}
				}
				releaseBucket(b);
			}
			++m_base;
			migrateFar();
		}
		m_drainFrame = now;
		m_drainPhase = now >= m_base ? 0 : PHASES;	// a frame before the window has nothing in the buckets (its slot is another frame's)
		memset(m_cursorChunk, 0, sizeof(m_cursorChunk));
		memset(m_cursorIdx, 0, sizeof(m_cursorIdx));
	}

	void migrateFar()
	{
		const UnsignedInt end = m_base + WHEEL_FRAMES;
		while (!m_far.empty() && (m_far[0].key >> 2) < end)
		{
			Item* u = m_far[0].u;
			const UnsignedInt k = m_far[0].key;
			heapErase(m_far, 0);
			++m_stats.farMigrated;
			if ((k >> 2) < m_base)
				heapPush(m_late, SLEEPY_LATE, u);
			else
				append(u, k);
		}
	}

	Chunk* allocChunk()
	{
		Chunk* c = m_free;
		if (c)
			m_free = c->next;
		else
		{
			c = new Chunk;
			m_allChunks.push_back(c);
		}
		c->next = nullptr;
		c->count = 0;
		++m_chunksInUse;
		return c;
	}

	void releaseBucket(Bucket& b)
	{
		Chunk* c = b.head;
		while (c)
		{
			Chunk* n = c->next;
			m_wheelSlots -= (Int)c->count;
			c->next = m_free;
			m_free = c;
			--m_chunksInUse;
			c = n;
		}
		b.head = b.tail = nullptr;
	}

	// (key, seq) min-heap with each item's index kept in Ops::index.
	void heapPush(std::vector<HeapEntry>& h, UnsignedByte where, Item* u)
	{
		HeapEntry e;
		e.key = Ops::key(u);
		e.seq = Ops::seq(u);
		e.u = u;
		h.push_back(e);
		Ops::where(u) = where;
		Ops::chunk(u) = nullptr;
		Ops::index(u) = (Int)h.size() - 1;
		heapUp(h, (Int)h.size() - 1);
	}

	void heapErase(std::vector<HeapEntry>& h, Int i)
	{
		const Int last = (Int)h.size() - 1;
		if (i != last)
		{
			h[i] = h[last];
			Ops::index(h[i].u) = i;
			h.pop_back();
			heapDown(h, heapUp(h, i));
		}
		else
		{
			h.pop_back();
		}
	}

	static Int heapUp(std::vector<HeapEntry>& h, Int i)
	{
		while (i > 0)
		{
			const Int p = (i - 1) / 2;
			if (!less(h[i], h[p]))
				break;
			std::swap(h[i], h[p]);
			Ops::index(h[i].u) = i;
			Ops::index(h[p].u) = p;
			i = p;
		}
		return i;
	}

	static void heapDown(std::vector<HeapEntry>& h, Int i)
	{
		const Int n = (Int)h.size();
		for (;;)
		{
			Int c = 2 * i + 1;
			if (c >= n)
				break;
			if (c + 1 < n && less(h[c + 1], h[c]))
				++c;
			if (!less(h[c], h[i]))
				break;
			std::swap(h[i], h[c]);
			Ops::index(h[i].u) = i;
			Ops::index(h[c].u) = c;
			i = c;
		}
	}

#if defined(GX_SLEEPY_ORACLE)
	struct OKey
	{
		UnsignedInt key;
		UnsignedInt64 seq;
		bool operator<(const OKey& o) const { return key < o.key || (key == o.key && seq < o.seq); }
	};

	void oracleInsert(Item* u)
	{
		if (!m_oracleOn)
			return;
		OKey k;
		k.key = Ops::key(u);
		k.seq = Ops::seq(u);
		m_oracle.insert(std::make_pair(k, u));
		m_oracleOf[u] = k;
	}

	void oracleErase(Item* u)
	{
		if (!m_oracleOn)
			return;
		typename std::map<Item*, OKey>::iterator it = m_oracleOf.find(u);
		if (it == m_oracleOf.end())
			return;
		m_oracle.erase(std::make_pair(it->second, u));
		m_oracleOf.erase(it);
	}

	void oraclePop(Item* u, UnsignedInt now)
	{
		if (!m_oracleOn)
			return;
		++m_oracleChecks;
		const bool anyDue = !m_oracle.empty() && (m_oracle.begin()->first.key >> 2) <= now;
		if (!u)
		{
			if (anyDue)
				oracleFail(now, "due item left behind", nullptr, m_oracle.begin()->second);
			return;
		}
		if (!anyDue || m_oracle.begin()->second != u)
			oracleFail(now, "wrong item", u, anyDue ? m_oracle.begin()->second : nullptr);
		oracleErase(u);
	}

	void oracleFail(UnsignedInt now, const char* what, Item* got, Item* want)
	{
		++m_oracleFails;
		if (m_oracleFails <= 20)
		{
			fprintf(stderr, "[GXHEAPORACLE] FAIL f=%u %s got=(key %u.%u seq %llu) want=(key %u.%u seq %llu)\n", now, what,
				got ? Ops::key(got) >> 2 : 0, got ? Ops::key(got) & 3 : 0, got ? (unsigned long long)Ops::seq(got) : 0ULL,
				want ? Ops::key(want) >> 2 : 0, want ? Ops::key(want) & 3 : 0, want ? (unsigned long long)Ops::seq(want) : 0ULL);
			fflush(stderr);
		}
	}

	bool m_oracleOn;
	bool m_breakFired;
	UnsignedInt64 m_breakAt;
	UnsignedInt64 m_oraclePops;	// every pop since construction, never reset (the red arm's clock)
	UnsignedInt64 m_oracleFails;
	UnsignedInt64 m_oracleChecks;
	std::set<std::pair<OKey, Item*> > m_oracle;
	std::map<Item*, OKey> m_oracleOf;
#endif

	Bucket* m_buckets;
	Chunk* m_free;
	std::vector<Chunk*> m_allChunks;
	std::vector<HeapEntry> m_late;
	std::vector<HeapEntry> m_far;
	std::vector<Item*> m_parked;
	UnsignedInt m_base;					// first frame of the wheel window
	UnsignedInt m_drainFrame;		// the frame popDue is draining (cursors valid), NO_FRAME before the first
	Int m_drainPhase;						// lowest phase of m_drainFrame that may still hold due items
	Chunk* m_cursorChunk[PHASES];
	UnsignedInt m_cursorIdx[PHASES];
	UnsignedInt64 m_seq;
	Int m_size;
	Item* m_running;
	Int m_chunksInUse;
	Int m_wheelSlots;						// slots used in all buckets (live + dead)
	Int m_wheelLive;						// live items in all buckets
	Int m_compactSlack;
	Stats m_stats;
};
