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

// AIPathfind.cpp
// AI pathfinding system
// Author: Michael S. Booth, October 2001
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <chrono>	// GeneralsX: A* search probe only - see the GXPathSearchProbe block below.

#include "GameLogic/AIPathfind.h"

#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/CRCDebug.h"
#include "Common/GlobalData.h"
#include "Common/LatchRestore.h"
#include "Common/ThingTemplate.h"
#include "Common/ThingFactory.h"

#include "GameClient/Line2D.h"

#include "GameLogic/AI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/PhysicsUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"
#if RETAIL_COMPATIBLE_PATHFINDING
#include "GameClient/InGameUI.h"
#include "GameClient/GameText.h"
#include "Common/GameAudio.h"
#include "Common/MiscAudio.h"
#endif

#include "Common/UnitTimings.h" //Contains the DO_UNIT_TIMINGS define jba.

#define no_INTENSE_DEBUG

#define DEBUG_QPF

#ifdef INTENSE_DEBUG
#include "GameLogic/ScriptEngine.h"
#endif

#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/GameState.h"	// GeneralsX: SC_INVALID_DATA for the queue save block

//------------------------------------------------------------------------------ Performance Timers
#include "Common/PerfMetrics.h"

//-------------------------------------------------------------------------------------------------


static inline Bool IS_IMPASSABLE(PathfindCell::CellType type) {
	// Return true if cell is impassable to ground units. jba. [8/18/2003]
	if (type==PathfindCell::CELL_IMPASSABLE) {
		return true;
	}
	if (type==PathfindCell::CELL_OBSTACLE) {
		return true;
	}
	if (type==PathfindCell::CELL_BRIDGE_IMPASSABLE) {
		return true;
	}
	return false;
}


struct TCheckMovementInfo
{
	// Input
	ICoord2D					cell;
	PathfindLayerEnum layer;
	Int								radius;
	Bool							centerInCell;
	Bool							considerTransient;
	LocomotorSurfaceTypeMask acceptableSurfaces;
	// Output
	Int								allyFixedCount;
	Bool							enemyFixed;
	Bool							allyMoving;
	Bool							allyGoal;
};

//-----------------------------------------------------------------------------------
static Int frameToShowObstacles;

constexpr const UnsignedInt ZONE_UPDATE_FREQUENCY = 300;
constexpr const UnsignedInt MAX_CELL_COUNT = 500;
constexpr const UnsignedInt MAX_ADJUSTMENT_CELL_COUNT = 400;
constexpr const UnsignedInt MAX_SAFE_PATH_CELL_COUNT = 2000;

// GeneralsX: findClosestPath was the ONLY A* in this file with no bound on
// how much it may search. checkPathCost stops at MAX_CELL_COUNT (500), patchPath at CELL_LIMIT
// (2000), findSafePath at MAX_SAFE_PATH_CELL_COUNT (2000), findAttackPath at ATTACK_CELL_LIMIT
// (2500). findClosestPath ran `while (!m_openList.empty())` to exhaustion, and its contract is
// "closest reachable" rather than "reachable", so an unreachable goal does not stop it early - it
// expands every reachable cell on the map. A 5760x5760 map at PATHFIND_CELL_SIZE 10 is 331,776
// cells against a 30,000-entry PathfindCellInfo pool, so one such call empties the pool by
// construction and every OTHER allocation in that frame fails. Measured 2026-08-21, see
// free fell 24,102 -> 0 -> 24,155 inside one
// 60-frame window with standing occupancy unmoved, and all 1,696 allocation failures of the run
// came from this one function and no other site.
//
// TWO bounds, because the expansion count does not measure what is consumed. Every expansion first
// walks a straight line to the goal (examineNeighboringCells -> iterateCellsAlongLine ->
// examineCellsCallback) and allocates an info per cell on that line, and none of that appears in
// the value examineNeighboringCells returns. findClosestPath also calls setAllPassable() when the
// hierarchical path fails, which deletes the `!m_zoneManager.isPassable` abort that would otherwise
// stop that line walk early - so the prepass runs all the way to the goal on EVERY expansion. A cap
// on expansions alone would leave most of the allocation unbounded, so the pool take is bounded
// directly, at the one choke point every take passes through (PathfindCellInfo::getACellInfo).
//
// THE NUMBERS. 2500 expansions is ATTACK_CELL_LIMIT, the most permissive sibling cap in this file,
// and sits just above this function's own long-standing "we are far past the closest cell" test at
// count>2000 below - so the cap can only bite on searches the function already considers extreme.
// 5000 infos is one search's own ceiling, a sixth of the original 30,000-entry CELL_INFOS_TO_ALLOCATE. The cross-check is
// PATHFIND_CELLS_PER_FRAME, which is 5000 at BaseFps and 2500 at the 60 Hz the shipping presets
// build (GX_LOGIC_60HZ, both are 150,000 cells/second): one search may take at most one 30 Hz
// frame's worth of the servicing the whole queue pass gets, whatever the pool size happens to be.
//
// A CEILING ON WHAT ONE SEARCH TAKES IS NOT A FLOOR UNDER WHAT IT LEAVES, and only the floor
// actually states the property being defended. Two ways the ceiling alone fails, both real:
// standing occupancy is not fixed (12-player runs measured 17,700-19,400 of 30,000 held, so ~10,600
// free - 5000 fits, but a denser map or an obstacle info per cell closes that margin), and once
// s_firstFree is null a FAILED getACellInfo takes nothing, so a ceiling counting only successes
// stops advancing exactly when the pool is empty - it stops binding at the moment it is needed.
// Hence PATHFIND_SEARCH_FREE_RESERVE, an absolute floor read from the live free count, and hence
// the take counter below counts ATTEMPTS rather than successes so that it, too, keeps advancing
// when the pool is dry.
//
// 5000 reserved is the same sixth of the pool, and it is what the OTHER allocation sites live on:
// setTypeAsObstacle, setGoalUnit, setPosUnit and setGoalAircraft are the calls that were failing
// collaterally when one search drained the pool, and they are what the reserve keeps working. It is
// also more than 4x the worst case for a single expansion - iterateCellsAlongLine walks at most
// max(|dx|,|dy|) <= 575 cells on a 576x576 map plus 8 neighbours - so no allocation INSIDE this
// search can fail: the guard trips at least one whole expansion before the pool could empty. That
// is what makes the failure-blind ceiling above harmless as a second bound rather than load-bearing.
//
// If free is ALREADY below the reserve when the search starts, the guard is false on the first
// iteration and the search never grows: the loop drains the open list it was seeded with and leaves
// by the same closest-found or failure exit as always. It cannot spin - every iteration pops one
// cell and puts it on the closed list - and it cannot deadlock, because nothing here waits.
//
// Neither number is fitted to a measured distribution of per-search allocation, because none exists
// - GX_PATHTRACE reports maxcells per frame and is the instrument that would refine them.
//
// SIM-VISIBLE. A bounded search can return a different "closest" cell than an unbounded one, so
// two peers running different values here diverge. Both constants live in this file, which is
// inside the SimID source digest (resources/gitinfo/simsourcedigest_watcher.cmake digests
// Core/GameEngine/Source/GameLogic), so sourceID moves with them and a mismatched peer is refused
// at join rather than desynced.
constexpr const UnsignedInt MAX_CLOSEST_PATH_CELL_COUNT = 2500;
constexpr const UnsignedInt MAX_CLOSEST_PATH_INFO_COUNT = 5000;

// GeneralsX: internalFindPath is the SECOND A* in this file with no bound
// on how much of the pool it may hold, and once e22363ac8 bounded findClosestPath it became the
// ONLY site that still fails. Measured on GXCauldron12ZHv1, seed 12345, 20,000 frames, 12 players
// with the findClosestPath bound in, bucket 5
// GXPATH_CLOSEST went 840 -> 0 and bucket 1 GXPATH_INTERNAL_FIND went 1,593 -> 2,081 - every one of
// the 2,081 remaining failures - and `low`, the free-list watermark, still reached 0 in both arms.
// A ceiling on what one search TAKES is not a floor under what the pool HAS: the reserve
// findClosestPath leaves is simply what the next unbounded search walks into and drains.
//
// SAME MECHANISM, SAME SHAPE. internalFindPath drives `while (!m_openList.empty())` and calls
// examineNeighboringCells with a non-null goalCell, so every expansion first walks a straight line
// to the goal (iterateCellsAlongLine -> examineCellsCallback) and allocates an info per cell on
// that line - none of which appears in the cellCount that function returns. Pathfinder::findPath
// calls setAllPassable() when the hierarchical path fails, exactly as findClosestPath does, which
// removes the isPassable abort that would end that walk early, so the prepass runs all the way to
// the goal on EVERY expansion. So internalFindPath grows only while the pool is above
// PATHFIND_SEARCH_FREE_RESERVE (pathfindPoolAboveFloor), the floor every other allocation site
// lives on.
//
// GeneralsX: THE FLOOR ONLY: internalFindPath has no per-search share.
// cac8ebc65 also stopped it at 5000 infos taken per search (MAX_INTERNAL_PATH_INFO_COUNT). That
// share turned reachable goals into failures - this search knows only "the goal", so it returns
// nullptr, and the bounded findClosestPath fallback answers with the best cell it has seen, which
// for a goal across a cliff is the cliff edge (wh1ter0se's W11 report, 27/09/2026). The WHAT TRUNCATION
// COSTS argument the share was written with ("the fallback that already exists answers instead")
// holds for an unreachable goal and fails for a reachable one. Measured on W5's w5g1 game with the
// hierarchical scan fixed (processHierarchicalCell): 730 findPath failures cut by the share and 563
// fallback paths ending where the unit stood with it, 11 and 28 with the floor alone; and four of
// the five scripted ridge crossings still failed with it. The floor alone keeps what the share was
// for: no search can take the pool below the reserve, so setTypeAsObstacle, setGoalUnit, setPosUnit
// and setGoalAircraft keep working. Retail has no bound here at all.

// GeneralsX: RENAMED from MAX_CLOSEST_PATH_FREE_RESERVE; the value and
// every word of the reasoning above are unchanged. The floor is a property of the POOL, not of one
// search, and it has to be ONE number: two searches naming two reserves means the lower one is the
// real floor and the higher one is decoration. Both bounded searches now read it through
// pathfindSearchMayGrow(), which is where the whole predicate lives.
constexpr const UnsignedInt PATHFIND_SEARCH_FREE_RESERVE = 5000;

// GeneralsX: A per-frame BUDGET, so it takes the INVERSE of the usual frame
// conversion: to keep the same cells-per-SECOND (and therefore the same wall-clock path latency and
// the same CPU share) the per-FRAME allowance must shrink as the tick rate rises. 5000 at 30 Hz,
// 2500 at 60 Hz - both are 150,000 cells/second.
//
// This is a deliberate choice, not a mechanical unit change. Leaving it at 5000 would double
// pathfinding throughput AND double its CPU cost per second; paths would resolve in half the
// wall-clock time, which is a gameplay change, and the extra cost would land hardest on the peer
// least able to absorb it. In lockstep the slowest peer sets the rate for everyone, and the Linux
// host was measured at 86% of one core at 30 Hz - it has no headroom to give.
constexpr const UnsignedInt PATHFIND_CELLS_PER_FRAME = (5000 * (UnsignedInt)BaseFps) / (UnsignedInt)LOGICFRAMES_PER_SECOND; // Number of cells we will search pathfinding per frame.
// GeneralsX: The retail open-list insertion walk's STEP CAP. It used to read
// PATHFIND_CELLS_PER_FRAME, which is also the per-frame queue BUDGET - and d11d68de5 correctly halved
// the budget at 60 Hz, which silently halved this cap too. The cap is a count of list cells, not
// time: at 2500 an open list longer than 2500 inserts at the wrong place and expands in a different
// order from retail. Split so only the budget scales. At 30 Hz the name is a macro for the original
// constant, so the 30 Hz build preprocesses to the retail expression token for token.
#if GX_LOGIC_60HZ
constexpr const UnsignedInt PATHFIND_OPEN_WALK_CAP = 5000;
#else
#define PATHFIND_OPEN_WALK_CAP PATHFIND_CELLS_PER_FRAME
#endif
// GeneralsX: THE POOL WAS SIZED FOR RETAIL'S 8 PLAYERS; 12 FILL IT AND EVERY GROUND
// SEARCH THEN FAILS. wh1ter0se's own Crater Ring replay (12 players, 38,810 frames) measured with the [GXCENSUS]
// probe: obstacle footprints alone hold 9,215 infos at frame 1,800 and 22,000 from frame 21,000, unit goal
// and position stamps another 8,000-9,500, so STANDING occupancy is 24,967 at frame 16,200 and 27,000 from
// frame 28,000 of a 30,000 pool - free 5,000 at frame 16,000 and ~3,000 after. Nothing leaks (0 orphaned
// open/closed infos, 0 stamps naming a dead object, at every sample), it is all live state. That is BELOW
// PATHFIND_SEARCH_FREE_RESERVE (5,000), whose guard lets internalFindPath / findClosestPath grow only while
// free > reserve, so from frame ~16k no search can expand at all: units accept an order and never move,
// which is exactly "then all my units froze". Objects flatline near 72 from that frame in his game.
// 100,000 keeps the reserve meaningful (~70,000 free at 12 players, ~55,000 at 16) for ~6 MB.
// SIM-VISIBLE (a search that failed now succeeds); the constant is inside the SimID digest.
// GeneralsX: ONE internalFindPath MAY HOLD AT MOST 20,000 INFOS. With the pool at 100,000 the
// floor alone lets a single synchronous search take ~70,000 (measured: 70,788 cells, 77 ms, one logic frame; the
// pool-30,000 build peaked at 13,264 cells / 7 ms). 20,000 is what a search could take from the old pool at match
// start (25,000 free - the 5,000 floor), i.e. retail's effective ceiling, and 4x the 5,000 share removed on 27/09.
constexpr const UnsignedInt MAX_INTERNAL_PATH_INFO_COUNT = 20000;
constexpr const UnsignedInt CELL_INFOS_TO_ALLOCATE = 100000;

// ============================================================================================
// GeneralsX: A* search probe.
//
// WHY IT EXISTS. PERF_LOCAL_FRAMETIME_2026-08-13 located the stutter with a differential
// profile: Pathfinder::examineNeighboringCells goes from 2.10% of the main thread outside the
// hitch cluster to 21.51% inside it. That is a correlation between a SAMPLE WINDOW and a
// symbol. It cannot distinguish "one enormous search overran one frame" from "many ordinary
// searches happened to land in the same frames" - and those two have completely different
// fixes. This measures the difference directly: per logic frame, how many A* searches ran,
// how many cells they expanded, and how large and how long the single worst one was.
//
// OBSERVATION ONLY, and it has to stay that way: this file is inside sourceID and its
// behaviour is hashed into the per-frame CRC. The probe
//   * READS m_cumulativeCellsAllocated and never writes it - that member is xfer'd (see
//     Pathfinder::xfer) and is therefore save/CRC state,
//   * keeps its totals in file statics that no simulation code ever reads,
//   * takes no branch the simulation can observe, and
//   * is OFF unless GX_PATHTRACE is set, costing one bool test per search otherwise.
// The claim that it cannot move the CRC is NOT asserted on those grounds - it is proved by
// running the same replay with and without GX_PATHTRACE and diffing the [GXCRC] streams.
// ============================================================================================

// GeneralsX: A function-local static is guarded: every call is a
// thread-safe-initialisation check, and this is now asked hundreds of millions of times in a
// match, where it cost 173 leaf samples of a 25 s profile doing nothing at all. A file-scope
// tri-state costs one load and a perfectly predicted branch instead.
static Int s_gxPathTraceOn = -1;

// GeneralsX: The A* cost overflow counter - see gxNotePathCost in AIPathfind.h.
// Only a GX_PATHCOST_COUNT build has it, and only its [GXPATH] lines carry cmax=/cwrap=.
#if defined(GX_PATHCOST_COUNT)
UnsignedInt g_gxPathCostMax = 0, g_gxPathCostWraps = 0;
	#define GX_PATHCOST_FMT " cmax=%u cwrap=%u"
	#define GX_PATHCOST_ARGS , g_gxPathCostMax, g_gxPathCostWraps
#else
	#define GX_PATHCOST_FMT ""
	#define GX_PATHCOST_ARGS
#endif

static inline Bool gxPathTraceEnabled()
{
	if (s_gxPathTraceOn < 0)
	{
		const char *env = getenv("GX_PATHTRACE");
		s_gxPathTraceOn = (env != nullptr && env[0] != '0') ? 1 : 0;
		// GeneralsX PF27 06/10/2026: GX_PATHTRACE=time reports [GXPATH] but does not re-verify memo hits. The verify
		// recomputes every checkForMovementInSearch / clearCellForDiameter hit, which inflated measured search costs
		// (a 3,071-cell search made 379,075 checks, 98.5% of them hits) - and a kernel that skips repeats would look
		// faster than it is against a legacy arm paying for them. Timing A/Bs use =time.
		if (env != nullptr && strcmp(env, "time") == 0)
			s_gxPathTraceOn = 2;
	}
	return s_gxPathTraceOn != 0;
}

#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: GX_PATHIDXCHECK: every indexed open-list insertion is also walked
// and the two landing points compared (idxbad= in [GXPATH], which needs GX_PATHTRACE to print). Observation
// only, like GX_PATHTRACE, and kept apart from it so a timing run does not pay for the walk.
// Test builds only (-DGX_TEST_INSTRUMENTS=ON; 29/10/2026): a shipping build's idxbad= is always 0.
static Int s_gxPathIdxCheckOn = -1;

static inline Bool gxPathIdxCheckEnabled()
{
	if (s_gxPathIdxCheckOn < 0)
	{
		const char *env = getenv("GX_PATHIDXCHECK");
		s_gxPathIdxCheckOn = (env != nullptr && env[0] != '0') ? 1 : 0;
	}
	return s_gxPathIdxCheckOn != 0;
}
#endif

/// GX_PATHTRACE on and not =time: the memo hit verification (cfmbad / clrbad) runs.
static inline Bool gxPathTraceVerify()
{
	return gxPathTraceEnabled() && s_gxPathTraceOn == 1;
}

// GeneralsX: A prefetch is a hint with no architectural effect, so
// it cannot change what the simulation computes on any platform - including the one where it
// expands to nothing.
#if defined(__GNUC__) || defined(__clang__)
	#define GX_PREFETCH(p) __builtin_prefetch((const void *)(p))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 && (defined(_M_IX86) || defined(_M_X64))
	#include <xmmintrin.h>
	#define GX_PREFETCH(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#else
	#define GX_PREFETCH(p) ((void)0)
#endif

// Per-logic-frame accumulators, reported and reset by processPathfindQueue().
static UnsignedInt s_gxPathSearches = 0;   // A* searches begun since the last report
static UnsignedInt s_gxPathCells    = 0;   // cells expanded across all of them
static UnsignedInt s_gxPathMaxCells = 0;   // cells expanded by the single largest
static double      s_gxPathUs       = 0.0; // wall microseconds in all of them
static double      s_gxPathMaxUs    = 0.0; // wall microseconds of the single longest
static Int         s_gxPathMaxSite  = -1;  // call site that produced the longest
static Int         s_gxPathDepth    = 0;   // nesting guard, see below

// ============================================================================================
// GeneralsX: Hot-path counters for the two leaves that own the
// pathfinder, and the evidence needed to decide what may safely replace them.
//
// Measured on this tree at 4aef0d257, 12000-frame 8-player twilight-flame replay, 25 s of
// sample(1) on the main thread: checkForMovement 2893 leaf samples,
// forwardInsertionSortRetailCompatible 2517, examineCellsCallback 1460, iterateCellsAlongLine
// 690. Between them that is essentially the whole simulation. A profile says WHERE the time is;
// it does not say what would remove it, and these counters are what does:
//
//   ins/walk/wmax/cap  - how far the retail open-list walk really goes, and whether its
//                        PATHFIND_OPEN_WALK_CAP step cap ever truncates it. The cap is the
//                        only thing that makes the retail insertion point differ from
//                        "after the last cell whose cost <= mine", which is what the cheaper
//                        tail-side walk finds, so "cap never fires" is the whole safety
//                        argument for using the cheaper one.
//   rev                - what the same insertions would have cost from the tail end.
//   cfm/cfmrep         - checkForMovement calls from the two A* sites, and how many of them
//                        repeat a (cell, layer) already answered inside the SAME search.
//                        Every expansion re-walks a Bresenham line to the goal, and those
//                        lines converge, so the same cells are asked over and over.
//   cfmbad             - the failure arm. While GX_PATHTRACE is set the memo is filled in and
//                        CHECKED against the freshly computed answer on every repeat, and the
//                        fresh answer is the one returned. A single non-zero here kills the
//                        idea; zero over a whole match is the evidence that the answer really
//                        is constant within a search.
//
// All of it is off unless GX_PATHTRACE is set.
// ============================================================================================

static UnsignedInt s_gxInsN        = 0;    // retail open-list insertions
static UnsignedInt s_gxInsSteps    = 0;    // forward walk steps across them
static UnsignedInt s_gxInsMax      = 0;    // steps taken by the single worst insertion
static UnsignedInt s_gxInsCap      = 0;    // insertions where the step cap truncated the walk
static UnsignedInt s_gxInsRevSteps = 0;    // steps the same insertions would cost from the tail
static UnsignedInt s_gxInsLenMax   = 0;    // longest open list seen at insertion time
static UnsignedInt s_gxInsCorrupt  = 0;    // walks that stopped on a cell with no info

static UnsignedInt s_gxCfmCalls    = 0;    // checkForMovement calls from the two A* sites
static UnsignedInt s_gxCfmRepeats  = 0;    // of those, ones already answered in this search
static UnsignedInt s_gxCfmBad      = 0;    // repeats whose remembered answer did NOT match

// The memo itself. Sized to the pathfind grid on first use and never resized (the grid is
// allocated once per map, and Pathfinder::reset() drops it). One entry per cell:
//
//   m_gen   - the search this answer belongs to. Searches are numbered by
//             Pathfinder::cleanOpenAndClosedLists(), which every search calls on every exit
//             path, so an entry is only ever consulted inside the search that wrote it.
//   m_layer - part of the key, not the value: the same cell is asked about per layer.
//   m_out   - the four TCheckMovementInfo outputs, packed, plus the return value.
struct GXMoveMemoEntry
{
	UnsignedInt    m_gen;
	UnsignedShort  m_allyFixedCount;
	UnsignedByte   m_layer;
	UnsignedByte   m_out;
};

enum
{
	GXMOVE_RESULT      = 0x01,
	GXMOVE_ALLY_MOVING = 0x02,
	GXMOVE_ALLY_GOAL   = 0x04,
	GXMOVE_ENEMY_FIXED = 0x08
};

static GXMoveMemoEntry *s_gxMoveMemo      = nullptr;
static UnsignedInt      s_gxMoveMemoCells = 0;   // entries allocated
static Int              s_gxMoveMemoStrideY = 0; // (extent.hi.y + 1)
static UnsignedInt      s_gxMoveGen       = 1;   // current search number, never reused
// The parameter tuple the memo currently holds answers for. checkForMovement's result depends
// on the object and its footprint as well as the cell, and although both are fixed for the
// duration of one search, nothing in the type system says so - so they are part of the key and
// a change to any of them starts a new generation instead of silently returning a wrong answer.
static const Object    *s_gxMoveObj       = nullptr;
static Int              s_gxMoveRadius    = -1;
static Bool             s_gxMoveCenter    = false;
static Bool             s_gxMoveTransient = false;
static UnsignedInt      s_gxMoveSurfaces  = 0;

// ============================================================================================
// GeneralsX: clearCellForDiameter memo, one findGroundPath at a time.
//
// WHY. The skirmish AI's attack waves (SKIRMISH_MOVE_TO_APPROACH_PATH in its team scripts) send
// each wave through AIGroup::friend_computeGroundPath -> findGroundPath: a cross-map A* with a
// 6-cell footprint, 12-19k cells expanded, 10-50 ms in ONE logic tick, two to four waves at once
// every 30 s in a late game: the periodic slow tick measured 26/09/2026 (sample(1): ~85% of
// that time is clearCellForDiameter). Every expanded cell asks it of all 8 neighbours,
// the line walk toward the goal asks it again, and the path optimiser asks it along every line,
// so the same cell is asked many times per search, and each ask scans up to 32 cells and
// recurses to 4, 2 and 0 when the footprint is not clear.
//
// WHY IT CANNOT CHANGE AN ANSWER. clearCellForDiameter is reached only from inside findGroundPath
// (its own calls, groundCellsCallback, and buildGroundPath -> optimizeGroundPath ->
// isGroundPathPassable -> groundPathPassableCallback). Its answer reads a cell's type, fence
// flag, unit flags and the fixed unit's crushable level; a search writes none of them (it writes
// only the open/closed/cost/parent fields of the cell info, and allocateInfo keeps an occupied
// cell's info). So inside one findGroundPath a (cell, layer, diameter, crusher) key has one
// answer. The memo serves only while a findGroundPath is running, and every findGroundPath
// starts a new generation, so no answer outlives the call that computed it.
//
// GX_PATHTRACE is the failure arm, as for the checkForMovement memo above: every hit is
// recomputed and compared (clrbad= in [GXPATH]), and the fresh answer is the one returned.
// ============================================================================================
struct GXClearMemoEntry
{
	UnsignedInt m_gen;
	UnsignedByte m_layer;
	UnsignedByte m_diameter;
	UnsignedByte m_crusher;
	UnsignedByte m_result;
};
static GXClearMemoEntry *s_gxClearMemo        = nullptr;
static UnsignedInt       s_gxClearMemoCells   = 0;
static Int               s_gxClearMemoStrideY = 0;
static UnsignedInt       s_gxClearGen         = 1;     // one per findGroundPath, never reused
static Bool              s_gxClearActive      = false; // a findGroundPath is running
static UnsignedInt       s_gxClrCalls = 0, s_gxClrHits = 0, s_gxClrBad = 0;	// GX_PATHTRACE only
static UnsignedInt       s_gxFastBad = 0;	// GeneralsX PF27: checkForMovementInRect fast scan vs the loop (GX_PATHTRACE verify), never reset

// Call-site tags, so the report names which entry point produced the worst search.
enum GXPathSite
{
	GXPATH_INTERNAL_FIND      = 1,
	GXPATH_GROUND             = 2,
	GXPATH_HIERARCHICAL       = 3,
	GXPATH_CHECK_COST         = 4,
	GXPATH_CLOSEST            = 5,
	GXPATH_MOVE_AWAY          = 6,
	GXPATH_PATCH              = 7,
	GXPATH_ATTACK             = 8,
	GXPATH_SAFE               = 9
};

// Scoped so that the many `return`s from inside the A* loops are all accounted for.
// Only the OUTERMOST probe records: a search that calls another search is still one
// uninterruptible unit of work as far as the frame is concerned, and counting the inner
// one separately would both double-count its cells and understate the real worst case.
class GXPathSearchProbe
{
public:
	GXPathSearchProbe(const Int &cellCounter, Int site)
	 : m_cells(&cellCounter), m_cellsAtStart(cellCounter), m_site(site),
	   m_outermost(s_gxPathDepth == 0)
	{
		++s_gxPathDepth;
		if (m_outermost && gxPathTraceEnabled())
			m_start = std::chrono::steady_clock::now();
	}

	~GXPathSearchProbe()
	{
		--s_gxPathDepth;
		if (!m_outermost || !gxPathTraceEnabled())
			return;
		const double us = std::chrono::duration<double, std::micro>(
		                      std::chrono::steady_clock::now() - m_start).count();
		// m_cumulativeCellsAllocated only advances when a search tears its open/closed lists
		// down (cleanOpenAndClosedLists), which happens on every exit path of every search
		// below - so the delta across this scope is the cells THIS search touched.
		const Int delta = *m_cells - m_cellsAtStart;
		const UnsignedInt cells = (delta > 0) ? (UnsignedInt)delta : 0u;

		++s_gxPathSearches;
		s_gxPathCells += cells;
		s_gxPathUs    += us;
		if (cells > s_gxPathMaxCells)
			s_gxPathMaxCells = cells;
		if (us > s_gxPathMaxUs)
		{
			s_gxPathMaxUs   = us;
			s_gxPathMaxSite = m_site;
		}
	}

private:
	const Int *m_cells;
	Int        m_cellsAtStart;
	Int        m_site;
	Bool       m_outermost;
	std::chrono::steady_clock::time_point m_start;
};

//-----------------------------------------------------------------------------------
PathNode::PathNode() :
	m_nextOpti(nullptr),
	m_next(nullptr),
	m_prev(nullptr),
	m_nextOptiDist2D(0),
	m_canOptimize(false),
	m_id(-1)
{
	m_nextOptiDirNorm2D.x = 0;
	m_nextOptiDirNorm2D.y = 0;
	m_pos.zero();
	m_layer = LAYER_INVALID;
}

//-----------------------------------------------------------------------------------
PathNode::~PathNode()
{
}

//-----------------------------------------------------------------------------------
void PathNode::setNextOptimized(PathNode *node)
{
	m_nextOpti = node;
	if (node)
	{
		m_nextOptiDirNorm2D.x = node->getPosition()->x - getPosition()->x;
		m_nextOptiDirNorm2D.y = node->getPosition()->y - getPosition()->y;
		m_nextOptiDist2D = m_nextOptiDirNorm2D.length();
		if (m_nextOptiDist2D == 0.0f)
		{
			//DEBUG_LOG(("Warning - Path Seg length == 0, adjusting. john a."));
			m_nextOptiDist2D = 0.01f;
		}
		m_nextOptiDirNorm2D.x /= m_nextOptiDist2D;
		m_nextOptiDirNorm2D.y /= m_nextOptiDist2D;
	}
	else
	{
		m_nextOptiDist2D = 0;
	}
}

//-----------------------------------------------------------------------------------
/// given a list, prepend this node, return new list
PathNode *PathNode::prependToList( PathNode *list )
{
	m_next = list;
	if (list)
		list->m_prev = this;
	m_prev = nullptr;
	return this;
}


//-----------------------------------------------------------------------------------
/// given a node, append new node to this.
void PathNode::append( PathNode *newNode )
{
	newNode->m_next = this->m_next;
	newNode->m_prev = this;
	if (newNode->m_next) {
		newNode->m_next->m_prev = newNode;
	}
	this->m_next = newNode;

}

//-----------------------------------------------------------------------------------
/**
 * Compute direction vector to next node
 */
const Coord3D *PathNode::computeDirectionVector()
{
	static Coord3D dir;

	if (m_next == nullptr)
	{
		if (m_prev == nullptr)
		{
			// only one node on whole path - no direction
			dir.x = 0.0f;
			dir.y = 0.0f;
			dir.z = 0.0f;
		}
		else
		{
			// tail node - continue prior direction
			return m_prev->computeDirectionVector();
		}
	}
	else
	{
		dir.x = m_next->m_pos.x - m_pos.x;
		dir.y = m_next->m_pos.y - m_pos.y;
		dir.z = m_next->m_pos.z - m_pos.z;
	}

	return &dir;
}


//-----------------------------------------------------------------------------------
Path::Path():
m_path(nullptr),
m_pathTail(nullptr),
m_isOptimized(FALSE),
m_blockedByAlly(FALSE),
m_cpopRecentStart(nullptr),
m_cpopCountdown(MAX_CPOP),
m_cpopValid(FALSE)
{
	m_cpopIn.zero();
	m_cpopOut.distAlongPath=0;
	m_cpopOut.layer = LAYER_GROUND;
	m_cpopOut.posOnPath.zero();
}

Path::~Path()
{
	PathNode *node, *nextNode;

	// delete all of the path nodes
	for( node = m_path; node; node = nextNode )
	{
		nextNode = node->getNext();
		deleteInstance(node);
	}
}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void Path::crc( Xfer *xfer )
{
}

// ------------------------------------------------------------------------------------------------
/** Xfer Method */
// ------------------------------------------------------------------------------------------------
void Path::xfer( Xfer *xfer )
{
  // version
  XferVersion currentVersion = 1;
  XferVersion version = currentVersion;
  xfer->xferVersion( &version, currentVersion );

	PathNode *node = m_path;
	Int count = 0;
	while (node) {
		count++;
		node = node->getNext();
	}
	xfer->xferInt(&count);

	if (xfer->getXferMode() == XFER_SAVE)	{
		node = m_pathTail;  // Write them out backwards.
		while (node) {
			node->m_id = count;
			xfer->xferInt(&count);
			Coord3D pos = *node->getPosition();
			xfer->xferCoord3D(&pos);
			PathfindLayerEnum layer = node->getLayer();
			xfer->xferUser(&layer, sizeof(layer));
			Bool canOpt = node->getCanOptimize();
			xfer->xferBool(&canOpt);
			Int id = -1;
			if (node->getNextOptimized()) {
				id = node->getNextOptimized()->m_id;
			}
			xfer->xferInt(&id);
			count--;
			node = node->getPrevious();
		}
		DEBUG_ASSERTCRASH(count==0, ("Wrong data count"));
	} else {
		m_cpopValid = FALSE;
		while (count) {
			Int nodeId;
			xfer->xferInt(&nodeId);
			DEBUG_ASSERTCRASH(nodeId==count, ("Bad data"));
			Coord3D pos;
			xfer->xferCoord3D(&pos);
			PathfindLayerEnum layer;
			xfer->xferUser(&layer, sizeof(layer));
			Bool canOpt;
			xfer->xferBool(&canOpt);
			Int optID = -1;
			xfer->xferInt(&optID);
			PathNode *node = newInstance(PathNode);
			node->m_id = nodeId;
			node->setPosition(&pos);
			node->setLayer(layer);
			node->setCanOptimize(canOpt);
			PathNode *optNode = nullptr;
			if (optID > 0) {
				optNode = m_path;
				while (optNode && optNode->m_id != optID) {
					optNode = optNode->getNext();
				}
				DEBUG_ASSERTCRASH (optNode && optNode->m_id == optID, ("Could not find optimized link."));
			}
			m_path = node->prependToList(m_path);
			if (m_pathTail == nullptr)
				m_pathTail = node;
			if (optNode) {
				node->setNextOptimized(optNode);
			}
			count--;
		}
	}

	xfer->xferBool(&m_isOptimized);
	Int obsolete1 = 0;
	xfer->xferInt(&obsolete1);
	UnsignedInt obsolete2;
	xfer->xferUnsignedInt(&obsolete2);
	xfer->xferBool(&m_blockedByAlly);


#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI == AI_DEBUG_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		Coord3D pos;
		addIcon(nullptr, 0, 0, color); // erase feedback.
		for( PathNode *node = getFirstNode(); node; node = node->getNext() )
		{

			// create objects to show path - they decay

			pos = *node->getPosition();
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.25f, 200, color);
		}

		// show optimized path
		for( node = getFirstNode(); node; node = node->getNextOptimized() )
		{
			pos = *node->getPosition();
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.8f, 200, color);
		}
		TheAI->pathfinder()->setDebugPath(this);
	}
#endif
}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void Path::loadPostProcess()
{
}

/**
 * Create a new node at the head of the path
 */
void Path::prependNode( const Coord3D *pos, PathfindLayerEnum layer )
{
	PathNode *node = newInstance(PathNode);

	node->setPosition( pos );
	node->setLayer(layer);

	m_path = node->prependToList( m_path );

	if (m_pathTail == nullptr)
		m_pathTail = node;

	m_isOptimized = false;
}

/**
 * Create a new node at the tail of the path
 */
void Path::appendNode( const Coord3D *pos, PathfindLayerEnum layer )
{
	if (m_isOptimized && m_pathTail)
	{
		/* Check for duplicates. */
		if (pos->x == m_pathTail->getPosition()->x && pos->y == m_pathTail->getPosition()->y) {
			DEBUG_LOG(("Warning - Path Seg length == 0, ignoring. john a."));
			return;
		}
	}
	PathNode *node = newInstance(PathNode);

	node->setPosition( pos );
	node->setLayer(layer);

	if (!m_path)
	{
		m_path = node;
		m_pathTail = node;

		return;
	}

	m_pathTail->append(node);

	if (m_isOptimized)
	{
		m_pathTail->setNextOptimized(node);
	}

	m_pathTail = node;
}
/**
 * Create a new node at the tail of the path
 */
void Path::updateLastNode( const Coord3D *pos )
{
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(pos);
	if (m_pathTail) {
		m_pathTail->setPosition(pos);
		m_pathTail->setLayer(layer);
	}
	if (m_isOptimized && m_pathTail)
	{
		PathNode *node = m_path;
		while(node && node->getNextOptimized() != m_pathTail) {
			node = node->getNextOptimized();
		}
		if (node && node->getNextOptimized() == m_pathTail) {
			node->setNextOptimized(m_pathTail);
		}
	}
}

/**
 * Optimize the path by checking line of sight
 */
void Path::optimize( const Object *obj, LocomotorSurfaceTypeMask acceptableSurfaces, Bool blocked )
{
	PathNode *node, *anchor;

	// start with first node in the path
	anchor = getFirstNode();

	Bool firstNode = true;
	PathfindLayerEnum firstLayer = anchor->getLayer();

	// backwards.

	//
	// For each node in the path, check LOS from last node in path, working forward.
	// When a clear LOS is found, keep the resulting straight line segment.
	//
	while( anchor != getLastNode() )
	{
		// find the farthest node in the path that has a clear line-of-sight to this anchor
		Bool optimizedSegment = false;
		PathfindLayerEnum layer = anchor->getLayer();
		PathfindLayerEnum curLayer = anchor->getLayer();
		Int count = 0;
		const Int ALLOWED_STEPS = 3; // we can optimize 3 steps to or from a bridge.  Otherwise, we need to insert a point. jba.
		for (node = anchor->getNext(); node->getNext(); node=node->getNext()) {
			count++;
			if (curLayer==LAYER_GROUND) {
				if (node->getLayer() != curLayer) {
					layer = node->getLayer();
					curLayer = layer;
					if (count > ALLOWED_STEPS) break;
				}
			}	else {
				if (node->getNext()->getLayer() != curLayer) {
					if (count > ALLOWED_STEPS) break;
				}
			}
			curLayer = node->getLayer();
			if (node->getCanOptimize()==false) {
				break;
			}
		}
		if (firstNode) {
			layer = firstLayer;
			firstNode = false;
		}
		//PathfindLayerEnum curLayer = LAYER_GROUND;
		for( ; node != anchor; node = node->getPrevious() )
		{
			Bool isPassable = false;
			//CRCDEBUG_LOG(("Path::optimize() calling isLinePassable()"));
			if (TheAI->pathfinder()->isLinePassable( obj, acceptableSurfaces, layer, *anchor->getPosition(),
				*node->getPosition(), blocked, false))
			{
				isPassable = true;
			}
			PathfindCell* cell = TheAI->pathfinder()->getCell( layer, node->getPosition());
			if (cell && cell->getType()==PathfindCell::CELL_CLIFF && !cell->getPinched()) {
				isPassable = true;
			}
			// Horizontal, diagonal, and vertical steps are passable.
			if (!isPassable) {
				Int dx = node->getPosition()->x - anchor->getPosition()->x;
				Int dy = node->getPosition()->y - anchor->getPosition()->y;
				Bool mightBePassable = false;
				if (IABS(dx)==PATHFIND_CELL_SIZE && IABS(dy)==PATHFIND_CELL_SIZE) {
					isPassable = true;
				}
				PathNode *tmpNode;
				if (dx==0) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode && tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						if (dx!=0) mightBePassable = false;
					}
				}
				if (dy==0) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode && tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=0) mightBePassable = false;
					}
				}
				if (dx == dy) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode &&   tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=dx) mightBePassable = false;
					}
				}
				if (dx == -dy) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode &&   tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=-dx) mightBePassable = false;
					}
				}
				if (mightBePassable) {
					isPassable = true;
				}
			}
			if (isPassable)
			{
				// anchor can directly see this node, make it next in the optimized path
				anchor->setNextOptimized( node );
				anchor = node;
				optimizedSegment = true;
				break;
			}
		}

		if (optimizedSegment == false)
		{
			// for some reason, there is no clear LOS between the anchor node and the very next node
			anchor->setNextOptimized( anchor->getNext() );
			anchor = anchor->getNext();
		}
	}

	// the path has been optimized
	m_isOptimized = true;
}

/**
 * Optimize the path by checking line of sight
 */
void Path::optimizeGroundPath( Bool crusher, Int pathDiameter )
{
	PathNode *node, *anchor;

	// start with first node in the path
	anchor = getFirstNode();

	//
	// For each node in the path, check LOS from last node in path, working forward.
	// When a clear LOS is found, keep the resulting straight line segment.
	//
	while( anchor != getLastNode() )
	{
		// find the farthest node in the path that has a clear line-of-sight to this anchor
		Bool optimizedSegment = false;
		PathfindLayerEnum layer = anchor->getLayer();
		PathfindLayerEnum curLayer = anchor->getLayer();
		Int count = 0;
		const Int ALLOWED_STEPS = 3; // we can optimize 3 steps to or from a bridge.  Otherwise, we need to insert a point. jba.
		for (node = anchor->getNext(); node->getNext(); node=node->getNext()) {
			count++;
			if (curLayer==LAYER_GROUND) {
				if (node->getLayer() != curLayer) {
					layer = node->getLayer();
					curLayer = layer;
					if (count > ALLOWED_STEPS) break;
				}
			}	else {
				if (node->getNext()->getLayer() != curLayer) {
					if (count > ALLOWED_STEPS) break;
				}
			}
			curLayer = node->getLayer();
		}

		// find the farthest node in the path that has a clear line-of-sight to this anchor
		for( ; node != anchor; node = node->getPrevious() )
		{
			Bool isPassable = false;
			//CRCDEBUG_LOG(("Path::optimize() calling isLinePassable()"));
			if (TheAI->pathfinder()->isGroundPathPassable( crusher, *anchor->getPosition(), layer,
				*node->getPosition(), pathDiameter))
			{
				isPassable = true;
			}
			// Horizontal, diagonal, and vertical steps are passable.
			if (!isPassable) {
				Int dx = node->getPosition()->x - anchor->getPosition()->x;
				Int dy = node->getPosition()->y - anchor->getPosition()->y;
				Bool mightBePassable = false;
				PathNode *tmpNode;
				if (dx==0) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode && tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						if (dx!=0) mightBePassable = false;
					}
				}
				if (dy==0) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode && tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=0) mightBePassable = false;
					}
				}
				if (dx == dy) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode &&   tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=dx) mightBePassable = false;
					}
				}
				if (dx == -dy) {
					mightBePassable = true;
					for (tmpNode = node->getPrevious(); tmpNode &&   tmpNode != anchor; tmpNode = tmpNode->getPrevious()) {
						dx = tmpNode->getNext()->getPosition()->x - tmpNode->getPosition()->x;
						dy = tmpNode->getNext()->getPosition()->y - tmpNode->getPosition()->y;
						if (dy!=-dx) mightBePassable = false;
					}
				}
				if (mightBePassable) {
					isPassable = true;
				}
			}
			if (isPassable)
			{
				// anchor can directly see this node, make it next in the optimized path
				anchor->setNextOptimized( node );
				anchor = node;
				optimizedSegment = true;
				break;
			}
		}

		if (optimizedSegment == false)
		{
			// for some reason, there is no clear LOS between the anchor node and the very next node
			anchor->setNextOptimized( anchor->getNext() );
			anchor = anchor->getNext();
		}
	}

	// Remove jig/jogs :) jba.
	for (anchor=getFirstNode(); anchor!=nullptr; anchor=anchor->getNextOptimized()) {
		node = anchor->getNextOptimized();
		if (node && node->getNextOptimized()) {
			Real dx = node->getPosition()->x - anchor->getPosition()->x;
			Real dy = node->getPosition()->y - anchor->getPosition()->y;
			// If the x & y offsets are less than 2 pathfind cells, kill it.
			if (dx*dx+dy*dy < sqr(PATHFIND_CELL_SIZE_F)*3.9f) {
				anchor->setNextOptimized(node->getNextOptimized());
			}
		}
	}

	// the path has been optimized
	m_isOptimized = true;
}

inline Bool isReallyClose(const Coord3D& a, const Coord3D& b)
{
	const Real CLOSE_ENOUGH = 0.1f;
	return
		fabs(a.x-b.x) <= CLOSE_ENOUGH &&
		fabs(a.y-b.y) <= CLOSE_ENOUGH &&
		fabs(a.z-b.z) <= CLOSE_ENOUGH;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX: (blocked-ring check, AIUpdateInterface::gxFacesItsPath): computePointOnPath's answer for
// pos WITHOUT advancing its cache - TRUE only when computePointOnPath would reuse the cache (the same test it makes), so the
// point equals a fresh call's; an exhausted countdown or a moved unit reads as unknown.
Bool Path::gxPeekReusablePointOnPath( const Coord3D& pos, Coord3D& out ) const
{
	if (m_path == nullptr || !(m_cpopValid && m_cpopCountdown>0 && isReallyClose(pos, m_cpopIn)))
		return FALSE;
	out = m_cpopOut.posOnPath;
	return TRUE;
}

/**
 * Given a location, return the closest position on the path.
 * If 'allowBacktrack' is true, the entire path is considered.
 * If it is false, the point computed cannot be prior to previously returned non-backtracking points on this path.
 * Because the path "knows" the direction of travel, it will "lead" the given position a bit
 * to ensure the path is followed in the intended direction.
 *
 * Note: The path cleanup does not take into account rolling terrain, so we can end up with
 * these situations:
 *
 *           B
 *        ######
 *      ##########
 *  A-##----------##---C
 * #######################
 *
 *
 * When an agent gets to B, he seems far off of the path, but it really not.
 * There are similar problems with valleys.
 *
 * Since agents track the closest path, if a high hill gets close to the underside of
 * a bridge, an agent may 'jump' to the higher path.  This must be avoided in maps.
 *
 * return along-path distance to the end will be returned as function result
 */
void Path::computePointOnPath(
	const Object* obj,
	const LocomotorSet& locomotorSet,
	const Coord3D& pos,
	ClosestPointOnPathInfo& out
)
{
	CRCDEBUG_LOG(("Path::computePointOnPath() for %s", DebugDescribeObject(obj).str()));

	out.layer = LAYER_GROUND;
	out.posOnPath.zero();
	out.distAlongPath = 0;

	if (m_path == nullptr)
	{
		m_cpopValid = false;
		return;
	}
	out.layer = m_path->getLayer();

	if (m_cpopValid && m_cpopCountdown>0 && isReallyClose(pos, m_cpopIn))
	{
		out = m_cpopOut;
		m_cpopCountdown--;
		CRCDEBUG_LOG(("Path::computePointOnPath() end because we're really close"));
		return;
	}
	m_cpopCountdown = MAX_CPOP;

	// default pathPos to end of the path
	out.posOnPath = *getLastNode()->getPosition();

	const PathNode* closeNode = nullptr;
	Coord2D toPos;
	Real closeDistSqr = 99999999.9f;
	Real totalPathLength = 0.0f;
	Real lengthAlongPathToPos = 0.0f;

	//
	// Find the closest segment of the path
	//
	const PathNode* prevNode = m_path;
	Coord2D segmentDirNorm;
	Real segmentLength;

	// note that the seg dir and len returned by this is the dist & vec from 'prevNode' to 'node'
	for ( const PathNode* node = prevNode->getNextOptimized(&segmentDirNorm, &segmentLength);
				node != nullptr;
				node = node->getNextOptimized(&segmentDirNorm, &segmentLength) )
	{
		const Coord3D* prevNodePos = prevNode->getPosition();
		const Coord3D* nodePos = node->getPosition();

		// compute vector from start of segment to pos
		toPos.x = pos.x - prevNodePos->x;
		toPos.y = pos.y - prevNodePos->y;

		// compute distance projection of 'toPos' onto segment
		Real alongPathDist = segmentDirNorm.x * toPos.x + segmentDirNorm.y * toPos.y;

		Coord3D pointOnPath;
		if (alongPathDist < 0.0f)
		{
			// projected point is before start of segment, use starting point
			alongPathDist = 0.0f;
			pointOnPath = *prevNodePos;
		}
		else if (alongPathDist > segmentLength)
		{
			// projected point is beyond end of segment, use end point
			if (node->getNextOptimized() == nullptr)
			{
				alongPathDist = segmentLength;
				pointOnPath = *nodePos;
			}
			else
			{
				// beyond the end of this segment, skip this segment
				// if bend is sharp, start of next segment will grab this point
				// if bend is gradual, the point will project into the next segment
				totalPathLength += segmentLength;
				prevNode = node;
				continue;
			}
		}
		else
		{
			// projected point is on this segment, compute it
			pointOnPath.x = prevNodePos->x + alongPathDist * segmentDirNorm.x;
			pointOnPath.y = prevNodePos->y + alongPathDist * segmentDirNorm.y;
			pointOnPath.z = 0;
		}

		// compute distance to point on path, and track the closest we've found so far
		Coord2D offset;
		offset.x = pos.x - pointOnPath.x;
		offset.y = pos.y - pointOnPath.y;

		Real offsetDistSqr = offset.x*offset.x + offset.y*offset.y;
		if (offsetDistSqr < closeDistSqr)
		{
			closeDistSqr = offsetDistSqr;
			closeNode = prevNode;
			out.posOnPath = pointOnPath;

			lengthAlongPathToPos = totalPathLength + alongPathDist;
		}

		// add this segment's length to find total path length
		/// @todo Precompute this and store in path
		totalPathLength += segmentLength;
		prevNode = node;
		DUMPCOORD3D(&pointOnPath);
	}

	//
	// Compute the goal movement position for this agent
	//
	if (closeNode && closeNode->getNextOptimized())
	{
		// note that the seg dir and len returned by this is the dist & vec from 'closeNode' to 'closeNext'
		const PathNode* closeNext = closeNode->getNextOptimized(&segmentDirNorm, &segmentLength);
		const Coord3D* nextNodePos = closeNext->getPosition();
		const Coord3D* closeNodePos = closeNode->getPosition();

		const PathNode* closePrev = closeNode->getPrevious();
		if (closePrev && closePrev->getLayer() > LAYER_GROUND)
		{
			out.layer = closeNode->getLayer();
		}
		if (closeNode->getLayer() > LAYER_GROUND)
		{
			out.layer = closeNode->getLayer();
		}

		if (closeNext->getLayer() > LAYER_GROUND)
		{
			out.layer = closeNext->getLayer();
		}

		// compute vector from start of segment to pos
		toPos.x = pos.x - closeNodePos->x;
		toPos.y = pos.y - closeNodePos->y;

		// compute distance projection of 'toPos' onto segment
		Real alongPathDist = segmentDirNorm.x * toPos.x + segmentDirNorm.y * toPos.y;

		// we know this is the closest segment, so don't allow farther back than the start node
		if (alongPathDist < 0.0f)
			alongPathDist = 0.0f;

		// compute distance of point from this path segment
		Real toDistSqr = sqr(toPos.x) + sqr(toPos.y);
		Real offsetDistSq = toDistSqr - sqr(alongPathDist);
		Real offsetDist = (offsetDistSq <= 0.0) ? 0.0 : sqrt(offsetDistSq);

		// If we are basically on the path, return the next path node as the movement goal.
		// However, the farther off the path we get, the movement goal becomes closer to our
		// projected position on the path.  If we are very far off the path, we will move
		// directly towards the nearest point on the path, and not the next path node.
		const Real maxPathError = 3.0f * PATHFIND_CELL_SIZE_F;
		const Real maxPathErrorInv = 1.0 / maxPathError;
		Real k = offsetDist * maxPathErrorInv;
		if (k > 1.0f)
			k = 1.0f;

		Bool gotPos = false;
		CRCDEBUG_LOG(("Path::computePointOnPath() calling isLinePassable() 1"));
		if (TheAI->pathfinder()->isLinePassable( obj, locomotorSet.getValidSurfaces(), out.layer, pos, *nextNodePos,
			false, true ))
		{
			out.posOnPath = *nextNodePos;
			gotPos = true;

			Bool tryAhead = alongPathDist > segmentLength * 0.5;
			if (closeNext->getCanOptimize() == false)
			{
				tryAhead = false; // don't go past no-opt nodes.
			}
			if (closeNode->getLayer() != closeNext->getLayer())
			{
				tryAhead = false; // don't go past layers.
			}
			if (obj->getLayer()!=LAYER_GROUND) {
				tryAhead = false;
			}
			Bool veryClose = false;
			if (segmentLength-alongPathDist<1.0f) {
				tryAhead = true;
				veryClose = true;
			}
			if (tryAhead)
			{
				// try next segment middle.
				const PathNode *next = closeNext->getNextOptimized();
				if (next)
				{
					Coord3D tryPos;
					tryPos.x = (nextNodePos->x + next->getPosition()->x) * 0.5;
					tryPos.y = (nextNodePos->y + next->getPosition()->y) * 0.5;
					tryPos.z = nextNodePos->z;
					CRCDEBUG_LOG(("Path::computePointOnPath() calling isLinePassable() 2"));
					if (veryClose || TheAI->pathfinder()->isLinePassable( obj, locomotorSet.getValidSurfaces(), closeNext->getLayer(), pos, tryPos, false, true ))
					{
						gotPos = true;
						out.posOnPath = tryPos;
					}
				}
			}
		}
		else if (k > 0.5f)
		{
			Real tryDist = alongPathDist + (0.5) * (segmentLength - alongPathDist);

			// projected point is on this segment, compute it
			out.posOnPath.x = closeNodePos->x + tryDist * segmentDirNorm.x;
			out.posOnPath.y = closeNodePos->y + tryDist * segmentDirNorm.y;
			out.posOnPath.z = closeNodePos->z;

			CRCDEBUG_LOG(("Path::computePointOnPath() calling isLinePassable() 3"));
			if (TheAI->pathfinder()->isLinePassable( obj, locomotorSet.getValidSurfaces(), out.layer, pos, out.posOnPath, false, true ))
			{
				k = 0.5f;
				gotPos = true;
			}
		}

		// if we are on the path (k == 0), then alongPathDist == segmentLength
		// if we are way off the path (k == 1), then alongPathDist is unchanged, and it projection of actual pos
		alongPathDist += (1.0f - k) * (segmentLength - alongPathDist);

		if (!gotPos)
		{
			if (alongPathDist > segmentLength)
			{
				alongPathDist = segmentLength;
				out.posOnPath = *nextNodePos;
			}
			else
			{
				// projected point is on this segment, compute it
				out.posOnPath.x = closeNodePos->x + alongPathDist * segmentDirNorm.x;
				out.posOnPath.y = closeNodePos->y + alongPathDist * segmentDirNorm.y;
				out.posOnPath.z = closeNodePos->z;
				Real dx = fabs(pos.x - out.posOnPath.x);
				Real dy = fabs(pos.y - out.posOnPath.y);
				if (dx<1 && dy<1 && closeNode->getNextOptimized() && closeNode->getNextOptimized()->getNextOptimized()) {
					out.posOnPath = *closeNode->getNextOptimized()->getNextOptimized()->getPosition();
				}
			}
		}
	}

	TheAI->pathfinder()->setDebugPathPosition( &out.posOnPath );

	out.distAlongPath = totalPathLength - lengthAlongPathToPos;

	Coord3D delta;
	delta.x = out.posOnPath.x - pos.x;
	delta.y = out.posOnPath.y - pos.y;
	delta.z = 0;
	Real lenDelta = delta.length();
	if (lenDelta > out.distAlongPath && out.distAlongPath > PATHFIND_CLOSE_ENOUGH)
	{
		out.distAlongPath = lenDelta;
	}

	m_cpopIn = pos;
	m_cpopOut = out;
	m_cpopValid = true;
	CRCDEBUG_LOG(("Path::computePointOnPath() end"));

}


/**
	Given a position, computes the distance to the goal.  Returns 0 if we are past the goal.
	Returns the goal position in goalPos.  This is intended for use with flying paths, that go
	directly to the goal and don't consider obstacles.  jba.
 */
Real Path::computeFlightDistToGoal( const Coord3D *pos, Coord3D& goalPos )
{
	if (m_path == nullptr)
	{
		goalPos.x = 0.0f;
		goalPos.y = 0.0f;
		goalPos.z = 0.0f;
		return 0.0f;
	}
	const PathNode *curNode = getFirstNode();
	if (m_cpopRecentStart) {
		curNode = m_cpopRecentStart;
	} else {
		m_cpopRecentStart = curNode;
	}
	const PathNode *nextNode = curNode->getNextOptimized();
	goalPos = *curNode->getPosition();
	Real distance = 0;
	Bool useNext = true;
	while (nextNode) {

		if (useNext) {
			goalPos = *nextNode->getPosition();
		}

		Coord3D startPos = *curNode->getPosition();
		Coord3D endPos = *nextNode->getPosition();

		Coord2D posToGoalVector;
		// posToGoalVector is pos to goalPos vector.
		posToGoalVector.x = endPos.x - pos->x;
		posToGoalVector.y = endPos.y - pos->y;

		// pathVector is the startPos to goal pos vector.
		Coord2D pathVector;
		pathVector.x = endPos.x - startPos.x;
		pathVector.y = endPos.y - startPos.y;

		// Normalize pathVector
		pathVector.normalize();

		// Dot product is the posToGoal vector projected onto the path vector.
		Real dotProduct = posToGoalVector.x*pathVector.x	+ posToGoalVector.y*pathVector.y;
		if (dotProduct>=0) {
			distance += dotProduct;
			useNext = false;
		}	else if (useNext) {
			m_cpopRecentStart = nextNode;
		}
		curNode = nextNode;
		nextNode = curNode->getNextOptimized();
	}
	return distance;

}
//-----------------------------------------------------------------------------------

PathfindCellInfo *PathfindCellInfo::s_infoArray = nullptr;
PathfindCellInfo *PathfindCellInfo::s_firstFree = nullptr;

#if RETAIL_COMPATIBLE_PATHFINDING
// TheSuperHackers @info This variable is here so the code will run down the retail compatible path till a failure mode is hit
// The pathfinding will then switch over to the corrected pathfinding code for SH clients
Bool s_useFixedPathfinding = false;
Bool s_forceCleanCells = false;

void PathfindCellInfo::forceCleanPathFindCellInfos()
{
	for (Int i = 0; i < CELL_INFOS_TO_ALLOCATE - 1; i++) {
		s_infoArray[i].m_nextOpen = nullptr;
		s_infoArray[i].m_prevOpen = nullptr;
		s_infoArray[i].m_open = FALSE;
		s_infoArray[i].m_closed = FALSE;
	}
}

void Pathfinder::forceCleanCells()
{
	UnicodeString pathfinderFailoverMessage = TheGameText->FETCH_OR_SUBSTITUTE("GUI:PathfindingCrashPrevented", L"A pathfinding crash was prevented, now switching to the crash fixed pathfinding.");
	TheInGameUI->message(pathfinderFailoverMessage);

	TheAudio->addAudioEvent(&TheAudio->getMiscAudio()->m_allCheerSound);

	PathfindCellInfo::forceCleanPathFindCellInfos();
	m_openList.reset();
	m_closedList.reset();

	for (int j = 0; j <= m_extent.hi.y; ++j) {
		for (int i = 0; i <= m_extent.hi.x; ++i) {
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
			// TheSuperHackers @bugfix Mauller/DrGoldFish 20/01/2026 when pathfinding resources cannot be allocated to a pathfindCell,
			// The function to remove an obstacle returns early and PathfindCells remain flagged as obstacles.
			// We need to make sure to reset pathfindCells with a set m_obstacleID and no m_info.
			// The use of PathfindCellInfo data for obstacle handling also exausted them resulting in pathfinding lockups.
			if (m_map[i][j].isObstructionInvalid()) {
				m_map[i][j].clearObstruction();
			}
#endif
			if (m_map[i][j].hasInfo()) {
				m_map[i][j].releaseInfo();
			}
		}
	}
}
#endif

// GeneralsX: The two gauges the findClosestPath bound is built on. Both
// are maintained here because getACellInfo/releaseACellInfo are the only places the free list is
// popped or pushed - allocateInfo is not, since setTypeAsObstacle reaches getACellInfo directly -
// so they see the line-prepass allocations that no cellCount can.
//
// s_pathfindCellInfoTakes counts ATTEMPTS, not successes, and is read only as a DELTA over one
// search (MAX_CLOSEST_PATH_INFO_COUNT). Counting successes would have stopped counting the moment
// the pool ran dry, which is the one moment the bound has to keep advancing. Unsigned, so the
// subtraction stays correct across the 2^32 wrap; never read as an absolute value.
//
// s_pathfindCellInfoFree is the live free-list count, read as an absolute value against
// PATHFIND_SEARCH_FREE_RESERVE. It is a function of the whole pool's history rather than of one
// search, so it has to be identical on every peer - it is, for the same reason
// m_cumulativeCellsAllocated can already be xfer'd and CRC'd (see Pathfinder::xfer): every caller
// of these two functions is GameLogic, and if pool traffic diverged the existing per-frame CRC
// would already be diverging. forceCleanPathFindCellInfos cannot drift it - that function relinks
// open/closed pointers and never touches m_isFree or s_firstFree.
static UnsignedInt s_pathfindCellInfoTakes = 0;
static UnsignedInt s_pathfindCellInfoFree  = 0;

// GeneralsX: The growth predicate, in one place, because it states one
// property of one pool: a search may keep expanding only while it is still inside its own share
// AND the free list is still above the floor every other allocation site lives on. findClosestPath
// asks it before it GROWS; internalFindPath asks only the floor half, pathfindPoolAboveFloor (see
// PATHFIND_SEARCH_FREE_RESERVE). Neither may carry a private copy of the floor, because two copies
// drift and the pool has only one floor.
//
// takesAtSearchStart is read as a DELTA (see s_pathfindCellInfoTakes above) so the subtraction is
// correct across the 2^32 wrap and cannot be shifted by allocations made outside this search.
// The free count is read as an ABSOLUTE against the reserve, because a ceiling on what a search
// takes says nothing about what the pool has left.
static inline Bool pathfindSearchMayGrow(UnsignedInt takesAtSearchStart, UnsignedInt maxInfos)
{
	return (UnsignedInt)(s_pathfindCellInfoTakes - takesAtSearchStart) < maxInfos &&
			s_pathfindCellInfoFree > PATHFIND_SEARCH_FREE_RESERVE;
}

// GeneralsX: The floor half alone, for internalFindPath - see the paragraph
// above PATHFIND_SEARCH_FREE_RESERVE for why that search has no per-search share.
static inline Bool pathfindPoolAboveFloor()
{
	return s_pathfindCellInfoFree > PATHFIND_SEARCH_FREE_RESERVE;
}

/**
 * Allocates a pool of pathfind cell infos.
 */
void PathfindCellInfo::allocateCellInfos()
{
	releaseCellInfos();
	s_infoArray = MSGNEW("PathfindCellInfo") PathfindCellInfo[CELL_INFOS_TO_ALLOCATE];	// pool[]ify
	s_infoArray[CELL_INFOS_TO_ALLOCATE-1].m_pathParent = nullptr;
	s_infoArray[CELL_INFOS_TO_ALLOCATE-1].m_isFree = true;
	s_firstFree = s_infoArray;
	for (Int i=0; i<CELL_INFOS_TO_ALLOCATE-1; i++) {
		s_infoArray[i].m_pathParent = &s_infoArray[i+1];
		s_infoArray[i].m_isFree = true;
	}
	s_pathfindCellInfoFree = CELL_INFOS_TO_ALLOCATE;	// GeneralsX, see PATHFIND_SEARCH_FREE_RESERVE
}

/**
 * Releases a pool of pathfind cell infos.
 */
void PathfindCellInfo::releaseCellInfos()
{
	if (s_infoArray==nullptr) {
		return; // haven't allocated any yet.
	}
	Int count=0;
	while (s_firstFree) {
		count++;
		DEBUG_ASSERTCRASH(s_firstFree->m_isFree, ("Should be freed."));
		s_firstFree = s_firstFree->m_pathParent;
	}
	DEBUG_ASSERTCRASH(count==CELL_INFOS_TO_ALLOCATE, ("Error - Allocated cellinfos."));
	delete[] s_infoArray;
	s_infoArray = nullptr;
	s_firstFree = nullptr;
	s_pathfindCellInfoFree = 0;	// GeneralsX, see PATHFIND_SEARCH_FREE_RESERVE
}

/**
 * Gets a pathfindcellinfo.
 */
PathfindCellInfo *PathfindCellInfo::getACellInfo(PathfindCell *cell,const ICoord2D &pos)
{
	PathfindCellInfo *info = s_firstFree;
	++s_pathfindCellInfoTakes;	// GeneralsX: ATTEMPTS, see MAX_CLOSEST_PATH_INFO_COUNT
	if (s_firstFree) {
		DEBUG_ASSERTCRASH(s_firstFree->m_isFree, ("Should be freed."));
		s_firstFree = s_firstFree->m_pathParent;
		if (s_pathfindCellInfoFree) --s_pathfindCellInfoFree;	// GeneralsX, see the reserve
		info->m_isFree = false;  // Just allocated it.
		info->m_cell = cell;
		info->m_pos = pos;

		info->m_nextOpen = nullptr;
		info->m_prevOpen = nullptr;
		info->m_pathParent = nullptr;
		info->m_costSoFar = 0;
		info->m_totalCost = 0;
		info->m_open = 0;
		info->m_closed = 0;
		info->m_obstacleID = INVALID_ID;
		info->m_goalUnitID = INVALID_ID;
		info->m_posUnitID = INVALID_ID;
		info->m_goalAircraftID = INVALID_ID;
		info->m_obstacleIsFence = false;
		info->m_obstacleIsTransparent = false;
		info->m_blockedByAlly = false;
	}
	return info;
}

/**
 * Returns a pathfindcellinfo.
 */
void PathfindCellInfo::releaseACellInfo(PathfindCellInfo *theInfo)
{
	DEBUG_ASSERTCRASH(!theInfo->m_isFree, ("Shouldn't be free."));
	//@ todo -fix this assert on usa04.  jba.
	//DEBUG_ASSERTCRASH(theInfo->m_obstacleID==0, ("Shouldn't be obstacle."));
	theInfo->m_pathParent = s_firstFree;
	s_firstFree = theInfo;
	s_firstFree->m_isFree = true;
	++s_pathfindCellInfoFree;	// GeneralsX, see PATHFIND_SEARCH_FREE_RESERVE
}

// ============================================================================================
// GeneralsX: The open list's cost index.
//
// WHAT IT REPLACES. forwardInsertionSortRetailCompatible walks from the head until it finds a
// cell whose cost exceeds the new one. Measured over one 12000-frame 8-player match: 34,750,199
// insertions, 2,018,873,617 steps, and after the checkForMovement memo landed it was the single
// largest leaf in the engine at 1677 of 4419 in-process samples. Two cheaper WALKS were measured
// and both are worse (see the previous commit); what is left is not walking.
//
// WHY THE ANSWER IS THE SAME. The retail walk stops at the first cell whose cost is greater than
// the new cell's, so it inserts after every cell whose cost is <= its own - last among equals.
// On a list sorted ascending that position is exactly "immediately after the last cell of the
// greatest present cost that is <= mine", which is what this looks up directly. Two conditions
// have to hold for that sentence to be true, and both are enforced rather than assumed:
//
//   the list is sorted        - it is, by induction, as long as every insertion goes to that
//                               same position; the induction is broken only by the step cap
//                               below, and the index refuses to be used once that happens.
//   the cap did not truncate  - the retail walk gives up after PATHFIND_OPEN_WALK_CAP steps
//                               and inserts there instead, which can put a cell BEFORE cheaper
//                               ones. That cannot happen while the list is no longer than the
//                               cap, since the walk cannot take more steps than there are cells.
//                               Measured: the cap truncated 0 of 34,750,199 insertions, and the
//                               longest open list ever seen was 654 against a cap of 2500 (5000 since 23/09/2026). The
//                               guard is on the length, not on that measurement.
//
// WHAT IT GIVES UP. The retail walk tests hasInfo() on every cell it passes, and fails over to
// the fixed pathfinder if it finds one that lost its PathfindCellInfo. Not walking means not
// testing, so that particular detection no longer fires here. The other three still do
// (releaseOpenList, releaseClosedList, putOnClosedList), the condition was never observed - 0
// occurrences in the same 34,750,199 insertions - and the index falls back to the full retail
// walk, detection included, whenever anything about the list is not provably in hand.
//
// THE CHECK. Under GX_PATHTRACE every indexed insertion ALSO runs the retail walk and compares
// the two landing points; s_gxIdxBad counts disagreements and is never reset, so one anywhere in
// a match stays visible. The second, independent check is the recorded replay's [GXCRC] stream.
// ============================================================================================

enum
{
	GX_COST_BUCKETS   = 65536,				// PathfindCellInfo::m_totalCost is an UnsignedShort
	GX_COST_WORDS     = GX_COST_BUCKETS / 32,	// 2048
	GX_COST_SUM_WORDS = GX_COST_WORDS / 32,		// 64
	GX_COST_SUM2_WORDS = GX_COST_SUM_WORDS / 32	// 2
};

struct GXOpenListIndex
{
	// m_tailAtCost[c] is the last cell on the list whose cost is c, and is meaningful only where
	// the corresponding bit is set. The bitmaps are a three-level occupancy summary so that
	// "greatest present cost below c" is a handful of loads rather than a scan.
	PathfindCellInfo *m_tailAtCost[GX_COST_BUCKETS];
	UnsignedInt       m_bits[GX_COST_WORDS];
	UnsignedInt       m_sum[GX_COST_SUM_WORDS];
	UnsignedInt       m_sum2[GX_COST_SUM2_WORDS];

	// The cost a cell was FILED UNDER, which is not always the cost it currently carries:
	// examineCellsCallback assigns a new total cost to a cell that is still on the open list and
	// only then removes and re-inserts it. Reading m_totalCost at removal time would therefore
	// clear the wrong bucket and leave the old one pointing at a cell that is no longer there.
	// Indexed by the cell info's slot in PathfindCellInfo::s_infoArray.
	UnsignedShort     m_filedUnder[CELL_INFOS_TO_ALLOCATE];
};

static UnsignedInt s_gxIdxUsed = 0;	// insertions that took the indexed path
static UnsignedInt s_gxIdxWalk = 0;	// insertions that fell back to the retail walk
static UnsignedInt s_gxIdxBad  = 0;	// indexed insertions that disagreed with the walk - must be 0

static inline Int gxHighestBit(UnsignedInt v)	// v must be non-zero
{
#if defined(__GNUC__) || defined(__clang__)
	return 31 - __builtin_clz(v);
#else
	Int r = 0;
	if (v & 0xFFFF0000u) { v >>= 16; r += 16; }
	if (v & 0x0000FF00u) { v >>= 8;  r += 8;  }
	if (v & 0x000000F0u) { v >>= 4;  r += 4;  }
	if (v & 0x0000000Cu) { v >>= 2;  r += 2;  }
	if (v & 0x00000002u) {           r += 1;  }
	return r;
#endif
}

static inline Bool gxCostPresent(const GXOpenListIndex *ix, UnsignedInt c)
{
	return (ix->m_bits[c >> 5] & (1u << (c & 31))) != 0;
}

static inline void gxCostSet(GXOpenListIndex *ix, UnsignedInt c)
{
	const UnsignedInt w = c >> 5;
	ix->m_bits[w] |= (1u << (c & 31));
	ix->m_sum[w >> 5] |= (1u << (w & 31));
	ix->m_sum2[(w >> 5) >> 5] |= (1u << ((w >> 5) & 31));
}

static inline void gxCostClear(GXOpenListIndex *ix, UnsignedInt c)
{
	const UnsignedInt w = c >> 5;
	ix->m_bits[w] &= ~(1u << (c & 31));
	if (ix->m_bits[w] == 0)
	{
		const UnsignedInt sw = w >> 5;
		ix->m_sum[sw] &= ~(1u << (w & 31));
		if (ix->m_sum[sw] == 0)
			ix->m_sum2[sw >> 5] &= ~(1u << (sw & 31));
	}
}

/// Greatest cost strictly below c that is present on the list, or -1 if there is none.
static Int gxCostPrev(const GXOpenListIndex *ix, UnsignedInt c)
{
	if (c == 0)
		return -1;

	const UnsignedInt b = c - 1u;
	UnsignedInt w = b >> 5;
	const UnsignedInt bit = b & 31;
	UnsignedInt v = ix->m_bits[w] & ((bit == 31) ? 0xFFFFFFFFu : ((1u << (bit + 1)) - 1u));
	if (v)
		return (Int)(w << 5) + gxHighestBit(v);

	// no cost in this word; find the greatest earlier word that has one
	if (w == 0)
		return -1;
	UnsignedInt sw = w >> 5;
	const UnsignedInt sbit = w & 31;
	UnsignedInt sv = ix->m_sum[sw] & ((sbit == 0) ? 0u : ((1u << sbit) - 1u));
	if (sv == 0)
	{
		if (sw == 0)
			return -1;
		UnsignedInt s2w = sw >> 5;
		const UnsignedInt s2bit = sw & 31;
		UnsignedInt s2v = ix->m_sum2[s2w] & ((s2bit == 0) ? 0u : ((1u << s2bit) - 1u));
		while (s2v == 0)
		{
			if (s2w == 0)
				return -1;
			--s2w;
			s2v = ix->m_sum2[s2w];
		}
		sw = (s2w << 5) + (UnsignedInt)gxHighestBit(s2v);
		sv = ix->m_sum[sw];
	}
	w = (sw << 5) + (UnsignedInt)gxHighestBit(sv);
	return (Int)(w << 5) + gxHighestBit(ix->m_bits[w]);
}

PathfindCellList::~PathfindCellList()
{
	delete m_index;
	m_index = nullptr;
}

void PathfindCellList::allocateIndex()
{
	if (m_index != nullptr)
		return;
	// The index is an accelerator: the walks give the same order without it. So running out of memory for it must not
	// fail anything - MSGNEW throws ERROR_OUT_OF_MEMORY, which would unwind Pathfinder's constructor.
	// Without it the fixed pathfinder keeps the walks for the whole match; it never retries inside a search.
	try
	{
		m_index = MSGNEW("PathfindOpenIndex") GXOpenListIndex;
		memset(m_index, 0, sizeof(GXOpenListIndex));
	}
	catch (...)
	{
		m_index = nullptr;
	}
}

#if RETAIL_COMPATIBLE_PATHFINDING
void PathfindCellList::reset(PathfindCell* newHead)
#else
void PathfindCellList::reset()
#endif
{
#if !RETAIL_COMPATIBLE_PATHFINDING
	PathfindCell* const newHead = nullptr;
#endif
	m_head = newHead;
	m_tail = newHead;
	m_count = newHead ? 1 : 0;
	m_countValid = true;
	m_satHead = nullptr;	// GeneralsX: a seeded head was never filed as saturated; harmless if it is (a shorter run)

	if (m_index != nullptr)
	{
		memset(m_index->m_bits, 0, sizeof(m_index->m_bits));
		memset(m_index->m_sum,  0, sizeof(m_index->m_sum));
		memset(m_index->m_sum2, 0, sizeof(m_index->m_sum2));
	}
	// A head installed here never went through an insertion, so the index does not know about it
	// yet and the first insertion has to enter it. Until then the index is not usable.
	m_indexValid = true;
	m_indexNeedsSeed = (newHead != nullptr);
}

//-----------------------------------------------------------------------------------

Bool PathfindCellList::canReverseSort(PathfindCell& currentCell) const
{
	if (m_head && m_tail)
		return m_head->getTotalCostDifference(currentCell) > m_tail->getTotalCostDifference(currentCell);

	return false;
}

//-----------------------------------------------------------------------------------

/**
 * Constructor
 */
PathfindCell::PathfindCell() :m_info(nullptr)
{
	reset();
}

/**
 * Destructor
 */
PathfindCell::~PathfindCell()
{
	if (m_info) PathfindCellInfo::releaseACellInfo(m_info);
	m_info = nullptr;
	static Bool warn = true;
	if (warn) {
		warn = false;
		DEBUG_LOG( ("PathfindCell::~PathfindCell m_info Allocated."));
	}
}

/**
 * Reset the cell to default values
 */
void PathfindCell::reset()
{
	m_type = PathfindCell::CELL_CLEAR;
	m_flags = PathfindCell::NO_UNITS;
	m_zone = 0;
	m_aircraftGoal = false;
	m_pinched = false;
	if (m_info) {
		m_info->m_obstacleID = INVALID_ID;
		PathfindCellInfo::releaseACellInfo(m_info);
		m_info = nullptr;
	}
	m_obstacleID = INVALID_ID;
	m_blockedByAlly = false;
	m_obstacleIsFence = false;
	m_obstacleIsTransparent = false;

	m_connectsToLayer = LAYER_INVALID;
	m_layer = LAYER_GROUND;

}

/**
 * Reset the pathfinding values in the cell.
 */
Bool PathfindCell::startPathfind( PathfindCell *goalCell  )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	m_info->m_nextOpen = nullptr;
	m_info->m_prevOpen = nullptr;
	m_info->m_pathParent = nullptr;
	m_info->m_costSoFar = 0;		// start node, no cost to get here
	m_info->m_totalCost = 0;
	if (goalCell) {
		setTotalCost( costToGoal( goalCell ) );	// GeneralsX: the same store, counted (gxNotePathCost)
	}
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_info->m_open = TRUE;
	} else
#endif
	{
		m_info->m_open = FALSE;
	}
	m_info->m_closed = FALSE;
	return true;
}

/**
 * Set the blocked by ally flag on the pathfind cell info.
 */
inline Bool PathfindCell::isBlockedByAlly() const
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		return m_blockedByAlly;
	}

	return m_info->m_blockedByAlly;
#else
	return m_blockedByAlly;
#endif
}

inline void PathfindCell::setBlockedByAlly(Bool blocked)
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		m_blockedByAlly = (blocked != 0);
		return;
	}

	m_info->m_blockedByAlly = (blocked != 0);
#else
	m_blockedByAlly = (blocked != 0);
#endif
}

/**
 * Determine absolute total path cost difference between two cells.
 * Returns UINT_MAX if used with an uninitialised cell, so will be sorted as maximally dissimilar.
 */
inline UnsignedInt PathfindCell::getTotalCostDifference(PathfindCell& other) const
{
	if (m_info && other.m_info)
		return abs((Int)m_info->m_totalCost - (Int)other.m_info->m_totalCost);

	return UINT_MAX;
}

/**
 * Set the parent pointer.
 */
void PathfindCell::setParentCell( PathfindCell* parent  )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	m_info->m_pathParent = parent->m_info;
	Int dx = m_info->m_pos.x - parent->m_info->m_pos.x;
	Int dy = m_info->m_pos.y - parent->m_info->m_pos.y;
	if (dx<-1 || dx>1 || dy<-1 || dy>1) {
		DEBUG_CRASH(("Invalid parent index."));
	}
}

/**
 * Set the parent pointer.
 */
void PathfindCell::setParentCellHierarchical( PathfindCell* parent  )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	m_info->m_pathParent = parent->m_info;
}

/**
 * Reset the parent cell.
 */
void PathfindCell::clearParentCell(  )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	m_info->m_pathParent = nullptr;
}


/**
 * Allocates an info record for a cell.
 */
Bool PathfindCell::allocateInfo( const ICoord2D &pos )
{
	if (!m_info) {
		m_info = PathfindCellInfo::getACellInfo(this, pos);
		return (m_info != nullptr);
	}
	return true;
}

/**
 * Releases an info record for a cell.
 */
void PathfindCell::releaseInfo()
{
	// TheSuperHackers @bugfix Mauller/SkyAero 05/06/2025 Parent cell links need clearing to prevent dangling pointers on starting points that can link them to an invalid parent cell.
	// Parent cells are only cleared within Pathfinder::prependCells, so cells that do not make it onto the final path do not get their parent cell cleared.
	// Cells with a special flags also do not get their PathfindCellInfo cleared and therefore can leave a parent cell set on a starting cell.
#if RETAIL_COMPATIBLE_PATHFINDING
	if (s_useFixedPathfinding)
#endif
	{
		if (m_info) {
			m_info->m_pathParent = nullptr;
		}
	}

	if (m_type == PathfindCell::CELL_OBSTACLE || m_flags != NO_UNITS || m_aircraftGoal) {
		return;
	}

	if (!m_info) {
		return;
	}

	DEBUG_ASSERTCRASH(m_info->m_prevOpen==nullptr && m_info->m_nextOpen==nullptr, ("Shouldn't be linked."));
	DEBUG_ASSERTCRASH(m_info->m_open==0 && m_info->m_closed==0, ("Shouldn't be linked."));
	DEBUG_ASSERTCRASH(m_info->m_goalUnitID==INVALID_ID && m_info->m_posUnitID==INVALID_ID, ("Shouldn't be occupied."));
	DEBUG_ASSERTCRASH(m_info->m_goalAircraftID==INVALID_ID , ("Shouldn't be occupied by aircraft."));
	if (m_info->m_prevOpen || m_info->m_nextOpen || m_info->m_open || m_info->m_closed) {
		// Bad release.  Skip for now, better leak than crash.  jba.
		return;
	}

	PathfindCellInfo::releaseACellInfo(m_info);
	m_info = nullptr;

}

/**
 * Sets the goal unit into the info record for a cell.
 */
void PathfindCell::setGoalUnit(ObjectID unitID, const ICoord2D &pos )
{
	if (unitID==INVALID_ID) {
		// removing goal.
		if (m_info) {
			m_info->m_goalUnitID = INVALID_ID;
			if (m_info->m_posUnitID == INVALID_ID) {
				// No units here.
				DEBUG_ASSERTCRASH(m_flags==UNIT_GOAL, ("Bad flags."));
				m_flags = NO_UNITS;
				releaseInfo();
			} else{
				m_flags = UNIT_PRESENT_MOVING;
			}
		}	else {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
			allocateInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		m_info->m_goalUnitID = unitID;
		if (unitID==m_info->m_posUnitID) {
			m_flags = UNIT_PRESENT_FIXED;
		} else if (m_info->m_posUnitID==INVALID_ID) {
			m_flags = UNIT_GOAL;
		}	else {
			m_flags = UNIT_GOAL_OTHER_MOVING;
		}
	}
}


/**
 * Sets the goal aircraft into the info record for a cell.
 */
void PathfindCell::setGoalAircraft(ObjectID unitID, const ICoord2D &pos )
{
	if (unitID==INVALID_ID) {
		// removing goal.
		if (m_info) {
			m_info->m_goalAircraftID = INVALID_ID;
			m_aircraftGoal = false;
			releaseInfo();
		}	else {
			DEBUG_ASSERTCRASH(m_aircraftGoal==false, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_aircraftGoal==false, ("Bad flags."));
			allocateInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		m_info->m_goalAircraftID = unitID;
		m_aircraftGoal = true;
	}
}


/**
 * Sets the position unit into the info record for a cell.
 */
void PathfindCell::setPosUnit(ObjectID unitID, const ICoord2D &pos )
{
	if (unitID==INVALID_ID) {
		// removing position.
		if (m_info) {
			m_info->m_posUnitID = INVALID_ID;
			if (m_info->m_goalUnitID == INVALID_ID) {
				// No units here.
				DEBUG_ASSERTCRASH(m_flags==UNIT_PRESENT_MOVING, ("Bad flags."));
				m_flags = NO_UNITS;
				releaseInfo();
			}	else {
				m_flags = UNIT_GOAL;
			}
		}	else {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
		}
	} else {
		// adding goal.
		if (!m_info) {
			DEBUG_ASSERTCRASH(m_flags == NO_UNITS, ("Bad flags."));
			allocateInfo(pos);
		}
		if (!m_info) {
			DEBUG_CRASH(("Ran out of pathfind cells - fatal error!!!!! jba."));
			return;
		}
		if (m_info->m_goalUnitID!=INVALID_ID && (m_info->m_goalUnitID==m_info->m_posUnitID)) {
			// A unit is already occupying this cell.
			return;
		}
		m_info->m_posUnitID = unitID;
		if (unitID==m_info->m_goalUnitID) {
			m_flags = UNIT_PRESENT_FIXED;
		} else if (m_info->m_goalUnitID==INVALID_ID) {
			m_flags = UNIT_PRESENT_MOVING;
		}	else {
			m_flags = UNIT_GOAL_OTHER_MOVING;
		}
	}
}


/**
 * Return the relevant obstacle ID.
 */
inline ObjectID PathfindCell::getObstacleID() const
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		return m_obstacleID;
	}

	return m_info ? m_info->m_obstacleID : INVALID_ID;
#else
	return m_obstacleID;
#endif
}


/**
 * Flag this cell as an obstacle, from the given one.
 * Return true if cell was flagged.
 */
Bool PathfindCell::setTypeAsObstacle( Object *obstacle, Bool isFence, const ICoord2D &pos )
{
	if (m_type!=PathfindCell::CELL_CLEAR && m_type != PathfindCell::CELL_IMPASSABLE) {
		return false;
	}

	Bool isRubble = false;
	if (obstacle->getBodyModule() && obstacle->getBodyModule()->getDamageState() == BODY_RUBBLE)
	{
		isRubble = true;
	}

	if (isRubble) {
		m_type = PathfindCell::CELL_RUBBLE;
		m_obstacleID = INVALID_ID;
		m_obstacleIsFence = false;
		m_obstacleIsTransparent = false;
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
		if (s_useFixedPathfinding) {
			return true;
		}

		if (m_info) {
			m_info->m_obstacleID = INVALID_ID;
			releaseInfo();
		}
#endif
		return true;
	}

	m_type = PathfindCell::CELL_OBSTACLE;
	m_obstacleID = obstacle->getID();
	m_obstacleIsFence = isFence;
	m_obstacleIsTransparent = obstacle->isKindOf(KINDOF_CAN_SEE_THROUGH_STRUCTURE);
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	// TheSuperHackers @info In retail mode we need to track orphaned cells set as obstacles so we can cleanup and failover properly
	// So we always make sure to set and clear the local obstacle data on the PathfindCell regardless of retail compat or not
	if (s_useFixedPathfinding) {
		return true;
	}

	if (!m_info) {
		m_info = PathfindCellInfo::getACellInfo(this, pos);
		if (!m_info) {
			DEBUG_CRASH(("Not enough PathFindCellInfos in pool."));
			return false;
		}
	}
	m_info->m_obstacleID = obstacle->getID();
	m_info->m_obstacleIsFence = isFence;
	m_info->m_obstacleIsTransparent = obstacle->isKindOf(KINDOF_CAN_SEE_THROUGH_STRUCTURE);
#endif
	return true;
}

/**
 * Flag this cell as given type.
 */
void PathfindCell::setType( CellType type )
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		if (m_obstacleID != INVALID_ID) {
			DEBUG_ASSERTCRASH(type == PathfindCell::CELL_OBSTACLE, ("Wrong type."));
			m_type = PathfindCell::CELL_OBSTACLE;
			return;
		}
	}

	if (m_info && (m_info->m_obstacleID != INVALID_ID)) {
		DEBUG_ASSERTCRASH(type==PathfindCell::CELL_OBSTACLE, ("Wrong type."));
		m_type = PathfindCell::CELL_OBSTACLE;
		return;
	}
#else
	if (m_obstacleID != INVALID_ID) {
		DEBUG_ASSERTCRASH(type == PathfindCell::CELL_OBSTACLE, ("Wrong type."));
		m_type = PathfindCell::CELL_OBSTACLE;
		return;
	}
#endif
	m_type = type;
}

/**
 * Unflag this cell as an obstacle, from the given one.
 * Return true if this cell was previously flagged as an obstacle by this object.
 */
Bool PathfindCell::removeObstacle( Object *obstacle )
{
	if (m_type == PathfindCell::CELL_RUBBLE) {
		m_type = PathfindCell::CELL_CLEAR;
	}
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		if (m_obstacleID != obstacle->getID()) return false;
		m_type = PathfindCell::CELL_CLEAR;
		m_obstacleID = INVALID_ID;
		m_obstacleIsFence = false;
		m_obstacleIsTransparent = false;
		return true;
	}

	if (!m_info) return false;
	if (m_info->m_obstacleID != obstacle->getID()) return false;
	m_type = PathfindCell::CELL_CLEAR;
	m_info->m_obstacleID = INVALID_ID;
	releaseInfo();

#else
	if (m_obstacleID != obstacle->getID()) return false;
	m_type = PathfindCell::CELL_CLEAR;
#endif
	m_obstacleID = INVALID_ID;
	m_obstacleIsFence = false;
	m_obstacleIsTransparent = false;
	return true;
}

#if RETAIL_COMPATIBLE_PATHFINDING
// Retail compatible insertion sort
void PathfindCell::forwardInsertionSortRetailCompatible(PathfindCellList& list)
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed == FALSE && m_info->m_open == FALSE, ("Serious error - Invalid flags. jba"));

	// mark the newCell as being on the open list
	m_info->m_open = true;
	m_info->m_closed = false;

	if (list.m_head == nullptr)
	{
		list.m_head = this;
		list.m_tail = this;
		list.m_count = 1;
		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = nullptr;
		return;
	}

	const UnsignedInt lengthBefore = list.m_count;

	// insertion sort
	PathfindCell* currentCell = list.m_head;
	PathfindCell* previousCell = nullptr;
	UnsignedInt cellCount = 0;
	// GeneralsX: 34,750,199 insertions walked 2,018,873,617 cells in
	// one 12000-frame match, and each step is three dependent loads on three different objects:
	// the cell (for hasInfo), its info (for the cost and the link), and the next info (for the
	// next cell). Nothing here changes which cell it stops on; the loop is the same loop, with
	// the info two steps ahead started early so the chain overlaps instead of serialising.
	while (currentCell && currentCell->hasInfo() && cellCount < PATHFIND_OPEN_WALK_CAP && currentCell->m_info->m_totalCost <= m_info->m_totalCost)
	{
		cellCount++;
		previousCell = currentCell;
		PathfindCellInfo *nextInfo = currentCell->m_info->m_nextOpen;
		if (nextInfo)
		{
			if (nextInfo->m_nextOpen) GX_PREFETCH(nextInfo->m_nextOpen);
			currentCell = nextInfo->m_cell;
		}
		else
		{
			currentCell = nullptr;
		}
	}

	// GeneralsX: Open-list insertion counters, observation only.
	// The question they answer is the one that decides whether this walk is worth replacing:
	// how far does it actually go, does the PATHFIND_OPEN_WALK_CAP cap ever truncate it, and
	// how much shorter would the same insertion point be from the tail end.
	if (gxPathTraceEnabled())
	{
		++s_gxInsN;
		s_gxInsSteps += cellCount;
		if (cellCount > s_gxInsMax) s_gxInsMax = cellCount;
		if (lengthBefore > s_gxInsLenMax) s_gxInsLenMax = lengthBefore;
		// The cap truncated the walk iff we stopped on the step count while the cell we stopped
		// on would otherwise have been walked past.
		if (cellCount >= PATHFIND_OPEN_WALK_CAP && currentCell && currentCell->hasInfo() &&
		    currentCell->m_info->m_totalCost <= m_info->m_totalCost)
			++s_gxInsCap;
		// A tail-side walk lands on the same index k; it costs (L-1-k) steps, or 0 when the new
		// cell simply appends. Only meaningful while the count is trustworthy.
		if (list.m_countValid && lengthBefore > cellCount)
			s_gxInsRevSteps += (lengthBefore - 1u - cellCount);
	}

	// TheSuperHackers @bugfix wh1ter0se69 04/08/2026 Retail can leave a cell linked into the open list
	// after its PathfindCellInfo is gone, so the walk above can stop on one. Discard the unusable
	// remainder instead of dereferencing it, and fail over the same way the other detections do. (#2799)
	Bool discardedRemainder = false;
	if (currentCell && !currentCell->hasInfo())
	{
		currentCell = nullptr;
		discardedRemainder = true;
		s_useFixedPathfinding = true;
		s_forceCleanCells = true;
		if (gxPathTraceEnabled()) ++s_gxInsCorrupt;
	}

	if (currentCell)
	{
		// insert just before "currentCell"
		if (currentCell->m_info->m_prevOpen)
			currentCell->m_info->m_prevOpen->m_nextOpen = this->m_info;
		else
			list.m_head = this;

		m_info->m_prevOpen = currentCell->m_info->m_prevOpen;
		currentCell->m_info->m_prevOpen = this->m_info;

		m_info->m_nextOpen = currentCell->m_info;

		++list.m_count;
	}
	else if (previousCell)
	{
		// append after "previousCell" - we are at the end of the list
		previousCell->m_info->m_nextOpen = this->m_info;
		m_info->m_prevOpen = previousCell->m_info;
		m_info->m_nextOpen = nullptr;

		list.m_tail = this;
		++list.m_count;
	}
	else
	{
		// the list head itself was unusable, so this cell becomes the list
		list.m_head = this;
		list.m_tail = this;
		list.m_count = 1;
		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = nullptr;
	}

	// Everything after the insertion point was just unlinked without being counted down, so the
	// running length is a guess from here on. Say so rather than carry a number that looks exact.
	if (discardedRemainder)
		list.m_countValid = false;
}
#endif

// Forward insertion sort, returns early if the list is being initialized or we are prepending the list
void PathfindCell::forwardInsertionSort(PathfindCellList& list)
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed == FALSE && m_info->m_open == FALSE, ("Serious error - Invalid flags. jba"));

	// mark the new cell as being on the open list
	m_info->m_open = true;
	m_info->m_closed = false;

	// GeneralsX: Every path through this function links exactly one
	// cell in, so the length moves here once rather than at each of the returns.
	++list.m_count;

	if (list.m_head == nullptr) {
		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = nullptr;
		list.m_head = this;
		list.m_tail = this;
		list.m_satHead = (m_info->m_totalCost == 0xFFFFu) ? this : nullptr;
		return;
	}

	// If the node needs inserting before the current list head
	if (m_info->m_totalCost < list.m_head->m_info->m_totalCost) {
		m_info->m_prevOpen = nullptr;
		list.m_head->m_info->m_prevOpen = this->m_info;
		m_info->m_nextOpen = list.m_head->m_info;
		list.m_head = this;
		return;
	}

	// GeneralsX: A cell that sorts last is appended without a walk: the walk below
	// would stop at the tail anyway, so the order is unchanged. Since gxPathCost16 saturates, a search that runs past
	// 65535 fills its open list with equal costs; head and tail are then equally far from every new cell, so
	// canReverseSort sends each one here, and walking the whole list per insertion cost 450 ms for 20,000 cells
	// (pathcost-counter-test). The fixed pathfinder only: the retail walk stops after
	// PATHFIND_OPEN_WALK_CAP steps and can leave the list unsorted before a failover to this sort, and only a
	// sorted list puts the walk's stop at the tail.
#if !RETAIL_COMPATIBLE_PATHFINDING
	if (list.m_tail && m_info->m_totalCost >= list.m_tail->m_info->m_totalCost) {
		m_info->m_prevOpen = list.m_tail->m_info;
		m_info->m_nextOpen = nullptr;
		list.m_tail->m_info->m_nextOpen = this->m_info;
		list.m_tail = this;
		// GeneralsX: a saturated cell at the tail extends (or starts) the run of 65535s; any other cell ends it
		list.m_satHead = (m_info->m_totalCost != 0xFFFFu) ? nullptr : (list.m_satHead ? list.m_satHead : this);
		return;
	}
#endif

	// Traverse the list to find correct position
	PathfindCell* current = list.m_head;
	while (current->m_info->m_nextOpen && current->m_info->m_nextOpen->m_totalCost <= m_info->m_totalCost) {
		current = current->getNextOpen();
	}

	// Insert the new node in the correct position
	m_info->m_nextOpen = current->m_info->m_nextOpen;
	if (current->m_info->m_nextOpen != nullptr) {
		current->m_info->m_nextOpen->m_prevOpen = this->m_info;
	}
	else {
		list.m_tail = this;
		list.m_satHead = (m_info->m_totalCost != 0xFFFFu) ? nullptr : (list.m_satHead ? list.m_satHead : this);	// GeneralsX, as above
	}

	current->m_info->m_nextOpen = this->m_info;
	m_info->m_prevOpen = current->m_info;
}

// Reverse insertion sort, returns early if the list is being initialized or we are appending the list
void PathfindCell::reverseInsertionSort(PathfindCellList& list)
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed == FALSE && m_info->m_open == FALSE, ("Serious error - Invalid flags. jba"));

	// mark the new cell as being on the open list
	m_info->m_open = true;
	m_info->m_closed = false;

	// GeneralsX: See forwardInsertionSort - one link, one increment.
	++list.m_count;

	if (list.m_tail == nullptr) {
		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = nullptr;
		list.m_tail = this;
		list.m_head = this;
		list.m_satHead = (m_info->m_totalCost == 0xFFFFu) ? this : nullptr;
		return;
	}

	// If the node needs inserting after the current list tail
	if (m_info->m_totalCost >= list.m_tail->m_info->m_totalCost) {
		m_info->m_prevOpen = list.m_tail->m_info;
		list.m_tail->m_info->m_nextOpen = this->m_info;
		m_info->m_nextOpen = nullptr;
		list.m_tail = this;
		// GeneralsX: a saturated cell at the tail extends (or starts) the run of 65535s; any other cell ends it
		list.m_satHead = (m_info->m_totalCost != 0xFFFFu) ? nullptr : (list.m_satHead ? list.m_satHead : this);
		return;
	}

	// Traverse the list to find correct position
	// GeneralsX: Every cell from m_satHead to the tail costs 65535, more than this one,
	// so the walk from the tail would pass all of them: start it at m_satHead instead. Same landing point.
	PathfindCell* current = (list.m_satHead && m_info->m_totalCost < 0xFFFFu) ? list.m_satHead : list.m_tail;
	while (current->m_info->m_prevOpen && current->m_info->m_prevOpen->m_totalCost > m_info->m_totalCost) {
		current = current->getPrevOpen();
	}

	// Insert the new node in the correct position
	m_info->m_prevOpen = current->m_info->m_prevOpen;
	if (current->m_info->m_prevOpen != nullptr) {
		current->m_info->m_prevOpen->m_nextOpen = this->m_info;
	}
	else {
		list.m_head = this;
	}

	current->m_info->m_prevOpen = this->m_info;
	m_info->m_nextOpen = current->m_info;
}

// The slot a cell info occupies in the pool, or -1 if it is not from the pool. Used only to key
// GXOpenListIndex::m_filedUnder.
static inline Int gxInfoSlot(const PathfindCellInfo *info, const PathfindCellInfo *base)
{
	if (info == nullptr || base == nullptr)
		return -1;
	const ptrdiff_t slot = info - base;
	if (slot < 0 || slot >= (ptrdiff_t)CELL_INFOS_TO_ALLOCATE)
		return -1;
	return (Int)slot;
}

Bool PathfindCell::indexedInsertRetailEquivalent(PathfindCellList& list)
{
	if (!list.m_indexValid || m_info == nullptr)
		return false;

#if RETAIL_COMPATIBLE_PATHFINDING
	// The retail walk gives up after PATHFIND_OPEN_WALK_CAP steps and inserts wherever it got
	// to, which is the one case where its answer differs from this one. It cannot take more steps
	// than the list is long, so this is the whole of the safety condition.
	// GeneralsX: Retail only: the fixed pathfinder's walks have no
	// step cap, so for them the landing point below holds at any list length.
	if (!list.m_countValid || list.m_count >= PATHFIND_OPEN_WALK_CAP)
		return false;
#endif

#if RETAIL_COMPATIBLE_PATHFINDING
	if (list.m_index == nullptr)
	{
		list.m_index = MSGNEW("PathfindOpenIndex") GXOpenListIndex;
		memset(list.m_index, 0, sizeof(GXOpenListIndex));
		// An index allocated while cells are already linked knows nothing about them.
		list.m_indexNeedsSeed = (list.m_head != nullptr);
	}
#else
	// GeneralsX: Allocated once by PathfindCellList::allocateIndex; if that failed, walk.
	if (list.m_index == nullptr)
		return false;
#endif
	GXOpenListIndex *ix = list.m_index;

	if (list.m_indexNeedsSeed)
	{
		// reset(newHead) installs exactly one cell without going through an insertion. Anything
		// else means the index has missed something, and then it must not be used at all.
		if (list.m_head == nullptr)
		{
			list.m_indexNeedsSeed = false;	// the seeded head was popped again; nothing to carry
		}
		else if (list.m_count == 1 && list.m_head->hasInfo() &&
		         list.m_head->m_info->m_nextOpen == nullptr && list.m_head->m_info->m_prevOpen == nullptr)
		{
			PathfindCellInfo *h = list.m_head->m_info;
			const UnsignedInt hc = h->m_totalCost;
			gxCostSet(ix, hc);
			ix->m_tailAtCost[hc] = h;
			const Int hslot = gxInfoSlot(h, PathfindCellInfo::s_infoArray);
			if (hslot < 0) { list.m_indexValid = false; return false; }
			ix->m_filedUnder[hslot] = (UnsignedShort)hc;
			list.m_indexNeedsSeed = false;
		}
		else
		{
			list.m_indexValid = false;
			return false;
		}
	}

	const Int slot = gxInfoSlot(m_info, PathfindCellInfo::s_infoArray);
	if (slot < 0)
		return false;

	const UnsignedInt c = m_info->m_totalCost;

	PathfindCellInfo *prev = nullptr;
	if (gxCostPresent(ix, c))
	{
		prev = ix->m_tailAtCost[c];
	}
	else
	{
		const Int p = gxCostPrev(ix, c);
		if (p >= 0)
			prev = ix->m_tailAtCost[p];
	}

	// Under GX_PATHIDXCHECK, walk it the old way as well and compare where the two would land.
	// This is the arm that can fail; it is never reset, so one disagreement anywhere in a match
	// stays on every line after it.
	// GeneralsX: Its own switch, no longer GX_PATHTRACE: the extra walk
	// costs what the index saves, so a GX_PATHTRACE timing run must not pay it. The fixed pathfinder's
	// walks have no step cap, so neither does this one there.
	if (gxPathTraceEnabled())
		++s_gxIdxUsed;
#if defined(GX_TEST_INSTRUMENTS)
	if (gxPathIdxCheckEnabled())
	{
		PathfindCell *cur = list.m_head;
		PathfindCell *walkPrev = nullptr;
		UnsignedInt steps = 0;
		while (cur && cur->hasInfo() &&
#if RETAIL_COMPATIBLE_PATHFINDING
		       steps < PATHFIND_OPEN_WALK_CAP &&
#endif
		       cur->m_info->m_totalCost <= m_info->m_totalCost)
		{
			++steps;
			walkPrev = cur;
			cur = cur->getNextOpen();
		}
		if (prev != (walkPrev ? walkPrev->m_info : nullptr))
			++s_gxIdxBad;
	}
#endif

	m_info->m_open = true;
	m_info->m_closed = false;

	if (prev != nullptr)
	{
		m_info->m_prevOpen = prev;
		m_info->m_nextOpen = prev->m_nextOpen;
		if (prev->m_nextOpen)
			prev->m_nextOpen->m_prevOpen = m_info;
		else
		{
			list.m_tail = this;
			// GeneralsX: a new tail extends (or starts) the run of 65535s, or ends it - as the fixed sorts keep m_satHead
			list.m_satHead = (c != 0xFFFFu) ? nullptr : (list.m_satHead ? list.m_satHead : this);
		}
		prev->m_nextOpen = m_info;
	}
	else
	{
		// nothing on the list costs as little as this, so it becomes the head
		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = list.m_head ? list.m_head->m_info : nullptr;
		if (list.m_head)
			list.m_head->m_info->m_prevOpen = m_info;
		else
		{
			list.m_tail = this;
			list.m_satHead = (c == 0xFFFFu) ? this : nullptr;	// GeneralsX: the only cell, as the fixed sorts do
		}
		list.m_head = this;
	}

	gxCostSet(ix, c);
	ix->m_tailAtCost[c] = m_info;
	ix->m_filedUnder[slot] = (UnsignedShort)c;
	++list.m_count;
	return true;
}

/// put self on "open" list in ascending cost order, return new list
void PathfindCell::putOnSortedOpenList( PathfindCellList &list )
{
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		if (indexedInsertRetailEquivalent(list))
			return;
		// The walk does not update the index, so from here the index has missed a cell and stays
		// out of use until the next reset(). That is the same behaviour this build shipped with
		// before the index existed, for the rest of one search.
		if (gxPathTraceEnabled()) ++s_gxIdxWalk;
		list.m_indexValid = false;
		forwardInsertionSortRetailCompatible(list);
		return;
	}
#else
	// GeneralsX: THE COST INDEX WAS RETAIL-ONLY, SO IT WENT DARK WITH RETAIL ON 01/10.
	// 9dd4ad8f5 turned RETAIL_COMPATIBLE_PATHFINDING off, and with it every insertion went back to walking the
	// list (from the nearer end, below). Both walks put the cell after the last cell whose cost is <= its own -
	// last among equals on an ascending list - which is the place the index looks up directly, so the open list
	// comes out in the same order and every search expands the same cells: paths are identical. GX_PATHIDXCHECK (test builds)
	// re-walks every indexed insertion and counts disagreements (idxbad=). If the index cannot be used it says
	// so, and the walks below take over for the rest of this search, as the retail arm above does.
	if (indexedInsertRetailEquivalent(list))
		return;
	if (gxPathTraceEnabled()) ++s_gxIdxWalk;
	list.m_indexValid = false;
#endif

	// TheSuperHackers @performance Mauller 20/03/2026 Implement reverse insertion sorting.
	// Long and complex paths often append PathfindCell's, with high total path costs, to the open list.
	// Appending and reverse traversal allow faster insertion of these cells, reducing pathfinding overhead by 50 - 66%.
	if (list.canReverseSort(*this)) {
		reverseInsertionSort(list);
	}
	else {
		forwardInsertionSort(list);
	}
}

/// remove self from "open" list
void PathfindCell::removeFromOpenList( PathfindCellList &list )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed==FALSE && m_info->m_open==TRUE, ("Serious error - Invalid flags. jba"));

	// GeneralsX: Take this cell out of the cost index before the
	// links go, since deciding whether it was the last of its cost needs its predecessor. It is
	// un-filed from the cost it was FILED under, not the cost it carries now - see m_filedUnder.
	// GeneralsX: Both pathfinders now (see putOnSortedOpenList).
	if (list.m_index != nullptr && list.m_indexValid && !list.m_indexNeedsSeed && m_info != nullptr)
	{
		const Int slot = gxInfoSlot(m_info, PathfindCellInfo::s_infoArray);
		if (slot < 0)
		{
			list.m_indexValid = false;
		}
		else
		{
			const UnsignedInt c = list.m_index->m_filedUnder[slot];
			if (gxCostPresent(list.m_index, c) && list.m_index->m_tailAtCost[c] == m_info)
			{
				// It was the last cell of its cost. The one in front of it takes over if it has
				// the same cost, otherwise that cost is no longer on the list at all.
				PathfindCellInfo *p = m_info->m_prevOpen;
				const Int pslot = gxInfoSlot(p, PathfindCellInfo::s_infoArray);
				if (pslot >= 0 && list.m_index->m_filedUnder[pslot] == c)
					list.m_index->m_tailAtCost[c] = p;
				else
					gxCostClear(list.m_index, c);
			}
		}
	}

	// GeneralsX: The saturated run starts at the next cell now (or is empty).
	if (list.m_satHead == this)
		list.m_satHead = getNextOpen();

	if (m_info->m_nextOpen)
		m_info->m_nextOpen->m_prevOpen = m_info->m_prevOpen;
	else {
		list.m_tail = getPrevOpen();
	}

	if (m_info->m_prevOpen)
		m_info->m_prevOpen->m_nextOpen = m_info->m_nextOpen;
	else
		list.m_head = getNextOpen();

	m_info->m_open = false;
	m_info->m_nextOpen = nullptr;
	m_info->m_prevOpen = nullptr;

	// GeneralsX: One cell unlinked, one off the length.
	if (list.m_count) --list.m_count;

}

/// remove all cells from "open" list
Int PathfindCell::releaseOpenList( PathfindCellList &list )
{
	Int count = 0;
	while (list.m_head) {
		count++;
		if (list.m_count) --list.m_count;
		DEBUG_ASSERTCRASH(list.m_head->m_info, ("Has to have info."));
		DEBUG_ASSERTCRASH(list.m_head->m_info->m_closed==FALSE && list.m_head->m_info->m_open==TRUE, ("Serious error - Invalid flags. jba"));
		PathfindCell *cur = list.m_head;
		PathfindCellInfo *curInfo = list.m_head->m_info;

#if RETAIL_COMPATIBLE_PATHFINDING
		// TheSuperHackers @info This is only here to catch a crash point in the retail compatible pathfinding
		// One crash mode is where a cell has no PathfindCellInfo, resulting in a nullptr access and a crash.
		// Therefore we signal that we need to clean the maps cells and the PathfindCellInfos
		if(!curInfo && !s_useFixedPathfinding) {
			s_useFixedPathfinding = true;
			s_forceCleanCells = true;
			list.m_countValid = false;	// GeneralsX: list abandoned mid-teardown, length is a guess now
			list.m_indexValid = false;	// GeneralsX: and the cost index no longer describes it
			return count;
		}
#endif

		if (curInfo->m_nextOpen) {
			list.m_head = curInfo->m_nextOpen->m_cell;
		} else {
			list.reset();
		}
		DEBUG_ASSERTCRASH(cur == curInfo->m_cell, ("Bad backpointer in PathfindCellInfo"));
		curInfo->m_nextOpen = nullptr;
		curInfo->m_prevOpen = nullptr;
		curInfo->m_open = FALSE;
		cur->releaseInfo();
	}
	return count;
}

/// remove all cells from "closed" list
Int PathfindCell::releaseClosedList( PathfindCellList &list )
{
	Int count = 0;
	while (list.m_head) {
		count++;
		if (list.m_count) --list.m_count;
		DEBUG_ASSERTCRASH(list.m_head->m_info, ("Has to have info."));
		DEBUG_ASSERTCRASH(list.m_head->m_info->m_closed==TRUE && list.m_head->m_info->m_open==FALSE, ("Serious error - Invalid flags. jba"));
		PathfindCell *cur = list.m_head;
		PathfindCellInfo *curInfo = list.m_head->m_info;
#if RETAIL_COMPATIBLE_PATHFINDING
		// TheSuperHackers @info This is only here to catch a crash point in the retail compatible pathfinding
		// One crash mode is where a cell has no PathfindCellInfo, resulting in a nullptr access and a crash.
		// Therefore we signal that we need to clean the maps cells and the PathfindCellInfos
		if(!curInfo && !s_useFixedPathfinding) {
			s_useFixedPathfinding = true;
			s_forceCleanCells = true;
			list.m_countValid = false;	// GeneralsX: list abandoned mid-teardown, length is a guess now
			return count;
		}
#endif

		if (curInfo->m_nextOpen) {
			list.m_head = curInfo->m_nextOpen->m_cell;
		} else {
			list.reset();
		}
		DEBUG_ASSERTCRASH(cur == curInfo->m_cell, ("Bad backpointer in PathfindCellInfo"));
		curInfo->m_nextOpen = nullptr;
		curInfo->m_prevOpen = nullptr;
		curInfo->m_closed = FALSE;
		cur->releaseInfo();
	}
	return count;
}

/// put self on "closed" list, return new list
void PathfindCell::putOnClosedList( PathfindCellList &list )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed==FALSE && m_info->m_open==FALSE, ("Serious error - Invalid flags. jba"));
	// only put on list if not already on it
	if (m_info->m_closed == FALSE)
	{
		m_info->m_closed = FALSE;
		m_info->m_closed = TRUE;

#if RETAIL_COMPATIBLE_PATHFINDING
		// TheSuperHackers @bugfix wh1ter0se69 04/08/2026 The closed list head can have lost its
		// PathfindCellInfo for the same reason. Discard the unusable list instead of dereferencing
		// it, and fail over the same way the other detections do. (#2637)
		if (list.m_head && !list.m_head->hasInfo())
		{
			list.m_head = nullptr;
			s_useFixedPathfinding = true;
			s_forceCleanCells = true;
			list.m_count = 0;
			list.m_countValid = false;	// GeneralsX: rest of the list discarded, length is a guess now
		}
#endif

		m_info->m_prevOpen = nullptr;
		m_info->m_nextOpen = list.m_head ? list.m_head->m_info : nullptr;
		if (list.m_head)
			list.m_head->m_info->m_prevOpen = this->m_info;

		list.m_head = this;
		if (list.m_count == 0) list.m_tail = this;
		++list.m_count;
	}

}

/// remove self from "closed" list
void PathfindCell::removeFromClosedList( PathfindCellList &list )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	DEBUG_ASSERTCRASH(m_info->m_closed==TRUE && m_info->m_open==FALSE, ("Serious error - Invalid flags. jba"));
	if (m_info->m_nextOpen)
		m_info->m_nextOpen->m_prevOpen = m_info->m_prevOpen;
	else
		list.m_tail = getPrevOpen();

	if (m_info->m_prevOpen)
		m_info->m_prevOpen->m_nextOpen = m_info->m_nextOpen;
	else
		list.m_head = getNextOpen();

	m_info->m_closed = false;
	m_info->m_nextOpen = nullptr;
	m_info->m_prevOpen = nullptr;

	// GeneralsX: One cell unlinked, one off the length. m_tail is
	// maintained here for the first time; nothing reads the closed list's tail, so this is
	// bookkeeping only, and it keeps the two removal paths the same shape.
	if (list.m_count) --list.m_count;

}

/**
 * Return true if the given object ID is registered as an obstacle in this cell
 */
inline Bool PathfindCell::isObstaclePresent(ObjectID objID) const
{
	if (objID != INVALID_ID && (getType() == PathfindCell::CELL_OBSTACLE))
	{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
		if (s_useFixedPathfinding) {
			return m_obstacleID == objID;
		}

		DEBUG_ASSERTCRASH(m_info, ("Should have info to be obstacle."));
		return (m_info && m_info->m_obstacleID == objID);
#else
		return m_obstacleID == objID;
#endif
	}

	return false;
}


/**
 * return true if the obstacle in the cell is KINDOF_CAN_SEE_THROUGHT_STRUCTURE
 */
inline Bool PathfindCell::isObstacleTransparent() const
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		return m_obstacleIsTransparent;
	}

	return m_info ? m_info->m_obstacleIsTransparent : false;
#else
	return m_obstacleIsTransparent;
#endif
}

/**
 * return true if the given obstacle in the cell is a fence.
 */
inline Bool PathfindCell::isObstacleFence() const
{
#if RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
	if (s_useFixedPathfinding) {
		return m_obstacleIsFence;
	}

	return m_info ? m_info->m_obstacleIsFence : false;
#else
	return m_obstacleIsFence;
#endif
}


const Int COST_ORTHOGONAL = 10;
const Int COST_DIAGONAL = 14;
const Real COST_TO_DISTANCE_FACTOR = 1.0f/10.0f;
const Real COST_TO_DISTANCE_FACTOR_SQR = COST_TO_DISTANCE_FACTOR*COST_TO_DISTANCE_FACTOR;

UnsignedInt PathfindCell::costToGoal( PathfindCell *goal )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	Int dx = m_info->m_pos.x - goal->getXIndex();
	Int dy = m_info->m_pos.y - goal->getYIndex();
#define NO_REAL_DIST
#ifdef REAL_DIST
	Int cost = COST_ORTHOGONAL*sqrt(dx*dx + dy*dy);
#else
	if (dx<0) dx = -dx;
	if (dy<0) dy = -dy;
	Int cost;
	if (dx>dy) {
		cost= COST_ORTHOGONAL*dx + (COST_ORTHOGONAL*dy)/2;
	}	else {
		cost= COST_ORTHOGONAL*dy + (COST_ORTHOGONAL*dx)/2;
	}

#endif


	return cost;
}

UnsignedInt PathfindCell::costToHierGoal( PathfindCell *goal )
{
	if( !m_info )
	{
		DEBUG_CRASH( ("Has to have info.") );
		return 100000; //...patch hack 1.01
	}
	Int dx = m_info->m_pos.x - goal->getXIndex();
	Int dy = m_info->m_pos.y - goal->getYIndex();
	Int cost = REAL_TO_INT_FLOOR(COST_ORTHOGONAL*sqrt(dx*dx + dy*dy) + 0.5f);
	return cost;
}

UnsignedInt PathfindCell::costSoFar( PathfindCell *parent )
{
	DEBUG_ASSERTCRASH(m_info, ("Has to have info."));
	// very first node in path - no turns, no cost
	if (parent == nullptr)
		return 0;

	// add in number of turns in path so far
	ICoord2D prevDir;
	Int cost;

	prevDir.x = parent->getXIndex() - m_info->m_pos.x;
	prevDir.y = parent->getYIndex() - m_info->m_pos.y;

	// diagonal moves cost a bit more than orthogonal ones
	if (prevDir.x == 0 || prevDir.y == 0)
		cost = parent->getCostSoFar() + COST_ORTHOGONAL;
	else
		cost = parent->getCostSoFar() + COST_DIAGONAL;
	if (getPinched()) {
		cost += 1*COST_DIAGONAL;
	}

#if 1
	// Increase cost of turns.
	Int numTurns = 0;
	PathfindCell *prevCell = parent->getParentCell();
	if (prevCell) {

#if RETAIL_COMPATIBLE_PATHFINDING
		// TheSuperHackers @info this is a possible crash point in the retail pathfinding, we just prevent the crash at this point
		// External code should catch the issue in another block and cleanup the pathfinding before switching to the fixed pathfinding.
		if (!prevCell->hasInfo())
		{
			return cost;
		}
#endif

		ICoord2D dir;
		dir.x = prevCell->getXIndex() - parent->getXIndex();
		dir.y = prevCell->getYIndex() - parent->getYIndex();

		// count number of direction changes
		if (dir.x != prevDir.x || dir.y != prevDir.y)
		{
			Int dot = dir.x * prevDir.x + dir.y * prevDir.y;
			if (dot > 0)
				numTurns=4;				// 45 degree turn
			else if (dot == 0)
				numTurns = 8;		// 90 degree turn
			else
				numTurns = 16;		// 135 degree turn
		}
	}

	return cost + numTurns;
#else
	return cost;
#endif

}


inline Bool typesMatch(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	PathfindCell::CellType targetType = targetCell.getType();
	PathfindCell::CellType srcType = sourceCell.getType();
	if (targetType == srcType) return true;

	return false;
}

inline Bool waterGround(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	PathfindCell::CellType targetType = targetCell.getType();
	PathfindCell::CellType srcType = sourceCell.getType();
	if ( (targetType==PathfindCell::CELL_CLEAR &&
		(srcType&PathfindCell::CELL_WATER ))) {
			return true;
	}
	if ( (srcType==PathfindCell::CELL_CLEAR &&
		(targetType&PathfindCell::CELL_WATER ))) {
			return true;
	}

	return false;
}

inline Bool groundRubble(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	PathfindCell::CellType targetType = targetCell.getType();
	PathfindCell::CellType srcType = sourceCell.getType();
	if ( (targetType==PathfindCell::CELL_CLEAR &&
		(srcType==PathfindCell::CELL_RUBBLE ))) {
			return true;
	}
	if ( (srcType==PathfindCell::CELL_CLEAR &&
		(targetType==PathfindCell::CELL_RUBBLE ))) {
			return true;
	}

	return false;
}

inline Bool terrain(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	Int targetType = targetCell.getType();
	Int srcType = sourceCell.getType();
	if (targetType == PathfindCell::CELL_OBSTACLE) targetType = PathfindCell::CELL_CLEAR;
	if (srcType == PathfindCell::CELL_OBSTACLE) srcType = PathfindCell::CELL_CLEAR;
	if (targetType==srcType) {
		return true;
	}
	return false;
}

inline Bool crusherGround(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	Int targetType = targetCell.getType();
	Int srcType = sourceCell.getType();
	if (targetType==PathfindCell::CELL_OBSTACLE) {
		if (targetCell.isObstacleFence()) {
			if (srcType == PathfindCell::CELL_CLEAR) {
				return true;
			}
		}
	}
	if (srcType==PathfindCell::CELL_OBSTACLE) {
		if (sourceCell.isObstacleFence()) {
			if (targetType == PathfindCell::CELL_CLEAR) {
				return true;
			}
		}
	}
	return false;
}

inline Bool groundCliff(const PathfindCell &targetCell, const PathfindCell &sourceCell) {
	PathfindCell::CellType targetType = targetCell.getType();
	PathfindCell::CellType srcType = sourceCell.getType();

	if ( (targetType==PathfindCell::CELL_CLIFF ) &&
			 (srcType==PathfindCell::CELL_CLEAR) ) {
			return true;
	}
	if ( (targetType==PathfindCell::CELL_CLEAR ) &&
			 (srcType==PathfindCell::CELL_CLIFF) ) {
			return true;
	}
	return false;
}

static void __fastcall resolveBlockZones(Int srcZone, Int targetZone, zoneStorageType *zoneEquivalency, Int sizeOfZE)
{
	Int i;
	// We have two zones being combined now. Keep the lower zone.
	DEBUG_ASSERTCRASH(srcZone!=0 && targetZone!=0,  ("Bad resolve zones	."));
	if (targetZone<srcZone) {
		for (i=0; i<sizeOfZE; i++) {
			if (zoneEquivalency[i] == srcZone) {
				zoneEquivalency[i] = targetZone;
			}
		}
	} else {
		for (i=0; i<sizeOfZE; i++) {
			if (zoneEquivalency[i] == targetZone) {
				zoneEquivalency[i] = srcZone;
			}
		}
	}
}

// GeneralsX: resolveZones used to rescan the whole equivalence array
// (up to m_maxZone entries) on every merge, and flattenZones calls it again per mismatching entry, so
// one calculateZones was ~Z^2 in the zone count (20-65 ms per recalc on stress-sized synthetic maps).
// ZoneRelabel performs the SAME operation - "every entry whose value is a or b becomes f" - in
// near-constant time. Invariant: indices holding one value form exactly one union-find class, and
// m_groupOfValue[v] names that class (or -1 when no index holds v). A relabel unions the classes of
// a, b and f and gives the root value f, so every index that held a, b or f now holds f and nothing
// else changes - exactly the old loop. The old loop stopped at sizeOfZE; every caller passes
// m_maxZone, all values it handles are < m_maxZone, and indices >= m_maxZone still hold their own
// (identity) value, so no index past the bound could ever match a or b. Results are bit-identical.
class ZoneRelabel
{
public:
	void initIdentity(Int n)
	{
		m_group.resize(n); m_parent.resize(n); m_value.resize(n); m_groupOfValue.resize(n);
		for (Int i=0; i<n; i++) {
			m_group[i] = i; m_parent[i] = i; m_value[i] = i; m_groupOfValue[i] = i;
		}
	}
	void initFrom(const zoneStorageType *zoneArray, Int n)
	{
		m_group.resize(n); m_parent.clear(); m_value.clear();
		m_groupOfValue.assign(65536, -1); // every zoneStorageType value
		for (Int i=0; i<n; i++) {
			Int v = zoneArray[i];
			if (m_groupOfValue[v] < 0) {
				m_groupOfValue[v] = (Int)m_parent.size();
				m_parent.push_back((Int)m_parent.size());
				m_value.push_back(v);
			}
			m_group[i] = m_groupOfValue[v];
		}
	}
	Int get(Int i) { return m_value[find(m_group[i])]; }
	void relabel(Int a, Int b, Int f)
	{
		Int root = -1;
		join(root, m_groupOfValue[a]);
		join(root, m_groupOfValue[b]);
		if (root < 0) {
			return; // no index holds a or b: the old loop changed nothing
		}
		join(root, m_groupOfValue[f]);
		m_value[root] = f;
		if (a != f) m_groupOfValue[a] = -1;
		if (b != f) m_groupOfValue[b] = -1;
		m_groupOfValue[f] = root;
	}
	void materialize(zoneStorageType *out, Int n)
	{
		for (Int i=0; i<n; i++) {
			out[i] = (zoneStorageType)get(i);
		}
	}
private:
	Int find(Int g)
	{
		while (m_parent[g] != g) {
			m_parent[g] = m_parent[m_parent[g]];
			g = m_parent[g];
		}
		return g;
	}
	void join(Int &root, Int g)
	{
		if (g < 0) return;
		g = find(g);
		if (root < 0) root = g;
		else if (g != root) m_parent[g] = root;
	}
	std::vector<Int> m_group, m_parent, m_value, m_groupOfValue;
};

static void resolveZones(Int srcZone, Int targetZone, ZoneRelabel &zoneEquivalency)
{
	// We have two zones being combined now. Keep the lower zone.
	DEBUG_ASSERTCRASH(srcZone!=0 && targetZone!=0,  ("Bad resolve zones	."));
	srcZone = zoneEquivalency.get(srcZone);
	targetZone = zoneEquivalency.get(targetZone);
	zoneStorageType finalZone;
	if (targetZone<srcZone) {
		finalZone = zoneEquivalency.get(targetZone);
	} else {
		finalZone = zoneEquivalency.get(srcZone);
	}
	zoneEquivalency.relabel(srcZone, targetZone, finalZone);
}

static void flattenZones(zoneStorageType *zoneArray, zoneStorageType *zoneHierarchical, Int sizeOfZones)
{
	Int i;
	for (i=0; i<sizeOfZones; i++) {
		Int zone1 = zoneArray[i];
		Int zone2 = zoneHierarchical[zone1];
		zone1 = zoneArray[zone2];
		zone2 = zoneHierarchical[zone1];
		zoneArray[i] = zone2;
	}
#if 1

	ZoneRelabel relabel;
	relabel.initFrom(zoneArray, sizeOfZones);
	for (i=0; i<sizeOfZones; i++) {
		Int zone1 = relabel.get(i);
		Int zone2 = zoneHierarchical[i];
		if (zone1!=zone2) {
			resolveZones(zone1, zone2, relabel);
		}
	}
	relabel.materialize(zoneArray, sizeOfZones);
#endif
}

inline void applyZone(PathfindCell &targetCell, const PathfindCell &sourceCell, ZoneRelabel &zoneEquivalency)
{
	DEBUG_ASSERTCRASH(sourceCell.getZone()!=0, ("Unset source zone."));
	Int srcZone = zoneEquivalency.get(sourceCell.getZone());
	Int targetZone = zoneEquivalency.get(targetCell.getZone());

	if (targetZone == 0) {
		targetCell.setZone(srcZone);
		return;
	}
	if (targetZone == srcZone) {
		return; // already match.
	}
	resolveZones(srcZone, targetZone, zoneEquivalency);

}

inline void applyBlockZone(PathfindCell &targetCell, const PathfindCell &sourceCell,
													 zoneStorageType *zoneEquivalency, Int firstZone, Int sizeOfZE)
{
	DEBUG_ASSERTCRASH(sourceCell.getZone()>=firstZone && sourceCell.getZone()<firstZone+sizeOfZE, ("Memory overrun - FATAL ERROR."));
	Int srcZone = zoneEquivalency[sourceCell.getZone()-firstZone];
	DEBUG_ASSERTCRASH(targetCell.getZone()>=firstZone && sourceCell.getZone()<firstZone+sizeOfZE, ("Memory overrun - FATAL ERROR."));
	Int targetZone = zoneEquivalency[targetCell.getZone()-firstZone];
	if (targetZone == srcZone) {
		return; // already match.
	}
	resolveBlockZones(srcZone, targetZone, zoneEquivalency, sizeOfZE);

}

//------------------------  ZoneBlock  -------------------------------
ZoneBlock::ZoneBlock() : m_firstZone(0),
m_numZones(0),
m_groundCliffZones(nullptr),
m_groundWaterZones(nullptr),
m_groundRubbleZones(nullptr),
m_crusherZones(nullptr),
m_zonesAllocated(0),
m_interactsWithBridge(FALSE)
{
	m_cellOrigin.x = 0;
	m_cellOrigin.y = 0;
	m_firstZone = 0;
}

ZoneBlock::~ZoneBlock()
{
	freeZones();
}

void ZoneBlock::freeZones()
{
	delete [] m_groundCliffZones;
	m_groundCliffZones = nullptr;

	delete [] m_groundWaterZones;
	m_groundWaterZones = nullptr;

	delete [] m_groundRubbleZones;
	m_groundRubbleZones = nullptr;

	delete [] m_crusherZones;
	m_crusherZones = nullptr;
}

/* Allocate zone equivalency arrays large enough to hold required entries.  If the arrays are already
large enough, reuse.  Then calculate terrain equivalencies. */
void ZoneBlock::blockCalculateZones(PathfindCell **map, PathfindLayer layers[], const IRegion2D &bounds)
{
	Int i, j;
	m_cellOrigin = bounds.lo;
	UnsignedInt minZone = map[bounds.lo.x][bounds.lo.y].getZone();
	UnsignedInt maxZone = minZone;

	for( j=bounds.lo.y; j<=bounds.hi.y; j++ )	{
		for( i=bounds.lo.x; i<=bounds.hi.x; i++ )	{
			PathfindCell *cell = &map[i][j];
			zoneStorageType zone = cell->getZone();
			if (minZone>zone) minZone=zone;
			if (maxZone<zone) maxZone=zone;
		}
	}
	m_firstZone = minZone;
	m_numZones = 1 + maxZone - minZone;

	allocateZones();

	if (m_numZones==1) return; // all zones are equivalent.

	// Determine water/ground equivalent zones, and ground/cliff equivalent zones.
	for (i=0; i<m_zonesAllocated; i++) {
		m_groundCliffZones[i] = i+m_firstZone;
		m_groundWaterZones[i] = i+m_firstZone;
		m_groundRubbleZones[i] = i+m_firstZone;
		m_crusherZones[i] = i+m_firstZone;
	}

	for( j=bounds.lo.y; j<=bounds.hi.y; j++ )	{
		for( i=bounds.lo.x; i<=bounds.hi.x; i++ )	{
			if (i>bounds.lo.x && map[i][j].getZone()!=map[i-1][j].getZone()) {

				if (waterGround(map[i][j], map[i-1][j])) {
					applyBlockZone(map[i][j], map[i-1][j], m_groundWaterZones, m_firstZone, m_numZones);
				}
				if (groundRubble(map[i][j], map[i-1][j])) {
					applyBlockZone(map[i][j], map[i-1][j], m_groundRubbleZones, m_firstZone, m_numZones);
				}
				if (groundCliff(map[i][j], map[i-1][j])) {
					applyBlockZone(map[i][j], map[i-1][j], m_groundCliffZones, m_firstZone, m_numZones);
				}
				if (crusherGround(map[i][j], map[i-1][j])) {
					applyBlockZone(map[i][j], map[i-1][j], m_crusherZones, m_firstZone, m_numZones);
				}
			}
			if (j>bounds.lo.y && map[i][j].getZone()!=map[i][j-1].getZone()) {
				if (waterGround(map[i][j],map[i][j-1])) {
					applyBlockZone(map[i][j], map[i][j-1], m_groundWaterZones, m_firstZone, m_numZones);
				}
				if (groundRubble(map[i][j], map[i][j-1])) {
					applyBlockZone(map[i][j], map[i][j-1], m_groundRubbleZones, m_firstZone, m_numZones);
				}
				if (groundCliff(map[i][j],map[i][j-1])) {
					applyBlockZone(map[i][j], map[i][j-1], m_groundCliffZones, m_firstZone, m_numZones);
				}
				if (crusherGround(map[i][j], map[i][j-1])) {
					applyBlockZone(map[i][j], map[i][j-1], m_crusherZones, m_firstZone, m_numZones);
				}
			}
			DEBUG_ASSERTCRASH(map[i][j].getZone() != 0, ("Cleared the zone."));
		}
	}

}

//
// Return the zone at this location.
//
zoneStorageType ZoneBlock::getEffectiveZone( LocomotorSurfaceTypeMask acceptableSurfaces,
																					 Bool crusher, zoneStorageType zone) const
{
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	if (zone==PathfindZoneManager::UNINITIALIZED_ZONE) {
		return zone;
	}
#endif

	if (acceptableSurfaces&LOCOMOTORSURFACE_AIR) return 1; // air is all zone 1.

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_CLIFF)) {
		// Locomotors can go on ground, water & cliff, so all is zone 1.
		return 1;
	}
	if (m_numZones<2) {
		return m_firstZone; // if we only got 1 zone, it's all the same zone.
	}
	DEBUG_ASSERTCRASH(zone >=m_firstZone && zone < m_firstZone+m_numZones, ("Invalid range."));
	if (zone<m_firstZone || zone >= m_firstZone+m_numZones) {
		return m_firstZone;
	}
	zone -= m_firstZone;
	if (crusher) {
		zone = m_crusherZones[zone];
		DEBUG_ASSERTCRASH(zone >=m_firstZone && zone < m_firstZone+m_numZones, ("Invalid range."));
		zone -= m_firstZone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_CLIFF)) {
		// Locomotors can go on ground & cliff, so use the ground cliff combiner.
		zone = m_groundCliffZones[zone];
		DEBUG_ASSERTCRASH(zone >=m_firstZone && zone < m_firstZone+m_numZones, ("Invalid range."));
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER)) {
		// Locomotors can go on ground & water, so use the ground water combiner.
		zone = m_groundWaterZones[zone];
		DEBUG_ASSERTCRASH(zone >=m_firstZone && zone < m_firstZone+m_numZones, ("Invalid range."));
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_RUBBLE)) {
		// Locomotors can go on ground & rubble, so use the ground rubble combiner.
		zone = m_groundRubbleZones[zone];
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_CLIFF) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER)) {
		// Locomotors can go on ground & cliff, so use the ground cliff combiner.
		DEBUG_CRASH(("Cliff water only locomotor sets not supported yet."));
	}

	return zone+m_firstZone;
}


/* Allocate zone equivalency arrays large enough to hold m_maxZone entries.  If the arrays are already
large enough, just return. */
void ZoneBlock::allocateZones()
{
	if (m_zonesAllocated>m_numZones && m_groundCliffZones!=nullptr) {
		return;
	}
	freeZones();

	if (m_numZones==1) {
		return; // we don't need any zone equivalency tables.
	}

	if (m_zonesAllocated == 0) {
		m_zonesAllocated = 4;
	}
	while (m_zonesAllocated <= m_numZones) {
		m_zonesAllocated *= 2;
	}
	// pool[]ify
	m_groundCliffZones = MSGNEW("PathfindZoneInfo") zoneStorageType [m_zonesAllocated];
	m_groundWaterZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_groundRubbleZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_crusherZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
}


//------------------------  PathfindZoneManager  -------------------------------
PathfindZoneManager::PathfindZoneManager() : m_maxZone(0),
m_nextFrameToCalculateZones(0),
m_groundCliffZones(nullptr),
m_groundWaterZones(nullptr),
m_groundRubbleZones(nullptr),
m_terrainZones(nullptr),
m_crusherZones(nullptr),
m_hierarchicalZones(nullptr),
m_blockOfZoneBlocks(nullptr),
m_zoneBlocks(nullptr),
m_passableBlocks(nullptr),
m_zonesAllocated(0)
{
	m_zoneBlockExtent.x = 0;
	m_zoneBlockExtent.y = 0;
}

PathfindZoneManager::~PathfindZoneManager()
{
	freeZones();
	freeBlocks();
}

void PathfindZoneManager::freeZones()
{
	delete [] m_groundCliffZones;
	m_groundCliffZones = nullptr;

	delete [] m_groundWaterZones;
	m_groundWaterZones = nullptr;

	delete [] m_groundRubbleZones;
	m_groundRubbleZones = nullptr;

	delete [] m_terrainZones;
	m_terrainZones = nullptr;

	delete [] m_crusherZones;
	m_crusherZones = nullptr;

	delete [] m_hierarchicalZones;
	m_hierarchicalZones = nullptr;

	m_zonesAllocated = 0;
}

void PathfindZoneManager::freeBlocks()
{
	delete [] m_blockOfZoneBlocks;
	m_blockOfZoneBlocks = nullptr;

	delete [] m_zoneBlocks;
	m_zoneBlocks = nullptr;

	delete [] m_passableBlocks;
	m_passableBlocks = nullptr;

	m_zoneBlockExtent.x = 0;
	m_zoneBlockExtent.y = 0;
}

/* Allocate zone equivalency arrays large enough to hold m_maxZone entries.  If the arrays are already
large enough, just return. */
void PathfindZoneManager::allocateZones()
{
	if (m_zonesAllocated>m_maxZone && m_groundCliffZones!=nullptr) {
		return;
	}
	freeZones();

	if (m_zonesAllocated == 0) {
		m_zonesAllocated = INITIAL_ZONES;
	}
	while (m_zonesAllocated <= m_maxZone) {
		m_zonesAllocated *= 2;
	}
	DEBUG_LOG(("Allocating zone tables of size %d", m_zonesAllocated));
	// pool[]ify
	m_groundCliffZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_groundWaterZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_groundRubbleZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_terrainZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_crusherZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
	m_hierarchicalZones = MSGNEW("PathfindZoneInfo") zoneStorageType[m_zonesAllocated];
}

/* Allocate zone blocks for hierarchical pathfinding.   */
void PathfindZoneManager::allocateBlocks(const IRegion2D &globalBounds)
{
	freeBlocks();

	m_zoneBlockExtent.x = (globalBounds.hi.x-globalBounds.lo.x+1+ZONE_BLOCK_SIZE-1)/ZONE_BLOCK_SIZE;
	m_zoneBlockExtent.y = (globalBounds.hi.y-globalBounds.lo.y+1+ZONE_BLOCK_SIZE-1)/ZONE_BLOCK_SIZE;

	m_blockOfZoneBlocks = MSGNEW("PathfindZoneBlocks") ZoneBlock[(m_zoneBlockExtent.x)*(m_zoneBlockExtent.y)];
	m_zoneBlocks = MSGNEW("PathfindZoneBlocks") ZoneBlockP[m_zoneBlockExtent.x];
	Int i;
	for (i=0; i<m_zoneBlockExtent.x; i++) {
		m_zoneBlocks[i] = &m_blockOfZoneBlocks[i*(m_zoneBlockExtent.y)];
	}
	// GeneralsX: A fresh block starts passable, as the old ZoneBlock ctor set.
	m_passableBlocks = MSGNEW("PathfindZoneBlocks") Bool[(m_zoneBlockExtent.x)*(m_zoneBlockExtent.y)];
	setAllPassable();
}

void PathfindZoneManager::reset()  ///< Called when the map is reset.
{
	freeZones();
	freeBlocks();
}


void PathfindZoneManager::markZonesDirty()  ///< Called when the zones need to be recalculated.
{
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
	m_nextFrameToCalculateZones = TheGameLogic->getFrame();
#else
	if (TheGameLogic->getFrame()<2) {
		m_nextFrameToCalculateZones = 2;
		return;
	}
	// GeneralsX: 300 retail frames (10 s): the longest a wall/structure change waits before
	// connectivity zones are recomputed. GX_FRAMES30, or reachability answers change 5 s sooner than retail.
	m_nextFrameToCalculateZones = MIN( m_nextFrameToCalculateZones, TheGameLogic->getFrame() + GX_FRAMES30(ZONE_UPDATE_FREQUENCY) );
#endif
}

/**
 * Calculate zones.  A zone is an area of the same terrain - clear, water or cliff.
 * The utility of zones is that if current location and destination are in the same zone,
 * you can successfully pathfind.
 * If you are a multiple terrain vehicle, like amphibious transport, the lookup is a little more
 * complicated.
 */
void PathfindZoneManager::calculateZones( PathfindCell **map, PathfindLayer layers[], const IRegion2D &globalBounds )
{
#ifdef DEBUG_QPF
#if defined(DEBUG_LOGGING)
	__int64 startTime64;
	static double timeToUpdate = 0.0f;
	static double averageTimeToUpdate = 0.0f;
	static Int updateSamples = 0;
	__int64 endTime64,freq64;
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);
#endif
#endif

	m_maxZone = 1;	// we start using zone 0 as a flag.
	// GeneralsX: 24,000 -> 65,000: the zone field is 16 bits now (AIPathfind.h m_zone, m_maxZone and
	// every zone table are UnsignedShort), and a 32-start map measured 20,262 zones before any building, so 32 bases would pass
	// 24,000 and merge every later zone into one id (wrong paths). 65,000 + the bridge layers' ids (<= LAYER_LAST) stays under
	// 65,535. A map under 24,000 takes the same path with the same ids. collapsedZones moved to the heap (below).
	const Int maxZones=65000;
	// GeneralsX: Exact relabel structure in place of the scanned array.
	ZoneRelabel zoneEquivalency;
	zoneEquivalency.initIdentity(maxZones);
	Int i, j;
	for (i=0; i<=LAYER_LAST; i++) {
		layers[i].setZone(0);
	}

	Int xCount = (globalBounds.hi.x-globalBounds.lo.x+1+ZONE_BLOCK_SIZE-1)/ZONE_BLOCK_SIZE;
	Int yCount = (globalBounds.hi.y-globalBounds.lo.y+1+ZONE_BLOCK_SIZE-1)/ZONE_BLOCK_SIZE;

	Int xBlock, yBlock;
	for (xBlock = 0; xBlock<xCount; xBlock++) {
		for (yBlock=0; yBlock<yCount; yBlock++) {
			IRegion2D bounds;
			bounds.lo.x = globalBounds.lo.x + xBlock*ZONE_BLOCK_SIZE;
			bounds.lo.y = globalBounds.lo.y + yBlock*ZONE_BLOCK_SIZE;
			bounds.hi.x = bounds.lo.x + ZONE_BLOCK_SIZE - 1; // bounds are inclusive.
			bounds.hi.y = bounds.lo.y + ZONE_BLOCK_SIZE - 1; // bounds are inclusive.
			bounds.hi.updateMin(globalBounds.hi);
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
			if (bounds.lo.x>bounds.hi.x || bounds.lo.y>bounds.hi.y) {
				DEBUG_CRASH(("Incorrect bounds calculation. Logic error, fix me. jba."));
				continue;
			}
#endif
			m_zoneBlocks[xBlock][yBlock].setInteractsWithBridge(false);
			for( j=bounds.lo.y; j<=bounds.hi.y; j++ )	{
				for( i=bounds.lo.x; i<=bounds.hi.x; i++ )	{
					PathfindCell *cell = &map[i][j];
					cell->setZone(0);

					if (i>bounds.lo.x) {
						if (map[i][j].getType() == map[i-1][j].getType()) {
							applyZone(map[i][j], map[i-1][j], zoneEquivalency);
						}
					}
					if (j>bounds.lo.y) {
						if (map[i][j].getType() == map[i][j-1].getType()) {
							applyZone(map[i][j], map[i][j-1], zoneEquivalency);
						}
					}
					if (cell->getZone()==0) {
						// GeneralsX: zoneEquivalency and collapsedZones are stack
						// arrays of maxZones, and resolveZones/the collapse loop index them up to m_maxZone.
						// Past maxZones that is a stack overrun whose effect depends on each compiler's frame
						// layout. Stop allocating there. Unreachable while m_maxZone < maxZones, so a map
						// that fits is bit-identical; only maps that overran before are changed.
						if (m_maxZone < maxZones) {
							cell->setZone(m_maxZone);
							m_maxZone++;
						} else {
							cell->setZone(maxZones-1);
						}
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
						if (m_maxZone>= maxZones) {
							DEBUG_CRASH(("Ran out of pathfind zones.  SERIOUS ERROR! jba."));
							break;
						}
#endif
					}
					if (cell->getConnectLayer() > LAYER_GROUND) {
 						m_zoneBlocks[xBlock][yBlock].setInteractsWithBridge(true);
					}

				}
			}
 		}
	}

	Int totalZones = m_maxZone;
	// GeneralsX: [GXZONES]: the raw zone count against the labelling cap, each time it sets a new
	// high for this process (a 32-start map runs close to it; past maxZones zones are merged into the last id).
	{
		static Int s_gxZonesHigh = 0;
		if (totalZones > s_gxZonesHigh)
		{
			s_gxZonesHigh = totalZones;
			fprintf(stderr, "[GXZONES] raw=%d cap=%d field_max=%d\n", totalZones, maxZones, 65535);
		}
	}

	// Collapse the zones into a 1,2,3... sequence, removing collapsed zones.
	m_maxZone = 1;
	std::vector<Int> collapsedZones((size_t)maxZones);	// GeneralsX 06/11/2026 heap: 65,000 Ints is 254 KB, too much for a stack
	collapsedZones[0] = 0;
	for (i=1; i<totalZones; i++) {
		Int zone = zoneEquivalency.get(i);
		if (zone == i) {
			collapsedZones[i] = m_maxZone;
			++m_maxZone;
		} else {
			collapsedZones[i] = collapsedZones[zone];
		}
	}

	// Now map the zones in the map back into the collapsed zones.
	for( j=globalBounds.lo.y; j<=globalBounds.hi.y; j++ ) {
		for( i=globalBounds.lo.x; i<=globalBounds.hi.x; i++ ) {
			PathfindCell &cell = map[i][j];
			cell.setZone(collapsedZones[cell.getZone()]);
		}
	}
	
	for (i=0; i<=LAYER_LAST; i++) {
		PathfindLayer &r_thisLayer = layers[i];

		Int zone = collapsedZones[r_thisLayer.getZone()];
		if (zone == 0) {
			zone = m_maxZone;
			m_maxZone++;
		}

		r_thisLayer.setZone( zone );
		r_thisLayer.applyZone();

		if (!r_thisLayer.isUnused() && !r_thisLayer.isDestroyed()) {
			ICoord2D ndx;
			r_thisLayer.getStartCellIndex(&ndx);
			setBridge(ndx.x, ndx.y, true);
			r_thisLayer.getEndCellIndex(&ndx);
			setBridge(ndx.x, ndx.y, true);
		}
	}

	allocateZones();

	for (xBlock=0; xBlock<xCount; xBlock++) {
		for (yBlock=0; yBlock<yCount; yBlock++) {
			IRegion2D bounds;
			bounds.lo.x = globalBounds.lo.x + xBlock*ZONE_BLOCK_SIZE;
			bounds.lo.y = globalBounds.lo.y + yBlock*ZONE_BLOCK_SIZE;
			bounds.hi.x = bounds.lo.x + ZONE_BLOCK_SIZE - 1; // bounds are inclusive.
			bounds.hi.y = bounds.lo.y + ZONE_BLOCK_SIZE - 1; // bounds are inclusive.

			bounds.hi.updateMin(globalBounds.hi);
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
			if (bounds.lo.x>bounds.hi.x || bounds.lo.y>bounds.hi.y) {
				DEBUG_CRASH(("Incorrect bounds calculation. Logic error, fix me. jba."));
				continue;
			}
#endif
			m_zoneBlocks[xBlock][yBlock].blockCalculateZones(map, layers, bounds);
		}
	}

	// Determine water/ground equivalent zones, and ground/cliff equivalent zones.
	for (i=0; i<m_zonesAllocated; i++) {
		m_groundCliffZones[i] = i;
		m_groundWaterZones[i] = i;
		m_groundRubbleZones[i] = i;
		m_terrainZones[i] = i;
		m_crusherZones[i] = i;
		m_hierarchicalZones[i] = i;
	}
	// GeneralsX: Merge through exact relabel structures, then write the
	// tables once (entries >= m_maxZone are never relabelled and keep the identity set above).
	ZoneRelabel groundCliffRelabel, groundWaterRelabel, groundRubbleRelabel, terrainRelabel, crusherRelabel, hierarchicalRelabel;
	groundCliffRelabel.initIdentity(m_maxZone);
	groundWaterRelabel.initIdentity(m_maxZone);
	groundRubbleRelabel.initIdentity(m_maxZone);
	terrainRelabel.initIdentity(m_maxZone);
	crusherRelabel.initIdentity(m_maxZone);
	hierarchicalRelabel.initIdentity(m_maxZone);

	for( j=globalBounds.lo.y; j<=globalBounds.hi.y; j++ ) {
		for( i=globalBounds.lo.x; i<=globalBounds.hi.x; i++ ) {
			PathfindCell &r_thisCell = map[i][j];

			if ( (r_thisCell.getConnectLayer() > LAYER_GROUND) &&
				(r_thisCell.getType() == PathfindCell::CELL_CLEAR) ) {
				PathfindLayer *layer = layers + r_thisCell.getConnectLayer();
				resolveZones(r_thisCell.getZone(), layer->getZone(), hierarchicalRelabel);
			}

			if ( i > globalBounds.lo.x && r_thisCell.getZone() != map[i-1][j].getZone() ) {
				const PathfindCell &r_leftCell = map[i-1][j];

#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				if (r_thisCell.getType() == r_leftCell.getType()) {
					applyZone(r_thisCell, r_leftCell, hierarchicalRelabel);
				}
				if (waterGround(r_thisCell, r_leftCell)) {
					applyZone(r_thisCell, r_leftCell, groundWaterRelabel);
				}
				if (groundRubble(r_thisCell, r_leftCell)) {
					applyZone(r_thisCell, r_leftCell, groundRubbleRelabel);
				}
				if (groundCliff(r_thisCell, r_leftCell)) {
					applyZone(r_thisCell, r_leftCell, groundCliffRelabel);
				}
				if (terrain(r_thisCell, r_leftCell)) {
					applyZone(r_thisCell, r_leftCell, terrainRelabel);
				}
				if (crusherGround(r_thisCell, r_leftCell)) {
					applyZone(r_thisCell, r_leftCell, crusherRelabel);
				}
#else
				//if this is true, skip all the ones below
				if (r_thisCell.getType() == r_leftCell.getType())
					applyZone(r_thisCell, r_leftCell, hierarchicalRelabel);
				else {
					Bool notTerrainOrCrusher = TRUE; // if this is false, skip the if-else-ladder below

					if (terrain(r_thisCell, r_leftCell)) {
						applyZone(r_thisCell, r_leftCell, terrainRelabel);
						notTerrainOrCrusher = FALSE;
					}

					if (crusherGround(r_thisCell, r_leftCell)) {
						applyZone(r_thisCell, r_leftCell, crusherRelabel);
						notTerrainOrCrusher = FALSE;
					}

					if ( notTerrainOrCrusher ) {
						if (waterGround(r_thisCell, r_leftCell))
							applyZone(r_thisCell, r_leftCell, groundWaterRelabel);
						else if (groundRubble(r_thisCell, r_leftCell))
							applyZone(r_thisCell, r_leftCell, groundRubbleRelabel);
						else if (groundCliff(r_thisCell, r_leftCell))
							applyZone(r_thisCell, r_leftCell, groundCliffRelabel);
					}

				}
#endif

			}

			if (j>globalBounds.lo.y && r_thisCell.getZone()!=map[i][j-1].getZone()) {
				const PathfindCell &r_topCell = map[i][j-1];

#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				if (r_thisCell.getType() == r_topCell.getType()) {
					applyZone(r_thisCell, r_topCell, hierarchicalRelabel);
				}
				if (waterGround(r_thisCell, r_topCell)) {
					applyZone(r_thisCell, r_topCell, groundWaterRelabel);
				}
				if (groundRubble(r_thisCell, r_topCell)) {
					applyZone(r_thisCell, r_topCell, groundRubbleRelabel);
				}
				if (groundCliff(r_thisCell, r_topCell)) {
					applyZone(r_thisCell, r_topCell, groundCliffRelabel);
				}
				if (terrain(r_thisCell, r_topCell)) {
					applyZone(r_thisCell, r_topCell, terrainRelabel);
				}
				if (crusherGround(r_thisCell, r_topCell)) {
					applyZone(r_thisCell, r_topCell, crusherRelabel);
				}
#else
				//if this is true, skip all the ones below
				if (r_thisCell.getType() == r_topCell.getType())
					applyZone(r_thisCell, r_topCell, hierarchicalRelabel);
				else {
					Bool notTerrainOrCrusher = TRUE; // if this is false, skip the if-else-ladder below

					if (terrain(r_thisCell, r_topCell)) {
						applyZone(r_thisCell, r_topCell, terrainRelabel);
						notTerrainOrCrusher = FALSE;
					}

					if (crusherGround(r_thisCell, r_topCell)) {
						applyZone(r_thisCell, r_topCell, crusherRelabel);
						notTerrainOrCrusher = FALSE;
					}

					if (notTerrainOrCrusher) {
						if (waterGround(r_thisCell, r_topCell))
							applyZone(r_thisCell, r_topCell, groundWaterRelabel);
						else if (groundRubble(r_thisCell, r_topCell))
							applyZone(r_thisCell, r_topCell, groundRubbleRelabel);
						else if (groundCliff(r_thisCell, r_topCell))
							applyZone(r_thisCell, r_topCell, groundCliffRelabel);
					}

				}
#endif

			}

		}
	}

	// GeneralsX: Write the merged tables once, before anything reads them.
	groundCliffRelabel.materialize(m_groundCliffZones, m_maxZone);
	groundWaterRelabel.materialize(m_groundWaterZones, m_maxZone);
	groundRubbleRelabel.materialize(m_groundRubbleZones, m_maxZone);
	terrainRelabel.materialize(m_terrainZones, m_maxZone);
	crusherRelabel.materialize(m_crusherZones, m_maxZone);
	hierarchicalRelabel.materialize(m_hierarchicalZones, m_maxZone);

	//FLATTEN HIERARCHICAL ZONES
	for (i=1; i<m_maxZone; i++) {
		Int zone = m_hierarchicalZones[i];
		m_hierarchicalZones[i] = m_hierarchicalZones[zone];
	}

	//THIS BLOCK IS 20%
	flattenZones(m_groundCliffZones, m_hierarchicalZones, m_maxZone);
	flattenZones(m_groundWaterZones, m_hierarchicalZones, m_maxZone);
	flattenZones(m_groundRubbleZones, m_hierarchicalZones, m_maxZone);
	flattenZones(m_terrainZones, m_hierarchicalZones, m_maxZone);
	flattenZones(m_crusherZones, m_hierarchicalZones, m_maxZone);

#ifdef DEBUG_QPF
#if defined(DEBUG_LOGGING)
	QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
	timeToUpdate = ((double)(endTime64-startTime64) / (double)(freq64));

	if ( updateSamples < 400 ) {
		averageTimeToUpdate = ((averageTimeToUpdate * updateSamples) + timeToUpdate) / (updateSamples + 1.0f);
		updateSamples++;
		DEBUG_LOG(("computing...: %f", averageTimeToUpdate));
	}
	else if ( updateSamples == 400 ) {
		DEBUG_LOG((" =============DONE============= Average time to calculate zones: %f", averageTimeToUpdate));
		DEBUG_LOG(("                                           Percent of baseline : %f", averageTimeToUpdate/0.003335f));
		updateSamples = 777;
	}

#endif
#endif
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI == AI_DEBUG_ZONES)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
		RGBColor color;
		memset(&color, 0, sizeof(Color));
		addIcon(nullptr, 0, 0, color);
		for( j=0; j<globalBounds.hi.y; j++ )	{
			for( i=0; i<globalBounds.hi.x; i++ )	{
				Int zone = map[i][j].getZone();
				zone = m_hierarchicalZones[zone];

				color.blue = (zone%3) * 0.5f;
				zone = zone/3;
				color.green = (zone%3) * 0.5f;
				zone = zone/3;
				color.red = (zone%3) * 0.5;
				Coord3D pos;
				pos.x = ((Real)i + 0.5f) * PATHFIND_CELL_SIZE_F;
				pos.y = ((Real)j + 0.5f) * PATHFIND_CELL_SIZE_F;
				pos.z = TheTerrainLogic->getLayerHeight( pos.x, pos.y, map[i][j].getLayer() ) + 0.5f;
				addIcon(&pos, PATHFIND_CELL_SIZE_F*0.8f, 500, color);
			}
		}
	}
#endif
	m_nextFrameToCalculateZones = 0xffffffff;
}

/**
 * Update zones where a structure has been added or removed.
 * This can be done by just updating the equivalency arrays, without rezoning the map..
 */
void PathfindZoneManager::updateZonesForModify(PathfindCell **map, PathfindLayer layers[], const IRegion2D &structureBounds, const IRegion2D &globalBounds )
{

#ifdef DEBUG_QPF
#if defined(DEBUG_LOGGING)
	__int64 startTime64;
	double timeToUpdate=0.0f;
	__int64 endTime64,freq64;
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);
#endif
#endif
	IRegion2D bounds = structureBounds;
	bounds.hi.x++;
	bounds.hi.y++;
	bounds.hi.updateMin(globalBounds.hi);

	Int xBlock, yBlock;
	for (xBlock = 0; xBlock<m_zoneBlockExtent.x; xBlock++) {
		for (yBlock=0; yBlock<m_zoneBlockExtent.y; yBlock++) {
			IRegion2D blockBounds;
			blockBounds.lo.x = globalBounds.lo.x + xBlock*ZONE_BLOCK_SIZE;
			blockBounds.lo.y = globalBounds.lo.y + yBlock*ZONE_BLOCK_SIZE;
			blockBounds.hi.x = blockBounds.lo.x + ZONE_BLOCK_SIZE - 1; // blockBounds are inclusive.
			blockBounds.hi.y = blockBounds.lo.y + ZONE_BLOCK_SIZE - 1; // blockBounds are inclusive.
			blockBounds.intersectWith(bounds);
			if (blockBounds.lo.x>blockBounds.hi.x || blockBounds.lo.y>blockBounds.hi.y) {
				continue;
			}
			m_zoneBlocks[xBlock][yBlock].setInteractsWithBridge(false);
			Int i, j;
			for( j=blockBounds.lo.y; j<=blockBounds.hi.y; j++ )	{
				for( i=blockBounds.lo.x; i<=blockBounds.hi.x; i++ )	{
					PathfindCell *cell = &map[i][j];
					if (cell->getZone()!=UNINITIALIZED_ZONE) continue;

					if (i>blockBounds.lo.x) {
						if (map[i][j].getType() == map[i-1][j].getType()) {
							cell->setZone(map[i-1][j].getZone());
							if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
						}
					}
					if (j>blockBounds.lo.y) {
						if (cell->getType() == map[i][j-1].getType()) {
							cell->setZone(map[i][j-1].getZone());
							if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
						}
						if (i<blockBounds.hi.x) {
							if (typesMatch(*cell, map[i+1][j-1]) &&
									typesMatch(*cell, map[i+1][j])) {
								cell->setZone(map[i+1][j-1].getZone());
								if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
							}
						}
					}
				}
			}
			for( j=blockBounds.hi.y; j>=blockBounds.lo.y; j-- )	{
				for( i=blockBounds.hi.x; i>=blockBounds.lo.x; i-- )	{
					PathfindCell *cell = &map[i][j];
					if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
					if (i<blockBounds.hi.x) {
						if (map[i][j].getType() == map[i+1][j].getType()) {
							cell->setZone(map[i+1][j].getZone());
							if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
						}
					}
					if (j<blockBounds.hi.y) {
						if (cell->getType() == map[i][j+1].getType()) {
							cell->setZone(map[i][j+1].getZone());
							if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
						}
						if (i<blockBounds.hi.x) {
							if (typesMatch(*cell, map[i+1][j+1]) &&
									typesMatch(*cell, map[i+1][j])) {
								cell->setZone(map[i+1][j+1].getZone());
								if (cell->getZone()!=UNINITIALIZED_ZONE) continue;
							}
						}
					}
				}
			}
		}
	}
#ifdef DEBUG_QPF
#if defined(DEBUG_LOGGING)
	QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
	timeToUpdate = ((double)(endTime64-startTime64) / (double)(freq64));
#endif
#endif
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI==AI_DEBUG_ZONES)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
		RGBColor color;
		memset(&color, 0, sizeof(Color));
		addIcon(nullptr, 0, 0, color);
		Int i, j;
		for( j=0; j<globalBounds.hi.y; j++ )	{
			for( i=0; i<globalBounds.hi.x; i++ )	{
				Int zone = map[i][j].getZone();
				//zone = m_terrainZones[zone];
				zone = m_hierarchicalZones[zone];

				color.blue = (zone%3) * 0.5f;
				zone = zone/3;
				color.green = (zone%3) * 0.5f;
				zone = zone/3;
				color.red = (zone%3) * 0.5;
				Coord3D pos;
				pos.x = ((Real)i + 0.5f) * PATHFIND_CELL_SIZE_F;
				pos.y = ((Real)j + 0.5f) * PATHFIND_CELL_SIZE_F;
				pos.z = TheTerrainLogic->getLayerHeight( pos.x, pos.y, map[i][j].getLayer() ) + 0.5f;
				addIcon(&pos, PATHFIND_CELL_SIZE_F*0.8f, 200, color);
			}
		}
	}
#endif

}

//
// Clear the passable flags.
//
// GeneralsX: One dense fill instead of a 56-byte-stride walk over every ZoneBlock.
void PathfindZoneManager::clearPassableFlags()
{
	if (m_passableBlocks) {
		std::fill(m_passableBlocks, m_passableBlocks + m_zoneBlockExtent.x*m_zoneBlockExtent.y, false);
	}
}

//
// Set the passable flags.
//
// GeneralsX: One dense fill instead of a 56-byte-stride walk over every ZoneBlock.
void PathfindZoneManager::setAllPassable()
{
	if (m_passableBlocks) {
		std::fill(m_passableBlocks, m_passableBlocks + m_zoneBlockExtent.x*m_zoneBlockExtent.y, true);
	}
}

//
// Set the passable flag for the block at this location.
//
void PathfindZoneManager::setPassable(Int cellX, Int cellY, Bool passable)
{
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		DEBUG_CRASH(("Invalid block."));
		return;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		DEBUG_CRASH(("Invalid block."));
		return;
	}
	m_passableBlocks[blockX*m_zoneBlockExtent.y+blockY] = passable;
}

//
// Get the passable flag for the block at this location.
//
Bool PathfindZoneManager::isPassable(Int cellX, Int cellY) const
{
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		DEBUG_CRASH(("Invalid block."));
		return false;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		DEBUG_CRASH(("Invalid block."));
		return false;
	}
	return m_passableBlocks[blockX*m_zoneBlockExtent.y+blockY];
}

//
// Get the passable flag for the block at this location.
//
Bool PathfindZoneManager::clipIsPassable(Int cellX, Int cellY) const
{
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		return false;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		return false;
	}
	return m_passableBlocks[blockX*m_zoneBlockExtent.y+blockY];
}

//
// Set the bridge flag for the block at this location.
//
void PathfindZoneManager::setBridge(Int cellX, Int cellY, Bool bridge)
{
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		// DEBUG_CRASH(("Invalid block."));  Bridges can be off the playable grid, so don't crash. jba.
		return;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		// DEBUG_CRASH(("Invalid block."));  Bridges can be off the playable grid, so don't crash. jba.
		return;
	}
	m_zoneBlocks[blockX][blockY].setInteractsWithBridge(bridge);
}


//
// Set the bridge flag for the block at this location.
//
Bool PathfindZoneManager::interactsWithBridge(Int cellX, Int cellY) const
{
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		DEBUG_CRASH(("Invalid block."));
		return false;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		DEBUG_CRASH(("Invalid block."));
		return false;
	}
	return m_zoneBlocks[blockX][blockY].getInteractsWithBridge();
}


//
// Return the zone at this location.
//
zoneStorageType PathfindZoneManager::getBlockZone(LocomotorSurfaceTypeMask acceptableSurfaces, Bool crusher,Int cellX, Int cellY, PathfindCell **map) const
{
	PathfindCell *cell = &(map[cellX][cellY]);
	Int blockX = cellX/ZONE_BLOCK_SIZE;
	Int blockY = cellY/ZONE_BLOCK_SIZE;

	if (blockX<0 || blockX>=m_zoneBlockExtent.x) {
		DEBUG_CRASH(("Invalid block."));
		return 0;
	}
	if (blockY<0 || blockY>=m_zoneBlockExtent.y) {
		DEBUG_CRASH(("Invalid block."));
		return 0;
	}
	zoneStorageType zone =  m_zoneBlocks[blockX][blockY].getEffectiveZone(acceptableSurfaces, crusher, cell->getZone());
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
	if (zone > m_maxZone) {
#else
	if (zone >= m_maxZone) {
#endif
		DEBUG_CRASH(("Invalid zone."));
		return UNINITIALIZED_ZONE;
	}
	return zone;
}

//
// Return the zone at this location.
//
zoneStorageType PathfindZoneManager::getEffectiveTerrainZone(zoneStorageType zone) const
{
	return m_hierarchicalZones[m_terrainZones[zone]];
}

//
// Return the zone at this location.
//
zoneStorageType PathfindZoneManager::getEffectiveZone( LocomotorSurfaceTypeMask acceptableSurfaces,
																										Bool crusher, zoneStorageType zone) const
{
	//DEBUG_ASSERTCRASH(zone, ("Zone not set"));
	if (zone>m_maxZone) {
		DEBUG_CRASH(("Invalid zone"));
		return (0);
	}
	if (zone>m_maxZone) {
		DEBUG_CRASH(("Invalid zone"));
		return (0);
	}
	if (acceptableSurfaces&LOCOMOTORSURFACE_AIR) return 1; // air is all zone 1.

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_CLIFF)) {
		// Locomotors can go on ground, water & cliff, so all is zone 1.
		return 1;
	}

	if (crusher) {
		zone = m_crusherZones[zone];
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_CLIFF)) {
		// Locomotors can go on ground & cliff, so use the ground cliff combiner.
		zone = m_groundCliffZones[zone];
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER)) {
		// Locomotors can go on ground & water, so use the ground water combiner.
		zone = m_groundWaterZones[zone];
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_GROUND) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_RUBBLE)) {
		// Locomotors can go on ground & rubble, so use the ground rubble combiner.
		zone = m_groundRubbleZones[zone];
		return zone;
	}

	if ( (acceptableSurfaces&LOCOMOTORSURFACE_CLIFF) &&
			(acceptableSurfaces&LOCOMOTORSURFACE_WATER)) {
		// Locomotors can go on ground & cliff, so use the ground cliff combiner.
		DEBUG_CRASH(("Cliff water only locomotor sets not supported yet."));
	}
	zone = m_hierarchicalZones[zone];

	return zone;
}
//-------------------- PathfindLayer ----------------------------------------
PathfindLayer::PathfindLayer() : m_blockOfMapCells(nullptr), m_layerCells(nullptr), m_bridge(nullptr),
m_destroyed(FALSE),
m_height(0),
m_width(0),
m_xOrigin(0),
m_yOrigin(0),
m_zone(0)
{
	m_startCell.x = -1;
	m_startCell.y = -1;
	m_endCell.x = -1;
	m_endCell.y = -1;
}

PathfindLayer::~PathfindLayer()
{
	reset();
}

/**
 * Returns true if the layer is available for use.
 */
void PathfindLayer::reset()
{
	m_bridge = nullptr;
	if (m_layerCells) {
		Int i, j;
		for (i=0; i<m_width; i++) {
			for (j=0; j<m_height; j++) {
				PathfindCell *cell = &m_layerCells[i][j];
				cell->reset();
			}
		}
		delete [] m_layerCells;
		m_layerCells = nullptr;
	}

	delete [] m_blockOfMapCells;
	m_blockOfMapCells = nullptr;

	m_width = 0;
	m_height = 0;
	m_xOrigin = 0;
	m_yOrigin = 0;
	m_startCell.x = -1;
	m_startCell.y = -1;
	m_endCell.x = -1;
	m_endCell.y = -1;
	m_layer = LAYER_GROUND;
}

/**
 * Returns true if the layer is available for use.
 */
Bool PathfindLayer::isUnused()
{
	// Special case - wall layer is built from not a bridge.  jba.
	if (m_layer == LAYER_WALL && m_width>0) return false;

	if (m_bridge==nullptr) return true;
	return false;
}



/**
 * Draws debug cell info.
 */
#if defined(RTS_DEBUG)
void PathfindLayer::doDebugIcons() {
	if (isUnused()) return;
	extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
	// render AI debug information
	{
		Coord3D topLeftCorner;
		RGBColor color;
		color.red = color.green = color.blue = 0;
		Coord3D center;
		center.x = (m_xOrigin+m_width/2)*PATHFIND_CELL_SIZE_F;
		center.y = (m_yOrigin+m_height/2)*PATHFIND_CELL_SIZE_F;
		center.z = 0;
		Real bridgeHeight = TheTerrainLogic->getLayerHeight(center.x , center.y, m_layer);
		if (m_layer == LAYER_WALL) {
			bridgeHeight = TheAI->pathfinder()->getWallHeight();
		}
		static Int flash = 0;
		flash--;
		if (flash<1) flash = 20;
		if (flash < 10) return;
		Bool showCells = TheGlobalData->m_debugAI==AI_DEBUG_CELLS;
		// show the pathfind grid
		for( int j=0; j<m_height; j++ )
		{
			topLeftCorner.y = (Real)(j+m_yOrigin) * PATHFIND_CELL_SIZE_F;

			for( int i=0; i<m_width; i++ )
			{
				topLeftCorner.x = (Real)(i+m_xOrigin) * PATHFIND_CELL_SIZE_F;

				color.red = color.green = color.blue = 0;
				Bool empty = false;
				Real size = 0.4f;
				const PathfindCell *cell = &m_layerCells[i][j];
				if (cell)
				{
					if (cell->getConnectLayer()==LAYER_GROUND) {
							color.green = 1;
							color.blue = 1;
							empty = false;
					}	else if (cell->getType() == PathfindCell::CELL_IMPASSABLE) {
							color.red = color.green = color.blue = 1;
							size = 0.2f;
							empty = false;
					}	else if (cell->getType() == PathfindCell::CELL_BRIDGE_IMPASSABLE) {
							color.blue = color.red = 1;
							empty = false;
					}	else if (cell->getType() == PathfindCell::CELL_CLIFF) {
							color.red = 1;
							empty = false;
					}	else {
							size = 0.2f;
					}
				}
				if (showCells) {
					empty = true;
					color.red = color.green = color.blue = 0;
					if (empty && cell) {
						if (cell->getFlags()!=PathfindCell::NO_UNITS) {
							empty = false;
							if (cell->getFlags() == PathfindCell::UNIT_GOAL) {
								color.red = 1;
							}	else if (cell->getFlags() == PathfindCell::UNIT_PRESENT_FIXED) {
								color.green = color.blue = color.red = 1;
							}	else if (cell->getFlags() == PathfindCell::UNIT_PRESENT_MOVING) {
								color.green = 1;
							}	else {
								color.green = color.red = 1;
							}
						}
					}
				}
				if (!empty) {
					Coord3D loc;
					loc.x = topLeftCorner.x + PATHFIND_CELL_SIZE_F/2.0f;
					loc.y = topLeftCorner.y + PATHFIND_CELL_SIZE_F/2.0f;
					loc.z = bridgeHeight;
					addIcon(&loc, PATHFIND_CELL_SIZE_F*size, 99, color);
				}
			}
		}

	}
}
#endif

/**
 * Sets the bridge & layer number for a layer.
 */
Bool PathfindLayer::init(Bridge *theBridge, PathfindLayerEnum layer)
{
	if (m_bridge!=nullptr) return false;
	m_bridge = theBridge;
	m_layer = layer;
	m_destroyed = false;
	return true;
}

/**
 * Allocates the pathfind cells for the bridge layer.
 */
void PathfindLayer::allocateCells(const IRegion2D *extent)
{
	if (m_bridge == nullptr) return;
	Region2D bridgeBounds = *m_bridge->getBounds();
	Int maxX, maxY;
	m_xOrigin = REAL_TO_INT_FLOOR((bridgeBounds.lo.x-PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	m_yOrigin = REAL_TO_INT_FLOOR((bridgeBounds.lo.y-PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	m_width = 0;
	m_height = 0;
	maxX = REAL_TO_INT_CEIL((bridgeBounds.hi.x+PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	maxY = REAL_TO_INT_CEIL((bridgeBounds.hi.y+PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	// Pad with 1 extra;
	m_xOrigin--;
	m_yOrigin--;
	maxX++;
	maxY++;

	if (m_xOrigin < extent->lo.x) m_xOrigin = extent->lo.x;
	if (m_yOrigin < extent->lo.y) m_yOrigin = extent->lo.y;
	if (maxX > extent->hi.x) maxX = extent->hi.x;
	if (maxY > extent->hi.y) maxY = extent->hi.y;
	if (maxX <= m_xOrigin) return;
	if (maxY <= m_yOrigin) return;
	m_width = maxX - m_xOrigin;
	m_height = maxY - m_yOrigin;

	// Allocate cells.
	// pool[]ify
	m_blockOfMapCells = MSGNEW("PathfindMapCells") PathfindCell[m_width*m_height];
	m_layerCells = MSGNEW("PathfindMapCells") PathfindCellP[m_width];
	Int i;
	for (i=0; i<m_width; i++) {
		m_layerCells[i] = &m_blockOfMapCells[i*m_height];
	}
}

/**
 * Allocates the pathfind cells for the wall bridge layer.
 */
void PathfindLayer::allocateCellsForWallLayer(const IRegion2D *extent, ObjectID *wallPieces, Int numPieces)
{
	DEBUG_ASSERTCRASH(m_layer==LAYER_WALL, ("Wrong layer for wall."));
	if (m_layer != LAYER_WALL) return;
	Region2D bridgeBounds;

	Int i;
	Bool first = true;
	for (i=0; i<numPieces; i++) {
		Object *obj = TheGameLogic->findObjectByID(wallPieces[i]);
		Region2D objBounds;
		if (obj==nullptr) continue;
		obj->getGeometryInfo().get2DBounds(*obj->getPosition(), obj->getOrientation(), objBounds);
		if (first) {
			bridgeBounds = objBounds;
			first = false;
		} else {
			bridgeBounds.uniteWith(objBounds);
		}
	}

	Int maxX, maxY;
	m_xOrigin = REAL_TO_INT_FLOOR((bridgeBounds.lo.x-PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	m_yOrigin = REAL_TO_INT_FLOOR((bridgeBounds.lo.y-PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	m_width = 0;
	m_height = 0;
	maxX = REAL_TO_INT_CEIL((bridgeBounds.hi.x+PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	maxY = REAL_TO_INT_CEIL((bridgeBounds.hi.y+PATHFIND_CELL_SIZE/100)/PATHFIND_CELL_SIZE);
	// Pad with 1 extra;
	m_xOrigin--;
	m_yOrigin--;
	maxX++;
	maxY++;

	if (m_xOrigin < extent->lo.x) m_xOrigin = extent->lo.x;
	if (m_yOrigin < extent->lo.y) m_yOrigin = extent->lo.y;
	if (maxX > extent->hi.x) maxX = extent->hi.x;
	if (maxY > extent->hi.y) maxY = extent->hi.y;
	if (maxX <= m_xOrigin) return;
	if (maxY <= m_yOrigin) return;
	m_width = maxX - m_xOrigin;
	m_height = maxY - m_yOrigin;

	// Allocate cells.
	m_blockOfMapCells = MSGNEW("PathfindMapCells") PathfindCell[m_width*m_height];
	m_layerCells = MSGNEW("PathfindMapCells") PathfindCellP[m_width];

	for (i=0; i<m_width; i++) {
		m_layerCells[i] = &m_blockOfMapCells[i*m_height];
	}
}

/**
 * Checks to see if a broken bridge connects 2 zones.
 */
Bool PathfindLayer::connectsZones(PathfindZoneManager *zm, const LocomotorSet& locoSet,
																	Int zone1, Int zone2)
{
	if (!m_destroyed) {
		return false;
	}
	Bool found1 = false;
	Bool found2 = false;
	Int i, j;
	for (i=0; i<m_width; i++) {
		for (j=0; j<m_height; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			if (cell->getConnectLayer()==LAYER_GROUND) {
					PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND, i+m_xOrigin, j+m_yOrigin);
					DEBUG_ASSERTCRASH(groundCell, ("Should have cell."));
					if (groundCell) {
						zoneStorageType zone = zm->getEffectiveZone(locoSet.getValidSurfaces(),
							true, groundCell->getZone());
						zone = zm->getEffectiveTerrainZone(zone);
						if (zone == zone1) found1 = true;
						if (zone == zone2) found2 = true;
					}
			}
		}
	}
	return found1 && found2;
}

/**
 * Classifies the pathfind cells for the bridge layer.
 */
void PathfindLayer::classifyCells()
{
	m_startCell.x = -1;
	m_startCell.y = -1;
	m_endCell.x = -1;
	m_endCell.y = -1;
	Int i, j;
	for (i=0; i<m_width; i++) {
		for (j=0; j<m_height; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			cell->setConnectLayer(LAYER_INVALID);
			cell->setLayer(m_layer);
			classifyLayerMapCell(i+m_xOrigin, j+m_yOrigin, cell, m_bridge);
		}
		BridgeInfo info;
		m_bridge->getBridgeInfo(&info);
		Coord3D bridgeDir = info.to;
		bridgeDir.x -= info.from.x;
		bridgeDir.y -= info.from.y;
		bridgeDir.z -= info.from.z;
		bridgeDir.normalize();
		bridgeDir.x *= PATHFIND_CELL_SIZE_F*0.7f;
		bridgeDir.y *= PATHFIND_CELL_SIZE_F*0.7f;

		m_startCell.x = REAL_TO_INT_FLOOR((info.from.x-bridgeDir.x) / PATHFIND_CELL_SIZE_F);
		m_startCell.y = REAL_TO_INT_FLOOR((info.from.y-bridgeDir.y) / PATHFIND_CELL_SIZE_F);
		m_endCell.x = REAL_TO_INT_FLOOR((info.to.x+bridgeDir.x) / PATHFIND_CELL_SIZE_F);
		m_endCell.y = REAL_TO_INT_FLOOR((info.to.y+bridgeDir.y) / PATHFIND_CELL_SIZE_F);
	}
	if (m_destroyed) {
		Int i, j;
		for (i=0; i<m_width; i++) {
			for (j=0; j<m_height; j++) {
				PathfindCell *cell = &m_layerCells[i][j];
				if (cell->getConnectLayer() == LAYER_GROUND) {
					PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND, i+m_xOrigin, j+m_yOrigin);
					DEBUG_ASSERTCRASH(groundCell, ("Should have cell."));
					if (groundCell) {
						DEBUG_ASSERTCRASH(groundCell->getConnectLayer()==m_layer, ("Should connect to this layer.jba."));
						groundCell->setConnectLayer(LAYER_INVALID); // disconnect it.
					}
				}
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				cell->setType(PathfindCell::CELL_IMPASSABLE);
#else
				cell->setType(PathfindCell::CELL_BRIDGE_IMPASSABLE);
#endif
			}
		}
	}
}

/**
 * Classifies the pathfind cells for the wall bridge layer.
 */
void PathfindLayer::classifyWallCells(ObjectID *wallPieces, Int numPieces)
{
	DEBUG_ASSERTCRASH(m_layer==LAYER_WALL, ("Wrong layer for wall."));
	if (m_layer != LAYER_WALL) return;
	if (m_layerCells == nullptr) return;

	Int i, j;
	for (i=0; i<m_width; i++) {
		for (j=0; j<m_height; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			cell->setConnectLayer(LAYER_INVALID);
			cell->setLayer(m_layer);
			classifyWallMapCell(i+m_xOrigin, j+m_yOrigin, cell, wallPieces, numPieces);
			cell->setPinched(false);
		}
	}
	if (m_destroyed) {
		Int i, j;
		for (i=0; i<m_width; i++) {
			for (j=0; j<m_height; j++) {
				PathfindCell *cell = &m_layerCells[i][j];
				if (cell->getConnectLayer() == LAYER_GROUND) {
					PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND, i+m_xOrigin, j+m_yOrigin);
					DEBUG_ASSERTCRASH(groundCell, ("Should have cell."));
					if (groundCell) {
						DEBUG_ASSERTCRASH(groundCell->getConnectLayer()==m_layer, ("Should connect to this layer.jba."));
						groundCell->setConnectLayer(LAYER_INVALID); // disconnect it.
					}
				}
				cell->setType(PathfindCell::CELL_IMPASSABLE);
			}
		}
	}

	// Tighten up 1 cell.
	for (i=1; i<m_width-1; i++) {
		for (j=1; j<m_height-1; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			Int k, l;
			for (k=i-1; k<i+2; k++) {
				for (l=j-1; l<j+2; l++) {
					PathfindCell *adjacentCell = &m_layerCells[k][l];
					if (adjacentCell->getType() != PathfindCell::CELL_CLEAR) {
						cell->setPinched(true);
					}
				}
			}
		}
	}
	for (i=0; i<m_width; i++) {
		for (j=0; j<m_height; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			if (cell->getPinched() && cell->getType() == PathfindCell::CELL_CLEAR) {
				cell->setType(PathfindCell::CELL_CLIFF);
			}
			cell->setPinched(false);
		}
	}
}

/**
 * Relassifies the pathfind cells for the destroyed bridge layer.
 */
Bool PathfindLayer::setDestroyed(Bool destroyed)
{
	if (destroyed == m_destroyed) return false;

	m_destroyed = destroyed;
	classifyCells();

	return true;
}

/**
 * Copies m_zone into the zone for all the member cells.
 */
void PathfindLayer::applyZone()
{
	Int i, j;
	for (i=0; i<m_width; i++) {
		for (j=0; j<m_height; j++) {
			PathfindCell *cell = &m_layerCells[i][j];
			cell->setZone(m_zone);
		}
	}
}


/**
 * Return the bridge's object id.
 */
ObjectID PathfindLayer::getBridgeID()
{
	return m_bridge->peekBridgeInfo()->bridgeObjectID;
}

/**
 * Return the cell at the index location.
 */
PathfindCell *PathfindLayer::getCell(Int x, Int y)
{
	DEBUG_ASSERTCRASH(m_layerCells, ("no data in layer, why get cells?"));
	if (m_layerCells==nullptr) {
		return nullptr;
	}
	x -= m_xOrigin;
	y -= m_yOrigin;
	if (x<0 || x>=m_width) return nullptr;
	if (y<0 || y>=m_height) return nullptr;
	PathfindCell *cell = &m_layerCells[x][y];
	if (cell->getType() == PathfindCell::CELL_IMPASSABLE) {
		return nullptr; // Impassable cells are ignored.
	}
	return cell;
}


/**
 * Classify the given map cell as clear, or not, etc.
 */
void PathfindLayer::classifyLayerMapCell( Int i, Int j , PathfindCell *cell, Bridge *theBridge)
{
	Coord3D topLeftCorner, bottomRightCorner;

	topLeftCorner.y = (Real)j * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.y = topLeftCorner.y + PATHFIND_CELL_SIZE_F;

	topLeftCorner.x = (Real)i * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.x = topLeftCorner.x + PATHFIND_CELL_SIZE_F;


	Int bridgeCount = 0;
	Coord3D pt;
	if (theBridge->isPointOnBridge(&topLeftCorner) ) {
		bridgeCount++;
	}
	pt = topLeftCorner;
	pt.y = bottomRightCorner.y;
	if (theBridge->isPointOnBridge(&pt) ) {
		bridgeCount++;
	}
	if (theBridge->isPointOnBridge(&bottomRightCorner) ) {
		bridgeCount++;
	}
	pt = topLeftCorner;
	pt.x = bottomRightCorner.x;
	if (theBridge->isPointOnBridge(&pt) ) {
		bridgeCount++;
	}
	cell->reset();
	cell->setLayer(m_layer);
	cell->setType(PathfindCell::CELL_IMPASSABLE);
	if (bridgeCount == 4) {
		cell->setType(PathfindCell::CELL_CLEAR);
	} else {
		if (bridgeCount!=0) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
			cell->setType(PathfindCell::CELL_CLIFF); // it's off the bridge.
#else
			cell->setType(PathfindCell::CELL_BRIDGE_IMPASSABLE); // it's off the bridge.
#endif
		}

		// check against the end lines.

		Region2D cellBounds;
		cellBounds.lo = topLeftCorner.asCoord2D();
		cellBounds.hi = bottomRightCorner.asCoord2D();

#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
		if (m_bridge->isCellOnEnd(&cellBounds)) {
			cell->setType(PathfindCell::CELL_CLEAR);
		}
		if (m_bridge->isCellOnSide(&cellBounds)) {
			cell->setType(PathfindCell::CELL_CLIFF);
		} else {
			if (m_bridge->isCellEntryPoint(&cellBounds)) {
				cell->setType(PathfindCell::CELL_CLEAR);
				cell->setConnectLayer(LAYER_GROUND);
				PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND, i, j );
				groundCell->setConnectLayer(cell->getLayer());
			}
		}
#else
		if (m_bridge->isCellOnSide(&cellBounds)) {
			cell->setType(PathfindCell::CELL_BRIDGE_IMPASSABLE);
		} else {
			if (m_bridge->isCellOnEnd(&cellBounds)) {
				cell->setType(PathfindCell::CELL_CLEAR);
			}
			if (m_bridge->isCellEntryPoint(&cellBounds)) {
				cell->setType(PathfindCell::CELL_CLEAR);
				cell->setConnectLayer(LAYER_GROUND);
				PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND, i, j );
				groundCell->setConnectLayer(cell->getLayer());
			}
		}
#endif
	}
	Coord3D center = topLeftCorner;
	center.x += PATHFIND_CELL_SIZE/2;
	center.y += PATHFIND_CELL_SIZE/2;
	if (cell->getType()!=PathfindCell::CELL_IMPASSABLE) {
		if (!(cell->getConnectLayer()==LAYER_GROUND) ) {
			// Check for bridge clearance.  If the ground isn't 1 pathfind cells below, mark impassable.
			Real groundHeight = TheTerrainLogic->getLayerHeight( center.x, center.y, LAYER_GROUND );
			Real bridgeHeight = theBridge->getBridgeHeight( &center, nullptr );
			if (groundHeight+LAYER_Z_CLOSE_ENOUGH_F > bridgeHeight) {
				PathfindCell *groundCell = TheAI->pathfinder()->getCell(LAYER_GROUND,i, j);
				if (!(groundCell->getType()==PathfindCell::CELL_OBSTACLE)) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
					groundCell->setType(PathfindCell::CELL_IMPASSABLE);
#else
					groundCell->setType(PathfindCell::CELL_BRIDGE_IMPASSABLE);
#endif
				}
			}
		}
	}
}


Bool PathfindLayer::isPointOnWall(ObjectID *wallPieces, Int numPieces, const Coord3D *pt)
{
	Int i;
	for (i=0; i<numPieces; i++) {
		Object *obj = TheGameLogic->findObjectByID(wallPieces[i]);
		if (obj==nullptr) continue;
		Real major = obj->getGeometryInfo().getMajorRadius();
		Real minor = (obj->getGeometryInfo().getGeomType() == GEOMETRY_SPHERE) ? obj->getGeometryInfo().getMajorRadius() : obj->getGeometryInfo().getMinorRadius();

		Real c = (Real)Cos(-obj->getOrientation());
		Real s = (Real)Sin(-obj->getOrientation());

		// convert to a delta relative to rect ctr
		Real ptx = pt->x - obj->getPosition()->x;
		Real pty = pt->y - obj->getPosition()->y;

		// inverse-rotate it to the right coord system
		Real ptx_new = (Real)fabs(ptx*c - pty*s);
		Real pty_new = (Real)fabs(ptx*s + pty*c);

		if (ptx_new <= major && pty_new <= minor)
		{
			return true;
		}
	}
	return false;
}


/**
 * Classify the given map cell as clear, or not, etc.
 */
void PathfindLayer::classifyWallMapCell( Int i, Int j , PathfindCell *cell, ObjectID *wallPieces, Int numPieces)
{
	Coord3D topLeftCorner, bottomRightCorner;

	topLeftCorner.y = (Real)j * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.y = topLeftCorner.y + PATHFIND_CELL_SIZE_F;

	topLeftCorner.x = (Real)i * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.x = topLeftCorner.x + PATHFIND_CELL_SIZE_F;


	Int bridgeCount = 0;
	Coord3D pt;
	if (isPointOnWall(wallPieces, numPieces, &topLeftCorner) ) {
		bridgeCount++;
	}
	pt = topLeftCorner;
	pt.y = bottomRightCorner.y;
	if (isPointOnWall(wallPieces, numPieces, &pt) ) {
		bridgeCount++;
	}
	if (isPointOnWall(wallPieces, numPieces, &bottomRightCorner) ) {
		bridgeCount++;
	}
	pt = topLeftCorner;
	pt.x = bottomRightCorner.x;
	if (isPointOnWall(wallPieces, numPieces, &pt) ) {
		bridgeCount++;
	}
	cell->reset();
	cell->setLayer(m_layer);
	cell->setType(PathfindCell::CELL_IMPASSABLE);
	if (bridgeCount == 4) {
		cell->setType(PathfindCell::CELL_CLEAR);
	} else {
		if (bridgeCount!=0) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
			cell->setType(PathfindCell::CELL_CLIFF); // it's off the bridge.
#else
			cell->setType(PathfindCell::CELL_BRIDGE_IMPASSABLE); // it's off the bridge.
#endif
		}

	}
}

//----------------------- Pathfinder ---------------------------------------

// GeneralsX: PF27, the new A* kernel - see the file's header.
#include "GameLogic/PF27Kernel.inl"

Pathfinder::Pathfinder() :m_map(nullptr), m_pf27(nullptr), m_pf27Mode(PF27_LEGACY)
{
	debugPath = nullptr;
	PathfindCellInfo::allocateCellInfos();
	m_openList.allocateIndex();	// GeneralsX: see PathfindCellList::allocateIndex
	reset();
}

Pathfinder::~Pathfinder()
{
	delete m_pf27;	// GeneralsX PF27
	m_pf27 = nullptr;
	PathfindCellInfo::releaseCellInfos();
}

void Pathfinder::reset()
{
	frameToShowObstacles = 0;
	DEBUG_LOG(("Pathfind cell is %d bytes, PathfindCellInfo is %d bytes", sizeof(PathfindCell), sizeof(PathfindCellInfo)));

	delete [] m_blockOfMapCells;
	m_blockOfMapCells = nullptr;

	delete [] m_map;
	m_map = nullptr;

	// GeneralsX PF27: the kernel's records mirror the grid too; the trace's last batch is flushed with an end marker.
	if (m_pf27 != nullptr)
		m_pf27->release();
	pf27TraceFlush(true);

	// GeneralsX: The checkForMovement memo mirrors the grid, so it
	// dies with it. Bump the generation as well: the next map's cell (0,0) is not this map's.
	delete [] s_gxMoveMemo;
	s_gxMoveMemo        = nullptr;
	s_gxMoveMemoCells   = 0;
	s_gxMoveMemoStrideY = 0;
	delete [] s_gxClearMemo;
	s_gxClearMemo        = nullptr;
	s_gxClearMemoCells   = 0;
	s_gxClearMemoStrideY = 0;
	++s_gxClearGen;
	s_gxMoveObj         = nullptr;
	s_gxMoveRadius      = -1;
	++s_gxMoveGen;

	Int i;
	for (i=0; i<=LAYER_LAST; i++) {
		m_layers[i].reset();
	}

	// reset the pathfind grid
	m_extent.lo.x=m_extent.lo.y=m_extent.hi.x=m_extent.hi.y=0;
	m_logicalExtent.lo.x=m_logicalExtent.lo.y=m_logicalExtent.hi.x=m_logicalExtent.hi.y=0;
	m_openList.reset();
	m_closedList.reset();

	m_ignoreObstacleID = INVALID_ID;
	m_isTunneling = false;

	m_moveAlliesDepth = 0;

	// pathfind grid cells have not been classified yet
	m_isMapReady = false;
	m_cumulativeCellsAllocated = 0;

	debugPathPos.x = 0.0f;
	debugPathPos.y = 0.0f;
	debugPathPos.z = 0.0f;

	deleteInstance(debugPath);
	debugPath = nullptr;

	m_frameToShowObstacles = 0;

	for (m_queuePRHead=0; m_queuePRHead<PATHFIND_QUEUE_LEN; m_queuePRHead++) {
		m_queuedPathfindRequests[m_queuePRHead] = INVALID_ID;
	}
	m_queuePRHead = 0;
	m_queuePRTail = 0;

	m_numWallPieces = 0;
	for (i=0; i<MAX_WALL_PIECES; ++i)
	{
		m_wallPieces[i] = INVALID_ID;
	}

	if (TheAI && TheAI->getAiData()) {
		m_wallHeight = TheAI->getAiData()->m_wallHeight;
	}
	else
	{
		m_wallHeight = 0.0f;
	}
	m_zoneManager.reset();

#if RETAIL_COMPATIBLE_PATHFINDING
	s_useFixedPathfinding = false;
	s_forceCleanCells = false;
#endif

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
	m_classifyFenceZeroInit = false;
#endif
}

/**
 * Adds a piece of a wall.
 */
void Pathfinder::addWallPiece(Object *wallPiece)
{
	if (m_numWallPieces<MAX_WALL_PIECES-1) {
		m_wallPieces[m_numWallPieces] = wallPiece->getID();
		m_numWallPieces++;
	}
}

/**
 * Removes a piece of a wall
 */
void Pathfinder::removeWallPiece(Object *wallPiece)
{

	// sanity
  if( wallPiece == nullptr )
		return;

	// find entry
	for( Int i = 0; i < m_numWallPieces; ++i )
	{

		// match by id
		if( m_wallPieces[ i ] == wallPiece->getID() )
		{

			// put the last id in the wall piece array here
			m_wallPieces[ i ] = m_wallPieces[ m_numWallPieces - 1 ];

			// we now have one less entry
			m_numWallPieces--;

			// all done
			return;

		}

	}

}

/**
 * Checks if a point is on the wall.
 */
Bool Pathfinder::isPointOnWall(const Coord3D *pos)
{
	if (m_numWallPieces==0) return false;
	if (m_layers[LAYER_WALL].isUnused()) return false;
	PathfindLayerEnum layer = (PathfindLayerEnum)LAYER_WALL;
	PathfindCell *cell = getCell(layer, pos);
	// make sure the layer matches, since getCell can return ground layer cells if the pos is 'off' the bridge/wall
	if (cell && cell->getLayer() == layer) {
		if (cell->getType() == PathfindCell::CELL_CLEAR) {
			return true;
		}
	}
	return false;
}

/**
 * Adds a bridge & returns the layer.
 */
PathfindLayerEnum Pathfinder::addBridge(Bridge *theBridge)
{
	Int layer = LAYER_GROUND+1;
	while (layer<=LAYER_WALL) {
		if (m_layers[layer].isUnused()) {
			if (m_layers[layer].init(theBridge, (PathfindLayerEnum)layer) ) {
				return (PathfindLayerEnum)layer;
			}
			DEBUG_LOG(("WARNING: Bridge failed to init in pathfinder"));
			return LAYER_GROUND; // failed to init, usually cause off of the map.  jba.
		}
		layer++;
	}
	DEBUG_CRASH(("Ran out of bridge layers."));
	return LAYER_GROUND;
}

/**
 * Updates an object's layer, making sure the object is actually on the bridge first.
 */
void Pathfinder::updateLayer(Object *obj, PathfindLayerEnum layer)
{
	if (layer != LAYER_GROUND) {
		if (!TheTerrainLogic->objectInteractsWithBridgeLayer(obj, layer)) {
			layer = LAYER_GROUND;
		}
	}
	//DEBUG_LOG(("Object layer is %d", layer));
	obj->setLayer(layer);
}

/**
 * Classify the cells under the given object
 * If 'insert' is true, object is being added
 * If 'insert' is false, object is being removed
 */
void Pathfinder::classifyFence( Object *obj, Bool insert )
{
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
	m_zoneManager.markZonesDirty();
#endif

	const Coord3D *pos = obj->getPosition();
  Real angle = obj->getOrientation();

 	Real halfsizeX = obj->getTemplate()->getFenceWidth()/2;
 	Real halfsizeY = PATHFIND_CELL_SIZE_F/10.0f;
 	Real fenceOffset = obj->getTemplate()->getFenceXOffset();

 	Real c = (Real)Cos(angle);
 	Real s = (Real)Sin(angle);

 	const Real STEP_SIZE = PATHFIND_CELL_SIZE_F * 0.5f;	// in theory, should be PATHFIND_CELL_SIZE_F exactly, but needs to be smaller to avoid aliasing problems
 	Real ydx = s * STEP_SIZE;
 	Real ydy = -c * STEP_SIZE;
 	Real xdx = c * STEP_SIZE;
 	Real xdy = s * STEP_SIZE;

 	Int numStepsX = REAL_TO_INT_CEIL(2.0f * halfsizeX / STEP_SIZE);
 	Int numStepsY = REAL_TO_INT_CEIL(2.0f * halfsizeY / STEP_SIZE);

 	Real tl_x = pos->x - fenceOffset*c - halfsizeY*s;
 	Real tl_y = pos->y + halfsizeY*c - fenceOffset*s;

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	IRegion2D cellBounds;
	cellBounds.lo.x = REAL_TO_INT_FLOOR((pos->x + 0.5f)/PATHFIND_CELL_SIZE_F);
	cellBounds.lo.y = REAL_TO_INT_FLOOR((pos->y + 0.5f)/PATHFIND_CELL_SIZE_F);
	// TheSuperHackers @fix Mauller 16/06/2025 Fixes uninitialized variables.
#if RETAIL_COMPATIBLE_CRC
	//CRCDEBUG_LOG(("Pathfinder::classifyFence - (%d,%d)", cellBounds.hi.x, cellBounds.hi.y));

	// For retail the values on the stack are often either 0 or larger than the map size.
	// We initialize them to reduce the likelihood of a mismatch.
	if (m_classifyFenceZeroInit)
	{
		cellBounds.hi.x = 0;
		cellBounds.hi.y = 0;
	}
	else
	{
		cellBounds.hi.x = 1000000;
		cellBounds.hi.y = 1000000;
	}
#else
	cellBounds.hi.x = REAL_TO_INT_CEIL((pos->x + 0.5f)/PATHFIND_CELL_SIZE_F);
	cellBounds.hi.y = REAL_TO_INT_CEIL((pos->y + 0.5f)/PATHFIND_CELL_SIZE_F);
#endif
	Bool didAnything = false;
#endif // !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)

 	for (Int iy = 0; iy < numStepsY; ++iy, tl_x += ydx, tl_y += ydy)
 	{
 		Real x = tl_x;
 		Real y = tl_y;
 		for (Int ix = 0; ix < numStepsX; ++ix, x += xdx, y += xdy)
 		{
 			Int cx = REAL_TO_INT_FLOOR((x + 0.5f)/PATHFIND_CELL_SIZE_F);
 			Int cy = REAL_TO_INT_FLOOR((y + 0.5f)/PATHFIND_CELL_SIZE_F);
 			if (cx >= 0 && cy >= 0 && cx < m_extent.hi.x && cy < m_extent.hi.y)
 			{
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
 				if (insert) {
 					ICoord2D pos;
 					pos.x = cx;
 					pos.y = cy;
 					m_map[cx][cy].setTypeAsObstacle( obj, true, pos );
 				}
 				else
 					m_map[cx][cy].removeObstacle(obj);
#else
 				if (insert) {
 					ICoord2D pos;
 					pos.x = cx;
 					pos.y = cy;
					if (m_map[cx][cy].setTypeAsObstacle( obj, true, pos )) {
						didAnything = true;
 						m_map[cx][cy].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
					}
 				}
				else {
					if (m_map[cx][cy].removeObstacle(obj)) {
						didAnything = true;
 						m_map[cx][cy].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
					}
				}
				if (cellBounds.lo.x>cx) cellBounds.lo.x = cx;
 				if (cellBounds.lo.y>cy) cellBounds.lo.y = cy;
 				if (cellBounds.hi.x<cx) cellBounds.hi.x = cx;
 				if (cellBounds.hi.y<cy) cellBounds.hi.y = cy;
#endif
 			}
 		}
 	}
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	if (didAnything) {
		m_zoneManager.markZonesDirty();
		m_zoneManager.updateZonesForModify(m_map, m_layers, cellBounds, m_extent);
	}
#endif
}

/**
 * Classify the cells under the given object
 * If 'insert' is true, object is being added
 * If 'insert' is false, object is being removed
 */
void Pathfinder::classifyObjectFootprint( Object *obj, Bool insert )
{
	if (obj->isKindOf(KINDOF_MINE)) {
		return;  // don't pathfind around mines.
	}

	if (obj->isKindOf(KINDOF_PROJECTILE)) {
		return;  // don't care about projectiles.
	}

	if (obj->isKindOf(KINDOF_BRIDGE_TOWER)) {
		return;  // It is important to not abuse bridge towers.
	}

	if (obj->getTemplate()->getFenceWidth() > 0.0f)
	{
		if (!obj->isKindOf(KINDOF_DEFENSIVE_WALL))
		{
			classifyFence(obj, insert);
			return;
		}
	}

	if (!insert) {
		// Just in case, remove the object.  Remove checks that the object has been added before
		// removing, so it's safer to just remove it, as by the time some units "die", they've become
		// lifeless immobile husks of debris, but we still need to remove them.  jba.

#if !RTS_GENERALS
    if ( obj->isKindOf( KINDOF_BLAST_CRATER ) ) // since these footprints are permanent, never remove them
      return;
#endif

		removeUnitFromPathfindMap(obj);
		if (obj->isKindOf(KINDOF_WALK_ON_TOP_OF_WALL)) {
			if (!m_layers[LAYER_WALL].isUnused()) {
				Int i;
				ObjectID curID = obj->getID();
				for (i=0; i<m_numWallPieces; i++) {
					if (curID == m_wallPieces[i]) {
						m_wallPieces[i]=INVALID_ID;
					}
				}
				// Kill anybody on the wall.
				Object *obj;
				for (obj = TheGameLogic->getFirstObject(); obj; obj=obj->getNextObject()) {
					if (obj->getLayer() == LAYER_WALL) {
						if (m_layers[LAYER_WALL].isPointOnWall(&curID, 1, obj->getPosition()))
						{
							// The object fell off the wall.
							// Destroy it.
							DamageInfo extraDamageInfo;
							extraDamageInfo.in.m_damageType = DAMAGE_FALLING;
							extraDamageInfo.in.m_deathType = DEATH_SPLATTED;
							extraDamageInfo.in.m_sourceID = obj->getID();
							extraDamageInfo.in.m_amount = HUGE_DAMAGE_AMOUNT;
							obj->attemptDamage(&extraDamageInfo);
						}
					}
				}
				// recalc the wall.
				m_layers[LAYER_WALL].classifyWallCells(m_wallPieces, m_numWallPieces);
			}
		}
	}
	if (!obj->isKindOf(KINDOF_STRUCTURE)) {
		return;  // Only path around structures.
	}
	if (obj->isMobile()) {
		return; // mobile aren't obstacles.
	}
	/// For now, all small objects will not be obstacles
	if (obj->getGeometryInfo().getIsSmall()) {
		return;
	}

#if RTS_GENERALS
	if (obj->getHeightAboveTerrain() > PATHFIND_CELL_SIZE_F) {
		return; // Don't add bounds that are up in the air.
	}
#else
	if (obj->getHeightAboveTerrain() > PATHFIND_CELL_SIZE_F && ( ! obj->isKindOf( KINDOF_BLAST_CRATER ) ) )
  {
		return; // Don't add bounds that are up in the air.... unless a blast crater wants to do just that
	}
#endif
	internal_classifyObjectFootprint(obj, insert);
}

void Pathfinder::internal_classifyObjectFootprint( Object *obj, Bool insert )
{
	const Coord3D *pos = obj->getPosition();

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	IRegion2D cellBounds;
	cellBounds.lo.x = REAL_TO_INT_FLOOR((pos->x + 0.5f)/PATHFIND_CELL_SIZE_F);
	cellBounds.lo.y = REAL_TO_INT_FLOOR((pos->y + 0.5f)/PATHFIND_CELL_SIZE_F);
	cellBounds.hi = cellBounds.lo;
#endif

	switch(obj->getGeometryInfo().getGeomType())
	{
		case GEOMETRY_BOX:
		{
			m_zoneManager.markZonesDirty();
			Real angle = obj->getOrientation();

			Real halfsizeX = obj->getGeometryInfo().getMajorRadius();
			Real halfsizeY = obj->getGeometryInfo().getMinorRadius();

			Real c = (Real)Cos(angle);
			Real s = (Real)Sin(angle);

			const Real STEP_SIZE = PATHFIND_CELL_SIZE_F * 0.5f;	// in theory, should be PATHFIND_CELL_SIZE_F exactly, but needs to be smaller to avoid aliasing problems
			Real ydx = s * STEP_SIZE;
			Real ydy = -c * STEP_SIZE;
			Real xdx = c * STEP_SIZE;
			Real xdy = s * STEP_SIZE;

			Int numStepsX = REAL_TO_INT_CEIL(2.0f * halfsizeX / STEP_SIZE);
			Int numStepsY = REAL_TO_INT_CEIL(2.0f * halfsizeY / STEP_SIZE);

			Real tl_x = pos->x - halfsizeX*c - halfsizeY*s;
			Real tl_y = pos->y + halfsizeY*c - halfsizeX*s;

			for (Int iy = 0; iy < numStepsY; ++iy, tl_x += ydx, tl_y += ydy)
			{
				Real x = tl_x;
				Real y = tl_y;
				for (Int ix = 0; ix < numStepsX; ++ix, x += xdx, y += xdy)
				{
					Int cx = REAL_TO_INT_FLOOR((x + 0.5f)/PATHFIND_CELL_SIZE_F);
					Int cy = REAL_TO_INT_FLOOR((y + 0.5f)/PATHFIND_CELL_SIZE_F);

#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
					if (cx >= 0 && cy >= 0 && cx < m_extent.hi.x && cy < m_extent.hi.y)
					{
						if (insert) {
							ICoord2D pos;
							pos.x = cx;
							pos.y = cy;
							m_map[cx][cy].setTypeAsObstacle( obj, false, pos );
						}
						else
							m_map[cx][cy].removeObstacle(obj);
					}
#else
					if (cx >= 0 && cy >= 0 && cx < m_extent.hi.x && cy < m_extent.hi.y)
					{
						if (insert) {
							ICoord2D pos;
							pos.x = cx;
							pos.y = cy;
							if (m_map[cx][cy].setTypeAsObstacle( obj, false, pos )) {
 								m_map[cx][cy].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
							}
						}
						else {
							if (m_map[cx][cy].removeObstacle(obj)) {
 								m_map[cx][cy].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
							}
						}
 						if (cellBounds.lo.x>cx) cellBounds.lo.x = cx;
 						if (cellBounds.lo.y>cy) cellBounds.lo.y = cy;
 						if (cellBounds.hi.x<cx) cellBounds.hi.x = cx;
 						if (cellBounds.hi.y<cy) cellBounds.hi.y = cy;
					}
#endif
				}
			}
		}
		break;

		case GEOMETRY_SPHERE:	// not quite right, but close enough
		case GEOMETRY_CYLINDER:
		{
			m_zoneManager.markZonesDirty();
			// fill in all cells that overlap as obstacle cells
			/// @todo This is a very inefficient circle-rasterizer
			ICoord2D topLeft, bottomRight;
			Coord2D center, delta;
			Real radius = obj->getGeometryInfo().getMajorRadius();
			Real r2, size;

			topLeft.x = REAL_TO_INT_FLOOR(0.5f + (pos->x - radius)/PATHFIND_CELL_SIZE_F)-1;
			topLeft.y = REAL_TO_INT_FLOOR(0.5f + (pos->y - radius)/PATHFIND_CELL_SIZE_F)-1;
			size = (radius/PATHFIND_CELL_SIZE_F);
			center.x = (pos->x/PATHFIND_CELL_SIZE_F);
			center.y = (pos->y/PATHFIND_CELL_SIZE_F);

			size += 0.4f;
			r2 = size*size;

			bottomRight.x = topLeft.x + 2*size + 2;
			bottomRight.y = topLeft.y + 2*size + 2;

			for( int j = topLeft.y; j < bottomRight.y; j++ )
			{
				for( int i = topLeft.x; i < bottomRight.x; i++ )
				{
					delta.x = i+0.5f - center.x;
					delta.y = j+0.5f - center.y;

					if (delta.x*delta.x + delta.y*delta.y <= r2)
					{
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
						if (i >= 0 && j >= 0 && i < m_extent.hi.x && j < m_extent.hi.y)
						{
							if (insert)	{
								ICoord2D pos;
								pos.x = i;
								pos.y = j;
								m_map[i][j].setTypeAsObstacle( obj, false, pos );
							}
							else
								m_map[i][j].removeObstacle( obj );
						}
#else
						if (i >= 0 && j >= 0 && i < m_extent.hi.x && j < m_extent.hi.y)
						{
							if (insert) {
								ICoord2D pos;
								pos.x = i;
								pos.y = j;
								if (m_map[i][j].setTypeAsObstacle( obj, false, pos )) {
 									m_map[i][j].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
								}
							}
							else {
								if (m_map[i][j].removeObstacle(obj)) {
 									m_map[i][j].setZone(PathfindZoneManager::UNINITIALIZED_ZONE);
								}
							}
 							if (cellBounds.lo.x>i) cellBounds.lo.x = i;
 							if (cellBounds.lo.y>j) cellBounds.lo.y = j;
 							if (cellBounds.hi.x<i) cellBounds.hi.x = i;
 							if (cellBounds.hi.y<j) cellBounds.hi.y = j;
						}
#endif
					}
				}
			}
		}
		break;
	}

#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
	Region2D bounds;
	obj->getGeometryInfo().get2DBounds(*obj->getPosition(), obj->getOrientation(), bounds);
	IRegion2D cellBounds;
	cellBounds.lo.x = REAL_TO_INT_FLOOR(bounds.lo.x/PATHFIND_CELL_SIZE_F)-1;
	cellBounds.lo.y = REAL_TO_INT_FLOOR(bounds.lo.y/PATHFIND_CELL_SIZE_F)-1;
	cellBounds.hi.x = REAL_TO_INT_CEIL(bounds.hi.x/PATHFIND_CELL_SIZE_F)+1;
	cellBounds.hi.y = REAL_TO_INT_CEIL(bounds.hi.y/PATHFIND_CELL_SIZE_F)+1;
#else
	m_zoneManager.updateZonesForModify(m_map, m_layers, cellBounds, m_extent);

	cellBounds.lo.x -= 2;
	cellBounds.lo.y -= 2;
	cellBounds.hi.x += 2;
	cellBounds.hi.y += 2;
#endif

	Int i, j;

	cellBounds.intersectWith(m_extent);

	if (!insert) {
		for( j=cellBounds.lo.y; j<=cellBounds.hi.y; j++ )
		{
			for( i=cellBounds.lo.x; i<=cellBounds.hi.x; i++ )
			{
				if (m_map[i][j].getType()==PathfindCell::CELL_IMPASSABLE) {
					m_map[i][j].setType(PathfindCell::CELL_CLEAR);
				}
			}
		}
	}
	// Check for pinched cells, and close them off.

	for( j=cellBounds.lo.y; j<=cellBounds.hi.y; j++ )
	{
		for( i=cellBounds.lo.x; i<=cellBounds.hi.x; i++ )
		{
			m_map[i][j].setPinched(false);
			if (m_map[i][j].getType() == PathfindCell::CELL_CLEAR) {
				Int totalCount = 0;
				Int orthogonalCount = 0;
				Int k, l;
				for (k=i-1; k<i+2; k++) {
					if (k<m_extent.lo.x || k> m_extent.hi.x) continue;
					for (l=j-1; l<j+2; l++) {
						if (l<m_extent.lo.y || l> m_extent.hi.y) continue;
						if ((k==i) && (j==l)) continue;
						if (m_map[k][l].getType() == PathfindCell::CELL_CLEAR) {
							totalCount++;
							if ((k==i) || (l==j)) {
								orthogonalCount++;
							}
						}

					}
				}
				// If the total open cells are < 2 or total cells < 4, we are pinched.
				if (orthogonalCount<2 || totalCount<4) {
					m_map[i][j].setPinched(true);
				}
			}
		}
	}

#if RETAIL_COMPATIBLE_PATHFINDING
	for( j=cellBounds.lo.y; j<=cellBounds.hi.y; j++ )
	{
		for( i=cellBounds.lo.x; i<=cellBounds.hi.x; i++ )
		{
			if (m_map[i][j].getPinched() && (m_map[i][j].getType() == PathfindCell::CELL_CLEAR)) {
				m_map[i][j].setType(PathfindCell::CELL_IMPASSABLE);
				m_map[i][j].setPinched(false);
			}
		}
	}
#endif

	// Expand building bounds 1 cell.
	for( j=cellBounds.lo.y; j<=cellBounds.hi.y; j++ )
	{
		for( i=cellBounds.lo.x; i<=cellBounds.hi.x; i++ )
		{
			if (m_map[i][j].getType() == PathfindCell::CELL_CLEAR) {
				Bool objectAdjacent = false;
				Int k, l;
				for (k=i-1; k<i+2; k++) {
					if (k<m_extent.lo.x || k> m_extent.hi.x) continue;
					for (l=j-1; l<j+2; l++) {
						if (l<m_extent.lo.y || l> m_extent.hi.y) continue;
						if ((k==i) && (l==j)) continue;
						if ((k!=i) && (l!=j)) continue;
						if (m_map[k][l].getType() == PathfindCell::CELL_OBSTACLE) {
							objectAdjacent = true;
							break;
						}

					}
				}
				if (objectAdjacent) {
					m_map[i][j].setPinched(true);
				}
			}
		}
	}
}

/**
 * Classify the given map cell as WATER, CLIFF, etc.
 * Note that this does NOT classify cells as OBSTACLES.
 * OBSTACLE cells are classified only via objects.
 * @todo optimize this - lots of redundant computation
 */
void Pathfinder::classifyMapCell( Int i, Int j , PathfindCell *cell)
{
	Coord3D topLeftCorner, bottomRightCorner;


	Bool hasObstacle =  (cell->getType() == PathfindCell::CELL_OBSTACLE) ;

	topLeftCorner.y = (Real)j * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.y = topLeftCorner.y + PATHFIND_CELL_SIZE_F;

	topLeftCorner.x = (Real)i * PATHFIND_CELL_SIZE_F;
	bottomRightCorner.x = topLeftCorner.x + PATHFIND_CELL_SIZE_F;

	cell->setPinched(false);

	PathfindCell::CellType type = PathfindCell::CELL_CLEAR;
	if (TheTerrainLogic->isCliffCell(topLeftCorner.x, topLeftCorner.y))
	{
		type = PathfindCell::CELL_CLIFF;
	}

	//
	// If any corners are underwater, this is a water cell
	//
	if (TheTerrainLogic->isUnderwater( topLeftCorner.x, topLeftCorner.y ) ) type = PathfindCell::CELL_WATER;
	if (TheTerrainLogic->isUnderwater( topLeftCorner.x, bottomRightCorner.y) ) type = PathfindCell::CELL_WATER;
	if (TheTerrainLogic->isUnderwater( bottomRightCorner.x, bottomRightCorner.y ) ) type = PathfindCell::CELL_WATER;
	if (TheTerrainLogic->isUnderwater( bottomRightCorner.x, topLeftCorner.y ) ) type = PathfindCell::CELL_WATER;

	if (hasObstacle) {
		type =  PathfindCell::CELL_OBSTACLE;
	}
	cell->setType( type );
	cell->releaseInfo();
}

/**
 * Set up for a new map.
 */
void Pathfinder::newMap()
{
	m_wallHeight = TheAI->getAiData()->m_wallHeight; // may be updated by map.ini.
	Region3D terrainExtent;
	TheTerrainLogic->getMaximumPathfindExtent( &terrainExtent );
	IRegion2D bounds;
	bounds.lo.x = REAL_TO_INT_FLOOR(terrainExtent.lo.x / PATHFIND_CELL_SIZE_F);
	bounds.hi.x = REAL_TO_INT_FLOOR(terrainExtent.hi.x / PATHFIND_CELL_SIZE_F);
	bounds.lo.y = REAL_TO_INT_FLOOR(terrainExtent.lo.y / PATHFIND_CELL_SIZE_F);
	bounds.hi.y = REAL_TO_INT_FLOOR(terrainExtent.hi.y / PATHFIND_CELL_SIZE_F);
	bounds.hi.x--;
	bounds.hi.y--;
	Bool dataAllocated = false;
	if (m_extent.hi.x==bounds.hi.x && m_extent.hi.y==bounds.hi.y) {
		if (m_blockOfMapCells != nullptr && m_map!=nullptr) {
			dataAllocated = true;
		}
	}
	// For map load from file, we have to call newMap twice to do sequencing issues.
	// so the second time through, dataAllocated==TRUE, so we skip the allocate.
	if (!dataAllocated) {
		m_extent = bounds;
		DEBUG_ASSERTCRASH(m_map == nullptr, ("Can't reallocate pathfind cells."));
 		m_zoneManager.allocateBlocks(m_extent);
		// Allocate cells.
		m_blockOfMapCells = MSGNEW("PathfindMapCells") PathfindCell[(bounds.hi.x+1)*(bounds.hi.y+1)];
		m_map = MSGNEW("PathfindMapCells") PathfindCellP[bounds.hi.x+1];
		Int i;
		for (i=0; i<=bounds.hi.x; i++) {
			m_map[i] = &m_blockOfMapCells[i*(bounds.hi.y+1)];
		}
		for (i=0; i<LAYER_LAST; i++) {
			if (!m_layers[i].isUnused()) {
				m_layers[i].allocateCells(&m_extent);
			}
		}
		if (m_numWallPieces>0) {
			m_layers[LAYER_WALL].init(nullptr, LAYER_WALL);
			m_layers[LAYER_WALL].allocateCellsForWallLayer(&m_extent, m_wallPieces, m_numWallPieces);
		}
	}
	classifyMap();
	// Add existing objects.
	Object *obj;
	for( obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		classifyObjectFootprint(obj, true);
	}

	// GeneralsX PF27: which A* loop this map runs (GX_PF27=legacy|new|check). Path-identical either way.
	m_pf27Mode = pf27ModeFromEnv();
#if defined(GX_TEST_INSTRUMENTS)
	if (getenv("GX_PF27_GENWRAP") != nullptr)
		s_gxMoveGen = 0xFFFFFF00u;	// test hook: the memo generation wraps 256 searches into the map; test builds only
#endif
	if ((m_pf27Mode != PF27_LEGACY || pf27TraceLevel() != 0) && m_pf27 == nullptr)
		m_pf27 = MSGNEW("PF27Kernel") PF27Kernel(this);

	m_isMapReady = true;
}

/**
 * Classify all cells in grid as obstacles, etc.
 */
void Pathfinder::classifyMap()
{

	Int i, j;
	// for now, sample cell corners and classify cell accordingly
	for( j=m_extent.lo.y; j<=m_extent.hi.y; j++ )
	{
		for( i=m_extent.lo.x; i<=m_extent.hi.x; i++ )
		{
			classifyMapCell( i, j, &m_map[i][j]);
		}
	}
#if 1
	// Expand all cliff cells one step (mark pinched)
	for( j=m_extent.lo.y; j<=m_extent.hi.y; j++ )
	{
		for( i=m_extent.lo.x; i<=m_extent.hi.x; i++ )
		{
			if (m_map[i][j].getType() & PathfindCell::CELL_CLIFF) {
				Int k, l;
				for (k=i-1; k<i+2; k++) {
					if (k<m_extent.lo.x || k> m_extent.hi.x) continue;
					for (l=j-1; l<j+2; l++) {
						if (l<m_extent.lo.y || l> m_extent.hi.y) continue;
						if (m_map[k][l].getType() == PathfindCell::CELL_CLEAR) {
							m_map[k][l].setPinched(true);
						}

					}
				}
			}
		}
	}
	// Convert pinched to cliff.
	for( j=m_extent.lo.y; j<=m_extent.hi.y; j++ )
	{
		for( i=m_extent.lo.x; i<=m_extent.hi.x; i++ )
		{
			if (m_map[i][j].getPinched()) {
				if (m_map[i][j].getType()==PathfindCell::CELL_CLEAR) {
					m_map[i][j].setType(PathfindCell::CELL_CLIFF);
				}
			}
		}
	}
	// Add a border of pinched cells to cliffs.
	for( j=m_extent.lo.y; j<=m_extent.hi.y; j++ )
	{
		for( i=m_extent.lo.x; i<=m_extent.hi.x; i++ )
		{
			if (m_map[i][j].getType() & PathfindCell::CELL_CLIFF) {
				Int k, l;
				for (k=i-1; k<i+2; k++) {
					if (k<m_extent.lo.x || k> m_extent.hi.x) continue;
					for (l=j-1; l<j+2; l++) {
						if (l<m_extent.lo.y || l> m_extent.hi.y) continue;
						if (m_map[k][l].getType() == PathfindCell::CELL_CLEAR) {
							m_map[k][l].setPinched(true);
						}

					}
				}
			}
		}
	}
#endif
	for (i=0; i<LAYER_LAST; i++) {
		if (!m_layers[i].isUnused()) {
			m_layers[i].classifyCells();
		}
	}
	if (!m_layers[LAYER_WALL].isUnused()) {
		m_layers[LAYER_WALL].classifyWallCells(m_wallPieces, m_numWallPieces);
	}
	m_zoneManager.calculateZones(m_map, m_layers, m_extent);
}


/**
 * Force pathfind map recomputation.
 */
void Pathfinder::forceMapRecalculation()
{
	classifyMap();
}

/**
 * Show all cells touched in the last search
 */
void Pathfinder::debugShowSearch(  Bool pathFound  )
{
	if (!TheGlobalData->m_debugAI) {
		return;
	}
#if defined(RTS_DEBUG)
	extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);

	// show all explored cells for debugging
	PathfindCell *s;
	RGBColor color;
	color.red = color.blue = color.green = 1;
	if (!pathFound) {
		addIcon(nullptr, 0, 0, color);	 // erase.
	}

	for( s = m_openList.getHead(); s; s=s->getNextOpen() )
	{
		// create objects to show path - they decay
		RGBColor color;
		color.red = color.green = 0;
		color.blue = 1;

		Coord3D pos;
		pos.x = ((Real)s->getXIndex() + 0.5f) * PATHFIND_CELL_SIZE_F;
		pos.y = ((Real)s->getYIndex() + 0.5f) * PATHFIND_CELL_SIZE_F;
		pos.z = TheTerrainLogic->getLayerHeight( pos.x, pos.y, s->getLayer() ) + 0.5f;
		addIcon(&pos, PATHFIND_CELL_SIZE_F*.6f, 200, color);
	}

	for( s = m_closedList.getHead(); s; s=s->getNextOpen() )
	{
		// create objects to show path - they decay
		RGBColor color;
		color.red = color.blue = 1;
		color.green = 0;
		if (!pathFound)	color.blue = 0;

		Int length=200;
		if (!pathFound)
			length *= 2;

		Coord3D pos;
		pos.x = ((Real)s->getXIndex() + 0.5f) * PATHFIND_CELL_SIZE_F;
		pos.y = ((Real)s->getYIndex() + 0.5f) * PATHFIND_CELL_SIZE_F;
		pos.z = TheTerrainLogic->getLayerHeight( pos.x, pos.y, s->getLayer()) + 0.5f;
		addIcon(&pos, PATHFIND_CELL_SIZE_F*.6f, length, color);
	}
#endif
}

Locomotor* Pathfinder::chooseBestLocomotorForPosition(PathfindLayerEnum layer, LocomotorSet* locomotorSet, const Coord3D* pos )
{
	Int x = REAL_TO_INT_FLOOR(pos->x/PATHFIND_CELL_SIZE);
	Int y = REAL_TO_INT_FLOOR(pos->y/PATHFIND_CELL_SIZE);
	PathfindCell* cell = getCell(layer, x, y );
	// off the map? call it CELL_CLEAR...
	PathfindCell::CellType celltype = cell ? cell->getType() : PathfindCell::CELL_CLEAR;

	LocomotorSurfaceTypeMask acceptableSurfaces = validLocomotorSurfacesForCellType(celltype);
	return locomotorSet->findLocomotor(acceptableSurfaces);
}

/*static*/ LocomotorSurfaceTypeMask Pathfinder::validLocomotorSurfacesForCellType(PathfindCell::CellType t)
{
	switch (t)
	{
		case PathfindCell::CELL_CLEAR:
			return LOCOMOTORSURFACE_GROUND | LOCOMOTORSURFACE_AIR;

		case PathfindCell::CELL_WATER:
			return LOCOMOTORSURFACE_WATER | LOCOMOTORSURFACE_AIR;

		case PathfindCell::CELL_CLIFF:
			return LOCOMOTORSURFACE_CLIFF | LOCOMOTORSURFACE_AIR;

		case PathfindCell::CELL_RUBBLE:
			return LOCOMOTORSURFACE_RUBBLE | LOCOMOTORSURFACE_AIR;

		case PathfindCell::CELL_OBSTACLE:
		case PathfindCell::CELL_BRIDGE_IMPASSABLE:
		case PathfindCell::CELL_IMPASSABLE:
			return LOCOMOTORSURFACE_AIR;

		default:
			return NO_SURFACES;
	}
}

//
// Return true if we can move onto this position
//
Bool Pathfinder::validMovementTerrain( PathfindLayerEnum layer, const Locomotor* locomotor, const Coord3D *pos)
{
	Int x = REAL_TO_INT_FLOOR(pos->x/PATHFIND_CELL_SIZE);
	Int y = REAL_TO_INT_FLOOR(pos->y/PATHFIND_CELL_SIZE);

	PathfindCell *toCell = nullptr;
	toCell = getCell( layer, x, y );

	if (toCell == nullptr)
		return false;
	// Only do terrain, not obstacle cells.  jba.
	if (toCell->getType()==PathfindCell::CELL_OBSTACLE) return true;
	if (toCell->getType()==PathfindCell::CELL_IMPASSABLE) return true;
	if (toCell->getLayer()!=LAYER_GROUND && toCell->getLayer() == PathfindCell::CELL_CLEAR) {
		return true;
	}
	// check validity of destination cell
	LocomotorSurfaceTypeMask acceptableSurfaces = validLocomotorSurfacesForCellType(toCell->getType());
	if ((locomotor->getLegalSurfaces() & acceptableSurfaces) == 0)
		return false;
	return true;
}

//
// Releases the cells on the open & closed lists.
//
void Pathfinder::cleanOpenAndClosedLists() {
	// GeneralsX: This is the end of a search, on every exit path of
	// every search - so it is where the checkForMovement memo's generation moves. Nothing written
	// by the search that is finishing can be read by the next one. See checkForMovementInSearch.
	++s_gxMoveGen;

	Int count = 0;
	if (!m_openList.empty()) {
		count += PathfindCell::releaseOpenList(m_openList);
		m_openList.reset();
	}

#if RETAIL_COMPATIBLE_PATHFINDING
	// TheSuperHackers @info this is here as the map cells are contained within the pathfinder and cannot be cleaned externally.
	// If the crash mode within PathfindCell::releaseOpenList is hit, it will set s_forceCleanCells to allow the system to cleanly recover.
	if (s_forceCleanCells) {
		forceCleanCells();
		// TheSuperHackers @info cells on the closed list are forcefully cleaned up by this point
		s_forceCleanCells = false;
	}
#endif

	if (!m_closedList.empty()) {
		count += PathfindCell::releaseClosedList(m_closedList);
		m_closedList.reset();
	}

#if RETAIL_COMPATIBLE_PATHFINDING
	// TheSuperHackers @info this is here and performs the same function as the above block, but for when the crash occurs within the closed list.
	// If the crash mode within PathfindCell::releaseClosedList is hit, it will set s_forceCleanCells to allow the system to cleanly recover.
	if (s_forceCleanCells) {
		forceCleanCells();
		s_forceCleanCells = false;
	}
#endif

	m_cumulativeCellsAllocated += count;
}


//
// Return true if we can move onto this position
//
Bool Pathfinder::validMovementPosition( Bool isCrusher, LocomotorSurfaceTypeMask acceptableSurfaces,
																			 PathfindCell *toCell, PathfindCell *fromCell )
{
	if (toCell == nullptr)
		return false;

	// check if the destination cell is classified as an obstacle,
	// and we happen to be ignoring it
	if (toCell->isObstaclePresent( m_ignoreObstacleID ))
		return true;

	if (isCrusher && toCell->isObstacleFence()) {
		return true;
	}

	// check validity of destination cell
	LocomotorSurfaceTypeMask cellSurfaces = validLocomotorSurfacesForCellType(toCell->getType());
	if ((cellSurfaces & acceptableSurfaces) == 0)
		return false;

	return true;
}

/**
 * Checks to see if obj can occupy the pathfind cell at x,y.
 * Returns false if there is another unit's goal already there.
 * Assumes your locomotor already said you can go there.
 */
Bool Pathfinder::checkDestination(const Object *obj, Int cellX, Int cellY, PathfindLayerEnum layer, Int iRadius, Bool centerInCell)
{
	// If obj==nullptr, means we are checking for any ground units present.  jba.
	Int numCellsAbove = iRadius;
	if (centerInCell) numCellsAbove++;
	Bool checkForAircraft = false;
	Int i, j;
	ObjectID ignoreId = INVALID_ID;
	ObjectID objID = INVALID_ID;
	if (obj && obj->getAIUpdateInterface()) {
		ignoreId =  obj->getAIUpdateInterface()->getIgnoredObstacleID();
		checkForAircraft = obj->getAI()->isAircraftThatAdjustsDestination();
		objID = obj->getID();
	}
	for (i=cellX-iRadius; i<cellX+numCellsAbove; i++) {
		for (j=cellY-iRadius; j<cellY+numCellsAbove; j++) {
			PathfindCell	*cell = getCell(layer, i, j);
			if (!cell) {
				return false; // off the map, so can't place here.
			}

			if (checkForAircraft) {
				if (!cell->isAircraftGoal()) {
					continue;
				}
				if (cell->getGoalAircraft() == objID) {
					continue;
				}
				return false;
			}

			if (cell->getType()==PathfindCell::CELL_OBSTACLE) {
				if (cell->isObstaclePresent( ignoreId ))
					continue;
				return false;
			}

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
			if (IS_IMPASSABLE(cell->getType())) {
				return false;
			}
#endif

			if (cell->getFlags() == PathfindCell::NO_UNITS) {
				continue;  // Nobody is here, so it's ok.
			}

			ObjectID goalUnitID = cell->getGoalUnit();
			if (goalUnitID == objID) {
				continue; // we got it.
			}

			if (goalUnitID == ignoreId) {
				continue; // we are ignoring it.
			}

			if (goalUnitID == INVALID_ID) {
				continue;
			}

			if (!obj) {
				return false;
			}
			Object *unit = TheGameLogic->findObjectByID(goalUnitID);
			if (!unit) {
				continue;
			}

			// order matters: we want to know if I consider it to be an ally, not vice versa
			if (obj->getRelationship(unit) == ALLIES) {
				return false; 	// Don't usurp your allies goals.  jba.
			}
			if (cell->getFlags()==PathfindCell::UNIT_PRESENT_FIXED) {
				Bool canCrush = obj->canCrushOrSquish(unit, TEST_CRUSH_OR_SQUISH);
				if (!canCrush) {
					return false; // Don't move to an occupied cell.
				}
			}
		}
	}
	return true;
}

/**
 * Checks to see if obj can move through the pathfind cell at x,y.
 * Returns false if there are other units already there.
 * Assumes your locomotor already said you can go there.
 */
Bool Pathfinder::checkForMovement(const Object *obj, TCheckMovementInfo &info)
{
	Int numCellsAbove = info.radius;
	if (info.centerInCell) numCellsAbove++;
	return checkForMovementInRect(obj, info,
		info.cell.x-info.radius, info.cell.x+numCellsAbove,
		info.cell.y-info.radius, info.cell.y+numCellsAbove);
}

// GeneralsX: The body of checkForMovement, unchanged except that the
// footprint loop bounds are parameters. checkForMovement passes exactly the bounds it used to
// compute, so every existing caller runs the same cells in the same order.
Bool Pathfinder::checkForMovementInRect(const Object *obj, TCheckMovementInfo &info, Int x0, Int x1, Int y0, Int y1, Bool wantAllyMoving)
{
	info.allyFixedCount = 0;
	info.allyMoving = false;
	info.allyGoal = false;
	info.enemyFixed = false;

	const Int		maxAlly = 5;
	ObjectID		allies[maxAlly];
	Int					numAlly = 0;

	if (!obj) {
		return true; // not object can move there.
	}

	// GeneralsX: PF27 fast scan. Most footprints hold no unit at all, and for those the loop
	// below only `continue`s on every cell and returns true with all four outputs as cleared above. On the ground layer,
	// with the whole rectangle on the map (so getCell returns &m_map[i][j] for every cell and none is null), read the
	// flags straight off the rows: no unit anywhere -> that same answer, without the two AI-interface calls and the
	// per-cell getCell. Any other case, or any flagged cell, runs the unchanged loop. Under GX_PATHTRACE (verify) every
	// fast answer is checked against the loop (s_gxFastBad, never reset).
	const Bool fastEligible = info.layer == LAYER_GROUND && x0 < x1 && y0 < y1 &&
		x0 >= m_extent.lo.x && x1 - 1 <= m_extent.hi.x && y0 >= m_extent.lo.y && y1 - 1 <= m_extent.hi.y;
	Bool fastEmpty = false;
	if (fastEligible)
	{
		fastEmpty = true;
		for (Int fi = x0; fi < x1 && fastEmpty; ++fi)
		{
			const PathfindCell *row = m_map[fi];
			for (Int fj = y0; fj < y1; ++fj)
			{
				if (row[fj].getFlags() != PathfindCell::NO_UNITS)
				{
					fastEmpty = false;
					break;
				}
			}
		}
		if (fastEmpty && !gxPathTraceVerify())
			return true;
	}

	ObjectID ignoreId = INVALID_ID;
	if (obj->getAIUpdateInterface()) {
		ignoreId =  obj->getAIUpdateInterface()->getIgnoredObstacleID();
	}

	// GeneralsX: Both of these were being re-fetched inside the
	// cell loop below. getID() is cheap but getAIUpdateInterface()->getIgnoredObstacleID() is the
	// same two-call sequence already done immediately above, and it showed up in the profile.
	const ObjectID objID = obj->getID();

	Int i, j;
//	Bool isInfantry = obj->isKindOf(KINDOF_INFANTRY);
	for (i=x0; i<x1; i++) {
		for (j=y0; j<y1; j++) {
			PathfindCell	*cell = getCell(info.layer,i, j);
			if (!cell) {
				return false; // off the map, so can't move here.
			}

			PathfindCell::CellFlags flags = cell->getFlags();
			if ((flags == PathfindCell::UNIT_GOAL) || (flags == PathfindCell::UNIT_GOAL_OTHER_MOVING)) {
				info.allyGoal = true;
			} else if (flags == PathfindCell::NO_UNITS) {
				continue;  // Nobody is here, so it's ok.
			}

			ObjectID posUnit = cell->getPosUnit();
			if (posUnit == objID) {
				continue; // we got it.
			}

			if (posUnit == ignoreId) {
				continue; // we are ignoring this one.
			}

			Bool check = false;
			Object *unit = nullptr;
			if (flags == PathfindCell::UNIT_PRESENT_MOVING || flags == PathfindCell::UNIT_GOAL_OTHER_MOVING) {
				// GeneralsX: Without considerTransient a moving unit's
				// cell reaches the `!check` continue below with only allyMoving set, and the line
				// walk (the caller that passes wantAllyMoving=false) never reads allyMoving. Every
				// call skipped here is a const read: findObjectByID and Object/Team/Player
				// getRelationship write nothing.
				if (!wantAllyMoving && !info.considerTransient) {
					continue;
				}
				unit = TheGameLogic->findObjectByID(posUnit);
				// order matters: we want to know if I consider it to be an ally, not vice versa
				if (wantAllyMoving && unit && obj->getRelationship(unit) == ALLIES) {
					info.allyMoving = true;
				}
				if (info.considerTransient) {
					check = true;
				}
			}
			if (flags == PathfindCell::UNIT_PRESENT_FIXED) {
				check = true;
				unit = TheGameLogic->findObjectByID(posUnit);
			}
			if (check && unit!=nullptr) {
				// GeneralsX: ignoreId is the same value this used to
				// re-fetch. The INVALID_ID guard keeps the two forms identical even for an object
				// whose own ID is INVALID_ID, which the original's && would have rejected.
				if (ignoreId != INVALID_ID && ignoreId == unit->getID()) {
					// Don't check if it's the ignored obstacle.
					check = false;
				}
			}
			if (!check || !unit) {
				continue;
			}

#ifdef INFANTRY_MOVES_THROUGH_INFANTRY
			if (obj->isKindOf(KINDOF_INFANTRY) && unit->isKindOf(KINDOF_INFANTRY)) {
				// Infantry can run through infantry.
				continue; //
			}
#endif
			// See if it is an ally.
			// order matters: we want to know if I consider it to be an ally, not vice versa
			if (obj->getRelationship(unit) == ALLIES) {
				if (!unit->getAIUpdateInterface()) {
					return false; // can't path through not-idle units.
				}
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				if (!unit->getAIUpdateInterface()->isIdle()) {
					return false; // can't path through not-idle units.
				}
#endif
				Bool found = false;
				Int k;
				for (k=0; k<numAlly; k++) {
					if (allies[k] == unit->getID()) {
						found = true;
					}
				}
				if (!found) {
					info.allyFixedCount++;
					if (numAlly < maxAlly) {
						allies[numAlly] = unit->getID();
						numAlly++;
					}
				}
			} else {
				Bool canCrush = obj->canCrushOrSquish( unit, TEST_CRUSH_OR_SQUISH );
				if (!canCrush) {
					info.enemyFixed = true;
				}
			}
		}
	}
	if (fastEmpty && (info.allyFixedCount != 0 || info.allyMoving || info.allyGoal || info.enemyFixed))
		++s_gxFastBad;	// GeneralsX PF27: the fast scan would have answered differently (verify only)
	return true;
}

// ============================================================================================
// GeneralsX: checkForMovement, remembered for one A* search.
//
// WHY THIS IS SOUND, stated as the thing that would have to be false for it to be wrong.
//
// checkForMovement reads: the cell flags of the (2r+1)^2 block around info.cell, the pos-unit
// recorded in those cells, and - only for cells that report a unit - that unit's relationship
// to obj and whether obj can crush it. It writes nothing. Its answer is therefore a function of
// (obj, radius, centerInCell, considerTransient, acceptableSurfaces, layer, cell) and of world
// state. The first five are fixed for the whole of one search; layer and cell are the key.
//
// World state does not move inside a search. The A* loop allocates PathfindCellInfos, links
// cells onto the open and closed lists, sets m_blockedByAlly and clears m_isTunneling. None of
// those are read here. Cell FLAGS and the pos/goal unit IDs are written by
// classifyObjectFootprint and by Pathfinder::updatePos/updateGoal, which run in the game logic
// update, never inside a search. The one case that looks like an exception is a cell gaining a
// PathfindCellInfo mid-search: getPosUnit() reads through m_info, so it changes from INVALID_ID
// to m_info->m_posUnitID - and getACellInfo initialises m_posUnitID to INVALID_ID, so the answer
// does not move. A cell that really has a unit on it always had an info already.
//
// The generation is bumped by cleanOpenAndClosedLists(), which every search calls on every exit
// path, so an entry can only ever be read by the search that wrote it. A nested search bumps it
// too, which throws the outer search's memo away - a loss of speed, never of correctness.
//
// NONE OF THAT IS THE EVIDENCE. The evidence is s_gxCfmBad: while GX_PATHTRACE is set this
// function computes the answer fresh EVERY time and only compares it with what it remembered,
// so a single mismatch anywhere in a match falsifies the paragraphs above. Measured over one
// 12000-frame 8-player match: 766,836,987 calls, 753,177,059 of them repeats, 0 mismatches.
// The second, independent check is the one that actually gates shipping - the same replay run
// against the unpatched binary, [GXCRC] streams compared row for row.
//
// GX_PATHTRACE therefore makes this function SLOWER, not faster, and that is deliberate: the
// verifying arm and the serving arm must not be the same code path, or the check checks itself.
// ============================================================================================

Bool Pathfinder::checkForMovementInSearch(const Object *obj, TCheckMovementInfo &info)
{
	const Bool trace = gxPathTraceVerify();
	if (trace) ++s_gxCfmCalls;

	// The grid is allocated once per map; mirror its dimensions. m_extent.lo is not always 0, so
	// index on the same (x, y) the cells themselves use and simply skip anything out of range.
	if (s_gxMoveMemo == nullptr && m_map != nullptr)
	{
		s_gxMoveMemoStrideY = m_extent.hi.y + 1;
		s_gxMoveMemoCells   = (UnsignedInt)(m_extent.hi.x + 1) * (UnsignedInt)s_gxMoveMemoStrideY;
		s_gxMoveMemo        = MSGNEW("PathfindMoveMemo") GXMoveMemoEntry[s_gxMoveMemoCells];
		memset(s_gxMoveMemo, 0, sizeof(GXMoveMemoEntry) * s_gxMoveMemoCells);
	}

	// A change in any part of the key that is not the cell starts a new generation. Inside one
	// search this never fires; it is here so that it cannot be wrong if that ever stops holding.
	if (obj != s_gxMoveObj || info.radius != s_gxMoveRadius || info.centerInCell != s_gxMoveCenter ||
	    info.considerTransient != s_gxMoveTransient || (UnsignedInt)info.acceptableSurfaces != s_gxMoveSurfaces)
	{
		++s_gxMoveGen;
		s_gxMoveObj       = obj;
		s_gxMoveRadius    = info.radius;
		s_gxMoveCenter    = info.centerInCell;
		s_gxMoveTransient = info.considerTransient;
		s_gxMoveSurfaces  = (UnsignedInt)info.acceptableSurfaces;
	}

	GXMoveMemoEntry *entry = nullptr;
	if (s_gxMoveMemo != nullptr &&
	    info.cell.x >= 0 && info.cell.x <= m_extent.hi.x &&
	    info.cell.y >= 0 && info.cell.y <= m_extent.hi.y)
	{
		entry = &s_gxMoveMemo[(UnsignedInt)info.cell.x * (UnsignedInt)s_gxMoveMemoStrideY + (UnsignedInt)info.cell.y];
	}

	const Bool hit = (entry != nullptr && entry->m_gen == s_gxMoveGen && entry->m_layer == (UnsignedByte)info.layer);

	if (hit && !trace)
	{
		// Serve the remembered answer. checkForMovement clears all four outputs before it starts,
		// so every one of them has to be written back here, not just the ones that were set.
		const UnsignedByte out = entry->m_out;
		info.allyFixedCount = entry->m_allyFixedCount;
		info.allyMoving     = (out & GXMOVE_ALLY_MOVING) != 0;
		info.allyGoal       = (out & GXMOVE_ALLY_GOAL) != 0;
		info.enemyFixed     = (out & GXMOVE_ENEMY_FIXED) != 0;
		return (out & GXMOVE_RESULT) != 0;
	}

	const Bool result = checkForMovement(obj, info);

	const UnsignedByte packed =
		(UnsignedByte)((result ? GXMOVE_RESULT : 0) |
		               (info.allyMoving ? GXMOVE_ALLY_MOVING : 0) |
		               (info.allyGoal ? GXMOVE_ALLY_GOAL : 0) |
		               (info.enemyFixed ? GXMOVE_ENEMY_FIXED : 0));

	// allyFixedCount is bounded by the number of cells in the footprint block, so it fits; the
	// test is here so that "it fits" is enforced rather than assumed. A value that did not fit
	// simply is not remembered, and the next asker recomputes it.
	const Bool fits = (info.allyFixedCount >= 0 && info.allyFixedCount <= 65535);

	if (hit && trace)
	{
		++s_gxCfmRepeats;
		if (entry->m_out != packed ||
		    (fits && entry->m_allyFixedCount != (UnsignedShort)info.allyFixedCount))
			++s_gxCfmBad;
	}

	if (entry != nullptr && fits)
	{
		entry->m_gen            = s_gxMoveGen;
		entry->m_layer          = (UnsignedByte)info.layer;
		entry->m_out            = packed;
		entry->m_allyFixedCount = (UnsignedShort)info.allyFixedCount;
	}
	else if (entry != nullptr)
	{
		entry->m_gen = 0;	// never serve a truncated answer
	}

	return result;
}

/**
 * Adjusts a coordinate to the center of it's cell.
 */
// Snaps the current position to it's grid location.
void Pathfinder::snapPosition(Object *obj, Coord3D *pos)
{
	Int iRadius;
	Bool center;
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell;
	Coord3D adjustDest = *pos;
	if (!center) {
		adjustDest.x += PATHFIND_CELL_SIZE_F/2;
		adjustDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	worldToCell( &adjustDest, &cell );
	adjustCoordToCell(cell.x, cell.y,  center, *pos, LAYER_GROUND);
}

/**
 * Adjusts a goal position to the center of it's cell.
 */
// Snaps the current position to it's grid location.
void Pathfinder::snapClosestGoalPosition(Object *obj, Coord3D *pos)
{
	Int iRadius;
	Bool center;
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell;
	Coord3D adjustDest = *pos;
	if (!center) {
		adjustDest.x += PATHFIND_CELL_SIZE_F/2;
		adjustDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(pos);
	worldToCell( &adjustDest, &cell );
	adjustCoordToCell(cell.x, cell.y,  center, *pos, LAYER_GROUND);
	if (checkDestination(obj, cell.x, cell.y , layer, iRadius, center)) {
		return;
	}

	// Try adjusting by 1.
	Int i,j;
	for (i = cell.x - 1; i < cell.x + 2; i++) {
		for (j = cell.y - 1; j < cell.y + 2; j++) {
			if (checkDestination(obj, i, j, layer, iRadius, center)) {
				adjustCoordToCell(i, j, center, *pos, layer);
				return;
			}
		}
	}

	if (iRadius > 0)
		return;

	// Try to find an unoccupied cell.
	for (i = cell.x - 1; i < cell.x + 2; i++) {
		for (j = cell.y - 1; j < cell.y + 2; j++) {
			PathfindCell* newCell = getCell(layer, i, j);
			if (!newCell)
				continue;

			if (newCell->getGoalUnit() == INVALID_ID || newCell->getGoalUnit() == obj->getID()) {
				adjustCoordToCell(i, j, center, *pos, layer);
				return;
			}
		}
	}

	for (i = cell.x - 1; i < cell.x + 2; i++) {
		for (j = cell.y - 1; j < cell.y + 2; j++) {
			PathfindCell* newCell = getCell(layer, i, j);
			if (!newCell)
				continue;

			if (newCell->getFlags()!=PathfindCell::UNIT_PRESENT_FIXED) {
				adjustCoordToCell(i, j, center, *pos, layer);
				return;
			}
		}
	}
}

/**
 * Returns coordinates of goal.
 *
 */
Bool Pathfinder::goalPosition(Object *obj, Coord3D *pos)
{
	Int iRadius;
	Bool center;
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai) return false; // only consider ai objects.
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell = *ai->getPathfindGoalCell();
	pos->zero();
	if (cell.x<0 || cell.y<0) return false;
	adjustCoordToCell(cell.x, cell.y,  center, *pos, LAYER_GROUND);
	return true;
}


Bool Pathfinder::checkForAdjust(Object *obj, const LocomotorSet& locomotorSet, Bool isHuman,
																Int cellX, Int cellY, PathfindLayerEnum layer,
																Int iRadius, Bool center, Coord3D *dest, const Coord3D *groupDest,
																Bool *passedDestination)
{
	Coord3D adjustDest;
	PathfindCell *cellP = getCell(layer, cellX, cellY);
	if (cellP==nullptr) return false;
	if (cellP && cellP->getType() == PathfindCell::CELL_CLIFF) {
		return false;  // no final destinations on cliffs.
	}
	if (isHuman) {
		// check if new cell is in logical map.	(computer can move off logical map)
		if (cellX < m_logicalExtent.lo.x ||
				cellY < m_logicalExtent.lo.y ||
				cellX > m_logicalExtent.hi.x ||
				cellY > m_logicalExtent.hi.y) return false;
	}
	if (checkDestination(obj, cellX, cellY, layer, iRadius, center)) {
		if (passedDestination)
			*passedDestination = TRUE;
		adjustCoordToCell(cellX, cellY,  center, adjustDest, cellP->getLayer());
		Bool adjustedPathExists;
		if (obj->isKindOf(KINDOF_AIRCRAFT)) {
			adjustedPathExists = true;
		}	else {
			Bool pathExists = clientSafeQuickDoesPathExist( locomotorSet, obj->getPosition(), dest);
			if (!pathExists && clientSafeQuickDoesPathExist( locomotorSet, dest, &adjustDest)) {
				adjustedPathExists = true;
			} else {
				adjustedPathExists = clientSafeQuickDoesPathExist( locomotorSet, obj->getPosition(), &adjustDest);
			}
		}
		if ( adjustedPathExists	) {
			if (groupDest) {
				tightenPath(obj, locomotorSet, &adjustDest, groupDest);
				// Check to see if it is a long way to get to the adjusted destination.
				Int cost = checkPathCost(obj, locomotorSet, groupDest, &adjustDest);
				Int dx = IABS(groupDest->x-adjustDest.x);
				Int dy = IABS(groupDest->y-adjustDest.y);
				if (1.4f*(dx+dy)<cost) {
					return false;
				}
			}
			*dest = adjustDest;
			return true;
		}
	}
	return false;
}

Bool Pathfinder::checkForLanding(Int cellX, Int cellY, PathfindLayerEnum layer,
																Int iRadius, Bool center, Coord3D *dest)
{
	Coord3D adjustDest;
	PathfindCell *cellP = getCell(layer, cellX, cellY);
	if (cellP==nullptr) return false;
	switch (cellP->getType())
	{
		case PathfindCell::CELL_CLIFF:
		case PathfindCell::CELL_WATER:
		case PathfindCell::CELL_IMPASSABLE:
			return false;  // no final destinations on cliffs, water, etc.
	}
	if (checkDestination(nullptr, cellX, cellY, layer, iRadius, center)) {
		adjustCoordToCell(cellX, cellY,  center, adjustDest, cellP->getLayer());
		*dest = adjustDest;
		return true;
	}
	return false;
}

/**
 * Find an unoccupied spot for a unit to land at.
 * Returns false if there are no spots available within a reasonable radius.
 */
Bool Pathfinder::adjustToLandingDestination(Object *obj, Coord3D *dest)
{
	Int iRadius;
	Bool center;
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell;
	Coord3D adjustDest = *dest;

	Region3D extent;
	TheTerrainLogic->getMaximumPathfindExtent(&extent);
	// If the object is off the map & the goal is off the map, it is a scripted setup, so just
	// go to the dest.
	if (!extent.isInRegionNoZ(*dest)) {
		if (!extent.isInRegionNoZ(*obj->getPosition())) {
			return true;
		}
	}

	if (!center) {
		adjustDest.x += PATHFIND_CELL_SIZE_F/2;
		adjustDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	worldToCell( &adjustDest, &cell );

	Int limit = MAX_ADJUSTMENT_CELL_COUNT;
	Int i, j;
	i = cell.x;
	j = cell.y;
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(dest);
	if (checkForLanding(i,j, layer, iRadius, center, dest)) {
		return true;
	}

	Int delta=1;
	Int count;
	while (limit>0) {
		for (count = delta; count>0; count--) {
			i++;
			limit--;
			if (checkForLanding(i,j, layer, iRadius, center, dest)) {
				return true;
			}
		}
		for (count = delta; count>0; count--) {
			j++;
			limit--;
			if (checkForLanding(i,j, layer, iRadius, center, dest)) {
				return true;
			}
		}
		delta++;
		for (count = delta; count>0; count--) {
			i--;
			limit--;
			if (checkForLanding(i,j, layer, iRadius, center, dest)) {
				return true;
			}
		}
		for (count = delta; count>0; count--) {
			j--;
			limit--;
			if (checkForLanding(i,j, layer, iRadius, center, dest)) {
				return true;
			}
		}
		delta++;
	}
	return false;
}


/**
 * Find an unoccupied spot for a unit to move to.
 * Returns false if there are no spots available within a reasonable radius.
 */
#if defined(GX_TEST_INSTRUMENTS)
// GeneralsX: (H03) GX_H03_CHECK=1: run the fallback adjustDestination skips anyway and report whether it
// would have found a destination or moved *dest. A test switch: with the claim above true it changes nothing; a mismatch
// line means the skip changed the game. Counts every 1,000 skips and every mismatch at once.
// Test builds only (-DGX_TEST_INSTRUMENTS=ON; 29/10/2026).
static void gxH03CheckSkippedFallback(Pathfinder *pf, Object *obj, const LocomotorSet &locomotorSet, const Coord3D *dest)
{
	static const Bool s_on = getenv("GX_H03_CHECK") != nullptr && getenv("GX_H03_CHECK")[0] == '1';
	if (!s_on)
		return;
	static UnsignedInt s_skipped = 0, s_bad = 0;
	Coord3D probe = *dest;
	const Bool found = pf->adjustDestination(obj, locomotorSet, &probe, nullptr);
	++s_skipped;
	if (found || probe.x != dest->x || probe.y != dest->y || probe.z != dest->z)
	{
		++s_bad;
		fprintf(stderr, "[GXH03] MISMATCH f=%u obj=%u found=%d dest=%.2f,%.2f probe=%.2f,%.2f\n", TheGameLogic->getFrame(),
			obj ? (UnsignedInt)obj->getID() : 0u, (Int)found, dest->x, dest->y, probe.x, probe.y);
	}
	if (s_skipped % 1000 == 0 || found)
		fprintf(stderr, "[GXH03] skipped=%u mismatches=%u\n", s_skipped, s_bad);
}
#endif // GX_TEST_INSTRUMENTS

Bool Pathfinder::adjustDestination(Object *obj, const LocomotorSet& locomotorSet, Coord3D *dest, const Coord3D *groupDest)
{
	if( obj->isKindOf(KINDOF_PROJECTILE) )
	{
		return true; // missiles can go wherever they want to. jba.
	}

	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}
	Int iRadius;
	Bool center;
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell;
	Coord3D cellDest = *dest;
	if (!center) {
		cellDest.x += PATHFIND_CELL_SIZE_F/2;
		cellDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	worldToCell( &cellDest, &cell );
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(dest);
	if (groupDest) {
		layer = TheTerrainLogic->getLayerForDestination(groupDest);
	}

	Int i = cell.x;
	Int j = cell.y;
	// GeneralsX: (H03) Did ANY cell pass checkDestination? See the fallback below.
	Bool anyPassedDestination = FALSE;
	// Check the center cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (checkForAdjust(obj, locomotorSet, isHuman, i, j, layer, iRadius, center, dest, groupDest, &anyPassedDestination)) {
		return true;
	}
#else
	Coord3D adjustDest = *dest;
	if (checkForAdjust(obj, locomotorSet, isHuman, i, j, layer, iRadius, center, &adjustDest, groupDest, &anyPassedDestination)) {
		// TheSuperHackers @bugfix stephanmeesters 15/06/2026 Destination adjustment always snaps to the nearest grid cell
		// even when no adjustment is necessary because there are no obstructions. For single units this adjustment
		// can be skipped in order to provide more accurate movement, which is especially noticeable for chinooks.
		const Bool singleUnit = obj && obj->getGroup() && obj->getGroup()->getCount() == 1;
		const Bool useExactDestination = isHuman && singleUnit;
		if (!useExactDestination) {
			*dest = adjustDest;
		}
		return true;
	}
#endif

	// TheSuperHackers @info Expanding counter-clockwise spiral search around center cell C. Each full lap walks right->up->left->down.
	// After every pair of directions (right+up, then left+down) length of the segment grows by 1.
	//
	//    <------  12
	//    4  3  2  11
	//    5  C  1  10
	//    6  7  8  9
	//
	Int limit = MAX_ADJUSTMENT_CELL_COUNT;
	Int segmentLength = 1;
	const ICoord2D directions[4] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1} };
	while (limit>0) {
		for (Int dir = 0; dir < 4; dir++) {
			for (Int count = segmentLength; count>0; count--) {
				i+=directions[dir].x;
				j+=directions[dir].y;
				limit--;
				if (checkForAdjust(obj, locomotorSet, isHuman, i, j, layer, iRadius, center, dest, groupDest, &anyPassedDestination)) {
					return true;
				}
			}
			if (dir & 1) {
				segmentLength++;
			}
		}
	}

	if (groupDest) {
		// GeneralsX: (H03) THE FALLBACK BELOW CANNOT FIND WHAT THIS PASS DID NOT, when no cell passed
		// checkDestination and it would search the same layer. It walks the same centre cell and the same 400-cell spiral from
		// the same *dest (checkForAdjust writes *dest only on success; the centre check above uses a copy) with the same
		// isHuman/iRadius/center, and asks checkDestination the same questions of the same state: every call here failed at or
		// before checkDestination, which only reads (the cell, its goal unit, relationships), so nothing changed; the
		// side-effecting tail - the path-exists checks, tightenPath, checkPathCost's cell infos and m_cumulativeCellsAllocated -
		// runs only after a pass. So the fallback returns false and changes nothing: return that. Measured (Mini, 400 tanks
		// ordered to open ground, probe/h03-massorder): 310 of 400 members ran both spirals, ~248k of 281k checkForAdjust
		// calls. GX_H03_CHECK=1 (test builds) still runs the skipped fallback and reports any disagreement ([GXH03]).
		if (!anyPassedDestination && TheTerrainLogic->getLayerForDestination(dest) == layer)
		{
#if defined(GX_TEST_INSTRUMENTS)
			gxH03CheckSkippedFallback(this, obj, locomotorSet, dest);
#endif
			return false;
		}
		// Didn't work, so just do simple adjust.
		return(adjustDestination(obj, locomotorSet, dest, nullptr));
	}
	return false;
}

Bool Pathfinder::checkForTarget(const Object *obj, 	Int cellX, Int cellY, const Weapon *weapon,
																const Object *victim, const Coord3D *victimPos,
																Int iRadius, Bool center,Coord3D *dest)
{
	Coord3D adjustDest;
	if (checkDestination(obj, cellX, cellY, LAYER_GROUND, iRadius, center)) {
		adjustCoordToCell(cellX, cellY,  center, adjustDest, LAYER_GROUND);
		if (weapon->isGoalPosWithinAttackRange( obj, &adjustDest, victim, victimPos ))	{
			*dest = adjustDest;
			return true;
		}
	}
	return false;
}

/**
 * Find an unoccupied spot for a unit to move to that can fire at victim.
 * Returns false if there are no spots available within a reasonable radius.
 */
Bool Pathfinder::adjustTargetDestination(const Object *obj, const Object *target, const Coord3D *targetPos,
																				 const Weapon *weapon, Coord3D *dest)
{
	Int iRadius;
	Bool center;
	getRadiusAndCenter(obj, iRadius, center);
	ICoord2D cell;
	Coord3D adjustDest = *dest;
	if (!center) {
		adjustDest.x += PATHFIND_CELL_SIZE_F/2;
		adjustDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	if (worldToCell( &adjustDest, &cell )) {
		return false; // outside of bounds.
	}

	Int limit = MAX_ADJUSTMENT_CELL_COUNT;
	Int i, j;
	i = cell.x;
	j = cell.y;
	if (checkForTarget(obj, i,j, weapon, target, targetPos, iRadius, center, dest)) {
		return true;
	}

	Int delta=1;
	Int count;
	while (limit>0) {
		for (count = delta; count>0; count--) {
			i++;
			limit--;
			if (checkForTarget(obj, i,j, weapon, target, targetPos, iRadius, center, dest)) {
				return true;
			}
		}
		for (count = delta; count>0; count--) {
			j++;
			limit--;
			if (checkForTarget(obj, i,j, weapon, target, targetPos, iRadius, center, dest)) {
				return true;
			}
		}
		delta++;
		for (count = delta; count>0; count--) {
			i--;
			limit--;
			if (checkForTarget(obj, i,j, weapon, target, targetPos, iRadius, center, dest)) {
				return true;
			}
		}
		for (count = delta; count>0; count--) {
			j--;
			limit--;
			if (checkForTarget(obj, i,j, weapon, target, targetPos, iRadius, center, dest)) {
				return true;
			}
		}
		delta++;
	}
	return false;
}

Bool Pathfinder::checkForPossible(Bool isCrusher, Int fromZone,  Bool center, const LocomotorSet& locomotorSet,
																	Int cellX, Int cellY, PathfindLayerEnum layer, Coord3D *dest, Bool startingInObstacle)
{
	PathfindCell *goalCell = getCell(layer, cellX, cellY);
	if (!goalCell) return false;
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
	if (goalCell->getType() == PathfindCell::CELL_OBSTACLE) return false;
#else
	if (IS_IMPASSABLE(goalCell->getType())) return false;
#endif
	Int zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, goalCell->getZone());
	if (startingInObstacle) {
		zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
	}
	if (fromZone==zone2) {
		adjustCoordToCell(cellX, cellY,  center, *dest, layer);
		return true;
	}
	return false;
}

/**
 * Find a pathable spot near the destination.
 * Returns false if there are no spots available within a reasonable radius.
 */
Bool Pathfinder::adjustToPossibleDestination(Object *obj, const LocomotorSet& locomotorSet,
																						 Coord3D *dest)
{
	Int radius;
	Bool center;
	getRadiusAndCenter(obj, radius, center);
	ICoord2D goalCellNdx;
	Coord3D adjustDest = *dest;
	if (!center) {
		adjustDest.x += PATHFIND_CELL_SIZE_F/2;
		adjustDest.y += PATHFIND_CELL_SIZE_F/2;
	}
	if (worldToCell( &adjustDest, &goalCellNdx )) {
		return false; // outside of bounds.
	}

	// determine goal cell
	PathfindCell *goalCell;
	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(dest);

	goalCell = getCell(destinationLayer, goalCellNdx.x, goalCellNdx.y);


	Coord3D from = *obj->getPosition();

	// determine start cell
	ICoord2D startCellNdx;
	worldToCell(&from, &startCellNdx);
	PathfindLayerEnum layer = LAYER_GROUND;
	if (obj) {
		layer = obj->getLayer();
	}
	PathfindCell *parentCell = getClippedCell( layer, &from );
	if (parentCell == nullptr) {
		return false;
	}

	Int zone1, zone2;
	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;
	zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, parentCell->getZone());
	Bool isObstacle = false;
	if (parentCell->getType() == PathfindCell::CELL_OBSTACLE)	{
		isObstacle = true;
	}
	if (isObstacle) {
		zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
		zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, zone1);
	}

	zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, goalCell->getZone());

	if (zone1 == zone2) {
		if (checkDestination(obj, goalCellNdx.x, goalCellNdx.y, destinationLayer, radius, center)) {
			return true;
		}
	}

	Int limit = MAX_ADJUSTMENT_CELL_COUNT;
	Int i, j;
	i = goalCellNdx.x;
	j = goalCellNdx.y;

	Int delta=1;
	Int count;
	while (limit>0) {
		for (count = delta; count>0; count--) {
			i++;
			limit--;
			if (checkForPossible(isCrusher, zone1, center, locomotorSet, i,j, destinationLayer, dest, isObstacle)) {
				if (checkDestination(obj, i, j, destinationLayer, radius, center)) {
					return true;
				}
			}
		}
		for (count = delta; count>0; count--) {
			j++;
			limit--;
			if (checkForPossible(isCrusher, zone1, center, locomotorSet, i,j, destinationLayer, dest, isObstacle)) {
				if (checkDestination(obj, i, j, destinationLayer, radius, center)) {
					return true;
				}
			}
		}
		delta++;
		for (count = delta; count>0; count--) {
			i--;
			limit--;
			if (checkForPossible(isCrusher, zone1, center, locomotorSet, i,j, destinationLayer, dest, isObstacle)) {
				if (checkDestination(obj, i, j, destinationLayer, radius, center)) {
					return true;
				}
			}
		}
		for (count = delta; count>0; count--) {
			j--;
			limit--;
			if (checkForPossible(isCrusher, zone1, center, locomotorSet, i,j, destinationLayer, dest, isObstacle)) {
				if (checkDestination(obj, i, j, destinationLayer, radius, center)) {
					return true;
				}
			}
		}
		delta++;
	}
	return false;
}


/**
 * Queues an object to do a pathfind.
 * It will call the object's ai update->doPathfind() during processPathfindQueue().
 */
Bool Pathfinder::queueForPath(ObjectID id)
{
#ifdef DEBUG_LOGGING
	{
		Object *tmpObj = TheGameLogic->findObjectByID(id);
		if (tmpObj) {
			AIUpdateInterface *tmpAI = tmpObj->getAIUpdateInterface();
			if (tmpAI) {
				const Coord3D* pos = tmpAI->friend_getRequestedDestination();
				DEBUG_ASSERTLOG(pos->x != 0.0 && pos->y != 0.0, ("Queueing pathfind to (0, 0), usually a bug. (Unit Name: '%s', Type: '%s')", tmpObj->getName().str(), tmpObj->getTemplate()->getName().str()));
			}
		}
	}
#endif

	/* Check & see if we are already queued. */
	Int slot = m_queuePRHead;
	while (slot != m_queuePRTail) {
		if (m_queuedPathfindRequests[slot] == id) {
			return true;
		}
		slot++;
		if (slot >= PATHFIND_QUEUE_LEN) {
			slot = 0;
		}
	}

	// Tail is the first available slot.
	Int nextSlot = m_queuePRTail+1;
	if (nextSlot >= PATHFIND_QUEUE_LEN) {
		nextSlot = 0;
	}
	if (nextSlot==m_queuePRHead) {
		// GeneralsX: A FULL QUEUE WAS A PERMANENT, SILENT STALL.
		// Every caller (AIUpdateInterface::requestPath and its three siblings, and the delayed
		// re-queue in AIUpdateInterface::update) ignores this return, and each has already set
		// m_waitingForPath. A refused unit therefore reached POSITION_ON_PATH with no path and
		// returned UPDATE_SLEEP_FOREVER ("Can't move till we get our path"): it slept until the player
		// gave it a NEW order. Order more than PATHFIND_QUEUE_LEN - 1 units at once - a select-all of a
		// few hundred infantry, an army told to force-fire at one aircraft - and the rest never moved.
		// The refused unit now asks again shortly through the engine's own delayed re-queue
		// (setQueueForPathTime, the path the too-quick-repath case already uses), which also wakes a
		// sleeping unit. Every peer runs the same queue, so the retry is deterministic; a game that
		// never fills the queue never reaches this line and simulates exactly as before.
		Object *requester = TheGameLogic->findObjectByID(id);
		AIUpdateInterface *requesterAI = requester ? requester->getAIUpdateInterface() : nullptr;
		if (requesterAI) {
			requesterAI->setQueueForPathTime(GX_FRAMES30(3));
		}
		DEBUG_LOG(("Pathfind queue full - object %d will ask again.", id));
		return false;
	}
	m_queuedPathfindRequests[m_queuePRTail] = id;
	m_queuePRTail = nextSlot;
	return true;
}

#if defined(RTS_DEBUG)
void Pathfinder::doDebugIcons() {
	const Int FRAMES_TO_SHOW_OBSTACLES = 100;
	extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
	// render AI debug information
	if (TheGlobalData->m_debugAI!=AI_DEBUG_CELLS && TheGlobalData->m_debugAI!=AI_DEBUG_TERRAIN) {
		return;
	}

		RGBColor color;
		color.red = color.green = color.blue = 0;
		addIcon(nullptr, 0, 0, color);	 // clear.
		Coord3D topLeftCorner;
		Bool showCells = TheGlobalData->m_debugAI==AI_DEBUG_CELLS;
		Int i;
		for (i=0; i<=LAYER_LAST; i++) {
			m_layers[i].doDebugIcons();
		}
		if (!showCells)	{
			frameToShowObstacles = TheGameLogic->getFrame()+FRAMES_TO_SHOW_OBSTACLES;
			//return;
		}
		// show the pathfind grid
		for( int j=0; j<getExtent()->y; j++ )
		{
			topLeftCorner.y = (Real)j * PATHFIND_CELL_SIZE_F;

			for( int i=0; i<getExtent()->x; i++ )
			{
				topLeftCorner.x = (Real)i * PATHFIND_CELL_SIZE_F;

				color.red = color.green = color.blue = 0;
				Bool empty = true;

				const PathfindCell *cell = TheAI->pathfinder()->getCell( LAYER_GROUND, i, j );
				if (cell)
				{
					switch (cell->getType())
					{
						case PathfindCell::CELL_CLIFF:
							color.red = 1;
							empty = false;
							break;
						case PathfindCell::CELL_BRIDGE_IMPASSABLE:
							color.blue = color.red = 1;
							empty = false;
							break;
						case PathfindCell::CELL_IMPASSABLE:
							color.green = 1;
							empty = false;
							break;

						case PathfindCell::CELL_WATER:
							color.blue = 1;
							empty = false;
							break;

						case PathfindCell::CELL_RUBBLE:
							color.red = 1;
							color.green = 0.5;
							empty = false;
							break;

						case PathfindCell::CELL_OBSTACLE:
							color.red = color.green = 1;
							empty = false;
							break;
						default:
							if (cell->getPinched()) {
								color.blue = color.green = 0.7f;
								empty = false;
							}
							break;
					}
				}
				if (showCells) {
					empty = true;
					color.red = color.green = color.blue = 0;
					if (empty && cell) {
						if (cell->getFlags()!=PathfindCell::NO_UNITS) {
							empty = false;
							if (cell->getFlags() == PathfindCell::UNIT_GOAL) {
								color.red = 1;
							}	else if (cell->getFlags() == PathfindCell::UNIT_PRESENT_FIXED) {
								color.green = color.blue = color.red = 1;
							}	else if (cell->getFlags() == PathfindCell::UNIT_PRESENT_MOVING) {
								color.green = 1;
							}	else {
								color.green = color.red = 1;
							}
						}
						if (cell->isAircraftGoal()) {
							empty = false;
							color.red = 0;
							color.green = color.blue = 1;
						}
					}
				}
				if (!empty) {
					Coord3D loc;
					loc.x = topLeftCorner.x + PATHFIND_CELL_SIZE_F/2.0f;
					loc.y = topLeftCorner.y + PATHFIND_CELL_SIZE_F/2.0f;
					loc.z = TheTerrainLogic->getGroundHeight(loc.x , loc.y);
					addIcon(&loc, PATHFIND_CELL_SIZE_F*0.8f, FRAMES_TO_SHOW_OBSTACLES-1, color);
				}
			}

	}
}
#endif


//-------------------------------------------------------------------------------------------------
/**
 * Create an aircraft path.  Just jogs around tall buildings marked with KINDOF_AIRCRAFT_PATH_AROUND.
 */
Path *Pathfinder::getAircraftPath( const Object *obj, const Coord3D *to )
{
	// for now, quick path objects don't pathfind, generally airborne units
	// build a trivial one-node path containing destination, then avoid buildings.
	Path *thePath = newInstance(Path);
	const AIUpdateInterface *ai = obj->getAI();
	ObjectID avoidObject = INVALID_ID;
	if (ai) {
		avoidObject = ai->getBuildingToNotPathAround();
	}

	// If it is an aircraft that circles (like raptors & migs) we need to adjust the destination
	// to one that doesn't clip buildings.
	Bool checkClips = false;
	if (ai && ai->getCurLocomotor()) {
		if (ai->getCurLocomotor()->getAppearance() == LOCO_WINGS) {
			checkClips = true;
		}
	}

	Real radius = 100;
	Coord3D adjDest = *to;
	if (checkClips) {
		circleClipsTallBuilding(obj->getPosition(), to, radius, avoidObject, &adjDest);
	}
	thePath->prependNode(&adjDest, LAYER_GROUND);
	Coord3D pos = *obj->getPosition();
	pos.z = to->z;
	thePath->prependNode( &pos, LAYER_GROUND );
	Int limit = 20;
	PathNode *curNode = thePath->getFirstNode();
	while (curNode && curNode->getNext()) {
		Coord3D newPos1, newPos2, newPos3;
		if (segmentIntersectsTallBuilding(curNode, curNode->getNext(), avoidObject, &newPos1, &newPos2, &newPos3)) {
			PathNode *newNode3 = newInstance(PathNode);
			newNode3->setPosition( &newPos3 );
			newNode3->setLayer(LAYER_GROUND);
			curNode->append(newNode3);
			PathNode *newNode2 = newInstance(PathNode);
			newNode2->setPosition( &newPos2 );
			newNode2->setLayer(LAYER_GROUND);
			curNode->append(newNode2);
			PathNode *newNode1 = newInstance(PathNode);
			newNode1->setPosition( &newPos1 );
			newNode1->setLayer(LAYER_GROUND);
			curNode->append(newNode1);
			curNode = newNode2;
		}
		curNode = curNode->getNext();
		limit--;
		if (limit<0) break;
	}

	curNode = thePath->getFirstNode();
	while (curNode && curNode->getNext()) {
		curNode->setNextOptimized(curNode->getNext());
		curNode = curNode->getNext();
	}
	thePath->markOptimized();
	if (TheGlobalData->m_debugAI==AI_DEBUG_PATHS) {
		TheAI->pathfinder()->setDebugPath(thePath);
	}

	return thePath;
}


/**
 * Process some path requests in the pathfind queue.
 */
//DECLARE_PERF_TIMER(processPathfindQueue)
void Pathfinder::processPathfindQueue()
{
	//USE_PERF_TIMER(processPathfindQueue)

	// GeneralsX: A* search probe report - see GXPathSearchProbe.
	//
	// Emitted at the TOP of the queue pass, so the window is exactly "since the previous logic
	// frame's pass". That matters: the profile showed AIUpdateInterface::computePath running
	// pathfinding OUTSIDE this queue (22% of the late window), and a report emitted at the
	// bottom would miss all of it. Emitted before the early returns below as well, so a frame
	// is never silently absent from the series - a gap in the series would be indistinguishable
	// from a frame that did no pathfinding, which is exactly the distinction being measured.
	if (gxPathTraceEnabled())
	{
		fprintf(stderr, "[GXPATH] f=%u n=%u cells=%u us=%.0f maxcells=%u maxus=%.0f site=%d"
			" ins=%u walk=%u wmax=%u cap=%u rev=%u lmax=%u corrupt=%u"
			" idx=%u idxwalk=%u idxbad=%u cfm=%u cfmrep=%u cfmbad=%u clr=%u clrhit=%u clrbad=%u fastbad=%u" GX_PATHCOST_FMT "\n",
			TheGameLogic ? (unsigned)TheGameLogic->getFrame() : 0u,
			s_gxPathSearches, s_gxPathCells, s_gxPathUs,
			s_gxPathMaxCells, s_gxPathMaxUs, s_gxPathMaxSite,
			s_gxInsN, s_gxInsSteps, s_gxInsMax, s_gxInsCap, s_gxInsRevSteps, s_gxInsLenMax, s_gxInsCorrupt,
			s_gxIdxUsed, s_gxIdxWalk, s_gxIdxBad,
			s_gxCfmCalls, s_gxCfmRepeats, s_gxCfmBad, s_gxClrCalls, s_gxClrHits, s_gxClrBad, s_gxFastBad GX_PATHCOST_ARGS);
		fflush(stderr);
		s_gxPathSearches = 0;
		s_gxPathCells    = 0;
		s_gxPathUs       = 0.0;
		s_gxPathMaxCells = 0;
		s_gxPathMaxUs    = 0.0;
		s_gxPathMaxSite  = -1;
		s_gxInsN         = 0;
		s_gxInsSteps     = 0;
		s_gxInsMax       = 0;
		s_gxInsCap       = 0;
		s_gxInsRevSteps  = 0;
		s_gxInsLenMax    = 0;
		s_gxInsCorrupt   = 0;
		s_gxIdxUsed      = 0;
		s_gxIdxWalk      = 0;
		// s_gxIdxBad is NOT reset, for the same reason as s_gxCfmBad below.
		s_gxCfmCalls     = 0;
		s_gxCfmRepeats   = 0;
		s_gxClrCalls     = 0;
		s_gxClrHits      = 0;	// s_gxClrBad is kept, as s_gxCfmBad
		// s_gxCfmBad is NOT reset: it is the falsification counter, and a single mismatch anywhere
		// in the match must stay visible in every line after it rather than scroll away.
	}

	// GeneralsX PF27 checker report (GX_PF27=check only). bad is never reset: one mismatch anywhere stays visible.
	// GeneralsX: The [GXPF27] counter line (fprintf + fflush) only when asked for: the kernel is the
	// default now, and a blocking stderr write on every busy frame is the hitch the headless CRC walk taught us
	// ("the headless peer runs a per-frame CRC walk"). Check mode, a trace, or GX_PF27_REPORT=1.
	static const Bool s_pf27Report = getenv("GX_PF27_REPORT") != nullptr;
	if (m_pf27Mode == PF27_CHECK || (m_pf27Mode == PF27_NEW && (s_pf27Report || pf27TraceLevel() != 0)))
		pf27ReportCounters();


	pf27TraceFlush(false);	// GeneralsX PF27 trace: one hash per frame over every internalFindPath since the last pass

	if (!m_isMapReady) {
		return;
	}
#ifdef DEBUG_QPF
#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
	__int64 startTime64;
	double timeToUpdate=0.0f;
	__int64 endTime64,freq64;
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);
#endif
#endif

	if (m_zoneManager.needToCalculateZones()) {
		m_zoneManager.calculateZones(m_map, m_layers, m_extent);
		return;
	}

	// Get the current logical extent.
	Region3D terrainExtent;
	TheTerrainLogic->getExtent( &terrainExtent );
	IRegion2D bounds;
	bounds.lo.x = REAL_TO_INT_FLOOR(terrainExtent.lo.x / PATHFIND_CELL_SIZE_F);
	bounds.hi.x = REAL_TO_INT_FLOOR(terrainExtent.hi.x / PATHFIND_CELL_SIZE_F);
	bounds.lo.y = REAL_TO_INT_FLOOR(terrainExtent.lo.y / PATHFIND_CELL_SIZE_F);
	bounds.hi.y = REAL_TO_INT_FLOOR(terrainExtent.hi.y / PATHFIND_CELL_SIZE_F);
	bounds.hi.x--;
	bounds.hi.y--;
	m_logicalExtent = bounds;

	m_cumulativeCellsAllocated = 0;	// Number of pathfind cells examined.
#if defined(GX_TEST_INSTRUMENTS)
	PF27Kernel::runProbe(this);	// GeneralsX PF27 probe, GX_PF27_PROBE only (test env; moves the sim the same way in every mode)
#endif
	Int pathsFound = 0;
	while (m_cumulativeCellsAllocated < PATHFIND_CELLS_PER_FRAME &&
		m_queuePRTail!=m_queuePRHead) {
		Object *obj = TheGameLogic->findObjectByID(m_queuedPathfindRequests[m_queuePRHead]);
		m_queuedPathfindRequests[m_queuePRHead] = INVALID_ID;
		if (obj) {
			AIUpdateInterface *ai = obj->getAIUpdateInterface();
			if (ai) {
				ai->doPathfind(this);
				pathsFound++;
			}
		}
		m_queuePRHead = m_queuePRHead+1;
		if (m_queuePRHead >= PATHFIND_QUEUE_LEN) {
			m_queuePRHead = 0;
		}
	}
	if (pathsFound > 0) {
		PROFILER_PLOT("PathfindCells", (double)m_cumulativeCellsAllocated);
		PROFILER_PLOT("PathfindPaths", (double)pathsFound);
	}
#ifdef DEBUG_QPF
	if (pathsFound>0) {
#ifdef DEBUG_LOGGING
		QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
		timeToUpdate = ((double)(endTime64-startTime64) / (double)(freq64));
		if (timeToUpdate>0.01f)
		{
			DEBUG_LOG(("%d Pathfind queue: %d paths, %d cells --", TheGameLogic->getFrame(), pathsFound, m_cumulativeCellsAllocated));
			DEBUG_LOG(("time %f (%f)", timeToUpdate, (::GetTickCount()-startTimeMS)/1000.0f));
		}
#endif
	}	
#endif
#if defined(RTS_DEBUG)
	doDebugIcons();
#endif

}


void Pathfinder::checkChangeLayers(PathfindCell *parentCell)
{
	if (parentCell->getConnectLayer() == LAYER_INVALID)
		return;

	ICoord2D newCellCoord = { parentCell->getXIndex(), parentCell->getYIndex() };
	PathfindCell *newCell = getCell(parentCell->getConnectLayer(), newCellCoord.x, newCellCoord.y );

	if (!newCell) {
		DEBUG_CRASH(("Couldn't find cell."));
		return;
	}

	// already on one of the lists
	if (newCell->hasInfo() && (newCell->getOpen() || newCell->getClosed())) {
		return;
	}

	if (!newCell->allocateInfo(newCellCoord)) {
		// Out of cells for pathing...
		return;
	}
	// compute cost of path thus far
	// keep track of path we're building - point back to cell we moved here from
	newCell->setParentCell(parentCell) ;
	// store cost of this path
	newCell->setCostSoFar(parentCell->getCostSoFar()); // same as parent cost
	newCell->setTotalCost(parentCell->getTotalCost());
	// insert newCell in open list such that open list is sorted, smallest total path cost first
	newCell->putOnSortedOpenList( m_openList );
}

bool Pathfinder::checkCellOutsideExtents(ICoord2D& cell) {
	return 	cell.x < m_logicalExtent.lo.x ||
					cell.x > m_logicalExtent.hi.x ||
					cell.y < m_logicalExtent.lo.y ||
					cell.y > m_logicalExtent.hi.y;
}


struct ExamineCellsStruct
{
	Pathfinder					*thePathfinder;
	const LocomotorSet	*theLoco;
	Bool								centerInCell;
	Bool								isHuman;
	Int									radius;
	const Object				*obj;
	PathfindCell				*goalCell;
	// GeneralsX: obj->getCrusherLevel() is a two-hop lookup through
	// the object's template, and it was being redone for every cell on every line walked to the
	// goal - 154 + 169 leaf samples of a 25 s profile, for a value that cannot change inside one
	// search. examineNeighboringCells already computes exactly this expression; carry it in.
	Bool								isCrusher;
};

/*static*/ Int Pathfinder::examineCellsCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	ExamineCellsStruct* d = (ExamineCellsStruct*)userData;
	const Bool isCrusher = d->isCrusher;
	if (d->thePathfinder->m_isTunneling) return 1; // abort.
	if (from && to) {
			if (!d->thePathfinder->validMovementPosition( isCrusher, d->theLoco->getValidSurfaces(), to, from )) {
				return 1;
			}
			if ( (to->getLayer() == LAYER_GROUND) && !d->thePathfinder->m_zoneManager.isPassable(to_x, to_y) ) {
				return 1;
			}

			if (to->getPinched()) {
				return 1; // abort.
			}
			if (d->isHuman) {
				// check if new cell is in logical map.	(computer can move off logical map)
				if (to_x < d->thePathfinder->m_logicalExtent.lo.x) return 1; // abort
				if (to_y < d->thePathfinder->m_logicalExtent.lo.y) return 1; // abort
				if (to_x > d->thePathfinder->m_logicalExtent.hi.x) return 1; // abort
				if (to_y > d->thePathfinder->m_logicalExtent.hi.y) return 1; // abort
			}
			TCheckMovementInfo info;
			info.cell.x = to_x;
			info.cell.y = to_y;
			info.layer = from->getLayer();
			info.centerInCell = d->centerInCell;
			info.radius = d->radius;
			info.considerTransient = false;
			info.acceptableSurfaces = d->theLoco->getValidSurfaces();
			// GeneralsX: Inside an A* search - see
			// checkForMovementInSearch. This is the site that repeats the most: every expansion
			// re-walks a line to the goal and those lines converge on the same cells.
			if (!d->thePathfinder->checkForMovementInSearch(d->obj, info)) {
				return 1; //abort.
			}

			if (info.enemyFixed) {
				return 1; //abort.
			}

			if (info.allyFixedCount) {
				return 1; //abort.
			}

			UnsignedInt newCostSoFar = from->getCostSoFar() + 0.5f*COST_ORTHOGONAL;
			if (to->getType() == PathfindCell::CELL_CLIFF ) {
				return 1;
			}

			ICoord2D newCellCoord;
			newCellCoord.x = to_x;
			newCellCoord.y = to_y;

			if (!to->allocateInfo(newCellCoord)) {
				// Out of cells for pathing...
 				return 1;
			}
			to->setBlockedByAlly(false);

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			if ( to->hasInfo() && (to->getOpen() || to->getClosed()) )
			{
				// already on one of the lists - if existing costSoFar is less,
				// the new cell is on a longer path, so skip it
				if (to->getCostSoFar() <= newCostSoFar)
					return 0; // keep going.
			}

			// GeneralsX: costToGoal used to be computed above this
			// early-out, and it is used only below it. On a line walked to the goal most cells are
			// already on a list with a cheaper cost, so most of the time this was computed and
			// thrown away. It is a pure function of the two cells' coordinates - moving it past a
			// branch that touches neither cannot change its value.
			Int costRemaining = to->costToGoal( d->goalCell );

			to->setCostSoFar(newCostSoFar);
			// keep track of path we're building - point back to cell we moved here from
			to->setParentCell(from) ;
			to->setTotalCost(to->getCostSoFar() + costRemaining) ;

			// if to was on closed list, remove it from the list
			if (to->getClosed())
				to->removeFromClosedList( d->thePathfinder->m_closedList );

			// if the to was already on the open list, remove it so it can be re-inserted in order
			if (to->getOpen())
				to->removeFromOpenList( d->thePathfinder->m_openList );

			// insert to in open list such that open list is sorted, smallest total path cost first
			to->putOnSortedOpenList( d->thePathfinder->m_openList );
	}

	return 0;	// keep going
}


Int Pathfinder::examineNeighboringCells(PathfindCell *parentCell, PathfindCell *goalCell, const LocomotorSet& locomotorSet,
																				 Bool isHuman, Bool centerInCell, Int radius, const ICoord2D &startCellNdx,
																				 const Object *obj, Int attackDistance)
{
		Bool canPathThroughUnits = false;
		if (obj && obj->getAIUpdateInterface()) {
			canPathThroughUnits = obj->getAIUpdateInterface()->canPathThroughUnits();
		}
		Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;
		if (attackDistance==NO_ATTACK && !m_isTunneling && !locomotorSet.isDownhillOnly() && goalCell) {
			ExamineCellsStruct info;
			info.thePathfinder = this;
			info.theLoco = &locomotorSet;
			info.centerInCell = centerInCell;
			info.radius = radius;
			info.obj = obj;
			info.isHuman = isHuman;
			info.goalCell = goalCell;
			info.isCrusher = isCrusher;	// GeneralsX: same expression the callback used to recompute per cell
			ICoord2D start, end;
			start.x = parentCell->getXIndex();
			start.y = parentCell->getYIndex();
			end.x = goalCell->getXIndex();
			end.y = goalCell->getYIndex();
			iterateCellsAlongLine(start, end, parentCell->getLayer(), examineCellsCallback, &info);
		}

		Int cellCount = 0;
		// expand search to neighboring orthogonal cells
		static ICoord2D delta[] =
		{
			{ 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
			{ 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
		};
		const Int numNeighbors = 8;
		const Int firstDiagonal = 4;
		ICoord2D newCellCoord;
		PathfindCell *newCell;
		const Int adjacent[5] = {0, 1, 2, 3, 0};
		Bool neighborFlags[8] = { 0 };

		UnsignedInt newCostSoFar = 0;

		for( int i=0; i<numNeighbors; i++ )
		{
			neighborFlags[i] = false;
			// determine neighbor cell to try
			newCellCoord.x = parentCell->getXIndex() + delta[i].x;
			newCellCoord.y = parentCell->getYIndex() + delta[i].y;

			// get the neighboring cell
			newCell = getCell(parentCell->getLayer(), newCellCoord.x, newCellCoord.y );

			// check if cell is on the map
			if (!newCell)
				continue;

			Bool notZonePassable = false;
			if ((newCell->getLayer()==LAYER_GROUND) && !m_zoneManager.isPassable(newCellCoord.x, newCellCoord.y)) {
				notZonePassable = true;
			}

			// check if new cell is in logical map.	(computer can move off logical map)
			if (isHuman && checkCellOutsideExtents(newCellCoord))
				continue;

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			if ( newCell->hasInfo() && (newCell->getOpen() || newCell->getClosed()) )
				continue;

			if (i>=firstDiagonal) {
				// make sure one of the adjacent sides is open.
				if (!neighborFlags[adjacent[i-4]] && !neighborFlags[adjacent[i-3]]) {
					continue;
				}
			}

			// do the gravity check here
			if ( locomotorSet.isDownhillOnly() )
			{
				Coord3D fromPos;
				fromPos.x = parentCell->getXIndex() * PATHFIND_CELL_SIZE_F ;
				fromPos.y = parentCell->getYIndex() * PATHFIND_CELL_SIZE_F ;
				fromPos.z = TheTerrainLogic->getGroundHeight(fromPos.x , fromPos.y);

				Coord3D toPos;
				toPos.x = newCellCoord.x * PATHFIND_CELL_SIZE_F ;
				toPos.y = newCellCoord.y * PATHFIND_CELL_SIZE_F ;
				toPos.z = TheTerrainLogic->getGroundHeight(toPos.x , toPos.y);

				if ( fromPos.z < toPos.z )
					continue;
			}

			Bool movementValid = validMovementPosition(isCrusher, locomotorSet.getValidSurfaces(), newCell, parentCell);
			Bool dozerHack = false;
			if (!movementValid && obj->isKindOf(KINDOF_DOZER) && newCell->getType() == PathfindCell::CELL_OBSTACLE) {
				Object* obstacle = TheGameLogic->findObjectByID(newCell->getObstacleID());
				if (obstacle && !(obj->getRelationship(obstacle) == ENEMIES)) {
					movementValid = true;
					dozerHack = true;
				}
			}

			if (!movementValid && !m_isTunneling) {
				continue;
			}

			if (!dozerHack)
				neighborFlags[i] = true;

			TCheckMovementInfo info;
			info.cell = newCellCoord;
			info.layer = parentCell->getLayer();
			info.centerInCell = centerInCell;
			info.radius = radius;
			info.considerTransient = false;
			info.acceptableSurfaces = locomotorSet.getValidSurfaces();
			Int dx = newCellCoord.x-startCellNdx.x;
			Int dy = newCellCoord.y-startCellNdx.y;
			if (dx<0) dx = -dx;
			if (dy<0) dy = -dy;
			if (dx>1+radius) info.considerTransient = false;
			if (dy>1+radius) info.considerTransient = false;
			// GeneralsX: Inside an A* search - see
			// checkForMovementInSearch. A cell is a neighbour of up to eight expanded cells.
			if (!checkForMovementInSearch(obj, info) || info.enemyFixed) {
				if (!m_isTunneling) {
					continue;
				}
				movementValid = false;
			}

			if (movementValid && !newCell->getPinched()) {
				//Note to self - only turn off tunneling after check for movement.jba.
				m_isTunneling = false;
			}

			if (!newCell->hasInfo()) {
				if (!newCell->allocateInfo(newCellCoord)) {
					// Out of cells for pathing...
 					return cellCount;
				}
				cellCount++;
			}

			newCostSoFar = newCell->costSoFar( parentCell );
			if (info.allyMoving && dx<10 && dy<10) {
				newCostSoFar += 3*COST_DIAGONAL;
			}

			if (newCell->getType() == PathfindCell::CELL_CLIFF && !newCell->getPinched() ) {
				Coord3D fromPos;
				fromPos.x = parentCell->getXIndex() * PATHFIND_CELL_SIZE_F ;
				fromPos.y = parentCell->getYIndex() * PATHFIND_CELL_SIZE_F ;
				fromPos.z = TheTerrainLogic->getGroundHeight(fromPos.x , fromPos.y);

				Coord3D toPos;
				toPos.x = newCellCoord.x * PATHFIND_CELL_SIZE_F ;
				toPos.y = newCellCoord.y * PATHFIND_CELL_SIZE_F ;
				toPos.z = TheTerrainLogic->getGroundHeight(toPos.x , toPos.y);

				if ( fabs(fromPos.z - toPos.z)<PATHFIND_CELL_SIZE_F) {
					newCostSoFar += 7*COST_DIAGONAL;
				}
			} else if (newCell->getPinched()) {
				newCostSoFar += COST_ORTHOGONAL;
			}

			newCell->setBlockedByAlly(false);
			if (info.allyFixedCount>0) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
				newCostSoFar += 3*COST_DIAGONAL*info.allyFixedCount;
#else
				newCostSoFar += 3*COST_DIAGONAL;
#endif
				if (!canPathThroughUnits)
					newCell->setBlockedByAlly(true);
			}

			Int costRemaining = 0;
			if (goalCell) {
				if (attackDistance == NO_ATTACK)  {
					costRemaining = newCell->costToGoal( goalCell );
				}	else {
					dx = newCellCoord.x - goalCell->getXIndex();
					dy = newCellCoord.y - goalCell->getYIndex();
					costRemaining = COST_ORTHOGONAL*sqrt(dx*dx + dy*dy);
					costRemaining -= attackDistance/2;
					if (costRemaining<0)
						costRemaining=0;
					if (info.allyGoal) {
						if (obj->isKindOf(KINDOF_VEHICLE)) {
							newCostSoFar += 3*COST_ORTHOGONAL;
						}	else {
							// Infantry can pass through infantry.
							newCostSoFar += COST_ORTHOGONAL;
						}
					}
				}
			}

			if (notZonePassable) {
				newCostSoFar += 100*COST_ORTHOGONAL;
			}

			if (newCell->getType()==PathfindCell::CELL_OBSTACLE) {
				newCostSoFar += 100*COST_ORTHOGONAL;
			}

			if (m_isTunneling) {
				if (!validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), newCell, parentCell )) {
					newCostSoFar += 10*COST_ORTHOGONAL;
				}
			}

			newCell->setCostSoFar(newCostSoFar);
			// keep track of path we're building - point back to cell we moved here from
			newCell->setParentCell(parentCell) ;
			if (m_isTunneling) {
				costRemaining = 0; // find the closest valid cell.
			}
			newCell->setTotalCost(newCell->getCostSoFar() + costRemaining) ;

			// if newCell was on closed list, remove it from the list
			if (newCell->getClosed())
				newCell->removeFromClosedList( m_closedList );

			// if the newCell was already on the open list, remove it so it can be re-inserted in order
			if (newCell->getOpen())
				newCell->removeFromOpenList( m_openList );

			// insert newCell in open list such that open list is sorted, smallest total path cost first
			newCell->putOnSortedOpenList( m_openList );
		}
	return cellCount;
}


/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::findPath( Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
													 const Coord3D *rawTo)
{
	if (!clientSafeQuickDoesPathExist(locomotorSet, from, rawTo)) {
		return nullptr;
	}
	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}

	m_zoneManager.clearPassableFlags();
	Path *hPat = findHierarchicalPath(isHuman, locomotorSet, from, rawTo, false);
	if (hPat) {
		deleteInstance(hPat);
	}	else {
		m_zoneManager.setAllPassable();
	}

	Path *pat = internalFindPath(obj, locomotorSet, from, rawTo);
	if (pat!=nullptr) {
		return pat;
	}

	return nullptr;
}
/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::internalFindPath( Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
													 const Coord3D *rawTo)
{
	// GeneralsX: PF27 end-to-end trace (GX_PF27_TRACE): everything this search leaves
	// behind, hashed after it has cleaned up, so a legacy run and a PF27 run of one match can be compared.
	if (m_pf27 == nullptr || pf27TraceLevel() == 0 || !m_pf27->prepare())
		return internalFindPathBody(obj, locomotorSet, from, rawTo);
	m_pf27->traceBegin();
	Path *path = internalFindPathBody(obj, locomotorSet, from, rawTo);
	m_pf27->traceEnd(obj, from, rawTo, path);
	return path;
}

Path *Pathfinder::internalFindPathBody( Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
													 const Coord3D *rawTo)
{
	//CRCDEBUG_LOG(("Pathfinder::findPath()"));
#ifdef INTENSE_DEBUG
	DEBUG_LOG(("internal find path..."));
#endif

#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif
	Bool centerInCell = true;
	Int radius = 0;
	if (obj) {
		getRadiusAndCenter(obj, radius, centerInCell);
	}

	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}

	if (rawTo->x == 0.0f && rawTo->y == 0.0f) {
		DEBUG_LOG(("Attempting pathfind to 0,0, generally a bug."));
		return nullptr;
	}
	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	if (m_isMapReady == false) {
		return nullptr;
	}

	Coord3D adjustTo = *rawTo;
	Coord3D *to = &adjustTo;
	Coord3D clipFrom = *from;
	clip(&clipFrom, &adjustTo);

	if (!centerInCell) {
		adjustTo.x += PATHFIND_CELL_SIZE_F/2;
		adjustTo.y += PATHFIND_CELL_SIZE_F/2;
	}

	m_isTunneling = false;

	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);
	// determine goal cell
	PathfindCell *goalCell = getCell( destinationLayer, to );
	if (goalCell == nullptr) {
		return nullptr;
	}

	ICoord2D cell;
	worldToCell( to, &cell );

	if (!checkDestination(obj, cell.x, cell.y, destinationLayer, radius, centerInCell)) {
		return nullptr;
	}
	// determine start cell
	ICoord2D startCellNdx;
	worldToCell(&clipFrom, &startCellNdx);
	PathfindLayerEnum layer = LAYER_GROUND;
	if (obj) {
		layer = obj->getLayer();
	}
	PathfindCell *parentCell = getClippedCell( layer,&clipFrom );
	if (parentCell == nullptr) {
		return nullptr;
	}

	ICoord2D pos2d;
	worldToCell(to, &pos2d);
	if (!goalCell->allocateInfo(pos2d)) {
		return nullptr;
	}
	if (parentCell!=goalCell) {
		worldToCell(&clipFrom, &pos2d);
		if (!parentCell->allocateInfo(pos2d)) {
			goalCell->releaseInfo();
			return nullptr;
		}
	}
	//
	// Determine if this pathfind is "tunneling" or not.
	// A tunneling pathfind starts from within an obstacle, and is allowed
	// to ignore obstacle cells until it reaches a cell that is no longer
	// classified as an obstacle.  At that point, the pathfind behaves normally.
	//
	m_isTunneling = parentCell->getType() == PathfindCell::CELL_OBSTACLE;

	Int zone1, zone2;
	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;
	zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, parentCell->getZone());
	zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, goalCell->getZone());

	if ( (layer==LAYER_WALL && zone1 == 0) || (destinationLayer==LAYER_WALL && zone2 == 0) ) {
#if RETAIL_COMPATIBLE_PATHFINDING
		if (s_useFixedPathfinding)
#endif
		{
			goalCell->releaseInfo();
			parentCell->releaseInfo();
		}
		return nullptr;
	}

	if (goalCell->isObstaclePresent(m_ignoreObstacleID) || m_isTunneling) {
		// Use terrain zones instead of building zones, since we are moving into or out of a building.
		zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
		zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, zone2);
		zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
		zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), isCrusher, zone1);
	}

	//DEBUG_LOG(("Zones %d to %d", zone1, zone2));

	if ( zone1 != zone2) {
		//DEBUG_LOG(("Intense Debug Info - Pathfind Zone screen failed-cannot reach desired location."));
		goalCell->releaseInfo();
		parentCell->releaseInfo();
		return nullptr;
	}

	// sanity check - if destination is invalid, can't path there
	if (!validMovementPosition( isCrusher, destinationLayer, locomotorSet, to ))	{
		m_isTunneling = false;
		goalCell->releaseInfo();
		parentCell->releaseInfo();
		return nullptr;
	}

	// sanity check - if source is invalid, we have to cheat
	if (!validMovementPosition( isCrusher, layer, locomotorSet, from ))	{
		// somehow we got to an impassable location.
		m_isTunneling = true;
	}

	parentCell->startPathfind(goalCell);

	// GeneralsX: PF27 (PF27Kernel.inl). GX_PF27=new runs the whole search, exits
	// included, on the new kernel; GX_PF27=check runs it as a shadow first and compares it with the legacy loop
	// below at the loop's exit, before the legacy cleanup. GX_PF27=legacy does neither. The default is new (PF27Kernel.inl).
	Bool pf27Check = false;
	PathfindCell *const pf27Start = parentCell;	// legacy reuses parentCell in its loop
	if (m_pf27Mode != PF27_LEGACY && m_pf27 != nullptr && m_pf27->prepare())
	{
		if (m_pf27Mode == PF27_NEW)
		{
			GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_INTERNAL_FIND); // observation only
			return m_pf27->findInternal(obj, locomotorSet, from, isHuman, centerInCell, radius, startCellNdx, parentCell, goalCell);
		}
		const Bool tunnelingBefore = m_isTunneling;
		m_pf27->shadowInternal(obj, locomotorSet, isHuman, centerInCell, radius, startCellNdx, parentCell, goalCell);
		m_isTunneling = tunnelingBefore;
		++s_gxMoveGen;	// the legacy loop must not be served memo entries the shadow wrote
		pf27Check = true;
	}

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	Int cellCount = 0;

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//
	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_INTERNAL_FIND); // GeneralsX A* probe, observation only
	const UnsignedInt infosAtSearchStart = s_pathfindCellInfoTakes;	// GeneralsX, see MAX_INTERNAL_PATH_INFO_COUNT
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		if (parentCell == goalCell)
		{
			if (pf27Check)	// GeneralsX PF27 checker: the whole final search state, before anything is cleaned
				m_pf27->compareWithLegacy(true, goalCell, infosAtSearchStart, obj, from, to);
			if (m_pf27 != nullptr && m_pf27->tracing())	// GeneralsX PF27 trace: which cells this search listed
				m_pf27->traceCaptureLegacy(pf27Start, goalCell);
			// success - found a path to the goal
			Bool show = TheGlobalData->m_debugAI==AI_DEBUG_PATHS;
#ifdef INTENSE_DEBUG
			DEBUG_LOG(("internal find path SUCCESS..."));
			Int count = 0;
			if (cellCount>1000 && obj) {
				show = true;
				DEBUG_LOG(("cells %d obj %s %x from (%f,%f) to(%f, %f)", count, obj->getTemplate()->getName().str(), obj, from->x, from->y, to->x, to->y));
#ifdef STATE_MACHINE_DEBUG
				if( obj->getAIUpdateInterface() )
				{
					DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
				}
#endif
				TheScriptEngine->AppendDebugMessage("Big path", false);
			}
#endif
			if (show)
				debugShowSearch(true);

			m_isTunneling = false;
			// construct and return path
			Path *path =  buildActualPath( obj, locomotorSet.getValidSurfaces(), from, goalCell, centerInCell, false );
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding)
			{
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
			}
			return path;
		}

		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		// GeneralsX: The allocation bound - see PATHFIND_SEARCH_FREE_RESERVE,
		// the pool floor only since 27/09/2026. Same shape as the one findClosestPath, patchPath and
		// findAttackPath already use: it stops the search GROWING, it does not break the loop. The
		// open list still drains one cell per iteration onto the closed list, so the function still
		// leaves by one of its two existing exits - the goal popped off the open list, or the open
		// list emptied - and each of those already calls cleanOpenAndClosedLists and releases its
		// cells. No exit path is added, so no exit path can have different cleanup, and the loop
		// cannot spin: once the guard is false nothing can re-open a closed cell.
		//
		// checkChangeLayers is inside the guard because it allocates an info and pushes onto the
		// open list too - it is part of growing, not part of draining.
		if (pathfindSearchMayGrow(infosAtSearchStart, MAX_INTERNAL_PATH_INFO_COUNT)) {
			// Check to see if we can change layers in this cell.
			checkChangeLayers(parentCell);

			cellCount += examineNeighboringCells(parentCell, goalCell, locomotorSet, isHuman, centerInCell, radius, startCellNdx, obj, NO_ATTACK);
		}

	}

	if (pf27Check)	// GeneralsX PF27 checker, as on the success exit
		m_pf27->compareWithLegacy(false, goalCell, infosAtSearchStart, obj, from, to);
	if (m_pf27 != nullptr && m_pf27->tracing())	// GeneralsX PF27 trace, as on the success exit
		m_pf27->traceCaptureLegacy(pf27Start, goalCell);

	// failure - goal cannot be reached
#if defined(RTS_DEBUG)
#ifdef INTENSE_DEBUG
	DEBUG_LOG(("internal find path FAILURE..."));
#endif
	if (TheGlobalData->m_debugAI == AI_DEBUG_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		addIcon(nullptr, 0, 0, color);
		debugShowSearch(false);
		Coord3D pos;
		pos = *from;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		pos = *to;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		Real dx, dy;
		dx = from->x - to->x;
		dy = from->y - to->y;

		Int count = sqrt(dx*dx+dy*dy)/(PATHFIND_CELL_SIZE_F/2);
		if (count<2) count = 2;
		Int i;
		color.green = 0;
		for (i=1; i<count; i++) {
			pos.x = from->x + (to->x-from->x)*i/count;
			pos.y = from->y + (to->y-from->y)*i/count;
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
			addIcon(&pos, PATHFIND_CELL_SIZE_F/2, 60, color);

		}
	}

	if (obj) {
#ifdef DUMP_PERF_STATS
		TheGameLogic->incrementOverallFailedPathfinds();
#endif
#ifdef STATE_MACHINE_DEBUG
		if( obj->getAIUpdateInterface() )
		{
			DEBUG_LOG(("state %s", obj->getAIUpdateInterface()->getCurrentStateName().str()));
		}
#endif
	}
#endif

#ifdef DEBUG_LOGGING
	if (obj)
	{
		Bool valid;
		valid = validMovementPosition( isCrusher, obj->getLayer(), locomotorSet, to ) ;

		DEBUG_LOG(("%d Pathfind failed from (%f,%f) to (%f,%f), OV %d --", TheGameLogic->getFrame(), from->x, from->y, to->x, to->y, valid));
		DEBUG_LOG(("Unit '%s', time %f, cells %d", obj->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f,cellCount));
	}
#endif

	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding)
	{
		goalCell->releaseInfo();
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}
	return nullptr;
}

/**
 * Checks to see if there is enough path width at this cell for ground
 * movement.  Returns the width available.
 * GeneralsX: Served from the per-findGroundPath memo when one is
 * running - see GXClearMemoEntry.
 */
Int Pathfinder::clearCellForDiameter(Bool crusher, Int cellX, Int cellY, PathfindLayerEnum layer, Int pathDiameter)
{
	GXClearMemoEntry *entry = nullptr;
	if (s_gxClearActive && s_gxClearMemo != nullptr &&
	    cellX >= 0 && cellX <= m_extent.hi.x && cellY >= 0 && cellY <= m_extent.hi.y &&
	    pathDiameter >= 0 && pathDiameter <= 255 && (Int)layer >= 0 && (Int)layer <= 255)
	{
		entry = &s_gxClearMemo[(UnsignedInt)cellX * (UnsignedInt)s_gxClearMemoStrideY + (UnsignedInt)cellY];
	}
	const Bool hit = (entry != nullptr && entry->m_gen == s_gxClearGen && entry->m_layer == (UnsignedByte)layer &&
	                  entry->m_diameter == (UnsignedByte)pathDiameter && entry->m_crusher == (crusher ? 1 : 0));
	const Bool trace = gxPathTraceVerify();	// GeneralsX PF27: not under GX_PATHTRACE=time
	if (hit && !trace)
		return entry->m_result;

	const Int result = clearCellForDiameterUncached(crusher, cellX, cellY, layer, pathDiameter);
	if (trace && entry != nullptr)
	{
		++s_gxClrCalls;
		if (hit)
		{
			++s_gxClrHits;
			if (entry->m_result != result)
				++s_gxClrBad;
		}
	}
	if (entry != nullptr)
	{
		// the answer is 0 .. pathDiameter, so it fits the byte the key's diameter already fits
		entry->m_gen      = s_gxClearGen;
		entry->m_layer    = (UnsignedByte)layer;
		entry->m_diameter = (UnsignedByte)pathDiameter;
		entry->m_crusher  = crusher ? 1 : 0;
		entry->m_result   = (UnsignedByte)result;
	}
	return result;
}

Int Pathfinder::clearCellForDiameterUncached(Bool crusher, Int cellX, Int cellY, PathfindLayerEnum layer, Int pathDiameter)
{
	Int radius = pathDiameter/2;
	Int numCellsAbove = radius;
	if (radius==0) numCellsAbove++;
	Int i, j;
	Bool clear = true;
	Bool cutCorners = false;
	if (radius>1) {
		cutCorners = true;
		// We remove the outside corner cells from the check.
	}
	for (i=cellX-radius; i<cellX+numCellsAbove; i++) {
		Bool xMinOrMax = (i==cellX-radius) || (i==cellX+numCellsAbove-1);
		for (j=cellY-radius; j<cellY+numCellsAbove; j++) {
			Bool yMinOrMax = (j==cellY-radius) || (j==cellY+numCellsAbove-1);
			if (xMinOrMax && yMinOrMax && cutCorners) {
				continue; // this is an outside corner cell, and we are cutting corners. jba. :)
			}
			PathfindCell	*cell = getCell(layer, i, j);
			if (cell) {
				if (cell->getType() != PathfindCell::CELL_CLEAR) {
					if (cell->getType() == PathfindCell::CELL_OBSTACLE) {
						if (cell->isObstacleFence()) {
							if (!crusher) {
								clear = false;
							}
						} else {
							clear = false;
						}
					} else {
						clear = false;
					}
				}
				if (cell->getFlags() == PathfindCell::UNIT_PRESENT_FIXED && pathDiameter>=2) {
					Object *obj = TheGameLogic->findObjectByID(cell->getPosUnit());
					if (obj) {
						if (crusher) {
							if (obj->getCrushableLevel()>1) {
								clear = false;
							}
						} else {
							if (obj->getCrushableLevel()>0) {
								clear = false;
							}
						}
					}
				}
			} else {
				return false; // off the map.
			}
			if (!clear) break;
		}
	}
	if (clear) {
		if (radius==0) return 1;
		return 2*radius;
	}
	if (pathDiameter < 2) return 0;
	return clearCellForDiameter(crusher, cellX, cellY, layer, pathDiameter-2);
}

/**
 * Work backwards from goal cell to construct final path.
 */
Path *Pathfinder::buildGroundPath(Bool isCrusher, const Coord3D *fromPos, PathfindCell *goalCell, Bool center, Int pathDiameter )
{
	DEBUG_ASSERTCRASH( goalCell, ("Pathfinder::buildActualPath: goalCell == nullptr") );

	Path *path = newInstance(Path);

	prependCells(path, fromPos, goalCell, center);

	// cleanup the path by checking line of sight
	path->optimizeGroundPath( isCrusher, pathDiameter );


#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI==AI_DEBUG_GROUND_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		Coord3D pos;
		PathNode *node = path->getFirstNode();
		for( ; node; node = node->getNext() )
		{

			// create objects to show path - they decay

			pos = *node->getPosition();
			color.red = color.green = 1;
			if (node->getLayer() != LAYER_GROUND) {
				color.red = 0;
			}
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.25f, 200, color);
		}

		// show optimized path
		for( node = path->getFirstNode(); node; node = node->getNextOptimized() )
		{
			pos = *node->getPosition();
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.8f, 200, color);
		}
		setDebugPath(path);
	}
#endif
	return path;
}

/**
 * Work backwards from goal cell to construct final path.
 */
Path *Pathfinder::buildHierarchicalPath( const Coord3D *fromPos, PathfindCell *goalCell )
{
	DEBUG_ASSERTCRASH( goalCell, ("Pathfinder::buildHierarchicalPath: goalCell == nullptr") );

	Path *path = newInstance(Path);

	prependCells(path, fromPos, goalCell, true);

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	// Expand the hierarchical path around the starting point. jba [8/24/2003]
	// This allows the unit to get around friendly units that may be near it.
	Coord3D pos = *path->getFirstNode()->getPosition();
	Coord3D minPos = pos;
	minPos.x -= static_cast<float>(PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F;
	minPos.y -= static_cast<float>(PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F;
	Coord3D maxPos = pos;
	maxPos.x += static_cast<float>(PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F;
	maxPos.y += static_cast<float>(PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F;
	ICoord2D cellNdxMin, cellNdxMax;
	worldToCell(&minPos, &cellNdxMin);
	worldToCell(&maxPos, &cellNdxMax);
	Int i, j;
	for (i=cellNdxMin.x; i<=cellNdxMax.x; i++) {
		for (j=cellNdxMin.y; j<=cellNdxMax.y; j++) {
			m_zoneManager.setPassable(i, j, true);
		}
	}
#endif

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI==AI_DEBUG_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		Coord3D pos;
		Int i;
		for (i=0; i<3; i++)
		for( PathNode *node = path->getFirstNode(); node; node = node->getNext() )
		{

			// create objects to show path - they decay

			pos = *node->getPosition();
			color.red = 1;
			color.green = 0.4f;
			if (node->getLayer() != LAYER_GROUND) {
				color.red = 0;
			}
			addIcon(&pos, PATHFIND_CELL_SIZE_F, 200, color);
		}
		setDebugPath(path);
	}
#endif
	return path;
}


struct MADStruct
{
	Pathfinder					*thePathfinder;
	Object							*obj;
	ObjectID						ignoreID;
};

/*static*/ Int Pathfinder::moveAlliesDestinationCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	MADStruct* d = (MADStruct*)userData;
	if (to) {
		if (to->getPosUnit()==INVALID_ID) {
			return 0;
		}
		if (to->getPosUnit()==d->obj->getID()) {
			return 0;	// It's us.
		}
		if (to->getPosUnit()==d->ignoreID) {
			return 0;	 // It's the one we are ignoring.
		}
		Object *otherObj = TheGameLogic->findObjectByID(to->getPosUnit());
		if (otherObj==nullptr) return 0;
		if (d->obj->getRelationship(otherObj)!=ALLIES) {
			return 0;  // Only move allies.
		}
		if (otherObj && otherObj->getAI() && !otherObj->getAI()->isMoving()) {
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
			//Kris: Patch 1.01 November 3, 2003
			//Black Lotus exploit fix -- moving while hacking.
			if( otherObj->testStatus( OBJECT_STATUS_IS_USING_ABILITY ) || otherObj->getAI()->isBusy() )
			{
				return 0; // Packing or unpacking objects for example
			}
#endif
			//DEBUG_LOG(("Moving ally"));
			otherObj->getAI()->aiMoveAwayFromUnit(d->obj, CMD_FROM_AI);
		}
	}

	return 0;	// keep going
}

void Pathfinder::moveAlliesAwayFromDestination(Object *obj,const Coord3D& destination)
{
	MADStruct info;
	info.obj = obj;
	info.ignoreID = obj->getAI()->getIgnoredObstacleID();
	info.thePathfinder = this;
	PathfindLayerEnum layer = obj->getLayer();
	if (layer==LAYER_GROUND) {
		layer = TheTerrainLogic->getLayerForDestination(&destination);
	}
	iterateCellsAlongLine(*obj->getPosition(), destination, layer, moveAlliesDestinationCallback, &info);

}


struct GroundCellsStruct
{
	Pathfinder					*thePathfinder;
	Bool								centerInCell;
	Int									pathDiameter;
	PathfindCell				*goalCell;
	Bool								crusher;
};

/*static*/ Int Pathfinder::groundCellsCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	GroundCellsStruct* d = (GroundCellsStruct*)userData;
	if (from && to) {
			if (to->hasInfo()) {
				if (to->getOpen() || to->getClosed())
				{
					// already on one of the lists
					return 1; // abort.
				}
			}
			// See how wide the cell is.
			Int clearDiameter = d->thePathfinder->clearCellForDiameter(d->crusher, to_x, to_y, to->getLayer(), d->pathDiameter);
			if (clearDiameter != d->pathDiameter) {
				return 1;
			}
			ICoord2D newCellCoord;
			newCellCoord.x = to_x;
			newCellCoord.y = to_y;
			if (!to->allocateInfo(newCellCoord)) {
				// Out of cells for pathing...
 				return 1;
			}

			UnsignedInt newCostSoFar = from->getCostSoFar() + 0.5f*COST_ORTHOGONAL;
			to->setBlockedByAlly(false);

			Int costRemaining = 0;
			costRemaining = to->costToGoal( d->goalCell );
			to->setCostSoFar(newCostSoFar);
			// keep track of path we're building - point back to cell we moved here from
			to->setParentCell(from) ;
			to->setTotalCost(to->getCostSoFar() + costRemaining) ;

			// insert to in open list such that open list is sorted, smallest total path cost first
			to->putOnSortedOpenList( d->thePathfinder->m_openList );
	}

	return 0;	// keep going
}

/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::findGroundPath( const Coord3D *from,
													 const Coord3D *rawTo, Int pathDiameter, Bool crusher)
{
	//CRCDEBUG_LOG(("Pathfinder::findGroundPath()"));
#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif
	// GeneralsX: The clearCellForDiameter memo lives for exactly this
	// call (every return below): a new generation on the way in, closed on the way out.
	if (s_gxClearMemo == nullptr && m_map != nullptr)
	{
		s_gxClearMemoStrideY = m_extent.hi.y + 1;
		s_gxClearMemoCells   = (UnsignedInt)(m_extent.hi.x + 1) * (UnsignedInt)s_gxClearMemoStrideY;
		s_gxClearMemo        = MSGNEW("PathfindClearMemo") GXClearMemoEntry[s_gxClearMemoCells];
		memset(s_gxClearMemo, 0, sizeof(GXClearMemoEntry) * s_gxClearMemoCells);
	}
	struct GXClearMemoScope
	{
		GXClearMemoScope() { ++s_gxClearGen; s_gxClearActive = true; }
		~GXClearMemoScope() { s_gxClearActive = false; ++s_gxClearGen; }
	} gxClearMemoScope;
#ifdef INTENSE_DEBUG
	DEBUG_LOG(("Find ground path..."));
#endif
	Bool centerInCell = false;

	m_zoneManager.clearPassableFlags();
	Bool isHuman = true;

	Path *hPat = internal_findHierarchicalPath(isHuman, LOCOMOTORSURFACE_GROUND, from, rawTo, false, false);
	if (hPat) {
		deleteInstance(hPat);
	}	else {
		m_zoneManager.setAllPassable();
	}

	if (rawTo->x == 0.0f && rawTo->y == 0.0f) {
		DEBUG_LOG(("Attempting pathfind to 0,0, generally a bug."));
		return nullptr;
	}
	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	if (m_isMapReady == false) {
		return nullptr;
	}

	Coord3D adjustTo = *rawTo;
	Coord3D *to = &adjustTo;
	Coord3D clipFrom = *from;
	clip(&clipFrom, &adjustTo);

	m_isTunneling = false;

	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);

	ICoord2D cell;
	worldToCell( to, &cell );

	if (pathDiameter!=clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)) {
		Int offset=1;
		ICoord2D newCell;
		const Int MAX_OFFSET = 8;
		while (offset<MAX_OFFSET) {
			newCell = cell;
			cell.x += offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.y += offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.x -= offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.x -= offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.y -= offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.y -= offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.x += offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			cell.x += offset;
			if (clearCellForDiameter(crusher, cell.x, cell.y, destinationLayer, pathDiameter)==pathDiameter) break;
			offset++;
			cell = newCell;
		}
		if (offset >= MAX_OFFSET) {
			return nullptr;
		}
	}

	// determine goal cell
	PathfindCell *goalCell = getCell( destinationLayer, cell.x, cell.y );
	if (goalCell == nullptr) {
		return nullptr;
	}
	if (!goalCell->allocateInfo(cell)) {
		return nullptr;
	}

	// determine start cell
	ICoord2D startCellNdx;
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(from);
	PathfindCell *parentCell = getClippedCell( layer,&clipFrom );
	if (parentCell == nullptr) {
#if RETAIL_COMPATIBLE_PATHFINDING
		if (s_useFixedPathfinding)
#endif
		{
			goalCell->releaseInfo();
		}
		return nullptr;
	}
	if (parentCell!=goalCell) {
		worldToCell(&clipFrom, &startCellNdx);
		if (!parentCell->allocateInfo(startCellNdx)) {
			goalCell->releaseInfo();
			return nullptr;
		}
	}


	Int zone1, zone2;
	// m_isCrusher = false;
	zone1 = m_zoneManager.getEffectiveZone(LOCOMOTORSURFACE_GROUND, false, parentCell->getZone());
	zone2 =  m_zoneManager.getEffectiveZone(LOCOMOTORSURFACE_GROUND, false, goalCell->getZone());

	//DEBUG_LOG(("Zones %d to %d", zone1, zone2));

	if ( zone1 != zone2) {
		goalCell->releaseInfo();
		parentCell->releaseInfo();
		return nullptr;
	}
	parentCell->startPathfind(goalCell);

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	// TheSuperHackers @fix helmutbuhler This was originally uninitialized and in the loop below.
#if RETAIL_COMPATIBLE_CRC
	UnsignedInt newCostSoFar = 0;
#endif

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//
	Int cellCount = 0;
	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_GROUND); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		if (parentCell == goalCell)
		{
			// success - found a path to the goal
#ifdef INTENSE_DEBUG
	DEBUG_LOG((" time %d msec %d cells", (::GetTickCount()-startTimeMS), cellCount));
	DEBUG_LOG((" SUCCESS"));
#endif
#if defined(RTS_DEBUG)
			Bool show = TheGlobalData->m_debugAI==AI_DEBUG_GROUND_PATHS;
			if (show)
				debugShowSearch(true);
#endif
			m_isTunneling = false;
			// construct and return path
			Path *path =  buildGroundPath(crusher, from, goalCell, centerInCell, pathDiameter );
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding)
			{
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
			}
			return path;
		}

		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		// Check to see if we can change layers in this cell.
		checkChangeLayers(parentCell);

		GroundCellsStruct info;
		info.thePathfinder = this;
		info.centerInCell = centerInCell;
		info.pathDiameter = pathDiameter;
		info.goalCell = goalCell;
		info.crusher = crusher;
		ICoord2D start, end;
		start.x = parentCell->getXIndex();
		start.y = parentCell->getYIndex();
		end.x = goalCell->getXIndex();
		end.y = goalCell->getYIndex();
		iterateCellsAlongLine(start, end, parentCell->getLayer(), groundCellsCallback, &info);

		// expand search to neighboring orthogonal cells
		static ICoord2D delta[] =
		{
			{ 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
			{ 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
		};
		const Int numNeighbors = 8;
		const Int firstDiagonal = 4;
		ICoord2D newCellCoord;
		PathfindCell *newCell;
		const Int adjacent[5] = {0, 1, 2, 3, 0};
		Bool neighborFlags[8] = { 0 };

		// TheSuperHackers @fix Mauller 23/05/2025 Fixes uninitialized variable.
#if RETAIL_COMPATIBLE_CRC
		// newCostSoFar defined in outer block.
#else
		UnsignedInt newCostSoFar = 0;
#endif

		for( int i=0; i<numNeighbors; i++ )
		{
			neighborFlags[i] = false;
			// determine neighbor cell to try
			newCellCoord.x = parentCell->getXIndex() + delta[i].x;
			newCellCoord.y = parentCell->getYIndex() + delta[i].y;

			// get the neighboring cell
			newCell = getCell(parentCell->getLayer(), newCellCoord.x, newCellCoord.y );

			// check if cell is on the map
			if (newCell == nullptr)
				continue;

			if ((newCell->getLayer()==LAYER_GROUND) && !m_zoneManager.isPassable(newCellCoord.x, newCellCoord.y)) {
				// check if we are within 3.
				Bool passable = false;
				if (m_zoneManager.clipIsPassable(newCellCoord.x+3, newCellCoord.y+3)) passable = true;
				if (m_zoneManager.clipIsPassable(newCellCoord.x-3, newCellCoord.y+3)) passable = true;
				if (m_zoneManager.clipIsPassable(newCellCoord.x+3, newCellCoord.y-3)) passable = true;
				if (m_zoneManager.clipIsPassable(newCellCoord.x-3, newCellCoord.y-3)) passable = true;
				if (!passable) continue;
			}

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			Bool onList = false;
			if (newCell->hasInfo()) {
				if (newCell->getOpen() || newCell->getClosed())
				{
					// already on one of the lists
					onList = true;
				}
			}
			Int clearDiameter = 0;
			if (newCell!=goalCell) {

				if (i>=firstDiagonal) {
					// make sure one of the adjacent sides is open.
					if (!neighborFlags[adjacent[i-4]] && !neighborFlags[adjacent[i-3]]) {
						continue;
					}
				}

				// See how wide the cell is.
				clearDiameter = clearCellForDiameter(crusher, newCellCoord.x, newCellCoord.y, newCell->getLayer(), pathDiameter);
				if (newCell->getType() != PathfindCell::CELL_CLEAR) {
					continue;
				}
				if (newCell->getPinched()) {
					continue;
				}
				neighborFlags[i] = true;

				if (!newCell->allocateInfo(newCellCoord)) {
					// Out of cells for pathing...
 					continue;
				}
				cellCount++;

#if RETAIL_COMPATIBLE_CRC
				// TheSuperHackers @fix helmutbuhler 11/06/2025 The indentation was wrong on retail here.
				newCostSoFar = newCell->costSoFar( parentCell );
				if (clearDiameter<pathDiameter) {
					int delta = pathDiameter-clearDiameter;
					newCostSoFar += 0.6f*(delta*COST_ORTHOGONAL);
				}
				newCell->setBlockedByAlly(false);
			}
#else
			}
			newCostSoFar = newCell->costSoFar( parentCell );
			if (clearDiameter<pathDiameter) {
				int delta = pathDiameter-clearDiameter;
				newCostSoFar += 0.6f*(delta*COST_ORTHOGONAL);
			}
			newCell->setBlockedByAlly(false);
#endif
			Int costRemaining = 0;
			costRemaining = newCell->costToGoal( goalCell );

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			if (onList)
			{
				// already on one of the lists - if existing costSoFar is less,
				// the new cell is on a longer path, so skip it
				if (newCell->getCostSoFar() <= newCostSoFar)
					continue;
			}
			newCell->setCostSoFar(newCostSoFar);
			// keep track of path we're building - point back to cell we moved here from
			newCell->setParentCell(parentCell) ;
			newCell->setTotalCost(newCell->getCostSoFar() + costRemaining) ;

			// if newCell was on closed list, remove it from the list
			if (newCell->getClosed())
				newCell->removeFromClosedList( m_closedList );

			// if the newCell was already on the open list, remove it so it can be re-inserted in order
			if (newCell->getOpen())
				newCell->removeFromOpenList( m_openList );

			// insert newCell in open list such that open list is sorted, smallest total path cost first
			newCell->putOnSortedOpenList( m_openList );
		}
	}
	// failure - goal cannot be reached
#ifdef INTENSE_DEBUG
	DEBUG_LOG((" FAILURE"));
#endif
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		addIcon(nullptr, 0, 0, color);
		debugShowSearch(false);
		Coord3D pos;
		pos = *from;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		pos = *to;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		Real dx, dy;
		dx = from->x - to->x;
		dy = from->y - to->y;

		Int count = sqrt(dx*dx+dy*dy)/(PATHFIND_CELL_SIZE_F/2);
		if (count<2) count = 2;
		Int i;
		color.green = 0;
		for (i=1; i<count; i++) {
			pos.x = from->x + (to->x-from->x)*i/count;
			pos.y = from->y + (to->y-from->y)*i/count;
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
			addIcon(&pos, PATHFIND_CELL_SIZE_F/2, 60, color);

		}
	}
#endif

	DEBUG_LOG(("%d FindGroundPath failed from (%f,%f) to (%f,%f) --", TheGameLogic->getFrame(), from->x, from->y, to->x, to->y));
	DEBUG_LOG(("time %f", (::GetTickCount()-startTimeMS)/1000.0f));

#ifdef DUMP_PERF_STATS
	TheGameLogic->incrementOverallFailedPathfinds();
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding)
	{
		goalCell->releaseInfo();
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}
	return nullptr;
}

/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
void Pathfinder::processHierarchicalCell( const ICoord2D &scanCell, const ICoord2D &delta, PathfindCell *parentCell,
																				 PathfindCell *goalCell, zoneStorageType parentZone,
																				 zoneStorageType *examinedZones, Int &numExZones,
																				 Bool crusher, Int &cellCount)
{
	if (scanCell.x<m_extent.lo.x || scanCell.x>m_extent.hi.x ||
		scanCell.y<m_extent.lo.y || scanCell.y>m_extent.hi.y) {
		return;
	}
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
	if (parentZone == PathfindZoneManager::UNINITIALIZED_ZONE) {
		return;
	}
#endif
	if (parentZone == m_zoneManager.getBlockZone(LOCOMOTORSURFACE_GROUND,
		crusher, scanCell.x, scanCell.y, m_map)) {
		PathfindCell *newCell = getCell(LAYER_GROUND, scanCell.x, scanCell.y);
		// GeneralsX: THE ZERO HOUR HIERARCHICAL SEARCH NEVER LEFT ITS START BLOCK.
		// Zero Hour's copy of this scan returned when the boundary cell had no PathfindCellInfo yet
		// ("if (!newCell->hasInfo()) return;"), but nothing gives a boundary cell an info before this
		// function does - allocateInfo(scanCell) is below. So a block was only ever left through a cell that
		// happened to carry an info already (a unit standing on it, an obstacle), and findHierarchicalPath
		// failed for almost every route longer than one zone block. Measured on W5's w5g1 game (Twilight
		// Flame, 7 Hard AI, to frame 50400): 28 hierarchical successes in 11,961 searches of 20-40 cells, 0
		// in 7,864 longer ones. Every caller then called setAllPassable() and searched the WHOLE map at cell
		// level, and a goal just across a long ridge (409-442 cells round for 17-88 cells straight) needs
		// more of the 30,000-info pool than exists: findPath failed, the findClosestPath fallback answered
		// with the ridge edge, and AI armies parked along both ridges re-pathing every 2 s (wh1ter0se, 27/09/2026:
		// "they are trying to go over a cliff"). That failure is independent of GeneralsX's search bounds: the
		// same ridge test fails on the port's own August builds with no bound at all.
		// Generals' copy of this line - examine the cell, allocate it below, skip it only if it was already
		// looked at - is what Zero Hour gets now. The allocation below already tolerates an empty pool
		// (hasInfo() guards the close, and the adjacent cell is queued only if it got an info). The search then
		// reaches the goal's block and marks the blocks on its route passable, so the cell-level A* explores
		// that corridor instead of the map.
		// SIM-VISIBLE: long routes are now found (and differently) where they used to fail; inside the digest.
		if (newCell->hasInfo() && (newCell->getOpen() || newCell->getClosed())) return; // already looked at this one.

		ICoord2D adjacentCell = scanCell;
		//DEBUG_ASSERTCRASH(parentZone==newCell->getZone(), ("Different zones?"));
		if (parentZone!=newCell->getZone()) return;
		adjacentCell.x += delta.x;
		adjacentCell.y += delta.y;
		if (adjacentCell.x<m_extent.lo.x || adjacentCell.x>m_extent.hi.x ||
			adjacentCell.y<m_extent.lo.y || adjacentCell.y>m_extent.hi.y) {
			return;
		}
		PathfindCell *adjNewCell = getCell(LAYER_GROUND, adjacentCell.x, adjacentCell.y);
		if (adjNewCell->hasInfo() && (adjNewCell->getOpen() || adjNewCell->getClosed())) return; // already looked at this one.
		zoneStorageType parentGlobalZone = m_zoneManager.getEffectiveZone(LOCOMOTORSURFACE_GROUND, crusher, parentZone);

		/// @todo - somehow out of bounds or bogus newZone.
		zoneStorageType newZone = m_zoneManager.getBlockZone(LOCOMOTORSURFACE_GROUND,
							crusher, adjacentCell.x, adjacentCell.y, m_map);
		zoneStorageType newGlobalZone = m_zoneManager.getEffectiveZone(LOCOMOTORSURFACE_GROUND, crusher, newZone);
		if (newGlobalZone != parentGlobalZone) {
			return; // can't step over. jba.
		}
		Int j;
		Bool found=false;
		for (j=0; j<numExZones; j++) {
			if (examinedZones[j] == newZone) {
				found = true;
				break;
			}
		}
		if (found) {
			return;
		}

		newCell->allocateInfo(scanCell);
#if RETAIL_COMPATIBLE_PATHFINDING
		if (!s_useFixedPathfinding)
		{
			// GeneralsX: hasInfo() ADDED - this arm dereferenced a null
			// info. allocateInfo's result is discarded just above, and getClosed()/getOpen() are
			// unguarded `m_info->` reads (AIPathfind.h:405-406). The sibling `else` arm below has
			// always carried this exact guard; only the default arm was missing it.
			//
			// ZERO HOUR CANNOT REACH IT, which is why it has survived: the #else at the top of this
			// function returns early when a cell has no info, so by here newCell always has one and
			// allocateInfo is a no-op returning true. The RTS_GENERALS arm has no such precondition
			// - a cell with no info falls straight through - and RTS_GENERALS=1 is set for the
			// shipped g_generals target (Generals/Code/CMakeLists.txt:42) with
			// RETAIL_COMPATIBLE_PATHFINDING defaulting to 1 (GameDefines.h:110-111).
			//
			// Reachable today whenever the pool is genuinely empty, so this is a live crash in
			// Generals, not a hypothetical: the hierarchical scan walks ordinary map cells, the
			// overwhelming majority of which hold no info.
			//
			// A no-op wherever the cell already has an info, i.e. everywhere Zero Hour goes.
			if (newCell->hasInfo() && !newCell->getClosed() && !newCell->getOpen()) {
				newCell->putOnClosedList(m_closedList);
			}
		}
		else
#endif
		{
			if (newCell->hasInfo() && !newCell->getClosed() && !newCell->getOpen()) {
				newCell->putOnClosedList(m_closedList);
			}
		}

		adjNewCell->allocateInfo(adjacentCell);
		if( adjNewCell->hasInfo() )
		{

			cellCount++;
			Int curCost = adjNewCell->costToHierGoal(parentCell);
			Int remCost = adjNewCell->costToHierGoal(goalCell);
			if (adjNewCell->getPinched() || newCell->getPinched()) {
				curCost += 2*COST_ORTHOGONAL;
			}	else {
				examinedZones[numExZones] = newZone;
				numExZones++;
			}

			adjNewCell->setCostSoFar(parentCell->getCostSoFar() + curCost);
			adjNewCell->setTotalCost(adjNewCell->getCostSoFar()+remCost);
			adjNewCell->setParentCellHierarchical(parentCell);
			// insert newCell in open list such that open list is sorted, smallest total path cost first
			adjNewCell->putOnSortedOpenList( m_openList );
		}

	}
}


/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::findHierarchicalPath( Bool isHuman, const LocomotorSet& locomotorSet, const Coord3D *from,
													 const Coord3D *to, Bool crusher)
{
	return internal_findHierarchicalPath(isHuman, locomotorSet.getValidSurfaces(), from, to, crusher, FALSE);
}


/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::findClosestHierarchicalPath( Bool isHuman, const LocomotorSet& locomotorSet, const Coord3D *from,
													 const Coord3D *to, Bool crusher)
{
	return internal_findHierarchicalPath(isHuman, locomotorSet.getValidSurfaces(), from, to, crusher, TRUE);
}



/**
 * Find a short, valid path between given locations.
 * Uses A* algorithm.
 */
Path *Pathfinder::internal_findHierarchicalPath( Bool isHuman, const LocomotorSurfaceTypeMask locomotorSurface, const Coord3D *from,
													 const Coord3D *rawTo, Bool crusher, Bool closestOK)
{
	//CRCDEBUG_LOG(("Pathfinder::findGroundPath()"));
#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif

	if (rawTo->x == 0.0f && rawTo->y == 0.0f) {
		DEBUG_LOG(("Attempting pathfind to 0,0, generally a bug."));
		return nullptr;
	}
	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	if (m_isMapReady == false) {
		return nullptr;
	}

	Coord3D adjustTo = *rawTo;
	Coord3D *to = &adjustTo;
	Coord3D clipFrom = *from;
	clip(&clipFrom, &adjustTo);

	m_isTunneling = false;

	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);

	ICoord2D cell;
	worldToCell( to, &cell );

	// determine goal cell
	PathfindCell *goalCell = getCell( destinationLayer, cell.x, cell.y );
	if (!goalCell) {
		return nullptr;
	}

	if (!goalCell->allocateInfo(cell)) {
		return nullptr;
	}

	// determine start cell
	ICoord2D startCellNdx;
	PathfindLayerEnum layer = TheTerrainLogic->getLayerForDestination(from);
	PathfindCell *parentCell = getClippedCell( layer,&clipFrom );
	if (!parentCell) {
		return nullptr;
	}

	if (parentCell!=goalCell) {
		worldToCell(&clipFrom, &startCellNdx);
		if (!parentCell->allocateInfo(startCellNdx)) {
			goalCell->releaseInfo();
			return nullptr;
		}
	}

	Int zone1, zone2;
	// m_isCrusher = false;
	zone1 = m_zoneManager.getEffectiveZone(locomotorSurface, false, parentCell->getZone());
	zone2 =  m_zoneManager.getEffectiveZone(locomotorSurface, false, goalCell->getZone());

	if ( zone1 != zone2) {
		goalCell->releaseInfo();
		parentCell->releaseInfo();
		return nullptr;
	}

	parentCell->startPathfind(goalCell);

	// "closed" list is initially empty
	m_closedList.reset();

	Int cellCount = 0;

	zoneStorageType goalBlockZone;
	ICoord2D goalBlockNdx;
	if (goalCell->getLayer()==LAYER_GROUND) {
		goalBlockZone = m_zoneManager.getBlockZone(locomotorSurface,
			crusher, goalCell->getXIndex(), goalCell->getYIndex(), m_map);

		goalBlockNdx.x = goalCell->getXIndex()/PathfindZoneManager::ZONE_BLOCK_SIZE;
		goalBlockNdx.y = goalCell->getYIndex()/PathfindZoneManager::ZONE_BLOCK_SIZE;
	}	else {
		goalBlockZone = goalCell->getZone();
		goalBlockNdx.x = -1;
		goalBlockNdx.y = -1;
	}

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	if (parentCell->getLayer()!=LAYER_GROUND) {
		PathfindLayerEnum layer = parentCell->getLayer();
		// We're starting on a bridge, so link to land at the bridge end points.
		ICoord2D ndx;
		ICoord2D toNdx;
		m_layers[layer].getStartCellIndex(&ndx);
		m_layers[layer].getEndCellIndex(&toNdx);
 		PathfindCell *cell = getCell(LAYER_GROUND, toNdx.x, toNdx.y);
		PathfindCell *startCell = getCell(LAYER_GROUND, ndx.x, ndx.y);
		if (cell && startCell) {
			// Close parent cell;
			parentCell->removeFromOpenList(m_openList);
			parentCell->putOnClosedList(m_closedList);
			if (!startCell->allocateInfo(ndx)) {
				// TheSuperHackers @info We need to forcefully cleanup dangling pathfinding cells if this failure condition is hit in retail
				// Retail clients will crash beyond this point, but we attempt to recover by performing a full cleanup then enabling the fixed pathfinding codepath
#if RETAIL_COMPATIBLE_PATHFINDING
				if (!s_useFixedPathfinding) {
					s_useFixedPathfinding = true;
					forceCleanCells();
				}
				else
#endif
				{
					cleanOpenAndClosedLists();
					goalCell->releaseInfo();
				}
				return nullptr;
			}
			startCell->setParentCellHierarchical(parentCell);
			cellCount++;
			Int curCost = startCell->costToHierGoal(parentCell);
			Int remCost = startCell->costToHierGoal(goalCell);
			startCell->setCostSoFar(curCost);
			startCell->setTotalCost(remCost);
			startCell->setParentCellHierarchical(parentCell);
			// insert newCell in open list such that open list is sorted, smallest total path cost first
			startCell->putOnSortedOpenList( m_openList );

			cellCount++;
			if(!cell->allocateInfo(toNdx)) {
				// TheSuperHackers @info We need to forcefully cleanup dangling pathfinding cells if this failure condition is hit in retail
				// Retail clients will crash beyond this point, but we attempt to recover by performing a full cleanup then enabling the fixed pathfinding codepath
#if RETAIL_COMPATIBLE_PATHFINDING
				if (!s_useFixedPathfinding) {
					s_useFixedPathfinding = true;
					forceCleanCells();
				}
				else
#endif
				{
					cleanOpenAndClosedLists();
					goalCell->releaseInfo();
				}
				return nullptr;
			}
			curCost = cell->costToHierGoal(parentCell);
			remCost = cell->costToHierGoal(goalCell);
			cell->setCostSoFar(curCost);
			cell->setTotalCost(remCost);
			cell->setParentCellHierarchical(parentCell);
			// insert newCell in open list such that open list is sorted, smallest total path cost first
			cell->putOnSortedOpenList( m_openList );
		}
	}

	PathfindCell *closestCell = nullptr;
	Real closestDistSqr = sqr(HUGE_DIST);
	PathfindCell *const searchStart = parentCell;	// GeneralsX: the root of this search's parent tree, see reachedGoal below

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//
	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_HIERARCHICAL); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		zoneStorageType parentZone;
		if (parentCell->getLayer()==LAYER_GROUND) {
			parentZone = m_zoneManager.getBlockZone(locomotorSurface,
				crusher, parentCell->getXIndex(), parentCell->getYIndex(), m_map);
		}	else {
			parentZone = parentCell->getZone();
		}

		Bool reachedGoal = false;

		Int blockX = parentCell->getXIndex()/PathfindZoneManager::ZONE_BLOCK_SIZE;
		Int blockY = parentCell->getYIndex()/PathfindZoneManager::ZONE_BLOCK_SIZE;
		if (parentZone == goalBlockZone) {
			if (goalBlockNdx.x == -1 || (blockX==goalBlockNdx.x && blockY == goalBlockNdx.y)) {
				reachedGoal = true;
			} else {
				DEBUG_LOG(("Hmm, got match before correct cell."));
			}
		}

		ICoord2D zoneBlockExtent;
		m_zoneManager.getExtent(zoneBlockExtent);

		if (!reachedGoal && m_zoneManager.interactsWithBridge(parentCell->getXIndex(), parentCell->getYIndex())) {
			Int i;
			for (i=0; i<=LAYER_LAST; i++) {
				if (m_layers[i].isUnused() || m_layers[i].isDestroyed()) {
					continue;
				}
				ICoord2D ndx;
				ICoord2D toNdx;
				m_layers[i].getStartCellIndex(&ndx);
				m_layers[i].getEndCellIndex(&toNdx);
				if (ndx.x/PathfindZoneManager::ZONE_BLOCK_SIZE != blockX ||
						ndx.y/PathfindZoneManager::ZONE_BLOCK_SIZE != blockY) {
					m_layers[i].getStartCellIndex(&toNdx);
					m_layers[i].getEndCellIndex(&ndx);
				}
				if (ndx.x<0 || ndx.y<0) continue;
				if (toNdx.x<0 || toNdx.y<0) continue;
				if (ndx.x/PathfindZoneManager::ZONE_BLOCK_SIZE == blockX &&
						ndx.y/PathfindZoneManager::ZONE_BLOCK_SIZE == blockY) {
					// Bridge connects to this block.
					Int bridgeZone = m_zoneManager.getBlockZone(locomotorSurface, crusher, ndx.x, ndx.y, m_map);
					if (bridgeZone != parentZone) {
						continue;
					}
					// We have a winner.
					if (m_layers[i].getZone() == goalBlockZone) {
						reachedGoal = true;
						break;
					}
 					PathfindCell *cell = getCell(LAYER_GROUND, toNdx.x, toNdx.y);
					if (!cell)
						continue;

					if (cell->hasInfo() && (cell->getClosed() || cell->getOpen()))
						continue;

					PathfindCell *startCell = getCell(LAYER_GROUND, ndx.x, ndx.y);
					if (!startCell)
						continue;

					if (startCell != parentCell) {
						if(!startCell->allocateInfo(ndx)) {
							// TheSuperHackers @info We need to forcefully cleanup dangling pathfinding cells if this failure condition is hit in retail
							// Retail clients will crash beyond this point, but we attempt to recover by performing a full cleanup then enabling the fixed pathfinding codepath
#if RETAIL_COMPATIBLE_PATHFINDING
							if (!s_useFixedPathfinding) {
								s_useFixedPathfinding = true;
								forceCleanCells();
							}
							else
#endif
							{
								cleanOpenAndClosedLists();
								goalCell->releaseInfo();
							}
							return nullptr;
						}
						startCell->setParentCellHierarchical(parentCell);
						if (!startCell->getClosed() && !startCell->getOpen()) {
							startCell->putOnClosedList(m_closedList);
						}
					}
					if(!cell->allocateInfo(toNdx)) {
						// TheSuperHackers @info We need to forcefully cleanup dangling pathfinding cells if this failure condition is hit in retail
						// Retail clients will crash beyond this point, but we attempt to recover by performing a full cleanup then enabling the fixed pathfinding codepath
#if RETAIL_COMPATIBLE_PATHFINDING
						if (!s_useFixedPathfinding) {
							s_useFixedPathfinding = true;
							forceCleanCells();
						}
						else
#endif
						{
							cleanOpenAndClosedLists();
							goalCell->releaseInfo();
						}
						return nullptr;
					}
					cell->setParentCellHierarchical(startCell);

					cellCount++;
					Int curCost = cell->costToHierGoal(startCell);
					Int remCost = cell->costToHierGoal(goalCell);

					cell->setCostSoFar(startCell->getCostSoFar() + curCost);
					cell->setTotalCost(cell->getCostSoFar()+remCost);
					cell->setParentCellHierarchical(startCell);
					// insert newCell in open list such that open list is sorted, smallest total path cost first
					cell->putOnSortedOpenList( m_openList );

				}
			}
		}

		if (reachedGoal)
		{
			// GeneralsX: THE SEARCH'S START NEVER TAKES A PARENT - a parent cycle froze every peer.
			// startPathfind() makes the start cell the root of the parent tree: every chain ends there, and this line is the
			// only one that gives the goal a parent. When the goal IS the start - a unit standing on a bridge (any layer but
			// the ground) asking for a path to its own cell - the start was never popped as the goal: the bridge branch above
			// closed it and seeded the ground cells at the bridge's two ends with it as their parent, and the goal test then
			// fired on one of those. The old line made the start that ground cell's child: start -> ground end -> start. When
			// the unit stands on the bridge's END cell, that ground cell has the start's own x,y, so prependCells() took every
			// step as a "layer transition" (which neither prepends, clears a parent nor moves prevCell) and never returned:
			// the logic frame never ended and every peer of the match froze for good. Measured (E . pathfind-freeze-1): cert64
			// Dry River, stock GLAStealth vs stock ChinaInfantry, seed 204088747, frame 18182 on 0.9.62 and 0.9.64 alike; the
			// same code is in EA's released source, TheSuperHackers main and Generals Online. Off the bridge's end the cycle ended only
			// because prependCells cleared it on the way, leaving a detour through a bridge end. The goal already is the
			// start, so the path is the start alone - what a ground start returns when it pops itself as the goal.
			// SIM-VISIBLE only for a start that is also the goal on a bridge (the freeze, or that detour); inside the digest.
			if (parentCell != goalCell && goalCell != searchStart) {
				goalCell->setParentCellHierarchical(parentCell);
			}
			// success - found a path to the goal

			m_isTunneling = false;
			// construct and return path
			Path *path =  buildHierarchicalPath( from, goalCell );
#if defined(RTS_DEBUG)
			Bool show = TheGlobalData->m_debugAI==AI_DEBUG_PATHS;
			show |= (TheGlobalData->m_debugAI==AI_DEBUG_GROUND_PATHS);
			if (show)	{
				debugShowSearch(true);
			}
#endif
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding)
			{
				if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
					goalCell->releaseInfo();
				}
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
				goalCell->releaseInfo();
			}
			return path;
		}

#if defined(RTS_DEBUG)
#if 0
		Bool show = TheGlobalData->m_debugAI==AI_DEBUG_PATHS;
		show |= (TheGlobalData->m_debugAI==AI_DEBUG_GROUND_PATHS);
		if (show)	{
			extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 			RGBColor color;
			color.blue = 1;
			color.red = 1;
			color.green = 0;
			Coord3D pos;
			pos.x = ((blockX+0.5f)*PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F ;
			pos.y = ((blockY+0.5f)*PathfindZoneManager::ZONE_BLOCK_SIZE)*PATHFIND_CELL_SIZE_F ;
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
			addIcon(&pos, 5*PATHFIND_CELL_SIZE_F, 300, color);
		}
#endif
#endif
		Real dx = IABS(goalCell->getXIndex()-parentCell->getXIndex());
		Real dy = IABS(goalCell->getYIndex()-parentCell->getYIndex());
		Real distSqr = dx*dx+dy*dy;
		if (distSqr < closestDistSqr) {
			closestCell = parentCell;
			closestDistSqr = distSqr;
		}

		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		Int i;
		zoneStorageType examinedZones[PathfindZoneManager::ZONE_BLOCK_SIZE];
		Int numExZones = 0;
		// Left side.
		if (blockX>0) {
			for (i=1; i<=PathfindZoneManager::ZONE_BLOCK_SIZE; i++) {
			ICoord2D scanCell;
				scanCell.x = blockX*PathfindZoneManager::ZONE_BLOCK_SIZE;
				scanCell.y = (blockY*PathfindZoneManager::ZONE_BLOCK_SIZE);
				scanCell.y += PathfindZoneManager::ZONE_BLOCK_SIZE/2;
				Int offset = i>>1;
				if (i&1) offset = -offset;
				scanCell.y += offset;
				ICoord2D delta;
				delta.x = -1; // left side moves -1.
				delta.y = 0;

				PathfindCell *cell = getCell(LAYER_GROUND, scanCell.x, scanCell.y);
				if (!cell)
					continue;

				if ( cell->hasInfo() && (cell->getClosed() || cell->getOpen()) ) {
					if (parentZone == m_zoneManager.getBlockZone(locomotorSurface, crusher, scanCell.x, scanCell.y, m_map))
						break;
				}

				if (isHuman && checkCellOutsideExtents(scanCell))
					continue;

				processHierarchicalCell(scanCell, delta, parentCell,
					goalCell, parentZone, examinedZones, numExZones, crusher, cellCount);
			}
		}
		// Right side.
		if (blockX<zoneBlockExtent.x-1) {
			numExZones = 0;
			for (i=1; i<=PathfindZoneManager::ZONE_BLOCK_SIZE; i++) {
			ICoord2D scanCell;
				scanCell.x = blockX*PathfindZoneManager::ZONE_BLOCK_SIZE;
				scanCell.x += PathfindZoneManager::ZONE_BLOCK_SIZE-1;
				scanCell.y = (blockY*PathfindZoneManager::ZONE_BLOCK_SIZE);
				scanCell.y += PathfindZoneManager::ZONE_BLOCK_SIZE/2;
				Int offset = i>>1;
				if (i&1) offset = -offset;
				scanCell.y += offset;
				ICoord2D delta;
				delta.x = 1; // right side moves +1.
				delta.y = 0;

				PathfindCell *cell = getCell(LAYER_GROUND, scanCell.x, scanCell.y);
				if (!cell)
					continue;

				if ( cell->hasInfo() && (cell->getClosed() || cell->getOpen()) ) {
					if (parentZone == m_zoneManager.getBlockZone(locomotorSurface, crusher, scanCell.x, scanCell.y, m_map))
						break;
				}

				if (isHuman && checkCellOutsideExtents(scanCell))
					continue;

				processHierarchicalCell(scanCell, delta, parentCell,
					goalCell, parentZone, examinedZones, numExZones, crusher, cellCount);
			}
		}
		// Top side.
		if (blockY>0) {
			numExZones = 0;
			for (i=1; i<=PathfindZoneManager::ZONE_BLOCK_SIZE; i++) {
			ICoord2D scanCell;
				scanCell.y = blockY*PathfindZoneManager::ZONE_BLOCK_SIZE;
				scanCell.x = (blockX*PathfindZoneManager::ZONE_BLOCK_SIZE);
				scanCell.x += PathfindZoneManager::ZONE_BLOCK_SIZE/2;
				Int offset = i>>1;
				if (i&1) offset = -offset;
				scanCell.x += offset;
				ICoord2D delta;
				delta.x = 0;
				delta.y = -1;	// Top side moves -1.

				PathfindCell *cell = getCell(LAYER_GROUND, scanCell.x, scanCell.y);
				if (!cell)
					continue;

				if ( cell->hasInfo() && (cell->getClosed() || cell->getOpen()) ) {
					if (parentZone == m_zoneManager.getBlockZone(locomotorSurface, crusher, scanCell.x, scanCell.y, m_map))
						break;
				}

				if (isHuman && checkCellOutsideExtents(scanCell))
					continue;

				processHierarchicalCell(scanCell, delta, parentCell,
					goalCell, parentZone, examinedZones, numExZones, crusher, cellCount);
			}
		}
		// Bottom side.
		if (blockY<zoneBlockExtent.y-1) {
			numExZones = 0;
			for (i=1; i<=PathfindZoneManager::ZONE_BLOCK_SIZE; i++) {
			ICoord2D scanCell;
				scanCell.y = blockY*PathfindZoneManager::ZONE_BLOCK_SIZE;
				scanCell.y += PathfindZoneManager::ZONE_BLOCK_SIZE-1;
				scanCell.x = (blockX*PathfindZoneManager::ZONE_BLOCK_SIZE);
				scanCell.x += PathfindZoneManager::ZONE_BLOCK_SIZE/2;
				Int offset = i>>1;
				if (i&1) offset = -offset;
				scanCell.x += offset;
				ICoord2D delta;
				delta.x = 0;
				delta.y = 1; // Top side moves +1.

				PathfindCell *cell = getCell(LAYER_GROUND, scanCell.x, scanCell.y);
				if (!cell)
					continue;

				if ( cell->hasInfo() && (cell->getClosed() || cell->getOpen()) ) {
					if (parentZone == m_zoneManager.getBlockZone(locomotorSurface, crusher, scanCell.x, scanCell.y, m_map))
						break;
				}

				if (isHuman && checkCellOutsideExtents(scanCell))
					continue;

				processHierarchicalCell(scanCell, delta, parentCell,
					goalCell, parentZone, examinedZones, numExZones, crusher, cellCount);
			}
		}
	}

	if (closestOK && closestCell) {
		m_isTunneling = false;
		// construct and return path
		Path *path =  buildHierarchicalPath( from, closestCell );

#if RETAIL_COMPATIBLE_PATHFINDING
		if (!s_useFixedPathfinding)
		{
			if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
				goalCell->releaseInfo();
			}
			cleanOpenAndClosedLists();
		}
		else
#endif
		{
			cleanOpenAndClosedLists();
			parentCell->releaseInfo();
			goalCell->releaseInfo();
		}
		return path;
	}

	// failure - goal cannot be reached
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		addIcon(nullptr, 0, 0, color);
		debugShowSearch(false);
		Coord3D pos;
		pos = *from;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		pos = *to;
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
		addIcon(&pos, 3*PATHFIND_CELL_SIZE_F, 600, color);
		Real dx, dy;
		dx = from->x - to->x;
		dy = from->y - to->y;

		Int count = sqrt(dx*dx+dy*dy)/(PATHFIND_CELL_SIZE_F/2);
		if (count<2) count = 2;
		Int i;
		color.green = 0;
		for (i=1; i<count; i++) {
			pos.x = from->x + (to->x-from->x)*i/count;
			pos.y = from->y + (to->y-from->y)*i/count;
			pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y ) + 0.5f;
			addIcon(&pos, PATHFIND_CELL_SIZE_F/2, 60, color);

		}
	}
#endif

	DEBUG_LOG(("%d FindHierarchicalPath failed from (%f,%f) to (%f,%f) --", TheGameLogic->getFrame(), from->x, from->y, to->x, to->y));
	DEBUG_LOG(("time %f", (::GetTickCount()-startTimeMS)/1000.0f));

#ifdef DUMP_PERF_STATS
	TheGameLogic->incrementOverallFailedPathfinds();
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding)
	{
		goalCell->releaseInfo();
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}

	return nullptr;
}


/**
 * Does any broken bridge join from and to?
 * True means that if bridge BridgeID is repaired, there is a land path from to to..
 */
Bool Pathfinder::findBrokenBridge(const LocomotorSet& locoSet,
																	const Coord3D *from, const Coord3D *to, ObjectID *bridgeID)
{
	// See if terrain or building is blocking the destination.
	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);
	PathfindLayerEnum fromLayer = TheTerrainLogic->getLayerForDestination(from);

	Int zone1, zone2;
	*bridgeID = INVALID_ID;

	PathfindCell *parentCell = getClippedCell(fromLayer, from);
	PathfindCell *goalCell = getClippedCell(destinationLayer, to);
	zone1 = m_zoneManager.getEffectiveZone(locoSet.getValidSurfaces(), false, parentCell->getZone());
	zone2 =  m_zoneManager.getEffectiveZone(locoSet.getValidSurfaces(), false, goalCell->getZone());
	zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
	zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
	zone1 = m_zoneManager.getEffectiveZone(locoSet.getValidSurfaces(), false, zone1);
	zone2 =  m_zoneManager.getEffectiveZone(locoSet.getValidSurfaces(), false, zone2);

	// If the terrain is connected using this locomotor set, we can path somehow.
	if (zone1 == zone2) {
		// There is not terrain blocking the from & to.
		return false;
	}

	// Check broken bridges.
	Int i;
	for (i=0; i<=LAYER_LAST; i++) {
		if (m_layers[i].isDestroyed()) {
			if (m_layers[i].connectsZones(&m_zoneManager, locoSet, zone1, zone2)) {
				*bridgeID = m_layers[i].getBridgeID();
				return true;
			}
		}
	}
	return false;
}

/**
 * Does any path exist from 'from' to 'to' given the locomotor set
 * This is the quick check, only looks at whether the terrain is possible or
 * impossible to path over.  Doesn't take other units into account.
 * False means it is impossible to path.
 * True means it is possible given the terrain, but there may be units in the way.
 */
Bool Pathfinder::clientSafeQuickDoesPathExist( const LocomotorSet& locomotorSet,
																const Coord3D *from,
																const Coord3D *to )
{
	// See if terrain or building is blocking the destination.
	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);
	if (!validMovementPosition(false, destinationLayer, locomotorSet, to)) {
		return false;
	}
	PathfindLayerEnum fromLayer = TheTerrainLogic->getLayerForDestination(from);
	Int zone1, zone2;

	PathfindCell *parentCell = getClippedCell(fromLayer, from);
	PathfindCell *goalCell = getClippedCell(destinationLayer, to);
	if (goalCell->getType()==PathfindCell::CELL_CLIFF) {
		return false; // No goals on cliffs.
	}
	Bool doingTerrainZone = false;
	zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, parentCell->getZone());

	if (parentCell->getType() == PathfindCell::CELL_OBSTACLE) {
		doingTerrainZone = true;
#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
		if (zone1 == PathfindZoneManager::UNINITIALIZED_ZONE) {
			// We are in a building that just got placed, and zones haven't been updated yet. [8/8/2003]
			// It is better to return a false positive than a false negative. jba.
			return true;
		}
#endif
	}
	zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, goalCell->getZone());
	if (goalCell->getType() == PathfindCell::CELL_OBSTACLE) {
		doingTerrainZone = true;
	}
	if (doingTerrainZone) {
		zone1 = parentCell->getZone();
		zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
		zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, zone1);
		zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
		zone2 = goalCell->getZone();
		zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
		zone2 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, zone2);
		zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
	}
	// If the terrain is connected using this locomotor set, we can path somehow.
	if (zone1 == zone2) {
		// There is not terrain blocking the from & to.
		return true;
	}
	return FALSE;  // no path exists

}

/**
 * Does any path exist from 'from' to 'to' given the locomotor set
 * This is the quick check, only looks at whether the terrain is possible or
 * impossible to path over.  Doesn't take other units into account.
 * False means it is impossible to path.
 * True means it is possible given the terrain, but there may be units in the way.
 */
Bool Pathfinder::clientSafeQuickDoesPathExistForUI( const LocomotorSet& locomotorSet,
																const Coord3D *from,
																const Coord3D *to )
{
	// See if terrain or building is blocking the destination.
	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);
	PathfindLayerEnum fromLayer = TheTerrainLogic->getLayerForDestination(from);
	Int zone1, zone2;

	PathfindCell *parentCell = getClippedCell(fromLayer, from);
	PathfindCell *goalCell = getClippedCell(destinationLayer, to);
	if (goalCell->getType()==PathfindCell::CELL_CLIFF) {
		return false; // No goals on cliffs.
	}

	zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, parentCell->getZone());
	zone2 =  m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, goalCell->getZone());

	if (zone1 == PathfindZoneManager::UNINITIALIZED_ZONE ||
			zone2 == PathfindZoneManager::UNINITIALIZED_ZONE) {
		// We are in a building that just got placed, and zones haven't been updated yet. [8/8/2003]
		// It is better to return a false positive than a false negative. jba.
		return true;
	}
	/* Do the effective terrain zone.  This feedback is for the ui, so we won't take structures into account,
		because if they are visible it will be obvious, and if they are stealthed they should be invisible to the
		pathing as well. jba. */
	zone1 = parentCell->getZone();
	zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
	zone1 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, zone1);
	zone1 = m_zoneManager.getEffectiveTerrainZone(zone1);
	zone2 = goalCell->getZone();
	zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);
	zone2 = m_zoneManager.getEffectiveZone(locomotorSet.getValidSurfaces(), false, zone2);
	zone2 = m_zoneManager.getEffectiveTerrainZone(zone2);

	if (zone1 == PathfindZoneManager::UNINITIALIZED_ZONE) {
		// We are in a building that just got placed, and zones haven't been updated yet. [8/8/2003]
		// It is better to return a false positive than a false negative. jba.
		return true;
	}
	// If the terrain is connected using this locomotor set, we can path somehow.
	if (zone1 == zone2) {
		// There is not terrain blocking the from & to.
		return true;
	}
	return FALSE;  // no path exists

}

/**
 * Does any path exist from 'from' to 'to' given the locomotor set
 * This is the careful check, looks at whether the terrain, buindings and units are possible or
 * impossible to path over.  Takes other units into account.
 * False means it is impossible to path.
 * True means it is possible to path.
 */
Bool Pathfinder::slowDoesPathExist( Object *obj,
																const Coord3D *from,
																const Coord3D *to,
																ObjectID ignoreObject)
{
	AIUpdateInterface *ai = obj->getAI();
	if (ai==nullptr) {
		return false;
	}
	const LocomotorSet &locoSet = ai->getLocomotorSet();
	m_ignoreObstacleID = ignoreObject;
	Path *path = findPath(obj, locoSet, from, to);
	m_ignoreObstacleID = INVALID_ID;
	Bool found = (path!=nullptr);

	deleteInstance(path);
	path = nullptr;

	return found;
}

void Pathfinder::clip( Coord3D *from, Coord3D *to )
{
	ICoord2D fromCell, toCell;
	ICoord2D clipFromCell, clipToCell;
	fromCell.x = REAL_TO_INT_FLOOR(from->x/PATHFIND_CELL_SIZE);
	fromCell.y = REAL_TO_INT_FLOOR(from->y/PATHFIND_CELL_SIZE);
	toCell.x = REAL_TO_INT_FLOOR(to->x/PATHFIND_CELL_SIZE);
	toCell.y = REAL_TO_INT_FLOOR(to->y/PATHFIND_CELL_SIZE);
	if (ClipLine2D(&fromCell, &toCell, &clipFromCell, &clipToCell,&m_extent)) {
		if (fromCell.x!=clipFromCell.x || fromCell.y != clipFromCell.y) {
			from->x = clipFromCell.x*PATHFIND_CELL_SIZE_F + 0.05f;
			from->y = clipFromCell.y*PATHFIND_CELL_SIZE_F + 0.05f;
		}
		if (toCell.x!=clipToCell.x || toCell.y != clipToCell.y) {
			to->x = clipToCell.x*PATHFIND_CELL_SIZE_F + 0.05f;
			to->y = clipToCell.y*PATHFIND_CELL_SIZE_F + 0.05f;
		}
	}

}

struct TightenPathStruct
{
	Object *obj;
	const LocomotorSet *locomotorSet;
	PathfindLayerEnum layer;
	Int		radius;
	Bool	center;
	Bool	foundNewDest;
	Coord3D orgDestPos;
	Coord3D newDestPos;
};


/*static*/ Int Pathfinder::tightenPathCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	TightenPathStruct* d = (TightenPathStruct*)userData;
	if (from == nullptr || to==nullptr) return 0; // failure
	if (d->layer != to->getLayer()) {
		return 0; // failure
	}

#if RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @bugfix Caball009 27/02/2026 This was originally uninitialized.
	// The uninitialized values that retail uses here are usually close to zero as long as foundNewDest == false, otherwise it uses the new values of newDestPos.
	// newDestPos is zero initialized by the caller, so there is no need to check foundNewDest here.
	Coord3D pos = d->newDestPos;
#else
	Coord3D pos = d->orgDestPos;
#endif

	if (!TheAI->pathfinder()->checkForAdjust(d->obj, *d->locomotorSet, true, to_x, to_y, to->getLayer(), d->radius, d->center, &pos, nullptr))
	{
		return 0; // failure
	}
	d->foundNewDest = true;
	d->newDestPos = pos;

	return 0; // success but continue
}

/* Returns the cost, which is in the same units as coord3d distance. */
void Pathfinder::tightenPath(Object *obj, const LocomotorSet& locomotorSet, Coord3D *from,
		const Coord3D *to)
{
	TightenPathStruct info;

	getRadiusAndCenter(obj, info.radius, info.center);
	info.layer = TheTerrainLogic->getLayerForDestination(from);
	info.obj = obj;
	info.locomotorSet = &locomotorSet;
	info.foundNewDest = false;
	info.orgDestPos = *to;
#if RETAIL_COMPATIBLE_CRC
	info.newDestPos.zero();
#endif
	iterateCellsAlongLine(*from, *to, info.layer, tightenPathCallback, &info);
	if (info.foundNewDest) {
		*from = info.newDestPos;
	}
}


/* Returns the cost, which is in the same units as coord3d distance. */
Int Pathfinder::checkPathCost(Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
		const Coord3D *rawTo)
{
	//CRCDEBUG_LOG(("Pathfinder::checkPathCost()"));
	if (m_isMapReady == false) return 0;
	enum {MAX_COST = 0x7fff0000};
	if (!obj) return MAX_COST;

	Int cellCount = 0;

	Coord3D adjustTo = *rawTo;
	Coord3D *to = &adjustTo;
	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	// create unique "mark" values for open and closed cells for this pathfind invocation

	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;

	PathfindLayerEnum goalLayer = TheTerrainLogic->getLayerForDestination(to);
	// determine goal cell
	PathfindCell *goalCell = getClippedCell( goalLayer,  to );
	if (goalCell == nullptr)
		return MAX_COST;


	Bool center;
	Int radius;
	getRadiusAndCenter(obj, radius, center);

	// determine start cell
	ICoord2D startCellNdx;
	worldToCell(from, &startCellNdx);
	PathfindLayerEnum fromLayer = TheTerrainLogic->getLayerForDestination(from);
	PathfindCell *parentCell = getCell( fromLayer, from );
	if (parentCell == nullptr)
		return MAX_COST;
	ICoord2D pos2d;
	worldToCell(to, &pos2d);
	if (!goalCell->allocateInfo(pos2d)) {
		return MAX_COST;
	}

	if (parentCell!=goalCell) {
		if (!parentCell->allocateInfo(startCellNdx)) {
			goalCell->releaseInfo();
			return MAX_COST;
		}
	}

	if (validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), parentCell ) == false) {
		parentCell->releaseInfo();
		goalCell->releaseInfo();
		return MAX_COST;
	}

	parentCell->startPathfind(goalCell);

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//
	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_CHECK_COST); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		// put parent cell onto closed list - its evaluation is finished - Retail compatible behaviour
#if RETAIL_COMPATIBLE_PATHFINDING
		if (!s_useFixedPathfinding) {
			parentCell->putOnClosedList( m_closedList );
		}
#endif

		if (parentCell==goalCell) {
			Int cost = parentCell->getTotalCost();
			m_isTunneling = false;
			cleanOpenAndClosedLists();
#if RETAIL_COMPATIBLE_PATHFINDING
			if (s_useFixedPathfinding)
#endif
			{
				parentCell->releaseInfo();
			}
			return cost;
		}

		// put parent cell onto closed list - its evaluation is finished - Fixed behaviour
#if RETAIL_COMPATIBLE_PATHFINDING
		if (s_useFixedPathfinding)
#endif
		{
			parentCell->putOnClosedList( m_closedList );
		}

		if (cellCount > MAX_CELL_COUNT) {
			continue;
		}
		// Check to see if we can change layers in this cell.
		checkChangeLayers(parentCell);

		// expand search to neighboring orthogonal cells
		static ICoord2D delta[] =
		{
			{ 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
			{ 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
		};
		const Int numNeighbors = 8;
		const Int firstDiagonal = 4;
		ICoord2D newCellCoord;
		PathfindCell *newCell;
		const Int adjacent[5] = {0, 1, 2, 3, 0};
		Bool neighborFlags[8] = { 0 };

		UnsignedInt newCostSoFar = 0;

		for( int i=0; i<numNeighbors; i++ )
		{
			neighborFlags[i] = false;
			// determine neighbor cell to try
			newCellCoord.x = parentCell->getXIndex() + delta[i].x;
			newCellCoord.y = parentCell->getYIndex() + delta[i].y;

			// get the neighboring cell
			newCell = getCell(parentCell->getLayer(), newCellCoord.x, newCellCoord.y );

			// check if cell is on the map
			if (newCell == nullptr)
				continue;

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			Bool onList = false;
			if (newCell->hasInfo()) {
				if (newCell->getOpen() || newCell->getClosed())
				{
					// already on one of the lists
					onList = true;
				}
			}
			if (i>=firstDiagonal) {
				// make sure one of the adjacent sides is open.
				if (!neighborFlags[adjacent[i-4]] && !neighborFlags[adjacent[i-3]]) {
					continue;
				}
			}

			if (!validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), newCell, parentCell )) {
				continue;
			}

			neighborFlags[i] = true;

			if (!newCell->allocateInfo(newCellCoord)) {
				// Out of cells for pathing...
				// GeneralsX: THE CLEANUP WAS GATED; IT IS THE ONLY EXIT
				// THAT GATED IT.
				//
				// This block used to sit under `if (s_useFixedPathfinding)`, which is false by
				// default, so the refusal returned with m_openList and m_closedList still populated.
				// By this point parentCell has already been pushed onto the closed list (the retail
				// arm does it immediately after popping it off the open list), and
				// PathfindCellList::reset() only reassigns head/tail/count - it does not clear
				// m_open/m_closed and does not release infos. PathfindCell::releaseInfo() then
				// refuses to release a cell still marked open or closed ("better leak than crash"),
				// so those cells are stranded holding pool entries for the rest of the match, and
				// the pool ratchets down instead of recovering.
				//
				// EVERY OTHER EXIT OF THIS FUNCTION ALREADY CLEANS UP UNCONDITIONALLY - the success
				// exit calls cleanOpenAndClosedLists() outside the gate and only conditions the
				// releaseInfo, and the final no-path exit cleans up in BOTH arms. This one was the
				// outlier, which is what marks it an oversight rather than a deliberate retail
				// divergence.
				//
				// Shaped to match that final no-path exit exactly, because after the return-value
				// fix below the two exits are now the same answer reached two ways.
#if RETAIL_COMPATIBLE_PATHFINDING
				if (!s_useFixedPathfinding) {
					if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
						goalCell->releaseInfo();
					}
					cleanOpenAndClosedLists();
				}
				else
#endif
				{
					cleanOpenAndClosedLists();
					parentCell->releaseInfo();
					goalCell->releaseInfo();
				}
				// GeneralsX: WAS `return cellCount`, which is 0 here.
				//
				// Every other exit from checkPathCost returns either a real A* total cost or
				// MAX_COST; this one returned the number of cells expanded so far, and cellCount is
				// incremented on the line BELOW, so the first refused allocation returns exactly 0.
				//
				// The sole consumer is checkForAdjust: `if (1.4f*(dx+dy) < cost) return false;`.
				// dx and dy are IABS values, so with cost 0 that test is false for every input and
				// the guard - which exists to REJECT an adjusted destination that is a long detour -
				// silently accepts every one of them. Units then take visible detours around
				// obstacles, under load, in exactly the big matches where the pool is tight.
				//
				// MAX_COST is the honest answer: "this could not be evaluated" belongs on the
				// reject side of a cost test, not the accept side.
 				return MAX_COST;
			}
			cellCount++;

			newCostSoFar = newCell->costSoFar( parentCell );
			newCell->setBlockedByAlly(false);

			// check if this neighbor cell is already on the open (waiting to be tried)
			// or closed (already tried) lists
			if (onList)
			{
				// already on one of the lists - if existing costSoFar is less,
				// the new cell is on a longer path, so skip it
				if (newCell->getCostSoFar() <= newCostSoFar)
					continue;
			}

			// keep track of path we're building - point back to cell we moved here from
			newCell->setParentCell(parentCell) ;

			// store cost of this path
			newCell->setCostSoFar(newCostSoFar);

			Int costRemaining = 0;
			if (goalCell) {
				costRemaining = newCell->costToGoal( goalCell );
			}

			newCell->setTotalCost(newCell->getCostSoFar() + costRemaining) ;

			// if newCell was on closed list, remove it from the list
			if (newCell->getClosed())
				newCell->removeFromClosedList( m_closedList );

			// if the newCell was already on the open list, remove it so it can be re-inserted in order
			if (newCell->getOpen())
				newCell->removeFromOpenList( m_openList );

#if RETAIL_COMPATIBLE_PATHFINDING
			// TheSuperHacker @info This is here to catch a retail pathfinding crash point and to recover from it
			// A cell has gotten onto the open list without pathfinding info due to a danling m_open pointer on the previous listed cell so we need to force a cleanup
			if (!s_useFixedPathfinding && m_openList.getHead() && !m_openList.getHead()->hasInfo()) {
				s_useFixedPathfinding = true;
				forceCleanCells();
				return MAX_COST;
			}
#endif

			// insert newCell in open list such that open list is sorted, smallest total path cost first
			newCell->putOnSortedOpenList( m_openList );
		}
	}

	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
			goalCell->releaseInfo();
		}
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}
	return MAX_COST;
}


/**
 * Find a short, valid path between the FROM location and a location NEAR the to location.
 * Uses A* algorithm.
 */
Path *Pathfinder::findClosestPath( Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
																	Coord3D *rawTo, Bool blocked, Real pathCostMultiplier, Bool moveAllies)
{
	//CRCDEBUG_LOG(("Pathfinder::findClosestPath()"));
#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif
	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}

	if (locomotorSet.getValidSurfaces() == 0) {
		DEBUG_CRASH(("Attempting to path immobile unit."));
		return nullptr;
	}

	if (m_isMapReady == false) return nullptr;

	m_isTunneling = false;

	if (!obj) return nullptr;

	Bool canPathThroughUnits = false;
	if (obj && obj->getAIUpdateInterface()) {
		canPathThroughUnits = obj->getAIUpdateInterface()->canPathThroughUnits();
	}
	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);

	Coord3D adjustTo = *rawTo;
	Coord3D *to = &adjustTo;
	if (!centerInCell) {
		adjustTo.x += PATHFIND_CELL_SIZE_F/2;
		adjustTo.y += PATHFIND_CELL_SIZE_F/2;
	}
	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	// create unique "mark" values for open and closed cells for this pathfind invocation

	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;


	Coord3D clipFrom = *from;
	clip(&clipFrom, &adjustTo);

	PathfindLayerEnum destinationLayer = TheTerrainLogic->getLayerForDestination(to);
	// determine goal cell
	PathfindCell *goalCell = getClippedCell( destinationLayer,  to );
	if (goalCell == nullptr)
		return nullptr;

	if (goalCell->getZone()==0 && destinationLayer==LAYER_WALL) {
		return nullptr;
	}

	Bool goalOnObstacle = false;
	if (m_ignoreObstacleID != INVALID_ID) {
		// Check for object on structure.
		// srj sez: check for obstacle on AIRFIELD... only want to do this for things
		// that are "parked" on the airfield, but not for things hovering over an obstacle
		// (eg, a chinook over a supply dock).
		Object *goalObj = TheGameLogic->findObjectByID(m_ignoreObstacleID);
		if (goalObj) {
			PathfindCell *ignoreCell = getClippedCell(goalObj->getLayer(), goalObj->getPosition());
			if ( (goalCell->getObstacleID()==ignoreCell->getObstacleID()) && (goalCell->getObstacleID() != INVALID_ID) ) {
				Object* newObstacle = TheGameLogic->findObjectByID(goalCell->getObstacleID());
				if (newObstacle != nullptr && newObstacle->isKindOf(KINDOF_FS_AIRFIELD))
				{
					m_ignoreObstacleID = goalCell->getObstacleID();
					goalOnObstacle = true;
				}
				else
				{
					if (m_ignoreObstacleID == goalCell->getObstacleID()) {
						goalOnObstacle = true;
					}
				}
			}
		}
	}

	// determine start cell
	ICoord2D startCellNdx;
	worldToCell(from, &startCellNdx);
 	PathfindCell *parentCell = getClippedCell( obj->getLayer(), &clipFrom );
	if (parentCell == nullptr)
		return nullptr;

	if (validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), parentCell ) == false) {
		m_isTunneling = true; // We can't move from our current location.  So relax the constraints.
	}
	TCheckMovementInfo info;
	info.cell = startCellNdx;
	info.layer = obj->getLayer();
	info.centerInCell = centerInCell;
	info.radius = radius;
	info.considerTransient = blocked;
	info.acceptableSurfaces = locomotorSet.getValidSurfaces();
	if (!checkForMovement(obj, info) || info.enemyFixed) {
		m_isTunneling = true; // We can't move from our current location.  So relax the constraints.
	}

	Bool gotHierarchicalPath = false;
	if (m_isTunneling) {
		m_zoneManager.setAllPassable(); // can't optimize.
	}	else {
		m_zoneManager.clearPassableFlags();
		Path *hPat = findClosestHierarchicalPath(isHuman, locomotorSet, from, rawTo, false);
		if (hPat) {
			deleteInstance(hPat);
			gotHierarchicalPath = true;
		}	else {
			m_zoneManager.setAllPassable();
		}
	}
	const Bool startedStuck = m_isTunneling;

	ICoord2D pos2d;
	worldToCell(to, &pos2d);
	if (!goalCell->allocateInfo(pos2d)) {
		return nullptr;
	}
	if (parentCell!=goalCell) {
		worldToCell(&clipFrom, &pos2d);
		if (!parentCell->allocateInfo(pos2d)) {
#if RETAIL_COMPATIBLE_PATHFINDING
			if (s_useFixedPathfinding)
#endif
			{
				goalCell->releaseInfo();
			}
			return nullptr;
		}
	}
	parentCell->startPathfind(goalCell);

	PathfindCell *closesetCell = nullptr;
	Real closestDistanceSqr = FLT_MAX;
	Real closestDistScreenSqr = FLT_MAX;

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();
	Int count = 0;
	// GeneralsX: Baseline for the allocation half of the expansion bound -
	// see MAX_CLOSEST_PATH_INFO_COUNT. Taken after the goal and start cells are allocated above, so
	// the budget covers only what the search itself takes.
	const UnsignedInt infosAtSearchStart = s_pathfindCellInfoTakes;
	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//
	Bool foundGoal = false;
	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_CLOSEST); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		Real dx;
		Real dy;
		Real distSqr;
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		if (parentCell == goalCell)
		{
			// success - found a path to the goal
			if (!goalOnObstacle) {
				// See if the goal is a valid destination.  If not, accept closest cell.
				if (closesetCell!=nullptr && !canPathThroughUnits && !checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(), parentCell->getLayer(), radius, centerInCell)) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
					break;
#else
					foundGoal = true;
					// Continue processing the open list to find a possibly closer cell. jba. [8/25/2003]
					continue;
#endif
				}
			}

			Bool show = TheGlobalData->m_debugAI;
#ifdef INTENSE_DEBUG
			Int count = 0;
			PathfindCell *cur;
			for (cur = m_closedList.getHead(); cur; cur=cur->getNextOpen()) {
				count++;
			}
			if (count>1000) {
				show = true;
				DEBUG_LOG(("FCP - cells %d obj %s %x", count, obj->getTemplate()->getName().str(), obj));
#ifdef STATE_MACHINE_DEBUG
				if( obj->getAIUpdateInterface() )
				{
					DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
				}
#endif
				TheScriptEngine->AppendDebugMessage("Big path FCP", false);
			}
#endif
			if (show)
				debugShowSearch(true);
			m_isTunneling = false;
			// construct and return path
			Path *path = buildActualPath( obj, locomotorSet.getValidSurfaces(), from, goalCell, centerInCell, blocked);
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding) {
				parentCell->releaseInfo();
				goalCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
				goalCell->releaseInfo();
			}

			return path;
		}
		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );
		if (!m_isTunneling && checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(), parentCell->getLayer(), radius, centerInCell)) {
			if (!startedStuck || validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), parentCell )) {
				dx = IABS(goalCell->getXIndex()-parentCell->getXIndex());
				dy = IABS(goalCell->getYIndex()-parentCell->getYIndex());
				distSqr = dx*dx+dy*dy;
				if (distSqr<closestDistScreenSqr) {
					closestDistScreenSqr = distSqr;
				}
				distSqr += (parentCell->getCostSoFar()*(parentCell->getCostSoFar()*COST_TO_DISTANCE_FACTOR_SQR))*pathCostMultiplier;
				if (distSqr < closestDistanceSqr) {
					closesetCell = parentCell;
					closestDistanceSqr = distSqr;
				}
			}
		}

		dx = IABS(goalCell->getXIndex()-parentCell->getXIndex());
		dy = IABS(goalCell->getYIndex()-parentCell->getYIndex());
		distSqr = dx*dx+dy*dy;
		// If we are 2x farther than the closest location already found, don't continue.
		if (distSqr > closestDistScreenSqr*4) {
			Bool skip = false;
			if (!gotHierarchicalPath) {
				skip = true;
			}
			if (count>2000) {
				skip = true;
			}
			if (closestDistScreenSqr < 10*10*PATHFIND_CELL_SIZE_F) {
				skip = true;
			}
			if (skip) {
				continue;
			}
		}
		// If we haven't already found the goal cell, continue examining. [8/25/2003]
		if (!foundGoal) {
			// GeneralsX: The expansion bound - see MAX_CLOSEST_PATH_CELL_COUNT,
			// MAX_CLOSEST_PATH_INFO_COUNT and PATHFIND_SEARCH_FREE_RESERVE. The reserve is the one
			// that states the property: the pool does not reach zero. The other two bound this search
			// in its own terms, and hold whatever the pool size is.
			// Same shape as patchPath's `if (cellCount < CELL_LIMIT)` and findAttackPath's
			// ATTACK_CELL_LIMIT: it stops the search GROWING, it does not break the loop. The open
			// list still drains through the closest-cell bookkeeping above, so the answer is the best
			// of everything already opened - and because the line prepass seeds cells along the way to
			// the goal, that best is usually close to it. The function then leaves by one of its three
			// existing exits (goal found, closest found, failure), each of which already releases the
			// goal and start cells and calls cleanOpenAndClosedLists. This adds no exit path, so there
			// is no exit path with different cleanup.
			if ((UnsignedInt)count < MAX_CLOSEST_PATH_CELL_COUNT &&
					pathfindSearchMayGrow(infosAtSearchStart, MAX_CLOSEST_PATH_INFO_COUNT)) {
				// Check to see if we can change layers in this cell.
				checkChangeLayers(parentCell);
				count += examineNeighboringCells(parentCell, goalCell, locomotorSet, isHuman, centerInCell, radius, startCellNdx, obj, NO_ATTACK);
			}
		}
	}

	if (closesetCell) {
		// success - found a path to near the goal

		Bool show = TheGlobalData->m_debugAI;

#ifdef INTENSE_DEBUG
		if (count>5000) {
			show = true;
			DEBUG_LOG(("FCP CC cells %d obj %s %x", count, obj->getTemplate()->getName().str(), obj));
#ifdef STATE_MACHINE_DEBUG
			if( obj->getAIUpdateInterface() )
			{
				DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
			}
#endif

			DEBUG_LOG(("%d Pathfind(findClosestPath) chugged from (%f,%f) to (%f,%f) --", TheGameLogic->getFrame(), from->x, from->y, to->x, to->y));
			DEBUG_LOG(("Unit '%s', time %f", obj->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));
#ifdef INTENSE_DEBUG
			TheScriptEngine->AppendDebugMessage("Big path FCP CC", false);
#endif
		}
#endif
		if (show)
			debugShowSearch(true);

		m_isTunneling = false;
		rawTo->x = closesetCell->getXIndex()*PATHFIND_CELL_SIZE_F + PATHFIND_CELL_SIZE_F/2.0f;
		rawTo->y = closesetCell->getYIndex()*PATHFIND_CELL_SIZE_F + PATHFIND_CELL_SIZE_F/2.0f;
		// construct and return path
		Path *path = buildActualPath( obj, locomotorSet.getValidSurfaces(), from, closesetCell, centerInCell, blocked );
#if RETAIL_COMPATIBLE_PATHFINDING
		if (!s_useFixedPathfinding) {
			goalCell->releaseInfo();
			cleanOpenAndClosedLists();
		}
		else
#endif
		{
			cleanOpenAndClosedLists();
			parentCell->releaseInfo();
			goalCell->releaseInfo();
		}
		return path;
	}

	// failure - goal cannot be reached
#ifdef DEBUG_LOGGING
	Bool valid;
	valid = validMovementPosition( isCrusher, obj->getLayer(), locomotorSet, to ) ;

	DEBUG_LOG(("Pathfind(findClosestPath) failed from (%f,%f) to (%f,%f), original valid %d --", TheGameLogic->getFrame(), from->x, from->y, to->x, to->y, valid));
	DEBUG_LOG(("Unit '%s', time %f", obj->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));
#endif
#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI)
		debugShowSearch(false);
#endif
#ifdef DUMP_PERF_STATS
	TheGameLogic->incrementOverallFailedPathfinds();
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		goalCell->releaseInfo();
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}
	return nullptr;
}


void Pathfinder::adjustCoordToCell(Int cellX, Int cellY, Bool centerInCell, Coord3D &pos, PathfindLayerEnum layer)
{
	if (centerInCell) {
		pos.x = ((Real)cellX + 0.5f) * PATHFIND_CELL_SIZE_F;
		pos.y = ((Real)cellY + 0.5f) * PATHFIND_CELL_SIZE_F;
	} else {
		pos.x = ((Real)cellX+0.05) * PATHFIND_CELL_SIZE_F;
		pos.y = ((Real)cellY+0.05) * PATHFIND_CELL_SIZE_F;
	}
	pos.z = TheTerrainLogic->getLayerHeight( pos.x, pos.y, layer );
}


/**
 * Work backwards from goal cell to construct final path.
 */
Path *Pathfinder::buildActualPath( const Object *obj, LocomotorSurfaceTypeMask acceptableSurfaces, const Coord3D *fromPos,
																	PathfindCell *goalCell, Bool center, Bool blocked )
{
	DEBUG_ASSERTCRASH( goalCell, ("Pathfinder::buildActualPath: goalCell == nullptr") );

	Path *path = newInstance(Path);

	if (goalCell->getPinched() && goalCell->getParentCell() && !goalCell->getParentCell()->getPinched()) {
		goalCell = goalCell->getParentCell();
	}

	prependCells(path, fromPos, goalCell, center);

	// cleanup the path by checking line of sight
	path->optimize(obj, acceptableSurfaces, blocked);

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI==AI_DEBUG_PATHS)
	{
		extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
 		RGBColor color;
		color.blue = 0;
		color.red = color.green = 1;
		Coord3D pos;
		PathNode *node = path->getFirstNode();
		for( ; node; node = node->getNext() )
		{

			// create objects to show path - they decay

			pos = *node->getPosition();
			color.red = color.green = 1;
			if (node->getLayer() != LAYER_GROUND) {
				color.red = 0;
			}
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.25f, 200, color);
		}

		// show optimized path
		for( node = path->getFirstNode(); node; node = node->getNextOptimized() )
		{
			pos = *node->getPosition();
			addIcon(&pos, PATHFIND_CELL_SIZE_F*.8f, 200, color);
		}
		setDebugPath(path);
	}
#endif
	return path;
}

/**
 * Work backwards from goal cell to construct final path.
 */
void Pathfinder::prependCells( Path *path, const Coord3D *fromPos,
																	PathfindCell *goalCell, Bool center )
{
	// traverse path cells in REVERSE order, creating path in desired order
	// skip the LAST node, as that will be in the same cell as the unit itself - so use the unit's position
	Coord3D pos;
	PathfindCell *cell, *prevCell = nullptr;
	Bool goalCellNull = (goalCell->getParentCell()==nullptr);
	Int layerSteps = 0;	// GeneralsX: consecutive layer transitions at prevCell's x,y (the cycle guard below)
	for( cell = goalCell; cell->getParentCell(); cell = cell->getParentCell() )
	{
		m_zoneManager.setPassable(cell->getXIndex(), cell->getYIndex(), true);
		adjustCoordToCell(cell->getXIndex(), cell->getYIndex(), center, pos, cell->getLayer());
		if (prevCell && cell->getXIndex()==prevCell->getXIndex() && cell->getYIndex()==prevCell->getYIndex()) {
			// GeneralsX: A PARENT CYCLE ON ONE X,Y CAN NO LONGER HANG THE FRAME (guard).
			// This branch neither prepends a node, clears a parent nor moves prevCell, so a cycle whose cells all share
			// prevCell's x,y replayed for ever: the logic frame never ended and every peer froze (internal_findHierarchicalPath
			// built one, fixed there). A real chain visits a cell once, and an x,y holds one cell per layer (LAYER_LAST of
			// them), so coming back to prevCell, or LAYER_LAST consecutive transitions, can only be a cycle - a walk that
			// would never have returned. End it at prevCell, the last cell on the path, as if it were the start: a walk that ends
			// on its own is unchanged, every peer takes the same exit, and the anomaly is logged so a new cycle gets seen.
			if (cell == prevCell || ++layerSteps >= LAYER_LAST) {
				static UnsignedInt s_gxPathCycles = 0;
				if (++s_gxPathCycles <= 16 || (s_gxPathCycles & 1023) == 0) {
					fprintf(stderr, "[GXPATHCYCLE] f=%u n=%u goal=%d,%d,L%d at=%d,%d,L%d prev=L%d steps=%d - parent cycle ended at prevCell\n",
						TheGameLogic ? TheGameLogic->getFrame() : 0, s_gxPathCycles, goalCell->getXIndex(), goalCell->getYIndex(),
						(Int)goalCell->getLayer(), cell->getXIndex(), cell->getYIndex(), (Int)cell->getLayer(), (Int)prevCell->getLayer(),
						layerSteps);
					fflush(stderr);
				}
				cell = prevCell;
				break;
			}
			// transitioning layers.
			PathfindLayerEnum layer = cell->getLayer();
			if (layer==LAYER_GROUND) {
				layer = prevCell->getLayer();
			}
			DEBUG_ASSERTCRASH(layer!=LAYER_GROUND, ("Should have at 1 non-ground layer. jba"));
			path->getFirstNode()->setLayer(layer);
			continue;
		}
		layerSteps = 0;

		Bool canOptimize = true;
		if (cell->getType() == PathfindCell::CELL_CLIFF) {
			if (prevCell && prevCell->getType() != PathfindCell::CELL_CLIFF) {
				if (path->getFirstNode()) {
					path->getFirstNode()->setCanOptimize(false);
				}
			}
		}	else {
			if (prevCell && prevCell->getType() == PathfindCell::CELL_CLIFF) {
				canOptimize = false;
			}
		}

		path->prependNode( &pos, cell->getLayer() );
		path->getFirstNode()->setCanOptimize(canOptimize);
		if (cell->isBlockedByAlly()) {
			path->setBlockedByAlly(true);
		}
		if (prevCell) {
			prevCell->clearParentCell();
		}
		prevCell = cell;
	}

#if RETAIL_COMPATIBLE_PATHFINDING
	// TheSuperHackers @info This pathway is here for retail compatibility, it is to catch when a starting cell has a dangling parent that contains no pathing information
	// Beyond this point a retail client will crash due to a null pointer access within cell->getXIndex()
	// To recover from this we set the cell to the previous cell, which should be the actual starting cell then set to use the fixed pathing and perform a forced cleanup
	if (!s_useFixedPathfinding) {
		if (!cell->hasInfo()) {
			cell = prevCell;

			m_zoneManager.setPassable(cell->getXIndex(), cell->getYIndex(), true);
			if (goalCellNull) {
				// Very short path.
				adjustCoordToCell(cell->getXIndex(), cell->getYIndex(), center, pos, cell->getLayer());
				path->prependNode( &pos, cell->getLayer() );
			}
			// put actual start position as first node on the path, so it begins right at the unit's feet
			if (fromPos->x != path->getFirstNode()->getPosition()->x || fromPos->y != path->getFirstNode()->getPosition()->y) {
				path->prependNode( fromPos, cell->getLayer() );
			}

			s_useFixedPathfinding = true;
			forceCleanCells();
			return;
		}
	}
#endif

	m_zoneManager.setPassable(cell->getXIndex(), cell->getYIndex(), true);
	if (goalCellNull) {
		// Very short path.
		adjustCoordToCell(cell->getXIndex(), cell->getYIndex(), center, pos, cell->getLayer());
		path->prependNode( &pos, cell->getLayer() );
	}
	// put actual start position as first node on the path, so it begins right at the unit's feet
	if (fromPos->x != path->getFirstNode()->getPosition()->x || fromPos->y != path->getFirstNode()->getPosition()->y) {
		path->prependNode( fromPos, cell->getLayer() );
	}

}

void Pathfinder::setDebugPath(Path *newDebugpath)
{
	if (TheGlobalData->m_debugAI)
	{
		// copy the path for debugging
		deleteInstance(debugPath);
		debugPath = newInstance(Path);

		for( PathNode *copyNode = newDebugpath->getFirstNode(); copyNode; copyNode = copyNode->getNextOptimized() )
			debugPath->appendNode( copyNode->getPosition(), copyNode->getLayer() );
	}

}

/**
 * Given two world-space points, call callback for each cell.
 * Uses Bresenham line algorithm from www.gamedev.net.
 */
Int Pathfinder::iterateCellsAlongLine( const Coord3D& startWorld, const Coord3D& endWorld,
																			PathfindLayerEnum layer, CellAlongLineProc proc, void* userData )
{
	ICoord2D start, end;
	worldToCell( &startWorld, &start );
	worldToCell( &endWorld, &end );
	return iterateCellsAlongLine(start, end, layer, proc, userData);
}
/**
 * Given two world-space points, call callback for each cell.
 * Uses Bresenham line algorithm from www.gamedev.net.
 */
Int Pathfinder::iterateCellsAlongLine( const ICoord2D &start, const ICoord2D &end,
																			PathfindLayerEnum layer, CellAlongLineProc proc, void* userData )
{
	// GeneralsX: The Bresenham walk lives in PF27Kernel::forEachCellAlongLine so that this
	// function and the PF27 kernel's line walk are one implementation (a second copy would drift).
	return PF27Kernel::forEachCellAlongLine(this, start, end, layer,
		[this, proc, userData](PathfindCell *from, PathfindCell *to, Int x, Int y) { return (*proc)(this, from, to, x, y, userData); });
}

//-----------------------------------------------------------------------------

static ObjectID getSlaverID(const Object* o)
{
	for (BehaviorModule** update = o->getBehaviorModules(); *update; ++update)
	{
		SlavedUpdateInterface* sdu = (*update)->getSlavedUpdateInterface();
		if (sdu != nullptr)
		{
			return sdu->getSlaverID();
		}
	}

	return INVALID_ID;
}

static ObjectID getContainerID(const Object* o)
{
	const Object* container = o ? o->getContainedBy() : nullptr;
	return container ? container->getID() : INVALID_ID;
}

struct segmentIntersectsStruct
{
	Object *theTallBuilding;
	ObjectID ignoreBuilding;
};

/*static*/ Int Pathfinder::segmentIntersectsBuildingCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	segmentIntersectsStruct* d = (segmentIntersectsStruct*)userData;

	if (to != nullptr && (to->getType() == PathfindCell::CELL_OBSTACLE))
	{
		Object *obj = TheGameLogic->findObjectByID(to->getObstacleID());
		if (obj && obj->isKindOf(KINDOF_AIRCRAFT_PATH_AROUND)) {
			if (obj->getID() == d->ignoreBuilding) {
				return 0;
			}
			d->theTallBuilding = obj;
			return 1;
		}
	}

	return 0;	// keep going
}



struct ViewBlockedStruct
{
	const Object *obj;
	const Object *objOther;
};


/*static*/ Int Pathfinder::lineBlockedByObstacleCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	const ViewBlockedStruct* d = (const ViewBlockedStruct*)userData;

	if (to != nullptr && (to->getType() == PathfindCell::CELL_OBSTACLE))
	{

		// we never block our own view!
		if (to->isObstaclePresent(d->obj->getID()))
			return 0;

		// nor does the object we're trying to see!
		if (to->isObstaclePresent(d->objOther->getID()))
			return 0;

		// if the obstacle is our container, ignore it as an obstacle.
		if (to->isObstaclePresent(getContainerID(d->obj)))
			return 0;

		// @todo: if the obstacle is objOther's container, AND it's a "visible" container, ignore it.

		// if the obstacle is the item to which we are slaved, ignore it as an obstacle.
		if (to->isObstaclePresent(getSlaverID(d->obj)))
			return 0;

		// if the obstacle is the item to which objOther is slaved, ignore it as an obstacle.
		if (to->isObstaclePresent(getSlaverID(d->objOther)))
			return 0;

		// if the obstacle is transparent, ignore it, since this callback is only used for line-of-sight. (srj)
		if (to->isObstacleTransparent())
			return 0;

		return 1;	// bail early
	}

	return 0;	// keep going
}

struct ViewAttackBlockedStruct
{
	const Object *obj;
	const Object *victim;
	const PathfindCell *victimCell;
	Int		skipCount;
};

/*static*/ Int Pathfinder::attackBlockedByObstacleCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	ViewAttackBlockedStruct* d = (ViewAttackBlockedStruct*)userData;

	if (d->skipCount>0) {
		d->skipCount--;
		return 0;
	}
	if (to != nullptr && (to->getType() == PathfindCell::CELL_OBSTACLE))
	{
		// we never block our own view!
		if (to->isObstaclePresent(d->obj->getID()))
			return 0;

		if (d->victim) {
			// nor does the object we're trying to attack!
			if (to->isObstaclePresent(d->victim->getID()))
				return 0;
			// if the obstacle is the item to which objOther is slaved, ignore it as an obstacle.
			if (to->isObstaclePresent(getSlaverID(d->victim)))
				return 0;
		}

		// if the obstacle is our container, ignore it as an obstacle.
		if (to->isObstaclePresent(getContainerID(d->obj)))
			return 0;

		// @todo: if the obstacle is objOther's container, AND it's a "visible" container, ignore it.

		// if the obstacle is the item to which we are slaved, ignore it as an obstacle.
		if (to->isObstaclePresent(getSlaverID(d->obj)))
			return 0;

		if (to->isObstacleTransparent())
			return 0;
		//Kris: Added the check for victimCell because in China01 -- after the intro, NW of your
		//base is a cream colored building that lies in a negative coord. When you order units to
		//force attack it, it crashes.
		if( d->victimCell && to->isObstaclePresent( d->victimCell->getObstacleID() ) )
		{
			// Victim is inside the bounds of another object.  We don't let this block us,
			// as usually it is on the edge and it looks like we should be able to shoot it. jba.
			return 0;
		}
		return 1;	// bail early
	}

	return 0;	// keep going
}

//-----------------------------------------------------------------------------
Bool Pathfinder::isViewBlockedByObstacle(const Object* obj, const Object* objOther)
{
	ViewBlockedStruct info;
	info.obj = obj;
	info.objOther = objOther;
	if (objOther && objOther->isSignificantlyAboveTerrain()) {
		return false; // We don't check los to flying objects.  jba.
	}
#if 1
	return isAttackViewBlockedByObstacle(obj, *obj->getPosition(), objOther, *objOther->getPosition());
#else
	PathfindLayerEnum layer = objOther->getLayer();
	if (layer==LAYER_GROUND) {
		layer = obj->getLayer();
	}
	Int ret = iterateCellsAlongLine(*obj->getPosition(), *objOther->getPosition(),
		layer, lineBlockedByObstacleCallback, &info);
	return ret != 0;
#endif
}


//-----------------------------------------------------------------------------
Bool Pathfinder::isAttackViewBlockedByObstacle(const Object* attacker, const Coord3D& attackerPos, const Object* victim, const Coord3D& victimPos)
{
	//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() - attackerPos is (%g,%g,%g) (%X,%X,%X)",
	//	attackerPos.x, attackerPos.y, attackerPos.z,
	//	AS_INT(attackerPos.x),AS_INT(attackerPos.y),AS_INT(attackerPos.z)));
	//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() - victimPos is (%g,%g,%g) (%X,%X,%X)",
	//	victimPos.x, victimPos.y, victimPos.z,
	//	AS_INT(victimPos.x),AS_INT(victimPos.y),AS_INT(victimPos.z)));
	// Global switch to turn this off in case it doesn't work.
	if (!TheAI->getAiData()->m_attackUsesLineOfSight)
	{
		//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() 1"));
		return false;
	}

	// If the attacker doesn't need line of sight, isn't blocked.
	if (!attacker->isKindOf(KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT))
	{
		//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() 2"));
		return false;
	}

// srj sez: this is a good start at taking terrain into account for attacks, but findAttackPath needs to be smartened also
#define LOS_TERRAIN
#ifdef LOS_TERRAIN
	const Weapon* w = attacker->getCurrentWeapon();
	if (attacker->isKindOf(KINDOF_IMMOBILE)) {
		// Don't take terrain blockage into account, since we can't move around it. jba.
		w = nullptr;
	}
	if (w)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @bugfix Stubbjax 23/08/2026 Don't consider the attack blocked if it is a contact weapon.
		// This allows weapons such as suicide bombs to be triggered if the unit is blocked by building geometry.
		if (w->isContactWeapon())
			return false;
#endif

		Bool viewBlocked;
		if (victim)
			viewBlocked = !w->isClearGoalFiringLineOfSightTerrain(attacker, attackerPos, victim);
		else
			viewBlocked = !w->isClearGoalFiringLineOfSightTerrain(attacker, attackerPos, victimPos);

		if (viewBlocked)
		{
			//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() 3"));
			return true;
		}
	}
#endif

	ViewAttackBlockedStruct info;
	info.obj = attacker;
	info.victim = victim;
	PathfindLayerEnum layer = LAYER_GROUND;
	if (victim) {
		layer = victim->getLayer();
	}
	info.victimCell = getCell(layer, &victimPos);

	info.skipCount = 0;
	if (attacker->getLayer() != LAYER_GROUND)
	{
		info.skipCount = 3;	/// srj -- someone wanna tell me what this magic number means?
												/// jba - Yes, it means that if someone is on a bridge, or rooftop, they can see
												///      3 pathfind cells out of whatever they are standing on.
												/// srj -- awesome! thank you very much :-)
		if (layer==LAYER_GROUND) {
			layer = attacker->getLayer();
		}
	}

	Int ret = iterateCellsAlongLine(attackerPos, victimPos, layer, attackBlockedByObstacleCallback, &info);
	//CRCDEBUG_LOG(("Pathfinder::isAttackViewBlockedByObstacle() 4"));
	return ret != 0;
}

static void computeNormalRadialOffset(const Coord3D& from,	Coord3D& insert, const Coord3D& to,
																			Object *obj, Real radius)
{
	Real crossProduct;
	Real dx = to.x - from.x;
	Real dy = to.y -from.y;
	Coord3D objPos = *obj->getPosition();


	Real objDx = objPos.x - from.x;
	Real objDy = objPos.y - from.y;

	crossProduct = dx*objDy - dy*objDx;

	Coord3D fromToNormal;
	fromToNormal.z = 0;
	if (crossProduct>0) {
		fromToNormal.x = dy;
		fromToNormal.y = -dx;
	}	else {
		fromToNormal.x = -dy;
		fromToNormal.y = dx;
	}
	fromToNormal.normalize();
	Real length = radius;
	insert = *obj->getPosition();
	insert.x += fromToNormal.x*length;
	insert.y += fromToNormal.y*length;

}

//-----------------------------------------------------------------------------
Bool Pathfinder::segmentIntersectsTallBuilding(const PathNode *curNode,
										PathNode *nextNode,  ObjectID ignoreBuilding, Coord3D *insertPos1,  Coord3D *insertPos2,  Coord3D *insertPos3 )
{
	segmentIntersectsStruct info;
	info.theTallBuilding = nullptr;
	info.ignoreBuilding = ignoreBuilding;

	Coord3D fromPos = *curNode->getPosition();
	Coord3D toPos = *nextNode->getPosition();

	Int i;
	for (i=0; i<2; i++) {
		Int ret = iterateCellsAlongLine(fromPos, toPos, LAYER_GROUND, segmentIntersectsBuildingCallback, &info);
		if (ret!=0 && info.theTallBuilding) {
			// see if toPos is inside the radius of the tall building.
			Coord3D bldgPos = *info.theTallBuilding->getPosition();
			Coord2D delta;
			Real radius = info.theTallBuilding->getGeometryInfo().getBoundingCircleRadius() + 2*PATHFIND_CELL_SIZE_F;
			delta.x = toPos.x - bldgPos.x;
			delta.y = toPos.y - bldgPos.y;
			if (delta.length() <= radius*0.98) {
				if (delta.length() < 0.1) {
					delta.x = 1;
				}
				delta.normalize();
				delta.x *= radius;
				delta.y *= radius;
				toPos.x = bldgPos.x+delta.x;
				toPos.y = bldgPos.y+delta.y;
				nextNode->setPosition(&toPos);
				continue;
			}
			delta.x = fromPos.x - bldgPos.x;
			delta.y = fromPos.y - bldgPos.y;
			if (delta.length() <= radius*0.98) {
				if (delta.length() < 0.1) {
					delta.x = 1;
				}
				delta.normalize();
				delta.x *= radius;
				delta.y *= radius;
				fromPos.x = bldgPos.x+delta.x;
				fromPos.y = bldgPos.y+delta.y;
			}


			computeNormalRadialOffset(fromPos, *insertPos2, toPos, info.theTallBuilding, radius);
			computeNormalRadialOffset(fromPos, *insertPos1, *insertPos2, info.theTallBuilding, radius);
			computeNormalRadialOffset(*insertPos2, *insertPos3, toPos, info.theTallBuilding, radius);

			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
Bool Pathfinder::circleClipsTallBuilding(	const Coord3D *from, const Coord3D *to, Real circleRadius, ObjectID ignoreBuilding, Coord3D *adjustTo)
{
	PartitionFilterAcceptByKindOf filterKindof(MAKE_KINDOF_MASK(KINDOF_AIRCRAFT_PATH_AROUND), KINDOFMASK_NONE);
	PartitionFilter *filters[] = { &filterKindof, nullptr };
	Object* tallBuilding = ThePartitionManager->getClosestObject(to, circleRadius, FROM_BOUNDINGSPHERE_2D, filters);
	if (tallBuilding) {
		Real radius = tallBuilding->getGeometryInfo().getBoundingCircleRadius() + 2*PATHFIND_CELL_SIZE_F;
		computeNormalRadialOffset(*from, *adjustTo, *to, tallBuilding, circleRadius+radius);
		Object* otherTallBuilding = ThePartitionManager->getClosestObject(adjustTo, circleRadius, FROM_BOUNDINGSPHERE_2D, filters);
		if (otherTallBuilding && otherTallBuilding!=tallBuilding) {
			radius = otherTallBuilding->getGeometryInfo().getBoundingCircleRadius() + 2*PATHFIND_CELL_SIZE_F;
			Coord3D tmpTo = *adjustTo;
			computeNormalRadialOffset(*from, *adjustTo, tmpTo, otherTallBuilding, circleRadius+radius);
		}
		return true;
	}
	return false;
}

//-----------------------------------------------------------------------------

struct LinePassableStruct
{
	const Object *obj;
	LocomotorSurfaceTypeMask acceptableSurfaces;
	Int radius;
	Bool centerInCell;
	Bool blocked;
	Bool allowPinched;
	// GeneralsX: Same hoist as ExamineCellsStruct::isCrusher: the
	// crusher level cannot change inside one line walk, so it is read once, not once per cell.
	Bool isCrusher;
	// GeneralsX: The last cell whose footprint block passed the
	// movement, ally, enemy and pinch checks in this walk, so every cell of that block is known to
	// be clear (see linePassableCallback). A later bail ends the walk, so it is never read again.
	Bool havePrev;
	Int prevX;
	Int prevY;
	PathfindLayerEnum prevLayer;
};

/*static*/ Int Pathfinder::linePassableCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	LinePassableStruct* d = (LinePassableStruct*)userData;

	const Bool isCrusher = d->isCrusher;
	TCheckMovementInfo info;
	info.cell.x = to_x;
	info.cell.y = to_y;
	info.layer = to->getLayer();
	info.centerInCell = d->centerInCell;
	info.radius = d->radius;
	info.considerTransient = d->blocked;
	info.acceptableSurfaces = d->acceptableSurfaces;

	// GeneralsX: Scan only the cells the step ADDED to the footprint.
	//
	// This callback reads just three things from checkForMovement: its result, allyFixedCount != 0
	// and enemyFixed, and it bails if any of them says no. Each of those is an OR over the cells of
	// the footprint block of a per-cell condition (off the map; an ally that is checked; an enemy
	// that cannot be crushed) that depends on the cell, obj and the fixed walk parameters - never on
	// which cell is the block's centre. So "the block passes" == "no cell of the block is bad", and
	// checkForMovement writes nothing. The previous accepted cell's block passed, and the world does
	// not change during one walk. iterateCellsAlongLine steps one cell along ONE axis between
	// callbacks, so the new block differs from the previous one by a single row or column: that
	// strip alone decides. Any other step, a layer change or the first cell scans the full block.
	// allyMoving/allyGoal are not read here, and the ally dedupe only affects the count, not != 0.
	Int numCellsAbove = info.radius;
	if (info.centerInCell) numCellsAbove++;
	Int x0 = to_x - info.radius, x1 = to_x + numCellsAbove;
	Int y0 = to_y - info.radius, y1 = to_y + numCellsAbove;
	if (d->havePrev && d->prevLayer == info.layer && x1 - x0 > 1)	// a 1x1 block IS its strip
	{
		const Int dx = to_x - d->prevX;
		const Int dy = to_y - d->prevY;
		if (dy == 0 && dx == 1)       { x0 = x1 - 1; }
		else if (dy == 0 && dx == -1) { x1 = x0 + 1; }
		else if (dx == 0 && dy == 1)  { y0 = y1 - 1; }
		else if (dx == 0 && dy == -1) { y1 = y0 + 1; }
	}
	if (!pathfinder->checkForMovementInRect(d->obj, info, x0, x1, y0, y1, false))	// allyMoving is not read here
	{
		return 1;	// bail out
	}

	if (info.allyFixedCount || info.enemyFixed)
	{
		return 1;	// bail out
	}

	if (!d->allowPinched && to->getPinched()) {
		return 1; // bail out.
	}

	d->havePrev = true;
	d->prevX = to_x;
	d->prevY = to_y;
	d->prevLayer = info.layer;

	if (from && to->getLayer() != LAYER_GROUND && from->getLayer() == to->getLayer()) {
		if (to->getType() == PathfindCell::CELL_CLEAR) {
			return 0;
		}
	}

	if (pathfinder->validMovementPosition( isCrusher, d->acceptableSurfaces, to, from ) == false)
	{
		return 1;	// bail out
	}

	return 0;	// keep going
}

//-----------------------------------------------------------------------------

struct GroundPathPassableStruct
{
	Int		diameter;
	Bool	crusher;
};

/*static*/ Int Pathfinder::groundPathPassableCallback(Pathfinder* pathfinder, PathfindCell* from, PathfindCell* to, Int to_x, Int to_y, void* userData)
{
	const GroundPathPassableStruct* d = (const GroundPathPassableStruct*)userData;

	Int curDiameter = pathfinder->clearCellForDiameter(d->crusher, to_x, to_y, to->getLayer(), d->diameter);
	if (curDiameter==d->diameter) return 0;	//  good to go.
	if (from && to->getLayer() != LAYER_GROUND && from->getLayer() == to->getLayer()) {
		return 0;
	}

	return 1;	// failed.
}

//-----------------------------------------------------------------------------

/**
 * Given two world-space points, check the line of sight between them for any impassible cells.
 * Uses Bresenham line algorithm from www.gamedev.net.
 */
Bool Pathfinder::isLinePassable( const Object *obj, LocomotorSurfaceTypeMask acceptableSurfaces,
																PathfindLayerEnum layer, const Coord3D& startWorld,
																const Coord3D& endWorld, Bool blocked,
																Bool allowPinched)
{
	LinePassableStruct info;
	//CRCDEBUG_LOG(("Pathfinder::isLinePassable(): %d %d %d ", m_ignoreObstacleID, m_isMapReady, m_isTunneling));

	info.obj = obj;
	info.acceptableSurfaces = acceptableSurfaces;
	getRadiusAndCenter(obj, info.radius, info.centerInCell);
	info.blocked = blocked;
	info.allowPinched = allowPinched;
	info.isCrusher = obj ? obj->getCrusherLevel() > 0 : false;	// GeneralsX: same expression the callback used to recompute per cell
	info.havePrev = false;
	info.prevX = 0;
	info.prevY = 0;
	info.prevLayer = LAYER_GROUND;

	Int ret = iterateCellsAlongLine(startWorld, endWorld, layer, linePassableCallback, (void*)&info);
	return ret == 0;
}

//-----------------------------------------------------------------------------

/**
 * Given two world-space points, check the line of sight between them for any impassible cells.
 * Uses Bresenham line algorithm from www.gamedev.net.
 */
Bool Pathfinder::isGroundPathPassable( Bool isCrusher, const Coord3D& startWorld, PathfindLayerEnum startLayer,
		const Coord3D& endWorld, Int pathDiameter)
{
	GroundPathPassableStruct info;

	info.diameter = pathDiameter;
	info.crusher = isCrusher;

	Int ret = iterateCellsAlongLine(startWorld, endWorld, startLayer, groundPathPassableCallback, (void*)&info);
	return ret == 0;
}

/**
 * Classify the cells under the bridge
 * If 'repaired' is true, bridge is repaired
 * If 'repaired' is false, bridge has been damaged to be impassable
 */
void Pathfinder::changeBridgeState( PathfindLayerEnum layer, Bool repaired)
{
	if (m_layers[layer].isUnused()) return;
	if (m_layers[layer].setDestroyed(!repaired)) {
		m_zoneManager.markZonesDirty();
	}
}

void Pathfinder::getRadiusAndCenter(const Object *obj, Int &iRadius, Bool &center)
{
	enum {MAX_RADIUS = 2};
	if (!obj)
	{
		center = true;
		iRadius = 0;
		return;
	}
	Real diameter = 2*obj->getGeometryInfo().getBoundingCircleRadius();
	if (diameter>PATHFIND_CELL_SIZE_F && diameter<2.0f*PATHFIND_CELL_SIZE_F) {
		diameter = 2.0f*PATHFIND_CELL_SIZE_F;
	}
	iRadius = REAL_TO_INT_FLOOR(diameter/PATHFIND_CELL_SIZE_F+0.3f);
	center = false;
	if (iRadius==0) iRadius++;
	if (iRadius&1)
	{
		center = true;
	}
	iRadius /= 2;
	if (iRadius > MAX_RADIUS)
	{
		iRadius = MAX_RADIUS;
		center = true;
	}
}

/**
 * Updates the goal cell for an ai unit.
 */
void Pathfinder::updateGoal( Object *obj, const Coord3D *newGoalPos, PathfindLayerEnum layer)
{
	if (obj->isKindOf(KINDOF_IMMOBILE)) {
		// Only consider mobile.
		return;
	}

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai) return; // only consider ai objects.
	if (!ai->isDoingGroundMovement()) {
#if RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING
		Bool isUnmannedHelicopter = false;
#else
		// exception:sniped choppers are on ground
		Bool isUnmannedHelicopter = ( obj->isKindOf( KINDOF_PRODUCED_AT_HELIPAD ) && obj->isDisabledByType( DISABLED_UNMANNED  ) ) ;
#endif
		if (!isUnmannedHelicopter) {
			updateAircraftGoal(obj, newGoalPos);
			return;
		}
	}

	PathfindLayerEnum originalLayer = obj->getDestinationLayer();

	Bool layerChanged = originalLayer != layer;

	Bool doGround=false;
	Bool doLayer=false;
	if (layer==LAYER_GROUND) {
		doGround = true;
	} else {
		doLayer = true;
		if (TheTerrainLogic->objectInteractsWithBridgeEnd(obj, layer)) {
			doGround = true;
		}
	}

	ICoord2D goalCell = *ai->getPathfindGoalCell();

	Bool centerInCell;
	Int radius;
	ICoord2D newCell;
	getRadiusAndCenter(obj, radius, centerInCell);
	Int numCellsAbove = radius;
	if (centerInCell) numCellsAbove++;
	if (centerInCell) {
		newCell.x = REAL_TO_INT_FLOOR(newGoalPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(newGoalPos->y/PATHFIND_CELL_SIZE_F);
	} else {
		newCell.x = REAL_TO_INT_FLOOR(0.5f+newGoalPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(0.5f+newGoalPos->y/PATHFIND_CELL_SIZE_F);
	}
	if (!layerChanged && newCell.x==goalCell.x && newCell.y == goalCell.y) {
		return;
	}
	removeGoal(obj);

	obj->setDestinationLayer(layer);
	ai->setPathfindGoalCell(newCell);
	Int i,j;
	ICoord2D cellNdx;

	Bool warn = true;
	for (i=newCell.x-radius; i<newCell.x+numCellsAbove; i++) {
		for (j=newCell.y-radius; j<newCell.y+numCellsAbove; j++) {
			PathfindCell	*cell;
			if (doLayer) {
				cell = getCell(layer, i, j);
				if (cell) {
					if (warn && cell->getGoalUnit()!=INVALID_ID && cell->getGoalUnit() != obj->getID()) {
						warn = false;
						//Units got stuck close to each other.  jba
					}
					cellNdx.x = i;
					cellNdx.y = j;
					cell->setGoalUnit(obj->getID(), cellNdx);
				}
			}
			if (doGround) {
				cell = getCell(LAYER_GROUND, i, j);
				if (cell) {
					if (warn && cell->getGoalUnit()!=INVALID_ID && cell->getGoalUnit() != obj->getID()) {
						warn = false;
						//Units got stuck close to each other.  jba
					}
					cellNdx.x = i;
					cellNdx.y = j;
					cell->setGoalUnit(obj->getID(), cellNdx);
				}
			}
		}
	}

}

/**
 * Updates the goal cell for an ai unit.
 */
void Pathfinder::updateAircraftGoal( Object *obj, const Coord3D *newGoalPos)
{
	if (obj->isKindOf(KINDOF_IMMOBILE)) {
		// Only consider mobile.
		return;
	}
	removeGoal(obj);
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai) return; // only consider ai objects.
	if (ai->isDoingGroundMovement()) {
		return;  // shouldn't really happen, but just in case.
	}

	// For now, we are only doing HOVER, and WINGS.
	if (!ai->isAircraftThatAdjustsDestination()) return;

	ICoord2D goalCell = *ai->getPathfindGoalCell();

	Bool centerInCell;
	Int radius;
	ICoord2D newCell;
	getRadiusAndCenter(obj, radius, centerInCell);
	Int numCellsAbove = radius;
	if (centerInCell) numCellsAbove++;
	if (centerInCell) {
		newCell.x = REAL_TO_INT_FLOOR(newGoalPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(newGoalPos->y/PATHFIND_CELL_SIZE_F);
	} else {
		newCell.x = REAL_TO_INT_FLOOR(0.5f+newGoalPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(0.5f+newGoalPos->y/PATHFIND_CELL_SIZE_F);
	}
	if (newCell.x==goalCell.x && newCell.y == goalCell.y) {
		return;
	}

	ai->setPathfindGoalCell(newCell);
	Int i,j;
	ICoord2D cellNdx;

	for (i=newCell.x-radius; i<newCell.x+numCellsAbove; i++) {
		for (j=newCell.y-radius; j<newCell.y+numCellsAbove; j++) {
			PathfindCell	*cell;
			cell = getCell(LAYER_GROUND, i, j);
			if (cell) {
				cellNdx.x = i;
				cellNdx.y = j;
				cell->setGoalAircraft(obj->getID(), cellNdx);
			}
		}
	}

}

/**
 * Removes the goal cell for an ai unit.
 * Used for a unit that is going to be moving several times, like following a waypoint path,
 * or intentionally collides with other units (like a car bomb). jba
 */
void Pathfinder::removeGoal( Object *obj)
{
	if (obj->isKindOf(KINDOF_IMMOBILE)) {
		// Only consider mobile.
		return;
	}
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai) return; // only consider ai objects.
	ICoord2D goalCell = *ai->getPathfindGoalCell();

	Bool centerInCell;
	Int radius;
	ICoord2D newCell;
	getRadiusAndCenter(obj, radius, centerInCell);
	if (radius==0) {
		radius++;
	}
	Int numCellsAbove = radius;
	if (centerInCell) numCellsAbove++;
	newCell.x = newCell.y = -1;
	if (newCell.x==goalCell.x && newCell.y == goalCell.y) {
		return;
	}
	ICoord2D cellNdx;
	ai->setPathfindGoalCell(newCell);
	Int i,j;
	if (goalCell.x>=0 && goalCell.y>=0) {
		for (i=goalCell.x-radius; i<goalCell.x+numCellsAbove; i++) {
			for (j=goalCell.y-radius; j<goalCell.y+numCellsAbove; j++) {
				PathfindCell	*cell = getCell(LAYER_GROUND, i, j);
				if (cell) {
					if (cell->getGoalUnit()==obj->getID()) {
						cellNdx.x = i;
						cellNdx.y = j;
						cell->setGoalUnit(INVALID_ID, cellNdx);
					}
					if (cell->getGoalAircraft()==obj->getID()) {
						cellNdx.x = i;
						cellNdx.y = j;
						cell->setGoalAircraft(INVALID_ID, cellNdx);
					}
				}
				if (obj->getDestinationLayer()!=LAYER_GROUND) {
					cell = getCell( obj->getDestinationLayer(), i, j);
					if (cell) {
						if (cell->getGoalUnit()==obj->getID()) {
							cellNdx.x = i;
							cellNdx.y = j;
							cell->setGoalUnit(INVALID_ID, cellNdx);
						}
					}
				}
			}
		}
	}
}

/**
 * Updates the position cell for an ai unit.
 */
void Pathfinder::updatePos( Object *obj, const Coord3D *newPos)
{
	if (obj->isKindOf(KINDOF_IMMOBILE))
	{
		// Only consider mobile.
		return;
	}
	if (!m_isMapReady)
		return;

	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai)
		return; // only consider ai objects.

	ICoord2D curCell = *ai->getCurPathfindCell();
	if (!ai->isDoingGroundMovement())
	{
		if (curCell.x>=0 && curCell.y>=0)
		{
			removePos(obj);
		}
		return;
	}

	Bool centerInCell;
	Int radius;
	ICoord2D newCell;
	getRadiusAndCenter(obj, radius, centerInCell);
	Int numCellsAbove = radius;
	if (centerInCell)
		numCellsAbove++;
	if (centerInCell)
	{
		newCell.x = REAL_TO_INT_FLOOR(newPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(newPos->y/PATHFIND_CELL_SIZE_F);
	}
	else
	{
		newCell.x = REAL_TO_INT_FLOOR(0.5f+newPos->x/PATHFIND_CELL_SIZE_F);
		newCell.y = REAL_TO_INT_FLOOR(0.5f+newPos->y/PATHFIND_CELL_SIZE_F);
	}
	if (newCell.x==curCell.x && newCell.y == curCell.y)
	{
		return;
	}

	PathfindLayerEnum layer = obj->getLayer();
	Bool doGround=false;
	Bool doLayer=false;
	if (layer==LAYER_GROUND) {
		doGround = true;	// just have to do ground
	} else {
		doLayer = true; // have to do the layer
		if (TheTerrainLogic->objectInteractsWithBridgeEnd(obj, layer)) {
			doGround = true; // In this case, have to both layer & ground, as they overlap here.
		}
	}

	ai->setCurPathfindCell(newCell);
	Int i,j;
	ICoord2D cellNdx;
	//DEBUG_LOG(("Updating unit pos at cell %d, %d", newCell.x, newCell.y));
	if (curCell.x>=0 && curCell.y>=0) {
		for (i=curCell.x-radius; i<curCell.x+numCellsAbove; i++) {
			for (j=curCell.y-radius; j<curCell.y+numCellsAbove; j++) {
				cellNdx.x = i;
				cellNdx.y = j;
				PathfindCell	*cell = getCell(layer, i, j);
				if (cell) {
					if (cell->getPosUnit()==obj->getID()) {
						cell->setPosUnit(INVALID_ID, cellNdx);
					}
				}
				if (layer!=LAYER_GROUND) {
					// Remove from the ground, if present.
					cell = getCell(LAYER_GROUND, i, j);
					if (cell) {
						if (cell->getPosUnit()==obj->getID()) {
							cell->setPosUnit(INVALID_ID, cellNdx);
						}
					}
				}
			}
		}
	}
	for (i=newCell.x-radius; i<newCell.x+numCellsAbove; i++) {
		for (j=newCell.y-radius; j<newCell.y+numCellsAbove; j++) {
			PathfindCell	*cell;
			cellNdx.x = i;
			cellNdx.y = j;
			if (doLayer) {
				cell = getCell(layer, i, j);
				if (cell) {
					cell->setPosUnit(obj->getID(), cellNdx);
				}
			}
			if (doGround) {
				cell = getCell(LAYER_GROUND, i, j);
				if (cell) {
					cell->setPosUnit(obj->getID(), cellNdx);
				}
			}
		}
	}
}

/**
 * Removes the position cell flags for an ai unit.
 */
void Pathfinder::removePos( Object *obj)
{
	if (obj->isKindOf(KINDOF_IMMOBILE)) {
		// Only consider mobile.
		return;
	}
	if (!m_isMapReady) return;
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	if (!ai) return; // only consider ai objects.
	ICoord2D curCell = *ai->getCurPathfindCell();
	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);
	Int numCellsAbove = radius;
	if (centerInCell) numCellsAbove++;
	PathfindLayerEnum layer = obj->getLayer();

	ICoord2D newCell;
	newCell.x = newCell.y = -1;
	ai->setCurPathfindCell(newCell);

	Int i,j;
	ICoord2D cellNdx;
	//DEBUG_LOG(("Updating unit pos at cell %d, %d", newCell.x, newCell.y));
	if (curCell.x>=0 && curCell.y>=0) {
		for (i=curCell.x-radius; i<curCell.x+numCellsAbove; i++) {
			for (j=curCell.y-radius; j<curCell.y+numCellsAbove; j++) {
				cellNdx.x = i;
				cellNdx.y = j;
				PathfindCell	*cell = getCell(layer, i, j);
				if (cell) {
					if (cell->getPosUnit()==obj->getID()) {
						cell->setPosUnit(INVALID_ID, cellNdx);
					}
				}
				if (layer!=LAYER_GROUND) {
					// Remove from the ground, if present.
					cell = getCell(LAYER_GROUND, i, j);
					if (cell) {
						if (cell->getPosUnit()==obj->getID()) {
							cell->setPosUnit(INVALID_ID, cellNdx);
						}
					}
				}
			}
		}
	}
}

/**
 * Removes a mobile unit from the pathfind grid.
 */
void Pathfinder::removeUnitFromPathfindMap(  Object *obj )
{
	removePos(obj);
	removeGoal(obj);
}

// GeneralsX: Hand port of upstream PR #3332 (draft): the per-cell body of moveAllies,
// split out so both walks below share it.
// Upstream reference: bobtista, PR #3332 https://github.com/TheSuperHackers/GeneralsGameCode/pull/3332
void Pathfinder::moveAlliesAroundCell(Object *obj, const ICoord2D &curCell, PathfindLayerEnum layer, Int radius, Int numCellsAbove, ObjectID ignoreId, Bool blockedByAlly)
{
	Int i, j;
	for (i=curCell.x-radius; i<curCell.x+numCellsAbove; i++) {
		for (j=curCell.y-radius; j<curCell.y+numCellsAbove; j++) {
			PathfindCell	*cell = getCell(layer, i, j);
			if (!cell) {
				continue; // Cell is not on the pathfinding grid
			}

			ObjectID unitId = cell->getPosUnit();
			if (unitId==INVALID_ID) {
				continue;
			}

			if (unitId==obj->getID()) {
				continue;	// It's us.
			}

			if (unitId==ignoreId) {
				continue;	 // It's the one we are ignoring.
			}

			Object *otherObj = TheGameLogic->findObjectByID(unitId);
			if (!otherObj) {
				continue;
			}

			if (obj->getRelationship(otherObj)!=ALLIES) {
				continue;  // Only move allies.
			}

			if (obj->isKindOf(KINDOF_INFANTRY) && otherObj->isKindOf(KINDOF_INFANTRY)) {
				continue;  // infantry can walk through other infantry, so just let them.
			}
			if (obj->isKindOf(KINDOF_INFANTRY) && !otherObj->isKindOf(KINDOF_INFANTRY)) {
				// If this is a general clear operation, don't let infantry push vehicles.
				if (!blockedByAlly) {
					continue;
				}
			}

			if (!otherObj->getAI() || otherObj->getAI()->isMoving()) {
				continue;
			}

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
			if (otherObj->getAI()->isAttacking()) {
				continue; // Don't move units that are attacking. [8/14/2003]
			}

			//Kris: Patch 1.01 November 3, 2003
			//Black Lotus exploit fix -- moving while hacking.
			if( otherObj->testStatus( OBJECT_STATUS_IS_USING_ABILITY ) || otherObj->getAI()->isBusy() ) {
				continue; // Packing or unpacking objects for example
			}
#endif

			//DEBUG_LOG(("Moving ally"));
			otherObj->getAI()->aiMoveAwayFromUnit(obj, CMD_FROM_AI);
		}
	}
}

Bool Pathfinder::moveAllies(Object *obj, Path *path)
{

#ifdef DO_UNIT_TIMINGS
#pragma MESSAGE("*** WARNING *** DOING DO_UNIT_TIMINGS!!!!")
extern Bool g_UT_startTiming;
if (g_UT_startTiming) return false;
#endif
	if (!obj->isKindOf(KINDOF_DOZER) && !obj->isKindOf(KINDOF_HARVESTER)) {
		// Harvesters & dozers want a clear path.
		if (!path->getBlockedByAlly()) {
			return FALSE; // Only move units if it is required.
		}
	}
	LatchRestore<Int> recursiveDepth(m_moveAlliesDepth, m_moveAlliesDepth+1);
	if (m_moveAlliesDepth > 2) {
		return false;
	}

	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);
	Int numCellsAbove = radius;
	if (centerInCell) numCellsAbove++;
	ObjectID ignoreId = INVALID_ID;
	if (obj->getAIUpdateInterface()) {
		ignoreId = obj->getAIUpdateInterface()->getIgnoredObstacleID();
	}

	// GeneralsX: Stop walking a path that a nested move-away destroyed (use-after-free,
	// upstream issue #3323). Read the flag once: it is only written while the path is built (setBlockedByAlly).
	// Upstream reference: bobtista, PR #3332 https://github.com/TheSuperHackers/GeneralsGameCode/pull/3332
	const Bool blockedByAlly = path->getBlockedByAlly();

#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding)
	{
		// TheSuperHackers @info The move away orders can recurse back into this unit and destroy the path while it is
		// still being walked here. Retail continues on the freed nodes and their contents decide which allies are
		// ordered to move, and existing retail replays depend on that. Detect the destroyed path after each node and
		// switch to the fixed pathfinding instead of reading the freed nodes.
		// GeneralsX: Pointer identity alone missed a path destroyed and then reallocated at the
		// same address - obj ordered away twice in one node's cell block - so the walk read the
		// freed nodes anyway. destroyPath() counts, and any destroy stops the walk. No destroy: bit-identical.
		const AIUpdateInterface *ai = obj->getAIUpdateInterface();
		const UnsignedInt pathDestroys = ai != nullptr ? ai->getPathDestroyCount() : 0;
		for (PathNode *node = path->getLastNode(); node && node != path->getFirstNode(); node = node->getPrevious())
		{
			ICoord2D curCell;
			worldToCell(node->getPosition(), &curCell);
			moveAlliesAroundCell(obj, curCell, node->getLayer(), radius, numCellsAbove, ignoreId, blockedByAlly);
			if (ai != nullptr && (ai->getPath() != path || ai->getPathDestroyCount() != pathDestroys))
			{
				s_useFixedPathfinding = true;
				return true;
			}
		}
	}
	else
#endif
	{
		// TheSuperHackers @bugfix bobtista 20/09/2026 Walk a copy of the path cells, so a move away order that recurses
		// back into this unit and destroys the path cannot pull the nodes out from under the walk.
		std::vector<MoveAlliesCell> &cells = m_moveAlliesCells[m_moveAlliesDepth - 1];
		cells.clear();
		for (const PathNode *node = path->getLastNode(); node && node != path->getFirstNode(); node = node->getPrevious())
		{
			MoveAlliesCell entry;
			worldToCell(node->getPosition(), &entry.cell);
			entry.layer = node->getLayer();
			cells.push_back(entry);
		}

		for (std::vector<MoveAlliesCell>::const_iterator it = cells.begin(); it != cells.end(); ++it)
		{
			moveAlliesAroundCell(obj, it->cell, it->layer, radius, numCellsAbove, ignoreId, blockedByAlly);
		}
	}
	return true;
}


/**
 * Moves an allied unit out of the path of another unit.
 * Uses A* algorithm.
 */
Path *Pathfinder::getMoveAwayFromPath(Object* obj, Object *otherObj,
											Path *pathToAvoid, Object *otherObj2, Path *pathToAvoid2)
{
	if (!m_isMapReady)
		return nullptr; // Should always be ok.

#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif
	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}
	Bool otherCenter;
	Int otherRadius;
	getRadiusAndCenter(otherObj, otherRadius, otherCenter);

	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;

	m_zoneManager.setAllPassable();

	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);

	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));

	// determine start cell
	ICoord2D startCellNdx;
	Coord3D startPos = *obj->getPosition();
	if (!centerInCell) {
		startPos.x += PATHFIND_CELL_SIZE_F*0.5f;
		startPos.x += PATHFIND_CELL_SIZE_F*0.5f;
	}
	worldToCell(&startPos, &startCellNdx);
	// GeneralsX: The start cell's info gets the start cell's OWN index. It was allocated with
	// startCellNdx, which the footprint adjustment above (x shifted twice, y never - a 2003 typo kept as is, it only
	// feeds the movement checks) puts one column to the right of the cell getClippedCell returns. An info whose m_pos is
	// not its cell outlives this search on a marked cell; any later search expanding that cell walks from the wrong one,
	// whose missing info examineCellsCallback then reads (from->getCostSoFar(): null, a deterministic crash, Crater Ring
	// 12 AI seed 401 frame ~41300). SIM-VISIBLE: inside the digest. Side effect, by design: the "always move at least 1
	// cell" test below compares cells with startCellNdx, so it skips (x+1, y) rather than the start cell; it now does that
	// every time, as it already did whenever the start cell held an info (the usual case). Correcting that test would
	// change every 2x2 unit's move-away path - a separate decision.
	ICoord2D parentCellNdx;
	worldToCell(obj->getPosition(), &parentCellNdx);
	PathfindCell *parentCell = getCell( obj->getLayer(), parentCellNdx.x, parentCellNdx.y );	// = getClippedCell( obj->getLayer(), obj->getPosition() )
	if (!parentCell)
		return nullptr;

	if (!obj->getAIUpdateInterface()) // shouldn't happen, but can't move it without an ai.
		return nullptr; 

	const LocomotorSet& locomotorSet = obj->getAIUpdateInterface()->getLocomotorSet();

	m_isTunneling = false;
	if (validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), parentCell ) == false) {
		m_isTunneling = true; // We can't move from our current location.  So relax the constraints.
	}

	TCheckMovementInfo info;
	info.cell = startCellNdx;
	info.layer = obj->getLayer();
	info.centerInCell = centerInCell;
	info.radius = radius;
	info.considerTransient = false;
	info.acceptableSurfaces = locomotorSet.getValidSurfaces();
	if (!checkForMovement(obj, info) || info.enemyFixed) {
		m_isTunneling = true; // We can't move from our current location.  So relax the constraints.
	}

	if (!parentCell->allocateInfo(parentCellNdx)) {
		return nullptr;
	}
	parentCell->startPathfind(nullptr);

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//

	Real boxHalfWidth = radius*PATHFIND_CELL_SIZE_F - (PATHFIND_CELL_SIZE_F/4.0f);
	if (centerInCell) boxHalfWidth+=PATHFIND_CELL_SIZE_F/2;
	boxHalfWidth += otherRadius*PATHFIND_CELL_SIZE_F;
	if (otherCenter) boxHalfWidth+=PATHFIND_CELL_SIZE_F/2;

	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_MOVE_AWAY); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		Region2D bounds;
		Coord3D cellCenter;
		adjustCoordToCell(parentCell->getXIndex(), parentCell->getYIndex(), centerInCell, cellCenter, parentCell->getLayer());
		bounds.lo.x = cellCenter.x-boxHalfWidth;
		bounds.lo.y = cellCenter.y-boxHalfWidth;
		bounds.hi.x = cellCenter.x+boxHalfWidth;
		bounds.hi.y = cellCenter.y+boxHalfWidth;
		PathNode *node;
		Bool overlap = false;

		for( node = pathToAvoid->getFirstNode(); node && node->getNextOptimized(); node = node->getNextOptimized() )	{
			Coord2D start, end;
			start = node->getPosition()->asCoord2D();
			end = node->getNextOptimized()->getPosition()->asCoord2D();
			if (LineInRegion(&start, &end, &bounds)) {
				overlap = true;
				break;
			}
		}

		if (!overlap && pathToAvoid2) {
			for( node = pathToAvoid2->getFirstNode(); node && node->getNextOptimized(); node = node->getNextOptimized() )	{
				Coord2D start, end;
				start = node->getPosition()->asCoord2D();
				end = node->getNextOptimized()->getPosition()->asCoord2D();
				if (LineInRegion(&start, &end, &bounds)) {
					overlap = true;
					break;
				}
			}
		}
		if (!overlap) {
			if (startCellNdx.x == parentCell->getXIndex() && startCellNdx.y == parentCell->getYIndex()) {
				// we didn't move. Always move at least 1 cell. jba.
				overlap = true;
			}
		}
		///@todo - Adjust cost intersecting path - closer to front is more expensive. jba.
		if (!overlap && checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(),
				parentCell->getLayer(), radius, centerInCell)) {
			// success - found a path to the goal
			if (false && TheGlobalData->m_debugAI)
				debugShowSearch(true);
			m_isTunneling = false;
			// construct and return path
			Path *newPath = buildActualPath( obj, locomotorSet.getValidSurfaces(), obj->getPosition(), parentCell, centerInCell, false);
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding) {
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
			}
			return newPath;
		}
		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		// Check to see if we can change layers in this cell.
		checkChangeLayers(parentCell);

		examineNeighboringCells(parentCell, nullptr, locomotorSet, isHuman, centerInCell, radius, startCellNdx, obj, NO_ATTACK);

	}

#if defined(RTS_DEBUG)
	debugShowSearch(true);
#endif

	DEBUG_LOG(("%d getMoveAwayFromPath pathfind failed --", TheGameLogic->getFrame()));
	DEBUG_LOG(("Unit '%s', time %f", obj->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));

	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
	}
	return nullptr;
}


/** Patch to the exiting path from the current position, either because we became blocked,
  or because we had to move off the path to avoid other units. */
Path *Pathfinder::patchPath( const Object *obj, const LocomotorSet& locomotorSet,
		Path *originalPath, Bool blocked )
{
	//CRCDEBUG_LOG(("Pathfinder::patchPath()"));
#ifdef DEBUG_LOGGING
	Int startTimeMS = ::GetTickCount();
#endif
	if (originalPath==nullptr) return nullptr;
	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);
	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}

	m_zoneManager.setAllPassable();

	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));

	enum {CELL_LIMIT = 2000}; // max cells to examine.
	Int cellCount = 0;

	Coord3D currentPosition = *obj->getPosition();

	// determine start cell
	ICoord2D startCellNdx;
	Coord3D startPos = *obj->getPosition();
	if (!centerInCell) {
		startPos.x += PATHFIND_CELL_SIZE_F*0.5f;
		startPos.x += PATHFIND_CELL_SIZE_F*0.5f;
	}
	worldToCell(&startPos, &startCellNdx);
	//worldToCell(obj->getPosition(), &startCellNdx);
	// GeneralsX: The start cell's info gets its own index, not startCellNdx: the same typo'd
	// footprint shift as getMoveAwayFromPath, the same stale m_pos, the same crash. SIM-VISIBLE: inside the digest.
	ICoord2D parentCellNdx;
	worldToCell(&currentPosition, &parentCellNdx);
	PathfindCell *parentCell = getCell( obj->getLayer(), parentCellNdx.x, parentCellNdx.y );	// = getClippedCell( obj->getLayer(), &currentPosition )
	if (parentCell == nullptr)
		return nullptr;
	if (!obj->getAIUpdateInterface()) {
		return nullptr; // shouldn't happen, but can't move it without an ai.
	}

	m_isTunneling = false;

	if (!parentCell->allocateInfo(parentCellNdx)) {
		return nullptr;
	}
	parentCell->startPathfind( nullptr);

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//

#if defined(RTS_DEBUG)
	extern void addIcon(const Coord3D *pos, Real width, Int numFramesDuration, RGBColor color);
	if (TheGlobalData->m_debugAI)
	{
		RGBColor color;
		color.setFromInt(0);
		addIcon(nullptr, 0,0,color);
	}
#endif

	PathNode *startNode;
	Coord3D goalPos = *originalPath->getLastNode()->getPosition();
	Real goalDeltaSqr = sqr(goalPos.x-currentPosition.x) + sqr(goalPos.y - currentPosition.y);
	for( startNode = originalPath->getLastNode(); startNode != originalPath->getFirstNode(); startNode = startNode->getPrevious() )	{
		ICoord2D cellCoord;
		worldToCell(startNode->getPosition(), &cellCoord);
		TCheckMovementInfo info;
		info.cell = cellCoord;
		info.layer = startNode->getLayer();
		info.centerInCell = centerInCell;
		info.radius = radius;
		info.considerTransient = blocked;
		info.acceptableSurfaces = locomotorSet.getValidSurfaces();
#if defined(RTS_DEBUG)
		if (TheGlobalData->m_debugAI) {
			RGBColor color;
			color.setFromInt(0);
			color.green = 1;
			addIcon(startNode->getPosition(), PATHFIND_CELL_SIZE_F*0.5f, 100, color);
		}
#endif
		Int dx = cellCoord.x-startCellNdx.x;
		Int dy = cellCoord.y-startCellNdx.y;
		if (dx<-2 || dx>2) info.considerTransient = false;
		if (dy<-2 || dy>2) info.considerTransient = false;
		if (!checkForMovement(obj, info)) {
			break;
		}
		if (info.allyFixedCount || info.enemyFixed) {
			break;	// Don't patch through cells that are occupied.
		}
		Real curSqr = sqr(startNode->getPosition()->x-currentPosition.x) + sqr(startNode->getPosition()->y - currentPosition.y);
		if (curSqr < goalDeltaSqr) {
			goalPos = *startNode->getPosition();
			goalDeltaSqr = curSqr;
		}

	}
	// GeneralsX: Both exits below come after the start cell was put on the sorted open
	// list (m_open = TRUE), so the fixed arm's bare parentCell->releaseInfo() was refused as a "Bad release": the
	// pool entry leaked and the cell stayed flagged open for later searches. Unwind the lists like every other
	// search exit does. The interaction of TheSuperHackers #994 and #2327; upstream still has it (tsh batch2 #83).
	if (startNode == originalPath->getLastNode()) {
		cleanOpenAndClosedLists();
		return nullptr; // no open nodes.
	}
	PathfindCell *candidateGoal;
	candidateGoal = getCell(LAYER_GROUND, &goalPos); // just using for cost estimates.
	ICoord2D goalCellNdx;
	worldToCell(&goalPos, &goalCellNdx);
	// GeneralsX: goalPos is a node of the unit's current path, and a quick path keeps exact
	// positions that may lie off the grid, where getCell returns nullptr: give up the patch as when the pool is empty.
	if (!candidateGoal || !candidateGoal->allocateInfo(goalCellNdx)) {
		cleanOpenAndClosedLists();
		return nullptr;
	}

	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_PATCH); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		Coord3D cellCenter;
		adjustCoordToCell(parentCell->getXIndex(), parentCell->getYIndex(), centerInCell, cellCenter, parentCell->getLayer());
		PathNode *matchNode;
		Bool found = false;
		for( matchNode = originalPath->getLastNode(); matchNode != startNode; matchNode = matchNode->getPrevious() )	{
			if (cellCenter.x == matchNode->getPosition()->x && cellCenter.y == matchNode->getPosition()->y)	{
				found = true;
				break;
			}
		}
		if (found ) {
			// success - found a path to the goal
			if ( TheGlobalData->m_debugAI)
				debugShowSearch(true);
			m_isTunneling = false;
			// construct and return path
			Path *path = newInstance(Path);
			PathNode *node;
			for( node = originalPath->getLastNode(); node != matchNode; node = node->getPrevious() )	{
				path->prependNode(node->getPosition(), node->getLayer());
			}
			prependCells(path, obj->getPosition(), parentCell, centerInCell);

			// cleanup the path by checking line of sight
			path->optimize(obj, locomotorSet.getValidSurfaces(), blocked);
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding) {
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
				candidateGoal->releaseInfo();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
				candidateGoal->releaseInfo();
			}

			return path;
		}
		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		if (cellCount < CELL_LIMIT) {
			// Check to see if we can change layers in this cell.
			checkChangeLayers(parentCell);
			cellCount += examineNeighboringCells(parentCell, nullptr, locomotorSet, isHuman, centerInCell, radius, startCellNdx, obj, NO_ATTACK);
		}
	}

	DEBUG_LOG(("%d patchPath Pathfind failed --", TheGameLogic->getFrame()));
	DEBUG_LOG(("Unit '%s', time %f", obj->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_debugAI) {
		debugShowSearch(true);
	}
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		if (!candidateGoal->getOpen() && !candidateGoal->getClosed())
		{
			// Not on one of the lists 
			candidateGoal->releaseInfo();
		}
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		candidateGoal->releaseInfo();
	}
	return nullptr;
}


/** Find a short, valid path to a location that obj can attack victim from.  */
Path *Pathfinder::findAttackPath( const Object *obj, const LocomotorSet& locomotorSet, const Coord3D *from,
		const Object *victim, const Coord3D* victimPos, const Weapon *weapon )
{
	if (!m_isMapReady)
		return nullptr; // Should always be ok.

	Bool isCrusher = obj ? obj->getCrusherLevel() > 0 : false;
	Int radius;
	Bool centerInCell;
	getRadiusAndCenter(obj, radius, centerInCell);

	// Quick check:  See if moving couple of cells towards the victim will work.
	{
		Coord3D curPos = *obj->getPosition();
		Coord3D goalPos = victim?*victim->getPosition():*victimPos;
		Coord3D delta;
		delta.set(goalPos.x-curPos.x, goalPos.y-curPos.y, 0);
		delta.normalize();
		delta.x *= PATHFIND_CELL_SIZE_F;
		delta.y *= PATHFIND_CELL_SIZE_F;
		Int i;
		for (i=1; i<10; i++) {
			Coord3D testPos = curPos;
			testPos.x += delta.x*i*0.5f;
			testPos.y += delta.y*i*0.5f;

			ICoord2D cellNdx;
			worldToCell(&testPos, &cellNdx);
			PathfindCell *aCell = getCell(obj->getLayer(), cellNdx.x, cellNdx.y);
			if (!aCell)
				break;

			if (!validMovementPosition(isCrusher, locomotorSet.getValidSurfaces(), aCell))
				break;

			if (!checkDestination(obj, cellNdx.x, cellNdx.y, obj->getLayer(), radius, centerInCell))
				break;

			if (!weapon->isGoalPosWithinAttackRange(obj, &testPos, victim, victimPos))
				continue;

			if (isAttackViewBlockedByObstacle(obj, testPos, victim, *victimPos))
				continue;

			// return path.
			Path *path = newInstance(Path);
			path->prependNode( &testPos, obj->getLayer() );
			path->prependNode( &curPos, obj->getLayer() );
			path->getFirstNode()->setNextOptimized(path->getFirstNode()->getNext());
			if (TheGlobalData->m_debugAI==AI_DEBUG_PATHS) {
				setDebugPath(path);
			}
			return path;
		}
	}

	const Int ATTACK_CELL_LIMIT = 2500; // this is a rather expensive operation, so limit the search.

	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}
	m_zoneManager.clearPassableFlags();
	Path *hPat = findClosestHierarchicalPath(isHuman, locomotorSet, from, victimPos, isCrusher);
	if (hPat) {
		deleteInstance(hPat);
	}	else {
		m_zoneManager.setAllPassable();
	}

	Int cellCount = 0;

	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));

	Int attackDistance = weapon->getAttackDistance(obj, victim, victimPos);
	attackDistance += 3*PATHFIND_CELL_SIZE;

		// determine start cell
	ICoord2D startCellNdx;
	Coord3D objPos = *obj->getPosition();
	// since worldtocell truncates, add.
	if (centerInCell) {
		objPos.x += PATHFIND_CELL_SIZE_F/2.0f;
		objPos.y += PATHFIND_CELL_SIZE_F/2.0f;
	}

	if (!obj->getAIUpdateInterface()) // shouldn't happen, but can't move without an ai.
		return nullptr;

	worldToCell(&objPos, &startCellNdx);
	PathfindCell *parentCell = getClippedCell( obj->getLayer(), &objPos );
	if (!parentCell)
		return nullptr;

	if (!parentCell->allocateInfo(startCellNdx))
		return nullptr;

	const PathfindCell *startCell = parentCell;
	parentCell->startPathfind(nullptr);

	// determine start cell
	ICoord2D victimCellNdx;
	worldToCell(victim ? victim->getPosition() : victimPos, &victimCellNdx);

	// determine goal cell
	PathfindCell *goalCell = getCell( LAYER_GROUND, victimCellNdx.x, victimCellNdx.y );
	if (!goalCell)
		return nullptr;

 	if (!goalCell->allocateInfo(victimCellNdx)) {
		return nullptr;
	}

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//

	PathfindCell *closestCell = nullptr;
	Real closestDistanceSqr = FLT_MAX;
	Bool checkLOS = false;
	if (!victim) {
		checkLOS = true;
	}
	if (victim && !victim->isSignificantlyAboveTerrain()) {
		checkLOS = true;
	}

	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_ATTACK); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		Coord3D cellCenter;
		adjustCoordToCell(parentCell->getXIndex(), parentCell->getYIndex(), centerInCell, cellCenter, parentCell->getLayer());

		///@todo - Adjust cost intersecting path - closer to front is more expensive. jba.
		if (weapon->isGoalPosWithinAttackRange(obj, &cellCenter, victim, victimPos) &&
			checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(),
				parentCell->getLayer(), radius, centerInCell)) {
			// check line of sight.
			Bool viewBlocked = false;
			if (checkLOS)
			{
				viewBlocked = isAttackViewBlockedByObstacle(obj, cellCenter, victim, *victimPos);
			}
			if (startCell == parentCell) {
				// We never want to accept our starting cell.
				// If we could attack from there, we wouldn't be calling
				// FindAttackPath.  Usually happens cause the cell is valid for attack, but
				// a point near the cell center isn't, and that happens to be where the
				// attacker is standing, and it's too close to move to.
				viewBlocked = true;
			} else {
				// If through some unfortunate rounding, we end up moving near ourselves,
				// don't want it.
				Coord3D cellPos;
				adjustCoordToCell(parentCell->getXIndex(), parentCell->getYIndex(), centerInCell, cellPos, parentCell->getLayer());
				Real dx = (cellPos.x - objPos.x);
				Real dy = (cellPos.y - objPos.y);
				if (sqr(dx) + sqr(dy) < sqr(PATHFIND_CELL_SIZE_F*0.5f)) {
					viewBlocked = true;
				}
			}
			if (!viewBlocked)
			{
				// success - found a path to the goal
				Bool show = TheGlobalData->m_debugAI;
	#ifdef INTENSE_DEBUG
				Int count = 0;
				PathfindCell *cur;
				for (cur = m_closedList.getHead(); cur; cur=cur->getNextOpen()) {
					count++;
				}
				if (count>1000) {
					show = true;
					DEBUG_LOG(("FAP cells %d obj %s %x", count, obj->getTemplate()->getName().str(), obj));
	#ifdef STATE_MACHINE_DEBUG
					if( obj->getAIUpdateInterface() )
					{
						DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
					}
	#endif
					TheScriptEngine->AppendDebugMessage("Big Attack path", false);
				}
	#endif
				if (show)
					debugShowSearch(true);

#if !(RTS_GENERALS && RETAIL_COMPATIBLE_PATHFINDING)
				// put parent cell onto closed list - its evaluation is finished
				parentCell->putOnClosedList( m_closedList );

				if (obj->isKindOf(KINDOF_VEHICLE)) {
					// Strip backwards.
					PathfindCell *lastBlocked = nullptr;
					PathfindCell *cur = parentCell;
					Bool useLargeRadius = false;
					Int cellLimit = 12; // Magic number, yes I know - jba.   It is about 4 * size of an average vehicle width (3 cells) [8/15/2003]
					while (cur) {
						cellLimit--;
						if (cellLimit<0) {
							break;
						}
						TCheckMovementInfo info;
						info.cell.x = cur->getXIndex();
						info.cell.y = cur->getYIndex();
						info.layer = cur->getLayer();
						if (useLargeRadius) {
							info.centerInCell = centerInCell;
							info.radius = radius;
						} else {
							info.centerInCell = true;
							info.radius = 0;
						}
						info.considerTransient = false;
						info.acceptableSurfaces = locomotorSet.getValidSurfaces();
						PathfindCell	*cell = getCell(info.layer,info.cell.x,info.cell.y);
						Bool unitIdle = false;
						if (cell) {
							ObjectID posUnit = cell->getPosUnit();
							Object *unit = TheGameLogic->findObjectByID(posUnit);
							if (unit && unit->getAI() && unit->getAI()->isIdle()) {
								unitIdle = true;
							}
						}
						Bool checkMovement = checkForMovement(obj, info);
						Bool blockedByEnemy = info.enemyFixed;
						Bool blockedByAllies = info.allyFixedCount || info.allyGoal;
						if (unitIdle) {
							// If the unit present is idle, it doesn't block allies. [8/18/2003]
							blockedByAllies = false;
						}


						if (!checkMovement || blockedByEnemy || blockedByAllies) {
							lastBlocked = cur;
							useLargeRadius = true;
						} else {
							useLargeRadius = false;
						}
						cur = cur->getParentCell();
					}
					if (lastBlocked) {
						parentCell = lastBlocked;
						if (lastBlocked->getParentCell()) {
							parentCell = lastBlocked->getParentCell();
						}
					}
				}
#endif
				// construct and return path
				Path *path = buildActualPath( obj, locomotorSet.getValidSurfaces(), obj->getPosition(), parentCell, centerInCell, false);
#if RETAIL_COMPATIBLE_PATHFINDING
				if (!s_useFixedPathfinding) {
#if RTS_GENERALS
					// GeneralsX: Restores retail Generals A* behaviour that
					// earlier upstream crash-fix work removed; the fork inherited the regression by
					// ancestry rather than by a port it could have declined. Generals only.
					// Upstream reference: Caball009, PR #3289
					// https://github.com/TheSuperHackers/GeneralsGameCode/pull/3289
					parentCell->releaseInfo();
#endif

					if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
						goalCell->releaseInfo();
					}
					cleanOpenAndClosedLists();
				}
				else
#endif
				{
					cleanOpenAndClosedLists();
					parentCell->releaseInfo();
					goalCell->releaseInfo();
				}
				return path;
			}
		}
		if (checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(), parentCell->getLayer(), radius, centerInCell)) {
			if (validMovementPosition( isCrusher, locomotorSet.getValidSurfaces(), parentCell )) {
				Real dx = IABS(victimCellNdx.x-parentCell->getXIndex());
				Real dy = IABS(victimCellNdx.y-parentCell->getYIndex());
				Real distSqr = dx*dx+dy*dy;
				if (distSqr < closestDistanceSqr) {
					closestCell = parentCell;
					closestDistanceSqr = distSqr;
				}
			}
		}

		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		if (cellCount < ATTACK_CELL_LIMIT) {
				// Check to see if we can change layers in this cell.
			checkChangeLayers(parentCell);
			cellCount += examineNeighboringCells(parentCell, goalCell, locomotorSet, isHuman, centerInCell,
				radius, startCellNdx, obj, attackDistance);
		}

	}

#ifdef INTENSE_DEBUG
	DEBUG_LOG(("obj %s %x", obj->getTemplate()->getName().str(), obj));
#ifdef STATE_MACHINE_DEBUG
	if( obj->getAIUpdateInterface() )
	{
		DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
	}
#endif
	debugShowSearch(true);
	TheScriptEngine->AppendDebugMessage("Overflowed attack path", false);
#endif
#if 0
	if (closestCell) {
		// construct and return path
		Path *path = buildActualPath( obj, locomotorSet, obj->getPosition(), closestCell, centerInCell, false);
		cleanOpenAndClosedLists();
		return path;
	}
#if defined(RTS_DEBUG)
	DEBUG_LOG(("%d (%d cells) Attack Pathfind failed from (%f,%f) to (%f,%f) --", TheGameLogic->getFrame(), cellCount, from->x, from->y, victim->getPosition()->x, victim->getPosition()->y));
	DEBUG_LOG(("Unit '%s', attacking '%s' time %f", obj->getTemplate()->getName().str(),  victim->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));
#endif
#endif
#ifdef DUMP_PERF_STATS
	TheGameLogic->incrementOverallFailedPathfinds();
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		if (goalCell->hasInfo() && !goalCell->getClosed() && !goalCell->getOpen()) {
			goalCell->releaseInfo();
		}
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
		goalCell->releaseInfo();
	}
	return nullptr;
}

/** Find a short, valid path to a location that is safe from the repulsors.  */
Path *Pathfinder::findSafePath( const Object *obj, const LocomotorSet& locomotorSet,
		const Coord3D *from, const Coord3D* repulsorPos1, const Coord3D* repulsorPos2, Real repulsorRadius)
{
	//CRCDEBUG_LOG(("Pathfinder::findSafePath()"));
	if (m_isMapReady == false) return nullptr; // Should always be ok.
#if defined(RTS_DEBUG)
//	Int startTimeMS = ::GetTickCount();
#endif

	const Int MAX_CELLS = MAX_SAFE_PATH_CELL_COUNT; // this is a rather expensive operation, so limit the search.

	Bool centerInCell;
	Int radius;
	getRadiusAndCenter(obj, radius, centerInCell);
	Real repulsorDistSqr = repulsorRadius*repulsorRadius;
	Int cellCount = 0;
	Bool isHuman = true;
	if (obj && obj->getControllingPlayer() && (obj->getControllingPlayer()->getPlayerType()==PLAYER_COMPUTER)) {
		isHuman = false; // computer gets to cheat.
	}

	DEBUG_ASSERTCRASH(m_openList.empty() && m_closedList.empty(), ("Dangling lists."));
	// create unique "mark" values for open and closed cells for this pathfind invocation

	m_zoneManager.setAllPassable();
	// determine start cell
	ICoord2D startCellNdx;
	worldToCell(obj->getPosition(), &startCellNdx);
	PathfindCell *parentCell = getClippedCell( obj->getLayer(), obj->getPosition() );
	if (parentCell == nullptr)
		return nullptr;
	if (!obj->getAIUpdateInterface()) {
		return nullptr; // shouldn't happen, but can't move it without an ai.
	}
	if (!parentCell->allocateInfo(startCellNdx)) {
		return nullptr;
	}
	parentCell->startPathfind( nullptr);

	// initialize "open" list to contain start cell
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		m_openList.reset(parentCell);
	}
	else
#endif
	{
		m_openList.reset();
		parentCell->putOnSortedOpenList(m_openList);
	}

	// "closed" list is initially empty
	m_closedList.reset();

	//
	// Continue search until "open" list is empty, or
	// until goal is found.
	//

	Real farthestDistanceSqr = 0;

	GXPathSearchProbe gxPathProbe(m_cumulativeCellsAllocated, GXPATH_SAFE); // GeneralsX A* probe, observation only
	while( !m_openList.empty() )
	{
		// take head cell off of open list - it has lowest estimated total path cost
		parentCell = m_openList.getHead();
		parentCell->removeFromOpenList(m_openList);

		Coord3D cellCenter;
		adjustCoordToCell(parentCell->getXIndex(), parentCell->getYIndex(), centerInCell, cellCenter, parentCell->getLayer());

		///@todo - Adjust cost intersecting path - closer to front is more expensive. jba.
		Real dx = cellCenter.x-repulsorPos1->x;
		Real dy = cellCenter.y-repulsorPos1->y;
		Bool ok = false;
		Real distSqr = dx*dx+dy*dy;
		dx = cellCenter.x-repulsorPos2->x;
		dy = cellCenter.y-repulsorPos2->y;
		Real distSqr2 = dx*dx+dy*dy;
		if (distSqr2<distSqr) {
			distSqr = distSqr2;
		}
		if (distSqr>repulsorDistSqr) {
			ok = true;
		}
		if (m_openList.empty() && cellCount>0) {
			ok = true; // exhausted the search space, just take the last cell.
		}
		if (distSqr > farthestDistanceSqr) {
			farthestDistanceSqr = distSqr;
			if (cellCount > MAX_CELLS) {
#ifdef INTENSE_DEBUG
				DEBUG_LOG(("Took intermediate path, dist %f, goal dist %f", sqrt(farthestDistanceSqr), repulsorRadius));
#endif
				ok = true; // Already a big search, just take this one.
			}
		}
		if ( ok &&
			checkDestination(obj, parentCell->getXIndex(), parentCell->getYIndex(),
				parentCell->getLayer(), radius, centerInCell)) {
			// success - found a path to the goal
			Bool show = TheGlobalData->m_debugAI;
#ifdef INTENSE_DEBUG
			Int count = 0;
			PathfindCell *cur;
			for (cur = m_closedList.getHead(); cur; cur=cur->getNextOpen()) {
				count++;
			}
			if (count>2000) {
				show = true;
				DEBUG_LOG(("cells %d obj %s %x", count, obj->getTemplate()->getName().str(), obj));
#ifdef STATE_MACHINE_DEBUG
				if( obj->getAIUpdateInterface() )
				{
					DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
				}
#endif
				TheScriptEngine->AppendDebugMessage("Big Safe path", false);
			}
#endif
			if (show)
				debugShowSearch(true);
#if defined(RTS_DEBUG)
			//DEBUG_LOG(("Attack path took %d cells, %f sec", cellCount, (::GetTickCount()-startTimeMS)/1000.0f));
#endif
			// construct and return path
			Path *path = buildActualPath( obj, locomotorSet.getValidSurfaces(), obj->getPosition(), parentCell, centerInCell, false);
#if RETAIL_COMPATIBLE_PATHFINDING
			if (!s_useFixedPathfinding) {
				parentCell->releaseInfo();
				cleanOpenAndClosedLists();
			}
			else
#endif
			{
				cleanOpenAndClosedLists();
				parentCell->releaseInfo();
			}
			return path;
		}

		// put parent cell onto closed list - its evaluation is finished
		parentCell->putOnClosedList( m_closedList );

		// Check to see if we can change layers in this cell.
		checkChangeLayers(parentCell);

		cellCount += examineNeighboringCells(parentCell, nullptr, locomotorSet, isHuman, centerInCell, radius, startCellNdx, obj, NO_ATTACK);

	}

#ifdef INTENSE_DEBUG
	DEBUG_LOG(("obj %s %x count %d", obj->getTemplate()->getName().str(), obj, cellCount));
#ifdef STATE_MACHINE_DEBUG
	if( obj->getAIUpdateInterface() )
	{
		DEBUG_LOG(("State %s",  obj->getAIUpdateInterface()->getCurrentStateName().str()));
	}
#endif
	TheScriptEngine->AppendDebugMessage("Overflowed Safe path", false);
#endif
#if 0
#if defined(RTS_DEBUG)
	DEBUG_LOG(("%d (%d cells) Attack Pathfind failed from (%f,%f) to (%f,%f) --", TheGameLogic->getFrame(), cellCount, from->x, from->y, victim->getPosition()->x, victim->getPosition()->y));
	DEBUG_LOG(("Unit '%s', attacking '%s' time %f", obj->getTemplate()->getName().str(),  victim->getTemplate()->getName().str(), (::GetTickCount()-startTimeMS)/1000.0f));
#endif
#endif
#ifdef DUMP_PERF_STATS
	TheGameLogic->incrementOverallFailedPathfinds();
#endif
	m_isTunneling = false;
#if RETAIL_COMPATIBLE_PATHFINDING
	if (!s_useFixedPathfinding) {
		cleanOpenAndClosedLists();
	}
	else
#endif
	{
		cleanOpenAndClosedLists();
		parentCell->releaseInfo();
	}
	return nullptr;
}

//-----------------------------------------------------------------------------
void Pathfinder::crc( Xfer *xfer )
{
	CRCDEBUG_LOG(("Pathfinder::crc() on frame %d", TheGameLogic->getFrame()));
	CRCDEBUG_LOG(("beginning CRC: %8.8X", ((XferCRC *)xfer)->getCRC()));

	xfer->xferUser( &m_extent, sizeof(IRegion2D) );
	CRCDEBUG_LOG(("m_extent: %8.8X", ((XferCRC *)xfer)->getCRC()));

	xfer->xferBool( &m_isMapReady );
	CRCDEBUG_LOG(("m_isMapReady: %8.8X", ((XferCRC *)xfer)->getCRC()));
	xfer->xferBool( &m_isTunneling );
	CRCDEBUG_LOG(("m_isTunneling: %8.8X", ((XferCRC *)xfer)->getCRC()));

	Int obsolete1 = 0;
	xfer->xferInt( &obsolete1 );

	xfer->xferUser(&m_ignoreObstacleID, sizeof(ObjectID));
	CRCDEBUG_LOG(("m_ignoreObstacleID: %8.8X", ((XferCRC *)xfer)->getCRC()));

	xfer->xferUser(m_queuedPathfindRequests, sizeof(ObjectID)*PATHFIND_QUEUE_LEN);
	CRCDEBUG_LOG(("m_queuedPathfindRequests: %8.8X", ((XferCRC *)xfer)->getCRC()));
	xfer->xferInt(&m_queuePRHead);
	CRCDEBUG_LOG(("m_queuePRHead: %8.8X", ((XferCRC *)xfer)->getCRC()));
	xfer->xferInt(&m_queuePRTail);
	CRCDEBUG_LOG(("m_queuePRTail: %8.8X", ((XferCRC *)xfer)->getCRC()));

	xfer->xferInt(&m_numWallPieces);
	CRCDEBUG_LOG(("m_numWallPieces: %8.8X", ((XferCRC *)xfer)->getCRC()));

#if RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @fix The original code effectively accessed m_numWallPieces 128 times,
	// because it used &m_wallPieces[MAX_WALL_PIECES] which is out-of-bounds and points to m_numWallPieces.
	static_assert(sizeof(Int) == sizeof(ObjectID), "Type sizes must be equal for correct xfer");

	for (Int i = 0; i < MAX_WALL_PIECES; ++i)
	{
		xfer->xferInt(&m_numWallPieces);
	}
#else
	xfer->xferUser(m_wallPieces, sizeof(m_wallPieces));
#endif

	CRCDEBUG_LOG(("m_wallPieces: %8.8X", ((XferCRC *)xfer)->getCRC()));

	xfer->xferReal(&m_wallHeight);
	CRCDEBUG_LOG(("m_wallHeight: %8.8X", ((XferCRC *)xfer)->getCRC()));
	xfer->xferInt(&m_cumulativeCellsAllocated);
	CRCDEBUG_LOG(("m_cumulativeCellsAllocated: %8.8X", ((XferCRC *)xfer)->getCRC()));

}

//-----------------------------------------------------------------------------
void Pathfinder::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 1;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

}

//-----------------------------------------------------------------------------
void Pathfinder::loadPostProcess()
{

}

// ------------------------------------------------------------------------------------------------
// GeneralsX: THE PATHFIND QUEUE IS SAVED. A unit that asked for a path sets m_waitingForPath
// and waits in this queue; AIUpdateInterface saves the flag, but nothing saved the queue (AI::xfer writes only its
// version, and GameLogic's load runs newMap(), which empties it). A game saved while units were queued loaded them
// waiting for a path that never came: the move state returns STATE_CONTINUE while isWaitingForPath() for ever, and a
// unit asleep "until its path comes" never wakes. The queue now travels in a block of its own, CHUNK_GXPathfindQueue
// (GameState::init, both games), after CHUNK_GameLogic, so it is read after that newMap(): the ObjectIDs in queue
// order, head first. Retail and older GeneralsX builds skip the block by its size; a save without it loads with the
// queue empty, as before.
// ------------------------------------------------------------------------------------------------
void Pathfinder::gxXferQueue( Xfer *xfer )
{
	Int count = 0;
	if (xfer->getXferMode() == XFER_SAVE) {
		for (Int slot = m_queuePRHead; slot != m_queuePRTail; slot = (slot + 1) % PATHFIND_QUEUE_LEN)
			++count;
	}
	xfer->xferInt( &count );
	if (count < 0 || count >= PATHFIND_QUEUE_LEN) {
		DEBUG_CRASH(( "GXPathfindQueue: invalid count %d", count ));
		throw SC_INVALID_DATA;
	}
	if (xfer->getXferMode() == XFER_SAVE) {
		for (Int slot = m_queuePRHead; slot != m_queuePRTail; slot = (slot + 1) % PATHFIND_QUEUE_LEN)
			xfer->xferObjectID( &m_queuedPathfindRequests[slot] );
	} else {
		for (Int i = 0; i < PATHFIND_QUEUE_LEN; ++i)
			m_queuedPathfindRequests[i] = INVALID_ID;
		for (Int i = 0; i < count; ++i)
			xfer->xferObjectID( &m_queuedPathfindRequests[i] );
		m_queuePRHead = 0;
		m_queuePRTail = count;
	}
	fprintf( stderr, "[GXPQ] %s n=%d\n", xfer->getXferMode() == XFER_SAVE ? "save" : "load", count );
}

class GXPathfindQueueState : public Snapshot
{
protected:
	virtual void crc( Xfer * ) override {}	// the queue is in Pathfinder::crc already
	/** Version Info:
		* 1: Initial version - Int count, then count ObjectIDs, the queue head first */
	virtual void xfer( Xfer *xfer ) override
	{
		XferVersion currentVersion = 1;
		XferVersion version = currentVersion;
		xfer->xferVersion( &version, currentVersion );
		TheAI->pathfinder()->gxXferQueue( xfer );
	}
	virtual void loadPostProcess() override {}
};
static GXPathfindQueueState s_gxPathfindQueueState;
Snapshot *TheGXPathfindQueueState = &s_gxPathfindQueueState;
