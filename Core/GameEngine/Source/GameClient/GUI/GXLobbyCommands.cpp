/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX @feature Claude 11/08/2026 See the design note on GXLobbyCommands.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GXLobbyPresence.h"		// GXLobbySignOut: the shared-lobby socket holds a copy of the sign-in
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXLoginFlow.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameNetwork/LANAPI.h"			// GXLobbySignOut: TheLAN's transport holds a copy of the sign-in
#include "GameNetwork/LANAPICallbacks.h"	// TheLAN
#include "GameNetwork/Transport.h"

// Colour literals live HERE rather than in multiplayer.ini, deliberately (SCOPE Phase 4f).
// multiplayer.ini ships inside the .big archives, so recolouring a help heading through data would
// move dataID and every peer would then refuse to join with DATA_DIFFERS - a four-platform rollout
// to change a colour.
static const Color GX_HELP_TITLE_COLOR  = GameMakeColor( 255, 200,  90, 255 );	// amber
static const Color GX_HELP_GROUP_COLOR  = GameMakeColor( 120, 200, 255, 255 );	// sky
static const Color GX_HELP_ROW_COLOR    = GameMakeColor( 215, 215, 215, 255 );	// near white
static const Color GX_HELP_REFUSE_COLOR = GameMakeColor( 255, 140, 140, 255 );	// soft red

// GeneralsX @feature Claude 11/08/2026 The role and system-line colours, in the SAME place and the
// same form as the four above - one vocabulary, not two - and for the same dataID reason.
//
// A TWO-COLOUR CHAT ROW IS NOT POSSIBLE IN THIS ENGINE, so the spec's "purple name with light-green
// text" is split across the two surfaces that can each carry one colour. Measured, not assumed:
// DisplayString::draw takes a single Color, SentenceDataStruct (render2dsentence.h) has no colour
// field at all, and the only markup the renderer parses is '&' for hotkeys, which carries no
// colour. Per-CELL colour does exist (Gadget.h ListEntryCell, W3DListBox.cpp), but
// WOLCustomLobby.wnd:ListboxChat is COLUMNS: 1 so it collapses to per-row - and promoting it to two
// columns at runtime means reallocating columnWidth, allocating the currently-null
// columnWidthPercentage, doing both before the first row is added or every draw heap-over-reads
// cells[1], and imposing a fixed gutter on every line of prose.
//
// So: the NAME colour goes to ListboxPlayers, which is already COLUMNS: 2 and already gets per-cell
// treatment, and the TEXT colour goes to the chat row. Both colours from the spec ship; neither is
// dropped, and neither surface is asked to do something it cannot.
//
// TWO ADJACENCIES TO PRESERVE, both one careless edit from being confusing:
//   GX_CHAT_SYSTEM_COLOR must stay distinguishable from GSCOLOR_CHAT_OWNER (255,255,0) - that is
//   YOUR line to the room, and this is your own client talking to you alone.
//   The ADMIN row colour must stay distinguishable from the private-message violet (220,160,255) -
//   a whisper must never be mistakable for an announcement. Still true of the console's crimson.
//
// GeneralsX @feature Claude 20/09/2026 (owner: "make the colors apply for text chat same as on
// website") THE ADMIN COLOUR IS NOW THE CONSOLE'S, ON BOTH SURFACES.
//
// It used to be two different colours for one role: light purple on the chat row and ORANGE in the
// name column, neither of them anything the console draws. An admin was therefore a different
// colour in the lobby chat, in the player list and on the website - which is three ways to learn a
// role and no way to recognise one. Both are now panel.css's --accent, #D62B44, taken from
// GXChatLine.h so the value has exactly one home; see the long note there for what the console does
// and does not define, and for why the moderator green below is unchanged.
//
// The split between a NAME colour and a ROW colour survives - it exists because a listbox row
// carries one colour and the two surfaces are the only way to ship both halves of the spec - but
// the two halves of the ADMIN's pair are no longer different hues.
static Color gxWebTone( unsigned int rgb, Color fallback )
{
	if (rgb == (unsigned int)GX_WEB_TONE_NONE)
		return fallback;
	return GameMakeColor( (UnsignedByte)((rgb >> 16) & 0xFF), (UnsignedByte)((rgb >> 8) & 0xFF),
		(UnsignedByte)(rgb & 0xFF), 255 );
}

static const Color GX_CHAT_SYSTEM_COLOR     = GameMakeColor( 250, 240, 130, 255 );	// pale yellow
// The console draws no moderator at all, so there is nothing to match and these keep what they had.
static const Color GX_CHAT_MOD_TEXT_COLOR   = GameMakeColor( 170, 235, 170, 255 );	// light green
static const Color GX_ROLE_MOD_NAME_COLOR   = GameMakeColor( 185, 130, 255, 255 );	// purple

/// Upper bound on the rows one /help group can print. Sized well above s_commands so the sort
/// buffer below can never be the thing that truncates the list; a static assert would be better
/// but the count is not known until after the table, so the check lives in gxRenderHelp's guard.
#define GX_LOBBY_HELP_MAX_ROWS	32

enum GXLobbyGroup
{
	GX_LOBBY_GROUP_CHAT = 0,
	GX_LOBBY_GROUP_FRIENDS,
	GX_LOBBY_GROUP_GAME,

	GX_LOBBY_GROUP_COUNT
};

#define GX_SCREEN_BIT(s)	(1u << (UnsignedInt)(s))
#define GX_ON_ALL			(GX_SCREEN_BIT(GX_LOBBY_SCREEN_ONLINE) | \
							 GX_SCREEN_BIT(GX_LOBBY_SCREEN_LAN) | \
							 GX_SCREEN_BIT(GX_LOBBY_SCREEN_GAME))
#define GX_ON_ONLINE		GX_SCREEN_BIT(GX_LOBBY_SCREEN_ONLINE)
#define GX_ON_ONLINE_GAME	(GX_SCREEN_BIT(GX_LOBBY_SCREEN_ONLINE) | \
							 GX_SCREEN_BIT(GX_LOBBY_SCREEN_GAME))
