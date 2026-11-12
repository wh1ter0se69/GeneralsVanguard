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

// AIPlayer.cpp ///////////////////////////////////////////////////////////////////////////////////
// Computerized opponent
// Author: Michael S. Booth, January 2002
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/GameMemory.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/SpecialPower.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/PlayerList.h"
#include "Common/BuildAssistant.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/WellKnownKeys.h"
#include "Common/Xfer.h"
#include "GameClient/ControlBar.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AIPlayer.h"
#include "GameLogic/GXOctant.h"
#if defined(GX_TEST_INSTRUMENTS)
#define GX_ROT_LOG(...) \
	do { fprintf(stderr, "[GXROT] f=%u ", (unsigned)TheGameLogic->getFrame()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
		fflush(stderr); } while (0)
#else
#define GX_ROT_LOG(...) do { } while (0)
#endif
#include "GameLogic/SidesList.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/DefaultProductionExitUpdate.h"	// a door's rally point (gxDoorHalfSkew)
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/UpdateModule.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/QueueProductionExitUpdate.h"	// a door's rally point (gxDoorHalfSkew)
#include "GameLogic/Module/RebuildHoleBehavior.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/GXHookTimer.h"	// [GXHOOK] CPU timer (test-instruments builds)
// the face-front arm's per-tick time: the door turn here and the defence facing in AISkirmishPlayer.cpp, one sum
GX_HOOK_STATS_DEFINE(gxFaceFrontHook, "face_front");
#include "GameNetwork/GameInfo.h"
#include "Lib/trig.h"	// GeneralsX: deterministic Sin/Cos - see WWMath/gamemath.h

// GeneralsX: The AI tests a structure spot as standing ON THE GROUND. Its spot searches move a
// candidate in x and y only, keeping the height of the spot they started from (the build-list spot, the warehouse, the base
// centre), and isLocationLegalToBuild sees an object only when the two boxes overlap in height too
// (PartitionManager::geomCollidesWithGeom). So from a spot on a ridge, every candidate on the low ground below was tested as a box
// floating far above it and passed through everything there: on Twin Ridges a GLA AI put two Barracks 20 units apart, the
// second tested 47 units above the first. The same code is in TheSuperHackers and Generals Online.
// This tests a grounded copy and leaves the caller's position untouched.
static LegalBuildCode gxLegalToBuildOnGround( const Coord3D *pos, const ThingTemplate *what, Real angle, UnsignedInt options,
	const Object *builder, Player *player )
{
	Coord3D ground = *pos;
	ground.z = TheTerrainLogic->getGroundHeight( ground.x, ground.y );
	return TheBuildAssistant->isLocationLegalToBuild( &ground, what, angle, options, builder, player );
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: base-layout:
// THE DOOR RULE, for a side whose list carries GXFrontDoors. R's own aim, the octant towards the front waypoint, reverses a
// door that stands past its waypoint, and it fans the flanks far off the way the base faces. With t = unit(spot -> front) and a = unit(base centre ->
// front): past the waypoint (t.a <= 0) aim along a, else along t + a. Then half the door's skew: units leave along model +X, but the rally point
// sits off to one side (ChinaBarracks -35 deg, war factories -21..-30, USA and GLA barracks 0), and aiming between the two keeps both within the
// octant. No trig: +, *, / and sqrt under -ffp-contract=off and /fp:precise, then gxOctantAngle's comparisons; x64 and arm64 are held equal by
// gxDoorRuleFixture. Simulated on the 0.9.69 positions: base-direction front 46 -> 94%, truly reversed 7.1 -> 0% (tools-v2/sim-v2.py, an upper bound).
// ------------------------------------------------------------------------------------------------
static Bool gxDoorKind( const ThingTemplate *t, Bool keyed )
{
	return t->isKindOf( KINDOF_FS_FACTORY )
		&& ( t->isKindOf( KINDOF_FS_BARRACKS ) || t->isKindOf( KINDOF_FS_WARFACTORY ) || ( keyed && t->isKindOf( KINDOF_FS_AIRFIELD ) ) );
}

// GeneralsX: base-layout: an airfield is drawn facing the front and queued at the rule's angle, and a turned one's padded box is
// huge, so turning it to the next octant would only push its neighbours into the position search. It keeps its list angle unless the rule's octant is more than one octant off.
static Bool gxAirfieldKeepsList( const ThingTemplate *t, Real want, Real listAngle )
{
	return t->isKindOf( KINDOF_FS_AIRFIELD ) && fabs( normalizeAngle( want - listAngle ) ) <= PI / 4 + 0.001f;
}

static Bool gxUnit( Real *x, Real *y )
{
	const Real len = (Real)sqrt( (*x) * (*x) + (*y) * (*y) );
	if( len <= 0.0f )
		return FALSE;
	*x /= len;
	*y /= len;
	return TRUE;
}

// h = unit(rally) + (1,0) has half the rally point's angle, so aim x conj(h) turns by half the skew with no trig. (1,0) = no skew: an airfield has no
// production exit.
static void gxDoorHalfSkew( const ThingTemplate *t, Real *hx, Real *hy )
{
	*hx = 1.0f;
	*hy = 0.0f;
	const ModuleInfo &mi = t->getBehaviorModuleInfo();
	for( Int i = 0; i < mi.getCount(); ++i )
	{
		const Coord3D *rally = nullptr;
		if( mi.getNthName( i ) == "DefaultProductionExitUpdate" )
			rally = &static_cast<const DefaultProductionExitUpdateModuleData *>( mi.getNthData( i ) )->m_naturalRallyPoint;
		else if( mi.getNthName( i ) == "QueueProductionExitUpdate" )
			rally = &static_cast<const QueueProductionExitUpdateModuleData *>( mi.getNthData( i ) )->m_naturalRallyPoint;
		if( rally == nullptr )
			continue;
		Real x = rally->x;
		Real y = rally->y;
		if( gxUnit( &x, &y ) )
		{
			*hx = x + 1.0f;
			*hy = y;
		}
		return;
	}
}

static void gxDoorAimVector( const Coord3D &centre, Real hx, Real hy, const Coord3D &at, const Coord3D &front, Real *outX, Real *outY )
{
	Real tx = front.x - at.x;
	Real ty = front.y - at.y;
	Real ax = front.x - centre.x;
	Real ay = front.y - centre.y;
	gxUnit( &tx, &ty );
	gxUnit( &ax, &ay );
	Real aimX = ax;
	Real aimY = ay;
	if( tx * ax + ty * ay > 0.0f )
	{
		aimX += tx;
		aimY += ty;
	}
	*outX = aimX * hx + aimY * hy;
	*outY = aimY * hx - aimX * hy;
}

static Real gxDoorAim( const Coord3D &centre, Real hx, Real hy, const Coord3D &at, const Coord3D &front )
{
	Real x, y;
	gxDoorAimVector( centre, hx, hy, at, front, &x, &y );
	return gxOctantAngle( x, y );
}

static Real gxDoorWant( const Coord3D &centre, const ThingTemplate *t, const Coord3D &at, const Coord3D &front )
{
	Real hx, hy;
	gxDoorHalfSkew( t, &hx, &hy );
	return gxDoorAim( centre, hx, hy, at, front );
}

#if defined(GX_TEST_INSTRUMENTS)
// GX_DOORRULE_FIXTURE=1, once per process: the door rule over a grid that holds every octant boundary and t.a = 0, on purpose, because one
// game may never reach them. Prints a hash of the raw aim bits and the chosen octants, and how many cases sat on each boundary: x64 and arm64 must print the
// same line, and a zero boundary count means the fixture missed what it is for.
void gxDoorRuleFixture()
{
	static Bool s_done = FALSE;
	if( s_done || getenv( "GX_DOORRULE_FIXTURE" ) == nullptr )
		return;
	s_done = TRUE;
	static const Real T = 0.41421356f;	// gxOctantAngle's tan(22.5 deg)
	static const Real dirs[16][2] = { { 1, T }, { 1, -T }, { -1, T }, { -1, -T }, { T, 1 }, { T, -1 }, { -T, 1 }, { -T, -1 },
		{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	static const Real rally[5][2] = { { 1, 0 }, { 36, -25 }, { 53, -30 }, { 57, -27 }, { 40, -15 } };
	unsigned int hash = 2166136261u;
	long long cases = 0, onOctant = 0, onPerp = 0;
	long long octants[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	Coord3D centre;
	centre.x = 1000.0f; centre.y = 1000.0f; centre.z = 0.0f;
	for( Int r = 0; r < 5; ++r )
	{
		Real rx = rally[r][0];
		Real ry = rally[r][1];
		gxUnit( &rx, &ry );
		const Real hx = rx + 1.0f;
		const Real hy = ry;
		for( Int d = 0; d < 16; ++d )
		{
			for( Int dist = 150; dist <= 1050; dist += 300 )
			{
				Coord3D front;
				front.x = centre.x + dirs[d][0] * dist;
				front.y = centre.y + dirs[d][1] * dist;
				front.z = 0.0f;
				for( Int ix = -30; ix <= 30; ++ix )
				{
					for( Int iy = -30; iy <= 30; ++iy )
					{
						Coord3D at;
						// the grid, and on its axes the exact perpendicular through the front point (t.a = 0)
						if( ix == 0 )
						{
							at.x = front.x - dirs[d][1] * ( (Real)iy * 10.0f );
							at.y = front.y + dirs[d][0] * ( (Real)iy * 10.0f );
						}
						else
						{
							at.x = centre.x + (Real)ix * 40.0f;
							at.y = centre.y + (Real)iy * 40.0f;
						}
						at.z = 0.0f;
						Real x, y;
						gxDoorAimVector( centre, hx, hy, at, front, &x, &y );
						const Real want = gxOctantAngle( x, y );
						Real tx = front.x - at.x, ty = front.y - at.y, ax = front.x - centre.x, ay = front.y - centre.y;
						gxUnit( &tx, &ty );
						gxUnit( &ax, &ay );
						const Real dot = tx * ax + ty * ay;
						if( dot > -1e-5f && dot < 1e-5f )
							++onPerp;
						const Real bx = x < 0 ? -x : x, by = y < 0 ? -y : y;
						const Real m1 = by - bx * T, m2 = bx - by * T;
						if( ( m1 > -1e-5f && m1 < 1e-5f ) || ( m2 > -1e-5f && m2 < 1e-5f ) )
							++onOctant;
						const Int oct = REAL_TO_INT( ( want + 2 * PI ) / ( PI / 4 ) + 0.5f ) % 8;
						++octants[oct];
						++cases;
						const Real vals[3] = { x, y, want };
						for( Int v = 0; v < 3; ++v )
						{
							unsigned int bits;
							memcpy( &bits, &vals[v], sizeof bits );
							hash = ( hash ^ bits ) * 16777619u;
						}
					}
				}
			}
		}
	}
	fprintf( stderr, "[GXROT] ev=doorrule_fixture cases=%lld on_octant=%lld on_perp=%lld hash=%08x oct=%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", cases, onOctant, onPerp, hash,
		octants[0], octants[1], octants[2], octants[3], octants[4], octants[5], octants[6], octants[7] );
	fflush( stderr );
}
#endif

#if defined(GX_TEST_INSTRUMENTS)
// GX_LAYOUTFB_FIXTURE=1 or GX_DOORRULE_FIXTURE=1, once per process: exercises the gate of buildStructureWithDozer's "nothing found"
// retry (BuildListInfo::gxUseLayoutFallback) on every kind of entry. Arm S is the fail arm: a stock supply centre's stock spot
// must NOT be taken there, so that entry keeps the rebuild wait. Prints the failed arms; pass=6 fail=0 is the only good line.
void gxLayoutFallbackFixture()
{
	static Bool s_done = FALSE;
	if( s_done || ( getenv( "GX_LAYOUTFB_FIXTURE" ) == nullptr && getenv( "GX_DOORRULE_FIXTURE" ) == nullptr ) )
		return;
	s_done = TRUE;
	Coord3D a, b, c;
	a.x = 100.0f; a.y = 200.0f; a.z = 0.0f;
	b.x = 300.0f; b.y = 400.0f; b.z = 0.0f;
	c.x = 500.0f; c.y = 600.0f; c.z = 0.0f;
	Int pass = 0, fail = 0;
	char failed[64] = "";
	#define GX_LFB_ARM( name, ok ) do { if( ok ) ++pass; else { ++fail; strlcat( failed, name, sizeof failed ); } } while( 0 )
	BuildListInfo *e = newInstance( BuildListInfo );
	// S: a stock supply centre (buildBySupplies' setFallbackSpot): refused, the entry untouched, its today's spot still pending for the supply readers
	e->setLocation( a ); e->setAngle( 0.5f ); e->setFallbackSpot( b, 1.0f );
	GX_LFB_ARM( "S", !e->gxUseLayoutFallback() && e->getLocation()->x == a.x && e->getLocation()->y == a.y && e->getAngle() == 0.5f
		&& e->hasFallbackSpot() && !e->hasLayoutFallback() && e->getFallbackLocation() != nullptr && e->getFallbackLocation()->x == b.x );
	deleteInstance( e );
	// L: a layout fallback: hidden from the supply readers, taken once, then gone
	e = newInstance( BuildListInfo );
	e->setLocation( a ); e->setAngle( 0.5f ); e->gxSetLayoutFallback( b, 1.0f );
	const Bool hidden = !e->hasFallbackSpot() && e->hasLayoutFallback();
	const Bool took = e->gxUseLayoutFallback() && e->getLocation()->x == b.x && e->getLocation()->y == b.y && e->getAngle() == 1.0f;
	GX_LFB_ARM( "L", hidden && took && !e->gxUseLayoutFallback() && !e->hasLayoutFallback() && e->getFallbackLocation() == nullptr && e->getLocation()->x == b.x );
	deleteInstance( e );
	// N: no fallback at all
	e = newInstance( BuildListInfo );
	e->setLocation( a );
	GX_LFB_ARM( "N", !e->gxUseLayoutFallback() && e->getLocation()->x == a.x && !e->hasFallbackSpot() );
	deleteInstance( e );
	// R: a layout fallback replaced by a supply centre's today's spot is a supply fallback from then on
	e = newInstance( BuildListInfo );
	e->setLocation( a ); e->gxSetLayoutFallback( b, 1.0f ); e->setFallbackSpot( c, 2.0f );
	GX_LFB_ARM( "R", !e->gxUseLayoutFallback() && e->getLocation()->x == a.x && e->hasFallbackSpot() && e->getFallbackLocation()->x == c.x );
	deleteInstance( e );
	// D: a list without GXTurnLayout drops a layout fallback ...
	e = newInstance( BuildListInfo );
	e->setLocation( a ); e->gxSetLayoutFallback( b, 1.0f ); e->gxDropLayoutFallback();
	GX_LFB_ARM( "D", !e->gxUseLayoutFallback() && e->getFallbackLocation() == nullptr && e->getLocation()->x == a.x );
	deleteInstance( e );
	// K: ... and keeps a supply centre's
	e = newInstance( BuildListInfo );
	e->setLocation( a ); e->setFallbackSpot( b, 1.0f ); e->gxDropLayoutFallback();
	GX_LFB_ARM( "K", e->hasFallbackSpot() && e->getFallbackLocation() != nullptr && e->getFallbackLocation()->x == b.x );
	deleteInstance( e );
	#undef GX_LFB_ARM
	fprintf( stderr, "[GXROT] ev=layoutfb_fixture pass=%d fail=%d failed=%s\n", pass, fail, fail ? failed : "-" );
	fflush( stderr );
}
#endif


#define SUPPLY_CENTER_CLOSE_DIST (20*PATHFIND_CELL_SIZE_F)

// GeneralsX: Gatherers on a supply centre that stands on its pro spot (AIPlayer::buildBySupplies,
// findDockFacePlacement): the free truck + one.
static const Int AI_PRO_SPOT_GATHERERS = 2;

#define USE_DOZER 1

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
AIPlayer::AIPlayer( Player *p ) :
m_player(p),
m_buildDelay(0),
m_teamDelay(0),
m_teamTimer(2),	// Important - don't start building teams until frame 1.
m_structureTimer(2), // Important - don't start building structures until frame 1.
m_readyToBuildTeam(false),
m_readyToBuildStructure(false),
m_structuresInQueue(0),
m_repairDozer(INVALID_ID),
m_skillsetSelector(INVALID_SKILLSET_SELECTION),
m_dozerQueuedForRepair(false),
m_supplySourceAttackCheckFrame(0),
m_attackedSupplyCenter(INVALID_ID),
m_teamSeconds(10),
m_curWarehouseID(INVALID_ID)
{
	m_frameLastBuildingBuilt = TheGameLogic->getFrame();
	p->setCanBuildUnits(false); // turn off ai production by default.
	m_gxFaceFront = FALSE;	// AI_PLAYER_FACE_FRONT, off until the scb turns it on
	m_gxAirWatchRadius = 0;	// off until the scb turns it on
	m_gxAirGuardCount = 0;	// no AA guard sent out, C not evaluated yet
	m_gxAirC = FALSE;
	m_gxAirCAirfield = FALSE;
	m_gxAirCKnown = FALSE;
	m_gxAirSeenFrame = 0;
	m_gxAirPrevRaidCount = 0;

	Int i;
	for (i=0; i<MAX_STRUCTURES_TO_REPAIR; i++) {
		m_structuresToRepair[i] = INVALID_ID;
	}
	m_repairDozerOrigin.zero();
	m_baseCenter.zero();
	m_baseCenterSet = false;
	m_difficulty = TheScriptEngine->getGlobalDifficulty();
	m_teamSeconds = TheAI->getAiData()->m_teamSeconds;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
AIPlayer::~AIPlayer()
{
	clearTeamsInQueue();
}

// ------------------------------------------------------------------------------------------------
/** Invoked when a structure I am building is finished building. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::onStructureProduced( Object *factory, Object *bldg )
{
	m_teamDelay = 0; // Cause the update queues & selection to happen immediately.
	m_buildDelay = 0; // Cause
	/* Find the info building this. */
	BuildListInfo *info;
	for( info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->getObjectID() != bldg->getID()) continue;
		Dict d;
		d.setAsciiString(TheKey_objectName, info->getBuildingName());
		d.setAsciiString(TheKey_objectScriptAttachment, info->getScript());
		// TheSuperHackers @bugfix bobtista 31/10/2025 Preserve scaffold health when AI building construction completes instead of resetting to initial health.
#if RETAIL_COMPATIBLE_CRC
		d.setInt(TheKey_objectInitialHealth, info->getHealth());
#endif
		d.setBool(TheKey_objectUnsellable, info->getUnsellable());

		info->setUnderConstruction(false);
		bldg->updateObjValuesFromMapProperties(&d);
		// clear the under construction status
		bldg->clearStatus( MAKE_OBJECT_STATUS_MASK2( OBJECT_STATUS_UNDER_CONSTRUCTION, OBJECT_STATUS_RECONSTRUCTING ) );

		// UnderConstruction just cleared, so update our upgrades
		bldg->updateUpgradeModules();

		TheScriptEngine->addObjectToCache(bldg);
		TheScriptEngine->runObjectScript(info->getScript(), bldg);
		if (TheGlobalData->m_debugAI) {
			AsciiString bldgName = bldg->getTemplate()->getName();
			bldgName.concat(" - Building completed.");
			TheScriptEngine->AppendDebugMessage(bldgName, false);
		}
		checkForSupplyCenter(info, bldg);
		return;
	}

	// Look in build list & see if this is spawned from a hole.
	for( info = m_player->getBuildList(); info; info = info->getNext() )
	{
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( info->getTemplateName() );
		if (!bldgPlan) {
			continue;
		}
		if (!bldgPlan->isEquivalentTo(bldg->getTemplate())) {
			continue; // not the same kind of building we're looking for.
		}
		// check for hole.
		if (info->getObjectID() != INVALID_ID) {
			// used to have a building.
			Object *obj = TheGameLogic->findObjectByID( info->getObjectID() );
			if (obj!=nullptr) {
				if (obj->isKindOf(KINDOF_REBUILD_HOLE)) {
					RebuildHoleBehaviorInterface *rhbi = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject( obj );
					if( rhbi ) {
						ObjectID spawnedID = rhbi->getReconstructedBuildingID();
						if (bldg->getID() == spawnedID) {
							DEBUG_LOG(("AI got rebuilt %s", bldgPlan->getName().str()));
							info->setObjectID(bldg->getID());
							return;
						}
					}
				}
			}
		}
	}

	if (TheGameLogic->getFrame()>0) {
		DEBUG_LOG(("***AI PLAYER-Structure not found in production queue."));
	}
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: AIData's ResourceGatherers for a side and difficulty (the lookup checkForSupplyCenter
// always did), shared with queueSupplyTruck's player-wide ceiling, which must not shrink with a pro-spot centre's own target.
// ------------------------------------------------------------------------------------------------
static Int dataGatherers(const AsciiString &side, GameDifficulty difficulty)
{
	Int desiredGatherers = 0;
	const AISideInfo *resInfo = TheAI->getAiData()->m_sideInfo;
	while (resInfo) {
		if (resInfo->m_side == side) {
			if (difficulty == DIFFICULTY_EASY) {
				desiredGatherers = resInfo->m_easy;
			}
			if (difficulty == DIFFICULTY_NORMAL) {
				desiredGatherers = resInfo->m_normal;
			}
			if (difficulty == DIFFICULTY_HARD) {
				desiredGatherers = resInfo->m_hard;
			}
		}
		resInfo = resInfo->m_next;
	}
	return desiredGatherers;
}

// ------------------------------------------------------------------------------------------------
/** See if the building is a supply center, and see how many supply trucks we want. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::checkForSupplyCenter( BuildListInfo *info, Object *bldg )
{
	class SupplyCenterDockUpdate;
	// if it is a supply center, I must have boxes
	static const NameKeyType key_centerUpdate = NAMEKEY("SupplyCenterDockUpdate");
	SupplyCenterDockUpdate *centerModule = (SupplyCenterDockUpdate*)bldg->findUpdateModule( key_centerUpdate );
	if( centerModule  )
	{
		info->setSupplyBuilding(true);
		const Int desiredGatherers = dataGatherers(m_player->getSide(), m_difficulty);
		info->setSupplyBuilding(true);
		info->setCurrentGatherers(-1);
		info->setDesiredGatherers(desiredGatherers+1); // get a freebie with the supply depots.
		// GeneralsX: A centre on its pro spot (the entry still holds today's spot as an unused fallback,
		// SidesList.h) runs exactly two trucks: both park in the gap and keep the dock busy, a third only queues (measured,
		// ai/supply-two). A centre that fell back to today's spot keeps today's count: two trucks there earn a quarter less.
		if (info->hasFallbackSpot())
			info->setDesiredGatherers(AI_PRO_SPOT_GATHERERS);
	}
}

// ------------------------------------------------------------------------------------------------
/** Queue up a supply truck to be built. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::queueSupplyTruck()
{
	Bool truckInQueue = false;
	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		WorkOrder *order;
		for( order = team->m_workOrders; order; order = order->m_next )
		{
			// GLA dozers (workers) are also resource gatherers, so make sure it isn't a worker. jba.
			if (order->m_isResourceGatherer) {
				truckInQueue = true;
			}
		}
	}

	if (truckInQueue) {
		return; // already building a supply truck.
	}
	Int totalHarvesters = 0;

	// See how many harvesters we have servicing this supply src.
	// Scan my units.
	Player::PlayerTeamList::const_iterator it;
	for (it = m_player->getPlayerTeams()->begin(); it != m_player->getPlayerTeams()->end(); ++it) {
		for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance()) {
			Team *team = iter.cur();
			if (!team) {
				continue;
			}
			for (DLINK_ITERATOR<Object> objIter = team->iterate_TeamMemberList(); !objIter.done(); objIter.advance()) {
				Object *obj = objIter.cur();
				if (!obj)  continue;
				if (!obj->isKindOf(KINDOF_HARVESTER)) continue;
				if (!obj->getAI()) continue;

				SupplyTruckAIInterface* supplyTruckAI = obj->getAI()->getSupplyTruckAIInterface();
				if( supplyTruckAI )	{
					totalHarvesters++;
				}
			}
		}
	}

	/* Find the info building this. */
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		if (info->isSupplyBuilding() == false) continue;
		Int desiredGatherers = info->getDesiredGatherers();
		Int curGatherers = info->getCurrentGatherers();

		if (curGatherers>=desiredGatherers) {
			// Check & see if any have died.
			Object *supplyCenter = TheGameLogic->findObjectByID(info->getObjectID());
			// Check for supplies.
			if (supplyCenter) {
				if (supplyCenter->isKindOf(KINDOF_REBUILD_HOLE)) {
					continue; // don't consider rebuild holes.
				}
				// Make sure we have a supplies near it.
				Coord3D center = *supplyCenter->getPosition();
				Real radius = SUPPLY_CENTER_CLOSE_DIST + supplyCenter->getGeometryInfo().getBoundingCircleRadius();

				PartitionFilterAcceptByKindOf f1(MAKE_KINDOF_MASK(KINDOF_SUPPLY_SOURCE), KINDOFMASK_NONE);
				PartitionFilterPlayer f2(m_player, false);	// Only find other.
				PartitionFilterOnMap filterMapStatus;

				PartitionFilter *filters[] = { &f1, &f2, &filterMapStatus, nullptr };

				Object *supplySource = ThePartitionManager->getClosestObject(&center, radius, FROM_BOUNDINGSPHERE_2D, filters);
				if (!supplySource) {
					// No supplies.
					continue;
				}
				static const NameKeyType key_warehouseUpdate = NAMEKEY("SupplyWarehouseDockUpdate");
				SupplyWarehouseDockUpdate *warehouseModule = (SupplyWarehouseDockUpdate*)supplySource->findUpdateModule( key_warehouseUpdate );
				if( warehouseModule )	{
					Int availableCash = warehouseModule->getBoxesStored()*TheGlobalData->m_baseValuePerSupplyBox;
					if (availableCash<=0) continue;
					if( m_player->getRelationship(supplySource->getTeam()) == ENEMIES ) {
						continue;
					}
				}
				// Ok, it has supplies available near it.
				checkForSupplyCenter(info, supplyCenter);
				Int curGatherers = 0;
				// See how many harvesters we have servicing this supply src.
				// Scan my units.
				Player::PlayerTeamList::const_iterator it;
				for (it = m_player->getPlayerTeams()->begin(); it != m_player->getPlayerTeams()->end(); ++it) {
					for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance()) {
						Team *team = iter.cur();
						if (!team) {
							continue;
						}
						for (DLINK_ITERATOR<Object> objIter = team->iterate_TeamMemberList(); !objIter.done(); objIter.advance()) {
							Object *obj = objIter.cur();
							if (!obj)  continue;
							if (!obj->isKindOf(KINDOF_HARVESTER)) continue;
							if (!obj->getAI()) continue;

							SupplyTruckAIInterface* supplyTruckAI = obj->getAI()->getSupplyTruckAIInterface();
							if( supplyTruckAI )	{
								ObjectID dock = supplyTruckAI->getPreferredDockID();
								if (dock == supplyCenter->getID()) {
									curGatherers++;
									if (!supplyTruckAI->isCurrentlyFerryingSupplies()) {
										// Note - although this is the ai, we are sending in CMD_FROM_PLAYER.
										// This causes the dock object to stick in the docking interface.
										// The supply truck ai issues dock commands, and they become confused.
										// Thus, player.  jba.  ;(
										obj->getAI()->aiDock(supplyCenter, CMD_FROM_PLAYER);
									}
								}
							}
						}
					}
				}
				//DEBUG_LOG(("Expected %d harvesters, found %d, need %d", info->getDesiredGatherers(),
				//	curGatherers, info->getDesiredGatherers()-curGatherers) );
				info->setCurrentGatherers(curGatherers);
			}
		} else {
			/* See if we have any "loose" harvesters (cause my supply center got nuked.) */
			Player::PlayerTeamList::const_iterator it;
			for (it = m_player->getPlayerTeams()->begin(); it != m_player->getPlayerTeams()->end(); ++it) {
				for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance()) {
					Team *team = iter.cur();
					if (!team) continue;
					for (DLINK_ITERATOR<Object> objIter = team->iterate_TeamMemberList(); !objIter.done(); objIter.advance()) {
						Object *obj = objIter.cur();
						if (!obj)  continue;
						if (!obj->isKindOf(KINDOF_HARVESTER)) continue;
						if (!obj->getAI()) continue;

						SupplyTruckAIInterface* supplyTruckAI = obj->getAI()->getSupplyTruckAIInterface();
						if( supplyTruckAI )	{
							ObjectID dock = supplyTruckAI->getPreferredDockID();
							if (TheGameLogic->findObjectByID(dock)!=nullptr) continue;
							if (supplyTruckAI->isCurrentlyFerryingSupplies() || supplyTruckAI->isForcedIntoWantingState())
							{
								// This thinks he is a gatherer, but doesn't have a preferred dock id.
								Object *center = TheGameLogic->findObjectByID(info->getObjectID());
								if (center) {
									info->setCurrentGatherers(info->getCurrentGatherers()+1);
									// Note - although this is the ai, we are sending in CMD_FROM_PLAYER.
									// This causes the dock object to stick in the docking interface.
									// The supply truck ai issues dock commands, and they become confused.
									// Thus, player.  jba.  ;(
									obj->getAI()->aiDock(center, CMD_FROM_PLAYER);
									DEBUG_LOG(("Re-attaching supply truck to supply center."));
									return;
								}
							}
						}
					}
				}
			}
			// GeneralsX: the player-wide ceiling stays today's (data + the free truck, x3): a pro-spot
			// centre's own target of two must not lower it, or a pro centre beside older fallback centres never gets its
			// second truck.
			const Int ceilingPerCentre = info->hasFallbackSpot() ? dataGatherers(m_player->getSide(), m_difficulty) + 1 : desiredGatherers;
			if (totalHarvesters >= ceilingPerCentre*3) {
				continue; // we got lotsa gatherers.
			}
			Bool canBuildUnits = m_player->getCanBuildUnits();
			// If we need a supply truck thingy, turn on unit building for a moment.
			m_player->setCanBuildUnits(true);
			const ThingTemplate *tTemplate = TheThingFactory->firstTemplate();
			while (tTemplate) {
				Bool isSupplyTruck = tTemplate->isKindOf(KINDOF_HARVESTER);
				if (isSupplyTruck) {
					Object *factory = findFactory(tTemplate, false);
					if (factory) {
						// we can build one.
						WorkOrder *order = newInstance(WorkOrder);
						order->m_thing = tTemplate;
						order->m_factoryID = INVALID_ID;
						order->m_numRequired = 1;
						order->m_required = true;
						order->m_isResourceGatherer =true;
						// prepend to head of list
						order->m_next = nullptr;
						TeamInQueue *team = newInstance(TeamInQueue);
						// Put in front of queue.
						prependTo_TeamBuildQueue(team);
						team->m_priorityBuild = true;
						team->m_workOrders = order;
						team->m_frameStarted = TheGameLogic->getFrame();
						// Stick it on the default team
						team->m_team = m_player->getDefaultTeam();
						AsciiString teamName = "Supply truck - building one at the ";
						teamName.concat(factory->getTemplate()->getName());
						TheScriptEngine->AppendDebugMessage(teamName, false);
						m_teamDelay = 0;
						if (info->getCurrentGatherers()==-1) {
							// First one is automatic. jba.
							order->m_factoryID = factory->getID();
							info->setCurrentGatherers(0);
						}	else {
							startTraining( order, team->m_priorityBuild, team->m_team->getName());
						}
						break;
					}
				}
				tTemplate = tTemplate->friend_getNextTemplate();
			}
			m_player->setCanBuildUnits(canBuildUnits);
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void deleteQueue(TeamInQueue* o)
{
	if (o)
	{
		deleteInstance(o);
	}
}

// ------------------------------------------------------------------------------------------------
/** Clear the current work order */
// ------------------------------------------------------------------------------------------------
void AIPlayer::clearTeamsInQueue()
{
	removeAll_TeamBuildQueue(deleteQueue);
	removeAll_TeamReadyQueue(deleteQueue);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Object *AIPlayer::buildStructureNow(const ThingTemplate *bldgPlan, BuildListInfo *info)
{

	// inst-construct the building
	Object *bldg = TheBuildAssistant->buildObjectNow( nullptr,
																						bldgPlan,
																						info->getLocation(),
																						info->getAngle(),
																						m_player );

	// store the object with the build order
	if (bldg)
	{
		Dict d;
		d.setAsciiString(TheKey_objectName, info->getBuildingName());
		d.setAsciiString(TheKey_objectScriptAttachment, info->getScript());
		// TheSuperHackers @bugfix bobtista 31/10/2025 Preserve scaffold health when AI building construction completes instead of resetting to initial health.
#if RETAIL_COMPATIBLE_CRC
		d.setInt(TheKey_objectInitialHealth, info->getHealth());
#endif
		d.setBool(TheKey_objectUnsellable, info->getUnsellable());

		bldg->updateObjValuesFromMapProperties(&d);

		info->setObjectID( bldg->getID() );
		info->setObjectTimestamp( TheGameLogic->getFrame()+1 );	// has to be non-zero, so just add 1.

		// clear the under construction status
		bldg->clearStatus( MAKE_OBJECT_STATUS_MASK2( OBJECT_STATUS_UNDER_CONSTRUCTION, OBJECT_STATUS_RECONSTRUCTING ) );

		// UnderConstruction just cleared, so update our upgrades
		bldg->updateUpgradeModules();

		if (TheGlobalData->m_debugAI) {
			AsciiString bldgName = bldgPlan->getName();
			bldgName.concat(" - Building completed.");
			TheScriptEngine->AppendDebugMessage(bldgName, false);
		}
		TheScriptEngine->addObjectToCache(bldg);
		TheScriptEngine->runObjectScript(info->getScript(), bldg);
		checkForSupplyCenter(info, bldg);
		ExitInterface *exitInterface = bldg->getObjectExitInterface();
		if( exitInterface )
		{
			Coord3D rallyPoint;
			Bool gotOffset = false;
			if (fabs(info->getRallyOffset()->x) > 1.0f || fabs(info->getRallyOffset()->y)>1.0f) {
				gotOffset;
			}
			if (!exitInterface->getNaturalRallyPoint(rallyPoint)) {
				rallyPoint = *info->getLocation();
			}
			if (gotOffset) {
				rallyPoint.x += info->getRallyOffset()->x;
				rallyPoint.y += info->getRallyOffset()->y;
				exitInterface->setRallyPoint(&rallyPoint);
			}
		}
	}
	return bldg;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Object *AIPlayer::buildStructureWithDozer(const ThingTemplate *bldgPlan, BuildListInfo *info)
{
	// Find a dozer.
	Object *dozer = findDozer(info->getLocation());
	if (dozer==nullptr) {
		return nullptr;
	}
	// Check available funds.
	Money *money = m_player->getMoney();
	if (money->countMoney()<bldgPlan->calcCostToBuild(m_player)) {
		return nullptr;
	}
	// construct the building
	Coord3D pos = *info->getLocation();
	pos.z += TheTerrainLogic->getGroundHeight(pos.x, pos.y);
	if( !dozer->getAIUpdateInterface() )
	{
		return nullptr;
	}
	Real angle = info->getAngle();
	Int gxDoorTurned = -1;	// -1 = no door candidate, else 0 / 1 = list angle / turned ([GXROT] ev=placed)
	Bool gxSearchAtWant = FALSE;	// the position search below starts at the door rule's angle
	// With AI_PLAYER_FACE_FRONT on, a barracks / war factory / arms dealer is turned so its door - model +X, where every production
	// building's NaturalRallyPoint sits - points at the base front (the point front defences use). Only an angle legal at this very
	// spot is taken (the octant, then +/-45 deg); else the list angle, as before - the position search below is untouched.
	const Bool gxKeyedDoors = m_gxFaceFront && isSkirmishAI() && gxFrontDoors();	// GXFrontDoors
	if( m_gxFaceFront && isSkirmishAI() && gxDoorKind( bldgPlan, gxKeyedDoors ) )
	{
		GX_HOOK_TIMER_ON(gxFaceFrontHook);
		Coord3D front;
		if( gxFrontPoint( &front ) )
		{
			const Real want = gxKeyedDoors ? gxDoorWant( m_baseCenter, bldgPlan, pos, front ) : gxOctantAngle( front.x - pos.x, front.y - pos.y );
			const Real tries[3] = { want, want + PI / 4, want - PI / 4 };
			Bool turned = FALSE;
			const Int nTries = ( gxKeyedDoors && gxAirfieldKeepsList( bldgPlan, want, info->getAngle() ) ) ? 0 : 3;
			for( Int t = 0; t < nTries && !turned; ++t )
			{
				if( gxLegalToBuildOnGround( &pos, bldgPlan, tries[t], BuildAssistant::CLEAR_PATH | BuildAssistant::TERRAIN_RESTRICTIONS |
						BuildAssistant::NO_OBJECT_OVERLAP, dozer, m_player ) == LBC_OK )
				{
					angle = tries[t];
					turned = TRUE;
				}
			}
			// No try fits this spot and the list angle is more than a quarter turn off the rule (a defence ring's clone keeps its "outward" there):
			// the door would face the rear wherever it lands, so the position search below runs at the rule's angle first. Its own passes fall
			// back to the list angle if that finds nothing. An enemy on the rule's box keeps the list angle: the enemy gate below returns
			// early and would never reach those passes.
			gxSearchAtWant = gxKeyedDoors && !turned && !bldgPlan->isKindOf( KINDOF_FS_AIRFIELD ) && fabs( normalizeAngle( want - info->getAngle() ) ) > PI / 2 + 0.001f
				&& gxLegalToBuildOnGround( &pos, bldgPlan, want, BuildAssistant::NO_ENEMY_OBJECT_OVERLAP, dozer, m_player ) == LBC_OK;
			if( gxSearchAtWant )
			{
				angle = want;
			}
			TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
			GX_ROT_LOG( "p=%d ev=door tmpl=%s list=%d want=%d got=%d turned=%d rule=%d", (Int)m_player->getPlayerIndex(), bldgPlan->getName().str(),
				REAL_TO_INT( info->getAngle() * 180.0f / PI ), REAL_TO_INT( want * 180.0f / PI ), REAL_TO_INT( angle * 180.0f / PI ), (Int)turned, (Int)gxKeyedDoors );
			gxDoorTurned = ( turned || gxSearchAtWant ) ? 1 : 0;
		}
	}
 	if( gxLegalToBuildOnGround( &pos, bldgPlan, angle,
																								 BuildAssistant::NO_ENEMY_OBJECT_OVERLAP,
																								 dozer, m_player ) != LBC_OK ) {
		// If there's enemy units or structures, don't build/rebuild.
		TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
		// A supply centre refused on its dock face moves to its stock spot and tries it at once (one level deep: useFallbackSpot is
		// one-shot; see AIPlayer::buildBySupplies), but only when that spot passes the same isLocationSafe gate as
		// AISkirmishPlayer::processBaseBuilding: an entry moved to an unsafe spot would wait there and lose the face for nothing.
		// A keyed entry's layout fallback, the stock spot, is taken here the same way.
		const Coord3D *fallback = info->getFallbackLocation();
		if (fallback != nullptr && isLocationSafe(fallback, bldgPlan)) {
			if (info->hasLayoutFallback())
				GX_ROT_LOG("p=%d ev=fallback why=enemy tmpl=%s x=%.2f y=%.2f", (Int)m_player->getPlayerIndex(), bldgPlan->getName().str(), fallback->x, fallback->y);
			info->useFallbackSpot();
			return buildStructureWithDozer(bldgPlan, info);
		}
		return nullptr;
	}

	// validate the the position to build at is valid
	if( gxLegalToBuildOnGround( &pos, bldgPlan, angle,
																								 BuildAssistant::CLEAR_PATH |
																								 BuildAssistant::TERRAIN_RESTRICTIONS |
																								 BuildAssistant::NO_OBJECT_OVERLAP,
																								 dozer, m_player ) != LBC_OK ) {
			// A dock-face spot that is no longer legal: the stock spot instead of a wiggle around the face (see above).
			// Not a keyed entry's layout fallback: a keyed spot that is only illegal searches around itself first, as before,
			// and moves to the stock spot only when that search finds nothing (below).
			if (!info->hasLayoutFallback() && info->useFallbackSpot()) {
				TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
				// GeneralsX: the same safety gate as above.
				return isLocationSafe(info->getLocation(), bldgPlan) ? buildStructureWithDozer(bldgPlan, info) : nullptr;
			}
			// Warn.
			AsciiString bldgName = bldgPlan->getName();
			bldgName.concat(" - Dozer unable to place.  Attempting to adjust position.");
			TheScriptEngine->AppendDebugMessage(bldgName, false);
			// try to fix.
			Real posOffset;
			Bool valid = false;
			// Wiggle it a little :)
			Real limit = 10*PATHFIND_CELL_SIZE_F;
			if (isSkirmishAI()) {
				limit = 120*PATHFIND_CELL_SIZE_F;
			}
			Coord3D newPos = pos;
			// GeneralsX: A skirmish AI searches the same rings a second time without
			// CLEAR_PATH before giving up, the build-list spot first: reaching the spot is not required, because an
			// unreachable dozer is teleported below (a dozer boxed in by its own base reaches nothing at all).
			UnsignedInt flags = BuildAssistant::CLEAR_PATH | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP;
			const Int passes = gxSearchAtWant ? 4 : ( isSkirmishAI() ? 2 : 1 );
			for (Int pass = 0; pass < passes && !valid; ++pass) {
				if (pass == 2) {	// the rule's angle found nothing; both passes again at the list angle, its own spot first
					angle = info->getAngle();
					gxDoorTurned = 0;
					flags |= BuildAssistant::CLEAR_PATH;
					newPos = pos;
					valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
					if (valid) break;
				}
				if (pass == 1 || pass == 3) {
					flags &= ~BuildAssistant::CLEAR_PATH;
					newPos = pos;
					valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
					if (valid) break;
				}
				for (posOffset = 0; posOffset<limit; posOffset += 2*PATHFIND_CELL_SIZE_F) {
					if (isSkirmishAI()) {
						posOffset += 2*PATHFIND_CELL_SIZE_F;
					}
					Real offset = posOffset/2;
					Real xPos, yPos;
					yPos = pos.y-offset;
					for (xPos = pos.x-offset; xPos <= pos.x+offset; xPos+=PATHFIND_CELL_SIZE_F) {
						if (isSkirmishAI()) xPos += PATHFIND_CELL_SIZE_F;
						newPos.x = xPos;
						newPos.y = yPos;
						valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
						if (valid) break;
						newPos.y = yPos+posOffset;
						valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
						// GeneralsX: Stop on this candidate too. Without the break the next step
						// overwrote a legal top- (here) or right-edge (below) candidate, so bar each ring's last one only the
						// bottom and left edges counted: a base near a map edge or corner found no spot and fell back to
						// building over its own structures (Mayhem - Defcon 51).
						if (valid) break;
					}
					if (valid) break;
					xPos = pos.x-offset;
					for (yPos = pos.y-offset; yPos <= pos.y+offset; yPos+=PATHFIND_CELL_SIZE_F) {
						if (isSkirmishAI()) yPos += PATHFIND_CELL_SIZE_F;
						newPos.x = xPos;
						newPos.y = yPos;
						valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
						if (valid) break;
						newPos.x = xPos+posOffset;
						valid = gxLegalToBuildOnGround( &newPos, bldgPlan, angle, flags, dozer, m_player ) == LBC_OK;
						if (valid) break;	// GeneralsX: See the top-edge break above.
					}
					if (valid) break;
				}
			}
			if (valid) pos = newPos;
			if (!valid) {
				// GeneralsX: With no legal spot found, retail builds on the build-list spot
				// anyway and refuses it only for enemies. A skirmish AI's spot is a template offset from its Command
				// Center, so that put buildings on its own structures, on neutral supply docks and over cliffs.
				// Both passes above found nothing, so a skirmish AI retries this entry after the rebuild
				// delay instead: it neither starves the rest of its build list nor repeats this search every pass.
				// A mission AI keeps the retail fallback; its spots are designer-placed.
				if (isSkirmishAI()) {
					TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
					// A keyed entry whose search found nothing moves to the stock list's spot for it and is tried there at once, one level
					// deep (the move uses the fallback up). Only a fallback from the layout data: a supply centre's stock spot, or none,
					// waits here as before (see gxLayoutFallbackFixture's arm S).
					if (info->gxUseLayoutFallback()) {
						GX_ROT_LOG("p=%d ev=fallback why=nothing tmpl=%s x=%.2f y=%.2f", (Int)m_player->getPlayerIndex(), bldgPlan->getName().str(),
							info->getLocation()->x, info->getLocation()->y);
						return isLocationSafe(info->getLocation(), bldgPlan) ? buildStructureWithDozer(bldgPlan, info) : nullptr;
					}
					info->setObjectTimestamp( TheGameLogic->getFrame()+1 );	// Has to be non-zero, so add 1.
					return nullptr;
				}
				valid = gxLegalToBuildOnGround( &pos, bldgPlan, angle,
																						 BuildAssistant::NO_ENEMY_OBJECT_OVERLAP,
																						 dozer, m_player ) == LBC_OK;
				if (!valid) {
					return nullptr;
				}
			}

	}

	TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
	// GeneralsX: Hand on the spot as tested: on the ground. The searches
	// above test grounded copies, but a candidate still carries its start spot's height, and the dozer's and the worker's
	// construct() re-check this exact position for a computer player - at the stale height they could refuse the spot the search
	// accepted (an elevated object overlapping the box tested up there), and the AI would pick the same spot again every pass.
	// construct() grounds the new structure itself, so only that re-check sees the change.
	pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
	if (!TheAI->pathfinder()->clientSafeQuickDoesPathExist(dozer->getAI()->getLocomotorSet(),
		dozer->getPosition(), &pos)) {
		AsciiString bldgName = bldgPlan->getName();
		bldgName.concat(" - Dozer unable to reach building.  Teleporting.");
		TheScriptEngine->AppendDebugMessage(bldgName, false);
		dozer->setPosition(&pos);
	}

	Object *bldg = TheBuildAssistant->buildObjectNow( dozer,
																						bldgPlan,
																						&pos,
																						angle,
																						m_player );
	// The PLACED structure: the object exists now. The readers count these; ev=door above and AISkirmishPlayer's ev=defence are
	// attempts, made before the legality checks that can still refuse the spot.
	// A defence was queued with its facing as the build-list angle: turned = that angle is not the template's view angle.
	if( bldg && m_gxFaceFront && ( gxDoorTurned >= 0 || bldgPlan->isKindOf( KINDOF_FS_BASE_DEFENSE ) ) )
	{
		GX_ROT_LOG( "p=%d ev=placed kind=%s tmpl=%s id=%u angle=%d turned=%d", (Int)m_player->getPlayerIndex(), gxDoorTurned >= 0 ? "door" : "defence",
			bldgPlan->getName().str(), (unsigned)bldg->getID(), REAL_TO_INT( angle * 180.0f / PI ),
			gxDoorTurned >= 0 ? gxDoorTurned : (Int)( angle != bldgPlan->getPlacementViewAngle() ) );
	}



#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI == AI_DEBUG_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = 1;
		color.green = 0;
		Coord3D myPos;
		myPos = *dozer->getPosition();
		myPos.z = TheTerrainLogic->getGroundHeight( myPos.x, myPos.y ) + 0.5f;
		addIcon(&myPos, 2*PATHFIND_CELL_SIZE_F, 120, color);
		myPos = pos;
		myPos.z = TheTerrainLogic->getGroundHeight( myPos.x, myPos.y ) + 0.5f;
		addIcon(&myPos, 2*PATHFIND_CELL_SIZE_F, 120, color);
		Real dx, dy;
		dx = dozer->getPosition()->x - pos.x;
		dy = dozer->getPosition()->y - pos.y;

		Int count = sqrt(dx*dx+dy*dy)/(PATHFIND_CELL_SIZE_F/2);
		if (count<2) count = 2;
		Int i;
		color.green = 1;
		for (i=1; i<count; i++) {
			myPos.x = dozer->getPosition()->x + (pos.x-dozer->getPosition()->x)*i/count;
			myPos.y = dozer->getPosition()->y + (pos.y-dozer->getPosition()->y)*i/count;
			myPos.z = TheTerrainLogic->getGroundHeight( myPos.x, myPos.y ) + 0.5f;
			addIcon(&myPos, PATHFIND_CELL_SIZE_F/2, 120, color);

		}
	}
#endif

	// store the object with the build order
	if (bldg)
	{
		ExitInterface *exitInterface = bldg->getObjectExitInterface();
		if( exitInterface )
		{
			Coord3D rallyPoint;
			Bool gotOffset = false;
			if (fabs(info->getRallyOffset()->x) > 1.0f || fabs(info->getRallyOffset()->y)>1.0f) {
				gotOffset;
			}
			if (!exitInterface->getNaturalRallyPoint(rallyPoint)) {
				rallyPoint = *info->getLocation();
			}
			if (gotOffset) {
				rallyPoint.x += info->getRallyOffset()->x;
				rallyPoint.y += info->getRallyOffset()->y;
				exitInterface->setRallyPoint(&rallyPoint);
			}
		}
		info->setObjectID( bldg->getID() );
		info->setObjectTimestamp( TheGameLogic->getFrame()+1 );	// Has to be non-zero, so add 1.
		info->setUnderConstruction(true);

		if (TheGlobalData->m_debugAI) {
			AsciiString bldgName = bldgPlan->getName();
			bldgName.concat(" - Building started.");
			TheScriptEngine->AppendDebugMessage(bldgName, false);
		}
	}
	TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
	return bldg;
}

// ------------------------------------------------------------------------------------------------
/** Build our base. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::processBaseBuilding()
{
	//
	// Refresh base buildings. Scan through list, if a building is missing,
	// rebuild it, unless it's rebuild count is zero.
	//
	if (m_readyToBuildStructure)
	{

		for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
		{
			AsciiString name = info->getTemplateName();
			if (name.isEmpty()) continue;
			const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
			if (!bldgPlan) {
				DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
				continue;
			}
			// check for hole.
			if (info->getObjectID() != INVALID_ID) {
				// used to have a building.
				Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );
				if (bldg==nullptr) {
					// got destroyed.
					ObjectID priorID;
					priorID = info->getObjectID();
					info->setObjectID(INVALID_ID);
					info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					// Scan for a GLA hole.	KINDOF_REBUILD_HOLE
					Object *obj;
					for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() ) {
						if (!obj->isKindOf(KINDOF_REBUILD_HOLE)) continue;
						RebuildHoleBehaviorInterface *rhbi = RebuildHoleBehavior::getRebuildHoleBehaviorInterfaceFromObject( obj );
						if( rhbi ) {
							ObjectID spawnerID = rhbi->getSpawnerID();
							if (priorID == spawnerID) {
								DEBUG_LOG(("AI Found hole to rebuild %s", bldgPlan->getName().str()));
								info->setObjectID(obj->getID());
							}
						}
 					}
				}	else {
					if (bldg->getControllingPlayer() == m_player) {
						// Check for built or dozer missing.
						if( bldg->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
						{
							// make sure dozer is working on him.
							ObjectID builder = bldg->getBuilderID();
							Object* myDozer = TheGameLogic->findObjectByID(builder);
							if (myDozer==nullptr) {
								DEBUG_LOG(("AI's Dozer got killed.  Find another dozer."));
 								myDozer = findDozer(bldg->getPosition());
								if (myDozer==nullptr || myDozer->getAI()==nullptr) {
									continue;
								}
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}	else {
								// make sure he is building.
								myDozer->getAI()->aiResumeConstruction(bldg, CMD_FROM_AI);
							}
						}
					} else {
						// oops, got captured.
						info->setObjectID(INVALID_ID);
						info->setObjectTimestamp(TheGameLogic->getFrame()+1);
					}
				}
			}
			if (info->getObjectID()==INVALID_ID && info->getObjectTimestamp()>0) {
				// this object was built at some time, and got destroyed at or near objectTimestamp.
				// Wait a few seconds before initiating a rebuild.
				if (info->getObjectTimestamp()+TheAI->getAiData()->m_rebuildDelaySeconds*LOGICFRAMES_PER_SECOND > TheGameLogic->getFrame()) {
					continue;
				}	else {
					DEBUG_LOG(("Enabling rebuild for %s", info->getTemplateName().str()));
					info->setObjectTimestamp(0); // ready to build.
				}
			}
			// check if this building has any "rebuilds" left
			if (info->isBuildable())
			{
				Object *bldg = TheGameLogic->findObjectByID( info->getObjectID() );

				if (bldg == nullptr)
				{


#ifdef USE_DOZER
					// dozer-construct the building
					bldg = buildStructureWithDozer(bldgPlan, info);
					// store the object with the build order
					if (bldg)
					{
						info->setObjectID( bldg->getID() );
						info->decrementNumRebuilds();

						m_readyToBuildStructure = false;
						m_structureTimer = TheAI->getAiData()->m_structureSeconds*static_cast<float>(LOGICFRAMES_PER_SECOND);
						if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
						}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
							m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
						}
						m_frameLastBuildingBuilt = TheGameLogic->getFrame();
						// only build one building per delay loop
						break;
					}

#else
					// force delay between rebuilds
					Int framesToBuild = bldgPlan->calcTimeToBuild(m_player);
					if (TheGameLogic->getFrame() - m_frameLastBuildingBuilt < framesToBuild)
					{
						m_buildDelay = framesToBuild - (TheGameLogic->getFrame() - m_frameLastBuildingBuilt);
						return;
					}	else {
						// building is missing, (re)build it
						// deduct money to build, if we have it
						Int cost = bldgPlan->calcCostToBuild( m_player );
						if (m_player->getMoney()->countMoney() >= cost)
						{
							// we have the money, deduct it
							m_player->getMoney()->withdraw( cost );

							// inst-construct the building
							bldg = buildStructureNow(bldgPlan, info);
							// store the object with the build order
							if (bldg)
							{
								info->setObjectID( bldg->getID() );
								info->decrementNumRebuilds();

								m_readyToBuildStructure = false;
								m_structureTimer = TheAI->getAiData()->m_structureSeconds*LOGICFRAMES_PER_SECOND;
								if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
									m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresPoorMod;
								}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
									m_structureTimer = m_structureTimer/TheAI->getAiData()->m_structuresWealthyMod;
								}
								m_frameLastBuildingBuilt = TheGameLogic->getFrame();
								// only build one building per delay loop
								break;
							}
						}
					}
#endif
				}
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** A team is about to be destroyed */
//-------------------------------------------------------------------------------------------------
void AIPlayer::aiPreTeamDestroy( const Team *deletedTeam )
{
	{
		for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); )
		{
			// GeneralsX: Advance only past an entry that stays. After a removal the iterator
			// restarts at the head, and the loop's own advance() then skipped that head: of two consecutive entries for
			// the team, the second stayed queued with a dangling pointer (tsh batch2 #417).
			TeamInQueue *team = iter.cur();
			if (team->m_team == deletedTeam) {
				// The members of the team all got killed before we could finish building the team.
				removeFrom_TeamBuildQueue(team);
				deleteInstance(team);
				iter = iterate_TeamBuildQueue();
			} else {
				iter.advance();
			}
		}
	}
	{
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamReadyQueue(); !iter.done(); )
		{
			// GeneralsX: Same as the build queue above.
			TeamInQueue *team = iter.cur();
			if (team->m_team == deletedTeam) {
				// The members of the team all got killed before we could activate the team.
				removeFrom_TeamReadyQueue(team);
				deleteInstance(team);
				iter = iterate_TeamReadyQueue();
			} else {
				iter.advance();
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Guard supply center */
//-------------------------------------------------------------------------------------------------
void AIPlayer::guardSupplyCenter( Team *team, Int minSupplies )
{
	m_supplySourceAttackCheckFrame = 0; // force check.
	Object *warehouse = nullptr;
	if (isSupplySourceAttacked()) {
		warehouse = TheGameLogic->findObjectByID(m_attackedSupplyCenter);
	}
	if (warehouse==nullptr) {
		warehouse = findSupplyCenter(minSupplies);
	}
	if (warehouse) {

		AIGroupPtr theGroup = TheAI->createGroup();
		if (!theGroup) {
			return;
		}
#if RETAIL_COMPATIBLE_AIGROUP
		team->getTeamAsAIGroup(theGroup);
#else
		team->getTeamAsAIGroup(theGroup.Peek());
#endif
		Coord3D location = *warehouse->getPosition();
		// It's probably a defensive move - position towards the enemy.
		// GeneralsX: getSkirmishEnemyPlayer() may be null (no current script player, or no
		// human in the match: AI-only games, the challenge dummy). Guard the warehouse itself then. tsh batch2 #395, idea of
		// Generals Online b5770a12b (reimplemented).
		Player *enemy = TheScriptEngine->getSkirmishEnemyPlayer();
		if (enemy) {
			Region2D bounds;
			getPlayerStructureBounds(&bounds, enemy->getPlayerIndex());
			Coord3D offset;
			offset.zero();
			offset.x = location.x - (bounds.lo.x+bounds.hi.x)*0.5f;
			offset.y = location.y - (bounds.lo.y+bounds.hi.y)*0.5f;
			offset.normalize();
			Real radius = warehouse->getGeometryInfo().getBoundingCircleRadius()*0.8f;

			location.x -= offset.x*radius;
			location.y -= offset.y*radius;
		}
		theGroup->groupGuardPosition( &location, GUARDMODE_NORMAL, CMD_FROM_SCRIPT );

	}
}

//-------------------------------------------------------------------------------------------------
/** Is a supply source attacked? */
//-------------------------------------------------------------------------------------------------
Bool AIPlayer::isSupplySourceAttacked()
{
	// TheSuperHackers @bugfix WebbontheWeb 27/09/2026 No longer scans for supply source attacks for just the last 10 frames.
	// A prior EA comment indicated that the intent was to look for 10 seconds into the attack history.
	// GeneralsX: the re-scan period stays 10 RETAIL frames (GX_FRAMES30); the window is 10 s at any
	// tick rate. Its only callers are the TEAM_GUARD_SUPPLY_CENTER action and the SUPPLY_SOURCE_ATTACKED condition, which no
	// shipped skirmish or multiplayer script uses.
	const Int RefreshRate = GX_FRAMES30(10);
#if RETAIL_COMPATIBLE_CRC
	const Int ScanWindow = GX_FRAMES30(10);
#else
	const Int ScanWindow = 10 * LOGICFRAMES_PER_SECOND;
#endif
	UnsignedInt curFrame = TheGameLogic->getFrame();
	if (curFrame==0) {
		m_supplySourceAttackCheckFrame = curFrame + RefreshRate;
		return false; // can't be attacked on first frame.
	}
	m_attackedSupplyCenter = INVALID_ID;
	if (curFrame < m_supplySourceAttackCheckFrame) {
		return false;
	}
	if (m_player->getAttackedFrame() + ScanWindow < curFrame) {
		return false; // haven't been attacked recently.
	}
	m_supplySourceAttackCheckFrame = curFrame + RefreshRate;

	// Scan my units.
	Player::PlayerTeamList::const_iterator it;
	for (it = m_player->getPlayerTeams()->begin(); it != m_player->getPlayerTeams()->end(); ++it) {
		for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance()) {
			Team *team = iter.cur();
			if (!team) {
				continue;
			}
			for (DLINK_ITERATOR<Object> objIter = team->iterate_TeamMemberList(); !objIter.done(); objIter.advance()) {
				Object *obj = objIter.cur();
				if (!obj) {
					continue;
				}
				if (!obj->isKindOf(KINDOF_CASH_GENERATOR) && !obj->isKindOf(KINDOF_DOZER) &&
					!obj->isKindOf(KINDOF_HARVESTER)) {
					continue;
				}
#if !RETAIL_COMPATIBLE_CRC
				// GeneralsX: With a 10-s window a dozer or harvester killed seconds ago - still allocated
				// while it dies - would win the scan, and TEAM_GUARD_SUPPLY_CENTER would send its team to the wreck instead of a
				// live target or the fallback supply center.
				if (obj->isEffectivelyDead()) {
					continue;
				}
#endif
				// check for attacked.
				BodyModuleInterface *body = obj->getBodyModule();
				if (body) {
					const DamageInfo *info = body->getLastDamageInfo();
					if (info) {
						if (info->out.m_noEffect) {
							continue;
						}
						const UnsignedInt *lastDamageTimestamp = body->getLastDamageTimestamp();
						if (lastDamageTimestamp && *lastDamageTimestamp + ScanWindow > curFrame) {
							// winner.
							m_attackedSupplyCenter = obj->getID();
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
/** Is the nearest supply source safe? */
//-------------------------------------------------------------------------------------------------
Bool AIPlayer::isSupplySourceSafe( Int minSupplies )
{
	// GeneralsX: a nearest source that is contested no longer hides the safe ones behind it. Only when every candidate is unsafe
	// do we report unsafe (the nearest one), so the expansion scripts keep waiting exactly as before.
	if (findSupplyCenter(minSupplies, true) != nullptr) return true;
	Object *warehouse = findSupplyCenter(minSupplies);
	if (warehouse==nullptr) return true; // it's safe cause it doesn't exist.
	return (isLocationSafe(warehouse->getPosition(), warehouse->getTemplate()));
}

// A scan marker is not an enemy presence. The USA spy-satellite ping (NO_COLLIDE IMMOBILE UNATTACKABLE INERT, gone after 13 s), the
// radar-van ping, vision objects and spy drones are all INERT. Counting them marked an AI base's home dock and nearby build spots
// unsafe while the ping lived, and the supply expansion (which skips unsafe docks) then built its stash at a far dock for good.
// Two INERT kinds still count: poison and radiation fields (CLEANUP_HAZARD), and the special-power view object
// (GameData SpecialPowerViewObject = SuperweaponPing), which an enemy's nuke, Scud Storm, artillery barrage, carpet bomb,
// cluster mines... leaves at its target before the strike lands.
class PartitionFilterRejectHarmlessMarkers : public PartitionFilter
{
public:
	virtual Bool allow( Object *other ) override
	{
		if( !other->isKindOf( KINDOF_INERT ) || other->isKindOf( KINDOF_CLEANUP_HAZARD ) )
			return TRUE;
		// The strike marker by template, not by name: a map may override or reskin it, and ThingFactory::newObject builds one of its
		// BuildVariations when it has any. Reached only for an INERT non-hazard object near the spot.
		const ThingTemplate *view = TheThingFactory->findTemplate( TheGlobalData->m_specialPowerViewObjectName, FALSE );
		if( view == nullptr )
			return FALSE;
		const ThingTemplate *mine = other->getTemplate();
		if( mine->isEquivalentTo( view ) )
			return TRUE;
		const std::vector<AsciiString>& variations = view->getBuildVariations();
		for( std::vector<AsciiString>::const_iterator it = variations.begin(); it != variations.end(); ++it )
		{
			const ThingTemplate *variation = TheThingFactory->findTemplate( *it, FALSE );
			if( variation != nullptr && mine->isEquivalentTo( variation ) )
				return TRUE;
		}
		return FALSE;
	}
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRejectHarmlessMarkers"; }
#endif
};

//-------------------------------------------------------------------------------------------------
/** Is this location safe for building this thing? */
//-------------------------------------------------------------------------------------------------
Bool AIPlayer::isLocationSafe(const Coord3D *pos, const ThingTemplate *tthing )
{
	if (tthing == nullptr) return 0;

	// See if we have enemies.
	Real radius = TheAI->getAiData()->m_supplyCenterSafeRadius;
	radius += tthing->getTemplateGeometryInfo().getBoundingCircleRadius();

	// only consider enemies.
	PartitionFilterPlayerAffiliation	filterTeam(m_player, (ALLOW_ALLIES|ALLOW_NEUTRAL), false);

	// and only stuff that is not dead
	PartitionFilterAlive filterAlive;

	// and only stuff that isn't stealthed (and not detected)
	// (note that stealthed allies aren't hidden from us, but we're only looking for enemies here)
	PartitionFilterRejectByObjectStatus filterStealth( MAKE_OBJECT_STATUS_MASK( OBJECT_STATUS_STEALTHED ),
																										 MAKE_OBJECT_STATUS_MASK2( OBJECT_STATUS_DETECTED, OBJECT_STATUS_DISGUISED ) );

	// (optional) only stuff that is significant
	PartitionFilterInsignificantBuildings filterInsignificant(true, false);

	PartitionFilterRejectByKindOf	filterHarvesters(MAKE_KINDOF_MASK(KINDOF_HARVESTER), KINDOFMASK_NONE);

	PartitionFilterRejectByKindOf	filterDozer(MAKE_KINDOF_MASK(KINDOF_DOZER), KINDOFMASK_NONE);

	PartitionFilterRejectHarmlessMarkers	filterMarkers;	// scan pings and drones are not a threat (see the class)

	PartitionFilter *filters[16];
	Int numFilters = 0;

	filters[numFilters++] = &filterTeam;
	filters[numFilters++] = &filterAlive;
	filters[numFilters++] = &filterStealth;

	filters[numFilters++] = &filterInsignificant;
	filters[numFilters++] = &filterHarvesters;
	filters[numFilters++] = &filterDozer;
	filters[numFilters++] = &filterMarkers;
	filters[numFilters] = nullptr;

	Object *enemy = ThePartitionManager->getClosestObject(  pos, radius, FROM_BOUNDINGSPHERE_2D, filters );
	if (enemy!=nullptr) {
		return false;
	}
	return true;

}


// ------------------------------------------------------------------------------------------------
/** Invoked when a unit I am training comes into existence */
// ------------------------------------------------------------------------------------------------
void AIPlayer::onUnitProduced( Object *factory, Object *unit )
{
	Bool found = false;
	// TheSuperHackers @fix Mauller 26/04/2025 Fixes uninitialized variable.
	// To keep retail compatibility it needs to be set true in VS6 builds.
#if defined(_MSC_VER) && _MSC_VER < 1300
	Bool supplyTruck = true;
#else
	Bool supplyTruck = false;
#endif

	// factory could be null at the start of the game.
	if (factory == nullptr) {
		return;
	}

	for (DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		// find work order entry and delete it
		WorkOrder *order;
		if (found) break;
		for( order = team->m_workOrders; order; order = order->m_next )
		{
			if (order->m_factoryID == factory->getID() && order->m_numCompleted < order->m_numRequired && unit->getTemplate()->isEquivalentTo(order->m_thing))
			{
				// found associated order, mark it complete.
				order->m_numCompleted++;
				// put new unit into the team under construction
				if (team->m_team)
					unit->setTeam( team->m_team );
				if (team->m_reinforcement) {
					team->m_reinforcementID = unit->getID();
				}
				AIUpdateInterface *ai = unit->getAIUpdateInterface();
				// GeneralsX: The null check two lines up did not cover this dereference (tsh batch2 #417).
				if (team->m_team && team->m_team->getPrototype()->getTemplateInfo()->m_hasHomeLocation) {
					if (ai) {
						std::vector<Coord3D> path;
						path.push_back( *ai->getGoalPosition() );
						path.push_back(team->m_team->getPrototype()->getTemplateInfo()->m_homeLocation);
						ai->aiFollowExitProductionPath(&path, nullptr, CMD_FROM_AI);
					}
				}

				order->m_factoryID = INVALID_ID; // no longer using this factory.
				if (ai) {
					// tell it to start gathering resources.
					// Here is the special bit for this exit style, force wanting on SupplyTruck types
					SupplyTruckAIInterface* supplyTruckAI = ai->getSupplyTruckAIInterface();
					if( supplyTruckAI )	{
						if (order->m_isResourceGatherer) {
							supplyTruck = true;
						} else {
							supplyTruck = false;
						}
						supplyTruckAI->setForceWantingState(supplyTruck);
						if (supplyTruck) {
							// assign to a supply depot.
							for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
							{
								if (info->isSupplyBuilding() && info->getDesiredGatherers()>0 &&
									info->getDesiredGatherers()>info->getCurrentGatherers()) {
										Object *obj = TheGameLogic->findObjectByID(info->getObjectID());
										if (obj) {
											info->setCurrentGatherers(info->getCurrentGatherers()+1);
											// Note - although this is the ai, we are sending in CMD_FROM_PLAYER.
											// This causes the dock object to stick in the docking interface.
											// The supply truck ai issues dock commands, and they become confused.
											// Thus, player.  jba.  ;(
											ai->aiDock(obj, CMD_FROM_PLAYER);
										}
									}
							}

						}
					}
				}
				found = true;
				break;
			}
		}
	}
	if (!supplyTruck && unit->isKindOf(KINDOF_DOZER)) {
		if (m_dozerQueuedForRepair) {
			m_repairDozer = unit->getID();
			m_dozerQueuedForRepair =false;
		} else {
			m_buildDelay = 0;
			m_structureTimer = 1;
		}
	}
	if (!found) {
		DEBUG_LOG(("***AI PLAYER-Unit not found in production queue."));
	}

	m_teamDelay = 0; // Cause the update queues & selection to happen immediately.
}

// Weights an enemy object's cost, in percent: income (supply centers/stashes, drop zones, black markets,
// internet centers, hackers) 300; army-wide tech, enemy superweapon hit by a superweapon 250; gatherers,
// power plants 200; rest 100. Strike-damaged up to 3x, disabled 1.5x. Supply centers also match CASH_GENERATOR
// (some stashes carry FS_POWER).
static Int gxStrategicCost( const Object *obj, Int cost, Bool superweaponStrike )
{
	Int pct = 100;
	if( obj->isKindOf( KINDOF_CASH_GENERATOR ) || obj->isKindOf( KINDOF_FS_SUPPLY_CENTER ) || obj->isKindOf( KINDOF_FS_SUPPLY_DROPZONE )
		|| obj->isKindOf( KINDOF_FS_BLACK_MARKET ) || obj->isKindOf( KINDOF_FS_INTERNET_CENTER ) || obj->isKindOf( KINDOF_MONEY_HACKER ) )
		pct = 300;
	else if( obj->isKindOf( KINDOF_FS_STRATEGY_CENTER ) || obj->isKindOf( KINDOF_FS_ADVANCED_TECH )
		|| ( superweaponStrike && obj->isKindOf( KINDOF_FS_SUPERWEAPON ) ) )
		pct = 250;
	else if( obj->isKindOf( KINDOF_HARVESTER ) || obj->isKindOf( KINDOF_FS_POWER ) )
		pct = 200;

	const BodyModuleInterface *body = obj->getBodyModule();
	if( body != nullptr )
	{
		const Int maxHealth = REAL_TO_INT( body->getMaxHealth() );
		const Int health = REAL_TO_INT( body->getHealth() );
		if( maxHealth > 0 && health < maxHealth )
		{
			const Int missing = (Int)( (Int64)( maxHealth - (health > 0 ? health : 0) ) * 100 / maxHealth );
			pct += pct * 2 * missing / 100;
		}
	}
	if( obj->isDisabledByType( DISABLED_EMP ) || obj->isDisabledByType( DISABLED_PARALYZED )
		|| obj->isDisabledByType( DISABLED_HACKED ) || obj->isDisabledByType( DISABLED_SUBDUED ) )
		pct += pct / 2;

	// saturated: a map.ini may set any build cost, and the narrowing of an out-of-range Int64 is the compiler's choice
	const Int64 value = (Int64)cost * pct / 100;
	return value > 0x7FFFFFFF ? 0x7FFFFFFF : ( value < -0x7FFFFFFF ? -0x7FFFFFFF : (Int)value );
}

//----------------------------------------------------------------------------------------------------------
/** GeneralsX: the strategic aim above is every computer player's (all difficulties). A GX_TEST_INSTRUMENTS build reads
	* GX_SWTARGET=<player index bits> instead (e.g. 0x3FFC = players 2-13), so one binary plays the strategic aim against
	* stock in the same match; unset there means the shipping rule. Never in a shipping build. */
AIPlayer::SuperweaponAim AIPlayer::gxSuperweaponAim( const Player *aimer, const SpecialPowerTemplate *power )
{
	if( aimer == nullptr || power == nullptr )
		return AIM_STOCK;
#if defined(GX_TEST_INSTRUMENTS)
	static Bool s_read = FALSE, s_set = FALSE;
	static UnsignedInt64 s_players = 0;	// Player indices up to 39
	if( !s_read )
	{
		s_read = TRUE;
		const char *v = getenv( "GX_SWTARGET" );
		if( v != nullptr && *v != '\0' )
		{
			s_players = (UnsignedInt64)strtoull( v, nullptr, 0 );
			s_set = TRUE;
			fprintf( stderr, "[GXSWTARGET] players=0x%llX\n", (unsigned long long)s_players );
		}
	}
	if( s_set && (s_players & gxPlayerBit(aimer->getPlayerIndex())) == 0 )
		return AIM_STOCK;
#endif
	switch( power->getSpecialPowerType() )
	{
		case SPECIAL_NEUTRON_MISSILE:
		case NUKE_SPECIAL_NEUTRON_MISSILE:
		case SUPW_SPECIAL_NEUTRON_MISSILE:
		case SPECIAL_SCUD_STORM:
		case SPECIAL_PARTICLE_UPLINK_CANNON:
		case SUPW_SPECIAL_PARTICLE_UPLINK_CANNON:
		case LAZR_SPECIAL_PARTICLE_UPLINK_CANNON:
			return AIM_STRATEGIC_SUPERWEAPON;
		default:
			return AIM_STRATEGIC;
	}
}

//----------------------------------------------------------------------------------------------------------
// GeneralsX: the strategic aim weighs each enemy object ONCE per decision. Stock walks every team list and prices every object in
// range again for each of the search's ~220 samples (calcCostToBuild included); this list is that walk done once, so a sample is a
// pass over a flat array. Same objects, same order and the same float expressions as the per-sample walk, so the same answer.
struct GxSuperweaponTarget
{
	Real x, y;
	Real value;		///< weighed (gxStrategicCost) and with stock's command center / superweapon rules applied
	Bool negative;	///< a sneak attack's defender: subtracts five times its value
};

static void gxCollectSuperweaponTargets( Int playerNdx, Bool includeMilitaryUnits, Bool superweaponStrike, std::vector<GxSuperweaponTarget> &out )
{
	out.clear();
	Player* pPlayer = ThePlayerList->getNthPlayer(playerNdx);
	if (pPlayer == nullptr)
		return;
	for (Player::PlayerTeamList::const_iterator it = pPlayer->getPlayerTeams()->begin(); it != pPlayer->getPlayerTeams()->end(); ++it)
	{
		for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance())
		{
			Team *team = iter.cur();
			if (!team) continue;
			for (DLINK_ITERATOR<Object> iter = team->iterate_TeamMemberList(); !iter.done(); iter.advance())
			{
				Object *pObj = iter.cur();
				// the dying are skipped: still on their team at 0 health, the damage bonus would score them 3x
				if (!pObj || pObj->isEffectivelyDead())
					continue;
				Bool applyNegValue = FALSE;
				if( !includeMilitaryUnits )
				{
					if( pObj->isKindOf( KINDOF_FS_BASE_DEFENSE ) || pObj->isKindOf( KINDOF_TECH_BASE_DEFENSE ) )
						applyNegValue = TRUE;	//Hostile structure
					else if( ( pObj->isKindOf( KINDOF_VEHICLE ) || pObj->isKindOf( KINDOF_INFANTRY ) )
						&& !pObj->isKindOf( KINDOF_DOZER ) && !pObj->isKindOf( KINDOF_HARVESTER ) )
						applyNegValue = TRUE;	//Hostile unit.
				}
				else if( pObj->isKindOf( KINDOF_AIRCRAFT ) && pObj->isSignificantlyAboveTerrain() )
					continue; // Don't target flying aircraft.  OK if in the airstrip.
				Int cost = pObj->getTemplate()->calcCostToBuild(pPlayer);
				if( !applyNegValue )
					cost = gxStrategicCost( pObj, cost, superweaponStrike );
				Real value = cost;
				if (pObj->isKindOf(KINDOF_COMMANDCENTER))
					value = includeMilitaryUnits ? value / 10 : value * 5.0f;	// as stock: superweapons cannot destroy one; a sneak attack's prime target
				if (pObj->isKindOf( KINDOF_FS_SUPERWEAPON ) && !superweaponStrike )	// a superweapon strike keeps gxStrategicCost's value
					value = includeMilitaryUnits ? value / 10 : value * 5.0f;
				GxSuperweaponTarget t;
				t.x = pObj->getPosition()->x;
				t.y = pObj->getPosition()->y;
				t.value = value;
				t.negative = applyNegValue;
				out.push_back( t );
			}
		}
	}
}

/// GeneralsX: getPlayerSuperweaponValue's sum over gxCollectSuperweaponTargets' list
static Int gxSuperweaponValueAt( const std::vector<GxSuperweaponTarget> &targets, const Coord3D *center, Real radius )
{
	if (radius < 4*PATHFIND_CELL_SIZE_F)
	{
		radius = 4*PATHFIND_CELL_SIZE_F;
	}
	Real cash = 0;
	Real radSqr = sqr(radius);
	for (size_t i = 0; i < targets.size(); ++i)
	{
		const GxSuperweaponTarget &t = targets[i];
		Real dx = center->x - t.x;
		Real dy = center->y - t.y;
		if (dx*dx+dy*dy<radSqr)
		{
			Real dist = sqrt(dx*dx+dy*dy);
			Real factor = 1.0f - (dist/(2*radius)); // 1.0 in center, 0.5 on edges.
			if( t.negative )
				cash -= factor * t.value * 5.0f; //Extremely undesired
			else
				cash += factor * t.value;
		}
	}
	return REAL_TO_INT(cash);
}

//----------------------------------------------------------------------------------------------------------
/**
 * Find a good spot to fire a superweapon.
 */
Bool AIPlayer::computeSuperweaponTarget(const SpecialPowerTemplate *power, Coord3D *retPos, Int playerNdx, Real weaponRadius)
{

  Bool success = FALSE;

	Region2D bounds;
	getPlayerStructureBounds(&bounds, playerNdx);

	if( bounds.hi.x == 0
		&& bounds.lo.x == 0
		&& bounds.hi.y == 0
		&& bounds.lo.y == 0
		)
	{
		Region3D bounds3D;
		// Degenerate bounds because he has no buildings.  Don't give up, scan the whole map for the leftovers.
		TheTerrainLogic->getExtent(&bounds3D);
		bounds.hi.x = bounds3D.hi.x;
		bounds.lo.x = bounds3D.lo.x;
		bounds.hi.y = bounds3D.hi.y;
		bounds.lo.y = bounds3D.lo.y;
	}

	if (weaponRadius<1.0f) {
		weaponRadius = 1.0f; // sanity to avoid divide by 0.
	}

	Int xCount, yCount;
	bounds.lo.x += weaponRadius;
	bounds.hi.x -= weaponRadius;
	if (bounds.hi.x<bounds.lo.x) {
		bounds.hi.x = bounds.lo.x = (bounds.hi.x+bounds.lo.x)/2.0f;
	}
	if (bounds.hi.y<bounds.lo.y) {
		bounds.hi.y = bounds.lo.y = (bounds.hi.y+bounds.lo.y)/2.0f;
	}

	xCount = REAL_TO_INT_CEIL(bounds.width()/weaponRadius)+1;
	yCount = REAL_TO_INT_CEIL(bounds.height()/weaponRadius)+1;

	if (xCount>10) xCount = 10;
	if (yCount>10) yCount = 10;

	Int cash = -1;
	Coord3D pos;
	Coord3D bestPos;
	Int x, y, xDelta, yDelta, xIndex, yIndex, xStart, yStart;

	Bool targetMilitaryUnits = TRUE;
	if( power->getSpecialPowerType() == SPECIAL_SNEAK_ATTACK )
	{
		//Military units will have a negative effect on where to drop the special power.
		//We want to target rich areas that are poorly defended or undefended.
		targetMilitaryUnits = FALSE;
	}
	const SuperweaponAim aim = gxSuperweaponAim( m_player, power );
	const Bool strategic = aim != AIM_STOCK;
	static std::vector<GxSuperweaponTarget> s_targets;	// GeneralsX: the strategic aim's weighed enemy objects, rebuilt per decision
	if( strategic )
		gxCollectSuperweaponTargets( playerNdx, targetMilitaryUnits, aim == AIM_STRATEGIC_SUPERWEAPON, s_targets );

	//Randomize which way we iterate the grid. We don't always want to start in the bottom left corner incase
	//of a bad calculation, it'll would always end up there.
	switch( GameLogicRandomValue( 1, 4 ) )
	{
		case 1:
			//x min to max, y min to max
			xDelta = 1,	yDelta = 1;
			xStart = 0,	yStart = 0;
			break;
		case 2:
			//x max to min, y min to max
			xDelta = -1, yDelta = 1;
			xStart = xCount, yStart = 0;
			break;
		case 3:
			//x min to max, y max to min
			xDelta = 1, yDelta = -1;
			xStart = 0, yStart = yCount;
			break;
		case 4:
		default:
			//x max to min, y max to min
			xDelta = -1, yDelta = -1;
			xStart = xCount, yStart = yCount;
			break;
	}

	//Calculate the generally best position
	xIndex = xStart;
	for( x = 0; x < xCount; x++, xIndex += xDelta )
	{
		yIndex = yStart;
		for( y = 0; y < yCount; y++, yIndex += yDelta )
		{
			pos.x = bounds.lo.x + ( bounds.width() * xIndex ) / xCount;
			pos.y = bounds.lo.y + ( bounds.height() * yIndex ) / yCount;
			pos.z = 0;
			Int curCash = strategic ? gxSuperweaponValueAt( s_targets, &pos, 2*weaponRadius ) : getPlayerSuperweaponValue( &pos, playerNdx, 2*weaponRadius, targetMilitaryUnits );
			if ( curCash > cash)
			{
				cash = curCash;
				bestPos = pos;
				success = true;
			}
		}
	}

	if (!success)
		return false;

	//Fine tune that position by looking at a even smaller radius.
	Coord3D veryBestPos;
	xCount = 11;
	yCount = 11;
	cash = -1;
	Int count = 0;
	for( x = 0; x < xCount; x++ )
	{
		for( y = 0; y < yCount; y++ )
		{
			pos.x = bestPos.x + (x-5)*(weaponRadius/10);
			// GeneralsX: stock steps y with x, so its 121 samples are the 11 points of one diagonal,
			// each scored 11 times; the strategic aim searches the square it was meant to (same 121 samples).
			pos.y = bestPos.y + ((strategic ? y : x)-5)*(weaponRadius/10);
			pos.z = 0;
			Int curCash = strategic ? gxSuperweaponValueAt( s_targets, &pos, weaponRadius ) : getPlayerSuperweaponValue( &pos, playerNdx, weaponRadius, targetMilitaryUnits );
			// GeneralsX: The first sample stands while nothing has beaten it: a first -1 used to add onto
			// an uninitialised veryBestPos, and when no sample beat -1 its garbage reached getGroundHeight.
			// The position is unchanged whenever a sample is >= 0, and success (cash > -1) is unchanged for every input.
			if ( curCash > cash || count == 0 )
			{
				cash = curCash;
				veryBestPos = pos;
				count = 1;
			}
			else if (curCash==cash)
			{
				veryBestPos.x += pos.x;
				veryBestPos.y += pos.y;
				count++;
			}
		}
	}
	if (count>1) {
		veryBestPos.x /= count;
		veryBestPos.y /= count;
	}
	veryBestPos.z = TheTerrainLogic->getGroundHeight(veryBestPos.x, veryBestPos.y);
	*retPos = veryBestPos;

  success = ( cash > -1 );


  return success;


}

//----------------------------------------------------------------------------------------------------------
/**
 * Get the target value for structures in an area.
 */
Int AIPlayer::getPlayerSuperweaponValue(Coord3D *center, Int playerNdx, Real radius, Bool includeMilitaryUnits, SuperweaponAim aim )
{
	if( aim != AIM_STOCK )	// GeneralsX: the strategic aim, one sample (the search builds the list once and reuses it)
	{
		std::vector<GxSuperweaponTarget> targets;
		gxCollectSuperweaponTargets( playerNdx, includeMilitaryUnits, aim == AIM_STRATEGIC_SUPERWEAPON, targets );
		return gxSuperweaponValueAt( targets, center, radius );
	}
	if (radius < 4*PATHFIND_CELL_SIZE_F)
	{
		radius = 4*PATHFIND_CELL_SIZE_F;
	}
	Player::PlayerTeamList::const_iterator it;
	Real cash = 0;
	Real radSqr = sqr(radius);

	Player* pPlayer = ThePlayerList->getNthPlayer(playerNdx);
	if (pPlayer == nullptr)
		return 0;
	for (it = pPlayer->getPlayerTeams()->begin(); it != pPlayer->getPlayerTeams()->end(); ++it)
	{
		for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance())
		{
			Team *team = iter.cur();
			if (!team) continue;
			for (DLINK_ITERATOR<Object> iter = team->iterate_TeamMemberList(); !iter.done(); iter.advance())
			{
				Object *pObj = iter.cur();
				if (!pObj)
					continue;

				Bool applyNegValue = FALSE;
				if( !includeMilitaryUnits )
				{
					if( pObj->isKindOf( KINDOF_FS_BASE_DEFENSE ) || pObj->isKindOf( KINDOF_TECH_BASE_DEFENSE ) )
					{
						//Hostile structure
						applyNegValue = TRUE;
					}
					else if( pObj->isKindOf( KINDOF_VEHICLE ) || pObj->isKindOf( KINDOF_INFANTRY ) )
					{
						if( !pObj->isKindOf( KINDOF_DOZER ) && !pObj->isKindOf( KINDOF_HARVESTER ) )
						{
							//Hostile unit.
							applyNegValue = TRUE;
						}
					}
				}
				else if (pObj->isKindOf(KINDOF_AIRCRAFT))
				{
					if (pObj->isSignificantlyAboveTerrain())
					{
						continue; // Don't target flying aircraft.  OK if in the airstrip.
					}
				}
				Coord3D pos = *pObj->getPosition();
				Real dx = center->x - pos.x;
				Real dy = center->y - pos.y;
				if (dx*dx+dy*dy<radSqr)
				{
					Real dist = sqrt(dx*dx+dy*dy);
					Real factor = 1.0f - (dist/(2*radius)); // 1.0 in center, 0.5 on edges.
					Real value = pObj->getTemplate()->calcCostToBuild(pPlayer);
					if (pObj->isKindOf(KINDOF_COMMANDCENTER))
					{
						if( !includeMilitaryUnits )
							value = value * 5.0f; //Command centers are prime targets for sneak attacks.
						else
							value = value / 10; // Command centers cannot be killed by any superweapon, so we don't want to target them as highly. jba.
					}
					if (pObj->isKindOf( KINDOF_FS_SUPERWEAPON ) )
					{
						if( !includeMilitaryUnits )
							value = value * 5.0f; //Superweapons are prime targets for sneak attacks.
						else
							value = value / 10; // Superweapons cannot be killed by any superweapon, so we don't want to target them as highly. jba.
					}
					if( applyNegValue )
					{
						cash -= factor * value * 5.0f; //Extremely undesired
					}
					else
					{
						cash += factor * value;
					}
				}
			}
		}
	}
	// GeneralsX: Saturated (REAL_TO_INT): a map's cost can leave Int range, and the plain
	// conversion of the float sum differs by CPU.
	return REAL_TO_INT(cash);
}
// ------------------------------------------------------------------------------------------------
/** Search the computer player's buildings for one that can build the given request
	* and start training the unit.
	* If busyOK is true, it will queue a unit even if one is building.  This lets
	* script invoked teams "push" to the front of the queue. */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::startTraining( WorkOrder *order, Bool busyOK, AsciiString teamName)
{
	Object *factory = findFactory(order->m_thing, busyOK);
	if( factory )
	{
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu && pu->queueCreateUnit( order->m_thing, pu->requestUniqueUnitID() )) {
			order->m_factoryID = factory->getID();
			if (TheGlobalData->m_debugAI) {
				AsciiString teamStr = "Queuing ";
				teamStr.concat(order->m_thing->getName());
				teamStr.concat(" for ");
				teamStr.concat(teamName);
				TheScriptEngine->AppendDebugMessage(teamStr, false);
			}
			return true;
		}
	}

	return FALSE;

}

// ------------------------------------------------------------------------------------------------
// GeneralsX: A Hard skirmish AI sitting on a large bank spends it.
// Above AI_HOARD_CASH:
// a team's units are built in parallel (buildSpecificAITeam), teams and structures are picked faster and attack teams may field extra copies (AISkirmishPlayer.cpp, ported from Zero
// Hour Reforged's cash hoard). Only in a match that STARTED richer than the default cash: Hard's scripted gifts alone lift
// a bank to ~$50k in a $10,000 start, so no bank threshold separates the
// two - the starting cash, a lobby option every peer shares, does. A default-cash match takes none of these branches.
// Trade-off: one fixed threshold for Hard only; a per-difficulty AIData field when Normal/Easy want it.
static const Int AI_HOARD_CASH = 20000;

Int AIPlayer::hoardThreshold()
{
	if (!isSkirmishAI() || m_player->getPlayerDifficulty() != DIFFICULTY_HARD || TheGameInfo == nullptr)
		return 0;
	if (TheGameInfo->getStartingCash().countMoney() <= TheGlobalData->m_defaultStartingCash.countMoney())
		return 0;
	return AI_HOARD_CASH;
}

// The largest batch any of our factories would build this unit in: a split order may land on any of them.
Int AIPlayer::unitsPerOrder(const ThingTemplate *thing)
{
	Int most = 1;
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		Object *factory = TheGameLogic->findObjectByID( info->getObjectID() );
		if (factory == nullptr || factory->getControllingPlayer() != m_player)
			continue;
		ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
		if (pu && TheBuildAssistant->isPossibleToMakeUnit( factory, thing )) {
			const Int n = pu->getUnitsPerOrder(thing);
			if (n > most) most = n;
		}
	}
	return most;
}

Bool AIPlayer::isHoarding()
{
	const Int threshold = hoardThreshold();
	return threshold > 0 && (Int)m_player->getMoney()->countMoney() > threshold;
}

// ------------------------------------------------------------------------------------------------
/** Search the computer player's buildings for one that can build the given request.
	* If busyOK is true, it will return a busy factory if there are no idle ones.  This is
	* used for script invoked teams "push" to the front of the queue. */
// ------------------------------------------------------------------------------------------------
Object *AIPlayer::findFactory(const ThingTemplate *thing, Bool busyOK)
{
	Object *busyFactory = nullptr; // We prefer a factory that isn't busy.
	for( BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		Object *factory = TheGameLogic->findObjectByID( info->getObjectID() );
		if( factory )
		{
			if (factory->getControllingPlayer() != m_player) {
				info->setObjectID(INVALID_ID);
				continue;
			}
			// ignore buildings that are under construction.
			if (factory->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
				continue;
			// also ignore buildings that are being sold.
			if (factory->testStatus(OBJECT_STATUS_SOLD))
				continue;
			ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
			// If it doesn't produce, continue.
			if (!pu) continue;
			// if we can't create the unit do nothing
			if( TheBuildAssistant->isPossibleToMakeUnit( factory, thing ) == FALSE )
				continue;
			// If the factory is not busy, return it.
			Bool busy = pu->getProductionCount()>0;
			if (!busy) return factory; // found a not busy factory.
			if (busyOK) busyFactory = factory;
		}

	}
	// We didn't find an idle factory, so return the busy one.
	if (busyOK) return busyFactory;
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
/** Return true if team can be considered for building */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::isPossibleToBuildTeam( TeamPrototype *proto, Bool requireIdleFactory, Bool &notEnoughMoney)
{
	/* Make sure we have at least one idle factory, and factories for all unit types. */
	Bool anyIdle = false;
	Int cost=0;
	notEnoughMoney = false;
	for( int i=0; i<proto->getTemplateInfo()->m_numUnitsInfo; i++ )
	{
		const TCreateUnitsInfo *unitInfo = &proto->getTemplateInfo()->m_unitsInfo[0];
		const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
		if (thing) {
			Int thingCost = thing->calcCostToBuild(m_player);
			// GeneralsX: was findFactory(thing,true) then findFactory(thing,false): the first
			// returns the first idle factory if any, else the last busy one, so what it returned answers both questions.
			Object *factory = findFactory(thing, true);
			if (nullptr == factory) {
				// Couldn't find a factory.
				return false;
			}
			if (!(factory->getProductionUpdateInterface()->getProductionCount()>0)) {
				// Found an idle factory.
				anyIdle = true;
			}
			// GeneralsX: Saturated (REAL_TO_INT): a map's cost can leave Int range, and the plain
			// conversion of the float sum differs by CPU.
			cost = REAL_TO_INT(cost + thingCost * ((unitInfo[i].maxUnits+unitInfo[i].minUnits)/2.0f));
		}
	}
	cost = REAL_TO_INT(cost * TheAI->getAiData()->m_teamResourcesToBuild);
	if (m_player->getMoney()->countMoney() < cost)	{
		notEnoughMoney = true;
		return false; // too expensive
	}
	if (anyIdle)
	{
		return true;
	}
	if (!requireIdleFactory)
	{
		// Doesn't require an idle factory, so we're ok.
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: ai-pro-rules-2 T2 (3d "lifetime"):
// the air-watch team (AI_PLAYER_TUNNEL_AIR_WATCH's team) is BUILT only while some hostile player owns an airfield and REINFORCED only
// while C holds (that airfield term, or a hostile aircraft seen in the last 180 s); C is evaluated by AISkirmishPlayer's watch and
// saved. Every other team, and every AI without the watch: TRUE (stock).
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::gxAirTeamAllowed(const TeamPrototype *proto, Bool reinforce) const
{
	if (m_gxAirWatchRadius == 0 || m_gxAirWatchTeam.isEmpty() || proto == nullptr)
		return TRUE;
	if (TheTeamFactory->findTeamPrototype(m_gxAirWatchTeam) != proto)
		return TRUE;
	return reinforce ? m_gxAirC : m_gxAirCAirfield;
}

// GeneralsX: findTeamPrototype's NAMEKEY is the one thing gxAirWatchTeamMemberSkips can change. Once the
// watch's team name has a key (a team of that name was loaded, or an earlier lookup made it) that is a read, and with no name there is no lookup.
Bool AIPlayer::gxAirWatchTeamKeyKnown() const
{
	return m_gxAirWatchTeam.isEmpty() || TheNameKeyGenerator->gxIsNameKnown(m_gxAirWatchTeam);
}

Bool AIPlayer::gxAirWatchTeamMemberSkips(const Object *unit, const Object *target) const
{
	if (m_gxAirWatchRadius <= 0 || unit == nullptr || target == nullptr || !target->isKindOf(KINDOF_AIRCRAFT))
		return FALSE;
	// a unit of a watch row (stock guard teams included), or a member of the AA team
	for (Int i = 0; i < m_gxAirGuardCount; ++i)
		if (m_gxAirGuards[i].unit == unit->getID())
			return TRUE;
	Team *t = const_cast<Team *>(unit->getTeam());	// Team::getPrototype() is not const
	return m_gxAirWatchTeam.isNotEmpty() && t != nullptr && t->getPrototype() == TheTeamFactory->findTeamPrototype(m_gxAirWatchTeam);
}

// With the watch acting, a unit of a watch row may attack an aircraft only if it is THAT row's plane (and the row is not
// returning); a tunnel guard or a member of the AA team may attack none other. The shared attack state asks this, so no path
// (retaliation, hunt, attack area, guard, attack squad, any mood scan, a continue-range retarget) gives a watched unit an
// aircraft it was not assigned. Every other unit, and every player whose watch is off (T0), is stock.
Bool AIPlayer::gxAirAttackAuthorized(const Object *unit, const Object *target) const
{
	if (m_gxAirWatchRadius <= 0 || unit == nullptr || target == nullptr || !target->isKindOf(KINDOF_AIRCRAFT))
		return TRUE;
	for (Int i = 0; i < m_gxAirGuardCount; ++i)
		if (m_gxAirGuards[i].unit == unit->getID())
			return m_gxAirGuards[i].plane == target->getID() && m_gxAirGuards[i].state != GX_AIR_RETURNING;
	if (unit->getAI() != nullptr && unit->getAI()->getAIStateType() == AI_GUARD_TUNNEL_NETWORK)
		return FALSE;	// a tunnel guard: aircraft only by assignment (its own nested attack states included)
	return !gxAirWatchTeamMemberSkips(unit, target);	// an AA team member: none
}

// A unit RECRUITED into a team keeps whatever it was doing, and the recruit's own aiMoveToPosition (CMD_FROM_AI, over a non-idle
// unit) runs as a 20-s TEMPORARY state that freezes the state underneath (AIUpdateInterface::privateMoveToPosition). A unit
// recruited into the air-watch team while it attacks an aircraft would keep that aircraft as its AI victim, with the T2 refusal
// in AIAttackState::update frozen with it (ev=auto_air until the move ended). Such a recruit is idled before its move: the attack
// state's exit clears the victim and the turret targets. Any other recruit, any other team, and every player whose watch is not acting: untouched.
void AIPlayer::gxAirRecruitEndsAirAttack(Object *unit) const
{
	AIUpdateInterface *ai = unit ? unit->getAIUpdateInterface() : nullptr;
	if (ai == nullptr || m_gxAirWatchRadius <= 0)
		return;
	const Object *v = ai->getCurrentVictim();
	if (v == nullptr || !v->isKindOf(KINDOF_AIRCRAFT) || gxAirAttackAuthorized(unit, v))
		return;	// (authorized = not watched by the new team, or a row's own plane)
#if defined(GX_TEST_INSTRUMENTS)
	if (gxTaaDiagOn())	// t2-cheap: T2's diagnostics (AIPlayer.h)
	{
		fprintf(stderr, "[GXTAA] f=%u p=%d ev=air_drop unit=%u plane=%u why=recruit\n", (unsigned)TheGameLogic->getFrame(), (Int)m_player->getPlayerIndex(),
			(unsigned)unit->getID(), (unsigned)v->getID());
		fflush(stderr);
	}
#endif
	ai->aiIdle(CMD_FROM_AI);
}

Bool AIPlayer::gxAirReturning(ObjectID unit) const
{
	for (Int i = 0; i < m_gxAirGuardCount; ++i)
		if (m_gxAirGuards[i].unit == unit)
			return m_gxAirGuards[i].state == GX_AIR_RETURNING;
	return FALSE;
}

void AIPlayer::gxAirLogTeam(const TeamPrototype *proto, const char *state)
{
#if defined(GX_TEST_INSTRUMENTS)
	if (!gxTaaDiagOn() || m_gxAirWatchRadius == 0 || m_gxAirWatchTeam.isEmpty() || proto == nullptr	// t2-cheap: gxTaaDiagOn (AIPlayer.h)
			|| TheTeamFactory->findTeamPrototype(m_gxAirWatchTeam) != proto)
		return;
	const UnsignedInt now = TheGameLogic->getFrame();
	fprintf(stderr, "[GXTAA] f=%u p=%d ev=team state=%s c=%d c_airfield=%d c_seen_f=%d c_seen_s=%d\n", (unsigned)now,
		(Int)m_player->getPlayerIndex(), state, (Int)m_gxAirC, (Int)m_gxAirCAirfield, m_gxAirSeenFrame ? (Int)(now - m_gxAirSeenFrame) : -1,
		m_gxAirSeenFrame ? (Int)((now - m_gxAirSeenFrame + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND) : -1);
	fflush(stderr);
#else
	(void)proto; (void)state;
#endif
}

// ------------------------------------------------------------------------------------------------
/** Check if this team is buildable, doesn't exceed maximum limits, meets conditions,
	* and isn't under construction. */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!gxAirTeamAllowed(proto, FALSE))	// the AA team only while a hostile airfield stands
		return false;
	// Check condition.
	if (!proto->evaluateProductionCondition()) {
		return false;
	}
	// check build limit
	if (proto->countTeamInstances() >= proto->getTemplateInfo()->m_maxInstances){
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			str.format("Team %s not chosen - %d already exist.", proto->getName().str(), proto->countTeamInstances());
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;	// Max already built.
	}

	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		if (team->m_team->getPrototype() == proto) {
			return false; // currently building one of these.
		}
	}
	Bool needMoney;
	if (!isPossibleToBuildTeam( proto, true, needMoney)) {
		if (TheGlobalData->m_debugAI) {
			AsciiString str;
			if (needMoney) {
				str.format("Team %s not chosen - Not enough money.", proto->getName().str());
			} else {
				str.format("Team %s not chosen - Factory/tech missing or busy.", proto->getName().str());
			}
			TheScriptEngine->AppendDebugMessage(str, false);
		}
		return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
/** See if any existing teams need reinforcements, and have higher priority. */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::selectTeamToReinforce( Int minPriority )
{
	// Find a high production priority team that needs reinforcements.
	Player::PlayerTeamList::const_iterator t;
	Team *curTeam = nullptr;
	Int curPriority = minPriority; // Don't reinforce a team unless it is above min priority.
	const ThingTemplate *curThing = nullptr;
	for (t = m_player->getPlayerTeams()->begin(); t != m_player->getPlayerTeams()->end(); ++t)
	{
		TeamPrototype *proto = (*t);
		Bool busy = false;
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
		{
			TeamInQueue *team = iter.cur();
			if (team->m_team->getPrototype() == proto) {
				busy = true; // currently building one of these.
			}
		}
		if (busy) continue;
		if (!gxAirTeamAllowed(proto, TRUE)) continue;	// the AA team only while C holds
		if (proto->getTemplateInfo()->m_automaticallyReinforce && proto->getTemplateInfo()->m_productionPriority>curPriority) {
			// Check the team instances.
			for (DLINK_ITERATOR<Team> iter = proto->iterate_TeamInstanceList(); !iter.done(); iter.advance())
			{
				Team *team = iter.cur();
				if (team->hasAnyUnits() == false)
				{
					continue; // empty.
				}
				const TCreateUnitsInfo *unitInfo = &team->getPrototype()->getTemplateInfo()->m_unitsInfo[0];
				for( int i=0; i<team->getPrototype()->getTemplateInfo()->m_numUnitsInfo; i++ )
				{
					if (unitInfo[i].maxUnits < 1) continue;
					const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
					if (thing==nullptr) continue;
					Int count=0;
					team->countObjectsByThingTemplate(1, &thing, false, &count);
					if (count < unitInfo[i].maxUnits)
					{
						// See if there is a factory available.
						if (nullptr != findFactory(thing, false))
						{
							curTeam = team;
							curPriority = proto->getTemplateInfo()->m_productionPriority;
							curThing = thing;
						}
					}
				}
			}
		}
	}
	if (curTeam && curThing)
	{
		gxAirLogTeam(curTeam->getPrototype(), "reinforce");	// test-instruments log
		/* We have something to build. */
		TeamInQueue *teamQ = newInstance(TeamInQueue);
		// Put in front of queue.
		prependTo_TeamBuildQueue(teamQ);
		teamQ->m_priorityBuild = false;
		teamQ->m_reinforcement = true;

		WorkOrder *order = newInstance(WorkOrder);
		order->m_thing = curThing;
		order->m_factoryID = INVALID_ID;
		order->m_numRequired = 1;
		order->m_required = true;
		// prepend to head of list
		order->m_next = nullptr;
		teamQ->m_workOrders = order;
		teamQ->m_frameStarted = TheGameLogic->getFrame();
		teamQ->m_team = curTeam;

		AsciiString teamName = curTeam->getPrototype()->getName();
		teamName.concat(" - AutoReinforcing one ");
		teamName.concat(curThing->getName());
		TheScriptEngine->AppendDebugMessage(teamName, false);

		// start the creation of a new unit
		Coord3D origin;
		origin = curTeam->getPrototype()->getTemplateInfo()->m_homeLocation;
		if (curTeam->getFirstItemIn_TeamMemberList())
		{
			origin = *curTeam->getFirstItemIn_TeamMemberList()->getPosition();
		}
		Object *unit = curTeam->tryToRecruit(curThing, &origin, TheAI->getAiData()->m_maxRecruitDistance);
		if (unit)
		{
			order->m_numCompleted = 1;

			AsciiString teamStr = "Team '";
			teamStr.concat(curTeam->getPrototype()->getName());
			teamStr.concat("' recruits ");
			teamStr.concat(curThing->getName());
			teamStr.concat(" from team '");
			teamStr.concat(unit->getTeam()->getPrototype()->getName());
			teamStr.concat("'");
			TheScriptEngine->AppendDebugMessage(teamStr, false);

			unit->setTeam(curTeam);

			teamQ->m_reinforcementID = unit->getID();

			AIUpdateInterface *ai = unit->getAIUpdateInterface();
			if (ai)
			{
				ai->aiIdle(CMD_FROM_AI);
			}
		} else {
			startTraining( order, teamQ->m_priorityBuild, teamQ->m_team->getName());
		}
		m_teamDelay = 0;
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
/** Determine the next team to build.  Return true if one was selected. */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::selectTeamToBuild()
{

	// find the highest priority of all teams
	Player::PlayerTeamList::const_iterator t;
	const Int invalidPri = -99999;
	Int hiPri = invalidPri;
	// collect all teams that are possible to build, and are at the highest priority
	Player::PlayerTeamList candidateList1;
	for (t = m_player->getPlayerTeams()->begin(); t != m_player->getPlayerTeams()->end(); ++t)
	{
		if (isAGoodIdeaToBuildTeam(*t))
		{
			candidateList1.push_back( (*t) );
			Int pri = (*t)->getTemplateInfo()->m_productionPriority;

			if (pri > hiPri)
			{
				hiPri = pri;
			}
		}
	}

	if (selectTeamToReinforce(hiPri)) {
		return true;
	}

	// check if no team prototypes are valid for production
	if (hiPri == invalidPri)
		return false;

	if (TheGlobalData->m_debugAI) {
		TheScriptEngine->AppendDebugMessage("**AI** Selecting team to build", false);
	}

	// collect all teams that are possible to build, and are at the highest priority
	Player::PlayerTeamList candidateList;
	Int count = 0;
	for (t = candidateList1.begin(); t != candidateList1.end(); ++t)
	{
		if ((*t)->getTemplateInfo()->m_productionPriority == hiPri)
		{
			candidateList.push_back( (*t) );
			count++;
		}
	}

	// pick a random team from the hi-priority set
	Int which = GameLogicRandomValue( 0, count-1 );

	TeamPrototype *teamProto = nullptr;
	Int i = 0;
	for (t = candidateList.begin(); t != candidateList.end(); ++t)
	{
		if (i == which)
		{
			teamProto = (*t);
			break;
		}

		i++;
	}
	if (teamProto) {
		if (!teamProto->getTemplateInfo()->m_hasHomeLocation && !isSkirmishAI()) {
			AsciiString teamStr = "Error : team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' has no Home Position (or Origin).");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
		}
		// Build it at low priority, as we have selected it automagically.
		buildSpecificAITeam(teamProto, false);
		m_readyToBuildTeam = false;
		m_teamTimer = m_teamSeconds*LOGICFRAMES_PER_SECOND;
		if (m_player->getMoney()->countMoney() < TheAI->getAiData()->m_resourcesPoor) {
			m_teamTimer = m_teamTimer/TheAI->getAiData()->m_teamPoorMod;
		}	else if (m_player->getMoney()->countMoney() > TheAI->getAiData()->m_resourcesWealthy) {
			m_teamTimer = m_teamTimer/TheAI->getAiData()->m_teamWealthyMod;
		}
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
/** Build a specific team.  If priorityBuild, put at front of queue with priority set. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildSpecificAIBuilding(const AsciiString &thingName)
{
	//
	AsciiString teamStr = "Error : Solo ai doesn't support BuildSpecificBuilding. '";
	teamStr.concat(thingName);
	teamStr.concat("' not built.");
	TheScriptEngine->AppendDebugMessage(teamStr, false);
}

// ------------------------------------------------------------------------------------------------
/** Build an upgrade. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildUpgrade(const AsciiString &upgrade)
{
	const UpgradeTemplate *curUpgrade = TheUpgradeCenter->findUpgrade(upgrade);
	if (curUpgrade==nullptr) {
		AsciiString msg = "Upgrade ";
		msg.concat(upgrade);
		msg.concat(" does not exist.  Ignoring request.");
		TheScriptEngine->AppendDebugMessage( msg, false);
		return;
	}
 	if (curUpgrade->getUpgradeType()==UPGRADE_TYPE_OBJECT) {
		AsciiString msg = "Player build upgrade: Upgrade ";
		msg.concat(upgrade);
		msg.concat(" is an object, not a player upgrade.  Ignoring request.");
		TheScriptEngine->AppendDebugMessage( msg, false);
		return;
	}
	// See if it is in progress.
	if (m_player->hasUpgradeInProduction(curUpgrade)) {
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" already has upgrade ");
		msg.concat(upgrade);
		msg.concat(" queued.  Ignoring request.");
		TheScriptEngine->AppendDebugMessage( msg, false);
		return;
	}
	// See if it is in progress.
	if (m_player->hasUpgradeComplete(curUpgrade)) {
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" already has upgrade ");
		msg.concat(upgrade);
		msg.concat(" completed.  Ignoring request.");
		TheScriptEngine->AppendDebugMessage( msg, false);
		return;
	}


	// No money.
	if( TheUpgradeCenter->canAffordUpgrade( m_player, curUpgrade ) == FALSE ) {
		AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
		msg.concat(" lacks money to build upgrade ");
		msg.concat(upgrade);
		msg.concat(" at this time.  Ignoring request.");
		TheScriptEngine->AppendDebugMessage( msg, false);
		return;
	}
	// Find a production queue.
	for( const BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext() )
	{
		Object *factory = TheGameLogic->findObjectByID( info->getObjectID() );
		if( factory )
		{
			if( factory->getStatusBits().test( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
				continue;
			if( factory->getStatusBits().test( OBJECT_STATUS_SOLD ) )
				continue;
			Bool canUpgradeHere = false;
			const CommandSet *commandSet = TheControlBar->findCommandSet( factory->getCommandSetString() );
			if( commandSet == nullptr) continue;
			for( Int j = 0; j < MAX_COMMANDS_PER_SET; j++ )
			{
				//Get the command button.
				const CommandButton *commandButton = commandSet->getCommandButton(j);
				if (commandButton==nullptr) continue;
				if (commandButton->getName().isEmpty() )	continue;
				if (commandButton->getUpgradeTemplate() == nullptr )	continue;
 				if (commandButton->getUpgradeTemplate()->getUpgradeName() == curUpgrade->getUpgradeName()) {
					canUpgradeHere = true;
				}
			}
			if (!canUpgradeHere) continue;
			ProductionUpdateInterface *pu = factory->getProductionUpdateInterface();
			// If it doesn't produce, continue.
			if (!pu) continue;
			// Try to queue it.
			if (pu->queueUpgrade(curUpgrade)) {
				AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
				msg.concat(" queues ");
				msg.concat(curUpgrade->getUpgradeName());
				msg.concat(" at ");
				msg.concat(factory->getTemplate()->getName());
				TheScriptEngine->AppendDebugMessage( msg, false);
				return;
			}
		}
	}

	AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
	msg.concat(" lacks factory to build upgrade ");
	msg.concat(upgrade);
	msg.concat(" at this time.  Ignoring request.");
	TheScriptEngine->AppendDebugMessage( msg, false);
}

// Places a supply center square on a face of its dock, AI_SUPPLY_CENTER_DOCK_GAP from the dock's edge.
// For centers that dock at bones (China, USA; not the GLA stash). Returns FALSE when no face is legal.
// Pro spot: long side, centred, front towards the dock; trucks load and unload in place in the gap.
static const Real AI_SUPPLY_CENTER_DOCK_GAP = 45.0f;

// A pro-spot face must also keep clear of QUEUED buildings (build-list entries with no live object); NO_OBJECT_OVERLAP
// sees only objects. Same test as the gap walk (geomCollidesWithGeom), footprint expanded by GX_QUEUED_CLEARANCE, in
// build-list order (deterministic). Locations are ground-relative (z = 0): grounded as buildStructureWithDozer does.
// Reads only; returns the first blocking entry or nullptr.
static const Real GX_QUEUED_CLEARANCE = 2.0f * PATHFIND_CELL_SIZE_F;

static BuildListInfo *gxQueuedBuildingAt(Player *player, const ThingTemplate *center, const Coord3D &p, Real angle)
{
	GeometryInfo geom = center->getTemplateGeometryInfo();
	geom.expandFootprint(GX_QUEUED_CLEARANCE);
	for (BuildListInfo *info = player->getBuildList(); info != nullptr; info = info->getNext())
	{
		if (info->getObjectID() != INVALID_ID && TheGameLogic->findObjectByID(info->getObjectID()) != nullptr)
			continue;	// it stands: the NO_OBJECT_OVERLAP test already sees it
		if (!info->isBuildable())
			continue;	// no rebuilds left: the AI will not build it again
		const ThingTemplate *t = TheThingFactory->findTemplate(info->getTemplateName());
		if (t == nullptr)
			continue;
		Coord3D q = *info->getLocation();
		q.z += TheTerrainLogic->getGroundHeight(q.x, q.y);	// ground-relative -> world, as buildStructureWithDozer does
		if (ThePartitionManager->geomCollidesWithGeom(&p, geom, angle, &q, t->getTemplateGeometryInfo(), info->getAngle()))
			return info;
	}
	return nullptr;
}

// China main base pro spot: a LONG dock face blocked only by QUEUED build-list entries moves them, not skips.
// Refused whole if over GX_RELOCATE_MAX colliders, any is CASH_GENERATOR or has a fallback, or the centre would sit
// on a queued entry's fallback spot; no free spot undoes all.
// Search: fixed-order square rings round the old spot, out to 400 wu (200 often failed: noslot). Deterministic.
// Only writes build-list locations (saved by BuildListInfo::xfer).
static const Real GX_RELOCATE_RADIUS = 400.0f;	// wu: the outermost ring's half-side round the entry's old spot
static const Int GX_RELOCATE_MAX = 4;			// colliders one face may move; more = skip the face as U2 does
static const Real GX_RELOCATE_STEP = 2.0f * PATHFIND_CELL_SIZE_F;	// ring spacing and the step along a ring
GX_HOOK_STATS(gxRelocHook, "dock_relocate");	// [GXHOOK] sub-hook: runs inside dock_face's own timer (a different var, so allowed)

static const Int GX_BUILD_ANGLES_MAX = 25;	// gxBuildAngles: the list angle + 3 turns from each of the 8 octants

struct GXRelocQueued
{
	BuildListInfo *info;
	const ThingTemplate *tmpl;
	Coord3D world;	// the entry's location grounded (ground-relative -> world, as gxQueuedBuildingAt grounds it)
	Bool hasFb;		// it has a saved fallback spot (hasFallbackSpot()), reserved against relocation destinations (test 4b)
	Coord3D fbWorld;	// hasFb only: that spot, read without using it up, grounded
	Real fbAngle;		// hasFb only: its angle there
	Real turn[GX_BUILD_ANGLES_MAX];	// every angle it may be built at, at its current spot (gxBuildAngles, below)
	Int nTurn;
	Real reach;		// gxRelocReach(tmpl, pad): tests (4) / (4b) cannot pair it with anything farther than reach + reach + clearances
};

// Every angle an entry may be built at: its list angle and, for a front-facing barracks or war factory door
// (AI_PLAYER_FACE_FRONT), the octant turn towards gxFrontPoint and +/-45 deg (every octant if the front can move).
// out[0] is the list angle; returns 1 .. GX_BUILD_ANGLES_MAX. The base front is read once per gxRelocateQueued call.
static const Int GX_DOOR_SKEW_CACHE = 8;
struct GXBuildFront
{
	Bool doors;		// the Hard AI turns a front-facing door here (AI_PLAYER_FACE_FRONT on, a skirmish AI)
	Bool fixed;		// doors only: gxFrontPointFixed()
	Bool hasFront;	// doors && fixed only: gxFrontPoint() answered, into `front`
	Coord3D front;
	// The base centre the door rule reads, and each template's half skew once per call: ring points ask per entry and a template's
	// module list is long. The answer is a pure function of the template, so the cache changes no value.
	Bool keyed;
	Coord3D centre;
	Int nSkew;
	struct { const ThingTemplate *t; Real hx, hy; } skew[GX_DOOR_SKEW_CACHE];
};
static void gxBuildFrontRead(AIPlayer *ai, GXBuildFront *f)
{
	f->doors = ai->gxFaceFront() && ai->isSkirmishAI();
	f->fixed = FALSE;
	f->hasFront = FALSE;
	f->front.zero();
	f->keyed = FALSE;
	f->centre.zero();
	f->nSkew = 0;
	if (!f->doors)
		return;
	f->keyed = ai->gxFrontDoors();
	if (f->keyed)
		ai->getBaseCenter(&f->centre);
	f->fixed = ai->gxFrontPointFixed();
	if (f->fixed)
		f->hasFront = ai->gxFrontPoint(&f->front);
}
static Int gxBuildAngles(GXBuildFront &fr, const ThingTemplate *t, const Coord3D &at, Real listAngle, Real out[GX_BUILD_ANGLES_MAX])
{
	Int n = 0;
	out[n++] = listAngle;
	if (!fr.doors || !gxDoorKind(t, fr.keyed))
		return n;
	Real wants[8];
	Int nw = 0;
	if (fr.fixed)
	{
		if (!fr.hasFront)
			return n;	// no front now, and none ever: the door is never turned
		if (fr.keyed)
		{
			// The door rule, buildStructureWithDozer's and buildBySupplies' own answer
			Real hx = 1.0f, hy = 0.0f;
			Int k = 0;
			while (k < fr.nSkew && fr.skew[k].t != t)
				++k;
			if (k < fr.nSkew)
			{
				hx = fr.skew[k].hx;
				hy = fr.skew[k].hy;
			}
			else
			{
				gxDoorHalfSkew(t, &hx, &hy);
				if (fr.nSkew < GX_DOOR_SKEW_CACHE)
				{
					fr.skew[fr.nSkew].t = t;
					fr.skew[fr.nSkew].hx = hx;
					fr.skew[fr.nSkew].hy = hy;
					++fr.nSkew;
				}
			}
			const Real want = gxDoorAim(fr.centre, hx, hy, at, fr.front);
			if (gxAirfieldKeepsList(t, want, listAngle))
				return n;	// buildStructureWithDozer's own rule: the list angle only
			wants[nw++] = want;
		}
		else
			wants[nw++] = gxOctantAngle(fr.front.x - at.x, fr.front.y - at.y);
	}
	else
	{
		static const float dirs[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
		for (Int d = 0; d < 8; ++d)
			wants[nw++] = gxOctantAngle(dirs[d][0], dirs[d][1]);	// gxOctantAngle's own 8 answers, bit for bit
	}
	for (Int w = 0; w < nw; ++w)
	{
		const Real want = wants[w];
		const Real tries[3] = { want, want + PI / 4, want - PI / 4 };	// buildStructureWithDozer's expressions
		for (Int k = 0; k < 3; ++k)
		{
			Bool dup = FALSE;
			for (Int i = 0; i < n && !dup; ++i)
				dup = out[i] == tries[k];
			if (!dup)
				out[n++] = tries[k];
		}
	}
	return n;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: geomCollidesWithGeom for the
// relocation's ring search and gxBuildTimeBlocked, with a 2D distance early-out in front. PartitionManager::geomCollidesWithGeom tests z
// first, then in 2D: two boxes by their corners (rectToFourPoints / testRotatedPointsAgainstRect - every corner, and every point a corner
// is tested inside, lies within sqrt(major^2 + minor^2) of its box's centre), a box against a non-box by turning the non-box into a SQUARE
// of half-side major (xy_collideTest_Rect_Circle: corners within major x sqrt 2), two non-boxes as circles of radius major. So every 2D
// shape it can test lies within gxGeomExtent of its centre, and two shapes whose centres are farther apart in 2D than the sum (+1 wu, the
// same slack as gxRelocApart) cannot collide: the full test - eight deterministic Sin/Cos and eight rotated corner tests for two boxes -
// is skipped only where it returns FALSE. Same guards as gxRelocApart (below): a negative or NaN radius, an extent or a coordinate of
// GX_RELOC_SANE wu or more skips nothing. Reads only.
// ------------------------------------------------------------------------------------------------
static const Real GX_RELOC_SANE = 1.0e6f;
static Real gxGeomExtent(const GeometryInfo &g)
{
	const Real major = g.getMajorRadius();
	const Real minor = g.getMinorRadius();
	if (!(major >= 0.0f) || !(minor >= 0.0f))
		return -1.0f;	// negative or NaN: no bound
	const Real e = g.getGeomType() == GEOMETRY_BOX ? sqrtf(major * major + minor * minor) : major * 1.4143f;	// 1.4143 > sqrt 2
	return e < GX_RELOC_SANE ? e : -1.0f;
}
static Bool gxGeomCollides(const Coord3D *p1, const GeometryInfo &g1, Real a1, const Coord3D *p2, const GeometryInfo &g2, Real a2)
{
	const Real e1 = gxGeomExtent(g1);
	const Real e2 = gxGeomExtent(g2);
	if (e1 >= 0.0f && e2 >= 0.0f
		&& p1->x < GX_RELOC_SANE && p1->x > -GX_RELOC_SANE && p1->y < GX_RELOC_SANE && p1->y > -GX_RELOC_SANE
		&& p2->x < GX_RELOC_SANE && p2->x > -GX_RELOC_SANE && p2->y < GX_RELOC_SANE && p2->y > -GX_RELOC_SANE)
	{
		const Real dist = e1 + e2 + 1.0f;
		const Real dx = p1->x - p2->x;
		const Real dy = p1->y - p2->y;
		if (dx * dx + dy * dy > dist * dist)
			return FALSE;
	}
	return ThePartitionManager->geomCollidesWithGeom(p1, g1, a1, p2, g2, a2);
}

// TRUE when BuildAssistant::isLocationClearOfObjects refuses `later` at laterPos / laterAngle because `standing`
// stands at standingPos / standingAngle. Mirrors its structure loop for templates (padding, bounds, factory exits).
// Not mirrored, so it only ever refuses MORE: the range pre-filter, IGNORE_STEALTHED and height (all tests are 2D).
// Reads only.
static Bool gxBuildTimeBlocked(Player *player, const ThingTemplate *later, const Coord3D &laterAt, Real laterAngle,
	const ThingTemplate *standing, const Coord3D &standingAt, Real standingRawAngle)
{
	if (!standing->isKindOf(KINDOF_STRUCTURE))
		return FALSE;
	if (!standing->isKindOf(KINDOF_INERT) && (standing->isKindOf(KINDOF_SHRUBBERY) || standing->isKindOf(KINDOF_CLEARED_BY_BUILD)))
		return FALSE;
	const Coord3D laterPos = laterAt;
	Coord3D standingPos = standingAt;
	standingPos.z = laterPos.z;	// 2D: see above
	const Real standingAngle = normalizeAngle(standingRawAngle);	// the standing object's orientation (Thing::setOrientation)
	// :780-790
	Real myFactoryExitWidth = later->getFactoryExitWidth();
	Real myExtraWidth = later->getFactoryExtraBibWidth();
	if (player && player->isSkirmishAIPlayer())
	{
		if (myExtraWidth < 3*PATHFIND_CELL_SIZE_F)
		{
			myExtraWidth = 3*PATHFIND_CELL_SIZE_F;
			myFactoryExitWidth -= myExtraWidth;
			if (myFactoryExitWidth < 0)
				myFactoryExitWidth = 0;
		}
	}
	// :792-816
	Bool checkMyExit = false;
	Coord3D myExitPos = laterPos;
	GeometryInfo myBounds = later->getTemplateGeometryInfo();
	myBounds.setMajorRadius(myBounds.getMajorRadius()+myExtraWidth);
	if (myBounds.getGeomType() != GEOMETRY_BOX)
		myBounds.set(GEOMETRY_BOX, false, 40, myBounds.getMajorRadius(), myBounds.getMajorRadius());
	else
		myBounds.setMinorRadius(myBounds.getMinorRadius()+myExtraWidth);
	GeometryInfo myGeom = later->getTemplateGeometryInfo();
	if (myGeom.getGeomType() != GEOMETRY_BOX)
		myGeom.setMinorRadius(myGeom.getMajorRadius());
	myGeom.setMajorRadius(myFactoryExitWidth/2.0f);
	if (myFactoryExitWidth > 0)
	{
		checkMyExit = true;
		Real c = Cos(laterAngle);
		Real s = Sin(laterAngle);
		Real offset = later->getTemplateGeometryInfo().getMajorRadius() + myFactoryExitWidth/2.0f;
		myExitPos.x += c*offset;
		myExitPos.y += s*offset;
	}
	// :839-865
	Real themFactoryExitWidth = standing->getFactoryExitWidth();
	Real hisExtraWidth = standing->getFactoryExtraBibWidth();
	Bool checkHisExit = false;
	Coord3D hisExitPos = standingPos;
	GeometryInfo hisBounds = standing->getTemplateGeometryInfo();
	hisBounds.setMajorRadius(hisBounds.getMajorRadius()+hisExtraWidth);
	if (hisBounds.getGeomType() != GEOMETRY_BOX)
		hisBounds.set(GEOMETRY_BOX, false, 40, hisBounds.getMajorRadius(), hisBounds.getMajorRadius());
	else
		hisBounds.setMinorRadius(hisBounds.getMinorRadius()+myExtraWidth);	// sic: :849 pads HIS minor radius with MY width
	GeometryInfo hisGeom = standing->getTemplateGeometryInfo();
	hisGeom.setMajorRadius(themFactoryExitWidth/2.0f);
	if (hisGeom.getGeomType() != GEOMETRY_BOX)
		hisGeom.setMinorRadius(standing->getTemplateGeometryInfo().getMajorRadius());
	if (themFactoryExitWidth > 0)
	{
		checkHisExit = true;
		Real c = Cos(standingAngle);
		Real s = Sin(standingAngle);
		Real offset = standing->getTemplateGeometryInfo().getMajorRadius() + themFactoryExitWidth/2.0f;
		hisExitPos.x += c*offset;
		hisExitPos.y += s*offset;
	}
	// :866-905
	if (gxGeomCollides(&standingPos, hisBounds, standingAngle, &laterPos, myBounds, laterAngle))
		return TRUE;
	if (!checkMyExit && !checkHisExit && !hisExtraWidth && !myExtraWidth)
		return FALSE;
	if (standing->isKindOf(KINDOF_IMMOBILE))
	{
		if (checkMyExit && gxGeomCollides(&standingPos, hisBounds, standingAngle, &myExitPos, myGeom, laterAngle))
			return TRUE;
		if (checkHisExit && gxGeomCollides(&hisExitPos, hisGeom, standingAngle, &laterPos, myBounds, laterAngle))
			return TRUE;
		if (checkMyExit && checkHisExit
			&& gxGeomCollides(&hisExitPos, hisGeom, standingAngle, &myExitPos, myGeom, laterAngle))
			return TRUE;
	}
	return FALSE;
}

// both build orders: (t1, p1, a1) built while (t2, p2, a2) stands, or the other way round
static Bool gxBuildTimeConflict(Player *player, const ThingTemplate *t1, const Coord3D &p1, Real a1, const ThingTemplate *t2,
	const Coord3D &p2, Real a2)
{
	return gxBuildTimeBlocked(player, t1, p1, a1, t2, p2, a2) || gxBuildTimeBlocked(player, t2, p2, a2, t1, p1, a1);
}

static void gxRelocLogFail(const Player *player, Int face, const ThingTemplate *center, const char *entry, const char *reason)
{
#if defined(GX_TEST_INSTRUMENTS)
	fprintf(stderr, "[GXRELOC] f=%u p=%d ev=relocate_fail face=%d centre=%s entry=%s reason=%s\n", (unsigned)TheGameLogic->getFrame(),
		(Int)player->getPlayerIndex(), face, center->getName().str(), entry, reason);
	fflush(stderr);
#else
	(void)player; (void)face; (void)center; (void)entry; (void)reason;
#endif
}

// gxRelocReach bounds how far from its centre any shape the build-time tests build for template t reaches; pairs
// farther than reach + reach + 2 x GX_QUEUED_CLEARANCE (+1 wu) apart are skipped (only FALSE tests are not run).
// Bound: major + max(major, minor) + 1.5 x exit width + 2 x pad. A negative radius, exit width or bib, or a reach of
// GX_RELOC_SANE wu or more, gets no reach (-1) and none of its pairs is skipped.
static Real gxRelocReach(const ThingTemplate *t, Real pad)
{
	const GeometryInfo &g = t->getTemplateGeometryInfo();
	const Real major = g.getMajorRadius();
	const Real minor = g.getMinorRadius();
	const Real exitW = t->getFactoryExitWidth();
	if (major < 0.0f || minor < 0.0f || exitW < 0.0f || t->getFactoryExtraBibWidth() < 0.0f)
		return -1.0f;
	const Real big = major > minor ? major : minor;
	const Real reach = major + big + 1.5f * exitW + 2.0f * pad;
	return reach < GX_RELOC_SANE ? reach : -1.0f;
}
// TRUE when no test (4) / (4b) can pair two entries this far apart: both have a reach (gxRelocReach) and their 2D distance exceeds the sum
static Bool gxRelocApart(const Coord3D &a, Real reachA, const Coord3D &b, Real reachB, Real clr)
{
	if (reachA < 0.0f || reachB < 0.0f)
		return FALSE;
	if (a.x > GX_RELOC_SANE || a.x < -GX_RELOC_SANE || a.y > GX_RELOC_SANE || a.y < -GX_RELOC_SANE
		|| b.x > GX_RELOC_SANE || b.x < -GX_RELOC_SANE || b.y > GX_RELOC_SANE || b.y < -GX_RELOC_SANE)
		return FALSE;
	const Real dist = reachA + reachB + clr;
	const Real dx = a.x - b.x;
	const Real dy = a.y - b.y;
	return dx * dx + dy * dy > dist * dist;
}

static Bool gxRelocateQueued(AIPlayer *ai, Player *player, const ThingTemplate *center, const Coord3D &p, Real centerAngle,
	const Coord3D *dockPos, const GeometryInfo &border, Real dockAngle, const Coord3D *keep, Real keepAngle, Int face)
{
	GX_HOOK_TIMER_ON(gxRelocHook);
	const GeometryInfo &centerGeom = center->getTemplateGeometryInfo();
	GeometryInfo centreClr = centerGeom;
	centreClr.expandFootprint(GX_QUEUED_CLEARANCE);

	GXBuildFront front;
	gxBuildFrontRead(ai, &front);	// once per call: gxBuildAngles' base front (constant through the call, see there)

	// every unbuilt buildable entry, in build-list order - gxQueuedBuildingAt's filter. Nothing it reads changes in this call (only
	// locations move), so it is taken once; a moved entry's `world` is updated with it.
	std::vector<GXRelocQueued> queued;
	for (BuildListInfo *info = player->getBuildList(); info != nullptr; info = info->getNext())
	{
		if (info->getObjectID() != INVALID_ID && TheGameLogic->findObjectByID(info->getObjectID()) != nullptr)
			continue;	// it stands or is under construction: NO_OBJECT_OVERLAP sees it
		if (!info->isBuildable())
			continue;	// no rebuilds left
		const ThingTemplate *t = TheThingFactory->findTemplate(info->getTemplateName());
		if (t == nullptr)
			continue;
		GXRelocQueued e;
		e.info = info;
		e.tmpl = t;
		e.world = *info->getLocation();
		e.world.z += TheTerrainLogic->getGroundHeight(e.world.x, e.world.y);
		e.hasFb = info->hasFallbackSpot();
		e.fbWorld.zero();
		e.fbAngle = 0.0f;
		if (e.hasFb)
		{
			e.fbWorld = *info->getFallbackLocation();	// ground-relative, as stored (buildBySupplies)
			e.fbWorld.z += TheTerrainLogic->getGroundHeight(e.fbWorld.x, e.fbWorld.y);
			e.fbAngle = info->getFallbackAngle();
		}
		e.nTurn = gxBuildAngles(front, t, e.world, info->getAngle(), e.turn);
		e.reach = 0.0f;
		queued.push_back(e);
	}
	// gxRelocReach's pad: the largest padding gxBuildTimeBlocked can put on any shape of these pairs - a skirmish AI's 3-cell minimum or
	// the biggest bib among the queued entries (each is both a `later` and a `standing` there)
	Real reachPad = 3.0f * PATHFIND_CELL_SIZE_F;
	for (Int i = 0; i < (Int)queued.size(); ++i)
		if (queued[i].tmpl->getFactoryExtraBibWidth() > reachPad)
			reachPad = queued[i].tmpl->getFactoryExtraBibWidth();
	for (Int i = 0; i < (Int)queued.size(); ++i)
		queued[i].reach = gxRelocReach(queued[i].tmpl, reachPad);
	const Real reachClr = 2.0f * GX_QUEUED_CLEARANCE + 1.0f;

	// Colliders, in build-list order: entries whose footprint (+clearance) overlaps the centre at p or conflict with it
	// at build time (gxBuildTimeConflict); left in place they fail their build and take the stock wiggle.
	// A PROTECTED collider (CASH_GENERATOR or with a fallback spot) refuses the whole relocation; all count to cap.
	// The centre may not sit on a queued entry's FALLBACK spot either (reason=centre_on_fallback).
	Int col[GX_RELOCATE_MAX];	// indices into queued
	Int n = 0;
	Int bad = -1;				// the first refusing entry: a protected collider, or one whose fallback spot the centre would take
	const char *badReason = nullptr;
	for (Int i = 0; i < (Int)queued.size(); ++i)
	{
		const GXRelocQueued &e = queued[i];
		// at every angle the entry may be built at (gxBuildAngles; a door that clears the centre at its list angle but
		// would be turned into it, built first, takes the face from the centre)
		Bool overlaps = FALSE;
		Bool conflicts = FALSE;
		for (Int k = 0; k < e.nTurn; ++k)
		{
			overlaps = overlaps || ThePartitionManager->geomCollidesWithGeom(&p, centreClr, centerAngle, &e.world,
				e.tmpl->getTemplateGeometryInfo(), e.turn[k]);
			conflicts = conflicts || gxBuildTimeConflict(player, e.tmpl, e.world, e.turn[k], center, p, centerAngle);
		}
		if (overlaps || conflicts)
		{
			// the reason text carries the protected line's two fields: gxRelocLogFail prints "reason=" + it, last on the line
			if (bad < 0 && e.tmpl->isKindOf(KINDOF_CASH_GENERATOR))
			{
				bad = i;
				badReason = overlaps ? "protected protection=cash conflict=footprint" : "protected protection=cash conflict=padding";
			}
			else if (bad < 0 && e.info->hasFallbackSpot())
			{
				bad = i;
				badReason = overlaps ? "protected protection=fallback conflict=footprint" : "protected protection=fallback conflict=padding";
			}
			if (n < GX_RELOCATE_MAX)
				col[n] = i;
			++n;
		}
		if (bad < 0 && e.hasFb
			&& (ThePartitionManager->geomCollidesWithGeom(&p, centreClr, centerAngle, &e.fbWorld, e.tmpl->getTemplateGeometryInfo(), e.fbAngle)
				|| gxBuildTimeConflict(player, e.tmpl, e.fbWorld, e.fbAngle, center, p, centerAngle)))
		{
			bad = i;
			badReason = "centre_on_fallback";
		}
	}
	if (n == 0)
		return FALSE;	// unreachable (the caller found one with the same test); U2's skip keeps N1 by construction
	if (n > GX_RELOCATE_MAX)
	{
		gxRelocLogFail(player, face, center, "-", "cap");
		return FALSE;
	}
	if (bad >= 0)
	{
		gxRelocLogFail(player, face, center, queued[bad].info->getTemplateName().str(), badReason);
		return FALSE;
	}

	// the builder for CLEAR_PATH: the first own live dozer in object-list order (read only)
	const Object *dozer = nullptr;
	for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() == player && obj->isKindOf(KINDOF_DOZER) && !obj->isEffectivelyDead())
		{
			dozer = obj;
			break;
		}
	}
	Coord3D keepWorld;
	keepWorld.zero();
	if (keep != nullptr)
	{
		keepWorld = *keep;
		keepWorld.z += TheTerrainLogic->getGroundHeight(keepWorld.x, keepWorld.y);
	}

	const Int rings = (Int)(GX_RELOCATE_RADIUS / GX_RELOCATE_STEP);
	Coord3D saved[GX_RELOCATE_MAX];
	Int ringOf[GX_RELOCATE_MAX];
	for (Int m = 0; m < n; ++m)
	{
		GXRelocQueued &e = queued[col[m]];
		const Coord3D o = *e.info->getLocation();
		const Real a = e.info->getAngle();
		const GeometryInfo &g = e.tmpl->getTemplateGeometryInfo();
		GeometryInfo gClr = g;
		gClr.expandFootprint(GX_QUEUED_CLEARANCE);
		Coord3D c;
		c.zero();
		Int found = 0;	// the ring the spot is on, 0 = none
		for (Int k = 1; k <= rings && found == 0; ++k)
		{
			for (Int j = 0; j < 8 * k; ++j)
			{
				Int ix, iy;
				if (j < 2 * k + 1)			{ ix = j - k;					iy = -k; }						// y-low edge, x ascending
				else if (j < 4 * k + 2)		{ ix = j - (2 * k + 1) - k;	iy = k; }						// y-high edge, x ascending
				else if (j < 6 * k + 1)		{ ix = -k;						iy = j - (4 * k + 2) - k + 1; }	// x-low edge, y ascending
				else						{ ix = k;						iy = j - (6 * k + 1) - k + 1; }	// x-high edge, y ascending
				c.x = o.x + (Real)ix * GX_RELOCATE_STEP;
				c.y = o.y + (Real)iy * GX_RELOCATE_STEP;
				Coord3D q = c;
				q.z = TheTerrainLogic->getGroundHeight(c.x, c.y);
				// tests (1) - (4b) hold for EVERY angle the entry may be built at here (gxBuildAngles: the list angle a, and a door's turns
				// to the base front), and against every angle another queued entry may be built at (its own turn[])
				Real ma[GX_BUILD_ANGLES_MAX];
				const Int mn = gxBuildAngles(front, e.tmpl, q, a, ma);
				Bool hit = FALSE;
				for (Int i = 0; i < mn && !hit; ++i)
				{
					const Real ang = ma[i];
					// (1) the new centre: its clearance, and both build orders
					hit = gxGeomCollides(&p, centreClr, centerAngle, &q, g, ang)
						|| gxBuildTimeConflict(player, e.tmpl, q, ang, center, p, centerAngle);
					// (2) the dock's gap ring, in 2D at the dock's height like the face walk
					if (!hit)
					{
						Coord3D d = q;
						d.z = dockPos->z;
						hit = gxGeomCollides(&d, g, ang, dockPos, border, dockAngle);
					}
					// (3) the centre at its fallback spot: its footprint, and both build orders
					if (!hit && keep != nullptr)
						hit = gxGeomCollides(&keepWorld, centerGeom, keepAngle, &q, g, ang)
							|| gxBuildTimeConflict(player, e.tmpl, q, ang, center, keepWorld, keepAngle);
					// (4) every other queued entry, at each of its own build angles: clearance, and both build orders
					for (Int x = 0; x < (Int)queued.size() && !hit; ++x)
					{
						if (x == col[m])
							continue;
						if (gxRelocApart(q, e.reach, queued[x].world, queued[x].reach, reachClr))
							continue;	// too far apart for any test below to be TRUE (gxRelocReach)
						for (Int xt = 0; xt < queued[x].nTurn && !hit; ++xt)
							hit = gxGeomCollides(&q, gClr, ang, &queued[x].world, queued[x].tmpl->getTemplateGeometryInfo(),
									queued[x].turn[xt])
								|| gxBuildTimeConflict(player, e.tmpl, q, ang, queued[x].tmpl, queued[x].world, queued[x].turn[xt]);
					}
					// (4b) every queued entry's saved FALLBACK spot, as (4): clearance, and both build orders (a non-colliding
					// queued centre that later uses its fallback would otherwise meet a relocated building there). A mover never has one - a
					// collider with a fallback spot refuses the relocation - so no entry is tested against itself. A fallback spot is a supply
					// centre's, which no door turn moves: its saved angle only.
					for (Int x = 0; x < (Int)queued.size() && !hit; ++x)
					{
						if (queued[x].hasFb && !gxRelocApart(q, e.reach, queued[x].fbWorld, queued[x].reach, reachClr))
							hit = gxGeomCollides(&q, gClr, ang, &queued[x].fbWorld, queued[x].tmpl->getTemplateGeometryInfo(),
									queued[x].fbAngle)
								|| gxBuildTimeConflict(player, e.tmpl, q, ang, queued[x].tmpl, queued[x].fbWorld, queued[x].fbAngle);
					}
				}
				if (hit)
					continue;
				// (5) legal to build, reachable by the dozer when there is one
				if (gxLegalToBuildOnGround(&q, e.tmpl, a, BuildAssistant::CLEAR_PATH | BuildAssistant::TERRAIN_RESTRICTIONS |
					BuildAssistant::NO_OBJECT_OVERLAP, dozer, player) != LBC_OK)
					continue;
				// (6) safe, as processBaseBuilding asks it: the location as it would be stored (ground-relative), the entry's template
				Coord3D asStored = c;
				asStored.z = 0.0f;
				if (!ai->isLocationSafe(&asStored, e.tmpl))
					continue;
				found = k;
				break;
			}
		}
		TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
		if (found == 0)
		{
			for (Int r = 0; r < m; ++r)
				queued[col[r]].info->setLocation(saved[r]);	// exact rollback: nothing of this call stays
			gxRelocLogFail(player, face, center, e.info->getTemplateName().str(), "noslot");
			return FALSE;
		}
		saved[m] = o;
		ringOf[m] = found;
		Coord3D stored;
		stored.x = c.x;
		stored.y = c.y;
		stored.z = 0.0f;	// ground-relative, as every build-list location
		e.info->setLocation(stored);
		e.world = stored;
		e.world.z += TheTerrainLogic->getGroundHeight(stored.x, stored.y);
		e.nTurn = gxBuildAngles(front, e.tmpl, e.world, a, e.turn);
	}
#if defined(GX_TEST_INSTRUMENTS)
	for (Int m = 0; m < n; ++m)
	{
		const BuildListInfo *info = queued[col[m]].info;
		fprintf(stderr, "[GXRELOC] f=%u p=%d ev=relocate face=%d centre=%s entry=%s from=%d,%d to=%d,%d ring=%d\n",
			(unsigned)TheGameLogic->getFrame(), (Int)player->getPlayerIndex(), face, center->getName().str(), info->getTemplateName().str(),
			(Int)saved[m].x, (Int)saved[m].y, (Int)info->getLocation()->x, (Int)info->getLocation()->y, (Int)(ringOf[m] * GX_RELOCATE_STEP));
	}
	fflush(stderr);
#else
	(void)ringOf;
#endif
	return TRUE;
}

// GeneralsX: One dock-face candidate: the trucks' shuttle line across the gap is passable ground (no cliff or
// water), the centre is legal on the ground there, and it is not over a QUEUED building (gxQueuedBuildingAt). `off` slides the
// centre along the face (0 = centred, the pro spot); the shuttle line slides with it. Reads only.
// GeneralsX: `queuedOnly`, when given: set TRUE, with the candidate (world space, grounded) in `queuedAt`, when
// the queued test is the ONLY one that refused it - what gxRelocateQueued needs (findDockFacePlacement); FALSE otherwise.
// GeneralsX: base-layout: gxQueuedBuildingAt tests a queued entry at its list angle only, so with the
// door rule a face could pass and a queued door's later turn then land on the centre. For GXFrontDoors: the same test at the door's other build
// angles. Reads only. Returns the first blocking entry or nullptr.
static BuildListInfo *gxQueuedDoorTurnAt(AIPlayer *ai, Player *player, const ThingTemplate *center, const Coord3D &p, Real angle)
{
	GX_HOOK_TIMER_ON(gxFaceFrontHook);
	GXBuildFront front;
	gxBuildFrontRead(ai, &front);
	if (!front.keyed)
		return nullptr;
	GeometryInfo geom = center->getTemplateGeometryInfo();
	geom.expandFootprint(GX_QUEUED_CLEARANCE);
	for (BuildListInfo *info = player->getBuildList(); info != nullptr; info = info->getNext())
	{
		if (info->getObjectID() != INVALID_ID && TheGameLogic->findObjectByID(info->getObjectID()) != nullptr)
			continue;	// it stands: the NO_OBJECT_OVERLAP test already sees it
		if (!info->isBuildable())
			continue;	// no rebuilds left: the AI will not build it again
		const ThingTemplate *t = TheThingFactory->findTemplate(info->getTemplateName());
		if (t == nullptr || !gxDoorKind(t, TRUE))
			continue;
		Coord3D q = *info->getLocation();
		q.z += TheTerrainLogic->getGroundHeight(q.x, q.y);
		Real ang[GX_BUILD_ANGLES_MAX];
		const Int n = gxBuildAngles(front, t, q, info->getAngle(), ang);
		for (Int k = 1; k < n; ++k)	// ang[0], the list angle: gxQueuedBuildingAt's own test
		{
			if (ThePartitionManager->geomCollidesWithGeom(&p, geom, angle, &q, t->getTemplateGeometryInfo(), ang[k]))
				return info;
		}
	}
	return nullptr;
}

static Bool gxDockFaceSpotOk(const Object *dock, const ThingTemplate *center, Player *player, Int face, Real nx, Real ny, Real dist,
	Real dockHalf, Real centerAngle, Real off, Coord3D *pos, Bool *queuedOnly = nullptr, Coord3D *queuedAt = nullptr, AIPlayer *ai = nullptr)
{
	if (queuedOnly != nullptr)
		*queuedOnly = FALSE;
	const Coord3D *dockPos = dock->getPosition();
	const Real tx = -ny;	// along the face
	const Real ty = nx;
	// the centre's +x - its front, where its trucks come out - points AT the dock, so its local-x half-length lies along the
	// face normal
	const Real centerHalf = center->getTemplateGeometryInfo().getMajorRadius();
	const Real margin = 1.5f * PATHFIND_CELL_SIZE_F;	// keep both ends off the cells under the two footprints
	Coord3D gapStart = *dockPos;
	Coord3D gapEnd = *dockPos;
	gapStart.x += nx * (dockHalf + margin) + tx * off;
	gapStart.y += ny * (dockHalf + margin) + ty * off;
	gapEnd.x += nx * (dist - centerHalf - margin) + tx * off;
	gapEnd.y += ny * (dist - centerHalf - margin) + ty * off;
	if (!TheAI->pathfinder()->isGroundPathPassable(FALSE, gapStart, LAYER_GROUND, gapEnd, 1))
		return FALSE;
	Coord3D p = *dockPos;
	p.x = dockPos->x + nx * dist + tx * off;
	p.y = dockPos->y + ny * dist + ty * off;
	// the walk measures the gap in 2D at the dock's height; the overlap test checks height first, so test at the ground
	p.z = TheTerrainLogic->getGroundHeight(p.x, p.y);
	const Bool legal = TheBuildAssistant->isLocationLegalToBuild( &p, center, centerAngle,
																				BuildAssistant::CLEAR_PATH |
																				BuildAssistant::TERRAIN_RESTRICTIONS |
																				BuildAssistant::NO_OBJECT_OVERLAP,
																				nullptr, player ) == LBC_OK;
	TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.
	if (!legal)
		return FALSE;
	// A face over a QUEUED building is skipped too (gxQueuedBuildingAt, above)
	const BuildListInfo *q = gxQueuedBuildingAt(player, center, p, centerAngle);
	if (q == nullptr && ai != nullptr && ai->gxFrontDoors())
		q = gxQueuedDoorTurnAt(ai, player, center, p, centerAngle);
	if (q != nullptr)
	{
#if defined(GX_TEST_INSTRUMENTS)
		fprintf(stderr, "[GXPLACE] f=%u p=%d ev=skip_queued face=%d off=%d centre=%s blocker=%s at=%d,%d blocker_at=%d,%d\n", (unsigned)TheGameLogic->getFrame(),
			(Int)player->getPlayerIndex(), face, (Int)off, center->getName().str(), q->getTemplateName().str(), (Int)p.x, (Int)p.y,
			(Int)q->getLocation()->x, (Int)q->getLocation()->y);
		fflush(stderr);
#endif
		if (queuedOnly != nullptr)
		{
			*queuedOnly = TRUE;
			*queuedAt = p;
		}
		return FALSE;
	}
	*pos = p;
	return TRUE;
}

// GeneralsX: `ai` (the AIPlayer that owns `player`, for its isLocationSafe gate; Player keeps its AI private,
// so the caller passes itself), `keep` / `keepAngle` (today's spot, ground-relative, the entry's fallback): given, a centred LONG face
// that only QUEUED entries block moves them (gxRelocateQueued, above) and keeps the face. No `ai`, no relocation.
static Bool findDockFacePlacement(const Object *dock, const ThingTemplate *center, const Coord2D &towardsBase, Player *player,
	Coord3D *pos, Real *angle, AIPlayer *ai = nullptr, const Coord3D *keep = nullptr, Real keepAngle = 0.0f)
{
	GX_HOOK_TIMER("dock_face");
	const Coord3D *dockPos = dock->getPosition();
	const Real dockAngle = dock->getOrientation();
	const GeometryInfo &dockGeom = dock->getGeometryInfo();
	// Faces in this order: the dock's LONG faces first (the two whose length is the box's longer side - where GO's top players
	// put it), the one facing the base first; then the short faces, likewise. Ties keep the lower face (deterministic).
	const Bool isBox = dockGeom.getGeomType() == GEOMETRY_BOX;
	const Bool longFacesAreX = isBox && dockGeom.getMajorRadius() <= dockGeom.getMinorRadius();
	Int order[4] = { 0, 1, 2, 3 };
	Real key[4];
	for (Int k = 0; k < 4; ++k)
	{
		const Real a = dockAngle + k * PI / 2;
		const Bool isLong = !isBox || (((k & 1) == 0) == longFacesAreX);
		key[k] = (isLong ? 0.0f : 10.0f) - (Cos(a) * towardsBase.x + Sin(a) * towardsBase.y);
	}
	for (Int i = 1; i < 4; ++i)
		for (Int j = i; j > 0 && key[order[j]] < key[order[j - 1]]; --j)
		{
			const Int tmp = order[j];
			order[j] = order[j - 1];
			order[j - 1] = tmp;
		}
	GeometryInfo border = dockGeom;
	border.expandFootprint(AI_SUPPLY_CENTER_DOCK_GAP);
	const GeometryInfo &centerGeom = center->getTemplateGeometryInfo();
	// Pass 1: every face, centred - the pro spot. Pass 2, only when pass 1 found nothing: the LONG faces again, the centre slid along
	// the face, still facing the dock across the same gap.
	// GeneralsX: Pass 2 exists for the main base: at ~45 s the China base template already fills the dock's
	// faces (a queued War Factory or Power Plant spot on one long face, a standing Barracks or Power Plant on the other, the Command
	// Center on a short face), so the centred spot failed on 5 of 6 main bases in 0.9.68 game and the centre went to
	// today's spot with 3 trucks. A slid centre's door still faces the dock's long side.
	// Only on a BOX dock, and only while the shuttle line still starts on the face itself (|off| <= the face's half-length less the
	// margin): off a cylinder, or past a short face's end, the line would start in open ground and could miss water or a cliff
	// between it and the dock.
	static const Real slide[4] = { 20.0f, -20.0f, 40.0f, -40.0f };
	for (Int pass = 0; pass < 2; ++pass)
	{
		for (Int t = 0; t < 4; ++t)
		{
			const Bool isLong = !isBox || (((order[t] & 1) == 0) == longFacesAreX);
			if (pass == 1 && (!isBox || !isLong))
				continue;
			const Real normal = dockAngle + order[t] * PI / 2;
			const Real nx = Cos(normal);
			const Real ny = Sin(normal);
			const Real centerAngle = normal + PI;	// facing back across the gap, at the dock
			// slide out along the face normal until the center clears the expanded footprint
			Coord3D p = *dockPos;
			Real dist = 0.0f;
			for (; dist < 600.0f; dist += 1.0f)
			{
				p.x = dockPos->x + nx * dist;
				p.y = dockPos->y + ny * dist;
				if (!ThePartitionManager->geomCollidesWithGeom(&p, centerGeom, centerAngle, dockPos, border, dockAngle))
					break;
			}
			if (dist >= 600.0f)
				continue;
			const Bool alongX = (order[t] & 1) == 0;
			const Real dockHalf = (dockGeom.getGeomType() == GEOMETRY_BOX && !alongX) ? dockGeom.getMinorRadius() : dockGeom.getMajorRadius();
			if (pass == 0)
			{
				Bool queuedOnly = FALSE;
				Coord3D queuedAt;
				queuedAt.zero();
				if (gxDockFaceSpotOk(dock, center, player, order[t], nx, ny, dist, dockHalf, centerAngle, 0.0f, pos, &queuedOnly, &queuedAt, ai))
				{
					*angle = centerAngle;
					return TRUE;
				}
				// GeneralsX: the China main base pro spot: a centred LONG face that only QUEUED entries block
				// keeps the centre and moves them (gxRelocateQueued, above); a relocation that cannot move them all restores every entry
				// exactly and the face is skipped as before. Short faces and the slid candidates of pass 2 never relocate.
				if (queuedOnly && isLong && ai != nullptr
					&& gxRelocateQueued(ai, player, center, queuedAt, centerAngle, dockPos, border, dockAngle, keep, keepAngle, order[t]))
				{
					*pos = queuedAt;
					*angle = centerAngle;
					return TRUE;
				}
				continue;
			}
			// the face runs along the other axis: its half-length is the radius the normal does not use
			const Real faceHalf = alongX ? dockGeom.getMinorRadius() : dockGeom.getMajorRadius();
			for (Int s = 0; s < 4; ++s)
			{
				if (fabs(slide[s]) > faceHalf - 1.5f * PATHFIND_CELL_SIZE_F)
					continue;
				if (gxDockFaceSpotOk(dock, center, player, order[t], nx, ny, dist, dockHalf, centerAngle, slide[s], pos, nullptr, nullptr, ai))
				{
					*angle = centerAngle;
					return TRUE;
				}
			}
		}
	}
	return FALSE;
}

// ------------------------------------------------------------------------------------------------
/** Build a supply center near a supply source with minimumCash or more resources. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildBySupplies(Int minimumCash, const AsciiString& thingName)
{
	const ThingTemplate* tTemplate = TheThingFactory->findTemplate(thingName);
	if (!tTemplate)
	{
		DEBUG_CRASH(("Template %s should exist; check ini and script files.", thingName.str()));
		return;
	}

	// GeneralsX: a supply centre goes to the nearest SAFE source (same rule as isSupplySourceSafe, which gates the expansion scripts).
	Object *bestSupplyWarehouse = findSupplyCenter(minimumCash, tTemplate->isKindOf(KINDOF_CASH_GENERATOR));

	if (!tTemplate->isKindOf(KINDOF_CASH_GENERATOR)) {
		// Build by the current warehouse.
		Object *curWarehouse = TheGameLogic->findObjectByID(m_curWarehouseID);
		if (curWarehouse) {
			bestSupplyWarehouse = curWarehouse;
		}
	}


	if (bestSupplyWarehouse) {
		Coord3D location;
		location = *bestSupplyWarehouse->getPosition();
		// offset back towards the base.
		// towards gxSupplyRef(), the stock centre on a keyed skirmish list, else m_baseCenter
		Coord2D offset;
		offset.x = location.x - gxSupplyRef().x;
		offset.y = location.y - gxSupplyRef().y;
		offset.normalize();
		Real radius = 3*PATHFIND_CELL_SIZE_F;
		// GeneralsX: getSkirmishEnemyPlayer() may be null (no current script player, or no
		// human in the match: AI-only games, the challenge dummy). Keep the base-relative offset then. tsh batch2 #395, idea of
		// Generals Online b5770a12b (reimplemented).
		Player *enemy = TheScriptEngine->getSkirmishEnemyPlayer();
		if (enemy && !tTemplate->isKindOf(KINDOF_CASH_GENERATOR)) {
			// It's probably a defensive structure - build towards the enemy.
			Region2D bounds;
			getPlayerStructureBounds(&bounds, enemy->getPlayerIndex());
			offset.x = location.x - (bounds.lo.x+bounds.hi.x)*0.5f;
			offset.y = location.y - (bounds.lo.y+bounds.hi.y)*0.5f;
			offset.normalize();
			radius = bestSupplyWarehouse->getGeometryInfo().getBoundingCircleRadius();
		}
		location.x -= offset.x*radius;
		location.y -= offset.y*radius;
		Real angle = tTemplate->getPlacementViewAngle();
		// GeneralsX: a factory the spend scripts queue beside a dock is most of the
		// backwards doors (43 of 48): it was queued at the view angle, R then often could not turn it at that
		// spot, and the wiggle moved it with the view angle. With GXFrontDoors it takes the door rule's angle here and the dozer path's own
		// flags in the first test, so the spot it queues is one the dozer can use at that angle.
		UnsignedInt gxFirstFlags = BuildAssistant::NO_OBJECT_OVERLAP;
		const Bool gxKeyedDoor = m_gxFaceFront && isSkirmishAI() && gxFrontDoors() && gxDoorKind(tTemplate, TRUE);
		if (gxKeyedDoor)
		{
			GX_HOOK_TIMER_ON(gxFaceFrontHook);
			Coord3D gxFront;
			if (gxFrontPoint(&gxFront))
			{
				angle = gxDoorWant(m_baseCenter, tTemplate, location, gxFront);
				gxFirstFlags = BuildAssistant::CLEAR_PATH | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::NO_OBJECT_OVERLAP;
			}
		}

 		// validate the the position to build at is valid
		Bool valid=false;
		Coord3D newPos = location;
		if( gxLegalToBuildOnGround( &location, tTemplate, angle,
																									 gxFirstFlags,
																									 nullptr, m_player ) != LBC_OK ) {
			// Warn.
			const Coord3D *warehouseLocation = bestSupplyWarehouse->getPosition();
			AsciiString debugMessage;
			debugMessage.format(" %s - buildBySupplies unable to place near dock at (%.2f,%.2f).  Attempting to adjust position.",
													tTemplate->getName().str(),
													warehouseLocation->x,
													warehouseLocation->y
													);
			TheScriptEngine->AppendDebugMessage(debugMessage, false);
			if( TheGlobalData->m_debugSupplyCenterPlacement )
				DEBUG_LOG(("%s", debugMessage.str()));
			// try to fix.
			Real posOffset;
			// Wiggle it a little :)
			for (posOffset = 0; posOffset<2*SUPPLY_CENTER_CLOSE_DIST; posOffset += 2*PATHFIND_CELL_SIZE_F) {
				Real offset = posOffset/2;
				Real xPos, yPos;
				yPos = location.y-offset;
				for (xPos = location.x-offset; xPos <= location.x+offset; xPos+=PATHFIND_CELL_SIZE_F) {
					newPos.x = xPos;
					newPos.y = yPos;
					valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																							 BuildAssistant::CLEAR_PATH |
																							 BuildAssistant::TERRAIN_RESTRICTIONS |
																							 BuildAssistant::NO_OBJECT_OVERLAP,
																							 nullptr, m_player ) == LBC_OK;
					if (valid) break;
					if( TheGlobalData->m_debugSupplyCenterPlacement )
						DEBUG_LOG(("buildBySupplies -- Fail at (%.2f,%.2f)", newPos.x, newPos.y));
					newPos.y = yPos+posOffset;
					valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																							 BuildAssistant::CLEAR_PATH |
																							 BuildAssistant::TERRAIN_RESTRICTIONS |
																							 BuildAssistant::NO_OBJECT_OVERLAP,
																							 nullptr, m_player ) == LBC_OK;
					if (valid) break;
					if( TheGlobalData->m_debugSupplyCenterPlacement )
						DEBUG_LOG(("buildBySupplies -- Fail at (%.2f,%.2f)", newPos.x, newPos.y));
				}
				if (valid) break;
				xPos = location.x-offset;
				for (yPos = location.y-offset; yPos <= location.y+offset; yPos+=PATHFIND_CELL_SIZE_F) {
					newPos.x = xPos;
					newPos.y = yPos;
					valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																							 BuildAssistant::CLEAR_PATH |
																							 BuildAssistant::TERRAIN_RESTRICTIONS |
																							 BuildAssistant::NO_OBJECT_OVERLAP,
																							 nullptr, m_player ) == LBC_OK;
					if (valid) break;
					if( TheGlobalData->m_debugSupplyCenterPlacement )
						DEBUG_LOG(("buildBySupplies -- Fail at (%.2f,%.2f)", newPos.x, newPos.y));
					newPos.x = xPos+posOffset;
					valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																							 BuildAssistant::CLEAR_PATH |
																							 BuildAssistant::TERRAIN_RESTRICTIONS |
																							 BuildAssistant::NO_OBJECT_OVERLAP,
																							 nullptr, m_player ) == LBC_OK;
					if (valid) break;
					if( TheGlobalData->m_debugSupplyCenterPlacement )
						DEBUG_LOG(("buildBySupplies -- Fail at (%.2f,%.2f)", newPos.x, newPos.y));
				}
				if (valid) break;
			}
		}
		if (valid)
		{
			if( TheGlobalData->m_debugSupplyCenterPlacement )
				DEBUG_LOG(("buildAISupplyCenter -- SUCCESS at (%.2f,%.2f)", newPos.x, newPos.y));
			location = newPos;
		}
		TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.
		location.z = 0; // All build list locations are ground relative.
		// GeneralsX: China/USA skirmish AI (campaign AIs keep their placement): queue the centre square on
		// a dock face (findDockFacePlacement) and keep the spot above, today's, as the entry's fallback. The first time the face
		// spot is refused - an enemy within the safety radius (AISkirmishPlayer::processBaseBuilding), an enemy on it or it is no
		// longer legal (buildStructureWithDozer) - the entry moves to today's spot and is tried there at once. Without that, a
		// face spot at an expansion waited on isLocationSafe instead of being built (ai-placement-2 trace: 88% of the wait of the
		// face expansions that were never built).
		Coord3D faceLocation;
		Real faceAngle = angle;
		Coord2D towardsBase;
		towardsBase.x = -offset.x;
		towardsBase.y = -offset.y;
		// GeneralsX: China only: its gatherers are ground trucks that park in the gap (USA's Chinooks fly,
		// the GLA stash ignores docking bones); every China general's centre (Side China*).
		if (isSkirmishAI() && tTemplate->isKindOf(KINDOF_CASH_GENERATOR) && !tTemplate->isKindOf(KINDOF_IGNORE_DOCKING_BONES)
			&& tTemplate->getDefaultOwningSide().startsWith("China")
			// The main base pro spot: `this` + the stock spot `location` (ground-relative, z = 0 above) / `angle`, the entry's fallback
			// below, so a long face only QUEUED entries block moves them (gxRelocateQueued) clear of that spot
			&& findDockFacePlacement(bestSupplyWarehouse, tTemplate, towardsBase, m_player, &faceLocation, &faceAngle, this, &location, angle)) {
			faceLocation.z = 0;
			m_player->addToPriorityBuildList(thingName, &faceLocation, faceAngle)->setFallbackSpot(location, angle);
		} else {
			m_player->addToPriorityBuildList(thingName, &location, angle);
		}
		if (gxDoorKind(tTemplate, TRUE))	// where a run-time door was queued (the readers' origin split)
		{
			GX_ROT_LOG("p=%d ev=supply_door tmpl=%s x=%d y=%d angle=%d keyed=%d", (Int)m_player->getPlayerIndex(), tTemplate->getName().str(),
				REAL_TO_INT(location.x), REAL_TO_INT(location.y), REAL_TO_INT(angle * 180.0f / PI), (Int)gxKeyedDoor);
		}
		m_curWarehouseID = bestSupplyWarehouse->getID();
	}
}

// ------------------------------------------------------------------------------------------------
/** Calculates the closest construction zone location based on a template. */
// ------------------------------------------------------------------------------------------------
Bool AIPlayer::calcClosestConstructionZoneLocation( const ThingTemplate *constructTemplate, Coord3D *location )
{
	if( !constructTemplate || !location )
	{
		return FALSE;
	}

  Bool success = FALSE;

	// offset back towards the base.
	Coord2D offset;
	offset.x = location->x - m_baseCenter.x;
	offset.y = location->y - m_baseCenter.y;
	offset.normalize();

	Real angle = constructTemplate->getPlacementViewAngle();

 	// validate the the position to build at is valid
	Bool valid=false;
	Coord3D newPos = *location;
	if( gxLegalToBuildOnGround( location, constructTemplate, angle, BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player ) != LBC_OK )
	{
		// Warn.
		AsciiString bldgName = constructTemplate->getName();
		bldgName.concat(" - calcClosestConstructionZoneLocation unable to place.  Attempting to adjust position.");
		TheScriptEngine->AppendDebugMessage( bldgName, false );
		// try to fix.
		Real posOffset;
		// Wiggle it a little :)
		for( posOffset = 0; posOffset < 2 * SUPPLY_CENTER_CLOSE_DIST; posOffset += 2 * PATHFIND_CELL_SIZE_F )
		{
			Real offset = posOffset / 2;
			Real xPos, yPos;
			yPos = location->y - offset;
			for( xPos = location->x - offset; xPos <= location->x + offset; xPos += PATHFIND_CELL_SIZE_F )
			{
				newPos.x = xPos;
				newPos.y = yPos;
				valid = gxLegalToBuildOnGround( &newPos, constructTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
				if( valid )
					break;

				newPos.y = yPos + posOffset;
				valid = gxLegalToBuildOnGround( &newPos, constructTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
			}

			if( valid )
				break;

			xPos = location->x - offset;
			for( yPos = location->y - offset; yPos <= location->y + offset; yPos += PATHFIND_CELL_SIZE_F )
			{
				newPos.x = xPos;
				newPos.y = yPos;
				valid = gxLegalToBuildOnGround( &newPos, constructTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
				if( valid )
					break;

				newPos.x = xPos + posOffset;
				valid = gxLegalToBuildOnGround( &newPos, constructTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
			}

			if( valid )
				break;
		}
	}
	if( valid )
	{
		//We succeeded in calculating the best position.
		location->set( newPos );
    success = TRUE;
	}
	else
	{
		//We failed to calculate a position, so zero out the position.
		location->zero();
    success = FALSE;
	}

	TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.

  return success;

}

// ------------------------------------------------------------------------------------------------
/** Build a specific building nearest specified team. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildSpecificBuildingNearestTeam( const AsciiString &thingName, const Team *team )
{
	const ThingTemplate *tTemplate = TheThingFactory->findTemplate( thingName );

	if( !tTemplate || !team )
	{
		//Assert will already happen in the failed findTemplate call.
		return;
	}

	//From the team's location, find the most valid build location.
	const Coord3D *location = team->getEstimateTeamPosition();
	if( !location )
	{
		return;
	}

	// offset back towards the base.
	Coord2D offset;
	offset.x = location->x - m_baseCenter.x;
	offset.y = location->y - m_baseCenter.y;
	offset.normalize();

	Real angle = tTemplate->getPlacementViewAngle();

 	// validate the the position to build at is valid
	Bool valid=false;
	Coord3D newPos = *location;
	if( gxLegalToBuildOnGround( location, tTemplate, angle, BuildAssistant::NO_OBJECT_OVERLAP, nullptr, m_player ) != LBC_OK )
	{
		// Warn.
		AsciiString bldgName = tTemplate->getName();
		bldgName.concat(" - buildSpecificBuildingNearestTeam unable to place.  Attempting to adjust position.");
		TheScriptEngine->AppendDebugMessage( bldgName, false );
		// try to fix.
		Real posOffset;
		// Wiggle it a little :)
		for( posOffset = 0; posOffset < 2 * SUPPLY_CENTER_CLOSE_DIST; posOffset += 2 * PATHFIND_CELL_SIZE_F )
		{
			Real offset = posOffset / 2;
			Real xPos, yPos;
			yPos = location->y-offset;
			for( xPos = location->x - offset; xPos <= location->x + offset; xPos += PATHFIND_CELL_SIZE_F )
			{
				newPos.x = xPos;
				newPos.y = yPos;
				valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
				if( valid )
					break;

				newPos.y = yPos + posOffset;
				valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
			}

			if( valid )
				break;

			xPos = location->x - offset;
			for( yPos = location->y - offset; yPos <= location->y + offset; yPos += PATHFIND_CELL_SIZE_F )
			{
				newPos.x = xPos;
				newPos.y = yPos;
				valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
				if( valid )
					break;

				newPos.x = xPos + posOffset;
				valid = gxLegalToBuildOnGround( &newPos, tTemplate, angle,
																						 BuildAssistant::CLEAR_PATH |
																						 BuildAssistant::TERRAIN_RESTRICTIONS |
																						 BuildAssistant::NO_OBJECT_OVERLAP,
																						 nullptr, m_player ) == LBC_OK;
			}

			if( valid )
				break;
		}
	}
	if( valid )
	{
		newPos.z = 0; // All build list locations are ground relative.
		m_player->addToPriorityBuildList( thingName, &newPos, angle );
	}

	TheTerrainVisual->removeAllBibs();	// isLocationLegalToBuild adds bib feedback, turn it off.  jba.

}

// ------------------------------------------------------------------------------------------------
/** Find a supply center we haven't built a supply depot near yet. */
// ------------------------------------------------------------------------------------------------
Object *AIPlayer::findSupplyCenter(Int minimumCash, Bool skipUnsafe)
{
	Object *bestSupplyWarehouse = nullptr;
	Real bestDistSqr = 0;
	Object *obj;
	Coord3D enemyCenter;
	enemyCenter.zero();
	Region2D bounds;
	Player *enemy = getAiEnemy();
	if (enemy) {
		getPlayerStructureBounds(&bounds, enemy->getPlayerIndex());
		enemyCenter.set( (bounds.lo.x+bounds.hi.x)*0.5f, (bounds.lo.y+bounds.hi.y)*0.5f, 0);
	}

	do {
		for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
		{
			if (!obj->isKindOf(KINDOF_STRUCTURE)) continue;
			if (!obj->isKindOf(KINDOF_SUPPLY_SOURCE)) continue;
			static const NameKeyType key_warehouseUpdate = NAMEKEY("SupplyWarehouseDockUpdate");
			SupplyWarehouseDockUpdate *warehouseModule = (SupplyWarehouseDockUpdate*)obj->findUpdateModule( key_warehouseUpdate );
			if( warehouseModule )	{
				Int availableCash = warehouseModule->getBoxesStored()*TheGlobalData->m_baseValuePerSupplyBox;
				if (availableCash<minimumCash) continue;
				if( m_player->getRelationship(obj->getTeam()) == ENEMIES ) {
					continue;
				}

				// Make sure we don't have a supply center near it.
				Coord3D center = *obj->getPosition();
				Real radius = SUPPLY_CENTER_CLOSE_DIST + obj->getGeometryInfo().getBoundingCircleRadius();

				PartitionFilterAcceptByKindOf f1(MAKE_KINDOF_MASK(KINDOF_CASH_GENERATOR), KINDOFMASK_NONE);
				PartitionFilterPlayer f2(m_player, true);	// Only find your own units.
				PartitionFilterOnMap filterMapStatus;


				PartitionFilter *filters[] = { &f1, &f2, &filterMapStatus, nullptr };

				Object *supplyCenter = ThePartitionManager->getClosestObject(&center, radius, FROM_BOUNDINGSPHERE_2D, filters);
				if (supplyCenter) {
					// We already have a supply center.
					continue;
				}

				Real dx, dy;
				// from gxSupplyRef(), the stock centre on a keyed skirmish list, else m_baseCenter
				const Coord3D &gxRef = gxSupplyRef();
				dx = obj->getPosition()->x - gxRef.x;
				dy = obj->getPosition()->y - gxRef.y;
				Real distSqr = dx*dx + dy*dy;
				if (enemy) {
					// make sure this isn't closer to our enemy than us.
					dx = obj->getPosition()->x - enemyCenter.x;
					dy = obj->getPosition()->y - enemyCenter.y;
					if (distSqr*0.4>(dx*dx+dy*dy)*0.6f) {
						// closer than 60/40 to enemy than to us, probably not a good candidate for expansion.
						continue;
					}
				}

				// GeneralsX: after every other filter, so the preference order (distance) is unchanged; an unsafe source is passed over, not waited on.
				if (skipUnsafe && (bestSupplyWarehouse==nullptr || bestDistSqr>distSqr) && !isLocationSafe(obj->getPosition(), obj->getTemplate())) continue;

				if (bestSupplyWarehouse==nullptr) {
					bestSupplyWarehouse = obj;
					bestDistSqr = distSqr;
				} else if (bestDistSqr>distSqr) {
					bestSupplyWarehouse = obj;
					bestDistSqr = distSqr;
				}
			}
		}
		if (bestSupplyWarehouse) break;
		minimumCash /= 2;
 	} while (minimumCash > 100);

	return bestSupplyWarehouse;
}

// ------------------------------------------------------------------------------------------------
/**  Build a base defense. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildAIBaseDefense(Bool flank)
{
	//
	AsciiString teamStr = "Error : Solo ai doesn't support buildAIBaseDefense. '";
	TheScriptEngine->AppendDebugMessage(teamStr, false);
}

// ------------------------------------------------------------------------------------------------
/** Build a base defense. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildAIBaseDefenseStructure(const AsciiString &thingName, Bool flank)
{
	//
	AsciiString teamStr = "Error : Solo ai doesn't support buildAIBaseDefenseStructure. '";
	TheScriptEngine->AppendDebugMessage(teamStr, false);
}

// ------------------------------------------------------------------------------------------------
/** Repair a bridge or other structure. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::repairStructure(ObjectID structure)
{
	Object *structureObj = TheGameLogic->findObjectByID(structure);
	if (structureObj==nullptr) return;
	if (structureObj->getBodyModule()==nullptr) return;
	// If the structure is not noticeably damaged, don't bother.
	BodyDamageType structureState = structureObj->getBodyModule()->getDamageState();
	if (structureState==BODY_PRISTINE) {
		return;
	}
	if (structureObj->isKindOf(KINDOF_BRIDGE)) {
		// Locate the correct post to repair.
	}
	Int i;
	for (i=0; i<m_structuresInQueue; i++) {
		if (m_structuresToRepair[i] == structureObj->getID()) {
			DEBUG_LOG(("info - Bridge already queued for repair."));
			return;
		}
	}
	if (m_structuresInQueue>=MAX_STRUCTURES_TO_REPAIR) {
		DEBUG_LOG(("Structure repair queue is full, ignoring repair request. JBA"));
		return;
	}
	m_structuresToRepair[m_structuresInQueue] = structureObj->getID();
	m_structuresInQueue++;
}

// ------------------------------------------------------------------------------------------------
/** select a skillset for the player. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::selectSkillset(Int skillset)
{
	DEBUG_ASSERTCRASH(m_skillsetSelector == INVALID_SKILLSET_SELECTION,
		("Selecting a skill set (%d) after one has already been chosen (%d) means some points have been incorrectly spent.", skillset + 1, m_skillsetSelector + 1));

	m_skillsetSelector = skillset;
}

// ------------------------------------------------------------------------------------------------
/** Do per frame work (if any) repairing bridges. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::updateBridgeRepair()
{
	if (m_structuresInQueue == 0) return;
	// Check once a second.
	m_bridgeTimer--;
	if (m_bridgeTimer>0) return;
	m_bridgeTimer = static_cast<float>(LOGICFRAMES_PER_SECOND);
	Object *bridgeObj=nullptr;
	while (bridgeObj==nullptr && m_structuresInQueue>0) {
		bridgeObj = TheGameLogic->findObjectByID(m_structuresToRepair[0]);
		if (bridgeObj==nullptr) {
			Int i;
			for (i=0; i<m_structuresInQueue-1; i++) {
				m_structuresToRepair[i] = m_structuresToRepair[i+1];
			}
			m_structuresInQueue--;
		}
	}
	if (m_structuresInQueue == 0) return;

	// Got a bridge to repair.
	Object *dozer = nullptr;
	Coord3D bridgePos = *bridgeObj->getPosition();
	BodyDamageType bridgeState = bridgeObj->getBodyModule()->getDamageState();
	if (m_repairDozer==INVALID_ID) {
		m_dozerIsRepairing = false;
		// Need a dozer.
		if (m_dozerQueuedForRepair) {
			return; // we're waiting for one.
		}
		dozer = findDozer(&bridgePos);
		if (dozer) {
			m_repairDozer = dozer->getID();
			m_repairDozerOrigin = *dozer->getPosition();
			dozer->getAI()->aiRepair(bridgeObj, CMD_FROM_AI);
			DEBUG_LOG(("Telling dozer to repair"));
			m_dozerIsRepairing = true;
			return;
		}
		queueDozer();
		m_dozerQueuedForRepair = true;
		return;
	}

	dozer = TheGameLogic->findObjectByID(m_repairDozer);
	if (dozer==nullptr) {
		m_repairDozer=INVALID_ID; // we got killed.
		m_bridgeTimer=0;
		return; // Just try to find a dozer next frame.
	}

	DozerAIInterface* dozerAI = dozer->getAI()->getDozerAIInterface();
	if (dozerAI==nullptr) {
		DEBUG_CRASH(("Unexpected - dozer doesn't have dozer interface."));
		return;
	}
	if (m_dozerIsRepairing) {
		if (!dozerAI->isAnyTaskPending())	{
			// should be done repairing.
			if (bridgeState==BODY_PRISTINE) {
				DEBUG_LOG(("Dozer finished repairing structure."));
				// we're done.
				Int i;
				for (i=0; i<m_structuresInQueue-1; i++) {
					m_structuresToRepair[i] = m_structuresToRepair[i+1];
				}
				m_structuresInQueue--;
				m_dozerIsRepairing = false;
				if (m_structuresInQueue==0) {
					// Go home.
					Coord3D pos = m_baseCenter;
					if (!m_baseCenterSet) {
						pos = m_repairDozerOrigin;
					}
					AIUpdateInterface *ai=dozer->getAI();
					TheAI->pathfinder()->adjustToPossibleDestination(dozer, ai->getLocomotorSet(), &pos);
					dozer->getAI()->aiMoveToPosition(&pos, CMD_FROM_AI);
					return;
				}
			}
		}	else {
			// dozer should be working on the bridge.
			return;
		}
	}
	dozer->getAI()->aiRepair(bridgeObj, CMD_FROM_AI);
	m_dozerIsRepairing = true;
	DEBUG_LOG(("Telling dozer to repair"));
}

// ------------------------------------------------------------------------------------------------
/** Build a specific team.  If priorityBuild, put at front of queue with priority set. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::buildSpecificAITeam( TeamPrototype *teamProto, Bool priorityBuild)
{
	//
	// Create "Team in queue" based on team population
	//
	if (teamProto)
	{
		if (!m_player->getCanBuildUnits()) {
			AsciiString teamStr = "Can't build team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' because build units is disabled.");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
			return;
		}
		if (priorityBuild && teamProto->getIsSingleton()) {
			Team *singletonTeam = TheTeamFactory->findTeam( teamProto->getName() );
			if (singletonTeam && singletonTeam->hasAnyObjects()) {
				AsciiString teamStr = "Unable to build singleton team '";
				teamStr.concat("' because team already exists.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		// Check & make sure we have factories.
		Bool needMoney;
		if (!isPossibleToBuildTeam(teamProto, false, needMoney)) {
			if (needMoney) {
				// Queue it up anyway.
				AsciiString teamStr = "Note - queueing team '";
				teamStr.concat(teamProto->getName());
				teamStr.concat("' but there is enough money.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
			} else {
				// Tech tree doesn't work.
				AsciiString teamStr = "Unable to build team '";
				teamStr.concat(teamProto->getName());
				if (needMoney) {
					teamStr.concat("' - Not enough money.");
				} else {
					teamStr.concat("' because required factories/tech don't exist.");
				}
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		const TCreateUnitsInfo *unitInfo = &teamProto->getTemplateInfo()->m_unitsInfo[0];
		WorkOrder *orders = nullptr;
		Int i;
		// GeneralsX: Hoarding: one order per unit. An order holds ONE factory until its unit is out
		// (WorkOrder::isWaitingToBuild), so "4 Raptors" came out of one airfield one at a time while the others idled. Split,
		// each unit takes the next idle factory; completion, recruiting and minimum-built work per order as before. A unit
		// built in batches (QuantityModifier: one queued Red Guard yields a pair) keeps retail's single order: its twin is
		// recruited by that order's next pass, and split orders sharing one factory would each claim a batch member and
		// re-buy the rest.
		const Bool parallel = isHoarding();
		// Queue up optional units.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].maxUnits-unitInfo[i].minUnits;
				const Int pieces = (parallel && count > 1 && unitsPerOrder(thing) == 1) ? count : 1;
				for (Int piece = 0; count > 0 && piece < pieces; ++piece) {
					WorkOrder *order = newInstance(WorkOrder);
					order->m_thing = thing;
					order->m_factoryID = INVALID_ID;
					order->m_numRequired = pieces > 1 ? 1 : count;
					// prepend to head of list
					order->m_next = orders;
					orders = order;
				}
			}
		}
		// Queue up required units.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].minUnits;
				const Int pieces = (parallel && count > 1 && unitsPerOrder(thing) == 1) ? count : 1;
				for (Int piece = 0; piece < pieces; ++piece) {
					WorkOrder *order = newInstance(WorkOrder);
					order->m_thing = thing;
					order->m_factoryID = INVALID_ID;
					order->m_numRequired = pieces > 1 ? 1 : count;
					order->m_required = true;
					// prepend to head of list
					order->m_next = orders;
					orders = order;
				}
			}
		}
		if (orders)
		{
			/* We have something to build. */
			TeamInQueue *team = newInstance(TeamInQueue);
			if (priorityBuild) {
				// Put in front of queue.
				prependTo_TeamBuildQueue(team);
				team->m_priorityBuild = true;
			}	else {
				// Put in back of queue.
				reverse_TeamBuildQueue();
				prependTo_TeamBuildQueue(team);
				reverse_TeamBuildQueue();
				team->m_priorityBuild = false;
			}
			team->m_workOrders = orders;
			team->m_frameStarted = TheGameLogic->getFrame();
			// create inactive team to place members into as they are built
			// when team is complete, the team is activated
			team->m_team = TheTeamFactory->createInactiveTeam( teamProto->getName() );
			// GeneralsX: A queue entry without a team can never complete, and the build and
			// ready queues dereference m_team everywhere. Drop it instead (tsh batch2 #417, idea of Generals Online
			// a0fc36d70, reimplemented at queue level).
			if (team->m_team == nullptr) {
				removeFrom_TeamBuildQueue(team);
				deleteInstance(team);
				return;
			}
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - starting team build.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
			m_teamDelay = 0;
			if (team->m_team->getPrototype()->getTemplateInfo()->m_executeActions) {
				const Script *script = team->getProductionConditionScript();
				if (script && script->getAction()) {
					TheScriptEngine->friend_executeAction(script->getAction(), team->m_team);
				}
			}
		} else {
			if (TheGlobalData->m_debugAI) {
				AsciiString teamName = teamProto->getName();
				teamName.concat(" - contains 0 buildable units.");
				TheScriptEngine->AppendDebugMessage(teamName, false);
			}
		}

	}
}

// ------------------------------------------------------------------------------------------------
/** Recruit a specific team, within the specific radius of the home position. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::recruitSpecificAITeam(TeamPrototype *teamProto, Real recruitRadius)
{
	if (recruitRadius < 1) recruitRadius = 99999.0f;
	//
	// Create "Team in queue" based on team population
	//
	if (teamProto)
	{
		if (teamProto->getIsSingleton()) {
			Team *singletonTeam = TheTeamFactory->findTeam( teamProto->getName() );
			if (singletonTeam && singletonTeam->hasAnyObjects()) {
				AsciiString teamStr = "Unable to recruit singleton team '";
				teamStr.concat("' because team already exists.");
				TheScriptEngine->AppendDebugMessage(teamStr, false);
				return;
			}
		}
		if (!teamProto->getTemplateInfo()->m_hasHomeLocation && !isSkirmishAI())
		{
			AsciiString teamStr = "Error : team '";
			teamStr.concat(teamProto->getName());
			teamStr.concat("' has no Home Position (or Origin).");
			TheScriptEngine->AppendDebugMessage(teamStr, false);
		}
		// create inactive team to place members into as they are built
		// when team is complete, the team is activated
		Team *theTeam = TheTeamFactory->createInactiveTeam( teamProto->getName() );
		AsciiString teamName = teamProto->getName();
		teamName.concat(" - Recruiting.");
		TheScriptEngine->AppendDebugMessage(teamName, false);
		const TCreateUnitsInfo *unitInfo = &teamProto->getTemplateInfo()->m_unitsInfo[0];
//		WorkOrder *orders = nullptr;
		Int i;
		Int unitsRecruited = 0;
		// Recruit.
		for( i=0; i<teamProto->getTemplateInfo()->m_numUnitsInfo; i++ )
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( unitInfo[i].unitThingName );
			if (thing)
			{
				int count = unitInfo[i].maxUnits;
				while (count>0) {
					Object *unit = theTeam->tryToRecruit(thing, &teamProto->getTemplateInfo()->m_homeLocation, recruitRadius);
					if (unit)
					{
						unitsRecruited++;

						AsciiString teamStr = "Team '";
						teamStr.concat(theTeam->getPrototype()->getName());
						teamStr.concat("' recruits ");
						teamStr.concat(thing->getName());
						teamStr.concat(" from team '");
						teamStr.concat(unit->getTeam()->getPrototype()->getName());
						teamStr.concat("'");
						TheScriptEngine->AppendDebugMessage(teamStr, false);

						unit->setTeam(theTeam);
						gxAirRecruitEndsAirAttack(unit);

						AIUpdateInterface *ai = unit->getAIUpdateInterface();
						if (ai)
						{
#ifdef DEBUG_LOGGING
							Coord3D pos = *unit->getPosition();
							Coord3D to = teamProto->getTemplateInfo()->m_homeLocation;
							DEBUG_LOG(("Moving unit from %f,%f to %f,%f", pos.x, pos.y , to.x, to.y ));
#endif
							ai->aiMoveToPosition( &teamProto->getTemplateInfo()->m_homeLocation, CMD_FROM_AI);
						}
					} else {
						break;
					}
					count--;
				}
			}
		}
		if (unitsRecruited>0)
		{
			/* We have something to build. */
			TeamInQueue *team = newInstance(TeamInQueue);
			// Put in front of queue.
			prependTo_TeamReadyQueue(team);
			team->m_priorityBuild = false;
			team->m_workOrders = nullptr;
			team->m_frameStarted = TheGameLogic->getFrame();
			team->m_team = theTeam;
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Finished recruiting.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}	else {
			//disband.
			if (!theTeam->getPrototype()->getIsSingleton()) {
				deleteInstance(theTeam);
				theTeam = nullptr;
			}
			AsciiString teamName = teamProto->getName();
			teamName.concat(" - Recruited 0 units, disbanding.");
			TheScriptEngine->AppendDebugMessage(teamName, false);
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Train our teams. */
// ------------------------------------------------------------------------------------------------
void AIPlayer::processTeamBuilding()
{
	// select a new team
	if (selectTeamToBuild()) {
		queueUnits();
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void AIPlayer::queueUnits()
{

	queueSupplyTruck();

	// For each member of the current team to build, try to find a faction building to build it.
	//
	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
	{
		TeamInQueue *team = iter.cur();
		// GeneralsX: A recruit search that found nothing is not repeated for another order of the
		// same unit in this team in this pass: Team::tryToRecruit is a pure scan of the whole object list (no RNG, no state),
		// its arguments are the same, and nothing between the two calls can add a candidate - a failed search changes
		// nothing, and a unit recruited for another order joins THIS team, which the search never takes from. A hoarding AI
		// splits a team's units into one order each, which would otherwise repeat the scan per unit.
		const ThingTemplate *noRecruit[8];
		Int numNoRecruit = 0;
		for( WorkOrder *order = team->m_workOrders; order; order = order->m_next )
		{
			Bool searched = false;
			for (Int k = 0; k < numNoRecruit; ++k)
				searched = searched || noRecruit[k] == order->m_thing;
			// check if there is a unit on the map that we can steal (recruit) instead of building
			// @todo: Should this try to alter the home location of the recruiting area to
			// the center of the team, or to the home area of this player?
			Coord3D home = team->m_team->getPrototype()->getTemplateInfo()->m_homeLocation;
			Bool hasHome = false;
			if (team->m_team->getPrototype()->getTemplateInfo()->m_hasHomeLocation) {
				hasHome = true;
			} else {
				hasHome = getBaseCenter(&home);
			}
			while (!searched && order->isWaitingToBuild()) {

				Object *unit = team->m_team->tryToRecruit(order->m_thing, &home, TheAI->getAiData()->m_maxRecruitDistance);
				if (unit)
				{
					order->m_numCompleted++;

					AsciiString teamStr = "Team '";
					teamStr.concat(team->m_team->getPrototype()->getName());
					teamStr.concat("' recruits ");
					teamStr.concat(order->m_thing->getName());
					teamStr.concat(" from team '");
					teamStr.concat(unit->getTeam()->getPrototype()->getName());
					teamStr.concat("'");
					TheScriptEngine->AppendDebugMessage(teamStr, false);

					unit->setTeam(team->m_team);
					gxAirRecruitEndsAirAttack(unit);

					AIUpdateInterface *ai = unit->getAIUpdateInterface();
					if (hasHome) {
						ai->aiMoveToPosition( &home, CMD_FROM_AI);
					} else {
						ai->aiIdle(CMD_FROM_AI); // stop, you've been recruited.
					}
				}	else {
					if (numNoRecruit < 8)
						noRecruit[numNoRecruit++] = order->m_thing;
					break;
				}
			}
			if (order->isWaitingToBuild())
			{

				// start the creation of a new unit
				startTraining( order, team->m_priorityBuild, team->m_team->getName());
			}
			else
			{
				// we are under construction, verify our factory still exists
				order->validateFactory(m_player);
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it's time to build another base building.
 */
void AIPlayer::doBaseBuilding()
{
	if (m_player->getCanBuildBase()) {
		// See if we are ready to start trying a structure.
		if (!m_readyToBuildStructure) {
			m_structureTimer--;
			if (m_structureTimer<=0) {
				m_readyToBuildStructure = true;
				m_buildDelay = 0;
			}
		}
		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_buildDelay--;
		if (m_buildDelay<1) {
			if (m_readyToBuildStructure) {
				processBaseBuilding();
			}
			if (m_buildDelay<1) {	// processBaseBuilding may reset m_buildDelay.
				m_buildDelay = 2*LOGICFRAMES_PER_SECOND; // check again in 2 seconds.
			}
			// Note that this timer gets shortcut when a building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any ready teams have finished moving to the rally point.
 */
void AIPlayer::checkReadyTeams()
{
	// See if any ready teams are gathered at their rally point
	{	// needed to scope iter.  silly ms c++.
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamReadyQueue(); !iter.done(); iter.advance())
		{
			TeamInQueue *team = iter.cur();
			// If 60 seconds passed, start anyway.
			Bool timeExpired = team->m_frameStarted+60*LOGICFRAMES_PER_SECOND < TheGameLogic->getFrame();
			Bool allIdle=TRUE;
			Bool anyIdle = FALSE;
			if (team->m_reinforcement) {
				Object *obj = TheGameLogic->findObjectByID(team->m_reinforcementID);
				if (obj && obj->getAIUpdateInterface()) {
					allIdle = obj->getAIUpdateInterface()->isIdle();
					anyIdle = allIdle;
				}
			} else {
				allIdle = team->m_team->isIdle();
				for (DLINK_ITERATOR<Object> iter = team->m_team->iterate_TeamMemberList(); !iter.done(); iter.advance()) {
					Object *obj = iter.cur();
					if (obj->getAI() && obj->getAI()->isIdle()) {
						anyIdle = true;
					}
				}
			}
			if (anyIdle && team->m_team->getPrototype()->getTemplateInfo()->m_executeActions) {
				const Script *script = team->getProductionConditionScript();
				if (script && script->getAction()) {
					// we have a start action.  So don't wait for allIdle as the team may be guarding.
					allIdle = true;
				}
			}
			if (timeExpired) allIdle = true;
			if (allIdle) {
				if (!team->m_sentToStartLocation) {
					team->m_sentToStartLocation = true;
					/*
					if (team->m_team->getPrototype()->getTemplateInfo()->m_hasHomeLocation &&
							!team->m_reinforcement) {
 						AIGroupPtr theGroup = TheAI->createGroup();
						if (theGroup) {
							team->m_team->getTeamAsAIGroup(theGroup.Peek());
							Coord3D destination = team->m_team->getPrototype()->getTemplateInfo()->m_homeLocation;
							theGroup->groupTightenToPosition( &destination, false, CMD_FROM_AI );
							team->m_frameStarted = TheGameLogic->getFrame();
							continue;
						}
					}
					*/
				}
				// Start the team up.
				removeFrom_TeamReadyQueue(team);
				if (team->m_reinforcement) {
					Object *obj = TheGameLogic->findObjectByID(team->m_reinforcementID);
					if (obj&&obj->getAIUpdateInterface()) {
						obj->getAIUpdateInterface()->joinTeam();
					}
				} else {
					// mark our completed team as "active" - this will invoke any OnCreate scripts, etc.
					team->m_team->setActive();
					if (isSkirmishAI()) {
						TheScriptEngine->clearTeamFlags();
					}
					if (TheGlobalData->m_debugAI) {
						AsciiString teamName = team->m_team->getPrototype()->getName();
						teamName.concat(" - team activated.");
						TheScriptEngine->AppendDebugMessage(teamName, false);
					}
				}
				deleteInstance(team);
				iter = iterate_TeamReadyQueue();
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if any queued teams have finished building, or have run out of time.
 */
void AIPlayer::checkQueuedTeams()
{
	// See if any teams are expired.
	{	// needed to scope iter.  silly ms c++.
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
		{
			TeamInQueue *team = iter.cur();
			if (team && team->isBuildTimeExpired())	{
				if (team->isMinimumBuilt())
				{
					if (team->areBuildsComplete()) {
						// Move to ready queue
						removeFrom_TeamBuildQueue(team);
						prependTo_TeamReadyQueue(team);
					}	else {
						continue;
					}
				}	else {
					// Disband.
					removeFrom_TeamBuildQueue(team);
					team->disband();
					deleteInstance(team);
					if (isSkirmishAI()) {
						TheScriptEngine->clearTeamFlags();
					}
				}
				iter = iterate_TeamBuildQueue();
			}
		}
	}

	// See if any teams are ready.
	{	// needed to scope iter.  silly ms c++.
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
		{
			TeamInQueue *team = iter.cur();
			if (team && team->isAllBuilt())
			{
				// Move to ready queue
				removeFrom_TeamBuildQueue(team);
				prependTo_TeamReadyQueue(team);
				iter = iterate_TeamBuildQueue();
				continue;
			}
			Bool anyIdle = false;
			for (DLINK_ITERATOR<Object> iter = team->m_team->iterate_TeamMemberList(); !iter.done(); iter.advance()) {
				Object *obj = iter.cur();
				if (obj && obj->getAI() && obj->getAI()->isIdle()) {
					anyIdle = true;
				}
			}
			if (anyIdle) {
				if (team->m_team->getPrototype()->getTemplateInfo()->m_executeActions) {
					const Script *script = team->getProductionConditionScript();
					if (script) {
						TheScriptEngine->friend_executeAction(script->getAction(), team->m_team);
					}
				}
			}
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it is time to start another ai team building.
 */
void AIPlayer::doTeamBuilding()
{
	// See if any teams are expired.
	if (m_player->getCanBuildUnits()) {
		// See if we are ready to start trying a team.
		if (!m_readyToBuildTeam) {
			m_teamTimer--;
			if (m_teamTimer<=0) {
				m_readyToBuildTeam = true;
				m_teamDelay = 0;
			}
		}

		// This timer is to keep from banging on the logic each frame.  If something interesting
		// happens, like a building is added or a unit finished, the timers are shortcut.
		m_teamDelay--;
		if (m_teamDelay<1) {
			queueUnits(); // update the queues.
			if (m_readyToBuildTeam) {
				processTeamBuilding();
			}
			m_teamDelay = 5*LOGICFRAMES_PER_SECOND; // check again in 5 seconds.
			// Note that this timer gets shortcut when a unit or building is completed.
		}
	}
}

//----------------------------------------------------------------------------------------------------------
/**
 * See if it is time to start another upgrade or skill building.
 */
void AIPlayer::doUpgradesAndSkills()
{
	if (TheGameLogic->getFrame() < 2) {
		// can't do updates on the first few frames
		return;
	}

	Bool checkScience = m_player->getSciencePurchasePoints()>0;
	if (!checkScience) {
		return;
	}
	const AISideInfo *sideInfo = TheAI->getAiData()->m_sideInfo;
	while (sideInfo) {
		if (sideInfo->m_side == m_player->getSide()) {
			break;
		}
		sideInfo = sideInfo->m_next;
	}
	if (sideInfo == nullptr) return;

	if (m_skillsetSelector == INVALID_SKILLSET_SELECTION) {
		Int limit = 0;
		// Pick randomly among the skillsets that have skills.
		// Designers sometimes only define skillset 1 & 2, or some such.  jba.
		if (sideInfo->m_skillSet2.m_numSkills>0) {
			limit = 1;
			if (sideInfo->m_skillSet3.m_numSkills>0) {
				limit = 2;
				if (sideInfo->m_skillSet4.m_numSkills>0) {
					limit = 3;
					if (sideInfo->m_skillSet5.m_numSkills>0) {
						limit = 4;
					}
				}
			}
		}
		if (isSkirmishAI()) {
			m_skillsetSelector = GameLogicRandomValue(0, limit);
		} else {
			m_skillsetSelector = 0; // Non-skirmish default to 0.  jba.
		}
	}

	// SKILLS
	if (m_player->getSciencePurchasePoints()>0) {
		const TSkillSet *skillset;
		switch(m_skillsetSelector) {
			default:
			case 0: skillset = &sideInfo->m_skillSet1; break;
			case 1: skillset = &sideInfo->m_skillSet2; break;
			case 2: skillset = &sideInfo->m_skillSet3; break;
			case 3: skillset = &sideInfo->m_skillSet4; break;
			case 4: skillset = &sideInfo->m_skillSet5; break;
		}
		Int i;
		for (i=0; i<skillset->m_numSkills; i++) {
			ScienceType science = skillset->m_skills[i];
			if (m_player->isCapableOfPurchasingScience(science)) {
				if (m_player->attemptToPurchaseScience(science)) {
						AsciiString msg = TheNameKeyGenerator->keyToName(m_player->getPlayerNameKey());
						msg.concat(" purchases from SkillSet");
						msg.concat('1'+m_skillsetSelector);
						msg.concat(' ');
						msg.concat(TheScienceStore->getInternalNameForScience(science));
						msg.concat(".");
						TheScriptEngine->AppendDebugMessage( msg, false);
				}
			}
		}
	}


}

//----------------------------------------------------------------------------------------------------------
/**
 * Perform computer-controlled player AI
 */
//DECLARE_PERF_TIMER(AIPlayer_update)
void AIPlayer::update()
{
	//USE_PERF_TIMER(AIPlayer_update)

	doBaseBuilding();		// See if it's time to build another building.

	checkReadyTeams(); // See if any teams are ready to start.

	checkQueuedTeams(); // See if any teams are complete.

	doTeamBuilding(); // See if it's time to start another team.

	doUpgradesAndSkills(); // See if it's time to build an upgrade or buy a skill.

	updateBridgeRepair(); // Handle any bridge repairs.

}

//----------------------------------------------------------------------------------------------------------
/**
 * Find any things that build stuff & add them to the build list.  Then build any initially built
 * buildings.
 */
void AIPlayer::newMap()
{
	BuildListInfo *info = m_player->getBuildList();
	// Add any factories placed to the build list.
	Object *obj;
	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{

		Player *owner = obj->getControllingPlayer();
		if (owner==m_player) {
			// See if it's a factory.
			ProductionUpdateInterface *pu = obj->getProductionUpdateInterface();
			// If it doesn't produce, continue.
			if (!pu) continue;
			m_player->addToBuildList(obj);
		}

	}
	computeCenterAndRadiusOfBase(&m_baseCenter, &m_baseRadius);

	// Build any with the initially built flag.
	for( /* nothing */; info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			DEBUG_LOG(("*** ERROR - Build list building '%s' doesn't exist.", name.str()));
			continue;
		}
		if (info->isInitiallyBuilt()) {
			buildStructureNow(bldgPlan, info);
		} else {
			info->incrementNumRebuilds(); // the initial build in the normal build list consumes a rebuild, so add one.
		}
	}
}

// ------------------------------------------------------------------------------------------------
/** Find the center of the base and the radius of buildings.  */
// ------------------------------------------------------------------------------------------------
void AIPlayer::computeCenterAndRadiusOfBase(Coord3D *center, Real *radius)
{
	//
	BuildListInfo *info;
	Coord2D totalPos;
	totalPos.x = 0;
	totalPos.y = 0;
	Int numBldg=0;
	for( info = m_player->getBuildList(); info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			continue;
		}
		Coord3D pos = *info->getLocation();
		totalPos.x += pos.x;
		totalPos.y += pos.y;
		numBldg++;
	}
	if (numBldg>0) {
		totalPos.x /= numBldg;
		totalPos.y /= numBldg;
	}

	m_baseCenterSet = numBldg>0;
	center->x = totalPos.x;
	center->y = totalPos.y;

	Real maxRadSqr = 0;
	//
	for( info = m_player->getBuildList(); info; info = info->getNext() )
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;
		const ThingTemplate *bldgPlan = TheThingFactory->findTemplate( name );
		if (!bldgPlan) {
			continue;
		}
		Coord3D pos = *info->getLocation();
		Real dx = pos.x-center->x;
		Real dy = pos.y-center->y;
		if (dx<0) dx = -dx;
		if (dy<0) dy = -dy;
		Real bldgRadius = bldgPlan->getTemplateGeometryInfo().getBoundingCircleRadius()*0.4f;
		dx += bldgRadius;
		dy += bldgRadius;
		Real radSqr = dx*dx+dy*dy;
		if (radSqr>maxRadSqr) maxRadSqr=radSqr;
	}
	*radius = sqrt(maxRadSqr);
}

//----------------------------------------------------------------------------------------------------------
/**
 * Checks to see if we're building a dozer.
 */
Bool AIPlayer::dozerInQueue()
{
	{	// needed to scope iter.  silly ms c++.
		for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance())
		{
			TeamInQueue *team = iter.cur();
			if (team && team->includesADozer() )
			{
				return true; // dozer is building already.
			}
		}
	}
	return false;
}

//----------------------------------------------------------------------------------------------------------
/**
 * Queues up a dozer.
 */
void AIPlayer::queueDozer()
{

	if (dozerInQueue()) return;
	// Find a factory that can build a dozer.

	Bool canBuildUnits = m_player->getCanBuildUnits();
	// If we need a dozer, turn on unit building for a moment.
	m_player->setCanBuildUnits(true);
	const ThingTemplate *tTemplate = TheThingFactory->firstTemplate();
	while (tTemplate) {
		if (tTemplate->isKindOf(KINDOF_DOZER)) {
			Object *factory = findFactory(tTemplate, true);
			if (factory) {
				// we can build one.
				WorkOrder *order = newInstance(WorkOrder);
				order->m_thing = tTemplate;
				order->m_factoryID = INVALID_ID;
				order->m_numRequired = 1;
				order->m_required = true;
				order->m_isResourceGatherer = FALSE;
				// prepend to head of list
				order->m_next = nullptr;
				TeamInQueue *team = newInstance(TeamInQueue);
				// Put in front of queue.
				prependTo_TeamBuildQueue(team);
				team->m_priorityBuild = true;
				team->m_workOrders = order;
				team->m_frameStarted = TheGameLogic->getFrame();
				// Stick it on the default team
				team->m_team = m_player->getDefaultTeam();
				AsciiString teamName = "DOZER - building one at the ";
				teamName.concat(factory->getTemplate()->getName());
				TheScriptEngine->AppendDebugMessage(teamName, false);
				m_teamDelay = 0;
				startTraining( order, team->m_priorityBuild, team->m_team->getName());
				break;
			}
		}
		tTemplate = tTemplate->friend_getNextTemplate();
	}
	// restore canbuildunits.
	m_player->setCanBuildUnits(canBuildUnits);
}


//-------------------------------------------------------------------------------------------------
/** Difficulty level for this player */
//-------------------------------------------------------------------------------------------------
enum GameDifficulty AIPlayer::getAIDifficulty() const
{
	return m_difficulty;
}


//----------------------------------------------------------------------------------------------------------
/**
 * Finds a dozer that isn't building or collecting resources.
 */
Object * AIPlayer::findDozer( const Coord3D *pos )
{
	// Add any factories placed to the build list.
	Object *obj;
	Object *dozer = nullptr;
	Bool needDozer = true;
	Object *closestDozer=nullptr;
	Real closestDistSqr = 0;

	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{

		Player *owner = obj->getControllingPlayer();
		if (owner==m_player) {
			// See if it's a dozer.
			if (obj->isKindOf(KINDOF_DOZER)) {

				AIUpdateInterface *ai = obj->getAIUpdateInterface();
				if (ai==nullptr) {
					continue;
				}


				DozerAIInterface* dozerAI = ai->getDozerAIInterface();
				if (dozerAI) {
					// Since workers can be dozers, hmmm....
					SupplyTruckAIInterface* supplyTruckAI = ai->getSupplyTruckAIInterface();
					if( !dozerAI->isAnyTaskPending() && supplyTruckAI ) {
						// If it is gathering supplies, don't steal it.
						if (supplyTruckAI->isCurrentlyFerryingSupplies() || supplyTruckAI->isForcedIntoWantingState())
						{
							continue;
						}
					}
					if (obj->getID() == m_repairDozer) {
						continue; // don't steal the repair dozer.
					}
					needDozer = false; // dozer exists, may be busy.
					if (dozerAI->isTaskPending(DOZER_TASK_BUILD)) {
						continue; // already building.
					}
					if (!dozerAI->isAnyTaskPending()) {
						dozer = obj; // prefer an idle dozer
					}
					if (dozer==nullptr) {
						dozer = obj; // but we'll take one doing stuff.
					}
					if (dozer && !dozerAI->isAnyTaskPending()) {
						// Got a good one, track closest.
						Real distSqr;
						Real dx, dy;
						dx = pos->x - dozer->getPosition()->x;
						dy = pos->y - dozer->getPosition()->y;
						distSqr = dx*dx+dy*dy;
						if (closestDozer == nullptr) {
							closestDozer = dozer;
							closestDistSqr = distSqr;
						} else if (distSqr < closestDistSqr) {
							closestDozer = dozer;
							closestDistSqr = distSqr;
						}
					}
				}
			}
		}

	}
	if (needDozer) {
		queueDozer();
	}
	if (closestDozer) return closestDozer;
	return dozer;
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void AIPlayer::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version
	* 2: added m_teamSeconds delay.
	* 3: Added m_curWarehouseID.
	* 1: Reset back to 1 with major save file changes.
*/
// ------------------------------------------------------------------------------------------------
void AIPlayer::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// team build queue count
	UnsignedShort teamBuildQueueCount = 0;
	for( DLINK_ITERATOR< TeamInQueue > teamInQueueIt = iterate_TeamBuildQueue();
			 teamInQueueIt.done() == FALSE;
			 teamInQueueIt.advance() )
		teamBuildQueueCount++;
	xfer->xferUnsignedShort( &teamBuildQueueCount );

	// team build queue data
	TeamInQueue *teamInQueue;
	if( xfer->getXferMode() == XFER_SAVE )
	{

		for( DLINK_ITERATOR< TeamInQueue > teamInQueueIt = iterate_TeamBuildQueue();
				 teamInQueueIt.done() == FALSE;
				 teamInQueueIt.advance() )
		{

			// get element data
			teamInQueue = teamInQueueIt.cur();

			// xfer it
			xfer->xferSnapshot( teamInQueue );

		}

	}
	else
	{

		// sanity, the list must be empty
		if( getFirstItemIn_TeamBuildQueue() != nullptr )
		{

			DEBUG_CRASH(( "AIPlayer::xfer - TeamBuildQueue head is not null, you should delete it or something before loading a new list" ));
			throw SC_INVALID_DATA;

		}

		// ready all data
		for( UnsignedShort i = 0; i < teamBuildQueueCount; ++i )
		{

			// allocate new team in queue instance
			teamInQueue = newInstance(TeamInQueue);

			// attach to end of list
			prependTo_TeamBuildQueue( teamInQueue );

			// xfer data
			xfer->xferSnapshot( teamInQueue );

		}

		// the list was loaded in reverse order, reverse the list so it's in the same order as before
		reverse_TeamBuildQueue();

	}

	// team ready queue count
	UnsignedShort teamReadyQueueCount = 0;
	for( DLINK_ITERATOR< TeamInQueue > teamReadyQueueIt = iterate_TeamReadyQueue();
			 teamReadyQueueIt.done() == FALSE;
			 teamReadyQueueIt.advance() )
		teamReadyQueueCount++;
	xfer->xferUnsignedShort( &teamReadyQueueCount );

	// team Ready queue data
	TeamInQueue *teamReadyQueue;
	if( xfer->getXferMode() == XFER_SAVE )
	{

		for( DLINK_ITERATOR< TeamInQueue > teamReadyQueueIt = iterate_TeamReadyQueue();
				 teamReadyQueueIt.done() == FALSE;
				 teamReadyQueueIt.advance() )
		{

			// get element
			teamReadyQueue = teamReadyQueueIt.cur();

			// xfer data
			xfer->xferSnapshot( teamReadyQueue );

		}

	}
	else
	{

		// sanity, the list must be empty
		if( getFirstItemIn_TeamReadyQueue() != nullptr )
		{

			DEBUG_CRASH(( "AIPlayer::xfer - TeamReadyQueue head is not null, you should delete it or something before loading a new list" ));
			throw SC_INVALID_DATA;

		}

		// read all data
		for( UnsignedShort i = 0; i < teamReadyQueueCount; ++i )
		{

			// allocate new team in queue instance
			teamInQueue = newInstance(TeamInQueue);

			// attach to end of list
			prependTo_TeamReadyQueue( teamInQueue );

			// xfer data
			xfer->xferSnapshot( teamInQueue );

		}

		// reverse the list since it was loaded in reverse order due to the prepend
		reverse_TeamReadyQueue();

	}

	// xfer player index ... this is really just for sanity
	PlayerIndex playerIndex = m_player->getPlayerIndex();
	xfer->xferUser( &playerIndex, sizeof( PlayerIndex ) );
	if( playerIndex != m_player->getPlayerIndex() )
	{

		DEBUG_CRASH(( "AIPlayer::xfer - player index mismatch" ));
		throw SC_INVALID_DATA;

	}

	// xfer the rest of the ai player data (it's pretty straight forward)
	xfer->xferBool( &m_readyToBuildTeam );
	xfer->xferBool( &m_readyToBuildStructure );
	xfer->xferInt( &m_teamTimer );
	xfer->xferInt( &m_structureTimer );

	xfer->xferInt( &m_buildDelay );
	xfer->xferInt( &m_teamDelay );

	xfer->xferInt(&m_teamSeconds);
 	xfer->xferObjectID(&m_curWarehouseID);

	xfer->xferInt( &m_frameLastBuildingBuilt );

	xfer->xferUser( &m_difficulty, sizeof( GameDifficulty ) );
	xfer->xferInt( &m_skillsetSelector );

	xfer->xferCoord3D( &m_baseCenter );
	xfer->xferBool( &m_baseCenterSet );
	xfer->xferReal( &m_baseRadius );

	xfer->xferUser( m_structuresToRepair, sizeof( ObjectID ) * MAX_STRUCTURES_TO_REPAIR );
	xfer->xferObjectID( &m_repairDozer );
	xfer->xferInt( &m_structuresInQueue );
	xfer->xferBool( &m_dozerQueuedForRepair );
	xfer->xferBool( &m_dozerIsRepairing );
	xfer->xferInt( &m_bridgeTimer );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void AIPlayer::loadPostProcess()
{
	// GeneralsX: A queued team that no longer exists loads back as a null m_team (the
	// TeamInQueue xfer looks it up by ID); drop those entries like a destroyed team's (tsh batch2 #417).
	AIPlayer::aiPreTeamDestroy( nullptr );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
TeamInQueue::~TeamInQueue()
{
	WorkOrder *order, *next;

	for( order = m_workOrders; order; order = next )
	{
		next = order->m_next;
		deleteInstance(order);
	}
	// If we have a team, activate it.  If it is empty, Team.cpp will remove empty active teams.
	if (m_team) m_team->setActive();
	m_workOrders = nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool TeamInQueue::isAllBuilt()
{
	WorkOrder *order;

	Bool stillBuilding = false;
	for( order = m_workOrders; order; order = order->m_next )
	{
		if (order->m_numRequired>order->m_numCompleted)
		{
			stillBuilding = true;
		}
	}
	return !stillBuilding;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool TeamInQueue::isBuildTimeExpired()
{
	if (m_team->getPrototype()->getTemplateInfo()->m_initialIdleFrames<1) {
		return false; // Unlimited time.
	}
	if (TheGameLogic->getFrame() > m_frameStarted + m_team->getPrototype()->getTemplateInfo()->m_initialIdleFrames) {
		return true;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool TeamInQueue::isMinimumBuilt()
{
	WorkOrder *order;

	for( order = m_workOrders; order; order = order->m_next )
	{
		Int count = order->m_numCompleted;
		if (order->m_factoryID != INVALID_ID) {
			count++; // we have one building.
		}
		if (order->m_numRequired>count)
		{
			if (order->m_required) {
				return false; // required units not built.
			}
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool TeamInQueue::includesADozer()
{
	WorkOrder *order;

	for( order = m_workOrders; order; order = order->m_next )
	{
		// GLA dozers (workers) are also resource gatherers, so make sure it isn't a gatherer. jba.
		if (order->m_thing->isKindOf(KINDOF_DOZER) && !order->m_isResourceGatherer) {
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool TeamInQueue::areBuildsComplete()
{
	WorkOrder *order;

	for( order = m_workOrders; order; order = order->m_next )
	{
		if (order->m_factoryID != INVALID_ID) {
			return false; // we have one building.
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void TeamInQueue::disband()
{
	Team *newTeam = m_team->getPrototype()->getControllingPlayer()->getDefaultTeam();
	AsciiString teamName = m_team->getPrototype()->getName();
	teamName.concat(" - team disbanded, build time expired.");
	TheScriptEngine->AppendDebugMessage(teamName, false);
	if (m_team != newTeam) {
		m_team->transferUnitsTo(newTeam);
		if (!m_team->getPrototype()->getIsSingleton()) {
			deleteInstance(m_team);
		}
		m_team = nullptr;
	}
}

// -------------------------------------------------------------------------------------------------
// GeneralsX: Same result as TheScriptEngine->findScriptByName(name): the first script with that
// name in side/list/group order. Cached per TeamInQueue and re-resolved whenever the condition name differs.
const Script *TeamInQueue::getProductionConditionScript()
{
	const AsciiString& name = m_team->getPrototype()->getTemplateInfo()->m_productionCondition;
	if (!m_productionConditionScriptValid || m_productionConditionScriptName != name)
	{
		m_productionConditionScript = TheScriptEngine->findScriptByName(name);
		m_productionConditionScriptName = name;
		m_productionConditionScriptValid = TRUE;
	}
	return m_productionConditionScript;
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void TeamInQueue::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void TeamInQueue::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// xfer work order count
	UnsignedShort workOrderCount = 0;
	WorkOrder *workOrder;
	for( workOrder = m_workOrders; workOrder; workOrder = workOrder->m_next )
		workOrderCount++;
	xfer->xferUnsignedShort( &workOrderCount );

	// xfer work orders
	if( xfer->getXferMode() == XFER_SAVE )
	{

		// xfer each work order
		for( workOrder = m_workOrders; workOrder; workOrder = workOrder->m_next )
		{

			// xfer work order data
			xfer->xferSnapshot( workOrder );

		}

	}
	else
	{

		// sanity
		if( m_workOrders != nullptr )
		{

			DEBUG_CRASH(( "TeamInQueue::xfer - m_workOrders should be null but isn't.  Perhaps you should blow it away before loading" ));
			throw SC_INVALID_DATA;

		}

		// load all work orders
		for( UnsignedShort i = 0; i < workOrderCount; ++i )
		{

			// allocate new work order
			workOrder = newInstance(WorkOrder);

			// attach to list at the end
			workOrder->m_next = nullptr;
			if( m_workOrders == nullptr )
				m_workOrders = workOrder;
			else
			{
				WorkOrder *last = m_workOrders;

				while( last->m_next != nullptr )
					last = last->m_next;

				last->m_next = workOrder;

			}

			// load work order data
			xfer->xferSnapshot( workOrder );

		}

	}

	// xfer the rest of the team in queue data
	xfer->xferBool( &m_priorityBuild );
	TeamID teamID = m_team ? m_team->getID() : TEAM_ID_INVALID;
	xfer->xferUser( &teamID, sizeof( TeamID ) );
	if( xfer->getXferMode() == XFER_LOAD )
		m_team = TheTeamFactory->findTeamByID( teamID );
	xfer->xferInt( &m_frameStarted );
	xfer->xferBool( &m_sentToStartLocation );
	xfer->xferBool( &m_stopQueueing );
	xfer->xferBool( &m_reinforcement );
	xfer->xferObjectID( &m_reinforcementID );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void TeamInQueue::loadPostProcess()
{

}

///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
WorkOrder::~WorkOrder()
{

}

// ------------------------------------------------------------------------------------------------
/** Verify factoryID still refers to an active object */
// ------------------------------------------------------------------------------------------------
void WorkOrder::validateFactory( Player *thisPlayer )
{

	if (m_factoryID == INVALID_ID)
		return;
	Object *factory = TheGameLogic->findObjectByID( m_factoryID );
	if ( factory == nullptr) {
		m_factoryID = INVALID_ID;
		return;
	}
	if (factory->getControllingPlayer()!=thisPlayer) {
		m_factoryID = INVALID_ID;
	}

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void WorkOrder::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void WorkOrder::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// thing template
	AsciiString thingTemplateName = m_thing ? m_thing->getName() : AsciiString::TheEmptyString;
	xfer->xferAsciiString( &thingTemplateName );
	if( xfer->getXferMode() == XFER_LOAD )
		m_thing = TheThingFactory->findTemplate( thingTemplateName );

	// factory id
	xfer->xferObjectID( &m_factoryID );

	// num completed
	xfer->xferInt( &m_numCompleted );

	// num required
	xfer->xferInt( &m_numRequired );

	// is required
	xfer->xferBool( &m_required );

	// is resource gatherer
	xfer->xferBool( &m_isResourceGatherer );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void WorkOrder::loadPostProcess()
{

}


//----------------------------------------------------------------------------------------------------------
/**
 * Get the bounds for a player's structure.
 */
void AIPlayer::getPlayerStructureBounds(Region2D *bounds, Int playerNdx, Bool conservative)
{
#if RTS_GENERALS && RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @info In the Generals code, lo.y used to be uninitialized for both Region2D variables in this function.
#endif
	bounds->zero();

	Player* pPlayer = ThePlayerList->getNthPlayer(playerNdx);
	if (pPlayer == nullptr)
		return;

	Bool firstObject = true;
	Bool firstStructure = true;
	Region2D objBounds;
	objBounds.zero();

	const Player::PlayerTeamList& list = *pPlayer->getPlayerTeams();
	for (Player::PlayerTeamList::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		for (DLINK_ITERATOR<Team> iter = (*it)->iterate_TeamInstanceList(); !iter.done(); iter.advance())
		{
			Team *team = iter.cur();
			if (!team)
				continue;
			for (DLINK_ITERATOR<Object> iter = team->iterate_TeamMemberList(); !iter.done(); iter.advance())
			{
				Object *pObj = iter.cur();
				if (!pObj)
					continue;
				if( pObj->isKindOf(KINDOF_STRUCTURE) )
				{
					if( conservative && pObj->isKindOf( KINDOF_CONSERVATIVE_BUILDING ) )
					{
						//Kris - Aug 14, 2003
						//Conservative buildings (captured tech buildings, sneak attack buildings are rejected).
						//This was added so base boundaries aren't potentially most of the map. Because sneak
						//attack uses an inverse cost calculation by avoiding defended areas, we need to keep
						//things close to the enemy base.
						continue;
					}

					Coord3D pos = *pObj->getPosition();
					if (firstObject) {
						objBounds.lo.x = objBounds.hi.x = pos.x;
						objBounds.lo.y = objBounds.hi.y = pos.y;
						firstObject = false;
					}
					else
					{
						objBounds.uniteWith(pos.asCoord2D());
					}
					if (firstStructure)
					{
						bounds->lo.x = bounds->hi.x = pos.x;
						bounds->lo.y = bounds->hi.y = pos.y;
						firstStructure = false;
					}
					else
					{
						bounds->uniteWith(pos.asCoord2D());
					}
				}
			}
		}
	}
	if (!firstStructure) {
		// Player had no structures, so use unit bounds.
		*bounds = objBounds;
	}
}
