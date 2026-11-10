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

// FILE: GameLogicDispatch.cpp ////////////////////////////////////////////////////////////////////
// Author: Mike Booth, Colin Day
// Description: Message logic to drive the game play
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/FpsLimitPolicy.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ThingFactory.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/StatsCollector.h"
#include "Common/Radar.h"
#include "Common/Xfer.h"	// GeneralsX @bugfix the reveal cheat's save block

#include "GameLogic/AIPathfind.h"
#include "GameLogic/ExperienceTracker.h"	// GeneralsX @feature LAN-safe cheats spawn at a veterancy level
#include "GameLogic/GameLogic.h"
#include "Common/GXAdminOrderMsg.h"	// GeneralsX @security an admin order's signature (gxCheatTarget)
#include "GameLogic/GXStress.h"	// GeneralsX @feature the spawn lattice's step, shared with the stress keep-out
#include "GameLogic/Locomotor.h"
#include "GameLogic/Object.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/ObjectIter.h"
#include "GameLogic/PartitionManager.h"	// GeneralsX @feature LAN-safe cheats need the shroud grid
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/OpenContain.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Weapon.h"

#include "GameClient/CommandXlat.h"
#include "GameClient/GXPerfReport.h"	// gxStatMatchEnd - the result, before reset() takes it
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/Eva.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GXKick.h"	// GeneralsX @feature the in-match kick's red line - display only, see onSelfDestruct
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Mouse.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Shell.h"
#include "GameClient/Module/BeaconClientUpdate.h"
#include "GameClient/LookAtXlat.h"

#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GameInfo.h"	// gxCheatNotice: an observer admin's own name

#include <algorithm>	// GeneralsX @feature std::sort, for the delete-objects cheat's stable victim order




static void gxCheatNotice( const GameMessage *msg, bool applied );	// GeneralsX @security see its definition

#define MAX_PATH_SUBJECTS 64
static Bool theBuildPlan = false;
static Object *thePlanSubject[ MAX_PATH_SUBJECTS ];
static int thePlanSubjectCount = 0;
//static WindowLayout *background = nullptr;

