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

// FILE: W3DScene.cpp /////////////////////////////////////////////////////////
//
// Scene manger for display using W3DDispaly.  A scene manager can customize
// the rendering process, culling, material passes ...
//
// Author: Colin Day, April 2001
//
///////////////////////////////////////////////////////////////////////////////

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
#include <stdlib.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>	// TARGET_OS_IPHONE exists only after this: GX_COLLECTPAR's platform default
#endif

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "GameClient/GXNoShroud.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/Object.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"		// [GXDRAWCHECK]
#include "GameLogic/PartitionManager.h"	// [GXDRAWCHECK]
#include "GameClient/Drawable.h"
#include "GameClient/DrawableInfo.h"	// [GXDRAWCHECK]
#include "GameClient/GameClient.h"	// [GXDRAWCHECK] TheGameClient
#include "Common/ThingTemplate.h"	// [GXDRAWCHECK] draw module infos
#include "W3DDevice/GameClient/Module/W3DModelDraw.h"	// [GXDRAWCHECK]
#include "W3DDevice/GameClient/W3DDisplay.h"	// [GXDRAWCHECK] the 3D scene, for plant a
#include <algorithm>	// [GXDRAWCHECK] std::find
#include "GameClient/ParticleSys.h"
#include "GameClient/Color.h"
#include "GameClient/View.h"
#include "GameClient/GXGfxProbe.h"	// [GXSHADOW], [GXGFXF] (graphics P2)
#include "W3DDevice/GameClient/GXParallel.h"	// R-CPU pose pre-pass
#include "W3DDevice/GameClient/GXRender/Scene/GxScene.h"	// WS-G L2 (census today)
#include "W3DDevice/GameClient/GXRender/Scene/GxL2Dry.h"
#include "W3DDevice/GameClient/GxInterop.h"	// WS-X hybrid spike
#include "W3DDevice/GameClient/GxDrawRun.h"	// WS-X2 DrawRun
#include "WW3D2/hlod.h"
#include "WW3D2/hcanim.h"
#include "WW3D2/w3d_file.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/dx8rendererdebugger.h"
#include "WWDebug/wwprofile.h"
#include <atomic>
#include <chrono>
#include <vector>
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/W3DShadow.h"
#include "W3DDevice/GameClient/GXShadowMapHooks.h"	// S7 A49: GxShadowMapFireArmed
#include "W3DDevice/GameClient/W3DStatusCircle.h"
#include "W3DDevice/GameClient/W3DCustomScene.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/W3DWorldSeam.h"	// F-depth+'s mid-frame tap
#include "WW3D2/camera.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/sortingrenderer.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/Renderer.h"
#include "WW3D2/light.h"
#include "WW3D2/matpass.h"
#include "WW3D2/shader.h"
#include "WW3D2/dx8caps.h"
#include "WW3D2/colorspace.h"

#include "WW3D2/shdlib.h"
#include "WW3D2/dx8vertexbuffer.h"	// gxDrawUnderworldFloor
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/vertmaterial.h"

///////////////////////////////////////////////////////////////////////////////
// DEFINITIONS ////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///@todo: Remove these globals since we no longer need W3D to call them for us.
extern void PrepareShadows();
extern void DoTrees(RenderInfoClass & rinfo);
extern void DoShadows(RenderInfoClass & rinfo, Bool stencilPass);
extern void DoParticles(RenderInfoClass & rinfo);

// No texturing, no zbuffer reading/writing, primary gradient, no
// blending, no fogging - mostly for use in solid-colored opaque objects.
#define SC_PLAYER_COLOR ( SHADE_CNST(ShaderClass::PASS_ALWAYS, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, \
	ShaderClass::SRCBLEND_ONE, ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, \
	ShaderClass::TEXTURING_DISABLE, ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )
static ShaderClass PlayerColorShader(SC_PLAYER_COLOR);

//=============================================================================
// RTS3DScene::RTS3DScene
//=============================================================================
/** */
//=============================================================================
RTS3DScene::RTS3DScene()
{
	setName("RTS3DScene");
	GxScene_Install();	// GeneralsX: WS-G: installs nothing unless a GX_L2_* switch is set
	GxInterop::Install();	// GeneralsX: WS-X: nothing unless GX_RENDER_HYBRID is set (GxInterop.h)
	GxDrawRun::Install();	// GeneralsX: WS-X2: nothing unless GX_HYBRID_DRAWRUN_TEST is set (GxDrawRun.h)
	m_drawTerrainOnly = false;
	m_numGlobalLights=0;
	m_defaultLightEnvAmbient.Set(0,0,0);
	m_defaultLightEnvValid = FALSE;
	m_enabledDynamicLightsValid = FALSE;
	Int i=0;
	for (; i<LightEnvironmentClass::MAX_LIGHTS; i++)
	{
		m_globalLight[i]=nullptr;
		m_infantryLight[i]=NEW_REF( LightClass, (LightClass::DIRECTIONAL) );
	}

	m_scratchLight = NEW_REF( LightClass, (LightClass::DIRECTIONAL) );
//	REF_PTR_SET(m_globalLight[lightIndex], pLight);

#if ENABLE_CONFIGURABLE_SHROUD
	if (gxShroudDrawn())
		m_shroudMaterialPass = NEW_REF(W3DShroudMaterialPassClass,());
	else
		m_shroudMaterialPass = nullptr;
#else
	m_shroudMaterialPass = NEW_REF(W3DShroudMaterialPassClass,());
#endif

	m_maskMaterialPass = NEW_REF(W3DMaskMaterialPassClass,());
	m_customPassMode = SCENE_PASS_DEFAULT;

	m_heatVisionMaterialPass = NEW_REF(MaterialPassClass,());
	m_heatVisionOnlyPass = NEW_REF(MaterialPassClass,());
	VertexMaterialClass *heatVisionMtl = NEW_REF(VertexMaterialClass,());
	heatVisionMtl->Set_Lighting(true);
	heatVisionMtl->Set_Ambient(0,0,0);
	heatVisionMtl->Set_Diffuse(0.02f,0.01f,0.00f);
	heatVisionMtl->Set_Emissive(0.5f,0.2f,0.0f);
	m_heatVisionMaterialPass->Set_Material(heatVisionMtl);
	ShaderClass heatVisionShader=ShaderClass::_PresetAdditiveSolidShader;
	heatVisionShader.Set_Depth_Compare(ShaderClass::PASS_EQUAL);
	m_heatVisionMaterialPass->Set_Shader(heatVisionShader);
	heatVisionMtl->Release_Ref();
	heatVisionShader.Set_Depth_Compare(ShaderClass::PASS_LEQUAL);
	heatVisionShader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
	m_heatVisionOnlyPass->Set_Material(heatVisionMtl);
	m_heatVisionOnlyPass->Set_Shader(heatVisionShader);


//	VertexMaterialClass *frenzyMtl = NEW_REF(VertexMaterialClass,());
//	frenzyMtl->Set_Lighting(TRUE);
//	frenzyMtl->Set_Ambient(  0, 0, 0 );
//	frenzyMtl->Set_Diffuse(  1.0f, 0.0f, 0.0f );
//	frenzyMtl->Set_Emissive( 1.0f, 0.0f, 0.0f );
//	m_frenzyMaterialPass = NEW_REF(MaterialPassClass,());
//	m_frenzyMaterialPass->Set_Material(frenzyMtl);
//	frenzyMtl->Release_Ref();
//	ShaderClass frenzyShader=ShaderClass::_PresetMultiplicativeShader;
//	frenzyShader.Set_Depth_Compare(ShaderClass::PASS_EQUAL);
//	frenzyShader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
//	m_frenzyMaterialPass->Set_Shader(frenzyShader);


	//Allocate memory to hold queue of visible render objects that need to be drawn last
	//because they are forced translucent.
	m_translucentObjectsCount = 0;
	if (TheGlobalData->m_maxVisibleTranslucentObjects > 0)
		m_translucentObjectsBuffer = NEW RenderObjClass* [TheGlobalData->m_maxVisibleTranslucentObjects];
	else
		m_translucentObjectsBuffer = nullptr;

	m_numPotentialOccluders=0;
	m_numPotentialOccludees=0;
	m_numNonOccluderOrOccludee=0;
	m_occludedObjectsCount=0;

	if (TheGlobalData->m_maxVisibleOccluderObjects > 0)
		m_potentialOccluders = NEW RenderObjClass* [TheGlobalData->m_maxVisibleOccluderObjects];
	else
		m_potentialOccluders = nullptr;

	if (TheGlobalData->m_maxVisibleOccludeeObjects > 0)
		m_potentialOccludees = NEW RenderObjClass* [TheGlobalData->m_maxVisibleOccludeeObjects];
	else
		m_potentialOccludees = nullptr;

	if (TheGlobalData->m_maxVisibleNonOccluderOrOccludeeObjects > 0)
		m_nonOccludersOrOccludees = NEW RenderObjClass* [TheGlobalData->m_maxVisibleNonOccluderOrOccludeeObjects];
	else
		m_nonOccludersOrOccludees = nullptr;

	//Modify the shader to make occlusion transparent
	ShaderClass shader = PlayerColorShader;
	shader.Set_Src_Blend_Func(ShaderClass::SRCBLEND_SRC_ALPHA);
	shader.Set_Dst_Blend_Func(ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA);

#ifdef USE_NON_STENCIL_OCCLUSION
	for (i=0; i<MAX_PLAYER_COUNT; i++)
	{
		m_occludedMaterialPass[i]=NEW_REF(MaterialPassClass,());
		VertexMaterialClass * vmtl = NEW_REF(VertexMaterialClass,());
		vmtl->Set_Lighting(true);
		vmtl->Set_Ambient(0,0,0);	//we're only using emissive so kill all other lights.
		vmtl->Set_Diffuse(0,0,0);
		m_occludedMaterialPass[i]->Set_Material(vmtl);
		m_occludedMaterialPass[i]->Set_Shader(shader);
		vmtl->Release_Ref();	//material pass is holding the pointer so release ref.
	}
#else
	for (i=0; i<MAX_PLAYER_COUNT; i++)
		m_occludedMaterialPass[i]=nullptr;
#endif

}

//=============================================================================
// RTS3DScene::~RTS3DScene
//=============================================================================
/** */
//=============================================================================
RTS3DScene::~RTS3DScene()
{
	Int i=0;
	for (; i<LightEnvironmentClass::MAX_LIGHTS; i++)
	{
		REF_PTR_RELEASE(m_globalLight[i]);
		REF_PTR_RELEASE(m_infantryLight[i]);
	}

	REF_PTR_RELEASE(m_scratchLight);

	REF_PTR_RELEASE(m_shroudMaterialPass);

	REF_PTR_RELEASE(m_maskMaterialPass);

	REF_PTR_RELEASE(m_heatVisionMaterialPass);

	REF_PTR_RELEASE(m_heatVisionOnlyPass);

	delete [] m_translucentObjectsBuffer;
	delete [] m_nonOccludersOrOccludees;
	delete [] m_potentialOccludees;
	delete [] m_potentialOccluders;

	for (i=0; i<MAX_PLAYER_COUNT; i++)
	{
		REF_PTR_RELEASE(m_occludedMaterialPass[i]);
	}
}


void	RTS3DScene::setGlobalLight(LightClass *pLight, Int lightIndex)
{
	if (m_numGlobalLights < (lightIndex+1))
		m_numGlobalLights=(lightIndex+1);
	REF_PTR_SET(m_globalLight[lightIndex], pLight);
}

/**Find all objects which need to be drawn in a special way because they are occluded by other
objects.
@todo:  Need some kind of scene subdivision or find way to use Partition manger to speed up the ray
intersection tests.  Maybe truncate the ray to terrain length before using it?
*/
void RTS3DScene::flagOccludedObjects(CameraClass * camera)
{
	Vector3 camPosition=camera->Get_Position();

	//Find which objects are actually occluded
	RenderObjClass **occludee=m_potentialOccludees;
	LineSegClass lineseg;
	CastResultStruct result;
	Bool hit=FALSE;
	Vector3 newEndPoint;
	result.ComputeContactPoint=false;
	RayCollisionTestClass raytest(lineseg,&result,COLL_TYPE_ALL,false,false);
	raytest.CollisionType=COLL_TYPE_ALL;

	m_occludedObjectsCount=0;

	for (Int i=0; i<m_numPotentialOccludees; i++,occludee++)
	{
		raytest.Ray.Set(camPosition,(*occludee)->Get_Position());

		RenderObjClass **occluder=m_potentialOccluders;

		//Check this object against all other possible blocking objects
		for (Int j=0; j<m_numPotentialOccluders; j++,occluder++)
		{
			// Do a quick ray-sphere test (Graphics Gems I,  p388)
			RenderObjClass *robj=*occluder;

			const SphereClass *sphere = &robj->Get_Bounding_Sphere();

			// make a vector from the ray origin to the sphere center
			Vector3 sphere_vector(sphere->Center - raytest.Ray.Get_P0());

			// get the dot product between the sphere_vector and the ray vector
			Real Alpha = Vector3::Dot_Product(sphere_vector, raytest.Ray.Get_Dir());

			Real Beta = sphere->Radius * sphere->Radius - (Vector3::Dot_Product(sphere_vector, sphere_vector) - Alpha * Alpha);

			if(Beta < 0.0f)
				continue;	//no intersection

			//Do a more accurate test against object geometry
			if (robj->Cast_Ray(raytest))
			{
				//Found an object closer than last closest object
				//Adjust our results and refine search
				raytest.CollidedRenderObj = robj;
				hit=TRUE;
				//reset the result space for next test
				result.StartBad = false;
				result.Fraction = 1.0f;
				break;
			}
		}
		if (hit)
		{
			//occludee was blocked by something so flag it for custom rendering
			DrawableInfo *drawInfo=(DrawableInfo *)(*occludee)->Get_User_Data();
			drawInfo->m_flags |= DrawableInfo::ERF_IS_OCCLUDED;
			m_potentialOccludees[m_occludedObjectsCount++] = *occludee;
		}
	}
}

//=============================================================================
// RTS3DScene::castRay
//=============================================================================
/** Does a ray intersection test against objects in our scene.
	By default, it only tests objects determined to be visible to the user.
	Setting testAll forces it to test all objects in the scene.
	CollisionType is used as a mask to ignore certain types of objects.
 */
//=============================================================================
Bool RTS3DScene::castRay(RayCollisionTestClass & raytest, Bool testAll, Int collisionType)
{
// this shouldn't be necessary here, and would be an undesirable performance hit.
// if you ever add or modify code here, it MIGHT become necessary... so do so with caution. (srj)
//#ifdef DIRTY_CONDITION_FLAGS
//	StDrawableDirtyStuffLocker lockDirtyStuff;
//#endif

	//temporary results for each object tested
	CastResultStruct result;
	RayCollisionTestClass tempRayTest(raytest.Ray,&result);
	Vector3 newEndPoint;
	Bool hit=FALSE;

	tempRayTest.CollisionType = COLL_TYPE_ALL;
	//check if a mesh is translucent before colliding with it. Skips headlights, etc.
	tempRayTest.CheckTranslucent = true;

	RefRenderObjListIterator it(&RenderList);

	// select the first object
	it.First();

	while (!it.Is_Done())
	{
		// get the render object
		RenderObjClass * robj = it.Peek_Obj();
		it.Next();

		// only intersect if it was visible or if we must test all
		if(robj->Get_Collision_Type() & collisionType && (testAll || robj->Is_Really_Visible()))
		{
			// Do a quick ray-sphere test (Graphics Gems I,  p388)
			const SphereClass *sphere = &robj->Get_Bounding_Sphere();

			// make a vector from the ray origin to the sphere center
			Vector3 sphere_vector(sphere->Center - tempRayTest.Ray.Get_P0());

			// get the dot product between the sphere_vector and the ray vector
			Real Alpha = Vector3::Dot_Product(sphere_vector, tempRayTest.Ray.Get_Dir());

			Real Beta = sphere->Radius * sphere->Radius - (Vector3::Dot_Product(sphere_vector, sphere_vector) - Alpha * Alpha);

			if(Beta < 0.0f)
				continue;	//no intersection

			//Do a more accurate test against object geometry
			if (robj->Cast_Ray(tempRayTest))
			{
				//Found an object closer than last closest object
				//Adjust our results and refine search
				raytest.CollidedRenderObj = robj;
				hit=TRUE;
				//Refine search by making ray shorter
				tempRayTest.Ray.Compute_Point(tempRayTest.Result->Fraction,&newEndPoint);
				tempRayTest.Ray.Set(raytest.Ray.Get_P0(),newEndPoint);
				//intersection point is at the end of shortened ray, so adjust fraction to 1.0
				tempRayTest.Result->Fraction=1.0f;
			}
		}
	}

	//Store results of ray intersection test including a clipped ray that ends on object.
	raytest.Ray=tempRayTest.Ray;
	return hit;
}

//=============================================================================
// GeneralsX: [GXDRAWCHECK] (GO). On 14/10 wh1ter0se saw two
// destroyed oil derricks drawn standing in a live 0.9.64 game. The game had them dead, and no replay or live test of ours
// reproduced it. So the client now watches for the two ways a building can be drawn in a state the game does not have,
// and says so once in the log:
//   ghost-clear: a fog-of-war ghost clone (a render object with a ghost and no drawable, W3DGhostObject's snapshot) is
//                in the scene while its object is CLEAR to the viewed player. That is fogged memory drawn over a
//                building he can see.
//   rubble-model: a DESTROYED structure (RUBBLE) has a draw module holding the render object of another
//                model than the one its condition flags select (W3DModelDrawModuleData::findBestInfo): a dead building
//                drawn standing. Only destroyed ones: a living structure legitimately holds other models (build-up
//                scaffolds, GLA construction models, door animations - 32 such holds in the first test run).
// Both are checked where the culling loop of Visibility_Check meets them, right after the dirty lock has flushed every
// drawable: ghost-clear for every ghost clone in the scene, rubble-model only for a model the loop has just found VISIBLE
// (drawn this frame). W3DModelDraw finishes a pending transition only while it draws, so an off-screen structure may
// legitimately still hold its old model; and a dead building drawn standing matters where it is drawn.
// A report needs the same finding on consecutive passes, with its first and latest sightings GXDC_HOLD_FRAMES apart (5 s
// at 60 Hz) and frames rendered in between, so a transition, an allow-to-finish animation or a flush still pending never
// reports; a real leak stays for good.
// COST (the first version cost up to 0.99 ms on one frame every 2 s): a pass starts every GXDC_INTERVAL logic
// frames and is spread over 8-64 rendered frames; each frame checks one slice (a hash of the ghost object or the drawable
// id, so an item is in the same slice on every pass) and adds no walk of its own - the culling loop walks the scene
// anyway. The slice count follows the last pass's item count, about GXDC_ITEMS_PER_SLICE items per frame. Nothing but ids
// and values is kept across frames. The checks only read: no game, drawable or scene state is written.
// CLIENT ONLY: this file is not in the SimID digest, and nothing in the simulation reads anything here. -headless never
// renders, so it never runs there.
// GX_DRAWCHECK=0 turns it off. GX_DRAWCHECK_STATS=1 prints the cost per frame and per pass. GX_TEST_DRAWCHECK_PLANT plants
// each case (test only, see gxDrawCheckPlant).
//=============================================================================
static const UnsignedInt GXDC_INTERVAL = 120;	// logic frames from one pass start to the next: 2 s at 60 Hz
static const Int GXDC_MIN_SLICES = 8;			// a pass takes at least 8 rendered frames...
static const Int GXDC_MAX_SLICES = 64;			// ...and at most 64
static const Int GXDC_ITEMS_PER_SLICE = 48;		// ghost clones + destroyed-structure models per frame the slice count aims at
static const size_t GXDC_MAX_FINDINGS = 64;		// per pass: a flood of findings never grows the work
static const UnsignedInt GXDC_HOLD_FRAMES = 300;	// logic frames from a finding's first to its latest sighting before a report
static const UnsignedInt GXDC_JUMP_FRAMES = 240;	// a logic jump this far between two rendered frames starts clean (fast forward ~20)
static const double GXDC_GAP_SECONDS = 1.0;		// so does this long without a 3D render: a save load's loading screen
static const Int GXDC_MAX_LINES = 32;			// per match, then one "suppressed" line

enum GxDcKind { GXDC_GHOST_CLEAR = 0, GXDC_MODEL = 1 };
struct GxDcFinding
{
	Int kind;
	ObjectID id;			// INVALID_ID for an orphan ghost (its object is gone)
	uintptr_t ghost;		// ghost-clear: the ghost object's address as a value (never dereferenced)
	Int x, y;				// ghost-clear: the clone's place - with the address it tells apart orphans that reuse a ghost object
	Int module;
	AsciiString sig;
	UnsignedInt first;		// logic frame of the first sighting, carried over while it holds
	UnsignedInt last;		// logic frame of this sighting
	Bool same(const GxDcFinding &o) const
	{
		return kind == o.kind && id == o.id && ghost == o.ghost && x == o.x && y == o.y && module == o.module && sig == o.sig;
	}
};
struct GxDcKey
{
	Int kind;
	ObjectID id;
	uintptr_t ghost;
	Int x, y;
	Bool operator==(const GxDcKey &o) const { return kind == o.kind && id == o.id && ghost == o.ghost && x == o.x && y == o.y; }
};

static Bool gxDrawCheckOn()
{
	static const Bool s_on = [] { const char *v = getenv("GX_DRAWCHECK"); return !(v != nullptr && v[0] == '0'); }();
	return s_on;
}

static Bool gxDrawCheckStats()
{
	static const Bool s_on = [] { const char *v = getenv("GX_DRAWCHECK_STATS"); return v != nullptr && v[0] == '1'; }();
	return s_on;
}

