/*
**	Command & Conquer Generals(tm)
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

// FILE: W3DBridgeBuffer.cpp ////////////////////////////////////////////////
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
// File name: W3DBridgeBuffer.cpp
//
// Created:   John Ahlquist, May 2001
//
// Desc:      Draw buffer to handle all the bridges in a scene.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------

#include "W3DDevice/GameClient/W3DBridgeBuffer.h"

#include "W3DDevice/GameClient/W3DAssetManager.h"
#include <WW3D2/texture.h>
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/GameLogic.h"	// liveGameWithAnotherHuman: GameLogic::gxCheatsAllowed
#include "Common/RandomValue.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameClient/TerrainRoads.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameLogic/Damage.h"
#include "GameLogic/Module/BodyModule.h"
#include "W3DDevice/GameLogic/W3DTerrainLogic.h"
#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/Module/W3DModelDraw.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/scene.h"


//-----------------------------------------------------------------------------
//         Private Data
//-----------------------------------------------------------------------------
// A W3D shader that does alpha, texturing, tests zbuffer, doesn't update zbuffer.
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailAlphaShader(SC_ALPHA_DETAIL);


#define SC_ALPHA_MIRROR ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_ONE, \
	ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailShader(SC_ALPHA_MIRROR);

#define NO_USE_BRIDGE_NORMALS

//-----------------------------------------------------------------------------
//         Private Classes
//-----------------------------------------------------------------------------
//=============================================================================
// W3DBridge constructor.
//=============================================================================
/** Initializes pointers & values.  */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
W3DBridge::W3DBridge() :
m_bridgeTexture(nullptr),
m_leftMesh(nullptr),
m_sectionMesh(nullptr),
m_rightMesh(nullptr),
m_visible(false),
m_curDamageState(BODY_PRISTINE),
m_scale(1.0)
{
}
// GX-SIMID-END

//=============================================================================
// W3DBridge destructor.
//=============================================================================
/** Frees objects.  */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
W3DBridge::~W3DBridge()
{
	clearBridge();
}
// GX-SIMID-END

//=============================================================================
// W3DBridge::renderBridge
//=============================================================================
/** Renders the bride.  It is assumed that the shared vertex and index buffers
are already set.  */
//=============================================================================
void W3DBridge::renderBridge(Bool wireframe)
{
	if (m_visible && m_numPolygons && m_numVertex) {
		if (!wireframe) DX8Wrapper::Set_Texture(0,m_bridgeTexture);
		// Draw all the bridges.
		DX8Wrapper::Draw_Triangles(	m_firstIndex, m_numPolygons, m_firstVertex,	m_numVertex);
	}
}

//=============================================================================
// W3DBridge::clearBridge
//=============================================================================
/** Frees all bridge objects (meshes & texture).  */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DBridge::clearBridge()
{
	m_visible = false;
	REF_PTR_RELEASE(m_bridgeTexture);
	REF_PTR_RELEASE(m_leftMesh);
	REF_PTR_RELEASE(m_sectionMesh);
	REF_PTR_RELEASE(m_rightMesh);
}
// GX-SIMID-END

//=============================================================================
// W3DBridge::cullBridge
//=============================================================================
/** Culls bridge to camera.  */
//=============================================================================
Bool W3DBridge::cullBridge(CameraClass * camera)
{
	///@todo - cull bridges.
	Bool wasVisible = m_visible;

	m_visible = true;

	return(wasVisible != m_visible);

}

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
#define BRIDGE_FLOAT_AMT (0.25f)
// GX-SIMID-END

// GeneralsX: load()'s own stated fallbacks, hoisted so init() can use
// them. They are the numbers the "should these be defaults in INI??? CBD" comment in load() refers
// to; a bridge whose model never arrives now describes itself with these instead of with whatever
// the previous occupant of its array slot left behind.
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
#define BRIDGE_DEFAULT_SCALE  (0.7f)
#define BRIDGE_DEFAULT_WIDTH  (34.0f)
#define BRIDGE_DEFAULT_LENGTH (170.0f)

//=============================================================================
// W3DBridge::init
//=============================================================================
/** Inits a bridges location & type so it can be load'ed.  */
//=============================================================================
void W3DBridge::init(Vector3 fromLoc, Vector3 toLoc, AsciiString bridgeTemplateName)
{
	m_start = fromLoc;
	m_end = toLoc;
	m_templateName = bridgeTemplateName;
	m_enabled = true;

	// GeneralsX: A SLOT MUST DESCRIBE ITSELF BEFORE load() RUNS.
	//
	// addBridge now hands this bridge to the LOGIC whether or not load() finds a W3D model, so
	// getBridgeInfo() can be asked about a slot whose meshes never arrived. Besides m_start/m_end
	// these three fields are everything getBridgeInfo() reads, and m_bridges is a reused array -
	// leaving them would read out whichever bridge sat in this slot for the previous map.
	//
	// The footprint these produce is NOT the real model's: measured on KandaharBridge the model
	// gives minY=-70.33, maxY=71.86, scale=1. A peer that cannot load the model therefore gets a
	// different bridge FOOTPRINT (m_bounds, and so the pathfind layer) from one that can - which
	// is exactly why such a peer is refused a network game. What is now identical either way is
	// the set of simulation objects and their transforms, which is what the CRC hashes.
	m_bridgeType = FIXED_BRIDGE;
	m_scale = BRIDGE_DEFAULT_SCALE;
	m_maxY = BRIDGE_DEFAULT_WIDTH * 0.5f;
	m_minY = -m_maxY;
	m_length = BRIDGE_DEFAULT_LENGTH;
}
// GX-SIMID-END

