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

// GeneralsX: WS-G Task 5: the device-less model of the classic DX8Wrapper state machine
// (CONTRACT_L2 §4.3 C5/C6 r9b; rw1-G/PLAN_model.md T2). ENGINE-FREE: no WW3D2 header, so scripts/test/l2-model-test.cpp drives it
// with hand-made call streams. Every ported function cites the wrapper code it mirrors AT 3a59e5b2a (W/ = Core/Libraries/Source/
// WWVegas/WW3D2/); scripts/test/l2-model-drift.sh pins those ranges, so a wrapper edit forces a re-port.
//
// What is ported: the parts of the wrapper that are functions of WRAPPER STATE (caches, dirty flags, Apply's order, the shader diff,
// the light-environment skip, transforms). What is NOT: texture / material / mapper Apply outputs, which are functions of GAME state -
// the call log carries them by value and the model replays those sends through its caches (it never calls engine code).

#pragma once

#include <cstdint>
#include <cstring>

#include "WW3D2/gxcalllog.h"	// Core/Libraries/Source/WWVegas/WW3D2 (the path this target already has, as "WW3D2/dx8wrapper.h"): the record format AND the by-value payload types (engine-free)

#ifndef GX_L2M_MUTANT
#define GX_L2M_MUTANT 0	// scripts/test/l2-model-test.sh plants one defect per build (see the cases there)
#endif

