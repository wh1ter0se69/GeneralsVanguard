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
// FILE: WOLBuddyOverlay.cpp
// Author: Chris Huybregts, November 2001
// Description: Lan Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
// GeneralsX: wcslcpy is a BSD/macOS libc function with no glibc
// equivalent, so this compiles on macOS and fails on Linux. WWLib/stringex.h provides a portable
// inline fallback guarded by HAVE_WCSLCPY - the same one LANAPI.cpp already relies on.
#include "WWLib/stringex.h"

#include "Common/AudioEventRTS.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "GameClient/GameText.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/Display.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/BuddyThread.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/GameSpy/ThreadUtils.h"
#include "GameClient/GUICallbacks.h"	// GeneralsX: GXBuddyOverlayClose
#include "GameClient/GXPlayerIdentity.h"	// GeneralsX: the local store
#include "GameClient/GXBuddyRCData.h"	// GeneralsX: gx RC payload

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////


// window ids ------------------------------------------------------------------------------
static NameKeyType parentID = NAMEKEY_INVALID;
static NameKeyType buttonHideID = NAMEKEY_INVALID;
static NameKeyType buttonAddBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonDeleteBuddyID = NAMEKEY_INVALID;
static NameKeyType textEntryID = NAMEKEY_INVALID;
static NameKeyType listboxBuddyID = NAMEKEY_INVALID;
static NameKeyType listboxChatID = NAMEKEY_INVALID;
static NameKeyType buttonAcceptBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonDenyBuddyID = NAMEKEY_INVALID;
static NameKeyType radioButtonBuddiesID = NAMEKEY_INVALID;
static NameKeyType radioButtonIgnoreID = NAMEKEY_INVALID;
static NameKeyType parentBuddiesID = NAMEKEY_INVALID;
static NameKeyType parentIgnoreID = NAMEKEY_INVALID;
static NameKeyType listboxIgnoreID = NAMEKEY_INVALID;
static NameKeyType buttonNotificationID = NAMEKEY_INVALID;


// Window Pointers ------------------------------------------------------------------------
static GameWindow *parent = nullptr;
static GameWindow *buttonHide = nullptr;
static GameWindow *buttonAddBuddy = nullptr;
static GameWindow *buttonDeleteBuddy = nullptr;
static GameWindow *textEntry = nullptr;
static GameWindow *listboxBuddy = nullptr;
static GameWindow *listboxChat = nullptr;
static GameWindow *buttonAcceptBuddy = nullptr;
static GameWindow *buttonDenyBuddy = nullptr;
static GameWindow *radioButtonBuddies = nullptr;
static GameWindow *radioButtonIgnore = nullptr;
static GameWindow *parentBuddies = nullptr;
static GameWindow *parentIgnore = nullptr;
static GameWindow *listboxIgnore = nullptr;

static Bool isOverlayActive = false;
void insertChat( BuddyMessage msg );

// GeneralsX: gx mode: this screen re-pointed at the local friends store.
//
// On the relay path there is no GameSpy behind these controls - TheGameSpyBuddyMessageQueue is
// never created and getLocalProfileID() is 0 - so every GameSpy-coupled body below either
// early-outs or branches on this flag. The flag is private to this file behind an explicit
// setter: the layout is owned and pumped by WOLLobbyMenu.cpp (GXBuddyOverlayOpen/Close/Toggle),
// which sets the mode BEFORE runInit and clears it AFTER destruction, so init and shutdown both
// run under the mode they were opened in and a later GameSpy-path overlay cannot inherit it.
// The last list state rendered, so the 1 Hz tick rebuilds ONLY when something changed.
// GadgetListBoxReset clears the selection and scroll position, and a user who takes more than a
// second to type a message would reach Enter with nothing selected (cross-model review's catch).
// Cleared when gx mode turns on, because reopening the overlay recreates an empty listbox that
// must be drawn whatever the store says.
static AsciiString s_gxListFingerprint;
static AsciiString s_gxIgnoreFingerprint;

static Bool s_gxMode = FALSE;
void GXBuddyOverlaySetGxMode( Bool on )
{
	s_gxMode = (on ? TRUE : FALSE);
	if (s_gxMode)
	{
		// A fresh open recreates empty listboxes, which must be drawn whatever the store says.
		// A sentinel, not an empty string: an empty ignore store hashes to "" too, and clearing
		// to "" would make the first refresh a no-op and leave the tab blank instead of saying
		// "Nobody ignored".
		s_gxListFingerprint = "!fresh";
		s_gxIgnoreFingerprint = "!fresh";
	}
}

// gx row payloads. The listbox item-data carries INDEX+1 into these arrays - 0 means an info row
// with nothing behind it, so a click on "No friends yet" can never act on anything. The arrays
// are rewritten wholesale on every refresh and the ids are COPIED in: GXPlayerIdentity's returned
// pointers die on the next store mutation and row indices have no stability across refreshes -
// the LobbyFriendsPanel idiom, kept for the same reasons.
enum { GX_BUDDY_ROWS = 128 };
static char s_gxBuddyRowId[GX_BUDDY_ROWS][GX_PLAYER_ID_LEN + 1];
static char s_gxIgnoreRowId[GX_BUDDY_ROWS][GX_PLAYER_ID_LEN + 1];
// RightClick pointers ---------------------------------------------------------------------
static GameWindow *rcMenu = nullptr;
static WindowLayout *noticeLayout = nullptr;
static UnsignedInt noticeExpires = 0;
enum { NOTIFICATION_EXPIRES = 3000 };

void setUnignoreText( WindowLayout *layout, AsciiString nick, GPProfile id);
void refreshIgnoreList();
// GeneralsX: gx RC menu creation, shared by both listboxes and the
// lobby roster (through GXBuddyOpenRosterRCMenu).
static void gxOpenRCMenu( const char *layoutName, Int gxKind, const char *id, Int slot,
						  UnicodeString nickWide, Int mouseX, Int mouseY, RCItemType itemType,
						  Bool flipIgnoreLabel );
// The latched nick behind each pending-request row, keyed by SLOT; written at render, read by the
// right-click so the payload snapshot is the exact name the human saw - the P<n> spelling in the
// row is presentation, never identity.
static AsciiString s_gxPendingNick[9];
void showNotificationBox( AsciiString nick, UnicodeString message);
void deleteNotificationBox();
static Bool lastNotificationWasStatus = FALSE;
static Int numOnlineInNotification = 0;

class BuddyControls
{
public:
	BuddyControls();
	GameWindow *listboxChat;
	NameKeyType listboxChatID;

	GameWindow *listboxBuddies;
	NameKeyType listboxBuddiesID;

	GameWindow *textEntryEdit;
	NameKeyType textEntryEditID;
	Bool isInit;
};

static BuddyControls buddyControls;
BuddyControls::BuddyControls(	)
{
	listboxChat = nullptr;
	listboxChatID = NAMEKEY_INVALID;
	listboxBuddies = nullptr;
	listboxBuddiesID = NAMEKEY_INVALID;
	textEntryEdit = nullptr;
	textEntryEditID = NAMEKEY_INVALID;
	isInit = FALSE;
}
// At this point I don't give a damn about how good this way is.  I'm doing it anyway.
enum
{
	BUDDY_RESETALL_CRAP = -1,
	BUDDY_WINDOW_BUDDIES = 0,
	BUDDY_WINDOW_DIPLOMACY,
	BUDDY_WINDOW_WELCOME_SCREEN,
};

void InitBuddyControls(Int type)
{
	// GeneralsX: gx mode needs no TheGameSpyInfo: its list is the local store.
	// The relay lobby happens to own one, but the game room and a match - where F5 now opens this overlay
	// too - do not (WOLLobbyMenuShutdown tears it down), and requiring it left the list empty there.
	// Only the overlay's own controls; the in-game Diplomacy pane keeps the GameSpy rule.
	if(!TheGameSpyInfo && !(s_gxMode && type == BUDDY_WINDOW_BUDDIES))
	{
		buddyControls.textEntryEditID = NAMEKEY_INVALID;
		buddyControls.textEntryEdit = nullptr;
		buddyControls.listboxBuddiesID = NAMEKEY_INVALID;
		buddyControls.listboxChatID = NAMEKEY_INVALID;
		buddyControls.listboxBuddies = nullptr;
		buddyControls.listboxChat = nullptr;
		buddyControls.isInit = FALSE;
		return;
	}
	switch (type) {
	case BUDDY_RESETALL_CRAP:
		buddyControls.textEntryEditID = NAMEKEY_INVALID;
		buddyControls.textEntryEdit = nullptr;
		buddyControls.listboxBuddiesID = NAMEKEY_INVALID;
		buddyControls.listboxChatID = NAMEKEY_INVALID;
		buddyControls.listboxBuddies = nullptr;
		buddyControls.listboxChat = nullptr;
		buddyControls.isInit = FALSE;
	break;
	case BUDDY_WINDOW_BUDDIES:
		buddyControls.textEntryEditID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:TextEntryChat" );
		buddyControls.textEntryEdit = TheWindowManager->winGetWindowFromId(nullptr,  buddyControls.textEntryEditID);
		buddyControls.listboxBuddiesID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ListboxBuddies" );
		buddyControls.listboxChatID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ListboxBuddyChat" );
		buddyControls.listboxBuddies = TheWindowManager->winGetWindowFromId( nullptr,  buddyControls.listboxBuddiesID );
		buddyControls.listboxChat = TheWindowManager->winGetWindowFromId( nullptr,  buddyControls.listboxChatID);
		GadgetTextEntrySetText(buddyControls.textEntryEdit, UnicodeString::TheEmptyString);
		buddyControls.isInit = TRUE;
		break;
	case BUDDY_WINDOW_DIPLOMACY:
		buddyControls.textEntryEditID = TheNameKeyGenerator->nameToKey( "Diplomacy.wnd:TextEntryChat" );
		buddyControls.textEntryEdit = TheWindowManager->winGetWindowFromId(nullptr,  buddyControls.textEntryEditID);
		buddyControls.listboxBuddiesID = TheNameKeyGenerator->nameToKey( "Diplomacy.wnd:ListboxBuddies" );
		buddyControls.listboxChatID = TheNameKeyGenerator->nameToKey( "Diplomacy.wnd:ListboxBuddyChat" );
		buddyControls.listboxBuddies = TheWindowManager->winGetWindowFromId( nullptr,  buddyControls.listboxBuddiesID );
		buddyControls.listboxChat = TheWindowManager->winGetWindowFromId( nullptr,  buddyControls.listboxChatID);
		GadgetTextEntrySetText(buddyControls.textEntryEdit, UnicodeString::TheEmptyString);
		buddyControls.isInit = TRUE;
		break;
	case BUDDY_WINDOW_WELCOME_SCREEN:
		break;
	default:
		DEBUG_CRASH(("Well, you really shouldn't have gotten here, if you really care about GUI Bugs, search for this string, you you don't care, call chris (who probably doesn't care either"));
	}

}