static AsciiString gxDcFlags(const ModelConditionFlags &c)
{
	AsciiString d, flat;
	c.buildDescription(&d);
	for (const char *p = d.str(); *p != '\0'; ++p)
		if (*p != '\n')
			flat.concat(*p);
	return flat;
}

/// The module data a draw module was built from, found by its tag in the drawable's template (Module::getModuleData is
/// protected). Only asked for modules that are W3DModelDraws, so the data is a W3DModelDrawModuleData.
static const W3DModelDrawModuleData *gxDcModuleData(const Drawable *draw, NameKeyType tag)
{
	const ModuleInfo &mi = draw->getTemplate()->getDrawModuleInfo();
	for (Int i = 0; i < mi.getCount(); ++i)
	{
		const ModuleData *md = mi.getNthData(i);
		if (md != nullptr && md->getModuleTagNameKey() == tag)
			return (const W3DModelDrawModuleData *)md;
	}
	return nullptr;
}

/// TEST ONLY: GX_TEST_DRAWCHECK_PLANT="<a|b>:<frame>:<object id>,..." puts each case in on purpose, so a test can show the
/// line fires. a = take the viewed player's fog snapshot of an object he sees CLEAR. Its clone goes into the scene, which
/// is exactly the ghost-clear case. Since the ghost-leak fix (PartitionData::getShroudedStatus frees a visible object's
/// snapshot whatever its previous status) the next recompute of that object's status frees it, so the plant holds only
/// while nothing invalidates that status (a structure whose cells see no vision change for the 5 s hold).
/// b = set one draw module of the object to the state of other condition flags, without touching the drawable's flags.
/// That is the model case. Runs before the dirty lock: both reach getDrawModules.
static void gxDrawCheckPlant()
{
	static Int s_n = -1;
	static char s_what[4];
	static UnsignedInt s_frame[4];
	static Int s_id[4];
	static Bool s_done[4];
	if (s_n < 0)
	{
		s_n = 0;
		const char *v = getenv("GX_TEST_DRAWCHECK_PLANT");
		char w = 0; unsigned f = 0; int id = 0, used = 0;
		while (v != nullptr && s_n < 4 && sscanf(v, "%c:%u:%d%n", &w, &f, &id, &used) == 3)
		{
			s_what[s_n] = w; s_frame[s_n] = f; s_id[s_n] = id; s_done[s_n] = FALSE; ++s_n;
			v += used;
			if (*v != ',')
				break;
			++v;
		}
	}
	if (s_n == 0 || TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGhostObjectManager == nullptr)
		return;
	for (Int i = 0; i < s_n; ++i)
	{
		if (s_done[i] || TheGameLogic->getFrame() < s_frame[i])
			continue;
		s_done[i] = TRUE;
		Object *obj = TheGameLogic->findObjectByID((ObjectID)s_id[i]);
		Drawable *draw = obj ? obj->getDrawable() : nullptr;
		const Int local = TheGhostObjectManager->getLocalPlayerIndex();
		AsciiString what("nothing (no such object)");
		if (draw != nullptr && s_what[i] == 'a')
		{
			PartitionData *pd = obj->friend_getPartitionData();
			GhostObject *ghost = pd ? pd->getGhostObject() : nullptr;
			Bool hasClone = FALSE;
			if (ghost != nullptr && W3DDisplay::m_3DScene != nullptr)
			{
				SceneIterator *sit = W3DDisplay::m_3DScene->Create_Iterator();
				for (sit->First(); !sit->Is_Done() && !hasClone; sit->Next())
				{
					const DrawableInfo *ci = (const DrawableInfo *)sit->Current_Item()->Get_User_Data();
					hasClone = ci != nullptr && ci->m_drawable == nullptr && ci->m_ghostObject == ghost;
				}
				W3DDisplay::m_3DScene->Destroy_Iterator(sit);
			}
			if (hasClone)
				what = "nothing (a clone of it is already in the scene)";
			else if (ghost != nullptr && pd->friend_getShroudednessPrevious(local) == OBJECTSHROUD_CLEAR)
			{
				ghost->snapShot(local);
				what.format("snapshot for player %d while CLEAR", local);
			}
			else
				what = ghost ? "nothing (not CLEAR to the viewed player)" : "nothing (no ghost object)";
		}
		else if (draw != nullptr && s_what[i] == 'b')
		{
			static const ModelConditionFlagType s_toggle[] = { MODELCONDITION_REALLY_DAMAGED, MODELCONDITION_DAMAGED, MODELCONDITION_RUBBLE };
			what = "nothing (no module with another model)";
			for (DrawModule **dm = draw->getDrawModules(); *dm && what.startsWith("nothing"); ++dm)
			{
				ObjectDrawInterface *odi = (*dm)->getObjectDrawInterface();
				const W3DModelDrawModuleData *md = gxDcModuleData(draw, (*dm)->getModuleTagNameKey());
				if (odi == nullptr || md == nullptr)
					continue;
				const ModelConditionInfo *now = md->findBestInfo(draw->getModelConditionFlags());
				for (Int t = 0; t < (Int)ARRAY_SIZE(s_toggle); ++t)
				{
					ModelConditionFlags other = draw->getModelConditionFlags();
					other.set(s_toggle[t], !other.test(s_toggle[t]));
					const ModelConditionInfo *alt = md->findBestInfo(other);
					if (now != nullptr && alt != nullptr && !alt->m_modelName.isEmpty() && alt->m_modelName.compareNoCase(now->m_modelName) != 0)
					{
						odi->replaceModelConditionState(other);
						what.format("module state of [%s]", gxDcFlags(other).str());
						break;
					}
				}
			}
		}
		fprintf(stderr, "[GXDRAWCHECK] plant %c f=%u id=%d: %s\n", s_what[i], TheGameLogic->getFrame(), s_id[i], what.str());
		fflush(stderr);
	}
}

/// The state of the pass under way. Values and ids only: no pointer to an object, drawable, ghost or render object.
struct GxDcState
{
	std::vector<GxDcFinding> cur;		// this pass's findings so far
	std::vector<GxDcFinding> prev;		// the last finished pass's
	std::vector<GxDcKey> reported;		// each object (each orphan ghost) once per kind per match: at most GXDC_MAX_LINES + 1
	Bool seen = FALSE;		// a call in a match has happened
	Bool left = FALSE;		// out of a match since (menu, shell map, loading a map)
	Bool started = FALSE;	// a pass has started in this match
	Bool active = FALSE;	// a pass is under way
	Bool havePrev = FALSE;	// prev holds a finished pass of this match
	Int slice = 0;			// the next slice of this pass
	Int slices = GXDC_MIN_SLICES;
	Int local = 0;			// the viewed player, read every frame
	UnsignedInt passStart = 0, lastFrame = 0, lastRender = 0, prevEndRenders = 0;
	std::chrono::steady_clock::time_point lastCall;
	Int lines = 0;
	Int clones = 0, models = 0;	// checked by this pass: sizes the next one, and the stats
	// GX_DRAWCHECK_STATS=1: the cost of every frame that did work, and of every whole pass
	Bool frameOpen = FALSE;
	UnsignedInt frames = 0, passes = 0, resets = 0, over100 = 0;	// over100: working frames that cost more than 100 us
	double frameUs = 0, passUs = 0, sumFrameUs = 0, maxFrameUs = 0, windowMaxFrameUs = 0, sumPassUs = 0;
};
static GxDcState s_dc;

/// GeneralsX: W3DDisplay::reset calls this, and GameClient::reset runs that for every new match,
/// every loaded save (GameState::loadGame -> GameLogic::clearGameData -> GameEngine::reset) and every return to the menu.
/// A save loaded from the in-game menu renders no scene in between, so the culling loop alone cannot see the change when
/// the loaded frame is later than the last one: the match's report budget and reported-once list then carried over and
/// could hide a finding of the new match. Now the next rendered frame starts the match clean.
void gxDrawCheckReset()
{
	s_dc.left = TRUE;
}

/// the slice an item is in: a hash of a stable key (a ghost object's address, a drawable id), the same on every pass
static Int gxDcSlot(uintptr_t key)
{
	return (Int)((((unsigned long long)key * 0x9E3779B97F4A7C15ull) >> 32) % (unsigned long long)s_dc.slices);
}

/// ghost-clear for one ghost clone (a render object with a ghost and no drawable) met by the culling loop
static void gxDrawCheckClone(const RenderObjClass *robj, const DrawableInfo *info, Int slice)
{
	if (gxDcSlot((uintptr_t)info->m_ghostObject) != slice)
		return;
	const Bool stats = gxDrawCheckStats();
	std::chrono::steady_clock::time_point t0;
	if (stats)
		t0 = std::chrono::steady_clock::now();
	++s_dc.clones;
	PartitionData *pd = info->m_ghostObject->friend_getPartitionData();
	// an orphan (its object is gone) is fogged memory too, and W3DGhostObjectManager::updateOrphanedObjects keeps its
	// status current for the viewed player: one still in the scene once that is CLEAR is the same leak
	const ObjectShroudStatus ss = pd ? pd->friend_getShroudednessPrevious(s_dc.local) : OBJECTSHROUD_INVALID;
	if ((ss == OBJECTSHROUD_CLEAR || ss == OBJECTSHROUD_PARTIAL_CLEAR) && s_dc.cur.size() < GXDC_MAX_FINDINGS)
	{
		const Object *obj = pd->getObject();
		const Vector3 at = robj->Get_Position();
		GxDcFinding f;
		f.kind = GXDC_GHOST_CLEAR; f.id = obj ? obj->getID() : INVALID_ID; f.ghost = (uintptr_t)info->m_ghostObject;
		f.x = (Int)at.X; f.y = (Int)at.Y; f.module = 0;
		f.first = f.last = TheGameLogic->getFrame();
		// CLEAR and PARTIAL_CLEAR both qualify: the status is left out of the signature so a clone that alternates between
		// them still holds
		f.sig.format("clone=%s at=%d,%d", robj->Get_Name(), f.x, f.y);
		s_dc.cur.push_back(f);
	}
	if (stats)
		s_dc.frameUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

/// rubble-model for one model the culling loop has just found visible (it is drawn this frame)
static void gxDrawCheckModel(RenderObjClass *robj, Drawable *draw, Int slice)
{
	if (!draw->getModelConditionFlags().test(MODELCONDITION_RUBBLE) || gxDcSlot((uintptr_t)(UnsignedInt)draw->getID()) != slice)
		return;	// a living structure's state is not final (construction stages, transitions): destroyed ones only
	const Object *obj = draw->getObject();
	if (obj == nullptr || !obj->isKindOf(KINDOF_STRUCTURE))
		return;
	const Bool stats = gxDrawCheckStats();
	std::chrono::steady_clock::time_point t0;
	if (stats)
		t0 = std::chrono::steady_clock::now();
	++s_dc.models;
	Int m = 0;
	for (DrawModule **dm = draw->getDrawModulesNonDirty(); *dm; ++dm, ++m)
	{
		ObjectDrawInterface *odi = (*dm)->getObjectDrawInterface();
		// as W3DGhostObject does: every ObjectDrawInterface on a drawable is a W3DModelDraw
		if (odi == nullptr || ((W3DModelDraw *)odi)->getRenderObject() != robj)
			continue;	// another module's model
		const W3DModelDrawModuleData *md = gxDcModuleData(draw, (*dm)->getModuleTagNameKey());
		const ModelConditionInfo *best = md ? md->findBestInfo(draw->getModelConditionFlags()) : nullptr;
		if (best != nullptr && best->m_modelName.compareNoCase(robj->Get_Name()) != 0 && s_dc.cur.size() < GXDC_MAX_FINDINGS)
		{
			GxDcFinding f;
			f.kind = GXDC_MODEL; f.id = obj->getID(); f.ghost = 0; f.x = 0; f.y = 0; f.module = m;
			f.first = f.last = TheGameLogic->getFrame();
			f.sig.format("shows=%s wants=%s cond=%s", robj->Get_Name(),
				best->m_modelName.isEmpty() ? "NONE" : best->m_modelName.str(), gxDcFlags(draw->getModelConditionFlags()).str());
			s_dc.cur.push_back(f);
		}
		break;
	}
	if (stats)
		s_dc.frameUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

/// stats: the last working frame's checks are done by the next call; add its cost up
static void gxDrawCheckCloseFrame()
{
	if (!s_dc.frameOpen)
		return;
	s_dc.frameOpen = FALSE;
	++s_dc.frames;
	s_dc.passUs += s_dc.frameUs;
	s_dc.sumFrameUs += s_dc.frameUs;
	if (s_dc.frameUs > s_dc.maxFrameUs)
		s_dc.maxFrameUs = s_dc.frameUs;
	if (s_dc.frameUs > s_dc.windowMaxFrameUs)
		s_dc.windowMaxFrameUs = s_dc.frameUs;
	if (s_dc.frameUs > 100.0)
		++s_dc.over100;
	s_dc.frameUs = 0;
}

/// a pass is complete: carry each finding's first sighting over from the last pass when it held there too, and report what
/// has held GXDC_HOLD_FRAMES from its first to its latest sighting, with frames rendered in between
static void gxDrawCheckEndPass(UnsignedInt frame)
{
	const UnsignedInt renders = (UnsignedInt)WW3D::Get_Frame_Count();
	const Bool rendered = s_dc.havePrev && renders >= s_dc.prevEndRenders + 2;
	for (size_t i = 0; i < s_dc.cur.size(); ++i)
	{
		GxDcFinding &f = s_dc.cur[i];
		Bool held = FALSE;
		for (size_t j = 0; j < s_dc.prev.size() && !held; ++j)
			if (s_dc.prev[j].same(f))
			{
				held = TRUE;
				f.first = s_dc.prev[j].first;
			}
		if (!held || !rendered || f.last < f.first + GXDC_HOLD_FRAMES || s_dc.lines > GXDC_MAX_LINES)
			continue;	// past the line budget nothing more is recorded: the history stays bounded
		const GxDcKey key = { f.kind, f.id, f.ghost, f.x, f.y };
		if (std::find(s_dc.reported.begin(), s_dc.reported.end(), key) != s_dc.reported.end())
			continue;
		s_dc.reported.push_back(key);
		if (s_dc.lines < GXDC_MAX_LINES)
		{
			const Object *obj = TheGameLogic->findObjectByID(f.id);
			fprintf(stderr, "[GXDRAWCHECK] f=%u kind=%s id=%u tmpl=%s viewer=%d module=%d since=%u %s\n", frame,
				f.kind == GXDC_GHOST_CLEAR ? "ghost-clear" : "rubble-model", (unsigned)f.id,
				obj ? obj->getTemplate()->getName().str() : "?", s_dc.local, f.module, f.first, f.sig.str());
		}
		else
			fprintf(stderr, "[GXDRAWCHECK] f=%u further reports suppressed for this match\n", frame);
		++s_dc.lines;
		fflush(stderr);
	}
	s_dc.prev.swap(s_dc.cur);
	s_dc.cur.clear();
	s_dc.havePrev = TRUE;
	s_dc.prevEndRenders = renders;
	s_dc.active = FALSE;
	++s_dc.passes;
}

/// stats line every 15 passes; the pass's cost is complete once its last working frame is closed
static void gxDrawCheckPassStats(UnsignedInt frame)
{
	s_dc.sumPassUs += s_dc.passUs;
	if (gxDrawCheckStats() && s_dc.passes > 0 && s_dc.passes % 15 == 0)
	{
		fprintf(stderr, "[GXDRAWCHECK] stats f=%u passes=%u frames=%u frame_mean_us=%.1f frame_max_us=%.1f frame_max15_us=%.1f "
			"pass_mean_us=%.1f last_pass_us=%.1f clones=%d models=%d slices=%d resets=%u frames_over_100us=%u\n",
			frame, s_dc.passes, s_dc.frames, s_dc.frames ? s_dc.sumFrameUs / s_dc.frames : 0.0, s_dc.maxFrameUs,
			s_dc.windowMaxFrameUs, s_dc.sumPassUs / s_dc.passes, s_dc.passUs, s_dc.clones, s_dc.models, s_dc.slices, s_dc.resets, s_dc.over100);
		fflush(stderr);
		s_dc.windowMaxFrameUs = 0;
	}
	s_dc.passUs = 0;
}

/// Once per rendered frame, from Visibility_Check after the dirty lock: ends and starts passes and returns the slice the
/// culling loop checks this frame (-1 = none).
static Int gxDrawCheckFrame()
{
	if (!gxDrawCheckOn())
		return -1;
	gxDrawCheckCloseFrame();
	if (TheGameLogic == nullptr || TheGameClient == nullptr || TheGhostObjectManager == nullptr)
		return -1;
	if (!TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->isLoadingMap())
	{
		s_dc.left = TRUE;	// the next match starts clean
		return -1;
	}
	if (ShaderClass::Is_Backface_Culling_Inverted())
		return -1;	// the reflection pass
	const UnsignedInt renders = (UnsignedInt)WW3D::Get_Frame_Count();
	if (s_dc.seen && renders == s_dc.lastRender)
		return -1;	// one slice per rendered frame (wireframe mode runs the visibility check twice)
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const UnsignedInt frame = TheGameLogic->getFrame();
	const Bool gap = s_dc.seen && std::chrono::duration<double>(now - s_dc.lastCall).count() > GXDC_GAP_SECONDS;
	const Bool back = s_dc.seen && frame < s_dc.lastFrame;
	if (s_dc.seen && (s_dc.left || gap || back || frame > s_dc.lastFrame + GXDC_JUMP_FRAMES))
	{	// a new match, or a save loaded (its loading screen renders no scene, so left is never set): forget the last one
		s_dc.cur.clear(); s_dc.prev.clear(); ++s_dc.resets;
		s_dc.started = FALSE; s_dc.active = FALSE; s_dc.havePrev = FALSE; s_dc.passUs = 0;
		// GeneralsX: The report budget and the reported-once list reset only for another match
		// (left: an out-of-match frame, or gxDrawCheckReset from W3DDisplay::reset - a new match or any loaded save) or an
		// earlier game state. A render gap (a minimised window, a suspended iPad) or a forward jump (fast forward) only
		// breaks pass continuity: resetting them there let one persistent defect report again after every gap, past the
		// 32 + 1 lines per match.
		if (s_dc.left || back)
		{
			s_dc.reported.clear();
			s_dc.lines = 0;
		}
	}
	s_dc.seen = TRUE;
	s_dc.left = FALSE;
	s_dc.lastFrame = frame;
	s_dc.lastRender = renders;
	s_dc.lastCall = now;

	Bool work = FALSE;
	double statsPrintUs = 0;	// the stats line is the instrument's own output: its print is not the detector's cost
	if (s_dc.active && s_dc.slice >= s_dc.slices)
	{
		gxDrawCheckEndPass(frame);
		work = TRUE;
	}
	if (!s_dc.active && (!s_dc.started || frame >= s_dc.passStart + GXDC_INTERVAL))
	{
		if (s_dc.started)
		{
			// GeneralsX: Measured, not just timed: the stats print (an fprintf + fflush, a pipe write
			// into the stderr log on Windows) was inside this frame's timed span, so every 15th pass reported ~120-270 us that
			// a player - who never sets GX_DRAWCHECK_STATS - never pays. Its time is left out.
			const std::chrono::steady_clock::time_point p0 = std::chrono::steady_clock::now();
			gxDrawCheckPassStats(frame);
			if (gxDrawCheckStats())
				statsPrintUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - p0).count();
		}
		const Int want = (s_dc.clones + s_dc.models + GXDC_ITEMS_PER_SLICE - 1) / GXDC_ITEMS_PER_SLICE;	// the last pass's items
		s_dc.slices = want < GXDC_MIN_SLICES ? GXDC_MIN_SLICES : (want > GXDC_MAX_SLICES ? GXDC_MAX_SLICES : want);
		s_dc.started = TRUE;
		s_dc.active = TRUE;
		s_dc.passStart = frame;
		s_dc.slice = 0;
		s_dc.clones = 0;
		s_dc.models = 0;
	}
	Int slice = -1;
	if (s_dc.active)
	{
		s_dc.local = TheGhostObjectManager->getLocalPlayerIndex();
		slice = s_dc.slice++;
		work = TRUE;
	}
	if (work)
	{
		s_dc.frameOpen = TRUE;
		if (gxDrawCheckStats())
			s_dc.frameUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - now).count() - statsPrintUs;
	}
	return slice;
}

//=============================================================================
// RTS3DScene::Visibility_Check
//=============================================================================
/** Custom visibility check method for the RTS3DScene, we can put optimized
  * culling methods in here */
