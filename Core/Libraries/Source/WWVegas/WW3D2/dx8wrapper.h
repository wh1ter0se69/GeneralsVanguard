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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8wrapper.h                           $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 2:40p                                              $*
 *                                                                                             *
 *                    $Revision:: 92                                                          $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Render to shadow buffer texture support														*
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include <Utility/bit_adapter.h>
#include "dllist.h"
// GeneralsX:
// Use angle brackets (not quotes) to skip local d3d8.h stub and get DXVK headers
// DXVK's d3d8.h includes d3d8types.h internally - no manual includes needed
#include <d3d8.h>
#include "WWMath/matrix4.h"
#include "statistics.h"
#include "WWLib/wwstring.h"
#include "WW3D2/lightenvironment.h"
#include "WW3D2/shader.h"
#include "WWMath/vector4.h"
#include "WWLib/cpudetect.h"
#include "dx8caps.h"

#include "texture.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "WW3D2/vertmaterial.h"
// GeneralsX: the `=2` call log's record format (engine-free) and the caller a RAW_ACCESS names.
#include "gxcalllog.h"
#if defined(__GNUC__) || defined(__clang__)
#define GX_LOG_CALLER() __builtin_return_address(0)
#elif defined(_MSC_VER)
#include <intrin.h>
#define GX_LOG_CALLER() _ReturnAddress()
#else
#define GX_LOG_CALLER() nullptr
#endif

/*
** Registry value names
*/
#define	VALUE_NAME_RENDER_DEVICE_NAME					"RenderDeviceName"
#define	VALUE_NAME_RENDER_DEVICE_WIDTH				"RenderDeviceWidth"
#define	VALUE_NAME_RENDER_DEVICE_HEIGHT				"RenderDeviceHeight"
#define	VALUE_NAME_RENDER_DEVICE_DEPTH				"RenderDeviceDepth"
#define	VALUE_NAME_RENDER_DEVICE_WINDOWED			"RenderDeviceWindowed"
#define	VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH		"RenderDeviceTextureDepth"

const unsigned MAX_TEXTURE_STAGES=8;
const unsigned MAX_VERTEX_STREAMS=2;
const unsigned MAX_VERTEX_SHADER_CONSTANTS=96;
const unsigned MAX_PIXEL_SHADER_CONSTANTS=8;
const unsigned MAX_SHADOW_MAPS=1;

#define prevVer
#define nextVer


enum {
	BUFFER_TYPE_DX8,
	BUFFER_TYPE_SORTING,
	BUFFER_TYPE_DYNAMIC_DX8,
	BUFFER_TYPE_DYNAMIC_SORTING,
	BUFFER_TYPE_INVALID
};

class VertexMaterialClass;
class CameraClass;
class LightEnvironmentClass;
class RenderDeviceDescClass;
class VertexBufferClass;
class DynamicVBAccessClass;
class IndexBufferClass;
class DynamicIBAccessClass;
class TextureClass;
class ZTextureClass;
class LightClass;
class SurfaceClass;
class DX8Caps;

struct DX8FrameStatistics
{
	DX8FrameStatistics() :
		matrix_changes(0),
		material_changes(0),
		vertex_buffer_changes(0),
		index_buffer_changes(0),
		light_changes(0),
		texture_changes(0),
		render_state_changes(0),
		texture_stage_state_changes(0),
		dx8_calls(0),
		draw_calls(0)
	{
	}

	unsigned matrix_changes;
	unsigned material_changes;
	unsigned vertex_buffer_changes;
	unsigned index_buffer_changes;
	unsigned light_changes;
	unsigned texture_changes;
	unsigned render_state_changes;
	unsigned texture_stage_state_changes;
	unsigned dx8_calls;
	unsigned draw_calls;
};

#define DX8_RECORD_MATRIX_CHANGE()				FrameStatistics.matrix_changes++
#define DX8_RECORD_MATERIAL_CHANGE()			FrameStatistics.material_changes++
#define DX8_RECORD_VERTEX_BUFFER_CHANGE()		FrameStatistics.vertex_buffer_changes++
#define DX8_RECORD_INDEX_BUFFER_CHANGE()		FrameStatistics.index_buffer_changes++
#define DX8_RECORD_LIGHT_CHANGE()				FrameStatistics.light_changes++
#define DX8_RECORD_TEXTURE_CHANGE()				FrameStatistics.texture_changes++
#define DX8_RECORD_RENDER_STATE_CHANGE()		FrameStatistics.render_state_changes++
#define DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE() FrameStatistics.texture_stage_state_changes++
// GeneralsX: [GXCENSUS] (GX_DRAWCENSUS=1 with GX_GFX=1): what the main thread asks of the device, per [GXGFXF] span.
// Counters are never-reset totals; GameClient/GXGfxProbe.h reads deltas at each sub-span mark.
// "_dev" counts calls reaching IDirect3DDevice8, "_same" counts calls re-sending the value the device already holds.
// Off (default) a site is one static-bool test; the hottest setter ASK counters are not counted at all. Client-only.
enum DX8CensusCounter
{
	DX8C_DRAW,			// draws reaching the device: THE never-reset whole-frame total ([GXGFX] dc= reads it too)
	DX8C_WORLD_DEV,		// world matrices sent (_Set_DX8_Transform)
	DX8C_WORLD_SAME,	// ... equal to the world matrix last sent through the wrapper
	DX8C_VIEW_DEV,
	DX8C_VIEW_SAME,
	DX8C_LENV,			// Set_Light_Environment calls
	DX8C_LENV_SAME,		// ... building the same lights + ambient as the previous call
	DX8C_LIGHT_DEV,		// SetLight/LightEnable sent
	DX8C_MAT,			// Set_Material asks
	DX8C_MAT_DEV,		// SetMaterial sent
	DX8C_MAT_SAME,		// ... byte-identical to the material last sent
	DX8C_TEX,			// Set_Texture asks that changed the wrapper's texture
	DX8C_TEX_DEV,		// SetTexture sent
	DX8C_TSS_DEV,		// SetTextureStageState sent (asks are not counted: ~150k a frame, and counting them cost)
	DX8C_RS_DEV,		// SetRenderState sent (the same)
	DX8C_SHADER,		// Set_Shader asks that changed the wrapper's shader
	DX8C_SHADER_APPLY,	// ShaderClass::Apply runs (Apply_Render_State_Changes)
	DX8C_VB_DEV,		// SetStreamSource(0, ...) sent by the wrapper, null bindings included
	DX8C_VB_SAME,		// ... same buffer as the last one the wrapper sent (UPPER bound: other code binds directly)
	DX8C_FVF_DEV,		// SetVertexShader sent by the wrapper (its cache is #if 0'd)
	DX8C_FVF_SAME,		// ... same FVF as the wrapper last sent (UPPER bound, as vb_same)
	DX8C_IB_DEV,		// SetIndices sent by the wrapper, null bindings included
	DX8C_IB_SAME,		// ... same buffer and base index (UPPER bound, as vb_same)
	DX8C_LK_DISC,		// vertex/index buffer locks by flag (wrapper buffers and the shadow rings)
	DX8C_LK_NOOW,
	DX8C_LK_PLAIN,
	DX8C_DYNVB_WRAP,	// the dynamic VB ran out and restarted at 0 (the next lock DISCARDs)
	DX8C_DYNVB_GROW,	// ... was re-created larger
	DX8C_DYNIB_WRAP,
	DX8C_DYNIB_GROW,
	DX8C_SORT_ORDER_NS,	// SortingRendererClass: stable_sort of the nodes
	DX8C_SORT_FILL_NS,	// ... vertex copy + per-triangle index/depth fill (Reforged's parallel fill, roadmap 1c)
	DX8C_SORT_TRI_NS,	// ... the triangle sort
	DX8C_SORT_TRIS,		// ... triangles flushed
	DX8C_OBJ,			// renderOneObject renders (W3DScene.cpp), and of those:
	DX8C_OBJ_SHROUD,	// ... drawn with the shroud material pass
	DX8C_OBJ_MULTILOD,	// ... whose render object has more than one LOD
	DX8C_OBJ_LT4,		// ... bounding sphere under 4 / 8 / 16 px of screen radius
	DX8C_OBJ_LT8,
	DX8C_OBJ_LT16,
	DX8C_MESH,			// sub-objects (meshes) of those objects, the same buckets: a draws-per-pass proxy
	DX8C_MESH_LT4,
	DX8C_MESH_LT8,
	DX8C_MESH_LT16,
	DX8C_SSTAT_DRAW,	// static shadow volume draws (W3DVolumetricShadow.cpp RenderMeshVolume)
	DX8C_SSTAT_VBSW,	// ... that changed vertex buffer from the previous static draw (roadmap 2b's VB spread)
	DX8C_OBJ_FOG,		// renderOneObject renders of fog-of-war ghost objects (also counted in obj and its buckets)
	DX8C_SKIP_XFORM,	// GX_WRAPSKIP: world/view sends skipped (the device held the matrix) - with the skips on,
	DX8C_SKIP_MAT,		// world_same/view_same/mat_same read ~0 because a skipped send is never made; these say what
	DX8C_SKIP_LIGHT,	// was skipped instead (lights: slots not re-flagged, enabled or already-disabled)
	DX8C_SKIP_LENV,		// GX_WRAPSKIP 8: Set_Light_Environment calls answered by the input compare (no light rebuilt)
	// GeneralsX: L2 census (GX_L2_CENSUS=1 with GX_DRAWCENSUS=1 GX_GFX=1; GXRender/Scene/
	// GxScene.cpp): the classic draw list of each Flush (dx8renderer.h GxClassicDraw), counted before it is drawn.
	DX8C_L2_FLUSH,		// Flushes with a non-empty list
	DX8C_L2_TASK,		// base-pass render tasks (one classic draw each, unless sorted), rigid and skin
	DX8C_L2_RIGID,		// ... rigid ones, and of those:
	DX8C_L2_R_SORT,		// ... sorted (SORT flag with sorting on: drawn later by the triangle sorter); of the UNSORTED ones:
	DX8C_L2_R_BLEND,	// ... a blending shader, no depth write, or the forced multiply
	DX8C_L2_R_OVR,		// ... a per-object override (alpha override or material override user data)
	DX8C_L2_R_CAM,		// ... ALIGNED / ORIENTED (world from the camera)
	DX8C_L2_R_SCALE,	// ... object scale != 1 (NORMALIZENORMALS around the draw)
	DX8C_L2_R_LENV0,	// ... no light environment (draws with the lights already set)
	DX8C_L2_R_PASS1,	// ... a second or later pass of a multi-pass mesh
	DX8C_L2_R_ELIG,		// ... PRE-CONTRACT PROXY of phase-1 eligible: unsorted, opaque, no override, not camera-aligned
	DX8C_L2_R_BIN,		// ... distinct (texture category, polygon renderer) pairs among r_elig in one Flush: instanced draws
	DX8C_L2_SKIN,		// skins deformed on the CPU into the dynamic VB
	DX8C_L2_SKIN_SORT,	// ... in a sorting container
	DX8C_L2_SKIN_B1,	// ... in a second or later dynamic-VB batch (the container's visible skins exceed 65535 vertices)
	DX8C_L2_S_TASK,		// skin base-pass render tasks
	DX8C_L2_MATPASS,	// procedural material passes (rigid and skin)
	DX8C_L2_DMATPASS,	// delayed procedural material passes
	DX8C_L2_DECAL,		// decal meshes
	DX8C_N
};

#define DX8_CENSUS(c)							(DX8Wrapper::CensusOn ? (void)++DX8Wrapper::Census[c] : (void)0)

#define DX8_RECORD_DX8_CALLS()					FrameStatistics.dx8_calls++
#define DX8_RECORD_DRAW_CALLS()					(FrameStatistics.draw_calls++, ++Census[DX8C_DRAW])

extern bool _DX8SingleThreaded;

// GeneralsX: called right before each DrawIndexedPrimitive the wrapper issues (DX8Wrapper::Draw and
// Draw_Sorting_IB_VB) with the FINAL parameters - the state the draw uses - for the L2 state-check (GXRender/Scene); null unless
// GX_L2_CHECK installs it. sorting_path: the sorting branch, which binds its own dynamic buffers past render_state.
extern void (*GxDrawHook)(unsigned primitive_type, unsigned min_vertex_index, unsigned vertex_count, unsigned start_index,
	unsigned polygon_count, bool sorting_path);
