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

// FILE: MapUtil.cpp //////////////////////////////////////////////////////////////////////////////
// Author: Matt Campbell, December 2001
// Description: Map utility/convenience routines
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include <cstdint>
#include <wctype.h>	// GeneralsX: towlower, for the case-insensitive map name filter

#include "Common/crc.h"
#include "Common/FileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/file.h"
#include "Common/GlobalData.h"
#include "GameClient/GXReplaceFile.h"
#include "Common/GameState.h"
#include "Common/GameEngine.h"
#include "Common/NameKeyGenerator.h"
#include "Common/DataChunk.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/MessageStream.h"
#include "Common/WellKnownKeys.h"
#include "Common/INI.h"
#include "Common/QuotedPrintable.h"
#include "Common/SkirmishBattleHonors.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/MapObject.h"
#include "GameClient/GameText.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Image.h"
#include "GameClient/Shell.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MapCacheCredibility.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/FPUControl.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "Common/PlayerCapacity.h"


//-------------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static const char *mapExtension = ".map";

// GeneralsX: Accept both '\' and '/' separators so user custom maps work on macOS/Linux paths.
static const char *findLastPathSeparator(const AsciiString& path)
{
	const char *backslash = path.reverseFind('\\');
	const char *slash = path.reverseFind('/');

	if (!backslash)
	{
		return slash;
	}

	if (!slash)
	{
		return backslash;
	}

	return slash > backslash ? slash : backslash;
}

static Bool hasValidMapLeafLayout(const AsciiString& filepathLower, const AsciiString& filenameLower)
{
	AsciiString endingStrBackslash;
	endingStrBackslash.format("%s\\%s%s", filenameLower.str(), filenameLower.str(), mapExtension);

	AsciiString endingStrSlash;
	endingStrSlash.format("%s/%s%s", filenameLower.str(), filenameLower.str(), mapExtension);

	return filepathLower.endsWithNoCase(endingStrBackslash.str()) || filepathLower.endsWithNoCase(endingStrSlash.str());
}

static Int m_width = 0;						///< Height map width.
static Int m_height = 0;					///< Height map height (y size of array).
static Int m_borderSize = 0;			///< Non-playable border area.
static std::vector<ICoord2D> m_boundaries;	///< All the boundaries we use for the map
static Int m_dataSize = 0;				///< size of m_data.
static UnsignedByte *m_data = nullptr;	///< array of z(height) values in the height map.
static Dict worldDict = 0;

static WaypointMap *m_waypoints = nullptr;
static Coord3DList	m_supplyPositions;
static Coord3DList	m_techPositions;

static Int m_mapDX = 0;
static Int m_mapDY = 0;

static UnsignedInt calcCRC( AsciiString fname )
{
	CRC theCRC;
	theCRC.clear();

	File *fp = TheFileSystem->openFile(fname.str(), File::READ);
	if( !fp )
	{
		DEBUG_CRASH(("Couldn't open '%s'", fname.str()));
		return 0;
	}

	UnsignedByte buf[4096];
	Int num;
	while ( (num=fp->read(buf, 4096)) > 0 )
	{
		theCRC.computeCRC(buf, num);
	}

	fp->close();
	fp = nullptr;

	return theCRC.get();
}

static Bool ParseObjectDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	Bool readDict = info->version >= K_OBJECTS_VERSION_2;

	Coord3D loc;
	loc.x = file.readReal();
	loc.y = file.readReal();
	loc.z = file.readReal();
	if (info->version <= K_OBJECTS_VERSION_2)
	{
		loc.z = 0;
	}

	Real angle = file.readReal();
	Int flags = file.readInt();
	AsciiString name = file.readAsciiString();
	Dict d;
	if (readDict)
	{
		d = file.readDict();
	}
	MapObject *pThisOne;

	// create the map object
	pThisOne = newInstance( MapObject )( loc, name, angle, flags, &d,
														TheThingFactory->findTemplate( name, FALSE ) );

//DEBUG_LOG(("obj %s owner %s",name.str(),d.getAsciiString(TheKey_originalOwner).str()));

	if (pThisOne->getProperties()->getType(TheKey_waypointID) == Dict::DICT_INT)
	{
		pThisOne->setIsWaypoint();

		// grab useful info
		(*m_waypoints)[pThisOne->getWaypointName()] = loc;
	}
	else if (pThisOne->getThingTemplate() && pThisOne->getThingTemplate()->isKindOf(KINDOF_TECH_BUILDING))
	{
		m_techPositions.push_back(loc);
	}
	else if (pThisOne->getThingTemplate() && pThisOne->getThingTemplate()->isKindOf(KINDOF_SUPPLY_SOURCE_ON_PREVIEW))
	{
		m_supplyPositions.push_back(loc);
	}

	deleteInstance(pThisOne);
	return TRUE;
}

static Bool ParseObjectsDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	file.m_currentObject = nullptr;
	file.registerParser( "Object", info->label, ParseObjectDataChunk );
	return (file.parse(userData));
}

static Bool ParseWorldDictDataChunk(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	worldDict = file.readDict();
	return true;
}

static Bool ParseSizeOnly(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	if (m_dataSize != 0) {
		throw ERROR_CORRUPT_FILE_FORMAT;	// a second HeightMapData chunk: the loader refuses it too
	}
	if (info->version > K_HEIGHT_MAP_VERSION_5) {
		throw ERROR_CORRUPT_FILE_FORMAT;	// GeneralsX @feature as the loader: a version it does not know is refused
	}
	m_width = file.readInt();
	m_height = file.readInt();
	if (info->version >= K_HEIGHT_MAP_VERSION_3) {
		m_borderSize = file.readInt();
	} else {
		m_borderSize = 0;
	}

	if (info->version >= K_HEIGHT_MAP_VERSION_4) {
		Int numBorders = file.readInt();
		// GeneralsX: As in WorldHeightMap's readers: the count sized a resize() before
		// anything checked it, and this one runs for every .map the map list scans (a transferred one included).
		// Each boundary is two ints that follow in this chunk, so a count the chunk cannot hold is damage.
		if (numBorders < 0 || numBorders > (Int)file.getChunkDataSizeLeft() / (Int)(2 * sizeof(Int))) {
			throw ERROR_CORRUPT_FILE_FORMAT;
		}
		m_boundaries.resize(numBorders);
		for (int i = 0; i < numBorders; ++i) {
			m_boundaries[i].x = file.readInt();
			m_boundaries[i].y = file.readInt();
		}
	}
	// GeneralsX: The list judges the height map header exactly as the loader does
	// (WorldHeightMap.cpp: heightMapSizeFits, boundariesFitHeightMap, the border check) without reading the samples, so a
	// map a start would refuse as damaged is not offered at all. This returned before dataSize, and an unreachable copy of
	// the loader's old unchecked allocation followed; a mismatched size was listed and refused only when a match started.
	// GeneralsX @feature Version 5 stores the height scale multiplier k before dataSize (WorldHeightMap.cpp's readers).
	if (info->version >= K_HEIGHT_MAP_VERSION_5) {
		const Int heightScaleK = file.readInt();
		if (heightScaleK < 1 || heightScaleK > MAP_HEIGHT_SCALE_K_MAX) {
			throw ERROR_CORRUPT_FILE_FORMAT;
		}
	}
	m_dataSize = file.readInt();
	const Int minSide = info->version == K_HEIGHT_MAP_VERSION_1 ? 3 : 2;	// as heightMapSizeFits: two samples a side once loaded
	if (m_width < minSide || m_height < minSide || m_dataSize <= 0 || m_dataSize > (Int)file.getChunkDataSizeLeft()
		|| (Int64)m_width * (Int64)m_height != (Int64)m_dataSize
		|| m_borderSize < 0 || (Int64)m_width - 2 * (Int64)m_borderSize < 2 || (Int64)m_height - 2 * (Int64)m_borderSize < 2) {
		throw ERROR_CORRUPT_FILE_FORMAT;
	}
	if (info->version >= K_HEIGHT_MAP_VERSION_5 && m_dataSize <= MAP_HEIGHT_SCALE_K_MAX) {
		throw ERROR_CORRUPT_FILE_FORMAT;	// GeneralsX @feature as the loader: a version 5 map has more samples than any k
	}
	for (size_t i = 0; i < m_boundaries.size(); ++i) {
		if (m_boundaries[i].x > 2 * (Int64)m_width || m_boundaries[i].y > 2 * (Int64)m_height) {
			throw ERROR_CORRUPT_FILE_FORMAT;
		}
	}
	return true;
}

