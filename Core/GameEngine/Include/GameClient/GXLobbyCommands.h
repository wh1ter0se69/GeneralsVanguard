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

// The one table every lobby chat box agrees on: /help, /clear, the "that verb is gone" answers, and the guard
// that stops a mistyped command being said to the room. It owns no command that does something; three readers of
// one table cannot drift. Not in LANAPI.cpp (under Source/GameNetwork, hashed into sourceID): the retired verbs
// LANAPI still recognises are intercepted UPSTREAM of it.

#pragma once

#ifndef __GXLOBBYCOMMANDS_H_
#define __GXLOBBYCOMMANDS_H_

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/Color.h"
// GXAssertRole. Both headers are under GameClient/ and outside
// the sim digest, so this include ties nothing new to a four-platform rebuild.
#include "GameClient/GXIdentityAssertion.h"
// GXLobbySaid's separator rule and the console-matched role palette. Engine-free on purpose so a
// standalone harness can exercise both; see the note at the top of it.
#include "GameClient/GXChatLine.h"

class GameWindow;

/// Which chat box the line was typed into.
///
/// This is not decoration. The social commands exist only on the relay lobby screen, so a /help
/// that listed /msg on the LAN lobby would be describing a command that screen genuinely does not
/// have - and the guard would then let it through to be broadcast.
enum GXLobbyScreen
{
	GX_LOBBY_SCREEN_ONLINE = 0,		///< WOLCustomLobby.wnd - the relay browser (Zero Hour only)
	GX_LOBBY_SCREEN_LAN,			///< LanLobbyMenu.wnd - the LAN browser (both trees)
	GX_LOBBY_SCREEN_GAME,			///< LanGameOptionsMenu.wnd - inside a lobby (both trees)
	GX_LOBBY_SCREEN_SCORE,			///< ScoreScreen.wnd - after a LAN match: /help and /clear only

	GX_LOBBY_SCREEN_COUNT
};

/// One line into whichever chat box the caller owns.
///
/// Each screen keeps its own sink because each writes to a different listbox with a different
/// column convention, and because a headless run has no listbox at all - dropping the line there
/// is what made the lobby untestable without a GUI.
typedef void (*GXLobbySayFunc)( const UnicodeString &line, Color color );

/// FIRST pass, before the caller's own dispatcher. Consumes /help, /clear, every retired verb and
/// every reserved-but-unbuilt one. TRUE when the line was dealt with and the caller must stop.
///
/// Must run first, because some retired verbs are still recognised DOWNSTREAM inside
/// LANAPI::applyLobbyCommand, which this branch deliberately does not edit.
extern Bool GXLobbyChatCommand( const AsciiString &line, GXLobbyScreen screen,
								GameWindow *chatListbox, GXLobbySayFunc say );

/// LAST pass, immediately before the line would be sent. TRUE when the caller must NOT send it.
/// `line` is rewritten in place for the `//` escape (a leading double slash becomes one); `outEscaped` says
/// whether it happened. The caller narrows its own copy ONLY then: AsciiString::translate casts each WideChar to
/// char, so round-tripping every line would mangle accented, Cyrillic, Arabic and CJK text on LAN screens.
extern Bool GXLobbyGuardUnknownCommand( AsciiString &line, GXLobbyScreen screen,
										GXLobbySayFunc say, Bool &outEscaped );

/// Recognise "/nick" on the WIDE line and hand back the
/// remainder. A name is characters, not bytes: every screen tokenises the ASCII translation for
/// dispatch, but AsciiString::translate casts each WideChar to char, so a code point with a zero
/// low byte truncates the narrow copy and an offset computed there cannot be mapped back onto the
/// wide string. The verb is therefore matched here on the wide characters (ASCII-only case fold -
/// the verb itself is seven-bit by construction), and the remainder never goes near the ascii
/// copy. Exactly "/nick": "/nickname" is a different, retired verb and does not match.
extern Bool GXLobbyTakeNickRest( const UnicodeString &line, UnicodeString &outRest );

/// The one-line entry banner, and what /clear reprints so a cleared box is never a dead end.
extern void GXLobbyPrintHelpHint( GXLobbyScreen screen, GXLobbySayFunc say );

// Role presentation.
//
// These live in Core, beside the colours, so the lobby screen and the game screen cannot disagree
// about what a moderator looks like. A role that renders one way in the browser and another way
// inside a game is a role nobody learns to recognise.
//
// A ROLE IS A COLOUR AND A DECORATION, NEVER A PERMISSION. Every rule that binds is enforced by the
// relay against the durable sub. See the note on GXAssertRole.

/// The role a roster entry claims, from the relay's one-letter code: "m", "a", "" or NULL.
/// Anything unrecognised is GX_ROLE_PLAYER, matching the assertion verifier's own rule.
extern GXAssertRole GXLobbyRoleFromCode( const char *code );

/// "[Name]", "[+Name]" or "[@Name]" - one shape on every chat surface. The mark inside the bracket is
/// the role; role marks at the front of the name itself are dropped (GXChatLine.h gxChatNameStart).
extern UnicodeString GXLobbyDecorateName( const UnicodeString &name, GXAssertRole role );

// THE SEPARATOR, IN ONE PLACE: every renderer adds the colon to the speaker, who arrives already decorated
// ("[Bob]", "[+Mod]", "[@Admin]"). NO DOUBLE SEPARATOR: a name already ending in ':' takes no second one (the
// in-match speaker is the LAN slot name, which is not filtered). AN EMPTY SPEAKER GETS NO COLON and the body is
// returned untouched (a relay notice names nobody).
extern UnicodeString GXLobbySaid( const UnicodeString &speaker, const UnicodeString &body );

