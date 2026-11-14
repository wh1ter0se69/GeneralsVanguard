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

// FILE: W3DRoadBuffer.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: W3DRoadBuffer.cpp
//
// Created:   John Ahlquist, May 2001
//
// Desc:      Draw buffer to handle all the roads in a scene.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------

#include "W3DDevice/GameClient/W3DRoadBuffer.h"

#include <WW3D2/assetmgr.h>
#include <WW3D2/texture.h>
#include "Common/GlobalData.h"
#include "Common/RandomValue.h"
//#include "Common/GameFileSystem.h"
#include "Common/FileSystem.h" // for LOAD_TEST_ASSETS
#include "GameClient/TerrainRoads.h"
#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

static const Real TEE_WIDTH_ADJUSTMENT = 1.03f;
// GeneralsX @feature Segments a map gets per road point pair, for sizing m_roads (loadRoads builds again at
// MaxRoadSegments when a map needs more). A dense real-world city measured 1.4.
static const Int SEGMENTS_PER_PAIR = 2;
// Never below the stock MaxRoadSegments: every stock map fits it, so none is built twice.
static const Int MIN_ROAD_SEGMENTS = 4000;


//-----------------------------------------------------------------------------
//         Private Data
//-----------------------------------------------------------------------------

// A W3D shader that does alpha, texturing, tests zbuffer, doesn't update zbuffer.
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_SCALE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailAlphaShader(SC_ALPHA_DETAIL);


#define SC_ALPHA_MIRROR ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_ONE, \
	ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailShader(SC_ALPHA_MIRROR);


// The radius of the center of the road is 1.5 - outer radius 2.0, inner radius 1.0.
static const Real CORNER_RADIUS = 1.5f;


// The radius of the center of the road is 0.5 - outer radius 1.0, inner radius 0.0.
static const Real TIGHT_CORNER_RADIUS = 0.5f;
/*

// ShaderClass::PASS_ALWAYS,

#define SC_ALPHA_2D ( SHADE_CNST(PASS_ALWAYS, DEPTH_WRITE_DISABLE, COLOR_WRITE_ENABLE, \
	SRCBLEND_SRC_ALPHA, DSTBLEND_ONE_MINUS_SRC_ALPHA, FOG_DISABLE, GRADIENT_DISABLE, \
	SECONDARY_GRADIENT_DISABLE, TEXTURING_ENABLE, DETAILCOLOR_DISABLE, DETAILALPHA_DISABLE, \
	ALPHATEST_DISABLE, CULL_MODE_ENABLE, DETAILCOLOR_DISABLE, DE AILALPHA_DISABLE) )
ShaderClass ShaderClass::_PresetAlpha2DShader(SC_ALPHA_2D);
*/

/** Calculate the sign of the cross product.  If the tails of the vectors are both placed
at 0,0, then the cross product can be interpreted as -1 means v2 is to the right of v1,
1 means v2 is to the left of v1, and 0 means v2 is parallel to v1. */

static Int xpSign(const Vector2 &v1, const Vector2 &v2) {
	Real xpdct = v1.X*v2.Y - v1.Y*v2.X;
	if (xpdct<0) return -1;
	if (xpdct>0) return 1;
	return 0;
}

//=============================================================================
// The road build index
//=============================================================================
// GeneralsX @feature The road build used to find a segment by walking m_roads from the start - once per segment per
// phase, so a real-world city map's 50,000 segments took 3.4 s to build. These structures answer the same questions
// the walks did and hand back the candidates in the walks' own order, so the first hit is the hit the walk found and
// the built roads are identical (proved by the [GXROADS] built hashes). Endpoints are compared with Vector2's ==,
// which is exact float equality: the key is the exact bit pattern, with -0 folded into +0 (they compare equal) and
// NaN never indexed (it compares equal to nothing, so the walks never matched it either).

struct RoadPtKey
{
	UnsignedInt x, y;
	bool operator==(const RoadPtKey &o) const { return x == o.x && y == o.y; }
};

struct RoadPtKeyHash
{
	size_t operator()(const RoadPtKey &k) const { return ((size_t)k.x * 0x9E3779B1u) ^ (((size_t)k.y * 0x85EBCA77u) << 1); }
};

static Bool roadPtKey(const Vector2 &v, RoadPtKey &k)
{
	if (!(v.X == v.X) || !(v.Y == v.Y)) {
		return false;
	}
	const Real x = v.X == 0.0f ? 0.0f : v.X;
	const Real y = v.Y == 0.0f ? 0.0f : v.Y;
	memcpy(&k.x, &x, sizeof(k.x));
	memcpy(&k.y, &y, sizeof(k.y));
	return true;
}

/// Which segments have an end at a point.
class RoadEndpointMap
{
public:
	void clear() { m_map.clear(); }
	void add(Int seg, const Vector2 &loc)
	{
		RoadPtKey k;
		if (!roadPtKey(loc, k)) {
			return;
		}
		std::vector<Int> &v = m_map[k];
		if (v.empty() || v.back() != seg) {
			v.push_back(seg);
		}
	}
	void addEnds(Int seg, const RoadSegment &r) { add(seg, r.m_pt1.loc); add(seg, r.m_pt2.loc); }
	/// Appends the segments with an end at loc (a segment can repeat across calls; callers sort and unique).
	void collect(const Vector2 &loc, std::vector<Int> &out) const
	{
		RoadPtKey k;
		if (!roadPtKey(loc, k)) {
			return;
		}
		Map::const_iterator it = m_map.find(k);
		if (it != m_map.end()) {
			out.insert(out.end(), it->second.begin(), it->second.end());
		}
	}
private:
	typedef std::unordered_map<RoadPtKey, std::vector<Int>, RoadPtKeyHash> Map;
	Map m_map;
};

/// The order the old array kept the segments in, as an implicit treap over segment slots: a position lookup, an
/// insert and a move each cost O(log n) instead of shifting everything behind them.
class RoadOrder
{
public:
	void reset(Int capacity)
	{
		m_n.assign(capacity + 1, Node());
		m_root = 0;
		m_size = 0;
	}
	Int size() const { return m_size; }
	/// The slot at pos (0 <= pos < size).
	Int at(Int pos) const
	{
		Int t = m_root;
		while (t) {
			const Int ls = m_n[m_n[t].l].size;
			if (pos < ls) {
				t = m_n[t].l;
			} else if (pos == ls) {
				return t - 1;
			} else {
				pos -= ls + 1;
				t = m_n[t].r;
			}
		}
		return -1;
	}
	/// The position of a placed slot.
	Int rankOf(Int slot) const
	{
		Int t = slot + 1;
		Int r = m_n[m_n[t].l].size;
		while (m_n[t].p) {
			const Int p = m_n[t].p;
			if (m_n[p].r == t) {
				r += m_n[m_n[p].l].size + 1;
			}
			t = p;
		}
		return r;
	}
	/// The old insertion: everything from pos on moves up one, the slot takes pos.
	void insertAt(Int pos, Int slot)
	{
		const Int t = slot + 1;
		m_n[t] = Node();
		m_n[t].size = 1;
		UnsignedInt h = (UnsignedInt)slot * 2654435761u;
		h ^= h >> 15;
		h *= 2246822519u;
		m_n[t].pri = h ^ (h >> 13);
		Int a, b;
		split(m_root, pos, a, b);
		m_root = merge(merge(a, t), b);
		m_n[m_root].p = 0;
		m_size++;
	}
	/// The old moveRoadSegTo: the segment at fromPos goes to toPos, the ones between shift by one.
	void moveTo(Int fromPos, Int toPos)
	{
		if (fromPos == toPos) {
			return;
		}
		Int a, b, m, c;
		split(m_root, fromPos, a, b);
		split(b, 1, m, c);
		m_root = merge(a, c);
		m_n[m].p = 0;
		split(m_root, toPos, a, b);
		m_root = merge(merge(a, m), b);
		m_n[m_root].p = 0;
	}
	/// The slots in order.
	void toVector(std::vector<Int> &out) const
	{
		out.clear();
		out.reserve(m_size);
		std::vector<Int> stack;
		Int t = m_root;
		while (t || !stack.empty()) {
			while (t) {
				stack.push_back(t);
				t = m_n[t].l;
			}
			t = stack.back();
			stack.pop_back();
			out.push_back(t - 1);
			t = m_n[t].r;
		}
	}
private:
	struct Node
	{
		Int l, r, p, size;
		UnsignedInt pri;
		Node() : l(0), r(0), p(0), size(0), pri(0) {}
	};
	std::vector<Node> m_n;	///< node i is slot i-1; node 0 is "none" and keeps size 0.
	Int m_root;
	Int m_size;
	void upd(Int t)
	{
		Node &n = m_n[t];
		n.size = 1 + m_n[n.l].size + m_n[n.r].size;
		if (n.l) m_n[n.l].p = t;
		if (n.r) m_n[n.r].p = t;
	}
	Int merge(Int a, Int b)
	{
		if (!a) return b;
		if (!b) return a;
		if (m_n[a].pri > m_n[b].pri) {
			m_n[a].r = merge(m_n[a].r, b);
			upd(a);
			return a;
		}
		m_n[b].l = merge(a, m_n[b].l);
		upd(b);
		return b;
	}
	/// The first k of t into a, the rest into b.
	void split(Int t, Int k, Int &a, Int &b)
	{
		if (!t) {
			a = b = 0;
			return;
		}
		if (m_n[m_n[t].l].size >= k) {
			Int tl;
			split(m_n[t].l, k, a, tl);
			m_n[t].l = tl;
			upd(t);
			b = t;
		} else {
			Int tr;
			split(m_n[t].r, k - m_n[m_n[t].l].size - 1, tr, b);
			m_n[t].r = tr;
			upd(t);
			a = t;
		}
	}
};

/// Which straight segments of other road types lie under an open road end (insertCrossTypeJoins): a uniform grid
/// over the segments' widened bounds. A cell's list is in ascending index, the old walk's order.
struct RoadJoinGrid
{
	Real loX, loY, cell;
	Int w, h;
	std::vector<std::vector<Int> > cells;
	std::vector<Int> always;	///< segments whose bounds are not finite: the old test ran on them too.

	static Bool bounds(const RoadSegment &r, Region2D &b)
	{
		const Vector2 loc1 = r.m_pt1.loc;
		const Vector2 loc2 = r.m_pt2.loc;
		b.lo.x = loc1.X;
		b.lo.y = loc1.Y;
		b.hi = b.lo;
		if (b.lo.x > loc2.X) b.lo.x = loc2.X;
		if (b.lo.y > loc2.Y) b.lo.y = loc2.Y;
		if (b.hi.x < loc2.X) b.hi.x = loc2.X;
		if (b.hi.y < loc2.Y) b.hi.y = loc2.Y;
		b.lo.x -= r.m_scale/2;
		b.lo.y -= r.m_scale/2;
		b.hi.x += r.m_scale/2;
		b.hi.y += r.m_scale/2;
		return b.lo.x - b.lo.x == 0.0f && b.lo.y - b.lo.y == 0.0f && b.hi.x - b.hi.x == 0.0f && b.hi.y - b.hi.y == 0.0f;
	}
	Int cellX(Real x) const
	{
		Int c = (Int)((x - loX) / cell);
		return c < 0 ? 0 : (c >= w ? w - 1 : c);
	}
	Int cellY(Real y) const
	{
		Int c = (Int)((y - loY) / cell);
		return c < 0 ? 0 : (c >= h ? h - 1 : c);
	}
	void build(const RoadSegment *roads, Int count)
	{
		cells.clear();
		always.clear();
		w = h = 0;
		Real hiX = 0, hiY = 0;
		Bool any = false;
		Int i;
		Region2D b;
		for (i=0; i<count; i++) {
			if (roads[i].m_type != SEGMENT) continue;
			if (!bounds(roads[i], b)) {
				always.push_back(i);
				continue;
			}
			if (!any) {
				loX = b.lo.x; loY = b.lo.y; hiX = b.hi.x; hiY = b.hi.y;
				any = true;
			} else {
				if (b.lo.x < loX) loX = b.lo.x;
				if (b.lo.y < loY) loY = b.lo.y;
				if (b.hi.x > hiX) hiX = b.hi.x;
				if (b.hi.y > hiY) hiY = b.hi.y;
			}
		}
		if (!any) return;
		// About one cell per segment (a stock map's 300 segments get 18 x 18, a city's 50,000 get 224 x 224): the cell
		// count is a cost knob only - a cell holds every segment whose bounds touch it, whatever its size.
		Int perAxis = 1;
		while (perAxis < 256 && perAxis * perAxis < count) {
			perAxis++;
		}
		cell = (hiX - loX > hiY - loY ? hiX - loX : hiY - loY) / perAxis;
		if (!(cell >= 1.0f)) cell = 1.0f;	// also catches a zero or NaN extent
		w = cellX_unclamped(hiX) + 1;
		h = cellY_unclamped(hiY) + 1;
		if (w > perAxis + 1) w = perAxis + 1;
		if (h > perAxis + 1) h = perAxis + 1;
		cells.resize((size_t)w * h);
		for (i=0; i<count; i++) {
			if (roads[i].m_type != SEGMENT || !bounds(roads[i], b)) continue;
			const Int x0 = cellX(b.lo.x), x1 = cellX(b.hi.x), y0 = cellY(b.lo.y), y1 = cellY(b.hi.y);
			for (Int y=y0; y<=y1; y++) {
				for (Int x=x0; x<=x1; x++) {
					cells[(size_t)y * w + x].push_back(i);
				}
			}
		}
	}
	Int cellX_unclamped(Real x) const { Int c = (Int)((x - loX) / cell); return c < 0 ? 0 : c; }
	Int cellY_unclamped(Real y) const { Int c = (Int)((y - loY) / cell); return c < 0 ? 0 : c; }
	/// The cell list for a point, or null when the point is outside every finite bound.
	const std::vector<Int> *at(const Vector2 &loc) const
	{
		if (w == 0 || !(loc.X - loc.X == 0.0f) || !(loc.Y - loc.Y == 0.0f)) return nullptr;
		if (loc.X < loX || loc.Y < loY) return nullptr;
		return &cells[(size_t)cellY(loc.Y) * w + cellX(loc.X)];
	}
};

/// Everything the build keeps between its phases; alive only inside buildRoadSegments.
struct RoadBuildScratch
{
	RoadOrder order;				///< Phases 1-2: the array order of the slots in m_roads.
	RoadEndpointMap ends;		///< Phases 1-2 (by slot) and 3-4 (by index): which segments have an end at a point.
	std::vector<Int> cand;	///< Scratch for candidate lists.
	RoadJoinGrid grid;			///< Phase 6.
};

static Bool s_dynamic = false;

//-----------------------------------------------------------------------------
//         Private Class
//-----------------------------------------------------------------------------

//=============================================================================
// RoadType constructor
//=============================================================================
/** Nulls index & vertex data. */
//=============================================================================
RoadType::RoadType():
m_roadTexture(nullptr),
m_vertexRoad(nullptr),
m_indexRoad(nullptr),
m_numRoadVertices(0),
m_numRoadIndices(0),
m_vertexCapacity(0),
m_indexCapacity(0),
m_stackingOrder(0),
m_uniqueID(-1)
{
}

//=============================================================================
// RoadType destructor
//=============================================================================
/** Frees index & vertex data. */
//=============================================================================
RoadType::~RoadType()
{
	REF_PTR_RELEASE(m_roadTexture);
	REF_PTR_RELEASE(m_vertexRoad);
	REF_PTR_RELEASE(m_indexRoad);
}

//=============================================================================
// RoadType applyTexture
//=============================================================================
/** Sets the W3D texture. */
//=============================================================================
void RoadType::applyTexture()
{
 	W3DShaderManager::setTexture(0,m_roadTexture);
	DX8Wrapper::Set_Index_Buffer(m_indexRoad,0);
	DX8Wrapper::Set_Vertex_Buffer(m_vertexRoad);
}


//=============================================================================
// RoadType loadTexture
//=============================================================================
/** Sets the W3D texture. */
//=============================================================================
void RoadType::loadTexture(AsciiString path, Int ID)
{
	/// @todo - delay loading textures and only load textures referenced by map.
	WW3DAssetManager *pMgr = W3DAssetManager::Get_Instance();

	m_roadTexture = pMgr->Get_Texture(path.str(), MIP_LEVELS_3);
	//Hack to disable texture reduction
	//m_roadTexture = pMgr->Get_Texture(path.str(), MIP_LEVELS_3, WW3D_FORMAT_UNKNOWN,true,TextureBaseClass::TEX_REGULAR, false);

	m_roadTexture->Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_BEST );

	m_roadTexture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	m_roadTexture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);

	// GeneralsX @feature No buffers here: W3DRoadBuffer::sizeRoadTypeBuffers sizes them per map.
	sizeBuffers(0, 0);

#ifdef LOAD_TEST_ASSETS
	m_texturePath = path;
#endif
	m_uniqueID = ID;
}

//=============================================================================
// RoadType sizeBuffers
//=============================================================================
/** Makes the vertex and index buffers hold numVertex and numIndex (0 = none). */
//=============================================================================
void RoadType::sizeBuffers(Int numVertex, Int numIndex)
{
	if (numVertex <= 0 || numIndex <= 0) {
		numVertex = 0;
		numIndex = 0;
	}
	m_numRoadVertices = 0;
	m_numRoadIndices = 0;
	if (numVertex == m_vertexCapacity && numIndex == m_indexCapacity) {
		return;
	}
	REF_PTR_RELEASE(m_vertexRoad);
	REF_PTR_RELEASE(m_indexRoad);
	m_vertexCapacity = 0;
	m_indexCapacity = 0;
	if (numVertex == 0) {
		return;
	}
	m_vertexRoad=NEW_REF(DX8VertexBufferClass,(DX8_FVF_XYZDUV1,numVertex+4, (s_dynamic?DX8VertexBufferClass::USAGE_DYNAMIC:DX8VertexBufferClass::USAGE_DEFAULT)));
	m_indexRoad=NEW_REF(DX8IndexBufferClass,(numIndex+4, (s_dynamic?DX8IndexBufferClass::USAGE_DYNAMIC:DX8IndexBufferClass::USAGE_DEFAULT)));
	m_vertexCapacity = numVertex;
	m_indexCapacity = numIndex;
}

#ifdef LOAD_TEST_ASSETS
//=============================================================================
// RoadType loadTexture
//=============================================================================
/** Sets the W3D texture. */
//=============================================================================
void RoadType::loadTestTexture()
{
	if (m_isAutoLoaded && m_uniqueID>0 && !m_texturePath.isEmpty()) {
		/// @todo - delay loading textures and only load textures referenced by map.
		m_roadTexture = NEW_REF(TextureClass, (m_texturePath.str(), m_texturePath.str(), MIP_LEVELS_3));
		m_roadTexture->Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_BEST );

		m_roadTexture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
		m_roadTexture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	}
}
#endif

//=============================================================================
// RoadSegment constructor
//=============================================================================
/** Nulls index & vertex data. */
//=============================================================================
RoadSegment::RoadSegment() :
m_curveRadius(0.0f),
m_type(SEGMENT),
m_scale(1.0f),
m_widthInTexture(1.0f),
m_uniqueID(0),
m_visible(false),
m_numVertex(0),
m_vb(nullptr),
m_numIndex(0),
m_ib(nullptr),
m_bounds(Vector3(0.0f, 0.0f, 0.0f), 1.0f)
{
}

//=============================================================================
// RoadSegment destructor
//=============================================================================
/** Frees index & vertex data. */
//=============================================================================
RoadSegment::~RoadSegment()
{
	m_numVertex = 0;
	delete[] m_vb;
	m_vb = nullptr;

	m_numIndex = 0;
	delete[] m_ib;
	m_ib = nullptr;
}



//=============================================================================
// RoadSegment::SetVertexBuffer
//=============================================================================
/** Allocates & sets the vertex entries. */
//=============================================================================
void RoadSegment::SetVertexBuffer(VertexFormatXYZDUV1 *vb, Int numVertex)
{
	delete[] m_vb;
	m_vb = nullptr;
	m_numVertex = 0;

	Vector3 verts[MAX_SEG_VERTEX];
	if (numVertex<1 || numVertex > MAX_SEG_VERTEX)
		return;

	m_vb = NEW VertexFormatXYZDUV1[numVertex];	// pool[]ify
	if (!m_vb)
		return;

	m_numVertex = numVertex;
	memcpy(m_vb, vb, numVertex*sizeof(VertexFormatXYZDUV1));
	Int i;
	for (i=0; i<numVertex; i++) {
		verts[i].X = m_vb[i].x;
		verts[i].Y = m_vb[i].y;
		verts[i].Z = m_vb[i].z;
	}
	SphereClass bounds(verts, numVertex);
	m_bounds = bounds;
}