// GeneralsX: BlendTileData, judged as the loader judges its framing (WorldHeightMap.cpp: after
// HeightMapData, once, a per-cell length equal to dataSize, and the per-cell arrays all present) without reading them, so
// a map whose terrain the start would refuse is not listed. loadMap requires one to have been seen.
static Bool s_sawBlendTileData = FALSE;
static Bool ParseBlendTileSizeOnly(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	if (m_dataSize <= 0 || s_sawBlendTileData || file.readInt() != m_dataSize) {
		throw ERROR_CORRUPT_FILE_FORMAT;
	}
	if (info->version == K_BLEND_TILE_VERSION_1 && ((Int64)(m_width + 1) / 2 - 2 * (Int64)m_borderSize < 2
			|| (Int64)(m_height + 1) / 2 - 2 * (Int64)m_borderSize < 2)) {
		throw ERROR_CORRUPT_FILE_FORMAT;	// the loader halves this map: two samples between the borders afterwards
	}
	const Int cellArrays = 2 + (info->version >= K_BLEND_TILE_VERSION_6 ? 1 : 0) + (info->version >= K_BLEND_TILE_VERSION_5 ? 1 : 0);
	if ((Int64)m_dataSize * (Int64)sizeof(Short) * cellArrays > (Int64)(Int)file.getChunkDataSizeLeft()) {
		throw ERROR_CORRUPT_FILE_FORMAT;
	}
	s_sawBlendTileData = TRUE;
	return true;
}

static Bool ParseSizeOnlyInChunk(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	return ParseSizeOnly(file, info, userData);
}

static Bool loadMap( AsciiString filename )
{
	CachedFileInputStream fileStrm;

	if( !fileStrm.open(filename) )
	{
		return FALSE;
	}

	ChunkInputStream *pStrm = &fileStrm;

	DataChunkInput file( pStrm );

	m_waypoints = NEW WaypointMap;

	// GeneralsX: These are file statics shared by every map the list scans, and nothing reset
	// them: a map with no HeightMapData was listed with the previous map's size. A map with no height map is damaged.
	m_width = m_height = m_borderSize = m_dataSize = 0;
	m_boundaries.clear();
	s_sawBlendTileData = FALSE;

	file.registerParser( "HeightMapData", AsciiString::TheEmptyString, ParseSizeOnlyInChunk );
	file.registerParser( "BlendTileData", AsciiString::TheEmptyString, ParseBlendTileSizeOnly );
	file.registerParser( "WorldInfo", AsciiString::TheEmptyString, ParseWorldDictDataChunk );
	file.registerParser( "ObjectsList", AsciiString::TheEmptyString, ParseObjectsDataChunk );
	if (!file.parse(nullptr) || m_dataSize <= 0 || !s_sawBlendTileData) {
		throw(ERROR_CORRUPT_FILE_FORMAT);
	}

	m_mapDX = m_width  - 2*m_borderSize;
	m_mapDY = m_height - 2*m_borderSize;

	return TRUE;
}

static void resetMap()
{
	delete[] m_data;
	m_data = nullptr;

	delete m_waypoints;
	m_waypoints = nullptr;

	m_techPositions.clear();
	m_supplyPositions.clear();
}

static void getExtent( Region3D *extent )
{
	extent->lo.x = 0.0f;

	extent->lo.y = 0.0f;

	// Note - m_mapDX & Y are the number of height map grids wide, so we have to
	// multiply by the grid width.
	extent->hi.x = m_mapDX*MAP_XY_FACTOR;
	extent->hi.y = m_mapDY*MAP_XY_FACTOR;

	extent->lo.z = 0;
	extent->hi.z = 0;
}

//-------------------------------------------------------------------------------

void WaypointMap::update()
{
	if (!m_waypoints)
	{
		m_numStartSpots = 1;
		return;
	}

	this->clear();

	AsciiString startingCamName = TheNameKeyGenerator->keyToName(TheKey_InitialCameraPosition);
	WaypointMap::const_iterator it;

	it = m_waypoints->find(startingCamName);
	if (it != m_waypoints->end())
	{
		(*this)[startingCamName] = it->second;
	}

	m_numStartSpots = 0;
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		startingCamName.format("Player_%d_Start", i+1); // start pos waypoints are 1-based
		it = m_waypoints->find(startingCamName);
		if (it != m_waypoints->end())
		{
			(*this)[startingCamName] = it->second;
			++m_numStartSpots;
		}
		else
		{
			break;
		}
	}

	m_numStartSpots = max(1, m_numStartSpots);
}

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
const char *const MapCache::m_mapCacheName = "MapCache.ini";
// GX-SIMID-END

AsciiString MapCache::getMapDir() const
{
	return "Maps";
}

AsciiString MapCache::getUserMapDir() const
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	tmp.concat(getMapDir());
	return tmp;
}

AsciiString MapCache::getMapExtension() const
{
	return "map";
}

void MapCache::writeCacheINI( const AsciiString &mapDir )
{
	AsciiString filepath = mapDir;
	// GeneralsX @bugfix fbraz 06/05/2026 Use portable separator for host filesystem paths to avoid creating literal "\\" filenames on POSIX.
	filepath.concat('/');

	TheFileSystem->createDirectory(mapDir);

	filepath.concat(m_mapCacheName);
	// GeneralsX: Written beside the cache and swapped into place, never
	// truncated in place: a half-written MapCache.ini is read back as a cache with maps missing, and
	// on Windows the user copy lives under Documents, which OneDrive keeps under sync. See
	// GameClient/GXReplaceFile.h.
	AsciiString tmppath = filepath;
	tmppath.concat(".tmp");
	FILE *fp = fopen(tmppath.str(), "w");
	DEBUG_ASSERTCRASH(fp != nullptr, ("Failed to create %s", tmppath.str()));
	if (fp == nullptr) {
		return;
	}

	fprintf(fp, "; FILE: %s /////////////////////////////////////////////////////////////\n", filepath.str());
	fprintf(fp, "; This INI file is auto-generated - do not modify\n");
	fprintf(fp, "; /////////////////////////////////////////////////////////////////////////////\n");

	MapCache::iterator it = begin();
	for (; it != end(); ++it)
	{
		if (it->first.startsWithNoCase(mapDir.str()))
		{
			const MapMetaData &md = it->second;
			fprintf(fp, "\nMapCache %s\n", AsciiStringToQuotedPrintable(it->first.str()).str());
			fprintf(fp, "  fileSize = %u\n", md.m_filesize);
			fprintf(fp, "  fileCRC = %u\n", md.m_CRC);
			fprintf(fp, "  timestampLo = %d\n", md.m_timestamp.m_lowTimeStamp);
			fprintf(fp, "  timestampHi = %d\n", md.m_timestamp.m_highTimeStamp);
			fprintf(fp, "  isOfficial = %s\n", (md.m_isOfficial)?"yes":"no");

			fprintf(fp, "  isMultiplayer = %s\n", (md.m_isMultiplayer)?"yes":"no");
			fprintf(fp, "  numPlayers = %d\n", md.m_numPlayers);

			fprintf(fp, "  extentMin = X:%2.2f Y:%2.2f Z:%2.2f\n", md.m_extent.lo.x, md.m_extent.lo.y, md.m_extent.lo.z);
			fprintf(fp, "  extentMax = X:%2.2f Y:%2.2f Z:%2.2f\n", md.m_extent.hi.x, md.m_extent.hi.y, md.m_extent.hi.z);

// BAD AND NOW UNUSED:  the mapcache.ini should not contain localized data... using the lookup tag instead
#if RTS_GENERALS
			fprintf(fp, "  displayName = %s\n", UnicodeStringToQuotedPrintable(md.m_displayName).str());
#else
			fprintf(fp, "  nameLookupTag = %s\n", md.m_nameLookupTag.str());
#endif

			Coord3D pos;
			WaypointMap::const_iterator itw = md.m_waypoints.begin();
			for (; itw != md.m_waypoints.end(); ++itw)
			{
				pos = itw->second;
				fprintf(fp, "  %s = X:%2.2f Y:%2.2f Z:%2.2f\n", itw->first.str(), pos.x, pos.y, pos.z);
			}
			Coord3DList::const_iterator itc3d = md.m_techPositions.begin();
			for (; itc3d != md.m_techPositions.end(); ++itc3d)
			{
				pos = *itc3d;
				fprintf(fp, "  techPosition = X:%2.2f Y:%2.2f Z:%2.2f\n", pos.x, pos.y, pos.z);
			}

			itc3d = md.m_supplyPositions.begin();
			for (; itc3d != md.m_supplyPositions.end(); ++itc3d)
			{
				pos = *itc3d;
				fprintf(fp, "  supplyPosition = X:%2.2f Y:%2.2f Z:%2.2f\n", pos.x, pos.y, pos.z);
			}
			fprintf(fp, "END\n\n");
		}
		else
		{
			//DEBUG_LOG(("%s does not start %s", mapDir.str(), it->first.str()));
		}
	}

	if (!gxCommitAndReplace(fp, tmppath.str(), filepath.str()))
		fprintf(stderr, "[MapCache] could not write %s - the previous cache is kept\n", filepath.str());
}

