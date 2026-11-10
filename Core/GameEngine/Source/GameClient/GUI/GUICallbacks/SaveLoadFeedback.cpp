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

#include "PreRTS.h"
#include "GameClient/GameText.h"
#include "GameClient/InGameUI.h"
#include "GameClient/MessageBox.h"
#include "GameClient/SaveLoadFeedback.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Diagnostic/SimulationId.h"
#include "GameClient/MapUtil.h"	// TheMapCache
#include "GameClient/ParticleSys.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"
#include "gitinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <utility>
#include <vector>

bool (*TheGXCrashLogWriter)( const char *reason, void *const *frames, int n ) = nullptr;

static UnicodeString getUnicodeSavePath( const AsciiString &filename )
{
	UnicodeString path;
	path.translate( TheGameState->getFilePathInSaveDirectory(filename) );
	return path;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: Which GeneralsX build wrote a save: a trailer after SG_EOF.
//
// Layout, appended once the save is complete: identity bytes, u16 little-endian length, "GXBUILD1".
// Identity is the full commit (GitSHA1), plus '~' for a build with uncommitted changes.
//
// Why AFTER SG_EOF: every loader stops at that token (GameState::xferSaveData) or earlier
// (getSaveGameInfoFromFile stops at the first block, the pre-check below at CHUNK_GameStateMap), so the
// trailer is invisible to retail and to every older GeneralsX - a new save still loads there. It
// travels with the file when a save is copied between machines, which is exactly the cross-build case;
// a sidecar file would not. And it cannot go stale: any build that rewrites the save rewrites the whole
// file, so an older build's rewrite simply has no trailer and reads as "unknown build" - a warning, as
// the owner asked for saves with no identity. A chunk registered with GameState would load just as well
// everywhere, but registering one means editing GameState.cpp, which moves the SimID digest.
// Written from presentSaveResult, the one place every save route reports through.
// ------------------------------------------------------------------------------------------------
static const char s_trailerMagic[8] = { 'G','X','B','U','I','L','D','1' };
enum { TRAILER_FIXED = 2 + 8 };

static AsciiString thisBuildIdentity()
{
	AsciiString id( GitSHA1 );
	if( GitUncommittedChanges )
		id.concat( '~' );
	// The commit alone does not fix the save format: build switches (RETAIL_COMPATIBLE_XFER_SAVE and the
	// rest engineID hashes) and uncommitted simulation edits (sourceID folds the dirty files in) change
	// what the xfer code writes at the same commit - and the same executable over other data (a mod, an
	// edited INI) has other templates for the save to name. dataID is the value the join check refuses on
	// (SimIdCompare), taken once at startup from the mounted data - cwd-relative, as a join is - so a save
	// is "this build" exactly where a match between the two would be allowed. '/f' + all three, mixed;
	// every platform agrees on them, as they must for a four-platform match.
	const SimulationId &sim = SimulationId::get();
	if( sim.valid )
	{
		char format[16];
		snprintf( format, sizeof( format ), "/f%08x",
			(unsigned)( ( sim.engineID * 0x9E3779B1u ^ sim.sourceID ) * 0x9E3779B1u ^ sim.dataID ) );
		id.concat( format );
	}
	// A save's text is WideChar in its native size (XferSave::xferUnicodeString): 2 bytes on Windows, 4
	// everywhere else, so one commit writes two save formats. The 2-byte one says so, and a save carried
	// between Windows and the other platforms is "another build".
	if( sizeof( WideChar ) != 4 )
	{
		char abi[8];
		snprintf( abi, sizeof( abi ), "/w%d", (int)sizeof( WideChar ) );
		id.concat( abi );
	}
	return id;
}

// The commit's first 7 characters, the form every GeneralsX build line shows, then any '~', '/f' and '/w' marks.
static AsciiString shortIdentity( const AsciiString &id )
{
	AsciiString s;
	Int i = 0;
	for( ; i < id.getLength() && isxdigit( (unsigned char)id.getCharAt( i ) ); ++i )
		if( i < 7 )
			s.concat( id.getCharAt( i ) );
	for( ; i < id.getLength(); ++i )
		s.concat( id.getCharAt( i ) );
	return s;
}

// GeneralsX: Returns whether the trailer is on disk: a save without one is now refused on load
// (it reads as a retail save), so a failed append must not be reported as "saved".
static Bool appendBuildTrailer( const AsciiString &filename )
{
	const AsciiString path = TheGameState->getFilePathInSaveDirectory( filename );
	const AsciiString id = thisBuildIdentity();
	FILE *fp = fopen( path.str(), "ab" );
	if( fp == nullptr )
	{
		fprintf( stderr, "[SAVELOAD] could not record the build in '%s'; it cannot be loaded back\n", filename.str() );
		return FALSE;
	}
	const unsigned char len[2] = { (unsigned char)( id.getLength() & 0xff ), (unsigned char)( id.getLength() >> 8 ) };
	const Bool ok = fwrite( id.str(), 1, id.getLength(), fp ) == (size_t)id.getLength()
		&& fwrite( len, 1, 2, fp ) == 2 && fwrite( s_trailerMagic, 1, 8, fp ) == 8;
	if( fclose( fp ) != 0 || !ok )
	{
		fprintf( stderr, "[SAVELOAD] could not record the build in '%s'; it cannot be loaded back\n", filename.str() );
		return FALSE;
	}
	return TRUE;
}

void presentSaveResult( const SaveResult &result )
{
	switch( result.saveCode )
	{
		case SC_OK:
		{
			if( appendBuildTrailer( result.filename ) )
				TheInGameUI->message( TheGameText->fetchGameOnly("GUI:GameSaveComplete") );
			else
			{
				UnicodeString msg;
				msg.format( TheGameText->fetchGameOnly("GUI:ErrorSavingGame"), getUnicodeSavePath(result.filename).str() );
				MessageBoxOk( TheGameText->fetch("GUI:Error"), msg, nullptr );
			}
			break;
		}
		case SC_UNABLE_TO_OPEN_FILE:
		{
			TheInGameUI->message( "GUI:Error" );
			break;
		}
		case SC_ERROR:
		{
			UnicodeString msg;
			msg.format( TheGameText->fetchGameOnly("GUI:ErrorSavingGame"), getUnicodeSavePath(result.filename).str() );
			MessageBoxOk( TheGameText->fetch("GUI:Error"), msg, nullptr );
			break;
		}
		default:
		{
			break;
		}
	}
}

// A load's own message box waits for the next client update (runPendingSaveLoad). After a failed load
// every interactive route resets the engine (PopupSaveLoad::doLoadGame, DebugMenu's doLoadSelectedSave:
// TheGameEngine->reset(), and GameWindowManager::reset destroys every window), so a box made here was gone
// before it was ever drawn.
static UnicodeString s_pendingTitle;
static UnicodeString s_pendingBody;

static void showLoadMessageNextUpdate( const UnicodeString &title, const UnicodeString &body )
{
	s_pendingTitle = title;
	s_pendingBody = body;
}

void presentLoadResult( SaveCode result, const AsciiString &filename )
{
	// stderr, not DEBUG_LOG: DEBUG_LOG is compiled out of release builds, and this is the line a
	// "my save will not load" report needs.
	if( result == SC_OK )
		fprintf( stderr, "[SAVELOAD] loaded '%s'\n", filename.str() );
	else
	{
		// GeneralsX: A failed load leaves GameLogic's loading flags set.
		// GameStateMap::xfer sets m_loadingSave and startNewGame sets m_loadingMap, and only a load that
		// SUCCEEDS clears them - so a throw anywhere after (a LAN/online save refused by the load screen,
		// a newer GameStateMap version, a truncated map) left them TRUE for the rest of the process. The
		// next skirmish then had no quit menu: canOpenQuitMenu() is FALSE while loading, so ESC fell
		// through GameLogic::quit to exitGame(). Every interactive load route reports through here, and
		// loadGame is synchronous, so no load is in flight: clear them. Client-side, so the digest stays.
		const Bool map = TheGameLogic ? TheGameLogic->isLoadingMap() : FALSE;
		const Bool save = TheGameLogic ? TheGameLogic->isLoadingSave() : FALSE;
		fprintf( stderr, "[SAVELOAD] load of '%s' failed: SaveCode %d (left loadingMap=%d loadingSave=%d; cleared)\n",
			filename.str(), (Int)result, (Int)map, (Int)save );
		if( TheGameLogic )
		{
			TheGameLogic->setLoadingMap( FALSE );
			TheGameLogic->setLoadingSave( FALSE );
		}
	}

	if( result != SC_OK && reportPreventedLoadCrash( filename ) )
		return;

	if( result == SC_INVALID_DATA )
	{
		UnicodeString msg;
		msg.format( TheGameText->fetchGameOnly("GUI:ErrorLoadingGame"), getUnicodeSavePath(filename).str() );
		showLoadMessageNextUpdate( TheGameText->fetch("GUI:Error"), msg );
	}
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: Read what a save says about itself, before anything is torn down.
//
// One pass over the block list with plain stdio - never XferLoad, whose skip() seeks BACKWARDS on a
// negative size - collecting what the gate below needs:
//   * the game mode (CHUNK_GameStateMap: version byte, save map, pristine map, Int mode (v2+)): a save
//     made during a LAN or online match was a SIGSEGV on load, not an error - GameStateMap::xfer restores
//     GAME_LAN/GAME_INTERNET, no network session supplies TheGameInfo, and the multiplayer load screen
//     dereferenced the null game (fix/incompatible-save-refusal);
//   * the save file type (CHUNK_GameState: version byte, Int SaveFileType (v2+));
//   * the identity trailer, accepted only where SG_EOF ends exactly that many bytes before end of file;
//   * `crash`: the loads MEASURED to crash or hang, each found by loading a mutated real save with -loadsave
//     (scripts/test/save-compat-test.sh has the fixtures and the red arms):
//       - (not any more) a skirmish save written with a different MAX_SLOTS: SkirmishGameInfo::xfer used to
//         read this build's MAX_SLOTS slots whatever the file held, and misread everything after them. Since
//         the observer seats (MAX_SLOTS 12 -> 14) it reads exactly the slots the save holds, so an older count
//         loads - every 12-slot save included - and is no longer stopped here. A save whose local slot a
//         build drops is stopped by MultiPlayerLoadScreen's local-slot guard.
//       - the radar naming an object the load will not create - measured with an object template this
//         build does not have (removed or renamed in the build that saved it): GameLogic::xfer skips those
//         objects, RadarObject::xfer then throws with the half-built entry already linked, and the teardown
//         after the caught throw dereferences its null object in Radar::deleteList. SIGSEGV, 6 of 9
//         templates tried; the other 3 failed to load without crashing. So a missing template is a crash
//         only where the radar names one of its objects: the objects are read and matched to the radar.
//       - a negative size on a block this build does not know: the loader skips unknown blocks by their
//         size, and XferLoad::skip seeks BACKWARDS on a negative one - by exactly the token and size it
//         just read, it re-reads the same block for ever. Hang.
// GameState.cpp, GameStateMap.cpp, GameInfo.cpp and Radar.cpp are simulation source, so reading the save
// here rather than fixing the readers there keeps the SimID digest where it is.
// Trade-off: a layout change INSIDE a block (a field added without a version bump) can still fault in the
// middle of the load - measured once, in Player::becomingTeamMember on a garbage battle-plan value. No
// pre-read can see that short of the real parse; the "another build" warning is what covers it.
// ------------------------------------------------------------------------------------------------
struct SaveScan
{
	SaveScan() : opened( FALSE ), mode( GAME_NONE ), fileType( -1 ), hasIdentity( FALSE ), objectsRead( FALSE ) { crash[0] = pristineMap[0] = '\0'; }
	Bool opened;
	Int mode;						///< GAME_NONE unless a CHUNK_GameStateMap of a known layout says otherwise
	Int fileType;				///< -1 when CHUNK_GameState is absent or of an unknown layout
	char crash[192];		///< non-empty: loading this save was measured to crash or hang, and why
	char pristineMap[256];	///< the map the save was started on, as CHUNK_GameStateMap stores it
	Bool hasIdentity;
	AsciiString identity;
	Bool objectsRead;						///< CHUNK_GameLogic's object list was read in full
	std::vector<UnsignedInt> loaded;		///< ids of the objects the load will create, sorted
	std::vector<std::pair<UnsignedInt, AsciiString> > skipped;	///< objects it will skip, with their template
};

// Every block GameState::init registers for SNAPSHOT_SAVELOAD: the loader reads these by their own
// xfer and ignores the stored size, and skips everything else BY the stored size. Names match exactly, as
// GameState::findBlockInfoByToken matches them (only SG_EOF is matched in any case). addSnapshotBlock leaves
// out a snapshot whose isXferEnabled() is FALSE, and three can be: the headless particle and ghost-object
// dummies and the terrain visual (W3DTerrainVisual, when headless). Those blocks are skipped like unknown
// ones then, so they count as known only while their snapshot takes part.
static const char *const s_knownBlocks[] = {
	"CHUNK_GameState", "CHUNK_Campaign", "CHUNK_GameStateMap", "CHUNK_TerrainLogic", "CHUNK_TeamFactory",
	"CHUNK_Players", "CHUNK_GameLogic", "CHUNK_Radar", "CHUNK_ScriptEngine", "CHUNK_SidesList",
	"CHUNK_TacticalView", "CHUNK_GameClient", "CHUNK_InGameUI", "CHUNK_Partition",
	"CHUNK_GXCheatReveal", "CHUNK_GXPathfindQueue",
};

static Bool isKnownBlock( const char *token )
{
	for( size_t i = 0; i < sizeof( s_knownBlocks ) / sizeof( s_knownBlocks[0] ); ++i )
		if( strcmp( token, s_knownBlocks[i] ) == 0 )
			return TRUE;
	const Snapshot *optional = nullptr;
	if( strcmp( token, "CHUNK_ParticleSystem" ) == 0 )
		optional = TheParticleSystemManager;
	else if( strcmp( token, "CHUNK_TerrainVisual" ) == 0 )
		optional = TheTerrainVisual;
	else if( strcmp( token, "CHUNK_GhostObject" ) == 0 )
		optional = TheGhostObjectManager;
	return optional != nullptr && optional->isXferEnabled();
}

static Bool readBytes( FILE *fp, void *out, size_t n )
{
	return n == 0 || fread( out, 1, n, fp ) == n;
}

static Bool readToken( FILE *fp, char out[256] )
{
	unsigned char len = 0;
	if( !readBytes( fp, &len, 1 ) || !readBytes( fp, out, len ) )
		return FALSE;
	out[len] = '\0';
	return TRUE;
}

static Bool readInt( FILE *fp, Int *out )
{
	unsigned char b[4];
	if( !readBytes( fp, b, 4 ) )
		return FALSE;
	*out = (Int)( (UnsignedInt)b[0] | ( (UnsignedInt)b[1] << 8 ) | ( (UnsignedInt)b[2] << 16 ) | ( (UnsignedInt)b[3] << 24 ) );
	return TRUE;
}

// TRUE when the save's map brings its own INI (map.ini, solo.ini) and that INI names `templateName`.
// GameLogic::loadMapINI loads those before the objects, and a map.ini can define object templates of its
// own that do not exist here in the menu: without this, a good save on such a map would be refused. Same
// path as loadMapINI (pristine map, the map cache's on-disk spelling, its folder). The file is READ, not
// just probed: doesFileExist answered TRUE for a map.ini that Tournament Desert does not have (measured,
// the archive side), so existence alone would have switched this check off for every map.
// Trade-off: "the INI mentions the name" rather than parsing its Object blocks - it can only err towards
// letting a save through, and the digested build's radar fix covers what that lets through.
static Bool mapIniMentions( const char *savedPristineMap, const char *templateName )
{
	if( savedPristineMap[0] == '\0' || templateName[0] == '\0' || TheFileSystem == nullptr )
		return FALSE;	// an empty name: strstr would match at every character, the terminator included
	AsciiString path = TheGameState->portableMapPathToRealMapPath( AsciiString( savedPristineMap ) );
	if( TheMapCache != nullptr )
		path = TheMapCache->getPathOnDisk( path );
	char dir[512];
	snprintf( dir, sizeof( dir ), "%s", path.str() );
	char *cut = dir + strlen( dir );
	while( cut > dir && *cut != '\\' && *cut != '/' )
		--cut;
	*cut = '\0';
	const size_t nameLen = strlen( templateName );
	static const char *const s_iniNames[] = { "map.ini", "solo.ini" };
	for( size_t n = 0; n < 2; ++n )
	{
		char ini[600];
		snprintf( ini, sizeof( ini ), "%s\\%s", dir, s_iniNames[n] );
		File *file = TheFileSystem->openFile( ini, File::READ | File::BINARY );
		if( file == nullptr )
			continue;
		const Int size = file->size();
		char *text = size > 0 ? NEW char[size + 1] : nullptr;
		const Int got = text != nullptr ? file->read( text, size ) : 0;
		file->close();
		if( text == nullptr )
			continue;
		text[got > 0 ? got : 0] = '\0';
		Bool found = FALSE;
		for( const char *hit = strstr( text, templateName ); hit != nullptr && !found; hit = strstr( hit + 1, templateName ) )
		{
			const char before = hit == text ? ' ' : hit[-1];
			const char after = hit[nameLen];
			found = !isalnum( (unsigned char)before ) && before != '_' && !isalnum( (unsigned char)after ) && after != '_';
		}
		delete[] text;
		if( found )
			return TRUE;
	}
	return FALSE;
}

// CHUNK_GameLogic: version byte, Int frame, the object table of contents (version byte, UnsignedInt count,
// count x (template name, UnsignedShort id)) - every object template the save uses - then UnsignedInt
// objects, each an UnsignedShort TOC id, an Int size and Object::xfer (version byte, ObjectID first).
static void scanObjects( FILE *fp, long blockEnd, SaveScan *scan )
{
	unsigned char version = 0, tocVersion = 0;
	Int frame = 0, count = 0, objects = 0;
	if( !readBytes( fp, &version, 1 ) || !readInt( fp, &frame ) || !readBytes( fp, &tocVersion, 1 ) || tocVersion != 1
		|| !readInt( fp, &count ) || count < 0 || TheThingFactory == nullptr )
		return;
	std::vector<std::pair<UnsignedShort, AsciiString> > missing;	// TOC id, name: templates the load will not have
	std::vector<UnsignedShort> present;
	char name[256];
	unsigned char id[2];
	for( Int i = 0; i < count; ++i )
	{
		if( !readToken( fp, name ) || !readBytes( fp, id, 2 ) )
			return;
		const UnsignedShort tocId = (UnsignedShort)( id[0] | ( id[1] << 8 ) );
		// A template the running match's own map INI created (an override) is gone once the load resets the
		// factory, before the save's objects are read: it counts as missing here too.
		const ThingTemplate *known = TheThingFactory->findTemplate( AsciiString( name ), FALSE );
		if( ( known == nullptr || known->isOverride() ) && !mapIniMentions( scan->pristineMap, name ) )
			missing.push_back( std::make_pair( tocId, AsciiString( name ) ) );
		else
			present.push_back( tocId );
	}
	if( !readInt( fp, &objects ) || objects < 0 )
		return;
	for( Int i = 0; i < objects; ++i )
	{
		Int size = 0, objectId = 0;
		unsigned char objectVersion = 0;
		if( !readBytes( fp, id, 2 ) || !readInt( fp, &size ) || size < 5 || (long)size > blockEnd - ftell( fp ) )
			return;	// short or bad: the loader fails on it by itself
		const long start = ftell( fp );
		if( !readBytes( fp, &objectVersion, 1 ) || !readInt( fp, &objectId ) || fseek( fp, start + size, SEEK_SET ) != 0 )
			return;
		const UnsignedShort tocId = (UnsignedShort)( id[0] | ( id[1] << 8 ) );
		if( std::find( present.begin(), present.end(), tocId ) != present.end() )
			scan->loaded.push_back( (UnsignedInt)objectId );
		else
			for( size_t m = 0; m < missing.size(); ++m )
				if( missing[m].first == tocId )
					scan->skipped.push_back( std::make_pair( (UnsignedInt)objectId, missing[m].second ) );
		// an id in neither list: GameLogic::xfer throws on it, a plain "cannot load"
	}
	std::sort( scan->loaded.begin(), scan->loaded.end() );
	scan->objectsRead = TRUE;
}

// CHUNK_Radar: version byte, the hidden/forced flags (16 + 16 bytes from version 2, two Bools before), then
// the local and the other object list, each a version byte, UnsignedShort count and count x (version byte,
// ObjectID, color).
static void scanRadar( FILE *fp, long blockEnd, SaveScan *scan )
{
	unsigned char version = 0;
	if( !scan->objectsRead || scan->crash[0] != '\0' || !readBytes( fp, &version, 1 ) || version > 2
		|| fseek( fp, version >= 2 ? 32 : 2, SEEK_CUR ) != 0 )
		return;
	for( Int list = 0; list < 2; ++list )
	{
		unsigned char listVersion = 0, count[2];
		if( !readBytes( fp, &listVersion, 1 ) || !readBytes( fp, count, 2 ) )
			return;
		for( Int i = 0; i < ( count[0] | ( count[1] << 8 ) ); ++i )
		{
			unsigned char entryVersion = 0;
			Int objectId = 0, color = 0;
			if( ftell( fp ) + 9 > blockEnd || !readBytes( fp, &entryVersion, 1 ) || !readInt( fp, &objectId ) || !readInt( fp, &color ) )
				return;
			if( std::binary_search( scan->loaded.begin(), scan->loaded.end(), (UnsignedInt)objectId ) )
				continue;
			for( size_t k = 0; k < scan->skipped.size(); ++k )
				if( scan->skipped[k].first == (UnsignedInt)objectId )
				{
					snprintf( scan->crash, sizeof( scan->crash ), "it uses object template '%s', which this build does not have",
						scan->skipped[k].second.str() );
					return;
				}
			snprintf( scan->crash, sizeof( scan->crash ), "its radar names object %u, which the save does not hold", (unsigned)objectId );
			return;
		}
	}
}

static void scanSave( const AsciiString &filename, SaveScan *scan )
{
	FILE *fp = fopen( TheGameState->getSaveGamePathForRead( filename ).str(), "rb" );
	if( fp == nullptr )
		return;	// missing or unreadable: the normal load path reports that itself
	scan->opened = TRUE;

	fseek( fp, 0, SEEK_END );
	const long fileSize = ftell( fp );
	fseek( fp, 0, SEEK_SET );

	long eofEnd = -1;
	char token[256];
	while( readToken( fp, token ) )
	{
		if( stricmp( token, "SG_EOF" ) == 0 )
		{
			eofEnd = ftell( fp );
			break;
		}
		Int size = 0;
		if( !readInt( fp, &size ) )
			break;	// truncated: the loader's read fails and it says "cannot load" by itself
		const long start = ftell( fp );
		if( !isKnownBlock( token ) && size < 0 )
		{
			// The walk cannot go on past a block it cannot size, so the identity trailer stays unread.
			snprintf( scan->crash, sizeof( scan->crash ), "block '%s', which this build does not know, has a negative size", token );
			break;
		}
		if( size < 0 || (long)size > fileSize - start )
			break;	// a known block: its size is never used to load it, but this walk cannot go on

		if( strcmp( token, "CHUNK_GameState" ) == 0 )
		{
			unsigned char version = 0;
			Int type = 0;
			if( readBytes( fp, &version, 1 ) && version == 2 && readInt( fp, &type ) )
				scan->fileType = type;
		}
		else if( strcmp( token, "CHUNK_GameStateMap" ) == 0 )
		{
			// A version > 2 has an unknown layout: Xfer::xferVersion refuses it, so the loader says so.
			unsigned char version = 0;
			char name[256];
			Int mode = GAME_NONE;
			if( readBytes( fp, &version, 1 ) && version <= 2 && readToken( fp, name ) && readToken( fp, name )
				&& ( version < 2 || readInt( fp, &mode ) ) )
			{
				snprintf( scan->pristineMap, sizeof( scan->pristineMap ), "%s", name );
				scan->mode = mode;
			}
		}
		else if( strcmp( token, "CHUNK_GameLogic" ) == 0 )
			scanObjects( fp, start + size, scan );
		else if( strcmp( token, "CHUNK_Radar" ) == 0 )
			scanRadar( fp, start + size, scan );
		fseek( fp, start + size, SEEK_SET );
	}

	if( eofEnd >= 0 && fileSize - eofEnd >= TRAILER_FIXED )
	{
		unsigned char tail[TRAILER_FIXED];
		fseek( fp, fileSize - TRAILER_FIXED, SEEK_SET );
		if( readBytes( fp, tail, TRAILER_FIXED ) && memcmp( tail + 2, s_trailerMagic, 8 ) == 0 )
		{
			const long len = (long)tail[0] | ( (long)tail[1] << 8 );
			if( eofEnd + len + TRAILER_FIXED == fileSize && len > 0 && len < 256 )
			{
				char id[256];
				fseek( fp, eofEnd, SEEK_SET );
				if( readBytes( fp, id, (size_t)len ) )
				{
					id[len] = '\0';
					scan->identity = id;
					scan->hasIdentity = TRUE;
				}
			}
		}
	}
	fclose( fp );
}

// Everything after the last separator: the crash log and the stderr line never carry a folder.
static const char *leafName( const AsciiString &filename )
{
	const char *s = filename.str();
	const char *leaf = s;
	for( ; *s; ++s )
		if( *s == '/' || *s == '\\' )
			leaf = s + 1;
	return leaf;
}

// GeneralsX: "If a load WOULD crash, catch it, PREVENT the crash, say it would
// have crashed, and ask to send the crash log" (owner, decision #7). Nothing that would fault has run;
// the crash log goes through the engine's own writer (GXCrashLog, wired by the ZH main loop) into
// CrashLogs/, where the launcher's consent card offers to send it. The upload itself stays off until the
// owner approves the endpoint - this only writes the file. Base Generals has no writer, and a write can
// fail: the box promises a report only when one was written.
static void reportPreventedCrash( const AsciiString &filename, const char *cause )
{
	SaveScan scan;
	scanSave( filename, &scan );	// for the build that made it; cheap, and the file is still there
	const AsciiString made = scan.hasIdentity ? shortIdentity( scan.identity ) : AsciiString( "unknown" );
	const AsciiString mine = shortIdentity( thisBuildIdentity() );
	// GeneralsX: NO FILE NAME in the crash log. The player names the
	// save, and a name like "wh1ter0se-MacBook base" is personal data in a report that may be uploaded. The
	// stderr line below keeps it: stderr stays on this machine. The ingest Worker and the launcher also
	// strip it from logs written before this change.
	char reason[512];
	snprintf( reason, sizeof( reason ), "prevented crash loading a save: %s (save build %s, this build %s)",
		cause, made.str(), mine.str() );
	for( char *c = reason; *c; ++c )
		if( *c == '\n' || *c == '\r' )
			*c = ' ';	// one line: the launcher and the ingest parse "reason:" as a line
	const Bool logged = TheGXCrashLogWriter != nullptr && TheGXCrashLogWriter( reason, nullptr, 0 );
	fprintf( stderr, "[SAVELOAD] prevented crash: '%s': %s; crash log %s\n",
		leafName( filename ), cause, logged ? "written" : "NOT written" );

	showLoadMessageNextUpdate(
		TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGamePreventedCrashTitle", L"LOAD STOPPED" ),
		logged
			? TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGamePreventedCrash",
				L"Loading this save would have crashed the game, so it was stopped. A crash report was saved; "
				L"the launcher will ask whether to send it." )
			: TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGamePreventedCrashNoLog",
				L"Loading this save would have crashed the game, so it was stopped." ) );
}

