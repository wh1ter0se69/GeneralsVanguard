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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "WWMath/matrix4.h"
#include "W3DDevice/GameClient/W3DBufferManager.h"
#include "GameClient/Shadow.h"

#include <vector>

///@todo Make the 100 below a 'better' number. Was 32, increased because of overcomplex models.
#define MAX_SHADOW_CASTER_MESHES	160	//number of meshes allowed in animated hierarchy (must be <256 since index is a byte).

class W3DShadowGeometry;	//forward reference
class W3DShadowGeometryManager;	//forward reference
struct Geometry;	//forward reference
struct PolyNeighbor;	//forward reference
class W3DVolumetricShadow;	//forward reference
class Drawable;	//forward reference

struct W3DVolumetricShadowRenderTask : public W3DBufferManager::W3DRenderTask
{
	W3DVolumetricShadow	*m_parentShadow;		///<main casting object to which this volume belongs.
	UnsignedByte		m_meshIndex;		///<mesh index of volume within parent to render.
	UnsignedByte		m_lightIndex;		///<light index of volume within parent to render.
	// GeneralsX @performance Claude 23/09/2026 Where a dynamic volume's first-pass upload landed in the dynamic
	// VB/IB, so the second (DECR) pass can draw it again without re-uploading. Valid only while m_uploadEpoch
	// equals the manager's upload epoch (0 = not uploaded).
	UnsignedInt		m_uploadEpoch;
	Int			m_uploadVertex;		///<GeneralsX 02/10: the upload's first vertex in the dynamic VB (the DECR pass re-sends indices)
};

// ShadowManager -------------------------------------------------------------
class AABoxClass;	// S7 A39: the walk's visible box
class W3DVolumetricShadowManager
{

public:

	W3DVolumetricShadowManager();
	~W3DVolumetricShadowManager();
	Bool init();	///<initialize resources used by manager, must have valid D3D device.
	// shadow list management
	void reset();
	W3DVolumetricShadow* addShadow( RenderObjClass *robj, Shadow::ShadowTypeInfo *shadowInfo, Drawable *draw);	///< adds shadow caster to rendering system.
	void removeShadow(W3DVolumetricShadow *shadow);	///< removed shadow from rendering system and frees its resources.
	void removeAllShadows(); ///< Remove all shadows.
	/// queues up a dynamic shadow caster for rendering - only used internally by shadow system.
	void addDynamicShadowTask(W3DVolumetricShadowRenderTask *task)
	{	W3DBufferManager::W3DRenderTask *oldTask=m_dynamicShadowVolumesToRender;
		m_dynamicShadowVolumesToRender=task;
		m_dynamicShadowVolumesToRender->m_nextTask=oldTask;
	}
	void invalidateCachedLightPositions();	///<forces shadow volumes to update regardless of last lightposition
	void loadTerrainShadows();

