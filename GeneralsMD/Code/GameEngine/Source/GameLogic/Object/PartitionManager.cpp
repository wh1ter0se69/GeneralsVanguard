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

// FILE: PartitionManager.cpp ////////////////////////////////////////////////////
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
// File name: PartitionManager.cpp
//
// Created:   Steven Johnson, September 2001
//
// Desc:      Partition management, this system will allow us to partition the
//					  objects in space, iterate objects in specified volumes,
//					  regions, by types and other properties.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>

#include "Common/ActionManager.h"
#include "Common/DiscreteCircle.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GameUtility.h"
#include "Common/MessageStream.h"
#include "Common/NameKeyGenerator.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Radar.h"
#include "Common/ThingFactory.h"	// for bullet type hack
#include "Common/ThingTemplate.h"
#include "Common/Xfer.h"

#include "GameLogic/AIPathfind.h"
#include "GameLogic/Module/RailroadGuideAIUpdate.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/CollideModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/Squad.h"
#include "GameLogic/GhostObject.h"

#include "GameClient/Line2D.h"
#include "GameClient/ControlBar.h"

#ifdef RTS_DEBUG
//#include "GameClient/InGameUI.h"	// for debugHints
#endif

#ifdef PM_CACHE_TERRAIN_HEIGHT
#include "Common/MapObject.h"
#endif

#ifdef DUMP_PERF_STATS
	long s_countInClosestObjects = 0;
	long s_countInClosestObjectsThisFrame = 0;
	Int64 s_timeInClosestObjects = 0;
	Int64 s_timeInClosestObjectsThisFrame = 0;
	UnsignedInt s_gcoPerfFrame = 0xffffffff;
#endif


extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);

const Real HUGE_DIST_SQR = (HUGE_DIST*HUGE_DIST);

#define DISABLE_INVALID_PREVENTION	//Steven, I had to turn this off because it was causing problem with map border resizing (USA04). -MW

//------------------------------------------------------------------------------ Performance Timers
//#include "Common/PerfMetrics.h"
//#include "Common/PerfTimer.h"

//-------------------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//         Defines
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
static PartitionContactList* TheContactList = nullptr;

//-----------------------------------------------------------------------------
//         Local Types
//-----------------------------------------------------------------------------
struct ThreatValueParms
{
	Int playerIndex;
	Real xCenter;
	Real yCenter;
	Real radius;
	UnsignedInt threatOrValue;
};

struct CollideInfo
{
	Coord3D position;
	GeometryInfo geom;
	Real angle;

	CollideInfo(const Coord3D* p, const GeometryInfo& g, Real a) : position(*p), geom(g), angle(a) { }
};

struct CellValueProcParms
{
	Int valueRequired;
	Bool greaterThan;
	ValueOrThreat valueType;
	PlayerMaskType allPlayersMask[MAX_PLAYER_COUNT];
	PlayerMaskType allowedPlayersMasks;
};

static int cellValueProc(PartitionCell* cell, void* userData);

/*
	Notes:
	-- collideLoc and collideNormal will only be filled in if result is true;
			they are undefined if result is false.
	-- either collideLoc or collideNormal may be null if you don't need 'em.
	-- collideLoc is not guaranteed to lie precisely on either 'a' or 'b'!
			(this matters iff the objects are overlapping and thus have multiple/ambiguous
			collision points; we pick an 'in-between' spot to simplify matters, such that
			the same spot will be used for the collision in both directions)
	-- collideNormal is the normal the collision surface at 'a' at collideLoc.
			(thus the normal for b is exactly opposite in direction)
	-- collideNormal is guaranteed to be a unit vector
*/
typedef Bool (*CollideTestProc)(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);

// if the dist is greater than maxDist, return false, and the output stuff is undefined.
typedef Bool (*DistCalcProc)
(
	const Coord3D *posA,
	const Object *objA,
	const Coord3D *posB,
	const Object *objB,
	Real& abDistSqr,
	Coord3D& abVec,
	Real maxDistSqr
);

//-----------------------------------------------------------------------------
//         Inline Functions
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
#define NO_FILTER_PROFILING

#ifdef FILTER_PROFILING
Bool DoFilterProfiling = false;
#endif

//DECLARE_PERF_TIMER(filtersAllow)
inline Bool filtersAllow(PartitionFilter **filters, Object *objOther)
{
	//USE_PERF_TIMER(filtersAllow)
#ifdef FILTER_PROFILING
	const Int MAXR = 32;
	static const char* names[MAXR];
	static Int rejections[MAXR];
	static Int usefulRejections[MAXR];
	static Int calls = 0;
	static Int maxEver = 0;

	Int idx;
	Bool allow = true;

	if (DoFilterProfiling)
	{
		if (calls == 0)
		{
			for (idx = 0; idx < MAXR; ++idx)
			{
				names[idx] = nullptr;
				rejections[idx] = 0;
				usefulRejections[idx] = 0;
			}
			maxEver = 0;
		}
		++calls;
	}

	for (PartitionFilter **fp = filters; fp && *fp; fp++)
	{
		if (!(*fp)->allow(objOther))
		{
			if (DoFilterProfiling)
			{
				const char** namesarr = names;
				for (idx = 0; idx < maxEver; ++idx)
					if (namesarr[idx] == (*fp)->debugGetName())
						break;
				if (idx == maxEver)
				{
					namesarr[maxEver++] = (*fp)->debugGetName();
					DEBUG_ASSERTCRASH(maxEver<MAXR,("hmm, enlarge this array"));
				}
				++rejections[idx];
				if (allow)
					++usefulRejections[idx];
				allow = false;
				if (maxEver < idx)
					maxEver = idx;
			}
			else
			{
				return false;
			}
		}
	}

	if (DoFilterProfiling && calls > 0 && calls % 1000==0)
	{
		DEBUG_LOG(("\n"));
		for (idx = 0; idx < maxEver; idx++)
		{
			DEBUG_LOG(("rejections[%s] = %d (useful = %d)",names[idx],rejections[idx],usefulRejections[idx]));
		}
	}

	return allow;
#else
	for (PartitionFilter **fp = filters; fp && *fp; fp++)
	{
		if (!(*fp)->allow(objOther))
		{
			return false;
		}
	}
	// assume true if no filters rejected it
	return true;
#endif
}

//-----------------------------------------------------------------------------
inline void vecDiff_2D(const Coord3D *posA, const Coord3D *posB, Coord3D *resultVec)
{
	DEBUG_ASSERTCRASH(posA && posB && resultVec, ("null parm"));
	resultVec->x = posA->x - posB->x;
	resultVec->y = posA->y - posB->y;
	resultVec->z = 0.0f;
}

//-----------------------------------------------------------------------------
inline void vecDiff_3D(const Coord3D *posA, const Coord3D *posB, Coord3D *resultVec)
{
	DEBUG_ASSERTCRASH(posA && posB && resultVec, ("null parm"));
	resultVec->x = posA->x - posB->x;
	resultVec->y = posA->y - posB->y;
	resultVec->z = posA->z - posB->z;
}

//-----------------------------------------------------------------------------
inline Real calcSqrDist_2D(const Coord3D *dist)
{
	return sqr(dist->x) + sqr(dist->y);
}

//-----------------------------------------------------------------------------
inline Real calcSqrDist_3D(const Coord3D *dist)
{
	return sqr(dist->x) + sqr(dist->y) + sqr(dist->z);
}

//-----------------------------------------------------------------------------
//         Local Functions
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
void hLineAddLooker(Int x1, Int x2, Int y, void *playerIndexVoid);
void hLineRemoveLooker(Int x1, Int x2, Int y, void *playerIndexVoid);
void hLineAddShrouder(Int x1, Int x2, Int y, void *playerIndexVoid);
void hLineRemoveShrouder(Int x1, Int x2, Int y, void *playerIndexVoid);
void hLineAddThreat(Int x1, Int x2, Int y, void *threatValueParms);
void hLineRemoveThreat(Int x1, Int x2, Int y, void *threatValueParms);
void hLineAddValue(Int x1, Int x2, Int y, void *threatValueParms);
void hLineRemoveValue(Int x1, Int x2, Int y, void *threatValueParms);

static void projectCoord3D(Coord3D *coord, const Coord3D *unitDir, Real dist);
static void flipCoord3D(Coord3D *coord);

static void rectToFourPoints(
	const CollideInfo *a,		// z is ignored
	Coord2D pts[]
);
static void testRotatedPointsAgainstRect(
	const Coord2D *pts,				// an array of 4
	const CollideInfo *a,
	Coord2D *avg,
	Int *avgTot
);

