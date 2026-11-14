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

// FILE: W3DVolumetricShadow.cpp ///////////////////////////////////////////////////////////
//
// Real time shadow volume representations
//
// Author: Colin Day, January 2001
// Adapted for W3D: Mark Wilczynski October 2001
//
//
///////////////////////////////////////////////////////////////////////////////
///@todo: Must cap shadow volumes if we ever allow camera inside the volumes.
///@todo: Find better way to determine when shadow volumes need updating - lights move, objects move.

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <assert.h>
#include <algorithm>
#include <memory>

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "WWLib/always.h"
#include "GameClient/View.h"
#include "GameClient/GXGfxProbe.h"	// [GXSHADOW]
#include "WW3D2/camera.h"
#include "WW3D2/light.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/hlod.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "d3dx8math.h"
#include "Common/GlobalData.h"
#include "Common/DrawModule.h"
#include <atomic>
#include "W3DDevice/GameClient/W3DVolumetricShadow.h"
#include "W3DDevice/GameClient/W3DShadow.h"
#include "W3DDevice/GameClient/GxShadowGpuProj.h"
#include "W3DDevice/GameClient/W3DWorldSeam.h"
#include "WW3D2/statistics.h"
#include "GameLogic/TerrainLogic.h"
#include "WW3D2/dx8caps.h"
#include "GameClient/Drawable.h"
#include "GameClient/GXLookGovernor.h"
#ifdef USE_WWSHADE
#include "wwshade/shdmesh.h"
#include "wwshade/shdsubmesh.h"
#endif


// Global Variables and Functions /////////////////////////////////////////////

W3DVolumetricShadowManager	*TheW3DVolumetricShadowManager=nullptr;
extern const FrustumClass *shadowCameraFrustum;	//defined in W3DShadow.

// GeneralsX: A prefetch is a hint with no architectural effect (same macro as
// AIPathfind.cpp). Used to fetch shadow casters a few entries ahead of the manager's per-frame walk.
#if defined(__GNUC__) || defined(__clang__)
	#define GX_SHADOW_PREFETCH(p) __builtin_prefetch((const void *)(p))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 && (defined(_M_IX86) || defined(_M_X64))
	#include <xmmintrin.h>
	#define GX_SHADOW_PREFETCH(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#else
	#define GX_SHADOW_PREFETCH(p) ((void)0)
#endif
enum { GX_SHADOW_PREFETCH_AHEAD = 8 };

// Dynamic shadow volumes are re-uploaded every draw anyway, so they go up in WORLD space under an identity
// world transform: a per-draw SetTransform(D3DTS_WORLD) makes DXVK rebuild its fixed-function constants (~60% of cost).
// Static volumes too: setShadowWorld() skips a SetTransform whose matrix equals the last one set.
// [GXCENSUS] lk_*: this file's shadow-ring locks, counted by flag (GX_DRAWCENSUS=1).
static inline DWORD gxCenLock(DWORD flags) { DX8Wrapper::Census_Lock(flags); return flags; }
static D3DMATRIX s_shadowWorld;
static Bool s_shadowWorldValid = FALSE;	// FALSE: the device's world transform is unknown (set at every shadow render start)
static void setShadowWorld(LPDIRECT3DDEVICE8 dev, const D3DMATRIX &m)
{
	if (s_shadowWorldValid && memcmp(&m, &s_shadowWorld, sizeof(m)) == 0)
	{
		gxShadowAdd(GXS_WSKIP);
		return;
	}
	gxShadowAdd(GXS_WSET);
	dev->SetTransform(D3DTS_WORLD, &m);
	DX8_CENSUS(DX8C_WORLD_DEV);					// [GXCENSUS] a world set the wrapper never sees:
	DX8Wrapper::Census_Forget_Transforms();		// its next world send is not "the same" as its last
	s_shadowWorld = m;
	s_shadowWorldValid = TRUE;
}
static const D3DMATRIX &shadowIdentityWorld()
{
	static const D3DMATRIX s_identity = To_D3DMATRIX(Matrix3D(true));
	return s_identity;
}

// GeneralsX: a dynamic volume is locked, copied and unlocked into the
// dynamic VB/IB once per frame, not once per stencil pass (the contents are identical). The DECR pass redraws the
// INCR pass's upload when it is still in the buffers: nothing since has DISCARDed either buffer (a DISCARD is the
// only thing that makes an earlier range unavailable; NOOVERWRITE locks only ever append past it). The epoch
// moves on every DISCARD of the vertex buffer and at every shadow render start; 0 is never a live epoch. (The
// DECR pass re-sends the indices itself, so an index-buffer DISCARD does not make an upload unusable.)
// W3DProjectedShadow.cpp also appends to (and may DISCARD) this ring, but only from its own render call, never
// between the two passes below, which both run inside one W3DVolumetricShadowManager::renderShadows.
static UnsignedInt s_shadowUploadEpoch = 1;
static Bool s_shadowReuseUploads = FALSE;	// TRUE only while the DECR pass walks the dynamic tasks
static inline void bumpShadowUploadEpoch() { if (++s_shadowUploadEpoch == 0) s_shadowUploadEpoch = 1; }

// GeneralsX: How big a shadow volume is on screen, in render-target pixels. Set once per
// renderShadows from the camera of the pass that draws the stencil shadows (DoShadows), so the answer follows the zoom AND
// the resolution: a bigger window draws more detail, never less. Perspective only; anything else reads as huge.
extern CameraClass *shadowCamera;	// W3DShadow.cpp DoShadows
static Vector3 s_ssCamPos(0, 0, 0);
static Vector3 s_ssCamFwd(0, 0, -1);
static Real s_ssPxPerUnit = 0.0f;	// pixels per world unit at depth 1; 0 = unknown
static void shadowScreenSizeBegin()
{
	s_ssPxPerUnit = 0.0f;
	if (shadowCamera == nullptr || shadowCamera->Get_Projection_Type() != CameraClass::PERSPECTIVE)
		return;
	Vector2 vmin, vmax, pmin, pmax;
	shadowCamera->Get_View_Plane(vmin, vmax);
	shadowCamera->Get_Viewport(pmin, pmax);
	int w = 0, h = 0, bits = 0;
	bool windowed = true;
	WW3D::Get_Render_Target_Resolution(w, h, bits, windowed);
	const Real planeH = vmax.Y - vmin.Y;
	const Real pxH = (pmax.Y - pmin.Y) * (Real)h;
	if (!(planeH > 0.0f) || !(pxH > 0.0f))
		return;
	const Matrix3D &tm = shadowCamera->Get_Transform();
	s_ssCamPos = tm.Get_Translation();
	s_ssCamFwd = -tm.Get_Z_Vector();	// a W3D camera looks down its own -Z
	s_ssPxPerUnit = pxH / planeH;
}
// Radius in pixels of a world-space sphere; 1e9 when unknown or at/behind the camera plane, so no size rule ever fires there.
static Real shadowScreenRadiusPx(const Vector3 &center, Real radius)
{
	if (s_ssPxPerUnit <= 0.0f)
		return 1.0e9f;
	const Real depth = Vector3::Dot_Product(center - s_ssCamPos, s_ssCamFwd);
	if (!(depth > 1.0f))
		return 1.0e9f;
	return radius * s_ssPxPerUnit / depth;
}

///////////////////////////////////////////////////////////////////////////////
// DEFINITIONS ////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// when the change in angle from the object to the light source
// (represented in degrees in the first number below)
// is large enough the shadow info will be reconstructed
const Real cosAngleToCare = cos ((0.2 * PI) / 180.0);	//1.5 degree difference
// Screen-size level of detail for the stencil volumes (a zoomed-out 6v6 has ~5,000 volumes and ~1,000 silhouette rebuilds per frame). A volume whose bounding sphere (the mesh AND its extruded shadow) is under SHADOW_MIN_SCREEN_RADIUS_PX pixels in radius is neither rebuilt, uploaded nor drawn: its whole shadow fits in a few pixels. A rotating mesh is rebuilt only once the rotation since its last build could move the shadow by about half a pixel (SHADOW_STALE_PX over a lever of two sphere radii), never more than SHADOW_MAX_STALE_RAD.
// Both are pixel rules, so retail zoom and every closer view keep the stock behaviour: a 2 px volume radius at the max zoom-out (~1.7 world units per pixel at 1440p) is ~25 px at the default camera height.
#define SHADOW_MIN_SCREEN_RADIUS_PX	1.5f
#define SHADOW_STALE_PX				0.5f
#define SHADOW_MAX_STALE_RAD		(10.0f * PI / 180.0f)
#define MAX_SILHOUETTE_EDGES	1024	//maximum number of shadov volume sides or edges in silhoutte
#define	SHADOW_EXTRUSION_BUFFER	0.1f		//amount to extend shadow volume beyond what's required to hit ground.
#define AIRBORNE_UNIT_GROUND_DELTA 2.0f
#define MAX_SHADOW_LENGTH_SCALE_FACTOR	1.0f //amount shadow can extend beyond the objects normal bounding sphere
#define MAX_SHADOW_LENGTH_EXTRA_AIRBORNE_SCALE_FACTOR 1.5f	//scales MAX_SHADOW_LENGTH_SCALE_FACTOR a little more for flying units
#define MAX_EXTRUSION_LENGTH (512.0f*MAP_XY_FACTOR)	//maximum length of a shadow extrusion - assumed as 512x512 cell map for now.
#define MAX_SHADOW_EXTRUSION_UNDER_OBJECT_BEFORE_CLAMP	5.0f		//maximum amount that shadow can reach below object base (z-position) before we clamp it's length to reduce artifacts.
#define SHADOW_SAMPLING_INTERVAL (MAP_XY_FACTOR * 2.0f)				//stepsize along ray used to find lowest point on terrain within shadow's reach.
#define OVERHANGING_OBJECT_CLAMP_ANGLE	(80.0f/180.0f*PI)				//for objects that are right on a cliff edge, clamp light angle to cast a nearly vertical shadow.

//#define SV_DEBUG
//#define SV_DEBUG_BOUNDS

struct SHADOW_STATIC_VOLUME_VERTEX	//vertex structure passed to D3D
{
		float x,y,z;
};
#define SHADOW_STATIC_VOLUME_FVF	D3DFVF_XYZ

#ifdef SV_DEBUG	//in debug mode, dynamic shadows are rendered with random diffuse color
	struct SHADOW_DYNAMIC_VOLUME_VERTEX	//vertex structure passed to D3D
	{
			float x,y,z;
			DWORD diffuse;
	};
	#define SHADOW_DYNAMIC_VOLUME_FVF	D3DFVF_XYZ|D3DFVF_DIFFUSE
#else
	typedef struct SHADOW_STATIC_VOLUME_VERTEX	SHADOW_DYNAMIC_VOLUME_VERTEX;
	#define SHADOW_DYNAMIC_VOLUME_FVF	D3DFVF_XYZ
#endif

LPDIRECT3DVERTEXBUFFER8 shadowVertexBufferD3D=nullptr;		///<D3D vertex buffer
LPDIRECT3DINDEXBUFFER8	shadowIndexBufferD3D=nullptr;	///<D3D index buffer
int nShadowVertsInBuf=0;	//model vetices in vertex buffer
int nShadowStartBatchVertex=0;
int nShadowIndicesInBuf=0;	//model vetices in vertex buffer
int nShadowStartBatchIndex=0;
// A DISCARD wipes every earlier upload for the DECR pass's redraw and costs a buffer rename, so the ring is sized for a battle's worth of turning casters per frame: 131072 vertices and 393216 indices (doubled from 65536 / 196608, at which the INCR pass wrapped the ring mid-pass in a big battle: ~175 of ~1030 dynamic volumes a frame lost their upload and the DECR pass transformed and uploaded them again, [GXSHADOW] reup).
// Indices stay 16-bit: each volume is addressed from its own base vertex, and the 16-bit limit is its own test in RenderDynamicMeshVolume, so the ring size does not set the largest volume a caster may have.
int SHADOW_VERTEX_SIZE=131072;
int SHADOW_INDEX_SIZE=393216;

//Rough bounding box around visible portion of the terrain
//useful for quick culling
static Real bcX;
static Real bcY;
static Real bcZ;
static Real beX;
static Real beY;
static Real beZ;

static LPDIRECT3DVERTEXBUFFER8 lastActiveVertexBuffer=nullptr;

/** A simple structure to hold random geometry (vertices, polygons, etc.).  We'll use this
* to store shadow volumes. */
struct Geometry
{
	enum VisibleState {
		STATE_UNKNOWN = CollisionMath::BOTH,
		STATE_VISIBLE = CollisionMath::INSIDE,
		STATE_INVISIBLE = CollisionMath::OUTSIDE,
	};

	Geometry() : m_verts(nullptr),m_indices(nullptr),m_numPolygon(0),m_numVertex(0),m_flags(0),m_worldFilled(FALSE) { m_gpuLight[0]=m_gpuLight[1]=m_gpuLight[2]=m_gpuLight[3]=0.0f; }
	~Geometry() { Release();}

	Int Create( Int numVertices, Int numPolygons )
	{
		if (numVertices)
			if((m_verts=NEW Vector3[numVertices]) == nullptr)
				return FALSE;
		if (numPolygons)
			if((m_indices=NEW UnsignedShort[numPolygons*3]) == nullptr)
				return FALSE;
		m_numPolygon=numPolygons;
		m_numVertex=numVertices;
		m_numActivePolygon=0;
		m_numActiveVertex=0;
		m_worldFilled=FALSE;
		return TRUE;
	}
	void Release()
	{
		delete [] m_verts;
		m_verts=nullptr;

		delete [] m_indices;
		m_indices=nullptr;

		m_numActivePolygon=m_numPolygon=0;
		m_numActiveVertex=m_numVertex=0;
		m_worldFilled=FALSE;
	}
	// GeneralsX: A static volume's slot holds its vertices already transformed by this matrix
	// (W3DVolumetricShadow::RenderMeshVolume); the object-space copy stays in m_verts.
	Bool isWorldFilled(const Matrix3D &xf) const { return m_worldFilled && memcmp(&xf, &m_worldFill, sizeof(xf)) == 0; }
	void setWorldFilled(const Matrix3D &xf) { m_worldFill = xf; m_worldFilled = TRUE; }
	void clearWorldFilled() { m_worldFilled = FALSE; }
	// GeneralsX: A SHADOW_GPU volume keeps no vertices: the light position in object space and
	// the extrusion scale it was last "rebuilt" for, i.e. what constructVolume would have used (see gxShadowGpuFlush).
	void setGpuLight(const Vector3 &lightPosObject, Real extrude) { m_gpuLight[0]=lightPosObject.X; m_gpuLight[1]=lightPosObject.Y; m_gpuLight[2]=lightPosObject.Z; m_gpuLight[3]=extrude; }
	const float *getGpuLight() const { return m_gpuLight; }
	Int GetFlags () { return m_flags;}
	void SetFlags (Int flags) { m_flags = flags;}
	Int GetNumPolygon () { return m_numPolygon;}
	Int GetNumVertex ()	{ return m_numVertex;}
	Int GetNumActivePolygon () { return m_numActivePolygon;}
	Int GetNumActiveVertex ()	{ return m_numActiveVertex;}
	Int SetNumActivePolygon (Int numPolygons) { return m_numActivePolygon=numPolygons;}
	Int SetNumActiveVertex (Int numVertices)	{ return m_numActiveVertex=numVertices;}
	UnsignedShort *GetPolygonIndex (long dwPolyId, short *psIndexList) const
	{
		*psIndexList++ = m_indices[dwPolyId*3];
		*psIndexList++ = m_indices[dwPolyId*3+1];
		*psIndexList++ = m_indices[dwPolyId*3+2];
		return &m_indices[dwPolyId];
	}
	Int SetPolygonIndex (long dwPolyId, short *psIndexList)
	{
		m_indices[dwPolyId*3]=psIndexList[0];
		m_indices[dwPolyId*3+1]=psIndexList[1];
		m_indices[dwPolyId*3+2]=psIndexList[2];
		return 3;
	}
	Vector3 *GetVertex (int dwVertId)
	{
		return &m_verts[dwVertId];
	}
	const Vector3 *SetVertex (int dwVertId, const Vector3 *pvVertex)
	{
		m_verts[dwVertId]=*pvVertex;
		return 	pvVertex;
	}
	///Find a vertex within given range
	Int	FindVertexInRange (Int start, Int end, Vector3 *pvVertex)
	{
		for (Int i=start; i<end; i++)
		{
			if ((m_verts[i]-*pvVertex).Length2() == 0)
				return i;
		}
		return -1;
	}

	AABoxClass &getBoundingBox() {return m_boundingBox;}
	void	setBoundingBox(const AABoxClass &box)	{m_boundingBox=box;}
	void	setBoundingSphere(const SphereClass &sphere) {m_boundingSphere=sphere;}
	SphereClass &getBoundingSphere() {return m_boundingSphere;}
	void	setVisibleState(VisibleState state)	{m_visibleState=state;}
	VisibleState	getVisibleState() {return m_visibleState;}

private:
	Vector3	*m_verts;
	UnsignedShort *m_indices;
	Int m_numPolygon;
	Int m_numVertex;
	Int m_numActivePolygon;	///<number of polygons filled with valid data
	Int m_numActiveVertex;		///<number of vertices filled with valid data
	Int	m_flags;				///<geometry attribute flags - static vs. dynamic, etc.
	AABoxClass m_boundingBox;	///<object space bounding box of shadow volume
	SphereClass m_boundingSphere;	///<object space bounding sphere of shadow volume
	VisibleState	m_visibleState;		///<flag if this geometry was visible in this frame.
	Matrix3D	m_worldFill;			///<static volume: the transform its slot's vertices were filled with
	Bool	m_worldFilled;				///<static volume: m_worldFill is valid
	float	m_gpuLight[4];				///<SHADOW_GPU volume: light position in object space (xyz), extrusion scale (w)
};

// CONST //////////////////////////////////////////////////////////////////////
const Int MAX_POLYGON_NEIGHBORS = 3;  // we use nothing but triangles for
																			// geometry polygons so we have at
																			// most 3 neighbors
const Int NO_NEIGHBOR = -1;  // entry value for neighbor when there isn't one

const Byte POLY_VISIBLE	  = 0x01;  // polygon is visible from light
const Byte POLY_PROCESSED = 0x02;  // this poly has been processed

// STRUCT /////////////////////////////////////////////////////////////////////

// NeighborEdge ---------------------------------------------------------------
typedef struct _NeighborEdge
{

	Short neighborIndex;  // index of polygon who is our neighbor, if there is
												// not a neighbor it contains NO_NEIGHBOR
	Short neighborEdgeIndex[ 2 ];  // the two vertex indices that represent the
																 // shared edge

} NeighborEdge;

// PolygonNeighbor ------------------------------------------------------------
struct  PolyNeighbor
{

	Short myIndex;  // our polygon index so we know who we are
	Byte status;  // status flags used when processing neighbors
	NeighborEdge neighbor[ MAX_POLYGON_NEIGHBORS ];

};

/**This class holds original mesh specific data and geometry.  The meshes stored in this
class have been cleaned to remove replicated vertices and also cache mesh data needed for
faster silhouette computation.  A model can contain many meshes for which we need to store
separate data so they can move relative to each other.*/
// GX_SHADOW_GPU edge-mesh vertex (the GPU path, further below, explains it)
struct GxShadowEdgeVertex
{
	float px, py, pz;	// corner position (object space)
	float nx, ny, nz;	// its face's normal; zero for the partner of a neighbourless edge, which always extrudes
	float rx, ry, rz;	// its face's poly[0] vertex, the point buildSilhouette tests the light against
	float base;			// first constant register of this copy: 9 + 4 * its slot in its window (gxShadowGpuAddCopy)
};
class W3DShadowGeometryMesh
{
	//for the sake of speed, give direct access to classes that need this data.
	friend class W3DShadowGeometry;
	friend class W3DVolumetricShadow;

public:
	W3DShadowGeometryMesh();
#ifdef DO_TERRAIN_SHADOW_VOLUMES
	virtual
#endif
	~W3DShadowGeometryMesh();

	/// @todo: Cache/Store face normals someplace so they are not recomputed when lights move.
	const Vector3& GetPolygonNormal(long dwPolyNormId) const
	{
		WWASSERT(m_polygonNormals);
		return m_polygonNormals[dwPolyNormId];
	}
	int GetNumPolygon () const {return m_numPolygons;}
	/// given loaded geometry this builds the polygon neighbor information
	void buildPolygonNeighbors();
	void buildPolygonNormals()
	{
		if (!m_polygonNormals)
		{	//need to allocate storage
			Vector3 *tempVec = NEW Vector3[m_numPolygons];
			for (int i=0; i<m_numPolygons; i++)
			{
				buildPolygonNormal(i,&tempVec[i]);
			}
			m_polygonNormals = tempVec;
		}
	}
protected:
	Vector3 *buildPolygonNormal (long dwPolyNormId, Vector3 *pvNorm) const
	{
		if (m_polygonNormals)
			return &(*pvNorm=m_polygonNormals[dwPolyNormId]);
		short indexList[3];
//		Vector3 vertexList[3];
		//get vertex indices for this polygon
		GetPolygonIndex(dwPolyNormId,indexList);
		//get the vertices
//		GetVertex(indexList[0],&vertexList[0]);
//		GetVertex(indexList[1],&vertexList[1]);
//		GetVertex(indexList[2],&vertexList[2]);
		const Vector3& v0=GetVertex(indexList[0]);
		const Vector3& v1=GetVertex(indexList[1]);
		const Vector3& v2=GetVertex(indexList[2]);

		//compute triangle normal by crossing 2 edges
		Vector3 edge1=v1-v0;
		Vector3 edge2=v1-v2;
#ifdef ALLOW_TEMPORARIES
		*pvNorm=Vector3::Cross_Product(edge2,edge1);
		pvNorm->Normalize();
#else
		Vector3::Normalized_Cross_Product(edge2,edge1, pvNorm);
#endif
		return pvNorm;
	}

	/// creating and deleting storage for the polygon neighbors
	Bool allocateNeighbors( Int numPolys );
	void deleteNeighbors();

	// geometry shadow data access
	PolyNeighbor *GetPolyNeighbor( Int polyIndex );
	int GetNumVertex ()	const {	return m_numVerts;}
	///Get indices to the 3 vertices of this face.
#ifdef DO_TERRAIN_SHADOW_VOLUMES
	virtual
#endif
	void GetPolygonIndex (long dwPolyId, short *psIndexList) const
	{	const TriIndex *polyi=&m_polygons[dwPolyId];
		*psIndexList++ = m_parentVerts[polyi->I];
		*psIndexList++ = m_parentVerts[polyi->J];
		*psIndexList++ = m_parentVerts[polyi->K];
	}
#ifdef DO_TERRAIN_SHADOW_VOLUMES
	virtual
#endif
	const Vector3& GetVertex (int dwVertId) const
	{
		return m_verts[dwVertId];
	}

	MeshClass *m_mesh;	///< W3D mesh for this geometry
	Int m_meshRobjIndex;	///<index of this mesh within hlod robj
	const Vector3	*m_verts;		///<array of vertices
	Vector3	*m_polygonNormals;	///<array of face normals
	Int m_numVerts;	 ///< number of actual vertices after duplicates are removed.
	Int m_numPolygons; ///<number of polygons in source geometry
	const TriIndex	*m_polygons;	///<array of 3 vertex indices per face
	UnsignedShort *m_parentVerts;	///<array of parent vertex indices for each vertex.
	/// the neighbor info indexed by polygon id
	PolyNeighbor *m_polyNeighbors;
	Int m_numPolyNeighbors;  // length of m_polyNeighbors and the number of polygons
							 // in our current geometry.
	W3DShadowGeometry *m_parentGeometry; // mesh hierarchy containing this mesh.

public:
	// GeneralsX: GX_SHADOW_GPU: this mesh's edge mesh and its copies in the shared store (gxGpuReady).
	Bool gxGpuReady();
	void gxGpuRelease();
	void gxSilhouetteEdge(const PolyNeighbor *visible, Int hiddenIndex, Short *edgeStart, Short *edgeEnd) const;
	const char *gxGpuEdgeMesh(std::vector<GxShadowEdgeVertex> &verts, std::vector<UnsignedShort> &idx);
	std::vector<GxShadowEdgeVertex> m_gpuVerts;	///< the edge mesh (one copy, base unset), kept to add copies
	std::vector<UnsignedShort> m_gpuIdx;
	std::vector<Int> m_gpuCopies;	///< its copies in the shared store; a frame's volume i draws through copy i % size
	Int m_gpuNumVerts;	///< per copy
	Int m_gpuNumTris;	///< per copy
	Int m_gpuDemand;	///< most volumes of this mesh drawn in one frame so far
	Bool m_gpuGrowFailed;	///< the store refused another copy: do not retry every frame
	UnsignedInt m_gpuRefusedGen;	///< refused for want of room or a device: retried once the store is rebuilt (0 = refused for good)
	UnsignedInt m_gpuFrame;	///< the shadow pass m_gpuHead/m_gpuCount belong to
	Int m_gpuHead;		///< this frame's last queued volume (gxShadowGpuFlush), -1 = none
	Int m_gpuCount;		///< this frame's queued volumes
	Int m_gpuState;	///< 0 not built, 1 ready, -1 refused (stays on the CPU path)
};

#ifdef DO_TERRAIN_SHADOW_VOLUMES

//Custom version of W3DShadowGeometryMesh for meshes stored as heightmap
class W3DShadowGeometryHeightmapMesh : public W3DShadowGeometryMesh
{

public:
	virtual int GetPolygonIndex (long dwPolyId, short *psIndexList) const;
	virtual Vector3 *GetVertex (int dwVertId, Vector3 *pvVertex);
	W3DShadowGeometryHeightmapMesh() : m_patchOriginX(0),m_patchOriginY(0) { }
	void setPatchOrigin(Int x, Int y) {m_patchOriginX=x; m_patchOriginY=y;}
	void getPatchOrigin(Int *x, Int *y) {*x=m_patchOriginX; *y=m_patchOriginY;}
	void setPatchSize(Int size)	{m_width=size; m_numPolygons=(size-1)*(size-1)*2;}
	Int getPatchSize()	{return m_width;}

	protected:

		Int m_heightmapPitch;	///<width of full heightmap of which this mesh is a sub-rectangle
		Int m_width;			///<patch width
		Int m_patchOriginX;		///<location of patch within parent heightmap
		Int m_patchOriginY;		///<location of patch within parent heightmap
};

int W3DShadowGeometryHeightmapMesh::GetPolygonIndex (long dwPolyId, short *psIndexList) const
{
	//Find top left vertex of cell containing polygon
	WorldHeightMap *map=nullptr;
	if (TheTerrainRenderObject)
		map=TheTerrainRenderObject->getMap();
	if (!map)
		return 0;

	Int row=dwPolyId/((m_width-1)<<1);
	Int column=(dwPolyId>>1)-row*((m_width-1));

#ifdef FLIP_TRIANGLES
	UnsignedByte alpha[4];
	float UA[4], VA[4];
	Bool flipForBlend;
	map->getAlphaUVData(column+m_patchOriginX, row+m_patchOriginY, UA, VA, alpha, &flipForBlend);
	if (flipForBlend)
	{
		if (dwPolyId &1)
		{	psIndexList[0]=row*m_width+column+1;
			psIndexList[1]=(row+1)*m_width+column+1;
			psIndexList[2]=(row+1)*m_width+column;
		}
		else
		{	psIndexList[0]=row*m_width+column;
			psIndexList[1]=row*m_width+column+1;
			psIndexList[2]=(row+1)*m_width+column;
		}
	}
	else
#endif
	{	if (dwPolyId &1)
		{	psIndexList[0]=row*m_width+column;
			psIndexList[1]=row*m_width+column+1;
			psIndexList[2]=(row+1)*m_width+column+1;
		}
		else
		{	psIndexList[0]=row*m_width+column;
			psIndexList[1]=(row+1)*m_width+column+1;
			psIndexList[2]=(row+1)*m_width+column;
		}
	}

	return 3;
}

Vector3 *W3DShadowGeometryHeightmapMesh::GetVertex (int dwVertId, Vector3 *pvVertex)
{
	WorldHeightMap *map=nullptr;

	if (TheTerrainRenderObject)
		map=TheTerrainRenderObject->getMap();

	if (!map)
		return nullptr;

	Int row=dwVertId/m_width;
	Int column=dwVertId-row*m_width;

	UnsignedByte *data=map->getDataPtr();
	pvVertex->X=(m_patchOriginX+column)*MAP_XY_FACTOR;
	pvVertex->Y=(m_patchOriginY+row)*MAP_XY_FACTOR;
	pvVertex->Z=(Real)data[(m_patchOriginX+column)+(m_patchOriginY+row)*map->getXExtent()]*map->getHeightScale();

	return pvVertex;
}

