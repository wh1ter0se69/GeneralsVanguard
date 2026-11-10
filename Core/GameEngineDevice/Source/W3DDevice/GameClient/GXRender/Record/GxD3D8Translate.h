// GeneralsX: WS-F1: GxD3D8Translate - D3D8 device calls -> GxPacketWriter packets (the L1
// d3d8 -> key / state builder, first used by the capture replay).
//
// It holds the D3D8 device state a fixed-function draw reads (render states, stage states, transforms, lights, material,
// viewport, the FVF, streams, indices, textures, the render target) and turns each Draw* into ONE GxDrawPacket:
//  * the pipeline key from the FVF (DXVK's FVF -> declaration order), the blend state (D3DBLEND_BOTH* fix-up; on a target
//    without alpha DST_ALPHA -> ONE and INV_DST_ALPHA -> ZERO, as DXVK's alpha swizzle), the write mask, the topology class,
//    fill and shade mode and the EFFECTIVE attachments; the pipeline itself from
//    GxPipelineCache::getFF (rw2-D's specialised-or-generic FF ubershader);
//  * dynamic state as DXVK binds it: the viewport flipped (y + height, -height) with the +0.5 correctness offset and the
//    MinZ bias, scissor = the viewport (D3D8 has no scissor test), cull CW -> FRONT / CCW -> BACK with front = CLOCKWISE,
//    depth, stencil (one face), D3D8 ZBIAS x -0.000005 x the depth format's r;
//  * the FF blocks + spec block from GxPackFF.
// What it cannot draw it REFUSES per draw with a named reason (programmable vertex/pixel shaders = WS-E, vertex blend, user
// clip planes, cube/volume textures, triangle fans on a device without them...): the replay counts those as UNDRAWABLE.
// Values are D3D8's numbers; no D3D8 header is needed.
#pragma once

#include "GxFFState.h"
#include "GxAlphaToCoverage.h"
#include "GxCustomCatalog.h"
#include "GxPacketWriter.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GXRender { namespace Res { class GxBuffer; class GxTexture; class GxImage; } }

