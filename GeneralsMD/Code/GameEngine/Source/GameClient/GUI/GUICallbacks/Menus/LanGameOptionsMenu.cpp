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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LanGameOptionsMenu.cpp
// Author: Chris Huybregts, October 2001
// Description: Lan Game Options Menu
///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine


#include "Common/PlayerTemplate.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/UserPreferences.h"
#include "Common/QuotedPrintable.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/CodeGui.h"		// the host password row is built in code, not in the .wnd
#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GXLobbySlotMemory.h"
#include "GameClient/GXMatchChat.h"		// gxLobbyDrainRelayNotices - the operator's voice in this room
#include "GameClient/GUICallbacks.h"	// GXApplyNickname
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/ChatRateLimit.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Mouse.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"

#include "GameNetwork/FirewallHelper.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/GameText.h"
#include "GameClient/LanStartCountdown.h"
#include "GameNetwork/GUIUtil.h"


extern char *LANnextScreen;
extern Bool LANisShuttingDown;
extern Bool LANbuttonPushed;
extern void MapSelectorTooltip(GameWindow *window, WinInstanceData *instData,	UnsignedInt mouse);
extern void gameAcceptTooltip(GameWindow *window, WinInstanceData *instData, UnsignedInt mouse);
Color white = GameMakeColor( 255, 255, 255, 255 );
static bool s_isIniting = FALSE;
// window ids ------------------------------------------------------------------------------
static NameKeyType parentLanGameOptionsID = NAMEKEY_INVALID;

// GeneralsX: The shipped .wnd layouts carry exactly this many slot
// rows. MAX_SLOTS is now larger; lookups for rows past this legitimately return null until the
// dynamic-row lobby rebuild lands, so the per-row asserts below tolerate them.
enum { LEGACY_WND_SLOT_ROWS = 8 };

static NameKeyType comboBoxPlayerID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonAcceptID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxColorID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxPlayerTemplateID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxTeamID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

//static NameKeyType buttonStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType textEntryMapDisplayID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonStartID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType buttonSelectMapID = NAMEKEY_INVALID;
static NameKeyType checkboxLimitSuperweaponsID = NAMEKEY_INVALID;
static NameKeyType comboBoxStartingCashID = NAMEKEY_INVALID;
static NameKeyType windowMapID = NAMEKEY_INVALID;
// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentLanGameOptions = nullptr;

/// The heading over the slot list - it carries the LOBBY'S NAME now, not the words "GAME OPTIONS".
/// See gxRefreshLobbyTitle. Cached because it is refreshed from the ~30 Hz menu update and
/// winGetWindowFromId is a lookup rather than a field read; both are cleared on init so a second
/// visit to the screen cannot hold a window belonging to the previous layout.
static GameWindow *staticTextLobbyTitle = nullptr;
static UnicodeString s_lobbyTitleShown;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonStart = nullptr;
static GameWindow *buttonSelectMap = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *textEntryMapDisplay = nullptr;
static GameWindow *checkboxLimitSuperweapons = nullptr;
// GeneralsX: Limit Armies ("O=Y"): built beside Limit Superweapons in
// InitLanGameGadgets - the retail layout has no such control. Matched by pointer, not id.
static GameWindow *checkboxLimitArmies = nullptr;
static GameWindow *comboBoxStartingCash = nullptr;
static GameWindow *windowMap = nullptr;

static GameWindow *comboBoxPlayer[MAX_SLOTS] = {0};
static GameWindow *buttonAccept[MAX_SLOTS] = {0};

static GameWindow *comboBoxColor[MAX_SLOTS] = {0};

static GameWindow *comboBoxPlayerTemplate[MAX_SLOTS] = {0};

static GameWindow *comboBoxTeam[MAX_SLOTS] = {0};

//static GameWindow *buttonStartPosition[MAX_SLOTS] = {0};
//
static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {0};

//external declarations of the Gadgets the callbacks can use
GameWindow *listboxChatWindowLanGame = nullptr;
static void lanGameRow( const UnicodeString &tag, const UnicodeString &line, Color color );	// with the chat seam
static void lanGameSay( const UnicodeString &line, Color color );
NameKeyType listboxChatWindowLanGameID = NAMEKEY_INVALID;
WindowLayout *mapSelectLayout = nullptr;

// Host-only password row built in code above the chat list (no .wnd change), as CodeGuiSearchField does.
// It hands the line a player would type to LANAPI::RequestChat, which handles lobby commands locally, so the
// password never reaches the wire. It skips setHostPassword (protected; would miss m_joinPassword).
static GameWindow *s_pwEntry = nullptr;
static GameWindow *s_pwLabel = nullptr;
static GameWindow *s_pwSetButton = nullptr;
static GameWindow *s_pwPrivateCheck = nullptr;

/// Forget the row's windows without destroying them. They are children of the chat listbox's
/// parent, so the screen's WindowLayout tears them down with everything else; what must not
/// survive is a raw pointer to a window the engine has already freed behind our back (see the
/// lifetime note on CodeGuiSearchField - GameWindowManager::reset() calls winDestroyAll()).
static void lanPasswordRowReset( void )
{
	s_pwEntry = nullptr;
	s_pwLabel = nullptr;
	s_pwSetButton = nullptr;
	s_pwPrivateCheck = nullptr;
}

/// Send one of the row's windows behind the chat listbox's siblings but just in front of the listbox itself.
///
/// The child list is front to back and winCreate prepends, so a new row would cover the bottom combo boxes' open lists.
/// The chat listbox is the anchor because it is authored over the row's strip and lanPasswordRowCreate moves it away.
static void lanPasswordRowSendBehind( GameWindow *win )
{
	if( win == nullptr || TheWindowManager == nullptr || listboxChatWindowLanGame == nullptr )
		return;

	// unlinkChildWindow leaves m_prev/m_next dangling and clears m_parent; insertWindowAheadOf
	// overwrites all three, so the window is never observed in the half-linked state.
	TheWindowManager->unlinkChildWindow( win );
	TheWindowManager->insertWindowAheadOf( win, listboxChatWindowLanGame );
}

/// The stand-in shown when a password is bound. The host never holds the plaintext (only a salted
/// verifier - see the HOST SIDE note in LANAPI.h), so there is nothing truthful to redisplay.
///
/// Read through one accessor rather than written out twice, because it is compared as well as
/// displayed: a .csf that overrides GUI:LanPasswordInEffect would otherwise make the box show one
/// string while the "leave it alone" test still looked for another, and pressing Set on an
/// untouched box would rebind the password to the placeholder text.
static UnicodeString lanPasswordPlaceholder( void )
{
	return TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LanPasswordInEffect", L"********" );
}

/// Push the typed form of a lobby command through the chat funnel. See the note above.
static void lanPasswordApply( const AsciiString &command )
{
	if( TheLAN == nullptr || command.isEmpty() )
		return;

	UnicodeString line;
	line.translate( command );
	TheLAN->RequestChat( line, LANAPIInterface::LANCHAT_NORMAL );
}

/// The authored Starting Money caption. The .wnd gives it no name: it is the static text that ends where the
/// starting-cash drop-down begins, on the same line.
static GameWindow *lanStartingCashCaption( void )
{
	if( comboBoxStartingCash == nullptr || comboBoxStartingCash->winGetParent() == nullptr )
		return nullptr;

	Int cx = 0, cy = 0;
	comboBoxStartingCash->winGetPosition( &cx, &cy );
	const Int slack = CodeGuiAtLeast( CodeGuiScaleX( 2 ), 3 );
	for( GameWindow *win = comboBoxStartingCash->winGetParent()->winGetChild(); win != nullptr; win = win->winGetNext() )
	{
		if( !BitIsSet( win->winGetStyle(), GWS_STATIC_TEXT ) )
			continue;
		Int x = 0, y = 0, w = 0, h = 0;
		win->winGetPosition( &x, &y );
		win->winGetSize( &w, &h );
		if( abs( y - cy ) <= slack && abs( x + w - cx ) <= slack )
			return win;
	}
	return nullptr;
}

/// The row's height: the options row's own (Limit Superweapons), so the two lines read as one grid. The seat rows'
/// share of the chat (InitLanGameGadgets) budgets the same number.
static Int lanPasswordRowHeight( void )
{
	Int w = 0, h = 0;
	if( checkboxLimitSuperweapons != nullptr )
		checkboxLimitSuperweapons->winGetSize( &w, &h );
	return CodeGuiAtLeast( h, CodeGuiAtLeast( CodeGuiScaleY( 21 ), 21 ) );
}

