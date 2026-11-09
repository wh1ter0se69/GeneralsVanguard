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

// FILE: PartitionManager.h //////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:    RTS3
//
// File name:  PartitionManager.h
//
// Created:    Steven Johnson, September 2001
//
// Desc:       Partition management, this system will allow us to partition the
//						 objects in space, iterate objects in specified volumes,
//						 regions, by types and other properties.
//
//-----------------------------------------------------------------------------

#pragma once

//-----------------------------------------------------------------------------
//           Includes
//-----------------------------------------------------------------------------
#include "Common/GameCommon.h"	// ensure we get DUMP_PERF_STATS, or not
#include "GameLogic/ObjectIter.h"
#include "Common/ObjectStatusTypes.h"
#include "Common/KindOf.h"
#include "Common/Snapshot.h"
#include "Common/Geometry.h"
#include "GameClient/Display.h"	// for ShroudLevel

//-----------------------------------------------------------------------------
//           defines
//-----------------------------------------------------------------------------

/*
	Note, this isn't very accurate currently, so use with caution. (srj)
*/
#define PM_CACHE_TERRAIN_HEIGHT

/*
	5% speed improvement, at cost of more memory.
*/
#define FASTER_GCO

const Real HUGE_DIST = 1000000.0f;

// GeneralsX @performance Claude 06/10/2026 an object's owner tag (PartitionData::friend_getOwnerTag) is its controlling
// player's index, or PARTITION_OWNER_NONE without a team; PartitionManager::m_cellOwnerMask has bit (1 << tag) per tag present in a cell.
enum { PARTITION_OWNER_NONE = MAX_PLAYER_COUNT, PARTITION_OWNER_TAGS };
// GeneralsX @feature Claude 06/11/2026 64-bit: MAX_PLAYER_COUNT 40 needs 41 owner tags. Client/sim-side cache only (the
// mask only lets a search skip cells; it is never saved or hashed), so its width cannot change a result.
typedef UnsignedInt64 PartitionOwnerMask;
inline constexpr PartitionOwnerMask gxOwnerTagBit(Int tag) { return (PartitionOwnerMask)1 << tag; }
static_assert(PARTITION_OWNER_TAGS <= 64, "owner tags must fit a PartitionOwnerMask");
const PartitionOwnerMask PARTITION_OWNERS_ALL = ~(PartitionOwnerMask)0;

//-----------------------------------------------------------------------------
//           Type Definitions
//-----------------------------------------------------------------------------

struct Coord3D;

class CellAndObjectIntersection;
class Object;
class PartitionManager;
class PartitionData;
class PartitionFilter;
class PartitionCell;
class Player;
class PolygonTrigger;
class Squad;
class Team;
class ThingTemplate;
class GhostObject;
class CommandButton;

enum CommandSourceType CPP_11(: Int);

// ----------------------------------------------------------------------------------------------
enum ValueOrThreat CPP_11(: Int)
{
	VOT_CashValue = 1,
	VOT_ThreatValue,
};

// ----------------------------------------------------------------------------------------------
enum FindPositionFlags CPP_11(: Int)
{
	FPF_NONE															= 0x00000000,		// no options, default behavior
	FPF_IGNORE_WATER											= 0x00000001,		// a position found underwater is ok
	FPF_WATER_ONLY												= 0x00000002,		// find positions in the water only
	FPF_IGNORE_ALL_OBJECTS								= 0x00000004,		// ignore all objects, positions inside objects are ok
	FPF_IGNORE_ALLY_OR_NEUTRAL_UNITS			= 0x00000008,		// ignore friendly units (requires relationshipObject)
	FPF_IGNORE_ALLY_OR_NEUTRAL_STRUCTURES	= 0x00000010,		// ignore friendly structures (requires relationshipObject)
	FPF_IGNORE_ENEMY_UNITS								= 0x00000020,		// ignore enemy units (requires relationshipObject)
	FPF_IGNORE_ENEMY_STRUCTURES						= 0x00000040,		// ignore enemy structures (requires relationshipObject)
	FPF_USE_HIGHEST_LAYER									= 0x00000080,		// examine pos on highest layer at given xy (rather than on ground layer)
	FPF_CLEAR_CELLS_ONLY									= 0x00000100,		// Reject anything that is not PathFindCell::Clear
	FPF_IGNORE_ALLIED_MINES								= 0x00000200,		// allied mines don't block (requires relationshipObject)
};
// ----------------------------------------------------------------------------------------------

const Real RANDOM_START_ANGLE = -99999.9f;			///< no start angle (an unlikely number to use for the start angle)

struct FindPositionOptions
{
	FindPositionOptions()
	{
		flags									= FPF_NONE;
		minRadius							= 0.0f;
		maxRadius							= 0.0f;
		startAngle						= RANDOM_START_ANGLE;
		maxZDelta							= 1e10f;	// ie, any z delta.
		ignoreObject					= nullptr;
		sourceToPathToDest    = nullptr;
		relationshipObject		= nullptr;
	};
	FindPositionFlags flags;					///< flags for finding the legal position
	Real minRadius;										///< min radius to search around
	Real maxRadius;										///< max radius to search around
	Real startAngle;									///< use this angle to start the search at
	Real maxZDelta;										///< maximum delta-z we will allow
	const Object *ignoreObject;				///< ignore this object in legal position checks
	const Object *sourceToPathToDest;	///< object that must be able to path to the position chosen
	const Object *relationshipObject;	///< object to use for relationship tests
};

//=====================================
/** */
//=====================================
enum DistanceCalculationType CPP_11(: Int)
{
	FROM_CENTER_2D					= 0,	///< measure from Object center in 2d.
	FROM_CENTER_3D					= 1,	///< measure from Object center in 3d.
	FROM_BOUNDINGSPHERE_2D	= 2,	///< measure from Object bounding sphere in 2d.
	FROM_BOUNDINGSPHERE_3D	= 3		///< measure from Object bounding sphere in 3d.
};

//=====================================
/**
	a Plain Old Data structure that is used to get optional results from collidesWith().
*/
struct CollideLocAndNormal
{
	Coord3D loc;
	Coord3D normal;
};

//=====================================
/**
	PartitionContactList is a utility class used by the Partition Manager
	to hold potential collisions as it updates objects in the partition.
	It stores pairs of potentially-colliding objects (eliminating duplicates)
	for processing after all partitions are updated.
*/
//=====================================
class PartitionContactList;


//=====================================
/**
	This class (often called COI for short) is the abstraction
	of the intersection between an Object and a Partition Cell.
	For every Cell that an Object's geometry touches, even partially,
	we allocate a COI. This allows us to maintain an efficient two-way
	list, such that for every Object, we know the Cells that it touches;
	and, for every Cell, we know the Objects that touch it.
*/
//=====================================
class CellAndObjectIntersection	// not MPO: we allocate these in arrays
{
private:
	PartitionCell							*m_cell;									///< the cell being touched
	PartitionData							*m_module;								///< the module (and thus, Object) touching
	CellAndObjectIntersection *m_prevCoi, *m_nextCoi;		///< if in use, next/prev in this cell. if not in use, next/prev free in this module.

public:

	// Note, we allocate these in arrays, thus we must have a default ctor (and NOT descend from MPO)
	CellAndObjectIntersection();
	~CellAndObjectIntersection();

	/**
		make 'this' refer to the specified cell and module. Normally, this
		involves updated the member variables and adding 'this' to the Cell's
		list of COIs.
	*/
	void addCoverage(PartitionCell *cell, PartitionData *module);

	/**
		make 'this' refer to nothing at all. this involves resetting the member
		variables to null, and removing 'this' from the Cell's list of COI's.
	*/
	void removeAllCoverage();

