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

// FILE: HotKey.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Sep 2002
//
//	Filename: 	HotKey.cpp
//
//	author:		Chris Huybregts
//
//	purpose:
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include <wctype.h>	// GeneralsX @bugfix towlower/towupper, HotKeyManager::labelWithHotKey
#include <stdio.h>
#include <stdlib.h>
//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "GameClient/HotKey.h"
#include "GameClient/GXGraphicsSettings.h"	// GX_REMASTERED_UI
#include "GameClient/KeyDefs.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameText.h"
#include "Common/AudioEventRTS.h"
#include "Common/UserPreferences.h"
#include "GameClient/ControlBar.h"
#include "GameClient/GadgetPushButton.h"
//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// GeneralsX @probe Claude 25/09/2026 See HotKey.h.
Bool GXKeyTraceEnabled()
{
	static Int s_on = -1;
	if( s_on < 0 )
	{
		const char *env = getenv( "GX_KEYTRACE" );
		s_on = ( env != nullptr && env[0] != '\0' && env[0] != '0' ) ? 1 : 0;
	}
	return s_on != 0;
}

// Every window in the hotkey map was bound by ControlBar::setControlCommand, so its gadget data is the CommandButton.
static const char *gxHotKeyWindowCommand( const GameWindow *win )
{
	if( win == nullptr )
		return "(null window)";
	const CommandButton *command = (const CommandButton *)GadgetButtonGetData( const_cast<GameWindow *>( win ) );
	return command ? command->getName().str() : "(no command)";
}

//-----------------------------------------------------------------------------
GameMessageDisposition HotKeyTranslator::translateGameMessage(const GameMessage *msg)
{
	GameMessageDisposition disp = KEEP_MESSAGE;
	GameMessage::Type t = msg->getType();

	if ( t == GameMessage::MSG_RAW_KEY_UP)
	{

		//char key = msg->getArgument(0)->integer;
		Int keyState = msg->getArgument(1)->integer;

		// for our purposes here, we don't care to distinguish between right and left keys,
		// so just fudge a little to simplify things.
		Int newModState = 0;

		if( keyState & KEY_STATE_CONTROL )
		{
			newModState |= CTRL;
		}

		if( keyState & KEY_STATE_SHIFT )
		{
			newModState |= SHIFT;
		}

		if( keyState & KEY_STATE_ALT )
		{
			newModState |= ALT;
		}
		if(newModState != 0)
		{
			if( GXKeyTraceEnabled() )
			{
				fprintf( stderr, "[GXKEY] hotkey: key=0x%02x state=0x%x mods=%d - a modifier is held, command-button hotkeys ignore it\n",
					msg->getArgument(0)->integer, keyState, newModState );
				fflush( stderr );
			}
			return disp;
		}
		WideChar key = TheKeyboard->getPrintableKey((KeyDefType)msg->getArgument(0)->integer, 0);
		UnicodeString uKey;
		uKey.concat(key);
		AsciiString aKey;
		aKey.translate(uKey);
		if(TheHotKeyManager && TheHotKeyManager->executeHotKey(aKey))
			disp = DESTROY_MESSAGE;
		else if( GXKeyTraceEnabled() )
		{
			fprintf( stderr, "[GXKEY] hotkey: key=0x%02x char='%s' state=0x%x - not fired, passed on\n",
				msg->getArgument(0)->integer, aKey.str(), keyState );
			fflush( stderr );
		}
	}
	return disp;
}

//-----------------------------------------------------------------------------
HotKey::HotKey()
{
	m_win = nullptr;
	m_key.clear();
}

//-----------------------------------------------------------------------------
HotKeyManager::HotKeyManager()
	: m_showIndicators( TRUE ), m_badgeCorner( BADGE_BOTTOM_LEFT ), m_badgeScale( 1.0f ), m_badgeStyle( BADGE_PLATE )
{

}

//-----------------------------------------------------------------------------
HotKeyManager::~HotKeyManager()
{
	m_hotKeyMap.clear();
	m_priorityHotKeyMap.clear();
}

//-----------------------------------------------------------------------------
void HotKeyManager::init()
{
	m_hotKeyMap.clear();
	m_priorityHotKeyMap.clear();
	// GeneralsX @feature wh1ter0se 20/08/2026 pick up the user's rebinds before the control bar
	// populates for the first time.
	loadCommandButtonOverrides();
}