//=============================================================================
// W3DBridge::init
//=============================================================================
/** Loads a bridge model(if not already loaded) and gets meshes for use at
specified location.  */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
Bool W3DBridge::load(BodyDamageType curDamageState)
{
	REF_PTR_RELEASE(m_bridgeTexture);
	REF_PTR_RELEASE(m_leftMesh);
	REF_PTR_RELEASE(m_sectionMesh);
	REF_PTR_RELEASE(m_rightMesh);

	Real scale, width, length;
	char textureFile[_MAX_PATH] = "No Texture";
	char modelName[_MAX_PATH] = "BRIDGESECTIONAL";

	/// @todo, should these be defaults in INI??? CBD
	scale = 0.7f;
	width = 34;
	length = 170;

	// try to find bridge in INI
	TerrainRoadType *bridge = TheTerrainRoads->findBridge( m_templateName );
	if (!bridge) return false;

	scale = bridge->getBridgeScale();
	switch (curDamageState) {
		default: return false;

		case 	BODY_PRISTINE:
			strlcpy(textureFile, bridge->getTexture().str(), ARRAY_SIZE(textureFile));
			strlcpy(modelName, bridge->getBridgeModel().str(), ARRAY_SIZE(modelName));
			break;
		case BODY_DAMAGED:
			strlcpy(textureFile, bridge->getTextureDamaged().str(), ARRAY_SIZE(textureFile));
			strlcpy(modelName, bridge->getBridgeModelNameDamaged().str(), ARRAY_SIZE(modelName));
			break;
		case BODY_REALLYDAMAGED:
			strlcpy(textureFile, bridge->getTextureReallyDamaged().str(), ARRAY_SIZE(textureFile));
			strlcpy(modelName, bridge->getBridgeModelNameReallyDamaged().str(), ARRAY_SIZE(modelName));
			break;
		case BODY_RUBBLE:
			strlcpy(textureFile, bridge->getTextureBroken().str(), ARRAY_SIZE(textureFile));
			strlcpy(modelName, bridge->getBridgeModelNameBroken().str(), ARRAY_SIZE(modelName));
			break;
	}

	WW3DAssetManager *pMgr = W3DAssetManager::Get_Instance();
	char left[_MAX_PATH];
	char section[_MAX_PATH];
	char right[_MAX_PATH];

	static_assert(ARRAY_SIZE(left) >= ARRAY_SIZE(section), "Incorrect array size");
	static_assert(ARRAY_SIZE(section) >= ARRAY_SIZE(modelName), "Incorrect array size");
	static_assert(ARRAY_SIZE(right) >= ARRAY_SIZE(modelName), "Incorrect array size");
	strcpy(left, modelName);
	strlcat(left, ".BRIDGE_LEFT", ARRAY_SIZE(left));
	strcpy(section, modelName);
	strlcat(section, ".BRIDGE_SPAN", ARRAY_SIZE(section));
	strcpy(right, modelName);
	strlcat(right, ".BRIDGE_RIGHT", ARRAY_SIZE(right));

	m_bridgeTexture = pMgr->Get_Texture(textureFile,  MIP_LEVELS_3);
	m_leftMtx.Make_Identity();
	m_rightMtx.Make_Identity();
	m_sectionMtx.Make_Identity();

	RenderObjClass *pObj = pMgr->Create_Render_Obj(modelName );
	if (!pObj) return false;
	Int i;
	for (i=0; i<pObj->Get_Num_Sub_Objects(); i++) {
		RenderObjClass *pSub = pObj->Get_Sub_Object(i);
		const Matrix3D& mtx = pSub->Get_Transform();
		if (0==strnicmp(left, pSub->Get_Name(), strlen(left))) {
			m_leftMtx = mtx;
			strlcpy(left, pSub->Get_Name(), ARRAY_SIZE(left));
		}
		if (0==strnicmp(section, pSub->Get_Name(), strlen(section))) {
			m_sectionMtx = mtx;
			strlcpy(section, pSub->Get_Name(), ARRAY_SIZE(section));
		}
		if (0==strnicmp(right, pSub->Get_Name(), strlen(right))) {
			m_rightMtx = mtx;
			strlcpy(right, pSub->Get_Name(), ARRAY_SIZE(right));
		}
		REF_PTR_RELEASE(pSub);
		//DEBUG_LOG(("Sub obj name %s", pSub->Get_Name()));
	}

	REF_PTR_RELEASE(pObj);

	m_leftMesh = (MeshClass*)pMgr->Create_Render_Obj(left );
	m_sectionMesh = (MeshClass*)pMgr->Create_Render_Obj(section);
	m_rightMesh = (MeshClass*)pMgr->Create_Render_Obj(right);
	m_scale = scale;


	if (m_leftMesh == nullptr) {
		clearBridge();
		return(false);
	}
	m_bridgeType = SECTIONAL_BRIDGE;

	if (m_rightMesh == nullptr || m_sectionMesh == nullptr) {
		m_bridgeType = FIXED_BRIDGE;
	}

	Int numVertex = m_leftMesh->Peek_Model()->Get_Vertex_Count();
	Vector3 *pVert = m_leftMesh->Peek_Model()->Get_Vertex_Array();
	m_leftMinX = FLT_MAX;
	m_leftMaxX = -FLT_MAX;
	m_minY = FLT_MAX;
	m_maxY = -FLT_MAX;
	for (i=0; i<numVertex; i++) {
		Vector3 vert;
		Matrix3D::Transform_Vector(m_leftMtx, pVert[i], &vert);
		if (m_leftMinX > vert.X) m_leftMinX = vert.X;
		if (m_minY > vert.Y) m_minY = vert.Y;
		if (vert.X > m_leftMaxX) m_leftMaxX = vert.X;
		if (vert.Y > m_maxY) m_maxY = vert.Y;	 // Note - we assume all sections are the same width, so we only do maxY for first section.
	}
	if (m_bridgeType == SECTIONAL_BRIDGE) {
		numVertex = m_sectionMesh->Peek_Model()->Get_Vertex_Count();
		pVert = m_sectionMesh->Peek_Model()->Get_Vertex_Array();
		m_sectionMinX = FLT_MAX;
		m_sectionMaxX = -FLT_MAX;
		for (i=0; i<numVertex; i++) {
			Vector3 vert;
			Matrix3D::Transform_Vector(m_sectionMtx, pVert[i], &vert);
			if (m_sectionMinX > vert.X) m_sectionMinX = vert.X;
			if (vert.X > m_sectionMaxX) m_sectionMaxX = vert.X;
		}

		numVertex = m_rightMesh->Peek_Model()->Get_Vertex_Count();
		pVert = m_rightMesh->Peek_Model()->Get_Vertex_Array();
		m_rightMinX = FLT_MAX;
		m_rightMaxX = -FLT_MAX;
		for (i=0; i<numVertex; i++) {
			Vector3 vert;
			Matrix3D::Transform_Vector(m_rightMtx, pVert[i], &vert);
			if (m_rightMinX > vert.X) m_rightMinX = vert.X;
			if (vert.X > m_rightMaxX) m_rightMaxX = vert.X;
		}
	} else {
		m_sectionMinX = m_leftMaxX;
		m_sectionMaxX = m_leftMaxX;
		m_rightMinX = m_leftMaxX;
		m_rightMaxX = m_leftMaxX;
	}
	length = m_rightMaxX - m_leftMinX;
	if (length < 1) length = 1;
	m_length = length;
	if (m_bridgeType == SECTIONAL_BRIDGE) {
		Real allowableError = 0.05f*length;
		// make sure the sections align.

		if (m_leftMaxX>m_sectionMinX+allowableError) {
			m_bridgeType = FIXED_BRIDGE;
		}

		if (m_rightMinX<m_sectionMaxX-allowableError) {
			m_bridgeType = FIXED_BRIDGE;
		}

	}
	return(true);
}
// GX-SIMID-END