// Seat (GX_DRAWRUN_DIGEST=2): called right after the same two draws are recorded (the ring's Rec_Draw_Indexed or the device call) - the digest
// bracket closes there, so direct device draws issued after a run's last own draw are never inside it. Null otherwise: one pointer test per hooked draw.
extern void (*GxPostDrawHook)();

void DX8_Assert();
void Log_DX8_ErrorCode(unsigned res);

WWINLINE void DX8_ErrorCode(unsigned res)
{
	if (res==D3D_OK) return;
	Log_DX8_ErrorCode(res);
}

// GeneralsX: DX8CALL / DX8CALL_HRES are known device calls - _Get_D3D_Device8_Call, never RAW_ACCESS.
#ifdef WWDEBUG
#define DX8CALL_HRES(x,res) DX8_Assert(); res = DX8Wrapper::_Get_D3D_Device8_Call()->x; DX8_ErrorCode(res); DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL(x) DX8_Assert(); DX8_ErrorCode(DX8Wrapper::_Get_D3D_Device8_Call()->x); DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL_D3D(x) DX8_Assert(); DX8_ErrorCode(DX8Wrapper::_Get_D3D8()->x); DX8Wrapper::Increment_DX8_CallCount();
#define DX8_THREAD_ASSERT() if (_DX8SingleThreaded) { WWASSERT_PRINT(DX8Wrapper::_Get_Main_Thread_ID()==ThreadClass::_Get_Current_Thread_ID(),"DX8Wrapper::DX8 calls must be called from the main thread!"); }
#else
#define DX8CALL_HRES(x,res) res = DX8Wrapper::_Get_D3D_Device8_Call()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL(x) DX8Wrapper::_Get_D3D_Device8_Call()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL_D3D(x) DX8Wrapper::_Get_D3D8()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8_THREAD_ASSERT() ;
#endif


#define no_EXTENDED_STATS
// EXTENDED_STATS collects additional timing statistics by turning off parts
// of the 3D drawing system (terrain, objects, etc.)
#ifdef EXTENDED_STATS
class DX8_Stats
{
public:
	bool m_showingStats;
	bool m_disableTerrain;
	bool m_disableWater;
	bool m_disableObjects;
	bool m_disableOverhead;
	bool m_disableConsole;
	int  m_debugLinesToShow;
	int	 m_sleepTime;
public:
	DX8_Stats::DX8_Stats() {
		m_disableConsole = m_showingStats = m_disableTerrain = m_disableWater = m_disableOverhead = m_disableObjects = false;
		m_sleepTime = 0;
		m_debugLinesToShow = -1; // -1 means show all expected lines of output
	}
};
#endif


// This virtual interface was added for the Generals RTS.
// It is called before resetting the dx8 device to ensure
// that all dx8 resources are released.  Otherwise reset fails. jba.
class DX8_CleanupHook
{
public:
	virtual void ReleaseResources()=0;
	virtual void ReAcquireResources()=0;
};


struct RenderStateStruct
{
	ShaderClass shader;
	VertexMaterialClass* material;
	TextureBaseClass * Textures[MAX_TEXTURE_STAGES];
	D3DLIGHT8 Lights[4];
	bool LightEnable[4];
  //unsigned lightsHash;
	// GeneralsX:
	// Changed from D3DMATRIX to Matrix4x4 - fighter19 pattern eliminates conversion functions
	// Matrices stored transposed, cast to D3DMATRIX* when calling D3D APIs
	Matrix4x4 world;
	Matrix4x4 view;
	unsigned vertex_buffer_types[MAX_VERTEX_STREAMS];
	unsigned index_buffer_type;
	unsigned short vba_offset;
	unsigned short vba_count;
	unsigned short iba_offset;
	VertexBufferClass* vertex_buffers[MAX_VERTEX_STREAMS];
	IndexBufferClass* index_buffer;
	unsigned short index_base_offset;

	RenderStateStruct();
	~RenderStateStruct();

	RenderStateStruct& operator= (const RenderStateStruct& src);
};

// GeneralsX: d3d8 call ring. GX_D3D8RING=1: inside Begin_Record/End_Record the DX8Wrapper setters and draws are
// recorded into a byte ring and replayed by a render thread; End_Record waits until everything is replayed.
// 3 = replay inline (measurement); 0 = direct. Unset: 1 on Windows (DXVK only) and macOS, 0 on iOS and Linux.
// Unrecorded device access drains the ring first; Present and Reset stay on the main thread. Client-only.
class DX8Ring
{
public:
	static int Mode;			// 0 off, 1 render thread, 3 inline replay; from GX_D3D8RING (or the platform default) at Init
	static bool Recording;		// main thread only
	static bool Pending;		// main thread only: recorded commands may not be replayed yet

	static void Init();
	static void Shutdown();		// joins the render thread (DX8Wrapper::Shutdown)
	static void Begin_Record();
	static void Resume();		// an audited point inside the bracket: recording may go on after a Fallback
	static void End_Record();
	static void Drain();		// main: every recorded command has been executed on return
	static void Fallback();		// an unrecorded device access while Pending: counted, Drain, and record nothing more until
								// Resume (the caller may keep the raw device pointer it was handed)
	static void Frame_Stats();	// once per Present: prints [GXRING] every 120 frames

	static void Rec_Render_State(DWORD state, DWORD value);
	static void Rec_Texture_Stage_State(DWORD stage, DWORD type, DWORD value);
	static void Rec_Texture(DWORD stage, IDirect3DBaseTexture8 *tex);
	static void Rec_Release(IUnknown *obj);	// the render thread drops this reference in stream order
	static void Rec_Call(void (*fn)(IDirect3DDevice8 *, void *), void *ctx);	// DX8Wrapper::Device_Call
	static void Rec_Transform(DWORD type, const D3DMATRIX *m);
	static void Rec_Material(const D3DMATERIAL8 *mat);
	static void Rec_Light(DWORD index, const D3DLIGHT8 *light);
	static void Rec_Light_Enable(DWORD index, BOOL enable);
	static void Rec_Vertex_Shader(DWORD handle);
	static void Rec_Pixel_Shader(DWORD handle);
	static void Rec_Vertex_Shader_Constant(DWORD reg, const void *data, DWORD count);
	static void Rec_Pixel_Shader_Constant(DWORD reg, const void *data, DWORD count);
	static void Rec_Clip_Plane(DWORD index, const float *plane);
	static void Rec_Stream_Source(DWORD stream, IDirect3DVertexBuffer8 *vb, DWORD stride);
	static void Rec_Indices(IDirect3DIndexBuffer8 *ib, DWORD base);
	static void Rec_Draw_Indexed(DWORD type, DWORD min_index, DWORD num_vertices, DWORD start_index, DWORD prim_count);
	// A dynamic VB/IB lock while recording: the caller writes into ring memory; at Stage_End the render thread does
	// the real Lock(offset, size, flags) + copy + Unlock at that point of the stream. Nests LIFO, like the lock scopes.
	static void *Stage_VB(IDirect3DVertexBuffer8 *vb, DWORD offset, DWORD size, DWORD flags);
	static void *Stage_IB(IDirect3DIndexBuffer8 *ib, DWORD offset, DWORD size, DWORD flags);
	static void Stage_End();	// at the matching unlock; records the copy
};

/**
** DX8Wrapper
**
** DX8 interface wrapper class.  This encapsulates the DX8 interface; adding redundant state
** detection, stat tracking, etc etc.  In general, we will wrap all DX8 calls with at least
** an WWINLINE function so that we can add stat tracking, etc if needed.  Direct access to the
** D3D device will require "friend" status and should be granted only in extreme circumstances :-)
*/
class DX8Wrapper
{
	enum ChangedStates {
		WORLD_CHANGED	=	1<<0,
		VIEW_CHANGED	=	1<<1,
		LIGHT0_CHANGED	=	1<<2,
		LIGHT1_CHANGED	=	1<<3,
		LIGHT2_CHANGED	=	1<<4,
		LIGHT3_CHANGED	=	1<<5,
		TEXTURE0_CHANGED=	1<<6,
		TEXTURE1_CHANGED=	1<<7,
		TEXTURE2_CHANGED=	1<<8,
		TEXTURE3_CHANGED=	1<<9,
		MATERIAL_CHANGED=	1<<14,
		SHADER_CHANGED	=	1<<15,
		VERTEX_BUFFER_CHANGED = 1<<16,
		INDEX_BUFFER_CHANGED = 1 << 17,
		WORLD_IDENTITY=	1<<18,
		VIEW_IDENTITY=		1<<19,

		TEXTURES_CHANGED=
			TEXTURE0_CHANGED|TEXTURE1_CHANGED|TEXTURE2_CHANGED|TEXTURE3_CHANGED,
		LIGHTS_CHANGED=
			LIGHT0_CHANGED|LIGHT1_CHANGED|LIGHT2_CHANGED|LIGHT3_CHANGED,
	};

	static void Draw_Sorting_IB_VB(
		unsigned primitive_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);

	static void Draw(
		unsigned primitive_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index=0,
		unsigned short vertex_count=0);

public:
#ifdef EXTENDED_STATS
	static DX8_Stats stats;
#endif

	static bool Init(void * hwnd, bool lite = false);
	static void Shutdown();

	static void SetCleanupHook(DX8_CleanupHook *pCleanupHook) {m_pCleanupHook = pCleanupHook;};
	/*
	** Some WW3D sub-systems need to be initialized after the device is created and shutdown
	** before the device is released.
	*/
	static void	Do_Onetime_Device_Dependent_Inits();
	static void Do_Onetime_Device_Dependent_Shutdowns();

	static bool Is_Device_Lost() { return IsDeviceLost; }
	static bool Is_Initted() { return IsInitted; }

	static bool Has_Stencil ();
	static void Get_Format_Name(unsigned int format, StringClass *tex_format);

	/*
	** Rendering
	*/
	static void Begin_Scene();
	static void End_Scene(bool flip_frame = true);

	// Flip until the primary buffer is visible.
	static void Flip_To_Primary();

	static void Clear(bool clear_color, bool clear_z_stencil, const Vector3 &color, float dest_alpha=0.0f, float z=1.0f, unsigned int stencil=0);

	static void	Set_Viewport(CONST D3DVIEWPORT8* pViewport);

	static void Set_Vertex_Buffer(const VertexBufferClass* vb, unsigned stream=0);
	static void Set_Vertex_Buffer(const DynamicVBAccessClass& vba);
	static void Set_Index_Buffer(const IndexBufferClass* ib,unsigned short index_base_offset);
	static void Set_Index_Buffer(const DynamicIBAccessClass& iba,unsigned short index_base_offset);
	static void Set_Index_Buffer_Index_Offset(unsigned offset);

	static void Get_Render_State(RenderStateStruct& state);
	static void Set_Render_State(const RenderStateStruct& state);
	static void Release_Render_State();

	static void Set_DX8_Material(const D3DMATERIAL8* mat);

	static void Set_Gamma(float gamma,float bright,float contrast,bool calibrate=true,bool uselimit=true);

	// Set_ and Get_Transform() functions take the matrix in Westwood convention format.

	static void Set_DX8_ZBias(int zbias);
	static void Set_Projection_Transform_With_Z_Bias(const Matrix4x4& matrix,float znear, float zfar);	// pointer to 16 matrices

