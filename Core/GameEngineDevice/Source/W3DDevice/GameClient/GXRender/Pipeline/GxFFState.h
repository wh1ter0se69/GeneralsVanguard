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

// GxFFState.h - the fixed-function ubershader's state blocks and the D3D8 -> block packing.
// Layout: set 0 b0/b1 dynamic UBOs (GxFFVsBlock, GxFFPsBlock), set 1 b0-7 texture stages, 128 B push constants.
// A specialised pipeline is the same SPIR-V with spec constant 32 true: push the block on EVERY FF draw either way.
// Fields follow DXVK 3.0 (zlib); gx_pipecache_test --ff-layout checks every offset against the compiled SPIR-V.
#pragma once
#include <cstddef>
#include <cstdint>

namespace GXRender {

// ---- the blocks ---------------------------------------------------------------------------------------------------------
struct GxFFLight {                       // 112 B. View space; Theta/Phi = cos(theta/2), cos(phi/2) (DXVK D3D9Light)
	float    Diffuse[4], Specular[4], Ambient[4];
	float    Position[4], Direction[4];
	uint32_t Type;                       // D3DLIGHTTYPE: 1 point, 2 spot, 3 directional
	float    Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};
struct GxFFMaterial {                    // 80 B (std140 pads a struct to 16)
	float Diffuse[4], Ambient[4], Specular[4], Emissive[4];
	float Power, pad0[3];
};
struct GxFFFog {                         // 16 B. DXVK D3D9SharedPushData
	uint32_t PackedFogColorAndAlphaRef;  // bits 0-23 fog colour B,G,R bytes as D3DCOLOR's low 24 bits; bits 24-31 alpha ref
	float    FogDistanceScale;           // 1 / (end - start), 0 when end == start
	float    FogDistanceEnd;
	float    FogDensity;
};
struct GxFFVsBlock {                     // set 0 binding 0. Matrices: D3DMATRIX element order (_11 _12 _13 _14 _21 ...)
	float        WorldView[16];          // World x View
	float        NormalMatrix[16];       // inverse(World x View) - the shader applies it transposed
	float        InverseView[16];        // transpose(inverse(View)) (clip planes only - unused while they are refused)
	float        Projection[16];
	float        WorldViewProj[16];      // World x View x Projection - NOT read by the shader (2.6 transforms in two steps); kept for the layout
	float        TexcoordMatrices[8][16];// D3DTS_TEXTURE0..7
	float        ViewportInverseOffset[4], ViewportInverseExtent[4];   // POSITIONT -> clip space
	GxFFLight    Lights[8];              // the first LightCount enabled lights, light-index order
	GxFFMaterial Material;
	GxFFFog      Fog;                    // vertex fog
	uint32_t     GlobalAmbient;          // D3DRS_AMBIENT (D3DCOLOR)
	float        TweenFactor;            // 0 (vertex blend / tween is refused)
	float        PointSize, PointSizeMin;
	float        PointSizeMax, PointScaleA, PointScaleB, PointScaleC;   // scale terms already / viewport height^2
};
struct GxFFPsStage {                     // 32 B
	uint32_t Constant;                   // D3DTSS_CONSTANT (D3D9 only: 0 under D3D8)
	uint32_t pad0;
	float    BumpEnvLScale, BumpEnvLOffset;
	float    BumpEnvMat[4];              // 00 10 01 11: .xy dots (du, dv) for U, .zw for V (DXVK's flipped major-ness)
};
struct GxFFPsBlock {                     // set 0 binding 1
	GxFFPsStage Stages[8];
	GxFFFog     Fog;                     // pixel fog + fog colour + alpha ref
	uint32_t    TextureFactor;           // D3DRS_TEXTUREFACTOR (D3DCOLOR)
	uint32_t    DrawBits;                // the per-draw look bits (GxDrawBit below; 0 = none - always 0 with the look mask off)
	float       OwnScale;                // S7 A41 (offset 280): a clamped instance's own light - mu of z' = z0 + mu (z - z0); 0 with the mask off
	float       OwnZ0;                   // (offset 284): its ground plane z0; 0 with the mask off. The block has no spare word left:
	                                     // the next per-draw consumer appends a 16-byte row
};
// GxFFPsBlock::DrawBits: ONE per-draw bits word, read
// only by the key-on shader families (GLSL declares it under #if defined(GX_LOOK_SHADOWMAP) ...: key off has no such member). Bits 0-2
// = the shadow map (the receiver's apply rule, written by the translator), 3-7 reserved for it, 8+ for F-depth+'s readers (soft particles, water).
enum GxDrawBit : uint32_t {
	GxDraw_Receive = 1u << 0,                // S7: this draw samples the shadow map (RECEIVE tag, not pre-transformed)
	GxDraw_ApplyRgb = 1u << 1,               // S7 A2: RGB x the shadow factor (the blend scales linearly with dest: not a modulate pass)
	GxDraw_ApplyAlpha = 1u << 2,             // S7 A3: alpha x the shadow alpha (blending AND alpha-to-coverage off)
	GxDraw_OwnLight = 1u << 3,               // S7 A41: look the map up at (x, y, OwnZ0 + OwnScale (z - OwnZ0)) - a clamped instance's own draws
	GxDraw_S7Mask = 0xffu,                   // bits 0-7: S7's
};

// The FF key = the push-constant block. Every word a plain uint; word i is also specialisation constant i.
enum GxFFSpecWord : uint32_t {
	GxFFW_AlphaTestAndModeArgs = 0,  // bits 0-2 VkCompareOp (7 = test off), 4-7 alpha precision, 8 point sprite, 16-18 clip planes, 24-31 dref shift
	GxFFW_FogArgs = 1,               // bit 0 enable, 8-9 vertex D3DFOGMODE, 16-17 pixel D3DFOGMODE, 24 use Z
	GxFFW_ProjMaskAndFfArgs = 2,     // bits 0-7 sampler projection mask, 8 global specular, 16 point scale, 24 PS is SM3 (0)
	GxFFW_Unused3 = 3,
	GxFFW_PsSamplerTypes = 4,        // 2 bits per stage: 0 2D, 3 null
	GxFFW_PsSamplerModes = 5,        // 0
	GxFFW_Unused6 = 6, GxFFW_Unused7 = 7,
	GxFFW_StageOps01 = 8,            // per stage 16 bits: 0-4 colour op, 8-12 alpha op, 15 result to temp; stage 2k low, 2k+1 high
	GxFFW_StageArgs0 = 12,           // per stage: 0-4/5-9/10-14 colour arg 0/1/2, 16-20/21-25/26-30 alpha arg 0/1/2 (DXVK arg packing)
	GxFFW_VsData0 = 20,              // 12 words = DXVK's packed FF VS data, byte offsets below
	GxFFW_Count = 32,
};
// Byte offsets inside the 48 VS-data bytes (words 20-31), as DXVK's D3D9FixedFunctionVS tail / DataPrimitives.
enum GxFFVsByte : uint32_t {
	GxFFB_TexcoordIndices = 0, GxFFB_TexcoordFlags = 8, GxFFB_TexcoordTransformFlags = 16, GxFFB_TexcoordDeclMask = 24 /* u32 */,
	GxFFB_HasPositionT = 28, GxFFB_HasColor0 = 29, GxFFB_HasColor1 = 30, GxFFB_HasPointSize = 31, GxFFB_HasFog = 32,
	GxFFB_VertexBlendMode = 33, GxFFB_VertexBlendIndexed = 34, GxFFB_VertexBlendCount = 35, GxFFB_VertexClipping = 36,
	GxFFB_NormalizeNormals = 37, GxFFB_LocalViewer = 38, GxFFB_RangeFog = 39, GxFFB_UseLighting = 40, GxFFB_LightCount = 41,
	GxFFB_DiffuseSource = 42, GxFFB_AmbientSource = 43, GxFFB_SpecularSource = 44, GxFFB_EmissiveSource = 45,
};
struct GxFFSpecBlock {
	uint32_t w[GxFFW_Count];
};

static_assert(sizeof(GxFFLight) == 112, "std140 light");
static_assert(sizeof(GxFFMaterial) == 80, "std140 material");
static_assert(sizeof(GxFFFog) == 16, "std140 fog");
static_assert(offsetof(GxFFVsBlock, TexcoordMatrices) == 320 && offsetof(GxFFVsBlock, ViewportInverseOffset) == 832 &&
              offsetof(GxFFVsBlock, Lights) == 864 && offsetof(GxFFVsBlock, Material) == 1760 &&
              offsetof(GxFFVsBlock, Fog) == 1840 && offsetof(GxFFVsBlock, GlobalAmbient) == 1856 &&
              offsetof(GxFFVsBlock, PointSizeMax) == 1872 && sizeof(GxFFVsBlock) == 1888, "std140 VS block");
static_assert(sizeof(GxFFPsStage) == 32 && offsetof(GxFFPsBlock, Fog) == 256 && offsetof(GxFFPsBlock, TextureFactor) == 272 &&
              offsetof(GxFFPsBlock, DrawBits) == 276 && offsetof(GxFFPsBlock, OwnScale) == 280 && offsetof(GxFFPsBlock, OwnZ0) == 284 &&
              sizeof(GxFFPsBlock) == 288, "std140 PS block");
static_assert(sizeof(GxFFSpecBlock) == 128, "push block = 128 B (Vulkan's guaranteed maxPushConstantsSize)");

// ---- the D3D8 state tracked (one snapshot per FF draw) ------------------------------------------------------------
struct GxD3D8Light {                     // D3DLIGHT8 as SetLight received it
	uint32_t type;
	float    diffuse[4], specular[4], ambient[4];   // r g b a
	float    position[3], direction[3];
	float    range, falloff, attenuation0, attenuation1, attenuation2, theta, phi;
};
struct GxD3D8Material { float diffuse[4], ambient[4], specular[4], emissive[4]; float power; };   // D3DMATERIAL8
struct GxD3D8Viewport { uint32_t x, y, width, height; float minZ, maxZ; };                         // D3DVIEWPORT8
enum : uint8_t { GxTexNone = 0, GxTex2D = 1, GxTexCube = 2, GxTexVolume = 3 };

struct GxD3D8FFState {
	float          world[16], view[16], projection[16];   // D3DMATRIX element order
	float          texture[8][16];                       // D3DTS_TEXTURE0..7
	uint32_t       rs[256];                              // D3DRENDERSTATETYPE -> the DWORD SetRenderState got (floats bit-cast)
	uint32_t       tss[8][32];                           // D3DTEXTURESTAGESTATETYPE -> DWORD, per stage
	GxD3D8Light    lights[8];                            // the ENABLED lights in light-index order (the first 8)
	uint32_t       lightCount;
	GxD3D8Material material;
	GxD3D8Viewport viewport;
	uint32_t       fvf;                                  // the FF vertex shader handle (an FVF code)
	uint8_t        textureType[8];                       // the texture bound on each stage (GxTex*)
	bool           pointList;                            // this draw's primitive type is D3DPT_POINTLIST
	bool           pixelShader;                          // a programmable pixel shader is bound: its stages are read whatever the FF stage ops say
};

// D3D8 device defaults (render states, stage states, identity matrices, no lights, zero material, 0/0/1/1 viewport).
void GxD3D8FFDefaults(GxD3D8FFState* s);

// The three blocks for one FF draw (DXVK 3.0's UpdateFixedFunctionVS/PS, UpdateFog, BindAlphaTestState, UpdatePointMode).
// The spec block is NORMALISED (dead fields zero) so equal FF behaviour -> equal key. Returns false (and *refuse = why) for
// state this ubershader does not draw - vertex blend / tween, user clip planes, a cube or volume texture on an active
// stage: the caller renders the session classic (native only where complete). D3D8 FF never samples a depth texture
// (the game creates depth SURFACES only, dx8wrapper.cpp:3064/4163), so there is no depth-compare case to refuse.
bool GxPackFF(const GxD3D8FFState& s, GxFFVsBlock* vs, GxFFPsBlock* ps, GxFFSpecBlock* spec, const char** refuse);
// GxPackFF's world part alone: vs->WorldView = View x World and vs->NormalMatrix = its inverse, the ONLY fields s.world reaches.
// For a VS block GxPackFF filled from a state that differs from s in the world matrix alone, the result equals GxPackFF(s)'s.
void GxPackFFWorld(const GxD3D8FFState& s, GxFFVsBlock* vs);
// GxPackFF's whole render / texture-stage state input set (A25): GxPackFFReadsRS / GxPackFFReadsTSS (inline, after namespace d3d8
// below - the translator tests them on every changed state).

// The FF part of the shader key (GxShaderKey.hash64 of an FF-specialised pipeline): GxFastHash64 over the 128 bytes, in memory
// only (never persisted); the cache's specialised map compares the WHOLE block, so the hash only buckets.
uint64_t GxFFSpecHash(const GxFFSpecBlock& spec);

// ---- the D3D8 numbers used (tests/gx_ffstate_test.cpp static_asserts them against d3d8.h when CMake finds the headers) ------
namespace d3d8 {
enum : uint32_t {
	RS_ZENABLE = 7, RS_SHADEMODE = 9, RS_ALPHATESTENABLE = 15, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_FOGENABLE = 28,
	RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37, RS_FOGDENSITY = 38,
	RS_RANGEFOGENABLE = 48, RS_TEXTUREFACTOR = 60, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
	RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145,
	RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148, RS_VERTEXBLEND = 151,
	RS_CLIPPLANEENABLE = 152, RS_POINTSIZE = 154, RS_POINTSIZE_MIN = 155, RS_POINTSPRITEENABLE = 156,
	RS_POINTSCALEENABLE = 157, RS_POINTSCALE_A = 158, RS_POINTSCALE_B = 159, RS_POINTSCALE_C = 160, RS_POINTSIZE_MAX = 166,
	RS_TWEENFACTOR = 170,
};
enum : uint32_t {
	TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
	TSS_BUMPENVMAT00 = 7, TSS_BUMPENVMAT01 = 8, TSS_BUMPENVMAT10 = 9, TSS_BUMPENVMAT11 = 10, TSS_TEXCOORDINDEX = 11,
	TSS_BUMPENVLSCALE = 22, TSS_BUMPENVLOFFSET = 23, TSS_TEXTURETRANSFORMFLAGS = 24, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27,
	TSS_RESULTARG = 28,
};
enum : uint32_t {
	TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4, TOP_BLENDTEXTUREALPHA = 13,
	TOP_BLENDTEXTUREALPHAPM = 15, TOP_BLENDCURRENTALPHA = 16, TOP_PREMODULATE = 17, TOP_BUMPENVMAP = 22,
	TOP_BUMPENVMAPLUMINANCE = 23, TOP_MULTIPLYADD = 25, TOP_LERP = 26,
	TA_SELECTMASK = 0x0f, TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4, TA_TEMP = 5,
	TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20,
	TTFF_DISABLE = 0, TTFF_PROJECTED = 256, TSS_TCI_PASSTHRU = 0, TSS_TCI_CAMERASPACENORMAL = 0x10000,
	CMP_NEVER = 1, CMP_ALWAYS = 8, FOG_NONE = 0, SHADE_GOURAUD = 2, MCS_MATERIAL = 0, MCS_COLOR1 = 1, MCS_COLOR2 = 2,
	VBF_DISABLE = 0, LIGHT_POINT = 1, LIGHT_DIRECTIONAL = 3,
	FVF_POSITION_MASK = 0x00E, FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_NORMAL = 0x010, FVF_PSIZE = 0x020,
	FVF_DIFFUSE = 0x040, FVF_SPECULAR = 0x080, FVF_TEXCOUNT_MASK = 0xf00, FVF_TEXCOUNT_SHIFT = 8,
};
} // namespace d3d8

// A25: the 33 render states and 17 stage states GxPackFF and its helpers READ, on any path (checked the
// set complete at c1ec75c0c; RS_ZENABLE is read only for a pre-transformed FVF, RS_VERTEXBLEND / RS_CLIPPLANEENABLE only to refuse -
// both kept). The translator invalidates its cached pack only when one of these changes; every other state is read per draw from the
// live state. A new read in the packer must be added here (gx_ffstate_test's pack-inputs arm perturbs every index NOT listed and
// requires identical output). Bitmasks built from the lists at compile time: a changed state costs a shift and a test, no call.
namespace detail {
using namespace d3d8;
// inline: one object for every includer (ODR)
inline constexpr uint32_t kPackFFRS[] = { RS_ZENABLE, RS_ALPHATESTENABLE, RS_ALPHAREF, RS_ALPHAFUNC, RS_FOGENABLE, RS_SPECULARENABLE,
	RS_FOGCOLOR, RS_FOGTABLEMODE, RS_FOGSTART, RS_FOGEND, RS_FOGDENSITY, RS_RANGEFOGENABLE, RS_TEXTUREFACTOR, RS_LIGHTING, RS_AMBIENT,
	RS_FOGVERTEXMODE, RS_COLORVERTEX, RS_LOCALVIEWER, RS_NORMALIZENORMALS, RS_DIFFUSEMATERIALSOURCE, RS_SPECULARMATERIALSOURCE,
	RS_AMBIENTMATERIALSOURCE, RS_EMISSIVEMATERIALSOURCE, RS_VERTEXBLEND, RS_CLIPPLANEENABLE, RS_POINTSIZE, RS_POINTSIZE_MIN,
	RS_POINTSPRITEENABLE, RS_POINTSCALEENABLE, RS_POINTSCALE_A, RS_POINTSCALE_B, RS_POINTSCALE_C, RS_POINTSIZE_MAX };
inline constexpr uint32_t kPackFFTSS[] = { TSS_COLOROP, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
	TSS_BUMPENVMAT00, TSS_BUMPENVMAT01, TSS_BUMPENVMAT10, TSS_BUMPENVMAT11, TSS_TEXCOORDINDEX, TSS_BUMPENVLSCALE, TSS_BUMPENVLOFFSET,
	TSS_TEXTURETRANSFORMFLAGS, TSS_COLORARG0, TSS_ALPHAARG0, TSS_RESULTARG };
struct PackFFMask { uint64_t rs[4]; uint32_t tss; };
constexpr PackFFMask packFFMask()
{
	PackFFMask m = {};
	for (uint32_t r : kPackFFRS)
		m.rs[r >> 6] |= uint64_t(1) << (r & 63);
	for (uint32_t t : kPackFFTSS)
		m.tss |= uint32_t(1) << t;
	return m;
}
inline constexpr PackFFMask kPackFFMask = packFFMask();
static_assert(sizeof(kPackFFRS) / sizeof(kPackFFRS[0]) == 33 && sizeof(kPackFFTSS) / sizeof(kPackFFTSS[0]) == 17,
	"GxPackFF reads 33 render states and 17 stage states: a read added to the packer is added to its list (and this count)");
} // namespace detail

inline bool GxPackFFReadsRS(uint32_t rs) { return rs < 256 && (detail::kPackFFMask.rs[rs >> 6] >> (rs & 63) & 1) != 0; }
inline bool GxPackFFReadsTSS(uint32_t tss) { return tss < 32 && (detail::kPackFFMask.tss >> tss & 1) != 0; }

} // namespace GXRender
