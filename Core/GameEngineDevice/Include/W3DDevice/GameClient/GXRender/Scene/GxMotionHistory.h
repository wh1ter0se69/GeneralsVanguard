// GeneralsX: What each mesh drew with in the previous world pass, for per-object motion
// vectors (rigid, §2.3 skinned). Header-only and free of engine types so the rules are
// unit-tested with plain c++ (Scene/tests/gx_motion_history_test.cpp). Keys are mesh addresses, frames are world-pass numbers.
// Render-only: nothing here is read by the simulation, and it lives outside every digested path.
//
// Trade-off: keyed by address with no destructor hook (mesh.cpp is digested), plus a TAG (the mesh's model): an address reused by a
// mesh of another model starts a new history; reused across >= 2 passes, the frame number catches it. What remains is a mesh of the
// SAME model reusing the address between two adjacent passes = one frame of wrong velocity on it, absorbed by every upscaler's history
// rejection. Reuse INSIDE a pass cannot happen: meshes are created and destroyed by the game's update, never during a world pass's
// draws. Upgrade path: a generation id if a picture ever shows it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace GXRender {

// GX_HIST_MUTANT=<n> (test-only builds): plant n breaks exactly one rule below so gx_motion_history_test must fail.
#ifndef GX_HIST_MUTANT
#define GX_HIST_MUTANT 0
#endif

// Rigid meshes: the world matrix (12 floats, compared bitwise) of the previous pass.
class GxRigidHistory
{
public:
	// `key` draws in pass `frame` with world `cur`. True when it drew in pass frame-1 with a different matrix; prevOut then holds that
	// matrix. A second call in the same pass (another texture category or pass of the same mesh) returns the first call's answer.
	bool update(const void* key, const void* tag, const float cur[12], uint32_t frame, float prevOut[12])
	{
		bool inserted = false;
		Entry& e = slot(key, &inserted);
		if (!inserted && e.tag != tag && GX_HIST_MUTANT != 9) {   // another mesh (model) at this address: a new history
			e.frame = frame - 2;   // not adjacent: the rule below makes it "not moved"
			e.moved = false;
		}
		e.tag = tag;
		if (inserted) {
			e.frame = frame;
			std::memcpy(e.cur, cur, sizeof(e.cur));
			e.moved = GX_HIST_MUTANT == 1;
			return e.moved;
		}
		if (e.frame != frame || GX_HIST_MUTANT == 3) {
			const bool adjacent = e.frame + 1 == frame || GX_HIST_MUTANT == 4;
			const bool changed = std::memcmp(e.cur, cur, sizeof(e.cur)) != 0 || GX_HIST_MUTANT == 2;
			e.moved = adjacent && changed;
			if (e.moved)
				std::memcpy(e.prev, e.cur, sizeof(e.prev));
			std::memcpy(e.cur, cur, sizeof(e.cur));
			e.frame = frame;
		}
		if (e.moved)
			std::memcpy(prevOut, e.prev, sizeof(e.prev));
		return e.moved;
	}

	// Drops every key last seen before frame-1. Call once per pass at most (it rebuilds the table).
	void sweep(uint32_t frame)
	{
		std::vector<Entry> old;
		old.swap(m_slots);
		m_slots.assign(old.size(), Entry());
		m_count = 0;
		for (const Entry& e : old)
			if (e.key && (e.frame + 1 >= frame || GX_HIST_MUTANT == 5)) {
				bool inserted = false;
				slot(e.key, &inserted) = e;
			}
	}

	size_t size() const { return m_count; }

private:
	struct Entry
	{
		const void* key = nullptr;	// nullptr = empty slot
		const void* tag = nullptr;	// the mesh's model
		uint32_t frame = 0;
		bool moved = false;
		float cur[12];
		float prev[12];
	};

	// Open addressing, linear probing, power-of-two capacity at most half full; deletes happen only in sweep's rebuild.
	Entry& slot(const void* key, bool* inserted)
	{
		if ((m_count + 1) * 2 > m_slots.size())
			grow();
		const size_t mask = m_slots.size() - 1;
		size_t i = hash(key) & mask;
		while (m_slots[i].key && m_slots[i].key != key)
			i = (i + 1) & mask;
		if (!m_slots[i].key) {
			m_slots[i].key = key;
			++m_count;
			*inserted = true;
		}
		return m_slots[i];
	}

	void grow()
	{
		std::vector<Entry> old;
		old.swap(m_slots);
		m_slots.assign(old.empty() ? 1024 : old.size() * 2, Entry());
		m_count = 0;
		for (const Entry& e : old)
			if (e.key) {
				bool inserted = false;
				slot(e.key, &inserted) = e;
			}
	}

	static size_t hash(const void* p)
	{
		uint64_t x = uint64_t(reinterpret_cast<uintptr_t>(p)) >> 4;
		x *= 0x9E3779B97F4A7C15ull;
		return size_t(x >> 20);
	}

	std::vector<Entry> m_slots;
	size_t m_count = 0;
};

// Skinned meshes: the deformed world-space positions (xyz per vertex) of the previous pass.
class GxSkinHistory
{
public:
	// Writes count*3 floats to prevOutXYZ: the positions `key` had in pass frame-1 when it had the same vertex count, else curXYZ
	// itself (first sight, a gap or a LOD switch: camera motion only for this mesh this pass). True when previous positions were
	// used. The first call of a pass rotates (this pass's positions become current); a second call in the same pass (W3DView::draw
	// renders the scene twice when a custom pass is on, W3DView.cpp:2363/2435) answers from the same previous positions.
	bool exchange(const void* key, const void* tag, const float* curXYZ, int count, uint32_t frame, float* prevOutXYZ)
	{
		Hist& h = m_map[key];
		if (h.tag != tag && GX_HIST_MUTANT != 9) {   // another mesh (model) at this address: a new history
			h.cur.clear();
			h.prevValid = false;
			h.frame = frame - 2;
		}
		h.tag = tag;
		const size_t n = size_t(count) * 3;
		if (h.frame != frame || GX_HIST_MUTANT == 8) {
			h.prevValid = h.frame + 1 == frame && (h.count == count || GX_HIST_MUTANT == 6) && !h.cur.empty();
			if (h.prevValid)
				h.prev.swap(h.cur);
			h.cur.assign(curXYZ, curXYZ + n);
			h.count = count;
			h.frame = frame;
		}
		const bool usable = h.prevValid && h.prev.size() >= n;
		std::memcpy(prevOutXYZ, usable ? h.prev.data() : curXYZ, n * sizeof(float));
		return usable != (GX_HIST_MUTANT == 7);
	}

	void sweep(uint32_t frame)
	{
		for (auto it = m_map.begin(); it != m_map.end();)
			it = it->second.frame + 1 >= frame ? std::next(it) : m_map.erase(it);
	}

	size_t size() const { return m_map.size(); }

	// Bytes held (both position buffers of every key), for the [GXMOTION] memory figure.
	size_t bytes() const
	{
		size_t b = 0;
		for (const auto& kv : m_map)
			b += (kv.second.cur.capacity() + kv.second.prev.capacity()) * sizeof(float);
		return b;
	}

private:
	struct Hist
	{
		uint32_t frame = 0;
		int count = 0;
		bool prevValid = false;
		const void* tag = nullptr;
		std::vector<float> cur, prev;
	};
	std::unordered_map<const void*, Hist> m_map;
};

} // namespace GXRender