// A guard inside the load (LoadScreen.cpp) turned a fault into a throw: remember why, so the failed load
// is reported as the crash it would have been rather than as a plain "cannot load".
static char s_preventedDuringLoad[192];

void notePreventedLoadCrash( const char *cause )
{
	snprintf( s_preventedDuringLoad, sizeof( s_preventedDuringLoad ), "%s", cause );
}

Bool reportPreventedLoadCrash( const AsciiString &filename )
{
	if( s_preventedDuringLoad[0] == '\0' )
		return FALSE;
	char cause[192];
	snprintf( cause, sizeof( cause ), "%s", s_preventedDuringLoad );
	s_preventedDuringLoad[0] = '\0';
	reportPreventedCrash( filename, cause );
	return TRUE;
}

// ------------------------------------------------------------------------------------------------
// The gate. Yes/No state lives here because a message box callback takes no arguments.
// ------------------------------------------------------------------------------------------------
static AsciiString s_warnedFile;						///< the save the box is asking about
static GameWinMsgBoxFunc s_warnedRetry = nullptr;
static AsciiString s_approvedFile;					///< Yes was pressed for this save: let it through once
static GameWinMsgBoxFunc s_pendingRetry = nullptr;	///< Yes was pressed: runPendingSaveLoad re-enters the caller

