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
 *                     $Archive:: /Commando/Code/ww3d2/dx8renderer.h                          $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/27/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 29                                                          $*
 *                                                                                             *
 * 06/27/02 KM Changes to max texture stage caps																*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWLib/wwstring.h"
#include "WWLib/simplevec.h"
#include "WWLib/Vector.h"
#include "dx8list.h"
#include "WW3D2/shader.h"
#include "dx8wrapper.h"
#include "WW3D2/meshmatdesc.h"
#include <vector>

class IndexBufferClass;
class VertexBufferClass;
class DX8RenderTypeArrayClass;
class MeshClass;
class MeshModelClass;
class DX8PolygonRendererClass;
class Vertex_Split_Table;
class DX8FVFCategoryContainer;
class DecalMeshClass;
class MaterialPassClass;
class MatPassTaskClass;
class PolyRenderTaskClass;
class TextureClass;
class VertexMaterialClass;
class CameraClass;

#define RECORD_RENDER(I,P) render_stats[I].count++; render_stats[I].polys+=P;

// GeneralsX: R-CPU 1f parallel object collection (W3DScene.cpp, GX_COLLECTPAR). While a
// collection worker renders an object, the five adds below that link it into lists every object shares (a render
// task, a material pass, a delayed material pass, a visible skin, a decal mesh) are recorded into that thread's sink
// instead of run; the main thread replays them later in object order, so every list ends up exactly as the serial
// walk leaves it. A null sink - every other thread, always - runs them as before.
struct GxCollectRec
{
	enum Kind { TASK, MATPASS, DELAYED_MATPASS, SKIN, DECAL, POSE };	// POSE is W3DScene's: a pose keep replayed on main
	int kind;
	void *a, *b, *c;
};
struct GxCollectSink
{
	GxCollectRec *rec;
	int count;
	int cap;
	bool overflow;
	void Push(int kind, void *a, void *b = nullptr, void *c = nullptr)
	{
		if (count >= cap) { overflow = true; return; }
		GxCollectRec &r = rec[count++];
		r.kind = kind; r.a = a; r.b = b; r.c = c;
	}
};
extern thread_local GxCollectSink *GxCollectActiveSink;
// Set by the main thread only around a collection dispatch or check, so the adds skip the thread-local read otherwise.
extern bool GxCollectSinkArmed;
inline GxCollectSink *GxCollect_Sink() { return GxCollectSinkArmed ? GxCollectActiveSink : nullptr; }
void GxCollect_Replay(const GxCollectRec &r);	// main thread, sink null; TASK..DECAL only