Bool isPatchShadowed(W3DShadowGeometryHeightmapMesh	*hm_mesh)
{
	WorldHeightMap *map=nullptr;
	Short poly[ 3 ];
	Vector3 vertex;
	Vector3 normal,lightVector;
	Int firstVisible=0;
	Int testVisible;

	if (TheTerrainRenderObject)
		map=TheTerrainRenderObject->getMap();

	if (!map)
		return FALSE;

	hm_mesh->buildPolygonNormal( 0, &normal );

	// get the vertex indices at this polygon
	hm_mesh->GetPolygonIndex( 0, poly, 3 );

	//
	// find out "lightVector" to this polygon
	//
	// since our light source could be very close to the object and that
	// would change the shadow we are going to say that the light vector
	// is from the light position to one of the vertices in the polygon.
	// To be more correct we should use the center of the polygon but
	// this is a good approximation ... an ever broader approximation that
	// we could use would be the object center
	//
	hm_mesh->GetVertex( poly[ 0 ], &vertex );
	lightVector= vertex - LightPosWorld[0];

	//
	// dot the light vector with the normal of the polygon to see if the
	// poly is visible from this location
	//
	if( Vector3::Dot_Product( lightVector, normal ) < 0.0f )
		firstVisible=1;

	for (Int i=1; i<hm_mesh->GetNumPolygon(); i++)
	{
		hm_mesh->buildPolygonNormal( i, &normal );

		// get the vertex indices at this polygon
		hm_mesh->GetPolygonIndex( i, poly, 3 );

		//
		// find out "lightVector" to this polygon
		//
		// since our light source could be very close to the object and that
		// would change the shadow we are going to say that the light vector
		// is from the light position to one of the vertices in the polygon.
		// To be more correct we should use the center of the polygon but
		// this is a good approximation ... an ever broader approximation that
		// we could use would be the object center
		//
		hm_mesh->GetVertex( poly[ 0 ], &vertex );
		lightVector= vertex - LightPosWorld[0];

		//
		// dot the light vector with the normal of the polygon to see if the
		// poly is visible from this location
		//
		testVisible=0;
		if( Vector3::Dot_Product( lightVector, normal ) < 0.0f )
			testVisible=1;

//		if (testVisible ^ firstVisible)
//			return TRUE;	//found polys facing different directions to sun, will cast shadow
		if (!testVisible)
			return TRUE;	//some part of mesh not facing light, so it could cast a shadow
	}
	return FALSE;
}

#define SV_MAX_TERRAIN_MESHES	16

static W3DShadowGeometryHeightmapMesh terrainMeshes[SV_MAX_TERRAIN_MESHES];
static Int numTerrainMeshes=0;

void W3DVolumetricShadowManager::loadTerrainShadows()
{
	WorldHeightMap *map=nullptr;
	Int patchSize=3;

	if (TheTerrainRenderObject)
		map=TheTerrainRenderObject->getMap();

	if (!map)
		return;

	for (Int y=0; y<map->getYExtent(); y += patchSize-1)
	{
		for (Int x=0; x<map->getXExtent(); x += patchSize-1)
		{
			W3DShadowGeometryHeightmapMesh	*hm_mesh=&terrainMeshes[numTerrainMeshes];
			hm_mesh->setPatchOrigin(x,y);
			hm_mesh->setPatchSize(patchSize);

			if(isPatchShadowed(hm_mesh))
			{	//some polygons in this patch cast shadows, need to generate a mesh
//				hm_mesh->buildPolygonNeighbors();
				numTerrainMeshes++;
			}
		}
	}

/*	W3DShadowGeometryHeightmapMesh	hm_mesh;
	short indexList[3];
	Vector3 vertexList[3];
*/
/*	W3DShadow *shadow = NEW W3DShadow;
	// add to our shadow list through the shadow next links
	shadow->m_next = m_shadowList;
	m_shadowList = shadow;
*/
}

#endif //DO_TERRAIN_SHADOW_VOLUMES

/** This class will wrap any shadow casting geometry with additional
data needed for efficient shadow volume generation.  The W3DVolumetricShadowManager
will allocate these structures and hash them for quick re-use on other
models sharing the same geometry.*/
class W3DShadowGeometry : public RefCountClass, public	HashableClass
{

	public:

		W3DShadowGeometry() { };
		virtual ~W3DShadowGeometry() override { };

		virtual	const char * Get_Key() override { return m_namebuf;	}

		Int init (RenderObjClass *robj);
		Int initFromHLOD (RenderObjClass *robj);	///<initialize the geometry from a W3D HLOD object.
		Int initFromMesh (RenderObjClass *robj);///<initialize the geometry from a W3D Mesh object.

		const char *		Get_Name() const	{ return m_namebuf;}
		void				Set_Name(const char *name)
		{
			strlcpy(m_namebuf,name,sizeof(m_namebuf));
		}
		Int					getMeshCount()	{ return m_meshCount;}
		W3DShadowGeometryMesh	*getMesh(Int index)	{ return &m_meshList[index];}


		int GetNumTotalVertex ()	{	return m_numTotalsVerts;}	///<total number of vertices in all meshes of this geometry

	private:

		char m_namebuf[2*W3D_NAME_LEN];	///<name of model hierarchy

		W3DShadowGeometryMesh m_meshList[MAX_SHADOW_CASTER_MESHES]; ///<collection of meshes for this geometry.
		Int m_meshCount;							///<number of meshes in hierarchy
		Int m_numTotalsVerts;						///<number of verts in entire hierarchy
};

#define MAX_SHADOW_VOLUME_VERTS 16384

Int W3DShadowGeometry::initFromHLOD(RenderObjClass *robj)
{
	HLodClass *hlod=(HLodClass *)robj;
	//locations of parent vertices inside the vertex array after duplicate
	//vertices are removed.
	UnsignedShort vertParent[MAX_SHADOW_VOLUME_VERTS];

	Int i,j,k,newVertexCount;

	Int top = hlod->Get_LOD_Count()-1;
	W3DShadowGeometryMesh *geomMesh=&m_meshList[m_meshCount];

	m_numTotalsVerts=0;

	for (i = 0; i < hlod->Get_Lod_Model_Count(top); i++)
	{
		if (hlod->Peek_Lod_Model(top,i) && hlod->Peek_Lod_Model(top,i)->Class_ID() == RenderObjClass::CLASSID_MESH)
		{
			DEBUG_ASSERTCRASH(m_meshCount < MAX_SHADOW_CASTER_MESHES, ("Too many shadow sub-meshes"));

			geomMesh->m_mesh = (MeshClass *)hlod->Peek_Lod_Model(top,i);
			geomMesh->m_meshRobjIndex=i;

//			if (!geomMesh->m_mesh->Peek_Model()->Get_Flag(MeshGeometryClass::CAST_SHADOW))
//				continue; // CNC3 (gth) Only cast shadows from meshes with the shadow flag ENABLED!

			if ((geomMesh->m_mesh->Is_Alpha() || geomMesh->m_mesh->Is_Translucent()) && !geomMesh->m_mesh->Peek_Model()->Get_Flag(MeshGeometryClass::CAST_SHADOW))
				continue; //transparent meshes that don't have forced shadows will not cast volumetric shadows
			if (W3DVolumetricShadowManager::gxShadowDropMesh(geomMesh->m_mesh->Get_Name(), FALSE))
				continue;	// GeneralsX: TEST-ONLY GX_SHADOW_DROP_MESH
			// CNC3 (gth) skin meshes should never cast a volumetric shadow
			if (geomMesh->m_mesh->Peek_Model()->Get_Flag(MeshGeometryClass::SKIN))
				continue;

			MeshModelClass *mm = geomMesh->m_mesh->Peek_Model();
			geomMesh->m_numVerts=mm->Get_Vertex_Count();
			geomMesh->m_verts=mm->Get_Vertex_Array();
			geomMesh->m_numPolygons=mm->Get_Polygon_Count();
			geomMesh->m_polygons=mm->Get_Polygon_Array();

			if (geomMesh->m_numVerts > MAX_SHADOW_VOLUME_VERTS)
				return FALSE;	//too many vertices to process

			//reset index of all vertices
			memset(vertParent,0xffffffff,sizeof(vertParent));
			newVertexCount=geomMesh->m_numVerts;
			//Find all duplicated vertices.
			for (j=0; j<geomMesh->m_numVerts; j++)
			{
				if (vertParent[j] != 0xffff)
					continue;	//this vertex has already been processed

				const Vector3 *v_curr=&geomMesh->m_verts[j];

				for (k=j+1; k<geomMesh->m_numVerts; k++)
				{
					Vector3 len(*v_curr - geomMesh->m_verts[k]);
					if (len.Length2() == 0)
					{	//found duplicate vertex
						vertParent[k]=j;
						newVertexCount--;	//decrease total vertices since duplicate found.
					}
				}
				vertParent[j]=j;	//first instance of new vertex
			}
			geomMesh->m_parentVerts = NEW UnsignedShort[geomMesh->m_numVerts];
			memcpy(geomMesh->m_parentVerts,vertParent,sizeof(UnsignedShort)*geomMesh->m_numVerts);
			geomMesh->m_numVerts=newVertexCount;	//adjust actual vertex count to ignore duplicates
			m_numTotalsVerts += newVertexCount;
			geomMesh->m_parentGeometry = this;

			// build our neighboring polygon information
//			geomMesh->buildPolygonNeighbors();

			geomMesh++;
			m_meshCount++;
		}


#ifdef USE_WWSHADE //(cnc3)(gth) Support for ShaderMeshes!
// I'm coding this as a completely independent block rather than re-factoring the code above
// because it will probably save us pain in future merges.
		if (hlod->Peek_Lod_Model(top,i) && hlod->Peek_Lod_Model(top,i)->Class_ID() == RenderObjClass::CLASSID_SHDMESH)
		{
			DEBUG_ASSERTCRASH(m_meshCount < MAX_SHADOW_CASTER_MESHES, ("Too many shadow sub-meshes"));

			ShdMeshClass * shd_mesh = (ShdMeshClass *)hlod->Peek_Lod_Model(top,i);

			for (int sub_mesh_index=0; sub_mesh_index < shd_mesh->Get_Sub_Mesh_Count(); sub_mesh_index++) {
				ShdSubMeshClass * sub_mesh = shd_mesh->Peek_Sub_Mesh(sub_mesh_index);

				if (!sub_mesh->Get_Flag(MeshGeometryClass::CAST_SHADOW))
					continue; // CNC3 (gth) Only cast shadows from meshes with the shadow flag ENABLED!

				//transparent meshes that don't have forced shadows will not cast volumetric shadows
				if (shd_mesh->Is_Translucent() && !sub_mesh->Get_Flag(MeshGeometryClass::CAST_SHADOW))
					continue;

				// skin meshes should never cast a volumetric shadow
				if (sub_mesh->Get_Flag(MeshGeometryClass::SKIN))
					continue;

				geomMesh->m_mesh = nullptr; //hope this doesn't cause problems!
				geomMesh->m_meshRobjIndex=i;

				// Count the polygons and vertices
				geomMesh->m_numVerts = sub_mesh->Get_Vertex_Count();
				geomMesh->m_numPolygons = sub_mesh->Get_Polygon_Count();

				geomMesh->m_verts=sub_mesh->Get_Vertex_Array();
				geomMesh->m_polygons=sub_mesh->Get_Polygon_Array();

				if (geomMesh->m_numVerts > MAX_SHADOW_VOLUME_VERTS)
					return FALSE;	//too many vertices to process

				//reset index of all vertices
				memset(vertParent,0xffffffff,sizeof(vertParent));
				newVertexCount=geomMesh->m_numVerts;
				//Find all duplicated vertices.
				for (j=0; j<geomMesh->m_numVerts; j++)
				{
					if (vertParent[j] != 0xffff)
						continue;	//this vertex has already been processed

					const Vector3 *v_curr=&geomMesh->m_verts[j];

					for (k=j+1; k<geomMesh->m_numVerts; k++)
					{
						Vector3 len(*v_curr - geomMesh->m_verts[k]);
						if (len.Length2() == 0)
						{	//found duplicate vertex
							vertParent[k]=j;
							newVertexCount--;	//decrease total vertices since duplicate found.
						}
					}
					vertParent[j]=j;	//first instance of new vertex
				}
				geomMesh->m_parentVerts = new UnsignedShort[geomMesh->m_numVerts];
				memcpy(geomMesh->m_parentVerts,vertParent,sizeof(UnsignedShort)*geomMesh->m_numVerts);
				geomMesh->m_numVerts=newVertexCount;	//adjust actual vertex count to ignore duplicates
				m_numTotalsVerts += newVertexCount;
				geomMesh->m_parentGeometry = this;

				// build our neighboring polygon information
//				geomMesh->buildPolygonNeighbors();

				geomMesh++;
				m_meshCount++;

			}
		}
#endif //(cnc3)(gth) Support for ShaderMeshes!

	}

//	for (i = 0; i < AdditionalModels.Count(); i++) {
//		res |= AdditionalModels[i].Model->Cast_Ray(raytest);
//	}

	return m_meshCount != 0;
}

Int W3DShadowGeometry::initFromMesh(RenderObjClass *robj)
{
	//locations of parent vertices inside the vertex array after duplicate
	//vertices are removed.
	UnsignedShort vertParent[MAX_SHADOW_VOLUME_VERTS];

	Int j,k,newVertexCount;
	W3DShadowGeometryMesh *geomMesh=&m_meshList[m_meshCount];

	assert (m_meshCount < MAX_SHADOW_CASTER_MESHES);

	geomMesh->m_mesh = (MeshClass *)robj;
	geomMesh->m_meshRobjIndex = -1;	//robj is the mesh so no index needed.
	if (((geomMesh->m_mesh->Is_Alpha() || geomMesh->m_mesh->Is_Translucent()) && !geomMesh->m_mesh->Peek_Model()->Get_Flag(MeshGeometryClass::CAST_SHADOW)))
		return FALSE; //transparent meshes that don't have forced shadows will not cast volumetric shadows
	if (W3DVolumetricShadowManager::gxShadowDropMesh(geomMesh->m_mesh->Get_Name(), FALSE))
		return FALSE;	// GeneralsX: TEST-ONLY GX_SHADOW_DROP_MESH

	MeshModelClass *mm = geomMesh->m_mesh->Peek_Model();
	geomMesh->m_numVerts=mm->Get_Vertex_Count();
	geomMesh->m_verts=mm->Get_Vertex_Array();
	geomMesh->m_numPolygons=mm->Get_Polygon_Count();
	geomMesh->m_polygons=mm->Get_Polygon_Array();

	if (geomMesh->m_numVerts > MAX_SHADOW_VOLUME_VERTS)
		return FALSE;	//too many vertices to process

	//reset index of all vertices
	memset(vertParent,0xffffffff,sizeof(vertParent));
	newVertexCount=geomMesh->m_numVerts;
	//Find all duplicated vertices.
	for (j=0; j<geomMesh->m_numVerts; j++)
	{
		if (vertParent[j] != 0xffff)
			continue;	//this vertex has already been processed

		const Vector3 *v_curr=&geomMesh->m_verts[j];

		for (k=j+1; k<geomMesh->m_numVerts; k++)
		{
			Vector3 len(*v_curr - geomMesh->m_verts[k]);
			if (len.Length2() == 0)
			{	//found duplicate vertex
				vertParent[k]=j;
				newVertexCount--;	//decrease total vertices since duplicate found.
			}
		}
		vertParent[j]=j;	//first instance of new vertex
	}

	geomMesh->m_parentVerts = NEW UnsignedShort[geomMesh->m_numVerts];
	memcpy(geomMesh->m_parentVerts,vertParent,sizeof(UnsignedShort)*geomMesh->m_numVerts);
	geomMesh->m_numVerts=newVertexCount;	//adjust actual vertex count to ignore duplicates
	geomMesh->m_parentGeometry = this;

	m_meshCount=1;
	m_numTotalsVerts=newVertexCount;

	// build our neighboring polygon information
//	geomMesh->buildPolygonNeighbors();

	return TRUE;
}