static void onSaveWarningYes()
{
	// Not retry() from here: MessageBoxSystem calls this BEFORE it destroys the box, and the load's
	// GameEngine::reset frees every window (winDestroyAll) - the box it then destroys would be freed memory.
	fprintf( stderr, "[SAVELOAD] warning accepted: loading '%s'\n", s_warnedFile.str() );
	s_approvedFile = s_warnedFile;
	s_warnedFile.clear();
	s_pendingRetry = s_warnedRetry;
	s_warnedRetry = nullptr;
}

void runPendingSaveLoad()
{
	if( !s_pendingTitle.isEmpty() )
	{
		MessageBoxOk( s_pendingTitle, s_pendingBody, nullptr );
		s_pendingTitle.clear();
		s_pendingBody.clear();
	}
	GameWinMsgBoxFunc retry = s_pendingRetry;
	if( retry == nullptr )
		return;
	s_pendingRetry = nullptr;
	retry();
	s_approvedFile.clear();	// the retry consumed it, or did not reach the gate; never carry it further
}

// Test hook: headless has nobody to press a button, so do what MessageBoxSystem does for a press - the
// button's callback, THEN winDestroy on the box - in that order, the order a load has to survive. Not
// MessageBoxSystem itself: a -headless box is a GameWindowDummy, whose user data (the callbacks) reads null.
static void pressWarningButton( GameWindow *box, GameWinMsgBoxFunc callback )
{
	callback();
	TheWindowManager->winDestroy( box );
	fprintf( stderr, "[SAVELOAD] test: the warning box is closed\n" );
}

