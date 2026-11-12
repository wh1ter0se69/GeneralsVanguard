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
** Pure decisions behind two game-setup conveniences, testable without an engine (scripts/test/lobby-setup-ui-test.sh):
** 1. Slot drop-down letter keys come from each label itself (no table); shared letters cycle, a unique one closes.
** 2. The heading's room name is another player's text: filter control/bidi characters, fit with "...".
*/

#pragma once

#include "Lib/BaseType.h"

/// The key a character answers to: ASCII letters upper-cased, digits as-is, Latin-1 accented
/// letters folded to their base letter. 0 for anything no key can produce (punctuation, controls,
/// letters outside Latin-1). Applied to both the label and the key, so they fold the same way.
inline WideChar ComboKeyFold( WideChar c )
{
	if( c >= L'a' && c <= L'z' )
		return (WideChar)( c - L'a' + L'A' );
	if( ( c >= L'A' && c <= L'Z' ) || ( c >= L'0' && c <= L'9' ) )
		return c;
	if( c >= 0xC0 && c <= 0xFF )
	{
		// 0xC0-0xDF, and 0xE0-0xFF is the same row lower-cased. '.' = no base letter (x, /, thorn).
		static const char base[] = "AAAAAAACEEEEIIIIDNOOOOO.OUUUUY.S";
		const char b = ( c == 0xFF ) ? 'Y' : base[ ( c - 0xC0 ) & 0x1F ];
		return b == '.' ? 0 : (WideChar)b;
	}
	return 0;
}

/// Lower case for ASCII and Latin-1 letters, without the C library: towlower in the "C" locale the game
/// runs in leaves Latin-1 alone, so a shifted "O-umlaut" would not match its key's "o-umlaut".
/// Nothing past Latin-1 is needed: no such letter is a lead (ComboKeyFold).
inline WideChar ComboCaseFold( WideChar c )
{
	if( ( c >= L'A' && c <= L'Z' ) || ( c >= 0xC0 && c <= 0xDE && c != 0xD7 ) )
		return (WideChar)( c + 0x20 );
	return c;
}

/// The letter an entry answers to: its first letter or digit, folded (ComboKeyFold). 0 when none.
inline WideChar ComboEntryLead( const WideChar *text )
{
	if( text == nullptr )
		return 0;
	for( ; *text != 0; ++text )
	{
		const WideChar c = *text;
		const WideChar f = ComboKeyFold( c );
		if( f != 0 )
			return f;
		// Spaces and punctuation (ASCII and Latin-1) are skipped: "  Open" and "(Open)" both answer O.
		if( c < 0xC0 || c == 0xD7 || c == 0xF7 )
			continue;
		// A letter no key can type leads this label: it has no key, rather than answering to
		// whatever letter happens to come second.
		return 0;
	}
	return 0;
}

/// How many entries answer to `key`. More than one = the list stays open so the key can cycle.
inline Int ComboKeyMatchCount( const WideChar *leads, Int count, WideChar key )
{
	Int n = 0;
	if( leads == nullptr || key == 0 )
		return 0;
	for( Int i = 0; i < count; ++i )
		if( leads[ i ] == key )
			++n;
	return n;
}

/// The entry a key picks, or -1. Searches from the entry after `current`, wrapping, so a letter
/// shared by several entries cycles through them. `key` must already be upper-cased.
inline Int ComboKeyPickIndex( const WideChar *leads, Int count, Int current, WideChar key )
{
	if( leads == nullptr || count <= 0 || key == 0 )
		return -1;
	if( current < -1 || current >= count )
		current = -1;
	for( Int step = 1; step <= count; ++step )
	{
		Int i = ( current + step ) % count;
		if( leads[ i ] == key )
			return i;
	}
	return -1;
}

/// Does an open drop-down claim a typed character, so the focused text field must not get it? Only
/// when it names an entry of that list. Any other letter reaches chat exactly as it does with a
/// mouse-opened list - a menu can open the next slot's list on its own, and a list that ate every
/// letter would silently eat the chat line typed while it stayed open.
inline Bool ComboListOwnsChar( const WideChar *leads, Int count, WideChar ch )
{
	return ComboKeyMatchCount( leads, count, ComboKeyFold( ch ) ) > 0;
}

/// What an open drop-down does with a character typed into the focused text field.
enum ComboTextAction
{
	COMBO_TEXT_PASS,	///< not the list's: it goes on to the text field (chat)
	COMBO_TEXT_EAT,		///< the auto-repeat of a pick: kept out of chat, picks nothing
	COMBO_TEXT_PICK		///< picks the entry it names, and is kept out of chat
};

/// The CHARACTER picks, when it arrives - not the key-down, which may come before or after it (Win32's
/// DirectInput can read the key a frame ahead of WM_CHAR; inside one SDL pump pass every character
/// precedes every key-down, so pairing them by arrival handed E's key the H). `owned`: it names an entry
/// (ComboListOwnsChar). `keystroke`: a key typed it - an IME's committed text never picks, whatever it
/// spells. `repeat`: the key was already down, so it never picks again - a held E picks once. Its repeats
/// stay out of chat when that key's first press picked (`heldPicked`), even once the list it picked on has
/// closed (the last slot), and go on to chat when that press was chat.
inline ComboTextAction ComboTextFor( Bool owned, Bool keystroke, Bool repeat, Bool heldPicked )
{
	if( !keystroke )
		return COMBO_TEXT_PASS;
	if( repeat )
		return heldPicked ? COMBO_TEXT_EAT : COMBO_TEXT_PASS;
	return owned ? COMBO_TEXT_PICK : COMBO_TEXT_PASS;
}

