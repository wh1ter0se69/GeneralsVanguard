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
// FILE: WOLLobbyMenu.cpp
// Author: Chris Huybregts, November 2001
// Description: WOL Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/MiniLog.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/CustomMatchPreferences.h"
#include "Common/version.h"
#include "Common/QuotedPrintable.h"		// UserName is stored quoted-printable; see GXApplyNickname
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/MapUtil.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameClient.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/ChatRateLimit.h"
#include "GameClient/GXNickname.h"		// gxNormalizeNickname - see gxLobbySetNickname
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"		// GeneralsX: relabelling ButtonBuddy - see WOLLobbyMenuInit
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Mouse.h"
#include "GameClient/Display.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/GameWindowTransitions.h"

#include "GameLogic/GameLogic.h"

#include "GameClient/LanguageFilter.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/GSConfig.h"
#include "GameNetwork/GameSpy/LadderDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/RankPointValue.h"

// GeneralsX @feature Matchmaking. This screen is now also driven by our own relay - see the
// "relay lobby" block below.
#include "Common/OptionPreferences.h"
#include "GameClient/GUICallbacks.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/GXFriendsKey.h"	// GXFriendsKeyOnline
#include "GameNetwork/NetworkInterface.h"	// TheNetwork, for GXFriendsKeyHere
#include "GameNetwork/Transport.h"
#include "GameNetwork/GXRelaySay.h"

// GeneralsX @feature Claude 08/08/2026 Friends and ignore. The store lives under GameClient/ and
// is not digested; see the long block by relayFriendIndexByNick for what it can and cannot do
// from this screen.
#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXLoginFlow.h"
#include "GameClient/GXLobbyPresence.h"


// ==============================================================================================
// GeneralsX @feature Claude 14/08/2026 Roster and chat come from the SHARED lobby room.
//
// This screen was always wired to a roster and a chat box; they were just wired to the WRONG room.
// SetUpOnlineLobbyLAN() enters Transport::getLocalHostRoom() - the room this client would HOST in -
// so the roster could only ever contain one member: you. Hence "I cannot see anyone until after a
// game": joining moves you into the host's room, and only then is anybody else in it.
//
// These adapters prefer TheGXLobbyPresence, which holds a membership in a room everyone shares, and
// fall back to TheLAN when it is not admitted. The fallback is not a nicety - it is what keeps a
// relay-less LAN game, and a client whose relay is unreachable, behaving exactly as it does today.
// ==============================================================================================
static inline Bool gxHaveLobbyPresence( void )
{
	return (TheGXLobbyPresence != NULL) && TheGXLobbyPresence->isAdmitted();
}

static Int gxLobbyRoster( UnsignedInt *outIps, Int maxIps )
{
	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->getRoster(outIps, maxIps);
	return (TheLAN != nullptr) ? TheLAN->getLobbyRoster(outIps, maxIps) : 0;
}

static const char *gxLobbyRosterNick( Int i )
{
	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->getRosterNick(i);
	return (TheLAN != nullptr) ? TheLAN->getLobbyRosterNick(i) : "";
}

static const char *gxLobbyRosterOS( Int i )
{
	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->getRosterOS(i);
	return (TheLAN != nullptr) ? TheLAN->getLobbyRosterOS(i) : "";
}

static const char *gxLobbyRosterRole( Int i )
{
	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->getRosterRole(i);
	return (TheLAN != nullptr) ? TheLAN->getLobbyRosterRole(i) : "";
}

static void gxLobbyRequestRoster( void )
{
	// The presence socket refreshes on its own 4 s timer, so an explicit request is a no-op there;
	// the LAN path still needs poking because its roster is request/reply.
	if (!gxHaveLobbyPresence() && TheLAN != nullptr)
		TheLAN->requestLobbyRoster();
}

static Bool gxLobbySendChat( const char *text )
{
	if (gxHaveLobbyPresence())
	{
		UnicodeString wide;
		wide.translate(AsciiString(text));
		return TheGXLobbyPresence->say(wide);
	}
	return (TheLAN != nullptr) ? TheLAN->sendLobbyChat(text) : FALSE;
}

static Bool gxLobbyTakeChat( char *outText, Int outSize, UnsignedInt *outSenderIp )
{
	if (outText == NULL || outSize <= 0)
		return FALSE;

	// Drain the shared room first, then the game room. Both, deliberately: a player sitting in a
	// game's setup screen still wants to see the lobby, and somebody browsing still wants to hear
	// the room they have joined. Neither queue starves the other - this returns one line per call
	// and the caller loops.
	if (gxHaveLobbyPresence())
	{
		UnicodeString wide;
		UnsignedInt from = 0;
		if (TheGXLobbyPresence->takeChat(wide, from))
		{
			AsciiString narrow;
			narrow.translate(wide);
			strncpy(outText, narrow.str(), outSize - 1);
			outText[outSize - 1] = '\0';
			if (outSenderIp != NULL)
				*outSenderIp = from;
			return TRUE;
		}
	}
	return (TheLAN != nullptr) ? TheLAN->takeLobbyChat(outText, outSize, outSenderIp) : FALSE;
}

// GeneralsX @bugfix Claude 09/09/2026 ONE normalised value, not one raw value and two opinions.
//
// This function is the whole bug (spec S9.7). It handed the SAME un-normalised display name to two
// senders that disagree about what a name is:
//
//   * TheLAN->setLobbyNickname  -> Transport::sendRelayNickname (Transport.cpp:1943), which filters
//     to [A-Za-z0-9_.-] but stops at GX_NICK_MAX - 1 = 23 characters;
//   * TheGXLobbyPresence->setNickname -> GXLobbyPresence::sendTags (GXLobbyPresence.cpp:348),
//     which writes the string into the datagram with no filter at all.
//
// The relay accepts neither shape. A name with a space arrives as four tokens and dies at
// relay.js:1400 (`parts.length !== 3`); a 22-character or punctuation-bearing name survives both
// client paths and dies at relay.js:1406 (NICK_RE, {1,20}). BOTH BRANCHES MATTER: they log
// byte-identically on the deployed relay, so a fix that only removed spaces would leave a residue
// that reads as "the fix did not work" (S9.7, S13).
//
// The refusal is silent, and the consequence is not: the relay stamps every chat line with the
// sender's VIRTUAL IP and never with a name, the client resolves the name from a roster the refused
// GXNICK never populated, and the player renders as "P3" to everyone in the lobby - while their
// name shows correctly in-game, because that comes from the LAN slot list, a different path.
//
// The common caller is relayClaimPersistedNickname (:707), which hands over the persisted UserName
// with no cap and no filter at all - on a fresh install, the machine name. "Alex's MacBook Pro"
// hits both branches at once.
//
// NOT FIXED IN Transport::sendRelayNickname, deliberately. Transport.cpp is inside the sim digest
// (simsourcedigest_watcher.cmake:66-104) and editing it is a hard wire break: computeEngineID
// hashes the digest, so every peer on the previous build would refuse every peer on this one. This
// file and GXLobbyPresence.cpp are both under GameClient, which is in none of those path lists.
//
// NOT FIXED IN GXApplyNickname (:4883) either. That function is deliberately more permissive: it
// keeps spaces because the LAN SLOT-LIST name legitimately has them, and relayNamesMatch (:963-986)
// exists precisely to reconcile "Big Bob" in a game listing with "BigBob" in the roster. Narrowing
// it there would break the friends and ignore features that read both spellings.
//
// THAT RECONCILIATION COVERS CHARACTER REMOVAL, NOT TRUNCATION - an accepted, bounded limitation of
// this fix, stated here rather than discovered later. relayNamesMatch walks both strings skipping
// insignificant characters and ends on `if (*a == '\0' || *b == '\0') return (*a == *b && sawOne)`
// (:976-977), so it equates "Big Bob" with "BigBob" and nothing else. The persisted-UserName path
// (relayClaimPersistedNickname, :707-729) has no cap of its own, so a UserName with MORE THAN 20
// significant characters now yields a roster nick that is a 20-character PREFIX of the LAN listing
// name, and relayNamesMatch returns FALSE for that pair: a friend or ignore entry stored under one
// spelling is invisible under the other. This is not a regression - before this fix the relay
// refused such a name outright and the roster nick was ABSENT - but it moves the case from "absent"
// to "present and different", and :988-990 says that distinction matters to the ignore
// bookkeeping. Closing it means capping GXApplyNickname or teaching relayNamesMatch about prefixes;
// both are follow-ups with their own test, and neither is in this plan.
//
// TWO NEIGHBOURS IN THIS FILE ARE DELIBERATELY LEFT ALONE. relayNameCharIsSignificant (:939-943) is
// byte-for-byte gxNicknameCharIsAccepted; relayMyCardNick (:1299-1313) is a complete
// filter-to-charset normalisation with NO LENGTH CAP whose output reaches the relay in three
// friend-card bodies (:4323 `fa`, :4373 `fq`, :4628 `fi`, each via relaySendPeerLine at :1452), and
// it falls back to TheLAN->GetMyName() exactly when the roster nick is empty - the P3 state. Both
// are on the friends/ignore comparison path, not on the GXNICK path: neither sets a relay-held
// nickname, so neither is in the wire invariant's scope and neither is changed here. Folding them
// into gxNicknameCharIsAccepted is a safe follow-up with no red arm in this plan.
// GeneralsX @bugfix Claude 15/09/2026 A refusal the player can see and act on - plan G, item G-1.
//
// The early return below was correct and INVISIBLE. "/nick !!!" normalises to nothing, so nothing
// was sent, the lobby list went on showing the old name, and the player was told the name was set.
// Silence is the defect.
//
// THE REFUSAL GOES BACK UP THROUGH outNote, NOT INTO ONE SCREEN'S CHAT BOX. That is the whole
// correction over the first attempt at this item, which called relaySocialSay from right here.
// relaySocialSay is THIS screen's sink: it writes to listboxLobbyChat, a static private to this
// file, set only in WOLLobbyMenuInit (:3913) and nullptr on every other screen - where the line
// took relaySocialSay's headless branch (:1640) and went to stderr, read by nobody.
//
// /nick HAS THREE ENTRY POINTS. Unbounded grep for GXApplyNickname across every .cpp/.h in the
// tree; each screen has its own sink and each prints outNote through it, unconditionally:
//   * WOLLobbyMenu.cpp:5070 (relaySubmitChat) -> relaySocialSay, defined :1619, writing to
//     listboxLobbyChat
//   * LanLobbyMenu.cpp:849 (lanLobbyHandleChat) -> lanLobbySay, defined :813, writing to
//     listboxChatWindow
//   * LanGameOptionsMenu.cpp:1946 (gxHandleLobbySlashCommand) -> lanGameSay, defined :1907,
//     writing to listboxChatWindowLanGame
// So a refusal folded into outNote lands in the chat box of whichever screen was asked, with no
// change to GUICallbacks.h and none to either LAN menu - and as ONE line rather than two, which
// also ends the contradiction the first attempt left ("You are now known as !!!" printed straight
// after a line saying the lobby list would not take it). Threading a GXLobbySayFunc down here
// instead would reach the same three screens at the cost of a signature change in
// GUICallbacks.h:349 and an edit to both LAN menus; outNote already goes where that would go.
//
// STILL A REQUIRED PARAMETER, STILL NO DEFAULT ARGUMENT. `outRefusal` has to be written at every
// call site, so a fourth one cannot inherit silence by forgetting; choosing wrong is a compile
// error, not a message that appears at startup unnoticed.
//
// THE OTHER CALLER PASSES nullptr ON PURPOSE. relayClaimPersistedNickname has no note to fill and
// nothing the player did to answer:
//   * WOLLobbyMenuInit (:3910) runs it two statements BEFORE listboxLobbyChat is fetched (:3913),
//     so even this screen's own sink could only reach stderr there;
//   * the host (:2894) and join (:3054) paths run it at the instant the player clicked Host or
//     Join, where a line about names reads as a failure of THAT click, and would repeat on every
//     host and every join for as long as the condition lasts.
// And the condition is not a typo. That path hands over the persisted UserName, which on a fresh
// install is the MACHINE NAME - so a refusal there means the computer is called something with no
// usable character in it, not that the player typed something wrong. "Your name was rejected",
// fired before the player has touched anything, is the confusing message this item exists to
// avoid, so that path stays deliberately silent.
//
// ACCEPTED, STATED RATHER THAN DISCOVERED LATER: that leaves a player with an unusable machine
// name rendering as P<slot> with no explanation, exactly as before this change. Telling them costs
// a one-shot notice posted AFTER the chat box exists (the init call cannot reach it at all), with
// its own "say it once per session" state; that is a separate item with its own test, and folding
// it in here would mean inventing the mechanism this fix was told to reuse.
//
// WHAT THE PLAYER READS, AND IT IS THE SAME ON ALL THREE SCREENS: exactly one line, outNote. On a
// refusal GXApplyNickname appends the clause below to its own "You are now known as X", so the
// single line carries both halves of the truth - X really did take everywhere else (RequestSetName
// ran and the preference was written, so the LAN slot list, GAME OPTIONS and in-match chat all
// carry it) and it did not take in the lobby list. Nothing is printed from inside this function.
static void gxLobbySetNickname( const char *name, UnicodeString *outRefusal )
{
	// ONCE, here, so the two senders below cannot disagree. gxNormalizeNickname empties `clean` on
	// FALSE, so an early return is the only path that can follow it.
	char clean[GX_RELAY_NICK_MAX + 1];
	if (!gxNormalizeNickname(name, clean, (Int)sizeof(clean)))
	{
		// Nothing survivable - an all-punctuation or empty name. Send NOTHING rather than a
		// datagram that can only be refused; the member keeps whatever name it already had.
		//
		// The message states the RULE, not the verdict: a player told "invalid nickname" has to
		// guess, and every guess costs another round trip through this same silence.
		//
		// LENGTH IS NEVER WHY THIS FIRES, so it is not offered as the reason. gxNormalizeNickname
		// (GXNickname.h) TRUNCATES on length - `cap = min(outSize - 1, GX_RELAY_NICK_MAX)` - and
		// returns FALSE only when `n == 0`, i.e. when NOT ONE character was accepted. A 30-character
		// name is cut and taken; it never reaches this branch. The first version of this message
		// said "up to 20 characters" among the reasons, which would send a player who typed "!!!"
		// away shortening a name whose length was never the problem.
		//
		// NO NUMBER IS NAMED AT ALL, and that is the second correction. The version after that one
		// kept the cap as "it keeps the first 20 of those" - still wrong, because 20 IS UNREACHABLE
		// ON THIS PATH. GXApplyNickname fills WideChar buf[g_lanPlayerNameLength + 1] and stops at
		// `kept < g_lanPlayerNameLength` (:4886, :4891), and g_lanPlayerNameLength is 12
		// (LANAPI.h:49 - LANAPI.h:48 carries the old 20, commented out). So at most twelve
		// characters ever reach gxNormalizeNickname's cap of twenty, and a player who read "the
		// first 20" and retried with a twenty-character name would get twelve. Two caps apply in
		// sequence and neither is this refusal's cause, so the message names the CHARACTERS and
		// leaves length alone. Do not put a number back without reconciling both caps.
		//
		// A CLAUSE, NOT A SENTENCE. GXApplyNickname concatenates this onto "You are now known as X",
		// so it opens mid-sentence on purpose; that is what makes the outcome one line instead of
		// two that argue. It is deliberately NOT a printf format: the caller appends it rather than
		// formatting through it, so a translated .csf entry cannot carry a stray %s into vswprintf.
		if (outRefusal != NULL)
		{
			*outRefusal = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNickUnusable",
				L"- but not in the lobby list: there is nothing in that name it can use. A lobby name "
				L"takes letters, digits, underscore, dot and hyphen. "
				L"Try /nick <name> again with some of them.");
		}
		return;
	}

	// BOTH, always. The name has to be claimed in the shared room so other browsers see it, and in
	// the game room so the slot list does. Claiming only one is how a player ends up named in one
	// list and "P3" in the other.
	if (TheGXLobbyPresence != NULL)
		TheGXLobbyPresence->setNickname(AsciiString(clean));
	if (TheLAN != nullptr)
		TheLAN->setLobbyNickname(clean);
}

void refreshGameList( Bool forceRefresh = FALSE );
void refreshPlayerList( Bool forceRefresh = FALSE );

#ifdef DEBUG_LOGGING
#define PERF_TEST
static LogClass s_perfLog("Perf.txt");
#define PERF_LOG(x) s_perfLog.log x
#else // DEBUG_LOGGING
#define PERF_LOG(x)
#endif // DEBUG_LOGGING

// Defined in LanLobbyMenu.cpp. The LAN screens all declare it the same way; see the note in
// WOLLobbyMenuInit for why this screen has to clear it too.
extern Bool LANbuttonPushed;

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static Bool isShuttingDown = false;
static Bool buttonPushed = false;
static const char *nextScreen = nullptr;
static Bool raiseMessageBoxes = false;
static time_t gameListRefreshTime = 0;
// GeneralsX @bugfix Claude 07/08/2026 10 s was long enough to read as broken.
//
// A player joining a game changes its slot count, and at a 10 s sweep the browser showed the old
// count until the next one - reported as "I joined from the PC and the iPad still said 4/8 until
// I hit refresh". The sweep is one small datagram out and one per listed game back, so 3 s costs
// almost nothing and makes the list feel live.
//
// GeneralsX @info Claude 17/08/2026 Two corrections, because both would send the next tuner wrong.
//
// THE BUDGET IS NOT PER-SENDER. This used to read "the relay's own per-sender reply budget is the
// backstop if this is ever tuned lower". There is no per-sender budget and there deliberately never
// will be: REPLY_BUDGET_PER_SEC (tools/relay/relay.js:293, default 1000 datagrams/s) is ONE counter
// for the whole listening port, shared by every client, and relay.js:2042-2045 records the reasoning
// - "a per-source counter would itself be an unbounded table keyed by an address the sender chose,
// which is the bug it exists to prevent". So the cost of tuning this down is not paid by the tuner,
// it is paid by everyone on that relay: B browsers against G games spend B*G/interval of a shared
// 1000/s, with one burst clamped to MAX_GAMES_PER_LIST = 32 (relay.js:286). Halving the interval
// doubles that draw against a ceiling that chat, GXWHO and GXMEMB also spend from.
//
// THIS INTERVAL WAS NEVER THE DOMINANT LATENCY TERM, so tuning it was the wrong lever anyway. The
// browser's staleness was mostly the transport publishing the PREVIOUS sweep - replies sat complete
// and unread for a whole interval on top of the wait for the poll, which is the documented "3-6 s"
// (docs/WORKDIR/LOBBY_PRESENTATION_FINDINGS.md §1.2). Transport::settleGameList() now ends a sweep
// on silence instead, which removes that term for zero extra datagrams. Measure before tuning this:
// the remaining term is a uniform 0..interval wait, so the honest gain from halving it is ~1.5 s of
// mean latency for double the relay draw.
static const time_t gameListRefreshInterval = 3000;
static time_t playerListRefreshTime = 0;
static const time_t playerListRefreshInterval = 5000;
// GeneralsX @bugfix Claude 20/09/2026 (owner) The roster as it was last PAINTED - see gxLobbyRosterPrint
// and refreshPlayerList. The initial value is not load-bearing and no claim is made that a real roster
// cannot hash to it: the first paint is forced by `playerListRefreshTime == 0` regardless, and it is that
// arm, not this value, that guarantees the screen is drawn once before the print means anything.
static UnsignedInt s_playerListPrint = 0;

void setUnignoreText( WindowLayout *layout, AsciiString nick, GPProfile id);
static void doSliderTrack(GameWindow *control, Int val);
Bool DontShowMainMenu = FALSE;
enum { COLUMN_PLAYERNAME = 1 };

// window ids ------------------------------------------------------------------------------
static NameKeyType parentWOLLobbyID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonRefreshID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType buttonBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyPlayersID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyChatID = NAMEKEY_INVALID;
static NameKeyType comboLobbyGroupRoomsID = NAMEKEY_INVALID;
//static NameKeyType // sliderChatAdjustID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentWOLLobby = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonRefresh = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *buttonBuddy = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *listboxLobbyPlayers = nullptr;
static GameWindow *listboxLobbyChat = nullptr;
static GameWindow *comboLobbyGroupRooms = nullptr;
static GameWindow *parent = nullptr;

static Int groupRoomToJoin = 0;
static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

// ==============================================================================================
// GeneralsX @feature Matchmaking - the relay lobby.
//
// This screen (WOLCustomLobby.wnd) is the multi-column game browser people recognise from Online.
// It ships in our archives and was unreachable, because the Online button used to start the
// GameSpy chain and GameSpy has been dead since 2014.
//
// Nothing below re-implements GameSpy. The screen and LobbyUtils are pure presentation: they read
// staging rooms out of TheGameSpyInfo, colour and sort them, and draw them. They do not care where
// the rows came from. So this is a DATA SOURCE SWAP - our relay's GXGAME replies are mapped onto
// GameSpyStagingRoom objects and handed to addStagingRoom, and the existing RefreshGameListBoxes
// draws them.
//
// The signal for "there is no GameSpy backend" is TheGameSpyPeerMessageQueue being null, which is
// what SetUpGameSpyForRelayLobby deliberately leaves alone: WOLLobbyMenuUpdate already gates its
// entire GameSpy message pump on that pointer, so a null queue disables the GameSpy half of this
// file without a single edit to it.
//
// Two things are honestly missing rather than approximated, and are called out where they bite:
//   * there is no chat channel in the relay at all, so the chat box is inert;
//   * the relay does not track player identities across rooms, so the player list can only show
//     the occupants of the SELECTED game - and of those, only the host is actually named.
// ==============================================================================================

/// TRUE when this screen is being driven by our relay rather than by GameSpy. Decided once, in
/// WOLLobbyMenuInit, off the absence of a GameSpy backend.
static Bool s_relayLobby = FALSE;

// GeneralsX @feature Claude 11/08/2026 The entry banner is printed ONCE per run. See the long note
// at the print site for why this is a flag and not the chat box's own emptiness.
static Bool s_lobbyBannerPrinted = FALSE;

/// TRUE when WE created the GameSpy presentation singletons and must therefore tear them down
/// again on the way out. Never true if a real GameSpy session was already up.
static Bool s_relayOwnsGameSpy = FALSE;

// The reply to a GXLIST is a burst of one datagram per game with no terminator, and
// Transport::requestGameList zeroes the count the moment it asks. So the count walks 0, 1, 2, 3
// across successive pumps and there is nothing that says "that was all of them". Rebuilding on
// every step would empty and refill the listbox under the player's cursor every ten seconds.
// Rebuild when the count has stopped moving instead: one frame of latency, no flicker, and a sweep
// that legitimately returns nothing still empties the list (0 twice in a row is settled too).
static Int s_relayCountLastFrame = -1;
static Int s_relayCountLastBuilt = -1;
// GeneralsX @bugfix Claude 07/08/2026 What the list CONTAINED when it was last built, not just how
// many rows it had. See relayLobbyUpdate.
static UnsignedInt s_relayListLastBuilt = 0;
static time_t s_relayLastQueryTime = 0;
// GeneralsX @bugfix Claude 12/08/2026 The withdrawal serial this screen has already asked about.
//
// A player who leaves a game they hosted lands back HERE, and the relay has just been told the game
// is gone - but this screen browses on a 3 s cadence, so their own browser could still show their
// own dead game for most of a poll after the relay had stopped serving it. That is the one case
// where the row is stale purely because we did not ask, and asking costs one small datagram.
static UnsignedInt s_relayWithdrawSerialSeen = 0;
// TRUE once the relay has answered anything about this room, so the UI can tell "still asking"
// from "asked, and the answer is nobody". Cleared on every lobby entry.
static Bool s_relayHeardFromRelay = FALSE;
static Int s_relaySelectedID = -1;

/// The listbox item data is a plain Int game ID, so a row the player clicked has to be resolvable
/// back to the room it lives in. The room is not optional and is not derivable from the address:
/// the relay allocates identities PER ROOM, so the first player in every room is 10.42.0.1 and the
/// host address does not identify a game.
struct RelayListedGame
{
	Int id;
	char room[32];
	UnsignedInt hostVirtualIP;
	Int players;
	Int slots;
};
static RelayListedGame s_relayGames[Transport::MAX_RELAY_LISTINGS];
static Int s_relayGameCount = 0;
static Int s_relayNextGameID = 1;

static void relayResetGameTable()
{
	s_relayGameCount = 0;
	s_relayNextGameID = 1;
	s_relayCountLastFrame = -1;
	s_relayCountLastBuilt = -1;
	s_relayListLastBuilt = 0;
	s_relayLastQueryTime = 0;
	s_relayHeardFromRelay = FALSE;
	s_relaySelectedID = -1;
	// GeneralsX @bugfix Claude 20/09/2026 (owner) Reset with its siblings: a print carried over from the
	// previous visit would match the new screen's first roster and hold the repaint back exactly when the
	// player is waiting for it.
	s_playerListPrint = 0;
}

static const RelayListedGame *relayFindGameByID( Int id )
{
	for (Int i = 0; i < s_relayGameCount; ++i)
	{
		if (s_relayGames[i].id == id)
			return &s_relayGames[i];
	}
	return nullptr;
}

/// The transport, but only when it is actually relaying. Everything that reads a game list has to
/// ask the TRANSPORT rather than the preferences file: since the relay assigns identities,
/// LocalVirtualIP is usually absent, and keying off it hides the list on exactly the setups
/// assignment was built for.
static Transport *relayTransport()
{
	Transport *t = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
	return (t != nullptr && t->isRelayEnabled()) ? t : nullptr;
}

