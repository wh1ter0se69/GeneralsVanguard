/*
**	Command & Conquer Generals(tm)
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

// FILE: GameEngine.h /////////////////////////////////////////////////////////
// The Game engine interface
// Author: Michael S. Booth, April 2001

#pragma once

#include "Common/SubsystemInterface.h"
#include "Common/GameType.h"

// forward declarations
class AudioManager;
class GameLogic;
class GameClient;
class MessageStream;															///< @todo Create a MessageStreamInterface abstract class
class FileSystem;
class Keyboard;
class LocalFileSystem;
class ArchiveFileSystem;
class FileSystem;
class Mouse;
class NetworkInterface;
class ModuleFactory;
class ThingFactory;
class FunctionLexicon;
class Radar;
class WebBrowser;
class ParticleSystemManager;

class GameEngine : public SubsystemInterface
{
public:

	GameEngine();
	virtual ~GameEngine() override;

	virtual void init() override;								///< Init engine by creating client and logic
	virtual void reset() override;								///< reset system to starting state
	virtual void update() override;							///< per frame update

	virtual void execute();											/**< The "main loop" of the game engine.
																								 It will not return until the game exits. */

	static Bool isTimeFrozen(); ///< Returns true if a script has frozen time.
	static Bool isGameHalted(); ///< Returns true if the game is paused or the network is stalling.

	virtual void setQuitting( Bool quitting );				///< set quitting status
	virtual Bool getQuitting();						///< is app getting ready to quit.

	virtual Bool isMultiplayerSession();
	virtual void serviceWindowsOS() {};		///< service the native OS
	virtual Bool isActive() {return m_isActive;}	///< returns whether app has OS focus.
	virtual void setIsActive(Bool isActive) { m_isActive = isActive; };

	/// GeneralsX @feature Claude 27/09/2026 The refresh rate, in Hz, of the display the game window is on;
	/// 0 when the device layer cannot tell (headless, a tool). The default max-FPS cap when the launcher
	/// sets none - see Common/FpsLimitPolicy.h. Answered by SDL3GameEngine and Win32GameEngine.
	virtual Int getDisplayRefreshRate() { return 0; }
	virtual void checkAbnormalQuitting();	///< check if user is quitting at an unusual time - as in cheating!

	/// GeneralsX @bugfix Claude 27/09/2026 One network + logic pass, no client, no draw - FramePacer::update's
	/// between-draws poll in a network game. FALSE when there is no network game to poll.
	static Bool updateBetweenDraws();

protected:

	virtual void resetSubsystems();

	Bool canUpdateGameLogic(UnsignedInt logicTimeQueryFlags);
	Bool canUpdateNetworkGameLogic();
	Bool canUpdateRegularGameLogic(UnsignedInt logicTimeQueryFlags);
	void updateLogic(); ///< update()'s logic half: the tick, when canUpdateGameLogic allows one

	virtual FileSystem *createFileSystem();								///< Factory for FileSystem classes
	virtual LocalFileSystem *createLocalFileSystem() = 0;	///< Factory for LocalFileSystem classes
	virtual ArchiveFileSystem *createArchiveFileSystem() = 0;	///< Factory for ArchiveFileSystem classes
	virtual GameLogic *createGameLogic() = 0;							///< Factory for GameLogic classes.
	virtual GameClient *createGameClient() = 0;						///< Factory for GameClient classes.
	virtual MessageStream *createMessageStream();					///< Factory for the message stream
	virtual ModuleFactory *createModuleFactory() = 0;			///< Factory for modules
	virtual ThingFactory *createThingFactory() = 0;				///< Factory for the thing factory
	virtual FunctionLexicon *createFunctionLexicon() = 0;	///< Factory for Function Lexicon
	virtual Radar *createRadar(Bool dummy) = 0;						///< Factory for radar
	virtual WebBrowser *createWebBrowser() = 0;						///< Factory for embedded browser
	virtual ParticleSystemManager* createParticleSystemManager(Bool dummy) = 0;
	virtual AudioManager *createAudioManager(Bool dummy) = 0;				///< Factory for Audio Manager

	Real m_logicTimeAccumulator; ///< Frame time accumulated towards submitting a new logic frame

	Bool m_quitting; ///< true when we need to quit the game
	Bool m_isActive; ///< app has OS focus.
	UnsignedInt m_frameAtClientUpdate; ///< GeneralsX: the logic frame at the last GameClient::UPDATE (updateBetweenDraws)
};

inline void GameEngine::setQuitting( Bool quitting ) { m_quitting = quitting; }
inline Bool GameEngine::getQuitting() { return m_quitting; }

// the game engine singleton
extern GameEngine *TheGameEngine;

/// This function creates a new game engine instance, and is device specific
extern GameEngine *CreateGameEngine();

/// The entry point for the game system
extern Int GameMain();
