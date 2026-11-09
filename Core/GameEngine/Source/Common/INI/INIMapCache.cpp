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

// FILE: INIMapCache.cpp ///////////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell, February 2002
// Desc:   Parsing MapCache INI entries
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Lib/BaseType.h"
#include "Common/INI.h"
#include "Common/FileSystem.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameText.h"
#include "GameNetwork/NetworkDefs.h"
#include "Common/NameKeyGenerator.h"
#include "Common/WellKnownKeys.h"
#include "Common/QuotedPrintable.h"

// GeneralsX @bugfix Copilot 27/04/2026 Support both POSIX and Windows separators when deriving map display names.
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


class MapMetaDataReader
{
public:
	Region3D m_extent;
	Int m_numPlayers;
	Bool m_isMultiplayer;
	AsciiString m_asciiDisplayName;
	AsciiString m_asciiNameLookupTag;

	Bool m_isOfficial;
	WinTimeStamp m_timestamp;
	UnsignedInt m_filesize;
	UnsignedInt m_CRC;

	Coord3D m_waypoints[MAX_SLOTS];
	Coord3D m_initialCameraPosition;
	Coord3DList m_supplyPositions;
	Coord3DList m_techPositions;
	static const FieldParse m_mapFieldParseTable[];		///< the parse table for INI definition
	const FieldParse *getFieldParse() const { return m_mapFieldParseTable; }
};


void parseSupplyPositionCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d,nullptr );
	mmdr->m_supplyPositions.push_front(coord3d);

}

void parseTechPositionsCoord3D( INI* ini, void * instance, void * /*store*/, const void* /*userData*/ )
{
	MapMetaDataReader *mmdr = (MapMetaDataReader *)instance;
	Coord3D coord3d;
	INI::parseCoord3D(ini, nullptr, &coord3d,nullptr );
	mmdr->m_techPositions.push_front(coord3d);

}