//-----------------------------------------------------------------------------
void HotKeyManager::reset()
{
	// GeneralsX @feature Claude 27/09/2026 Not m_priorityHotKeyMap: this runs on every context switch, and the
	// General's Points panel's keys must outlive those while it is open (see addPriorityHotKey).
	m_hotKeyMap.clear();
	// Re-read on reset so a rebind made between matches applies without restarting the game.
	loadCommandButtonOverrides();
}

//-----------------------------------------------------------------------------
// GeneralsX @feature Claude 27/09/2026 See HotKey.h.
Bool HotKeyManager::addPriorityHotKey( GameWindow *win, const AsciiString& keyIn )
{
	AsciiString key = keyIn;
	key.toLower();
	HotKeyMap::iterator it = m_priorityHotKeyMap.find( key );
	if( it != m_priorityHotKeyMap.end() )
	{
		if( GXKeyTraceEnabled() && it->second.m_win != win )
		{
			fprintf( stderr, "[GXKEY] panel bind: '%s' REFUSED for %s - already bound to %s\n",
				key.str(), gxHotKeyWindowCommand( win ), gxHotKeyWindowCommand( it->second.m_win ) );
			fflush( stderr );
		}
		return FALSE;
	}
	HotKey newHK;
	newHK.m_key.set( key );
	newHK.m_win = win;
	m_priorityHotKeyMap[ key ] = newHK;
	if( GXKeyTraceEnabled() )
	{
		fprintf( stderr, "[GXKEY] panel bind: '%s' -> %s (wins over the command bar while the panel is open)\n",
			key.str(), gxHotKeyWindowCommand( win ) );
		fflush( stderr );
	}
	return TRUE;
}

//-----------------------------------------------------------------------------
void HotKeyManager::clearPriorityHotKeys()
{
	m_priorityHotKeyMap.clear();
}

//-----------------------------------------------------------------------------
const HotKey *HotKeyManager::findBinding( const AsciiString& lowerKey ) const
{
	HotKeyMap::const_iterator it = m_priorityHotKeyMap.find( lowerKey );
	if( it != m_priorityHotKeyMap.end() )
		return &it->second;
	it = m_hotKeyMap.find( lowerKey );
	return it != m_hotKeyMap.end() ? &it->second : nullptr;
}

//-----------------------------------------------------------------------------
Bool HotKeyManager::addHotKey( GameWindow *win, const AsciiString& keyIn)
{
	AsciiString key = keyIn;
	key.toLower();
	HotKeyMap::iterator it = m_hotKeyMap.find(key);
	if( it != m_hotKeyMap.end() )
	{
		if( GXKeyTraceEnabled() && it->second.m_win != win )
		{
			fprintf( stderr, "[GXKEY] bind: '%s' REFUSED for %s - already bound to %s\n",
				key.str(), gxHotKeyWindowCommand( win ), gxHotKeyWindowCommand( it->second.m_win ) );
			fflush( stderr );
		}
		DEBUG_LOG(("Hotkey %s is already mapped to window %s, current window is %s", key.str(), it->second.m_win->winGetInstanceData()->m_decoratedNameString.str(), win->winGetInstanceData()->m_decoratedNameString.str()));
		return FALSE;
	}
	HotKey newHK;
	newHK.m_key.set(key);
	newHK.m_win = win;
	m_hotKeyMap[key] = newHK;
	return TRUE;
}

//-----------------------------------------------------------------------------
Bool HotKeyManager::isHotKeyBoundTo( const AsciiString& keyIn, const GameWindow *win ) const
{
	AsciiString key = keyIn;
	key.toLower();
	const HotKey *hk = findBinding( key );
	return hk != nullptr && hk->m_win == win;
}