/// Turn the relay's current game list into staging rooms.
///
/// Rebuilt wholesale rather than diffed, because the relay has no teardown message ON PURPOSE - a
/// host that crashes or loses its link cannot send one, so a room drops off by simply not being in
/// the next sweep. The list IS the truth; diffing it would only invent ways to disagree with it.
// GeneralsX @bugfix Claude 07/08/2026 Show the authored map name in the browser row.
//
// The row prints md->m_displayName when TheMapCache->findMap() hits, and the raw string otherwise
// (LobbyUtils.cpp:617-638, and identically in the row tooltip at :301-319). It was always missing,
// so the lobby showed "twilight flame" where the game-options screen says "Twilight Flame (8)".
//
// The reason is NOT that the advertised string is escaped or wrongly cased - findMap lowercases its
// argument anyway. It is that the advertisement carries only the map's LEAF STEM. LANAPI.cpp
// (setGameAdvertisement caller) strips the directories and the ".map" on purpose, because a full
// path overflows the browser row and the widget then renders the string's TAIL - the player is
// shown "s\twilight flame\twilight flame.map)". So the wire says `twilight flame` while the cache
// is keyed by the lowercased full path, `maps\twilight flame\twilight flame.map`. It cannot match.
//
// Do NOT "fix" this by feeding listing->map to portableMapPathToRealMapPath, which is what the
// obvious reading of the working call site further down this file suggests: that function requires
// a "Maps\" / "UserData\Maps\" prefix and returns the EMPTY STRING for anything else, so on a bare
// stem it would replace a slightly-wrong name with no name at all - and trip a DEBUG_CRASH on the
// way past.
//
// Resolve by leaf instead of by reconstructing a path. Reconstructing "Maps\<stem>\<stem>.map"
// works for official maps but misses every user map on macOS, Linux and iPadOS, because
// portableMapPathToRealMapPath emits a literal backslash while disk-enumerated user-map keys carry
// the platform's own separator and findMap does not normalise either one. Scanning for the leaf is
// separator-agnostic, one code path, and also survives a map whose folder name differs from its
// file stem.
//
// Official maps win ties so the answer is deterministic when a user map shadows a stock folder
// name. That ambiguity is unresolvable from a stem alone and is only cosmetic here: the browser is
// a display, and the map that actually gets played is negotiated in the host's slot list.
static AsciiString relayResolveAdvertisedMap( const char *advertised )
{
	AsciiString raw( advertised ? advertised : "" );

	if (raw.isEmpty() || TheMapCache == nullptr)
		return raw;

	// A peer that advertises a real path (or a stem that happens to be one) needs no scan.
	if (TheMapCache->findMap( raw ) != nullptr)
		return raw;

	AsciiString wanted = raw;
	if (wanted.endsWithNoCase( ".map" ))
		wanted.truncateBy( 4 );

	AsciiString best;
	Bool bestIsOfficial = FALSE;

	for (MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
	{
		// Cache keys are full paths and carry whichever separator the platform produced, so look
		// for the last of EITHER. LobbyUtils' own fallback checks only '\\', which is one of the
		// reasons a POSIX-separated key reads back as the whole path.
		const char *key = it->first.str();
		const char *leaf = key;
		for (const char *c = key; *c != '\0'; ++c)
		{
			if (*c == '\\' || *c == '/')
				leaf = c + 1;
		}

		AsciiString stem( leaf );
		if (stem.endsWithNoCase( ".map" ))
			stem.truncateBy( 4 );

		if (stem.compareNoCase( wanted ) != 0)
			continue;

		const Bool isOfficial = it->second.m_isOfficial;
		if (best.isEmpty() || (isOfficial && !bestIsOfficial))
		{
			best = it->first;
			bestIsOfficial = isOfficial;
		}

		if (bestIsOfficial)
			break;			// an official map is the best answer available; stop looking
	}

	// Missing the map entirely is normal - the host may own one we do not. Keeping the raw stem
	// leaves today's behaviour rather than blanking the column.
	return best.isEmpty() ? raw : best;
}

// GeneralsX @feature Claude 07/08/2026 Turn the wire's OS tag into something readable.
//
// The tag on the wire stays one or two characters ("W", "M", "L", "iP") because it rides a
// keepalive and is validated by the relay's PLAT_RE. The label is expanded HERE, on the client,
// so the wording can change without touching the protocol or redeploying the relay - and so an
// unknown tag from a future peer still renders as itself rather than vanishing.
//
// Real logos would need texture art inside a .big plus a MappedImages INI entry, and dataID does
// scan Data\INI, so that risks refusing joins unless every peer ships identical data. The Apple
// and Windows marks are also trademarks, which is a separate question from whether it is possible.
static UnicodeString relayPlatformLabel( const char *tag )
{
	UnicodeString label;
	if (tag == NULL || tag[0] == '\0')
		return label;

	if (strcmp(tag, "W") == 0)
		label = L"Win";
	else if (strcmp(tag, "M") == 0)
		label = L"Mac";
	else if (strcmp(tag, "L") == 0)
		label = L"Linux";
	else if (strcmp(tag, "iP") == 0)
		label = L"iPad";
	else
		label.translate(AsciiString(tag));	// unknown platform: show what it claimed

	return label;
}

// GeneralsX @bugfix Claude 07/08/2026 Widen the player list's icon column.
//
// WOLCustomLobby.wnd declares this listbox COLUMNS: 2 at 10% and 90%. Ten percent is about two
// characters, and the note further down records that overflow WRAPS rather than clipping - so a
// longer label would make one row visibly taller than its neighbours. That ceiling was a data
// artefact, not a law: the percentages live in ListboxData and are turned into pixels by one
// formula, so widening the column in code lifts it. See RelayFixGameListColumns for the same
// treatment of the game list, and why this is preferred to shipping a corrected .wnd.
static void relayFixPlayerListColumns( GameWindow *listbox )
{
	if (listbox == nullptr)
		return;

	ListboxData *list = (ListboxData *)listbox->winGetUserData();
	if (list == nullptr || list->columnWidth == nullptr || list->columnWidthPercentage == nullptr)
		return;

	if (list->columns != 2)
		return;			// a modded layout keeps whatever its author intended

	list->columnWidthPercentage[0] = 22;	// fits "Linux"
	list->columnWidthPercentage[1] = 78;

	Int totalWidth, height;
	listbox->winGetSize( &totalWidth, &height );
	if (list->slider)
	{
		ICoord2D sliderSize;
		list->slider->winGetSize( &sliderSize.x, &sliderSize.y );
		totalWidth -= sliderSize.x;
	}

	for (Int i = 0; i < 2; ++i)
		list->columnWidth[i] = list->columnWidthPercentage[i] * totalWidth / 100;
}

// GeneralsX @bugfix Claude 07/08/2026 Claim the persisted name as the RELAY-held nickname too.
//
// The name has always been restored into the slot list on both the host and join paths, via
// RequestSetName from LANPreferences - but never as the relay nickname, which is the one the
// browser's player list and chat actually render. So a name set with /nickname in the lobby was
// forgotten the moment you entered a game, and had to be typed again.
//
// Two halves were missing and both are needed: the transport now REMEMBERS the claimed name and
// re-announces it whenever the room changes (a relay nickname lives on a per-ROOM member record,
// and joining a game moves you to that game's room), and this claims the persisted name up front
// so a returning player never has to type it at all.
//
// This is the device-scoped identity, and it needs no account and no device UID: the name lives
// in that machine's own LANPreferences, so it follows the device everywhere and survives a
// restart. It is not AUTHENTICATED - two peers can still claim the same name - but nothing here
// can fix that without the account system.
static void relayClaimPersistedNickname()
{
	if (TheLAN == nullptr)
		return;

	LANPreferences userprefs;
	UnicodeString name = userprefs.getUserName();
	if (name.isEmpty())
		return;			// nothing chosen yet; the relay will show us as P<slot>

	AsciiString ascii;
	ascii.translate(name);

	// gxLobbySetNickname normalises to the relay's own rule - [A-Za-z0-9_.-], capped at 20 - so a
	// name with spaces or punctuation is narrowed rather than rejected. sendRelayNickname filters
	// too, but caps at GX_NICK_MAX - 1 = 23, which is exactly why it cannot be the only guard:
	// a 21-to-23 character name survives it and the relay still refuses it on NICK_RE.
	//
	// nullptr: this is not the player asking, and there is no outNote going anywhere from here.
	// A refusal here is their MACHINE NAME being unusable, at a moment they did not choose - see
	// the block on the function itself.
	gxLobbySetNickname(ascii.str(), nullptr);
}

// ==============================================================================================
// GeneralsX @feature Claude 08/08/2026 Friends, lobby ignore, and a friends-only game filter.
//
// The store behind this is GXPlayerIdentity (Core/.../GameClient/GXPlayerIdentity.{h,cpp}). It was
// written, correct, compiling - and completely unreferenced: an unbounded grep for the class name
// returned nothing outside its own two files. Everything here is wiring. No storage logic is
// duplicated, and GXPlayer.ini's format is not touched.
//
// HOW A ROOM SLOT BECOMES A REAL PERSON. This is the part that used to be missing, and everything
// else here follows from it.
//
// A friend edge is keyed by the durable 26-character id, and that id is deliberately never put in a
// roster, never in a game listing and never broadcast. So nothing the RELAY sends carries one:
// GXMEMB gives a room slot plus an unauthenticated nickname, and a GXGAME listing gives the host's
// advertised name. GXKEY does not change that - it tells the relay our id so that GXSEEN/GXHERE can
// answer "is this id connected", and the relay still never hands an id to anybody.
//
// That is correct and it must stay that way, because an id is a BEARER TOKEN: knowing it is being
// it. A roster that carried ids would hand every stranger in the lobby the ability to impersonate
// everyone else in it, permanently, to all of their friends.
//
// So an id is learned the only way a bearer token safely can be - by DISCLOSURE, from its owner,
// to somebody who asked. That is the friend card, three unicast peer lines with consent at both
// ends:
//
//     A: /friend request P3   ->  "fq <nick>"           carries NO id. A is asking.
//     B: /friend accept P1    ->  "fa <idB> <nickB>"    B discloses, because A asked.
//     A (automatically)       ->  "fi <idA> <nickA>"    A discloses, because B accepted.
//
// Neither id reaches anyone who did not first act. An unsolicited "fa" or "fi" is dropped on the
// floor - accepting one would let any member of the lobby inject themselves into your friends list.
//
// WHAT THE CARD IS AND IS NOT WORTH. A peer composes its own card, so a peer can put SOMEBODY
// ELSE'S id in it. That sounds fatal and is not, and the reason is worth stating so nobody
// "hardens" it into something worse: to name another player's id you must already know it, and if
// you know it you can already be them everywhere. The card adds no attack that holding the id did
// not already give. What it must not do is let a card arrive UNASKED, and that is what the pending
// state below enforces.
//
// The remaining honest gap is that this exchange needs both people in one lobby at the same time.
// /friend add <code> still exists for a code passed out of band, and /mycode still prints yours on
// demand rather than parking a credential in the chat box where it lands in every screenshot.
//
// WHAT IS NOW KEYED ON THE ID, AND WHAT IS STILL KEYED ON A NAME. The distinction is the whole
// difference between a trust boundary and a display filter, so it is spelled out rather than left
// to be inferred:
//
//   ON THE ID (real):
//   * the friends list itself - it always was; GXPlayer.ini has only ever held `friend.<id>=<name>`
//   * the player-list friend colour and the "in this lobby" marker, FOR ANY SLOT WHOSE ID IS KNOWN
//   * the durable ignore list, which was deliberately left unwired until this existed. /ignore on a
//     slot with a known id is now permanent and survives a restart
//   * presence - "online" in the friends panel comes from GXHERE, which the relay answers from its
//     own registration table
//   * a private message, which is addressed to a room slot and delivered by the relay to that one
//     member
//
//   ON A NAME (a display convenience, never a trust boundary):
//   * the friends-only BROWSER filter. A game listing carries the host's advertised name and
//     nothing else, and there is no channel to learn a host's id before joining their game -
//     GXHERE answers "connected to this relay", not "hosting room X". So this stays a name match
//     and its failure mode stays a stranger's game appearing in a filtered list.
//   * the friend colour and ignore for a slot whose id is NOT known - i.e. anyone you have not
//     exchanged a card with. Marked at every call site as the fallback it is.
//   * /ignore on such a slot, which stays SESSION-scoped and slot-keyed exactly as before. That is
//     not a limitation to be fixed later: a durable ignore keyed on a nickname mutes whoever claims
//     that name next, which is the precise failure GXPlayerIdentity.h warns about.
//
// ONE MORE THING THAT IS NOT A LIMITATION. A card exchange, a private message and the presence poll
// all work against the CURRENTLY DEPLOYED relay or degrade to nothing, never to a leak. See
// relaySendPeerLine for why a private message is not a relay control tag.
// ==============================================================================================

/// Room slots are the last octet of a relay-assigned virtual address, 1..MEMBERS_PER_ROOM.
// GeneralsX @bugfix Claude 14/08/2026 This indexes RELAY SLOTS, so it must equal the relay's
// maximum - and it silently did not, once MEMBERS_PER_ROOM went 8 -> 12.
//
// Eleven statics are sized [GX_ROOM_SLOTS + 1] and indexed by a slot the relay issues, which is
// 1..12. At 8 that is a nine-element array indexed up to 12: an out-of-bounds write of a Bool plus
// up to 32 bytes of nickname, reachable by any player typing "/ignore <name>". The numeric branch
// of relayResolveRoomSlot clamps, but the NICKNAME branch returns ips[i] straight from the roster
// and does not - and the /ignore path has no upper bound between resolve and use, only a slot != 0
// and a not-me test.
//
// It does NOT need nine concurrent players. getLobbyRoster caps the COUNT at this value, never the
// VALUES: with two people in the room, one of them holding relay slot 11 is enough.
//
// Raising it also fixes two silent truncations in the same file - the roster fetch dropped members
// past the eighth, and rows 9-12 of the player list could never be reached.
// MUST equal Transport::GX_ROSTER_MAX (Transport.h) and the relay's MEMBERS_PER_ROOM (relay.js).
// Written as a literal because GX_ROSTER_MAX is private and Transport.h is inside the sourceID
// digest - reaching for it would move the digest and force an atomic four-platform rebuild for a
// bounds fix that otherwise needs none. If the relay's room size changes again, all three move
// together, and this one being SMALLER is an out-of-bounds write rather than a display glitch.
// GeneralsX @feature Claude 25/09/2026 12 -> 14 with the relay's MEMBERS_PER_ROOM (12 players + 2
// observers), all three together as the note above says.
// GeneralsX @feature Claude 06/11/2026 scale32: 14 -> 34 with the relay (32 players + 2 observers), all three together.
enum { GX_ROOM_SLOTS = 34 };
/// Comfortably over the relay's MAX_NICK_LEN (20) and over Transport's own GX_NICK_MAX (24),
/// which is private. Sized here rather than shared so this file cannot be broken by a change
/// inside the digest.
enum { GX_LOBBY_NICK_MAX = 32 };

/// TRUE while the browser is showing only games hosted by somebody on the friends list. Session
/// state on purpose: it is a view setting, and a filter silently restored on a later launch is how
/// an empty browser gets reported as the relay being down.
static Bool s_relayFriendsOnly = FALSE;

/// 26 base32 characters plus a NUL - GX_PLAYER_ID_LEN + 1. Sized here rather than taken from
/// Transport for the same reason GX_LOBBY_NICK_MAX is: this file must not be breakable by a change
/// inside the digest.
enum { GX_LOBBY_ID_MAX = 27 };

/// Transport::m_relayRoom is char[32] and the relay's TOKEN_RE caps a room at 31 characters. Sized
/// here rather than shared for the same reason GX_LOBBY_NICK_MAX is: this file must not be
/// breakable by a change inside the digest.
enum { GX_LOBBY_ROOM_MAX = 32 };

// Session ignore, keyed by room slot, remembering the nickname that slot held when it was set.
// The nickname is not decoration. Slots are recycled across humans, so an entry is DROPPED as soon
// as the name at that slot changes - muting an innocent arrival is a worse failure than an ignore
// that a rename escapes, and a rename is visible in the lobby while a silent mute is not.
//
// This is the FALLBACK path now. A slot whose durable id is known is ignored durably instead, in
// the store, and the two are checked in that order - see relaySlotIsIgnored.
static Bool s_ignoreSlot[GX_ROOM_SLOTS + 1];
static char s_ignoreNick[GX_ROOM_SLOTS + 1][GX_LOBBY_NICK_MAX];

// ---------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/08/2026 Slot -> durable id, learned this session from friend cards.
//
// SESSION SCOPE, DELIBERATELY. A room slot is the last octet of a relay-assigned virtual address;
// the relay recycles those across humans, and the operator watched his own walk P1 -> P3 -> P4 -> P5
// across a few joins. A binding that outlived the room would attach a real person's identity to
// whoever inherits their slot, which is exactly the failure the id exists to prevent - it would be
// the nickname problem again with a longer key.
//
// s_slotIdNick is the expiry mechanism, not decoration: it holds the name the slot carried at the
// moment the binding was made, and the binding is dropped as soon as the roster disagrees. Same
// rule s_ignoreNick already follows, and the two have to agree or a line could be labelled with one
// person's name and matched against another's identity.
static char s_slotId[GX_ROOM_SLOTS + 1][GX_LOBBY_ID_MAX];
static char s_slotIdNick[GX_ROOM_SLOTS + 1][GX_LOBBY_NICK_MAX];

// The friend-card handshake, one pending state per slot. See the design block above for the
// sequence; these are what make an UNSOLICITED card droppable, which is the only thing standing
// between "friends are real" and "anyone in the lobby can add themselves to your list".
//
// GeneralsX @feature Claude 11/08/2026 THE SENDING HALF IS BACK, driven by the overlay, not by
// chat verbs. The chat-hygiene pass retired /friend request and /friend accept and left this
// state only-ever-cleared; the buddy overlay's right-click menus are the surface that was always
// supposed to drive it. GXRelayCardAction is the single writer: Add on a stranger sends "fq" and
// arms s_cardWeAsked; Add on a request row sends "fa" and arms s_cardWeAccepted; Decline clears
// s_cardTheyAsked and sends NOTHING - silence is the decline, nothing of ours is disclosed. Every
// action revalidates the clicked name against the live roster first, and flags are armed only
// AFTER the send call succeeds, so a failed send leaves the exchange restartable.
//
// The mutual-add decision survives intact: the three messages only CARRY codes, each side still
// performs its own unilateral add, and clicking Add on a request row IS that add - not a
// permission grant. Against a peer still on the shipped build the exchange degrades honestly:
// their build shows our "fq" as the reworded ask-for-their-code row and cannot answer "fa", so
// click-to-add needs both ends on this build; the manual /add path is unchanged either way.
static Bool s_cardTheyAsked[GX_ROOM_SLOTS + 1];		///< they sent "fq"; an overlay row offers Add/Decline
static Bool s_cardWeAsked[GX_ROOM_SLOTS + 1];		///< our "fq" is out; solicits exactly one "fa"
static Bool s_cardWeAccepted[GX_ROOM_SLOTS + 1];	///< our "fa" is out; solicits exactly one "fi"

// GeneralsX @feature Claude 11/08/2026 THE NONCE IS THE CONTINUITY PROOF. Slots are recycled and
// nicknames are claimable by anyone who can read the room, so neither can authenticate that the
// peer answering an exchange is the peer it was started with - cross-model review demonstrated
// the replay concretely. Each "fq" therefore carries a fresh unguessable token, unicast to the
// occupant holding the slot AT SEND TIME, and every "fa"/"fi" must echo the nonce of the exchange
// it answers: a replacement occupant never saw it and cannot forge it. s_cardOurNonce is the one
// we minted (validates their "fa", and our "fi" echoes it back); s_cardTheirNonce arrived in
// their "fq" (our "fa" echoes it, and it validates their "fi"). Both wiped wherever the flags
// are - a nonce must never outlive the exchange that minted it. The residual that remains is
// social, not mechanical: a nick is still a claim, and accepting a request still trusts the name
// on the row. Phase 3's verified identity is the answer to that half.
enum { GX_CARD_NONCE_LEN = 12 };
static char s_cardOurNonce[GX_ROOM_SLOTS + 1][GX_CARD_NONCE_LEN + 1];
static char s_cardTheirNonce[GX_ROOM_SLOTS + 1][GX_CARD_NONCE_LEN + 1];

// GeneralsX @feature Claude 11/08/2026 THE OTHER HALF OF THE SLOT-REUSE PROBLEM, and it is not
// the same as the one the nonce solves. The nonce stops a replacement occupant COMPLETING an
// exchange it did not start. It cannot stop us DISCLOSING to one: accepting unicasts our card to
// whoever holds the slot at that instant, and if the roster we validated against is stale, that
// is not the person whose name we clicked. Cross-model review demonstrated exactly that window -
// "before a fresh roster reply arrives".
//
// AND IT CANNOT BE CLOSED FROM THIS SIDE OF THE WIRE. An earlier draft gated acceptance on a
// "roster observation" counter; cross-model review pointed out that getLobbyRoster() returns the
// CACHED roster, so the counter advanced on every redraw and proved nothing about a reply having
// arrived. Transport owns the only true reply signal and it is inside the simulation digest, so
// this branch cannot export one. Rather than keep a gate that reads like a proof and is not, the
// honest bound is TIME: a request may only be click-accepted while it is fresh, and a stale one
// must be re-made by the other side. That does not make disclosure safe - it bounds how long the
// window stays open, which is all a client can do here.
//
// Stated plainly for whoever reads this next: ACCEPTING DISCLOSES THIS PLAYER'S CODE TO WHOEVER
// HOLDS THE SLOT AT THAT INSTANT. The nonce proves the *answer* came from the peer we were
// talking to; nothing proves the *recipient* still is. That is the bearer-token model's floor,
// documented at GXPlayerIdentity.h:37, and Phase 3's verified identity is the fix - not a
// cleverer arrangement of these three datagrams.
enum { GX_CARD_REQUEST_TTL_MS = 20000 };
static UnsignedInt s_cardTheyAskedMs[GX_ROOM_SLOTS + 1];
// GeneralsX @bugfix Claude 15/08/2026 The mirror of the above for the OUTGOING offer. Without
// it s_cardWeAsked never expires: sendRelayPeerLine does not report whether the datagram left,
// so one lost 'fq' made every later Add answer "you have already asked" for the rest of the
// session, and only a restart cleared it. Same TTL as the incoming side.
static UnsignedInt s_cardWeAskedMs[GX_ROOM_SLOTS + 1];

// The name the slot held when any of the three flags above was set.
//
// WITHOUT THIS THE CONSENT CHECK IS DEFEATABLE BY WAITING. Pending card state is keyed on a room
// SLOT, and slots are recycled while a lobby is open - so "I asked P3 to swap codes" survives P3
// leaving, and the stranger who lands in slot 3 next can answer a question they were never asked.
// Both directions are real: an unsolicited "fa" into a stale s_cardWeAsked puts a stranger on the
// friends list under the name of somebody you trusted, and a stale s_cardTheyAsked makes /friend
// accept disclose this player's code to whoever inherited the slot.
//
// Same latch-and-compare the ignore bookkeeping uses, and it inherits the same known weakness: two
// consecutive occupants who have both claimed NO nickname compare equal. That is why the binding it
// guards is session-scoped and why the id it produces is still only worth what a bearer token is
// worth - see the design block at the top of this section.
static char s_cardNick[GX_ROOM_SLOTS + 1][GX_LOBBY_NICK_MAX];

// Slot of the last private message we received, so /r has something to answer. 0 = none.
//
// s_lastDmNick is NOT decoration and is not a display cache. Room slots are recycled across humans
// while a lobby is open, so a bare slot number would make /r send a private reply to whoever
// inherited the slot of the person who messaged you - a message intended for one person delivered
// to a stranger, which is the precise failure this whole feature exists to prevent, reached by
// pressing reply. The name is latched at receive time and re-checked before the reply goes out.
static UnsignedInt s_lastDmFrom = 0;
static char s_lastDmNick[GX_LOBBY_NICK_MAX] = { 0 };

// The outstanding presence question. GXHERE is a POSITIONAL bitmask with no request id, so the ids
// have to be remembered in the order they were asked about or the answer is read against the wrong
// friends. Snapshotted rather than re-derived from the store at reply time: the list can change
// between question and answer (a /friend add lands, or a name refresh reorders nothing but could),
// and re-deriving would silently shift every bit.
static char s_presenceAsked[16][GX_LOBBY_ID_MAX];
static Int  s_presenceAskedCount = 0;
static UnsignedInt s_presenceMask = 0;			///< bit i = s_presenceAsked[i] is online
static Int  s_presenceKnownCount = 0;			///< ids the last answer covered, 0 = never answered
static UnsignedInt s_presenceLastPollMs = 0;
static UnsignedInt s_presenceAnsweredMs = 0;	///< when that answer arrived; GXPresenceAnswerCurrent ages it out

static void relayClearIgnores()
{
	for (Int i = 0; i <= GX_ROOM_SLOTS; ++i)
	{
		s_ignoreSlot[i] = FALSE;
		s_ignoreNick[i][0] = '\0';
	}
}

/// Drop every per-room binding. A stale binding is worse than none, so this is unconditional - see
/// relaySyncSocialSessionToRoom for WHEN it is right to call it.
static void relayClearSocialSession()
{
	for (Int i = 0; i <= GX_ROOM_SLOTS; ++i)
	{
		s_slotId[i][0] = '\0';
		s_slotIdNick[i][0] = '\0';
		s_cardTheyAsked[i] = FALSE;
		s_cardWeAsked[i] = FALSE;
		s_cardWeAskedMs[i] = 0;
		s_cardWeAccepted[i] = FALSE;
		s_cardNick[i][0] = '\0';
		s_cardOurNonce[i][0] = '\0';
		s_cardTheirNonce[i][0] = '\0';
		s_cardTheyAskedMs[i] = 0;
	}
	s_lastDmFrom = 0;
	s_lastDmNick[0] = '\0';
}

// GeneralsX @bugfix Claude 10/08/2026 THE FRIEND REQUEST THAT NEVER ARRIVED.
//
// This was reported as "somebody added me and I never got a request", and the request is not what
// was broken - it is delivered, and it is displayed. It was being DESTROYED a moment later.
//
// relayClearSocialSession() used to be called unconditionally from WOLLobbyMenuInit, and that
// function runs every time this screen is entered - including the extremely common return trip
// CREATE GAME -> BACK, or joining a game and leaving it. Every pending "they asked to swap codes"
// went with it. Reproduced 10/08/2026 against the live relay with scripts/test/gxpeer-probe.py:
// the requester was still in the room and still holding its slot, the chat box still showed
// "Persist wants to swap friend codes with you. /friend accept P3 to agree" - and /friend listed
// nothing pending while /friend accept P3 answered "Nobody by that name has asked to swap codes
// with you." The screen contradicted itself and the instruction on it did not work.
//
// The ORIGINAL REASONING WAS SOUND AND IS PRESERVED: entering a game moves us to that game's relay
// room, slots are the last octet of a per-room address space, and a binding carried into a
// different room would attach a real person's identity to whoever holds that number there. That is
// the one thing this feature must never do. So the wipe still happens - but on a ROOM CHANGE, which
// is what the comment always described, rather than on a SCREEN ENTRY, which is what it did.
//
// WHY KEEPING THEM WITHIN ONE ROOM IS SAFE, stated because this guards bearer tokens. Slot recycling
// inside a room is already defended per-use and not by this wipe: relaySlotDurableId() and
// relayCardPeerUnchanged() both re-read the roster on EVERY use and drop the binding the moment the
// slot answers to a different name (s_slotIdNick / s_cardNick are latched for exactly that). Those
// checks are what make an inherited slot safe, and they run whether or not this function cleared
// anything.
//
// The residual risk is the one the design already accepts and documents at s_cardNick: two
// consecutive occupants of one slot who claim the SAME name - or no name - compare equal. This
// change extends that pre-existing window across a screen re-entry; it does not create a new class
// of it, and the alternative is a friends feature that silently loses every request.
// GeneralsX @bugfix Claude 16/08/2026 THE BOUNDARY HAS TO WATCH THE ROOM THE BINDINGS ARE IN.
//
// This guard used to read the GAME room unconditionally. That was right while the peer line only
// ever travelled on the game socket, and it stopped being right the moment the carrier began
// following the roster (see relaySendPeerLine): with presence up, every array this protects is keyed
// by SHARED-room slots while the value being compared came from the game room. The two then move
// independently, and both directions are wrong.
//
//   * The game room changes while presence does not - joining or leaving a game - and a wipe fires
//     for bindings that never left their room. That is the 10/08 "friend request that never arrived"
//     bug again, from the other side.
//   * Presence re-acquires onto a different shard, or is re-admitted with a new address after a
//     relay restart, while the game room is untouched - and NO wipe fires. Stale s_slotId entries,
//     which are real people's durable identity ids, survive into a slot space where those numbers
//     name somebody else. That is precisely the thing the comment above says this must never do,
//     and it is the more dangerous half.
//
// So the key is the identity of the slot space itself: which carrier, which room, and - for
// presence - the address we were admitted under, because a re-acquire into the SAME shard still
// reshuffles who holds which number and our own address changing is the observable signal that a new
// assignment round happened. Losing presence produces a different key from having it, which is why
// getRoom() returning "" for "not admitted" is load-bearing rather than incidental.
//
// The per-use defences are unchanged and still carry the residual risk: relaySlotDurableId() and
// relayCardPeerUnchanged() re-read the roster on every use and drop a binding whose slot has started
// answering to a different name.
static char s_socialRoom[GX_LOBBY_ROOM_MAX * 2] = { 0 };

/// Wipe the per-room bindings if and only if the slot space they are keyed by changed under us.
static void relaySyncSocialSessionToRoom()
{
	char key[sizeof(s_socialRoom)];

	if (gxHaveLobbyPresence())
	{
		snprintf(key, sizeof(key), "P %s %u",
			TheGXLobbyPresence->getRoom(), TheGXLobbyPresence->getMyAddress());
	}
	else
	{
		Transport *transport = relayTransport();
		const char *room = (transport != nullptr) ? transport->getRelayRoom() : "";
		if (room == nullptr)
			room = "";
		snprintf(key, sizeof(key), "G %s", room);
	}

	if (strcmp(key, s_socialRoom) == 0)
		return;						// same slot space we made them in - a pending request is valid

	relayClearSocialSession();
	strlcpy(s_socialRoom, key, sizeof(s_socialRoom));
}

/// TRUE when c is inside the relay's nickname charset (relay.js NICK_RE, [A-Za-z0-9_.-]).
static Bool relayNameCharIsSignificant( char c )
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
	    || c == '_' || c == '.' || c == '-';
}

/// Compare two display names the way this lobby actually produces them.
///
/// The two strings this feature has to reconcile are not the same by construction, and comparing
/// them byte for byte is a bug rather than a simplification:
///
///   * the relay-held NICKNAME is filtered to [A-Za-z0-9_.-] by Transport::sendRelayNickname,
///     because the roster reply is space-separated with '|' between the fields and a stray space
///     would corrupt it for every client in the room;
///   * the name ADVERTISED with a game is the host's LAN slot-list name, which LANAPI passes to
///     setGameAdvertisement unfiltered.
///
/// GXApplyNickname sets both from ONE thing the player typed. So "Big Bob" reaches us as "BigBob"
/// in the roster and as "Big Bob" in a listing, and a friend stored under either spelling would
/// show up in one of the two places and be invisible in the other.
///
/// Both sides are therefore folded to the relay's charset and compared without case. A name that
/// folds away to nothing matches nobody - otherwise a friend recorded as "!!!" would match every
/// player whose name is also all punctuation, including the next one to arrive.
static Bool relayNamesMatch( const char *a, const char *b )
{
	if (a == nullptr || b == nullptr)
		return FALSE;

	Bool sawOne = FALSE;
	for (;;)
	{
		while (*a != '\0' && !relayNameCharIsSignificant(*a))
			++a;
		while (*b != '\0' && !relayNameCharIsSignificant(*b))
			++b;

		if (*a == '\0' || *b == '\0')
			return (*a == *b && sawOne) ? TRUE : FALSE;

		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return FALSE;

		sawOne = TRUE;
		++a;
		++b;
	}
}

/// The relay-held nickname for a room slot, "" when that slot has not claimed one or is not in the
/// roster at all. outPresent distinguishes those two, which matters: "absent" must not be read as
/// "renamed" by the ignore bookkeeping, or a roster that has not come back yet would clear entries.
static const char *relayRosterNickForSlot( UnsignedInt slot, Bool *outPresent = nullptr )
{
	if (outPresent != nullptr)
		*outPresent = FALSE;
	if (TheLAN == nullptr)
		return "";

	UnsignedInt ips[GX_ROOM_SLOTS];
	const Int n = gxLobbyRoster(ips, GX_ROOM_SLOTS);
	for (Int i = 0; i < n; ++i)
	{
		if (ips[i] != slot)
			continue;
		if (outPresent != nullptr)
			*outPresent = TRUE;
		const char *nick = gxLobbyRosterNick(i);
		return (nick != nullptr) ? nick : "";
	}
	return "";
}

/// GeneralsX @feature Claude 11/08/2026 The role code the relay published for a room slot: "m", "a"
/// or "". Mirrors relayRosterNickForSlot exactly, because the two are read from the same roster and
/// a second lookup shape would be a second place for the slot arithmetic to go wrong.
///
/// PRESENTATION ONLY. The relay decided this from a signed assertion and enforces every rule that
/// binds; what this returns decides a colour and a pair of brackets.
static const char *relayRosterRoleForSlot( UnsignedInt slot )
{
	if (TheLAN == nullptr)
		return "";

	UnsignedInt ips[GX_ROOM_SLOTS];
	const Int n = gxLobbyRoster(ips, GX_ROOM_SLOTS);
	for (Int i = 0; i < n; ++i)
	{
		if (ips[i] != slot)
			continue;
		const char *role = gxLobbyRosterRole(i);
		return (role != nullptr) ? role : "";
	}
	return "";
}

/// GeneralsX @feature Claude 08/08/2026 The durable id bound to a room slot, "" when we have not
/// exchanged a friend card with whoever is in it.
///
/// This is also where a binding EXPIRES, checked lazily for the same reason the ignore bookkeeping
/// is: there is no roster callback to hang it on, and this runs on the paths where the answer is
/// actually needed. A slot that has changed hands - or been renamed, which is indistinguishable
/// from the outside - loses its binding immediately. Attributing one person's identity to whoever
/// inherited their slot is the single worst thing this feature could do, so the test is biased
/// hard towards forgetting.
///
/// "Absent from the roster" is NOT "renamed": a roster request that has not come back yet must not
/// wipe every binding on the screen. Same distinction relaySlotIsIgnored already draws.
static const char *relaySlotDurableId( UnsignedInt slot )
{
	if (slot == 0 || slot > (UnsignedInt)GX_ROOM_SLOTS)
		return "";
	if (s_slotId[slot][0] == '\0')
		return "";

	Bool present = FALSE;
	const char *now = relayRosterNickForSlot(slot, &present);
	if (present && strcmp(now, s_slotIdNick[slot]) != 0)
	{
		s_slotId[slot][0] = '\0';
		s_slotIdNick[slot][0] = '\0';
		// The card handshake state belongs to the person who left with the slot, not to whoever
		// arrived in it, or an inherited "they asked" would let a stranger be accepted by mistake.
		s_cardTheyAsked[slot] = FALSE;
		s_cardWeAsked[slot] = FALSE;
		s_cardWeAskedMs[slot] = 0;
		s_cardWeAccepted[slot] = FALSE;
		s_cardNick[slot][0] = '\0';
		s_cardOurNonce[slot][0] = '\0';
		s_cardTheirNonce[slot][0] = '\0';
		s_cardTheyAskedMs[slot] = 0;
		return "";
	}
	return s_slotId[slot];
}

/// Latch the name a slot holds now, so pending card state can be expired when it changes hands.
static void relayCardLatch( UnsignedInt slot )
{
	if (slot == 0 || slot > (UnsignedInt)GX_ROOM_SLOTS)
		return;
	strlcpy(s_cardNick[slot], relayRosterNickForSlot(slot), sizeof(s_cardNick[slot]));
}

/// TRUE while the slot is still held by whoever the pending card state was recorded against.
/// Clears all three flags and answers FALSE otherwise, so a stale exchange cannot be completed by
/// the next occupant. See s_cardNick for why this is a security check and not tidiness.
static Bool relayCardPeerUnchanged( UnsignedInt slot )
{
	if (slot == 0 || slot > (UnsignedInt)GX_ROOM_SLOTS)
		return FALSE;

	Bool present = FALSE;
	const char *now = relayRosterNickForSlot(slot, &present);
	// "Not in the roster yet" is not "somebody else" - a roster request that has not come back must
	// not cancel an exchange in progress. Only a slot that IS present under a DIFFERENT name does.
	if (present && strcmp(now, s_cardNick[slot]) != 0)
	{
		s_cardTheyAsked[slot] = FALSE;
		s_cardWeAsked[slot] = FALSE;
		s_cardWeAskedMs[slot] = 0;
		s_cardWeAccepted[slot] = FALSE;
		s_cardNick[slot][0] = '\0';
		s_cardOurNonce[slot][0] = '\0';
		s_cardTheirNonce[slot][0] = '\0';
		s_cardTheyAskedMs[slot] = 0;
		return FALSE;
	}
	return TRUE;
}

/// Record the id a slot just disclosed, latching the name it holds right now so the binding expires
/// by itself when the slot changes hands.
static void relayBindSlotId( UnsignedInt slot, const char *id )
{
	if (slot == 0 || slot > (UnsignedInt)GX_ROOM_SLOTS)
		return;
	if (!GXPlayerIdentity::isValidPlayerId(id))
		return;
	strlcpy(s_slotId[slot], id, sizeof(s_slotId[slot]));
	strlcpy(s_slotIdNick[slot], relayRosterNickForSlot(slot), sizeof(s_slotIdNick[slot]));
}

/// TRUE if chat from this room slot should be dropped. Also the point where a stale SESSION entry
/// expires: checked lazily rather than on a roster callback because there is no roster callback to
/// hang it on, and this runs on the drain path where the answer is actually needed.
///
/// GeneralsX @feature Claude 08/08/2026 Two ignore scopes, checked in this order:
///
///   1. the DURABLE, id-keyed list in GXPlayerIdentity, which is only reachable for a slot we have
///      exchanged a friend card with. It survives restarts and follows the person across rooms and
///      across slot reassignment, because it is keyed on something that is actually theirs;
///   2. the SESSION, slot-keyed entry below, which is the honest answer for a stranger and expires
///      the moment the slot changes hands.
///
/// The first is not a strictly better version of the second and must not replace it. A durable
/// ignore keyed on anything a stranger can claim - a nickname, a slot - silently mutes an innocent
/// arrival tomorrow, and a silent mute is invisible to the person it wrongs.
static Bool relaySlotIsIgnored( UnsignedInt slot )
{
	if (slot == 0 || slot > (UnsignedInt)GX_ROOM_SLOTS)
		return FALSE;

	const char *id = relaySlotDurableId(slot);
	if (id[0] != '\0' && GXPlayerIdentity::isIgnored(id))
		return TRUE;

	if (!s_ignoreSlot[slot])
		return FALSE;

	Bool present = FALSE;
	const char *now = relayRosterNickForSlot(slot, &present);
	if (present && strcmp(now, s_ignoreNick[slot]) != 0)
	{
		// Somebody else holds this slot now, or the same person renamed. Either way the entry no
		// longer names the person it was aimed at, so it goes rather than muting a stranger.
		s_ignoreSlot[slot] = FALSE;
		s_ignoreNick[slot][0] = '\0';
		return FALSE;
	}
	return TRUE;
}

/// Resolve a chat argument to a room slot: "P3", "3", or a nickname currently in the roster.
/// 0 when nothing matches.
static UnsignedInt relayResolveRoomSlot( const AsciiString &arg )
{
	if (arg.isEmpty() || TheLAN == nullptr)
		return 0;

	const char *s = arg.str();
	if ((s[0] == 'P' || s[0] == 'p') && s[1] != '\0')
		++s;

	Bool allDigits = (*s != '\0');
	for (const char *c = s; *c != '\0'; ++c)
	{
		if (*c < '0' || *c > '9')
		{
			allDigits = FALSE;
			break;
		}
	}
	if (allDigits)
	{
		const UnsignedInt slot = (UnsignedInt)atoi(s);
		return (slot >= 1 && slot <= (UnsignedInt)GX_ROOM_SLOTS) ? slot : 0;
	}

	UnsignedInt ips[GX_ROOM_SLOTS];
	const Int n = gxLobbyRoster(ips, GX_ROOM_SLOTS);
	for (Int i = 0; i < n; ++i)
	{
		const char *nick = gxLobbyRosterNick(i);
		if (nick != nullptr && nick[0] != '\0' && relayNamesMatch(nick, arg.str()))
			// GeneralsX @security Claude 06/11/2026 scale32: the relay admits ids up to 10.42.0.254, so a roster entry can exceed
			// GX_ROOM_SLOTS; every caller indexes a [GX_ROOM_SLOTS + 1] table (s_ignoreSlot...) with this - bound it like the digits arm.
			return (ips[i] >= 1 && ips[i] <= (UnsignedInt)GX_ROOM_SLOTS) ? ips[i] : 0;
	}
	return 0;
}

/// Index into the friends list of the friend whose cached nickname is `name`, or -1.
/// No side effects - this is the predicate the player list paints with, and it runs per row.
static Int relayFriendIndexByNick( const char *name )
{
	if (name == nullptr || name[0] == '\0')
		return -1;

	const Int n = GXPlayerIdentity::getFriendCount();
	for (Int i = 0; i < n; ++i)
	{
		const char *nick = GXPlayerIdentity::getFriendNick(i);
		// A friend with no cached name cannot be matched by name, and must not match everybody.
		if (nick == nullptr || nick[0] == '\0')
			continue;
		if (relayNamesMatch(nick, name))
			return i;
	}
	return -1;
}

/// Index of the friend identified by `token`, which may be a full 26-character code or a cached
/// nickname. -1 when neither matches.
static Int relayFriendIndexByToken( const AsciiString &token )
{
	if (token.isEmpty())
		return -1;

	AsciiString upper = token;
	upper.toUpper();
	if (GXPlayerIdentity::isValidPlayerId(upper.str()))
	{
		const Int n = GXPlayerIdentity::getFriendCount();
		for (Int i = 0; i < n; ++i)
		{
			if (strcmp(GXPlayerIdentity::getFriendId(i), upper.str()) == 0)
				return i;
		}
		return -1;
	}
	return relayFriendIndexByNick(token.str());
}

/// GeneralsX @feature Claude 08/08/2026 Our own room slot, 0 when we are not on a relay.
///
/// GeneralsX @bugfix Claude 15/08/2026 Ask the SHARED ROOM which slot we are, when that is what the
/// rest of this screen is reading.
///
/// The roster and the chat queue are both keyed by the last octet of the shared-room virtual
/// address, and every other accessor that reads them already branches on gxHaveLobbyPresence():
/// gxLobbyRoster(), gxLobbyRosterNick() and gxLobbyTakeChat() all prefer TheGXLobbyPresence and
/// fall back to TheLAN. This one did not - it went straight to the GAME transport's virtual IP,
/// which is a different session with a different address, and on the lobby screen is not the
/// number the relay published for us at all.
///
/// The consequence is not a missing name, which would be obvious - it is the WRONG one. The
/// transport's octet still matches SOME roster row, so relayNickFor() returns whoever happens to
/// hold that slot, and your own echoed lines get labelled with another player's name. Four other
/// call sites read this same value as an identity test - "cannot message yourself" (:2456),
/// "cannot ignore yourself" (:2312), the friend-card action guard (:4018) and the private-line
/// drain (:4200) - so the same confusion silently refuses actions on that player and drops their
/// private traffic.
///
/// GXLobbyPresence::m_mySlot is the authoritative answer whenever presence is up: it is assigned
/// from the relay's own reply (GXLobbyPresence.cpp:488) and its header comment already says it is
/// "what the UI indexes by".
static UnsignedInt relayMySlot()
{
	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->getMySlot();
	Transport *t = relayTransport();
	return (t != nullptr) ? (t->getLocalVirtualIP() & 0xff) : 0;
}

/// GeneralsX @bugfix Claude 16/08/2026 The same question, asked of the OTHER room.
///
/// relayMySlot() above answers "which roster row is me", and once presence is up that row lives in
/// the SHARED lobby room. But a slot number arriving from Transport::takeRelayPeerLine() was
/// assigned by the relay in the hosted/joined GAME room, which hands out its own slots from its own
/// space. The two are unrelated small integers, and with GX_ROOM_SLOTS at 12 they collide often.
///
/// Comparing one against the other is not a missing check, it is a WRONG one: a game-room peer who
/// happens to hold the number of my shared-room row is treated as me, and their private line is
/// dropped by the self-test that is supposed to suppress my own echo. The failure is silent and
/// looks exactly like the other player never sent anything.
///
/// So transport-sourced slots are compared against the transport's own identity, always - never
/// against the presence slot, however authoritative that is for the roster.
static UnsignedInt relayMyTransportSlot()
{
	Transport *t = relayTransport();
	return (t != nullptr) ? (t->getLocalVirtualIP() & 0xff) : 0;
}

/// GeneralsX @feature Claude 08/08/2026 The name to put in a friend card, filtered to the relay's
/// nickname charset.
///
/// FILTERED HERE EVEN THOUGH THE RELAY ALREADY FILTERS ITS COPY. A card is a space-separated line
/// composed by this client and parsed by another one; a space in the middle of it would shift every
/// field after it, and the field after the name in "fa <id> <nick>" is nothing - but in a future
/// kind it would be. Cheaper to make the invariant true than to remember it holds today.
static AsciiString relayMyCardNick()
{
	const char *fromRoster = relayRosterNickForSlot(relayMySlot());
	AsciiString source(fromRoster);
	if (source.isEmpty() && TheLAN != nullptr)
		source.translate(TheLAN->GetMyName());

	AsciiString out;
	for (const char *p = source.str(); *p != '\0'; ++p)
	{
		if (relayNameCharIsSignificant(*p))
			out.concat(*p);
	}
	return out;
}

/// GeneralsX @feature Claude 08/08/2026 Resolve a command argument to a room slot, accepting one
/// more form than relayResolveRoomSlot: a friend's CODE or cached name, resolved through the
/// slot bindings learned this session.
///
/// The extra form matters for /msg. A player who has swapped codes thinks of that person by name,
/// and their relay nickname in this room may be something else entirely - the roster name is
/// per-room and freely changeable, the friend entry is not.
///
/// Returns 0 when nothing matches. `outWasFriendNotHere` distinguishes "no such player" from "that
/// IS your friend but they are not in this room", because those need different answers and merging
/// them produces the useless "nobody by that name" for somebody the player can see in their own
/// friends list.
static UnsignedInt relayResolveTargetSlot( const AsciiString &arg, Bool *outWasFriendNotHere = nullptr )
{
	if (outWasFriendNotHere != nullptr)
		*outWasFriendNotHere = FALSE;

	const UnsignedInt direct = relayResolveRoomSlot(arg);
	if (direct != 0)
		return direct;

	const Int idx = relayFriendIndexByToken(arg);
	if (idx < 0)
		return 0;

	const char *id = GXPlayerIdentity::getFriendId(idx);
	for (Int s = 1; s <= GX_ROOM_SLOTS; ++s)
	{
		if (strcmp(relaySlotDurableId((UnsignedInt)s), id) == 0)
			return (UnsignedInt)s;
	}

	if (outWasFriendNotHere != nullptr)
		*outWasFriendNotHere = TRUE;
	return 0;
}

/// The browser's test. Same match as relayFriendIndexByNick, plus one write-back: the host is
/// using that spelling of their name right now, so record it. noteFriendNick compares before it
/// saves, so this settles after a single rewrite and cannot touch the file on every sweep.
///
/// STILL NAME-MATCHED, and it has to be. A GXGAME listing carries the host's advertised name and
/// nothing else; there is no field for an id and no channel to learn one before joining, and GXHERE
/// answers "this id is connected to the relay" rather than "this id is hosting room X". So this is
/// a display filter, exactly as it was, and a stranger who picks a friend's name appears in a
/// filtered list. Annoying, not dangerous - nothing downstream of it grants anything.
static Bool relayHostIsFriend( const char *advertisedName )
{
	const Int idx = relayFriendIndexByNick(advertisedName);
	if (idx < 0)
		return FALSE;

	GXPlayerIdentity::noteFriendNick(GXPlayerIdentity::getFriendId(idx), advertisedName);
	return TRUE;
}