//=============================================================================
// RoadSegment::SetIndexBuffer
//=============================================================================
/** Allocates & sets the index entries. */
//=============================================================================
void RoadSegment::SetIndexBuffer(UnsignedShort *ib, Int numIndex)
{
	delete[] m_ib;
	m_ib = nullptr;
	m_numIndex = 0;

	if (numIndex < 1 || numIndex > MAX_SEG_INDEX)
		return;

	m_ib = NEW UnsignedShort[numIndex];
	if (!m_ib)
		return;

	m_numIndex = numIndex;
	memcpy(m_ib, ib, numIndex*sizeof(UnsignedShort));
}

//=============================================================================
// RoadSegment::GetVertices
//=============================================================================
/** Copies vertex entries into destination_vb. */
//=============================================================================
Int RoadSegment::GetVertices(VertexFormatXYZDUV1 *destination_vb, Int numToCopy)
{
	if (m_vb == nullptr || numToCopy<1) return	(0);
	if (numToCopy > m_numVertex) return(0);
	memcpy(destination_vb, m_vb, numToCopy*sizeof(VertexFormatXYZDUV1));
	return(numToCopy);
}

//=============================================================================
// RoadSegment::GetIndices
//=============================================================================
/** Copies index entries into destination_ib. */
//=============================================================================
Int RoadSegment::GetIndices(UnsignedShort *destination_ib, Int numToCopy, Int offset)
{
	if (m_ib == nullptr || numToCopy<1) return	(0);
	if (numToCopy > m_numIndex) return(0);
	Int i;
	for (i=0; i<numToCopy; i++) {
		destination_ib[i] = m_ib[i]+offset;
	}
	return(numToCopy);
}

//=============================================================================
// RoadSegment::updateSegLighting
//=============================================================================
/** Updates the diffuse lighting in the vertex buffer. */
//=============================================================================
void RoadSegment::updateSegLighting()
{
	Int i;
	Int borderSizeInLine=TheTerrainRenderObject->getMap()->getBorderSizeInline();
	for (i=0; i<m_numVertex; i++) {
		Int x = m_vb[i].x/MAP_XY_FACTOR+0.5;
		Int y = m_vb[i].y/MAP_XY_FACTOR+0.5;
		x += borderSizeInLine;
		y += borderSizeInLine;
		m_vb[i].diffuse = (255<<24)|TheTerrainRenderObject->getStaticDiffuse(x, y);
	}
}

//-----------------------------------------------------------------------------
//         Private Methods
//-----------------------------------------------------------------------------

//=============================================================================
// W3DRoadBuffer::loadTee
//=============================================================================
/** Loads a tee into the vertex buffer for a road join. */
//=============================================================================
void W3DRoadBuffer::loadTee(RoadSegment *pRoad, Vector2 loc1,
														Vector2 loc2, Bool is4Way, Real scale)
{
	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
//	Real roadLen = scale;
	if (abs(roadVector.X) < MIN_ROAD_SEGMENT && abs(roadVector.Y) < MIN_ROAD_SEGMENT) {
		roadVector.Set(1.0f, 0.0f);
		roadNormal.Set(0.0f, 1.0f);
	} else {
		roadVector.Normalize();
		roadNormal.Normalize();
	}

	Real uOffset = (425.0f/512.0f);
	Real vOffset = (255.0f/512.0f);
	if (is4Way) {
		uOffset = (425.0f/512.0f);
		vOffset = (425.0f/512.0f);
	}
	Real teeFactor = scale*TEE_WIDTH_ADJUSTMENT/2.0f;
	Real left = pRoad->m_widthInTexture*scale/2.0f;
	loadFloatSection(pRoad, loc1, loc2-loc1, teeFactor, left, teeFactor, uOffset, vOffset, scale);
}

//=============================================================================
// W3DRoadBuffer::loadAlphaJoin
//=============================================================================
/** Loads an alpha join into the vertex buffer for a road join. */
//=============================================================================
void W3DRoadBuffer::loadAlphaJoin(RoadSegment *pRoad, Vector2 loc1,
														Vector2 loc2,Real scale)
{

	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
//	Real roadLen = scale;
	if (abs(roadVector.X) < MIN_ROAD_SEGMENT && abs(roadVector.Y) < MIN_ROAD_SEGMENT) {
		roadVector.Set(1.0f, 0.0f);
		roadNormal.Set(0.0f, 1.0f);
	} else {
		roadVector.Normalize();
		roadNormal.Normalize();
	}

	Real uOffset = (106.0f/512.0f);
	Real vOffset = (425.0f/512.0f);

	Real roadwidth = scale;
	Real uScale = pRoad->m_widthInTexture;

	roadVector *= roadwidth*48/128; // we just want 48 pixels.
	roadNormal *= uScale*(1+8.0f/128); // we want 8 extra pixels.


	Vector2	corners[NUM_CORNERS];

	corners[topLeft] =  loc1 + roadNormal*0.5f - roadVector*0.65f ;
	corners[bottomLeft] = corners[topLeft] - roadNormal;
	corners[bottomRight] = corners[bottomLeft] + roadVector;
	corners[topRight] = corners[topLeft] + roadVector;
	loadFloat4PtSection(pRoad, loc1, roadNormal, roadVector, corners, uOffset, vOffset, scale, uScale);
}

//=============================================================================
// W3DRoadBuffer::loadY
//=============================================================================
/** Loads a Y into the vertex buffer for a road join. */
//=============================================================================
void W3DRoadBuffer::loadY(RoadSegment *pRoad, Vector2 loc1,
														Vector2 loc2,Real scale)
{

	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
//	Real roadLen = scale;
	if (abs(roadVector.X) < MIN_ROAD_SEGMENT && abs(roadVector.Y) < MIN_ROAD_SEGMENT) {
		roadVector.Set(1.0f, 0.0f);
		roadNormal.Set(0.0f, 1.0f);
	} else {
		roadVector.Normalize();
		roadNormal.Normalize();
	}

	Real uOffset = (255.0f/512.0f);
	Real vOffset = (226.0f/512.0f);

	Real roadwidth = scale;

	roadVector *= roadwidth;
	roadNormal *= roadwidth;


	Vector2	corners[NUM_CORNERS];

	roadVector *= 1.59f;
	corners[topLeft] =  loc1 + roadNormal*0.29f - roadVector*0.5f ;
	corners[bottomLeft] = corners[topLeft] - roadNormal*1.08f;
	corners[bottomRight] = corners[bottomLeft] + roadVector;
	corners[topRight] = corners[topLeft] + roadVector;
	loadFloat4PtSection(pRoad, loc1, roadNormal, roadVector, corners, uOffset, vOffset, scale, scale);
}

//=============================================================================
// W3DRoadBuffer::loadH
//=============================================================================
/** Loads a h shaped tee into the vertex buffer for a road join. */
//=============================================================================
void W3DRoadBuffer::loadH(RoadSegment *pRoad, Vector2 loc1,
														Vector2 loc2, Bool flip, Real scale)
{

	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
//	Real roadLen = scale;
	if (abs(roadVector.X) < MIN_ROAD_SEGMENT && abs(roadVector.Y) < MIN_ROAD_SEGMENT) {
		roadVector.Set(1.0f, 0.0f);
		roadNormal.Set(0.0f, 1.0f);
	} else {
		roadVector.Normalize();
		roadNormal.Normalize();
	}

	Real uOffset = (202.0f/512.0f);
	Real vOffset = (364.0f/512.0f);

	Real roadwidth = scale;

	roadVector *= roadwidth;
	roadNormal *= roadwidth;


	Vector2	corners[NUM_CORNERS];

	roadNormal *= 1.35f;
	if (flip) {
		corners[bottomLeft] = loc1 - roadNormal*0.20f - roadVector*pRoad->m_widthInTexture/2;
	}	else {
		corners[bottomLeft] = loc1 - roadNormal*0.8f - roadVector*pRoad->m_widthInTexture/2;
	}
	Vector2 width = roadVector*pRoad->m_widthInTexture/2;
	width = width + roadVector * 1.2f;
	corners[bottomRight] = corners[bottomLeft] + width;
	corners[topRight] = corners[bottomRight] + roadNormal;
	corners[topLeft] = corners[bottomLeft] + roadNormal;
	if (flip) roadNormal = -roadNormal;
	loadFloat4PtSection(pRoad, loc1, roadNormal, roadVector, corners, uOffset, vOffset, scale, scale);
}

//=============================================================================
// W3DRoadBuffer::loadFloatSection
//=============================================================================
/** Loads a section of road using a mesh that floats a little above the
terrain. */
//=============================================================================
void W3DRoadBuffer::loadFloatSection(RoadSegment *pRoad, Vector2 loc,
														Vector2 roadVector, Real halfHeight, Real left, Real right,
														Real uOffset, Real vOffset, Real scale)
{
	if (m_map==nullptr) {
		return;
	}

	Vector2 roadNormal(-roadVector.Y, roadVector.X);
	roadVector.Normalize();
	roadVector *= right;

	roadNormal.Normalize();
	if (halfHeight<0) halfHeight = -halfHeight;
	roadNormal *= halfHeight;

	Vector2 roadLeft = roadVector;
	roadLeft.Normalize();
	roadLeft *= left;
	roadVector += roadLeft;
	Vector2 leftCenter = loc;
	leftCenter -= roadLeft;

	Vector2	corners[NUM_CORNERS];

	corners[bottomLeft] = leftCenter - roadNormal;
	corners[bottomRight] = corners[bottomLeft];
	corners[bottomRight] += roadVector;
	corners[topRight] = corners[bottomRight];
 	corners[topRight] += 2*roadNormal;
	corners[topLeft] = corners[bottomLeft];
 	corners[topLeft] += 2*roadNormal;

	loadFloat4PtSection(pRoad, loc, roadNormal, roadVector, corners, uOffset, vOffset, scale, scale);

}

// GeneralsX: A road section's length and width come from the map (its points, the object's
// scale, the road type's RoadWidth), and converting a huge or NaN count to Int was undefined before the caller's clamps
// ran. Clamp in Real first: at least 2, as the callers always required, and at most a million (the vertex buffer fills
// long before). Every count that converted cleanly before comes out the same.
static Int clampRoadCount(Real count)
{
	if (!(count >= 2.0f))
		return 2;
	if (count >= 1000000.0f)
		return 1000000;
	return (Int)count;
}

//=============================================================================
// W3DRoadBuffer::loadFloat4PtSection
//=============================================================================
/** Loads a section of road using a mesh that floats a little above the
terrain.  The road is loaded into the quadrilateral defined by the
4 corners points.  loc specifies the point where u==uOffset && v==vOffset, and
the road vector gives the direction of the road, and the road normal is perpendicular
to the road normal.  */
//=============================================================================
void W3DRoadBuffer::loadFloat4PtSection(RoadSegment *pRoad, Vector2 loc,
														Vector2 roadNormal, Vector2 roadVector,
														Vector2 *cornersP,
														Real uOffset, Real vOffset, Real uScale, Real vScale)
{

	// GeneralsX @feature world-unit tolerances: kept at the retail step on a taller map (getHeightScale), so roads hug it as closely
	const Real FLOAT_AMOUNT = MAP_HEIGHT_SCALE/8;
	const Real MAX_ERROR = MAP_HEIGHT_SCALE*1.1f;
	UnsignedShort ib[MAX_SEG_INDEX];
	VertexFormatXYZDUV1 vb[MAX_SEG_VERTEX];
	Int numRoadVertices = 0;
	Int numRoadIndices = 0;

	TRoadSegInfo info;
	info.loc = loc;
	info.roadNormal = roadNormal;
	info.roadVector = roadVector;
	info.corners[bottomLeft] = cornersP[bottomLeft];
	info.corners[bottomRight] = cornersP[bottomRight];
	info.corners[topLeft] = cornersP[topLeft];
	info.corners[topRight] = cornersP[topRight];
	info.uOffset = uOffset;
	info.vOffset = vOffset;
	info.scale = uScale;
	pRoad->SetRoadSegInfo(&info);



	Real roadLen = roadVector.Length();
	Real halfHeight = roadNormal.Length();
	roadNormal.Normalize();
	roadVector.Normalize();
	Vector2 curVector;
	Int uCount = clampRoadCount((roadLen/MAP_XY_FACTOR)+1);
	Int vCount = clampRoadCount((2*halfHeight/MAP_XY_FACTOR)+1);


	const int maxRows = 100;
	typedef struct {
		Bool collapsed;
		Bool deleted;
		Vector3 vtx[maxRows];
		Int diffuseRed;
		Bool lightGradient;
		Int vertexIndex[maxRows];
		Real uIndex;
	} TColumn;
	const Int DIFFUSE_LIMIT = 25; // if more than that, we tesselate :) jba.

	if (vCount>maxRows) vCount = maxRows;
	TColumn prevColumn, curColumn, nextColumn;

	prevColumn.deleted = true;
	curColumn.deleted = true;
	Int i, j, k;
	Vector2 v2 = cornersP[bottomLeft];
	Vector3 origin(v2.X, v2.Y, 0);
	v2 = cornersP[bottomRight] - cornersP[bottomLeft];
	Vector3 uVector1(v2.X, v2.Y, 0);
	v2 = cornersP[topRight] - cornersP[topLeft];
	Vector3 uVector2(v2.X, v2.Y, 0);
	v2 = cornersP[topLeft];
	Vector3 origin2(v2.X, v2.Y, 0);
	v2 = cornersP[topLeft] - cornersP[bottomLeft];
	Vector3 vVector1(v2.X, v2.Y, 0);
	v2 = cornersP[topRight] - cornersP[bottomRight];
	Vector3 vVector2(v2.X, v2.Y, 0);
	uVector2 += (vVector1 - vVector2);
	for (i=0; i<=uCount; i++) {
		Real iFactor = ((Real)i / (uCount-1));
		Real iBarFactor = 1.0f-iFactor;
		if (i<uCount) {
			nextColumn.collapsed = false;
			nextColumn.deleted = false;
			nextColumn.lightGradient = false;
			nextColumn.uIndex = i;

			Real minHeight=m_map->getMaxHeightValue()*m_map->getHeightScale();
			Real maxHeight = m_map->getMinHeightValue()*m_map->getHeightScale();
			for (j=0; j<vCount; j++) {
				Real jFactor = ((Real)j / (vCount-1));
				Real jBarFactor = 1.0f-jFactor;
				nextColumn.vtx[j] = origin +  (uVector1 * jBarFactor * iFactor) + (uVector2 * jFactor * iFactor) +
													(vVector1 * iBarFactor * jFactor) + (vVector2 * iFactor * jFactor) ;
				Real z = TheTerrainRenderObject->getMaxCellHeight(nextColumn.vtx[j].X, nextColumn.vtx[j].Y);
				if (z<minHeight) minHeight = z;
				if (z>maxHeight) maxHeight = z;
				nextColumn.vertexIndex[j] = -1;
				nextColumn.vtx[j].Z = z;
				Int diffuse = 0;
				if (j==0) {
					nextColumn.diffuseRed = (diffuse & 0x00ff);
				} else {
					Int red = diffuse&0x00ff;
					if (abs(red-nextColumn.diffuseRed) > DIFFUSE_LIMIT) {
						nextColumn.lightGradient = true;
					}
				}
			}

			if (true) { // !nextColumn.lightGradient) {
				nextColumn.collapsed = true;
				nextColumn.vtx[0].Z = maxHeight;
				nextColumn.vtx[1] = nextColumn.vtx[vCount-1];
				nextColumn.vtx[1].Z = maxHeight;
			}	else {
				for (j=0; j<vCount; j++) {
					nextColumn.vtx[j].Z = maxHeight;
				}
			}
			if (i<2) {
				curColumn = nextColumn;
			} else {
				if (prevColumn.collapsed && curColumn.collapsed && nextColumn.collapsed) {
					Bool okToDelete = false;

					Real theZ = prevColumn.vtx[0].Z * (curColumn.uIndex-prevColumn.uIndex) +
										nextColumn.vtx[0].Z * (nextColumn.uIndex-curColumn.uIndex);
					theZ /= nextColumn.uIndex-prevColumn.uIndex;
					if (theZ >= curColumn.vtx[0].Z && theZ < curColumn.vtx[0].Z + MAX_ERROR) {
						theZ = prevColumn.vtx[1].Z * (curColumn.uIndex-prevColumn.uIndex) +
											nextColumn.vtx[1].Z * (nextColumn.uIndex-curColumn.uIndex);
						theZ /= nextColumn.uIndex-prevColumn.uIndex;
						if (theZ >= curColumn.vtx[1].Z && theZ < curColumn.vtx[1].Z + MAX_ERROR) {
							okToDelete = true;
						}
					}
					if (okToDelete) {
						curColumn.deleted = true;
					}
				}
			}
		}
		if (!curColumn.deleted && i!=1) {
			// Write out the vertices.
			for (j=0; j<vCount; j++) {
				Real U, V;
				if (numRoadVertices >= MAX_SEG_VERTEX) {
					break;
				}
				curVector.Set(curColumn.vtx[j].X - loc.X, curColumn.vtx[j].Y - loc.Y);
				V = Vector2::Dot_Product(roadNormal, curVector);
				U = Vector2::Dot_Product(roadVector, curVector);
				Int diffuse = 0;
			#ifdef RTS_DEBUG
				//diffuse &= 0xFFFF00FF; // strip out green.
			#endif
				vb[numRoadVertices].u1 = uOffset+U/(uScale*4);
				vb[numRoadVertices].v1 = vOffset-V/(vScale*4);	// Road is 1/16 texture height.
				vb[numRoadVertices].x = curColumn.vtx[j].X;
				vb[numRoadVertices].y = curColumn.vtx[j].Y;
				vb[numRoadVertices].z = curColumn.vtx[j].Z+FLOAT_AMOUNT;
				vb[numRoadVertices].diffuse = diffuse;
				curColumn.vertexIndex[j] = numRoadVertices;
				numRoadVertices++;
				if (j==1 && curColumn.collapsed) {
					break;
				}
			}
			if (numRoadVertices >= MAX_SEG_VERTEX) {
				break;
			}
			if (i>1) {
				// Write out the triangles.
				j = 0;
				k = 0;
				while (j<vCount-1 && k<vCount-1) {
					// GeneralsX: Up to six indices are written below, so a bare
					// ">= MAX_SEG_INDEX" test still let the last iteration write past
					// the end of ib[].
					if (numRoadIndices + 6 > MAX_SEG_INDEX) {
						break;
					}
					UnsignedShort *curIb = ib+numRoadIndices;
					if (k==0 || !prevColumn.collapsed) {
						*curIb++ = prevColumn.vertexIndex[j+1];
						*curIb++ = prevColumn.vertexIndex[j];
						*curIb++ = curColumn.vertexIndex[k];
						numRoadIndices+=3;
					}
					if (j==0 || !curColumn.collapsed) {
						Int offset = 1;
						if (curColumn.collapsed && !prevColumn.collapsed) {
							offset = vCount-1;
						}
						*curIb++ = prevColumn.vertexIndex[j+offset];
						*curIb++ = curColumn.vertexIndex[k];
						*curIb++ = curColumn.vertexIndex[k+1];
						numRoadIndices+=3;
					}
					if (prevColumn.collapsed && curColumn.collapsed) {
						break;
					}
					if (!prevColumn.collapsed) {
						j++;
					}
					if (!curColumn.collapsed) {
						k++;
					}
				}
				prevColumn = curColumn;
			}	else if (i==0) {
				prevColumn = curColumn;
			}
			if (numRoadIndices >= MAX_SEG_INDEX) {
				break;
			}
		}
		curColumn = nextColumn;
	}
	pRoad->SetVertexBuffer(vb, numRoadVertices);
	pRoad->SetIndexBuffer(ib, numRoadIndices);
}