// GeneralsX: WS-G ("L2", §3 WS-G): the classic draw
// list - what one DX8MeshRendererClass::Flush is about to draw, as one entry per base-pass render task, deformed skin,
// procedural material pass, decal mesh and delayed material pass, IN FLUSH'S ORDER (rigid containers by pass and visible
// category, their material passes; skins batch by batch as the 65535-vertex dynamic VB fills; decals; delayed passes).
// Gx_List_Pending only reads the lists. GxL2FlushHook (null unless GameEngineDevice's GXRender/Scene installs it) runs
// at the start (end=false) and the end (end=true) of every Flush that has a camera; while GxL2Executed is non-null,
// Flush appends what it actually processes to it, in the same form (GX_L2_CHECK compares the two). Off, the classic
// path costs one pointer test per Flush and one per processed entry.
struct GxClassicDraw
{
	enum Kind { TASK, SKIN, MATPASS, DELAYED_MATPASS, DECAL };
	int kind;
	DX8TextureCategoryClass *cat;	// TASK
	DX8PolygonRendererClass *pr;	// TASK
	MaterialPassClass *pass;		// MATPASS, DELAYED_MATPASS
	MeshClass *mesh;				// all but DECAL
	DecalMeshClass *decal;			// DECAL
	int batch;						// skin containers: the dynamic-VB batch (0, 1, ...); else 0
};
extern void (*GxL2FlushHook)(bool end);
// GeneralsX: WS-G Task 5 T3: the call log's close point - once per Flush, right after its two null-buffer
// setters (after FLUSH_END); null unless GX_L2_MODEL=1 installs it.
extern void (*GxL2FlushClosedHook)();
// GeneralsX: WS-G S1 seat: asked once per task the category loop draws, right
// after its list entry; true = the seat drew this task inside its claimed run (PREPARE -> GxDrawRun::Draw_Run -> CARRY-OUT, at the
// run's first task) and the loop only unlinks it. Null unless GameEngineDevice's GXRender/Scene installs it (GX_L2_DRAWRUN=1, not in
// check mode).
class DX8TextureCategoryClass;
extern bool (*GxL2SeatHook)(DX8TextureCategoryClass *cat, PolyRenderTaskClass *prt);
// The task walk the seat needs (PolyRenderTaskClass is private to dx8renderer.cpp): the next visible task, its mesh and polygon renderer,
// and whether the loop skips it (VERTEX_BUFFER_OVERFLOW). Pure reads.
class MeshClass;
class DX8PolygonRendererClass;
PolyRenderTaskClass *Gx_Task_Next(PolyRenderTaskClass *t);
MeshClass *Gx_Task_Mesh(PolyRenderTaskClass *t);
DX8PolygonRendererClass *Gx_Task_Renderer(PolyRenderTaskClass *t);
bool Gx_Task_Skipped(PolyRenderTaskClass *t);
// The flush site: a caller sets GxFlushSiteNext just before it calls Flush; Flush moves it to
// GxFlushSite and resets it, so an untagged flush (WW3D::Flush - ww3d.cpp is digested - or any new caller) is UNSET.
enum GxFlushSite
{
	GXFS_UNSET, GXFS_MAIN_OPAQUE, GXFS_OCCL_TRANSL_PRE, GXFS_OCCL_TRANSL_STENCIL, GXFS_OCCL_BUCKET, GXFS_OCCL_OTHERS,
	GXFS_OCCL_OCCLUDERS, GXFS_OCCL_EMPTY, GXFS_OCCL_PLAIN, GXFS_TRANSLUCENT, GXFS_STATIC_SORT, GXFS_PROJ_SHADOW, GXFS_N
};
extern int GxFlushSiteNext;
extern int GxFlushSite;
extern std::vector<GxClassicDraw> *GxL2Executed;

// GeneralsX: the
// object-velocity bridge's hooks. GXRender/Scene/GxMotionUnits.cpp installs them only while its key is on and a world pass is open;
// null (the default, always on the classic path) = no call. Render-only: nothing here is read by the simulation.
struct GxMotionHooks
{
	void (*rigidBegin)(const MeshClass *mesh, const void *model, const Matrix3D &world);	// a non-skin, non-sorted mesh task, before its draws
	void (*rigidEnd)();													// ... and after them (the pending matrix is scoped to the task)
	void (*skinMesh)(const MeshClass *mesh, const void *model, const Vector3 *pos, int count, unsigned batchOffset);	// after Get_Deformed_Vertices
	void (*skinBatch)(bool open);	// true once a non-sorting skin batch is filled (before its draws), false after its draws
};
extern GxMotionHooks *GxMotion;

// GeneralsX: S7 GPU sun shadow map (
// A7/A8, PLAN Task 5): the shadow-map caster tag's hooks, a SIBLING of GxMotion (the motion bridge is untouched). The ZH glue
// (W3DShadowMap.cpp) installs them only while GXShadowMap is on and the frame's pass is active; null (the default, always on the
// classic path and key off) = no call. meshBegin decides per non-skin, non-sorted mesh task whether its draws are casters (pass 0
// only: a multi-pass material must not cast twice) and sets the device's CAST tag on CHANGE; containerEnd clears it at the end of
// a rigid container, so the skin and sorting paths that follow never inherit it. Render-only.
struct GxShadowCastHooks
{
	void (*meshBegin)(const MeshClass *mesh, int pass);
	void (*containerEnd)();
	// A41: a procedural or delayed material pass draws through Render_Material_Pass,
	// not through a mesh task - it gets its root's own light here (never CAST); containerEnd() after each such loop clears it
	void (*passBegin)(const MeshClass *mesh);
};
extern GxShadowCastHooks *GxShadowCast;

/**
** DX8TextureCategoryClass
** This class is used for each Material-Texture-Shader combination that is encountered during rendering.
** Each polygon_renderer that uses the same 'TextureCategory' will be linked to the 'TextureCategory' object.
** Then, all polygons will be rendered in 'TextureCategory' batches to reduce the number of stage changes
** (and most importantly, texture changes) that we cause in DX8.
*/
class DX8TextureCategoryClass : public MultiListObjectClass
{
	int												pass;
	TextureClass *									textures[MeshMatDescClass::MAX_TEX_STAGES];
	ShaderClass										shader;
	VertexMaterialClass *						material;
	DX8PolygonRendererList						PolygonRendererList;
	DX8FVFCategoryContainer*					container;