WindowMsgHandledType BuddyControlSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2)
{
	// GeneralsX: gx mode has no GameSpy profile - that is the point -
	// so the profile gate applies only to the GameSpy path. TheGameSpyInfo itself still exists on
	// the relay (SetUpGameSpyForRelayLobby creates it) and every gx body below is its own branch.
	// GeneralsX: ...and, since F5 opens the overlay outside the lobby, with no
	// TheGameSpyInfo at all in gx mode: every gx branch below reads the local store, never that object.
	if((!s_gxMode && (!TheGameSpyInfo || TheGameSpyInfo->getLocalProfileID() == 0)) || !buddyControls.isInit)
	{
		return MSG_IGNORED;
	}

	switch( msg )
	{
		case GLM_RIGHT_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if( controlID == buddyControls.listboxBuddiesID )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout;
					if(rc->pos < 0)
						break;

					// GeneralsX: gx: friend rows carry INDEX+1 into the
					// copied-id array, pending-request rows carry the NEGATED room slot, and 0 is
					// an info row that gets no menu.
					if (s_gxMode)
					{
						const Int idx = static_cast<Int>(reinterpret_cast<intptr_t>(
							GadgetListBoxGetItemData(control, rc->pos, 0)));
						if (idx == 0 || idx < -8 || idx > GX_BUDDY_ROWS)
							break;
						GadgetListBoxSetSelected(control, rc->pos);
						if (idx < 0)
						{
							// A pending-request row: BY_SLOT, snapshot = the latched nick the row
							// was rendered from (raw, possibly empty - P<n> is presentation).
							const Int slot = -idx;
							gxOpenRCMenu("Menus/RCBuddyRequestMenu.wnd", GXRCMenuData::BY_SLOT,
								NULL, slot, UnicodeString::TheEmptyString, rc->mouseX, rc->mouseY,
								ITEM_REQUEST, FALSE);
							// The payload nick must be the raw latch, not the rendered text.
							if (rcMenu != nullptr)
							{
								GXRCMenuData *pd = (GXRCMenuData *)rcMenu->winGetUserData();
								if (pd != nullptr && pd->m_id == (GPProfile)GX_RC_PROFILE_SENTINEL
										&& slot >= 1 && slot <= 8)
									pd->m_nick = s_gxPendingNick[slot];
							}
							break;
						}
						gxOpenRCMenu("Menus/RCBuddiesMenu.wnd", GXRCMenuData::BY_ID,
							s_gxBuddyRowId[idx - 1], 0, GadgetListBoxGetText(control, rc->pos),
							rc->mouseX, rc->mouseY, ITEM_BUDDY,
							GXPlayerIdentity::isIgnored(s_gxBuddyRowId[idx - 1]));
						break;
					}

					// GeneralsX: 64-bit safe pointer cast
					GPProfile profileID = static_cast<GPProfile>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rc->pos, 0)));
					// GeneralsX: 64-bit safe pointer cast
					RCItemType itemType = static_cast<RCItemType>(static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rc->pos, 1))));
					UnicodeString nick = GadgetListBoxGetText(control, rc->pos);

					GadgetListBoxSetSelected(control, rc->pos);
					if (itemType == ITEM_BUDDY)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddiesMenu.wnd");
					else if (itemType == ITEM_REQUEST)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddyRequestMenu.wnd");
					else
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCNonBuddiesMenu.wnd");
					rcMenu = rcLayout->getFirstWindow();
					rcMenu->winGetLayout()->runInit();
					rcMenu->winBringToTop();
					rcMenu->winHide(FALSE);


					ICoord2D rcSize, rcPos;
					rcMenu->winGetSize(&rcSize.x, &rcSize.y);
					rcPos.x = rc->mouseX;
					rcPos.y = rc->mouseY;
					if(rc->mouseX + rcSize.x > TheDisplay->getWidth())
						rcPos.x = TheDisplay->getWidth() - rcSize.x;
					if(rc->mouseY + rcSize.y > TheDisplay->getHeight())
						rcPos.y = TheDisplay->getHeight() - rcSize.y;
					rcMenu->winSetPosition(rcPos.x, rcPos.y);


					GameSpyRCMenuData *rcData = NEW GameSpyRCMenuData;
					rcData->m_id = profileID;
					rcData->m_nick.translate(nick);
					rcData->m_itemType = itemType;
					setUnignoreText(rcLayout, rcData->m_nick, rcData->m_id);
					rcMenu->winSetUserData((void *)rcData);
					TheWindowManager->winSetLoneWindow(rcMenu);
				}
				else
					return MSG_IGNORED;
				break;
			}
			case GEM_EDIT_DONE:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if(controlID != buddyControls.textEntryEditID)
					return MSG_IGNORED;

				// GeneralsX: gx: the pane sends a private message to the
				// selected FRIEND row, dispatched BY ID through the one social-command seam -
				// never by P<slot>, because the resolver revalidates id->slot at send time and a
				// roster reshuffle between selection and send must not deliver to a replacement
				// occupant. A friend with no bound slot (not in this room, or codes never
				// swapped) is refused by the resolver; its refusal line prints in the lobby chat,
				// which this overlay floats above. The peer-line channel is byte-oriented, so the
				// text narrows exactly as a typed /msg does - same constraint, same place.
				if (s_gxMode)
				{
					// GeneralsX: LOBBY ONLY (-B P2 on 7a6121659). F5 opens this
					// overlay in a game room and a match too, but private messages are the lobby screen's:
					// replies are drained, and every refusal printed, only while it runs (WOLLobbyMenuUpdate).
					// Sending from anywhere else would go out with no visible outcome and no reply ever shown,
					// so say so here. The typed text stays in the box only while this overlay is open: every
					// screen change closes it (Shell::doPush/doPop), so promising it back in the lobby would lie.
					if (!GXRelayLobbyIsActive())
					{
						if (buddyControls.listboxChat)
							GadgetListBoxAddEntryText(buddyControls.listboxChat,
								TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBuddyChatLobbyOnly",
									L"Private messages work in the online lobby only."),
								GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
						break;
					}
					UnicodeString entry;
					entry.set(GadgetTextEntryGetText(buddyControls.textEntryEdit));
					GadgetTextEntrySetText(buddyControls.textEntryEdit, UnicodeString::TheEmptyString);
					entry.trim();
					if (entry.isEmpty())
						break;

					Int selected = -1;
					GadgetListBoxGetSelected(buddyControls.listboxBuddies, &selected);
					Int idx = 0;
					if (selected >= 0)
					{
						// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
						idx = static_cast<Int>(reinterpret_cast<intptr_t>(
							GadgetListBoxGetItemData(buddyControls.listboxBuddies, selected, 0)));
					}
					if (idx <= 0 || idx > GX_BUDDY_ROWS)
					{
						// No friend row selected (info and pending rows do not receive messages).
						if (buddyControls.listboxChat)
							GadgetListBoxAddEntryText(buddyControls.listboxChat,
								TheGameText->fetch("Buddy:SelectBuddyToChat"),
								GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
						break;
					}

					// No pane echo: relayHandleSocialCommand returns TRUE for its error paths too,
					// so a pane echo gated on it would claim delivery for a refused send. The
					// /msg machinery already prints both the sent line and any refusal into the
					// lobby chat this overlay floats above - one record, one place.
					AsciiString text;
					text.translate(entry);
					AsciiString cmd;
					cmd.format("/msg %s %s", s_gxBuddyRowId[idx - 1], text.str());
					GXRelayRunSocialCommand(cmd);
					break;
				}

				// see if someone's selected
				Int selected = -1;
				GadgetListBoxGetSelected(buddyControls.listboxBuddies, &selected);
				if (selected >= 0)
				{
					// GeneralsX: 64-bit safe pointer cast
					GPProfile selectedProfile = static_cast<GPProfile>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(buddyControls.listboxBuddies, selected)));
					BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
					BuddyInfoMap::iterator recipIt = m->find(selectedProfile);
					if (recipIt == m->end())
						break;

					DEBUG_LOG(("Trying to send a buddy message to %d.", selectedProfile));
					if (TheGameSpyGame && TheGameSpyGame->isInGame() && TheGameSpyGame->isGameInProgress() &&
						!ThePlayerList->getLocalPlayer()->isPlayerActive())
					{
						DEBUG_LOG(("I'm dead - gotta look for cheats."));
						for (Int i=0; i<MAX_SLOTS; ++i)
						{
							DEBUG_LOG(("Slot[%d] profile is %d", i, TheGameSpyGame->getGameSpySlot(i)->getProfileID()));
							if (TheGameSpyGame->getGameSpySlot(i)->getProfileID() == selectedProfile)
							{
								// can't send to someone in our game if we're dead/observing.  security breach and all that.  no seances for you.
								if (buddyControls.listboxChat)
								{
									GadgetListBoxAddEntryText( buddyControls.listboxChat, TheGameText->fetch("Buddy:CantTalkToIngameBuddy"),
										GameSpyColor[GSCOLOR_DEFAULT], -1, -1 );
								}
								return MSG_HANDLED;
							}
						}
					}

					// read the user's input and clear the entry box
					UnicodeString txtInput;
					txtInput.set(GadgetTextEntryGetText( buddyControls.textEntryEdit ));
					GadgetTextEntrySetText(buddyControls.textEntryEdit, UnicodeString::TheEmptyString);
					txtInput.trim();
					if (!txtInput.isEmpty())
					{
						// Send the message
						BuddyRequest req;
						req.buddyRequestType = BuddyRequest::BUDDYREQUEST_MESSAGE;
						wcslcpy(req.arg.message.text, txtInput.str(), MAX_BUDDY_CHAT_LEN);
						req.arg.message.recipient = selectedProfile;
						TheGameSpyBuddyMessageQueue->addRequest(req);

						// save message for future incarnations of the buddy window
						BuddyMessageList *messages = TheGameSpyInfo->getBuddyMessages();
						BuddyMessage message;
						message.m_timestamp = time(nullptr);
						message.m_senderID = TheGameSpyInfo->getLocalProfileID();
						message.m_senderNick = TheGameSpyInfo->getLocalBaseName();
						message.m_recipientID = selectedProfile;
						message.m_recipientNick = recipIt->second.m_name;
						message.m_message = UnicodeString(req.arg.message.text);
						messages->push_back(message);

						// put message on screen
						insertChat(message);
					}
				}
				else
				{
					// nobody selected.  Prompt the user.
					if (buddyControls.listboxChat)
					{
						GadgetListBoxAddEntryText( buddyControls.listboxChat, TheGameText->fetch("Buddy:SelectBuddyToChat"),
							GameSpyColor[GSCOLOR_DEFAULT], -1, -1 );
					}
				}
				break;
			}
		default:
			return MSG_IGNORED;
	}
	return MSG_HANDLED;
}


