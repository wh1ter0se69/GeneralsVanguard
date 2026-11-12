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

// FILE: PopupJoinGame.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Jul 2002
//
//	Filename: 	PopupJoinGame.cpp
//
//	author:		Matthew D. Campbell
//
//	purpose:	Contains the Callbacks for the Join Game Popup
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GUICallbacks.h"		// GXCreateLobbyLANAPI
#include "GameClient/GXPlayerIdentity.h"	// GXLobbyLANAPI::init: the assertion every transport declares
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/LANAPICallbacks.h"	// TheLAN, LANAPI, LANGameInfo
#include "GameClient/Shell.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GXChatLine.h"			// GXLobbyLANAPI::OnChat: stamp, room slot, emote name
#include "GameClient/GXLobbyCommands.h"		// GXLobbyLANAPI::OnChat: role decoration, separator, stamp
#include "GameClient/LanguageFilter.h"


//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static NameKeyType parentPopupID = NAMEKEY_INVALID;
static NameKeyType textEntryGamePasswordID = NAMEKEY_INVALID;
static NameKeyType buttonCancelID = NAMEKEY_INVALID;

static GameWindow *parentPopup = nullptr;
static GameWindow *textEntryGamePassword = nullptr;
// GeneralsX: Kept as a file static now, not a local of the init. The LAN
// prompt has to be able to rewrite this field AFTER the layout is up - a second refusal reuses an
// already-open overlay, and GameSpyOpenOverlay does not re-run runInit in that case.
static GameWindow *staticTextGameName = nullptr;
// GeneralsX: The caption to the LEFT of that field.
//
// It is a plain static carrying TEXT = "GUI:GameName", which the English CSF renders as
// "Game Name:" - so anything but a game name written into StaticTextGameName is read by the player
// as the game's name. It has NO name of its own in the shipped .wnd (both caption statics are
// declared NAME = "PopupJoinGame.wnd:", and parseName hands that one string to nameToKey), so the
// two of them share a window id and winGetWindowFromId cannot tell them apart. Found by geometry
// instead - see lanFindRowCaption.
static GameWindow *staticTextGameNameCaption = nullptr;

static void joinGame( AsciiString password );

// LAN / relay lobby password prompt. PopupJoinGame.wnd is stock; nothing here edits a .wnd, so mask, entry limits,
// caption and field width are set on the live windows. No OK button: Enter submits, Cancel or ESC abandons.
// The password is never logged, echoed or persisted here: it goes to LANAPI::applyLobbyCommand, the local echo
// is discarded, and the box is cleared on submit, cancel and open.

/// TRUE while this popup is serving a LAN/relay join rather than a GameSpy staging room.
static Bool s_lanMode = FALSE;
/// Latched ONLY across lanPromptOpen's GameSpyOpenOverlay call, so PopupJoinGameInit can tell who
/// opened it. Without this the mode would be a sticky flag, and it is not ours to clear: Shell.cpp
/// runs GameSpyCloseAllOverlays on every shell push and pop, so the popup routinely disappears
/// without either of this file's close paths running. A flag left TRUE that way would make the NEXT
/// GameSpy-opened join popup take the LAN branch and hand a GameSpy password to LANAPI.
static Bool s_lanOpening = FALSE;
/// TRUE when the prompt is being re-shown because the last attempt was refused.
static Bool s_lanRefused = FALSE;
/// Lobby name to show on the FIRST prompt; empty falls back to a neutral line.
static UnicodeString s_lanGameLabel;

// The join to re-issue, recorded by GXLobbyLANAPI's Request* overrides below rather than derived
// from OnGameJoin's LANGameInfo*. Two reasons: a direct-connect join can be refused before any
// LANGameInfo exists to hand back, and a pointer into LANAPI's game list can be freed by
// removeGame while the player is still typing. The NAME is re-resolved at retry time instead.
static Bool          s_retryValid  = FALSE;
static Bool          s_retryDirect = FALSE;
static UnicodeString s_retryGameName;
static UnsignedInt   s_retryIP     = 0;

// A password typed HERE is borrowed, not adopted: applyLobbyCommand also sets setHostPassword and the global
// s_carriedLobbyPassword, which is wrong for a prompt answered to enter a friend's lobby.
// So remember whether the player already had one and hand it back once this join is answered, only if it was ours.
static Bool s_lanPwBorrowed = FALSE;
/// TRUE between submitting a password and the join answering, so a second RET_NEEDS_PASSWORD can be
/// told from the first one. See OnGameJoin.
static Bool s_lanSubmitted = FALSE;