//=============================================================================
// W3DRoadBuffer::loadLit4PtSection
//=============================================================================
/** Loads a section of road using a mesh that floats a little above the
terrain.  The road is loaded into the quadrilateral defined by the
4 corners points.  loc specifies the point where u==uOffset && v==vOffset, and
the road vector gives the direction of the road, and the road normal is perpendicular
to the road normal.  */
//=============================================================================
void W3DRoadBuffer::loadLit4PtSection(RoadSegment *pRoad, UnsignedShort *ib, VertexFormatXYZDUV1 *vb, RefRenderObjListIterator *pDynamicLightsIterator)
{

	// GeneralsX @feature world-unit tolerances: kept at the retail step on a taller map (getHeightScale), so roads hug it as closely
	const Real FLOAT_AMOUNT = MAP_HEIGHT_SCALE/8;
	const Real MAX_ERROR = MAP_HEIGHT_SCALE*1.1f;


	if (pRoad->m_uniqueID != m_curUniqueID) {
		return;
	}
	if (!pRoad->m_visible) {
		return;
	}
	Int numLights = 0;
	const Int maxLights = 8;
	LightClass *lights[maxLights];

	for (pDynamicLightsIterator->First(); !pDynamicLightsIterator->Is_Done(); pDynamicLightsIterator->Next()) {
			LightClass *pLight = (LightClass*)pDynamicLightsIterator->Peek_Obj();
			SphereClass bounds = pLight->Get_Bounding_Sphere();
			if (Spheres_Intersect(pRoad->getBounds(), bounds)) {
				lights[numLights] = pLight;
				numLights++;
				if (numLights == maxLights) break;
			}
	}

	if (numLights == 0) return;

	TRoadSegInfo info;
	pRoad->GetRoadSegInfo(&info);
	Real roadLen = info.roadVector.Length();
	Real halfHeight = info.roadNormal.Length();
	info.roadNormal.Normalize();
	info.roadVector.Normalize();
	Vector2 curVector;
	Int uCount = (roadLen/MAP_XY_FACTOR)+1;
	Int vCount = (2*halfHeight/MAP_XY_FACTOR)+1;


	const int maxRows = 100;
	typedef struct {
		Bool collapsed;
		Bool deleted;
		Vector3 vtx[maxRows];
		Int diffuseRed;
		Bool lightGradient;
		Int vertexIndex[maxRows];
		Real uIndex;
	} TColumn;
//	const Int DIFFUSE_LIMIT = 25; // if more than that, we tesselate :) jba.

	if (vCount>maxRows) vCount = maxRows;
	TColumn prevColumn, curColumn, nextColumn;

	prevColumn.deleted = true;
	curColumn.deleted = true;
	Int i, j, k;
	Vector2 v2 = info.corners[bottomLeft];
	Vector3 origin(v2.X, v2.Y, 0);
	v2 = info.corners[bottomRight] - info.corners[bottomLeft];
	Vector3 uVector1(v2.X, v2.Y, 0);
	v2 = info.corners[topRight] - info.corners[topLeft];
	Vector3 uVector2(v2.X, v2.Y, 0);
	v2 = info.corners[topLeft];
	Vector3 origin2(v2.X, v2.Y, 0);
	v2 = info.corners[topLeft] - info.corners[bottomLeft];
	Vector3 vVector1(v2.X, v2.Y, 0);
	v2 = info.corners[topRight] - info.corners[bottomRight];
	Vector3 vVector2(v2.X, v2.Y, 0);
	uVector2 += (vVector1 - vVector2);
	for (i=0; i<=uCount; i++) {
		Real iFactor = ((Real)i / (uCount-1));
		Real iBarFactor = 1.0f-iFactor;
		if (i<uCount) {
			nextColumn.collapsed = false;
			nextColumn.deleted = false;
			nextColumn.lightGradient = false;
			nextColumn.uIndex = i;

			Real minHeight=m_map->getMaxHeightValue()*m_map->getHeightScale();
			Real maxHeight = m_map->getMinHeightValue()*m_map->getHeightScale();
			for (j=0; j<vCount; j++) {
				Real jFactor = ((Real)j / (vCount-1));
				Real jBarFactor = 1.0f-jFactor;
				nextColumn.vtx[j] = origin +  (uVector1 * jBarFactor * iFactor) + (uVector2 * jFactor * iFactor) +
													(vVector1 * iBarFactor * jFactor) + (vVector2 * iFactor * jFactor) ;
				Real z = TheTerrainRenderObject->getMaxCellHeight(nextColumn.vtx[j].X, nextColumn.vtx[j].Y);
				if (z<minHeight) minHeight = z;
				if (z>maxHeight) maxHeight = z;
				nextColumn.vertexIndex[j] = -1;
				nextColumn.vtx[j].Z = z;
				Int k;
				for (k=0; k<numLights; k++) {
					Vector3 offset = nextColumn.vtx[j] - lights[k]->Get_Position();
					Real range = lights[k]->Get_Attenuation_Range();
					// for culling, expand one cell radius.
					range += MAP_XY_FACTOR;
					if (offset.Length2() < range*range) {
						nextColumn.lightGradient = true;
					}
				}
			}
			if (!nextColumn.lightGradient) {
				nextColumn.collapsed = true;
				nextColumn.vtx[0].Z = maxHeight;
				nextColumn.vtx[1] = nextColumn.vtx[vCount-1];
				nextColumn.vtx[1].Z = maxHeight;
			}	else {
				for (j=0; j<vCount; j++) {
					nextColumn.vtx[j].Z = maxHeight;
				}
			}
			if (i<2) {
				curColumn = nextColumn;
			} else {
				if (prevColumn.collapsed && curColumn.collapsed && nextColumn.collapsed) {
					Bool okToDelete = false;

					Real theZ = prevColumn.vtx[0].Z * (curColumn.uIndex-prevColumn.uIndex) +
										nextColumn.vtx[0].Z * (nextColumn.uIndex-curColumn.uIndex);
					theZ /= nextColumn.uIndex-prevColumn.uIndex;
					if (theZ >= curColumn.vtx[0].Z && theZ < curColumn.vtx[0].Z + MAX_ERROR) {
						theZ = prevColumn.vtx[1].Z * (curColumn.uIndex-prevColumn.uIndex) +
											nextColumn.vtx[1].Z * (nextColumn.uIndex-curColumn.uIndex);
						theZ /= nextColumn.uIndex-prevColumn.uIndex;
						if (theZ >= curColumn.vtx[1].Z && theZ < curColumn.vtx[1].Z + MAX_ERROR) {
							okToDelete = true;
						}
					}
					if (okToDelete) {
						curColumn.deleted = true;
					}
				}
			}
		}
		if (!curColumn.deleted && i!=1) {
			// Write out the vertices.
			for (j=0; j<vCount; j++) {
							// GeneralsX: No per-iteration bound existed here; only a check
							// after the loop, so a wide column could still overrun vb[].
							if (m_curNumRoadVertices >= MAX_SEG_VERTEX) {
								break;
							}
				Real U, V;
				if (m_curNumRoadVertices >= m_curVertexCapacity) {
					break;
				}
				curVector.Set(curColumn.vtx[j].X - info.loc.X, curColumn.vtx[j].Y - info.loc.Y);
				V = Vector2::Dot_Product(info.roadNormal, curVector);
				U = Vector2::Dot_Product(info.roadVector, curVector);
				Int diffuse = (255<<24)|TheTerrainRenderObject->getStaticDiffuse(curColumn.vtx[j].X/MAP_XY_FACTOR+0.5, curColumn.vtx[j].Y/MAP_XY_FACTOR+0.5);
				Real shadeR, shadeG, shadeB;
				shadeB = (diffuse & 0xFF)/255.0;
				shadeG = ((diffuse>>8) & 0xFF)/255.0;
				shadeR = ((diffuse>>16) & 0xFF)/255.0;
				Int k;
				for (k=0; k<numLights; k++) {
					Real factor;
					if (lights[k]->Get_Type() == LightClass::POINT) {
						Vector3 lightLoc = lights[k]->Get_Position();
						Vector3 vtx = curColumn.vtx[j];
						Vector3 offset = vtx - lightLoc;
						double range, midRange;
						lights[k]->Get_Far_Attenuation_Range(midRange, range);
						if (vtx.X < lightLoc.X-range) continue;
						if (vtx.X > lightLoc.X+range) continue;
						if (vtx.Y < lightLoc.Y-range) continue;
						if (vtx.Y > lightLoc.Y+range) continue;
						Real dist = offset.Length();
						if (dist >= range) continue;
						if (midRange < 0.1) continue;
	#if 1
						factor = 1.0f - (dist - midRange) / (range - midRange);
	#else
						// f = 1.0 / (atten0 + d*atten1 + d*d/atten2);
						if (fabs(range-midRange)<1e-5)	{
							// if the attenuation range is too small assume uniform with cutoff
							factor = 1.0;
						}	else  {
							factor = 1.0f/(0.1+dist/midRange + 5.0f*dist*dist/(range*range));
						}
	#endif
						factor = WWMath::Clamp(factor,0.0f,1.0f);
						Real shade = 0.5f;
						shade *= factor;
						Vector3 diffuse;
						lights[k]->Get_Diffuse(&diffuse);
						Vector3 ambient;
						lights[k]->Get_Ambient(&ambient);
						if (shade > 1.0) shade = 1.0;
						if(shade < 0.0f) shade = 0.0f;
						shadeR += shade*diffuse.X;
						shadeG += shade*diffuse.Y;
						shadeB += shade*diffuse.Z;
						shadeR += factor*ambient.X;
						shadeG += factor*ambient.Y;
						shadeB += factor*ambient.Z;
					}
				}
 				if (shadeR > 1.0) shadeR = 1.0;
				if(shadeR < 0.0f) shadeR = 0.0f;
				if (shadeG > 1.0) shadeG = 1.0;
				if(shadeG < 0.0f) shadeG = 0.0f;
				if (shadeB > 1.0) shadeB = 1.0;
				if(shadeB < 0.0f) shadeB = 0.0f;
				shadeR*=255;
				shadeG*=255;
				shadeB*=255;
				diffuse=REAL_TO_INT(shadeB) | (REAL_TO_INT(shadeG) << 8) | (REAL_TO_INT(shadeR) << 16) | ((int)255 << 24);

			#ifdef RTS_DEBUG
				//diffuse &= 0xFFFF00FF; // strip out green.
			#endif
				vb[m_curNumRoadVertices].u1 = info.uOffset+U/(info.scale*4);
				vb[m_curNumRoadVertices].v1 = info.vOffset-V/(info.scale*4);	// Road is 1/16 texture height.
				vb[m_curNumRoadVertices].x = curColumn.vtx[j].X;
				vb[m_curNumRoadVertices].y = curColumn.vtx[j].Y;
				vb[m_curNumRoadVertices].z = curColumn.vtx[j].Z+FLOAT_AMOUNT;
				vb[m_curNumRoadVertices].diffuse = diffuse;
				curColumn.vertexIndex[j] = m_curNumRoadVertices;
				m_curNumRoadVertices++;
				if (j==1 && curColumn.collapsed) {
					break;
				}
			}
			if (m_curNumRoadVertices >= MAX_SEG_VERTEX) {
				break;
			}
			if (i>1 && (!prevColumn.collapsed || !curColumn.collapsed)) {
				// Write out the triangles.
				j = 0;
				k = 0;
				while (j<vCount-1 && k<vCount-1) {
					if (m_curNumRoadIndices >= m_curIndexCapacity) {
						break;
					}
					UnsignedShort *curIb = ib+m_curNumRoadIndices;
					if (k==0 || !prevColumn.collapsed) {
						*curIb++ = prevColumn.vertexIndex[j+1];
						*curIb++ = prevColumn.vertexIndex[j];
						*curIb++ = curColumn.vertexIndex[k];
						m_curNumRoadIndices+=3;
					}
					if (j==0 || !curColumn.collapsed) {
						Int offset = 1;
						if (curColumn.collapsed && !prevColumn.collapsed) {
							offset = vCount-1;
						}
						*curIb++ = prevColumn.vertexIndex[j+offset];
						*curIb++ = curColumn.vertexIndex[k];
						*curIb++ = curColumn.vertexIndex[k+1];
						m_curNumRoadIndices+=3;
					}
					if (prevColumn.collapsed && curColumn.collapsed) {
						break;
					}
					if (!prevColumn.collapsed) {
						j++;
					}
					if (!curColumn.collapsed) {
						k++;
					}
				}
				prevColumn = curColumn;
			}	else if (i==0) {
				prevColumn = curColumn;
			}
			if (m_curNumRoadIndices >= MAX_SEG_INDEX) {
				break;
			}
		}
		curColumn = nextColumn;
	}
}

//=============================================================================
// W3DRoadBuffer::loadCurve
//=============================================================================
/** Loads a curve segment into the vertex buffer for a road end cap or join. */
//=============================================================================
void W3DRoadBuffer::loadCurve(RoadSegment *pRoad, Vector2 loc1, Vector2 loc2, Real scale)
{

	Real uOffset = (4.0f/512.0f);
	Real vOffset = (255.0f/512.0f);	 // CORNER_RADIUS radius 1.5 curve.
	if (pRoad->m_curveRadius == TIGHT_CORNER_RADIUS) {
		vOffset = (425.0f/512.0f);
	}

	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
	Real roadLen = scale;

	Real curveHeight = pRoad->m_widthInTexture*scale/2.0f;

	roadVector.Normalize();
	roadVector *= roadLen;

	roadNormal.Normalize();
	roadNormal *= abs(curveHeight);

	Vector2	corners[NUM_CORNERS];

	corners[bottomLeft] = loc1 - roadNormal;
	corners[bottomRight] = corners[bottomLeft];
	corners[bottomRight] += roadVector;
	corners[topRight] = corners[bottomRight];
 	corners[topRight] += 2*roadNormal;
	corners[topLeft] = corners[bottomLeft];
 	corners[topLeft] += 2*roadNormal;

	// Tweak a little
	if (pRoad->m_curveRadius == TIGHT_CORNER_RADIUS) {
		corners[bottomLeft] = loc1 - roadNormal;
		corners[bottomRight] = corners[bottomLeft];
		corners[bottomRight] += roadVector*0.5f;
		corners[topRight] = corners[bottomRight];
 		corners[topRight] += 2*roadNormal;
		corners[topLeft] = corners[bottomLeft];
 		corners[topLeft] += 2*roadNormal;

		corners[bottomRight] += roadVector*0.1f;
		corners[bottomRight] += roadNormal*0.2f;
		corners[bottomLeft] -= roadNormal*0.1f;
		corners[bottomLeft] -= roadVector*0.02f;
		corners[topLeft] -= roadVector*0.02f;

		corners[topRight] -= roadVector*0.4f;
		corners[topRight] += roadNormal*0.2f;
	} else {
		corners[bottomLeft] = loc1 - roadNormal;
		corners[bottomRight] = corners[bottomLeft];
		corners[bottomRight] += roadVector;
		corners[topRight] = corners[bottomRight];
 		corners[topRight] += 2*roadNormal;
		corners[topLeft] = corners[bottomLeft];
 		corners[topLeft] += 2*roadNormal;

		corners[bottomRight] += roadVector*0.1f;
		corners[bottomRight] += roadNormal*0.4f;
		corners[bottomLeft] -= roadNormal*0.2f;
		corners[bottomLeft] -= roadVector*0.02f;
		corners[topLeft] -= roadVector*0.02f;

		corners[topRight] -= roadVector*0.4f;
		corners[topRight] += roadNormal*0.4f;
	}

	loadFloat4PtSection(pRoad, loc1, roadNormal, roadVector, corners, uOffset, vOffset, scale, scale);

}

//=============================================================================
// W3DRoadBuffer::preloadRoadSegment
//=============================================================================
/** Loads a road segment into the vertex buffer for drawing. */
//=============================================================================
void W3DRoadBuffer::preloadRoadSegment(RoadSegment *pRoad)
{

	Vector2 roadVector = pRoad->m_pt2.loc-pRoad->m_pt1.loc;
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
//	Real roadLen = roadVector.Length();

	Real uOffset = 0.0f;
	Real vOffset = (85.0f/512.0f);

	Real roadHeight = pRoad->m_widthInTexture*pRoad->m_scale/2.0f;
	roadNormal.Normalize();
	roadNormal *= roadHeight;

	Vector2 corners[NUM_CORNERS];
	corners[bottomLeft] = pRoad->m_pt1.bottom;
	corners[topLeft] = pRoad->m_pt1.top;
	corners[bottomRight] = pRoad->m_pt2.bottom;
	corners[topRight] = pRoad->m_pt2.top;
	loadFloat4PtSection(pRoad, pRoad->m_pt1.loc, roadNormal, roadVector, corners, uOffset, vOffset,
		pRoad->m_scale, pRoad->m_scale);

}

//=============================================================================
// W3DRoadBuffer::preloadRoadsInVertexAndIndexBuffers
//=============================================================================
/** Loads the roads into the vertex buffer for drawing. */
//=============================================================================
void W3DRoadBuffer::preloadRoadsInVertexAndIndexBuffers()
{
	if (!m_initialized) {
		return;
	}

	Int curRoad;

	// Do road segments.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == SEGMENT) {
			preloadRoadSegment(&m_roads[curRoad]);
		}
	}
	// Do curves.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == CURVE) {
			loadCurve(&m_roads[curRoad], m_roads[curRoad].m_pt1.loc, m_roads[curRoad].m_pt2.loc, m_roads[curRoad].m_scale);
		}
	}
	// Do tees.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == THREE_WAY_Y) {
			loadY(&m_roads[curRoad], m_roads[curRoad].m_pt1.loc, m_roads[curRoad].m_pt2.loc,
				m_roads[curRoad].m_scale);
		}
	}
	// Do tees.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == THREE_WAY_H || m_roads[curRoad].m_type == THREE_WAY_H_FLIP) {
			loadH(&m_roads[curRoad], m_roads[curRoad].m_pt1.loc, m_roads[curRoad].m_pt2.loc,
				m_roads[curRoad].m_type == THREE_WAY_H_FLIP, m_roads[curRoad].m_scale);
		}
	}
	// Do tees.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == TEE || m_roads[curRoad].m_type == FOUR_WAY) {
			loadTee(&m_roads[curRoad], m_roads[curRoad].m_pt1.loc, m_roads[curRoad].m_pt2.loc,
				(m_roads[curRoad].m_type == FOUR_WAY), m_roads[curRoad].m_scale);
		}
	}
	// Do joins.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		if (m_roads[curRoad].m_type == ALPHA_JOIN) {
			loadAlphaJoin(&m_roads[curRoad], m_roads[curRoad].m_pt1.loc, m_roads[curRoad].m_pt2.loc,
				 m_roads[curRoad].m_scale);
		}
	}
	updateLighting();
}

//=============================================================================
// W3DRoadBuffer::loadRoadsInVertexAndIndexBuffers
//=============================================================================
/** Loads the roads into the vertex buffer for drawing. */
//=============================================================================
void W3DRoadBuffer::loadRoadsInVertexAndIndexBuffers()
{
	if ( !m_initialized) {
		return;
	}
	m_curNumRoadVertices = 0;
	m_curNumRoadIndices = 0;
	VertexFormatXYZDUV1 *vb;
	UnsignedShort *ib;
	// Lock the buffers.
	if (m_roadTypes[m_curRoadType].getIB() == nullptr) {
		this->m_roadTypes[m_curRoadType].setNumVertices(0);
		this->m_roadTypes[m_curRoadType].setNumIndices(0);
		return;
	}
	m_curVertexCapacity = m_roadTypes[m_curRoadType].getVertexCapacity();
	m_curIndexCapacity = m_roadTypes[m_curRoadType].getIndexCapacity();
	DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_roadTypes[m_curRoadType].getIB(), s_dynamic?D3DLOCK_DISCARD:0);
	DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_roadTypes[m_curRoadType].getVB(), s_dynamic?D3DLOCK_DISCARD:0);
	vb=(VertexFormatXYZDUV1*)lockVtxBuffer.Get_Vertex_Array();
	ib = lockIdxBuffer.Get_Index_Array();
	// Add to the index buffer & vertex buffer.

	Int curRoad;

	// Do road segments.
	TCorner corner;
	for (corner = SEGMENT; corner < NUM_JOINS; corner = (TCorner)(corner+1)) {
		for (curRoad=0; curRoad<m_numRoads; curRoad++) {
			if (m_roads[curRoad].m_type == corner) {
				loadRoadSegment(ib, vb, &m_roads[curRoad]);
			}
		}
	}
	this->m_roadTypes[m_curRoadType].setNumVertices(m_curNumRoadVertices);
	this->m_roadTypes[m_curRoadType].setNumIndices(m_curNumRoadIndices);
}