/// Build the row above the chat listbox, shrinking it to make room. Host only.
static void lanPasswordRowCreate( void )
{
	lanPasswordRowReset();

	// Only the host binds a password. A joiner's /password is a different gesture entirely (it
	// stores a credential for the NEXT join attempt), and a box labelled the same way on both
	// sides would read as "set the password for this game" to someone who cannot.
	if( TheLAN == nullptr || TheWindowManager == nullptr || listboxChatWindowLanGame == nullptr )
		return;
	if( !TheLAN->AmIHost() )
		return;

	GameWindow *listParent = listboxChatWindowLanGame->winGetParent();
	if( listParent == nullptr )
		return;

	Int lx = 0, ly = 0, lw = 0, lh = 0;
	listboxChatWindowLanGame->winGetPosition( &lx, &ly );	// parent-relative device pixels
	listboxChatWindowLanGame->winGetSize( &lw, &lh );

	// Int-only arithmetic throughout: Lib/BaseType.h replaces the min/max macros with
	// template<typename T> T max(T,T), so mixing Int and UnsignedInt is a hard compile error.
	const Int rowH   = lanPasswordRowHeight();
	const Int gap    = CodeGuiAtLeast( CodeGuiScaleY( 4 ), 3 );
	const Int minEntryW = CodeGuiAtLeast( CodeGuiScaleX( 90 ), 70 );

	// GeneralsX @bugfix The row wears the screen's own controls, not the CodeGui theme: the caption is a twin of
	// the Starting Money caption, the box of the chat entry, Set of the Select Map button, Private of Limit
	// Superweapons. And it sits on the options row's grid, as if authored with it: the caption in the Starting
	// Money column, the box from where the starting-cash drop-down starts, Set ending where Limit Superweapons
	// ends, Private in the Limit Armies column. Without those templates (another layout) it keeps the plain row.
	GameWindow *captionLook = lanStartingCashCaption();
	GameWindow *entryLook = ( textEntryChat != nullptr && textEntryChat->winGetParent() == listParent ) ? textEntryChat : nullptr;
	GameWindow *buttonLook = ( buttonSelectMap != nullptr && buttonSelectMap->winGetParent() == listParent ) ? buttonSelectMap : nullptr;
	const Bool onGrid = captionLook != nullptr && captionLook->winGetParent() == listParent && comboBoxStartingCash != nullptr &&
											checkboxLimitSuperweapons != nullptr && checkboxLimitSuperweapons->winGetParent() == listParent;

	Int labelX = lx, labelW = CodeGuiAtLeast( CodeGuiScaleX( 62 ), 50 );
	Int setW = CodeGuiAtLeast( CodeGuiScaleX( 46 ), 40 );
	Int privW = CodeGuiAtLeast( CodeGuiScaleX( 78 ), 64 );
	Int entryX = labelX + labelW + gap;
	Int setX = 0, privX = 0;
	Bool gridFits = FALSE;
	if( onGrid )
	{
		Int cy = 0, ch = 0, cashX = 0, sx = 0, sy = 0, sw = 0, sh = 0;
		captionLook->winGetPosition( &labelX, &cy );
		captionLook->winGetSize( &labelW, &ch );
		comboBoxStartingCash->winGetPosition( &cashX, &cy );
		checkboxLimitSuperweapons->winGetPosition( &sx, &sy );
		checkboxLimitSuperweapons->winGetSize( &sw, &sh );

		const Int gridSetW = CodeGuiAtLeast( CodeGuiScaleX( 64 ), 48 );
		const Int gridSetX = sx + sw - gridSetW;
		Int ax = sx + sw + gap, aw = sw;
		if( checkboxLimitArmies != nullptr && checkboxLimitArmies->winGetParent() == listParent )
		{
			Int ay = 0, ah = 0;
			checkboxLimitArmies->winGetPosition( &ax, &ay );
			checkboxLimitArmies->winGetSize( &aw, &ah );
		}
		// The Private column must be a whole one inside the parent: Limit Armies is absent, or cut short, when its
		// strip has no room, and a box built from it would be unreachable.
		Int pw = 0, ph = 0;
		listParent->winGetSize( &pw, &ph );
		if( gridSetX - gap - cashX >= minEntryW && aw >= privW && ax + aw <= pw )
		{
			entryX = cashX;
			setW = gridSetW;
			setX = gridSetX;
			privX = ax;
			privW = aw;
			gridFits = TRUE;
		}
	}
	if( !gridFits )
	{
		labelX = lx;
		labelW = CodeGuiAtLeast( CodeGuiScaleX( 62 ), 50 );
		entryX = labelX + labelW + gap;
		setX = lx + lw - privW - gap - setW;
		privX = setX + setW + gap;
	}

	// Refuse to shrink the chat into uselessness on a small backbuffer, and refuse to build a row
	// whose entry would be narrower than the caption beside it.
	const Int entryW = setX - gap - entryX;
	if( lh <= ( rowH + gap ) * 2 || entryW < minEntryW )
		return;

	// FETCH_OR_SUBSTITUTE returns a UnicodeString by value and the CodeGui factories take
	// const WideChar*, so each label is held in a named local: a temporary's .str() would still be
	// alive for the duration of the call, but naming them keeps that from being something the next
	// reader has to re-derive.
	const UnicodeString labelText   = TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LanPasswordLabel", L"Password" );
	const UnicodeString setText     = TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LanPasswordSet", L"SET" );		// capitals, as every retail button caption
	const UnicodeString setTipText  = TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LanPasswordSetTip",
		L"Require this password to join. Leave the box empty to clear it." );
	const UnicodeString privateText = TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LanPrivateGame", L"Private" );

	if( gridFits )
		s_pwLabel = CodeGuiLabelLike( captionLook, listParent, nullptr, labelX, ly, labelW, rowH, labelText.str() );
	if( s_pwLabel == nullptr )
		s_pwLabel = CodeGuiLabel( listParent, labelX, ly, labelW, rowH, nullptr, labelText.str(), FALSE, 12, TRUE );

	// maxTextLen is bounded by s_carriedLobbyPassword[65] in LANAPI.cpp, which strlcpy-truncates;
	// stopping short of that here means what the player typed is what gets bound, always.
	s_pwEntry = CodeGuiTextEntryLike( entryLook, listParent, "LanGameOptionsMenu.wnd:TextEntryLobbyPassword",
		entryX, ly, entryW, rowH, 32 );
	if( s_pwEntry == nullptr )
		s_pwEntry = CodeGuiTextEntry( listParent, entryX, ly, entryW, rowH,
			"LanGameOptionsMenu.wnd:TextEntryLobbyPassword", 32, 12, FALSE );
	if( s_pwEntry == nullptr )
	{
		lanPasswordRowReset();
		return;
	}

	s_pwSetButton = CodeGuiButtonLike( buttonLook, listParent, "LanGameOptionsMenu.wnd:ButtonLobbyPasswordSet",
		setX, ly, setW, rowH, setText.str(), setTipText.str() );
	if( s_pwSetButton == nullptr )
		s_pwSetButton = CodeGuiButton( listParent, setX, ly, setW, rowH,
			"LanGameOptionsMenu.wnd:ButtonLobbyPasswordSet",
			setText.str(), setTipText.str() );

	// The retail checkbox's twin. Its state bit is set by hand: GadgetCheckBoxSetChecked sends GBM_SELECTED, which
	// would reach lanPasswordRowHandle and re-send /private while the screen is still being built.
	if( checkboxLimitSuperweapons != nullptr && checkboxLimitSuperweapons->winGetParent() == listParent )
		s_pwPrivateCheck = CodeGuiCheckboxLike( checkboxLimitSuperweapons,
			"LanGameOptionsMenu.wnd:CheckboxLimitSuperweapons", "LanGameOptionsMenu.wnd:CheckboxLobbyPrivate",
			privX, ly, privW, privateText.str(), nullptr );
	if( s_pwPrivateCheck != nullptr )
	{
		if( TheLAN->isHostingPrivate() )
			BitSet( s_pwPrivateCheck->winGetInstanceData()->m_state, WIN_STATE_SELECTED );
	}
	else
	{
		s_pwPrivateCheck = CodeGuiCheckbox( listParent, privX, ly, privW, rowH,
			"LanGameOptionsMenu.wnd:CheckboxLobbyPrivate",
			privateText.str(), TheLAN->isHostingPrivate() );
	}

	// Reflect a password carried in from GENERALSX_LANPASS or from a /password typed in the lobby
	// before hosting, so the box agrees with what the host is actually enforcing. The plaintext is
	// never stored host-side (only a salted verifier), so this shows a fixed placeholder rather
	// than the password - clearing the box and pressing Set still clears the password.
	if( TheLAN->isHostingWithPassword() )
		GadgetTextEntrySetText( s_pwEntry, lanPasswordPlaceholder() );

	// Depth, now that all four exist. In this order so they end up label, entry, Set, Private front
	// to back - insertWindowAheadOf drops each one immediately before the listbox, so the last one
	// in lands nearest to it. They do not overlap each other, so the order among them is cosmetic;
	// what is NOT cosmetic is that all four end up behind the slot columns. See the note on
	// lanPasswordRowSendBehind.
	lanPasswordRowSendBehind( s_pwLabel );
	lanPasswordRowSendBehind( s_pwEntry );
	lanPasswordRowSendBehind( s_pwSetButton );
	lanPasswordRowSendBehind( s_pwPrivateCheck );

	// Take the row's space out of the chat listbox. winSetSize fires GGM_RESIZED, which
	// GadgetListBoxSystem handles by repositioning the up/down buttons and the slider and
	// recomputing the column widths, so a runtime resize is fully supported.
	listboxChatWindowLanGame->winSetPosition( lx, ly + rowH + gap );
	listboxChatWindowLanGame->winSetSize( lw, lh - rowH - gap );
}

/// TRUE when the message belonged to the password row and has been consumed.
static Bool lanPasswordRowHandle( UnsignedInt msg, GameWindow *control )
{
	if( control == nullptr )
		return FALSE;

	const Bool isCommit = ( ( msg == GBM_SELECTED && control == s_pwSetButton ) ||
													( msg == GEM_EDIT_DONE && control == s_pwEntry ) );

	if( isCommit )
	{
		if( s_pwEntry == nullptr )
			return FALSE;

		UnicodeString typed;
		typed.set( GadgetTextEntryGetText( s_pwEntry ) );
		typed.trim();

		// The placeholder means "leave it exactly as it is". Without this, opening the screen on a
		// password-protected game and pressing Set would silently rebind the password to the literal
		// asterisks - locking the host out of the password they believe is in force. Compared as
		// UnicodeString against the same accessor that wrote it, so a .csf override stays consistent.
		if( TheLAN->isHostingWithPassword() && typed == lanPasswordPlaceholder() )
			return TRUE;

		AsciiString pw;
		pw.translate( typed );

		if( pw.isEmpty() )
		{
			lanPasswordApply( AsciiString( "/nopassword" ) );
		}
		else
		{
			AsciiString cmd( "/password " );
			cmd.concat( pw );
			lanPasswordApply( cmd );

			// Do not leave the plaintext sitting in a box on screen once it is bound.
			GadgetTextEntrySetText( s_pwEntry, lanPasswordPlaceholder() );
		}
		return TRUE;
	}

	if( msg == GBM_SELECTED && control == s_pwPrivateCheck )
	{
		// Read the gadget's own state rather than tracking our own: GadgetCheckBoxSystem has already
		// toggled it by the time GBM_SELECTED reaches the owner.
		lanPasswordApply( GadgetCheckBoxIsChecked( s_pwPrivateCheck )
											? AsciiString( "/private" ) : AsciiString( "/public" ) );
		return TRUE;
	}

	return FALSE;
}

// Ready-gated start, forced start and host-only cancel. The host cannot start until every other human has
// accepted, or may force a start after a fixed window. No new wire message (LANMessage is in the digest):
// the countdown is retail's MSG_GAME_START_TIMER; force and cancel ride on LANCHAT_SYSTEM chat. State is host-only.
static LanStart::State s_lanStart = { LanStart::PHASE_IDLE, 0, -1 };

/// Slots this menu closed when the start began, so a cancel reopens exactly those and does not
/// reopen one the host had closed on purpose.
static Bool s_lanStartClosedSlot[MAX_SLOTS] = { FALSE };

/// Which GxStartControl caption the host's button currently carries, or -1 for "not set yet".
/// Tracked so the refresh below does not rewrite the caption on every slot-list update.
static Int s_lanStartControlShown = -1;

static Int getNextSelectablePlayer(Int start)
{
	LANGameInfo *game = TheLAN->GetMyGame();
	if (!game->amIHost())
		return -1;
	for (Int j=start; j<MAX_SLOTS; ++j)
	{
		LANGameSlot *slot = game->getLANSlot(j);
		if (slot && slot->getStartPos() == -1 &&
			( (j==game->getLocalSlotNum() && game->getConstSlot(j)->getPlayerTemplate()!=PLAYERTEMPLATE_OBSERVER)
			|| slot->isAI()))
		{
			return j;
		}
	}
	return -1;
}

static Int getFirstSelectablePlayer(const GameInfo *game)
{
	const GameSlot *slot = game->getConstSlot(game->getLocalSlotNum());
	if (!game->amIHost() || (slot && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER))
		return game->getLocalSlotNum();

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		slot = game->getConstSlot(i);
		if (slot && slot->isAI())
			return i;
	}

	return game->getLocalSlotNum();
}

void updateMapStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], Bool onLoadScreen = FALSE );
void positionStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void LanPositionStartSpots()
{

	positionStartSpots( TheLAN->GetMyGame(), buttonMapStartPosition, windowMap);
}
static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int idx = -1;
	Int i=0;
	for (; i<MAX_SLOTS; ++i)
	{
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (window && comboBoxPlayer[i] && window == GadgetComboBoxGetEditBox(comboBoxPlayer[i]))
		{
			idx = i;
			break;
		}
	}
	if (idx == -1)
		return;

	// TheSuperHackers @bugfix Okladnoj 17/10/2026 Check that the game still exists as it is deleted while the setup screen fades out after the host left.
	LANGameInfo *myGame = TheLAN->GetMyGame();
	if (!myGame)
		return;

	LANGameSlot *slot = myGame->getLANSlot(i);
	if (!slot)
		return;

	LANPlayer *player = slot->getUser();
	if (!player)
	{
		DEBUG_ASSERTCRASH(myGame->getIP(i) == 0, ("No player info in listbox!"));
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}

	setLANPlayerTooltip(player);
}

// GeneralsX: -----------------------------------------------------------
// Host-side start policy. See the note on s_lanStart above for why none of it is on the wire.

/// TRUE when every human in the lobby except the host has accepted.
///
/// Slot 0 is skipped rather than special-cased later: StartPressed accepts on the host's behalf
/// the moment it runs, so including it would make the gate report "not ready" for the one player
/// who is by definition ready. AI, open and closed slots are not humans and never block.
static Bool gxEveryPeerReady( const GameInfo *game )
{
	if( game == nullptr )
		return FALSE;

	for( Int i = 1; i < MAX_SLOTS; ++i )
	{
		const GameSlot *slot = game->getConstSlot(i);
		if( slot != nullptr && slot->isHuman() && !slot->isAccepted() )
			return FALSE;
	}
	return TRUE;
}

/// Say something to the whole lobby, host included.
///
/// RequestChat both broadcasts and echoes locally, so this is one call rather than a send plus a
/// GadgetListBoxAddEntryText. LANCHAT_SYSTEM is also the only chat type OnChat does not run
/// through TheLanguageFilter, and it is what every other lobby notice already uses.
static void gxLobbySay( const UnicodeString &text )
{
	if( TheLAN != nullptr )
		TheLAN->RequestChat( text, LANAPIInterface::LANCHAT_SYSTEM );
}

/// GX_LOBBY=1 -> one stderr line per start-policy transition, in the [GXNET]/[GXACT] idiom.
///
/// This exists because the alternative is photographing a button. The gate flips on a peer's
/// accept arriving and the countdown runs for five seconds; a screenshot of either is a race, and
/// a screenshot cannot say WHY the state changed. Transitions only - never per frame.
static void gxLobbyTrace( const char *fmt, ... )
{
	static const Bool s_on = (getenv("GX_LOBBY") != nullptr);
	if( !s_on )
		return;

	va_list args;
	va_start( args, fmt );
	fprintf( stderr, "[GXLOBBY] " );
	vfprintf( stderr, fmt, args );
	fprintf( stderr, "\n" );
	va_end( args );
	fflush( stderr );
}