//-------------------------------------------------------------------------------------------------
/** Find the caption static that labels `field` - the one sharing its row and sitting to its left.
	*
	* By geometry rather than by name because the shipped .wnd gives both caption statics the same
	* decorated name, hence the same window id, and winGetWindowFromId returns whichever the tree
	* walk reaches first. Position is unambiguous and survives a renamed or re-authored layout: a
	* .wnd that does NOT put a caption there simply yields nullptr, and the caller then leaves the
	* field where it is - which is the safe direction, since a layout with no caption cannot be
	* contradicted by what we write. */
//-------------------------------------------------------------------------------------------------
static GameWindow *lanFindRowCaption( GameWindow *popup, GameWindow *field )
{
	if( popup == nullptr || field == nullptr )
		return nullptr;

	Int fieldX, fieldY;
	field->winGetPosition( &fieldX, &fieldY );

	GameWindow *best = nullptr;
	Int bestX = 0;

	// winGetPosition is parent-relative and every candidate is a sibling of the field, so the two
	// coordinate systems agree without going through winGetScreenPosition.
	for( GameWindow *child = popup->winGetChild(); child != nullptr; child = child->winGetNext() )
	{
		if( child == field )
			continue;
		if( !BitIsSet( child->winGetStyle(), GWS_STATIC_TEXT ) )
			continue;

		Int x, y, w, h;
		child->winGetPosition( &x, &y );
		child->winGetSize( &w, &h );

		if( y != fieldY )
			continue;			// different row
		if( x + w > fieldX )
			continue;			// not wholly to the left of the field

		if( best == nullptr || x > bestX )
		{
			best  = child;
			bestX = x;
		}
	}

	return best;
}

//-------------------------------------------------------------------------------------------------
/** Push the current prompt state onto the controls. Safe to call whether the layout was just
	* created by runInit or was already on screen from a previous refusal. */
//-------------------------------------------------------------------------------------------------
static void lanPromptApply( void )
{
	if( textEntryGamePassword != nullptr )
	{
		GadgetTextEntrySetText( textEntryGamePassword, UnicodeString::TheEmptyString );

		EntryData *pwEntry = (EntryData *)textEntryGamePassword->winGetUserData();
		if( pwEntry != nullptr )
		{
			// Asterisks, not the password - same flag and same reasoning as PopupHostGame's box.
			// The two screens ask about the same secret and must not disagree about showing it.
			pwEntry->secretText = TRUE;

			// The box must accept what a host can set. The stock .wnd limits it to 9 alphanumeric ASCII characters while
			// the LAN host's password row allows 31 with symbols, so the right password could not be typed.
			// 32 matches the host row (s_carriedLobbyPassword is [65]). LAN only: GameSpy keeps its 10-character limit.
			pwEntry->maxTextLen        = 32;
			pwEntry->alphaNumericalOnly = FALSE;
			pwEntry->aSCIIOnly          = FALSE;
		}
	}

	if( staticTextGameName != nullptr )
	{
		if( s_lanRefused )
		{
			// Take the row over before writing a status into it: the caption on its left says "Game Name:".
			// Blank the caption and grow the field left onto its space, from code, since UI archives have no delivery path.
			// Idempotent: the field's left edge equals the caption's after the first pass.
			if( staticTextGameNameCaption != nullptr )
			{
				GadgetStaticTextSetText( staticTextGameNameCaption, UnicodeString::TheEmptyString );

				Int capX, capY, fieldX, fieldY, fieldW, fieldH;
				staticTextGameNameCaption->winGetPosition( &capX, &capY );
				staticTextGameName->winGetPosition( &fieldX, &fieldY );
				staticTextGameName->winGetSize( &fieldW, &fieldH );

				if( capX < fieldX )
				{
					staticTextGameName->winSetPosition( capX, fieldY );
					staticTextGameName->winSetSize( fieldW + (fieldX - capX), fieldH );
				}
			}

			// A refusal here means one thing and says one thing: RET_BAD_PASSWORD, the host telling THIS client the password was wrong. Naming it discloses nothing the player did not have: handleJoinDeny only delivers a deny addressed to us while m_pendingAction is ACT_JOIN, so the prompt is only on screen because that host answered us by name. Every other outcome is reported by LANAPI::OnGameJoin rather than here.
			GadgetStaticTextSetText( staticTextGameName,
				TheGameText->FETCH_OR_SUBSTITUTE( "LAN:JoinBadPassword", L"Wrong password" ) );
		}
		else
		{
			// The first ask. The field keeps its own job - it is captioned "Game Name:" - so it
			// gets the lobby name or nothing at all. Nothing at all is honest when the browser row
			// carried no name, and the popup is already self-explanatory: a captioned password box
			// with a Cancel button.
			GadgetStaticTextSetText( staticTextGameName, s_lanGameLabel );
		}
	}

	// Focus only. Modality is a STACK (GameWindowManager::winSetModal), pushed once by
	// PopupJoinGameInit and popped by winDestroy; pushing again here for an overlay that is already
	// up would only reorder the stack, and this popup is already its head. (Before TheSuperHackers
	// PR #3224 it was worse than pointless: the second push duplicated the entry and one copy was
	// left behind for the rest of the session.)
	if( textEntryGamePassword != nullptr )
		TheWindowManager->winSetFocus( textEntryGamePassword );
}

