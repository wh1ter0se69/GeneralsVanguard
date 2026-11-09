/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX @feature Claude 23/09/2026 THE LOBBY-AGREED STRESS BATTLEFIELD - the parts that must be
// bit-identical on every peer, with no engine in them so scripts/test/stress-option-test.cpp can
// exercise them standalone.
//
// WHY A LOBBY OPTION. GX_STRESS used to be a per-process env var read on the client side, which
// spawned through MSG_GX_CHEAT_SPAWN_UNIT from whichever peer had it set. With two humans in the
// match GameLogic::gxCheatsAllowed refuses every cheat on every peer, so a real Windows + Mac match
// could not be loaded at all. Owner decision C, 23/09/2026: the HOST's settings travel with the
// game options ("ST=" in GameInfoToAsciiString), every peer parses the same value, and the
// simulation applies it on the same logic frame from synchronised data only. Joiners never read
// their own environment for it.
//
// WHAT IT DOES at logic frame `frame`, for every playable player in player-index order:
//   * `units` of template `unitId`, either on a ring around the centroid of the map's objects
//     (place 'R' - the old GX_STRESS recipe, armies in contact) or at a staging point in front of
//     the player's own base (place 'B').
//   * `buildings` structures, cycling through `buildingIds`, dotted around the player's own base
//     on ONE global lattice shared by every player (gxStressPlanBuildings). The lattice pitch is
//     the widest footprint plus a lane, and a cell is used only where one whole lane of open ground
//     surrounds the footprint, so no building can close a gap - between two stress buildings, or
//     in the terrain, or beside a structure that was already there.
//   * orders: attack-move toward the nearest enemy base ('E') or the map centre ('C'), re-issued to
//     idle stress units every `every` frames. 'N' issues nothing (the old recipe).
//
// A PEER THAT DOES NOT KNOW THE OPTION IS REFUSED, twice over. Its ParseAsciiStringToGameInfo ends
// its key chain with `else { optionsOk = false; break; }`, so "ST=" makes it drop the whole
// announce - it never sees the lobby and never sends a join. And every file this touches is inside
// the sim digest, so its sourceID differs and the host's SimIdCompare denies the join anyway - unless
// either build reports sourceID 0 (no provenance), which SimIdCompare never compares; then only the
// parser refuses.
// scripts/test/stress-option-refusal-test.sh checks both properties against the tree.

#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

enum
{
	GX_STRESS_MAX_UNITS          = 200,			///< per player. 12 x 200 = 2400 units in one frame is already a hitch
	GX_STRESS_MAX_BUILDINGS      = 400,			///< per player
	GX_STRESS_MAX_BUILDING_TYPES = 8,
	GX_STRESS_MAX_FRAME          = 1000000,
	GX_STRESS_MAX_RADIUS         = 10000,
	GX_STRESS_MAX_EVERY          = 100000,
	GX_STRESS_WIRE_MAX           = 80,			///< buffer for the widest value (77 chars) plus its NUL
	GX_STRESS_ANNOUNCE_BYTES     = 81			///< "ST=" + widest value + ';' - reserved in LANAPI.h's budget
};

struct GXStressOption
{
	int  units;													///< per playable player; 0 = none
	int  unitId;												///< ThingTemplate id (identical data is already enforced by dataID)
	int  frame;													///< logic frame the spawn happens on
	char place;													///< 'R' ring at the map's centroid, 'B' in front of each player's base
	int  radius;												///< ring radius for 'R'
	char order;													///< 'N' none, 'E' nearest enemy base, 'C' map centre
	int  every;													///< re-issue interval, frames
	int  buildings;											///< per playable player; 0 = none
	int  numBuildingIds;
	int  buildingIds[GX_STRESS_MAX_BUILDING_TYPES];
};

inline void gxStressClear( GXStressOption *o ) { memset( o, 0, sizeof(*o) ); }
inline bool gxStressIsOn( const GXStressOption &o ) { return o.units > 0 || o.buildings > 0; }

inline bool gxStressEqual( const GXStressOption &a, const GXStressOption &b )
{
	if( a.units != b.units || a.unitId != b.unitId || a.frame != b.frame || a.place != b.place
			|| a.radius != b.radius || a.order != b.order || a.every != b.every
			|| a.buildings != b.buildings || a.numBuildingIds != b.numBuildingIds )
		return false;
	for( int i = 0; i < a.numBuildingIds && i < GX_STRESS_MAX_BUILDING_TYPES; ++i )
		if( a.buildingIds[i] != b.buildingIds[i] )
			return false;
	return true;
}