/// What the host's one button means right now.
enum GxStartControl
{
	GXSTART_CTRL_PLAY = 0,  ///< everyone is ready: press to start, on a countdown
	GXSTART_CTRL_FORCE,     ///< somebody is not: press to give them a window and then start
	GXSTART_CTRL_CANCEL     ///< a start is running: press to abort it
};

/// Re-apply the host's start control to the current lobby state.
///
/// Relabelled, not greyed: a disabled window receives no input, and a new button would mean editing the .wnd.
/// PLAY GAME is unavailable until every other human accepted; FORCE START is offered instead, inert for
/// LanStart::FORCE_WINDOW_MS. Joiners' button is ACCEPT, owned by EnableAcceptControls.
static void gxUpdateStartControl()
{
	if( buttonStart == nullptr || TheLAN == nullptr || !TheLAN->AmIHost() )
		return;

	GxStartControl want;
	if( s_lanStart.phase != LanStart::PHASE_IDLE )
		want = GXSTART_CTRL_CANCEL;
	else if( gxEveryPeerReady( TheLAN->GetMyGame() ) )
		want = GXSTART_CTRL_PLAY;
	else
		want = GXSTART_CTRL_FORCE;

	if( s_lanStartControlShown != (Int)want )
	{
		static const char *names[3] = { "PLAY GAME", "FORCE START", "CANCEL" };
		gxLobbyTrace("control=%s peersReady=%d phase=%d",
			names[(Int)want], (Int)gxEveryPeerReady(TheLAN->GetMyGame()), (Int)s_lanStart.phase);

		s_lanStartControlShown = (Int)want;
		switch( want )
		{
			case GXSTART_CTRL_CANCEL:
				buttonStart->winSetText( TheGameText->FETCH_OR_SUBSTITUTE("GUI:Cancel", L"CANCEL") );
				break;
			case GXSTART_CTRL_FORCE:
				buttonStart->winSetText( TheGameText->FETCH_OR_SUBSTITUTE("LAN:ForceStart", L"FORCE START") );
				break;
			case GXSTART_CTRL_PLAY:
			default:
				buttonStart->winSetText( TheGameText->FETCH_OR_SUBSTITUTE("GUI:Start", L"PLAY GAME") );
				break;
		}
	}

	// The control is never dead for the host: while a start is running it is the only way to abort
	// it, and while the lobby is not ready it is the only way to force one. What gets locked out is
	// the map picker, which would change the match under a countdown.
	buttonStart->winEnable( TRUE );
	if( buttonSelectMap != nullptr )
		buttonSelectMap->winEnable( want != GXSTART_CTRL_CANCEL );
}

/// Hand the countdown to the engine and lock the lobby down.
static void gxBeginCountdown()
{
	LANGameInfo *myGame = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;
	if( myGame == nullptr )
		return;

	// Retail closes every still-open slot so a late joiner cannot land in the match being started.
	// Remember which ones we closed: a cancel has to put the lobby back the way it was, and
	// blanket-reopening would quietly undo a slot the host had closed deliberately.
	for( Int i = 0; i < MAX_SLOTS; i++ )
	{
		LANGameSlot *slot = myGame->getLANSlot(i);
		s_lanStartClosedSlot[i] = FALSE;
		if( slot != nullptr && slot->isOpen() )
		{
			slot->setState( SLOT_CLOSED );
			if( comboBoxPlayer[i] != nullptr )
				GadgetComboBoxSetSelectedPos( comboBoxPlayer[i], SLOT_CLOSED );
			s_lanStartClosedSlot[i] = TRUE;
		}
	}

	const Int seconds = TheMultiplayerSettings->getStartCountdownTimerSeconds();
	gxLobbyTrace("countdown begin seconds=%d peersReady=%d", seconds,
		(Int)gxEveryPeerReady(myGame));
	if( seconds > 0 )
	{
		LanStart::beginCountdown( s_lanStart, timeGetTime(), seconds );
		// The tick belongs to LANAPI, not to this menu: RequestGameStartTimer rebroadcasts
		// MSG_GAME_START_TIMER once a second off timeGetTime() and calls RequestGameStart at zero.
		// Every peer therefore counts down on the HOST's wall clock, not on its own frame rate,
		// and a peer whose renderer is struggling still sees 5,4,3,2,1 at one second apart.
		TheLAN->RequestGameStartTimer( seconds );
	}
	else
	{
		// StartCountdownTimer = 0 in multiplayer.ini means "no countdown", which retail honoured.
		LanStart::reset( s_lanStart );
		TheLAN->RequestGameStart();
	}

	// Push the closed slots now rather than waiting for the periodic resend - the whole countdown
	// is shorter than one resend interval, so without this the peers would enter the match still
	// believing those slots were open.
	TheLAN->RequestGameOptions( GenerateGameOptionsString(), true );
	lanUpdateSlotList();
	gxUpdateStartControl();
}

/// Arm the forced start: the stragglers get LanStart::FORCE_WINDOW_MS to accept.
static void gxBeginForceWindow()
{
	LanStart::beginForceWindow( s_lanStart, timeGetTime() );
	gxLobbyTrace("force window armed for %u ms", LanStart::FORCE_WINDOW_MS);
	gxLobbySay( TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:ForceStartArmed",
		L"The host is starting the game. Accept within %d seconds.",
		(Int)(LanStart::FORCE_WINDOW_MS / 1000u)) );
	gxUpdateStartControl();
}

/// Abort a countdown or a force window and hand the lobby back.
///
/// HOST ONLY, structurally rather than by politeness: the only caller from the UI is behind
/// AmIHost(), and a joiner has nothing to cancel anyway because RequestGameStartTimer refuses to
/// arm unless slot 0 is the local IP. A joiner calling ResetGameStartTimer would clear a timer it
/// never had while the host kept right on broadcasting.
static void gxCancelStart( const UnicodeString &why )
{
	LANGameInfo *myGame = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;

	gxLobbyTrace("cancel from phase=%d", (Int)s_lanStart.phase);
	LanStart::reset( s_lanStart );
	if( TheLAN != nullptr )
		TheLAN->ResetGameStartTimer();

	if( myGame != nullptr )
	{
		for( Int i = 0; i < MAX_SLOTS; i++ )
		{
			if( !s_lanStartClosedSlot[i] )
				continue;
			s_lanStartClosedSlot[i] = FALSE;

			LANGameSlot *slot = myGame->getLANSlot(i);
			if( slot != nullptr && slot->getState() == SLOT_CLOSED )
			{
				slot->setState( SLOT_OPEN );
				if( comboBoxPlayer[i] != nullptr )
					GadgetComboBoxSetSelectedPos( comboBoxPlayer[i], SLOT_OPEN );
			}
		}

		// Drop everyone back to un-accepted. This is what actually makes the lobby editable again
		// and is the reason the operator wanted a cancel at all: UpdateSlotList disables a
		// joiner's own colour/faction/team combos for exactly as long as that joiner is accepted,
		// so a "cancelled" lobby that kept the accepts would be one nobody but the host could edit.
		myGame->resetAccepted();
		TheLAN->RequestGameOptions( GenerateGameOptionsString(), true );
	}

	gxLobbySay( why );
	lanUpdateSlotList();
	gxUpdateStartControl();
}

// GeneralsX: The first start only delivered the map; LANAPI has kept
// the room screen alive (never shut down) and needs the menu's start machinery reset so the
// room is startable again. Accepts are reset by LANAPI's post-transfer options rebroadcast -
// the room comes back as a normal lobby, everyone re-readies. Start-closed slots stay closed:
// reopening seats here invites a mid-flight join that immediately forces another round.
void lanNotifyTransferOnlyStart( void )
{
	LANbuttonPushed = FALSE;
	LanStart::reset( s_lanStart );
	if (TheLAN != nullptr)
		TheLAN->ResetGameStartTimer();
	s_lanStartControlShown = -1;
	gxUpdateStartControl();
	lanUpdateSlotList();
	updateGameOptions();
}

/// One tick of the host's start policy. Called from the menu's layout update, which Shell::update
/// runs at a fixed ~30 Hz - but every decision below is taken on timeGetTime(), so the rate this
/// is polled at cannot change when anything happens.
static void gxDriveStartPhase()
{
	if( s_lanStart.phase == LanStart::PHASE_IDLE )
		return;

	// LANbuttonPushed is set once the match is genuinely on its way out of this screen; carrying
	// on past it would be driving a lobby that no longer exists.
	if( TheLAN == nullptr || !TheLAN->AmIHost() || LANisShuttingDown || LANbuttonPushed )
		return;

	LANGameInfo *myGame = TheLAN->GetMyGame();
	if( myGame == nullptr )
	{
		LanStart::reset( s_lanStart );
		s_lanStartControlShown = -1;
		return;
	}

	const Bool ready = gxEveryPeerReady( myGame );

	switch( LanStart::tick( s_lanStart, timeGetTime(), ready != FALSE ) )
	{
		case LanStart::EVENT_ANNOUNCE_FORCE:
			// s_lanStart.lastAnnounced, not a second call to remainingSeconds(): re-reading the
			// clock here can land the other side of a second boundary and print 19 for the 20 mark.
			gxLobbySay( TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("LAN:ForceStartRemaining",
				L"%d seconds left to accept.", s_lanStart.lastAnnounced) );
			break;

		case LanStart::EVENT_ALL_READY:
			gxLobbyTrace("force window: everyone accepted, cutting to the countdown");
			gxLobbySay( TheGameText->FETCH_OR_SUBSTITUTE("LAN:ForceStartEveryoneReady",
				L"Everyone is ready.") );
			gxBeginCountdown();
			break;

		case LanStart::EVENT_FORCE_EXPIRED:
			gxLobbyTrace("force window expired; accepting on behalf of the stragglers");
			gxLobbySay( TheGameText->FETCH_OR_SUBSTITUTE("LAN:ForceStartExpired",
				L"Out of time - starting without the players who did not accept.") );
			// Accept on their behalf so the slot list the match is built from agrees with what
			// just happened, and so the countdown's own "did the lobby change under me" check
			// does not immediately read the stragglers as a cancellation.
			for( Int i = 0; i < MAX_SLOTS; i++ )
			{
				LANGameSlot *slot = myGame->getLANSlot(i);
				if( slot != nullptr && slot->isHuman() )
					slot->setAccept();
			}
			gxBeginCountdown();
			break;

		case LanStart::EVENT_COUNTDOWN_LOST:
			gxLobbyTrace("countdown lost: peersReady=%d", (Int)ready);
			// Either the slot list changed - LANGameInfo::resetAccepted() cancels the engine's
			// timer from underneath us on every join, leave and slot change - or the timer simply
			// never fired. Both leave a lobby that would otherwise sit on CANCEL forever.
			gxCancelStart( TheGameText->FETCH_OR_SUBSTITUTE("LAN:StartAbortedLobbyChanged",
				L"The lobby changed - start cancelled.") );
			break;

		case LanStart::EVENT_NONE:
		default:
			break;
	}
}

