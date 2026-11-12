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

// FILE: W3DGameEngine.cpp ////////////////////////////////////////////////////////////////////////
// Author: Colin Day, April 2001
// Description:
//   Implementation of the Win32 game engine, this is the highest level of
//   the game application, it creates all the devices we will use for the game
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <windows.h>

#include "Win32Device/Common/Win32GameEngine.h"
#include "Common/PerfTimer.h"

#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameClient/GXLobbyPresence.h"	// TheGXLobbyPresence::update
#include "Common/GlobalData.h"	// TheGlobalData->m_lanRole - which launches skip the lobby
#include "GameClient/GXIdentityPush.h"	// gxDeclareIdentityIfChanged - the headless peer's GXIDN
#include "GameClient/GXReplayDiag.h"	// gxReplayTelSuspend / gxReplayTelAway - the replay telemetry's gap and away flag
#include "W3DDevice/GameClient/GxNativeInstall.h"	// GeneralsX: NoteWindowsMessage (PCL ping, Reflex flash)

extern DWORD TheMessageTime;

//-------------------------------------------------------------------------------------------------
/** Constructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::Win32GameEngine()
{
	// Stop blue screen
	m_previousErrorMode = SetErrorMode( SEM_FAILCRITICALERRORS );
}

//-------------------------------------------------------------------------------------------------
/** Destructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::~Win32GameEngine()
{
	// GeneralsX: Close the lobby-presence socket with the engine.
	if (TheGXLobbyPresence != NULL)
	{
		delete TheGXLobbyPresence;
		TheGXLobbyPresence = NULL;
	}

	// restore it (this isn't really necessary, but feels good.)
	SetErrorMode( m_previousErrorMode );
}


//-------------------------------------------------------------------------------------------------
/** Initialize the game engine */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::init()
{

	// GeneralsX: The shared-lobby presence socket. Created HERE, in the
	// device engine, and not in GameEngine::init(): Common/ is inside the sourceID digest and this
	// feature is deliberately digest-neutral so it can ship without an atomic four-platform rebuild.
	// It opens no socket until an Online screen calls enterLobby(), so constructing it always costs
	// nothing on the singleplayer path.
	if (TheGXLobbyPresence == NULL)
	{
		TheGXLobbyPresence = NEW GXLobbyPresence;

		/*
		 * Acquire the relay address on every launch with a relay configured, so a direct match launch files
		 * telemetry too: the panel names no player unless everybody in the room is reporting.
		 * Here in init(), NOT update(): ResolveIP is a blocking gethostbyname, and under lockstep one stalled peer stalls all.
		 */
		TheGXLobbyPresence->acquireRelayAddr();
	}


	// extending functionality
	GameEngine::init();

}

//-------------------------------------------------------------------------------------------------
/** Reset the system */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::reset()
{

	// extending functionality
	GameEngine::reset();

}

