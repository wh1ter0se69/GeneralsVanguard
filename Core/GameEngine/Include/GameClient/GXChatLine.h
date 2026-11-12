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

// The decisions a chat line makes, engine-free so
// scripts/test/chat-line-test.cpp links it alone. They live here, not in GXLobbyCommands.cpp (which needs the
// whole GUI to compile), so renderers cannot disagree untested. Under GameClient/, so outside the sim digest: a
// colour change costs no four-platform rebuild.

#pragma once

#ifndef __GXCHATLINE_H_
#define __GXCHATLINE_H_

// ------------------------------------------------------------------------------------------------
// 1. THE SEPARATOR
//
// Every chat renderer used to write its own `L"%ls %ls"` - a SPACE between the speaker and their
// words. For an ordinary player the decoration hid it ("[Bob] hello" reads as though the bracket
// were the separator); for a role there was no separator at all, because an admin's decoration is a
// bare "@Name" (GXLobbyDecorateName). An admin's line rendered "13:13 @Admin hello".
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

// 2. THE ROLE COLOUR matches the admin console's one role entry: .badge[data-tone="admin"] uses var(--accent)
// #D62B44 (tools/admin/panel/public/panel.css). Sanctions are not on this wire.
// The console has NO moderator colour, so 'm' returns GX_WEB_TONE_NONE and the game keeps its green.
// THE ROLE COMES FROM THE RELAY ONLY (GXMEMB's one-letter code): anything but exactly "a" or "m" is a player.

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

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature 3. THE TIME STAMP
//
// Local wall-clock time in front of every said line, in the reader's own clock format: "21:16 " for a
// 24-hour locale, "9:16PM " for a 12-hour one. No seconds, no space before AM/PM, no leading zero on a
// 12-hour hour: the stamp costs width on every row of a pane that word-wraps, so it stays short.

enum { GX_CHAT_CLOCK_MAX = 16 };

/// Write the stamp, trailing space included, into `out`. Out-of-range input reads as 00.
inline void gxChatClockText( int hour, int minute, bool twelveHour, char out[GX_CHAT_CLOCK_MAX] )
{
	if( hour < 0 || hour > 23 )
		hour = 0;
	if( minute < 0 || minute > 59 )
		minute = 0;

	int n = 0;
	if( twelveHour )
	{
		const int h = ( hour % 12 == 0 ) ? 12 : hour % 12;
		if( h >= 10 )
			out[n++] = (char)( '0' + h / 10 );
		out[n++] = (char)( '0' + h % 10 );
	}
	else
	{
		out[n++] = (char)( '0' + hour / 10 );
		out[n++] = (char)( '0' + hour % 10 );
	}
	out[n++] = ':';
	out[n++] = (char)( '0' + minute / 10 );
	out[n++] = (char)( '0' + minute % 10 );
	if( twelveHour )
	{
		out[n++] = ( hour < 12 ) ? 'A' : 'P';
		out[n++] = 'M';
	}
	out[n++] = ' ';
	out[n] = '\0';
}

/// TRUE when a locale's short time pattern is a 12-hour clock.
///
/// Reads the two pattern languages the platforms hand back. A pattern with a '%' is POSIX strftime
/// (Linux nl_langinfo T_FMT): 12-hour when it uses %I, %l or %r. Anything else is an ICU / Windows
/// pattern (CFDateFormatter, LOCALE_STIMEFORMAT): 12-hour when an hour field is 'h' or 'K', 24-hour
/// for 'H' or 'k'. Text inside single quotes is a literal in both of those - fr_CA writes "HH 'h' mm" -
/// and is skipped. An empty or unreadable pattern is 24-hour.
inline bool gxChatTwelveHourFromPattern( const char *pattern )
{
	if( pattern == 0 )
		return false;

	bool posix = false;
	for( const char *p = pattern; *p != '\0'; ++p )
		if( *p == '%' )
			posix = true;

	if( posix )
	{
		for( const char *p = pattern; *p != '\0'; ++p )
		{
			if( *p != '%' )
				continue;
			++p;
			while( *p == 'E' || *p == 'O' || *p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#' )
				++p;							// glibc modifiers and flags
			if( *p == 'I' || *p == 'l' || *p == 'r' )
				return true;
			if( *p == '\0' )
				break;
		}
		return false;
	}

	bool quoted = false;
	for( const char *p = pattern; *p != '\0'; ++p )
	{
		if( *p == '\'' )
		{
			quoted = !quoted;
			continue;
		}
		if( quoted )
			continue;
		if( *p == 'h' || *p == 'K' )
			return true;
		if( *p == 'H' || *p == 'k' )
			return false;
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
// 4. WHO SAID IT, IN A RELAY ROOM
//
// A game room's roster is keyed by room slot: the last octet of the virtual address the relay gave
// each member (Transport.cpp captureRelayRoster). A chat line's sender address is the one the relay
// stamped on the datagram, so it maps to a slot - but only when it is in OUR room's /24. Anything else
// is a plain-LAN address or a stale one, and reads as no slot, which reads as no role.

/// The room slot of `vip`, or 0 when it is not an address in the room `localVip` belongs to.
inline unsigned int gxChatRosterSlotFor( unsigned int vip, unsigned int localVip )
{
	if( vip == 0 || localVip == 0 || ( vip & 0xFFFFFF00u ) != ( localVip & 0xFFFFFF00u ) )
		return 0;
	return vip & 0xFFu;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature 5. ONE NAME SHAPE, AND WHAT A NAME CANNOT START WITH
//
// Every chat surface names its speaker the same way: "[Name]" for a player, "[@Name]" for an admin,
// "[+Name]" for a moderator. The mark inside the bracket comes only from the relay's role for the
// slot the relay stamped on the line. A player's name is the sender's own claim in a LAN room, so the
// role marks are dropped from the front of any name before it is drawn: "[@x]" can never come from a
// name. Honest clients cannot set such a name (the relay nick charset is [A-Za-z0-9_.-] and the
// nickname paths refuse '@'), so only a forged one changes.

/// TRUE for a character that marks a role at the start of a bracketed name.
inline bool gxChatIsRoleMarker( unsigned int c )
{
	return c == (unsigned int)'@' || c == (unsigned int)'+' || c == (unsigned int)'<';
}

/// Index of the first character of `name` that is not a role marker; the name's length when all are.
template <class CharT>
inline int gxChatNameStart( const CharT *name )
{
	int i = 0;
	if( name == 0 )
		return 0;
	while( name[i] != 0 && gxChatIsRoleMarker( (unsigned int)name[i] ) )
		++i;
	return i;
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature 6. THE CHAT COLOURS, THE SAME ON EVERY CHAT SURFACE
//
// Admin crimson, moderator purple, every ordinary player white, the time and notices a lighter gray.
// The admin crimson is the console's hue (GX_WEB_TONE_ADMIN) lifted until it reads on the dark chat
// box: #D62B44 measures 3.5-3.7:1 there, #EB5068 4.8-5.0:1. Every tone here clears 4.5:1 against
// the box (scripts/test/chat-line-test.cpp case 14).
enum
{
	GX_CHAT_TONE_ADMIN  = 0x00EB5068u,
	GX_CHAT_TONE_MOD    = 0x00B982FFu,
	GX_CHAT_TONE_PLAYER = 0x00FFFFFFu,
	GX_CHAT_TONE_MUTED  = 0x00AFAFAFu	///< the time column and notices
};

#endif // __GXCHATLINE_H_
