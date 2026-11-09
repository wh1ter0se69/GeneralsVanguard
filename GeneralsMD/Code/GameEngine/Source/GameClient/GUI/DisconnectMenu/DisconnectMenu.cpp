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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/DisconnectMenu.h"
#include "GameClient/GUICallbacks.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/ChatRateLimit.h"
#include "GameClient/GameText.h"
#include "GameClient/GXMatchChat.h"
#include "GameNetwork/NetworkDefs.h"		// ALL_SLOTS_MASK
#include "GameNetwork/NetworkInterface.h"

const char *const DisconnectMenu::m_playerNameTextControlNames[] = {
	"DisconnectScreen.wnd:StaticPlayer1Name",
	"DisconnectScreen.wnd:StaticPlayer2Name",
	"DisconnectScreen.wnd:StaticPlayer3Name",
	"DisconnectScreen.wnd:StaticPlayer4Name",
	"DisconnectScreen.wnd:StaticPlayer5Name",
	"DisconnectScreen.wnd:StaticPlayer6Name",
	"DisconnectScreen.wnd:StaticPlayer7Name",
	nullptr
};

const char *const DisconnectMenu::m_playerTimeoutTextControlNames[] = {
	"DisconnectScreen.wnd:StaticPlayer1Timeout",
	"DisconnectScreen.wnd:StaticPlayer2Timeout",
	"DisconnectScreen.wnd:StaticPlayer3Timeout",
	"DisconnectScreen.wnd:StaticPlayer4Timeout",
	"DisconnectScreen.wnd:StaticPlayer5Timeout",
	"DisconnectScreen.wnd:StaticPlayer6Timeout",
	"DisconnectScreen.wnd:StaticPlayer7Timeout",
	nullptr
};

const char *const DisconnectMenu::m_playerVoteButtonControlNames[] = {
	"DisconnectScreen.wnd:ButtonKickPlayer1",
	"DisconnectScreen.wnd:ButtonKickPlayer2",
	"DisconnectScreen.wnd:ButtonKickPlayer3",
	"DisconnectScreen.wnd:ButtonKickPlayer4",
	"DisconnectScreen.wnd:ButtonKickPlayer5",
	"DisconnectScreen.wnd:ButtonKickPlayer6",
	"DisconnectScreen.wnd:ButtonKickPlayer7",
	nullptr
};

const char *const DisconnectMenu::m_playerVoteCountControlNames[] = {
	"DisconnectScreen.wnd:StaticPlayer1Votes",
	"DisconnectScreen.wnd:StaticPlayer2Votes",
	"DisconnectScreen.wnd:StaticPlayer3Votes",
	"DisconnectScreen.wnd:StaticPlayer4Votes",
	"DisconnectScreen.wnd:StaticPlayer5Votes",
	"DisconnectScreen.wnd:StaticPlayer6Votes",
	"DisconnectScreen.wnd:StaticPlayer7Votes",
	nullptr
};

const char *const DisconnectMenu::m_packetRouterTimeoutControlName = "DisconnectScreen.wnd:StaticPacketRouterTimeout";
const char *const DisconnectMenu::m_packetRouterTimeoutLabelControlName = "DisconnectScreen.wnd:StaticPacketRouterTimeoutLabel";
const char *const DisconnectMenu::m_textDisplayControlName = "DisconnectScreen.wnd:ListboxTextDisplay";

static const Color chatNormalColor =  GameMakeColor(255,0,0,255);

DisconnectMenu *TheDisconnectMenu = nullptr;

DisconnectMenu::DisconnectMenu() {
	m_disconnectManager = nullptr;
}

DisconnectMenu::~DisconnectMenu() {
}

// GeneralsX @bugfix Claude 12/08/2026 A ROW WITH NO CONTROL NAME MUST NOT REACH nameToKey.
//
// The four tables below are declared [MAX_SLOTS] but defined with SEVEN names, so every entry from
// index 7 upward is a null pointer - and NameKeyGenerator::nameToKey(const char*) dereferences its
// argument with no null check (calcHashForString). Every lookup here already null-checks the
// resulting GameWindow*, which is why this looked safe; the crash is one step earlier, on the NAME.
//
// MEASURED 2026-08-12: this killed all twelve peers of a twelve-player match within ~30 frames,
// with an identical SIGSEGV in every one -
//   nameToKey(name=0x0) <- DisconnectMenu::setPlayerName(playerNum=7) <- populateDisconnectScreen
// - the moment the disconnect screen turned on. It is NOT caused by raising the slot count: index 7
// is the EIGHTH player, so a stock eight-player game whose disconnect screen appears crashes the
// same way. Raising MAX_SLOTS only makes it reachable more often, and a busy twelve-peer lobby
// reaches it immediately.
//
// A missing row is a display limit of DisconnectScreen.wnd (it has seven), not an error: skip it.
static NameKeyType rowKey( const char *controlName )
{
	return (controlName != nullptr) ? TheNameKeyGenerator->nameToKey( controlName ) : NAMEKEY_INVALID;
}