//-------------------------------------------------------------------------------------------------
/** Update the game engine by updating the GameClient and
	* GameLogic singletons. */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::update()
{
	// GeneralsX: Pump the Windows message queue FIRST (PP-5). It used to
	// run only at the END of update(), after GameEngine::update() had already drained the mouse
	// and drawn - and execute() sleeps in FramePacer right after that. WM_MOUSEMOVE/button (and
	// WM_CHAR) messages that arrived during that sleep therefore sat unpumped through the whole
	// next iteration and reached Win32Mouse one iteration late: mean +S, worst +1 period. This is
	// the order the SDL3 engine already uses (pollSDL3Events() first in SDL3GameEngine::update).
	// The keyboard is DirectInput, polled in Keyboard::UPDATE, and was never affected.
	serviceWindowsOS();

	// Pump LANAPI from the engine, as SDL3GameEngine.cpp does. Without it ACCEPT and BACK do nothing on Windows:
	// no other live pump survives joining a game, so Transport::update() never runs.
	// It also pumps during a match: LANAPI::update() holds the host's re-advertisement, or the listing ages out.
	// Safe because player timeouts are gated on !isGameInProgress(). Keep identical to the SDL3 pump.
	if (TheLAN != nullptr && TheGameLogic != nullptr)
	{
		TheLAN->update();
	}

	// GeneralsX: Shared-lobby presence. IDENTICAL to the SDL3 engine, and
	// that is the point - see the note above about d19090d14. There are two engine classes, so a
	// pump added to one of them reaches three platforms and silently misses the fourth.
	if (TheGXLobbyPresence != nullptr)
	{
		TheGXLobbyPresence->update();
	}

	// GeneralsX: Declare the SIGNED identity from HERE too. IDENTICAL to the
	// SDL3 engine, and that is the point - see the d19090d14 note above. Every writer of
	// Transport::m_relayIdentity was GUI menu code, so a peer with no menus sent no GXIDN however
	// valid its GXLogin.txt was, and RELAY_REQUIRE_IDENTITY=1 drops it 15 s after it joins. The whole
	// trace, and why it cannot live in HeadlessMatch.cpp or Transport.cpp, is in the header.
	gxDeclareIdentityIfChanged();

	// GeneralsX: The replay telemetry's AWAY flag (U): a network game keeps simulating while the
	// window is minimised (the IsIconic loop below breaks out) and while it has lost focus (Alt+Tab keeps drawing to nobody), and
	// those rows had no flag. isActive() is WinMain's WM_ACTIVATEAPP state, pumped by serviceWindowsOS() above; IsIconic covers a
	// minimise that kept activation. Set BEFORE GameEngine::update() samples, so a solo minimise's last row is U as well as
	// suspended, and the first row after a restore is not U.
	{
		extern HWND ApplicationHWnd;
		gxReplayTelAway(ApplicationHWnd != NULL && (::IsIconic(ApplicationHWnd) || !isActive()));
	}

	// call the engine normal update
	GameEngine::update();

	extern HWND ApplicationHWnd;
	if (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
		// GeneralsX: A solo game stops in the loop below until it is restored: the replay's
		// telemetry closes its row there and shows "suspended N s", never one 12 s frame (GXReplayDiag.h). A network game breaks out
		// after one pass and keeps running, so it is not suspended. Same predicate as the Sleep(5) below.
		if (!(TheGameEngine->getQuitting() || TheGameLogic->isInInternetGame() || TheGameLogic->isInLanGame() || TheNetwork != nullptr))
			gxReplayTelSuspend(true);
		while (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
			// We are alt-tabbed out here.  Sleep a bit, & process windows
			// so that we can become un-alt-tabbed out.
			// GeneralsX: Not in a network match (PP-4). There the loop
			// runs exactly once and breaks below to keep the logic going, so this Sleep(5) was
			// paid on EVERY main-loop iteration of a minimised peer - logic frames included - and
			// capped it at 1/(L+5 ms). Under lockstep that cap is charged to the whole lobby.
			// FramePacer (execute(), after update()) already paces the iteration there: the render
			// cap, floored at 8x the tick rate in a network game. An UNCAPPED peer now polls flat
			// out while minimised, exactly as it does while visible. Same predicate as the break.
			if (!(TheGameEngine->getQuitting() || TheGameLogic->isInInternetGame() || TheGameLogic->isInLanGame() || TheNetwork != nullptr))
				Sleep(5);
			serviceWindowsOS();

			if (TheLAN != nullptr) {
				// BGC - need to update TheLAN so we can process and respond to other
				// people's messages who may not be alt-tabbed out like we are.
				TheLAN->setIsActive(isActive());
				TheLAN->update();
			}

			// GeneralsX: And keep our lobby presence alive while
			// alt-tabbed, for exactly BGC's reason above: this loop can run for minutes, the relay
			// times a membership out after 60 s of silence, and coming back to find yourself gone
			// from the player list - and your own id handed to somebody else - is the same class of
			// bug as going deaf to other people's messages.
			if (TheGXLobbyPresence != nullptr) {
				TheGXLobbyPresence->update();
			}

			// GeneralsX: And re-read a renewed bot login from HERE too.
			// In a menu or lobby this loop runs for as long as the window stays minimised, and the
			// declare above it ran exactly once, before entry: a GX_IDENTITY_FILE the host's timer rewrote
			// meanwhile was never re-read, the two pumps above kept sending the expired assertion, and an
			// enforcing relay dropped the memberships they were keeping alive. Same call, same order as the
			// unminimised path. Not compiled on this Mac; the pin is scripts/test/gxm1-reach-test.sh.
			gxDeclareIdentityIfChanged();

			// GeneralsX: Also keep running once a network session exists (TheNetwork): the start callback creates it BEFORE queueing MSG_NEW_GAME, and
			// isInLanGame() only turns true after that message is dispatched by GameEngine::update(), so a minimised peer sat here and never started. The Sleep(5) predicate above
			// stays identical. Idea from Generals Online a9861ab5c, reimplemented.
			// If we are running a multiplayer game, keep running the logic.
			// There is code in the client to skip client redraw if we are
			// iconic.  jba.
			if (TheGameEngine->getQuitting() || TheGameLogic->isInInternetGame() || TheGameLogic->isInLanGame() || TheNetwork != nullptr) {
				break; // keep running.
			}
		}
		gxReplayTelSuspend(false);	// GeneralsX: the loop runs again (a no-op if it never stopped)

    // When we are alt-tabbed out... the MilesAudioManager seems to go into a coma sometimes
    // and not regain focus properly when we come back. This seems to wake it up nicely.
    AudioAffect aa = (AudioAffect)0x10;
		TheAudio->setVolume(TheAudio->getVolume( aa ), aa );

	}

}

