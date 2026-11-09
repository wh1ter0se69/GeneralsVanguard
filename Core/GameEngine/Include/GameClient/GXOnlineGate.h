/*
**	GeneralsX @feature Claude 11/08/2026 - who may open the Online lobby.
**
**	WHY THIS IS A HEADER OF ITS OWN, AND PURE.
**
**	The Online gate is decided in two places that must never disagree: the ONLINE button, and the
**	navigation that runs after a login finishes. They drifted apart the first time this was written
**	and the result was a bypass - the button refused an unauthenticated player, and then the
**	post-login path let the same player straight in the moment their login FAILED, on the grounds
**	that "being unable to sign in is not a reason to be unable to play". Press Online, answer Yes,
**	cancel in the browser, and you were in. Found by cross-model review, not by us.
**
**	So the decision lives here, once, as a function of the two inputs it actually depends on, and
**	both call sites ask it. It takes no engine types on purpose: the whole point is that
**	scripts/test/online-gate-test.cpp can drive every state with
**	`c++ -std=c++17 -I Core/GameEngine/Include` and no engine, no window, no network - the class of
**	bug above is invisible to the Worker's 109 checks and has to be testable somewhere.
**
**	The enum below MIRRORS GXLoginState. It is duplicated rather than included because
**	GameClient/GXLoginFlow.h pulls in UnicodeString and Color and would drag the engine into a
**	standalone test. MainMenu.cpp static_asserts the two against each other, so a value added or
**	reordered in one is a COMPILE ERROR rather than a silent mismatch.
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