	static void Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix4x4& m);
	static void Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix3D& m);
	static void Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m);
	static void Set_World_Identity();
	static void Set_View_Identity();
	static bool Is_World_Identity();
	static bool Is_View_Identity();

	// Note that *_DX8_Transform() functions take the matrix in DX8 format - transposed from Westwood convention.

	static void _Set_DX8_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m);
	// GeneralsX: Matrix4x4 for GLM compatibility (fighter19 pattern)
	static void _Get_DX8_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m);

	static void Set_DX8_Light(int index,D3DLIGHT8* light);
	static void Set_DX8_Render_State(D3DRENDERSTATETYPE state, unsigned value);
	static void Set_DX8_Clip_Plane(DWORD Index, CONST float* pPlane);
	static void Set_DX8_Texture_Stage_State(unsigned stage, D3DTEXTURESTAGESTATETYPE state, unsigned value);
	static void Set_DX8_Texture(unsigned int stage, IDirect3DBaseTexture8* texture);
	static void Set_Light_Environment(LightEnvironmentClass* light_env);
	static LightEnvironmentClass* Get_Light_Environment() { return Light_Environment; }
	static void Set_Fog(bool enable, const Vector3 &color, float start, float end);

	static WWINLINE const D3DLIGHT8& Peek_Light(unsigned index);
	static WWINLINE bool Is_Light_Enabled(unsigned index);

	static bool Validate_Device();

	// Deferred

	static void Set_Shader(const ShaderClass& shader);
	static void Get_Shader(ShaderClass& shader);
	static void Set_Texture(unsigned stage,TextureBaseClass* texture);
	static void Set_Material(const VertexMaterialClass* material);
	static void Set_Light(unsigned index,const D3DLIGHT8* light);
	static void Set_Light(unsigned index,const LightClass &light);

	static void Apply_Render_State_Changes();	// Apply deferred render state changes (will be called automatically by Draw...)

	static void Draw_Triangles(
		unsigned buffer_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);
	static void Draw_Triangles(
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);
	static void Draw_Strip(
		unsigned short start_index,
		unsigned short index_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);

	/*
	** Resources
	*/

	static IDirect3DVolumeTexture8* _Create_DX8_Volume_Texture
	(
		unsigned int width,
		unsigned int height,
		unsigned int depth,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED
	);

	static IDirect3DCubeTexture8* _Create_DX8_Cube_Texture
	(
		unsigned int width,
		unsigned int height,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED,
		bool rendertarget=false
	);


	static IDirect3DTexture8* _Create_DX8_ZTexture
	(
		unsigned int width,
		unsigned int height,
		WW3DZFormat zformat,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED
	);


	static IDirect3DTexture8 * _Create_DX8_Texture
	(
		unsigned int width,
		unsigned int height,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED,
		bool rendertarget=false
	);
	static IDirect3DTexture8 * _Create_DX8_Texture(const char *filename, MipCountType mip_level_count);
	static IDirect3DTexture8 * _Create_DX8_Texture(IDirect3DSurface8 *surface, MipCountType mip_level_count);

	static IDirect3DSurface8 * _Create_DX8_Surface(unsigned int width, unsigned int height, WW3DFormat format);
	static IDirect3DSurface8 * _Create_DX8_Surface(const char *filename);
	static IDirect3DSurface8 * _Get_DX8_Front_Buffer();
	static SurfaceClass * _Get_DX8_Back_Buffer(unsigned int num=0);

	static void _Copy_DX8_Rects(
			IDirect3DSurface8* pSourceSurface,
			CONST RECT* pSourceRectsArray,
			UINT cRects,
			IDirect3DSurface8* pDestinationSurface,
			CONST POINT* pDestPointsArray
	);

	static void _Update_Texture(TextureClass *system, TextureClass *video);
	static void Flush_DX8_Resource_Manager(unsigned int bytes=0);
	static unsigned int Get_Free_Texture_RAM();

	static unsigned _Get_Main_Thread_ID() { return _MainThreadID; }
	static const D3DADAPTER_IDENTIFIER8& Get_Current_Adapter_Identifier() { return CurrentAdapterIdentifier; }

	/*
	** Statistics
	*/
	static void Begin_Statistics();
	static void End_Statistics();
	static const DX8FrameStatistics& Get_Last_Frame_Statistics();
	static unsigned long Get_FrameCount();
	static void Increment_DX8_CallCount() { DX8_RECORD_DX8_CALLS(); }

	// GeneralsX: [GXGFX] (GameClient/GXGfxProbe.h) times Present from outside WW3D2,
	// which cannot include GameClient headers: End_Scene calls this before (false) and after (true) Present.
	// Null unless GX_GFX or GX_HITCH is set, so the unprobed cost is one pointer test per frame.
	static void (*Present_Probe)(bool after);
	// GeneralsX: seat: called in End_Scene right before Present (flipping frames only) - the C2b digest
	// bracket's frame end (GXRender/Scene GxScene.cpp, GX_DRAWRUN_DIGEST=2). Null otherwise: one pointer test per presented frame.
	static void (*GxPrePresentHook)();
	// GeneralsX: every Present call so far (never reset - FrameCount is, at
	// Reset), the submission epoch GxInterop's packet ring keys its progress guard on; and a hook run at the start of
	// Reset_Device, after the ring drained, so external work can release what it holds on the device.
	static unsigned long long Get_Present_Count() { return PresentCount; }
	static void (*Pre_Reset_Hook)();
	static void (*Pre_Release_Hook)();	// external work drains and drops its device references before Release_Device
	// GeneralsX: W3DWorldSeam drops its D3DPOOL_DEFAULT world targets here, before Reset and
	// before Release_Device (DXVK refuses Reset while DEFAULT-pool resources live). Called next to Pillarbox_Cleanup.
	static void (*Default_Pool_Release_Hook)();

	// GeneralsX: A whole-frame draw count at the device boundary.
	// Neither existing counter counts what reaches the D3D8 device: Debug_Statistics' draw_calls ([GXDRAW] draws)
	// adds sorting inserts and skinned meshes on top of the real draws, and FrameStatistics.draw_calls moves only at
	// the two wrapper sites and is zeroed by Begin_Render after the shadow/water pre-pass. This total moves at the
	// two wrapper sites (DX8_RECORD_DRAW_CALLS) AND at every direct device draw, which must call Record_Direct_Draw()
	// (scripts/test/gx-draw-sites-lint.sh fails a site that does not). It is never reset: [GXGFX] reads it at each
	// Present and takes the delta, so no Begin/End_Statistics reset can drop a pass.
	static void Record_Direct_Draw() { ++Census[DX8C_DRAW]; }
	static unsigned long long Get_Draw_Calls_Total() { return Census[DX8C_DRAW]; }

	// GeneralsX: [GXCENSUS] totals (enum DX8CensusCounter above). CensusOn is set once by
	// W3DDisplay when GX_DRAWCENSUS and GX_GFX are set; nothing below runs a compare or a clock while it is false.
#if defined(GX_TEST_INSTRUMENTS) || defined(GX_RENDER_CENSUS)
	static bool								CensusOn;
#else
	// Shipping build: every `if (CensusOn)` is dead code and the census costs nothing (+0.11 ms otherwise).
	// A measurement build defines GX_RENDER_CENSUS (CXXFLAGS at configure) or is a GX_TEST_INSTRUMENTS build.
	static constexpr bool					CensusOn = false;