//-----------------------------------------------------------------------------
Bool HotKeyManager::executeHotKey( const AsciiString& keyIn )
{
	AsciiString key = keyIn;
	key.toLower();
	const HotKey *hk = findBinding( key );
	if( hk == nullptr )
	{
		if( GXKeyTraceEnabled() )
		{
			fprintf( stderr, "[GXKEY] hotkey: '%s' is bound to no button (%d key(s) bound)\n", key.str(),
				(Int)( m_hotKeyMap.size() + m_priorityHotKeyMap.size() ) );
			fflush( stderr );
		}
		return FALSE;
	}
	GameWindow *win = hk->m_win;
	if( !win )
		return FALSE;
	if( GXKeyTraceEnabled() )
	{
		fprintf( stderr, "[GXKEY] hotkey: '%s' -> %s hidden=%d enabled=%d%s\n", key.str(), gxHotKeyWindowCommand( win ),
			(Int)BitIsSet( win->winGetStatus(), WIN_STATUS_HIDDEN ), (Int)BitIsSet( win->winGetStatus(), WIN_STATUS_ENABLED ),
			BitIsSet( win->winGetStatus(), WIN_STATUS_HIDDEN ) ? " - hidden, nothing happens"
				: BitIsSet( win->winGetStatus(), WIN_STATUS_ENABLED ) ? " - pressing it" : " - disabled click" );
		fflush( stderr );
	}
	if( !BitIsSet( win->winGetStatus(), WIN_STATUS_HIDDEN ) )
	{
		if( BitIsSet( win->winGetStatus(), WIN_STATUS_ENABLED ) )
 		{
 			TheWindowManager->winSendSystemMsg( win->winGetParent(), GBM_SELECTED, (WindowMsgData)win, win->winGetWindowId() );

 			// here we make the same click sound that the GUI uses when you click a button
 			AudioEventRTS buttonClick("GUIClick");

 			if( TheAudio )
 			{
 				TheAudio->addAudioEvent( &buttonClick );
 			}
			return TRUE;
 		}

		AudioEventRTS disabledClick( "GUIClickDisabled" );
		if( TheAudio )
		{
			TheAudio->addAudioEvent( &disabledClick );
		}
	}
	return FALSE;
}

//-----------------------------------------------------------------------------
AsciiString HotKeyManager::searchHotKey( const AsciiString& label)
{
	return searchHotKey(TheGameText->fetch(label));
}

//-----------------------------------------------------------------------------
AsciiString HotKeyManager::searchHotKey( const UnicodeString& uStr )
{
	if(uStr.isEmpty())
		return AsciiString::TheEmptyString;

	const WideChar *marker = (const WideChar *)uStr.str();
	while (marker && *marker)
	{
		if (*marker == L'&')
		{
			// found a '&' - now look for the next char
			UnicodeString tmp = UnicodeString::TheEmptyString;
			tmp.concat(*(marker+1));
			AsciiString retStr;
			retStr.translate(tmp);
			return retStr;
		}
		marker++;
	}
	return AsciiString::TheEmptyString;
}

//-----------------------------------------------------------------------------
// GeneralsX @bugfix Claude 26/09/2026 See HotKey.h. scripts/test/audit/bench/tooltip_hotkey_label.cpp carries a copy.
UnicodeString HotKeyManager::labelWithHotKey( const UnicodeString& label, const AsciiString& boundKey )
{
	const Int len = label.getLength();
	const WideChar *s = label.str();

	// The marker the tooltip draws: an '&' before a printable character, as Render2DSentenceClass hides them.
	Int marker = -1;
	for( Int i = 0; i + 1 < len; ++i )
	{
		if( s[i] == L'&' && s[i + 1] > L' ' )
		{
			marker = i;
			break;
		}
	}
	const WideChar want = ( boundKey.getLength() == 1 )
		? (WideChar)towlower( (wint_t)(unsigned char)boundKey.getCharAt( 0 ) ) : 0;

	// Already true - the retail case, and the label is returned untouched.
	if( marker < 0 ? want == 0 : ( want != 0 && (WideChar)towlower( (wint_t)s[marker + 1] ) == want ) )
		return label;

	// Every marker out, the character after it kept.
	UnicodeString plain;
	for( Int i = 0; i < len; ++i )
	{
		if( s[i] == L'&' && i + 1 < len && s[i + 1] > L' ' )
			++i;
		plain.concat( s[i] );
	}
	// "Scorpion (&A)" rebound to S reads "&Scorpion", not "&Scorpion (A)".
	if( marker >= 1 && marker == len - 3 && s[marker - 1] == L'(' && s[len - 1] == L')' )
	{
		plain.truncateBy( 3 );
		plain.trimEnd();
	}

	if( want == 0 )
		return plain;

	UnicodeString out;
	Bool placed = FALSE;
	const Int plainLen = plain.getLength();
	for( Int i = 0; i < plainLen; ++i )
	{
		const WideChar c = plain.getCharAt( i );
		if( !placed && (WideChar)towlower( (wint_t)c ) == want )
		{
			out.concat( L'&' );
			placed = TRUE;
		}
		out.concat( c );
	}
	if( !placed )
	{
		out.concat( L" (&" );
		out.concat( (WideChar)towupper( (wint_t)want ) );
		out.concat( L')' );
	}
	return out;
}