/// GeneralsX @feature Claude 08/08/2026 Is the player in this room slot a friend?
///
/// TWO ANSWERS OF DIFFERENT QUALITY, and the caller is told which one it got. `outByName` comes back
/// TRUE when the match came from the nickname fallback, so a caller that is deciding anything more
/// than a colour can refuse to act on it.
///
///   * a slot whose durable id we hold is matched on that id. Real, and it survives the player
///     renaming themselves mid-lobby.
///   * a slot whose id we do not hold falls back to matching the roster nickname against the name
///     cached beside each friend. Anyone may claim any name, so this is a display convenience.
///
/// The fallback is kept rather than dropped because dropping it would make the friend colour vanish
/// for every friend added by code out of band - which is most of them until two people happen to be
/// in one lobby together.
static Bool relaySlotIsFriend( UnsignedInt slot, Bool *outByName = nullptr )
{
	if (outByName != nullptr)
		*outByName = FALSE;

	const char *id = relaySlotDurableId(slot);
	if (id[0] != '\0')
		return GXPlayerIdentity::isFriend(id);

	if (outByName != nullptr)
		*outByName = TRUE;
	return (relayFriendIndexByNick(relayRosterNickForSlot(slot)) >= 0) ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/08/2026 The unicast peer line: private messages and friend cards.
//
// WHY THIS IS NOT A RELAY CONTROL TAG. Read this before adding a "GXDM" to relay.js, because that
// is the obvious design and it is wrong twice over.
//
//   * On a relay that KNOWS the tag, every text primitive it has is a broadcast. GXCHT goes to
//     every member of the room by construction, and a private message would need per-recipient
//     relay state that does not exist. "Private" would have to be enforced by the recipients
//     agreeing not to display it, which is not privacy, it is etiquette.
//   * On a relay that does NOT know the tag, the datagram falls off the end of the dispatch chain
//     into forward(). Whether that DROPS it or BROADCASTS IT TO ALL SEVEN PEERS depends on which
//     build the operator has installed - the guard that drops it was added on 07/08/2026. So a
//     private message would be private or public depending on a relay the sender cannot inspect.
//     Measured 08/08/2026 against the deployed /opt/gxrelay/relay.js: that guard IS present, and
//     GXKEY/GXSEEN/GXHERE are NOT. Deployed relays are not all the same relay, and the ones that
//     matter are the ones you cannot check.
//
// A relay-header unicast has neither problem. It is an ordinary relayed datagram - the 12-byte GXR1
// header with a single destination - and forward() delivers it to that one member without ever
// looking at the payload. Same path the join handshake and the map transfer already use. It works
// against every relay build there has ever been, and it cannot be turned into a broadcast by
// deploying the wrong version of anything.
//
// Transport owns exactly one primitive - "a bounded line to that slot" - and knows nothing about
// what is in it. Every message KIND is parsed here, outside the digest, so adding one costs no
// four-platform rebuild.
//
//     dm <text>              a private message
//     fq <nick>              friend request. NO id: this is the asking half
//     fa <id> <nick>         accept, disclosing the accepter's id
//     fi <id> <nick>         the requester's id, only ever sent in reply to an "fa" we solicited
//
// A kind this build does not know is IGNORED SILENTLY rather than displayed. Displaying it would
// make the chat box a place a peer can write arbitrary text into by inventing a verb.
// GeneralsX @bugfix Claude 16/08/2026 SEND ON THE SOCKET THAT SHARES THE ROOM WITH THE ROSTER.
//
// Every caller of this function resolves its slot out of the roster - relayResolveTargetSlot(), a
// friend card's row, the slot a line arrived from. When presence is up that roster is the SHARED
// room, and this function sent into the GAME room regardless. The two rooms are separate /24s on the
// relay and it routes on the last octet, so the number meant "P3 in the lobby" and arrived meaning
// "whoever is third in your game room".
//
// That is not a message that fails to send. It is a message DELIVERED TO THE WRONG PLAYER, and the
// payloads are private messages and friend cards carrying a durable identity id. Where the game room
// had no such slot it vanished silently instead, which is why the feature read as "unreliable"
// rather than as broken.
//
// The fix is the same branch every other accessor on this screen already makes (gxLobbyRoster,
// gxLobbyRosterNick, relayMySlot ...): ask the carrier that shares a room with the roster the slot
// was read from. One channel is live at a time and the choice is a pure function of
// gxHaveLobbyPresence(), which is what lets the reply inside relayDrainPeerLines go back the way it
// came without threading a channel through the parser.
static Bool relaySendPeerLine( UnsignedInt slot, const char *body )
{
	if (body == nullptr)
		return FALSE;

	if (gxHaveLobbyPresence())
		return TheGXLobbyPresence->sendPeerLine(slot, body);

	Transport *transport = relayTransport();
	if (transport == nullptr)
		return FALSE;
	return transport->sendRelayPeerLine(slot, body);
}

/// One line into the lobby chat box. Every message this feature produces is local - none of it is
/// ever sent to the relay, which is the whole point of doing the filtering on the receive side.
static void relaySocialSay( const UnicodeString &line, Color color )
{
	// GeneralsX @feature Claude 10/08/2026 Give this a sink when there is no chat listbox.
	//
	// This is the SINGLE output chokepoint for everything the relay/friends feature prints, and it
	// used to silently DROP every line when the listbox was absent - which is every headless run.
	// That is the reason the lobby has never been testable without a GUI, and why the 2026-08-10
	// friends bug ("entering the lobby screen wipes pending requests") had to be found by driving a
	// Vulkan window blind.
	//
	// stderr is the house pattern for output that must survive a release build (INI.cpp:196-197,
	// SimulationId.cpp:663). The [GXLOBBY] prefix is what a harness greps for.
	//
	// Note this deliberately does NOT require TheGameSpyInfo: that object only exists to own the
	// listbox's text buffer, so a headless peer having none is not a reason to lose the line.
	// GeneralsX @feature Claude 11/08/2026 "[S]HH:MM " - this line came from YOUR client and
	// nobody else can see it. Applied HERE, at the single sink every client-originated line already
	// goes through, so no caller can forget it and no two callers can format it differently.
	//
	// The headless branch below is left BARE on purpose: "[GXLOBBY]" is what a harness greps for,
	// and a timestamp in front of it would make every existing grep pattern miss.
	if (TheGameSpyInfo == nullptr || listboxLobbyChat == nullptr)
	{
		AsciiString plain;
		plain.translate(line);
		fprintf(stderr, "[GXLOBBY] %s\n", plain.str());
		fflush(stderr);
		return;
	}

	// GeneralsX @feature Claude 13/08/2026 Composed AFTER the headless bail, not before it. The
	// prefix is only printed once per run of like lines, so GXLobbyChatRow has to be called exactly
	// as often as a row is really inserted - composing above the early return would count a row that
	// never existed and swallow the prefix off the next one that did.
	const UnicodeString shown = GXLobbyChatRow(listboxLobbyChat, GXLobbySystemPrefix(), line);
	TheGameSpyInfo->addText(shown, color, listboxLobbyChat);
}

// ---------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/08/2026 GXKEY and GXSEEN/GXHERE - the client half of the protocol
// tools/relay/relay.js has spoken since 08/08/2026 and nothing had ever spoken back.
//
// WHAT GXKEY BUYS AND WHAT IT DOES NOT. It tells the relay our durable id so that the relay can
// answer somebody ELSE'S "is this id connected". It does NOT put our id in the roster, and there is
// no version of this that does - see the design block at the top of this section. Presence is the
// entire product of declaring an id, and it is worth having: it is the difference between a friends
// list that is a text file and one that tells you whether to bother waiting.
//
// DEGRADES TO NOTHING, NEVER TO A LEAK, on a relay that does not know the tags. Measured 08/08/2026
// against the deployed /opt/gxrelay/relay.js: it has no GXKEY, GXSEEN or GXHERE, and it DOES have
// forward()'s unknown-control-tag guard - so both requests are dropped and counted, one log line per
// rate-limit window. The friends panel then reports presence as unknown rather than as offline,
// which is the honest answer and is why relayPresenceFor has three return values instead of two.

/// Declare our durable id to the relay. Cheap and idempotent: Transport remembers it and re-sends
/// it on its own 30 s tag tick, so calling this on every entry to the lobby costs one datagram.
static void relayDeclareIdentity()
{
	// GeneralsX @bugfix Claude 18/09/2026 THE LOBBY PRESENCE SOCKET TOO, and before anything that can
	// return early.
	//
	// The shared-lobby socket is a SECOND member on the relay - its own endpoint, its own member
	// record - and it was never given the identity, so its `sub` was permanently null. Every sanction
	// the relay applies keys on `sub` (enforceModeration returns at once when it is null), so a muted
	// or banned account was silenced on its game socket and went on talking in the shared lobby: the
	// bypass docs/WORKDIR/SHARED_LOBBY_ROOM_DESIGN.md:358-363 predicted before either socket existed.
	//
	// ABOVE THE relayTransport() GUARD DELIBERATELY. The presence socket is orthogonal to the game
	// transport by design, and a transport that is not relaying must not be able to suppress the
	// identity on a socket that is up and in a room.
	//
	// The same value the game socket is handed below, passed as the pointer it already is - an
	// AsciiString temporary here would be a heap copy of the token, freed with the bytes still in it.
	// "" when nobody is signed in, which setIdentity treats as the clear, so this stays a no-op for a
	// LAN player, an offline player and anyone without an account.
	if (TheGXLobbyPresence != NULL)
		TheGXLobbyPresence->setIdentity(GXPlayerIdentity::getVerifiedAssertionText());

	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	const char *id = GXPlayerIdentity::getPlayerId();
	if (id == nullptr || id[0] == '\0')
		return;			// the CSPRNG refused at startup; GXPlayerIdentity has already said so loudly

	transport->sendRelayKey(id);

	// GeneralsX @feature Claude 11/08/2026 And the SIGNED identity, beside the self-minted one.
	//
	// The two are not alternatives. The durable id is a client-minted bearer token and is what
	// friendship and presence are keyed on; the assertion is what a relay can actually verify, and
	// is what a ban keys on. Sending both here means one call site declares everything this peer
	// claims to be, rather than two that can drift.
	//
	// "" when nobody is logged in, which sendRelayIdentity treats as "forget it" - so this is a
	// no-op for a LAN player, an offline player and anyone without an account, forever.
	transport->sendRelayIdentity(GXPlayerIdentity::getVerifiedAssertionText());
}

/// GeneralsX @bugfix Claude 11/08/2026 Re-declare when the identity CHANGES while the lobby is open.
///
/// Found by cross-model review. relayDeclareIdentity ran once, on screen entry, so a /login that
/// completed while the player was sitting in the lobby never reached the relay: GXLoginFlow refreshes
/// the local cache, but nothing re-sent it, and Transport's own 30 s resend is a no-op while
/// m_relayIdentity is still empty. With RELAY_REQUIRE_IDENTITY on, the relay went on dropping that
/// player every grace period - and the only cure was to leave the screen and come back, which is
/// exactly the "bounce the screen" folklore this project keeps trying to delete.
///
/// The same path covers /logout, which now has to TELL the relay rather than just forgetting locally.
///
/// Compared by VALUE rather than by a "did a login happen" flag: the flag is a second thing to keep
/// in sync, and the value is what actually has to match. An assertion is at most 256 bytes and this
/// runs at the lobby's refresh cadence, so the compare is free.
// GeneralsX @bugfix Claude 18/09/2026 A FINGERPRINT, NOT A COPY. This used to keep the last
// declared assertion itself in s_lastDeclared - a second copy of a bearer token, living as long as
// the process and outliving every sign-out, because only a later lobby tick ever overwrote it (a
// /logout from the LAN lobby never reached one). Found by cross-model review. The comparison only
// needs to know whether the value CHANGED, and a 64-bit FNV-1a of at most GX_ASSERTION_MAX bytes
// answers that without holding the token: a collision would cost one skipped re-declare, at odds
// of about 2^-64 per change.
static unsigned long long gxAssertionFingerprint( const char *s )
{
	unsigned long long h = 1469598103934665603ULL;		// FNV-1a 64 offset basis
	for (int i = 0; s[i] != '\0' && i < GX_ASSERTION_MAX; ++i)
	{
		h ^= (unsigned char)s[i];
		h *= 1099511628211ULL;							// FNV-1a 64 prime
	}
	return h;
}

static void relayRedeclareIdentityIfChanged()
{
	static unsigned long long s_lastDeclared = gxAssertionFingerprint("");

	const char *now = GXPlayerIdentity::getVerifiedAssertionText();
	if (now == NULL)
		now = "";
	const unsigned long long fp = gxAssertionFingerprint(now);
	if (fp == s_lastDeclared)
		return;

	s_lastDeclared = fp;

	// GeneralsX @bugfix Claude 18/09/2026 The lobby presence socket follows every change too, and
	// above the transport guard for the reason relayDeclareIdentity gives. A /login that completes
	// while the player sits in the lobby has to reach BOTH sockets, or the shared room goes on
	// treating them as nobody; a /logout has to clear BOTH, or the relay keeps applying that account's
	// role and sanctions to the presence member. (A sign-out also clears it directly, in
	// GXLobbySignOut - this is the change detector catching up, and a repeat clear is a no-op.)
	if (TheGXLobbyPresence != NULL)
		TheGXLobbyPresence->setIdentity(now);

	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;
	// "" is the clear, and sendRelayIdentity turns it into the wire clear rather than silence.
	transport->sendRelayIdentity(now);
}

/// Ask which friends are connected. At most 16 per datagram, which is the relay's MAX_SEEN_IDS.
///
/// PAGINATION IS NOT IMPLEMENTED AND THAT IS A STATED LIMITATION, not an oversight: with more than
/// 16 friends the first 16 get presence and the rest read as unknown. The alternative - rotating
/// which page is asked for - would make the panel flicker between "online" and "unknown" for the
/// same person, which is worse than a stable "I do not know".
static void relayPollPresence()
{
	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	// A second question while the first is unanswered POISONS both, in Transport, by design - GXHERE
	// carries no request id and a mispaired answer reports a stranger's presence against a friend.
	// So do not fire one off the back of a /friend the player typed twice in a row: that would cost
	// a whole poll interval of "checking..." for no reason. The background tick is 30 s; this only
	// has to be short enough not to feel like the panel is ignoring the command.
	static const UnsignedInt PRESENCE_MIN_GAP_MS = 3000;
	const UnsignedInt now = timeGetTime();
	if (s_presenceLastPollMs != 0 && (now - s_presenceLastPollMs) < PRESENCE_MIN_GAP_MS)
		return;

	// Built into a LOCAL snapshot and copied in only on success. Writing straight into
	// s_presenceAsked would break the pairing on the failure path: the ids would have been replaced
	// while s_presenceMask and s_presenceKnownCount still described the PREVIOUS question, and
	// relayPresenceFor would then report one friend's presence against another's name.
	const Int have = GXPlayerIdentity::getFriendCount();
	Int n = 0;
	char snapshot[16][GX_LOBBY_ID_MAX];
	const char *ids[16];
	for (Int i = 0; i < have && n < 16; ++i)
	{
		const char *id = GXPlayerIdentity::getFriendId(i);
		if (!GXPlayerIdentity::isValidPlayerId(id))
			continue;
		strlcpy(snapshot[n], id, sizeof(snapshot[n]));
		ids[n] = snapshot[n];
		++n;
	}

	if (n == 0)
	{
		s_presenceAskedCount = 0;
		s_presenceKnownCount = 0;
		return;
	}

	if (!transport->requestRelayPresence(ids, n))
		return;

	// Is this the SAME question as last time? Almost always yes - the poll runs every 30 s and a
	// friends list rarely changes - and the answer to an identical question stays valid while the
	// new one is in flight. Without this test the panel would drop back to "checking..." for one
	// round trip on every tick, which reads as the feature being broken rather than as it working.
	//
	// Compared rather than assumed, because when the list DOES change every bit after the change is
	// off by one, and a friend silently reported with somebody else's presence is the exact class of
	// wrong answer this whole feature is trying to stop being.
	Bool sameQuestion = (n == s_presenceAskedCount);
	for (Int i = 0; sameQuestion && i < n; ++i)
	{
		if (strcmp(s_presenceAsked[i], snapshot[i]) != 0)
			sameQuestion = FALSE;
	}

	for (Int i = 0; i < n; ++i)
		strlcpy(s_presenceAsked[i], snapshot[i], sizeof(s_presenceAsked[i]));
	s_presenceAskedCount = n;
	s_presenceLastPollMs = now;

	if (!sameQuestion)
		s_presenceKnownCount = 0;
}

/// Presence of one id: 1 online, 0 offline, -1 not known.
///
/// THREE VALUES, DELIBERATELY. Folding "I have not been told" into "offline" is how a relay that
/// does not speak GXSEEN turns into a friends list that confidently reports everybody as away -
/// and the deployed relay is exactly that relay today.
static Int relayPresenceFor( const char *id )
{
	if (s_presenceKnownCount <= 0 || id == nullptr || id[0] == '\0')
		return -1;
	// GeneralsX @bugfix Claude 25/09/2026 An answer nobody has renewed is not an answer (GXFriendsKey.h).
	if (!GXPresenceAnswerCurrent(timeGetTime(), s_presenceAnsweredMs))
		return -1;
	for (Int i = 0; i < s_presenceKnownCount && i < 16; ++i)
	{
		if (strcmp(s_presenceAsked[i], id) == 0)
			return ((s_presenceMask >> i) & 1u) ? 1 : 0;
	}
	return -1;
}

// GeneralsX @feature Claude 11/08/2026 The buddy overlay's window into the SAME answer. A thin
// exported wrapper rather than promoting the static: takeRelayPresence is a destructive read
// with exactly one permitted consumer (relayUpdatePresence, above), and every other reader must
// come through this lobby-side decode or the two would race and each see half the answers.
Int GXRelayPresenceFor( const char *id )
{
	return relayPresenceFor(id);
}

static void relayRebuildStagingRooms()
{
	Transport *transport = relayTransport();
	if (transport == nullptr || TheGameSpyInfo == nullptr || TheLAN == nullptr)
		return;

	// GeneralsX @bugfix Claude 07/08/2026 The transient-empty guard that used to live here is gone,
	// because the cause is now fixed at source.
	//
	// It debounced three consecutive empty sweeps before believing the browser was empty, which
	// worked but was the wrong place: Transport::requestGameList() was zeroing the list callers
	// read, so the emptiness was an artefact of the transport's own refresh and every reader would
	// have needed the same guard. The transport now double-buffers the sweep and only publishes it
	// once it has something to say, so getGameListCount() no longer reports a lobby as empty while
	// its answer is still in flight. See the note on getGameListCount() in Transport.h.
	//
	// Debouncing on top of that would only delay a lobby that has genuinely emptied.

	// IDs must survive a rebuild or the selection moves under the player's cursor:
	// RefreshGameListBox restores the selection by ID, and LobbyUtils re-sorts every time.
	RelayListedGame previous[Transport::MAX_RELAY_LISTINGS];
	const Int previousCount = s_relayGameCount;
	for (Int i = 0; i < previousCount; ++i)
		previous[i] = s_relayGames[i];
	s_relayGameCount = 0;

	TheGameSpyInfo->clearStagingRoomList();

	// addStagingRoom only marks the list dirty once the caller has said the list is complete, and
	// hasStagingRoomListChanged() is what refreshGameList() gates the redraw on. One GXLIST sweep
	// IS the complete list, so say so before adding anything.
	TheGameSpyInfo->sawFullGameList();

	const Int count = transport->getGameListCount();
	for (Int g = 0; g < count; ++g)
	{
		const Transport::RelayGameListing *listing = transport->getGameListing(g);
		if (listing == nullptr || listing->hostVirtualIP == 0)
			continue;

		// Do not offer our own game - browsing it would be dialling ourselves. The ROOM is half of
		// that test and not an optimisation: addresses are allocated per room, so every host in the
		// browser is 10.42.0.1 and so are we. Matching on the address alone hides every game.
		if (listing->hostVirtualIP == TheLAN->GetLocalIP()
			&& strcmp(listing->room, transport->getRelayRoom()) == 0)
			continue;

		// GeneralsX @feature Claude 08/08/2026 Friends-only filter.
		//
		// Applied HERE rather than when drawing, so a hidden game is absent from s_relayGames as
		// well as from the listbox. That matters: relayJoinSelectedGame resolves the selected row
		// back through relayFindGameByID, and a row filtered out of the display but left in the
		// table would still be dialable from a stale selection.
		//
		// Matched on the advertised name, which is unauthenticated - see the block above. A
		// filtered browser is a convenience, never a safety property.
		if (s_relayFriendsOnly && !relayHostIsFriend(listing->name))
			continue;

		if (s_relayGameCount >= Transport::MAX_RELAY_LISTINGS)
			break;

		RelayListedGame &entry = s_relayGames[s_relayGameCount];
		entry.id = 0;
		for (Int p = 0; p < previousCount; ++p)
		{
			if (previous[p].hostVirtualIP == listing->hostVirtualIP
				&& strcmp(previous[p].room, listing->room) == 0)
			{
				entry.id = previous[p].id;
				break;
			}
		}
		if (entry.id == 0)
			entry.id = s_relayNextGameID++;
		strlcpy(entry.room, listing->room, sizeof(entry.room));
		entry.hostVirtualIP = listing->hostVirtualIP;
		entry.players = listing->players;
		entry.slots = listing->slots;
		++s_relayGameCount;

		// GeneralsX @feature Claude 08/08/2026 Prefix the row with the relay's REGION, the way
		// Generals Online labels its list ("[EU] FFA 10k NO RULES").
		//
		// Composed HERE and not on the wire. The region is a property of the relay we asked, not of
		// each game - every row in this list came from the same GXLIST reply - so putting it in the
		// advertisement would spend bytes on every row against a length-capped datagram to say the
		// same thing N times. Transport::getRelayRegion() is the single runtime source; it is never
		// a compiled-in constant, because "adding a relay must not require updating clients".
		//
		// GameClient/ is not digested, so the FORMAT of this row can be changed for one platform
		// without a lockstep-wide rebuild. Keep it that way: put presentation here, not in LANAPI.
		//
		// GeneralsX @feature Claude 17/08/2026 The lobby's own NAME leads the row, with the host in
		// parentheses after it: "[EU] FFA 10k NO RULES (Bobtista)". That is the whole point of
		// carrying it - a browser full of player names says nothing about what the games ARE.
		//
		// THE UNNAMED CASE IS THE COMMON ONE AND IT GETS ITS OWN BRANCH, not an empty substitution.
		// Most games have no lobby name at all: every host who never typed one, and every game seen
		// through a relay that predates the field. Formatting those with the same string would
		// render "[EU]  (Bobtista)" - a double space and a parenthesised name with nothing to be
		// parenthetical to. They keep exactly today's row instead.
		//
		// listing->lobbyName was sanitised on the way in (parseRelayGameLine) as well as by the host
		// that typed it and the relay that stored it, so it is safe to place in a widget here. The
		// filter above still matches listing->name, the HOST's name, and must keep doing so - it is
		// the field a friend is recognised by, and this row format is presentation only.
		UnicodeString hostName;
		if (listing->lobbyName[0] != '\0')
		{
			hostName.format(L"[%hs] %hs (%hs)", transport->getRelayRegion(),
				listing->lobbyName, listing->name);
		}
		else
		{
			hostName.format(L"[%hs] %hs", transport->getRelayRegion(), listing->name);
		}

		GameSpyStagingRoom room;
		room.init();					// resets every slot; must run before the slots are filled in
		room.setID(entry.id);
		room.setGameName(hostName);
		room.setMap(relayResolveAdvertisedMap(listing->map));

		// The relay's reply carries no exe/ini CRC, so the version check this screen would normally
		// do CANNOT be performed here. Reporting our own CRCs is the honest encoding of "unknown":
		// the alternative, zero, paints every row with the mismatch colour and refuses every join,
		// which claims a version conflict we have not detected. A genuine mismatch still fails, it
		// just fails later - the lobby's own slot-list exchange is where it surfaces.
		room.setExeCRC(TheGlobalData->m_exeCRC);
		room.setIniCRC(TheGlobalData->m_iniCRC);

		room.setHasPassword(FALSE);
		room.setAllowObservers(FALSE);
		room.setLadderIP(AsciiString::TheEmptyString);
		room.setLadderPort(0);

		// GeneralsX @feature Claude 07/08/2026 End-to-end latency, both halves measured.
		//
		// Everything in a relayed game travels browser -> relay -> host, so the round trip is our
		// leg plus the host's. We time ours from the GXROOM/GXMEMB pair; the host reports its own
		// with GXPING and the relay hands it to us with the listing. Neither half is modelled.
		//
		// Both halves are required. Showing our leg alone would put an identical number on every
		// row - it describes OUR connection, not the distance to that particular game - and showing
		// the host's alone would flatter a host that is next to the relay while we are not. When
		// either is missing the cell stays blank: an EMPTY ping string is what insertGame reads as
		// "no data", and that is the honest rendering of a measurement we do not have. It is also
		// what an un-updated relay produces, since it never appends the host's field.
		// Both halves are stored, not just the sum. The tooltip prints the breakdown, and deriving
		// the host's leg there as (total - ourLegRightNow) would mix a live reading into a total
		// frozen at this moment - the remainder would be a residue of two instants, not a
		// measurement.
		const UnsignedInt myLegMs = (TheLAN != nullptr) ? TheLAN->getRelayRttMs() : 0;
		if (myLegMs > 0 && listing->pingMs > 0)
			room.setPingMs((Int)myLegMs + listing->pingMs, listing->pingMs);
		else
			room.setPingMs(0, 0);

		room.setReportedNumPlayers(listing->players);
		room.setReportedMaxPlayers(listing->slots);
		room.setReportedNumObservers(0);

		// Slot occupancy. The relay advertises a COUNT and the host's name, and nothing else - it
		// deliberately does not track who is in a room. So slot 0 is the host, named, and the rest
		// of the occupied slots are real (the count comes from the host's own slot list) but
		// anonymous. Numbering them rather than inventing names keeps that visible.
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			GameSpyGameSlot *slot = room.getGameSpySlot(s);
			if (slot == nullptr)
				continue;

			if (s < listing->players)
			{
				UnicodeString slotName;
				if (s == 0)
					slotName = hostName;
				else
					slotName = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT(
						"GUI:RelayLobbyOccupiedSlot", L"Player %d", s + 1);
				slot->setState(SLOT_PLAYER, slotName, (s == 0) ? listing->hostVirtualIP : 0);
			}
			else if (s < listing->slots)
			{
				slot->setState(SLOT_OPEN);
			}
			else
			{
				slot->setState(SLOT_CLOSED);
			}
			slot->setPingString(AsciiString::TheEmptyString);
		}

		TheGameSpyInfo->addStagingRoom(room);
	}
}

/// The ID of the row the player has selected, or 0.
static Int relaySelectedGameID()
{
	GameWindow *listbox = GetGameListBox();
	if (listbox == nullptr)
		return 0;

	Int selected = -1;
	GadgetListBoxGetSelected(listbox, &selected);
	if (selected < 0)
		return 0;

	// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
	return static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(listbox, selected)));
}

/// The player list, filled from the SELECTED game's slots.
///
/// GameSpy filled this from chat-room presence: everyone in the lobby, whether or not they were in
/// a game. We have no presence and the relay will not give us one - it does not track identities
/// across rooms, by design. The occupants of the game you are looking at is the honest equivalent,
/// and it is the only player data that actually exists.
static void relayRefreshPlayerList()
{
	if (TheGameSpyInfo == nullptr)
		return;

	PlayerInfoMap *players = TheGameSpyInfo->getPlayerInfoMap();
	players->clear();

	GameSpyStagingRoom *room = TheGameSpyInfo->findStagingRoomByID(relaySelectedGameID());
	if (room != nullptr)
	{
		room->cleanUpSlotPointers();
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			const GameSpyGameSlot *slot = room->getGameSpySlot(s);
			if (slot == nullptr || !slot->isHuman())
				continue;

			PlayerInfo info;
			info.m_name.translate(slot->getName());
			if (info.m_name.isEmpty())
				continue;

			// The map is keyed by the displayed name and playerTooltip looks a clicked row straight
			// back up in it without checking for a miss, so every row drawn must be a key here.
			// Everything else stays at its PlayerInfo default: profile 0, no rank, no locale, which
			// is exactly what we know about these people.
			(*players)[info.m_name] = info;
		}
	}

	PopulateLobbyPlayerListbox();
}

/// Ask the relay what games exist. The replies arrive asynchronously on the transport's own socket,
/// so nothing is waited on - relayLobbyUpdate notices the count moving and rebuilds.
static void relayRequestGameList()
{
	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	transport->requestGameList();
	s_relayLastQueryTime = timeGetTime();
	// Any query answers "has a withdrawal happened since we last asked", so recording the serial
	// here - rather than only where the forced query is decided - is what keeps one withdrawal from
	// forcing a query on every frame that follows it.
	s_relayWithdrawSerialSeen = Transport::getAdvertWithdrawSerial();

	// A re-query that returns the same NUMBER of games can still be a different set of games -
	// one host leaves as another arrives - and the count alone would call that "no change" and
	// never redraw. Clearing what we last built from forces the settled count, whatever it turns
	// out to be, to rebuild.
	s_relayCountLastFrame = -1;
	s_relayCountLastBuilt = -1;
	s_relayListLastBuilt = 0;
}

// --- friends, ignore and the browser filter: presentation --------------------------------------

/// Point the group-room dropdown at the current filter without re-entering the toggle. Setting the
/// selection programmatically can raise GCM_SELECTED, and that handler calls relaySetFriendsOnly -
/// which returns immediately when the value has not changed, so the loop closes after one step.
static void relaySyncFilterCombo()
{
	if (comboLobbyGroupRooms == nullptr)
		return;

	const Int wanted = s_relayFriendsOnly ? 1 : 0;
	const Int entries = GadgetComboBoxGetLength(comboLobbyGroupRooms);
	for (Int i = 0; i < entries; ++i)
	{
		// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
		const Int data = static_cast<Int>(reinterpret_cast<intptr_t>(
			GadgetComboBoxGetItemData(comboLobbyGroupRooms, i)));
		if (data == wanted)
		{
			GadgetComboBoxSetSelectedPos(comboLobbyGroupRooms, i, FALSE);
			return;
		}
	}
}

static void relaySetFriendsOnly( Bool on )
{
	if (s_relayFriendsOnly == (on ? TRUE : FALSE))
		return;
	s_relayFriendsOnly = on ? TRUE : FALSE;

	// Force relayLobbyUpdate to rebuild. Its gate compares the settled game count and a content
	// fingerprint against what it last built from - and the relay's answer has not changed, only
	// our view of it has, so neither of those would ever move on their own.
	s_relayCountLastBuilt = -1;
	s_relayListLastBuilt = 0;

	relaySyncFilterCombo();

	if (s_relayFriendsOnly)
	{
		UnicodeString note;
		note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsFilterOn",
			L"Showing only games hosted by one of your %d friend(s). Matched by name, which anyone "
			L"can claim - treat it as a shortcut, not a guarantee."),
			GXPlayerIdentity::getFriendCount());
		relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
	}
	else
	{
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsFilterOff",
			L"Showing all games."), GameSpyColor[GSCOLOR_MOTD]);
	}
}

/// The friends panel, printed into the chat box.
///
/// Printed rather than drawn in an overlay, and the overlay was ruled out by MEASUREMENT rather
/// than by preference. WOLBuddyOverlay.wnd ships, and it was extracted from WindowZH.big and read:
/// it declares BuddyMenuParent, ButtonHide, RadioButtonIgnore, RadioButtonBuddies, BuddiesParent,
/// ListboxBuddyChat, ListboxBuddies, TextEntryChat, IgnoreParent, ListboxIgnore and Line - and NOT
/// ButtonAdd, ButtonDelete, ButtonYes or ButtonNo, all four of which WOLBuddyOverlay.cpp:722-728
/// looks up by name. On top of that, GameSpyOpenOverlay dereferences the buddy message queue the
/// relay path deliberately never creates.
///
/// Adding the missing controls means editing a .wnd, .wnd files live inside WindowZH.big, and
/// editing shipped data moves dataID - after which every peer refuses to join with DATA_DIFFERS.
/// The chat box is a real, working, already-registered text sink and costs no shipped data at all.
///
/// The player's OWN code is NOT printed here. It is a bearer credential, and a panel opened by a
/// button would park it in the chat box for every screenshot and stream. /mycode prints it on
/// demand, once, with the warning attached.
static void relayPrintFriends()
{
	const Int n = GXPlayerIdentity::getFriendCount();

	UnicodeString header;
	header.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsHeader",
		L"Friends (%d). /help lists what you can do with them."), n);
	relaySocialSay(header, GameSpyColor[GSCOLOR_MOTD]);

	if (n == 0)
	{
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsEmpty",
			L"  Nobody yet. Ask somebody for their code - /mycode shows yours - and /add <code> "
			L"<name>. You are friends once you have each other."),
			GameSpyColor[GSCOLOR_MOTD]);
	}

	// GeneralsX @feature Claude 08/08/2026 Refresh presence while the panel is being read. The
	// answer arrives asynchronously and lands in the NEXT print - which is why the rows below say
	// "checking" rather than "offline" when nothing has come back yet.
	relayPollPresence();

	Transport *transport = relayTransport();
	const Int listed = (transport != nullptr) ? transport->getGameListCount() : 0;

	for (Int i = 0; i < n; ++i)
	{
		const char *id = GXPlayerIdentity::getFriendId(i);
		const char *nick = GXPlayerIdentity::getFriendNick(i);
		const Bool named = (nick != nullptr && nick[0] != '\0');

		// GeneralsX @feature Claude 08/08/2026 "Here in the lobby" is now an ID match wherever we
		// have exchanged a card with the slot, and only falls back to the name otherwise. That is
		// the difference between "somebody using your friend's name is in this room" and "your
		// friend is in this room", and the panel now distinguishes them.
		Bool inLobby = FALSE;
		Bool inLobbyByName = FALSE;
		UnsignedInt inLobbySlot = 0;
		for (Int s = 1; s <= GX_ROOM_SLOTS && !inLobby; ++s)
		{
			if (strcmp(relaySlotDurableId((UnsignedInt)s), id) == 0)
			{
				inLobby = TRUE;
				inLobbySlot = (UnsignedInt)s;
			}
		}
		if (!inLobby && named)
		{
			UnsignedInt ips[GX_ROOM_SLOTS];
			const Int rosterCount = gxLobbyRoster(ips, GX_ROOM_SLOTS);
			for (Int r = 0; r < rosterCount && !inLobby; ++r)
			{
				if (relayNamesMatch(gxLobbyRosterNick(r), nick))
				{
					inLobby = TRUE;
					inLobbyByName = TRUE;
					inLobbySlot = ips[r];
				}
			}
		}

		// Hosting stays a NAME match, and it is the one thing here that cannot be improved: a game
		// listing carries the host's advertised name and no identity at all. See relayHostIsFriend.
		Bool hosting = FALSE;
		if (named)
		{
			for (Int g = 0; g < listed && !hosting; ++g)
			{
				const Transport::RelayGameListing *l = transport->getGameListing(g);
				if (l != nullptr && relayNamesMatch(l->name, nick))
					hosting = TRUE;
			}
		}

		UnicodeString wideNick;
		if (named)
			wideNick.translate(AsciiString(nick));
		else
			wideNick = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendUnnamed",
				L"(no name recorded - /add their code again with a name)");

		UnicodeString where;
		if (inLobby && !inLobbyByName)
			where.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendInLobbyConfirmed",
				L"  - here in the lobby as P%d (code confirmed)"), (Int)inLobbySlot);
		else if (hosting)
			where = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendHosting",
				L"  - hosting a game in the list (matched by name)");
		else if (inLobby)
			where.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendInLobbyByName",
				L"  - someone called that is here as P%d (matched by name)"), (Int)inLobbySlot);
		else
		{
			// GeneralsX @feature Claude 08/08/2026 Presence, from GXHERE. THREE states, not two: a
			// relay that does not speak GXSEEN must not make every friend read as offline, because
			// that is indistinguishable from them actually being away and it is what the deployed
			// relay would do today.
			switch (relayPresenceFor(id))
			{
				case 1:
					where = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendOnline",
						L"  - online, somewhere else on this relay");
					break;
				case 0:
					where = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendOffline",
						L"  - offline");
					break;
				default:
					where = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendPresenceUnknown",
						L"  - checking...");
					break;
			}
		}

		UnicodeString row;
		row.format(L"  %d. %ls%ls", i + 1, wideNick.str(), where.str());
		// GeneralsX @tweak Claude 10/08/2026 Blue, matching the player list, so "a friend" is one
		// colour everywhere instead of gold here and blue three inches to the right. The two shades
		// keep the reachable/not distinction this line already made: bright for somebody you can
		// act on right now, dim for a name on the list who is not here.
		relaySocialSay(row, (hosting || inLobby) ? GameMakeColor( 105, 190, 255, 255 )
		                                         : GameMakeColor(  95, 150, 205, 255 ));
	}

	// GeneralsX @feature Claude 08/08/2026 Anyone who has asked to swap codes, so a friend request
	// is not something you have to have been looking at the chat box to notice.
	for (Int s = 1; s <= GX_ROOM_SLOTS; ++s)
	{
		if (!s_cardTheyAsked[s])
			continue;
		UnicodeString wideNick;
		wideNick.translate(AsciiString(relayRosterNickForSlot((UnsignedInt)s)));
		UnicodeString row;
		row.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendPendingRow",
			L"  P%d %ls would like to be friends   (ask them for their code, then /add it)"),
			s, wideNick.str());
		relaySocialSay(row, GameSpyColor[GSCOLOR_MOTD]);
	}

	// The ignores, so "why can I not see so-and-so" has an answer on screen. Both scopes are listed
	// and they are LABELLED differently, because "for this lobby" and "forever" are things a player
	// has to be able to tell apart before they wonder why somebody is still silent tomorrow.
	Int ignored = 0;
	for (Int s = 1; s <= GX_ROOM_SLOTS; ++s)
	{
		if (!relaySlotIsIgnored((UnsignedInt)s))
			continue;
		++ignored;

		const char *slotId = relaySlotDurableId((UnsignedInt)s);
		const Bool durable = (slotId[0] != '\0' && GXPlayerIdentity::isIgnored(slotId));

		UnicodeString wideNick;
		const char *shown = durable ? relayRosterNickForSlot((UnsignedInt)s) : s_ignoreNick[s];
		if (shown != nullptr && shown[0] != '\0')
			wideNick.translate(AsciiString(shown));

		UnicodeString row;
		row.format(durable
			? TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoredRowDurable",
				L"  Ignored permanently: P%d %ls   (/unignore P%d)")
			: TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoredRow",
				L"  Ignored in this lobby: P%d %ls   (/unignore P%d)"), s, wideNick.str(), s);
		relaySocialSay(row, GameMakeColor( 160, 160, 160, 255 ));
	}

	// The durable list can hold people who are not in this room, and they would otherwise be
	// invisible - which is the failure mode of a permanent mute nobody can see.
	const Int durableIgnores = GXPlayerIdentity::getIgnoreCount();
	if (durableIgnores > 0)
	{
		UnicodeString row;
		row.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreDurableCount",
			L"  %d player(s) are ignored permanently, in this lobby or not. /unignore <name or P3> "
			L"clears one who is here."), durableIgnores);
		relaySocialSay(row, GameMakeColor( 160, 160, 160, 255 ));
	}
	else if (ignored == 0)
	{
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreHint",
			L"  /ignore <name or P3> hides someone's chat. It lasts for this lobby, or forever if "
			L"you have swapped friend codes with them."),
			GameMakeColor( 160, 160, 160, 255 ));
	}
}

