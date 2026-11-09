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

// FILE: LoadScreen.h /////////////////////////////////////////////////////////////////////////////////
// Author: Chris Huybregts, March 2002
// Desc:   The file will hold the LoadScreen Base class and it's derived classes
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// SYSTEM INCLUDES ////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "Common/SubsystemInterface.h"
#include "GameClient/GameWindow.h"
#include "GameNetwork/GameInfo.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/ChallengeGenerals.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////

// TYPE DEFINES ///////////////////////////////////////////////////////////////
class VideoBuffer;
class VideoStreamInterface;
class WindowVideoManager;

// GeneralsX @feature Claude 13/08/2026 A hook, because this file is Core and the row builder is not.
//
// The load screens' .wnd files author their player rows 0..7 (verified by extracting
// MultiplayerLoadScreen.wnd and GameSpyLoadScreen.wnd from WindowZH.big), while MAX_SLOTS is 12 -
// so both show eight of twelve. The lobby, the in-game player list and the score screen solve this
// with CodeGuiRowsEnsure, which clones a column's missing rows into the band the authored ones
// occupy. That function lives in GeneralsMD and reaches GeneralsMD's own widget factories, so this
// file cannot call it: LoadScreen.cpp is shared with the Generals twin, which has no CodeGui at
// all, and this repository does not add #if gates to Core - that is how the Generals side got
// broken during the re-root.
//
// So Core declares the shape of the favour it wants and GeneralsMD grants it. A twin that never
// registers a builder keeps today's behaviour EXACTLY: the call site below is a null check.
//
// Deliberately not a virtual on LoadScreen: the row building has to happen inside init(), which is
// Core code, before Core binds the pointers - a subclass override would run too late to help.
//
// GeneralsX @bugfix Claude 25/09/2026 `placedByCaller` is for the map start markers, which are placed
// on the map's own waypoints by positionStartSpots rather than laid out in a column: TRUE creates the
// missing ones and positions nothing (CODEGUI_ROWS_FREE), FALSE fits a column into its band.
typedef Int (*LoadScreenRowBuilder)( GameWindow *parent, const char *idFormat, Int rowCount, Bool placedByCaller );

/// Register the row builder. Called once by the game that has one; never called by the twin.
void LoadScreenSetRowBuilder( LoadScreenRowBuilder builder );

/// Build rows idFormat 0..rowCount-1 under parent, if a builder was registered. Returns rows
/// created, or 0 when no builder exists, when parent is null, or when the layout already covers
/// rowCount. Safe and side-effect-free in all three cases.
Int LoadScreenEnsureRows( GameWindow *parent, const char *idFormat, Int rowCount, Bool placedByCaller = FALSE );

// GeneralsX @feature Claude 08/11/2026 SPLIT THE PLAYER LIST WHEN IT DOES NOT FIT (owner 08/11, msg b496: with 32 players
// the multiplayer load screen's rows ran past the frame and off the screen). Called once the screen knows how many rows
// it filled: up to the authored row count nothing moves; past it the columns become two side-by-side groups, each with
// its own header row, the rows sharing the authored band. Same hook arrangement as the row builder above: registered by
// the game that has CodeGui, a no-op in the twin. Columns must be created with placedByCaller = TRUE (positioned here).
typedef void (*LoadScreenRowSplitter)( GameWindow *parent, const char * const *idFormats, Int columnCount,
																			 const char * const *titleNames, Int titleCount, Int usedRows );
void LoadScreenSetRowSplitter( LoadScreenRowSplitter splitter );
void LoadScreenSplitRows( GameWindow *parent, const char * const *idFormats, Int columnCount,
													const char * const *titleNames, Int titleCount, Int usedRows );

///////////////////////////////////////////////////////////////////////////////////////////////////
// Class LoadScreen is the parent class for each other kind of load screen
///////////////////////////////////////////////////////////////////////////////////////////////////
class LoadScreen
{
public:
	LoadScreen();
	virtual ~LoadScreen();

	virtual void init( GameInfo *game ) = 0;		///< Init the loadscreen
	virtual void reset() = 0;		///< Reset the system
	virtual void update() = 0;  ///< Update the state of the slider bars
	virtual void update( Int percent ); ///< Update the state of the slider bars
	void waitForPipelineWarmup(); ///< GeneralsX A2: see LoadScreen.cpp
	virtual void processProgress(Int playerId, Int percentage) = 0;
	virtual void setProgressRange( Int min, Int max ) = 0;
	/// GeneralsX @bugfix Claude 23/09/2026 Start positions are picked after the map loads, so after
	/// init() drew the map preview; screens that show them redraw here. See populateRandomStartPosition.
	virtual void refreshStartSpots( GameInfo * /*game*/ ) { }
protected:
	void setLoadScreen( GameWindow *g ) { m_loadScreen = g; }
	GameWindow *m_loadScreen;		///< The GameWindow that is our loadscreen
	Bool m_gxWarmWaited;				///< GeneralsX A2: this screen already held for the pipeline warm-up (LoadScreen::update)
	GameWindow *m_gxWarmText;		///< GeneralsX A2: this screen's own static text that says what the hold is (null: says nothing)

private:

};