	/**
		return the Cell for this COI (null if the COI is not in use)
	*/
	PartitionCell *getCell() { return m_cell; }

	/**
		return the Module for this COI (null if the COI is not in use)
	*/
	PartitionData *getModule() { return m_module; }

	/**
		return the previous COI in the Cell's list of COIs.
	*/
	CellAndObjectIntersection *getPrevCoi() { return m_prevCoi; }

	/**
		return the next COI in the Cell's list of COIs.
	*/
	CellAndObjectIntersection *getNextCoi() { return m_nextCoi; }

	// only for use by PartitionCell.
	void friend_addToCellList(CellAndObjectIntersection **pListHead);
	void friend_removeFromCellList(CellAndObjectIntersection **pListHead);
};

/**
	This class encapsulates one area interaction with PartitionCells.  The user decides what to do with it.
*/
class SightingInfo : public MemoryPoolObject, public Snapshot
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( SightingInfo, "SightingInfo" );

public:

	SightingInfo();
	void reset();
	Bool isInvalid() const;

	Coord3D					m_where;
	Real						m_howFar;
	PlayerMaskType	m_forWhom;	// ask not for whom the sighting is masked; it masks for thee

	UnsignedInt			m_data;			// Threat and value use as the value.  Sighting uses it for a Timestamp

protected:

	// snapshot method
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

};

//=====================================
/**
	We sometimes need to save whether or not an area was fogged or permanently revealed through a
	script. This helps us do so.
*/
//=====================================
enum
{
	STORE_DONTTOUCH = 0,
	STORE_FOG = 1,
	STORE_PERMANENTLY_REVEALED = 2
};

struct ShroudStatusStoreRestore
{
	std::vector<UnsignedByte> m_foggedOrRevealed[MAX_PLAYER_COUNT];
	Int m_cellsWide;	// m_cellsHigh is computed by m_foggedOrRevealed[0].size() / m_cellsWide
};

//=====================================
/**
	The world's terrain is partitioned into a large grid of Partition Cells.
	The Cell is the fundamental unit of space in the Partition Manager.
*/
//=====================================
class PartitionCell : public Snapshot	// not MPO: allocated in an array
{
private:
	CellAndObjectIntersection*		m_firstCoiInCell;	///< list of COIs in this cell (may be null).
	ShroudLevel										m_shroudLevel[MAX_PLAYER_COUNT];
#ifdef PM_CACHE_TERRAIN_HEIGHT
	Real													m_loTerrainZ;			///< lowest terrain-pt in this cell
	Real													m_hiTerrainZ;			///< highest terrain-pt in this cell
#endif
	Int														m_threatValue[MAX_PLAYER_COUNT];
	Int														m_cashValue[MAX_PLAYER_COUNT];
	Short													m_coiCount;					///< number of COIs in this cell.
	Short													m_cellX;						///< x-coord of this cell within the Partition Mgr coords (NOT in world coords)
	Short													m_cellY;						///< y-coord of this cell within the Partition Mgr coords (NOT in world coords)

public:

	// Note, we allocate these in arrays, thus we must have a default ctor (and NOT descend from MPO)
	PartitionCell();
#ifdef PM_CACHE_TERRAIN_HEIGHT
	void init(Int x, Int y, Real loZ, Real hiZ) { m_cellX = x; m_cellY = y; m_loTerrainZ = loZ; m_hiTerrainZ = hiZ; }
#else
	void init(Int x, Int y) { m_cellX = x; m_cellY = y; }
#endif
	~PartitionCell();

	// --------------- inherited from Snapshot interface --------------
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	Int getCoiCount() const { return m_coiCount; }		///< return number of COIs touching this cell.
	Int getCellX() const { return m_cellX; }
	Int getCellY() const { return m_cellY; }

	void addLooker( Int playerIndex );
	void removeLooker( Int playerIndex );
	void addShrouder( Int playerIndex );
	void removeShrouder( Int playerIndex );
	CellShroudStatus getShroudStatusForPlayer( Int playerIndex ) const;

	// @todo: All of these are inline candidates
	UnsignedInt getThreatValue( Int playerIndex );
	void addThreatValue( Int playerIndex, UnsignedInt threatValue );
	void removeThreatValue( Int playerIndex, UnsignedInt threatValue );

	UnsignedInt getCashValue( Int playerIndex );
	void addCashValue( Int playerIndex, UnsignedInt cashValue );
	void removeCashValue( Int playerIndex, UnsignedInt cashValue );

	void invalidateShroudedStatusForAllCois(Int playerIndex);

#ifdef PM_CACHE_TERRAIN_HEIGHT
	Real getLoTerrain() const { return m_loTerrainZ; }
	Real getHiTerrain() const { return m_hiTerrainZ; }
#endif

	void getCellCenterPos(Real& x, Real& y);

	CellAndObjectIntersection *getFirstCoiInCell() { return m_firstCoiInCell; }


	#ifdef RTS_DEBUG
	void validateCoiList();
	#endif

	// intended only for CellAndObjectIntersection.
	void friend_addToCellList(CellAndObjectIntersection *coi);

	// intended only for CellAndObjectIntersection.
	void friend_removeFromCellList(CellAndObjectIntersection *coi);
};

//=====================================
/**
	A PartitionData is the part of an Object that understands
	how to maintain the Object in the space partitioning system.
*/
//=====================================
class PartitionData : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(PartitionData, "PartitionDataPool" )

private:

	enum DirtyStatus
	{
		NOT_DIRTY,
		NEED_COLLISION_CHECK,
		NEED_CELL_UPDATE_AND_COLLISION_CHECK
	};

	Object											*m_object;								///< Object this module is for
	GhostObject										*m_ghostObject;							///< Temporary object used when real object is gone but player thinks it's there because of fog.
	PartitionData								*m_next;									///< next module in master list
	PartitionData								*m_prev;									///< prev module in master list
	PartitionData								*m_nextDirty;
	PartitionData								*m_prevDirty;

	Int													m_coiArrayCount;					///< number of COIs allocated (may be more than are in use)
	Int													m_coiInUseCount;					///< number of COIs that are actually in use
	CellAndObjectIntersection		*m_coiArray;							///< The array of COIs
	Int64												m_doneFlag;							///< GeneralsX: 64-bit (gcoIterFlag never wraps; a skipped cell leaves old flags, Codex guardscan-adv-r1)
	Int													m_ownerTag;							///< GeneralsX: the owner tag its COIs are counted under in their cells
	Coord3D											m_fillPos;						///< GeneralsX: the Object's position when its cells were last filled (updateCellsTouched)
	DirtyStatus									m_dirtyStatus;
	ObjectShroudStatus					m_shroudedness[MAX_PLAYER_COUNT];
	ObjectShroudStatus					m_shroudednessPrevious[MAX_PLAYER_COUNT];	///<previous frames value of m_shroudedness
	Bool												m_everSeenByPlayer[MAX_PLAYER_COUNT];		///<whether this object has ever been seen by a given player.
	const PartitionCell					*m_lastCell;							///< The last cell I thought my center was in.

	/**
		Given a shape's geometry and size parameters, calculate the maximum number of COIs
		that the object could possibly occupy.
	*/
	Int calcMaxCoiForShape(GeometryType geom, Real majorRadius, Real minorRadius, Bool isSmall);

	/**
		Given an object's geometry and size parameters, calculate the maximum number of COIs
		that the object could possibly occupy. (This simply extracts the information from the object
		and calls calcMaxCoiForShape.)
	*/
	Int calcMaxCoiForObject();

	/**
		allocate the array of COIs (m_coiArray) for this module.
	*/
	void allocCoiArray();

	/**
		free the array of COIs (m_coiArray) for this module (if any).
	*/
	void freeCoiArray();

	/**
			marks self as touching no cells. (and any previous cells as not touching self!)
	*/
	void removeAllTouchedCells();

	/**
		this discards all current 'touch' information (via removeAllTouchedCells) and recalculates
		the cells touched by this module, based on the object's geometry. this will be called frequently and so
		needs to be as efficient as possible.
	*/
	void updateCellsTouched();

	/**
		If you imagine the array of Partition Cells as pixels, then this method
		'sets' the pixel [cell] at cell coordinate (x, y).
	*/
	void addSubPixToCoverage(PartitionCell *cell);

	/**
		fill in the pixels covered by the given 'small' shape with the given
		center and radius. 'small' shapes are special in that they
		are always assumed to cover at most 4 Cells, and so we can use
		a more efficient special-purpose filler, rather than a general
		rasterizer.
	*/
	void doSmallFill(
		Real centerX,
		Real centerY,
		Real radius
	);

	/// helper function for doCircleFill.
	void hLineCircle(Int x1, Int x2, Int y);

	/**
		fill in the pixels covered by the given circular shape with the given
		center and radius. Note that this is used for both spheres and cylinders.
	*/
	void doCircleFill(
		Real centerX,
		Real centerY,
		Real radius
	);

	/**
		A more advanced implementation of doCircleFill that is 100% accurate.
	*/
	void doCircleFillPrecise(Real centerX, Real centerY, Real radius);

	/**
		fill in the pixels covered by the given rectangular shape with the given
		center, dimensions, and rotation.
	*/
	void doRectFill(
		Real centerX,
		Real centerY,
		Real halfsizeX,
		Real halfsizeY,
		Real angle
	);

	/**
		do a careful test of the geometries of 'this' and 'that', and return
		true iff the geometries touch (or intersect). if true is returned,
		loc will be filled in to the collision location, and normal will be
		filled in to the normal to the surface of 'this' at the collide location.
	*/
	Bool collidesWith(const PartitionData *that, CollideLocAndNormal *cinfo) const;