//-----------------------------------------------------------------------------
// GeneralsX @feature wh1ter0se 20/08/2026 Rebindable command-button hotkeys.
//
// The keybind file lives in the USER data directory, not in a BIG archive, and deliberately so:
// a keybind is a per-user preference and must not participate in dataID. Putting it in the game
// data would make two players with different keybinds fail to join each other with DATA_DIFFERS.
//
// Format is the standard UserPreferences "key = value" map, keyed by the CommandButton's INI
// name VERBATIM:
//     Command_ConstructChinaDozer = J
//     Early_Command_LeafletDrop   = L
// No prefix is added by us. CommandButton names already begin with "Command_" - and a good few
// with "Early_Command_" - so requiring a prefix of our own would mean writing
// "Command_Command_ConstructChinaDozer" and would leave the Early_ buttons unbindable.
//-----------------------------------------------------------------------------
static const char *KEYBIND_PREFS_FILE = "CommandButtonHotKeys.ini";
static const char *KEYBIND_SHOW_KEY   = "ShowHotKeyIndicators";

// GeneralsX @feature Claude 29/09/2026 The letter badge (HotKey.h getBadgeCorner). Values are matched case-blind; one
// this build does not know keeps the default. The launcher (gxio keybinds.rs BADGE_KEYS) writes the same spellings and
// looks for KEYBIND_BADGE_CORNER in this binary before it offers any of them.
static const char *KEYBIND_BADGE_CORNER = "GXHotKeyBadgeCorner";
static const char *KEYBIND_BADGE_SIZE   = "GXHotKeyBadgeSize";
static const char *KEYBIND_BADGE_STYLE  = "GXHotKeyBadgeStyle";

// The value that CLEARS a button's key (gxio keybinds.rs NO_KEY): stored as an empty override, which
// getCommandButtonHotKey returns as it stands - the button binds no key and draws no letter, exactly as a button
// with no '&' in its label does. An engine from before this reads it as four characters and keeps the retail letter.
static const char *KEYBIND_NO_KEY = "none";

static Int badgeChoice( const UserPreferences& prefs, const char *key, const char *const *names, Int count, Int fallback )
{
	AsciiString v = prefs.getAsciiString( key, AsciiString::TheEmptyString );
	v.trim();
	for( Int i = 0; i < count; ++i )
		if( stricmp( v.str(), names[i] ) == 0 )
			return i;
	return fallback;
}

//-----------------------------------------------------------------------------
void HotKeyManager::loadCommandButtonOverrides()
{
	m_commandButtonOverrides.clear();
	m_showIndicators = TRUE;	// on unless the player turns it off
	m_badgeCorner = BADGE_BOTTOM_LEFT;
	m_badgeScale = 1.0f;
	m_badgeStyle = BADGE_PLATE;

	UserPreferences prefs;
	if( prefs.load( KEYBIND_PREFS_FILE ) == FALSE )
		return;	// no file yet is the normal case - retail '&' markers stay in charge

	m_showIndicators = prefs.getBool( KEYBIND_SHOW_KEY, TRUE );

	// Order matches the enums in HotKey.h, and the launcher's lists.
	static const char *const CORNERS[] = { "TopLeft", "TopRight", "BottomLeft", "BottomRight" };
	static const char *const SIZES[] = { "Small", "Normal", "Large" };
	static const Real SCALES[] = { 0.8f, 1.0f, 1.3f };
	static const char *const STYLES[] = { "Plate", "Framed" };
	m_badgeCorner = (BadgeCorner)badgeChoice( prefs, KEYBIND_BADGE_CORNER, CORNERS, ARRAY_SIZE( CORNERS ), BADGE_BOTTOM_LEFT );
	m_badgeScale = SCALES[ badgeChoice( prefs, KEYBIND_BADGE_SIZE, SIZES, ARRAY_SIZE( SIZES ), 1 ) ];
	m_badgeStyle = (BadgeStyle)badgeChoice( prefs, KEYBIND_BADGE_STYLE, STYLES, ARRAY_SIZE( STYLES ), BADGE_PLATE );

	for( UserPreferences::const_iterator it = prefs.begin(); it != prefs.end(); ++it )
	{
		if( stricmp( it->first.str(), KEYBIND_SHOW_KEY ) == 0 )
			continue;	// the indicators switch, not a button binding
		if( stricmp( it->first.str(), KEYBIND_BADGE_CORNER ) == 0 || stricmp( it->first.str(), KEYBIND_BADGE_SIZE ) == 0
				|| stricmp( it->first.str(), KEYBIND_BADGE_STYLE ) == 0 )
			continue;	// the letter badge, not a button binding
		if( stricmp( it->first.str(), "GXFriendsKey" ) == 0 )
			continue;	// the Friends key (MetaEvent.cpp), not a button binding
		// GeneralsX @feature Claude 26/09/2026 The launcher's GENERAL tab chords (MetaEvent.cpp gxLoadGeneralKeys).
		if( stricmp( it->first.str(), "GXSelectInfantryKey" ) == 0 || stricmp( it->first.str(), "GXSelectAllKey" ) == 0
				|| stricmp( it->first.str(), "GXIdleWorkerKey" ) == 0 || stricmp( it->first.str(), "GXCameraResetKey" ) == 0 )
			continue;
#if GX_REMASTERED_UI	// GXGraphicsSettings.h: while 0 the binary names no GXRemasteredKey, so the launcher offers no row
		if( stricmp( it->first.str(), "GXRemasteredKey" ) == 0 )
			continue;
#endif

		AsciiString name = it->first;
		AsciiString key = it->second;
		key.trim();
		if( name.isEmpty() || key.isEmpty() )
			continue;

		if( stricmp( key.str(), KEYBIND_NO_KEY ) == 0 )
		{
			m_commandButtonOverrides[ name ] = AsciiString::TheEmptyString;	// cleared: no key, no letter
			continue;
		}

		// Only ever accept a single character. Anything longer would bind a key that
		// executeHotKey() can never match, leaving a letter drawn on a button that does nothing.
		if( key.getLength() != 1 )
		{
			DEBUG_LOG(( "HotKeyManager: ignoring keybind '%s' = '%s' - not a single character",
									name.str(), key.str() ));
			continue;
		}

		key.toLower();
		m_commandButtonOverrides[ name ] = key;
	}

	DEBUG_LOG(( "HotKeyManager: loaded %d command button keybind override(s)",
							(Int)m_commandButtonOverrides.size() ));
}