//-------------------------------------------------------------------------------------------------
/** This function may be called from within this application to let
  * Microsoft Windows do its message processing and dispatching.  Presumably
	* we would call this at least once each time around the game loop to keep
	* Windows services from backing up */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::serviceWindowsOS()
{
	MSG msg;
  Int returnValue;

	//
	// see if we have any messages to process, a nullptr window handle tells the
	// OS to look at the main window associated with the calling thread, us!
	//
	while( PeekMessage( &msg, nullptr, 0, 0, PM_NOREMOVE ) )
	{

		// get the message
		returnValue = GetMessage( &msg, nullptr, 0, 0 );

		// this is one possible way to check for quitting conditions as a message
		// of WM_QUIT will cause GetMessage() to return 0
/*
		if( returnValue == 0 )
		{

			setQuitting( true );
			break;

		}
*/

		TheMessageTime = msg.time;
		// GeneralsX: before dispatch - PCL Stats' ping can be a thread message that no window
		// procedure sees. Two atomics in the native device; nothing without Reflex.
		GxNative::NoteWindowsMessage(msg.message);
		// translate and dispatch the message
		TranslateMessage( &msg );
		DispatchMessage( &msg );
		TheMessageTime = 0;

	}

}

//-------------------------------------------------------------------------------------------------
/** GeneralsX: The refresh rate of the display the game window is on, or 0 when
  * Windows cannot say. The max-FPS cap's default when the launcher sets none (Common/FpsLimitPolicy.h).
  * MonitorFromWindow picks the monitor holding most of the window; EnumDisplaySettingsW on that monitor's
  * device reads the mode it runs now. dmDisplayFrequency 0 and 1 both mean "the hardware default", i.e.
  * unknown. Whole Hz: Windows reports a 59.94 Hz mode as 59, which FramePacer's tick-rate floor lifts to
  * 60. Headless has no window to follow. */
//-------------------------------------------------------------------------------------------------
Int Win32GameEngine::getDisplayRefreshRate()
{
	extern HWND ApplicationHWnd;
	if (ApplicationHWnd == nullptr || (TheGlobalData != nullptr && TheGlobalData->m_headless))
		return 0;

	const HMONITOR monitor = ::MonitorFromWindow(ApplicationHWnd, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFOEXW info;
	ZeroMemory(&info, sizeof(info));
	info.cbSize = sizeof(info);
	if (monitor == nullptr || !::GetMonitorInfoW(monitor, (LPMONITORINFO)&info))
		return 0;

	DEVMODEW mode;
	ZeroMemory(&mode, sizeof(mode));
	mode.dmSize = sizeof(mode);
	if (!::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
		return 0;

	return (mode.dmDisplayFrequency > 1) ? (Int)mode.dmDisplayFrequency : 0;
}