Int W3DShadowGeometry::init(RenderObjClass *robj)
{
	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// MEMBER DEFINITIONS /////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// W3DShadowGeometry =============================================================
// ============================================================================
W3DShadowGeometryMesh::W3DShadowGeometryMesh()
{
	// init polygon neighbor information
	m_polyNeighbors = nullptr;
	m_numPolyNeighbors = 0;
	m_parentVerts = nullptr;
	m_polygonNormals = nullptr;
	m_gpuNumVerts = 0;
	m_gpuNumTris = 0;
	m_gpuDemand = 1;
	m_gpuGrowFailed = FALSE;
	m_gpuRefusedGen = 0;
	m_gpuFrame = 0;
	m_gpuHead = -1;
	m_gpuCount = 0;
	m_gpuState = 0;
}

// ~W3DShadowGeometry ============================================================
// ============================================================================
W3DShadowGeometryMesh::~W3DShadowGeometryMesh()
{
	// remove our neighbor list information allocated
	deleteNeighbors();

	delete [] m_parentVerts;
	delete [] m_polygonNormals;
	gxGpuRelease();
}

// GetPolyNeighbor ============================================================
// Return the poly neighbor structure at the given index
// ============================================================================
PolyNeighbor *W3DShadowGeometryMesh::GetPolyNeighbor( Int polyIndex )
{
if (!m_polyNeighbors) {
	buildPolygonNeighbors();
}

	// sanity
	if( polyIndex < 0 || polyIndex >= m_numPolyNeighbors )
	{

//		DBGPRINTF(( "Invalid neighbor index '%d'\n", polyIndex ));
		assert( 0 );
		return nullptr;

	}

	return &m_polyNeighbors[ polyIndex ];

}

// buildPolygonNeighbors ======================================================
// Whenever we set a new geometry we want to build some information about
// the faces in the new geometry so that we can efficiently traverse across
// the surface to neighboring polygons
// ============================================================================
void W3DShadowGeometryMesh::buildPolygonNeighbors()
{
	Int numPolys;
	Int i, j;
	// Jani: Make sure we have polygon normals BEFORE we need them...
	buildPolygonNormals();

	// how many polygons are in our geometry
	numPolys = GetNumPolygon();

	//
	// if there are no polygons for this geometry then we should have no
	// neighbor information
	//
	if( numPolys == 0 )
	{

		//
		// if our geometry has somehow deformed and we used to have polygon
		// neighbor information we should delete it before we bail
		//
		if( m_numPolyNeighbors != 0 )
			deleteNeighbors();

		return;  // nothing to see here people, move along

	}

	//
	// in the event that this geometry can deform on the fly or we are
	// building our neighbor information for the very first time ...
	// if our current geometry has a different number of polygons than
	// we had previously calculated we need to delete and reallocate a
	// new storage space for the neighbor information
	//
	if( numPolys != m_numPolyNeighbors )
	{

		// delete the old neighbor storage
		deleteNeighbors();

		// allocate a new pool for neighbor information
		if( allocateNeighbors( numPolys ) == FALSE )
			return;

	}

	//
	// initialize all polygon neighbor information to none and assign our
	// own reference to myIndex so we know who we are
	//
	for( i = 0; i < m_numPolyNeighbors; i++ )
	{

		// assign our own identification
		m_polyNeighbors[ i ].myIndex = i;

		// assign our neighbors to none
		for( j = 0; j < MAX_POLYGON_NEIGHBORS; j++ )
			m_polyNeighbors[ i ].neighbor[ j ].neighborIndex = NO_NEIGHBOR;

	}

	// Neighbour search: the 2003 version tested every polygon pair (O(n^2)) lazily, so one big building cost
	// 30-52 ms in a frame. Only polygons sharing a vertex index with i can be neighbours, so visit just those,
	// in ascending order with the pair test unchanged: same neighbours, same slots.
	// Used for meshes of 64+ polygons with dense indices and preallocated scratch; otherwise the 2003 scan.
	// The scratch is unique_ptr arrays, not std::vector: a checked-iterator MSVC vector allocates in its noexcept
	// constructor, so a failed allocation would end the process instead of reaching the catch.
	std::unique_ptr<Short[]> polyIndex;
	std::unique_ptr<size_t[]> vertexStart;
	std::unique_ptr<Int[]> vertexPolys, candidates, merged;
	Int indexBase = 0;
	bool useIndex = m_numPolyNeighbors >= 64;
	if( useIndex )
	{
		try
		{
			const size_t numPolys = (size_t)m_numPolyNeighbors;
			polyIndex.reset( new Short[ 3 * numPolys ] );
			for( i = 0; i < m_numPolyNeighbors; i++ )
				GetPolygonIndex( i, &polyIndex[ 3 * (size_t)i ] );
			indexBase = *std::min_element( polyIndex.get(), polyIndex.get() + 3 * numPolys );
			const size_t numIndices = (size_t)( *std::max_element( polyIndex.get(), polyIndex.get() + 3 * numPolys ) - indexBase ) + 1;
			if( numIndices > 3 * numPolys + 1024 )
				useIndex = false;
			else
			{
				vertexStart.reset( new size_t[ numIndices + 1 ]() );	// zeroed: pass 0 counts into it
				std::unique_ptr<size_t[]> fill;
				for( Int pass = 0; pass < 2; pass++ )	// pass 0 counts, pass 1 fills
				{
					if( pass == 1 )
					{
						for( size_t v = 0; v < numIndices; v++ )
							vertexStart[ v + 1 ] += vertexStart[ v ];
						fill.reset( new size_t[ numIndices ] );
						std::copy( vertexStart.get(), vertexStart.get() + numIndices, fill.get() );
						vertexPolys.reset( new Int[ vertexStart[ numIndices ] ] );	// every entry is written below
					}
					for( i = 0; i < m_numPolyNeighbors; i++ )
					{
						const Short *p = &polyIndex[ 3 * (size_t)i ];
						for( Int k = 0; k < 3; k++ )
						{
							if( ( k >= 1 && p[ k ] == p[ 0 ] ) || ( k >= 2 && p[ k ] == p[ 1 ] ) )
								continue;	// an index the polygon repeats is listed once
							const size_t v = (size_t)( p[ k ] - indexBase );
							if( pass == 0 )
								++vertexStart[ v + 1 ];
							else
								vertexPolys[ fill[ v ]++ ] = i;
						}
					}
				}
				candidates.reset( new Int[ numPolys ] );
				merged.reset( new Int[ numPolys ] );
			}
		}
		catch( ... )	// GameMemory.cpp's operator new throws ERROR_OUT_OF_MEMORY (an enum), not std::bad_alloc
		{
			useIndex = false;
		}
	}

	// assign polygon data for each of our polygons
	for( i = 0; i < m_numPolyNeighbors; i++ )
	{
		Short poly[ 3 ];  // vertex indices for this polygon
		Short otherPoly[ 3 ];  // vertex indices for other polygon

		// get the indices of the three triangle points for this polygon
		GetPolygonIndex( i, poly );
		const Vector3& vNorm=GetPolygonNormal(i);

		// find the neighbors of this polygon: every polygon sharing a vertex index with it, ascending (GeneralsX, see vertexPolys).
		// Each vertex's list is already ascending, so a linear merge orders them. When the lists add up to the whole mesh (a vertex
		// most polygons share: a fan) the 2003 scan of every polygon is the cheaper walk and is used as it was, so no mesh costs more
		// than it did.
		size_t listed = 0;
		for( Int k = 0; k < 3 && useIndex; k++ )
		{
			const size_t v = (size_t)( poly[ k ] - indexBase );
			listed += vertexStart[ v + 1 ] - vertexStart[ v ];
		}
		const bool scanAll = !useIndex || listed >= (size_t)m_numPolyNeighbors;
		size_t numCandidates = scanAll ? (size_t)m_numPolyNeighbors : 0;
		for( Int k = 0; k < 3 && !scanAll; k++ )
		{
			const size_t v = (size_t)( poly[ k ] - indexBase );
			const Int *end = std::merge( candidates.get(), candidates.get() + numCandidates, vertexPolys.get() + vertexStart[ v ],
				vertexPolys.get() + vertexStart[ v + 1 ], merged.get() );
			numCandidates = (size_t)( end - merged.get() );
			candidates.swap( merged );
		}
		if( !scanAll )
		{
			numCandidates = (size_t)( std::unique( candidates.get(), candidates.get() + numCandidates ) - candidates.get() );
		}
		for( size_t c = 0; c < numCandidates; c++ )
		{
			j = scanAll ? (Int)c : candidates[ c ];
			Int a, b;
			Int index1, index2;
			Int index1Pos[2]; //positions of shared edge vertices in triangle list. (0,1 or 2)
			Int diff1,diff2;

			// ignore our own polygon
			if( i == j )
				continue;

			// get the vertex index information for this other polygon
			GetPolygonIndex( j, otherPoly );

			//
			// if 2 of the 3 vertex indices are the same then these polygons
			// are neighbors
			//
			//Also check if winding order of vertices on edge is opposite.
			//If vertices are in same order as our polygon, then it's
			//not a valid edge because the neighbor is flipped.

			index1 = -1;
			index2 = -1;
			for( a = 0; a < 3; a++ )
				for( b = 0; b < 3; b++ )
					if( poly[ a ] == otherPoly[ b ] )
					{

						if( index1 == -1 )
						{	index1 = poly[ a ];  // record matching index1
							index1Pos[0]=a;
							index1Pos[1]=b;
						}
						else if( index2 == -1 )
						{	//Check direction of edge in each polygon.  If they are same direction skip it.
							diff1 = a-index1Pos[0];
							diff2 = b-index1Pos[1];
							if ( ((diff1&0x80000000)^((abs(diff1)&2)<<30)) != ((diff2&0x80000000)^((abs(diff2)&2)<<30)))
							{

								const Vector3& vOtherNorm=GetPolygonNormal(j);

								//Check if the 2 polygons face in exactly opposite directions - don't allow this type of neighbor.
								if (fabs(Vector3::Dot_Product(vOtherNorm,vNorm) + 1.0f) <= 0.01f)
									continue;

								index2 = poly[ a ];  // record matching index2
							}
							else
								continue;
						}
						else
						{//This is the same poly facing opposite direction.	//assert( 0 );  // should never match 3 vertices!
							index1=index2=-1;
							continue;
						}
					}
			if( index1 != -1 && index2 != -1  )
			{
				//
				// the polygon j is a neighbor of our polygon i, put the j
				// index into the first free neighbor slot for polygon i
				//
				for( a = 0; a < MAX_POLYGON_NEIGHBORS; a++ )
					if( m_polyNeighbors[ i ].neighbor[ a ].neighborIndex == NO_NEIGHBOR )
					{

						// record the neighbor index
						m_polyNeighbors[ i ].neighbor[ a ].neighborIndex = j;

						// record the sharded edge vertex indices
						m_polyNeighbors[ i ].neighbor[ a ].neighborEdgeIndex[ 0 ] = index1;
						m_polyNeighbors[ i ].neighbor[ a ].neighborEdgeIndex[ 1 ] = index2;

						break;  // exit for a

					}

				//
				// error condition, if our counter a is at the max number
				// of neighbors, which is 3 for a triangle mesh, we did something
				// wrong here because that would mean we found a 4th match!
				//
				if (a == MAX_POLYGON_NEIGHBORS)
				{
//					Vector3 pv[3];
//					char errorText[255];

//					GetVertex (poly[0], &pv[0]);
//					GetVertex (poly[1], &pv[1]);
//					GetVertex (poly[2], &pv[2]);

//					pv[0] = pv[0] + pv[1] + pv[2];
//					pv[0] /= 3.0f;	//find center of polygon

//					sprintf(errorText,"%s: Shadow Polygon with too many neighbors at %f,%f,%f",m_parentGeometry->Get_Name(),pv[0].X,pv[0].Y,pv[0].Z);
//					DEBUG_LOG(("****%s Shadow Polygon with too many neighbors at %f,%f,%f",m_parentGeometry->Get_Name(),pv[0].X,pv[0].Y,pv[0].Z));
//					DEBUG_ASSERTCRASH(a != MAX_POLYGON_NEIGHBORS,(errorText));
				}

			}

		}

	}

}

// allocateNeighbors ==========================================================
// Allocate storage for the polygon neighbors and record its size
// ============================================================================
Bool W3DShadowGeometryMesh::allocateNeighbors( Int numPolys )
{

	// assure we're not re-allocating without deleting
	assert( m_numPolyNeighbors == 0 );
	assert( m_polyNeighbors == nullptr );

	// allocate the list
	m_polyNeighbors = NEW PolyNeighbor[ numPolys ];
	if( m_polyNeighbors == nullptr )
	{

//		DBGPRINTF(( "Unable to allocate polygon neighbors\n" ));
		assert( 0 );
		return FALSE;

	}

	// list is now actually allocated
	m_numPolyNeighbors = numPolys;

	return TRUE;  // success!

}

// deleteNeighbors ============================================================
// Delete all polygon neighbor storage and information
// ============================================================================
void W3DShadowGeometryMesh::deleteNeighbors()
{

	// delete list
	delete [] m_polyNeighbors;
	m_polyNeighbors = nullptr;
	m_numPolyNeighbors = 0;

	// sanity error checking
	assert( m_numPolyNeighbors == 0 );
	assert( m_polyNeighbors == nullptr );

}

// GX_SHADOW_GPU: dynamic shadow volumes extruded by the GPU. Each mesh gets ONE edge mesh, built once; the
// vertex shader extrudes a corner when its face does not see the light, so a "rebuild" only stores L and s.
// Refused (stays on CPU): over 65535 edge-mesh vertices, or asymmetric neighbour data.
// GX_SHADOW_GPU=2 queues but never draws (a measurement arm).

// On by default where it measured a gain: Windows (+12.5..16.5% fps on G2, every run beats every run). macOS stays OFF: an n=3+3 run on G2 was flat (no fps gain, the shadow pass 0.4 ms cheaper; GPU-bound there).
// iOS and Linux have never run it. GX_SHADOW_GPU=0 = off, =1 = on, =2 = the bound arm (a measurement: volumes never drawn).
static Int gxShadowGpuMode()
{
	static const Int s_mode = [] {
#if defined(_WIN32)
		// compatibility graphics: the CPU volumes, the retail path, until the vs.1.1 extrusion is seen drawing right on
		// Windows' own d3d9. GX_SHADOW_GPU still overrides.
		const Int def = DX8Wrapper::Is_Compat_D3D9() ? 0 : 1;
#else
		const Int def = 0;
#endif
		const char *e = getenv("GX_SHADOW_GPU");
		if (e == nullptr)
			return def;
		return (e[0] >= '0' && e[0] <= '2' && e[1] == '\0') ? (Int)(e[0] - '0') : def;
	}();
	return s_mode;
}

// Constants: c0 = (0, 1, 0, 0); c1..c4 = the view matrix's columns; c5..c8 = the projection's columns; for the copy in slot j of
// the drawn window c[9+4j]..c[11+4j] = its volume's Matrix3D rows and c[12+4j] = (L, s), zero when no volume uses it this draw.
enum { GX_SHADOW_GPU_CONST0 = 9, GX_SHADOW_GPU_KMAX = 61 };	// 9 + 4 * 61 = 253 of the 256 vertex-shader constants
static Int gxShadowGpuK()	// most copies of one mesh (GX_SHADOW_GPU_K, default 64); a frame with more of its volumes draws their windows again
{
	static const Int s_k = [] {
		const char *e = getenv("GX_SHADOW_GPU_K");
		const Int k = (e != nullptr) ? atoi(e) : 64;
		return (k < 1) ? (Int)1 : k;
	}();
	return s_k;
}
static Int gxShadowGpuSlots()	// copies per window = volumes per draw: what the constants hold (61), or fewer (GX_SHADOW_GPU_SLOTS; 1 = a draw per volume)
{
	static const Int s_slots = [] {
		Int fit = ((Int)DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps().MaxVertexShaderConst - GX_SHADOW_GPU_CONST0) / 4;
		if (fit > GX_SHADOW_GPU_KMAX)
			fit = GX_SHADOW_GPU_KMAX;
		if (fit < 1)
			fit = 1;
		const char *e = getenv("GX_SHADOW_GPU_SLOTS");
		const Int n = (e != nullptr) ? atoi(e) : fit;
		return (n < 1) ? (Int)1 : (n > fit ? fit : n);
	}();
	return s_slots;
}
static size_t gxShadowGpuBudget()	// bytes the copy store may hold (GX_SHADOW_GPU_MB, default 64)
{
	static const size_t s_bytes = [] {
		const char *e = getenv("GX_SHADOW_GPU_MB");
		const Int mb = (e != nullptr) ? atoi(e) : 64;
		return (size_t)(mb > 0 ? mb : 64) << 20;
	}();
	return s_bytes;
}

// The vertex shader (vs.1.1) and its declaration: GxShadowVolumeVS.h, the one copy the native renderer's catalogue also reads
// (GeneralsX; it was assembled here by macros, the header holds that block's dwords byte for byte).
#include "W3DDevice/GameClient/GxShadowVolumeVS.h"
#include "W3DDevice/GameClient/GXShadowMapHooks.h"   // S7: GxShadowMapActive
#include "W3DDevice/GameClient/GXShadowMapFit.h"     // S7 A39: the own light
#include "W3DDevice/GameClient/GXShadowDropSpec.h"   // S7 A60/A73: TEST-ONLY drop specs

static std::vector<W3DShadowGeometryMesh *> s_gxGpuMeshes;	// meshes holding copies in the store
static size_t s_gxGpuBytes = 0;	// what the store holds (the GX_SHADOW_GPU_MB budget)
static DWORD s_gxGpuShader = 0;
static Int s_gxGpuShaderState = 0;	// 0 not created yet, 1 created, -1 failed (every volume stays on the CPU path)
static UnsignedInt s_gxGpuFrame = 1;	// this shadow pass, for W3DShadowGeometryMesh::m_gpuFrame

// TRUE when dynamic volumes go to the GPU (GX_SHADOW_GPU set, the device can run the shader, the shader exists). Creates the shader
// on first use. A device without hardware vs.1.1 or with fewer than the 13 constants one volume needs (c0..c12) keeps every volume on
// the CPU path, as does a failed CreateVertexShader (a device with only software vertex processing refuses usage 0): no wrong picture.
static Bool gxShadowGpuOn()
{
	if (gxShadowGpuMode() == 0)
		return FALSE;
	if (s_gxGpuShaderState == 0)
	{
		s_gxGpuShaderState = -1;
		LPDIRECT3DDEVICE8 dev = DX8Wrapper::_Get_D3D_Device8();
		const D3DCAPS8 &caps = DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();
		const char *why = nullptr;
		if (dev == nullptr)
			why = "no device";
		else if (caps.VertexShaderVersion < D3DVS_VERSION(1, 1) || caps.MaxVertexShaderConst < GX_SHADOW_GPU_CONST0 + 4)
			why = "the device lacks hardware vs.1.1 or 13 vertex-shader constants";
		DWORD handle = 0;
		if (why == nullptr && (FAILED(dev->CreateVertexShader((const DWORD *)kGxShadowDecl, (const DWORD *)kGxShadowVS, &handle, 0)) || handle == 0))
			why = "CreateVertexShader failed";
		if (why == nullptr)
		{
			s_gxGpuShader = handle;
			s_gxGpuShaderState = 1;
		}
		fprintf(stderr, "[GXSHADOWGPU] mode=%d k=%d slots=%d budget_mb=%d vs=%u.%u consts=%u vertex shader %s%s\n", (int)gxShadowGpuMode(),
			(int)gxShadowGpuK(), (int)gxShadowGpuSlots(), (int)(gxShadowGpuBudget() >> 20), (unsigned)((caps.VertexShaderVersion >> 8) & 0xff),
			(unsigned)(caps.VertexShaderVersion & 0xff), (unsigned)caps.MaxVertexShaderConst,
			why ? "NOT USED, dynamic volumes stay on the CPU path: " : "created", why ? why : "");
	}
	return s_gxGpuShaderState > 0;
}

// The shared edge of visible and its neighbour hiddenIndex, in visible's winding: addSilhouetteEdge's choice, slot-0 fallback included.
void W3DShadowGeometryMesh::gxSilhouetteEdge(const PolyNeighbor *visible, Int hiddenIndex, Short *edgeStart, Short *edgeEnd) const
{
	Int neighborIndex = 0;
	for (Int i = 0; i < MAX_POLYGON_NEIGHBORS; i++)
		if (visible->neighbor[i].neighborIndex == hiddenIndex)
		{
			neighborIndex = i;
			break;
		}
	Short v[3];
	GetPolygonIndex(visible->myIndex, v);
	const Short *e = visible->neighbor[neighborIndex].neighborEdgeIndex;
	if (v[0] != e[0] && v[0] != e[1])
	{	*edgeStart = v[1]; *edgeEnd = v[2]; }
	else if (v[1] != e[0] && v[1] != e[1])
	{	*edgeStart = v[2]; *edgeEnd = v[0]; }
	else
	{	*edgeStart = v[0]; *edgeEnd = v[1]; }
}

// The edge mesh (one copy, base unset): nullptr when this mesh cannot be drawn by the GPU path, else the reason it cannot.
const char *W3DShadowGeometryMesh::gxGpuEdgeMesh(std::vector<GxShadowEdgeVertex> &verts, std::vector<UnsignedShort> &idx)
{
	const Int numPolys = GetNumPolygon();
	if (numPolys <= 0)
		return "no faces";
	if (m_polyNeighbors == nullptr)
		buildPolygonNeighbors();	// also builds the face normals
	if (m_polyNeighbors == nullptr || m_numPolyNeighbors != numPolys || m_polygonNormals == nullptr)
		return "no neighbour data";
	verts.reserve(numPolys * 3);
	idx.reserve(numPolys * 9);
	for (Int i = 0; i < numPolys; i++)
	{
		Short poly[3];
		GetPolygonIndex(i, poly);
		const Vector3 &n = GetPolygonNormal(i);
		const Vector3 &ref = GetVertex(poly[0]);
		for (Int k = 0; k < 3; k++)
		{
			const Vector3 &v = GetVertex(poly[k]);
			const GxShadowEdgeVertex ev = { v.X, v.Y, v.Z, n.X, n.Y, n.Z, ref.X, ref.Y, ref.Z, 0.0f };
			verts.push_back(ev);
		}
	}
	// face i's corner holding vertex index vi (its first, as GetPolygonIndex lists them); -1 if the face lacks it
	struct Corner
	{
		const W3DShadowGeometryMesh *m;
		Int operator()(Int face, Short vi) const
		{
			Short p[3];
			m->GetPolygonIndex(face, p);
			for (Int k = 0; k < 3; k++)
				if (p[k] == vi)
					return face * 3 + k;
			return -1;
		}
	} corner = { this };
	std::vector<char> processed(numPolys, 0);
	for (Int i = 0; i < numPolys; i++)
	{
		const PolyNeighbor *us = &m_polyNeighbors[i];
		Bool freeSlot = FALSE;
		for (Int j = 0; j < MAX_POLYGON_NEIGHBORS; j++)
		{
			const Int n = us->neighbor[j].neighborIndex;
			if (n == NO_NEIGHBOR)
			{
				freeSlot = TRUE;
				continue;
			}
			if (processed[n])
				continue;	// buildSilhouette tested this pair from the other face
			Short p, q, p2, q2;
			gxSilhouetteEdge(us, n, &p, &q);					// i lit, n not
			gxSilhouetteEdge(&m_polyNeighbors[n], i, &p2, &q2);	// n lit, i not
			const Int pA = corner(i, p), qA = corner(i, q), pB = corner(n, p), qB = corner(n, q);
			if (p2 != q || q2 != p || pA < 0 || qA < 0 || pB < 0 || qB < 0)
				return "asymmetric neighbours";
			const UnsignedShort tri[6] = { (UnsignedShort)pA, (UnsignedShort)pB, (UnsignedShort)qA, (UnsignedShort)qA, (UnsignedShort)pB, (UnsignedShort)qB };
			idx.insert(idx.end(), tri, tri + 6);
		}
		if (freeSlot)
		{	// addNeighborlessEdges: this face's edges that no filled slot shares; the partner corners always extrude
			Short poly[3];
			GetPolygonIndex(i, poly);
			for (Int k = 0; k < 3; k++)
			{
				const Int k2 = (k == 2) ? 0 : k + 1;
				Bool shared = FALSE;
				for (Int j = 0; j < MAX_POLYGON_NEIGHBORS && !shared; j++)
				{
					const NeighborEdge &ne = us->neighbor[j];
					shared = ne.neighborIndex != NO_NEIGHBOR &&
						((ne.neighborEdgeIndex[0] == poly[k] && ne.neighborEdgeIndex[1] == poly[k2]) ||
						 (ne.neighborEdgeIndex[1] == poly[k] && ne.neighborEdgeIndex[0] == poly[k2]));
				}
				if (shared)
					continue;
				const Vector3 &a = GetVertex(poly[k]);
				const Vector3 &b = GetVertex(poly[k2]);
				const Int pB = (Int)verts.size();
				const GxShadowEdgeVertex ea = { a.X, a.Y, a.Z, 0, 0, 0, 0, 0, 0, 0 };
				const GxShadowEdgeVertex eb = { b.X, b.Y, b.Z, 0, 0, 0, 0, 0, 0, 0 };
				verts.push_back(ea);
				verts.push_back(eb);
				const UnsignedShort tri[6] = { (UnsignedShort)(i * 3 + k), (UnsignedShort)pB, (UnsignedShort)(i * 3 + k2), (UnsignedShort)(i * 3 + k2), (UnsignedShort)pB, (UnsignedShort)(pB + 1) };
				idx.insert(idx.end(), tri, tri + 6);
			}
		}
		processed[i] = 1;
	}
	if (idx.empty())
		return "no edges";
	if (verts.size() > 65535)
		return "over 65535 vertices";
	return nullptr;
}

// The copy store: pages, each one MANAGED VB of at most 65535 vertices (16-bit indices, absolute within the page) and its IB,
// cut into windows of up to gxShadowGpuSlots() consecutive copies. A copy's vertices carry base 9 + 4 * its slot in the window,
// so a draw takes one whole window, whatever meshes its copies belong to: slots this frame's volumes use get their registers,
// the rest get zeros, and a zero matrix puts all three corners of each triangle on one point (zero area, nothing rasterised).
// Copies are only ever appended; the store is freed when no mesh holds copies any more (device reset, all geometry gone).
enum { GX_SHADOW_GPU_PAGE_VERTS = 65535, GX_SHADOW_GPU_PAGE_INDICES = 3 * 65535 };
struct GxShadowGpuPage { IDirect3DVertexBuffer8 *vb; IDirect3DIndexBuffer8 *ib; Int numVerts; Int numIndices; };
struct GxShadowGpuWindow { Int page; Int firstVertex; Int numVerts; Int firstIndex; Int numIndices; Int numCopies; };
struct GxShadowGpuCopy { Int window; Int slot; };
static std::vector<GxShadowGpuPage> s_gxGpuPages;
static std::vector<GxShadowGpuWindow> s_gxGpuWindows;
static std::vector<GxShadowGpuCopy> s_gxGpuCopies;
static UnsignedInt s_gxGpuStoreGen = 1;	// bumped each time the store is freed: a mesh refused for room or a device tries again

static void gxShadowGpuFreeStore()
{
	for (size_t p = 0; p < s_gxGpuPages.size(); p++)
	{
		s_gxGpuPages[p].vb->Release();
		s_gxGpuPages[p].ib->Release();
	}
	s_gxGpuPages.clear();
	s_gxGpuWindows.clear();
	s_gxGpuCopies.clear();
	s_gxGpuBytes = 0;
	if (++s_gxGpuStoreGen == 0)
		s_gxGpuStoreGen = 1;
}

// Appends one copy of an edge mesh: its copy id, or -1 when the budget or the device refuses.
static Int gxShadowGpuAddCopy(const std::vector<GxShadowEdgeVertex> &verts, const std::vector<UnsignedShort> &idx)
{
	LPDIRECT3DDEVICE8 dev = DX8Wrapper::_Get_D3D_Device8();
	const Int nv = (Int)verts.size();
	const Int ni = (Int)idx.size();
	if (dev == nullptr || nv > GX_SHADOW_GPU_PAGE_VERTS || ni > GX_SHADOW_GPU_PAGE_INDICES)
		return -1;
	if (s_gxGpuPages.empty() || s_gxGpuPages.back().numVerts + nv > GX_SHADOW_GPU_PAGE_VERTS ||
		s_gxGpuPages.back().numIndices + ni > GX_SHADOW_GPU_PAGE_INDICES)
	{
		const UINT vbBytes = GX_SHADOW_GPU_PAGE_VERTS * sizeof(GxShadowEdgeVertex);
		const UINT ibBytes = GX_SHADOW_GPU_PAGE_INDICES * sizeof(UnsignedShort);
		if (s_gxGpuBytes + vbBytes + ibBytes > gxShadowGpuBudget())
			return -1;
		GxShadowGpuPage page = { nullptr, nullptr, 0, 0 };
		if (FAILED(dev->CreateVertexBuffer(vbBytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &page.vb)) ||
			FAILED(dev->CreateIndexBuffer(ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &page.ib)))
		{
			if (page.vb)
				page.vb->Release();
			if (page.ib)
				page.ib->Release();
			return -1;
		}
		s_gxGpuPages.push_back(page);
		s_gxGpuBytes += vbBytes + ibBytes;
	}
	const Int p = (Int)s_gxGpuPages.size() - 1;
	GxShadowGpuPage &page = s_gxGpuPages[p];
	if (s_gxGpuWindows.empty() || s_gxGpuWindows.back().page != p || s_gxGpuWindows.back().numCopies >= gxShadowGpuSlots())
	{
		const GxShadowGpuWindow w = { p, page.numVerts, 0, page.numIndices, 0, 0 };
		s_gxGpuWindows.push_back(w);
	}
	GxShadowGpuWindow &w = s_gxGpuWindows.back();
	BYTE *dst = nullptr;
	if (FAILED(page.vb->Lock(page.numVerts * sizeof(GxShadowEdgeVertex), nv * sizeof(GxShadowEdgeVertex), &dst, 0)) || dst == nullptr)
		return -1;
	GxShadowEdgeVertex *v = (GxShadowEdgeVertex *)dst;
	for (Int i = 0; i < nv; i++)
	{
		v[i] = verts[i];
		v[i].base = (float)(GX_SHADOW_GPU_CONST0 + 4 * w.numCopies);
	}
	page.vb->Unlock();
	dst = nullptr;
	if (FAILED(page.ib->Lock(page.numIndices * sizeof(UnsignedShort), ni * sizeof(UnsignedShort), &dst, 0)) || dst == nullptr)
		return -1;	// the vertices written above lie past the page's count: the next copy overwrites them
	UnsignedShort *x = (UnsignedShort *)dst;
	for (Int i = 0; i < ni; i++)
		x[i] = (UnsignedShort)(idx[i] + page.numVerts);
	page.ib->Unlock();
	const GxShadowGpuCopy c = { (Int)s_gxGpuWindows.size() - 1, w.numCopies };
	page.numVerts += nv;
	page.numIndices += ni;
	w.numVerts += nv;
	w.numIndices += ni;
	w.numCopies++;
	s_gxGpuCopies.push_back(c);
	gxShadowAdd(GXS_GPUBLD);
	return (Int)s_gxGpuCopies.size() - 1;
}

// TRUE when this mesh has copies in the store; builds its edge mesh on first use, and adds copies when last frame's demand
// outgrew them (up to GX_SHADOW_GPU_K, and at most GX_SHADOW_GPU_COPY_VERTS vertices of copies). A mesh refused for its geometry stays
// on the CPU path; one refused for want of room or a device tries again once the store has been rebuilt.
enum { GX_SHADOW_GPU_COPY_VERTS = 8192 };	// a window draw runs the shader over all its copies: keep big meshes from filling windows
static Int gxShadowGpuWant(Int demand, Int numVerts)
{
	Int want = (demand < gxShadowGpuK()) ? demand : gxShadowGpuK();
	if (numVerts > 0 && want > GX_SHADOW_GPU_COPY_VERTS / numVerts)
		want = GX_SHADOW_GPU_COPY_VERTS / numVerts;
	return (want > 1) ? want : 1;
}
Bool W3DShadowGeometryMesh::gxGpuReady()
{
	if (m_gpuState < 0)
	{
		if (m_gpuRefusedGen == 0 || m_gpuRefusedGen == s_gxGpuStoreGen)
			return FALSE;
		m_gpuState = 0;
		m_gpuGrowFailed = FALSE;	// the failure belonged to the old store
	}
	Int want = gxShadowGpuWant(m_gpuDemand, (Int)m_gpuVerts.size());
	if (m_gpuState > 0 && ((Int)m_gpuCopies.size() >= want || m_gpuGrowFailed))
		return TRUE;
	const char *refused = (DX8Wrapper::_Get_D3D_Device8() == nullptr) ? "no device" : nullptr;
	Bool transient = (refused != nullptr);
	if (refused == nullptr && m_gpuVerts.empty())
	{
		refused = gxGpuEdgeMesh(m_gpuVerts, m_gpuIdx);
		want = gxShadowGpuWant(m_gpuDemand, (Int)m_gpuVerts.size());
	}
	const Bool first = m_gpuCopies.empty();
	while (refused == nullptr && (Int)m_gpuCopies.size() < want)
	{
		const Int c = gxShadowGpuAddCopy(m_gpuVerts, m_gpuIdx);
		if (c < 0)
		{
			m_gpuGrowFailed = TRUE;	// the budget or the device: keep the copies we have
			break;
		}
		m_gpuCopies.push_back(c);
	}
	if (refused == nullptr && m_gpuCopies.empty())
	{
		refused = "store full or buffer creation failed";
		transient = TRUE;
	}
	if (first && gxShadowOn())
		fprintf(stderr, "[GXSHADOWGPU] mesh %s#%d faces=%d verts=%d tris=%d copies=%d %s%s\n",
			m_parentGeometry ? m_parentGeometry->Get_Name() : "?", (int)(this - (m_parentGeometry ? m_parentGeometry->getMesh(0) : this)),
			(int)GetNumPolygon(), (int)m_gpuVerts.size(), (int)(m_gpuIdx.size() / 3), (int)m_gpuCopies.size(),
			refused ? "REFUSED: " : "ready", refused ? refused : "");
	if (refused != nullptr)
	{
		std::vector<GxShadowEdgeVertex>().swap(m_gpuVerts);	// clear() keeps the capacity
		std::vector<UnsignedShort>().swap(m_gpuIdx);
		gxShadowAdd(GXS_GPUREF);
		m_gpuState = -1;
		m_gpuRefusedGen = transient ? s_gxGpuStoreGen : 0;
		return FALSE;
	}
	if (first)
	{
		m_gpuNumVerts = (Int)m_gpuVerts.size();
		m_gpuNumTris = (Int)(m_gpuIdx.size() / 3);
		m_gpuState = 1;
		s_gxGpuMeshes.push_back(this);
	}
	return TRUE;
}

// Gives up this mesh's copies (they stay in the store, unused, until it is freed with the last mesh). A refused mesh stays refused.
void W3DShadowGeometryMesh::gxGpuRelease()
{
	m_gpuCopies.clear();
	m_gpuGrowFailed = FALSE;
	if (m_gpuState > 0)
	{
		m_gpuState = 0;
		std::vector<W3DShadowGeometryMesh *>::iterator it = std::find(s_gxGpuMeshes.begin(), s_gxGpuMeshes.end(), this);
		if (it != s_gxGpuMeshes.end())
			s_gxGpuMeshes.erase(it);
		if (s_gxGpuMeshes.empty())
			gxShadowGpuFreeStore();
	}
}

// The GPU volumes of this frame, queued by the INCR pass and drawn by both passes (gxShadowGpuFlush), chained per mesh.
struct GxShadowGpuDraw
{
	const Matrix3D *xform;	// the mesh's transform (stable for the whole shadow pass)
	const float *light;		// Geometry::getGpuLight
	Int next;				// the mesh's previous entry, -1 = none
};
static std::vector<GxShadowGpuDraw> s_gxGpuQueue;
static std::vector<W3DShadowGeometryMesh *> s_gxGpuTouched;	// meshes with entries this frame, in first-use order
static Bool s_gxGpuQueueOpen = FALSE;	// TRUE while the INCR pass walks the casters
// A queued volume's slot for this frame: volume i of a mesh with n copies uses copy i % n in round i / n (a round draws each of
// its windows once). Built by the INCR flush, sorted by round then window, replayed by the DECR flush.
struct GxShadowGpuFill { Int round; Int window; Int slot; Int entry; };
static std::vector<GxShadowGpuFill> s_gxGpuFills;
static Bool s_gxGpuFillsBuilt = FALSE;

static void gxShadowGpuBeginFrame()
{
	s_gxGpuQueue.clear();
	s_gxGpuTouched.clear();
	s_gxGpuFillsBuilt = FALSE;
	if (++s_gxGpuFrame == 0)
		s_gxGpuFrame = 1;
}

static void gxShadowGpuReleaseAll()
{
	while (!s_gxGpuMeshes.empty())
		s_gxGpuMeshes.back()->gxGpuRelease();	// erases itself; the last one frees the store
	gxShadowGpuFreeStore();
	LPDIRECT3DDEVICE8 dev = DX8Wrapper::_Get_D3D_Device8();
	if (s_gxGpuShader != 0 && dev != nullptr)
		dev->DeleteVertexShader(s_gxGpuShader);
	s_gxGpuShader = 0;
	s_gxGpuShaderState = 0;
	gxShadowGpuBeginFrame();
}

// Draws the queued GPU volumes with the current stencil pass state, one draw per window per round, then puts restoreFVF back as
// the vertex shader.
static void gxShadowGpuFlush(LPDIRECT3DDEVICE8 dev, DWORD restoreFVF, Bool countDemand)
{
	if (s_gxGpuTouched.empty() || gxShadowGpuMode() == 2 || s_gxGpuShaderState <= 0)
		return;
	if (!s_gxGpuFillsBuilt)
	{
		s_gxGpuFills.clear();
		for (size_t m = 0; m < s_gxGpuTouched.size(); m++)
		{
			W3DShadowGeometryMesh *mesh = s_gxGpuTouched[m];
			if (countDemand && mesh->m_gpuCount > mesh->m_gpuDemand)
				mesh->m_gpuDemand = mesh->m_gpuCount;	// next frame's gxGpuReady may add copies
			const Int n = (Int)mesh->m_gpuCopies.size();
			Int i = 0;
			for (Int e = mesh->m_gpuHead; e >= 0 && n > 0; e = s_gxGpuQueue[e].next, i++)
			{
				const GxShadowGpuCopy &c = s_gxGpuCopies[mesh->m_gpuCopies[i % n]];
				const GxShadowGpuFill f = { i / n, c.window, c.slot, e };
				s_gxGpuFills.push_back(f);
			}
		}
		// Trade-off: a comparison sort of ~2,500 entries a frame; bucket by window if it ever shows up in [GXSHADOW] upd/incr
		std::sort(s_gxGpuFills.begin(), s_gxGpuFills.end(), [](const GxShadowGpuFill &a, const GxShadowGpuFill &b) {
			return a.round != b.round ? a.round < b.round : a.window < b.window; });
		s_gxGpuFillsBuilt = TRUE;
	}
	D3DMATRIX view, proj;
	dev->GetTransform(D3DTS_VIEW, &view);
	dev->GetTransform(D3DTS_PROJECTION, &proj);
	float projC[4][4];
	GxShadowGpuProjConstants(W3DWorldSeam::worldPass(), proj, projC);	// + the world pass's jitter (GxShadowGpuProj.h)
	const float c[GX_SHADOW_GPU_CONST0][4] =
	{
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ view._11, view._21, view._31, view._41 }, { view._12, view._22, view._32, view._42 },
		{ view._13, view._23, view._33, view._43 }, { view._14, view._24, view._34, view._44 },
		{ projC[0][0], projC[0][1], projC[0][2], projC[0][3] }, { projC[1][0], projC[1][1], projC[1][2], projC[1][3] },
		{ projC[2][0], projC[2][1], projC[2][2], projC[2][3] }, { projC[3][0], projC[3][1], projC[3][2], projC[3][3] },
	};
	dev->SetVertexShader(s_gxGpuShader);
	dev->SetVertexShaderConstant(0, c, GX_SHADOW_GPU_CONST0);
	static float block[GX_SHADOW_GPU_KMAX][4][4];
	Int page = -1;
	for (size_t f = 0; f < s_gxGpuFills.size(); )
	{
		const Int round = s_gxGpuFills[f].round;
		const Int win = s_gxGpuFills[f].window;
		const GxShadowGpuWindow &w = s_gxGpuWindows[win];
		memset(block, 0, sizeof(block[0]) * w.numCopies);
		for (; f < s_gxGpuFills.size() && s_gxGpuFills[f].round == round && s_gxGpuFills[f].window == win; f++)
		{
			const GxShadowGpuDraw &d = s_gxGpuQueue[s_gxGpuFills[f].entry];
			memcpy(block[s_gxGpuFills[f].slot], d.xform, sizeof(float) * 12);	// Matrix3D = three Vector4 rows
			memcpy(block[s_gxGpuFills[f].slot][3], d.light, sizeof(float) * 4);
		}
		if (w.page != page)
		{
			page = w.page;
			dev->SetStreamSource(0, s_gxGpuPages[page].vb, sizeof(GxShadowEdgeVertex));
			dev->SetIndices(s_gxGpuPages[page].ib, 0);
			lastActiveVertexBuffer = s_gxGpuPages[page].vb;
			gxShadowAdd(GXS_GPUSW);
		}
		dev->SetVertexShaderConstant(GX_SHADOW_GPU_CONST0, block, 4 * w.numCopies);
		if (DX8Wrapper::_Is_Triangle_Draw_Enabled())
		{
			Debug_Statistics::Record_DX8_Polys_And_Vertices(w.numIndices / 3, w.numVerts, ShaderClass::_PresetOpaqueShader);
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, w.firstVertex, w.numVerts, w.firstIndex, w.numIndices / 3);
			DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc=
			gxShadowAdd(GXS_GPUDRAW);
		}
	}
	dev->SetVertexShader(restoreFVF);
}

void W3DVolumetricShadow::RenderGpuMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform)
{
	if (!s_gxGpuQueueOpen)
		return;	// the DECR pass redraws the queue (gxShadowGpuFlush)
	Geometry *geometry = m_shadowVolume[lightIndex][meshIndex];
	W3DShadowGeometryMesh *geomMesh = m_geometry->getMesh(meshIndex);
	// The shader too is re-created here, not only by a rebuild: after a device reset a volume whose pose and light did not change
	// is never rebuilt. Nothing to draw it with (the shader, the store or this mesh refused - e.g. after a
	// SetGeometry swap): marked stale, so the next update rebuilds it, which puts it back on the CPU path.
	if (geometry == nullptr || !gxShadowGpuOn() || !geomMesh->gxGpuReady())
	{
		if (geometry != nullptr)
			geometry->SetFlags(geometry->GetFlags() | SHADOW_GPU_STALE);
		return;
	}
	if (geomMesh->m_gpuFrame != s_gxGpuFrame)
	{
		geomMesh->m_gpuFrame = s_gxGpuFrame;
		geomMesh->m_gpuHead = -1;
		geomMesh->m_gpuCount = 0;
		s_gxGpuTouched.push_back(geomMesh);
	}
	const GxShadowGpuDraw d = { meshXform, geometry->getGpuLight(), geomMesh->m_gpuHead };
	geomMesh->m_gpuHead = (Int)s_gxGpuQueue.size();
	geomMesh->m_gpuCount++;
	s_gxGpuQueue.push_back(d);
	gxShadowAdd(GXS_GPUV);
}

