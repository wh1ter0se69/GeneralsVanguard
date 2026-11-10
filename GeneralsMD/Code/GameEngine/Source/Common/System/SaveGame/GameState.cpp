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

// FILE: GameState.cpp ////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, September 2002
// Desc:   Game state singleton from which to load and save the game state
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"
#ifndef _WIN32
#include <filesystem>     // std::filesystem for Linux directory operations
#include "socket_compat.h" // CreateDirectory stub for Linux
#endif
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GameStateMap.h"
#include "Common/LatchRestore.h"
#include "Common/MapObject.h"
#include "Common/PlayerList.h"
#include "Common/RandomValue.h"
#include "Common/Radar.h"
#include "Common/Team.h"
#include "Common/WellKnownKeys.h"
#include "Common/XferLoad.h"
#include "Common/XferSave.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/AIPathfind.h"	// GeneralsX: TheGXPathfindQueueState
#include "GameLogic/GameLogic.h"
#include "GameLogic/GhostObject.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/TerrainLogic.h"
#include "Lib/PathUtil.h"


// PUBLIC DATA ////////////////////////////////////////////////////////////////////////////////////
GameState *TheGameState = nullptr;

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static const Char *SAVE_FILE_EOF       = "SG_EOF";
static const Char *SAVE_GAME_EXTENSION = ".sav";
static const Char *ZERO_NAME_ONLY      = "00000000";
static const Int MAX_SAVE_FILE_NUMBER  =  99999999;