void DisconnectMenu::init() {
	m_disconnectManager = nullptr;
	HideDisconnectWindow();
	m_menuState = DISCONNECTMENUSTATETYPE_SCREENOFF;
}

void DisconnectMenu::attachDisconnectManager(DisconnectManager *disconnectManager) {
	m_disconnectManager = disconnectManager;
}

void DisconnectMenu::showScreen() {
	HideDiplomacy();
	HideInGameChat();
	HideQuitMenu();
	ShowDisconnectWindow();
	m_menuState = DISCONNECTMENUSTATETYPE_SCREENON;
}

void DisconnectMenu::hideScreen() {
	HideDisconnectWindow();
	m_menuState = DISCONNECTMENUSTATETYPE_SCREENOFF;
}

void DisconnectMenu::setPlayerName(Int playerNum, UnicodeString name) {
	NameKeyType id = rowKey(m_playerNameTextControlNames[playerNum]);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		if (!name.isEmpty()) {
			GadgetStaticTextSetText(control, name);
//			showPlayerControls(playerNum);
		}
	}

	id = rowKey(m_playerTimeoutTextControlNames[playerNum]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		if (!name.isEmpty()) {
			GadgetStaticTextSetText(control, L"");
		}
	}

	if (!name.isEmpty()) {
		showPlayerControls(playerNum);
	} else {
		hidePlayerControls(playerNum);
	}
}

void DisconnectMenu::setPlayerTimeoutTime(Int playerNum, time_t newTime) {
	NameKeyType id = rowKey(m_playerTimeoutTextControlNames[playerNum]);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	char str[33]; // itoa uses a max of 33 bytes.
	itoa(newTime, str, 10);
	AsciiString asciiNum;
	asciiNum.set(str);
	UnicodeString uninum;
	uninum.translate(asciiNum);
	if (control != nullptr) {
		GadgetStaticTextSetText(control, uninum);
	}
}

void DisconnectMenu::showPlayerControls(Int slot) {
	NameKeyType id = rowKey(m_playerNameTextControlNames[slot]);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(FALSE);
	}

	id = rowKey(m_playerTimeoutTextControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(FALSE);
	}

	id = rowKey(m_playerVoteButtonControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(FALSE);
		control->winEnable(TRUE);
	}

	id = rowKey(m_playerVoteCountControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(FALSE);
	}
}

void DisconnectMenu::hidePlayerControls(Int slot) {
	NameKeyType id = rowKey(m_playerNameTextControlNames[slot]);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(TRUE);
	}

	id = rowKey(m_playerTimeoutTextControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(TRUE);
	}

	id = rowKey(m_playerVoteButtonControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(TRUE);
		control->winEnable(TRUE);
	}

	id = rowKey(m_playerVoteCountControlNames[slot]);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);
	if (control != nullptr) {
		control->winHide(TRUE);
	}
}

void DisconnectMenu::showPacketRouterTimeout() {
	NameKeyType id = TheNameKeyGenerator->nameToKey(m_packetRouterTimeoutLabelControlName);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		control->winHide(FALSE);
	}

	id = TheNameKeyGenerator->nameToKey(m_packetRouterTimeoutControlName);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		GadgetStaticTextSetText(control, L""); // start it off with a blank string.
		control->winHide(FALSE);
	}
}

void DisconnectMenu::hidePacketRouterTimeout() {
	NameKeyType id = TheNameKeyGenerator->nameToKey(m_packetRouterTimeoutLabelControlName);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		control->winHide(TRUE);
	}

	id = TheNameKeyGenerator->nameToKey(m_packetRouterTimeoutControlName);
	control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		control->winHide(TRUE);
	}
}

void DisconnectMenu::setPacketRouterTimeoutTime(time_t newTime) {
	NameKeyType id = TheNameKeyGenerator->nameToKey(m_packetRouterTimeoutControlName);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	char str[33]; // itoa uses a max of 33 bytes.
	itoa(newTime, str, 10);
	AsciiString asciiNum;
	asciiNum.set(str);
	UnicodeString uninum;
	uninum.translate(asciiNum);
	if (control != nullptr) {
		GadgetStaticTextSetText(control, uninum);
	}
}