///////////////////////////////////////////////////////////////////////////////////////////////////
// class SinglePlayerLoadScreen is to be used only when we're loading a single player mission
///////////////////////////////////////////////////////////////////////////////////////////////////
class SinglePlayerLoadScreen : public LoadScreen
{
public:
	SinglePlayerLoadScreen();
	virtual ~SinglePlayerLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;		///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;		 ///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override
	{
		DEBUG_CRASH(("We Got to a single player load screen throw the Network..."));
	}

	virtual void setProgressRange( Int min, Int max ) override;

private:
	GameWindow *m_progressBar;				///< Pointer to the Progress Bar on the window
	GameWindow *m_percent;

	GameWindow *m_objectiveWin;
	GameWindow *m_objectiveLines[MAX_OBJECTIVE_LINES];
	GameWindow *m_unitDesc[MAX_DISPLAYED_UNITS];
	GameWindow *m_location;

	Int m_currentObjectiveLine;
	Int m_currentObjectiveLineCharacter;
	Int m_currentObjectiveWidthOffset;
	Bool m_finishedObjectiveText;

	UnicodeString m_unicodeObjectiveLines[MAX_OBJECTIVE_LINES];

	VideoBuffer *m_videoBuffer;
	VideoStreamInterface *m_videoStream;

	void moveWindows( Int frame );

	AudioEventRTS m_ambientLoop;
	AudioHandle m_ambientLoopHandle;
};



///////////////////////////////////////////////////////////////////////////////////////////////////
// class ChallengeLoadScreen is to be used only when we're loading a Generals' Challenge mission
///////////////////////////////////////////////////////////////////////////////////////////////////
class ChallengeLoadScreen : public LoadScreen
{
public:
	ChallengeLoadScreen();
	virtual ~ChallengeLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;		///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;		 ///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override
	{
		DEBUG_CRASH(("We Got to a single player load screen throw the Network..."));
	}

	virtual void setProgressRange( Int min, Int max ) override;

private:
	GameWindow *m_progressBar;				///< Pointer to the Progress Bar on the window

	VideoBuffer *m_videoBuffer;
	VideoStreamInterface *m_videoStream;

	WindowVideoManager *m_wndVideoManager;

	AudioEventRTS m_ambientLoop;
	AudioHandle m_ambientLoopHandle;

	GameWindow *m_bioNameLeft;
	GameWindow *m_bioAgeLeft;
	GameWindow *m_bioBirthplaceLeft;
	GameWindow *m_bioStrategyLeft;
	GameWindow *m_bioBigNameEntryLeft;
	GameWindow *m_bioNameEntryLeft;
	GameWindow *m_bioAgeEntryLeft;
	GameWindow *m_bioBirthplaceEntryLeft;
	GameWindow *m_bioStrategyEntryLeft;
	GameWindow *m_bioNameRight;
	GameWindow *m_bioAgeRight;
	GameWindow *m_bioBirthplaceRight;
	GameWindow *m_bioStrategyRight;
	GameWindow *m_bioBigNameEntryRight;
	GameWindow *m_bioNameEntryRight;
	GameWindow *m_bioAgeEntryRight;
	GameWindow *m_bioBirthplaceEntryRight;
	GameWindow *m_bioStrategyEntryRight;

	GameWindow *m_portraitLeft;
	GameWindow *m_portraitRight;
	GameWindow *m_portraitMovieLeft;
	GameWindow *m_portraitMovieRight;

//	GameWindow *m_overlayReticleCrosshairs;
	GameWindow *m_overlayReticleCircleLineOuter;
	GameWindow *m_overlayReticleCircleLineInner;
	GameWindow *m_overlayReticleCircleAlphaOuter;
	GameWindow *m_overlayReticleCircleAlphaInner;
	GameWindow *m_overlayVsBackdrop;
	GameWindow *m_overlayVs;

	void activatePieces( Int frame, const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent );
	void activatePiecesMinSpec(const GeneralPersona *generalPlayer, const GeneralPersona *generalOpponent);
};



///////////////////////////////////////////////////////////////////////////////////////////////////
// class ShellGameLoadScreen is to be used for the Shell Game loadscreen
////	///////////////////////////////////////////////////////////////////////////////////////////////
class ShellGameLoadScreen : public LoadScreen
{
public:
	ShellGameLoadScreen();
	virtual ~ShellGameLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;		///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;		 ///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override
	{
		DEBUG_CRASH(("We Got to a single player load screen throw the Network..."));
	}
	virtual void setProgressRange( Int min, Int max ) override { }

private:
	GameWindow *m_progressBar	;				///< Pointer to the Progress Bar on the window

};


