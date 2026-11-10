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

// GeneralsX: The one table every lobby chat box agrees on.
//
// WHAT THIS OWNS. /help, /clear, the "that verb is gone, use this one" answers, and the guard that
// stops a mistyped command being said out loud to the room. It does NOT own any command that
// actually does something - those stay on the screens that have the state to do them.
//
// WHY IT IS SHARED RATHER THAN PER SCREEN. Three things read the command set: /help renders it,
// the retired table is matched against it, and the unknown-command guard decides from it whether a
// verb may pass through. Three readers of one table cannot drift. Three hand-maintained lists
// always do - which is exactly how /lobbyhelp came to describe four commands out of the sixteen
// that existed, and how the entry banner came to advertise a /friend syntax nobody had typed in
// weeks.
//
// WHY IT IS NOT IN LANAPI. LANAPI.cpp is under Source/GameNetwork, which
// simsourcedigest_watcher.cmake hashes into sourceID. A /help string there would move the digest
// and force an atomic four-platform rebuild for a line of help text - and any peer that did not
// rebuild would be refused at join with a message about incompatible builds. Everything here is
// presentation, GameClient is outside the digest, so this ships on one platform at a time if it
// has to. That is also why the retired verbs LANAPI still recognises (/pass, /nopassword,
// /lobbyhelp) are intercepted UPSTREAM of it rather than deleted from it.

#pragma once

#ifndef __GXLOBBYCOMMANDS_H_
#define __GXLOBBYCOMMANDS_H_

#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/Color.h"
// GeneralsX: GXAssertRole. Both headers are under GameClient/ and outside
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
///
/// `line` is rewritten in place for the `//` escape: a leading double slash becomes a single one
/// and the line goes out literally. `outEscaped` says whether that happened.
///
/// OUT-PARAM RATHER THAN "JUST COPY THE ASCII BACK", and this is a correctness requirement rather
/// than a style choice. The LAN screens hold the player's line as a UnicodeString and send it as
/// one - LANAPI::RequestChat puts the wide characters straight on the wire - so a caller that
/// unconditionally did `txtInput.translate(ascii)` would round-trip every ordinary chat line
/// through AsciiString, and AsciiString::translate casts each WideChar to char (its own @todo says
/// "will only work for 7-bit ascii"). Every accented, Cyrillic, Arabic or CJK line typed on a LAN
/// screen would arrive mangled. So the caller narrows its own copy ONLY when this says the escape
/// really fired, which is the one case where the two strings must diverge.
extern Bool GXLobbyGuardUnknownCommand( AsciiString &line, GXLobbyScreen screen,
										GXLobbySayFunc say, Bool &outEscaped );

/// GeneralsX: Recognise "/nick" on the WIDE line and hand back the
/// remainder. A name is characters, not bytes: every screen tokenises the ASCII translation for
/// dispatch, but AsciiString::translate casts each WideChar to char, so a code point with a zero
/// low byte truncates the narrow copy and an offset computed there cannot be mapped back onto the
/// wide string. The verb is therefore matched here on the wide characters (ASCII-only case fold -
/// the verb itself is seven-bit by construction), and the remainder never goes near the ascii
/// copy. Exactly "/nick": "/nickname" is a different, retired verb and does not match.
extern Bool GXLobbyTakeNickRest( const UnicodeString &line, UnicodeString &outRest );

/// The one-line entry banner, and what /clear reprints so a cleared box is never a dead end.
extern void GXLobbyPrintHelpHint( GXLobbyScreen screen, GXLobbySayFunc say );

// GeneralsX: Role presentation.
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

/// "[Name]", "<@Name>" or "@Name". The wrapper IS the role marker, which is why '@' is refused in
/// every path that assigns a name - see GXApplyNickname.
extern UnicodeString GXLobbyDecorateName( const UnicodeString &name, GXAssertRole role );