/// Is an open drop-down part of the UI that has the keyboard? Each argument is a TOP-LEVEL window, nullptr
/// = none: the list's own, the one on top of the modal stack, and the one the keystroke is going to (the
/// focused window, or the text field its character arrived at). Under a modal - a message box, a popup, an
/// IME candidate list - a list behind it takes no keys: winSetModal does not close it, so it stays visible
/// and enabled while its letters belong to the box in front. Nor does it when the
/// keystroke is going to another layout (an overlay's own chat).
inline Bool ComboListHasKeyboard( const void *listRoot, const void *modalRoot, const void *targetRoot )
{
	if( modalRoot != nullptr && modalRoot != listRoot )
		return FALSE;
	return targetRoot == nullptr || targetRoot == listRoot;
}

/// Does a key-down pick on the KEY path (no text field focused, so no character comes)? Only a fresh
/// press: an auto-repeat never picks, so a held E picks once instead of walking on through the next
/// slot's list its pick opened. The press says so itself (KEY_STATE_AUTOREPEAT: Keyboard::checkKeyRepeat
/// is the only repeat source - DirectInput reports changes only, SDL3Keyboard drops SDL's repeats). Not
/// a record of the keys that picked, cleared by their key-ups: focus loss sends no key-up
/// (Keyboard::resetKeys on KEY_LOST), so that key's next real press was swallowed.
inline Bool ComboKeyPressPicks( Bool autoRepeat )
{
	return !autoRepeat;
}

/// The character a Win32 key types in the ACTIVE layout, from MapVirtualKeyEx( vk, MAPVK_VK_TO_CHAR )
/// (`vkToChar`) - never the key table, which is chosen once, at startup, from the game's language,
/// and which would name a key the layout gives no character by its US letter (
/// the SDL twin). A dead key (top bit set) types nothing by itself, so it names no letter; 0 is no character.
inline WideChar ComboLayoutChar( UnsignedInt vkToChar )
{
	if( vkToChar & 0x80000000u )
		return 0;
	return (WideChar)( vkToChar & 0xFFFF );
}

/// Did a key type this text? For a text path that, unlike Win32's WM_CHAR, does not say (SDL's
/// SDL_EVENT_TEXT_INPUT): only when it is ONE character and it is the one the key-down just before it
/// names in the active layout (`keyDownChar`, 0 = none since the last text), case aside. An IME's commit
/// follows the key that committed it - Space, Return, a candidate digit - which names something else, so a
/// one-character commit is not taken for a typed letter. SDL's iOS software keyboard
/// sends a key-down per character ahead of its text (SDL 3.4.2, SDL_uikitviewcontroller.m
/// textFieldTextDidChange -> SDL_SendKeyboardUnicodeKey), so its letters are vouched for the same way.
inline Bool ComboTextTypedByKey( Int count, WideChar ch, WideChar keyDownChar )
{
	if( count != 1 || keyDownChar == 0 )
		return FALSE;
	return ComboCaseFold( ch ) == ComboCaseFold( keyDownChar );
}

/// The character an SDL key-down vouches for (ComboTextTypedByKey): its keycode when that is a printable
/// character, and nothing for a Ctrl/Cmd chord, which types nothing itself - text straight after one (an
/// iPad's Cmd+V paste of one letter) is not its keystroke.
inline WideChar ComboKeyDownVouch( UnsignedInt keycode, Bool chord )
{
	if( chord )
		return 0;
	return ( ( keycode >= 0x20 && keycode < 0x7F ) || ( keycode >= 0xA0 && keycode <= 0xFFFF ) ) ? (WideChar)keycode : 0;
}

/// Does that vouch survive an SDL event on the way to its text? Only a key event (`keyOrText`: a key-down,
/// which replaces it, a key-up, or the text itself): iOS's software keyboard sends the key's up - and a
/// Shift down/up around it - before its text. Anything else in between - a click, a focus change, an IME's
/// editing text - ends it, so a stale key-down cannot vouch for text that came some other way (character
/// equality alone is not provenance).
inline Bool ComboVouchSurvives( Bool keyOrText )
{
	return keyOrText;
}

/// May this character of an untrusted name appear in the heading?
inline Bool LobbyTitleCharAllowed( WideChar c )
{
	if( c < 0x20 || ( c >= 0x7F && c <= 0x9F ) )
		return FALSE;		// C0, DEL, C1
	if( ( c >= 0x200E && c <= 0x200F ) || ( c >= 0x202A && c <= 0x202E ) || ( c >= 0x2066 && c <= 0x2069 ) )
		return FALSE;		// LRM/RLM, embeddings/overrides, isolates
	if( c == 0xFEFF || c == 0xFFFE || c == 0xFFFF )
		return FALSE;		// BOM / non-characters
	return TRUE;
}

/// Width in pixels of the first n characters of the text being fitted.
typedef Int (*LobbyTitlePrefixWidthFunc)( void *ctx, Int n );

/// How many characters of `text` to show in a box `maxWidth` pixels wide. Returns `len` when the
/// whole text fits; otherwise the longest prefix, with trailing spaces trimmed, that fits together
/// with an ellipsis `ellipsisWidth` wide (possibly 0 - the caller then shows the ellipsis alone).
inline Int LobbyTitleFitLength( const WideChar *text, Int len, Int maxWidth, Int ellipsisWidth,
																LobbyTitlePrefixWidthFunc prefixWidth, void *ctx )
{
	if( text == nullptr || len <= 0 || prefixWidth == nullptr )
		return 0;
	if( prefixWidth( ctx, len ) <= maxWidth )
		return len;
	Int n = len - 1;
	while( n > 0 && prefixWidth( ctx, n ) + ellipsisWidth > maxWidth )
		--n;
	while( n > 0 && text[ n - 1 ] == L' ' )
		--n;
	return n;
}