/// The one range check, shared by the writer (the host refuses to publish anything else) and the
/// reader (a remote host is untrusted: an out-of-range value rejects the whole options string).
inline bool gxStressValid( const GXStressOption &o )
{
	if( o.units < 0 || o.units > GX_STRESS_MAX_UNITS || o.buildings < 0 || o.buildings > GX_STRESS_MAX_BUILDINGS )
		return false;
	if( !gxStressIsOn( o ) )
		return false;
	if( o.unitId < 0 || o.unitId > 0xFFFF || ( o.units > 0 && o.unitId == 0 ) )
		return false;
	if( o.frame < 1 || o.frame > GX_STRESS_MAX_FRAME || o.radius < 1 || o.radius > GX_STRESS_MAX_RADIUS
			|| o.every < 1 || o.every > GX_STRESS_MAX_EVERY )
		return false;
	if( o.place != 'R' && o.place != 'B' )
		return false;
	if( o.order != 'N' && o.order != 'E' && o.order != 'C' )
		return false;
	if( o.buildings == 0 )
		return o.numBuildingIds == 0;
	if( o.numBuildingIds < 1 || o.numBuildingIds > GX_STRESS_MAX_BUILDING_TYPES )
		return false;
	for( int i = 0; i < o.numBuildingIds; ++i )
		if( o.buildingIds[i] < 1 || o.buildingIds[i] > 0xFFFF )
			return false;
	return true;
}

/// Writes the VALUE of the ST field ("units,unitId,frame,place,radius,order,every,buildings,ids"),
/// ids ':'-separated or "0" for none. Returns its length, or -1 (nothing usable written) when the
/// option is invalid or the buffer too small - never a truncated value.
inline int gxStressFormat( const GXStressOption &o, char *buf, size_t size )
{
	if( buf == nullptr || size == 0 || !gxStressValid( o ) )
		return -1;
	char tmp[128];
	int len = snprintf( tmp, sizeof(tmp), "%d,%X,%d,%c,%d,%c,%d,%d,", o.units, (unsigned)o.unitId, o.frame,
		o.place, o.radius, o.order, o.every, o.buildings );
	if( o.buildings == 0 )
		len += snprintf( tmp + len, sizeof(tmp) - len, "0" );
	for( int i = 0; i < o.numBuildingIds; ++i )
		len += snprintf( tmp + len, sizeof(tmp) - len, i == 0 ? "%X" : ":%X", (unsigned)o.buildingIds[i] );
	if( len <= 0 || (size_t)len >= size )
		return -1;
	memcpy( buf, tmp, (size_t)len + 1 );
	return len;
}

inline bool gxStressParseDec( const char *s, size_t len, int *out )
{
	if( len == 0 || len > 9 )		// 9 digits cannot overflow an int
		return false;
	int v = 0;
	for( size_t i = 0; i < len; ++i )
	{
		if( s[i] < '0' || s[i] > '9' )
			return false;
		v = v * 10 + ( s[i] - '0' );
	}
	*out = v;
	return true;
}

inline bool gxStressParseHex( const char *s, size_t len, int *out )
{
	if( len == 0 || len > 4 )
		return false;
	int v = 0;
	for( size_t i = 0; i < len; ++i )
	{
		const char c = s[i];
		int d;
		if( c >= '0' && c <= '9' )      d = c - '0';
		else if( c >= 'A' && c <= 'F' ) d = c - 'A' + 10;
		else if( c >= 'a' && c <= 'f' ) d = c - 'a' + 10;
		else return false;
		v = v * 16 + d;
	}
	*out = v;
	return true;
}

