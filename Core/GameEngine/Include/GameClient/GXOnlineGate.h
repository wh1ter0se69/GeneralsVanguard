/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

/*
**	Who may open the Online lobby, decided once for two callers that must never disagree: the ONLINE button and the
**	navigation after a login finishes (they once drifted into a bypass when login FAILED). Pure:
**	scripts/test/online-gate-test.cpp. The enum MIRRORS GXLoginState; MainMenu.cpp static_asserts the two.
*/

#pragma once

#ifndef GX_ONLINE_GATE_H
#define GX_ONLINE_GATE_H

namespace GXOnlineGate
{

/// Mirror of GXLoginState. Kept in step by static_assert at the call site.
enum LoginState
{
	LOGIN_IDLE = 0,
	LOGIN_WAITING,
	LOGIN_OK,
	LOGIN_FAILED,
	LOGIN_TIMEOUT,
	LOGIN_CANCELLED
};

enum Decision
{
	DECISION_WAIT,		///< a login is still in flight; do nothing and keep waiting
	DECISION_ENTER,		///< signed in and usable: open the lobby
	DECISION_REFUSE		///< stay on the main menu and say why
};

/*
 * The decision, for the path that runs after a login attempt has been started.
 *
 * ENTER requires BOTH a successful flow AND a usable identity. GX_LOGIN_OK alone is not enough:
 * "the service answered" and "this client now holds an assertion it can prove" are different
 * claims, and an assertion that arrived but did not verify would otherwise open the lobby to
 * somebody the lobby cannot name. Every other state - failed, timed out, cancelled, or an idle
 * flow that never really started - refuses. There is deliberately no "failed, so let them in
 * anyway" arm; that arm WAS the bypass.
 */
inline Decision afterLogin( LoginState state, bool hasVerifiedIdentity )
{
	if (state == LOGIN_WAITING)
		return DECISION_WAIT;

	if (state == LOGIN_OK && hasVerifiedIdentity)
		return DECISION_ENTER;

	return DECISION_REFUSE;
}

/*
 * The decision for the ONLINE button itself.
 *
 * Identity is the only thing that opens the lobby. It deliberately does NOT consult any
 * "previously declined" preference: a refusal that suppressed the gate would be a bypass wearing
 * a preference's clothes.
 */
inline Decision onOnlinePressed( LoginState state, bool hasVerifiedIdentity )
{
	if (hasVerifiedIdentity)
		return DECISION_ENTER;

	// Their browser is already open. Starting a second flow would invalidate the first one's nonce.
	if (state == LOGIN_WAITING)
		return DECISION_WAIT;

	return DECISION_REFUSE;
}

}	// namespace GXOnlineGate

#endif	// GX_ONLINE_GATE_H