	PolyRenderTaskClass *						render_task_head;			// polygon renderers queued for rendering
	static bool											m_gForceMultiply;  // Forces opaque materials to use the multiply blend - pseudo transparent effect.  jba.

public:

	DX8TextureCategoryClass(DX8FVFCategoryContainer* container,TextureClass** textures, ShaderClass shd, VertexMaterialClass* mat,int pass);
	virtual ~DX8TextureCategoryClass() override;

	void									Add_Render_Task(DX8PolygonRendererClass * p_renderer,MeshClass * p_mesh);

	void									Render();
	void									Gx_List_Tasks(std::vector<GxClassicDraw> &out, const void *skin_batches, int batch);	// WS-G, dx8renderer.cpp
	bool									Anything_To_Render() { return (render_task_head != nullptr); }
	void									Clear_Render_List();

	TextureClass *						Peek_Texture(int stage)	{ return textures[stage]; }
	const VertexMaterialClass *	Peek_Material() { return material; }
	ShaderClass							Get_Shader() { return shader; }

	DX8PolygonRendererList&			Get_Polygon_Renderer_List() { return PolygonRendererList; }

	unsigned Add_Mesh(
		Vertex_Split_Table& split_buffer,
		unsigned vertex_offset,
		unsigned index_offset,
		IndexBufferClass* index_buffer,
		unsigned pass);
	void Log(bool only_visible);

	void Remove_Polygon_Renderer(DX8PolygonRendererClass* p_renderer);
	void Add_Polygon_Renderer(DX8PolygonRendererClass* p_renderer,DX8PolygonRendererClass* add_after_this=nullptr);


	DX8FVFCategoryContainer * Get_Container() { return container; }

	// Force multiply blend on all objects inserted from now on. (Doesn't affect the objects that are already in the lists)
	static void						SetForceMultiply(bool multiply) { m_gForceMultiply=multiply; }
	static bool						GetForceMultiply() { return m_gForceMultiply; }

};

// ----------------------------------------------------------------------------

/**
** DX8FVFCategoryContainer
*/

class DX8FVFCategoryContainer : public MultiListObjectClass
{
public:
	enum {
		MAX_PASSES=4
	};
protected:

	TextureCategoryList									texture_category_list[MAX_PASSES];
	TextureCategoryList									visible_texture_category_list[MAX_PASSES];

	MatPassTaskClass *									visible_matpass_head;
	MatPassTaskClass *									visible_matpass_tail;

	IndexBufferClass *									index_buffer;
	int														used_indices;
	unsigned													FVF;
	unsigned													passes;
	unsigned													uv_coordinate_channels;
	bool														sorting;
	bool														AnythingToRender;
	bool														AnyDelayedPassesToRender;

	void Generate_Texture_Categories(Vertex_Split_Table& split_table,unsigned vertex_offset);
	void Insert_To_Texture_Category(
		Vertex_Split_Table& split_table,
		TextureClass** textures,
		VertexMaterialClass* mat,
		ShaderClass shader,
		int pass,
		unsigned vertex_offset);

	bool Anything_To_Render()					{ return AnythingToRender; }
	bool Any_Delayed_Passes_To_Render()	{ return AnyDelayedPassesToRender; }

	void Render_Procedural_Material_Passes();
	void Gx_List_Mat_Passes(std::vector<GxClassicDraw> &out, const void *skin_batches, int batch);	// WS-G

	DX8TextureCategoryClass* Find_Matching_Texture_Category(
		TextureClass* texture,
		unsigned pass,
		unsigned stage,
		DX8TextureCategoryClass* ref_category);

	DX8TextureCategoryClass* Find_Matching_Texture_Category(
		VertexMaterialClass* vmat,
		unsigned pass,
		DX8TextureCategoryClass* ref_category);

public:

	DX8FVFCategoryContainer(unsigned FVF,bool sorting);
	virtual ~DX8FVFCategoryContainer() override;