/// Strict inverse of gxStressFormat: exactly nine fields, digits only, then gxStressValid. On
/// failure *out is untouched and the caller must reject the whole options string.
inline bool gxStressParse( const char *val, GXStressOption *out )
{
	if( val == nullptr || out == nullptr )
		return false;
	const char *f[9];
	size_t n[9];
	int fields = 0;
	const char *p = val;
	for( ;; )
	{
		const char *comma = strchr( p, ',' );
		const size_t len = comma ? (size_t)( comma - p ) : strlen( p );
		if( fields == 9 )
			return false;
		f[fields] = p;
		n[fields] = len;
		++fields;
		if( comma == nullptr )
			break;
		p = comma + 1;
	}
	if( fields != 9 )
		return false;

	GXStressOption o;
	gxStressClear( &o );
	if( !gxStressParseDec( f[0], n[0], &o.units ) || !gxStressParseHex( f[1], n[1], &o.unitId )
			|| !gxStressParseDec( f[2], n[2], &o.frame ) || n[3] != 1
			|| !gxStressParseDec( f[4], n[4], &o.radius ) || n[5] != 1
			|| !gxStressParseDec( f[6], n[6], &o.every ) || !gxStressParseDec( f[7], n[7], &o.buildings ) )
		return false;
	o.place = f[3][0];
	o.order = f[5][0];

	if( !( n[8] == 1 && f[8][0] == '0' ) )
	{
		const char *q = f[8];
		const char *end = f[8] + n[8];
		for( ;; )
		{
			const char *colon = q;
			while( colon < end && *colon != ':' )
				++colon;
			if( o.numBuildingIds == GX_STRESS_MAX_BUILDING_TYPES
					|| !gxStressParseHex( q, (size_t)( colon - q ), &o.buildingIds[o.numBuildingIds] ) )
				return false;
			++o.numBuildingIds;
			if( colon == end )
				break;
			q = colon + 1;
		}
	}

	if( !gxStressValid( o ) )
		return false;
	*out = o;
	return true;
}

/// Rings of onGxCheatSpawnUnit's hex lattice needed to hold `count` units: ring 0 is one slot and
/// ring r >= 1 holds 6r, so the smallest R with 1 + 3R(R+1) >= count.
inline int gxStressLatticeRings( int count )
{
	int r = 0;
	while( 1 + 3 * r * ( r + 1 ) < count )
		++r;
	return r;
}

/// One step of onGxCheatSpawnUnit's hex lattice: 2.5 body radii, floored at radius 4 so a degenerate
/// footprint cannot collapse the lattice onto a point. Here so the stress keep-out circles are sized
/// by the very rule that places the units (GameLogic::gxSpawnUnitsAround).
inline float gxStressLatticeStep( float bodyRadius ) { return ( bodyRadius > 4.0f ? bodyRadius : 4.0f ) * 2.5f; }

/// How far `count` stress units can reach from their lattice centre - the keep-out circle's radius.
/// The outer ring, plus ONE MORE step because gxSpawnUnitsAround lets findPositionAround move a unit
/// up to a step (maxRadius = ringStep) off an illegal lattice point, plus the unit's own radius.
/// Buildings built on the spawn frame cannot see those units (the partition grid refreshes at the
/// end of the frame), so this circle is all that keeps them apart.
inline float gxStressUnitExtent( int count, float bodyRadius )
{
	return (float)( gxStressLatticeRings( count ) + 1 ) * gxStressLatticeStep( bodyRadius ) + bodyRadius;
}

/// The building lattice's pitch: the widest footprint plus a lane that passes the stress unit and two
/// pathfinder cells more, never under five cells. Here, not inline in GameLogic::gxStressSpawn, so
/// stress-option-test.cpp's lane flood fill runs on the pitch the engine actually plans with.
inline float gxStressPitch( float widestFootprint, float unitRadius, float cellSize )
{
	const float lane = 2.0f * unitRadius + 2.0f * cellSize;
	return widestFootprint + ( lane > 5.0f * cellSize ? lane : 5.0f * cellSize );
}

/// Orders go out on the spawn frame and every `every` frames after it; never under policy 'N'.
inline bool gxStressOrderDue( const GXStressOption &o, unsigned int frame )
{
	if( o.order == 'N' || o.every < 1 || frame < (unsigned int)o.frame )
		return false;
	return ( frame - (unsigned int)o.frame ) % (unsigned int)o.every == 0;
}

/// Place 'B': a player's units start 40% of the way from its base toward the map centre - in front
/// of its own building field rather than inside it.
inline void gxStressStaging( float ax, float ay, float cx, float cy, float *sx, float *sy )
{
	*sx = ax + ( cx - ax ) * 0.4f;
	*sy = ay + ( cy - ay ) * 0.4f;
}

// ------------------------------------------------------------------------------------------------
// BUILDING PLANNER
// ------------------------------------------------------------------------------------------------

struct GXStressAnchor    { int player; float x, y; };				///< a player's base, in player-index order
struct GXStressKeepOut   { float x, y, r; };								///< where units will stand - no buildings
struct GXStressPlacement { int player; int typeSlot; int gx, gy; float x, y; };