public:

	PartitionData();

	void attachToObject( Object* object );
	void detachFromObject();
	void attachToGhostObject(GhostObject* object);
	void detachFromGhostObject();

	void setNext( PartitionData *next ) { m_next = next; }			///< set next pointer
	PartitionData *getNext() { return m_next; }						///< get the next pointer
	void setPrev( PartitionData *prev ) { m_prev = prev; }			///< set the prev pointer
	PartitionData *getPrev() { return m_prev; }						///< get the prev pointer

	/// mark the given module as being "dirty", needing recalcing during next update phase.
	// if needToUpdateCells is true, we'll recalc the partition cells it touches and do collision testing.
	// if needToUpdateCells is false, we'll just do the collision testing.
	void makeDirty(Bool needToUpdateCells);

	Bool isInNeedOfUpdatingCells() const { return m_dirtyStatus == NEED_CELL_UPDATE_AND_COLLISION_CHECK; }
	Bool isInNeedOfCollisionCheck() const { return m_dirtyStatus != NOT_DIRTY; }

	void invalidateShroudedStatusForPlayer(Int playerIndex);
	void invalidateShroudedStatusForAllPlayers();

	// GeneralsX @feature Claude 26/09/2026 Is any cell this object covers CLEAR for the player? Exactly the condition
	// under which getShroudedStatus answers CLEAR or PARTIAL_CLEAR, but a pure read: it touches neither m_shroudedness,
	// m_everSeenByPlayer nor the ghost snapshots, which simulation code reads back. For client questions (WAYPOINT ENTER).
	Bool isAnyCellClearForPlayer( Int playerIndex ) const;
	ObjectShroudStatus getShroudedStatus(Int playerIndex);
	/// GeneralsX @feature Claude 02/11/2026 rb-sense-2: TRUE iff one of the cells this object touches is marked in `mark` (a
	/// PartitionManager::markLookCells grid) and CLEAR for playerIndex - getShroudedStatus's CLEAR (any touched cell clear), limited to the
	/// marked cells. Read only.
	Bool touchesMarkedClearCell(Int playerIndex, const std::vector<UnsignedByte> &mark) const;
	// GeneralsX @bugfix Claude 02/10/2026 Is the object in the SHROUD for the player - off the map, or every cell it
	// covers never explored? The cells alone, a pure read: unlike getShroudedStatus it has no ghost-object, seen-before
	// or relationship refinement, so every peer answers alike. For logic that must not depend on the lazy cache.
	Bool isFullyShroudedForPlayer( Int playerIndex ) const;

	Int wasSeenByAnyPlayers() const	///<check if a player in the game has seen the object but is now looking at fogged version.
	{
		Int i=0;
		for (; i<MAX_PLAYER_COUNT; i++)
			if (m_everSeenByPlayer[i] && m_shroudedness[i] == OBJECTSHROUD_FOGGED)
				return i;
		return i;
	}


	Int getControllingPlayerIndex() const;

	/**
		enumerate the objects that share space with 'this'
		(ie, the objects in the same Partition Cells) and
		add 'em to the given contact list. also, if self
		is intersecting the ground, add it to the list as a possible
		collide-with-ground.
	*/
	void addPossibleCollisions(PartitionContactList *ctList);

	Object *getObject() { return m_object; }				///< return the Object that owns this module
	const Object *getObject() const { return m_object; }				///< return the Object that owns this module
	void friend_setObject(Object *object) { m_object = object;}	///< to be used only by the partition manager.
	GhostObject *getGhostObject() const { return m_ghostObject; }	///< return the ghost object that serves as fogged memory of object.
	void friend_setGhostObject(GhostObject *object) {m_ghostObject=object;}	///<used by ghost object manager to free link to partition data.
	void friend_setShroudednessPrevious(Int playerIndex,ObjectShroudStatus status); ///<only used to restore state after map border resizing and/or xfer!
	ObjectShroudStatus friend_getShroudednessPrevious(Int playerIndex) {return m_shroudednessPrevious[playerIndex];}

	void friend_removeAllTouchedCells() { removeAllTouchedCells(); }	///< this is only for use by PartitionManager
	void friend_updateCellsTouched()	{ updateCellsTouched(); } ///< this is only for use by PartitionManager
	Int friend_getCoiInUseCount() { return m_coiInUseCount; } ///< this is only for use by PartitionManager
	Bool friend_collidesWith(const PartitionData *that, CollideLocAndNormal *cinfo) const { return collidesWith(that, cinfo); }	///< this is only for use by PartitionContactList

	// these are only for use by getClosestObjects.
	// (note, if we ever use other bits in this, smarten this up...)
	Int64 friend_getDoneFlag() { return m_doneFlag; }
	void friend_setDoneFlag(Int64 i) { m_doneFlag = i; }
	/// GeneralsX @performance Claude 06/10/2026 the owner tag its COIs are counted under (PartitionManager::m_cellOwnerMask). The
	/// Object's controlling player changed: Object::setOrRestoreTeam and TeamPrototype call friend_updateOwnerTag to recount.
	Int friend_getOwnerTag() const { return m_ownerTag; }
	void friend_updateOwnerTag();
	/// GeneralsX: for visitObjectsNearToFar's stale-cell check (PartitionManager only)
	PartitionData *friend_getNextDirty() const { return m_nextDirty; }
	CellAndObjectIntersection *friend_getCoi(Int i) { return &m_coiArray[i]; }
	/// GeneralsX: Object::reactToTransformChange moved it by less than isPosDifferent notices, so its cells stay as they
	/// are: report how far it now is from where they were filled (PartitionManager::m_maxDrift)
	void friend_noteSubThresholdMove(const Coord3D *pos);

	Bool isInListDirtyModules(PartitionData* const* pListHead) const
	{
		Bool result = (*pListHead == this || m_prevDirty || m_nextDirty);
		DEBUG_ASSERTCRASH(result == (m_dirtyStatus != NOT_DIRTY), ("dirty flag mismatch"));
		return result;
	}
	void prependToDirtyModules(PartitionData** pListHead)
	{
		DEBUG_ASSERTCRASH((m_dirtyStatus != NOT_DIRTY), ("dirty flag mismatch"));
		m_nextDirty = *pListHead;
		if (*pListHead)
			(*pListHead)->m_prevDirty = this;
		*pListHead = this;
	}
	void removeFromDirtyModules(PartitionData** pListHead)
	{
		m_dirtyStatus = NOT_DIRTY;
		if (m_nextDirty)
			m_nextDirty->m_prevDirty = m_prevDirty;
		if (m_prevDirty)
			m_prevDirty->m_nextDirty = m_nextDirty;
		else
			*pListHead = m_nextDirty;
		m_prevDirty = 0;
		m_nextDirty = 0;
	}

};