#define GX_ON_LAN			(GX_SCREEN_BIT(GX_LOBBY_SCREEN_LAN) | \
							 GX_SCREEN_BIT(GX_LOBBY_SCREEN_GAME))

struct GXLobbyCommandDef
{
	const char *verb;			///< lower case, no slash
	const WideChar *args;		///< nullptr when the command takes none
	const char *helpLabel;		///< .csf label for the one-line description
	const WideChar *helpText;	///< fallback used when the label is absent
	UnsignedInt screens;		///< bitmask of GX_SCREEN_BIT
	GXLobbyGroup group;
};

// Deliberately NOT written in sorted order. renderHelp sorts at draw time, so an edit that appends
// a row in the wrong place cannot silently un-sort what the player reads.
static const GXLobbyCommandDef s_commands[] =
{
	{ "help",     L"[command]",      "GUI:GXHelpHelp",     L"list the commands, or explain one",               GX_ON_ALL,         GX_LOBBY_GROUP_CHAT    },
	{ "clear",    nullptr,           "GUI:GXHelpClear",    L"empty this chat box",                             GX_ON_ALL,         GX_LOBBY_GROUP_CHAT    },
	// GeneralsX @feature Claude 21/09/2026 LAN ONLY, and the mask is load-bearing. /me is served by
	// LANAPI::RequestPlayerChat, which only the LAN screens and the score screen call; the relay
	// lobby sends its own bytes and would broadcast the line verbatim. Listing it on ONLINE would
	// turn a clear refusal into "/me waves" said out loud.
	{ "me",       L"<text>",         "GUI:GXHelpMe",       L"say what you are doing, in the third person",     GX_ON_LAN,         GX_LOBBY_GROUP_CHAT    },
	{ "msg",      L"<name> <text>",  "GUI:GXHelpMsg",      L"private message - only they see it",              GX_ON_ONLINE,      GX_LOBBY_GROUP_CHAT    },
	{ "r",        L"<text>",         "GUI:GXHelpReply",    L"answer the last person who messaged you",         GX_ON_ONLINE,      GX_LOBBY_GROUP_CHAT    },
	// GeneralsX @feature Claude 11/08/2026 /ignore takes a friend CODE too, for somebody absent.
	{ "ignore",   L"<name, P3 or code>", "GUI:GXHelpIgnore",   L"hide someone's chat - a code works even when they are not here", GX_ON_ONLINE, GX_LOBBY_GROUP_CHAT },
	{ "unignore", L"<name, P3 or code>", "GUI:GXHelpUnignore", L"stop hiding them",                            GX_ON_ONLINE,      GX_LOBBY_GROUP_CHAT    },
	{ "friends",  nullptr,           "GUI:GXHelpFriends",  L"your friends list, and who is online",            GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	{ "add",      L"<code> [name]",  "GUI:GXHelpAdd",      L"add somebody by friend code",                     GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	{ "remove",   L"<name or code>", "GUI:GXHelpRemove",   L"take somebody off your friends list",             GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	{ "mycode",   nullptr,           "GUI:GXHelpMyCode",   L"show your own friend code - keep it to yourself", GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	// ONLINE ONLY, and that mask is doing real work. This table is shared by both products, but
	// GXLoginFlow is ticked from GameClient and the relay lobby exists only in Zero Hour, so
	// offering /login on the LAN screen would be offering a verb that starts something no screen
	// there ever reports on. Logging out is a file delete and works anywhere, but it belongs beside
	// its opposite rather than on a screen where logging in is impossible.
	// (Since 23/09/2026 Zero Hour's LanLobbyMenuUpdate does drain GXLoginFlow::takeMessage, because
	// its relay notice tells the player to type /login and the dispatch below takes it on any screen;
	// the Generals LAN screen still reports nothing, so the mask stays.)
	{ "login",    nullptr,           "GUI:GXHelpLogin",    L"link your Discord account",                       GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	{ "logout",   nullptr,           "GUI:GXHelpLogout",   L"unlink it again",                                 GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
#if RTS_ZEROHOUR
	// GXApplyNickname is defined in GeneralsMD/.../WOLLobbyMenu.cpp and declared in the ZERO HOUR
	// GUICallbacks.h, so the Generals tree has nothing to call and must not be told it has. Core
	// sources are compiled once per product (they reach each target through the
	// corei_gameengine_private INTERFACE library), so this really does discriminate - the same way
	// Core/.../GameClient/MapUtil.cpp:994 already uses it.
	// GeneralsX @feature Claude 11/08/2026 GX_ON_ALL: the LAN browser now applies it too, and its
	// handler also moves the name box - LanLobbyMenuShutdown writes the pref FROM that box, so a
	// /nick that left it alone was silently reverted on leaving the screen.
	{ "nick",     L"<name>",         "GUI:GXHelpNick",     L"change the name everyone sees",                   GX_ON_ALL,         GX_LOBBY_GROUP_GAME    },
#endif
	{ "password", L"[text]",         "GUI:GXHelpPassword", L"require a password to join; no text clears it",   GX_ON_ALL,         GX_LOBBY_GROUP_GAME    },
	{ "private",  nullptr,           "GUI:GXHelpPrivate",  L"stop listing this game in the browser",           GX_ON_ALL,         GX_LOBBY_GROUP_GAME    },
	{ "public",   nullptr,           "GUI:GXHelpPublic",   L"list it again",                                   GX_ON_ALL,         GX_LOBBY_GROUP_GAME    },
};

static const Int s_commandCount = sizeof(s_commands) / sizeof(s_commands[0]);

/*
 * /login. The same feature on all four platforms, which it was not before: the loopback flow needs
 * no HTTP client in the game at all, so the Windows engine - which has no HTTPS and no CA store -
 * runs the identical code path.
 *
 * This is now the SECONDARY way in. The primary is pressing Online while signed out, which asks.
 */

/*
 * "Type /login again to renew" has to actually renew on the second /login.
 *
 * It did not. Every /login with a verified identity took the status branch and returned, so the
 * message was an instruction whose only effect was to reprint itself - the one case where a
 * player is TOLD to do something and the game quietly refuses. Found by the cross-model review of
 * the merged range, not by anyone using it, which is its own comment on how visible it was.
 *
 * The arm is the confirmation state the message already implies, and it is deliberately NOT a
 * timer: the prompt says "again", the player answers "again", and a clock that silently disarms
 * between the two would recreate the same dead end for a slow reader. It is process-lifetime and
 * one-shot - consumed the moment the flow starts - so it cannot open a browser twice, and it can
 * only ever be armed by the player having just been shown the prompt.
 */
static Bool s_renewArmed = FALSE;

// GeneralsX @feature Claude 18/09/2026 See GXLobbyCommands.h. Defined here, beside s_renewArmed,
// because disarming a renewal is part of signing out and that flag is private to this file.
GXSignOutResult GXLobbySignOut( void )
{
	// Read BEFORE anything is cleared - it is what the result reports, not a gate on the cleanup.
	const Bool wasSignedIn = GXPlayerIdentity::hasVerifiedIdentity();

	// Cancel FIRST, then drop the identity - see the /logout handler's note for why the order
	// matters: an approval that lands after the drop would otherwise be accepted.
	GXLoginFlow::cancel();
	s_renewArmed = FALSE;

	const GXSignInRemoval removed = GXPlayerIdentity::clearVerifiedIdentity();

	// The transport's copy. "" is the clear: sendRelayIdentity empties m_relayIdentity, and - only when
	// a relay room is joined - tells the relay to forget the account for this member, then repeats that
	// on every registration keepalive until a new sign-in is declared (one lost datagram must not leave
	// the account attached). On a LAN transport it just empties the copy.
	if (TheLAN != nullptr && TheLAN->getTransport() != nullptr)
		TheLAN->getTransport()->sendRelayIdentity("");

	// GeneralsX @bugfix Claude 18/09/2026 And the lobby presence socket's copy - the THIRD one, since
	// that socket now declares the identity too so that mute and ban reach lobby chat. Cleared whole,
	// with the clear the optimiser may not drop, and the relay is told to forget the account for that
	// membership. It is needed HERE and not only in the lobby's change detector: that socket lives for
	// the whole process, a sign-out leaves the lobby screen, and the detector only runs while the
	// screen is up - so relying on it would keep the token in memory, and the account bound to a
	// membership that never lapses, until the player happened to come back.
	if (TheGXLobbyPresence != NULL)
		TheGXLobbyPresence->clearIdentity();

	if (removed == GX_SIGNIN_FILE_SURVIVED)
		return GX_SIGNOUT_FILE_SURVIVED;
	if (removed == GX_SIGNIN_COPY_SURVIVED)
		return GX_SIGNOUT_COPY_SURVIVED;
	return wasSignedIn ? GX_SIGNOUT_DONE : GX_SIGNOUT_NOT_SIGNED_IN;
}

static void gxHandleLogin( GXLobbySayFunc say )
{
	if (GXPlayerIdentity::hasVerifiedIdentity())
	{
		UnicodeString line;
		UnicodeString name;
		name.translate(AsciiString(GXPlayerIdentity::getVerifiedName()));
		// GeneralsX @bugfix Claude 24/09/2026 BOT MODE HAS NO RENEWAL TO OFFER. A bot login lives 24 h, so it
		// always reads 0 days below: the first /login offered a renewal and the second promised a browser that
		// GXLoginFlow::start() refuses in bot mode, corrected one line late. Say who renews it; arm nothing.
		if (GXPlayerIdentity::isBotMode())
		{
			s_renewArmed = FALSE;
			line.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginBotMode",
				L"Logged in as %ls (bot mode). The operator renews GX_IDENTITY_FILE - not a browser.").str(),
				name.str());
			say(line, GX_HELP_ROW_COLOR);
			return;
		}
		const Int days = GXPlayerIdentity::getVerifiedDaysLeft();
		if (days <= 5)
		{
			if (s_renewArmed)
			{
				// The second /login. Consume the arm BEFORE starting, so a failed start cannot
				// leave it primed for an unrelated /login much later.
				s_renewArmed = FALSE;
				say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginRenewing",
					L"Renewing your sign-in - approve it in the browser."), GX_HELP_TITLE_COLOR);
				GXLoginFlow::start();
				return;
			}

			// The only proactive prompt in the whole feature. A browser window opening on its own
			// reads as malware, so the player is told and left to decide.
			s_renewArmed = TRUE;
			line.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginExpiring",
				L"Logged in as %ls - expires in %d days. Type /login again to renew.").str(),
				name.str(), days);
		}
		else
		{
			// A renewal that landed, or a fresh sign-in: whatever was armed is stale now.
			s_renewArmed = FALSE;
			line.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginAlready",
				L"Logged in as %ls, for another %d days.").str(), name.str(), days);
		}
		say(line, GX_HELP_ROW_COLOR);
		return;
	}

	// Signed out entirely - the arm belonged to an identity that is gone.
	s_renewArmed = FALSE;

	if (GXLoginFlow::state() == GX_LOGIN_WAITING)
	{
		say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginWaiting",
			L"Still waiting for your browser. Approve the sign-in there and come back."),
			GX_HELP_TITLE_COLOR);
		return;
	}

	// No platform split any more. The loopback flow needs no HTTP client, so the Windows engine
	// runs exactly this - which is why the launcher's --login path and the "use the launcher"
	// message that used to live here are both gone.
	GXLoginFlow::start();
	// start() queues its own line either way, so there is deliberately nothing to say here: two
	// messages for one action is worse than one.
}

