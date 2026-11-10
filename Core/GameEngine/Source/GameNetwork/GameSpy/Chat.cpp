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

// FILE: Chat.cpp //////////////////////////////////////////////////////
// Generals GameSpy chat-related code
// Author: Matthew D. Campbell, July 2002

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdint>
#include "Common/AudioEventRTS.h"
#include "Common/INI.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/GameText.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/LanguageFilter.h"
#include "GameClient/GameWindowManager.h"
#include "GameNetwork/GameSpy/PeerDefsImplementation.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameClient/InGameUI.h"

#define OFFSET(x) (sizeof(Int) * (x))
static const FieldParse GameSpyColorFieldParse[] =
{

	{ "Default",						INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_DEFAULT) },
	{ "CurrentRoom",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CURRENTROOM) },
	{ "ChatRoom",						INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_ROOM) },
	{ "Game",								INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_GAME) },
	{ "GameFull",						INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_GAME_FULL) },
	{ "GameCRCMismatch",		INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_GAME_CRCMISMATCH) },
	{ "PlayerNormal",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_PLAYER_NORMAL) },
	{ "PlayerOwner",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_PLAYER_OWNER) },
	{ "PlayerBuddy",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_PLAYER_BUDDY) },
	{ "PlayerSelf",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_PLAYER_SELF) },
	{ "PlayerIgnored",			INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_PLAYER_IGNORED) },
	{ "ChatNormal",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_NORMAL) },
	{ "ChatEmote",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_EMOTE) },
	{ "ChatOwner",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_OWNER) },
	{ "ChatOwnerEmote",			INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_OWNER_EMOTE) },
	{ "ChatPriv",						INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_PRIVATE) },
	{ "ChatPrivEmote",			INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_PRIVATE_EMOTE) },
	{ "ChatPrivOwner",			INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_PRIVATE_OWNER) },
	{ "ChatPrivOwnerEmote",	INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_PRIVATE_OWNER_EMOTE) },
	{ "ChatBuddy",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_BUDDY) },
	{ "ChatSelf",						INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_CHAT_SELF) },
	{ "AcceptTrue",					INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_ACCEPT_TRUE) },
	{ "AcceptFalse",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_ACCEPT_FALSE) },
	{ "MapSelected",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_MAP_SELECTED) },
	{ "MapUnselected",			INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_MAP_UNSELECTED) },
	{ "MOTD",								INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_MOTD) },
	{ "MOTDHeading",				INI::parseColorInt,	nullptr,	OFFSET(GSCOLOR_MOTD_HEADING) },

	{ nullptr,					nullptr,						nullptr,						0 }

};

void INI::parseOnlineChatColorDefinition( INI* ini )
{
	// parse the ini definition
	ini->initFromINI( GameSpyColor, GameSpyColorFieldParse );
}