#endif
	static unsigned long long			Census[DX8C_N];
	static const char *const			CensusNames[DX8C_N];
	static long long Census_Now_Ns();
	static void Census_Light_Environment(LightEnvironmentClass* light_env);
	static void Census_Lock(unsigned flags)
	{
		if (CensusOn)
			++Census[(flags & D3DLOCK_DISCARD) ? DX8C_LK_DISC : (flags & D3DLOCK_NOOVERWRITE) ? DX8C_LK_NOOW : DX8C_LK_PLAIN];
	}
	// A world/view matrix that reached the device by another path (the shadow code's direct SetTransform) or a
	// device reset: the next wrapper send cannot be called "the same" as the one before it.
	static void Census_Forget_Transforms() { Forget_Transforms(); }
	static bool								CensusTransformValid[2];	// [0] world, [1] view

	// GeneralsX: skip re-sending state the device holds (DXVK dirties its constants on every SetMaterial/SetTransform).
	// GX_WRAPSKIP mask, read once: 1 lights, 2 material, 4 world/view matrix, 8 light inputs (needs 1); default 15.
	// Compares are against what the WRAPPER last sent: direct world/view matrix writes must call Forget_Transforms(),
	// and nothing may set material or lights directly (scripts/test/gx-wrapskip-lint.sh).
	static unsigned Wrap_Skip() { return WrapSkipMask; }	// a plain load: no static-init guard on these hot paths
	static unsigned Read_Wrap_Skip();
	static unsigned							WrapSkipMask;				// 0 (old path) until Do_Onetime_Device_Dependent_Inits
	// GeneralsX: GX_WRAPSKIP 8: bumped by EVERY write to
	// the wrapper's light state (render_state.Lights/LightEnable and their LIGHTn_CHANGED bits, CurrentDX8Lights/
	// LightEnables/LightValid): Set_Light, Set_DX8_Light, Set_Render_State, Invalidate_Cached_Render_States, Init and
	// Do_Onetime_Device_Dependent_Inits. Set_Light_Environment skips its per-light rebuild only while this has not moved
	// since its own last full pass (and its inputs are bit-identical). A new writer of that state must bump it.
	static unsigned							LightStateSerial;
	static void Forget_Transforms() { CensusTransformValid[0] = CensusTransformValid[1] = false; XformShadowValid[0] = XformShadowValid[1] = false; GxXformValid = 0; }
	static bool								XformShadowValid[2];		// DX8Transforms[WORLD/VIEW] is what the device holds
	static D3DMATERIAL8						MaterialShadow;				// the material last sent
	static bool								MaterialShadowValid;
	static unsigned							CensusLastFVF;				// the FVF the wrapper last sent; ~0 = unknown

	// Needed by shader class
	static bool						Get_Fog_Enable() { return FogEnable; }
	static D3DCOLOR				Get_Fog_Color() { return FogColor; }

	// Utilities
	static Vector4 Convert_Color(unsigned color);
	static unsigned int Convert_Color(const Vector4& color);
	static unsigned int Convert_Color(const Vector3& color, const float alpha);
	static void Clamp_Color(Vector4& color);
	static unsigned int Convert_Color_Clamp(const Vector4& color);

	static void			  Set_Alpha (const float alpha, unsigned int &color);

	static void _Enable_Triangle_Draw(bool enable) { _EnableTriangleDraw=enable; }
	static bool _Is_Triangle_Draw_Enabled() { return _EnableTriangleDraw; }

	/*
	** Additional swap chain interface
	**
	**		Use this interface to render to multiple windows (in windowed mode).
	**	To render to an additional window, the sequence of calls should look
	**	something like this:
	**
	**	DX8Wrapper::Set_Render_Target (swap_chain_ptr);
	**
	**	WW3D::Begin_Render (true, true, Vector3 (0, 0, 0));
	**	WW3D::Render (scene, camera, FALSE, FALSE);
	**	WW3D::End_Render ();
	**
	**	swap_chain_ptr->Present (nullptr, nullptr, nullptr, nullptr);
	**
	**	DX8Wrapper::Set_Render_Target ((IDirect3DSurface8 *)nullptr);
	**
	*/
	static IDirect3DSwapChain8 *	Create_Additional_Swap_Chain (HWND render_window);

	/*
	** Render target interface. If render target format is WW3D_FORMAT_UNKNOWN, current display format is used.
	*/
	static TextureClass *	Create_Render_Target (int width, int height, WW3DFormat format = WW3D_FORMAT_UNKNOWN);

	static void					Set_Render_Target (IDirect3DSurface8 *render_target, bool use_default_depth_buffer = false);
	static void					Set_Render_Target (IDirect3DSurface8* render_target, IDirect3DSurface8* dpeth_buffer);

	static void					Set_Render_Target (IDirect3DSwapChain8 *swap_chain);
	static bool					Is_Render_To_Texture() { return IsRenderToTexture; }

	// GeneralsX: Render-target nesting (pillarbox -> world -> script filter).
	// Get returns what is bound NOW, both AddRef'd (the caller releases): rt is CurrentRenderTarget, or
	// nullptr on the swapchain backbuffer; ds is the DEVICE's bound depth, never CurrentDepthBuffer, which
	// is null after a (surf,bool) bind. Set binds such a pair (rt nullptr = back to the backbuffer)
	// through Set_Render_Target, leaves Is_Render_To_Texture as it found it, and returns false, without
	// asserting, when the device refuses the pair (the bookkeeping then names rt: restore the old pair).
	static void					Get_Render_Target_Pair (IDirect3DSurface8 *&rt, IDirect3DSurface8 *&ds);
	static bool					Set_Render_Target_Pair (IDirect3DSurface8 *rt, IDirect3DSurface8 *ds);

	// Platform display size provider — set by SDL3 layer at init
	typedef bool (*DisplaySizeFunc)(int& outW, int& outH, float& outDensity);
	static void					Set_Display_Size_Provider(DisplaySizeFunc nativeSize, DisplaySizeFunc windowSize);
	static bool					GetNativeDisplaySize(int& outW, int& outH, float& outDensity);
	static bool					GetWindowSize(int& outW, int& outH, float& outDensity);

	// GeneralsX: Borderless fullscreen, WINDOWS ONLY. When set, a device that
	// is not windowed presents AS A WINDOW into a popup covering its monitor, at the desktop mode, with
	// the game resolution pillarboxed into it - the same model SDL3 fullscreen already uses on macOS
	// and Linux, where this flag is never read. Unset, a non-windowed device mode-sets exclusively,
	// exactly as before. W3DDisplay::init decides it: neither -win nor -fullscreen on the command line.
	// PUBLIC on purpose: its callers are free functions (dx8wrapper.cpp, W3DDisplay.cpp), not friends.
	static void Set_Borderless(bool on) { Borderless = on; }
	static bool Is_Borderless() { return Borderless; }

	// GeneralsX: Vsync for a WINDOWED present - which is every present on the SDL3
	// platforms, fullscreen included (_PresentParameters.Windowed). D3D8 copies a windowed back buffer to the
	// window immediately, and DXVK follows it (d3d8_d3d9_util.h: a windowed swapchain gets
	// D3DPRESENT_INTERVAL_IMMEDIATE, so Vulkan IMMEDIATE, so MoltenVK displaySyncEnabled = NO: tearing). D3D8's
	// own switch for a vblank-synced windowed present is D3DSWAPEFFECT_COPY_VSYNC, which DXVK maps to interval
	// ONE, i.e. FIFO. Before the device exists this only records the choice, which the present parameters
	// read; after, a change resets the device (the max-FPS cap can change in a match). W3DDisplay decides it
	// (macOS only, from the max-FPS cap). A d3d9.presentInterval in dxvk.conf still overrides both ways
	// (d3d9_swapchain.cpp).
	static void Set_Windowed_VSync(bool on);
	static bool Get_Windowed_VSync() { return WindowedVSync; }	///< GeneralsX: for the replay diagnostics block
	static unsigned int Get_FullScreen_Present_Interval();		///< GeneralsX: D3DPRESENT_INTERVAL_*, for the replay diagnostics block

	// Pillarbox: render game at chosen resolution, blit centered onto backbuffer
	static bool					Pillarbox_Get_Rect(int& x, int& y, int& w, int& h);
	static void					Pillarbox_Begin();
	static void					Pillarbox_End();
	static void					Pillarbox_Process_Resize();

	// for depth map support KJM V
	static void Create_Render_Target
	(
		int width,
		int height,
		WW3DFormat format,
		WW3DZFormat zformat,
		TextureClass** target,
		ZTextureClass** depth_buffer
	);
	static void					Set_Render_Target_With_Z (TextureClass * texture, ZTextureClass* ztexture=nullptr);

	static void Set_Shadow_Map(int idx, ZTextureClass* ztex) { Shadow_Map[idx]=ztex; }
	static ZTextureClass* Get_Shadow_Map(int idx) { return Shadow_Map[idx]; }
	// for depth map support KJM ^

	// shader system updates KJM v
	static void Apply_Default_State();

	static void Set_Vertex_Shader(DWORD vertex_shader);
	static void Set_Pixel_Shader(DWORD pixel_shader);

	static void Set_Vertex_Shader_Constant(int reg, const void* data, int count);
	static void Set_Pixel_Shader_Constant(int reg, const void* data, int count);

	static DWORD Get_Vertex_Processing_Behavior() { return Vertex_Processing_Behavior; }

	// Needed by scene lighting class
	static void						Set_Ambient(const Vector3& color);
	static const Vector3&		Get_Ambient() { return Ambient_Color; }
	// shader system updates KJM ^




	// d3d8 ring spike: any device access through here waits until the ring is replayed (DX8Ring above)
	// GeneralsX: an unclassified access while the call log is on is RAW_ACCESS (its caller logged);
	// DX8CALL uses _Get_D3D_Device8_Call (the body as it was).
	static IDirect3DDevice8* _Get_D3D_Device8() { if (GxLogOn && GxLogScope == 0) Gx_Log_Raw_Access(GX_LOG_CALLER()); if (DX8Ring::Pending || DX8Ring::Recording) DX8Ring::Fallback(); return D3DDevice; }
	static IDirect3DDevice8* _Get_D3D_Device8_Call() { if (DX8Ring::Pending || DX8Ring::Recording) DX8Ring::Fallback(); return D3DDevice; }
	static IDirect3D8* _Get_D3D8() { return D3DInterface; }
	static IDirect3DDevice8* D3DDevice_For_Ring() { return D3DDevice; }	// DX8Ring only: no drain
	typedef void (*DeviceCallFn)(IDirect3DDevice8 *dev, void *ctx);
	static void Device_Call(DeviceCallFn fn, void *ctx);	// fn(dev, ctx) on the device's thread, in stream order
	// GeneralsX: GXCAP (GameEngineDevice GXCap.h): when set, Set_Render_Device hands the new device
	// to this hook right after CreateDevice and keeps what it returns (the recording proxy). nullptr = today's path.
	typedef IDirect3DDevice8 *(*DeviceWrapHook)(IDirect3DDevice8 *device, UINT adapter, D3DDEVTYPE type, DWORD behavior,
		const D3DPRESENT_PARAMETERS *pp);
	static DeviceWrapHook Device_Wrap_Hook;
	// GeneralsX: L1 native device: called once in Init right
	// after Direct3DCreate8; a non-null result replaces D3DInterface for the whole session (the native proxy forwards every
	// adapter query to DXVK's interface and decides in CreateDevice). nullptr = today's path.
	typedef IDirect3D8 *(*InterfaceWrapHook)(IDirect3D8 *dxvk);
	static InterfaceWrapHook Interface_Wrap_Hook;
	/// Returns the display format - added by TR for video playback - not part of W3D
	static WW3DFormat	getBackBufferFormat();
	static bool Reset_Device(bool reload_assets=true);

	static const DX8Caps*	Get_Current_Caps() { WWASSERT(CurrentCaps); return CurrentCaps; }

	static bool Registry_Save_Render_Device( const char * sub_key );
	static bool Registry_Load_Render_Device( const char * sub_key, bool resize_window );

	static const char* Get_DX8_Render_State_Name(D3DRENDERSTATETYPE state);
	static const char* Get_DX8_Texture_Stage_State_Name(D3DTEXTURESTAGESTATETYPE state);
	static unsigned Get_DX8_Render_State(D3DRENDERSTATETYPE state) { return RenderStates[state]; }

	// Names of the specific values of render states and texture stage states
	static void Get_DX8_Texture_Stage_State_Value_Name(StringClass& name, D3DTEXTURESTAGESTATETYPE state, unsigned value);
	static void Get_DX8_Render_State_Value_Name(StringClass& name, D3DRENDERSTATETYPE state, unsigned value);

	static const char* Get_DX8_Texture_Address_Name(unsigned value);
	static const char* Get_DX8_Texture_Filter_Name(unsigned value);
	static const char* Get_DX8_Texture_Arg_Name(unsigned value);
	static const char* Get_DX8_Texture_Op_Name(unsigned value);
	static const char* Get_DX8_Texture_Transform_Flag_Name(unsigned value);
	static const char* Get_DX8_ZBuffer_Type_Name(unsigned value);
	static const char* Get_DX8_Fill_Mode_Name(unsigned value);
	static const char* Get_DX8_Shade_Mode_Name(unsigned value);
	static const char* Get_DX8_Blend_Name(unsigned value);
	static const char* Get_DX8_Cull_Mode_Name(unsigned value);
	static const char* Get_DX8_Cmp_Func_Name(unsigned value);
	static const char* Get_DX8_Fog_Mode_Name(unsigned value);
	static const char* Get_DX8_Stencil_Op_Name(unsigned value);
	static const char* Get_DX8_Material_Source_Name(unsigned value);
	static const char* Get_DX8_Vertex_Blend_Flag_Name(unsigned value);
	static const char* Get_DX8_Patch_Edge_Style_Name(unsigned value);
	static const char* Get_DX8_Debug_Monitor_Token_Name(unsigned value);
	static const char* Get_DX8_Blend_Op_Name(unsigned value);

	static void Invalidate_Cached_Render_States();
	static void Set_Transform_Dirty();

	static void Set_Draw_Polygon_Low_Bound_Limit(unsigned n) { DrawPolygonLowBoundLimit=n; }
	static unsigned Get_Draw_Polygon_Low_Bound_Limit() { return DrawPolygonLowBoundLimit; }	// GeneralsX: seat (GxL2Dry)

	// GeneralsX: a read-only view of the wrapper's caches for the L2 state-check (GXRender/Scene):
	// what the wrapper believes the device holds.
	struct GxShadowView
	{
		const unsigned *renderStates;							// [256], 0x12345678 = unknown (Invalidate_Cached_Render_States)
		const unsigned (*textureStageStates)[32];				// [MAX_TEXTURE_STAGES][32], the same sentinel
		IDirect3DBaseTexture8 *const *textures;					// [MAX_TEXTURE_STAGES]
		const D3DLIGHT8 *lights;								// [4]
		const bool *lightEnables, *lightValid;					// [4]
		const Matrix4x4 *transforms;							// [D3DTS_WORLD+1]: WORLD and VIEW as last sent
		unsigned fvf;											// Vertex_Shader as last sent
		const void *vb, *ib;									// render_state stream 0 / index buffer as applied
		unsigned indexBaseOffset, vbaOffset;					// the base vertex DX8 receives = indexBaseOffset + vbaOffset (:3771)
		const D3DMATERIAL8 *material; bool materialValid;		// GxMaterialSent (below)
		const Matrix4x4 *projection; bool projectionValid;		// GxProjSent (below)
		const Matrix4x4 *texm; unsigned texmValid;				// [8] the texture matrices last sent; bit n: texm[n] is known
		unsigned xformValid;									// bit 0: transforms[WORLD] is what the device holds, bit 1: [VIEW]
		unsigned ibaOffset;										// render_state.iba_offset: Draw adds it to start_index
		const Matrix4x4 *viewSent; bool viewSentValid;			// GxViewSent (below)
	};
	// Kept only while GxCaptureOn (GX_L2_CHECK, set before the first frame): every material sent (independent of GX_WRAPSKIP's
	// MaterialShadow), every projection sent (Set_Transform and Set_Projection_Transform_With_Z_Bias send locals), and every texture
	// matrix sent (_Set_DX8_Transform and Set_Transform's default path) in an array of its own: Set_Transform_Dirty and
	// Invalidate_Cached_Render_States zero DX8Transforms while the device keeps the matrices.
	static bool								GxCaptureOn;
	static D3DMATERIAL8						GxMaterialSent;
	static bool								GxMaterialSentValid;
	static Matrix4x4						GxProjSent;
	static bool								GxProjSentValid;
	static Matrix4x4						GxTexmSent[8];
	static unsigned							GxTexmValid;
	// DX8Transforms[WORLD/VIEW] as last sent (bit 0 / bit 1), cleared by Forget_Transforms - which Invalidate_Cached_Render_States,
	// Set_Transform_Dirty and every direct world/view writer call - since those zero or bypass the cache while the device keeps
	// its matrices (lease c3/c5's C9 VIEW "diff" was a zeroed cache vs a real device view).
	static unsigned							GxXformValid;
	// The VIEW last sent, never zeroed by the cache resets: no code sets the view behind the wrapper (the direct SetTransform calls are
	// all WORLD), so it stays the device's view until a Reset. C9 checks that premise (VIEWSENT); the L2 check's
	// capture uses it where a zeroed cache would read as unknown.
	static Matrix4x4						GxViewSent;
	static bool								GxViewSentValid;
	// (A35, /§2.6): the motion bridge records a
	// moved draw with the viewport it ran with (Set_Viewport has no cache: kept only while GxCaptureOn, like the projection above),
	// reads the wrapper's applied world/view and buffers, and forces its own constant upload past the cache (W3DTreeBuffer writes
	// c4-c12 straight to the device, so the cache can claim values the device no longer holds).
	static D3DVIEWPORT8						GxViewportSent;
	static bool								GxViewportSentValid;
	static bool								GxViewportCaptureOn;	// set only by the bridge between its Begin and End
	static const RenderStateStruct& Gx_Render_State() { return render_state; }
	static void Gx_Forget_VS_Constants(int reg, int count) { memset(&Vertex_Shader_Constants[reg], 0xFF, sizeof(Vector4) * count); }
	// A device Reset restores the device's default material / projection / texture matrices: the captured ones are unknown
	// until sent again. WORLD/VIEW go with Invalidate_Cached_Render_States, which Reset_Device calls.
	static void Gx_Capture_Forget() { GxMaterialSentValid = GxProjSentValid = GxViewSentValid = GxViewportSentValid = false; GxTexmValid = 0; }
	// Device truth: per render / texture-stage state, the value the DEVICE
	// holds and whether it is known. Seeded by reading the device (Gx_Truth_Enable; again after a Reset, which restores DXVK's
	// ResetState values); every wrapper write that reaches the device updates it; Invalidate_Cached_Render_States leaves it alone
	// (it only empties the cache); a raw writer behind the wrapper records the value it wrote (GxRawRS / GxRawTSS below, or
	// Gx_Truth_Raw_*); a failed Reset forgets everything. Off (the default), the updates are one branch each.
	static bool								GxTruthOn;
	static unsigned							GxTruthRS[256];
	static bool								GxTruthRSKnown[256];
	static unsigned							GxTruthTSS[MAX_TEXTURE_STAGES][32];
	static bool								GxTruthTSSKnown[MAX_TEXTURE_STAGES][32];
	static void Gx_Truth_Enable();
	static void Gx_Truth_Reseed();
	static void Gx_Truth_Forget()	// a failed Reset: nothing about the device is known any more
	{
		if (!GxTruthOn) return;
		memset(GxTruthRSKnown, 0, sizeof(GxTruthRSKnown));
		memset(GxTruthTSSKnown, 0, sizeof(GxTruthTSSKnown));
	}
	static unsigned long long				GxTruthRawCalls;	// raw writes recorded while GxTruthOn (C9 prints them per frame)
	static void Gx_Truth_Raw_RS(unsigned state, unsigned value)
	{
		if (GxTruthOn && state < 256) { GxTruthRS[state] = value; GxTruthRSKnown[state] = true; ++GxTruthRawCalls; }
	}
	static void Gx_Truth_Raw_TSS(unsigned stage, unsigned type, unsigned value)
	{
		if (GxTruthOn && stage < MAX_TEXTURE_STAGES) { GxTruthTSS[stage][type] = value; GxTruthTSSKnown[stage][type] = true; ++GxTruthRawCalls; }
	}
	// GX_SEAT_MUTANT=2 only (seat): the base vertex the wrapper believes, moved without the setter - nothing is sent.
	// DX8Wrapper's pending change flags minus the two identity descriptors (0 right after an Apply): GxDrawRun::Truth.wrapperDirty.
	static unsigned Gx_Pending_Changes() { return render_state_changed & ~((unsigned)WORLD_IDENTITY | (unsigned)VIEW_IDENTITY); }	// Apply keeps the identity flags (cache descriptors, not pending changes)
	static void Gx_Poke_Index_Base(unsigned short offset) { render_state.index_base_offset = offset; }
	static GxShadowView Gx_Shadows()
	{
		GxShadowView v = { RenderStates, TextureStageStates, Textures, CurrentDX8Lights, CurrentDX8LightEnables, CurrentDX8LightValid,
			DX8Transforms, Vertex_Shader, render_state.vertex_buffers[0], render_state.index_buffer, render_state.index_base_offset,
			render_state.vba_offset, &GxMaterialSent, GxMaterialSentValid, &GxProjSent, GxProjSentValid, GxTexmSent, GxTexmValid,
			GxXformValid, render_state.iba_offset, &GxViewSent, GxViewSentValid };
		return v;
	}

	// GeneralsX: the `=2` call log (gxcalllog.h). The check
	// (GXRender/Scene) sets GxLogOn around a modelled flush only; off (the default) every hook is one predicted-false test of it.
	static bool								GxLogOn;
	static gxlog::Log *						GxLog;			// where records go (owned by the check)
	static int								GxLogParent;	// > 0 inside a logged high-level entry or an Apply: low-level records are NESTED
	static int								GxLogScope;		// > 0 inside a known device access (READ / RESOURCE / modelled): no RAW_ACCESS
	static uint8_t							GxLogBracket, GxLogStage;	// Apply's current step (gxlog::Bracket) and texture stage
	static unsigned							GxLogEntry;		// the newest classic list index (set by the ENTRY marker)
	static int								GxLogDrawRec;	// the open DRAW record (-1 none): Draw_Sorting_IB_VB patches its payload
	static void Gx_Log(unsigned op, unsigned a, unsigned b, const void *payload, unsigned n, unsigned flags);
	static void Gx_Log_Ptr(unsigned op, unsigned a, unsigned b, const void *ptr, unsigned flags);	// payload: the pointer as a u64
	static void Gx_Log_Raw_Access(const void *caller);
	static void Gx_Log_Snapshot(gxlog::Snapshot &out);
	static void Gx_Log_Render_State(unsigned op, unsigned a, const RenderStateStruct &state);	// payload: gxlog::Logical of state
	static void Gx_Log_Vertex_Buffer(unsigned op, unsigned a, unsigned b, unsigned type, const VertexBufferClass *vb);	// gxlog::VbDesc
	static void Gx_Log_Index_Buffer(unsigned op, unsigned a, unsigned b, unsigned type, const IndexBufferClass *ib);	// gxlog::IbDesc
	static void Gx_Log_Light_Environment(LightEnvironmentClass *light_env);
	static void Gx_Log_Draw(unsigned primitive_type, unsigned start_index, unsigned polygon_count, unsigned min_vertex_index,
		unsigned vertex_count);
	static void Gx_Log_Draw_Sort(IDirect3DVertexBuffer8 *vb, IDirect3DIndexBuffer8 *ib, unsigned fvf_size, unsigned fvf,
		unsigned vb_offset, unsigned ib_offset);
	static void Gx_Log_Apply(bool begin);	// APPLY_BEGIN + raise GxLogParent / brackets reset + lower + APPLY_END
	static void Gx_Log_Body_Open(unsigned op, bool parent);
	static void Gx_Log_Body_Close(bool parent);
	// A known device access for one body, early returns included: logs op when it opens (RESOURCE / READ always; a high-level op such
	// as INVALIDATE only at GxLogParent 0), raises GxLogScope, and GxLogParent when asked. Off: the constructor's one test of GxLogOn.
	class GxLogBody
	{
	public:
		explicit GxLogBody(unsigned op, bool parent = false) : On(GxLogOn), Parent(parent) { if (On) Gx_Log_Body_Open(op, parent); }
		~GxLogBody() { if (On) Gx_Log_Body_Close(Parent); }
	private:
		bool On, Parent;
	};