/// Handle one of this feature's slash commands. TRUE when the line was consumed.
///
/// EVERY branch under a verb we own returns TRUE, including the error paths. That is deliberate
/// and it is a safety property, not tidiness: a "/add <code>" that fell through would put the
/// player's bearer credential in front of the whole room.
///
/// GeneralsX @feature Claude 11/08/2026 That is now belt AND braces. relaySubmitChat used to
/// broadcast anything it did not recognise; GXLobbyGuardUnknownCommand refuses it instead, so a
/// fallthrough here is a bug rather than a disclosure. Keep the returns anyway - the guard is the
/// backstop, not the design.
static Bool relayHandleSocialCommand( const AsciiString &verb, AsciiString rest )
{
	if (verb == "friends")
	{
		relayPrintFriends();
		return TRUE;
	}

	// GeneralsX @feature Claude 11/08/2026 /add and /remove, flattened out of /friend's sub-verbs.
	//
	// THE CARD EXCHANGE THAT USED TO LIVE HERE IS GONE BY DECISION, not by accident. Owner,
	// 11/08/2026: "two adds = friend - if I add someone and someone adds me, that's it, I don't have
	// to accept". That is already how the store behaves - GXPlayerIdentity::addFriend is unilateral
	// and immediate - so /friend request and /friend accept only ever existed to carry the two codes
	// to each other, and that job moves to the friends overlay (Phase 4a).
	//
	// The RECEIVE half is deliberately left intact further down this file. A peer on an older build
	// can still send an 'fq' peer line, and an unanswered request must read as something the player
	// can act on rather than becoming a crash or a silent drop - see the note on relayCardLatch.
	if (verb == "add")
	{
		AsciiString code;
		rest.nextToken(&code);
		rest.trim();				// whatever is left is the name
		code.toUpper();				// codes are base32 upper; accept a lower-case paste

		if (!GXPlayerIdentity::isValidPlayerId(code.str()))
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendBadCode",
				L"That is not a friend code. A code is exactly 26 characters using A-Z and 2-7. "
			L"Usage: /add <code> <name>"), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// Tested here rather than inferred from addFriend's return. addFriend answers FALSE for
		// a malformed id (already excluded), for your own id, AND for a failed save - one
		// value for three causes. Reporting "that is your own code" at a read-only UserData
		// directory would send somebody hunting the wrong problem entirely.
		if (strcmp(code.str(), GXPlayerIdentity::getPlayerId()) == 0)
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendSelf",
				L"That is your own code - you cannot add yourself."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		if (!GXPlayerIdentity::addFriend(code.str(), rest.str()))
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendSaveFailed",
				L"Could not save your friends list. Check that GXPlayer.ini in your user data "
				L"folder is writable."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		if (rest.isEmpty())
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendAddedNoName",
				L"Friend added, but with no name - the game list filter matches on name, so run "
				L"/add <code> <name> again once you know what they call themselves."),
				GameSpyColor[GSCOLOR_MOTD]);
		}
		else
		{
			UnicodeString wide;
			wide.translate(rest);
			UnicodeString note;
			note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendAdded",
				L"%ls is now on your friends list. It is saved, so it survives a restart."),
				wide.str());
			relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
		}
		return TRUE;
	}

	if (verb == "remove")
	{
		const Int idx = relayFriendIndexByToken(rest);
		if (idx < 0)
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendNotFound",
				L"No friend by that name or code. /friends lists them."),
				GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// COPY the id before removing. getFriendId returns a pointer into the store's own map
		// node, and removeFriend erases that node - passing the pointer straight back in would
		// be reading it after the erase.
		const AsciiString id( GXPlayerIdentity::getFriendId(idx) );
		AsciiString shownName( GXPlayerIdentity::getFriendNick(idx) );
		if (shownName.isEmpty())
			shownName = id;

		// The id came out of the store a line ago, so "not present" is not a possible cause of a
		// FALSE here and neither is a malformed id. That leaves the save, and the entry IS
		// already gone from memory at this point - so the honest report is that the change will
		// not survive a restart, not that nothing happened.
		if (!GXPlayerIdentity::removeFriend(id.str()))
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendRemoveFailed",
				L"Removed for now, but the friends list could not be written - it will come back "
				L"next time you start the game. Check that GXPlayer.ini is writable."),
				GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		UnicodeString wide;
		wide.translate(shownName);
		UnicodeString note;
		note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendRemoved",
			L"%ls removed from your friends list."), wide.str());
		relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
		return TRUE;
	}

	if (verb == "mycode")
	{
		const char *id = GXPlayerIdentity::getPlayerId();
		if (id == nullptr || id[0] == '\0')
		{
			// getPlayerId comes back empty when the platform CSPRNG refused, which
			// GXPlayerIdentity treats as fatal rather than inventing a guessable id, or while the
			// Windows migration has not yet brought the old GXPlayer.ini across (holdNewIdentity).
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNoIdentity",
				L"No player identity this session - friends are unavailable. See the [GXID] line on "
				L"stderr."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMyCodeWarning",
			L"Your friend code is below. Give it only to people you want as friends: anyone holding "
			L"it can appear as you to your other friends. Do not post it."),
			GameSpyColor[GSCOLOR_MOTD]);

		UnicodeString wide;
		wide.translate(AsciiString(id));
		UnicodeString note;
		note.format(L"%ls", wide.str());
		relaySocialSay(note, GameSpyColor[GSCOLOR_CHAT_OWNER]);
		return TRUE;
	}

	if (verb == "ignore" || verb == "unignore")
	{
		const Bool wantIgnore = (verb == "ignore");
		rest.trim();

		if (rest.isEmpty())
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreUsage",
				L"Usage: /ignore <name, P3 or code>, /unignore <name, P3 or code>. For a stranger it "
				L"lasts while you are in this lobby; with a friend code it is permanent, because "
				L"there is then something real to key it on."),
				GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		const UnsignedInt slot = relayResolveTargetSlot(rest);
		if (slot == 0)
		{
			// GeneralsX @feature Claude 11/08/2026 A CODE works on somebody who is not here.
			//
			// The resolver above answers for anyone PRESENT - by name, by P3, or by the code of a
			// friend whose slot is bound - and present targets keep taking that path so the row
			// recolours and the session entry latches. What could not be done at all was muting
			// somebody absent: the store has always keyed ignores on ids, but the only road in
			// insisted on a room slot first. A valid code now goes straight at the store.
			AsciiString code = rest;
			code.toUpper();				// codes are base32 upper; accept a lower-case paste
			if (GXPlayerIdentity::isValidPlayerId(code.str()))
			{
				if (strcmp(code.str(), GXPlayerIdentity::getPlayerId()) == 0)
				{
					relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreSelf",
						L"You cannot ignore yourself."), GameSpyColor[GSCOLOR_MOTD]);
					return TRUE;
				}

				// The short prefix is what gets echoed, never the whole code: the full string is a
				// bearer credential and the chat box ends up in screenshots. The nick is fetched
				// and COPIED before any mutation - store pointers die on the next write.
				UnicodeString shown;
				const char *knownNick = GXPlayerIdentity::getFriendNickById(code.str());
				if (knownNick != NULL && knownNick[0] != '\0')
				{
					shown.translate(AsciiString(knownNick));
				}
				else
				{
					char prefixBuf[8];
					strlcpy(prefixBuf, code.str(), 7);	// six characters identify it
					shown.translate(AsciiString(prefixBuf));
				}

				UnicodeString note;
				if (wantIgnore)
				{
					// The return value is the SAVE, not the intent: addIgnore writes memory first
					// and answers FALSE only when persisting failed. Malformed and self are already
					// excluded above, so FALSE here has exactly one meaning.
					if (GXPlayerIdentity::addIgnore(code.str()))
						note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreSetByCode",
							L"Ignoring %ls permanently by friend code. They do not have to be here "
							L"for it to hold."), shown.str());
					else
						note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreByCodeSaveFailed",
							L"Ignoring %ls for this session, but the list could not be saved - check "
							L"that GXPlayer.ini in your user data folder is writable, or this will "
							L"not survive a restart."), shown.str());
					relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
					return TRUE;
				}

				// Unignore: "was never there" and "could not save" both come back FALSE from
				// removeIgnore, so the two are told apart by asking first.
				if (!GXPlayerIdentity::isIgnored(code.str()))
				{
					note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayUnignoreByCodeAbsent",
						L"%ls is not on your ignore list."), shown.str());
				}
				else if (GXPlayerIdentity::removeIgnore(code.str()))
				{
					note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayUnignoreByCode",
						L"No longer ignoring %ls."), shown.str());
				}
				else
				{
					note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayUnignoreByCodeSaveFailed",
						L"%ls is unignored for this session, but the list could not be saved - check "
						L"that GXPlayer.ini in your user data folder is writable, or they will still "
						L"be ignored after a restart."), shown.str());
				}
				relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
				return TRUE;
			}

			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreNoSuchPlayer",
				L"Nobody in this lobby by that name. Chat lines are labelled [name] or [P3] - either "
				L"works - and a 26-character friend code works even when they are not here."),
				GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		if (wantIgnore && slot == relayMySlot())
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreSelf",
				L"You cannot ignore yourself."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// GeneralsX @feature Claude 08/08/2026 Durable when, and ONLY when, there is a real identity
		// to key it on.
		//
		// The store has always had an id-keyed ignore list and it was deliberately left unreachable,
		// because until the card exchange existed the only key available was a nickname - and a
		// durable mute keyed on a nickname silences whoever claims that name next, invisibly, with
		// no way for the wronged player to find out. So: a slot we have swapped codes with is
		// ignored PERMANENTLY; anyone else is ignored for this lobby, exactly as before. Both are
		// correct answers to different questions and neither replaces the other.
		//
		// COPIED before use. relaySlotDurableId returns a pointer into s_slotId, which addIgnore
		// cannot disturb - but the durable path below also drops the session entry, and keeping the
		// two orders independent is what stops the next edit here from being a use-after-free.
		char durableId[GX_LOBBY_ID_MAX];
		strlcpy(durableId, relaySlotDurableId(slot), sizeof(durableId));
		const Bool haveId = (durableId[0] != '\0');

		Bool wentDurable = FALSE;
		if (haveId)
		{
			wentDurable = wantIgnore ? GXPlayerIdentity::addIgnore(durableId)
			                         : GXPlayerIdentity::removeIgnore(durableId);
		}

		// The session entry is maintained either way. On the durable path it is cleared rather than
		// set, so a player who was session-ignored and is now permanently ignored does not carry two
		// entries that expire on different rules.
		s_ignoreSlot[slot] = (wantIgnore && !wentDurable) ? TRUE : FALSE;
		if (s_ignoreSlot[slot])
		{
			// Latch the name they hold RIGHT NOW. relaySlotIsIgnored compares against it, so the
			// entry expires by itself the moment the slot is somebody else.
			strlcpy(s_ignoreNick[slot], relayRosterNickForSlot(slot), sizeof(s_ignoreNick[slot]));
		}
		else
		{
			s_ignoreNick[slot][0] = '\0';
		}

		UnicodeString wide;
		wide.translate(rest);
		UnicodeString note;
		if (wantIgnore && wentDurable)
			note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreSetDurable",
				L"Ignoring %ls (P%d) permanently - you have their friend code, so this survives a "
				L"restart and follows them between lobbies."), wide.str(), (Int)slot);
		else if (wantIgnore)
			note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreSet",
				L"Ignoring %ls (P%d) for this lobby. Their chat will not appear."),
				wide.str(), (Int)slot);
		else
			note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayIgnoreCleared",
				L"No longer ignoring %ls (P%d)."), wide.str(), (Int)slot);
		relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
		return TRUE;
	}

	// GeneralsX @feature Claude 08/08/2026 Private messages.
	//
	//     /msg <name|P3|friend code> <text>
	//     /r <text>                            answer whoever last messaged you
	//
	// The line goes out as a unicast relay-header datagram addressed to that one room slot, so the
	// relay delivers it to that member and to nobody else - see relaySendPeerLine for why this is
	// not a relay control tag, which is the design that would have leaked it.
	//
	// EVERY path returns TRUE, including every error. relaySubmitChat broadcasts what it does not
	// recognise, so a mistyped /msg falling through would put a private message in front of the
	// whole room - the exact failure this feature exists to prevent, arrived at by a typo.
	// GeneralsX @feature Claude 11/08/2026 One spelling each. The five retired aliases are named
	// in GXLobbyCommands.cpp's retired table, which runs BEFORE this - so /w still answers, it just
	// answers with "that is now /msg" instead of putting the message in front of the room.
	if (verb == "msg" || verb == "r")
	{
		const Bool isReply = (verb == "r");

		UnsignedInt slot = 0;
		if (isReply)
		{
			slot = s_lastDmFrom;
			rest.trim();
			if (slot == 0)
			{
				relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgNoReplyTarget",
					L"Nobody has messaged you yet. /msg <name> <text> starts one."),
					GameSpyColor[GSCOLOR_MOTD]);
				return TRUE;
			}

			// REFUSE rather than send to whoever holds the slot now. Slots are recycled while a
			// lobby is open, so without this the reply to somebody who has left is delivered to
			// the stranger who replaced them - privately, and with no sign to the sender that it
			// went to the wrong person. Refusing loudly is the only acceptable behaviour here.
			Bool stillPresent = FALSE;
			const char *nowNick = relayRosterNickForSlot(slot, &stillPresent);
			if (!stillPresent || strcmp(nowNick, s_lastDmNick) != 0)
			{
				s_lastDmFrom = 0;
				s_lastDmNick[0] = '\0';
				relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgReplyTargetGone",
					L"They have left, or somebody else holds that slot now - nothing was sent. Use "
					L"/msg <name> <text> if they come back."), GameSpyColor[GSCOLOR_MOTD]);
				return TRUE;
			}
		}
		else
		{
			AsciiString target;
			rest.nextToken(&target);
			rest.trim();
			if (target.isEmpty())
			{
				relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgUsage",
					L"Usage: /msg <name or P3> <text>. Only they see it. /r <text> answers the last "
					L"person who messaged you."), GameSpyColor[GSCOLOR_MOTD]);
				return TRUE;
			}

			Bool friendElsewhere = FALSE;
			slot = relayResolveTargetSlot(target, &friendElsewhere);
			if (slot == 0)
			{
				// "Your friend is not here" is a different answer from "no such player", and giving
				// the second for the first sends the player hunting a spelling mistake that is not
				// there. Private messages cannot cross rooms: the relay routes on a virtual address
				// that only exists inside one room.
				relaySocialSay(friendElsewhere
					? TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgFriendNotHere",
						L"They are on your friends list but not in this lobby - a private message only "
						L"reaches somebody in the same lobby.")
					: TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgNoSuchPlayer",
						L"Nobody in this lobby by that name. Chat lines are labelled [name] or [P3] - "
						L"either works."), GameSpyColor[GSCOLOR_MOTD]);
				return TRUE;
			}
		}

		if (slot == relayMySlot())
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgSelf",
				L"You cannot message yourself."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		if (rest.isEmpty())
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgEmpty",
				L"Say something after the name."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		AsciiString body("dm ");
		body.concat(rest);
		if (!relaySendPeerLine(slot, body.str()))
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgFailed",
				L"Could not send - not connected to a relay room."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// Echoed locally, because the recipient is the only other copy - there is no broadcast to
		// hear ourselves on. Rendered with the arrow pointing away so a transcript cannot be
		// misread as something that was said to the room.
		UnicodeString wideTarget;
		const char *targetNick = relayRosterNickForSlot(slot);
		if (targetNick != nullptr && targetNick[0] != '\0')
			wideTarget.translate(AsciiString(targetNick));
		else
			wideTarget.format(L"P%d", (Int)slot);

		UnicodeString wideText;
		wideText.translate(rest);
		UnicodeString shown;
		shown.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgSentRow",
			L"[to %ls, private] %ls"), wideTarget.str(), wideText.str());
		relaySocialSay(shown, GameMakeColor( 220, 160, 255, 255 ));
		return TRUE;
	}

	return FALSE;
}

// GeneralsX @feature Claude 09/08/2026 The seam the code-built friends panel drives.
//
// Deliberately NOT a second parser. The split below is character-for-character what relaySubmitChat
// does above the same call (skip the '/', nextToken the verb, lowercase it, trim the rest), so a
// button and a typed line reach relayHandleSocialCommand in indistinguishable states. If the chat
// path's parsing ever changes, this has to change with it - which is exactly why it is three lines
// next to the handler rather than a copy living in the panel's own file.
//
// The panel synthesises the same argument text a player would type ("/ignore P3", "/msg P3 hello",
// "/friend add <code>"), so name and slot resolution, the code-format check and every error string
// stay in this file and are never reimplemented on the UI side.
Bool GXRelayRunSocialCommand( const AsciiString &line )
{
	// isEmpty first: getCharAt has a DEBUG_ASSERTCRASH on the index, so reading [0] out of an empty
	// string is a debug-build crash even though it returns 0 in release.
	if (line.isEmpty() || line.getCharAt(0) != '/')
		return FALSE;

	AsciiString rest = line.str() + 1;
	AsciiString verb;
	rest.nextToken(&verb);
	verb.toLower();
	rest.trim();

	return relayHandleSocialCommand(verb, rest);
}

/// Host a game on the relay. Same two calls the Direct Connect screen makes; the LAN callbacks push
/// the game options screen from OnGameCreate.
// GeneralsX @feature Claude 16/08/2026 `hostName` and `password` come from PopupHostGame when the
// player hosted through it; both null reproduces the previous behaviour exactly, which is what the
// LAN screens and any non-popup caller still get.
static void relayHostGame( const UnicodeString *hostName = nullptr,
                           const AsciiString *password = nullptr )
{
	if (TheLAN == nullptr)
		return;

	UnsignedInt localIP = TheLAN->GetLocalIP();
	UnicodeString localIPString;
	localIPString.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(localIP));

	// This layout has no name entry - GameSpy took the name from the login, which we do not have.
	// The LAN preferences hold the same name the Direct Connect and LAN screens use, so a player
	// keeps one identity across all three rather than being renamed by which screen they came in
	// through.
	//
	// GeneralsX @bugfix Claude 16/08/2026 THE HOST'S NAME IS THE HOST'S NAME AGAIN.
	//
	// This used to read `hostName` here when PopupHostGame supplied one, on the reasoning that the
	// browser's "Game (host)" column IS this name and the relay model had no separate game-name
	// field. The consequence was reported immediately: the text typed into the popup's Game Name
	// box became the host's PLAYER name, so their own slot in GAME OPTIONS showed the lobby's name
	// instead of who they are.
	//
	// There is a separate field now - GameInfo::setLobbyName, carried as "LN=" in the announce -
	// so the two identities no longer have to share one value. `hostName` is applied below, after
	// the game exists, and never to the player.
	LANPreferences userprefs;
	UnicodeString name = userprefs.getUserName();
	if (name.isEmpty())
		name = TheGameText->fetch("GUI:Player");
	name.truncateTo(g_lanPlayerNameLength);

	// BEFORE RequestGameCreate, and that ordering is the whole point. applyLobbyCommand's own reply
	// text documents the pre-hosting case - "It applies to the next game you host" - and it carries
	// the value in s_carriedLobbyPassword precisely so it survives into the game about to be made.
	// Binding it afterwards would leave a window in which the room is advertised and joinable with
	// no password at all, which is the one moment an uninvited joiner is actually looking.
	//
	// Driven through applyLobbyCommand rather than a new LANAPI setter ON PURPOSE. LANAPI.{h,cpp}
	// live under GameNetwork/, inside the sourceID digest; adding a method there would move the
	// digest and every peer on the shipped build would refuse every join - a hard wire break, for a
	// menu. applyLobbyCommand is already public, already does the three assignments this needs
	// (m_joinPassword, setHostPassword, s_carriedLobbyPassword), and is the same path /password
	// has used since 08/08. The verb takes the whole remainder of the line, so a password
	// containing spaces survives; the local echo is discarded because the popup is the feedback.
	if (password != nullptr)
	{
		AsciiString cmd("/password ");
		cmd.concat(password->str());		// empty clears, exactly as typing "/password" alone does
		UnicodeString ignoredEcho;
		TheLAN->applyLobbyCommand(cmd, ignoredEcho);
	}

	TheLAN->RequestSetName(name);
	relayClaimPersistedNickname();		// relay-held half of the same identity
	TheLAN->RequestGameCreate(localIPString, TRUE);

	// AFTER RequestGameCreate, because that call is what constructs the LANGameInfo this writes to;
	// there is nothing to name before it. The next announce carries it - the periodic re-announce
	// is what publishes lobby state, so nothing needs to be re-sent by hand here.
	//
	// localIPString above is still the argument to RequestGameCreate and still has nothing to do
	// with the display name: that parameter feeds LANGameInfo's IDENTITY, which is built as
	// "%8.8X%8.8X" of IP and seed and used as the lookup key in LANAPI.cpp's game search. Passing
	// a human name there would be truncated away by g_lanGameNameLength and would break matching
	// between two lobbies that chose the same title.
	if (hostName != nullptr && !hostName->isEmpty() && TheLAN->GetMyGame() != nullptr)
	{
		AsciiString lobbyName;
		lobbyName.translate(*hostName);
		TheLAN->GetMyGame()->setLobbyName(lobbyName);	// sanitises and caps; see setLobbyName
	}
}

/// GeneralsX @feature Claude 16/08/2026 PopupHostGame's relay entry point.
///
/// The popup is stock Zero Hour UI (Menus/PopupHostGame.wnd, inside WindowZH.big) and it already
/// carries a Game Name and a Game Password box. Nothing about it is edited here and nothing needs
/// to be: LANAPI.h's note that "there is no way to add a text box" is about ADDING one - editing a
/// .wnd moves dataID and every peer then refuses with DATA_DIFFERS - and this adds none. It opens a
/// layout that has shipped, unused, since the GameSpy chain that used to open it died.
Bool GXRelayLobbyIsActive( void )
{
	return s_relayLobby;
}

void GXRelayHostGameWithOptions( const UnicodeString &gameName, const AsciiString &password )
{
	relayHostGame(&gameName, &password);
}

/// Join the selected game.
///
/// The sequence below is lifted from JoinDirectConnectGame and none of it is interchangeable:
///
///   1. take the room from the LISTING, never from Options.ini - the game we picked decides where
///      it lives, and our own configuration says nothing about it;
///   2. ask the relay for an identity IN THAT ROOM, because addresses are allocated per room and
///      the one we hold was allocated somewhere else. Carrying it over would not be a private
///      address in a new place, it would be somebody else's address;
///   3. throw TheLAN away and build a new one, so no LANGameInfo is alive across the change.
///      LANGameInfo snapshots the local IP in its constructor and AmIHost and getLocalSlotNum read
///      that snapshot rather than TheLAN - an identity that lands after one exists reaches nothing
///      that decides anything;
///   4. only then SetLocalIP, which rebinds the transport and re-runs initRelay against the new
///      room and identity;
///   5. and only then dial the host.
// GeneralsX @feature Claude 06/08/2026 GX_UI trace for driving this screen without seeing it.
//
// A DXVK window does not survive a GDI screen grab - CopyFromScreen returns a black client area,
// and probe-102-hotkey.sh already records nine blank screenshots being mistaken for a result. So
// an automated run cannot tell "the browser is empty" from "the click missed the row": both end
// as a silent GUI:NoGameSelected. This prints what the list actually holds and what is actually
// selected, which is the only way to close that loop from a shell.
//
// GameClient/ is NOT in simsourcedigest_watcher.cmake, so this ships to one peer without moving
// sourceID and without a lockstep-wide rebuild. Env-gated, so it costs nothing when unset.
static void relayTraceGameList( const char *where )
{
	static const Bool s_on = (getenv("GX_UI") != NULL);
	if (!s_on)
		return;

	GameWindow *listbox = GetGameListBox();
	Int selectedRow = -1;
	if (listbox != nullptr)
		GadgetListBoxGetSelected(listbox, &selectedRow);

	fprintf(stderr, "[GXUI] %s: listbox=%s selectedRow=%d selectedID=%d listedGames=%d\n",
		where, (listbox != nullptr) ? "present" : "NULL", selectedRow,
		relaySelectedGameID(), s_relayGameCount);
	for (Int i = 0; i < s_relayGameCount; ++i)
	{
		fprintf(stderr, "[GXUI]   row %d: id=%d room='%s' host=%d.%d.%d.%d players=%d slots=%d\n",
			i, s_relayGames[i].id, s_relayGames[i].room,
			PRINTF_IP_AS_4_INTS(s_relayGames[i].hostVirtualIP),
			s_relayGames[i].players, s_relayGames[i].slots);
	}
	fflush(stderr);
}

static void relayJoinSelectedGame()
{
	if (TheLAN == nullptr)
		return;

	relayTraceGameList("join pressed");

	const Int selectedID = relaySelectedGameID();
	const RelayListedGame *game = (selectedID > 0) ? relayFindGameByID(selectedID) : nullptr;
	if (game == nullptr)
	{
		GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), nullptr);
		return;
	}

	if (game->slots > 0 && game->players >= game->slots)
	{
		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedRoomFull"));
		return;
	}

	const UnsignedInt hostIP = game->hostVirtualIP;
	AsciiString targetRoom(game->room);

	// Deliberately NOT SetLobbyAttemptHostJoin(TRUE). On the GameSpy path that flag is cleared by a
	// response from the peer thread, and there is no peer thread here: a join that times out would
	// leave it set forever, and Back is gated on it too - so a single unreachable host would lock
	// the player on this screen with every button dead. LANAPI already refuses a second attempt
	// (RequestGameJoinDirectConnect answers RET_BUSY while m_pendingAction is set), which is the
	// same protection the Direct Connect screen relies on.

	Transport *transport = TheLAN->getTransport();
	if (transport != nullptr && transport->isRelayEnabled() && !targetRoom.isEmpty()
		&& strcmp(targetRoom.str(), transport->getRelayRoom()) != 0)
	{
		OptionPreferences optprefs;
		const AsciiString relayHost = optprefs.getRelayAddress();

		UnsignedInt joinIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), targetRoom.str(), joinIP))
		{
			DEBUG_LOG(("relayJoinSelectedGame - moving from room '%s' to '%s'; we are %d.%d.%d.%d there",
				transport->getRelayRoom(), targetRoom.str(), PRINTF_IP_AS_4_INTS(joinIP)));

			delete TheLAN;
			TheLAN = GXCreateLobbyLANAPI();
			DEBUG_ASSERTCRASH(TheLAN->GetMyGame() == nullptr,
				("relayJoinSelectedGame - a LANGameInfo already exists and has snapshotted the old local IP; SetLocalIP below will not reach it"));
			TheLAN->init();
			TheLAN->SetLocalIP(joinIP);

			// The browser was built from the old transport's replies and the new one has heard
			// nothing yet. Re-ask, so a failed join leaves a screen that refills itself instead of
			// an empty list.
			relayRequestGameList();
		}
		else
		{
			// Dialling anyway would reach the relay but never the host: we are not a member of the
			// room the packet has to be forwarded in. Say so rather than letting it time out as a
			// generic unreachable peer.
			DEBUG_LOG(("relayJoinSelectedGame - the relay would not give us an identity in room '%s'; the join will not reach the host",
				targetRoom.str()));
		}
	}

	LANPreferences userprefs;
	UnicodeString name = userprefs.getUserName();
	if (name.isEmpty())
		name = TheGameText->fetch("GUI:Player");
	name.truncateTo(g_lanPlayerNameLength);

	TheLAN->RequestSetName(name);
	relayClaimPersistedNickname();		// relay-held half of the same identity
	TheLAN->RequestGameJoinDirectConnect(hostIP);
}

/// Per-frame work for the relay lobby. Nothing else pumps the transport on this screen, and without
/// a pump the relay's replies sit in the socket and the browser stays empty - the same trap that
/// made the headless driver have to pump LANAPI itself.
static void relayLobbyUpdate()
{
	if (TheLAN == nullptr)
		return;

	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	TheLAN->update();

	// Re-ask on the same cadence the screen already refreshes on, so a game that appears or dies
	// after we arrived shows up without the player pressing anything. Hosts re-advertise every 5 s
	// and the relay delists purely by not hearing from them, so this is also how a dead host leaves
	// the list.
	//
	// GeneralsX @bugfix Claude 12/08/2026 Except after a withdrawal, which is not something to wait
	// out a poll for: the relay has already been told the game is gone, so the answer to the next
	// query is known to differ from the one on screen. Compared as a serial rather than timed, so
	// this fires exactly once per withdrawal however long the frame took.
	if (s_relayLastQueryTime == 0
		|| Transport::getAdvertWithdrawSerial() != s_relayWithdrawSerialSeen
		|| (time_t)(s_relayLastQueryTime + gameListRefreshInterval) <= (time_t)timeGetTime())
	{
		relayRequestGameList();
	}

	// GeneralsX @bugfix Claude 07/08/2026 Rebuild when the list's CONTENT changes, not only when the
	// number of games does.
	//
	// The old test was `count != s_relayCountLastBuilt`, so a row was only ever rebuilt when a game
	// appeared or disappeared. A player joining or leaving an EXISTING game changes that game's
	// players field and nothing else - the count is still 1 - so the Slots column sat at its old
	// value until the player hit Refresh by hand. That was reported as the browser "not being
	// realtime", and it was: the data arrived every 3 s and was then discarded.
	//
	// Fingerprint deliberately EXCLUDES the ping. Latency moves on its own and would otherwise
	// rebuild the list every sweep for a number that is already redrawn in place.
	const Int count = transport->getGameListCount();

	UnsignedInt listPrint = 2166136261u;			// FNV-1a over the fields that are rendered
	for (Int g = 0; g < count; ++g)
	{
		const Transport::RelayGameListing *l = transport->getGameListing(g);
		if (l == nullptr)
			continue;

		const char *bytes[3] = { l->room, l->name, l->map };
		for (Int b = 0; b < 3; ++b)
		{
			for (const char *c = bytes[b]; *c != '\0'; ++c)
				listPrint = (listPrint ^ (UnsignedInt)(unsigned char)*c) * 16777619u;
			listPrint = (listPrint ^ 0xFFu) * 16777619u;	// field terminator
		}

		const UnsignedInt nums[3] = { l->hostVirtualIP, (UnsignedInt)l->players, (UnsignedInt)l->slots };
		for (Int n = 0; n < 3; ++n)
		{
			for (Int shift = 0; shift < 32; shift += 8)
				listPrint = (listPrint ^ ((nums[n] >> shift) & 0xFFu)) * 16777619u;
		}
	}

	if (count == s_relayCountLastFrame
		&& (count != s_relayCountLastBuilt || listPrint != s_relayListLastBuilt))
	{
		s_relayCountLastBuilt = count;
		s_relayListLastBuilt = listPrint;
		relayRebuildStagingRooms();

		// Redraw directly rather than through refreshGameList(), which only redraws when
		// hasStagingRoomListChanged() says so - and that flag is only ever raised by addStagingRoom.
		// A sweep that returns NO games adds nothing, so it never raises it, and the screen would go
		// on showing rows for games that have all gone away. That is the one case where a redraw
		// matters most: every row on screen is now unjoinable.
		RefreshGameListBoxes();

		// A game we had selected may be gone. Nothing else notices, because the selection ID stays
		// whatever it was and the player list would keep showing a room that no longer exists.
		s_relaySelectedID = -1;
	}
	s_relayCountLastFrame = count;

	const Int selectedID = relaySelectedGameID();
	if (selectedID != s_relaySelectedID)
	{
		s_relaySelectedID = selectedID;
		relayRefreshPlayerList();

		// Join follows the selection here too, not just on a click: a rebuild can drop the game the
		// player had highlighted, and Join would otherwise stay lit over nothing.
		if (buttonJoin)
			buttonJoin->winEnable(selectedID > 0);
	}
}

#if defined(RTS_DEBUG)
Bool g_fakeCRC = FALSE;
Bool g_debugSlots = FALSE;
#endif

std::list<PeerResponse> TheLobbyQueuedUTMs;

// GeneralsX @refactor Claude 11/08/2026 handleLobbySlashCommands is DELETED, not preserved.
//
// It was the GameSpy lobby's /host /me /refresh (/fakecrc /slots in debug) interceptor, and it
// has been unreachable in this build since the Online button started pushing WOLCustomLobby.wnd
// directly: nothing calls StartPatchCheck, so nothing reaches startOnline, so the GameSpy login
// screens are never pushed, TheGameSpyPeerMessageQueue stays null, s_relayLobby is always TRUE,
// and the relay branch break'd before the only call site. The relay path's commands live in the
// shared GXLobbyCommands table and relayHandleSocialCommand. g_fakeCRC and g_debugSlots survive
// above - WOLGameSetupMenu and the staging-room CRC path still read them; only the toggles died
// with the dead screen.

static Bool s_tryingToHostOrJoin = FALSE;
// GeneralsX @feature Claude 18/09/2026 The sign-out request flags - see gxDrainSignOut.
static Bool s_signOutAsk = FALSE;	///< Sign Out was pressed: show the confirmation next frame
static Bool s_signOutGo = FALSE;	///< Yes was pressed and the sign-out DONE: leave (or explain) next frame
static GXSignOutResult s_signOutResult = GX_SIGNOUT_NOT_SIGNED_IN;	///< what that sign-out achieved
// GeneralsX @bugfix Claude 23/09/2026 relayReacquireIfStranded's rebind failed and took the socket with it.
static Bool s_reacquireRebindOwed = FALSE;
void SetLobbyAttemptHostJoin(Bool start)
{
	s_tryingToHostOrJoin = start;
}