//=============================================================================
// W3DBridge::getBridgeInfo
//=============================================================================
/** Gets the location info for the bridge.  */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DBridge::getBridgeInfo(BridgeInfo *pInfo)
{

	pInfo->from.x = m_start.X;
	pInfo->from.y = m_start.Y;
	pInfo->from.z = m_start.Z;
	pInfo->to.x = m_end.X;
	pInfo->to.y = m_end.Y;
	pInfo->to.z = m_end.Z;
	pInfo->bridgeWidth = (m_maxY - m_minY) *m_scale;

	Vector3 vec = 	m_end-m_start;
	Vector3 vecNormal(-vec.Y, vec.X, 0);
	vecNormal.Normalize();

	// From left = from + vecNormal*maxY*scale
	pInfo->fromLeft.x = m_start.X + vecNormal.X * m_maxY * m_scale;
	pInfo->fromLeft.y = m_start.Y + vecNormal.Y * m_maxY * m_scale;
	pInfo->fromLeft.z = m_start.Z + vecNormal.Z * m_maxY * m_scale;

	// From right = from + vecNormal*minY*scale
	pInfo->fromRight.x = m_start.X + vecNormal.X * m_minY * m_scale;
	pInfo->fromRight.y = m_start.Y + vecNormal.Y * m_minY * m_scale;
	pInfo->fromRight.z = m_start.Z + vecNormal.Z * m_minY * m_scale;

	// to left = to + vecNormal*maxY*scale
	pInfo->toLeft.x = m_end.X + vecNormal.X * m_maxY * m_scale;
	pInfo->toLeft.y = m_end.Y + vecNormal.Y * m_maxY * m_scale;
	pInfo->toLeft.z = m_end.Z + vecNormal.Z * m_maxY * m_scale;

	// to right = to + vecNormal*minY*scale
	pInfo->toRight.x = m_end.X + vecNormal.X * m_minY * m_scale;
	pInfo->toRight.y = m_end.Y + vecNormal.Y * m_minY * m_scale;
	pInfo->toRight.z = m_end.Z + vecNormal.Z * m_minY * m_scale;

}
// GX-SIMID-END



//=============================================================================
// W3DBridge::getModelVertices
//=============================================================================
/** Gets the vertex values for a section of a bridge.  */
//=============================================================================
Int W3DBridge::getModelVertices(VertexFormatXYZNDUV1 *destination_vb, Int curVertex, Real xOffset,
																Vector3 &vec, Vector3 &vecNormal, Vector3 &vecZ, Vector3 &offset,
																const Matrix3D &mtx,
																MeshClass *pMesh, RefRenderObjListIterator *pLightsIterator)
{
	if (pMesh == nullptr)
		return(0);

	Int i;
	Int numVertex = pMesh->Peek_Model()->Get_Vertex_Count();
	Vector3 *pVert = pMesh->Peek_Model()->Get_Vertex_Array();

	const Vector3 *pNormal = 	pMesh->Peek_Model()->Get_Vertex_Normal_Array();

	// If we happen to have too many bridges, stop.
	if (curVertex+numVertex+2>= W3DBridgeBuffer::MAX_BRIDGE_VERTEX) {
		return(0);
	}

	Vector3 lightRay[MAX_GLOBAL_LIGHTS];
	const Coord3D *lightPos;

	for (Int lightIndex=0; lightIndex < TheGlobalData->m_numGlobalLights; lightIndex++)
	{
		lightPos=&TheGlobalData->m_terrainLightPos[lightIndex];
		lightRay[lightIndex].Set(-lightPos->x,-lightPos->y,	-lightPos->z);
//		__asm {int 3}; //see if it really needs normalization!!
		lightRay[lightIndex].Normalize();
	}

	const Vector2*uvs=pMesh->Peek_Model()->Get_UV_Array_By_Index(0);
	VertexFormatXYZNDUV1 *curVb = destination_vb+curVertex;

	for (i=0; i<numVertex; i++) {
		curVb->u1 = uvs[i].U;
		curVb->v1 = uvs[i].V;
		Vector3 vLoc;
		Vector3 vertex;
		Matrix3D::Transform_Vector(mtx, pVert[i], &vertex);
		vLoc = (vertex.X+xOffset) * vec + vertex.Y*vecNormal + vertex.Z*vecZ;

		vLoc.X += m_start.X;
		vLoc.Y += m_start.Y;
		vLoc.Z += m_start.Z;

		curVb->x = vLoc.X;
		curVb->y = vLoc.Y;
		curVb->z = vLoc.Z;

		VERTEX_FORMAT vb;
		vb.x = vLoc.X;
		vb.y = vLoc.Y;
		vb.z = vLoc.Z;

		Vector3 normal;
		Matrix3D::Rotate_Vector(mtx, pNormal[i], &normal);
		curVb->diffuse = 0xFF000000;
#ifdef USE_BRIDGE_NORMALS
		curVb->nx = normal.X;
		curVb->ny = normal.Y;
		curVb->nz = normal.Z;
#else
		normal = (normal.X) * vec + normal.Y*vecNormal + normal.Z*vecZ;
		normal.Normalize();
		TheTerrainRenderObject->doTheLight(&vb, lightRay, &normal, nullptr, 1.0f);
		curVb->diffuse = vb.diffuse | 0xFF000000;
#endif
		curVb++;
	}
	return(numVertex);
}

//=============================================================================
// W3DBridge::getModelVerticesFixed
//=============================================================================
/** Gets the vertex values for a section of a fixed bridge.  */
//=============================================================================
Int W3DBridge::getModelVerticesFixed(VertexFormatXYZNDUV1 *destination_vb, Int curVertex,
																const Matrix3D &mtx, MeshClass *pMesh, RefRenderObjListIterator *pLightsIterator)
{
	if (pMesh == nullptr)
		return(0);

	Vector3 vec = m_end - m_start;
	if (vec.Length2() < 1.0f) {
		vec.Normalize();
	}
	Vector3 vecNormal(-vec.Y, vec.X, 0);
	vecNormal.Normalize();
	Real deltaZ = m_end.Z - m_start.Z;
	deltaZ /= vec.Length();
	Real deltaX = sqrt(1.0 - deltaZ*deltaZ);
	Vector3 vecZ(-deltaZ, 0, deltaX);
	vec /= m_length;
	vecNormal *= m_scale;
	vecZ *= m_scale;
	Real xOffset = -m_leftMinX;
	return(getModelVertices(destination_vb, curVertex, xOffset, vec, vecNormal, vecZ, m_start, mtx, pMesh, pLightsIterator));
}