void MapCache::updateCache()
{
	setFPMode();

	const AsciiString mapDir = getMapDir();
	const AsciiString userMapDir = getUserMapDir();

	// Create the standard map cache if required. Is only relevant for Mod developers.
	// TheSuperHackers @tweak This step is done before loading any other map caches to not poison the cached state.
	if (m_doCreateStandardMapCacheINI)
	{
#if defined(RTS_DEBUG)
		// only create the map cache file if "Maps" folder exists
		const Bool buildMapCache = TheLocalFileSystem->doesFileExist(mapDir.str());
#else
		const Bool buildMapCache = TheGlobalData->m_buildMapCache;
#endif
		if (buildMapCache)
		{
			const Bool isOfficial = TRUE;
			const Bool filterByAllowedMaps = !m_allowedMaps.empty();

			if (loadMapsFromDisk(mapDir, isOfficial, filterByAllowedMaps))
			{
				writeCacheINI(mapDir);
			}
		}
		m_doCreateStandardMapCacheINI = FALSE;
	}

	// getMapDir() (official root) loads from a prebuilt MapCache.ini, so maps dropped there are silently never seen;
	// only getUserMapDir() is scanned from disk. Warn instead of failing silently. Plain warning, not an error:
	// it fires only when the standard cache is not being built, when the contents cannot matter.
	if (!m_doCreateStandardMapCacheINI && TheLocalFileSystem != nullptr)
	{
		// TheLocalFileSystem, not TheFileSystem: the latter searches archives too, so shipped maps in MapsZH.big would match.
		// Base path goes in originalDirectory: the recursion passes only the subfolder name as currentDirectory.
		// Only one level below mapDir is searched. The trailing separator is required: nothing is inserted between the parts.
		FilenameList strayMaps;
		AsciiString mapDirSearch;
		mapDirSearch.format("%s\\", mapDir.str());
		TheLocalFileSystem->getFileListInDirectory(AsciiString::TheEmptyString, mapDirSearch,
			AsciiString("*.map"), strayMaps, TRUE);
		if (!strayMaps.empty())
		{
			fprintf(stderr,
				"[GXMAP] %d .map file(s) are in '%s', which is the OFFICIAL map folder and is read "
				"from a prebuilt cache - they will NOT appear in game. Custom maps belong in '%s'.\n",
				(Int)strayMaps.size(), mapDir.str(), userMapDir.str());
			fflush(stderr);
		}
	}

	// Load user map cache first.
	if (m_doLoadUserMapCacheINI)
	{
		// TheSuperHackers @fix wh1ter0se 31/07/2026 Rebuild the cache from disk when its INI file is
		// unreadable, and write a good one back so the bad file does not survive to the next startup.
		if (!loadMapsFromMapCacheINI(userMapDir))
		{
			loadMapsFromDisk(userMapDir, FALSE);
			writeCacheINI(userMapDir);
		}
		m_doLoadUserMapCacheINI = FALSE;
	}

	// Drop anything left over from a previous container root before the scan runs, so a stale entry
	// cannot shadow the live copy of the same map for this session.
	//
	// The result feeds the write below. Erasing them only in memory means the scan can find nothing
	// changed, skip writeCacheINI, and load and prune the very same dead entries again on the next
	// launch - self-healing that never actually heals the file.
	const Bool gxPrunedStaleRoots = clearMapsOutsideRoot(userMapDir);

	// Load user maps from disk and update any discrepancies from the map cache.
	if (loadMapsFromDisk(userMapDir, FALSE) || gxPrunedStaleRoots)
	{
		writeCacheINI(userMapDir);
		m_doLoadStandardMapCacheINI = TRUE;
	}

	// Load standard maps from map cache last.
	// This overwrites matching user maps to prevent munkees getting rowdy :)
	if (m_doLoadStandardMapCacheINI)
	{
		// TheSuperHackers @fix wh1ter0se 31/07/2026 Same recovery as for the user map cache above.
		if (!loadMapsFromMapCacheINI(mapDir))
		{
			const Bool isOfficial = TRUE;
			loadMapsFromDisk(mapDir, isOfficial);
			writeCacheINI(mapDir);
		}
		m_doLoadStandardMapCacheINI = FALSE;
	}
}

void MapCache::prepareUnseenMaps( const AsciiString &mapDir )
{
	MapCache::iterator it = begin();
	for (; it != end(); ++it)
	{
		const AsciiString &mapName = it->first;

		if (mapName.startsWithNoCase(mapDir.str()))
		{
			it->second.m_doesExist = FALSE;
		}
	}
}

// Drops user-map entries not under the current user map root, e.g. one written under a previous iOS data-container UUID.
// clearUnseenMaps and prepareUnseenMaps only inspect keys under the CURRENT map directory, and INIMapCache's parser sets
// m_doesExist = TRUE on everything it reads, so such an entry would stay, name a path nothing can open, and could win the
// display-name dedupe or findMap ahead of the live copy. The official root is a separate scan (m_isOfficial) and is untouched.
Bool MapCache::clearMapsOutsideRoot( const AsciiString &userMapDir )
{
	Bool erasedSomething = FALSE;

	MapCache::iterator it = begin();
	while (it != end())
	{
		MapCache::iterator next = it;
		++next;

		if (!it->second.m_isOfficial && !it->first.startsWithNoCase(userMapDir.str()))
		{
			erase(it);
			erasedSomething = TRUE;
		}

		it = next;
	}

	return erasedSomething;
}

Bool MapCache::clearUnseenMaps( const AsciiString &mapDir )
{
	Bool erasedSomething = FALSE;

	MapCache::iterator it = begin();
	while (it != end())
	{
		MapCache::iterator next = it;
		++next;

		const AsciiString &mapName = it->first;
		const MapMetaData &mapData = it->second;

		if (mapName.startsWithNoCase(mapDir.str()) && !mapData.m_doesExist)
		{
			erase(it);
			erasedSomething = TRUE;
		}

		it = next;
	}

	return erasedSomething;
}

// TheSuperHackers @fix wh1ter0se 31/07/2026 Survive an unreadable MapCache.ini. INI::load throws on
// a corrupt or truncated file, and the exception used to escape to the top level and kill the game
// on startup, leaving the user no recovery short of deleting the file by hand.
Bool MapCache::loadMapsFromMapCacheINI( const AsciiString &mapDir )
{
	INI ini;
	AsciiString fname;
	// GeneralsX @bugfix fbraz 06/05/2026 Keep MapCache INI path consistent with writeCacheINI() on POSIX hosts.
	fname.format("%s/%s", mapDir.str(), m_mapCacheName);

	if (TheFileSystem->doesFileExist(fname.str()))
	{
		try
		{
			ini.load( fname, INI_LOAD_OVERWRITE, nullptr );
		}
		catch (...)
		{
			DEBUG_LOG(("MapCache::loadMapsFromMapCacheINI - could not parse '%s'; discarding its entries", fname.str()));

			// Discard whatever the broken file managed to add before it threw. Map cache keys are
			// lowercased full paths, so the entries it contributed all live under this map directory.
			prepareUnseenMaps(mapDir);
			clearUnseenMaps(mapDir);

			return FALSE;
		}
	}

	return TRUE;
}