//=============================================================================
void RTS3DScene::Visibility_Check(CameraClass * camera)
{
	gxDrawCheckPlant();	// GeneralsX @diagnostic [GXDRAWCHECK] test plants: before the lock, they reach getDrawModules
#ifdef DIRTY_CONDITION_FLAGS
	StDrawableDirtyStuffLocker lockDirtyStuff;
#endif
	const Int gxDcSlice = gxDrawCheckFrame();	// GeneralsX @diagnostic [GXDRAWCHECK]: every drawable is flushed here

	RefRenderObjListIterator it(&RenderList);
	DrawableInfo *drawInfo = nullptr;
	Drawable	*draw = nullptr;
	RenderObjClass * robj;

	m_numPotentialOccluders=0;
	m_numPotentialOccludees=0;
	m_translucentObjectsCount=0;
	m_numNonOccluderOrOccludee=0;

	Int currentFrame = TheGameLogic ? TheGameLogic->getFrame() : 0;
	if (currentFrame <= TheGlobalData->m_defaultOcclusionDelay)
		currentFrame = TheGlobalData->m_defaultOcclusionDelay+1;	//make sure occlusion is enabled when game starts (frame 0).

	if (ShaderClass::Is_Backface_Culling_Inverted())
	{
		//we are rendering reflections
		///@todo: Have better flag to detect reflection pass

		// Loop over all top-level RenderObjects in this scene. If the bounding sphere is not in front
		// of all the frustum planes, it is invisible.
		for (it.First(); !it.Is_Done(); it.Next()) {

			robj = it.Peek_Obj();

			draw=nullptr;
			drawInfo = (DrawableInfo *)robj->Get_User_Data();
			if (drawInfo)
				draw=drawInfo->m_drawable;

			if( draw )
			{
				if (robj->Is_Force_Visible()) {
					robj->Set_Visible(true);
				} else {
					robj->Set_Visible(draw->getDrawsInMirror() && !camera->Cull_Sphere(robj->Get_Bounding_Sphere()));
				}
			}
			else
			{
				//perform normal culling on non-drawables
				if (robj->Is_Force_Visible()) {
					robj->Set_Visible(true);
				} else {
					robj->Set_Visible(!camera->Cull_Sphere(robj->Get_Bounding_Sphere()));
				}
			}
		}
	}
	else
	{

		// Loop over all top-level RenderObjects in this scene. If the bounding sphere is not in front
		// of all the frustum planes, it is invisible.
		for (it.First(); !it.Is_Done(); it.Next()) {

			robj = it.Peek_Obj();

			if (gxDcSlice >= 0)	// GeneralsX @diagnostic [GXDRAWCHECK]: this frame's slice of the ghost clones, read only
			{
				const DrawableInfo *gxDcInfo = (const DrawableInfo *)robj->Get_User_Data();
				if (gxDcInfo != nullptr && gxDcInfo->m_drawable == nullptr && gxDcInfo->m_ghostObject != nullptr)
					gxDrawCheckClone(robj, gxDcInfo, gxDcSlice);
			}

			if (robj->Is_Force_Visible()) {
				robj->Set_Visible(true);
			} else if (robj->Is_Hidden()) {
				robj->Set_Visible(false);
			} else {

				bool isVisible=!camera->Cull_Sphere(robj->Get_Bounding_Sphere());

				if (isVisible)
				{
					//need to keep track of occluders and occludees for subsequent code.
					drawInfo = (DrawableInfo *)robj->Get_User_Data();
					if (drawInfo && (draw=drawInfo->m_drawable) != nullptr)
					{
						if (draw->isDrawableEffectivelyHidden() || draw->getFullyObscuredByShroud())
						{
							isVisible = FALSE;
						  robj->Set_Visible(isVisible);
            }
						//assume normal rendering.
						drawInfo->m_flags = DrawableInfo::ERF_IS_NORMAL;	//clear any rendering flags that may be in effect.

            if ( ! isVisible )
              continue;

						if (gxDcSlice >= 0)	// GeneralsX @diagnostic [GXDRAWCHECK]: a visible model of this frame's slice, read only
							gxDrawCheckModel(robj, draw, gxDcSlice);

						if (draw->getEffectiveOpacity() != 1.0f && m_translucentObjectsCount < TheGlobalData->m_maxVisibleTranslucentObjects)
						{
							drawInfo->m_flags |= DrawableInfo::ERF_IS_TRANSLUCENT;
							m_translucentObjectsBuffer[m_translucentObjectsCount++] = robj;
						}
						if (TheGlobalData->m_enableBehindBuildingMarkers && TheGameLogic && TheGameLogic->getShowBehindBuildingMarkers())
						{
							const Bool isTranslucent = (drawInfo->m_flags & DrawableInfo::ERF_IS_TRANSLUCENT) != 0;
							//visible drawable. Check if it's either an occluder or occludee
							if (draw->isKindOf(KINDOF_STRUCTURE) && m_numPotentialOccluders < TheGlobalData->m_maxVisibleOccluderObjects)
							{
								//object which could occlude other objects that need to be visible.
								//Make sure this object is not translucent so it's not rendered twice (from m_potentialOccluders and m_translucentObjectsBuffer)
								if (!isTranslucent)
									m_potentialOccluders[m_numPotentialOccluders++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_POTENTIAL_OCCLUDER;
							}
							else if (draw->getObject() &&
									(draw->isKindOf(KINDOF_SCORE) || draw->isKindOf(KINDOF_SCORE_CREATE) || draw->isKindOf(KINDOF_SCORE_DESTROY) || draw->isKindOf(KINDOF_MP_COUNT_FOR_VICTORY)) &&
									(draw->getObject()->getSafeOcclusionFrame()) <= currentFrame && m_numPotentialOccludees < TheGlobalData->m_maxVisibleOccludeeObjects)
							{
								//object which could be occluded but still needs to be visible.
								//We process translucent units twice (also in m_translucentObjectsBuffer) because we need to see them when occluded.
								m_potentialOccludees[m_numPotentialOccludees++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_POTENTIAL_OCCLUDEE;
							}
							else if (drawInfo->m_flags == DrawableInfo::ERF_IS_NORMAL && m_numNonOccluderOrOccludee < TheGlobalData->m_maxVisibleNonOccluderOrOccludeeObjects)
							{
								//regular object with no custom effects but still needs to be delayed to get the occlusion feature to work correctly.
								//Make sure this object is not translucent so it's not rendered twice (from m_nonOccludersOrOccludees and m_translucentObjectsBuffer)
								if (!isTranslucent)
									m_nonOccludersOrOccludees[m_numNonOccluderOrOccludee++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_IS_NON_OCCLUDER_OR_OCCLUDEE;
							}
						}
					}
				}

				robj->Set_Visible(isVisible);
			}

			///@todo: We're not using LOD yet so I disabled this code. MW
			// Also, should check how multiple passes (reflections) get along
			// with the LOD manager - we're rendering double the load it thinks we are.
			// Prepare visible objects for LOD:
			//	if (robj->Is_Really_Visible()) {
			//			robj->Prepare_LOD(*camera);
			//		}
		}
	}

   Visibility_Checked = true;
}

//============================================================================
// RTS3DScene::renderSingleDrawable
//=============================================================================
/** Renders a single drawable entity. */
//=============================================================================
void RTS3DScene::renderSpecificDrawables(RenderInfoClass &rinfo, Int numDrawable, Drawable **theDrawables)
{
#ifdef DIRTY_CONDITION_FLAGS
	StDrawableDirtyStuffLocker lockDirtyStuff;
#endif
	const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();
	RefRenderObjListIterator it(&UpdateList);
	// loop through all render objects in the list:
	for (it.First(&RenderList); !it.Is_Done();)
	{
		RenderObjClass *robj;
		// get the render object
		robj = it.Peek_Obj();

		it.Next();	//advance to next object in case this one gets deleted during renderOneObject().

		DrawableInfo *drawInfo = (DrawableInfo *)robj->Get_User_Data();
		Drawable *draw=nullptr;
		if (drawInfo)
			draw = drawInfo->m_drawable;
		if (!draw) continue;
		Bool match = false;
		for (Int i = 0; i<numDrawable; i++) {
			if (theDrawables[i] == draw) {
				match = true;
				break;
			}
		}
		if (match) {
			renderOneObject(rinfo, robj, localPlayerIndex);
		}
	}
}

// GeneralsX: [GXCENSUS] (GX_DRAWCENSUS=1, dx8wrapper.h DX8C_OBJ*/DX8C_MESH*): what
// renderOneObject renders, by bounding-sphere radius on screen (cumulative buckets under 16, 8 and 4 px), how many
// carry more than one LOD, and their meshes at the current LOD as a draws-per-pass proxy (roadmap 2e: draw less at
// extreme zoom). Off: one test of a static bool. Counts RENDERS: the occlusion passes render an object again, and the
// pixel size assumes the tactical view, so render-to-texture passes (shadow/water maps) land in the buckets approximately.
static void gxCensusObject(RenderInfoClass &rinfo, RenderObjClass *robj, Bool shrouded, Bool fogGhost = FALSE)
{
	if (!DX8Wrapper::CensusOn)
		return;
	const SphereClass &sph = robj->Get_Bounding_Sphere();
	const float d = (sph.Center - rinfo.Camera.Get_Position()).Length();
	const float halfH = 0.5f * (float)(TheTacticalView ? TheTacticalView->getHeight() : 0);
	const float t = tanf(0.5f * rinfo.Camera.Get_Vertical_FOV());
	const float px = (d > 1.0e-3f && t > 1.0e-6f) ? sph.Radius / d * halfH / t : 1.0e9f;
	// the meshes HLodClass::Render would draw: current LOD and additional models, minus hidden ones (upgrades,
	// muzzle flashes) and OBBOX helpers (rcpu r3)
	int meshes = 0;
	if (robj->Class_ID() == RenderObjClass::CLASSID_HLOD) {
		HLodClass *h = (HLodClass *)robj;
		const int lod = h->Get_LOD_Level();
		for (int i = 0; i < h->Get_Lod_Model_Count(lod); ++i) {
			RenderObjClass *m = h->Peek_Lod_Model(lod, i);
			if (m != nullptr && m->Class_ID() != RenderObjClass::CLASSID_OBBOX && m->Is_Not_Hidden_At_All())
				++meshes;
		}
		for (int i = 0; i < h->Get_Additional_Model_Count(); ++i) {
			RenderObjClass *m = h->Peek_Additional_Model(i);
			if (m != nullptr && m->Is_Not_Hidden_At_All())
				++meshes;
		}
	} else
		meshes = 1;
	unsigned long long *c = DX8Wrapper::Census;
	++c[DX8C_OBJ];
	c[DX8C_MESH] += (unsigned)meshes;
	if (shrouded)
		++c[DX8C_OBJ_SHROUD];
	if (fogGhost)
		++c[DX8C_OBJ_FOG];
	if (robj->Get_LOD_Count() > 1)
		++c[DX8C_OBJ_MULTILOD];
	if (px < 16.0f) { ++c[DX8C_OBJ_LT16]; c[DX8C_MESH_LT16] += (unsigned)meshes; }
	if (px < 8.0f) { ++c[DX8C_OBJ_LT8]; c[DX8C_MESH_LT8] += (unsigned)meshes; }
	if (px < 4.0f) { ++c[DX8C_OBJ_LT4]; c[DX8C_MESH_LT4] += (unsigned)meshes; }
}

//============================================================================
// RTS3DScene::renderOneObject
//=============================================================================
/** Renders a single drawable entity. */
//=============================================================================
void RTS3DScene::renderOneObject(RenderInfoClass &rinfo, RenderObjClass *robj, Int localPlayerIndex)
{
	GxFlushScope gxProf(GxFlushProf::SUBMIT);	// GX_FLUSHPROF: inside the object-flush bracket only
	GxObjPrep p;
	if (prepareOneObject(robj, localPlayerIndex, p))
		renderPreparedObject(rinfo, robj, p, FALSE);
}

// GeneralsX: R-CPU 1f: renderOneObject up to and including the shroud query, in the
// original order, plus every drawable value the lighting half reads. FALSE where renderOneObject returned unlit.
Bool RTS3DScene::prepareOneObject(RenderObjClass *robj, Int localPlayerIndex, GxObjPrep &p)
{
	Drawable *draw = nullptr;
	DrawableInfo *drawInfo = nullptr;
	Bool drawableHidden=FALSE;
	Object* obj = nullptr;
	ObjectShroudStatus ss=OBJECTSHROUD_INVALID;
	Bool ssFromObject = FALSE;	// ss came from an object's shroud query, not from "no object, assume clear"

	p.draw = nullptr;
	p.drawInfo = nullptr;
	p.tintColor = nullptr;
	p.selectionColor = nullptr;
	p.heatOpacity = 0.0f;
	p.stealthLook = 0;
	p.ss = OBJECTSHROUD_INVALID;
	p.drawableHidden = FALSE;
	p.infantry = FALSE;
	p.dynamicLights = FALSE;
	p.sphValid = FALSE;
	p.noSceneLights = FALSE;
	p.image3d = robj->Class_ID() == RenderObjClass::CLASSID_IMAGE3D;
	if (p.image3d)
		return TRUE;	// decals: renderPreparedObject renders them unlit

	// Where renderOneObject always asked for it: before the shroud query and every early return. It is not free of side
	// effects - an HLOD's bounding-box helper evaluates its motion, and a compressed motion's decode cache is shared and
	// order-dependent - so it is asked here, once, for every object, and renderPreparedObject reuses it.
	p.sph = robj->Get_Bounding_Sphere();
	p.sphValid = TRUE;

	drawInfo = (DrawableInfo *)robj->Get_User_Data();
	if (drawInfo)
	{
		draw = drawInfo->m_drawable;
		//If we have a drawInfo but not drawable, we must be dealing with
		//a ghost object which is always fogged.
		if (!draw)
			ss = OBJECTSHROUD_FOGGED;
	}

	if (draw && (drawableHidden=draw->isDrawableEffectivelyHidden()) != TRUE)
	{
		obj = draw->getObject();
		if (obj) {
			ss = obj->getShroudedStatus(localPlayerIndex);
			ssFromObject = TRUE;
			// For objects like planes, that pop out of the shroud, fire, then head back,
			// we keep drawing them for 2 seconds after they return to the fogged area,
			// so the player can see them and missiles chasing them.  jba.
			if (ss == OBJECTSHROUD_CLEAR) {
				draw->setShroudClearFrame(TheGameLogic->getFrame());
			}	else if (ss >= OBJECTSHROUD_FOGGED && draw->getShroudClearFrame() != InvalidShroudClearFrame) {
				UnsignedInt limit = 2*LOGICFRAMES_PER_SECOND;
				if (obj->isEffectivelyDead()) {
					limit += 3*LOGICFRAMES_PER_SECOND;
				}
				if (TheGameLogic->getFrame() < limit + draw->getShroudClearFrame()) {
					// It's been less than 2 seconds since we could see them clear, so keep showing them.
					ss = OBJECTSHROUD_PARTIAL_CLEAR;
				}
			}
 			if (!robj->Peek_Scene())
 				return FALSE;	//this object was removed by the getShroudedStatus() call.
		}
		else
		{
			//drawable with no object so no way to know if it's shrouded.
			ss = OBJECTSHROUD_CLEAR;	//assume not shrouded/fogged.
			//Check to see if there is another unrelated object which controls the shroud status
			//(Hack for prison camps which contain enemy prisoner drawables)
			if (drawInfo->m_shroudStatusObjectID != INVALID_ID)
			{
				Object *shroudObject=TheGameLogic->findObjectByID(drawInfo->m_shroudStatusObjectID);
				if (shroudObject && shroudObject->getShroudedStatus(localPlayerIndex) >= OBJECTSHROUD_FOGGED)
					ss = OBJECTSHROUD_SHROUDED;	//we will assume that drawables without objects are 'particle' like and therefore don't need drawing if fogged/shrouded.
				ssFromObject = (shroudObject != nullptr);
			}
		}

		p.infantry = draw->isKindOf(KINDOF_INFANTRY);
		p.tintColor = draw->getTintColor();
		p.selectionColor = draw->getSelectionColor();
		p.heatOpacity = draw->getSecondMaterialPassOpacity();
		p.stealthLook = draw->getStealthLook();
		p.dynamicLights = draw->getReceivesDynamicLights();
	}
	else if (drawableHidden)
	{
		return FALSE;	//don't bother with anything else
	}

	// GeneralsX: Fog interpolation (W3DShroud::init): a logically CLEAR object over fog that is still
	// fading keeps the shroud pass, or it pops fully lit over still-dark ground. Set here, before
	// both the serial path (renderPreparedObject) and the collection workers' "plain" test read p.ss.
	// GeneralsX: Only a CLEAR that came from an object's shroud query. A drawable with no object -
	// such as the building ghost that follows the cursor while placing (InGameUI's m_placeIcon, newDrawable with no object) -
	// is CLEAR by assumption above and retail never gives it the shroud pass, fogged ground or not; the fade had it turn
	// darker only while vision was changing near it (tester report 21/10, ghost at the edge of vision).
	W3DShroud *fadeShroud = TheTerrainRenderObject ? TheTerrainRenderObject->getShroud() : nullptr;
	if (ss == OBJECTSHROUD_CLEAR && ssFromObject && fadeShroud && fadeShroud->isFading())
	{
		// A mesh's cached sphere keeps its unscaled radius while W3DModelDraw scales its render transform, so scale the radius by
		// the transform's largest axis (and the object scale) - conservative for objects whose sphere already includes it
		//
		const Matrix3D &tm = robj->Get_Transform();
		Real scale = tm.Get_X_Vector().Length();
		const Real sy = tm.Get_Y_Vector().Length();
		const Real sz = tm.Get_Z_Vector().Length();
		if (sy > scale) scale = sy;
		if (sz > scale) scale = sz;
		if (robj->Get_ObjectScale() > scale) scale = robj->Get_ObjectScale();
		SphereClass footprint = p.sph;
		if (scale > 1.0f)
			footprint.Radius *= scale;
		if (fadeShroud->isFadingUnder(footprint))
			ss = OBJECTSHROUD_PARTIAL_CLEAR;
	}

	p.draw = draw;
	p.drawInfo = drawInfo;
	p.drawableHidden = drawableHidden;
	p.ss = ss;
	return TRUE;
}

// GeneralsX: R-CPU 1f: HLodClass::Render (hlod.cpp, digested) on a collection worker, minus
// its first call, Animatable3DObjClass::Render - the pose keep, which counts into the shared PoseStats - recorded as a
// POSE entry for the main thread to run in the same place. The walk itself is HLodClass::Render's line for line: the
// current LOD's models but its OBBOX helpers, then the additional models (matched to this LOD when enabled). Keep the
// two in step. Meshes render for real; their shared adds land in the sink (dx8renderer.cpp).
static void gxCollectRenderTree(RenderObjClass *robj, RenderInfoClass &rinfo)
{
	if (robj->Class_ID() != RenderObjClass::CLASSID_HLOD)
	{
		robj->Render(rinfo);	// a mesh, or a null object (empty): gxTreeWorkerSafe admits nothing else
		return;
	}
	HLodClass *h = (HLodClass *)robj;
	if (h->Is_Not_Hidden_At_All() == false)
		return;
	GxCollectActiveSink->Push(GxCollectRec::POSE, h);
	const int lod = h->Get_LOD_Level();
	for (int i = 0; i < h->Get_Lod_Model_Count(lod); i++)
	{
		RenderObjClass *m = h->Peek_Lod_Model(lod, i);
		if (m->Class_ID() != RenderObjClass::CLASSID_OBBOX)	///We have no use for these - MW
			gxCollectRenderTree(m, rinfo);
	}
	if (h->Is_Sub_Objects_Match_LOD_Enabled())
	{
		for (int i = 0; i < h->Get_Additional_Model_Count(); i++)
		{
			RenderObjClass *m = h->Peek_Additional_Model(i);
			m->Set_LOD_Level(h->Get_LOD_Level());
			gxCollectRenderTree(m, rinfo);
		}
	}
	else
	{
		for (int i = 0; i < h->Get_Additional_Model_Count(); i++)
			gxCollectRenderTree(h->Peek_Additional_Model(i), rinfo);
	}
}

// GeneralsX: R-CPU 1f: renderOneObject after the shroud query. Reads the drawable only
// through p. onWorker (a collection worker; gxCollectBegin admits only plain-lit, unshrouded, default-pass objects with
// drawables): no census, and the tree walk is gxCollectRenderTree's.
void RTS3DScene::renderPreparedObject(RenderInfoClass &rinfo, RenderObjClass *robj, const GxObjPrep &p, Bool onWorker)
{
	Drawable *draw = p.draw;
	DrawableInfo *drawInfo = p.drawInfo;
	const Bool drawableHidden = p.drawableHidden;
	ObjectShroudStatus ss = (ObjectShroudStatus)p.ss;
	Bool doExtraMaterialPop=FALSE;
	Bool doExtraFlagsPop=FALSE;
	LightClass **sceneLights=m_globalLight;

	if (p.image3d)
	{
		robj->Render(rinfo);	//notify decals system that this track is visible
		return;	//decals are not lit by this system yet so skip rest of lighting
	}

	LightEnvironmentClass lightEnv;
	// GeneralsX: The plain scene lights are added to lightEnv lazily. An object lit
	// only by m_globalLight (no tint, no selection colour, not infantry, and no LightList or dynamic light
	// passing its cull) renders with the prebuilt m_defaultLightEnv instead of building an identical one.
	// See the comment where rinfo.light_environment is chosen below.
	Bool lightEnvHasSceneLights = FALSE;
	// Adds the plain scene lights exactly once, before any other light, so every Add_Light call keeps its original order.
	auto addPlainSceneLights = [&]()
	{
		if (lightEnvHasSceneLights)
			return;
		for (Int globalLightIndex = 0; globalLightIndex < m_numGlobalLights; globalLightIndex++)
			lightEnv.Add_Light(*m_globalLight[globalLightIndex]);
		lightEnvHasSceneLights = TRUE;
	};
	SphereClass sph = p.sphValid ? p.sph : robj->Get_Bounding_Sphere();

	// all this ambient business no longer handles the tinting and flashing stuff,
	// but it does still light the drawable explicitly, and can be fudged like this
	// infantry test does, here...
	// the tint has been delegated to the getTint() stuff, below... MLorenzen
	Vector3 ambient = Get_Ambient_Light();
	if (draw && drawableHidden != TRUE)
	{
#ifdef NOT_IN_USE
		const Vector3* drawAmbient = draw->getAmbientLight();
		if (drawAmbient)
			ambient.Add(ambient, *drawAmbient, &ambient);
#endif
		if (p.infantry)
		{
			//ambient = m_infantryAmbient;  //has no effect - see comment on m_infantryAmbient
			sceneLights = m_infantryLight;
		}

		lightEnv.Reset(sph.Center, ambient);


		// HANDLE THE SPECIAL DRAWABLE-LEVEL COLORING SETTINGS FIRST

		const Vector3 *tintColor = p.tintColor;
		const Vector3 *selectionColor = p.selectionColor;

		if ( tintColor || selectionColor )
		{
			Vector3 sumTint, temp, restore;

			sumTint.Set(0,0,0);

			if (tintColor)
				Vector3::Add(sumTint, *tintColor, &sumTint);
			if (selectionColor)
				Vector3::Add(sumTint, *selectionColor, &sumTint);

			for (Int globalLightIndex = 0; globalLightIndex < m_numGlobalLights; globalLightIndex++)
			{
				sceneLights[globalLightIndex]->Get_Diffuse( &temp );
				restore = temp;

				Vector3::Add(temp, sumTint, &temp);

				sceneLights[globalLightIndex]->Set_Diffuse( temp );
				lightEnv.Add_Light(*sceneLights[globalLightIndex]);
				sceneLights[globalLightIndex]->Set_Diffuse( restore );

			}

			temp = lightEnv.Get_Equivalent_Ambient();
			Vector3::Add(sumTint, temp, &temp );

			lightEnv.Set_Output_Ambient( temp );
			lightEnvHasSceneLights = TRUE;

		}
		else if (sceneLights != m_globalLight) // no funny coloring going on, so just add the lights normally
		{
			for (Int globalLightIndex = 0; globalLightIndex < m_numGlobalLights; globalLightIndex++)
			{
				lightEnv.Add_Light(*sceneLights[globalLightIndex]);
			}
			lightEnvHasSceneLights = TRUE;
		}
		// else: plain m_globalLight, added lazily below.

		//Apply custom render pass for any drawables with heatvision enabled
		if (p.heatOpacity != 0 )
		{
			rinfo.materialPassEmissiveOverride = p.heatOpacity;

      //if ( draw->testTintStatus( TINT_STATUS_FRENZY ) )
      //{
			//	rinfo.Push_Material_Pass(m_heatVisionMaterialPass);
      //}
			//else
      if (p.stealthLook == STEALTHLOOK_VISIBLE_DETECTED )
			{
			  rinfo.materialPassEmissiveOverride = p.heatOpacity;
				// THIS WILL EXPLICITLY SKIP THE FIRST PASS SO THAT HEATVISION ONLY WILL RENDER
				rinfo.Push_Override_Flags(RenderInfoClass::RINFO_OVERRIDE_ADDITIONAL_PASSES_ONLY);
				rinfo.Push_Material_Pass(m_heatVisionOnlyPass);
        doExtraFlagsPop = TRUE;
			}
			else
			{
				//THIS CALLS FOR THE HEATVISION TO RENDER
			  rinfo.materialPassEmissiveOverride = p.heatOpacity;
				rinfo.Push_Material_Pass(m_heatVisionMaterialPass);
			}

			doExtraMaterialPop = TRUE;
		}
	}
	else
	{
		//no drawable (a hidden one never gets here: prepareOneObject)

		//Render object without a drawable.  Must be either some fluff/debug object or a ghostObject.
		if (ss == OBJECTSHROUD_FOGGED)
		{
			//Must be ghost object because we don't fog normal things.  Fogged objects always have a predefined
			//lighting environment applied which emulates the look of fog.
			rinfo.light_environment = &m_foggedLightEnv;
			gxCensusObject(rinfo, robj, FALSE, TRUE);	// a fog ghost: counted, not a shroud material pass (rcpu r2)
			robj->Render(rinfo);
			rinfo.light_environment = nullptr;
			return;
		}
		else
		{
			lightEnv.Reset(sph.Center, ambient);
			// plain m_globalLight, added lazily below.
		}
	}

	if (!drawableHidden)
	{
		//standard scene lights
		if (!p.noSceneLights)	// a collection worker never reads LightList (adv3)
		{
			RefRenderObjListIterator it2(&LightList);
			for (it2.First(); !it2.Is_Done(); it2.Next())
			{
				LightClass *pLight = (LightClass*)it2.Peek_Obj();
				SphereClass lSph = pLight->Get_Bounding_Sphere();
				Bool cull = (pLight->Get_Type() == LightClass::POINT && !Spheres_Intersect(sph, lSph));
				if (!cull) {
					addPlainSceneLights();
					lightEnv.Add_Light(*pLight);
				}
			}
		}

    if( p.dynamicLights )
    {
		  // dynamic lights
		  auto addDynamicLight = [&](W3DDynamicLight *pDyna)
		  {
			  SphereClass lSph = pDyna->Get_Bounding_Sphere();
			  if (pDyna->Get_Type() == LightClass::POINT && !Spheres_Intersect(sph, lSph)) {
				  return;
			  }
			  addPlainSceneLights();
			  lightEnv.Add_Light(*(LightClass*)pDyna);
		  };
		  // GeneralsX: Inside a pass, the enabled lights in list order (Customized_Render).
		  if (m_enabledDynamicLightsValid)
		  {
			  for (size_t k = 0; k < m_enabledDynamicLights.size(); ++k)
				  addDynamicLight(m_enabledDynamicLights[k]);
		  }
		  else
		  {
			  RefRenderObjListIterator dynaLightIt(&m_dynamicLightList);
			  for (dynaLightIt.First(); !dynaLightIt.Is_Done(); dynaLightIt.Next())
			  {
				  W3DDynamicLight* pDyna = (W3DDynamicLight*)dynaLightIt.Peek_Obj();
				  if (pDyna->isEnabled())
					  addDynamicLight(pDyna);
			  }
		  }
    }

		// An environment of only the plain scene lights is exactly what updateFixedLightEnvironments built
		// into m_defaultLightEnv at the start of this Render pass: the same Add_Light calls on the same lights
		// in the same order, from the same ambient (compared here). It differs only in ObjectCenter, which a
		// directional light never reads (Init_From_Directional_Light), and in the camera given to
		// Pre_Render_Update, which only fills OutputLights; the sole consumer, DX8Wrapper::Set_Light_Environment,
		// reads the light count, Get_Equivalent_Ambient and the InputLights, never OutputLights or ObjectCenter.
		if (!lightEnvHasSceneLights && m_defaultLightEnvValid && ambient == m_defaultLightEnvAmbient)
		{
			rinfo.light_environment = &m_defaultLightEnv;
		}
		else
		{
			addPlainSceneLights();
			lightEnv.Pre_Render_Update(rinfo.Camera.Get_Transform());
			rinfo.light_environment = &lightEnv;
		}

		if (drawInfo)
		{
#if ENABLE_CONFIGURABLE_SHROUD
			if (!gxShroudDrawn())
				ss = OBJECTSHROUD_CLEAR;
#endif

			if (m_customPassMode == SCENE_PASS_DEFAULT)
			{
				if (!onWorker)
					gxCensusObject(rinfo, robj, ss > OBJECTSHROUD_CLEAR);
				if (ss <= OBJECTSHROUD_CLEAR)
				{
					if (onWorker)
						gxCollectRenderTree(robj, rinfo);
					else
						robj->Render(rinfo);
				}
				else
				{
					rinfo.Push_Material_Pass(m_shroudMaterialPass);
					robj->Render(rinfo);
					rinfo.Pop_Material_Pass();
				}
			}
			else if (m_maskMaterialPass)
			{
				rinfo.Push_Material_Pass(m_maskMaterialPass);
				rinfo.Push_Override_Flags(RenderInfoClass::RINFO_OVERRIDE_ADDITIONAL_PASSES_ONLY);
				gxCensusObject(rinfo, robj, FALSE);	// the alpha-mask pass renders too (rcpu r3)
				robj->Render(rinfo);
				rinfo.Pop_Override_Flags();
				rinfo.Pop_Material_Pass();
			}
		}
		else
		{
			gxCensusObject(rinfo, robj, FALSE);
			robj->Render(rinfo);
		}
	}

	rinfo.light_environment = nullptr;
	if (doExtraMaterialPop)	//check if there is an extra material on the stack from the added material effect.
		rinfo.Pop_Material_Pass();
	if (doExtraFlagsPop)
		rinfo.Pop_Override_Flags();	//flags used to disable base pass and only render custom heat vision pass.
}

//DECLARE_PERF_TIMER(translucentRender)

/**Draw everything that was submitted from this scene*/
void RTS3DScene::Flush(RenderInfoClass & rinfo)
{
	gxGfxSubStart();	// [GXGFXF]: Flush can run without Customized_Render before it
	// GeneralsX: WS-G: the L2 contract's pass context - the same test as DoShadows' below
	GxL2Dry_SetPass(m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE);
	// TheSuperHackers @bugfix Now always prepares shadows to guarantee correct state before doing any
	// shadow draw calls. Originally just drawing shadows for trees would not properly prepare shadows.
	PrepareShadows();

	//don't draw shadows in this mode because they interfere with destination alpha or are invisible (wireframe)
	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
		DoShadows(rinfo, false);	//draw all non-stencil shadows (decals) since they fall under other objects.
	gxGfxSubMark(GXSUB_DECAL);

	// GeneralsX: d3d8 call ring spike (DX8Ring, GX_D3D8RING): the object flushes are
	// recorded and replayed by the render thread while this thread collects the next player's objects; End_Record
	// returns once all of it has reached the device. A no-op when GX_D3D8RING is unset.
	DX8Ring::Begin_Record();
	GxFlushSiteNext = GXFS_MAIN_OPAQUE;	// GeneralsX: WS-G flush sites (dx8renderer.h GxFlushSite)
	GxFlushProf::InBracket = GxFlushProf::On;
	const long long gxProfT0 = GxFlushProf::On ? GxFlushProf::Now() : 0;
	TheDX8MeshRenderer.Flush();	//draw all non-translucent objects.
	gxGfxSubMark(GXSUB_OPAQUE);

	//draw all non-translucent objects which were separated because they are hidden and need custom rendering.
#ifdef USE_NON_STENCIL_OCCLUSION
	flushOccludedObjects(rinfo);
#else
	if (DX8Wrapper::Has_Stencil())
		flushOccludedObjectsIntoStencil(rinfo);
#endif
	DX8Ring::End_Record();
	if (GxFlushProf::On) { GxFlushProf::Ns[GxFlushProf::TOTAL] += GxFlushProf::Now() - gxProfT0; GxFlushProf::InBracket = false; }	// Frame() at Present
	gxGfxSubMark(GXSUB_OCCL);

	// (gth) CNC3 Flush the shader meshes
	SHD_FLUSH;
	gxGfxSubMark(GXSUB_SHD);

	// Draw the trees last so they alpha blend onto everything correctly.
	DoTrees(rinfo);
	gxGfxSubMark(GXSUB_TREES);

	//don't draw shadows in this mode because they interfere with destination alpha
	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
		DoShadows(rinfo, true);	//draw all stencil shadows
	gxGfxSubMark(GXSUB_VOL);

	// GeneralsX: F-depth+ (GxSceneTap.h): the opaque world is complete - terrain, objects,
	// trees, shadows - and water and every transparent follow. When a native reader is on, the device copies the depth and the
	// opaque colour here for it; otherwise (and on classic) this is one test.
	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
		W3DWorldSeam::opaqueDone();

	WW3D::Render_And_Clear_Static_Sort_Lists(rinfo);	//draws things like water
	gxGfxSubMark(GXSUB_STATIC);

	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
		flushTranslucentObjects(rinfo);	//draw all translucent meshes which don't need per-polygon sorting.
	gxGfxSubMark(GXSUB_TRANSL);

	{
		//USE_PERF_TIMER(translucentRender)

		//don't draw transparent in this mode because they interfere with destination alpha
		if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
			DoParticles(rinfo);	//queue up particles for rendering.
		gxGfxSubMark(GXSUB_PARTQ);

		SortingRendererClass::Flush();	//draw sorted translucent polygons like particles.
		gxGfxSubMark(GXSUB_SORT);
	}
	TheDX8MeshRenderer.Clear_Pending_Delete_Lists();
	GxL2Dry_SetPass(false);	// WS-G: a flush outside RTS3DScene::Flush is not the plain scene pass
	m_enabledDynamicLightsValid = FALSE;	// see Customized_Render
}

/**Generate a predefined light environment(s) that will be applied to many objects.  Useful for things like totally fogged
objects and most generic map objects that are not lit by dynamic lights.*/
void RTS3DScene::updateFixedLightEnvironments(RenderInfoClass & rinfo)
{
	//Figure out how dimly lit fogged objects should be compared to fully lit.
	Real foggedLightFrac = (Real)TheGlobalData->m_fogAlpha/(Real)TheGlobalData->m_clearAlpha;
	Real infantryLightScale;
	if( TheGlobalData->m_scriptOverrideInfantryLightScale != -1.0f )
		infantryLightScale = TheGlobalData->m_scriptOverrideInfantryLightScale;
	else
		infantryLightScale = TheGlobalData->m_infantryLightScale[TheGlobalData->m_timeOfDay];

	//Generate the default light environment
	m_defaultLightEnvAmbient = Get_Ambient_Light();
	m_defaultLightEnv.Reset(Vector3(0,0,0), m_defaultLightEnvAmbient);
	m_foggedLightEnv.Reset(Vector3(0,0,0), Get_Ambient_Light()*foggedLightFrac);

	Vector3 oldDiffuse, oldAmbient;
	for (Int globalLightIndex = 0; globalLightIndex < m_numGlobalLights; globalLightIndex++)
	{
		m_defaultLightEnv.Add_Light(*m_globalLight[globalLightIndex]);
		//copy default lighting for infantry so we can tweak it.
		*m_infantryLight[globalLightIndex]=*m_globalLight[globalLightIndex];
		m_infantryLight[globalLightIndex]->Set_Transform(m_globalLight[globalLightIndex]->Get_Transform());

		m_globalLight[globalLightIndex]->Get_Diffuse(&oldDiffuse);
		m_globalLight[globalLightIndex]->Get_Ambient(&oldAmbient);
    oldDiffuse *= infantryLightScale;
    oldAmbient *= infantryLightScale;
    static Vector3 id (1.0f, 1.0f, 1.0f);
    oldDiffuse.Cap_Absolute_To(id);
    oldAmbient.Cap_Absolute_To(id);
		m_infantryLight[globalLightIndex]->Set_Ambient(oldAmbient);//CLAMPED
		m_infantryLight[globalLightIndex]->Set_Diffuse(oldDiffuse);//CLAMPED

		//copy the normal light for fog so we can modify it
		m_scratchLight->Set_Transform(m_globalLight[globalLightIndex]->Get_Transform());
		//modify light with attenuated value to adjust for fog.
		m_globalLight[globalLightIndex]->Get_Diffuse(&oldDiffuse);
		m_scratchLight->Set_Diffuse(oldDiffuse*foggedLightFrac);
		m_globalLight[globalLightIndex]->Get_Ambient(&oldAmbient);
		m_scratchLight->Set_Ambient(oldAmbient*foggedLightFrac);
		m_foggedLightEnv.Add_Light(*m_scratchLight);
	}

	m_defaultLightEnv.Pre_Render_Update(rinfo.Camera.Get_Transform());
	m_foggedLightEnv.Pre_Render_Update(rinfo.Camera.Get_Transform());

	m_infantryAmbient = Get_Ambient_Light();// * infantryLightScale;	//for now don't adjust ambient so that we don't lose directional lighting.
}

/**Generate custom rendering passes for each potential player color.  This is currently only used
to render occluded objects using the color of the player*/
void RTS3DScene::updatePlayerColorPasses()
{
#ifdef USE_NON_STENCIL_OCCLUSION
	Vector3 hsv,rgb;

	if (TheGlobalData->m_enableBehindBuildingMarkers && TheGameLogic->getShowBehindBuildingMarkers())
	{
		Int numPlayers=ThePlayerList->getPlayerCount();

		for (Int i=0; i<numPlayers; i++)
		{	Player *player=ThePlayerList->getNthPlayer(i);
			Int playerIndex=player->getPlayerIndex();
			Real red,green,blue,alpha;
			GameGetColorComponentsReal(player->getPlayerColor(),&red,&green,&blue,&alpha);
			RGB_To_HSV(hsv,Vector3(red,green,blue));
			hsv.Z*=TheGlobalData->m_occludedLuminanceScale;
			HSV_To_RGB(rgb,hsv);
			VertexMaterialClass *vmat=m_occludedMaterialPass[playerIndex]->Peek_Material();
			vmat->Set_Emissive(rgb);
		}
	}
#endif
}

#define ZBias 0.0001f

//DECLARE_PERF_TIMER(NonTerrainRender)
void RTS3DScene::Render(RenderInfoClass & rinfo)
{
	//USE_PERF_TIMER(NonTerrainRender)
	DX8Wrapper::Set_Fog(FogEnabled, FogColor, FogStart, FogEnd);

	//Override the behind building selection if it's not available on current hardware (needs stencil).
	TheWritableGlobalData->m_enableBehindBuildingMarkers = TheWritableGlobalData->m_enableBehindBuildingMarkers && DX8Wrapper::Has_Stencil();

	if (Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
	{
		if (m_customPassMode == SCENE_PASS_DEFAULT)
		{
			//Regular rendering pass with no effects
			updatePlayerColorPasses();///@todo: this probably doesn't need to be done each frame.
			updateFixedLightEnvironments(rinfo);
			m_defaultLightEnvValid = TRUE;	// only for the objects of this pass; see renderOneObject
			Customized_Render(rinfo);
			Flush(rinfo);
			m_defaultLightEnvValid = FALSE;
		}
		else if (m_customPassMode == SCENE_PASS_ALPHA_MASK)
		{
			//a projected alpha texture which will later be used to determine where
			//wireframe should be visible.
			///@todo: Clearing to black may not be needed if the scene already did the clear.
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA);
			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 0);
			//Since all objects will be rendered with same material, disable resetting until all are done.
			m_maskMaterialPass->setAllowUninstall(FALSE);

			Customized_Render(rinfo);	//render mask into alpha channel and fill z-buffer with depth values.
			Flush(rinfo);
			m_maskMaterialPass->setAllowUninstall(TRUE);
			m_maskMaterialPass->UnInstall_Materials();

			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED);

			ShaderClass::Invalidate();
		}
	}
	else
	{
		Bool old_enable=WW3D::Is_Texturing_Enabled();
		if (Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_CLEAR_LINE)
		{
			//render scene with solid black color but have destination alpha store
			//a projected alpha texture which will later be used to determine where
			//wireframe should be visible.
			///@todo: Clearing to black may not be needed if the scene already did the clear.
			Renderer::Clear(true, false, Vector3(0.0f,0.0f,0.0f),1.0f);	// Clear color but not z
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA);
			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 0);

			//We're only filling the z-buffer so ignore normal textures and state changes to speed things up.
			m_customPassMode = SCENE_PASS_ALPHA_MASK;
			m_maskMaterialPass->setAllowUninstall(FALSE);

			Customized_Render(rinfo);	//render mask into alpha channel and fill z-buffer with depth values.
			Flush(rinfo);

			m_maskMaterialPass->setAllowUninstall(TRUE);
			m_maskMaterialPass->UnInstall_Materials();

			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED);
			WW3D::Enable_Coloring(0xff008000);
			WW3D::Enable_Texturing(false);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_FILLMODE,D3DFILL_WIREFRAME);

			//Move maximum z-buffer value in a little to shift all z-values closer
			//and thus forcing line to appear on top of previous pass.
			Real nearZ,farZ;
			rinfo.Camera.Get_Zbuffer_Range(nearZ, farZ);
			rinfo.Camera.Set_Zbuffer_Range(nearZ, farZ-ZBias);
			rinfo.Camera.Apply();