//=============================================================================
// W3DBridge::getIndicesNVertices
//=============================================================================
/** Gets the index values and vertex values for a bridge.  */
//=============================================================================
void W3DBridge::getIndicesNVertices(UnsignedShort *destination_ib, VertexFormatXYZNDUV1 *destination_vb,
																		Int *curIndexP, Int *curVertexP, RefRenderObjListIterator *pLightsIterator)
{
	Int numI;
	Int numV;
	m_firstVertex = *curVertexP;
	m_firstIndex = *curIndexP;
	m_numVertex = 0;
	m_numPolygons = 0;
	// GeneralsX: A bridge with no model draws nothing, and says nothing.
	//
	// addBridge keeps a slot for a bridge whose model did not load (its logic object must exist on
	// every peer), and load() leaves all three meshes null whenever the left one is missing. Past
	// this point the helpers return 0 for a null mesh, which the checks below read as a vertex or
	// index OVERFLOW - a false DEBUG_ASSERTCRASH per artless bridge per buffer rebuild in a debug
	// build. Zero polygons is already what renderBridge skips.
	if (m_leftMesh == nullptr) {
		return;
	}
	if (m_sectionMesh == nullptr) {
		numV = getModelVerticesFixed(destination_vb, *curVertexP, m_leftMtx, m_leftMesh, pLightsIterator);
		if (!numV)
		{	//not enough room for vertices
			DEBUG_ASSERTCRASH( numV, ("W3DBridge::GetIndicesNVertices(). Vertex overflow.") );
			return;
		}
		numI = getModelIndices( destination_ib, *curIndexP, *curVertexP, m_leftMesh);
		if (!numI)
		{	//not enough room for indices
			DEBUG_ASSERTCRASH( numI, ("W3DBridge::GetIndicesNVertices(). Index overflow.") );
			return;
		}
		*curIndexP += numI;
		*curVertexP += numV;
		m_numVertex += numV;
		m_numPolygons += numI/3;
		return;
	}

	Vector3 vec = m_end - m_start;
	if (vec.Length2() < 1.0f) {
		vec.Normalize();
	}

	Vector3 vecNormal(-vec.Y, vec.X, 0);
	vecNormal.Normalize();
	vecNormal *= m_scale;

	// Rotate along the y axis to get the appropriate Z height adjustment.
	Real deltaZ = m_end.Z - m_start.Z;
	Real desiredLength = vec.Length();
	deltaZ /= desiredLength;
	Real deltaX = sqrt(1.0 - deltaZ*deltaZ);
	Vector3 vecZ(-deltaZ, 0, deltaX);
	vecZ *= m_scale;

	Real spanLength = m_rightMinX - m_leftMaxX;
	Int numSpans = 1;
	if (m_bridgeType != FIXED_BRIDGE) {
		Real spannable = desiredLength - (m_length-spanLength);
		numSpans = REAL_TO_INT_FLOOR( (spannable + spanLength/2)/spanLength);
		if (numSpans<0) numSpans = 0;
	}

	Real bridgeLength = m_length + (numSpans-1)*spanLength;
	Real xOffset = -m_leftMinX;

	// Draw the left end.
	vec /= bridgeLength;
	numV = getModelVertices(destination_vb, *curVertexP, xOffset, vec, vecNormal, vecZ, m_start,
		m_leftMtx, m_leftMesh, pLightsIterator);
	if (!numV)
	{	//not enough room for vertices
		DEBUG_ASSERTCRASH( numV, ("W3DBridge::GetIndicesNVertices(). Vertex overflow.") );
		return;
	}
	numI = getModelIndices( destination_ib, *curIndexP, *curVertexP, m_leftMesh);
	if (!numI)
	{	//not enough room for indices
		DEBUG_ASSERTCRASH( numI, ("W3DBridge::GetIndicesNVertices(). Index overflow.") );
		return;
	}
	*curIndexP += numI;
	*curVertexP += numV;
	m_numVertex += numV;
	m_numPolygons += numI/3;

	Int i;
	// draw the spans.
	for (i=0; i<numSpans; i++) {
		numV = getModelVertices(destination_vb, *curVertexP, xOffset+i*spanLength, vec, vecNormal, vecZ, m_start,
			m_sectionMtx, m_sectionMesh, pLightsIterator);
		if (!numV)
		{	//not enough room for vertices
			DEBUG_ASSERTCRASH( numV, ("W3DBridge::GetIndicesNVertices(). Vertex overflow.") );
			return;
		}
		numI = getModelIndices( destination_ib, *curIndexP, *curVertexP, m_sectionMesh);
		if (!numI)
		{	//not enough room for indices
			DEBUG_ASSERTCRASH( numI, ("W3DBridge::GetIndicesNVertices(). Index overflow.") );
			return;
		}
		*curIndexP += numI;
		*curVertexP += numV;
		m_numVertex += numV;
		m_numPolygons += numI/3;
	}

	// Draw the right end.
	numV = getModelVertices(destination_vb, *curVertexP, xOffset+(numSpans-1)*spanLength, vec, vecNormal, vecZ, m_start,
		m_rightMtx, m_rightMesh, pLightsIterator);
	if (!numV)
	{	//not enough room for vertices
		DEBUG_ASSERTCRASH( numV, ("W3DBridge::GetIndicesNVertices(). Vertex overflow.") );
		return;
	}
	numI = getModelIndices( destination_ib, *curIndexP, *curVertexP, m_rightMesh);
	if (!numI)
	{	//not enough room for indices
		DEBUG_ASSERTCRASH( numI, ("W3DBridge::GetIndicesNVertices(). Index overflow.") );
		return;
	}
	*curIndexP += numI;
	*curVertexP += numV;
	m_numVertex += numV;
	m_numPolygons += numI/3;
}

//=============================================================================
// W3DBridge::getModelIndices
//=============================================================================
/** Gets the index values for a particular mesh section of the bridge.  */
//=============================================================================
Int W3DBridge::getModelIndices(UnsignedShort *destination_ib, Int curIndex, Int vertexOffset, MeshClass *pMesh)
{
	if (pMesh == nullptr)
		return(0);
	Int numPoly = pMesh->Peek_Model()->Get_Polygon_Count();
	const TriIndex *pPoly =pMesh->Peek_Model()->Get_Polygon_Array();
	if (curIndex+3*numPoly+6 >= W3DBridgeBuffer::MAX_BRIDGE_INDEX) {
		return(0);
	}
	UnsignedShort *curIb = destination_ib+curIndex;
	Int i;
	for (i=0; i<numPoly; i++) {
		*curIb++ = vertexOffset + pPoly[i].I;
		*curIb++ = vertexOffset + pPoly[i].J;
		*curIb++ = vertexOffset + pPoly[i].K;
	}
	return(numPoly*3);
}

//-----------------------------------------------------------------------------
//         Private Functions
//-----------------------------------------------------------------------------

///@todo - Sort bridges by texture for better performance.