	// rendering
	void renderShadows( Bool forceStencilFill );
	// GeneralsX @feature D · look-shadowmaps 17/10/2026 S7 (PREREG_look-shadowmaps A4/A7/A8/A28, A39/A41): the shadow map's caster walk -
	// every caster today's volumes would draw (enabled, not invisible), as its ROOT render object (sorted, for the mesh tag's lookup),
	// with its OWN LIGHT when its elevation clamp binds (GXShadowMapFit.h ownLight: the instance's live m_shadowLengthScale - template
	// or raised at runtime by updateOptimalExtrusionPadding, which the walk runs exactly where Update() would: A41 P2-2), and the
	// nearest axis distance of their bounds from 2003's point light L along the map's axis. Render-only.
	struct GxShadowMapWalkOut
	{
		std::vector<RenderObjClass *> roots;	///< sorted, unique (mutable: A44's early cast-only flush Render()s the off-screen ones)
		std::vector<uint64_t> own;					///< parallel to roots: the root's own light (packed; 0 = none)
		std::vector<uint8_t> cand;					///< parallel to roots: A47 - one of its shadows passes 2003's own Update pre-cull (1)
		std::vector<uint8_t> posed;					///< parallel to roots: A48 - classic's pose trigger found a visible geometry mesh (1)
		Real nearDist = 0.0f;						///< the nearest caster bound's axis distance from L
		Real topZ = 0.0f;							///< the highest world Z any caster reaches (the fit raises its slab to it: Codex code-fit3-r1 #1)
		Real minZ0 = 0.0f;							///< the lowest own-light plane (the fit lowers its slab to it: A41 P2-4)
		Bool any = FALSE, anyOwn = FALSE;
		UnsignedInt clampedTemplate = 0, clampedRuntime = 0;	///< roots with a binding clamp: the template's / raised at runtime (M5)
		UnsignedInt paddingNow = 0, conflicts = 0;	///< runtime clamps computed this walk; roots whose shadows disagree on the own light
		UnsignedInt pulledAir = 0, pulledGround = 0, pulledRuntime = 0;	///< A79: shadows in the pull class (template airborne / grounded; runtime)
	};
	// A79 (E · look-shadowmaps-7): eps / muMax = the frame's own-light pull (GXShadowMapFit.h ownPullEps / kOwnPullMuMax; eps 0 = none) - a
	// pulled root's sphere enters nearDist pulled (pulledNear), and the census counts the pull class
	void gxShadowMapWalk(GxShadowMapWalkOut &out, const Vector3 &L, const Vector3 &axis, const AABoxClass &visibleBox, Real pullEps = 0.0f,
		Real pullMuMax = 0.0f);
	// GeneralsX @test D · look-shadowmaps-5 18/10/2026 S7 PREREG A60/A60a (TEST-ONLY, the D60 diagnostic): TRUE when the spec names this
	// mesh - <CONTAINERPREFIX>.<MESH>, case-insensitive (its name after the last '.' is MESH, the part before starts with CONTAINERPREFIX).
	// map = FALSE: GX_SHADOW_DROP_MESH, 2003's volume geometry skips it (key OFF arms only). map = TRUE: GX_SHADOWMAP_DROP_CAST, S7's
	// meshBegin strips CAST from its draws (nothing else changes). Unset: FALSE. A73 (E · look-shadowmaps-6): a comma list of up to 8
	// entries, and an entry ending in '.' names every mesh of that container prefix (GXShadowDropSpec.h).
	static Bool gxShadowDropMesh(const char *meshName, Bool map);
	void ReleaseResources();
	Bool ReAcquireResources();

protected:

		// to render the stencil buffer polygon to the screen
		void renderStencilShadows();

		// GeneralsX @performance Claude 23/09/2026 A contiguous array replaces the m_next chain. Each caster is a
		// ~23 KB object, so walking the chain cost one dependent cache miss per caster on every rendered frame - most
		// of it on casters the enable check then skips (fogged, stealthed). An array lets the walk prefetch ahead.
		// Stored oldest first; every walk runs it BACKWARDS, which is exactly the old newest-first chain order.
		std::vector<W3DVolumetricShadow *> m_shadowList;
		W3DVolumetricShadowRenderTask *m_dynamicShadowVolumesToRender;
		W3DShadowGeometryManager *m_W3DShadowGeometryManager;
};

extern W3DVolumetricShadowManager *TheW3DVolumetricShadowManager;

// W3DVolumetricShadow ---------------------------------------------------------------------
class W3DVolumetricShadow	: public Shadow
{
	friend class W3DVolumetricShadowManager;

	enum
	{
		SHADOW_DYNAMIC	=	0x1,		//flag indicating a dynamic shadow caster (animated mesh).
		SHADOW_GPU		=	0x2,		///<GeneralsX 08/10/2026: a dynamic volume extruded by the GPU from its mesh's edge mesh (GX_SHADOW_GPU)
		SHADOW_GPU_STALE =	0x4			///<GeneralsX 09/10/2026: a SHADOW_GPU volume nothing could draw (RenderGpuMeshVolume): rebuilt on the next update
	};

	public:

		W3DVolumetricShadow();
		~W3DVolumetricShadow();

	protected:

		virtual void release() override	{TheW3DVolumetricShadowManager->removeShadow(this);}	///<release shadow from manager

		#if defined(RTS_DEBUG)
		virtual void getRenderCost(RenderCost & rc) const override;
		#endif

		// tie in geometry and transformation for this shadow
		void SetGeometry( W3DShadowGeometry *geometry );
		void setShadowLengthScale(Real value) {m_shadowLengthScale = value;}
		void updateOptimalExtrusionPadding();	///<for immobile objects, figure out the length of extrusion needed to hit ground - expensive!
		void setOptimalExtrusionPadding(Real value)	{m_extraExtrusionPadding=value;}
		const W3DShadowGeometry *getGeometry() {return m_geometry;}

		void setRenderObject( RenderObjClass	*robj) {assert(m_robj==nullptr); m_robj=robj;}
 		void setRenderObjExtent ( Real extent) { m_robjExtent = extent; }