//=============================================================================
// W3DRoadBuffer::visibilityChanged
//=============================================================================
/** Returns true if any road segment's visibility changed. */
//=============================================================================
Bool W3DRoadBuffer::visibilityChanged(const IRegion2D &bounds)
{
	if ( !m_initialized) {
		return false;
	}
	Bool visChanged = false;
	Int curRoad;
	// Do road segments.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		Bool curVis = m_roads[curRoad].m_visible;
		Bool newVis = true;
		Real halfScale = m_roads[curRoad].m_scale/2;
		// Throw out segs out of view.
		if (m_roads[curRoad].m_pt1.loc.X + halfScale < bounds.lo.x &&
			m_roads[curRoad].m_pt2.loc.X + halfScale < bounds.lo.x) {
			newVis = false;
		}
		if (m_roads[curRoad].m_pt1.loc.X - halfScale > bounds.hi.x &&
			m_roads[curRoad].m_pt2.loc.X - halfScale > bounds.hi.x) {
			newVis = false;
		}
		// Throw out segs out of view.
		if (m_roads[curRoad].m_pt1.loc.Y + halfScale < bounds.lo.y &&
			m_roads[curRoad].m_pt2.loc.Y + halfScale < bounds.lo.y) {
			newVis = false;
		}
		if (m_roads[curRoad].m_pt1.loc.Y - halfScale > bounds.hi.y &&
			m_roads[curRoad].m_pt2.loc.Y - halfScale > bounds.hi.y) {
			newVis = false;
		}
		if (newVis != curVis) {
			visChanged = true;
		}
		m_roads[curRoad].m_visible = newVis;
	}
	return visChanged;
}

//=============================================================================
// W3DRoadBuffer::loadLitRoadsInVertexAndIndexBuffers
//=============================================================================
/** Loads the roads into the vertex buffer for drawing. */
//=============================================================================
void W3DRoadBuffer::loadLitRoadsInVertexAndIndexBuffers(RefRenderObjListIterator *pDynamicLightsIterator)
{
	if ( !m_initialized) {
		return;
	}
	m_curNumRoadVertices = 0;
	m_curNumRoadIndices = 0;
	VertexFormatXYZDUV1 *vb;
	UnsignedShort *ib;
	if (m_roadTypes[m_curRoadType].getIB() == nullptr) {
		this->m_roadTypes[m_curRoadType].setNumVertices(0);
		this->m_roadTypes[m_curRoadType].setNumIndices(0);
		return;
	}
	m_curVertexCapacity = m_roadTypes[m_curRoadType].getVertexCapacity();
	m_curIndexCapacity = m_roadTypes[m_curRoadType].getIndexCapacity();
	// Lock the buffers.
	DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_roadTypes[m_curRoadType].getIB());
	DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_roadTypes[m_curRoadType].getVB());
	vb=(VertexFormatXYZDUV1*)lockVtxBuffer.Get_Vertex_Array();
	ib = lockIdxBuffer.Get_Index_Array();
	// Add to the index buffer & vertex buffer.

	Int curRoad;
	if (true) {
		// Do road segments.
		TCorner corner;
		for (corner = SEGMENT; corner < NUM_JOINS; corner = (TCorner)(corner+1)) {
			for (curRoad=0; curRoad<m_numRoads; curRoad++) {
				if (m_roads[curRoad].m_type == corner) {
					loadLit4PtSection(&m_roads[curRoad], ib, vb, pDynamicLightsIterator);
				}
			}
		}
	}
	this->m_roadTypes[m_curRoadType].setNumVertices(m_curNumRoadVertices);
	this->m_roadTypes[m_curRoadType].setNumIndices(m_curNumRoadIndices);
}

//=============================================================================
// W3DRoadBuffer::loadRoadSegment
//=============================================================================
/** Loads a road segment into the vertex buffer for drawing. */
//=============================================================================
void W3DRoadBuffer::loadRoadSegment(UnsignedShort *ib, VertexFormatXYZDUV1 *vb, RoadSegment *pRoad)
{
	if (pRoad->m_uniqueID != m_curUniqueID) {
		return;
	}
	if (!pRoad->m_visible) {
		return;
	}
	Int curVertex = m_curNumRoadVertices;
	// GeneralsX @feature A segment that does not fit is counted for [GXROADS] (it is not drawn). The buffer
	// holds at most MaxRoadVertex/Index, fewer when this type's roads need fewer.
	if (curVertex+pRoad->GetNumVertex() >= m_maxRoadVertex || curVertex+pRoad->GetNumVertex() > m_curVertexCapacity ||
		m_curNumRoadIndices+pRoad->GetNumIndex() >= m_maxRoadIndex || m_curNumRoadIndices+pRoad->GetNumIndex() > m_curIndexCapacity) {
		m_rebuildDropped++;
		return;
	}
	m_curNumRoadVertices += pRoad->GetVertices(vb+curVertex, pRoad->GetNumVertex());
	m_curNumRoadIndices += pRoad->GetIndices(ib+m_curNumRoadIndices, pRoad->GetNumIndex(), curVertex);

}


//=============================================================================
// W3DRoadBuffer::checkLinkBefore
//=============================================================================
/** Checks to see if any segments need to link before this one. */
//=============================================================================
void W3DRoadBuffer::checkLinkBefore(Int ndx)
{
	// Positions are the old array's; the segment at a position is m_roads[m_build->order.at(position)].
	RoadOrder &order = m_build->order;
	// Check for things whose loc == our loc2.
	if (m_roads[order.at(ndx)].m_pt2.count != 1) {
		return;
	}

	Vector2 loc2 = m_roads[order.at(ndx)].m_pt2.loc;

	Int endOfCurSeg = ndx+1;
	while (endOfCurSeg<m_numRoads-1) {
		if (m_roads[order.at(endOfCurSeg)].m_pt1.loc != m_roads[order.at(endOfCurSeg+1)].m_pt2.loc) {
			break;
		}
		endOfCurSeg++;
	}
	Int checkNdx = endOfCurSeg+1;
	while (checkNdx < m_numRoads) {
		// The old loop stepped checkNdx to the next segment with an end at loc2; the index names it.
		checkNdx = firstRoadEndFrom(loc2, checkNdx);
		if (checkNdx < 0) {
			return;
		}
		RoadSegment &found = m_roads[order.at(checkNdx)];
		if (found.m_pt1.loc == loc2) {
			order.moveTo(checkNdx, ndx);
			loc2 = m_roads[order.at(ndx)].m_pt2.loc;
			if (m_roads[order.at(ndx)].m_pt2.count != 1) return;
			endOfCurSeg++;
		} else if (found.m_pt2.loc == loc2) {
			flipTheRoad(&found);
			order.moveTo(checkNdx, ndx);
			loc2 = m_roads[order.at(ndx)].m_pt2.loc;
			if (m_roads[order.at(ndx)].m_pt2.count != 1) return;
			endOfCurSeg++;
		}	else {
			checkNdx++;
		}
		if (checkNdx <= endOfCurSeg) {
			checkNdx = endOfCurSeg+1;
		}
	}

}

//=============================================================================
// W3DRoadBuffer::checkLinkAfter
//=============================================================================
/** Checks to see if any segments need to link after this one. */
//=============================================================================
void W3DRoadBuffer::checkLinkAfter(Int ndx)
{
	RoadOrder &order = m_build->order;
	// Check for things whose loc == our loc1.
	if (m_roads[order.at(ndx)].m_pt1.count != 1) {
		return;
	}
	if (ndx >= m_numRoads-1) {
		return;
	}

	Vector2 loc1 = m_roads[order.at(ndx)].m_pt1.loc;

	Int checkNdx = ndx+1;
	while (checkNdx < m_numRoads && ndx < m_numRoads-1) {
		checkNdx = firstRoadEndFrom(loc1, checkNdx);
		if (checkNdx < 0) {
			return;
		}
		RoadSegment &found = m_roads[order.at(checkNdx)];
		if (found.m_pt2.loc == loc1) {
			ndx++;
			order.moveTo(checkNdx, ndx);
			loc1 = m_roads[order.at(ndx)].m_pt1.loc;
			if (m_roads[order.at(ndx)].m_pt1.count != 1) return;
		} else if (found.m_pt1.loc == loc1) {
			flipTheRoad(&found);
			ndx++;
			order.moveTo(checkNdx, ndx);
			loc1 = m_roads[order.at(ndx)].m_pt1.loc;
			if (m_roads[order.at(ndx)].m_pt1.count != 1) return;
		}	else {
			checkNdx++;
		}
	}

}

//=============================================================================
// W3DRoadBuffer::firstRoadEndFrom
//=============================================================================
/** The first position at or after fromPos whose segment has an end at loc, or -1 (phases 1-2). */
//=============================================================================
Int W3DRoadBuffer::firstRoadEndFrom(const Vector2 &loc, Int fromPos)
{
	std::vector<Int> &cand = m_build->cand;
	cand.clear();
	m_build->ends.collect(loc, cand);
	Int best = -1;
	for (size_t c=0; c<cand.size(); c++) {
		const Int pos = m_build->order.rankOf(cand[c]);
		if (pos >= fromPos && (best < 0 || pos < best)) {
			best = pos;
		}
	}
	return best;
}

static Bool warnSegments = true;
#define CHECK_SEGMENTS {if (m_numRoads >= m_maxRoadSegments) { m_segmentsFull = true; if (warnSegments) DEBUG_LOG(("****** Too many road segments.  Need to increase ini values.  See john a.")); warnSegments = false; return;}}

//=============================================================================
// W3DRoadBuffer::addMapObject
//=============================================================================
/** Loads the roads from the map objects. */
//=============================================================================
void W3DRoadBuffer::addMapObject(RoadSegment *pRoad, Int slot, Bool updateTheCounts)
{

	RoadSegment cur = *pRoad;
	Vector2 loc1, loc2;
	loc1 = cur.m_pt1.loc;
	loc2 = cur.m_pt2.loc;
 	Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
	Vector2 roadNormal(-roadVector.Y, roadVector.X);
	roadNormal.Normalize();
	roadNormal *= (cur.m_scale*cur.m_widthInTexture/2.0f);
	cur.m_pt1.top = loc1+roadNormal;
	cur.m_pt1.bottom = loc1 - roadNormal;
	cur.m_pt2.top = loc2+roadNormal;
	cur.m_pt2.bottom = loc2 - roadNormal;

	if (updateTheCounts) {
		updateCounts(&cur);
	}

	CHECK_SEGMENTS;
	// The old loop walked every segment in array order for the first that is this road (both ends equal) or that
	// shares the end whose count is 1. Only a segment with an end at loc1 or loc2 can be either; the index names
	// them, and they are walked in array order, so the one found is the one the walk found.
	RoadOrder &order = m_build->order;
	std::vector<Int> &cand = m_build->cand;
	cand.clear();
	m_build->ends.collect(loc1, cand);
	m_build->ends.collect(loc2, cand);
	for (size_t c=0; c<cand.size(); c++) {
		cand[c] = order.rankOf(cand[c]);
	}
	std::sort(cand.begin(), cand.end());
	cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
	Bool flip = false;
	Bool addBefore = false;
	Bool addAfter = false;
	Bool bothMatch = false;
	Int addIndex = m_numRoads;
	for (size_t c=0; c<cand.size(); c++) {
		const RoadSegment &road = m_roads[order.at(cand[c])];
		bothMatch = false;
		if ( (road.m_pt1.loc==loc1 && road.m_pt2.loc==loc2) ||
			(road.m_pt1.loc==loc2 && road.m_pt2.loc==loc1) ) {
			bothMatch = true;	// identical segment, so discard.
			break;
		}
		if (cur.m_pt1.count==1) {
			if ((road.m_pt1.loc == loc1)) {
				flip = true;
				addAfter = true;
			}
			if ((road.m_pt2.loc ==loc1)) {
				flip = false;
				addBefore = true;
			}
		}
		if (cur.m_pt2.count==1) {
			if ((road.m_pt1.loc == loc2)) {
				flip = false;
				addAfter = true;
			}
			if ((road.m_pt2.loc == loc2)) {
				flip = true;
				addBefore = true;
			}
		}
		if (addBefore || addAfter) {
			addIndex = cand[c];
			break;
		}
	}
	if (bothMatch) {
		return;
	}
	if (addAfter) {
		addIndex++;
	}

	m_roads[slot] = cur;
	if (flip) {
		flipTheRoad(&m_roads[slot]);
	}
	order.insertAt(addIndex, slot);
	m_build->ends.addEnds(slot, m_roads[slot]);
	m_numRoads++;
	if (addBefore) {
		checkLinkBefore(addIndex);
	} else if (addAfter) {
		checkLinkAfter(addIndex);
	}
}

//=============================================================================
// W3DRoadBuffer::addMapObjects
//=============================================================================
/** Loads the roads from the map objects. */
//=============================================================================
void W3DRoadBuffer::addMapObjects()
{
	MapObject *pMapObj;
	MapObject *pMapObj2;
	m_mapRoadPairs = 0;
	m_refusedRoadPairs = 0;
	// During the two passes m_roads holds the segments by SLOT (the order they were accepted) and m_build->order holds
	// the old array's order of those slots; the array is put back in that order at the end.
	m_build->order.reset(m_maxRoadSegments);
	m_build->ends.clear();
	for (pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext()) {
		if (m_numRoads >= m_maxRoadSegments) {
			m_segmentsFull = true;
			// GeneralsX @feature Full: count the pairs left out for [GXROADS].
			for (; pMapObj; pMapObj = pMapObj->getNext()) {
				if (pMapObj->getFlag(FLAG_ROAD_POINT1) && pMapObj->getNext() && pMapObj->getNext()->getFlag(FLAG_ROAD_POINT2)) {
					m_mapRoadPairs++;
					m_refusedRoadPairs++;
					pMapObj = pMapObj->getNext();
				}
			}
			break;
		}
		if (pMapObj->getFlag(FLAG_ROAD_POINT1)) {
			pMapObj2 = pMapObj->getNext();
#ifdef RTS_DEBUG
			DEBUG_ASSERTLOG(pMapObj2 && pMapObj2->getFlag(FLAG_ROAD_POINT2), ("Bad Flag"));
#endif
			if (pMapObj2==nullptr) break;
			if (!pMapObj2->getFlag(FLAG_ROAD_POINT2)) continue;
			m_mapRoadPairs++;
			Vector2 loc1, loc2;
			loc1.Set(pMapObj->getLocation()->x, pMapObj->getLocation()->y);
			loc2.Set(pMapObj2->getLocation()->x, pMapObj2->getLocation()->y);
			if (loc1.X==loc2.X && loc1.Y==loc2.Y) {
				loc2.X += 0.25;
			}
			RoadSegment	curRoad;
			curRoad.m_scale = DEFAULT_ROAD_SCALE;
			curRoad.m_widthInTexture = 1.0f;
			curRoad.m_uniqueID = 1;
			Bool found = false;
			TerrainRoadType *road = TheTerrainRoads->findRoad( pMapObj->getName() );
			if( road )
			{
				curRoad.m_widthInTexture = road->getRoadWidthInTexture();
				curRoad.m_scale = road->getRoadWidth();
				curRoad.m_uniqueID = road->getID();
				found = TRUE;
			}
#ifdef LOAD_TEST_ASSETS
			const Real DEFAULT_SCALE = 30;
			if (!found) {
				Int i;
				for (i=0; i<m_maxRoadTypes; i++) {
					if (pMapObj->getName() == m_roadTypes[i].getPath()) {
						curRoad.m_scale = DEFAULT_SCALE;
						curRoad.m_uniqueID = m_roadTypes[i].getUniqueID();
						found = true;
					}
				}
			}
			if (!found && m_curOpenRoad<m_maxRoadTypes) {
				m_maxUID++;
				curRoad.m_scale = DEFAULT_SCALE;
				curRoad.m_uniqueID = m_maxUID;
				m_roadTypes[m_curOpenRoad].loadTexture(pMapObj->getName(), m_maxUID);
				m_curOpenRoad++;
			}
#endif
			curRoad.m_pt1.loc = loc1;
			curRoad.m_pt1.isAngled = pMapObj->getFlag(FLAG_ROAD_CORNER_ANGLED);
			curRoad.m_pt1.isJoin = pMapObj->getFlag(FLAG_ROAD_JOIN);
			curRoad.m_pt2.loc =loc2;
			curRoad.m_pt2.isAngled = pMapObj2->getFlag(FLAG_ROAD_CORNER_ANGLED);
			curRoad.m_pt2.isJoin = pMapObj2->getFlag(FLAG_ROAD_JOIN);
			curRoad.m_type = SEGMENT;
			curRoad.m_curveRadius = pMapObj->getFlag(FLAG_ROAD_CORNER_TIGHT)?TIGHT_CORNER_RADIUS:CORNER_RADIUS;

			addMapObject(&curRoad, m_numRoads, true);
			pMapObj = pMapObj2;
		}
	}
	// Second pass, as always: every accepted segment is added again, now with its final counts. The slots stay put;
	// the order is rebuilt.
	std::vector<Int> firstOrder;
	m_build->order.toVector(firstOrder);
	m_build->order.reset(m_maxRoadSegments);
	m_build->ends.clear();
	m_numRoads = 0;
	size_t i;
	for (i=0; i<firstOrder.size(); i++) {
		RoadSegment curRoad = m_roads[firstOrder[i]];
		addMapObject(&curRoad, firstOrder[i], false);
	}
	// m_roads by position again: the order the rest of the build and the drawing read.
	std::vector<Int> finalOrder;
	m_build->order.toVector(finalOrder);
	std::vector<RoadSegment> inOrder(finalOrder.size());
	for (i=0; i<finalOrder.size(); i++) {
		inOrder[i] = m_roads[finalOrder[i]];
	}
	for (i=0; i<finalOrder.size(); i++) {
		m_roads[i] = inOrder[i];
	}
	m_numRoads = (Int)finalOrder.size();

}

//=============================================================================
// W3DRoadBuffer::updateCounts
//=============================================================================
/** Updates the count and last fields. */
//=============================================================================
void W3DRoadBuffer::updateCounts(RoadSegment *pRoad)
{
	pRoad->m_pt1.last = true;
	pRoad->m_pt2.last = true;
	pRoad->m_pt1.multi = false;
	pRoad->m_pt2.multi = false;
	pRoad->m_pt1.count = 0;
	pRoad->m_pt2.count = 0;
	Vector2 loc1, loc2;
	loc1 = pRoad->m_pt1.loc;
	loc2 = pRoad->m_pt2.loc;
	// Only a segment with an end at loc1 or loc2 changes anything; the index names them, each once.
	std::vector<Int> &cand = m_build->cand;
	cand.clear();
	m_build->ends.collect(loc1, cand);
	m_build->ends.collect(loc2, cand);
	std::sort(cand.begin(), cand.end());
	cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
	for (size_t c=0; c<cand.size(); c++) {
		RoadSegment &road = m_roads[cand[c]];
		// Only connect same type of road.
		if (road.m_uniqueID != pRoad->m_uniqueID) {
			continue;
		}
		if ((road.m_pt1.loc == loc1)) {
			road.m_pt1.count++;
			pRoad->m_pt1.count++;
		}
		if ((road.m_pt1.loc == loc2)) {
			road.m_pt1.count++;
			pRoad->m_pt2.count++;
		}
		if ((road.m_pt2.loc ==loc1)) {
			road.m_pt2.count++;
			pRoad->m_pt1.count++;
		}
		if ((road.m_pt2.loc == loc2)) {
			road.m_pt2.count++;
			pRoad->m_pt2.count++;
		}
		road.m_pt1.multi = road.m_pt1.count > 1;
		road.m_pt2.multi = road.m_pt2.count > 1;
	}
	pRoad->m_pt1.multi = pRoad->m_pt1.count > 1;
	pRoad->m_pt2.multi = pRoad->m_pt2.count > 1;
}