//-------------------------------------------------------------------------------------------------
/** Tear the prompt down and forget everything about it. */
//-------------------------------------------------------------------------------------------------
static void lanPromptClose( void )
{
	s_lanMode = FALSE;
	s_lanRefused = FALSE;
	s_lanGameLabel.clear();

	if( textEntryGamePassword != nullptr )
		GadgetTextEntrySetText( textEntryGamePassword, UnicodeString::TheEmptyString );

	// Off the modal stack NOW, not whenever the deferred winDestroy gets round to it. The retry
	// that follows can put a message box up, and a message box under a still-modal popup dims the
	// screen and never draws. Since TheSuperHackers
	// PR #3224 winUnsetModal unlinks the window from anywhere in the stack and returns
	// WIN_ERR_GENERAL_FAILURE with no side effect when the window is not on it at all, so
	// winDestroy's own later unset finds nothing and this cannot double-pop.
	if( parentPopup != nullptr )
		TheWindowManager->winUnsetModal( parentPopup );

	GameSpyCloseOverlay( GSOVERLAY_GAMEPASSWORD );

	// winDestroy is deferred, so these are stale rather than freed - but a second refusal would
	// otherwise write through them into windows that are on their way out.
	parentPopup = nullptr;
	textEntryGamePassword = nullptr;
	staticTextGameName = nullptr;
	staticTextGameNameCaption = nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Give back a password this prompt bound on the player's behalf. No-op when they already had one
	* of their own, and no-op when nothing was borrowed. */
//-------------------------------------------------------------------------------------------------
static void lanPromptRelease( void )
{
	s_lanSubmitted = FALSE;

	if( !s_lanPwBorrowed )
		return;
	s_lanPwBorrowed = FALSE;

	if( TheLAN == nullptr )
		return;

	// /nopassword is the mirror image of the /password used to bind it: it clears m_joinPassword,
	// the host-side verifier and the carried static together. Running it on a LANAPI that was
	// rebuilt since (relayJoinSelectedGame does that mid-join) is still correct - a new instance
	// re-read the same carried static in its constructor, so this is what clears it there too.
	UnicodeString discardedEcho;
	TheLAN->applyLobbyCommand( AsciiString( "/nopassword" ), discardedEcho );
}

//-------------------------------------------------------------------------------------------------
/** Show (or re-show) the prompt for a LAN/relay join. Returns FALSE when there is no UI to show it
	* on, so the caller can fall back to the message box rather than swallowing the refusal. */
//-------------------------------------------------------------------------------------------------
static Bool lanPromptOpen( Bool refused, LANGameInfo *theGame )
{
	if( TheWindowManager == nullptr || TheGameText == nullptr )
		return FALSE;

	s_lanRefused = refused;
	s_lanGameLabel.clear();

	// Only on the first ask. A refusal must say nothing about which lobby it came from.
	//
	// NEVER getName(). RequestGameCreate builds that as "<hexIP><hexSeed>" and truncates it to
	// g_lanGameNameLength, which is exactly the length of the hex prefix - so it is always a
	// uniqueness token like "0A2A000101249D48" and never anything the host typed. The two things
	// that ARE display names are the lobby name (set through PopupHostGame) and, failing that, the
	// host player's name, which is what the browser row this player just clicked shows them.
	if( !refused && theGame != nullptr )
	{
		const AsciiString lobbyName = theGame->getLobbyName();
		if( !lobbyName.isEmpty() )
		{
			s_lanGameLabel.translate( lobbyName );
		}
		else
		{
			const GameSlot *hostSlot = theGame->getConstSlot( 0 );
			if( hostSlot != nullptr && !hostSlot->getName().isEmpty() )
				s_lanGameLabel = hostSlot->getName();
		}
	}

	// Reuses the overlay slot the GameSpy path uses. Only GSOVERLAY_BUDDY touches GameSpy state in
	// GameSpyOpenOverlay; this one is a winCreateLayout and a runInit, and is a no-op re-show when
	// the layout is already up.
	s_lanOpening = TRUE;
	GameSpyOpenOverlay( GSOVERLAY_GAMEPASSWORD );
	s_lanOpening = FALSE;

	if( !GameSpyIsOverlayOpen( GSOVERLAY_GAMEPASSWORD ) )
	{
		s_lanMode = FALSE;
		return FALSE;
	}

	// Set here as well as in PopupJoinGameInit: a second refusal re-shows a layout that is already
	// up, and GameSpyOpenOverlay does not re-run runInit on that path.
	s_lanMode = TRUE;

	lanPromptApply();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The player pressed Enter in the password box on a LAN/relay join. */
//-------------------------------------------------------------------------------------------------
static void lanPromptSubmit( const UnicodeString &typed )
{
	// Trimmed, and the emptiness test is on the trimmed value. applyLobbyCommand trims before binding (LANAPI.cpp).
	// This prompt allows whitespace, and an untrimmed whitespace-only entry builds "/password " with no operand,
	// which is the documented CLEAR and would silently wipe the player's own host password.
	UnicodeString entered = typed;
	entered.trim();

	// Nothing typed is not a decision - leave the prompt up rather than clearing a password the
	// player may already hold and knocking again without one.
	if( entered.isEmpty() )
		return;

	// A lobby that is not there any more is reported as a lobby that is not there, not as a bad password. Nothing is disclosed by being honest: the prompt is only on screen because this host answered US (see lanPromptApply), and RET_GAME_GONE is what the base would report for the same join without the prompt. Routed through OnGameJoin rather than formatted here so it picks up the whole existing failure dialog, including the borrowed-password hand-back on the way past.
	LANGameInfo *game = nullptr;
	if( TheLAN == nullptr || !s_retryValid )
	{
		// s_lanPwBorrowed is deliberately NOT cleared when there is no TheLAN to hand the password
		// back to. applyLobbyCommand also wrote the process-global s_carriedLobbyPassword, which
		// outlives any one LANAPI, so dropping the record here would strand it; leaving the flag
		// set means the next lanPromptRelease runs /nopassword on whichever instance exists then,
		// and that is the one call that clears the carried copy too.
		lanPromptClose();
		if( TheLAN != nullptr )
			TheLAN->OnGameJoin( LANAPIInterface::RET_GAME_GONE, nullptr );
		return;
	}

	if( !s_retryDirect )
	{
		// Re-resolve by name; the pointer we were denied with may have been freed by removeGame
		// while the player typed.
		game = TheLAN->LookupGame( s_retryGameName );
		if( game == nullptr )
		{
			lanPromptClose();
			TheLAN->OnGameJoin( LANAPIInterface::RET_GAME_GONE, nullptr );
			return;
		}
	}

	{
		// Whether the player already had a lobby password of their own, read BEFORE we bind ours.
		// This is the whole test for "borrowed" - see the note on s_lanPwBorrowed.
		const Bool hadOwnPassword = TheLAN->isHostingWithPassword();

		AsciiString plain;
		plain.translate( entered );

		AsciiString cmd( "/password " );
		cmd.concat( plain.str() );

		// applyLobbyCommand consumes the line: it sets m_joinPassword, binds the host-side
		// verifier and carries the value for the next lobby, and RequestChat is never involved -
		// so nothing typed here reaches the wire as text. The reply it hands back is a local echo
		// for the chat box; this prompt IS the feedback, so it is dropped on the floor.
		UnicodeString discardedEcho;
		TheLAN->applyLobbyCommand( cmd, discardedEcho );

		// Best effort, not a guarantee - AsciiString is refcounted and LANAPI now holds its own
		// copy by design. It still beats leaving the plaintext in three live buffers on this
		// screen for the rest of the session.
		cmd.clear();
		plain.clear();
		entered.clear();

		if( !hadOwnPassword )
			s_lanPwBorrowed = TRUE;
	}

	// Armed until the join answers, so a host that answers the retry with RET_NEEDS_PASSWORD again
	// is shown as a refusal rather than as a fresh first ask. See OnGameJoin.
	s_lanSubmitted = TRUE;

	// Close BEFORE re-dialling. The retry can answer synchronously (RET_BUSY, RET_GAME_GONE) and
	// that answer ends up in a message box, which is invisible while a modal popup is up.
	const Bool direct = s_retryDirect;
	const UnsignedInt ip = s_retryIP;
	lanPromptClose();

	// The host still holds the salt and the fresh single-use challenge it sent with the refusal
	// (LOBBYPW_CHALLENGE_LIFETIME_MS is 120 s), and LANAPI::sendJoinRequest attaches an answer
	// computed from them as soon as m_joinPassword is non-empty. So this is an ordinary re-join;
	// nothing about the handshake is reimplemented here.
	if( direct )
		TheLAN->RequestGameJoinDirectConnect( ip );
	else
		TheLAN->RequestGameJoin( game, ip );
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Chat lines on the LAN screens: the LAN lobby, a game setup room and the score screen.
//
// LANAPI::OnChat draws them, and it lives in the simulation source digest, so the lines are drawn
// here instead (GXLobbyLANAPI::OnChat below). What changes against it:
//   * every said line carries the local time, in the OS's own 12/24-hour form (GXLobbyTimestamp);
//   * one line shape and one set of colours with the Online lobby: a gray time column, then "[Name]: text"
//     in white, "[@Name]: text" in crimson for a relay-proven admin, "[+Name]" in purple for a moderator.
//     The role is the relay's roster letter for the room slot the relay stamped on the datagram - never
//     the name or the chat type the sender wrote;
//   * there are no emotes: an older client's emote is drawn as an ordinary line, filtered like any
//     other (the base drew emotes unfiltered);
//   * a notice carries "[S]" (this client) or "[L]" (the room), in gray, as in the Online lobby.
//-------------------------------------------------------------------------------------------------

/// How often a game setup room re-asks the relay who is in it, for the roles on its chat lines.
static const UnsignedInt GX_CHAT_ROSTER_REFRESH_MS = 4000;

//-------------------------------------------------------------------------------------------------
/** LANAPI with the password prompt wired in. See the note on GXCreateLobbyLANAPI in
	* GUICallbacks.h for why this is a subclass and not a hook in LANAPI itself. */
//-------------------------------------------------------------------------------------------------
class GXLobbyLANAPI : public LANAPI
{
public:

	GXLobbyLANAPI() : m_gxRosterAskedMs( 0 ) {}

	/// The relay's role for whoever holds `ip`'s slot in our room. A player when there is no relay,
	/// no roster yet, or the address is not in our room.
	GXAssertRole gxRelayRoleOf( UnsignedInt ip )
	{
		const UnsignedInt slot = gxChatRosterSlotFor( ip, m_localIP );
		if( slot == 0 )
			return GX_ROLE_PLAYER;
		UnsignedInt slots[MAX_SLOTS];
		const Int n = getLobbyRoster( slots, MAX_SLOTS );
		for( Int i = 0; i < n; ++i )
		{
			if( slots[i] == slot )
				return GXLobbyRoleFromCode( getLobbyRosterRole( i ) );
		}
		return GX_ROLE_PLAYER;
	}

	/// Keep the game room's roster fresh while a game is being set up and on the score screen after it, so
	/// a chat line's role is known before it is said and a role revoked during the match is gone there too.
	/// Every member asks, the host included: a listed host's transport asks only on
	/// its 30 s tag tick, and a /private host not at all.
	///
	/// The host's advertised ping is timed from whichever roster request is outstanding
	/// (Transport::requestRelayRoster), so these requests are timed samples too. One sent while the
	/// tag tick's is still unanswered voids that one sample; the next, 4 s later, is timed again.
	virtual void update() override
	{
		LANAPI::update();
#if defined(GX_TEST_INSTRUMENTS)
		// GX_TEST_LAN_CREATE_REFUSE=1 (test builds only): once, in the LAN lobby, the refusal a busy create
		// gets, so LANAPI::OnGameCreate's rows can be seen landing in the two-column chat box without a
		// second machine racing a join.
		static Bool s_createRefused = FALSE;
		if( !s_createRefused && m_inLobby && listboxChatWindow != nullptr && getenv( "GX_TEST_LAN_CREATE_REFUSE" ) != nullptr )
		{
			s_createRefused = TRUE;
			OnGameCreate( RET_BUSY );
		}
#endif
		if( m_inLobby || m_currentGame == nullptr )
			return;
		if( m_currentGame->isGameInProgress() && !TheShell->isShellActive() )
			return;		// the match itself: nothing reads the roster until the score screen (OnChat's test)
		const UnsignedInt now = timeGetTime();
		if( m_gxRosterAskedMs != 0 && now - m_gxRosterAskedMs < GX_CHAT_ROSTER_REFRESH_MS )
			return;
		m_gxRosterAskedMs = now;
		requestLobbyRoster();
	}

	/// No emotes: a "/me" line that got past the screen's guard (only a "//me" escape does) is said as
	/// typed, a normal line, rather than turned into an emote by LANAPI::RequestPlayerChat.
	virtual void RequestPlayerChat( UnicodeString message ) override
	{
		if( message.startsWithNoCase( L"/me" ) )
		{
			RequestChat( message, LANCHAT_NORMAL );
			return;
		}
		LANAPI::RequestPlayerChat( message );
	}

	virtual void OnChat( UnicodeString player, UnsignedInt ip, UnicodeString message, ChatType format ) override
	{
		GameWindow *chatWindow = nullptr;
		if( m_inLobby )
			chatWindow = listboxChatWindow;
		else if( m_currentGame && m_currentGame->isGameInProgress() && TheShell->isShellActive() )
			chatWindow = listboxChatWindowScoreScreen;
		else if( m_currentGame && !m_currentGame->isGameInProgress() )
			chatWindow = listboxChatWindowLanGame;
		if( chatWindow == nullptr )
			return;

		if( format == LANCHAT_LOG )
		{
			UnicodeString logged( L"[LOG] " );
			logged.concat( message );
			GXChatBoxAdd( chatWindow, UnicodeString::TheEmptyString, logged, GameMakeColor( 255, 255, 0, 255 ) );	// yellow, as before
			return;
		}

		// A notice: "[S]" when this client raised it (ip 0 is one raised without an address: a failed map
		// transfer, the admin check), "[L]" when the host sent it (its start-policy lines). The chat type
		// is the sender's own claim, so a "system" line from anybody else is drawn as their words.
		if( format == LANCHAT_SYSTEM )
		{
			const Bool local = ( ip == 0 || ip == m_localIP );
			const Bool fromHost = !m_inLobby && m_currentGame != nullptr && ip == m_currentGame->getIP( 0 );
			if( local || fromHost )
			{
				GXChatBoxAdd( chatWindow, local ? UnicodeString( L"[S]" ) : UnicodeString( L"[L]" ), message, GXChatMutedColor() );
				return;
			}
		}

		// Everything else is somebody talking. An emote from an older client is an ordinary line:
		// there are no emotes any more.
		if( TheLanguageFilter != nullptr )
			TheLanguageFilter->filterLine( message );

		const GXAssertRole role = gxRelayRoleOf( ip );
		GXChatBoxAdd( chatWindow, UnicodeString::TheEmptyString, GXLobbySaid( GXLobbyDecorateName( player, role ), message ),
			GXLobbyRoleTextColor( role, GXChatPlayerColor() ) );
	}

	/// The base writes its refusal lines ("game exists", "busy"...) into the LAN lobby's chat box without
	/// a column, which in a two-column box is the time column; they are moved into the text column.
	virtual void OnGameCreate( ReturnType ret ) override
	{
		const Bool lobby = m_inLobby;
		LANAPI::OnGameCreate( ret );
		if( lobby && listboxChatWindow != nullptr )
			GXChatBoxAdopt( listboxChatWindow );
	}

	/// Every lobby transport is born identified: a new Transport starts with an empty m_relayIdentity, and the
	/// relay keys sanctions on the account.
	/// Here because it is the one door they all use (LANAPI is in the digest); HeadlessLANAPI must not pick this up.
	/// After LANAPI::init(): the relay discards an identity from an endpoint not yet filed as a member.
	virtual void init() override
	{
		LANAPI::init();
		setLobbyIdentity(GXPlayerIdentity::getVerifiedAssertionText());
	}

	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) override
	{
		s_retryValid  = (game != nullptr);
		s_retryDirect = FALSE;
		s_retryGameName.clear();
		if( game != nullptr )
			s_retryGameName = game->getName();
		s_retryIP = ip;
		LANAPI::RequestGameJoin( game, ip );
	}

	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) override
	{
		// Recorded even though the direct-connect handshake re-enters RequestGameJoin above once
		// the host's announce arrives: a host that refuses before that point leaves the IP as the
		// only thing there is to retry with.
		s_retryValid  = (ipaddress != 0);
		s_retryDirect = TRUE;
		s_retryGameName.clear();
		s_retryIP     = ipaddress;
		LANAPI::RequestGameJoinDirectConnect( ipaddress );
	}

	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override
	{
		// RET_NEEDS_PASSWORD is the ordinary first knock at a protected game and RET_BAD_PASSWORD
		// is a mistyped one; both are answerable, and both used to end in a message box telling
		// the player to go and type a slash command.
		if( ret == RET_NEEDS_PASSWORD || ret == RET_BAD_PASSWORD )
		{
			// RET_BAD_PASSWORD is unambiguous. RET_NEEDS_PASSWORD is normally the first knock, but
			// it also lands here after handleJoinDeny has spent its three automatic answers
			// (LANAPIhandlers.cpp) - in which case a password WAS submitted and did not satisfy
			// the host, and re-showing the lobby name as though nothing had happened would leave
			// the player pressing Enter into silence.
			const Bool refused = (ret == RET_BAD_PASSWORD) || s_lanSubmitted;
			s_lanSubmitted = FALSE;

			if( lanPromptOpen( refused, theGame ) )
				return;		// keep the password: the prompt is where it gets corrected

			// No UI to prompt on. Fall back to the base's message box, and do not sit on a
			// password the player never asked to keep.
			lanPromptRelease();
			LANAPI::OnGameJoin( ret, theGame );
			return;
		}

		// RET_GAME_GONE and RET_TIMEOUT are reported verbatim, not flattened into the prompt's refusal line: that would
		// hide a host that quit or a lost datagram ("check the password" forever). RET_TIMEOUT is where
		// LANAPICallbacks.cpp appends LAN:SimIdTimeoutHint, since an incompatible peer usually presents as a timeout.
		lanPromptRelease();
		LANAPI::OnGameJoin( ret, theGame );
	}

private:
	UnsignedInt m_gxRosterAskedMs;	///< when update() last asked the relay for the room roster; 0 = not yet
};

LANAPI *GXCreateLobbyLANAPI( void )
{
	return NEW GXLobbyLANAPI();
}

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** Initialize the PopupHostGameInit menu */
//-------------------------------------------------------------------------------------------------
void PopupJoinGameInit( WindowLayout *layout, void *userData )
{
	// GeneralsX: Whoever opened this popup decides what it is for. The
	// GameSpy path calls GameSpyOpenOverlay directly and sets nothing, so it always lands on FALSE;
	// only lanPromptOpen holds the latch across its own open call.
	s_lanMode = s_lanOpening;

	parentPopupID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:ParentJoinPopUp");
	parentPopup = TheWindowManager->winGetWindowFromId(nullptr, parentPopupID);

	textEntryGamePasswordID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:TextEntryGamePassword");
	textEntryGamePassword = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGamePasswordID);
	GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);

	// GeneralsX: Asterisks, not the password, on BOTH paths.
	//
	// The .wnd authors this as an ordinary text box, so it rendered a typed password in the clear
	// on a screen people stream. Unconditional rather than LAN-only: the GameSpy path has the same
	// shoulder, and secretText is display-only - GadgetTextEntryGetText still returns the real
	// string, so joinGame below is unaffected. Set from code because editing the layout would need
	// a UI archive that has no delivery path.
	if (textEntryGamePassword != nullptr)
	{
		EntryData *pwEntry = (EntryData *)textEntryGamePassword->winGetUserData();
		if (pwEntry != nullptr)
			pwEntry->secretText = TRUE;
	}

	NameKeyType staticTextGameNameID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:StaticTextGameName");
	staticTextGameName = TheWindowManager->winGetWindowFromId(parentPopup, staticTextGameNameID);
	GadgetStaticTextSetText(staticTextGameName, UnicodeString::TheEmptyString);

	// The unnamed caption sitting on the same row, to the left of that field. See the note on the
	// static and on lanFindRowCaption. Resolved on both paths but only ever written to by the LAN
	// one; the GameSpy path is left exactly as it was.
	staticTextGameNameCaption = lanFindRowCaption(parentPopup, staticTextGameName);

	buttonCancelID = NAMEKEY("PopupJoinGame.wnd:ButtonCancel");

	if (s_lanMode)
	{
		// GeneralsX: A LAN/relay join. There is no staging room and
		// TheGameSpyInfo is NULL on this path by construction - the LAN lobby never runs
		// SetUpGameSpy - so the lookup below is not merely useless here, it is a null dereference.
		lanPromptApply();
	}
	else if (TheGameSpyInfo != nullptr)
	{
		GameSpyStagingRoom *ourRoom = TheGameSpyInfo->findStagingRoomByID(TheGameSpyInfo->getCurrentStagingRoomID());
		if (ourRoom)
			GadgetStaticTextSetText(staticTextGameName, ourRoom->getGameName());
	}

	TheWindowManager->winSetFocus( parentPopup );
	TheWindowManager->winSetModal( parentPopup );

	// After winSetFocus(parentPopup), or the caret lands on the popup rather than in the box and
	// the player's first keystroke goes nowhere.
	if (s_lanMode && textEntryGamePassword != nullptr)
		TheWindowManager->winSetFocus( textEntryGamePassword );

}

