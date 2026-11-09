// GeneralsX @test D · rw3-X-2 13/10/2026 A23-B §7.1 EXEC digest sink, shared by gx_native_test and gx_proxy_test (moved from
// gx_native_test.cpp unchanged). TEST-ONLY.
#pragma once

#include "GxNativeCsTrace.h"
#include "GxD3D8Translate.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The sink: (op, canonical args, payload bytes) hashed in EXECUTION order; device objects numbered by first appearance, a new
// number after the object at that address was destroyed; the creation ledger hashed apart, in front order with its position.
// rw3-X-3 (A23-B merge gate): also the TRANSLATOR-INPUT digest "trin" (Rec::g_trInputSink: every translator entry as it received
// its arguments; Res objects numbered apart, a new number after destroyed() / retiredImage()) and "obs" (observe(): what a STRICT
// arm saw come back - read-back pixels, HRESULTs, out-parameters). --strict compares all four CS=0 vs CS=1.
class ExecDigest final : public GXRender::Dev::GxCsExecSink, public GXRender::Rec::GxTranslateInputSink
{
public:
	void exec(GXRender::Dev::GxCsOp op, const GXRender::Dev::GxCsArg* a, size_t n, const GXRender::Dev::GxCsBytes* b) override
	{
		std::lock_guard<std::mutex> g(m_lock);
		mix(m_exec, 0xE0E0u + uint64_t(op));
		std::string line = m_dump ? "op " + std::to_string(int(op)) : std::string();
		for (size_t i = 0; i < n; ++i) {
			const uint64_t v = a[i].object ? (a[i].v ? (id(a[i].v) | (1ull << 63)) : 0x7fffffffffffffffull) : a[i].v;
			mix(m_exec, v);
			if (m_dump)
				line += (a[i].object ? " o" : " ") + std::to_string(a[i].object && a[i].v ? (v & ~(1ull << 63)) : v);
		}
		if (b) {
			// presence of a ZERO-length payload is not an exec input (the argument that carries pointer presence is hashed above;
			// CS=0 hands the caller's null, CS=1 a ring address for 0 bytes - the translator sees the same pointer either way)
			const bool has = b->p && b->rowBytes && b->rows;
			mix(m_exec, has ? 0xB1u : b->rowBytes && b->rows ? 0xB0u : 0xB2u);
			mix(m_exec, b->rowBytes);
			mix(m_exec, b->rows);
			uint64_t ph = 1469598103934665603ull;
			if (b->p)
				for (uint32_t r = 0; r < b->rows; ++r) {
					bytes(m_exec, static_cast<const uint8_t*>(b->p) + r * b->pitch, b->rowBytes);
					bytes(ph, static_cast<const uint8_t*>(b->p) + r * b->pitch, b->rowBytes);
				}
			if (m_dump)
				line += " bytes " + std::string(has || !(b->rowBytes && b->rows) ? "" : "null ") + std::to_string(b->rowBytes) + "x" + std::to_string(b->rows) + " " + std::to_string(ph);
		}
		if (m_dump)
			std::fprintf(m_dump, "%s\n", line.c_str());
		++m_execN;
		if (m_log)
			m_ops.push_back(uint16_t(op));
	}
	void destroyed(const void* o) override
	{
		std::lock_guard<std::mutex> g(m_lock);
		m_ids.erase(uint64_t(reinterpret_cast<uintptr_t>(o)));
		m_trIds.erase(uint64_t(reinterpret_cast<uintptr_t>(o)));
	}
	void retiredImage(const void* o) override
	{
		std::lock_guard<std::mutex> g(m_lock);
		m_trIds.erase(uint64_t(reinterpret_cast<uintptr_t>(o)));
	}
	void input(GXRender::Rec::GxTrIn e, const GXRender::Rec::GxTrArg* a, size_t n, const GXRender::Rec::GxTrBytes* b, size_t nb) override
	{
		std::lock_guard<std::mutex> g(m_lock);
		mix(m_trin, 0x7700u + uint64_t(e));
		std::string line = m_dump ? "tr " + std::to_string(int(e)) : std::string();
		for (size_t i = 0; i < n; ++i) {
			const uint64_t v = a[i].object ? (a[i].v ? (trId(a[i].v) | (1ull << 63)) : 0x7fffffffffffffffull) : a[i].v;
			mix(m_trin, v);
			if (m_dump)
				line += (a[i].object ? " o" : " ") + std::to_string(a[i].object && a[i].v ? (v & ~(1ull << 63)) : v);
		}
		for (size_t i = 0; i < nb; ++i) {   // the bytes the entry reads (n = 0: none); pointer presence is an argument above
			mix(m_trin, b[i].n);
			uint64_t ph = 1469598103934665603ull;
			if (b[i].p && b[i].n) {
				bytes(m_trin, static_cast<const uint8_t*>(b[i].p), b[i].n);
				bytes(ph, static_cast<const uint8_t*>(b[i].p), b[i].n);
			}
			if (m_dump)
				line += " bytes " + std::to_string(b[i].n) + " " + std::to_string(ph);
		}
		if (m_dump)
			std::fprintf(m_dump, "%s\n", line.c_str());
		++m_trinN;
	}
	// a STRICT arm's observation (its read-back pixels, an HRESULT, an out-parameter): in call order, on the arm's thread
	void observe(const void* p, size_t n)
	{
		std::lock_guard<std::mutex> g(m_lock);
		mix(m_obs, n);
		bytes(m_obs, static_cast<const uint8_t*>(p), n);
		if (m_dump) {
			uint64_t ph = 1469598103934665603ull;
			bytes(ph, static_cast<const uint8_t*>(p), n);
			std::fprintf(m_dump, "obs %zu %llu\n", n, (unsigned long long)ph);
		}
		++m_obsN;
	}
	void created(uint64_t pos, uint32_t kind, const GXRender::Dev::GxCsArg* a, size_t n) override
	{
		std::lock_guard<std::mutex> g(m_lock);
		mix(m_ledger, pos);
		mix(m_ledger, kind);
		for (size_t i = 0; i < n; ++i)
			mix(m_ledger, a[i].v);
		++m_ledgerN;
	}
	std::string text() const
	{
		char b[192];
		std::snprintf(b, sizeof(b), "exec %016llx/%llu ledger %016llx/%llu trin %016llx/%llu obs %016llx/%llu", (unsigned long long)m_exec,
			(unsigned long long)m_execN, (unsigned long long)m_ledger, (unsigned long long)m_ledgerN, (unsigned long long)m_trin,
			(unsigned long long)m_trinN, (unsigned long long)m_obs, (unsigned long long)m_obsN);
		return b;
	}
	bool execEqual(const ExecDigest& o) const { return m_exec == o.m_exec && m_execN == o.m_execN && m_ledger == o.m_ledger && m_ledgerN == o.m_ledgerN; }
	bool trinEqual(const ExecDigest& o) const { return m_trin == o.m_trin && m_trinN == o.m_trinN; }
	bool obsEqual(const ExecDigest& o) const { return m_obs == o.m_obs && m_obsN == o.m_obsN; }
	bool equal(const ExecDigest& o) const { return execEqual(o) && trinEqual(o) && obsEqual(o); }
	// which of the digests differ ("" = none)
	std::string parts(const ExecDigest& o) const
	{
		std::string r;
		if (!execEqual(o))
			r += " exec";
		if (!trinEqual(o))
			r += " trin";
		if (!obsEqual(o))
			r += " obs";
		return r.empty() ? r : r.substr(1);
	}
	uint64_t execCount() const { return m_execN; }
	bool m_log = false;
	std::vector<uint16_t> m_ops;
	FILE* m_dump = nullptr;   // GX_STRICT_DUMP=<prefix>: one line per exec entry (debugging a DIFFERS)

private:
	static void mix(uint64_t& h, uint64_t v)
	{
		for (int i = 0; i < 8; ++i) {
			h ^= (v >> (8 * i)) & 0xff;
			h *= 1099511628211ull;
		}
	}
	static void bytes(uint64_t& h, const uint8_t* p, size_t n)
	{
		for (size_t i = 0; i < n; ++i) {
			h ^= p[i];
			h *= 1099511628211ull;
		}
	}
	uint64_t id(uint64_t p)
	{
		auto it = m_ids.find(p);
		if (it != m_ids.end())
			return it->second;
		m_ids[p] = m_next;
		return m_next++;
	}
	uint64_t trId(uint64_t p)
	{
		auto it = m_trIds.find(p);
		if (it != m_trIds.end())
			return it->second;
		m_trIds[p] = m_trNext;
		return m_trNext++;
	}
	std::mutex m_lock;
	uint64_t m_exec = 1469598103934665603ull, m_ledger = 1469598103934665603ull, m_execN = 0, m_ledgerN = 0, m_next = 1;
	uint64_t m_trin = 1469598103934665603ull, m_trinN = 0, m_trNext = 1, m_obs = 1469598103934665603ull, m_obsN = 0;
	std::map<uint64_t, uint64_t> m_ids, m_trIds;
};