	static unsigned Define_FVF(MeshModelClass* mmc,bool enable_lighting);
	bool Is_Sorting() const { return sorting; }
	virtual bool Is_Skin() const { return false; }	// WS-G (GXRender/Scene): the L2 contract's skin-container test

	void Change_Polygon_Renderer_Texture(
		DX8PolygonRendererList& polygon_renderer_list,
		TextureClass* texture,
		TextureClass* new_texture,
		unsigned pass,
		unsigned stage);

	void Change_Polygon_Renderer_Material(
		DX8PolygonRendererList& polygon_renderer_list,
		VertexMaterialClass* vmat,
		VertexMaterialClass* new_vmat,
		unsigned pass);

	void Remove_Texture_Category(DX8TextureCategoryClass* tex_category);

	virtual void Render()=0;
	virtual void Gx_List_Pending(std::vector<GxClassicDraw> &out)=0;	// WS-G: what Render() would draw, in its order
	virtual void Add_Mesh(MeshModelClass* mmc)=0;
	virtual void Log(bool only_visible)=0;
	virtual bool Check_If_Mesh_Fits(MeshModelClass* mmc)=0;

	unsigned Get_FVF() const { return FVF; }

	void Add_Visible_Texture_Category(DX8TextureCategoryClass * tex_category,int pass)
	{
		WWASSERT(pass<MAX_PASSES);
		WWASSERT(tex_category != nullptr);
		WWASSERT(texture_category_list[pass].Contains(tex_category));
		visible_texture_category_list[pass].Add(tex_category);
		AnythingToRender=true;
	}

	/*
	** Material pass rendering.  The following two functions allow procedural material passes
	** to be applied to meshes in this FVF category.  In certain cases, the game will *only* render
	** the procedural pass and not the base materials for the mesh.  When this happens there can
	** be rendering errors unless these procedural passes are rendered after all of the meshes in
	** the scene.  The virtual method Add_Delayed_Material_Pass is used in this case.
	*/
	void Add_Visible_Material_Pass(MaterialPassClass * pass,MeshClass * mesh);
	virtual void Add_Delayed_Visible_Material_Pass(MaterialPassClass * pass, MeshClass * mesh) = 0;
	virtual void Render_Delayed_Procedural_Material_Passes() = 0;
};


/**
** DX8RigidFVFCategoryContainer
** This is an FVFCategoryContainer for rigid (non-skin) meshes
*/
class DX8RigidFVFCategoryContainer : public DX8FVFCategoryContainer
{
public:
	DX8RigidFVFCategoryContainer(unsigned FVF,bool sorting);
	virtual ~DX8RigidFVFCategoryContainer() override;

	virtual void Add_Mesh(MeshModelClass* mmc) override;
	virtual void Log(bool only_visible) override;
	virtual bool Check_If_Mesh_Fits(MeshModelClass* mmc) override;

	virtual void Render() override;	// Generic render function
	virtual void Gx_List_Pending(std::vector<GxClassicDraw> &out) override;
	void Gx_List_Delayed(std::vector<GxClassicDraw> &out);	// WS-G: what Render_Delayed_Procedural_Material_Passes would draw

	/*
	** This method adds a material pass which must be rendered after all of the other rendering is complete.
	** This is needed whenever a mesh turns off its base passes and renders a translucent pass on its geometry.
	*/
	virtual void Add_Delayed_Visible_Material_Pass(MaterialPassClass * pass, MeshClass * mesh) override;
	virtual void Render_Delayed_Procedural_Material_Passes() override;

protected:


	VertexBufferClass *	vertex_buffer;
	int						used_vertices;

	MatPassTaskClass *	delayed_matpass_head;
	MatPassTaskClass *	delayed_matpass_tail;

};


/**
** DX8SkinFVFCategoryContainer
** This is an FVFCategoryContainer for skin meshes
*/
class DX8SkinFVFCategoryContainer: public DX8FVFCategoryContainer
{
public:
	DX8SkinFVFCategoryContainer(bool sorting);
	virtual ~DX8SkinFVFCategoryContainer() override;

	virtual void Render() override;
	virtual void Gx_List_Pending(std::vector<GxClassicDraw> &out) override;
	virtual bool Is_Skin() const override { return true; }
	virtual void Add_Mesh(MeshModelClass* mmc) override;
	virtual void Log(bool only_visible) override;
	virtual bool Check_If_Mesh_Fits(MeshModelClass* mmc) override;