void StartPressed()
{
	LANGameInfo *myGame = TheLAN->GetMyGame();

	Bool isReady = true;
	Bool allHaveMap = true;
	Int playerCount = 0;
	if (!myGame)
	{
		return;
	}
	myGame->getLANSlot(0)->setAccept(); // cause we are, of course!

	int i;

	int numUsers = 0;
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = myGame->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			numUsers++;
		}
	}

	// Check for too many players
	const MapMetaData *md = TheMapCache->findMap( myGame->getMap() );
	if (!md || md->m_numPlayers < numUsers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:TooManyPlayers"), (md)?md->m_numPlayers:0);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// GeneralsX: The map must also have room for every slot next to the
	// civilian sides it keeps (Common/PlayerCapacity.h). Refused here, never trimmed: owner decision.
	const Int slotCapacity = TheMapCache->getPlayerSlotCapacity( myGame->getMap() );
	if (slotCapacity >= 0 && myGame->countSlotsTakingPlayers() > slotCapacity)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:TooManyPlayers"), slotCapacity);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// THE START RULE is GameInfo::getStartVerdict, one function for every setup screen and inside the SimID digest. An all-AI lobby (humans only as observers) may start; retail refused it here with GUI:NeedHumanPlayers. It starts as a sandbox, counted in nobody's stats, and is announced below.
	const GameInfo::StartVerdict startVerdict = myGame->getStartVerdict();

	// Check for too few players
	if (startVerdict == GameInfo::START_NOBODY_PLAYS || numUsers < TheGlobalData->m_netMinPlayers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:NeedMorePlayers"),numUsers);
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	// Check for too few teams
	int numRandom = 0;
	std::set<Int> teams;
	for (i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = myGame->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			if (slot->getTeamNumber() >= 0)
			{
				teams.insert(slot->getTeamNumber());
			}
			else
			{
				++numRandom;
			}
		}
	}
	if (numRandom + teams.size() < TheGlobalData->m_netMinPlayers)
	{
		if (TheLAN->AmIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetchGameOnly("LAN:NeedMoreTeams"));
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
		}
		return;
	}

	if (startVerdict == GameInfo::START_SANDBOX)
	{
		UnicodeString text;
		text.format(TheGameText->fetchGameOnly("GUI:SandboxMode"));
			TheLAN->OnChat(L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM);
	}

	// see if everyone's accepted and count the number of players in the game
	UnicodeString mapDisplayName;
	const MapMetaData *mapData = TheMapCache->findMap( myGame->getMap() );
	Bool willTransfer = TRUE;
	if (mapData)
	{
		mapDisplayName.format(L"%ls", mapData->m_displayName.str());
		willTransfer = !mapData->m_isOfficial;
	}
	else
	{
		// GeneralsX: The map's NAME, never its path (owner report).
		mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(myGame->getMap()).str());
		willTransfer = WouldMapTransfer(myGame->getMap());
	}
	for( i = 0; i < MAX_SLOTS; i++ )
	{
		LANGameSlot *slot = myGame->getLANSlot(i);
		if( slot->isHuman() && !slot->isAccepted())
			isReady = false;
		// GeneralsX: EVERY human, accepted or not. Accepting never checks
		// the map, so an accepted slot without an official map passed here and the start ran into a transfer that cannot
		// happen (DoAnyMapTransfers refuses it). Since M1 2b that includes a slot whose map.ini/solo.ini differ from ours.
		if( slot->isHuman() && !willTransfer && !slot->hasMap())
		{
			UnicodeString msg;
			msg.format(TheGameText->fetchGameOnly("GUI:PlayerNoMap"), slot->getName().str(), mapDisplayName.str());
			lanGameSay(msg, chatSystemColor);
			allHaveMap = false;
		}
		if( slot->isHuman() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER )
			playerCount++;
	}

	// GeneralsX: Two ways out of here now, and both of them start a
	// match eventually. Retail's "not ready" branch only nudged - it printed NotifiedStartIntent
	// and accepted on the host's behalf, and the host then had to come back and press again. The
	// operator wants that press to mean something: it arms a bounded window and starts at the end
	// of it. Reaching this line at all is an explicit act; nothing on a refresh or restore path
	// calls StartPressed.
	gxLobbyTrace("StartPressed: isReady=%d allHaveMap=%d", (Int)isReady, (Int)allHaveMap);
	if(isReady && allHaveMap)
	{
		gxBeginCountdown();
	}
	else if (allHaveMap)
	{
		gxBeginForceWindow();
	}
	// If a human is missing a map that will not transfer, neither branch runs and the loop above
	// has already named them in the chat window. Forcing a start there would start a match one of
	// the players physically cannot load.

}

// GeneralsX: Null-guard the widgets. These are only assigned by
// LanGameOptionsMenuInit, but the caller is LANGameInfo::resetAccepted() - a STATE operation on
// the slot list that LANAPI::OnPlayerJoin performs for every join. Any path that resets accepts
// without the lobby screen being up therefore dereferenced a null window: a headless host
// crashed here the moment a peer joined. Same class as OnChat's existing chatWindow guard.
void LANEnableStartButton(Bool enabled)
{
	if (buttonStart != nullptr)
		buttonStart->winEnable(enabled);
	if (buttonSelectMap != nullptr)
		buttonSelectMap->winEnable(enabled);

	// GeneralsX: The host's Start control has a policy of its own now,
	// so re-apply it rather than letting the caller's blanket verdict stand. The caller that
	// matters is LANGameInfo::resetAccepted(), which is inside the engine and passes TRUE: after
	// it runs nobody has accepted, and an unconditionally enabled Start would be exactly the gate
	// this feature exists to close. On a joiner gxUpdateStartControl returns immediately, so the
	// ACCEPT button keeps the behaviour it has always had.
	gxUpdateStartControl();
}

// GeneralsX: After a LETTER KEY sets a slot (GadgetComboBoxKeyPick), open
// the next slot's drop-down so a lobby can be filled by typing - E, E, H, M - without the mouse.
// Only ever reached from inside the host-only slot branch, so a joiner cannot get here. Skips slots
// whose drop-down is disabled or hidden, and HUMAN slots: on one of those the next letter would
// kick the player, which should take a deliberate click, not a keystroke meant for an AI slot.
// Nothing eligible after this slot: the list simply stays closed.
static void lanOpenNextSlotDropDown( GameInfo *game, Int from )
{
	if( game == nullptr )
		return;
	for( Int j = from + 1; j < MAX_SLOTS; ++j )
	{
		GameWindow *combo = comboBoxPlayer[ j ];
		if( combo == nullptr || combo->winIsHidden() || !combo->winGetEnabled() )
			continue;
		const GameSlot *slot = game->getConstSlot( j );
		if( slot == nullptr || slot->isHuman() )
			continue;
		GadgetComboBoxOpenList( combo );
		return;
	}
}

static void handleColorSelection(int index)
{
	GameWindow *combo = comboBoxColor[index];
	Int color, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	// GeneralsX: Cast via intptr_t for 64-bit compatibility
	color = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));

	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (color == slot->getColor())
			return;

		if (color >= -1 && color < TheMultiplayerSettings->getNumColors())
		{
			Bool colorAvailable = TRUE;
			if(color != -1 )
			{
				for(Int i=0; i <MAX_SLOTS; i++)
				{
					LANGameSlot *checkSlot = myGame->getLANSlot(i);
					if(color == checkSlot->getColor() && slot != checkSlot)
					{
						colorAvailable = FALSE;
						break;
					}
				}
			}
			if(!colorAvailable)
				return;
		}

		slot->setColor(color);

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the color from the host
			if (!slot->isLocalPlayer() || !AreSlotListUpdatesEnabled())
				return;

			AsciiString options;
			options.format("Color=%d", color);
			TheLAN->RequestGameOptions(options, true);
		}
	}
}

static void handlePlayerTemplateSelection(int index)
{
	GameWindow *combo = comboBoxPlayerTemplate[index];
	Int playerTemplate, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	// GeneralsX: Cast via intptr_t for 64-bit compatibility
	playerTemplate = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (playerTemplate == slot->getPlayerTemplate())
			return;
		// GeneralsX: Only the host changes another player's slot. A joiner's
		// combos for other slots are disabled, so this only ever came from a programmatic selection,
		// and "PlayerTemplate=N" is applied by the host to the SENDER's own slot, not to `index`.
		if (!myGame->amIHost() && index != myGame->getLocalSlotNum())
			return;

		Int oldTemplate = slot->getPlayerTemplate();
		slot->setPlayerTemplate(playerTemplate);

		if (oldTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// was observer, so populate color & team with all, and enable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
		}
		else if (playerTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// is becoming observer, so populate color & team with random only, and disable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
		}

		myGame->resetAccepted();

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the playerTemplate from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("PlayerTemplate=%d", playerTemplate);
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}

static void handleStartPositionSelection(Int player, int startPos)
{
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(player);
		if (startPos == slot->getStartPos())
			return;
		Bool skip = FALSE;
		if (startPos < 0)
		{
			skip = TRUE;
		}

		if(!skip)
		{
			Bool isAvailable = TRUE;
			for(Int i = 0; i < MAX_SLOTS; ++i)
			{
				if(i != player && myGame->getSlot(i)->getStartPos() == startPos)
				{
					isAvailable = FALSE;
					break;
				}
			}
			if( !isAvailable )
				return;
		}
		slot->setStartPos(startPos);

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				myGame->resetAccepted();
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the color from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("StartPos=%d", slot->getStartPos());
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}



static void handleTeamSelection(int index)
{
	GameWindow *combo = comboBoxTeam[index];
	Int team, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	// GeneralsX: Cast via intptr_t for 64-bit compatibility
	team = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(combo, selIndex)));
	LANGameInfo *myGame = TheLAN->GetMyGame();

	if (myGame)
	{
		LANGameSlot * slot = myGame->getLANSlot(index);
		if (team == slot->getTeamNumber())
			return;

		slot->setTeamNumber(team);
		myGame->resetAccepted();

		if (myGame->amIHost())
		{
			if (!s_isIniting)
			{
				// send around a new slotlist
				myGame->resetAccepted();
				TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
				lanUpdateSlotList();
			}
		}
		else
		{
			// request the team from the host
			if (AreSlotListUpdatesEnabled())
			{
				AsciiString options;
				options.format("Team=%d", team);
				TheLAN->RequestGameOptions(options, true);
			}
		}
	}
}

static void handleStartingCashSelection()
{
  LANGameInfo *myGame = TheLAN->GetMyGame();

  if (myGame)
  {
    Int selIndex;
    GadgetComboBoxGetSelectedPos(comboBoxStartingCash, &selIndex);

    Money startingCash;
    // GeneralsX: Cast via uintptr_t for 64-bit compatibility
    startingCash.deposit( static_cast<UnsignedInt>(reinterpret_cast<uintptr_t>(GadgetComboBoxGetItemData( comboBoxStartingCash, selIndex ))), FALSE, FALSE );
    myGame->setStartingCash( startingCash );
    myGame->resetAccepted();

    if (myGame->amIHost())
    {
      if (!s_isIniting)
      {
        // send around the new data
        TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
        lanUpdateSlotList(); // Update the accepted button UI
      }
    }
  }
}

static void handleLimitSuperweaponsClick()
{
  LANGameInfo *myGame = TheLAN->GetMyGame();

  if (myGame)
  {
    // At the moment, 1 and 0 are the only choices supported in the GUI, though the system could
    // support more.
    if ( GadgetCheckBoxIsChecked( checkboxLimitSuperweapons ) )
    {
      myGame->setSuperweaponRestriction( 1 );
    }
    else
    {
      myGame->setSuperweaponRestriction( 0 );
    }
    myGame->resetAccepted();

    if (myGame->amIHost())
    {
      if (!s_isIniting)
      {
        // send around a new slotlist
        TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
        lanUpdateSlotList(); // Update the accepted button UI
      }
    }
  }
}

/// Rebuild every faction list from the game's current Limit Armies state (GUIUtil filters on it), each
/// left on its slot's own faction (keepSlotPick): entry 0 would reach handlePlayerTemplateSelection and
/// put every slot on Random - on a joiner, as a PlayerTemplate=-1 request per slot.
static void lanRefreshArmyCombos( LANGameInfo *game )
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, game, slot == nullptr || !slot->isAI(), TRUE);
	}
}

/// Host only. Turning it on puts every slot on a general's sub-faction back on Random (EnforceArmyLimit)
/// and sends the new options round, exactly like Limit Superweapons. A joiner's copy is disabled and
/// only ever set from the host's options (updateGameOptions), which also lands here - hence the guard.
static void handleLimitArmiesClick()
{
	LANGameInfo *myGame = TheLAN->GetMyGame();
	if (!myGame || !myGame->amIHost() || checkboxLimitArmies == nullptr)
		return;

	const Bool on = GadgetCheckBoxIsChecked( checkboxLimitArmies );
	myGame->setOldFactionsOnly( on );
	EnforceArmyLimit( myGame );
	lanRefreshArmyCombos( myGame );
	myGame->resetAccepted();

	if (!s_isIniting)
	{
		TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
		lanUpdateSlotList();

		// Remembered now, not only when a match starts (LANAPI::OnGameStart): a host who changes it and
		// goes Back keeps the choice. LANPreferences is the one place both entrances read it from - this
		// screen's Init and the relay host popup. NOT CustomMatchPreferences: its constructor reads
		// TheGameSpyInfo, which is null here (never created for LAN, torn down while a relay room is open).
		LANPreferences lanPref;
		lanPref.setBool( "LimitArmies", on );
		lanPref.write();
	}
}

// The LAN host's armies survive a match, a re-host and a restart, so they are stored in LANPreferences (Network.ini).
// LobbySlots holds armies by slot plus the host's team and start position, written as soon as the host changes them.
// LobbySlotsMap is the map those positions were picked on. In this file because LANAPI* is in the digest.
static AsciiString s_lanSlotMemoryWritten;	///< the room's last state as compared by lanSlotMemoryUpdate; "" = no armies