Bool MapCache::loadMapsFromDisk( const AsciiString &mapDir, Bool isOfficial, Bool filterByAllowedMaps )
{
	prepareUnseenMaps(mapDir);

	FilenameList filepathList;
	FilenameListIter filepathIt;
	AsciiString toplevelPattern;
	toplevelPattern.format("%s\\", mapDir.str());
	Bool mapListChanged = FALSE;
	AsciiString filenamepattern;
	filenamepattern.format("*.%s", getMapExtension().str());

	TheFileSystem->getFileListInDirectory(toplevelPattern, filenamepattern, filepathList, TRUE);

	filepathIt = filepathList.begin();

	for (; filepathIt != filepathList.end(); ++filepathIt)
	{
		FileInfo fileInfo;
		AsciiString filepathLower = *filepathIt;
		filepathLower.toLower();

		const char *szFilenameLower = findLastPathSeparator(filepathLower);
		if (!szFilenameLower)
		{
			DEBUG_CRASH(("Couldn't find path separator in map name!"));
			continue;
		}

		AsciiString filenameLower = szFilenameLower+1;
		filenameLower.truncateBy(strlen(mapExtension));

		if (filterByAllowedMaps && m_allowedMaps.find(filenameLower) == m_allowedMaps.end())
		{
			DEBUG_CRASH(("Map '%s' has been filtered out", filenameLower.str()));
			continue;
		}

		if (!hasValidMapLeafLayout(filepathLower, filenameLower))
		{
			DEBUG_CRASH(("Found map '%s' in wrong spot (%s)", filenameLower.str(), filepathLower.str()));
			continue;
		}

		if (!TheFileSystem->getFileInfo(*filepathIt, &fileInfo))
		{
			DEBUG_CRASH(("Could not get file info for map %s", filepathIt->str()));
			continue;
		}

		// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
		mapListChanged |= addMap(mapDir, *filepathIt, filepathLower, fileInfo, isOfficial);
		// GX-SIMID-END
	}

	if (clearUnseenMaps(mapDir))
	{
		mapListChanged = TRUE;
	}

	return mapListChanged;
}

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
// GeneralsX: See MapMetaData::m_filePathOnDisk.
AsciiString MapCache::getPathOnDisk(const AsciiString& lowerPath) const
{
	// GeneralsX: NORMALISE, do not trust the parameter's name.
	//
	// addMap stores the whole key lowercased, and findMap lowercases its argument before looking
	// up - but this did not, so a caller holding a path that is only PARTLY lowercased missed and
	// got its own argument back. portableMapPathToRealMapPath produces exactly that on a joining
	// non-Windows peer: the user-root keeps its real casing while the portable tail is lowercase.
	// findMap then succeeds, so the transfer is correctly skipped, and this silently returns the
	// unusable path - the one case where the whole translation is needed most.
	//
	// Cheap, and the blast radius grew: this now has four callers rather than one.
	AsciiString key = lowerPath;
	key.toLower();

	MapCache::const_iterator it = find(key);
	if (it == end() || it->second.m_filePathOnDisk.isEmpty())
		return lowerPath;
	return it->second.m_filePathOnDisk;
}
// GX-SIMID-END

// GeneralsX: A cache hit must also carry start data a scan could produce.
// Rule and why: GameClient/MapCacheCredibility.h. A rejected entry falls through to the miss path
// below, which re-reads the map file and returns TRUE, so updateCache rewrites MapCache.ini with the
// real values. Nothing re-judges that fresh result inside the same scan, so there is no loop.
// Trade-off: a genuine map that trips the rule is re-scanned (and the cache rewritten) on every
// updateCache; none of 1216 real entries does. Keep a scanned-this-session flag if one ever does.
static Bool gxCachedStartsCredible(const AsciiString &fname, const MapMetaData &md)
{
	const Int numStarts = min(md.m_numPlayers, (Int)MAX_SLOTS);
	const Coord3D *starts[MAX_SLOTS];
	AsciiString name;
	for (Int k = 0; k < numStarts; ++k)
	{
		name.format("Player_%d_Start", k + 1);
		WaypointMap::const_iterator it = md.m_waypoints.find(name);
		starts[k] = (it != md.m_waypoints.end()) ? &it->second : nullptr;
	}
	WaypointMap::const_iterator cam = md.m_waypoints.find(TheNameKeyGenerator->keyToName(TheKey_InitialCameraPosition));
	if (GXMapCache::startsCredible(numStarts, cam != md.m_waypoints.end() ? &cam->second : nullptr, starts))
		return TRUE;

	fprintf(stderr, "[GXMAP] cached start positions for '%s' (numPlayers=%d) cannot have come from the map "
		"file - re-scanning it. See MapCacheCredibility.h.\n", fname.str(), md.m_numPlayers);
	fflush(stderr);
	return FALSE;
}