// ------------------------------------------------------------------------------------------------
/** Issue the movement command to the object */
// ------------------------------------------------------------------------------------------------
static void doMoveTo( Object *obj, const Coord3D *pos )
{
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	DEBUG_ASSERTCRASH(ai, ("Attempted doMoveTo() on an Object with no AI"));
	if (ai)
	{
		if (theBuildPlan)
		{
			int i;

			// if this object isn't in the buildPlan set, add it
			for( i=0; i<thePlanSubjectCount; i++ )
				if (thePlanSubject[i] == obj)
					break;

			if (i == thePlanSubjectCount)
				thePlanSubject[ thePlanSubjectCount++ ] = obj;

			ai->queueWaypoint( pos );
		}
		else
		{
			ai->clearWaypointQueue();
			obj->leaveGroup();
			obj->releaseWeaponLock(LOCKED_TEMPORARILY);	// release any temporary locks.
			ai->aiMoveToPosition( pos, CMD_FROM_PLAYER );
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void doSetRallyPoint( Object *obj, const Coord3D& pos )
{
	Bool isLocalPlayer = obj->isLocallyControlled();

	//
	// we must be able to find a path from the object to the point they have chosen, cause setting
	// a rally point at a invalid location would suck.  To be super nice, we have to make sure
	// that every type of object that can be created from the thing setting the rally point
	// can actually find a path from the thing to the point
	//

	// to see the never-finished code to check all locomotor sets, see past revs of GUICommandTranslator.cpp -MDC

	//
	// for now, just use the basic human locomotor ... and enable the above code when Steven
	// tells me how to get the locomotor sets based on a thing template (CBD)
	//
	NameKeyType key = NAMEKEY( "BasicHumanLocomotor" );
	LocomotorSet locomotorSet;
	locomotorSet.addLocomotor( TheLocomotorStore->findLocomotorTemplate( key ) );
	if( TheAI->pathfinder()->clientSafeQuickDoesPathExist( locomotorSet, obj->getPosition(), &pos ) == FALSE )
	{

		// user feedback
		if( isLocalPlayer )
		{

			// display error message to user
			TheInGameUI->message( TheGameText->fetchGameOnly( "GUI:RallyPointNoPath" ) );

			// play the no can do sound
			static AudioEventRTS rallyNotSet("UnableToSetRallyPoint");
			rallyNotSet.setPosition(&pos);
			rallyNotSet.setPlayerIndex(obj->getControllingPlayer()->getPlayerIndex());
			TheAudio->addAudioEvent(&rallyNotSet);

		}

		return;

	}

	// feedback to the player
	if( isLocalPlayer )
	{

		// print a message to the user
		UnicodeString info;
		info.format( TheGameText->fetchGameOnly( "GUI:RallyPointSet" ),
								 obj->getTemplate()->getDisplayName().str() );
		// GeneralsX: The display name can be map text (a map.ini DisplayName); message() would
		// run the finished line through printf a second time.
		TheInGameUI->messageNoFormat( info );

		// play a sound for setting the rally point
		static AudioEventRTS rallyPointSet("RallyPointSet");
		rallyPointSet.setPosition(&pos);
		rallyPointSet.setPlayerIndex(obj->getControllingPlayer()->getPlayerIndex());
		TheAudio->addAudioEvent(&rallyPointSet);

		// mark the UI as dirty so that we re-evaluate the selection and show the rally point
		Drawable *draw = obj->getDrawable();
		if( draw && draw->isSelected() )
			TheControlBar->markUIDirty();

	}

	// if this object has a ProductionExitUpdate interface, we are setting a rally point
	ExitInterface *exitInterface = obj->getObjectExitInterface();
	if( exitInterface )
	{
		// set the rally point
		exitInterface->setRallyPoint( &pos );

	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Player *getMessagePlayer(GameMessage *msg)
{
	return ThePlayerList->getNthPlayer( msg->getPlayerIndex() );
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature LAN-safe cheats - per-player bookkeeping for the reveal-map toggle.
//
// PartitionCell::addLooker is a COUNTER, not a flag (PartitionManager.cpp:1274 does
// min(m_currentShroud - 1, -1)), while undoRevealMapForPlayerPermanently issues exactly ONE
// removeLooker per cell. So reveal-reveal-undo leaves -2 -> -1, i.e. the map stays permanently lit,
// and an undo with no preceding reveal trips the "permanent shroud blob" DEBUG_ASSERTCRASH in
// removeLooker (PartitionManager.cpp:1313). s_gxRevealed makes the toggle idempotent.
//
// s_gxRadarForced remembers whether WE were the ones who forced the minimap on, so that un-revealing
// cannot stomp a force set by VictoryConditions (local defeat) or by a map script's
// doRadarForceEnable. Both arrays are written ONLY from the synchronised handlers below, so they are
// identical on every peer; neither is in any crc().
//
// GeneralsX: BOTH ARE SAVED, because what they describe is. A save keeps
// what a reveal did to the world - PartitionCell::xfer the permanent looker on every cell, Radar::xfer the forced
// minimap - so a save that dropped these loaded a revealed map whose toggle said "shrouded": Re-shroud was skipped,
// and Reveal + Re-shroud stacked a second permanent looker that nothing ever removes. They travel in a block of
// their own, CHUNK_GXCheatReveal (GameState::init, both games), after every stock block: retail and older
// GeneralsX builds skip a block they do not know by its size, so the save stays loadable there; a save without it
// loads with both arrays cleared by GameLogic::reset() on the way in.
// Trade-off: a save made by a build before this one with a reveal active still loads as "not revealed" - nothing in
// it says who added the permanent lookers (a map script's doRevealMapEntirePermanently adds the same one).
// ------------------------------------------------------------------------------------------------
static Bool s_gxRevealed[ MAX_PLAYER_COUNT ] = { FALSE };
static Bool s_gxRadarForced[ MAX_PLAYER_COUNT ] = { FALSE };

void gxCheatResetRevealState( void )
{
	memset( s_gxRevealed, 0, sizeof( s_gxRevealed ) );
	memset( s_gxRadarForced, 0, sizeof( s_gxRadarForced ) );
}

class GXCheatRevealState : public Snapshot
{
protected:
	virtual void crc( Xfer * ) override {}
	/** Version Info:
		* 1: Initial version - MAX_PLAYER_COUNT Bools of s_gxRevealed, then as many of s_gxRadarForced
		* 2: GeneralsX 06/11/2026 - MAX_PLAYER_COUNT 16 -> 40 (a v1 save is refused before this block, GameState v3) */
	virtual void xfer( Xfer *xfer ) override
	{
		XferVersion currentVersion = 2;
		XferVersion version = currentVersion;
		xfer->xferVersion( &version, currentVersion );
		static_assert( MAX_PLAYER_COUNT == 40, "the block's layout is MAX_PLAYER_COUNT entries each - bump its version" );
		for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
			xfer->xferBool( &s_gxRevealed[ i ] );
		for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
			xfer->xferBool( &s_gxRadarForced[ i ] );
	}
	virtual void loadPostProcess() override {}
};
static GXCheatRevealState s_gxCheatRevealState;
Snapshot *TheGXCheatRevealState = &s_gxCheatRevealState;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Object * getSingleObjectFromSelection(const AIGroup *currentlySelectedGroup)
{
	if( currentlySelectedGroup && !currentlySelectedGroup->isEmpty() )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		DEBUG_ASSERTCRASH(selectedObjects.size() == 1, ("Trying to get single object from multiple selection!"));
		VecObjectID::const_iterator it = selectedObjects.begin();
		return TheGameLogic->findObjectByID(*it);
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::closeWindows()
{
	HideDiplomacy();
	ResetDiplomacy();
	HideInGameChat();
	ResetInGameChat();
	TheControlBar->hidePurchaseScience();
	TheControlBar->hideSpecialPowerShortcut();
	HideQuitMenu();

	// hide the options menu
	NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
	GameWindow *button = TheWindowManager->winGetWindowFromId( nullptr, buttonID );
	GameWindow *window = TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:OptionsMenuParent") );
	if(window)
		TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																			(WindowMsgData)button, buttonID );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::clearGameData( Bool showScoreScreen )
{
	if( !isInGame() )
	{
		DEBUG_CRASH(("We tried to clear the game data when we weren't in a game"));
		return;
	}

	setClearingGameData( TRUE );

	/*
	 * GeneralsX: THE END-OF-MATCH RESULT, while the match still exists.
	 *
	 * HERE, and not two lines higher, and not after the score screen. Three constraints pin it:
	 *
	 *  - BELOW THE isInGame() GUARD above. A hook at the literal top of this function also fires on
	 *    the no-op path, where there is no game to report.
	 *  - BEFORE TheGameEngine->reset() below, which empties ThePlayerList and every ScoreKeeper.
	 *    Everything this reads is gone after it.
	 *  - NOT hung off the score screen, which is gated on !TheGlobalData->m_headless further down -
	 *    so a headless peer, which is exactly the standing lobby host, would never report.
	 *
	 * THIS FUNCTION IS NOT AN END-OF-MATCH HOOK and must not be mistaken for one. It is "unload
	 * whatever game is loaded", and the animated main-menu backdrop is a real GAME_SHELL game torn
	 * down through it at match START - MainMenu.cpp's and MapSelectMenu.cpp's doGameStart() both
	 * call it immediately before posting MSG_NEW_GAME. There are 35 references to it tree-wide.
	 * gxStatMatchEnd() carries its own guards for all of that: it sends only when a relay match
	 * actually produced samples, only when the victory conditions recorded an end frame, and only
	 * once per match.
	 */
	gxStatMatchEnd();

	// GeneralsX: Give the frame limit back when a game ends.
	//
	// Starting a network game used to turn it OFF - LANAPICallbacks.cpp, StagingRoomGameInfo.cpp
	// and GameSpyGameInfo.cpp all set m_useFpsLimit = false as they posted MSG_NEW_GAME - on the
	// theory that during a network match the peers agree the logic rate between themselves and a
	// local cap must not fight it. Nothing ever turned it back on. So the moment a match ended the
	// pacer stopped limiting anything, for the rest of the process: the victory banner ran at
	// roughly 10x speed and the shell menus stayed that way afterwards, on every peer.
	//
	// GeneralsX: Those three assignments are GONE - they were also making
	// the render loop spin unbounded for the whole match (~540 iterations/second against a 30 Hz
	// logic rate). This restore is therefore no longer load-bearing for the network case, and is
	// kept because one path still legitimately turns the limit off: the g_UT_startTiming
	// unit-timing harness in GameLogic.cpp.
	//
	// GeneralsX: Back ON, not back to Options.ini "FPSLimit": that key is
	// no longer a setting - the max-FPS cap is, and 0 in it already means unlimited, which is also
	// all -noFPSLimit sets now (Common/FpsLimitPolicy.h).
	//
	// This is pacing only and touches no simulation state, so it cannot affect a CRC. Verified
	// separately: the uncapped post-game frames stayed byte-identical across macOS and Windows for
	// 3,600 frames past the host's exit, so the bug was never a determinism problem - only a
	// deeply strange thing to look at.
	{
		TheWritableGlobalData->m_useFpsLimit = TRUE;
		TheFramePacer->enableLogicTimeScale( FALSE );
	}

//	m_background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
//	DEBUG_ASSERTCRASH(m_background,("We Couldn't Load Menus/BlankWindow.wnd"));
//	m_background->hide(FALSE);
//	m_background->bringForward();
	// reset the game engine to accept data for a new game
	if(TheStatsCollector)
		TheStatsCollector->writeFileEnd();

	TheScriptActions->closeWindows(FALSE); // Close victory or defeat windows.

	Bool shellGame = FALSE;
	if ((!isInShellGame() || !isInGame()) && showScoreScreen && !TheGlobalData->m_headless)
	{
		shellGame = TRUE;
#ifdef ALLOW_DEBUG_UTILS
		// GeneralsX: Emit runtime diagnostics when ScoreScreen is pushed after game end.
		fprintf(stderr, "[SKIRMISH_DIAG] clearGameData showScoreScreen=%d isInShellGame=%d isInGame=%d headless=%d -> pushing ScoreScreen\n",
			showScoreScreen ? 1 : 0,
			isInShellGame() ? 1 : 0,
			isInGame() ? 1 : 0,
			TheGlobalData->m_headless ? 1 : 0);
		fflush(stderr);
#endif
		TheTransitionHandler->setGroup("FadeWholeScreen");
		TheShell->push("Menus/ScoreScreen.wnd");
		TheShell->showShell(FALSE); // by passing in false, we don't want to run the Init on the shell screen we just pushed on
		TheTransitionHandler->reverse("FadeWholeScreen");

		void FixupScoreScreenMovieWindow();
		FixupScoreScreenMovieWindow();

		destroyQuitMenu();
	}

	TheGameEngine->reset();
	setGameMode(GAME_NONE);
//	m_background->bringForward();
//	if(shellGame)


	if (TheGlobalData->m_initialFile.isEmpty() == FALSE || m_quitToDesktopAfterMatch)
	{
		TheGameEngine->setQuitting(TRUE);
		m_quitToDesktopAfterMatch = FALSE;
	}

	HideControlBar();
	closeWindows();

	TheMouse->setVisibility(TRUE);

	if(m_background)
	{
		m_background->destroyWindows();
		deleteInstance(m_background);
		m_background = nullptr;
	}

	setClearingGameData( FALSE );

}

// ------------------------------------------------------------------------------------------------
/** Prepare for a new game */
// ------------------------------------------------------------------------------------------------
void GameLogic::prepareNewGame( GameMode gameMode, GameDifficulty diff, Int rankPoints )
{
	//Kris: Commented this out, but leaving it around incase it bites us later. I cleaned up the
	//      nomenclature. Look for setLoadingMap() and setLoadingSave()
	//setGameLoading(TRUE);

	TheScriptEngine->setGlobalDifficulty(diff);

	if(!m_background)
	{
		m_background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
		DEBUG_ASSERTCRASH(m_background,("We Couldn't Load Menus/BlankWindow.wnd"));
		m_background->hide(FALSE);
		m_background->bringForward();
	}
	m_background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	setGameMode( gameMode );
	if (!TheGlobalData->m_pendingFile.isEmpty())
	{
		TheWritableGlobalData->m_mapName = TheGlobalData->m_pendingFile;
		TheWritableGlobalData->m_pendingFile.clear();
	}

	m_rankPointsToAddAtGameStart = rankPoints;
	DEBUG_LOG(("GameLogic::prepareNewGame() - m_rankPointsToAddAtGameStart = %d", m_rankPointsToAddAtGameStart));

	// If we're about to start a game, hide the shell.
	if(!isInShellGame())
		TheShell->hideShell();

	m_startNewGame = FALSE;

}

//-------------------------------------------------------------------------------------------------
/** This message handles dispatches object command messages to the
  * appropriate objects.
	* @todo Rename this to "CommandProcessor", or similar. */
//-------------------------------------------------------------------------------------------------
void GameLogic::logicMessageDispatcher( GameMessage *msg, void *userData )
{
#ifdef RTS_DEBUG
	DEBUG_ASSERTCRASH(msg != nullptr && msg != (GameMessage*)0xdeadbeef, ("bad msg"));
#endif

	Player *msgPlayer = getMessagePlayer(msg);
	if (msgPlayer == nullptr)
	{
		DEBUG_CRASH(("logicMessageDispatcher: Processing message from unknown player (player index '%d')", msg->getPlayerIndex()));
		return;
	}

	AIGroupPtr currentlySelectedGroup = nullptr;

	if (isInGame())
	{
		if (msg->getType() >= GameMessage::MSG_BEGIN_NETWORK_MESSAGES && msg->getType() <= GameMessage::MSG_END_NETWORK_MESSAGES)
		{
			if (msg->getType() != GameMessage::MSG_LOGIC_CRC && msg->getType() != GameMessage::MSG_SET_REPLAY_CAMERA)
			{
				currentlySelectedGroup = TheAI->createGroup(); // can't do this outside a game - it'll cause sync errors galore.
				CRCGEN_LOG(( "Creating AIGroup %d in GameLogic::logicMessageDispatcher()", currentlySelectedGroup?currentlySelectedGroup->getID():0 ));
#if RETAIL_COMPATIBLE_AIGROUP
				msgPlayer->getCurrentSelectionAsAIGroup(currentlySelectedGroup);
#else
				msgPlayer->getCurrentSelectionAsAIGroup(currentlySelectedGroup.Peek());
#endif

				// We can't issue commands to groups that contain units that don't belong to the issuing player, so pretend like
				// there's nothing selected. Also, if currentlySelectedGroup is empty, go ahead and delete it, so that we can skip
				// any processing on it.
				if (currentlySelectedGroup->isEmpty())
				{
#if RETAIL_COMPATIBLE_AIGROUP
					TheAI->destroyGroup(currentlySelectedGroup);
#endif
					currentlySelectedGroup = nullptr;
				}

				// If there are any units that the player doesn't own, then remove them from the "currentlySelectedGroup"
				if (currentlySelectedGroup)
					if (currentlySelectedGroup->removeAnyObjectsNotOwnedByPlayer(msgPlayer))
						currentlySelectedGroup = nullptr;

				if(TheStatsCollector)
					TheStatsCollector->collectMsgStats(msg);
			}
		}
	}

#ifdef DEBUG_LOGGING
	AsciiString commandName;

	commandName = msg->getCommandAsString();
	if (msg->getType() < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || msg->getType() > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (NON-LOGIC-MESSAGE!!!)");
	}
	else if (msg->getType() == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName = " (CRC message!)";
	}
#if 0
	if (commandName.isNotEmpty() /*&& msg->getType() != GameMessage::MSG_FRAME_TICK*/)
	{
		DEBUG_LOG(("Frame %d: GameLogic::logicMessageDispatcher() saw a %s from player %d (%ls)", getFrame(), commandName.str(),
			msgPlayer->getPlayerIndex(), msgPlayer->getPlayerDisplayName().str()));
	}
#endif
#endif // DEBUG_LOGGING

	// process the message
	GameMessage::Type msgType = msg->getType();
	switch( msgType )
	{
		case GameMessage::MSG_NEW_GAME:
		{
			gxCheatResetRevealState();	// GeneralsX @feature Radar::reset() also clears m_radarForceOn
			onNewGame(msg);
			break;
		}
		case GameMessage::MSG_CLEAR_GAME_DATA:
		{
			gxCheatResetRevealState();	// GeneralsX @feature
			onClearGameData(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_META_BEGIN_PATH_BUILD:
		{
			onBeginPathBuild(msg);
			break;
		}
		case GameMessage::MSG_META_END_PATH_BUILD:
		{
			onEndPathBuild(msg);
			break;
		}
		case GameMessage::MSG_SET_RALLY_POINT:
		{
			onSetRallyPoint(msg);
			break;
		}
		case GameMessage::MSG_DO_WEAPON:
		{
			onDoWeapon(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_COMBATDROP_AT_OBJECT:
		{
			onCombatdropAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_COMBATDROP_AT_LOCATION:
		{
			onCombatdropAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_WEAPON_AT_OBJECT:
		{
			onDoWeaponAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SWITCH_WEAPONS:
		{
			onDoSwitchWeapons(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SET_MINE_CLEARING_DETAIL:
		{
			onSetMineClearingDetail(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_ENABLE_RETALIATION_MODE:
		{
			onEnableRetaliationMode(msg);
			break;
		}
		case GameMessage::MSG_DO_WEAPON_AT_LOCATION:
		{
			onDoWeaponAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER:
		{
			onDoSpecialPower(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION:
		{
			onDoSpecialPowerAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT:
		{
			onDoSpecialPowerAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_ATTACKMOVETO:
		{
			onDoAttackmoveto(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCEMOVETO:
		{
			onDoForcemoveto(msg, currentlySelectedGroup);
			break;
		}
		// MSG_DO_SALVAGE is intentionally set up to mimic the moveto.
		case GameMessage::MSG_DO_SALVAGE:
		case GameMessage::MSG_DO_MOVETO:
		{
			onDoMoveto(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_ADD_WAYPOINT:
		{
			onAddWaypoint(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_GUARD_POSITION:
		{
			onDoGuardPosition(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_GUARD_OBJECT:
		{
			onDoGuardObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_STOP:
		{
			onDoStop(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SCATTER:
		{
			onDoScatter(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CREATE_FORMATION:
		{
			onCreateFormation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CLEAR_INGAME_POPUP_MESSAGE:
		{
			onClearIngamePopupMessage(msg);
			break;
		}
		case GameMessage::MSG_DO_CHEER:
		{
			onDoCheer(msg, currentlySelectedGroup);
			break;
		}

#if defined(RTS_DEBUG) || defined (_ALLOW_DEBUG_CHEATS_IN_RELEASE)

		case GameMessage::MSG_DEBUG_KILL_SELECTION:
		{
			onDebugKillSelection(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DEBUG_HURT_OBJECT:
		{
			onDebugHurtObject(msg);
			break;
		}
		case GameMessage::MSG_DEBUG_KILL_OBJECT:
		{
			onDebugKillObject(msg);
			break;
		}
#endif

		// GeneralsX @feature LAN-safe cheats. DELIBERATELY OUTSIDE the RTS_DEBUG /
		// _ALLOW_DEBUG_CHEATS_IN_RELEASE guards above. The enum values are unconditional; if the
		// handlers were conditional, a peer built without cheats would receive the message, match no
		// case (this switch has no default:), silently no-op, and then mismatch at the next
		// NET_CRC_INTERVAL. That is the single most dangerous mistake available here.
		// GeneralsX: Each handler says whether it applied; gxCheatNotice turns an applied
		// ADMIN cheat (a match with more than one human) into a line every peer draws on the same frame.
		case GameMessage::MSG_GX_CHEAT_GIVE_MONEY:
		{
			gxCheatNotice( msg, onGxCheatGiveMoney(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_SET_MONEY:
		{
			gxCheatNotice( msg, onGxCheatSetMoney(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_REVEAL_MAP:
		{
			gxCheatNotice( msg, onGxCheatRevealMap(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_KILL_PLAYER:
		{
			gxCheatNotice( msg, onGxCheatKillPlayer(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_KILL_OBJECTS:
		{
			gxCheatNotice( msg, onGxCheatKillObjects(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_SPAWN_UNIT:
		{
			gxCheatNotice( msg, onGxCheatSpawnUnit(msg) );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS:
		{
			gxCheatNotice( msg, onGxCheatDeleteObjects(msg) );
			break;
		}

		case GameMessage::MSG_ENTER:
		{
			onEnter(msg, currentlySelectedGroup);
			break;
		}
#if RTS_ZEROHOUR
		case GameMessage::MSG_GX_QUEUE_ENTER:
		{
			onGxQueueEnter(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_GX_VOTEKICK_START:
		{
			onGxVoteKickStart(msg);
			break;
		}
		case GameMessage::MSG_GX_VOTEKICK_CAST:
		{
			onGxVoteKickCast(msg);
			break;
		}
#endif
		case GameMessage::MSG_EXIT:
		{
			onExit(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_EVACUATE:
		{
			onEvacuate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_EXECUTE_RAILED_TRANSPORT:
		{
			onExecuteRailedTransport(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_INTERNET_HACK:
		{
			onInternetHack(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_GET_REPAIRED:
		{
			onGetRepaired(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOCK:
		{
			onDock(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_GET_HEALED:
		{
			onGetHealed(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_REPAIR:
		{
			onDoRepair(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_RESUME_CONSTRUCTION:
		{
			onResumeConstruction(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION:
		{
			onDoSpecialPowerOverrideDestination(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_ATTACK_OBJECT:
		{
			onDoAttackObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT:
		{
			onDoForceAttackObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCE_ATTACK_GROUND:
		{
			onDoForceAttackGround(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_QUEUE_UPGRADE:
		{
			onQueueUpgrade(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CANCEL_UPGRADE:
		{
			onCancelUpgrade(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_QUEUE_UNIT_CREATE:
		{
			onQueueUnitCreate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CANCEL_UNIT_CREATE:
		{
			onCancelUnitCreate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOZER_CONSTRUCT:
		case GameMessage::MSG_DOZER_CONSTRUCT_LINE:
		{
			onDozerConstruct(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOZER_CANCEL_CONSTRUCT:
		{
			onDozerCancelConstruct(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SELL:
		{
			onSell(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_TOGGLE_OVERCHARGE:
		{
			onToggleOvercharge(msg, currentlySelectedGroup);
			break;
		}

#ifdef ALLOW_SURRENDER
		case GameMessage::MSG_DO_SURRENDER:
		{
			onDoSurrender(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_PICK_UP_PRISONER:
		{
			onPickUpPrisoner(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_RETURN_TO_PRISON:
		{
			onReturnToPrison(msg, currentlySelectedGroup);
			break;
		}
#endif

		// No sound does exactly the same logical processing as the usual message. Just double them up.
		case GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND:
		case GameMessage::MSG_CREATE_SELECTED_GROUP:
		{
			onCreateSelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP:
		{
			onRemoveFromSelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_DESTROY_SELECTED_GROUP:
		{
			onDestroySelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_SELECTED_GROUP_COMMAND:
		{
			break;
		}
		case GameMessage::MSG_PLACE_BEACON:
		{
			onPlaceBeacon(msg);
			break;
		}
		case GameMessage::MSG_REMOVE_BEACON:
		{
			onRemoveBeacon(msg);
			break;
		}
		case GameMessage::MSG_SET_BEACON_TEXT:
		{
			onSetBeaconText(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SELF_DESTRUCT:
		{
			onSelfDestruct(msg);
			break;
		}
		case GameMessage::MSG_SET_REPLAY_CAMERA:
		{
			onSetReplayCamera(msg);
			break;
		}
		case GameMessage::MSG_CREATE_TEAM0:
		case GameMessage::MSG_CREATE_TEAM1:
		case GameMessage::MSG_CREATE_TEAM2:
		case GameMessage::MSG_CREATE_TEAM3:
		case GameMessage::MSG_CREATE_TEAM4:
		case GameMessage::MSG_CREATE_TEAM5:
		case GameMessage::MSG_CREATE_TEAM6:
		case GameMessage::MSG_CREATE_TEAM7:
		case GameMessage::MSG_CREATE_TEAM8:
		case GameMessage::MSG_CREATE_TEAM9:
		{
			onCreateTeam(msg);
			break;
		}
		case GameMessage::MSG_SELECT_TEAM0:
		case GameMessage::MSG_SELECT_TEAM1:
		case GameMessage::MSG_SELECT_TEAM2:
		case GameMessage::MSG_SELECT_TEAM3:
		case GameMessage::MSG_SELECT_TEAM4:
		case GameMessage::MSG_SELECT_TEAM5:
		case GameMessage::MSG_SELECT_TEAM6:
		case GameMessage::MSG_SELECT_TEAM7:
		case GameMessage::MSG_SELECT_TEAM8:
		case GameMessage::MSG_SELECT_TEAM9:
		{
			onSelectTeam(msg);
			break;
		}
		case GameMessage::MSG_ADD_TEAM0:
		case GameMessage::MSG_ADD_TEAM1:
		case GameMessage::MSG_ADD_TEAM2:
		case GameMessage::MSG_ADD_TEAM3:
		case GameMessage::MSG_ADD_TEAM4:
		case GameMessage::MSG_ADD_TEAM5:
		case GameMessage::MSG_ADD_TEAM6:
		case GameMessage::MSG_ADD_TEAM7:
		case GameMessage::MSG_ADD_TEAM8:
		case GameMessage::MSG_ADD_TEAM9:
		{
			onAddTeam(msg);
			break;
		}
		case GameMessage::MSG_LOGIC_CRC:
		{
			onLogicCrc(msg);
			break;
		}
		case GameMessage::MSG_PURCHASE_SCIENCE:
		{
			onPurchaseScience(msg);
			break;
		}
	}

#if RETAIL_COMPATIBLE_AIGROUP
	// TheSuperHackers @bugfix xezon 28/06/2025 This hack avoids crashing when players are selected during Replay playback.
	// It can read data from an already deleted AIGroup and return this function when its member size is 0, signifying that
	// it is indeed deleted.
	if (currentlySelectedGroup && currentlySelectedGroup->getCount() == 0)
		return;
#endif

	/**/ /// @todo: multiplayer semantics
	if (currentlySelectedGroup && TheRecorder->isPlaybackMode() && TheGlobalData->m_useCameraInReplay && TheControlBar->getObserverLookAtPlayer() == msgPlayer /*&& !TheRecorder->isMultiplayer()*/)
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		TheInGameUI->deselectAllDrawables();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			const Object *obj = findObjectByID(*it);
			if (obj)
			{
				Drawable *draw = obj->getDrawable();
				if (draw)
					TheInGameUI->selectDrawable(draw);
			}
		}
	}
	/**/

	if( currentlySelectedGroup != nullptr )
	{
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(currentlySelectedGroup);
#else
		currentlySelectedGroup->removeAll();
#endif
	}

}

bool GameLogic::onNewGame(MAYBE_UNUSED GameMessage *msg)
{
#if !RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @fix stephanmeesters 11/03/2026
	// Make sure we're ready to start a new game. This prevents an issue where an infinite disconnect screen
	// can be force-triggered in an online match by using cheats.
	if ( isInGame() || isClearingGameData() || isLoadingMap() )
	{
		DEBUG_CRASH( ("Called MSG_NEW_GAME while game is not ready (inGame=%d, clearingData=%d, loadingMap=%d)",
			isInGame(), isClearingGameData(), isLoadingMap()) );

		return false;
	}
#endif

	//DEBUG_ASSERTCRASH(msg->getArgumentCount() == 1 || msg->getArgumentCount() == 2, ("%d arguments to MSG_NEW_GAME", msg->getArgumentCount()));
	GameMode gameMode = (GameMode)msg->getArgument( 0 )->integer;
	Int rankPoints = 0;
	GameDifficulty diff = DIFFICULTY_NORMAL;
	if ( msg->getArgumentCount() >= 2 )
		diff = (GameDifficulty)msg->getArgument( 1 )->integer;
	if ( msg->getArgumentCount() >= 3 )
		rankPoints = msg->getArgument( 2 )->integer;

	if ( msg->getArgumentCount() >= 4 )
	{
		// GeneralsX: The fourth argument is the GAME SPEED - the skirmish
		// slider, a replay's recorded speed, a restart's - and no longer the render cap. It used to be
		// both: retail's logic followed the render rate, so the slider capped rendering, and here that
		// meant a launcher cap of 144 became 1000 at the slider's "--" end, or 45 halfway down. The cap
		// is the player's one max-FPS setting and nothing here writes it (Common/FpsLimitPolicy.h).
		const Int gameSpeedFps = msg->getArgument( 3 )->integer;
		DEBUG_LOG(("Game speed %d FPS", gameSpeedFps));

		// Network games are deliberately excluded. FramePacer::getActualLogicTimeScaleFps defers
		// to TheNetwork->getFrameRate() while a network game exists, and CommandXlat's
		// changeLogicTimeScale refuses outright for the same reason; the peers agree the logic
		// rate between themselves and the client must not second-guess it.
		if (TheNetwork == nullptr)
		{
			// A speed under the tick rate slows the match through the logic time scale. And the scale
			// must be on whenever the render rate can exceed the tick rate, or canUpdateRegularGameLogic()
			// ticks once per rendered frame and the match runs at render speed. Otherwise - normal speed,
			// cap at or under the tick rate - it stays off: coupled, the logic still steps once per
			// iteration at the tick rate (SET_VISUAL_SPEED_MULTIPLIER speeds it up either way - it steps the
			// logic with the loop, FramePacer::isScriptFastForward). Off explicitly, so a start does not
			// inherit the last game's scale.
			const Int scaleFps = FpsLimitPolicy::soloStartLogicTimeScaleFps(gameSpeedFps, LOGICFRAMES_PER_SECOND);
			if (scaleFps < (Int)LOGICFRAMES_PER_SECOND
				|| FpsLimitPolicy::needsLogicTimeScale(TheFramePacer->getFramesPerSecondLimit(), LOGICFRAMES_PER_SECOND))
			{
				TheFramePacer->setLogicTimeScaleFps(scaleFps);
				TheFramePacer->enableLogicTimeScale(TRUE);
			}
			else
			{
				TheFramePacer->enableLogicTimeScale(FALSE);
			}
		}
	}

	// prepare for new game
	prepareNewGame( gameMode, diff, rankPoints );

	// start new game
	startNewGame( FALSE );

	return true;
}

bool GameLogic::onClearGameData(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
#if defined(RTS_DEBUG)
	if (TheDisplay && TheGlobalData->m_dumpAssetUsage)
		TheDisplay->dumpAssetUsage(TheGlobalData->m_mapName.str());
#endif

	if (currentlySelectedGroup)
	{
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(currentlySelectedGroup);
#else
		currentlySelectedGroup->removeAll();
#endif
	}
	currentlySelectedGroup = nullptr;
	clearGameData();

	return true;
}

bool GameLogic::onBeginPathBuild(MAYBE_UNUSED GameMessage *msg)
{
	DEBUG_LOG(("META: begin path build"));
	DEBUG_ASSERTCRASH(!theBuildPlan, ("mismatched theBuildPlan"));

	if (theBuildPlan == false)
	{
		theBuildPlan = true;
		thePlanSubjectCount = 0;
	}

	return true;
}

bool GameLogic::onEndPathBuild(MAYBE_UNUSED GameMessage *msg)
{
	DEBUG_LOG(("META: end path build"));
	DEBUG_ASSERTCRASH(theBuildPlan, ("mismatched theBuildPlan"));

	// tell everyone who participated in the plan to move
	for( int i=0; i<thePlanSubjectCount; i++ )
	{
		AIUpdateInterface *ai = thePlanSubject[i]->getAIUpdateInterface();
		if (ai)
			ai->executeWaypointQueue();
	}

	theBuildPlan = false;
	thePlanSubjectCount = 0;

	return true;
}

bool GameLogic::onSetRallyPoint(MAYBE_UNUSED GameMessage *msg)
{
	Object *obj = findObjectByID( msg->getArgument( 0 )->objectID );
	Coord3D dest = msg->getArgument( 1 )->location;

	if (!obj)
		return false;

#if !RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @fix stephanmeesters 11/03/2026 Validate the owner of the source object
	Player *msgPlayer = getMessagePlayer(msg);
	if ( obj->getControllingPlayer() != msgPlayer )
	{
		DEBUG_CRASH( ("MSG_SET_RALLY_POINT: Player '%ls' attempted to set the rally point of object '%s' owned by player '%ls'.",
			msgPlayer->getPlayerDisplayName().str(),
			obj->getTemplate()->getName().str(),
			obj->getControllingPlayer()->getPlayerDisplayName().str()) );
		return false;
	}
#endif

	doSetRallyPoint( obj, dest );

	return true;
}

bool GameLogic::onDoWeapon(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Int maxShotsToFire = msg->getArgument( 1 )->integer;

	// lock it just till the weapon is empty or the attack is "done"
	if( currentlySelectedGroup && currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
	{
		currentlySelectedGroup->groupAttackPosition( nullptr, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onCombatdropAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *targetObject = findObjectByID( msg->getArgument( 0 )->objectID );

	// issue command for either single object or for selected group
	if( currentlySelectedGroup && targetObject )
		currentlySelectedGroup->groupCombatDrop( targetObject,
																							*targetObject->getPosition(),
																							CMD_FROM_PLAYER );

	/*
	if( sourceObject && targetObject )
	{
		AIUpdateInterface* sourceAI = sourceObject->getAIUpdateInterface();
		if (sourceAI)
		{
			sourceAI->aiCombatDrop( targetObject, *targetObject->getPosition(), CMD_FROM_PLAYER );
		}
	}
	*/

	return true;
}

bool GameLogic::onCombatdropAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D targetLoc = msg->getArgument( 0 )->location;

	if( currentlySelectedGroup )
		currentlySelectedGroup->groupCombatDrop( nullptr, targetLoc, CMD_FROM_PLAYER );

	/*
	if( sourceObject )
	{
		AIUpdateInterface* sourceAI = sourceObject->getAIUpdateInterface();
		if (sourceAI)
		{
			sourceAI->aiCombatDrop( nullptr, targetLoc, CMD_FROM_PLAYER );
		}
	}
	*/

	return true;
}

bool GameLogic::onDoWeaponAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// Lock the weapon choice to the right weapon, then give an attack command

	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Object *targetObject = findObjectByID( msg->getArgument( 1 )->objectID );
	Int maxShotsToFire = msg->getArgument( 2 )->integer;

	// sanity
	if( targetObject == nullptr )
		return false;

	// issue command for either single object or for selected group
	if( currentlySelectedGroup )
	{
			// lock it just till the weapon is empty or the attack is "done"
		if (currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
			currentlySelectedGroup->groupAttackObject( targetObject, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoSwitchWeapons(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	// lock until un-switched, or switched to something else.
	if( currentlySelectedGroup )
		currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_PERMANENTLY );

	return true;
}

bool GameLogic::onSetMineClearingDetail(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->setMineClearingDetail(true);
	}

	return true;
}

bool GameLogic::onEnableRetaliationMode(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_CRC
	(void)msgPlayer;
	//Logically turns on or off retaliation mode for a specified player.
	const Int playerIndex = msg->getArgument( 0 )->integer;
	const Bool enableRetaliation = msg->getArgument( 1 )->boolean;

	Player *player = ThePlayerList->getNthPlayer( playerIndex );
	if( player )
	{
		DEBUG_ASSERTCRASH(player == msgPlayer,
			("Retaliation mode of player '%ls' was illegally set by player '%ls'. Before: '%d', after: '%d'.",
				player->getPlayerDisplayName().str(), msgPlayer->getPlayerDisplayName().str(),
				player->isLogicalRetaliationModeEnabled(), enableRetaliation) );

		player->setLogicalRetaliationModeEnabled( enableRetaliation );
	}
#else
	// TheSuperHackers @fix stephanmeesters 08/03/2026 Ensure that players can only set their own retaliation mode.
	const Bool enableRetaliation = msg->getArgument( 0 )->boolean;
	msgPlayer->setLogicalRetaliationModeEnabled( enableRetaliation );
#endif

	return true;
}

bool GameLogic::onDoWeaponAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Coord3D targetLoc = msg->getArgument( 1 )->location;
	Int maxShotsToFire = msg->getArgument( 2 )->integer;

	// issue command for either single object or for selected group
	if( currentlySelectedGroup )
	{
			// lock it just till the weapon is empty or the attack is "done"
		if (currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
			currentlySelectedGroup->groupAttackPosition( &targetLoc, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoSpecialPower(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( 0 )->integer;

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( 1 )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument(2)->objectID;
	Object* source = findObjectByID(sourceID);
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPower( specialPowerID, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		//Use the selected group!
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPower( specialPowerID, options );
		}
	}

	return true;
}

bool GameLogic::onDoSpecialPowerAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const Bool hasAngle = msg->getArgumentCount() >= 6;
	Int argumentIndex = 0;

	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( argumentIndex++ )->integer;

	// Location argument 2 is destination
	Coord3D targetCoord = msg->getArgument( argumentIndex++ )->location;

	// Angle argument 3 is the orientation of the special power (if applicable)
	Real angle = hasAngle ? msg->getArgument( argumentIndex++ )->real : INVALID_ANGLE;

	// Object in way -- if applicable (some specials care, others don't)
	ObjectID objectID = msg->getArgument( argumentIndex++ )->objectID;
	Object *objectInWay = findObjectByID( objectID );

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( argumentIndex++ )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument( argumentIndex++ )->objectID;
	Object* source = findObjectByID( sourceID );
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_AT_LOCATION: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPowerAtLocation( specialPowerID, &targetCoord, angle, objectInWay, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		//Use the selected group!
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPowerAtLocation( specialPowerID, &targetCoord, angle, objectInWay, options );
		}
	}

	return true;
}

bool GameLogic::onDoSpecialPowerAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{

	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( 0 )->integer;

	// argument 2 is target object
	ObjectID targetID = msg->getArgument(1)->objectID;
	Object *target = findObjectByID( targetID );
	if( !target )
	{
		return false;
	}

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( 2 )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument(3)->objectID;
	Object* source = findObjectByID(sourceID);
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_AT_OBJECT: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPowerAtObject( specialPowerID, target, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPowerAtObject( specialPowerID, target, options );
		}
	}
	return true;
}

bool GameLogic::onDoAttackmoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupAttackMoveToPosition( &dest, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoForcemoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, false, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoMoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if( currentlySelectedGroup )
	{
		//DEBUG_LOG(("GameLogicDispatch - got a MSG_DO_MOVETO command"));
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, false, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onAddWaypoint(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;
#if defined(GX_TEST_INSTRUMENTS)
	// GeneralsX: WAYPOINT ENTER arms: a fallback click is proved SENT and executed on every peer.
	fprintf( stderr, "[GXENTERQ] addwaypoint f=%u from=%d\n", (unsigned)getFrame(), msg->getPlayerIndex() );
	fflush( stderr );
#endif

	if( currentlySelectedGroup )
	{
		//DEBUG_LOG(("GameLogicDispatch - got a MSG_DO_MOVETO command"));
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, true, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoGuardPosition(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D loc = msg->getArgument( 0 )->location;
	GuardMode gm = (GuardMode)msg->getArgument( 1 )->integer;
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupGuardPosition(&loc, gm, CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoGuardObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object* obj = findObjectByID( msg->getArgument( 0 )->objectID );
	if (!obj)
		return false;

	GuardMode gm = (GuardMode)msg->getArgument( 1 )->integer;
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupGuardObject(obj, gm, CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoStop(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupIdle(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoScatter(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupScatter(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onCreateFormation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupCreateFormation(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onClearIngamePopupMessage(MAYBE_UNUSED GameMessage *msg)
{
	if( TheInGameUI )
	{
		TheInGameUI->clearPopupMessageData();
	}

	return true;
}

bool GameLogic::onDoCheer(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units play cheer animation.
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->groupCheer( CMD_FROM_PLAYER );
	}

	return true;
}

#if defined(RTS_DEBUG) || defined (_ALLOW_DEBUG_CHEATS_IN_RELEASE)

bool GameLogic::onDebugKillSelection(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units die
	if( currentlySelectedGroup )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *obj = findObjectByID(*it);
			if (obj)
			{
				obj->kill();
			}
		}
	}

	return true;
}

bool GameLogic::onDebugHurtObject(MAYBE_UNUSED GameMessage *msg)
{
	Object* objToHurt = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
	if (objToHurt)
	{
		DamageInfo damageInfo;
		damageInfo.in.m_damageType = DAMAGE_UNRESISTABLE;
		damageInfo.in.m_deathType = DEATH_NORMAL;
		damageInfo.in.m_sourceID = INVALID_ID;
		damageInfo.in.m_amount = objToHurt->getBodyModule()->getMaxHealth() / 10.0f;
		objToHurt->attemptDamage( &damageInfo );
	}

	return true;
}

bool GameLogic::onDebugKillObject(MAYBE_UNUSED GameMessage *msg)
{
	Object* objToHurt = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
	if (objToHurt)
	{
		objToHurt->kill();
	}

	return true;
}

#endif

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature LAN-safe cheats.
//
// These run inside GameLogic::update on EVERY peer, on the same logic frame, in the same order
// (Network::RelayCommandsToCommandList -> ConnectionManager::getFrameCommandList, slot order 0..N).
// They must therefore be a pure function of (message arguments, logic state).
//
// FORBIDDEN in here: ThePlayerList->getLocalPlayer(), TheInGameUI, TheTacticalView, TheGameClient's
// drawable list, timeGetTime(). Those differ per machine and are the only way to reintroduce a
// desync. "Who cheated" is getMessagePlayer(msg); "who gets hit" is an explicit Int argument.
//
// The target is carried as a PLAYER INDEX, which is safe because player indices are lockstep-
// identical on every peer. Proof, not assumption: PlayerList::crc walks m_players[0 .. m_playerCount)
// in index order and xfers each one, PartitionCell::crc xfers
// m_shroudLevel[MAX_PLAYER_COUNT] indexed by player index, and GameLogic::getCRC feeds both into the
// frame CRC. If two peers disagreed about which Player sits at index N, every existing LAN game would
// mismatch at the first NET_CRC_INTERVAL. Indices come from PlayerList::newGame walking TheSidesList
// in file order, which is the same map on both machines.
//
// This file is compiled into BOTH g_gameengine and z_gameengine, so it may only use API that exists
// in both trees. Note in particular obj->kill() with NO arguments: Generals/.../Object.h:215 takes no
// parameters, GeneralsMD/.../Object.h:228 has two defaulted ones.
// ------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------
/** GameMessage::getArgument() out of range returns a shared zeroed static and only DEBUG_CRASHes,
	* which is a no-op in the shipping build. A short message would therefore be silently read as
	* "player 0, amount 0" instead of being rejected. Check the count explicitly. */
// ------------------------------------------------------------------------------------------------
static Bool gxCheatArgsOk( const GameMessage *msg, Int needed )
{
	// GeneralsX: The LOGICAL count: an admin order's signature trailer (Common/GXAdminOrder.h)
	// is not arguments, and must never be read as SPAWN_UNIT's optional count and veterancy.
	return msg != nullptr && gxAdminLogicalArgCount( msg ) >= needed;
}

// ------------------------------------------------------------------------------------------------
/** GeneralsX: May MSG_GX_CHEAT_* run in THIS match at all?
	*
	* Only in a sandbox: a match with at most one human in it - single player, skirmish, and the
	* one-human headless harness runs (-lanhost with -lanai). GX_STRESS no longer comes through here -
	* it is a lobby option the simulation applies itself (GameLogic::gxStressSpawn). With a
	* second human present a cheat is a weapon: KILL_PLAYER removes them outright, and every peer
	* could send it, in release, from the Debug menu. Measured before this gate: slot 1 cheated slot
	* 0 dead on both peers of a two-human match (scripts/test/net-kill-gates.sh forge-kill).
	*
	* Decided from SIMULATION state only, so every peer - and a replay of the match - answers the
	* same: player types are fixed when ThePlayerList is built and are CRC'd with it. Observers are
	* humans and count, because an observer holding the Debug menu could otherwise kill the one
	* player. The engine's own "ReplayObserver", which every game adds, is not a person and does not.
	*
	* NOT from TheNetwork, the game mode or TheGameInfo: a replay has no network and is GAME_REPLAY
	* whatever the recorded match was, and the slot list is lobby state. Any of those would let a
	* replay run a cheat its match refused, or the reverse. */
// ------------------------------------------------------------------------------------------------
Bool GameLogic::gxCheatsAllowed()
{
	if( ThePlayerList == nullptr )
		return FALSE;

	const NameKeyType replayObserver = NAMEKEY( "ReplayObserver" );
	Int humans = 0;
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		const Player *p = ThePlayerList->getNthPlayer( i );
		if( p != nullptr && p->getPlayerType() == PLAYER_HUMAN && p->getPlayerNameKey() != replayObserver )
			++humans;
	}
	// GeneralsX: (observer seats) ...and every OBSERVER SLOT that shares the ReplayObserver
	// (PlayerList::assignSlotIndices) is a person too. In that layout an observer has no Player of its own, so the loop
	// above cannot see one, and one player watched by two observers read as a sandbox any of the three could cheat in.
	// The slot map is fixed at the start from the same slot list on every peer and in a replay (its header's).
	const Player *shared = ThePlayerList->findPlayerWithNameKey( replayObserver );
	for( Int s = 0; shared != nullptr && s < MAX_SLOTS; ++s )
	{
		if( ThePlayerList->getPlayerFromSlotIndex( s ) == shared )
			++humans;
	}
	return humans <= 1;
}

// ------------------------------------------------------------------------------------------------
/** The slot a player occupies, from the simulation's own "player<slot>" naming - the name
	* NetGameCommandMsg::constructGameMessage gives the sender of every network order (NetCommandMsg.cpp),
	* and the one GXKick.cpp and GX_TEST_FORGE read the same way. KEYNAME only looks the name up; nothing
	* here creates a name key. -1 for a player that is no slot (neutral, civilian, a map's own sides). */
// ------------------------------------------------------------------------------------------------
static Int gxSlotOfPlayer( const Player *p )
{
	Int slot = -1;
	if( p == nullptr || sscanf( KEYNAME( p->getPlayerNameKey() ).str(), "player%d", &slot ) != 1
			|| slot < 0 || slot >= MAX_SLOTS )
		return -1;
	return slot;
}

// ------------------------------------------------------------------------------------------------
/** GeneralsX: May THIS sender's cheat order run?
	*
	* The sandbox rule first, unchanged: at most one human, anybody may. Otherwise only a slot the RELAY proved an
	* admin for this match. The mask is simulation state - startNewGame copied it from the game options on a new
	* game, the host stamped it on MSG_GAME_START, and every other peer refused to start unless its own relay roster
	* confirmed every bit (LANAPI::OnGameStart) - so every peer, and a replay, give the same answer for the same
	* order. The sender is the Player the network layer built the message for, never anything the order says. */
// ------------------------------------------------------------------------------------------------
// GeneralsX: (observer seats) A slot "is" a sender when the start's slot map sends it to
// that Player (PlayerList::assignSlotIndices): a player's own "player<slot>", or - every observer slot sharing it - the
// ReplayObserver. Fixed at the start from the same slot list on every peer and in a replay.
static Bool gxSlotIsSender( Int slot, const Player *sender )
{
	return sender != nullptr && ThePlayerList != nullptr && ThePlayerList->getPlayerFromSlotIndex( slot ) == sender;
}

Bool GameLogic::gxCheatAllowedFor( const Player *sender ) const
{
	if( gxCheatsAllowed() )
		return TRUE;
	for( Int slot = 0; slot < MAX_SLOTS; ++slot )
		if( ( m_gxAdminSlotMask & gxSlotBit( slot ) ) != 0 && gxSlotIsSender( slot, sender ) )
			return TRUE;
	return FALSE;
}

// The slot whose key verified the order being dispatched (gxAdminOrderAccepted), for gxCheatNotice: several observer
// slots share one Player, so the Player alone cannot name the admin who sent it. -1 = none. Display only.
static Int s_gxAdminOrderSlot = -1;

// ------------------------------------------------------------------------------------------------
/** GeneralsX: See GameLogic.h. The sender's SLOT only picks which stamped key to check
	* against; it is never evidence by itself, because the network does not bind an order's sender field to the peer it
	* came from (Common/GXAdminOrder.h). The signature is the evidence. */
// ------------------------------------------------------------------------------------------------
// GeneralsX: Every admin slot the sender can be is tried:
// its own, or each observer slot sharing the ReplayObserver - an admin seated as an observer signs as its NETWORK slot
// (Network.cpp gxSealAdminOrder), which no Player names. The payload binds the slot and each slot has its own key, so
// at most one verifies, and which one is decided from the order's own arguments and simulation state alone - the same
// on every peer and in a replay, which never records the network slot.
Bool GameLogic::gxAdminOrderAccepted( const GameMessage *msg, const Player *sender )
{
	s_gxAdminOrderSlot = -1;
	UnsignedInt seq = 0;
	unsigned char sig[64];
	if( msg == nullptr || !gxAdminTrailerOf( msg, &seq, sig ) )
		return FALSE;
	for( Int slot = 0; slot < MAX_SLOTS; ++slot )
	{
		if( ( m_gxAdminSlotMask & gxSlotBit( slot ) ) == 0 || !gxSlotIsSender( slot, sender ) )
			continue;
		GXAdminPayload payload;
		gxAdminPayloadOf( msg, slot, seq, m_gxAdminSeed, &payload );
		if( !gxAdminOrderDecide( &payload, m_gxAdminKeys[slot], sig, seq, m_gxAdminSeq[slot] ) )
			continue;
		m_gxAdminSeq[slot] = seq;
		s_gxAdminOrderSlot = slot;
		return TRUE;
	}
	return FALSE;
}

// ------------------------------------------------------------------------------------------------
/** GeneralsX: WHO OWNS A CHEAT SPAWN: a spawn ordered BY a defeated player or an
	* observer, or aimed AT one, makes "civilian" units - the Neutral player's, which nobody commands - so a player who is
	* out of the game can still stock the map for the living, but never field units of their own or hand them to a live
	* player. Neutral, not the map's PlyrCivilian: every slot player is set NEUTRAL to it (PlayerList::newGame), where
	* PlyrCivilian's relations are the map's own and it may not exist; garrisoning and capturing judge only that relation
	* (ActionManager::canGarrison, canCaptureBuilding), and the engine's unmanned vehicles, which any infantry may crew,
	* are always the Neutral player's (onGxCheatSpawnUnit makes its vehicles so). Every other order keeps gxCheatTarget's
	* refusal of a defeated or observing target. isPlayerActive() is simulation state (killPlayer, and the observer flag
	* fixed at the start), so every peer and a replay choose the same owner. The Debug menu asks the same question to say
	* so before the click.
	*
	* GeneralsX: ...AIMED AT one only: the ORDERER's state no longer decides the owner.
	* "as an admin I need to be able to select a player from the cheats tab and then go to spawn and spawning a unit would
	* assign in that player" - an admin watching as an observer, or one already defeated, got Neutral units instead.
	* Whether an orderer may cheat at all is gxCheatTarget's gate (the sandbox, or a signed relay-proven admin), and no
	* other cheat asks after the orderer's state: an observing admin already gives cash to, or defeats, any live player. */
// ------------------------------------------------------------------------------------------------
Bool GameLogic::gxSpawnGoesNeutral( const Player *target )
{
	return target != nullptr && !target->isPlayerActive();
}

// ------------------------------------------------------------------------------------------------
/** Resolve argument 0 to the player the cheat is aimed at - or to nothing, which every handler
	* treats as "do nothing", when the match refuses cheats or the target is out of the game. Every
	* MSG_GX_CHEAT_* handler resolves its target here first, so these refusals cover them all. A spawn
	* at a player who is out of the game resolves to the Neutral player instead (gxSpawnGoesNeutral). */
// ------------------------------------------------------------------------------------------------
static Player *gxCheatTarget( const GameMessage *msg )
{
	if( gxCheatArgsOk( msg, 1 ) == FALSE || ThePlayerList == nullptr )
		return nullptr;

	// The sandbox (at most one human) takes any order, signed or not, exactly as before. Anything else must be a SIGNED
	// order from a relay-proven admin slot (gxAdminOrderAccepted), which is also where its sequence number is spent.
	const Player *sender = ThePlayerList->getNthPlayer( msg->getPlayerIndex() );
	if( GameLogic::gxCheatsAllowed() == FALSE && TheGameLogic->gxAdminOrderAccepted( msg, sender ) == FALSE )
	{
		static Int s_refusals = 0;	// Trade-off: capped so a flood of cheats cannot turn into a flood of fflush
		if( s_refusals < 32 )
		{
			++s_refusals;
			const Bool adminSlot = TheGameLogic->gxCheatAllowedFor( sender );
			fprintf( stderr, "[GXGATE] MSG_GX_CHEAT %d refused: more than one human in this match and player %d %s (frame %u)\n",
				(Int)msg->getType(), msg->getPlayerIndex(),
				adminSlot ? "is an admin slot but the order carries no valid signature" : "is no relay-proven admin",
				(unsigned)TheGameLogic->getFrame() );
			fflush( stderr );
		}
		return nullptr;
	}

	const Int idx = msg->getArgument( 0 )->integer;

	// Index 0 is permanently the neutral player (PlayerList::init sets m_playerCount = 1), and
	// PlayerList::getNthPlayer only range-checks against MAX_PLAYER_COUNT, not the live count -
	// indices in [getPlayerCount(), MAX_PLAYER_COUNT) return a real but unconfigured Player.
	if( idx <= 0 || idx >= ThePlayerList->getPlayerCount() )
		return nullptr;

	Player *target = ThePlayerList->getNthPlayer( idx );

	// GeneralsX: ...EXCEPT A SPAWN, which is not refused but made NEUTRAL: aimed at a defeated
	// or observing player, its units go to the Neutral player (gxSpawnGoesNeutral, not by the orderer's state
	// since 05/10). Checked before the refusal below, which every other order still meets unchanged.
	if( msg->getType() == GameMessage::MSG_GX_CHEAT_SPAWN_UNIT && GameLogic::gxSpawnGoesNeutral( target ) )
	{
		DEBUG_LOG(("GX cheat frame %d: SPAWN_UNIT aimed at player %d from player %d goes to the Neutral player",
			TheGameLogic->getFrame(), idx, msg->getPlayerIndex()));
		return ThePlayerList->getNeutralPlayer();
	}

	// GeneralsX: A DEFEATED OR OBSERVING TARGET TAKES NO ORDER BUT REVEAL -
	// enforced where the order EXECUTES. The Debug menu refuses one at click time (DebugMenu.cpp
	// beginCheatOrder), but the order lands a run-ahead later, and the target can be defeated in
	// between: "Give cash" then paid a player who can never spend it. isPlayerActive() is simulation
	// state (killPlayer / Player::xfer), so every peer and a replay decide alike. Reveal / Re-shroud
	// stay allowed, exactly as at click time: showing a defeated player the map is how they watch.
	if( target != nullptr && !target->isPlayerActive() && msg->getType() != GameMessage::MSG_GX_CHEAT_REVEAL_MAP )
	{
		DEBUG_LOG(("GX cheat frame %d: order %d refused, player %d is %s (from player %d)", TheGameLogic->getFrame(),
			(Int)msg->getType(), idx, target->isPlayerObserver() ? "observing" : "defeated", msg->getPlayerIndex()));
		return nullptr;
	}

	return target;
}

// ------------------------------------------------------------------------------------------------
/** GeneralsX: EVERY PLAYER SEES AN ADMIN CHEAT.
	*
	* Called for every MSG_GX_CHEAT_* with whether its handler applied it. Only an applied cheat in a match with
	* more than one human is announced - that is the admin path; a sandbox (one human) says nothing to nobody. It
	* runs inside the simulation's own dispatch, on every peer on the same frame, and its words come from
	* simulation state only (display names, the order's own arguments), so every peer draws the same line. Display
	* only: it writes nothing the simulation reads, and a [GXADMIN] line on stderr is the harness's record of it. */
// ------------------------------------------------------------------------------------------------
static void gxCheatNotice( const GameMessage *msg, bool applied )
{
	if( !applied || msg == nullptr || ThePlayerList == nullptr || GameLogic::gxCheatsAllowed() )
		return;

	// Not const: Player::getPlayerDisplayName is not a const member.
	Player *sender = ThePlayerList->getNthPlayer( msg->getPlayerIndex() );
	const Int targetIdx = msg->getArgument( 0 )->integer;	// the handler applied it, so argument 0 was checked
	Player *target = ThePlayerList->getNthPlayer( targetIdx );
	if( sender == nullptr || target == nullptr )
		return;

	UnicodeString what;
	switch( msg->getType() )
	{
		case GameMessage::MSG_GX_CHEAT_GIVE_MONEY:
		{
			// Unsigned negation, as the handler's withdraw does: -INT_MIN would overflow.
			const Int amount = msg->getArgument( 1 )->integer;
			if( amount < 0 )
				what.format( L"took $%u from", 0u - (UnsignedInt)amount );
			else
				what.format( L"gave $%d to", amount );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_SET_MONEY:
		{
			const Int amount = msg->getArgument( 1 )->integer;
			what.format( L"set the cash to $%d of", ( amount < 0 ) ? 0 : amount );
			break;
		}
		case GameMessage::MSG_GX_CHEAT_REVEAL_MAP:
			what = msg->getArgument( 1 )->boolean ? L"revealed the map for" : L"re-shrouded the map for";
			break;
		case GameMessage::MSG_GX_CHEAT_KILL_PLAYER:		what = L"defeated"; break;
		case GameMessage::MSG_GX_CHEAT_KILL_OBJECTS:
			what = msg->getArgument( 1 )->boolean ? L"destroyed the units and buildings of" : L"destroyed the units of";
			break;
		case GameMessage::MSG_GX_CHEAT_SPAWN_UNIT:
			// The handler gave them to the Neutral player if the target is out of the game (gxCheatTarget): say so, or
			// the line names as their owner a player who does not own them. The target has not changed since - same dispatch.
			what = GameLogic::gxSpawnGoesNeutral( target ) ? L"spawned civilian (Neutral) units, aimed at" : L"spawned units for";
			break;
		case GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS:	what = L"deleted objects of"; break;
		default:										what = L"used a cheat on"; break;
	}

	// An observer admin's own name: its Player is the ReplayObserver every observer slot shares.
	const Int fromSlot = ( s_gxAdminOrderSlot >= 0 ) ? s_gxAdminOrderSlot : gxSlotOfPlayer( sender );
	UnicodeString who = sender->getPlayerDisplayName();
	if( ThePlayerList->isSharedBySlots( sender->getPlayerIndex() ) && TheGameInfo != nullptr
			&& TheGameInfo->getConstSlot( fromSlot ) != nullptr )
		who = TheGameInfo->getConstSlot( fromSlot )->getName();
	UnicodeString line = L"[Admin] ";
	line.concat( who );
	line.concat( L" " );
	line.concat( what );
	line.concat( L" " );
	line.concat( target->getPlayerDisplayName() );

	fprintf( stderr, "[GXADMIN] notice frame=%u from=slot%d cheat=%d target=slot%d shown=%d\n",
		(unsigned)TheGameLogic->getFrame(), (int)fromSlot, (int)msg->getType(),
		(int)gxSlotOfPlayer( target ), TheInGameUI != nullptr ? 1 : 0 );
	fflush( stderr );

	if( TheInGameUI == nullptr )
		return;
	// The lobby's admin colour, GX_WEB_TONE_ADMIN 0xD62B44 (GameClient/GXChatLine.h), so the line reads as the
	// operator's voice does in the lobby and in match chat. messageNoFormat: a display name is not a format string.
	RGBColor admin;
	admin.red = 0xD6 / 255.0f;
	admin.green = 0x2B / 255.0f;
	admin.blue = 0x44 / 255.0f;
	TheInGameUI->messageNoFormat( &admin, line );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatGiveMoney(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	Money *m = target->getMoney();
	if( m == nullptr )
		return false;

	// Money::deposit/withdraw take UnsignedInt, so a negative delta must be negated, not cast.
	// playSound=FALSE: this fires on every peer at once and the chime is meaningless for a remote
	// target. trackIncome=FALSE: cheated cash must not pollute the cash-per-minute readout. Same
	// choice ScriptActions::doSetMoney makes.
	const Int amount = msg->getArgument( 1 )->integer;
	if( amount < 0 )
		m->withdraw( (UnsignedInt)( -amount ), FALSE );
	else
		m->deposit( (UnsignedInt)amount, FALSE, FALSE );

	DEBUG_LOG(("GX cheat frame %d: GIVE_MONEY %d to player %d (from player %d), now %u",
		getFrame(), amount, target->getPlayerIndex(), msg->getPlayerIndex(), m->countMoney()));

	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatSetMoney(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	Money *m = target->getMoney();
	if( m == nullptr )
		return false;

	const Int amount = msg->getArgument( 1 )->integer;
	m->withdraw( m->countMoney(), FALSE );
	if( amount > 0 )
		m->deposit( (UnsignedInt)amount, FALSE, FALSE );

	DEBUG_LOG(("GX cheat frame %d: SET_MONEY %d on player %d (from player %d), now %u",
		getFrame(), amount, target->getPlayerIndex(), msg->getPlayerIndex(), m->countMoney()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Reveal or re-shroud the whole map for one player, and make that player's MINIMAP actually show
	* it. Those are two independent gates and only one of them is the shroud:
	*
	*  1) The shroud grid. It is simulation state and it IS in the frame CRC (PartitionCell::crc xfers
	*     m_shroudLevel for all MAX_PLAYER_COUNT players and PartitionManager::crc walks every cell),
	*     so a one-sided reveal is a guaranteed mismatch. This MUST happen here and not on the client.
	*     revealMapForPlayerPermanently, not revealMapForPlayer: the permanent variant does addLooker
	*     with no matching removeLooker so cells land on CELLSHROUD_CLEAR, whereas the plain variant
	*     lands them on CELLSHROUD_FOGGED, where W3DRadar::canRenderObject rejects every blip
	*     (W3DRadar.cpp:665, "shroudedStatus > OBJECTSHROUD_PARTIAL_CLEAR"). Fogged gives a dim map
	*     with no units on it, which reads as "the cheat did nothing".
	*
	*  2) Whether the minimap widget is drawn AT ALL. rts::localPlayerHasRadar()
	*     (Core/.../GameUtility.cpp:52) is isRadarForced || (!isRadarHidden && player->hasRadar()), and
	*     W3DLeftHUDDraw draws nothing whatsoever when it is false. With no Command Center,
	*     Player::hasRadar() is false and the minimap stays black no matter how deshrouded the world
	*     is. So the reveal cheat also forces the radar on for its target.
	*
	*     That force is display-only and cannot desync: Radar::crc has an empty body
	*     (Radar.cpp:1254), TheRadar is absent from GameLogic::getCRC (which covers Objects, the random
	*     seed, ThePartitionManager, ThePlayerList and TheAI), and the sole reader of isRadarForced is
	*     localPlayerHasRadar, whose every consumer is client draw/input code. The engine already calls
	*     forceOn from logic - GameLogic.cpp:1755 for observers and VictoryConditions.cpp:248 on local
	*     defeat. Because forceOn is keyed on player index, forcing the TARGET's radar changes the
	*     display only on the target's own machine and correctly changes nothing on anyone else's. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatRevealMap(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || ThePartitionManager == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	const Int  idx    = target->getPlayerIndex();
	const Bool reveal = msg->getArgument( 1 )->boolean;

	if( idx < 0 || idx >= MAX_PLAYER_COUNT )
		return false;

	if( s_gxRevealed[ idx ] == reveal )
		return false;								// already in that state; addLooker is a counter

	s_gxRevealed[ idx ] = reveal;

	// Gate 2 - the minimap widget. Only ever release a force we ourselves took, so that un-revealing
	// cannot switch off a radar that VictoryConditions turned on for a defeated player or that a map
	// script forced on.
	if( TheRadar != nullptr )
	{
		if( reveal )
		{
			if( TheRadar->isRadarForced( idx ) == FALSE )
			{
				s_gxRadarForced[ idx ] = TRUE;
				TheRadar->forceOn( idx, TRUE );
			}
		}
		else if( s_gxRadarForced[ idx ] )
		{
			s_gxRadarForced[ idx ] = FALSE;
			TheRadar->forceOn( idx, FALSE );
		}
	}

	// Gate 1 - the shroud grid. begin/endSetShroudLevel batches the per-cell texture pushes; without
	// it W3DRadar::setShroudLevel does Get_Surface_Level + Lock + Unlock once PER CELL, thousands of
	// times in one frame. PartitionCell::addLooker pushes to TheDisplay AND TheRadar on the shroud
	// edge transition, but only when the index matches the observed/local player, so a peer revealing
	// someone else's map correctly repaints nothing and no refreshShroudForLocalPlayer() is wanted.
	if( TheRadar != nullptr )
		TheRadar->beginSetShroudLevel();

	if( reveal )
	{
		ThePartitionManager->revealMapForPlayerPermanently( idx );
	}
	else
	{
		// GeneralsX: Re-shroud = lift the reveal, nothing more.
		// This used to copy MSG_META_DEMO_ENSHROUD (LookAtXlat.cpp:653) and
		// follow the undo with shroudMapForPlayer, which pulses addShrouder/removeShrouder on every cell and so turns
		// every cell nobody is looking at from FOGGED (m_currentShroud 0) to SHROUDED (1): black, "never seen".
		// The undo alone is the exact inverse of the reveal: removeLooker on a cell at -1 goes to min(activeShroudLevel,1),
		// i.e. 0 = FOGGED where no shroud generator covers it (1 = SHROUDED where one does, as in a normal game), and a
		// cell a unit is looking at stays below -1 so it stays CLEAR. Retail's debug key keeps its own black-out.
		// A cell never seen before the reveal also ends FOGGED (addLooker took 1 straight to -1, so that memory is gone): the reveal
		// marks the map explored. Fogged enemy structures show as ghosts only where m_everSeenByPlayer was set - the same
		// not-CRC'd, rendered-peer-only state ordinary fog already has; it is not a new class of dependency.
		// GX-RESHROUD-BEGIN (scripts/test/reshroud-fog-test.py compiles this line against the real PartitionManager code)
		ThePartitionManager->undoRevealMapForPlayerPermanently( idx );
		// GX-RESHROUD-END
	}

	if( TheRadar != nullptr )
	{
		TheRadar->endSetShroudLevel();

		// Rebuild the blips now rather than on the next multiple of OVERLAY_REFRESH_RATE frames.
		// This reads PartitionData::getShroudedStatus, which memoises into m_shroudedness /
		// m_everSeenByPlayer and can free ghost snapshots - none of which is CRC'd
		// (PartitionManager::crc walks m_cells only, not PartitionData), and all of which the client
		// draw pass would have done a few frames later anyway.
		TheRadar->refreshObjects();
	}

	DEBUG_LOG(("GX cheat frame %d: REVEAL_MAP %d for player %d (from player %d)",
		getFrame(), (Int)reveal, idx, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Force a player to be defeated, through exactly the path the game itself uses, so that the score
	* screen, observer mode and the remaining-player checks all happen normally.
	*
	* Mirrors GameLogic::onSelfDestruct, except the victim is an ARGUMENT rather than the sender.
	* MSG_SELF_DESTRUCT cannot simply be retargeted: NetGameCommandMsg::constructGameMessage
	* (Core/.../NetCommandMsg.cpp:157) overwrites the message's player index with the Player named
	* "player<senderNetSlot>", so it can only ever kill its own sender.
	*
	* There is no Player::setDefeated - defeat is DERIVED. VictoryConditions::update polls
	* hasSinglePlayerBeenDefeated() every frame on every peer and does all the bookkeeping itself:
	* the permanent map reveal for the dead player, the GUI:PlayerHasBeenDefeated banner, the AI slot's
	* lastFrameInGame, a belt-and-braces killPlayer(), the diplomacy popup refresh, and
	* m_localPlayerDefeated + TheRadar->forceOn for a locally defeated player. All of that comes free
	* provided the kill itself is synchronised, which is the whole point of routing it through here. */
// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
/** Hand @p target's assets to its first living mutual ally, exactly as onSelfDestruct does for a quitter (MSG_SELF_DESTRUCT(TRUE),
	* the Exit and Surrender buttons' order); nothing when it has none. The caller kills @p target after it either way - that also
	* takes the beacons and the odds and ends that do not transfer. Used by the cheat kill and the vote kick. */
// ------------------------------------------------------------------------------------------------
static void gxTransferToLivingAlly( Player *target )
{
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		if( i == target->getPlayerIndex() )
			continue;

		Player *other = ThePlayerList->getNthPlayer( i );
		if( other == nullptr )
			continue;

		if( target->getRelationship( other->getDefaultTeam() ) == ALLIES &&
				other->getRelationship( target->getDefaultTeam() ) == ALLIES )
		{
			if( TheVictoryConditions && TheVictoryConditions->hasSinglePlayerBeenDefeated( other ) )
				continue;

			other->transferAssetsFromThat( target );
			break;
		}
	}
}

bool GameLogic::onGxCheatKillPlayer(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;
	if( target == ThePlayerList->getNeutralPlayer() )
		return false;
	// (an observing or already-dead target never gets here - gxCheatTarget refuses it)

	if( msg->getArgument( 1 )->boolean )
		gxTransferToLivingAlly( target );	// hand the assets to a living mutual ally first, exactly as onSelfDestruct does

	// kill unconditionally: onSelfDestruct also calls this after a successful transfer, to take out
	// the beacons and other odds and ends that do not transfer.
	target->killPlayer();

	DEBUG_LOG(("GX cheat frame %d: KILL_PLAYER %d transferToAlly=%d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)msg->getArgument( 1 )->boolean, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Destroy everything a player owns, optionally including structures. Distinct from KILL_PLAYER:
	* this wipes the army without marking the player dead, though VictoryConditions will then defeat
	* them on its own if nothing survives, which is usually what you want. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatKillObjects(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	const Bool includeStructures = msg->getArgument( 1 )->boolean;

	// COLLECT, THEN KILL. Never kill inside the walk: killing a transport ejects its passengers,
	// which re-teams objects and can unlink them mid-iteration. Team::killTeam does the same two-pass
	// dance and its own comment explains why. Collect IDs rather than pointers, so that a deferred
	// destroy happening between the two passes cannot hand us a dangling Object*.
	std::vector<ObjectID> doomed;
	for( Object *obj = getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->isEffectivelyDead() )
			continue;
		if( obj->getControllingPlayer() != target )
			continue;
		if( includeStructures == FALSE && obj->isKindOf( KINDOF_STRUCTURE ) )
			continue;

		doomed.push_back( obj->getID() );
	}

	for( size_t i = 0; i < doomed.size(); ++i )
	{
		Object *obj = findObjectByID( doomed[ i ] );
		if( obj != nullptr )
			obj->kill();							// NO ARGUMENTS - Generals/.../Object.h:215 takes none
	}

	DEBUG_LOG(("GX cheat frame %d: KILL_OBJECTS on player %d, %d objects, structures=%d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)doomed.size(), (Int)includeStructures, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Upper bound on MSG_GX_CHEAT_SPAWN_UNIT's count argument.
	*
	* The count arrives from a free-text field. Without a clamp, one fat-fingered "100000" builds a
	* hundred thousand Objects inside a single logic frame - and because this is a synchronised order
	* it happens on EVERY peer at the same instant, so the whole game locks up together and reads as a
	* network fault rather than as the typo it is. That is a self-inflicted denial of service.
	*
	* 50 is chosen as "clearly more than a full test army, clearly less than a stall": the placement
	* loop below is O(count) partition ring searches plus O(count) object constructions, all inside
	* one frame's message dispatch. Raise it only with a frame-time measurement in hand. */
// ------------------------------------------------------------------------------------------------
static const Int GX_CHEAT_MAX_SPAWN_COUNT = 50;

// ------------------------------------------------------------------------------------------------
/** Pull a position from a message argument onto the playable map.
	*
	* The caller's position came from the SENDER's camera and travelled in the message. Resolving it
	* here from TheTacticalView would give a different answer on every peer. Clamp it into the map the
	* same way onPlaceBeacon does, so a bad argument cannot drop an object off the partition grid
	* (where it would have no cell intersections, hence be permanently shrouded and unreachable). */
// ------------------------------------------------------------------------------------------------
static void gxCheatClampToMap( Coord3D *pos )
{
	if( pos == nullptr || TheTerrainLogic == nullptr )
		return;

	Region3D extent;
	TheTerrainLogic->getExtent( &extent );
	// TheSuperHackers @refactor 46607a90 Region3D query methods now take a const reference
	// rather than a pointer, so this call site dereferences.
	if( extent.isInRegionNoZ( *pos ) == FALSE )
		*pos = TheTerrainLogic->findClosestEdgePoint( pos );

	pos->z = TheTerrainLogic->getGroundHeight( pos->x, pos->y );
}

// ------------------------------------------------------------------------------------------------
/** Spawn 'count' objects for a player, clustered around a location, at a veterancy level.
	*
	* PLACEMENT. Every unit dropped on the bare message position would be one stack of N models on one
	* point. The engine's own "spawn a group" code is ObjectCreationList's m_spreadFormation path
	* (ObjectCreationList.cpp:1386) and UnitCrateCollide::executeCrateBehavior: create the object, then
	* ThePartitionManager->findPositionAround a centre, then setPosition. This does the same, with one
	* correction.
	*
	* The correction is that findPositionAround CANNOT SEE THE SIBLINGS we placed earlier in this very
	* loop. Object::setPosition only marks the PartitionData dirty; the cells it occupies are not
	* recomputed until ThePartitionManager->UPDATE() at the end of GameLogic::update (GameLogic.cpp,
	* well after processCommandList), so iteratePotentialCollisions inside tryPosition finds nothing
	* where we just put unit 3. Calling ThePartitionManager->update() between spawns - which
	* GameLogic::startNewGame does when it lays out starting units - would fix that, but it also runs
	* processContactList and would therefore fire collide modules in the middle of command dispatch,
	* which no other message handler does.
	*
	* So the separation comes from geometry, not from collision queries: units are assigned to a
	* hexagonal lattice (ring r holds 6r slots, rings are one ringStep apart) sized from the template's
	* own bounding circle. Radial gap is 2.5 body radii and the arc gap between neighbouring slots is
	* (pi/3) * ringStep ~= 2.6 body radii, so no two slots can overlap however big the unit is.
	*
	* findPositionAround is then run per slot to VALIDATE that slot - water, cliffs, z-delta, buildings,
	* pathability - starting its search at the slot's own outward angle. In the ordinary case it costs
	* one sample and changes nothing: minRadius is 0, so its first iteration is dist == minRadius, where
	* it sets angleSpacing = TWO_PI and therefore tests exactly one point, the lattice point itself. It
	* only wanders when that point is genuinely illegal, and a unit nudged a couple of metres into a
	* neighbour beats a unit standing in a cliff. Any residual overlap is transient anyway:
	* ThePartitionManager->UPDATE() ends the same frame by running processContactList, which is the
	* engine's own mechanism for pushing coincident mobile objects apart. When the search fails outright
	* we fall back to the raw lattice point rather than skipping the unit - deterministic, and the same
	* choice the patched OCL spread path makes (ObjectCreationList.cpp:1396).
	*
	* Nothing here consumes the logic RNG: the start angle is passed explicitly, so findPositionAround
	* skips its GameLogicRandomValueReal call. That is not required for sync - the handler runs
	* identically on every peer either way - but it keeps the cheat out of the shared random stream and
	* makes the same order produce the same cluster twice.
	*
	* VETERANCY is applied unconditionally rather than only when it is above LEVEL_REGULAR: a brand new
	* Object already picks up its owner's production veterancy (Object.cpp:472 calls
	* getProductionVeterancyLevel), so "spawn me a REGULAR one" has to be able to pull a unit back DOWN
	* from whatever the player's barracks upgrades grant. provideFeedback=FALSE suppresses the promotion
	* FX and sound, matching ObjectCreationList.cpp:1016 - these units were never promoted, they were
	* born this way, and the FX path consults isLogicallyVisible(), which is per-viewer. */
// ------------------------------------------------------------------------------------------------
Int GameLogic::gxSpawnUnitsAround( Player *target, const ThingTemplate *tmpl, const Coord3D &where, Int count,
																	 VeterancyLevel vet, std::vector<ObjectID> *spawnedIds )
{
	if( target == nullptr || tmpl == nullptr || TheThingFactory == nullptr || count < 1 )
		return 0;

	Team *team = target->getDefaultTeam();
	if( team == nullptr )
		return 0;

	Coord3D centre = where;
	gxCheatClampToMap( &centre );

	// One lattice step = 2.5 body radii. The floor keeps a template with a degenerate geometry (some
	// non-combat props declare a near-zero footprint) from collapsing the whole lattice onto a point.
	const Real bodyRadius = tmpl->getTemplateGeometryInfo().getBoundingCircleRadius();
	const Real ringStep   = gxStressLatticeStep( bodyRadius );	// shared with the stress keep-out (GXStress.h)

	Int spawned = 0;
	Int ring    = 0;						// 0 = the centre slot
	Int slot    = 0;						// index within the current ring
	for( Int i = 0; i < count; ++i )
	{
		Object *obj = TheThingFactory->newObject( tmpl, team );
		if( obj == nullptr )
			break;								// out of Objects; do not keep hammering the factory

		// hex lattice: ring 0 has the single centre slot, ring r >= 1 has 6r slots
		const Int  slotsInRing = ( ring == 0 ) ? 1 : 6 * ring;
		const Real slotAngle   = TWO_PI * ( (Real)slot / (Real)slotsInRing );
		const Real slotDist    = (Real)ring * ringStep;

		Coord3D pos = centre;
		pos.x += slotDist * Cos( slotAngle );
		pos.y += slotDist * Sin( slotAngle );
		gxCheatClampToMap( &pos );

		if( ThePartitionManager != nullptr )
		{
			FindPositionOptions fpOptions;
			fpOptions.minRadius     = 0.0f;
			fpOptions.maxRadius     = ringStep;
			fpOptions.startAngle    = slotAngle;					// explicit: do not touch the logic RNG
			fpOptions.flags         = FPF_USE_HIGHEST_LAYER;	// land on the bridge, not underneath it
			fpOptions.ignoreObject  = obj;								// we are the thing being placed

			Coord3D found = pos;
			if( ThePartitionManager->findPositionAround( &pos, &fpOptions, &found ) )
				pos = found;
		}

		obj->setPosition( &pos );
		if( spawnedIds != nullptr )
			spawnedIds->push_back( obj->getID() );
		if( obj->getExperienceTracker() != nullptr )
			obj->getExperienceTracker()->setVeterancyLevel( vet, FALSE );

		++spawned;

		// advance to the next lattice slot
		if( ++slot >= slotsInRing )
		{
			slot = 0;
			++ring;
		}
	}

	return spawned;
}

// ------------------------------------------------------------------------------------------------
/** MSG_GX_CHEAT_SPAWN_UNIT: validate the order's arguments, then place through gxSpawnUnitsAround
	* above - the same lattice the lobby-agreed stress battlefield uses (GameLogic.cpp). */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatSpawnUnit(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || TheThingFactory == nullptr || gxCheatArgsOk( msg, 3 ) == FALSE )
		return false;

	// The wire carries the NUMERIC template id, which is what MSG_DOZER_CONSTRUCT does - there is no
	// string argument type in GameMessageArgumentDataType. Both peers must be running identical game
	// data for the id to resolve to the same template; they must already be running identical
	// binaries for the protocol to line up at all, so this adds no new constraint.
	//
	// The >0xFFFF reject matters: ids are UnsignedShort, so a nonsense argument would truncate into a
	// perfectly valid id and spawn a confidently wrong unit rather than failing.
	const Int templateArg = msg->getArgument( 1 )->integer;
	if( templateArg <= 0 || templateArg > 0xFFFF )
		return false;

	const ThingTemplate *tmpl = TheThingFactory->findByTemplateID( (UnsignedShort)templateArg );
	if( tmpl == nullptr )
		return false;

	// Arguments 3 and 4 are OPTIONAL - see the contract on MSG_GX_CHEAT_SPAWN_UNIT in
	// Common/MessageStream.h. A three-argument message keeps meaning exactly "one LEVEL_REGULAR unit",
	// so a sender that predates them still behaves the way it always did instead of being misread.
	Int count = 1;
	if( gxCheatArgsOk( msg, 4 ) )
		count = msg->getArgument( 3 )->integer;
	if( count < 1 )
		count = 1;
	if( count > GX_CHEAT_MAX_SPAWN_COUNT )
		count = GX_CHEAT_MAX_SPAWN_COUNT;

	Int vetArg = (Int)LEVEL_REGULAR;
	if( gxCheatArgsOk( msg, 5 ) )
		vetArg = msg->getArgument( 4 )->integer;
	if( vetArg < (Int)LEVEL_FIRST )
		vetArg = (Int)LEVEL_FIRST;
	if( vetArg > (Int)LEVEL_LAST )
		vetArg = (Int)LEVEL_LAST;
	const VeterancyLevel vet = (VeterancyLevel)vetArg;

	std::vector<ObjectID> spawnedIds;
	const Int spawned = gxSpawnUnitsAround( target, tmpl, msg->getArgument( 2 )->location, count, vet, &spawnedIds );

	// GeneralsX: NEUTRAL VEHICLES COME UNMANNED, so a living player can take them
	// spawned "as civilians, ready to enter / capture". A transport takes only its owner's units and hijacking needs an
	// enemy (TransportContain::isValidContainerFor, ActionManager::canHijackVehicle), so a manned Neutral vehicle would be
	// of no use to anyone. Unmanned is the engine's own "any infantry may crew it" state (ActionManager::canEnterObject;
	// the takeover is PhysicsUpdate's collide), and the engine always pairs it with the Neutral player: a sniped vehicle,
	// a Neutron blast, a script's "set unmanned". Their exclusions too: no drone, and no rider-change bike (a bike without
	// its rider scuttles). neverManned: these never had a pilot, so none of a sniped pilot's death - no splatter sound, no
	// car bomb's dead-man's trigger killing a convertible vehicle, no reset of the veterancy set above.
	if( target == ThePlayerList->getNeutralPlayer() )
	{
		for( size_t i = 0; i < spawnedIds.size(); ++i )
		{
			Object *obj = findObjectByID( spawnedIds[ i ] );
			if( obj == nullptr || !obj->isKindOf( KINDOF_VEHICLE ) || obj->isKindOf( KINDOF_DRONE ) )
				continue;
#if RTS_ZEROHOUR
			const ContainModuleInterface *contain = obj->getContain();
			if( contain != nullptr && contain->isRiderChangeContain() )
				continue;
#endif
			obj->setDisabledUntil( DISABLED_UNMANNED, FOREVER, TRUE );
		}
	}

	DEBUG_LOG(("GX cheat frame %d: SPAWN_UNIT template %d x%d vet %d for player %d around %.1f,%.1f (from player %d)",
		getFrame(), templateArg, spawned, (Int)vet, target->getPlayerIndex(),
		msg->getArgument( 2 )->location.x, msg->getArgument( 2 )->location.y, msg->getPlayerIndex()));

	return spawned > 0;
}

// ------------------------------------------------------------------------------------------------
/** Pass-1 collector for MSG_GX_CHEAT_DELETE_OBJECTS.
	*
	* Player::iterateObjects takes a plain function pointer plus a void*, so the filter state has to
	* ride along in a struct. It COLLECTS ONLY - see the handler for why nothing may be destroyed from
	* inside the walk. */
// ------------------------------------------------------------------------------------------------
struct GxCheatDeleteContext
{
	UnsignedShort					templateID;		///< 0 == every type
	std::vector<ObjectID>	doomed;
};

static void gxCheatCollectDoomed( Object *obj, void *userData )
{
	GxCheatDeleteContext *ctx = (GxCheatDeleteContext *)userData;
	if( obj == nullptr || ctx == nullptr )
		return;

	// already on TheGameLogic's destroy list; counting it again would only inflate the log line
	if( obj->isDestroyed() )
		return;

	if( ctx->templateID != 0 )
	{
		// Compare template IDs rather than ThingTemplate pointers: getTemplate() can hand back an
		// override (ThingTemplate.h - "m_override is not used here" for getTemplateID), and the
		// override shares its parent's id, which is precisely the identity the wire argument names.
		const ThingTemplate *tmpl = obj->getTemplate();
		if( tmpl == nullptr || tmpl->getTemplateID() != ctx->templateID )
			return;
	}

	ctx->doomed.push_back( obj->getID() );
}

// ------------------------------------------------------------------------------------------------
/** Remove objects a player owns: every type or one type, all of them or the oldest few.
	*
	* Argument contract, mirrored in the enum comment in Common/MessageStream.h so the sender and this
	* handler cannot drift: templateID <= 0 means every type, count <= 0 means all of them.
	*
	* DESTROY, NOT KILL. Object::kill() runs the death sequence: death FX, the corpse/wreckage OCL, the
	* slow-death update, the "unit lost" EVA and the score bookkeeping. That is the right call for
	* MSG_GX_CHEAT_KILL_OBJECTS, which is meant to simulate an army being wiped out. This cheat exists
	* to CLEAR CLUTTER, so it wants the objects gone, not gone-and-then-some-burning-hulks;
	* TheGameLogic->destroyObject is the engine's "make it not exist" path. It is also the safer of the
	* two here: it is DEFERRED (GameLogic.cpp - the object is pushed onto m_objectsToDestroy and
	* actually deleted by processDestroyList at the end of the frame) and it early-outs on
	* obj->isDestroyed(), so naming the same object twice is harmless.
	*
	* TWO PASSES, ALWAYS. Removing objects inside Player::iterateObjects is the classic crash in this
	* codebase: the walk is over intrusive DLINK team-member lists, and taking an object out re-teams
	* its passengers and unlinks nodes the iterator is standing on. Team::killTeam does the same
	* collect-then-act dance for the same reason. Collect ObjectIDs rather than Object*, so that
	* anything freed between the passes resolves to nullptr instead of to a dangling pointer.
	*
	* WHICH ONES, when count is a limit: the collected ids are SORTED, so "delete 2 Humvees" removes the
	* two OLDEST Humvees. ObjectIDs are handed out by an incrementing counter in creation order, and
	* creation order is lockstep-identical on every peer, so this is both well-defined to the user and
	* provably the same choice everywhere. The raw iterateObjects order would also have been identical
	* on every peer, but it is essentially reverse-insertion order across an arbitrary set of team
	* prototypes - same units deleted, no way to say so in a sentence. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatDeleteObjects(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 3 ) == FALSE )
		return false;

	const Int templateArg = msg->getArgument( 1 )->integer;
	const Int countArg    = msg->getArgument( 2 )->integer;

	GxCheatDeleteContext ctx;
	// Template ids start at 1 (ThingFactory.cpp - "m_nextTemplateID = 1; // not zero!"), so 0 is a
	// sentinel no real template can ever collide with, and 0 is what "every type" means here.
	ctx.templateID = 0;
	if( templateArg > 0 )
	{
		// Resolve through the factory rather than just casting. Ids are UnsignedShort on the wire, so
		// a nonsense argument above 65535 would otherwise TRUNCATE into a perfectly valid id and this
		// cheat would confidently delete the wrong unit type on every peer at once. Reject anything
		// that does not resolve; "delete a type that does not exist" is a no-op, not a guess.
		if( TheThingFactory == nullptr || templateArg > 0xFFFF )
			return false;

		const ThingTemplate *tmpl = TheThingFactory->findByTemplateID( (UnsignedShort)templateArg );
		if( tmpl == nullptr )
			return false;

		ctx.templateID = tmpl->getTemplateID();
	}

	// pass 1 - collect only
	target->iterateObjects( gxCheatCollectDoomed, &ctx );

	std::sort( ctx.doomed.begin(), ctx.doomed.end() );

	size_t limit = ctx.doomed.size();
	if( countArg > 0 && (size_t)countArg < limit )
		limit = (size_t)countArg;

	// pass 2 - remove
	Int removed = 0;
	for( size_t i = 0; i < limit; ++i )
	{
		Object *obj = findObjectByID( ctx.doomed[ i ] );

		// isDestroyed() is not paranoia: destroying a transport takes its passengers with it, and
		// those passengers are on this same list. destroyObject() would early-out on them anyway -
		// the check is here so 'removed' reports what this pass actually took out.
		if( obj == nullptr || obj->isDestroyed() )
			continue;

		destroyObject( obj );
		++removed;
	}

	DEBUG_LOG(("GX cheat frame %d: DELETE_OBJECTS on player %d, template %d (0=any), asked %d (<=0=all), matched %d, removed %d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)ctx.templateID, countArg, (Int)ctx.doomed.size(),
		removed, msg->getPlayerIndex()));

	return removed > 0;
}

bool GameLogic::onEnter(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enter = findObjectByID( msg->getArgument( 1 )->objectID );

	// sanity
	if( enter == nullptr )
		return false;

	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupEnter( enter, CMD_FROM_PLAYER );
	}

	return true;
}

#if RTS_ZEROHOUR
// GeneralsX: WAYPOINT ENTER (MSG_GX_QUEUE_ENTER). One argument, the container. Anything else is
// not a message this build sends, and every peer drops it alike because every peer received the same bytes. Each
// selected unit decides for itself, in m_memberList order, from simulation state only (AIUpdateInterface::gxQueueEnter).
bool GameLogic::onGxQueueEnter(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if( msg->getArgumentCount() != 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_OBJECTID )
		return false;
	Object *container = findObjectByID( msg->getArgument( 0 )->objectID );
	if( container == nullptr || !currentlySelectedGroup )
		return false;
	const Int took = currentlySelectedGroup->groupGxQueueEnter( container, CMD_FROM_PLAYER );
#if defined(GX_TEST_INSTRUMENTS)
	gxTestEnterQueueExecuted( msg, container, took );
#endif
	return took > 0;
}
#endif

#if RTS_ZEROHOUR
// ------------------------------------------------------------------------------------------------
/** GeneralsX: IN-MATCH VOTE KICK - the simulation's half. Every rule is Common/GXVoteKick.h; this
	* only feeds it the synchronised orders, the frame and the living humans, and carries out a PASSED vote.
	*
	* WHO: the sender is getMessagePlayer(msg) - the Player NetGameCommandMsg::constructGameMessage built the order for from the
	* sender's network slot - never an argument. An observer's order resolves to the ReplayObserver every observer slot shares,
	* which is never active, so observers neither start nor vote. A defeated or kicked player is not active either.
	*
	* THE KICK is a quitter's fate (gxTransferToLivingAlly, then killPlayer): what Network::quitGame and the Exit button's
	* MSG_SELF_DESTRUCT(TRUE) do, decided here on the same frame on every peer. VictoryConditions then derives the defeat as for
	* any dead player. The kicked CLIENT leaves by itself a few seconds later (GameClient/GXVoteKick.h, the moderator kick's
	* exit): that is only the network catching up - a client that never leaves stays a dead player who owns nothing and whose
	* every vote order is refused here.
	*
	* SPOOFING, stated: an order's sender is a packet field (NetCommandMsg playerID) the network does not bind to the peer the
	* datagram came from (Common/GXAdminOrder.h), so a modified client can cast a ballot in another slot's name. It can already
	* send that slot's MSG_SELF_DESTRUCT - its surrender - which is strictly stronger; the vote adds no power a forger lacks.
	*
	* FORBIDDEN in here, as in the cheat handlers: TheInGameUI, TheNetwork, TheGameInfo, clocks. The client reads
	* getGxVoteKick() for the banner and the lines. The [GXVOTE] trace is stderr only. */
// ------------------------------------------------------------------------------------------------
static_assert( MAX_PLAYER_COUNT <= GXVK_MAX_PLAYERS, "a Player index must fit the vote kick's masks" );

const GXVoteKickRules &GameLogic::gxVoteKickRules()
{
	static const GXVoteKickRules s_rules = { 30u * LOGICFRAMES_PER_SECOND, 300u * LOGICFRAMES_PER_SECOND, 2u };
	return s_rules;
}

GXVoteKickMask GameLogic::gxVoteKickLiving()
{
	GXVoteKickMask living = 0;
	if( ThePlayerList == nullptr )
		return living;
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		const Player *p = ThePlayerList->getNthPlayer( i );
		if( p != nullptr && p->getPlayerType() == PLAYER_HUMAN && p->isPlayerActive() && !ThePlayerList->isSharedBySlots( i ) )
			living |= gxVoteKickBit( i );
	}
	return living;
}

bool GameLogic::onGxVoteKickStart( GameMessage *msg )
{
	// One INTEGER, the target's Player index. Anything else names nobody, and the rules refuse "nobody" as a target.
	const Bool wellFormed = msg->getArgumentCount() == 1 && msg->getArgumentDataType( 0 ) == ARGUMENTDATATYPE_INTEGER;
	const Int target = wellFormed ? msg->getArgument( 0 )->integer : -1;
	const GXVoteKickMask living = gxVoteKickLiving();
	gxVoteKickResolved( gxVoteKickStart( &m_gxVoteKick, gxVoteKickRules(), getFrame(), msg->getPlayerIndex(), target, living ),
		msg->getPlayerIndex(), living );
	return true;
}

bool GameLogic::onGxVoteKickCast( GameMessage *msg )
{
	// INTEGER vote id, BOOLEAN yes. A malformed ballot is no ballot - never read as a NO, which would veto.
	const Bool wellFormed = msg->getArgumentCount() == 2 && msg->getArgumentDataType( 0 ) == ARGUMENTDATATYPE_INTEGER
		&& msg->getArgumentDataType( 1 ) == ARGUMENTDATATYPE_BOOLEAN;
	const UnsignedInt voteId = wellFormed ? (UnsignedInt)msg->getArgument( 0 )->integer : 0u;	// id 0 is never a vote's
	const GXVoteKickMask living = gxVoteKickLiving();
	gxVoteKickResolved( gxVoteKickCast( &m_gxVoteKick, gxVoteKickRules(), getFrame(), msg->getPlayerIndex(), voteId,
		wellFormed && msg->getArgument( 1 )->boolean, living ), msg->getPlayerIndex(), living );
	return true;
}

void GameLogic::gxVoteKickUpdate()
{
	if( m_gxVoteKick.target < 0 )
		return;
	const GXVoteKickMask living = gxVoteKickLiving();
	gxVoteKickResolved( gxVoteKickTick( &m_gxVoteKick, gxVoteKickRules(), getFrame(), living ), -1, living );
}

void GameLogic::gxVoteKickResolved( GXVoteKickVerdict verdict, Int actor, GXVoteKickMask living )
{
	if( verdict == GXVK_NONE )
		return;
	const Bool running = m_gxVoteKick.target >= 0;
	const Bool refused = verdict >= GXVK_REFUSED_BUSY;
	static Int s_refusals = 0;	// Trade-off: capped so a flood of refused orders cannot turn into a flood of fflush
	if( !refused || s_refusals++ < 64 )
	{
		fprintf( stderr, "[GXVOTE] f=%u id=%u %s actor=%d starter=%d target=%d yes=0x%X living=0x%X\n",
			(unsigned)getFrame(), (unsigned)( running ? m_gxVoteKick.id : m_gxVoteKick.lastId ), gxVoteKickVerdictName( verdict ),
			(int)actor, (int)( running ? m_gxVoteKick.starter : m_gxVoteKick.lastStarter ),
			(int)( running ? m_gxVoteKick.target : m_gxVoteKick.lastTarget ), (unsigned)m_gxVoteKick.yes, (unsigned)living );
		fflush( stderr );
	}
	if( verdict != GXVK_PASSED )
		return;
	Player *target = ThePlayerList->getNthPlayer( m_gxVoteKick.lastTarget );
	if( target == nullptr || !target->isPlayerActive() )
		return;	// the rules never pass a vote on an inactive target; belt and braces
	gxTransferToLivingAlly( target );
	target->killPlayer();
}
#endif // RTS_ZEROHOUR

#if RTS_ZEROHOUR && defined(GX_TEST_INSTRUMENTS)
// ------------------------------------------------------------------------------------------------
/** GeneralsX: WAYPOINT ENTER - the simulation side of the test driver. NEVER in a shipping
	* build (GX_TEST_INSTRUMENTS). It is simulation code reading the environment, so a harness must give every peer the
	* same GX_TEST_ENTERQ_SIM - except for `corrupt`, whose whole point is one peer.
	*
	*   GX_TEST_ENTERQ_SIM="<trigger>:<action>[:<arg>...],..."   trigger = <n> (right after the n-th executed
	*   MSG_GX_QUEUE_ENTER, during command processing) or @<frame> (that logic frame, after its orders)
	*   kill:<d> unman:<d> lock lockai unlockai corrupt touch-unman:<d> capture:<d>:<slot>
	*   spawn:<slot>:<template>:<count>[:<dx>:<dy>]   slot -1 = the neutral player; around slot 0's container 1 + (dx,dy)
	* <d> numbers slot 0's containers (a contain module, getContainMax() > 0) by ascending ObjectID, LIVE. */
// ------------------------------------------------------------------------------------------------
namespace
{
	struct GxEnterqSimAction
	{
		Int n;						///< after the n-th executed order; 0 = by frame
		UnsignedInt frame;	///< @frame
		AsciiString words[8];
		Int numWords;
	};
	std::vector<GxEnterqSimAction> s_gxEnterqSim;
	Bool s_gxEnterqSimParsed = FALSE;
	Int s_gxEnterqExecuted = 0;
	ObjectID s_gxEnterqTouchTarget = INVALID_ID;

	void gxEnterqSimParse()
	{
		if( s_gxEnterqSimParsed )
			return;
		s_gxEnterqSimParsed = TRUE;
		const char *env = getenv( "GX_TEST_ENTERQ_SIM" );
		AsciiString list( env ? env : "" ), entry;
		while( list.nextToken( &entry, "," ) )
		{
			GxEnterqSimAction a;
			a.n = 0; a.frame = 0; a.numWords = 0;
			AsciiString rest = entry, word, trigger;
			if( !rest.nextToken( &trigger, ":" ) )
				continue;
			if( trigger.getLength() > 1 && trigger.getCharAt( 0 ) == '@' )
				a.frame = (UnsignedInt)atoi( trigger.str() + 1 );
			else
				a.n = atoi( trigger.str() );
			while( a.numWords < 8 && rest.nextToken( &word, ":" ) )
				a.words[ a.numWords++ ] = word;
			if( a.numWords > 0 )
				s_gxEnterqSim.push_back( a );
		}
	}

	void gxEnterqCollect( Object *obj, void *userData )
	{
		std::vector<Object *> *out = (std::vector<Object *> *)userData;
		if( obj && !obj->isEffectivelyDead() )
			out->push_back( obj );
	}

	Bool gxEnterqIdLess( const Object *a, const Object *b ) { return a->getID() < b->getID(); }

	// Slot <slot>'s living objects, ascending ObjectID; containers only, or infantry only.
	void gxEnterqObjects( Int slot, Bool containers, std::vector<Object *> &out )
	{
		out.clear();
		Player *p = ThePlayerList ? ThePlayerList->getPlayerFromSlotIndex( slot ) : nullptr;
		if( p == nullptr )
			return;
		std::vector<Object *> all;
		p->iterateObjects( gxEnterqCollect, &all );
		for( size_t i = 0; i < all.size(); ++i )
		{
			Object *o = all[ i ];
			const Bool isContainer = o->getContain() != nullptr && o->getContain()->getContainMax() > 0;
			// Combat infantry: a GLA start's Worker is INFANTRY and DOZER, and the client driver leaves it out too.
			if( containers ? isContainer : ( o->isKindOf( KINDOF_INFANTRY ) && !o->isKindOf( KINDOF_DOZER ) && o->getContainedBy() == nullptr ) )
				out.push_back( o );
		}
		std::sort( out.begin(), out.end(), gxEnterqIdLess );
	}

	Object *gxEnterqContainer( Int d )
	{
		std::vector<Object *> c;
		gxEnterqObjects( 0, TRUE, c );
		return ( d >= 1 && d <= (Int)c.size() ) ? c[ d - 1 ] : nullptr;
	}

	// What a sniper does to a vehicle (ActiveBody.cpp, the DISABLED_UNMANNED branch).
	void gxEnterqUnman( Object *o )
	{
		o->setDisabled( DISABLED_UNMANNED );
		TheGameLogic->deselectObject( o, PLAYERMASK_ALL, TRUE );
		if( o->getAI() )
			o->getAI()->aiIdle( CMD_FROM_AI );
		o->setTeam( ThePlayerList->getNeutralPlayer()->getDefaultTeam() );
	}

	void gxEnterqRun( const GxEnterqSimAction &a, const char *when )
	{
		const AsciiString &act = a.words[ 0 ];
		Object *target = nullptr;
		if( act == "kill" || act == "unman" || act == "touch-unman" || act == "capture" )
			target = a.numWords > 1 ? gxEnterqContainer( atoi( a.words[ 1 ].str() ) ) : nullptr;

		if( act == "kill" && target )
			target->kill();
		else if( act == "unman" && target )
			gxEnterqUnman( target );
		else if( act == "touch-unman" && target )
			s_gxEnterqTouchTarget = target->getID();
		else if( act == "capture" && target && a.numWords > 2 )
		{
			Player *to = ThePlayerList->getPlayerFromSlotIndex( atoi( a.words[ 2 ].str() ) );
			if( to && to->getDefaultTeam() )
				target->defect( to->getDefaultTeam(), 0 );
		}
		else if( act == "lock" || act == "lockai" || act == "unlockai" || act == "corrupt" )
		{
			std::vector<Object *> inf;
			gxEnterqObjects( 0, FALSE, inf );
			for( size_t i = 0; i < inf.size(); ++i )
			{
				AIUpdateInterface *ai = inf[ i ]->getAI();
				if( act == "lock" )
					inf[ i ]->setWeaponLock( PRIMARY_WEAPON, LOCKED_TEMPORARILY );
				else if( ai && act != "corrupt" )
					ai->gxTestLockMachine( act == "lockai" );
				else if( ai && ai->gxTestCorruptQueue() )
				{
					target = inf[ i ];
					break;
				}
			}
		}
		else if( act == "spawn" && a.numWords > 3 )
		{
			const Int slot = atoi( a.words[ 1 ].str() );
			Player *owner = slot < 0 ? ThePlayerList->getNeutralPlayer() : ThePlayerList->getPlayerFromSlotIndex( slot );
			const ThingTemplate *tmpl = TheThingFactory ? TheThingFactory->findTemplate( a.words[ 2 ], FALSE ) : nullptr;
			// Around slot 0's container 1, or - before it has any - its first combat infantry.
			Object *nearObj = gxEnterqContainer( 1 );
			if( nearObj == nullptr )
			{
				std::vector<Object *> inf;
				gxEnterqObjects( 0, FALSE, inf );
				nearObj = inf.empty() ? nullptr : inf[ 0 ];
			}
			if( owner && tmpl && nearObj )
			{
				Coord3D at = *nearObj->getPosition();
				if( a.numWords > 5 )
				{
					at.x += (Real)atoi( a.words[ 4 ].str() );
					at.y += (Real)atoi( a.words[ 5 ].str() );
				}
				GameLogic::gxSpawnUnitsAround( owner, tmpl, at, atoi( a.words[ 3 ].str() ), LEVEL_REGULAR, nullptr );
			}
		}
		const ContainModuleInterface *contain = target ? target->getContain() : nullptr;
		fprintf( stderr, "[GXENTERQ] sim %s %s container=%u aboard=%d f=%u\n", when, act.str(),
			(unsigned)( target ? target->getID() : INVALID_ID ), contain ? (Int)contain->getContainCount() : -1,
			(unsigned)TheGameLogic->getFrame() );
		fflush( stderr );
	}
}

void GameLogic::gxTestEnterQueueExecuted( const GameMessage *msg, Object *container, Int took )
{
	gxEnterqSimParse();
	++s_gxEnterqExecuted;
	Int locked = 0;
	Player *sender = ThePlayerList->getNthPlayer( msg->getPlayerIndex() );
	std::vector<Object *> all;
	if( sender )
		sender->iterateObjects( gxEnterqCollect, &all );
	for( size_t i = 0; i < all.size(); ++i )
		if( all[ i ]->isKindOf( KINDOF_INFANTRY ) && !all[ i ]->isKindOf( KINDOF_DOZER ) && all[ i ]->isCurWeaponLocked() )
			++locked;
	fprintf( stderr, "[GXENTERQ] exec n=%d f=%u from=%d container=%u took=%d templocked=%d\n", s_gxEnterqExecuted,
		(unsigned)getFrame(), msg->getPlayerIndex(), (unsigned)container->getID(), took, locked );
	fflush( stderr );
	AsciiString when;
	when.format( "n=%d", s_gxEnterqExecuted );
	for( size_t i = 0; i < s_gxEnterqSim.size(); ++i )
		if( s_gxEnterqSim[ i ].n == s_gxEnterqExecuted )
			gxEnterqRun( s_gxEnterqSim[ i ], when.str() );
}

void GameLogic::gxTestEnterQueueFrame()
{
	gxEnterqSimParse();
	AsciiString when;
	when.format( "@%u", (unsigned)getFrame() );
	for( size_t i = 0; i < s_gxEnterqSim.size(); ++i )
		if( s_gxEnterqSim[ i ].n == 0 && s_gxEnterqSim[ i ].frame == getFrame() )
			gxEnterqRun( s_gxEnterqSim[ i ], when.str() );
}

void GameLogic::gxTestEnterQueueBeforePartition()
{
	if( s_gxEnterqTouchTarget == INVALID_ID )
		return;
	Object *target = findObjectByID( s_gxEnterqTouchTarget );
	if( target == nullptr )
	{
		s_gxEnterqTouchTarget = INVALID_ID;
		return;
	}
	std::vector<Object *> inf;
	gxEnterqObjects( 0, FALSE, inf );
	for( size_t i = 0; i < inf.size(); ++i )
	{
		AIUpdateInterface *ai = inf[ i ]->getAI();
		if( ai == nullptr || ai->gxTestQueueOwnedGoal() != s_gxEnterqTouchTarget )
			continue;
		// The partition's own overlap test, at the positions this frame's collision pass is about to use: the walker
		// touches its queued transport NOW, and the transport is sniped before the collisions are processed.
		if( !ThePartitionManager->isColliding( inf[ i ], target ) )
			continue;
		const Real dx = inf[ i ]->getPosition()->x - target->getPosition()->x;
		const Real dy = inf[ i ]->getPosition()->y - target->getPosition()->y;
		gxEnterqUnman( target );
		fprintf( stderr, "[GXENTERQ] sim touch-unman f=%u container=%u unit=%u dist=%.1f\n", (unsigned)getFrame(),
			(unsigned)target->getID(), (unsigned)inf[ i ]->getID(), (double)sqrtf( dx * dx + dy * dy ) );
		fflush( stderr );
		s_gxEnterqTouchTarget = INVALID_ID;
		return;
	}
}
#endif

bool GameLogic::onExit(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);
	Object *objectWantingToExit = findObjectByID( msg->getArgument( 0 )->objectID );
#if RETAIL_COMPATIBLE_AIGROUP
	Object *objectContainingExiter = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *objectContainingExiter = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif

	// sanity
	if( objectWantingToExit == nullptr )
		return false;

	if( objectContainingExiter == nullptr )
		return false;

	// sanity, the player must actually control this object
	if( objectWantingToExit->getControllingPlayer() != msgPlayer )
		return false;

#if !RETAIL_COMPATIBLE_CRC
	// GeneralsX: Refuse here, before anything touches the unit. privateExit refuses an exit
	// from a container the unit is not in (TheSuperHackers #3089/#3136), but the weapon lock below was already released
	// and a Chinook named as the exiter had already started landing in ChinookAIUpdate::aiDoCommand by the time it got
	// there. Same predicate, at the network entry.
	const ContainModuleInterface *exiterContain = objectContainingExiter->getContain();
	if( exiterContain == nullptr || !exiterContain->isContained( objectWantingToExit ) )
		return false;
#endif

	objectWantingToExit->releaseWeaponLock(LOCKED_TEMPORARILY);	// release any temporary locks.

	// exit whatever objectWantingToExit is INSIDE of
	AIUpdateInterface *ai = objectWantingToExit->getAIUpdateInterface();
	if( ai )
		ai->aiExit( objectContainingExiter, CMD_FROM_PLAYER );
	// Just like Enter, Exit needs to know the thing to exit.  This can no longer be assumed because of the Tunnel system.
	// If you do not specify the thing to Exit, it will Exit the thing it thinks it is in.  For a tunnel network,
	// that will be the specific Tunnel it entered.  (Scripts can talk directly to the guy to say Get Out Regardless)

	return true;
}

bool GameLogic::onEvacuate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// issue command for either single object or for selected group
	//	AIGroup *group = TheAI->findGroup( *selectedGroupID );
	if( currentlySelectedGroup )
	{
		//Coord3D pos;
		//Bool hasArgs = FALSE;
		//hasArgs = (msg->getArgumentCount() > 0);

		//if (hasArgs)
		//	pos = msg->getArgument(0)->location;

		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.

		// evacuate message is for the selected group
		//if (hasArgs)
		//	currentlySelectedGroup->groupMoveToAndEvacuate( &pos, CMD_FROM_PLAYER );
		//else
		currentlySelectedGroup->groupEvacuate( CMD_FROM_PLAYER );

		// no, this is bad, don't do here, do when POSTING message
		//			pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), GameMessage::MSG_EVACUATE );
	}

	return true;
}

bool GameLogic::onExecuteRailedTransport(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// issue command to currently selected objects
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupExecuteRailedTransport( CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onInternetHack(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
//			ObjectID sourceID = msg->getArgument( 0 )->objectID;
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupHackInternet( CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onGetRepaired(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *repairDepot = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( repairDepot == nullptr )
		return false;

	// tell the currently selected group to go get repaired
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupGetRepaired( repairDepot, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onDock(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *dockBuilding = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( dockBuilding == nullptr )
		return false;

	// tell the currently selected group to go get repaired
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupDock( dockBuilding, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onGetHealed(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *healDest = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( healDest == nullptr )
		return false;

	// tell the currently selected group to enter the building for healing
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupGetHealed( healDest, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onDoRepair(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *repairTarget = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( repairTarget == nullptr )
		return false;

	//
	// tell the currently selected group of objects to go repair the target object, note
	// that only one of them will actually go ahead and do the repair
	//
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupRepair( repairTarget, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onResumeConstruction(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *constructTarget = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( constructTarget == nullptr )
		return false;

	//
	// tell the currently selected group of objects to resume construction on
	// the target object, note that only one of them will go off and resume construction
	// on the target
	//
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupResumeConstruction( constructTarget, CMD_FROM_PLAYER );

	// no, this is bad, don't do here, do when POSTING message
	//		pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), msg->getType() );

	return true;
}

bool GameLogic::onDoSpecialPowerOverrideDestination(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{

	const Coord3D *loc = &msg->getArgument( 0 )->location;
	SpecialPowerType spType = (SpecialPowerType)msg->getArgument( 1 )->integer;

	ObjectID sourceID = msg->getArgument(2)->objectID;
	Object* source = findObjectByID(sourceID);

	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupOverrideSpecialPowerDestination( spType, loc, CMD_FROM_PLAYER );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupOverrideSpecialPowerDestination( spType, loc, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoAttackObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enemy = findObjectByID( msg->getArgument( 0 )->objectID );

	// Check enemy, as it is possible that he died this frame.
	if (enemy)
	{
		if (currentlySelectedGroup)
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
			currentlySelectedGroup->groupAttackObject( enemy, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoForceAttackObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enemy = findObjectByID( msg->getArgument( 0 )->objectID );

	// Check enemy, as it is possible that he died this frame.
	if (enemy)
	{
		if (currentlySelectedGroup)
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
			currentlySelectedGroup->groupForceAttackObject( enemy, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoForceAttackGround(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const Coord3D *pos = &msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		/////////////////////////////////////////////////////////////////////
		//Lorenzen sez: unclear, yet how to solve this for all cases
		//Kris: This code was added to allow the toxin tractor to force attack
		//      while contaminating. When this change was made, it was causing
		//      rangers and scud launchers to reset to primary weapon mode whenever
		//      force attacking while not idle. I fixed this by enforcing the
		//      temporary and permanent modes that are already set when attempting
		//      the new lock. In this case, the temp lock attempt will fail whenever
		//      a permanent lock is in effect, thus fixing the ranger and scud and
		//      allowing the tox tractor to work as well.
		Bool forceAttackRequiresPrimaryWeapon = !currentlySelectedGroup->isIdle();
		if ( forceAttackRequiresPrimaryWeapon )
		{
			currentlySelectedGroup->setWeaponLockForGroup( PRIMARY_WEAPON, LOCKED_TEMPORARILY );
			currentlySelectedGroup->groupAttackPosition( pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);
		}
		else
		///////////////////////////////////////////////////////////////////
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);
			currentlySelectedGroup->groupAttackPosition( pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onQueueUpgrade(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// GeneralsX: the upgrade's name id, not its NameKey: a NameKey differs between peers (GXWireId.h)
	const UpgradeTemplate *upgradeT = TheUpgradeCenter->findUpgradeByWireId( (UnsignedInt)(msg->getArgument( 1 )->integer) );
	if (!upgradeT)	// sanity
		return false;

	if (currentlySelectedGroup)
		currentlySelectedGroup->queueUpgrade( upgradeT );

	return true;
}

bool GameLogic::onCancelUpgrade(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	// GeneralsX: the upgrade's name id, not its NameKey: a NameKey differs between peers (GXWireId.h)
	const UpgradeTemplate *upgradeT = TheUpgradeCenter->findUpgradeByWireId( (UnsignedInt)(msg->getArgument( 0 )->integer) );

	// sanity
	if( producer == nullptr || upgradeT == nullptr )
		return false;

	// the player must actually control the producer object
	if( producer->getControllingPlayer() != msgPlayer )
		return false;

	// producer must have a production update
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
		return false;

	// cancel the upgrade
	pu->cancelUpgrade( upgradeT );

	return true;
}

bool GameLogic::onQueueUnitCreate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	const ThingTemplate *whatToCreate;
	ProductionID productionID;

	// get data from the message
	whatToCreate = TheThingFactory->findByTemplateID( msg->getArgument( 0 )->integer );

	// sanity
	if ( producer == nullptr || whatToCreate == nullptr )
		return false;

	// get the production interface for the producer
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
	{
		DEBUG_CRASH( ("MSG_QUEUE_UNIT_CREATE: Producer '%s' doesn't have a unit production interface",
													producer->getTemplate()->getName().str()) );
		return false;
	}

	// GeneralsX: The ID used to come from the message: the sender's ControlBar took it from
	// this factory's counter on the SENDER's machine only, so every other peer's counter fell behind, the next
	// logic-side queue (a script, a flight deck, the AI) got different IDs on different machines, and a cancel by ID
	// could remove different entries = desync. Every peer allocates it here instead; the message's second argument is
	// no longer read (sweep c066-04).
	productionID = pu->requestUniqueUnitID();

	// queue the build
	pu->queueCreateUnit( whatToCreate, productionID );

	return true;
}

bool GameLogic::onCancelUnitCreate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	ProductionID productionID = (ProductionID)msg->getArgument( 0 )->integer;

	// sanity
	if( producer == nullptr )
		return false;

	// sanity, the player must control the producer
	if( producer->getControllingPlayer() != msgPlayer )
		return false;

	// get the unit production interface
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
		return false;

	// cancel the production
	pu->cancelUnitCreate( productionID );

	return true;
}

bool GameLogic::onDozerConstruct(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const ThingTemplate *place;
	Coord3D loc;
	Real angle;

	// get player, what to place, and location
#if RETAIL_COMPATIBLE_AIGROUP
	Object *constructorObject = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *constructorObject = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	place = TheThingFactory->findByTemplateID( msg->getArgument( 0 )->integer );
	loc = msg->getArgument( 1 )->location;
	angle = msg->getArgument( 2 )->real;

	if( place == nullptr || constructorObject == nullptr )
		return false;  //These are not crashes, as the object may have died before this message came in

	if( msg->getType() == GameMessage::MSG_DOZER_CONSTRUCT )
	{
		TheBuildAssistant->buildObjectNow( constructorObject, place, &loc, angle,
																				constructorObject->getControllingPlayer() );
	}
	else
	{
		Coord3D locEnd;

		// get the end of the line location in the world
		locEnd = msg->getArgument( 3 )->location;

		// place the line of structures, the end location being present will make it happen
		TheBuildAssistant->buildObjectLineNow( constructorObject, place, &loc, &locEnd, angle,
																						constructorObject->getControllingPlayer() );
	}

	// place the sound for putting a building down

	static AudioEventRTS placeBuilding("PlaceBuilding");
	placeBuilding.setObjectID(constructorObject->getID());
	TheAudio->addAudioEvent( &placeBuilding );

	// no, this is bad, don't do here, do when POSTING message
	//		pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), msg->getType() );

	return true;
}

bool GameLogic::onDozerCancelConstruct(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

	// get the building to cancel construction on
#if RETAIL_COMPATIBLE_AIGROUP
	Object *building = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *building = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	if( building == nullptr )
		return false;

	// the player sending this message must actually control this building
	if( building->getControllingPlayer() != msgPlayer )
		return false;

	// Check to make sure it is actually under construction
	if( !building->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) )
		return false;

	// OK, refund the money to the player, unless it is a rebuilding Hole.
	if( !building->testStatus(OBJECT_STATUS_RECONSTRUCTING))
	{
		Money *money = msgPlayer->getMoney();
		UnsignedInt amount = building->getTemplate()->calcCostToBuild( msgPlayer );
		money->deposit( amount, TRUE, FALSE );
	}

	//
	// Destroy the building ... killing the
	// building will automatically cause the dozer also cancel its own building
	// behavior and it will go on its merry way doing other assigned tasks
	//
	building->kill();

	return true;
}

bool GameLogic::onSell(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupSell( CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onToggleOvercharge(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupToggleOvercharge( CMD_FROM_PLAYER );

	return true;
}

#ifdef ALLOW_SURRENDER

bool GameLogic::onDoSurrender(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units surrender
	if( currentlySelectedGroup )
	{
		Object* objWeSurrenderedTo = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
		Bool surrender = msg->getArgument( 1 )->boolean;
		currentlySelectedGroup->groupSurrender( objWeSurrenderedTo, surrender, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onPickUpPrisoner(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *prisoner = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );

	if( prisoner )
	{
		// use selected group
		if( currentlySelectedGroup )
			currentlySelectedGroup->groupPickUpPrisoner( prisoner, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onReturnToPrison(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupReturnToPrison( nullptr, CMD_FROM_PLAYER );

	return true;
}

#endif

bool GameLogic::onCreateSelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	Bool createNewGroup = msg->getArgument( 0 )->boolean;
	Bool firstObject = TRUE;

	for (Int i = 1; i < msg->getArgumentCount(); ++i) {
		Object *obj = findObjectByID( msg->getArgument( i )->objectID );
		if (!obj) {
			continue;
		}

		selectObject(obj, createNewGroup && firstObject, msgPlayer->getPlayerMask());
		firstObject = FALSE;
	}

	return true;
}

bool GameLogic::onRemoveFromSelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	for (Int i = 0; i < msg->getArgumentCount(); ++i) {
		ObjectID objID = msg->getArgument(i)->objectID;
		Object *objToRemove = findObjectByID(objID);
		if (!objToRemove) {
			continue;
		}

		deselectObject(objToRemove, msgPlayer->getPlayerMask());
	}

	return true;
}

bool GameLogic::onDestroySelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->setCurrentlySelectedAIGroup(nullptr);

	return true;
}

bool GameLogic::onPlaceBeacon(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	if (msgPlayer->getPlayerTemplate() == nullptr)
		return false;

	Coord3D pos = msg->getArgument( 0 )->location;
	Region3D r;
	TheTerrainLogic->getExtent(&r);
	if (!r.isInRegionNoZ(pos))
		pos = TheTerrainLogic->findClosestEdgePoint(&pos);
	const ThingTemplate *thing = TheThingFactory->findTemplate( msgPlayer->getPlayerTemplate()->getBeaconTemplate() );

	if (thing && !TheVictoryConditions->hasSinglePlayerBeenDefeated(msgPlayer))
	{
		// how many does this player have active?
		Int count;
		msgPlayer->countObjectsByThingTemplate( 1, &thing, false, &count );
		DEBUG_LOG(("Player already has %d beacons active", count));
		if (count >= TheMultiplayerSettings->getMaxBeaconsPerPlayer())
		{
			if (msgPlayer == ThePlayerList->getLocalPlayer())
			{
				// tell the user
				TheInGameUI->message( TheGameText->fetchGameOnly("GUI:TooManyBeacons") );

				// play a sound
				static AudioEventRTS aSound("BeaconPlacementFailed");
				aSound.setPosition(&pos);
				aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
				TheAudio->addAudioEvent(&aSound);
			}

			return false;
		}
		Object *object = TheThingFactory->newObject( thing, msgPlayer->getDefaultTeam() );
		object->setPosition( &pos );
		object->setProducer(nullptr);

		if (msgPlayer->getRelationship( ThePlayerList->getLocalPlayer()->getDefaultTeam() ) == ALLIES || ThePlayerList->getLocalPlayer()->isPlayerObserver())
		{
			// tell the user
			UnicodeString s;
			s.format(TheGameText->fetchGameOnly("GUI:BeaconPlaced"), msgPlayer->getPlayerDisplayName().str());
			// GeneralsX: s holds the placing player's lobby name - peer-controlled text. message() takes
			// a printf format, so a name like "%s%n" read and wrote through missing arguments on every ally's machine.
			TheInGameUI->messageNoFormat( s );

			// play a sound
			static AudioEventRTS aSound("BeaconPlaced");
			aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
			aSound.setPosition(&pos);
			TheAudio->addAudioEvent(&aSound);

			// beacons are a rare event; play a nifty radar event thingy
			TheRadar->createEvent( object->getPosition(), RADAR_EVENT_INFORMATION );

			if (ThePlayerList->getLocalPlayer()->getRelationship(msgPlayer->getDefaultTeam()) == ALLIES)
				TheEva->setShouldPlay(EVA_BeaconDetected);

			TheControlBar->markUIDirty(); // check if we should grey out the button
		}
		else
		{

			Int updateCount = 0;
			static NameKeyType nameKeyClientUpdate = NAMEKEY("BeaconClientUpdate");
			ClientUpdateModule ** clientModules = object->getDrawable()->getClientUpdateModules();
			if (clientModules)
			{
				while (*clientModules)
				{
					if ((*clientModules)->getModuleNameKey() == nameKeyClientUpdate)
					{
						(*(BeaconClientUpdate **)clientModules)->hideBeacon();
						++updateCount;
					}

					++clientModules;
				}
			}
			DEBUG_ASSERTCRASH(updateCount == 1, ("Saw %d update modules for the beacon!", updateCount));

		}
	}
	else
	{
		// tell the user
		TheInGameUI->message( TheGameText->fetchGameOnly("GUI:BeaconPlacementFailed") );

		// play a sound
		static AudioEventRTS aSound("BeaconPlacementFailed");
		aSound.setPosition(&pos);
		aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
		TheAudio->addAudioEvent(&aSound);
	}

	return true;
}

bool GameLogic::onRemoveBeacon(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	AIGroupPtr allSelectedObjects = TheAI->createGroup();
#if RETAIL_COMPATIBLE_AIGROUP
	msgPlayer->getCurrentSelectionAsAIGroup(allSelectedObjects); // need to act on all objects, so we can hide teammates' beacons.
#else
	msgPlayer->getCurrentSelectionAsAIGroup(allSelectedObjects.Peek()); // need to act on all objects, so we can hide teammates' beacons.
#endif
	if( allSelectedObjects )
	{
		const VecObjectID& selectedObjects = allSelectedObjects->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *beacon = findObjectByID(*it);
			if (beacon)
			{
				// TheSuperHackers @bugfix Prevent runtime crashing when a beacon is no longer associated with an initialized player.
				const PlayerTemplate *playerTemplate = beacon->getControllingPlayer()->getPlayerTemplate();
				if (!playerTemplate)
					continue;

				const ThingTemplate *thing = TheThingFactory->findTemplate( playerTemplate->getBeaconTemplate() );
				if (thing && thing->isEquivalentTo(beacon->getTemplate()))
				{
					if (beacon->getControllingPlayer() == msgPlayer)
					{
						destroyObject(beacon); // the owner is telling it to go away.  such is life.

						TheControlBar->markUIDirty(); // check if we should un-grey out the button
					}
					else if (msgPlayer == ThePlayerList->getLocalPlayer())
					{
						Drawable *beaconDrawable = beacon->getDrawable();
						if (beaconDrawable)
						{

							static NameKeyType nameKeyClientUpdate = NAMEKEY("BeaconClientUpdate");
							ClientUpdateModule ** clientModules = beaconDrawable->getClientUpdateModules();
							if (clientModules)
							{
								while (*clientModules)
								{
									if ((*clientModules)->getModuleNameKey() == nameKeyClientUpdate)
										(*(BeaconClientUpdate **)clientModules)->hideBeacon();

									++clientModules;
								}
							}
						}
					}
				}
			}
		}
#if RETAIL_COMPATIBLE_AIGROUP
		if (allSelectedObjects->isEmpty())
		{
			TheAI->destroyGroup(allSelectedObjects);
			allSelectedObjects = nullptr;
		}
#endif
	}

	return true;
}

bool GameLogic::onSetBeaconText(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if( currentlySelectedGroup )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *beacon = findObjectByID(*it);
			if (beacon)
			{
				Drawable *beaconDrawable = beacon->getDrawable();
				if (beaconDrawable)
				{
					UnicodeString s;
					for( int i=0; i<msg->getArgumentCount(); i++ )
					{
						s.concat( msg->getArgument(i)->wChar );
					}

					if (s.isEmpty())
						beaconDrawable->clearCaptionText();
					else
						beaconDrawable->setCaptionText(s);
				}
			}
		}
	}

	return true;
}

bool GameLogic::onSelfDestruct(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	// GeneralsX: An observer quitting (GameLogic::quit, the Surrender button) sends
	// this from its slot, which resolves to the ReplayObserver every observer slot shares. It is dead from its
	// creation, so the kill takes nothing out - but on every OTHER observer's peer it is the local Player, and
	// killPlayer resets that caster's control bar to the observer list. Same start-time slot map as
	// Network::processDestroyPlayerCommand's guard, so every peer, and a replay, skips it alike.
	if (ThePlayerList->isSharedBySlots(msgPlayer->getPlayerIndex()))
		return true;
	// GeneralsX: In-match kick. Simulation state, identical on every peer, read
	// before the kill so that only a self-destruct that takes a LIVING player out can draw the line - once.
	const Bool wasAlive = !msgPlayer->isPlayerDead();

	if (msg->getArgument(0)->boolean)
	{
		// transfer control to any living ally
		Int i=0;
		for (; i<ThePlayerList->getPlayerCount(); ++i)
		{
			if (i != msgPlayer->getPlayerIndex())
			{
				Player *otherPlayer = ThePlayerList->getNthPlayer(i);
				if (msgPlayer->getRelationship(otherPlayer->getDefaultTeam()) == ALLIES &&
					otherPlayer->getRelationship(msgPlayer->getDefaultTeam()) == ALLIES)
				{
					if (TheVictoryConditions->hasSinglePlayerBeenDefeated(otherPlayer))
						continue;

					// a living ally!  hooray!
					otherPlayer->transferAssetsFromThat(msgPlayer);
					msgPlayer->killPlayer(); // just to be safe (and to kill beacons etc that don't transfer)
					break;
				}
			}
		}
		if (i == ThePlayerList->getPlayerCount())
		{
			// didn't find any allies.  die, loner!
			msgPlayer->killPlayer();
		}
	}
	else
	{
		msgPlayer->killPlayer();
	}

	// There is no reason to do any notification here, it now takes place in the victory conditions.
	// bonehead.

	// GeneralsX: In-match kick: "<name> was kicked by <label>" in red.
	//
	// DISPLAY ONLY, AND THAT IS THE WHOLE ARGUMENT FOR IT BEING SAFE HERE. The purity rule above the cheat
	// handlers ("FORBIDDEN in here: ... TheInGameUI") exists because those handlers must not branch the
	// simulation on UI state. This does not branch anything: it runs after the kill, returns void, and
	// writes only TheInGameUI's message list and one log line - VictoryConditions already calls TheInGameUI
	// from logic in exactly this way for "has been Defeated". The kill above reads argument 0 only, so the
	// ticket arguments the kicked client appended change nothing it does. The decision (key, parser, checks)
	// is Common/GXKickTicket.h, inside the digest, so builds that would decide differently refuse each other
	// at join. gxKickShowLine (GameClient/GXKick.cpp) says what else it reads and why none of it matters.
	if (wasAlive)
		gxKickShowLine(msgPlayer, msg);

	return true;
}

bool GameLogic::onSetReplayCamera(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	if (TheRecorder->isPlaybackMode() && TheGlobalData->m_useCameraInReplay && TheControlBar->getObserverLookAtPlayer() == msgPlayer)
	{
		if (TheTacticalView->isCameraMovementFinished())
		{
			const Coord3D pos = msg->getArgument( 0 )->location;
			const Real angle = msg->getArgument( 1 )->real;
			const Real pitch = msg->getArgument( 2 )->real;
			const Real zoom = msg->getArgument( 3 )->real;
			const Mouse::MouseCursor mouseCursor = static_cast<Mouse::MouseCursor>(msg->getArgument( 4 )->integer);
			const ICoord2D mousePos = msg->getArgument( 5 )->pixel;

			// TheSuperHackers @info Definitely call in user mode to ensure the camera operates with auto-zoom
			// over terrain elevations, because the Replay Camera does not store the absolute camera location,
			// but key parameters relative to the terrain height at the camera pivot.
			TheTacticalView->userSetPosition(pos);
			TheTacticalView->userSetAngle(angle);
			TheTacticalView->userSetPitch(pitch);
			TheTacticalView->userSetZoom(zoom);

			// TheSuperHackers @fix Make sure there is no scrolling ever.
			const Coord2D scroll = {0, 0};
			TheTacticalView->userScrollBy(&scroll);

			if (msg->getArgumentCount() >= 8)
			{
				// TheSuperHackers @feature Override all the settings above with real camera position and view direction.
				// This ensures that the camera looks EXACTLY like it was at the time of recording, no matter how the
				// View is configured or tweaked. Note that the above settings are still required to set regardless, because
				// when the replay camera is exited, then the pivot position and angles will be needed to build the camera
				// where it was left off.
				const Coord3D camPos = msg->getArgument( 6 )->location;
				const Coord3D camDir = msg->getArgument( 7 )->location;

				TheTacticalView->setUserControlled(false);
				TheTacticalView->set3DCameraLookAt(camPos, camDir, 0.0f);
			}

			// TheSuperHackers @fix Lock the new location to avoid user input from changing the camera in this frame.
			TheTacticalView->lockUserControlUntilFrame( getFrame() + 1 );

			if (!TheLookAtTranslator->hasMouseMovedRecently())
			{
				// GeneralsX: The cursor is the order's integer, and setCursor (and every
				// device's override) indexes its cursor tables with it; any peer's order is recorded in everyone's
				// replay (net-hardening weapon-slot-r1). An honest recording holds a real cursor.
				if ((Int)mouseCursor >= 0 && (Int)mouseCursor < Mouse::NUM_MOUSE_CURSORS)
					TheMouse->setCursor( mouseCursor );
				TheMouse->setPosition( mousePos.x, mousePos.y );
				TheLookAtTranslator->setCurrentPos( mousePos );
			}
		}
	}

	return true;
}

bool GameLogic::onCreateTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processCreateTeamGameMessage(msg->getType() - GameMessage::MSG_CREATE_TEAM0, msg);

	return true;
}

bool GameLogic::onSelectTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processSelectTeamGameMessage(msg->getType() - GameMessage::MSG_SELECT_TEAM0);

	return true;
}

bool GameLogic::onAddTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processAddTeamGameMessage(msg->getType() - GameMessage::MSG_ADD_TEAM0);

	return true;
}

bool GameLogic::onLogicCrc(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	if (TheNetwork)
	{
		// GeneralsX: Resolve the slot by INDEX, not by matching display names.
		// Upstream reference: TheSuperHackers, PR #3033
		// https://github.com/TheSuperHackers/GeneralsGameCode/pull/3033
		//
		// The old loop scanned TheNetwork->getPlayerName(i) for a string equal to this player's
		// display name and took the first hit. Display names are not unique - they arrive off the
		// wire and nothing dedupes them - so in a lobby where two players share a name the loop
		// binds the WRONG slot, and the isPlayerConnected() test below then asks about somebody
		// else's connection. If that other peer happens to be disconnected, this returns early and
		// the reporting player's CRC is silently DROPPED.
		//
		// The CRC itself was always stored at getPlayerIndex(), so the value never landed on the
		// wrong player - the damage is a missing report, which the desync dialog then renders as
		// "no report" against a peer that did in fact report. That dialog exists to answer "who
		// desynced", so a name collision makes it answer confidently and wrongly.
		//
		// ThePlayerList->getSlotIndex() is a direct player-index -> slot-index lookup built once
		// from the lobby (PlayerList.cpp:530, filled by assignSlotIndices), which is the mapping
		// the name scan was approximating.
		if (msgPlayer->getPlayerType() != PLAYER_HUMAN)
		{
			return false;
		}

		// GeneralsX: The SENDER'S SLOT, carried by the message itself
		// (NetGameCommandMsg::constructGameMessage). Observer slots all map onto the one ReplayObserver,
		// so its Player index cannot tell three observers' reports apart and has no single slot to look
		// up. For a player slot the two agree (one Player per slot), so nothing changes there; the
		// lookup stays as the fallback for a message that did not come off the network.
		const Int slotIndex = (msg->getSenderSlot() >= 0) ? msg->getSenderSlot()
			: ThePlayerList->getSlotIndex(msgPlayer->getPlayerIndex());
		if (slotIndex < 0 || !TheNetwork->isPlayerConnected(slotIndex))
		{
			return false;
		}

		if (slotIndex == (Int)TheNetwork->getLocalPlayerID())
		{
#if defined(RTS_DEBUG)
			// don't even put this in release, cause someone might hack it.
			if (!TheDebugIgnoreSyncErrors)
			{
#endif
				m_shouldValidateCRCs = TRUE;
#if defined(RTS_DEBUG)
			}
#endif
		}

		UnsignedInt newCRC = msg->getArgument(0)->integer;
		//DEBUG_LOG(("Received CRC of %8.8X from %ls on frame %d", newCRC,
			//msgPlayer->getPlayerDisplayName().str(), m_frame));
		m_cachedCRCs[slotIndex] = newCRC;	// keyed by SLOT: one report per observer, never one per shared Player
	}
	else if (TheRecorder && TheRecorder->isPlaybackMode())
	{
		UnsignedInt newCRC = msg->getArgument(0)->integer;
		//DEBUG_LOG(("Saw CRC of %X from player %d.  Our CRC is %X.  Arg count is %d",
			//newCRC, msgPlayer->getPlayerIndex(), getCRC(), msg->getArgumentCount()));

		TheRecorder->handleCRCMessage(newCRC, msgPlayer->getPlayerIndex(), (msg->getArgument(1)->boolean));
	}

	return true;
}

bool GameLogic::onPurchaseScience(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	// GeneralsX: the science's name id, not its NameKey: a NameKey differs between peers (GXWireId.h)
	ScienceType science = TheScienceStore->getScienceFromWireId( (UnsignedInt)msg->getArgument( 0 )->integer );

	// sanity
	if( science == SCIENCE_INVALID )
		return false;

	msgPlayer->attemptToPurchaseScience(science);

	return true;
}