/// A room hosted from the NETWORK screen. A relay room and a Direct Connect room both come in with isDirectConnect set, and
/// both are held open for someone about to join: a remembered full house would refuse them.
static Bool lanSlotMemoryApplies( LANGameInfo *game )
{
	return game != nullptr && game->amIHost() && !game->getIsDirectConnect();
}

static std::vector<GXLobbySlotMemory::Seat> lanSlotMemorySeats( const LANGameInfo *game )
{
	std::vector<GXLobbySlotMemory::Seat> seats;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot == nullptr)
			continue;

		char kind;
		if (i == 0)
			kind = GXLobbySlotMemory::KIND_HOST;
		else if (slot->getState() == SLOT_EASY_AI)
			kind = GXLobbySlotMemory::KIND_EASY;
		else if (slot->getState() == SLOT_MED_AI)
			kind = GXLobbySlotMemory::KIND_MEDIUM;
		else if (slot->getState() == SLOT_BRUTAL_AI)
			kind = GXLobbySlotMemory::KIND_HARD;
		else
			continue;

		GXLobbySlotMemory::Seat seat = { i, kind, slot->getColor(), slot->getPlayerTemplate(), slot->getStartPos(), slot->getTeamNumber() };
		if (i == 0)
			seat.color = seat.playerTemplate = -1;	// they have preferences of their own
		seats.push_back( seat );
	}
	return seats;
}

/// The room as lanSlotMemoryUpdate compares it. The map is part of it only while there are armies to go with it.
static AsciiString lanSlotMemoryState( const LANGameInfo *game )
{
	AsciiString state( GXLobbySlotMemory::encode( lanSlotMemorySeats( game ) ).c_str() );
	if (state.isNotEmpty())
	{
		state.concat( '|' );
		state.concat( game->getMap() );
	}
	return state;
}

/// The same test as the faction list a host picks from (PopulatePlayerTemplateComboBox), less Limit Armies, which Init
/// applies to every slot next. Observer is left out on purpose: an army's list does not offer it.
static bool lanSlotMemoryFactionOk( int playerTemplate )
{
	const PlayerTemplate *fac = (playerTemplate >= 0 && playerTemplate < ThePlayerTemplateStore->getPlayerTemplateCount())
		? ThePlayerTemplateStore->getNthPlayerTemplate( playerTemplate ) : nullptr;
	if (fac == nullptr || fac->getStartingBuilding().isEmpty())
		return false;
	const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName( fac->getName() );
	return general == nullptr || general->isStartingEnabled();
}

/// Called once per room, before adjustSlotsForMap counts what is occupied. Returns the armies the map had no room for.
/// An unknown map restores nothing: its capacity is what keeps every army on a start position.
static Int lanSlotMemoryRestore( LANGameInfo *game, const LANPreferences &pref, Int *armiesKept )
{
	*armiesKept = 0;
	const AsciiString saved = pref.getAsciiString( "LobbySlots", AsciiString::TheEmptyString );
	const MapMetaData *md = saved.isEmpty() ? nullptr : TheMapCache->findMap( game->getMap() );
	if (md == nullptr)
		return 0;

	GXLobbySlotMemory::Limits limits;
	limits.maxSlots = MAX_SLOTS;
	limits.numStarts = md->m_numPlayers;
	limits.capacity = md->m_numPlayers;
	// The same second number StartPressed refuses on. Refusing is right for what a host just chose, not for what a host chose
	// on some other map: this would open a room whose Start button can only complain.
	const Int slotCapacity = TheMapCache->getPlayerSlotCapacity( game->getMap() );
	if (slotCapacity >= 0 && slotCapacity < limits.capacity)
		limits.capacity = slotCapacity;
	limits.numColors = TheMultiplayerSettings->getNumColors();
	limits.numTeams = MAX_SLOTS/2;
	limits.sameMap = QuotedPrintableToAsciiString( pref.getAsciiString( "LobbySlotsMap", AsciiString::TheEmptyString ) )
		.compareNoCase( game->getMap() ) == 0;
	limits.hostColor = game->getConstSlot(0)->getColor();
	limits.templateOk = lanSlotMemoryFactionOk;

	const GXLobbySlotMemory::Fit fit = GXLobbySlotMemory::fit( GXLobbySlotMemory::parse( saved.str() ), limits );
	for (size_t i = 0; i < fit.seats.size(); ++i)
	{
		const GXLobbySlotMemory::Seat &seat = fit.seats[i];
		LANGameSlot *slot = game->getLANSlot( seat.slot );
		if (slot == nullptr)
			continue;
		if (seat.kind != GXLobbySlotMemory::KIND_HOST)
		{
			// First: GameSlot::setState clears the rest of a slot that was not an AI already.
			slot->setState( seat.kind == GXLobbySlotMemory::KIND_EASY ? SLOT_EASY_AI
				: seat.kind == GXLobbySlotMemory::KIND_MEDIUM ? SLOT_MED_AI : SLOT_BRUTAL_AI );
			slot->setColor( seat.color );
			slot->setPlayerTemplate( seat.playerTemplate );
		}
		slot->setStartPos( seat.startPos );
		slot->setTeamNumber( seat.team );
	}
	*armiesKept = fit.armiesKept;
	return fit.armiesDropped;
}

/// Every change of the room reaches this through lanUpdateSlotList, which also runs on joins and accepts, so most calls find
/// nothing new. It stays quiet until the room differs from how Init left it: opening a room on a smaller map keeps the full
/// list on disk for the next larger one, until the host actually changes something.
static void lanSlotMemoryUpdate( LANGameInfo *game )
{
	if (!lanSlotMemoryApplies( game ) || game->isGameInProgress())
		return;
	const AsciiString state = lanSlotMemoryState( game );
	if (state == s_lanSlotMemoryWritten)
		return;
	s_lanSlotMemoryWritten = state;

	LANPreferences pref;
	if (state.isEmpty())
	{
		pref.erase( "LobbySlots" );
		pref.erase( "LobbySlotsMap" );
	}
	else
	{
		pref.setAsciiString( "LobbySlots", GXLobbySlotMemory::encode( lanSlotMemorySeats( game ) ).c_str() );
		const AsciiString map = AsciiStringToQuotedPrintable( game->getMap() );
		pref.setAsciiString( "LobbySlotsMap", map );
		// The armies are only worth anything with the map that holds them. Left at the last map a match started on, hosting
		// again after Back would trim 11 armies to fit it.
		pref["Map"] = map;
	}
	pref.write();
}

void lanUpdateSlotList()
{
	if(!AreSlotListUpdatesEnabled() || s_isIniting)
		return;
	UpdateSlotList( TheLAN->GetMyGame(), comboBoxPlayer, comboBoxColor,
		comboBoxPlayerTemplate, comboBoxTeam, buttonAccept, buttonStart, buttonMapStartPosition);

	updateMapStartSpots(TheLAN->GetMyGame(), buttonMapStartPosition);

	// GeneralsX: This is the one function every accept, join, leave and
	// option change funnels through, so it is where the host's Start gate is kept honest. It is a
	// pure refresh: gxUpdateStartControl only reads the slot list and never starts anything.
	gxUpdateStartControl();

	lanSlotMemoryUpdate( TheLAN->GetMyGame() );
}