/// The engine's answer for one candidate: buildable terrain, nothing immobile in the way, and a
/// ground path from the player's base. Must depend on synchronised state only.
typedef bool (*GXStressLegalFn)( void *ctx, int player, int typeSlot, float x, float y );

/// Is the ground at this point open - passable, and no structure or other obstacle on it? For any
/// player: it is a property of the map. Must depend on synchronised state only.
typedef bool (*GXStressOpenFn)( void *ctx, float x, float y );

/// True when the square of half-size `half` around (x, y) lies on the map and is `open` at every
/// sample. Samples are `step` apart with both edges included, so no `step`-sized cell the square
/// touches is skipped.
inline bool gxStressGroundOpen( float x, float y, float half, float step,
																float loX, float loY, float hiX, float hiY, GXStressOpenFn open, void *ctx )
{
	const int n = (int)ceilf( 2.0f * half / step );
	for( int j = 0; j <= n; ++j )
	{
		const float sy = ( j == n ) ? y + half : y - half + (float)j * step;
		for( int i = 0; i <= n; ++i )
		{
			const float sx = ( i == n ) ? x + half : x - half + (float)i * step;
			if( sx < loX || sy < loY || sx > hiX || sy > hiY || !open( ctx, sx, sy ) )
				return false;
		}
	}
	return true;
}

/// Cell i (0 <= i < 8k) of the square ring at Chebyshev distance k, in one fixed integer order:
/// south edge west->east, east edge south->north, north edge east->west, west edge north->south.
inline void gxStressRingCell( int k, int i, int *dx, int *dy )
{
	if( i < 2 * k )      { *dx = -k + i;             *dy = -k; }
	else if( i < 4 * k ) { *dx = k;                  *dy = -k + ( i - 2 * k ); }
	else if( i < 6 * k ) { *dx = k - ( i - 4 * k );  *dy = k; }
	else                 { *dx = -k;                 *dy = k - ( i - 6 * k ); }
}