//=====================================
/**
	this is an ABC. PartitionData::iterate allows you to pass multiple filters
	to filter out objects you don't want. it calls this method on the objects
	in question; any that get false returned, are not iterated. Note that
	either modMain or posMain will be null, but not both: the filter must be aware
	of this, and respond appropriately. (ie, the filter may be used for filtering
	against an object, or against a position.)
*/
class PartitionFilter
{
public:
	virtual Bool allow(Object *objOther) = 0;
	/**
		GeneralsX @performance Claude 06/10/2026 a superset of the owner tags (PartitionManager::m_cellOwnerMask) of every object
		allow() could pass. getClosestObjects asks only the FIRST filter in its list and skips any cell holding none of
		those tags: the walk it replaces would have rejected each of that cell's objects at this filter, calling no other.
		So a filter that returns less than PARTITION_OWNERS_ALL here must have an allow() without side effects.
	*/
	virtual PartitionOwnerMask getCellOwnerMask() { return PARTITION_OWNERS_ALL; }
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() = 0;
#endif
};

//=====================================
/**
	GeneralsX @performance Claude 30/09/2026 PartitionManager::visitObjectsNearToFar's visitor. visit() gets every object
	that passes the distance test and the filters, in exactly the order iterateObjectsInRange would have inserted them.
	After each ring of cells the walk asks whether it may stop: every object not visited yet has its centre farther than
	minCentreDist from the query position and a bounding circle radius of at most maxRadius (canStopAfter), and an
	object whose cells are stale this frame (it moved since the last PartitionManager::update) is asked about by its
	current centre distance (mayStillMatter); the walk then continues until that object's cells have been visited.
*/
class PartitionRingVisitor
{
public:
	virtual ~PartitionRingVisitor() { }
	virtual void visit(Object *obj, Real distSqr) = 0;
	virtual Bool canStopAfter(Real minCentreDist, Real maxRadius) = 0;
	virtual Bool mayStillMatter(const Object *obj, Real centreDist) = 0;
	virtual void stoppedEarly() { }	///< the walk really ended before its range (for audits)
};

//=====================================
/**
	Reject any objects that aren't currently flying.
*/
class PartitionFilterIsFlying : public PartitionFilter
{
public:
	PartitionFilterIsFlying() { }
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterIsFlying"; }
#endif
};

//=====================================
class PartitionFilterWouldCollide : public PartitionFilter
{
private:
	Coord3D m_position;
	GeometryInfo m_geom;
	Real m_angle;
  Bool m_desiredCollisionResult;  // collision must match this for allow to return true
public:
	PartitionFilterWouldCollide(const Coord3D& pos, const GeometryInfo& geom, Real angle, Bool desired);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterWouldCollide"; }
#endif
};

//=====================================
/**
	Reject any objects that aren't controlled by the same player.
*/
class PartitionFilterSamePlayer : public PartitionFilter
{
private:
	const Player *m_player;
public:
	PartitionFilterSamePlayer(const Player *player) : m_player(player) { }
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterSamePlayer"; }
#endif
};

//=====================================
/**
	Reject any objects that don't match the alliance
	affiliations compared with 'm_obj'. You
	may reject objects that are allied, neutral,
	or enemy with respect to it.
*/
class PartitionFilterRelationship : public PartitionFilter
{
private:
	const Object *m_obj;
	Int m_flags;
public:
	enum RelationshipAllowTypes
	{
		ALLOW_ALLIES					= (1<<ALLIES),		///< allow objects that m_obj considers allies
		ALLOW_ENEMIES					= (1<<ENEMIES),		///< allow objects that m_obj considers enemy
		ALLOW_NEUTRAL					= (1<<NEUTRAL),		///< allow objects that m_obj considers neutral
		SKIP_CELLS_BY_OWNER		= (1<<8)					///< GeneralsX: as the first filter, let getClosestObjects skip cells by owner (getCellOwnerMask)
	};
	PartitionFilterRelationship(const Object *obj, Int flags) : m_obj(obj), m_flags(flags) { }
	virtual Bool allow(Object *objOther) override;
	virtual PartitionOwnerMask getCellOwnerMask() override;
	/// GeneralsX: the owner tags of every object obj->getRelationship() can answer with one of the ALLOW_* bits in flags
	static PartitionOwnerMask cellOwnerMaskFor(const Object *obj, Int flags);
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRelationship"; }
#endif
};

//=====================================
/**
	Reject any objects that aren't on the specific
	team.
*/
class PartitionFilterAcceptOnTeam : public PartitionFilter
{
private:
	const Team *m_team;
public:
	PartitionFilterAcceptOnTeam(const Team *team);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterAcceptOnTeam"; }
#endif
};

//=====================================
/**
	Reject any objects that aren't on the specific
	squad.
*/
class PartitionFilterAcceptOnSquad : public PartitionFilter
{
private:
	const Squad *m_squad;
public:
	PartitionFilterAcceptOnSquad(const Squad *squad);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterAcceptOnSquad"; }
#endif
};

//=====================================
/**
	Reject any objects that are not within clear line-of-sight
	of a given object. "Line of sight" takes into account
	terrain (ie, no hills between 'em) but does not
	yet take into account structures. Note that this
	requires more computation that other filters, so
	this should generally put toward the end of the filter
	list so that simpler filters can reject other (simpler)
	cases earlier.
*/
class PartitionFilterLineOfSight : public PartitionFilter
{
private:
	const Object *m_obj;
public:
	PartitionFilterLineOfSight(const Object *obj);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterLineOfSight"; }
#endif
};

//=====================================
/**
	Only objects that Can Possibly be attacked by the given object
*/
class PartitionFilterPossibleToAttack : public PartitionFilter
{
private:
	const Object *m_obj;
	CommandSourceType m_commandSource;
	AbleToAttackType m_attackType;
public:
	PartitionFilterPossibleToAttack(AbleToAttackType t, const Object *obj, CommandSourceType commandSource);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPossibleToAttack"; }
#endif
};

//=====================================
/**
	Only objects that Can Possibly be entered by the given object
*/
class PartitionFilterPossibleToEnter : public PartitionFilter
{
private:
	const Object *m_obj;
	CommandSourceType m_commandSource;

public:
	PartitionFilterPossibleToEnter(const Object *obj, CommandSourceType commandSource);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPossibleToEnter"; }
#endif
};