//-------------------------------------------------------------------------------------------------
/** Initialize the Gadgets Options Menu */
//-------------------------------------------------------------------------------------------------
void InitLanGameGadgets()
{
	//Initialize the gadget IDs
	parentLanGameOptionsID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:LanGameOptionsMenuParent" );
	buttonBackID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonBack" );
	buttonStartID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonStart" );
	textEntryChatID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:TextEntryChat" );
	textEntryMapDisplayID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:TextEntryMapDisplay" );
	listboxChatWindowLanGameID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ListboxChatWindowLanGame" );
	buttonEmoteID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonEmote" );
	buttonSelectMapID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ButtonSelectMap" );
  checkboxLimitSuperweaponsID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:CheckboxLimitSuperweapons" );
  comboBoxStartingCashID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:ComboBoxStartingCash" );
	windowMapID = TheNameKeyGenerator->nameToKey( "LanGameOptionsMenu.wnd:MapWindow" );

	// Initialize the pointers to our gadgets
	parentLanGameOptions = TheWindowManager->winGetWindowFromId( nullptr, parentLanGameOptionsID );
	DEBUG_ASSERTCRASH(parentLanGameOptions, ("Could not find the parentLanGameOptions"));
	buttonEmote = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonEmoteID  );
	DEBUG_ASSERTCRASH(buttonEmote, ("Could not find the buttonEmote"));
	buttonSelectMap = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonSelectMapID  );
	DEBUG_ASSERTCRASH(buttonSelectMap, ("Could not find the buttonSelectMap"));
	buttonStart = TheWindowManager->winGetWindowFromId( parentLanGameOptions,buttonStartID  );
	DEBUG_ASSERTCRASH(buttonStart, ("Could not find the buttonStart"));
	buttonBack = TheWindowManager->winGetWindowFromId( parentLanGameOptions,  buttonBackID);
	DEBUG_ASSERTCRASH(buttonBack, ("Could not find the buttonBack"));
	listboxChatWindowLanGame = TheWindowManager->winGetWindowFromId( parentLanGameOptions, listboxChatWindowLanGameID );
	GXChatBoxUseTimeColumn(listboxChatWindowLanGame);	// GeneralsX @feature the gray time column, before the first row
	DEBUG_ASSERTCRASH(listboxChatWindowLanGame, ("Could not find the listboxChatWindowLanGame"));
	textEntryChat = TheWindowManager->winGetWindowFromId( parentLanGameOptions, textEntryChatID );
	DEBUG_ASSERTCRASH(textEntryChat, ("Could not find the textEntryChat"));
	textEntryMapDisplay = TheWindowManager->winGetWindowFromId( parentLanGameOptions, textEntryMapDisplayID );
	DEBUG_ASSERTCRASH(textEntryMapDisplay, ("Could not find the textEntryMapDisplay"));
  checkboxLimitSuperweapons = TheWindowManager->winGetWindowFromId( parentLanGameOptions, checkboxLimitSuperweaponsID );
  DEBUG_ASSERTCRASH(checkboxLimitSuperweapons, ("Could not find the checkboxLimitSuperweapons"));
  comboBoxStartingCash = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxStartingCashID );
  DEBUG_ASSERTCRASH(comboBoxStartingCash, ("Could not find the comboBoxStartingCash"));
	PopulateStartingCashComboBox(comboBoxStartingCash, TheLAN->GetMyGame());

	// GeneralsX: Limit Armies, to the RIGHT of Limit Superweapons: on the
	// retail 800x600 layout that strip (x 612..754, y 331..355) is empty - Select Map ends at y 317 and
	// the chat list starts at y 364. Wearing the retail checkbox's own art (CodeGuiCheckboxLike), and
	// labelled from the retail string table: GUI:LimitArmies / TOOLTIP:LimitArmies ship in every
	// language's CSF for the GameSpy host popup; the English literal is only a fallback.
	checkboxLimitArmies = nullptr;
	if (checkboxLimitSuperweapons != nullptr && checkboxLimitSuperweapons->winGetParent() != nullptr)
	{
		Int sx = 0, sy = 0, sw = 0, sh = 0, pw = 0, ph = 0;
		checkboxLimitSuperweapons->winGetPosition( &sx, &sy );
		checkboxLimitSuperweapons->winGetSize( &sw, &sh );
		checkboxLimitSuperweapons->winGetParent()->winGetSize( &pw, &ph );
		const Int gap = CodeGuiAtLeast( CodeGuiScaleX( 4 ), 2 );
		const Int x = sx + sw + gap;
		const Int room = pw - x - gap;
		const Int w = (room < sw) ? room : sw;
		if (w > 0)
		{
			const UnicodeString label = TheGameText->FETCH_OR_SUBSTITUTE( "GUI:LimitArmies", L"Limit Armies" );
			const UnicodeString tip = TheGameText->FETCH_OR_SUBSTITUTE( "TOOLTIP:LimitArmies",
				L"Only USA, China and GLA may be chosen - no generals." );
			checkboxLimitArmies = CodeGuiCheckboxLike( checkboxLimitSuperweapons,
				"LanGameOptionsMenu.wnd:CheckboxLimitSuperweapons", "LanGameOptionsMenu.wnd:CheckboxLimitArmies",
				x, sy, w, label.str(), tip.str() );
		}
	}

	windowMap = TheWindowManager->winGetWindowFromId( parentLanGameOptions,windowMapID  );
	DEBUG_ASSERTCRASH(windowMap, ("Could not find the LanGameOptionsMenu.wnd:MapWindow" ));

	Int localSlotNum = TheLAN->GetMyGame()->getLocalSlotNum();
	DEBUG_ASSERTCRASH(localSlotNum >= 0, ("Bad slot number!"));

	//Tooltip function is being set for techBuildings, and supplyDocks
	windowMap->winSetTooltipFunc(MapSelectorTooltip);

	// The .wnd authors eight of every per-slot control but the loop binds MAX_SLOTS and uses the results unchecked,
	// so build what the layout lacks BEFORE any row lookup.
	// Seat columns scroll; rows take one row from the top of the chat history if it keeps the password row plus
	// LAN_CHAT_KEEP_LINES lines, else the authored eight stay.
	static const char * const lanSeatColumns[] =
	{
		"LanGameOptionsMenu.wnd:ComboBoxPlayer%d",
		"LanGameOptionsMenu.wnd:ComboBoxColor%d",
		"LanGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d",
		"LanGameOptionsMenu.wnd:ComboBoxTeam%d",
		"LanGameOptionsMenu.wnd:ButtonAccept%d",
	};
	enum { LAN_CHAT_KEEP_LINES = 3 };
	Int chatKeep = 0;
	if( listboxChatWindowLanGame != nullptr )
	{
		const Int rowGap = CodeGuiAtLeast( CodeGuiScaleY( 4 ), 3 );
		const Int lineH = TheWindowManager->winFontHeight( listboxChatWindowLanGame->winGetFont() ) + 1;	// computeTotalHeight's row
		chatKeep = lanPasswordRowHeight() + rowGap + LAN_CHAT_KEEP_LINES * lineH + rowGap;
	}
	CodeGuiRowsScrollSetup( parentLanGameOptions, lanSeatColumns, (Int)( sizeof( lanSeatColumns ) / sizeof( lanSeatColumns[ 0 ] ) ), MAX_SLOTS,
													listboxChatWindowLanGame, 1, chatKeep, listboxChatWindowLanGame );
	CodeGuiRowsEnsure( parentLanGameOptions, "LanGameOptionsMenu.wnd:ButtonMapStartPosition%d", MAX_SLOTS, CODEGUI_ROWS_FREE );

	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		AsciiString tmpString;
		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxPlayer%d", i);
		comboBoxPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayer[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxPlayerID[i] );
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (comboBoxPlayer[i])
		{
			GadgetComboBoxReset(comboBoxPlayer[i]);
			GadgetComboBoxGetEditBox(comboBoxPlayer[i])->winSetTooltipFunc(playerTooltip);

			if(localSlotNum != i)
			{
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Open"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Closed"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:EasyAI"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:MediumAI"),white);
				GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:HardAI"),white);
				GadgetComboBoxSetSelectedPos(comboBoxPlayer[i],0);
			}
		}
		/*
		if(i != 0)
		{
			TheLAN->GetMyGame()->getLANSlot(i)->setState(SLOT_OPEN);
		}
		*/

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxColor%d", i);
		comboBoxColorID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxColor[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxColorID[i] );
		DEBUG_ASSERTCRASH(comboBoxColor[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxColor[%d]",i ));
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (comboBoxColor[i])
		{
			PopulateColorComboBox(i, comboBoxColor, TheLAN->GetMyGame());
			GadgetComboBoxSetSelectedPos(comboBoxColor[i], 0);
		}

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d", i);
		comboBoxPlayerTemplateID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayerTemplate[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxPlayerTemplateID[i] );
		DEBUG_ASSERTCRASH(comboBoxPlayerTemplate[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxPlayerTemplate[%d]",i ));
		PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, TheLAN->GetMyGame(), TRUE);

		// add tooltips to the player template combobox and listbox
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (comboBoxPlayerTemplate[i])
		{
			comboBoxPlayerTemplate[i]->winSetTooltipFunc(playerTemplateComboBoxTooltip);
			GadgetComboBoxGetListBox(comboBoxPlayerTemplate[i])->winSetTooltipFunc(playerTemplateListBoxTooltip);
		}

		tmpString.format("LanGameOptionsMenu.wnd:ComboBoxTeam%d", i);
		comboBoxTeamID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxTeam[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, comboBoxTeamID[i] );
		DEBUG_ASSERTCRASH(comboBoxTeam[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the comboBoxTeam[%d]",i ));
		PopulateTeamComboBox(i, comboBoxTeam, TheLAN->GetMyGame());

		tmpString.clear();
		tmpString.format("LanGameOptionsMenu.wnd:ButtonAccept%d", i);
		buttonAcceptID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonAccept[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonAcceptID[i] );
		DEBUG_ASSERTCRASH(buttonAccept[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the buttonAccept[%d]",i ));
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		if (buttonAccept[i])
			buttonAccept[i]->winSetTooltipFunc(gameAcceptTooltip);
//
//		tmpString.format("LanGameOptionsMenu.wnd:ButtonStartPosition%d", i);
//		buttonStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
//		buttonStartPosition[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonStartPositionID[i] );
//		DEBUG_ASSERTCRASH(buttonStartPosition[i], ("Could not find the ButtonStartPosition[%d]",i ));

		tmpString.format("LanGameOptionsMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( parentLanGameOptions, buttonMapStartPositionID[i] );
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i] || i >= LEGACY_WND_SLOT_ROWS, ("Could not find the ButtonMapStartPosition[%d]",i ));

		if(i !=0 && buttonAccept[i])
			buttonAccept[i]->winHide(TRUE);
	}
	if( buttonAccept[0] )
		GadgetButtonSetEnabledColor(buttonAccept[0], acceptTrueColor );

	// GeneralsX: Built last, because it measures the chat listbox and then
	// resizes it - anything that repositions the list after this would leave the row floating.
	lanPasswordRowCreate();
}

void DeinitLanGameGadgets()
{
	// Forget the code-built row FIRST. Its windows are children of the chat listbox's parent and are
	// torn down with the layout; what must not survive is a dangling pointer into freed memory.
	lanPasswordRowReset();

	parentLanGameOptions = nullptr;
	buttonEmote = nullptr;
	buttonSelectMap = nullptr;
	buttonStart = nullptr;
	buttonBack = nullptr;
	listboxChatWindowLanGame = nullptr;
	textEntryChat = nullptr;
	textEntryMapDisplay = nullptr;
  checkboxLimitSuperweapons = nullptr;
	checkboxLimitArmies = nullptr;
  comboBoxStartingCash = nullptr;
	if (windowMap)
	{
		windowMap->winSetUserData(nullptr);
		windowMap = nullptr;
	}
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		comboBoxPlayer[i] = nullptr;
		comboBoxColor[i] = nullptr;
		comboBoxPlayerTemplate[i] = nullptr;
		comboBoxTeam[i] = nullptr;
		buttonAccept[i] = nullptr;
//		buttonStartPosition[i] = nullptr;
		buttonMapStartPosition[i] = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the Lan Game Options Menu */
//-------------------------------------------------------------------------------------------------
void LanGameOptionsMenuInit( WindowLayout *layout, void *userData )
{
	if (TheLAN->GetMyGame() && TheLAN->GetMyGame()->isGameInProgress())
	{
		// If we init while the game is in progress, we are really returning to the menu
		// after the game.  So, we pop the menu and go back to the lobby.  Whee!
		DEBUG_LOG(("Popping to lobby after a game!"));
		TheShell->popImmediate();
		return;
	}
	s_isIniting = TRUE;

	// The heading is re-resolved and re-written for this visit. Both must be cleared, not just the
	// window: keeping the last-shown string would suppress the first write on a screen that has
	// been reopened, leaving whatever the layout ships with ("GAME OPTIONS") on a named lobby.
	staticTextLobbyTitle = nullptr;
	s_lobbyTitleShown.clear();

	LANbuttonPushed = false;
	LANisShuttingDown = false;

	// GeneralsX: A fresh lobby is never mid-start. These are file
	// statics that outlive the screen, so without this a lobby left during a countdown would
	// reopen showing CANCEL - and worse, a start is never implied by entering a screen.
	LanStart::reset( s_lanStart );
	s_lanStartControlShown = -1;
	for( Int resetSlot = 0; resetSlot < MAX_SLOTS; ++resetSlot )
		s_lanStartClosedSlot[resetSlot] = FALSE;

	//initialize the gadgets
	EnableSlotListUpdates(FALSE);
	InitLanGameGadgets();
	EnableSlotListUpdates(TRUE);
	Int start = 0;
	Int armiesKept = 0;
	Int armiesNoRoom = 0;

	// Make sure the text fields are clear
	GadgetListBoxReset( listboxChatWindowLanGame );
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	//The dialog needs to react differently depending on whether it's the host or not.
	TheMapCache->updateCache();
	if (TheLAN->AmIHost())
	{
		// read in some prefs
		LANGameInfo *game = TheLAN->GetMyGame();
		LANGameSlot *slot = game->getLANSlot(0);
		LANPreferences pref;
		slot->setColor( pref.getPreferredColor() );
		slot->setPlayerTemplate( pref.getPreferredFaction() );
		slot->setNATBehavior(FirewallHelperClass::FIREWALL_TYPE_SIMPLE);
		game->setMap( pref.getPreferredMap() );
    game->setStartingCash( pref.getStartingCash() );
    game->setSuperweaponRestriction( pref.getSuperweaponRestricted() ? 1 : 0 );
		game->setOldFactionsOnly( pref.getBool( "LimitArmies", FALSE ) );
		if (lanSlotMemoryApplies( game ))
			armiesNoRoom = lanSlotMemoryRestore( game, pref, &armiesKept );
		EnforceArmyLimit( game );	// the host's own remembered faction may be a general, and so may a remembered army's
		if (armiesKept > 0)
			lanRefreshArmyCombos( game );	// they were filled for humans, and an army's list has no Observer
		AsciiString lowerMap = pref.getPreferredMap();
		lowerMap.toLower();
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(lowerMap);
		if (it != TheMapCache->end())
		{
			TheLAN->GetMyGame()->getSlot(0)->setMapAvailability(true);
			TheLAN->GetMyGame()->setMapCRC( it->second.m_CRC );
			TheLAN->GetMyGame()->setMapSize( it->second.m_filesize );

			TheLAN->GetMyGame()->adjustSlotsForMap(); // BGC- adjust the slots for the selected map.
		}

		//GadgetTextEntrySetText(comboBoxPlayer[0], TheLAN->GetMyName());
		lanUpdateSlotList();
		updateGameOptions();
		start = 1; // leave my combo boxes usable

		// TheSuperHackers @tweak disable the combo box for the host's player name
		comboBoxPlayer[0]->winEnable(FALSE);
	}
	else
	{

		//DEBUG_LOG(("LanGameOptionsMenuInit(): map is %s", TheLAN->GetMyGame()->getMap().str()));
		buttonStart->winSetText(TheGameText->fetch("GUI:Accept"));
		buttonSelectMap->winEnable( FALSE );
    checkboxLimitSuperweapons->winEnable( FALSE ); // Can look but only host can touch
		if (checkboxLimitArmies)
			checkboxLimitArmies->winEnable( FALSE );     // Ditto
    comboBoxStartingCash->winEnable( FALSE );      // Ditto
		TheLAN->GetMyGame()->setMapCRC( TheLAN->GetMyGame()->getMapCRC() );		// force a recheck
		TheLAN->GetMyGame()->setMapSize( TheLAN->GetMyGame()->getMapSize() ); // of if we have the map
		TheLAN->RequestHasMap();
		lanUpdateSlotList();
		updateGameOptions();
	}
	for (Int i = start; i < MAX_SLOTS; ++i)
	{
		// GeneralsX: slots 8+ have no .wnd control until the lobby rebuild lands
		//I'm a client, disable the controls I can't touch.
		if (!TheLAN->AmIHost() && comboBoxPlayer[i])
			comboBoxPlayer[i]->winEnable(FALSE);

		if (comboBoxColor[i])
			comboBoxColor[i]->winEnable(FALSE);
		if (comboBoxPlayerTemplate[i])
			comboBoxPlayerTemplate[i]->winEnable(FALSE);
		if (comboBoxTeam[i])
			comboBoxTeam[i]->winEnable(FALSE);
//		buttonStartPosition[i]->winEnable(FALSE);
	}

//	for (i = 0; i < MAX_SLOTS; ++i)
//	{
//		if (buttonStartPosition[i])
//			buttonStartPosition[i]->winHide(TRUE); // not picking start spots this way any more
//	}
//
	// Show the Menu
	layout->hide( FALSE );

	// Set Keyboard to Main Parent
	TheWindowManager->winSetFocus( parentLanGameOptions );

	s_isIniting = FALSE;

	if (TheLAN->AmIHost())
	{
		TheLAN->RequestGameOptions(GenerateGameOptionsString(),true);
		TheLAN->RequestGameAnnounce();
	}
	// Before the first refresh below, which is what would otherwise write the room as Init left it (lanSlotMemoryUpdate).
	if (lanSlotMemoryApplies( TheLAN->GetMyGame() ))
		s_lanSlotMemoryWritten = lanSlotMemoryState( TheLAN->GetMyGame() );
	lanUpdateSlotList();
	if (armiesNoRoom > 0)
	{
		UnicodeString text;
		text.format( L"This map has room for %d of your %d remembered armies.", armiesKept, armiesKept + armiesNoRoom );
		TheLAN->OnChat( L"SYSTEM", TheLAN->GetLocalIP(), text, LANAPI::LANCHAT_SYSTEM );
	}
	LanPositionStartSpots();
	TheTransitionHandler->setGroup("LanGameOptionsFade");

	// animate controls
	//TheShell->registerWithAnimateManager(buttonBack, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 1);

}

//-------------------------------------------------------------------------------------------------
/** Update options on screen */
//-------------------------------------------------------------------------------------------------
void updateGameOptions()
{
	LANGameInfo *theGame = TheLAN->GetMyGame();
	UnicodeString mapDisplayName;
	if (theGame && AreSlotListUpdatesEnabled())
	{
		const GameSlot *localSlot = theGame->getConstSlot(theGame->getLocalSlotNum());
		const MapMetaData *mapData = TheMapCache->findMap( TheLAN->GetMyGame()->getMap() );
		if (mapData && localSlot && localSlot->hasMap())
		{
			mapDisplayName.format(L"%ls", mapData->m_displayName.str());
		}
		else
		{
			// GeneralsX: The old leaf hack only knew backslashes, so on a
			// POSIX joiner - whose map path is forward-slashed - the label under the map preview
			// showed the FULL PATH of a map the player does not even have yet (owner report).
			// getMapLeafName handles both separators.
			mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(TheLAN->GetMyGame()->getMap()).str());
		}
		UnicodeString old = GadgetStaticTextGetText(textEntryMapDisplay);
		if(old.compare(mapDisplayName) != 0)
			LanPositionStartSpots();
		GadgetStaticTextSetText(textEntryMapDisplay, mapDisplayName);

    GadgetCheckBoxSetChecked( checkboxLimitSuperweapons, theGame->getSuperweaponRestriction() != 0 );
		// Only on a change, and then the faction lists are wrong too (retail WOLGameSetupMenu pattern).
		if (checkboxLimitArmies && GadgetCheckBoxIsChecked( checkboxLimitArmies ) != theGame->oldFactionsOnly())
		{
			GadgetCheckBoxSetChecked( checkboxLimitArmies, theGame->oldFactionsOnly() );
			lanRefreshArmyCombos( theGame );
			lanUpdateSlotList();
		}
		Int itemCount = GadgetComboBoxGetLength(comboBoxStartingCash);
    Int index = 0;
    for ( ; index < itemCount; index++ )
    {
      // GeneralsX: Cast via intptr_t for 64-bit compatibility
      Int value  = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(comboBoxStartingCash, index)));
      if ( value == theGame->getStartingCash().countMoney() )
      {
        GadgetComboBoxSetSelectedPos(comboBoxStartingCash, index, TRUE);
        break;
      }
    }

    DEBUG_ASSERTCRASH( index < itemCount, ("Could not find new starting cash amount %d in list", theGame->getStartingCash().countMoney() ) );
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
void setLANPlayerTooltip(LANPlayer* player)
{
	UnicodeString tooltip;

	if (!player->getLogin().isEmpty() || !player->getHost().isEmpty())
	{
		tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:LANPlayer"), player->getLogin().str(), player->getHost().str());
	}

#if defined(RTS_DEBUG)
	UnicodeString ip;
	ip.format(L" - %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(player->getIP()));
	tooltip.concat(ip);
#endif

	if (!tooltip.isEmpty())
	{
		TheMouse->setCursorTooltip( tooltip );
	}
}


//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{
	DeinitLanGameGadgets();
	textEntryMapDisplay = nullptr;
	LANisShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (LANnextScreen != nullptr) );

	if (LANnextScreen != nullptr)
	{
		TheShell->push(LANnextScreen);
	}

	LANnextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu shutdown method */
//-------------------------------------------------------------------------------------------------
void LanGameOptionsMenuShutdown( WindowLayout *layout, void *userData )
{
	TheMouse->setCursor(Mouse::ARROW);
	TheMouse->setMouseText(UnicodeString::TheEmptyString,nullptr,nullptr);
	EnableSlotListUpdates(FALSE);
	LANisShuttingDown = true;

	// The map selector is a top-level layout the Shell does not own, so Shell::doPop's destroyWindows() misses
	// it; leaving with it open would leave its windows drawn and hit-tested. Above the popImmediate early return.
	if( mapSelectLayout )
	{
		mapSelectLayout->destroyWindows();
		deleteInstance(mapSelectLayout);
		mapSelectLayout = nullptr;
	}

	// GeneralsX: Above the popImmediate early return on purpose - that
	// is the branch taken when the match actually starts, and it is the one that most needs the
	// phase cleared before the screen comes back after the game.
	LanStart::reset( s_lanStart );
	s_lanStartControlShown = -1;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	TheTransitionHandler->reverse("LanGameOptionsFade");
	if (TheLAN)
		TheLAN->ResetGameStartTimer();

	/*
	// hide menu
	layout->hide( TRUE );

	// Reset the LAN singleton
//	TheLAN->reset();

	// our shutdown is complete
	TheShell->shutdownComplete( layout );
	*/
}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu update method */
/// Put the lobby's name over the slot list instead of "GAME OPTIONS".
///
/// Refreshed because a joiner's name arrives with the slot list, after Init; written only when it changes.
/// Falls back to the stock GUI:GameOptions label when there is no name.
static void gxRefreshLobbyTitle( void )
{
	if (staticTextLobbyTitle == nullptr)
	{
		staticTextLobbyTitle = TheWindowManager->winGetWindowFromId(
			nullptr, TheNameKeyGenerator->nameToKey("LanGameOptionsMenu.wnd:StaticTextTitle"));
		if (staticTextLobbyTitle == nullptr)
		{
			return;
		}
	}

	// NOT getName() - that is the lookup key ("%8.8X%8.8X" of IP and seed), not a display name.
	// getLobbyName() is the human one, carried as "LN=" in the announce, so a JOINER sees the
	// host's title too rather than only the host seeing their own.
	UnicodeString want;
	LANGameInfo *myGame = (TheLAN != nullptr) ? TheLAN->GetMyGame() : nullptr;
	if (myGame != nullptr)
	{
		// GeneralsX: AN UNNAMED ROOM IS CALLED BY ITS HOST, as the browser
		// row calls it ("[eu] host", WOLLobbyMenu.cpp) and as PopupJoinGame's label does. Most rooms
		// are unnamed - every host who never typed one - and those kept the words GAME OPTIONS,
		// which is what was reported. Slot 0 is the host in a LAN/relay game.
		UnicodeString name;
		if (!myGame->getLobbyName().isEmpty())
		{
			name.translate(myGame->getLobbyName());
		}
		else
		{
			const GameSlot *hostSlot = myGame->getConstSlot(0);
			if (hostSlot != nullptr && hostSlot->isHuman())
				name = hostSlot->getName();
		}

		if (!name.isEmpty())
		{
			// The region is applied HERE rather than carried on the wire: every peer in a lobby is on
			// the same relay and already knows its own region, so sending it would spend announce bytes
			// on a value the receiver can derive. Transport::getRelayRegion() is the single runtime
			// source - the browser's own "[eu] name" column reads the same one (WOLLobbyMenu.cpp).
			const Transport *transport = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
			const char *region = (transport != nullptr) ? transport->getRelayRegion() : nullptr;
			if (region != nullptr && *region != '\0')
			{
				// The name is an ARGUMENT, never the format: it came from another player.
				want.format(L"[%hs] %ls", region, name.str());
			}
			else
			{
				// A LAN game has no relay and so no region; the bare title is correct there, not
				// "[] name".
				want = name;
			}
		}
	}
	if (want.isEmpty())
	{
		want = TheGameText->fetch("GUI:GameOptions");
	}

	if (want.compare(s_lobbyTitleShown) != 0)
	{
		// Another player's text: filtered, and fitted with "..." rather than wrapped out of the box.
		GadgetStaticTextSetUntrustedLine(staticTextLobbyTitle, want);
		s_lobbyTitleShown = want;
	}
}


void LanGameOptionsMenuUpdate( WindowLayout * layout, void *userData)
{
	if(LANisShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	gxRefreshLobbyTitle();
	//TheLAN->update(); // this is handled in the lobby

	// GeneralsX: THE OPERATOR'S VOICE IN THE GAME-SETUP ROOM. The admin
	// panel's Say addresses a game's room, and a relay game sits HERE between "joined" and "started" - but
	// this screen had no drain, so the line was queued and shown only later, stale, at the start of the
	// match. "@Admin text" / "<@Mod> text" in the role colour, the relay's own notices grey, player lines
	// discarded (they are lobby chat, not this room's - this room's chat is LANAPI's own). See
	// GXMatchChat.h gxLobbyDrainRelayNotices; the NETWORK lobby's drain, which the shell also runs while
	// hidden under this screen, now stands aside when hidden so it cannot eat the line first - and this
	// one does the same, for the same reason, when something is on top of it.
	if (!layout->isHidden())
		gxLobbyDrainRelayNotices(lanGameRow, chatSystemColor);

	// GeneralsX: The forced-start deadline and the countdown watchdog.
	//
	// This is a UI update, not a network pump - the LANAPI pump lives in the engine loop and must
	// stay there (eebae98ea). All this does is read timeGetTime() and the slot list. Shell::update
	// calls it at a fixed ~30 Hz, but nothing here is derived from how often it runs.
	gxDriveStartPhase();
}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanGameOptionsMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (LANbuttonPushed)
				break;

			switch( key )
			{
				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonBack, buttonBackID );
					}
					// don't let key fall through anywhere else
					return MSG_HANDLED;
				}
			}
		}
	}
	return MSG_IGNORED;
}