///////////////////////////////////////////////////////////////////////////////////////////////////
// class MultiPlayerLoadScreen is to be used for multiplayer communication on the loadscreens
////	///////////////////////////////////////////////////////////////////////////////////////////////
class MultiPlayerLoadScreen : public LoadScreen
{
public:
	MultiPlayerLoadScreen();
	virtual ~MultiPlayerLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;		///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;		 ///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override;
	virtual void setProgressRange( Int min, Int max ) override { }
	virtual void refreshStartSpots( GameInfo *game ) override;
private:
	GameWindow *m_progressBars[MAX_SLOTS];	///< pointer array to all the progress bars on the window
	GameWindow *m_playerNames[MAX_SLOTS];		///< pointer array to all the static text player names on the window
	GameWindow *m_playerSide[MAX_SLOTS];		///< pointer array to all the static text player sides
	Int m_playerLookup[MAX_SLOTS];					///< lookup table to translate network slot info screen slot (to account for holes in the slot list)
	GameWindow *m_mapPreview;
	GameWindow *m_buttonMapStartPosition[MAX_SLOTS];
	GameWindow *m_portraitLocalGeneral;
	GameWindow *m_featuresLocalGeneral;
	GameWindow *m_nameLocalGeneral;

};

///////////////////////////////////////////////////////////////////////////////////////////////////
// class MultiPlayerLoadScreen is to be used for multiplayer communication on the loadscreens
////	///////////////////////////////////////////////////////////////////////////////////////////////
class GameSpyLoadScreen : public LoadScreen
{
public:
	GameSpyLoadScreen();
	virtual ~GameSpyLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;		///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;		 ///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override;
	virtual void setProgressRange( Int min, Int max ) override { }
	virtual void refreshStartSpots( GameInfo *game ) override;
private:
	GameWindow *m_progressBars[MAX_SLOTS];	///< pointer array to all the progress bars on the window
	GameWindow *m_playerNames[MAX_SLOTS];		///< pointer array to all the static text player names on the window
	GameWindow *m_playerSide[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerFavoriteFactions[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerTotalDisconnects[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerWin[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerWinLosses[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerRank[MAX_SLOTS];		///< pointer array to all the static text player sides
	GameWindow *m_playerOfficerMedal[MAX_SLOTS];		///< pointer array to all the static text player munkees
	GameWindow *m_mapPreview;
	GameWindow *m_buttonMapStartPosition[MAX_SLOTS];

	Int m_playerLookup[MAX_SLOTS];					///< lookup table to translate network slot info screen slot (to account for holes in the slot list)

	GameWindow *m_portraitLocalGeneral;
	GameWindow *m_featuresLocalGeneral;
	GameWindow *m_nameLocalGeneral;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// class MapTransferLoadScreen is to be used for map transfers before multiplayer game load screens
////	///////////////////////////////////////////////////////////////////////////////////////////////
class MapTransferLoadScreen : public LoadScreen
{
public:
	MapTransferLoadScreen();
	virtual ~MapTransferLoadScreen() override;

	virtual void init( GameInfo *game ) override;		///< Init the loadscreen
	virtual void reset() override;							///< Reset the system
	virtual void update() override
	{
		DEBUG_CRASH(("Call update(Int) instead.  This update isn't supported"));
	};
	virtual void update(Int percent) override;				///< Update the state of the progress bar
	virtual void processProgress(Int playerId, Int percentage) override
	{
		DEBUG_CRASH(("Call processProgress(Int, Int, AsciiString) instead."));
	}
	void processProgress(Int playerId, Int percentage, AsciiString stateStr);
	virtual void setProgressRange( Int min, Int max ) override { }
	void processTimeout(Int secondsLeft);
	void setCurrentFilename(AsciiString filename);
private:
	GameWindow *m_progressBars[MAX_SLOTS];	///< pointer array to all the progress bars on the window
	GameWindow *m_playerNames[MAX_SLOTS];		///< pointer array to all the static text player names on the window
	GameWindow *m_progressText[MAX_SLOTS];	///< pointer array to all the static text player sides
	Int m_playerLookup[MAX_SLOTS];					///< lookup table to translate network slot info screen slot (to account for holes in the slot list)
	Int m_oldProgress[MAX_SLOTS];						///< old vals, so we can call processProgress() every frame and not touch the GUI
	GameWindow *m_fileNameText;
	GameWindow *m_timeoutText;
	Int m_oldTimeout;												///< old val, so we can call processTimeout() every frame and not touch the GUI
	Bool m_gatedSceneRender;								///< init() set m_loadScreenRender, so the destructor restores it
	Bool m_prevLoadScreenRender;						///< the m_loadScreenRender value init() found
	UnsignedInt m_drawViewsAtStart;					///< Display::getDrawViewsCount() when the screen came up
};