protected:

	static bool	Create_Device();
	static void Release_Device();

	static void Reset_Statistics();
	static void Enumerate_Devices();
	static void Set_Default_Global_Render_States();

	/*
	** Device Selection Code.
	** For backward compatibility, the public interface for these functions is in the ww3d.
	** header file.  These functions are protected so that we aren't exposing two interfaces.
	*/
	static bool Set_Any_Render_Device();
	static bool	Set_Render_Device(const char * dev_name,int width=-1,int height=-1,int bits=-1,int windowed=-1,bool resize_window=false);
	static bool	Set_Render_Device(int dev=-1,int resx=-1,int resy=-1,int bits=-1,int windowed=-1,bool resize_window = false, bool reset_device = false, bool restore_assets=true);
	static bool Set_Next_Render_Device();
	static bool Toggle_Windowed();

	static int	Get_Render_Device_Count();
	static int	Get_Render_Device();
	static const RenderDeviceDescClass & Get_Render_Device_Desc(int deviceidx);
	static const char * Get_Render_Device_Name(int device_index);
	static bool Set_Device_Resolution(int width=-1,int height=-1,int bits=-1,int windowed=-1, bool resize_window=false);
	static void Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed);
	static void Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed);
	static int	Get_Device_Resolution_Width() { return ResolutionWidth; }
	static int	Get_Device_Resolution_Height() { return ResolutionHeight; }

	static bool Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth);
	static bool Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth);
	static bool Is_Windowed() { return IsWindowed; }

	static void	Set_Texture_Bitdepth(int depth)	{ WWASSERT(depth==16 || depth==32); TextureBitDepth = depth; }
	static int	Get_Texture_Bitdepth()			{ return TextureBitDepth; }

	static void Set_MSAA_Mode(D3DMULTISAMPLE_TYPE mode) { MultiSampleAntiAliasing = mode; }
	static D3DMULTISAMPLE_TYPE Get_MSAA_Mode() { return MultiSampleAntiAliasing; }

	static void	Set_Swap_Interval(int swap);
	static int	Get_Swap_Interval();
	static void Set_Polygon_Mode(int mode);

	/*
	** Internal functions
	*/
	static void Resize_And_Position_Window();
	static bool Find_Color_And_Z_Mode(int resx,int resy,int bitdepth,D3DFORMAT * set_colorbuffer,D3DFORMAT * set_backbuffer, D3DFORMAT * set_zmode);
	static bool Find_Color_Mode(D3DFORMAT colorbuffer, int resx, int resy, UINT *mode);
	static bool Find_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT *zmode);
	static bool Test_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT zmode);
	static void Compute_Caps(WW3DFormat display_format);

	/*
	** Protected Member Variables
	*/

	static DX8_CleanupHook *m_pCleanupHook;

	static RenderStateStruct			render_state;
	static unsigned						render_state_changed;
	// GeneralsX: fighter19 pattern Matrix4x4 not D3DMATRIX
	static Matrix4x4						DX8Transforms[D3DTS_WORLD+1];

	static bool								IsInitted;
	static bool								IsDeviceLost;
	static void *							Hwnd;
	static unsigned						_MainThreadID;

	static bool								_EnableTriangleDraw;

	static int								CurRenderDevice;
	static int								ResolutionWidth;
	static int								ResolutionHeight;
	static int								BitDepth;
	static int								TextureBitDepth;
	static bool								IsWindowed;
	static bool								Borderless;
	static bool								WindowedVSync;
	static D3DFORMAT					DisplayFormat;
	static D3DMULTISAMPLE_TYPE	MultiSampleAntiAliasing;

	static D3DMATRIX						old_world;
	static D3DMATRIX						old_view;
	static D3DMATRIX						old_prj;

	// shader system updates KJM v
	static DWORD							Vertex_Shader;
	static DWORD							Pixel_Shader;

	static Vector4							Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
	static Vector4							Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];

	static LightEnvironmentClass*		Light_Environment;
	static RenderInfoClass*				Render_Info;

	static DWORD							Vertex_Processing_Behavior;

	static ZTextureClass*				Shadow_Map[MAX_SHADOW_MAPS];

	static Vector3							Ambient_Color;
	// shader system updates KJM ^

	static bool								world_identity;
	static unsigned						RenderStates[256];
	static unsigned						TextureStageStates[MAX_TEXTURE_STAGES][32];
	static IDirect3DBaseTexture8 *	Textures[MAX_TEXTURE_STAGES];

	// These fog settings are constant for all objects in a given scene,
	// unlike the matching renderstates which vary based on shader settings.
	static bool								FogEnable;
	static D3DCOLOR						FogColor;

	static DX8FrameStatistics			FrameStatistics;
	static bool								CurrentDX8LightEnables[4];
	// GeneralsX: Shadow of the D3DLIGHT8 last sent per slot, valid only while
	// CurrentDX8LightValid[slot]; lets Set_DX8_Light skip re-sending an identical enabled light.
	static D3DLIGHT8						CurrentDX8Lights[4];
	static bool								CurrentDX8LightValid[4];

	static unsigned long FrameCount;
	static unsigned long long PresentCount;

	static DX8Caps*						CurrentCaps;

	static D3DADAPTER_IDENTIFIER8		CurrentAdapterIdentifier;

	static IDirect3D8 *					D3DInterface;			//d3d8;
	static IDirect3DDevice8 *			D3DDevice;				//d3ddevice8;

	static IDirect3DSurface8 *			CurrentRenderTarget;
	static IDirect3DSurface8 *			CurrentDepthBuffer;
	static IDirect3DSurface8 *			DefaultRenderTarget;
	static IDirect3DSurface8 *			DefaultDepthBuffer;

	static unsigned							DrawPolygonLowBoundLimit;

	static bool								IsRenderToTexture;

	// Display size providers (set by SDL3 layer, called by pillarbox)
	static DisplaySizeFunc				s_getNativeDisplaySize;
	static DisplaySizeFunc				s_getWindowSize;

	// Pillarbox state
	static bool								s_pillarboxEnabled;
	static bool								s_pillarboxActive;
	static int								s_bbW, s_bbH;
	static int								s_dstX, s_dstY, s_dstW, s_dstH;
	static float							s_pixelDensity;
	static IDirect3DTexture8*			s_offscreenTex;
	static IDirect3DSurface8*			s_offscreenSurf;
	static IDirect3DSurface8*			s_depthSurf;
	static IDirect3DSurface8*			s_savedBackbuffer;
	static IDirect3DSurface8*			s_savedDepth;

	static bool								Pillarbox_Setup(int gameW, int gameH);
	static void								Pillarbox_Cleanup();

	static int								ZBias;
	static float							ZNear;
	static float							ZFar;
	// GeneralsX:
	// Changed from D3DMATRIX to Matrix4x4 - fighter19 pattern
	static Matrix4x4					ProjectionMatrix;

	friend void DX8_Assert();
	friend class WW3D;
	friend class DX8IndexBufferClass;
	friend class DX8VertexBufferClass;
};

