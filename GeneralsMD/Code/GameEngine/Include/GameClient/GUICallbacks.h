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

// FILE: GUICallbacks.h ///////////////////////////////////////////////////////////////////////////
// Created:    Colin Day, June 2001
// Desc:       GUI Callbacks header
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "GameClient/GameWindow.h"

// EXTERNALS //////////////////////////////////////////////////////////////////////////////////////

//=================================================================================================
//																				Shell Menus																						 //
//=================================================================================================

// Main Menu --------------------------------------------------------------------------------------
extern void MainMenuInit( WindowLayout *layout, void *userData );
extern void MainMenuUpdate( WindowLayout *layout, void *userData );
extern void MainMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType MainMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType MainMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Single Player Menu -----------------------------------------------------------------------------
extern void SinglePlayerMenuInit( WindowLayout *layout, void *userData );
extern void SinglePlayerMenuUpdate( WindowLayout *layout, void *userData );
extern void SinglePlayerMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType SinglePlayerMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType SinglePlayerMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Options Menu -----------------------------------------------------------------------------------
extern void OptionsMenuInit( WindowLayout *layout, void *userData );
extern void OptionsMenuUpdate( WindowLayout *layout, void *userData );
extern void OptionsMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType OptionsMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType OptionsMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Map Select Menu --------------------------------------------------------------------------------
extern void MapSelectMenuInit( WindowLayout *layout, void *userData );
extern void MapSelectMenuUpdate( WindowLayout *layout, void *userData );
extern void MapSelectMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType MapSelectMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType MapSelectMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Replay Menu --------------------------------------------------------------------------------
extern void ReplayMenuInit( WindowLayout *layout, void *userData );
extern void ReplayMenuUpdate( WindowLayout *layout, void *userData );
extern void ReplayMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType ReplayMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ReplayMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Credits Menu --------------------------------------------------------------------------------
extern void CreditsMenuInit( WindowLayout *layout, void *userData );
extern void CreditsMenuUpdate( WindowLayout *layout, void *userData );
extern void CreditsMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType CreditsMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType CreditsMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );


// Score Screen --------------------------------------------------------------------------------
extern void ScoreScreenInit( WindowLayout *layout, void *userData );
extern void ScoreScreenUpdate( WindowLayout *layout, void *userData );
extern void ScoreScreenShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType ScoreScreenSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ScoreScreenInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Save/Load Menu ----------------------------------------------------------------------------------
extern void SaveLoadMenuInit( WindowLayout *layout, void *userData );
extern void SaveLoadMenuFullScreenInit( WindowLayout *layout, void *userData );
extern void SaveLoadMenuUpdate( WindowLayout *layout, void *userData );
extern void SaveLoadMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType SaveLoadMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType SaveLoadMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Popup Communicator ------------------------------------------------------------------------------
extern void PopupCommunicatorInit( WindowLayout *layout, void *userData );
extern void PopupCommunicatorUpdate( WindowLayout *layout, void *userData );
extern void PopupCommunicatorShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType PopupCommunicatorSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType PopupCommunicatorInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// SkirmishGameOptionsMenu
extern void SkirmishGameOptionsMenuInit( WindowLayout *layout, void *userData );
extern void SkirmishGameOptionsMenuUpdate( WindowLayout *layout, void *userData );
extern void SkirmishGameOptionsMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType SkirmishGameOptionsMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType SkirmishGameOptionsMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// SkirmishMapSelectMenu
extern void SkirmishMapSelectMenuInit( WindowLayout *layout, void *userData );
extern void SkirmishMapSelectMenuUpdate( WindowLayout *layout, void *userData );
extern void SkirmishMapSelectMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType SkirmishMapSelectMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType SkirmishMapSelectMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Generals' Challenge Mode Menu
extern void ChallengeMenuInit( WindowLayout *layout, void *userData );
extern void ChallengeMenuUpdate( WindowLayout *layout, void *userData );
extern void ChallengeMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType ChallengeMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ChallengeMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Keyboard Options Menu ---------------------------------------------------------------------------
extern void KeyboardOptionsMenuInit( WindowLayout *layout, void *userData );
extern void KeyboardOptionsMenuUpdate( WindowLayout *layout, void *userData );
extern void KeyboardOptionsMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType KeyboardOptionsMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType KeyboardOptionsMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Lan Lobby Menu ----------------------------------------------------------------------------------
extern void LanLobbyMenuInit( WindowLayout *layout, void *userData );
extern void LanLobbyMenuUpdate( WindowLayout *layout, void *userData );
extern void LanLobbyMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType LanLobbyMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType LanLobbyMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Lan Game Options Menu ---------------------------------------------------------------------------
extern void LanGameOptionsMenuInit( WindowLayout *layout, void *userData );
extern void LanGameOptionsMenuUpdate( WindowLayout *layout, void *userData );
extern void LanGameOptionsMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType LanGameOptionsMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType LanGameOptionsMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Lan Map Select Menu -----------------------------------------------------------------------------
extern void LanMapSelectMenuInit( WindowLayout *layout, void *userData );
extern void LanMapSelectMenuUpdate( WindowLayout *layout, void *userData );
extern void LanMapSelectMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType LanMapSelectMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType LanMapSelectMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Lan Map Select Menu -----------------------------------------------------------------------------
extern void GameInfoWindowInit( WindowLayout *layout, void *userData );
extern WindowMsgHandledType GameInfoWindowSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// (Patch) Download Menu ----------------------------------------------------------------------------------
extern void DownloadMenuInit( WindowLayout *layout, void *userData );
extern void DownloadMenuUpdate( WindowLayout *layout, void *userData );
extern void DownloadMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType DownloadMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType DownloadMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Extras Menu (SagePatch QoL) ----------------------------------------------------------------------------------
extern void ExtrasMenuInit( WindowLayout *layout, void *userData );
extern void ExtrasMenuUpdate( WindowLayout *layout, void *userData );
extern void ExtrasMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType ExtrasMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ExtrasMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Popup host Game Internet -----------------------------------------------------------------------------------
extern void DifficultySelectInit( WindowLayout *layout, void *userData );
extern WindowMsgHandledType DifficultySelectSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType DifficultySelectInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );


//=================================================================================================
//																				WOL UI																								 //
//=================================================================================================
//  WOL Ladder Screen ---------------------------------------------------------------------------------
extern void WOLLadderScreenInit( WindowLayout *layout, void *userData );
extern void WOLLadderScreenUpdate( WindowLayout *layout, void *userData );
extern void WOLLadderScreenShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLLadderScreenSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLLadderScreenInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Login Menu ---------------------------------------------------------------------------------
extern void WOLLoginMenuInit( WindowLayout *layout, void *userData );
extern void WOLLoginMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLLoginMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLLoginMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLLoginMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Locale Select Popup ---------------------------------------------------------------------------------
extern void WOLLocaleSelectInit( WindowLayout *layout, void *userData );
extern void WOLLocaleSelectUpdate( WindowLayout *layout, void *userData );
extern void WOLLocaleSelectShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLLocaleSelectSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLLocaleSelectInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Message Window ------------------------------------------------------------------------------
extern void WOLMessageWindowInit( WindowLayout *layout, void *userData );
extern void WOLMessageWindowUpdate( WindowLayout *layout, void *userData );
extern void WOLMessageWindowShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLMessageWindowSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLMessageWindowInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Quick Match Menu ----------------------------------------------------------------------------
extern void WOLQuickMatchMenuInit( WindowLayout *layout, void *userData );
extern void WOLQuickMatchMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLQuickMatchMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLQuickMatchMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLQuickMatchMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Welcome Menu --------------------------------------------------------------------------------
extern void WOLWelcomeMenuInit( WindowLayout *layout, void *userData );
extern void WOLWelcomeMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLWelcomeMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLWelcomeMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLWelcomeMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Welcome Menu --------------------------------------------------------------------------------
extern void WOLStatusMenuInit( WindowLayout *layout, void *userData );
extern void WOLStatusMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLStatusMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLStatusMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLStatusMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Quickmatch Score Screen --------------------------------------------------------------------------------
extern void WOLQMScoreScreenInit( WindowLayout *layout, void *userData );
extern void WOLQMScoreScreenUpdate( WindowLayout *layout, void *userData );
extern void WOLQMScoreScreenShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLQMScoreScreenSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLQMScoreScreenInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Lobby Menu ---------------------------------------------------------------------------------
extern void WOLLobbyMenuInit( WindowLayout *layout, void *userData );
extern void WOLLobbyMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLLobbyMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLLobbyMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLLobbyMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Game Setup Menu ---------------------------------------------------------------------------------
extern void WOLGameSetupMenuInit( WindowLayout *layout, void *userData );
extern void WOLGameSetupMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLGameSetupMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLGameSetupMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLGameSetupMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Custom Score Screen --------------------------------------------------------------------------------
extern void WOLCustomScoreScreenInit( WindowLayout *layout, void *userData );
extern void WOLCustomScoreScreenUpdate( WindowLayout *layout, void *userData );
extern void WOLCustomScoreScreenShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLCustomScoreScreenSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLCustomScoreScreenInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Map Select Overlay ---------------------------------------------------------------------------------
extern void WOLMapSelectMenuInit( WindowLayout *layout, void *userData );
extern void WOLMapSelectMenuUpdate( WindowLayout *layout, void *userData );
extern void WOLMapSelectMenuShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLMapSelectMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLMapSelectMenuInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  WOL Buddy Overlay ---------------------------------------------------------------------------------
extern void WOLBuddyOverlayInit( WindowLayout *layout, void *userData );
extern void WOLBuddyOverlayUpdate( WindowLayout *layout, void *userData );
extern void WOLBuddyOverlayShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLBuddyOverlaySystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType WOLBuddyOverlayInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// WOL Buddy Overlay Right Click menu callbacks --------------------------------------------------------------
extern void WOLBuddyOverlayRCMenuInit( WindowLayout *layout, void *userData );
extern WindowMsgHandledType WOLBuddyOverlayRCMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//  GameSpy Player Info Overlay ---------------------------------------------------------------------------------
extern void GameSpyPlayerInfoOverlayInit( WindowLayout *layout, void *userData );
extern void GameSpyPlayerInfoOverlayUpdate( WindowLayout *layout, void *userData );
extern void GameSpyPlayerInfoOverlayShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType GameSpyPlayerInfoOverlaySystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType GameSpyPlayerInfoOverlayInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Popup host Game Internet -----------------------------------------------------------------------------------
extern void PopupHostGameInit( WindowLayout *layout, void *userData );
extern void PopupHostGameUpdate( WindowLayout * layout, void *userData);
extern WindowMsgHandledType PopupHostGameSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType PopupHostGameInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Popup InGame Message Box -----------------------------------------------------------------------------------
extern void InGamePopupMessageInit( WindowLayout *layout, void *userData );
extern WindowMsgHandledType InGamePopupMessageSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType InGamePopupMessageInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Popup join Game Internet -----------------------------------------------------------------------------------
extern void PopupJoinGameInit( WindowLayout *layout, void *userData );
extern WindowMsgHandledType PopupJoinGameSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType PopupJoinGameInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// GeneralsX: the lobby password prompt is a LANAPI subclass, not an edit to LANAPI.
// OnGameJoin is in Core/GameNetwork, inside the sourceID digest: editing it would make every peer refuse every join.
// The override turns RET_NEEDS_PASSWORD / RET_BAD_PASSWORD into a prompt and hands the rest to LANAPI::OnGameJoin.
// Use it instead of NEW LANAPI() in menus that build TheLAN; the base class only gives the old /password message.
class LANAPI;
extern LANAPI *GXCreateLobbyLANAPI( void );