// Tooltips -------------------------------------------------------------------------------

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);//TheGameText->fetch("TOOLTIP:PlayersInLobby") );
		return;
	}

	// GeneralsX @bugfix Claude 30/07/2026 Same crash as gameTooltip, same screen, same window of
	// frames - see the long note in Core/.../GameSpy/LobbyUtils.cpp. WOLLobbyMenuShutdown calls
	// TearDownGameSpy(), which nulls TheGameSpyInfo, and then leaves the layout on screen animating
	// out; this listbox keeps its tooltip func the whole time, so pointer motion over a populated
	// row loads through a null vtable. Line 1338 clears the module's own listboxLobbyPlayers
	// handle, which does nothing for the GameWindow that actually still carries the callback.
	if (TheGameSpyInfo == nullptr)
	{
		if (TheMouse != nullptr)
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}

	UnicodeString uName = GadgetListBoxGetText(window, row, COLUMN_PLAYERNAME);
	AsciiString aName;
	aName.translate(uName);

	PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);

	// The invariant that every drawn row is a key in this map is maintained by convention only -
	// the note at the top of this file says so outright. A row left on screen after the map has
	// been repopulated is enough to miss, and &(end()->second) is a fault, not an empty tooltip.
	if (it == TheGameSpyInfo->getPlayerInfoMap()->end())
	{
		if (TheMouse != nullptr)
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}

	PlayerInfo *info = &(it->second);
	Bool isLocalPlayer = (TheGameSpyInfo->getLocalName().compareNoCase(info->m_name) == 0);

	if (col == 0)
	{
		if (info->m_preorder)
		{
			TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyOfficersClub") );
		}
		else
		{
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);
		}
		return;
	}

	AsciiString	playerLocale = info->m_locale;
	AsciiString localeIdentifier;
	localeIdentifier.format("WOL:Locale%2.2d", atoi(playerLocale.str()));
	Int					playerWins   = info->m_wins;
	Int					playerLosses = info->m_losses;
	UnicodeString	playerInfo;
	playerInfo.format(TheGameText->fetchGameOnly("TOOLTIP:PlayerInfo"), TheGameText->fetch(localeIdentifier).str(), playerWins, playerLosses);

	UnicodeString tooltip = UnicodeString::TheEmptyString;//TheGameText->fetch("TOOLTIP:PlayersInLobby");
	if (isLocalPlayer)
	{
		tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:LocalPlayer"), uName.str());
	}
	else
	{
		// not us
		if (TheGameSpyInfo->getBuddyMap()->find(info->m_profileID) != TheGameSpyInfo->getBuddyMap()->end())
		{
			// buddy
			tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:BuddyPlayer"), uName.str());
		}
		else
		{
			if (info->m_profileID)
			{
				// non-buddy profiled player
				tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:ProfiledPlayer"), uName.str());
			}
			else
			{
				// non-profiled player
				tooltip.format(TheGameText->fetchGameOnly("TOOLTIP:GenericPlayer"), uName.str());
			}
		}
	}

	if (info->isIgnored())
	{
		tooltip.concat(TheGameText->fetch("TOOLTIP:IgnoredModifier"));
	}

	if (info->m_profileID)
	{
		tooltip.concat(playerInfo);
	}

	Int rank = 0;
	Int i = 0;
	while( info->m_rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;
	AsciiString sideName = "GUI:RandomSide";
	if (info->m_side > 0)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(info->m_side);
		if (fac)
		{
			sideName.format("SIDE:%s", fac->getSide().str());
		}
	}
	AsciiString rankName;
	rankName.format("GUI:GSRank%d", rank);
	UnicodeString tmp;
	tmp.format(L"\n%ls %ls", TheGameText->fetch(sideName).str(), TheGameText->fetch(rankName).str());
	tooltip.concat(tmp);

	TheMouse->setCursorTooltip( tooltip, -1, nullptr, 1.5f ); // the text and width are the only params used.  the others are the default values.
}

static void populateGroupRoomListbox(GameWindow *lb)
{
	if (!lb)
		return;

	GadgetComboBoxReset(lb);
	Int indexToSelect = -1;
	GroupRoomMap::iterator iter;

	// now populate the combo box
	for (iter = TheGameSpyInfo->getGroupRoomList()->begin(); iter != TheGameSpyInfo->getGroupRoomList()->end(); ++iter)
	{
		GameSpyGroupRoom room = iter->second;
		if (room.m_groupID != TheGameSpyConfig->getQMChannel())
		{
			DEBUG_LOG(("populateGroupRoomListbox(): groupID %d", room.m_groupID));
			if (room.m_groupID == TheGameSpyInfo->getCurrentGroupRoom())
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
				indexToSelect = selected;
			}
			else
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_ROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
			}
		}
		else
		{
			DEBUG_LOG(("populateGroupRoomListbox(): skipping QM groupID %d", room.m_groupID));
		}
	}

	GadgetComboBoxSetSelectedPos(lb, indexToSelect);
}

static const char *const rankNames[] = {
	"Private",
	"Corporal",
	"Sergeant",
	"Lieutenant",
	"Captain",
	"Major",
	"Colonel",
	"General",
	"Brigadier",
	"Commander",
};
static_assert(ARRAY_SIZE(rankNames) == MAX_RANKS, "Incorrect array size");


const Image* LookupSmallRankImage(Int side, Int rankPoints)
{
	if (rankPoints == 0)
		return nullptr;

	Int rank = 0;
	Int i = 0;
	while( rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;

	if (rank < 0 || rank >= 10)
		return nullptr;

	AsciiString sideStr = "N";
	switch(side)
	{
		case 2:  //USA
		case 5:  //Super Weapon
		case 6:  //Laser
		case 7:  //Air Force
			sideStr = "USA";
			break;

		case 3:  //China
		case 8:  //Tank
		case 9:  //Infantry
		case 10: //Nuke
			sideStr = "CHA";
			break;

		case 4:  //GLA
		case 11: //Toxin
		case 12: //Demolition
		case 13: //Stealth
			sideStr = "GLA";
			break;
	}

	AsciiString fullImageName;
	fullImageName.format("%s-%s", rankNames[rank], sideStr.str());
	const Image *img = TheMappedImageCollection->findImageByName(fullImageName);
	DEBUG_ASSERTLOG(img, ("*** Could not load small rank image '%s' from TheMappedImageCollection!", fullImageName.str()));
	return img;
}

static Int insertPlayerInListbox(const PlayerInfo& info, Color color)
{
	UnicodeString uStr;
	uStr.translate(info.m_name);

	Int currentRank = info.m_rankPoints;
	Int currentSide = info.m_side;
	/* since PersistentStorage updates now update PlayerInfo, we don't need this.
	if (info.m_profileID)
	{
		PSPlayerStats psStats = TheGameSpyPSMessageQueue->findPlayerStatsByID(info.m_profileID);
		if (psStats.id)
		{
			currentRank = CalculateRank(psStats);

			PerGeneralMap::iterator it;
			Int numGames = 0;
			for(it = psStats.games.begin(); it != psStats.games.end(); ++it)
			{
				if(it->second >= numGames)
				{
					numGames = it->second;
					currentSide = it->first;
				}
			}
			if(numGames == 0 || psStats.gamesAsRandom >= numGames )
			{
				currentSide = 0;
			}
		}
	}
	*/

	Bool isPreorder = TheGameSpyInfo->didPlayerPreorder(info.m_profileID);

	const Image *preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
	Int w = (preorderImg)?preorderImg->getImageWidth():10;
	//Int h = (preorderImg)?preorderImg->getImageHeight():10;
	w = min(GadgetListBoxGetColumnWidth(listboxLobbyPlayers, 0), w);
	Int h = w;
	if (!isPreorder)
		preorderImg = nullptr;

	const Image *rankImg = LookupSmallRankImage(currentSide, currentRank);

#if 0  //Officer's Club (preorder image) no longer used in Zero Hour
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, preorderImg, -1, 0, w, h);
	GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, index, 1, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 2);
#else
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, -1, 0, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 1);
#endif
	return index;
}


void PopulateLobbyPlayerListbox()
{

	if (!listboxLobbyPlayers)
		return;

	// GeneralsX @feature Claude 06/08/2026 The online-player list, on a relay.
	//
	// Everything below this block reads TheGameSpyInfo's PlayerInfoMap, which is populated by
	// peerchat. There is no peerchat on a relay, so that map is permanently empty and the list was
	// permanently blank - the operator asked for online players and there was nothing behind it.
	//
	// The relay knows the room's membership (it routes by it) and now answers GXROOM with it. The
	// request is fired here and the ANSWER is used on the next refresh: it arrives asynchronously
	// on the transport's own socket, and refreshPlayerList already runs on a timer, so a one-cycle
	// lag costs nothing and avoids blocking the UI on a round trip.
	//
	// Players are named by relay-assigned virtual IP because that is the only identity a relay has
	// - there are no accounts. The last octet is the room slot, and the local player is marked so
	// the list is not a row of indistinguishable numbers.
	if (s_relayLobby)
	{
		GadgetListBoxReset(listboxLobbyPlayers);

		if (TheLAN != nullptr)
		{
			gxLobbyRequestRoster();

			UnsignedInt ips[GX_ROOM_SLOTS];
			const Int n = gxLobbyRoster(ips, GX_ROOM_SLOTS);
			if (n > 0)
				s_relayHeardFromRelay = TRUE;
			{
				const Transport *t = relayTransport();
				if (t != nullptr && t->hasPublishedGameList())
					s_relayHeardFromRelay = TRUE;	// same relay, so it is reachable and answering
			}

			// GeneralsX @bugfix Claude 06/08/2026 The roster stores the last OCTET (the room slot),
			// while m_localVirtualIP is a full address in host order. Comparing them directly never
			// matched, so nobody was ever marked "(you)".
			//
			// GeneralsX @bugfix Claude 15/08/2026 ...and once it DID match, it matched the wrong
			// row. Masking the game transport's address fixed the type error but not the source
			// error: on the lobby screen the roster comes from the shared room, so the transport's
			// octet is a number from a different session that still lands on SOME row. The observed
			// result was another player painted green as "(you)" while the operator's own row was
			// not - the same wrong identity that was putting that player's name on the operator's
			// echoed chat lines. relayMySlot() is now the single answer; ask it.
			const UnsignedInt me = relayMySlot();

			for (Int i = 0; i < n; ++i)
			{
				// Short, because this listbox column is narrow: "player 1" wrapped to "playe" on one
				// line and "1" on the next, which is what the operator saw and read as wrong names.
				// A real name needs the peer to advertise one - the relay only knows 10.42.0.N.
				// This column fits TWO characters. "P2 (you)" wrapped; "P2*" still clipped the
				// asterisk. So the text carries no marker at all and "you" is shown by COLOUR,
				// which costs no width. Widening the column means editing the .wnd inside a .big.
				// GeneralsX @feature Claude 07/08/2026 Show the relay-held nickname when there is
				// one. The name comes back inside the roster, so it is known for everyone in the
				// room the moment we ask - including peers who set it long before we arrived.
				// GeneralsX @bugfix Claude 07/08/2026 Names go in column 1, not column 0.
				//
				// WOLCustomLobby.wnd declares this listbox as
				//     COLUMNS: 2, COLUMNSWIDTH: 10, COLUMNSWIDTH: 90
				// so column 0 is a tenth of the width - it is the ICON column, which is what the
				// rank/preorder badge is drawn into. Names were being written there, leaving about
				// two characters of room: "helloworld" rendered as "he" and "iPadPlayer" as "iPad".
				// The old comment here concluded the column simply could not fit a name and that
				// widening it meant editing a .wnd inside a .big - it did not, the name was just in
				// the wrong column. Column 1 is the 90% one and needs no data change at all.
				UnicodeString row;
				const char *nick = gxLobbyRosterNick(i);
				if (nick != NULL && nick[0] != '\0')
				{
					UnicodeString wideNick;
					wideNick.translate(AsciiString(nick));
					row.format(L"%ls", wideNick.str());
				}
				else
				{
					row.format(L"P%d", (Int)ips[i]);
				}

				// GeneralsX @feature Claude 08/08/2026 Friends and ignored players are coloured, not
				// labelled. This column has already been established as too narrow for a marker -
				// "P2 (you)" wrapped and "P2*" clipped the asterisk - and colour costs no width at
				// all. Order matters: "you" wins over everything (you cannot be your own friend and
				// the store refuses to record it), then ignored, then friend.
				// GeneralsX @feature Claude 08/08/2026 The friend test is now relaySlotIsFriend,
				// which matches on the DURABLE ID for any slot we have swapped a card with and only
				// falls back to the nickname otherwise. Two shades rather than one: a confirmed
				// friend is brighter than a name that merely looks like one, so the friend colour
				// stops meaning two different things at once. The column is still too narrow for a
				// marker (see above), and colour still costs no width.
				//
				// GeneralsX @tweak Claude 10/08/2026 BLUE, not gold - owner's call, and the legend
				// printed on entry is updated in the same change so the help cannot contradict the
				// screen. (That legend was ALREADY wrong in a second way: it said "gold", but a
				// GameSpy-path friend renders in GSCOLOR_PLAYER_BUDDY, which multiplayer.ini sets to
				// R:0 G:255 B:255 - cyan - in both retail data and Community Patch 2. Only this
				// relay path was ever gold.)
				//
				// The literals stay HERE rather than moving to GSCOLOR_PLAYER_BUDDY on purpose:
				// that entry is loaded from Data\INI\multiplayer.ini, which lives inside INIZH.big
				// and the Community Patch .big. Recolouring through the INI would be a data rollout
				// to every player on four platforms to change a colour, and these two are already
				// hardcoded here for exactly that reason.
				//
				// Light azure rather than a pure blue: the lobby background is dark and a saturated
				// (0,0,255) has almost no contrast against it. These two also have to stay clear of
				// the three colours already in this list - green (you), grey (ignored) and the red
				// PlayerNormal - which a mid blue does and a teal would not.
				Color rowColor = GameSpyColor[GSCOLOR_PLAYER_NORMAL];
				Bool friendByName = FALSE;
				if (ips[i] == me)
					rowColor = GameMakeColor( 120, 255, 120, 255 );
				else if (relaySlotIsIgnored(ips[i]))
					rowColor = GameMakeColor( 110, 110, 110, 255 );
				else if (relaySlotIsFriend(ips[i], &friendByName))
					rowColor = friendByName ? GameMakeColor(  95, 150, 205, 255 )
					                        : GameMakeColor( 105, 190, 255, 255 );

				// GeneralsX @feature Claude 07/08/2026 OS tag in the icon column.
				//
				// Column 0 has been free since names moved to column 1 - it is the 10% column the
				// stock GameSpy path draws the rank badge into, and the relay path draws nothing
				// there. That is the whole reason this fits without touching shipped .wnd data.
				//
				// TEXT, not a logo: an actual Windows/Apple/Tux image would have to go into a .big,
				// which is shipped data and a much larger job. Two characters is also the hard
				// ceiling here - the note above records that overflow WRAPS rather than clipping,
				// so a longer tag would make one row taller than its neighbours. "iP" sits exactly
				// at that limit, which is why iPadOS is not spelled out.
				//
				// The row must be CREATED by the column-0 call. GadgetListBoxAddEntryText treats
				// row == -1 as "allocate a new row", so calling it twice with -1 would produce two
				// rows rather than two columns of one - the returned index has to be threaded, the
				// same way the GameSpy path above threads it.
				const char *os = gxLobbyRosterOS(i);
				const UnicodeString osTag = relayPlatformLabel(os);

				// GeneralsX @feature Claude 11/08/2026 The role's NAME colour, and this is the
				// surface that can carry it. ListboxChat is COLUMNS: 1, so a chat row has exactly
				// one colour and the spec's "purple name with light-green text" cannot both live
				// there. ListboxPlayers is COLUMNS: 2 and ListEntryCell already holds a Color per
				// cell, so the name colour lands here and the text colour lands on the chat row.
				//
				// It OVERRIDES the friend colour rather than the other way round, because a
				// moderator you have befriended is still the person you need to be able to find in
				// this list. The ignored-grey above still wins, since that is a decision the player
				// made about somebody and it must not be overridden by anything.
				const GXAssertRole slotRole =
					GXLobbyRoleFromCode(gxLobbyRosterRole(i));
				const Color nameColor = relaySlotIsIgnored(ips[i])
					? rowColor
					: GXLobbyRoleNameColor(slotRole, rowColor);

				const Int rowIndex = GadgetListBoxAddEntryText(listboxLobbyPlayers, osTag, rowColor, -1, 0);
				GadgetListBoxAddEntryText(listboxLobbyPlayers, row, nameColor, rowIndex, 1);
				// GeneralsX @feature Claude 11/08/2026 The room slot rides the row, STAMPED AT
				// RENDER - the right-click handler must never resolve a row index against a
				// roster that may have refreshed underneath the click. Info rows below carry no
				// item-data, so they can never grow a menu.
				// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
				GadgetListBoxSetItemData(listboxLobbyPlayers,
					reinterpret_cast<void *>(static_cast<intptr_t>(ips[i])), rowIndex, 0);
			}

			// GeneralsX @bugfix Claude 07/08/2026 Do not answer before the relay has.
			//
			// The roster is a round trip. For the first few seconds after entering the lobby we
			// have asked and not yet heard, and saying "Nobody else here yet" in that window is
			// simply wrong - it was reported as the lobby looking empty until a manual refresh.
			// Same mistake the game list made with "No lobbies available at this moment", and the
			// same fix: say what is actually true, which is that we are still asking.
			if (n == 0 && !s_relayHeardFromRelay)
			{
				GadgetListBoxAddEntryText(listboxLobbyPlayers,
					TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbySearching", L"Checking who's here..."),
					GameSpyColor[GSCOLOR_MOTD], -1, 1);
			}
			else if (n == 0)
			{
				GadgetListBoxAddEntryText(listboxLobbyPlayers,
					TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyNoPlayers", L"Nobody else here yet"),
					GameSpyColor[GSCOLOR_MOTD], -1, 1);
			}
		}
		return;
	}

	// Display players
	PlayerInfoMap *players = TheGameSpyInfo->getPlayerInfoMap();
	PlayerInfoMap::iterator it;
	BuddyInfoMap *buddies = TheGameSpyInfo->getBuddyMap();
	BuddyInfoMap::iterator bIt;
	if (listboxLobbyPlayers)
	{
		// save off old selection
		Int maxSelectedItems = GadgetListBoxGetNumEntries(listboxLobbyPlayers);
		Int *selectedIndices;
		GadgetListBoxGetSelected(listboxLobbyPlayers, (Int *)(&selectedIndices));
		std::set<AsciiString> selectedNames;
		std::set<AsciiString>::const_iterator selIt;
		std::set<Int> indicesToSelect;
		UnicodeString uStr;
		Int numSelected = 0;
		Int i=0;
		for (; i<maxSelectedItems; ++i)
		{
			if (selectedIndices[i] < 0)
			{
				break;
			}
			++numSelected;
			AsciiString selectedName;
			uStr = GadgetListBoxGetText(listboxLobbyPlayers, selectedIndices[i], COLUMN_PLAYERNAME);
			selectedName.translate(uStr);
			selectedNames.insert(selectedName);
			DEBUG_LOG(("Saving off old selection %d (%s)", selectedIndices[i], selectedName.str()));
		}

		// save off old top entry
		Int previousTopIndex = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);

		GadgetListBoxReset(listboxLobbyPlayers);

		// Ops
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			if (info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID))
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_OWNER]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Buddies
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt != buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_BUDDY]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Everyone else
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt == buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_NORMAL]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// restore selection
		if (!indicesToSelect.empty())
		{
			std::set<Int>::const_iterator indexIt = indicesToSelect.begin();
			const size_t count = indicesToSelect.size();
			size_t index = 0;
			Int *newIndices = NEW Int[count];
			while (index < count)
			{
				newIndices[index] = *indexIt;
				DEBUG_LOG(("Queueing up index %d to re-select", *indexIt));
				++index;
				++indexIt;
			}
			GadgetListBoxSetSelected(listboxLobbyPlayers, newIndices, count);
			delete[] newIndices;
		}

		if (indicesToSelect.size() != numSelected)
		{
			TheWindowManager->winSetLoneWindow(nullptr);
		}

		// restore top visible entry
		GadgetListBoxSetTopVisibleEntry(listboxLobbyPlayers, previousTopIndex);
	}

}

//-------------------------------------------------------------------------------------------------
/** Initialize the WOL Lobby Menu */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuInit( WindowLayout *layout, void *userData )
{
	nextScreen = nullptr;
	buttonPushed = false;
	isShuttingDown = false;
	s_signOutAsk = FALSE;	// GeneralsX @feature Claude 18/09/2026 never carry a request into a new visit
	s_signOutGo = FALSE;
	s_signOutResult = GX_SIGNOUT_NOT_SIGNED_IN;
	s_reacquireRebindOwed = FALSE;	// SetUpOnlineLobbyLAN below binds afresh, or a joined game keeps its live socket

	// GeneralsX @feature Claude 14/08/2026 Take up presence in the SHARED lobby room.
	//
	// Idempotent, and deliberately NOT paired with a leaveLobby() in WOLLobbyMenuShutdown: this
	// screen is torn down when you join a game, and dropping presence there is precisely the
	// behaviour being fixed - the person in a game-setup screen is the one everyone else most wants
	// to see and talk to. The socket lives for the process and costs one small datagram every 5 s.
	//
	// It opens nothing when no relay is configured, so LAN-only players are unaffected.
	if (TheGXLobbyPresence != NULL)
	{
		TheGXLobbyPresence->enterLobby();
	}

	// GeneralsX @bugfix Claude 30/07/2026 Clear LANbuttonPushed here, like every other lobby Init.
	//
	// LANbuttonPushed is a latch that means "a screen transition is in flight, stop touching the
	// network". LANAPI::update() honours it at TWO points - it returns outright at the top, and the
	// receive loop is written `for (i=0; i<MAX_MESSAGES && !LANbuttonPushed; ++i)`. So while it is
	// set the client neither sends its periodic RequestGameOptions("HELLO") nor drains its inbound
	// queue. It is deaf and mute, not merely quiet.
	//
	// It is set by OnGameJoin, OnGameCreate, OnHostLeave and OnPlayerLeave, each of which pushes or
	// pops a screen, and it is cleared by the Init of whatever screen you land on -
	// LanLobbyMenuInit, LanGameOptionsMenuInit, NetworkDirectConnectInit all do it on their first
	// three lines. This screen never did, and this screen is the one the relay path actually uses:
	// OnHostLeave and OnPlayerLeave call TheShell->pop(), which lands back HERE, not on
	// LanLobbyMenu. So the first time a peer left, was dropped, or backed out, LANAPI stopped dead
	// for the rest of the process lifetime.
	//
	// Symptoms this produced, all one bug: the game list never refreshed again so the host looked
	// like it had disappeared and only a full restart brought it back; a peer that had bounced once
	// was invisible to everyone and saw nobody, including chat; and Accept appeared to do nothing
	// because the message was never put on the wire. The host, meanwhile, timed the silent peer out
	// after s_resendDelta*8 and announced "player was not responding", which reads as a network
	// fault and is not one.
	LANbuttonPushed = false;

	SetLobbyAttemptHostJoin(FALSE); // not trying to host or join

	gameListRefreshTime = 0;
	playerListRefreshTime = 0;

	// GeneralsX @feature Matchmaking. Decide, once, who is driving this screen.
	//
	// A null peer message queue means no GameSpy backend was ever started - which is now the only
	// way anyone reaches this layout, since the Online button pushes it directly instead of running
	// the patch-check -> login -> peerchat chain. Everything GameSpy-shaped below is skipped in that
	// case, and every one of those calls would otherwise dereference a null queue.
	//
	// This runs BEFORE the singletons are touched, because SetUpOnlineLobbyLAN destroys and rebuilds
	// TheLAN and the relay identity has to be pinned before any LANGameInfo exists.
	s_relayLobby = (TheGameSpyPeerMessageQueue == nullptr);
	if (s_relayLobby)
	{
		relayResetGameTable();

		// GeneralsX @bugfix Claude 06/08/2026 Do NOT re-derive our identity if we are already in
		// somebody else's game room.
		//
		// SetUpOnlineLobbyLAN() destroys TheLAN and asks the relay for an address in OUR OWN room
		// (Transport::getLocalHostRoom, NetworkDirectConnect.cpp:494). That room is empty, so it
		// always returns 10.42.0.1 - and it overwrites the process-global assigned identity.
		//
		// This screen stays on the shell stack under the game-options screen and re-initialises,
		// so a peer that joined somebody's game as 10.42.0.2 was silently reset to 10.42.0.1 - the
		// HOST's address. Measured in a live session: localIP went 10.42.0.1 -> 10.42.0.2 -> back
		// to 10.42.0.1 in one run.
		//
		// After that the peer is deaf to exactly one machine and it is the one that matters:
		// LANAPI::update() skips any inbound datagram whose senderIP == m_localIP, so every packet
		// FROM the host was discarded as if it were our own echo. Meanwhile the host saw a player
		// claiming the host's own address and never opened a slot for it, logging "Game Options
		// went to NOBODY - no human slot other than local slot 0". Accept and Back were sent and
		// arrived; only the replies were thrown away. That is why both buttons looked dead.
		//
		// So: only claim a fresh identity when we do not already hold one in a joined game.
		const Transport *heldTransport = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
		const Bool alreadyInAGameRoom =
			(heldTransport != nullptr) && heldTransport->isRelayEnabled()
			&& (TheLAN->GetMyGame() != nullptr);

		if (!alreadyInAGameRoom)
			SetUpOnlineLobbyLAN();
		else
			DEBUG_LOG(("WOLLobbyMenuInit - keeping the identity we hold in the joined game's room"));

		if (TheGameSpyInfo == nullptr)
		{
			SetUpGameSpyForRelayLobby();
			s_relayOwnsGameSpy = TRUE;
		}
	}

	parentWOLLobbyID = TheNameKeyGenerator->nameToKey( "WOLCustomLobby.wnd:WOLLobbyMenuParent" );
	parent = TheWindowManager->winGetWindowFromId(nullptr, parentWOLLobbyID);

	buttonBackID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBack");
	buttonBack = TheWindowManager->winGetWindowFromId(parent, buttonBackID);

	buttonHostID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonHost");
	buttonHost = TheWindowManager->winGetWindowFromId(parent, buttonHostID);

	buttonRefreshID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonRefresh");
	buttonRefresh = TheWindowManager->winGetWindowFromId(parent, buttonRefreshID);

	buttonJoinID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonJoin");
	buttonJoin = TheWindowManager->winGetWindowFromId(parent, buttonJoinID);
	buttonJoin->winEnable(FALSE);

	buttonBuddyID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBuddy");
	buttonBuddy = TheWindowManager->winGetWindowFromId(parent, buttonBuddyID);

	buttonEmoteID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonEmote");
	buttonEmote = TheWindowManager->winGetWindowFromId(parent, buttonEmoteID);

	textEntryChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:TextEntryChat");
	textEntryChat = TheWindowManager->winGetWindowFromId(parent, textEntryChatID);

	listboxLobbyPlayersID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxPlayers");
	listboxLobbyPlayers = TheWindowManager->winGetWindowFromId(parent, listboxLobbyPlayersID);
	listboxLobbyPlayers->winSetTooltipFunc(playerTooltip);
	relayFixPlayerListColumns(listboxLobbyPlayers);
	relayClaimPersistedNickname();		// so a returning player is named without typing /nickname

	listboxLobbyChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxChat");
	listboxLobbyChat = TheWindowManager->winGetWindowFromId(parent, listboxLobbyChatID);
	TheGameSpyInfo->registerTextWindow(listboxLobbyChat);

	// GeneralsX @feature Claude 13/08/2026 No block survives a visit to this screen. The window is
	// freshly created, so a remembered row index would name a row in a box that no longer exists -
	// and the pointer can legitimately be reused at the same address by the next visit, which is the
	// one case identity alone cannot catch.
	GXLobbyChatRowReset();

	comboLobbyGroupRoomsID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ComboBoxGroupRooms");
	comboLobbyGroupRooms = TheWindowManager->winGetWindowFromId(parent, comboLobbyGroupRoomsID);

	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	if (s_relayLobby)
	{
		// GeneralsX @feature Matchmaking. Turn off the controls that have nothing behind them.
		//
		// GeneralsX @feature Claude 06/08/2026 Lobby chat now HAS something behind it. The note
		// that used to sit here said "the relay carries no chat channel at all" - true of the
		// DEPLOYED relay and not of tools/relay/relay.js, which has implemented GXCHT/GXSAY since
		// 31/07 and was never shipped. The client half is now written too, so the entry is live.
		//
		// The moderation concern the old note raised is real and is NOT solved by this: chat on a
		// public relay is unauthenticated, and anyone who knows the room can talk. What exists
		// today is a length cap and control-byte stripping on both ends. Muting and reporting need
		// identity, and identity needs the account work that is not started.
		//
		// Buddies are GameSpy profiles. There are no accounts on the relay, so there is nothing a
		// buddy list could be a list OF, and GameSpyOpenOverlay(GSOVERLAY_BUDDY) dereferences the
		// buddy message queue we deliberately never created.
		// GeneralsX @bugfix Claude 06/08/2026 ButtonEmote is the SEND button, not an emote picker -
		// LanLobbyMenu.cpp:926 reads the entry and sends on it. Leaving it disabled while enabling
		// the text entry is why typing worked and nothing was ever sent: the operator's line stayed
		// in the box because the only control that submits it was dead.
		if (buttonEmote)
			buttonEmote->winEnable(TRUE);

		// GeneralsX @feature Claude 08/08/2026 ButtonBuddy is now the FRIENDS button.
		//
		// Reused rather than replaced: a new control means a new .wnd, .wnd files live inside
		// WindowZH.big, and editing shipped data moves dataID - every peer would then refuse to
		// join with DATA_DIFFERS. So the control keeps its artwork and gets a new meaning and a
		// tooltip that says so. The GBM_SELECTED handler below still refuses the GameSpy overlay on
		// the relay path; only the relay branch is new.
		//
		// Relabelled too, and that is safe here for a checked reason rather than a hopeful one.
		// WOLCustomLobby.wnd declares this control WINDOWTYPE = PUSHBUTTON with TEXT = "GUI:Buddies"
		// and the generic Buttons-Left background - it is a text button, not an icon, so
		// GadgetButtonSetText (which only sends GGM_SET_LABEL) replaces the word and touches no
		// artwork. Verified by extracting the .wnd out of WindowZH.big and reading it; the archive
		// itself is untouched, which is what keeps dataID where it is.
		//
		// TOOLTIPDELAY is -1 in the layout, which Mouse.cpp reads as "use the global delay" rather
		// than "no tooltip", so the tooltip below really does appear.
		if (buttonBuddy)
		{
			buttonBuddy->winEnable(TRUE);
			// FRIENDS, not "Friends": it sits in a row with CREATE GAME, JOIN GAME and BACK, and a
			// single title-case label in that row reads as a control from a different screen.
			GadgetButtonSetText(buttonBuddy,
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsButtonLabel", L"FRIENDS"));
			buttonBuddy->winSetTooltip(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsButton",
				L"Friends - your list, who is here, and how to add somebody."));
		}

		// Session state that must not survive a room change. Entering a game moves us to that
		// game's relay room and this screen re-initialises underneath it, so a slot-keyed ignore
		// from the old room would land on a different person in the new one.
		relayClearIgnores();

		// The group-room dropdown listed GameSpy chat lobbies. Ours is one relay, so there is no
		// second room to switch to - but there IS a second VIEW of the one we have, so the control
		// becomes the game-list filter instead of dead furniture.
		if (comboLobbyGroupRooms)
		{
			GadgetComboBoxReset(comboLobbyGroupRooms);
			OptionPreferences prefs;
			AsciiString relayHost = prefs.getRelayAddress();
			UnicodeString label;
			if (relayHost.isEmpty())
				label = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyNoRelay", L"No server configured");
			else
				// GeneralsX @bugfix Claude 06/08/2026 Do NOT put the relay address on screen.
				//
				// This printed the raw host - an IP in practice - at the top of the lobby, where it
				// is visible in every screenshot, stream and bug report anyone posts. It tells the
				// player nothing they can act on (they cannot choose a different one from here) and
				// it hands the address to anyone who wants to flood the room or the socket.
				//
				// This repository is deliberately free of operator addresses - CLAUDE.md says real
				// hosts live only in untracked local.env, and the repo was re-rooted because
				// addresses in old commits cannot be removed. Rendering one into the UI walks
				// straight around that.
				//
				// A name, not an address. When the signed relay list lands (autoship Layer 1) this
				// becomes the relay's advertised region label; until then it is a fixed string.
				label = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyConnected", L"Online");

			if (relayHost.isEmpty())
			{
				// Nothing to filter and nothing to switch to. Keep the old behaviour exactly.
				GadgetComboBoxAddEntry(comboLobbyGroupRooms, label, GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetSelectedPos(comboLobbyGroupRooms, 0, FALSE);
				comboLobbyGroupRooms->winEnable(FALSE);
			}
			else
			{
				// GeneralsX @feature Claude 08/08/2026 The dropdown is the game-list filter.
				//
				// Item data carries the meaning, not the row index: the strings are localised and
				// the order could reasonably change, and reading the selection back by position
				// would then silently invert the filter. Same 64-bit-safe cast this file uses for
				// every other listbox/combobox payload.
				const Int allIdx = GadgetComboBoxAddEntry(comboLobbyGroupRooms,
					TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyFilterAll", L"Online - all games"),
					GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetItemData(comboLobbyGroupRooms, allIdx,
					reinterpret_cast<void *>(static_cast<intptr_t>(0)));

				const Int friendsIdx = GadgetComboBoxAddEntry(comboLobbyGroupRooms,
					TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyFilterFriends",
						L"Online - friends' games"),
					GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetItemData(comboLobbyGroupRooms, friendsIdx,
					reinterpret_cast<void *>(static_cast<intptr_t>(1)));

				GadgetComboBoxSetSelectedPos(comboLobbyGroupRooms,
					s_relayFriendsOnly ? friendsIdx : allIdx, FALSE);
				comboLobbyGroupRooms->winEnable(TRUE);
			}
		}
	}
	else
	{
		populateGroupRoomListbox(comboLobbyGroupRooms);
	}

	// Show Menu
	layout->hide( FALSE );

	// if we're not in a room, this will join the best available one
	if (s_relayLobby)
	{
		// GeneralsX @feature Claude 08/08/2026 Declare our durable id, and drop every binding that
		// was made in whatever room we were in before.
		//
		// The clear comes FIRST and is not optional. This screen is re-entered after a game, and it
		// may be re-entered into a DIFFERENT room; a slot binding carried across would attach a real
		// person's identity to whoever now holds their old slot, which is the one thing this feature
		// must never do. relayDeclareIdentity is idempotent - Transport keeps the id and re-sends it
		// on its own tag tick - so calling it on every entry costs one datagram.
		//
		// GeneralsX @bugfix Claude 10/08/2026 ON A ROOM CHANGE, not on every entry to this screen -
		// the unconditional wipe destroyed every pending friend request on a CREATE GAME -> BACK
		// round trip. See relaySyncSocialSessionToRoom for the reproduction and for why keeping the
		// bindings inside one room is safe.
		relaySyncSocialSessionToRoom();
		relayDeclareIdentity();

		// No group rooms: they are GameSpy chat channels, and joining one is a peerchat round trip.
		// Our rooms are relay rooms and we are already in ours - SetUpOnlineLobbyLAN put us there.
		//
		// GeneralsX @bugfix Claude 06/08/2026 ONCE, not on every entry to this screen. This block
		// runs again whenever the lobby is re-entered and the chat listbox survives, so the banner
		// stacked up - the operator's screenshot showed each line printed twice.
		//
		// GeneralsX @bugfix Claude 11/08/2026 A HAVE-PRINTED FLAG, not the box's own emptiness.
		//
		// The old guard was `GadgetListBoxGetNumEntries(listboxLobbyChat) == 0`. That is the box's
		// state, and /clear is the player deliberately setting it - so /clear followed by CREATE
		// GAME -> BACK came back to an empty box and reprinted the whole banner, which makes /clear
		// a no-op with extra steps. A flag records what WE did, which is the thing /clear must not
		// be able to undo. It is never reset: the cost of losing it is one line, and GXLobbyChatCommand
		// reprints the same hint from /clear itself so a cleared box is never a dead end.
		//
		// GeneralsX @feature Claude 11/08/2026 ONE line instead of three paragraphs. The colour
		// legend that used to be the second paragraph is now `/help colours` - it is reference
		// material a player wants once, not something to re-read on every entry.
		if (!s_lobbyBannerPrinted)
		{
			s_lobbyBannerPrinted = TRUE;
			GXLobbyPrintHelpHint(GX_LOBBY_SCREEN_ONLINE, relaySocialSay);
		}

		// GeneralsX @feature Claude 08/08/2026 Say so, EVERY time, when the filter is still on.
		//
		// Outside the once-only block above on purpose. The filter survives leaving and re-entering
		// this screen, and a filtered browser with no friends online is indistinguishable from a
		// relay that has stopped answering - which is exactly the report that costs an evening.
		if (s_relayFriendsOnly)
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendsFilterStillOn",
				L"The game list is filtered to friends' games. Use the dropdown at the top to see "
				L"everything again."), GameSpyColor[GSCOLOR_MOTD]);
		}
	}
	else if (!TheGameSpyInfo->getCurrentGroupRoom())
	{
		if (groupRoomToJoin)
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - rejoining group room %d", groupRoomToJoin));
			TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
			groupRoomToJoin = 0;
		}
		else
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - joining best group room"));
			TheGameSpyInfo->joinBestGroupRoom();
		}
	}
	else
	{
		DEBUG_LOG(("WOLLobbyMenuInit() - not joining group room because we're already in one"));
	}

	GrabWindowInfo();

	TheGameSpyInfo->clearStagingRoomList();
	if (s_relayLobby)
	{
		// GeneralsX @feature Matchmaking. The relay's answer to PEERREQUEST_STARTGAMELIST. One
		// GXLIST out, one GXGAME datagram per game back, asynchronously on the transport's own
		// socket - relayLobbyUpdate notices them arriving.
		relayRequestGameList();
	}
	else
	{
		PeerRequest req;
		req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
		req.gameList.restrictGameList = TheGameSpyConfig->restrictGamesToLobby();
		TheGameSpyPeerMessageQueue->addRequest(req);
	}

	// animate controls
//	TheShell->registerWithAnimateManager(parent, WIN_ANIMATION_SLIDE_TOP, TRUE);
	TheShell->showShellMap(TRUE);
	TheGameSpyGame->reset();

	CustomMatchPreferences pref;