// shader system updates KJM v
// GeneralsX: a render / texture-stage state written behind the wrapper's cache. Records the value in the
// device truth (DX8Wrapper::GxTruth*) and sends exactly what the raw call sent. Every audited raw writer uses these;
// the check (GXRender/Scene) compares the truth with the device and shows any writer the audit missed.
// Off: one branch per call.
inline HRESULT GxRawRS(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state, DWORD value)
{
	DX8Wrapper::Gx_Truth_Raw_RS(state, value);
	return dev->SetRenderState(state, value);
}
inline HRESULT GxRawTSS(IDirect3DDevice8 *dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	DX8Wrapper::Gx_Truth_Raw_TSS(stage, type, value);
	return dev->SetTextureStageState(stage, type, value);
}

WWINLINE void DX8Wrapper::Set_Vertex_Shader(DWORD vertex_shader)
{
#if 0 //(gth) some code is bypassing this accessor function so we can't count on this variable...
	// may be incorrect if shaders are created and destroyed dynamically
	if (Vertex_Shader==vertex_shader) return;
#endif

	if (CensusOn) {	// [GXCENSUS] its own last value: Vertex_Shader is never reset by Invalidate_Cached_Render_States
		++Census[DX8C_FVF_DEV];
		if (CensusLastFVF==vertex_shader) ++Census[DX8C_FVF_SAME];
		CensusLastFVF=vertex_shader;
	}
	Vertex_Shader=vertex_shader;
	if (DX8Ring::Recording) { DX8Ring::Rec_Vertex_Shader(Vertex_Shader); return; }
	DX8CALL(SetVertexShader(Vertex_Shader));
}

WWINLINE void DX8Wrapper::Set_Pixel_Shader(DWORD pixel_shader)
{
	// may be incorrect if shaders are created and destroyed dynamically
	if (Pixel_Shader==pixel_shader) return;

	Pixel_Shader=pixel_shader;
	if (DX8Ring::Recording) { DX8Ring::Rec_Pixel_Shader(Pixel_Shader); return; }
	DX8CALL(SetPixelShader(Pixel_Shader));
}

WWINLINE void DX8Wrapper::Set_Vertex_Shader_Constant(int reg, const void* data, int count)
{
	int memsize=sizeof(Vector4)*count;

	// may be incorrect if shaders are created and destroyed dynamically
	if (memcmp(data, &Vertex_Shader_Constants[reg],memsize)==0) return;

	memcpy(&Vertex_Shader_Constants[reg],data,memsize);
	if (DX8Ring::Recording) { DX8Ring::Rec_Vertex_Shader_Constant(reg,data,count); return; }
	DX8CALL(SetVertexShaderConstant(reg,data,count));
}

WWINLINE void DX8Wrapper::Set_Pixel_Shader_Constant(int reg, const void* data, int count)
{
	int memsize=sizeof(Vector4)*count;

	// may be incorrect if shaders are created and destroyed dynamically
	if (memcmp(data, &Pixel_Shader_Constants[reg],memsize)==0) return;

	memcpy(&Pixel_Shader_Constants[reg],data,memsize);
	if (DX8Ring::Recording) { DX8Ring::Rec_Pixel_Shader_Constant(reg,data,count); return; }
	DX8CALL(SetPixelShaderConstant(reg,data,count));
}
// shader system updates KJM ^
// GeneralsX: fighter19 pattern Matrix4x4 parameter not D3DMATRIX
WWINLINE void DX8Wrapper::_Set_DX8_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m)
{
	WWASSERT(transform<=D3DTS_WORLD);
#if 0 // (gth) this optimization is breaking generals because they set the transform behind our backs.
	if (mtx!=DX8Transforms[transform])
#endif
	{
		if ((transform==D3DTS_WORLD || transform==D3DTS_VIEW) && (Wrap_Skip() & 4u)) {
			// GX_WRAPSKIP 4: the device already holds this world/view matrix (see Wrap_Skip)
			const int v = (transform==D3DTS_VIEW);
			if (XformShadowValid[v] && memcmp(&DX8Transforms[transform], &m, sizeof(Matrix4x4))==0) {
				DX8_CENSUS(DX8C_SKIP_XFORM);
				if (GxLogOn) Gx_Log(gxlog::DX8_XFORM, transform, 0, &m, sizeof(Matrix4x4), 0);	// skipped
				return;
			}
			XformShadowValid[v] = true;
		}
		if (CensusOn && (transform==D3DTS_WORLD || transform==D3DTS_VIEW)) {	// [GXCENSUS] roadmap 2d
			const int v = (transform==D3DTS_VIEW);
			++Census[v ? DX8C_VIEW_DEV : DX8C_WORLD_DEV];
			if (CensusTransformValid[v] && memcmp(&DX8Transforms[transform], &m, sizeof(Matrix4x4))==0)
				++Census[v ? DX8C_VIEW_SAME : DX8C_WORLD_SAME];
			CensusTransformValid[v] = true;
		}
		DX8Transforms[transform]=m;
		if (GxLogOn) Gx_Log(gxlog::DX8_XFORM, transform, 0, &m, sizeof(Matrix4x4), gxlog::SENT);
		if (GxCaptureOn) {	// GxShadowView
			if (transform>=D3DTS_TEXTURE0 && transform<=D3DTS_TEXTURE7) {
				GxTexmSent[transform-D3DTS_TEXTURE0]=m;
				GxTexmValid|=1u<<(transform-D3DTS_TEXTURE0);
			}
			else if (transform==D3DTS_WORLD || transform==D3DTS_VIEW) GxXformValid|=transform==D3DTS_VIEW ? 2u : 1u;
			if (transform==D3DTS_VIEW) { GxViewSent=m; GxViewSentValid=true; }
		}
		SNAPSHOT_SAY(("DX8 - SetTransform %d [%f,%f,%f,%f][%f,%f,%f,%f][%f,%f,%f,%f][%f,%f,%f,%f]",
			transform,
			m[0][0],m[0][1],m[0][2],m[0][3],
			m[1][0],m[1][1],m[1][2],m[1][3],
			m[2][0],m[2][1],m[2][2],m[2][3],
			m[3][0],m[3][1],m[3][2],m[3][3]));
		DX8_RECORD_MATRIX_CHANGE();
		if (DX8Ring::Recording) { DX8Ring::Rec_Transform(transform,(const D3DMATRIX*)&m); return; }
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m));
	}
}

// GeneralsX: Matrix4x4 for GLM compatibility (fighter19 pattern)
WWINLINE void DX8Wrapper::_Get_DX8_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m)
{
	DX8CALL(GetTransform(transform,(D3DMATRIX*)&m));
}

// ----------------------------------------------------------------------------
//
// Set the index offset for the current index buffer
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_Index_Buffer_Index_Offset(unsigned offset)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_INDEX_OFFSET, offset, 0, nullptr, 0, 0);
	if (render_state.index_base_offset==offset) return;
	render_state.index_base_offset=offset;
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
// Set the fog settings. This function should be used, rather than setting the
// appropriate renderstates directly, because the shader sets some of the
// renderstates on a per-mesh / per-pass basis depending on global fog states
// (stored in the wrapper) as well as the shader settings.
// This function should be called rarely - once per scene would be appropriate.
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_Fog(bool enable, const Vector3 &color, float start, float end)
{
	// Set global states
	FogEnable = enable;
	FogColor = Convert_Color(color,0.0f);

	// Invalidate the current shader (since the renderstates set by the shader
	// depend on the global fog settings as well as the actual shader settings)
	ShaderClass::Invalidate();

	// Set renderstates which are not affected by the shader
	Set_DX8_Render_State(D3DRS_FOGSTART, std::bit_cast<DWORD>(start));
	Set_DX8_Render_State(D3DRS_FOGEND,   std::bit_cast<DWORD>(end));
}