namespace gxl2m {

// D3D8 numbers the model needs (d3d8types.h), engine-free.
enum : unsigned {
	RS_ZFUNC = 23, RS_ZBIAS = 47, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_NORMALIZENORMALS = 143,
	TS_VIEW = 2, TS_PROJECTION = 3, TS_TEXTURE0 = 16, TS_TEXTURE7 = 23, TS_WORLD = 256,
};
constexpr unsigned kSentinel = 0x12345678u;	// Invalidate_Cached_Render_States' cache value, W/dx8wrapper.cpp:1710-1717
constexpr int kStages = gxlog::kStages;	// MAX_TEXTURE_STAGES, W/dx8wrapper.h:74
constexpr int kLights = gxlog::kLights;
constexpr unsigned kSkipMaterial = 2u, kSkipXform = 4u;	// GX_WRAPSKIP bits, W/dx8wrapper.h:650-675

// The payload types live in gxcalllog.h so the wrapper's hooks (WW3D2) can build them without including this header.
using gxlog::Mat4; using gxlog::Light; using gxlog::Material; using gxlog::VbDesc; using gxlog::IbDesc; using gxlog::Logical;
using gxlog::LenvLight; using gxlog::Lenv; using gxlog::DrawArgs; using gxlog::SortBind; using gxlog::DrawRec; using gxlog::ShaderGlobals;
using gxlog::BT_DX8; using gxlog::BT_SORTING; using gxlog::BT_DYNAMIC_DX8; using gxlog::BT_DYNAMIC_SORTING; using gxlog::BT_INVALID;


// The DEVICE as the model sees it (what a draw can observe), next to the wrapper's own caches in State.
struct Device {
	uint32_t rs[256]; bool rsKnown[256];		// = the device truth (GxTruthRS/Known, W/dx8wrapper.h:892-918)
	uint32_t tss[kStages][32]; bool tssKnown[kStages][32];
	uint64_t tex[kStages];						// = DX8Wrapper::Textures[] (W/dx8wrapper.h:1035), the binding the device holds
	Material mat; bool matValid;				// GxMaterialSent
	Light light[kLights]; bool lightOn[kLights], lightValid[kLights];	// CurrentDX8Lights / Enables / Valid
	Mat4 world, view; uint32_t xformValid;		// DX8Transforms[WORLD/VIEW] + GxXformValid
	Mat4 viewSent; bool viewSentValid;			// GxViewSent (never zeroed by the cache resets)
	Mat4 texm[kStages]; uint32_t texmValid;		// GxTexmSent / GxTexmValid
	Mat4 proj; bool projValid;					// GxProjSent / Valid
	uint64_t stream[2]; uint32_t stride[2];		// SetStreamSource (D3D handle, FVF size)
	uint64_t indices; uint32_t indexBase;		// SetIndices
	uint32_t vertexShader;						// DX8Wrapper::Vertex_Shader (the FVF; never cached, W/dx8wrapper.h:1117-1127)
	bool streamKnown, indicesKnown;				// stream 0 + FVF / indices above are known (set by the model; not in the snapshot)
};

// RenderStateStruct (W/dx8wrapper.h:300-326) - the LOGICAL state Apply sends from. Pointers are ids; a buffer carries the descriptor
// Apply reads from the object (D3D handle, FVF size, FVF, vertex count - PLAN_model r1 #3).
// render_state_changed (W/dx8wrapper.h:388-410)
enum : uint32_t { WORLD_CHANGED = 1u << 0, VIEW_CHANGED = 1u << 1, LIGHT0_CHANGED = 1u << 2, LIGHTS_CHANGED = 0xFu << 2,
	TEXTURE0_CHANGED = 1u << 6, MATERIAL_CHANGED = 1u << 14, SHADER_CHANGED = 1u << 15, VERTEX_BUFFER_CHANGED = 1u << 16,
	INDEX_BUFFER_CHANGED = 1u << 17, WORLD_IDENTITY = 1u << 18, VIEW_IDENTITY = 1u << 19 };

// A light environment as Set_Light_Environment reads it (W/dx8wrapper.cpp:4656-4769; GX_Lenv_Same's fields, :4596-4633) - logged by value.


// C6 MODEL_CALL (CONTRACT_L2 r9b): every low-level setter INVOCATION, sent or skipped by its cache.
enum CallOp : unsigned { C_RS = 1, C_TSS, C_TEXTURE, C_MATERIAL, C_LIGHT, C_XFORM, C_STREAM, C_INDICES, C_VSHADER };
struct CallSink {
	virtual ~CallSink() {}
	virtual void invoked(unsigned op, unsigned a, unsigned b, uint64_t value, bool sent) = 0;
};

struct State;
bool note(State &s, unsigned op, unsigned a, unsigned b, uint64_t value, bool sent);
// The value a light / transform / material invocation reports: FNV-1a over its by-value payload.
inline uint64_t payloadHash(const void *p, unsigned n)
{
	uint64_t h = 1469598103934665603ull;
	for (unsigned i = 0; i < n; ++i) { h ^= ((const uint8_t *)p)[i]; h *= 1099511628211ull; }
	return h;
}	// reports an invocation to s.sink; returns `sent`

struct State {
	CallSink *sink;								// nullable: who hears each invocation (T3 compares them with the call log)
	Logical rs; uint32_t changed;				// render_state + render_state_changed
	int maxTexPerPass;							// CurrentCaps->Get_Max_Textures_Per_Pass() (Apply's texture loop bound)
	Lenv lenvKey; bool lenvKeyValid; uint32_t lenvSerial;	// s_lenvKey / s_lenvKeyValid / s_lenvSerial (W/dx8wrapper.cpp:4609-4611)
	int mutant;									// the check's runtime GX_L2_MUTANT: 4 (shader sends every bit), 24 (index offset no-op)
	uint32_t vsCache;							// DX8Wrapper::Vertex_Shader - what the capture reads as the FVF (MH25); the
												// sorting draw binds its FVF raw, behind it (dev.vertexShader moves, this does not)
	uint32_t curShader; bool shaderDirty;		// ShaderClass::CurrentShader / ShaderDirty (ZW/shader.h:442-443)
	ShaderGlobals sg;
	uint32_t cacheRS[256];						// RenderStates (W/dx8wrapper.h:1033)
	uint32_t cacheTSS[kStages][32];				// TextureStageStates (:1034)
	Mat4 xform[TS_WORLD + 1];					// DX8Transforms (:991)
	bool xformShadowValid[2];					// XformShadowValid (:676), [0] world [1] view
	Material matShadow; bool matShadowValid;	// MaterialShadow(Valid) (:677-678)
	uint32_t lightSerial;						// LightStateSerial
	uint32_t wrapSkip;							// Wrap_Skip() mask
	bool truthOn, captureOn;					// GxTruthOn / GxCaptureOn
	Device dev;
	unsigned unknown;							// > 0: an unmodelled access made every slot UNKNOWN (PLAN_model T1 RAW_ACCESS)
};

// The low-level sends, each the wrapper function it names. They return true when the call reached the device (not skipped).
bool sendRS(State &s, unsigned state, uint32_t value);					// Set_DX8_Render_State, W/dx8wrapper.h:1329
bool sendTSS(State &s, unsigned stage, unsigned state, uint32_t value);	// Set_DX8_Texture_Stage_State, :1358
bool sendTexture(State &s, unsigned stage, uint64_t tex);				// Set_DX8_Texture, :1387
bool sendMaterial(State &s, const Material &m);						// Set_DX8_Material, :1268
bool sendLight(State &s, int index, const Light *l);					// Set_DX8_Light, :1294
bool sendXform(State &s, unsigned transform, const Mat4 &m);			// _Set_DX8_Transform, :1167

// The flush-start snapshot -> the model's State (cache and truth kept apart, MH1). Bindings start unknown; sink / mutant / unknown cleared.
void seed(State &s, const gxlog::Snapshot &sn);

// ShaderClass::Apply (ZW/shader.cpp:409-1060) for a shader whose ShaderBits are `bits`: the diff against curShader / shaderDirty and
// every send it makes, through sendRS / sendTSS (so the caches and the truth follow, MH3). GxL2ModelShader.cpp.
void shaderApply(State &s, uint32_t bits);

// The logged inputs Apply consumes: a texture / material Apply is a function of GAME state, so its sends come from the call log (or, in
// C5's boundary-replay arm, from the source PLAN_model T4 names) and are replayed through the send functions above.
struct Inputs {
	virtual ~Inputs() {}
	virtual void textureApply(State &s, int stage, uint64_t tex) = 0;	// Textures[i]->Apply(i) or Apply_Null(i)
	virtual void materialApply(State &s, uint64_t material) = 0;		// material->Apply() or VertexMaterialClass::Apply_Null()
};

// The logical setters and Apply, each the wrapper function it names (GxL2ModelApply.cpp).
void setTexture(State &s, unsigned stage, uint64_t tex);				// Set_Texture, W/dx8wrapper.h:1654
void setMaterial(State &s, uint64_t material);						// Set_Material, :1663
void setShader(State &s, uint32_t bits);								// Set_Shader, :1680
void setTransform(State &s, unsigned t, const Mat4 &stored);			// Set_Transform, :1741 (`stored` = the transposed matrix it keeps / sends)
void setWorldIdentity(State &s);										// W/dx8wrapper.cpp:5388
void setViewIdentity(State &s);										// :5397
void setVertexBuffer(State &s, unsigned stream, const VbDesc &vb);		// :3275 (vb.obj == 0: null)
void setVertexBufferDynamic(State &s, const VbDesc &vb, uint16_t vbaOffset, uint16_t vbaCount);	// :3325
void setIndexBuffer(State &s, const IbDesc &ib, uint16_t indexBaseOffset);	// :3301
void setIndexBufferDynamic(State &s, const IbDesc &ib, uint16_t ibaOffset, uint16_t indexBaseOffset);	// :3349
void setIndexOffset(State &s, uint16_t offset);						// Set_Index_Buffer_Index_Offset, W/dx8wrapper.h:1223
void setLight(State &s, unsigned index, const Light *l);				// Set_Light, W/dx8wrapper.cpp:4475
void setLightEnvironment(State &s, const Lenv *env);					// :4656
void apply(State &s, Inputs &in);										// Apply_Render_State_Changes, :3674
void releaseRenderState(State &s);									// Release_Render_State, W/dx8wrapper.h:1882
void invalidate(State &s);											// Invalidate_Cached_Render_States, W/dx8wrapper.cpp:1699
void copyRenderState(Logical &dst, const Logical &src);				// RenderStateStruct::operator=, W/dx8wrapper.h:1951 (MH12)
void setRenderState(State &s, const Logical &src);					// Set_Render_State, :1849 (the sorter's restore)

// Two RenderStateStruct copies compared field by field - every field operator= copies, a light's payload only where BOTH copies enable
// it (a disabled slot's payload is not observable, MH12). Each differing field goes to `out` (nullable; the name is only valid during the
// call). Returns the count. C6's SORTQ compare and C5's pending logical state.
struct FieldSink {
	virtual ~FieldSink() {}
	virtual void field(const char *name, uint64_t classic, uint64_t l2) = 0;
};
unsigned logicalDiff(const Logical &classic, const Logical &l2, FieldSink *out);

// DX8Wrapper::Draw (W/dx8wrapper.cpp:3473-3621). The call log carries the switches it read and, for a sorting draw, the dynamic
// buffers Draw_Sorting_IB_VB bound (:3369-3466) - raw, behind render_state, the flags and Vertex_Shader (MH16).
enum DrawOut { DRAW_NONE = 0, DRAW_INDEXED, DRAW_SORTING };
// lowBound = DrawPolygonLowBoundLimit; trianglesOn = _Is_Triangle_Draw_Enabled(); `a` is updated to the arguments the device draw
// gets (the vertex-count fallback, + iba_offset), as the GxDrawHook sees them.
DrawOut draw(State &s, Inputs &in, DrawArgs &a, unsigned lowBound, bool trianglesOn, const SortBind *sb);



// What the checks observe while the driver replays a log (T3 C6, T4 C5).
struct ReplayHooks {
	virtual ~ReplayHooks() {}
	virtual void callMismatch(const gxlog::Record &logged, const char *why) = 0;	// MODEL_CALL
	virtual void entry(uint32_t, uint32_t, const State &) {}						// ENTRY: before the entry's setters
	virtual void drawn(const gxlog::Record &, const DrawArgs &, DrawOut, const State &) {}
	virtual void sortq(uint32_t, const Logical &, const Logical &) {}				// predicted, logged (only operator='s fields differ)
	virtual void unknown(uint64_t) {}												// RAW_ACCESS: caller address
	virtual void end(const State &) {}												// after the last record (the flush end, MH20)
};

// Replays one flush's log through the model: each high-level entry runs its PORT and its predicted invocations are compared with the
// NESTED invocations logged under it (MODEL_CALL, sent / skipped tags included); a non-nested low-level record is a direct call and is
// replayed through its send function; TEXTURE / MATERIAL brackets are INPUTS (Inputs), never predicted.
void replay(State &s, const gxlog::Log &log, ReplayHooks &h);

// ---- C5 INVARIANCE (CONTRACT_L2 §4.3 C5; the boundary replay of §3.4 in its DrawRun form S1-DR, r10; PLAN_model T4) ---------------
// A claimed run: its first / last TASK's classic seq (the ENTRY records' `a`), in flush order.
struct Run { uint32_t first, last; };
constexpr uint32_t kEntryTask = 0;	// an ENTRY record's `b` for a TASK: GxClassicDraw::TASK (W/dx8renderer.h:110; GxScene static_asserts it)
struct C5Sink {
	virtual ~C5Sink() {}
	// One INVARIANCE mismatch. point = entry (an unclaimed entry's start) | draw | call (after any other unit outside the runs) |
	// sortq | end | run (the run itself: unsourced, payload_unstable, run_range); seq = the classic entry. Logical fields are rs.<name>.
	virtual void invariance(uint32_t seq, const char *point, const char *field, uint64_t classic, uint64_t l2) = 0;
};
struct C5Counts {
	unsigned runs;				// runs replaced by their boundary replay
	unsigned scaledRuns;		// of them, with a scaled task (the wrapper's NORMALIZENORMALS FALSE after DrawRun)
	unsigned compares;			// compare points outside the runs (non-vacuity)
	unsigned mismatches;		// INVARIANCE mismatches (every field below included)
	unsigned unsourced;			// replay-arm input Applies with no source in the run (field unsourced:<bracket>) - a C5 failure
	unsigned payloadUnstable;	// classic re-Applies of one object in a run whose logged outputs differ (field payload_unstable:<bracket>)
	unsigned badRanges;			// runs whose ENTRY .. TASK_END range is not in the log (field run_range): left as recorded in both arms
};
// Replays one flush's log twice, in lockstep over the same records: `a` AS RECORDED (hA hears it exactly as replay() would: C6), and
// `b` with each run's records - its first TASK's ENTRY up to its last TASK's TASK_END - replaced by the boundary replay: the first task's
// setters + one Apply (PREPARE), the wrapper's NORMALIZENORMALS FALSE if any task of the run is scaled, the last task's setters without
// the NORMALIZENORMALS pair + one Apply (CARRY-OUT). Input brackets of those Applies come from the classic Apply of the same object at the
// corresponding task, or the nearest earlier one in the run. Both states are seeded by the caller; b.mutant == 5 is M5 (the carry-out
// replays the run's FIRST task). Compared OBSERVATIONALLY at every unit outside the runs and at the flush end (C5Sink).
void replayC5(State &a, State &b, const gxlog::Log &log, const Run *runs, unsigned nRuns, ReplayHooks &hA, C5Sink &h5, C5Counts &n);

} // namespace gxl2m