void insertChat( BuddyMessage msg )
{
	if (buddyControls.listboxChat)
	{
		BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
		BuddyInfoMap::iterator senderIt = m->find(msg.m_senderID);
		BuddyInfoMap::iterator recipientIt = m->find(msg.m_recipientID);
		Bool localSender = (msg.m_senderID == TheGameSpyInfo->getLocalProfileID());
		UnicodeString s;
		//UnicodeString timeStr = UnicodeString(_wctime( (const time_t *)&msg.m_timestamp ));
		UnicodeString timeStr;
		if (localSender /*&& recipientIt != m->end()*/)
		{
			s.format(L"[%hs -> %hs] %s", TheGameSpyInfo->getLocalBaseName().str(), msg.m_recipientNick.str(), msg.m_message.str());
			Int index = GadgetListBoxAddEntryText( buddyControls.listboxChat, s, GameSpyColor[GSCOLOR_PLAYER_SELF], -1, -1 );
			GadgetListBoxAddEntryText( buddyControls.listboxChat, timeStr, GameSpyColor[GSCOLOR_PLAYER_SELF], index, 1);
		}
		else if (!localSender /*&& senderIt != m->end()*/)
		{
			if (!msg.m_senderID)
			{
				s = msg.m_message;
				Int index = GadgetListBoxAddEntryText( buddyControls.listboxChat, s, GameSpyColor[GSCOLOR_DEFAULT], -1, -1 );
				GadgetListBoxAddEntryText( buddyControls.listboxChat, timeStr, GameSpyColor[GSCOLOR_DEFAULT], index, 1);
			}
			else
			{
				s.format(L"[%hs] %s", msg.m_senderNick.str(), msg.m_message.str());
				Int index = GadgetListBoxAddEntryText( buddyControls.listboxChat, s, GameSpyColor[GSCOLOR_PLAYER_BUDDY], -1, -1 );
				GadgetListBoxAddEntryText( buddyControls.listboxChat, timeStr, GameSpyColor[GSCOLOR_PLAYER_BUDDY], index, 1);
			}
		}
	}
}

void updateBuddyInfo()
{
	// GeneralsX: gx: the list IS the local store, plus relay presence.
	//
	// Friends from GXPlayerIdentity, status from GXRelayPresenceFor - the lobby's already-decoded
	// three-valued answer (1 online / 0 offline / -1 unknown), never a second Transport consumer:
	// takeRelayPresence is a destructive read with exactly one permitted caller. Names render in
	// the id-quality friend blue the player list already established; the status column speaks
	// retail's own Buddy:Online / Buddy:Offline strings.
	if (s_gxMode)
	{
		if (!buddyControls.isInit || buddyControls.listboxBuddies == nullptr)
			return;

		// Rebuild ONLY when the rendered state would differ: reset wipes selection and scroll,
		// and the 1 Hz tick must not fight the player's mouse. The fingerprint covers everything
		// a row is drawn from - ids, nicks, presence, the pending set.
		AsciiString fingerprint;
		{
			const char *fpSelf = GXPlayerIdentity::getPlayerId();
			if (fpSelf == NULL || fpSelf[0] == '\0')
			{
				fingerprint = "!";
			}
			else
			{
				const Int fn = GXPlayerIdentity::getFriendCount();
				for (Int i = 0; i < fn; ++i)
				{
					const char *fid = GXPlayerIdentity::getFriendId(i);
					if (fid == NULL || fid[0] == '\0')
						continue;
					fingerprint.concat(fid);
					fingerprint.concat((char)('a' + 1 + GXRelayPresenceFor(fid)));	// -1,0,1 -> a,b,c
					const char *fnick = GXPlayerIdentity::getFriendNick(i);
					if (fnick != NULL)
						fingerprint.concat(fnick);
					fingerprint.concat('\n');
				}
				const Int fpend = GXRelayPendingRequestCount();
				for (Int p = 0; p < fpend; ++p)
				{
					Int fslot = 0;
					AsciiString fpnick;
					if (!GXRelayPendingRequestAt(p, &fslot, &fpnick))
						continue;
					fingerprint.concat('#');
					fingerprint.concat((char)('0' + fslot));
					fingerprint.concat(fpnick);
					fingerprint.concat('\n');
				}
			}
		}
		if (fingerprint == s_gxListFingerprint)
			return;
		s_gxListFingerprint = fingerprint;

		// Remember which FRIEND was selected, by id - the row index means nothing after a reset.
		char keepSelId[GX_PLAYER_ID_LEN + 1];
		keepSelId[0] = '\0';
		{
			Int selRow = -1;
			GadgetListBoxGetSelected(buddyControls.listboxBuddies, &selRow);
			if (selRow >= 0)
			{
				// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
				const Int selIdx = static_cast<Int>(reinterpret_cast<intptr_t>(
					GadgetListBoxGetItemData(buddyControls.listboxBuddies, selRow, 0)));
				if (selIdx > 0 && selIdx <= GX_BUDDY_ROWS)
					strlcpy(keepSelId, s_gxBuddyRowId[selIdx - 1], sizeof(keepSelId));
			}
		}

		GadgetListBoxReset(buddyControls.listboxBuddies);

		const char *self = GXPlayerIdentity::getPlayerId();
		if (self == NULL || self[0] == '\0')
		{
			// The CSPRNG-failure state is real and documented: an empty id means the store could
			// not mint an identity, and every friends feature is honestly unavailable. Since
			// 23/09/2026 it is also the state while the Windows migration has not yet brought the
			// old GXPlayer.ini across (GXPlayerIdentity::holdNewIdentity).
			GadgetListBoxAddEntryText(buddyControls.listboxBuddies,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBuddyUnavailable",
					L"Friends are unavailable this session - your player identity could not be loaded. See the [GXID] line on stderr."),
				GameMakeColor(128, 128, 128, 255), -1, 0);
			return;
		}

		const Int n = GXPlayerIdentity::getFriendCount();
		Int shown = 0;
		for (Int i = 0; i < n && shown < GX_BUDDY_ROWS; ++i)
		{
			const char *id = GXPlayerIdentity::getFriendId(i);
			const char *nick = GXPlayerIdentity::getFriendNick(i);
			if (id == NULL || id[0] == '\0')
				continue;
			strlcpy(s_gxBuddyRowId[shown], id, sizeof(s_gxBuddyRowId[shown]));

			UnicodeString name;
			if (nick != NULL && nick[0] != '\0')
			{
				name.translate(AsciiString(nick));
			}
			else
			{
				char prefixBuf[8];
				strlcpy(prefixBuf, id, 7);	// enough to recognise it; never the whole credential
				name.translate(AsciiString(prefixBuf));
			}

			UnicodeString status;
			Color statusColor;
			const Int here = GXRelayPresenceFor(s_gxBuddyRowId[shown]);
			if (here == 1)
			{
				status = TheGameText->fetch("Buddy:Online");
				statusColor = GameMakeColor(220, 220, 220, 255);
			}
			else if (here == 0)
			{
				status = TheGameText->fetch("Buddy:Offline");
				statusColor = GameMakeColor(128, 128, 128, 255);
			}
			else
			{
				status = TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBuddyChecking", L"checking...");
				statusColor = GameMakeColor(128, 128, 128, 255);
			}

			const Color nameColor = GameMakeColor(105, 190, 255, 255);
			const Int row = GadgetListBoxAddEntryText(buddyControls.listboxBuddies, name, nameColor, -1, 0);
			GadgetListBoxAddEntryText(buddyControls.listboxBuddies, status, statusColor, row, 1);
			// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
			GadgetListBoxSetItemData(buddyControls.listboxBuddies,
				reinterpret_cast<void *>(static_cast<intptr_t>(shown + 1)), row, 0);
			if (keepSelId[0] != '\0' && strcmp(keepSelId, s_gxBuddyRowId[shown]) == 0)
				GadgetListBoxSetSelected(buddyControls.listboxBuddies, row);
			++shown;
		}

		// Pending requests, after the friends: rows for slots whose occupant sent "fq". These are
		// BY_SLOT - an inbound "fq" deliberately carries no id - so the item-data is the NEGATED
		// slot and the latched nick is kept beside it for the right-click's snapshot. A transient
		// chat line is never the only record of a request; that is the Phase 0 lesson.
		Int pendingShown = 0;
		const Int pend = GXRelayPendingRequestCount();
		for (Int p = 0; p < pend && (shown + pendingShown) < GX_BUDDY_ROWS; ++p)
		{
			Int slot = 0;
			AsciiString pnick;
			if (!GXRelayPendingRequestAt(p, &slot, &pnick) || slot < 1 || slot > 8)
				continue;
			s_gxPendingNick[slot] = pnick;

			UnicodeString name;
			if (!pnick.isEmpty())
				name.translate(pnick);
			else
				name.format(L"P%d", slot);

			const Color reqColor = GameMakeColor(255, 200, 120, 255);
			const Int row = GadgetListBoxAddEntryText(buddyControls.listboxBuddies, name, reqColor, -1, 0);
			GadgetListBoxAddEntryText(buddyControls.listboxBuddies,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBuddyWantsFriends", L"wants to be friends"),
				reqColor, row, 1);
			// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
			GadgetListBoxSetItemData(buddyControls.listboxBuddies,
				reinterpret_cast<void *>(static_cast<intptr_t>(-slot)), row, 0);
			++pendingShown;
		}

		if (shown == 0 && pendingShown == 0)
		{
			GadgetListBoxAddEntryText(buddyControls.listboxBuddies,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXBuddyEmpty",
					L"No friends yet. Right-click a player in the lobby list, or /add <code>."),
				GameMakeColor(128, 128, 128, 255), -1, 0);
		}
		return;
	}

	if (!TheGameSpyBuddyMessageQueue->isConnected())
	{
		GadgetListBoxReset(buddyControls.listboxBuddies);
		return;
	}

	if (!buddyControls.isInit)
		return;

	int selected;
	GPProfile selectedProfile = 0;
	int visiblePos = GadgetListBoxGetTopVisibleEntry(buddyControls.listboxBuddies);

	GadgetListBoxGetSelected(buddyControls.listboxBuddies, &selected);
	if (selected >= 0)
		// GeneralsX: 64-bit safe pointer cast
		selectedProfile = static_cast<GPProfile>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(buddyControls.listboxBuddies, selected)));

	selected = -1;
	GadgetListBoxReset(buddyControls.listboxBuddies);

	// Add buddies
	BuddyInfoMap *buddies = TheGameSpyInfo->getBuddyMap();
	BuddyInfoMap::iterator bIt;
	for (bIt = buddies->begin(); bIt != buddies->end(); ++bIt)
	{
		BuddyInfo info = bIt->second;
		GPProfile profileID = bIt->first;

		// insert name into box
		UnicodeString formatStr;
		formatStr.translate(info.m_name.str());//, info.m_status, info.m_statusString.str(), info.m_locationString.str());
		Color nameColor = (TheGameSpyInfo->isSavedIgnored(profileID)) ?
			GameSpyColor[GSCOLOR_PLAYER_IGNORED] : GameSpyColor[GSCOLOR_PLAYER_BUDDY];
		int index = GadgetListBoxAddEntryText(buddyControls.listboxBuddies, formatStr, nameColor, -1, -1);

		// insert status into box
		AsciiString marker;
		marker.format("Buddy:%ls", info.m_statusString.str());
		if (!info.m_statusString.compareNoCase(L"Offline") ||
			!info.m_statusString.compareNoCase(L"Online") ||
			!info.m_statusString.compareNoCase(L"Matching"))
		{
			formatStr = TheGameText->fetch(marker);
		}
		else if (!info.m_statusString.compareNoCase(L"Staging") ||
			!info.m_statusString.compareNoCase(L"Loading") ||
			!info.m_statusString.compareNoCase(L"Playing"))
		{
			formatStr.format(TheGameText->fetchGameOnly(marker.str()), info.m_locationString.str());
		}
		else if (!info.m_statusString.compareNoCase(L"Chatting"))
		{
			UnicodeString roomName;
			GroupRoomMap::iterator gIt = TheGameSpyInfo->getGroupRoomList()->find( _wtoi(info.m_locationString.str()) );
			if (gIt != TheGameSpyInfo->getGroupRoomList()->end())
			{
				AsciiString s;
				s.format("GUI:%s", gIt->second.m_name.str());
				roomName = TheGameText->fetch(s);
			}
			formatStr.format(TheGameText->fetchGameOnly(marker.str()), roomName.str());
		}
		else
		{
			formatStr = info.m_statusString;
		}
		GadgetListBoxAddEntryText(buddyControls.listboxBuddies, formatStr, GameSpyColor[GSCOLOR_DEFAULT], index, 1);
		GadgetListBoxSetItemData(buddyControls.listboxBuddies, (void *)(profileID), index, 0 );
		GadgetListBoxSetItemData(buddyControls.listboxBuddies, (void *)(ITEM_BUDDY), index, 1 );

		if (profileID == selectedProfile)
			selected = index;
	}

	// add requests
	buddies = TheGameSpyInfo->getBuddyRequestMap();
	for (bIt = buddies->begin(); bIt != buddies->end(); ++bIt)
	{
		BuddyInfo info = bIt->second;
		GPProfile profileID = bIt->first;

		// insert name into box
		UnicodeString formatStr;
		formatStr.translate(info.m_name.str());
		int index = GadgetListBoxAddEntryText(buddyControls.listboxBuddies, formatStr, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		GadgetListBoxSetItemData(buddyControls.listboxBuddies, (void *)(profileID), index, 0 );

		// insert status into box
		formatStr = TheGameText->fetch("GUI:BuddyAddReq");
		GadgetListBoxAddEntryText(buddyControls.listboxBuddies, formatStr, GameSpyColor[GSCOLOR_DEFAULT], index, 1);
		GadgetListBoxSetItemData(buddyControls.listboxBuddies, (void *)(ITEM_REQUEST), index, 1 );

		if (profileID == selectedProfile)
			selected = index;
	}


	// select the same guy
	if (selected >= 0)
	{
		GadgetListBoxSetSelected(buddyControls.listboxBuddies, selected);
	}

	// view the same spot
	GadgetListBoxSetTopVisibleEntry(buddyControls.listboxBuddies, visiblePos);
}