/// A verb this build no longer has, or does not have YET, and what to do instead.
struct GXLobbyNoticeDef
{
	const char *verb;
	const char *helpLabel;
	const WideChar *helpText;
	UnsignedInt screens;
};

// THIS TABLE IS WHY PRUNING IS SAFE, and it is not tidiness.
//
// relaySubmitChat BROADCASTS a line it does not recognise. So retiring an alias without catching
// it means "/w bob meet me at the back" - a private-message verb this build no longer has - is
// said out loud to the whole room. That is the exact failure /msg exists to prevent, arrived at by
// a rename. Every retired spelling is named here, and GXLobbyGuardUnknownCommand refuses anything
// this table missed rather than sending it.
static const GXLobbyNoticeDef s_retired[] =
{
	{ "friend",     "GUI:GXGoneFriend",     L"/friend is now /friends to list, /add <code> to add somebody and /remove to drop them.", GX_ON_ONLINE },
#if RTS_ZEROHOUR
	// Scoped to exactly where /nick exists: every Zero Hour screen since the LAN browser gained it
	// (GeneralsX @feature Claude 11/08/2026), still nothing in the Generals tree. Outside this
	// scope /nickname falls through to the generic refusal, which is the honest answer there.
	{ "nickname",   "GUI:GXGoneNickname",   L"That is now /nick <name>.",                                                              GX_ON_ALL },
#endif
	{ "m",          "GUI:GXGoneMsgAlias",   L"That is now /msg <name> <text>. Nothing was sent.",                                      GX_ON_ONLINE },
	{ "w",          "GUI:GXGoneMsgAlias",   L"That is now /msg <name> <text>. Nothing was sent.",                                      GX_ON_ONLINE },
	{ "whisper",    "GUI:GXGoneMsgAlias",   L"That is now /msg <name> <text>. Nothing was sent.",                                      GX_ON_ONLINE },
	{ "tell",       "GUI:GXGoneMsgAlias",   L"That is now /msg <name> <text>. Nothing was sent.",                                      GX_ON_ONLINE },
	{ "reply",      "GUI:GXGoneReply",      L"That is now /r <text>. Nothing was sent.",                                               GX_ON_ONLINE },
	{ "pass",       "GUI:GXGonePass",       L"That is now /password <text>.",                                                          GX_ON_ALL    },
	{ "nopassword", "GUI:GXGoneNoPassword", L"Type /password with nothing after it to clear the password.",                            GX_ON_ALL    },
	{ "lobbyhelp",  "GUI:GXGoneLobbyHelp",  L"That is now /help.",                                                                     GX_ON_ALL    },
};