namespace GXRender { namespace Rec {

struct GxD3D8Target
{
	Res::GxImage* image = nullptr;
	VkImageView view = VK_NULL_HANDLE;
	bool texture = false;     // a render-target TEXTURE's surface (sampled later: handed to readers at scope close)
	uint32_t d3dFormat = 0;   // what the game created (X8R8G8B8 has no alpha: DXVK's alpha swizzle)
};

struct GxTranslateStats
{
	uint64_t draws = 0, drawn = 0, refused = 0, clears = 0;
	uint64_t custom = 0;                     // drawn through a WS-E custom pipeline (also counted in `generic`)
	uint64_t customBuilds = 0;               // custom pipelines built on the caller (setBuildCustomOnMiss)
	uint64_t specialised = 0, generic = 0;   // which FF pipeline drew
	uint64_t castOnly = 0;                   // S7: cast-only draws handed to the writer's capture (never drawn)
	uint64_t castOnlyDropped = 0;            // S7 A44: draws under CAST_ONLY that are not casters (no CAST, or no z test + z write): neither
	                                         // drawn nor captured - castOnly + castOnlyDropped = the draws issued under CAST_ONLY
	uint64_t castOnlySolid = 0;              // S7 A61: cast-only captures that carry SOLID (sorted CAST_SHADOW casters): <= castOnly
	// S7 P2 (PREREG A2/A3): receiver draws per composition class - exact (opaque, alpha / constant blends), additive (dest x ONE),
	// nonlinear (a SRCCOLOR / INVSRCCOLOR / DESTALPHA-family factor), modulate (never darkened: dest already is), alphaSkip (blended
	// or A2C: alpha kept)
	uint64_t rcvExact = 0, rcvAdditive = 0, rcvNonlinear = 0, rcvModulate = 0, rcvAlphaSkip = 0;
	std::map<std::string, uint64_t> refusedWhy;
	std::map<std::string, uint64_t> passLabels;
	std::map<std::string, uint64_t> drawClasses;   // drawn draws by (pre-transformed, blend, depth test, depth write)
	// rw3-T cut 3b (dirty-pack): FF draws that reused the last GxPackFF output (no input of it changed) / that packed; of the reuses,
	// those whose world matrix alone had changed (WorldView + NormalMatrix refreshed by GxPackFFWorld); in check mode
	// (GX_TR_PACK_CHECK=1, setPackCheck) every reuse is re-packed and byte-compared, and a difference is counted (must stay 0)
	uint64_t packReused = 0, packed = 0, packWorld = 0, packChecks = 0, packCheckMismatches = 0;
	// (A108): with the session's A2C on (setAlphaToCoverage), every draw by GxAlphaToCoverage.h's answer - on, or off by
	// reason (a2cOff[GxA2C_Session] stays 0: a session-off translator counts nothing). The device prints them per window ([GXA2C]).
	uint64_t a2cOn = 0;
	uint64_t a2cOff[GxA2C_WhyCount] = {};
};

// TEST-ONLY plants (like GxRecDebug): each one breaks ONE rule of the translator; gx_translate_test's X-<case> arm turns it on and
// must see exactly that case fail. All false in the product.
struct GxTranslateDebug
{
	bool a2cIgnoreBlend = false;      // (A108): the blend guard skipped - a BLENDED alpha-tested draw gets A2C (a2c-derive's X-)
	bool a2cSpecBitOff = false;       // (A108): the key gets A2C but the spec copy lacks bit 3 (no ramp: a2c-consistent's X-)
	bool smBitsIgnoreBlend = false;   // S7 P2: applyRgb set on EVERY receiver (a modulate pass darkened twice: sm-bits must catch it)
	bool smOwnMaskOff = false;        // S7 A41: the own-light words written with the look mask OFF (the pack-check must catch it: A40 T1)
	bool smOwnNoCaster = false;       // S7 A41: a clamped caster captured with its plain World (sm-ownlight's X- must catch it)
	bool smOwnNoReceiver = false;     // S7 A41: a clamped instance's receiver never gets bit 3 (it looks the map up unscaled)
	bool smCastOnlyIgnoresCast = false;   // S7 A44: under CAST_ONLY every draw is captured (the CAST + z rule ignored: sm-castonly-drop's X-)
	bool smCastOnlyIgnoresSolid = false;  // S7 A61: SOLID ignored - a z-write-off CAST draw is dropped again (sm-castonly-solid's X-)
	bool skipBlendFixup = false;      // D3DBLEND_BOTH* not rewritten
	bool skipAlphaSwizzle = false;    // DST_ALPHA kept on a target without alpha
	bool skipViewportOffset = false;  // no +0.5 correctness offset
	bool swapCull = false;            // CW -> BACK, CCW -> FRONT
	bool zbiasNoRValue = false;       // ZBIAS not scaled by the depth format's r-value
	bool backStencilDefault = false;  // back-face stencil left at its defaults
	bool sampleMaskAlways = false;    // MULTISAMPLEMASK applied on a single-sample target
	bool anisoIgnoresPoint = false;   // the anisotropy override also rewrites POINT min filters
	bool fvfSpecularFirst = false;    // FVF specular placed before diffuse
	bool upKeepsStream = false;       // an UP draw leaves stream 0 bound
	bool allowZeroPrims = false;      // a zero-primitive draw is not refused
	bool depthClearFollowsDrop = false; // a depth clear on a sample-mismatched pair is dropped with the depth
	bool skipStrideCheck = false;     // a stride below the attribute extent reaches Vulkan
	bool multiplyReversed = false;    // MultiplyTransform computes current * m in row-major storage
	bool deleteErasesBound = false;   // deleting a bound shader drops its data at once
	bool zeroDefaultLight = false;    // LightEnable on an unset index enables a zero light
	bool lightsSorted = false;        // enabled lights packed in index order, the first 8
	bool vsZeroClears = false;        // SetVertexShader(0) binds FVF 0
	bool vsZeroKeepsProgram = false;  // SetVertexShader(0) leaves a live programmable shader bound
	bool drawClassWrongBit = false;   // the draw class's blend bit read from depthWrite (stats().drawClasses identity case)
	bool packKeepsOnMaterial = false; // SetMaterial does not invalidate the cached pack (cut 3b: a setter that misses an input)
	bool packWorldStale = false;      // a world-matrix change leaves the cached WorldView / NormalMatrix as they were (cut 3b)
	bool packOverlayInCache = false;  // a custom ps's c0-c7 overlay is written into the cached PS block, not a copy (cut 3b)
	bool packVsOverlayInCache = false; // a custom vs's c0-c95 overlay is written into the cached VS block, not a copy (cut 3b)
	// cut 3b owner review P3: four more setters that would miss their pack invalidation
	bool packKeepsOnLight = false;    // SetLight does not invalidate the cached pack
	bool packKeepsOnViewport = false; // SetViewport does not invalidate the cached pack
	bool packTableShort = false;      // A25 plant: RS_TEXTUREFACTOR left out of the pack-input table (a GxPackFF input that keeps the pack)
	bool nonInputStale = false;       // A25 plant: a draw that REUSES the pack reads rs / the sampler tss from the last repack's copy
	                                  // (a non-input change never reaches the GPU; inert when every change repacks, GX_A25_SLOW_P1)
	bool packKeepsOnUnbind = false;   // the pack misses a stage going bound -> unbound: the setter's invalidation AND the draw's
	                                  // textureType compare skipped - either alone catches it (P3: the setter-only plant was NOT detected)
	bool packKeepsOnView = false;     // SetTransform VIEW does not invalidate the cached pack
	bool psCatalogMiss = false;       // createPixelShader resolves every ps as not in the catalogue (cs-live-programs' X-: the refusal
	                                  // the native device gave live water's and the remastered ground's programs before their ports)
};

// TEST-ONLY (A23-B merge gate): the translator-INPUT digest hook. Every public entry point that changes
// translator state reports, at its top, what it RECEIVED - scalars, Res objects by address (the sink numbers them), the bytes
// behind its pointer arguments: exactly the bytes that entry itself reads, under its own read condition. The device's EXEC trace
// records the call; this records what the translator got - a value forwarded differently from the one traced (CS=0 vs CS=1)
// shows here and not there. Null sink = nothing (the product: one relaxed load per entry).
enum class GxTrIn : uint16_t
{
	Init = 1, Reset, ResetStates, RenderState, TextureStageState, Transform, Material, Light, LightEnable, Viewport, Aniso,
	VertexShader, PixelShader, CreateVs, CreatePs, DeleteVs, DeletePs, VsConst, PsConst, Stream, Indices, Texture, RenderTarget,
	Clear, Draw, DrawIndexed, DrawUP, DrawIndexedUP, BuildCustomOnMiss
};
struct GxTrArg
{
	uint64_t v;
	bool object;
	GxTrArg(uint64_t x) : v(x), object(false) {}
	GxTrArg(const void* p) : v(uint64_t(reinterpret_cast<uintptr_t>(p))), object(true) {}
};
struct GxTrBytes
{
	const void* p;
	size_t n;
};
class GxTranslateInputSink
{
public:
	virtual ~GxTranslateInputSink() = default;
	virtual void input(GxTrIn entry, const GxTrArg* args, size_t n, const GxTrBytes* bytes, size_t nb) = 0;
};
extern std::atomic<GxTranslateInputSink*> g_trInputSink;

class GxD3D8Translate
{
public:
	void init(Vk::VkCtx& ctx, GxPacketWriter& writer, GxPipelineCache& cache, bool triangleFans);
	void reset();   // D3D8 device defaults
	// DXVK's ResetState (a device Reset): render / stage / sampler states to the defaults, textures / streams / indices unbound;
	// transforms, viewport, material, lights, shaders and their constants persist.
	void resetStates();