void HandleBuddyResponses()
{
	if (TheGameSpyBuddyMessageQueue)
	{
		BuddyResponse resp;
		if (TheGameSpyBuddyMessageQueue->getResponse( resp ))
		{
			switch (resp.buddyResponseType)
			{
			case BuddyResponse::BUDDYRESPONSE_LOGIN:
				{
					deleteNotificationBox();
				}
				break;
			case BuddyResponse::BUDDYRESPONSE_DISCONNECT:
				{
					lastNotificationWasStatus = FALSE;
					numOnlineInNotification = 0;
					showNotificationBox(AsciiString::TheEmptyString, TheGameText->fetch("Buddy:MessageDisconnected"));
				}
				break;
			case BuddyResponse::BUDDYRESPONSE_MESSAGE:
				{
					if ( wcscmp(resp.arg.message.text, L"I have authorized your request to add me to your list") == 0 )
						break;

					if (TheGameSpyInfo->isSavedIgnored(resp.profile))
					{
						//DEBUG_CRASH(("Player is ignored!"));
						break; // no buddy messages from ignored people
					}

					// save message for future incarnations of the buddy window
					BuddyMessageList *messages = TheGameSpyInfo->getBuddyMessages();
					BuddyMessage message;
					message.m_timestamp = resp.arg.message.date;
					message.m_senderID = resp.profile;
					message.m_recipientID = TheGameSpyInfo->getLocalProfileID();
					message.m_recipientNick = TheGameSpyInfo->getLocalBaseName();
					message.m_message = resp.arg.message.text;
					// insert status into box
					BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
					BuddyInfoMap::iterator senderIt = m->find(message.m_senderID);
					AsciiString nick;
					if (senderIt != m->end())
						nick = senderIt->second.m_name.str();
					else
						nick = resp.arg.message.nick;
					message.m_senderNick = nick;
					messages->push_back(message);

					DEBUG_LOG(("Inserting buddy chat from '%s'/'%s'", nick.str(), resp.arg.message.nick));

					// put message on screen
					insertChat(message);

					// play audio notification
					AudioEventRTS buddyMsgAudio("GUIMessageReceived");
					if( TheAudio )
					{
						TheAudio->addAudioEvent( &buddyMsgAudio );
					}

					UnicodeString snippet = message.m_message;
					snippet.truncateTo(11);

					UnicodeString s;
					s.format(TheGameText->fetchGameOnly("Buddy:MessageNotification"), nick.str(), snippet.str());
					lastNotificationWasStatus = FALSE;
					numOnlineInNotification = 0;
					showNotificationBox(AsciiString::TheEmptyString, s);
				}
				break;
			case BuddyResponse::BUDDYRESPONSE_REQUEST:
				{
					// save request for future incarnations of the buddy window
					BuddyInfoMap *m = TheGameSpyInfo->getBuddyRequestMap();
					BuddyInfo info;
					info.m_countryCode = resp.arg.request.countrycode;
					info.m_email = resp.arg.request.email;
					info.m_name = resp.arg.request.nick;
					info.m_id = resp.profile;
					info.m_status = (GPEnum)0;
					info.m_statusString = resp.arg.request.text;
					(*m)[resp.profile] = info;

					// TODO: put request on screen
					updateBuddyInfo();
					// insert status into box
					lastNotificationWasStatus = FALSE;
					numOnlineInNotification = 0;
					showNotificationBox(info.m_name, TheGameText->fetchGameOnly("Buddy:AddNotification"));
				}
				break;
			case BuddyResponse::BUDDYRESPONSE_STATUS:
				{
					BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
					BuddyInfoMap::const_iterator bit = m->find(resp.profile);
					Bool seenPreviously = FALSE;
					GPEnum oldStatus = GP_OFFLINE;
					GPEnum newStatus = resp.arg.status.status;
					if (bit != m->end())
					{
						seenPreviously = TRUE;
						oldStatus = (*m)[resp.profile].m_status;
					}
					BuddyInfo info;
					info.m_countryCode = resp.arg.status.countrycode;
					info.m_email = resp.arg.status.email;
					info.m_name = resp.arg.status.nick;
					info.m_id = resp.profile;
					info.m_status = newStatus;
					info.m_statusString = UnicodeString(MultiByteToWideCharSingleLine(resp.arg.status.statusString).c_str());
					info.m_locationString = UnicodeString(MultiByteToWideCharSingleLine(resp.arg.status.location).c_str());
					(*m)[resp.profile] = info;

					updateBuddyInfo();
					PopulateLobbyPlayerListbox();
					RefreshGameListBoxes();
					if ( (newStatus == GP_OFFLINE && seenPreviously) ||
						(newStatus == GP_ONLINE && (oldStatus == GP_OFFLINE || !seenPreviously)) )
					//if (!info.m_statusString.compareNoCase(L"Offline") ||
					//!info.m_statusString.compareNoCase(L"Online"))
					{
						// insert status into box
						// GeneralsX: The status text is the peer's own (GameSpy statusString). It used to name the
						// label directly, so a status of "Message" picked Buddy:MessageNotification - a two-argument template given one argument
						// below. Only the six status lines the game ships, each a one-argument (nick) template, are
						// selectable; any other status takes the plain online / offline line.
						AsciiString marker;
						static const WideChar *const s_statusLines[] = { L"Online", L"Offline", L"Loading", L"Matching", L"Playing", L"Staging" };
						for (size_t k = 0; k < sizeof(s_statusLines) / sizeof(s_statusLines[0]) && marker.isEmpty(); ++k)
						{
							if (!info.m_statusString.compareNoCase(s_statusLines[k]))
								marker.format("Buddy:%lsNotification", s_statusLines[k]);
						}

						lastNotificationWasStatus = TRUE;
						if (newStatus != GP_OFFLINE)
							++numOnlineInNotification;

						// GeneralsX: The template is a printf format for the nick: game-table text only (a label a
						// localisation lacks fell through fetch() to the loaded map's map.str); a missing one takes the plain line too.
						Bool exists = FALSE;
						UnicodeString note;
						if (marker.isNotEmpty())
							note = TheGameText->fetchGameOnly( marker.str(), &exists );
						if( !exists )
							note = TheGameText->fetchGameOnly( newStatus == GP_OFFLINE ? "Buddy:OfflineNotification" : "Buddy:OnlineNotification" );
						showNotificationBox(info.m_name, note);
					}
					else if( newStatus == GP_RECV_GAME_INVITE && !seenPreviously)
					{
						lastNotificationWasStatus = TRUE;
						if (newStatus != GP_OFFLINE)
							++numOnlineInNotification;

						showNotificationBox(info.m_name, TheGameText->fetchGameOnly("Buddy:OnlineNotification"));
					}
				}
				break;
			}
		}
	}
	else
	{
		DEBUG_CRASH(("No buddy message queue!"));
	}
	if(noticeLayout && timeGetTime() > noticeExpires)
	{
		deleteNotificationBox();
	}
}