// gxHandleLobbySlashCommand (below lanGameRow): /nickname works here too (GXApplyNickname sets all three names).
// Returns TRUE when the line was a slash command. Test order is load-bearing: (1) GXLobbyChatCommand, since verbs
// it retires are still recognised by LANAPI::applyLobbyCommand (in the digest); (2) /nick, needing the slot-list
// redraw; (3) GXLobbyGuardUnknownCommand. txtInput is non-const because a // escape rewrites it; send what comes back.
// GeneralsX @feature One row into this screen's chat box (GXChatBoxAdd): `tag` is "[S]", "[L]" or empty.
static void lanGameRow( const UnicodeString &tag, const UnicodeString &line, Color color )
{
	// Same reasoning as the relay lobby's sink: a headless run has no listbox, and silently
	// dropping the line is what made the lobby untestable without a GUI.
	if (listboxChatWindowLanGame == NULL)
	{
		UnicodeString whole = tag;
		whole.concat(line);
		AsciiString plain;
		plain.translate(whole);
		fprintf(stderr, "[GXLOBBY] %s\n", plain.str());
		fflush(stderr);
		return;
	}
	GXChatBoxAdd(listboxChatWindowLanGame, tag, line, color);
}

// A notice this client raised: "[S]", gray (a refusal keeps its red).
static void lanGameSay( const UnicodeString &line, Color color )
{
	lanGameRow(UnicodeString(L"[S]"), line, GXChatNoticeColor(color));
}