const FieldParse MapMetaDataReader::m_mapFieldParseTable[] =
{

	{ "isOfficial",							INI::parseBool,			nullptr,	offsetof( MapMetaDataReader, m_isOfficial ) },
	{ "isMultiplayer",					INI::parseBool,			nullptr,	offsetof( MapMetaDataReader, m_isMultiplayer ) },
	{ "extentMin",							INI::parseCoord3D,	nullptr, offsetof( MapMetaDataReader, m_extent.lo ) },
	{ "extentMax",							INI::parseCoord3D,	nullptr, offsetof( MapMetaDataReader, m_extent.hi ) },
	{ "numPlayers",							INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_numPlayers ) },
	{ "fileSize",								INI::parseUnsignedInt,	nullptr,	offsetof( MapMetaDataReader, m_filesize ) },
	{ "fileCRC",								INI::parseUnsignedInt,	nullptr,	offsetof( MapMetaDataReader, m_CRC ) },
	{ "timestampLo",						INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_timestamp.m_lowTimeStamp ) },
	{ "timestampHi",						INI::parseInt,			nullptr,	offsetof( MapMetaDataReader, m_timestamp.m_highTimeStamp ) },
	{ "displayName",						INI::parseAsciiString,	nullptr,	offsetof( MapMetaDataReader, m_asciiDisplayName ) },
	{ "nameLookupTag",					INI::parseAsciiString,	nullptr,	offsetof( MapMetaDataReader, m_asciiNameLookupTag ) },

	{ "supplyPosition",					parseSupplyPositionCoord3D,	nullptr, 0 },
	{ "techPosition",						parseTechPositionsCoord3D,	nullptr, 0 },

	{ "Player_1_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) },
	{ "Player_2_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 1 },
	{ "Player_3_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 2 },
	{ "Player_4_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 3 },
	{ "Player_5_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 4 },
	{ "Player_6_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 5 },
	{ "Player_7_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 6 },
	{ "Player_8_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 7 },
	// GeneralsX @feature Claude 12/08/2026 MAX_SLOTS is 12; this table must cover every slot or
	// a cached 9..12-start map silently loses its upper start positions on reload.
	{ "Player_9_Start",					INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 8 },
	{ "Player_10_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 9 },
	{ "Player_11_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 10 },
	{ "Player_12_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 11 },
	// GeneralsX @feature Claude 06/11/2026 MAX_SLOTS 34: every slot's start, or a 13..34-start map loses them on reload
	// (and 13/14 never had them: the table stopped at 12 when MAX_SLOTS went to 14).
	{ "Player_13_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 12 },
	{ "Player_14_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 13 },
	{ "Player_15_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 14 },
	{ "Player_16_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 15 },
	{ "Player_17_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 16 },
	{ "Player_18_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 17 },
	{ "Player_19_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 18 },
	{ "Player_20_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 19 },
	{ "Player_21_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 20 },
	{ "Player_22_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 21 },
	{ "Player_23_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 22 },
	{ "Player_24_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 23 },
	{ "Player_25_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 24 },
	{ "Player_26_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 25 },
	{ "Player_27_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 26 },
	{ "Player_28_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 27 },
	{ "Player_29_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 28 },
	{ "Player_30_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 29 },
	{ "Player_31_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 30 },
	{ "Player_32_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 31 },
	{ "Player_33_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 32 },
	{ "Player_34_Start",				INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_waypoints ) + sizeof(Coord3D) * 33 },

	{ "InitialCameraPosition",	INI::parseCoord3D,	nullptr,	offsetof( MapMetaDataReader, m_initialCameraPosition ) },

	{ nullptr,					nullptr,						nullptr,						0 }

};

// GeneralsX @probe Claude 19/08/2026 GX_SKIRMISH - shared gate shape with SidesList.cpp.
static Bool gxMapDiag(void)
{
	static Int s_on = -1;
	if (s_on < 0)
	{
		const char *env = getenv("GX_SKIRMISH");
		s_on = (env != NULL && atoi(env) > 0) ? 1 : 0;
	}
	return s_on != 0;
}

void INI::parseMapCacheDefinition( INI* ini )
{
	const char *c;
	AsciiString name;
	// GeneralsX @bugfix MapMetaDataReader has no constructor, so its scalar members started out
	// indeterminate, and initFromINI() only DEBUG_CRASHes on an unknown or missing field - which
	// compiles away in release, leaving the block parsed but the member never written. MapCache.ini
	// lives in a writable user directory (the app sandbox on iOS), so a truncated or foreign-written
	// entry that omits e.g. numPlayers used to hand us garbage that was copied straight into the map
	// cache and used as a loop bound below. Value-initialise the reader so a missing field means
	// zero instead of stack garbage.
	MapMetaDataReader mdr = {};
	MapMetaData md;

	// read the name
	c = ini->getNextToken(" \n\r\t");
	name.set( c );
	name = QuotedPrintableToAsciiString(name);
	md.m_waypoints.clear();

	ini->initFromINI( &mdr, mdr.getFieldParse() );

	md.m_extent = mdr.m_extent;
	md.m_isOfficial = mdr.m_isOfficial != 0;
	md.m_doesExist = TRUE;
	md.m_isMultiplayer = mdr.m_isMultiplayer != 0;
	// GeneralsX @bugfix numPlayers comes from the cache file, but nothing validated it on the way in.
	// MapCache::writeCacheINI only ever emits a count that WaypointMap::update() clamped to
	// [1, MAX_SLOTS], so clamping here cannot change any cache file we wrote ourselves; it stops a
	// hand-edited or corrupted entry from claiming more start positions than exist. The count is
	// read past the end of the 8-entry mdr.m_waypoints array below, and is also used as a slot-array
	// bound by the lobby screens that consume MapMetaData, so an out-of-range value corrupts memory
	// well beyond this function.
	md.m_numPlayers = mdr.m_numPlayers;
	if (md.m_numPlayers > MAX_SLOTS)
	{
		md.m_numPlayers = MAX_SLOTS;
	}
	md.m_filesize = mdr.m_filesize;
	md.m_CRC = mdr.m_CRC;
	md.m_timestamp = mdr.m_timestamp;

	md.m_waypoints[TheNameKeyGenerator->keyToName(TheKey_InitialCameraPosition)] = mdr.m_initialCameraPosition;

#if RTS_GENERALS
	md.m_displayName = QuotedPrintableToUnicodeString(mdr.m_asciiDisplayName);
#else
	md.m_nameLookupTag = QuotedPrintableToAsciiString(mdr.m_asciiNameLookupTag);

	if (md.m_nameLookupTag.isEmpty())
	{
		// maps without localized name tags
		AsciiString tempdisplayname;
		const char *displayNameStart = findLastPathSeparator(name);
		tempdisplayname = displayNameStart ? displayNameStart + 1 : name;
		md.m_displayName.translate(tempdisplayname);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
	}
	else
	{
		// official maps with name tags
		md.m_displayName = TheGameText->fetch(md.m_nameLookupTag);
		if (md.m_numPlayers >= 2)
		{
			UnicodeString extension;
			extension.format(L" (%d)", md.m_numPlayers);
			md.m_displayName.concat(extension);
		}
	}
#endif

	AsciiString startingCamName;
	// GeneralsX @bugfix keep the read of mdr.m_waypoints bounded by the array it indexes, not only by
	// the (now clamped) player count, so this loop stays safe if the count above is ever widened.
	for (Int i=0; i<md.m_numPlayers && i<MAX_SLOTS; ++i)
	{
		startingCamName.format("Player_%d_Start", i+1); // start pos waypoints are 1-based
		md.m_waypoints[startingCamName] = mdr.m_waypoints[i];
	}

	Coord3DList::iterator it = mdr.m_supplyPositions.begin();
	while( it != mdr.m_supplyPositions.end())
	{
		md.m_supplyPositions.push_front(*it);
		it++;
	}

	it = mdr.m_techPositions.begin();
	while( it != mdr.m_techPositions.end())
	{
		md.m_techPositions.push_front(*it);
		it++;
	}

	if(TheMapCache && !md.m_displayName.isEmpty())
	{
		AsciiString lowerName = name;
		lowerName.toLower();
		md.m_fileName = lowerName;

		// GeneralsX @bugfix Claude 19/08/2026 DROP CACHE ENTRIES WHOSE FILE IS NO LONGER THERE.
		//
		// MapCache.ini keys a user map by its ABSOLUTE path, and on iOS that path contains the app's
		// data-container UUID - which iOS reassigns on every reinstall. Measured 2026-08-19 on the
		// iPad: all 15 cached entries sat under container 6723c9a0-... while the live container was
		// 51f6c02b-..., so every lookup missed and the lobby refused to start with "is not in the map
		// cache" for a map the directory scan had just listed as available one line earlier.
		//
		// Keeping a dead entry is never useful: the cache exists to avoid re-reading a map file that
		// IS there. Dropping it costs one rescan and lets the scan re-add the map under its current,
		// real path. Self-healing beats a stale entry that shadows a perfectly good map.
		//
		// Deliberately NOT re-keying the cache on a container-relative path, which is the tempting
		// larger fix: this key is also the multiplayer map identity (GameInfo::setMap ->
		// percentEncodeMapName), so changing its shape changes what peers compare and is an
		// all-peers-at-once contract change, not a local repair.
		// GeneralsX @bugfix Claude 20/08/2026 THIS PRUNE CANNOT BE DONE HERE, AND DOES NOT NEED TO BE.
		//
		// It used to call doesFileExist(lowerName) and drop the entry when that failed. On iOS
		// lowerName is exactly the path this file's own note establishes cannot be resolved - the
		// container UUID is uppercase on a case-sensitive volume - so the test failed for VALID
		// current entries just as it did for stale ones. Every launch therefore discarded the whole
		// user-map cache, re-parsed every map and recomputed every CRC, which is the opposite of
		// what a cache is for and makes startup scale with the size of the collection.
		//
		// The self-healing the original note wanted now lives in two places that can actually tell a
		// stale entry from a live one, because this one could not:
		//
		//   MapCache::clearMapsOutsideRoot erases user-map entries that are not under the current
		//   user map root - i.e. written under a previous container UUID. clearUnseenMaps CANNOT do
		//   that, and an earlier revision of this comment wrongly said it could: it and
		//   prepareUnseenMaps both only inspect keys starting with the root they are given, so an
		//   old-UUID key is never examined by either, and md.m_doesExist = TRUE above means it would
		//   survive anyway. Caught by a cross-model review of that revision.
		//
		//   MapCache::addMap decides the live ones, from the directory scan's own results, and now
		//   also stores the case-intact path on a cache HIT - which the path fix depends on and did
		//   not get before, because this prune was keeping the iOS cache permanently cold and hiding
		//   the gap.
		//
		// The trace stays, because a prune that never fires and one that fires on everything look
		// identical from the outside and that ambiguity has already cost a build cycle here.
		if (gxMapDiag())
		{
			fprintf(stderr, "[MAPDIAG] cache entry KEEP '%s' (existence decided by the scan)\n",
				lowerName.str());
			fflush(stderr);
		}

//		DEBUG_LOG(("INI::parseMapCacheDefinition - adding %s to map cache", lowerName.str()));
		(*TheMapCache)[lowerName] = md;
	}
}

