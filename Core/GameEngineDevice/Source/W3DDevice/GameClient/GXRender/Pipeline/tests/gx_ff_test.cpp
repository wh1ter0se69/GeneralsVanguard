// gx_ff_test - the WS-D week-2 gates for the FF ubershader (C · rw2-D, 11/10/2026; RW2_BRIEFS.md §D, PLAN_rw2-D.md).
//
//   gx_ff_test --layout [--layout-mutant]
//       Every member offset (and array stride) of the VS / PS uniform blocks and the push block, read from the compiled
//       SPIR-V, against GxFFState.h's C++ layout. Fail arm --layout-mutant: one expected offset shifted -> that member fails.
//   gx_ff_test --identity <ffstream>... [--synthetic] [--mutant] [--validation] [--device <sel>]
//       Generic vs specialised PIXEL identity: for every distinct {draw part, FF state} of the census FF streams (census.py
//       --emit-ffstream), rebuilt as a D3D8 snapshot and packed by GxPackFF, with seeded run-time values (matrices, lights,
//       material, fog, tfactor, vertices, 8 noise textures): draw once with the generic pipeline, once with the specialised
//       one, compare every byte. --synthetic adds coverage the census never met (fog modes, alpha funcs, specular, an op
//       sweep). Fail arm --mutant: the specialised pipelines use gx_ff_mutant.frag (MODULATE x 1.0078125) -> must differ.
//       --a2c-family (B · a2c-1, A108; the integrator's ruling (6)): the specialised pipelines use the ALPHA-TO-COVERAGE family
//       (gx_ff.a2c / gx_ff_flat.a2c) with spec bit 3 clear, as every packed key has it -> must be identical to today's family, every
//       census state and the synthetic alpha funcs included (what a session that loaded the family draws for a non-A2C draw).
//       Fail arm --a2c-family --a2c-bit: bit 3 set on every alpha-tested entry (the ramp in force) -> must differ.
//   gx_ff_test --cold <ffstream> [--set builtin|<stream>] [--cache-dir <dir>] [--sync-spec] [--gap <frames>]
//       Cold-cache replay: prebuild the generic set, then replay the stream's FF pipelines in first-use order as frames,
//       every known pipeline drawn every frame through getFF(). Reports the draw thread's time per frame, generic misses,
//       specialised compiles and fallback draws. Fail arm --sync-spec: specialised variants compiled on the draw thread.
// Exit 0 = the arm's own verdict PASS, 1 = FAIL, 2 = could not run.
#include "../GxPipelineCache.h"
#include "../GxFFState.h"
#include "VkCtx.h"
#include "vk_mem_alloc.h"
#include "gx_ff_testkit.h"

#include "../shaders/gx_ff.vert.h"
#include "../shaders/gx_ff.frag.h"
#include "../shaders/gx_ff_flat.frag.h"
#include "../shaders/gx_ff_mutant.frag.h"
#include "../shaders/gx_ff.a2c.frag.h"
#include "../shaders/gx_ff_flat.a2c.frag.h"
#include "../GxAlphaToCoverage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace GXRender;
using namespace GXRender::d3d8;

namespace {

const GxDrawPart kBuiltinSet[] = {
#include "../GxGenericSet.inc"
};

// ---- the FF stream ----------------------------------------------------------------------------------------------------
enum { kVsFields = 17, kSpecFields = 5, kStageFields = 11, kFFFields = kVsFields + kSpecFields + 8 * kStageFields + 1 };
struct FFEntry {
	int frame = -1;
	GxDrawPart part;
	GxAttachConfig cfg;
	int ff[kFFFields];
	std::string src;
	bool ortho = false;   // synthetic only: an orthographic projection, so fog takes DXVK's Z path (ComputeWNearFar)
};

bool parsePart(std::istringstream& ss, GxDrawPart& part, GxAttachConfig& cfg, int& frame) {
	std::memset(&part, 0, sizeof(part));
	unsigned n = 0;
	ss >> frame >> n;
	if (!ss || n > GxMaxInputs)
		return false;
	part.inputCount = uint8_t(n);
	for (unsigned i = 0; i < n; i++) {
		unsigned sem, fmt, bind, off;
		ss >> sem >> fmt >> bind >> off;
		part.inputs[i] = { uint8_t(sem), uint8_t(fmt), uint8_t(bind), 0, uint16_t(off), 0 };
	}
	unsigned topo, poly, flat, a2c, smask, b[8], color, depth, samples;
	ss >> topo >> poly >> flat >> a2c >> smask;
	for (unsigned& x : b)
		ss >> x;
	ss >> color >> depth >> samples;
	if (!ss)
		return false;
	part.topologyClass = uint8_t(topo);
	part.polygonMode = uint8_t(poly);
	part.flatShading = uint8_t(flat);
	part.alphaToCoverage = uint8_t(a2c);
	part.sampleMask = smask;
	part.blend = { uint8_t(b[0]), uint8_t(b[1]), uint8_t(b[2]), uint8_t(b[3]), uint8_t(b[4]), uint8_t(b[5]), uint8_t(b[6]),
	               uint8_t(b[7]) };
	cfg = { color, depth, uint8_t(samples) };
	return true;
}

bool readFFStream(const std::string& path, std::vector<FFEntry>& out) {
	std::ifstream in(path);
	if (!in) {
		std::fprintf(stderr, "[GXFF] cannot open %s\n", path.c_str());
		return false;
	}
	std::string line;
	const std::string name = std::filesystem::path(path).stem().string();
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		const size_t bar = line.find('|');
		if (bar == std::string::npos) {
			std::fprintf(stderr, "[GXFF] %s: a line without FF fields (not an --emit-ffstream file)\n", path.c_str());
			return false;
		}
		FFEntry e;
		std::istringstream a(line.substr(0, bar)), b(line.substr(bar + 1));
		if (!parsePart(a, e.part, e.cfg, e.frame))
			return false;
		for (int& v : e.ff)
			b >> v;
		int extra;
		if (!b || (b >> extra)) {
			std::fprintf(stderr, "[GXFF] %s: expected %d FF fields\n", path.c_str(), int(kFFFields));
			return false;
		}
		e.src = name;
		out.push_back(e);
	}
	return true;
}