static const Int s_retiredCount = sizeof(s_retired) / sizeof(s_retired[0]);

// Names claimed for behaviour that is NOT built here, kept off the broadcast path so that six
// months of habit cannot cement them to something else.
//
// /friendsonly today filters the BROWSER. It is to mean "only my friends may join this lobby",
// with /hidden to keep a lobby off the list. That is Phase 6 - it needs RET_NOT_A_FRIEND, wire
// fields on GameToJoin/GameNotJoined, and sizeof(LANMessage) must stay 471, all inside the SimID
// digest - so it cannot ship here. The old meaning comes off the command surface (the dropdown
// above the game list already does it) and the new one answers honestly until it exists.
static const GXLobbyNoticeDef s_reserved[] =
{
	{ "friendsonly", "GUI:GXSoonFriendsOnly", L"Not built yet - this will make a lobby only your friends can join. To filter the game list, use the dropdown above it.", GX_ON_ALL },
	{ "friendonly",  "GUI:GXSoonFriendsOnly", L"Not built yet - this will make a lobby only your friends can join. To filter the game list, use the dropdown above it.", GX_ON_ALL },
	{ "hidden",      "GUI:GXSoonHidden",      L"Not built yet - this will keep a lobby off the browser. /private already stops it being listed.",                        GX_ON_ALL },
};

static const Int s_reservedCount = sizeof(s_reserved) / sizeof(s_reserved[0]);

static const char *s_groupLabel[GX_LOBBY_GROUP_COUNT] =
{
	"GUI:GXHelpGroupChat", "GUI:GXHelpGroupFriends", "GUI:GXHelpGroupGame"
};

static const WideChar *s_groupText[GX_LOBBY_GROUP_COUNT] =
{
	L"CHAT", L"FRIENDS", L"THIS GAME"
};

/// Split "/verb rest" into a lower-cased verb and the remainder. FALSE when it is not a command.
///
/// isEmpty is tested first because AsciiString::getCharAt carries a DEBUG_ASSERTCRASH on the
/// index, so reading [0] out of an empty string is a debug-build crash even though release
/// quietly returns 0.
static Bool gxSplitCommand( const AsciiString &line, AsciiString &verb, AsciiString &rest )
{
	if (line.isEmpty() || line.getCharAt(0) != '/')
		return FALSE;

	rest = line.str() + 1;
	rest.nextToken(&verb);
	verb.toLower();
	rest.trim();
	return !verb.isEmpty();
}

static Bool gxVisibleOn( UnsignedInt screens, GXLobbyScreen screen )
{
	return (screens & GX_SCREEN_BIT(screen)) != 0;
}

/// Match a verb against one of the notice tables. nullptr when it is not in it.
static const GXLobbyNoticeDef *gxFindNotice( const GXLobbyNoticeDef *table, Int count,
											 const AsciiString &verb, GXLobbyScreen screen )
{
	for (Int i = 0; i < count; ++i)
	{
		if (!gxVisibleOn(table[i].screens, screen))
			continue;
		if (verb == table[i].verb)
			return &table[i];
	}
	return nullptr;
}

/// One "  /verb <args>  -  description" row.
static UnicodeString gxFormatCommandRow( const GXLobbyCommandDef &def )
{
	UnicodeString head;
	if (def.args != nullptr)
		head.format(L"  /%hs %ls", def.verb, def.args);
	else
		head.format(L"  /%hs", def.verb);

	UnicodeString row;
	row.format(L"%ls  -  %ls", head.str(),
		TheGameText->FETCH_OR_SUBSTITUTE(def.helpLabel, def.helpText).str());
	return row;
}

/// The game list's colour key, which used to be one of the three banner paragraphs.
///
/// Kept as a /help TOPIC rather than a command: it is reference material a player wants once, and
/// a fifteenth verb for it would work against the point of this whole change.
static void gxRenderColours( GXLobbySayFunc say )
{
	say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXHelpColoursGames",
		L"Game list: normal is joinable, dimmed is full, an odd colour is a different build and "
		L"would refuse you. The icons after Slots are password, observers allowed, stats recorded."),
		GX_HELP_ROW_COLOR);
	say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXHelpColoursPlayers",
		L"Player list: green is you, blue is a friend, grey is somebody you are ignoring."),
		GX_HELP_ROW_COLOR);
}