Color GameSpyColor[GSCOLOR_MAX] =
{
	GameMakeColor(255,255,255,255),	// GSCOLOR_DEFAULT
	GameMakeColor(255,255,  0,255),	// GSCOLOR_CURRENTROOM
	GameMakeColor(255,255,255,255),	// GSCOLOR_ROOM
	GameMakeColor(128,128,0,255),		// GSCOLOR_GAME
	GameMakeColor(128,128,128,255),	// GSCOLOR_GAME_FULL
	GameMakeColor(128,128,128,255),	// GSCOLOR_GAME_CRCMISMATCH
#if RTS_GENERALS
	GameMakeColor(255,  0,  0,255),	// GSCOLOR_PLAYER_NORMAL
#else
	GameMakeColor(255,255,255,255),	// GSCOLOR_PLAYER_NORMAL
#endif
	GameMakeColor(255,  0,255,255),	// GSCOLOR_PLAYER_OWNER
	GameMakeColor(255,  0,128,255),	// GSCOLOR_PLAYER_BUDDY
	GameMakeColor(255,  0,  0,255),	// GSCOLOR_PLAYER_SELF
	GameMakeColor(128,128,128,255),	// GSCOLOR_PLAYER_IGNORED
#if RTS_GENERALS
	GameMakeColor(255,0,0,255),			// GSCOLOR_CHAT_NORMAL
#else
	GameMakeColor(255,255,255,255),		// GSCOLOR_CHAT_NORMAL
#endif
	GameMakeColor(255,128,0,255),		// GSCOLOR_CHAT_EMOTE,
	GameMakeColor(255,255,0,255),		// GSCOLOR_CHAT_OWNER,
	GameMakeColor(128,255,0,255),		// GSCOLOR_CHAT_OWNER_EMOTE,
	GameMakeColor(0,0,255,255),			// GSCOLOR_CHAT_PRIVATE,
	GameMakeColor(0,255,255,255),		// GSCOLOR_CHAT_PRIVATE_EMOTE,
	GameMakeColor(255,0,255,255),		// GSCOLOR_CHAT_PRIVATE_OWNER,
	GameMakeColor(255,128,255,255),	// GSCOLOR_CHAT_PRIVATE_OWNER_EMOTE,
	GameMakeColor(255,  0,255,255),	// GSCOLOR_CHAT_BUDDY,
	GameMakeColor(255,  0,128,255),	// GSCOLOR_CHAT_SELF,
	GameMakeColor(  0,255,  0,255),	// GSCOLOR_ACCEPT_TRUE,
	GameMakeColor(255,  0,  0,255),	// GSCOLOR_ACCEPT_FALSE,
	GameMakeColor(255,255,  0,255),	// GSCOLOR_MAP_SELECTED,
	GameMakeColor(255,255,255,255),	// GSCOLOR_MAP_UNSELECTED,
	GameMakeColor(255,255,255,255),	// GSCOLOR_MOTD,
	GameMakeColor(255,255,  0,255),	// GSCOLOR_MOTD_HEADING,
};

Bool GameSpyInfo::sendChat( UnicodeString message, Bool isAction, GameWindow *playerListbox )
{
	static UnicodeString s_prevMsg = UnicodeString::TheEmptyString;  //stop spam before it happens

	RoomType roomType = StagingRoom;
	if (getCurrentGroupRoom())
		roomType = GroupRoom;

	PeerRequest req;
	req.text = message.str();

	message.trim();
	// Echo the user's input to the chat window
	if (!message.isEmpty())
	{
		if (!playerListbox)
		{	// Public message
			if( isAction  ||  message.compare(s_prevMsg) != 0 )  //don't send duplicate messages
			{
				req.message.isAction = isAction;
				req.peerRequestType = PeerRequest::PEERREQUEST_MESSAGEROOM;
				TheGameSpyPeerMessageQueue->addRequest(req);
				s_prevMsg = message;
			}
			return false;
		}

		// Get the selections (is this a private message?)
		Int maxSel = GadgetListBoxGetMaxSelectedLength(playerListbox);
		Int *selections;
		GadgetListBoxGetSelected(playerListbox, (Int *)&selections);

		if (selections[0] == -1)
		{	// Public message
			if( isAction  ||  message.compare(s_prevMsg) != 0 )  //don't send duplicate messages
			{
				req.message.isAction = isAction;
				req.peerRequestType = PeerRequest::PEERREQUEST_MESSAGEROOM;
				TheGameSpyPeerMessageQueue->addRequest(req);
				s_prevMsg = message;
			}
			return false;
		}
		else
		{
			// Private message

			// Construct a list
			AsciiString names = AsciiString::TheEmptyString;
			AsciiString tmp = AsciiString::TheEmptyString;
			AsciiString aStr; // AsciiString buf for translating Unicode entries
			names.format("%s", TheGameSpyInfo->getLocalName().str());
			for (int i=0; i<maxSel; i++)
			{
				if (selections[i] != -1)
				{
					aStr.translate(GadgetListBoxGetText(playerListbox, selections[i], GadgetListBoxGetNumColumns(playerListbox)-1));
					if (aStr.compareNoCase(TheGameSpyInfo->getLocalName()))
					{
						tmp.format(",%s", aStr.str());
						names.concat(tmp);
					}
				}
				else
				{
					break;
				}
			}

			if (!names.isEmpty())
			{
				req.nick = names.str();
				req.message.isAction = isAction;
				req.peerRequestType = PeerRequest::PEERREQUEST_MESSAGEPLAYER;
				TheGameSpyPeerMessageQueue->addRequest(req);
			}
			s_prevMsg = message;
			return true;
		}
	}
	s_prevMsg = message;
	return false;
}