//=====================================
/**
	Only objects that Can Possibly be hijacked by the given object
*/
class PartitionFilterPossibleToHijack : public PartitionFilter
{
private:
	const Object *m_obj;
	CommandSourceType m_commandSource;

public:
	PartitionFilterPossibleToHijack(const Object *obj, CommandSourceType commandSource);
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPossibleToHijack"; }
#endif
};

//=====================================
/**
 * Accept only the last object who attacked me. Very fast.
 */
class PartitionFilterLastAttackedBy : public PartitionFilter
{
private:
	ObjectID m_lastAttackedBy;
public:
	PartitionFilterLastAttackedBy(Object *obj);
	virtual Bool allow(Object *other) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterLastAttackedBy"; }
#endif
};

//=====================================
/**
	Only objects that match the given masks are accepted.
*/
class PartitionFilterAcceptByObjectStatus : public PartitionFilter
{
private:
	ObjectStatusMaskType m_mustBeSet, m_mustBeClear;
public:
	PartitionFilterAcceptByObjectStatus( ObjectStatusMaskType mustBeSet, ObjectStatusMaskType mustBeClear) : m_mustBeSet(mustBeSet), m_mustBeClear(mustBeClear) { }
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterAcceptByObjectStatus"; }
#endif
};

//=====================================
/**
	Just like PartitionFilterAcceptByObjectStatus, except that objects
	that match the given masks are REJECTED.
*/
class PartitionFilterRejectByObjectStatus : public PartitionFilter
{
private:
	ObjectStatusMaskType m_mustBeSet, m_mustBeClear;
public:
	PartitionFilterRejectByObjectStatus( ObjectStatusMaskType mustBeSet, ObjectStatusMaskType mustBeClear )
		: m_mustBeSet(mustBeSet), m_mustBeClear(mustBeClear)
	{
	}
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRejectByObjectStatus"; }
#endif
};

//=====================================
/**
	Objects that are stealthed and not detected or disguised are accepted or rejected based on allow bool.
*/
class PartitionFilterStealthedAndUndetected : public PartitionFilter
{
private:
	const Object *m_obj;
	Bool m_allow;
public:
	PartitionFilterStealthedAndUndetected( const Object *obj, Bool allow ) { m_obj = obj; m_allow = allow; }
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterStealthedAndUndetected"; }
#endif
};

//=====================================
/**
	Only objects that match the given masks are accepted.
*/
class PartitionFilterAcceptByKindOf : public PartitionFilter
{
private:
	KindOfMaskType m_mustBeSet, m_mustBeClear;
public:
	PartitionFilterAcceptByKindOf(const KindOfMaskType& mustBeSet, const KindOfMaskType& mustBeClear) : m_mustBeSet(mustBeSet), m_mustBeClear(mustBeClear) { }
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterAcceptByKindOf"; }
#endif
};

//=====================================
/**
	Just like PartitionFilterAcceptByKindOf, except that objects
	that match the given masks are REJECTED.
*/
class PartitionFilterRejectByKindOf : public PartitionFilter
{
private:
	KindOfMaskType m_mustBeSet, m_mustBeClear;
public:
	PartitionFilterRejectByKindOf(const KindOfMaskType& mustBeSet, const KindOfMaskType& mustBeClear)
		: m_mustBeSet(mustBeSet), m_mustBeClear(mustBeClear)
	{
	}
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRejectByKindOf"; }
#endif
};

//=====================================
/**
 * Reject any objects "behind" the given object.
 * This is a 3D check.
 */
class PartitionFilterRejectBehind: public PartitionFilter
{
private:
	Object *m_obj;
public:
	PartitionFilterRejectBehind( Object *obj );
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRejectBehind"; }
#endif
};

//=====================================
/**
 * Allow only living victims
 */
class PartitionFilterAlive : public PartitionFilter
{
public:
	PartitionFilterAlive() { }
protected:
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterAlive"; }
#endif
};

//=====================================
/**
 * If obj is on the map, reject all off-map objects.
 * If obj is off the map, reject all on-map objects.
 */
class PartitionFilterSameMapStatus : public PartitionFilter
{
private:
	const Object *m_obj;
public:
	PartitionFilterSameMapStatus(const Object *obj) : m_obj(obj) { }
protected:
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterSameMapStatus"; }
#endif
};

//=====================================
/**
 * If obj is on the map, accept it.
 */
class PartitionFilterOnMap : public PartitionFilter
{
public:
	PartitionFilterOnMap() { }
protected:
	virtual Bool allow(Object *objOther) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterOnMap"; }
#endif
};

//=====================================
/**
 * Reject buildings, unless they can attack, or
 * we are the computer-controlled AI and the building
 * is owned by the enemy.
 */
class PartitionFilterRejectBuildings : public PartitionFilter
{
private:
	const Object *m_self;
	Bool m_acquireEnemies;
public:
	PartitionFilterRejectBuildings(const Object *o);
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRejectBuildings"; }
#endif
};

//=====================================
/**
 * Accept/Reject Insignificant buildings
 * Note: This will allow things that
 */
class PartitionFilterInsignificantBuildings : public PartitionFilter
{
private:
	Bool m_allowNonBuildings;
	Bool m_allowInsignificant;
public:
	PartitionFilterInsignificantBuildings(Bool allowNonBuildings, Bool allowInsignificant) :
			m_allowNonBuildings(allowNonBuildings), m_allowInsignificant(allowInsignificant) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterInsignificantBuildings"; }
#endif
};

//=====================================
/**
 * Accept if they are clear, not fogged or shrouded
 */
class PartitionFilterFreeOfFog : public PartitionFilter
{
private:
	Int m_comparisonIndex;
public:
	PartitionFilterFreeOfFog(Int toWhom) :
			m_comparisonIndex(toWhom){}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterFreeOfFog"; }
#endif
};

//=====================================
/**
 * Accept repulsor (enemies, or flagged as repulsor).
 */
class PartitionFilterRepulsor : public PartitionFilter
{
private:
	const Object *m_self;
public:
	PartitionFilterRepulsor(const Object *o) : m_self(o) { }
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterRepulsor"; }
#endif
};

//=====================================
/**
 * Reject all objects outside of the irregularly shaped area defined by the set of points
 * passed in at creation. This is done using the even-odd rule against the points, assuming
 * that the area is closed.
 */
class PartitionFilterIrregularArea : public PartitionFilter
{
private:
	Coord3D *m_area;
	Int m_numPointsInArea;

public:
	PartitionFilterIrregularArea(Coord3D* area, Int numPointsInArea) : m_area(area), m_numPointsInArea(numPointsInArea) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterIrregularArea"; }
#endif
};

//=====================================
/**
 * Reject all objects inside the irregularly trigger area
 * passed in at creation. This is done using the even-odd rule against the points, assuming
 * that the area is closed.
 */
class PartitionFilterPolygonTrigger : public PartitionFilter
{
private:
	const PolygonTrigger *m_trigger;

public:
	PartitionFilterPolygonTrigger(const PolygonTrigger *trigger) : m_trigger(trigger) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPolygonTrigger"; }
#endif
};

//=====================================
/**
 * Reject all objects that aren't (or are) the player
 * passed in at creation. .
 */
class PartitionFilterPlayer : public PartitionFilter
{
private:
	const Player *m_player;
	Bool  m_match;

public:
	PartitionFilterPlayer(const Player *player, Bool match) : m_player(player), m_match(match) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPlayer"; }
#endif
};

//=====================================
/**
 * Allow or reject (based on match) all Objects whose affiliation matches one of those
 * specified by
 */
class PartitionFilterPlayerAffiliation : public PartitionFilter
{
private:
	const Player *m_player;
	Bool  m_match;
	UnsignedInt m_affiliation;

public:
	// whichAffiliation should use AllowPlayerRelationship flags specified in PlayerList.h
	PartitionFilterPlayerAffiliation(const Player *player, UnsignedInt whichAffiliation, Bool match)
		: m_player(player), m_affiliation(whichAffiliation), m_match(match)
	{
	}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterPlayerAffiliation"; }
#endif
};