void showNotificationBox( AsciiString nick, UnicodeString message)
{
//	if(!GameSpyIsOverlayOpen(GSOVERLAY_BUDDY))
//		return;
	if( !noticeLayout )
		noticeLayout = TheWindowManager->winCreateLayout( "Menus/PopupBuddyListNotification.wnd" );
	noticeLayout->hide( FALSE );
	if (buttonNotificationID == NAMEKEY_INVALID)
	{
		buttonNotificationID = TheNameKeyGenerator->nameToKey("PopupBuddyListNotification.wnd:ButtonNotification");
	}
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr,buttonNotificationID);
	if(!win)
	{
		deleteNotificationBox();
		return;
	}

	if (lastNotificationWasStatus && numOnlineInNotification > 1)
	{
		message = TheGameText->fetchGameOnly("Buddy:MultipleOnlineNotification");
	}

	if (nick.isNotEmpty())
		message.format(message, nick.str());
	GadgetButtonSetText(win, message);
	//GadgetStaticTextSetText(win, message);
	noticeExpires = timeGetTime() + NOTIFICATION_EXPIRES;
	noticeLayout->bringForward();

	AudioEventRTS buttonClick("GUICommunicatorIncoming");

	if( TheAudio )
	{
		TheAudio->addAudioEvent( &buttonClick );
	}

}

void deleteNotificationBox()
{
	lastNotificationWasStatus = FALSE;
	numOnlineInNotification = 0;
	if(noticeLayout)
	{
		noticeLayout->destroyWindows();
		deleteInstance(noticeLayout);
		noticeLayout = nullptr;
	}
}

void PopulateOldBuddyMessages()
{
	// show previous messages
	BuddyMessageList *messages = TheGameSpyInfo->getBuddyMessages();
	for (BuddyMessageList::iterator mIt = messages->begin(); mIt != messages->end(); ++mIt)
	{
		const BuddyMessage &message = *mIt;
		insertChat(message);
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the WOL Buddy Overlay */
//-------------------------------------------------------------------------------------------------
void WOLBuddyOverlayInit( WindowLayout *layout, void *userData )
{
	parentID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:BuddyMenuParent" );
	buttonHideID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ButtonHide" );
	buttonAddBuddyID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ButtonAdd" );
	buttonDeleteBuddyID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ButtonDelete" );
	//textEntryID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:TextEntryChat" );
	//listboxBuddyID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ListboxBuddies" );
	//listboxChatID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ListboxBuddyChat" );
	buttonAcceptBuddyID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ButtonYes" );
	buttonDenyBuddyID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ButtonNo" );
	radioButtonBuddiesID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:RadioButtonBuddies" );
	radioButtonIgnoreID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:RadioButtonIgnore" );
	parentBuddiesID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:BuddiesParent" );
	parentIgnoreID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:IgnoreParent" );
	listboxIgnoreID = TheNameKeyGenerator->nameToKey( "WOLBuddyOverlay.wnd:ListboxIgnore" );


	parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );
	buttonHide = TheWindowManager->winGetWindowFromId( parent,  buttonHideID);
	buttonAddBuddy = TheWindowManager->winGetWindowFromId( parent,  buttonAddBuddyID);
	buttonDeleteBuddy = TheWindowManager->winGetWindowFromId( parent,  buttonDeleteBuddyID);
	//	textEntry = TheWindowManager->winGetWindowFromId( parent,  textEntryID);
	//listboxBuddy = TheWindowManager->winGetWindowFromId( parent,  listboxBuddyID);
	//listboxChat = TheWindowManager->winGetWindowFromId( parent,  listboxChatID);
	buttonAcceptBuddy = TheWindowManager->winGetWindowFromId( parent,  buttonAcceptBuddyID);
	buttonDenyBuddy = TheWindowManager->winGetWindowFromId( parent,  buttonDenyBuddyID);
	radioButtonBuddies = TheWindowManager->winGetWindowFromId( parent,  radioButtonBuddiesID);
	radioButtonIgnore = TheWindowManager->winGetWindowFromId( parent,  radioButtonIgnoreID);
	parentBuddies = TheWindowManager->winGetWindowFromId( parent,  parentBuddiesID);
	parentIgnore = TheWindowManager->winGetWindowFromId( parent,  parentIgnoreID);
	listboxIgnore = TheWindowManager->winGetWindowFromId( parent,  listboxIgnoreID);

	InitBuddyControls(BUDDY_WINDOW_BUDDIES);

	GadgetRadioSetSelection(radioButtonBuddies,FALSE);
	parentBuddies->winHide(FALSE);
	parentIgnore->winHide(TRUE);

	//GadgetTextEntrySetText(textEntry, UnicodeString::TheEmptyString);

	// GeneralsX: Both calls are GameSpy-fed and one of them crashes on
	// the relay path: updateBuddyInfo dereferences TheGameSpyBuddyMessageQueue->isConnected()
	// unguarded, and that queue is never created here. The gx population arrives with the store
	// re-point; until then the overlay opens with its controls live and its lists empty.
	if (!s_gxMode)
	{
		PopulateOldBuddyMessages();
	}

	// Show Menu
	layout->hide( FALSE );

	// Set Keyboard to Main Parent
	TheWindowManager->winSetFocus( parent );

	isOverlayActive = true;
	updateBuddyInfo();	// branches on gx mode internally - the gx body reads the local store

}

//-------------------------------------------------------------------------------------------------
/** WOL Buddy Overlay shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLBuddyOverlayShutdown( WindowLayout *layout, void *userData )
{
	listboxIgnore = nullptr;

	// hide menu
	layout->hide( TRUE );

	// our shutdown is complete
	//TheShell->shutdownComplete( layout );

	isOverlayActive = false;

	InitBuddyControls(BUDDY_RESETALL_CRAP);

}


//-------------------------------------------------------------------------------------------------
/** WOL Buddy Overlay update method */
//-------------------------------------------------------------------------------------------------
void WOLBuddyOverlayUpdate( WindowLayout * layout, void *userData)
{
	// GeneralsX: In gx mode this update DRIVES the screen instead of
	// closing it. The GameSpy branch below closed the overlay whenever the buddy queue was absent
	// - which on the relay path is always - and that single line is why the retail overlay was
	// unreachable for as long as it was. The notification expiry lives here too: the GameSpy
	// path polls it inside HandleBuddyResponses, which gx mode never calls.
	if (s_gxMode)
	{
		// 1 Hz refresh: presence answers ride the lobby's own poll cadence (30 s tick, 3 s min
		// gap), so redrawing faster buys nothing, and the store only changes through actions that
		// already redraw. The persistent stamp across open/close is harmless - Init draws
		// immediately on open regardless.
		static UnsignedInt s_lastGxRefreshMs = 0;
		const UnsignedInt now = timeGetTime();
		if (now - s_lastGxRefreshMs >= 1000)
		{
			s_lastGxRefreshMs = now;
			updateBuddyInfo();
			if (parentIgnore != nullptr && !parentIgnore->winIsHidden())
				refreshIgnoreList();
		}
		if (noticeExpires != 0 && timeGetTime() > noticeExpires)
			deleteNotificationBox();
		return;
	}

	if (!TheGameSpyBuddyMessageQueue || !TheGameSpyBuddyMessageQueue->isConnected())
		GameSpyCloseOverlay(GSOVERLAY_BUDDY);
}

//-------------------------------------------------------------------------------------------------
/** WOL Buddy Overlay input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLBuddyOverlayInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

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
																							(WindowMsgData)buttonHide, buttonHideID );

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
/** WOL Buddy Overlay window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLBuddyOverlaySystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	if(BuddyControlSystem(window, msg, mData1, mData2) == MSG_HANDLED)
	{
		return MSG_HANDLED;
	}
	switch( msg )
	{


		case GWM_CREATE:
			{

				break;
			}

		case GWM_DESTROY:
			{
				break;
			}

		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}
		case GLM_RIGHT_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if( controlID == listboxIgnoreID )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout;
					if(rc->pos < 0)
						break;

					// GeneralsX: gx: ignore-tab rows carry INDEX+1 into
					// the copied-id array; 0 is an info row. A row that is also a friend gets the
					// friend menu; anyone else gets the stranger menu - whose Ignore button the
					// label helper has already flipped to Unignore. The shared creation helper
					// also brings the screen-edge clamp this site never had.
					if (s_gxMode)
					{
						const Int idx = static_cast<Int>(reinterpret_cast<intptr_t>(
							GadgetListBoxGetItemData(control, rc->pos)));
						if (idx <= 0 || idx > GX_BUDDY_ROWS)
							break;
						GadgetListBoxSetSelected(control, rc->pos);
						const char *rowId = s_gxIgnoreRowId[idx - 1];
						const Bool rowIsFriend = GXPlayerIdentity::isFriend(rowId);
						gxOpenRCMenu(rowIsFriend ? "Menus/RCBuddiesMenu.wnd" : "Menus/RCNonBuddiesMenu.wnd",
							GXRCMenuData::BY_ID, rowId, 0, GadgetListBoxGetText(control, rc->pos),
							rc->mouseX, rc->mouseY, rowIsFriend ? ITEM_BUDDY : ITEM_NONBUDDY,
							GXPlayerIdentity::isIgnored(rowId));
						break;
					}

					Bool isBuddy = false, isRequest = false;
					// GeneralsX: 64-bit safe pointer cast
					GPProfile profileID = static_cast<GPProfile>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rc->pos)));
					UnicodeString nick = GadgetListBoxGetText(control, rc->pos);
					BuddyInfoMap *buddies = TheGameSpyInfo->getBuddyMap();
					BuddyInfoMap::iterator bIt;
					bIt = buddies->find(profileID);
					if (bIt != buddies->end())
					{
						isBuddy = true;
					}
					else
					{
						buddies = TheGameSpyInfo->getBuddyRequestMap();
						bIt = buddies->find(profileID);
						if (bIt != buddies->end())
						{
							isRequest = true;
						}
						else
						{
							// neither buddy nor request
							//break;
						}
					}

					GadgetListBoxSetSelected(control, rc->pos);
					if (isBuddy)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddiesMenu.wnd");
					else if (isRequest)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddyRequestMenu.wnd");
					else
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCNonBuddiesMenu.wnd");
					rcMenu = rcLayout->getFirstWindow();
					rcMenu->winGetLayout()->runInit();
					rcMenu->winBringToTop();
					rcMenu->winHide(FALSE);



					rcMenu->winSetPosition(rc->mouseX, rc->mouseY);
					GameSpyRCMenuData *rcData = NEW GameSpyRCMenuData;
					rcData->m_id = profileID;
					rcData->m_nick.translate(nick);
					rcData->m_itemType = (isBuddy)?ITEM_BUDDY:((isRequest)?ITEM_REQUEST:ITEM_NONBUDDY);
					setUnignoreText(rcLayout, rcData->m_nick, rcData->m_id);
					rcMenu->winSetUserData((void *)rcData);
					TheWindowManager->winSetLoneWindow(rcMenu);
				}
				break;
			}
		case GBM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (controlID == buttonHideID)
				{
					// GeneralsX: The gx layout is not in overlayLayouts[]
					// - GameSpyCloseOverlay would find a null entry and the overlay would never
					// close. Its owner closes it, with the same runShutdown/destroy sequence.
					if (s_gxMode)
					{
						GXBuddyOverlayClose();
						break;
					}
					GameSpyCloseOverlay( GSOVERLAY_BUDDY );
				}
				else if (controlID == radioButtonBuddiesID)
				{
					parentBuddies->winHide(FALSE);
					parentIgnore->winHide(TRUE);
				}
				else if (controlID == radioButtonIgnoreID)
				{
					parentBuddies->winHide(TRUE);
					parentIgnore->winHide(FALSE);
					refreshIgnoreList();
				}
				else if (controlID == buttonAddBuddyID)
				{
					/*
					UnicodeString uName = GadgetTextEntryGetText(textEntry);
					AsciiString aName;
					aName.translate(uName);
					if (!aName.isEmpty())
					{
						TheWOLBuddyList->requestBuddyAdd(aName);
					}
					GadgetTextEntrySetText(textEntry, UnicodeString::TheEmptyString);
					*/
				}
				else if (controlID == buttonDeleteBuddyID)
				{
					/*
					int selected;
					AsciiString selectedName = AsciiString::TheEmptyString;

					GadgetListBoxGetSelected(listbox, &selected);
					if (selected >= 0)
						selectedName = TheNameKeyGenerator->keyToName((NameKeyType)(int)GadgetListBoxGetItemData(listbox, selected));

					if (!selectedName.isEmpty())
					{
						TheWOLBuddyList->requestBuddyDelete(selectedName);
					}
					*/
				}
				break;
			}
		case GLM_DOUBLE_CLICKED:
			{
				/*
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxBuddyID )
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						UnicodeString buddyName;
						GameWindow *listboxWindow = TheWindowManager->winGetWindowFromId( parent, listboxBuddyID );

							// get text of buddy name
						buddyName = GadgetListBoxGetText( listboxWindow, rowSelected,0 );
						GPProfile buddyID = (GPProfile)GadgetListBoxGetItemData( listboxWindow, rowSelected, 0 );

						Int index = -1;
						gpGetBuddyIndex(TheGPConnection, buddyID, &index);
						if (index >= 0)
						{
							GPBuddyStatus status;
							gpGetBuddyStatus(TheGPConnection, rowSelected, &status);

							UnicodeString string;
							string.format(L"To join %s in %hs:", buddyName.str(), status.locationString);
							GameSpyAddText(string, GSCOLOR_DEFAULT);

							if (status.status == GP_CHATTING)
							{
								AsciiString location = status.locationString;
								AsciiString val;
								location.nextToken(&val, "/");
								location.nextToken(&val, "/");
								location.nextToken(&val, "/");

								string.format(L"  ???");
								if (!val.isEmpty())
								{
									Int groupRoom = atoi(val.str());
									if (TheGameSpyChat->getCurrentGroupRoomID() == groupRoom)
									{
										// already there
										string.format(L"  nothing");
										GameSpyAddText(string, GSCOLOR_DEFAULT);
									}
									else
									{
										GroupRoomMap *rooms = TheGameSpyChat->getGroupRooms();
										if (rooms)
										{
											Bool needToJoin = true;
											GroupRoomMap::iterator it = rooms->find(groupRoom);
											if (it != rooms->end())
											{
												// he's in a different room
												if (TheGameSpyChat->getCurrentGroupRoomID())
												{
													string.format(L"  leave group room");
													GameSpyAddText(string, GSCOLOR_DEFAULT);

													TheGameSpyChat->leaveRoom(GroupRoom);
												}
												else if (TheGameSpyGame->isInGame())
												{
													if (TheGameSpyGame->isGameInProgress())
													{
														string.format(L"  can't leave game in progress");
														GameSpyAddText(string, GSCOLOR_DEFAULT);
														needToJoin = false;
													}
													else
													{
														string.format(L"  leave game setup");
														GameSpyAddText(string, GSCOLOR_DEFAULT);

														TheGameSpyChat->leaveRoom(StagingRoom);
														TheGameSpyGame->leaveGame();
													}
												}
												if (needToJoin)
												{
													string.format(L"  join lobby %d", groupRoom);
													TheGameSpyChat->joinGroupRoom(groupRoom);
													GameSpyAddText(string, GSCOLOR_DEFAULT);
												}
											}
										}
									}
								}
							}
						}
						else
						{
							DEBUG_CRASH(("No buddy associated with that ProfileID"));
							GameSpyUpdateBuddyOverlay();
						}
					}
				}
				*/
				break;
			}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}