/// The whole list: title, then each group that has anything on this screen, sorted by verb.
static void gxRenderHelp( GXLobbyScreen screen, GXLobbySayFunc say )
{
	say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXHelpTitle",
		L"Commands - only you see this. /help <command> explains one, /help colours explains the "
		L"game list."), GX_HELP_TITLE_COLOR);

	for (Int g = 0; g < GX_LOBBY_GROUP_COUNT; ++g)
	{
		// Collect this group's visible rows, then insertion-sort the indices by verb. Sorted at
		// render time rather than trusted from the table, so appending a row in the wrong place
		// cannot un-sort what the player reads.
		Int order[GX_LOBBY_HELP_MAX_ROWS];
		Int n = 0;
		for (Int i = 0; i < s_commandCount && n < GX_LOBBY_HELP_MAX_ROWS; ++i)
		{
			if (s_commands[i].group != (GXLobbyGroup)g)
				continue;
			if (!gxVisibleOn(s_commands[i].screens, screen))
				continue;

			Int j = n++;
			while (j > 0 && strcmp(s_commands[order[j - 1]].verb, s_commands[i].verb) > 0)
			{
				order[j] = order[j - 1];
				--j;
			}
			order[j] = i;
		}

		// e.g. FRIENDS on a LAN screen. Say nothing rather than print an empty heading, which
		// would read as "the friends commands are broken here" rather than "they are elsewhere".
		if (n == 0)
			continue;

		say(TheGameText->FETCH_OR_SUBSTITUTE(s_groupLabel[g], s_groupText[g]), GX_HELP_GROUP_COLOR);

		for (Int k = 0; k < n; ++k)
			say(gxFormatCommandRow(s_commands[order[k]]), GX_HELP_ROW_COLOR);
	}
}

/// /help <command>
static void gxRenderHelpFor( const AsciiString &wanted, GXLobbyScreen screen, GXLobbySayFunc say )
{
	AsciiString name = wanted;
	name.toLower();
	if (!name.isEmpty() && name.getCharAt(0) == '/')
		name = name.str() + 1;

	if (name == "colours" || name == "colors" || name == "colour" || name == "color")
	{
		gxRenderColours(say);
		return;
	}

	for (Int i = 0; i < s_commandCount; ++i)
	{
		if (!gxVisibleOn(s_commands[i].screens, screen))
			continue;
		if (name != s_commands[i].verb)
			continue;

		say(gxFormatCommandRow(s_commands[i]), GX_HELP_ROW_COLOR);
		return;
	}

	// Retired and reserved verbs answer here too, so "/help w" explains the rename instead of
	// denying that the command was ever real.
	const GXLobbyNoticeDef *notice = gxFindNotice(s_retired, s_retiredCount, name, screen);
	if (notice == nullptr)
		notice = gxFindNotice(s_reserved, s_reservedCount, name, screen);

	if (notice != nullptr)
	{
		say(TheGameText->FETCH_OR_SUBSTITUTE(notice->helpLabel, notice->helpText),
			GX_HELP_REFUSE_COLOR);
		return;
	}

	say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXHelpNoSuch",
		L"No such command here. /help on its own lists them."), GX_HELP_REFUSE_COLOR);
}

void GXLobbyPrintHelpHint( GXLobbyScreen screen, GXLobbySayFunc say )
{
	if (say == nullptr)
		return;

	if (screen == GX_LOBBY_SCREEN_ONLINE)
		say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLobbyBanner",
			L"Connected. Everyone here reads what you type - /help lists the commands."),
			GX_HELP_TITLE_COLOR);
	else
		say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLobbyBannerLan",
			L"/help lists the commands."), GX_HELP_TITLE_COLOR);
}

