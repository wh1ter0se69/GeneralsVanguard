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


//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static NameKeyType parentPopupID = NAMEKEY_INVALID;
static NameKeyType textEntryGamePasswordID = NAMEKEY_INVALID;
static NameKeyType buttonCancelID = NAMEKEY_INVALID;

static GameWindow *parentPopup = nullptr;
static GameWindow *textEntryGamePassword = nullptr;
// GeneralsX @feature Claude 17/08/2026 Kept as a file static now, not a local of the init. The LAN
// prompt has to be able to rewrite this field AFTER the layout is up - a second refusal reuses an
// already-open overlay, and GameSpyOpenOverlay does not re-run runInit in that case.
static GameWindow *staticTextGameName = nullptr;
// GeneralsX @fix Claude 17/08/2026 (cross-model review) The caption to the LEFT of that field.
//
// It is a plain static carrying TEXT = "GUI:GameName", which the English CSF renders as
// "Game Name:" - so anything but a game name written into StaticTextGameName is read by the player
// as the game's name. It has NO name of its own in the shipped .wnd (both caption statics are
// declared NAME = "PopupJoinGame.wnd:", and parseName hands that one string to nameToKey), so the
// two of them share a window id and winGetWindowFromId cannot tell them apart. Found by geometry
// instead - see lanFindRowCaption.
static GameWindow *staticTextGameNameCaption = nullptr;

static void joinGame( AsciiString password );

//-----------------------------------------------------------------------------
// GeneralsX @feature Claude 17/08/2026 LAN / relay lobby password prompt.
//
// THE LAYOUT IS STOCK AND ALREADY SHIPS. Menus/PopupJoinGame.wnd is inside WindowZH.big and
// carries the controls this needs - ParentJoinPopUp, TextEntryGamePassword, StaticTextGameName,
// ButtonCancel and two unnamed captions - because it is what GameSpy used for the same question.
// Nothing here adds, edits or overrides a .wnd. That is not a stylistic preference: there is no
// delivery path for custom UI archives today (the confirm-password field added on 16/08 lives in a
// !GXUI.big that exists on one machine), so a layout change would work for the author and reach
// nobody. Everything this file needs to differ from the authored layout - the mask, the entry
// limits, the caption and the field's width - is therefore set on the live windows instead, which
// is per-instance and is undone for free when the overlay is destroyed and recreated.
//
// There is no OK button in the layout and none is invented. EA's own idiom is Enter to submit,
// which arrives as GEM_EDIT_DONE, and Cancel or ESC to abandon.
//
// THE TYPED PASSWORD IS NEVER LOGGED, ECHOED OR PERSISTED BY THIS FILE. It goes to
// LANAPI::applyLobbyCommand - the same public entry point /password has used since 08/08, and the
// only place in the engine that binds one - and the local echo that call returns is discarded.
// The box is masked, and it is cleared on submit, on cancel and on open.
//
// WHAT THIS POPUP IS ALLOWED TO SAY, and why it is one short line.
//
// The prompt reports exactly one outcome: the host said the password was wrong. Every other way a
// join can end - gone, timed out, full, wrong build - is handed back to LANAPI::OnGameJoin, which
// already has a fixed-size modal and much better copy for each of them, including the SimID build
// comparison and the LAN:SimIdTimeoutHint that is the only thing telling a player they are on the
// wrong build. Swallowing those into a generic line was tried and reverted; see OnGameJoin.
//
// It has to be SHORT because the field it lands in is 224x24 at the 800x600 authoring resolution
// and W3DStaticText.cpp word-wraps at size.x-10 then hard-clips to the box (:94, :112-115). Window
// rects scale linearly with resolution while adjustFontSize's Classic factor is capped at 2.0
// (GlobalLanguage.cpp:283-287), so 800x600 is the tightest case - and the point size is not fixed
// either, because the field renders through HEADERTEMPLATE "LabelRegular": stock English
// HeaderTemplate.ini sets that to Arial 10, and the ControlBarPro archives many players have
// installed raise it to 12 or 14. Measured against Arial at the 14-point worst case,
// "Could not join. Check the password and try again." is ~427 px against a 214 px wrap width -
// two lines in a box that clips after one. So the copy is kept to a couple of words AND the field
// is widened over the caption before anything is written into it.
//-----------------------------------------------------------------------------

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