///////////////////////////////////////////////////////////////////////////////////////////////////
#define GAME_STATE_BLOCK_STRING "CHUNK_GameState"  // block of save game data with game info data
#define CAMPAIGN_BLOCK_STRING "CHUNK_Campaign"		 // block of game data that has campaign info

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
SaveGameInfo::SaveGameInfo()
{

	date.day					= 0;
	date.dayOfWeek		= 0;
	date.hour					= 0;
	date.milliseconds = 0;
	date.minute				= 0;
	date.month				= 0;
	date.second				= 0;
	date.year					= 0;
	missionNumber			= 0;
	saveFileType			= SAVE_FILE_TYPE_NORMAL;

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
SaveGameInfo::~SaveGameInfo()
{

}

// ------------------------------------------------------------------------------------------------
/** Is this date newer than the other one passed in? */
// ------------------------------------------------------------------------------------------------
Bool SaveDate::isNewerThan( SaveDate *other )
{

	// year
	if( year > other->year )
		return TRUE;
	else if( year < other->year )
		return FALSE;
	else
	{

		// month
		if( month > other->month )
			return TRUE;
		else if( month < other->month )
			return FALSE;
		else
		{

			// day
			if( day > other->day )
				return TRUE;
			else if( day < other->day )
				return FALSE;
			else
			{

				// hour
				if( hour > other->hour )
					return TRUE;
				else if( hour < other->hour )
					return FALSE;
				else
				{

					// minute
					if( minute > other->minute )
						return TRUE;
					else if( minute < other->minute )
						return FALSE;
					else
					{

						// second
						if( second > other->second )
							return TRUE;
						else if( second < other->second )
							return FALSE;
						else
						{

							// millisecond
							if( milliseconds > other->milliseconds )
								return TRUE;
							else
								return FALSE;

						}

					}

				}

			}

		}

	}

}

// ------------------------------------------------------------------------------------------------
/** Find a snapshot block info that matches the token passed in */
// ------------------------------------------------------------------------------------------------
GameState::SnapshotBlock *GameState::findBlockInfoByToken( AsciiString token, SnapshotType which )
{

	// sanity
	if( token.isEmpty() )
		return nullptr;

	// search for match our list
	SnapshotBlock *blockInfo;
	SnapshotBlockListIterator it;
	for( it = m_snapshotBlockList[which].begin(); it != m_snapshotBlockList[which].end(); ++it )
	{

		// get info
		blockInfo = &(*it);

		// check for match
		if( blockInfo->blockName == token )
			return blockInfo;

	}

	// not found
	return nullptr;

}

// TheSuperHackers @tweak Use the user's default locale instead of the system default to match Windows regional settings.
// This allows regional formats such as Europe (English) to use 24-hour and DD/MM/YYYY formats in-game.
UnicodeString getUnicodeDateBuffer(SYSTEMTIME timeVal)
{
	// setup date buffer for local region date format
	#define DATE_BUFFER_SIZE 256
#ifdef _WIN32
	OSVERSIONINFO	osvi;
	osvi.dwOSVersionInfoSize=sizeof(OSVERSIONINFO);
	UnicodeString displayDateBuffer;
	if (GetVersionEx(&osvi))
	{	//check if we're running Win9x variant since they may need different characters
		if (osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
		{
			char dateBuffer[ DATE_BUFFER_SIZE ];
			GetDateFormat( LOCALE_USER_DEFAULT,
										 DATE_SHORTDATE,
										 &timeVal,
										 nullptr,
										 dateBuffer, sizeof(dateBuffer) );
			displayDateBuffer.translate(dateBuffer);
			return displayDateBuffer;
		}
	}
	wchar_t dateBuffer[ DATE_BUFFER_SIZE ];
	GetDateFormatW( LOCALE_USER_DEFAULT,
								 DATE_SHORTDATE,
								 &timeVal,
								 nullptr,
								 dateBuffer, ARRAY_SIZE(dateBuffer) );
	displayDateBuffer.set(dateBuffer);
	return displayDateBuffer;
	//displayDateBuffer.format( L"%ls", dateBuffer );
#else
	// Linux: Use simple ISO 8601 date format (YYYY-MM-DD)
	UnicodeString displayDateBuffer;
	char dateBuffer[DATE_BUFFER_SIZE];
	snprintf(dateBuffer, DATE_BUFFER_SIZE, "%04d-%02d-%02d", 
		timeVal.wYear, timeVal.wMonth, timeVal.wDay);
	displayDateBuffer.translate(dateBuffer);
	return displayDateBuffer;
#endif
}

UnicodeString getUnicodeTimeBuffer(SYSTEMTIME timeVal)
{
	// setup time buffer for local region time format
	UnicodeString displayTimeBuffer;
#ifdef _WIN32
	OSVERSIONINFO	osvi;
	osvi.dwOSVersionInfoSize=sizeof(OSVERSIONINFO);
	if (GetVersionEx(&osvi))
	{	//check if we're running Win9x variant since they may need different characters
		if (osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
		{
			char timeBuffer[ DATE_BUFFER_SIZE ];
			GetTimeFormat( LOCALE_USER_DEFAULT,
										 TIME_NOSECONDS|TIME_FORCE24HOURFORMAT|TIME_NOTIMEMARKER,
										 &timeVal,
										 nullptr,
										 timeBuffer, sizeof(timeBuffer) );
			displayTimeBuffer.translate(timeBuffer);
			return displayTimeBuffer;
		}
	}
	// setup time buffer for local region time format
	#define TIME_BUFFER_SIZE 256
	wchar_t timeBuffer[ TIME_BUFFER_SIZE ];
	GetTimeFormatW( LOCALE_USER_DEFAULT,
								 TIME_NOSECONDS,
								 &timeVal,
								 nullptr,
								 timeBuffer,
								 ARRAY_SIZE(timeBuffer) );
	displayTimeBuffer.set(timeBuffer);
	return displayTimeBuffer;
#else
	// Linux: Use simple 24-hour time format (HH:MM)
	char timeBuffer[DATE_BUFFER_SIZE];
	snprintf(timeBuffer, DATE_BUFFER_SIZE, "%02d:%02d", 
		timeVal.wHour, timeVal.wMinute);
	displayTimeBuffer.translate(timeBuffer);
	return displayTimeBuffer;
#endif
}

// ------------------------------------------------------------------------------------------------
GameState::GameState()
{

	m_availableGames = nullptr;
	m_isInLoadGame = FALSE;

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
GameState::~GameState()
{

	// clear our snapshot block list
	for (Int i=0; i<SNAPSHOT_MAX; ++i)
	m_snapshotBlockList[i].clear();

	// make certain that the post process list is clean
	m_snapshotPostProcessList.clear();

	// clear any available game
	clearAvailableGames();

}

// ------------------------------------------------------------------------------------------------
/** Init the game state subsystem */
// ------------------------------------------------------------------------------------------------
void GameState::init()
{

	// add all the snapshot objects to our list of data blocks for save game files
	addSnapshotBlock( GAME_STATE_BLOCK_STRING,				TheGameState,							SNAPSHOT_SAVELOAD );
	addSnapshotBlock( CAMPAIGN_BLOCK_STRING,					TheCampaignManager,				SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_GameStateMap",						TheGameStateMap,					SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_TerrainLogic",						TheTerrainLogic,					SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_TeamFactory",						TheTeamFactory,						SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_Players",								ThePlayerList,						SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_GameLogic",							TheGameLogic,							SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_Radar",									TheRadar,									SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_ScriptEngine",						TheScriptEngine,					SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_SidesList",							TheSidesList,							SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_TacticalView",						TheTacticalView,					SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_GameClient",							TheGameClient,						SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_InGameUI",								TheInGameUI,							SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_Partition",							ThePartitionManager,			SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_ParticleSystem",					TheParticleSystemManager,	SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_TerrainVisual",					TheTerrainVisual,					SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_GhostObject",						TheGhostObjectManager,		SNAPSHOT_SAVELOAD );
	// GeneralsX: LAST, and only in a save: a loader that does not know it skips it by its size.
	addSnapshotBlock( "CHUNK_GXCheatReveal",					TheGXCheatRevealState,		SNAPSHOT_SAVELOAD );
	addSnapshotBlock( "CHUNK_GXPathfindQueue",				TheGXPathfindQueueState,	SNAPSHOT_SAVELOAD );

	// add all the snapshot objects to our list of data blocks for deep CRCs of logic
	addSnapshotBlock( "CHUNK_TeamFactory",						TheTeamFactory,						SNAPSHOT_DEEPCRC_LOGICONLY );
	addSnapshotBlock( "CHUNK_Players",								ThePlayerList,						SNAPSHOT_DEEPCRC_LOGICONLY );
	addSnapshotBlock( "CHUNK_GameLogic",							TheGameLogic,							SNAPSHOT_DEEPCRC_LOGICONLY );
	addSnapshotBlock( "CHUNK_ScriptEngine",						TheScriptEngine,					SNAPSHOT_DEEPCRC_LOGICONLY );
	addSnapshotBlock( "CHUNK_SidesList",							TheSidesList,							SNAPSHOT_DEEPCRC_LOGICONLY );
	addSnapshotBlock( "CHUNK_Partition",							ThePartitionManager,			SNAPSHOT_DEEPCRC_LOGICONLY );

	m_isInLoadGame = FALSE;

}

// ------------------------------------------------------------------------------------------------
/** Reset */
// ------------------------------------------------------------------------------------------------a
void GameState::reset()
{

	// clear the post process snapshot list
	m_snapshotPostProcessList.clear();

	// clear any available game
	clearAvailableGames();

	m_isInLoadGame = FALSE;

}

// ------------------------------------------------------------------------------------------------
/** Clear any available games entries */
// ------------------------------------------------------------------------------------------------
void GameState::clearAvailableGames()
{
	AvailableGameInfo *gameInfo;

	while( m_availableGames )
	{

		gameInfo = m_availableGames->next;
		delete m_availableGames;
		m_availableGames = gameInfo;

	}

}

// ------------------------------------------------------------------------------------------------
/** Add a snapshot and block name pair to the systems used to load and save */
// ------------------------------------------------------------------------------------------------
void GameState::addSnapshotBlock( AsciiString blockName, Snapshot *snapshot, SnapshotType which )
{

	// sanity
	if( blockName.isEmpty() || snapshot == nullptr )
	{

		DEBUG_CRASH(( "addSnapshotBlock: Invalid parameters" ));
		return;

	}

	// Snapshots with xfer disabled are not registered, so their blocks are omitted when saving
	// and are skipped like unknown blocks when loading.
	if( !snapshot->isXferEnabled() )
	{
		return;
	}

	// add to the list
	SnapshotBlock blockInfo;
	blockInfo.snapshot = snapshot;
	blockInfo.blockName = blockName;
	m_snapshotBlockList[which].push_back( blockInfo );

}

// ------------------------------------------------------------------------------------------------
/** Given the filename of a save file, find the highest filename number */
// ------------------------------------------------------------------------------------------------
static void findHighFileNumber( AsciiString filename, void *userData )
{

	// sanity
	if( filename.isEmpty() )
		return;

	// sanity check for ".sav" at the end of the filename
	if( filename.endsWithNoCase( SAVE_GAME_EXTENSION ) == FALSE )
		return;

	// strip off the extension at the end of the filename
	AsciiString nameOnly = filename;
	nameOnly.truncateBy( strlen( SAVE_GAME_EXTENSION ) );

	// convert filename (which is only numbers) to a number
	Int fileNumber = atoi( nameOnly.str() );

	//
	// atoi will return zero if the string could not be converted, if the filename is
	// not literally "00000000.sav" then that means the conversion could not be done so
	// we should reject this filename from further processing
	//
	if( fileNumber == 0 && nameOnly.compare( ZERO_NAME_ONLY ) != 0 )
		return;

	// compare against the file number we're keeping tally of in the user data parameter
	Int *highFileNumber = (Int *)userData;
	if( fileNumber >= *highFileNumber )
		*highFileNumber = fileNumber;

}

// ------------------------------------------------------------------------------------------------
/** Given the save files on disk, find the "next" filename to use when saving a game */
// ------------------------------------------------------------------------------------------------
AsciiString GameState::findNextSaveFilename( UnicodeString desc )
{
// works, but needs approval from mgmt (srj)
	// GS activating for patch
#define COCKBEER_DONT_USE_NORMAL_FILE_NAMES_THANKS
#ifdef USE_NORMAL_FILE_NAMES_THANKS

	Int i;
	AsciiString adesc;
	for (i = 0; i < desc.getLength(); ++i)
	{
		char c = (char)desc.getCharAt(i);
		if (isalnum(c))
			adesc.concat(c);
		else
			adesc.concat('_');
	}

	for (i = 1; i <= 9999; ++i)
	{
		AsciiString leaf;
		leaf.format("%s_%04d%s", adesc.str(), i, SAVE_GAME_EXTENSION);

		AsciiString path = getFilePathInSaveDirectory(leaf);
		if( _access( path.str(), 0 ) == -1 )
			return leaf;	// note that this returns the leaf, not the full path
	}
#else
	//
	// This method has code to support two modes of finding the next filename, one of
	// them starts with a filename of 00000000.sav and counts up the number looking for
	// a file that doesn't exist and therefore a filename we can use (LOWEST_NUMBER).
	// The other method iterates all save files and returns a filename that is one larger
	// than the largest filename number encountered (HIGHEST_NUMBER)
	//
	enum FindNextFileType { LOWEST_NUMBER = 0, HIGHEST_NUMBER = 1 };
	FindNextFileType searchType = LOWEST_NUMBER;

	if( searchType == HIGHEST_NUMBER )
	{

		// iterate all the save files in the directory and find the highest file number
		Int highFileNumber = -1;
		iterateSaveFiles( findHighFileNumber, &highFileNumber );

		// check for a filename that is too big (this is unlikely but theoretically possible)
		if( highFileNumber + 1 > MAX_SAVE_FILE_NUMBER )
			return AsciiString::TheEmptyString;

		// construct filename with a number higher than the highest one we found
		AsciiString filename;
		filename.format( "%08d%s", highFileNumber + 1, SAVE_GAME_EXTENSION );
		return filename;

	}
	else if( searchType == LOWEST_NUMBER )
	{
		AsciiString filename;
		AsciiString fullPath;
		Int i = 0;

		while( TRUE )
		{

			// construct filename (########.sav)
			filename.format( "%08d%s", i, SAVE_GAME_EXTENSION );

			// construct full path to file given the filename
			fullPath = getFilePathInSaveDirectory(filename);

			// if file does not exist we're all good
			if( _access( fullPath.str(), 0 ) == -1 )
				return filename;

			// test the text filename
			i++;

			// check for at the max limit (this is highly unlikely but possible)
			if( i > MAX_SAVE_FILE_NUMBER )
				return AsciiString::TheEmptyString;

		}

	}
	else
	{

		DEBUG_CRASH(( "GameState::findNextSaveFilename - Unknown file search type '%d'", searchType ));
		return AsciiString::TheEmptyString;

	}
#endif

	// no appropriate filename could be found, return the empty string
	return AsciiString::TheEmptyString;

}

// ------------------------------------------------------------------------------------------------
/** Save the current state of the engine in a save file
	* NOTE: filename is a *filename only* */
// ------------------------------------------------------------------------------------------------
SaveResult GameState::saveGame( AsciiString filename, UnicodeString desc,
													SaveFileType saveType, SnapshotType which )
{

	// if there is no filename, this is a new file being created, find an appropriate filename
	if( filename.isEmpty() )
		filename = findNextSaveFilename( desc );
	if( filename.isEmpty() )
	{

		DEBUG_CRASH(( "GameState::saveGame - Unable to find valid filename for save game" ));
		return SaveResult( SC_NO_FILE_AVAILABLE );

	}

	// make absolutely sure the save directory exists
	CreateDirectory( getSaveDirectory().str(), nullptr );

	// construct path to file
	AsciiString filepath = getFilePathInSaveDirectory(filename);

	// save description as current description in the game state
	m_gameInfo.description = desc;

	// open the save file
	// GeneralsX: Write the save to a temporary file and put it in place of the save only once it
	// is complete. Opening the real path truncated it before the first block was written, so a save that failed halfway
	// (a full disk, a block error) left a truncated file where the player's earlier save in that slot had been, and the
	// list then silently dropped it (sweep c153-02).
	// The temporary sits next to the save (same folder, same volume, whatever links the player made: one rename puts it
	// in place) and swaps ".sav" for ".tmp", so its path is no longer than the save's. It is only ever
	// CREATED (openNew): an existing file under that name - a leftover, or the map a crafted save extracted as an alias
	// of it, which this very save reads back - is never truncated; the next of ten names is tried instead.
	// Trade-off: ten names, so ten leftovers from saves killed mid-write make this slot unsavable (SC_UNABLE_TO_OPEN_FILE);
	// delete stale ".tm?" files in the save list's scan if that ever happens.
	AsciiString tmpBase = filepath;
	if( tmpBase.endsWithNoCase( ".sav" ) )
		tmpBase.truncateBy( 4 );
	AsciiString tmpPath;
	XferSave xferSave;
	Bool opened = FALSE;
	try {
		for( Int attempt = 0; attempt < 10 && !opened; ++attempt )
		{
			tmpPath = tmpBase;
			tmpPath.concat( ".tm" );
			tmpPath.concat( attempt == 0 ? 'p' : (char)( '0' + attempt ) );
			opened = xferSave.openNew( tmpPath );
		}
	} catch(...) {
		opened = FALSE;
	}
	if( !opened )
	{
		DEBUG_LOG(( "Error opening a temporary file for '%s'", filepath.str() ));
		return SaveResult( SC_UNABLE_TO_OPEN_FILE, filename );
	}

	// save our save file type
	SaveGameInfo *gameInfo = getSaveGameInfo();
	gameInfo->saveFileType = saveType;

	// save our mission map name if applicable
	if( saveType == SAVE_FILE_TYPE_MISSION )
		gameInfo->missionMapName = TheCampaignManager->getCurrentMap();
	else
		gameInfo->missionMapName.clear();

	// set the pristine map to the current campaign map
	// this is now done during startNewGame()
//	gameInfo->pristineMapName = TheCampaignManager->getCurrentMap();

	// write the save file
	try
	{

		// save file
		xferSaveData( &xferSave, which );

	}
	catch( ... )
	{

		// close the file, drop the partial save and get out of here; the old save is untouched
		xferSave.close();
		remove( tmpPath.str() );
		return SaveResult( SC_ERROR, filename );

	}

	// close the file and put it in place of the old save
	if( !xferSave.commitAndReplace( filepath ) )
		return SaveResult( SC_ERROR, filename );

	return SaveResult( SC_OK, filename );

}

// ------------------------------------------------------------------------------------------------
/** A mission save */
// ------------------------------------------------------------------------------------------------
SaveResult GameState::missionSave()
{

	// get campaign
	Campaign *campaign = TheCampaignManager->getCurrentCampaign();

	// get mission #
	Int missionNumber = TheCampaignManager->getCurrentMissionNumber() + 1;

	// format a string for the mission save description
	UnicodeString format = TheGameText->fetchGameOnly( "GUI:MissionSave" );
	UnicodeString desc;
	desc.format( format, TheGameText->fetch( campaign->m_campaignNameLabel ).str(), missionNumber );

	// do an automatic mission save
	return saveGame( "", desc, SAVE_FILE_TYPE_MISSION );

}

// ------------------------------------------------------------------------------------------------
/** Load the save game pointed to by filename */
// ------------------------------------------------------------------------------------------------
SaveCode GameState::loadGame( AvailableGameInfo gameInfo )
{
	// sanity check for file
	if( doesSaveGameExist( gameInfo.filename ) == FALSE )
		return SC_FILE_NOT_FOUND;

	// clear game data just like loading from the debug map load screen for mission saves
	if( gameInfo.saveGameInfo.saveFileType == SAVE_FILE_TYPE_MISSION )
	{

		if (TheGameLogic->isInGame())
			TheGameLogic->clearGameData( FALSE );

	}

	//
	// clear the save directory of any temporary "scratch pad" maps that were extracted
	// from any previously loaded save game files
	//
	TheGameStateMap->clearScratchPadMaps();

	AsciiString filepath = getSaveGamePathForRead(gameInfo.filename);

	// open the save file
	XferLoad xferLoad;
	xferLoad.open( filepath );

	// clear out the game engine
	TheGameEngine->reset();

	// lock creation of new ghost objects
	TheGhostObjectManager->saveLockGhostObjects( TRUE );

	LatchRestore<Bool> inLoadGame(m_isInLoadGame, TRUE);

	// load the save data
	Bool error = FALSE;
	try
	{

		// load file
		xferSaveData( &xferLoad, SNAPSHOT_SAVELOAD );

	}
	catch( XferStatus status )
	{
		fprintf( stderr, "[SAVELOAD] '%s' failed to load: xfer status %d (a save from the original game has another layout; see RETAIL_COMPATIBLE_XFER_SAVE)\n", filepath.str(), (Int)status );
		error = TRUE;
	}
	catch( ... )
	{
		fprintf( stderr, "[SAVELOAD] '%s' failed to load: exception that is not an xfer status\n", filepath.str() );
		error = TRUE;
	}

	// close the file
	xferLoad.close();

	// un-savelock the ghost objects
	TheGhostObjectManager->saveLockGhostObjects( FALSE );

	try
	{
		// do the post-process from a save game load
		gameStatePostProcessLoad();
	}
	catch (...)
	{
		fprintf( stderr, "[SAVELOAD] '%s' failed to load: exception in the post-load step\n", filepath.str() );
		error = TRUE;
	}

	// check for error
	if( error == TRUE )
	{
		// clear it out, again
		if (TheGameLogic->isInGame())
			TheGameLogic->clearGameData( FALSE );
		TheGameEngine->reset();

		return SC_INVALID_DATA;	// you can't use a naked "throw" outside of a catch statement!

	}

	//
	// when loading a mission save, we want to do as much normal loading stuff as we
	// can cause we don't have any real save game data to load other than the
	// game state map info and campaign manager stuff
	//
	if( getSaveGameInfo()->saveFileType == SAVE_FILE_TYPE_MISSION )
	{

		InitRandom(0);

		TheWritableGlobalData->m_pendingFile = getSaveGameInfo()->missionMapName;
		GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
		msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
		msg->appendIntegerArgument(TheCampaignManager->getGameDifficulty());
		msg->appendIntegerArgument(TheCampaignManager->getRankPoints());

		// remove the mission save data, we've got all we need and have started the load
		SaveGameInfo *gameInfo = getSaveGameInfo();
		gameInfo->saveFileType = SAVE_FILE_TYPE_NORMAL;
		gameInfo->missionMapName.clear();

	}

	return SC_OK;

}

//-------------------------------------------------------------------------------------------------
static void showQueuedSaveGameLoadFailure( void )
{
	UnicodeString title = TheGameText->FETCH_OR_SUBSTITUTE("GUI:SaveGameLoadFailedTitle", L"CANNOT LOAD SAVE");
	UnicodeString body = TheGameText->FETCH_OR_SUBSTITUTE("GUI:SaveGameLoadFailed", L"The saved game file could not be opened or is invalid.");

	MessageBoxOk(title, body, nullptr);
}

// ------------------------------------------------------------------------------------------------
/** Load the save game requested on startup, after the shell has been initialized */
// ------------------------------------------------------------------------------------------------
void GameState::loadQueuedSaveGame()
{
	AvailableGameInfo gameInfo;
	gameInfo.filename = TheGlobalData->m_loadSaveGame;
	gameInfo.next = nullptr;
	gameInfo.prev = nullptr;

	TheWritableGlobalData->m_loadSaveGame.clear();

	if( gameInfo.filename.endsWithNoCase( SAVE_GAME_EXTENSION ) == FALSE )
	{
		DEBUG_LOG(("Save game '%s' is not a save game file", gameInfo.filename.str()));
		showQueuedSaveGameLoadFailure();
		return;
	}

	// getSaveGameInfoFromFile throws when the file is missing, so check before reading it
	if( doesSaveGameExist( gameInfo.filename ) == FALSE )
	{
		DEBUG_LOG(("Save game '%s' was not found", gameInfo.filename.str()));
		showQueuedSaveGameLoadFailure();
		return;
	}

	// getSaveGameInfoFromFile throws on a malformed file instead of returning a SaveCode
	try
	{
		getSaveGameInfoFromFile( gameInfo.filename, &gameInfo.saveGameInfo );
	}
	catch( ... )
	{
		DEBUG_LOG(("Save game '%s' could not be read", gameInfo.filename.str()));
		showQueuedSaveGameLoadFailure();
		return;
	}

	// this hides the shell, keeping the menu screens on the stack for when the game ends
	TheGameLogic->prepareNewGame( GAME_SINGLE_PLAYER, DIFFICULTY_NORMAL, 0 );

	if( loadGame( gameInfo ) != SC_OK )
	{
		DEBUG_LOG(("Failed to load save game '%s'", gameInfo.filename.str()));
		if( TheGameLogic->isInGame() )
			TheGameLogic->clearGameData( FALSE );
		TheGameEngine->reset();
		TheGameEngine->setQuitting( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
AsciiString GameState::getSaveDirectory() const
{
	AsciiString tmp = TheGlobalData->getPath_UserData();
	// GeneralsX: Use POSIX separator on non-Windows so saves are created inside a real Save directory.
#ifdef _WIN32
	tmp.concat("Save\\");
#else
	tmp.concat("Save/");
#endif
	return tmp;
}

//-------------------------------------------------------------------------------------------------
AsciiString GameState::getFilePathInSaveDirectory(const AsciiString& leaf) const
{
	AsciiString tmp = getSaveDirectory();
	tmp.concat(leaf);
	return tmp;
}

//-------------------------------------------------------------------------------------------------
AsciiString GameState::getSaveGamePathForRead(const AsciiString& filenameOrPath) const
{
	if (isAbsolutePath(filenameOrPath.str()))
	{
		return filenameOrPath;
	}

	return getFilePathInSaveDirectory(filenameOrPath);
}

//-------------------------------------------------------------------------------------------------
Bool GameState::isInSaveDirectory(const AsciiString& path) const
{
	return FileSystem::isPathInDirectory(path, getSaveDirectory());
}

// ------------------------------------------------------------------------------------------------
AsciiString GameState::getMapLeafName(const AsciiString& in) const
{
	const char* pBackslash = strrchr(in.str(), '\\');
	const char* pSlash = strrchr(in.str(), '/');
	const char* p = pBackslash;
	if (p == nullptr || (pSlash != nullptr && pSlash > p))
	{
		p = pSlash;
	}
	if (p)
	{
		//
		// p points to the last path separator (if found), however, if a separator was found there better
		// be another character beyond it, otherwise the map filename would actually
		// be a *directory*  Just move to the first character beyond it so we are looking
		// at the name only
		//
		++p;
		DEBUG_ASSERTCRASH( p != nullptr && *p != 0, ("GameState::xfer - Illegal map name encountered") );
		return p;
	}
	else
	{
		return in;
	}
}

// ------------------------------------------------------------------------------------------------
static const char* findLastPathSeparatorInRangeInclusive(const char* start, const char* end)
{
	while (end >= start)
	{
		if (*end == '\\' || *end == '/')
			return end;
		--end;
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: Portable map paths are NOT separator-portable, and the map
// cache key is a raw string compare, so the two have to be brought into the same shape here.
//
// MapCache keys are whatever the enumerator returned, which is platform-native throughout:
// StdLocalFileSystem hands back std::filesystem paths (all '/'), Win32LocalFileSystem builds
// originalDirectory + currentDirectory + name (all '\'). GameInfo::setMapCRC then looks the map up
// with std::map::find on the string portableMapPathToRealMapPath produced. Two separators leak into
// that string on POSIX and each is enough on its own to miss:
//
//   * the join was a hard-coded "\\" - so macOS built ".../maps\name/name.map" against a stored
//     ".../maps/name/name.map", differing by exactly ONE character;
//   * the TAIL comes from the portable path, which crosses the wire un-normalised
//     (realMapPathToPortableMapPath only lowercases). A Windows host therefore hands a macOS
//     joiner "name\name.map", so fixing the join alone would still leave a backslash in the tail.
//
// The result was that a transferred map was unfindable on macOS/Linux/iPadOS while Windows - where
// every separator is a backslash already - matched fine. Built-in maps were never affected because
// the archive enumerator also emits backslashes on every platform.
//
// File access itself always worked: StdLocalFileSystem::fixFilenameFromWindowsPath replaces
// separators on the way in. Only the cache lookup, which compares raw strings, could see this.
static AsciiString toPlatformPathSeparators(const AsciiString& in)
{
#ifdef _WIN32
	const char from = '/', to = '\\';
#else
	const char from = '\\', to = '/';
#endif
	std::string s(in.str());
	for (std::string::iterator it = s.begin(); it != s.end(); ++it)
	{
		if (*it == from)
			*it = to;
	}
	return AsciiString(s.c_str());
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: Make the PORTABLE path actually portable.
//
// A portable map path goes ON THE WIRE and is CRC'd on both sides: LANAPI::RequestHasMap CRCs it
// into MapStatus.mapCRC, and LANAPI::handleHasMap recomputes it locally and SILENTLY RETURNS on a
// mismatch (LANAPIhandlers.cpp). So if two peers derive different strings for the same map, the
// host never learns the joiner lacks it, the transfer mask stays empty, no file is ever sent, and
// the joiner sits until its two-minute timeout and reports "Unable to transfer the map".
//
// getMapLeafAndDirName preserves whatever separators its INPUT had, and the input is the local real
// path - so the portable string inherited the local platform's separator. Windows produced
// "userdata\maps\foo\foo.map" and POSIX "userdata\maps\foo/foo.map" for the same map. Measured
// end to end 2026-08-10: a Windows host and a macOS joiner, host started the match alone having
// sent nothing while the joiner was simultaneously printing "You do not have the map".
//
// The PORTABLE_* prefixes are already backslash-shaped ("UserData\\Maps\\"), so backslash is the
// canonical portable separator; forcing it here makes the string identical on every platform and
// costs nothing, since portableMapPathToRealMapPath converts back on the way in.
static AsciiString toPortablePathSeparators(const AsciiString& in)
{
	std::string s(in.str());
	for (std::string::iterator it = s.begin(); it != s.end(); ++it)
	{
		if (*it == '/')
			*it = '\\';
	}
	return AsciiString(s.c_str());
}

// ------------------------------------------------------------------------------------------------
static AsciiString getMapLeafAndDirName(const AsciiString& in)
{
	const char* start = in.str();
	const char* end = in.str() + in.getLength() - 1;
	const char* p = findLastPathSeparatorInRangeInclusive(start, end);
	if (p)
	{
		const char* p2 = findLastPathSeparatorInRangeInclusive(start, p-1);
		if (p2)
		{
			// we have something like:
			//	maps\foo\foo.map
			//	maps/foo/foo.map
			//	c:\mydocs\c&cdata\maps\foo\foo.map
			return p2 + 1;
		}
		else
		{
			// we have something like:
			//  save\foo.map
			return in;
		}
	}
	else
	{
		DEBUG_CRASH(("Illegal map-dir-name... should have at least one backslash"));
		return in;
	}
}

// ------------------------------------------------------------------------------------------------
const char* PORTABLE_SAVE				= "Save\\";
const char* PORTABLE_MAPS				= "Maps\\";
const char* PORTABLE_USER_MAPS	= "UserData\\Maps\\";

// ------------------------------------------------------------------------------------------------
AsciiString GameState::realMapPathToPortableMapPath(const AsciiString& in) const
{
	AsciiString prefix;
	if (in.startsWithNoCase(getSaveDirectory()))
	{
		prefix = PORTABLE_SAVE;
		prefix.concat(getMapLeafName(in));
	}
	else if (in.startsWithNoCase(TheMapCache->getMapDir()))
	{
		prefix = PORTABLE_MAPS;
		prefix.concat(getMapLeafAndDirName(in));
	}
	else if (in.startsWithNoCase(TheMapCache->getUserMapDir()))
	{
		prefix = PORTABLE_USER_MAPS;
		prefix.concat(getMapLeafAndDirName(in));
	}
	else
	{
		DEBUG_CRASH(("Map file was not found in any of the expected directories; this is impossible"));
		//throw INI_INVALID_DATA;
		// uncaught exceptions crash us. better to just use a bad path.
		// GeneralsX: Release-visible, capped: this "bad path" is invisible
		// in shipping builds (DEBUG_CRASH compiles out) yet it used to go on the WIRE as a
		// "portable" name the far side silently refuses. The send path now checks
		// isPortableFilePath and refuses to announce; this log is how an operator learns which
		// input produced the unconvertible path. Capped because polls can hit this too.
		static Int s_fallbackLogs = 0;
		if (s_fallbackLogs < 4)
		{
			++s_fallbackLogs;
			fprintf(stderr, "[GXFILE] portable-path fallback for '%s' - not under save/maps/user-maps\n", in.str());
			fflush(stderr);
		}
		prefix = in;
	}
	// GeneralsX: Canonical separator BEFORE toLower, so the string this
	// returns is byte-identical on every platform. It is CRC'd across the wire - see
	// toPortablePathSeparators above for what a per-platform spelling costs.
	prefix = toPortablePathSeparators(prefix);
	prefix.toLower();
	return prefix;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX: TRUE iff this string is a genuinely portable path - one of
// the three PORTABLE_* families realMapPathToPortableMapPath can produce on its non-fallback
// branches. The fallback ships the raw local input as a "portable" name; every receiver's
// normalization refuses that silently, so the sender must be able to recognise it BEFORE putting
// it on the wire and starving the lobby for the whole transfer timeout.
Bool GameState::isPortableFilePath(const AsciiString& in) const
{
	return in.startsWithNoCase(PORTABLE_SAVE)
		|| in.startsWithNoCase(PORTABLE_MAPS)
		|| in.startsWithNoCase(PORTABLE_USER_MAPS);
}

// ------------------------------------------------------------------------------------------------
AsciiString GameState::portableMapPathToRealMapPath(const AsciiString& in) const
{
	AsciiString prefix;
	// The directory where the real map path should be contained in.
	AsciiString containingBasePath;
	if (in.startsWithNoCase(PORTABLE_SAVE))
	{
		// the save dir ends with "\\"
		prefix = getSaveDirectory();
		containingBasePath = prefix;
		prefix.concat(getMapLeafName(in));
	}
	else if (in.startsWithNoCase(PORTABLE_MAPS))
	{
		// the map dir DOES NOT end with "\\", must add it
		prefix = TheMapCache->getMapDir();
		prefix.concat("\\");
		containingBasePath = prefix;
		prefix.concat(getMapLeafAndDirName(in));

		// GeneralsX: Platform-native separators, computed DETERMINISTICALLY.
		// archive-backed (backslash key on every platform) or loose on disk (native key), so this
		// has to follow the cache rather than assume either.
		prefix = toPlatformPathSeparators(prefix);
		containingBasePath = toPlatformPathSeparators(containingBasePath);
	}
	else if (in.startsWithNoCase(PORTABLE_USER_MAPS))
	{
		// the map dir DOES NOT end with "\\", must add it
		prefix = TheMapCache->getUserMapDir();
		prefix.concat("\\");
		containingBasePath = prefix;
		prefix.concat(getMapLeafAndDirName(in));

		// GeneralsX: The reported bug. User maps are always loose on disk,
		// so the key is platform-native, while this string was built with a hard-coded '\\' join and
		// a tail that crossed the wire un-normalised. Same cache-following treatment as the stock
		// branch, which also gets the not-yet-cached case right: a map being RECEIVED is not in the
		// cache when this runs; the native form is what the filesystem and the rescan both produce.
		// is what the filesystem wants and what the post-transfer rescan will key it under.
		prefix = toPlatformPathSeparators(prefix);
		containingBasePath = toPlatformPathSeparators(containingBasePath);
	}
	else
	{
		DEBUG_CRASH(("Map file was not found in any of the expected directories; this is impossible"));
		// Empty string represents a failure, either caused by an invalid prefix or a relative path leading outside the base path.
		return AsciiString::TheEmptyString;
	}

	if (!FileSystem::isPathInDirectory(prefix, containingBasePath))
	{
		DEBUG_LOG(("Normalized file path for '%s' was outside the expected base path of '%s'.", prefix.str(), containingBasePath.str()));
		return AsciiString::TheEmptyString;
	}
	// GeneralsX: Preserve path casing on macOS/Linux to avoid load failures on case-sensitive filesystems.
#ifdef _WIN32
	prefix.toLower();
#endif
	return prefix;
}

// ------------------------------------------------------------------------------------------------
/** Does the save game file exist */
// ------------------------------------------------------------------------------------------------
Bool GameState::doesSaveGameExist( AsciiString filename )
{

	AsciiString filepath = getSaveGamePathForRead(filename);

	// open file
	XferLoad xfer;
	try
	{

		// try to open it
		xfer.open( filepath );

	}
	catch( ... )
	{
		// unable to open file, it must not be here
		return FALSE;

	}

	// close the file, we don't want to to anything with it right now
	xfer.close();

	return TRUE;

}

// ------------------------------------------------------------------------------------------------
/** Get save game info from the filename specified */
// ------------------------------------------------------------------------------------------------
void GameState::getSaveGameInfoFromFile( AsciiString filename, SaveGameInfo *saveGameInfo )
{
	AsciiString token;
	Int blockSize;
	Bool done = FALSE;
	SnapshotBlock *blockInfo;

	// sanity
	if( filename.isEmpty() == TRUE || saveGameInfo == nullptr )
	{

		DEBUG_CRASH(( "GameState::getSaveGameInfoFromFile - Illegal parameters" ));
		return;

	}

	filename = getSaveGamePathForRead( filename );

	// open file for partial loading
	XferLoad xferLoad;
	xferLoad.open( filename );

	//
	// disable post processing cause we're not really doing a load of game data that
	// needs post processing and we don't want to keep track of any snapshots we loaded
	//
	xferLoad.setOptions( XO_NO_POST_PROCESSING );

	// read all data blocks in the file
	while( done == FALSE )
	{

		// read next token
		xferLoad.xferAsciiString( &token );

		// check for end of file token
		if( token.compareNoCase( SAVE_FILE_EOF ) == 0 )
		{

			// we should never get here, if we did, we didn't find block of data we needed
			DEBUG_CRASH(( "GameState::getSaveGameInfoFromFile - Game info not found in file '%s'", filename.str() ));
			done = TRUE;

		}
		else
		{

			// find matching token in the save file lexicon
			blockInfo = findBlockInfoByToken( token, SNAPSHOT_SAVELOAD );
			if( blockInfo == nullptr )
				throw SC_UNKNOWN_BLOCK;

			// read the data size of this block
			blockSize = xferLoad.beginBlock();

			// is this the block of game info data
			if( stricmp( token.str(), GAME_STATE_BLOCK_STRING ) == 0 )
			{
				GameState tempGameState;

				// parse this data
				try
				{

					// load data
					xferLoad.xferSnapshot( &tempGameState );

				}
				catch( ... )
				{

					DEBUG_CRASH(( "GameState::getSaveGameInfoFromFile - Error loading block '%s' in file '%s'",
												blockInfo->blockName.str(), filename.str() ));
					throw;

				}

				// data was found, copy game state info over
				*saveGameInfo = *tempGameState.getSaveGameInfo();

				// we're all done with this file now
				done = TRUE;

			}
			else
			{

				// not a block we care about, just skip it
				xferLoad.skip( blockSize );

				// end of block
				xferLoad.endBlock();

			}

		}

	}

	// close the file
	xferLoad.close();

}

// ------------------------------------------------------------------------------------------------
/** Create game info and add to available list */
// ------------------------------------------------------------------------------------------------
static void addGameToAvailableList( AsciiString filename, void *userData )
{
	AvailableGameInfo **listHead = (AvailableGameInfo **)userData;

	// sanity
	DEBUG_ASSERTCRASH( listHead != nullptr, ("addGameToAvailableList - Illegal parameters") );
	DEBUG_ASSERTCRASH( filename.isEmpty() == FALSE, ("addGameToAvailableList - Illegal filename") );

	try {
	// get header info from this listbox
	SaveGameInfo saveGameInfo;
	TheGameState->getSaveGameInfoFromFile( filename, &saveGameInfo );

	// allocate new info
	AvailableGameInfo *newInfo = new AvailableGameInfo;

	// assign data
	newInfo->prev = nullptr;
	newInfo->next = nullptr;
	newInfo->saveGameInfo = saveGameInfo;
	newInfo->filename = filename;

	// attach to list
	if( *listHead == nullptr )
		*listHead = newInfo;
	else
	{
		AvailableGameInfo *curr, *prev;

		// insert this info so that the most recent games are always at the top of this list
		for( curr = *listHead; curr != nullptr; curr = curr->next )
		{

			// save current as previous
			prev = curr;

			// check to see if curr is older than the new info, if so, put new info just ahead of curr
			if( newInfo->saveGameInfo.date.isNewerThan( &curr->saveGameInfo.date ) )
			{

				if( curr->prev )
					curr->prev->next = newInfo;
				else
					*listHead = newInfo;
				newInfo->prev = curr->prev;
				curr->prev = newInfo;
				newInfo->next = curr;

				break;

			}

		}

		// if not inserted, put at end
		if( curr == nullptr )
		{

			prev->next = newInfo;
			newInfo->prev = prev;

		}

	}
	} catch(...) {
		// ignore invalid save entries
	}


}

// ------------------------------------------------------------------------------------------------
/** Populate the listbox passed in with a list of the save games present on the hard drive */
// ------------------------------------------------------------------------------------------------
void GameState::populateSaveGameListbox( GameWindow *listbox, SaveLoadLayoutType layoutType )
{
	Int index;

	// sanity
	if( listbox == nullptr )
		return;

	// first clear all entries in the listbox
	GadgetListBoxReset( listbox );

	// setup the first entry of the listbox to be a new game when saving is allowed
	if( layoutType != SLLT_LOAD_ONLY )
	{
		UnicodeString newGameText = TheGameText->fetch( "GUI:NewSaveGame" );
		Color newGameColor = GameMakeColor( 200, 200, 255, 255 );

		index = GadgetListBoxAddEntryText( listbox, newGameText, newGameColor, -1 );
		GadgetListBoxSetItemData( listbox, nullptr, index );

	}

	// clear the available games
	clearAvailableGames();

	// iterate all the save files in the directory and populate the listbox
	iterateSaveFiles( addGameToAvailableList, &m_availableGames );

	// add all games found to the list box
	AvailableGameInfo *info;
	SaveGameInfo *saveGameInfo;
	SYSTEMTIME systemTime;
	UnsignedInt count = 0;
	for( info = m_availableGames; info; info = info->next, count++ )
	{

		// get save game info
		saveGameInfo = &info->saveGameInfo;

		// setup a system time structure given the data we saved in the file
		systemTime.wYear = saveGameInfo->date.year;
		systemTime.wMonth = saveGameInfo->date.month;
		systemTime.wDayOfWeek = saveGameInfo->date.dayOfWeek;
		systemTime.wDay = saveGameInfo->date.day;
		systemTime.wHour = saveGameInfo->date.hour;
		systemTime.wMinute = saveGameInfo->date.minute;
		systemTime.wSecond = saveGameInfo->date.second;
		systemTime.wMilliseconds = saveGameInfo->date.milliseconds;

		// setup date buffer for local region date format
		UnicodeString displayDateBuffer = getUnicodeDateBuffer(systemTime);

		// setup time buffer for local region time format
		UnicodeString displayTimeBuffer = getUnicodeTimeBuffer(systemTime);

		// description string
		UnicodeString displayLabel = saveGameInfo->description;
		if( displayLabel.isEmpty() == TRUE )
		{
			Bool exists = FALSE;

			displayLabel = TheGameText->fetch( saveGameInfo->mapLabel, &exists );
			if( exists == FALSE )
				displayLabel.format( L"%S", saveGameInfo->mapLabel.str() );

		}

		// pick color for text (we alternate it each game)
		Color color;
		if( saveGameInfo->saveFileType == SAVE_FILE_TYPE_MISSION )
			color = GameMakeColor( 200, 255, 200, 255 );
		else if( count & 0x1 )
			color = GameMakeColor( 255, 255, 255, 255 );
		else
			color = GameMakeColor( 170, 170, 235, 255 );

		// add string to listbox
		index = GadgetListBoxAddEntryText( listbox, displayLabel, color, -1, 0 );
		GadgetListBoxAddEntryText( listbox, displayTimeBuffer, color, index, 1 );
		GadgetListBoxAddEntryText( listbox, displayDateBuffer, color, index, 2 );

		// add this available game info in the user data pointer of that listbox item
		GadgetListBoxSetItemData( listbox, info, index );

	}

	// select the top "new game" entry
	GadgetListBoxSetSelected( listbox, 0 );

}

///////////////////////////////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS ////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

// ------------------------------------------------------------------------------------------------
/** Iterate the save game files */
// ------------------------------------------------------------------------------------------------
void GameState::iterateSaveFiles( IterateSaveFileCallback callback, void *userData )
{

	// sanity
	if( callback == nullptr )
		return;

	// GeneralsX: Both branches used to change the process working directory
	// to the Save folder for the scan (and the Windows one ignored a failed change, then listed the
	// game directory). The cwd is process-wide - dataID and any other thread's relative path resolve
	// against it - and nothing here needs it: the callbacks get bare leaf names and resolve them
	// against the Save folder themselves (getSaveGamePathForRead). Scan by the Save-folder path.
#ifdef _WIN32
	// iterate all items in the save directory
	WIN32_FIND_DATA item;  // search item
	HANDLE hFile = INVALID_HANDLE_VALUE;  // handle for search resources
#else
	// GeneralsX: Guard save-directory iteration against filesystem exceptions and invalid path lifetimes on macOS/Linux
	AsciiString saveDirPath = getSaveDirectory();

	try {
		for( const auto &entry : std::filesystem::directory_iterator( saveDirPath.str() ) ) {
			if( entry.is_regular_file() ) {
				std::string filename_str = entry.path().filename().string();

				// See if there is a ".sav" at end of this filename
				const char *c = strrchr( filename_str.c_str(), '.' );
				if( c && stricmp( c, ".sav" ) == 0 ) {
					AsciiString filename;
					filename.set( filename_str.c_str() );
					callback( filename, userData );
				}
			}
		}
	} catch( const std::filesystem::filesystem_error &e ) {
#ifdef _DEBUG
		DEBUG_LOG(( "GameState::iterateSaveFiles failed while iterating save directory '%s': %s\r\n", saveDirPath.str(), e.what() ));
#endif
	} catch( ... ) {
#ifdef _DEBUG
		DEBUG_LOG(( "GameState::iterateSaveFiles failed with unknown exception while iterating save directory '%s'\r\n", saveDirPath.str() ));
#endif
	}

	return;
#endif

// GeneralsX: Windows file iteration (protected for Linux build)
#ifdef _WIN32
	Bool done = FALSE;
	Bool first = TRUE;
	while( done == FALSE )
	{

		// if our first time through we need to start the search
		if( first )
		{

			// start search
			hFile = FindFirstFile( getFilePathInSaveDirectory( "*" ).str(), &item );
			if( hFile == INVALID_HANDLE_VALUE )
				return;

			// we are no longer on our first item
			first = FALSE;

		}

		// see if this is a file, and therefore a possible save file
		if( !(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) )
		{

			// see if there is a ".sav" at end of this filename
			Char *c = strrchr( item.cFileName, '.' );
			if( c && stricmp( c, ".sav" ) == 0 )
			{

				// construction asciistring filename
				AsciiString filename;
				filename.set( item.cFileName );

				// call the callback
				callback( filename, userData );

			}

		}

		// on to the next file
		if( FindNextFile( hFile, &item ) == 0 )
			done = TRUE;

	}

	// close search resources
	FindClose( hFile );
#endif

}

// ------------------------------------------------------------------------------------------------
/** Save game to xfer or load game using xfer */
// ------------------------------------------------------------------------------------------------
void GameState::friend_xferSaveDataForCRC( Xfer *xfer, SnapshotType which )
{
	DEBUG_LOG(("GameState::friend_xferSaveDataForCRC() - SnapshotType %d", which));
	SaveGameInfo *gameInfo = getSaveGameInfo();
	gameInfo->description.clear();
	gameInfo->saveFileType = SAVE_FILE_TYPE_NORMAL;
	gameInfo->missionMapName.clear();
	gameInfo->pristineMapName.clear();

	xferSaveData(xfer, which);
}

// ------------------------------------------------------------------------------------------------
/** Save game to xfer or load game using xfer */
// ------------------------------------------------------------------------------------------------
void GameState::xferSaveData( Xfer *xfer, SnapshotType which )
{

	// sanity
	if( xfer == nullptr )
		throw SC_INVALID_XFER;

	// save or load all blocks
	if( xfer->getXferMode() == XFER_SAVE )
	{
		DEBUG_LOG(("GameState::xferSaveData() - XFER_SAVE"));

		// save all blocks
		AsciiString blockName;
		SnapshotBlock *blockInfo;
		SnapshotBlockListIterator it;
		for( it = m_snapshotBlockList[which].begin(); it != m_snapshotBlockList[which].end(); ++it )
		{

			// get list data
			blockInfo = &(*it);

			// get block name
			blockName = blockInfo->blockName;

			DEBUG_LOG(("Looking at block '%s'", blockName.str()));

			//
			// for mission save files, we only save the game state block and campaign manager
			// because anything else is not needed.
			//
			if( getSaveGameInfo()->saveFileType != SAVE_FILE_TYPE_MISSION ||
					(blockName.compareNoCase( GAME_STATE_BLOCK_STRING ) == 0 ||
					 blockName.compareNoCase( CAMPAIGN_BLOCK_STRING ) == 0) )
			{

				// xfer block name
				xfer->xferAsciiString( &blockName );

				// xfer this block
				try
				{

					// begin new data block
					xfer->beginBlock();

					// xfer block data
					xfer->xferSnapshot( blockInfo->snapshot );

					// end this block
					xfer->endBlock();

				}
				catch( ... )
				{

					DEBUG_CRASH(( "Error saving block '%s' in file '%s'",
												blockName.str(), xfer->getIdentifier().str() ));
					throw;

				}

			}

		}

		// write an end of file token
		AsciiString eofToken = SAVE_FILE_EOF;
		xfer->xferAsciiString( &eofToken );

	}
	else
	{
		DEBUG_LOG(("GameState::xferSaveData() - not XFER_SAVE"));
		AsciiString token;
		Int blockSize;
		Bool done = FALSE;
		SnapshotBlock *blockInfo;

		// read all data blocks in the file
		while( done == FALSE )
		{

			// read next token
			xfer->xferAsciiString( &token );

			// check for end of file token
			if( token.compareNoCase( SAVE_FILE_EOF ) == 0 )
			{

				// all done
				done = TRUE;

			}
			else
			{

				// find matching token in the save file lexicon
				blockInfo = findBlockInfoByToken( token, which );
				if( blockInfo == nullptr )
				{

					// log the block not found
					DEBUG_LOG(( "GameState::xferSaveData - Skipping unknown block '%s'", token.str() ));

					//
					// block was not found, this could have been a block from an older file
					// format where the block was removed, skip the block data and try to continue
					//
					Int dataSize = xfer->beginBlock();
					xfer->skip( dataSize );

					// continue with while loop reading block tokens
					continue;

				}

				try
				{

					// read block start
					blockSize = xfer->beginBlock();

					// parse this data
					xfer->xferSnapshot( blockInfo->snapshot );

					// read block end
					xfer->endBlock();

				}
				catch( XferStatus status )
				{

					// GeneralsX: say WHY a load failed (the generic catch below prints nothing in a release build)
					fprintf( stderr, "[SAVELOAD] load failed in block '%s' of '%s': xfer status %d\n",
						blockInfo->blockName.str(), xfer->getIdentifier().str(), (Int)status );
					throw;

				}
				catch( ... )
				{

					DEBUG_CRASH(( "Error loading block '%s' in file '%s'",
												blockInfo->blockName.str(), xfer->getIdentifier().str() ));
					throw;

				}

			}

		}

	}

}

// ------------------------------------------------------------------------------------------------
/** Add a snapshot to the post process list for later */
// ------------------------------------------------------------------------------------------------
void GameState::addPostProcessSnapshot( Snapshot *snapshot )
{

	// sanity
	if( snapshot == nullptr )
	{

		DEBUG_CRASH(( "GameState::addPostProcessSnapshot - invalid parameters" ));
		return;

	}

/*
	//
	// This is n^2 and gets real real, REAL slow on game maps.  jba.
	// Please keep this code around tho, it can be useful in debugging save games
	//

	// verify the snapshot isn't in the list already
	SnapshotListIterator it;
	for( it = m_snapshotPostProcessList.begin(); it != m_snapshotPostProcessList.end(); ++it )
	{

		if( (*it) == snapshot )
		{

			DEBUG_CRASH(( "GameState::addPostProcessSnapshot - snapshot is already in list!" ));
			return;

		}

	}
*/

	// add to the list
	m_snapshotPostProcessList.push_back( snapshot );

}

// ------------------------------------------------------------------------------------------------
/** Post process entry point after all game data has been xferd from disk */
// ------------------------------------------------------------------------------------------------
void GameState::gameStatePostProcessLoad()
{

	// post process each snapshot that registered with us
	SnapshotListIterator it;
	Snapshot *snapshot;
	for( it = m_snapshotPostProcessList.begin(); it != m_snapshotPostProcessList.end(); /*emtpy*/ )
	{

		// get snapshot
		snapshot = *it;

		// increment iterator
		++it;

		// do processing
		snapshot->loadPostProcess();

	}

	// clear the snapshot post process list as we are now done with it
	m_snapshotPostProcessList.clear();

	// evil... must ensure this is updated prior to the script engine running the first time.
	ThePartitionManager->update();

}

// ------------------------------------------------------------------------------------------------
/** Xfer method for the game state itself
	* Version Info:
	* 1: Initial version
	* 2: Added save file type and mission map name (regular save vs automatic mission save)
	* 3: GeneralsX 06/11/2026 - nothing new in this block; it marks the 40-Player layout (MAX_PLAYER_COUNT 16 -> 40,
	*    64-bit player masks) of every block after it. Version 2 and older are refused at load (below). */
// ------------------------------------------------------------------------------------------------
void GameState::xfer( Xfer *xfer )
{

	// version
	XferVersion currentVersion = 3;
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	// GeneralsX: A save written before 32-player games has 16-wide player arrays and 16-bit
	// player masks in dozens of blocks with no per-block count, so reading it on this layout would misparse silently.
	// Refuse it cleanly at LOAD (loadGame catches this and reports the load failed); the save list, which reads this
	// block alone through getSaveGameInfoFromFile, still lists it.
	if( version < 3 && xfer->getXferMode() == XFER_LOAD && m_isInLoadGame )
	{
		fprintf( stderr, "[SAVELOAD] refused: this save predates 32-player games (16-Player layout, GameState v%d); "
			"it cannot be loaded by this build\n", (Int)version );
		throw XFER_INVALID_VERSION;
	}

	// get structure for our current game info
	SaveGameInfo *saveGameInfo = getSaveGameInfo();

	// version 2
	if( version >= 2 )
	{

		// file type
		xfer->xferUser( &saveGameInfo->saveFileType, sizeof( SaveFileType ) );

		// mission map name
		xfer->xferAsciiString( &saveGameInfo->missionMapName );

	}

	// current system time
	SYSTEMTIME systemTime;
	GetLocalTime( &systemTime );

	// date and time
	saveGameInfo->date.year = systemTime.wYear;
	xfer->xferUnsignedShort( &saveGameInfo->date.year );
	saveGameInfo->date.month = systemTime.wMonth;
	xfer->xferUnsignedShort( &saveGameInfo->date.month );
	saveGameInfo->date.day = systemTime.wDay;
	xfer->xferUnsignedShort( &saveGameInfo->date.day );
	saveGameInfo->date.dayOfWeek = systemTime.wDayOfWeek;
	xfer->xferUnsignedShort( &saveGameInfo->date.dayOfWeek );
	saveGameInfo->date.hour = systemTime.wHour;
	xfer->xferUnsignedShort( &saveGameInfo->date.hour );
	saveGameInfo->date.minute = systemTime.wMinute;
	xfer->xferUnsignedShort( &saveGameInfo->date.minute );
	saveGameInfo->date.second = systemTime.wSecond;
	xfer->xferUnsignedShort( &saveGameInfo->date.second );
	saveGameInfo->date.milliseconds = systemTime.wMilliseconds;
	xfer->xferUnsignedShort( &saveGameInfo->date.milliseconds );

	// user description
	xfer->xferUnicodeString( &saveGameInfo->description );

	Bool exists = FALSE;
	Dict *dict = MapObject::getWorldDict();
	if( dict )
		saveGameInfo->mapLabel = dict->getAsciiString( TheKey_mapName, &exists );

	// if no label was found, we'll use the map name (just filename, no directory info)
	if (exists == FALSE || saveGameInfo->mapLabel == AsciiString::TheEmptyString)
	{
		const char* p = TheGlobalData->m_mapName.reverseFind('\\');
		if (p == nullptr)
			saveGameInfo->mapLabel = TheGlobalData->m_mapName;
		else
		{
			p++;  // skip the '\' we're on
			saveGameInfo->mapLabel.set(p);
		}
	}

	// xfer map label
	xfer->xferAsciiString( &saveGameInfo->mapLabel );

	// campaign info
	Campaign *campaign = TheCampaignManager->getCurrentCampaign();
	if( campaign )
	{

		// campaign side
		saveGameInfo->campaignSide = campaign->m_name;
		xfer->xferAsciiString( &saveGameInfo->campaignSide );

		// campaign mission number
		saveGameInfo->missionNumber = TheCampaignManager->getCurrentMissionNumber();
		xfer->xferInt( &saveGameInfo->missionNumber );


	}
	else
	{

		// write empty campaign side
		saveGameInfo->campaignSide = AsciiString::TheEmptyString;
		xfer->xferAsciiString( &saveGameInfo->campaignSide );

		// invalid mission number
		saveGameInfo->missionNumber = CampaignManager::INVALID_MISSION_NUMBER;
		xfer->xferInt( &saveGameInfo->missionNumber );

	}

}