//	GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//	if (slider)
//	{
//		GadgetSliderSetPosition(slider, pref.getChatSizeSlider());
//		doSliderTrack(slider, pref.getChatSizeSlider());
//	}
//
	if (pref.usesLongGameList())
	{
		ToggleGameListType();
	}

	// Set Keyboard to chat window
	if (!s_relayLobby)
		TheWindowManager->winSetFocus( textEntryChat );
	raiseMessageBoxes = true;

	TheLobbyQueuedUTMs.clear();
	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);
	DontShowMainMenu = TRUE;

}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	isShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (nextScreen != nullptr) );

	if (nextScreen != nullptr)
	{
		TheShell->push(nextScreen);
	}

	nextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	s_signOutAsk = FALSE;	// GeneralsX @feature Claude 18/09/2026 nor out of this one
	s_signOutGo = FALSE;
	s_signOutResult = GX_SIGNOUT_NOT_SIGNED_IN;

	// GeneralsX @feature Claude 11/08/2026 The buddy overlay is an off-stack layout with its own
	// lifecycle, so nothing tears it down with this screen - it would otherwise stay on top of
	// whatever comes next, still holding keyboard focus. Closing is a no-op when it is not open.
	// (The CodeGui panel that used to get the same treatment here is deleted.)
	GXBuddyCloseRCMenu();
	GXBuddyOverlayClose();

	// GeneralsX @bugfix Claude 25/09/2026 QUITTING FROM A RELAY GAME ROOM CRASHED ON EXIT.
	//
	// This function runs TWICE for one visit in relay mode. Hosting or joining pushes the game
	// options screen, and Shell::push shuts this screen down first - that run saves the prefs and
	// then TearDownGameSpy() below nulls TheGameSpyInfo. The layout stays on the shell stack under
	// the game room, and at process exit Shell::deconstruct popImmediate()s every layout on the
	// stack, so this shutdown runs again with TheGameSpyInfo == nullptr.
	// CustomMatchPreferences' constructor read TheGameSpyInfo->getLocalProfileID() unguarded, and
	// the process died reading address 0 in ~GameEngine: every "quit from a room" on Windows and
	// macOS. The constructor now loads nothing when TheGameSpyInfo is null (Core/.../Common/
	// UserPreferences.cpp - inside the sourceID digest, so it ships in a digest batch, not with this
	// GameClient-only guard). This skip stays anyway: the second run has nothing to save, so it
	// should not build a prefs object at all.
	//
	// Skipping loses nothing: the first run already wrote these prefs, and the value cannot have
	// changed since (GetGameInfoListBox() is a constant nullptr). Everything else below is already
	// second-run safe -
	// the overlay/RC closers and ReleaseWindowInfo are idempotent, the GameSpy calls are guarded,
	// s_relayOwnsGameSpy was cleared by the first run, and nextScreen was cleared by its
	// shutdownComplete().
	if (TheGameSpyInfo != nullptr)
	{
		CustomMatchPreferences pref;
//		GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//		if (slider)
//		{
//			pref.setChatSizeSlider(GadgetSliderGetPosition(slider));
//		}
		if (GetGameInfoListBox())
		{
			pref.setUsesLongGameList(FALSE);
		}
		else
		{
			pref.setUsesLongGameList(TRUE);
		}
		pref.write();
	}

	ReleaseWindowInfo();

	if (TheGameSpyInfo)
		TheGameSpyInfo->unregisterTextWindow(listboxLobbyChat);

	//TheGameSpyChat->stopListingGames();
	if (TheGameSpyPeerMessageQueue)
	{
		PeerRequest req;
		req.peerRequestType = PeerRequest::PEERREQUEST_STOPGAMELIST;
		TheGameSpyPeerMessageQueue->addRequest(req);
	}

	listboxLobbyChat = nullptr;
	listboxLobbyPlayers = nullptr;
	GXLobbyChatRowReset();		// the box is going away; nothing may be continued into the next one

	// GeneralsX @feature Matchmaking. Give the presentation singletons back.
	//
	// They exist only for the duration of this screen. Leaving them alive would change what every
	// other part of the game sees: Shell::push and Shell::pop close all GameSpy overlays whenever
	// TheGameSpyInfo is non-null, and the score screen, the recorder and GameLogic all branch on it
	// to decide whether a match was an internet game. None of that should change because somebody
	// looked at the lobby once, and MainMenuInit's own teardown will not fire for us - it is gated
	// on the peer message queue, which we never create.
	//
	// This runs on the push into the game options screen too (Shell::push shuts the current screen
	// down first), which is exactly what we want: the match itself then runs with the same globals
	// the Direct Connect path has always used. Coming back re-creates them in Init.
	if (s_relayOwnsGameSpy)
	{
		TearDownGameSpy();
		s_relayOwnsGameSpy = FALSE;
	}
	s_relayLobby = FALSE;

	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	DontShowMainMenu = FALSE;

	RaiseGSMessageBox();
	TheTransitionHandler->reverse("WOLCustomLobbyFade");

}

static void fillPlayerInfo(const PeerResponse *resp, PlayerInfo *info)
{
	info->m_name			= resp->nick.c_str();
	info->m_profileID	= resp->player.profileID;
	info->m_flags			= resp->player.flags;
	info->m_wins			= resp->player.wins;
	info->m_losses		= resp->player.losses;
	info->m_locale		= resp->locale.c_str();
	info->m_rankPoints= resp->player.rankPoints;
	info->m_side			= resp->player.side;
	info->m_preorder	= resp->player.preorder;
}

#ifdef PERF_TEST
static const char* getMessageString(Int t)
{
	switch(t)
	{
		case PeerResponse::PEERRESPONSE_LOGIN:
			return "login";
		case PeerResponse::PEERRESPONSE_DISCONNECT:
			return "disconnect";
		case PeerResponse::PEERRESPONSE_MESSAGE:
			return "message";
		case PeerResponse::PEERRESPONSE_GROUPROOM:
			return "group room";
		case PeerResponse::PEERRESPONSE_STAGINGROOM:
			return "staging room";
		case PeerResponse::PEERRESPONSE_STAGINGROOMPLAYERINFO:
			return "staging room player info";
		case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
			return "group room join";
		case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
			return "staging room create";
		case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
			return "staging room join";
		case PeerResponse::PEERRESPONSE_PLAYERJOIN:
			return "player join";
		case PeerResponse::PEERRESPONSE_PLAYERLEFT:
			return "player part";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
			return "player nick";
		case PeerResponse::PEERRESPONSE_PLAYERINFO:
			return "player info";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
			return "player flags";
		case PeerResponse::PEERRESPONSE_ROOMUTM:
			return "room UTM";
		case PeerResponse::PEERRESPONSE_PLAYERUTM:
			return "player UTM";
		case PeerResponse::PEERRESPONSE_QUICKMATCHSTATUS:
			return "QM status";
		case PeerResponse::PEERRESPONSE_GAMESTART:
			return "game start";
		case PeerResponse::PEERRESPONSE_FAILEDTOHOST:
			return "host failure";
	}
	return "unknown";
}
#endif // PERF_TEST

//-------------------------------------------------------------------------------------------------

// GeneralsX @feature Claude 06/08/2026 One submit path for lobby chat, shared by the Enter key
// (GEM_EDIT_DONE) and the send button (ButtonEmote). They were separate in the stock code and that
// is how the button ended up doing nothing while typing appeared to work.
// GeneralsX @feature Claude 07/08/2026 Name for a roster slot, "" when that slot has not claimed
// one. Chat carries only the sender's virtual address, so this is how a line gets a name on it.
static UnicodeString relayNickFor( UnsignedInt lastOctet )
{
	// GeneralsX @feature Claude 08/08/2026 One roster lookup, shared with the ignore bookkeeping.
	// This used to walk the roster itself; relayRosterNickForSlot is the same walk and is the
	// version relaySlotIsIgnored has to agree with exactly, or a chat line could be labelled with
	// one name and matched against another.
	UnicodeString out;
	const char *nick = relayRosterNickForSlot(lastOctet);
	if (nick != NULL && nick[0] != '\0')
		out.translate(AsciiString(nick));
	return out;
}

/// A slot's display name, falling back to "P3" so a line is never labelled with nothing.
static UnicodeString relayLabelFor( UnsignedInt slot )
{
	UnicodeString out = relayNickFor(slot);
	if (out.isEmpty())
		out.format(L"P%d", (Int)slot);
	return out;
}

// ---------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 08/08/2026 Drain the unicast peer channel: private messages and friend
// cards. See the design block at the top of the social section for the message kinds.
//
// EVERYTHING HERE IS DEFENSIVE AND IT HAS TO BE. A peer line is composed entirely by another
// client - unlike a chat line, which the relay stamps a sender onto - so every field is hostile
// input. The sender's slot is the only thing not taken from the payload; it comes out of the relay
// routing header. A kind this build does not know is dropped in silence, so a peer cannot write
// arbitrary text into this chat box by inventing a verb.
// GeneralsX @feature Claude 11/08/2026 The card exchange's ONE writer, called by the overlay's
// right-click handler for every BY_SLOT action. Centralised here because every input it trusts -
// the roster, the latches, the slot->id bindings, relaySendPeerLine - is a static of this file,
// and the id-vs-name taxonomy above governs all of it. Contract, in order:
//   * the CURRENT roster nick must still equal the snapshot taken when the menu opened - a slot
//     recycled under an open menu acts on nothing, Decline included (a stale Decline could
//     otherwise clear a NEW occupant's fresh request);
//   * Offer and Accept require a non-empty local id (the CSPRNG-failure state means friends are
//     honestly unavailable) and arm their latch only AFTER the send call succeeds;
//   * Delete resolves the durable id AT ACTION TIME through the slot binding - a name-only
//     "friend" never reaches an id-keyed mutation - and routes through the social-command seam
//     so the refusal strings stay in one place;
//   * Decline sends nothing. Silence is the decline; nothing of ours is disclosed.
Bool GXRelayCardAction( Int action, Int slot, const AsciiString &nickSnapshot )
{
	if (slot <= 0 || slot > GX_ROOM_SLOTS || (UnsignedInt)slot == relayMySlot())
		return FALSE;

	Bool present = FALSE;
	const char *now = relayRosterNickForSlot((UnsignedInt)slot, &present);
	if (!present || strcmp(now, nickSnapshot.str()) != 0)
		return FALSE;

	if (action == GX_RC_ACT_IGNORE)
	{
		AsciiString line;
		line.format(relaySlotIsIgnored((UnsignedInt)slot) ? "/unignore P%d" : "/ignore P%d", slot);
		return GXRelayRunSocialCommand(line);
	}

	if (action == GX_RC_ACT_DELETE)
	{
		if (s_cardTheyAsked[slot])
		{
			s_cardTheyAsked[slot] = FALSE;
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendDeclined",
				L"Request dismissed. They were not told."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}
		char durableId[GX_LOBBY_ID_MAX];
		strlcpy(durableId, relaySlotDurableId((UnsignedInt)slot), sizeof(durableId));
		if (durableId[0] == '\0')
			return FALSE;
		AsciiString line;
		line.format("/remove %s", durableId);
		return GXRelayRunSocialCommand(line);
	}

	if (action == GX_RC_ACT_ADD)
	{
		const char *myId = GXPlayerIdentity::getPlayerId();
		if (myId == nullptr || myId[0] == '\0')
			return FALSE;

		if (s_cardTheyAsked[slot])
		{
			// Accepting DISCLOSES our card to whoever holds this slot right now. Nothing on this
			// side of the wire can prove the slot did not change hands while the request sat on
			// screen - see the declaration block - so the honest bound is a time-to-live on the
			// request itself. An old one is dropped rather than answered.
			if (s_cardTheyAskedMs[slot] == 0
					|| (timeGetTime() - s_cardTheyAskedMs[slot]) > (UnsignedInt)GX_CARD_REQUEST_TTL_MS)
			{
				s_cardTheyAsked[slot] = FALSE;
				s_cardTheirNonce[slot][0] = '\0';
				s_cardTheyAskedMs[slot] = 0;
				gxLobbyRequestRoster();
				relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendRequestStale",
					L"That request is too old to answer safely - the slot may belong to somebody "
					L"else by now. Ask them to send it again."), GameSpyColor[GSCOLOR_MOTD]);
				return FALSE;
			}

			// Accepting echoes THEIR nonce - the proof we are answering the exchange they
			// started, not one somebody inherited. A request from a build that sent no nonce
			// cannot be accepted this way; /add is the road for that peer.
			if (s_cardTheirNonce[slot][0] == '\0')
			{
				relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendOldRequest",
					L"That request came from an older build and cannot be answered with a click - "
					L"swap codes with /add instead."), GameSpyColor[GSCOLOR_MOTD]);
				return FALSE;
			}
			AsciiString body("fa ");
			body.concat(s_cardTheirNonce[slot]);
			body.concat(' ');
			body.concat(myId);
			body.concat(' ');
			body.concat(relayMyCardNick());
			if (!relaySendPeerLine((UnsignedInt)slot, body.str()))
				return FALSE;
			relayCardLatch((UnsignedInt)slot);
			s_cardWeAccepted[slot] = TRUE;
			s_cardTheyAsked[slot] = FALSE;
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendAccepted",
				L"Accepted - your code is on its way to them, theirs comes back the same way."),
				GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// GeneralsX @bugfix Claude 15/08/2026 Do not offer to somebody who is ALREADY a friend, and
		// do not offer twice. Reported from the live lobby: the buddy list showed the player and
		// the button still answered "Friend request sent".
		//
		// Keyed on the DURABLE ID, never on the nickname. A nickname-keyed check would silently
		// refuse a stranger who happens to have taken a friend's name, which is the same failure the
		// ignore list is careful to avoid one screen away. When we hold no id for the slot we have
		// genuinely never swapped codes with them, so the offer is allowed - that is the only way to
		// obtain one.
		const char *slotId = relaySlotDurableId((UnsignedInt)slot);
		if (slotId[0] != '\0' && GXPlayerIdentity::isFriend(slotId))
		{
			// No name interpolated on purpose: the player clicked a row that already shows it, and
			// the roster nickname is attacker-controlled text.
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendAlready",
				L"They are already on your friends list."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;			// handled - deliberately not FALSE, which reads as "button broken"
		}

		// An offer already in flight. Re-sending mints a NEW nonce, which invalidates the one the
		// other side is holding, so a second click would quietly break the exchange the first click
		// started rather than hurrying it along.
		if (s_cardWeAsked[slot]
				&& s_cardWeAskedMs[slot] != 0
				&& (timeGetTime() - s_cardWeAskedMs[slot]) <= (UnsignedInt)GX_CARD_REQUEST_TTL_MS)
		{
			relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendPending",
				L"You have already asked - waiting for them to accept."), GameSpyColor[GSCOLOR_MOTD]);
			return TRUE;
		}

		// Offering mints the exchange's nonce. A CSPRNG failure refuses the offer - a guessable
		// nonce is not a smaller version of this feature, it is the vulnerability again.
		if (!GXPlayerIdentity::mintNonce(s_cardOurNonce[slot], GX_CARD_NONCE_LEN))
			return FALSE;
		AsciiString body("fq ");
		body.concat(s_cardOurNonce[slot]);
		body.concat(' ');
		body.concat(relayMyCardNick());
		if (!relaySendPeerLine((UnsignedInt)slot, body.str()))
		{
			s_cardOurNonce[slot][0] = '\0';
			return FALSE;
		}
		relayCardLatch((UnsignedInt)slot);
		s_cardWeAsked[slot] = TRUE;
		s_cardWeAskedMs[slot] = timeGetTime();
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendAsked",
			L"Friend request sent. Nothing of yours is shared unless they accept."),
			GameSpyColor[GSCOLOR_MOTD]);
		return TRUE;
	}

	return FALSE;
}

// The overlay's window into the pending rows: slots whose occupant sent "fq", with the nick that
// was latched when it arrived. Count-then-at iteration; both re-walk the array, which at eight
// slots costs nothing and needs no cursor state to invalidate.
Int GXRelayPendingRequestCount( void )
{
	Int n = 0;
	for (Int s = 1; s <= GX_ROOM_SLOTS; ++s)
	{
		if (s_cardTheyAsked[s])
			++n;
	}
	return n;
}

Bool GXRelayPendingRequestAt( Int index, Int *outSlot, AsciiString *outNick )
{
	Int n = 0;
	for (Int s = 1; s <= GX_ROOM_SLOTS; ++s)
	{
		if (!s_cardTheyAsked[s])
			continue;
		if (n == index)
		{
			if (outSlot != nullptr)
				*outSlot = s;
			if (outNick != nullptr)
				*outNick = AsciiString(s_cardNick[s]);
			return TRUE;
		}
		++n;
	}
	return FALSE;
}

// GeneralsX @bugfix Claude 16/08/2026 Read from the carrier that shares a room with the roster.
//
// The counterpart to relaySendPeerLine's branch, and it has a second job that branch does not: every
// piece of state this parser touches is INDEXED BY SLOT - s_cardOurNonce, s_cardTheirNonce,
// s_cardWeAsked, the ignore list, relayBindSlotId's slot -> durable-id binding, s_lastDmFrom. Those
// arrays hold one meaning of "slot" at a time.
//
// So the two carriers are NOT drained together, and that is the whole point rather than an
// oversight. Draining both would put game-room slots and shared-room slots into one set of arrays,
// where a nonce minted for the lobby's P3 would be checked against a friend card from the game
// room's P3 - which is the bug this commit exists to remove, reintroduced one layer up. It is also
// why this differs deliberately from gxLobbyTakeChat, which does drain both: a chat line's sender
// number is used to LABEL it and nothing more, so a collision there misattributes a visible line
// instead of silently binding an identity to the wrong player.
//
// One channel at a time, chosen by the same predicate as the roster. A peer still on the previous
// build sends its cards on the game socket addressed with shared-room slots; those were already
// being delivered to the wrong player before this change, so declining to read them here loses
// nothing that ever worked.
static void relayDrainPeerLines()
{
	if (listboxLobbyChat == nullptr)
		return;

	const Bool viaPresence = gxHaveLobbyPresence();
	Transport *transport = viaPresence ? nullptr : relayTransport();
	if (!viaPresence && transport == nullptr)
		return;

	// The slot that means "me" in whichever room we are reading from. relayMySlot() already answers
	// this for the shared room; the game room needs the transport's own octet, because on the lobby
	// screen relayMySlot() would be the shared-room row and would silently eat another player's
	// private line whenever the two numbers happened to coincide.
	const UnsignedInt meSlot = viaPresence ? relayMySlot() : relayMyTransportSlot();

	char line[256];
	UnsignedInt from = 0;
	while (viaPresence
			? TheGXLobbyPresence->takePeerLine(line, sizeof(line), &from)
			: transport->takeRelayPeerLine(line, sizeof(line), &from))
	{
		if (from == 0 || from > (UnsignedInt)GX_ROOM_SLOTS || from == meSlot)
			continue;

		// An ignored player cannot reach us privately either. Checked BEFORE the kind is parsed, so
		// a blocked peer cannot use a friend request as a way to keep putting text on screen.
		if (relaySlotIsIgnored(from))
			continue;

		AsciiString rest(line);
		AsciiString kind;
		rest.nextToken(&kind);
		rest.trim();

		if (kind == "dm")
		{
			if (rest.isEmpty())
				continue;

			UnicodeString wideText;
			wideText.translate(rest);
			UnicodeString shown;
			shown.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayMsgRecvRow",
				L"[%ls, private to you] %ls"), relayLabelFor(from).str(), wideText.str());
			relaySocialSay(shown, GameMakeColor( 220, 160, 255, 255 ));

			// /r answers the most recent sender. Set AFTER the line is shown so a reply always
			// answers something the player has actually seen. The name is latched with it; see the
			// declaration for why a bare slot number would eventually reply to a stranger.
			s_lastDmFrom = from;
			strlcpy(s_lastDmNick, relayRosterNickForSlot(from), sizeof(s_lastDmNick));
			continue;
		}

		if (kind == "fq")
		{
			// The asking half, and it carries no id at all - so an unsolicited one costs the
			// recipient nothing but a line of text they can ignore.
			//
			// GeneralsX @feature Claude 11/08/2026 "fq <nonce> <nick>". The nonce is the
			// exchange's continuity proof (see the declaration block): our eventual "fa" echoes
			// it, and their "fi" must echo it back. Outstanding solicitations to this slot are
			// deliberately NOT cleared - an earlier draft cleared them to kill a nick-replay,
			// but the nonce closes that replay properly and clearing broke the legitimate
			// crossing (two people clicking Add on each other at once). A bare "fq <nick>" from
			// a build that predates nonces still shows its row; accepting it is refused for want
			// of a nonce, and /add remains the road for that peer.
			AsciiString maybeNonce;
			rest.nextToken(&maybeNonce);
			rest.trim();
			Bool nonceOk = (maybeNonce.getLength() == GX_CARD_NONCE_LEN);
			for (Int nc = 0; nonceOk && nc < GX_CARD_NONCE_LEN; ++nc)
			{
				const char ch = maybeNonce.getCharAt(nc);
				if (!((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7')))
					nonceOk = FALSE;
			}
			if (nonceOk)
			{
				strlcpy(s_cardTheirNonce[from], maybeNonce.str(), sizeof(s_cardTheirNonce[from]));
			}
			else
			{
				// Not a nonce: the whole remainder was the nick all along.
				s_cardTheirNonce[from][0] = '\0';
				if (!maybeNonce.isEmpty())
				{
					AsciiString whole = maybeNonce;
					if (!rest.isEmpty())
					{
						whole.concat(' ');
						whole.concat(rest);
					}
					rest = whole;
				}
			}
			s_cardTheyAsked[from] = TRUE;
			s_cardTheyAskedMs[from] = timeGetTime();
			// Ask the relay who is in the room now, so the row the player sees is as current as
			// the client can make it before they decide.
			gxLobbyRequestRoster();
			// GeneralsX @bugfix Claude 11/08/2026 LATCH THE ROSTER NAME, NEVER THE CARRIED ONE.
			// s_cardNick is not a label - it is the identity this exchange is recorded against.
			// relayCardPeerUnchanged compares it against the live roster on every enumeration and
			// clears all three card flags when they differ, and GXRelayCardAction refuses SILENTLY
			// on the same comparison. So storing the WIRE value here meant that any peer whose
			// carried nick differed at all from their roster nick - a rename in flight, a trimmed
			// or decorated name, or simply a peer who chose to send something else - turned ACCEPT
			// into a dead button that reported nothing at all, and a slot crossing tore the
			// exchange down.
			//
			// The carried nick is attacker-controlled and publicly visible - the same reasoning
			// that put a nonce on this protocol. The roster is what the relay says. Latch the
			// roster unconditionally; the carried nick is DISPLAY ONLY, and only while the roster
			// has nothing to show, because requestLobbyRoster() above is asynchronous.
			relayCardLatch(from);
			if (s_cardNick[from][0] == '\0' && !rest.isEmpty())
				strlcpy(s_cardNick[from], rest.str(), sizeof(s_cardNick[from]));
			UnicodeString shown;
			shown.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendRequestRecv",
				L"%ls would like to be friends. Open FRIENDS and right-click their name to answer, "
				L"or /add their code. Nothing of yours has been sent."), relayLabelFor(from).str());
			relaySocialSay(shown, GameSpyColor[GSCOLOR_MOTD]);
			continue;
		}

		if (kind == "fa" || kind == "fi")
		{
			// UNSOLICITED CARDS ARE DROPPED, and this test is the whole security of the exchange.
			// Without it any member of the lobby could put themselves on this player's friends
			// list - and from there into an id-keyed ignore decision and a "confirmed friend"
			// colour - by sending one datagram nobody asked for.
			//
			// relayCardPeerUnchanged is the second half of the same test and is not optional: the
			// pending state is keyed on a room SLOT, so without it the answer could come from
			// whoever inherited the slot of the person we actually asked.
			//
			// GeneralsX @feature Claude 11/08/2026 "fa <nonce> <id> <nick>" / "fi <nonce> <id>
			// <nick>". The nonce check is the REAL solicitation test: the flag says an exchange
			// is open, the nonce proves the answer comes from the endpoint the exchange was
			// started with - a recycled slot can echo a visible nickname but never a nonce that
			// was unicast to its previous occupant. relayCardPeerUnchanged stays as the
			// belt-and-suspenders roster check it always was.
			const Bool solicited = (kind == "fa") ? s_cardWeAsked[from] : s_cardWeAccepted[from];
			if (!solicited || !relayCardPeerUnchanged(from))
				continue;

			AsciiString echoedNonce;
			rest.nextToken(&echoedNonce);
			rest.trim();
			const char *expectedNonce = (kind == "fa") ? s_cardOurNonce[from] : s_cardTheirNonce[from];
			if (expectedNonce[0] == '\0' || strcmp(echoedNonce.str(), expectedNonce) != 0)
				continue;

			AsciiString theirId;
			rest.nextToken(&theirId);
			rest.trim();
			theirId.toUpper();
			if (!GXPlayerIdentity::isValidPlayerId(theirId.str()))
				continue;
			// Their own id must not equal ours. addFriend refuses it anyway, but refusing here as
			// well keeps the SLOT BINDING from being made - and a slot bound to our own id would
			// make us our own friend for colouring and, worse, a valid /msg destination.
			if (strcmp(theirId.str(), GXPlayerIdentity::getPlayerId()) == 0)
				continue;

			// GeneralsX @feature Claude 11/08/2026 On "fa" the REPLY GOES FIRST, and state only
			// mutates when the send call succeeds. The old order added them, cleared the latch,
			// then tried to send "fi" - a failed send left a one-sided exchange the peer could
			// never learn about and nothing could restart. Now a failed send leaves the latch
			// armed and nothing added, so clicking Add again replays the whole exchange cleanly.
			// Loss beyond the send call is the peer-line protocol's documented best-effort limit,
			// the same one "dm" lives with.
			if (kind == "fa")
			{
				const char *myId = GXPlayerIdentity::getPlayerId();
				if (myId == nullptr || myId[0] == '\0')
					continue;
				AsciiString body("fi ");
				body.concat(s_cardOurNonce[from]);	// the exchange's nonce, echoed back to them
				body.concat(' ');
				body.concat(myId);
				body.concat(' ');
				body.concat(relayMyCardNick());
				if (!relaySendPeerLine(from, body.str()))
					continue;
				s_cardWeAsked[from] = FALSE;
				s_cardOurNonce[from][0] = '\0';
				// If they had ALSO asked us (the crossing), this completion resolves that too -
				// the pending row retires rather than inviting a second, redundant exchange.
				s_cardTheyAsked[from] = FALSE;
			}
			else
			{
				s_cardWeAccepted[from] = FALSE;
				s_cardTheirNonce[from][0] = '\0';
			}

			relayBindSlotId(from, theirId.str());
			const Bool added = GXPlayerIdentity::addFriend(theirId.str(), rest.str());

			UnicodeString shown;
			shown.format(added
				? TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendCardDone",
					L"%ls is now on your friends list - codes swapped, so it is really them next time.")
				: TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayFriendCardSaveFailed",
					L"Swapped codes with %ls, but the friends list could not be written. Check that "
					L"GXPlayer.ini in your user data folder is writable."),
				relayLabelFor(from).str());
			relaySocialSay(shown, GameSpyColor[GSCOLOR_MOTD]);
			continue;
		}

		// Anything else is from a newer build. Dropped in silence, deliberately - see the note at
		// the top of this function.
	}
}

/// Drain a presence answer and a slow background poll, so the friends panel is warm rather than
/// blank when it is opened.
///
/// THE POLL IS SLOW ON PURPOSE. It is one datagram, but on a relay that does not speak GXSEEN it is
/// one datagram that gets dropped and rate-limit-logged, and the deployed relay is that relay. 30 s
/// matches the cadence Transport already uses for its own identity tags, for the same reason.
static void relayUpdatePresence()
{
	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	UnsignedInt mask = 0;
	Int count = 0;
	if (transport->takeRelayPresence(&mask, &count))
	{
		// Guarded against an answer wider than the question. The mask is positional and
		// s_presenceAsked has a fixed 16 entries, so a longer count would read past it.
		if (count > 0 && count <= 16 && count <= s_presenceAskedCount)
		{
			s_presenceMask = mask;
			s_presenceKnownCount = count;
			s_presenceAnsweredMs = timeGetTime();
		}
	}

	// GeneralsX @bugfix Claude 11/08/2026 A login or logout completed while this screen is open has
	// to reach the relay. Cheap: a strncmp against the last value declared, and a no-op unless it
	// actually moved. See relayRedeclareIdentityIfChanged.
	relayRedeclareIdentityIfChanged();

	static const UnsignedInt PRESENCE_POLL_MS = 30000;
	const UnsignedInt now = timeGetTime();
	if (GXPlayerIdentity::getFriendCount() > 0
		&& (s_presenceLastPollMs == 0 || (now - s_presenceLastPollMs) >= PRESENCE_POLL_MS))
	{
		relayPollPresence();
		// Stamped here as well as inside relayPollPresence, and it is not redundant: that function
		// leaves the stamp alone when the send FAILS, so without this a relay that is refusing
		// everything would be re-polled on every single frame.
		s_presenceLastPollMs = now;
	}
}

/// GeneralsX @bugfix Claude 23/09/2026 A signed-out lobby transport the relay no longer accepts gets a NEW
/// id, the way this screen's entry gets one - enterRelayRoom, then SetLocalIP - instead of waiting for the
/// player to leave and come back (Codex r3 on 3d0cefbfc). See Transport::isRelayIdentityStranded for why
/// the transport cannot do this itself and why nothing the sign-out discarded comes back.
///
/// Only with no game on this TheLAN and nothing in flight: a LANGameInfo holds its own copy of the address,
/// so a new one would split the game's identity from the socket's. At most one attempt per 30 s - the
/// presence socket's GX_GAVEUP_RETRY_MS - because an unanswered enterRelayRoom blocks this thread for its
/// DNS lookup and its retries (5 x 400 ms), exactly as this screen's own entry does in an outage.
///
/// GeneralsX @bugfix Claude 23/09/2026 ...AND DOUBLED ON EVERY UNANSWERED ONE, to 5 min (Claude r6). Silence is
/// the whole signal, and a relay outage - or a flood draining relay.js's shared REPLY_BUDGET_PER_SEC, which
/// anyone can send - is silence too: at a flat 30 s that was a 2 s freeze of this screen every 30 s for as
/// long as it lasted. An answer resets it. What a forged trigger can buy is bounded besides: an answered
/// re-entry is a NEW id, never the signed-out one, so it is never stranded again - one id change per sign-out.
static void relayReacquireIfStranded()
{
	static const UnsignedInt REACQUIRE_RETRY_MS = 30000;
	static const UnsignedInt REACQUIRE_MAX_MS = 300000;
	static UnsignedInt s_nextReacquireMs = 0;
	static UnsignedInt s_reacquireWaitMs = REACQUIRE_RETRY_MS;

	Transport *transport = relayTransport();
	if (transport == nullptr || !(transport->isRelayIdentityStranded() || s_reacquireRebindOwed))
		return;
	// hasPendingAction: a same-room browser join sets no flag here (see relayJoinSelectedGame), and a new id
	// mid-handshake leaves the host holding the old one (Codex r4).
	if (s_tryingToHostOrJoin || isShuttingDown || TheLAN->GetMyGame() != nullptr || TheLAN->hasPendingAction())
		return;
	const UnsignedInt now = timeGetTime();
	if (s_nextReacquireMs != 0 && (Int)(now - s_nextReacquireMs) < 0)
		return;
	s_nextReacquireMs = now + s_reacquireWaitMs;

	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();
	const AsciiString room = transport->getRelayRoom();
	UnsignedInt ip = 0;
	if (relayHost.isEmpty() || !Transport::enterRelayRoom(relayHost.str(), room.str(), ip))
	{
		s_reacquireWaitMs = min(s_reacquireWaitMs * 2, REACQUIRE_MAX_MS);
		return;
	}
	s_reacquireWaitMs = REACQUIRE_RETRY_MS;
	// GeneralsX @bugfix Claude 23/09/2026 THE DEADLINE TOO (Codex on 5db38bb2f): it was stamped above with the
	// interval an earlier outage had raised, up to 5 min, so a failed rebind below - which re-arms this very
	// path - waited that out instead of the 30 s an answer resets to.
	s_nextReacquireMs = now + s_reacquireWaitMs;

	fprintf(stderr, "[GXRELAY] lobby id unanswered since sign-out; re-entering room '%s' as %d.%d.%d.%d\n",
		room.str(), PRINTF_IP_AS_4_INTS(ip));
	fflush(stderr);
	// A failed bind has already deleted the old socket, and initRelay zeroed the count: nothing would ever
	// ask again. So the failure itself is what re-arms the next attempt, 30 s on (Codex r4).
	s_reacquireRebindOwed = !TheLAN->SetLocalIP(ip);
	if (s_reacquireRebindOwed)
		return;
	relayDeclareIdentity();		// the durable id now, not on the 30 s tag tick; a no-op identity when signed out
}

// GeneralsX @feature Claude 07/08/2026 Apply a nickname to every identity at once.
//
// Three separate identities existed and nothing kept them in step:
//   * the relay-held nickname  - the lobby browser's player list and lobby chat
//   * the LAN slot-list name   - GAME OPTIONS and in-match chat, set by RequestSetName
//   * the persisted preference - what you are called on the next launch
// /nickname used to set only the first, which is exactly why a name chosen in the lobby
// disappeared the instant you joined a game and the slot list reverted to the machine name.
//
// Deliberately not gated on being in a relay room: the LAN name and the preference are worth
// setting even when the relay half cannot be, so the command does something useful on the
// game-options screen and in a plain LAN game too.
// GeneralsX @refactor Claude 11/08/2026 WIDE in, normalised ONCE, one value drives everything.
//
// Three changes travelling together, each forced by a real defect:
//   * The parameter is the WIDE remainder. Every caller used to tokenise the ASCII translation of
//     the chat line, and AsciiString::translate casts each WideChar to char - a non-ASCII name was
//     mangled before this function could even see it. Callers now use GXLobbyTakeNickRest.
//   * Normalisation lives HERE, not in the callers, so no two surfaces can disagree: no leading or
//     trailing spaces, none of the , : ; the LAN packet format tokenises on (the LAN name box
//     removes those the keystroke after they appear - same net rule, applied once), and at most
//     g_lanPlayerNameLength characters, exactly the box's truncateTo. Characters past the cap are
//     dropped unseen, as the box drops them. Non-ASCII among the kept characters is REFUSED, not
//     narrowed - every consumer below except RequestSetName is byte-oriented.
//   * outApplied hands the one normalised value back, because the LAN BROWSER's handler must put
//     it in the name box - LanLobbyMenuShutdown writes the pref FROM that box on the way out.
Bool GXApplyNickname( const UnicodeString &requested, UnicodeString &outNote,
					  UnicodeString *outApplied )
{
	WideChar buf[g_lanPlayerNameLength + 1];
	Int kept = 0;
	const WideChar *c = requested.str();
	while (c != NULL && *c == L' ')
		++c;
	for (; c != NULL && *c != L'\0' && kept < g_lanPlayerNameLength; ++c)
	{
		if (*c == L',' || *c == L':' || *c == L';')
			continue;
		// GeneralsX @feature Claude 11/08/2026 '@' is RESERVED for the role decoration - a moderator
		// renders as <@Name> and an admin as @Name - so a name that could contain one would let
		// anyone who can type forge the marker.
		//
		// REFUSED HERE AS WELL AS AT THE RELAY, and the "as well as" is the whole point. The relay's
		// NICK_RE has always dropped it, but silently and only from its own copy: `/nick bob@evil`
		// SUCCEEDED, the LAN slot list and LANMessage.name[] carried `bob@evil`, the roster showed
		// `bobevil`, and the player was told the name was set. That is the difference between a rule
		// and a surprise, and it is why this is a refusal rather than one more character to strip.
		//
		// Checked BEFORE the > 127 test only because both are refusals and this one is cheaper to
		// explain; neither ordering changes what is accepted.
		if (*c == L'@')
		{
			outNote = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNickReserved",
				L"Names cannot contain @ - it marks moderators and admins.");
			return FALSE;
		}
		if (*c > 127)
		{
			outNote = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNickAscii",
				L"Names are plain characters for now - no accents or non-Latin letters.");
			return FALSE;
		}
		buf[kept++] = *c;
	}
	while (kept > 0 && buf[kept - 1] == L' ')
		--kept;
	buf[kept] = L'\0';

	if (kept == 0)
	{
		outNote = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNickUsage",
			L"Usage: /nick <name>   (up to 12 plain characters)");
		return FALSE;
	}

	const UnicodeString wide(buf);
	AsciiString narrow;
	narrow.translate(wide);		// safe: anything above 127 was refused above

	// GeneralsX @bugfix Claude 15/09/2026 The relay's refusal, carried back to whichever screen
	// asked - plan G, item G-1. Empty means the relay-held nickname was claimed, or was never
	// attempted at all because TheLAN is null. Non-empty is the clause appended to outNote below,
	// and every /nick call site prints outNote through its own sink, so this is how the message
	// reaches the two LAN screens as well as this one.
	UnicodeString relayRefusal;

	if (TheLAN != nullptr)
	{
		// Slot list + the periodic re-announce, so GAME OPTIONS shows it.
		TheLAN->RequestSetName(wide);
		// Relay-held, so the browser and anyone joining later sees it without a refresh. May
		// legitimately fail when we are not on a relay; that does not make the command a failure.
		// &relayRefusal: the player typed this one line ago and is looking at a chat box, so a name
		// the relay cannot hold has to be said out loud rather than dropped.
		gxLobbySetNickname(narrow.str(), &relayRefusal);
		gxLobbyRequestRoster();
	}

	// Persist, so it survives a restart rather than reverting to the machine name.
	//
	// GeneralsX @bugfix Claude 07/08/2026 QUOTED-PRINTABLE, not the raw string.
	//
	// LANPreferences::getUserName() reads this key back through
	// QuotedPrintableToUnicodeString (LanLobbyMenu.cpp), so a name written raw is decoded as if it
	// were encoded, and comes back as unrenderable code points - the GAME OPTIONS slot showed a
	// three-character name as three empty boxes. Every other writer of this key in the tree
	// (LanLobbyMenu, NetworkDirectConnect, SkirmishGameOptionsMenu) already encodes it; this one
	// did not, so the name only looked right until something read it back.
	LANPreferences prefs;
	prefs["UserName"] = UnicodeStringToQuotedPrintable(wide);
	prefs.write();

	// GeneralsX @bugfix Claude 15/09/2026 ONE note, telling the truth about BOTH halves - G-1.
	//
	// The relay-held nickname is the only half that can fail here, and it fails without stopping
	// anything: RequestSetName has already run and the preference is already written, so the LAN
	// slot list, GAME OPTIONS and in-match chat carry `wide` either way. That is why this still
	// returns TRUE and still fills outApplied on a refusal - the LAN browser's name box has to
	// follow the name that WAS set, or LanLobbyMenuShutdown writes the old one straight back over
	// the preference above.
	//
	// APPENDED, NOT FORMATTED THROUGH. The success line is the format string, as it always was;
	// the refusal is a plain clause concatenated after it. Running a fetched, translatable string
	// through vswprintf as a format is a crash waiting for a bad .csf entry, and there is no reason
	// to widen that surface for a clause that needs no substitution of its own.
	outNote.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayNickSet",
		L"You are now known as %ls"), wide.str());
	if (!relayRefusal.isEmpty())
	{
		outNote.concat(L" ");
		outNote.concat(relayRefusal);
	}
	if (outApplied != NULL)
		*outApplied = wide;
	return TRUE;
}