static Bool gxHandleLobbySlashCommand( UnicodeString &txtInput )
{
	AsciiString ascii;
	ascii.translate(txtInput);

	if (GXLobbyChatCommand(ascii, GX_LOBBY_SCREEN_GAME, listboxChatWindowLanGame, lanGameSay))
		return TRUE;

	// isEmpty first: getCharAt has a DEBUG_ASSERTCRASH on the index, so reading [0] out of an empty
	// string is a debug-build crash even though release returns 0.
	if (!ascii.isEmpty() && ascii.getCharAt(0) == '/')
	{
		AsciiString rest = ascii.str() + 1;
		AsciiString verb;
		rest.nextToken(&verb);
		verb.toLower();

		// GeneralsX: The name comes off the WIDE line - see
		// GXLobbyTakeNickRest. The ascii tokens above still dispatch everything else.
		{
			UnicodeString nickRest;
			if (GXLobbyTakeNickRest(txtInput, nickRest))
			{
				UnicodeString note;
				GXApplyNickname(nickRest, note);
				lanGameSay(note, chatSystemColor);
				// The slot list carries the name, so redraw it rather than waiting for the next
				// options round trip - otherwise your own row keeps the old name until somebody
				// else changes something.
				lanUpdateSlotList();
				return TRUE;
			}
		}
	}

	Bool escaped = FALSE;
	if (GXLobbyGuardUnknownCommand(ascii, GX_LOBBY_SCREEN_GAME, lanGameSay, escaped))
		return TRUE;

	// Only the // escape may change what is sent, and then only by one leading character. Trimmed
	// on the WIDE string, never re-derived from `ascii`: RequestChat puts these wide characters
	// straight on the wire and AsciiString::translate is 7-bit only (its own @todo), so an
	// unconditional round trip would mangle every non-ASCII line typed on this screen.
	if (escaped)
		txtInput = UnicodeString(txtInput.str() + 1);
	return FALSE;
}


//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType LanGameOptionsMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	switch( msg )
	{
		//-------------------------------------------------------------------------------------------------
		case GWM_CREATE:
			{
				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				if (windowMap)
					windowMap->winSetUserData(nullptr);

				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}
		//-------------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				// GeneralsX: Idea of upstream PR #2676: at match start hideShell()
				// runs DeinitLanGameGadgets(), which nulls every gadget pointer, but the layout is only
				// hidden - a mouse-down held across the start still delivers its mouse-up to the grab
				// window (hiding does not clear m_grabWindow), and ESC sends GBM_SELECTED with the
				// nulled buttonBack. parentLanGameOptions is null exactly while the gadgets are, so
				// every gadget case below drops the message then.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				LANGameInfo *myGame = TheLAN->GetMyGame();

        if ( controlID == comboBoxStartingCashID )
        {
          handleStartingCashSelection();
        }
        else
        {
				  for (Int i = 0; i < MAX_SLOTS; i++)
				  {
					  if (controlID == comboBoxColorID[i])
					  {
						  handleColorSelection(i);
					  }
					  else if (controlID == comboBoxPlayerTemplateID[i])
					  {
						  handlePlayerTemplateSelection(i);
					  }
					  else if (controlID == comboBoxTeamID[i])
					  {
						  handleTeamSelection(i);
					  }
					  else if( controlID == comboBoxPlayerID[i] && myGame->amIHost() )
					  {
						  // First, and on the slot states as they are: this pick cannot change a LATER
						  // slot, and the refresh below keeps an open list open (dontHide).
						  if( GadgetComboBoxTakeKeyPick( control ) )
							  lanOpenNextSlotDropDown( myGame, i );
						  // We don't have anything that'll happen if we click on ourselves
						  if(i == myGame->getLocalSlotNum())
						   break;
						  // Get
						  Int pos = -1;
						  GadgetComboBoxGetSelectedPos(comboBoxPlayer[i], &pos);
						  if( pos != SLOT_PLAYER && pos >= 0)
						  {
							  if( myGame->getLANSlot(i)->getState() == SLOT_PLAYER )
							  {
								  UnicodeString name = myGame->getPlayerName(i);
								  myGame->getLANSlot(i)->setState(SlotState(pos));
								  myGame->resetAccepted();
								  TheLAN->OnPlayerLeave(name);
							  }
							  else if( myGame->getLANSlot(i)->getState() != pos )
							  {
								  Bool wasAI = (myGame->getLANSlot(i)->isAI());
								  myGame->getLANSlot(i)->setState(SlotState(pos));
								  Bool isAI = (myGame->getLANSlot(i)->isAI());
								  if (wasAI || isAI)
									  myGame->resetAccepted();
								  if (wasAI ^ isAI)
									  PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, myGame, wasAI);
								  if (!s_isIniting)
								  {
									  TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
									  lanUpdateSlotList();
								  }
							  }
						  }
              break;
            }
          }
				}
        break;
			}
		
//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				// GeneralsX: Same guard as GCM_SELECTED above.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				// GeneralsX: The code-built password row. Matched on the
				// window POINTER rather than on a control id, so it cannot collide with a stock id
				// from the layout, and tested before the id chain below for the same reason.
				if( lanPasswordRowHandle( msg, control ) )
					break;

				if ( controlID == buttonBackID )
				{
					if( mapSelectLayout )
						{
							mapSelectLayout->destroyWindows();
							deleteInstance(mapSelectLayout);
							mapSelectLayout = nullptr;
						}
					TheLAN->RequestGameLeave();
					//TheShell->pop();

				}
				else if ( controlID == buttonEmoteID )
				{
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty() && !gxHandleLobbySlashCommand(txtInput))
					{
					// GeneralsX: One chat line every ChatRate::MIN_INTERVAL_MS,
					// shared with every other chat box in the game - see GameClient/ChatRateLimit.h.
					// Checked AFTER the slash-command interception so a command costs no allowance,
					// and the refused line goes BACK in the box rather than being dropped.
						unsigned int gxChatWait = 0;
						if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait))
						{
							GadgetTextEntrySetText(textEntryChat, txtInput);
							lanGameSay(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ChatRateLimited",
								L"Please wait %d more second(s) before sending another message.",
								(Int)gxChatWait), chatSystemColor);
						}
						else
						{
							TheLAN->RequestPlayerChat(txtInput);
						}
					}
				}
				else if ( controlID == buttonSelectMapID )
				{
					//buttonBack->winEnable( false );

					mapSelectLayout = TheWindowManager->winCreateLayout( "Menus/LanMapSelectMenu.wnd" );
					mapSelectLayout->runInit();
					mapSelectLayout->hide( FALSE );
					mapSelectLayout->bringForward();

				}
				else if ( controlID == buttonStartID )
				{
					if (TheLAN->AmIHost())
					{
						// GeneralsX: One control, two meanings: it reads
						// PLAY GAME in an idle lobby and CANCEL while a start is running. Only the
						// host ever gets the second one - this whole branch is behind AmIHost(),
						// and the joiner branch below is the unchanged ACCEPT path.
						gxLobbyTrace("start control pressed by the HOST, phase=%d", (Int)s_lanStart.phase);
						if (s_lanStart.phase != LanStart::PHASE_IDLE)
							gxCancelStart( TheGameText->FETCH_OR_SUBSTITUTE("LAN:StartCancelledByHost",
								L"The host cancelled the start.") );
						else
							StartPressed();
						//TheLAN->RequestGameStart();
					}
					else
					{
						// GeneralsX: A joiner's press is ACCEPT and only
						// ever ACCEPT. There is no branch here that cancels, and there could not
						// usefully be one: LANAPI::ResetGameStartTimer would clear a timer this
						// process never armed while the host went on broadcasting MSG_GAME_START_TIMER
						// once a second.
						gxLobbyTrace("start control pressed by a NON-HOST: accept only, no cancel path");
						//I'm the Client... send an accept message to the host.
						TheLAN->RequestAccept();

						// Disable the accept button
						EnableAcceptControls(TRUE, TheLAN->GetMyGame(), comboBoxPlayer, comboBoxColor, comboBoxPlayerTemplate,
							comboBoxTeam, buttonAccept, buttonStart, buttonMapStartPosition);

					}
				}
        else if ( controlID == checkboxLimitSuperweaponsID )
        {
          handleLimitSuperweaponsClick();
        }
				else if ( checkboxLimitArmies != nullptr && control == checkboxLimitArmies )
				{
					handleLimitArmiesClick();
				}
				else
				{
					for (Int i = 0; i < MAX_SLOTS; i++)
					{
						if (controlID == buttonMapStartPositionID[i])
						{
							LANGameInfo *game = TheLAN->GetMyGame();
							Int playerIdxInPos = -1;
							for (Int j=0; j<MAX_SLOTS; ++j)
							{
								LANGameSlot *slot = game->getLANSlot(j);
								if (slot && slot->getStartPos() == i)
								{
									playerIdxInPos = j;
									break;
								}
							}
							if (playerIdxInPos >= 0)
							{
								LANGameSlot *slot = game->getLANSlot(playerIdxInPos);
								if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
								{
									// it's one of my type.  Try to change it.
									Int nextPlayer = getNextSelectablePlayer(playerIdxInPos+1);
									handleStartPositionSelection(playerIdxInPos, -1);
									if (nextPlayer >= 0)
									{
										handleStartPositionSelection(nextPlayer, i);
									}
								}
							}
							else
							{
								// nobody in the slot - put us in
								Int nextPlayer = getNextSelectablePlayer(0);
								if (nextPlayer < 0)
									nextPlayer = getFirstSelectablePlayer(game);
								handleStartPositionSelection(nextPlayer, i);
							}
						}
					}
				}

				break;
			}
		//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED_RIGHT:
		{
			// GeneralsX: Same guard as GCM_SELECTED above.
			// Upstream reference: Caball009, PR #2676
			// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
			if (LANbuttonPushed || !parentLanGameOptions)
				break;

			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			for (Int i = 0; i < MAX_SLOTS; i++)
			{
				if (controlID == buttonMapStartPositionID[i])
				{
					LANGameInfo *game = TheLAN->GetMyGame();
					Int playerIdxInPos = -1;
					for (Int j=0; j<MAX_SLOTS; ++j)
					{
						LANGameSlot *slot = game->getLANSlot(j);
						if (slot && slot->getStartPos() == i)
						{
							playerIdxInPos = j;
							break;
						}
					}
					if (playerIdxInPos >= 0)
					{
						LANGameSlot *slot = game->getLANSlot(playerIdxInPos);
						if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
						{
							// it's one of my type.  Remove it.
							handleStartPositionSelection(playerIdxInPos, -1);
						}
					}
				}
			}
			break;
		}
		//-------------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				// GeneralsX: Same guard as GCM_SELECTED above.
				// Upstream reference: Caball009, PR #2676
				// https://github.com/TheSuperHackers/GeneralsGameCode/pull/2676
				if (LANbuttonPushed || !parentLanGameOptions)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				// GeneralsX: Return in the password box commits it, the same
				// way Return in the chat box sends. See the note on the row.
				if( lanPasswordRowHandle( msg, control ) )
					break;

				// Take the user's input and echo it into the chat window as well as
				// send it to the other clients on the lan
				if ( controlID == textEntryChatID )
				{

					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty())
						if (!gxHandleLobbySlashCommand(txtInput))
						{
					// GeneralsX: One chat line every ChatRate::MIN_INTERVAL_MS,
					// shared with every other chat box in the game - see GameClient/ChatRateLimit.h.
					// Checked AFTER the slash-command interception so a command costs no allowance,
					// and the refused line goes BACK in the box rather than being dropped.
							unsigned int gxChatWait = 0;
							if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait))
							{
								GadgetTextEntrySetText(textEntryChat, txtInput);
								lanGameSay(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ChatRateLimited",
									L"Please wait %d more second(s) before sending another message.",
									(Int)gxChatWait), chatSystemColor);
							}
							else
							{
								TheLAN->RequestPlayerChat(txtInput);
							}
						}

				}
				break;
			}
		//-------------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;
	}
	return MSG_HANDLED;
}

//-------------------------------------------------------------------------------------------------
/** Utility FUnction used as a bridge from other windows to this one */
//-------------------------------------------------------------------------------------------------
void PostToLanGameOptions( PostToLanGameType post )
{
	if (post >= POST_TO_LAN_GAME_TYPE_COUNT)
		return;
	LanPositionStartSpots();
	switch (post)
	{
			//-------------------------------------------------------------------------------------------------
		case SEND_GAME_OPTS:
		{
			LANGameInfo *game = TheLAN->GetMyGame();
			game->resetAccepted();
			updateGameOptions();
			lanUpdateSlotList();

			//buttonBack->winEnable( true );

			for(Int i = 0; i < MAX_SLOTS; ++i)
			{
				game->getSlot(i)->setStartPos(-1);
			}

			TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
			break;
		}
		case MAP_BACK:
		{
				//buttonBack->winEnable( true );
		}
		default:
			return;
	}
}
