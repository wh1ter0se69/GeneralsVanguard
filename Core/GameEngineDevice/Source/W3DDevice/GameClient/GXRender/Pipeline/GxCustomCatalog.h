// GxCustomCatalog.h - the custom-shader catalogue (WS-E, rw2-E, 11/10/2026; PLAN §3 WS-E, DESIGN_pipeline-cache.md "custom").
//
// The game loads its .pso/.vso bytecode through the file system (W3DShaderManager.cpp LoadAndCreateD3DShader), so a mod can
// replace terrain.pso under the same name. The native renderer therefore never trusts a NAME: a port is selected by the
// BYTECODE (every token, compared in full after a hash lookup) and, for a vertex shader, the vertex DECLARATION too. Bytes this
// catalogue does not know select CLASSIC for the whole session (GxCsSessionVerdict), decided before gameplay and held for it.
// A known blob with no port yet (program 0) is reported as such; the lead decides whether a session may go native with one.
#pragma once

#include "GxPipelineKey.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GXRender {

// Program ids = the values of gxCsProgram (spec constant 40 of gx_cs_ps.frag) / the vertex module's identity. 0 = none (FF).
enum GxCsPsId : uint8_t {
	GxCsPsNone = 0, GxCsPsFTerrain, GxCsPsFTerrain0, GxCsPsFTerrainNoise, GxCsPsFTerrainNoise2,
	GxCsPsTerrain, GxCsPsTerrainNoise, GxCsPsTerrainNoise2, GxCsPsRoadNoise2,
	GxCsPsTrees, GxCsPsMonochrome, GxCsPsWave, GxCsPsWaterRiver, GxCsPsWaterWater, GxCsPsWaterTrapezoid,
	GxCsPsVelocity,   // gx:vel-ps (GxSyntheticPrograms.h): its own module, gx_cs_vel.frag, not a case of gx_cs_ps.frag
	GxCsPsWaterGlare, GxCsPsWaterFoam,   // water:glare / water:foam (live water, GameClient/GXWaterLive.h): cases of gx_cs_ps.frag again
	GxCsPsGroundBase, GxCsPsGroundMult, GxCsPsGroundMultLite,   // gx:ground-* (the remastered ground, TerrainTex.cpp gxgEnsureDevice)
	GxCsPsWaterRA3,   // water:ra3 (RA3 water's sheen, GameClient/GXWaterRA3.h): a case of gx_cs_ps.frag
	GxCsPsWaterRA3River,   // water:ra3river (RA3 water's sheen on rivers, the same header): a case of gx_cs_ps.frag
	GxCsPsCount
};
enum GxCsVsId : uint8_t { GxCsVsNone = 0, GxCsVsTrees, GxCsVsWave, GxCsVsVelRigid, GxCsVsVelSkin,   // VelRigid/VelSkin: gx_cs_vel.vert
	GxCsVsShadow = 5,   // gx:shadowvol (GxShadowVolumeVS.h, C · rw3-R11): 5, not 3 - render/ws-motion-units appends 3/4 (gx:vel-*)
	GxCsVsCount };

struct GxCsEntry {
	const char*     files;       // file name(s) the blob ships under ('|' separated); a built-in program is 'water:<name>'
	uint8_t         pixel;       // 1 = ps, 0 = vs
	uint8_t         program;     // GxCsPsId / GxCsVsId; 0 = known blob, no port yet
	const uint32_t* dwords;
	uint32_t        count;
	uint64_t        hash;        // FNV-1a64 of the bytes
	const uint32_t* decl;        // a ported vertex shader's expected D3DVSD declaration (tokens); null for a ps
	uint32_t        declCount;
};

enum GxCsVerdict : uint8_t { GxCsUnknown, GxCsKnownUnported, GxCsPorted };

// The vertex-shader constants a ported vs can read: c0-c95, the translator's store (GxD3D8Translate m_vsConst) aliased onto the first
// 1536 B of the VS block (the ports declare vec4 c[96]). The native device reports min(DXVK's, this) and refuses a write past it; a
// replay tool refuses a capture that writes past it (rw3-R11: a classic GX_SHADOW_GPU capture packs 61 volumes a draw, c9-c252).
constexpr uint32_t kGxCsVsConstCount = 96;
struct GxCsResolved {
	GxCsVerdict    verdict = GxCsUnknown;
	const GxCsEntry* entry = nullptr;   // set for GxCsKnownUnported / GxCsPorted
	const char*    why = "";
};

const GxCsEntry* GxCsEntries(size_t* count);
// tokens = the exact dwords handed to CreatePixelShader / CreateVertexShader (the replay: the GXCAP record's function bytes).
GxCsResolved GxCsResolvePixel(const uint32_t* tokens, size_t count);
// decl = the D3DVSD declaration handed to CreateVertexShader. A ported vs whose declaration differs from the catalogue's is
// UNKNOWN (the ported inputs map by register, so a different declaration could feed the port the wrong data).
GxCsResolved GxCsResolveVertex(const uint32_t* tokens, size_t count, const uint32_t* decl, size_t declCount);

// The key inputs of a ported vertex shader from its declaration (register -> GxSemantic per the entry's port: Trees = v0
// Position, v1 Normal, v2 Color0, v7 Tex0; Wave = v0 Position, v1 Color0, v2 Tex0; VelRigid = v0 Position; VelSkin = v0 Position,
// v1 Normal (= the previous position); Shadow = v0 Position, v1 Normal, v2 Tex0 (FLOAT3), v3 Tex1 (FLOAT1); offsets accumulate in
// declaration order).
// false + why = not drawable by the port.
bool GxCsVertexInputs(const GxCsEntry& e, const uint32_t* decl, size_t declCount, GxStructKey* s, std::string* why);

// One shader the game tried to load this session: found = the file system produced bytes.
struct GxCsLoad {
	std::string           path;     // as passed to LoadAndCreateD3DShader ("shaders\\terrain.pso"); only the last component is used
	bool                  found = false;
	bool                  pixel = true;
	std::vector<uint32_t> tokens;
	std::vector<uint32_t> decl;     // a vertex shader's declaration
};
struct GxCsSession {
	bool classic = false;                // the session must render with the classic backend
	bool complete = true;                // every loaded blob is ported (false = a known blob without a port was loaded)
	std::string why;                     // the first reason, for the log
	std::vector<std::string> unported;   // known blobs loaded without a port
};
// Decides once, before gameplay. A load that found no file, or whose bytes the catalogue does not know, forces CLASSIC
// (a missing terrain.pso makes the game take its own fallback terrain path, which the verified set does not cover: the
// renderer never substitutes another port for it). A known-unported blob leaves classic=false and complete=false.
GxCsSession GxCsSessionVerdict(const std::vector<GxCsLoad>& loads);

} // namespace GXRender