//=====================================
/**
 * Accept all objects that aren't (or are) the thing
 * passed in at creation. .
 */
class PartitionFilterThing : public PartitionFilter
{
private:
	const ThingTemplate *m_tThing;
	Bool  m_match;

public:
	PartitionFilterThing(const ThingTemplate *thing, Bool match) : m_tThing(thing), m_match(match) {
		DEBUG_ASSERTCRASH(m_tThing != nullptr, ("ThingTemplate for PartitionFilterThing is null"));
	}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterThing"; }
#endif
};

//=====================================
/**
 * Accept all objects that can/cannot be garrisoned by anyone.
 */
class PartitionFilterGarrisonable : public PartitionFilter
{
private:
	Player *m_player;
	Bool  m_match;

public:
	PartitionFilterGarrisonable( Bool match ) : m_match(match)
	{
		m_player = nullptr;
	}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterGarrisonable"; }
#endif
};

//=====================================
/**
 * Accept all objects that can/cannot be garrisoned by source *Player*.
 */
class PartitionFilterGarrisonableByPlayer : public PartitionFilter
{
private:
	Player *m_player;
	Bool  m_match;
	CommandSourceType m_commandSource;

public:
	PartitionFilterGarrisonableByPlayer( Player *player, Bool match, CommandSourceType commandSource ):
			m_player(player), m_match(match), m_commandSource(commandSource)
	{
	}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterGarrisonableByPlayer"; }
#endif
};

//=====================================
/**
 * Accept all objects that are/n't unmanned.
 */
class PartitionFilterUnmannedObject : public PartitionFilter
{
private:
	Bool  m_match;

public:
	PartitionFilterUnmannedObject( Bool match ) : m_match(match) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterUnmannedObject"; }
#endif
};

//=====================================
/**
 * Accept all objects that can/cannot have object X perform a command ability on them
 */
class PartitionFilterValidCommandButtonTarget : public PartitionFilter
{
private:
	Object *m_source;
	const CommandButton *m_commandButton;
	Bool m_match;
	CommandSourceType m_commandSource;

public:
	PartitionFilterValidCommandButtonTarget( Object *source, const CommandButton *commandButton, Bool match, CommandSourceType commandSource) :
		m_source(source), m_commandButton(commandButton), m_match(match), m_commandSource(commandSource) {}
protected:
	virtual Bool allow( Object *other ) override;
#if defined(RTS_DEBUG)
	virtual const char* debugGetName() override { return "PartitionFilterValidCommandButtonTarget"; }
#endif
};

//=====================================
/**
	PartitionManager is the singleton class that manages the entire partition/collision
	system. It maintains the set of PartitionCells that correspond to the world system,
	and updates the PartitionDatas as needed during update phase.
*/
class PartitionManager : public SubsystemInterface, public Snapshot
{

private:

#ifdef FASTER_GCO
	typedef std::vector<ICoord2D>		OffsetVec;
	typedef std::vector<OffsetVec>	RadiusVec;
#endif

	PartitionData		*m_moduleList;		///< master partition module list

	Region3D				m_worldExtents;		///< should be same as TheTerrainLogic->getExtents()
	Real						m_cellSize;				///< edge size of each cell, in world coord space
	Real						m_cellSizeInv;		///< 1/cellSize (used for efficiency)
	Int							m_cellCountX;			///< number of cells, x
	Int							m_cellCountY;			///< number of cells, y
	Int							m_totalCellCount;	///< x * y
	PartitionCell*	m_cells;					///< array of cells
	// GeneralsX @performance Claude 06/10/2026 per cell (same index as m_cells): the owner tags of the objects whose COIs are
	// in it (bit 1 << tag, see PARTITION_OWNER_NONE), and per cell and tag how many COIs carry it (32-bit: 16 wrapped at
	// 65,536 stacked objects, Codex guardscan-adv-r1). Beside the cells, not in PartitionCell: a guard scan reads one mask
	// for each of ~14,000 cells, so a dense array is what makes skipping a cell cheap, and PartitionCell keeps its size for
	// the shroud and everything else that walks cells (fps-profile-2: counts inside the cell cost more than they saved).
	PartitionOwnerMask*		m_cellOwnerMask;
	UnsignedInt*		m_cellOwnerCount;	///< [cell index * PARTITION_OWNER_TAGS + tag]
	PartitionData*	m_dirtyModules;
	Bool						m_updatedSinceLastReset;	///< Used to force a return of OBJECTSHROUD_INVALID before update has been called.

	std::queue<SightingInfo *> m_pendingUndoShroudReveals;	///< Anything can queue up an Undo to happen later. This is a queue, because "later" is a constant

#ifdef FASTER_GCO
	Int							m_maxGcoRadius;
	RadiusVec				m_radiusVec;
	// GeneralsX @performance Claude 07/11/2026 per cell offset (dx + m_cellCountX - 1, dy + m_cellCountY - 1; row-major,
	// 2 * m_cellCountX - 1 wide): where getClosestObjects' ring walk meets that offset (its index in m_radiusVec read ring by
	// ring), or -1 when no ring holds it. iterateObjectsInRangePure orders its objects by it.
	std::vector<Int>	m_walkRank;
#endif
	// GeneralsX @performance Claude 30/09/2026 the largest bounding circle radius any Object has had when its cells were
	// filled (PartitionData::updateCellsTouched) since the last reset. Only ever grows, so it bounds every object in the
	// partition (visitObjectsNearToFar's early stop).
	Real						m_maxObjectRadius;
	// GeneralsX @performance Claude 30/09/2026 the farthest any Object has been from the position its cells were filled at
	// without being marked dirty (isPosDifferent ignores moves of <= 0.005 per call, and those add up), since the last
	// reset. Only ever grows. visitObjectsNearToFar's early stop widens its bound by it (Codex sc-hunt-r2).
	Real						m_maxDrift;

protected:

	/**
		This is an internal function that is used to implement the public
		getClosestObject and iterateObjects calls.
	*/
	Object *getClosestObjects(
		const Object *obj,
		const Coord3D *pos,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters,
		SimpleObjectIterator *iter,	// if nonnull, append ALL satisfactory objects to the iterator (not just the single closest)
		Real *closestDistArg,
		Coord3D *closestVecArg
	);

	void shutdown();

	/// used to validate the positions for findPositionAround family of methods
	Bool tryPosition( const Coord3D *center, Real dist, Real angle,
										const FindPositionOptions *options, Coord3D *result );

	typedef Int (*CellAlongLineProc)(PartitionCell* cell, void* userData);

	Int iterateCellsAlongLine(const Coord3D& pos, const Coord3D& posOther, CellAlongLineProc proc, void* userData);

	// note iterateCellsBreadthFirst returns the cell index that made the CellBreadthFirstProc return
	// non-Zero.
	typedef Int (*CellBreadthFirstProc)(PartitionCell* cell, void* userData);
	Int iterateCellsBreadthFirst(const Coord3D *pos, CellBreadthFirstProc proc, void *userData);

#ifdef FASTER_GCO
	Int calcMinRadius(const ICoord2D& cur);
	void calcRadiusVec();
	/// GeneralsX: iterateObjectsInRangePure's object-list walk; false (iter untouched) when maxDist stops short of a ring
	Bool collectInWalkOrder(const Object *obj, Real maxDist, DistanceCalculationType dc, PartitionFilter **filters,
		SimpleObjectIterator *iter);
#endif