static void relaySubmitChat( void )
{
	if (!textEntryChat)
		return;

	UnicodeString txtInput;
	txtInput.set(GadgetTextEntryGetText( textEntryChat ));
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
	txtInput.trim();
	if (txtInput.isEmpty())
		return;

	AsciiString ascii;
	ascii.translate(txtInput);

	// GeneralsX @feature Claude 11/08/2026 /help, /clear, and the verbs this build has retired.
	//
	// FIRST, and that ordering is load-bearing rather than tidy: three of the retired verbs
	// (/pass, /nopassword, /lobbyhelp) are still recognised DOWNSTREAM by
	// LANAPI::applyLobbyCommand, which this branch deliberately does not edit because LANAPI.cpp
	// is inside the SimID source digest. Intercepting above it is what retires them without
	// forcing an atomic four-platform rebuild.
	if (GXLobbyChatCommand(ascii, GX_LOBBY_SCREEN_ONLINE, listboxLobbyChat, relaySocialSay))
	{
		GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
		TheWindowManager->winSetFocus( textEntryChat );
		return;
	}

	// GeneralsX @feature Claude 21/08/2026 One chat line every ChatRate::MIN_INTERVAL_MS, shared
	// with every other chat box in the game - see GameClient/ChatRateLimit.h. Placed BELOW the
	// slash-command interception on purpose, so /help and /clear cost no allowance, and the
	// refused line goes back in the box rather than being dropped.
	//
	// The notice goes through relaySocialSay rather than the relay, for the same reason the
	// send-failure notice above does: your own client telling you to slow down is a local event
	// and must not be broadcast to the room.
	{
		unsigned int gxChatWait = 0;
		if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait))
		{
			GadgetTextEntrySetText(textEntryChat, txtInput);
			TheWindowManager->winSetFocus( textEntryChat );
			relaySocialSay(
				TheGameText->FETCH_OR_SUBSTITUTE_FORMAT("GUI:ChatRateLimited",
					L"Please wait %d more second(s) before sending another message.",
					(Int)gxChatWait),
				GameSpyColor[GSCOLOR_MOTD]);
			return;
		}
	}

	// GeneralsX @feature Claude 07/08/2026 /nick <name> - claim a display name.
	//
	// Sent to the RELAY, which holds it on the member record and returns it with the roster. That
	// is what makes it stick without anyone refreshing: a name broadcast peer-to-peer is only
	// heard by whoever is already in the room, so late arrivals would never learn it and everyone
	// would have to keep re-announcing. One GXNICK, and every peer that asks for the roster from
	// then on - including peers who have not joined yet - gets the name for free.
	//
	// Handled here rather than in handleLobbySlashCommands because that helper is the GameSpy
	// path: it queries the QR2 hosting state and writes to the peerchat queue, neither of which
	// exists on a relay.
	if (ascii.getCharAt(0) == '/')
	{
		AsciiString rest = ascii.str() + 1;
		AsciiString verb;
		rest.nextToken(&verb);
		verb.toLower();
		// GeneralsX @refactor Claude 11/08/2026 The name comes off the WIDE line - see
		// GXLobbyTakeNickRest. The ascii tokens above still dispatch every other verb.
		{
			UnicodeString nickRest;
			if (GXLobbyTakeNickRest(txtInput, nickRest))
			{
				UnicodeString note;
				GXApplyNickname(nickRest, note);
				relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
				GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				TheWindowManager->winSetFocus( textEntryChat );
				return;
			}
		}

		// GeneralsX @feature Claude 08/08/2026 Friends and ignore. Handled before the send below
		// for the same reason /nick is - and here it also matters that an unrecognised line used to
		// be BROADCAST: see the note on relayHandleSocialCommand.
		if (relayHandleSocialCommand(verb, rest))
		{
			GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
			TheWindowManager->winSetFocus( textEntryChat );
			return;
		}

		// GeneralsX @feature Claude 08/08/2026 /password, /private, /public. The relay
		// lobby is the screen a player hosts and joins from, so it is where these have to be
		// typeable; the LAN screens get the same commands for free because LANAPI::RequestChat
		// intercepts them, and this screen does not go through RequestChat.
		//
		// This MUST come before sendLobbyChat below. That call puts the line on the relay, and a
		// password that reaches the relay has been disclosed to every member of the room - which
		// is the exact observer this feature exists to keep it away from.
		//
		// Ordered after the social commands only because the two verb sets are disjoint and this
		// one is the more expensive miss; neither may fall through to the send.
		if (TheLAN != nullptr)
		{
			UnicodeString note;
			if (TheLAN->applyLobbyCommand(ascii, note))
			{
				// GeneralsX @bugfix Claude 13/08/2026 Through the sink, not around it. This is a
				// line this client produced for this player alone - the definition of [S] - and it
				// was the one such line printing bare, because it wrote to the listbox directly.
				// Going around the sink also meant it landed between a block's header and its
				// continuations without closing the block.
				relaySocialSay(note, GameSpyColor[GSCOLOR_MOTD]);
				GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				TheWindowManager->winSetFocus( textEntryChat );
				return;
			}
		}

		// GeneralsX @feature Claude 11/08/2026 A '/' line nothing recognised is REFUSED, not sent.
		//
		// THIS IS WHAT MAKES RETIRING AN ALIAS SAFE. sendLobbyChat below publishes to every member
		// of the room, so without this "/w bob meet me at the back" - a private-message verb this
		// build no longer has - is said out loud to everyone, which is the exact failure /msg
		// exists to prevent, reached by a rename. // is the escape for a line that really starts
		// with a slash, and it rewrites `ascii` in place.
		Bool escaped = FALSE;
		if (GXLobbyGuardUnknownCommand(ascii, GX_LOBBY_SCREEN_ONLINE, relaySocialSay, escaped))
		{
			GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
			TheWindowManager->winSetFocus( textEntryChat );
			return;
		}

		// The local echo below must show what was actually SENT, which after a // escape is one
		// character shorter than what was typed.
		//
		// Trimmed on the WIDE string rather than re-derived from `ascii`. AsciiString::translate
		// casts each WideChar to char - its own @todo says "will only work for 7-bit ascii" - so
		// `txtInput.translate(ascii)` would flatten every accented, Cyrillic, Arabic or CJK line
		// the player echoes back to themselves.
		if (escaped)
			txtInput = UnicodeString(txtInput.str() + 1);
	}
	if (!gxLobbySendChat(ascii.str()))
	{
		// Say so rather than swallowing it. A line that vanishes with no trace is the failure the
		// operator hit, and "nothing happened" is indistinguishable from "the relay ate it".
		// Through the sink for the same reason as the note above: your client telling you alone
		// that the send failed is [S], and it must close any block it lands after.
		relaySocialSay(
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyChatFailed",
				L"Could not send - not connected to a relay room."),
			GameSpyColor[GSCOLOR_MOTD]);
		return;
	}

	// Echoed locally because the relay sends GXSAY to every OTHER member of the room - the sender
	// never receives their own line.
	// Prefer the relay-held nickname, so your own lines read the same way everyone else sees them.
	//
	// GeneralsX @bugfix Claude 15/08/2026 Go through relayMySlot(), do NOT re-derive the slot here.
	// This line had its own inlined copy of the pre-fix relayMySlot() body, so it asked the GAME
	// transport which slot we are while the roster it then indexes comes from the shared room. That
	// is what put another player's name on our own echoed chat lines. There must be exactly one
	// answer to "which slot am I" on this screen, and relayMySlot() is it.
	UnicodeString myName = relayNickFor(relayMySlot());
	if (myName.isEmpty())
		myName = TheLAN->GetMyName();
	if (myName.isEmpty())
		myName = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyChatMe", L"me");
	// GeneralsX @feature Claude 11/08/2026 Timestamped like every other line. Your own echo is
	// decorated with YOUR role too - the relay knows it, and a moderator who cannot see their own
	// marker has no way to tell whether it is working.
	UnicodeString mine = GXLobbyTimestamp();
	mine.concat(GXLobbySaid(
		GXLobbyDecorateName(myName, GXPlayerIdentity::getVerifiedRole()), txtInput));
	// Empty prefix for the same reason as an incoming player line: this is somebody talking to the
	// room - you - and it must not fold into a [S] block or be folded into by one.
	mine = GXLobbyChatRow(listboxLobbyChat, UnicodeString::TheEmptyString, mine);
	TheGameSpyInfo->addText(mine, GameSpyColor[GSCOLOR_CHAT_OWNER], listboxLobbyChat);

	// GeneralsX @bugfix Claude 06/08/2026 Put the caret back in the box.
	//
	// Submitting drops focus, so on iPadOS the on-screen keyboard closes and the entry stops
	// accepting input - the operator had to tap another control and tap back to type a second
	// message. On desktop it is barely noticeable because the field usually keeps keyboard focus
	// anyway; on a touch device it makes the box look broken after exactly one line.
	// The screen already does this on entry (winSetFocus at init), so this is the same call at the
	// one other moment focus is lost.
	if (textEntryChat)
		TheWindowManager->winSetFocus( textEntryChat );
}

/** refreshGameList
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
void refreshGameList( Bool forceRefresh )
{
	Int refreshInterval = gameListRefreshInterval;

	if (forceRefresh || ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime())))
	{
		if (TheGameSpyInfo->hasStagingRoomListChanged())
		{
			//DEBUG_LOG(("################### refreshing game list"));
			//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
			RefreshGameListBoxes();
			gameListRefreshTime = timeGetTime();
		} else {
			//DEBUG_LOG(("-"));
		}
	} else {
		//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
	}
}
//-------------------------------------------------------------------------------------------------
/** refreshPlayerList
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
/* GeneralsX @bugfix Claude 20/09/2026 (owner) THE ROSTER AS IT IS RENDERED, as one number.
 *
 * Deliberately the same device the GAME list already uses a few hundred lines up (s_relayListLastBuilt):
 * an FNV-1a over exactly the fields that are drawn, so the screen rebuilds when its CONTENT changes and
 * not merely when the number of rows does. A count alone would miss one player leaving as another joins,
 * which is the defect that pattern was introduced to fix for games.
 *
 * s_relayHeardFromRelay is mixed in because it is drawn too: it is the difference between an empty list
 * and "Checking who's here...". Without it, a relay that answers with nobody in the room leaves the
 * placeholder up until the next tick, because an empty roster hashes to the same basis as no roster. */
static UnsignedInt gxLobbyRosterPrint( void )
{
	UnsignedInt print = 2166136261u;
	UnsignedInt ips[GX_ROOM_SLOTS];
	const Int n = gxLobbyRoster(ips, GX_ROOM_SLOTS);
	for (Int i = 0; i < n; ++i)
	{
		for (Int shift = 0; shift < 32; shift += 8)
			print = (print ^ ((ips[i] >> shift) & 0xFFu)) * 16777619u;

		const char *bytes[3] = { gxLobbyRosterNick(i), gxLobbyRosterOS(i), gxLobbyRosterRole(i) };
		for (Int b = 0; b < 3; ++b)
		{
			for (const char *c = (bytes[b] != NULL) ? bytes[b] : ""; *c != '\0'; ++c)
				print = (print ^ (UnsignedInt)(unsigned char)*c) * 16777619u;
			print = (print ^ 0xFFu) * 16777619u;	// field terminator
		}
	}
	return (print ^ (s_relayHeardFromRelay ? 1u : 0u)) * 16777619u;
}

void refreshPlayerList( Bool forceRefresh )
{
		Int refreshInterval = playerListRefreshInterval;

		/* GeneralsX @bugfix Claude 20/09/2026 (owner) A 5 SECOND REPAINT WAITING ON A 0.21 SECOND ANSWER.
		 *
		 * This gate was the timer alone. WOLLobbyMenuInit zeroes playerListRefreshTime, so the FIRST paint
		 * happened on the screen's first frame - necessarily before any relay answer could exist - and the
		 * second one a full 5 s later. Measured from this Mac against production on 20/09/2026: admission
		 * 122 ms, roster 88 ms median, so 210 ms of data sat behind a 5000 ms repaint and the player read
		 * "Checking who's here..." for the other 4.8 s. The owner reported it as the lobby being slow to
		 * show who is around, and it was - the data had arrived.
		 *
		 * So repaint when the roster CHANGES as well as on the timer. The presence socket refreshes on its
		 * own 4 s timer, and the screen's 5 s one beat against it, which is where the measurement's "up to
		 * ~9 s stale" came from; keying on content removes that term instead of tuning it.
		 *
		 * THE TIMER STAYS, as the backstop. PopulateLobbyPlayerListbox draws more than the roster, and on
		 * the LAN path it is also what asks for one (gxLobbyRequestRoster) - on the relay path that call is
		 * a no-op, so this costs no extra datagrams there. On the LAN path a change buys exactly one extra
		 * request and then the print matches, so it settles rather than looping. */
		const UnsignedInt print = gxLobbyRosterPrint();

		if (forceRefresh ||((playerListRefreshTime == 0) || (print != s_playerListPrint)
			|| ((playerListRefreshTime + refreshInterval) <= timeGetTime())))
		{
				PopulateLobbyPlayerListbox();
				playerListRefreshTime = timeGetTime();
				s_playerListPrint = print;
		}
}
//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu update method */
//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 11/08/2026 The retail buddy overlay, opened DIRECTLY on the relay path.
//
// GameSpyOpenOverlay is not usable here: it dereferences TheGameSpyBuddyMessageQueue->isConnected()
// before doing anything, and the relay path deliberately never creates that queue. It also parks
// its layout in overlayLayouts[], whose entries alone receive runUpdate() and managed destruction -
// so a layout created OUTSIDE that table must own its whole lifecycle, and this one does: created
// + runInit on open, runUpdate pumped from WOLLobbyMenuUpdate every frame it exists, runShutdown
// then destroyWindows + deleteInstance on close - the same sequence GameSpyCloseOverlay performs
// for the table's layouts. gx mode is set BEFORE runInit and cleared AFTER destruction, so both
// halves run under the mode they were opened in and a later GameSpy-path overlay in the same
// process cannot inherit gx behaviour. The overlay is lobby-scoped by design: this screen's
// Shutdown and the Shell transitions close it, exactly as they close the friends panel.
static WindowLayout *s_gxOverlay = nullptr;

Bool GXBuddyOverlayIsOpen( void )
{
	return (s_gxOverlay != nullptr);
}

void GXBuddyOverlayOpen( void )
{
	if (s_gxOverlay != nullptr)
	{
		s_gxOverlay->bringForward();
		return;
	}
	GXBuddyOverlaySetGxMode(TRUE);
	s_gxOverlay = TheWindowManager->winCreateLayout("Menus/WOLBuddyOverlay.wnd");
	if (s_gxOverlay == nullptr)
	{
		// The layout ships inside WindowZH.big; failing to load it means the data is damaged.
		// Clear the mode rather than leaving the next GameSpy overlay poisoned by it.
		GXBuddyOverlaySetGxMode(FALSE);
		return;
	}
	s_gxOverlay->runInit();
	s_gxOverlay->hide(FALSE);
	s_gxOverlay->bringForward();
}

void GXBuddyOverlayClose( void )
{
	// An RC menu opened FROM the overlay is an independent lone-window layout; closing the
	// overlay without it leaves an actionable menu floating over nothing (codex r2 of the diff
	// review - the Escape path reached here without the transition closers running).
	GXBuddyCloseRCMenu();
	if (s_gxOverlay == nullptr)
		return;
	// Nulled FIRST: runShutdown paths can re-enter close (the Shell transitions and this screen's
	// own Shutdown both call it), and the second entry must see "already closed".
	WindowLayout *layout = s_gxOverlay;
	s_gxOverlay = nullptr;
	layout->runShutdown();
	layout->destroyWindows();
	deleteInstance(layout);
	GXBuddyOverlaySetGxMode(FALSE);
}

void GXBuddyOverlayToggle( void )
{
	if (GXBuddyOverlayIsOpen())
		GXBuddyOverlayClose();
	else
		GXBuddyOverlayOpen();
}

void GXBuddyOverlayPump( void )
{
	if (s_gxOverlay == nullptr)
		return;
	// GeneralsX @bugfix Claude 25/09/2026 Presence follows the overlay out of the lobby (codex-B P2). The lobby
	// screen polls it (WOLLobbyMenuUpdate); in a game room or a match that screen is gone, so while the overlay
	// is open there, poll from here - GXSEEN needs no room (Transport::requestRelayPresence) and this stays the
	// one takeRelayPresence consumer. Not while the lobby screen runs its own, so nothing polls twice.
	if (!(s_relayLobby && listboxLobbyChat != nullptr && TheLAN != nullptr))
		relayUpdatePresence();
	s_gxOverlay->runUpdate();
}

// ONLINE ONLY (owner, 25/09/2026): the relay lobby, a relay game room, a relay match. Offline - skirmish,
// campaign, replay, a plain LAN match, the main menu - the key falls through to the meta map and a bare F5
// recalls View 5 as it always did. The rule and why each fact is needed: GameClient/GXFriendsKey.h.
Bool GXFriendsKeyHere( void )
{
	const Bool relaying = relayTransport() != nullptr;
	return GXFriendsKeyOnline(
		TheShell != nullptr && TheShell->isShellActive(),
		s_relayLobby,
		relaying,
		relaying && TheLAN->GetMyGame() != nullptr,
		TheGameLogic != nullptr && TheGameLogic->isInMultiplayerGame(),
		TheNetwork != nullptr);
}

// GeneralsX @feature Claude 18/09/2026 SIGN-OUT FROM THE PLAYER'S OWN NAME. See GUICallbacks.h.
//
// Two flags, drained one frame apart, so no message box is ever opened from inside another
// window's callback (the wedge MainMenu.cpp documents) and the confirmation's Yes never does the
// work itself. Both are cleared on screen entry AND exit: a request raised just before the lobby
// went away must not fire on the next visit, when the player has long forgotten asking.
// (s_signOutAsk / s_signOutGo are declared beside s_tryingToHostOrJoin, above WOLLobbyMenuInit,
// because Init and Shutdown clear them.)

void GXLobbyAskSignOut( void )
{
	s_signOutAsk = TRUE;
}

/// THE SIGN-OUT HAPPENS HERE, AT THE CLICK - only the window work waits a frame.
///
/// Everything below touches files, memory and a socket, and no window, so it is safe inside a
/// message box's callback: the wedge MainMenu.cpp documents is opening ANOTHER box from one, and
/// the shell must not be popped from one either - both of those are left to gxDrainSignOut.
///
/// Doing the work here rather than there is what makes Yes mean yes. The lobby can go away while
/// this box is open - the relay connection drops, and the screen is popped underneath it - and a
/// Yes that only raised a flag would then be drained by nobody: the player confirmed, and stayed
/// signed in, and was told nothing. Now the sign-in is gone the moment they confirm, whatever
/// screen is left to report it.
static void gxSignOutYesCallback( void )
{
	// ONLY WHILE THIS LOBBY IS STILL THE SCREEN (cross-model review, 18/09/2026). A relay join does
	// not set s_tryingToHostOrJoin - deliberately, see relayJoinSelectedGame - so one already under
	// way can complete while this box is open, and the box then outlives the lobby over the GAME
	// ROOM. Signing out there is exactly what /logout refuses to do in a relay room: the relay would
	// be told to forget the account, and may remove the player from a room others are waiting in.
	// So a Yes that arrives after the lobby has gone does nothing at all - the question was about
	// leaving a lobby that no longer exists, and the player can sign out from the next one. The
	// drain below applies the same test, which is also what keeps its simulated Back away from a
	// screen that is animating out (WOLLobbyMenuSystem ignores input then anyway).
	if (!s_relayLobby || isShuttingDown || s_tryingToHostOrJoin)
		return;

	s_signOutResult = GXLobbySignOut();

	// Bring this screen's own bookkeeping up to date now rather than on a tick that may not come.
	// It keeps only a fingerprint of what it last declared (never the token), and the transport's
	// copy was emptied by GXLobbySignOut, so this sends nothing a second time. No window is touched.
	relayRedeclareIdentityIfChanged();

	s_signOutGo = TRUE;
}

static void gxSignOutNoCallback( void )
{
	// Nothing was done, so there is nothing to undo.
}

/// WHAT SIGNING OUT MID-SESSION DOES, decided rather than left to fall out of the code:
///
/// It is offered ONLY here, on the player's own name in the Online lobby - never inside a game
/// room (where /logout refuses too) and never during a match, which has no control for it. So no
/// match and no room other players are waiting in can be touched by it.
///
/// It signs out AND LEAVES THIS LOBBY, exactly as Back and ESC do. Staying would leave the relay
/// holding a member with no identity, and what the relay does with one depends on
/// RELAY_REQUIRE_IDENTITY - tolerate it, or remove it from the room. Leaving makes the outcome the
/// same whatever the server is configured to do. The player lands where they came from, and Online
/// then asks "Sign in?" - which is how signing back in (and so a fresh assertion, carrying the
/// account's current role) is reached.
///
/// If the sign-in file could not be deleted, it STAYS, and says so in the chat: the player is signed
/// out for this session but will be signed back in next launch, and a screen that closed on that
/// would take the only place the explanation could be read.
///
/// The sign-out itself is done by gxSignOutYesCallback at the click; this only does what needs a
/// window - show the question, and afterwards leave or explain.
static void gxDrainSignOut( void )
{
	// Nothing here may act on a lobby that is going away - see gxSignOutYesCallback.
	if (!s_relayLobby || isShuttingDown)
	{
		s_signOutAsk = FALSE;
		s_signOutGo = FALSE;
		return;
	}

	if (s_signOutAsk)
	{
		s_signOutAsk = FALSE;
		// Re-checked when asking, not when the menu opened: a frame has passed.
		if (GXPlayerIdentity::hasVerifiedIdentity() && !s_tryingToHostOrJoin)
		{
			// A QUESTION, TWO LINES. Asked rather than asserted, because whether the file can be
			// deleted is only known afterwards - and afterwards is when the rare failure is reported,
			// in the chat, below. The retail box does not grow (see the sign-in prompt in
			// MainMenu.cpp, whose five-line predecessor lost its last line), so the break is placed
			// by hand. The second sentence is the one thing a player could wrongly assume: that a
			// copy of this sign-in held anywhere else stops working. It does not, until it expires.
			MessageBoxYesNo(
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignOutTitle", L"Sign out?"),
				TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXSignOutPrompt",
					L"Remove the sign-in saved on this computer\n"
					L"and leave the lobby? It is not revoked on the server."),
				gxSignOutYesCallback, gxSignOutNoCallback);
		}
		return;		// never ask and act in the same frame
	}

	if (!s_signOutGo)
		return;
	s_signOutGo = FALSE;

	// The sign-out itself already happened, in gxSignOutYesCallback. What is left is the WINDOW
	// half, which is why it waited for a frame outside every callback.
	//
	// ANYTHING LEFT ON DISK KEEPS THE PLAYER HERE, where the chat can say what and why. The same
	// words /logout uses for the same outcomes; the same soft red as its refusals.
	if (s_signOutResult == GX_SIGNOUT_FILE_SURVIVED)
	{
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginOutPartial",
			L"Signed out for now, but the account file could not be deleted - you will be "
			L"signed in again next launch."), GameMakeColor(255, 140, 140, 255));
		return;
	}
	if (s_signOutResult == GX_SIGNOUT_COPY_SURVIVED)
	{
		relaySocialSay(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginOutCopy",
			L"Signed out, but a leftover copy (GXLogin.txt.tmp) could not be deleted. It is "
			L"never used to sign in; delete it by hand to remove it."), GameMakeColor(255, 140, 140, 255));
		return;
	}

	// Leave exactly the way ESC does: a simulated press of Back, so the one leave path runs.
	if (parent != nullptr && buttonBack != nullptr)
		TheWindowManager->winSendSystemMsg(parent, GBM_SELECTED, (WindowMsgData)buttonBack, buttonBackID);
}