//#include "Common/ThingTemplate.h"

// updateOptimalExtrusionPadding ==============================================
// Use raycasting to figure out a shadow extrusion length that guarantees that
// the highest point of the object is extruded long enough to hit some ground.
// This is a very slow operation so only do once for static non-moving objects.
// ============================================================================
void W3DVolumetricShadow::updateOptimalExtrusionPadding()
{
	if (m_robj)
	{
//		DrawableInfo *drawInfo=(DrawableInfo *)m_robj->Get_User_Data();
//		Drawable *draw = drawInfo->m_drawable;

//		if (strstr(draw->getTemplate()->getName().str(),"Right02") != 0)
//			draw = draw;	//debug code for China06 wacky bridge shadow

		// get the light
		Vector3 lightPosWorld=TheW3DShadowManager->getLightPosWorld(0);

		// check if object has a limit/clamp on shadow length and adjust light
		// position of necessary.
		if (m_shadowLengthScale)
		{	//Find light's distance from origin in xy plane
			Real lightXYDistance = sqrt(lightPosWorld.X*lightPosWorld.X + lightPosWorld.Y * lightPosWorld.Y);
			Real newZ=lightXYDistance*m_shadowLengthScale;

			if (newZ > lightPosWorld.Z)
			{	//clamped z component is higher than actual light position allows so adjust it.
				lightPosWorld.Z = newZ;
			}
		}

		// find maximum shadow length which will not cause any corners of the object's bounding box
		// to cast shadows that drop significantly below the object's base.  This will help avoid
		// artifacts when we have an object on a cliff/hill casting shadows onto the ground below.
		// We need this hack because the terrain does not cast shadows and looks weird when objects
		//	sitting on an incline cast shadows down below.
		Vector3 objPos=m_robj->Get_Position();
		Vector3 lastValidTerrainPoint = objPos;
		Real baseGroundHeight=objPos.Z;
		const AABoxClass &box=m_robj->Get_Bounding_Box();
		Vector3 lightRay,shadowRay;
		LineSegClass lineseg;
		CastResultStruct result;
		Vector3 Corners[4];

		RayCollisionTestClass raytest(lineseg,&result);

		//Get vertices of top of bounding box since they will generate the longest shadow
		Corners[0]=box.Center+box.Extent;	//top right corner
		Corners[1]=Corners[0];
		Corners[1].X -= 2.0f*box.Extent.X;		//top left corner
		Corners[2]=Corners[1];
		Corners[2].Y -= 2.0f*box.Extent.Y;		//bottom left corner
		Corners[3]=Corners[2];
		Corners[3].X += 2.0f*box.Extent.X;		//bottom right corner

		//find the corner that causes the longest shadow projection
		//and clamp light position to make sure it falls on even ground about
		//the same height as the object's base.
		for (Int i=0; i<4; i++)
		{
			//Cast ray from top volume corners onto ground plane
			lightRay = Corners[i] - lightPosWorld;	//vector light to corner
			lightRay.Normalize();

			raytest.Ray.Set(Corners[i],Corners[i]+lightRay*MAX_EXTRUSION_LENGTH);
			result.Reset();

			//find out where this ray intersects terrain.
			if (TheTerrainRenderObject->Cast_Ray(raytest) && !raytest.Result->StartBad)
			{	//Found intersection point where shadow has its maximum length.  Do a quick
				//search to see if terrain falls significantly below the height of the object
				//anywhere between the base and the intersection point.  If so, we either need
				//to extend shadow extrusion or make the light angle more vertical.

				shadowRay.Set(result.ContactPoint-Corners[i]);	//vector from object corner to terrain intersection.
				shadowRay.Z = 0;	//remove z-component since we'll be sampling along the xy plane.

				//walk along the shadow/light direction vector looking for large dips - indicating object
				//is on hill or cliff.
				Real len=shadowRay.Length();
				Int  numSteps=REAL_TO_INT_CEIL(len/SHADOW_SAMPLING_INTERVAL);
				Real stepSize = 1.0f/(Real)numSteps;
				Vector3 terrainPoint;

				Real t=stepSize;
				for (Int j=0; j<numSteps; j++)
				{
					terrainPoint = Corners[i] + shadowRay*t;
					terrainPoint.Z=0;	//ignore height

					Real terrainHeight=TheTerrainRenderObject->getHeightMapHeight(terrainPoint.X,terrainPoint.Y,nullptr);
					if (terrainHeight < (objPos.Z - MAX_SHADOW_EXTRUSION_UNDER_OBJECT_BEFORE_CLAMP))	//check if terrain dips more than 10 units under object.
					{
						if (j == 0)	//this is the initial point so object must be right on the edge of a cliff.
						{	baseGroundHeight = terrainHeight;	//force extrusion all the way down cliff.
							Real tanAngle=tan(OVERHANGING_OBJECT_CLAMP_ANGLE);	//clamp to about 89 degrees or close to vertical lightpos.
							setShadowLengthScale(tanAngle);	//update the clamp angle to shorted shadow enough so this corner on flat ground.
							break;
						}

						//Find ray from last valid terrain contact point to object box corner.
						Vector3 clampRay(Corners[i]-lastValidTerrainPoint);
						Real clampAngle=asin(clampRay.Z/clampRay.Length());
						if (clampAngle >= (PI/2.0f) || clampAngle <= 0)
							clampAngle = OVERHANGING_OBJECT_CLAMP_ANGLE;	//clamp to about 89 degrees or close to vertical lightpos.
						Real tanAngle=tan(clampAngle);
						if (tanAngle > m_shadowLengthScale)
							setShadowLengthScale(tanAngle);	//update the clamp angle to shorted shadow enough so this corner on flat ground.
						break;
					}

					if (terrainHeight < baseGroundHeight)
					{	baseGroundHeight = terrainHeight;	//point was below object but within safety margin so record it's position.
						lastValidTerrainPoint = terrainPoint;
						lastValidTerrainPoint.Z = baseGroundHeight;
					}

					t+=stepSize;
				}
			}
		}


		m_extraExtrusionPadding = objPos.Z - baseGroundHeight + SHADOW_EXTRUSION_BUFFER;

		DEBUG_ASSERTCRASH(m_extraExtrusionPadding <= (255.0f*MAP_HEIGHT_SCALE*MAP_HEIGHT_SCALE_K_MAX),("Warning: Volumetric Shadow UpdateOptimalExtrusionPadding too large"));
	}
}

// getRenderCost ============================================================
// Returns number of draw calls for this shadow.
// ============================================================================
#if defined(RTS_DEBUG)
void W3DVolumetricShadow::getRenderCost(RenderCost & rc) const
{
	Int drawCount = 0;

	if (m_geometry && m_isEnabled && !m_isInvisibleEnabled && TheGlobalData->m_useShadowVolumes)
	{
		Int i,j;

		HLodClass *hlod=(HLodClass *)m_robj;
		MeshClass *mesh;
		Int meshIndex;

		for( i = 0; i < MAX_SHADOW_LIGHTS; i++ )
		{
			for (j=0; j<m_geometry->getMeshCount(); j++)
			{
				meshIndex=m_geometry->getMesh(j)->m_meshRobjIndex;

				if (meshIndex >= 0)
					mesh = (MeshClass *)hlod->Peek_Lod_Model(0,meshIndex);
				else
					mesh = (MeshClass *)m_robj;

				if (mesh && mesh->Is_Not_Hidden_At_All())
						drawCount++;
			}
		}
	}

	rc.addShadowDrawCalls(drawCount*2);
}
#endif

/************************************ New Buffered Rendering Code ************************/
void W3DVolumetricShadow::RenderVolume(Int meshIndex, Int lightIndex)
{
	HLodClass *hlod=(HLodClass *)m_robj;
	MeshClass *mesh=nullptr;

	Int meshRobjIndex=m_geometry->getMesh(meshIndex)->m_meshRobjIndex;

	if (meshRobjIndex >= 0)
		mesh = (MeshClass *)hlod->Peek_Lod_Model(0,meshRobjIndex);
	else
		mesh = (MeshClass *)m_robj;

	if (mesh)
	{
#ifdef SV_DEBUG_BOUNDS
			RenderMeshVolumeBounds(meshIndex,lightIndex, &mesh->Get_Transform());
#endif
			if (m_shadowVolume[0][ meshIndex ]->GetFlags() & SHADOW_GPU)
				RenderGpuMeshVolume(meshIndex,lightIndex,&mesh->Get_Transform());	// (GX_SHADOW_GPU)
			else if (m_shadowVolume[0][ meshIndex ]->GetFlags() & SHADOW_DYNAMIC)
				RenderDynamicMeshVolume(meshIndex,lightIndex,&mesh->Get_Transform());
			else
				RenderMeshVolume(meshIndex,lightIndex,&mesh->Get_Transform());
	}
}

// A static volume's slot holds WORLD-space vertices, drawn under the identity world: setting each mesh
// matrix per draw (setShadowWorld) makes DXVK rebuild and re-upload fixed-function constants on every change.
// fillStaticShadowSlot refills the slot only when the transform changed, in Update(), so DXVK uploads each
// dirty range once. GX_SHADOW_OBJSPACE=1 keeps the old way for rollback.
static Bool shadowObjSpace()
{
	static const char *s_env = getenv("GX_SHADOW_OBJSPACE");
	static const Bool s_on = s_env != nullptr && s_env[0] == '1';
	return s_on;
}
static const Matrix3D &shadowIdentityMatrix()
{
	static const Matrix3D s_identity(true);
	return s_identity;
}
// TRUE when the slot holds this volume's vertices for meshXform (world space), or in object space with GX_SHADOW_OBJSPACE=1.
static Bool fillStaticShadowSlot(Geometry *geometry, W3DBufferManager::W3DVertexBufferSlot *vbSlot, const Matrix3D &meshXform)
{
	const Matrix3D &fill = shadowObjSpace() ? shadowIdentityMatrix() : meshXform;
	if (geometry->isWorldFilled(fill))
		return TRUE;
	const Int numVerts = geometry->GetNumActiveVertex();
	const VertexFormatXYZ *src = (const VertexFormatXYZ *)geometry->GetVertex(0);
	if (numVerts <= 0 || src == nullptr || numVerts > vbSlot->m_size)
		return FALSE;
	{
		DX8VertexBufferClass::AppendLockClass lockVtxBuffer(vbSlot->m_VB->m_DX8VertexBuffer, vbSlot->m_start, numVerts);
		VertexFormatXYZ *dst = (VertexFormatXYZ *)lockVtxBuffer.Get_Vertex_Array();
		if (dst == nullptr)
			return FALSE;
		if (shadowObjSpace())
			memcpy(dst, src, numVerts * sizeof(VertexFormatXYZ));
		else
			for (Int i = 0; i < numVerts; i++)
			{
				Vector3 w;
				Matrix3D::Transform_Vector(fill, *(const Vector3 *)&src[i], &w);
				dst[i] = *(const VertexFormatXYZ *)&w;
			}
	}
	geometry->setWorldFilled(fill);
	gxShadowAdd(GXS_SREFILL);
	return TRUE;
}

void W3DVolumetricShadow::RenderMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform)
{
	Geometry *geometry;
	Int numVerts, numPolys, numIndex;

	//Get D3D Device used by W3D for quicker access.
	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	if (!m_pDev)
		return;

	geometry = m_shadowVolume[lightIndex][ meshIndex ];

	//
	// if our count is out of sync with our geometry data something
	// is wrong here
	//
	assert( geometry );

	// get geometry requirements
	numVerts = geometry->GetNumActiveVertex();
	numPolys = geometry->GetNumActivePolygon();
	numIndex = numPolys * 3;

	// reject shadows with no data
	if( numVerts == 0 || numPolys == 0 )
		return;

	W3DBufferManager::W3DVertexBufferSlot *vbSlot=m_shadowVolumeVB[lightIndex][ meshIndex ];
	if (!vbSlot)
		return;

	// GeneralsX: The slot holds world-space vertices (fillStaticShadowSlot, when the volume was
	// queued); a slot not filled for this transform draws nothing. Never fill here: an upload into a buffer this frame's
	// command list has already drawn from makes DXVK end and restart the render pass (DxvkContext::copyBuffer).
	if (!geometry->isWorldFilled(shadowObjSpace() ? shadowIdentityMatrix() : *meshXform))
		return;
	// GeneralsX: Align mesh world transform with Generals base shadow path.
	if (shadowObjSpace())
		setShadowWorld(m_pDev, To_D3DMATRIX(*meshXform));
	else
		setShadowWorld(m_pDev, shadowIdentityWorld());
	if (vbSlot->m_VB->m_DX8VertexBuffer->Get_DX8_Vertex_Buffer() != lastActiveVertexBuffer)
	{	lastActiveVertexBuffer=vbSlot->m_VB->m_DX8VertexBuffer->Get_DX8_Vertex_Buffer();
		DX8_CENSUS(DX8C_SSTAT_VBSW);	// [GXCENSUS] roadmap 2b: the static draws' VB spread
		m_pDev->SetStreamSource(0,lastActiveVertexBuffer,
			vbSlot->m_VB->m_DX8VertexBuffer->FVF_Info().Get_FVF_Size());	//12 bytes per vertex.
	}

	DEBUG_ASSERTCRASH(vbSlot->m_size >= numVerts,("Overflowing Shadow Vertex Buffer Slot"));

	W3DBufferManager::W3DIndexBufferSlot *ibSlot=m_shadowVolumeIB[lightIndex][ meshIndex ];
	if (!ibSlot)
		return;

	DEBUG_ASSERTCRASH(ibSlot->m_size >= numIndex,("Overflowing Shadow Index Buffer Slot"));

	m_pDev->SetIndices(ibSlot->m_IB->m_DX8IndexBuffer->Get_DX8_Index_Buffer(),vbSlot->m_start);

	if (DX8Wrapper::_Is_Triangle_Draw_Enabled())
	{
		Debug_Statistics::Record_DX8_Polys_And_Vertices(numPolys,numVerts,ShaderClass::_PresetOpaqueShader);
		m_pDev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,numVerts,ibSlot->m_start,numPolys);
		DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc=
		DX8_CENSUS(DX8C_SSTAT_DRAW);
	}

}

// GeneralsX: Dynamic (animated) volumes, batched. They are uploaded in world space under
// an identity world (above), so nothing but the index base told one volume's draw from the next. The INCR pass now
// appends each volume to the ring with its indices rebased to the open batch's first vertex and draws the batch once
// (a zoomed-out 6v6: ~3,300 volume draws a frame became a handful). The DECR pass walks the volumes in the other order
// (the dynamic list is newest caster first), so it re-sends only their INDICES, in its own order, rebased to one base
// vertex per run: the vertices are still in the ring from the INCR pass, and at most three runs cover the ring. The triangles reach the device in exactly the old order (draws are only merged, never reordered), so the
// stencil result is the same bit for bit. A batch spans at most 65535 vertices, so a rebased index stays <= 0xFFFE
// (Metal's restart index 0xFFFF is never written), and it is drawn before anything that could invalidate it: a DISCARD
// of either ring buffer, the end of the pass, any other draw.
static Bool s_shadowBatching = FALSE;	// TRUE only while the INCR pass walks the casters
static Int s_batchBase = -1;			// open batch: first vertex (its indices are relative to it); -1 = none
static Int s_batchStartIndex = 0;
static Int s_batchNumIndices = 0;
static Int s_batchVertEnd = 0;			// one past the last vertex of the open batch
static Int s_runBase = -1;				// DECR pass: the pending run of reused volumes; -1 = none
static Int s_runStartIndex = 0;			// its indices, re-sent in this pass's order (see RenderDynamicMeshVolume)
static Int s_runNumIndices = 0;
static Int s_runMinIndex = 0;			// lowest vertex it references, relative to s_runBase
static Int s_runVertEnd = 0;			// one past the run's last vertex, relative to s_runBase

static void drawShadowRange(LPDIRECT3DDEVICE8 dev, Int base, Int minIndex, Int numVerts, Int startIndex, Int numIndices)
{
	if (numIndices <= 0)
		return;
	dev->SetIndices(shadowIndexBufferD3D, base);
	setShadowWorld(dev, shadowIdentityWorld());
	if (shadowVertexBufferD3D != lastActiveVertexBuffer)
	{
		dev->SetStreamSource(0, shadowVertexBufferD3D, sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX));
		lastActiveVertexBuffer = shadowVertexBufferD3D;
	}
	if (DX8Wrapper::_Is_Triangle_Draw_Enabled())
	{
		Debug_Statistics::Record_DX8_Polys_And_Vertices(numIndices / 3, numVerts, ShaderClass::_PresetOpaqueShader);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, minIndex, numVerts, startIndex, numIndices / 3);
		DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc=
	}
}

static void flushShadowBatch(LPDIRECT3DDEVICE8 dev)
{
	if (s_batchBase >= 0)
		drawShadowRange(dev, s_batchBase, 0, s_batchVertEnd - s_batchBase, s_batchStartIndex, s_batchNumIndices);
	s_batchBase = -1;
	s_batchNumIndices = 0;
}

static void flushShadowRun(LPDIRECT3DDEVICE8 dev)
{
	if (s_runBase >= 0)
		drawShadowRange(dev, s_runBase, s_runMinIndex, s_runVertEnd - s_runMinIndex, s_runStartIndex, s_runNumIndices);
	s_runBase = -1;
	s_runNumIndices = 0;
}

void W3DVolumetricShadow::RenderDynamicMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform)
{
	Geometry *geometry;
	Int numVerts, numPolys, numIndex;
	SHADOW_DYNAMIC_VOLUME_VERTEX* pvVertices;
	UnsignedShort *pvIndices;

	//Get D3D Device used by W3D for quicker access.
	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	if (!m_pDev)
		return;


	geometry = m_shadowVolume[lightIndex][ meshIndex ];

	//
	// if our count is out of sync with our geometry data something
	// is wrong here
	//
	assert( geometry );

	// get geometry requirements
	numVerts = geometry->GetNumActiveVertex();
	numPolys = geometry->GetNumActivePolygon();
	numIndex = numPolys * 3;

	// reject shadows with no data
	if( numVerts == 0 || numPolys == 0 )
		return;

	// GeneralsX: Skip a volume larger than the whole dynamic VB/IB. The room tests
	// below go negative for it, and the DISCARD lock then maps numVerts*stride / numIndex*2 bytes of a
	// SHADOW_VERTEX_SIZE / SHADOW_INDEX_SIZE buffer, so the memcpy wrote past it. Same outcome TSH chose
	// for oversized static volumes (W3DBufferManager getSlot): that mesh casts no shadow.
	// GeneralsX: >= on the vertex count: a volume of exactly 65536 vertices would use
	// index 0xFFFF. Metal keeps primitive restart always on (MoltenVK cannot disable it), so that index can
	// drop a triangle on macOS/iPad. One vertex less of headroom costs nothing.
	// 65536: the 16-bit base-relative index limit and the Metal 0xFFFF restart index above, whatever the ring size.
	if( numVerts >= 65536 || numVerts >= SHADOW_VERTEX_SIZE || numIndex > SHADOW_INDEX_SIZE )
		return;

	// GeneralsX: TS-06: in the DECR pass, draw the INCR pass's upload again if it is
	// still in the buffers. The geometry cannot change between the passes (only static volumes are drawn between).
	// GeneralsX: ...as part of one range with the reused uploads right before it when they
	// sit back to back in the index ring under the same base (flushShadowRun draws it).
	W3DVolumetricShadowRenderTask &uploadTask = m_shadowVolumeRenderTask[lightIndex][meshIndex];
	if (s_shadowReuseUploads && uploadTask.m_uploadEpoch == s_shadowUploadEpoch)
	{
		gxShadowAdd(GXS_REUSE);
		const Int vtx = uploadTask.m_uploadVertex;	// its vertices, still in the ring since the INCR pass
		// A run's base must be <= every vertex it references, with every index <= 0xFFFE above it. The base is chosen low
		// enough to also take the volumes still to come in this pass, which sit LOWER in the ring (older casters): 0 when the
		// volume ends inside the first 65535 vertices, else the start of the ring's top 65535 vertices, else its own vertex.
		if (s_runBase < 0 || vtx < s_runBase || vtx + numVerts - s_runBase > 65535)
		{
			flushShadowRun(m_pDev);
			Int top = nShadowVertsInBuf - 65535;
			s_runBase = (vtx + numVerts <= 65535) ? 0 : (top > vtx ? vtx : (top > 0 ? top : 0));
			s_runMinIndex = vtx - s_runBase;
			s_runVertEnd = s_runMinIndex + numVerts;
		}
		if (nShadowIndicesInBuf > (SHADOW_INDEX_SIZE-numIndex))
		{	// the index ring is full: draw what the run has, then start the ring over (the vertices stay)
			const Int base = s_runBase;
			flushShadowRun(m_pDev);
			s_runBase = base;
			s_runMinIndex = vtx - s_runBase;
			s_runVertEnd = s_runMinIndex + numVerts;
			if (shadowIndexBufferD3D->Lock(0,numIndex*sizeof(short),(unsigned char**)&pvIndices, gxCenLock(D3DLOCK_DISCARD)) != D3D_OK)
				return;
			nShadowIndicesInBuf=0;
			nShadowStartBatchIndex=0;
			gxShadowAdd(GXS_DISC);
		}
		else if (shadowIndexBufferD3D->Lock(nShadowIndicesInBuf*sizeof(short),numIndex*sizeof(short), (unsigned char**)&pvIndices, gxCenLock(D3DLOCK_NOOVERWRITE)) != D3D_OK)
			return;
		if (pvIndices)
		{
			const UnsignedShort *srcIndex = geometry->GetPolygonIndex(0,(short *)pvIndices);
			const UnsignedShort rebase = (UnsignedShort)(vtx - s_runBase);
			for (Int i=0; i<numIndex; i++)
				pvIndices[i] = (UnsignedShort)(srcIndex[i] + rebase);
		}
		shadowIndexBufferD3D->Unlock();
		if (s_runNumIndices == 0)
			s_runStartIndex = nShadowIndicesInBuf;
		s_runNumIndices += numIndex;
		if (vtx - s_runBase < s_runMinIndex)
			s_runMinIndex = vtx - s_runBase;
		if (vtx - s_runBase + numVerts > s_runVertEnd)
			s_runVertEnd = vtx - s_runBase + numVerts;
		nShadowIndicesInBuf += numIndex;
		nShadowStartBatchIndex=nShadowIndicesInBuf;
		return;
	}
	flushShadowRun(m_pDev);	// a fresh upload draws now: the reused ranges before it go first
	uploadTask.m_uploadEpoch = 0;	//set again below only once this upload has fully landed
	if (s_shadowReuseUploads)
		gxShadowAdd(GXS_REUP);	// the DECR pass had to upload again (a DISCARD since the INCR pass)
	gxShadowAdd(GXS_UPB, (uint64_t)numVerts * sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX) + (uint64_t)numIndex * sizeof(short));

	const Bool vbFull = nShadowVertsInBuf > (SHADOW_VERTEX_SIZE-numVerts);
	const Bool ibFull = nShadowIndicesInBuf > (SHADOW_INDEX_SIZE-numIndex);
	// The open batch is drawn before a DISCARD (a later draw would read the new, empty buffer) and before it would span
	// more than 65535 vertices (the rebased indices must stay below 0xFFFF).
	if (s_batchBase >= 0 && (vbFull || ibFull || nShadowVertsInBuf + numVerts - s_batchBase > 65535))
		flushShadowBatch(m_pDev);

	if (vbFull)	//check if room for model verts
	{	//flush the buffer by drawing the contents and re-locking again
		if (shadowVertexBufferD3D->Lock(0,numVerts*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),(unsigned char**)&pvVertices, gxCenLock(D3DLOCK_DISCARD)) != D3D_OK)
			return;
		nShadowVertsInBuf=0;
		nShadowStartBatchVertex=0;
		bumpShadowUploadEpoch();	//earlier uploads are gone
		gxShadowAdd(GXS_DISC);
	}
	else
	{	if (shadowVertexBufferD3D->Lock(nShadowVertsInBuf*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),numVerts*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX), (unsigned char**)&pvVertices, gxCenLock(D3DLOCK_NOOVERWRITE)) != D3D_OK)
			return;
	}
#ifdef SV_DEBUG
	srand(0x1345465);
#endif
	if(pvVertices)
	{
#ifdef SV_DEBUG
		for (Int i=0; i<numVerts; i++)
		{
			Matrix3D::Transform_Vector(*meshXform, *geometry->GetVertex(i), (Vector3 *)pvVertices);	//cast is valid since both start with xyz
			pvVertices->diffuse=(rand()%255) | ((rand()%255)<<8) | ((rand()%255)<<16);
			pvVertices++;
		}
#else
		// GeneralsX: Upload in world space (was a memcpy of the object-space
		// vertices plus a per-draw world transform). Same arithmetic Matrix3D::Transform_Vector does everywhere.
		const Vector3 *src = geometry->GetVertex(0);
		const Matrix3D &xf = *meshXform;
		for (Int i=0; i<numVerts; i++, pvVertices++)
		{
			Vector3 w;
			Matrix3D::Transform_Vector(xf, src[i], &w);
			pvVertices->x = w.X;
			pvVertices->y = w.Y;
			pvVertices->z = w.Z;
		}
#endif
	}

	shadowVertexBufferD3D->Unlock();

	if (ibFull)	//check if room for model verts
	{	//flush the buffer by drawing the contents and re-locking again
		if (shadowIndexBufferD3D->Lock(0,numIndex*sizeof(short),(unsigned char**)&pvIndices, gxCenLock(D3DLOCK_DISCARD)) != D3D_OK)
			return;
		nShadowIndicesInBuf=0;
		nShadowStartBatchIndex=0;
		// GeneralsX: no epoch bump here - the DECR pass reuses only the VERTICES and re-sends indices itself
		gxShadowAdd(GXS_DISC);
	}
	else
	{	if (shadowIndexBufferD3D->Lock(nShadowIndicesInBuf*sizeof(short),numIndex*sizeof(short), (unsigned char**)&pvIndices, gxCenLock(D3DLOCK_NOOVERWRITE)) != D3D_OK)
			return;
	}

	// Batching: this volume joins the open batch (or opens one here); its indices are rebased to the batch's first vertex.
	if (s_shadowBatching && s_batchBase < 0)
	{
		s_batchBase = nShadowStartBatchVertex;
		s_batchStartIndex = nShadowStartBatchIndex;
		s_batchNumIndices = 0;
	}
	const Int base = s_shadowBatching ? s_batchBase : nShadowStartBatchVertex;
	const UnsignedShort rebase = (UnsignedShort)(nShadowStartBatchVertex - base);

	if(pvIndices)
	{
		const UnsignedShort *srcIndex = geometry->GetPolygonIndex(0,(short *)pvIndices);
		if (rebase == 0)
			memcpy(pvIndices,srcIndex,numPolys*3*sizeof(short));
		else
			for (Int i=0; i<numIndex; i++)
				pvIndices[i] = (UnsignedShort)(srcIndex[i] + rebase);
	}

	shadowIndexBufferD3D->Unlock();

	if (s_shadowBatching)
	{
		DEBUG_ASSERTCRASH(nShadowStartBatchIndex == s_batchStartIndex + s_batchNumIndices, ("shadow batch indices not contiguous"));
		s_batchNumIndices += numIndex;
		s_batchVertEnd = nShadowStartBatchVertex + numVerts;
	}
	else
		drawShadowRange(m_pDev, base, 0, numVerts, nShadowStartBatchIndex, numIndex);

	uploadTask.m_uploadEpoch = s_shadowUploadEpoch;
	uploadTask.m_uploadVertex = nShadowStartBatchVertex;

	nShadowVertsInBuf += numVerts;
	nShadowStartBatchVertex=nShadowVertsInBuf;

	nShadowIndicesInBuf += numIndex;
	nShadowStartBatchIndex=nShadowIndicesInBuf;
}