//  Network Direct ConnectWindow ---------------------------------------------------------------------------------
/// GeneralsX: Matchmaking. Build TheLAN for an online screen, in the one order that works.
///
/// Extracted verbatim out of NetworkDirectConnectInit so the WOL lobby cannot drift from it. The
/// sequence - throw TheLAN away, ask the relay for an identity in OUR host room, re-new, init,
/// SetLocalIP - is ordered because LANGameInfo snapshots the local IP in its constructor, and
/// AmIHost and slot matching read that snapshot rather than TheLAN. An identity that lands after a
/// LANGameInfo exists reaches nothing that decides anything.
///
/// Returns TRUE when relay mode came up, FALSE when this is a plain LAN (in which case TheLAN is
/// still built, just against an interface address).
extern Bool SetUpOnlineLobbyLAN( void );

// GeneralsX: /nickname, applied to the relay nickname, the LAN slot-list name and the persisted preference together.
// Takes a wide string and normalises inside (space-trim, LAN , : ; strip, g_lanPlayerNameLength cap, ASCII only).
// outApplied returns the normalised value; returns FALSE and fills outNote on rejection.
extern Bool GXApplyNickname( const UnicodeString &requested, UnicodeString &outNote,
							 UnicodeString *outApplied = nullptr );

// GeneralsX: Hosting through the stock PopupHostGame on the relay path.
//
// PopupHostGame.wnd is shared by two lobbies that have nothing else in common: the GameSpy one,
// whose Create button builds a PeerRequest and hands it to TheGameSpyPeerMessageQueue, and ours,
// where that queue is NULL by construction (it is the switch the whole relay lobby is built on -
// see WOLLobbyMenu's s_relayLobby). Dereferencing it would be a null call on every relay host, so
// the popup must ask which lobby it is in before it acts. That is what these two are for.
extern Bool GXRelayLobbyIsActive( void );
extern void GXRelayHostGameWithOptions( const UnicodeString &gameName, const AsciiString &password );