void WOLLobbyMenuUpdate( WindowLayout * layout, void *userData)
{
	// GeneralsX @feature Claude 25/09/2026 The gx overlay's update is pumped by GameClient::update now
	// (GXBuddyOverlayPump): F5 opens it in the game room and in a match, where this screen is gone.

	gxDrainSignOut();	// GeneralsX @feature Claude 18/09/2026 see above

		if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->remove("MainMenuDefaultMenuLogoFade");
			TheTransitionHandler->setGroup("WOLCustomLobbyFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}
	if (TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == 1)
	{
		SignalUIInteraction(SHELL_SCRIPT_HOOK_GENERALS_ONLINE_ENTERED_FROM_GAME);
	}


	// GeneralsX @feature Claude 11/08/2026 Drain the login flow's queued lines into the chat box.
	//
	// WITHOUT THIS THE FEATURE DOES NOT WORK AT ALL. The service issues a short code the player has
	// to type in the browser, and this is the only thing that ever puts that code on screen -
	// GXLoginFlow cannot print it itself because it lives below the GUI and has no idea which
	// screen, if any, is on top. Left undrained, /login opens a browser asking for a code the
	// player has never been shown. (Found by cross-model review, 11/08/2026, after the queue was
	// written and never consumed.)
	{
		UnicodeString loginLine;
		Color loginColor = 0;
		while (GXLoginFlow::takeMessage(&loginLine, &loginColor))
		{
			relaySocialSay(loginLine, loginColor);
		}
	}

	// GeneralsX @feature Claude 06/08/2026 Drain incoming relay lobby chat into the chat box.
	//
	// Drained here rather than from the transport's receive path because the listbox is a UI object
	// and doRecv runs wherever the socket is serviced. The queue is bounded (Transport.h), so a
	// flood costs at most GX_CHAT_QUEUE lines per frame and cannot grow without limit.
	//
	// The sender is identified by the relay-assigned virtual IP, which is the only identity that
	// exists on a relay - there are no accounts. Last octet is the room slot, so "player 3" is the
	// honest thing to call them until there is something better to use.
	if (s_relayLobby && listboxLobbyChat && TheLAN != nullptr)
	{
		char chatLine[256];
		UnsignedInt senderIp = 0;
		while (gxLobbyTakeChat(chatLine, sizeof(chatLine), &senderIp))
		{
			UnicodeString wide, shown;
			wide.translate(AsciiString(chatLine));

			// GeneralsX @feature Claude 19/09/2026 The OPERATOR, from the admin panel's Say - drawn
			// exactly like an admin's own line ("HH:MM @Admin text" in the admin row colour) so it
			// reads as somebody with authority speaking rather than as a grey room notice. Only the
			// relay can produce this sender (GXRelaySay.h); never ignorable, never folded into a block.
			//
			// GeneralsX @bugfix Claude 20/09/2026 ...AND A MODERATOR IS DRAWN AS A MODERATOR. The role
			// comes from the sender token the relay stamped, so it is as trustworthy as the relay and
			// nothing on this side decides it. This branch used to be admin-only and the relay had one
			// token, so a moderator's Say drew as "@Admin" in the admin colour.
			const GXSayVoice sayVoice = gxSayVoiceOf(senderIp);
			if (sayVoice == GX_SAY_VOICE_OPERATOR || sayVoice == GX_SAY_VOICE_MODERATOR)
			{
				const GXAssertRole sayRole = (sayVoice == GX_SAY_VOICE_OPERATOR) ? GX_ROLE_ADMIN : GX_ROLE_MOD;
				UnicodeString spoken = GXLobbyTimestamp();
				spoken.concat(GXLobbySaid(GXLobbyOperatorName(sayRole), wide));
				shown = GXLobbyChatRow(listboxLobbyChat, UnicodeString::TheEmptyString, spoken);
				TheGameSpyInfo->addText(shown,
					GXLobbyRoleTextColor(sayRole, GameSpyColor[GSCOLOR_CHAT_NORMAL]), listboxLobbyChat);
				continue;
			}

			// GeneralsX @feature Claude 07/08/2026 Sender 0 is the RELAY talking, not a player.
			//
			// Room slots are the last octet of a virtual address and are never 0, so the relay uses
			// 0.0.0.0 as the sender for a system notice - currently "[P3] has changed their name to
			// [lol]". Rendered grey and verbatim, with no [name] wrapper of its own, so it reads as
			// a statement about the room rather than as something a player typed.
			if (senderIp == 0)
			{
				// GeneralsX @feature Claude 11/08/2026 "[L]HH:MM " - the LOBBY is talking, not a
				// player and not your own client.
				//
				// "[L]" does NOT collide with the OS column on this same screen - that column
				// stores the wire tag "L" but RENDERS "Linux". Checked, because it looked like one.
				//
				// Grouped on the same terms as [S]: a burst of room notices in one minute - which
				// is what a lobby filling up looks like - prints one "[L]HH:MM " and indents the
				// rest under it.
				const UnicodeString roomLine =
					GXLobbyChatRow(listboxLobbyChat, GXLobbyRoomPrefix(), wide);
				TheGameSpyInfo->addText(roomLine,
					GameMakeColor( 160, 160, 160, 255 ), listboxLobbyChat);
				// A notice means room state changed under us - re-ask, so the player list and the
				// names on subsequent chat lines update without waiting for the next sweep.
				if (TheLAN != nullptr)
					gxLobbyRequestRoster();
				continue;
			}

			// GeneralsX @feature Claude 08/08/2026 Lobby ignore, applied HERE and nowhere else.
			//
			// Receive side, in the menu layer, on purpose. Filtering at the sender would be a
			// promise the wire cannot keep - the relay broadcasts GXSAY to every member of the room
			// and has no per-recipient state, so "do not send this to player X" is not expressible; and
			// filtering in Transport would put a UI preference inside the digest, where changing it
			// costs a four-platform rebuild. The line is still DRAINED either way, so an ignored
			// talker cannot fill the bounded queue and starve everybody else out of it.
			if (relaySlotIsIgnored(senderIp))
				continue;

			// GeneralsX @feature Claude 11/08/2026 The role decoration and the row colour.
			//
			// The relay decided the role from a signed assertion and enforces every actual rule
			// itself, so this is presentation and nothing else - a patched client can paint itself
			// any colour it likes and still be muted.
			//
			// The DECORATION carries the role without relying on colour at all: <@Name> for a
			// moderator, @Name for an admin. Somebody who cannot tell the two greens apart, or who
			// is reading a screenshot, can still tell them apart. '@' is refused in every path that
			// assigns a name, which is what makes the marker mean something.
			const GXAssertRole role = GXLobbyRoleFromCode(relayRosterRoleForSlot(senderIp));
			const UnicodeString who = relayNickFor(senderIp);
			UnicodeString body;
			if (who.isEmpty())
			{
				UnicodeString anon;
				anon.format(L"P%d", (Int)senderIp);
				body = GXLobbySaid(GXLobbyDecorateName(anon, role), wide);
			}
			else
			{
				body = GXLobbySaid(GXLobbyDecorateName(who, role), wide);
			}
			// Owner decision, 2026-08-11: EVERY line carries the stamp, not just [S].
			//
			// GeneralsX @feature Claude 13/08/2026 A player line NEVER folds into the block above,
			// and never opens one - hence the empty prefix, with the stamp carried in the body.
			// Two reasons. Folding it under a [S] header would make somebody else's words look like
			// this client talking to itself, which is the one confusion the tags exist to prevent.
			// And folding consecutive player lines together would put two different speakers under
			// one stamp while saving nothing, because each line still has to name its speaker.
			UnicodeString spoken = GXLobbyTimestamp();
			spoken.concat(body);
			shown = GXLobbyChatRow(listboxLobbyChat, UnicodeString::TheEmptyString, spoken);
			TheGameSpyInfo->addText(shown,
				GXLobbyRoleTextColor(role, GameSpyColor[GSCOLOR_CHAT_NORMAL]), listboxLobbyChat);
		}

		// GeneralsX @bugfix Claude 16/08/2026 RE-KEY HERE, because admission is asynchronous.
		//
		// The call in WOLLobbyMenuInit is not enough on its own and this is not belt-and-braces.
		// enterLobby() leaves presence in STATE_ACQUIRING and the identity arrives later, in GXYOU, on
		// the engine's own update - so on the FIRST entry to this screen the key is recorded through
		// the game-room branch, and moments later the drain below starts reading presence slots
		// against it. Nothing called the sync again, so the session's bindings were keyed to one room
		// and guarded by another until the next screen entry, which then saw the key change and wiped
		// every pending request that session had collected.
		//
		// Cheap enough to do per frame: it formats a short key and compares it, and returns on the
		// first mismatch-free call. Placed BEFORE the drain so no slot-keyed state is parsed in a
		// frame whose boundary has not been settled yet.
		relaySyncSocialSessionToRoom();

		// GeneralsX @feature Claude 08/08/2026 The private channel, drained beside the public one
		// and for the same reason: the listbox is a UI object and the socket is serviced elsewhere.
		// Both queues are bounded in Transport, so a flood costs a fixed number of lines per frame.
		relayDrainPeerLines();
		relayUpdatePresence();
		relayReacquireIfStranded();
	}

	// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	// GeneralsX @feature Claude 06/08/2026 GX_UI: why are this screen's buttons dead?
	//
	// Measured over ssh: on WOLCustomLobby every synthetic click is a no-op - Refresh, Join and
	// Back all did nothing across 60+ seconds - while the identical click mechanism works on
	// MainMenu. The click is landing (the driver re-asserts foreground and would abort if it
	// could not), so the input is being swallowed above the button handlers. The four gates that
	// can do that are all here, and none of them is observable from outside the process.
	//
	// GBM_SELECTED opens with `if (buttonPushed) break;`, and the update work is additionally
	// gated on isAnimFinished() and TheTransitionHandler->isFinished(). A transition that never
	// reports finished would leave a screen that draws normally and ignores every button - which
	// is exactly the reported symptom, and is consistent with this box being slow enough that
	// LanGameOptionsMenu.wnd once took 90 s to build.
	{
		static const Bool s_on = (getenv("GX_UI") != NULL);
		static UnsignedInt s_last = 0;
		const UnsignedInt now = timeGetTime();
		if (s_on && (now - s_last) > 1000)
		{
			s_last = now;
			// selectedRow and joinEnabled are the two that actually matter for driving this screen:
			// ButtonJoin is created DISABLED (winEnable(FALSE) in Init) and is only re-enabled by
			// GLM_SELECTED on the games listbox. A disabled button never emits GBM_SELECTED, so a
			// click on it is silent - which is indistinguishable from a click that missed. Watching
			// selectedRow says whether the row click landed; joinEnabled says whether Join is even
			// clickable yet.
			GameWindow *glb = GetGameListBox();
			Int selRow = -1;
			if (glb != nullptr)
				GadgetListBoxGetSelected(glb, &selRow);
			const Int joinEnabled =
				(buttonJoin != nullptr) ? ((buttonJoin->winGetStatus() & WIN_STATUS_ENABLED) ? 1 : 0) : -1;

			fprintf(stderr,
				"[GXUI] lobby state: animFinished=%d transitionFinished=%d buttonPushed=%d "
				"relayLobby=%d shuttingDown=%d raiseMsgBox=%d listedGames=%d selectedRow=%d joinEnabled=%d\n",
				(Int)TheShell->isAnimFinished(), (Int)TheTransitionHandler->isFinished(),
				(Int)buttonPushed, (Int)s_relayLobby, (Int)isShuttingDown,
				(Int)raiseMessageBoxes, s_relayGameCount, selRow, joinEnabled);
			fflush(stderr);
		}
	}

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = false;
	}

	// GeneralsX @feature Matchmaking. The relay's half of the update. The GameSpy block below is
	// already gated on TheGameSpyPeerMessageQueue, which is null here, so these are exclusive.
	if (s_relayLobby && !isShuttingDown && !buttonPushed)
	{
		relayLobbyUpdate();
	}

	if (TheShell->isAnimFinished() && TheTransitionHandler->isFinished() && !buttonPushed && TheGameSpyPeerMessageQueue)
	{
		HandleBuddyResponses();
		HandlePersistentStorageResponses();

#ifdef PERF_TEST
		UnsignedInt start = timeGetTime();
		UnsignedInt end = timeGetTime();
		std::list<Int> responses;
		Int numMessages = 0;
#endif // PERF_TEST

		Int allowedMessages = TheGameSpyInfo->getMaxMessagesPerUpdate();
		Bool sawImportantMessage = FALSE;
		Bool shouldRepopulatePlayers = FALSE;
		PeerResponse resp;
		while (allowedMessages-- && !sawImportantMessage && TheGameSpyPeerMessageQueue->getResponse( resp ))
		{
#ifdef PERF_TEST
			++numMessages;
			responses.push_back(resp.peerResponseType);
#endif // PERF_TEST
			switch (resp.peerResponseType)
			{
			case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
				sawImportantMessage = TRUE;
				if (resp.joinGroupRoom.ok)
				{
					//buttonPushed = true;
					TheGameSpyInfo->setCurrentGroupRoom(resp.joinGroupRoom.id);
					TheGameSpyInfo->getPlayerInfoMap()->clear();
					GroupRoomMap::iterator iter = TheGameSpyInfo->getGroupRoomList()->find(resp.joinGroupRoom.id);
					if (iter != TheGameSpyInfo->getGroupRoomList()->end())
					{
						GameSpyGroupRoom room = iter->second;
						UnicodeString msg;
						msg.format(TheGameText->fetchGameOnly("GUI:LobbyJoined"), room.m_translatedName.str());
						TheGameSpyInfo->addText(msg, GameSpyColor[GSCOLOR_DEFAULT], nullptr);
					}
				}
				else
				{
					DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
					TheGameSpyInfo->joinBestGroupRoom();
				}
				populateGroupRoomListbox(comboLobbyGroupRooms);
				shouldRepopulatePlayers = TRUE;
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERINFO:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERJOIN:
				{
					if (resp.player.roomType == GroupRoom)
					{
						PlayerInfo p;
						fillPlayerInfo(&resp, &p);
						TheGameSpyInfo->updatePlayerInfo(p);
						shouldRepopulatePlayers = TRUE;
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERUTM:
			case PeerResponse::PEERRESPONSE_ROOMUTM:
				{
					DEBUG_LOG(("Putting off a UTM in the lobby"));
					TheLobbyQueuedUTMs.push_back(resp);
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERLEFT:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->playerLeftGroupRoom(resp.nick.c_str());
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_MESSAGE:
				{
					TheGameSpyInfo->addChat(resp.nick.c_str(), resp.message.profileID,
						UnicodeString(resp.text.c_str()), !resp.message.isPrivate, resp.message.isAction, listboxLobbyChat);
				}
				break;
			case PeerResponse::PEERRESPONSE_DISCONNECT:
				{
					sawImportantMessage = TRUE;
					UnicodeString title, body;
					AsciiString disconMunkee;
					disconMunkee.format("GUI:GSDisconReason%d", resp.discon.reason);
					title = TheGameText->fetch( "GUI:GSErrorTitle" );
					body = TheGameText->fetch( disconMunkee );
					GameSpyCloseAllOverlays();
					GSMessageBoxOk( title, body );
					TheGameSpyInfo->reset();
					TheShell->pop();
				}
				break;
			case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					if (resp.createStagingRoom.result == PEERJoinSuccess)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
						TheGameSpyInfo->markAsStagingRoomHost();
						TheGameSpyInfo->setGameOptions();
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					Bool isHostPresent = TRUE;
					if (resp.joinStagingRoom.ok == PEERTrue)
					{
						GameSpyStagingRoom *room = TheGameSpyInfo->getCurrentStagingRoom();
						if (!room)
						{
							isHostPresent = FALSE;
						}
						else
						{
							isHostPresent = FALSE;
							for (Int i=0; i<MAX_SLOTS; ++i)
							{
								AsciiString hostName;
								hostName.translate(room->getConstSlot(0)->getName());
								const char *firstPlayer = resp.stagingRoomPlayerNames[i].c_str();
								if (strcmp(hostName.str(), firstPlayer) == 0)
								{
									DEBUG_LOG(("Saw host %s == %s in slot %d", hostName.str(), firstPlayer, i));
									isHostPresent = TRUE;
								}
							}
						}
					}
					if (resp.joinStagingRoom.ok == PEERTrue && isHostPresent)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
					}
					else
					{
						UnicodeString s;

						switch(resp.joinStagingRoom.result)
						{
						case PEERFullRoom:        // The room is full.
							s = TheGameText->fetch("GUI:JoinFailedRoomFull");
							break;
						case PEERInviteOnlyRoom:  // The room is invite only.
							s = TheGameText->fetch("GUI:JoinFailedInviteOnly");
							break;
						case PEERBannedFromRoom:  // The local user is banned from the room.
							s = TheGameText->fetch("GUI:JoinFailedBannedFromRoom");
							break;
						case PEERBadPassword:     // An incorrect password (or none) was given for a passworded room.
							s = TheGameText->fetch("GUI:JoinFailedBadPassword");
							break;
						case PEERAlreadyInRoom:   // The local user is already in or entering a room of the same type.
							s = TheGameText->fetch("GUI:JoinFailedAlreadyInRoom");
							break;
						case PEERNoConnection:    // Can't join a room if there's no chat connection.
							s = TheGameText->fetch("GUI:JoinFailedNoConnection");
							break;
						default:
							s = TheGameText->fetch("GUI:JoinFailedDefault");
							break;
						}
						GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), s);
						if (groupRoomToJoin)
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - rejoining group room %d", groupRoomToJoin));
							TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
							groupRoomToJoin = 0;
						}
						else
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
							TheGameSpyInfo->joinBestGroupRoom();
						}
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOMLISTCOMPLETE:
				TheGameSpyInfo->sawFullGameList();
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOM:
				{
					GameSpyStagingRoom room;
					switch(resp.stagingRoom.action)
					{
					case PEER_CLEAR:
						TheGameSpyInfo->clearStagingRoomList();
						//TheGameSpyInfo->addText( L"gameList: PEER_CLEAR", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					case PEER_ADD:
					case PEER_UPDATE:
					{
						if (resp.stagingRoom.percentComplete == 100)
						{
							TheGameSpyInfo->sawFullGameList();
						}

						//if (ParseAsciiStringToGameInfo(&room, resp.stagingRoomMapName.c_str()))
						//if (ParseAsciiStringToGameInfo(&room, resp.stagingServerGameOptions.c_str()))
						Bool serverOk = TRUE;
						if (resp.stagingRoomMapName.empty())
						{
							serverOk = FALSE;
						}
						// fix for ghost game problem - need to iterate over all resp.stagingRoomPlayerNames[i]
						Bool sawSelf = FALSE;
						//for (Int i=0; i<MAX_SLOTS; ++i)
						//{
							if (TheGameSpyInfo->getLocalName() == resp.stagingRoomPlayerNames[0].c_str())
							{
								sawSelf = TRUE; // don't show ghost games for myself
							}
						//}
						if (sawSelf)
							serverOk = FALSE;

						if (serverOk)
						{
							room.setGameName(UnicodeString(resp.stagingServerName.c_str()));
							room.setID(resp.stagingRoom.id);
							room.setHasPassword(resp.stagingRoom.requiresPassword);
							room.setVersion(resp.stagingRoom.version);
							room.setExeCRC(resp.stagingRoom.exeCRC);
							room.setIniCRC(resp.stagingRoom.iniCRC);
							room.setAllowObservers(resp.stagingRoom.allowObservers);
              room.setUseStats(resp.stagingRoom.useStats);
							room.setPingString(resp.stagingServerPingString.c_str());
							room.setLadderIP(resp.stagingServerLadderIP.c_str());
							room.setLadderPort(resp.stagingRoom.ladderPort);
							room.setReportedNumPlayers(resp.stagingRoom.numPlayers);
							room.setReportedMaxPlayers(resp.stagingRoom.maxPlayers);
							room.setReportedNumObservers(resp.stagingRoom.numObservers);

							Int i;
							AsciiString gsMapName = resp.stagingRoomMapName.c_str();
							AsciiString mapName = "";
							for (i=0; i<gsMapName.getLength(); ++i)
							{
								char c = gsMapName.getCharAt(i);
								if (c != '/')
									mapName.concat(c);
								else
									mapName.concat('\\');
							}
							room.setMap(TheGameState->portableMapPathToRealMapPath(mapName));

							Int numPlayers = 0;
							for (i=0; i<MAX_SLOTS; ++i)
							{
								GameSpyGameSlot *slot = room.getGameSpySlot(i);
								if (slot)
								{
									slot->setWins( resp.stagingRoom.wins[i] );
									slot->setLosses( resp.stagingRoom.losses[i] );
									slot->setProfileID( resp.stagingRoom.profileID[i] );
									slot->setPlayerTemplate( resp.stagingRoom.faction[i] );
									slot->setColor( resp.stagingRoom.color[i] );
									if (resp.stagingRoom.profileID[i] == SLOT_EASY_AI)
									{
										slot->setState(SLOT_EASY_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_MED_AI)
									{
										slot->setState(SLOT_MED_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_BRUTAL_AI)
									{
										slot->setState(SLOT_BRUTAL_AI);
										++numPlayers;
									}
									else if (!resp.stagingRoomPlayerNames[i].empty())
									{
										UnicodeString nameUStr;
										nameUStr.translate(resp.stagingRoomPlayerNames[i].c_str());
										slot->setState(SLOT_PLAYER, nameUStr);
										++numPlayers;
									}
									else
									{
										slot->setState(SLOT_OPEN);
									}
								}
							}
							DEBUG_ASSERTCRASH(numPlayers, ("Game had no players!"));
							//DEBUG_LOG(("Saw room: hasPass=%d, allowsObservers=%d", room.getHasPassword(), room.getAllowObservers()));
							if (resp.stagingRoom.action == PEER_ADD)
							{
								TheGameSpyInfo->addStagingRoom(room);
								//TheGameSpyInfo->addText( L"gameList: PEER_ADD", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
							else
							{
								TheGameSpyInfo->updateStagingRoom(room);
								//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
						}
						else
						{
							room.setID(resp.stagingRoom.id);
							TheGameSpyInfo->removeStagingRoom(room);
							//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE FAILED", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						}
						break;
					}
					case PEER_REMOVE:
						room.setID(resp.stagingRoom.id);
						TheGameSpyInfo->removeStagingRoom(room);
						//TheGameSpyInfo->addText( L"gameList: PEER_REMOVE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					default:
						//TheGameSpyInfo->addText( L"gameList: Unknown", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					}
				}
				break;
			}
		}
#if 0
		if (shouldRepopulatePlayers)
		{
			PopulateLobbyPlayerListbox();
		}
#else
		refreshPlayerList();
#endif

#ifdef PERF_TEST
		// check performance
		end = timeGetTime();
		PERF_LOG(("Frame time was %d ms", end-start));
		std::list<Int>::const_iterator it;
		for (it = responses.begin(); it != responses.end(); ++it)
		{
			PERF_LOG(("  %s", getMessageString(*it)));
		}
		PERF_LOG((""));
#endif // PERF_TEST

#if 0
// Removed 2-17-03 to pull out into a function so we can do the same checks
		Int refreshInterval = gameListRefreshInterval;

		if ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime()))
		{
			if (TheGameSpyInfo->hasStagingRoomListChanged())
			{
				//DEBUG_LOG(("################### refreshing game list"));
				//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
				RefreshGameListBoxes();
				gameListRefreshTime = timeGetTime();
			} else {
				//DEBUG_LOG(("-"));
			}
		} else {
			//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
		}
#else
	refreshGameList();
#endif
	}
}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (buttonPushed)
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

//static void doSliderTrack(GameWindow *control, Int val)
//{
//	Int sliderW, sliderH, sliderX, sliderY;
//	control->winGetPosition(&sliderX, &sliderY);
//	control->winGetSize(&sliderW, &sliderH);
//	Real cursorY = sliderY + (100-val)*0.01f*sliderH;
//
//	extern GameWindow *listboxLobbyGamesSmall;
//	extern GameWindow *listboxLobbyGamesLarge;
//	extern GameWindow *listboxLobbyGameInfo;
//
//	static Int gwsX = 0, gwsY = 0, gwsW = 0, gwsH = 0;
//	static Int gwlX = 0, gwlY = 0, gwlW = 0, gwlH = 0;
//	static Int gwiX = 0, gwiY = 0, gwiW = 0, gwiH = 0;
//	static Int pwX = 0, pwY = 0, pwW = 0, pwH = 0;
//	static Int chatPosX = 0, chatPosY = 0, chatW = 0, chatH = 0;
//	static Int spacing = 0;
//	if (chatPosX == 0)
//	{
//		listboxLobbyChat->winGetPosition(&chatPosX, &chatPosY);
//		listboxLobbyChat->winGetSize(&chatW, &chatH);
//
////		listboxLobbyGamesSmall->winGetPosition(&gwsX, &gwsY);
////		listboxLobbyGamesSmall->winGetSize(&gwsW, &gwsH);
//
//		listboxLobbyGamesLarge->winGetPosition(&gwlX, &gwlY);
//		listboxLobbyGamesLarge->winGetSize(&gwlW, &gwlH);
//
////		listboxLobbyGameInfo->winGetPosition(&gwiX, &gwiY);
////		listboxLobbyGameInfo->winGetSize(&gwiW, &gwiH);
////
//		listboxLobbyPlayers->winGetPosition(&pwX, &pwY);
//		listboxLobbyPlayers->winGetSize(&pwW, &pwH);
//
//		spacing = chatPosY - pwY - pwH;
//	}
//
//	Int newChatY = cursorY;
//	Int newChatH = chatH + chatPosY - newChatY;
//	listboxLobbyChat->winSetPosition(chatPosX, newChatY);
//	listboxLobbyChat->winSetSize(chatW, newChatH);
//
//	Int newH = cursorY - pwY - spacing;
//	listboxLobbyPlayers->winSetSize(pwW, newH);
////	listboxLobbyGamesSmall->winSetSize(gwsW, newH);
//	listboxLobbyGamesLarge->winSetSize(gwlW, newH);
////	listboxLobbyGameInfo->winSetSize(gwiW, newH);


//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	static NameKeyType buttonGameListTypeToggleID = NAMEKEY_INVALID;

	// GeneralsX @bugfix Claude 30/07/2026 A screen that is animating out must not act on clicks.
	//
	// WOLLobbyMenuShutdown disarms its own guards and then leaves the layout live:
	//
	//   TearDownGameSpy();      // deletes and nulls TheGameSpyInfo, TheLadderList, ...
	//   s_relayLobby = FALSE;   // and EVERY relay branch below is written `if (s_relayLobby) break;`
	//   isShuttingDown = true;
	//   TheShell->reverseAnimatewindow();   // popImmediate is FALSE on the Shell::push path
	//
	// so from that point the layout keeps drawing and keeps taking input for as many frames as the
	// exit animation lasts, with every `if (s_relayLobby) break;` now evaluating false and every
	// GameSpy singleton null. One tap on Back in that window reaches
	// `TheGameSpyInfo->leaveGroupRoom()` and takes the process out. Measured: the windows peer sent
	// Request Game Leave to the host and then vanished with no crash text and no Application event,
	// which from the player's side looks exactly like the button doing nothing.
	//
	// Guarding each individual deref would be whack-a-mole across Back, Host, Join, the two
	// right-click menus and the list toggle, and would leave the next handler somebody adds
	// exposed. The screen is going away; the only correct answer to any interaction is to ignore
	// it. GWM_CREATE / GWM_DESTROY / GWM_INPUT_FOCUS are deliberately still let through - those are
	// lifecycle, not interaction, and swallowing them would strand the window manager.
	if( isShuttingDown )
	{
		switch( msg )
		{
			case GWM_CREATE:
			case GWM_DESTROY:
			case GWM_INPUT_FOCUS:
				break;
			default:
				return MSG_IGNORED;
		}
	}

	switch( msg )
	{


		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
			{
				buttonGameListTypeToggleID = NAMEKEY("WOLCustomLobby.wnd:ButtonGameListToggle");
//				sliderChatAdjustID = NAMEKEY("WOLCustomLobby.wnd:SliderChatAdjust");

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
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
		case GLM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if ( controlID == GetGameListBoxID() )
				{
					int rowSelected = mData2;
					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. There is no extended-info round trip to make:
						// the GXGAME reply is everything the relay knows about a game. Following the
						// selection with the player list is the whole of it.
						//
						// GeneralsX @bugfix Claude 07/08/2026 Enable Join on the row's GAME, not on
						// the row's INDEX.
						//
						// Row 0 of this listbox is the column header added by RefreshGameListBox, and
						// the "no lobbies available" notice takes a row too. Neither carries item
						// data, so `rowSelected >= 0` lit Join up over a row that can never be joined:
						// relayJoinSelectedGame then read selectedID 0, found no game, and popped
						// GUI:NoGameSelected. Clicking the top row and pressing Join therefore did
						// nothing, which is indistinguishable from Join being broken - and it cost
						// several driven runs to tell those apart.
						//
						// relaySelectedGameID() already returns 0 for a row with no game, so gating on
						// it makes the button's enabled state and the join's precondition the same
						// test instead of two tests that can disagree.
						s_relaySelectedID = relaySelectedGameID();

						// GeneralsX @bugfix Claude 07/08/2026 Do not leave a non-game row HIGHLIGHTED.
						//
						// Gating Join on the row's game (above) stopped the header row from arming a
						// join, but the listbox still DREW it selected - so the column header sat
						// there highlighted as though it were a chosen game, and so did the "no
						// lobbies available" notice. Dropping the selection makes the visible state
						// agree with the button state, and turns a click on the header into a
						// visible no-op rather than something that looks like it worked.
						if (s_relaySelectedID == 0 && rowSelected >= 0)
							GadgetListBoxSetSelected(control, -1);

						buttonJoin->winEnable(s_relaySelectedID > 0);
						relayRefreshPlayerList();
						break;
					}
					if( rowSelected >= 0 )
					{
						buttonJoin->winEnable(TRUE);
						static UnsignedInt lastFrame = 0;
						static Int lastID = -1;
						UnsignedInt now = TheGameClient->getFrame();

						PeerRequest req;
						req.peerRequestType = PeerRequest::PEERREQUEST_GETEXTENDEDSTAGINGROOMINFO;
					// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
					req.stagingRoom.id = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rowSelected, 0)));

						lastID = req.stagingRoom.id;
						lastFrame = now;
					}
					else
					{
						buttonJoin->winEnable(FALSE);
					}
					if (GetGameInfoListBox())
					{
						RefreshGameInfoListBox(GetGameListBox(), GetGameInfoListBox());
					}
				}

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (HandleSortButton((NameKeyType)controlID))
					break;

				// GeneralsX @bugfix Claude 06/08/2026 ButtonEmote is the SEND button on this screen.
				if (s_relayLobby && controlID == buttonEmoteID)
				{
					relaySubmitChat();
					break;
				}

				// If we back out, just bail - we haven't gotten far enough to need to log out
				if ( controlID == buttonBackID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. Nothing to leave - a relay room is not a
						// chat channel and leaveGroupRoom() is a peerchat request. And no next
						// screen: WOLWelcomeMenu is the GameSpy login screen, so pushing it would
						// send a player who pressed Back straight back into the dead service the
						// Online button was changed to avoid. Pop to wherever we came from.
						SetLobbyAttemptHostJoin( TRUE ); // pretend, so nothing else queues up
						buttonPushed = true;
						nextScreen = nullptr;
						TheShell->pop();
						break;
					}

					// Leave any group room, then pop off the screen
					TheGameSpyInfo->leaveGroupRoom();

					SetLobbyAttemptHostJoin( TRUE ); // pretend, since we don't want to queue up another action
					buttonPushed = true;
					nextScreen = "Menus/WOLWelcomeMenu.wnd";
					TheShell->pop();

				}
				else if ( controlID == buttonRefreshID )
				{
					// Added 2/17/03 added the game refresh button
					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. Redrawing what we already have is not a
						// refresh - the relay only answers when asked, so ask again first.
						relayRequestGameList();
					}
					refreshGameList(TRUE);
					refreshPlayerList(TRUE);
				}
				else if ( controlID == buttonHostID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. GSOVERLAY_GAMEOPTIONS is the GameSpy
						// staging-room overlay and is driven end to end by the peer thread. Our
						// lobby IS a LAN game on a virtual network, so hosting is the same two
						// calls the Direct Connect screen makes, and LANAPI::OnGameCreate pushes
						// the LAN game options screen from there.
						//
						// No SetLobbyAttemptHostJoin here either - see relayJoinSelectedGame.
						//
						// GeneralsX @feature Claude 16/08/2026 ASK FIRST. This used to host
						// immediately, so the only way to protect a game was to notice the
						// Password box on the NEXT screen and type /password - by which point the
						// room had already been advertised unprotected.
						//
						// GSOVERLAY_GAMEOPTIONS is Menus/PopupHostGame.wnd (GameSpyOverlay.cpp's
						// table, commented "Hosting options (game name, password, etc)"), which
						// ships in stock WindowZH.big with both text boxes already in it. Opening
						// it edits no layout, so dataID does not move and no peer starts refusing
						// joins - the constraint LANAPI.h records applies to ADDING a box, not to
						// using one that is already there.
						//
						// The overlay's Create button calls back into
						// GXRelayHostGameWithOptions; nothing hosts until it does.
						//
						// Still NO SetLobbyAttemptHostJoin, for the reason the line above already
						// gives. The popup's Cancel path clears that flag unconditionally, which is
						// harmless when it was never raised, and raising it here would arm a
						// GameSpy-side latch on a screen whose GameSpy half is deliberately inert.
						GameSpyOpenOverlay(GSOVERLAY_GAMEOPTIONS);
						break;
					}

					SetLobbyAttemptHostJoin( TRUE );
					TheLobbyQueuedUTMs.clear();
					groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					GameSpyOpenOverlay(GSOVERLAY_GAMEOPTIONS);
				}
				else if ( controlID == buttonJoinID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						relayJoinSelectedGame();
						break;
					}

					TheLobbyQueuedUTMs.clear();
					// Look for a game to join
					groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					Int selected;
					GadgetListBoxGetSelected(GetGameListBox(), &selected);
					if (selected >= 0)
					{
						// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
						Int selectedID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(GetGameListBox(), selected)));
						if (selectedID > 0)
						{
							StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
							StagingRoomMap::iterator srmIt = srm->find(selectedID);
							if (srmIt != srm->end())
							{
								GameSpyStagingRoom *roomToJoin = srmIt->second;
								if (!roomToJoin || roomToJoin->getExeCRC() != TheGlobalData->m_exeCRC || roomToJoin->getIniCRC() != TheGlobalData->m_iniCRC)
								{
									// bad crc.  don't go.
									DEBUG_LOG(("WOLLobbyMenuSystem - CRC mismatch with the game I'm trying to join. My CRC's - EXE:0x%08X INI:0x%08X  Their CRC's - EXE:0x%08x INI:0x%08x", TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC, roomToJoin->getExeCRC(), roomToJoin->getIniCRC()));
#if defined(RTS_DEBUG)
									if (TheGlobalData->m_netMinPlayers)
									{
										GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
										break;
									}
									else if (g_fakeCRC)
									{
										TheWritableGlobalData->m_exeCRC = roomToJoin->getExeCRC();
										TheWritableGlobalData->m_iniCRC = roomToJoin->getIniCRC();
									}
#else
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
									break;
#endif
								}
								Bool unknownLadder = (roomToJoin->getLadderPort() && TheLadderList->findLadder(roomToJoin->getLadderIP(), roomToJoin->getLadderPort()) == nullptr);
								if (unknownLadder)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedUnknownLadder"));
									break;
								}
								if (roomToJoin->getNumPlayers() == MAX_SLOTS)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedRoomFull"));
									break;
								}
								TheGameSpyInfo->markAsStagingRoomJoiner(selectedID);
								TheGameSpyGame->setGameName(roomToJoin->getGameName());
								TheGameSpyGame->setLadderIP(roomToJoin->getLadderIP());
								TheGameSpyGame->setLadderPort(roomToJoin->getLadderPort());
								SetLobbyAttemptHostJoin( TRUE );
								if (roomToJoin->getHasPassword())
								{
									GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
								}
								else
								{
									// no password - just join it
									PeerRequest req;
									req.peerRequestType = PeerRequest::PEERREQUEST_JOINSTAGINGROOM;
									req.text = srmIt->second->getGameName().str();
									req.stagingRoom.id = selectedID;
									req.password = "";
									TheGameSpyPeerMessageQueue->addRequest(req);
								}
							}
						}
						else
						{
							GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameInfo"), nullptr);
						}
					}
					else
					{
						GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), nullptr);
					}
				}
				else if ( controlID == buttonBuddyID )
				{
					// GeneralsX @feature Matchmaking. The buddy list is a GameSpy profile feature and
					// GameSpyOpenOverlay dereferences the buddy message queue we never created.
					// GeneralsX @feature Claude 08/08/2026 On the relay path this is the FRIENDS
					// button instead. It must still never reach GameSpyToggleOverlay - that null
					// dereference is the reason the control was dead in the first place.
					//
					// GeneralsX @feature Claude 09/08/2026 It now opens a real panel instead of
					// printing the list into chat. relayPrintFriends stays exactly where it was and
					// is still what /friend with no argument runs, so the text form is unchanged for
					// headless peers and for anyone who prefers typing; this only gives the button
					// something better to do than narrate.
					// GeneralsX @feature Claude 11/08/2026 And now it opens the RETAIL overlay -
					// WOLBuddyOverlay.wnd re-pointed at the local store - instead of the CodeGui
					// panel. Same rule as ever: never through GameSpyToggleOverlay on this path.
					if (s_relayLobby)
					{
						GXBuddyOverlayToggle();
						break;
					}

					GameSpyToggleOverlay( GSOVERLAY_BUDDY );
				}
				else if ( controlID == buttonGameListTypeToggleID )
				{
					ToggleGameListType();
				}
				else if ( controlID == buttonEmoteID )
				{
					// GeneralsX @feature Matchmaking. No chat channel exists on the relay, and
					// sendChat posts to the peer message queue. See the note in Init.
					if (s_relayLobby)
						break;

				// read the user's input and clear the entry box
					UnicodeString txtInput;
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					txtInput.trim();
					if (!txtInput.isEmpty())
					{
						// Send the message
						TheGameSpyInfo->sendChat( txtInput, FALSE, listboxLobbyPlayers ); // 'emote' button now just sends text
					}
				}

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if (s_tryingToHostOrJoin)
					break;
				// GeneralsX @feature Matchmaking. The group-room dropdown does not switch rooms
				// here: it names the relay we are on, and joinGroupRoom/leaveGroupRoom are both
				// peerchat requests.
				// GeneralsX @feature Claude 08/08/2026 It IS the game-list filter now, so the relay
				// path reads the selection instead of ignoring the message outright.
				if (s_relayLobby)
				{
					GameWindow *relayControl = (GameWindow *)mData1;
					if (relayControl != nullptr
						&& relayControl->winGetWindowId() == (Int)comboLobbyGroupRoomsID)
					{
						Int row = -1;
						GadgetComboBoxGetSelectedPos(relayControl, &row);
						if (row >= 0)
						{
							// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
							const Int data = static_cast<Int>(reinterpret_cast<intptr_t>(
								GadgetComboBoxGetItemData(relayControl, row)));
							// relaySetFriendsOnly returns immediately when nothing changed, which is
							// what makes it safe for this to also be raised by our own
							// GadgetComboBoxSetSelectedPos call in relaySyncFilterCombo.
							relaySetFriendsOnly(data != 0);
						}
					}
					break;
				}
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == comboLobbyGroupRoomsID )
				{
					int rowSelected = -1;
					GadgetComboBoxGetSelectedPos(control, &rowSelected);

					DEBUG_LOG(("Row selected = %d", rowSelected));
					if (rowSelected >= 0)
					{
						Int groupID;
						// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
						groupID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(comboLobbyGroupRooms, rowSelected)));
						DEBUG_LOG(("ItemData was %d, current Group Room is %d", groupID, TheGameSpyInfo->getCurrentGroupRoom()));
						if (groupID && groupID != TheGameSpyInfo->getCurrentGroupRoom())
						{
							TheGameSpyInfo->leaveGroupRoom();
							TheGameSpyInfo->joinGroupRoom(groupID);

							if (TheGameSpyConfig->restrictGamesToLobby())
							{
								TheGameSpyInfo->clearStagingRoomList();
								RefreshGameListBoxes();
								PeerRequest req;
								req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
								req.gameList.restrictGameList = TRUE;
								TheGameSpyPeerMessageQueue->addRequest(req);
							}
						}
					}
				}
			}
			break;

		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if (controlID == GetGameListBoxID())
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						GadgetListBoxSetSelected( control, rowSelected );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonJoinID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonJoinID );
					}
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		case GLM_RIGHT_CLICKED:
			{
				// GeneralsX @feature Claude 11/08/2026 On the relay this is the SENDING HALF of
				// the card exchange - the click-a-name idiom the retail RC menus were built for,
				// re-pointed at slots instead of GameSpy profiles. The old blanket break said
				// "every entry is a no-op or a null queue write", which was true until the
				// overlay's handler learned the BY_SLOT payload; now the stranger menu's Add
				// offers our card, the request menu answers one, and the friend menu's entries
				// act on the durable id resolved at action time.
				if (s_relayLobby)
				{
					GameWindow *rControl = (GameWindow *)mData1;
					if (rControl == nullptr || rControl->winGetWindowId() != listboxLobbyPlayersID)
						break;
					RightClickStruct *rc = (RightClickStruct *)mData2;
					if (rc == nullptr || rc->pos < 0)
						break;
					// The slot was stamped at render; info rows carry none.
					// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
					const Int slot = static_cast<Int>(reinterpret_cast<intptr_t>(
						GadgetListBoxGetItemData(rControl, rc->pos, 0)));
					// GeneralsX @feature Claude 18/09/2026 THE PLAYER'S OWN NAME opens EA's "this is
					// me" menu - RCLocalPlayerMenu.wnd, the one the GameSpy branch below opens for
					// the local profile - carrying Sign Out. Only while signed in (there is nothing
					// to remove otherwise) and not mid host/join (leaving would strand it). The
					// retail GameSpy path is untouched; this is the relay branch's own row.
					if (slot > 0 && slot <= GX_ROOM_SLOTS && (UnsignedInt)slot == relayMySlot())
					{
						if (GXPlayerIdentity::hasVerifiedIdentity() && !s_tryingToHostOrJoin)
						{
							GadgetListBoxSetSelected(rControl, rc->pos);
							GXBuddyOpenSelfRCMenu(rc->mouseX, rc->mouseY);
						}
						break;
					}
					if (slot <= 0 || slot > GX_ROOM_SLOTS)
						break;

					// Revalidation point one of two: the rendered row must still name the current
					// occupant. Roster nicks are GXNICK tokens (seven-bit), so the ascii round
					// trip of the row text is lossless here. Mismatch means the roster moved under
					// the click: redraw and refuse, and the next click acts on what it sees.
					Bool present = FALSE;
					const char *now = relayRosterNickForSlot((UnsignedInt)slot, &present);
					if (!present)
						break;
					char expected[64];
					if (now[0] != '\0')
						strlcpy(expected, now, sizeof(expected));
					else
						snprintf(expected, sizeof(expected), "P%d", slot);
					AsciiString rowAscii;
					rowAscii.translate(GadgetListBoxGetText(rControl, rc->pos, 1));
					if (strcmp(rowAscii.str(), expected) != 0)
					{
						PopulateLobbyPlayerListbox();
						break;
					}

					GadgetListBoxSetSelected(rControl, rc->pos);
					Bool byName = FALSE;
					const char *layoutName = "Menus/RCNonBuddiesMenu.wnd";
					if (s_cardTheyAsked[slot])
						layoutName = "Menus/RCBuddyRequestMenu.wnd";
					else if (relaySlotIsFriend((UnsignedInt)slot, &byName) && !byName)
						// A name-only match stays on the stranger menu: its Add offers the
						// exchange that upgrades them to id-confirmed, and no id-keyed entry can
						// fire off a name claim - the taxonomy above, applied to a menu.
						layoutName = "Menus/RCBuddiesMenu.wnd";

					// Snapshot the RAW roster nick - possibly empty; the P<n> spelling is
					// presentation, not identity.
					GXBuddyOpenRosterRCMenu(layoutName, slot, AsciiString(now),
						rc->mouseX, rc->mouseY, relaySlotIsIgnored((UnsignedInt)slot));
					break;
				}

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if( controlID == listboxLobbyPlayersID )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					GPProfile profileID = 0;
					AsciiString aName;
					aName.translate(GadgetListBoxGetText(control, rc->pos, COLUMN_PLAYERNAME));
					PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
					if (it != TheGameSpyInfo->getPlayerInfoMap()->end())
						profileID = it->second.m_profileID;

					Bool isBuddy = FALSE;
					if (profileID <= 0)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCNoProfileMenu.wnd");
					else
					{
						if (profileID == TheGameSpyInfo->getLocalProfileID())
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCLocalPlayerMenu.wnd");
						}
						else if(TheGameSpyInfo->isBuddy(profileID))
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddiesMenu.wnd");
							isBuddy = TRUE;
						}
						else
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCNonBuddiesMenu.wnd");
					}
					if(!rcLayout)
						break;

					GadgetListBoxSetSelected(control, rc->pos);

					rcMenu = rcLayout->getFirstWindow();
					rcMenu->winGetLayout()->runInit();
					rcMenu->winBringToTop();
					rcMenu->winHide(FALSE);
					setUnignoreText( rcLayout, aName, profileID);
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
					rcData->m_nick = aName;
					rcData->m_itemType = (isBuddy)?ITEM_BUDDY:ITEM_NONBUDDY;
					rcMenu->winSetUserData((void *)rcData);
					TheWindowManager->winSetLoneWindow(rcMenu);
				}
				else if( controlID == GetGameListBoxID() )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
					Int selectedID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rc->pos)));
					if (selectedID > 0)
					{
						StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
						StagingRoomMap::iterator srmIt = srm->find(selectedID);
						if (srmIt != srm->end())
						{
							GameSpyStagingRoom *theRoom = srmIt->second;
							if (!theRoom)
								break;
							const LadderInfo *linfo = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
							if (linfo)
							{
								rcLayout = TheWindowManager->winCreateLayout("Menus/RCGameDetailsMenu.wnd");
								if (!rcLayout)
									break;

								GadgetListBoxSetSelected(control, rc->pos);

								rcMenu = rcLayout->getFirstWindow();
								rcMenu->winGetLayout()->runInit();
								rcMenu->winBringToTop();
								rcMenu->winHide(FALSE);
								rcMenu->winSetPosition(rc->mouseX, rc->mouseY);

								rcMenu->winSetUserData((void *)selectedID);
								TheWindowManager->winSetLoneWindow(rcMenu);
							}
						}
					}
				}
				break;
			}

//		//---------------------------------------------------------------------------------------------
//		case GSM_SLIDER_TRACK:
//		{
//				if (buttonPushed)
//					break;
//
//			GameWindow *control = (GameWindow *)mData1;
//			Int val = (Int)mData2;
//			Int controlID = control->winGetWindowId();
//			if (controlID == sliderChatAdjustID)
//			{
//				doSliderTrack(control, val);
//			}
//			break;
//		}

		//---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				if (buttonPushed)
					break;

				// read the user's input and clear the entry box
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryChat ));
				GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				txtInput.trim();
				if (txtInput.isEmpty())
					break;

				// GeneralsX @feature Claude 06/08/2026 Relay lobby chat - see relaySubmitChat().
				// Deliberately NOT routed through TheGameSpyInfo->sendChat: that writes to the
				// peerchat message queue, which does not exist on a relay, and its slash commands
				// query the GameSpy QR2 hosting state.
				if (s_relayLobby)
				{
					GadgetTextEntrySetText(textEntryChat, txtInput);	// relaySubmitChat re-reads it
					relaySubmitChat();
					break;
				}

				// Send the message. The GameSpy slash interceptor that used to gate this is
				// deleted - see the note where it lived - so the unreachable GameSpy path just
				// sends, as retail did before the slash commands existed.
				//
				// GeneralsX @feature Claude 21/08/2026 Rate limited like every other chat box.
				// No notice on this one: it is the legacy GameSpy path, which this build cannot
				// reach, and inventing a sink for a dead screen is not worth the code.
				{
					unsigned int gxChatWait = 0;
					if (!ChatRate::consumeShared(timeGetTime(), &gxChatWait))
					{
						GadgetTextEntrySetText(textEntryChat, txtInput);
						break;
					}
				}
				TheGameSpyInfo->sendChat( txtInput, false, listboxLobbyPlayers );
				break;
			}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