WindowMsgHandledType PopupBuddyNotificationSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		case GWM_CREATE:
			{
				break;
			}

		case GWM_DESTROY:
			{
				break;
			}

		case GBM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (controlID == buttonNotificationID)
				{
					GameSpyOpenOverlay( GSOVERLAY_BUDDY );
				}
				break;
			}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}

/*
static NameKeyType buttonAcceptBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonDenyBuddyID = NAMEKEY_INVALID;
*/
static NameKeyType buttonAddID = NAMEKEY_INVALID;
static NameKeyType buttonDeleteID = NAMEKEY_INVALID;
static NameKeyType buttonPlayID = NAMEKEY_INVALID;
static NameKeyType buttonIgnoreID = NAMEKEY_INVALID;
static NameKeyType buttonStatsID = NAMEKEY_INVALID;
// Window Pointers ------------------------------------------------------------------------
//static GameWindow *rCparent = nullptr;


//-------------------------------------------------------------------------------------------------
/** WOL Buddy Overlay Right Click menu callbacks */
//-------------------------------------------------------------------------------------------------
void WOLBuddyOverlayRCMenuInit( WindowLayout *layout, void *userData )
{
	AsciiString controlName;
	controlName.format("%s:ButtonAdd",layout->getFilename().str()+6);
	buttonAddID =  TheNameKeyGenerator->nameToKey( controlName );
	controlName.format("%s:ButtonDelete",layout->getFilename().str()+6);
	buttonDeleteID =  TheNameKeyGenerator->nameToKey( controlName );
	controlName.format("%s:ButtonPlay",layout->getFilename().str()+6);
	buttonPlayID =  TheNameKeyGenerator->nameToKey( controlName );
	controlName.format("%s:ButtonIgnore",layout->getFilename().str()+6);
	buttonIgnoreID =  TheNameKeyGenerator->nameToKey( controlName );
	controlName.format("%s:ButtonStats",layout->getFilename().str()+6);
	buttonStatsID =  TheNameKeyGenerator->nameToKey( controlName );
}
static void closeRightClickMenu(GameWindow *win)
{

	if(win)
	{
		// GeneralsX: Release the manager's lone-window claim BEFORE the
		// destroy. The menu was registered with winSetLoneWindow at creation; destroying it
		// directly (the transition-close path never goes through the manager's GGM_CLOSE
		// dispatch) left m_loneWindow pointing at freed memory, and the next mouse event
		// dereferenced it.
		//
		// RE-ENTRANT, and that is why the guard exists: winSetLoneWindow SENDS GGM_CLOSE to the
		// current lone window before clearing it, and this file's GGM_CLOSE handler calls right
		// back into here - it would recurse until the stack died. The guard lets the inner call
		// return immediately; the outer one then completes the release and the destroy.
		static Bool s_closing = FALSE;
		if (s_closing)
			return;
		s_closing = TRUE;
		TheWindowManager->winSetLoneWindow(nullptr);
		s_closing = FALSE;

		// GeneralsX: A menu dismissed without a click still holds its
		// payload - the historical GameSpy path leaked it here. Freed by the DERIVED type after
		// the sentinel check: the base has no virtual dtor.
		GameSpyRCMenuData *lingering = (GameSpyRCMenuData *)win->winGetUserData();
		if (lingering)
		{
			win->winSetUserData(nullptr);
			if (lingering->m_id == (GPProfile)GX_RC_PROFILE_SENTINEL)
				delete static_cast<GXRCMenuData *>(lingering);
			else
				delete lingering;
		}

		WindowLayout *winLay = win->winGetLayout();
		if(!winLay)
			return;
		winLay->destroyWindows();
		deleteInstance(winLay);
		winLay = nullptr;

	}
}