//=============================================================================
// W3DRoadBuffer::updateCountsAndFlags
//=============================================================================
/** Updates the count and last fields. */
//=============================================================================
void W3DRoadBuffer::updateCountsAndFlags()
{
	Int i;
	for (i=0; i<m_numRoads; i++) {
		m_roads[i].m_pt1.last = true;
		m_roads[i].m_pt2.last = true;
		m_roads[i].m_pt1.count = 0;
		m_roads[i].m_pt2.count = 0;
	}
	// The old pass compared every pair of same-type segments. Per end that is: count = the other same-type ends at
	// its point, last = no later segment of its type has an end there.
	RoadEndpointMap &ends = m_build->ends;
	ends.clear();
	for (i=0; i<m_numRoads; i++) {
		ends.addEnds(i, m_roads[i]);
	}
	std::vector<Int> &cand = m_build->cand;
	for (i=0; i<m_numRoads; i++) {
		TRoadPt *pts[2] = {&m_roads[i].m_pt1, &m_roads[i].m_pt2};
		for (Int e=0; e<2; e++) {
			const Vector2 loc = pts[e]->loc;
			cand.clear();
			ends.collect(loc, cand);
			Int count = 0;
			Bool later = false;
			for (size_t c=0; c<cand.size(); c++) {
				const Int j = cand[c];
				if (j == i || m_roads[j].m_uniqueID != m_roads[i].m_uniqueID) {
					continue;
				}
				if (m_roads[j].m_pt1.loc == loc) count++;
				if (m_roads[j].m_pt2.loc == loc) count++;
				if (j > i) later = true;
			}
			pts[e]->count = count;
			pts[e]->last = !later;
		}
	}
}

//=============================================================================
// W3DRoadBuffer::insertTee
//=============================================================================
/** Inserts a Tee intersection. */
//=============================================================================
void W3DRoadBuffer::insertTee(Vector2 loc, Int index1, Real scale)
{

	if (insertY(loc,index1, scale)) {
		return;
	}

	// pr1-3 point to the points on the segments that form the tee.
	// They are the points on the segments that are != loc.
	TRoadPt *pr1=nullptr;
	TRoadPt *pr2=nullptr;
	TRoadPt *pr3=nullptr;

	// pc1-3 point to the center points of the segments.  These are the
	// points that are at loc.
	TRoadPt *pc1=nullptr;
	TRoadPt *pc2=nullptr;
	TRoadPt *pc3=nullptr;

	if (m_roads[index1].m_pt1.loc == loc) {
		pr1 = &m_roads[index1].m_pt2;
		pc1 = &m_roads[index1].m_pt1;
	} else {
		pr1 = &m_roads[index1].m_pt1;
		pc1 = &m_roads[index1].m_pt2;
	}
	Int index2=0;
	Int index3=0;
	std::vector<Int> &cand = m_build->cand;
	roadsAtPointAfter(loc, index1, cand);
	for (size_t c=0; c<cand.size(); c++) {
		const Int i = cand[c];
		if (m_roads[i].m_pt1.loc == loc) {
			m_roads[i].m_pt1.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt2;
				pc2 = &m_roads[i].m_pt1;
				index2 = i;
			} else {
				pr3 = &m_roads[i].m_pt2;
				pc3 = &m_roads[i].m_pt1;
				index3 = i;
			}
		}
		if (m_roads[i].m_pt2.loc == loc) {
			m_roads[i].m_pt2.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt1;
				pc2 = &m_roads[i].m_pt2;
				index2 = i;
			} else {
				pr3 = &m_roads[i].m_pt1;
				pc3 = &m_roads[i].m_pt2;
				index3 = i;
			}
		}
	}
	if (pr2 == nullptr || pr3 == nullptr) {
		return;
	}

	Vector2 v1 = pr1->loc - loc;
	v1.Normalize();
	Vector2 v2 = pr2->loc - loc;
	v2.Normalize();
	Vector2 v3 = pr3->loc - loc;
	v3.Normalize();
	Real dot12 = v1.Dot_Product(v1, v2);
	Real dot13 = v1.Dot_Product(v1, v3);
	Real dot32 = v1.Dot_Product(v3, v2);
	// The greatest negative dot product is the pair that is heading most opposite each other.
	Bool do12 = false;
	Bool do13 = false;
	Bool do32 = false;

	if (dot12<dot13) {
		if (dot12<dot32) {
			do12 = true;
		} else {
			do32 = true;
		}
	} else {
		if (dot13<dot32) {
			do13 = true;
		} else {
			do32 = true;
		}
	}

	Vector2 upVector;
	Vector2 decider;
	if (do12) {
		upVector = v2-v1;
		decider = v3;
	}
	if (do13) {
		upVector = v3-v1;
		decider = v2;
	}
	if (do32) {
		upVector = v2-v3;
		decider = v1;
	}
	upVector.Normalize();


	// Check to see if the Tee side is slanted.
	const Real cos60 = 0.5f;
	Real dot = fabs(Vector2::Dot_Product(upVector, decider));
	if (dot > cos60) {
		// The arm of the tee is slanted, so do a slant tee.
		Real angle = (PI/2); // 90 degrees.
		Real xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X,upVector.Y,0), Vector3(decider.X, decider.Y,0));
		Bool mirror = false;
		if (xpdct<0) {
			angle = -angle;
			mirror = true;
		}
		upVector.Normalize();
		upVector *= 0.5*scale; // we are offseting one half road width.
		Vector2 teeVector(upVector);
		teeVector.Rotate(angle);

		Bool flip;
		if (do12) {
			flip = xpSign(teeVector, v3) == 1;
			offsetH(pc1, pc2, pc3, loc, upVector, teeVector, flip, mirror, m_roads[index1].m_widthInTexture);
		}
		if (do13) {
			flip = xpSign(teeVector, v2) == 1;
			offsetH(pc1, pc3, pc2, loc, upVector, teeVector, flip, mirror, m_roads[index1].m_widthInTexture);
		}
		if (do32) {
			flip = xpSign(teeVector, v1) == 1;
			offsetH(pc3, pc2, pc1, loc, upVector, teeVector, flip, mirror, m_roads[index1].m_widthInTexture);

		}

		// The offsets moved these ends: the index learns their new points.
		m_build->ends.addEnds(index1, m_roads[index1]);
		m_build->ends.addEnds(index2, m_roads[index2]);
		m_build->ends.addEnds(index3, m_roads[index3]);
		pc1->last = true;
		pc1->count = 0;
		pc2->last = true;
		pc2->count = 0;
		pc3->last = true;
		pc3->count = 0;

		CHECK_SEGMENTS;
		m_roads[m_numRoads].m_pt1.loc.Set(loc);
		m_roads[m_numRoads].m_pt2.loc.Set(loc+teeVector);
		m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_scale = m_roads[index1].m_scale;
		m_roads[m_numRoads].m_widthInTexture = m_roads[index1].m_widthInTexture;
		m_roads[m_numRoads].m_pt1.count = -3;
		m_roads[m_numRoads].m_type = flip?THREE_WAY_H_FLIP:THREE_WAY_H;
		m_roads[m_numRoads].m_uniqueID = m_roads[index1].m_uniqueID;
		m_numRoads++;
	m_build->ends.addEnds(m_numRoads-1, m_roads[m_numRoads-1]);
	} else {
		// Do a not slanted tee.
		Real angle = (PI/2); // 90 degrees.
		Real xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X,upVector.Y,0), Vector3(decider.X, decider.Y,0));
		if (xpdct<0) angle = -angle;
		upVector.Normalize();
		upVector *= 0.5*scale; // we are offseting one half road width.
		Vector2 teeVector(upVector);
		teeVector.Rotate(angle);

		if (do12) {
			offset3Way(pc1, pc2, pc3, loc, upVector, teeVector, m_roads[index1].m_widthInTexture);
		}
		if (do13) {
			offset3Way(pc1, pc3, pc2, loc, upVector, teeVector, m_roads[index1].m_widthInTexture);
		}
		if (do32) {
			offset3Way(pc3, pc2, pc1, loc, upVector, teeVector, m_roads[index1].m_widthInTexture);
		}
		// The offsets moved these ends: the index learns their new points.
		m_build->ends.addEnds(index1, m_roads[index1]);
		m_build->ends.addEnds(index2, m_roads[index2]);
		m_build->ends.addEnds(index3, m_roads[index3]);
		pc1->last = true;
		pc1->count = 0;
		pc2->last = true;
		pc2->count = 0;
		pc3->last = true;
		pc3->count = 0;

		CHECK_SEGMENTS;
		m_roads[m_numRoads].m_pt1.loc.Set(loc);
		m_roads[m_numRoads].m_pt2.loc.Set(loc+teeVector);
		m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_scale = m_roads[index1].m_scale;
		m_roads[m_numRoads].m_widthInTexture = m_roads[index1].m_widthInTexture;
		m_roads[m_numRoads].m_pt1.count = -3;
		m_roads[m_numRoads].m_type = TEE;
		m_roads[m_numRoads].m_uniqueID = m_roads[index1].m_uniqueID;
		m_numRoads++;
	m_build->ends.addEnds(m_numRoads-1, m_roads[m_numRoads-1]);
	}
}

//=============================================================================
// W3DRoadBuffer::insertY
//=============================================================================
/** Inserts a Y intersection if the corner meets "Y" criteria. */
//=============================================================================
Bool W3DRoadBuffer::insertY(Vector2 loc, Int index1, Real scale)
{
	// pr1-3 point to the points on the segments that form the tee.
	// They are the points on the segments that are != loc.
	TRoadPt *pr1=nullptr;
	TRoadPt *pr2=nullptr;
	TRoadPt *pr3=nullptr;

	// pc1-3 point to the center points of the segments.  These are the
	// points that are at loc.
	TRoadPt *pc1=nullptr;
	TRoadPt *pc2=nullptr;
	TRoadPt *pc3=nullptr;

	if (m_roads[index1].m_pt1.loc == loc) {
		pr1 = &m_roads[index1].m_pt2;
		pc1 = &m_roads[index1].m_pt1;
	} else {
		pr1 = &m_roads[index1].m_pt1;
		pc1 = &m_roads[index1].m_pt2;
	}
	Int index2=0;
	Int index3=0;
	std::vector<Int> &cand = m_build->cand;
	roadsAtPointAfter(loc, index1, cand);
	for (size_t c=0; c<cand.size(); c++) {
		const Int i = cand[c];
		if (m_roads[i].m_pt1.loc == loc) {
			m_roads[i].m_pt1.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt2;
				pc2 = &m_roads[i].m_pt1;
				index2 = i;
			} else {
				pr3 = &m_roads[i].m_pt2;
				pc3 = &m_roads[i].m_pt1;
				index3 = i;
			}
		}
		if (m_roads[i].m_pt2.loc == loc) {
			m_roads[i].m_pt2.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt1;
				pc2 = &m_roads[i].m_pt2;
				index2 = i;
			} else {
				pr3 = &m_roads[i].m_pt1;
				pc3 = &m_roads[i].m_pt2;
				index3 = i;
			}
		}
	}
	if (pr2 == nullptr || pr3 == nullptr) {
		return false;
	}

	Vector2 v1 = pr1->loc - loc;
	v1.Normalize();
	Vector2 v2 = pr2->loc - loc;
	v2.Normalize();
	Vector2 v3 = pr3->loc - loc;
	v3.Normalize();

	Bool do12 = false;
	Bool do13 = false;
	Bool do32 = false;

	Real dot12 = v1.Dot_Product(v1, v2);
	Real dot13 = v1.Dot_Product(v1, v3);
	Real dot32 = v1.Dot_Product(v3, v2);
	Real score12 = 2.0f;
	Real score13 = 2.0f;
	Real score32 = 2.0f;

//	const Real cos60 = 0.5f;
	const Real cos30 = 0.866f;
	const Real cos45 = 0.707f;

	if (dot12 < (-cos30)) return false; // Too close to a straight line, do a straight side tee.
	if (dot13 < (-cos30)) return false; // Too close to a straight line, do a straight side tee.
	if (dot32 < (-cos30)) return false; // Too close to a straight line, to a straight side tee.



	Int s1 = 0;
	Int s2 = xpSign(v1, v2);
	Int s3 = xpSign(v1, v3);

	if (s2!=s3 && (s2+s3==0)) {
		Vector2 v1_90(-v1.Y, v1.X);
		if (xpSign(v1_90, v2) == 1 && xpSign(v1_90, v3) == 1) {
			// s2 & s3 could be Y legs.
			do32 = true;
			score32 = fabs(dot12+cos45) + fabs(dot13+cos45);
		}
	}

	s1 = xpSign(v3, v1);
	s2 = xpSign(v3, v2);
	if (s2!=s1 && (s2+s1==0)) {
		// s2 & s3 could be Y legs.
		Vector2 v3_90(-v3.Y, v3.X);
		if (xpSign(v3_90, v2) == 1 && xpSign(v3_90, v1) == 1) {
			do12 = true;
			score12 = fabs(dot13+cos45) + fabs(dot32+cos45);
		}
	}

	s1 = xpSign(v2, v1);
	s3 = xpSign(v2, v3);
	if (s3!=s1 && (s3+s1==0)) {
		// s2 & s3 could be Y legs.
		Vector2 v2_90(-v2.Y, v2.X);
		if (xpSign(v2_90, v3) == 1 && xpSign(v2_90, v1) == 1) {
			do13 = true;
			score13 = fabs(dot12+cos45) + fabs(dot32+cos45);
		}
	}

	if (score12<score13) {
		do13 = false;
		if (score12<score32) {
			do32 = false;
		} else {
			do12 = false;
		}
	} else {
		do12 = false;
		if (score13<score32) {
			do32 = false;
		} else {
			do13 = false;
		}
	}


	Vector2 upVector;
	if (do12) {
		upVector = v3;
	} else if (do13) {
		upVector = v2;
	} else if (do32) {
		upVector = v1;
	} else {
		return false;
	}

	Real angle = -(PI/2); // -90 degrees.
	upVector.Normalize();
	upVector *= 0.5*scale; // we are offseting one half road width.
	Vector2 teeVector(upVector);
	teeVector.Rotate(angle);

	// Offset the ends of the road to meet the stumps of the y.
	if (do12) {
		if (xpSign(v3, v1) == -1) {
			offsetY(pc1, pc2, pc3, loc, upVector, m_roads[index1].m_widthInTexture);
		} else {
			offsetY(pc2, pc1, pc3, loc, upVector, m_roads[index1].m_widthInTexture);
		}
	}
	if (do13) {
		if (xpSign(v2, v1) == -1) {
			offsetY(pc1, pc3, pc2, loc, upVector, m_roads[index1].m_widthInTexture);
		} else {
			offsetY(pc3, pc1, pc2, loc, upVector, m_roads[index1].m_widthInTexture);
		}
	}
	if (do32) {
		if (xpSign(v1, v3) == -1) {
			offsetY(pc3, pc2, pc1, loc, upVector, m_roads[index1].m_widthInTexture);
		} else {
			offsetY(pc2, pc3, pc1, loc, upVector, m_roads[index1].m_widthInTexture);
		}
	}

	// The offsets moved these ends: the index learns their new points.
	m_build->ends.addEnds(index1, m_roads[index1]);
	m_build->ends.addEnds(index2, m_roads[index2]);
	m_build->ends.addEnds(index3, m_roads[index3]);
	pc1->last = true;
	pc1->count = 0;
	pc2->last = true;
	pc2->count = 0;
	pc3->last = true;
	pc3->count = 0;

	if (m_numRoads >= m_maxRoadSegments) {
		m_segmentsFull = true;
		return false;
	}
	m_roads[m_numRoads].m_pt1.loc.Set(loc);
	m_roads[m_numRoads].m_pt2.loc.Set(loc+teeVector);
	m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
	m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
	m_roads[m_numRoads].m_scale = m_roads[index1].m_scale;
	m_roads[m_numRoads].m_widthInTexture = m_roads[index1].m_widthInTexture;
	m_roads[m_numRoads].m_pt1.count = -3;
	m_roads[m_numRoads].m_type = THREE_WAY_Y;
	m_roads[m_numRoads].m_uniqueID = m_roads[index1].m_uniqueID;
	m_numRoads++;
	m_build->ends.addEnds(m_numRoads-1, m_roads[m_numRoads-1]);


	return true;


}

//=============================================================================
// W3DRoadBuffer::offset3Way
//=============================================================================
/** Offsets the points coming into a 3 way intersection so that they move to
the join points of the 3 way intersection. */
//=============================================================================
void W3DRoadBuffer::offset3Way(TRoadPt *pc1, TRoadPt *pc2, TRoadPt *pc3, Vector2 loc, Vector2 upVector,
															 Vector2 teeVector, Real widthInTexture)
{
	pc1->loc = loc - upVector;
	pc2->loc = loc + upVector;
	pc3->loc = loc + teeVector;

	// Adjust the top & bottoms, so they merge smoothly into the tee.

	// Make sure the t vector goes right with respect to the up vector.
	Real xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X,upVector.Y,0), Vector3(teeVector.X, teeVector.Y,0));
	Vector2 rightTee = teeVector;
	if (xpdct<0) {
		rightTee.X = -teeVector.X;
		rightTee.Y = -teeVector.Y;
	}
	rightTee *= (widthInTexture);

	xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X, upVector.Y, 0), Vector3(pc1->top.X-pc1->loc.X, pc1->top.Y-pc1->loc.Y,0));
	if (xpdct>0) {
		pc1->bottom = pc1->loc - rightTee;
		pc1->top = pc1->loc + rightTee;
	} else {
		pc1->bottom = pc1->loc + rightTee;
		pc1->top = pc1->loc - rightTee;
	}
	xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X, upVector.Y, 0), Vector3(pc2->top.X-pc2->loc.X, pc2->top.Y-pc2->loc.Y,0));
	if (xpdct>0) {
		pc2->bottom = pc2->loc - rightTee;
		pc2->top = pc2->loc + rightTee;
	} else {
		pc2->bottom = pc2->loc + rightTee;
		pc2->top = pc2->loc - rightTee;
	}
	upVector *= (widthInTexture);
	xpdct = Vector3::Cross_Product_Z(Vector3(rightTee.X, rightTee.Y, 0), Vector3(pc3->top.X-pc3->loc.X, pc3->top.Y-pc3->loc.Y,0));
	if (xpdct<0) {
		pc3->bottom = pc3->loc - upVector;
		pc3->top = pc3->loc + upVector;
	} else {
		pc3->bottom = pc3->loc + upVector;
		pc3->top = pc3->loc - upVector;
	}
}

//=============================================================================
// W3DRoadBuffer::offsetH
//=============================================================================
/** Offsets the points coming into a 3 way intersection so that they move to
the join points of the 3 way intersection. */
//=============================================================================
void W3DRoadBuffer::offsetH(TRoadPt *pc1, TRoadPt *pc2, TRoadPt *pc3, Vector2 loc,
														Vector2 upVector, Vector2 teeVector,
														Bool flip, Bool mirror, Real widthInTexture)
{

	if (flip != mirror) {
		pc1->loc = loc - upVector*2.05f;
		pc2->loc = loc + upVector*.46f;
	} else {
		pc1->loc = loc - upVector*.46f;
		pc2->loc = loc + upVector*2.05f;
	}
	//pc3->loc = loc + teeVector;

	// Adjust the top & bottoms, so they merge smoothly into the tee.

	// Make sure the t vector goes right with respect to the up vector.
	Real xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X,upVector.Y,0), Vector3(teeVector.X, teeVector.Y,0));
	Vector2 rightTee = teeVector;
	if (xpdct<0) {
		rightTee.X = -teeVector.X;
		rightTee.Y = -teeVector.Y;
	}
	rightTee *= (widthInTexture);

	xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X, upVector.Y, 0), Vector3(pc1->top.X-pc1->loc.X, pc1->top.Y-pc1->loc.Y,0));
	if (xpdct>0) {
		pc1->bottom = pc1->loc - rightTee;
		pc1->top = pc1->loc + rightTee;
	} else {
		pc1->bottom = pc1->loc + rightTee;
		pc1->top = pc1->loc - rightTee;
	}
	xpdct = Vector3::Cross_Product_Z(Vector3(upVector.X, upVector.Y, 0), Vector3(pc2->top.X-pc2->loc.X, pc2->top.Y-pc2->loc.Y,0));
	if (xpdct>0) {
		pc2->bottom = pc2->loc - rightTee;
		pc2->top = pc2->loc + rightTee;
	} else {
		pc2->bottom = pc2->loc + rightTee;
		pc2->top = pc2->loc - rightTee;
	}


	Vector2 arm = teeVector;
	if (flip) {
		arm.Rotate(PI/4);
	} else {
		arm.Rotate(-PI/4);
	}
	Vector2 armNormal(-arm.Y, arm.X);
	armNormal *= widthInTexture;

	pc3->loc += arm*2.10f;
	xpdct = xpSign(arm, pc3->top-loc);
	if (xpdct==1) {
		pc3->top = pc3->loc + armNormal;
		pc3->bottom = pc3->loc - armNormal;
	} else {
		pc3->top = pc3->loc - armNormal;
		pc3->bottom = pc3->loc + armNormal;
	}

}