//-----------------------------------------------------------------------------
AsciiString HotKeyManager::getCommandButtonHotKey( const AsciiString& commandButtonName,
																									 const AsciiString& textLabel )
{
	HotKeyOverrideMap::const_iterator it = m_commandButtonOverrides.find( commandButtonName );
	// GeneralsX @test Claude 30/09/2026 GX_KEYS_TRACE=1: one stderr line per lookup, to see what a proof run's buttons ask
	// for and whether the override file answered (the Mini in-game proof showed a rebound button keeping its letter).
	static const Bool s_trace = getenv( "GX_KEYS_TRACE" ) != nullptr;
	if( s_trace )
	{
		fprintf( stderr, "[GXKEYS] button '%s' label '%s' overrides=%d -> %s\n", commandButtonName.str(), textLabel.str(),
				(int)m_commandButtonOverrides.size(), it != m_commandButtonOverrides.end() ? it->second.str() : "(none)" );
		fflush( stderr );
	}
	if( it != m_commandButtonOverrides.end() )
		return it->second;

	// No override - retail behaviour, the '&' marker inside the localized string.
	AsciiString key = searchHotKey( textLabel );
#if RTS_ZEROHOUR
	// GeneralsX @feature Claude 03/11/2026 Sell defaults to Z, for every army (asked by a pro player on 0.9.69: "I like it
	// as Z for all armies"). Retail's CONTROLBAR:Sell is plain "Sell", no '&', so Sell had no key at all. Measured on the
	// release data (English generals.csf + CommandButton.ini + every CommandSet): Command_Sell sits in 213 sets, slot 14 in
	// 209 and slot 12 in 4, and no other button in any of them - nor on any of the 13 shortcut bars - uses Z; the only
	// stock Z is Supply Drop Zone on the USA dozers, whose sets carry no Sell. Retail CommandMap binds no bare Z either.
	// Only when the label gives no letter: a localized or modded label that marks one keeps it. A player's override,
	// including a cleared key (KEYBIND_NO_KEY), returned above and wins. If a mod puts a Z button in a set beside Sell,
	// that button registers first (lower slot) and Sell's letter is drawn greyed, as for any refused key.
	// The launcher (gxio commandmap.rs DEFAULT_KEYS_MARKER) shows Z as Sell's game default only beside a binary that
	// contains GX_DEFAULT_KEYS_MARKER, so it never promises a key an older engine does not bind.
	static const char *GX_DEFAULT_KEYS_MARKER = "GXDefaultKey Command_Sell=z";
	if( key.isEmpty() && strcmp( commandButtonName.str(), "Command_Sell" ) == 0 )
	{
		if( s_trace )
		{
			fprintf( stderr, "[GXKEYS] button '%s' has no letter in its label - %s\n", commandButtonName.str(), GX_DEFAULT_KEYS_MARKER );
			fflush( stderr );
		}
		key = "z";
	}
#endif
	return key;
}

//-----------------------------------------------------------------------------
HotKeyManager *TheHotKeyManager = nullptr;

//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