//=============================================================================
// W3DBridgeBuffer::cull
//=============================================================================
/** Culls the bridges, marking the visible flag.  If a bridge changes visibility, it sets
m_anythingChanged */
//=============================================================================
void W3DBridgeBuffer::cull(CameraClass * camera)
{
	Int curBridge;

	m_anythingChanged = m_updateVis;

	for (curBridge=0; curBridge<m_numBridges; curBridge++) {
		if (m_bridges[curBridge].cullBridge(camera)) {
			m_anythingChanged = true;
		}
	}
}


//=============================================================================
// W3DBridgeBuffer::loadBridgesInVertexAndIndexBuffers
//=============================================================================
/** Loads the bridges into the vertex buffer for drawing. */
//=============================================================================
void W3DBridgeBuffer::loadBridgesInVertexAndIndexBuffers(RefRenderObjListIterator *pLightsIterator)
{
	if (!m_indexBridge || !m_vertexBridge || !m_initialized) {
		return;
	}
	m_curNumBridgeVertices = 0;
	m_curNumBridgeIndices = 0;
	VertexFormatXYZNDUV1 *vb;
	UnsignedShort *ib;
	// Lock the buffers.
	DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexBridge);
	DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexBridge);
	vb=(VertexFormatXYZNDUV1*)lockVtxBuffer.Get_Vertex_Array();
	ib = lockIdxBuffer.Get_Index_Array();

//	UnsignedShort *curIb = ib;

//	VertexFormatXYZNDUV1 *curVb = vb;

	Int curBridge;

	for (curBridge=0; curBridge<m_numBridges; curBridge++) {
		m_bridges[curBridge].getIndicesNVertices(ib, vb, &m_curNumBridgeIndices,
			&m_curNumBridgeVertices, pLightsIterator);
	}
}

//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DBridgeBuffer::~W3DBridgeBuffer
//=============================================================================
/** Destructor. Releases w3d assets. */
//=============================================================================
W3DBridgeBuffer::~W3DBridgeBuffer()
{
	freeBridgeBuffers();
}

//=============================================================================
// W3DBridgeBuffer::W3DBridgeBuffer
//=============================================================================
/** Constructor. Sets m_initialized to true if it finds the w3d models it needs
for the bridges. */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
W3DBridgeBuffer::W3DBridgeBuffer()
{
	m_initialized = false;
	m_vertexMaterial = nullptr;
	m_vertexBridge = nullptr;
	m_indexBridge = nullptr;
	m_bridgeTexture = nullptr;
	m_curNumBridgeVertices=0;
	m_curNumBridgeIndices=0;
	clearAllBridges();
	allocateBridgeBuffers();
	m_initialized = true;
}
// GX-SIMID-END


//=============================================================================
// W3DBridgeBuffer::freeBridgeBuffers
//=============================================================================
/** Frees the index and vertex buffers. */
//=============================================================================
void W3DBridgeBuffer::freeBridgeBuffers()
{
	REF_PTR_RELEASE(m_vertexBridge);
	REF_PTR_RELEASE(m_indexBridge);
	REF_PTR_RELEASE(m_vertexMaterial);
}

//=============================================================================
// W3DBridgeBuffer::allocateBridgeBuffers
//=============================================================================
/** Allocates the index and vertex buffers. */
//=============================================================================
void W3DBridgeBuffer::allocateBridgeBuffers()
{
	if (TheGlobalData->m_headless)
		return;
	m_vertexBridge=NEW_REF(DX8VertexBufferClass,(DX8_FVF_XYZNDUV1,MAX_BRIDGE_VERTEX+4,DX8VertexBufferClass::USAGE_DYNAMIC));
	m_indexBridge=NEW_REF(DX8IndexBufferClass,(MAX_BRIDGE_INDEX+4, DX8IndexBufferClass::USAGE_DYNAMIC));
	m_vertexMaterial=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
#ifdef USE_BRIDGE_NORMALS
	m_vertexMaterial= NEW VertexMaterialClass();
	m_vertexMaterial->Set_Shininess(0.0);
	m_vertexMaterial->Set_Ambient(1,1,1);
	m_vertexMaterial->Set_Diffuse(1,1,1);
	m_vertexMaterial->Set_Specular(0,0,0);
	m_vertexMaterial->Set_Emissive(0,0,0);
	m_vertexMaterial->Set_Opacity(1);
	m_vertexMaterial->Set_Lighting(true);
	m_vertexMaterial->Set_Diffuse_Color_Source(VertexMaterialClass::COLOR1);
#endif
	m_curNumBridgeVertices=0;
	m_curNumBridgeIndices=0;
}

//=============================================================================
// W3DBridgeBuffer::clearAllBridges
//=============================================================================
/** Removes all bridges. */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DBridgeBuffer::clearAllBridges()
{
	Int curBridge;
	for (curBridge=0; curBridge<m_numBridges; curBridge++) {
		m_bridges[curBridge].clearBridge();
	}
	m_curNumBridgeIndices = 0;
	m_numBridges=0;
}
// GX-SIMID-END

//=============================================================================
// W3DBridgeBuffer::loadBridges
//=============================================================================
/** loadBridges.  When loaded, tell the terrain logic where the bridge is. */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DBridgeBuffer::loadBridges(W3DTerrainLogic *pTerrainLogic, Bool saveGame)
{
	// TheSuperHackers @logic-client-separation helmutbuhler 26/4/2025
	// W3DBridgeBuffer shouldn't add objects to W3DTerrainLogic
	clearAllBridges();
	MapObject *pMapObj;
	MapObject *pMapObj2;
	for (pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext()) {
		if (pMapObj->getFlag(FLAG_BRIDGE_POINT1)) {
			pMapObj2 = pMapObj->getNext();
			if ( !pMapObj2 || !pMapObj2->getFlag(FLAG_BRIDGE_POINT2)) {
				DEBUG_LOG(("Missing second bridge point.  Ignoring first."));
			}
			if (pMapObj2==nullptr) break;
			if (!pMapObj2->getFlag(FLAG_BRIDGE_POINT2)) continue;
			// GeneralsX @note /09/2026 THIS Z IS MAP DATA, DESPITE THE OBJECT IT HANGS OFF.
			//
			// Flagged when the bridge desync was fixed: a simulation value read through
			// TheTerrainRenderObject looks exactly like the model-derived footprint that caused
			// that bug. It is not the same thing, and the difference is worth writing down so
			// nobody has to re-derive it.
			//
			// getHeightMapHeight does NOT sample render state. Its first act is
			//   WorldHeightMap *logicHeightMap = TheTerrainVisual ? TheTerrainVisual->getLogicHeightMap() : m_map;
			// (Core/.../BaseHeightMap.cpp:873) and the engine's own comment above that line says it
			// was deliberately made "logic safe" for this reason. m_logicHeightMap is constructed
			// straight from the map file stream (W3DTerrainVisual.cpp:606) and is described there
			// as "the true, golden standard"; the deformable render copy is a separate
			// m_clientHeightMap. The m_map fallback is the same object anyway - initHeightData is
			// handed m_logicHeightMap (W3DTerrainVisual.cpp:663) and stores it at
			// BaseHeightMap.cpp:1788. So both arms of that ternary are the map's own heightmap and
			// the height is identical on every peer that loaded the same map.
			//
			// It also cannot usefully move. The logic-side API, W3DTerrainLogic::getGroundHeight,
			// forwards to this very call (W3DTerrainLogic.cpp:284), and WorldHeightMap exposes no
			// interpolating height-at-(x,y) of its own - the triangle interpolation only exists
			// here. Routing through TheTerrainLogic would be a rename onto the identical body, and
			// it would need a null branch, because WorldBuilder calls loadRoadsAndBridges(nullptr,
			// FALSE) (wbview3d.cpp:517, :1960) with no TerrainLogic at all. Two call paths that
			// must stay equal, for no change in behaviour: not worth it.
			Vector3 from, to;
			from.Set(pMapObj->getLocation()->x, pMapObj->getLocation()->y, 0);
			from.Z = TheTerrainRenderObject->getHeightMapHeight(from.X, from.Y, nullptr) + BRIDGE_FLOAT_AMT;
			to.Set(pMapObj2->getLocation()->x, pMapObj2->getLocation()->y, 0);
			to.Z = TheTerrainRenderObject->getHeightMapHeight(to.X, to.Y, nullptr) + BRIDGE_FLOAT_AMT;
			addBridge(from, to, pMapObj->getName(), pTerrainLogic, pMapObj->getProperties());
			pMapObj = pMapObj2;
		}
	}
	if (pTerrainLogic) {
		pTerrainLogic->updateBridgeDamageStates();
	}
}
// GX-SIMID-END