//=============================================================================
// W3DRoadBuffer::offsetY
//=============================================================================
/** Offsets the points coming into a 3 way intersection so that they move to
the join points of the 3 way intersection. */
//=============================================================================
void W3DRoadBuffer::offsetY(TRoadPt *pc1, TRoadPt *pc2, TRoadPt *pc3, Vector2 loc, Vector2 upVector,
															 Real widthInTexture)
{
	pc3->loc += upVector*0.55f;
	pc3->top += upVector*0.55f;
	pc3->bottom += upVector*0.55f;

	// Adjust the bottom, so they merge smoothly into the tee.

	// Make sure the t vector goes right with respect to the up vector.
	Vector2 arm = upVector;
	arm.Rotate(3*PI/4);
	Vector2 armNormal(-arm.Y, arm.X);
	armNormal *= widthInTexture;
	pc2->loc += arm*1.1f;
	Int xpdct = xpSign(arm, pc2->top-loc);
	if (xpdct==1) {
		pc2->top = pc2->loc + armNormal;
		pc2->bottom = pc2->loc - armNormal;
	} else {
		pc2->top = pc2->loc - armNormal;
		pc2->bottom = pc2->loc + armNormal;
	}

	arm = upVector;
	arm.Rotate(-3*PI/4);
	armNormal.Set(-arm.Y, arm.X);
	armNormal *= widthInTexture;

	pc1->loc += arm*1.1f;
	xpdct = xpSign(arm, pc1->top-loc);
	if (xpdct==1) {
		pc1->top = pc1->loc + armNormal;
		pc1->bottom = pc1->loc - armNormal;
	} else {
		pc1->top = pc1->loc - armNormal;
		pc1->bottom = pc1->loc + armNormal;
	}
}

//=============================================================================
// W3DRoadBuffer::offset4Way
//=============================================================================
/** Offsets the points coming into a 4 way intersection so that they move to
the join points of the 4 way intersection. */
//=============================================================================
void W3DRoadBuffer::offset4Way(TRoadPt *pc1, TRoadPt *pc2, TRoadPt *pc3, TRoadPt *pr3,
															 TRoadPt *pc4, Vector2 loc, Vector2 alignVector, Real widthInTexture)
{
	pc1->loc = loc - alignVector;
	pc2->loc = loc + alignVector;

	Vector2 v3 = pr3->loc - loc;
	Real angle = (PI/2); // 90 degrees.
	Real xpdct = Vector3::Cross_Product_Z(Vector3(alignVector.X,alignVector.Y,0), Vector3(v3.X, v3.Y,0));
	if (xpdct<0) angle = -angle;
	Vector2 teeVector(alignVector);
	teeVector.Rotate(angle);
	pc3->loc = loc + teeVector;
	pc4->loc = loc - teeVector;

	Vector2 realTee(alignVector);
	realTee.Rotate(PI/2);
	realTee *= widthInTexture;

	// Fix up the top & bottom points.
	Vector3 align3(alignVector.X,alignVector.Y,0);

	xpdct = Vector3::Cross_Product_Z(align3, Vector3(pc1->top.X-pc1->loc.X, pc1->top.Y-pc1->loc.Y,0));
	if (xpdct>0) {
		pc1->bottom = pc1->loc - realTee;
		pc1->top = pc1->loc + realTee;
	} else {
		pc1->bottom = pc1->loc + realTee;
		pc1->top = pc1->loc - realTee;
	}
	xpdct = Vector3::Cross_Product_Z(align3, Vector3(pc2->top.X-pc2->loc.X, pc2->top.Y-pc2->loc.Y,0));
	if (xpdct>0) {
		pc2->bottom = pc2->loc - realTee;
		pc2->top = pc2->loc + realTee;
	} else {
		pc2->bottom = pc2->loc + realTee;
		pc2->top = pc2->loc - realTee;
	}
	alignVector *= widthInTexture;

	Vector3 tee3(realTee.X,realTee.Y,0);
	xpdct = Vector3::Cross_Product_Z(tee3, Vector3(pc3->top.X-pc3->loc.X, pc3->top.Y-pc3->loc.Y,0));
	if (xpdct<0) {
		pc3->bottom = pc3->loc - alignVector;
		pc3->top = pc3->loc + alignVector;
	} else {
		pc3->bottom = pc3->loc + alignVector;
		pc3->top = pc3->loc - alignVector;
	}
	xpdct = Vector3::Cross_Product_Z(tee3, Vector3(pc4->top.X-pc4->loc.X, pc4->top.Y-pc4->loc.Y,0));
	if (xpdct<0) {
		pc4->bottom = pc4->loc - alignVector;
		pc4->top = pc4->loc + alignVector;
	} else {
		pc4->bottom = pc4->loc + alignVector;
		pc4->top = pc4->loc - alignVector;
	}

	pc1->last = true;
	pc1->count = 0;
	pc2->last = true;
	pc2->count = 0;
	pc3->last = true;
	pc3->count = 0;
	pc4->last = true;
	pc4->count = 0;

}