static Bool xy_collideTest_Rect_Rect(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool xy_collideTest_Rect_Circle(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool xy_collideTest_Circle_Rect(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool xy_collideTest_Circle_Circle(const Coord3D *a_pos, const Coord3D *b_pos, const Real a_radius, const Real b_radius, CollideLocAndNormal *cinfo);

static Bool collideTest_Sphere_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Sphere_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Sphere_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Cylinder_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Cylinder_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Cylinder_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Box_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Box_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);
static Bool collideTest_Box_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo);

static Bool distCalcProc_CenterAndCenter_2D(const Coord3D *posA, const Object *objA, const Coord3D *posB, const Object *objB, Real& abDistSqr, Coord3D& abVec, Real maxDistSqr);
static Bool distCalcProc_BoundaryAndBoundary_2D(const Coord3D *posA, const Object *objA, const Coord3D *posB, const Object *objB, Real& abDistSqr, Coord3D& abVec, Real maxDistSqr);
static Bool distCalcProc_CenterAndCenter_3D(const Coord3D *posA, const Object *objA, const Coord3D *posB, const Object *objB, Real& abDistSqr, Coord3D& abVec, Real maxDistSqr);
static Bool distCalcProc_BoundaryAndBoundary_3D(const Coord3D *posA, const Object *objA, const Coord3D *posB, const Object *objB, Real& abDistSqr, Coord3D& abVec, Real maxDistSqr);

static Bool doesCircleOverlapCell(Real centerX, Real centerY, Real radius, Real cellX, Real cellY, Real cellSize);

//-----------------------------------------------------------------------------
inline void projectCoord3D(Coord3D *coord, const Coord3D *unitDir, Real dist)
{
	coord->x += unitDir->x * dist;
	coord->y += unitDir->y * dist;
	coord->z += unitDir->z * dist;
}

//-----------------------------------------------------------------------------
inline void flipCoord3D(Coord3D *coord)
{
	coord->x = -coord->x;
	coord->y = -coord->y;
	coord->z = -coord->z;
}

//-----------------------------------------------------------------------------
static void testRotatedPointsAgainstRect(
	const Coord2D *pts,				// an array of 4
	const CollideInfo *a,
	Coord2D *avg,
	Int *avgTot
)
{
	//DEBUG_ASSERTCRASH(a->geom.getGeomType() == GEOMETRY_BOX, ("only boxes are ok here"));
	Real major = a->geom.getMajorRadius();
	Real minor = (a->geom.getGeomType() == GEOMETRY_SPHERE) ? a->geom.getMajorRadius() : a->geom.getMinorRadius();

	Real c = (Real)Cos(-a->angle);
	Real s = (Real)Sin(-a->angle);

	for (Int i = 0; i < 4; ++i, ++pts)
	{
		// convert to a delta relative to rect ctr
		Real ptx = pts->x - a->position.x;
		Real pty = pts->y - a->position.y;

		// inverse-rotate it to the right coord system
		Real ptx_new = (Real)fabs(ptx*c - pty*s);
		Real pty_new = (Real)fabs(ptx*s + pty*c);

		#ifdef INTENSE_DEBUG
		Real mag_a = sqr(ptx)+sqr(pty);
		Real mag_b = sqr(ptx_new)+sqr(pty_new);
		DEBUG_ASSERTCRASH(fabs(mag_a - mag_b) <= 1.0, ("hmm, unlikely"));
		#endif

		if (ptx_new <= major && pty_new <= minor)
		{
			avg->x += pts->x;
			avg->y += pts->y;
			*avgTot += 1;
		}
	}
}

//-----------------------------------------------------------------------------
static void rectToFourPoints(
	const CollideInfo *a,		// z is ignored
	Coord2D pts[]
)
{
	Real c = (Real)Cos(a->angle);
	Real s = (Real)Sin(a->angle);

	Real exc = a->geom.getMajorRadius()*c;
	Real eyc = a->geom.getMinorRadius()*c;
	Real exs = a->geom.getMajorRadius()*s;
	Real eys = a->geom.getMinorRadius()*s;

	// tl
	pts[0].x = a->position.x - exc - eys;
	pts[0].y = a->position.y + eyc - exs;

	// tr
	pts[1].x = a->position.x + exc - eys;
	pts[1].y = a->position.y + eyc + exs;

	// bl
	pts[2].x = a->position.x - exc + eys;
	pts[2].y = a->position.y - eyc - exs;

	// br
	pts[3].x = a->position.x + exc + eys;
	pts[3].y = a->position.y - eyc + exs;
}

//-----------------------------------------------------------------------------
/**
	2d utility routine for rect/cyl & rect/sphere collisions testing.
*/
static Bool xy_collideTest_Circle_Rect(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Bool result = xy_collideTest_Rect_Circle(b, a, cinfo);
	if (cinfo)
		flipCoord3D(&cinfo->normal);
	return result;
}

//-----------------------------------------------------------------------------
/**
	2d utility routine for rect/cyl & rect/sphere collisions testing.
*/
static Bool xy_collideTest_Rect_Circle(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
#if 1
	/// @todo srj -- this is better than the other one, since it actually handles rotated rects,
	// but still not as accurate as is could be. (srj)
	CollideInfo btmp = *b;
	btmp.geom.setMinorRadius(btmp.geom.getMajorRadius());
	return xy_collideTest_Rect_Rect(a, &btmp, cinfo);
#else
	// note, this actually tests the intersection of the the rect with the circle's
	// bounding box. in practice, this is usually good enough, since most of
	// our sphere/cyl shapes are small relative to boxes. (in fact, the WWMath
	// library takes a similar shortcut when colliding spheres with boxes in 3d.
	// so I figured it was probably good enough for us too.)

	Real circ_l = b->position.x - b->geom.getMajorRadius();
	Real circ_r = b->position.x + b->geom.getMajorRadius();
	Real circ_t = b->position.y - b->geom.getMajorRadius();
	Real circ_b = b->position.y + b->geom.getMajorRadius();
	Real rect_l = a->position.x - a->geom.getMajorRadius();
	Real rect_r = a->position.x + a->geom.getMajorRadius();
	Real rect_t = a->position.y - a->geom.getMinorRadius();
	Real rect_b = a->position.y + a->geom.getMinorRadius();

	if (circ_r >= rect_l &&	circ_l <= rect_r &&
		circ_b >= rect_t &&	circ_t <= rect_b)
	{
		if (cinfo)
		{
			vecDiff_2D(&b->position, &a->position, &cinfo->normal);
			cinfo->normal.normalize();
			cinfo->loc.x = (max(circ_l, rect_l) + min(circ_r, rect_r)) * 0.5f;
			cinfo->loc.y = (max(circ_t, rect_t) + min(circ_b, rect_b)) * 0.5f;
			cinfo->loc.z = (a->position.z + b->position.z) * 0.5f;
		}
		return true;
	}
	return false;
#endif
}

//-----------------------------------------------------------------------------
/**
	2d utility routine for cyl & sphere collisions testing.
*/
static Bool xy_collideTest_Circle_Circle(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Coord3D diff;
	vecDiff_2D(&b->position, &a->position, &diff);
	Real distSqr = calcSqrDist_2D(&diff);
	Real touchingDistSqr = sqr(a->geom.getMajorRadius() + b->geom.getMajorRadius());
	if (distSqr <= touchingDistSqr)
	{
		// calc the approximate location and normal.
		if (cinfo)
		{
			cinfo->normal = diff;
			cinfo->normal.normalize();
			cinfo->loc = a->position;
			projectCoord3D(&cinfo->loc, &cinfo->normal, a->geom.getMajorRadius());
		}

		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
/**
	2d utility routine for rect collisions testing.
*/
static Bool xy_collideTest_Rect_Rect(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Coord2D pts[4];
	Coord2D avg; avg.x = avg.y = 0.0f;
	Int avgTot = 0;

	rectToFourPoints(a, pts);
	testRotatedPointsAgainstRect(pts, b, &avg, &avgTot);

	rectToFourPoints(b, pts);
	testRotatedPointsAgainstRect(pts, a, &avg, &avgTot);

	if (avgTot > 0)
	{
		if (cinfo)
		{
			// this is a little hokey, but generally adequate...
			cinfo->loc.x = avg.x / avgTot;
			cinfo->loc.y = avg.y / avgTot;
			cinfo->loc.z = (a->position.z + b->position.z) * 0.5f;

			// yow, what exactly does it mean to find the normal to two *intersecting*
			// rects? with "true" physics we could project ahead and determine
			// what the collision point would be, which would of course always
			// give a perfect, symmetrical normal. however, we almost always determine
			// collisions "after the fact" and thus the rects already intersect, meaning
			// that it's tricky to find the normal. we could do it the "right" way, which
			// would be to back up the objects to find the perfect point (see above), but
			// we don't have velocity information at this point, so we can't really
			// do that. so, naturally, we provide a (poor) guess by just doing the center-to-center
			// normal. this is rarely the correct surface normal for a box-box collision,
			// but until I either (a) rewrite everything to have predictive collisions,
			// or (b) come up with a better definition of a useful normal in this case,
			// I'm not sure we can do a whole lot better... (srj)
			vecDiff_2D(&b->position, &a->position, &cinfo->normal);
			cinfo->normal.normalize();
		}
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
inline Bool z_collideTest_Sphere_Nonsphere(CollideTestProc xyproc, const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	// case 1: center of sphere is within the nonsphere z-space.
	if (a->position.z >= b->position.z && a->position.z <= b->position.z + b->geom.getMaxHeightAbovePosition())
	{
		if (xyproc(a, b, cinfo))
		{
			// a side-to-side collision; just need to adjust the z-coord of collideLoc
			if (cinfo)
			{
				cinfo->loc.z = a->position.z;
			}
			return true;
		}
		return false;
	}

	// case 2: center of sphere is above or below nonsphere, but within
	// major-radius of top or bottom.

	// 2a: sphere is below the nonsphere
	Real b_bot = b->position.z;
	if (a->position.z < b_bot && a->position.z + a->geom.getMajorRadius() >= b_bot)
	{
		// find the radius of the slice of the sphere that is at b_bot
		CollideInfo amod = *a;
		amod.position.z = b_bot;
		amod.geom.setMajorRadius((Real)sqrtf(sqr(a->geom.getMajorRadius()) - sqr(b_bot - a->position.z)));
		if (xyproc(&amod, b, cinfo))
		{
			// if you want to have 'end' collisions, you should add something like:
			//		if (circle a is contained by circle b) normal points straight up;
			// but since that's currently never possible (and even if it was, wouldn't matter)
			// we don't bother with the calculation.
			return true;
		}
		return false;
	}

	// 2b: sphere is above the nonsphere
	Real b_top = b->position.z + b->geom.getMaxHeightAbovePosition();
	if (a->position.z > b_top && a->position.z - a->geom.getMajorRadius() <= b_top)
	{
		CollideInfo amod = *a;
		amod.position.z = b_top;
		amod.geom.setMajorRadius((Real)sqrtf(sqr(a->geom.getMajorRadius()) - sqr(a->position.z - b_top)));
		if (xyproc(&amod, b, cinfo))
		{
			// if you want to have 'end' collisions, you should add something like:
			//		if (circle a is contained by circle b) normal points straight down;
			// but since that's currently never possible (and even if it was, wouldn't matter)
			// we don't bother with the calculation.
			return true;
		}
		return false;
	}

	return false;
}

//-----------------------------------------------------------------------------
inline Bool z_collideTest_Nonsphere_Nonsphere(CollideTestProc xyproc, const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	// note: we already know that there is a z-intersection (our caller filters that out)
	// so we need not recheck that here.

	// Preflight for close objects.
	Real dSqr = sqr(a->position.x-b->position.x) + sqr(a->position.y-b->position.y);
	Real minRadius = a->geom.getMajorRadius();
	Real r = a->geom.getMinorRadius();
	if (minRadius>r) minRadius = r;
	r = b->geom.getMajorRadius();
	if (minRadius>r) minRadius = r;
	r = b->geom.getMinorRadius();
	if (minRadius>r) minRadius = r;

	Bool closeEnough = sqr(minRadius) > dSqr;

	if (closeEnough || xyproc(a, b, cinfo))
	{
		// since we allow objects to overlap at times, how do we distinguish
		// between a "top-to-bottom" vs "side-to-side" collision?
		// currently, we don't: cylindrical shapes currently never collide
		// top-to-bottom, so we just declare everything to be side-to-side.
		// if this changes, need to smarten this.

		// case 1: side-to-side collision
		// just need to adjust the z-coord of collideLoc
		if (cinfo)
		{
			if (b->position.z > a->position.z)
				cinfo->loc.z = (b->position.z + a->position.z + a->geom.getMaxHeightAbovePosition()) * 0.5f;
			else
				cinfo->loc.z = (a->position.z + b->position.z + b->geom.getMaxHeightAbovePosition()) * 0.5f;
		}
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
static Bool collideTest_Sphere_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Coord3D diff;
	vecDiff_3D(&b->position, &a->position, &diff);
	Real distSqr = calcSqrDist_3D(&diff);
	Real touchingDistSqr = sqr(a->geom.getMajorRadius() + b->geom.getMajorRadius());
	if (distSqr <= touchingDistSqr)
	{
		if (cinfo)
		{
			cinfo->normal = diff;
			cinfo->normal.normalize();
			cinfo->loc = a->position;
			projectCoord3D(&cinfo->loc, &cinfo->normal, a->geom.getMajorRadius());
		}

		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
static Bool collideTest_Sphere_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Sphere_Nonsphere(xy_collideTest_Circle_Circle, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool collideTest_Sphere_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Sphere_Nonsphere(xy_collideTest_Circle_Rect, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool collideTest_Cylinder_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Bool result = z_collideTest_Sphere_Nonsphere(xy_collideTest_Circle_Circle, b, a, cinfo);
	if (cinfo)
		flipCoord3D(&cinfo->normal);
	return result;
}

//-----------------------------------------------------------------------------
static Bool collideTest_Cylinder_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Nonsphere_Nonsphere(xy_collideTest_Circle_Circle, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool collideTest_Cylinder_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Nonsphere_Nonsphere(xy_collideTest_Circle_Rect, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool collideTest_Box_Sphere(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	Bool result = z_collideTest_Sphere_Nonsphere(xy_collideTest_Circle_Rect, b, a, cinfo);
	if (cinfo)
		flipCoord3D(&cinfo->normal);
	return result;
}

//-----------------------------------------------------------------------------
static Bool collideTest_Box_Cylinder(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Nonsphere_Nonsphere(xy_collideTest_Rect_Circle, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool collideTest_Box_Box(const CollideInfo *a, const CollideInfo *b, CollideLocAndNormal *cinfo)
{
	return z_collideTest_Nonsphere_Nonsphere(xy_collideTest_Rect_Rect, a, b, cinfo);
}

//-----------------------------------------------------------------------------
static Bool distCalcProc_CenterAndCenter_2D(
	const Coord3D *posA,
	const Object *objA,
	const Coord3D *posB,
	const Object *objB,
	Real& abDistSqr,
	Coord3D& abVec,
	Real maxDistSqr
)
{
	// note that object positions are defined as the bottom center of the geometry,
	// thus we must add the radius to the z coord to get the proper center of the bounding sphere.
	Coord3D diff;
	diff.x = posB->x - posA->x;
	diff.y = posB->y - posA->y;
	diff.z = 0.0f;

	//if (abDistSqr)
	{
		abDistSqr = sqr(diff.x) + sqr(diff.y);
	}

	//if (abVec)
	{
		abVec = diff;
	}

	return abDistSqr < maxDistSqr;
}

//-----------------------------------------------------------------------------
static Bool distCalcProc_BoundaryAndBoundary_2D(
	const Coord3D *posA,
	const Object *objA,
	const Coord3D *posB,
	const Object *objB,
	Real& abDistSqr,
	Coord3D& abVec,
	Real maxDistSqr
)
{
	Coord3D diff;
	diff.x = posB->x - posA->x;
	diff.y = posB->y - posA->y;
	diff.z = 0.0f;

	Real actualDistSqr = sqr(diff.x) + sqr(diff.y);

	Real shrinkFactor = 1.0f;
	Real shrunkenDistSqr = actualDistSqr;
	Real totalRad = (objA ? objA->getGeometryInfo().getBoundingCircleRadius() : 0.0f) +
							(objB ? objB->getGeometryInfo().getBoundingCircleRadius() : 0.0f);

	if (totalRad > 0.0f)
	{
		Real actualDist = sqrtf(actualDistSqr);
		Real shrunkenDist = actualDist - totalRad;
		if (shrunkenDist <= 0.0f)
		{
			shrinkFactor = 0.0f;
			shrunkenDistSqr = 0.0f;	// sorry, distances can't be negative
		}
		else
		{
			shrinkFactor = shrunkenDist / actualDist;
			shrunkenDistSqr = sqr(shrunkenDist);
		}
	}

	//if (abDistSqr)
	{
		abDistSqr = shrunkenDistSqr;
	}

	//if (abVec)
	{
		DEBUG_ASSERTCRASH(shrinkFactor >= 0.0f && shrinkFactor <= 1.0f, ("Hmm, this should not be possible."));
		diff.x *= shrinkFactor;
		diff.y *= shrinkFactor;
		abVec = diff;
	}
	return abDistSqr < maxDistSqr;
}

//-----------------------------------------------------------------------------
static Bool distCalcProc_CenterAndCenter_3D(
	const Coord3D *posA,
	const Object *objA,
	const Coord3D *posB,
	const Object *objB,
	Real& abDistSqr,
	Coord3D& abVec,
	Real maxDistSqr
)
{
	// note that object positions are defined as the bottom center of the geometry,
	// thus we must add the radius to the z coord to get the proper center of the bounding sphere.
	Coord3D diff;
	diff.x = posB->x - posA->x;
	diff.y = posB->y - posA->y;
	diff.z = posB->z - posA->z;

	//if (abDistSqr)
	{
		abDistSqr = sqr(diff.x) + sqr(diff.y) + sqr(diff.z);
	}

	//if (abVec)
	{
		abVec = diff;
	}
	return abDistSqr < maxDistSqr;
}

//-----------------------------------------------------------------------------
static Bool distCalcProc_BoundaryAndBoundary_3D(
	const Coord3D *posA,
	const Object *objA,
	const Coord3D *posB,
	const Object *objB,
	Real& abDistSqr,
	Coord3D& abVec,
	Real maxDistSqr
)
{
	const GeometryInfo* geomA = objA ? &objA->getGeometryInfo() : nullptr;
	const GeometryInfo* geomB = objB ? &objB->getGeometryInfo() : nullptr;

	// note that object positions are defined as the bottom center of the geometry,
	// thus we must add the radius to the z coord to get the proper center of the bounding sphere.
	Coord3D diff;
	diff.x = posB->x - posA->x;
	diff.y = posB->y - posA->y;
	diff.z = ((posB->z + (geomB ? geomB->getZDeltaToCenterPosition() : 0.0f))
						- (posA->z + (geomA ? geomA->getZDeltaToCenterPosition() : 0.0f)));

	Real actualDistSqr = sqr(diff.x) + sqr(diff.y) + sqr(diff.z);

	Real shrinkFactor = 1.0f;
	Real shrunkenDistSqr = actualDistSqr;
	Real totalRad = (geomA?geomA->getBoundingSphereRadius():0) + (geomB?geomB->getBoundingSphereRadius():0);
	if (totalRad > 0.0f)
	{
		Real actualDist = sqrtf(actualDistSqr);
		Real shrunkenDist = actualDist - totalRad;
		if (shrunkenDist <= 0.0f)
		{
			shrinkFactor = 0.0f;
			shrunkenDistSqr = 0.0f;	// sorry, distances can't be negative
		}
		else
		{
			shrinkFactor = shrunkenDist / actualDist;
			shrunkenDistSqr = sqr(shrunkenDist);
		}
	}

	//if (abDistSqr)
	{
		abDistSqr = shrunkenDistSqr;
	}

	//if (abVec)
	{
		DEBUG_ASSERTCRASH(shrinkFactor >= 0.0f && shrinkFactor <= 1.0f, ("Hmm, this should not be possible."));
		diff.x *= shrinkFactor;
		diff.y *= shrinkFactor;
		diff.z *= shrinkFactor;
		abVec = diff;
	}
	return abDistSqr < maxDistSqr;
}

//-----------------------------------------------------------------------------
//         Private Types
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Private Data
//-----------------------------------------------------------------------------

static DistCalcProc theDistCalcProcs[] =
{
	distCalcProc_CenterAndCenter_2D,
	distCalcProc_CenterAndCenter_3D,
	distCalcProc_BoundaryAndBoundary_2D,
	distCalcProc_BoundaryAndBoundary_3D,
};

// NOTE: This *DEPENDS* on the order of the geometry enum defines
static CollideTestProc theCollideTestProcs[] =
{
	collideTest_Sphere_Sphere,
	collideTest_Sphere_Cylinder,
	collideTest_Sphere_Box,
	collideTest_Cylinder_Sphere,
	collideTest_Cylinder_Cylinder,
	collideTest_Cylinder_Box,
	collideTest_Box_Sphere,
	collideTest_Box_Cylinder,
	collideTest_Box_Box
};

//-----------------------------------------------------------------------------
//         Public Data
//-----------------------------------------------------------------------------
PartitionManager *ThePartitionManager = nullptr;  ///< the object manager singleton

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

class PartitionContactList
{
private:

	// GeneralsX: fix/fps-contact-flat. Measured by on
	// probe/pf-contact (Windows, pinned): 2,000 infantry under one order = 208k pair attempts and 87k unique pairs per
	// frame, 13.9 ms mean logic tick; a macOS sample put most of addToContactList in the chain walk's cache misses and
	// ~14% of it in the node pool's mutex. So the pairs now live in ONE flat array reused across frames, and "is this
	// pair already listed?" is an open-addressed table holding the pair itself: an attempt reads one table line instead
	// of two object ids, a socket and a chain of pooled nodes, and a new pair costs no pool allocation.
	//
	// RESULT-IDENTICAL BY CONSTRUCTION - processContactList() gets the same pairs in the same order:
	//  * a pair is listed on its first attempt only, in either order, exactly as the pooled chain decided;
	//  * the old list was LIFO (each new node pushed on its front), so processContactList() walks the array from
	//    its END; the table and its hash decide nothing but "seen?", so no hash, size or probe order is visible;
	//  * removeSpecificPartitionData() and processContactList() null a pair in place, as before, AND turn its table
	//    slot into a tombstone that matches nothing - nulling the old node did the same, since its fields were the
	//    chain's key - so a later attempt at the same pair is listed again, as before.
	// Only one PartitionContactList is alive at a time (PartitionManager::update, not re-entrant; TheContactList
	// assumes it too), so the storage is static and keeps its capacity; resetContactList() leaves the table empty.
	struct Contact
	{
		PartitionData*	m_obj;		///< one object that is possibly colliding
		PartitionData*	m_other;	///< the other object
		UnsignedInt			m_slot;		///< its table slot, NO_SLOT once a grow() dropped a removed pair
	};
	struct Slot
	{
		PartitionData*	m_lo;			///< the pair, lower address first; empty when null
		PartitionData*	m_hi;
	};
	enum { MIN_SLOT_BITS = 16 };
	static const UnsignedInt NO_SLOT = 0xffffffffu;

	static std::vector<Contact> s_contacts;	///< this frame's pairs in insertion order
	static std::vector<Slot> s_slots;				///< power-of-two size, never more than half full (tombstones count)
	static UnsignedInt s_slotMask;
	static Int s_live;											///< live instances (one at a time, see the constructor)

	static PartitionData* tombstone() { return reinterpret_cast<PartitionData*>(static_cast<uintptr_t>(1)); }
	static UnsignedInt firstSlot(const PartitionData *lo, const PartitionData *hi);
	static void grow();

public:

	PartitionContactList()
	{
		// the storage is shared: a second live list would see and clear this one's pairs
		DEBUG_ASSERTCRASH(s_live == 0, ("only one PartitionContactList may be alive at a time"));
		++s_live;
	}

	~PartitionContactList()
	{
		resetContactList();
		--s_live;
	}

	/**
		add a pair of objects to the contact list. If the pair is already
		present (in either this order or reverse order), nothing is done.
		Note that it is OK for other==null (this indicates a collisions with
		the ground) but it is not OK for obj==null.
	*/
	void addToContactList(PartitionData *obj, PartitionData *other);

	/**
		process all pairs in the contact list: first, determine if they
		truly collide, based on their geometry. if so, call the collide
		actions for each object in the pair.
	*/
	void processContactList();

	/**
		discard the contents of the contact list.
	*/
	void resetContactList();

	/**
		remove any contacts that refer to the given data.
	*/
	void removeSpecificPartitionData(PartitionData* data);

};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

#ifdef FASTER_GCO
// nothing
#else
//-----------------------------------------------------------------------------
CellOutwardIterator::CellOutwardIterator(PartitionManager *mgr, Int x, Int y)
{
	m_mgr = mgr;
	m_cellCenterX = x;
	m_cellCenterY = y;
	m_cellRadius = 0;
	m_maxRadius = max(max(x, mgr->getCellCountX() - x), max(y, mgr->getCellCountY() - y));
	m_delta[0] = m_delta[1] = 0;
	m_inc = -1;
	m_cnt = 0;
	m_axis = 1;
	m_iter = 1;
	m_delta[m_axis] -= m_inc;
}

//-----------------------------------------------------------------------------
CellOutwardIterator::~CellOutwardIterator()
{
}

//-----------------------------------------------------------------------------
PartitionCell *CellOutwardIterator::nextCell(Bool skipEmpties)
{

try_again:

	if (m_cellRadius <= m_maxRadius)
	{

		if (m_iter > 0)
		{
			m_delta[m_axis] += m_inc;
			--m_iter;
			PartitionCell *cell = m_mgr->getCellAt(m_cellCenterX + m_delta[0], m_cellCenterY + m_delta[1]);
			if (!cell)
				goto try_again;
			if (skipEmpties && !cell->getFirstCoiInCell())
				goto try_again;
			return cell;
		}

		m_axis += 1;
		if (m_axis >= 2)
		{
			m_axis = 0;
			m_inc = -m_inc;
			m_cnt += 1;
			m_cellRadius = max(IABS(m_delta[0]), IABS(m_delta[1]));
		}

		m_iter = m_cnt;
		goto try_again;
	}

	return nullptr;
}
#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
CellAndObjectIntersection::CellAndObjectIntersection()
{
	m_cell = nullptr;
	m_module = nullptr;
	m_prevCoi = nullptr;
	m_nextCoi = nullptr;
}

//-----------------------------------------------------------------------------
CellAndObjectIntersection::~CellAndObjectIntersection()
{
	DEBUG_ASSERTCRASH(m_prevCoi == nullptr && m_nextCoi == nullptr, ("destroying a linked COI"));
	DEBUG_ASSERTCRASH(!getModule(), ("destroying an in-use COI"));
}

//-----------------------------------------------------------------------------
void CellAndObjectIntersection::friend_addToCellList(CellAndObjectIntersection **pListHead)
{
	DEBUG_ASSERTCRASH(m_prevCoi == nullptr && m_nextCoi == nullptr && *pListHead != this, ("trying to add a cell to list, but it appears to already be in a list"));

	this->m_nextCoi = *pListHead;
	if (*pListHead)
		(*pListHead)->m_prevCoi = this;
	*pListHead = this;
}

//-----------------------------------------------------------------------------
void CellAndObjectIntersection::friend_removeFromCellList(CellAndObjectIntersection **pListHead)
{
#define DEBUG_ASSERTINLIST(c) \
	DEBUG_ASSERTCRASH((c)->m_prevCoi != nullptr || (c)->m_nextCoi != nullptr || *pListHead == (c), ("cell is not in list"));

	DEBUG_ASSERTINLIST(this);

	if (this->m_prevCoi)
	{
		DEBUG_ASSERTINLIST(this->m_prevCoi);
		DEBUG_ASSERTCRASH(*pListHead != this, ("bad linkage"));
		this->m_prevCoi->m_nextCoi = this->m_nextCoi;
	}
	else
	{
		DEBUG_ASSERTCRASH(*pListHead == this, ("bad linkage"));
		*pListHead = this->m_nextCoi;
	}

	if (this->m_nextCoi)
	{
		DEBUG_ASSERTINLIST(this->m_nextCoi);
		this->m_nextCoi->m_prevCoi = this->m_prevCoi;
	}

	this->m_prevCoi = nullptr;
	this->m_nextCoi = nullptr;

#undef DEBUG_ASSERTINLIST
}

//-----------------------------------------------------------------------------
void CellAndObjectIntersection::addCoverage(PartitionCell *cell, PartitionData *module)
{
	DEBUG_ASSERTCRASH(m_cell == nullptr || m_cell == cell, ("mismatch"));
	DEBUG_ASSERTCRASH(m_module == nullptr || m_module == module, ("mismatch"));

	if (m_module != nullptr && m_module != module)
	{
		DEBUG_CRASH(("COI already in use by another module!"));
		return;
	}

	if (m_cell == nullptr)
	{
		cell->friend_addToCellList(this);
		ThePartitionManager->friend_addCellOwner(cell, module->friend_getOwnerTag());
	}

	m_cell = cell;
	m_module = module;
}

//-----------------------------------------------------------------------------
void CellAndObjectIntersection::removeAllCoverage()
{
	if (m_module == nullptr)
	{
		DEBUG_CRASH(("COI not in use"));
		return;
	}

	m_cell->friend_removeFromCellList(this);
	ThePartitionManager->friend_removeCellOwner(m_cell, m_module->friend_getOwnerTag());
	m_cell = nullptr;
	m_module = nullptr;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionCell::PartitionCell()
{
	m_cellX = m_cellY = 0;
	m_firstCoiInCell = nullptr;
	m_coiCount = 0;
#ifdef PM_CACHE_TERRAIN_HEIGHT
	m_loTerrainZ = HUGE_DIST;		// huge positive
	m_hiTerrainZ = -HUGE_DIST;	// huge negative
#endif
	/*
		You may be asking yourself: why do we model the shroud for all players,
		rather than just the local player? And the answer is: because this allows
		us to checksum these values for net games, to help prevent "shroud cheaters"
		(who use a trainer to disable the shroud on their system).
	*/
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		// Default is "passive shroud".  1,0.
		m_shroudLevel[i].m_currentShroud = 1;
		m_shroudLevel[i].m_activeShroudLevel = 0;

		// default cash value is 0
		m_cashValue[i] = 0;

		// default threat value is 0
		m_threatValue[i] = 0;
	}
}

//-----------------------------------------------------------------------------
PartitionCell::~PartitionCell()
{
	DEBUG_ASSERTCRASH(m_firstCoiInCell == nullptr && m_coiCount == 0, ("destroying a nonempty PartitionCell"));
	// but don't destroy the Cois; they don't belong to us
}

//-----------------------------------------------------------------------------
void PartitionCell::invalidateShroudedStatusForAllCois(Int playerIndex)
{
	for (CellAndObjectIntersection* coi = m_firstCoiInCell; coi; coi = coi->getNextCoi())
	{
		coi->getModule()->invalidateShroudedStatusForPlayer(playerIndex);
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::addLooker(Int playerIndex)
{
	CellShroudStatus oldShroud = getShroudStatusForPlayer( playerIndex );
	// The decreasing Algorithm: A 1 will go straight to -1, otherwise it just gets decremented
	m_shroudLevel[playerIndex].m_currentShroud = min( m_shroudLevel[playerIndex].m_currentShroud - 1, -1 );

	CellShroudStatus newShroud = getShroudStatusForPlayer( playerIndex );

//	DEBUG_LOG(( "ADD    %d, %d.  CS = %d, AS = %d for player %d.",
//							m_cellX,
//							m_cellY,
//							m_shroudLevel[playerIndex].m_currentShroud,
//							m_shroudLevel[playerIndex].m_activeShroudLevel,
//							playerIndex
//							));

	if( oldShroud != newShroud )
	{
		// On an edge trigger, tell all objects to think about their shroudedness
		invalidateShroudedStatusForAllCois( playerIndex );

		if( playerIndex == rts::getObservedOrLocalPlayer()->getPlayerIndex() )
		{
			// and if this is the local player, do the Client update.
			TheDisplay->setShroudLevel(m_cellX, m_cellY, newShroud);
			TheRadar->setShroudLevel(m_cellX, m_cellY, newShroud);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::removeLooker(Int playerIndex)
{
	CellShroudStatus oldShroud = getShroudStatusForPlayer( playerIndex );
	// the increasing Algorithm: a -1 goes up to min(1,activeLevel), otherwise it just gets incremented
	if( m_shroudLevel[playerIndex].m_currentShroud == -1 )
		m_shroudLevel[playerIndex].m_currentShroud = min( m_shroudLevel[playerIndex].m_activeShroudLevel, (Short)1 );
	else
	{
		DEBUG_ASSERTCRASH( m_shroudLevel[playerIndex].m_currentShroud < 0, ("Someone is RemoveLooker-ing on a cell that is not looked at.  This will make a permanent shroud blob.") );
		m_shroudLevel[playerIndex].m_currentShroud++;
	}
	CellShroudStatus newShroud = getShroudStatusForPlayer( playerIndex );

//	DEBUG_LOG(( "REMOVE %d, %d.  CS = %d, AS = %d for player %d.",
//							m_cellX,
//							m_cellY,
//							m_shroudLevel[playerIndex].m_currentShroud,
//							m_shroudLevel[playerIndex].m_activeShroudLevel,
//							playerIndex
//							));

	if( oldShroud != newShroud )
	{
		// On an edge trigger, tell all objects to think about their shroudedness
		invalidateShroudedStatusForAllCois( playerIndex );

		if( playerIndex == rts::getObservedOrLocalPlayer()->getPlayerIndex() )
		{
			// and if this is the local player, do the Client update.
			TheDisplay->setShroudLevel(m_cellX, m_cellY, newShroud);
			TheRadar->setShroudLevel(m_cellX, m_cellY, newShroud);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::addShrouder( Int playerIndex )
{
	CellShroudStatus oldShroud = getShroudStatusForPlayer( playerIndex );
	// Increasing active shroud: activeLevel gets incremented, and CS is set to 1 if at zero
	// do the algorithm
	m_shroudLevel[playerIndex].m_activeShroudLevel++;
	if( m_shroudLevel[playerIndex].m_currentShroud == 0 )
	{
		m_shroudLevel[playerIndex].m_currentShroud = 1;
	}
	CellShroudStatus newShroud = getShroudStatusForPlayer( playerIndex );

	if( oldShroud != newShroud )
	{
		// On an edge trigger, tell all objects to think about their shroudedness
		invalidateShroudedStatusForAllCois( playerIndex );

		// and update the client if we are on the local player
		if( playerIndex == rts::getObservedOrLocalPlayer()->getPlayerIndex() )
		{
			TheDisplay->setShroudLevel(m_cellX, m_cellY, newShroud);
			TheRadar->setShroudLevel(m_cellX, m_cellY, newShroud);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::removeShrouder( Int playerIndex )
{
	// Decreasing active shroud: just decrement activeLevel.  This will never result in a client change.
	// Either it was passive shroud and is now active, or it was being looked at and still is.
	m_shroudLevel[playerIndex].m_activeShroudLevel--;
	DEBUG_ASSERTCRASH( m_shroudLevel[playerIndex].m_activeShroudLevel >= 0, ("Shroud generation has gone negative.  This can't happen.") );
}

//-----------------------------------------------------------------------------
//Bool PartitionCell::isShroudedForPlayer( Int playerIndex ) const
//{
	// There isn't an absolute answer.  This cell is only shrouded in regards to a person
//	return (m_shroudLevel[playerIndex].m_currentShroud == 1);
//}

//-----------------------------------------------------------------------------
CellShroudStatus PartitionCell::getShroudStatusForPlayer( Int playerIndex ) const
{
	// There are now three answers, but the question still requires "to whom"

	if( m_shroudLevel[playerIndex].m_currentShroud == 1 )
		return CELLSHROUD_SHROUDED;
	else if( m_shroudLevel[playerIndex].m_currentShroud == 0 )
		return CELLSHROUD_FOGGED;// ie Nobody actively looking
	else
		return CELLSHROUD_CLEAR;
}

//-----------------------------------------------------------------------------
UnsignedInt PartitionCell::getThreatValue( Int playerIndex )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
		return m_threatValue[playerIndex];
	}
	return 0;
}

//-----------------------------------------------------------------------------
void PartitionCell::addThreatValue( Int playerIndex, UnsignedInt threatValue )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
#ifdef DEBUG_CRASHING
		UnsignedInt oldThreatVal = m_threatValue[playerIndex];
		DEBUG_ASSERTCRASH(oldThreatVal <= oldThreatVal + threatValue, ("adding new threat value overflowed allotted storage."));
#endif
		m_threatValue[playerIndex] += threatValue;
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::removeThreatValue( Int playerIndex, UnsignedInt threatValue )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
#ifdef DEBUG_CRASHING
		UnsignedInt oldThreatVal = m_threatValue[playerIndex];
		DEBUG_ASSERTCRASH(oldThreatVal >= oldThreatVal - threatValue, ("removing new threat value underflowed allotted storage."));
#endif
		m_threatValue[playerIndex] -= threatValue;
	}
}

//-----------------------------------------------------------------------------
UnsignedInt PartitionCell::getCashValue( Int playerIndex )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
		return m_cashValue[playerIndex];
	}
	return 0;
}

//-----------------------------------------------------------------------------
void PartitionCell::addCashValue( Int playerIndex, UnsignedInt cashValue )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
#ifdef DEBUG_CRASHING
		UnsignedInt oldCashVal = m_cashValue[playerIndex];
		DEBUG_ASSERTCRASH(oldCashVal <= oldCashVal + cashValue, ("adding new cash value overflowed allotted storage."));
#endif
		m_cashValue[playerIndex] += cashValue;
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::removeCashValue( Int playerIndex, UnsignedInt cashValue )
{
	if (playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT) {
#ifdef DEBUG_CRASHING
		UnsignedInt oldCashVal = m_cashValue[playerIndex];
		DEBUG_ASSERTCRASH(oldCashVal >= oldCashVal - cashValue, ("removing new cash value underflowed allotted storage."));
#endif
		m_cashValue[playerIndex] -= cashValue;
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::friend_addToCellList(CellAndObjectIntersection *coi)
{
	if (coi)
	{
		coi->friend_addToCellList(&m_firstCoiInCell);
		++m_coiCount;
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::friend_removeFromCellList(CellAndObjectIntersection *coi)
{
	if (coi)
	{
		coi->friend_removeFromCellList(&m_firstCoiInCell);
		--m_coiCount;
	}
}

//-----------------------------------------------------------------------------
void PartitionCell::getCellCenterPos(Real& x, Real& y)
{
	ThePartitionManager->getCellCenterPos(m_cellX, m_cellY, x, y);
}

//-----------------------------------------------------------------------------
#ifdef RTS_DEBUG
void PartitionCell::validateCoiList()
{
	CellAndObjectIntersection *nextCoi = 0, *prevCoi = 0;
	for (CellAndObjectIntersection *coi = getFirstCoiInCell(); coi; prevCoi = coi, coi = nextCoi)
	{
		nextCoi = coi->getNextCoi();
		DEBUG_ASSERTCRASH(coi->getPrevCoi() == prevCoi, ("coi link mismatch"));
		DEBUG_ASSERTCRASH(prevCoi == nullptr || prevCoi->getNextCoi() == coi, ("coi link mismatch"));
		DEBUG_ASSERTCRASH((coi == getFirstCoiInCell()) == (prevCoi == nullptr) , ("coi link mismatch"));
		DEBUG_ASSERTCRASH(nextCoi == nullptr || nextCoi->getPrevCoi() == coi, ("coi link mismatch"));
	}
}
#endif

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void PartitionCell::crc( Xfer *xfer )
{

	// GeneralsX: 16 planes while the match has <= 16 Players (what every 16-Player build
	// hashed, so <= 14-slot matches and their replays keep their CRC), all MAX_PLAYER_COUNT above that (GameCommon.h).
	xfer->xferUser(&m_shroudLevel, sizeof(ShroudLevel) * gxCrcPlayerSpan());
	xfer->xferUser(&m_cellX, sizeof(m_cellX));
	xfer->xferUser(&m_cellY, sizeof(m_cellY));

}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void PartitionCell::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// xfer shroud data
	xfer->xferUser( &m_shroudLevel, sizeof( ShroudLevel ) * MAX_PLAYER_COUNT );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void PartitionCell::loadPostProcess()
{

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionData::PartitionData()
{
	//DEBUG_LOG(("create pd %08lx",this));
	m_next = nullptr;
	m_prev = nullptr;
	m_nextDirty = nullptr;
	m_prevDirty = nullptr;
	m_object = nullptr;
	m_ghostObject = nullptr;
	m_coiArrayCount = 0;
	m_coiArray = nullptr;
	m_coiInUseCount = 0;
	m_doneFlag = 0;
	m_ownerTag = PARTITION_OWNER_NONE;
	m_fillPos.zero();
	m_dirtyStatus = NOT_DIRTY;
	m_lastCell = nullptr;
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		m_everSeenByPlayer[i] = false;
		m_shroudedness[i] = OBJECTSHROUD_INVALID;
		m_shroudednessPrevious[i] = OBJECTSHROUD_INVALID;
	}
}

//-----------------------------------------------------------------------------
PartitionData::~PartitionData()
{
	//DEBUG_LOG(("toss pd for pd %08lx obj %08lx",this,m_object));
	removeAllTouchedCells();
	freeCoiArray();
	DEBUG_ASSERTCRASH(ThePartitionManager, ("ThePartitionManager is null"));
	if (ThePartitionManager && ThePartitionManager->isInListDirtyModules(this))
	{
		//DEBUG_LOG(("remove pd %08lx from dirty list (%08lx %08lx)",this,m_prevDirty,m_nextDirty));
		ThePartitionManager->removeFromDirtyModules(this);
		//DEBUG_ASSERTCRASH(!ThePartitionManager->isInListDirtyModules(this), ("hmm"));
	}
}

//-----------------------------------------------------------------------------
Int PartitionData::getControllingPlayerIndex() const
{
	const Player* p = getObject()->getControllingPlayer();
	if (p)
	{
		Int playerIndex = p->getPlayerIndex();
		DEBUG_ASSERTCRASH(playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT, ("bad playerIndex"));
		return playerIndex;
	}

	DEBUG_CRASH(("this should never happen"));
	throw ERROR_BUG;
	return 0;
}

//-----------------------------------------------------------------------------
// GeneralsX: see m_cellOwnerMask
void PartitionManager::friend_addCellOwner(const PartitionCell *cell, Int tag)
{
	const Int i = (Int)(cell - m_cells);
	DEBUG_ASSERTCRASH(i >= 0 && i < m_totalCellCount && tag >= 0 && tag < PARTITION_OWNER_TAGS, ("bad cell owner"));
	if (m_cellOwnerCount[i * PARTITION_OWNER_TAGS + tag]++ == 0)
		m_cellOwnerMask[i] |= gxOwnerTagBit(tag);
}

//-----------------------------------------------------------------------------
void PartitionManager::friend_removeCellOwner(const PartitionCell *cell, Int tag)
{
	const Int i = (Int)(cell - m_cells);
	DEBUG_ASSERTCRASH(i >= 0 && i < m_totalCellCount && tag >= 0 && tag < PARTITION_OWNER_TAGS, ("bad cell owner"));
	if (--m_cellOwnerCount[i * PARTITION_OWNER_TAGS + tag] == 0)
		m_cellOwnerMask[i] &= ~gxOwnerTagBit(tag);
}

//-----------------------------------------------------------------------------
// GeneralsX: see PARTITION_OWNER_NONE.
static Int calcOwnerTag(const Object *obj)
{
	const Player *p = obj ? obj->getControllingPlayer() : nullptr;
	if (p == nullptr)
		return PARTITION_OWNER_NONE;
	DEBUG_ASSERTCRASH(p->getPlayerIndex() >= 0 && p->getPlayerIndex() < MAX_PLAYER_COUNT, ("bad playerIndex"));
	return p->getPlayerIndex();
}

//-----------------------------------------------------------------------------
void PartitionData::friend_updateOwnerTag()
{
	const Int tag = calcOwnerTag(m_object);
	if (tag == m_ownerTag)
		return;

	CellAndObjectIntersection *coi = m_coiArray;
	for (Int i = m_coiArrayCount; i > 0; --i, ++coi)
	{
		if (coi->getModule())
		{
			ThePartitionManager->friend_removeCellOwner(coi->getCell(), m_ownerTag);
			ThePartitionManager->friend_addCellOwner(coi->getCell(), tag);
		}
	}
	m_ownerTag = tag;
}

//-----------------------------------------------------------------------------
// only used to restore state after map border resizing and/or xfer!
void PartitionData::friend_setShroudednessPrevious(Int playerIndex, ObjectShroudStatus status)
{
	m_shroudednessPrevious[playerIndex] = status;

	// if our status is anything but shrouded, assume we've never been seen by the player.
	m_everSeenByPlayer[playerIndex] = (status == OBJECTSHROUD_SHROUDED) ? false : true;

	// if we're already invalid, mark us as special-invalid.
#ifndef DISABLE_INVALID_PREVENTION
	if (m_shroudedness[playerIndex] == OBJECTSHROUD_INVALID)
		m_shroudedness[playerIndex] = OBJECTSHROUD_INVALID_BUT_PREVIOUS_VALID;
#endif
}

//-----------------------------------------------------------------------------
// GeneralsX: see the header. The same cell walk as getShroudedStatus below, read-only.
Bool PartitionData::isAnyCellClearForPlayer( Int playerIndex ) const
{
	CellAndObjectIntersection *coi = m_coiArray;	// getCell() is non-const; the cell is only read
	for( Int i = m_coiInUseCount; i; --i, ++coi )
	{
		if( coi->getCell()->getShroudStatusForPlayer( playerIndex ) == CELLSHROUD_CLEAR )
			return TRUE;
	}
	return FALSE;
}

//-----------------------------------------------------------------------------
// GeneralsX: see the header. getShroudedStatus's SHROUDED condition from the cells, read-only.
Bool PartitionData::isFullyShroudedForPlayer( Int playerIndex ) const
{
	CellAndObjectIntersection *coi = m_coiArray;	// getCell() is non-const; the cell is only read
	for( Int i = m_coiInUseCount; i; --i, ++coi )
	{
		if( coi->getCell()->getShroudStatusForPlayer( playerIndex ) != CELLSHROUD_SHROUDED )
			return FALSE;
	}
	return TRUE;	// every cell shrouded, or none at all: off the map
}

//-----------------------------------------------------------------------------
// GeneralsX: rb-sense-2: getShroudedStatus's CLEAR (one touched cell clear) limited to the cells marked in
// `mark` (PartitionManager::markLookCells). No cache: the AI rule bank asks it once per candidate per pass.
Bool PartitionData::touchesMarkedClearCell(Int playerIndex, const std::vector<UnsignedByte> &mark) const
{
	if (playerIndex < 0 || playerIndex >= MAX_PLAYER_COUNT || ThePartitionManager == nullptr)
		return FALSE;
	const size_t countX = (size_t)ThePartitionManager->getCellCountX();
	CellAndObjectIntersection *coi = m_coiArray;
	for (Int i = m_coiInUseCount; i; --i, ++coi)
	{
		const PartitionCell *cell = coi->getCell();
		if (cell == nullptr)
			continue;
		const size_t k = (size_t)cell->getCellY() * countX + (size_t)cell->getCellX();
		if (k < mark.size() && mark[k] != 0 && cell->getShroudStatusForPlayer(playerIndex) == CELLSHROUD_CLEAR)
			return TRUE;
	}
	return FALSE;
}

ObjectShroudStatus PartitionData::getShroudedStatus(Int playerIndex)
{
	// sanity
	DEBUG_ASSERTCRASH( playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT,
										 ("PartitionData::getShroudedStatus - Invalid player index '%d'", playerIndex) );

	if (!ThePartitionManager->getUpdatedSinceLastReset())
	{
		// The shroud should be invalid until update has been called.
		// I'll set myself as invalid so the next time you ask I'll recompute.  (If I just return Invalid, then the old state will persist unfairly over a reset)
		invalidateShroudedStatusForPlayer(playerIndex);
		return m_shroudedness[playerIndex];
	}

#ifndef DISABLE_INVALID_PREVENTION
	if (m_shroudedness[playerIndex] == OBJECTSHROUD_INVALID || m_shroudedness[playerIndex] == OBJECTSHROUD_INVALID_BUT_PREVIOUS_VALID)
	{
		Bool updateShroudednessPrevious = (m_shroudedness[playerIndex] != OBJECTSHROUD_INVALID_BUT_PREVIOUS_VALID);
#else
	if (m_shroudedness[playerIndex] == OBJECTSHROUD_INVALID)
	{
#endif
		Int shroudedCells = 0;
		Int foggedCells = 0;
		CellAndObjectIntersection* coi = m_coiArray;
		for (Int i = m_coiInUseCount; i; --i, ++coi)
		{
			if( coi->getCell()->getShroudStatusForPlayer(playerIndex) == CELLSHROUD_SHROUDED )
				++shroudedCells;
			else if( coi->getCell()->getShroudStatusForPlayer(playerIndex) == CELLSHROUD_FOGGED )
				++foggedCells;
		}

		if( m_coiInUseCount == 0 )
		{	m_shroudedness[playerIndex] = OBJECTSHROUD_SHROUDED;				// Stuff off the map have no COIs since they aren't in the PartitionCell range
			m_everSeenByPlayer[playerIndex] = false;	//force object as never seen by the player
		}
		else if( shroudedCells == m_coiInUseCount )
		{	m_shroudedness[playerIndex] = OBJECTSHROUD_SHROUDED;				// every cell I use is shrouded
			m_everSeenByPlayer[playerIndex] = false; //force object as never seen by the player
			// GeneralsX: Whatever the previous status was, not only FOGGED: a snapshot kept through the
			// "enemy building never seen" refinement below (previous SHROUDED) is fog memory too, and every cell is shrouded
			// now. freeSnapShot is a no-op without a snapshot; see the CLEAR branch below.
			if (m_ghostObject)
			{	//we are shrouding an area that used to be fogged so release our memory of what was there.
				m_ghostObject->freeSnapShot(playerIndex);
			}
		}
		else if( shroudedCells + foggedCells == m_coiInUseCount )
		{
			m_shroudedness[playerIndex] = OBJECTSHROUD_FOGGED;	//object is visible but fogged.
			if (m_object && m_ghostObject)	//object does not exist for modules holding only GhostObjects
			{
				//fogged but may not be visible if faction unit or faction building that has not been seen before
				Player *player=ThePlayerList->getNthPlayer(playerIndex);
				if (player->getRelationship(m_object->getTeam()) == NEUTRAL)
				{	//anything neutral that moves around will not be rendered inside fog.
					if (!m_object->isKindOf(KINDOF_IMMOBILE))
						m_shroudedness[playerIndex] = OBJECTSHROUD_SHROUDED;
				}
				else	//Not neutral
				{	//enemy unit will always be shrouded unless it's a building that's already been seen by the player.  Fogged Mines are also always
					//shroued no matter what.
					if (!(m_object->isKindOf(KINDOF_IMMOBILE) && m_everSeenByPlayer[playerIndex]) || m_object->isKindOf(KINDOF_MINE))
						m_shroudedness[playerIndex] = OBJECTSHROUD_SHROUDED;
				}
				if (m_shroudedness[playerIndex] == OBJECTSHROUD_FOGGED)
				{	//successfully applied fog to object so check if we need to freeze it's state
					if (m_shroudednessPrevious[playerIndex] < OBJECTSHROUD_FOGGED)
					{	//object was not previously fogged but now is fogged.
						//need to record its current state so that it doesn't change
						//while fogged.
						m_ghostObject->snapShot(playerIndex);
					}
				}
			}
		}
		else if( shroudedCells == 0  &&  foggedCells == 0 )
		{	//Record that this object was seen by the player.  This info will be used to show fogged enemy faction buildings.
			m_everSeenByPlayer[playerIndex] = true;
			m_shroudedness[playerIndex] = OBJECTSHROUD_CLEAR;
			// GeneralsX: A visible object has no fog memory: free its snapshot whatever the previous
			// status was (both visible branches; freeSnapShot is a no-op without one), not only after FOGGED. A snapshot taken
			// while FOGGED survives a later recompute that the "enemy building never seen" refinement above turns into SHROUDED
			// (e.g. a derrick fogged while neutral, then captured by an enemy unseen): previous became SHROUDED, so retail's
			// "== FOGGED" never freed it, and the stale clone stayed in the scene for good - a destroyed building drawn standing
			// over its rubble. A save written
			// by a leaking build can hold such a snapshot with previous CLEAR. freeSnapShot touches only
			// the client's scene and snapshot list; m_shroudedness, m_everSeenByPlayer and m_shroudednessPrevious are written
			// exactly as before. Freeing at the refinement instead would make a fogged building vanish from the fog when an
			// enemy captures it unseen - telling the player about the capture.
			if (m_ghostObject)
			{	//object was previously fogged but now is visible so we no longer
				//need a ghost object.
				m_ghostObject->freeSnapShot(playerIndex);
			}
		}
		else
		{	//Record that this object was seen by the player.  This info will be used to show fogged enemy faction buildings.
			m_everSeenByPlayer[playerIndex] = true;
			m_shroudedness[playerIndex] = OBJECTSHROUD_PARTIAL_CLEAR;		// I am at least partially clear otherwise
			// GeneralsX: Whatever the previous status was, see the CLEAR branch above.
			if (m_ghostObject)
			{	//object was previously fogged but now is visible so we no longer
				//need a ghost object.
				m_ghostObject->freeSnapShot(playerIndex);
			}
		}
#ifndef DISABLE_INVALID_PREVENTION
 		if (m_coiInUseCount && updateShroudednessPrevious)
		{
#else
 		if (m_coiInUseCount)
		{
#endif
			//only remember the previous state of objects actually on the map.
 			m_shroudednessPrevious[playerIndex] = m_shroudedness[playerIndex];
		}
	}

	return m_shroudedness[playerIndex];
}

//-----------------------------------------------------------------------------
void PartitionData::removeAllTouchedCells()
{
	CellAndObjectIntersection *coi = m_coiArray;
	for (Int i = m_coiArrayCount; i > 0; --i, ++coi)
	{
		if (coi->getModule())
		{
			DEBUG_ASSERTCRASH(coi->getModule() == this, ("coi uses wrong module"));
			coi->removeAllCoverage();
			DEBUG_ASSERTCRASH(!coi->getModule(), ("coi should no longer be in use"));
			--m_coiInUseCount;
		}
	}
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("hmm, coi count mismatch"));
}

// -----------------------------------------------------------------------------
void PartitionData::addSubPixToCoverage(PartitionCell *cell)
{
	DEBUG_ASSERTCRASH(m_coiInUseCount < m_coiArrayCount, ("not enough cois allocated for this object"));
	if (cell)
	{
		// see if we already have a coi for this cell.
		CellAndObjectIntersection *coi = m_coiArray;
		CellAndObjectIntersection *coiToUse = nullptr;
		for (Int i = __min(m_coiInUseCount,m_coiArrayCount); i; --i, ++coi)
		{
			if (coi->getCell() == cell)
			{
				coiToUse = coi;
				break;
			}
		}
		DEBUG_ASSERTCRASH(coiToUse != nullptr || m_coiInUseCount < m_coiArrayCount, ("not enough cois allocated for this object"));
		if (coiToUse == nullptr && m_coiInUseCount < m_coiArrayCount)
		{
			// nope, no coi for this cell, allocate a new one
			coiToUse = &m_coiArray[m_coiInUseCount++];
		}
		if (coiToUse)
		{
			coiToUse->addCoverage(cell, this);
		}
	}
}

// -----------------------------------------------------------------------------
void PartitionData::doRectFill(
	Real centerX,
	Real centerY,
	Real halfsizeX,
	Real halfsizeY,
	Real angle
)
{
	Real c = (Real)Cos(angle);
	Real s = (Real)Sin(angle);

	Real actualCellSize = ThePartitionManager->getCellSize();
	Real stepSize = actualCellSize * 0.5f; // in theory, should be getCellSize() exactly, but needs to be smaller to avoid aliasing problems
	Real ydx = s * stepSize;
	Real ydy = -c * stepSize;
	Real xdx = c * stepSize;
	Real xdy = s * stepSize;

	// GS 12-26-02  Oh my.  The step size has been fudged down to ensure that a cell is not jumped over,
	// but then the original step size was used to determine the number of steps.  This error gets
	// more pronounced on bigger buildings.  ie a 100 X 160 registers cells for a 80 x 100.  If you
	// fudge one, you have to fudge the other.
//	Real stepSizeInvTimes2 = 2.0f * ThePartitionManager->getCellSizeInv();
	Real stepSizeInvTimes2 = 2.0f * (1.0f / stepSize);

	/*
		srj fixes subtle bug: need +1 to get all of misaligned geometries accted for.
	*/
//	Int numStepsX = REAL_TO_INT_CEIL(halfsizeX * stepSizeInvTimes2) + 1;
//	Int numStepsY = REAL_TO_INT_CEIL(halfsizeY * stepSizeInvTimes2) + 1;
	Int numStepsX = REAL_TO_INT_CEIL(halfsizeX * stepSizeInvTimes2);
	Int numStepsY = REAL_TO_INT_CEIL(halfsizeY * stepSizeInvTimes2);
	// GS 12-26-02  Oh my part 2.  Now that we are no longer greatly underestimating our COI imprint,
	// we don't need this fudge.  The fudge is not in the calcMaxCOIForShape, so it just results in
	// good COIs getting dropped since we didn't allocate enough to handle this fudge.

	Real tl_x = centerX - halfsizeX*c - halfsizeY*s;
	Real tl_y = centerY + halfsizeY*c - halfsizeX*s;

	for (Int iy = 0; iy < numStepsY; ++iy, tl_x += ydx, tl_y += ydy)
	{
		Real x = tl_x;
		Real y = tl_y;
		for (Int ix = 0; ix < numStepsX; ++ix, x += xdx, y += xdy)
		{
			Int cellx, celly;
			ThePartitionManager->worldToCell(x, y, &cellx, &celly);
			PartitionCell *cell = ThePartitionManager->getCellAt(cellx, celly);	// might be null if off the edge
			if (cell)
			{
				addSubPixToCoverage(cell);
			}
		}
	}

}

// -----------------------------------------------------------------------------
void PartitionData::hLineCircle(Int x1, Int x2, Int y)
{
	for (Int x = x1; x <= x2; ++x)
	{
		PartitionCell* cell = ThePartitionManager->getCellAt(x, y);
		if (cell)
		{
      addSubPixToCoverage(cell);
		}
	}
}

// -----------------------------------------------------------------------------
// Marks all partition cells that intersect a circle of the given center and radius
// as covered by this object using a variation of the midpoint circle algorithm.
void PartitionData::doCircleFill(
	Real centerX,
	Real centerY,
	Real radius
)
{
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("expected no coi in use here"));

	Int cellCenterX, cellCenterY;
	ThePartitionManager->worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);

	Int cellRadius = ThePartitionManager->worldToCellDist(radius);
	if (cellRadius < 1)
		cellRadius = 1;

	Int y = cellRadius - 1;
	Int dec = 3 - 2*cellRadius;

#if RETAIL_COMPATIBLE_CRC
	// Cell coverage diverges at radii >= 240 between algorithms.
	Int end = cellRadius - 1;
	Int& endRef = (cellRadius < 240) ? y : end;
	for (Int x = 0; x <= endRef; ++x)
#else
	for (Int x = 0; x <= y; ++x)
#endif
	{
		hLineCircle(cellCenterX - x, cellCenterX + x, cellCenterY + y);
		hLineCircle(cellCenterX - x, cellCenterX + x, cellCenterY - y);
		hLineCircle(cellCenterX - y, cellCenterX + y, cellCenterY + x);
		hLineCircle(cellCenterX - y, cellCenterX + y, cellCenterY - x);

		if (dec >= 0)
		{
			dec += (1 - y) << 2;
			--y;
		}
		dec += (x << 2) + 6;
	}
}

static Bool doesCircleOverlapCell(Real centerX, Real centerY, Real radius, Real cellX, Real cellY, Real cellSize)
{
	Real closestX = std::max(cellX, std::min(centerX, cellX + cellSize));
	Real closestY = std::max(cellY, std::min(centerY, cellY + cellSize));
	Real distX = centerX - closestX;
	Real distY = centerY - closestY;

	return (sqr(distX) + sqr(distY)) < sqr(radius);
}

void PartitionData::doCircleFillPrecise(Real centerX, Real centerY, Real radius)
{
	Int minCellX, minCellY, maxCellX, maxCellY;
	ThePartitionManager->worldToCell(centerX - radius, centerY - radius, &minCellX, &minCellY);
	ThePartitionManager->worldToCell(centerX + radius, centerY + radius, &maxCellX, &maxCellY);

	Real cellSize = ThePartitionManager->getCellSize();

	for (Int x = minCellX; x <= maxCellX; ++x)
	{
		for (Int y = minCellY; y <= maxCellY; ++y)
		{
			Real cellWorldX = x * cellSize;
			Real cellWorldY = y * cellSize;

			if (doesCircleOverlapCell(centerX, centerY, radius, cellWorldX, cellWorldY, cellSize))
			{
				PartitionCell* cell = ThePartitionManager->getCellAt(x, y);
				if (cell)
				{
					addSubPixToCoverage(cell);
				}
			}
		}
	}
}

// -----------------------------------------------------------------------------
void PartitionData::doSmallFill(
	Real centerX,
	Real centerY,
	Real radius
)
{
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("expected no coi in use here"));

	Real halfCellSize = ThePartitionManager->getCellSize() * 0.5f;
	if (radius > halfCellSize)
	{
		DEBUG_CRASH(("object is too large to use a 'small' geometry, truncating size to cellsize"));
		radius = halfCellSize;
	}

	Int cx1, cy1, cx2, cy2;
	ThePartitionManager->worldToCell(centerX - radius, centerY - radius, &cx1, &cy1);
	ThePartitionManager->worldToCell(centerX + radius, centerY + radius, &cx2, &cy2);

	DEBUG_ASSERTCRASH(IABS(cx2-cx1)<=1,("bad cx"));
	DEBUG_ASSERTCRASH(IABS(cy2-cy1)<=1,("bad cy"));

	for (Int x = cx1; x <= cx2; x++)
	{
		for (Int y = cy1; y <= cy2; y++)
		{
			PartitionCell *cell = ThePartitionManager->getCellAt(x, y);
			if (cell)
			{
				m_coiArray[m_coiInUseCount++].addCoverage(cell, this);
			}
		}
	}

	#ifdef INTENSE_DEBUG
	for (int i = 0; i < m_coiInUseCount; i++)
	{
		for (int j = 0; j < i; j++)
		{
			DEBUG_ASSERTCRASH(m_coiArray[i].getCell() != m_coiArray[j].getCell(), ("dup cells"));
		}
	}
	#endif
}

//-----------------------------------------------------------------------------
// GeneralsX: Can this object be left out of a contact pair it is too far from?
// Only an object that nothing in processContactList() can slide sideways while it stays in the partition:
//  * not PARACHUTING and not a container: PhysicsBehavior's parachute bounce moves exactly a parachuting
//    object or the container holding one (PhysicsUpdate.cpp, objToBounce->setPosition);
//  * not contained: a rider that stays in the partition (an Overlord's gattling or propaganda tower) is
//    moved to the exit bone or scattered when its container dies (TransportContain::onRemoving);
//  * not infantry: infantry walking into a NON-enclosing garrison (the America Fire Base, the only
//    GarrisonContain with IsEnclosingContainer = No in INIZH.big and the community patch) enter it in
//    OpenContain::onCollide and are put on a fire point while staying in the partition
//    (GarrisonContain.cpp). A mod that lets vehicles do that would reopen this path.
UnsignedInt g_gxContactSkipped = 0;	// pairs skipped, read only by the GX_TICKMS diagnostic (GameEngine.cpp)

static inline Bool contactSkipCandidate(const Object *o)
{
	return !o->testStatus(OBJECT_STATUS_PARACHUTING) && o->getContain() == nullptr
		&& o->getContainedBy() == nullptr && !o->isKindOf(KINDOF_INFANTRY);
}

// The farthest a collide test can reach from an object's position in XY. Box: its corner,
// sqrt(major^2 + minor^2), the bounding circle. Sphere and cylinder: xy_collideTest_Rect_Circle tests
// them against a box as a SQUARE of half-side majorRadius turned by the object's angle, whose corner
// is majorRadius * sqrt(2) out. Taking the larger for every type covers all nine collide procs.
static inline Real contactReachXY(const GeometryInfo& g)
{
	const Real square = g.getMajorRadius() * 1.4143f;
	const Real circle = g.getBoundingCircleRadius();
	return square > circle ? square : circle;
}

// True only when the pair cannot pass collidesWith(): every collide proc (and its Z pre-test, which can
// only reject more) needs the XY centres within reachA + reachB. The slack absorbs float rounding and
// the moves a candidate can still make before processContactList() tests the pair: SlowDeathBehavior's
// fling lifts a dying object by up to 1 in Z and turns it (SlowDeathBehavior.cpp beginSlowDeath), which
// changes neither XY distance nor reach, and a hijack's capture (ConvertToHijackedVehicleCrateCollide ->
// setTeam -> onCapture aiIdle -> StateMachine::clear -> AIAttackApproachTargetState::onExit, AIStates.cpp)
// snaps a unit to its goal within sqrt(0.125) * PATHFIND_CELL_SIZE = 3.536 (review finding F2).
// aReach is contactReachXY(a's geometry), hoisted out of the cell loop by the caller.
static constexpr Real CONTACT_SKIP_SLACK = 4.0f;
static_assert(CONTACT_SKIP_SLACK * CONTACT_SKIP_SLACK > 0.125f * PATHFIND_CELL_SIZE_F * PATHFIND_CELL_SIZE_F,
	"the slack must cover the AIAttackApproachTargetState::onExit goal snap");
static inline Bool contactClearlyApart(const Coord3D *pa, Real aReach, const Object *b)
{
	const Coord3D *pb = b->getPosition();
	const Real dx = pa->x - pb->x, dy = pa->y - pb->y;
	const Real r = aReach + contactReachXY(b->getGeometryInfo()) + CONTACT_SKIP_SLACK;
	return dx*dx + dy*dy > r*r;
}

//-----------------------------------------------------------------------------
void PartitionData::addPossibleCollisions(PartitionContactList *ctList)
{
// actually, we do occasionally want to detect collisions of dead AIs.
// e.g., dead technicals flying thru the air should check for collisions with
// immobile structures, because otherwise they pass thru them, which looks dopey.
// if preventing dead-ai-collisions is necessary to fix something else, please
// ensure that the above case is cool before signing off. (srj)
#ifdef NOPE_READ_THE_COMMENT
	// dead objects never collide with things...
 	AIUpdateInterface *ai = getObject()->getAIUpdateInterface();
 	if (ai && ai->isAiInDeadState())
	{
		return;
	}
#endif

	//DEBUG_LOG(("adding possible collision for %s",getObject()->getTemplate()->getName().str()));

	// GeneralsX: Pairs with a projectile in them that cannot touch never enter the
	// contact list. In a Toxin Tractor fight every stream projectile shares partition cells with the
	// tractors and the other projectiles around it, and each such pair cost a hash walk and a pooled node
	// here, only for processContactList() to drop it on collidesWith() (measured on the 180-tractor
	// fixture: ~87% of projectile x vehicle attempts never touch). Option 3 of
	//
	//
	// Why the outcome is identical, not just similar: processContactList() does nothing at all with a
	// pair whose collidesWith() is false (it `continue`s before onCollide and before makeDirty), so
	// dropping such a pair here changes nothing PROVIDED collidesWith() would still be false when that
	// loop reaches it. Between here and there the code that runs is friend_updateCellsTouched() for
	// other dirty modules (moves nothing) and earlier pairs' onCollide() with everything it reaches
	// (detonation, radius damage, deaths). What that can do to an EXISTING object in the partition:
	// the parachute bounce, a dying container moving a rider that stays in the partition, and infantry
	// entering a non-enclosing garrison (all three excluded by contactSkipCandidate), a SlowDeath fling
	// (Z and angle only, covered by contactClearlyApart), ActiveBody::setGeometryInfoZ on a damaged
	// structure (Z only), a hijack's goal snap (within the slack, contactClearlyApart), or destroying it
	// (destroyObject is deferred to processDestroyList, after this pass, so the object keeps its place in
	// the partition and its position until then). A train shoves a live victim up to ~10 units sideways
	// in RailroadBehavior::onCollide, so the skip is off while any RailroadBehavior exists (review finding
	// F1): a live count, so a later game without trains gets the skip back. Every other
	// position or geometry write reachable from there lands on a NEW object or on one that enters an
	// enclosing container, which unregisters it and nulls its nodes. (Adversarial reviews,
	// 24/09/2026, found the fling, the rider, the Fire Base, the train and the hijack snap.)
	// The test is symmetric, so a pair is skipped from both sides or neither, and a pair that is kept is
	// added at the same first attempt as before: the order of every pair that reaches onCollide is
	// unchanged.
	const Object *thisObj = getObject();
	const Bool thisIsProjectile = thisObj->isKindOf(KINDOF_PROJECTILE);
	// GX_CONTACT_SKIP_AB=1 (measurement only) applies the skip on even logic frames alone, so one run
	// holds its own paired control: the skip leaves the simulation unchanged, so it may be switched per
	// frame, and even and odd ticks then run the same fight on the same core at the same moment.
	// scripts/test/stream-weapon-ab.sh compares the two halves.
	// It is inert whenever the game has more than one network player (review finding F5).
	static const Bool s_skipAB = getenv("GX_CONTACT_SKIP_AB") != nullptr;
	const Bool thisIsCandidate = RailroadBehavior::getLiveCount() == 0 && contactSkipCandidate(thisObj)
		&& (!s_skipAB || (TheGameLogic->getFrame() & 1) == 0
			|| (TheNetwork != nullptr && TheNetwork->getNumPlayers() > 1));
	const Coord3D *thisPos = thisObj->getPosition();
	const Real thisReach = contactReachXY(thisObj->getGeometryInfo());

	CellAndObjectIntersection *myCoi = m_coiArray;
	for (Int i = m_coiInUseCount; i > 0; --i, ++myCoi)
	{
		PartitionCell *cell = myCoi->getCell();
		if (cell->getCoiCount() < 2)
			continue;

		for (CellAndObjectIntersection *coi = cell->getFirstCoiInCell(); coi; coi = coi->getNextCoi())
		{
			PartitionData *that = coi->getModule();
			if (this != that)
			{
				const Object *thatObj = that->getObject();
				if (thisIsCandidate && thatObj != nullptr
					&& (thisIsProjectile || thatObj->isKindOf(KINDOF_PROJECTILE))
					&& contactClearlyApart(thisPos, thisReach, thatObj) && contactSkipCandidate(thatObj))
				{
					++g_gxContactSkipped;
					continue;
				}
				ctList->addToContactList(this, that);
			}
		}
	}
}

//-----------------------------------------------------------------------------
Bool PartitionData::collidesWith(const PartitionData *that, CollideLocAndNormal *cinfo) const
{
	const Object *thisObj = this->getObject();
	const Object *thatObj = that->getObject();

	if( thisObj->isKindOf( KINDOF_NO_COLLIDE )  ||  thatObj->isKindOf( KINDOF_NO_COLLIDE ) )
		return FALSE; // A collision extent of zero size is still a point and can collide, but we don't always want to.

	CollideInfo thisInfo(thisObj->getPosition(), thisObj->getGeometryInfo(), thisObj->getOrientation());
	CollideInfo thatInfo(thatObj->getPosition(), thatObj->getGeometryInfo(), thatObj->getOrientation());

	// invariant for all geometries: first do z collision check.
	Real thisTop = thisInfo.position.z + thisInfo.geom.getMaxHeightAbovePosition();
	Real thisBot = thisInfo.position.z - thisInfo.geom.getMaxHeightBelowPosition();
	Real thatTop = thatInfo.position.z + thatInfo.geom.getMaxHeightAbovePosition();
	Real thatBot = thatInfo.position.z - thatInfo.geom.getMaxHeightBelowPosition();
	if (thisTop >= thatBot && thisBot <= thatTop)
	{
		GeometryType thisGeom = thisObj->getGeometryInfo().getGeomType();
		GeometryType thatGeom = thatObj->getGeometryInfo().getGeomType();

		//
		// NOTE: This assumes geometry enumerations that start at GEOMETRY_FIRST AND depends on the
		// order in which they appear in the enum list
		//
		CollideTestProc collideProc = theCollideTestProcs[ (thisGeom - GEOMETRY_FIRST) * GEOMETRY_NUM_TYPES + (thatGeom - GEOMETRY_FIRST) ];
		return (*collideProc)(&thisInfo, &thatInfo, cinfo);
	}
	else
	{
		// no z-intersection -> no collision.
		return false;
	}
}

//-----------------------------------------------------------------------------
/* See if thisObj collides with geom at pos & angle. */
Bool PartitionManager::geomCollidesWithGeom(const Coord3D* pos1,
		const GeometryInfo& geom1,
		Real angle1,
		const Coord3D* pos2,
		const GeometryInfo& geom2,
		Real angle2) const
{
	CollideInfo thisInfo(pos1, geom1, angle1);
	CollideInfo thatInfo(pos2, geom2, angle2);

	// invariant for all geometries: first do z collision check.
	if (thisInfo.position.z + thisInfo.geom.getMaxHeightAbovePosition() >= thatInfo.position.z &&
			thisInfo.position.z <= thatInfo.position.z + thatInfo.geom.getMaxHeightAbovePosition())
	{
		GeometryType thisGeom = geom1.getGeomType();
		GeometryType thatGeom = geom2.getGeomType();

		//
		// NOTE: This assumes geometry enumerations that start at GEOMETRY_FIRST AND depends on the
		// order in which they appear in the enum list
		//
		CollideTestProc collideProc = theCollideTestProcs[ (thisGeom - GEOMETRY_FIRST) * GEOMETRY_NUM_TYPES + (thatGeom - GEOMETRY_FIRST) ];
		CollideLocAndNormal cloc;
		return (*collideProc)(&thisInfo, &thatInfo, &cloc);
	}
	else
	{
		// no z-intersection -> no collision.
		return false;
	}
}

//-----------------------------------------------------------------------------
void PartitionData::updateCellsTouched()
{
	GeometryType geom;
	Bool isSmall;
	Coord3D pos;
	Real angle,majorRadius,minorRadius;


	Object *obj = getObject();

	if (obj)
	{
		//we have no object using this PartitionData but we still have a GhostObject so copy its data.
		geom = obj->getGeometryInfo().getGeomType();
		isSmall = obj->getGeometryInfo().getIsSmall();
		pos = *(obj->getPosition());
		angle = obj->getOrientation();
		majorRadius = obj->getGeometryInfo().getMajorRadius();
		minorRadius = obj->getGeometryInfo().getMinorRadius();
		ThePartitionManager->friend_noteObjectRadius(obj->getGeometryInfo().getBoundingCircleRadius());	// GeneralsX: visitObjectsNearToFar's bound
	}
	else if (m_ghostObject)
	{
		geom = m_ghostObject->getGeometryType();
		isSmall = m_ghostObject->getGeometrySmall();
		pos = *m_ghostObject->getParentPosition();
		angle = m_ghostObject->getParentAngle();
		majorRadius = m_ghostObject->getGeometryMajorRadius();
		minorRadius = m_ghostObject->getGeometryMinorRadius();
	}
	else
	{
		DEBUG_CRASH(("must be attached to an Object here"));
		return;
	}

	removeAllTouchedCells();
	m_fillPos = pos;	// GeneralsX: see friend_noteSubThresholdMove
	if (isSmall)
	{
		doSmallFill(pos.x, pos.y, majorRadius);
	}
	else
	{
		switch(geom)
		{
			case GEOMETRY_SPHERE:
			case GEOMETRY_CYLINDER:
			{
#if RETAIL_COMPATIBLE_CRC || RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM
				doCircleFill(pos.x, pos.y, majorRadius);
#else
				// TheSuperHackers @bugfix Stubbjax 29/01/2026 Use precise circle fill to improve
				// collision accuracy, most notably for objects with geometry radii >= 20 and < 40.
				doCircleFillPrecise(pos.x, pos.y, majorRadius);
#endif
				break;
			}

			case GEOMETRY_BOX:
			{
				doRectFill(pos.x, pos.y, majorRadius, minorRadius, angle);
				break;
			}
		};
	}

	Int currentCellIndexX, currentCellIndexY;
	ThePartitionManager->worldToCell( pos.x, pos.y, &currentCellIndexX, &currentCellIndexY );
	const PartitionCell *currentCell = ThePartitionManager->getCellAt( currentCellIndexX, currentCellIndexY );
	if(obj && currentCell != m_lastCell )
	{
		// To not expose PartitionCells, he will think in terms of points.  He will
		// unlook at a point and look at the new point.  We do the rounding and the
		// changing into PartitionCells
		obj->onPartitionCellChange();
		m_lastCell = currentCell;
	}

	// if we have moved, our shroudedness status might be different for all players,
	// so it must all be invalidated.
	invalidateShroudedStatusForAllPlayers();

#ifdef INTENSE_DEBUG
	for (Int i = 0; i < m_coiInUseCount; i++)
	{
		for (Int j = 0; j < i; j++)
		{
			if (m_coiArray[i].getCell() == m_coiArray[j].getCell())
			{
				DEBUG_CRASH(("dup cells in COI array, this is bad"));
			}
		}
	}
#endif

}

//-----------------------------------------------------------------------------
void PartitionData::invalidateShroudedStatusForPlayer(Int playerIndex)
{
#ifndef DISABLE_INVALID_PREVENTION
	if (m_shroudedness[playerIndex] != OBJECTSHROUD_INVALID && m_shroudedness[playerIndex] != OBJECTSHROUD_INVALID_BUT_PREVIOUS_VALID)
#endif
		m_shroudedness[playerIndex] = OBJECTSHROUD_INVALID;
}

//-----------------------------------------------------------------------------
void PartitionData::invalidateShroudedStatusForAllPlayers()
{
	for (Int i = 0; i < MAX_PLAYER_COUNT; i++)
	{
		invalidateShroudedStatusForPlayer(i);
	}
}

#if defined(RTS_DEBUG)
static AsciiString theObjName;
#endif

//-----------------------------------------------------------------------------
Int PartitionData::calcMaxCoiForShape(GeometryType geom, Real majorRadius, Real minorRadius, Bool isSmall)
{
	Int result;


  // THis is commented out, since some cases od big extets labeled small seem to be escaping.
  //M Lorenzen 8/26/03
//	if (isSmall)
//	{
//		#if defined(RTS_DEBUG)
//		Int chk = calcMaxCoiForShape(geom, majorRadius, minorRadius, false);
//		DEBUG_ASSERTCRASH(chk <= 4, ("Small objects should be <= 4 cells, but I calced %s as %d",theObjName.str(),chk));
//		#endif
//		result = 4;
//	}
//	else
	{
		switch(geom)
		{
			case GEOMETRY_SPHERE:
			case GEOMETRY_CYLINDER:
			{
				// note that majorRadius is a radius, not a diameter.
				// this actually allocates a few too many, but that's ok.
				Int cells = ThePartitionManager->worldToCellDist(majorRadius*2) + 1;
				result = cells * cells;
#if !RETAIL_COMPATIBLE_CRC
				break;
#endif
			}
			case GEOMETRY_BOX:
			{
				Real diagonal = (Real)(sqrtf(majorRadius*majorRadius + minorRadius*minorRadius));
				Int cells = ThePartitionManager->worldToCellDist(diagonal*2) + 1;
				result = cells * cells;
				break;
			}
			default:
				return 4;
		};
		static_assert(GEOMETRY_NUM_TYPES == 3, "GEOMETRY_NUM_TYPES has changed");
	}
	if (result < 4)
		result = 4;
	return result;
}

//-----------------------------------------------------------------------------
Int PartitionData::calcMaxCoiForObject()
{
	Object *obj = getObject();
	DEBUG_ASSERTCRASH(obj != nullptr, ("must be attached to an Object here 2"));

	GeometryType geom = obj->getGeometryInfo().getGeomType();
	Real majorRadius = obj->getGeometryInfo().getMajorRadius();
	Real minorRadius = obj->getGeometryInfo().getMinorRadius();
	Bool isSmall = obj->getGeometryInfo().getIsSmall();
#if defined(RTS_DEBUG)
theObjName = obj->getTemplate()->getName();
#endif
	return calcMaxCoiForShape(geom, majorRadius, minorRadius, isSmall);
}

//-----------------------------------------------------------------------------
// GeneralsX: see PartitionManager::m_maxDrift. Nothing to note while it has no cells or
// while its cells are about to be refilled anyway (it is on the dirty list, which the early stop checks by position).
void PartitionData::friend_noteSubThresholdMove(const Coord3D *pos)
{
	if (m_coiInUseCount == 0 || isInNeedOfUpdatingCells())
		return;
	ThePartitionManager->friend_noteDrift(sqrtf(sqr(pos->x - m_fillPos.x) + sqr(pos->y - m_fillPos.y)));
}

//-----------------------------------------------------------------------------
void PartitionData::makeDirty(Bool needToUpdateCells)
{
	//DEBUG_LOG(("makeDirty for pd %08lx obj %08lx",this,m_object));
	if (!ThePartitionManager->isInListDirtyModules(this))
	{
		if (needToUpdateCells)
			m_dirtyStatus = NEED_CELL_UPDATE_AND_COLLISION_CHECK;
		else if (m_dirtyStatus == NOT_DIRTY)
			m_dirtyStatus = NEED_COLLISION_CHECK;
		ThePartitionManager->prependToDirtyModules(this);
	}
	else if (needToUpdateCells)
	{
		// If I am in the list already, I may be in there for a half update, so if my
		// needs have increased I'll upgrade what I want.
		m_dirtyStatus = NEED_CELL_UPDATE_AND_COLLISION_CHECK;
	}
}

//-----------------------------------------------------------------------------
void PartitionData::allocCoiArray()
{
	DEBUG_ASSERTCRASH(m_coiArrayCount == 0 && m_coiArray == nullptr, ("hmm, coi should probably be null here"));
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("hmm, coi count mismatch"));
	m_coiArrayCount = calcMaxCoiForObject();
	m_coiArray = MSGNEW("PartitionManager_COI") CellAndObjectIntersection[m_coiArrayCount];	// may throw!
	m_coiInUseCount = 0;
	makeDirty(true);
}

//-----------------------------------------------------------------------------
void PartitionData::freeCoiArray()
{
	delete [] m_coiArray;	// yes, it's OK to call this on null...
	m_coiArray = nullptr;
	m_coiArrayCount = 0;
	m_coiInUseCount = 0;
	makeDirty(true);
}

//-----------------------------------------------------------------------------
void PartitionData::attachToObject(Object* object)
{

	// remember who contains us
	m_object = object;
	m_ownerTag = calcOwnerTag(object);	// GeneralsX: no COI is in use yet (asserted below)

	// we only snapshot things that are immobile and have something to draw.  Don't need ghostobjects for others.
	if (object->isKindOf(KINDOF_IMMOBILE))
	{	const ThingTemplate *tmplate=object->getTemplate();
		const ModuleInfo &drawMod=tmplate->getDrawModuleInfo();
		//Objects with default draw modules usually don't need ghostobjects so skip them to save memory.
		Bool makeGhostObject=TRUE;
		for (Int i=0; i<drawMod.getCount(); i++)
		{
			if (strcmp(drawMod.getNthName(i).str(),"W3DDefaultDraw") == 0)
			{	makeGhostObject=FALSE;
				break;
			}
		}
		if (makeGhostObject)
			m_ghostObject = TheGhostObjectManager->addGhostObject(object, this);
	}

	//DEBUG_LOG(("attach pd for pd %08lx obj %08lx",this,m_object));

	// (re)calc maxCoi and (re)alloc cois
	DEBUG_ASSERTCRASH(m_coiArrayCount == 0 && m_coiArray == nullptr, ("hmm, coi should probably be null here"));
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("hmm, coi count mismatch"));
	freeCoiArray();
	allocCoiArray();	// may throw!

	if (object)
		object->friend_setPartitionData(this);
	makeDirty(true);
}

//-----------------------------------------------------------------------------
void PartitionData::detachFromObject()
{
	// this is a little hokey... if we are in the midst of processing the contact
	// list, we have to ensure that any potential collisions get removed from that
	// list, since we are about to go bye-bye. ugly, but effective. (srj)
	//
	// note 2: we used to do this in our dtor, but it's too late then, since we require
	// our Object to still be present in order to figure out what to remove.
	// so do it here, since detaching it should nuke it from the obj list anyway. (srj)
	if (TheContactList)
		TheContactList->removeSpecificPartitionData(this);

	if (m_object)
		m_object->friend_setPartitionData(nullptr);
	removeAllTouchedCells();
	freeCoiArray();

	//DEBUG_LOG(("detach pd for pd %08lx obj %08lx",this,m_object));

	// no longer attached to object
	m_object = nullptr;
	TheGhostObjectManager->removeGhostObject(m_ghostObject);
	m_ghostObject = nullptr;
}

//-----------------------------------------------------------------------------
void PartitionData::attachToGhostObject(GhostObject* object)
{

	// remember who contains us
	m_object = nullptr;	//it's only attached to a ghost object, no parent object.
	m_ghostObject = object;

	// (re)calc maxCoi and (re)alloc cois
	DEBUG_ASSERTCRASH(m_coiArrayCount == 0 && m_coiArray == nullptr, ("hmm, coi should probably be null here"));
	DEBUG_ASSERTCRASH(m_coiInUseCount == 0, ("hmm, coi count mismatch"));
	freeCoiArray();

	m_coiArrayCount = calcMaxCoiForShape(object->getGeometryType(), object->getGeometryMajorRadius(), object->getGeometryMinorRadius(),object->getGeometrySmall());
	m_coiArray = MSGNEW("PartitionManager_COI") CellAndObjectIntersection[m_coiArrayCount];	// may throw!
	m_coiInUseCount = 0;
	makeDirty(true);

	if (m_ghostObject)
		m_ghostObject->updateParentObject(nullptr,this);
}

//-----------------------------------------------------------------------------
void PartitionData::detachFromGhostObject()
{
	// this is a little hokey... if we are in the midst of processing the contact
	// list, we have to ensure that any potential collisions get removed from that
	// list, since we are about to go bye-bye. ugly, but effective. (srj)
	//
	// note 2: we used to do this in our dtor, but it's too late then, since we require
	// our Object to still be present in order to figure out what to remove.
	// so do it here, since detaching it should nuke it from the obj list anyway. (srj)
	if (TheContactList)
		TheContactList->removeSpecificPartitionData(this);

	removeAllTouchedCells();
	freeCoiArray();

	//DEBUG_LOG(("detach pd for pd %08lx obj %08lx",this,m_object));

	// no longer attached to object
	m_object = nullptr;
	m_ghostObject = nullptr;
}

//-----------------------------------------------------------------------------
inline UnsignedInt hash2ints(Int a, Int b)
{
	// do it this way so that [a,b] always hashes to the same value as [b,a].
	// this is unsophisticated but reasonable, since all ObjectIDs will
	// quite likely be well below 65536...
	if (a < b)
	{
		return (a<<16)+b;
	}
	else
	{
		return (b<<16)+a;
	}
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
std::vector<PartitionContactList::Contact> PartitionContactList::s_contacts;
std::vector<PartitionContactList::Slot> PartitionContactList::s_slots;
UnsignedInt PartitionContactList::s_slotMask = 0;
Int PartitionContactList::s_live = 0;

//-----------------------------------------------------------------------------
inline UnsignedInt PartitionContactList::firstSlot( const PartitionData *lo, const PartitionData *hi )
{
	// any mix of the two addresses will do: it only spreads the pairs over the table (see the class)
	uint64_t k = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(lo)) * 0x9E3779B97F4A7C15ull;
	k ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hi));
	k *= 0xC2B2AE3D27D4EB4Full;
	return static_cast<UnsignedInt>(k >> 32) & s_slotMask;
}

//-----------------------------------------------------------------------------
void PartitionContactList::grow()
{
	size_t n = s_slots.empty() ? (static_cast<size_t>(1) << MIN_SLOT_BITS) : s_slots.size() * 2;
	while ((s_contacts.size() + 1) * 2 > n)
		n *= 2;
	s_slots.assign(n, Slot());
	s_slotMask = static_cast<UnsignedInt>(n - 1);
	// re-place this frame's live pairs; a removed pair needs no tombstone in the new table (it matches nothing either way)
	for (size_t c = 0; c < s_contacts.size(); ++c)
	{
		Contact &ct = s_contacts[c];
		if (ct.m_obj == nullptr || ct.m_other == nullptr)
		{
			ct.m_slot = NO_SLOT;
			continue;
		}
		PartitionData *lo = std::less<PartitionData*>()(ct.m_obj, ct.m_other) ? ct.m_obj : ct.m_other;
		PartitionData *hi = (lo == ct.m_obj) ? ct.m_other : ct.m_obj;
		UnsignedInt i = firstSlot(lo, hi);
		while (s_slots[i].m_lo != nullptr)
			i = (i + 1) & s_slotMask;
		s_slots[i].m_lo = lo;
		s_slots[i].m_hi = hi;
		ct.m_slot = i;
	}
}

//-----------------------------------------------------------------------------
void PartitionContactList::addToContactList( PartitionData *obj, PartitionData *other )
{
	if (obj == other || obj == nullptr || other == nullptr)
		return;

	if (obj->getObject() == nullptr || other->getObject() == nullptr)
		return;

	if ((s_contacts.size() + 1) * 2 > s_slots.size())
		grow();

	// make sure given hit has not already been recorded (in either order)
	PartitionData *lo = std::less<PartitionData*>()(obj, other) ? obj : other;
	PartitionData *hi = (lo == obj) ? other : obj;
	UnsignedInt i = firstSlot(lo, hi);
	for (;;)
	{
		const Slot &sl = s_slots[i];
		if (sl.m_lo == nullptr)
			break;
		if (sl.m_lo == lo && sl.m_hi == hi)
			return;	// already noted
		i = (i + 1) & s_slotMask;
	}

	// new hit: stored first, the key published after, so a failed push_back leaves no key without its contact
	Contact ct;
	ct.m_obj = obj;
	ct.m_other = other;
	ct.m_slot = i;
	s_contacts.push_back(ct);
	s_slots[i].m_lo = lo;
	s_slots[i].m_hi = hi;
}

//-----------------------------------------------------------------------------
void PartitionContactList::removeSpecificPartitionData(PartitionData* data)
{
	for (size_t c = 0; c < s_contacts.size(); ++c)
	{
		Contact &ct = s_contacts[c];
		if (ct.m_obj == data || ct.m_other == data)
		{
			ct.m_obj = nullptr;
			ct.m_other = nullptr;
			if (ct.m_slot != NO_SLOT)
			{
				s_slots[ct.m_slot].m_lo = tombstone();
				s_slots[ct.m_slot].m_hi = tombstone();
			}
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionContactList::resetContactList()
{
	// every slot in use this frame belongs to one pair of s_contacts, so clearing those leaves the table empty
	for (size_t c = 0; c < s_contacts.size(); ++c)
	{
		if (s_contacts[c].m_slot != NO_SLOT)
			s_slots[s_contacts[c].m_slot] = Slot();
	}
	s_contacts.clear();
}

//-----------------------------------------------------------------------------
void PartitionContactList::processContactList()
{
	// newest first: the order of the LIFO list this array replaces (see the class). Indexed, not iterated: an
	// onCollide() below can reach removeSpecificPartitionData(), which nulls pairs further down in place.
	for (size_t c = s_contacts.size(); c-- > 0; )
	{
		PartitionData *cdObj = s_contacts[c].m_obj;
		PartitionData *cdOther = s_contacts[c].m_other;
		if (cdObj == nullptr || cdOther == nullptr)
			continue;

		// we know that their partitions overlap; determine if they REALLY collide
		// before proceeding...
		CollideLocAndNormal cinfo;
		if (!cdObj->friend_collidesWith(cdOther, &cinfo))
			continue;

		Object* obj = cdObj->getObject();
		Object* other = cdOther->getObject();

		if( obj->getStatusBits().test( OBJECT_STATUS_NO_COLLISIONS ) ||
				other->getStatusBits().test( OBJECT_STATUS_NO_COLLISIONS ) )
			continue;

		DEBUG_ASSERTCRASH(!(obj->isKindOf(KINDOF_IMMOBILE) && other->isKindOf(KINDOF_IMMOBILE)),
			("we should never have collisions between two immobile things reported"));

		// the onCollide() calls can remove the object(s) from the partition mgr,
		// thus destroying the partitiondata for 'em. go ahead and null these out here
		// so we won't be tempted to use 'em (since they might be bogus).
		s_contacts[c].m_obj = nullptr;
		s_contacts[c].m_other = nullptr;
		if (s_contacts[c].m_slot != NO_SLOT)
		{
			// as removeSpecificPartitionData: the old node's nulled fields were its chain key, so it matched nothing
			s_slots[s_contacts[c].m_slot].m_lo = tombstone();
			s_slots[s_contacts[c].m_slot].m_hi = tombstone();
		}

		obj->onCollide(other, &cinfo.loc, &cinfo.normal);
		flipCoord3D(&cinfo.normal);

 		//Before checking the "other" case, make sure that the previous collision didn't
 		//absorb him. This becomes a conflict for pilots giving veterancy to transports
 		//and pilots entering transports. Both would occur if these isDestroyed checks
 		//were missing.
 		if( !obj->isDestroyed() && !other->isDestroyed() )
 		{
 			other->onCollide(obj, &cinfo.loc, &cinfo.normal);
 		}

		//
		// NOTE: it is VERY IMPORTANT (for performance reasons) to not re-dirty immobile things.
		//
		// NOTE also that we re-get partitiondata from the object, since it might have been
		// removed from the partition system by the onCollide call...
		//
		if (!obj->isDestroyed() && obj->friend_getPartitionData() != nullptr && !obj->isKindOf(KINDOF_IMMOBILE))
		{
//DEBUG_LOG(("%d: re-dirtying collision of %s %08lx with %s %08lx",TheGameLogic->getFrame(),obj->getTemplate()->getName().str(),obj,other->getTemplate()->getName().str(),other));
			obj->friend_getPartitionData()->makeDirty(false);
		}
		if (!other->isDestroyed() && other->friend_getPartitionData() != nullptr && !other->isKindOf(KINDOF_IMMOBILE))
		{
//DEBUG_LOG(("%d: re-dirtying collision of %s %08lx with %s %08lx [other]",TheGameLogic->getFrame(),other->getTemplate()->getName().str(),other,obj->getTemplate()->getName().str(),obj));
			other->friend_getPartitionData()->makeDirty(false);
		}
	}
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionManager::PartitionManager()
{
	m_moduleList = nullptr;
	m_cellSize = m_cellSizeInv = 0.0f;
	m_cellCountX = 0;
	m_cellCountY = 0;
	m_totalCellCount = 0;
	m_cells = nullptr;
	m_cellOwnerMask = nullptr;
	m_cellOwnerCount = nullptr;
	m_maxObjectRadius = 0.0f;
	m_maxDrift = 0.0f;
	m_worldExtents.lo.zero();
	m_worldExtents.hi.zero();
	m_dirtyModules = nullptr;
	m_updatedSinceLastReset = false;
#ifdef FASTER_GCO
	m_maxGcoRadius = 0;
#endif
}

//-----------------------------------------------------------------------------
PartitionManager::~PartitionManager()
{

	shutdown();

}

//-----------------------------------------------------------------------------
#ifdef PM_CACHE_TERRAIN_HEIGHT
static void calcHeights(const Region3D& world, Real cellSize, Int x, Int y, Real& loZ, Real& hiZ)
{
	DEBUG_ASSERTCRASH(TheTerrainLogic, ("no TheTerrainLogic"));
	Real xbase = world.lo.x + (x * cellSize);
	Real ybase = world.lo.y + (y * cellSize);
	const Real ROUGH_STEP_SIZE = MAP_XY_FACTOR;	// no point in stepping smaller than grid scale
	Real numSteps = ceilf(cellSize / ROUGH_STEP_SIZE);
	Real step = cellSize / numSteps;
	loZ = HUGE_DIST;		// huge positive
	hiZ = -HUGE_DIST;		// huge negative
	for (Real yy = 0; yy <= cellSize; yy += step)
	{
		for (Real xx = 0; xx <= cellSize; xx += step)
		{
			Real h = TheTerrainLogic->getGroundHeight( xbase + xx, ybase + yy );
			if (h < loZ) loZ = h;
			if (h > hiZ) hiZ = h;
		}
	}
}
#endif

//-----------------------------------------------------------------------------
void PartitionManager::init()
{
	m_cellSize = TheGlobalData->m_partitionCellSize;
	if (m_cellSize < 1.0)
		m_cellSize = 1.0;
	// GeneralsX: The terrain-height cache below samples every cell whole, about every MAP_XY_FACTOR
	// units in Zero Hour and every 2 in Generals: the grid tiles the map, so the work is the map's area - until one cell is
	// larger than the map. A map.ini PartitionCellSize of 100,000 was 100 million samples (2.5 billion in Generals), and at
	// 1e9 the float step stopped advancing the loop. Held to 1,000 world units (NaN too); shipped data uses 40.
	const Real MAX_PARTITION_CELL_SIZE = 1000.0f;
	if (!(m_cellSize <= MAX_PARTITION_CELL_SIZE))
		m_cellSize = MAX_PARTITION_CELL_SIZE;

	m_cellSizeInv = (Real)(1.0 / m_cellSize);

	DEBUG_ASSERTCRASH(m_cells == nullptr, ("double init"));

	if (TheTerrainLogic)
	{
		TheTerrainLogic->getExtent(&m_worldExtents);
		//DEBUG_ASSERTLOG(m_worldExtents.width() > 0 && m_worldExtents.height() > 0, ("TheTerrainLogic must be loaded before ThePartitionManager"));
		// you'd think it wouldn't be legal for terrainlogic to have zero area, but apparently
		// that's what it resets itself to. let's just make the world a simple place and pretend
		// it's nonzero in area, so that we can proceed without crashing.
		if (m_worldExtents.width() < 1.0f)
			m_worldExtents.hi.x = m_worldExtents.lo.x + 1.0f;
		if (m_worldExtents.height() < 1.0f)
			m_worldExtents.hi.y = m_worldExtents.lo.y + 1.0f;
		m_cellCountX = REAL_TO_INT_CEIL(m_worldExtents.width() * m_cellSizeInv);
		m_cellCountY = REAL_TO_INT_CEIL(m_worldExtents.height() * m_cellSizeInv);
		// GeneralsX: The product used to be taken in Int, so a large enough extent wrapped it
		// (2^20 x 2^12 cells -> 0), allocated that, and the loop below then initialised cellCountX x cellCountY cells
		// far past the allocation. The map's boundaries are bounded where they are read (WorldHeightMap.cpp
		// boundariesFitHeightMap), so this only fires on a grid no map produces; refusing it beats corrupting the heap,
		// and a start refuses an ERROR_CORRUPT_FILE_FORMAT as a damaged map (GameLogic::startNewGame).
		// GeneralsX: PartitionCellSize comes from GameData, which a map.ini may override: 1 on an
		// 8,000-unit map asked for 64 million PartitionCells (gigabytes, then each initialised) before play.
		// The grid is refused past 2^20 cells. Shipped data uses 40 everywhere and no map.ini sets it (817 scanned); the
		// largest real map (802 x 860 height cells) makes 43,000 cells at 40.
		const Int64 MAX_PARTITION_CELLS = 1 << 20;
		const Int64 totalCellCount = (Int64)m_cellCountX * (Int64)m_cellCountY;
		if (m_cellCountX <= 0 || m_cellCountY <= 0 || totalCellCount > MAX_PARTITION_CELLS) {
			throw ERROR_CORRUPT_FILE_FORMAT;
		}
		m_totalCellCount = (Int)totalCellCount;
		m_cells = MSGNEW("PartitionManager_Cells") PartitionCell[m_totalCellCount];
		m_cellOwnerMask = MSGNEW("PartitionManager_CellOwnerMask") PartitionOwnerMask[m_totalCellCount];
		memset(m_cellOwnerMask, 0, sizeof(PartitionOwnerMask) * (size_t)m_totalCellCount);
		m_cellOwnerCount = MSGNEW("PartitionManager_CellOwnerCount") UnsignedInt[(size_t)m_totalCellCount * PARTITION_OWNER_TAGS];
		memset(m_cellOwnerCount, 0, sizeof(UnsignedInt) * (size_t)m_totalCellCount * PARTITION_OWNER_TAGS);
		for (Int x = 0; x < m_cellCountX; x++)
		{
			for (Int y = 0; y < m_cellCountY; y++)
			{
#ifdef PM_CACHE_TERRAIN_HEIGHT
				Real loZ, hiZ;
				calcHeights(m_worldExtents, m_cellSize, x, y, loZ, hiZ);
				getCellAt(x, y)->init(x, y, loZ, hiZ);
#else
				getCellAt(x, y)->init(x, y);
#endif
			}
		}

#ifdef FASTER_GCO
		calcRadiusVec();
#endif

	}
	else
	{
		m_cellSize = m_cellSizeInv = 0.0f;
		m_cellCountX = 0;
		m_cellCountY = 0;
		m_totalCellCount = 0;
		m_cells = nullptr;
		m_worldExtents.lo.zero();
		m_worldExtents.hi.zero();
	}
}

//-----------------------------------------------------------------------------
#ifdef DUMP_PERF_STATS
void PartitionManager::getPMStats(double& gcoTimeThisFrameTotal, double& gcoTimeThisFrameAvg)
{
	Int64 freq64;
	GetPrecisionTimerTicksPerSec(&freq64);

	double gcoTimeInMSecs = (double)s_timeInClosestObjectsThisFrame * 1000.0f / (double)freq64;

	gcoTimeThisFrameTotal = gcoTimeInMSecs;
	gcoTimeThisFrameAvg = gcoTimeInMSecs / (double)s_countInClosestObjectsThisFrame;
}
#endif

// GX_GCO_PURE_CHECK=1 (measurement only): after each object-list walk, run the ring walk too and compare the two insertion
// orders object for object - for the caller's filters, and again with no filters at all, so the order of every object in range
// is checked (a pure filter's result is a subsequence of it). The ring walk sets only its own done flags and the filters are
// pure, so the simulation reads nothing it changes. [GXGCOCHK] prints the totals every 64 calls, at each mismatch and at reset().
static const Bool s_gcoPureCheck = getenv("GX_GCO_PURE_CHECK") != nullptr;
static UnsignedInt s_gcoChkCalls = 0, s_gcoChkObjs = 0, s_gcoChkMulti = 0, s_gcoChkSameDist = 0, s_gcoChkBad = 0;
static UnsignedInt s_gcoChkAllObjs = 0, s_gcoChkAllBad = 0, s_gcoChkSameCell = 0;

static void gcoPureCheckReport(const char *why)
{
	fprintf(stderr, "[GXGCOCHK] %s f=%u calls=%u objs=%u multi=%u same_dist=%u mismatches=%u all_objs=%u all_mismatches=%u same_cell=%u\n",
		why, TheGameLogic ? TheGameLogic->getFrame() : 0u, s_gcoChkCalls, s_gcoChkObjs, s_gcoChkMulti, s_gcoChkSameDist, s_gcoChkBad,
		s_gcoChkAllObjs, s_gcoChkAllBad, s_gcoChkSameCell);
	fflush(stderr);
}

//-----------------------------------------------------------------------------
void PartitionManager::reset()
{
	if (s_gcoPureCheck && s_gcoChkCalls != 0)
		gcoPureCheckReport("reset");

#ifdef DUMP_PERF_STATS
	s_countInClosestObjects = 0;
	s_timeInClosestObjects = 0;
	s_countInClosestObjectsThisFrame = 0;
	s_timeInClosestObjectsThisFrame = 0;
	s_gcoPerfFrame = 0xffffffff;
#endif

	resetPendingUndoShroudRevealQueue();

	shutdown();
	//init();
}

//-----------------------------------------------------------------------------
void PartitionManager::shutdown()
{
	m_updatedSinceLastReset = false;
	removeAllDirtyModules();

#ifdef RTS_DEBUG
	// the above *should* remove all the touched cells (via unRegisterObject), but let's check:
	DEBUG_ASSERTCRASH( m_moduleList == nullptr, ("hmm, modules left over"));
	PartitionData *mod, *nextMod;
	for( mod = m_moduleList; mod; mod = nextMod )
	{
		nextMod = mod->getNext();
		DEBUG_ASSERTCRASH(mod->friend_getCoiInUseCount() == 0, ("hmm, coi count mismatch"));
		mod->friend_removeAllTouchedCells();
	}
#endif

#ifdef FASTER_GCO
	m_radiusVec.clear();
	m_walkRank.clear();
#endif

	resetPendingUndoShroudRevealQueue();

	delete [] m_cells;
	m_cells = nullptr;
	delete [] m_cellOwnerMask;
	m_cellOwnerMask = nullptr;
	delete [] m_cellOwnerCount;
	m_cellOwnerCount = nullptr;
	m_maxObjectRadius = 0.0f;	// GeneralsX: every object left the partition above
	m_maxDrift = 0.0f;

	m_cellSize = m_cellSizeInv = 0.0f;
	m_cellCountX = 0;
	m_cellCountY = 0;
	m_totalCellCount = 0;
	m_worldExtents.lo.zero();
	m_worldExtents.hi.zero();
}

//-----------------------------------------------------------------------------
//DECLARE_PERF_TIMER(PartitionManager_update)
void PartitionManager::update()
{
	//USE_PERF_TIMER(PartitionManager_update)
	{
#ifdef INTENSE_DEBUG
		Int cc = 0;
#endif
		if (!m_updatedSinceLastReset)
		{
			m_updatedSinceLastReset = true;
		}

		PartitionContactList ctList;
		TheContactList = &ctList;
		while (m_dirtyModules)
		{
#ifdef INTENSE_DEBUG
			++cc;
#endif

			// save it.
			PartitionData *dirty = m_dirtyModules;
			DEBUG_ASSERTCRASH(dirty->getObject() != nullptr || dirty->getGhostObject() != nullptr,
												("must be attached to an Object here %08lx",dirty));

			// get this BEFORE removing from dirty list, since that clears the
			// flag in question.
			Bool updateEm = dirty->isInNeedOfUpdatingCells();
			Bool collideEm = dirty->isInNeedOfCollisionCheck() && dirty->getObject();	//only update collisions if we have object

			// detach it from the dirty list.
			removeFromDirtyModules(dirty);

			if (updateEm)
			{
				dirty->friend_updateCellsTouched();
			}

			if (collideEm && !dirty->getObject()->isKindOf(KINDOF_IMMOBILE))
			{
				dirty->addPossibleCollisions(&ctList);
			}
		}

		ctList.processContactList();
#ifdef INTENSE_DEBUG
		DEBUG_ASSERTLOG(cc==0,("updated partition info for %d objects",cc));
#endif
		TheContactList = nullptr;

		processPendingUndoShroudRevealQueue();
	}

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugThreatMap)
	{
		if (TheGameLogic->getFrame() % TheGlobalData->m_debugThreatMapTileDuration)
		{
			Int cellCount = m_cellCountX * m_cellCountY;
			for (int i = 0; i < cellCount; ++i)
			{
				UnsignedInt threat = m_cells[i].getThreatValue(rts::getObservedOrLocalPlayer()->getPlayerIndex());
				if (threat > 0)
				{
					Real threatMul = INT_TO_REAL(threat) / TheGlobalData->m_maxDebugThreat;
					if (threatMul > 1.0f)
						threatMul = 1.0f;

					Real size = TheGlobalData->m_partitionCellSize;
					Coord3D pos = { m_cells[i].getCellX() * size,
													m_cells[i].getCellY() * size,
													0 };
					pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
					RGBColor color;
					color.red = threatMul;
					color.blue = 0.0f;
					color.green = 0.0f;

					addIcon(&pos, size, TheGlobalData->m_debugThreatMapTileDuration, color);
				}
			}
		}
	}

	if (TheGlobalData->m_debugCashValueMap)
	{
		if (TheGameLogic->getFrame() % TheGlobalData->m_debugCashValueMapTileDuration)
		{
			Int cellCount = m_cellCountX * m_cellCountY;
			for (int i = 0; i < cellCount; ++i)
			{
				UnsignedInt value = m_cells[i].getCashValue(rts::getObservedOrLocalPlayer()->getPlayerIndex());
				if (value > 0)
				{
					Real valueMul = INT_TO_REAL(value) / TheGlobalData->m_maxDebugValue;
					if (valueMul > 1.0f)
						valueMul = 1.0f;

					Real size = TheGlobalData->m_partitionCellSize;
					Coord3D pos = { m_cells[i].getCellX() * size,
													m_cells[i].getCellY() * size,
													0 };
					pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
					RGBColor color;
					color.red = 0.0f;
					color.blue = 0.0f;
					color.green = valueMul;

					addIcon(&pos, size, TheGlobalData->m_debugCashValueMapTileDuration, color);
				}
			}
		}
	}
#endif // defined(RTS_DEBUG)
}

//------------------------------------------------------------------------------
void PartitionManager::registerObject( Object* object )
{
	// sanity
	if( object == nullptr )
		return;

	// if object is already part of this system get out of here
	if( object->friend_getPartitionData() != nullptr )
	{
		DEBUG_LOG(( "Object '%s' already registered with partition manager",
								object->getTemplate()->getName().str() ));
		return;
	}

	// allocate a new module of partition data
	PartitionData *mod = newInstance( PartitionData );

	// link the module to the list in the partition manager
	mod->setPrev( nullptr );
	mod->setNext( m_moduleList );
	if( m_moduleList )
		m_moduleList->setPrev( mod );
	m_moduleList = mod;

	// add module to object
	mod->attachToObject( object );

}

//------------------------------------------------------------------------------
void PartitionManager::unRegisterObject( Object* object )
{
	// sanity
	if( object == nullptr )
		return;

	// get the partition module
	PartitionData *mod = object->friend_getPartitionData();
	if( mod == nullptr )
		return;

	GhostObject *ghost;

	// need to figure out if any players have a fogged memory of this object.
	// if so, we can't remove it from the shroud system just yet.
	if ((ghost=mod->getGhostObject()) != nullptr && mod->wasSeenByAnyPlayers() < MAX_PLAYER_COUNT)
	{
		if (TheContactList)
			TheContactList->removeSpecificPartitionData(mod);
		object->friend_setPartitionData(nullptr);
		mod->friend_setObject(nullptr);
		//Tell the ghost object that its parent is dead.
		ghost->updateParentObject(nullptr, mod);
		return;
	}

	// detach the module from the object
	mod->detachFromObject();

	// remove module from master list
	PartitionData *next = mod->getNext();
	PartitionData *prev = mod->getPrev();
	if( next )
		next->setPrev( prev );
	if( prev )
		prev->setNext( next );
	else
		m_moduleList = next;

	// delete module
	deleteInstance(mod);

}

//------------------------------------------------------------------------------
void PartitionManager::registerGhostObject( GhostObject* object)
{
	// sanity
	if( object == nullptr )
		return;

	// if object is already part of this system get out of here
	if( object->friend_getPartitionData() != nullptr )
	{
		DEBUG_LOG(( "GhostObject already registered with partition manager"));
		return;
	}

	// allocate a new module of partition data
	PartitionData *mod = newInstance( PartitionData );

	// link the module to the list in the partition manager
	mod->setPrev( nullptr );
	mod->setNext( m_moduleList );
	if( m_moduleList )
		m_moduleList->setPrev( mod );
	m_moduleList = mod;

	// add module to object
	mod->attachToGhostObject( object);
}

//------------------------------------------------------------------------------
void PartitionManager::unRegisterGhostObject( GhostObject* object )
{
	// sanity
	if( object == nullptr )
		return;

	// get the partition module
	PartitionData *mod = object->friend_getPartitionData();
	if( mod == nullptr )
		return;

	// detach the module from the object
	mod->detachFromGhostObject();

	// remove module from master list
	PartitionData *next = mod->getNext();
	PartitionData *prev = mod->getPrev();
	if( next )
		next->setPrev( prev );
	if( prev )
		prev->setNext( next );
	else
		m_moduleList = next;

	// delete module
	deleteInstance(mod);
}

/**
	Reveals the map for the given player, but does not override Shroud generation.  (Script)
*/
void PartitionManager::revealMapForPlayer( Int playerIndex )
{
	// By looking and then stopping on every cell, I clear all Passive Shroud
	// By adding a looker directly I don't hit the Ally logic of the normal look/doShroudReveal
	for (int i = 0; i < m_totalCellCount; ++i)
	{
		m_cells[i].addLooker( playerIndex );
		m_cells[i].removeLooker( playerIndex );
	}
}

/**
	Reveals the map for the given player, AND permanently disables all Shroud generation (Observer Mode).
	*/
void PartitionManager::revealMapForPlayerPermanently( Int playerIndex )
{
	// By skipping the removeLooker, I consider myself as actively looking at everything,
	// so Shroud generation will no longer function
	// By adding a looker directly I don't hit the Ally logic of the normal look/doShroudReveal
	for (int i = 0; i < m_totalCellCount; ++i)
	{
		m_cells[i].addLooker( playerIndex );
	}
}

/**
		Adds a layer of permanent blindness.  Used solely to undo the permanent reveal for debugging
	*/
void PartitionManager::undoRevealMapForPlayerPermanently( Int playerIndex )
{
	//First make sure no lingering looks will leave holes when they aren't wanted.
	processEntirePendingUndoShroudRevealQueue();

	// This will have amusing consequences if done without a preceding revealMapForPlayerPermanently.
	// Everything you own can become shrouded.
	for (int i = 0; i < m_totalCellCount; ++i)
	{
		m_cells[i].removeLooker( playerIndex );
	}
}

/**
	Resets the shroud for the given player with passive shroud (can re-explore).
	*/
void PartitionManager::shroudMapForPlayer( Int playerIndex )
{
	//First make sure no lingering looks will leave holes when they aren't wanted.
	processEntirePendingUndoShroudRevealQueue();

	// By pulsing a blast of shroud like this, we will set everything not actively looked at as Passive Shroud
	for (int i = 0; i < m_totalCellCount; ++i)
	{
		m_cells[i].addShrouder( playerIndex );
		m_cells[i].removeShrouder( playerIndex );
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::refreshShroudForLocalPlayer()
{
	// This is a drawing refresh only, and so is allowed to use the Local Player.
	TheDisplay->clearShroud();
	TheRadar->clearShroud();

	if (m_totalCellCount != 0)
	{
		const Int playerIndex = rts::getObservedOrLocalPlayer()->getPlayerIndex();
		TheRadar->beginSetShroudLevel();

		for (int i = 0; i < m_totalCellCount; ++i)
		{
			const Int x = m_cells[i].getCellX();
			const Int y = m_cells[i].getCellY();
			const CellShroudStatus status = m_cells[i].getShroudStatusForPlayer(playerIndex);
			TheDisplay->setShroudLevel(x, y, status);
			TheRadar->setShroudLevel(x, y, status);
			m_cells[i].invalidateShroudedStatusForAllCois(playerIndex);
		}
		TheRadar->endSetShroudLevel();
	}
}

//-----------------------------------------------------------------------------
CellShroudStatus PartitionManager::getShroudStatusForPlayer(Int playerIndex, Int x, Int y) const
{
	if( playerIndex < 0 )
		return CELLSHROUD_SHROUDED;// Safety.  There are no Negative players, but PlayerIndex is typedef'd to Int, not UnsignedInt

	const PartitionCell* cell = getCellAt(x, y);
	return cell ? cell->getShroudStatusForPlayer(playerIndex) : CELLSHROUD_SHROUDED;
}

//-----------------------------------------------------------------------------
CellShroudStatus PartitionManager::getShroudStatusForPlayer(Int playerIndex, const Coord3D *loc ) const
{
	Int x, y;

	worldToCell( loc->x, loc->y, &x, &y );

	return getShroudStatusForPlayer( playerIndex, x, y );
}


//-----------------------------------------------------------------------------
ObjectShroudStatus PartitionManager::getPropShroudStatusForPlayer(Int playerIndex, const Coord3D *loc ) const
{
	Int x, y;

	worldToCell( loc->x - m_cellSize*0.5f, loc->y - m_cellSize*0.5f, &x, &y );

	CellShroudStatus cellStat = getShroudStatusForPlayer( playerIndex, x, y );
	if (cellStat != getShroudStatusForPlayer( playerIndex, x+1, y )) {
		return OBJECTSHROUD_PARTIAL_CLEAR;
	}
	if (cellStat != getShroudStatusForPlayer( playerIndex, x+1, y+1 )) {
		return OBJECTSHROUD_PARTIAL_CLEAR;
	}
	if (cellStat != getShroudStatusForPlayer( playerIndex, x, y+1 )) {
		return OBJECTSHROUD_PARTIAL_CLEAR;
	}
	if (cellStat == CELLSHROUD_SHROUDED) {
		return OBJECTSHROUD_SHROUDED;
	}
	if (cellStat == CELLSHROUD_CLEAR) {
		return OBJECTSHROUD_CLEAR;
	}
	return OBJECTSHROUD_FOGGED;
}



#ifdef FASTER_GCO
//-----------------------------------------------------------------------------
Int PartitionManager::calcMinRadius(const ICoord2D& cur)
{
	/*
		ok, so if obj "A" is in cell (0,0), and obj "B"
		is in cell (cur.x, cur.y), calc the smallest distance they could be
		from each other, in integral multiples of cellSize. Note that
		each object can be anywhere within a given cell (not just in the center!)
		so even objects in the same cell can still be cellSize*1.414 apart,
		and objects in adjacent cells can be nearly-zero distance apart!
	*/
	Real halfCell = m_cellSize * 0.5;
	Coord3D centerPos[4] =
	{
		{ -halfCell, -halfCell },
		{ halfCell, -halfCell },
		{ -halfCell, halfCell },
		{ halfCell, halfCell }
	};

	Real x = cur.x * m_cellSize;
	Real y = cur.y * m_cellSize;
	Coord3D otherPos[4] =
	{
		{ x - halfCell, y - halfCell },
		{ x + halfCell, y - halfCell },
		{ x - halfCell, y + halfCell },
		{ x + halfCell, y + halfCell }
	};

	/*
		this is ugly and there's probably a faster way, but we only do this once per PM reset,
		so it really shouldn't matter... (I hope)
	*/

	double minDistSqr = 1e12;				// double, not real
	for (int i = 0; i < 4; ++i)
	{
		for (int j = 0; j < 4; ++j)
		{
			// double, not real
			double dx = centerPos[i].x - otherPos[j].x;
			double dy = centerPos[i].y - otherPos[j].y;
			double curDistSqr = dx*dx + dy*dy;
			if (minDistSqr > curDistSqr)
				minDistSqr = curDistSqr;
		}
	}

	// double, not real
	double dist = sqrtf(minDistSqr);
	Int minRadius = REAL_TO_INT_CEIL( dist / m_cellSize );

	return minRadius;
}
#endif

#ifdef FASTER_GCO
//-----------------------------------------------------------------------------
void PartitionManager::calcRadiusVec()
{
	Real cellSize = getCellSize();
	Int cx = getCellCountX();
	Int cy = getCellCountY();

	// double, not real
	double dx = (double)cx * (double)cellSize;
	double dy = (double)cy * (double)cellSize;
	double maxPossibleDist = sqrt(dx*dx + dy*dy);

	m_maxGcoRadius = REAL_TO_INT_CEIL(maxPossibleDist / cellSize);

	m_radiusVec.clear();
	m_radiusVec.resize(m_maxGcoRadius+1, OffsetVec());

	ICoord2D cur;
	for (cur.y = -cy+1; cur.y < cy; ++cur.y)
	{
		for (cur.x = -cx+1; cur.x < cx; ++cur.x)
		{
			/*
				m_radiusVec[curRadius] contains a list of the cells (foo) that could
				contain objects that are <= (curRadius * cellSize) distance away from cell (0,0).
			*/
			Int curRadius = calcMinRadius(cur);
			DEBUG_ASSERTCRASH(curRadius <= m_maxGcoRadius, ("expected max of %d but got %d",m_maxGcoRadius,curRadius));
			if (curRadius <= m_maxGcoRadius)
				m_radiusVec[curRadius].push_back(cur);
		}
	}

#if defined(RTS_DEBUG)
	Int total = 0;
	for (Int i = 0; i <= m_maxGcoRadius; ++i)
	{
		total += m_radiusVec[i].size();
		//DEBUG_LOG(("radius %d has %d entries",i,m_radiusVec[i].size()));
	}
	DEBUG_ASSERTCRASH(total == (cx*2-1)*(cy*2-1),("expected %d, got %d",(cx*2-1)*(cy*2-1),total));
#endif

	// GeneralsX: m_walkRank: the order getClosestObjects' ring walk reads the offsets in
	const Int rankW = cx * 2 - 1;
	m_walkRank.assign((size_t)rankW * (size_t)(cy * 2 - 1), -1);
	Int rank = 0;
	for (Int r = 0; r <= m_maxGcoRadius; ++r)
	{
		const OffsetVec& offsets = m_radiusVec[r];
		for (OffsetVec::const_iterator it = offsets.begin(); it != offsets.end(); ++it)
			m_walkRank[(it->y + cy - 1) * rankW + (it->x + cx - 1)] = rank++;
	}
}
#endif

//-----------------------------------------------------------------------------
#ifdef FASTER_GCO
static Int64 gcoIterFlag = 1;	// nonzero, thanks (see getClosestObjects). GeneralsX: 64-bit, so it never wraps onto a flag a skipped cell left
#endif

//DECLARE_PERF_TIMER(getClosestObjects)
Object *PartitionManager::getClosestObjects(
	const Object *obj,
	const Coord3D *pos,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	SimpleObjectIterator *iterArg,	// if nonnull, append ALL satisfactory objects to the iterator (not just the single closest)
	Real *closestDistArg,
	Coord3D *closestVecArg
)
{
	//USE_PERF_TIMER(getClosestObjects)

#ifdef DUMP_PERF_STATS
	if (TheGameLogic->getFrame() != s_gcoPerfFrame)
	{
		s_gcoPerfFrame = TheGameLogic->getFrame();
		s_countInClosestObjectsThisFrame = 0;
		s_timeInClosestObjectsThisFrame = 0;
	}
	++s_countInClosestObjects;
	++s_countInClosestObjectsThisFrame;

	Int64 startTime64;
	GetPrecisionTimer(&startTime64);
#endif

#ifdef RTS_DEBUG
	static Int theEntrancyCount = 0;
	DEBUG_ASSERTCRASH(theEntrancyCount == 0, ("sorry, this routine is not reentrant"));
	++theEntrancyCount;
#endif

	DEBUG_ASSERTCRASH((obj==nullptr) != (pos == nullptr), ("either obj or pos must be null"));

	DistCalcProc distProc = theDistCalcProcs[dc];

	const Coord3D *objPos;
	const Object *objToUse;
	if (pos)
	{
		objPos = pos;
		objToUse = nullptr;
	}
	else
	{
		objPos = obj->getPosition();
		objToUse = obj;
	}
	Int cellCenterX, cellCenterY;
	worldToCell(objPos->x, objPos->y, &cellCenterX, &cellCenterY);

	Object* closestObj = nullptr;
	Real closestDistSqr = maxDist * maxDist;	// if it's not closer than this, we shouldn't consider it anyway...
	Coord3D closestVec;
#if !RETAIL_COMPATIBLE_CRC // TheSuperHackers @info This should be safe to initialize because it is unused, but let us be extra safe for now.
	closestVec.x = maxDist;
	closestVec.y = maxDist;
	closestVec.z = maxDist;
#endif

#ifdef FASTER_GCO

	Int maxRadius = m_maxGcoRadius;
	if (maxDist < HUGE_DIST)
	{
		// don't go outwards any farther than necessary.
		maxRadius = min(m_maxGcoRadius, worldToCellDist(maxDist));
	}
#if defined(INTENSE_DEBUG)
	/*
		Note, if you ever enable this code, be forewarned that it can give
		you "false positives" for objects that are located just off the map... (srj)
	*/
	Int maxRadiusLimit = maxRadius + 3;
	if (maxRadiusLimit > m_maxGcoRadius) maxRadiusLimit = m_maxGcoRadius;
#else
	Int maxRadiusLimit = maxRadius;
#endif

	Bool foundAny = false;

	// GeneralsX: the counter lives at file scope (gcoIterFlag) because visitObjectsNearToFar
	// marks the same done flags: two counters could meet a stale value of the other and skip an object.
	Int64 &theIterFlag = gcoIterFlag;
	++theIterFlag;

	// GeneralsX: a cell holding no object the first filter could pass is skipped: every
	// object in it would have been rejected there (see PartitionFilter::getCellOwnerMask), so the same objects pass,
	// in the same order. Only its objects' done flags are not set; they are compared only with a later walk's own flag, and
	// gcoIterFlag is 64-bit so no later walk reuses this one's value.
	const PartitionOwnerMask ownerMask = (filters && *filters) ? (*filters)->getCellOwnerMask() : PARTITION_OWNERS_ALL;

	/*
		m_radiusVec[curRadius] contains a list of the cells (foo) that could
		contain objects that are <= (curRadius * cellSize) distance away from cell (0,0).
	*/
  for (Int curRadius = 0; curRadius <= maxRadiusLimit; ++curRadius)
  {
    const OffsetVec& offsets = m_radiusVec[curRadius];
		if (offsets.empty())
			continue;
    for (OffsetVec::const_iterator it = offsets.begin(); it != offsets.end(); ++it)
		{
			const Int cellX = cellCenterX + it->x;
			const Int cellY = cellCenterY + it->y;
			// (the dense mask first: a skipped cell is never touched; getCellAt's bounds and index)
			if (ownerMask != PARTITION_OWNERS_ALL && (cellX < 0 || cellY < 0 || cellX >= m_cellCountX || cellY >= m_cellCountY
					|| (m_cellOwnerMask[cellY * m_cellCountX + cellX] & ownerMask) == 0))
				continue;
			PartitionCell* thisCell = getCellAt(cellX, cellY);
			if (thisCell == nullptr)
				continue;

			for (CellAndObjectIntersection *thisCoi = thisCell->getFirstCoiInCell(); thisCoi; thisCoi = thisCoi->getNextCoi())
			{
				PartitionData *thisMod = thisCoi->getModule();
				Object *thisObj = thisMod->getObject();

				// never compare against ourself.
				if (thisObj == obj || thisObj == nullptr)
					continue;

				// since an object can exist in multiple COIs, we use this to avoid processing
				// the same one more than once.
				if (thisMod->friend_getDoneFlag() == theIterFlag)
					continue;
				thisMod->friend_setDoneFlag(theIterFlag);

				Real thisDistSqr;
				Coord3D distVec;
				if (!(*distProc)(objPos, objToUse, thisObj->getPosition(), thisObj, thisDistSqr, distVec, closestDistSqr))
					continue;

				if (!filtersAllow(filters, thisObj))
					continue;

				// ok, this is within the range, and the filters allow it.
				// add it to the iter, if we have one....
				if (iterArg)
				{
					iterArg->insert(thisObj, thisDistSqr);
				}
				else
				{
					// hey, this is the new closest object! cool.
					// (note that we can't break out now 'cuz we have to finish examining the
					// rest of curRadius)
					closestObj = thisObj;
					closestDistSqr = thisDistSqr;
					closestVec = distVec;

					if (!foundAny)
					{
						// if not adding to iterArg, we want to stop once we have the closest object.
						maxRadiusLimit = curRadius;
					}
					foundAny = true;
				}

			}
		}
  }

#else // not FASTER_GCO

	CellOutwardIterator iter(this, cellCenterX, cellCenterY);
	if (maxDist < HUGE_DIST)
	{
		// don't go outwards any farther than necessary.
		Int max = worldToCellDist(maxDist) + 1;
		// default value for "max" is largest possible, based on map size, so we should
		// never make it any larger than that
		if (max < iter.getMaxRadius())
			iter.setMaxRadius(max);
	}

	Bool foundAny = false;

	static Int theIterFlag = 1;	// nonzero, thanks
	++theIterFlag;

	PartitionCell *thisCell;
	while ((thisCell = iter.nextNonEmpty()) != nullptr)
	{
		CellAndObjectIntersection *nextCoi;
		for (CellAndObjectIntersection *thisCoi = thisCell->getFirstCoiInCell(); thisCoi; thisCoi = nextCoi)
		{
			nextCoi = thisCoi->getNextCoi();

			PartitionData *thisMod = thisCoi->getModule();

			Object *thisObj = thisMod->getObject();

			// never compare against ourself.
			if (thisObj == obj)
				continue;

			if (thisMod->friend_getDoneFlag() == theIterFlag)
				continue;

			thisMod->friend_setDoneFlag(theIterFlag);

			// hmm, ok, calc the distance.
			Real thisDistSqr;
			Coord3D distVec;
			if (!(*distProc)(objPos, objToUse, thisObj->getPosition(), thisObj, &thisDistSqr, &distVec, closestDistSqr))
				continue;

			// check the filters now
			if (!filtersAllow(filters, thisObj))
				continue;

			// ok, guess this is a winner!
			if (iterArg)
			{
				iterArg->insert(thisObj, thisDistSqr);
			}
			else
			{
				closestObj = thisObj;
				closestDistSqr = thisDistSqr;
				closestVec = distVec;

				if (!foundAny)
				{
					// if not adding to iterArg, we want to stop once we have the closest object.
					// since all objects in this radius (and the next radius, due to slop) might
					// be slightly closer, we still have to check all of them. so set the termination
					// radius to be our-current-radius-plus-1. (if we ARE adding to the iterArg, we skip
					// this, cuz we want to go all the way out to the original max we specified as an arg.)
					iter.setMaxRadius(iter.getCurCellRadius() + 2);
				}
				foundAny = true;
			}
		}
	}

#endif  // not FASTER_GCO

	if (closestVecArg)
	{
		*closestVecArg = closestVec;
	}
	if (closestDistArg)
	{
		*closestDistArg = (Real)sqrtf(closestDistSqr);
	}

#ifdef RTS_DEBUG
	--theEntrancyCount;
#endif
#ifdef DUMP_PERF_STATS
	Int64 endTime64;
	GetPrecisionTimer(&endTime64);
	Int64 delta = (endTime64 - startTime64);
	s_timeInClosestObjects += delta;
	s_timeInClosestObjectsThisFrame += delta;
#endif

	return closestObj;	// might be null...
}


//-----------------------------------------------------------------------------
Object *PartitionManager::getClosestObject(
	const Object *obj,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	Real *closestDist,
	Coord3D *closestDistVec
)
{
	return getClosestObjects(obj, nullptr, maxDist, dc, filters, nullptr, closestDist, closestDistVec);
}

//-----------------------------------------------------------------------------
Object *PartitionManager::getClosestObject(
	const Coord3D *pos,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	Real *closestDist,
	Coord3D *closestDistVec
)
{
	return getClosestObjects(nullptr, pos, maxDist, dc, filters, nullptr, closestDist, closestDistVec);
}

//-----------------------------------------------------------------------------
void PartitionManager::getVectorTo(const Object *obj, const Object *otherObj, DistanceCalculationType dc, Coord3D& vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real distSqr;
	(*distProc)(obj->getPosition(), obj, otherObj->getPosition(), otherObj, distSqr, vec, HUGE_DIST_SQR);
}

//-----------------------------------------------------------------------------
void PartitionManager::getVectorTo(const Object *obj, const Coord3D *pos, DistanceCalculationType dc, Coord3D& vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real distSqr;
	(*distProc)(obj->getPosition(), obj, pos, nullptr, distSqr, vec, HUGE_DIST_SQR);
}

//-----------------------------------------------------------------------------
Real PartitionManager::getDistanceSquared(const Object *obj, const Object *otherObj, DistanceCalculationType dc, Coord3D *vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real thisDistSqr;
	Coord3D thisVec;
	(*distProc)(obj->getPosition(), obj, otherObj->getPosition(), otherObj, thisDistSqr, thisVec, HUGE_DIST_SQR);
	if (vec)
		*vec = thisVec;
	return thisDistSqr;
}

//-----------------------------------------------------------------------------
Real PartitionManager::getDistanceSquared(const Object *obj, const Coord3D *pos, DistanceCalculationType dc, Coord3D *vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real thisDistSqr;
	Coord3D thisVec;
	(*distProc)(obj->getPosition(), obj, pos, nullptr, thisDistSqr, thisVec, HUGE_DIST_SQR);
	if (vec)
		*vec = thisVec;
	return thisDistSqr;
}

//-----------------------------------------------------------------------------
// Gets the distance if obj were at goalPos.  Used to calculate attack position paths.
Real PartitionManager::getGoalDistanceSquared(const Object *obj, const Coord3D *goalPos, const Object *otherObj, DistanceCalculationType dc, Coord3D *vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real thisDistSqr;
	Coord3D thisVec;
	(*distProc)(goalPos, obj, otherObj->getPosition(), otherObj, thisDistSqr, thisVec, HUGE_DIST_SQR);
	if (vec)
		*vec = thisVec;
	return thisDistSqr;
}

//-----------------------------------------------------------------------------
// Gets the distance if obj were at goalPos.  Used to calculate attack position paths.
Real PartitionManager::getGoalDistanceSquared(const Object *obj, const Coord3D *goalPos, const Coord3D *otherPos, DistanceCalculationType dc, Coord3D *vec)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	Real thisDistSqr;
	Coord3D thisVec;
	(*distProc)(goalPos, obj, otherPos, nullptr, thisDistSqr, thisVec, HUGE_DIST_SQR);
	if (vec)
		*vec = thisVec;
	return thisDistSqr;
}

//-----------------------------------------------------------------------------
Real PartitionManager::getRelativeAngle2D( const Object *obj, const Object *otherObj )
{
	return getRelativeAngle2D(obj, otherObj->getPosition());
}

//-----------------------------------------------------------------------------
Real PartitionManager::getRelativeAngle2D( const Object *obj, const Coord3D *pos )
{
	Coord3D v;

	// compute vector to given position
	Coord3D objPos = *obj->getPosition();
	v.x = pos->x - objPos.x;
	v.y = pos->y - objPos.y;
	v.z = 0.0f;

	Real dist = (Real)sqrtf(sqr(v.x) + sqr(v.y));

	// normalize
	if (dist == 0.0f)
		return 0.0f;


	const Coord3D *dir = obj->getUnitDirectionVector2D();

	Real distInv = 1.0f / dist;
	v.x *= distInv;
	v.y *= distInv;
	v.z *= distInv;

	// dot of two unit vectors is cos of angle
	Real c = dir->x*v.x + dir->y*v.y; // + dir->z*v.z;

	// bound it in case of numerical error
	if (c < -1.0)
		c = -1.0;
	else if (c > 1.0)
		c = 1.0;

	Real value = (Real)ACos( c );

	// Determine sign by checking Z component of dir cross v
	// Note this is assumes 2D, and is identical to dotting the perpendicular of v with dir
	Real perpZ = dir->x * v.y - dir->y * v.x;
	if (perpZ < 0.0f)
		value = -value;

	// note: to make this 3D, 'dir' and 'v' can be normalized and dotted just as they are
	// to test sign, compute N = dir X v, then P = N x dir, then S = P . v, where sign of
	// S is sign of angle - MSB

	return value;
}

//-----------------------------------------------------------------------------
SimpleObjectIterator *PartitionManager::iterateObjectsInRange(
	const Object *obj,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	IterOrderType order
)
{
	MemoryPoolObjectHolder iterHolder;
	SimpleObjectIterator *iter = newInstance(SimpleObjectIterator);
	iterHolder.hold(iter);

	getClosestObjects(obj, nullptr, maxDist, dc, filters, iter, nullptr, nullptr);

	iter->sort(order);
	iterHolder.release();
	return iter;
}

//-----------------------------------------------------------------------------
// GeneralsX: the object-list walk behind iterateObjectsInRangePure. With an iterator, the ring
// walk in getClosestObjects never lowers its distance limit (maxDist^2) and never stops early, so it inserts every object
// that passes the distance test and the filters, at the first cell of its COIs it reaches: the lowest m_walkRank among them,
// leaving out the cells the first filter's owner mask skips. Inside one cell it goes in the cell's COI order. Sorting the
// objects by (that rank, position in that cell's COI list) therefore rebuilds its insertion order; two cells never share a
// rank and a cell lists an object once, so no two objects share a key.
#ifdef FASTER_GCO
namespace
{
	struct GcoWalkHit
	{
		Int rank;
		Int coiPos;
		Object *obj;
		Real distSqr;
		bool operator<(const GcoWalkHit &o) const { return rank != o.rank ? rank < o.rank : coiPos < o.coiPos; }
	};
	std::vector<GcoWalkHit> s_gcoWalkHits;	// scratch, cleared per call
}

Bool PartitionManager::collectInWalkOrder(const Object *obj, Real maxDist, DistanceCalculationType dc,
	PartitionFilter **filters, SimpleObjectIterator *iter)
{
	// getClosestObjects' ring limit: anything short of every ring is its walk, not this one
	Int maxRadius = m_maxGcoRadius;
	if (maxDist < HUGE_DIST)
		maxRadius = min(m_maxGcoRadius, worldToCellDist(maxDist));
	if (maxRadius < m_maxGcoRadius || m_walkRank.empty())
		return false;

	DistCalcProc distProc = theDistCalcProcs[dc];
	const Coord3D *objPos = obj->getPosition();
	Int cellCenterX, cellCenterY;
	worldToCell(objPos->x, objPos->y, &cellCenterX, &cellCenterY);
	const Real maxDistSqr = maxDist * maxDist;
	const PartitionOwnerMask ownerMask = (filters && *filters) ? (*filters)->getCellOwnerMask() : PARTITION_OWNERS_ALL;
	const Int rankW = m_cellCountX * 2 - 1;

	s_gcoWalkHits.clear();
	for (PartitionData *mod = m_moduleList; mod; mod = mod->getNext())
	{
		Object *thisObj = mod->getObject();
		if (thisObj == obj || thisObj == nullptr)
			continue;

		// the cell the ring walk reaches it in first
		Int bestRank = -1;
		PartitionCell *bestCell = nullptr;
		const Int coiCount = mod->friend_getCoiInUseCount();
		for (Int i = 0; i < coiCount; ++i)
		{
			PartitionCell *cell = mod->friend_getCoi(i)->getCell();
			const Int cellX = cell->getCellX();
			const Int cellY = cell->getCellY();
			if (ownerMask != PARTITION_OWNERS_ALL && (m_cellOwnerMask[cellY * m_cellCountX + cellX] & ownerMask) == 0)
				continue;
			const Int dx = cellX - cellCenterX;
			const Int dy = cellY - cellCenterY;
			if (dx <= -m_cellCountX || dx >= m_cellCountX || dy <= -m_cellCountY || dy >= m_cellCountY)
				continue;	// the centre is off the map: no ring holds this offset
			const Int rank = m_walkRank[(dy + m_cellCountY - 1) * rankW + (dx + m_cellCountX - 1)];
			if (rank >= 0 && (bestRank < 0 || rank < bestRank))
			{
				bestRank = rank;
				bestCell = cell;
			}
		}
		if (bestCell == nullptr)
			continue;

		Real thisDistSqr;
		Coord3D distVec;
		if (!(*distProc)(objPos, obj, thisObj->getPosition(), thisObj, thisDistSqr, distVec, maxDistSqr))
			continue;
		if (!filtersAllow(filters, thisObj))
			continue;

		Int coiPos = 0;
		for (CellAndObjectIntersection *coi = bestCell->getFirstCoiInCell(); coi && coi->getModule() != mod; coi = coi->getNextCoi())
			++coiPos;
		const GcoWalkHit hit = { bestRank, coiPos, thisObj, thisDistSqr };
		s_gcoWalkHits.push_back(hit);
	}

	std::sort(s_gcoWalkHits.begin(), s_gcoWalkHits.end());	// unique keys: no tie, so any sort gives this order
	if (s_gcoPureCheck)
		for (size_t i = 1; i < s_gcoWalkHits.size(); ++i)
			s_gcoChkSameCell += s_gcoWalkHits[i].rank == s_gcoWalkHits[i - 1].rank;	// two objects met first in one cell: the COI order decides
	for (std::vector<GcoWalkHit>::const_iterator it = s_gcoWalkHits.begin(); it != s_gcoWalkHits.end(); ++it)
		iter->insert(it->obj, it->distSqr);
	return true;
}
#endif

//-----------------------------------------------------------------------------
SimpleObjectIterator *PartitionManager::iterateObjectsInRangePure(
	const Object *obj,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	IterOrderType order
)
{
	MemoryPoolObjectHolder iterHolder;
	SimpleObjectIterator *iter = newInstance(SimpleObjectIterator);
	iterHolder.hold(iter);

#ifdef FASTER_GCO
	const Bool listWalk = collectInWalkOrder(obj, maxDist, dc, filters, iter);
#else
	const Bool listWalk = false;
#endif
	if (!listWalk)
	{
		getClosestObjects(obj, nullptr, maxDist, dc, filters, iter, nullptr, nullptr);
	}
	else if (s_gcoPureCheck)
	{
		// compare a list walk's iterator with the ring walk's, object for object; returns how many matched, counts a mismatch
		auto compareWithRing = [&](SimpleObjectIterator *list, PartitionFilter **f, UnsignedInt &bad, const char *what) -> Int
		{
			MemoryPoolObjectHolder ringHolder;
			SimpleObjectIterator *ring = newInstance(SimpleObjectIterator);
			ringHolder.hold(ring);
			getClosestObjects(obj, nullptr, maxDist, dc, f, ring, nullptr, nullptr);
			Real a = 0.0f, b = 0.0f;
			Object *x = list->firstWithNumeric(&a);
			Object *y = ring->firstWithNumeric(&b);
			Int n = 0;
			while (x != nullptr && x == y && a == b)
			{
				++n;
				x = list->nextWithNumeric(&a);
				y = ring->nextWithNumeric(&b);
			}
			if (x != nullptr || y != nullptr)
			{
				++bad;
				fprintf(stderr, "[GXGCOCHK] MISMATCH %s at element %d (list %s, ring %s)\n", what, n, x ? "has more" : "ended", y ? "has more" : "ended");
			}
			return n;
		};
		++s_gcoChkCalls;
		const UnsignedInt badBefore = s_gcoChkBad + s_gcoChkAllBad;
		const Int n = compareWithRing(iter, filters, s_gcoChkBad, "filtered");
		s_gcoChkObjs += n;
		s_gcoChkMulti += n >= 2;

		MemoryPoolObjectHolder allHolder;
		SimpleObjectIterator *all = newInstance(SimpleObjectIterator);
		allHolder.hold(all);
		collectInWalkOrder(obj, maxDist, dc, nullptr, all);
		s_gcoChkAllObjs += compareWithRing(all, nullptr, s_gcoChkAllBad, "unfiltered");
		if (s_gcoChkBad + s_gcoChkAllBad != badBefore)
			gcoPureCheckReport("mismatch");
	}

	iter->sort(order);

	if (listWalk && s_gcoPureCheck)
	{
		Real prev = 0.0f, cur = 0.0f;
		Object *o = iter->firstWithNumeric(&prev);
		while (o != nullptr && (o = iter->nextWithNumeric(&cur)) != nullptr)
		{
			if (cur == prev)
				++s_gcoChkSameDist;
			prev = cur;
		}
		if ((s_gcoChkCalls & 63) == 0)
			gcoPureCheckReport("totals");
	}

	iterHolder.release();
	return iter;
}

//-----------------------------------------------------------------------------
// GeneralsX: the objects iterateObjectsInRange(obj, ...) would collect, handed to the visitor
// in its insertion order, with an early stop. This is getClosestObjects' FASTER_GCO ring walk (same rings, cells, COI
// order, done flags, distance test and filters) as its own copy, so getClosestObjects - every other search - stays the
// retail loop (01/10/2026: with the visitor branches inside it, small searches ran 2-4% slower, ai-smartcheap-3 g5).
// Early stop: every object not visited yet has all of its cells in rings > curRadius, whose cells are more than
// curRadius * cellSize from any point of this cell (calcMinRadius); its centre is within its bounding radius
// (<= m_maxObjectRadius) of its cells even when the map edge clips them, plus however far it crept without being
// re-filled (<= m_maxDrift), so it is farther than curRadius * cellSize - m_maxObjectRadius - m_maxDrift away.
// That holds for objects whose cells are current. One that moved (or turned) since the last update() still sits in its
// old cells (PartitionData::makeDirty): if its CURRENT distance says it may still matter, walk on until its old cells
// have been visited - it is then visited exactly where the full walk would have visited it.
#ifndef FASTER_GCO
#error visitObjectsNearToFar copies the FASTER_GCO walk
#endif
void PartitionManager::visitObjectsNearToFar(
	const Object *obj,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	PartitionRingVisitor *visitor
)
{
	DistCalcProc distProc = theDistCalcProcs[dc];
	const Coord3D *objPos = obj->getPosition();
	Int cellCenterX, cellCenterY;
	worldToCell(objPos->x, objPos->y, &cellCenterX, &cellCenterY);
	const Real maxDistSqr = maxDist * maxDist;	// getClosestObjects' closestDistSqr, never lowered while collecting

	Int maxRadius = m_maxGcoRadius;
	if (maxDist < HUGE_DIST)
	{
		maxRadius = min(m_maxGcoRadius, worldToCellDist(maxDist));
	}
#if defined(INTENSE_DEBUG)
	Int maxRadiusLimit = maxRadius + 3;
	if (maxRadiusLimit > m_maxGcoRadius) maxRadiusLimit = m_maxGcoRadius;
#else
	Int maxRadiusLimit = maxRadius;
#endif

	Int64 &theIterFlag = gcoIterFlag;	// shared with getClosestObjects
	++theIterFlag;
	// GeneralsX: a cell holding no object the first filter could pass is skipped, as in getClosestObjects
	// (see PartitionFilter::getCellOwnerMask: such a filter's allow() has no side effects), so the same objects pass, in the same order.
	const PartitionOwnerMask ownerMask = (filters && *filters) ? (*filters)->getCellOwnerMask() : PARTITION_OWNERS_ALL;
	Int stopRing = -1;	// the ring after which the walk may stop, once decided
	for (Int curRadius = 0; curRadius <= maxRadiusLimit; ++curRadius)
	{
		const OffsetVec& offsets = m_radiusVec[curRadius];
		if (offsets.empty())
			continue;
		for (OffsetVec::const_iterator it = offsets.begin(); it != offsets.end(); ++it)
		{
			const Int cellX = cellCenterX + it->x;
			const Int cellY = cellCenterY + it->y;
			// (the dense mask first: a skipped cell is never touched; getCellAt's bounds and index)
			if (ownerMask != PARTITION_OWNERS_ALL && (cellX < 0 || cellY < 0 || cellX >= m_cellCountX || cellY >= m_cellCountY
					|| (m_cellOwnerMask[cellY * m_cellCountX + cellX] & ownerMask) == 0))
				continue;
			PartitionCell* thisCell = getCellAt(cellX, cellY);
			if (thisCell == nullptr)
				continue;

			for (CellAndObjectIntersection *thisCoi = thisCell->getFirstCoiInCell(); thisCoi; thisCoi = thisCoi->getNextCoi())
			{
				PartitionData *thisMod = thisCoi->getModule();
				Object *thisObj = thisMod->getObject();
				if (thisObj == obj || thisObj == nullptr)
					continue;
				if (thisMod->friend_getDoneFlag() == theIterFlag)
					continue;
				thisMod->friend_setDoneFlag(theIterFlag);

				Real thisDistSqr;
				Coord3D distVec;
				if (!(*distProc)(objPos, obj, thisObj->getPosition(), thisObj, thisDistSqr, distVec, maxDistSqr))
					continue;
				if (!filtersAllow(filters, thisObj))
					continue;

				visitor->visit(thisObj, thisDistSqr);	// in the order iterateObjectsInRange would have inserted it
			}
		}

		if (stopRing < 0 && curRadius < maxRadiusLimit
			&& visitor->canStopAfter(curRadius * m_cellSize - m_maxObjectRadius - m_maxDrift, m_maxObjectRadius))
		{
			stopRing = curRadius;
			for (PartitionData *d = m_dirtyModules; d; d = d->friend_getNextDirty())
			{
				Object *o = d->getObject();
				const Int coiCount = d->friend_getCoiInUseCount();
				if (o == nullptr || o == obj || coiCount == 0 || !d->isInNeedOfUpdatingCells() || d->friend_getDoneFlag() == theIterFlag)
					continue;
				// GeneralsX: the first filter rejects it wherever it is, so it cannot be visited; without this, a rejected object left
				// unflagged by a skipped cell would hold the walk open.
				if (ownerMask != PARTITION_OWNERS_ALL && (ownerMask & gxOwnerTagBit(d->friend_getOwnerTag())) == 0)
					continue;
				const Real centreDist = sqrtf(sqr(o->getPosition()->x - objPos->x) + sqr(o->getPosition()->y - objPos->y));
				if (!visitor->mayStillMatter(o, centreDist))
					continue;
				for (Int i = 0; i < coiCount; ++i)
				{
					const PartitionCell *c = d->friend_getCoi(i)->getCell();
					ICoord2D off;
					off.x = c->getCellX() - cellCenterX;
					off.y = c->getCellY() - cellCenterY;
					stopRing = max(stopRing, calcMinRadius(off));
				}
			}
			if (stopRing < maxRadiusLimit)
			{
				maxRadiusLimit = stopRing;
				visitor->stoppedEarly();
			}
		}
	}
}

//-----------------------------------------------------------------------------
SimpleObjectIterator *PartitionManager::iterateObjectsInRange(
	const Coord3D *pos,
	Real maxDist,
	DistanceCalculationType dc,
	PartitionFilter **filters,
	IterOrderType order
)
{
	MemoryPoolObjectHolder iterHolder;
	SimpleObjectIterator *iter = newInstance(SimpleObjectIterator);
	iterHolder.hold(iter);

	getClosestObjects(nullptr, pos, maxDist, dc, filters, iter, nullptr, nullptr);

	iter->sort(order);
	iterHolder.release();
	return iter;
}

//-----------------------------------------------------------------------------
SimpleObjectIterator* PartitionManager::iteratePotentialCollisions(
	const Coord3D* pos,
	const GeometryInfo& geom,
	Real angle,
	Bool use2D
)
{
	Real maxDist = geom.getBoundingSphereRadius();
	maxDist *= 1.1f;	// just a little slop

	MemoryPoolObjectHolder iterHolder;
	SimpleObjectIterator *iter = newInstance(SimpleObjectIterator);
	iterHolder.hold(iter);

	PartitionFilterWouldCollide filter(*pos, geom, angle, true);
	PartitionFilter *filters[] = { &filter, nullptr };

	getClosestObjects(nullptr, pos, maxDist, use2D ? FROM_BOUNDINGSPHERE_2D : FROM_BOUNDINGSPHERE_3D, filters, iter, nullptr, nullptr);

	iterHolder.release();
	return iter;
}

//-----------------------------------------------------------------------------
Bool PartitionManager::isColliding( const Object *a, const Object *b ) const
{
	//Make sure we have objects
  if( !a || !b )
	{
    return false;
	}

	//Get both object's partition data
  const PartitionData* ad = a->friend_getConstPartitionData();
  const PartitionData* bd = b->friend_getConstPartitionData();

	//Make sure we got it
  if( !ad || !bd )
	{
    return false;
	}

	//See if the partition data collides.
	return ad->friend_collidesWith( bd, nullptr );
}

//-----------------------------------------------------------------------------
SimpleObjectIterator *PartitionManager::iterateAllObjects(PartitionFilter **filters)
{
	MemoryPoolObjectHolder iterHolder;
	SimpleObjectIterator *iter = newInstance(SimpleObjectIterator);
	iterHolder.hold(iter);

	// no distance constraints; we're gonna have to process 'em all anyway,
	// so go thru in the fastest way we know how.
	PartitionData *mod, *nextMod;
	for( mod = m_moduleList; mod; mod = nextMod )
	{
		nextMod = mod->getNext();
		Object *obj = mod->getObject();
		if (obj && filtersAllow(filters, obj))
		{
			iter->insert( obj );
		}
	}

	iterHolder.release();
	return iter;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool PartitionManager::tryPosition( const Coord3D *center,
																		Real dist,
																		Real angle,
																		const FindPositionOptions *options,
																		Coord3D *result )
{

	// compute the spot on the terrain we've picked
	Coord3D pos;
	pos.x = dist * Cos( angle ) + center->x;
	pos.y = dist * Sin( angle ) + center->y;

	PathfindLayerEnum layer = LAYER_GROUND;
	if ((options->flags & FPF_USE_HIGHEST_LAYER) != 0)
	{
		pos.z = 99999.0f;
		layer = TheTerrainLogic->getHighestLayerForDestination(&pos);
		pos.z = TheTerrainLogic->getLayerHeight(pos.x, pos.y, layer);
		// ensure we are slightly above the bridge, to account for fudge & sloppy art
		if (layer != LAYER_GROUND)
			pos.z += 1.0f;
	}
	else
	{
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
	}

	if (fabs(pos.z - center->z) > options->maxZDelta)
		return FALSE;

	//
	// we don't usually find positions on cliffs.
	// someday, add bit options for this, like for water.
	//
	if (TheTerrainLogic->isCliffCell(pos.x, pos.y) && layer == LAYER_GROUND)
		return FALSE;

	//
	// srj sez:
	// we don't usually find positions on impassable areas.
	// someday, add bit options for this, like for water.
	//
	{
		Int cellX = REAL_TO_INT_FLOOR(pos.x / PATHFIND_CELL_SIZE);
		Int cellY = REAL_TO_INT_FLOOR(pos.y / PATHFIND_CELL_SIZE);
		PathfindCell* cell = TheAI->pathfinder()->getCell(layer, cellX, cellY);
		if (!cell || cell->getType() == PathfindCell::CELL_IMPASSABLE)
		{
			return FALSE;
		}
		if( BitIsSet(options->flags, FPF_CLEAR_CELLS_ONLY) && cell->getType() != PathfindCell::CELL_CLEAR )
			return FALSE;

	}

	//
	// we don't usually find positions in the water unless we explicitly say that's OK,
	// or if the option is set we can only pick position underwater
	//
	if( BitIsSet( options->flags, FPF_IGNORE_WATER ) == FALSE )
	{
		Bool isUnderwater = TheTerrainLogic->isUnderwater( pos.x, pos.y );

		//
		// if we want water spots only and this is underwater it's no good, otherwise we want
		// the default behavior where underwater spots are invalid
		//
		if( BitIsSet( options->flags, FPF_WATER_ONLY ) && (isUnderwater == FALSE || layer != LAYER_GROUND) )
			return FALSE;
		else if( isUnderwater == TRUE && layer == LAYER_GROUND )
			return FALSE;

	}

	// object checks
	if( BitIsSet( options->flags, FPF_IGNORE_ALL_OBJECTS ) == FALSE )
	{

		//
		// iterate the potential collisions at this location using a
		// very small sphere geometry around the point
		//
		GeometryInfo geometry( GEOMETRY_SPHERE, TRUE, 5.0f, 5.0f, 5.0f );
		ObjectIterator *iter = iteratePotentialCollisions( &pos, geometry, angle, true );
		MemoryPoolObjectHolder hold( iter );
//	Bool overlap = FALSE;

		for( Object *them = iter->first(); them; them = iter->next() )
		{

			// if this is the ignore object, ignore it
			if( them == options->ignoreObject )
				continue;  // continue in the object iterator for loop

			// relationship checks
			if( options->relationshipObject )
			{

				// if this is an ally/neutral unit and we ignore those, do so
				if( BitIsSet( options->flags, FPF_IGNORE_ALLY_OR_NEUTRAL_UNITS ) == TRUE &&
						options->relationshipObject->getRelationship( them ) != ENEMIES &&
						(them->isKindOf( KINDOF_INFANTRY ) || them->isKindOf( KINDOF_VEHICLE )) )
					continue;

				// if this is an ally/neutral structure and we ignore those, do so
				if( BitIsSet( options->flags, FPF_IGNORE_ALLY_OR_NEUTRAL_STRUCTURES ) == TRUE &&
						options->relationshipObject->getRelationship( them ) != ENEMIES &&
						them->isKindOf( KINDOF_STRUCTURE ) )
					continue;

				// if this is an enemy unit and we ignore those, do so
				if( BitIsSet( options->flags, FPF_IGNORE_ENEMY_UNITS ) == TRUE &&
						options->relationshipObject->getRelationship( them ) == ENEMIES &&
						(them->isKindOf( KINDOF_INFANTRY ) || them->isKindOf( KINDOF_VEHICLE )) )
					continue;

				// if this is an enemy structure and we ignore those, do so
				if( BitIsSet( options->flags, FPF_IGNORE_ENEMY_STRUCTURES ) == TRUE &&
						options->relationshipObject->getRelationship( them ) == ENEMIES &&
						them->isKindOf( KINDOF_STRUCTURE ) )
					continue;

				// GeneralsX @bugfix wh1ter0se 11/08/2026 Allied mines must not invalidate candidate
				// positions (upstream #2776). The pathfinder deliberately drives through mines
				// (classifyObjectFootprint skips KINDOF_MINE), but this overlap check could not
				// express that, so a China mine ring starved every dock approach search around the
				// structure. Opt-in per call site; allied only - enemy and neutral mines still block.
				if( BitIsSet( options->flags, FPF_IGNORE_ALLIED_MINES ) == TRUE &&
						options->relationshipObject->getRelationship( them ) == ALLIES &&
						them->isKindOf( KINDOF_MINE ) )
					continue;

			}

			//
			// if we have a source that must path to the destination we will ignore that too
			// NOTE: If needed, we can reorganize the meaning of this method to be
			// more explicit on when to pay attention to any 'source' objects and when not too
			// but for now we're keeping the API very simple for the kinds of queries
			// that we're likely to do for an RTS (CBD)
			//
			if( them == options->sourceToPathToDest )
				continue;  // continue in the object iterator for loop

			// oops, we have overlapped something
			return FALSE;

		}

	}

	//
	// finally ... if sourceToPathDest is valid ... the source object must be able to
	// find a path the the position we have selected.  We want to do this last because
	// a pathfind can sometimes be an relatively expensive operation
	//
	if( options->sourceToPathToDest )
	{
		const AIUpdateInterface *ai = options->sourceToPathToDest->getAIUpdateInterface();

		// check for path existence
		if( ai && TheAI->pathfinder()->clientSafeQuickDoesPathExist( ai->getLocomotorSet(),
																									options->sourceToPathToDest->getPosition(),
																									&pos ) == FALSE )
				return FALSE;

	}

	// save result and return TRUE for position found
	*result = pos;
	return TRUE;

}

//
// the following determines how fast we expand our concentric ring search for the
// find position methods
//
static Real ringSpacing = 5.0f;

//-------------------------------------------------------------------------------------------------
/** This method will attempt to find a legal position from the center position specified,
	* at least minRadis away from it, but no more than maxRadius away.
	*
	* Return TRUE if position is found and that position is returned in 'result'
	* return FALSE no legal position exists or invalid params
	*/
//-------------------------------------------------------------------------------------------------
Bool PartitionManager::findPositionAround( const Coord3D *center,
																					 const FindPositionOptions *options,
																					 Coord3D *result )
{

	// sanity
	if( center == nullptr || result == nullptr || options == nullptr )
		return FALSE;

	Region3D extent;
	TheTerrainLogic->getMaximumPathfindExtent(&extent);
	// If the goal is off the map, it is a scripted setup, so just
	// use the center.
	if (!extent.isInRegionNoZ(*center)) {
		*result = *center;
		return true;
	}
	// sanity, FPF_IGNORE_WATER and FPF_WATER_ONLY are mutually exclusive
	DEBUG_ASSERTCRASH( !(BitIsSet( options->flags, FPF_IGNORE_WATER ) == TRUE &&
										   BitIsSet( options->flags, FPF_WATER_ONLY ) == TRUE),
										 ("PartitionManager::findPositionAround - The options FPF_WATER_ONLY and FPF_IGNORE_WATER are mutually exclusive.  You cannot use them together") );

	// pick a random angle from the center location to start at
	Real startAngle;
	if( options->startAngle == RANDOM_START_ANGLE )
		startAngle = GameLogicRandomValueReal( 0.0f, TWO_PI );
	else
		startAngle = options->startAngle;

	// start the search at the most inner ring (minRadius) and expand to outer most ring (maxRadius)
	for( Real dist = options->minRadius; dist <= options->maxRadius; dist += ringSpacing )
	{

		//
		// given the spacing that we've been using for the angles on the 'idea' inner circle,
		// we will need more points on larger circles to cover more ground
		//
		Real angleSpacing;
		if( dist == options->minRadius )
			angleSpacing = TWO_PI;
		else
			angleSpacing = (ringSpacing / (dist + 1.0f)) * (TWO_PI / 6.0f);  // larger float = more samples

		//
		// on this "ring", try all the angles available to us in a circle ... we'll start at
		// the 'startAngle', then try 'angleSpacing' to the left, then 'angleSpacing' to the
		// right, then '2 angleSpacings' to the left etc., ping ponging around the start angle
		// until we've done them all the way around the circle
		//

		// how many samples will we test
		Int samples = REAL_TO_INT_CEIL( (TWO_PI / angleSpacing) / 2.0f);
		for( Int i = 0; i < samples; ++i )
		{

			// try one "side"
			if( tryPosition( center, dist, startAngle + angleSpacing * i, options, result ) == TRUE )
				return TRUE;

			//
			// try the other "side" ... note for the first point at minRadius this position is
			// the same so we won't test it again in that case
			//
			if( i != 0 )
				if( tryPosition( center, dist, startAngle - angleSpacing * i, options, result ) == TRUE )
					return TRUE;

		}

	}

	// no position was able to be found
	return FALSE;

}

//-----------------------------------------------------------------------------
// This is the main accessor of the shroud system.  At this level, allies are taken
// into consideration as specified by the caller.  Look/Unlook are the ones sending Ally info, as that
// is in Object where Allies make sense.  AddLooker literally just adds a looker for the player you specify.
// This way, Full map reveals and Observer mode active look will not carry over to all
// allies.  They'll use the RevealWholeDamnMap series, which call addLooker directly.
// GeneralsX: A map script's reveal or shroud radius (MAP_REVEAL_AT_WAYPOINT, MAP_SHROUD_AT_WAYPOINT,
// MAP_REVEAL_PERMANENTLY_AT_WAYPOINT and the named reveals) reaches these four as the file gave it: 1e10 overflowed the cell
// conversion and DiscreteCircle reserved radius << 1 edges - a negative or multi-gigabyte request. Capped at a
// million world units (100,000 cells): a circle that large around any point of a map already covers all of it, so the cells
// it touches are the same. Real maps use 1..20,000 (2,641 maps scanned); sight ranges are far below. A NaN radius is 0 (one
// cell, the minimum below): Zero Hour's fast_float_ceil turned NaN into INT_MAX cells, the same crash.
// GeneralsX: The threat and value maps size the same circles from an object's VisionRange, which a
// map.ini may override, so they take the same cap (real vision ranges reach 99,999; batch-0958).
static const Real MAX_SHROUD_RADIUS = 1000000.0f;
static Real shroudRadius(Real radius)
{
	if (!(radius == radius))
		return 0.0f;
	return radius > MAX_SHROUD_RADIUS ? MAX_SHROUD_RADIUS : radius;
}

// GeneralsX: The cap above is in world units, so with PartitionCellSize = 1 (a map.ini may set it)
// a reveal still built a million-cell DiscreteCircle and walked each scanline's whole two-million-cell span.
// A circle that reaches every cell of the grid touches the same cells, each once, as any larger one, so the
// radius a circle is built with stops there: the Manhattan distance from the centre cell to the farthest grid cell, plus 2
// (the discrete circle of that radius contains the whole diamond, and so the grid - scripts/test/discrete-circle-test.sh
// compares every cell against the uncapped circle, centres outside the grid included). Only the circle's size changes:
// the threat and value falloff keeps its own radius, so every cell gets exactly the value it got before.
// A circle whose bounding square (every row and span lies within centre +- radius) misses the grid touches no cell: 0, and
// the caller draws nothing - nor builds a circle around a centre far enough out to overflow its Int rows.
// Left: a centre outside the grid whose circle reaches part of it costs one linear walk of its radius (at most a million).
static Int gridCircleRadius(Int cellCenterX, Int cellCenterY, Int cellRadius, Int cellCountX, Int cellCountY)
{
	if ((Int64)cellCenterX + cellRadius < 0 || (Int64)cellCenterX - cellRadius > (Int64)cellCountX - 1
		|| (Int64)cellCenterY + cellRadius < 0 || (Int64)cellCenterY - cellRadius > (Int64)cellCountY - 1)
		return 0;
	const Int64 farX = cellCenterX > (Int64)cellCountX - 1 - cellCenterX ? (Int64)cellCenterX : (Int64)cellCountX - 1 - cellCenterX;
	const Int64 farY = cellCenterY > (Int64)cellCountY - 1 - cellCenterY ? (Int64)cellCenterY : (Int64)cellCountY - 1 - cellCenterY;
	const Int64 reach = farX + farY + 2;
	return cellRadius > reach ? (Int)reach : cellRadius;
}

void PartitionManager::doShroudReveal(Real centerX, Real centerY, Real radius, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		// Object's Look is the one who knows about allies.  Anyone can pask a player mask to me and all
		// of those players will have an active looker applied to a bunch of cells
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			circle.drawCircle(hLineAddLooker, (void*)currentIndex);
		}
	}
}

//-----------------------------------------------------------------------------
// GeneralsX: rb-sense-2: the AI rule bank's own-sight test.
// markLookCells marks the cells one look reveals - doShroudReveal's centre cell, radius (shroudRadius cap, at least one cell),
// gridCircleRadius and DiscreteCircle, clipped as hLineAddLooker clips - so a cell is marked iff that look adds a looker to it.
namespace
{
struct GXMarkLine
{
	UnsignedByte *cells;
	Int countX, countY;
};
void hLineMarkCells(Int x1, Int x2, Int y, void *markVoid)
{
	const GXMarkLine &m = *static_cast<const GXMarkLine *>(markVoid);
	if (y < 0 || y >= m.countY || x1 >= m.countX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= m.countX)
		x2 = m.countX - 1;
	for (Int x = x1; x <= x2; ++x)
		m.cells[y * m.countX + x] = 1;
}
}

void PartitionManager::markLookCells(const SightingInfo &look, std::vector<UnsignedByte> &mark) const
{
	const size_t n = (m_cellCountX > 0 && m_cellCountY > 0) ? (size_t)m_cellCountX * (size_t)m_cellCountY : 0u;
	if (mark.size() != n)
		mark.assign(n, 0);
	if (n == 0 || look.isInvalid())
		return;
	Int cellCenterX, cellCenterY;
	worldToCell(look.m_where.x, look.m_where.y, &cellCenterX, &cellCenterY);
	Int cellRadius = REAL_TO_INT_CEIL(shroudRadius(look.m_howFar) * m_cellSizeInv);	// worldToCellDist, which is not const
	if (cellRadius < 1)
		cellRadius = 1;
	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;
	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);
	GXMarkLine m = { &mark[0], m_cellCountX, m_cellCountY };
	circle.drawCircle(hLineMarkCells, &m);
}

//-----------------------------------------------------------------------------
void PartitionManager::processPendingUndoShroudRevealQueue( Bool considerTimestamp )
{
	//Keep going until the front one is in the future.  UndoShroudReveal on each one you process.
	//Again, you know these are in order, because we control the adding of the Constant in-the-queue time.
	UnsignedInt compareTime;
	if(considerTimestamp)
		compareTime = TheGameLogic->getFrame();
	else
		compareTime = UINT_MAX;

	while( !m_pendingUndoShroudReveals.empty() && (m_pendingUndoShroudReveals.front()->m_data < compareTime) )
	{
		SightingInfo *thisInfo = m_pendingUndoShroudReveals.front();

		undoShroudReveal( thisInfo->m_where.x, thisInfo->m_where.y, thisInfo->m_howFar, thisInfo->m_forWhom );

		deleteInstance(thisInfo);
		m_pendingUndoShroudReveals.pop();
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::processEntirePendingUndoShroudRevealQueue()
{
	// Something major is about to happen, so we need to rush through our queue and get everything in order.
	// Examples are map resize, full map undo-reveal, stuff like that.

	// Don't like duplicating code, so route to original function.
	processPendingUndoShroudRevealQueue(FALSE);
}

//-----------------------------------------------------------------------------
void PartitionManager::resetPendingUndoShroudRevealQueue()
{
	while( !m_pendingUndoShroudReveals.empty() )
	{
		SightingInfo *thisInfo = m_pendingUndoShroudReveals.front();
		deleteInstance(thisInfo);
		m_pendingUndoShroudReveals.pop();
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::undoShroudReveal(Real centerX, Real centerY, Real radius, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			circle.drawCircle(hLineRemoveLooker, (void*)currentIndex);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::queueUndoShroudReveal(Real centerX, Real centerY, Real radius, PlayerMaskType playerMask)
{
	UnsignedInt now = TheGameLogic->getFrame();
	SightingInfo *newInfo = newInstance(SightingInfo);

	newInfo->m_where.x = centerX;
	newInfo->m_where.y = centerY;
	newInfo->m_howFar = radius;
	newInfo->m_forWhom = playerMask;
	// GeneralsX: UnlookPersistDuration (GameData, which a map.ini may override) is how long every
	// unlook waits in this queue: a duration of weeks kept every unlook of the match queued. Capped at a
	// minute; shipped data says 5 seconds.
	const UnsignedInt MAX_UNLOOK_PERSIST = 60 * LOGICFRAMES_PER_SECOND;
	const UnsignedInt persist = TheGlobalData->m_unlookPersistDuration;
	newInfo->m_data = now + (persist > MAX_UNLOOK_PERSIST ? MAX_UNLOOK_PERSIST : persist);

	m_pendingUndoShroudReveals.push(newInfo);
}

//-----------------------------------------------------------------------------
void PartitionManager::doShroudCover(Real centerX, Real centerY, Real radius, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		// Object's Shroud is the one who knows about allies.  Anyone can pask a player mask to me and all
		// of those players will have an active shrouder applied to a bunch of cells
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			circle.drawCircle(hLineAddShrouder, (void*)currentIndex);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::undoShroudCover(Real centerX, Real centerY, Real radius, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			circle.drawCircle(hLineRemoveShrouder, (void*)currentIndex);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::doThreatAffect( Real centerX, Real centerY, Real radius, UnsignedInt threatVal, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);
	Real fCellCenterX = INT_TO_REAL(cellCenterX);
	Real fCellCenterY = INT_TO_REAL(cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	Real fCellRadius = INT_TO_REAL(cellRadius + 1);

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	ThreatValueParms parms;
	parms.radius = fCellRadius;
	parms.threatOrValue = threatVal;
	parms.xCenter = fCellCenterX;
	parms.yCenter = fCellCenterY;

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			parms.playerIndex = currentIndex;
			circle.drawCircle(hLineAddThreat, &parms);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::undoThreatAffect( Real centerX, Real centerY, Real radius, UnsignedInt threatVal, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);
	Real fCellCenterX = INT_TO_REAL(cellCenterX);
	Real fCellCenterY = INT_TO_REAL(cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	Real fCellRadius = INT_TO_REAL(cellRadius + 1);

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	ThreatValueParms parms;
	parms.radius = fCellRadius;
	parms.threatOrValue = threatVal;
	parms.xCenter = fCellCenterX;
	parms.yCenter = fCellCenterY;

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			parms.playerIndex = currentIndex;
			circle.drawCircle(hLineRemoveThreat, &parms);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::doValueAffect( Real centerX, Real centerY, Real radius, UnsignedInt valueVal, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);
	Real fCellCenterX = INT_TO_REAL(cellCenterX);
	Real fCellCenterY = INT_TO_REAL(cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	Real fCellRadius = INT_TO_REAL(cellRadius + 1);

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	ThreatValueParms parms;
	parms.radius = fCellRadius;
	parms.threatOrValue = valueVal;
	parms.xCenter = fCellCenterX;
	parms.yCenter = fCellCenterY;

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			parms.playerIndex = currentIndex;
			circle.drawCircle(hLineAddValue, &parms);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::undoValueAffect( Real centerX, Real centerY, Real radius, UnsignedInt valueVal, PlayerMaskType playerMask)
{
	Int cellCenterX, cellCenterY;
	worldToCell(centerX, centerY, &cellCenterX, &cellCenterY);
	Real fCellCenterX = INT_TO_REAL(cellCenterX);
	Real fCellCenterY = INT_TO_REAL(cellCenterY);

	Int cellRadius = worldToCellDist(shroudRadius(radius));
	if (cellRadius < 1)
		cellRadius = 1;

	Real fCellRadius = INT_TO_REAL(cellRadius + 1);

	const Int circleRadius = gridCircleRadius(cellCenterX, cellCenterY, cellRadius, m_cellCountX, m_cellCountY);
	if (circleRadius == 0)
		return;

	DiscreteCircle circle(cellCenterX, cellCenterY, circleRadius);

	ThreatValueParms parms;
	parms.radius = fCellRadius;
	parms.threatOrValue = valueVal;
	parms.xCenter = fCellCenterX;
	parms.yCenter = fCellCenterY;

	for( Int currentIndex = ThePlayerList->getPlayerCount() - 1; currentIndex >=0; currentIndex-- )
	{
		const Player *currentPlayer = ThePlayerList->getNthPlayer( currentIndex );
		if( BitIsSet( playerMask, currentPlayer->getPlayerMask() ) )
		{
			parms.playerIndex = currentIndex;
			circle.drawCircle(hLineRemoveValue, &parms);
		}
	}
}

//-----------------------------------------------------------------------------
void PartitionManager::getCellCenterPos(Int x, Int y, Real& xx, Real& yy)
{
	DEBUG_ASSERTCRASH(x >= 0 && y >= 0, ("hmm, invalid cell"));
	Real half = m_cellSize*0.5f;
	xx = m_worldExtents.lo.x + (x * m_cellSize) + half;
	yy = m_worldExtents.lo.y + (y * m_cellSize) + half;
}

//-----------------------------------------------------------------------------
#ifdef PM_CACHE_TERRAIN_HEIGHT

	struct TerrainExtremeData
	{
		Real* minZ;
		Real* maxZ;
		Coord2D* minZPos;
		Coord2D* maxZPos;
		Bool isValid;
	};

static Int checkTerrainExtreme(PartitionCell* cell, void* userData)
{
	TerrainExtremeData* data = (TerrainExtremeData*)userData;

	data->isValid = true;

	Real tmp;

	if (data->minZ)
	{
		tmp = cell->getLoTerrain();
		if (tmp < *data->minZ )
		{
			*data->minZ = tmp;
			if (data->minZPos)
			{
				cell->getCellCenterPos(data->minZPos->x, data->minZPos->y);
			}
		}
	}

	if (data->maxZ)
	{
		tmp = cell->getHiTerrain();
		if (tmp > *data->maxZ )
		{
			*data->maxZ = tmp;
			if (data->maxZPos)
			{
				cell->getCellCenterPos(data->maxZPos->x, data->maxZPos->y);
			}
		}
	}

	return 0;	// zero to continue
}
#endif

//-----------------------------------------------------------------------------
#ifdef PM_CACHE_TERRAIN_HEIGHT
Bool PartitionManager::estimateTerrainExtremesAlongLine(const Coord3D& pos, const Coord3D& posOther, Real* minZ, Real* maxZ, Coord2D* minZPos, Coord2D* maxZPos)
{
	TerrainExtremeData data;
	data.minZ = minZ;
	data.maxZ = maxZ;
	data.minZPos = minZPos;
	data.maxZPos = maxZPos;
	data.isValid = false;
	if (minZ) *minZ = HUGE_DIST;
	if (maxZ) *maxZ = -HUGE_DIST;
	iterateCellsAlongLine(pos, posOther, checkTerrainExtreme, &data);
	return data.isValid;
}
#endif

//-----------------------------------------------------------------------------
// Uses Bresenham line algorithm from www.gamedev.net.
Int PartitionManager::iterateCellsAlongLine(const Coord3D& pos, const Coord3D& posOther, CellAlongLineProc proc, void* userData)
{
	ICoord2D start, end, delta;
	Int x, y;
	Int xinc1, xinc2;
	Int yinc1, yinc2;
	Int den, num, numadd;
	Int numpixels;

	worldToCell(pos.x, pos.y, &start.x, &start.y);
	worldToCell(posOther.x, posOther.y, &end.x, &end.y);

	delta.x = abs(end.x - start.x);			// The difference between the x's
	delta.y = abs(end.y - start.y);			// The difference between the y's
	x = start.x;												// Start x off at the first pixel
	y = start.y;												// Start y off at the first pixel

	if (end.x >= start.x)								// The x-values are increasing
	{
		xinc1 = 1;
		xinc2 = 1;
	}
	else																// The x-values are decreasing
	{
		xinc1 = -1;
		xinc2 = -1;
	}

	if (end.y >= start.y)               // The y-values are increasing
	{
		yinc1 = 1;
		yinc2 = 1;
	}
	else																// The y-values are decreasing
	{
		yinc1 = -1;
		yinc2 = -1;
	}
	Bool checkY = true;
	if (delta.x >= delta.y)							// There is at least one x-value for every y-value
	{
		xinc1 = 0;												// Don't change the x when numerator >= denominator
		yinc2 = 0;												// Don't change the y for every iteration
		den = delta.x;
		num = delta.x / 2;
		numadd = delta.y;
		numpixels = delta.x;							// There are more x-values than y-values
	}
	else																// There is at least one y-value for every x-value
	{
		checkY = false;
		xinc2 = 0;												// Don't change the x for every iteration
		yinc1 = 0;												// Don't change the y when numerator >= denominator
		den = delta.y;
		num = delta.y / 2;
		numadd = delta.x;
		numpixels = delta.y;							// There are more y-values than x-values
	}

	for (Int curpixel = 0; curpixel <= numpixels; curpixel++)
	{
		PartitionCell* cell = getCellAt(x, y);	// might be null if off the edge
		DEBUG_ASSERTCRASH(cell != nullptr, ("off the map"));
		if (cell)
		{
			Int ret = (*proc)(cell, userData);
			if (ret != 0)
				return ret;	// bail early
		}

		num += numadd;										// Increase the numerator by the top of the fraction
		if (num >= den)										// Check if numerator >= denominator
		{
			num -= den;											// Calculate the new numerator value
			x += xinc1;											// Change the x as appropriate
			y += yinc1;											// Change the y as appropriate
		}
		x += xinc2;												// Change the x as appropriate
		y += yinc2;												// Change the y as appropriate
	}

	return 0;
}

//-----------------------------------------------------------------------------
Int PartitionManager::iterateCellsBreadthFirst(const Coord3D *pos, CellBreadthFirstProc proc, void *userData)
{
	// starting at pos, iterate the cells in the following manner:
	// left, up, right, down
	// using Breadth-first search.
	// Call proc on each cell, and if it returns non-zero, then return the cell index
	// -1 means error, but we should add a define later for this.

	// GeneralsX: worldToCell does not clamp, so an off-map start indexed past the grid.
	// tsh batch2 #412, idea of Generals Online 5499057ee (reimplemented).
	if (m_cells == nullptr || m_cellCountX <= 0 || m_cellCountY <= 0)
		return -1;

	Int cellX, cellY;
	worldToCell(pos->x, pos->y, &cellX, &cellY);
	cellX = clamp(0, cellX, m_cellCountX - 1);
	cellY = clamp(0, cellY, m_cellCountY - 1);

	// Note, bool. not Bool, cause bool will cause this to be a bitfield.
	std::vector<bool> bitField;
	Int cellCount = m_cellCountX * m_cellCountY;
	bitField.resize(cellCount);
	// 0 means not done, 1 means done.

	for (Int i = 0; i < cellCount; ++i) {
		bitField[i] = false;
	}

	std::queue<PartitionCell *> cellQ;

	// mark the starting cell done
	bitField[cellY * m_cellCountX + cellX] = true;
	cellQ.push(&m_cells[cellY * m_cellCountX + cellX]);

	Int curX = cellX;
	Int curY = cellY;
	while (!cellQ.empty()) {
		// first, add the neighbors
		// left
		if (curX - 1 >= 0) {
			if (!bitField[curY * m_cellCountX + curX - 1]) {
				bitField[curY * m_cellCountX + curX - 1] = true;
				cellQ.push(&m_cells[curY * m_cellCountX + curX - 1]);
			}
		}

		// top
		if (curY - 1 >= 0) {
			if (!bitField[(curY - 1) * m_cellCountX + curX]) {
				bitField[(curY - 1) * m_cellCountX + curX] = true;
				cellQ.push(&m_cells[(curY - 1) * m_cellCountX + curX]);
			}
		}

		// right
		if (curX + 1 < m_cellCountX) {
			if (!bitField[curY * m_cellCountX + curX + 1]) {
				bitField[curY * m_cellCountX + curX + 1] = true;
				cellQ.push(&m_cells[curY * m_cellCountX + curX + 1]);
			}
		}

		// bottom
		// GeneralsX: was 'curY + 1 > 0', always true: from the last row this read the
		// bit field past its end and queued a cell past the grid (tsh batch2 #412).
		if (curY + 1 < m_cellCountY) {
			if (!bitField[(curY + 1) * m_cellCountX + curX]) {
				bitField[(curY + 1) * m_cellCountX + curX] = true;
				cellQ.push(&m_cells[(curY + 1) * m_cellCountX + curX]);
			}
		}

		// now process the current top.
		PartitionCell *curCell = cellQ.front();
		cellQ.pop();
		curX = curCell->getCellX();
		curY = curCell->getCellY();

		if ((proc)(curCell, userData) != 0)
			return (curCell->getCellY() * m_cellCountX + curCell->getCellX());
	}

	return -1;
}

//-----------------------------------------------------------------------------
static Real calcDist2D(Real x1, Real y1, Real x2, Real y2)
{
	return sqrtf(sqr(x1-x2) + sqr(y1-y2));
}

//-----------------------------------------------------------------------------
Bool PartitionManager::isClearLineOfSightTerrain(const Object* obj, const Coord3D& objPos, const Object* other, const Coord3D& otherPos)
{
	Coord3D pos, posOther;

	if (obj)
	{
		pos = *obj->getPosition();
		// note that we want to measure from the top of the collision
		// shape, not the bottom! (most objects have eyes a lot closer
		// to their head than their feet. if we have really odd critters
		// with eye-feet, we'll need to change this assumption.)
		pos.z += obj->getGeometryInfo().getMaxHeightAbovePosition();
	}
	else
	{
		pos = objPos;
	}

	if (other)
	{
		posOther = *other->getPosition();
		// note that we want to measure from the top of the collision
		// shape, not the bottom! (most objects have eyes a lot closer
		// to their head than their feet. if we have really odd critters
		// with eye-feet, we'll need to change this assumption.)
		posOther.z += other->getGeometryInfo().getMaxHeightAbovePosition();
	}
	else
	{
		posOther = otherPos;
	}

#ifdef NO_BAD_AND_INACCURATE
/*
	This looks tempting, and in theory would be better than the below code; however, it
	is not (yet) accurate enough for our targeting needs. what we really need to do is add a Bresenham
	line iterator to the TerrainLogic itself, which would probably suffice. Until then, this code is
	left in as an example of what NOT to do. (srj)
*/
	Real maxZ;
	Coord2D maxZPos;
	Bool valid = estimateTerrainExtremesAlongLine(pos, posOther, nullptr, &maxZ, nullptr, &maxZPos);
	DEBUG_ASSERTCRASH(valid, ("this should never happen unless both positions are off-map"));
	if (!valid)
		return true;

	Real terrainAtHighPoint = maxZ;

	Real totalDist2D = calcDist2D(pos.x, pos.y, posOther.x, posOther.y);
	Real terrainDist2D = calcDist2D(pos.x, pos.y, maxZPos.x, maxZPos.y);

	const Real TINY_DIST = 0.01f;
	Real percent = (totalDist2D > TINY_DIST) ? (terrainDist2D / totalDist2D) : 0.0f;
	if (percent < 0.0f) percent = 0.0f;
	if (percent > 1.0f) percent = 1.0f;

	Real lineOfSightAtHighPoint = pos.z + (posOther.z - pos.z) * percent;

	// if terrainAtHighPoint > lineOfSightAtHighPoint, we can't see.
	// add a little fudge to account for slop.
	const Real LOS_FUDGE = 0.5f;
	if (terrainAtHighPoint > lineOfSightAtHighPoint + LOS_FUDGE)
	{
		//DEBUG_LOG(("isClearLineOfSightTerrain fails"));
		return false;
	}

	return true;

#else
	return TheTerrainLogic->isClearLineOfSight(pos, posOther);
#endif
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void PartitionManager::crc( Xfer *xfer )
{

	for (Int i=0; i<m_totalCellCount; ++i)
	{
		m_cells[i].crc(xfer);
	}

}

// ------------------------------------------------------------------------------------------------
/** Xfer Method
	* Version Info:
	* 1: Initial version
	* 2: m_pendingUndoShroudReveals stores Unlooks waiting to happen.
	*/
// ------------------------------------------------------------------------------------------------
void PartitionManager::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 2;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// cell size information
	Real cellSize = m_cellSize;
	xfer->xferReal( &cellSize );

	//
	// cell size should match what we have allocated in the partition manager, if it doesn't
	// we need to increment the version here and edit this code to be able to load
	// the old cell size if changed after we release
	//
	if( cellSize != m_cellSize )
	{

		DEBUG_CRASH(( "Partition cell size has changed, this save game file is invalid" ));
		throw SC_INVALID_DATA;

	}

	// cell count
	Int totalCellCount = m_totalCellCount;
	xfer->xferInt( &totalCellCount );

	// total cell count better match what we have allocated for cells in the partition manager
	if( totalCellCount != m_totalCellCount )
	{

		DEBUG_CRASH(( "Partition total cell count mismatch %d, should be %d",
									totalCellCount, m_totalCellCount ));
		throw SC_INVALID_DATA;

	}

	// xfer each cell information
	PartitionCell *cell;
	for( Int i = 0; i < totalCellCount; ++i )
	{

		// get this partition cell
		cell = &m_cells[ i ];

		// xfer the data for this cell
		xfer->xferSnapshot( cell );

	}

	// when loading tell the partition manager to rethink and refresh all shroud information
	if( xfer->getXferMode() == XFER_LOAD )
	{

		// tell partition manager to re-evaluate shroud things when next asked
		m_updatedSinceLastReset = FALSE;

		// refresh the shroud for the local player which will update the radar and everything
		refreshShroudForLocalPlayer();

	}

	if( version >= 2 )
	{
		Int queueSize = m_pendingUndoShroudReveals.size();
		xfer->xferInt(&queueSize);
		// This xfer thing is still cool.  On save, I just wrote the number of elements.  On load, the first line
		// above was a decoy, and the second found out the size.

		if(xfer->getXferMode() == XFER_LOAD)
		{
			// have to remove this assert, because during load there is a setTeam call for each guy on a sub-team, and that results
			// in a queued unlook, so we actually have stuff in here at the start.  I am fairly certain that setTeam should wait
			// until loadPostProcess, but I ain't gonna change it now.
//			DEBUG_ASSERTCRASH(m_pendingUndoShroudReveals.empty(), ("At load, we appear to not be in a reset state.") );

			// I have to split this up though, since on Load I need to make new instances.
			for( Int infoIndex = 0; infoIndex < queueSize; infoIndex++ )
			{
				SightingInfo *newInfo = newInstance(SightingInfo);
				xfer->xferSnapshot(newInfo);
				m_pendingUndoShroudReveals.push(newInfo);
			}
		}
		else
		{
			// And on Save, I need to loop through, but not destroy anything, so I pop-push n times, cycling through the queue once
			// (No random access on queues, and speed not a concern in saveload)
			for( Int infoIndex = 0; infoIndex < queueSize; infoIndex++ )
			{
				SightingInfo *saveInfo = m_pendingUndoShroudReveals.front();
				xfer->xferSnapshot(saveInfo);
				m_pendingUndoShroudReveals.pop();
				m_pendingUndoShroudReveals.push(saveInfo);
			}
		}

	}
	else
	{
		// Version 1 save games will just not have any SightingInfos in the queue to be undone.
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void PartitionManager::loadPostProcess()
{

}

//-----------------------------------------------------------------------------
Real PartitionManager::getGroundOrStructureHeight(Real posx, Real posy)
{
	// get the terrain height
	Real terrainHeightHere = TheTerrainLogic->getGroundHeight( posx, posy );

	// scan all objects in the radius of our extent and find the tallest height among them
	PartitionFilterAcceptByKindOf filter1( MAKE_KINDOF_MASK( KINDOF_STRUCTURE ), KINDOFMASK_NONE );
	PartitionFilter *filters[] = { &filter1, nullptr };
  Coord3D pos;
  pos.x = posx;
  pos.y = posy;
  pos.z = terrainHeightHere;
	const Real RANGE = 1.0f;
	ObjectIterator *iter = iterateObjectsInRange( &pos, RANGE, FROM_BOUNDINGSPHERE_2D, filters );
	MemoryPoolObjectHolder hold( iter );

	Real tallestHeight = 0.0f;
	Real thisHeight;
	for( Object* obj = iter->first(); obj; obj = iter->next() )
	{
		// store the height of the tallest object under us
		thisHeight = obj->getGeometryInfo().getMaxHeightAbovePosition();
		if( thisHeight > tallestHeight )
			tallestHeight = thisHeight;

	}

  return terrainHeightHere + tallestHeight;
}

//-------------------------------------------------------------------------------------------------
void PartitionManager::getMostValuableLocation( Int playerIndex, UnsignedInt whichPlayerTypes, ValueOrThreat valType, Coord3D *outLocation )
{
	if (!outLocation)
		return;

	PlayerMaskType playerMask = ThePlayerList->getPlayersWithRelationship(playerIndex, whichPlayerTypes);
	if (playerMask == 0)
		return;

	Int cellCount = m_cellCountX * m_cellCountY;

	PlayerMaskType allPlayerMasks[MAX_PLAYER_COUNT] = { 0 };
	Int totalPlayerCount = ThePlayerList->getPlayerCount();

	Int i;
	for (i = 0; i < totalPlayerCount; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (!player) {
			continue;
		}

		allPlayerMasks[i] = player->getPlayerMask();
	}

	Int greatestValueCell = -1;
	Int maxCellValue = -1;
	for (i = 0; i < cellCount; ++i) {
		Int cellValue = 0;

		for (Int player = 0; player < MAX_PLAYER_COUNT; ++player) {
			if (BitIsSet(allPlayerMasks[player], playerMask)) {
				if (valType == VOT_CashValue) {
					cellValue += m_cells[i].getCashValue(player);
				} else {
					cellValue += m_cells[i].getThreatValue(player);
				}
			}
		}

		if (cellValue > maxCellValue) {
			maxCellValue = cellValue;
			greatestValueCell = i;
		}
	}

	if (greatestValueCell == -1 || maxCellValue == -1) {
		DEBUG_CRASH(("PartitionManager::getMostValuableLocation: jkmcd"));
		return;
	}

	outLocation->set(m_cells[greatestValueCell].getCellX() * TheGlobalData->m_partitionCellSize,
									 m_cells[greatestValueCell].getCellY() * TheGlobalData->m_partitionCellSize,
									 0
									);
}

//-------------------------------------------------------------------------------------------------
Bool PartitionManager::getNearestGroupWithValue( Int playerIndex, UnsignedInt whichPlayerTypes, ValueOrThreat valType,
															 const Coord3D *sourceLocation, Int valueRequired, Bool greaterThan, Coord3D *outLocation )
{
	if (!(sourceLocation && outLocation))
		return false;

	PlayerMaskType playerMask = ThePlayerList->getPlayersWithRelationship(playerIndex, whichPlayerTypes);
	if (playerMask == 0)
		return false;

	PlayerMaskType allPlayerMasks[MAX_PLAYER_COUNT] = { 0 };
	Int totalPlayerCount = ThePlayerList->getPlayerCount();

	Int i;
	for (i = 0; i < totalPlayerCount; ++i) {
		Player *player = ThePlayerList->getNthPlayer(i);
		if (!player) {
			continue;
		}

		allPlayerMasks[i] = player->getPlayerMask();
	}

	CellValueProcParms parms;
	parms.valueRequired = valueRequired;
	parms.greaterThan = valueRequired;
	parms.valueType = valType;
	parms.allowedPlayersMasks = playerMask;
	for (i = 0; i < MAX_PLAYER_COUNT; ++i)
		parms.allPlayersMask[i] = allPlayerMasks[i];

	Int nearestGreat = iterateCellsBreadthFirst(sourceLocation, cellValueProc, &parms);
	if (nearestGreat != -1) {
		(*outLocation).x = m_cells[nearestGreat].getCellX() * TheGlobalData->m_partitionCellSize;
		(*outLocation).y = m_cells[nearestGreat].getCellY() * TheGlobalData->m_partitionCellSize;
		(*outLocation).z = 0;
		return true;
	}

	// all done
	// GeneralsX: Report whether a cell was found, so the caller does not move to an
	// unset location (tsh batch2 #412).
	return false;
}

//-------------------------------------------------------------------------------------------------
void PartitionManager::storeFoggedCells(ShroudStatusStoreRestore &outPartitionStore, Bool storeToFog) const
{
	Int i, j, p;

	if (storeToFog) {
		// This is the first pass
		outPartitionStore.m_cellsWide = m_cellCountX;

		// Resize all of our arrays.
		for (p = 0; p < MAX_PLAYER_COUNT; ++p) {
			outPartitionStore.m_foggedOrRevealed[p].resize(m_totalCellCount);
			for (i = 0; i < m_totalCellCount; ++i) {
				outPartitionStore.m_foggedOrRevealed[p][i] = STORE_DONTTOUCH;
			}
		}
	}

	for (p = 0; p < MAX_PLAYER_COUNT; ++p) {
		if (outPartitionStore.m_foggedOrRevealed[p].size() != m_totalCellCount) {
			DEBUG_CRASH(("PartitionManager::storeFoggedCells: jkmcd - x36872"));
			continue;
		}

		for (j = 0; j < m_cellCountY; ++j) {
			for (i = 0; i < m_cellCountX; ++i) {
				UnsignedByte &byteToWrite = outPartitionStore.m_foggedOrRevealed[p][j * m_cellCountX + i];

				if (storeToFog && m_cells[j * m_cellCountX + i].getShroudStatusForPlayer(p) == CELLSHROUD_FOGGED) {
					byteToWrite = STORE_FOG;
				}

				if (!storeToFog && m_cells[j * m_cellCountX + i].getShroudStatusForPlayer(p) == CELLSHROUD_CLEAR) {
					byteToWrite = STORE_PERMANENTLY_REVEALED;
				}
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void PartitionManager::restoreFoggedCells(const ShroudStatusStoreRestore &inPartitionStore, Bool restoreToFog)
{
	Int i, j, p;

	Int storeWidth = inPartitionStore.m_cellsWide;
	Int storeHeight = inPartitionStore.m_foggedOrRevealed[0].size() / storeWidth;

	for (p = 0; p < MAX_PLAYER_COUNT; ++p) {
		for (j = 0; j < storeHeight; ++j) {
			if (j >= m_cellCountY) {
				// All done.
				return;
			}

			for (i = 0; i < storeWidth; ++i) {
				if (i >= m_cellCountX) {
					// Skip to the next line. This info will be thrown away.
					break;
				}

				UnsignedByte byteToRestore = inPartitionStore.m_foggedOrRevealed[p][j * storeWidth + i];
				if (byteToRestore == STORE_DONTTOUCH) {
					continue;
				}

				if (byteToRestore == STORE_FOG && restoreToFog) {
					// restore the fog status of this cell
					m_cells[j * m_cellCountX + i].addLooker(p);
					m_cells[j * m_cellCountX + i].removeLooker(p);
				}

				if (byteToRestore == STORE_PERMANENTLY_REVEALED && !restoreToFog) {
					// Add an extra looker.
					m_cells[j * m_cellCountX + i].addLooker(p);
				}
			}
		}
	}
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionFilterRejectBuildings::PartitionFilterRejectBuildings(const Object *o) :
	m_self(o),
	m_acquireEnemies(false)
{
	// if I am a computer-controlled opponent, auto-acquire enemy buildings
	if (m_self->getControllingPlayer()->getPlayerType() == PLAYER_COMPUTER)
	{
		m_acquireEnemies = true;
	}
}

//-----------------------------------------------------------------------------
Bool PartitionFilterRejectBuildings::allow( Object *other )
{
	// this filter allows all non-buildings
	// um, no, it clearly doesn't any more.
	// didn't the person adding the code below read the comment?
	if (!other->isKindOf( KINDOF_STRUCTURE ))
		return true;

	const Player* myPlayer = m_self->getControllingPlayer();
	if (!myPlayer)
		return false;

	// Get the controlling team of other.
	ContainModuleInterface* contain = other->getContain();
	const Player* otherPlayer = contain ? contain->getApparentControllingPlayer(myPlayer) : nullptr;

	if (!otherPlayer)
		otherPlayer = other->getControllingPlayer();

	if (!otherPlayer)
		return false;

	Relationship relationship = myPlayer->getRelationship(otherPlayer->getDefaultTeam());
	if (relationship != ENEMIES)
		return false;

	// if I am a computer-controlled opponent, auto-acquire enemy buildings (if we can see them!)
	if (m_acquireEnemies)
		return true;

	if (other->isKindOf( KINDOF_FS_BASE_DEFENSE))
	{
		// Don't reject base defenses.
		return true;
	}

	if (other->getContain() != nullptr && other->isAbleToAttack())
	{
		// Don't reject garrisoned buildings that can attack
		return true;
  }

	return false;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Bool PartitionFilterFreeOfFog::allow( Object *other )
{
	return other->getShroudedStatus(m_comparisonIndex) == OBJECTSHROUD_CLEAR;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Bool PartitionFilterInsignificantBuildings::allow( Object *other )
{
	if (other->isStructure()) {
		if (other->isNonFactionStructure() && !m_allowInsignificant) {
			ContainModuleInterface *cmi = other->getContain();
			if (cmi) {
				if (!cmi->isGarrisonable() || cmi->getContainCount() == 0) {
					return false;
				}
			}
		}
		return true;

	} else {
		if (m_allowNonBuildings) {
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------

Bool PartitionFilterRepulsor::allow( Object *other )
{
	if (other == m_self)
	{
		// don't repulse yourself. :)
		return false;
	}

	// If it's flagged, it's a repulsor.
	if (other->testStatus(OBJECT_STATUS_REPULSOR))
	{
		return	true;
	}

	if (other->isEffectivelyDead())
		return false; // no dead enemies.

	Relationship r = m_self->getRelationship(other);
	if (r != ENEMIES)
	{
		return false; // only enemies auto repulse.
	}

	if (other->isKindOf( KINDOF_STRUCTURE ))
	{
		// always pay attention to buildings that can attack
		if (other->isAbleToAttack())
			return true;
		return false;
	}

	if ( other->isKindOf( KINDOF_INERT ))
		return false;


	if ( ! other->isAbleToAttack() )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------

Bool PartitionFilterIrregularArea::allow( Object *other )
{

	return PointInsideArea2D(other->getPosition(), m_area, m_numPointsInArea);
}

//-----------------------------------------------------------------------------
Bool PartitionFilterPolygonTrigger::allow( Object *other )
{
	ICoord3D iPos;
	iPos.x = other->getPosition()->x;
	iPos.y = other->getPosition()->y;
	iPos.z = 0; // Trigger areas compare on xy only.
	return m_trigger->pointInTrigger(iPos);
}

//-----------------------------------------------------------------------------

Bool PartitionFilterPlayer::allow( Object *other )
{
	return ((m_player == other->getControllingPlayer()) == m_match);
}

//-----------------------------------------------------------------------------
Bool PartitionFilterPlayerAffiliation::allow( Object *other )
{
	Relationship rel = m_player->getRelationship(other->getTeam());
	switch (rel)
	{
		case ENEMIES:
			if (m_affiliation & ALLOW_ENEMIES) {
				return m_match;
			}
			break;

		case NEUTRAL:
			if (m_affiliation & ALLOW_NEUTRAL) {
				return m_match;
			}
			break;

		case ALLIES:
			if (m_affiliation & ALLOW_ALLIES) {
				return m_match;
			}
			break;
	}

	if (other->getControllingPlayer() == m_player) {
		return m_match;
	}

	return !m_match;
}

//-----------------------------------------------------------------------------
Bool PartitionFilterThing::allow( Object *other )
{
	return (m_tThing->isEquivalentTo(other->getTemplate()) == m_match);
}

//-----------------------------------------------------------------------------
Bool PartitionFilterGarrisonable::allow( Object *other )
{
	ContainModuleInterface *cmi = other->getContain();
	if (!cmi) {
		return !m_match;
	}

	return (cmi->isGarrisonable() == m_match);
}

//-----------------------------------------------------------------------------
Bool PartitionFilterGarrisonableByPlayer::allow( Object *other )
{
	return TheActionManager->canPlayerGarrison(m_player, other, m_commandSource) == m_match;
}

//-----------------------------------------------------------------------------
Bool PartitionFilterUnmannedObject::allow( Object *other )
{
	return (other->isDisabledByType( DISABLED_UNMANNED ) == m_match);
}

//-----------------------------------------------------------------------------
Bool PartitionFilterValidCommandButtonTarget::allow( Object *other )
{
	return (m_commandButton->isValidToUseOn(m_source, other, nullptr, m_commandSource) == m_match);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterIsFlying::allow(Object *objOther)
{
	return objOther->isUsingAirborneLocomotor();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionFilterWouldCollide::PartitionFilterWouldCollide(const Coord3D& pos, const GeometryInfo& geom, Real angle, Bool desired) :
  m_position(pos),
	m_geom(geom),
  m_angle(angle),
  m_desiredCollisionResult(desired)
{
}

//-----------------------------------------------------------------------------

Bool PartitionFilterWouldCollide::allow(Object *objOther)
{
	CollideInfo thisInfo(&m_position, m_geom, m_angle);
	CollideInfo thatInfo(objOther->getPosition(), objOther->getGeometryInfo(), objOther->getOrientation());

  Bool doesCollide;

	// invariant for all geometries: first do z collision check.
	if (thisInfo.position.z + thisInfo.geom.getMaxHeightAbovePosition() >= thatInfo.position.z &&
			thisInfo.position.z <= thatInfo.position.z + thatInfo.geom.getMaxHeightAbovePosition())
	{
		GeometryType thisGeom = m_geom.getGeomType();
		GeometryType thatGeom = objOther->getGeometryInfo().getGeomType();

		//
		// NOTE: This assumes geometry enumerations that start at GEOMETRY_FIRST AND depends on the
		// order in which they appear in the enum list
		//
		CollideTestProc collideProc = theCollideTestProcs[ (thisGeom - GEOMETRY_FIRST) * GEOMETRY_NUM_TYPES + (thatGeom - GEOMETRY_FIRST) ];
		CollideLocAndNormal cinfo;
		doesCollide = (*collideProc)(&thisInfo, &thatInfo, &cinfo);
	}
	else
	{
		// no z-intersection -> no collision.
		doesCollide = false;
	}

  return doesCollide == m_desiredCollisionResult;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterSamePlayer::allow(Object *objOther)
{
	if (m_player == objOther->getControllingPlayer())
		return true;

	return false;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterRelationship::allow(Object *objOther)
{
	Relationship r = m_obj->getRelationship(objOther);

	if ((m_flags & (1<<r)) != 0)
		return true;

	return false;
}

//-----------------------------------------------------------------------------
// GeneralsX: the owner tags allow() could pass, following Object::getRelationship and
// Team::getRelationship step by step; any doubt answers PARTITION_OWNERS_ALL (no cell skipped).
PartitionOwnerMask PartitionFilterRelationship::getCellOwnerMask()
{
	return (m_flags & SKIP_CELLS_BY_OWNER) ? cellOwnerMaskFor(m_obj, m_flags) : PARTITION_OWNERS_ALL;
}

//-----------------------------------------------------------------------------
PartitionOwnerMask PartitionFilterRelationship::cellOwnerMaskFor(const Object *obj, Int flags)
{
	// no team, or an undetected defector looking: everything is NEUTRAL
	const Team *team = obj->getTeam();
	if (team == nullptr || obj->getIsUndetectedDefector())
		return (flags & ALLOW_NEUTRAL) ? PARTITION_OWNERS_ALL : 0;

	// an undetected defector of any owner is ALLIES; an override for a particular team is per team, not per owner
	if ((flags & ALLOW_ALLIES) || team->hasOverrideTeamRelationships() || team->getControllingPlayer()->hasTeamRelationships())
		return PARTITION_OWNERS_ALL;

	// a teamless object is NEUTRAL (Team::getRelationship(nullptr))
	PartitionOwnerMask mask = (flags & ALLOW_NEUTRAL) ? gxOwnerTagBit(PARTITION_OWNER_NONE) : 0;

	// without those overrides the answer for any team depends on its controlling player only, so ask with one of theirs
	// (PlayerList holds Player(i) at index i for every i < MAX_PLAYER_COUNT; those past getPlayerCount are not skipped)
	const Int count = ThePlayerList->getPlayerCount();
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		const Player *other = (i < count) ? ThePlayerList->getNthPlayer(i) : nullptr;
		const Team *sample = other ? other->getDefaultTeam() : nullptr;
		if (sample == nullptr || sample->getControllingPlayer() != other || (flags & (1 << team->getRelationship(sample))) != 0)
			mask |= gxOwnerTagBit(i);
	}
	return mask;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
PartitionFilterAcceptOnTeam::PartitionFilterAcceptOnTeam(const Team *team) : m_team(team)
{
}

//-----------------------------------------------------------------------------
Bool PartitionFilterAcceptOnTeam::allow(Object *objOther)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)
	return (objOther->getTeam() == m_team);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
PartitionFilterAcceptOnSquad::PartitionFilterAcceptOnSquad(const Squad *squad) : m_squad(squad)
{
}

//-----------------------------------------------------------------------------

Bool PartitionFilterAcceptOnSquad::allow(Object *objOther)
{
	return (m_squad && m_squad->isOnSquad(objOther) && !objOther->isEffectivelyDead());
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Bool PartitionFilterSameMapStatus::allow(Object* objOther)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)
	return objOther->isOffMap() == m_obj->isOffMap();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Bool PartitionFilterOnMap::allow(Object* objOther)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)
	return !objOther->isOffMap();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Bool PartitionFilterAlive::allow( Object *objOther )
{
	return !objOther->isEffectivelyDead();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
PartitionFilterRejectBehind::PartitionFilterRejectBehind( Object *obj )
{
	m_obj = obj;
}

//-----------------------------------------------------------------------------

Bool PartitionFilterRejectBehind::allow( Object *other )
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)

//const Coord3D *pos = m_obj->getPosition();
//const Coord3D *dir = m_obj->getUnitDirectionVector2D();
	Vector3 dir = m_obj->getTransformMatrix()->Get_X_Vector();
	dir.Normalize();
//const Coord3D *otherPos = other->getPosition();

	Coord3D v;
	ThePartitionManager->getVectorTo( m_obj, other, FROM_CENTER_3D, v );

	Real dot = dir.X * v.x + dir.Y * v.y + dir.Z * v.z;

	if (dot > 0.0f)
		return true;

	return false;
}



//-----------------------------------------------------------------------------
PartitionFilterLineOfSight::PartitionFilterLineOfSight(const Object *obj)
{
	m_obj = obj;
}

//-----------------------------------------------------------------------------
Bool PartitionFilterLineOfSight::allow(Object *objOther)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)

	if (!ThePartitionManager->isClearLineOfSightTerrain(m_obj, *m_obj->getPosition(), objOther, *objOther->getPosition()))
		return false;

	if (TheAI && TheAI->pathfinder()->isViewBlockedByObstacle(m_obj, objOther))
		return false;

	return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionFilterPossibleToAttack::PartitionFilterPossibleToAttack(AbleToAttackType t, const Object *obj, CommandSourceType commandSource) :
	m_attackType(t),
	m_obj(obj),
	m_commandSource(commandSource)
{
}

//-----------------------------------------------------------------------------
Bool PartitionFilterPossibleToAttack::allow(Object *objOther)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)

	// we should have already filtered out isAbleToAttack!
	DEBUG_ASSERTCRASH(m_obj->isAbleToAttack(), ("if the object is unable to attack at all, you should filter that out ahead of time!"));

	CanAttackResult result = m_obj->getAbleToAttackSpecificObject( m_attackType, objOther, m_commandSource );
	if( result == ATTACKRESULT_POSSIBLE || result == ATTACKRESULT_POSSIBLE_AFTER_MOVING )
	{
		return TRUE;
	}
	return FALSE;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
PartitionFilterPossibleToEnter::PartitionFilterPossibleToEnter(const Object *obj, CommandSourceType commandSource) :
	m_obj(obj),
	m_commandSource(commandSource)
{
}

//-----------------------------------------------------------------------------
Bool PartitionFilterPossibleToEnter::allow(Object *objOther)
{
	if (!objOther || !m_obj)
		return FALSE;

	if( TheActionManager->canEnterObject( m_obj, objOther, m_commandSource, DONT_CHECK_CAPACITY ) )
		return TRUE;
	else
		return FALSE;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
PartitionFilterPossibleToHijack::PartitionFilterPossibleToHijack(const Object *obj, CommandSourceType commandSource) :
	m_obj(obj),
	m_commandSource(commandSource)
{
}

//-----------------------------------------------------------------------------
Bool PartitionFilterPossibleToHijack::allow(Object *objOther)
{
	if (!objOther || !m_obj)
		return FALSE;

	if( TheActionManager->canHijackVehicle(m_obj, objOther, m_commandSource) )
		return TRUE;
	else
		return FALSE;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
PartitionFilterLastAttackedBy::PartitionFilterLastAttackedBy(Object *obj)
{
	if (obj && obj->getBodyModule()) {
		m_lastAttackedBy = obj->getBodyModule()->getLastDamageInfo()->in.m_sourceID;
	} else {
		m_lastAttackedBy = INVALID_ID;
	}
}


//-----------------------------------------------------------------------------

Bool PartitionFilterLastAttackedBy::allow(Object *other)
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)
	if (other->getID() == m_lastAttackedBy)
		return true;

	return false;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterAcceptByObjectStatus::allow(Object *objOther)
{
	ObjectStatusMaskType status = objOther->getStatusBits();
	return status.testForAll( m_mustBeSet ) && status.testForNone( m_mustBeClear );
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterRejectByObjectStatus::allow(Object *objOther)
{
	ObjectStatusMaskType status = objOther->getStatusBits();

	return !( status.testForAll( m_mustBeSet ) && status.testForNone( m_mustBeClear ) );
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterAcceptByKindOf::allow(Object *objOther)
{
	return objOther->isKindOfMulti(m_mustBeSet, m_mustBeClear);
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterRejectByKindOf::allow(Object *objOther)
{
	return !objOther->isKindOfMulti(m_mustBeSet, m_mustBeClear);
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
Bool PartitionFilterStealthedAndUndetected::allow( Object *objOther )
{
	// objOther is guaranteed to be non-null, so we don't need to check (srj)

	Bool stealthed = objOther->testStatus( OBJECT_STATUS_STEALTHED );
	Bool detected = objOther->testStatus( OBJECT_STATUS_DETECTED );
	Bool disguised = objOther->testStatus( OBJECT_STATUS_DISGUISED );

	if( stealthed && !detected )
	{
		if( !objOther->isKindOf( KINDOF_DISGUISER ) )
		{
			//We have a stealthed & undetected object.
			return m_allow;
		}
		else if( disguised )
		{
			//Exception case -- bomb trucks can't be considered stealthed units when they are disguised as the enemy.

      StealthUpdate *update = objOther->getStealth();

			if( update && update->isDisguised() )
			{
				Player *ourPlayer = m_obj->getControllingPlayer();
				Player *otherPlayer = ThePlayerList->getNthPlayer( update->getDisguisedPlayerIndex() );
				if( ourPlayer && otherPlayer )
				{
					if( ourPlayer->getRelationship( otherPlayer->getDefaultTeam() ) == ENEMIES )
					{
						//Our stealthed & undetected object is disguised as a unit perceived to be our enemy,
						//therefore it's not considered to be stealthed!
						return !m_allow;
					}
					//Our object is disguised as a "friendly" object, therefore it's considered to be stealthed.
					return m_allow;
				}
			}
			//Our bomb truck is not disguised therefore it's not stealthed.
			return !m_allow;
		}
	}
	else
	{
		//This handles neutral containers that hold stealth units. This specifically fixes a bug where hunt scripts would ignore
		//this case -- units would acquire the building Jarmen Kell occupied even though it was not stealth detected.
		const ContainModuleInterface* contain = objOther->getContain();
		if( contain && !contain->getContainedItemsList()->empty() )
		{
			const Player* victimApparentController = contain->getApparentControllingPlayer( m_obj->getControllingPlayer() );
			//Check if it's stealthed!
			if( contain->getStealthUnitsContained() == contain->getContainCount() )
			{
				//Check if the first object inside is detected (if one is detected, all are detected).
				ContainedItemsList::const_iterator it = contain->getContainedItemsList()->begin();
				Object *member = (*it);
				if( member && !(*it)->getStatusBits().test( OBJECT_STATUS_DETECTED ) )
				{
					//Finally check the relationship!
					if( victimApparentController && m_obj->getTeam()->getRelationship( victimApparentController->getDefaultTeam() ) == ENEMIES )
					{
						//Our object is a neutral building garrisoned by enemy units we can't see, therefore it is stealthed.
						return m_allow;
					}
				}
			}
		}
	}

	//The unit is not stealthed!
	return !m_allow;
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
static int cellValueProc(PartitionCell* cell, void* userData)
{
	CellValueProcParms *parms = (CellValueProcParms*) userData;

	UnsignedInt val = 0;
	for (Int i = 0; i < MAX_PLAYER_COUNT; ++i) {
		if (BitIsSet(parms->allowedPlayersMasks, parms->allPlayersMask[i])) {
			if (parms->valueType == VOT_CashValue) {
				val += cell->getCashValue(i);
			} else {
				val += cell->getThreatValue(i);
			}
		}
	}

	if ((val > parms->valueRequired && parms->greaterThan) ||
			(val < parms->valueRequired && !parms->greaterThan)) {
		return 1;
	}

	return 0;
}

// -----------------------------------------------------------------------------
// GeneralsX: The eight hLine* scanlines below walked x1..x2 whole and skipped the cells off the grid
// one by one, so a wide circle around a centre outside the grid cost its full span on every in-grid row - billions of
// skipped steps. Each now clips the span to the grid first: the same cells, in the same order.
void hLineAddLooker(Int x1, Int x2, Int y, void *playerIndexVoid)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	// GeneralsX: 64-bit safe pointer cast
	Int playerIndex = static_cast<Int>(reinterpret_cast<intptr_t>(playerIndexVoid));

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;
		cell->addLooker(playerIndex);
	}
}

// -----------------------------------------------------------------------------
void hLineRemoveLooker(Int x1, Int x2, Int y, void *playerIndexVoid)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	// GeneralsX: 64-bit safe pointer cast
	Int playerIndex = static_cast<Int>(reinterpret_cast<intptr_t>(playerIndexVoid));

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;
		cell->removeLooker(playerIndex);
	}
}

// -----------------------------------------------------------------------------
void hLineAddShrouder(Int x1, Int x2, Int y, void *playerIndexVoid)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	// GeneralsX: 64-bit safe pointer cast
	Int playerIndex = static_cast<Int>(reinterpret_cast<intptr_t>(playerIndexVoid));

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;
		cell->addShrouder( playerIndex );
	}
}

// -----------------------------------------------------------------------------
void hLineRemoveShrouder(Int x1, Int x2, Int y, void *playerIndexVoid)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	// GeneralsX: 64-bit safe pointer cast
	Int playerIndex = static_cast<Int>(reinterpret_cast<intptr_t>(playerIndexVoid));

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;
		cell->removeShrouder( playerIndex );
	}
}

// -----------------------------------------------------------------------------
void hLineAddThreat(Int x1, Int x2, Int y, void *threatValueParms)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	ThreatValueParms *parms = (ThreatValueParms*)threatValueParms;

	Real distance;
	Real mulVal = 1.0f;

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;

		distance = sqrt( gx_square(x - parms->xCenter) + gx_square(y - parms->yCenter) );
		mulVal = 1 - distance / parms->radius;
		if (mulVal < 0.0f)
			mulVal = 0.0f;
		else if (mulVal > 1.0f)
			mulVal = 1.0f;

		cell->addThreatValue( parms->playerIndex, REAL_TO_UNSIGNEDINT(parms->threatOrValue * mulVal) );
	}
}

// -----------------------------------------------------------------------------
void hLineRemoveThreat(Int x1, Int x2, Int y, void *threatValueParms)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	ThreatValueParms *parms = (ThreatValueParms*)threatValueParms;

	Real distance;
	Real mulVal = 1.0f;

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;

		distance = sqrt( gx_square(x - parms->xCenter) + gx_square(y - parms->yCenter) );
		mulVal = 1 - distance / parms->radius;
		if (mulVal < 0.0f)
			mulVal = 0.0f;
		else if (mulVal > 1.0f)
			mulVal = 1.0f;

		cell->removeThreatValue( parms->playerIndex, REAL_TO_UNSIGNEDINT(parms->threatOrValue * mulVal) );
	}
}

// -----------------------------------------------------------------------------
void hLineAddValue(Int x1, Int x2, Int y, void *threatValueParms)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	ThreatValueParms *parms = (ThreatValueParms*)threatValueParms;

	Real distance;
	Real mulVal = 1.0f;

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;

		distance = sqrt( gx_square(x - parms->xCenter) + gx_square(y - parms->yCenter) );
		mulVal = 1 - distance / parms->radius;
		if (mulVal < 0.0f)
			mulVal = 0.0f;
		else if (mulVal > 1.0f)
			mulVal = 1.0f;

		cell->addCashValue( parms->playerIndex, REAL_TO_UNSIGNEDINT(parms->threatOrValue * mulVal) );
	}
}

// -----------------------------------------------------------------------------
void hLineRemoveValue(Int x1, Int x2, Int y, void *threatValueParms)
{
	if (y < 0 || y >= ThePartitionManager->m_cellCountY || x1 >= ThePartitionManager->m_cellCountX || x2 < 0)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 >= ThePartitionManager->m_cellCountX)
		x2 = ThePartitionManager->m_cellCountX - 1;

	ThreatValueParms *parms = (ThreatValueParms*)threatValueParms;

	Real distance;
	Real mulVal = 1.0f;

	PartitionCell* cell = &ThePartitionManager->m_cells[y * ThePartitionManager->m_cellCountX + x1];	// yes, this could be invalid. we'll skip the bad ones.
	for (Int x = x1; x <= x2; ++x, ++cell)
	{
		if (x < 0 || x >= ThePartitionManager->m_cellCountX)
			continue;

		distance = sqrt( gx_square(x - parms->xCenter) + gx_square(y - parms->yCenter) );
		mulVal = 1 - distance / parms->radius;
		if (mulVal < 0.0f)
			mulVal = 0.0f;
		else if (mulVal > 1.0f)
			mulVal = 1.0f;

		cell->removeCashValue( parms->playerIndex, REAL_TO_UNSIGNEDINT(parms->threatOrValue * mulVal) );
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
SightingInfo::SightingInfo()
{
	reset();
}

// ------------------------------------------------------------------------------------------------
/** Resetting sighting info */
// ------------------------------------------------------------------------------------------------
void SightingInfo::reset()
{
	m_where.zero();
	m_howFar = 0.0f;
	m_forWhom = 0;
	m_data = 0;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool SightingInfo::isInvalid() const
{
	return m_howFar == 0.0f;
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void SightingInfo::crc( Xfer *xfer )
{

}

// ------------------------------------------------------------------------------------------------
/** Xfer Method
	* Version Info:
	* 1: Initial version */
// ------------------------------------------------------------------------------------------------
void SightingInfo::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// where
	xfer->xferCoord3D( &m_where );

	// how far
	xfer->xferReal( &m_howFar );

	// for whom
	xfer->xferUser( &m_forWhom, sizeof( PlayerMaskType ) );

	// how much
	xfer->xferUnsignedInt( &m_data );

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void SightingInfo::loadPostProcess()
{

}

// ------------------------------------------------------------------------------------------------
SightingInfo::~SightingInfo()
{

}

