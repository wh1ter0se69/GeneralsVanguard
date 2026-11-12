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

// FILE: MapUtil.h /////////////////////////////////////////////////////////
// Author: Matt Campbell, December 2001
// Description: Map utility/convenience routines
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"

#include "Common/STLTypedefs.h"

class GameWindow;
class AsciiString;
struct Coord3D;
struct FileInfo;
class Image;
class DataChunkInput;
struct DataChunkInfo;
// This matches the windows timestamp.
enum { SUPPLY_TECH_SIZE = 15};
typedef std::list <ICoord2D> ICoord2DList;

class TechAndSupplyImages
{
public:
	ICoord2DList m_techPosList;
	ICoord2DList m_supplyPosList;
};

struct WinTimeStamp
{
	UnsignedInt m_lowTimeStamp;
	UnsignedInt m_highTimeStamp;
};


class WaypointMap : public std::map<AsciiString, Coord3D>
{
public:
	void update();	///< returns the number of multiplayer start spots found
	Int m_numStartSpots;
};

typedef std::list <Coord3D> Coord3DList;

class MapMetaData
{
public:
	UnicodeString m_displayName;
	AsciiString m_nameLookupTag;
	Region3D m_extent;
	Int m_numPlayers;

	Bool m_isMultiplayer;
	Bool m_isOfficial;
	Bool m_doesExist; ///< Flag to indicate whether the map physically exists. Should be true.
	UnsignedInt m_filesize;
	UnsignedInt m_CRC;

	WinTimeStamp m_timestamp;

	WaypointMap m_waypoints;
	Coord3DList m_supplyPositions;
	Coord3DList m_techPositions;
	AsciiString m_fileName;

	// The path as it is on disk, case intact. m_fileName is lowercased: it is the cache key and the identity sent
	// in the game options (GameInfoToAsciiString). That breaks on iOS, whose app-data volume is case-sensitive and whose
	// sandbox cannot list /private/var/mobile for a fallback. Keep both: lowercase for keys and the wire, this to open.
	AsciiString m_filePathOnDisk;
};

// TheSuperHackers @performance xezon 02/11/2025 Simplifies and improves the implementation of MapCache
// to prevent expensive reoccurring redundant map cache reads.

class MapCache : public std::map<AsciiString, MapMetaData>
{
	typedef std::set<AsciiString> MapNameSet;

public:
	// GeneralsX: Translate a lowercased map identity back to the path as it
	// exists on disk. Returns the input unchanged when the cache has no on-disk path for it - an
	// entry restored from MapCache.ini has only the lowercased form - so callers keep today's
	// behaviour rather than getting an empty string.
	AsciiString getPathOnDisk(const AsciiString& lowerPath) const;

	// GeneralsX: Erase user-map entries that do not live under the current
	// user map root - i.e. left over from a previous iOS data-container UUID. clearUnseenMaps cannot:
	// it only inspects keys under the root it is given.
	Bool clearMapsOutsideRoot(const AsciiString& userMapDir);

	MapCache()
		: m_doCreateStandardMapCacheINI(TRUE)
		, m_doLoadStandardMapCacheINI(TRUE)
		, m_doLoadUserMapCacheINI(TRUE)
	{}

	void updateCache();

	AsciiString getMapDir() const;
	AsciiString getUserMapDir() const;
	AsciiString getMapExtension() const;

	const MapMetaData *findMap(AsciiString mapName);

	// GeneralsX: How many lobby slots this map takes (Common/PlayerCapacity.h),
	// read from the map's own side list before it is loaded, for the host's Start check. -1 when the
	// map cannot be read; the start itself refuses an over-capacity lobby on every peer regardless.
	Int getPlayerSlotCapacity(const AsciiString &mapName);

	// allow us to create a set of shippable maps to be in mapcache.ini.  For use with -buildMapCache.
	void addShippingMap(AsciiString mapName) { mapName.toLower(); m_allowedMaps.insert(mapName); }

private:
	void prepareUnseenMaps(const AsciiString &mapDir);
	Bool clearUnseenMaps(const AsciiString &mapDir);
	Bool loadMapsFromMapCacheINI(const AsciiString &mapDir); // returns false if the map cache file exists but could not be parsed
	Bool loadMapsFromDisk(const AsciiString &mapDir, Bool isOfficial, Bool filterByAllowedMaps = FALSE); // returns true if we needed to (re)parse a map
	Bool addMap(const AsciiString &mapDir, const AsciiString &fname, const AsciiString &lowerFname, FileInfo &fileInfo, Bool isOfficial); ///< returns true if it had to (re)parse the map
	void writeCacheINI(const AsciiString &mapDir);

	static const char *const m_mapCacheName;

	MapNameSet m_allowedMaps;
	Bool m_doCreateStandardMapCacheINI;
	Bool m_doLoadStandardMapCacheINI;
	Bool m_doLoadUserMapCacheINI;
};

extern MapCache *TheMapCache;
extern TechAndSupplyImages TheSupplyAndTechImageLocations;

// TheSuperHackers @refactor xezon 28/11/2025 Refactors the map list population implementation
// by breaking it into smaller pieces to make it more maintainable.

// GeneralsX: Optional display-name filter, so a search box can narrow the list without
// duplicating any of the grouping, ordering, medal or item-data logic. It is defaulted to empty,
// which means "match everything", so every existing caller keeps its current behaviour untouched.
Int populateMapListbox( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString,
	UnicodeString filter = UnicodeString::TheEmptyString );		/// Read a list of maps from the run directory and fill in the listbox.  Return the selected index
Int populateMapListboxNoReset( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect = AsciiString::TheEmptyString,
	UnicodeString filter = UnicodeString::TheEmptyString );		/// Read a list of maps from the run directory and fill in the listbox.  Return the selected index
Bool isValidMap( AsciiString mapName, Bool isMultiplayer );						/// Validate a map
Image *getMapPreviewImage( AsciiString mapName );
AsciiString getDefaultMap( Bool isMultiplayer );											/// Find a valid map
AsciiString getDefaultOfficialMap();
Bool isOfficialMap( AsciiString mapName );
Bool parseMapPreviewChunk(DataChunkInput &file, DataChunkInfo *info, void *userData);
void findDrawPositions( Int startX, Int startY, Int width, Int height, Region3D extent,
															 ICoord2D *ul, ICoord2D *lr );
Bool WouldMapTransfer( const AsciiString& mapName );