//=============================================================================
// W3DRoadBuffer::insert4Way
//=============================================================================
/** Inserts a 4 way intersection. */
//=============================================================================
void W3DRoadBuffer::insert4Way(Vector2 loc, Int index1, Real scale)
{
	// pr1-4 point to the points on the segments that form the tee.
	// They are the points on the segments that are != loc.
	TRoadPt *pr1=nullptr;
	TRoadPt *pr2=nullptr;
	TRoadPt *pr3=nullptr;
	TRoadPt *pr4=nullptr;

	// pc1-4 point to the center points of the segments.  These are the
	// points that are at loc.
	TRoadPt *pc1=nullptr;
	TRoadPt *pc2=nullptr;
	TRoadPt *pc3=nullptr;
	TRoadPt *pc4=nullptr;

	if (m_roads[index1].m_pt1.loc == loc) {
		pr1 = &m_roads[index1].m_pt2;
		pc1 = &m_roads[index1].m_pt1;
	} else {
		pr1 = &m_roads[index1].m_pt1;
		pc1 = &m_roads[index1].m_pt2;
	}
	Int index2=0;
	Int index3=0;
	Int index4=0;
	std::vector<Int> &cand = m_build->cand;
	roadsAtPointAfter(loc, index1, cand);
	for (size_t c=0; c<cand.size(); c++) {
		const Int i = cand[c];
		if (m_roads[i].m_pt1.loc == loc) {
			m_roads[i].m_pt1.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt2;
				pc2 = &m_roads[i].m_pt1;
				index2 = i;
			} else if (pr3==nullptr) {
				pr3 = &m_roads[i].m_pt2;
				pc3 = &m_roads[i].m_pt1;
				index3 = i;
			}	else {
				pr4 = &m_roads[i].m_pt2;
				pc4 = &m_roads[i].m_pt1;
				index4 = i;
			}
		}
		if (m_roads[i].m_pt2.loc == loc) {
			m_roads[i].m_pt2.count = -2;
			if (pr2==nullptr) {
				pr2 = &m_roads[i].m_pt1;
				pc2 = &m_roads[i].m_pt2;
				index2 = i;
			} else if (pr3==nullptr) {
				pr3 = &m_roads[i].m_pt1;
				pc3 = &m_roads[i].m_pt2;
				index3 = i;
			}	else {
				pr4 = &m_roads[i].m_pt1;
				pc4 = &m_roads[i].m_pt2;
				index4 = i;
			}
		}
	}
	if (pr2 == nullptr || pr3 == nullptr || pr4==nullptr) {
		return;
	}

	Vector2 v1 = pr1->loc - loc;
	v1.Normalize();
	Vector2 v2 = pr2->loc - loc;
	v2.Normalize();
	Vector2 v3 = pr3->loc - loc;
	v3.Normalize();
	Vector2 v4 = pr4->loc - loc;
	v4.Normalize();

	Real dot12 = v1.Dot_Product(v1, v2);
	Real dot13 = v1.Dot_Product(v1, v3);
	Real dot14 = v1.Dot_Product(v1, v4);
	Real dot23 = v1.Dot_Product(v2, v3);
	Real dot24 = v1.Dot_Product(v2, v4);
	Real dot34 = v1.Dot_Product(v3, v4);
	// The most negative dot product is the pair that is heading most opposite each other.

	Int curPair = 12;
	Real curDot = dot12;
	if (dot13<curDot) {
		curPair = 13;
		curDot = dot13;
	}
	if (dot14<curDot) {
		curPair = 14;
		curDot = dot14;
	}
	if (dot23<curDot) {
		curPair = 23;
		curDot = dot23;
	}
	if (dot24<curDot) {
		curPair = 24;
		curDot = dot24;
	}
	if (dot34<curDot) {
		curPair = 34;
		curDot = dot34;
	}
	Bool do12 = (curPair==12);	 // Do segment 1 to 2
	Bool do13 = (curPair==13);	 // Do segment 1 to 3 etc.
	Bool do14 = (curPair==14);
	Bool do23 = (curPair==23);
	Bool do24 = (curPair==24);
	Bool do34 = (curPair==34);

	Vector2 alignVector;
	if (do12) {
		alignVector = v2-v1;
	}
	if (do13) {
		alignVector = v3-v1;
	}
	if (do14) {
		alignVector = v4-v1;
	}
	if (do23) {
		alignVector = v3-v2;
	}
	if (do24) {
		alignVector = v4-v2;
	}
	if (do34) {
		alignVector = v4-v3;
	}
	alignVector.Normalize();
	alignVector *= 0.5*scale; // we are offseting one half road width.


	if (do12) {
		offset4Way(pc1, pc2, pc3, pr3, pc4, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	if (do13) {
		offset4Way(pc1, pc3, pc2, pr2, pc4, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	if (do14) {
		offset4Way(pc1, pc4, pc3, pr3, pc2, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	if (do23) {
		offset4Way(pc2, pc3, pc1, pr1, pc4, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	if (do24) {
		offset4Way(pc2, pc4, pc1, pr1, pc3, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	if (do34) {
		offset4Way(pc3, pc4, pc1, pr1, pc2, loc, alignVector, m_roads[index1].m_widthInTexture);
	}
	// The offsets moved these ends: the index learns their new points.
	m_build->ends.addEnds(index1, m_roads[index1]);
	m_build->ends.addEnds(index2, m_roads[index2]);
	m_build->ends.addEnds(index3, m_roads[index3]);
	m_build->ends.addEnds(index4, m_roads[index4]);
	if (alignVector.X<0) {
		// Since the 4 way intersection is symmetrical, make it go right (pos x)
		alignVector.X = -alignVector.X;
		alignVector.Y = -alignVector.Y;
	}
	CHECK_SEGMENTS;
	m_roads[m_numRoads].m_pt1.loc.Set(loc);
	m_roads[m_numRoads].m_pt2.loc.Set(loc+alignVector);
	m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
	m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
	m_roads[m_numRoads].m_scale = m_roads[index1].m_scale;
	m_roads[m_numRoads].m_widthInTexture = TEE_WIDTH_ADJUSTMENT; // It is symmetrical.
	m_roads[m_numRoads].m_pt1.count = -4;
	m_roads[m_numRoads].m_type = FOUR_WAY;
	m_roads[m_numRoads].m_uniqueID = m_roads[index1].m_uniqueID;
	m_numRoads++;
	m_build->ends.addEnds(m_numRoads-1, m_roads[m_numRoads-1]);



}


//=============================================================================
// W3DRoadBuffer::roadsAtPointAfter
//=============================================================================
/** The segments after index1 with an end at loc, in array order (phases 3-4). Ends the offsets moved stay listed
under their old point; the callers compare the live locations, as the old walks did. */
//=============================================================================
void W3DRoadBuffer::roadsAtPointAfter(const Vector2 &loc, Int index1, std::vector<Int> &out)
{
	out.clear();
	m_build->ends.collect(loc, out);
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	std::vector<Int>::iterator first = std::upper_bound(out.begin(), out.end(), index1);
	out.erase(out.begin(), first);
}

//=============================================================================
// W3DRoadBuffer::insertTeeIntersections
//=============================================================================
/** Inserts Tee intersections at 3 way intersections. */
//=============================================================================
void W3DRoadBuffer::insertTeeIntersections()
{
	// Insert the tees.
	Int numRoadSegments = m_numRoads;
	Int i;
	// The tee scans ask which segments after one have an end at its junction point. The index answers from here on,
	// and learns every segment the tees append and every end the offsets move.
	m_build->ends.clear();
	for (i=0; i<numRoadSegments; i++) {
		m_build->ends.addEnds(i, m_roads[i]);
	}
//	Int segmentStartIndex = -1;
	for (i=0; i<numRoadSegments; i++) {
		if (m_roads[i].m_type != SEGMENT) {
			continue;
		}
		if (m_roads[i].m_pt1.count==2) {
			insertTee(m_roads[i].m_pt1.loc, i, m_roads[i].m_scale);
		}
		if (m_roads[i].m_pt2.count==2) {
			insertTee(m_roads[i].m_pt2.loc, i, m_roads[i].m_scale);
		}
		if (m_roads[i].m_pt1.count==3) {
			insert4Way(m_roads[i].m_pt1.loc, i, m_roads[i].m_scale);
		}
		if (m_roads[i].m_pt2.count==3) {
			insert4Way(m_roads[i].m_pt2.loc, i, m_roads[i].m_scale);
		}
	}
}

//=============================================================================
// W3DRoadBuffer::insertCurveSegments
//=============================================================================
/** Inserts curved segments along connected segments. */
//=============================================================================
void W3DRoadBuffer::insertCurveSegments()
{
	// Insert the curve segments.
	Int numRoadSegments = m_numRoads;
	Int i;
	Int segmentStartIndex = -1;
	for (i=0; i<numRoadSegments; i++) {
		if (i<numRoadSegments-1 && m_roads[i].m_pt1.loc == m_roads[i+1].m_pt2.loc) {
			if (m_roads[i+1].m_pt2.count==1 && m_roads[i].m_pt1.count==1) {
				insertCurveSegmentAt(i, i+1);
				if (segmentStartIndex < 0) {
					segmentStartIndex = i;
				}
			}
		}	else if (segmentStartIndex>=0) {
			if (m_roads[i].m_pt1.loc == m_roads[segmentStartIndex].m_pt2.loc) {
				if (m_roads[segmentStartIndex].m_pt2.count==1 && m_roads[i].m_pt1.count==1) {
					insertCurveSegmentAt(i, segmentStartIndex);
				}
			}
			segmentStartIndex = -1;
		}
	}
}
//=============================================================================
// W3DRoadBuffer::findCrossTypeJoinVector
//=============================================================================
/** Finds a road segment of different type && sets the join vector. */
//=============================================================================
Int W3DRoadBuffer::findCrossTypeJoinVector(Vector2 loc, Vector2 *joinVector, Int uniqueID)
{
	Vector2 newVector = *joinVector;
	// The old walk tested every segment's widened bounds in array order. The grid names the segments whose bounds can
	// hold loc; walked in ascending index (merged with the ones whose bounds are not finite) they give the same first hit.
	const RoadJoinGrid &grid = m_build->grid;
	const std::vector<Int> *cell = grid.at(loc);
	size_t a = 0, b = 0;
	while ((cell && a < cell->size()) || b < grid.always.size()) {
		Int i;
		if (cell && a < cell->size() && (b >= grid.always.size() || (*cell)[a] < grid.always[b])) {
			i = (*cell)[a++];
		} else {
			i = grid.always[b++];
		}
		// we only want different road types.
		if (m_roads[i].m_uniqueID == uniqueID) continue;
		// Only join to straight line segments.
		if (m_roads[i].m_type != SEGMENT) continue;
		Vector2 loc1, loc2;
		loc1 = m_roads[i].m_pt1.loc;
		loc2 = m_roads[i].m_pt2.loc;
		Region2D bounds;
		bounds.lo.x = loc1.X;
		bounds.lo.y = loc1.Y;
		bounds.hi = bounds.lo;
		if (bounds.lo.x > loc2.X) bounds.lo.x = loc2.X;
		if (bounds.lo.y > loc2.Y) bounds.lo.y = loc2.Y;
		if (bounds.hi.x < loc2.X) bounds.hi.x = loc2.X;
		if (bounds.hi.y < loc2.Y) bounds.hi.y = loc2.Y;
		bounds.lo.x -= m_roads[i].m_scale/2;
		bounds.lo.y -= m_roads[i].m_scale/2;
		bounds.hi.x += m_roads[i].m_scale/2;
		bounds.hi.y += m_roads[i].m_scale/2;
		if (loc.X >= bounds.lo.x && loc.Y >= bounds.lo.y && loc.X <= bounds.hi.x && loc.Y <= bounds.hi.y) {
			Vector3 v1 = Vector3(loc1.X, loc1.Y, 0);
			Vector3 v2 = Vector3(loc2.X, loc2.Y, 0);
			LineSegClass roadLine(v1,v2);
			Vector3 vLoc(loc.X, loc.Y, 0);
			Vector3 ptOnLine = roadLine.Find_Point_Closest_To(vLoc);
			Real dist = Vector3::Distance(ptOnLine, vLoc);
			if (dist < m_roads[i].m_scale*0.55f) {
				Vector2 roadVec = loc2-loc1;
				if (xpSign(roadVec, *joinVector) == 1) {
					roadVec.Rotate(PI/2);
				} else {
					roadVec.Rotate(-PI/2);
				}
				newVector = roadVec;
				*joinVector = newVector;
				return m_roads[i].m_uniqueID;
			}
		}

	}
	return 0;
}


//=============================================================================
// W3DRoadBuffer::adjustStacking
//=============================================================================
/** Adjusts the stacking order. */
//=============================================================================
void W3DRoadBuffer::adjustStacking(Int topUniqueID, Int bottomUniqueID)
{
	Int i, j;
	for (i=0; i<m_maxRoadTypes; i++) {
		if (m_roadTypes[i].getUniqueID() == topUniqueID) break;
	}
	DEBUG_ASSERTLOG(i<m_maxRoadTypes, ("***** Wrong unique id- john a should fix."));
	if (i>=m_maxRoadTypes) return;

	for (j=0; j<m_maxRoadTypes; j++) {
		if (m_roadTypes[j].getUniqueID() == bottomUniqueID) break;
	}
	DEBUG_ASSERTLOG(j<m_maxRoadTypes, ("***** Wrong unique id- john a should fix."));
	if (j>=m_maxRoadTypes) return;

	if (m_roadTypes[i].getStacking() > m_roadTypes[j].getStacking()) {
		return; // It's already on top.
	}
	Int newStacking = m_roadTypes[j].getStacking();
	for (j=0; j<m_maxRoadTypes; j++) {
		if (m_roadTypes[j].getStacking()>newStacking) {
			m_roadTypes[j].setStacking(m_roadTypes[j].getStacking()+1);
		}
	}
	m_roadTypes[i].setStacking(newStacking+1);

}


//=============================================================================
// W3DRoadBuffer::insertCrossTypeJoins
//=============================================================================
/** Inserts alpha blend type joins at open ends. */
//=============================================================================
void W3DRoadBuffer::insertCrossTypeJoins()
{
	// Insert the curve segments.
	Int numRoadSegments = m_numRoads;
	Int i;
	// The joins only append ALPHA_JOIN segments (never a candidate) and move tops and bottoms (never a bound), so the
	// grid built here is good for the whole pass.
	m_build->grid.build(m_roads, numRoadSegments);
	for (i=0; i<numRoadSegments; i++) {
		Vector2 loc1, loc2;
		Bool isPt1 = false;
		if ((m_roads[i].m_pt2.count==0 && m_roads[i].m_pt2.isJoin)) {
			loc1 = m_roads[i].m_pt2.loc;
			loc2 = m_roads[i].m_pt1.loc;

		} else if ((m_roads[i].m_pt1.count==0 && m_roads[i].m_pt1.isJoin)) {
			loc1 = m_roads[i].m_pt1.loc;
			loc2 = m_roads[i].m_pt2.loc;
			isPt1 = true;
		}	else {
			continue;
		}
		Vector2 joinVector(1, 0);


		Vector2 roadVector(loc2.X-loc1.X, loc2.Y-loc1.Y);
		roadVector.Normalize();
		joinVector = roadVector;
		Int otherID = findCrossTypeJoinVector(loc1, &joinVector, m_roads[i].m_uniqueID);
		if (!otherID ) {
			joinVector*=100;
		}
		Vector2 roadNormal(-roadVector.Y, roadVector.X);
		Vector2 joinNormal(-joinVector.Y, joinVector.X);


		Vector2 p1 = loc1 + roadNormal * m_roads[i].m_scale * m_roads[i].m_widthInTexture / 2;
		Vector2 p2 = loc2 + roadNormal * m_roads[i].m_scale * m_roads[i].m_widthInTexture / 2;

		Vector3 v1 = Vector3(p1.X, p1.Y, 0);
		Vector3 v2 = Vector3(p2.X, p2.Y, 0);
		LineSegClass roadLine(v1,v2);
		Vector3 vLoc1(loc1.X, loc1.Y, 0);
		v1 = Vector3(joinNormal.X, joinNormal.Y, 0)+vLoc1;
		LineSegClass joinLine(vLoc1,v1);
		Vector3 pInt1, pInt2;
		//Vector3 pInt3, pInt4;



		Real nu; // not used.
		Vector2 top = m_roads[i].m_pt1.top;
		if (joinLine.Find_Intersection(roadLine, &pInt1, &nu, &pInt2, &nu) ) {
			if (isPt1) {
				m_roads[i].m_pt1.top.Set(pInt1.X, pInt1.Y);
				top = m_roads[i].m_pt1.top;
			}	else {
				m_roads[i].m_pt2.bottom.Set(pInt1.X, pInt1.Y);
				top = m_roads[i].m_pt2.bottom;
			}
		}
		p1 = loc1 - roadNormal * m_roads[i].m_scale * m_roads[i].m_widthInTexture / 2;
		p2 = loc2 - roadNormal * m_roads[i].m_scale * m_roads[i].m_widthInTexture / 2;
		v1.Set(p1.X, p1.Y, 0);
		v2.Set(p2.X, p2.Y, 0);
		roadLine.Set(v1,v2);
		Vector2 bottom = m_roads[i].m_pt1.bottom;
		if (joinLine.Find_Intersection(roadLine, &pInt1, &nu, &pInt2, &nu) ) {
			if (isPt1) {
				m_roads[i].m_pt1.bottom.Set(pInt1.X, pInt1.Y);
				bottom = m_roads[i].m_pt1.bottom;
			}	else {
				m_roads[i].m_pt2.top.Set(pInt1.X, pInt1.Y);
				bottom = m_roads[i].m_pt2.top;
			}
		}

		bottom = bottom-top;
		Real scaleAdjustment = bottom.Length() / (m_roads[i].m_scale * m_roads[i].m_widthInTexture);
		if (otherID) {
			adjustStacking(m_roads[i].m_uniqueID, otherID);
		}
		CHECK_SEGMENTS;
		m_roads[m_numRoads].m_pt1.loc.Set(loc1);
		m_roads[m_numRoads].m_pt2.loc.Set(loc1+joinVector);
		m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_scale = m_roads[i].m_scale;
		m_roads[m_numRoads].m_widthInTexture = m_roads[i].m_scale * scaleAdjustment;
		m_roads[m_numRoads].m_pt1.count = 0;
		m_roads[m_numRoads].m_type = ALPHA_JOIN;
		m_roads[m_numRoads].m_uniqueID = m_roads[i].m_uniqueID;
		m_numRoads++;
	}
}


//=============================================================================
// W3DRoadBuffer::miter
//=============================================================================
/** Adjusts the end points to create a smooth miter join between road segments. */
//=============================================================================
void W3DRoadBuffer::miter(Int ndx1, Int ndx2)
{
	// adjust a mitered join.  jba.
	Vector3 p1 = Vector3(m_roads[ndx1].m_pt1.top.X, m_roads[ndx1].m_pt1.top.Y, 0);
	Vector3 p2 = Vector3(m_roads[ndx1].m_pt2.top.X, m_roads[ndx1].m_pt2.top.Y, 0);
	LineSegClass offsetLine1(p1, p2);
	p1 = Vector3(m_roads[ndx2].m_pt1.top.X, m_roads[ndx2].m_pt1.top.Y, 0);
	p2 = Vector3(m_roads[ndx2].m_pt2.top.X, m_roads[ndx2].m_pt2.top.Y, 0);
	LineSegClass offsetLine2(p1,p2);
	Vector3 pInt1, pInt2;
	Real nu; // not used.
	if (offsetLine1.Find_Intersection(offsetLine2, &pInt1, &nu, &pInt2, &nu) ) {
		m_roads[ndx2].m_pt2.top.X = pInt1.X;
		m_roads[ndx2].m_pt2.top.Y = pInt1.Y;
		m_roads[ndx1].m_pt1.top.X = pInt1.X;
		m_roads[ndx1].m_pt1.top.Y = pInt1.Y;
	}
	p1 = Vector3(m_roads[ndx1].m_pt1.bottom.X, m_roads[ndx1].m_pt1.bottom.Y, 0);
	p2 = Vector3(m_roads[ndx1].m_pt2.bottom.X, m_roads[ndx1].m_pt2.bottom.Y, 0);
	offsetLine1=LineSegClass (p1,p2);
	p1 = Vector3(m_roads[ndx2].m_pt1.bottom.X, m_roads[ndx2].m_pt1.bottom.Y, 0);
	p2 = Vector3(m_roads[ndx2].m_pt2.bottom.X, m_roads[ndx2].m_pt2.bottom.Y, 0);
	offsetLine2 = LineSegClass(p1,p2);
	if (offsetLine1.Find_Intersection(offsetLine2, &pInt1, &nu, &pInt2, &nu) ) {
		m_roads[ndx2].m_pt2.bottom.X = pInt1.X;
		m_roads[ndx2].m_pt2.bottom.Y = pInt1.Y;
		m_roads[ndx1].m_pt1.bottom.X = pInt1.X;
		m_roads[ndx1].m_pt1.bottom.Y = pInt1.Y;
	}
}

//=============================================================================
// W3DRoadBuffer::insertCurveSegmentAt
//=============================================================================
/** Inserts curves at the corner of 2 segments. */
//=============================================================================
void W3DRoadBuffer::insertCurveSegmentAt(Int ndx1, Int ndx2)
{
	const Real DOT_LIMIT = 0.5f;	// If the dot product of the new line is less than this, abort.
	Real radius = m_roads[ndx1].m_curveRadius*m_roads[ndx1].m_scale;
	Vector2 originalPt = m_roads[ndx1].m_pt1.loc;
	// we got a segment.
	LineSegClass line1(Vector3(m_roads[ndx1].m_pt1.loc.X, m_roads[ndx1].m_pt1.loc.Y, 0), Vector3(m_roads[ndx1].m_pt2.loc.X, m_roads[ndx1].m_pt2.loc.Y, 0));
	LineSegClass line2(Vector3(m_roads[ndx2].m_pt1.loc.X, m_roads[ndx2].m_pt1.loc.Y, 0), Vector3(m_roads[ndx2].m_pt2.loc.X, m_roads[ndx2].m_pt2.loc.Y, 0));
	Vector2 *pr1, *pr2, *pr3, *pr4;

	if (m_roads[ndx1].m_uniqueID != m_roads[ndx2].m_uniqueID) {
		return;
	}
	Real curSin = Vector3::Dot_Product(line1.Get_Dir(), line2.Get_Dir());
	Real xpdct = Vector3::Cross_Product_Z(line1.Get_Dir(), line2.Get_Dir());
	Bool turnRight;
	if (xpdct > 0) {
		pr1 = &m_roads[ndx1].m_pt1.loc;
		pr2 = &m_roads[ndx1].m_pt2.loc;
		pr3 = &m_roads[ndx2].m_pt1.loc;
		pr4 = &m_roads[ndx2].m_pt2.loc;
		turnRight = true;
	} else {
		pr4 = &m_roads[ndx1].m_pt1.loc;
		pr3 = &m_roads[ndx1].m_pt2.loc;
		pr2 = &m_roads[ndx2].m_pt1.loc;
		pr1 = &m_roads[ndx2].m_pt2.loc;
		turnRight = false;
 		line1.Set(Vector3(pr1->X, pr1->Y, 0), Vector3(pr2->X, pr2->Y, 0));
 		line2.Set(Vector3(pr3->X, pr3->Y, 0), Vector3(pr4->X, pr4->Y, 0));
	}
	Real angle = WWMath::Acos(curSin);
	Real count = angle / (PI/6.0f); // number of 30 degree steps.
	if (count<0.9 || m_roads[ndx1].m_pt1.isAngled) {
		miter(ndx1, ndx2);
		return;

	}

	Vector3 offset1(radius*line1.Get_Dir());
	Vector3 offset2(radius*line2.Get_Dir());
	offset1.Rotate_Z(-PI/2);
	offset2.Rotate_Z(-PI/2);

	Vector3 p1 = Vector3(pr1->X, pr1->Y, 0)+offset1;
	Vector3 p2 = Vector3(pr2->X, pr2->Y, 0)+offset1;
	LineSegClass offsetLine1(p1,p2);
	p1 = Vector3(pr3->X, pr3->Y, 0)+offset2;
	p2 = Vector3(pr4->X, pr4->Y, 0)+offset2;
	LineSegClass offsetLine2(p1,p2);
	Vector3 pInt1, pInt2;
	Vector3 pInt3, pInt4;
	Real nu; // not used.
	if (offsetLine1.Find_Intersection(offsetLine2, &pInt1, &nu, &pInt2, &nu) ) {
		m_roads[ndx2].m_pt2.last = true;
		LineSegClass cross1(pInt1, pInt1-offset2);
		LineSegClass cross2(pInt1, pInt1-offset1);
		cross1.Find_Intersection(line2, &pInt1, &nu, &pInt2, &nu);
		cross2.Find_Intersection(line1, &pInt3, &nu, &pInt4, &nu);
		// Make sure the lines didn't clip out of existence.
		Real theDot = Vector3::Dot_Product(line2.Get_Dir(), pInt1-Vector3(pr3->X, pr3->Y, 0));
		if (theDot < DOT_LIMIT) {
			*pr1 = originalPt;
			*pr4 = originalPt;
			miter(ndx1, ndx2);
			return;
		}
		theDot = Vector3::Dot_Product(line1.Get_Dir(), Vector3(pr2->X, pr2->Y, 0)-pInt3);
		if (theDot < DOT_LIMIT) {
			*pr1 = originalPt;
			*pr4 = originalPt;
			miter(ndx1, ndx2);
			return;
		}
		*pr4 = Vector2(pInt1.X, pInt1.Y);
		Real angle = -PI/6.0f; // -30 degrees.
		Vector2 pt2 = *pr4;
		Vector2 pt1 = *pr3;
		Vector2 direction(pt1.X-pt2.X, pt1.Y-pt2.Y);
		// offset = normal of the vector from pt1 to pt2.
		Vector2 centerOfCurve(-direction.Y,  direction.X);
		centerOfCurve.Normalize();
		centerOfCurve *= m_roads[ndx1].m_curveRadius*m_roads[ndx1].m_scale;
		centerOfCurve += pt2;

		rotateAbout(&pt2, centerOfCurve, angle);
		direction.Rotate(angle);
		pt1 = pt2+direction;

		// GeneralsX: Check for room BEFORE the first store, as every other insertion does: a
		// map whose road points fill MaxRoadSegments wrote this corner's curve one element past m_roads.
		CHECK_SEGMENTS;
		m_roads[m_numRoads].m_pt1.loc=pt2;
		m_roads[m_numRoads].m_pt2.loc=pt1;
		m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
		m_roads[m_numRoads].m_scale = m_roads[ndx1].m_scale;
		m_roads[m_numRoads].m_widthInTexture = m_roads[ndx1].m_widthInTexture;
		m_roads[m_numRoads].m_type = CURVE;
		m_roads[m_numRoads].m_curveRadius = m_roads[ndx1].m_curveRadius;
		m_roads[m_numRoads].m_uniqueID = m_roads[ndx1].m_uniqueID;
		m_numRoads++;
		if (count > 2.0) {
			Int i;
			for (i=2; i<count; i++) {
				// offset = normal of the vector from pt1 to pt2.
				direction.Rotate(angle);
				rotateAbout(&pt2, centerOfCurve, angle);
				pt1 = pt2+direction;
				CHECK_SEGMENTS;
				m_roads[m_numRoads].m_pt1.loc.Set(pt2);
				m_roads[m_numRoads].m_pt2.loc.Set(pt1);
				m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
				m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
				m_roads[m_numRoads].m_scale = m_roads[ndx1].m_scale;
				m_roads[m_numRoads].m_widthInTexture = m_roads[ndx1].m_widthInTexture;
				m_roads[m_numRoads].m_type = CURVE;
				m_roads[m_numRoads].m_curveRadius = m_roads[ndx1].m_curveRadius;
				m_roads[m_numRoads].m_uniqueID = m_roads[ndx1].m_uniqueID;
				m_numRoads++;
			}
		}

		*pr1 = Vector2(pInt3.X, pInt3.Y);

		m_roads[ndx1].m_pt1.last = true;
		if (count > 1.0) {
			pt2 = *pr1;
			pt1 = *pr1+*pr1-*pr2;
			direction.Set(pt1.X-pt2.X, pt1.Y-pt2.Y);
			pt1 = pt2+direction;
			CHECK_SEGMENTS;
			m_roads[m_numRoads].m_pt1.loc.Set(pt2);
			m_roads[m_numRoads].m_pt2.loc.Set(pt1);
			m_roads[m_numRoads].m_pt1.last = true; // if not, that one will clear flag in prior loop.
			m_roads[m_numRoads].m_pt2.last = true; // if not, that one will clear flag in prior loop.
			m_roads[m_numRoads].m_scale = m_roads[ndx1].m_scale;
			m_roads[m_numRoads].m_widthInTexture = m_roads[ndx1].m_widthInTexture;
			m_roads[m_numRoads].m_type = CURVE;
			m_roads[m_numRoads].m_curveRadius = m_roads[ndx1].m_curveRadius;
			m_roads[m_numRoads].m_uniqueID = m_roads[ndx1].m_uniqueID;
			m_numRoads++;
		}

		// Recalculate top & bottom.
 		Vector2 roadVector = m_roads[ndx1].m_pt2.loc - m_roads[ndx1].m_pt1.loc;
		Vector2 roadNormal(-roadVector.Y, roadVector.X);
		roadNormal.Normalize();
		roadNormal *= (m_roads[ndx1].m_scale*m_roads[ndx1].m_widthInTexture/2.0f);
		m_roads[ndx1].m_pt1.top = m_roads[ndx1].m_pt1.loc+roadNormal;
		m_roads[ndx1].m_pt1.bottom = m_roads[ndx1].m_pt1.loc - roadNormal;

 		roadVector = m_roads[ndx2].m_pt2.loc - m_roads[ndx2].m_pt1.loc;
		roadNormal = Vector2(-roadVector.Y, roadVector.X);
		roadNormal.Normalize();
		roadNormal *= (m_roads[ndx2].m_scale*m_roads[ndx2].m_widthInTexture/2.0f);
		m_roads[ndx2].m_pt2.top = m_roads[ndx2].m_pt2.loc+roadNormal;
		m_roads[ndx2].m_pt2.bottom = m_roads[ndx2].m_pt2.loc - roadNormal;

	}


}

//=============================================================================
// W3DRoadBuffer::rotateAbout
//=============================================================================
/** Rotates ptP about center. */
//=============================================================================
void W3DRoadBuffer::rotateAbout(Vector2 *ptP, Vector2 center, Real angle)
{
	Vector2 offset;
	offset.X = ptP->X - center.X;
	offset.Y = ptP->Y - center.Y;
	Vector2 orgOffset = offset;
	offset.Rotate(angle);
	offset.Y -= orgOffset.Y;
	offset.X -= orgOffset.X;
	*ptP += offset;
}

//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DRoadBuffer::~W3DRoadBuffer
//=============================================================================
/** Destructor. Releases w3d assets. */
//=============================================================================
W3DRoadBuffer::~W3DRoadBuffer()
{
	freeRoadBuffers();
	REF_PTR_RELEASE(m_map);
}

//=============================================================================
// W3DRoadBuffer::W3DRoadBuffer
//=============================================================================
/** Constructor.  */
//=============================================================================
W3DRoadBuffer::W3DRoadBuffer()	:
	m_roads(nullptr),
	m_numRoads(0),
	m_initialized(false),
	m_map(nullptr),
#ifdef LOAD_TEST_ASSETS
	m_maxUID(0),
#endif // LOAD_TEST_ASSETS
	m_lightsIterator(nullptr),
	m_maxRoadSegments(500),
	m_maxRoadTypes(8),
	m_maxRoadVertex(1000),
	m_maxRoadIndex(2000),
	m_curRoadType(0),
	m_curVertexCapacity(0),
	m_curIndexCapacity(0),
	m_mapRoadPairs(0),
	m_refusedRoadPairs(0),
	m_rebuildDropped(0),
	m_maxRebuildDropped(0),
	m_iniMaxRoadSegments(500),
	m_segmentsFull(false),
	m_build(nullptr)

{
	allocateRoadBuffers();
}


//=============================================================================
// W3DRoadBuffer::freeRoadBuffers
//=============================================================================
/** Frees the index and vertex buffers. */
//=============================================================================
void W3DRoadBuffer::freeRoadBuffers()
{
	delete[] m_roads;
	m_roads = nullptr;
	m_maxRoadSegments = 0;

	delete[] m_roadTypes;
	m_roadTypes = nullptr;
}

//=============================================================================
// W3DRoadBuffer::allocateRoadBuffers
//=============================================================================
/** Allocates the index and vertex buffers. */
//=============================================================================
void W3DRoadBuffer::allocateRoadBuffers()
{
	Int i = 0;
	const std::chrono::steady_clock::time_point allocStart = std::chrono::steady_clock::now();

	// save data for max limits
	// GeneralsX @feature The segment array is sized per map by loadRoads, at most MaxRoadSegments.
	m_iniMaxRoadSegments = TheGlobalData->m_maxRoadSegments;
	m_maxRoadSegments = 0;
	m_maxRoadVertex = TheGlobalData->m_maxRoadVertex;
	m_maxRoadIndex = TheGlobalData->m_maxRoadIndex;
	m_maxRoadTypes = TheGlobalData->m_maxRoadTypes;

#ifdef LOAD_TEST_ASSETS
	m_maxRoadTypes+=4;
#endif

	m_curNumRoadVertices=0;
	m_curNumRoadIndices=0;
	m_roads = nullptr;
	m_roadTypes = MSGNEW("RoadBuffer") RoadType[m_maxRoadTypes];

	// load roads from INI
	TerrainRoadType *road;
	i = 0;
	for( road = TheTerrainRoads->firstRoad(); road; road = TheTerrainRoads->nextRoad( road ) )
	{

		// get a path to the texture file
		if( i < m_maxRoadTypes )
		{
			Int id = road->getID();
			m_roadTypes[ i++ ].loadTexture( road->getTexture(), id );
#ifdef LOAD_TEST_ASSETS
			if( m_maxUID < id )
				m_maxUID = id;
#endif // LOAD_TEST_ASSETS

		}

	}

#ifdef LOAD_TEST_ASSETS
	while (i<m_maxRoadTypes && m_roadTypes[i].isAutoLoaded()) {
		m_roadTypes[i++].loadTestTexture();
	}
	m_curOpenRoad = i;
#endif

	// GeneralsX @feature What creating the road types and their buffers cost (once per terrain, not per map).
	Int typesWithBuffers = 0;
	for (Int t=0; t<m_maxRoadTypes; t++) {
		if (m_roadTypes[t].getVertexCapacity() > 0) {
			typesWithBuffers++;
		}
	}
	fprintf(stderr, "[GXROADS] alloc types_with_buffers=%d ms=%.1f\n", typesWithBuffers,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - allocStart).count());
	m_initialized = true;

}

//=============================================================================
// W3DRoadBuffer::clearAllRoads
//=============================================================================
/** Removes all roads. */
//=============================================================================
void W3DRoadBuffer::clearAllRoads()
{
	Int i;
	if (m_roads)
	for (i=0; i<m_numRoads; i++) {
		m_roads[i].SetIndexBuffer(nullptr,0);
		m_roads[i].SetVertexBuffer(nullptr,0);
	}
	m_numRoads = 0;
	if (m_roadTypes)
	for (i=0; i<m_maxRoadTypes; i++) {
		m_roadTypes[i].setStacking(0); // Reset stacking orders
		m_roadTypes[i].setNumVertices(0);
		m_roadTypes[i].setNumIndices(0);
	}
}
//=============================================================================
// W3DRoadBuffer::setMap
//=============================================================================
/** Sets the height map. */
//=============================================================================
void W3DRoadBuffer::setMap(WorldHeightMap *pMap)
{
	REF_PTR_SET(m_map, pMap);
}

//=============================================================================
// W3DRoadBuffer::loadRoads
//=============================================================================
/** Loads the roads from the map objects. */
//=============================================================================
void W3DRoadBuffer::loadRoads()
{
	if (!m_initialized || m_roadTypes == nullptr) {
		return;
	}
	// Free any existing road segments.
	clearAllRoads();
	//Int ticks = ::GetTickCount();
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	// GeneralsX @feature Size the segment array from the map's own roads (junctions, curves and joins add segments
	// to the point pairs). If that fills it, build again at MaxRoadSegments, so a map keeps every road a
	// MaxRoadSegments array would have kept.
	Int want = countMapRoadPairs()*SEGMENTS_PER_PAIR + 64;
	if (want < MIN_ROAD_SEGMENTS) {
		want = MIN_ROAD_SEGMENTS;
	}
	if (want > m_iniMaxRoadSegments || want < 0) {
		want = m_iniMaxRoadSegments;
	}
	sizeSegmentArray(want);
	buildRoadSegments();
	Bool rebuilt = false;
	if (m_segmentsFull && m_maxRoadSegments < m_iniMaxRoadSegments) {
		clearAllRoads();
		sizeSegmentArray(m_iniMaxRoadSegments);
		buildRoadSegments();
		rebuilt = true;
	}
	const std::chrono::steady_clock::time_point preloadStart = std::chrono::steady_clock::now();
	preloadRoadsInVertexAndIndexBuffers();
	const std::chrono::steady_clock::time_point preloadEnd = std::chrono::steady_clock::now();
	sizeRoadTypeBuffers();
	m_maxRebuildDropped = 0;
	m_updateBuffers = true;
	// GeneralsX @feature One line per load: how many roads the map has, how many fit, and what the buffers cost.
	Int typesUsed = 0;
	Int vbBytes = 0;
	Int ibBytes = 0;
	Int segHeapBytes = 0;
	for (Int i=0; i<m_numRoads; i++) {
		segHeapBytes += m_roads[i].GetNumVertex()*sizeof(VertexFormatXYZDUV1) + m_roads[i].GetNumIndex()*sizeof(UnsignedShort);
	}
	for (Int i=0; i<m_maxRoadTypes; i++) {
		if (m_roadTypes[i].getVertexCapacity() > 0) {
			typesUsed++;
			vbBytes += (m_roadTypes[i].getVertexCapacity()+4)*sizeof(VertexFormatXYZDUV1);
			ibBytes += (m_roadTypes[i].getIndexCapacity()+4)*sizeof(UnsignedShort);
		}
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	fprintf(stderr, "[GXROADS] load pairs=%d refused=%d segments=%d cap=%d max=%d full=%d rebuilt=%d types=%d vb_kib=%d ib_kib=%d "
		"seg_array_kib=%d seg_heap_kib=%d ms=%.1f\n",
		m_mapRoadPairs, m_refusedRoadPairs, m_numRoads, m_maxRoadSegments, m_iniMaxRoadSegments, m_segmentsFull ? 1 : 0,
		rebuilt ? 1 : 0,
		typesUsed, vbBytes/1024, ibBytes/1024, (Int)(m_maxRoadSegments*sizeof(RoadSegment)/1024), segHeapBytes/1024, ms);
	fprintf(stderr, "[GXROADS] phases build_ms=%.1f preload_ms=%.1f after_ms=%.1f\n",
		std::chrono::duration<double, std::milli>(preloadStart - start).count(),
		std::chrono::duration<double, std::milli>(preloadEnd - preloadStart).count(),
		ms - std::chrono::duration<double, std::milli>(preloadEnd - start).count());
	// What was built, in order: equal hashes on two builds mean they draw the same roads.
	UnsignedInt hash = 2166136261u;
	for (Int i=0; i<m_numRoads; i++) {
		const RoadSegment &r = m_roads[i];
		const Real f[6] = {r.m_pt1.loc.X, r.m_pt1.loc.Y, r.m_pt2.loc.X, r.m_pt2.loc.Y, r.m_scale, r.m_widthInTexture};
		const Int n[2] = {(Int)r.m_type, r.m_uniqueID};
		const unsigned char *b = (const unsigned char *)f;
		for (size_t k=0; k<sizeof(f); k++) {
			hash = (hash ^ b[k]) * 16777619u;
		}
		b = (const unsigned char *)n;
		for (size_t k=0; k<sizeof(n); k++) {
			hash = (hash ^ b[k]) * 16777619u;
		}
	}
	// And what they draw: every segment's vertex and index bytes, in order (equal = the same triangles on screen).
	UnsignedInt vtxHash = 2166136261u;
	Int vertices = 0;
	Int indices = 0;
	for (Int i=0; i<m_numRoads; i++) {
		const Int nv = m_roads[i].GetNumVertex();
		const Int ni = m_roads[i].GetNumIndex();
		const Int n[2] = {nv, ni};
		const unsigned char *b = (const unsigned char *)n;
		for (size_t k=0; k<sizeof(n); k++) {
			vtxHash = (vtxHash ^ b[k]) * 16777619u;
		}
		b = (const unsigned char *)m_roads[i].GetVertexData();
		for (size_t k=0; b && k<nv*sizeof(VertexFormatXYZDUV1); k++) {
			vtxHash = (vtxHash ^ b[k]) * 16777619u;
		}
		b = (const unsigned char *)m_roads[i].GetIndexData();
		for (size_t k=0; b && k<ni*sizeof(UnsignedShort); k++) {
			vtxHash = (vtxHash ^ b[k]) * 16777619u;
		}
		vertices += nv;
		indices += ni;
	}
	fprintf(stderr, "[GXROADS] built segments=%d hash=%08x vtx=%08x vertices=%d indices=%d\n", m_numRoads, hash, vtxHash,
		vertices, indices);
	//ticks = ::GetTickCount() - ticks;
	//char buf[256];
	//sprintf(buf, "%d road segs, %d milisec.\n", m_numRoads, ticks);
	//::OutputDebugString(buf);
}

//=============================================================================
// W3DRoadBuffer::buildRoadSegments
//=============================================================================
/** Builds the road segments from the map objects into m_roads. */
//=============================================================================
void W3DRoadBuffer::buildRoadSegments()
{
	m_segmentsFull = false;
	RoadBuildScratch scratch;
	m_build = &scratch;
	std::chrono::steady_clock::time_point t[6];
	t[0] = std::chrono::steady_clock::now();
	addMapObjects();
	t[1] = std::chrono::steady_clock::now();
	updateCountsAndFlags();
	t[2] = std::chrono::steady_clock::now();
	insertTeeIntersections();
	t[3] = std::chrono::steady_clock::now();
	insertCurveSegments();
	t[4] = std::chrono::steady_clock::now();
	insertCrossTypeJoins();
	t[5] = std::chrono::steady_clock::now();
	// Where the build's time goes, per phase (the first two walk every segment for every segment).
	fprintf(stderr, "[GXROADS] build segments=%d add_ms=%.1f counts_ms=%.1f tees_ms=%.1f curves_ms=%.1f joins_ms=%.1f\n", m_numRoads,
		std::chrono::duration<double, std::milli>(t[1] - t[0]).count(), std::chrono::duration<double, std::milli>(t[2] - t[1]).count(),
		std::chrono::duration<double, std::milli>(t[3] - t[2]).count(), std::chrono::duration<double, std::milli>(t[4] - t[3]).count(),
		std::chrono::duration<double, std::milli>(t[5] - t[4]).count());
	m_build = nullptr;
}

//=============================================================================
// W3DRoadBuffer::countMapRoadPairs
//=============================================================================
/** Road point pairs on the map, counted the way addMapObjects reads them. */
//=============================================================================
Int W3DRoadBuffer::countMapRoadPairs()
{
	Int pairs = 0;
	for (MapObject *pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext()) {
		if (pMapObj->getFlag(FLAG_ROAD_POINT1) && pMapObj->getNext() && pMapObj->getNext()->getFlag(FLAG_ROAD_POINT2)) {
			pairs++;
			pMapObj = pMapObj->getNext();
		}
	}
	return pairs;
}

//=============================================================================
// W3DRoadBuffer::sizeSegmentArray
//=============================================================================
/** Makes m_roads hold count segments. Call after clearAllRoads. */
//=============================================================================
void W3DRoadBuffer::sizeSegmentArray(Int count)
{
	if (count < 0) {
		count = 0;
	}
	if (count == m_maxRoadSegments && (m_roads != nullptr || count == 0)) {
		return;
	}
	delete[] m_roads;
	m_roads = count > 0 ? MSGNEW("RoadBuffer") RoadSegment[count] : nullptr;
	m_maxRoadSegments = count;
}

//=============================================================================
// W3DRoadBuffer::sizeRoadTypeBuffers
//=============================================================================
/** Sizes each road type's buffers to what its roads on this map can fill, at most MaxRoadVertex/Index;
a type with no roads gets none. */
//=============================================================================
void W3DRoadBuffer::sizeRoadTypeBuffers()
{
	// One pass over the segments (a dense map's are ~10 MB): a pass per road type cost ~0.2 s of load on one.
	std::vector<Int> numVertex(m_maxRoadTypes, 0);
	std::vector<Int> numIndex(m_maxRoadTypes, 0);
	Int i, j;
	for (j=0; j<m_numRoads; j++) {
		const Int uniqueID = m_roads[j].m_uniqueID;
		for (i=0; i<m_maxRoadTypes; i++) {
			if (m_roadTypes[i].getUniqueID() == uniqueID) {
				numVertex[i] += m_roads[j].GetNumVertex();
				numIndex[i] += m_roads[j].GetNumIndex();
			}
		}
	}
	for (i=0; i<m_maxRoadTypes; i++) {
		if (m_roadTypes[i].getUniqueID() < 0) {
			numVertex[i] = numIndex[i] = 0;
		}
		m_roadTypes[i].sizeBuffers(numVertex[i] < m_maxRoadVertex ? numVertex[i] : m_maxRoadVertex,
			numIndex[i] < m_maxRoadIndex ? numIndex[i] : m_maxRoadIndex);
	}
}

//=============================================================================
// W3DRoadBuffer::updateLighting
//=============================================================================
/** Draws the roads.  Uses terrain bounds to cull. */
//=============================================================================
void W3DRoadBuffer::updateLighting()
{
	/*
	CRASH FIX: Kris Morness
	When the player alt-tabs out of the game, m_roads is freed up, but when the other player
	places a structure in this area, the terrain gets flattened and calls this code, and BOOM!

	Submitted By:   	Lee, Pei
	Date Submitted: 	08/21/03 18:47:25
	Found:
	When playing a 2 player game both as USA-based armies, if one of the players uses Satellite Spy to reveal an area not yet reveal by either player then Alt-Tab's out, and then the enemy send a dozer over to build some structure in the previously revealed area, then the game will crash to desktop for the player who Alt-Tab'd.

	Steps to reproduce:
	- Play a 2 player network game with both player being USA.
		(Not sure if this will happen if the players are of different USA-based armies)
	- Have the victim reveal an area that is not yet reveal to either player.
	- Wait until the area has been shrouded again. (Not sure if required)
	- Have the victim Alt-Tab.
	- Have the remaining player then send a dozer to build a Cold Fusion Reactor in the area
		previously revealed by the other player's Spy Satellite.  (Not sure if it has to be Cold Fusion
		Reactor.)

	Result:
	As soon as the fence is set up, the player who Alt-tab'd would get Zero Hour crashing to desktop with Serious Error occurred.
	*/
	if( !m_roads )
	{
		return;
	}
	Int curRoad;
	// Do road segments.
	for (curRoad=0; curRoad<m_numRoads; curRoad++) {
		m_roads[curRoad].updateSegLighting();
	}
	m_updateBuffers = true;
}

//=============================================================================
// W3DRoadBuffer::updateCenter
//=============================================================================
/** Sets the flag to reload the vertex buffer. */
//=============================================================================
void W3DRoadBuffer::updateCenter()
{
	m_updateBuffers = true;
}

//=============================================================================
// W3DRoadBuffer::drawRoads
//=============================================================================
/** Draws the roads.   */
//=============================================================================
void W3DRoadBuffer::drawRoads(CameraClass * camera, TextureClass *cloudTexture, TextureClass *noiseTexture, Bool wireframe,
															Int minX, Int maxX, Int minY, Int maxY, RefRenderObjListIterator *pDynamicLightsIterator)
{
	IRegion2D bounds;
	bounds.lo.x = minX*MAP_XY_FACTOR;
	bounds.hi.x = maxX*MAP_XY_FACTOR;
	bounds.lo.y = minY*MAP_XY_FACTOR;
	bounds.hi.y = maxY*MAP_XY_FACTOR;

#define NO_TEST_CULL 1
#ifdef TEST_CULL
	bounds.lo.x += 32*MAP_XY_FACTOR;
	bounds.hi.x -= 32*MAP_XY_FACTOR;
#endif
#define noLOG_STATS
#ifdef LOG_STATS
	Int polys = 0;
#endif

	Int i;

	Int maxStacking = 0;
	for (i=0; i<m_maxRoadTypes; i++) {
		if (m_roadTypes[i].getStacking() > maxStacking) {
			maxStacking = m_roadTypes[i].getStacking();
		}
	}
	Int stacking;
	W3DShaderManager::ShaderTypes st=W3DShaderManager::ST_ROAD_BASE; //set default shader
	if (cloudTexture) {
		st=W3DShaderManager::ST_ROAD_BASE_NOISE1;
		if (noiseTexture)
			st=W3DShaderManager::ST_ROAD_BASE_NOISE12;
	}
	else
	if (noiseTexture)
		st=W3DShaderManager::ST_ROAD_BASE_NOISE2;

	Int devicePasses = 1;	//assume regular rendering
 	//Find number of passes required to render current shader
	devicePasses=W3DShaderManager::getShaderPasses(st);

	W3DShaderManager::setTexture(1,cloudTexture);	//cloud
	W3DShaderManager::setTexture(2,noiseTexture);	//noise/lightmap


	Bool loadBuffers = false;
	const std::chrono::steady_clock::time_point roadStart = std::chrono::steady_clock::now();
	Bool checked = m_updateBuffers;
	if (m_updateBuffers) {
		if (visibilityChanged(bounds)) {
			loadBuffers = true;
		}
	}
	m_updateBuffers = false;
	if (loadBuffers) {
		m_rebuildDropped = 0;
	}
	std::chrono::steady_clock::duration roadTime = std::chrono::steady_clock::now() - roadStart;

	for (stacking=0; stacking <= maxStacking; stacking++) {
		for (i=0; i<m_maxRoadTypes; i++) {
			if (stacking != m_roadTypes[i].getStacking()) {
				continue;
			}
			m_curUniqueID = m_roadTypes[i].getUniqueID();
			m_curRoadType = i;
			if (loadBuffers) {
				const std::chrono::steady_clock::time_point loadStart = std::chrono::steady_clock::now();
				loadRoadsInVertexAndIndexBuffers();
				roadTime += std::chrono::steady_clock::now() - loadStart;
			}
			if (m_roadTypes[i].getNumIndices() == 0) continue;
			if (wireframe) {
				m_roadTypes[i].applyTexture();
				DX8Wrapper::Set_Texture(0,nullptr);
			} else {
				m_roadTypes[i].applyTexture();
			}
	#ifdef RTS_DEBUG
			//DX8Wrapper::Set_Shader(detailShader); // shows clipping.
	#endif
			for (Int pass=0; pass < devicePasses; pass++)
			{
				if (!wireframe)
		 			W3DShaderManager::setShader(st, pass);
				//Draw all this road type.
				DX8Wrapper::Draw_Triangles(	0, m_roadTypes[i].getNumIndices()/3, 0,	m_roadTypes[i].getNumVertices());
#ifdef LOG_STATS
				polys += m_roadTypes[i].getNumIndices()/3;
#endif
			}

			if (!wireframe)	//shader was applied at least once?
 				W3DShaderManager::resetShader(st);
		}
	}
#ifdef LOG_STATS
	if (loadBuffers) {
		DEBUG_LOG(("Road poly count %d", polys));
	}
#endif
	// GeneralsX @feature Once a second while roads are being rebuilt: how often, the CPU it cost (visibility test plus
	// buffer fills), and the visible segments the vertex/index limits left undrawn.
	static std::chrono::steady_clock::time_point s_secStart = std::chrono::steady_clock::now();
	static Int s_checks = 0, s_rebuilds = 0, s_dropRebuilds = 0, s_dropMax = 0;
	static double s_us = 0.0;
	if (checked) {
		s_checks++;
		s_us += std::chrono::duration<double, std::micro>(roadTime).count();
	}
	if (loadBuffers) {
		s_rebuilds++;
		if (m_rebuildDropped > 0) {
			s_dropRebuilds++;
			if (m_rebuildDropped > s_dropMax) {
				s_dropMax = m_rebuildDropped;
			}
			if (m_rebuildDropped > m_maxRebuildDropped) {
				m_maxRebuildDropped = m_rebuildDropped;
				DEBUG_LOG(("Road segments not drawn (vertex/index limit): %d", m_rebuildDropped));
			}
		}
	}
	const std::chrono::steady_clock::time_point roadNow = std::chrono::steady_clock::now();
	if (roadNow - s_secStart >= std::chrono::seconds(1)) {
		if (s_checks > 0) {
			fprintf(stderr, "[GXROADS] per_s checks=%d rebuilds=%d cpu_ms=%.2f drop_rebuilds=%d drop_max=%d segments=%d\n",
				s_checks, s_rebuilds, s_us/1000.0, s_dropRebuilds, s_dropMax, m_numRoads);
		}
		s_secStart = roadNow;
		s_checks = s_rebuilds = s_dropRebuilds = s_dropMax = 0;
		s_us = 0.0;
	}

#if 0
	// Need to use a separate set of index & vertex buffers for this.  jba.
	DX8Wrapper::Set_Index_Buffer(nullptr,0);
	DX8Wrapper::Set_Vertex_Buffer(nullptr);
	if (pDynamicLightsIterator) {
		for (i=0; i<m_maxRoadTypes; i++) {
			m_curRoadType = i;
			m_curUniqueID = m_roadTypes[i].getUniqueID();
			if (m_curUniqueID < 0 || m_curUniqueID >= m_maxRoadTypes) continue;
			loadLitRoadsInVertexAndIndexBuffers(pDynamicLightsIterator);
			if (this->m_curNumRoadIndices == 0) continue;
			if (wireframe) {
					DX8Wrapper::Set_Texture(0,nullptr);
			} else {
				m_roadTypes[i].applyTexture();
				if (cloudTexture) {
					DX8Wrapper::Set_Texture(1,cloudTexture);
				}
			}
			DX8Wrapper::Set_Shader(detailAlphaShader);
			//Draw all the roads.
			DX8Wrapper::Draw_Triangles(	0, m_curNumRoadIndices/3, 0,	m_curNumRoadVertices);
		}
	}
#endif
	m_curRoadType = 0;
}