Bool DisconnectMenu::sendChat(UnicodeString text) {
	// GeneralsX @bugfix Claude 16/09/2026 THE SHARED CHAT COOLDOWN, WHICH THIS SCREEN WAS MISSING.
	//
	// ChatRateLimit.h's whole premise is that a limiter written into one chat box is not a chat
	// limiter, it is a limiter on that screen and the next screen is a hole - and the disconnect
	// screen was that hole. Measured by grep, unbounded: no ChatRate and no consumeShared anywhere
	// in this file, while InGameChat.cpp:270 gates on it ahead of BOTH the native send and the
	// record. So this was the one path in the game that could outrun the relay's match-record
	// bucket (GX_MATCH_BURST = 4 refilling one per 700 ms, relay.js).
	//
	// WHAT THAT COST, precisely, because "it gets rate limited" undersells it in one direction and
	// oversells it in the other. The relay's refusal happens BEFORE sinkEmit, so an over-budget line
	// produces no record at all - not a truncated one, not a flagged one. It does increment
	// stats.matchThrottled and write a logLimited line naming room and sender, so it is not silent
	// to an operator watching the relay; but in the TRANSCRIPT it is an unmarked hole, and the text
	// is gone for good. Evidence with invisible gaps is worse than evidence with visible ones.
	//
	// HERE, IN THE FUNNEL, rather than in DisconnectWindow.cpp's GEM_EDIT_DONE handler. This is the
	// single point both sends pass through, so a second caller appearing later cannot route around
	// the gate - and DisconnectMenu.h:56 is where a reviewer looks for it.
	//
	// AHEAD OF BOTH SENDS, exactly as InGameChat does it: a line that is not delivered must not be
	// recorded either, or the transcript claims something was said that nobody heard.
	unsigned int gxChatWait = 0;
	if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait)) {
		showChat(TheGameText->FETCH_OR_SUBSTITUTE_FORMAT(
			"GUI:ChatRateLimited",
			L"Please wait %d more second(s) before sending another message.",
			(Int)gxChatWait));
		return FALSE;
	}

	TheNetwork->sendDisconnectChat(text);

	// GeneralsX @feature Claude 09/09/2026 Channel 'd' of the GXM1 record envelope.
	//
	// THIS IS A SECOND SEND PATH AND AN InGameChat-ONLY HOOK MISSES IT ENTIRELY. Disconnect-screen
	// chat goes through ConnectionManager::sendDisconnectChat (ConnectionManager.cpp:3282) and
	// NetDisconnectChatCommandMsg, not through TheNetwork->sendChat - so without this line the
	// transcript would silently omit the conversation players have while a match is falling apart,
	// which is exactly the conversation an operator most wants to read.
	//
	// THE AUDIENCE IS EVERY SLOT, and that is a fact rather than a default: sendDisconnectChat
	// carries no playerMask and relays to ALL_SLOTS_MASK ^ (1 << m_localSlot)
	// (ConnectionManager.cpp:3295), so every remaining peer could see it. Taking the value from
	// ALL_SLOTS_MASK rather than writing 0xfff means that when MAX_SLOTS grows the mask grows with it,
	// and one that exceeds the envelope's width is REFUSED by gxMatchBuildEnvelope - a loud failure
	// instead of a quietly wrong audience in evidence. (25/09/2026: MAX_SLOTS is 14, ALL_SLOTS_MASK is
	// 0x3fff, and GXM2's four mask digits carry it.)
	//
	// The line is recorded even when the local peer is the one being dropped: gxMatchSendRecord
	// writes to the 8086 relay socket, which is independent of the 8088 lockstep socket that is in
	// trouble, and Transport::sendRelayChat writes to it directly rather than through the pump.
	gxMatchSendRecord(text, (Int)ALL_SLOTS_MASK, 'd');
	return TRUE;
}

void DisconnectMenu::showChat(UnicodeString text) {
	NameKeyType displayID = TheNameKeyGenerator->nameToKey(m_textDisplayControlName);
	GameWindow *displayControl = TheWindowManager->winGetWindowFromId(nullptr, displayID);

	if (displayControl != nullptr) {
		GadgetListBoxAddEntryText(displayControl, text, chatNormalColor, -1, -1);
	}
}

void DisconnectMenu::quitGame() {
	TheNetwork->quitGame();
}

void DisconnectMenu::removePlayer(Int slot, UnicodeString playerName) {
	hidePlayerControls(slot);

	NameKeyType displayID = TheNameKeyGenerator->nameToKey(m_textDisplayControlName);
	GameWindow *displayControl = TheWindowManager->winGetWindowFromId(nullptr, displayID);

	UnicodeString text;
//	UnicodeString name;
//	name.translate(playerName);
	text.format(TheGameText->fetchGameOnly("Network:PlayerLeftGame"), playerName.str());

	if (displayControl != nullptr) {
		GadgetListBoxAddEntryText(displayControl, text, chatNormalColor, -1, -1);
	}
}

void DisconnectMenu::voteForPlayer(Int slot) {
	DEBUG_LOG(("Casting vote for disconnect slot %d", slot));
	TheNetwork->voteForPlayerDisconnect(slot); // Do this next.
}

void DisconnectMenu::updateVotes(Int slot, Int votes) {
	NameKeyType id = rowKey(m_playerVoteCountControlNames[slot]);
	GameWindow *control = TheWindowManager->winGetWindowFromId(nullptr, id);

	if (control != nullptr) {
		char votestr[16];
		itoa(votes, votestr, 10);
		AsciiString asciivotes;
		asciivotes.set(votestr);
		UnicodeString unistr;
		unistr.translate(asciivotes);

		GadgetStaticTextSetText(control, unistr);
	}
}