Bool GXLobbyChatCommand( const AsciiString &line, GXLobbyScreen screen,
						 GameWindow *chatListbox, GXLobbySayFunc say )
{
	AsciiString verb, rest;
	if (say == nullptr || !gxSplitCommand(line, verb, rest))
		return FALSE;

	if (verb == "help")
	{
		if (rest.isEmpty())
			gxRenderHelp(screen, say);
		else
			gxRenderHelpFor(rest, screen, say);
		return TRUE;
	}

	if (verb == "clear")
	{
		// The listbox IS the transcript. GameSpyInfo::addText writes straight into it and keeps no
		// separate buffer of its own (Chat.cpp:381), so resetting the listbox is the whole
		// implementation and nothing repopulates it afterwards.
		if (chatListbox != nullptr)
			GadgetListBoxReset(chatListbox);

		// GeneralsX @feature Claude 13/08/2026 The block above no longer exists - say so explicitly.
		// The entry-count guard inside GXLobbyChatRow catches this in every case but one: a box
		// holding a single row is emptied to zero, and zero is also the count it recorded before
		// that row went in, so "undisturbed" reads true and the hint below would print indented
		// under a header that is gone. Cheaper to be told than to reason about.
		GXLobbyChatRowReset();

		// Reprinted deliberately. The entry banner is now once per run, so without this a cleared
		// box would be a blank screen with nothing on it pointing at /help.
		GXLobbyPrintHelpHint(screen, say);
		return TRUE;
	}

	if (verb == "login")
	{
		gxHandleLogin(say);
		return TRUE;
	}

	if (verb == "logout")
	{
		// GeneralsX @feature Claude 18/09/2026 NOT INSIDE A RELAY GAME ROOM - a deliberate refusal.
		//
		// Signing out there used to be half a sign-out: the claims and the file went, but nothing
		// cleared the transport's copy, which went on re-declaring the account to the relay every
		// 30 s for as long as the room lasted. Clearing it properly is no better in a room: the relay
		// then holds a member with no identity, and a relay that requires one removes that member -
		// out of a room other players are waiting in. Neither is an acceptable result of typing a
		// command, so the room says no and says where it can be done. The lobbies are unaffected.
		//
		// RELAY ROOMS ONLY (cross-model review, 18/09/2026). A plain LAN room has no relay to hold
		// or drop anyone, so refusing there took away a working command for a hazard that does not
		// exist. Only a signed-in player is refused - with nothing to sign out of, there is nothing
		// a room could lose.
		const Bool relayRoom = (TheLAN != nullptr && TheLAN->getTransport() != nullptr
			&& TheLAN->getTransport()->isRelayEnabled());
		if (screen == GX_LOBBY_SCREEN_GAME && relayRoom && GXPlayerIdentity::hasVerifiedIdentity())
		{
			say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLogoutInRoom",
				L"Leave this game room first, then sign out from the lobby."), GX_HELP_REFUSE_COLOR);
			return TRUE;
		}
		// GeneralsX @bugfix wh1ter0se 11/08/2026 Cancel any flow still waiting on the browser, and
		// disarm the renewal, BEFORE dropping the identity.
		//
		// This became reachable with the renewal path above and was not before it: a waiting flow
		// used to imply signed-OUT, so /logout returned at the guard and never cleared anything.
		// A renewal is the first thing that holds a verified identity and a pending flow at the
		// same time. Without this, approving that renewal in the browser after typing /logout is
		// still accepted by GXLoginFlow::update(), which signs the player back in seconds after
		// the game told them they were signed out - and the listening socket outlives the
		// intent that opened it. cancel() is unconditional and idempotent. (Cross-model review.)
		//
		// GeneralsX @refactor Claude 18/09/2026 The cancel, the disarm and the delete now live in
		// GXLobbySignOut, which the lobby's right-click "Sign Out" shares - so the sequence above
		// is stated once. It also clears the transport's copy, which this verb never did.
		//
		// Deleting a file needs no HTTPS, so unlike logging IN this works on all four platforms
		// including Windows - which is why it is not behind SAGE_UPDATE_CHECK.
		//
		// THE RETURN VALUE IS REPORTED. The claims are dropped either way, so the player is signed
		// out for this session regardless - but if the file survived, they are signed back in at
		// the next launch, and a message saying "signed out" would be a lie that costs them a
		// second attempt to work out. (Cross-model review, 11/08/2026.)
		//
		// Each result has its own words because each is a different fact. NOT_SIGNED_IN keeps the
		// old reply; any leftovers were still cleaned up behind it.
		switch (GXLobbySignOut())
		{
			case GX_SIGNOUT_DONE:
				say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginOut",
					L"Signed out. Your friends list is untouched."), GX_HELP_ROW_COLOR);
				break;
			case GX_SIGNOUT_FILE_SURVIVED:
				say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginOutPartial",
					L"Signed out for now, but the account file could not be deleted - you will be "
					L"signed in again next launch."), GX_HELP_REFUSE_COLOR);
				break;
			case GX_SIGNOUT_COPY_SURVIVED:
				say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginOutCopy",
					L"Signed out, but a leftover copy (GXLogin.txt.tmp) could not be deleted. It is "
					L"never used to sign in; delete it by hand to remove it."), GX_HELP_REFUSE_COLOR);
				break;
			case GX_SIGNOUT_NOT_SIGNED_IN:
			default:
				say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLoginNotIn",
					L"You are not logged in."), GX_HELP_REFUSE_COLOR);
				break;
		}
		return TRUE;
	}

	const GXLobbyNoticeDef *notice = gxFindNotice(s_retired, s_retiredCount, verb, screen);
	if (notice == nullptr)
		notice = gxFindNotice(s_reserved, s_reservedCount, verb, screen);

	if (notice != nullptr)
	{
		say(TheGameText->FETCH_OR_SUBSTITUTE(notice->helpLabel, notice->helpText),
			GX_HELP_REFUSE_COLOR);
		return TRUE;
	}

	return FALSE;
}

// See the header note: the verb is matched on WIDE characters so the remainder - the name - can
// be taken from the original line without an ascii round trip.
Bool GXLobbyTakeNickRest( const UnicodeString &line, UnicodeString &outRest )
{
	static const WideChar s_verb[] = { L'n', L'i', L'c', L'k' };
	const WideChar *c = line.str();
	if (c == NULL || *c != L'/')
		return FALSE;
	++c;
	for (Int i = 0; i < 4; ++i, ++c)
	{
		WideChar ch = *c;
		if (ch >= L'A' && ch <= L'Z')
			ch = (WideChar)(ch - L'A' + L'a');
		if (ch != s_verb[i])
			return FALSE;
	}
	// Space OR tab, matching AsciiString::nextToken's separator set - a pasted "/nick\tBob" that
	// this helper refused would be recognised as a valid verb by the ascii dispatch and then
	// BROADCAST as chat by the guard, name and all (cross-model review's catch).
	if (*c != L'\0' && *c != L' ' && *c != L'\t')
		return FALSE;
	while (*c == L' ' || *c == L'\t')
		++c;
	outRest = UnicodeString(c);
	return TRUE;
}

