/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// GxCustomCatalog.cpp - see GxCustomCatalog.h.
#include "GxCustomCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace GXRender {

#include "GxCustomCatalog.inc"

static const size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

static uint64_t fnv(const uint32_t* d, size_t n) { return GxFnv1a64(d, n * sizeof(uint32_t)); }

const GxCsEntry* GxCsEntries(size_t* count) {
	if (count)
		*count = kEntryCount;
	return kEntries;
}

static const GxCsEntry* findBlob(bool pixel, const uint32_t* t, size_t n) {
	if (!t || n == 0)
		return nullptr;
	const uint64_t h = fnv(t, n);
	for (const GxCsEntry& e : kEntries)
		if (e.pixel == (pixel ? 1 : 0) && e.hash == h && e.count == n && std::memcmp(e.dwords, t, n * sizeof(uint32_t)) == 0)
			return &e;   // the hash finds it, the full compare decides: a colliding mod blob is still UNKNOWN
	return nullptr;
}

GxCsResolved GxCsResolvePixel(const uint32_t* tokens, size_t count) {
	GxCsResolved r;
	const GxCsEntry* e = findBlob(true, tokens, count);
	if (!e) {
		r.why = "pixel shader bytecode not in the catalogue";
		return r;
	}
	r.entry = e;
	r.verdict = e->program ? GxCsPorted : GxCsKnownUnported;
	r.why = e->program ? "ported" : "known blob, no port yet";
	return r;
}

GxCsResolved GxCsResolveVertex(const uint32_t* tokens, size_t count, const uint32_t* decl, size_t declCount) {
	GxCsResolved r;
	const GxCsEntry* e = findBlob(false, tokens, count);
	if (!e) {
		r.why = "vertex shader bytecode not in the catalogue";
		return r;
	}
	r.entry = e;
	if (!e->program) {
		r.verdict = GxCsKnownUnported;
		r.why = "known blob, no port yet";
		return r;
	}
	if (!e->decl || !decl || declCount != e->declCount || std::memcmp(e->decl, decl, declCount * sizeof(uint32_t)) != 0) {
		r.entry = nullptr;
		r.why = "vertex declaration differs from the catalogue's";
		return r;
	}
	r.verdict = GxCsPorted;
	r.why = "ported";
	return r;
}