	// These are all friend functions now. They will continue to function as before, but can be passed into
	// the DiscreteCircle::drawCircle function.
	friend void hLineAddLooker(Int x1, Int x2, Int y, void *playerIndex);
	friend void hLineRemoveLooker(Int x1, Int x2, Int y, void *playerIndex);
	friend void hLineAddShrouder(Int x1, Int x2, Int y, void *playerIndex);
	friend void hLineRemoveShrouder(Int x1, Int x2, Int y, void *playerIndex);

	friend void hLineAddThreat(Int x1, Int x2, Int y, void *threatValueParms);
	friend void hLineRemoveThreat(Int x1, Int x2, Int y, void *threatValueParms);
	friend void hLineAddValue(Int x1, Int x2, Int y, void *threatValueParms);
	friend void hLineRemoveValue(Int x1, Int x2, Int y, void *threatValueParms);

	void processPendingUndoShroudRevealQueue(Bool considerTimestamp = TRUE);				///< keep popping and processing until you get to one that is in the future
	void resetPendingUndoShroudRevealQueue();					///< Just delete everything in the queue without doing anything with them

public:

	PartitionManager();
	virtual ~PartitionManager() override;

	// --------------- inherited from Subsystem interface -------------
	virtual void init() override;			///< initialize
	virtual void reset() override;			///< system reset
	virtual void update() override;		///< system update
	// ----------------------------------------------------------------

	// --------------- inherited from Snapshot interface --------------
	virtual void crc( Xfer *xfer ) override;
	virtual void xfer( Xfer *xfer ) override;
	virtual void loadPostProcess() override;

	Bool getUpdatedSinceLastReset() const { return m_updatedSinceLastReset; }

	void registerObject( Object *object );				///< add thing to system
	void unRegisterObject( Object *object );			///< remove thing from system
	void registerGhostObject( GhostObject* object);	///<recreate partition data needed to hold object (only used to restore after PM reset).
	void unRegisterGhostObject (GhostObject *object);	///< release partition data held for ghost object.

	void processEntirePendingUndoShroudRevealQueue(); ///< process every pending one regardless of timestamp

	/// return the number of PartitionCells in the x-dimension.
	Int getCellCountX() { DEBUG_ASSERTCRASH(m_cellCountX != 0, ("partition not inited")); return m_cellCountX; }

	/// return the number of PartitionCells in the y-dimension.
	Int getCellCountY() { DEBUG_ASSERTCRASH(m_cellCountY != 0, ("partition not inited")); return m_cellCountY; }

	/// return the PartitionCell located at cell coordinates (x,y).
	PartitionCell *getCellAt(Int x, Int y);
	const PartitionCell *getCellAt(Int x, Int y) const;

	/// A convenience function to reveal shroud at some location
	// Queuing does not give you control of the timestamp to enforce the queue.  I own the delay, you don't.
	void doShroudReveal( Real centerX, Real centerY, Real radius, PlayerMaskType playerMask);
	/// GeneralsX @feature Claude 02/11/2026 rb-sense-2: mark 1 in `mark` (m_cellCountX * m_cellCountY bytes, row-major; resized and cleared
	/// when its size differs) every cell doShroudReveal(look.m_where, look.m_howFar) adds a looker to - the same centre, radius, cap and
	/// circle. Nothing for an invalid look. Touches no cell (the AI rule bank's own-sight test).
	void markLookCells(const SightingInfo &look, std::vector<UnsignedByte> &mark) const;
	void undoShroudReveal( Real centerX, Real centerY, Real radius, PlayerMaskType playerMask);
	void queueUndoShroudReveal( Real centerX, Real centerY, Real radius, PlayerMaskType playerMask );

	void doShroudCover( Real centerX, Real centerY, Real radius, PlayerMaskType playerMask);
	void undoShroudCover( Real centerX, Real centerY, Real radius, PlayerMaskType playerMask);

	/// Perform threat map and value map updates.
	void doThreatAffect( Real centerX, Real centerY, Real radius, UnsignedInt threatVal, PlayerMaskType playerMask);
	void undoThreatAffect( Real centerX, Real centerY, Real radius, UnsignedInt threatVal, PlayerMaskType playerMask);
	void doValueAffect( Real centerX, Real centerY, Real radius, UnsignedInt valueVal, PlayerMaskType playerMask);
	void undoValueAffect( Real centerX, Real centerY, Real radius, UnsignedInt valueVal, PlayerMaskType playerMask);

	void getCellCenterPos(Int x, Int y, Real& xx, Real& yy);

	// find the cell that covers the world coords (wx,wy) and return its coords.
	void worldToCell(Real wx, Real wy, Int *cx, Int *cy) const;

	// given a distance in world coords, return the number of cells needed to cover that distance (rounding up)
	Int worldToCellDist(Real w);

	Object *getClosestObject(
		const Object *obj,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters = nullptr,
		Real *closestDist = nullptr,
		Coord3D *closestDistVec = nullptr
	);

	/// GeneralsX: every object iterateObjectsInRange(obj, maxDist, dc, filters) would return, handed to the visitor in the
	/// iterator's insertion order (not sorted), ring by ring, stopping early once the visitor says nothing else can matter.
	void visitObjectsNearToFar(const Object *obj, Real maxDist, DistanceCalculationType dc, PartitionFilter **filters,
		PartitionRingVisitor *visitor);
	/// GeneralsX: PartitionData::updateCellsTouched reports each object's bounding radius (see m_maxObjectRadius)
	void friend_noteObjectRadius(Real r) { if (r > m_maxObjectRadius) m_maxObjectRadius = r; }
	void friend_noteDrift(Real d) { if (d > m_maxDrift) m_maxDrift = d; }	///< see m_maxDrift
	Object *getClosestObject(
		const Coord3D *pos,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters = nullptr,
		Real *closestDist = nullptr,
		Coord3D *closestDistVec = nullptr
	);

	Real getRelativeAngle2D( const Object *obj, const Object *otherObj );
	Real getRelativeAngle2D( const Object *obj, const Coord3D *pos );

	void getVectorTo(const Object *obj, const Object *otherObj, DistanceCalculationType dc, Coord3D& vec);
	void getVectorTo(const Object *obj, const Coord3D *pos, DistanceCalculationType dc, Coord3D& vec);

	// just like 'getDistance', but return the dist-sqr, meaning we save a sqrt() call if you don't need it.
	Real getDistanceSquared(const Object *obj, const Object *otherObj, DistanceCalculationType dc, Coord3D *vec = nullptr);
	Real getDistanceSquared(const Object *obj, const Coord3D *pos, DistanceCalculationType dc, Coord3D *vec = nullptr);

	// just like 'getDistanceSquared', but return the dist-sqr where the obj is at goalPos.
	Real getGoalDistanceSquared(const Object *obj, const Coord3D *goalPos, const Object *otherObj, DistanceCalculationType dc, Coord3D *vec = nullptr);
	Real getGoalDistanceSquared(const Object *obj, const Coord3D *goalPos, const Coord3D *otherPos, DistanceCalculationType dc, Coord3D *vec = nullptr);

#ifdef PM_CACHE_TERRAIN_HEIGHT
	// note that the 2d positions aren't guaranteed to be the actual spot within the cell where the terrain
	// is lowest or highest.... just the center of the relevant cell. this function is used for rough-n-quick
	// estimates only.
	Bool estimateTerrainExtremesAlongLine(const Coord3D& startWorld, const Coord3D& endWorld, Real* minZ, Real* maxZ, Coord2D* minZPos, Coord2D* maxZPos);
#endif

#ifdef DUMP_PERF_STATS
	void getPMStats(double& gcoTimeThisFrameTotal, double& gcoTimeThisFrameAvg);
#endif