// GeneralsX: The one gx way to open an RC menu, from either listbox and
// (later) the lobby roster. Mirrors the GameSpy creation sites exactly - runInit, bring to top,
// screen-edge clamp - then diverges where gx must: the Stats and Play buttons are hidden (no
// stats service exists here, and Play is a stub even upstream), the Ignore button's label is
// flipped from the LOCAL store's answer, and the payload is the sentinel-tagged gx type.
static void gxOpenRCMenu( const char *layoutName, Int gxKind, const char *id, Int slot,
						  UnicodeString nickWide, Int mouseX, Int mouseY, RCItemType itemType,
						  Bool flipIgnoreLabel )
{
	// Close any menu already open FIRST. winSetLoneWindow below synchronously closes the previous
	// lone window, and that menu's GWM_DESTROY nulls the shared rcMenu - which would throw away
	// the handle to the menu being created here, leaving it open and unclosable.
	if (rcMenu != nullptr)
		closeRightClickMenu(rcMenu);

	WindowLayout *rcLayout = TheWindowManager->winCreateLayout(layoutName);
	if (rcLayout == nullptr)
		return;
	rcMenu = rcLayout->getFirstWindow();
	rcMenu->winGetLayout()->runInit();
	rcMenu->winBringToTop();
	rcMenu->winHide(FALSE);

	ICoord2D rcSize, rcPos;
	rcMenu->winGetSize(&rcSize.x, &rcSize.y);
	rcPos.x = mouseX;
	rcPos.y = mouseY;
	if(mouseX + rcSize.x > TheDisplay->getWidth())
		rcPos.x = TheDisplay->getWidth() - rcSize.x;
	if(mouseY + rcSize.y > TheDisplay->getHeight())
		rcPos.y = TheDisplay->getHeight() - rcSize.y;
	rcMenu->winSetPosition(rcPos.x, rcPos.y);

	// runInit refreshed the file-static button ids for THIS layout.
	GameWindow *hideMe = TheWindowManager->winGetWindowFromId(rcMenu, buttonStatsID);
	if (hideMe)
	{
		// GeneralsX: EXCEPT ON THE PLAYER'S OWN ROW. RCLocalPlayerMenu.wnd
		// has exactly one control, ButtonStats (measured: extracted from stock WindowZH.big), so
		// hiding it there would open an empty box. There is no stats service to point it at, and
		// the one thing a player can do to themselves here is sign out - so it keeps EA's button,
		// art, size and position, and only its words change. Relabelling is a runtime text set:
		// no layout is edited, so there is no data rollout and dataID does not move.
		if (gxKind == GXRCMenuData::SELF)
			GadgetButtonSetText(hideMe, TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignOut", L"Sign Out"));
		else
			hideMe->winHide(TRUE);
	}
	hideMe = TheWindowManager->winGetWindowFromId(rcMenu, buttonPlayID);
	if (hideMe)
		hideMe->winHide(TRUE);

	// The label helper: same control lookup as setUnignoreText. The caller answers the "is this
	// one ignored" question from whichever store owns the row - the id store for BY_ID rows, the
	// session slot bookkeeping for roster rows - because only the caller knows which applies.
	if (flipIgnoreLabel)
	{
		AsciiString controlName;
		controlName.format("%s:ButtonIgnore", rcLayout->getFilename().str()+6);
		GameWindow *win = TheWindowManager->winGetWindowFromId(rcLayout->getFirstWindow(),
			TheNameKeyGenerator->nameToKey( controlName ));
		if (win)
			GadgetButtonSetText(win, TheGameText->fetch("GUI:Unignore"));
	}

	GXRCMenuData *rcData = NEW GXRCMenuData;
	rcData->m_gxKind = (GXRCMenuData::Kind)gxKind;
	if (id != NULL)
		strlcpy(rcData->m_gxId, id, sizeof(rcData->m_gxId));
	rcData->m_gxSlot = slot;
	rcData->m_nick.translate(nickWide);
	rcData->m_itemType = itemType;
	rcMenu->winSetUserData((void *)rcData);
	TheWindowManager->winSetLoneWindow(rcMenu);
}

// GeneralsX: Close whatever RC menu is open. The menus are independent
// lone-window layouts - a roster menu can exist while the overlay itself is closed - so screen
// transitions must close them EXPLICITLY or a menu survives above the next screen, holding a
// payload whose slot no longer means anything (cross-model review's catch). rcMenu is nulled by
// the GWM_DESTROY the destruction raises.
void GXBuddyCloseRCMenu( void )
{
	if (rcMenu != nullptr)
		closeRightClickMenu(rcMenu);
}

// GeneralsX: The lobby roster's way in - see GUICallbacks.h. The
// snapshot arrives as the RAW roster nick and is carried into the payload verbatim; roster nicks
// are GXNICK tokens (seven-bit), so the ascii/wide round trip here is lossless.
void GXBuddyOpenRosterRCMenu( const char *layoutName, Int slot, const AsciiString &nickSnapshot,
							  Int mouseX, Int mouseY, Bool flipIgnoreLabel )
{
	UnicodeString wide;
	wide.translate(nickSnapshot);
	gxOpenRCMenu(layoutName, GXRCMenuData::BY_SLOT, NULL, slot, wide, mouseX, mouseY,
		ITEM_NONE, flipIgnoreLabel);
}

// GeneralsX: The player's own roster row - see GUICallbacks.h.
// EA's own "this is me" menu, which the GameSpy lobby opened for the local player and the relay
// lobby never opened at all. No id, no slot: SELF acts only on this player's own sign-in.
void GXBuddyOpenSelfRCMenu( Int mouseX, Int mouseY )
{
	gxOpenRCMenu("Menus/RCLocalPlayerMenu.wnd", GXRCMenuData::SELF, NULL, 0, UnicodeString::TheEmptyString,
		mouseX, mouseY, ITEM_NONE, FALSE);
}

void RequestBuddyAdd(Int profileID, AsciiString nick)
{
	// request to add a buddy
	BuddyRequest req;
	req.buddyRequestType = BuddyRequest::BUDDYREQUEST_ADDBUDDY;
	req.arg.addbuddy.id = profileID;
	UnicodeString buddyAddstr;
	buddyAddstr = TheGameText->fetch("GUI:BuddyAddReq");
	wcslcpy(req.arg.addbuddy.text, buddyAddstr.str(), MAX_BUDDY_CHAT_LEN);
	TheGameSpyBuddyMessageQueue->addRequest(req);

	// GeneralsX: Neither invite label is in the game's own tables, so a plain fetch() fell back to the loaded
	// map's map.str (the buddy overlay opens in game), and both texts are printf formats. Game tables only (a missing label is the
	// literal MISSING line, as before).
	UnicodeString s;
	Bool exists = TRUE;
	s.format(TheGameText->fetchGameOnly("Buddy:InviteSent", &exists));
	if (!exists)
	{
		// no string yet.  don't display.
		return;
	}

	// save message for future incarnations of the buddy window
	BuddyMessageList *messages = TheGameSpyInfo->getBuddyMessages();
	BuddyMessage message;
	message.m_timestamp = time(nullptr);
	message.m_senderID = 0;
	message.m_senderNick = "";
	message.m_recipientID = TheGameSpyInfo->getLocalProfileID();
	message.m_recipientNick = TheGameSpyInfo->getLocalBaseName();
	message.m_message.format(TheGameText->fetchGameOnly("Buddy:InviteSentToPlayer"), nick.str());

	// insert status into box
	messages->push_back(message);

	DEBUG_LOG(("Inserting buddy add request"));

	// put message on screen
	insertChat(message);

	// play audio notification
	AudioEventRTS buddyMsgAudio("GUIMessageReceived");
	if( TheAudio )
	{
		TheAudio->addAudioEvent( &buddyMsgAudio );
	}

	lastNotificationWasStatus = FALSE;
	numOnlineInNotification = 0;
	showNotificationBox(AsciiString::TheEmptyString, s);
}

WindowMsgHandledType WOLBuddyOverlayRCMenuSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		case GWM_CREATE:
			{

				break;
			}

		case GWM_DESTROY:
			{
				rcMenu = nullptr;
				break;
			}

		case GGM_CLOSE:
			{
				closeRightClickMenu(window);
				//rcMenu = nullptr;
				break;
			}


		case GBM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				GameSpyRCMenuData *rcData = (GameSpyRCMenuData*)window->winGetUserData();
				if(!rcData)
					break;

				// GeneralsX: gx payloads discriminate on the sentinel
				// BEFORE any use of the GameSpy fields, and are deleted as their DERIVED type -
				// the base has no virtual dtor. Every action routes through the one shared seam
				// (GXRelayRunSocialCommand) so the format checks, error strings and refusals stay
				// in relayHandleSocialCommand; the id came from the store, so the taxonomy holds:
				// only id-keyed rows ever reach an id-keyed mutation. BY_SLOT actions arrive with
				// the card exchange.
				if (rcData->m_id == (GPProfile)GX_RC_PROFILE_SENTINEL)
				{
					GXRCMenuData *gx = static_cast<GXRCMenuData *>(rcData);
					window->winSetUserData(nullptr);

					Bool acted = FALSE;
					AsciiString line;
					if (gx->m_gxKind == GXRCMenuData::BY_ID && gx->m_gxId[0] != '\0')
					{
						if (controlID == buttonDeleteID)
							line.format("/remove %s", gx->m_gxId);
						else if (controlID == buttonIgnoreID)
							line.format(GXPlayerIdentity::isIgnored(gx->m_gxId)
								? "/unignore %s" : "/ignore %s", gx->m_gxId);
						else if (controlID == buttonAddID)
							line.format("/add %s %s", gx->m_gxId, gx->m_nick.str());
					}
					else if (gx->m_gxKind == GXRCMenuData::SELF)
					{
						// GeneralsX: Sign Out. NOTHING IS DONE HERE: this
						// only asks the lobby to show its confirmation, next frame, once this menu is
						// gone. Opening a message box from inside another window's callback is the
						// wedge MainMenu.cpp documents (a stranded modal, every button dead), and
						// signing out without asking would be the one irreversible click on a menu
						// whose other entries all undo.
						if (controlID == buttonStatsID)
							GXLobbyAskSignOut();
					}
					else if (gx->m_gxKind == GXRCMenuData::BY_SLOT && gx->m_gxSlot > 0)
					{
						// The slot semantics - offer, accept, decline, ignore, remove-by-resolved-
						// id - live with the latches in WOLLobbyMenu.cpp. m_nick is the snapshot
						// the menu was opened with; the action revalidates it against the roster.
						Int act = -1;
						if (controlID == buttonAddID)
							act = GX_RC_ACT_ADD;
						else if (controlID == buttonDeleteID)
							act = GX_RC_ACT_DELETE;
						else if (controlID == buttonIgnoreID)
							act = GX_RC_ACT_IGNORE;
						if (act >= 0)
							acted = GXRelayCardAction(act, gx->m_gxSlot, gx->m_nick);
					}
					delete gx;

					if (!line.isEmpty())
						acted = GXRelayRunSocialCommand(line) || acted;
					// ONLY when the overlay is actually open. A roster right-click opens these
					// same menus with the overlay closed, and s_gxMode is set only by
					// GXBuddyOverlayOpen - so refreshing unconditionally would take
					// updateBuddyInfo's GameSpy branch and dereference the buddy queue the relay
					// path never creates. The lobby's own list is redrawn by the seam's callees.
					if ((acted || !line.isEmpty()) && s_gxMode)
					{
						updateBuddyInfo();
						refreshIgnoreList();
					}
					closeRightClickMenu(window);
					break;
				}

				DEBUG_ASSERTCRASH(rcData, ("WOLBuddyOverlayRCMenuSystem GBM_SELECTED:: we're attempting to read the GameSpyRCMenuData from the window, but the data's not there"));
				GPProfile profileID = rcData->m_id;
				AsciiString nick = rcData->m_nick;

				Bool isBuddy = false, isRequest = false;
				Bool isGameSpyUser = profileID > 0;
				if (rcData->m_itemType == ITEM_BUDDY)
					isBuddy = TRUE;
				else if (rcData->m_itemType == ITEM_REQUEST)
					isRequest = TRUE;

				delete rcData;
				rcData = nullptr;

				window->winSetUserData(nullptr);
				//DEBUG_ASSERTCRASH(profileID > 0, ("Bad profile ID in user data!"));

				if( controlID == buttonAddID )
				{
					if(!isGameSpyUser)
						break;
					DEBUG_LOG(("ButtonAdd was pushed"));
					if (isRequest)
					{
						// ok the request
						BuddyRequest req;
						req.buddyRequestType = BuddyRequest::BUDDYREQUEST_OKADD;
						req.arg.profile.id = profileID;
						TheGameSpyBuddyMessageQueue->addRequest(req);

						BuddyInfoMap *m = TheGameSpyInfo->getBuddyRequestMap();
						m->erase( profileID );
						// if the profile ID is not from a buddy and we're okaying his request, then
						// request to add him to our list automatically CLH 2-18-03
						if(!TheGameSpyInfo->isBuddy(profileID))
						{
							RequestBuddyAdd(profileID, nick);
						}
						updateBuddyInfo();
					}
					else if (!isBuddy)
					{
						RequestBuddyAdd(profileID, nick);
					}
				}
				else if( controlID == buttonDeleteID )
				{
					if(!isGameSpyUser)
						break;
					if (isBuddy)
					{
						// delete the buddy
						BuddyRequest req;
						req.buddyRequestType = BuddyRequest::BUDDYREQUEST_DELBUDDY;
						req.arg.profile.id = profileID;
						TheGameSpyBuddyMessageQueue->addRequest(req);
					}
					else
					{
						// delete the request
						BuddyRequest req;
						req.buddyRequestType = BuddyRequest::BUDDYREQUEST_DENYADD;
						req.arg.profile.id = profileID;
						TheGameSpyBuddyMessageQueue->addRequest(req);
						BuddyInfoMap *m = TheGameSpyInfo->getBuddyRequestMap();
						m->erase( profileID );
					}
					BuddyInfoMap *buddies = (isBuddy)?TheGameSpyInfo->getBuddyMap():TheGameSpyInfo->getBuddyRequestMap();
					buddies->erase(profileID);
					updateBuddyInfo();
					DEBUG_LOG(("ButtonDelete was pushed"));
					PopulateLobbyPlayerListbox();
				}
				else if( controlID == buttonPlayID )
				{
					DEBUG_LOG(("buttonPlayID was pushed"));
				}
				else if( controlID == buttonIgnoreID )
				{
					DEBUG_LOG(("%s is isGameSpyUser %d", nick.str(), isGameSpyUser));
					if( isGameSpyUser )
					{
						if(TheGameSpyInfo->isSavedIgnored(profileID))
						{
							TheGameSpyInfo->removeFromSavedIgnoreList(profileID);
						}
						else
						{
							TheGameSpyInfo->addToSavedIgnoreList(profileID, nick);
						}
					}
					else
					{
						if(TheGameSpyInfo->isIgnored(nick))
						{
							TheGameSpyInfo->removeFromIgnoreList(nick);
						}
						else
						{
							TheGameSpyInfo->addToIgnoreList(nick);
						}
					}
					updateBuddyInfo();
					refreshIgnoreList();
					// repopulate our player listboxes now
					PopulateLobbyPlayerListbox();
				}
				else if( controlID == buttonStatsID )
				{
					DEBUG_LOG(("buttonStatsID was pushed"));
					GameSpyCloseOverlay(GSOVERLAY_PLAYERINFO);
					SetLookAtPlayer(profileID,nick );
					GameSpyOpenOverlay(GSOVERLAY_PLAYERINFO);
					PSRequest req;
					req.requestType = PSRequest::PSREQUEST_READPLAYERSTATS;
					req.player.id = profileID;
					TheGameSpyPSMessageQueue->addRequest(req);
				}
				closeRightClickMenu(window);
				break;
			}
		default:
			return MSG_IGNORED;

	}
	return MSG_HANDLED;
}