// GeneralsX: THE SEPARATOR,
// IN ONE PLACE.
//
// Every chat renderer in the tree had its own `L"%ls %ls"` - the lobby's incoming line, the lobby's
// own echo, the lobby's operator line, the in-match player line, the in-match operator line - and
// every one of them separated the speaker from their words with a SPACE. For an ordinary player the
// decoration hid it: "[Bob] hello" reads as though the bracket were the separator. For a role it
// does not exist at all, because GXLobbyDecorateName drops the brackets for an admin: the owner's
// own line rendered "13:13 @wh1ter0se hello", which reads as a sentence beginning with a name rather
// than as somebody speaking.
//
// So the separator stops being five copies of a format string. Every renderer comes through here,
// and a sixth renderer added later gets it for free - which is the whole reason it is a function and
// not five edits.
//
// THE SPEAKER IS ALREADY DECORATED when it arrives: "[Bob]", "<@Mod>", "@Admin". Those wrappers are
// the ROLE marker (see GXLobbyDecorateName) and say nothing about where the name ends, which is why
// the colon is added to all three the same way.
//
// NO DOUBLE SEPARATOR. A name that already ends in ':' takes no second one. That is not theoretical:
// the lobby's nick charset excludes ':' (GXNickname.h), but the in-match speaker is the LAN slot
// name, which is the player's own game name and is filtered by nothing of the sort - so "Bob:" is
// reachable there and "Bob:: hi" is what the naive concat would print.
//
// AN EMPTY SPEAKER GETS NO COLON AT ALL, and returns the body untouched. A relay notice ("[L]13:13
// P3 has changed their name") names nobody, so an orphan ": " in front of it would be worse than
// the space it replaced.
extern UnicodeString GXLobbySaid( const UnicodeString &speaker, const UnicodeString &body );

/// "@Admin", or "<@Mod>" for GX_ROLE_MOD - the name an OPERATOR speaks under: a GXSAY the relay
/// stamped with one of its operator sender tokens (GameNetwork/GXRelaySay.h). The decoration is
/// what makes it unforgeable - '@' is refused in every path that assigns a player a name, so
/// "[Admin]" is somebody's nickname and "@Admin" is only ever this. GeneralsX:
///
/// GeneralsX: IT TAKES THE ROLE. It used to be void and hard-coded to admin,
/// which drew a moderator's line as an admin's. No default argument: the two call
/// sites both know which voice they are drawing, and a default is how the constant got there.
extern UnicodeString GXLobbyOperatorName( GXAssertRole role );

/// The colour for a whole chat ROW spoken by somebody holding this role. Returns `fallback` for
/// GX_ROLE_PLAYER, so an ordinary line keeps exactly the colour its screen already chose - and so
/// this header needs no GameSpy include to name that colour.
extern Color GXLobbyRoleTextColor( GXAssertRole role, Color fallback );

/// The colour for a NAME cell in the player list. Distinct from the row colour on purpose: this
/// engine cannot put two colours on one chat line, so the pair is split across the two surfaces.
/// Returns `fallback` for GX_ROLE_PLAYER, so a caller keeps whatever it already decided.
extern Color GXLobbyRoleNameColor( GXAssertRole role, Color fallback );

/// "HH:MM " - local wall-clock, and it goes on EVERY line.
///
/// Owner decision, 2026-08-11, reversing the earlier [S]-only rule. The cost is real and is width:
/// the pane is 724x160 design pixels of Arial 10, single column, and it WORD-WRAPS rather than
/// truncating (WOLCustomLobby.wnd:ListboxChat carries no ONE_LINE status, so GadgetListBox.cpp:504
/// gives every row a wrap width), so six characters on every line means long lines wrap one word
/// sooner. Nothing is lost, and the stamp is a fixed-width element, so it forms a clean left gutter
/// to scan down.
///
/// LOCAL time, not UTC: a player reads this against the clock on their own wall. No timezone suffix
/// for the same width reason, and because it would say something the reader already knows.
///
/// This is the bare form, used by a line whose speaker is named in the body - player chat, and your
/// own echo. A TAGGED line puts the tag in front of the stamp; see below.
extern UnicodeString GXLobbyTimestamp( void );

// GeneralsX: THE TAG GOES BEFORE THE STAMP, and a run of tagged lines
// prints its prefix once.
//
// Owner decision, 2026-08-13, against the screenshot of a /help listing: twenty rows each opening
// "04:44 [S] " is a wall of gutter with the actual text pushed off to the right, and the tag - the
// thing that says WHO is talking - was buried in the middle of it. Two changes, both here so no
// screen can implement half of them:
//
//   1. "[S]04:44 " rather than "04:44 [S] ". The tag leads because it is the field a reader sorts
//      by; the stamp is the field they look up afterwards.
//   2. A consecutive run sharing one prefix prints it ONCE - see GXLobbyChatRow.
//
// The full tag set is exactly two, and both live here so they cannot drift apart. An untagged row
// (player chat) keeps the bare stamp: its speaker is already named in the body, so a blank tag
// column would spend width on every ordinary line to align a field it does not have.

/// "[S]HH:MM " - a line this client produced, for this player alone. Nobody else sees it.
extern UnicodeString GXLobbySystemPrefix( void );