// GeneralsX @feature Claude 17/08/2026 (cross-model review) A password typed HERE is borrowed, not
// adopted.
//
// applyLobbyCommand is the only public way to bind a join password, and it deliberately does three
// things at once: m_joinPassword, setHostPassword, and the process-global s_carriedLobbyPassword
// that a freshly constructed LANAPI reads back. That is right for somebody who typed /password -
// the verb is documented as applying to the next game you host - but it is wrong for this box.
// Somebody answering a prompt to get into a friend's lobby has not asked to password-protect their
// own next game, and would have no idea they had.
//
// So: remember whether the player already had one, and hand it back the moment this join has been
// answered one way or the other. Only when it was ours to begin with - a password the player set
// themselves is theirs, and this must not delete it.
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

			// GeneralsX @bugfix Claude 17/08/2026 THE BOX MUST ACCEPT WHAT A HOST CAN SET.
			//
			// The shipped .wnd authors this field MAXLEN 10, ALPHANUMERICALONLY 1, ASCIIONLY 1 -
			// and the accept test is `charPos < maxTextLen-1`, so that is NINE alphanumeric ASCII
			// characters. The LAN host's own password row is built by
			// LanGameOptionsMenu::lanPasswordRowCreate through CodeGuiTextEntry with maxTextLen 32
			// and aSCIIOnly/alphaNumericalOnly both left FALSE by its memset - so a host can set,
			// and its UI invites, a 31-character password with spaces and symbols in it.
			//
			// Without this the prompt is a trap on every such lobby: the correct password cannot be
			// typed, so it reads as "Wrong password" forever with Cancel as the only exit. It would
			// also be a NARROWING, because the thing this popup replaces - /password in the chat
			// box - goes through RequestChat with no length or character limit at all.
			//
			// 32 rather than more for the same reason the host row stops there: applyLobbyCommand
			// strlcpy's into s_carriedLobbyPassword[65] (LANAPI.cpp:63, :1372), and stopping short
			// of that means what the player typed is what gets bound, always.
			//
			// LAN only. The GameSpy path's 10/alphanumeric matches PopupHostGame's own GameSpy
			// password box exactly, so those two agree already and are left alone.
			pwEntry->maxTextLen        = 32;
			pwEntry->alphaNumericalOnly = FALSE;
			pwEntry->aSCIIOnly          = FALSE;
		}
	}

	if( staticTextGameName != nullptr )
	{
		if( s_lanRefused )
		{
			// GeneralsX @fix Claude 17/08/2026 (cross-model review) Take the row over before
			// writing a status into it.
			//
			// The caption to the left says "Game Name:", so a status left beside it reads as the
			// name of the game. Blank the caption and grow the field left onto the space it
			// occupied: one row, one sentence, nothing contradicting it. Done from code and never
			// in the .wnd - there is no delivery path for a UI archive, so a layout edit would
			// work here and reach nobody.
			//
			// Idempotent. After the first pass the field's left edge already equals the caption's,
			// so recomputing gives the same rect - which matters because a refusal arriving while
			// the prompt is still up re-shows the SAME layout instance (GameSpyOpenOverlay only
			// re-runs runInit when it has to create one).
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

			// A refusal here means one thing and says one thing: RET_BAD_PASSWORD, which is the
			// host telling THIS client the password was wrong. Naming it discloses nothing the
			// player did not already have - handleJoinDeny only delivers a deny addressed to us
			// while m_pendingAction is ACT_JOIN (LANAPIhandlers.cpp:985-987), so the prompt can
			// only be on screen because that host answered us by name. A lobby that is not there
			// never opens this prompt at all; it times out into the base's message box exactly as
			// it did before this feature existed. Every other outcome is reported by
			// LANAPI::OnGameJoin rather than here.
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
	// screen and never draws - that exact trap cost a session on 16/08. Since TheSuperHackers
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
	// GeneralsX @fix Claude 17/08/2026 (cross-model review) TRIMMED, and the emptiness test is on
	// the trimmed value. The comment this replaces said the opposite and was wrong twice.
	//
	// It claimed trimming would break parity with the host, "which binds its password exactly as
	// typed, spaces included". No host does. applyLobbyCommand runs
	// `rest.nextToken(&verb); verb.toLower(); rest.trim();` before binding (LANAPI.cpp:1360-1364),
	// and every way of setting a lobby password - the host row in LanGameOptionsMenu, relayHostGame
	// in WOLLobbyMenu, a typed /password - goes through that one call. A password with an outer
	// space cannot be SET, so refusing to send one is parity, not a divergence from it.
	//
	// And it claimed the divergence was harmless. It was not, and it is now load-bearing: this
	// prompt clears alphaNumericalOnly on the entry box (see lanPromptApply) so that a joiner can
	// type the spaces and symbols a host's own 32-character row allows - which means whitespace
	// really does arrive here now. An untrimmed whitespace-only string builds "/password " with an
	// empty operand, and that is applyLobbyCommand's documented CLEAR: it wipes m_joinPassword, the
	// host-side verifier and the carried global. Worse, isHostingWithPassword() reads TRUE in
	// exactly that case, so s_lanPwBorrowed stays FALSE and lanPromptRelease will never put it
	// back - the player's own host password destroyed, silently, with no restore path.
	UnicodeString entered = typed;
	entered.trim();

	// Nothing typed is not a decision - leave the prompt up rather than clearing a password the
	// player may already hold and knocking again without one.
	if( entered.isEmpty() )
		return;

	// GeneralsX @fix Claude 17/08/2026 (cross-model review) A lobby that is not there any more is
	// reported as a lobby that is not there, not as a bad password.
	//
	// These two branches used to re-show the prompt saying the password was wrong, which is untrue
	// and leaves the only exit as Cancel. Nothing is disclosed by being honest: the prompt is only
	// on screen because this host answered US (see lanPromptApply), and RET_GAME_GONE is what the
	// base would have reported for the same join without the prompt in the way. Routed through
	// OnGameJoin rather than formatted here so it picks up the whole existing failure dialog -
	// including the borrowed-password hand-back on the way past.
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
/** LANAPI with the password prompt wired in. See the note on GXCreateLobbyLANAPI in
	* GUICallbacks.h for why this is a subclass and not a hook in LANAPI itself. */
//-------------------------------------------------------------------------------------------------
class GXLobbyLANAPI : public LANAPI
{
public:

	/// GeneralsX @bugfix Claude 18/09/2026 EVERY LOBBY TRANSPORT IS BORN IDENTIFIED.
	///
	/// A signed-in player was identified to the relay on ONE screen: the Online lobby handed the
	/// assertion over on entry (WOLLobbyMenu relayDeclareIdentity). Every other path that reaches a
	/// relay room builds a NEW LANAPI and a new Transport - a browser join (relayJoinSelectedGame), a
	/// Direct Connect join and SetUpOnlineLobbyLAN each `delete TheLAN` first - and a fresh Transport
	/// starts with an empty m_relayIdentity. So a player who JOINED somebody else's game was an
	/// unidentified member in that room for the whole game - and a relay that requires sign-in drops
	/// an unidentified member.
	///
	///
	/// HERE, rather than at each of those call sites, because they are the callers that keep being
	/// added - this is the one door they all go through (GXCreateLobbyLANAPI, then init()). The base
	/// class is untouched on purpose: LANAPI.{h,cpp} are inside the simulation source digest, and
	/// HeadlessLANAPI must NOT pick this up - a harness peer has no sign-in to declare.
	///
	/// AFTER LANAPI::init(), never before: init() binds the transport and sends the registration, and
	/// the relay silently discards an identity from an endpoint it has not filed as a member. From here
	/// the transport repeats it after every registration keepalive by itself (Transport.cpp
	/// sendRelayRegistration -> resendRelayIdentity).
	///
	/// A SIGNED-OUT PLAYER IS NOT RE-IDENTIFIED BY THIS. getVerifiedAssertionText() is "" once
	/// clearVerifiedIdentity() has run, and sendRelayIdentity("") on a transport that holds nothing
	/// declares nothing and settles no clear - so a sign-out still wins, and the clear it owes keeps
	/// riding this new transport's keepalives (GXRelayIdentityWire.h). It is a no-op for a LAN player,
	/// an offline player and anyone without an account.
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

		// GeneralsX @fix Claude 17/08/2026 (cross-model review, second pass) RET_GAME_GONE and
		// RET_TIMEOUT are reported verbatim. A previous revision flattened them into the prompt's
		// refusal line while a submitted password was outstanding; that was wrong twice over.
		//
		// It protected nothing. handleJoinDeny only delivers a deny that is addressed to us while
		// m_pendingAction is ACT_JOIN (LANAPIhandlers.cpp:985-987), so this prompt can only be open
		// because the host answered THIS client by name - the lobby's existence is disclosed by the
		// host, before any of this code runs, and the joiner clicked the row in the browser to get
		// here. Flattening added no secrecy.
		//
		// And it destroyed two diagnoses that matter. A host that quits between the prompt and the
		// retry, or a lost datagram on a bad link, told the player "check the password" forever
		// with no exit but Cancel. Worse, RET_TIMEOUT is the code LANAPICallbacks.cpp:903-910
		// appends LAN:SimIdTimeoutHint to, precisely because an incompatible peer usually presents
		// as a timeout - so the build-mismatch hint was swallowed on exactly the path a password
		// game is most likely to retry on.
		lanPromptRelease();
		LANAPI::OnGameJoin( ret, theGame );
	}
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
	// GeneralsX @feature Claude 17/08/2026 Whoever opened this popup decides what it is for. The
	// GameSpy path calls GameSpyOpenOverlay directly and sets nothing, so it always lands on FALSE;
	// only lanPromptOpen holds the latch across its own open call.
	s_lanMode = s_lanOpening;

	parentPopupID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:ParentJoinPopUp");
	parentPopup = TheWindowManager->winGetWindowFromId(nullptr, parentPopupID);

	textEntryGamePasswordID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:TextEntryGamePassword");
	textEntryGamePassword = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGamePasswordID);
	GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);

	// GeneralsX @feature Claude 17/08/2026 Asterisks, not the password, on BOTH paths.
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
		// GeneralsX @feature Claude 17/08/2026 A LAN/relay join. There is no staging room and
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
						// GeneralsX @feature Claude 17/08/2026 On the LAN/relay path there is
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
			// GeneralsX @fix Claude 17/08/2026 (cross-model review) The one teardown path this file
			// does not own.
			//
			// lanPromptRelease runs on ESC, on Cancel and on every OnGameJoin that is not a
			// password refusal - but Shell.cpp calls GameSpyCloseAllOverlays on every shell push
			// and pop, and that destroys the popup without either of those running. A prompt torn
			// down that way would leave s_lanPwBorrowed TRUE forever, and since applyLobbyCommand
			// also wrote setHostPassword and the process-global s_carriedLobbyPassword that a
			// freshly constructed LANAPI reads back (LANAPI.cpp:1370-1372, :215), the player's
			// NEXT hosted game would silently demand the password they typed to get into somebody
			// else's lobby.
			//
			// Keyed on window identity, NOT on s_lanMode. GWM_DESTROY is dispatched from
			// processDestroyList (GameWindowManager.cpp:118), i.e. deferred - so on the retry path
			// the OLD popup's destroy message can arrive AFTER the new one has been created and
			// s_lanMode set TRUE again. Comparing against the live parentPopup makes this fire
			// exactly once, for the instance nobody else cleaned up: lanPromptClose has already
			// nulled parentPopup on the paths it owns, and points it at the new popup on a retry.
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
				// GeneralsX @feature Claude 17/08/2026 See the ESC handler for why the LAN path
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

				// GeneralsX @feature Claude 17/08/2026 The LAN/relay retry. Split out before the
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
	// GeneralsX @bugfix Claude 17/08/2026 This used to print the password itself. A lobby password
	// is a credential people reuse, and DEBUG_LOG lands in a file that gets attached to bug
	// reports - so the one fact worth recording is whether a password was supplied, never which.
	DEBUG_LOG(("Attempting to join game %d(%ls)%s", ourRoom->getID(), ourRoom->getGameName().str(),
		password.isEmpty() ? "" : " with a password"));
	GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
	parentPopup = nullptr;
}