//			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 4);
			Customized_Render(rinfo);	//render wireframe where z-test passes
			Flush(rinfo);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_FILLMODE,D3DFILL_SOLID);

			rinfo.Camera.Set_Zbuffer_Range(nearZ, farZ);
			rinfo.Camera.Apply();

//			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 0);
			WW3D::Enable_Texturing(old_enable);
			WW3D::Enable_Coloring(0);

			ShaderClass::Invalidate();
		}
		else
		{
			//old W3D custom rendering code.

			//Disable writes to color buffer to save memory bandwidth - we only need Z.
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,0);
			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 0);
			Customized_Render(rinfo);
			Flush(rinfo);
			//Re-enable writes to color buffer.
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED);

			switch (Get_Extra_Pass_Polygon_Mode()) {
			case EXTRA_PASS_LINE:
				WW3D::Enable_Texturing(false);
				DX8Wrapper::Set_DX8_Render_State(D3DRS_FILLMODE,D3DFILL_WIREFRAME);
				DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 7);
				Customized_Render(rinfo);
				break;
			case EXTRA_PASS_CLEAR_LINE:
				Renderer::Clear(true, false, Vector3(0.0f,0.0f,0.0f), 0.0f);	// Clear color but not z
				WW3D::Enable_Texturing(false);
				WW3D::Enable_Coloring(0xff008000);
				DX8Wrapper::Set_DX8_Render_State(D3DRS_FILLMODE,D3DFILL_WIREFRAME);
				DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 7);
				Customized_Render(rinfo);
				break;
			}
			Flush(rinfo);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_FILLMODE,D3DFILL_SOLID);
			DX8Wrapper::Set_DX8_Render_State (D3DRS_ZBIAS, 0);
			WW3D::Enable_Texturing(old_enable);
			WW3D::Enable_Coloring(0);
			ShaderClass::Invalidate();
		}
	}
}

//=============================================================================
// RTS3DScene::Customized_Renderer
//=============================================================================
/** Custom render method for the RTS3DScene, custom render properties for our
  * particular game go here */