//=============================================================================
//=============================================================================
static RenderObjClass* createTower( SimpleSceneClass *scene,
																		W3DAssetManager *assetManager,
																		MapObject *mapObject,
																	  BridgeTowerType type,
																	  BridgeInfo *bridgeInfo )
{
	RenderObjClass* tower = nullptr;

	// sanity
	if( scene == nullptr ||
			assetManager == nullptr ||
			mapObject == nullptr ||
			bridgeInfo == nullptr ||
			type < 0 || type >= BRIDGE_MAX_TOWERS )
		return nullptr;

	// get template for this bridge
	DEBUG_ASSERTCRASH( TheTerrainRoads, ("createTower: TheTerrainRoads is null") );
	TerrainRoadType *bridgeTemplate = TheTerrainRoads->findBridge( mapObject->getName() );
	if( bridgeTemplate == nullptr )
		return nullptr;

	// given the type of tower (corner position) find the appropriate spot to put the tower
	Coord3D towerPos;
	switch( type )
	{

		case BRIDGE_TOWER_FROM_LEFT:	towerPos = bridgeInfo->fromLeft;		break;
		case BRIDGE_TOWER_FROM_RIGHT: towerPos = bridgeInfo->fromRight;		break;
		case BRIDGE_TOWER_TO_LEFT:		towerPos = bridgeInfo->toLeft;			break;
		case BRIDGE_TOWER_TO_RIGHT:		towerPos = bridgeInfo->toRight;			break;
		default: return nullptr;

	}

	// set the Z position to that of the terrain
	towerPos.z = TheTerrainRenderObject->getHeightMapHeight( towerPos.x, towerPos.y, nullptr);

	// find the thing template for the tower we want to construct
	AsciiString towerTemplateName = bridgeTemplate->getTowerObjectName( type );
	DEBUG_ASSERTCRASH( TheThingFactory, ("createTower: TheThingFactory is null") );
	const ThingTemplate *towerTemplate = TheThingFactory->findTemplate( towerTemplateName );
	if( towerTemplate == nullptr )
		return nullptr;

	// find the name of the render object to show
	const ModuleInfo& mi = towerTemplate->getDrawModuleInfo();
	if( mi.getCount() <= 0 )
		return nullptr;
	const ModuleData* mdd = mi.getNthData(0);
	const W3DModelDrawModuleData* md = mdd ? mdd->getAsW3DModelDrawModuleData() : nullptr;
	if( md == nullptr )
		return nullptr;
	ModelConditionFlags state;
	state.clear();
	AsciiString modelName = md->getBestModelNameForWB( state );

	// create the render object
	Int playerColor = 0xFFFFFF;
	tower = assetManager->Create_Render_Obj( modelName.str(), 1.0f, playerColor );

	// tie the render object into the map object
	mapObject->setBridgeRenderObject( type, tower );

	// set the position of the tower render object to the position in the world
	Matrix3D transform;
	transform.Make_Identity();
	transform.Set_X_Translation( towerPos.x );
	transform.Set_Y_Translation( towerPos.y );
	transform.Set_Z_Translation( towerPos.z );
	tower->Set_Transform( transform );

	// set the angle for the tower
	/// @todo --> write me

	// add tower render object to the scene
	scene->Add_Render_Object( tower );

	// return the render object of the tower created
	return tower;

}

//=============================================================================
//=============================================================================
static void updateTowerPos( RenderObjClass* tower,
														BridgeTowerType type,
														BridgeInfo* bridgeInfo )
{

	// sanity
	if( tower == nullptr || type < 0 || type >= BRIDGE_MAX_TOWERS || bridgeInfo == nullptr )
		return;

	//
	// compute the angle of the bridge ... we consider the angle of the bridge to be
	// from 'from' to 'to' in the bridge info ... and so does the game
	//
	Coord2D v;
	v.x = bridgeInfo->toLeft.x - bridgeInfo->fromLeft.x;
	v.y = bridgeInfo->toLeft.y - bridgeInfo->fromLeft.y;
	Real angle = v.toAngle();

	//
	// given the type of tower (corner position) find the appropriate spot to put the tower
	// NOTE that we're also adjusting the angle for the from side to point the
	// opposite way the "bridge is pointing"
	//
	Coord3D towerPos;
	switch( type )
	{

		case BRIDGE_TOWER_FROM_LEFT:	towerPos = bridgeInfo->fromLeft;		angle += PI; break;
		case BRIDGE_TOWER_FROM_RIGHT: towerPos = bridgeInfo->fromRight;		angle += PI; break;
		case BRIDGE_TOWER_TO_LEFT:		towerPos = bridgeInfo->toLeft;			break;
		case BRIDGE_TOWER_TO_RIGHT:		towerPos = bridgeInfo->toRight;			break;
		default: return;

	}

	// set the position of the tower render object to the position in the world
	Matrix3D transform;
	transform.Make_Identity();
	transform.Set_X_Translation( towerPos.x );
	transform.Set_Y_Translation( towerPos.y );
	transform.Set_Z_Translation( towerPos.z );
	transform.Rotate_Z( angle );
	tower->Set_Transform( transform );

	// set the angle for the tower
//	tower->setAngle( angle );

}

