/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX: Discord login, in the engine, on every platform.
//
// ================================================================================================
// THE FLOW, WHICH IS ONE CLICK
// ================================================================================================
//
// The player presses Online while signed out and is asked whether to sign in. If they say yes:
// this module opens a single-shot listener on 127.0.0.1, opens their browser at the login service,
// the service sends them to Discord's consent screen, they accept, and the service POSTs the
// signed assertion back to that listener. Nothing is typed and nothing is polled.
//
// WHY THE ANSWER COMES BACK TO A LOCAL SOCKET is the entire security argument and it lives in
// GXLoopback.h. In one line: the result is delivered to whichever machine's browser did the
// consenting, so a login link mailed to somebody else logs THEM in on THEIR machine and hands the
// sender nothing.
//
// ================================================================================================
// THIS NOW WORKS ON WINDOWS, WHICH IS THE POINT
// ================================================================================================
//
// The previous revision could not exist on Windows: it fetched over HTTPS, and vcpkg.json gates
// curl to "!windows" while that box has no CA store either. Login had to be routed through the
// launcher.
//
// This revision needs NO HTTP CLIENT AT ALL. The browser does every network request; the game
// opens a socket, opens a URL, and verifies a signature. All three exist on all four platforms, so
// this file is compiled unconditionally and the launcher's --login path is redundant.
//
// There is no worker thread, no curl handle and no generation counter any more. The previous
// revision needed all three because it polled a remote service from a detached thread and a late
// reply from an abandoned attempt could overwrite a newer one. With a socket that is closed on
// cancel, an abandoned attempt has nowhere to deliver to, and the whole concurrency argument
// disappears. What replaces it is a single non-blocking accept per frame.
//
// LAN AND OFFLINE PLAY NEVER REQUIRE ANY OF THIS. An absent assertion is a normal, permanent state
// and every failure below lands in "play unauthenticated".

#ifndef GX_LOGIN_FLOW_H
#define GX_LOGIN_FLOW_H

#include "Common/GameCommon.h"
#include "GameClient/Color.h"
#include "Common/UnicodeString.h"

enum GXLoginState
{
	GX_LOGIN_IDLE = 0,	///< nothing in progress. Also the state after a result has been consumed
	GX_LOGIN_WAITING,	///< the browser is open and the listener is armed
	GX_LOGIN_OK,		///< verified and written; GXPlayerIdentity has been refreshed
	GX_LOGIN_FAILED,	///< the service refused, or what came back did not verify
	GX_LOGIN_TIMEOUT,	///< the player never finished. Not an error, and not logged as one
	GX_LOGIN_CANCELLED
};

namespace GXLoginFlow
{
	/// Arm the listener and open the browser. FALSE if a login is already in progress, if no trust
	/// anchor is compiled in, or if the socket could not be bound - which fails closed, with no
	/// fallback port.
	Bool start(void);

	/// Abandon the attempt: closes the listener and wipes the nonce. Safe when idle. Also called
	/// at teardown, because a listening socket must not outlive the engine.
	void cancel(void);

	/// Main-thread tick. One non-blocking accept; on a delivery, verifies the assertion, writes it
	/// and refreshes GXPlayerIdentity. Cheap and safe to call every frame.
	void update(void);

	GXLoginState state(void);

	/// Take one queued UI line, if any. Queued rather than pushed because this module sits below
	/// the GUI and a login outlives the screen that started it.
	Bool takeMessage(UnicodeString *line, Color *color);
}

#endif	// GX_LOGIN_FLOW_H