		// called once per frame, updates shadow volume when necessary
		void Update();
		void updateVolumes(Real zoffset, Bool allowSizeSkip = TRUE);	///<update shadow volumes of all meshes in this model (GeneralsX: allowSizeSkip FALSE for airborne casters)
		Bool gxClassicPoseTrigger(SphereClass *bound);	///<S7 A48/A51: updateVolumes' pose-touching prefix only; TRUE when a geometry mesh is visible; *bound = their spheres' union
		void updateMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform, const AABoxClass &meshBox, float floorZ);///<update shadow volume of this mesh.

		// rendering interface
		void RenderVolume(Int meshIndex, Int lightIndex);	///<renders a specific volume from the model hierarchy
		///render single mesh which could belong to a larger hierarchy (optimized for static meshes).
		void RenderMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform);
		///render single mesh which could belong to a larger hierarchy (optimized for animated meshes).
		void RenderDynamicMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform);
		void RenderGpuMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform);	///<GeneralsX 08/10/2026: queues a SHADOW_GPU volume for both stencil passes
		void RenderMeshVolumeBounds(Int meshIndex, Int lightIndex, const Matrix3D *meshXform);	///<render bounding volume around shadow volume - for debug use.

		void setLightPosHistory(Int lightIndex, Int meshIndex, Vector3 &pos) {m_lightPosHistory[lightIndex][meshIndex]=pos;}	///<updates the last position of light

		// silhouette tools
		void buildSilhouette(Int meshIndex, Vector3 *lightPosWorld);
		void addSilhouetteEdge(Int meshIndex, PolyNeighbor *visible, PolyNeighbor *hidden );
		void addNeighborlessEdges(Int meshIndex, PolyNeighbor *us );
		void addSilhouetteIndices(Int meshIndex, Short edgeStart, Short edgeEnd );
		Bool allocateSilhouette(Int meshIndex, Int numVertices );  // allocate memory for sil
		void deleteSilhouette(Int meshIndex );  // resets and frees silhouette memory
		void resetSilhouette( Int meshIndex );  // reset silhouette to empty

		// shadow volume access
		void constructVolume( Vector3 *lightPos, Real shadowExtrudeDistance, Int volumeIndex, Int meshIndex );
		void constructVolumeVB( Vector3 *lightPosObject,Real shadowExtrudeDistance, Int volumeIndex, Int meshIndex );
		Bool allocateShadowVolume( Int volumeIndex, Int meshIndex );  // allocate mem
		void deleteShadowVolume( Int volumeIndex );  // delete all volume data
		void resetShadowVolume( Int volumeIndex, Int meshIndex );  // reset shadow volume

		// this is our current geometry we will derive the shadow from
		W3DShadowGeometry *m_geometry;

		RenderObjClass	*m_robj;	///<render object belonging to model casting shadow

		Real	m_shadowLengthScale;	///<scale factor used to reduce/clamp shadow length
		Real	m_gxTemplateLengthScale;	///<S7 A39 M5: the template's own clamp (m_shadowLengthScale as created), to tell runtime raises apart
 		Real	m_robjExtent;			///<maximum possible horizontal reach of shadow from center of object.
		Real	m_extraExtrusionPadding;	///<amount to extrude shadow beyond where it would normally stop (ground level below object).

		//
		// this geometry is our actual shadow volume data, we have an array
		// of them so we can have separate shadow volume for each light source
		// casting shadows
		//

		static Geometry m_tempShadowVolume;	//scratch buffer to use by all shadows during volume construction.
		Geometry *m_shadowVolume[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];
		W3DBufferManager::W3DVertexBufferSlot *m_shadowVolumeVB[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];
		W3DBufferManager::W3DIndexBufferSlot *m_shadowVolumeIB[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];
		W3DVolumetricShadowRenderTask m_shadowVolumeRenderTask[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];
		Int m_shadowVolumeCount[MAX_SHADOW_CASTER_MESHES];  // how man shadows are valid in m_shadowVolume
		Vector3 m_lightPosHistory[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];
		Matrix4x4 m_objectXformHistory[ MAX_SHADOW_LIGHTS ][MAX_SHADOW_CASTER_MESHES];

		// silhouette building space
		Short *m_silhouetteIndex[MAX_SHADOW_CASTER_MESHES];  // silhouette vertex index list, edges occur
									 // as disjoint pairs.  The actual size of this
									 // piece of memory must accommodate #vertices*2
		Short m_numSilhouetteIndices[MAX_SHADOW_CASTER_MESHES];  // total number of edge indices in the index
									   // array, these are pairs and therefore
									   // always a multiple of two
		Short m_maxSilhouetteEntries[MAX_SHADOW_CASTER_MESHES];  // how big the silhouette index can hold max

		Int	  m_numIndicesPerMesh[MAX_SHADOW_CASTER_MESHES];	///<silhouette indices from each mesh.

};
