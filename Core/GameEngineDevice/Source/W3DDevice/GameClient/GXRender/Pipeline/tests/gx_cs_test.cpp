// gx_cs_test.cpp - the custom-shader catalogue's gates, no GPU (WS-E, rw2-E, 11/10/2026).
//   build:  cmake --build <dir> --target gx_cs_test && <dir>/gx_cs_test            (exit 0 = every check passed)
// Every acceptance check has a refusal twin: a fixture the catalogue must NOT accept (an overridden asset, a missing asset, a renamed
// copy, a different vertex declaration, a one-bit change). gx_cs_test_broken is this same suite linked against a catalogue that
// trusts the file NAME instead of the bytes (-DGX_CS_BROKEN_BY_NAME): it must FAIL, which shows the twins can fire. The pixels are gated elsewhere (gxcap-replay-vk parity vs the classic path).
#include "GxCustomCatalog.h"
#include "GxSyntheticPrograms.h"
#include "../shaders/gx_cs_ps.frag.h"
#include "../shaders/gx_cs_ps.mut.frag.h"
#include "../shaders/gx_cs_trees.vert.h"
#include "../shaders/gx_cs_trees.mut.vert.h"
#include "../shaders/gx_cs_wave.vert.h"
#include "../shaders/gx_cs_wave.mut.vert.h"
#include "../shaders/gx_cs_vel.frag.h"
#include "../shaders/gx_cs_shadow.vert.h"
#include "../shaders/gx_cs_shadow.mut.vert.h"
#include "../../../../../../Include/W3DDevice/GameClient/GxShadowVolumeVS.h"   // the bytes the game creates (rw3-R11)
#include "../../../../../../../GameEngine/Include/GameClient/GXWaterLive.h"   // live water's two ps.1.1 programs
#include "../../../../../../../GameEngine/Include/GameClient/GXGroundPrograms.h"   // the remastered ground's three (R-G1, the same session)
#include "../../../../../../../GameEngine/Include/GameClient/GXWaterRA3.h"   // RA3 water's sheen and its river sheen

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace GXRender;

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint32_t> toks(const GxCsEntry& e) { return std::vector<uint32_t>(e.dwords, e.dwords + e.count); }

static bool iequals(const std::string& a, const char* b) {   // case-insensitive, portable (strcasecmp is POSIX only)
	size_t n = strlen(b);
	if (a.size() != n) return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
	return true;
}

static const GxCsEntry* byFile(const char* name) {
	size_t n; const GxCsEntry* es = GxCsEntries(&n);
	for (size_t i = 0; i < n; i++) {
		std::string l = es[i].files; size_t a = 0;
		while (a <= l.size()) { size_t b = l.find('|', a); if (b == std::string::npos) b = l.size();
			if (iequals(l.substr(a, b - a), name)) return &es[i]; a = b + 1; }
	}
	return nullptr;
}

static GxCsLoad load(const char* path, const GxCsEntry* e, bool found = true) {
	GxCsLoad l; l.path = path; l.found = found; l.pixel = e->pixel;
	if (found) { l.tokens = toks(*e); if (e->decl) l.decl.assign(e->decl, e->decl + e->declCount); }
	return l;
}

static bool hasSpecId(const uint32_t* spv, size_t bytes, uint32_t id) {   // OpDecorate (71) <target> SpecId (1) <id>
	for (size_t i = 5; i + 4 <= bytes / 4; ) {
		const uint32_t op = spv[i] & 0xFFFF, len = spv[i] >> 16;
		if (len == 0) return false;
		if (op == 71 && len == 4 && spv[i + 2] == 1 && spv[i + 3] == id) return true;
		i += len;
	}
	return false;
}