//=============================================================================
// W3DBridgeBuffer::worldBuilderUpdateBridgeTowers
//=============================================================================
/** loadBridges.  When loaded, tell the terrain logic where the bridge is. */
//=============================================================================
void W3DBridgeBuffer::worldBuilderUpdateBridgeTowers( W3DAssetManager *assetManager,
																											SimpleSceneClass *scene )
{
	MapObject *pMapObj;
	MapObject *pMapObj2;

	for( pMapObj = MapObject::getFirstMapObject(); pMapObj; pMapObj = pMapObj->getNext() )
	{

		if( pMapObj->getFlag( FLAG_BRIDGE_POINT1 ) )
		{

			pMapObj2 = pMapObj->getNext();
			if( !pMapObj2 || !pMapObj2->getFlag( FLAG_BRIDGE_POINT2 ) )
				DEBUG_LOG(("Missing second bridge point.  Ignoring first."));

			if( pMapObj2 == nullptr )
				break;
			if( !pMapObj2->getFlag( FLAG_BRIDGE_POINT2 ) )
				continue;

			//
			// now that we've got the two map objects that are bridge point 1 and 2, get the
			// bridge info that has been stored
			//
			for( Int i = 0; i < m_numBridges; ++i )
			{

				//
				// find the bridge with the matching name and position ... note we're just matching
				// (x,y) here cause name and location (without the additional complication of Z) is
				// really all we have to match bridges.
				/// @todo integrate the editor with the game ... will never happen tho ...
				//
				if( m_bridges[ i ].getTemplateName() == pMapObj->getName() &&
						m_bridges[ i ].getStart()->X == pMapObj->getLocation()->x &&
						m_bridges[ i ].getStart()->Y == pMapObj->getLocation()->y &&
						m_bridges[ i ].getEnd()->X == pMapObj2->getLocation()->x &&
						m_bridges[ i ].getEnd()->Y == pMapObj2->getLocation()->y )
				{
					RenderObjClass *towerRenderObj;

					// get the bridge info
					BridgeInfo bridgeInfo;
					m_bridges[ i ].getBridgeInfo( &bridgeInfo );

					// go through all bridge tower render objects
					Bool created;
					for( Int j = 0; j < BRIDGE_MAX_TOWERS; ++j )
					{

						// create render object if needed
						created = FALSE;
						towerRenderObj = pMapObj->getBridgeRenderObject( (BridgeTowerType)j );
						if( towerRenderObj == nullptr )
						{

							towerRenderObj = createTower( scene, assetManager, pMapObj, (BridgeTowerType)j, &bridgeInfo );
							created = TRUE;

						}

						// sanity
						DEBUG_ASSERTCRASH( towerRenderObj != nullptr, ("worldBuilderUpdateBridgeTowers: unable to create tower for bridge '%s'",
															 m_bridges[ i ].getTemplateName().str()) );

						// update the position of the towers
						updateTowerPos( towerRenderObj, (BridgeTowerType)j, &bridgeInfo );

						// release the initial ref count of 1 for a newly created tower
						if( created )
							REF_PTR_RELEASE( towerRenderObj );

					}

				}

			}

			// skip the 2nd map object representing the second half of the bridgef
			pMapObj = pMapObj2;

		}

	}

}

// GeneralsX: Is this a live game with somebody else in it? Counted as
// GameLogic::gxCheatsAllowed counts humans - observers included, the engine's ReplayObserver not -
// from the player list, which is already this map's: GameLogic::startNewGame runs
// ThePlayerList->newGame() before TerrainLogic::newMap loads the bridges. TheNetwork is null in
// skirmish, the campaign and a replay; a solo headless -lanhost has it but only one human.
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
static Bool liveGameWithAnotherHuman()
{
	// GeneralsX: ASKED OF gxCheatsAllowed ITSELF, not counted
	// again here: an observer slot shares the ReplayObserver and has no Player of its own, so a copy of the old count
	// read one player watched by an observer as one human, and let that peer build fallback bridge geometry.
	return TheNetwork != nullptr && ThePlayerList != nullptr && !GameLogic::gxCheatsAllowed();
}
// GX-SIMID-END

//=============================================================================
// W3DBridgeBuffer::addBridge
//=============================================================================
/** Adds a bridge.  Name is the GDF object name. */
//=============================================================================
// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
void W3DBridgeBuffer::addBridge(Vector3 fromLoc, Vector3 toLoc, AsciiString name, W3DTerrainLogic *pTerrainLogic, Dict *props)
{
	if (m_numBridges >= MAX_BRIDGES) {
		return;
	}

	if (!m_initialized) {
		return;
	}

	// GeneralsX: A BRIDGE POINT PAIR NAMING NO BRIDGE IS STILL IGNORED.
	//
	// Found by review (r4): the decouple below first applied to this case too, so a map whose
	// bridge points name a template TerrainRoads does not define gained an invisible GenericBridge
	// and a pathfind layer made of fallback geometry, where the game has always ignored the pair.
	// Every peer with the same INI answers this the same way, so it needs no decoupling - only a
	// missing MODEL for a real template does, because that is the part that differs by install.
	// A template that names NO model is the same case: retail's load() fails on it on every install
	// (Roads.ini's six landmark bridges - EuropeanLandmarkBridge, TsingMaLandmarkBridge and the rest -
	// are objects, built through TerrainLogic::addLandmarkBridgeToLogic, and carry no BridgeModelName),
	// so the pair was always ignored and must stay ignored for retail replays to match.
	TerrainRoadType *bridgeTemplate = TheTerrainRoads->findBridge(name);
	if (bridgeTemplate == nullptr || bridgeTemplate->getBridgeModel().isEmpty()) {
		return;
	}

	m_bridges[m_numBridges].init(fromLoc, toLoc, name);

	// GeneralsX: A SIMULATION OBJECT MUST NOT EXIST ONLY WHEN ITS ART DOES.
	//
	// This used to read `if (load(BODY_PRISTINE)) { ... addBridgeToLogic ...; m_numBridges++; }`,
	// so on a peer whose install cannot supply the bridge model the map's GenericBridge objects
	// were never created at all. MEASURED 2026-09-20 on this box, one symlink the only variable:
	// a Zero Hour data root with no reachable base-Generals archives ran twilight flame with 452
	// objects at frame 0 where the same build with them had 456, every later object id shifted by
	// two, and the frame-0 CRC differed (4A947AEF vs BAF0B59D) and never recovered for the whole
	// match. The only visible symptom was one [ASSET_ROOT] warning.
	//
	// The renderer needs no model: getIndicesNVertices gives a meshless slot zero polygons and
	// renderBridge skips a bridge with none, so an artless bridge still simply is not drawn. It does
	// now consume an m_bridges slot, which is what keeps info.bridgeIndex pointing at its own slot.
	if (!m_bridges[m_numBridges].load(BODY_PRISTINE)) {
		// GeneralsX: THE LAST LINE, where the map's own bridges are built.
		//
		// TerrainVisual::isEveryBridgeModelAvailable refuses network play up front, but it can only
		// see the bridges TheTerrainRoads held when it was asked - before this map's map.ini, which
		// may define bridges of its own. An artless bridge's footprint comes from
		// fallback numbers, not the model everybody else loaded, so in a live game with another
		// human the choice is between stopping here, loudly, and a desync nobody can explain. Solo
		// play keeps the fallback: nobody disagrees with it. The [ASSET_ROOT] WARNING line is what
		// the cross-platform harness gate reads, since ReleaseCrash reports only to its own file.
		if (liveGameWithAnotherHuman()) {
			AsciiString reason;
			reason.format("bridge model '%s' (bridge '%s' on this map) cannot be loaded, so this peer would "
				"simulate a different bridge from every other player. Install the base Generals archives "
				"(Textures.big/W3D.big/Terrain.big) in a ZH_Generals folder inside the game folder.",
				bridgeTemplate->getBridgeModel().str(), name.str());
			fprintf(stderr, "[ASSET_ROOT] WARNING: %s\n", reason.str());
			fflush(stderr);
			RELEASE_CRASH(reason.str());
		}
		DEBUG_LOG(("W3DBridgeBuffer::addBridge - no model for bridge '%s'; it will not be drawn, "
			"but its logic object is created anyway so the simulation matches every other peer.",
			name.str()));
	}

	W3DBridge *pBridge = m_bridges+m_numBridges;
	if (pTerrainLogic) {
		BridgeInfo info;
		pBridge->getBridgeInfo(&info);
		info.bridgeIndex = m_numBridges;
		pTerrainLogic->addBridgeToLogic(&info, props, name);
	}
	m_numBridges++;
}
// GX-SIMID-END