/** Debug function to draw bounding boxes around shadow volumes */
void W3DVolumetricShadow::RenderMeshVolumeBounds(Int meshIndex, Int lightIndex, const Matrix3D *meshXform)
{
	Geometry *geometry;
	Int numVerts, numPolys, numIndex;
	SHADOW_DYNAMIC_VOLUME_VERTEX* pvVertices;
	UnsignedShort *pvIndices;
	// Vertex Positions as a function of the box extents
	static Vector3						_BoxVerts[8] =
	{
		Vector3(  1.0f, 1.0f, 1.0f ),		// +z ring of 4 verts
		Vector3( -1.0f, 1.0f, 1.0f ),
		Vector3( -1.0f,-1.0f, 1.0f ),
		Vector3(  1.0f,-1.0f, 1.0f ),

		Vector3(  1.0f, 1.0f,-1.0f ),		// -z ring of 4 verts;
		Vector3( -1.0f, 1.0f,-1.0f ),
		Vector3( -1.0f,-1.0f,-1.0f ),
		Vector3(  1.0f,-1.0f,-1.0f ),
	};
	// Face Connectivity
	static Vector3i					_BoxFaces[12] =
	{
		Vector3i( 0,1,2 ),		// +z faces
		Vector3i( 0,2,3 ),
		Vector3i( 4,7,6 ),		// -z faces
		Vector3i( 4,6,5 ),
		Vector3i( 0,3,7 ),		// +x faces
		Vector3i( 0,7,4 ),
		Vector3i( 1,5,6 ),		// -x faces
		Vector3i( 1,6,2 ),
		Vector3i( 4,5,1 ),		// +y faces
		Vector3i( 4,1,0 ),
		Vector3i( 3,2,6 ),		// -y faces
		Vector3i( 3,6,7 )
	};

	static Vector3 verts[8];

	//Get D3D Device used by W3D for quicker access.
	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	if (!m_pDev)
		return;

	Vector3 meshPosition;
	meshXform->Get_Translation(&meshPosition);	//current mesh position

	geometry = m_shadowVolume[lightIndex][ meshIndex ];
	AABoxClass &aab=geometry->getBoundingBox();

	// compute the vertex positions
	meshPosition += aab.Center;	//get world space position of bounding box

	for (int ivert=0; ivert<8; ivert++)
	{
		verts[ivert].X = meshPosition.X + _BoxVerts[ivert][0] * aab.Extent.X;
		verts[ivert].Y = meshPosition.Y + _BoxVerts[ivert][1] * aab.Extent.Y;
		verts[ivert].Z = meshPosition.Z + _BoxVerts[ivert][2] * aab.Extent.Z;
	}

	//
	// if our count is out of sync with our geometry data something
	// is wrong here
	//
	assert( geometry );

	// get geometry requirements
	numVerts = 8;
	numPolys = 12;
	numIndex = numPolys * 3;

	// reject shadows with no data
	if( numVerts == 0 || numPolys == 0 )
		return;


	if (nShadowVertsInBuf > (SHADOW_VERTEX_SIZE-numVerts))	//check if room for model verts
	{	//flush the buffer by drawing the contents and re-locking again
		if (shadowVertexBufferD3D->Lock(0,numVerts*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),(unsigned char**)&pvVertices, gxCenLock(D3DLOCK_DISCARD)) != D3D_OK)
			return;
		nShadowVertsInBuf=0;
		nShadowStartBatchVertex=0;
		bumpShadowUploadEpoch();	//earlier uploads are gone
		gxShadowAdd(GXS_DISC);
	}
	else
	{	if (shadowVertexBufferD3D->Lock(nShadowVertsInBuf*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),numVerts*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX), (unsigned char**)&pvVertices, gxCenLock(D3DLOCK_NOOVERWRITE)) != D3D_OK)
			return;
	}
	srand(0x1345465);
	if(pvVertices)
	{	for (Int i=0; i<8; i++)
		{
			pvVertices->x=verts[i][0];
			pvVertices->y=verts[i][1];
			pvVertices->z=verts[i][2];
#ifdef SV_DEBUG
			pvVertices->diffuse=(rand()%255) | ((rand()%255)<<8) | ((rand()%255)<<16);
#endif
			pvVertices++;
		}
	}

	shadowVertexBufferD3D->Unlock();

	if (nShadowIndicesInBuf > (SHADOW_INDEX_SIZE-numIndex))	//check if room for model verts
	{	//flush the buffer by drawing the contents and re-locking again
		if (shadowIndexBufferD3D->Lock(0,numIndex*sizeof(short),(unsigned char**)&pvIndices, gxCenLock(D3DLOCK_DISCARD)) != D3D_OK)
			return;
		nShadowIndicesInBuf=0;
		nShadowStartBatchIndex=0;
		bumpShadowUploadEpoch();	//earlier uploads are gone
		gxShadowAdd(GXS_DISC);
	}
	else
	{	if (shadowIndexBufferD3D->Lock(nShadowIndicesInBuf*sizeof(short),numIndex*sizeof(short), (unsigned char**)&pvIndices, gxCenLock(D3DLOCK_NOOVERWRITE)) != D3D_OK)
			return;
	}


	if(pvIndices)
	{
		for (Int i=0; i<numPolys; i++,pvIndices+=3)
		{
			pvIndices[0] = _BoxFaces[i][0];
			pvIndices[1] = _BoxFaces[i][1];
			pvIndices[2] = _BoxFaces[i][2];
		}
	}

	shadowIndexBufferD3D->Unlock();

	m_pDev->SetIndices(shadowIndexBufferD3D,nShadowStartBatchVertex);


	//todo: replace this with mesh transform
	// GeneralsX: Keep bounds pass consistent with Generals matrix convention.
	Matrix4x4 mWorld(1);	//identity since boxes are pre-transformed to world space.
	D3DMATRIX dxmWorld = To_D3DMATRIX(mWorld);
	setShadowWorld(m_pDev, dxmWorld);

	m_pDev->SetStreamSource(0,shadowVertexBufferD3D,sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX));
	m_pDev->SetVertexShader(SHADOW_DYNAMIC_VOLUME_FVF);

	m_pDev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,numVerts,nShadowStartBatchIndex,numPolys);
	DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc=

	nShadowVertsInBuf += numVerts;
	nShadowStartBatchVertex=nShadowVertsInBuf;

	nShadowIndicesInBuf += numIndex;
	nShadowStartBatchIndex=nShadowIndicesInBuf;
}

// Shadow =====================================================================
// Shadow default constructor
// ============================================================================
W3DVolumetricShadow::W3DVolumetricShadow()
{
	Int i,j;

	m_geometry = nullptr;
	m_shadowLengthScale = 0.0f;
	m_gxTemplateLengthScale = 0.0f;
	m_extraExtrusionPadding = 0.0f;
	m_robj = nullptr;
	m_isEnabled = TRUE;
	m_isInvisibleEnabled = FALSE;

	for (j=0; j < MAX_SHADOW_CASTER_MESHES; j++)
	{	m_numSilhouetteIndices[j] = 0;
		m_maxSilhouetteEntries[j] = 0;
		m_silhouetteIndex[j] = nullptr;
		m_shadowVolumeCount[j] = 0;
	}

	for( i = 0; i < MAX_SHADOW_LIGHTS; i++ )
	{
		for (j=0; j < MAX_SHADOW_CASTER_MESHES; j++)
		{
			m_shadowVolume[ i ][j] = nullptr;
			m_shadowVolumeVB[i][j] = nullptr;
			m_shadowVolumeIB[i][j] = nullptr;
			m_shadowVolumeRenderTask[i][j].m_parentShadow = this;
			m_shadowVolumeRenderTask[i][j].m_meshIndex = (UnsignedByte)j;
			m_shadowVolumeRenderTask[i][j].m_lightIndex = (UnsignedByte)i;
			m_shadowVolumeRenderTask[i][j].m_uploadEpoch = 0;
			m_objectXformHistory[ i ][j].Make_Identity();
			m_lightPosHistory[ i ][j] = Vector3(0,0,0);
		}
	}

}

// ~W3DVolumetricShadow ====================================================================
// W3DVolumetricShadow destructor
// ============================================================================
W3DVolumetricShadow::~W3DVolumetricShadow()
{
	Int i,j;

	// we must free any silhouette data allocated
	for (j = 0; j < MAX_SHADOW_CASTER_MESHES; j++)
		deleteSilhouette(j);

	// free any shadow volume data
	for( i = 0; i < MAX_SHADOW_LIGHTS; i++ )
	{	for (j = 0; j < MAX_SHADOW_CASTER_MESHES; j++)
		{
			delete m_shadowVolume[i][j];
			if( m_shadowVolumeVB[i][j])
				TheW3DBufferManager->releaseSlot(m_shadowVolumeVB[i][j]);
			if( m_shadowVolumeIB[i][j])
				TheW3DBufferManager->releaseSlot(m_shadowVolumeIB[i][j]);
		}
	}

	if (m_geometry)
		REF_PTR_RELEASE(m_geometry);

	m_geometry=nullptr;
	m_robj=nullptr;

}

void W3DVolumetricShadow::SetGeometry( W3DShadowGeometry *geometry )
{

	Short numPrevVertices = 0;
	Short numNewVertices = 0;

	//
	// our geometry has changed, we need to allocate enough memory for the
	// silhouette data.  If silhouette data is present it must be reallocated
	// to accomoddate the new size if smaller
	//

	// if we had previous geometry how many vertices did it have

	for (Int i=0; i<MAX_SHADOW_CASTER_MESHES; i++)
	{
		if( m_geometry )
			numPrevVertices = m_geometry->getMesh(i)->GetNumVertex();

		// now many vertices does our new geometry have
		if( geometry )
			numNewVertices = geometry->getMesh(i)->GetNumVertex();

		//
		// TODO: Colin, may want to change this in the future
		// if our new geometry requires more memory allocate it, if it requires
		// less we'll leave it around for future switches in geometry
		//
		if( numNewVertices > numPrevVertices )
		{

			deleteSilhouette(i);
			if( allocateSilhouette(i, numNewVertices ) == FALSE )
				return;

		}
	}

	// assign the new geometry, possible over an old geometry
	m_geometry = geometry;

}

/**Called once per frame for each object, when necessary it will reconstruct
 the shadow volume for this shadow from the silhouette of the geometry
 and any light sources
*/
void W3DVolumetricShadow::Update()
{
	static Int currentTime, lastTime, delay = 0;
	// OBJECT_PILE
	// static Vector3 originCompareVector(0,0,9999);
	static Vector3 originCompareVector(0,0,0);
	Vector3 pos;

	// sanity
	if( m_geometry == nullptr)
		return;

	//
	// for now we will just rebuild a shadow volume every so often, this
	// should be changed to be built only when the light angle sufficiently
	// changes or when the object rotation sufficiently changes
	//

//	currentTime = timeGetTime();
//	if( currentTime - lastTime >= delay )
	{
		pos=m_robj->Get_Position();
		if (pos == originCompareVector)
		{	//the transform on this object was never set so we can't make any determination
			//if it's visible or what the shadow looks like.
			return;
		}
		// GeneralsX: Reject an off-screen caster BEFORE the terrain height
		// lookup. Both branches below return on the same box test with a per-branch extent (m_robjExtent,
		// or 1.5x it for an airborne caster), and the ground height only picks the branch. Outside the box
		// grown by the WIDER of the two extents is outside it for either one (float addition is monotone),
		// so this returns only where the original would have, with nothing skipped but the height query,
		// which is a const read. NaN compares false and falls through to the unchanged code.
		// scripts/test/audit/bench/shadow_cull_before_height.cpp checks it.
		{
			const Real airborneExtent = MAX_SHADOW_LENGTH_EXTRA_AIRBORNE_SCALE_FACTOR * m_robjExtent;
			const Real widestExtent = (airborneExtent > m_robjExtent) ? airborneExtent : m_robjExtent;
			if (WWMath::Fabs(pos.X - bcX) > (beX + widestExtent) ||
				WWMath::Fabs(pos.Y - bcY) > (beY + widestExtent) ||
				WWMath::Fabs(pos.Z - bcZ) > (beZ + widestExtent))
				return;	//shadow can't be visible for either extent
		}
		//Check if this is a "flying" unit.  Flying units are defined as anything that moves more than
		//AIRBORNE_UNIT_GROUND_DELTA world units above ground.  This will allow "jumping" units like rocket
		//buggies from being flagged as "flying".  We minimize the number of flying units because their shadow
		//volumes are extruded longer to reach the lowest point on the map.  Regular shadows only extend far
		//enough to reach the base of the model (presumes base is already touching the ground).
		Real groundHeight;
		if (TheTerrainLogic)
			groundHeight=TheTerrainLogic->getGroundHeight(pos.X,pos.Y);	//logic knows about bridges so use if available.
		else
			groundHeight=TheTerrainRenderObject->getHeightMapHeight(pos.X,pos.Y, nullptr);
   		if (fabs(pos.Z - groundHeight) >= AIRBORNE_UNIT_GROUND_DELTA)
   		{
 			Real extent = MAX_SHADOW_LENGTH_EXTRA_AIRBORNE_SCALE_FACTOR * m_robjExtent;
 			if (WWMath::Fabs(pos.X - bcX) > (beX + extent) ||
 				WWMath::Fabs(pos.Y - bcY) > (beY + extent) ||
 				WWMath::Fabs(pos.Z - bcZ) > (beZ + extent))
 				return;	//shadow can't be visible so no point in updating.

			//this unit is above ground, extend shadow volume to reach lowest point on the terrain plus extra bit to make
			//sure shadow goes under ground.
   			updateVolumes(fabs(pos.Z - TheTerrainRenderObject->getMinHeight()) + SHADOW_EXTRUSION_BUFFER, FALSE);	// GeneralsX: no size skip, see updateVolumes
   		}
   		else
 		{	//normal object that is not floating above ground so we don't need to extend the shadow lower than the object's
			//base since it should be sitting directly at ground level.

 			if (WWMath::Fabs(pos.X - bcX) > (beX + m_robjExtent) ||
 				WWMath::Fabs(pos.Y - bcY) > (beY + m_robjExtent) ||
 				WWMath::Fabs(pos.Z - bcZ) > (beZ + m_robjExtent))
 				return;	//shadow can't be visible so no point in updating.

				//check if this object has never had it's extrusion length updated.  Will only be true for
				//immobile objects because finding an optimal extrusion length is expensive.
				if (!m_extraExtrusionPadding)
					updateOptimalExtrusionPadding();

   			updateVolumes(m_extraExtrusionPadding);
 		}


	//	floorZ = 2.0f;	//lower slightly so shadows go under ground.


		// update delay time
		lastTime = currentTime;
	}

}

/** Update shadow volumes belonging to all meshes of this shadow caster.
*	Use zoffset to extend shadows below object's base by given amount.
*/
void W3DVolumetricShadow::updateVolumes(Real zoffset, Bool allowSizeSkip)
{
	Int i,j;

	HLodClass *hlod=(HLodClass *)m_robj;
	MeshClass *mesh;
	static AABoxClass aaBox;
	static SphereClass sphere;
	Int meshIndex;

	DEBUG_ASSERTCRASH(hlod != nullptr,("updateVolumes : hlod is null!"));

	Bool parentVis=m_robj->Is_Really_Visible();

	for( i = 0; i < MAX_SHADOW_LIGHTS; i++ )
	{
		for (j=0; j<m_geometry->getMeshCount(); j++)
		{
			meshIndex=m_geometry->getMesh(j)->m_meshRobjIndex;

			if (meshIndex >= 0)
				mesh = (MeshClass *)hlod->Peek_Lod_Model(0,meshIndex);
			else
				mesh = (MeshClass *)m_robj;

			if (mesh)
			{
				if (!mesh->Is_Not_Hidden_At_All())
					continue;

				// GeneralsX: Too small on screen to see: no rebuild, upload or draw (see
				// SHADOW_MIN_SCREEN_RADIUS_PX). Judged on the volume's sphere from its last build; a volume never built yet
				// is built once. Its history is untouched, so when it grows again the usual rotation test rebuilds it. Never for an
				// airborne caster (allowSizeSkip FALSE): its extrusion reaches down to the lowest terrain, past the sphere it was built with.
				if (allowSizeSkip && m_shadowVolume[i][j])
				{
					const SphereClass &vs = m_shadowVolume[i][j]->getBoundingSphere();
					if (shadowScreenRadiusPx(vs.Center + mesh->Get_Transform().Get_Translation(), vs.Radius) < gxGovShadowMinPx(SHADOW_MIN_SCREEN_RADIUS_PX))	// GeneralsX: S14 look governor (GameClient/GXLookGovernor.h): shadow step
						continue;
				}

				/**@todo: Getting the transform of the mesh may be forcing a full hierarchy evaluation.
					Expensive for off-screen models... do we really need this?	*/
				//Extend floor of model by 'zoffset' to compensate for flying units.
				updateMeshVolume(j, i, &mesh->Get_Transform(), mesh->Get_Bounding_Box(),m_robj->Get_Position().Z - zoffset);
				//update visibility if not set yet
				if (m_shadowVolume[i][j])
				{
					if(m_shadowVolume[i][j]->getVisibleState() ==	Geometry::STATE_UNKNOWN)
					{	//Updating the mesh volume didn't update the visibility, must do it here.
						//First check against bounding sphere
						if (parentVis)
 						{ //parent is visible so most likely all sub_objects are also visible so probably all shadows also visible.
 						  //skip additional visibility tests
 							m_shadowVolume[i][j]->setVisibleState(Geometry::STATE_VISIBLE);
 						}
 						else
						{	sphere=m_shadowVolume[i][j]->getBoundingSphere();
							sphere.Center += mesh->Get_Transform().Get_Translation();
							CollisionMath::OverlapType result=CollisionMath::Overlap_Test(*shadowCameraFrustum,sphere);
							if (result == CollisionMath::OVERLAPPED)
							{	//do a more accurate test against bounding box.
								aaBox=m_shadowVolume[i][j]->getBoundingBox();
								aaBox.Translate(mesh->Get_Transform().Get_Translation());	//translate bounding box to world space.
								if (CollisionMath::Overlap_Test(*shadowCameraFrustum,aaBox) != CollisionMath::OUTSIDE)
									m_shadowVolume[i][j]->setVisibleState(Geometry::STATE_VISIBLE);
								else
									m_shadowVolume[i][j]->setVisibleState(Geometry::STATE_INVISIBLE);
							}
							else
								m_shadowVolume[i][j]->setVisibleState((Geometry::VisibleState)result);
						}
					}
					if (m_shadowVolume[i][j]->getVisibleState() ==	Geometry::STATE_VISIBLE)
					{	//shadow volume is visible.  Add it to list of rendertasks.
						W3DBufferManager::W3DVertexBufferSlot *vbSlot=m_shadowVolumeVB[i][j];
						if (vbSlot)
						{	//add to static mesh volume list.
							if (!fillStaticShadowSlot(m_shadowVolume[i][j], vbSlot, mesh->Get_Transform()))
								continue;	// GeneralsX: a slot that could not be filled is not drawn
							W3DBufferManager::W3DRenderTask *oldTask=vbSlot->m_VB->m_renderTaskList;
							vbSlot->m_VB->m_renderTaskList=&m_shadowVolumeRenderTask[i][j];
							vbSlot->m_VB->m_renderTaskList->m_nextTask=oldTask;
						}
						else
						{
							TheW3DVolumetricShadowManager->addDynamicShadowTask(&m_shadowVolumeRenderTask[i][j]);
						}
					}
				}
			}
		}
	}
}

/*floorZ is the assumed ground height below the model.  The code will try to extrude shadows just long enough to hit this point in order
to reduce fill rate usage.*/
void W3DVolumetricShadow::updateMeshVolume(Int meshIndex, Int lightIndex, const Matrix3D *meshXform, const AABoxClass &meshBox, float floorZ )
{
	Vector3 lightPosObject;
	Matrix4x4 worldToObject;
	Vector3 objectCenter;
	Vector3 toLight;
	Vector3 toPrevLight;
	Vector3 prevPosToLight;
	Vector3 lightPosWorld;
	Vector3 prevObjPosition;
	//Figuring out if mesh has rotated is cheaper (no normalization) than figuring out if light angle has changed.
	//So we divide the 2 tests.  Also, our light (sun) almost never moves so no second test needed at all.
	Bool isMeshRotating = false;	//flag if mesh has rotated since last update. Translation doesn't matter for infinite light source.
	Bool isLightMoving = false;	//flag if light has moved since last update.
	Bool gxZRise = false;		// [GXSHADOW]: isLightMoving was set by a Z rise, not by the light

	Matrix4x4 objectToWorld(*meshXform);
	Matrix4x4 *prevXForm=&m_objectXformHistory[ lightIndex ][meshIndex];

	//
	// build the shadow silhouette and construct shadow volume from
	// this light location.  The for loop wrapped around this is
	// theoretical code for future enhancements of multiple lights that
	// cast shadows
	//

#ifdef CNC3 //(gth) numerical error requires that the axis vectors be normalized...

	//When dealing with infinite light sources, we can assume that the shadow doesn't
	//change much based on object position.  Only the orientation to light matters.
	Vector3 va = (Vector3 &)(*prevXForm)[0];
	Vector3 vb = (Vector3 &)objectToWorld[0];
	va.Normalize();
	vb.Normalize();
	Real cosAngle = WWMath::Fabs(Vector3::Dot_Product(va,vb));

	if (cosAngle >= cosAngleToCare)
	{

		va = (Vector3 &)(*prevXForm)[1];
		vb = (Vector3 &)objectToWorld[1];
		va.Normalize();
		vb.Normalize();
		cosAngle = WWMath::Fabs(Vector3::Dot_Product(va,vb));

		if (cosAngle >= cosAngleToCare)
		{
			va = (Vector3 &)(*prevXForm)[2];
			vb = (Vector3 &)objectToWorld[2];
			va.Normalize();
			vb.Normalize();
			cosAngle = WWMath::Fabs(Vector3::Dot_Product(va,vb));
			if (cosAngle < cosAngleToCare)
				isMeshRotating=true;
		}
		else
			isMeshRotating =true;
	}
	else
		isMeshRotating =true;


#else // CNC3 (old generals code)

#ifdef ASSUME_NEAR_LIGHTSOURCE
	if (memcmp(&objectToWorld,prevXForm,sizeof(objectToWorld)))
		isMeshRotating = true; //mesh transform has not changed since last update.
#else
	//When dealing with infinite light sources, we can assume that the shadow doesn't
	//change much based on object position.  Only the orientation to light matters.
	Real cosAngle = fabs (Vector3::Dot_Product((Vector3 &)(prevXForm->operator [](0)),(Vector3 &)(objectToWorld.operator [](0))));
	if (cosAngle >= cosAngleToCare)
	{	cosAngle = fabs (Vector3::Dot_Product((Vector3 &)(prevXForm->operator [](1)),(Vector3 &)(objectToWorld.operator [](1))));
		if (cosAngle >= cosAngleToCare)
		{
			cosAngle = fabs (Vector3::Dot_Product((Vector3 &)(prevXForm->operator [](2)),(Vector3 &)(objectToWorld.operator [](2))));
			if (cosAngle < cosAngleToCare)
				isMeshRotating=true;
		}
		else
			isMeshRotating =true;
	}
	else
		isMeshRotating =true;
#endif	//near light source
#endif // CNC3

	// get the light
	lightPosWorld = TheW3DShadowManager->getLightPosWorld(lightIndex);

	// get the object
	meshXform->Get_Translation(&objectCenter);	//current mesh position

	// GeneralsX: A small volume tolerates more rotation before its silhouette is rebuilt: the
	// stale volume is drawn under the CURRENT mesh transform, so it only points a little off the sun, by the angle turned
	// since its last build. Allowed: what moves a point two sphere radii out by SHADOW_STALE_PX pixels (capped); a volume of
	// 72 px radius or more keeps the stock cosAngleToCare.
	if (isMeshRotating && m_shadowVolume[lightIndex][meshIndex] != nullptr)
	{
		const SphereClass &vs = m_shadowVolume[lightIndex][meshIndex]->getBoundingSphere();
		const Real px = shadowScreenRadiusPx(vs.Center + objectCenter, vs.Radius);
		Real tolerance = (px > 0.0f) ? gxGovShadowStalePx(SHADOW_STALE_PX) / (2.0f * px) : SHADOW_MAX_STALE_RAD;	// GeneralsX: S14 look governor (GameClient/GXLookGovernor.h): shadow step
		if (tolerance > SHADOW_MAX_STALE_RAD)
			tolerance = SHADOW_MAX_STALE_RAD;
		const Real cosTolerance = cosf(tolerance);
		if (cosTolerance < cosAngleToCare)
		{
			// The angle turned since the last build, exactly: for rotations trace(R * R0^T) = 1 + 2 cos(angle), and that trace is
			// the sum of the dot products of matching rows, each normalised here so a scaled mesh reads its rotation alone.
			Real trace = 0.0f;
			Bool measurable = TRUE;
			for (Int axis = 0; axis < 3 && measurable; ++axis)
			{
				const Vector3 &a = (Vector3 &)(prevXForm->operator [](axis));
				const Vector3 &b = (Vector3 &)(objectToWorld.operator [](axis));
				const Real len2 = a.Length2() * b.Length2();
				if (len2 > 0.0f)
					trace += Vector3::Dot_Product(a, b) / sqrtf(len2);
				else
					measurable = FALSE;
			}
			if (measurable && 0.5f * (trace - 1.0f) >= cosTolerance)
				isMeshRotating = false;	// within tolerance: keep the volume, and the history it was built from
		}
	}

	// check if object has a limit/clamp on shadow length and adjust light
	// position of necessary.
	if (m_shadowLengthScale)
	{	//Find light's distance from origin in xy plane
		Real lightXYDistance = sqrt(lightPosWorld.X*lightPosWorld.X + lightPosWorld.Y * lightPosWorld.Y);
		Real newZ=lightXYDistance*m_shadowLengthScale;

		if (newZ > lightPosWorld.Z)
		{	//clamped z component is higher than actual light position allows so adjust it.
			lightPosWorld.Z = newZ;
		}
	}

	if (lightPosWorld != m_lightPosHistory[ lightIndex ][meshIndex])
	{	//Light position has moved, see if enough to matter

		// compute vector from the light to the current object position
		toLight = objectCenter - lightPosWorld;
		toLight.Normalize();

		// compute vector from the previous light to the object position
		toPrevLight = objectCenter - m_lightPosHistory[ lightIndex ][meshIndex];
		toPrevLight.Normalize();

		Real cosAngle = fabs (Vector3::Dot_Product(toLight,toPrevLight));
		if (cosAngle < cosAngleToCare)	//less than 45 degree change
			isLightMoving =true;
	}
	else
	///@todo: Find a better way to deal with this - use maximum extrusion once!  Also avoid hit for units climbing hills.
	if (fabs(objectCenter.Z - prevXForm->operator [](2).W) > SHADOW_EXTRUSION_BUFFER)
	{
		isLightMoving = true;	//treat model rising just like rotation since volume needs update for longer extrusion.
		gxZRise = true;
	}

	// reconstruct if needed (or a GPU volume RenderGpuMeshVolume could not draw)
	const Bool gxStale = m_shadowVolume[ lightIndex ][meshIndex] != nullptr && (m_shadowVolume[ lightIndex ][meshIndex]->GetFlags() & SHADOW_GPU_STALE);
	if (isLightMoving || isMeshRotating || gxStale)
	{
		//
		// transform the light in the world to object space, we
		// care only about the rotation of components for the coordinate
		// system change, not the translations
		//
		Real det;
		Matrix4x4::Inverse(&worldToObject, &det, &objectToWorld);

		// find out light position in object space
		Matrix4x4::Transform_Vector(worldToObject,lightPosWorld,&lightPosObject);

		//Updating shadow volumes is expensive, so verify that this volume is even visible.

		//Generate bounding box around shadow volume by extruding AABB corners
		AABoxClass box(meshBox);	//copy current mesh bounding box (will be smaller than shadow box).
		SphereClass sphere;			//rough bounding sphere of shadow volume - based on box.
		Vector3 Corners[8];
		Vector3 lightRay;
		Real vectorScale,vectorScaleTemp, vectorScaleMax;
		Real length;

		//Get vertices of top of bounding box
		Corners[0]=box.Center+box.Extent;	//top right corner
		Corners[1]=Corners[0];
		Corners[1].X -= 2.0f*box.Extent.X;		//top left corner
		Corners[2]=Corners[1];
		Corners[2].Y -= 2.0f*box.Extent.Y;		//bottom left corner
		Corners[3]=Corners[2];
		Corners[3].X += 2.0f*box.Extent.X;		//bottom right corner

		//Project top volume corners onto ground plane
		lightRay = Corners[0] - lightPosWorld;	//vector light to corner
		length= 1.0f/lightRay.Length();
		lightRay *= length;
		vectorScaleMax=vectorScale=(Real)fabs((Corners[0].Z-floorZ)/lightRay.Z);	//length of vector from top corner to ground.
		Corners[4]=Corners[0]+lightRay*vectorScale;
		vectorScaleMax *= length;

		lightRay = Corners[1] - lightPosWorld;	//vector light to corner
		length= 1.0f/lightRay.Length();
		lightRay *= length;
		vectorScaleTemp=(Real)fabs((Corners[1].Z-floorZ)/lightRay.Z);	//length of vector from top corner to ground.
		Corners[5]=Corners[1]+lightRay*vectorScaleTemp;
		vectorScaleTemp *= length;

		if (vectorScaleTemp > vectorScaleMax)
			vectorScaleMax=vectorScaleTemp;	//keep track of maximum required extrusion length.

		lightRay = Corners[2] - lightPosWorld;	//vector light to corner
		length= 1.0f/lightRay.Length();
		lightRay *= length;
		vectorScale=(Real)fabs((Corners[2].Z-floorZ)/lightRay.Z);	//length of vector from top corner to ground.
		Corners[6]=Corners[2]+lightRay*vectorScale;
		vectorScale *= length;

		if (vectorScale > vectorScaleMax)
			vectorScaleMax=vectorScale;	//keep track of maximum required extrusion length.

		lightRay = Corners[3] - lightPosWorld;	//vector light to corner
		length= 1.0f/lightRay.Length();
		lightRay *= length;
		vectorScaleTemp=(Real)fabs((Corners[3].Z-floorZ)/lightRay.Z);	//length of vector from top corner to ground.
		Corners[7]=Corners[3]+lightRay*vectorScaleTemp;
		vectorScaleTemp *= length;

		if (vectorScaleTemp > vectorScaleMax)
			vectorScaleMax=vectorScaleTemp;	//keep track of maximum required extrusion length.

		box.Init(Corners, 8);	//generate a new bounding box
		sphere.Init(box.Center,box.Extent.Length());	//generate object space bounding sphere containing box.

		CollisionMath::OverlapType result=CollisionMath::Overlap_Test(*shadowCameraFrustum,sphere);
		if (result == CollisionMath::OVERLAPPED)	//do a more accurate test
			result=CollisionMath::Overlap_Test(*shadowCameraFrustum, box);

		if (result != CollisionMath::OUTSIDE)
		{
			//
			// reset the silhouette data and build a new one from this light
			// source perspective
			//

			gxShadowAdd(isMeshRotating ? GXS_RBROT : (gxZRise ? GXS_RBZ : GXS_RBLIGHT));
			// GeneralsX: GX_SHADOW_GPU: a volume that is (or now turns) dynamic keeps no silhouette or
			// vertices - this rebuild stores only what constructVolume would have extruded with, and the GPU extrudes the mesh's edge
			// mesh (see gxShadowGpuFlush). Same trigger, same frustum test and same moment as the CPU rebuild below.
			Geometry *gpuVolume = m_shadowVolume[ lightIndex ][meshIndex];
			if (gpuVolume != nullptr && gxShadowGpuOn()
				&& ((gpuVolume->GetFlags() & SHADOW_DYNAMIC) || (m_shadowVolumeVB[ lightIndex ][meshIndex] && isMeshRotating))
				&& m_geometry->getMesh(meshIndex)->gxGpuReady())
			{
				if (m_shadowVolumeVB[ lightIndex ][meshIndex])
				{	// the static volume turns dynamic, as below, but needs no object-space copy
					gxShadowAdd(GXS_DYNFLIP);
					resetShadowVolume( lightIndex,meshIndex );	//frees its slots and vertices
				}
				else
					gpuVolume->Release();	//a CPU dynamic volume's vertices (none once it is a GPU volume)
				gpuVolume->SetFlags((gpuVolume->GetFlags() | SHADOW_DYNAMIC | SHADOW_GPU) & ~SHADOW_GPU_STALE);
				if (vectorScaleMax > MAX_EXTRUSION_LENGTH)
					vectorScaleMax = MAX_EXTRUSION_LENGTH;
				gpuVolume->setGpuLight(lightPosObject, vectorScaleMax);
			}
			else
			{
			if (gpuVolume != nullptr && (gpuVolume->GetFlags() & SHADOW_GPU))
			{	// the GPU path went away (its edge mesh or the shader could not be rebuilt after a device reset): back to the CPU
				gpuVolume->SetFlags(gpuVolume->GetFlags() & ~(SHADOW_GPU | SHADOW_GPU_STALE));
				allocateShadowVolume( lightIndex,meshIndex );	//a dynamic volume's vertex arrays
			}
				if (m_numSilhouetteIndices[meshIndex] != 0)
				{	//this silhouette was built before and is being updated.
					//this probably means it will change again in the future.
					//make future updates faster by pre-caching face normals.
					m_geometry->getMesh(meshIndex)->buildPolygonNormals();
				}
				resetSilhouette(meshIndex);
				buildSilhouette(meshIndex, &lightPosObject);

				//
				// in a multiple shadow situation we would be allocating a volume
				// for this current shadow light, not the 0 index volume all the time
				//
				if (!m_shadowVolume[ lightIndex ][meshIndex])
					allocateShadowVolume( lightIndex,meshIndex );
				if( m_shadowVolumeVB[ lightIndex ][meshIndex] )
				{	//Updating an existing vertex buffer shadow volume.  This means we're
					//probably dealing with an animated mesh.  Update flags to reflect this fact.
					if (isMeshRotating || isLightMoving)
					{
						if (isMeshRotating)
						{	//rotating meshes will most likely need updates each frame, so stop using static vertex buffers.
							gxShadowAdd(GXS_DYNFLIP);
							m_shadowVolume[ lightIndex ][meshIndex]->SetFlags(
								m_shadowVolume[ lightIndex ][meshIndex]->GetFlags() | SHADOW_DYNAMIC);
						}
						//release memory used to store vertices/polygons
						resetShadowVolume( lightIndex,meshIndex );	//free vertex buffers since not used for dynamic.
						//Resize the shadow volume since we'll need room to store the vertices in memory instead of VB.
						allocateShadowVolume( lightIndex,meshIndex );
					}
				}

				//
				// construct the shadow volume at this light position in the
				// passed shadow volume geometry index
				//
				// GeneralsX: Clamp extrusion length to prevent runaway volume spikes on shallow light rays.
				if (vectorScaleMax > MAX_EXTRUSION_LENGTH)
					vectorScaleMax = MAX_EXTRUSION_LENGTH;
				if (m_shadowVolume[ lightIndex ][meshIndex]->GetFlags() & SHADOW_DYNAMIC)
					constructVolume( &lightPosObject, vectorScaleMax, lightIndex, meshIndex );
				else
					constructVolumeVB( &lightPosObject, vectorScaleMax, lightIndex, meshIndex );
			}

			//
			// store the current light position and orientation that
			// we constructed shadow info at
			//
			m_objectXformHistory[ lightIndex ][meshIndex] = objectToWorld;
			m_lightPosHistory[lightIndex][meshIndex] = lightPosWorld;

			box.Translate(-objectCenter);	//translate box to object space.
			m_shadowVolume[ lightIndex ][meshIndex]->setBoundingBox(box);
			sphere.Center -= objectCenter;
			m_shadowVolume[ lightIndex ][meshIndex]->setBoundingSphere(sphere);
			m_shadowVolume[ lightIndex ][meshIndex]->setVisibleState(Geometry::STATE_VISIBLE);	//this volume needs rendering.
		}
		else
		if (m_shadowVolume[ lightIndex ][meshIndex])
		{	//outside view frustum, shadow wasn't updated.
			box.Translate(-objectCenter);	//translate box to object space.
			m_shadowVolume[ lightIndex ][meshIndex]->setBoundingBox(box);
			sphere.Center -= objectCenter;
			m_shadowVolume[ lightIndex ][meshIndex]->setBoundingSphere(sphere);
			m_shadowVolume[ lightIndex ][meshIndex]->setVisibleState(Geometry::STATE_INVISIBLE);
		}
	}
	else
	{	//not reconstructing volume, so don't know if visible or not.
		if (m_shadowVolume[ lightIndex ][meshIndex])
			m_shadowVolume[ lightIndex ][meshIndex]->setVisibleState(Geometry::STATE_UNKNOWN);
	}
}