	void renderState(uint32_t state, uint32_t value);
	// S7 (PREREG A1/A23): the shadow-map tags for the draws that follow (GxShadowCasters.h kShadowTag*); the
	// device sets them through IGxShadowMap::SetShadowTags, only on change. 0 = no S7 effect on any packet.
	void shadowTags(uint32_t tags) { m_shadowTags = tags; }
	uint32_t shadowTags() const { return m_shadowTags; }
	// S7 A41 (PREREG A41/A42, integrator A40 T2/T3): a clamped instance's own light for the draws that follow (IGxShadowMap::SetOwnLight,
	// on change only; 0 = none). packed = mu's float bits | z0's float bits << 32. Its CAST captures take World x S (z' = z0 + mu (z - z0))
	// - in the shadow capture record only, never the recorded draw - and, with the look mask on, its receivers get DrawBits bit 3 and the
	// PS block's OwnScale / OwnZ0.
	void shadowOwn(uint64_t packed) { m_shadowOwn = packed; }
	uint64_t shadowOwn() const { return m_shadowOwn; }
	// S7 P2: the device's look mask has bit 0 (the receiver family is loaded): every draw then carries DrawBits in its PS block. Off
	// (the default, key off) = the word is never written and stays 0.
	void setReceivers(bool on) { m_receivers = on; }
	// (A108): the session's alpha-to-coverage - GXAlphaToCoverage on AND the device loaded the a2c shader family
	// (creation MSAA >= 2). Off (the default, key off) = no draw is examined: today's keys and spec blocks exactly.
	void setAlphaToCoverage(bool on) { m_a2c = on; }
	bool alphaToCoverage() const { return m_a2c; }
	void textureStageState(uint32_t stage, uint32_t type, uint32_t value);
	void transform(uint32_t type, const float m[16], bool multiply);
	void material(const GxD3D8Material& m);
	void light(uint32_t index, const GxD3D8Light& l);
	// d3d9_device.cpp LightEnable: an index never set gets DefaultLight (d3d9_state.h:176); the eight enable SLOTS are kept as
	// DXVK keeps them (enable fills the first empty slot, disable empties the index's slot) and the FF block takes them in slot order.
	void lightEnable(uint32_t index, bool on);
	void viewport(const GxD3D8Viewport& vp);
	void vertexShader(uint32_t handle, bool programmable);   // an FVF, or a CreateVertexShader handle
	void pixelShader(uint32_t handle);
	// WS-E (rw2-E, measurement branch): programmable shaders are drawn only when the catalogue ports their exact bytecode
	// (and, for a vs, its declaration); anything else is refused per draw with the catalogue's reason. Constants: c0-c95.
	void createVertexShader(uint32_t handle, const uint32_t* decl, size_t declCount, const uint32_t* tokens, size_t count);
	void createPixelShader(uint32_t handle, const uint32_t* tokens, size_t count);
	// A deleted shader that is still bound keeps drawing (DXVK keeps the bound object); its data goes when it is unbound.
	void deleteVertexShader(uint32_t handle);
	void deletePixelShader(uint32_t handle);
	void vsConstants(uint32_t first, uint32_t count, const float* data);
	// DXVK's d3d9.samplerAnisotropy (the shipped dxvk.conf: 16; 11/10: the native renderer honours the player's classic
	// setting, read once): every sampler with a mip filter whose min filter is above POINT gets this anisotropy whatever its
	// MAXANISOTROPY (d3d9_device.cpp:7194-7202; GxSamplers keeps the mip-filter condition). -1 = no override (DXVK's default).
	void setAnisotropyOverride(int32_t n);
	void psConstants(uint32_t first, uint32_t count, const float* data);   // c0-c7: a custom ps reads them from the PS block's stages
	const float* testVsConst(uint32_t r) const { return m_vsConst[r]; }   // TEST-ONLY (A23-B arm cs-constants)
	const float* testPsConst(uint32_t r) const { return m_psConst[r]; }
	// TEST-ONLY (rw3-X-3 cs-ff-payload): the light SetLight / LightEnable defined at this index (null = none)
	const GxD3D8Light* testLight(uint32_t i) const { const auto it = m_lights.find(i); return it == m_lights.end() ? nullptr : &it->second; }
	// TEST-ONLY (A23-B shader-payload arms): the catalogue verdict a created shader resolved to (255 = no such handle)
	uint8_t testVsVerdict(uint32_t h) const { const auto it = m_vsProg.find(h); return it == m_vsProg.end() ? 255 : uint8_t(it->second.r.verdict); }
	uint32_t testBoundPs() const { return m_ps; }   // TEST-ONLY: the pixel shader handle the translator draws with
	uint32_t testBoundVs() const { return m_vsProgrammable ? m_vs : 0; }   // TEST-ONLY: the programmable vertex shader it draws with
	uint8_t testPsVerdict(uint32_t h) const { const auto it = m_psProg.find(h); return it == m_psProg.end() ? 255 : uint8_t(it->second.verdict); }
	void stream(uint32_t index, Res::GxBuffer* buffer, uint32_t stride);
	void indices(Res::GxBuffer* buffer, uint32_t baseVertex);
	void texture(uint32_t stage, Res::GxTexture* t);
	// bindViewport = a d3d8 SetRenderTarget with a colour surface: the viewport resets to the whole target, depth [0, 1] (DXVK,
	// even for the same surface). false = the same binding under new storage (a Present rotation).
	void renderTarget(const GxD3D8Target& color, const GxD3D8Target& depth, bool bindViewport = false);
	// D3DRECTs (x1, y1, x2, y2), clipped to the viewport as D3D8 does; flags = D3DCLEAR_TARGET 1 | ZBUFFER 2 | STENCIL 4
	void clear(uint32_t count, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

	bool draw(uint32_t primType, uint32_t startVertex, uint32_t primCount);
	bool drawIndexed(uint32_t primType, uint32_t minIndex, uint32_t numVertices, uint32_t startIndex, uint32_t primCount);
	bool drawUP(uint32_t primType, uint32_t primCount, const void* vertices, uint32_t stride);
	bool drawIndexedUP(uint32_t primType, uint32_t minIndex, uint32_t numVertices, uint32_t primCount, const void* indices,
		bool index32, const void* vertices, uint32_t stride);

	// drawClasses is rebuilt from per-draw class counters when the stats are read (rw3-T cut 3a: no string or map work per draw)
	const GxTranslateStats& stats() const;
	// Tests: called with every packet right before GxPacketWriter::draw (null = none, the product).
	void setObserver(void (*fn)(const GxDrawPacket&, void*), void* user) { m_observe = fn; m_observeUser = user; }
	GxTranslateDebug& debug() { return m_debug; }
	// cut 3b check mode (also GX_TR_PACK_CHECK=1 at init): every draw that reuses the cached pack packs again and compares the bytes
	// (stats().packChecks / packCheckMismatches); the draw still takes the cached blocks, exactly as without the check
	void setPackCheck(bool on) { m_packCheck = on; }
	// The d3d8 state as the device's getters answer it (GetRenderState, GetTransform, ...). lights/lightCount are the ones the
	// last draw packed (a draw that reuses the cached pack leaves them as the pack that made it: no light input changed since).
	const GxD3D8FFState& state() const { return m_ff; }
	// The device's production custom-pipeline miss path: a custom key nobody built is built on the
	// caller, counted in stats().customBuilds. Off = rw2-E's rule (a counted miss that draws nothing).
	void setBuildCustomOnMiss(bool on);
	const GxD3D8Target& colorTarget() const { return m_color; }
	// An attachment of the writer's current pass (the last one begun here). A whole-target clear waits in that pass as its load op
	// and GxPacketWriter::forget resets the pass: the device runs the clear before destroying such an image.
	// Compared, never dereferenced (it may be stale).
	bool inWriterPass(const Res::GxImage* img) const { return img && (img == m_passColor || img == m_passDepth); }

	// The vertex inputs an FVF declares (DXVK's FVF -> declaration order), offsets from 0; false = an FVF this does not model.
	bool fvfInputs(uint32_t fvf, GxStructKey* key, uint32_t* stride, std::string* why) const;
	static uint32_t primVertices(uint32_t primType, uint32_t primCount);

private:
	enum class Kind { Plain, Indexed, UP, IndexedUP };
	bool castOnlyCaptures() const;   // S7 A44: under CAST_ONLY, is this draw a caster (CAST + the drawn path's z test / z write)
	bool emit(Kind kind, uint32_t primType, uint32_t primCount, uint32_t first, int32_t vertexOffset, const void* upVertices,
		uint32_t upVertexBytes, uint32_t upStride, const void* upIndices, uint32_t upIndexBytes, bool upIndex32);
	bool refuse(const char* why);
	void ensurePass();
	void label(const char* name);
	void beginPass(const GxPassDesc& p);

	Vk::VkCtx* m_ctx = nullptr;
	GxPacketWriter* m_w = nullptr;
	GxPipelineCache* m_cache = nullptr;
	bool m_fans = true;
	GxD3D8FFState m_ff = {};
	uint32_t m_shadowTags = 0;   // S7
	uint64_t m_shadowOwn = 0;    // S7 A41: the current own light (0 = none)
	float m_ownWorld[16] = {};   // S7 A41: World x S for a clamped caster's capture (the packet points at it; the recorded draw keeps m_ff.world)
	float m_castOwnMu = 0.f;     // S7 A79: the mu castWorld() applied (0 exactly when it returned m_ff.world) - the packet's castOwnMu
	const float* castWorld();    // S7 A41: m_ff.world, or World x S into m_ownWorld while an own light is set
	bool m_receivers = false;    // S7 P2
	bool m_a2c = false;          // (A108)
	std::map<uint32_t, GxD3D8Light> m_lights;   // every light index SetLight / LightEnable has defined
	uint32_t m_lightSlots[8];                    // DXVK's enabledLightIndices: a light index or kNoLight
	uint32_t m_vs = 0;                    // current vertex shader: an FVF code or a handle
	bool m_vsProgrammable = false;
	bool m_vsDeclOnly = false;   // SetVertexShader(0) unbound a live programmable shader: FF on its declaration (refused)
	uint32_t m_ps = 0;
	struct VsProg { GxCsResolved r; std::vector<uint32_t> decl; };
	std::map<uint32_t, VsProg> m_vsProg;      // CreateVertexShader handle -> the catalogue's verdict + its declaration
	std::map<uint32_t, GxCsResolved> m_psProg;
	uint32_t m_vsDeleted = 0, m_psDeleted = 0;   // a deleted handle still bound (0 = none)
	int32_t m_anisoOverride = -1;
	float m_psConst[8][4] = {};
	float m_vsConst[96][4] = {};              // c0-c95; the custom vertex shader reads them from the VS block
	Res::GxBuffer* m_streams[16] = {};
	uint32_t m_strides[16] = {};
	Res::GxBuffer* m_indices = nullptr;
	uint32_t m_baseVertex = 0;
	Res::GxTexture* m_textures[8] = {};
	GxD3D8Target m_color, m_depth;
	bool m_passOpen = false;
	const Res::GxImage* m_passColor = nullptr;   // the writer's current pass (inWriterPass)
	const Res::GxImage* m_passDepth = nullptr;
	std::string m_label;
	mutable GxTranslateStats m_stats;   // mutable: stats() rebuilds drawClasses from m_drawClassCount
	uint64_t m_drawClassCount[32] = {};   // drawn draws by class index (16 positionT, 8 blend, 4 ztest, 2 zwrite with ztest, 1 rt-texture)
	GxTranslateDebug m_debug;
	bool m_buildCustomOnMiss = false;
	// cut 3b (dirty-pack, conditions): GxPackFF's last output and the per-draw derived inputs it was packed with. Valid
	// until a setter changes a byte GxPackFF reads (m_ff, the lights, the enable slots) - when in doubt a setter invalidates; the
	// derived inputs emit() writes into m_ff each draw are compared instead. L1 only: rw3-G's instanced path packs from a copy and
	// neither reads nor writes this.
	struct PackCache
	{
		GxFFVsBlock vs;
		GxFFPsBlock ps;
		GxFFSpecBlock spec;
		uint32_t fvf;
		uint8_t textureType[8];
		bool pointList, pixelShader, lightsSorted;
		uint64_t vsGen, psGen;   // A22: a new process-wide stamp at every write to vs / ps (GxDrawPacket::vsGen / psGen)
	};
	PackCache m_pack = {};
	bool m_packValid = false;
	bool m_packWorldDirty = false;   // the world matrix changed since m_pack was made (the only change: anything else invalidates)
	bool m_packCheck = false;
	struct StaleSnap { uint32_t rs[256]; uint32_t tss[8][32]; };
	std::unique_ptr<StaleSnap> m_staleSnap;   // TEST-ONLY (m_debug.nonInputStale): rs / tss as the last repack saw them; allocated
	                                          // only when the plant is on (no product storage)
	void (*m_observe)(const GxDrawPacket&, void*) = nullptr;
	void* m_observeUser = nullptr;
};

}} // namespace GXRender::Rec