bool GxCsVertexInputs(const GxCsEntry& e, const uint32_t* decl, size_t declCount, GxStructKey* s, std::string* why) {
	auto fail = [&](const char* m) { if (why) *why = m; return false; };
	if (e.pixel || e.program == 0 || !s || !decl)
		return fail("not a ported vertex shader");
	if (declCount != e.declCount || std::memcmp(e.decl, decl, declCount * sizeof(uint32_t)) != 0)
		return fail("declaration differs from the catalogue's");
	// What each port reads, in declaration order (gx_cs_trees.vert / gx_cs_wave.vert). The D3DVSD tokens are decoded, not
	// assumed: token type = bits 29-31 (1 STREAM, 2 STREAMDATA, 7 END), STREAMDATA data type = bits 16-19 (1 FLOAT2, 2 FLOAT3,
	// 4 D3DCOLOR), register = bits 0-4. One stream (0) only.
	struct Reg { uint8_t reg, dataType, semantic, format, size; };
	static const Reg trees[4] = { { 0, 2, GxSemPosition, GxFmtFloat3, 12 }, { 1, 2, GxSemNormal, GxFmtFloat3, 12 },
	                              { 2, 4, GxSemColor0, GxFmtD3DColor, 4 }, { 7, 1, GxSemTex0, GxFmtFloat2, 8 } };
	static const Reg wave[3] = { { 0, 2, GxSemPosition, GxFmtFloat3, 12 }, { 1, 4, GxSemColor0, GxFmtD3DColor, 4 },
	                             { 2, 1, GxSemTex0, GxFmtFloat2, 8 } };
	// gx_cs_vel.vert (GxSyntheticPrograms.h): v0 position; the skin variant adds v1 = the previous position (Normal's location)
	static const Reg velSkin[2] = { { 0, 2, GxSemPosition, GxFmtFloat3, 12 }, { 1, 2, GxSemNormal, GxFmtFloat3, 12 } };
	// gx:shadowvol (gx_cs_shadow.vert): corner, face normal, face poly[0] vertex, copy base register (GxShadowVolumeVS.h)
	static const Reg shadow[4] = { { 0, 2, GxSemPosition, GxFmtFloat3, 12 }, { 1, 2, GxSemNormal, GxFmtFloat3, 12 },
	                               { 2, 2, GxSemTex0, GxFmtFloat3, 12 }, { 3, 0, GxSemTex1, GxFmtFloat1, 4 } };
	const Reg* want = e.program == GxCsVsTrees ? trees : e.program == GxCsVsWave ? wave
	                : e.program == GxCsVsVelRigid || e.program == GxCsVsVelSkin ? velSkin : e.program == GxCsVsShadow ? shadow : nullptr;
	const uint32_t count = e.program == GxCsVsTrees || e.program == GxCsVsShadow ? 4 : e.program == GxCsVsWave ? 3
	                     : e.program == GxCsVsVelSkin ? 2 : 1;
	if (!want)
		return fail("no vertex-input table for this program");
	uint32_t n = 0, off = 0, stream = 0;
	bool ended = false;
	std::memset(s->inputs, 0, sizeof(s->inputs));
	for (size_t i = 0; i < declCount && !ended; i++) {
		const uint32_t t = decl[i], type = t >> 29;
		if (t == 0xFFFFFFFFu || type == 7) { ended = true; break; }
		if (type == 1) {   // D3DVSD_STREAM(n)
			if ((t & 0xFF) != 0) return fail("a stream other than 0");
			stream = t & 0xFF;
		} else if (type == 2) {   // D3DVSD_REG(reg, datatype)
			const uint32_t reg = t & 0x1F, dt = (t >> 16) & 0xF;
			if (n >= count || reg != want[n].reg || dt != want[n].dataType)
				return fail("declaration element the port does not read");
			GxVtxInput& in = s->inputs[n];
			in.semantic = want[n].semantic;
			in.format = want[n].format;
			in.binding = uint8_t(stream);
			in.offset = uint16_t(off);
			off += want[n].size;
			n++;
		} else if (type != 0) {
			return fail("declaration token the port does not model");
		}
	}
	if (!ended || n != count)
		return fail("declaration does not carry exactly the port's elements");
	s->inputCount = uint8_t(n);
	return true;
}

static std::string lastComponent(const std::string& p) {
	const size_t i = p.find_last_of("\\/");
	return i == std::string::npos ? p : p.substr(i + 1);
}

static bool nameMatches(const GxCsEntry& e, const std::string& file) {
	std::string f = file;
	std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	std::string list = e.files;
	size_t a = 0;
	while (a <= list.size()) {
		size_t b = list.find('|', a);
		if (b == std::string::npos)
			b = list.size();
		std::string n = list.substr(a, b - a);
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		if (n == f)
			return true;
		a = b + 1;
	}
	return false;
}

GxCsSession GxCsSessionVerdict(const std::vector<GxCsLoad>& loads) {
	GxCsSession s;
	auto classic = [&](const std::string& why) {
		if (!s.classic) {
			s.classic = true;
			s.why = why;
		}
	};
	for (const GxCsLoad& l : loads) {
		const std::string file = lastComponent(l.path);
		if (!l.found) {
			classic("shader asset missing: " + l.path);
			continue;
		}
#ifdef GX_CS_BROKEN_BY_NAME
		// TEST-ONLY (gx_cs_test_broken, the suite's fail arm): trust the NAME - what a catalogue keyed by file name would do.
		GxCsResolved r;
		for (const GxCsEntry& e : kEntries)
			if (e.pixel == (l.pixel ? 1 : 0) && nameMatches(e, file)) { r.entry = &e; r.verdict = e.program ? GxCsPorted : GxCsKnownUnported; }
#else
		const GxCsResolved r = l.pixel ? GxCsResolvePixel(l.tokens.data(), l.tokens.size())
		                               : GxCsResolveVertex(l.tokens.data(), l.tokens.size(), l.decl.data(), l.decl.size());
#endif
		if (r.verdict == GxCsUnknown) {
			classic("shader asset not in the catalogue (overridden or modded): " + l.path);
			continue;
		}
		if (!nameMatches(*r.entry, file))   // known bytes under a name they do not ship under: a copy or a rename
			classic("shader bytes are the catalogue's '" + std::string(r.entry->files) + "' but were loaded as " + l.path);
		if (r.verdict == GxCsKnownUnported) {
			s.complete = false;
			s.unported.push_back(l.path);
		}
	}
	return s;
}

} // namespace GXRender
