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

// FILE: InGameChat.cpp ///////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell - June 2002
// Desc: GUI callbacks for the in-game chat entry
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/AudioEventRTS.h"
#include "Common/GameAudio.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/ChatRateLimit.h"
#include "GameClient/GXMatchChat.h"
#include "GameClient/GXVoteKick.h"
#include "GameClient/LanguageFilter.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"

static GameWindow *chatWindow = nullptr;
static GameWindow *chatTextEntry = nullptr;
static GameWindow *chatTypeStaticText = nullptr;
static UnicodeString s_savedChat;
static InGameChatType inGameChatType;

// GeneralsX: THE KEYSTROKE THAT OPENS THE CHAT ALSO SUBMITTED IT.
//
// Measured on the owner's Windows machine with the temporary key-path probe added in af881e0bd
// and removed again for release. ONE press of Enter produced TWO calls to ToggleInGameChat:
//
//   [GXKEY] META_CHAT_EVERYONE arrived
//   [GXKEY] ToggleInGameChat justHid=0 window=1 hidden=1   <- opens it, focuses the text entry
//   [GXKEY] ToggleInGameChat justHid=0 window=1 hidden=0   <- the SAME Enter, now via GEM_EDIT_DONE
//
// ShowInGameChat hands the keyboard focus to the text entry, and the same key event then reaches
// that entry and fires GEM_EDIT_DONE, which submits an empty line and closes the window again. The
// chat opened and shut inside one keypress. The existing justHid flag then swallowed the NEXT
// press, so it took two presses to get one visible attempt - reported as "I press enter ten times
// before I can chat".
//
// Frames, not a sticky flag: the two calls are the same client frame by construction, and a flag
// that has to be cleared by a call which might not arrive is how justHid itself can strand. An
// UnsignedInt that only ever compares equal to "this frame" cannot get stuck.
static UnsignedInt s_shownOnFrame = 0;
static Bool        s_shownFrameValid = false;

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ShowInGameChat( Bool immediate )
{
	if (TheGameLogic->isInReplayGame())
		return;

	if (TheInGameUI->isQuitMenuVisible())
		return;

	if (TheDisconnectMenu && TheDisconnectMenu->isScreenVisible())
		return;

	if (chatWindow)
	{
		chatWindow->winHide(FALSE);
		chatWindow->winEnable(TRUE);
		chatTextEntry->winHide(FALSE);
		chatTextEntry->winEnable(TRUE);
		GadgetTextEntrySetText( chatTextEntry, s_savedChat );
		s_savedChat.clear();
	}
	else
	{
		chatWindow = TheWindowManager->winCreateFromScript( "InGameChat.wnd" );

		static NameKeyType textEntryChatID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:TextEntryChat" );
		chatTextEntry = TheWindowManager->winGetWindowFromId( nullptr, textEntryChatID );
		GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );

		static NameKeyType chatTypeStaticTextID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:StaticTextChatType" );
		chatTypeStaticText = TheWindowManager->winGetWindowFromId( nullptr, chatTypeStaticTextID );
	}
	TheWindowManager->winSetFocus( chatTextEntry );
	SetInGameChatType( INGAME_CHAT_EVERYONE );

	// See the note by s_shownOnFrame: the keystroke that got us here is still in flight and is
	// about to reach the entry we just focused.
	s_shownOnFrame = TheGameClient ? TheGameClient->getFrame() : 0;
	s_shownFrameValid = (TheGameClient != nullptr);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ResetInGameChat()
{
	if(chatWindow)
		TheWindowManager->winDestroy( chatWindow );
	chatWindow = nullptr;
	chatTextEntry = nullptr;
	chatTypeStaticText = nullptr;
	s_savedChat.clear();
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void HideInGameChat( Bool immediate )
{
	if (chatWindow)
	{
		s_savedChat = GadgetTextEntryGetText( chatTextEntry );
		chatWindow->winHide(TRUE);
		chatWindow->winEnable(FALSE);
		chatTextEntry->winHide(TRUE);
		chatTextEntry->winEnable(FALSE);
		TheWindowManager->winSetFocus( nullptr );
	}
	TheWindowManager->winSetFocus( nullptr );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void SetInGameChatType( InGameChatType chatType )
{
	inGameChatType = chatType;
	if (chatTypeStaticText)
	{
		switch (inGameChatType)
		{
		case INGAME_CHAT_EVERYONE:
			if (ThePlayerList->getLocalPlayer()->isPlayerActive())
				GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Everyone") );
			else
				GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Observers") );
			break;
		case INGAME_CHAT_ALLIES:
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Allies") );
			break;
		case INGAME_CHAT_PLAYERS:
			GadgetStaticTextSetText( chatTypeStaticText, TheGameText->fetch("Chat:Players") );
			break;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
Bool IsInGameChatActive() {
	if (chatWindow != nullptr) {
		if (chatWindow->winIsHidden() == FALSE) {
			return TRUE;
		}
	}
	return FALSE;
}

// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus();
}
extern int isThreadHosting;

Bool handleInGameSlashCommands(UnicodeString uText)
{
	AsciiString message;
	message.translate(uText);

	if (message.getCharAt(0) != '/')
	{
		return FALSE; // not a slash command
	}

	AsciiString remainder = message.str() + 1;
	AsciiString token;
	remainder.nextToken(&token);
	token.toLower();

	if (token == "host")
	{
		UnicodeString s;
		s.format(L"Hosting qr2:%d thread:%d", getQR2HostingStatus(), isThreadHosting);
		TheInGameUI->message(s);
		return TRUE; // was a slash command
	}

	// GeneralsX: IN-MATCH VOTE KICK from the chat box, in both UI modes: "/kick <player name>".
	// The name as typed, else the one name equal to it ignoring case, else the one name it starts (ignoring case): LAN names
	// are unique only case-sensitively, so "Alice" must never reach "ALICE". The GeneralsX Tab board's
	// KICK button calls the same gxVoteKickStart; this is the way in that needs no UI. Never sent as chat. Every line below
	// that carries a name goes out through messageNoFormat: a nickname may hold a printf directive.
	if (token == "kick" || token == "votekick")
	{
		const WideChar *rest = uText.str() + 1;
		while (*rest != 0 && *rest != L' ')
			++rest;
		while (*rest == L' ')
			++rest;
		const UnicodeString want(rest);
		// Resolve the NAMED player among every human in the match first, eligible or not, and only then ask whether a vote
		// on that player may start: a filter applied first would turn "/kick Alice" (defeated) into a vote on ALICE, or a
		// prefix into its one eligible match.
		Int exact = -1, folded = -1, foldedCount = 0, prefix = -1, prefixes = 0;
		UnicodeString kickable;
		for (Int i = 0; ThePlayerList != nullptr && i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player *p = ThePlayerList->getNthPlayer(i);
			if (p == nullptr || p->getPlayerType() != PLAYER_HUMAN || ThePlayerList->isSharedBySlots(i))
				continue;
			const UnicodeString name = p->getPlayerDisplayName();
			if (gxVoteKickCanStart(i, nullptr))
			{
				if (!kickable.isEmpty())
					kickable.concat(L", ");
				kickable.concat(name);
			}
			if (want.isEmpty())
				continue;
			if (name.compare(want) == 0)
				exact = i;
			else if (name.compareNoCase(want) == 0)
			{
				folded = i;
				++foldedCount;
			}
			else if (name.startsWithNoCase(want))
			{
				prefix = i;
				++prefixes;
			}
		}
		const Int target = (exact >= 0) ? exact : (foldedCount == 1) ? folded : (foldedCount == 0 && prefixes == 1) ? prefix : -1;
		UnicodeString why;
		if (target >= 0)
		{
			if (gxVoteKickCanStart(target, &why))
				gxVoteKickStart(target);
			else
			{
				UnicodeString line(L"No vote to kick ");
				line.concat(ThePlayerList->getNthPlayer(target)->getPlayerDisplayName());
				line.concat(L": ");
				line.concat(why);
				TheInGameUI->messageNoFormat(line);
			}
		}
		else if (!want.isEmpty() && (prefixes > 1 || foldedCount > 1))
			TheInGameUI->messageNoFormat(UnicodeString(L"More than one player's name matches that - type the name exactly."));
		else if (kickable.isEmpty())
		{
			// Nobody can be kicked right now: say why, from the rules' own reason for the first other human.
			for (Int i = 0; ThePlayerList != nullptr && i < ThePlayerList->getPlayerCount() && why.isEmpty(); ++i)
			{
				Player *p = ThePlayerList->getNthPlayer(i);
				if (p != nullptr && p != ThePlayerList->getLocalPlayer() && p->getPlayerType() == PLAYER_HUMAN && p->isPlayerActive())
					gxVoteKickCanStart(i, &why);
			}
			UnicodeString line(L"No vote kick now: ");
			line.concat(why.isEmpty() ? UnicodeString(L"there is nobody you can vote to kick") : why);
			TheInGameUI->messageNoFormat(line);
		}
		else
		{
			UnicodeString line(L"Usage: /kick <player name>. You can vote to kick: ");
			line.concat(kickable);
			TheInGameUI->messageNoFormat(line);
		}
		return TRUE; // was a slash command
	}

	return FALSE; // not a slash command
}

// GeneralsX: The writer's own copy of a line typed in a skirmish, drawn the way
// ConnectionManager::sendChat -> processChat draws a sender's own line in a network match: sanitised, "[name]:
// text" in the player's colour, with the incoming-chat sound. The name is the in-game display name, since a
// skirmish has no LAN user.
static void showSoloChatLine(UnicodeString text)
{
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if (localPlayer == nullptr || !GXSanitizeChatText(text))
		return;

	UnicodeString name = localPlayer->getPlayerDisplayName();
	GXSanitizeChatText(name);
	UnicodeString decorated;
	decorated.format(L"[%ls]", name.str());
	const UnicodeString line = GXLobbySaid(decorated, text);

	RGBColor rgb;
	rgb.setFromInt(localPlayer->getPlayerColor());
	TheInGameUI->messageColor(&rgb, L"%ls", line.str());

	AudioEventRTS audioEvent("GUICommunicatorIncoming");
	TheAudio->addAudioEvent(&audioEvent);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ToggleInGameChat( Bool immediate )
{
	// GeneralsX @note justHid exists to swallow the SECOND ToggleInGameChat that the same Enter
	// causes (the text entry's GEM_EDIT_DONE, then the meta key). If that second call never
	// arrives, the flag stays set and eats the NEXT genuine Enter instead - which is why the fix
	// by s_shownOnFrame uses a frame number rather than another sticky flag.
	// The temporary key-path probe that measured this was removed for release; recover it from
	// af881e0bd if this path needs re-instrumenting.
	static Bool justHid = false;
	if (justHid)
	{
		justHid = false;
		return;
	}

	if (TheGameLogic->isInReplayGame())
		return;

	// GeneralsX: CHAT OPENS IN A SOLO GAME TOO.
	// EA refused here unless a SECOND human was in the match (`!isMultiPlayer() && m_netMinPlayers`,
	// and m_netMinPlayers is 1 by default), so a LAN/Online host alone with AIs pressed Enter and got nothing; a
	// skirmish was also refused one step earlier, in CommandXlat's MSG_META_CHAT_* cases. Only a game with no
	// GameInfo at all (the campaign) is still refused - the send below reads it.
	if (TheGameInfo == nullptr)
		return;

	if (chatWindow)
	{
		Bool show = chatWindow->winIsHidden();
		if (show)
			ShowInGameChat( immediate );
		else
		{
			if (chatTextEntry)
			{
				// See the note by s_shownOnFrame. The same Enter that opened this window is what
				// brought us back here; submitting on it closes the chat before anything can be
				// typed into it.
				if (s_shownFrameValid && TheGameClient && TheGameClient->getFrame() == s_shownOnFrame)
				{
					s_shownFrameValid = false;
					return;
				}

				// Send what is there, clear it out, and hide the window
				UnicodeString msg = GadgetTextEntryGetText( chatTextEntry );
				msg.trim();
				if (!msg.isEmpty() && !handleInGameSlashCommands(msg))
				{
				// GeneralsX: One chat line every ChatRate::MIN_INTERVAL_MS,
				// shared with every other chat box in the game - see GameClient/ChatRateLimit.h.
				// Checked AFTER the slash-command interception, so a command costs no allowance,
				// and the refused line is put BACK in the box rather than dropped: a rate limit
				// that also eats what you typed is a worse experience than the spam it prevents.
					// Returning here instead of falling through also leaves the chat box OPEN with
					// the line still in it, which is the whole point.
					unsigned int gxChatWait = 0;
					if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait))
					{
						TheInGameUI->message(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT(
							"GUI:ChatRateLimited",
							L"Please wait %d more second(s) before sending another message.",
							(Int)gxChatWait));
						return;
					}

					const Player *localPlayer = ThePlayerList->getLocalPlayer();
					// GeneralsX: scale32: the audience mask is RelayMaskType (64 bit), slots go to 33.
					RelayMaskType playerMask = 0;
					// GeneralsX: WHO "I" AM IS A SLOT, not a Player. Every observer slot
					// maps onto the one ReplayObserver, so on an observer's peer another observer's slot yields the very
					// Player that is local here - and "player == localPlayer" put that other observer into a Players or
					// Allies line the sender meant for itself. A slot that shares the local Player without being the
					// local slot is therefore left out of both; for a player (one Player per slot) the two tests agree.
					const Int localSlot = TheGameInfo->getLocalSlotNum();

					for (Int i=0; i<MAX_SLOTS; ++i)
					{
						const Player *player = ThePlayerList->getPlayerFromSlotIndex(i);
						if (player && localPlayer)
						{
							const Bool isMe = (i == localSlot);
							const Bool sharesMyPlayer = (player == localPlayer) && !isMe;
							switch (inGameChatType)
							{
							case INGAME_CHAT_EVERYONE:
								if (!TheGameInfo->getConstSlot(i)->isMuted())
									playerMask |= gxSlotBit(i);
								break;
							case INGAME_CHAT_ALLIES:
								if ( !sharesMyPlayer && ( (player->getRelationship(localPlayer->getDefaultTeam()) == ALLIES &&
									localPlayer->getRelationship(player->getDefaultTeam()) == ALLIES) || isMe ) )
									playerMask |= gxSlotBit(i);
								break;
							case INGAME_CHAT_PLAYERS:
								if ( isMe )
									playerMask |= gxSlotBit(i);
								break;
							}
						}
					}
					TheLanguageFilter->filterLine(msg);
					// GeneralsX: A skirmish has no TheNetwork (GameLogic.cpp picks
					// TheSkirmishGameInfo exactly when it is null): nobody to send to and nothing to send with,
					// so the line is only shown to its writer - no packet and no GameMessage, so the replay
					// never sees it, and gxMatchSendRecord below refuses without TheNetwork. A solo LAN/Online
					// host DOES have TheNetwork and keeps the normal path: sendChat finds no connection to send
					// to and its processChat echoes the line locally, as for any sender.
					if (TheNetwork != nullptr)
						TheNetwork->sendChat(msg, playerMask);
					else
						showSoloChatLine(msg);

					// GeneralsX: A RECORD-ONLY copy for the operator
					// transcript. Feature 7 of the admin panel spec.
					//
					// AFTER the native send and AFTER the language filter, so what lands in the
					// transcript is the line the other players actually received rather than the
					// one that was typed.
					//
					// RECORD-ONLY, AND THAT IS THE ENTIRE DESIGN. `playerMask` above is an audience:
					// Everyone, Allies or Players, enforced on receive at
					// ConnectionManager.cpp:1028. The relay's GXCHT carries no mask and relay.js
					// fans an accepted chat line out to every member of the room, so calling
					// Transport::sendRelayChat with `msg` here - which is tempting, the room is
					// right and this file is out of the sim digest - would put ally and private
					// conversation in plaintext on the wire to every player in the match,
					// opponents included. Discarding it in a receiving UI would restore nothing,
					// because the violation is in the sending.
					//
					// gxMatchSendRecord wraps the line in the GXM2 envelope carrying that same
					// mask. The relay recognises it ahead of parseChat, writes it to its event
					// sink, and emits no GXSAY at all.
					//
					// Note what `playerMask` means for INGAME_CHAT_EVERYONE: the loop above clears
					// the bit of any slot this client has LOCALLY muted
					// (TheGameInfo->getConstSlot(i)->isMuted()). So the recorded audience is the
					// audience this sender chose, which is the honest thing to record.
					char gxMatchChan = '\0';
					switch (inGameChatType)
					{
					// One space after each colon, deliberately unaligned: gxm1-reach-test.sh
					// greps these three lines VERBATIM, and a column of padding would make the
					// needle depend on how many spaces somebody felt like typing.
					case INGAME_CHAT_EVERYONE: gxMatchChan = 'e'; break;
					case INGAME_CHAT_ALLIES: gxMatchChan = 'a'; break;
					case INGAME_CHAT_PLAYERS: gxMatchChan = 'p'; break;
					}
					if (gxMatchChan != '\0')
						gxMatchSendRecord(msg, playerMask, gxMatchChan);
				}
				GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );
				HideInGameChat( immediate );
				justHid = true;
			}
		}
	}
	else
	{
		ShowInGameChat( immediate );
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType InGameChatInput( GameWindow *window, UnsignedInt msg,
																			WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
//			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					HideInGameChat();
					return MSG_HANDLED;
					//return MSG_IGNORED;
				}

			}

			return MSG_HANDLED;

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType InGameChatSystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		//---------------------------------------------------------------------------------------------
		case GGM_FOCUS_CHANGE:
		{
//			Bool focus = (Bool) mData1;
			//if (focus)
				//TheWindowManager->winSetGrabWindow( chatTextEntry );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;
		}

		//---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
		{
			ToggleInGameChat();
			//HideInGameChat();

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			static NameKeyType buttonClearID = TheNameKeyGenerator->nameToKey( "InGameChat.wnd:ButtonClear" );
			if (control && control->winGetWindowId() == buttonClearID)
			{
				if (chatTextEntry)
					GadgetTextEntrySetText( chatTextEntry, UnicodeString::TheEmptyString );
				s_savedChat.clear();
			}
			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