// GeneralsX: The retail buddy overlay on the relay path.
//
// WOLBuddyOverlay.wnd re-pointed at the local friends store. The layout is owned by
// WOLLobbyMenu.cpp (created directly - GameSpyOpenOverlay null-derefs the buddy queue the relay
// path never creates, and its overlayLayouts[] table would never pump or destroy a layout it does
// not hold); the screen internals live in WOLBuddyOverlay.cpp behind the gx-mode flag, set through
// the explicit setter so the two files share no statics. Open/Close/Toggle are idempotent; the
// Shell transitions and the lobby screen's Shutdown call Close exactly as they do for the panel.
extern Bool GXBuddyOverlayIsOpen( void );
extern void GXBuddyOverlayOpen( void );
extern void GXBuddyOverlayClose( void );
extern void GXBuddyOverlayToggle( void );
extern void GXBuddyOverlaySetGxMode( Bool on );		///< defined in WOLBuddyOverlay.cpp
extern void GXBuddyCloseRCMenu( void );				///< RC menus are lone layouts; transitions close them explicitly
/// GeneralsX: Runs the overlay's update wherever it is open - lobby, game room or
/// match - from GameClient::update. It used to be pumped by the lobby screen alone, which went deaf the
/// moment the overlay could be opened anywhere else.
extern void GXBuddyOverlayPump( void );
/// GeneralsX: Does the Friends key (MetaEvent.cpp, F5 by default) belong to
/// Friends right now? Only in a live relay session - the relay lobby, a relay game room, a relay match
/// The rule: GameClient/GXFriendsKey.h.
extern Bool GXFriendsKeyHere( void );

/// GeneralsX: Presence of one id: 1 online, 0 offline, -1 not known.
/// The lobby's already-decoded GXHERE answer - never a second Transport consumer;
/// takeRelayPresence is a destructive read with exactly one permitted caller.
extern Int GXRelayPresenceFor( const char *id );

// GeneralsX: The card exchange, overlay-driven. GXRelayCardAction is
// the ONE writer of the fq/fa/fi pending state: the overlay's right-click handler maps its
// buttons onto these actions and every piece of trust logic - roster revalidation against the
// snapshot, send-before-mutate ordering, durable-id resolution at action time, decline-in-silence
// - lives with the latches in WOLLobbyMenu.cpp. Returns FALSE when it acted on nothing (roster
// moved, send failed, no local id), which the caller treats as "the moment passed", not an error.
enum
{
	GX_RC_ACT_ADD    = 0,	///< stranger: offer our card ("fq"); pending row: accept ("fa")
	GX_RC_ACT_DELETE = 1,	///< pending row: decline in silence; friend: /remove by resolved id
	GX_RC_ACT_IGNORE = 2,	///< /ignore or /unignore P<slot> through the social-command seam
};
extern Bool GXRelayCardAction( Int action, Int slot, const AsciiString &nickSnapshot );

/// The overlay's pending-request rows: slots whose occupant sent "fq", with the latched nick.
extern Int  GXRelayPendingRequestCount( void );
extern Bool GXRelayPendingRequestAt( Int index, Int *outSlot, AsciiString *outNick );

/// Open one of the retail RC menus for a ROSTER row (BY_SLOT payload). Defined in
/// WOLBuddyOverlay.cpp beside the overlay's own creation sites so the clamp, the hidden
/// Stats/Play buttons and the payload shape can never drift apart between surfaces.
extern void GXBuddyOpenRosterRCMenu( const char *layoutName, Int slot,
									 const AsciiString &nickSnapshot, Int mouseX, Int mouseY,
									 Bool flipIgnoreLabel );

// GeneralsX: Sign-out, from the player's own name in the Online lobby.
//
// GXBuddyOpenSelfRCMenu opens EA's retail RCLocalPlayerMenu.wnd with its one button relabelled
// "Sign Out" (WOLBuddyOverlay.cpp). Pressing it calls GXLobbyAskSignOut, which only RAISES A FLAG:
// WOLLobbyMenuUpdate shows the confirmation on the next frame, and the confirmation's Yes raises a
// second flag that the next frame acts on. Neither message box is ever opened from inside another
// window's callback. Both live in WOLLobbyMenu.cpp, which owns the lobby they act on.
extern void GXBuddyOpenSelfRCMenu( Int mouseX, Int mouseY );
extern void GXLobbyAskSignOut( void );