void setUnignoreText( WindowLayout *layout, AsciiString nick, GPProfile id)
{
	AsciiString controlName;
	controlName.format("%s:ButtonIgnore",layout->getFilename().str()+6);
	NameKeyType ID =  TheNameKeyGenerator->nameToKey( controlName );
	GameWindow *win = TheWindowManager->winGetWindowFromId(layout->getFirstWindow(), ID);
	if(win)
	{
		if(TheGameSpyInfo->isSavedIgnored(id) || TheGameSpyInfo->isIgnored(nick))
			GadgetButtonSetText(win, TheGameText->fetch("GUI:Unignore"));
	}
}

void refreshIgnoreList()
{
	// GeneralsX: gx: the ignore tab reads the local store. The store
	// keeps only ids - there is no ignore nick - so a row shows the friend nick when the ignored
	// id is also a known friend, and a six-character prefix otherwise; never the whole credential.
	// Rendered in the grey the player list already uses for ignored rows.
	if (s_gxMode)
	{
		if (listboxIgnore == nullptr)
			return;

		// Same rule as the buddy list: the 1 Hz tick must not reset selection and scroll under
		// the player's mouse, so rebuild only when the ignore set actually differs.
		AsciiString fingerprint;
		{
			const Int fn = GXPlayerIdentity::getIgnoreCount();
			for (Int i = 0; i < fn; ++i)
			{
				const char *fid = GXPlayerIdentity::getIgnoreId(i);
				if (fid == NULL || fid[0] == '\0')
					continue;
				fingerprint.concat(fid);
				const char *fnick = GXPlayerIdentity::getFriendNickById(fid);
				if (fnick != NULL)
					fingerprint.concat(fnick);
				fingerprint.concat('\n');
			}
		}
		if (fingerprint == s_gxIgnoreFingerprint)
			return;
		s_gxIgnoreFingerprint = fingerprint;

		GadgetListBoxReset(listboxIgnore);
		const Int n = GXPlayerIdentity::getIgnoreCount();
		Int shown = 0;
		for (Int i = 0; i < n && shown < GX_BUDDY_ROWS; ++i)
		{
			const char *id = GXPlayerIdentity::getIgnoreId(i);
			if (id == NULL || id[0] == '\0')
				continue;
			strlcpy(s_gxIgnoreRowId[shown], id, sizeof(s_gxIgnoreRowId[shown]));

			UnicodeString name;
			const char *nick = GXPlayerIdentity::getFriendNickById(s_gxIgnoreRowId[shown]);
			if (nick != NULL && nick[0] != '\0')
			{
				name.translate(AsciiString(nick));
			}
			else
			{
				char prefixBuf[8];
				strlcpy(prefixBuf, id, 7);
				name.translate(AsciiString(prefixBuf));
			}

			const Int row = GadgetListBoxAddEntryText(listboxIgnore, name,
				GameMakeColor(110, 110, 110, 255), -1);
			// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
			GadgetListBoxSetItemData(listboxIgnore,
				reinterpret_cast<void *>(static_cast<intptr_t>(shown + 1)), row);
			++shown;
		}
		if (shown == 0)
		{
			GadgetListBoxAddEntryText(listboxIgnore,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXIgnoreEmpty", L"Nobody ignored."),
				GameMakeColor(128, 128, 128, 255), -1);
		}
		return;
	}

	SavedIgnoreMap tempMap;
	tempMap = TheGameSpyInfo->returnSavedIgnoreList();
	SavedIgnoreMap::iterator it = tempMap.begin();
	GadgetListBoxReset(listboxIgnore);
	while(it != tempMap.end())
	{
		UnicodeString name;
		name.translate(it->second);
		Int pos = GadgetListBoxAddEntryText(listboxIgnore, name, GameMakeColor(255,100,100,255),-1);
		GadgetListBoxSetItemData(listboxIgnore, (void *)it->first,pos );
		++it;
	}
	IgnoreList tempList;
	tempList = TheGameSpyInfo->returnIgnoreList();
	IgnoreList::iterator iListIt = tempList.begin();
	while( iListIt != tempList.end())
	{
		AsciiString aName = *iListIt;
		UnicodeString name;
		name.translate(aName);
		Int pos = GadgetListBoxAddEntryText(listboxIgnore, name, GameMakeColor(255,100,100,255),-1);
		GadgetListBoxSetItemData(listboxIgnore, nullptr,pos );
		++iListIt;
	}

//
//	GPProfile profileID = 0;
//	PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
//	if (it != TheGameSpyInfo->getPlayerInfoMap()->end())
//		profileID = it->second.m_profileID;

}