WWINLINE void DX8Wrapper::Set_Ambient(const Vector3& color)
{
	Ambient_Color=color;
	Set_DX8_Render_State(D3DRS_AMBIENT, DX8Wrapper::Convert_Color(color,0.0f));
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer to be used in the subsequent render calls. If there was
// a vertex buffer being used earlier, release the reference to it. Passing
// nullptr just will release the vertex buffer.
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_DX8_Material(const D3DMATERIAL8* mat)
{
	WWASSERT(mat);
	if (Wrap_Skip() & 2u) {	// GX_WRAPSKIP 2: the device already holds this material (see Wrap_Skip)
		if (MaterialShadowValid && memcmp(&MaterialShadow, mat, sizeof(D3DMATERIAL8))==0) {
			DX8_CENSUS(DX8C_SKIP_MAT);
			if (GxLogOn) Gx_Log(gxlog::DX8_MATERIAL, 0, 0, mat, sizeof(D3DMATERIAL8), 0);	// skipped
			return;
		}
		MaterialShadow = *mat;
		MaterialShadowValid = true;
	}
	if (GxLogOn) Gx_Log(gxlog::DX8_MATERIAL, 0, 0, mat, sizeof(D3DMATERIAL8), gxlog::SENT);
	DX8_RECORD_MATERIAL_CHANGE();
	if (GxCaptureOn) { GxMaterialSent = *mat; GxMaterialSentValid = true; }	// GxShadowView
	if (CensusOn) {	// [GXCENSUS] the wrapper keeps no material cache (Set_Material's is commented out)
		static D3DMATERIAL8 s_last;
		static bool s_lastValid = false;
		++Census[DX8C_MAT_DEV];
		if (s_lastValid && memcmp(&s_last, mat, sizeof(D3DMATERIAL8))==0) ++Census[DX8C_MAT_SAME];
		s_last = *mat;
		s_lastValid = true;
	}
	SNAPSHOT_SAY(("DX8 - SetMaterial"));
	if (DX8Ring::Recording) { DX8Ring::Rec_Material(mat); return; }
	DX8CALL(SetMaterial(mat));
}

WWINLINE void DX8Wrapper::Set_DX8_Light(int index, D3DLIGHT8* light)
{
	++LightStateSerial;	// GX_WRAPSKIP 8 (see LightStateSerial)
	if (light) {
		// GeneralsX: Every mesh draw re-applies the same 3-4 scene lights, and
		// only the disable path was cached. Skip SetLight+LightEnable(TRUE) when this slot is already enabled
		// with byte-identical parameters; the device state is then exactly what the two calls would set.
		if (CurrentDX8LightEnables[index] && CurrentDX8LightValid[index]
			&& memcmp(&CurrentDX8Lights[index], light, sizeof(D3DLIGHT8)) == 0) {
			if (GxLogOn) Gx_Log(gxlog::DX8_LIGHT, index, 1, light, sizeof(D3DLIGHT8), 0);	// skipped
			return;
		}
		if (GxLogOn) Gx_Log(gxlog::DX8_LIGHT, index, 1, light, sizeof(D3DLIGHT8), gxlog::SENT);
		DX8_CENSUS(DX8C_LIGHT_DEV);
		DX8_RECORD_LIGHT_CHANGE();
		if (DX8Ring::Recording) {
			DX8Ring::Rec_Light(index,light);
			DX8Ring::Rec_Light_Enable(index,TRUE);
		} else {
			DX8CALL(SetLight(index,light));
			DX8CALL(LightEnable(index,TRUE));
		}
		CurrentDX8LightEnables[index]=true;
		CurrentDX8Lights[index]=*light;
		CurrentDX8LightValid[index]=true;
		SNAPSHOT_SAY(("DX8 - SetLight %d",index));
	}
	else if (CurrentDX8LightEnables[index]) {
		if (GxLogOn) Gx_Log(gxlog::DX8_LIGHT, index, 0, nullptr, 0, gxlog::SENT);
		DX8_CENSUS(DX8C_LIGHT_DEV);
		DX8_RECORD_LIGHT_CHANGE();
		CurrentDX8LightEnables[index]=false;
		if (DX8Ring::Recording) { DX8Ring::Rec_Light_Enable(index,FALSE); }
		else { DX8CALL(LightEnable(index,FALSE)); }
		SNAPSHOT_SAY(("DX8 - DisableLight %d",index));
	}
	else if (GxLogOn) Gx_Log(gxlog::DX8_LIGHT, index, 0, nullptr, 0, 0);	// already off
}

WWINLINE void DX8Wrapper::Set_DX8_Render_State(D3DRENDERSTATETYPE state, unsigned value)
{
	// Can't monitor state changes because setShader call to GERD may change the states!
	if (RenderStates[state]==value) {
		if (GxLogOn) Gx_Log(gxlog::RS, state, value, nullptr, 0, 0);	// skipped
		return;
	}

#ifdef MESH_RENDER_SNAPSHOT_ENABLED
	if (WW3D::Is_Snapshot_Activated()) {
		StringClass value_name(0,true);
		Get_DX8_Render_State_Value_Name(value_name,state,value);
		SNAPSHOT_SAY(("DX8 - SetRenderState(state: %s, value: %s)",
			Get_DX8_Render_State_Name(state),
			value_name.str()));
	}
#endif

	RenderStates[state]=value;
	if (GxLogOn) Gx_Log(gxlog::RS, state, value, nullptr, 0, gxlog::SENT);
	if (GxTruthOn) { GxTruthRS[state]=value; GxTruthRSKnown[state]=true; }	// device truth
	if (DX8Ring::Recording) { DX8Ring::Rec_Render_State(state,value); }
	else { DX8CALL(SetRenderState( state, value )); }
	DX8_RECORD_RENDER_STATE_CHANGE();
	DX8_CENSUS(DX8C_RS_DEV);
}

WWINLINE void DX8Wrapper::Set_DX8_Clip_Plane(DWORD Index, CONST float* pPlane)
{
	if (DX8Ring::Recording) { DX8Ring::Rec_Clip_Plane(Index,pPlane); return; }
	DX8CALL(SetClipPlane( Index, pPlane ));
}

WWINLINE void DX8Wrapper::Set_DX8_Texture_Stage_State(unsigned stage, D3DTEXTURESTAGESTATETYPE state, unsigned value)
{
  	if (stage >= MAX_TEXTURE_STAGES)
  	{	if (GxLogOn) Gx_Log(gxlog::TSS, stage<<16|(unsigned)state, value, nullptr, 0, gxlog::SENT);
  		if (DX8Ring::Recording) { DX8Ring::Rec_Texture_Stage_State(stage,state,value); return; }
  		DX8CALL(SetTextureStageState( stage, state, value ));
  		return;
  	}

	// Can't monitor state changes because setShader call to GERD may change the states!
	if (TextureStageStates[stage][(unsigned int)state]==value) {
		if (GxLogOn) Gx_Log(gxlog::TSS, stage<<16|(unsigned)state, value, nullptr, 0, 0);	// skipped
		return;
	}
#ifdef MESH_RENDER_SNAPSHOT_ENABLED
	if (WW3D::Is_Snapshot_Activated()) {
		StringClass value_name(0,true);
		Get_DX8_Texture_Stage_State_Value_Name(value_name,state,value);
		SNAPSHOT_SAY(("DX8 - SetTextureStageState(stage: %d, state: %s, value: %s)",
			stage,
			Get_DX8_Texture_Stage_State_Name(state),
			value_name.str()));
	}
#endif

	TextureStageStates[stage][(unsigned int)state]=value;
	if (GxLogOn) Gx_Log(gxlog::TSS, stage<<16|(unsigned)state, value, nullptr, 0, gxlog::SENT);
	if (GxTruthOn) { GxTruthTSS[stage][(unsigned int)state]=value; GxTruthTSSKnown[stage][(unsigned int)state]=true; }
	if (DX8Ring::Recording) { DX8Ring::Rec_Texture_Stage_State(stage,state,value); }
	else { DX8CALL(SetTextureStageState( stage, state, value )); }
	DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE();
	DX8_CENSUS(DX8C_TSS_DEV);
}

WWINLINE void DX8Wrapper::Set_DX8_Texture(unsigned int stage, IDirect3DBaseTexture8* texture)
{
  	if (stage >= MAX_TEXTURE_STAGES)
  	{	if (GxLogOn) Gx_Log_Ptr(gxlog::DX8_TEXTURE, stage, 0, texture, gxlog::SENT);
  		if (DX8Ring::Recording) { DX8Ring::Rec_Texture(stage,texture); return; }
  		DX8CALL(SetTexture(stage, texture));
  		return;
  	}

	if (Textures[stage]==texture) {
		if (GxLogOn) Gx_Log_Ptr(gxlog::DX8_TEXTURE, stage, 0, texture, 0);	// skipped
		return;
	}
	if (GxLogOn) Gx_Log_Ptr(gxlog::DX8_TEXTURE, stage, 0, texture, gxlog::SENT);

	SNAPSHOT_SAY(("DX8 - SetTexture(%x) ",texture));

	if (DX8Ring::Recording) {
		// the old texture's last reference may be ours: drop it on the render thread, after the replayed SetTexture
		// has unbound it, never under a draw still waiting in the ring
		if (texture) texture->AddRef();
		DX8Ring::Rec_Texture(stage, texture);
		if (Textures[stage]) DX8Ring::Rec_Release(Textures[stage]);
		Textures[stage] = texture;
		DX8_RECORD_TEXTURE_CHANGE();
		return;
	}
	if (Textures[stage]) Textures[stage]->Release();
	Textures[stage] = texture;
	if (Textures[stage]) Textures[stage]->AddRef();
	DX8CALL(SetTexture(stage, texture));
	DX8_RECORD_TEXTURE_CHANGE();
	DX8_CENSUS(DX8C_TEX_DEV);
}

WWINLINE void DX8Wrapper::_Copy_DX8_Rects(
  IDirect3DSurface8* pSourceSurface,
  CONST RECT* pSourceRectsArray,
  UINT cRects,
  IDirect3DSurface8* pDestinationSurface,
  CONST POINT* pDestPointsArray
)
{
	DX8CALL(CopyRects(
  pSourceSurface,
  pSourceRectsArray,
  cRects,
  pDestinationSurface,
  pDestPointsArray));
}

WWINLINE Vector4 DX8Wrapper::Convert_Color(unsigned color)
{
	Vector4 col;
	col[3]=((color&0xff000000)>>24)/255.0f;
	col[0]=((color&0xff0000)>>16)/255.0f;
	col[1]=((color&0xff00)>>8)/255.0f;
	col[2]=((color&0xff)>>0)/255.0f;
//	col=Vector4(1.0f,1.0f,1.0f,1.0f);
	return col;
}

#if 0
WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector3& color, const float alpha)
{
	WWASSERT(color.X<=1.0f);
	WWASSERT(color.Y<=1.0f);
	WWASSERT(color.Z<=1.0f);
	WWASSERT(alpha<=1.0f);
	WWASSERT(color.X>=0.0f);
	WWASSERT(color.Y>=0.0f);
	WWASSERT(color.Z>=0.0f);
	WWASSERT(alpha>=0.0f);

	return D3DCOLOR_COLORVALUE(color.X,color.Y,color.Z,alpha);
}
WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector4& color)
{
	WWASSERT(color.X<=1.0f);
	WWASSERT(color.Y<=1.0f);
	WWASSERT(color.Z<=1.0f);
	WWASSERT(color.W<=1.0f);
	WWASSERT(color.X>=0.0f);
	WWASSERT(color.Y>=0.0f);
	WWASSERT(color.Z>=0.0f);
	WWASSERT(color.W>=0.0f);

	return D3DCOLOR_COLORVALUE(color.X,color.Y,color.Z,color.W);
}
#else

// ----------------------------------------------------------------------------
//
// Convert RGBA color from float vector to 32 bit integer
// Note: Color vector needs to be clamped to [0...1] range!
//
// ----------------------------------------------------------------------------

WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector3& color,float alpha)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	const float scale = 255.0;
	unsigned int col;

	// Multiply r, g, b and a components (0.0,...,1.0) by 255 and convert to integer. Or the integer values togerher
	// such that 32 bit integer has AAAAAAAARRRRRRRRGGGGGGGGBBBBBBBB.
	__asm
	{
		sub	esp,20					// space for a, r, g and b float plus fpu rounding mode

		// Store the fpu rounding mode

		fwait
		fstcw		[esp+16]				// store control word to stack
		mov		eax,[esp+16]		// load it to eax
		mov		edi,eax				// take copy
		and		eax,~(1024|2048)	// mask out certain bits
		or			eax,(1024|2048)	// or with precision control value "truncate"
		sub		edi,eax				// did it change?
		jz			skip					// .. if not, skip
		mov		[esp],eax			// .. change control word
		fldcw		[esp]
skip:

		// Convert the color

		mov	esi,dword ptr color
		fld	dword ptr[scale]

		fld	dword ptr[esi]			// r
		fld	dword ptr[esi+4]		// g
		fld	dword ptr[esi+8]		// b
		fld	dword ptr[alpha]		// a
		fld	st(4)
		fmul	st(4),st
		fmul	st(3),st
		fmul	st(2),st
		fmulp	st(1),st
		fistp	dword ptr[esp+0]		// a
		fistp	dword ptr[esp+4]		// b
		fistp	dword ptr[esp+8]		// g
		fistp	dword ptr[esp+12]		// r
		mov	ecx,[esp]				// a
		mov	eax,[esp+4]				// b
		mov	edx,[esp+8]				// g
		mov	ebx,[esp+12]			// r
		shl	ecx,24					// a << 24
		shl	ebx,16					// r << 16
		shl	edx,8						//	g << 8
		or		eax,ecx					// (a << 24) | b
		or		eax,ebx					// (a << 24) | (r << 16) | b
		or		eax,edx					// (a << 24) | (r << 16) | (g << 8) | b

		fstp	st(0)

		// Restore fpu rounding mode

		cmp	edi,0					// did we change the value?
		je		not_changed			// nope... skip now...
		fwait
		fldcw	[esp+16];
not_changed:
		add	esp,20

		mov	col,eax
	}
	return col;
#else
	return color.Convert_To_ARGB(alpha);
#endif // defined(_MSC_VER) && _MSC_VER < 1300
}

// ----------------------------------------------------------------------------
//
// Clamp color vector to [0...1] range
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Clamp_Color(Vector4& color)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (CPUDetectClass::Has_CMOV_Instruction()) {
	__asm
	{
		mov	esi,dword ptr color

		mov edx,0x3f800000

		mov edi,dword ptr[esi]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi],edi

		mov edi,dword ptr[esi+4]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+4],edi

		mov edi,dword ptr[esi+8]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+8],edi

		mov edi,dword ptr[esi+12]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+12],edi
	}
	return;
	}
#endif // defined(_MSC_VER) && _MSC_VER < 1300

	for (int i=0;i<4;++i) {
		float f=(color[i]<0.0f) ? 0.0f : color[i];
		color[i]=(f>1.0f) ? 1.0f : f;
	}
}

// ----------------------------------------------------------------------------
//
// Convert RGBA color from float vector to 32 bit integer
//
// ----------------------------------------------------------------------------

WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector4& color)
{
	return Convert_Color(reinterpret_cast<const Vector3&>(color),color[3]);
}

WWINLINE unsigned int DX8Wrapper::Convert_Color_Clamp(const Vector4& color)
{
	Vector4 clamped_color=color;
	DX8Wrapper::Clamp_Color(clamped_color);
	return Convert_Color(reinterpret_cast<const Vector3&>(clamped_color),clamped_color[3]);
}

#endif


WWINLINE void DX8Wrapper::Set_Alpha (const float alpha, unsigned int &color)
{
	unsigned char *component = (unsigned char*) &color;

	component [3] = 255.0f * alpha;
}

WWINLINE void DX8Wrapper::Get_Render_State(RenderStateStruct& state)
{
	state=render_state;
}

WWINLINE void DX8Wrapper::Get_Shader(ShaderClass& shader)
{
	shader=render_state.shader;
}

WWINLINE void DX8Wrapper::Set_Texture(unsigned stage,TextureBaseClass* texture)
{
	WWASSERT(stage<(unsigned int)CurrentCaps->Get_Max_Textures_Per_Pass());
	if (GxLogOn && GxLogParent == 0) Gx_Log_Ptr(gxlog::SET_TEXTURE, stage, 0, texture, 0);
	if (texture==render_state.Textures[stage]) return;
	DX8_CENSUS(DX8C_TEX);
	REF_PTR_SET(render_state.Textures[stage],texture);
	render_state_changed|=(TEXTURE0_CHANGED<<stage);
}