/// Plan every player's buildings on ONE lattice over the map (cell = gxStressPitch, building at the
/// cell centre). Players take turns, one building per turn in anchor order, each walking square rings
/// outward from its own base starting at ring 2 - rings 0 and 1 of EVERY base are its core (command
/// centre and dozer: a 3x3 block of cells, about 1.5 pitches each way) and stay clear of everybody's
/// buildings. Supply docks further out are NOT reserved; the moat rule below keeps a lane round them
/// as round every other structure. A cell is taken when it is on the map, not already taken, clear of
/// every keep-out circle, inside an open moat, and `legal` for that player; a refusal is NOT
/// remembered, because ground one player cannot reach may be another player's front yard.
///
/// Why this cannot wall anything off, for a unit of radius `unitRadius`: a cell is used only if the
/// square reaching one whole lane past the widest footprint is `open` and on the map - no terrain
/// obstacle, no structure that was already there, no map edge - so every building stands in its own
/// open moat. The lane is at least the unit's diameter plus two cells (gxStressPitch), so a unit can
/// circle any building inside its moat, a whole cell off it and off everything else, and two
/// buildings' moats never reach each other's footprints (their centres are a pitch apart). Any route
/// a footprint cuts therefore goes round it instead: a gap narrower than footprint plus two lanes is
/// never built in. Integer ring order, no RNG, no unordered container: identical inputs give
/// identical output on every peer.
inline void gxStressPlanBuildings( float loX, float loY, float hiX, float hiY,
																	 float widestFootprint, float unitRadius, float cellSize,
																	 const std::vector<GXStressAnchor> &anchors, int perPlayer, int numTypes,
																	 const std::vector<GXStressKeepOut> &keepOut,
																	 GXStressOpenFn open, GXStressLegalFn legal, void *ctx,
																	 std::vector<GXStressPlacement> *out )
{
	out->clear();
	const float pitch = gxStressPitch( widestFootprint, unitRadius, cellSize );
	const float moat = pitch - 0.5f * widestFootprint;		// half the footprint, then one whole lane
	if( !( pitch > 0.0f ) || !( cellSize > 0.0f ) || perPlayer <= 0 || numTypes <= 0 || anchors.empty()
			|| open == nullptr || legal == nullptr )
		return;
	const int nx = (int)( ( hiX - loX ) / pitch );
	const int ny = (int)( ( hiY - loY ) / pitch );
	if( nx <= 0 || ny <= 0 )
		return;
	const int maxRing = nx > ny ? nx : ny;

	struct Cursor { int ax, ay, ring, idx, placed; bool done; };
	std::vector<Cursor> cur( anchors.size() );
	for( size_t p = 0; p < anchors.size(); ++p )
	{
		int ax = (int)floorf( ( anchors[p].x - loX ) / pitch );
		int ay = (int)floorf( ( anchors[p].y - loY ) / pitch );
		cur[p].ax = ax < 0 ? 0 : ( ax >= nx ? nx - 1 : ax );
		cur[p].ay = ay < 0 ? 0 : ( ay >= ny ? ny - 1 : ay );
		cur[p].ring = 2;
		cur[p].idx = 0;
		cur[p].placed = 0;
		cur[p].done = false;
	}

	// Every base's core is reserved for EVERY player, not just its owner: a neighbour's field would
	// otherwise reach into it wherever two bases are closer than the fields are wide.
	std::vector<unsigned char> taken( (size_t)nx * (size_t)ny, 0 );
	for( size_t p = 0; p < anchors.size(); ++p )
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
			{
				const int gx = cur[p].ax + dx, gy = cur[p].ay + dy;
				if( gx >= 0 && gy >= 0 && gx < nx && gy < ny )
					taken[(size_t)gy * nx + gx] = 1;
			}
	for( bool progress = true; progress; )
	{
		progress = false;
		for( size_t p = 0; p < anchors.size(); ++p )
		{
			Cursor &c = cur[p];
			while( !c.done && c.placed < perPlayer )
			{
				if( c.idx >= 8 * c.ring )
				{
					c.idx = 0;
					++c.ring;
				}
				if( c.ring > maxRing )
				{
					c.done = true;
					break;
				}
				int dx, dy;
				gxStressRingCell( c.ring, c.idx++, &dx, &dy );
				const int gx = c.ax + dx, gy = c.ay + dy;
				if( gx < 0 || gy < 0 || gx >= nx || gy >= ny || taken[(size_t)gy * nx + gx] )
					continue;
				const float x = loX + ( gx + 0.5f ) * pitch;
				const float y = loY + ( gy + 0.5f ) * pitch;
				bool blocked = false;
				for( size_t k = 0; k < keepOut.size() && !blocked; ++k )
				{
					const float ddx = x - keepOut[k].x, ddy = y - keepOut[k].y;
					const float r = keepOut[k].r + pitch * 0.5f;
					blocked = ddx * ddx + ddy * ddy < r * r;
				}
				const int slot = c.placed % numTypes;
				if( blocked || !gxStressGroundOpen( x, y, moat, cellSize, loX, loY, hiX, hiY, open, ctx )
						|| !legal( ctx, anchors[p].player, slot, x, y ) )
					continue;
				taken[(size_t)gy * nx + gx] = 1;
				GXStressPlacement pl;
				pl.player = anchors[p].player; pl.typeSlot = slot; pl.gx = gx; pl.gy = gy; pl.x = x; pl.y = y;
				out->push_back( pl );
				++c.placed;
				progress = true;
				break;
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ORDER TARGETS
// ------------------------------------------------------------------------------------------------

struct GXStressSide { int player; float x, y; bool hasBase; };		///< base centre now, in player-index order
typedef bool (*GXStressEnemiesFn)( void *ctx, int a, int b );

/// Where side `self`'s stress units attack-move to. 'C': the map centre. 'E': the nearest enemy
/// side that still has a base (squared distance, ties to the lower player), else the map centre.
/// 'N' (or anything else): no order - returns false.
inline bool gxStressPickTarget( char order, const std::vector<GXStressSide> &sides, size_t self,
																GXStressEnemiesFn enemies, void *ctx, float cx, float cy,
																float *tx, float *ty )
{
	if( order == 'C' )
	{
		*tx = cx;
		*ty = cy;
		return true;
	}
	if( order != 'E' || self >= sides.size() || enemies == nullptr )
		return false;
	int best = -1;
	float bestD = 0.0f;
	for( size_t q = 0; q < sides.size(); ++q )
	{
		if( q == self || !sides[q].hasBase || !enemies( ctx, sides[self].player, sides[q].player ) )
			continue;
		const float dx = sides[q].x - sides[self].x, dy = sides[q].y - sides[self].y;
		const float d = dx * dx + dy * dy;
		if( best < 0 || d < bestD )
		{
			best = (int)q;
			bestD = d;
		}
	}
	*tx = best < 0 ? cx : sides[best].x;
	*ty = best < 0 ? cy : sides[best].y;
	return true;
}