Bool MapCache::addMap(
	const AsciiString &/*mapDir*/,
	const AsciiString &fname,
	const AsciiString &lowerFname,
	FileInfo &fileInfo,
	Bool isOfficial)
{
	MapCache::iterator it = find(lowerFname);
	if (it != end())
	{
		// Found the map in our cache. Check to see if it has changed.
		const MapMetaData& md = it->second;

		// Size alone is not unchanged: a re-edited map of identical size kept a stale cached CRC, which join admission and
		// the has-map handshake compare. User maps therefore also compare timestamps (persisted in MapCache.ini); official
		// maps live in the immutable .big and their timestamps may differ from the shipped cache, so they are exempt.
		if (md.m_filesize == fileInfo.sizeLow && md.m_CRC != 0
			&& (isOfficial
				|| (md.m_timestamp.m_highTimeStamp == fileInfo.timestampHigh
					&& md.m_timestamp.m_lowTimeStamp == fileInfo.timestampLow))
			&& gxCachedStartsCredible(fname, md))
		{
			// Force a lookup so that we don't display the English localization in all builds.
			if (md.m_nameLookupTag.isEmpty())
			{
				// unofficial maps or maps without names
				AsciiString tempdisplayname;
				const char *displayNameStart = findLastPathSeparator(fname);
				tempdisplayname = displayNameStart ? displayNameStart + 1 : fname;
				(*this)[lowerFname].m_displayName.translate(tempdisplayname);
				if (md.m_numPlayers >= 2)
				{
					UnicodeString extension;
					extension.format(L" (%d)", md.m_numPlayers);
					(*this)[lowerFname].m_displayName.concat(extension);
				}
			}
			else
			{
				// official maps with name tags
				(*this)[lowerFname].m_displayName = TheGameText->fetch(md.m_nameLookupTag);
				if (md.m_numPlayers >= 2)
				{
					UnicodeString extension;
					extension.format(L" (%d)", md.m_numPlayers);
					(*this)[lowerFname].m_displayName.concat(extension);
				}
			}

			it->second.m_doesExist = TRUE;

			// Record the real path on a cache hit too: m_filePathOnDisk was set only on a miss, so an entry restored from
			// MapCache.ini left it empty and getPathOnDisk fell back to the lowercased identity, which names nothing on iOS.
			// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
			it->second.m_filePathOnDisk = fname;
			// GX-SIMID-END

//			DEBUG_LOG(("MapCache::addMap - found match for map %s", lowerFname.str()));
			return FALSE;	// OK, it checks out.
		}
		DEBUG_LOG(("%s didn't match file in MapCache", fname.str()));
		DEBUG_LOG(("size: %d / %d", fileInfo.sizeLow, md.m_filesize));
		DEBUG_LOG(("time1: %d / %d", fileInfo.timestampHigh, md.m_timestamp.m_highTimeStamp));
		DEBUG_LOG(("time2: %d / %d", fileInfo.timestampLow, md.m_timestamp.m_lowTimeStamp));
	}

	DEBUG_LOG(("MapCache::addMap(): caching '%s' because '%s' was not found", fname.str(), lowerFname.str()));

	// GeneralsX: An unreadable or corrupt map was cached anyway: loadMap's failure was
	// ignored (and on an open failure m_waypoints stayed null for the update() below), a throw escaped the whole scan,
	// and a zero CRC was stored as the map's identity. Skip such a map and drop any stale entry for it instead
	// (tsh batch2 #247, idea of Generals Online 5b715ba23, reimplemented).
	Bool loaded = FALSE;
	try
	{
		loaded = loadMap(fname); // Just load for querying the data, since we aren't playing this map.
	}
	catch (...)
	{
		loaded = FALSE;
	}
	const UnsignedInt mapCRC = loaded ? calcCRC(fname) : 0;
	if (!loaded || mapCRC == 0)
	{
		DEBUG_LOG(("MapCache::addMap(): '%s' could not be read; not caching it", fname.str()));
		resetMap();
		if (it != end())
		{
			erase(it);
			return TRUE;
		}
		return FALSE;
	}

	// The map is now loaded.  Pick out what we need.
	MapMetaData md;
	md.m_fileName = lowerFname;
	// GeneralsX: Keep the real, case-intact path beside the lowercased
	// identity. See MapMetaData::m_filePathOnDisk for why this exists.
	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	md.m_filePathOnDisk = fname;
	// GX-SIMID-END
	md.m_filesize = fileInfo.sizeLow;
	md.m_isOfficial = isOfficial;
	md.m_doesExist = TRUE;
	md.m_waypoints.update();
	md.m_numPlayers = md.m_waypoints.m_numStartSpots;
	md.m_isMultiplayer = (md.m_numPlayers >= 2);
	md.m_timestamp.m_highTimeStamp = fileInfo.timestampHigh;
	md.m_timestamp.m_lowTimeStamp = fileInfo.timestampLow;
	md.m_supplyPositions = m_supplyPositions;
	md.m_techPositions = m_techPositions;
	md.m_CRC = mapCRC;

	Bool exists = false;
	AsciiString nameLookupTag = worldDict.getAsciiString(TheKey_mapName, &exists);
	md.m_nameLookupTag = nameLookupTag;

	if (!exists || nameLookupTag.isEmpty())
	{
		DEBUG_LOG(("Missing TheKey_mapName!"));
		AsciiString tempdisplayname;
		const char *displayNameStart = findLastPathSeparator(fname);
		tempdisplayname = displayNameStart ? displayNameStart + 1 : fname;
		md.m_displayName.translate(tempdisplayname);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
		TheGameText->reset();
	}
	else
	{
		AsciiString stringFileName = fname;
		stringFileName.truncateBy(4);
		const char *stringFileSeparator = findLastPathSeparator(stringFileName);
		if (stringFileSeparator && *stringFileSeparator == '/')
		{
			stringFileName.concat("/map.str");
		}
		else
		{
			stringFileName.concat("\\map.str");
		}
		TheGameText->initMapStringFile(stringFileName);
		md.m_displayName = TheGameText->fetch(nameLookupTag);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
		DEBUG_LOG(("Map name is now '%ls'", md.m_displayName.str()));
		TheGameText->reset();
	}

	getExtent(&(md.m_extent));

	// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
	(*this)[lowerFname] = md;
	// GX-SIMID-END

	DEBUG_LOG(("  filesize = %d bytes", md.m_filesize));
	DEBUG_LOG(("  displayName = %ls", md.m_displayName.str()));
	DEBUG_LOG(("  CRC = %X", md.m_CRC));
	DEBUG_LOG(("  timestamp = %d", md.m_timestamp));
	DEBUG_LOG(("  isOfficial = %s", (md.m_isOfficial)?"yes":"no"));

	DEBUG_LOG(("  isMultiplayer = %s", (md.m_isMultiplayer)?"yes":"no"));
	DEBUG_LOG(("  numPlayers = %d", md.m_numPlayers));

	DEBUG_LOG(("  extent = (%2.2f,%2.2f) -> (%2.2f,%2.2f)",
		md.m_extent.lo.x, md.m_extent.lo.y,
		md.m_extent.hi.x, md.m_extent.hi.y));

	Coord3D pos;
	WaypointMap::iterator itw = md.m_waypoints.begin();
	for (; itw != md.m_waypoints.end(); ++itw)
	{
		pos = itw->second;
		DEBUG_LOG(("    waypoint %s: (%2.2f,%2.2f)", itw->first.str(), pos.x, pos.y));
	}

	resetMap();

	return TRUE;
}

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
MapCache *TheMapCache = nullptr;
// GX-SIMID-END

// PUBLIC FUNCTIONS //////////////////////////////////////////////////////////////////////////////

Bool WouldMapTransfer( const AsciiString& mapName )
{
	return mapName.startsWithNoCase(TheMapCache->getUserMapDir());
}

//-------------------------------------------------------------------------------------------------
typedef std::set<UnicodeString, rts::less_than_nocase<UnicodeString>/**/> MapNameList;
typedef std::map<UnicodeString, AsciiString> MapDisplayToFileNameList;

static void buildMapListForNumPlayers(MapNameList &outMapNames, MapDisplayToFileNameList &outFileNames, Int numPlayers)
{
	MapCache::iterator it = TheMapCache->begin();

	for (; it != TheMapCache->end(); ++it)
	{
		const MapMetaData &mapData = it->second;
		if (mapData.m_numPlayers == numPlayers)
		{
			outMapNames.insert(it->second.m_displayName);
			outFileNames[it->second.m_displayName] = it->first;
		}
	}
}

//-------------------------------------------------------------------------------------------------
struct MapListBoxData
{
	MapListBoxData()
		: listbox(nullptr)
		, numLength(0)
		, numColumns(0)
		, w(10)
		, h(10)
		, color(GameMakeColor(255, 255, 255, 255))
		, battleHonors(nullptr)
		, easyImage(nullptr)
		, mediumImage(nullptr)
		, brutalImage(nullptr)
		, maxBrutalImage(nullptr)
		, mapToSelect()
		, selectionIndex(0) // always select *something*
		, isMultiplayer(false)
		, filter()
	{
	}

	GameWindow *listbox;
	Int numLength;
	Int numColumns;
	Int w;
	Int h;
	Color color;
	const SkirmishBattleHonors *battleHonors;
	const Image *easyImage;
	const Image *mediumImage;
	const Image *brutalImage;
	const Image *maxBrutalImage;
	AsciiString mapToSelect;
	Int selectionIndex;
	Bool isMultiplayer;
	UnicodeString filter;
};