Bool GXLobbyGuardUnknownCommand( AsciiString &line, GXLobbyScreen screen, GXLobbySayFunc say,
								 Bool &outEscaped )
{
	outEscaped = FALSE;

	if (line.isEmpty() || line.getCharAt(0) != '/')
		return FALSE;

	// "//text" is how you say something that really does begin with a slash. One slash is eaten
	// and the rest goes out verbatim. The caller is told, because on the LAN screens the line the
	// player actually typed is a UnicodeString and must NOT be re-derived from this narrowed copy
	// in any other case - see the note on the declaration.
	if (line.getLength() > 1 && line.getCharAt(1) == '/')
	{
		// BUT NOT WHEN THE SEND PATH WOULD READ IT BACK AS A COMMAND.
		//
		// The LAN screens do not send the line themselves - they hand it to LANAPI::RequestChat,
		// which parses it AGAIN with applyLobbyCommand (password, pass, nopassword, private,
		// public, lobbyhelp). So "//password hunter2" would be stripped to "/password hunter2"
		// here and then silently BIND A PASSWORD instead of saying the words, which is both a
		// surprise and a disclosure of intent the player did not make.
		//
		// The relay lobby has no such second parser - it calls applyLobbyCommand BEFORE this guard
		// and sends the bytes raw afterwards - so the escape is unrestricted there.
		//
		// The collision set is EXACTLY what applyLobbyCommand consumes, listed literally rather
		// than derived from the tables above. Deriving it from "every verb visible on this screen"
		// was too broad: it refused "//help" and "//clear", which LANAPI has never heard of and
		// would have forwarded as ordinary chat.
		//
		// A literal list is a duplicate of LANAPI.cpp:1189-1248 and that is deliberate. LANAPI.cpp
		// is inside the SimID source digest, so this branch may not edit it to export the set; and
		// a list that is too SHORT only costs a surprising side effect, while one that is too LONG
		// silently disables a documented escape. If a verb is ever added there, add it here.
		if (screen != GX_LOBBY_SCREEN_ONLINE)
		{
			static const char *const reparsedByLanApi[] =
			{
				"password", "pass", "nopassword", "private", "public", "lobbyhelp"
			};
			static const Int reparsedCount = sizeof(reparsedByLanApi) / sizeof(reparsedByLanApi[0]);

			AsciiString peeled = line.str() + 1;
			AsciiString peeledVerb, peeledRest;
			if (gxSplitCommand(peeled, peeledVerb, peeledRest))
			{
				Bool collides = FALSE;
				for (Int i = 0; !collides && i < reparsedCount; ++i)
				{
					if (peeledVerb == reparsedByLanApi[i])
						collides = TRUE;
				}

				if (collides && say != nullptr)
				{
					UnicodeString note;
					note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXEscapeCollides",
						L"A line starting with /%hs cannot be said literally on this screen - it "
						L"would be read as the command. Nothing was sent."), peeledVerb.str());
					say(note, GX_HELP_REFUSE_COLOR);
					return TRUE;
				}
			}
		}

		line = line.str() + 1;
		outEscaped = TRUE;
		return FALSE;
	}

	AsciiString verb, rest;
	if (!gxSplitCommand(line, verb, rest))
		return FALSE;		// a bare "/" is not a command; let it go as chat

	// A verb this screen owns has already been handled by the caller's own dispatcher - unless the
	// caller handles it AFTER this guard, which is exactly what the LAN screens do: /password,
	// /private and /public are intercepted inside LANAPI::RequestChat below us. Those must pass.
	for (Int i = 0; i < s_commandCount; ++i)
	{
		if (gxVisibleOn(s_commands[i].screens, screen) && verb == s_commands[i].verb)
			return FALSE;
	}

	if (say == nullptr)
		return FALSE;

	// REFUSED, not broadcast. Without this a mistyped or retired command is said out loud to the
	// whole room, and for the private-message verbs that publishes the message itself.
	UnicodeString note;
	note.format(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXUnknownCommand",
		L"There is no /%hs here, so nothing was sent. /help lists the commands; start the line "
		L"with // to say something that really begins with a slash."), verb.str());
	say(note, GX_HELP_REFUSE_COLOR);
	return TRUE;
}

// ================================================================================================
// GeneralsX @feature Claude 11/08/2026 Role presentation. See the declarations in the header.
// ================================================================================================

GXAssertRole GXLobbyRoleFromCode( const char *code )
{
	// The relay's one-letter roster field. Absent, empty, longer than one character or a letter
	// this build does not know are all GX_ROLE_PLAYER - the same fail-to-least-privilege rule
	// GXIdentityAssertion applies to the assertion itself, and for the same reason: a newer relay
	// must not be able to make an older client render something it cannot reason about.
	if (code == NULL || code[0] == '\0' || code[1] != '\0')
		return GX_ROLE_PLAYER;
	if (code[0] == 'm')
		return GX_ROLE_MOD;
	if (code[0] == 'a')
		return GX_ROLE_ADMIN;
	return GX_ROLE_PLAYER;
}

UnicodeString GXLobbyDecorateName( const UnicodeString &name, GXAssertRole role )
{
	UnicodeString out;
	switch (role)
	{
		case GX_ROLE_MOD:
			// Angle brackets AND the '@', so a moderator is distinguishable from an admin without
			// relying on colour at all. A player who cannot tell the two greens apart - or who is
			// reading a screenshot - still can.
			out.format(L"<@%ls>", name.str());
			break;
		case GX_ROLE_ADMIN:
			out.format(L"@%ls", name.str());
			break;
		case GX_ROLE_PLAYER:
		default:
			// Unchanged from what this screen has always drawn. An ordinary player's line must not
			// move a pixel because roles now exist.
			out.format(L"[%ls]", name.str());
			break;
	}
	return out;
}

UnicodeString GXLobbySaid( const UnicodeString &speaker, const UnicodeString &body )
{
	// Nobody is speaking: a room notice states something about the room and names no sender. It gets
	// its words back unchanged rather than a colon with nothing in front of it.
	if (speaker.isEmpty())
		return body;

	UnicodeString out = speaker;
	// getCharAt asserts on an empty string, which is why the isEmpty above is a guard and not a
	// tidiness check.
	if (gxChatNeedsSeparator((unsigned int)speaker.getCharAt(speaker.getLength() - 1)))
		out.concat((WideChar)GX_CHAT_SEPARATOR);
	out.concat(L' ');
	out.concat(body);
	return out;
}

UnicodeString GXLobbyOperatorName( GXAssertRole role )
{
	// GeneralsX @bugfix Claude 20/09/2026 THE ROLE IS AN ARGUMENT, because it used to be the constant
	// GX_ROLE_ADMIN and a moderator speaking through /api/say was drawn as "@Admin". The name and the
	// decoration move together on purpose: "<@Admin>" would be a third thing that is neither.
	if (role == GX_ROLE_MOD)
	{
		return GXLobbyDecorateName(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXModeratorName", L"Mod"),
			GX_ROLE_MOD);
	}
	return GXLobbyDecorateName(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXOperatorName", L"Admin"),
		GX_ROLE_ADMIN);
}