// addSilhouetteEdge ==========================================================
// It has been determined that the polygon neighbor in the "neighborIndex"
// of "visible" needs to be added to the silhouette.  We will add those two
// vertex indices to the silhouette in the order they were specified in
// "visible" to assure that the constructed edge is in counter clockwise order
// ============================================================================
void W3DVolumetricShadow::addSilhouetteEdge(Int meshIndex, PolyNeighbor *visible, PolyNeighbor *hidden )
{
	Int i;
	Int neighborIndex = 0;
	Short visibleIndexList[ 3 ];
	Short edgeStart, edgeEnd;

	W3DShadowGeometryMesh *geomMesh=m_geometry->getMesh(meshIndex);

	// sanity
	assert( visible && hidden );

	//
	// which index in the neighbor list of "visible" refers to the
	// polygon "hidden"
	//
	for( i = 0; i < MAX_POLYGON_NEIGHBORS; i++ )
	{

		if( visible->neighbor[ i ].neighborIndex == hidden->myIndex )
		{

			neighborIndex = i;
			break;  // exit for

		}

	}

	// get the three vertex indices of "visible"
	geomMesh->GetPolygonIndex( visible->myIndex, visibleIndexList );

	//
	// we know that 2 of the 3 vertex indices will be present in the edge.
	// will construct the edge as follows to ensure we have counter
	// clockwise order.  note that this assumes the vertices of the
	// polygons specified in the geometry are in counter clockwise order,
	// which they are
	//
	// 1) [ v1  Absent, v2 Present, v3 Present ] -> edge = (v2, v3)
	// 2) [ v1 Present, v2  Absent, v3 Present ] -> edge = (v3, v1)
	// 3) [ v1 Present, v2 Present, v3 Absent  ] -> edge = (v1, v2)
	//
	if( (visibleIndexList[ 0 ] !=
			 visible->neighbor[ neighborIndex ].neighborEdgeIndex[ 0 ]) &&
			(visibleIndexList[ 0 ] !=
			visible->neighbor[ neighborIndex ].neighborEdgeIndex[ 1 ]) )
	{

		// case 1 above
		edgeStart = visibleIndexList[ 1 ];
		edgeEnd = visibleIndexList[ 2 ];

	}
	else if( (visibleIndexList[ 1 ] !=
					 visible->neighbor[ neighborIndex ].neighborEdgeIndex[ 0 ]) &&
					 (visibleIndexList[ 1 ] !=
					 visible->neighbor[ neighborIndex ].neighborEdgeIndex[ 1 ]) )
	{

		// case 2 above
		edgeStart = visibleIndexList[ 2 ];
		edgeEnd = visibleIndexList[ 0 ];

	}
	else
	{

		// case 3 above
		edgeStart = visibleIndexList[ 0 ];
		edgeEnd = visibleIndexList[ 1 ];

	}

	// add to silhouette edge list
	addSilhouetteIndices(meshIndex, edgeStart, edgeEnd );

}

// addNeighborlessEdges =======================================================
// Given a polygon neighbor information, it has been determined that this
// polygon is visible and has edges which are not connected to other
// polygons, these edges need to be added to the silhouette.  The edge(s)
// must be added in such an order that we create silhouette edges in a
// counter clockwise order.
// ============================================================================
void W3DVolumetricShadow::addNeighborlessEdges(Int meshIndex, PolyNeighbor *us )
{
	Short vertexIndexList[ 3 ];
	Int i, j;
	Short edgeStart, edgeEnd;
	Bool addEdge;

	// sanity
	assert( us );

	W3DShadowGeometryMesh *geomMesh = m_geometry->getMesh(meshIndex);

	// get the vertex index list from the geometry
	geomMesh->GetPolygonIndex( us->myIndex, vertexIndexList );

	//
	// go through each edge, if these indices to NOT appear TOGETHER in
	// neighbor list then we must add it.
	//
	for( i = 0; i < 3; i++ )
	{

		// get the edge start and end vertex indices
		edgeStart = vertexIndexList[ i ];
		if( i == 2 )
			edgeEnd = vertexIndexList[ 0 ];  // wraps to begging of list
		else
			edgeEnd = vertexIndexList[ i + 1 ];

		// do these two vertices appear in a neighbor list of the poly?
		addEdge = TRUE;
		for( j = 0; j < MAX_POLYGON_NEIGHBORS; j++ )
		{

			if( us->neighbor[ j ].neighborIndex != NO_NEIGHBOR )
			{

				if( (us->neighbor[ j ].neighborEdgeIndex[ 0 ] == edgeStart &&
						 us->neighbor[ j ].neighborEdgeIndex[ 1 ] == edgeEnd) ||
						(us->neighbor[ j ].neighborEdgeIndex[ 1 ] == edgeStart &&
						 us->neighbor[ j ].neighborEdgeIndex[ 0 ] == edgeEnd) )
				{

					addEdge = FALSE;
					break;  // exit for j, no need to search on

				}

			}

		}

		// add the edge if no neighbors have that edge
		if( addEdge == TRUE )
		{

			addSilhouetteIndices(meshIndex, edgeStart, edgeEnd );

		}

	}

}

// addSilhouetteIndices =======================================================
// Add these two indices to the silhouette data
// ============================================================================
void W3DVolumetricShadow::addSilhouetteIndices(Int meshIndex, Short edgeStart, Short edgeEnd )
{

//	DBGPRINTF(( "addSilhouetteIndices: Adding (%d,%d), the storage before the add is = (%d/%d)\n",
//							edgeStart, edgeEnd, m_numSilhouetteIndices, m_maxSilhouetteEntries ));

	// add to silhouette edge list
	assert( m_numSilhouetteIndices[meshIndex] < m_maxSilhouetteEntries[meshIndex] );
	m_silhouetteIndex[meshIndex][ m_numSilhouetteIndices[meshIndex]++ ] = edgeStart;
	assert( m_numSilhouetteIndices[meshIndex] < m_maxSilhouetteEntries[meshIndex] );
	m_silhouetteIndex[meshIndex][ m_numSilhouetteIndices[meshIndex]++ ] = edgeEnd;

}

// buildSilhouette ============================================================
// Given a light position, and our polygon neighbor information this will
// build the silhouette of the object edges from the given light position
// ============================================================================
void W3DVolumetricShadow::buildSilhouette(Int meshIndex, Vector3 *lightPosObject)
{
	PolyNeighbor *polyNeighbor;  // the poly we're looking at right now
	Vector3 lightVector;  // vector from light to polygon
	Bool visibleNeighborless;
	Int numPolys;  // number of polys in our geometry
	W3DShadowGeometryMesh *geomMesh;
	Int i, j;
	Int meshEdgeStart=0; //index to first edge contributed by specific mesh

	//
	// go through each of our shadow geometry polygon info and find out
	// which polys are visible from this light source and which ones are not
	//

	geomMesh = m_geometry->getMesh(meshIndex);

	//record where this meshes indices will begin.
	meshEdgeStart=m_numSilhouetteIndices[meshIndex];

	numPolys = geomMesh->GetNumPolygon();
	for( i = 0; i < numPolys; i++ )
	{
		Short poly[ 3 ];

		// get this polygon neighbor information
		polyNeighbor = geomMesh->GetPolyNeighbor( i );

		// take this opportunity to initialize our processing flags to zero
		polyNeighbor->status = 0;

		// get the normal for this polygon
		const Vector3& normal=geomMesh->GetPolygonNormal(i);

		// get the vertex indices at this polygon
		geomMesh->GetPolygonIndex( i, poly );

		//
		// find out "lightVector" to this polygon
		//
		// since our light source could be very close to the object and that
		// would change the shadow we are going to say that the light vector
		// is from the light position to one of the vertices in the polygon.
		// To be more correct we should use the center of the polygon but
		// this is a good approximation ... an ever broader approximation that
		// we could use would be the object center
		//
		const Vector3& vertex=geomMesh->GetVertex( poly[ 0 ] );
		lightVector= vertex - *lightPosObject;

		//
		// dot the light vector with the normal of the polygon to see if the
		// poly is visible from this location
		//
		if( Vector3::Dot_Product( lightVector, normal ) < 0.0f )
			BitSet( polyNeighbor->status, POLY_VISIBLE );

	}

	//
	// check all our polys using our poly neighbors, where one poly neighbor
	// is not the same visible status as a neighbor that represents a
	// silhouette edge
	//
	for( i = 0; i < numPolys; i++ )
	{
		PolyNeighbor *otherNeighbor;

		// get this poly neighbor ... this is "us"
		polyNeighbor = geomMesh->GetPolyNeighbor( i );

		// initialize ourselves to not be a visible edge
		visibleNeighborless = FALSE;

		// check our 3 potential neighbors
		for( j = 0; j < MAX_POLYGON_NEIGHBORS; j++ )
		{

			// initialize this neighbor to nuttin
			otherNeighbor = nullptr;

			// get our neighbor if present and cull them if processed
			if( polyNeighbor->neighbor[ j ].neighborIndex != NO_NEIGHBOR )
			{

				// get the jth polygon neighbor ... this is "them"
				otherNeighbor =
					geomMesh->GetPolyNeighbor(
						polyNeighbor->neighbor[ j ].neighborIndex );

				//
				// ignore neighbors that are marked as processed as those
				// ones have already detected edges if present
				//
				if( BitIsSet( otherNeighbor->status, POLY_PROCESSED ) )
					continue;  // for j

			}

			//
			// finally, if our own visible status is different from our
			// neighbor visible status then that defines an edge we must
			// add to the silhouette.  Also, a visible polygon that has
			// no neighbor automatically makes a silhouette edge.  Note that
			// if we have no neighbor we just record the fact that we have
			// real model end edges to add after this inner j loop;
			//
			if( BitIsSet( polyNeighbor->status, POLY_VISIBLE ) )
			{

				// check for no neighbor edges
				if( otherNeighbor == nullptr )
				{

					visibleNeighborless = TRUE;

				}
				else if( BitIsSet( otherNeighbor->status, POLY_VISIBLE ) == FALSE )
				{

					// "we" are visible and "they" are not
					addSilhouetteEdge(meshIndex, polyNeighbor, otherNeighbor );

				}

			}
			else if( otherNeighbor != nullptr &&
							 BitIsSet( otherNeighbor->status, POLY_VISIBLE ) )
			{

				// "they" are visible and "we" are not
				addSilhouetteEdge(meshIndex, otherNeighbor, polyNeighbor );

			}

		}

		//
		// if this polygon is visible, add any edges that are not
		// neighbors of adjacent polygons.
		//
		if( visibleNeighborless == TRUE )
		{

			addNeighborlessEdges(meshIndex, polyNeighbor );

		}

		//
		// this polyNeighbor is now considered "processed", any other
		// polygons that reference back to this one can ignore their
		// processing cause any edges were already detected
		//
		BitSet( polyNeighbor->status, POLY_PROCESSED );

	}

	//record number of edge indices contrinuted by this mesh
	m_numIndicesPerMesh[meshIndex]=m_numSilhouetteIndices[meshIndex]-meshEdgeStart;

}