int main(int argc, char** argv) {
	(void)argc; (void)argv;
	// ---- the table
	size_t n; const GxCsEntry* es = GxCsEntries(&n);
	size_t ported = 0, unported = 0, vsPorted = 0;
	for (size_t i = 0; i < n; i++) { if (es[i].program) { ported++; vsPorted += es[i].pixel ? 0 : 1; } else unported++; }
	CHECK(n == 30, "16 file blobs (ShadersZH.big + shaders.big) + the 3 built-in water programs + live water's 2 + the 3 gx: synthetic programs + gx:shadowvol + the ground's 3 + RA3 water's 2, table has %zu", n);
	CHECK(ported == 27 && vsPorted == 5 && unported == 3, "27 ported (22 ps + Trees.vso + wave.vso + gx:vel-rigid + gx:vel-skin + gx:shadowvol), 3 never loaded by the game (invmonochrome.pso, motionblur.*): got %zu ported %zu vs %zu unported", ported, vsPorted, unported);
	for (size_t i = 0; i < n; i++) {   // the ps program ids are 1..8 once each, the table order never leaks
		const GxCsEntry& e = es[i];
		CHECK(e.hash == GxFnv1a64(e.dwords, e.count * 4), "%s: stored hash is the bytes' FNV-1a64", e.files);
		const GxCsResolved r = e.pixel ? GxCsResolvePixel(e.dwords, e.count) : GxCsResolveVertex(e.dwords, e.count, e.decl, e.declCount);
		const GxCsVerdict want = e.program ? GxCsPorted : GxCsKnownUnported;
		CHECK(r.verdict == want && r.entry == &e, "%s resolves to itself as %d", e.files, int(want));
	}
	uint32_t seenPs = 0;
	for (size_t i = 0; i < n; i++) if (es[i].pixel && es[i].program) { CHECK(!(seenPs & (1u << es[i].program)), "ps id %u once", es[i].program); seenPs |= 1u << es[i].program; }
	CHECK(seenPs == 0x7FFFFEu, "ps ids 1..22 all present (%x)", seenPs);

	// ---- live water's two programs (GXWaterLive.h, created by W3DWater.cpp's glue while the option is on): the game's own arrays resolve
	// as the catalogue's water: entries, byte for byte, so the native renderer draws the glints and the foam instead of refusing them
	{
		struct W { const unsigned* a; size_t n; const char* file; uint8_t id; } ws[2] = {
			{ kGxWaterGlareShader, sizeof(kGxWaterGlareShader) / 4, "water:glare", GxCsPsWaterGlare },
			{ kGxWaterFoamShader, sizeof(kGxWaterFoamShader) / 4, "water:foam", GxCsPsWaterFoam } };
		CHECK(GxCsPsWaterGlare == 16 && GxCsPsWaterFoam == 17, "water:glare / water:foam are ps 16 / 17, after gx:vel-ps (no id moved)");
		for (const W& w : ws) {
			static_assert(sizeof(unsigned) == sizeof(uint32_t), "the arrays are the token dwords");
			const uint32_t* t = reinterpret_cast<const uint32_t*>(w.a);
			const GxCsEntry* e = byFile(w.file);
			const GxCsResolved r = GxCsResolvePixel(t, w.n);
			CHECK(e && e->pixel && e->program == w.id && e->count == w.n && std::memcmp(e->dwords, t, w.n * 4) == 0,
			      "the catalogue holds the game's %s byte for byte, as ps %u", w.file, unsigned(w.id));
			CHECK(r.verdict == GxCsPorted && r.entry == e, "the game's %s bytes resolve as ported (%s)", w.file, r.why);
			for (size_t k : { size_t(1), w.n - 2 }) {   // one bit in the first instruction's token and in the last instruction's source
				std::vector<uint32_t> v(t, t + w.n);
				v[k] ^= 1u;
				CHECK(GxCsResolvePixel(v.data(), v.size()).verdict == GxCsUnknown, "%s with token %zu flipped is UNKNOWN", w.file, k);
			}
		}
	}
	// ---- R-G1: the remastered ground's three programs, the COMPILED arrays TerrainTex.cpp creates (GXGroundPrograms.h;
	// a text read of the source could be fooled by an #if 0 copy or a commented-out token), so a token change without a regenerated
	// catalogue fails here instead of every ground draw being refused on native
	{
		struct G { const unsigned* a; size_t n; const char* file; uint8_t id; } gs[3] = {
			{ kGxGroundBasePs, sizeof(kGxGroundBasePs) / 4, "gx:ground-base", GxCsPsGroundBase },
			{ kGxGroundMultPs, sizeof(kGxGroundMultPs) / 4, "gx:ground-mult", GxCsPsGroundMult },
			{ kGxGroundMultLitePs, sizeof(kGxGroundMultLitePs) / 4, "gx:ground-mult-lite", GxCsPsGroundMultLite } };
		CHECK(GxCsPsGroundBase == 18 && GxCsPsGroundMult == 19 && GxCsPsGroundMultLite == 20,
		      "gx:ground-base / -mult / -mult-lite are ps 18 / 19 / 20, after live water's (no id moved)");
		for (const G& g : gs) {
			const std::vector<uint32_t> t(g.a, g.a + g.n);
			const GxCsEntry* e = byFile(g.file);
			CHECK(t.size() > 2 && t.front() == 0xFFFF0101u && t.back() == 0x0000FFFFu, "%s: %zu tokens, ps.1.1 to END", g.file, t.size());
			CHECK(e && e->pixel && e->program == g.id && e->count == t.size() && std::memcmp(e->dwords, t.data(), t.size() * 4) == 0,
			      "the catalogue holds the game's %s byte for byte, as ps %u", g.file, unsigned(g.id));
			const GxCsResolved r = GxCsResolvePixel(t.data(), t.size());
			CHECK(r.verdict == GxCsPorted && r.entry == e, "the game's %s bytes resolve as ported (%s)", g.file, r.why);
			if (t.size() > 3) {
				for (size_t k : { size_t(1), t.size() - 2 }) {
					std::vector<uint32_t> v = t;
					v[k] ^= 1u;
					CHECK(GxCsResolvePixel(v.data(), v.size()).verdict == GxCsUnknown, "%s with token %zu flipped is UNKNOWN", g.file, k);
				}
			}
		}
	}
	// ---- RA3 water's sheen (GXWaterRA3.h kGxWaterRA3SheenShader, created by W3DWater.cpp's glue while the option is on): the COMPILED
	// array resolves as the catalogue's water:ra3, byte for byte, so the native renderer draws the motion instead of refusing the pass
	{
		const std::vector<uint32_t> t(kGxWaterRA3SheenShader, kGxWaterRA3SheenShader + sizeof(kGxWaterRA3SheenShader) / 4);
		CHECK(GxCsPsWaterRA3 == 21, "water:ra3 is ps 21, after the ground's (no id moved)");
		const GxCsEntry* e = byFile("water:ra3");
		CHECK(t.size() > 3 && t.front() == 0xFFFF0101u && t.back() == 0x0000FFFFu, "water:ra3: %zu tokens, ps.1.1 to END", t.size());
		CHECK(e && e->pixel && e->program == GxCsPsWaterRA3 && e->count == t.size() && std::memcmp(e->dwords, t.data(), t.size() * 4) == 0,
		      "the catalogue holds the game's water:ra3 byte for byte, as ps 21");
		const GxCsResolved r = GxCsResolvePixel(t.data(), t.size());
		CHECK(r.verdict == GxCsPorted && r.entry == e, "the game's water:ra3 bytes resolve as ported (%s)", r.why);
		for (size_t k : { size_t(1), t.size() - 2 }) {
			std::vector<uint32_t> v = t;
			v[k] ^= 1u;
			CHECK(GxCsResolvePixel(v.data(), v.size()).verdict == GxCsUnknown, "water:ra3 with token %zu flipped is UNKNOWN", k);
		}
	}
	// ---- RA3 water's river sheen (GXWaterRA3.h kGxWaterRA3RiverSheenShader): the same, as water:ra3river, ps 22
	{
		const std::vector<uint32_t> t(kGxWaterRA3RiverSheenShader, kGxWaterRA3RiverSheenShader + sizeof(kGxWaterRA3RiverSheenShader) / 4);
		CHECK(GxCsPsWaterRA3River == 22 && GxCsPsCount == 23, "water:ra3river is ps 22, after water:ra3 (no id moved)");
		const GxCsEntry* e = byFile("water:ra3river");
		CHECK(t.size() > 3 && t.front() == 0xFFFF0101u && t.back() == 0x0000FFFFu, "water:ra3river: %zu tokens, ps.1.1 to END", t.size());
		CHECK(e && e->pixel && e->program == GxCsPsWaterRA3River && e->count == t.size() && std::memcmp(e->dwords, t.data(), t.size() * 4) == 0,
		      "the catalogue holds the game's water:ra3river byte for byte, as ps 22");
		const GxCsResolved r = GxCsResolvePixel(t.data(), t.size());
		CHECK(r.verdict == GxCsPorted && r.entry == e, "the game's water:ra3river bytes resolve as ported (%s)", r.why);
		for (size_t k : { size_t(1), t.size() - 2 }) {
			std::vector<uint32_t> v = t;
			v[k] ^= 1u;
			CHECK(GxCsResolvePixel(v.data(), v.size()).verdict == GxCsUnknown, "water:ra3river with token %zu flipped is UNKNOWN", k);
		}
	}
	// ---- every ported ps id that gx_cs_ps.frag runs has a case in its program switch (the compiled module's OpSwitch literals): a
	// catalogue id without one would draw the default's magenta on native. 15 (gx:vel-ps) is gx_cs_vel.frag's.
	{
		std::vector<uint32_t> lits;
		const uint32_t* spv = spv_gx_cs_ps_frag;
		const size_t words = sizeof(spv_gx_cs_ps_frag) / 4;
		for (size_t i = 5; i < words;) {
			const uint32_t op = spv[i] & 0xFFFF, len = spv[i] >> 16;
			if (len == 0) break;
			if (op == 251 && len >= 3)   // OpSwitch selector default (literal label)*
				for (uint32_t k = 3; k + 1 < len; k += 2) lits.push_back(spv[i + k]);
			i += len;
		}
		uint32_t cases = 0;
		for (uint32_t l : lits) if (l < 32) cases |= 1u << l;
		for (size_t i = 0; i < n; i++)
			if (es[i].pixel && es[i].program && es[i].program != GxCsPsVelocity)
				CHECK(cases & (1u << es[i].program), "ps %u (%s) has a case in gx_cs_ps.frag's switch", unsigned(es[i].program), es[i].files);
	}

	// ---- the motion bridge's synthetic programs (GxSyntheticPrograms.h, A35): the game's own arrays resolve as
	// the catalogue's gx: entries with their declarations, and map to the inputs gx_cs_vel.vert reads
	{
		const GxCsResolved ps = GxCsResolvePixel(gxVelPs, sizeof(gxVelPs) / 4);
		CHECK(ps.verdict == GxCsPorted && ps.entry && ps.entry->program == GxCsPsVelocity && std::strcmp(ps.entry->files, "gx:vel-ps") == 0, "gxVelPs is gx:vel-ps, program 15");
		const GxCsResolved rv = GxCsResolveVertex(gxVelRigidVs, sizeof(gxVelRigidVs) / 4, gxVelRigidDecl, sizeof(gxVelRigidDecl) / 4);
		CHECK(rv.verdict == GxCsPorted && rv.entry && rv.entry->program == GxCsVsVelRigid, "gxVelRigidVs + its declaration is gx:vel-rigid (%s)", rv.why);
		const GxCsResolved sv = GxCsResolveVertex(gxVelSkinVs, sizeof(gxVelSkinVs) / 4, gxVelSkinDecl, sizeof(gxVelSkinDecl) / 4);
		CHECK(sv.verdict == GxCsPorted && sv.entry && sv.entry->program == GxCsVsVelSkin, "gxVelSkinVs + its declaration is gx:vel-skin (%s)", sv.why);
		CHECK(GxCsResolveVertex(gxVelRigidVs, sizeof(gxVelRigidVs) / 4, gxVelSkinDecl, sizeof(gxVelSkinDecl) / 4).verdict == GxCsUnknown,
		      "gx:vel-rigid with the skin declaration is UNKNOWN");
		GxStructKey sk = {};
		std::string why;
		CHECK(rv.entry && GxCsVertexInputs(*rv.entry, gxVelRigidDecl, sizeof(gxVelRigidDecl) / 4, &sk, &why) && sk.inputCount == 1 &&
		      sk.inputs[0].semantic == GxSemPosition && sk.inputs[0].offset == 0 && sk.inputs[0].format == GxFmtFloat3, "vel-rigid inputs: v0 Position @0 (%s)", why.c_str());
		CHECK(sv.entry && GxCsVertexInputs(*sv.entry, gxVelSkinDecl, sizeof(gxVelSkinDecl) / 4, &sk, &why) && sk.inputCount == 2 &&
		      sk.inputs[1].semantic == GxSemNormal && sk.inputs[1].offset == 12 && sk.inputs[1].format == GxFmtFloat3, "vel-skin inputs: v1 Normal @12 (%s)", why.c_str());
		CHECK(!hasSpecId(spv_gx_cs_vel_frag, sizeof(spv_gx_cs_vel_frag), 40), "gx_cs_vel.frag is its own module (no program constant)");
	}

	// ---- refusal twins
	const GxCsEntry* tn2 = byFile("terrainnoise2.pso");
	CHECK(tn2 && tn2->program == GxCsPsTerrainNoise2, "terrainnoise2.pso is program 7 (what the shader module's case 7u ports)");
	{   // a one-bit change to a token = an overridden terrain.pso
		auto t = toks(*tn2); t[8] ^= 1u;
		CHECK(GxCsResolvePixel(t.data(), t.size()).verdict == GxCsUnknown, "a patched terrainnoise2.pso must be UNKNOWN");
	}
	{   // a different length (an extra instruction appended before the end token)
		auto t = toks(*tn2); t.insert(t.end() - 1, 0x00000001u);
		CHECK(GxCsResolvePixel(t.data(), t.size()).verdict == GxCsUnknown, "a longer ps must be UNKNOWN");
	}
	CHECK(GxCsResolvePixel(nullptr, 0).verdict == GxCsUnknown && GxCsResolvePixel(tn2->dwords, 0).verdict == GxCsUnknown, "empty input is UNKNOWN");
	{   // a vertex blob offered as a pixel shader, and the reverse
		const GxCsEntry* tv = byFile("Trees.vso");
		CHECK(tv && GxCsResolvePixel(tv->dwords, tv->count).verdict == GxCsUnknown, "Trees.vso bytes are not a pixel shader");
		CHECK(GxCsResolveVertex(tn2->dwords, tn2->count, nullptr, 0).verdict == GxCsUnknown, "terrainnoise2 bytes are not a vertex shader");
		// the declaration is part of the key
		std::vector<uint32_t> d(tv->decl, tv->decl + tv->declCount);
		CHECK(GxCsResolveVertex(tv->dwords, tv->count, d.data(), d.size()).verdict == GxCsPorted, "Trees.vso + its declaration is ported");
		d[2] ^= 1u << 16;   // v1 FLOAT3 -> a different data type
		CHECK(GxCsResolveVertex(tv->dwords, tv->count, d.data(), d.size()).verdict == GxCsUnknown, "Trees.vso with another declaration is UNKNOWN");
		CHECK(GxCsResolveVertex(tv->dwords, tv->count, nullptr, 0).verdict == GxCsUnknown, "Trees.vso with no declaration is UNKNOWN");
		GxStructKey sk; std::string why;
		CHECK(GxCsVertexInputs(*tv, tv->decl, tv->declCount, &sk, &why) && sk.inputCount == 4, "Trees inputs: %s", why.c_str());
		CHECK(sk.inputs[0].semantic == GxSemPosition && sk.inputs[0].offset == 0 && sk.inputs[0].format == GxFmtFloat3, "v0 -> Position @0");
		CHECK(sk.inputs[1].semantic == GxSemNormal && sk.inputs[1].offset == 12, "v1 -> Normal @12");
		CHECK(sk.inputs[2].semantic == GxSemColor0 && sk.inputs[2].offset == 24 && sk.inputs[2].format == GxFmtD3DColor, "v2 -> Color0 @24");
		CHECK(sk.inputs[3].semantic == GxSemTex0 && sk.inputs[3].offset == 28 && sk.inputs[3].format == GxFmtFloat2, "v7 -> Tex0 @28");
		CHECK(!GxCsVertexInputs(*tv, d.data(), d.size(), &sk, &why), "a changed declaration yields no inputs (%s)", why.c_str());
	}
	{	// gx:shadowvol = the bytes the game creates (GxShadowVolumeVS.h), not a copy that could drift
		const GxCsEntry* sv = byFile("gx:shadowvol");
		const size_t nv = sizeof(kGxShadowVS) / 4, nd = sizeof(kGxShadowDecl) / 4;
		CHECK(sv && sv->program == GxCsVsShadow && GxCsVsShadow == 5 && !sv->pixel, "gx:shadowvol is vertex program 5 (3/4 = gx:vel-*)");
		// the game's windows (W3DVolumetricShadow.cpp gxShadowGpuSlots: (MaxVertexShaderConst - 9) / 4 slots, capped 61) end inside the store
		const uint32_t slots = (kGxCsVsConstCount - 9) / 4, top = 9 + 4 * slots - 1;
		CHECK(kGxCsVsConstCount == 96 && slots == 21 && top == 92 && top < kGxCsVsConstCount, "96 constants -> 21 slots, top register c%u", top);
		CHECK(sv && sv->count == nv && std::memcmp(sv->dwords, kGxShadowVS, sizeof(kGxShadowVS)) == 0, "the catalogue holds the game's shadow vs byte for byte");
		CHECK(sv && sv->declCount == nd && std::memcmp(sv->decl, kGxShadowDecl, sizeof(kGxShadowDecl)) == 0, "...and its declaration");
		const GxCsResolved r = GxCsResolveVertex(kGxShadowVS, nv, kGxShadowDecl, nd);
		CHECK(r.verdict == GxCsPorted && r.entry == sv, "the game's shadow vs + declaration resolve as ported (%s)", r.why);
		std::vector<uint32_t> t(kGxShadowVS, kGxShadowVS + nv), d(kGxShadowDecl, kGxShadowDecl + nd);
		t[40] ^= 1u;   // one bit: dp4 r2.x -> r3.x (the first dp4's destination register)
		CHECK(GxCsResolveVertex(t.data(), t.size(), kGxShadowDecl, nd).verdict == GxCsUnknown, "a one-bit change of the shadow vs is UNKNOWN");
		d[4] = 0x40010003;   // v3 FLOAT1 -> FLOAT2
		CHECK(GxCsResolveVertex(kGxShadowVS, nv, d.data(), d.size()).verdict == GxCsUnknown, "the shadow vs with another declaration is UNKNOWN");
		GxStructKey sk; std::string why;
		CHECK(sv && GxCsVertexInputs(*sv, kGxShadowDecl, nd, &sk, &why) && sk.inputCount == 4, "shadow inputs: %s", why.c_str());
		CHECK(sk.inputs[0].semantic == GxSemPosition && sk.inputs[0].offset == 0 && sk.inputs[0].format == GxFmtFloat3, "v0 -> Position @0");
		CHECK(sk.inputs[1].semantic == GxSemNormal && sk.inputs[1].offset == 12 && sk.inputs[1].format == GxFmtFloat3, "v1 -> Normal @12");
		CHECK(sk.inputs[2].semantic == GxSemTex0 && sk.inputs[2].offset == 24 && sk.inputs[2].format == GxFmtFloat3, "v2 -> Tex0 FLOAT3 @24");
		CHECK(sk.inputs[3].semantic == GxSemTex1 && sk.inputs[3].offset == 36 && sk.inputs[3].format == GxFmtFloat1, "v3 -> Tex1 FLOAT1 @36 (40-B vertex)");
		CHECK(sv && !GxCsVertexInputs(*sv, d.data(), d.size(), &sk, &why), "a changed shadow declaration yields no inputs (%s)", why.c_str());
		if (sv) {
			for (uint8_t gap : { uint8_t(6) }) {   // ids no table names (render/dlss: 3/4 = gx:vel-*, checked below)
				GxCsEntry e = *sv;
				e.program = gap;
				CHECK(!GxCsVertexInputs(e, e.decl, e.declCount, &sk, &why) && why.find("no vertex-input table") != std::string::npos,
				      "vertex program %u without a table is refused, not read as another port (%s)", unsigned(gap), why.c_str());
			}
			for (uint8_t other : { uint8_t(3), uint8_t(4) }) {   // gx:vel-rigid / gx:vel-skin read v0[, v1]: the shadow declaration is not theirs
				GxCsEntry e = *sv;
				e.program = other;
				CHECK(!GxCsVertexInputs(e, e.decl, e.declCount, &sk, &why) && why.find("does not read") != std::string::npos,
				      "the shadow declaration under vertex program %u is refused, not read as that port (%s)", unsigned(other), why.c_str());
			}
		}
	}

	// ---- the session verdict (fixtures: overridden asset, missing asset, renamed copy)
	const char* kFiles[] = { "terrain.pso", "terrainnoise.pso", "terrainnoise2.pso", "roadnoise2.pso", "fterrain.pso", "fterrain0.pso",
	                         "fterrainnoise.pso", "fterrainnoise2.pso", "Trees.vso", "Trees.pso", "monochrome.pso", "wave.pso", "wave.vso" };
	std::vector<GxCsLoad> clean;
	for (const char* f : kFiles) clean.push_back(load((std::string("shaders\\") + f).c_str(), byFile(f)));
	{
		const GxCsSession s = GxCsSessionVerdict(clean);
		CHECK(!s.classic && s.complete && s.unported.empty(), "every shader file the game loads, as shipped, keeps the session native and complete (%s)", s.why.c_str());
	}
	{   // OVERRIDDEN terrain.pso: a mod's bytes under the shipped name
		std::vector<GxCsLoad> v = clean; v[0].tokens[5] ^= 0x10u;
		const GxCsSession s = GxCsSessionVerdict(v);
		CHECK(s.classic, "an overridden terrain.pso must force CLASSIC");
		CHECK(s.why.find("terrain.pso") != std::string::npos, "...and name it: %s", s.why.c_str());
	}
	{   // MISSING asset: the game would take its own 2-stage terrain fallback (W3DShaderManager.cpp:2058); never substitute a port
		std::vector<GxCsLoad> v = clean; v[0] = load("shaders\\terrain.pso", byFile("terrain.pso"), false);
		const GxCsSession s = GxCsSessionVerdict(v);
		CHECK(s.classic && s.why.find("missing") != std::string::npos, "a missing terrain.pso must force CLASSIC (%s)", s.why.c_str());
	}
	{   // RENAMED copy: terrainnoise2's bytes loaded as terrain.pso
		std::vector<GxCsLoad> v = clean; v[0].tokens = toks(*tn2);
		CHECK(GxCsSessionVerdict(v).classic, "known bytes under a different name must force CLASSIC");
	}
	{   // a known blob without a port: not a mod, but the session is incomplete (the lead decides)
		std::vector<GxCsLoad> v = clean; v.push_back(load("shaders\\invmonochrome.pso", byFile("invmonochrome.pso")));
		const GxCsSession s = GxCsSessionVerdict(v);
		CHECK(!s.classic && !s.complete && s.unported.size() == 1, "invmonochrome.pso loaded: native-possible but INCOMPLETE");
	}
	{   // path spelling: forward slashes, upper case
		std::vector<GxCsLoad> v = clean; v[0].path = "SHADERS/TERRAIN.PSO";
		CHECK(!GxCsSessionVerdict(v).classic, "the name match ignores case and separators");
	}
	CHECK(!GxCsSessionVerdict({}).classic, "no shaders loaded (no ps hardware) is not a reason for CLASSIC");

	// ---- the committed SPIR-V carries the selectors the pipeline cache sets
	CHECK(hasSpecId(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag), 40), "gx_cs_ps.frag has spec constant 40 (program)");
	CHECK(!hasSpecId(spv_gx_cs_ps_frag, sizeof(spv_gx_cs_ps_frag), 41), "...and the shipped module has NO mutate constant 41");
	CHECK(hasSpecId(spv_gx_cs_ps_mut_frag, sizeof(spv_gx_cs_ps_mut_frag), 41), "the mutant module has constant 41");
	CHECK(sizeof(spv_gx_cs_trees_vert) != sizeof(spv_gx_cs_trees_mut_vert), "the Trees mutant module differs from the shipped one");
	CHECK(sizeof(spv_gx_cs_wave_vert) != sizeof(spv_gx_cs_wave_mut_vert), "the wave mutant module differs from the shipped one");
	CHECK(sizeof(spv_gx_cs_shadow_vert) != sizeof(spv_gx_cs_shadow_mut_vert), "the shadow mutant module differs from the shipped one");

	std::printf("gx_cs_test: %d checks, %d failed\n", g_n, g_fail);
	return g_fail ? 1 : 0;
}