Color GXLobbyRoleTextColor( GXAssertRole role, Color fallback )
{
	// The fallback is passed in rather than named here so that Core does not have to reach into
	// GameNetwork/GameSpy for GSCOLOR_CHAT_NORMAL - and so that a screen with a different idea of
	// "an ordinary line" keeps it.
	switch (role)
	{
		case GX_ROLE_MOD:   return GX_CHAT_MOD_TEXT_COLOR;
		case GX_ROLE_ADMIN: return gxWebTone(GX_WEB_TONE_ADMIN, fallback);
		case GX_ROLE_PLAYER:
		default:            return fallback;
	}
}

Color GXLobbyRoleNameColor( GXAssertRole role, Color fallback )
{
	switch (role)
	{
		case GX_ROLE_MOD:   return GX_ROLE_MOD_NAME_COLOR;
		case GX_ROLE_ADMIN: return gxWebTone(GX_WEB_TONE_ADMIN, fallback);
		case GX_ROLE_PLAYER:
		default:            return fallback;
	}
}

UnicodeString GXLobbyTimestamp( void )
{
	// LOCAL time, not UTC. A player reads this against the clock on their own wall; a UTC stamp
	// would be correct and useless. No timezone suffix - it would cost width in a pane that wraps,
	// to say something the reader already knows.
	const time_t nowT = time(NULL);
	const struct tm *lt = localtime(&nowT);
	UnicodeString out;
	if (lt == NULL)
	{
		// localtime can return NULL on a broken clock. The line still has to be produced - these
		// helpers sit in front of the only sinks the lobby has, and returning nothing here drops a
		// message rather than a timestamp.
		return out;
	}
	out.format(L"%02d:%02d ", (Int)lt->tm_hour, (Int)lt->tm_min);
	return out;
}

UnicodeString GXLobbySystemPrefix( void )
{
	// The TAG goes first and the stamp follows it with no separator - "[S]04:44 ". See the design
	// note on the header for why the 2026-08-11 ordering was reversed.
	UnicodeString out(L"[S]");
	out.concat(GXLobbyTimestamp());
	return out;
}

UnicodeString GXLobbyRoomPrefix( void )
{
	// "[L]" does NOT collide with the OS column on the same screen: that column stores the wire tag
	// "L" but RENDERS "Linux" (WOLLobbyMenu.cpp). Checked, because it looked like a clash.
	UnicodeString out(L"[L]");
	out.concat(GXLobbyTimestamp());
	return out;
}

// GeneralsX @feature Claude 13/08/2026 One prefix per run of like lines. Design note on the header.

/// What a continuation line carries instead of the prefix.
///
/// AN INDENT AND NOTHING ELSE - no repeated glyph, no ditto mark. A per-line marker is the same
/// spam in a cheaper font, and the reader already has two stronger signals: the row is indented
/// where a stamped row is not, and it is the colour of the block it belongs to. Three spaces rather
/// than an attempt to align under the header's text, because the font is proportional (Arial 10) so
/// any space count that lines up at one resolution is wrong at the next, and a hanging indent that
/// is honestly approximate reads better than one that is nearly-but-not-quite a column.
static const WideChar *const GX_LOBBY_ROW_INDENT = L"   ";

/// Longest run that may share one prefix, header included.
///
/// The stamp is not allowed to disappear for an unbounded stretch: scrolling into the middle of a
/// long block should still land near something that says when. 32 is above the 20 rows /help prints
/// on the relay lobby (title, three headings, sixteen commands) so the listing is one block with
/// room to grow, and it is about three panes tall, so a stamp is never more than that far above.
#define GX_LOBBY_ROW_MAX_RUN	32

static GameWindow *s_rowBox = nullptr;		///< the listbox the open block lives in
static UnicodeString s_rowPrefix;			///< the prefix printed at the head of that block
static Int s_rowEntries = 0;				///< entries in s_rowBox as of the last call, pre-insert
static Int s_rowRun = 0;					///< rows in the open block; 0 means no block is open

void GXLobbyChatRowReset( void )
{
	s_rowBox = nullptr;
	s_rowPrefix.clear();
	s_rowEntries = 0;
	s_rowRun = 0;
}

UnicodeString GXLobbyChatRow( GameWindow *listbox, const UnicodeString &prefix,
							  const UnicodeString &body )
{
	// Read BEFORE the caller inserts, and compare against what we read before the previous insert.
	// Our own caller adds exactly one entry, so a healthy sequence steps by one; anything else means
	// somebody wrote to this box behind us, or it was emptied (/clear zeroes endPos,
	// GadgetListBox.cpp:1405), and either way the row above is no longer ours to continue.
	//
	// The one case this cannot see is a SATURATED box: at LENGTH 1000 with AUTOPURGE the count stops
	// growing, so an insert and an interleaved insert both leave it unchanged. Accepting "unchanged"
	// keeps grouping alive past the thousandth line at the cost of that detection; the alternative
	// is grouping switching itself off forever mid-session, which is worse and much harder to
	// explain. Nothing calls this out of range - the count is only ever compared, never indexed.
	const Int entries = GadgetListBoxGetNumEntries(listbox);
	const Bool undisturbed = (entries == s_rowEntries + 1) || (entries == s_rowEntries);

	const Bool folds = (listbox != nullptr)
					&& (listbox == s_rowBox)
					&& !prefix.isEmpty()
					&& (prefix == s_rowPrefix)
					&& (s_rowRun > 0)
					&& (s_rowRun < GX_LOBBY_ROW_MAX_RUN)
					&& undisturbed;

	UnicodeString out;
	if (folds)
	{
		out = UnicodeString(GX_LOBBY_ROW_INDENT);
		out.concat(body);
		++s_rowRun;
	}
	else
	{
		out = prefix;
		out.concat(body);
		s_rowBox = listbox;
		s_rowPrefix = prefix;
		// An empty prefix opens NO block. That is what makes player chat a hard boundary: it prints
		// bare, and the next tagged line cannot fold onto it.
		s_rowRun = prefix.isEmpty() ? 0 : 1;
	}

	s_rowEntries = entries;
	return out;
}