//=============================================================================
// W3DBridgeBuffer::updateCenter
//=============================================================================
/** Updates the drawing buffer, based on the camera position. */
//=============================================================================
void W3DBridgeBuffer::updateCenter(CameraClass *camera, RefRenderObjListIterator *pLightsIterator)
{
	cull(camera);
	if (m_anythingChanged || m_curNumBridgeIndices == 0) {
		loadBridgesInVertexAndIndexBuffers(pLightsIterator);
	}
	m_updateVis = false;
}

//=============================================================================
// W3DBridgeBuffer::drawBridges
//=============================================================================
/** Draws the bridges. */
//=============================================================================
void W3DBridgeBuffer::drawBridges(CameraClass * camera, Bool wireframe, TextureClass *cloudTexture)
{

	Int curBridge;
	if (TheTerrainLogic) {
		for (curBridge=0; curBridge<m_numBridges; curBridge++) {
			m_bridges[curBridge].setEnabled(false);
		}
		/* Check for any changed damage states. */
		Bool changed = false;
		for (Bridge *bridge = TheTerrainLogic->getFirstBridge(); bridge; bridge = bridge->getNext()) {
			BridgeInfo info;
			bridge->getBridgeInfo(&info);
			if (info.bridgeIndex<0 || info.bridgeIndex>=m_numBridges) {
				continue;
			}
			m_bridges[info.bridgeIndex].setEnabled(true);
			if (m_bridges[info.bridgeIndex].getDamageState() != info.curDamageState) {
				changed = true;
				BodyDamageType curState = m_bridges[info.bridgeIndex].getDamageState();
				m_bridges[info.bridgeIndex].setDamageState(info.curDamageState);
				if (!m_bridges[info.bridgeIndex].load(info.curDamageState)) {
					// put the old model back.
					m_bridges[info.bridgeIndex].load(curState);
					m_bridges[info.bridgeIndex].setDamageState(info.curDamageState);
				}
			}
		}
		if (changed) {
			loadBridgesInVertexAndIndexBuffers(nullptr);
		}
	}	else {
		// In wb, all are enabled.
		for (curBridge=0; curBridge<m_numBridges; curBridge++) {
			m_bridges[curBridge].setEnabled(true);
		}
	}



	if (m_curNumBridgeIndices == 0) {
		return;
	}

	DX8Wrapper::Set_Material(m_vertexMaterial);
	// Setup the vertex buffer, shader & texture.
	DX8Wrapper::Set_Index_Buffer(m_indexBridge,0);
	DX8Wrapper::Set_Vertex_Buffer(m_vertexBridge);
	DX8Wrapper::Set_Shader(detailAlphaShader);
#ifdef RTS_DEBUG
	//DX8Wrapper::Set_Shader(detailShader); // shows alpha clipping.
#endif

	DX8Wrapper::Apply_Render_State_Changes();

	if (!wireframe && cloudTexture)
	{	//Force a cloud texture projection into stage 1
		W3DShaderManager::setTexture(1,cloudTexture);
		W3DShaderManager::setShader(W3DShaderManager::ST_CLOUD_TEXTURE,1);
	}

	for (curBridge=0; curBridge<m_numBridges; curBridge++) {
		if (m_bridges[curBridge].isEnabled() && m_bridges[curBridge].isVisible()) {
			m_bridges[curBridge].renderBridge(wireframe);
		}
	}

	if (!wireframe && cloudTexture)
		//Force a cloud texture projection into stage 1
		W3DShaderManager::resetShader(W3DShaderManager::ST_CLOUD_TEXTURE);

	//Render shroud pass over all the bridges
	if (!wireframe && TheTerrainRenderObject->getShroud())
	{
		//Reset to a known shader.
		DX8Wrapper::Invalidate_Cached_Render_States();
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Material(m_vertexMaterial);
		DX8Wrapper::Set_Index_Buffer(m_indexBridge,0);
		DX8Wrapper::Set_Vertex_Buffer(m_vertexBridge);
		DX8Wrapper::Apply_Render_State_Changes();
		//Apply custom shroud projection shader.
		W3DShaderManager::setTexture(0,TheTerrainRenderObject->getShroud()->getShroudTexture());
		W3DShaderManager::setShader(W3DShaderManager::ST_SHROUD_TEXTURE, 0);
		for (curBridge=0; curBridge<m_numBridges; curBridge++) {
			if (m_bridges[curBridge].isEnabled() && m_bridges[curBridge].isVisible()) {
				//Pretend we're in wireframe so function doesn't reset the shroud texture.
				m_bridges[curBridge].renderBridge(TRUE);
			}
		}
		W3DShaderManager::resetShader(W3DShaderManager::ST_SHROUD_TEXTURE);
	}
}