//-------------------------------------------------------------------------------------------------
// GeneralsX: Case-insensitive substring test for the map display-name filter.
// UnicodeString offers compareNoCase / startsWithNoCase / endsWithNoCase but no case-insensitive
// substring search (find() takes a single WideChar), and a search box needs substring matching,
// so it is spelled out here. str() never returns nullptr, it returns a pointer to a null WideChar.
static Bool mapNameMatchesFilter(const UnicodeString& name, const UnicodeString& filter)
{
	if (filter.isEmpty())
		return TRUE;

	if (name.isEmpty())
		return FALSE;

	for (const WideChar *hay = name.str(); *hay; ++hay)
	{
		const WideChar *h = hay;
		const WideChar *n = filter.str();

		while (*n && *h && towlower(*h) == towlower(*n))
		{
			++h;
			++n;
		}

		if (*n == 0)
			return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
static Bool addMapToMapListbox(
	MapListBoxData& lbData,
	const AsciiString& mapDir,
	const AsciiString& mapName,
	const MapMetaData& mapMetaData)
{
	const Bool mapOk = mapName.startsWithNoCase(mapDir.str()) && lbData.isMultiplayer == mapMetaData.m_isMultiplayer && !mapMetaData.m_displayName.isEmpty()
		&& mapNameMatchesFilter(mapMetaData.m_displayName, lbData.filter);

	if (mapOk)
	{
		// GeneralsX: A full map listbox GROWS instead of ending the list.
		// The row capacity comes from the layout's LISTBOXDATA LENGTH inside WindowZH.big / Window.big
		// (retail: Skirmish 300, LAN and Online 100, solo 200; UI mods ship their own, e.g. 1200),
		// and the gadget refuses to append past it because AUTOPURGE is 0. Every map past the cap
		// simply vanished, and a selected map past it fell back to row 0. Raising it here instead of
		// in the .wnd keeps the retail layout untouched (a modified WindowZH.big would move dataID)
		// and covers every map menu and every UI mod at once. GadgetListBoxSetListLength keeps the
		// rows already added. ListboxData::listLength is a Short, hence the ceiling.
		if (GadgetListBoxGetNumEntries(lbData.listbox) >= lbData.numLength)
		{
			const Int maxListLength = 0x7FFF;
			if (lbData.numLength >= maxListLength)
				return false;
			lbData.numLength = min(maxListLength, max(16, lbData.numLength * 2));
			GadgetListBoxSetListLength(lbData.listbox, lbData.numLength);
		}

		UnicodeString mapDisplayName;
		/// @todo: mapDisplayName = TheGameText->fetch(mapMetaData.m_displayName.str());
		mapDisplayName = mapMetaData.m_displayName;

		Int index = -1;
		Int imageItemData = -1;
		if (lbData.numColumns > 1 && mapMetaData.m_isMultiplayer)
		{
			const Int numEasy = lbData.battleHonors->getEnduranceMedal(mapName.str(), SLOT_EASY_AI);
			const Int numMedium = lbData.battleHonors->getEnduranceMedal(mapName.str(), SLOT_MED_AI);
			const Int numBrutal = lbData.battleHonors->getEnduranceMedal(mapName.str(), SLOT_BRUTAL_AI);
			if (numBrutal)
			{
				const Int maxBrutalSlots = mapMetaData.m_numPlayers - 1;
				if (lbData.maxBrutalImage != nullptr && numBrutal == maxBrutalSlots)
				{
					index = GadgetListBoxAddEntryImage( lbData.listbox, lbData.maxBrutalImage, index, 0, lbData.w, lbData.h, TRUE);
					imageItemData = 4;
				}
				else
				{
					index = GadgetListBoxAddEntryImage( lbData.listbox, lbData.brutalImage, index, 0, lbData.w, lbData.h, TRUE);
					imageItemData = 3;
				}
			}
			else if (numMedium)
			{
				imageItemData = 2;
				index = GadgetListBoxAddEntryImage( lbData.listbox, lbData.mediumImage, index, 0, lbData.w, lbData.h, TRUE);
			}
			else if (numEasy)
			{
				imageItemData = 1;
				index = GadgetListBoxAddEntryImage( lbData.listbox, lbData.easyImage, index, 0, lbData.w, lbData.h, TRUE);
			}
			else
			{
				imageItemData = 0;
				index = GadgetListBoxAddEntryImage( lbData.listbox, nullptr, index, 0, lbData.w, lbData.h, TRUE);
			}
		}

		index = GadgetListBoxAddEntryText( lbData.listbox, mapDisplayName, lbData.color, index, lbData.numColumns-1 );
		DEBUG_ASSERTCRASH(index >= 0, ("Expects valid index"));

		if (mapName == lbData.mapToSelect)
		{
			lbData.selectionIndex = index;
		}

		// now set the char* as the item data.  this works because the map cache isn't being
		// modified while a map listbox is up.
		GadgetListBoxSetItemData( lbData.listbox, (void *)(mapName.str()), index );

		if (lbData.numColumns > 1)
		{
			GadgetListBoxSetItemData( lbData.listbox, reinterpret_cast<void*>(std::uintptr_t(imageItemData)), index, 1 );
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------------------
static Bool addMapCollectionToMapListbox(
	MapListBoxData& lbData,
	const AsciiString& mapDir,
	const MapNameList& mapNames,
	const MapDisplayToFileNameList& fileNames)
{
	MapNameList::const_iterator mapNameIt = mapNames.begin();

	for (; mapNameIt != mapNames.end(); ++mapNameIt)
	{
		MapDisplayToFileNameList::const_iterator fileNameIt = fileNames.find(*mapNameIt);
		DEBUG_ASSERTCRASH(fileNameIt != fileNames.end(), ("Map '%s' not found in file names map", mapNameIt->str()));

		const AsciiString& asciiMapName = fileNameIt->second;

#if RTS_ZEROHOUR
		//Patch 1.03 -- Purposely filter out these broken maps that exist in Generals.
		if( !asciiMapName.compare( "maps\\armored fury\\armored fury.map" ) ||
			!asciiMapName.compare( "maps\\scorched earth\\scorched earth.map" ) )
		{
			continue;
		}
#endif

		MapCache::iterator mapCacheIt = TheMapCache->find(asciiMapName);
		DEBUG_ASSERTCRASH(mapCacheIt != TheMapCache->end(), ("Map '%s' not found in map cache.", mapNameIt->str()));
		/*
		if (it != TheMapCache->end())
		{
			DEBUG_LOG(("populateMapListbox(): looking at %s (displayName = %ls), mp = %d (== %d?) mapDir=%s (ok=%d)",
				it->first.str(), it->second.m_displayName.str(), it->second.m_isMultiplayer, isMultiplayer,
				mapDir.str(), it->first.startsWith(mapDir.str())));
		}
		*/

		const Bool ok = addMapToMapListbox(lbData, mapDir, mapCacheIt->first, mapCacheIt->second);

		if (!ok)
			return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------------------
/** Load the listbox with all the map files available to play */
//-------------------------------------------------------------------------------------------------
Int populateMapListboxNoReset( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect,
	UnicodeString filter )
{
	if(!TheMapCache)
		return -1;

	if (!listbox)
		return -1;

	MapListBoxData lbData;
	lbData.listbox = listbox;
	lbData.numLength = GadgetListBoxGetListLength( listbox );
	lbData.numColumns = GadgetListBoxGetNumColumns( listbox );
	lbData.mapToSelect = mapToSelect;
	lbData.isMultiplayer = isMultiplayer;
	lbData.filter = filter;

	if (lbData.numColumns > 1)
	{
		lbData.easyImage = TheMappedImageCollection->findImageByName("Star-Bronze");
		lbData.mediumImage = TheMappedImageCollection->findImageByName("Star-Silver");
		lbData.brutalImage = TheMappedImageCollection->findImageByName("Star-Gold");
		lbData.maxBrutalImage = TheMappedImageCollection->findImageByName("RedYell_Star");
		lbData.battleHonors = new SkirmishBattleHonors;

		lbData.w = lbData.brutalImage ? lbData.brutalImage->getImageWidth() : 10;
		lbData.w = min(GadgetListBoxGetColumnWidth(listbox, 0), lbData.w);
		lbData.h = lbData.w;
	}

	AsciiString mapDir;
	if (useSystemMaps)
	{
		mapDir = TheMapCache->getMapDir();
	}
	else
	{
		mapDir = TheGlobalData->getPath_UserData();
		mapDir.concat(TheMapCache->getMapDir());
	}
	mapDir.toLower();

	MapNameList mapNames;
	MapDisplayToFileNameList fileNames;
	Int curNumPlayersInMap = 1;

	// GeneralsX: Lay the rows out once at the end instead of after every
	// add: the list is no longer capped at the layout's length, and per-add layout is O(maps^2).
	GadgetListBoxBeginBulkAdd(listbox);

	for (; curNumPlayersInMap <= MAX_SLOTS; ++curNumPlayersInMap)
	{
		buildMapListForNumPlayers(mapNames, fileNames, curNumPlayersInMap);

		const Bool ok = addMapCollectionToMapListbox(lbData, mapDir, mapNames, fileNames);

		mapNames.clear();
		fileNames.clear();

		if (!ok)
			break;
	}

	GadgetListBoxEndBulkAdd(listbox);

	delete lbData.battleHonors;
	lbData.battleHonors = nullptr;

	GadgetListBoxSetSelected(listbox, &lbData.selectionIndex, 1);

	if (lbData.selectionIndex >= 0)
	{
		Int topIndex = GadgetListBoxGetTopVisibleEntry(listbox);
		Int bottomIndex = GadgetListBoxGetBottomVisibleEntry(listbox);
		Int rowsOnScreen = bottomIndex - topIndex;

		if (lbData.selectionIndex >= bottomIndex)
		{
			Int newTop = max( 0, lbData.selectionIndex - max( 1, rowsOnScreen / 2 ) );
			//The trouble is that rowsOnScreen/2 can be zero if bottom is 1 and top is zero
			GadgetListBoxSetTopVisibleEntry( listbox, newTop );
		}
	}

	return lbData.selectionIndex;
}

//-------------------------------------------------------------------------------------------------
/** Load the listbox with all the map files available to play */
//-------------------------------------------------------------------------------------------------
Int populateMapListbox( GameWindow *listbox, Bool useSystemMaps, Bool isMultiplayer, AsciiString mapToSelect,
	UnicodeString filter )
{
	if(!TheMapCache)
		return -1;

	if (!listbox)
		return -1;

	// reset the listbox content
	GadgetListBoxReset( listbox );

	return populateMapListboxNoReset( listbox, useSystemMaps, isMultiplayer, mapToSelect, filter );
}



//-------------------------------------------------------------------------------------------------
/** Validate a map */
//-------------------------------------------------------------------------------------------------
Bool isValidMap( AsciiString mapName, Bool isMultiplayer )
{
	if(!TheMapCache || mapName.isEmpty())
		return FALSE;
	TheMapCache->updateCache();

	mapName.toLower();
	MapCache::iterator it = TheMapCache->find(mapName);
	if (it != TheMapCache->end())
	{
		if (isMultiplayer == it->second.m_isMultiplayer)
		{
			return TRUE;
		}
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Find a valid map */
//-------------------------------------------------------------------------------------------------
AsciiString getDefaultMap( Bool isMultiplayer )
{
	if(!TheMapCache)
		return AsciiString::TheEmptyString;
	TheMapCache->updateCache();

	MapCache::iterator it = TheMapCache->begin();
	for (; it != TheMapCache->end(); ++it)
	{
		if (isMultiplayer == it->second.m_isMultiplayer)
		{
			return it->first;
		}
	}

	return AsciiString::TheEmptyString;
}


AsciiString getDefaultOfficialMap()
{
	if(!TheMapCache)
		return AsciiString::TheEmptyString;
	TheMapCache->updateCache();

	MapCache::iterator it = TheMapCache->begin();
	for (; it != TheMapCache->end(); ++it)
	{
		if (it->second.m_isMultiplayer && it->second.m_isOfficial)
		{
			return it->first;
		}
	}
	return AsciiString::TheEmptyString;
}


Bool isOfficialMap( AsciiString mapName )
{
	if(!TheMapCache || mapName.isEmpty())
		return FALSE;
	TheMapCache->updateCache();
	mapName.toLower();
	MapCache::iterator it = TheMapCache->find(mapName);
	if (it != TheMapCache->end())
		return it->second.m_isOfficial;
	return FALSE;
}


const MapMetaData *MapCache::findMap(AsciiString mapName)
{
	mapName.toLower();
	MapCache::iterator it = find(mapName);
	if (it == end())
		return nullptr;
	return &(it->second);
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: The map's sides, counted the way the start path sees them.
//
// Reads the "SidesList" chunk the same way SidesList::ParseSidesDataChunk does (it caps the list at
// MAX_PLAYER_COUNT and skips each side's build list), keeps only what the capacity rule needs, and
// leaves TheSidesList alone - the shell map may be running under this lobby. Everything after the
// sides (teams, scripts) is skipped by DataChunkInput when the parser returns.
// ------------------------------------------------------------------------------------------------
namespace
{
struct MapSidesForCapacity
{
	Int numSides;
	Bool isCivilian[MAX_PLAYER_COUNT];
	Bool isNamed[MAX_PLAYER_COUNT];
	Bool sawSides;
};

const Int GX_SIDES_DATA_VERSION_3 = 3;	// SidesList.cpp K_SIDES_DATA_VERSION_3: build list entries grow five fields

Bool parseSidesForCapacity(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
	MapSidesForCapacity *out = (MapSidesForCapacity *)userData;
	out->sawSides = TRUE;
	out->numSides = 0;
	const Int count = file.readInt();
	for (Int i = 0; i < count; ++i)
	{
		if (i >= MAX_PLAYER_COUNT)
			break;
		Dict d = file.readDict();
		out->isCivilian[out->numSides] = (d.getAsciiString(TheKey_playerFaction) == "FactionCivilian");
		out->isNamed[out->numSides] = d.getAsciiString(TheKey_playerName).isNotEmpty();
		++out->numSides;
		const Int buildCount = file.readInt();
		for (Int j = 0; j < buildCount; ++j)
		{
			file.readAsciiString();	// building name
			file.readAsciiString();	// template name
			file.readReal(); file.readReal(); file.readReal();	// location
			file.readReal();	// angle
			file.readByte();	// initially built
			file.readInt();	// rebuilds
			if (info->version >= GX_SIDES_DATA_VERSION_3)
			{
				file.readAsciiString();	// script
				file.readInt();	// health
				file.readByte(); file.readByte(); file.readByte();	// whiner, unsellable, repairable
			}
		}
	}
	return true;
}
}

Int MapCache::getPlayerSlotCapacity(const AsciiString &mapName)
{
	const MapMetaData *md = findMap(mapName);
	if (md == nullptr)
		return -1;

	// Keyed on the map and its CRC: the host re-presses Start, the file does not change under it.
	static std::map<AsciiString, std::pair<UnsignedInt, Int> > s_known;
	AsciiString key = mapName;
	key.toLower();
	std::map<AsciiString, std::pair<UnsignedInt, Int> >::const_iterator hit = s_known.find(key);
	if (hit != s_known.end() && hit->second.first == md->m_CRC)
		return hit->second.second;

	MapSidesForCapacity sides;
	memset(&sides, 0, sizeof(sides));
	Int capacity = -1;
	CachedFileInputStream fileStrm;
	if (fileStrm.open(getPathOnDisk(key)))
	{
		ChunkInputStream *pStrm = &fileStrm;
		DataChunkInput file(pStrm);
		file.registerParser("SidesList", AsciiString::TheEmptyString, parseSidesForCapacity);
		// GeneralsX: The chunk readers throw ERROR_CORRUPT_FILE_FORMAT on a damaged file
		// (DataChunk.cpp readChunkBytes), and this runs from the lobby's Start buttons and HeadlessMatch, outside any
		// handler. A damaged map's capacity is unknown (-1, as for a map that does not open); the start then refuses
		// the map itself (GameLogic::startNewGame).
		Bool parsed = FALSE;
		try
		{
			parsed = file.parse(&sides);
		}
		catch (ErrorCode)
		{
			fprintf(stderr, "[GXCAP] map \"%s\": damaged SidesList, capacity unknown\n", key.str());
			parsed = FALSE;
		}
		if (parsed && sides.sawSides)
		{
			const KeptMapSides kept = keepMapSidesForMultiplayer(sides.numSides, sides.isCivilian, sides.isNamed);
			capacity = playerSlotCapacity(kept);
			fprintf(stderr, "[GXCAP] map \"%s\": %d side(s), keeps %d (%d named) -> %d lobby slot(s) fit\n",
				key.str(), sides.numSides, kept.total, kept.named, capacity);
		}
	}
	s_known[key] = std::make_pair(md->m_CRC, capacity);
	return capacity;
}

// ------------------------------------------------------------------------------------------------
/** Embed the pristine map into the xfer stream */
// ------------------------------------------------------------------------------------------------
static Bool copyFromBigToDir( const AsciiString& infile, const AsciiString& outfile )
{
	File *file = TheFileSystem->openFile( infile.str(), File::READ | File::BINARY );
	if( file == nullptr )
	{
		DEBUG_CRASH(( "copyFromBigToDir - Error opening source file '%s'", infile.str() ));
		return FALSE;
	}

	const Int fileSize = file->seek( 0, File::END );
	file->seek( 0, File::START );

	// GeneralsX: A buffer that cannot be allocated is a skipped preview, with the source
	// file closed. The engine allocator throws ERROR_OUT_OF_MEMORY instead of returning null (GameMemory.cpp), and a
	// failed seek returns a negative size (#3485 had taken out the catch that covered both).
	char *buffer = nullptr;
	if( fileSize >= 0 )
	{
		try
		{
			buffer = NEW char[ fileSize ];
		}
		catch( ... )
		{
			buffer = nullptr;
		}
	}
	if( buffer == nullptr )
	{
		file->close();
		DEBUG_CRASH(( "copyFromBigToDir - Unable to allocate buffer for file '%s'", infile.str() ));
		return FALSE;
	}

	const Bool readOk = file->read( buffer, fileSize ) == fileSize;
	file->close();
	DEBUG_ASSERTCRASH( readOk, ( "copyFromBigToDir - Error reading from file '%s'", infile.str() ) );

	Bool writeOk = FALSE;
	if( readOk )
	{
		// GeneralsX: Opening the target allocates and can throw too: free the buffer before
		// the call site's catch turns it into a skipped preview.
		try
		{
			File *filenew = TheFileSystem->openFile( outfile.str(), File::WRITE | File::CREATE | File::BINARY );
			if( filenew != nullptr )
			{
				writeOk = filenew->write( buffer, fileSize ) == fileSize;
				filenew->close();
			}
		}
		catch( ... )
		{
			delete [] buffer;
			throw;
		}
		DEBUG_ASSERTCRASH( writeOk, ( "copyFromBigToDir - Error writing to file '%s'", outfile.str() ) );
	}

	delete [] buffer;

	return writeOk;
}

Image *getMapPreviewImage( AsciiString mapName )
{
	if(!TheGlobalData)
		return nullptr;
	DEBUG_LOG(("%s Map Name", mapName.str()));
	AsciiString tgaName = mapName;
	AsciiString name;
	AsciiString tempName;
	tgaName.truncateBy(4); // ".map"
	name = tgaName;
	tgaName.concat(".tga");

	AsciiString portableName = TheGameState->realMapPathToPortableMapPath(name);
	tempName.set(AsciiString::TheEmptyString);
	for(Int i = 0; i < portableName.getLength(); ++i)
	{
		char c = portableName.getCharAt(i);
		if (c == '\\' || c == ':')
			tempName.concat('_');
		else
			tempName.concat(c);
	}

	name = tempName;
	name.concat(".tga");


	// copy file over
	// copy source tgaName, to name

	Image *image = (Image *)TheMappedImageCollection->findImageByName(tempName);
	if(!image)
	{

		if(!TheFileSystem->doesFileExist(tgaName.str()))
			return nullptr;
		AsciiString mapPreviewDir;
		mapPreviewDir.format(MAP_PREVIEW_DIR_PATH, TheGlobalData->getPath_UserData().str());
		TheFileSystem->createDirectory(mapPreviewDir);

		mapPreviewDir.concat(name);

		// GeneralsX: Any other throw on the way (opening the target allocates too) stays a
		// skipped preview, as it was before #3485 took the catch out.
		Bool success = FALSE;
		try
		{
			success = copyFromBigToDir(tgaName, mapPreviewDir);
		}
		catch (...)
		{
			success = FALSE;	// no rethrow
		}

		if (success)
		{
    	image = newInstance(Image);
			image->setName(tempName);
			//image->setFullPath("mission.tga");
			image->setFilename(name);
			image->setStatus(IMAGE_STATUS_NONE);
			Region2D uv;
			uv.hi.x = 1.0f;
			uv.hi.y = 1.0f;
			uv.lo.x	= 0.0f;
			uv.lo.y = 0.0f;
			image->setUV(&uv);
			image->setTextureHeight(128);
			image->setTextureWidth(128);
			TheMappedImageCollection->addImage(image);
		}
		else
		{
			image = nullptr;
		}
	}

	return image;



/*
	// sanity
	if( mapName.isEmpty() )
		return nullptr;
	Region2D uv;
	mapPreviewImage = TheMappedImageCollection->findImageByName("MapPreview");
	if(mapPreviewImage)
		deleteInstance(mapPreviewImage);

	mapPreviewImage = TheMappedImageCollection->newImage();
	mapPreviewImage->setName("MapPreview");
	mapPreviewImage->setStatus(IMAGE_STATUS_RAW_TEXTURE);
// allocate our terrain texture
	TextureClass * texture = new TextureClass( size.x, size.y,
																			 WW3D_FORMAT_X8R8G8B8, MIP_LEVELS_1 );
	uv.lo.x = 0.0f;
	uv.lo.y = 1.0f;
	uv.hi.x = 1.0f;
	uv.hi.y = 0.0f;
	mapPreviewImage->setStatus( IMAGE_STATUS_RAW_TEXTURE );
	mapPreviewImage->setRawTextureData( texture );
	mapPreviewImage->setUV( &uv );
	mapPreviewImage->setTextureWidth( size.x );
	mapPreviewImage->setTextureHeight( size.y );
	mapPreviewImage->setImageSize( &size );


	CachedFileInputStream theInputStream;
	if (theInputStream.open(AsciiString(mapName.str())))
	{
		ChunkInputStream *pStrm = &theInputStream;
		pStrm->absoluteSeek(0);
		DataChunkInput file( pStrm );
		if (file.isValidFileType()) {	// Backwards compatible files aren't valid data chunk files.
			// Read the waypoints.
			file.registerParser( "MapPreview", AsciiString::TheEmptyString, parseMapPreviewChunk );
			if (!file.parse(nullptr)) {
				DEBUG_CRASH(("Unable to read MapPreview info."));
				deleteInstance(mapPreviewImage);
				return nullptr;
			}
		}
		theInputStream.close();
	}
	else
	{
		deleteInstance(mapPreviewImage);
		return nullptr;
	}


	return mapPreviewImage;

*/
	return nullptr;
}

Bool parseMapPreviewChunk(DataChunkInput &file, DataChunkInfo *info, void *userData)
{
/*
	ICoord2D size;

	SurfaceClass *surface;
	size.x = file.readInt();
	size.y = file.readInt();


	surface = (TextureClass *)mapPreviewImage->getRawTextureData()->Get_Surface_Level();
	//texture->Get_Surface_Level();

	DEBUG_LOG(("BeginMapPreviewInfo"));
	int pitch;
	void *pBits = surface->Lock(&pitch);
	const unsigned int bytesPerPixel = surface->Get_Bytes_Per_Pixel();
	UnsignedInt *buffer = new UnsignedInt[size.x * size.y];
	Int x,y;
	for (y=0; y<size.y; y++) {
		for(x = 0; x< size.x; x++)
		{
			surface->Draw_Pixel( x, y, file.readInt(), bytesPerPixel, pBits, pitch );
			buffer[y + x] = file.readInt();
			DEBUG_LOG(("x:%d, y:%d, %X", x, y, buffer[y + x]));
		}
	}
	mapPreviewImage->setRawTextureData(buffer);
	DEBUG_ASSERTCRASH(file.atEndOfChunk(), ("Unexpected data left over."));
	DEBUG_LOG(("EndMapPreviewInfo"));
	surface->Unlock();
	REF_PTR_RELEASE(surface);
	return true;
*/
	return FALSE;
}

void findDrawPositions( Int startX, Int startY, Int width, Int height, Region3D extent,
															 ICoord2D *ul, ICoord2D *lr )
{

	Real ratioWidth;
	Real ratioHeight;
	Coord2D radar;
	ratioWidth = extent.width()/(width * 1.0f);
	ratioHeight = extent.height()/(height* 1.0f);

	if( ratioWidth >= ratioHeight)
	{
		radar.x = extent.width() / ratioWidth;
		radar.y = extent.height()/ ratioWidth;
		ul->x = 0;
		ul->y = (height - radar.y) / 2.0f;
		lr->x = radar.x;
		lr->y = height - ul->y;
	}
	else
	{
		radar.x = extent.width() / ratioHeight;
		radar.y = extent.height()/ ratioHeight;
		ul->x = (width - radar.x ) / 2.0f;
		ul->y = 0;
		lr->x = width - ul->x;
		lr->y = radar.y;
	}

	// make them pixel positions
	ul->x += startX;
	ul->y += startY;
	lr->x += startX;
	lr->y += startY;

}