uint32_t rng(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
float frand(uint32_t& s, float lo, float hi) { return lo + (hi - lo) * float(rng(s) >> 8) / float(1u << 24); }
uint32_t asU(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

bool partHas(const GxDrawPart& p, uint8_t sem) {
	for (unsigned i = 0; i < p.inputCount; i++)
		if (p.inputs[i].semantic == sem)
			return true;
	return false;
}

// The census entry's FF state as a D3D8 snapshot (the inverse of DXVK 2.6's key derivation for the fields it recorded),
// with seeded run-time values. ff[] order = census.py FF_VS_FIELDS, FF_SPEC_FIELDS, 8 x FF_STAGE_FIELDS, GlobalSpecular.
void toSnapshot(const FFEntry& e, uint32_t seed, GxD3D8FFState& s) {
	const int* v = e.ff;
	const int* sp = v + kVsFields;
	GxD3D8FFDefaults(&s);
	const bool posT = v[4] != 0;
	uint32_t fvf = posT ? FVF_XYZRHW : FVF_XYZ;
	if (!posT && partHas(e.part, GxSemNormal))
		fvf |= FVF_NORMAL;
	if (v[5]) fvf |= FVF_DIFFUSE;
	if (v[6]) fvf |= FVF_SPECULAR;
	if (v[7]) fvf |= FVF_PSIZE;
	const uint32_t decl = uint32_t(v[3]);
	uint32_t texCount = 0;
	for (uint32_t i = 0; i < 8; i++)
		if ((decl >> (3 * i)) & 7u)
			texCount = i + 1;
	fvf |= texCount << FVF_TEXCOUNT_SHIFT;
	for (uint32_t i = 0; i < texCount; i++) {
		static const uint32_t code[5] = { 0, 3, 0, 1, 2 };   // components 1/2/3/4 -> D3DFVF_TEXTUREFORMAT1/2/3/4
		fvf |= code[std::min<uint32_t>((decl >> (3 * i)) & 7u, 4)] << (16 + 2 * i);
	}
	s.fvf = fvf;
	for (uint32_t i = 0; i < 8; i++) {
		const int* st = v + kVsFields + kSpecFields + kStageFields * i;
		uint32_t* t = s.tss[i];
		t[TSS_COLOROP] = uint32_t(st[0]);
		t[TSS_COLORARG0] = uint32_t(st[1]);
		t[TSS_COLORARG1] = uint32_t(st[2]);
		t[TSS_COLORARG2] = uint32_t(st[3]);
		t[TSS_ALPHAOP] = uint32_t(st[4]);
		t[TSS_ALPHAARG0] = uint32_t(st[5]);
		t[TSS_ALPHAARG1] = uint32_t(st[6]);
		t[TSS_ALPHAARG2] = uint32_t(st[7]);
		t[TSS_RESULTARG] = st[8] ? TA_TEMP : TA_CURRENT;
		t[TSS_TEXCOORDINDEX] = ((uint32_t(v[0]) >> (3 * i)) & 7u) | (((uint32_t(v[1]) >> (3 * i)) & 7u) << 16);
		t[TSS_TEXTURETRANSFORMFLAGS] = ((uint32_t(v[2]) >> (3 * i)) & 7u) | (st[9] ? TTFF_PROJECTED : 0u);
		s.textureType[i] = st[10] ? GxTex2D : GxTexNone;
		t[TSS_BUMPENVMAT00] = asU(frand(seed, -0.2f, 0.2f));
		t[TSS_BUMPENVMAT01] = asU(frand(seed, -0.2f, 0.2f));
		t[TSS_BUMPENVMAT10] = asU(frand(seed, -0.2f, 0.2f));
		t[TSS_BUMPENVMAT11] = asU(frand(seed, -0.2f, 0.2f));
		t[TSS_BUMPENVLSCALE] = asU(frand(seed, 0.5f, 2.0f));
		t[TSS_BUMPENVLOFFSET] = asU(frand(seed, -0.2f, 0.2f));
	}
	uint32_t* rs = s.rs;
	rs[RS_LIGHTING] = uint32_t(v[8]);
	rs[RS_NORMALIZENORMALS] = uint32_t(v[9]);
	rs[RS_LOCALVIEWER] = uint32_t(v[10]);
	rs[RS_RANGEFOGENABLE] = uint32_t(v[11]);
	rs[RS_COLORVERTEX] = 1;
	rs[RS_DIFFUSEMATERIALSOURCE] = uint32_t(v[13]);
	rs[RS_AMBIENTMATERIALSOURCE] = uint32_t(v[14]);
	rs[RS_SPECULARMATERIALSOURCE] = uint32_t(v[15]);
	rs[RS_EMISSIVEMATERIALSOURCE] = uint32_t(v[16]);
	rs[RS_SPECULARENABLE] = uint32_t(v[kFFFields - 1]);
	rs[RS_ALPHATESTENABLE] = sp[0] != 7;
	rs[RS_ALPHAFUNC] = uint32_t(sp[0]) + 1u;
	rs[RS_FOGENABLE] = uint32_t(sp[1]);
	rs[RS_FOGVERTEXMODE] = uint32_t(sp[2]);
	rs[RS_FOGTABLEMODE] = uint32_t(sp[3]);
	s.pointList = e.part.topologyClass == GxTopoPoints;
	rs[RS_POINTSCALEENABLE] = uint32_t(sp[4]) & 1u;          // DXVK 2.6 PointMode: bit 0 scale, bit 1 sprite (census: always 0)
	rs[RS_POINTSPRITEENABLE] = (uint32_t(sp[4]) >> 1) & 1u;
	// run-time values (never part of the key)
	rs[RS_AMBIENT] = rng(seed);
	rs[RS_TEXTUREFACTOR] = rng(seed);
	rs[RS_ALPHAREF] = 64u + (rng(seed) & 127u);
	rs[RS_FOGCOLOR] = rng(seed) & 0xffffffu;
	rs[RS_FOGSTART] = asU(frand(seed, 1.5f, 3.0f));
	rs[RS_FOGEND] = asU(frand(seed, 6.0f, 9.0f));
	rs[RS_FOGDENSITY] = asU(frand(seed, 0.05f, 0.4f));
	rs[RS_POINTSIZE] = asU(frand(seed, 2.0f, 6.0f));
	s.lightCount = uint32_t(v[12]);
	for (uint32_t i = 0; i < s.lightCount && i < 8; i++) {
		GxD3D8Light& l = s.lights[i];
		l.type = 1u + (seed + i) % 3u;   // point / spot / directional in turn
		for (int c = 0; c < 4; c++) {
			l.diffuse[c] = frand(seed, 0.2f, 1.0f);
			l.specular[c] = frand(seed, 0.0f, 1.0f);
			l.ambient[c] = frand(seed, 0.0f, 0.4f);
		}
		l.position[0] = frand(seed, -3, 3); l.position[1] = frand(seed, -3, 3); l.position[2] = frand(seed, 0, 6);
		l.direction[0] = frand(seed, -1, 1); l.direction[1] = frand(seed, -1, 1); l.direction[2] = frand(seed, 0.2f, 1);
		l.range = frand(seed, 6, 40);
		l.falloff = frand(seed, 0.5f, 2.0f);
		l.attenuation0 = frand(seed, 0.5f, 1.0f);
		l.attenuation1 = frand(seed, 0.0f, 0.2f);
		l.attenuation2 = frand(seed, 0.0f, 0.05f);
		l.theta = frand(seed, 0.3f, 1.0f);
		l.phi = l.theta + frand(seed, 0.2f, 1.0f);
	}
	for (int c = 0; c < 4; c++) {
		s.material.diffuse[c] = frand(seed, 0, 1);
		s.material.ambient[c] = frand(seed, 0, 1);
		s.material.specular[c] = frand(seed, 0, 1);
		s.material.emissive[c] = frand(seed, 0, 0.3f);
	}
	s.material.power = frand(seed, 2, 30);
	const float ang = frand(seed, -0.6f, 0.6f), ca = std::cos(ang), sa = std::sin(ang);
	const float world[16] = { ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0, frand(seed, -0.3f, 0.3f), frand(seed, -0.3f, 0.3f), 0, 1 };
	std::memcpy(s.world, world, 64);
	const float proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 100.0f / 99.0f, 1, 0, 0, -100.0f / 99.0f, 0 };   // 90 degrees, 1..100
	const float ortho[16] = { 1.0f / 8.8f, 0, 0, 0, 0, 1.0f / 8.8f, 0, 0, 0, 0, 0.1f, 0, 0, 0, -0.1f, 1 };      // w = 1: Z fog
	std::memcpy(s.projection, e.ortho ? ortho : proj, 64);
	for (auto& m : s.texture) {
		const float t[16] = { frand(seed, 0.5f, 2), frand(seed, -0.3f, 0.3f), 0, 0, frand(seed, -0.3f, 0.3f), frand(seed, 0.5f, 2), 0, 0,
		                      frand(seed, -0.5f, 0.5f), frand(seed, -0.5f, 0.5f), 1, 0, frand(seed, -1, 1), frand(seed, -1, 1), 0, 1 };
		std::memcpy(m, t, 64);
	}
	s.viewport = { 0, 0, 64, 64, 0.0f, 1.0f };
}

uint32_t fmtBytes(uint8_t f) {
	static const uint32_t b[GxFmtCount] = { 0, 4, 8, 12, 16, 4, 4 };
	return f < GxFmtCount ? b[f] : 0;
}

// Vertex data for the part's real inputs: positions inside the 90-degree frustum (or the 64x64 viewport for POSITIONT).
std::vector<uint8_t> makeVertices(const GxDrawPart& p, bool posT, uint32_t count, uint32_t seed, uint32_t* strides) {
	uint32_t extent[GxNullBinding] = {};
	for (unsigned i = 0; i < p.inputCount; i++)
		extent[p.inputs[i].binding] = std::max(extent[p.inputs[i].binding], uint32_t(p.inputs[i].offset) + fmtBytes(p.inputs[i].format));
	uint32_t base[GxNullBinding] = {}, total = 0;
	for (uint32_t b = 0; b < GxNullBinding; b++) {
		strides[b] = extent[b];
		base[b] = total;
		total += extent[b] * count;
	}
	std::vector<uint8_t> out(std::max(total, 16u), 0);
	for (uint32_t vtx = 0; vtx < count; vtx++)
		for (unsigned i = 0; i < p.inputCount; i++) {
			const GxVtxInput& in = p.inputs[i];
			uint8_t* dst = out.data() + base[in.binding] + extent[in.binding] * vtx + in.offset;
			if (in.format == GxFmtD3DColor || in.format == GxFmtUByte4) {
				uint32_t c = rng(seed);
				std::memcpy(dst, &c, 4);
				continue;
			}
			float f[4] = {};
			const uint32_t n = fmtBytes(in.format) / 4;
			if (in.semantic == GxSemPosition) {
				if (posT) {
					f[0] = frand(seed, -8, 72); f[1] = frand(seed, -8, 72); f[2] = frand(seed, 0, 1); f[3] = frand(seed, 0.2f, 1.0f);
				} else {
					f[2] = frand(seed, 2, 8);
					f[0] = frand(seed, -1.1f, 1.1f) * f[2]; f[1] = frand(seed, -1.1f, 1.1f) * f[2]; f[3] = 1;
				}
			} else if (in.semantic == GxSemNormal) {
				float x = frand(seed, -1, 1), y = frand(seed, -1, 1), z = frand(seed, -1, 1), l = std::sqrt(x * x + y * y + z * z) + 1e-6f;
				f[0] = x / l; f[1] = y / l; f[2] = z / l;
			} else if (in.semantic == GxSemPSize) {
				f[0] = frand(seed, 1, 8);
			} else {
				for (float& c : f)
					c = frand(seed, -0.5f, 2.5f);
			}
			std::memcpy(dst, f, n * 4);
		}
	return out;
}

bool g_specCorrupt = false;
bool g_specCorruptVs = false; // --spec-corrupt-vs: the same for a VS-only key word (fail arm for the vertex stage's constants)
bool g_blankMutant = false;   // --blank-mutant: every entry's Projection zeroed (nothing on screen) - the coverage rule must fail   // --spec-corrupt: the specialised pipeline's constants disagree with the pushed key (fail arm)
bool g_plantHazard = false;   // --plant-hazard: the readback's layout transition does not wait for the colour writes
bool g_a2cFamily = false;     // --a2c-family: the specialised side is the alpha-to-coverage family (B · a2c-1)
bool g_a2cBit = false;        // --a2c-bit: ...with bit 3 set on every alpha-tested entry (its fail arm)
std::map<std::string, int> g_errorIds;
std::mutex g_errorLock;
void captureLog(Vk::LogLevel, const char* line, void*) {
	std::fputs(line, stderr);
	std::fputc('\n', stderr);
	const char* p = std::strstr(line, "VUID-");
	if (std::strstr(line, "ERROR") && p) {
		std::string id(p, std::strcspn(p, " ]:,\""));
		std::lock_guard<std::mutex> g(g_errorLock);
		g_errorIds[id]++;
	}
}

// ---- --layout: SPIR-V member offsets vs GxFFState.h ---------------------------------------------------------------------
struct SpirvInfo {
	std::map<uint32_t, std::string> names;                              // OpName
	std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;   // OpMemberName
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;          // OpMemberDecorate Offset
	std::map<uint32_t, uint32_t> arrayStride;                           // OpDecorate ArrayStride
	std::map<uint32_t, std::vector<uint32_t>> structMembers;            // OpTypeStruct member type ids
	std::map<uint32_t, uint32_t> arrayElem;                             // OpTypeArray -> element type
};
SpirvInfo parseSpirv(const uint32_t* w, size_t words) {
	SpirvInfo s;
	for (size_t i = 5; i < words;) {
		const uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
		if (!n || i + n > words)
			break;
		auto str = [&](size_t at) { return std::string(reinterpret_cast<const char*>(w + at)); };
		if (op == 5) s.names[w[i + 1]] = str(i + 2);
		else if (op == 6) s.memberNames[{ w[i + 1], w[i + 2] }] = str(i + 3);
		else if (op == 72 && w[i + 3] == 35) s.offsets[{ w[i + 1], w[i + 2] }] = w[i + 4];
		else if (op == 71 && w[i + 2] == 6) s.arrayStride[w[i + 1]] = w[i + 3];
		else if (op == 30) s.structMembers[w[i + 1]] = std::vector<uint32_t>(w + i + 2, w + i + n);
		else if (op == 28) s.arrayElem[w[i + 1]] = w[i + 2];
		i += n;
	}
	return s;
}

int runLayout(bool mutant) {
	struct Expect { const char* structName; const char* member; uint32_t offset; };
	std::vector<Expect> vsx = {
		{ "D3D9FixedFunctionVS", "WorldView", offsetof(GxFFVsBlock, WorldView) },
		{ "D3D9FixedFunctionVS", "NormalMatrix", offsetof(GxFFVsBlock, NormalMatrix) },
		{ "D3D9FixedFunctionVS", "InverseView", offsetof(GxFFVsBlock, InverseView) },
		{ "D3D9FixedFunctionVS", "Projection", offsetof(GxFFVsBlock, Projection) },
		{ "D3D9FixedFunctionVS", "WorldViewProj", offsetof(GxFFVsBlock, WorldViewProj) },
		{ "D3D9FixedFunctionVS", "TexcoordMatrices", offsetof(GxFFVsBlock, TexcoordMatrices) },
		{ "D3D9FixedFunctionVS", "ViewportInfo", offsetof(GxFFVsBlock, ViewportInverseOffset) },
		{ "D3D9FixedFunctionVS", "Lights", offsetof(GxFFVsBlock, Lights) },
		{ "D3D9FixedFunctionVS", "Material", offsetof(GxFFVsBlock, Material) },
		{ "D3D9FixedFunctionVS", "Fog", uint32_t(offsetof(GxFFVsBlock, Fog)) + (mutant ? 4u : 0u) },   // the planted shift
		{ "D3D9FixedFunctionVS", "GlobalAmbient", offsetof(GxFFVsBlock, GlobalAmbient) },
		{ "D3D9FixedFunctionVS", "TweenFactor", offsetof(GxFFVsBlock, TweenFactor) },
		{ "D3D9FixedFunctionVS", "PointSize", offsetof(GxFFVsBlock, PointSize) },
		{ "D3D9FixedFunctionVS", "PointSizeMin", offsetof(GxFFVsBlock, PointSizeMin) },
		{ "D3D9FixedFunctionVS", "PointSizeMax", offsetof(GxFFVsBlock, PointSizeMax) },
		{ "D3D9FixedFunctionVS", "PointScaleA", offsetof(GxFFVsBlock, PointScaleA) },
		{ "D3D9FixedFunctionVS", "PointScaleB", offsetof(GxFFVsBlock, PointScaleB) },
		{ "D3D9FixedFunctionVS", "PointScaleC", offsetof(GxFFVsBlock, PointScaleC) },
		{ "D3D9ViewportInfo", "inverseOffset", 0 },
		{ "D3D9ViewportInfo", "inverseExtent", offsetof(GxFFVsBlock, ViewportInverseExtent) - offsetof(GxFFVsBlock, ViewportInverseOffset) },
		{ "D3D9Light", "Diffuse", offsetof(GxFFLight, Diffuse) }, { "D3D9Light", "Specular", offsetof(GxFFLight, Specular) },
		{ "D3D9Light", "Ambient", offsetof(GxFFLight, Ambient) }, { "D3D9Light", "Position", offsetof(GxFFLight, Position) },
		{ "D3D9Light", "Direction", offsetof(GxFFLight, Direction) }, { "D3D9Light", "Type", offsetof(GxFFLight, Type) },
		{ "D3D9Light", "Range", offsetof(GxFFLight, Range) }, { "D3D9Light", "Falloff", offsetof(GxFFLight, Falloff) },
		{ "D3D9Light", "Attenuation0", offsetof(GxFFLight, Attenuation0) }, { "D3D9Light", "Attenuation1", offsetof(GxFFLight, Attenuation1) },
		{ "D3D9Light", "Attenuation2", offsetof(GxFFLight, Attenuation2) }, { "D3D9Light", "Theta", offsetof(GxFFLight, Theta) },
		{ "D3D9Light", "Phi", offsetof(GxFFLight, Phi) },
		{ "D3DMATERIAL9", "Diffuse", offsetof(GxFFMaterial, Diffuse) }, { "D3DMATERIAL9", "Ambient", offsetof(GxFFMaterial, Ambient) },
		{ "D3DMATERIAL9", "Specular", offsetof(GxFFMaterial, Specular) }, { "D3DMATERIAL9", "Emissive", offsetof(GxFFMaterial, Emissive) },
		{ "D3DMATERIAL9", "Power", offsetof(GxFFMaterial, Power) },
	};
	std::vector<Expect> fsx = {
		{ "SharedData", "Stages", offsetof(GxFFPsBlock, Stages) }, { "SharedData", "fog", offsetof(GxFFPsBlock, Fog) },
		{ "SharedData", "textureFactor", offsetof(GxFFPsBlock, TextureFactor) },
		{ "D3D9SharedPSStage", "Constant", offsetof(GxFFPsStage, Constant) }, { "D3D9SharedPSStage", "Padding", offsetof(GxFFPsStage, pad0) },
		{ "D3D9SharedPSStage", "BumpEnvLScale", offsetof(GxFFPsStage, BumpEnvLScale) },
		{ "D3D9SharedPSStage", "BumpEnvLOffset", offsetof(GxFFPsStage, BumpEnvLOffset) },
		{ "D3D9SharedPSStage", "BumpEnvMat", offsetof(GxFFPsStage, BumpEnvMat) },
	};
	for (const char* st : { "GxFFFog" }) {
		for (std::vector<Expect>* v : { &vsx, &fsx }) {
			v->push_back({ st, "packedFogColorAndAlphaRef", offsetof(GxFFFog, PackedFogColorAndAlphaRef) });
			v->push_back({ st, "fogDistanceScale", offsetof(GxFFFog, FogDistanceScale) });
			v->push_back({ st, "fogDistanceEnd", offsetof(GxFFFog, FogDistanceEnd) });
			v->push_back({ st, "fogDensity", offsetof(GxFFFog, FogDensity) });
		}
	}
	static const char* kSpecNames[GxFFW_Count] = { "alphaTestAndModeArgs", "fogArgs", "packedProjMaskAndFfArgs",
		"unused_vsSamplerTypesAndBools", "psSamplerTypes", "psSamplerModes", "unused_psBools", "unused_spec7", "packedStageOps01",
		"packedStageOps23", "packedStageOps45", "packedStageOps67", "packedStageArgs0", "packedStageArgs1", "packedStageArgs2",
		"packedStageArgs3", "packedStageArgs4", "packedStageArgs5", "packedStageArgs6", "packedStageArgs7", "vsData0", "vsData1",
		"vsData2", "vsData3", "vsData4", "vsData5", "vsData6", "vsData7", "vsData8", "vsData9", "vsData10", "vsData11" };
	static_assert(GxFFW_FogArgs == 1 && GxFFW_PsSamplerTypes == 4 && GxFFW_StageOps01 == 8 && GxFFW_StageArgs0 == 12 &&
	              GxFFW_VsData0 == 20, "kSpecNames follows GxFFSpecWord");
	for (uint32_t i = 0; i < GxFFW_Count; i++) {
		vsx.push_back({ "GxFFSpec", kSpecNames[i], i * 4 });
		fsx.push_back({ "GxFFSpec", kSpecNames[i], i * 4 });
	}
	struct Stride { const char* structName; const char* member; uint32_t stride; };
	const std::vector<Stride> vsStrides = { { "D3D9FixedFunctionVS", "TexcoordMatrices", 64 },
	                                        { "D3D9FixedFunctionVS", "Lights", sizeof(GxFFLight) } };
	const std::vector<Stride> fsStrides = { { "SharedData", "Stages", sizeof(GxFFPsStage) } };

	int bad = 0, checked = 0;
	auto check = [&](const char* mod, const uint32_t* code, size_t bytes, const std::vector<Expect>& ex, const std::vector<Stride>& sx) {
		const SpirvInfo s = parseSpirv(code, bytes / 4);
		auto findStruct = [&](const char* name) -> uint32_t {
			for (const auto& kv : s.names)
				if (kv.second == name && s.structMembers.count(kv.first))
					return kv.first;
			return 0;
		};
		auto findMember = [&](uint32_t sid, const char* name) -> int {
			for (const auto& kv : s.memberNames)
				if (kv.first.first == sid && kv.second == name)
					return int(kv.first.second);
			return -1;
		};
		for (const Expect& e : ex) {
			const uint32_t sid = findStruct(e.structName);
			const int m = sid ? findMember(sid, e.member) : -1;
			auto it = m >= 0 ? s.offsets.find({ sid, uint32_t(m) }) : s.offsets.end();
			checked++;
			if (it == s.offsets.end() || it->second != e.offset) {
				bad++;
				std::printf("[GXFF] layout %s %s.%s: SPIR-V %s, C++ %u  MISMATCH\n", mod, e.structName, e.member,
				            it == s.offsets.end() ? "absent" : std::to_string(it->second).c_str(), e.offset);
			}
		}
		for (const Stride& x : sx) {
			const uint32_t sid = findStruct(x.structName);
			const int m = sid ? findMember(sid, x.member) : -1;
			const uint32_t type = m >= 0 ? s.structMembers.at(sid)[size_t(m)] : 0;
			auto it = s.arrayStride.find(type);
			checked++;
			if (it == s.arrayStride.end() || it->second != x.stride) {
				bad++;
				std::printf("[GXFF] layout %s %s.%s stride: SPIR-V %s, C++ %u  MISMATCH\n", mod, x.structName, x.member,
				            it == s.arrayStride.end() ? "absent" : std::to_string(it->second).c_str(), x.stride);
			}
		}
	};
	check("vs", spv_gx_ff_vert, sizeof(spv_gx_ff_vert), vsx, vsStrides);
	check("fs", spv_gx_ff_frag, sizeof(spv_gx_ff_frag), fsx, fsStrides);
	check("fs-flat", spv_gx_ff_flat_frag, sizeof(spv_gx_ff_flat_frag), fsx, fsStrides);
	const bool pass = mutant ? bad > 0 : bad == 0;
	std::printf("[GXFF] layout checked=%d mismatches=%d arm=%s verdict=%s\n", checked, bad, mutant ? "layout-mutant" : "none",
	            pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

// ---- the Vulkan side shared by --identity and --cold --------------------------------------------------------------------
struct Gpu {
	Vk::VkCtx ctx;
	GxGenericShaders sh;
	VkShaderModule mutantFs = VK_NULL_HANDLE;
	VkShaderModule a2cFs = VK_NULL_HANDLE, a2cFsFlat = VK_NULL_HANDLE;   // B · a2c-1: the alpha-to-coverage family
	TestKit::FFLayout layout;
	TestKit::FFResources res;
	GxAttachConfig cfg = {};
	VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
	VmaAllocation colorMem = nullptr, depthMem = nullptr;
	VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
	VkBuffer vb = VK_NULL_HANDLE, nullVb = VK_NULL_HANDLE, rb = VK_NULL_HANDLE;
	VmaAllocation vbMem = nullptr, nullMem = nullptr, rbMem = nullptr;
	uint8_t* vbPtr = nullptr;
	uint8_t* rbPtr = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkQueryPool occlusion = VK_NULL_HANDLE;   // samples that passed (raster + discard + depth) in the last render() with readback
	uint64_t lastSamples = 0;
	static constexpr uint32_t W = 64, kVbBytes = 8u << 20;

	VkShaderModule module(const uint32_t* code, size_t bytes) {
		VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		ci.codeSize = bytes;
		ci.pCode = code;
		VkShaderModule m = VK_NULL_HANDLE;
		vkCreateShaderModule(ctx.device(), &ci, nullptr, &m);
		return m;
	}
	bool buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& b, VmaAllocation& m, uint8_t** ptr) {
		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = size;
		bi.usage = usage;
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo info = {};
		if (vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b, &m, &info) != VK_SUCCESS)
			return false;
		std::memset(info.pMappedData, 0, size_t(size));
		vmaFlushAllocation(ctx.allocator(), m, 0, VK_WHOLE_SIZE);   // the zeros (the null stream's) reach the GPU (Codex rw2-D-r2)
		if (ptr)
			*ptr = static_cast<uint8_t*>(info.pMappedData);
		return true;
	}
	bool image(VkFormat fmt, bool isDepth, VkImage& img, VmaAllocation& mem, VkImageView& view) {
		VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fmt;
		ii.extent = { W, W, 1 };
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
		                   : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		VmaAllocationCreateInfo ai = {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		if (vmaCreateImage(ctx.allocator(), &ii, &ai, &img, &mem, nullptr) != VK_SUCCESS)
			return false;
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = img;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = fmt;
		vi.subresourceRange = { VkImageAspectFlags(isDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1 };
		return vkCreateImageView(ctx.device(), &vi, nullptr, &view) == VK_SUCCESS;
	}

	bool init(const std::string& device, bool validation, std::string* why) {
		Vk::VkCtxDesc desc;
		desc.validation = validation;
		desc.syncValidation = true;
		desc.deviceSelect = device.empty() ? nullptr : device.c_str();
		desc.log = captureLog;
		if (!ctx.init(desc, why))
			return false;
		std::fprintf(stderr, "[GXFF] %s\n", ctx.deviceLine().c_str());
		if (validation && (!ctx.validationEnabled() || !ctx.syncValidationEnabled())) {
			*why = "--validation refused: core + sync validation are not both running";
			return false;
		}
		ctx.resetValidationCounters();
		sh.vs = module(spv_gx_ff_vert, sizeof(spv_gx_ff_vert));
		sh.fs = module(spv_gx_ff_frag, sizeof(spv_gx_ff_frag));
		sh.fsFlat = module(spv_gx_ff_flat_frag, sizeof(spv_gx_ff_flat_frag));
		mutantFs = module(spv_gx_ff_mutant_frag, sizeof(spv_gx_ff_mutant_frag));
		a2cFs = module(spv_gx_ff_a2c_frag, sizeof(spv_gx_ff_a2c_frag));
		a2cFsFlat = module(spv_gx_ff_flat_a2c_frag, sizeof(spv_gx_ff_flat_a2c_frag));
		if (!layout.create(ctx.device()) || !res.create(ctx, layout, 8u << 20, 7)) {
			*why = "FF layout / resources";
			return false;
		}
		sh.layout = layout.layout;
		std::vector<GxAttachConfig> cfgs;
		if (!GxSessionAttachConfigs(ctx, 21u /* D3DFMT_A8R8G8B8 */, 1, cfgs, why))
			return false;
		for (const GxAttachConfig& c : cfgs)
			if (c.depthFormat)
				cfg = c;
		if (!cfg.depthFormat) {
			*why = "no depth configuration";
			return false;
		}
		if (!image(VkFormat(cfg.colorFormat), false, color, colorMem, colorView) ||
		    !image(VkFormat(cfg.depthFormat), true, depth, depthMem, depthView) ||
		    !buffer(kVbBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vb, vbMem, &vbPtr) ||
		    !buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullVb, nullMem, nullptr) ||
		    !buffer(W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rb, rbMem, &rbPtr)) {
			*why = "targets / buffers";
			return false;
		}
		VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = ctx.queue(Vk::QueueKind::Graphics).family;
		vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool);
		VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qi.queryType = VK_QUERY_TYPE_OCCLUSION;
		qi.queryCount = 1;
		if (vkCreateQueryPool(ctx.device(), &qi, nullptr, &occlusion) != VK_SUCCESS) {
			*why = "occlusion query pool";
			return false;
		}
		return true;
	}

	struct Draw {
		VkPipeline pipe;
		const GxStructKey* key;
		const GxFFVsBlock* vs;
		const GxFFPsBlock* ps;
		const GxFFSpecBlock* spec;
		uint32_t vbOffset, vertexCount;
		uint32_t strides[GxNullBinding];
	};
	// One submit: clear, every draw, optional readback of the colour target into rbPtr.
	bool render(const std::vector<Draw>& draws, bool readback) {
		const VkDevice dev = ctx.device();
		VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkCommandBuffer cb = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(dev, &cai, &cb);
		VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cb, &cbi);
		VkImageMemoryBarrier2 b[2] = {};
		for (VkImageMemoryBarrier2& x : b) {
			x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			x.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		}
		// the previous submit's copy / attachment writes completed (we waited on it); UNDEFINED discards the old contents
		b[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		b[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		b[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
		b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		b[0].image = color;
		b[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		b[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		b[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		b[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		b[1].image = depth;
		b[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
		VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dep.imageMemoryBarrierCount = 2;
		dep.pImageMemoryBarriers = b;
		vkCmdPipelineBarrier2(cb, &dep);
		if (readback)
			vkCmdResetQueryPool(cb, occlusion, 0, 1);
		VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		ca.imageView = colorView;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ca.clearValue.color = { { 0.25f, 0.5f, 0.75f, 0.5f } };
		VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
		da.imageView = depthView;
		da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		da.clearValue.depthStencil = { 1.0f, 0 };
		VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
		ri.renderArea = { { 0, 0 }, { W, W } };
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		ri.pDepthAttachment = &da;
		ri.pStencilAttachment = &da;
		vkCmdBeginRendering(cb, &ri);
		if (readback)
			vkCmdBeginQuery(cb, occlusion, 0, 0);
		const VkViewport vp = { 0, 0, float(W), float(W), 0, 1 };
		const VkRect2D sc = { { 0, 0 }, { W, W } };
		for (const Draw& d : draws) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe);
			vkCmdSetViewport(cb, 0, 1, &vp);
			vkCmdSetScissor(cb, 0, 1, &sc);
			vkCmdSetDepthBias(cb, 0, 0, 0);
			const float bc[4] = { 0, 0, 0, 0 };
			vkCmdSetBlendConstants(cb, bc);
			vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
			vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
			vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
			vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
			vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
			static const VkPrimitiveTopology topo[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
			                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
			vkCmdSetPrimitiveTopology(cb, topo[d.key->topologyClass < 3 ? d.key->topologyClass : 2]);
			vkCmdSetDepthTestEnable(cb, VK_TRUE);
			vkCmdSetDepthWriteEnable(cb, VK_TRUE);
			vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
			vkCmdSetStencilTestEnable(cb, VK_FALSE);
			vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
			                  VK_COMPARE_OP_ALWAYS);
			vkCmdSetDepthBiasEnable(cb, VK_FALSE);
			// real streams laid out back to back from vbOffset (makeVertices), the null stream at stride 0
			VkDeviceSize at = d.vbOffset;
			for (uint32_t bnd = 0; bnd < GxNullBinding; bnd++) {
				if (!d.strides[bnd])
					continue;
				const VkDeviceSize stride = d.strides[bnd];
				vkCmdBindVertexBuffers2(cb, bnd, 1, &vb, &at, nullptr, &stride);
				at += stride * d.vertexCount;
			}
			const VkDeviceSize zero = 0;
			vkCmdBindVertexBuffers2(cb, GxNullBinding, 1, &nullVb, &zero, nullptr, &zero);
			res.bind(cb, layout.layout, *d.vs, *d.ps, *d.spec);
			vkCmdDraw(cb, d.vertexCount, 1, 0, 0);
		}
		if (readback)
			vkCmdEndQuery(cb, occlusion, 0);
		vkCmdEndRendering(cb);
		if (readback) {
			VkImageMemoryBarrier2 t = b[0];
			t.srcStageMask = g_plantHazard ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			t.srcAccessMask = g_plantHazard ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			t.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			t.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			t.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			t.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			VkDependencyInfo d2 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			d2.imageMemoryBarrierCount = 1;
			d2.pImageMemoryBarriers = &t;
			vkCmdPipelineBarrier2(cb, &d2);
			VkBufferImageCopy r = {};
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.imageExtent = { W, W, 1 };
			vkCmdCopyImageToBuffer(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &r);
			VkBufferMemoryBarrier2 h = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
			h.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			h.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			h.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
			h.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
			h.srcQueueFamilyIndex = h.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			h.buffer = rb;
			h.size = VK_WHOLE_SIZE;
			VkDependencyInfo d3 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			d3.bufferMemoryBarrierCount = 1;
			d3.pBufferMemoryBarriers = &h;
			vkCmdPipelineBarrier2(cb, &d3);
		}
		vkEndCommandBuffer(cb);
		Vk::SubmitDesc sd;
		sd.commandBuffers.push_back(cb);
		const uint64_t v = ctx.submit(Vk::QueueKind::Graphics, sd);
		const bool ok = v && ctx.queue(Vk::QueueKind::Graphics).timeline.wait(v);
		vkFreeCommandBuffers(dev, pool, 1, &cb);
		if (ok && readback) {
			vmaInvalidateAllocation(ctx.allocator(), rbMem, 0, VK_WHOLE_SIZE);
			lastSamples = 0;
			if (vkGetQueryPoolResults(dev, occlusion, 0, 1, sizeof(lastSamples), &lastSamples, sizeof(lastSamples),
			                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
				return false;
		}
		return ok;
	}

	void destroy() {
		const VkDevice dev = ctx.device();
		if (!dev)
			return;
		ctx.waitIdle();   // R19: drains the submit thread first
		if (pool) vkDestroyCommandPool(dev, pool, nullptr);
		if (occlusion) vkDestroyQueryPool(dev, occlusion, nullptr);
		for (auto [b, m] : { std::pair{ vb, vbMem }, std::pair{ nullVb, nullMem }, std::pair{ rb, rbMem } })
			if (b) vmaDestroyBuffer(ctx.allocator(), b, m);
		if (colorView) vkDestroyImageView(dev, colorView, nullptr);
		if (depthView) vkDestroyImageView(dev, depthView, nullptr);
		if (color) vmaDestroyImage(ctx.allocator(), color, colorMem);
		if (depth) vmaDestroyImage(ctx.allocator(), depth, depthMem);
		res.destroy();
		layout.destroy(dev);
		for (VkShaderModule m : { sh.vs, sh.fs, sh.fsFlat, mutantFs, a2cFs, a2cFsFlat })
			if (m) vkDestroyShaderModule(dev, m, nullptr);
	}
};

// One prepared FF draw: the snapshot packed, its vertices in the vertex buffer at vbOffset.
struct Prepared {
	GxPipelineKey key;
	GxFFVsBlock vs;
	GxFFPsBlock ps;
	GxFFSpecBlock spec;
	uint32_t vbOffset = 0, vertexCount = 0, strides[GxNullBinding] = {};
	int frame = -1;
	bool modulate = false;   // an active stage uses D3DTOP_MODULATE (what the mutant changes)
};

bool prepare(Gpu& g, const FFEntry& e, const GxD3D8FFState& s, uint32_t seed, uint32_t& vbHead, Prepared& out, const char** why) {
	if (!GxPackFF(s, &out.vs, &out.ps, &out.spec, why))
		return false;
	out.key = {};
	out.key.s = GxMakeKey(e.part, g.cfg);
	out.vertexCount = e.part.topologyClass == GxTopoPoints ? 96 : e.part.topologyClass == GxTopoLines ? 96 : 3 * 32;
	const std::vector<uint8_t> v = makeVertices(e.part, (s.fvf & FVF_POSITION_MASK) == FVF_XYZRHW, out.vertexCount, seed, out.strides);
	const uint32_t at = (vbHead + 15u) & ~15u;
	if (at + v.size() > Gpu::kVbBytes) {
		*why = "vertex buffer full";
		return false;
	}
	std::memcpy(g.vbPtr + at, v.data(), v.size());
	vmaFlushAllocation(g.ctx.allocator(), g.vbMem, at, v.size());   // no-op on coherent memory (Codex rw2-D-r1 #3)
	out.vbOffset = at;
	vbHead = at + uint32_t(v.size());
	out.frame = e.frame;
	for (uint32_t i = 0; i < 8; i++) {
		const uint32_t ops = (out.spec.w[GxFFW_StageOps01 + i / 2] >> ((i & 1) * 16)) & 0xffffu;
		if ((ops & 0x1fu) == TOP_DISABLE)
			break;
		out.modulate = out.modulate || (ops & 0x1fu) == TOP_MODULATE || ((ops >> 8) & 0x1fu) == TOP_MODULATE;
	}
	return true;
}

// The synthetic coverage the census never met: fog modes, alpha funcs, specular, and a colour/alpha op sweep.
void synthesize(const std::vector<FFEntry>& census, std::vector<std::pair<FFEntry, int>>& out) {
	int k = 0;
	for (const FFEntry& e : census) {
		FFEntry x = e;
		int* sp = x.ff + kVsFields;
		sp[1] = 1;                                         // fog on
		if (k & 1) { sp[2] = 0; sp[3] = 1 + k % 3; }       // pixel fog EXP / EXP2 / LINEAR
		else { sp[2] = 1 + k % 3; sp[3] = 0; }             // vertex fog
		sp[0] = k % 8;                                     // VkCompareOp NEVER..ALWAYS
		x.ff[kFFFields - 1] = k & 1;                       // specular add
		x.ortho = k % 4 == 2;                              // every 4th: orthographic, Z fog
		x.src += x.ortho ? "+fog/alpha/zfog" : "+fog/alpha";
		out.push_back({ x, k });
		k++;
	}
	// op sweep: the first census entry with two bound stages, stage 0/1 ops cycled through every D3DTOP
	const FFEntry* base = nullptr;
	for (const FFEntry& e : census)
		if (e.ff[kVsFields + kSpecFields + 10] && e.ff[kVsFields + kSpecFields + kStageFields + 10] && !base)
			base = &e;
	if (!base)
		return;
	static const int args[6] = { 0x02, 0x00, 0x03, 0x01, 0x12, 0x24 };   // TEXTURE, DIFFUSE, TFACTOR, CURRENT, TEXTURE|COMP, SPECULAR|AREP
	for (int op = 2; op <= 26; op++) {
		FFEntry x = *base;
		int* s0 = x.ff + kVsFields + kSpecFields;
		int* s1 = s0 + kStageFields;
		s0[0] = op; s0[1] = args[op % 6]; s0[2] = args[(op + 1) % 6]; s0[3] = args[(op + 2) % 6];
		s0[4] = 2 + (op * 5) % 25; s0[5] = args[(op + 3) % 6]; s0[6] = args[(op + 4) % 6]; s0[7] = args[(op + 5) % 6];
		s0[8] = op % 3 == 0;                                                                // result to TEMP
		s1[0] = 2 + (op * 7) % 25; s1[1] = 0x05; s1[2] = 0x01; s1[3] = 0x02;               // reads TEMP / CURRENT / TEXTURE
		s1[4] = 2 + (op * 3) % 25; s1[5] = 0x01; s1[6] = 0x02; s1[7] = 0x05;
		x.src += "+op" + std::to_string(op);
		out.push_back({ x, 1000 + op });
	}
}

int runIdentity(Gpu& g, const std::vector<std::string>& streams, bool synthetic, bool mutant) {
	std::vector<FFEntry> all;
	for (const std::string& p : streams)
		if (!readFFStream(p, all))
			return 2;
	// distinct {draw part, FF fields}: the frame and source do not change the pixels
	std::vector<FFEntry> distinct;
	std::set<std::string> seen;
	for (const FFEntry& e : all) {
		std::string k(reinterpret_cast<const char*>(&e.part), sizeof(e.part));
		k.append(reinterpret_cast<const char*>(e.ff), sizeof(e.ff));
		if (seen.insert(k).second)
			distinct.push_back(e);
	}
	std::vector<std::pair<FFEntry, int>> work;
	for (size_t i = 0; i < distinct.size(); i++)
		work.push_back({ distinct[i], int(i) });
	if (synthetic)
		synthesize(distinct, work);

	GxPipelineCache cache;
	std::string why;
	if (!cache.init(g.ctx, g.sh, "", 0, &why)) {
		std::fprintf(stderr, "[GXFF] cache init: %s\n", why.c_str());
		return 2;
	}
	g_gxPipeTestMutants.syncSpec = true;   // identity compares finished pipelines: build each specialised one on the spot
	g_gxPipeTestMutants.specFs = mutant ? g.mutantFs : g_a2cFamily ? g.a2cFs : VK_NULL_HANDLE;
	g_gxPipeTestMutants.specFsFlat = g_a2cFamily && !mutant ? g.a2cFsFlat : VK_NULL_HANDLE;
	g_gxPipeTestMutants.specCorrupt = g_specCorrupt;
	g_gxPipeTestMutants.specCorruptVs = g_specCorruptVs;
	int ran = 0, identical = 0, differing = 0, refused = 0, notSpecialised = 0, failed = 0, modulateKeys = 0, modulateDiff = 0;
	int blank = 0, blankAlpha = 0, colourUnchanged = 0, a2cBitEntries = 0;
	uint32_t minTouched = Gpu::W * Gpu::W;
	uint64_t totalTouched = 0;
	uint64_t diffPixels = 0;
	int maxDelta = 0;
	std::vector<uint8_t> a(Gpu::W * Gpu::W * 4), b(a.size());
	for (const auto& [e, seed] : work) {
		GxD3D8FFState s;
		toSnapshot(e, uint32_t(seed) * 7919u + 17u, s);
		Prepared p;
		uint32_t vbHead = 0;
		const char* reason = nullptr;
		if (!prepare(g, e, s, uint32_t(seed) * 104729u + 3u, vbHead, p, &reason)) {
			refused++;
			std::printf("[GXFF] refused %s#%d: %s\n", e.src.c_str(), seed, reason ? reason : "?");
			continue;
		}
		if (g_blankMutant) {   // TEST-ONLY: what a VS regression looks like - every vertex collapses to the origin
			std::memset(p.vs.Projection, 0, sizeof(p.vs.Projection));
			std::memset(p.vs.ViewportInverseExtent, 0, sizeof(p.vs.ViewportInverseExtent));
		}
		if (g_a2cBit && (p.spec.w[GxFFW_AlphaTestAndModeArgs] & 7u) != 7u) {   // TEST-ONLY fail arm: the ramp in force on the a2c side
			p.spec.w[GxFFW_AlphaTestAndModeArgs] |= GxFFA2CBit;
			a2cBitEntries++;
		}
		const VkPipeline gen = cache.get(p.key, 0);
		bool isSpec = false;
		const VkPipeline spec = cache.getFF(p.key, p.spec, 0, &isSpec);
		if (!gen || !spec || !isSpec) {
			(isSpec ? failed : notSpecialised)++;
			std::printf("[GXFF] no %s pipeline for %s#%d\n", gen ? "specialised" : "generic", e.src.c_str(), seed);
			continue;
		}
		Gpu::Draw d = { gen, &p.key.s, &p.vs, &p.ps, &p.spec, p.vbOffset, p.vertexCount, {} };
		std::memcpy(d.strides, p.strides, sizeof(d.strides));
		if (!g.render({ d }, true)) { failed++; continue; }
		std::memcpy(a.data(), g.rbPtr, a.size());
		const uint64_t samples = g.lastSamples;
		d.pipe = spec;
		if (!g.render({ d }, true)) { failed++; continue; }
		std::memcpy(b.data(), g.rbPtr, b.size());
		ran++;
		uint32_t px = 0;
		int md = 0;
		for (size_t i = 0; i < a.size(); i += 4) {
			bool diff = false;
			for (int c = 0; c < 4; c++) {
				const int dd = std::abs(int(a[i + c]) - int(b[i + c]));
				md = std::max(md, dd);
				diff = diff || dd;
			}
			px += diff;
		}
		if (p.modulate) modulateKeys++;
		if (px) {
			differing++;
			diffPixels += px;
			maxDelta = std::max(maxDelta, md);
			if (p.modulate) modulateDiff++;
			if (differing <= 12)
				std::printf("[GXFF] DIFF %s#%d: %u px, max |d| %d, spec %08x %08x %08x stage0 %08x/%08x\n", e.src.c_str(), seed, px, md,
				            p.spec.w[0], p.spec.w[1], p.spec.w[2], p.spec.w[GxFFW_StageOps01], p.spec.w[GxFFW_StageArgs0]);
		} else {
			identical++;
		}
		// Coverage: an entry that draws nothing compares equal and proves nothing (rw2-D adversarial review P2-1). The signal
		// is the occlusion query - samples that survived rasterisation, discard and the depth test - not "pixels unlike the
		// clear colour", which a colour-write-mask-0 part (5 census depth-only draws) or a zero-alpha blend never changes.
		// Every entry must pass samples, except one whose alpha test can reject everything (any func but ALWAYS).
		uint32_t touched = 0;   // colour pixels changed: reported (an entry can pass samples yet leave colour as it was)
		for (size_t i = 0; i < a.size(); i += 4)
			touched += !(a[i] == 0xbf && a[i + 1] == 0x80 && a[i + 2] == 0x40 && a[i + 3] == 0x80);   // clear = B 0.75 G 0.5 R 0.25 A 0.5
		const bool alphaTested = (p.spec.w[GxFFW_AlphaTestAndModeArgs] & 7u) != 7u;
		if (!samples)
			(alphaTested ? blankAlpha : blank)++;
		if (!samples && !alphaTested && blank <= 6)
			std::printf("[GXFF] BLANK %s#%d: no sample passed\n", e.src.c_str(), seed);
		if (samples && !touched)
			colourUnchanged++;
		if (!alphaTested)
			minTouched = std::min(minTouched, uint32_t(std::min<uint64_t>(samples, 0xffffffffu)));
		totalTouched += touched;
	}
	g_gxPipeTestMutants.syncSpec = false;
	g_gxPipeTestMutants.specFs = VK_NULL_HANDLE;
	g_gxPipeTestMutants.specFsFlat = VK_NULL_HANDLE;
	g_gxPipeTestMutants.specCorrupt = false;
	g_gxPipeTestMutants.specCorruptVs = false;
	const GxPipeStats st = cache.stats();
	cache.shutdown();
	const Vk::ValidationCounters vc = g.ctx.validation();
	const uint32_t vErr = vc.errors + vc.syncHazards;
	if (vc.errors)
		std::printf("[GXFF] first validation error: %s\n", vc.firstError.c_str());
	const bool pass = g_plantHazard ? vc.syncHazards > 0 : g_blankMutant ? blank > 0
	                : (mutant || g_specCorrupt || g_specCorruptVs || g_a2cBit) ? (differing > 0 && (!g_a2cBit || a2cBitEntries > 0))
	                : (differing == 0 && ran > 0 && failed == 0 && notSpecialised == 0 && refused == 0 && blank == 0 && vErr == 0);
	std::printf("[GXFF] identity entries=%zu ran=%d identical=%d differing=%d diff_px=%llu max_delta=%d modulate_keys=%d "
	            "modulate_differing=%d refused=%d not_specialised=%d failed=%d blank=%d blank_alpha_tested=%d min_samples=%u "
	            "colour_unchanged=%d mean_px=%llu "
	            "spec_compiled=%u spec_max_ms=%.1f "
	            "validation_errors=%u (sync %u) arm=%s verdict=%s\n", work.size(), ran, identical, differing,
	            (unsigned long long)diffPixels, maxDelta, modulateKeys, modulateDiff, refused, notSpecialised, failed, blank, blankAlpha, minTouched, colourUnchanged,
	            (unsigned long long)(ran ? totalTouched / uint64_t(ran) : 0),
	            st.specCompiled, st.specMaxMs, vErr, vc.syncHazards, g_plantHazard ? "plant-hazard" : g_blankMutant ? "blank-mutant" : g_specCorrupt ? "spec-corrupt"
	            : g_specCorruptVs ? "spec-corrupt-vs" : mutant ? "mutant" : g_a2cBit ? "a2c-bit" : g_a2cFamily ? "a2c-family" : "none",
	            pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int runCold(Gpu& g, const std::string& stream, const std::string& setArg, const std::string& cacheDir, bool syncSpec, int gap,
            int expectMiss, int lookups) {
	std::vector<FFEntry> all;
	if (!readFFStream(stream, all))
		return 2;
	std::stable_sort(all.begin(), all.end(), [](const FFEntry& x, const FFEntry& y) { return x.frame < y.frame; });
	std::vector<GxDrawPart> parts;
	if (setArg == "builtin") {
		parts.assign(std::begin(kBuiltinSet), std::end(kBuiltinSet));
	} else {
		std::vector<FFEntry> setEntries;
		if (!readFFStream(setArg, setEntries))
			return 2;
		for (const FFEntry& e : setEntries) {
			bool dup = false;
			for (const GxDrawPart& p : parts)
				dup = dup || !std::memcmp(&p, &e.part, sizeof(p));
			if (!dup)
				parts.push_back(e.part);
		}
	}
	GxPipelineCache cache;
	std::string why;
	if (!cache.init(g.ctx, g.sh, cacheDir, 0, &why)) {
		std::fprintf(stderr, "[GXFF] cache init: %s\n", why.c_str());
		return 2;
	}
	cache.prebuild(parts.data(), parts.size(), &g.cfg, 1, 4);
	cache.waitPrebuild();
	const GxPipeStats pre = cache.stats();
	// The generic set is the contract the replay rests on: an incomplete prebuild fails here, before any timing (Codex rw2-D-r3).
	if (!cache.prebuildComplete()) {
		std::printf("[GXFF] cold prebuild INCOMPLETE (prebuilt=%u prebuild_failed=%u) verdict=FAIL\n", pre.prebuilt, pre.prebuildFailed);
		cache.shutdown();
		return 1;
	}
	g_gxPipeTestMutants.syncSpec = syncSpec;
	std::vector<Prepared> live;
	std::set<std::string> seen;
	uint32_t vbHead = 0;
	std::vector<double> getMs, frameMs;
	double worstGet = 0, worstGetCpu = 0, worstCall = 0, worstCallCpu = 0, cpuAtWorst = 0;
	// Thread CPU time tells "off the CPU" (preempted) from "computing" in a slow frame. POSIX only: MSVC's CRT has no
	// clock_gettime and GetThreadTimes ticks at the scheduler quantum, so Windows reports -1 (n/a) (Codex rw2-D-r1 #2).
	auto threadCpuMs = [] {
#if defined(_WIN32)
		return 0.0;
#else
		timespec ts;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
#endif
	};
#if defined(_WIN32)
	const bool haveCpu = false;
#else
	const bool haveCpu = true;
#endif
	int worstFrame = -1, frames = 0, arrivals = 0, prepFailed = 0, nullPipes = 0, renderFailed = 0;
	uint64_t drawn = 0;
	size_t next = 0;
	while (next < all.size()) {
		const int f = all[next].frame;
		while (next < all.size() && all[next].frame == f) {   // this frame's new pipelines
			const FFEntry& e = all[next++];
			std::string k(reinterpret_cast<const char*>(&e.part), sizeof(e.part));
			k.append(reinterpret_cast<const char*>(e.ff), sizeof(e.ff));
			if (!seen.insert(k).second)
				continue;
			GxD3D8FFState s;
			toSnapshot(e, uint32_t(live.size()) * 7919u + 17u, s);
			Prepared p;
			const char* reason = nullptr;
			if (prepare(g, e, s, uint32_t(live.size()) * 104729u + 3u, vbHead, p, &reason)) {
				live.push_back(p);
				arrivals++;
			} else {
				prepFailed++;
				std::printf("[GXFF] cold: %s f%d not prepared: %s\n", e.src.c_str(), e.frame, reason ? reason : "?");
			}
		}
		const int until = next < all.size() ? std::min(all[next].frame - f, gap) : gap;   // frames until the next arrival
		for (int i = 0; i < std::max(until, 1); i++) {
			const auto t0 = std::chrono::steady_clock::now();
			std::vector<Gpu::Draw> draws;
			double tGet = 0, tCpu = 0;
			// --lookups N: a real frame looks a pipeline up per DRAW (thousands), not per distinct pipeline: getFF is called N
			// times cycling over the live set; each live pipeline is still drawn once (rw2-D adversarial review P3-8).
			const size_t calls = std::max(size_t(lookups > 0 ? lookups : 0), live.size());   // every live entry at least once (Codex rw2-D-r3)
			for (size_t c = 0; c < calls && !live.empty(); c++) {
				const Prepared& p = live[c % live.size()];
				const double c0 = threadCpuMs();
				const auto g0 = std::chrono::steady_clock::now();
				const VkPipeline pipe = cache.getFF(p.key, p.spec, uint32_t(f + i), nullptr);
				const double call = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count();
				const double callCpu = threadCpuMs() - c0;
				tCpu += callCpu;
				tGet += call;
				if (call > worstCall) {
					worstCall = call;
					worstCallCpu = callCpu;   // wall >> cpu = the thread was off the CPU (preempted), not computing
				}
				if (!pipe) {
					nullPipes++;
					continue;
				}
				if (c >= live.size())
					continue;   // the extra lookups only time getFF
				Gpu::Draw d = { pipe, &p.key.s, &p.vs, &p.ps, &p.spec, p.vbOffset, p.vertexCount, {} };
				std::memcpy(d.strides, p.strides, sizeof(d.strides));
				draws.push_back(d);
			}
			if (g.render(draws, false))
				drawn += draws.size();
			else
				renderFailed++;
			const double fm = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			getMs.push_back(tGet);
			frameMs.push_back(fm);
			worstGetCpu = std::max(worstGetCpu, tCpu);
			if (tGet > worstGet) {
				worstGet = tGet;
				worstFrame = f + i;
				cpuAtWorst = tCpu;
			}
			frames++;
		}
	}
	g_gxPipeTestMutants.syncSpec = false;
	cache.waitSpecIdle();
	const GxPipeStats st = cache.stats();
	cache.shutdown();
	auto pct = [](std::vector<double> v, double q) {
		if (v.empty()) return 0.0;
		std::sort(v.begin(), v.end());
		return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1)))];
	};
	int over = 0;
	for (double x : getMs)
		over += x > 1.0;
	const Vk::ValidationCounters vc = g.ctx.validation();
	const uint32_t vErr = vc.errors + vc.syncHazards;
	if (vc.errors)
		std::printf("[GXFF] first validation error: %s\n", vc.firstError.c_str());
	// Pre-registered (PLAN_rw2-D.md task 7): the draw thread never compiles a specialised variant: getFF time per frame
	// <= 1 ms at every frame and prebuild_wait 0. The fail arm (--sync-spec) must break the first clause.
	const bool noHitch = worstGet <= 1.0 && st.prebuildWait == 0;
	// The run must have done the work it claims (Codex rw2-D-r1 #4): every stream entry prepared, every lookup a pipeline,
	// every frame submitted, and something drawn - an empty or broken stream is a FAIL, never a timing PASS.
	// ...and the specialised path must really have run (rw2-D adversarial review P3-4): something compiled, nothing failed.
	const bool ran = arrivals > 0 && drawn > 0 && prepFailed == 0 && nullPipes == 0 && renderFailed == 0 &&
	                 st.specCompiled > 0 && st.specFailed == 0;
	// --expect-miss N (the unseen-map arm): N generic misses are the expected outcome - each one a compile on the caller,
	// i.e. a hitch the miss record bounds to once per machine - so the verdict is the miss count, not the no-hitch rule.
	// Without --expect-miss a generic miss is a FAIL however fast it compiled (Codex rw2-D-r3): the set must cover the stream.
	const bool pass = ran && (expectMiss >= 0 ? (st.genericMiss == uint32_t(expectMiss) && st.prebuildWait == 0 && vErr == 0)
	                          : syncSpec ? (!noHitch && st.genericMiss == 0) : (noHitch && st.genericMiss == 0 && vErr == 0));
	std::printf("[GXFF] cold lookups_per_frame=%d stream=%s set=%s parts=%zu prebuilt=%u prebuild_ms=%.1f frames=%d arrivals=%d live=%zu "
	            "generic_miss=%u prebuild_wait=%u spec_queued=%u spec_compiled=%u spec_failed=%u spec_fallback_draws=%llu "
	            "spec_max_ms=%.1f get_ms_per_frame p50=%.3f p99=%.3f max=%.3f (frame %d) frames_get_over_1ms=%d "
	            "frame_ms p50=%.2f p99=%.2f max=%.2f get_cpu_ms max=%.3f (at the worst frame %.3f) worst_call_ms=%.3f (cpu %.3f) "
	            "handover_deferred=%llu drawn=%llu prep_failed=%d null_pipes=%d render_failed=%d validation_errors=%u arm=%s verdict=%s\n",
	            lookups > 0 ? lookups : int(live.size()), stream.c_str(), setArg.c_str(), parts.size(), pre.prebuilt, pre.prebuildMs, frames, arrivals, live.size(),
	            st.genericMiss, st.prebuildWait, st.specQueued, st.specCompiled, st.specFailed,
	            (unsigned long long)st.specFallbackDraws, st.specMaxMs, pct(getMs, 0.5), pct(getMs, 0.99), worstGet, worstFrame,
	            over, pct(frameMs, 0.5), pct(frameMs, 0.99), pct(frameMs, 1.0), haveCpu ? worstGetCpu : -1.0, haveCpu ? cpuAtWorst : -1.0,
	            worstCall, haveCpu ? worstCallCpu : -1.0,
	            (unsigned long long)st.specHandoverDeferred, (unsigned long long)drawn, prepFailed, nullPipes, renderFailed, vErr, expectMiss >= 0 ? "expect-miss" : syncSpec ? "sync-spec" : "none",
	            pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	std::vector<std::string> identity;
	std::string cold, setArg = "builtin", cacheDir, device;
	bool layout = false, layoutMutant = false, synthetic = false, mutant = false, validation = false, syncSpec = false;
	int gap = 4, expectMiss = -1, lookups = 0;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--layout") layout = true;
		else if (a == "--layout-mutant") layout = layoutMutant = true;
		else if (a == "--identity") { while (i + 1 < argc && argv[i + 1][0] != '-') identity.push_back(argv[++i]); }
		else if (a == "--synthetic") synthetic = true;
		else if (a == "--mutant") mutant = true;
		else if (a == "--cold") cold = next();
		else if (a == "--set") setArg = next();
		else if (a == "--cache-dir") cacheDir = next();
		else if (a == "--sync-spec") syncSpec = true;
		else if (a == "--gap") gap = std::max(1, std::atoi(next().c_str()));
		else if (a == "--expect-miss") expectMiss = std::atoi(next().c_str());
		else if (a == "--validation") validation = true;
		else if (a == "--plant-hazard") g_plantHazard = true;
		else if (a == "--spec-corrupt") g_specCorrupt = true;
		else if (a == "--spec-corrupt-vs") g_specCorruptVs = true;
		else if (a == "--blank-mutant") g_blankMutant = true;
		else if (a == "--a2c-family") g_a2cFamily = true;
		else if (a == "--a2c-bit") g_a2cFamily = g_a2cBit = true;
		else if (a == "--fail-prebuild") g_gxPipeTestMutants.failLastPrebuildKey = true;   // fail arm: an incomplete generic set
		else if (a == "--lookups") lookups = std::max(0, std::atoi(next().c_str()));
		else if (a == "--device") device = next();
		else { std::fprintf(stderr, "[GXFF] unknown argument %s\n", a.c_str()); return 2; }
	}
	if (layout)
		return runLayout(layoutMutant);
	if (identity.empty() && cold.empty()) {
		std::fprintf(stderr, "usage: gx_ff_test --layout [--layout-mutant] | --identity <ffstream>... [--synthetic] [--mutant] | "
		                     "--cold <ffstream> [--set builtin|<ffstream>] [--cache-dir d] [--sync-spec] [--gap n]\n");
		return 2;
	}
	Gpu g;
	std::string why;
	if (!g.init(device, validation, &why)) {
		std::fprintf(stderr, "[GXFF] init: %s\n", why.c_str());
		g.destroy();
		return 2;
	}
	const int rc = !identity.empty() ? runIdentity(g, identity, synthetic, mutant) : runCold(g, cold, setArg, cacheDir, syncSpec, gap, expectMiss, lookups);
	g.destroy();
	return rc;
}