//-------------------------------------------------------------------------------------------------
/** PopupHostGameInput callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupJoinGameInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
//			if (buttonPushed)
//				break;

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
						// GeneralsX: On the LAN/relay path there is
						// nothing to stand down: LANAPI already cleared m_pendingAction when it
						// handed us the refusal, and SetLobbyAttemptHostJoin gates the GameSpy
						// screen's own buttons, which this join never touched.
						if( s_lanMode )
						{
							lanPromptRelease();		// give back a password we bound for this join
							lanPromptClose();
							return MSG_HANDLED;
						}

						GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
						SetLobbyAttemptHostJoin( FALSE );
						parentPopup = nullptr;
					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** PopupHostGameSystem callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupJoinGameSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
  switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{

			break;

		}
    //---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			// The one teardown path this file does not own: Shell.cpp calls GameSpyCloseAllOverlays on every push and pop,
			// destroying the popup without lanPromptRelease; the join password would then leak into the next hosted game.
			// Keyed on window identity, not s_lanMode: GWM_DESTROY is deferred and can arrive after the retry popup exists.
			if( window == parentPopup )
			{
				lanPromptRelease();
				s_lanMode = FALSE;
				s_lanRefused = FALSE;
				s_lanGameLabel.clear();
				parentPopup = nullptr;
				textEntryGamePassword = nullptr;
				staticTextGameName = nullptr;
				staticTextGameNameCaption = nullptr;
			}

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			if (controlID == buttonCancelID)
			{
				// GeneralsX: See the ESC handler for why the LAN path
				// does not touch SetLobbyAttemptHostJoin.
				if( s_lanMode )
				{
					lanPromptRelease();		// give back a password we bound for this join
					lanPromptClose();
					break;
				}

				GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
				SetLobbyAttemptHostJoin( FALSE );
				parentPopup = nullptr;
			}
			break;
		}

    //----------------------------------------------------------------------------------------------
    case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			break;

		}
    //---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

      if( controlID == textEntryGamePasswordID )
			{
				// read the user's input and clear the entry box
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryGamePassword ));
				GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);

				// GeneralsX: The LAN/relay retry. Split out before the
				// GameSpy path's trim only because lanPromptSubmit does its own - and has to, for
				// a reason that has nothing to do with the GameSpy request below. See it.
				if( s_lanMode )
				{
					lanPromptSubmit(txtInput);
					txtInput.clear();
					break;
				}

				txtInput.trim();
				if (!txtInput.isEmpty())
				{
					AsciiString munkee;
					munkee.translate(txtInput);
					joinGame(munkee);
				}
			}
			break;
		}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}


//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static void joinGame( AsciiString password )
{
	GameSpyStagingRoom *ourRoom = TheGameSpyInfo->findStagingRoomByID(TheGameSpyInfo->getCurrentStagingRoomID());
	if (!ourRoom)
	{
		GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
		SetLobbyAttemptHostJoin( FALSE );
		parentPopup = nullptr;
		return;
	}
	PeerRequest req;
	req.peerRequestType = PeerRequest::PEERREQUEST_JOINSTAGINGROOM;
	req.text = ourRoom->getGameName().str();
	req.stagingRoom.id = ourRoom->getID();
	req.password = password.str();
	TheGameSpyPeerMessageQueue->addRequest(req);
	// GeneralsX: This used to print the password itself. A lobby password
	// is a credential people reuse, and DEBUG_LOG lands in a file that gets attached to bug
	// reports - so the one fact worth recording is whether a password was supplied, never which.
	DEBUG_LOG(("Attempting to join game %d(%ls)%s", ourRoom->getID(), ourRoom->getGameName().str(),
		password.isEmpty() ? "" : " with a password"));
	GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
	parentPopup = nullptr;
}