//=============================================================================
// GeneralsX: R-CPU pose pre-pass (modernize/render-split). Rebuilding
// skeletal poses was ~8% of the main thread in a running 6v6 (Mini profile, HLodClass::Update_Sub_Object_Transforms
// inclusive), one object at a time inside Render. Here, after visibility and every On_Frame_Update and before anything
// renders, each visible ROOT HLOD whose pose Render would rebuild now (Animatable3DObjClass::Pose_Needs_Build: the same
// tests; HLodClass::Render calls Animatable3DObjClass::Render unconditionally - keep the two in step) is rebuilt on the
// GXParallel workers instead; Render then finds the pose current and keeps it (pose cache,
// Keep_Current_Pose). Exact: the same function on the same object at the same WW3D clock reading, which only steps in
// W3DDisplay::draw before the views draw; a job poses one root tree (its LODs, additional models and their own
// sub-objects) and nothing else. Trees with a compressed motion, embedded sounds or a sub-object class outside the
// whitelist (HLodClass::Sub_Object_Is_Worker_Safe) stay with Render on the main thread (Pose_Is_Worker_Safe).
// GX_POSEPAR=0 turns it off; GX_POSECACHE=2 checks every pre-built pose against a rebuild.
static int gxPosePrepassMode()
{
	static const int s_mode = [] {
		const char *v = getenv("GX_POSEPAR");
		return (v == nullptr || *v == 0) ? 1 : atoi(v);
	}();
	return s_mode;
}

// The checks run in the jobs too (review of the first cut: on the main thread they cost about half of what the workers
// saved). Both only read the job's own tree. Counts go through two atomics, one add per chunk.
static std::atomic<unsigned> s_posePrebuilt{ 0 };
static std::atomic<unsigned> s_poseMainOnly{ 0 };

static void gxBuildPoses(void *ctx, int begin, int end)
{
	HLodClass **objs = static_cast<HLodClass **>(ctx);
	unsigned built = 0, mainOnly = 0;
	for (int i = begin; i < end; ++i)
	{
		HLodClass *hlod = objs[i];
		if (!hlod->Pose_Needs_Build())
			continue;
		if (!hlod->Pose_Is_Worker_Safe())
		{
			++mainOnly;	// Render rebuilds it on the main thread, as before
			continue;
		}
		hlod->Build_Pose();
		++built;
	}
	s_posePrebuilt.fetch_add(built, std::memory_order_relaxed);
	s_poseMainOnly.fetch_add(mainOnly, std::memory_order_relaxed);
}

static void gxPrebuildPoses(RefRenderObjListClass &renderList)
{
	if (gxPosePrepassMode() == 0 || Animatable3DObjClass::Pose_Cache_Mode() == 0)
		return;
	const auto t0 = std::chrono::steady_clock::now();
	static std::vector<HLodClass *> s_jobs;
	s_jobs.clear();
	RefRenderObjListIterator it(&renderList);
	for (it.First(); !it.Is_Done(); it.Next())
	{
		RenderObjClass *robj = it.Peek_Obj();
		if (robj->Class_ID() == RenderObjClass::CLASSID_HLOD && robj->Is_Really_Visible() && robj->Get_Container() == nullptr)
			s_jobs.push_back(static_cast<HLodClass *>(robj));
	}
	const auto t1 = std::chrono::steady_clock::now();
	s_posePrebuilt.store(0, std::memory_order_relaxed);
	s_poseMainOnly.store(0, std::memory_order_relaxed);
	gxParallelFor((int)s_jobs.size(), 16, gxBuildPoses, s_jobs.data());
	const auto t2 = std::chrono::steady_clock::now();
	Animatable3DObjClass::PoseStatsStruct &st = Animatable3DObjClass::PoseStats;
	st.prebuilt += s_posePrebuilt.load(std::memory_order_relaxed);
	st.mainOnly += s_poseMainOnly.load(std::memory_order_relaxed);
	st.prepassCollectNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
	st.prepassRunNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
}

// GeneralsX: A depth-only floor under the whole world, drawn right after the
// terrain: nothing below FLOOR_Z can show. The 2003 animations hide parts by parking their bones kilometres under the ground -
// the GLA Worker's gun and supply sack sit at z -1261..-8197 (UIWRKR_IDA/RNA/TRA2...), and 70+ skeletons do the same
// (Rebels, angry mobs, hackers, tunnel defenders, Black Lotus...). Inside the map the terrain hides them from any camera; but a
// camera far enough out to see past the map's near edge (Debug camera heights, the whole-map spectator view) looks under the
// map, and they showed there as small specks moving in the black void as the workers walked.
// Depth only (colour writes off), so the void keeps its clear colour, fog never touches it, and nothing above FLOOR_Z changes:
// terrain heights are unsigned bytes * MAP_HEIGHT_SCALE, so no ground is ever below 0. Client-only, render-only.
// GX_FLOOR_Z (TEST-ONLY, read once, for picture A/Bs): "off" = no floor, a number = that height (a fail arm sets one above the
// ground to prove a comparison sees a change); unset = -100.
static void gxDrawUnderworldFloor( RenderInfoClass &rinfo )
{
	static Bool s_read = FALSE;
	static Bool s_on = TRUE;
	static Real FLOOR_Z = -100.0f;
	if (!s_read)
	{
		s_read = TRUE;
		const char *env = getenv("GX_FLOOR_Z");
		if (env != nullptr && *env != '\0')
		{
			if (strcmp(env, "off") == 0)
				s_on = FALSE;
			else
				FLOOR_Z = (Real)atof(env);
			fprintf(stderr, "[GXFLOOR] GX_FLOOR_Z='%s' (TEST-ONLY): %s z=%.1f\n", env, s_on ? "on" : "off", FLOOR_Z);
		}
	}
	// Depth only needs colour writes off; a device that cannot (no D3DPMISCCAPS_COLORWRITEENABLE) gets no floor.
	if (!s_on || !(DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps().PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE))
		return;
	const Real HALF = 200000.0f;	// far past any far clip plane, centred under the camera
	const Vector3 cam = rinfo.Camera.Get_Position();

	ShaderClass shader = ShaderClass::_PresetOpaque2DShader;
	shader.Set_Depth_Compare(ShaderClass::PASS_LEQUAL);
	shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_ENABLE);
	shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
	DX8Wrapper::Set_Shader(shader);
	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Set_Texture(0, nullptr);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, Matrix3D(true));
	// Put back what the DEVICE holds, as the occluded-object pass does: the wrapper's cache can hold the 0x12345678 "unknown"
	// sentinel after Invalidate_Cached_Render_States (terrain shader paths), and restoring that would switch colour off
	// Not recording here (the ring records only inside Flush), so this device read costs no drain.
	DWORD colorWrites = D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_RED;
	DX8Wrapper::_Get_D3D_Device8()->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrites);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0);
	{
		DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, 4);
		DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, 6);
		Bool locked = FALSE;
		{
			DynamicVBAccessClass::WriteLockClass lock(&vb_access);
			VertexFormatXYZNDUV2 *vb = lock.Get_Formatted_Vertex_Array();
			DynamicIBAccessClass::WriteLockClass lockib(&ib_access);
			UnsignedShort *ib = lockib.Get_Index_Array();
			if (vb != nullptr && ib != nullptr)
			{
				const Real xs[4] = { cam.X - HALF, cam.X + HALF, cam.X + HALF, cam.X - HALF };
				const Real ys[4] = { cam.Y - HALF, cam.Y - HALF, cam.Y + HALF, cam.Y + HALF };
				for (Int i = 0; i < 4; ++i)
				{
					vb[i].x = xs[i]; vb[i].y = ys[i]; vb[i].z = FLOOR_Z;
					vb[i].nx = 0.0f; vb[i].ny = 0.0f; vb[i].nz = 1.0f;
					vb[i].diffuse = 0; vb[i].u1 = 0.0f; vb[i].v1 = 0.0f; vb[i].u2 = 0.0f; vb[i].v2 = 0.0f;
				}
				ib[0] = 0; ib[1] = 1; ib[2] = 2; ib[3] = 0; ib[4] = 2; ib[5] = 3;
				locked = TRUE;
			}
		}
		if (locked)
		{
			DX8Wrapper::Set_Index_Buffer(ib_access, 0);
			DX8Wrapper::Set_Vertex_Buffer(vb_access);
			DX8Wrapper::Draw_Triangles(0, 2, 0, 4);
		}
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, colorWrites);
	ShaderClass::Invalidate();
}

void RTS3DScene::Customized_Render( RenderInfoClass &rinfo )
{
#ifdef DIRTY_CONDITION_FLAGS
	StDrawableDirtyStuffLocker lockDirtyStuff;
#endif

	RenderObjClass *terrainObject=nullptr,*robj;
	m_translucentObjectsCount = 0;	//start of new frame so no translucent objects
	m_occludedObjectsCount = 0;
	gxGfxSubStart();	// [GXGFXF] scene sub-spans (GameClient/GXGfxProbe.h)

	const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();

#define USE_LIGHT_ENV 1

   if (!Visibility_Checked) {
      // set the visibility bit in all render objects in all layers.
	   Visibility_Check(&rinfo.Camera);
#ifdef USE_NON_STENCIL_OCCLUSION
	   flagOccludedObjects(&rinfo.Camera);
#endif
   }
   Visibility_Checked = false;
	gxGfxSubMark(GXSUB_VIS);


	RefRenderObjListIterator it(&UpdateList);
	// allow all objects in the update list to do their "every frame" processing
	for (it.First(); !it.Is_Done(); it.Next()) {
		RenderObjClass * robj = it.Peek_Obj();
		if (robj->Class_ID() == RenderObjClass::CLASSID_TILEMAP)
			terrainObject=robj;	//found terrain object, store for later.
		if (!ShaderClass::Is_Backface_Culling_Inverted()) {
			// If we are doing water mirror, we draw with backface culling inverted.  In this case,
			// we only want to call On_Frame_Update if we aren't drawing water, as otherwise
			// we get 2 frame updates per frame, and it screws up the particle emitters.
			it.Peek_Obj()->On_Frame_Update();
		}
	}
	// GeneralsX: renderOneObject walked the whole dynamic light pool for every object it
	// lights, and the pool keeps every light pulse ever made (a spent one is only disabled, for reuse), so a battle
	// paid objects x pool_size list steps and isEnabled() loads - the hottest lines of renderOneObject in the Mini
	// profile. A light's enabled state changes in the On_Frame_Update calls just above (W3DDynamicLight's decay) and
	// outside rendering (getADynamicLight: light pulses, police-car draw modules, which run before the scene renders),
	// so from here to the end of this pass's Flush the enabled lights, in list order, are exactly these.
	m_enabledDynamicLights.clear();
	{
		RefRenderObjListIterator dynaLightIt(&m_dynamicLightList);
		for (dynaLightIt.First(); !dynaLightIt.Is_Done(); dynaLightIt.Next())
		{
			W3DDynamicLight *pDyna = (W3DDynamicLight *)dynaLightIt.Peek_Obj();
			if (pDyna->isEnabled())
				m_enabledDynamicLights.push_back(pDyna);
		}
	}
	m_enabledDynamicLightsValid = TRUE;
	gxGfxSubMark(GXSUB_FUPD);

	// GeneralsX: S7 PREREG A49/A50: the shadow map's frame starts here - after every On_Frame_Update,
	// before gxPrebuildPoses and any object - under gxPrebuildPoses' own guard (the plain scene pass: never the wireframe / extra / mirrored
	// passes) and only for the camera W3DView armed. A null test unless armed. Its time is charged to [GXGFXF] TERR (census line says so).
	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE
		&& !ShaderClass::Is_Backface_Culling_Inverted())
		GxShadowMapFireArmed(&rinfo.Camera, this);

	// GeneralsX: R-CPU pose pre-pass, see above. Only in the plain scene pass: the alpha-mask, extra-polygon and
	// mirrored (water reflection) passes render objects whose poses the plain pass has already made current, and a
	// pose they still need is rebuilt by Render as before (the walk is not free).
	if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE
		&& !ShaderClass::Is_Backface_Culling_Inverted())
		gxPrebuildPoses(RenderList);

	//terrain needs to be rendered first
	if (terrainObject)	// Don't check visibility - terrain is always visible. jba.
	{
		robj=terrainObject;
		rinfo.light_environment = nullptr;		// Terrain is self lit.
		rinfo.Camera.Set_User_Data(this);	//pass the scene to terrain via user data.
		if (m_customPassMode == SCENE_PASS_DEFAULT && m_shroudMaterialPass)
		{
			rinfo.Push_Material_Pass(m_shroudMaterialPass);
			robj->Render(rinfo);
			rinfo.Pop_Material_Pass();
		}
		else if (m_customPassMode == SCENE_PASS_ALPHA_MASK && m_maskMaterialPass)
		{
			rinfo.Push_Material_Pass(m_maskMaterialPass);
			robj->Render(rinfo);
			rinfo.Pop_Material_Pass();
		}
		else
		robj->Render(rinfo);
		// The normal pass only: not the mask, wireframe or mirrored (water reflection) passes.
		if (m_customPassMode == SCENE_PASS_DEFAULT && Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE
			&& !ShaderClass::Is_Backface_Culling_Inverted())
			gxDrawUnderworldFloor(rinfo);
	}
	gxGfxSubMark(GXSUB_TERR);

	if (m_drawTerrainOnly) {
		return;
	}
#ifdef EXTENDED_STATS
	if (DX8Wrapper::stats.m_disableObjects) {
		return;
	}
#endif

	// loop through all render objects in the list:
	for (it.First(&RenderList); !it.Is_Done();)
	{
		// get the render object
		robj = it.Peek_Obj();
 		it.Next();	//advance to next object in case this one gets deleted during renderOneObject().

		if (robj->Class_ID() == RenderObjClass::CLASSID_TILEMAP)
			continue;	//we already rendered terrain

		if (robj->Is_Really_Visible()) {
			DrawableInfo *drawInfo = (DrawableInfo *)robj->Get_User_Data();
			Drawable *draw=nullptr;
			if (drawInfo)
				draw = drawInfo->m_drawable;
#ifdef USE_NON_STENCIL_OCCLUSION
			if (!(draw && drawInfo->m_flags & DrawableInfo::ERF_DELAYED_RENDER))	//model rendering is delayed for some reason until end of normal scene
#else
			if (!(draw && drawInfo->m_flags & (DrawableInfo::ERF_DELAYED_RENDER|DrawableInfo::ERF_POTENTIAL_OCCLUDER|DrawableInfo::ERF_IS_NON_OCCLUDER_OR_OCCLUDEE)))	//in this mode we delay almost all objects in order to do correct sorting with stencil.
#endif
				renderOneObject(rinfo, robj, localPlayerIndex);
		}
	}

	//Tell shadow manager to render shadows at the end of this frame
	//Don't draw shadows if there is no terrain present.
	if (TheW3DShadowManager && terrainObject && !ShaderClass::Is_Backface_Culling_Inverted() &&
		Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
	{
		TheW3DShadowManager->queueShadows(TRUE);
	}

	// only render particles once per frame
	if (terrainObject != nullptr && TheParticleSystemManager != nullptr &&
		Get_Extra_Pass_Polygon_Mode() == EXTRA_PASS_DISABLE)
	{
		TheParticleSystemManager->queueParticleRender();
	}
	gxGfxSubMark(GXSUB_OBJ);
}

/**Convert a player index to a color index, we use this because color indices are
assigned in left-right binary flipped fashion so as not to occupy lower bits unless
necessary.  This is used because lower bits are free for use by stencil shadow
rendering*/
#define NUMBER_PLAYER_COLOR_BITS 4	//need 4 bits to encode 8 players because indices start at 1 (not 0).
Int playerIndexToColorIndex(Int playerIndex)
{
	Int tmp=playerIndex;
	Int result=0;
	Int flippedPosition;

	//Player index is stored in 4 bits
	for (Int i=0; i<NUMBER_PLAYER_COLOR_BITS; i++)
	{
		flippedPosition = NUMBER_PLAYER_COLOR_BITS-1-i;	//correct position of bit after it's flipped left/right

		if (flippedPosition > i)
		{
			//shifting left
			result |= (tmp & (1<<i))<<(flippedPosition-i);
		}
		else
		{
			//shifting right
			result |= (tmp & (1<<i))>>(i-flippedPosition);
		}
	}
	return result;
}

/**Utility function used to render a full screen quad with the specified color and
stencil mask*/
void renderStenciledPlayerColor( UnsignedInt color, UnsignedInt stencilRef, Bool clear=FALSE)
{
	GxShadowScope gxPcol(GXS_T_PCOL);	// [GXSHADOW] pcol: the stencil part of the occluded-object flush
	struct _TRANSLITVERTEX {
	    Vector4 p;
		DWORD color;   // diffuse color
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

    v[0].p.Set(xpos+width, ypos+height, 0.0f, 1.0f );
    v[1].p.Set(xpos+width, 0, 0.0f, 1.0f );
    v[2].p.Set(xpos, ypos+height, 0.0f, 1.0f );
    v[3].p.Set(xpos,  0, 0.0f, 1.0f );
    v[0].color = color;
    v[1].color = color;
    v[2].color = color;
    v[3].color = color;

	DX8Wrapper::Set_Shader(PlayerColorShader);
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Apply_Render_State_Changes();	//force update all render states

	LPDIRECT3DDEVICE8 m_pDev=DX8Wrapper::_Get_D3D_Device8();

	if (!m_pDev)
		return;	//need device to render anything.

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	m_pDev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

	// Set stencil states
	DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, TRUE );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, TRUE );
	DWORD	oldColorWriteEnable=0x12345678;
	if (clear)
	{
		//we want to clear the stencil buffer to some known value wherever a player index is stored
		Int occludedMask=TheW3DShadowManager->getStencilShadowMask();
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,      0x80808080 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,     occludedMask );	//isolate bits containing occluder|playerIndex
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,0xffffffff );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_LESS );	//only draw to pixels that match the reference value
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_REPLACE );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,  D3DSTENCILOP_REPLACE );	//pixels which had occluded player colors, get MSB set.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_ZERO );	//pixels which had no occluded player colors are cleared.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_NEVER  );	//fail all access to the frame buffer to improve memory bandwidth

		//disable writes to color buffer
		if (DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps().PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE)
		{
			DX8Wrapper::_Get_D3D_Device8()->GetRenderState(D3DRS_COLORWRITEENABLE, &oldColorWriteEnable);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,0);
		}
		else
		{
			//device does not support disabling writes to color buffer so fake it through alpha blending
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ZERO );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ONE );
		}
	}
	else
	{
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,      stencilRef );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,     0xffffffff );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,0xffffffff );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_EQUAL );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,  D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );

		//Make occluded pixels transparent
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	}

	if (DX8Wrapper::_Is_Triangle_Draw_Enabled())
		{ m_pDev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANSLITVERTEX)); DX8Wrapper::Record_Direct_Draw(); }

	// turn off the stencil buffer
	DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);	//restore shader state
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_ALWAYS);

	if (oldColorWriteEnable != 0x12345678)
		DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,oldColorWriteEnable);

}

// GeneralsX: GX_OCCLSTATS=1: where flushOccludedObjectsIntoStencil's time goes (render-split G0 found the
// "occl" span at 4.7 ms of a 12.9 ms late-game scene on wh1ter0se's PC). One "[GXOCCL]" line per second: calls, then per-call means
// of what it sees (occluders, occludees and the translucent ones among them, other objects, player colours, mesh flushes) and
// the wall ms of its phases - occludees by player (plus the bucket sort), other objects, occluders, the colour quads and the
// stencil clear - with the mesh-renderer flushes' share of those phases separated out ("flush"). Off: one branch per call.
struct GxOcclStats
{
	unsigned long calls, occluders, occludees, transl, others, colors, flushes, plain;
	int64_t tOccludees, tOthers, tOccluders, tColors, tFlush, tFlushTransl, tFlushBucket, tEmpty;
	int64_t tPlainFlush;	// the no-occlusion path's flush, kept out of the stencil calls' per-call means (rcpu r1)
	int64_t nextPrint;
};
static GxOcclStats s_gxOccl;
static bool gxOcclOn()
{
	static const bool on = getenv( "GX_OCCLSTATS" ) != nullptr;
	return on;
}
static void gxOcclPrint()
{
	const int64_t now = gxGfxWallNs();
	if( now < s_gxOccl.nextPrint )
		return;
	if( s_gxOccl.nextPrint != 0 && ( s_gxOccl.calls > 0 || s_gxOccl.plain > 0 ) )
	{
		// every field but plain/plain_flush_ms is a mean per STENCIL call; plain_flush_ms is per plain call
		const double c = s_gxOccl.calls > 0 ? (double)s_gxOccl.calls : 1.0;
		const double pc = s_gxOccl.plain > 0 ? (double)s_gxOccl.plain : 1.0;
		fprintf( stderr, "[GXOCCL] calls=%lu plain=%lu occluders=%.1f occludees=%.1f transl=%.1f others=%.1f colors=%.1f flushes=%.1f "
			"occludees_ms=%.3f others_ms=%.3f occluders_ms=%.3f colors_ms=%.3f flush_ms=%.3f flush_transl_ms=%.3f flush_bucket_ms=%.3f empty_flush_ms=%.3f plain_flush_ms=%.3f\n",
			s_gxOccl.calls, s_gxOccl.plain, s_gxOccl.occluders / c, s_gxOccl.occludees / c, s_gxOccl.transl / c, s_gxOccl.others / c,
			s_gxOccl.colors / c, s_gxOccl.flushes / c, s_gxOccl.tOccludees / c / 1e6, s_gxOccl.tOthers / c / 1e6,
			s_gxOccl.tOccluders / c / 1e6, s_gxOccl.tColors / c / 1e6, s_gxOccl.tFlush / c / 1e6, s_gxOccl.tFlushTransl / c / 1e6,
			s_gxOccl.tFlushBucket / c / 1e6, s_gxOccl.tEmpty / c / 1e6, s_gxOccl.tPlainFlush / pc / 1e6 );
	}
	const int64_t next = now + 1000000000LL;
	s_gxOccl = GxOcclStats();
	s_gxOccl.nextPrint = next;
}
// A mesh-renderer flush, timed into flush_ms when GX_OCCLSTATS is on (and into one of the split totals, when given).
static void gxOcclFlush( int64_t *also = nullptr, bool plain = false )
{
	if( !gxOcclOn() )
	{
		TheDX8MeshRenderer.Flush();
		return;
	}
	const int64_t t0 = gxGfxWallNs();
	TheDX8MeshRenderer.Flush();
	const int64_t dt = gxGfxWallNs() - t0;
	if( plain )
	{
		s_gxOccl.tPlainFlush += dt;
		return;
	}
	s_gxOccl.tFlush += dt;
	if( also != nullptr )
		*also += dt;
	++s_gxOccl.flushes;
}