// GeneralsX: the one seam between the code-built friends panel and the typed social commands.
// Takes a whole "/verb rest" line and calls relayHandleSocialCommand, so /friend, /ignore, /unignore, /msg, /mycode and
// /friendsonly share one implementation. Returns TRUE when the line was consumed.
// Not routed through the chat text entry, which would need a focused box and echo the command back.
extern Bool GXRelayRunSocialCommand( const AsciiString &line );

// GeneralsX: The CodeGui friends panel (LobbyFriendsPanel.cpp) is
// DELETED - the retail overlay above replaced it, driven through this same social-command seam.
// The panel's own reasoning ("the shipped .wnd has no Add/Delete/Yes/No buttons") was correct and
// still is: the overlay does not use those buttons either - add and accept live on the retail
// right-click menus, exactly as EA shipped them.

extern void NetworkDirectConnectInit( WindowLayout *layout, void *userData );
extern void NetworkDirectConnectUpdate( WindowLayout *layout, void *userData );
extern void NetworkDirectConnectShutdown( WindowLayout *layout, void *userData );
extern WindowMsgHandledType NetworkDirectConnectSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType NetworkDirectConnectInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

//=================================================================================================
//																				IME UI																								 //
//=================================================================================================

// IME Candidate Window
extern WindowMsgHandledType IMECandidateWindowSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType IMECandidateWindowInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern void IMECandidateMainDraw( GameWindow *window, WinInstanceData *instData );
extern void IMECandidateTextAreaDraw( GameWindow *window, WinInstanceData *instData );


//=================================================================================================
//																				In Game UI																						 //
//=================================================================================================

// Control Bar ------------------------------------------------------------------------------------
extern WindowMsgHandledType ControlBarSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ControlBarObserverSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ControlBarInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType LeftHUDInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern void ToggleControlBar( Bool immediate = TRUE );
extern void HideControlBar( Bool immediate = TRUE );
extern void ShowControlBar( Bool immediate = TRUE );
// GeneralsX: TOGGLE_GAME_HUD. Declared in BOTH games' GUICallbacks.h
// because it is defined in Core/ControlBarCallback.cpp, which compiles into both.
extern void ToggleGameHud();


// Replay Controls --------------------------------------------------------------------------------
extern WindowMsgHandledType ReplayControlSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType ReplayControlInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );


// InGame Chat Controls --------------------------------------------------------------------------------
extern WindowMsgHandledType InGameChatSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType InGameChatInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
enum InGameChatType CPP_11(: Int)
{
	INGAME_CHAT_ALLIES,
	INGAME_CHAT_EVERYONE,
	INGAME_CHAT_PLAYERS
};
extern void ToggleInGameChat( Bool immediate = TRUE );
extern void HideInGameChat( Bool immediate = TRUE );
extern void ShowInGameChat( Bool immediate = TRUE );
void ResetInGameChat();
void SetInGameChatType( InGameChatType chatType );
Bool IsInGameChatActive();


// Diplomacy Controls --------------------------------------------------------------------------------
WindowMsgHandledType DiplomacySystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
WindowMsgHandledType DiplomacyInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
void ToggleDiplomacy( Bool immediate = TRUE );
void HideDiplomacy( Bool immediate = TRUE );
void ResetDiplomacy();

// Generals Exp Points  --------------------------------------------------------------------------------
WindowMsgHandledType GeneralsExpPointsSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
WindowMsgHandledType GeneralsExpPointsInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );


// IdleWorker Controls --------------------------------------------------------------------------------
WindowMsgHandledType IdleWorkerSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Disconnect Menu --------------------------------------------------------------------------------
extern void ShowDisconnectWindow();
extern void HideDisconnectWindow();
extern WindowMsgHandledType DisconnectControlSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType DisconnectControlInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Establish Connections Window -------------------------------------------------------------------
extern void ShowEstablishConnectionsWindow();
extern void HideEstablishConnectionsWindow();
extern WindowMsgHandledType EstablishConnectionsControlSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType EstablishConnectionsControlInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// The in game quit menu --------------------------------------------------------------------------
extern void destroyQuitMenu();
extern Bool canOpenQuitMenu();
extern void ToggleQuitMenu();
extern void HideQuitMenu();
extern WindowMsgHandledType QuitMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Message of the Day -----------------------------------------------------------------------------
extern WindowMsgHandledType MOTDSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// Message Box --------------------------------------------------------------------------------
extern WindowMsgHandledType MessageBoxSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
extern WindowMsgHandledType QuitMessageBoxSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );
