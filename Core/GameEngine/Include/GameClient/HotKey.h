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


// FILE: HotKey.h /////////////////////////////////////////////////
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
//	Filename: 	HotKey.h
//
//	author:		Chris Huybregts
//
//	purpose:
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

#pragma once

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "Common/SubsystemInterface.h"
#include "Common/MessageStream.h"
//-----------------------------------------------------------------------------
// FORWARD REFERENCES /////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
class AsciiString;
class GameWindow;
//-----------------------------------------------------------------------------
// TYPE DEFINES ///////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
class HotKeyTranslator : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage(const GameMessage *msg) override;
	virtual ~HotKeyTranslator() override { }
};

//-----------------------------------------------------------------------------
class HotKey
{
public:
	HotKey();
	GameWindow *m_win;
	AsciiString m_key;
	// we may need a checkmark system.
};

//-----------------------------------------------------------------------------
class HotKeyManager : public SubsystemInterface
{
public:
	HotKeyManager();
	virtual ~HotKeyManager() override;
	// Inherited from subsystem interface -----------------------------------------------------------
	virtual	void init() override;															///< Initialize the Hotkey system
	virtual void update() override {}														///< A No-op for us
	virtual void reset() override;															///< Reset
	//-----------------------------------------------------------------------------------------------

	// Returns FALSE when the key is already taken by another visible button (the first one bound
	// keeps it). The refused button's letter is still drawn, greyed: see isHotKeyBoundTo.
	Bool addHotKey( GameWindow *win, const AsciiString& key);
	Bool executeHotKey( const AsciiString& key); // called front eh HotKeyTranslator
	// GeneralsX: TRUE while pressing 'key' would fire 'win'. A button whose
	// letter lost to another (a unit's command set owns S, the general's-power shortcut wants S too)
	// or whose binding was cleared by a context switch reads FALSE, and its letter is drawn dimmed
	// instead of being hidden (W3DPushButton.cpp drawHotKeyLabel).
	Bool isHotKeyBoundTo( const AsciiString& key, const GameWindow *win ) const;

	// GeneralsX: The General's Points panel's keys, in a layer of their own that
	// executeHotKey and isHotKeyBoundTo consult FIRST: while the panel is open its keys win over the command bar, whose
	// letters on the same key grey. reset() - every context switch - leaves this layer alone, because
	// evaluateContextUI repopulates the open panel BEFORE switchToContext resets the map; the panel owns it instead
	// (ControlBar::bindPurchaseScienceHotKeys fills it, hidePurchaseScience empties it). Returns FALSE when an earlier
	// panel button already holds the key.
	Bool addPriorityHotKey( GameWindow *win, const AsciiString& key );
	void clearPriorityHotKeys();

	AsciiString searchHotKey( const AsciiString& label);
	AsciiString searchHotKey( const UnicodeString& uStr );

	// GeneralsX: The tooltip's title highlights the letter after its '&' (the name is drawn
	// with WIN_STATUS_HOTKEY_TEXT), and that letter came straight from the CSF - so after a rebind, or while another
	// button owns the key, the tooltip named a key that did not fire the button. This returns the label marked for
	// boundKey instead: unchanged when its '&' already marks that key (retail), the '&' moved to the key's first
	// occurrence, or " (&K)" appended when the label has no such letter. An empty boundKey (nothing fires this
	// button) strips the marker. A trailing "(&X)" group - the community packs' shape - is dropped whole.
	// Bench: scripts/test/audit/bench/tooltip_hotkey_label.cpp (a copy of this function; keep the two in step).
	static UnicodeString labelWithHotKey( const UnicodeString& label, const AsciiString& boundKey );

	// GeneralsX @feature wh1ter0se 20/08/2026 Rebindable command-button hotkeys.
	//
	// Retail welds the letter to the '&' marker inside the localized CONTROLBAR string, which is
	// why the community indicator packs had to paint A/V/Y into the cameo pixels - and why those
	// letters then lie the moment anybody rebinds. This is the ONE accessor that resolves a
	// command button's key. Both the binding (addHotKey) and the drawn glyph go through it, so
	// they cannot disagree by construction.
	//
	// Precedence: the user's keybind file wins; otherwise fall back to the retail '&' marker.
	AsciiString getCommandButtonHotKey( const AsciiString& commandButtonName, const AsciiString& textLabel );

	// GeneralsX: True when the user's keybind file names this very button (a cleared "none" counts).
	// A cross-faction shortcut superweapon button takes the key of its normal command unless the user bound IT explicitly.
	Bool hasCommandButtonOverride( const AsciiString& commandButtonName ) const { return m_commandButtonOverrides.find( commandButtonName ) != m_commandButtonOverrides.end(); }

	// Re-read the user's keybind file. Safe to call at any time; the control bar is rebuilt on the
	// next populate, so a rebind takes effect without a restart.
	void loadCommandButtonOverrides();

	// The launcher's "Hotkey indicators" switch. This lives here rather than in TheGlobalData on
	// purpose: Core/GameEngine/Include/Common is inside the simulation source digest, so a field
	// there would move sourceID and force a four-platform rebuild for what is only a per-user
	// display preference.
	Bool getShowCommandButtonIndicators() const { return m_showIndicators; }

	// GeneralsX: The letter badge, from the launcher's Keybinds page:
	// which corner of the button, how big, and how it is drawn. Same file and same
	// reasoning as the indicators switch above - a per-user display preference, outside the digest. The defaults are the
	// look before this change (bottom-left, a dark plate), so a file without these entries changes nothing.
	enum BadgeCorner { BADGE_TOP_LEFT, BADGE_TOP_RIGHT, BADGE_BOTTOM_LEFT, BADGE_BOTTOM_RIGHT };
	enum BadgeStyle { BADGE_PLATE, BADGE_FRAMED };
	BadgeCorner getBadgeCorner() const { return m_badgeCorner; }
	Real getBadgeScale() const { return m_badgeScale; }	///< multiplies the letter's point size: 0.8, 1.0 or 1.3
	BadgeStyle getBadgeStyle() const { return m_badgeStyle; }

private:
	typedef std::map<AsciiString, HotKey> HotKeyMap;
	HotKeyMap m_hotKeyMap;
	HotKeyMap m_priorityHotKeyMap;	///< the General's Points panel's keys, see addPriorityHotKey

	const HotKey *findBinding( const AsciiString& lowerKey ) const;	///< the priority layer first, then the map

	typedef std::map<AsciiString, AsciiString> HotKeyOverrideMap;
	HotKeyOverrideMap m_commandButtonOverrides;	///< commandButton name -> user-chosen key
	Bool m_showIndicators;											///< draw the letter over the cameo at all
	BadgeCorner m_badgeCorner;
	Real m_badgeScale;
	BadgeStyle m_badgeStyle;
};
extern HotKeyManager *TheHotKeyManager;

class CommandButton;
/// GeneralsX: The button whose key a shortcut-bar superweapon button shows and binds (ControlBar.cpp):
/// a cross-faction one answers its normal command, everything else answers itself. The tooltip asks it too, so title and cameo agree.
const CommandButton *gxShortcutKeySource( const CommandButton *shortcut );

// GeneralsX: GX_KEYTRACE=1 turns on the [GXKEY] trace: one stderr line per step a
// command-button hotkey takes, from the raw key (WindowXlat, MetaEvent, HotKeyTranslator) through the button it
// resolves to (addHotKey refusals included) and the button's processing (ControlBar), to the GUI command it arms
// (InGameUI::setGUICommand) and the order the next click issues or drops (GUICommandTranslator). Off by default and
// a single cached getenv, so it costs nothing unless set. Written for the "select units, press A, they stand still"
// report, which a static trace of this path could not explain: with retail data it matches retail line for line.
Bool GXKeyTraceEnabled();
//-----------------------------------------------------------------------------
// INLINING ///////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// EXTERNALS //////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
