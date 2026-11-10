/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// GeneralsX: The two decisions a chat line makes, engine-free.
//
// Owner, 20/09/2026, two reports on the same rendered row:
//   "@wh1ter0se: hello the ':' is missing"
//   "make the colors apply for text chat same as on website"
//
// Both answers live here rather than inside GXLobbyCommands.cpp because that file needs the whole
// GUI to compile and nothing in it can be exercised from a standalone harness - which is how five
// renderers came to disagree about the separator without a single test noticing. What is here is
// plain C++ over plain integers, so scripts/test/chat-line-test.cpp links it on its own.
//
// GameClient/, so it is OUTSIDE the sim digest (simsourcedigest_watcher.cmake) and a colour change
// does not cost a four-platform rebuild. That is the same reason GXLobbyCommands.h gives at length.

#pragma once

#ifndef __GXCHATLINE_H_
#define __GXCHATLINE_H_

// ------------------------------------------------------------------------------------------------
// 1. THE SEPARATOR
//
// Every chat renderer used to write its own `L"%ls %ls"` - a SPACE between the speaker and their
// words. For an ordinary player the decoration hid it ("[Bob] hello" reads as though the bracket
// were the separator); for a role there was no separator at all, because an admin's decoration is a
// bare "@Name" (GXLobbyDecorateName). The owner's own line rendered "13:13 @wh1ter0se hello".
//
// NO DOUBLE SEPARATOR. A speaker whose last character is already the separator takes no second one.
// That is reachable rather than theoretical: the lobby nick charset excludes ':' (GXNickname.h),
// but the IN-MATCH speaker is the LAN slot name - the player's own game name, filtered by nothing of
// the sort - so "Bob:" is a name somebody can hold and "Bob:: hi" is what a naive concat prints.

enum { GX_CHAT_SEPARATOR = ':' };

/// TRUE when a speaker ending in `lastChar` still needs the separator appended.
///
/// Takes the character rather than the string so it is the same answer for a narrow name, a
/// UnicodeString's WideChar and a UTF-32 code point, none of which share a type in this engine.
inline bool gxChatNeedsSeparator( unsigned int lastChar )
{
	return lastChar != (unsigned int)GX_CHAT_SEPARATOR;
}

// ------------------------------------------------------------------------------------------------
// 2. THE ROLE COLOUR, AND WHAT THE WEBSITE ACTUALLY DEFINES
//
// Owner: a role must read the same colour in the game as it does in the admin console. The console's
// whole palette is one :root block in tools/admin/panel/public/panel.css, and exactly ONE entry in it
// is about a role:
//
//     .badge[data-tone="admin"] { background: var(--accent); ... }        --accent: #D62B44
//
// Everything else the console colours - test peer amber, not-signed-in grey, banned struck through,
// muted dimmed - is a SANCTION or an identity label, not a role, and none of them is on this wire:
// GXMEMB carries "<id>|<nick>|<plat>|<role>" and the role field is one character, 'm', 'a' or absent
// (relay.js MEMB_ROLE_MAX, Transport.cpp captureRelayRoster). The client cannot know that somebody is
// muted, banned, a test peer or signed out, so it must not pretend to.
//
// AND THE CONSOLE HAS NO MODERATOR COLOUR AT ALL. Its 'm' role is a console VIEWER rank - a
// moderator gets fewer buttons - and no badge, tone or rule anywhere draws a moderator as a player.
// So there is nothing to match, and this returns GX_WEB_TONE_NONE for 'm' rather than inventing a
// colour and calling it "the website's". The game keeps the green it already had; see the report.
//
// THE ROLE COMES FROM THE RELAY AND ONLY FROM THE RELAY. The argument is the relay's own one-letter
// roster code, and anything that is not exactly "a" or "m" is a player - a client that puts its own
// answer here is putting it in the wrong place, because the roster is keyed by the slot the relay
// stamped on the line (GXRelaySay.h), not by anything the datagram claimed.

/// 0xRRGGBB, no alpha: the console is CSS and the game is RGBA, so the conversion is the caller's.
enum { GX_WEB_TONE_NONE = 0xFFFFFFFFu };	///< the console defines no colour for this

/// panel.css --accent. The one solid crimson in a console list, and the only role it colours.
enum { GX_WEB_TONE_ADMIN = 0x00D62B44u };

/// The console colour for the relay's one-letter roster role code, or GX_WEB_TONE_NONE.
///
/// `code` is the whole field, NUL-terminated. Exactly one character is a role; a longer string is a
/// relay this client does not understand and is a player, which is the same rule
/// GXLobbyRoleFromCode applies and the same rule the assertion verifier applies.
inline unsigned int gxChatWebToneForRoleCode( const char *code )
{
	if( code == 0 || code[0] == '\0' || code[1] != '\0' )
		return (unsigned int)GX_WEB_TONE_NONE;
	if( code[0] == 'a' )
		return (unsigned int)GX_WEB_TONE_ADMIN;
	return (unsigned int)GX_WEB_TONE_NONE;		// 'm' included - the console draws no moderator
}

#endif // __GXCHATLINE_H_