	void Add_Visible_Skin(MeshClass * mesh);

	/*
	** Since skins are already rendered after the rigid meshes, the Add_Delayed_Material_Pass function simply
	** routes into the Add_Visible_Material_Pass method and no extra overhead is added.
	*/
	virtual void Add_Delayed_Visible_Material_Pass(MaterialPassClass * pass, MeshClass * mesh) override { Add_Visible_Material_Pass(pass,mesh); }
	virtual void Render_Delayed_Procedural_Material_Passes() override { }

private:

	void Reset();
	void clearVisibleSkinList();

	unsigned int								VisibleVertexCount;
	MeshClass *									VisibleSkinHead;
	MeshClass *									VisibleSkinTail;

};



/**
** DX8MeshRendererClass
** This object is controller for the entire DX8 mesh rendering system.  It organizes mesh
** fragments into groups based on FVF, texture, and material.  During rendering, a list of
** the visible mesh fragments is composed and rendered.  There is a global instance of this
** class called TheDX8MeshRenderer that should be used for all mesh rendering.
*/
class DX8MeshRendererClass
{
public:
	DX8MeshRendererClass();
	~DX8MeshRendererClass();

	void						Init();
	void						Shutdown();

	void						Flush();
	void						Gx_List_Pending(std::vector<GxClassicDraw> &out);	// WS-G: the classic draw list of the next Flush (GxClassicDraw above)
	// GeneralsX: WS-X hybrid spike: called inside Flush at the two points where native L2 work
	// would execute without reordering the flush's categories - after the rigid lists (native rigid draws replace those,
	// ahead of the classic skins and decals) and after the delayed procedural passes - with the classic draws that segment
	// made. Null (the default) costs one test per boundary.
	enum { GX_BOUNDARY_RIGID, GX_BOUNDARY_DELAYED };
	typedef void (*GxBoundaryFn)(int boundary, unsigned long long classicDraws);
	static GxBoundaryFn GxBoundary;
	void						Clear_Pending_Delete_Lists();

	void						Log_Statistics_String(bool only_visible);
	static void				Request_Log_Statistics();

	void						Register_Mesh_Type(MeshModelClass* mmc);
	void						Unregister_Mesh_Type(MeshModelClass* mmc);
	void						Set_Camera(CameraClass* cam) { camera=cam; }
	CameraClass *			Peek_Camera()	{ return camera; }
	void						Add_To_Render_List(DecalMeshClass * decalmesh);

	// Enable or disable lighting on all objects inserted from now on. (Doesn't affect the objects that are already in the lists)
	void						Enable_Lighting(bool enable) { enable_lighting=enable; }

	// This should be called at the beginning of a game or menu or after a major modifications to the scene...
	void						Invalidate(bool shutdown=false);	// Added flag so it doesn't allocate more mem when shutting down. -MW

protected:

	void Render_Decal_Meshes();

	bool													enable_lighting;
	CameraClass *										camera;

	SimpleDynVecClass<FVFCategoryList *>		texture_category_container_lists_rigid;
	FVFCategoryList *									texture_category_container_list_skin;

	DecalMeshClass *									visible_decal_meshes;


};

extern DX8MeshRendererClass TheDX8MeshRenderer;

// GeneralsX: measurement (GX_FLUSHPROF=1): main-thread time inside the object-flush
// bracket of RTS3DScene::Flush (DX8Ring::Begin_Record..End_Record), split into object submission (renderOneObject), the rigid
// and the skin container flushes; [GXFLUSHPROF] every 120 frames, us per frame. Unset = one bool test per scope.
struct GxFlushProf
{
	enum { TOTAL, SUBMIT, RIGID, SKIN, N };
	static const bool On;
	static bool InBracket;
	static long long Ns[N];
	static long long Now();
	static void Frame();	// once per Present (DX8Wrapper): every bracket of a multi-pass frame lands in one frame
};
struct GxFlushScope
{
	int b;
	long long t0;
	explicit GxFlushScope(int block) : b(GxFlushProf::On && GxFlushProf::InBracket ? block : -1), t0(b >= 0 ? GxFlushProf::Now() : 0) {}
	~GxFlushScope() { if (b >= 0) GxFlushProf::Ns[b] += GxFlushProf::Now() - t0; }
};