// GeneralsX: R-CPU 1f parallel object collection (
// §1.1). flushOccludedObjectsIntoStencil renders nearly every object when building
// occlusion is on, each one lit and linked into the mesh renderer's shared lists by renderOneObject on the main thread.
// Here those objects are first prepared on the main thread in the order the function renders them (prepareOneObject:
// the drawable reads and the shroud query, which writes simulation caches), then the plain ones - drawable, no tint,
// selection or heat vision, unshrouded, every pose current, every tree class a mesh/null/HLOD (gxTreeWorkerSafe) - are
// lit and walked on the GXParallel workers. A worker's mesh renders write only that mesh's own state; the adds that
// would link it into a shared list are recorded (GxCollectSink, dx8renderer.cpp), and so is each pose keep. The
// function then replays each object's records at the object's own place in its loops, between the same flushes, so
// every list is exactly what the serial walk would have built; any other object renders there as before.
// GX_COLLECTPAR: 0 off; 1 on; 2 on + check - each worker-collected object is also collected serially on the
// main thread, the two record lists and every linked mesh's lighting state are compared ([GXCOLL] chk= bad=), and the
// serial records are the ones replayed; 3 = 2 with the workers perturbed on purpose (the check's negative control);
// 4 = 1 plus a deliberate data race in the jobs (the ThreadSanitizer lane's mutant). GX_COLLECTSTATS=1: [GXCOLL] lines.
// Unset or empty = GX_COLLECTPAR_DEFAULT.
// GeneralsX: On by default where it gains: macOS - the Mini, G2 R1 frame time
// -0.758 ms (n=6+6, fully separated, 0.9.62 round 36). Windows stays OFF: wh1ter0se's PC overlapped on 0.9.61 (+0.248 ms) and was not
// re-timed on 0.9.62. iOS and Linux were never measured. GX_COLLECTPAR=0 turns it off, =1 on.
// scripts/test/collectpar-default-test.sh compiles this per platform.
#if defined(__APPLE__) && !TARGET_OS_IPHONE
static constexpr int GX_COLLECTPAR_DEFAULT = 1;	// macOS
#else
static constexpr int GX_COLLECTPAR_DEFAULT = 0;	// Windows, iOS, Linux
#endif
static int gxCollectMode()
{
	static const int s_mode = [] {
		const char *v = getenv("GX_COLLECTPAR");
		return (v == nullptr || *v == 0) ? GX_COLLECTPAR_DEFAULT : atoi(v);
	}();
	return s_mode;
}

static bool gxCollectCheck()	// modes 2 and 3; mode 4 is mode 1 plus the race
{
	return gxCollectMode() == 2 || gxCollectMode() == 3;
}

enum { GXC_SKIP, GXC_SERIAL, GXC_CAND, GXC_DONE, GXC_UNPREP };	// UNPREP: renderOneObject at its place (query + render)
enum { GXC_R_PREP, GXC_R_OVERFLOW, GXC_R_LIGHT, GXC_R_DIRTY, GXC_R_MOTION, GXC_R_POSE, GXC_R_CLASS, GXC_R_MESHREG, GXC_R_MESHSORT, GXC_R_LOD, GXC_R_COUNT };
static const int GXC_CLS = 64;	// [GXCOLL] histogram of the class ids that keep objects serial
static const int GXC_MAXREC = 128;	// records per object; a tree needing more renders serially (ovf=). 64 left ~2%
static const int GXC_CHUNK = 16;

struct GxCollectObj
{
	RenderObjClass *robj;
	RTS3DScene::GxObjPrep prep;
	Real emissive;	// rinfo.materialPassEmissiveOverride as the serial walk would leave it here (heat vision sets it)
	int state;
	int reason;
	int n;
	bool sceneLocal;	// rendering this tree stays inside it, in any LOD (gxTreeSceneLocal, the classify pass)
	bool motionSafe;	// every HLOD in it plays a raw motion, none or its base pose (the classify pass)
};
struct GxCollectBatch
{
	std::vector<GxCollectObj> objs;
	std::vector<GxCollectRec> recs;	// GXC_MAXREC per object
	size_t next = 0;
	bool active = false;
	bool queryLights = false;	// a shroud query of this batch made LightList non-empty (see gxCollectRun)
	bool renderLights = false;	// a render during the replay changed LightList
	bool mixedLights = false;	// both: cannot happen inside the prepared range (counted)
	bool lightStop = false;	// the preparing stopped at a tree that is not scene-local
	bool motionStop = false;	// ... at a tree playing a compressed or sound-carrying motion
	size_t stopAt = 0;	// objects prepared before the stop (all of them when none)
	std::vector<RenderObjClass *> lightsAfterQuery;	// LightList as that query left it (compared, never dereferenced)
};
static GxCollectBatch s_gxc;	// main thread, except each job's own objs[i] and recs slot

struct GxCollectCtx
{
	RTS3DScene *scene;
	CameraClass *camera;
	Real fogScale, fogStart, fogEnd, alpha, passAlpha;
	TexProjectClass *projector;
	RenderInfoClass::RINFO_OVERRIDE_FLAGS flags;
	int mode;
};

struct GxCollectStats
{
	unsigned long batches, objs, done, skip, serial[GXC_R_COUNT], cls[GXC_CLS], recs, chk, bad, lightsOff, queryLights, renderLights, mixedLights, lightStops, motionStops, prepared;
	int64_t tPrep, tPar, tReplay, nextPrint;
};
static GxCollectStats s_gxcStats;
static bool gxCollectStatsOn()
{
	static const bool on = getenv("GX_COLLECTSTATS") != nullptr || gxCollectCheck();
	return on;
}
static void gxCollectPrint()
{
	const int64_t now = gxGfxWallNs();
	if (now < s_gxcStats.nextPrint)
		return;
	GxCollectStats &s = s_gxcStats;
	if (s.nextPrint != 0 && (s.batches > 0 || s.lightsOff > 0))
	{
		const double b = s.batches > 0 ? (double)s.batches : 1.0;
		char cls[256] = "";
		size_t at = 0;
		for (int c = 0; c < GXC_CLS && at < sizeof(cls) - 16; ++c)
		{
			if (s.cls[c] != 0)
				at += snprintf(cls + at, sizeof(cls) - at, "%s%d:%.1f", at ? "," : "", c, s.cls[c] / b);
		}
		fprintf(stderr, "[GXCOLL] mode=%d workers=%d batches=%lu objs=%.1f done=%.1f skip=%.1f prep=%.1f ovf=%.1f light=%.1f "
			"lightsoff=%lu qlight=%lu lstop=%lu mstop=%lu prepfrac=%.3f rlight=%lu lmix=%lu dirty=%.1f "
			"motion=%.1f pose=%.1f class=%.1f[%s] meshreg=%.1f meshsort=%.1f lod=%.1f recs=%.1f prep_ms=%.3f par_ms=%.3f "
			"replay_ms=%.3f chk=%lu bad=%lu\n",
			gxCollectMode(), gxParallelWorkers(), s.batches, s.objs / b, s.done / b, s.skip / b, s.serial[GXC_R_PREP] / b,
			s.serial[GXC_R_OVERFLOW] / b, s.serial[GXC_R_LIGHT] / b, s.lightsOff, s.queryLights, s.lightStops, s.motionStops, s.objs > 0 ? (double)s.prepared / s.objs : 0.0, s.renderLights, s.mixedLights, s.serial[GXC_R_DIRTY] / b, s.serial[GXC_R_MOTION] / b, s.serial[GXC_R_POSE] / b,
			s.serial[GXC_R_CLASS] / b, cls, s.serial[GXC_R_MESHREG] / b, s.serial[GXC_R_MESHSORT] / b, s.serial[GXC_R_LOD] / b,
			s.recs / b, s.tPrep / b / 1e6, s.tPar / b / 1e6, s.tReplay / b / 1e6, s.chk, s.bad);
	}
	const int64_t next = now + 1000000000LL;
	s = GxCollectStats();
	s.nextPrint = next;
}

// Whether every Render the worker walk reaches stays inside this tree: MeshClass::Render (mesh.cpp, digested) links
// into shared lists only through the five hooked adds - except a first render, which registers the shared model
// (Register_For_Rendering), and a static sort level (WW3D::Add_To_Static_Sort_List): both stay on the main thread.
// An HLOD must leave Animatable3DObjClass::Render nothing to rebuild (the keep it runs on the main thread finds the
// pose current) and no transform dirty (RenderObjClass::Validate_Transform, reached from a mesh's bounding box, would
// rebuild the root's pose on the worker); Pose_Is_Worker_Safe excludes the blended and combo poses Pose_Needs_Build
// does not judge. Any other class (boxes, dazzles, emitters, spheres, rings draw or link elsewhere) keeps the object
// on the main thread.
static int gxMeshWorkerSafe(MeshClass *mesh)
{
	if (mesh->Is_Not_Hidden_At_All() == false)
		return 0;	// MeshClass::Render returns at once
	MeshModelClass *model = mesh->Peek_Model();
	if (!model->Has_Polygon_Renderers())
		return GXC_R_MESHREG;
	if (WW3D::Are_Static_Sort_Lists_Enabled() && model->Get_Sort_Level() != SORT_LEVEL_NONE)
		return GXC_R_MESHSORT;
	return 0;
}
// GeneralsX: hcanim-ceiling option B: the motions a collected tree may carry - Pose_Is_Worker_Safe's
// set (no motion, the base pose, a raw single motion without sounds) plus a single TIMECODED compressed motion without sounds. Such a
// pose is still built only on the main thread (the pose pre-pass keeps Pose_Is_Worker_Safe, and a dirty or stale pose stays serial in
// gxTreeWorkerSafe), so a worker meets it only clean and clock-current, where Render keeps it (Keep_Current_Pose) without decoding.
// What changes is the order of the main thread's decodes (the in-order prepare may pose the tree before its serial place), and a
// timecoded channel's values do not depend on that order: TimeCodedMotionChannelClass::get_index returns the packet
// binary_search_index would, CachedIdx only skips the search (motchan.cpp). Adaptive-delta channels do depend on it
// (CacheFrame/CacheData), so they stay out, as do blends, combos, morphs and embedded sounds. The order-independence holds for a
// query at or above the first key (below it binary_search_index never ends, either path); every shipped channel starts at key 0.
// Not under GX_POSECACHE=3, the pose cache's negative control: Is_Clock_Pose_Current drops its frame test there, so a stale
// compressed child of a clean static root would pass Pose_Needs_Build and a worker would cull its meshes with the old transform
// before the main thread's POSE replay moves them (hcanim-ceiling-q1 C3).
static bool gxMotionWorkerSafe(HLodClass *h)
{
	if (h->Animatable3DObjClass::Pose_Is_Worker_Safe())
		return true;
	if (Animatable3DObjClass::Pose_Cache_Mode() == 3)
		return false;
	// Peek_Animation: the SINGLE_ANIM motion, nullptr for blends and combos. HCompressedAnimClass has no class id of its own (it
	// and HMorphAnimClass both answer CLASSID_UNKNOWNANIM), hence the dynamic_cast.
	HCompressedAnimClass *c = dynamic_cast<HCompressedAnimClass *>(h->Animatable3DObjClass::Peek_Animation());
	return c != nullptr && !c->Has_Embedded_Sounds() && c->Get_Flavor() == ANIM_FLAVOR_TIMECODED;
}

// 0 when a worker may walk the tree, else the first rule it breaks (cls: the class id of a GXC_R_CLASS object).
static int gxTreeWorkerSafe(RenderObjClass *robj, int &cls)
{
	switch (robj->Class_ID())
	{
		case RenderObjClass::CLASSID_MESH:
			return gxMeshWorkerSafe((MeshClass *)robj);
		case RenderObjClass::CLASSID_NULL:
			return 0;
		case RenderObjClass::CLASSID_HLOD:
		{
			HLodClass *h = (HLodClass *)robj;
			if (h->Are_Sub_Object_Transforms_Dirty())
				return GXC_R_DIRTY;
			// gxMotionWorkerSafe motions only (the classify pass already keeps the others out: adv8 - an adaptive-delta
			// motion's shared decode cache is order-dependent, so even its bounding-box helper's evaluation must run at the
			// object's serial place).
			if (!gxMotionWorkerSafe(h))
				return GXC_R_MOTION;
			if (h->Pose_Needs_Build())
				return GXC_R_POSE;
			for (int lod = 0; lod < h->Get_Lod_Count(); lod++)
			{
				for (int i = 0; i < h->Get_Lod_Model_Count(lod); i++)
				{
					RenderObjClass *m = h->Peek_Lod_Model(lod, i);
					if (m->Class_ID() == RenderObjClass::CLASSID_OBBOX)
						continue;
					if (const int r = gxTreeWorkerSafe(m, cls))
						return r;
				}
			}
			for (int i = 0; i < h->Get_Additional_Model_Count(); i++)
			{
				RenderObjClass *m = h->Peek_Additional_Model(i);
				if (const int r = gxTreeWorkerSafe(m, cls))
					return r;
				// The walk's Set_LOD_Level must be a no-op: an HLOD that changes LOD re-registers its models with the
				// scene (HLodClass::Set_LOD_Level, Notify_Removed/Notify_Added). Meshes and nulls ignore it.
				if (h->Is_Sub_Objects_Match_LOD_Enabled() && m->Class_ID() == RenderObjClass::CLASSID_HLOD)
				{
					const int want = MIN(MAX(0, h->Get_LOD_Level()), m->Get_LOD_Count() - 1);
					if (m->Get_LOD_Level() != want)
						return GXC_R_LOD;
				}
			}
			return 0;
		}
		default:
			cls = robj->Class_ID();
			return GXC_R_CLASS;
	}
}

// A light below a container with dirty sub-object transforms: its Get_Position (Add_Light) or bounding sphere would
// rebuild that container's pose (RenderObjClass::Validate_Transform); in serial order the first object lit with it does,
// on the main thread, so every object lit with such a dynamic light stays there (adv1 P1).
static bool gxLightContainerDirty(RenderObjClass *light)
{
	for (RenderObjClass *c = light->Get_Container(); c != nullptr; c = c->Get_Container())
	{
		if (c->Are_Sub_Object_Transforms_Dirty())
			return true;
	}
	return false;
}

// Whether rendering this tree, in any LOD, stays inside it - read-only (Peek_ only, no refcounts: it runs on the
// workers). A render that reaches outside its tree can change what a later object reads: an LOD switch registers that
// model's lights in LightList, a pose rebuild moves its dynamic lights, and a dazzle casts visibility rays through the
// whole scene (Compute_Point_Visibility -> every object's Cast_Ray, which rebuilds other objects' dirty poses and sees
// whatever trees a shroud query has swapped in). So the batch's preparing stops at the first tree that is not
// scene-local (adv1-adv7). Meshes, nulls, boxes, spheres, rings and emitters (an empty Render) stay inside;
// a light, a dazzle, a collection, a distance LOD or any other class does not.
static bool gxTreeSceneLocal(RenderObjClass *robj, bool &motionSafe)
{
	switch (robj->Class_ID())
	{
		case RenderObjClass::CLASSID_MESH:
		case RenderObjClass::CLASSID_NULL:
		case RenderObjClass::CLASSID_AABOX:
		case RenderObjClass::CLASSID_OBBOX:
		case RenderObjClass::CLASSID_PARTICLEEMITTER:
		case RenderObjClass::CLASSID_SPHERE:
		case RenderObjClass::CLASSID_RING:
			return true;
		case RenderObjClass::CLASSID_HLOD:
		{
			HLodClass *h = (HLodClass *)robj;
			// motionSafe: every HLOD plays a gxMotionWorkerSafe motion. An adaptive-delta motion keeps a decode cache every
			// object playing it shares, and its results depend on that cache's state (motchan.cpp, adv8), so such a tree
			// is evaluated - bounding sphere included - only at its serial place, never prepared early or collected; a
			// timecoded one's results do not (gxMotionWorkerSafe).
			if (!gxMotionWorkerSafe(h))
				motionSafe = false;
			for (int lod = 0; lod < h->Get_Lod_Count(); lod++)
			{
				for (int i = 0; i < h->Get_Lod_Model_Count(lod); i++)
				{
					if (!gxTreeSceneLocal(h->Peek_Lod_Model(lod, i), motionSafe))
						return false;
				}
			}
			for (int i = 0; i < h->Get_Additional_Model_Count(); i++)
			{
				if (!gxTreeSceneLocal(h->Peek_Additional_Model(i), motionSafe))
					return false;
			}
			return true;
		}
		default:
			return false;
	}
}

// The batch's first pass, read-only on the workers before any query: which trees render scene-locally.
static void gxClassifyJob(void *, int begin, int end)
{
	for (int i = begin; i < end; ++i)
	{
		bool motionSafe = true;
		s_gxc.objs[i].sceneLocal = gxTreeSceneLocal(s_gxc.objs[i].robj, motionSafe);
		s_gxc.objs[i].motionSafe = motionSafe;
	}
}

static int s_gxcRacy;	// GX_COLLECTPAR=4 only: written by every job without a lock, for the ThreadSanitizer lane

static void gxCollectJob(void *ctx, int begin, int end)
{
	const GxCollectCtx &c = *(const GxCollectCtx *)ctx;
	for (int i = begin; i < end; ++i)
	{
		GxCollectObj &o = s_gxc.objs[i];
		if (o.state != GXC_CAND)
			continue;
		int cls = 0;
		if (const int r = gxTreeWorkerSafe(o.robj, cls))
		{
			o.state = GXC_SERIAL;
			o.reason = r;
			o.n = cls;	// a serial object has no records; n carries the class id for the stats
			continue;
		}
		if (c.mode == 4)
			++s_gxcRacy;
		RenderInfoClass wr(*c.camera);
		wr.fog_scale = c.fogScale;
		wr.fog_start = c.fogStart;
		wr.fog_end = c.fogEnd;
		wr.alphaOverride = c.alpha;
		wr.materialPassAlphaOverride = c.passAlpha;
		wr.materialPassEmissiveOverride = o.emissive + (c.mode == 3 ? 0.5f : 0.0f);
		wr.Texture_Projector = c.projector;
		if (c.flags != RenderInfoClass::RINFO_OVERRIDE_DEFAULT)
			wr.Push_Override_Flags(c.flags);
		GxCollectSink sink = { &s_gxc.recs[(size_t)i * GXC_MAXREC], 0, GXC_MAXREC, false };
		GxCollectActiveSink = &sink;
		c.scene->renderPreparedObject(wr, o.robj, o.prep, TRUE);
		GxCollectActiveSink = nullptr;
		if (sink.overflow)
		{
			o.state = GXC_SERIAL;	// its meshes' state is rewritten by the serial render
			o.reason = GXC_R_OVERFLOW;
			continue;
		}
		if (c.mode == 3 && sink.count >= 2)
			--sink.count;
		o.n = sink.count;
		o.state = GXC_DONE;
	}
}