WWINLINE void DX8Wrapper::Set_Material(const VertexMaterialClass* material)
{
/*	if (material && render_state.material &&
		// !stricmp(material->Get_Name(),render_state.material->Get_Name())) {
		material->Get_CRC()!=render_state.material->Get_CRC()) {
		return;
	}
*/
//	if (material==render_state.material) {
//		return;
//	}
	if (GxLogOn && GxLogParent == 0) Gx_Log_Ptr(gxlog::SET_MATERIAL, 0, 0, material, 0);
	DX8_CENSUS(DX8C_MAT);
	REF_PTR_SET(render_state.material,const_cast<VertexMaterialClass*>(material));
	render_state_changed|=MATERIAL_CHANGED;
	SNAPSHOT_SAY(("DX8Wrapper::Set_Material(%s)",material ? material->Get_Name() : "null"));
}

WWINLINE void DX8Wrapper::Set_Shader(const ShaderClass& shader)
{
	if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_SHADER, shader.Get_Bits(), 0, nullptr, 0, 0);
	if (!ShaderClass::ShaderDirty && ((unsigned&)shader==(unsigned&)render_state.shader)) {
		return;
	}
	DX8_CENSUS(DX8C_SHADER);
	render_state.shader=shader;
	render_state_changed|=SHADER_CHANGED;
#ifdef MESH_RENDER_SNAPSHOT_ENABLED
	StringClass str;
#endif
	SNAPSHOT_SAY(("DX8Wrapper::Set_Shader(%s)",shader.Get_Description(str).str()));
}

// GeneralsX:
// Applied fighter19 pattern: store Matrix4x4 transposed, cast to D3DMATRIX* for D3D calls
WWINLINE void DX8Wrapper::Set_Projection_Transform_With_Z_Bias(const Matrix4x4& matrix, float znear, float zfar)
{
	ZFar=zfar;
	ZNear=znear;
	ProjectionMatrix=matrix.Transpose();

	if (!Get_Current_Caps()->Support_ZBias() && ZNear!=ZFar) {
		Matrix4x4 tmp=ProjectionMatrix;
		float tmp_zbias=ZBias;
		tmp_zbias*=(1.0f/16.0f);
		tmp_zbias*=1.0f / (ZFar - ZNear);
		tmp[2][2]-=tmp_zbias*tmp[3][2];
		if (GxCaptureOn) { GxProjSent = tmp; GxProjSentValid = true; }
		if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, D3DTS_PROJECTION, 0, &tmp, sizeof(Matrix4x4), gxlog::SENT);
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&tmp));
	}
	else {
		if (GxCaptureOn) { GxProjSent = ProjectionMatrix; GxProjSentValid = true; }
		if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, D3DTS_PROJECTION, 0, &ProjectionMatrix, sizeof(Matrix4x4), gxlog::SENT);
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&ProjectionMatrix));
	}
}

WWINLINE void DX8Wrapper::Set_DX8_ZBias(int zbias)
{
	if (zbias==ZBias) return;
	if (zbias>15) zbias=15;
	if (zbias<0) zbias=0;
	ZBias=zbias;

	if (!Get_Current_Caps()->Support_ZBias() && ZNear!=ZFar) {
		// GeneralsX:
		// Changed D3DMATRIX → Matrix4x4 to match ProjectionMatrix type change
		Matrix4x4 tmp=ProjectionMatrix;
		float tmp_zbias=ZBias;
		tmp_zbias*=(1.0f/16.0f);
		tmp_zbias*=1.0f / (ZFar - ZNear);
		tmp[2][2]-=tmp_zbias*tmp[3][2];
		if (GxCaptureOn) { GxProjSent = tmp; GxProjSentValid = true; }	// the capture missed this send
		if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, D3DTS_PROJECTION, 0, &tmp, sizeof(Matrix4x4), gxlog::SENT);
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&tmp));
	}
	else {
		Set_DX8_Render_State (D3DRS_ZBIAS, ZBias);
	}
}

// GeneralsX:
// Applied fighter19 pattern: transpose and store as Matrix4x4, cast pointer for D3D calls
WWINLINE void DX8Wrapper::Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix4x4& m)
{
	switch ((int)transform) {
	case D3DTS_WORLD:
		render_state.world=m.Transpose();
		if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_TRANSFORM, transform, 0, &render_state.world, sizeof(Matrix4x4), 0);
		render_state_changed|=(unsigned)WORLD_CHANGED;
		render_state_changed&=~(unsigned)WORLD_IDENTITY;
		break;
	case D3DTS_VIEW:
		render_state.view=m.Transpose();
		if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_TRANSFORM, transform, 0, &render_state.view, sizeof(Matrix4x4), 0);
		render_state_changed|=(unsigned)VIEW_CHANGED;
		render_state_changed&=~(unsigned)VIEW_IDENTITY;
		break;
	case D3DTS_PROJECTION:
		{
			Matrix4x4 ProjectionMatrix=m.Transpose();
			ZFar=0.0f;
			ZNear=0.0f;
			if (GxCaptureOn) { GxProjSent = ProjectionMatrix; GxProjSentValid = true; }
			if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, transform, 0, &ProjectionMatrix, sizeof(Matrix4x4), gxlog::SENT);
			if (DX8Ring::Recording) { DX8Ring::Rec_Transform(D3DTS_PROJECTION,(const D3DMATRIX*)&ProjectionMatrix); break; }
			DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&ProjectionMatrix));
		}
		break;
	default:
		DX8_RECORD_MATRIX_CHANGE();
		Matrix4x4 m2=m.Transpose();
		if (GxCaptureOn && transform>=D3DTS_TEXTURE0 && transform<=D3DTS_TEXTURE7) {	// GxShadowView
			GxTexmSent[transform-D3DTS_TEXTURE0]=m2;
			GxTexmValid|=1u<<(transform-D3DTS_TEXTURE0);
		}
		if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, transform, 0, &m2, sizeof(Matrix4x4), gxlog::SENT);
		if (DX8Ring::Recording) { DX8Ring::Rec_Transform(transform,(const D3DMATRIX*)&m2); break; }
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m2));
		break;
	}
}

// GeneralsX:
// Applied fighter19 pattern: convert Matrix3D to Matrix4x4, transpose, cast for D3D calls
WWINLINE void DX8Wrapper::Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix3D& m)
{
	switch ((int)transform) {
	case D3DTS_WORLD:
		render_state.world=Matrix4x4(m).Transpose();
		if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_TRANSFORM, transform, 0, &render_state.world, sizeof(Matrix4x4), 0);
		render_state_changed|=(unsigned)WORLD_CHANGED;
		render_state_changed&=~(unsigned)WORLD_IDENTITY;
		break;
	case D3DTS_VIEW:
		render_state.view=Matrix4x4(m).Transpose();
		if (GxLogOn && GxLogParent == 0) Gx_Log(gxlog::SET_TRANSFORM, transform, 0, &render_state.view, sizeof(Matrix4x4), 0);
		render_state_changed|=(unsigned)VIEW_CHANGED;
		render_state_changed&=~(unsigned)VIEW_IDENTITY;
		break;
	default:
		DX8_RECORD_MATRIX_CHANGE();
		Matrix4x4 m2=Matrix4x4(m).Transpose();
		if (GxCaptureOn && transform>=D3DTS_TEXTURE0 && transform<=D3DTS_TEXTURE7) {	// GxShadowView
			GxTexmSent[transform-D3DTS_TEXTURE0]=m2;
			GxTexmValid|=1u<<(transform-D3DTS_TEXTURE0);
		}
		if (GxLogOn) Gx_Log(gxlog::XFORM_RAW, transform, 0, &m2, sizeof(Matrix4x4), gxlog::SENT);
		if (DX8Ring::Recording) { DX8Ring::Rec_Transform(transform,(const D3DMATRIX*)&m2); break; }
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m2));
		break;
	}
}

WWINLINE bool DX8Wrapper::Is_World_Identity()
{
	return !!(render_state_changed&(unsigned)WORLD_IDENTITY);
}

WWINLINE bool DX8Wrapper::Is_View_Identity()
{
	return !!(render_state_changed&(unsigned)VIEW_IDENTITY);
}

// GeneralsX:
// Applied fighter19 pattern: transpose back when reading stored matrices
WWINLINE void DX8Wrapper::Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m)
{
	D3DMATRIX mat;

	switch ((int)transform) {
	case D3DTS_WORLD:
		if (render_state_changed&WORLD_IDENTITY) m.Make_Identity();
		else m=render_state.world.Transpose();
		break;
	case D3DTS_VIEW:
		if (render_state_changed&VIEW_IDENTITY) m.Make_Identity();
		else m=render_state.view.Transpose();
		break;
	default:
		DX8CALL(GetTransform(transform,&mat));
		m=*(Matrix4x4*)&mat;
		m=m.Transpose();
		break;
	}
}

WWINLINE const D3DLIGHT8& DX8Wrapper::Peek_Light(unsigned index)
{
	return render_state.Lights[index];
}

WWINLINE bool DX8Wrapper::Is_Light_Enabled(unsigned index)
{
	return render_state.LightEnable[index];
}


WWINLINE void DX8Wrapper::Set_Render_State(const RenderStateStruct& state)
{
	int i;
	if (GxLogOn && GxLogParent == 0) Gx_Log_Render_State(gxlog::SET_RENDER_STATE, 0, state);

	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i)
	{
		if (render_state.vertex_buffers[i])
		{
			render_state.vertex_buffers[i]->Release_Engine_Ref();
		}
	}

	render_state=state;
	render_state_changed=0xffffffff;
	++LightStateSerial;	// GX_WRAPSKIP 8: the lights came with the state

	if (render_state.index_buffer) {
		render_state.index_buffer->Add_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i)
	{
		if (render_state.vertex_buffers[i])
		{
			render_state.vertex_buffers[i]->Add_Engine_Ref();
		}
	}
}

WWINLINE void DX8Wrapper::Release_Render_State()
{
	int i;

	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		if (render_state.vertex_buffers[i]) {
			render_state.vertex_buffers[i]->Release_Engine_Ref();
		}
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(render_state.vertex_buffers[i]);
	}
	REF_PTR_RELEASE(render_state.index_buffer);
	REF_PTR_RELEASE(render_state.material);


	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_RELEASE(render_state.Textures[i]);
	}
}


WWINLINE RenderStateStruct::RenderStateStruct()
	:
	material(0),
	index_buffer(0)
{
	unsigned i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) vertex_buffers[i]=0;
	for (i=0;i<MAX_TEXTURE_STAGES;++i) Textures[i]=0;
  //lightsHash = (unsigned)this;
}

WWINLINE RenderStateStruct::~RenderStateStruct()
{
	unsigned i;
	REF_PTR_RELEASE(material);
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(vertex_buffers[i]);
	}
	REF_PTR_RELEASE(index_buffer);

	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_RELEASE(Textures[i]);
	}
}


WWINLINE unsigned flimby( char* name, unsigned crib )
{
  unsigned lnt prevVer = 0x00000000;
  unsigned D3D2_BASE_VEC nextVer = 0;
  for( unsigned t = 0; t < crib; ++t )
  {
    (D3D2_BASE_VEC)nextVer += name[t];
    (D3D2_BASE_VEC)nextVer %= 32;
    (D3D2_BASE_VEC)nextVer-- ;
    (lnt) prevVer ^=  ( 1 << (D3D2_BASE_VEC)prevVer );
  }
  return (lnt) prevVer;
}

WWINLINE RenderStateStruct& RenderStateStruct::operator= (const RenderStateStruct& src)
{
	unsigned i;
	REF_PTR_SET(material,src.material);
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_SET(vertex_buffers[i],src.vertex_buffers[i]);
	}
	REF_PTR_SET(index_buffer,src.index_buffer);

	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_SET(Textures[i],src.Textures[i]);
	}

	LightEnable[0]=src.LightEnable[0];
	LightEnable[1]=src.LightEnable[1];
	LightEnable[2]=src.LightEnable[2];
	LightEnable[3]=src.LightEnable[3];
	if (LightEnable[0]) {
		Lights[0]=src.Lights[0];
		if (LightEnable[1]) {
			Lights[1]=src.Lights[1];
			if (LightEnable[2]) {
				Lights[2]=src.Lights[2];
				if (LightEnable[3]) {
					Lights[3]=src.Lights[3];
				}
			}
		}


    //lightsHash = flimby((char*)(&Lights[0]), sizeof(D3DLIGHT8)-1 );

	}

	shader=src.shader;
	world=src.world;
	view=src.view;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		vertex_buffer_types[i]=src.vertex_buffer_types[i];
	}
	index_buffer_type=src.index_buffer_type;
	vba_offset=src.vba_offset;
	vba_count=src.vba_count;
	iba_offset=src.iba_offset;
	index_base_offset=src.index_base_offset;

	return *this;
}