// constructVolume ============================================================
// Given a fresh new geometry class called "shadowVolume" to hold the actual
// shadow volume data, this method will create the shadow volume polygons
// given the information in the current silhouette of this Shadow and the
// light source position.
//
// The light source should be in object space and the shadow volume polygon
// data is also constructed in object space.
//
// The polygon we will create for a given edge will be that edge extruded
// out in the direction away from the light source.  This conceptual 4 sided
// polygon is however broken up into two triangles for storage.
//
// This version is designed to construct the volume inside a system memory
// buffer - to be rendered via a dynamic vertex buffer.
//
// ============================================================================
void W3DVolumetricShadow::constructVolume( Vector3 *lightPosObject,Real shadowExtrudeDistance, Int volumeIndex, Int meshIndex )
{
	Geometry *shadowVolume;
	Vector3 extrude2;  // the polypoints extruded from edge and light
	Short indexList[ 3 ];
	Int i,k;
	Int vertexCount;
	Int polygonCount;
	Int indicesPerMesh;
	W3DShadowGeometryMesh *geomMesh;

	// sanity
	if( volumeIndex < 0 ||
			volumeIndex >= MAX_SHADOW_LIGHTS ||
			lightPosObject == nullptr )
	{

		assert( 0 );
		return;

	}

	// get the geometry struct we're storing the actual shadow volume data in
	shadowVolume = m_shadowVolume[ volumeIndex ][meshIndex];

	if( shadowVolume == nullptr )
	{

//		DBGPRINTF(( "No volume allocated at index '%d'\n", volumeIndex ));
		assert( 0 );
		return;

	}

	// step through each of the silhouette pairs
	vertexCount = 0;
	polygonCount = 0;

	indicesPerMesh=m_numIndicesPerMesh[meshIndex];
	if (!indicesPerMesh)
		return;	//nothing to draw

	geomMesh = m_geometry->getMesh(meshIndex);

	shadowVolume->SetNumActivePolygon(0);
	shadowVolume->SetNumActiveVertex(0);

#ifdef RECORD_SHADOW_STRIP_STATS
	Int numStrips=0;	//keeps track of number of strips generated.
	Int stripLength=1;	//keeps track of segments in strip (each being 2 triangles).
	Int maxStripLength=0;	//keeps track of longest strip generated.
#endif

	Short *silhouetteIndices=m_silhouetteIndex[meshIndex];

	//Initialize first strip info
	Short stripStartIndex=silhouetteIndices[ 0 ];
	Short stripStartVertex=0;

	//Insert the first vertex and extrusion into strip.
	// get edge point
	const Vector3& ev2=  // second edge of silhouette
		geomMesh->GetVertex( silhouetteIndices[ 0 ] );

	// take one edge point and extrude away from the light
	extrude2 = ev2 - *lightPosObject;
	extrude2 *= shadowExtrudeDistance;
	extrude2 += ev2;

	shadowVolume->SetVertex( vertexCount, &ev2 );
	shadowVolume->SetVertex( vertexCount + 1, &extrude2 );

	vertexCount=2;
	Int lastEdgeVertex2Index=0;
	Int lastExtrude2Index=1;

	for( i = 0; i < indicesPerMesh; i += 2 )
	{
		Short currentEdgeEnd=silhouetteIndices[i+1];

		//look for edge connected to this one and move it next to this edge
		//in index list.  Rendering edges back-to-back improves vertex cache usage.
		for (k=i+2; k<indicesPerMesh; k+=2)
			if (silhouetteIndices[k]==currentEdgeEnd)
			{	//swap the two edges
				// GeneralsX: Avoid strict-aliasing/alignment UB when swapping edge pairs on 64-bit/ARM.
				Short edge0a = silhouetteIndices[i+2];
				Short edge0b = silhouetteIndices[i+3];
				silhouetteIndices[i+2] = silhouetteIndices[k];
				silhouetteIndices[i+3] = silhouetteIndices[k+1];
				silhouetteIndices[k] = edge0a;
				silhouetteIndices[k+1] = edge0b;
				break;
			}

		if (k >= indicesPerMesh)
		{	//reached end of strip. Insert final edge.
			//Check if last edge wraps around to start.
			if (currentEdgeEnd == stripStartIndex)
			{	//add end of strip that wraps around to start (forming closed cylinder/shape)
				//
				// add the polygon consisting of the two edge vertices and the
				// first extruded point
				//
				indexList[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
				indexList[ 1 ] = lastExtrude2Index;  // lastextrude2 index
				indexList[ 2 ] = stripStartVertex;  // edgeVertex2 index
				shadowVolume->SetPolygonIndex( polygonCount, indexList );

				indexList[ 0 ] = stripStartVertex;  // edgeVertex2 index
				indexList[ 1 ] = lastExtrude2Index;  // extrude1 index
				indexList[ 2 ] = stripStartVertex+1;  // extrude2 index
				shadowVolume->SetPolygonIndex( polygonCount + 1, indexList );
			}
			else
			{	//add end of strip.  Finishes the last 2 polygons.
				const Vector3& ev=geomMesh->GetVertex(currentEdgeEnd);
				shadowVolume->SetVertex( vertexCount, &ev );

				//
				// add the polygon consisting of the two edge vertices and the
				// first extruded point
				//
				indexList[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
				indexList[ 1 ] = lastExtrude2Index;  // lastextrude2 index
				indexList[ 2 ] = vertexCount;  // edgeVertex2 index
				shadowVolume->SetPolygonIndex( polygonCount, indexList );

				// take the other edge point and extrude away from light
				extrude2 = ev - *lightPosObject;
				extrude2 *= shadowExtrudeDistance;
				extrude2 += ev;
				// add the one new vertex
				shadowVolume->SetVertex( vertexCount + 1, &extrude2 );

				indexList[ 0 ] = vertexCount;  // edgeVertex2 index
				indexList[ 1 ] = lastExtrude2Index;  // extrude1 index
				indexList[ 2 ] = vertexCount+1;  // extrude2 index
				shadowVolume->SetPolygonIndex( polygonCount + 1, indexList );

				lastEdgeVertex2Index=vertexCount;
				lastExtrude2Index=vertexCount+1;
				vertexCount += 2;
			}

			if ((i+2) >= indicesPerMesh)
			{	//finished with all silhouette edges
				polygonCount += 2;
#ifdef RECORD_SHADOW_STRIP_STATS
				numStrips++;
#endif
				break;	//reached end of all edges
			}

			//Start a new strip by adding first vertex and extrusion.
			const Vector3& ev=geomMesh->GetVertex( silhouetteIndices[ i+2 ] );
			// take one edge point and extrude away from the light
			extrude2 = ev - *lightPosObject;
			extrude2 *= shadowExtrudeDistance;
			extrude2 += ev;

			lastEdgeVertex2Index=vertexCount;
			lastExtrude2Index=vertexCount + 1;
			//record start of new strip info
			stripStartIndex=silhouetteIndices[ i+2 ];
			stripStartVertex=lastEdgeVertex2Index;

			shadowVolume->SetVertex( lastEdgeVertex2Index, &ev );
			shadowVolume->SetVertex( lastExtrude2Index, &extrude2 );
			vertexCount += 2;

			polygonCount += 2;
#ifdef RECORD_SHADOW_STRIP_STATS
			numStrips++;
			stripLength=1;
#endif
			continue;
		}
		else
		{	//continue existing strip by adding extra vertex and extrusion

			const Vector3& ev=geomMesh->GetVertex( currentEdgeEnd );
			shadowVolume->SetVertex( vertexCount, &ev );
			//
			// add the polygon consisting of the two edge vertices and the
			// first extruded point
			//
			indexList[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
			indexList[ 1 ] = lastExtrude2Index;  // lastextrude2 index
			indexList[ 2 ] = vertexCount;  // edgeVertex2 index
			shadowVolume->SetPolygonIndex( polygonCount, indexList );

			// take the other edge point and extrude away from light
			extrude2 = ev - *lightPosObject;
			extrude2 *= shadowExtrudeDistance;
			extrude2 += ev;

			// add the one new vertex
			shadowVolume->SetVertex( vertexCount + 1, &extrude2 );

			indexList[ 0 ] = vertexCount;  // edgeVertex2 index
			indexList[ 1 ] = lastExtrude2Index;  // extrude1 index
			indexList[ 2 ] = vertexCount+1;  // extrude2 index
			shadowVolume->SetPolygonIndex( polygonCount + 1, indexList );

			lastEdgeVertex2Index=vertexCount;
			lastExtrude2Index=vertexCount+1;

			vertexCount += 2;
			polygonCount += 2;
		}
#ifdef RECORD_SHADOW_STRIP_STATS
		//Continuing strip.
		stripLength++;
		// GeneralsX: Use MAX macro (cross-platform, defined in BaseTypeCore.h)
		maxStripLength=MAX(maxStripLength,stripLength);
#endif
	}

	shadowVolume->SetNumActivePolygon(polygonCount);
	shadowVolume->SetNumActiveVertex(vertexCount);
}

// constructVolumeVB ==========================================================
// Given a fresh new geometry class called "shadowVolume" to hold the actual
// shadow volume data, this method will create the shadow volume polygons
// given the information in the current silhouette of this Shadow and the
// light source position.
//
// The light source should be in object space and the shadow volume polygon
// data is also constructed in object space.
//
// The polygon we will create for a given edge will be that edge extruded
// out in the direction away from the light source.  This conceptual 4 sided
// polygon is however broken up into two triangles for storage.
//
// This version is designed to construct the volume directly inside a vertex
// buffer so it's optimal for static geometry.  Since it's assumed to be called
// only once per model, we can use some more expensive computations to generate
// the volume.
//
// ============================================================================
void W3DVolumetricShadow::constructVolumeVB( Vector3 *lightPosObject,Real shadowExtrudeDistance, Int volumeIndex, Int meshIndex )
{
	Geometry *shadowVolume;
	Vector3 extrude2;  // the polypoints extruded from edge and light
	Vector3 edgeVertex2;  // second edge of silhouette
	Int i,k;
	Int vertexCount;
	Int polygonCount;
	Int indicesPerMesh;
	W3DShadowGeometryMesh *geomMesh;

	W3DBufferManager::W3DVertexBufferSlot *vbSlot;
	W3DBufferManager::W3DIndexBufferSlot *ibSlot;

	// sanity
	if( volumeIndex < 0 ||
			volumeIndex >= MAX_SHADOW_LIGHTS ||
			lightPosObject == nullptr )
	{

		assert( 0 );
		return;

	}

	// get the geometry struct we're storing the actual shadow volume data in
	shadowVolume = m_shadowVolume[ volumeIndex ][meshIndex];

	if( shadowVolume == nullptr )
	{

//		DBGPRINTF(( "No volume allocated at index '%d'\n", volumeIndex ));
		assert( 0 );
		return;

	}

	//*****************************************************************************************/
	//Do an initial pass through silhouette data to determine the actual vertex/polygon counts.
	//This number can't be determined any other way since it depends on degree of vertex sharing
	//in model.  We don't want to overallocate because vertex buffer space is limited.
	//This pass is also used to sort the edges so they are all connected in strip order.
	{
		#ifdef RECORD_SHADOW_STRIP_STATS
			Int numStrips=0;	//keeps track of number of strips generated.
			Int stripLength=1;	//keeps track of segments in strip (each being 2 triangles).
			Int maxStripLength=0;	//keeps track of longest strip generated.
		#endif

		// step through each of the silhouette pairs
		vertexCount = 0;
		polygonCount = 0;

		indicesPerMesh=m_numIndicesPerMesh[meshIndex];
		if (!indicesPerMesh)
			return;	//nothing to draw

		Short *silhouetteIndices=m_silhouetteIndex[meshIndex];

		//Initialize first strip info
		Short stripStartIndex=silhouetteIndices[ 0 ];
		Short stripStartVertex=0;

		vertexCount=2;
		Int lastEdgeVertex2Index=0;
		Int lastExtrude2Index=1;

		for( i = 0; i < indicesPerMesh; i += 2 )
		{
			Short currentEdgeEnd=silhouetteIndices[i+1];

			//look for edge connected to this one and move it next to this edge
			//in index list.  Rendering edges back-to-back improves vertex cache usage.
			for (k=i+2; k<indicesPerMesh; k+=2)
				if (silhouetteIndices[k]==currentEdgeEnd)
				{	//swap the two edges
					// GeneralsX: Avoid strict-aliasing/alignment UB when swapping edge pairs on 64-bit/ARM.
					Short edge0a = silhouetteIndices[i+2];
					Short edge0b = silhouetteIndices[i+3];
					silhouetteIndices[i+2] = silhouetteIndices[k];
					silhouetteIndices[i+3] = silhouetteIndices[k+1];
					silhouetteIndices[k] = edge0a;
					silhouetteIndices[k+1] = edge0b;
					break;
				}

			if (k >= indicesPerMesh)
			{	//reached end of strip. Insert final edge.
				//Check if last edge wraps around to start.
				if (currentEdgeEnd == stripStartIndex)
				{	//add end of strip that wraps around to start (forming closed cylinder/shape)
					//
					// add the polygon consisting of the two edge vertices and the
					// first extruded point
					//
				}
				else
				{	//add end of strip.  Finishes the last 2 polygons.
					lastEdgeVertex2Index=vertexCount;
					lastExtrude2Index=vertexCount+1;
					vertexCount += 2;
				}

				if ((i+2) >= indicesPerMesh)
				{	//finished with all silhouette edges
					polygonCount += 2;
	#ifdef RECORD_SHADOW_STRIP_STATS
					numStrips++;
	#endif
					break;	//reached end of all edges
				}

				lastEdgeVertex2Index=vertexCount;
				lastExtrude2Index=vertexCount + 1;
				//record start of new strip info
				stripStartIndex=silhouetteIndices[ i+2 ];
				stripStartVertex=lastEdgeVertex2Index;

				vertexCount += 2;

				polygonCount += 2;
	#ifdef RECORD_SHADOW_STRIP_STATS
				numStrips++;
				stripLength=1;
	#endif
				continue;
			}
			else
			{	//continue existing strip by adding extra vertex and extrusion

				lastEdgeVertex2Index=vertexCount;
				lastExtrude2Index=vertexCount+1;

				vertexCount += 2;
				polygonCount += 2;
			}
	#ifdef RECORD_SHADOW_STRIP_STATS
			//Continuing strip.
			stripLength++;
			// GeneralsX: Use MAX macro (cross-platform)
			maxStripLength=MAX(maxStripLength,stripLength);
	#endif
		}
	}
	//***********************************************************************************************

	DEBUG_ASSERTCRASH(m_shadowVolumeVB[ volumeIndex ][meshIndex] == nullptr,("Updating Existing Static Vertex Buffer Shadow"));
	vbSlot=m_shadowVolumeVB[ volumeIndex ][meshIndex] = TheW3DBufferManager->getSlot(W3DBufferManager::VBM_FVF_XYZ,
		vertexCount);

	DEBUG_ASSERTCRASH(vbSlot != nullptr, ("Can't allocate vertex buffer slot for shadow volume"));
	if (vbSlot != nullptr)
	{
		DEBUG_ASSERTCRASH(vbSlot->m_size >= vertexCount,("Overflowing Shadow Vertex Buffer Slot"));
	}

	DEBUG_ASSERTCRASH(m_shadowVolume[ volumeIndex ][meshIndex]->GetNumPolygon() == 0,("Updating Existing Static Shadow Volume"));

	DEBUG_ASSERTCRASH(m_shadowVolumeIB[ volumeIndex ][meshIndex] == nullptr,("Updating Existing Static Index Buffer Shadow"));
	ibSlot=m_shadowVolumeIB[ volumeIndex ][meshIndex] = TheW3DBufferManager->getSlot(polygonCount*3);

	DEBUG_ASSERTCRASH(ibSlot != nullptr, ("Can't allocate index buffer slot for shadow volume"));
	if (ibSlot != nullptr)
	{
		DEBUG_ASSERTCRASH(ibSlot->m_size >= (polygonCount*3),("Overflowing Shadow Index Buffer Slot"));
	}

	if (!ibSlot || !vbSlot)
	{	//could not allocate storage to hold buffers
		if (ibSlot)
			TheW3DBufferManager->releaseSlot(ibSlot);
		if (vbSlot)
			TheW3DBufferManager->releaseSlot(vbSlot);

		m_shadowVolumeIB[ volumeIndex ][meshIndex]=nullptr;
		m_shadowVolumeVB[ volumeIndex ][meshIndex]=nullptr;
		return;
	}

	geomMesh = m_geometry->getMesh(meshIndex);

	// GeneralsX: The vertices go to a CPU copy in object space (the slot's size, so a write
	// stays as bounded as it was in the slot); RenderMeshVolume fills the slot from it, transformed to world space.
	shadowVolume->Release();
	if (!shadowVolume->Create(vbSlot->m_size, 0))
		return;
	VertexFormatXYZ *vb = (VertexFormatXYZ*)shadowVolume->GetVertex(0);

	if (vb == nullptr)
		return;

	DX8IndexBufferClass::AppendLockClass lockIdxBuffer(ibSlot->m_IB->m_DX8IndexBuffer,ibSlot->m_start,polygonCount*3);
	UnsignedShort *ib = (UnsignedShort*)lockIdxBuffer.Get_Index_Array();

	if (ib == nullptr)
		return;

	shadowVolume->SetNumActivePolygon(polygonCount);
	shadowVolume->SetNumActiveVertex(vertexCount);

	Short *silhouetteIndices=m_silhouetteIndex[meshIndex];

	//Initialize first strip info
	Short stripStartIndex=silhouetteIndices[ 0 ];
	Short stripStartVertex=0;

	//Insert the first vertex and extrusion into strip.
	// get edge point
	const Vector3& ev=geomMesh->GetVertex( silhouetteIndices[ 0 ] );

	// take one edge point and extrude away from the light
	extrude2 = ev - *lightPosObject;
	extrude2 *= shadowExtrudeDistance;
	extrude2 += ev;

	*vb++ = *(VertexFormatXYZ *)&ev;
	*vb++ = *(VertexFormatXYZ *)&extrude2;

	vertexCount=2;
	polygonCount=0;
	Int lastEdgeVertex2Index=0;
	Int lastExtrude2Index=1;

	for( i = 0; i < indicesPerMesh; i += 2 )
	{
		Short currentEdgeEnd=silhouetteIndices[i+1];

		//Check if another edge is connected to this one, edges were sorted in initial
		//pass so only need to check the next one.
		if (((i+2) >= indicesPerMesh) || silhouetteIndices[i+2] != currentEdgeEnd)
		{	//reached end of strip. Insert final edge.
			//Check if last edge wraps around to start.
			if (currentEdgeEnd == stripStartIndex)
			{	//add end of strip that wraps around to start (forming closed cylinder/shape)
				//
				// add the polygon consisting of the two edge vertices and the
				// first extruded point
				//
				ib[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
				ib[ 4 ] = ib[ 1 ] = lastExtrude2Index;  // lastextrude2 index
				ib[ 3 ] = ib[ 2 ] = stripStartVertex;  // edgeVertex2 index
				ib[ 5 ] = stripStartVertex+1;  // extrude2 index
				ib += 6;	//skip past 2 triangles just added.
			}
			else
			{	//add end of strip.  Finishes the last 2 polygons.
				const Vector3& ev=geomMesh->GetVertex( currentEdgeEnd );
				*vb++ = *(VertexFormatXYZ *)&ev;

				//
				// add the polygon consisting of the two edge vertices and the
				// first extruded point
				//
				ib[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
				ib[ 4 ] = ib[ 1 ] = lastExtrude2Index;  // lastextrude2 index
				ib[ 3 ] = ib[ 2 ] = vertexCount;  // edgeVertex2 index
				ib[ 5 ] = vertexCount+1;  // extrude2 index
				ib += 6;	//skip past 2 triangles just added.

				// take the other edge point and extrude away from light
				extrude2 = ev - *lightPosObject;
				extrude2 *= shadowExtrudeDistance;
				extrude2 += ev;
				// add the one new vertex
				*vb++ = *(VertexFormatXYZ *)&extrude2;

				lastEdgeVertex2Index=vertexCount;
				lastExtrude2Index=vertexCount+1;
				vertexCount += 2;
			}

			if ((i+2) >= indicesPerMesh)
			{	//finished with all silhouette edges
				polygonCount += 2;
				break;	//reached end of all edges
			}

			//Start a new strip by adding first vertex and extrusion.
			const Vector3& evb=geomMesh->GetVertex( silhouetteIndices[ i+2 ] );
			// take one edge point and extrude away from the light
			extrude2 = evb - *lightPosObject;
			extrude2 *= shadowExtrudeDistance;
			extrude2 += evb;

			lastEdgeVertex2Index=vertexCount;
			lastExtrude2Index=vertexCount + 1;
			//record start of new strip info
			stripStartIndex=silhouetteIndices[ i+2 ];
			stripStartVertex=lastEdgeVertex2Index;

			*vb++ = *(VertexFormatXYZ *)&evb;
			*vb++ = *(VertexFormatXYZ *)&extrude2;
			vertexCount += 2;

			polygonCount += 2;
			continue;
		}
		else
		{	//continue existing strip by adding extra vertex and extrusion

			const Vector3& ev=geomMesh->GetVertex( currentEdgeEnd );
			*vb++ = *(VertexFormatXYZ *)&ev;
			//
			// add the polygon consisting of the two edge vertices and the
			// first extruded point
			//
			ib[ 0 ] = lastEdgeVertex2Index;  // lastedgeVertex2 index
			ib[ 4 ] = ib[ 1 ] = lastExtrude2Index;  // lastextrude2 index
			ib[ 3 ] = ib[ 2 ] = vertexCount;  // edgeVertex2 index
			ib[ 5 ] = vertexCount+1;  // extrude2 index
			ib += 6;	//skip past 2 triangles just added

			// take the other edge point and extrude away from light
			extrude2 = ev - *lightPosObject;
			extrude2 *= shadowExtrudeDistance;
			extrude2 += ev;

			// add the one new vertex
			*vb++ = *(VertexFormatXYZ *)&extrude2;

			lastEdgeVertex2Index=vertexCount;
			lastExtrude2Index=vertexCount+1;

			vertexCount += 2;
			polygonCount += 2;
		}
	}

//	DEBUG_ASSERTLOG(polygonCount == vertexCount, ("WARNING***Shadow volume mesh not optimal: %s",m_geometry->Get_Name()));
}

// allocateShadowVolume =======================================================
// Allocate a space for us to construct the shadow volume in
// ============================================================================
Bool W3DVolumetricShadow::allocateShadowVolume( Int volumeIndex, Int meshIndex )
{
	Int numVertices, numPolygons;
	Geometry *shadowVolume;

	// sanity
	if( volumeIndex < 0 || volumeIndex >= MAX_SHADOW_LIGHTS )
	{

//		DBGPRINTF(( "Illegal allocate shadow volume index '%d'\n", volumeIndex ));
		assert( 0 );
		return FALSE;

	}

	if ((shadowVolume = m_shadowVolume[ volumeIndex ][meshIndex]) == nullptr)
	{
		// poolify
		shadowVolume = NEW Geometry;		// create the new geometry
		// we now have one more valid geometry volume
		m_shadowVolumeCount[meshIndex]++;
	}

	if( shadowVolume == nullptr )
	{

//		DBGPRINTF(( "Unable to allocate '%d' shadow volume\n", volumeIndex ));
		assert( 0 );
		// we now have one more valid geometry volume
		m_shadowVolumeCount[meshIndex]--;
		return FALSE;

	}

	// assign to list
	m_shadowVolume[ volumeIndex ][meshIndex] = shadowVolume;

	//
	// polygons are determined from the edges extruded from the light.
	// since we have a list of disjoint edge pairs we will have a 4 sided
	// polygon for each edge pair, however we will be splitting the
	// 4 sided polys into 2 triangles so it just works out that num polys
	// is the number of silhouette indices
	//
	numPolygons = m_maxSilhouetteEntries[meshIndex];

	//
	// vertices are an extrusion of the edges, and since we have disjoint
	// pairs of edge indices we will have num indices * 2 actual vertex
	// points.  it may be a good future optimization to not duplicate
	// any vertices that appear twice here, but then again the cost over
	// optimizing this routine over the rendering and transformation
	// optimization may not be worth it
	//
	numVertices = m_maxSilhouetteEntries[meshIndex] * 2;

	//Only allocate space here for dynamic shadows.  Shadows for static/non-animated
	//models will be stored in vertex buffers which are allocated once exact size
	//is known.
	if (shadowVolume->GetFlags() & SHADOW_DYNAMIC)
	{
		//for dynamic shadow casters, we need to allocate the maximum amount of vertices that could ever be required.
//		if (m_shadowVolumeVB[ volumeIndex ][meshIndex])
//			TheW3DBufferManager->releaseSlot(m_shadowVolumeVB[ volumeIndex ][meshIndex]);
//		m_shadowVolumeVB[ volumeIndex ][meshIndex] = TheW3DVertexBufferManager->getSlot(W3DVertexBufferManager::VBM_FVF_XYZ, numVertices);

		// allocate memory for the vertices and polygons
		if( shadowVolume->Create( numVertices, numPolygons ) == FALSE )
		{

	//		DBGPRINTF(( "Unable to create shadow volume\n" ));
			assert( 0 );
			delete shadowVolume;
			return FALSE;

		}
	}

	return TRUE;  // success

}

// deleteShadowVolume =========================================================
// Free all resources allocated to the shadow volume(s)
// ============================================================================
void W3DVolumetricShadow::deleteShadowVolume( Int volumeIndex )
{

	// sanity
	if( volumeIndex < 0 || volumeIndex >= MAX_SHADOW_LIGHTS )
	{

//		DBGPRINTF(( "Illegal delete shadow volume index '%d'\n", volumeIndex ));
		assert( 0 );
		return;

	}

	// delete it!
	for (Int meshIndex=0; meshIndex<MAX_SHADOW_CASTER_MESHES; meshIndex++)
	{
		if( m_shadowVolume[ volumeIndex ][meshIndex] )
		{

			delete m_shadowVolume[ volumeIndex ][meshIndex];
			m_shadowVolume[ volumeIndex ][meshIndex] = nullptr;

			// we now have one less shadow volume
			m_shadowVolumeCount[meshIndex]--;

		}
	}

}

// resetShadowVolume ==========================================================
// Reset the contents of the shadow volume information.  Since we're using
// a geometry class it would be ideal if these structures had a reset
// option where their resources were released back to a pool rather than
// delete and allocate new storage space
// ============================================================================
void W3DVolumetricShadow::resetShadowVolume( Int volumeIndex, Int meshIndex )
{
	Geometry *geometry;

	// sanity
	if( volumeIndex < 0 || volumeIndex >= MAX_SHADOW_LIGHTS )
	{

//		DBGPRINTF(( "Illegal reset shadow volume index '%d'\n", volumeIndex ));
		assert( 0 );
		return;

	}

	geometry = m_shadowVolume[ volumeIndex ][meshIndex];

	//Release buffers used to hold shadow volume geometry
	if (geometry)
	{	if (m_shadowVolumeVB[volumeIndex][meshIndex])
		{	TheW3DBufferManager->releaseSlot(m_shadowVolumeVB[volumeIndex][meshIndex]);
			m_shadowVolumeVB[volumeIndex][meshIndex]=nullptr;
		}
		if (m_shadowVolumeIB[ volumeIndex ][meshIndex])
		{	TheW3DBufferManager->releaseSlot(m_shadowVolumeIB[volumeIndex][meshIndex]);
			m_shadowVolumeIB[volumeIndex][meshIndex]=nullptr;
		}
		geometry->Release();
	}

}

// allocateSilhouette =========================================================
// Allocate space for new silhouette storage, the number of vertices passed
// in is the total vertices in the model, a silhouette must be able to
// accommodate that as a series of disjoint edge pairs, otherwise known
// as numVertices * 2
// ============================================================================
Bool W3DVolumetricShadow::allocateSilhouette(Int meshIndex, Int numVertices )
{
	Int numEntries = numVertices * 5;	///@todo: HACK, HACK... Should be 2!

	// sanity
	assert( m_silhouetteIndex[meshIndex] == nullptr &&
					m_numSilhouetteIndices[meshIndex] == 0 &&
					numEntries > 0 );

	// allocate memory
	m_silhouetteIndex[meshIndex] = NEW short[ numEntries ];
	if( m_silhouetteIndex[meshIndex] == nullptr )
	{

//		DBGPRINTF(( "Unable to allocate silhouette storage '%d'\n", numEntries ));
		assert( 0 );
		return FALSE;

	}

	// set our list to empty just to be clean
	m_numSilhouetteIndices[meshIndex] = 0;

	// save the size of our silhouette list
	m_maxSilhouetteEntries[meshIndex] = numEntries;

	return TRUE;  // success

}

// deleteSilhouette ===========================================================
// Delete all silhouette data and memory allocated
// ============================================================================
void W3DVolumetricShadow::deleteSilhouette( Int meshIndex )
{

	delete [] m_silhouetteIndex[meshIndex];
	m_silhouetteIndex[meshIndex] = nullptr;
	m_numSilhouetteIndices[meshIndex] = 0;

}

// resetSilhouette ============================================================
// Resets the silhouette to empty, it does NOT free any of the memory
// allocated for silhouette data
// ============================================================================
void W3DVolumetricShadow::resetSilhouette( Int meshIndex )
{

	m_numSilhouetteIndices[meshIndex] = 0;

}

// renderStencilShadows =======================================================
// The stencil buffer now has our shadow information in it, take that
// info and draw a big transparent rectangle over the screen for the final
// shadow pass wherever there is data in the stencil buffer
// ============================================================================
void W3DVolumetricShadowManager::renderStencilShadows()
{
	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	if (!m_pDev)
		return;	//need device to render anything.

	struct _TRANSLITVERTEX {
	    D3DXVECTOR4 p;
		DWORD color;   // diffuse color
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

    v[0].p = D3DXVECTOR4( xpos+width, ypos+height, 0.0f, 1.0f );
    v[1].p = D3DXVECTOR4( xpos+width, 0, 0.0f, 1.0f );
    v[2].p = D3DXVECTOR4(  xpos, ypos+height, 0.0f, 1.0f );
    v[3].p = D3DXVECTOR4(  xpos,  0, 0.0f, 1.0f );
    v[0].color = TheW3DShadowManager->getShadowColor();
    v[1].color = TheW3DShadowManager->getShadowColor();
    v[2].color = TheW3DShadowManager->getShadowColor();
    v[3].color = TheW3DShadowManager->getShadowColor();

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	m_pDev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

	// Use alpha blending to draw the transparent shadow
    GxRawRS(m_pDev,  D3DRS_ALPHABLENDENABLE, TRUE );
//  GxRawRS(m_pDev,  D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA );
//  GxRawRS(m_pDev,  D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
		GxRawRS(m_pDev,  D3DRS_SRCBLEND,  D3DBLEND_DESTCOLOR);
		GxRawRS(m_pDev,  D3DRS_DESTBLEND, D3DBLEND_ZERO );


	// Set stencil states
    GxRawRS(m_pDev,  D3DRS_ZENABLE,          TRUE );
		GxRawRS(m_pDev, D3DRS_ZFUNC, D3DCMP_ALWAYS);

	// Only write where stencil val >= 1 (count indicates # of shadows that
	// overlap that pixel)
    GxRawRS(m_pDev,  D3DRS_STENCILENABLE, TRUE );
    GxRawRS(m_pDev,  D3DRS_STENCILFUNC, D3DCMP_LESSEQUAL );	//reference value is less or equal to stencil
    GxRawRS(m_pDev,  D3DRS_STENCILPASS, D3DSTENCILOP_KEEP );
	//Upper bits of stencil could be used for storing occluded models which are player colored.  So we mask out those
	//pixels and only use the lower bits for shadow calculations.
	GxRawRS(m_pDev,  D3DRS_STENCILMASK,     ~TheW3DShadowManager->getStencilShadowMask());
    GxRawRS(m_pDev,  D3DRS_STENCILREF,      0x1 );


	GxRawRS(m_pDev, D3DRS_SHADEMODE, D3DSHADE_FLAT);

	if (DX8Wrapper::_Is_Triangle_Draw_Enabled())
		{ m_pDev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANSLITVERTEX)); DX8Wrapper::Record_Direct_Draw(); }

	GxRawRS(m_pDev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	GxRawRS(m_pDev,  D3DRS_ALPHABLENDENABLE, FALSE );
	// turn off the stencil buffer
	GxRawRS(m_pDev,  D3DRS_STENCILENABLE, FALSE );

}

void W3DVolumetricShadowManager::renderShadows( Bool forceStencilFill )
{
	// GeneralsX: S7: with the shadow map on this frame, no volume
	// is updated or drawn and no darkening quad: the map's receivers darken instead (step 3 = off brings all of this back)
	if (GxShadowMapActive)
		return;
	GxShadowScope gxAll(GXS_T_ALL);	// [GXSHADOW] (GameClient/GXGfxProbe.h)
	W3DVolumetricShadow *shadow;
	Int numRenderedShadows = 0;

 	AABoxClass bbox;

 	//Get a bounding box around our visible universe.  Bounded by terrain and the sky
 	//so much tighter fitting volume than what's actually visible.  This will cull
 	//particles falling under the ground.

 	TheTerrainRenderObject->getMaximumVisibleBox(*shadowCameraFrustum, &bbox, TRUE);

 	bcX = bbox.Center.X;
 	bcY = bbox.Center.Y;
 	bcZ = bbox.Center.Z;
 	beX = bbox.Extent.X;
 	beY = bbox.Extent.Y;
 	beZ = bbox.Extent.Z;

	if (!m_shadowList.empty() && TheGlobalData->m_useShadowVolumes)
	{

		LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

		if (!m_pDev)
			return;	//need device to render anything.

 		//According to Nvidia there's a D3D bug that happens if you don't start with a
 		//new dynamic VB each frame - so we force a DISCARD by overflowing the counter.
		nShadowIndicesInBuf = SHADOW_INDEX_SIZE;	// GeneralsX: was 0xffff, which only overflows a buffer of <= 64K entries
		nShadowVertsInBuf = SHADOW_VERTEX_SIZE;
		bumpShadowUploadEpoch();	// GeneralsX: no upload from an earlier frame is ever redrawn
		shadowScreenSizeBegin();

		//Set W3D to some known state
		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);

		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);	//turn off textures
		DX8Wrapper::Set_Texture(1,nullptr);	//turn off textures
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		// turn off z writing
		GxRawRS(m_pDev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	  GxRawRS(m_pDev,  D3DRS_ZENABLE,          TRUE );
		GxRawRS(m_pDev, D3DRS_ZWRITEENABLE , FALSE);
		GxRawRS(m_pDev, D3DRS_ALPHATESTENABLE, FALSE);
		GxRawRS(m_pDev, D3DRS_FOGENABLE, FALSE);


		// setup the TMU to default
		GxRawRS(m_pDev, D3DRS_SHADEMODE, D3DSHADE_FLAT);
		GxRawRS(m_pDev, D3DRS_LIGHTING, FALSE);
		GxRawTSS(m_pDev,  0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		GxRawTSS(m_pDev,  0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		GxRawTSS(m_pDev,  0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
		GxRawTSS(m_pDev,  0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		GxRawTSS(m_pDev,  0, D3DTSS_TEXCOORDINDEX, 0 );

		GxRawTSS(m_pDev,  1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
		GxRawTSS(m_pDev,  1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		GxRawTSS(m_pDev,  1, D3DTSS_TEXCOORDINDEX, 1 );
		m_pDev->SetTexture(0,nullptr);
		m_pDev->SetTexture(1,nullptr);

		DWORD oldColorWriteEnable=0x12345678;

	#ifdef SV_DEBUG
		GxRawRS(m_pDev, D3DRS_ALPHABLENDENABLE , TRUE);
		GxRawRS(m_pDev,  D3DRS_STENCILENABLE, FALSE );
		GxRawRS(m_pDev,  D3DRS_SRCBLEND, /*D3DBLEND_DESTCOLOR*/D3DBLEND_ONE );
		GxRawRS(m_pDev,  D3DRS_DESTBLEND, D3DBLEND_ZERO );
		GxRawRS(m_pDev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	#else
		//disable writes to color buffer
		if (DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps().PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE)
		{	DX8Wrapper::_Get_D3D_Device8()->GetRenderState(D3DRS_COLORWRITEENABLE, &oldColorWriteEnable);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,0);
		}
		else
		{	//device does not support disabling writes to color buffer so fake it through alpha blending
			GxRawRS(m_pDev,  D3DRS_SRCBLEND, D3DBLEND_ZERO );
			GxRawRS(m_pDev,  D3DRS_DESTBLEND, D3DBLEND_ONE );
			GxRawRS(m_pDev, D3DRS_ALPHABLENDENABLE , TRUE);
		}
		GxRawRS(m_pDev,  D3DRS_STENCILENABLE, TRUE );
	#endif
		//Any pixels with stencil already set to 128 contains a potential occluder.  If this pixels also has any of the player
		//color stencil bits also set, it means that it's an occluded player color and we need to NOT render shadows here.  We
		//do this determination by comparing the value in the combined bits against a value containing only a potential occluder.
		//If the value of just the potential occluder bit is >= than the combined bits, then we know none of the player color
		//bits were set and it's okay to render shadow.
		if (TheW3DShadowManager->getStencilShadowMask() == 0x80808080)
			GxRawRS(m_pDev,  D3DRS_STENCILFUNC,     D3DCMP_NOTEQUAL );	//in this mode, MSB indicates occluded player pixels.
		else
			GxRawRS(m_pDev,  D3DRS_STENCILFUNC,     D3DCMP_GREATEREQUAL );	//in this mode, multiple bits indicate occluded player pixels.
		GxRawRS(m_pDev,  D3DRS_STENCILREF,      0x80808080 );			//isolate MSB, it's used to indicate pixels containing potential occluders.
		GxRawRS(m_pDev,  D3DRS_STENCILMASK,     TheW3DShadowManager->getStencilShadowMask());	//isolate upper bits containing PotentialOccluderBit|PlayerColorBits
		GxRawRS(m_pDev,  D3DRS_STENCILWRITEMASK,0xffffffff );
		GxRawRS(m_pDev,  D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP );
		GxRawRS(m_pDev,  D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );
		GxRawRS(m_pDev,  D3DRS_STENCILPASS,  D3DSTENCILOP_INCR );

		m_pDev->SetVertexShader(SHADOW_DYNAMIC_VOLUME_FVF);

		GxRawRS(m_pDev, D3DRS_CULLMODE,D3DCULL_CW);
//		GxRawRS(m_pDev, D3DRS_ZBIAS,1);	///@todo: See if this helps or makes things worse.
		//GxRawRS(m_pDev, D3DRS_FILLMODE,D3DFILL_WIREFRAME);


		lastActiveVertexBuffer=nullptr;	//reset
		s_shadowWorldValid = FALSE;	//someone else may have set the world transform since

		m_dynamicShadowVolumesToRender=nullptr;	//clear list of pending dynamic shadows
		gxShadowGpuBeginFrame();	// this frame's GPU volumes, queued by RenderGpuMeshVolume below
		s_gxGpuQueueOpen = TRUE;
		W3DVolumetricShadowRenderTask *shadowDynamicTasksStart,*shadowDynamicTask;
		gxShadowAdd(GXS_CAST, (uint64_t)m_shadowList.size());
		const int64_t gxIncr0 = gxShadowT0();
		int64_t gxUpdNs = 0;

		s_shadowBatching = TRUE;	// dynamic volumes append to one draw per batch (flushShadowBatch)
		s_batchBase = -1;
		s_runBase = -1;
		// step through each of our shadows and render, newest first (the old chain order)
		for( size_t n = m_shadowList.size(); n-- > 0; )
		{
			if (n >= GX_SHADOW_PREFETCH_AHEAD)
				GX_SHADOW_PREFETCH(m_shadowList[n - GX_SHADOW_PREFETCH_AHEAD]);	// its enable flags, ahead of need
			shadow = m_shadowList[n];
			if (shadow->m_isEnabled && !shadow->m_isInvisibleEnabled)
			{
				//Record last added task
				shadowDynamicTasksStart=m_dynamicShadowVolumesToRender;
				gxShadowAdd(GXS_CASTON);
				const int64_t gxUpd0 = gxShadowT0();
				shadow->Update();
				if (gxUpd0 != 0)
					gxUpdNs += gxGfxWallNs() - gxUpd0;
				shadowDynamicTask=m_dynamicShadowVolumesToRender;
				while (shadowDynamicTask != shadowDynamicTasksStart)
				{	//update() added a dynamic shadow
					//dynamic shadow columns don't need to wait in queue since they
					//all use the same vertex buffer.  Flush them ASAP.
					gxShadowAdd(GXS_DYNV);
					shadow->RenderVolume(shadowDynamicTask->m_meshIndex,shadowDynamicTask->m_lightIndex);
					//move to next dynamic task
					shadowDynamicTask=(W3DVolumetricShadowRenderTask *)shadowDynamicTask->m_nextTask;
					numRenderedShadows++;
				}
			}
		}

		flushShadowBatch(m_pDev);	// the batched dynamic volumes, still before every static one as before
		s_shadowBatching = FALSE;
		s_gxGpuQueueOpen = FALSE;
		gxShadowGpuFlush(m_pDev, SHADOW_DYNAMIC_VOLUME_FVF, TRUE);	// the GPU volumes (GX_SHADOW_GPU), INCR

		// Set vertex format to that used by static shadow volumes
		m_pDev->SetVertexShader(W3DBufferManager::getDX8Format(W3DBufferManager::VBM_FVF_XYZ));

		//Empty queue of static shadow volumes to render.
		W3DBufferManager::W3DVertexBuffer *nextVb;
		W3DVolumetricShadowRenderTask *nextTask;
		for (nextVb=TheW3DBufferManager->getNextVertexBuffer(nullptr,W3DBufferManager::VBM_FVF_XYZ);nextVb != nullptr; nextVb=TheW3DBufferManager->getNextVertexBuffer(nextVb,W3DBufferManager::VBM_FVF_XYZ))
		{
			nextTask=(W3DVolumetricShadowRenderTask *)nextVb->m_renderTaskList;
			while (nextTask)
			{
				gxShadowAdd(GXS_STATV);
				nextTask->m_parentShadow->RenderVolume(nextTask->m_meshIndex,nextTask->m_lightIndex);
				nextTask=(W3DVolumetricShadowRenderTask *)nextTask->m_nextTask;
				numRenderedShadows++;
			}
		}
		if (gxIncr0 != 0)
		{	// [GXSHADOW]: the INCR pass (both loops above) without the Update() time inside it
			gxShadowAdd(GXS_T_UPD, (uint64_t)gxUpdNs);
			gxShadowAdd(GXS_T_INCR, (uint64_t)(gxGfxWallNs() - gxIncr0 - gxUpdNs));
		}
		const int64_t gxDecr0 = gxShadowT0();

		// change the stencil op to decrement
		GxRawRS(m_pDev,  D3DRS_STENCILPASS,  D3DSTENCILOP_DECRSAT);

		//
		// invert normals of shadow volumes so we can decrement in the
		// stencil buffer and render
		//

		GxRawRS(m_pDev, D3DRS_CULLMODE,D3DCULL_CCW);

		for (nextVb=TheW3DBufferManager->getNextVertexBuffer(nullptr,W3DBufferManager::VBM_FVF_XYZ);nextVb != nullptr; nextVb=TheW3DBufferManager->getNextVertexBuffer(nextVb,W3DBufferManager::VBM_FVF_XYZ))
		{
			nextTask=(W3DVolumetricShadowRenderTask *)nextVb->m_renderTaskList;
			while (nextTask)
			{
				nextTask->m_parentShadow->RenderVolume(nextTask->m_meshIndex,nextTask->m_lightIndex);
				nextTask=(W3DVolumetricShadowRenderTask *)nextTask->m_nextTask;
			}
		}

		m_pDev->SetVertexShader(SHADOW_DYNAMIC_VOLUME_FVF);
		s_shadowReuseUploads = TRUE;	// GeneralsX: redraw the INCR pass's uploads (TS-06)
		//flush any dynamic shadow volumes
		shadowDynamicTask=m_dynamicShadowVolumesToRender;
		while (shadowDynamicTask)
		{	//dynamic shadow columns don't need to wait in queue since they
			//all use the same vertex buffer.  Flush them ASAP.
			shadowDynamicTask->m_parentShadow->RenderVolume(shadowDynamicTask->m_meshIndex,shadowDynamicTask->m_lightIndex);
			shadowDynamicTask=(W3DVolumetricShadowRenderTask *)shadowDynamicTask->m_nextTask;
		}
		flushShadowRun(m_pDev);	// the last reused range
		s_shadowReuseUploads = FALSE;
		gxShadowGpuFlush(m_pDev, SHADOW_DYNAMIC_VOLUME_FVF, FALSE);	// the same GPU volumes again, DECR
		gxShadowGpuBeginFrame();
		gxShadowTAdd(GXS_T_DECR, gxDecr0);

		//Reset all render tasks for next frame.
		for (nextVb=TheW3DBufferManager->getNextVertexBuffer(nullptr,W3DBufferManager::VBM_FVF_XYZ);nextVb != nullptr; nextVb=TheW3DBufferManager->getNextVertexBuffer(nextVb,W3DBufferManager::VBM_FVF_XYZ))
		{
			nextVb->m_renderTaskList=nullptr;
		}

		GxRawRS(m_pDev, D3DRS_CULLMODE,D3DCULL_CW);
//		GxRawRS(m_pDev, D3DRS_ZBIAS,0);	///@todo: See if this helps or makes things worse.
		//GxRawRS(m_pDev, D3DRS_FILLMODE,D3DFILL_SOLID);


		if (oldColorWriteEnable != 0x12345678)
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,oldColorWriteEnable);

		//
		// render the big transparent square of shadows in the stencil buffer
		// to the screen
		//
///@todo: Put this check back in after water is fixed so it doesn't require shadow rendering to fix alpha.
//		if (numRenderedShadows)
		{
			const int64_t gxQuad0 = gxShadowT0();
			renderStencilShadows();
			gxShadowTAdd(GXS_T_QUAD, gxQuad0);
		}

		GxRawRS(m_pDev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		GxRawRS(m_pDev, D3DRS_ALPHABLENDENABLE , FALSE);
		GxRawRS(m_pDev, D3DRS_LIGHTING, FALSE);

		DX8Wrapper::Invalidate_Cached_Render_States();
	}
	else
	if (forceStencilFill)
	{	//no shadows to render, but still need to fill stencil buffer
		//for other effects.

		//Set W3D to some known state
		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		renderStencilShadows();

		DX8Wrapper::Invalidate_Cached_Render_States();
	}

}

/** This class will manage shadow geometry for each render object.  Shadow geometry may
be the same as render geometry but doesn't need to be.  This allows lower LOD versions of
the geometry to be used in shadow calculations.  Shadow geometry also keeps extended vertex
connectivity information that's not used during rendering.
*/
class W3DShadowGeometryManager
{
public:
	W3DShadowGeometryManager();
	~W3DShadowGeometryManager();

	int			 		Load_Geom(RenderObjClass *robj, const char *name);
	W3DShadowGeometry *		Get_Geom(const char * name);
	W3DShadowGeometry *		Peek_Geom(const char * name);
	Bool					Add_Geom(W3DShadowGeometry *new_anim);
	void			 		Free_All_Geoms();

	void					Register_Missing( const char * name );
	Bool					Is_Missing( const char * name );
	void					Reset_Missing();

private:

	HashTableClass	*	GeomPtrTable;
	HashTableClass	*	MissingGeomTable;

	friend	class		W3DShadowGeometryManagerIterator;
};

/*
** An Iterator to get to all loaded W3DShadowGeometries in a W3DShadowGeometryManager
*/
class W3DShadowGeometryManagerIterator : public HashTableIteratorClass {
public:
	W3DShadowGeometryManagerIterator( W3DShadowGeometryManager & manager ) : HashTableIteratorClass( *manager.GeomPtrTable ) {}
	W3DShadowGeometry * Get_Current_Geom();
};

/** Used to cause a rebuild of all shadow volumes*/
void W3DVolumetricShadowManager::invalidateCachedLightPositions()
{

	if (m_shadowList.empty())
		return;	//there are no shadows to render.

	W3DVolumetricShadow *shadow;
	Vector3 vec(0,0,0);

	// step through each of our shadows and update previous light position.
	for( size_t n = m_shadowList.size(); n-- > 0; )
	{
		shadow = m_shadowList[n];
		for(Int i = 0; i < MAX_SHADOW_LIGHTS; i++ )
		{
			for (Int meshIndex=0; meshIndex<MAX_SHADOW_CASTER_MESHES; meshIndex++)
			{
				shadow->setLightPosHistory(i,meshIndex,vec);
				if (shadow->m_shadowVolume[i][meshIndex])
					shadow->m_shadowVolume[i][meshIndex]->clearWorldFilled();	// GeneralsX: its slot may be a new, empty buffer
			}
		}
	}
}

// W3DVolumetricShadowManager =============================================================
// ============================================================================
W3DVolumetricShadowManager::W3DVolumetricShadowManager()
{

	m_W3DShadowGeometryManager = NEW W3DShadowGeometryManager;

	TheW3DBufferManager = NEW W3DBufferManager;

}

// ~W3DVolumetricShadowManager ============================================================
// ============================================================================
W3DVolumetricShadowManager::~W3DVolumetricShadowManager()
{
	ReleaseResources();
	delete m_W3DShadowGeometryManager;
	m_W3DShadowGeometryManager = nullptr;
	delete TheW3DBufferManager;
	TheW3DBufferManager=nullptr;

	//all shadows should be freed up at this point but check anyway
	assert(m_shadowList.empty());

}

/** Releases all W3D/D3D assets before a reset.. */
void W3DVolumetricShadowManager::ReleaseResources()
{
	if (shadowIndexBufferD3D)
		shadowIndexBufferD3D->Release();
	if (shadowVertexBufferD3D)
		shadowVertexBufferD3D->Release();
	shadowIndexBufferD3D=nullptr;
	shadowVertexBufferD3D=nullptr;
	gxShadowGpuReleaseAll();	// edge meshes and the shader are rebuilt on next use
	if (TheW3DBufferManager)
	{	TheW3DBufferManager->ReleaseResources();
		invalidateCachedLightPositions();	//vertex buffers need to be refilled.
	}
}

/** (Re)allocates all W3D/D3D assets after a reset.. */
Bool W3DVolumetricShadowManager::ReAcquireResources()
{
	ReleaseResources();

	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	DEBUG_ASSERTCRASH(m_pDev, ("Trying to ReAcquireResources on W3DVolumetricShadowManager without device"));

	if (FAILED(m_pDev->CreateIndexBuffer
	(
		SHADOW_INDEX_SIZE*sizeof(WORD),
		D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC,
		D3DFMT_INDEX16,
		D3DPOOL_DEFAULT,
		&shadowIndexBufferD3D
	)))
		return FALSE;

	if (shadowVertexBufferD3D == nullptr)
	{	// Create vertex buffer

		if (FAILED(m_pDev->CreateVertexBuffer
		(
			SHADOW_VERTEX_SIZE*sizeof(SHADOW_DYNAMIC_VOLUME_VERTEX),
			D3DUSAGE_WRITEONLY|D3DUSAGE_DYNAMIC,
			0,
			D3DPOOL_DEFAULT,
			&shadowVertexBufferD3D
		)))
			return FALSE;
	}

	if (TheW3DBufferManager)
		if (!TheW3DBufferManager->ReAcquireResources())
			return FALSE;

	return TRUE;
}

// Init =======================================================================
// User called initialization
// ============================================================================
Bool W3DVolumetricShadowManager::init()
{
	return TRUE;
}

// Reset ======================================================================
// Reset our list of shadows to empty
// ============================================================================
void W3DVolumetricShadowManager::reset()
{

	assert (m_shadowList.empty());
	m_W3DShadowGeometryManager->Free_All_Geoms();
	TheW3DBufferManager->freeAllBuffers();

}

// addShadow ==================================================================
// Add the shadows for this hierarchy to the shadow management for
// rendering.
// ============================================================================
W3DVolumetricShadow* W3DVolumetricShadowManager::addShadow(RenderObjClass *robj, Shadow::ShadowTypeInfo *shadowInfo, Drawable *draw)
{
	if (!DX8Wrapper::Has_Stencil() || !robj || !TheGlobalData->m_useShadowVolumes)
		return nullptr;	//right now we require a stencil buffer

	W3DShadowGeometry *sg=nullptr;
	if (!robj)
		return nullptr;	//must have a render object in order to read shadow geometry

	const char *name=robj->Get_Name();

	if (!name)
		return nullptr;

	sg=m_W3DShadowGeometryManager->Get_Geom(name);

	if (sg==nullptr)
	{	//did not find a cached copy of the shadow geometry, create a new one
		m_W3DShadowGeometryManager->Load_Geom(robj,name);
		//try loading again
		sg=m_W3DShadowGeometryManager->Get_Geom(name);
		if (sg==nullptr)
			return nullptr;	//could not create the shadow geometry
	}

	W3DVolumetricShadow *shadow = NEW W3DVolumetricShadow;	// poolify

	// sanity
	if( shadow == nullptr )
		return nullptr;

	shadow->setRenderObject(robj);
	shadow->SetGeometry(sg);
 	SphereClass sphere;
 	robj->Get_Obj_Space_Bounding_Sphere(sphere);
 	shadow->setRenderObjExtent(sphere.Radius*MAX_SHADOW_LENGTH_SCALE_FACTOR);

	Real sunElevationAngleTan = 0;
	if (shadowInfo->m_sizeX)
	{	//need to adjust sun elevation for this model in order to limit shadow length
		sunElevationAngleTan=tan(shadowInfo->m_sizeX/180.0f*PI);
	}
	shadow->setShadowLengthScale(sunElevationAngleTan);
	shadow->m_gxTemplateLengthScale = sunElevationAngleTan;	// S7 A39 M5

	if (!draw || !draw->isKindOf(KINDOF_IMMOBILE))
		shadow->setOptimalExtrusionPadding(SHADOW_EXTRUSION_BUFFER);

	// add to our shadow list (newest last; walks run it backwards)
	m_shadowList.push_back(shadow);
	return shadow;
}

/** removeShadow ===========================================================
 Removes the shadows for this hierarchy from the shadow manger.  No further
 shadows from this caster will be rendered.
 ===========================================================================
*/
void W3DVolumetricShadowManager::removeShadow(W3DVolumetricShadow *shadow)
{
	//search for this shadow, newest first as the chain did
	for( size_t n = m_shadowList.size(); n-- > 0; )
	{
		if (m_shadowList[n] == shadow)
		{
			m_shadowList.erase(m_shadowList.begin() + n);
			delete shadow;
			break;
		}
	}
}

/** removeAllShadows ===========================================================
 Removes all shadows from the shadow manger.  No further
 shadows will be rendered.
 ===========================================================================
*/
void W3DVolumetricShadowManager::removeAllShadows()
{
	// empty the list first, then free newest first as the chain did
	std::vector<W3DVolumetricShadow *> shadows;
	shadows.swap(m_shadowList);
	for( size_t n = shadows.size(); n-- > 0; )
		delete shadows[n];
}

W3DShadowGeometryManager::W3DShadowGeometryManager()
{
	// Create the hash tables
	GeomPtrTable = NEW HashTableClass( 2048 );
	MissingGeomTable = NEW HashTableClass( 2048 );
}

W3DShadowGeometryManager::~W3DShadowGeometryManager()
{
	Free_All_Geoms();

	delete GeomPtrTable;
	GeomPtrTable = nullptr;

	delete MissingGeomTable;
	MissingGeomTable = nullptr;
}

/** Release all loaded animations */
void W3DShadowGeometryManager::Free_All_Geoms()
{
	// Make an iterator, and release all ptrs
	W3DShadowGeometryManagerIterator it( *this );
	for( it.First(); !it.Is_Done(); it.Next() ) {
		W3DShadowGeometry *geom = it.Get_Current_Geom();
		geom->Release_Ref();
	}

	// Then clear the table
	GeomPtrTable->Reset();
}

/** Find animation in cache */
W3DShadowGeometry * W3DShadowGeometryManager::Peek_Geom(const char * name)
{
	return (W3DShadowGeometry*)GeomPtrTable->Find( name );
}

/** Get animation from cache and increment its reference count */
W3DShadowGeometry * W3DShadowGeometryManager::Get_Geom(const char * name)
{
	W3DShadowGeometry * geom = Peek_Geom( name );
	if ( geom != nullptr ) {
		geom->Add_Ref();
	}
	return geom;
}

/** Add animation to cache */
Bool W3DShadowGeometryManager::Add_Geom(W3DShadowGeometry *new_geom)
{
	WWASSERT (new_geom != nullptr);

	// Increment the refcount on the new animation and add it to our table.
	new_geom->Add_Ref ();
	GeomPtrTable->Add( new_geom );

	return true;
}

/*
** An entry for a table of anims not found, so we can quickly determine their loss
*/
class MissingGeomClass : public HashableClass {

public:
	MissingGeomClass( const char * name ) : Name( name ) {}
	virtual	~MissingGeomClass() override {}

	virtual	const char * Get_Key() override { return Name;	}

private:
	StringClass	Name;

};

/*
** Missing Geoms
**
** The idea here, allow the system to register which anims are determined to be missing
** so that if they are asked for again, we can quickly return nullptr, without searching the
** disk again.
*/
void	W3DShadowGeometryManager::Register_Missing( const char * name )
{
	MissingGeomTable->Add( NEW MissingGeomClass( name ) );
}

Bool	W3DShadowGeometryManager::Is_Missing( const char * name )
{
	return ( MissingGeomTable->Find( name ) != nullptr );
}

/** Create shadow geometry from a reference W3D RenderObject*/
int W3DShadowGeometryManager::Load_Geom(RenderObjClass *robj, const char *name)
{
	Bool res=FALSE;

	W3DShadowGeometry * newgeom = NEW W3DShadowGeometry;

	if (newgeom == nullptr) {
		goto Error;
	}

	SET_REF_OWNER( newgeom );

	newgeom->Set_Name(name);

	switch (robj->Class_ID())
	{
		case RenderObjClass::CLASSID_HLOD:
			res=newgeom->initFromHLOD(robj);
			break;
		case RenderObjClass::CLASSID_MESH:
			res=newgeom->initFromMesh(robj);
			break;
		default:
			break;	//unknown render object type
	};

	if (res != TRUE)
	{	// load failed!
		newgeom->Release_Ref();
		//DEBUG_LOG(("****Shadow Volume Creation Failed on %s",name));
		goto Error;
	} else if (Peek_Geom(newgeom->Get_Name()) != nullptr)
	{	// duplicate exists!
		newgeom->Release_Ref();	// Release the one we just loaded
		goto Error;
	} else
	{	Add_Geom( newgeom );
		newgeom->Release_Ref();
	}

	return 0;

Error:
	return 1;
}

/*
** Iterator converter from HashableClass to W3DShadowGeometry
*/
W3DShadowGeometry * W3DShadowGeometryManagerIterator::Get_Current_Geom()
{
	return (W3DShadowGeometry *)Get_Current();
}

// GeneralsX: TEST-ONLY (W3DVolumetricShadow.h). Two independent specs, each read
// once with a thread-safe one-time init: map = FALSE reads GX_SHADOW_DROP_MESH (2003's volume geometry: use it with the
// key OFF only - with the key on it also changes the walk's pose trigger and bounds), map = TRUE reads
// GX_SHADOWMAP_DROP_CAST (the shadow map's CAST tag at draw time only: the walk, the bounds and the off-screen selection are untouched). The first 8
// matches of each are printed (`[GXSHADOW] TEST-ONLY drop <name> (volume|cast)`), then a running count every 4096th.
namespace
{
	// A73: the parsing and matching live in GXShadowDropSpec.h (host-tested; a single entry parses as A60's did), plus a comma list and
	// a trailing '.' = every mesh of the prefix. Each entry is printed as A60 printed its one (mesh '*' = every mesh).
	struct GxDropSpec
	{
		GXShadowDrop::Spec spec;
		std::atomic<unsigned> hits{ 0 };
		explicit GxDropSpec(const char *env)
		{
			const char *e = getenv(env);
			spec.parse(e);
			for (unsigned i = 0; spec.on && i < spec.count; ++i)
				fprintf(stderr, "[GXSHADOW] TEST-ONLY %s=%s: container prefix '%s', mesh '%s'\n", env, e, spec.entries[i].prefix,
					spec.entries[i].mesh[0] != '\0' ? spec.entries[i].mesh : "*");
		}
		bool on() const { return spec.on; }
		bool matches(const char *name) const { return spec.matches(name); }
	};
}

Bool W3DVolumetricShadowManager::gxShadowDropMesh(const char *meshName, Bool map)
{
	static GxDropSpec s_volume("GX_SHADOW_DROP_MESH"), s_cast("GX_SHADOWMAP_DROP_CAST");	// C++11: initialised once, thread-safe
	GxDropSpec &d = map ? s_cast : s_volume;
	if (!d.on() || !d.matches(meshName))
		return FALSE;
	const unsigned n = ++d.hits;
	if (n <= 8 || (n & 4095u) == 0)
		fprintf(stderr, "[GXSHADOW] TEST-ONLY drop %s (%s) n=%u\n", meshName, map ? "cast" : "volume", n);
	return TRUE;
}

// GeneralsX: shadow map (W3DVolumetricShadow.h). The same enable test renderShadows applies; the root is
// the top of the render object's container chain (user data and the mesh tag live there).
// With the map on, renderShadows returns before any Update(), so 2003's one-shot runtime clamp
// (updateOptimalExtrusionPadding) is run HERE under exactly Update()'s conditions - geometry present, the transform set (not the
// origin), inside the visible-terrain box grown by the object's extent, grounded (|z - ground| < AIRBORNE_UNIT_GROUND_DELTA: a building
// under construction sits below the terrain and waits), padding still 0 - in the same frame, so it computes the same value 2003 would
// (and step 3's volumes then use it as 2003's would). The own light (GXShadowMapFit.h) reads the instance's live clamp.
void W3DVolumetricShadowManager::gxShadowMapWalk(GxShadowMapWalkOut &out, const Vector3 &L, const Vector3 &axis, const AABoxClass &visibleBox,
	Real pullEps, Real pullMuMax)
{
	struct Item { RenderObjClass *root; uint64_t own; bool cand, posed; };
	static std::vector<Item> items;
	items.clear();
	out.roots.clear();
	out.own.clear();
	out.cand.clear();
	out.posed.clear();
	out.nearDist = out.topZ = out.minZ0 = 0.0f;
	out.any = out.anyOwn = FALSE;
	out.clampedTemplate = out.clampedRuntime = out.paddingNow = out.conflicts = 0;
	out.pulledAir = out.pulledGround = out.pulledRuntime = 0;
	const GXShadowFit::Vec3 Lv = GXShadowFit::v3(L.X, L.Y, L.Z);
	const Vector3 bc = visibleBox.Center, be = visibleBox.Extent;
	// A49: newest first, as renderShadows visits them (:4606) - the classic pose trigger and the runtime padding run in classic's order
	for (size_t n = m_shadowList.size(); n-- > 0; )
	{
		W3DVolumetricShadow *shadow = m_shadowList[n];
		if (!shadow->m_isEnabled || shadow->m_isInvisibleEnabled || shadow->m_robj == nullptr)
			continue;
		RenderObjClass *root = shadow->m_robj;
		while (root->Get_Container() != nullptr)
			root = root->Get_Container();
		const Vector3 pos = shadow->m_robj->Get_Position();
		Real z0 = pos.Z;
		bool cand = false, aloft = false;	// A47: 2003's own Update pre-cull (Update(): geometry, not the origin, the box grown by the branch's extent)
		if (shadow->m_geometry != nullptr && !(pos.X == 0.0f && pos.Y == 0.0f && pos.Z == 0.0f))
		{
			const Real ground = TheTerrainLogic ? TheTerrainLogic->getGroundHeight(pos.X, pos.Y)
				: TheTerrainRenderObject->getHeightMapHeight(pos.X, pos.Y, nullptr);
			const bool airborne = fabs(pos.Z - ground) >= AIRBORNE_UNIT_GROUND_DELTA;
			const Real ext = airborne ? MAX_SHADOW_LENGTH_EXTRA_AIRBORNE_SCALE_FACTOR * shadow->m_robjExtent : shadow->m_robjExtent;
			cand = !(WWMath::Fabs(pos.X - bc.X) > (be.X + ext) || WWMath::Fabs(pos.Y - bc.Y) > (be.Y + ext) ||
				WWMath::Fabs(pos.Z - bc.Z) > (be.Z + ext));
			aloft = airborne;
			if (airborne)
				z0 = ground;	// airborne: its 2003 volume reaches the ground below it
			else if (!shadow->m_extraExtrusionPadding &&
				!(WWMath::Fabs(pos.X - bc.X) > (be.X + shadow->m_robjExtent) || WWMath::Fabs(pos.Y - bc.Y) > (be.Y + shadow->m_robjExtent) ||
				  WWMath::Fabs(pos.Z - bc.Z) > (be.Z + shadow->m_robjExtent)))
			{
				shadow->updateOptimalExtrusionPadding();
				++out.paddingNow;
			}
		}
		// A48: classic's POSE TRIGGER, exactly - the prefix of updateVolumes (:2810-2850) that touches the pose, for
		// the shadows classic's Update reaches (cand): each geometry mesh in order, LOD 0 (or m_robj itself), skipped when hidden, else its
		// transform read. The same hierarchies get posed as key off (embedded sounds, emitter visibility included); only the moment in the
		// frame differs. posed = classic found a visible geometry mesh (else it casts nothing: the glue collects nothing for it).
		SphereClass gbound;
		const bool posed = cand && shadow->gxClassicPoseTrigger(&gbound);
		bool bound = false;
		const GXShadowFit::OwnLight o = GXShadowFit::ownLight(Lv, shadow->m_shadowLengthScale, z0, &bound);
		const bool pulled = bound && GXShadowFit::ownPullApplies(o.mu, pullMuMax, pullEps);   // A79: a thin copy, replayed pulled
		if (bound)
		{
			if (shadow->m_shadowLengthScale > shadow->m_gxTemplateLengthScale)
			{
				++out.clampedRuntime;
				out.pulledRuntime += pulled ? 1u : 0u;
			}
			else
			{
				++out.clampedTemplate;
				if (pulled)
					++(aloft ? out.pulledAir : out.pulledGround);
			}
			if (!out.anyOwn || z0 < out.minZ0)
				out.minZ0 = z0;
			out.anyOwn = TRUE;
		}
		Item it = { root, o.packed, cand, posed };
		items.push_back(it);
		// A51: the caster bounds are the union of the VISIBLE GEOMETRY MESHES' spheres classic's trigger just validated (a mesh sphere is
		// its model's object-space sphere through a current transform: no pose, unlike an HLOD sphere with a BOUNDINGBOX bone); only posed
		// candidates count - by classic's own cull nothing else can shadow the view
		if (posed)
		{
			const SphereClass &sp = gbound;
			Real d = (sp.Center.X - L.X) * axis.X + (sp.Center.Y - L.Y) * axis.Y + (sp.Center.Z - L.Z) * axis.Z - sp.Radius;
			if (pulled)   // A79b #8: the unscaled sphere (conservative for S, as before), then P - so no pulled copy crosses the near plane
				d = GXShadowFit::pulledNear(d, sp.Radius, pullEps);
			const Real top = sp.Center.Z + sp.Radius;
			if (!out.any || top > out.topZ)
				out.topZ = top;
			if (!out.any || d < out.nearDist)
				out.nearDist = d;
			out.any = TRUE;
		}
	}
	std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.root < b.root; });
	for (size_t i = 0; i < items.size(); ++i)
	{
		if (!out.roots.empty() && out.roots.back() == items[i].root)
		{
			out.cand.back() = out.cand.back() | uint8_t(items[i].cand ? 1 : 0);	// A47: a candidate when any of its shadows is
			out.posed.back() = out.posed.back() | uint8_t(items[i].posed ? 1 : 0);	// A48
			if (out.own.back() != items[i].own)
				++out.conflicts;	// a root whose shadows disagree: the first keeps it (counted, M5)
			continue;
		}
		out.roots.push_back(items[i].root);
		out.own.push_back(items[i].own);
		out.cand.push_back(uint8_t(items[i].cand ? 1 : 0));
		out.posed.push_back(uint8_t(items[i].posed ? 1 : 0));
	}
}

// GeneralsX: the part of updateVolumes (:2810-2850) that touches
// the POSE, alone - the same mesh choice (LOD 0 by m_meshRobjIndex, or m_robj itself), the same order, the same hidden check before each
// transform read - and nothing else (no volume, no history, no size skip). The shadow-map walk calls it for exactly the shadows classic's
// Update reaches, so the hierarchies posed with the key on are key off's (embedded animation sounds and emitter visibility included).
// TRUE when a geometry mesh is visible (classic would build a volume for it).
Bool W3DVolumetricShadow::gxClassicPoseTrigger(SphereClass *bound)
{
	HLodClass *hlod = (HLodClass *)m_robj;
	Bool any = FALSE;
	for (Int j = 0; j < m_geometry->getMeshCount(); ++j)
	{
		const Int meshIndex = m_geometry->getMesh(j)->m_meshRobjIndex;
		MeshClass *mesh = meshIndex >= 0 ? (MeshClass *)hlod->Peek_Lod_Model(0, meshIndex) : (MeshClass *)m_robj;
		if (mesh == nullptr || !mesh->Is_Not_Hidden_At_All())
			continue;
		(void)mesh->Get_Transform();
		// A51: the mesh's own sphere, now through its current transform (no pose: a MeshClass sphere is its model's object-space sphere)
		const SphereClass &ms = mesh->Get_Bounding_Sphere();
		if (!any)
			*bound = ms;
		else
			bound->Add_Sphere(ms);
		any = TRUE;
	}
	return any;
}