/// "[L]HH:MM " - a line the room produced: a name change, somebody being refused.
extern UnicodeString GXLobbyRoomPrefix( void );

/// Compose one chat row, printing `prefix` only when it is not a repeat of the row directly above.
///
/// Returns the string to insert; call it IMMEDIATELY before adding the result to `listbox` as
/// exactly ONE new entry. Pass an EMPTY prefix for a row that must never be folded into the block
/// above - that both prints the row bare and closes any open block.
///
/// EVERY writer to a lobby chat listbox must come through here. A writer that bypasses it drops an
/// unrelated row between a block's header and its continuations, and the continuations then read as
/// belonging to that row instead. There is a guard for exactly that (the entry count is checked
/// against what this function last saw), so a missed writer degrades to a repeated prefix rather
/// than to a misattributed line - but the guard is a safety net, not the design.
///
/// THE GROUPING KEY IS THE RENDERED PREFIX ITSELF, which is tag plus stamp, and that is the whole
/// argument for correctness: a block can only ever cover lines that would have printed the same
/// stamp, so the one stamp shown is true for every line under it. Nothing is suppressed across a
/// boundary that would have changed it. It also needs no threshold and no second clock, and if the
/// stamp is ever given seconds the grouping tightens to per-second with no other edit.
///
/// GROUPING IS PER-ROW, NOT ONE MERGED ENTRY WITH '\n' IN IT, and that is deliberate. The renderer
/// does honour an embedded newline (render2dsentence.cpp:1079), but a listbox row carries exactly
/// ONE colour (ListEntryCell), so merging /help into one entry would flatten its group headings
/// into the body text - and the wheel and the up/down buttons scroll by whole ROWS
/// (GadgetListBox.cpp:248), so a merged block taller than the 160px pane could only be read by
/// dragging the scrollbar. Separate rows keep both, and keep this out of the listbox internals.
extern UnicodeString GXLobbyChatRow( GameWindow *listbox, const UnicodeString &prefix,
									 const UnicodeString &body );

/// Forget any open block. For a screen coming up or going down, so a block cannot span a visit.
extern void GXLobbyChatRowReset( void );

// GeneralsX: SIGN-OUT, ONE SEQUENCE FOR EVERY SURFACE.
//
// The `/logout` chat verb and the lobby's right-click "Sign Out" both come through here, so the
// steps below cannot drift between them - the same reason GXRelayRunSocialCommand is one seam.
//
//   1. GXLoginFlow::cancel() - a login still waiting on the browser would otherwise sign the player
//      straight back in the moment it was approved.
//   2. The saved sign-in is deleted (GXLogin.txt, and any interrupted write's .tmp) and the claims
//      and the raw assertion are dropped from memory - GXPlayerIdentity::clearVerifiedIdentity().
//   3. The TRANSPORT's copy is cleared too. It holds the assertion to re-send every 30 s, and it
//      outlives every screen; left alone it went on declaring the account to the relay after the
//      player had signed out. When a relay room is joined, clearing it sends the relay the
//      authenticated clear, so the relay forgets the account now rather than at member timeout.
//   4. GeneralsX: The LOBBY PRESENCE socket's copy too
//      (GXLobbyPresence::clearIdentity). That socket declares the identity so that mute and ban
//      reach lobby chat, it lives for the whole process, and its membership never lapses while the
//      game runs - so its copy is cleared whole and the relay is sent the same authenticated clear.
//
// It does NOT revoke anything on the server: the assertion the Worker minted stays valid until it
// expires. It removes the sign-in from THIS computer, which is what a player can actually control.
//
// IT ALWAYS CLEANS UP, signed in or not (cross-model review, 18/09/2026). An expired or unreadable
// GXLogin.txt, a crashed login's .tmp and a login still waiting on the browser are all things a
// player asking to be signed out wants gone, and none of them makes hasVerifiedIdentity() true - so
// gating the cleanup on it left exactly those behind. The result still says whether there WAS a
// verified sign-in, because the words the player is shown depend on it.
enum GXSignOutResult
{
	GX_SIGNOUT_NOT_SIGNED_IN = 0,	///< there was no verified sign-in; leftovers were cleaned up anyway
	GX_SIGNOUT_DONE,				///< the saved sign-in is gone from disk and from memory
	GX_SIGNOUT_FILE_SURVIVED,		///< signed out for this session, but GXLogin.txt could not be deleted
	GX_SIGNOUT_COPY_SURVIVED		///< signed out, but a leftover GXLogin.txt.tmp could not be deleted
};
extern GXSignOutResult GXLobbySignOut( void );

#endif // __GXLOBBYCOMMANDS_H_
