/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// Discord login, in the engine, on every platform: a single-shot listener on 127.0.0.1, the browser opened at the
// login service, and the service POSTs the signed assertion back to it (the security argument is in GXLoopback.h).
// No HTTP client or worker thread: the game opens a socket and a URL and verifies a signature, so it builds
// everywhere. LAN and offline play never require it; every failure lands in unauthenticated play.

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