void RTS3DScene::gxCollectRun(RenderInfoClass &rinfo, Int localPlayerIndex)
{
	const int mode = gxCollectMode();
	s_gxc.active = false;
	s_gxc.next = 0;
	// Whole-batch conditions. A worker's RenderInfoClass carries no material pass, so the passes must be empty; pose
	// cache mode 0 rebuilds every clock pose in Render; the renderer debugger and WW profiler keep global tables.
	bool ok = mode != 0 && m_customPassMode == SCENE_PASS_DEFAULT && rinfo.Additional_Pass_Count() == 0
		&& Animatable3DObjClass::Pose_Cache_Mode() != 0 && !DX8RendererDebugger::Is_Enabled() && !s_gxc.objs.empty();
#ifdef ENABLE_WWPROFILE
	ok = ok && !WWProfileManager::Is_Profile_Enabled();
#endif
	if (!ok)
		return;
	// Scene lights (LightList: lights inside model hierarchies) - exactness holds while the list is empty, which is the
	// only case the batch runs (adv1/adv2): a shroud query that swaps a ghost for a live tree, or a render that
	// switches an HLOD's LOD, re-registers the lights in those trees, and a light's state and ancestry then depend on
	// exactly when it is read. Each object records whether the list was still empty right after its own query; one that
	// sees it non-empty, and every object after a render made it non-empty, renders serially with the live list.
	if (!LightList.Is_Empty())
	{
		if (gxCollectStatsOn())
			++s_gxcStats.lightsOff;
		return;
	}
	s_gxc.queryLights = false;
	s_gxc.renderLights = false;
	s_gxc.mixedLights = false;
	s_gxc.lightStop = false;
	s_gxc.motionStop = false;
	s_gxc.lightsAfterQuery.clear();
	s_gxc.stopAt = s_gxc.objs.size();
	GxFlushScope gxProf(GxFlushProf::SUBMIT);	// GX_FLUSHPROF: preparing the batch is submission too
	const bool stats = gxCollectStatsOn();
	const int64_t t0 = stats ? gxGfxWallNs() : 0;

	// Which trees render scene-locally in any LOD, on the workers, before any query (read-only; gxTreeSceneLocal).
	gxParallelFor((int)s_gxc.objs.size(), GXC_CHUNK, gxClassifyJob, nullptr);

	// Prepare every object in render order, and pick the ones a worker may collect. The preparing stops at the first
	// object whose render may reach outside its own tree (gxTreeSceneLocal: lights, dazzles, unknown classes) and after
	// the first query that leaves LightList non-empty (adv1-adv7): from there every object is left to
	// renderOneObject at its place, query then render, interleaved, exactly as before (the stopping query's own object
	// renders serially with the live lists). Inside the prepared range every render stays inside its own tree and no
	// query changes LightList, so every prepared object reads what the serial walk would - lit without scene lights.
	Real emissive = rinfo.materialPassEmissiveOverride;
	for (size_t i = 0; i < s_gxc.objs.size(); ++i)
	{
		GxCollectObj &o = s_gxc.objs[i];
		o.emissive = emissive;
		o.n = 0;
		o.reason = GXC_R_PREP;
		if (!o.sceneLocal || !o.motionSafe)
		{
			// A motion outside gxMotionWorkerSafe (adaptive-delta, sound-carrying, blended, morph) may be evaluated through
			// caches every object playing it shares, so a later object's query or prepared bounds must not move ahead of
			// it (adv9): stop before it too.
			s_gxc.lightStop = !o.sceneLocal;
			s_gxc.motionStop = o.sceneLocal;
			for (size_t j = i; j < s_gxc.objs.size(); ++j)
			{
				s_gxc.objs[j].state = GXC_UNPREP;
				s_gxc.objs[j].reason = o.sceneLocal ? GXC_R_MOTION : GXC_R_LIGHT;
			}
			s_gxc.stopAt = i;
			break;
		}
		const Bool prepared = prepareOneObject(o.robj, localPlayerIndex, o.prep);
		if (!LightList.Is_Empty())
		{
			s_gxc.queryLights = true;
			RefRenderObjListIterator it(&LightList);
			for (it.First(); !it.Is_Done(); it.Next())
				s_gxc.lightsAfterQuery.push_back(it.Peek_Obj());
			o.state = prepared ? GXC_SERIAL : GXC_SKIP;
			o.prep.noSceneLights = FALSE;	// the live list, as the serial walk reads it here
			o.reason = GXC_R_LIGHT;
			for (size_t j = i + 1; j < s_gxc.objs.size(); ++j)
			{
				s_gxc.objs[j].state = GXC_UNPREP;
				s_gxc.objs[j].reason = GXC_R_LIGHT;
			}
			s_gxc.stopAt = i + 1;	// this object was prepared; the rest were not (prepfrac)
			break;
		}
		if (!prepared)
		{
			o.state = GXC_SKIP;
			continue;
		}
		o.prep.noSceneLights = TRUE;	// LightList is empty right after this object's own query
		const GxObjPrep &p = o.prep;
		const Bool lit = p.draw != nullptr && p.drawableHidden != TRUE;	// renderPreparedObject's drawable branch
		ObjectShroudStatus ss = (ObjectShroudStatus)p.ss;
#if ENABLE_CONFIGURABLE_SHROUD
		if (!gxShroudDrawn())
			ss = OBJECTSHROUD_CLEAR;
#endif
		const bool plain = !p.image3d && lit && p.drawInfo != nullptr && p.tintColor == nullptr && p.selectionColor == nullptr
			&& p.heatOpacity == 0 && ss <= OBJECTSHROUD_CLEAR;
		o.state = plain ? GXC_CAND : GXC_SERIAL;
		if (lit && p.heatOpacity != 0)
			emissive = p.heatOpacity;	// renderPreparedObject sets rinfo.materialPassEmissiveOverride and leaves it
	}
	if (s_gxc.recs.size() < s_gxc.objs.size() * GXC_MAXREC)
		s_gxc.recs.resize(s_gxc.objs.size() * GXC_MAXREC);

	// Lazy caches the workers would otherwise fill concurrently: the camera's frustum and view, and every light's
	// bounding sphere (RenderObjClass::Get_Bounding_Sphere). The objects' own caches are each one worker's.
	rinfo.Camera.Get_Frustum();
	bool dynDirty = false;
	{
		RefRenderObjListIterator dyn(&m_dynamicLightList);
		for (dyn.First(); !dyn.Is_Done(); dyn.Next())
		{
			if (gxLightContainerDirty(dyn.Peek_Obj()))
				dynDirty = true;
			else
				dyn.Peek_Obj()->Get_Bounding_Sphere();
		}
	}
	for (GxCollectObj &o : s_gxc.objs)
	{
		if (dynDirty && o.state == GXC_CAND && o.prep.dynamicLights)
		{
			o.state = GXC_SERIAL;
			o.reason = GXC_R_LIGHT;
		}
	}
	const int64_t t1 = stats ? gxGfxWallNs() : 0;

	GxCollectCtx ctx;
	ctx.scene = this;
	ctx.camera = &rinfo.Camera;
	ctx.fogScale = rinfo.fog_scale;
	ctx.fogStart = rinfo.fog_start;
	ctx.fogEnd = rinfo.fog_end;
	ctx.alpha = rinfo.alphaOverride;
	ctx.passAlpha = rinfo.materialPassAlphaOverride;
	ctx.projector = rinfo.Texture_Projector;
	ctx.flags = rinfo.Current_Override_Flags();
	ctx.mode = mode;
	GxCollectSinkArmed = true;	// the pool's dispatch publishes it to the workers; cleared after the join
	gxParallelFor((int)s_gxc.objs.size(), GXC_CHUNK, gxCollectJob, &ctx);
	GxCollectSinkArmed = false;


	s_gxc.active = true;

	if (stats)
	{
		GxCollectStats &s = s_gxcStats;
		++s.batches;
		s.prepared += (unsigned long)s_gxc.stopAt;
		s.tPrep += t1 - t0;
		s.tPar += gxGfxWallNs() - t1;
		for (const GxCollectObj &o : s_gxc.objs)
		{
			++s.objs;
			if (o.state == GXC_SKIP)
				++s.skip;
			else if (o.state == GXC_DONE)
			{
				++s.done;
				s.recs += (unsigned long)o.n;
			}
			else
			{
				++s.serial[o.reason];
				if (o.reason == GXC_R_CLASS && o.n >= 0 && o.n < GXC_CLS)
					++s.cls[o.n];
			}
		}
	}
}

// Read access to two MeshClass members the check fingerprints (protected; a pointer to member through a derived class).
struct GxMeshPeek : public MeshClass
{
	static float MeshClass::*Emissive() { return &GxMeshPeek::m_materialPassEmissiveOverride; }
	static float MeshClass::*PassAlpha() { return &GxMeshPeek::m_materialPassAlphaOverride; }
};
static unsigned gxFnv(unsigned h, const void *data, size_t n)
{
	const unsigned char *p = (const unsigned char *)data;
	for (size_t i = 0; i < n; ++i)
		h = (h ^ p[i]) * 16777619u;
	return h;
}
static unsigned gxFnvV(unsigned h, const Vector3 &v)
{
	const float f[3] = { v.X, v.Y, v.Z };
	return gxFnv(h, f, sizeof(f));
}
// What a linked mesh carries into the flush from its Render: the light environment it copied (every input light and
// the equivalent ambient: what DX8Wrapper::Set_Light_Environment reads) and the three overrides.
static unsigned gxMeshFingerprint(MeshClass *mesh)
{
	unsigned h = 2166136261u;
	if (mesh == nullptr)
		return h;
	const LightEnvironmentClass *env = mesh->Get_Lighting_Environment();
	const int has = env != nullptr ? 1 : 0;
	h = gxFnv(h, &has, sizeof(has));
	if (env != nullptr)
	{
		const int n = env->Get_Light_Count();
		h = gxFnv(h, &n, sizeof(n));
		for (int i = 0; i < n; ++i)
		{
			h = gxFnvV(gxFnvV(h, env->Get_Light_Direction(i)), env->Get_Light_Diffuse(i));
			const int point = env->isPointLight(i) ? 1 : 0;
			h = gxFnv(h, &point, sizeof(point));
			if (point)
			{
				const float r[2] = { env->getPointIrad(i), env->getPointOrad(i) };
				h = gxFnv(h, r, sizeof(r));
				h = gxFnvV(gxFnvV(gxFnvV(h, env->getPointDiffuse(i)), env->getPointAmbient(i)), env->getPointCenter(i));
			}
		}
		h = gxFnvV(h, env->Get_Equivalent_Ambient());
	}
	const float o[3] = { mesh->Get_Alpha_Override(), mesh->*GxMeshPeek::Emissive(), mesh->*GxMeshPeek::PassAlpha() };
	return gxFnv(h, o, sizeof(o));
}
static MeshClass *gxRecMesh(const GxCollectRec &r)
{
	switch (r.kind)
	{
		case GxCollectRec::TASK:
		case GxCollectRec::MATPASS:
		case GxCollectRec::DELAYED_MATPASS:
			return (MeshClass *)r.c;
		case GxCollectRec::SKIN:
			return (MeshClass *)r.b;
		default:
			return nullptr;	// a decal mesh: not a MeshClass
	}
}

void RTS3DScene::gxRenderCollected(RenderInfoClass &rinfo, RenderObjClass *robj, Int localPlayerIndex)
{
	if (!s_gxc.active)
	{
		renderOneObject(rinfo, robj, localPlayerIndex);
		return;
	}
	const size_t i = s_gxc.next++;
	if (i >= s_gxc.objs.size() || s_gxc.objs[i].robj != robj)
	{
		DEBUG_CRASH(("gxRenderCollected: the loops visit a different object than gxCollectRun prepared"));
		s_gxc.active = false;
		renderOneObject(rinfo, robj, localPlayerIndex);
		return;
	}
	const bool stats = gxCollectStatsOn();
	const int64_t t0 = stats ? gxGfxWallNs() : 0;
	GxCollectObj &o = s_gxc.objs[i];
	// GX_FLUSHPROF: replaying or serially rendering a collected object is submission; an
	// UNPREP object goes through renderOneObject, which times itself
	GxFlushScope gxProf(o.state == GXC_UNPREP ? -1 : (int)GxFlushProf::SUBMIT);
	// A safety net - the preparing stops at every query and render that can change LightList, so it should not fire
	// (rlight=/lmix= stay 0): if a render before this object changed it anyway, the serial walk lights this object and
	// every later one with the live list, so they render serially, worker records unused; with a query having changed it
	// too (lmix) the objects before that query cannot be lit exactly - counted.
	if (!s_gxc.renderLights && o.state != GXC_UNPREP)
	{
		bool changed;
		if (!s_gxc.queryLights)
			changed = !LightList.Is_Empty();
		else
		{
			size_t k = 0;
			changed = false;
			RefRenderObjListIterator it(&LightList);
			for (it.First(); !changed && !it.Is_Done(); it.Next(), ++k)
				changed = k >= s_gxc.lightsAfterQuery.size() || s_gxc.lightsAfterQuery[k] != it.Peek_Obj();
			changed = changed || k != s_gxc.lightsAfterQuery.size();
		}
		if (changed)
		{
			s_gxc.renderLights = true;
			s_gxc.mixedLights = s_gxc.queryLights;
		}
	}
	if (s_gxc.renderLights && (o.state == GXC_DONE || o.state == GXC_SERIAL))
	{
		o.prep.noSceneLights = FALSE;
		o.state = GXC_SERIAL;
	}
	const GxCollectRec *rec = &s_gxc.recs[i * GXC_MAXREC];
	if (o.state == GXC_DONE && gxCollectCheck())
	{
		// The check: fingerprint what the worker left in each mesh, collect the object again serially into a sink,
		// compare, and replay the serial records (the serial walk ran the pose keeps itself).
		static std::vector<GxCollectRec> s_w;
		static std::vector<unsigned> s_wf;
		static GxCollectRec s_serial[4096];
		s_w.clear();
		s_wf.clear();
		for (int k = 0; k < o.n; ++k)
		{
			if (rec[k].kind != GxCollectRec::POSE)
			{
				s_w.push_back(rec[k]);
				s_wf.push_back(gxMeshFingerprint(gxRecMesh(rec[k])));
			}
		}
		GxCollectSink sink = { s_serial, 0, 4096, false };
		GxCollectActiveSink = &sink;
		GxCollectSinkArmed = true;
		renderPreparedObject(rinfo, robj, o.prep, FALSE);
		GxCollectSinkArmed = false;
		GxCollectActiveSink = nullptr;
		bool same = !sink.overflow && sink.count == (int)s_w.size();
		for (int k = 0; same && k < sink.count; ++k)
		{
			const GxCollectRec &a = sink.rec[k];
			const GxCollectRec &b = s_w[k];
			same = a.kind == b.kind && a.a == b.a && a.b == b.b && a.c == b.c && gxMeshFingerprint(gxRecMesh(a)) == s_wf[k];
		}
		++s_gxcStats.chk;
		if (!same)
			++s_gxcStats.bad;
		for (int k = 0; k < sink.count; ++k)
			GxCollect_Replay(sink.rec[k]);
	}
	else if (o.state == GXC_DONE)
	{
		gxCensusObject(rinfo, robj, FALSE);
		for (int k = 0; k < o.n; ++k)
		{
			if (rec[k].kind == GxCollectRec::POSE)
				static_cast<Animatable3DObjClass *>((HLodClass *)rec[k].a)->Animatable3DObjClass::Render(rinfo);
			else
				GxCollect_Replay(rec[k]);
		}
	}
	else if (o.state == GXC_UNPREP)
	{
		renderOneObject(rinfo, robj, localPlayerIndex);
	}
	else if (o.state != GXC_SKIP)
	{
		renderPreparedObject(rinfo, robj, o.prep, FALSE);
	}
	if (stats)
		s_gxcStats.tReplay += gxGfxWallNs() - t0;
}

static void gxCollectEnd()
{
	if (s_gxc.active)
	{
		DEBUG_ASSERTCRASH(s_gxc.next == s_gxc.objs.size(), ("gxCollectEnd: %u of %u prepared objects rendered",
			(unsigned)s_gxc.next, (unsigned)s_gxc.objs.size()));
		s_gxc.active = false;
	}
	if (gxCollectStatsOn())
	{
		s_gxcStats.queryLights += s_gxc.queryLights ? 1 : 0;
		s_gxcStats.renderLights += s_gxc.renderLights ? 1 : 0;
		s_gxcStats.mixedLights += s_gxc.mixedLights ? 1 : 0;
		s_gxcStats.lightStops += s_gxc.lightStop ? 1 : 0;
		s_gxcStats.motionStops += s_gxc.motionStop ? 1 : 0;
	}
	s_gxc.queryLights = false;
	s_gxc.renderLights = false;
	s_gxc.mixedLights = false;
	s_gxc.lightStop = false;
	s_gxc.motionStop = false;
	s_gxc.lightsAfterQuery.clear();
	s_gxc.objs.clear();
	if (gxCollectStatsOn())
		gxCollectPrint();
}
static void gxCollectAdd(RenderObjClass *robj)
{
	GxCollectObj o;
	o.robj = robj;
	o.state = GXC_SKIP;
	o.reason = GXC_R_PREP;
	o.n = 0;
	o.sceneLocal = true;
	o.motionSafe = true;
	o.emissive = 0;
	s_gxc.objs.push_back(o);
}

#define MAX_VISIBLE_OCCLUDED_PLAYER_OBJECTS	512 //maximum number of occluded objects permitted per player
void RTS3DScene::flushOccludedObjectsIntoStencil(RenderInfoClass & rinfo)
{
	GxShadowScope gxOcc(GXS_T_OCC);	// [GXSHADOW] occ: mostly mesh submission; pcol inside it is the stencil part
	const bool gxStats = gxOcclOn();
	int64_t gxT = gxStats ? gxGfxWallNs() : 0;
	auto gxPhase = [&]( int64_t &into )	// GX_OCCLSTATS: close the phase that started at gxT
	{
		if( gxStats )
		{
			const int64_t now = gxGfxWallNs();
			into += now - gxT;
			gxT = now;
		}
	};
	RenderObjClass *robj;
	Drawable *draw;
	RenderObjClass *playerObjects[MAX_PLAYER_COUNT][MAX_VISIBLE_OCCLUDED_PLAYER_OBJECTS];
	RenderObjClass **lastPlayerObject[MAX_PLAYER_COUNT];
	Int playerColorIndex[MAX_PLAYER_COUNT];
	Int visiblePlayerColors[MAX_PLAYER_COUNT];	///<color assigned to each of the visible players
	Int numObjects;
	Vector3 hsv,rgb;

	Int usedPlayerColorIndex=1;
	Int numVisiblePlayerColors=0;

	//Clear pointers into temporary arrays where each player's objects will be stored.
	//We do this so that all objects are sorted by color which reduces the number of
	//state changes needed when drawing them.
	for (Int i=0; i<MAX_PLAYER_COUNT; i++)
	{
		lastPlayerObject[i]=&playerObjects[i][0];
		playerColorIndex[i]=-1;
	}

	//Assume no player colors are visible and all stencil bits are free for use by shadows.
	TheW3DShadowManager->setStencilShadowMask(0);

	const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();

	if (m_numPotentialOccludees && m_numPotentialOccluders)
	{
		//bucket sort all possibly occluded objects by player index/color.
		Int k=0;
		for (; k<m_numPotentialOccludees; k++)
		{
			robj=m_potentialOccludees[k];

			draw = ((DrawableInfo *)robj->Get_User_Data())->m_drawable;
			Object *object=draw->getObject();

			Int index=object->getControllingPlayer()->getPlayerIndex();

			if ((lastPlayerObject[index]-&playerObjects[index][0]) >= MAX_VISIBLE_OCCLUDED_PLAYER_OBJECTS)
			{
				DEBUG_CRASH(("Exceeded Maximum Number of potentially occluded models"));
				continue;
			}

			*lastPlayerObject[index] = robj;
			lastPlayerObject[index]++;	//increment to next object
		}

		// GeneralsX: R-CPU 1f: this branch's objects, in the order its loops render them.
		if (gxCollectMode() != 0)
		{
			for (Int p = 0; p < MAX_PLAYER_COUNT; p++)
				for (RenderObjClass **o = &playerObjects[p][0]; o < lastPlayerObject[p]; ++o)
					gxCollectAdd(*o);
			for (Int j = 0; j < m_numNonOccluderOrOccludee; j++)
				gxCollectAdd(m_nonOccludersOrOccludees[j]);
			for (Int j = 0; j < m_numPotentialOccluders; j++)
				gxCollectAdd(m_potentialOccluders[j]);
			gxCollectRun(rinfo, localPlayerIndex);
		}

		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, TRUE );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, TRUE );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0xffffffff);
		//Always store player index into stencil unless it is occluded by another
		//player's potentially occluded objects.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_ALWAYS );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,  D3DSTENCILOP_REPLACE );

		//Find out which player indices are actually used and remap them to
		//a color index.  Render all objects using the same color index at once.
		//We render potential occludees first because this allows them to z-sort correctly
		//when they are behind an occluder.
		for (k=0; k<MAX_PLAYER_COUNT; k++)
		{
			if ((numObjects=lastPlayerObject[k]-&playerObjects[k][0]) != 0)
			{
				//this player has some objects so draw them using his color index.
				if (playerColorIndex[k]==-1)	//color index not assigned yet?
				{
					//assign a new color index to this player
					playerColorIndex[k]=playerIndexToColorIndex(usedPlayerColorIndex++);
					//assign a color to this index by copying it from the controlling player
					//of all objects in this list.
					draw = ((DrawableInfo *)playerObjects[k][0]->Get_User_Data())->m_drawable;
					Object *object=draw->getObject();

					Int color=object->getControllingPlayer()->getPlayerColor();
					RGB_To_HSV(hsv,Vector3(((color>>16)&0xff)/255.0f,((color>>8)&0xff)/255.0f,(color &0xff)/255.0f));
					hsv.Z*=TheGlobalData->m_occludedLuminanceScale;
					HSV_To_RGB(rgb,hsv);
					visiblePlayerColors[numVisiblePlayerColors++]=DX8Wrapper::Convert_Color(rgb,0.5f);
				}

				Int thisPlayerColorIndex=playerColorIndex[k];

				//Store this object's color index into bits 3-6 of stencil buffer
				DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF, thisPlayerColorIndex<<3);

				//Render all of this player's objects for which we care when they are occluded.
				RenderObjClass **renderList=&playerObjects[k][0];
				for (Int j=0; j<numObjects; j++)
				{
					DrawableInfo *drawInfo=((DrawableInfo *)(*renderList)->Get_User_Data());
					if (drawInfo->m_flags & DrawableInfo::ERF_IS_TRANSLUCENT)
					{
						if( gxStats )
							++s_gxOccl.transl;
						// TheSuperHackers @info This only draws the occlusion of translucent objects.
						DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
						GxFlushSiteNext = GXFS_OCCL_TRANSL_PRE;
						gxOcclFlush( &s_gxOccl.tFlushTransl );	//render all the submitted meshes using current stencil function
						SHD_FLUSH;
						//Disable writing to color buffer since translucent objects are rendered at end of frame.
						DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_NEVER );	//never allow frame buffer writes.
						DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_REPLACE );	//always replace existing stencil value
						gxRenderCollected(rinfo, (*renderList), localPlayerIndex);
						DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
						GxFlushSiteNext = GXFS_OCCL_TRANSL_STENCIL;
						gxOcclFlush( &s_gxOccl.tFlushTransl );	//render all the submitted meshes using current stencil function
						SHD_FLUSH;
						DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );
						DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_ALWAYS );
					}
					else
					{
						gxRenderCollected(rinfo, (*renderList), localPlayerIndex);
					}
					renderList++;	//advance to next object
				}

				DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
				GxFlushSiteNext = GXFS_OCCL_BUCKET;
				gxOcclFlush( &s_gxOccl.tFlushBucket );	//render all the submitted meshes using current stencil function
			}
		}
		gxPhase( s_gxOccl.tOccludees );
		//Stencil buffer is now filled with color indices of potentially occluded objects.  We now draw
		//non-occluder or occludee objects such as small rocks, shrubs, etc. which we don't care about
		//but need to render here so that they don't interfere with building occlusion.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE );	//these objects are not stored in stencil
		RenderObjClass **nonOccluderOrOccludeeList=m_nonOccludersOrOccludees;
		for (k=0; k<m_numNonOccluderOrOccludee; k++)
		{
			gxRenderCollected(rinfo, (*nonOccluderOrOccludeeList), localPlayerIndex);
			nonOccluderOrOccludeeList++;	//advance to next one
		}
		DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
		GxFlushSiteNext = GXFS_OCCL_OTHERS;
		gxOcclFlush();	//render all the submitted meshes using current stencil function

		gxPhase( s_gxOccl.tOthers );
		//Stencil buffer is now filled with color indices of potentially occluded objects.  We now draw
		//occluder objects so they cover up and modify stencil MSB wherever they are in front of other objects.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, TRUE );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, TRUE );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF, 0xffffffff);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);	//isolate lowest player color
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0x80);	//only write to MSB
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_ALWAYS );	//check if player colors stored in pixel
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,  D3DSTENCILOP_REPLACE );

		//Render all potential occluders on top of already rendered potential occludees.
		RenderObjClass **occluderList=m_potentialOccluders;
		for (k=0; k<m_numPotentialOccluders; k++)
		{
			gxRenderCollected(rinfo, (*occluderList), localPlayerIndex);
			occluderList++;	//advance to next one
		}

		DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
		GxFlushSiteNext = GXFS_OCCL_OCCLUDERS;
		gxOcclFlush();	//render all the submitted meshes using current stencil function

		gxPhase( s_gxOccl.tOccluders );
		if( gxStats )
		{
			// The fixed cost of one flush: nothing is pending here, so this walks the containers and draws nothing.
			const int64_t t0 = gxGfxWallNs();
			GxFlushSiteNext = GXFS_OCCL_EMPTY;
			TheDX8MeshRenderer.Flush();
			s_gxOccl.tEmpty += gxGfxWallNs() - t0;
			gxT = gxGfxWallNs();
		}
		//We now have a stencil buffer where pixels that are occluded have a bit pattern of 1INDX000.
		//INDX contains the occluded player's color index.  We walk through all the player colors and
		//draw them wherever the stencil matches the color's index.
		Int usedPlayerColorBits=0;
		for (k=0; k<numVisiblePlayerColors; k++)
		{
			Int color=visiblePlayerColors[k];
			Int stencilRef=(playerIndexToColorIndex(k+1)<<3)|0x80;
			renderStenciledPlayerColor(color,stencilRef);
			usedPlayerColorBits |= stencilRef;	//keep track of all bits used for occlusion/player colors.
		}

		TheW3DShadowManager->setStencilShadowMask(usedPlayerColorBits);
		if (numVisiblePlayerColors >= 8 && TheGlobalData->m_useShadowVolumes)
		{
			//for cases where we have 8 or more visible players, we're only left with 3 bits to store
			//stencil shadows.  That's probably not enough since it will only allow 7 overlapping shadows.
			//So we clear the stencil buffer, leaving only the MSB set on any occluded player pixels so that
			//shadow code knows not to overwrite these pixels.
			renderStenciledPlayerColor(0,0, TRUE);
			TheW3DShadowManager->setStencilShadowMask(0x80808080);	//msb indicates occluded player pixels so ignore it when filling screen with shadow
		}

		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE );
		gxPhase( s_gxOccl.tColors );
		if( gxStats )
		{
			++s_gxOccl.calls;
			s_gxOccl.occluders += m_numPotentialOccluders;
			s_gxOccl.occludees += m_numPotentialOccludees;
			s_gxOccl.others += m_numNonOccluderOrOccludee;
			s_gxOccl.colors += numVisiblePlayerColors;
		}
	}
	else
	if (m_numNonOccluderOrOccludee || m_numPotentialOccluders || m_numPotentialOccludees)
	{
		if( gxStats )
			++s_gxOccl.plain;
		//no occluded objects so don't need to render anything special.  Just draw the queued up
		//objects like normal because they were skipped in the main scene traversal.
		// GeneralsX: R-CPU 1f: this branch's objects, in the order its loops render them.
		if (gxCollectMode() != 0)
		{
			for (Int j = 0; j < m_numPotentialOccludees; j++)
			{
				const DrawableInfo *drawInfo = static_cast<const DrawableInfo *>(m_potentialOccludees[j]->Get_User_Data());
				if ((drawInfo->m_flags & DrawableInfo::ERF_IS_TRANSLUCENT) == 0)
					gxCollectAdd(m_potentialOccludees[j]);
			}
			for (Int j = 0; j < m_numPotentialOccluders; j++)
				gxCollectAdd(m_potentialOccluders[j]);
			for (Int j = 0; j < m_numNonOccluderOrOccludee; j++)
				gxCollectAdd(m_nonOccludersOrOccludees[j]);
			gxCollectRun(rinfo, localPlayerIndex);
		}


		RenderObjClass **occludeeList=m_potentialOccludees;
		Int k=0;
		for (; k<m_numPotentialOccludees; k++)
		{
			// TheSuperHackers @bugfix xezon 18/10/2025 No longer draws translucent objects
			// as non-translucent ones here. They are drawn in another pass.
			DrawableInfo *drawInfo = static_cast<DrawableInfo *>((*occludeeList)->Get_User_Data());
			if ((drawInfo->m_flags & DrawableInfo::ERF_IS_TRANSLUCENT) == 0)
			{
				gxRenderCollected(rinfo, (*occludeeList), localPlayerIndex);
			}
			occludeeList++;	//advance to next one
		}

		RenderObjClass **occluderList=m_potentialOccluders;
		for (k=0; k<m_numPotentialOccluders; k++)
		{
			gxRenderCollected(rinfo, (*occluderList), localPlayerIndex);
			occluderList++;	//advance to next one
		}

		RenderObjClass **nonOccluderOrOccludeeList=m_nonOccludersOrOccludees;
		for (k=0; k<m_numNonOccluderOrOccludee; k++)
		{
			gxRenderCollected(rinfo, (*nonOccluderOrOccludeeList), localPlayerIndex);
			nonOccluderOrOccludeeList++;	//advance to next one
		}
		DX8Ring::Resume();	// d3d8 ring spike: this function holds no raw device pointer
		GxFlushSiteNext = GXFS_OCCL_PLAIN;
		gxOcclFlush( nullptr, true );	//render all the submitted meshes using current stencil function
	}

	gxCollectEnd();
	if( gxStats )
		gxOcclPrint();
	//Reset scene ambient because we sometimes mess around with it to make objects
	//glow, etc. when processing drawables.  This is a good place to do it because this
	//function gets called right after we flush regular render objects.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_AMBIENT,DX8Wrapper::Convert_Color(this->Get_Ambient_Light(),0.0f));
}