/// "[@Admin]", or "[+Mod]" for GX_ROLE_MOD - the name an OPERATOR speaks under: a GXSAY the relay
/// stamped with one of the admin socket's sender tokens (GameNetwork/GXRelaySay.h). The decoration is
/// what makes it unforgeable - '@' is refused in every path that assigns a player a name, so
/// "[Admin]" is somebody's nickname and "[@Admin]" is only ever this.
///
/// IT TAKES THE ROLE, with no default argument: the two call sites both know which voice they are drawing.
extern UnicodeString GXLobbyOperatorName( GXAssertRole role );

/// "@Admin" / "<@Mod>" - the operator's name INSIDE A MATCH, where player lines are "[<name>]: text" from
/// digested code that does not check names; the missing bracket is what no player line can start with.
extern UnicodeString GXMatchOperatorName( GXAssertRole role );

/// The colour for a whole chat ROW spoken by somebody holding this role. Returns `fallback` for
/// GX_ROLE_PLAYER, so an ordinary line keeps exactly the colour its screen already chose - and so
/// this header needs no GameSpy include to name that colour.
extern Color GXLobbyRoleTextColor( GXAssertRole role, Color fallback );

/// The colour for a NAME cell in the player list. Distinct from the row colour on purpose: this
/// engine cannot put two colours on one chat line, so the pair is split across the two surfaces.
/// Returns `fallback` for GX_ROLE_PLAYER, so a caller keeps whatever it already decided.
extern Color GXLobbyRoleNameColor( GXAssertRole role, Color fallback );

/// "HH:MM " (or "9:16PM " where the OS clock is 12-hour) - local wall-clock, and it goes on EVERY line. The cost
/// is width: the pane word-wraps, so six to eight characters on every line wrap long lines one word sooner.
/// LOCAL time, not UTC, no timezone suffix. The bare form, used by a line whose speaker is named in the body
/// (player chat, your own echo); a TAGGED line puts the tag in front of the stamp, see below.
extern UnicodeString GXLobbyTimestamp( void );

// THE TAG GOES BEFORE THE STAMP, and a run of tagged lines prints its prefix once.
//   1. "[S]04:44 " rather than "04:44 [S] ": the tag leads because it is the field a reader sorts by.
//   2. A consecutive run sharing one prefix prints it ONCE - see GXLobbyChatRow.
// The full tag set is exactly two. An untagged row (player chat) keeps the bare stamp.

/// "[S]HH:MM " - a line this client produced, for this player alone. Nobody else sees it.
extern UnicodeString GXLobbySystemPrefix( void );

/// "[L]HH:MM " - a line the room produced: a name change, somebody being refused.
extern UnicodeString GXLobbyRoomPrefix( void );

/// Compose one chat row, printing `prefix` only when it is not a repeat of the row above. Call it IMMEDIATELY
/// before adding the result to `listbox` as exactly ONE new entry. An EMPTY prefix never folds and closes any
/// open block. EVERY writer to a lobby chat listbox must come through here, or an unrelated row lands inside a
/// block. Per-row, not one entry with '\n': a row has ONE colour.
extern UnicodeString GXLobbyChatRow( GameWindow *listbox, const UnicodeString &prefix,
									 const UnicodeString &body );

/// Forget any open block. For a screen coming up or going down, so a block cannot span a visit.
extern void GXLobbyChatRowReset( void );

// GeneralsX @feature The chat boxes: a gray time column beside the words, the same on every chat screen.
//
/// Give a chat listbox its time column. Call once, when the screen creates the box, before its first
/// row (it empties the box). FALSE leaves it one column: the time then goes inline, in the line's colour.
extern Bool GXChatBoxUseTimeColumn( GameWindow *listbox );
/// One chat row: the time, then `body`. `tag` is empty for somebody talking, "[S]" for a notice this
/// client raised, "[L]" for one the room sent (a run of notices prints its tag and time once).
/// Returns the row index, or -1. EVERY writer to a chat box comes through here.
extern Int GXChatBoxAdd( GameWindow *listbox, const UnicodeString &tag, const UnicodeString &body, Color color );
/// Move rows that another writer added without a column (they land in the time column) into the text
/// column, as "[S]" notices. For code this branch cannot change.
extern void GXChatBoxAdopt( GameWindow *listbox );
/// White - every ordinary player's line, on every chat screen.
extern Color GXChatPlayerColor( void );
/// The lighter gray of the time column and of notices.
extern Color GXChatMutedColor( void );
/// The colour a notice is drawn in: always the notice gray, whatever its caller asked for, so no
/// notice can be mistaken for an admin's crimson.
extern Color GXChatNoticeColor( Color requested );

// SIGN-OUT, ONE SEQUENCE FOR EVERY SURFACE (`/logout` and the right-click "Sign Out"): 1. GXLoginFlow::cancel()
// (a pending login would sign the player back in); 2. delete GXLogin.txt and any .tmp, drop the claims and raw
// assertion; 3. clear the TRANSPORT's copy and send the authenticated clear; 4. the same for the LOBBY PRESENCE
// socket. It does NOT revoke anything on the server. It ALWAYS cleans up, signed in or not.
enum GXSignOutResult
{
	GX_SIGNOUT_NOT_SIGNED_IN = 0,	///< there was no verified sign-in; leftovers were cleaned up anyway
	GX_SIGNOUT_DONE,				///< the saved sign-in is gone from disk and from memory
	GX_SIGNOUT_FILE_SURVIVED,		///< signed out for this session, but GXLogin.txt could not be deleted
	GX_SIGNOUT_COPY_SURVIVED		///< signed out, but a leftover GXLogin.txt.tmp could not be deleted
};
extern GXSignOutResult GXLobbySignOut( void );

#endif // __GXLOBBYCOMMANDS_H_