// GeneralsX @feature Chat text drawn in a player's own lobby colour is unreadable for the
// darker half of the multiplayer palette (red, blue, purple) on the near-black chat listbox.
// Lift anything below a luma floor by blending it toward white: luma is linear in the
// components, so the exact blend factor can be solved for instead of iterating, and blending
// toward white keeps the hue recognisable instead of washing the colour out to grey.
static Color MakeChatColorReadable( Color color )
{
	const Int minLuma = 128;

	UnsignedByte red, green, blue, alpha;
	GameGetColorComponents(color, &red, &green, &blue, &alpha);

	// Rec.601 weights scaled to 256 so this stays integer math.
	const Int luma = (77 * red + 150 * green + 29 * blue) >> 8;
	if (luma >= minLuma)
		return color;

	const Real blend = (Real)(minLuma - luma) / (Real)(255 - luma);
	red   = (UnsignedByte)(red   + (255 - red)   * blend);
	green = (UnsignedByte)(green + (255 - green) * blend);
	blue  = (UnsignedByte)(blue  + (255 - blue)  * blend);

	return GameMakeColor(red, green, blue, alpha);
}

// GeneralsX @feature Resolve the lobby colour a chat line should be drawn in. Speakers who hold
// no slot in the current staging room (anyone chatting in a group room, or a player who already
// left), slots still set to a random colour and observers keep the GameSpy palette colour,
// which is what the "???" / "None" swatch in the slot list shows for them.
static Bool GetStagingRoomChatColor( AsciiString nick, Color *color )
{
	if (TheGameSpyGame == nullptr || !TheGameSpyGame->isInGame() || TheMultiplayerSettings == nullptr)
		return FALSE;

	const Int slotNum = TheGameSpyGame->getSlotNum(nick);
	if (slotNum < 0)
		return FALSE;

	const GameSlot *slot = TheGameSpyGame->getConstSlot(slotNum);
	if (slot == nullptr || slot->getColor() < 0 || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
		return FALSE;

	const MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(slot->getColor());
	if (def == nullptr)
		return FALSE;

	*color = MakeChatColorReadable(def->getColor());
	return TRUE;
}

void GameSpyInfo::addChat( AsciiString nick, Int profileID, UnicodeString msg, Bool isPublic, Bool isAction, GameWindow *win )
{
	PlayerInfoMap::iterator it = getPlayerInfoMap()->find(nick);
	if (it != getPlayerInfoMap()->end())
	{
		addChat( it->second, msg, isPublic, isAction, win );
	}
	else
	{
	}
}

void GameSpyInfo::addChat( PlayerInfo p, UnicodeString msg, Bool isPublic, Bool isAction, GameWindow *win )
{
	Int style;
	if(isSavedIgnored(p.m_profileID) || isIgnored(p.m_name))
		return;

	Bool isOwner = p.m_flags & PEER_FLAG_OP;
	Bool isBuddy = getBuddyMap()->find(p.m_profileID) != getBuddyMap()->end();

	Bool isMe = p.m_name.compare(TheGameSpyInfo->getLocalName()) == 0;

	if(!isMe)
	{
		if(m_disallowAsainText)
		{
			const WideChar *buff = msg.str();
			Int length =  msg.getLength();
			for(Int i = 0; i < length; ++i)
			{
				if(buff[i] >= 256)
					return;
			}
		}
		else if(m_disallowNonAsianText)
		{
			const WideChar *buff = msg.str();
			Int length =  msg.getLength();
			Bool hasUnicode = FALSE;
			for(Int i = 0; i < length; ++i)
			{
				if(buff[i] >= 256)
				{
					hasUnicode = TRUE;
					break;
				}
			}
			if(!hasUnicode)
				return;
		}

		if (!isPublic)
		{
			AudioEventRTS privMsgAudio("GUIMessageReceived");

			if( TheAudio )
			{
				TheAudio->addAudioEvent( &privMsgAudio );
			}
		}
	}


	if (isBuddy)
	{
		style = GSCOLOR_CHAT_BUDDY;
	}
	else if (isPublic && isAction)
	{
		style = (isOwner)?GSCOLOR_CHAT_OWNER_EMOTE:GSCOLOR_CHAT_EMOTE;
	}
	else if (isPublic)
	{
		style = (isOwner)?GSCOLOR_CHAT_OWNER:GSCOLOR_CHAT_NORMAL;
	}
	else if (isAction)
	{
		style = (isOwner)?GSCOLOR_CHAT_PRIVATE_OWNER_EMOTE:GSCOLOR_CHAT_PRIVATE_EMOTE;
	}
	else
	{
		style = (isOwner)?GSCOLOR_CHAT_PRIVATE_OWNER:GSCOLOR_CHAT_PRIVATE;
	}

	UnicodeString name;
	name.translate(p.m_name);

	// filters language
//  if( TheGlobalData->m_languageFilterPref )
//  {
    TheLanguageFilter->filterLine(msg);
//  }

	UnicodeString fullMsg;
	if (isAction)
	{
		fullMsg.format( L"%ls %ls", name.str(), msg.str() );
	}
	else
	{
		fullMsg.format( L"[%ls] %ls", name.str(), msg.str() );
	}

	// GeneralsX @feature Public chat in a game room is drawn in the speaker's lobby colour so
	// who said what reads at a glance. Private messages keep their palette colour, otherwise a
	// whisper would be indistinguishable from something the whole room can see.
	Color chatColor = GameSpyColor[style];
	if (isPublic)
	{
		Color slotColor;
		if (GetStagingRoomChatColor(p.m_name, &slotColor))
			chatColor = slotColor;
	}

	Int index = addText(fullMsg, chatColor, win);
	if (index >= 0)
	{
		GadgetListBoxSetItemData(win, reinterpret_cast<void*>(std::uintptr_t(p.m_profileID)), index);
	}
}

Int GameSpyInfo::addText( UnicodeString message, Color c, GameWindow *win )
{
	if (TheGameSpyGame && TheGameSpyGame->isInGame() && TheGameSpyGame->isGameInProgress())
	{
		static AudioEventRTS messageFromChatSound("GUIMessageReceived");
		TheAudio->addAudioEvent(&messageFromChatSound);

		// GeneralsX: message is composed chat text (a peer's name and words); message() would use it as a
		// printf format, so "%s%n" in a chat line read and wrote through missing arguments.
		TheInGameUI->messageNoFormat(message);
	}

	if (!win)
	{
		// try to pick up a registered text window
		if (m_textWindows.empty())
			return -1;

		win = *(m_textWindows.begin());
	}
	Int index = GadgetListBoxAddEntryText(win, message, c, -1, -1);
	GadgetListBoxSetItemData(win, (void *)-1, index);

	return index;
}

void GameSpyInfo::registerTextWindow( GameWindow *win )
{
	m_textWindows.insert(win);
}

void GameSpyInfo::unregisterTextWindow( GameWindow *win )
{
	m_textWindows.erase(win);
}