/*Version which does not require stencil buffer*/
void RTS3DScene::flushOccludedObjects(RenderInfoClass & rinfo)
{
	RenderObjClass *robj;
	Drawable *draw;

	TheW3DShadowManager->setStencilShadowMask(0);

	if (m_occludedObjectsCount)
	{
		const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();

		if (DX8Wrapper::Has_Stencil())	//just in case we have shadows, disable them over occluded pixels.
		{
			//Set all stencil pixels of potentially occluded objects to 128.
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, TRUE );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, TRUE );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,      128 );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILMASK,     0xffffffff );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILWRITEMASK,0xffffffff );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFAIL,  D3DSTENCILOP_KEEP );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILPASS,  D3DSTENCILOP_REPLACE );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILFUNC,  D3DCMP_ALWAYS );
		}

		//First draw all the solid colored models
		///@todo: Optimize this so that the extra passes don't actually install the material since it's all the same.
		rinfo.Push_Override_Flags(RenderInfoClass::RINFO_OVERRIDE_ADDITIONAL_PASSES_ONLY);	//disable textures
		Int i=0;
		for (; i<m_occludedObjectsCount; i++)
		{
			robj=m_potentialOccludees[i];

			draw = ((DrawableInfo *)robj->Get_User_Data())->m_drawable;
			Object *object=draw->getObject();

			Int index=object->getControllingPlayer()->getPlayerIndex();

			rinfo.Push_Material_Pass(m_occludedMaterialPass[index]);
			robj->Render(rinfo);
			rinfo.Pop_Material_Pass();
		}
		rinfo.Pop_Override_Flags();
		TheDX8MeshRenderer.Flush();

		//Now draw the normal models so they cover up the colored models on any pixels that

		//Now draw the normal models so they cover up the colored models on any pixels that
		//Normal models will clear stencil value from 128 back to 0 where the object pixels are
		//not occluded but will leave  128 in stencil where still occluded.
		if (DX8Wrapper::Has_Stencil())
			DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILREF,      0 );

		for (i=0; i<m_occludedObjectsCount; i++)
		{
			robj=m_potentialOccludees[i];
			renderOneObject(rinfo, robj, localPlayerIndex);//WW3D::Render(*robj,rinfo);
		}

		//Flush all the submitted translucent objects.
		TheDX8MeshRenderer.Flush();
		m_occludedObjectsCount = 0;
		DX8Wrapper::Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE );
		TheW3DShadowManager->setStencilShadowMask(0x80808080);	//upper MSB always contains flag indicating occluded player color.
	}

	//Reset scene ambient because we sometimes mess around with it to make objects
	//glow, etc. when processing drawables.  This is a good place to do it because this
	//function gets called right after we flush regular render objects.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_AMBIENT,DX8Wrapper::Convert_Color(this->Get_Ambient_Light(),0.0f));
}

void RTS3DScene::flushTranslucentObjects(RenderInfoClass & rinfo)
{
	RenderObjClass *robj;
	Drawable *draw;

	if (m_translucentObjectsCount)
	{
		const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();

		for (Int i=0; i<m_translucentObjectsCount; i++)
		{
			robj=m_translucentObjectsBuffer[i];
			draw = ((DrawableInfo *)robj->Get_User_Data())->m_drawable;

			rinfo.alphaOverride = draw->getEffectiveOpacity();

			renderOneObject(rinfo, robj, localPlayerIndex);//WW3D::Render(*robj,rinfo);
		}

		//Flush all the submitted translucent objects.
		GxFlushSiteNext = GXFS_TRANSLUCENT;
		TheDX8MeshRenderer.Flush();
		WW3D::Render_And_Clear_Static_Sort_Lists(rinfo);	//draws things like water
		rinfo.alphaOverride = 1.0f;	//disable forced alpha
		m_translucentObjectsCount = 0;
	}

	//Reset scene ambient because we sometimes mess around with it to make objects
	//glow, etc. when processing drawables.  This is a good place to do it because this
	//function gets called right after we flush regular render objects.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_AMBIENT,DX8Wrapper::Convert_Color(this->Get_Ambient_Light(),0.0f));
}

// GeneralsX: createLightsIterator()/destroyLightsIterator() removed from here:
// the iterator is two non-owning pointers, so callers build it on the stack over getLightList().
// Upstream reference: Mauller, PR #2646
// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2646

//=============================================================================
// RTS3DScene::addDynamicLight
//=============================================================================
/** Adds a dynamic light. */
//=============================================================================
void RTS3DScene::addDynamicLight(W3DDynamicLight * obj)
{
	m_dynamicLightList.Add(obj);
	UpdateList.Add(obj);
}

//=============================================================================
// RTS3DScene::addDynamicLight
//=============================================================================
/** Adds a dynamic light. */
//=============================================================================
W3DDynamicLight * RTS3DScene::getADynamicLight()
{
	RefRenderObjListIterator dynaLightIt(&m_dynamicLightList);
	W3DDynamicLight *pLight;
	for (dynaLightIt.First(); !dynaLightIt.Is_Done(); dynaLightIt.Next())
	{
		pLight = (W3DDynamicLight*)dynaLightIt.Peek_Obj();
		if (!pLight->isEnabled()) {
			pLight->setEnabled(true);
			return(pLight);
		}
	}
	pLight = NEW_REF(W3DDynamicLight, ());
	addDynamicLight( pLight );
	pLight->Release_Ref();
	pLight->setEnabled(true);
	return(pLight);
}

//=============================================================================
// RTS3DScene::removeDynamicLight
//=============================================================================
/** Removes a dynamic light. */
//=============================================================================
void RTS3DScene::removeDynamicLight(W3DDynamicLight * obj)
{
	m_dynamicLightList.Remove(obj);
}

//=============================================================================
// RTS3DScene::doRender
//=============================================================================
/** Render the scene */
//=============================================================================
void RTS3DScene::doRender( CameraClass * cam )
{
	m_camera = cam;
	DRAW();
	m_camera = nullptr;

}

//=============================================================================
// RTS3DScene::draw
//=============================================================================
/** Customized render for the 2d scene management */
//=============================================================================
void RTS3DScene::draw()
{
	if (m_camera == nullptr) {
		DEBUG_CRASH(("Null m_camera in RTS3DScene::draw"));
		return;
	}
	WW3D::Render( this, m_camera );
}


///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

//=============================================================================
// RTS2DScene::RTS2DScene
//=============================================================================
/** */
//=============================================================================
RTS2DScene::RTS2DScene()
{
	setName("RTS2DScene");
	m_status = NEW_REF( W3DStatusCircle, () );
	Add_Render_Object( m_status );
}

//=============================================================================
// RTS2DScene::~RTS2DScene
//=============================================================================
/** */
//=============================================================================
RTS2DScene::~RTS2DScene()
{
	this->Remove_Render_Object(m_status);
	REF_PTR_RELEASE(m_status);
}

//=============================================================================
// RTS2DScene::Custimized_Render
//=============================================================================
/** Customized render for the 2d scene management */
//=============================================================================
void RTS2DScene::Customized_Render( RenderInfoClass &rinfo )
{
	// call simple scene class renderer
	SimpleSceneClass::Customized_Render( rinfo );
}

//=============================================================================
// RTS2DScene::doRender
//=============================================================================
/** Render the scene */
//=============================================================================
void RTS2DScene::doRender( CameraClass * cam )
{
	m_camera = cam;
	DRAW();
	m_camera = nullptr;
}

//=============================================================================
// RTS2DScene::draw
//=============================================================================
/** Customized render for the 2d scene management */
//=============================================================================
void RTS2DScene::draw()
{
	if (m_camera == nullptr) {
		DEBUG_CRASH(("Null m_camera in RTS2DScene::draw"));
		return;
	}
	WW3D::Render( this, m_camera );
}



///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

//=============================================================================
// RTS3DInterfaceScene::RTS3DInterfaceScene
//=============================================================================
/** */
//=============================================================================
RTS3DInterfaceScene::RTS3DInterfaceScene()
{
}

//=============================================================================
// RTS3DInterfaceScene::~RTS3DInterfaceScene
//=============================================================================
/** */
//=============================================================================
RTS3DInterfaceScene::~RTS3DInterfaceScene()
{
}

//=============================================================================
// RTS3DInterfaceScene::Custimized_Render
//=============================================================================
/** Customized render for the 3d interface scene management */
//=============================================================================
void RTS3DInterfaceScene::Customized_Render( RenderInfoClass &rinfo )
{
	// call simple scene class renderer
	SimpleSceneClass::Customized_Render( rinfo );
}



/// The following is an :archive" of a partial attempt at detecting the mapshroud hack
/*
 *

void RTS3DScene::Visibility_Check(CameraClass * camera)
{
#ifdef DIRTY_CONDITION_FLAGS
	StDrawableDirtyStuffLocker lockDirtyStuff;
#endif

	RefRenderObjListIterator it(&RenderList);
	DrawableInfo *drawInfo = nullptr;
	Drawable	*draw = nullptr;
	RenderObjClass * robj;

	m_numPotentialOccluders=0;
	m_numPotentialOccludees=0;
	m_translucentObjectsCount=0;
	m_numNonOccluderOrOccludee=0;

	Int currentFrame=0;
	if (TheGameLogic) currentFrame = TheGameLogic->getFrame();
	if (currentFrame <= TheGlobalData->m_defaultOcclusionDelay)
		currentFrame = TheGlobalData->m_defaultOcclusionDelay+1;	//make sure occlusion is enabled when game starts (frame 0).


	if (ShaderClass::Is_Backface_Culling_Inverted())
	{	//we are rendering reflections
		///@todo: Have better flag to detect reflection pass

		// Loop over all top-level RenderObjects in this scene. If the bounding sphere is not in front
		// of all the frustum planes, it is invisible.
		for (it.First(); !it.Is_Done(); it.Next()) {

			robj = it.Peek_Obj();

			draw=nullptr;
			drawInfo = (DrawableInfo *)robj->Get_User_Data();
			if (drawInfo)
				draw=drawInfo->m_drawable;

			if( draw )
			{
				if (robj->Is_Force_Visible()) {
					robj->Set_Visible(true);
				} else {
					robj->Set_Visible(draw->getDrawsInMirror() && !camera->Cull_Sphere(robj->Get_Bounding_Sphere()));
				}
			}
			else
			{	//perform normal culling on non-drawables
				if (robj->Is_Force_Visible()) {
					robj->Set_Visible(true);
				} else {
					robj->Set_Visible(!camera->Cull_Sphere(robj->Get_Bounding_Sphere()));
				}
			}
		}
	}
	else
	{

		// Loop over all top-level RenderObjects in this scene. If the bounding sphere is not in front
		// of all the frustum planes, it is invisible.
		for (it.First(); !it.Is_Done(); it.Next()) {

			robj = it.Peek_Obj();

			if (robj->Is_Force_Visible()) {
				robj->Set_Visible(true);
			} else if (robj->Is_Hidden()) {
				robj->Set_Visible(false);
			} else {

				UnsignedByte isVisible = 0;


        //Cheater Foil
        isVisible |= (UnsignedByte)(camera->Cull_Sphere(robj->Get_Bounding_Sphere()) == FALSE);
        isVisible |= (UnsignedByte)(draw->isDrawableEffectivelyHidden());
        isVisible |= (draw->getFullyObscuredByShroudWithCheatSpy());
				robj->Set_VisibleWithCheatSpy(isVisible);
				if (robj->Is_VisibleWithCheatSpy())//this will clear for the bit set above

				{	//need to keep track of occluders and ocludees for subsequent code.
					drawInfo = (DrawableInfo *)robj->Get_User_Data();
					if (drawInfo && (draw=drawInfo->m_drawable) != nullptr)
					{

//            now handled above in the cheater foil <<<<<<<<<<<<
//						if (draw->isDrawableEffectivelyHidden() || draw->getFullyObscuredByShroud())
//						{	robj->Set_Visible(false);
//							continue;
//						}
						//assume normal rendering.
						drawInfo->m_flags = DrawableInfo::ERF_IS_NORMAL;	//clear any rendering flags that may be in effect.


						if (draw->getEffectiveOpacity() != 1.0f && m_translucentObjectsCount < TheGlobalData->m_maxVisibleTranslucentObjects)
						{	drawInfo->m_flags |= DrawableInfo::ERF_IS_TRANSLUCENT;	//object is translucent
							m_translucentObjectsBuffer[m_translucentObjectsCount++] = robj;
						}
						if (TheGlobalData->m_enableBehindBuildingMarkers && TheGameLogic->getShowBehindBuildingMarkers())
						{
							//visible drawable. Check if it's either an occluder or occludee
							if (draw->isKindOf(KINDOF_STRUCTURE) && m_numPotentialOccluders < TheGlobalData->m_maxVisibleOccluderObjects)
							{	//object which could occlude other objects that need to be visible.
								//Make sure this object is not translucent so it's not rendered twice (from m_potentialOccluders and m_translucentObjectsBuffer)
								if (drawInfo->m_flags ^ DrawableInfo::ERF_IS_TRANSLUCENT)
									m_potentialOccluders[m_numPotentialOccluders++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_POTENTIAL_OCCLUDER;
							}
							else
							if (draw->getObject() &&
									(draw->isKindOf(KINDOF_SCORE) || draw->isKindOf(KINDOF_SCORE_CREATE) || draw->isKindOf(KINDOF_SCORE_DESTROY) || draw->isKindOf(KINDOF_MP_COUNT_FOR_VICTORY)) &&
									(draw->getObject()->getSafeOcclusionFrame()) <= currentFrame && m_numPotentialOccludees < TheGlobalData->m_maxVisibleOccludeeObjects)
							{	//object which could be occluded but still needs to be visible.
								//We process transucent units twice (also in m_translucentObjectsBuffer) because we need to see them when occluded.
								m_potentialOccludees[m_numPotentialOccludees++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_POTENTIAL_OCCLUDEE;
							}
							else
							if (drawInfo->m_flags == DrawableInfo::ERF_IS_NORMAL && m_numNonOccluderOrOccludee < TheGlobalData->m_maxVisibleNonOccluderOrOccludeeObjects)
							{	//regular object with no custom effects but still needs to be delayed to get the occlusion feature to work correctly.
								//Make sure this object is not translucent so it's not rendered twice (from m_potentialOccluders and m_translucentObjectsBuffer)
								if (drawInfo->m_flags ^ DrawableInfo::ERF_IS_TRANSLUCENT)	//make sure not translucent
									m_nonOccludersOrOccludees[m_numNonOccluderOrOccludee++]=robj;
								drawInfo->m_flags |= DrawableInfo::ERF_IS_NON_OCCLUDER_OR_OCCLUDEE;
							}
						}


					}
				}

//				robj->Set_Visible(isVisible); //now handled above in the cheater foil
			}

			///@todo: We're not using LOD yet so I disabled this code. MW
			// Also, should check how multiple passes (reflections) get along
			// with the LOD manager - we're rendering double the load it thinks we are.
			// Prepare visible objects for LOD:
			//	if (robj->Is_Really_Visible()) {
			//			robj->Prepare_LOD(*camera);
			//		}
		}
	}

   Visibility_Checked = true;
}


 *
 */