	SimpleObjectIterator *iterateObjectsInRange(
		const Object *obj,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters = nullptr,
		IterOrderType order = ITER_FASTEST
	);

	SimpleObjectIterator *iterateObjectsInRange(
		const Coord3D *pos,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters = nullptr,
		IterOrderType order = ITER_FASTEST
	);

	/**
		GeneralsX @performance Claude 07/11/2026 the iterator iterateObjectsInRange(obj, ...) returns, same objects in the same
		order. When maxDist reaches every ring (REALLY_FAR, HUGE_DIST), it walks the object list instead of every cell offset
		(4x the map's cells, per call): each object goes where the ring walk would have met it first. The filters are then
		called in object-list order, so every filter in the list must have an allow() without side effects.
	*/
	SimpleObjectIterator *iterateObjectsInRangePure(
		const Object *obj,
		Real maxDist,
		DistanceCalculationType dc,
		PartitionFilter **filters,
		IterOrderType order
	);

	SimpleObjectIterator *iterateAllObjects(PartitionFilter **filters = nullptr);

	/**
		return the Objects that would (or would not) collide with the given
		geometry.
	*/
	SimpleObjectIterator* iteratePotentialCollisions(
		const Coord3D* pos,
		const GeometryInfo& geom,
		Real angle,
		Bool use2D = false
	);

	Bool isColliding( const Object *a, const Object *b ) const;

	/// Checks a geometry against an arbitrary geometry.
	Bool geomCollidesWithGeom( const Coord3D* pos1,
							const GeometryInfo& geom1,
							Real angle1,
							const Coord3D* pos2,
							const GeometryInfo& geom2,
							Real angle2
  ) const;

	/// finding legal positions in the world
	Bool findPositionAround( const Coord3D *center,
													 const FindPositionOptions *options,
													 Coord3D *result );

	/// return the size of a PartitionCell, in world coords.
	Real getCellSize() { return m_cellSize; }				// only for the use of PartitionData!

	/// return (1.0 / getCellSize); this is used frequently, so we cache it for efficiency
	Real getCellSizeInv() { return m_cellSizeInv; }

	/**
		return true iff there is clear line-of-sight between the two positions.
		this only takes terrain into account; it does not consider objects, units,
		trees, buildings, etc.
	*/
	Bool isClearLineOfSightTerrain(const Object* obj, const Coord3D& objPos, const Object* other, const Coord3D& otherPos);

	/// GeneralsX: intended only for CellAndObjectIntersection and PartitionData - a COI with this owner tag joined / left the cell
	void friend_addCellOwner(const PartitionCell *cell, Int tag);
	void friend_removeCellOwner(const PartitionCell *cell, Int tag);

	Bool isInListDirtyModules(PartitionData* o) const
	{
		return o->isInListDirtyModules(&m_dirtyModules);
	}
	void prependToDirtyModules(PartitionData* o)
	{
		o->prependToDirtyModules(&m_dirtyModules);
	}
	void removeFromDirtyModules(PartitionData* o)
	{
		o->removeFromDirtyModules(&m_dirtyModules);
	}
	void removeAllDirtyModules()
	{
		while (m_dirtyModules)
		{
			PartitionData *tmp = m_dirtyModules;
			removeFromDirtyModules(tmp);
		}
	}

	/**
		virtual Reveals the map for the given player, but does not override Shroud generation.  (Script)
		*/
	void revealMapForPlayer( Int playerIndex );

	/**
		Reveals the map for the given player, AND permanently disables all Shroud generation (Observer Mode).
		*/
	void revealMapForPlayerPermanently( Int playerIndex );

	/**
		Adds a layer of permanent blindness.  Used solely to undo the permanent reveal for debugging
		*/
	void undoRevealMapForPlayerPermanently( Int playerIndex );

	/**
		Resets the shroud for the given player with passive shroud (can re-explore).
		*/
	void shroudMapForPlayer( Int playerIndex );

	/** this doesn't change the actual shroud values in logic, just pushes them
		back out to the display... this should generally only be used when the local
		player changes.
		*/
	void refreshShroudForLocalPlayer();

	/**
		Shrouded has no absolute meaning.  It only makes sense to say "Shrouded for him".
	*/
	CellShroudStatus getShroudStatusForPlayer( Int playerIndex, Int x, Int y ) const;
	CellShroudStatus getShroudStatusForPlayer( Int playerIndex, const Coord3D *loc ) const;

	ObjectShroudStatus getPropShroudStatusForPlayer(Int playerIndex, const Coord3D *loc ) const;

	Real getGroundOrStructureHeight(Real posx, Real posy);

	void getMostValuableLocation( Int playerIndex, UnsignedInt whichPlayerTypes, ValueOrThreat valType, Coord3D *outLocation );
	Bool getNearestGroupWithValue( Int playerIndex, UnsignedInt whichPlayerTypes, ValueOrThreat valType, const Coord3D *sourceLocation,
																 Int valueRequired, Bool greaterThan, Coord3D *outLocation );

	// If saveToFog is true, then we are writing STORE_FOG.
	// If saveToFog is false, then we are writing STORE_PERMENANT_REVEAL
	void storeFoggedCells(ShroudStatusStoreRestore &outPartitionStore, Bool storeToFog) const;
	void restoreFoggedCells(const ShroudStatusStoreRestore &inPartitionStore, Bool restoreToFog);
};

// -----------------------------------------------------------------------------
inline void PartitionManager::worldToCell(Real wx, Real wy, Int *cx, Int *cy) const
{
	*cx = REAL_TO_INT_FLOOR((wx - m_worldExtents.lo.x) * m_cellSizeInv);
	*cy = REAL_TO_INT_FLOOR((wy - m_worldExtents.lo.y) * m_cellSizeInv);
}

//-----------------------------------------------------------------------------
inline Int PartitionManager::worldToCellDist(Real w)
{
	return REAL_TO_INT_CEIL(w  * m_cellSizeInv);
}

//-----------------------------------------------------------------------------
inline PartitionCell *PartitionManager::getCellAt(Int x, Int y)
{
	return (x < 0 || y < 0 || x >= m_cellCountX || y >= m_cellCountY) ? nullptr : &m_cells[y * m_cellCountX + x];
}

//-----------------------------------------------------------------------------
inline const PartitionCell *PartitionManager::getCellAt(Int x, Int y) const
{
	return (x < 0 || y < 0 || x >= m_cellCountX || y >= m_cellCountY) ? nullptr : &m_cells[y * m_cellCountX + x];
}

//-----------------------------------------------------------------------------

#ifdef FASTER_GCO
// nothing
#else
class CellOutwardIterator
{
private:
	PartitionManager	*m_mgr;
	Int								m_cellCenterX, m_cellCenterY;
	Int								m_maxRadius;
	Int								m_cellRadius;
	Int								m_delta[2];
	Int								m_inc;
	Int								m_cnt;
	Int								m_axis;
	Int								m_iter;
	PartitionCell *nextCell(Bool skipEmpties);
public:
	CellOutwardIterator(PartitionManager *mgr, Int x, Int y);
	~CellOutwardIterator();
	PartitionCell *next() { return nextCell(false); }
	PartitionCell *nextNonEmpty() { return nextCell(true); }
	Int getCurCellRadius() const { return m_cellRadius; }
	Int getMaxRadius() const { return m_maxRadius; }
	void setMaxRadius(Int max) { m_maxRadius = max; }
};
#endif

//-----------------------------------------------------------------------------
//           Inlining
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//           Externals
//-----------------------------------------------------------------------------
extern PartitionManager *ThePartitionManager;  ///< object manager singleton
