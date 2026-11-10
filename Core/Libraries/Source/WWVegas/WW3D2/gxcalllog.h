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

// GeneralsX: WS-G Task 5: the `=2` call log's record format (CONTRACT_L2 §4.1, r9b).
// Header-only and ENGINE-FREE on purpose: the wrapper's check-mode hooks write it (T1), the device-less model in GXRender/Scene reads
// it (GxL2Model.h), and scripts/test/l2-model-test.cpp builds logs by hand. Nothing here runs unless the check is on.

#pragma once

#include <cstdint>
#include <vector>

namespace gxlog {

enum Op : uint16_t {
	// No enumerator may share a spelling with a name digested code uses (simid-closure matches constants by spelling: DRAW and READ
	// read as reaches from SubsystemInterface.h DRAW() and file.h READ - render-integration 11/10, closure on 41446a29e)
	// high-level wrapper entries (the model runs its PORT of each)
	SET_TEXTURE = 1,		// a = stage, payload = u64 texture id
	SET_MATERIAL,			// payload = u64 material id
	SET_SHADER,				// a = ShaderBits
	SET_TRANSFORM,			// a = D3DTRANSFORMSTATETYPE, payload = Mat4 (as stored / sent: transposed)
	SET_WORLD_IDENTITY,
	SET_VIEW_IDENTITY,
	SET_VB,					// a = stream, payload = VbDesc
	SET_VB_DYNAMIC,			// a = vba offset, b = vba count, payload = VbDesc
	SET_IB,					// a = index base offset, payload = IbDesc
	SET_IB_DYNAMIC,			// a = iba offset, b = index base offset, payload = IbDesc
	SET_INDEX_OFFSET,		// a = offset
	SET_LIGHT,				// a = index, b = 1 if non-null, payload = Light
	SET_LIGHT_ENV,			// b = 1 if non-null, payload = Lenv
	SET_RENDER_STATE,		// payload = Logical (Set_Render_State's source)
	INVALIDATE,				// Invalidate_Cached_Render_States
	OP_DRAW,				// payload = DrawRec; the Apply it makes follows as APPLY_BEGIN .. APPLY_END
	APPLY_BEGIN,
	APPLY_END,
	// low-level invocations, logged BEFORE the cache compare (CONTRACT r9b MODEL_CALL); flags & SENT = it reached the device
	RS = 32,				// a = state, b = value
	TSS,					// a = stage << 16 | state, b = value
	DX8_TEXTURE,			// a = stage, payload = u64 D3D texture
	DX8_MATERIAL,			// payload = Material
	DX8_LIGHT,				// a = index, b = 1 if non-null, payload = Light
	DX8_XFORM,				// a = transform, payload = Mat4
	STREAM,					// Apply's SetStreamSource: a = stream, b = stride, payload = u64 D3D buffer (0 = null)
	INDICES,				// Apply's SetIndices: b = base vertex index, payload = u64 D3D buffer (0 = null)
	VSHADER,				// Apply's Set_Vertex_Shader: a = FVF
	XFORM_RAW,				// Set_Transform's raw PROJECTION / TEXTUREn send (no cache): a = transform, payload = Mat4 - replayed for
							// state, never compared at call level
	// markers and device-access provenance
	ENTRY = 64,				// a = classic list seq, b = GxClassicDraw::Kind - appended BEFORE the entry's setters
	CAT_BEGIN,				// payload = u64 category id
	TASK_END,				// a = seq, after the task's epilogue (NORMALIZENORMALS FALSE)
	FLUSH_END,				// before the flush's null-buffer setters
	SORTQ,					// a = seq, payload = Logical (the copy Insert_Triangles stored)
	DEVICE_READ,			// a device read (C9's readbacks, _Get_DX8_Transform of a non-world/view type): changes nothing
	RESOURCE,				// buffer / texture creation or upload: creates no tracked render state
	RAW_ACCESS,				// an unclassified _Get_D3D_Device8(): payload = u64 caller address; every slot UNKNOWN from here
};

// ---- the by-value payloads (the model aliases these: gxl2m::Mat4 = gxlog::Mat4 ...) -------------------------------------------
constexpr int kStages = 8;	// MAX_TEXTURE_STAGES
constexpr int kLights = 4;
struct Mat4 { float m[4][4]; };
struct Light { uint32_t type; float diffuse[4], specular[4], ambient[4], position[3], direction[3], range, falloff, att0, att1, att2,
	theta, phi; };	// D3DLIGHT8's layout
struct Material { float diffuse[4], ambient[4], specular[4], emissive[4], power; };	// D3DMATERIAL8's layout
static_assert(sizeof(Light) == 104 && sizeof(Material) == 68, "D3D8 layouts");

// RenderStateStruct (W/dx8wrapper.h:300-326) - the LOGICAL state Apply sends from. Pointers are ids; a buffer carries the descriptor
// Apply reads from the object (D3D handle, FVF size, FVF, vertex count).
enum : uint32_t { BT_DX8 = 0, BT_SORTING = 1, BT_DYNAMIC_DX8 = 2, BT_DYNAMIC_SORTING = 3, BT_INVALID = 4 };	// W/dx8wrapper.h:82-88
struct VbDesc { uint32_t type; uint64_t obj, d3d; uint32_t fvfSize, fvf, vertexCount; };
struct IbDesc { uint32_t type; uint64_t obj, d3d; };
struct Logical {
	uint32_t shaderBits; uint64_t material; uint64_t tex[kStages];
	Light lights[kLights]; bool lightEnable[kLights];
	Mat4 world, view;							// stored TRANSPOSED, as the wrapper stores them
	VbDesc vb[2]; IbDesc ib;
	uint16_t vbaOffset, vbaCount, ibaOffset, indexBaseOffset;
};

// A light environment as Set_Light_Environment reads it (W/dx8wrapper.cpp:4656-4769; GX_Lenv_Same's fields, :4596-4633).
struct LenvLight { bool point; float dir[3], diff[3], pdiff[3], pamb[3], pcenter[3], irad, orad; };
struct Lenv { int count; uint32_t ambientColor; LenvLight l[kLights]; };	// ambientColor = Convert_Color(Get_Equivalent_Ambient(), 0)

// DX8Wrapper::Draw's arguments, and Draw_Sorting_IB_VB's dynamic buffers (W/dx8wrapper.cpp:3369-3466).
struct DrawArgs { unsigned prim, startIndex, polyCount, minIndex, vertexCount; };
struct SortBind { uint64_t vb, ib; uint32_t fvfSize, fvf, vbOffset, ibOffset; };	// ibOffset = dyn_ib_access.IndexBufferOffset
// An OP_DRAW record's payload: the arguments and the two switches Draw read, and the sorting draw's dynamic buffers.
struct DrawRec { DrawArgs args; uint32_t lowBound; uint8_t trianglesOn, hasSort, pad[2]; SortBind sb; };

// What ShaderClass::Apply reads besides its bits and its cache (ZW/shader.cpp:409-1060; ZW = GeneralsMD/Code/Libraries/Source/WWVegas/
// WW3D2/), copied into the flush-start snapshot by the call log (MODEL_RESEARCH MH4).
struct ShaderGlobals {
	bool fogAllowed;			// ShaderClass::Is_Fog_Allowed() / the caps' fog support as Apply tests it
	bool fogEnable;				// DX8Wrapper FogEnable (Set_Fog, W/dx8wrapper.h:1238-1252)
	uint32_t fogColor;			// DX8Wrapper FogColor
	uint32_t polygonCullMode;	// ShaderClass::_PolygonCullMode (ZW/shader.cpp:52)
	uint32_t textureOpCaps;		// the caps' TextureOpCaps (ZW/shader.cpp:413)
	bool npatches;				// Support_NPatches()
	uint32_t vendor;			// the caps' vendor id, as Apply's voodoo3 test reads it (ZW/shader.cpp:552-553)
	uint32_t device;			// DX8Caps::Get_Device() (ZW/shader.cpp:553)
	bool modAlphaAddClr;		// DX8Caps::Support_ModAlphaAddClr() (ZW/shader.cpp:847)
	uint32_t npatchesLevel;		// WW3D::Get_NPatches_Level() (ZW/shader.cpp:1053; 1 at start, ZW/ww3d.cpp:217)
};

// The flush-start snapshot: every wrapper static the model needs, by value. DX8Wrapper::Gx_Log_Snapshot fills it;
// gxl2m::seed() turns it into the model's State. Device bindings are not in it: no static says what the device has bound.
struct Snapshot {
	Logical rs; uint32_t changed;
	uint32_t cacheRS[256]; uint32_t cacheTSS[kStages][32]; uint64_t devTex[kStages];
	Light lights[kLights]; bool lightOn[kLights], lightValid[kLights]; uint32_t lightSerial;
	Lenv lenvKey; bool lenvKeyValid; uint32_t lenvSerial;
	Mat4 xformWorld, xformView; bool xformShadowValid[2];
	Material matShadow; bool matShadowValid;
	uint32_t curShader; bool shaderDirty; uint32_t vsCache; uint32_t wrapSkip; int maxTexPerPass;
	bool truthOn, captureOn;
	uint32_t truthRS[256]; bool truthRSKnown[256]; uint32_t truthTSS[kStages][32]; bool truthTSSKnown[kStages][32];
	Material matSent; bool matSentValid; Mat4 projSent; bool projSentValid; Mat4 texmSent[kStages]; uint32_t texmValid;
	Mat4 viewSent; bool viewSentValid; uint32_t xformValid;
	ShaderGlobals sg;
};

enum Bracket : uint8_t { BR_NONE = 0, BR_SHADER, BR_TEXTURE, BR_MATERIAL, BR_LIGHTS, BR_XFORM, BR_BINDINGS };
enum : uint8_t {
	SENT = 1,		// a low-level invocation that reached the device (not skipped by its cache)
	NESTED = 2,		// emitted INSIDE a high-level entry or an Apply (GxLogDepth > 0): its parent's prediction covers it
};

struct Record {
	uint16_t op;
	uint8_t bracket;		// which step of Apply_Render_State_Changes made a low-level invocation (BR_TEXTURE: stage in `stage`)
	uint8_t flags;
	uint8_t stage;
	uint8_t pad[3];
	uint32_t a, b;
	uint32_t payload;		// byte offset into Log::blob, or ~0u
	uint32_t entry;			// the newest classic list entry when the call was made
};
static_assert(sizeof(Record) == 24, "record layout");

// One flush's log: the records and their by-value payloads. Reused across flushes (cleared, never freed) in check mode.
struct Log {
	std::vector<Record> rec;
	std::vector<uint8_t> blob;
	// Payloads start on an 8-byte boundary (the vector's buffer is new-aligned), so get<T> never yields a misaligned pointer for the
	// payload types (u64 ids, VbDesc, Logical, Material, Light, Mat4, Lenv, DrawRec).
	uint32_t put(const void *p, uint32_t n)
	{
		while (blob.size() % 8u) blob.push_back(0);
		const uint32_t at = (uint32_t)blob.size();
		blob.insert(blob.end(), (const uint8_t *)p, (const uint8_t *)p + n);
		return at;
	}
	template <class T> const T *get(uint32_t at) const { return at == ~0u ? nullptr : (const T *)(blob.data() + at); }
	void clear() { rec.clear(); blob.clear(); }
};

} // namespace gxlog