static void onSaveWarningNo()
{
	// "No returns to the menu": nothing has been torn down, so the menu the player was on is still there.
	fprintf( stderr, "[SAVELOAD] warning declined: '%s' not loaded\n", s_warnedFile.str() );
	s_warnedFile.clear();
	s_warnedRetry = nullptr;
}

Bool gateSaveLoad( const AsciiString &filename, GameWinMsgBoxFunc retry )
{
	s_preventedDuringLoad[0] = '\0';	// a note belongs to the load it was made in, never to a later one

	// Test hook: every route is gated, so a test of the guards INSIDE the load (the load screen's, the
	// loader's) needs a way past this one. scripts/test/save-compat-test.sh, saveload-refusal-test.sh.
	if( getenv( "GX_TEST_SAVE_UNGATED" ) != nullptr )
		return FALSE;

	// Yes answers the build warning and nothing else: the file is scanned again, so one replaced while the
	// box was up still meets the refusal and the crash checks.
	const Bool approved = s_approvedFile.isNotEmpty() && s_approvedFile == filename;
	s_approvedFile.clear();

	SaveScan scan;
	scanSave( filename, &scan );
	if( !scan.opened )
		return FALSE;	// the normal load path reports a missing file itself

	if( scan.mode == GAME_LAN || scan.mode == GAME_INTERNET )
	{
		fprintf( stderr, "[SAVELOAD] refused '%s': saved during %s match (game mode %d), which cannot be resumed from a save\n",
			filename.str(), scan.mode == GAME_LAN ? "a LAN" : "an online", (Int)scan.mode );
		MessageBoxOk(
			TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameLoadFailedTitle", L"CANNOT LOAD SAVE" ),
			TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameNetworkMatch",
				L"This save was made during a LAN or online match. Network matches cannot be resumed from a save." ),
			nullptr );
		return TRUE;
	}

	if( scan.crash[0] != '\0' )
	{
		reportPreventedCrash( filename, scan.crash );
		return TRUE;
	}

	// A mission save holds only CHUNK_GameState and CHUNK_Campaign (GameState::xferSaveData): the map
	// itself loads fresh from the map files, so there is no other build's state to trip over.
	if( scan.fileType == SAVE_FILE_TYPE_MISSION )
		return FALSE;

	const AsciiString mine = thisBuildIdentity();
	// GeneralsX: A save with no build trailer is a retail save or one from a very old
	// GeneralsX. Measured on the published 0.9.58 (Mini) with two real retail Zero Hour saves: both are well-formed
	// block by block but the loader reads another layout (RETAIL_COMPATIBLE_XFER_SAVE is 0), fails with an xfer READ
	// error far past the end of the file, and -loadsave then closes the game. A Yes button there only closed the game
	// with no reason, so the box now says so and offers no Yes. Saves that DO name a build keep their Yes/No.
	if( !scan.hasIdentity )
	{
		fprintf( stderr, "[SAVELOAD] refused '%s': it does not say which build made it, so it is a save from the original game "
			"or a very old GeneralsX, whose layout this build cannot read (this is %s)\n", filename.str(), mine.str() );
		MessageBoxOk(
			TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameLoadFailedTitle", L"CANNOT LOAD SAVE" ),
			TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameRetail",
				L"This save was made by the original game (or a much older GeneralsX). Saves from the original game cannot be loaded by GeneralsX." ),
			nullptr );
		return TRUE;
	}

	if( approved || ( scan.hasIdentity && scan.identity == mine ) )
		return FALSE;

	fprintf( stderr, "[SAVELOAD] warning: '%s' was made by build %s (this is %s)\n",
		filename.str(), shortIdentity( scan.identity ).str(), mine.str() );
	UnicodeString made, here, body;
	made.translate( shortIdentity( scan.identity ) );
	here.translate( shortIdentity( mine ) );
	body.format( TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameOtherBuild",
		L"This save was made by a different GeneralsX build (%ls; this is %ls). It may not work.\n\nLoad it anyway?" ),
		made.str(), here.str() );

	s_warnedFile = filename;
	s_warnedRetry = retry;

	GameWindow *box = MessageBoxYesNo( TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameOtherBuildTitle", L"SAVE FROM ANOTHER BUILD" ),
		body, onSaveWarningYes, onSaveWarningNo );
	const char *answer = getenv( "GX_TEST_SAVE_WARNING_ANSWER" );
	if( box != nullptr && answer != nullptr && stricmp( answer, "yes" ) == 0 )
		pressWarningButton( box, onSaveWarningYes );
	else if( box != nullptr && answer != nullptr && stricmp( answer, "no" ) == 0 )
		pressWarningButton( box, onSaveWarningNo );
	return TRUE;
}

// GeneralsX: Do not write the save the gate would refuse: it would sit
// in the list unloadable here, and a build without that check still crashes on it.
Bool refuseNetworkMatchSave()
{
	if( TheGameLogic == nullptr || !TheGameLogic->isInMultiplayerGame() )	// GAME_LAN or GAME_INTERNET
		return FALSE;

	fprintf( stderr, "[SAVELOAD] save refused: a %s match cannot be resumed from a save\n",
		TheGameLogic->isInLanGame() ? "LAN" : "online" );
	MessageBoxOk(
		TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameNetworkMatchTitle", L"CANNOT SAVE" ),
		TheGameText->FETCH_OR_SUBSTITUTE( "GUI:SaveGameNetworkMatchSave",
			L"LAN and online matches cannot be saved: a network match cannot be resumed from a save." ),
		nullptr );
	return TRUE;
}
