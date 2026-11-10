// GeneralsX: PF27 - the open list of the new pathfinding kernel (fixes/pathfinding-2027).
//
// The legacy open list (PathfindCell::forwardInsertionSort / reverseInsertionSort / removeFromOpenList in
// AIPathfind.cpp) keeps cells sorted by total cost, and a cell lands AFTER every cell of equal cost, so among equal
// costs the order is the order of (latest) insertion. Popping its head therefore pops the minimum of
// (cost, insertion order). This queue pops exactly the same node: one FIFO list per 16-bit cost value, and a
// two-level bitmap to find the lowest non-empty cost. Insert, remove and pop are O(1); nothing walks a list.
// scripts/test/pf27-bucket-queue-test.sh drives this header and a verbatim model of the legacy list with the same
// random operations and requires the same pop sequence.
//
// Integer only, no pointers in any decision, no allocation after init(): deterministic on every platform.

#pragma once

#include <cstdint>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

class PF27BucketQueue
{
public:
	enum { NUM_COSTS = 65536, LO_WORDS = NUM_COSTS / 64, HI_WORDS = LO_WORDS / 64 };

	PF27BucketQueue() : m_capacity(0), m_size(0), m_next(nullptr), m_prev(nullptr), m_cost(nullptr),
		m_head(nullptr), m_tail(nullptr) { memset(m_lo, 0, sizeof(m_lo)); memset(m_hi, 0, sizeof(m_hi)); }
	~PF27BucketQueue() { release(); }

	/// Node ids are 0..capacity-1. Throws (operator new) on allocation failure, like the rest of the engine.
	void init(int32_t capacity)
	{
		release();
		m_capacity = capacity;
		m_next = new int32_t[capacity];
		m_prev = new int32_t[capacity];
		m_cost = new uint16_t[capacity];
		m_head = new int32_t[NUM_COSTS];
		m_tail = new int32_t[NUM_COSTS];
		clear();
	}

	void release()
	{
		delete[] m_next; delete[] m_prev; delete[] m_cost; delete[] m_head; delete[] m_tail;
		m_next = m_prev = m_head = m_tail = nullptr;
		m_cost = nullptr;
		m_capacity = 0;
		m_size = 0;
	}

	/// Empties the queue. A bucket's head/tail are only read while its bit is set, so only the bitmaps are cleared.
	void clear()
	{
		memset(m_lo, 0, sizeof(m_lo));
		memset(m_hi, 0, sizeof(m_hi));
		m_size = 0;
	}

	bool empty() const { return m_size == 0; }
	int32_t size() const { return m_size; }

	/// Appends node at the tail of its cost's bucket: after every node already queued at that cost.
	void push(int32_t node, uint16_t cost)
	{
		m_cost[node] = cost;
		m_next[node] = -1;
		if (isSet(cost))
		{
			const int32_t t = m_tail[cost];
			m_prev[node] = t;
			m_next[t] = node;
			m_tail[cost] = node;
		}
		else
		{
			m_prev[node] = -1;
			m_head[cost] = node;
			m_tail[cost] = node;
			setBit(cost);
		}
		++m_size;
	}

	/// Unlinks node from the bucket of the cost it was pushed with. The node must be queued.
	void remove(int32_t node)
	{
		const uint16_t cost = m_cost[node];
		const int32_t p = m_prev[node];
		const int32_t n = m_next[node];
		if (p >= 0) m_next[p] = n; else m_head[cost] = n;
		if (n >= 0) m_prev[n] = p; else m_tail[cost] = p;
		if (m_head[cost] < 0)
			clearBit(cost);
		--m_size;
	}

	/// Removes and returns the head of the lowest non-empty bucket, or -1 when empty.
	int32_t pop()
	{
		if (m_size == 0)
			return -1;
		for (int32_t w = 0; w < HI_WORDS; ++w)
		{
			if (m_hi[w] == 0)
				continue;
			const int32_t lo = w * 64 + ctz64(m_hi[w]);
			const int32_t cost = lo * 64 + ctz64(m_lo[lo]);
			const int32_t node = m_head[cost];
			remove(node);
			return node;
		}
		return -1;	// unreachable while m_size is consistent
	}

	uint16_t costOf(int32_t node) const { return m_cost[node]; }

private:

	static inline int32_t ctz64(uint64_t v)
	{
#if defined(_MSC_VER)
		unsigned long i;
		_BitScanForward64(&i, v);
		return (int32_t)i;
#else
		return (int32_t)__builtin_ctzll(v);
#endif
	}

	bool isSet(uint16_t c) const { return (m_lo[c >> 6] >> (c & 63)) & 1u; }
	void setBit(uint16_t c)
	{
		m_lo[c >> 6] |= (uint64_t)1 << (c & 63);
		m_hi[c >> 12] |= (uint64_t)1 << ((c >> 6) & 63);
	}
	void clearBit(uint16_t c)
	{
		m_lo[c >> 6] &= ~((uint64_t)1 << (c & 63));
		if (m_lo[c >> 6] == 0)
			m_hi[c >> 12] &= ~((uint64_t)1 << ((c >> 6) & 63));
	}

	int32_t m_capacity;
	int32_t m_size;
	int32_t *m_next;
	int32_t *m_prev;
	uint16_t *m_cost;
	int32_t *m_head;	///< per cost: first node, valid only while the cost's bit is set
	int32_t *m_tail;	///< per cost: last node, valid only while the cost's bit is set
	uint64_t m_lo[LO_WORDS];	///< bit c: cost c has a node
	uint64_t m_hi[HI_WORDS];	///< bit w: m_lo[w] != 0
};
