/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 GeneralsX contributors
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

// See the design note on GXLobbyCommands.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/GXLobbyCommands.h"
#include "GameClient/GXLobbyPresence.h"		// GXLobbySignOut: the shared-lobby socket holds a copy of the sign-in
#include "GameClient/GXPlayerIdentity.h"
#include "GameClient/GXLoginFlow.h"
#include "GameClient/DisplayString.h"			// GXChatBoxUseTimeColumn: the stamp's width in the box's font
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindow.h"
#include "GameNetwork/LANAPI.h"			// GXLobbySignOut: TheLAN's transport holds a copy of the sign-in
#include "GameNetwork/LANAPICallbacks.h"	// TheLAN
#include "GameNetwork/Transport.h"

#include <stdlib.h>
#include <time.h>

#if defined(__APPLE__)
// GXLobbyTimestamp: the locale's short time pattern. CoreFoundation's header cannot be included in an engine
// file (its MacTypes.h redefines Byte against BaseTypeCore.h), so the five calls are declared by symbol, the way
// GXGfxProbe.h reaches the Objective-C runtime. CFIndex is a signed long, CFStringEncoding a UInt32, Boolean a UInt8.
extern "C" const void *gxCFLocaleCopyCurrent( void ) __asm__( "_CFLocaleCopyCurrent" );
extern "C" void *gxCFDateFormatterCreate( const void *allocator, const void *locale, long dateStyle, long timeStyle )
	__asm__( "_CFDateFormatterCreate" );
extern "C" const void *gxCFDateFormatterGetFormat( const void *formatter ) __asm__( "_CFDateFormatterGetFormat" );
extern "C" unsigned char gxCFStringGetCString( const void *string, char *buffer, long size, unsigned int encoding )
	__asm__( "_CFStringGetCString" );
extern "C" void gxCFRelease( const void *object ) __asm__( "_CFRelease" );
enum { GX_CF_DATE_NO_STYLE = 0, GX_CF_DATE_SHORT_STYLE = 1 };	// kCFDateFormatterNoStyle, kCFDateFormatterShortStyle
static const unsigned int GX_CF_STRING_UTF8 = 0x08000100u;	// kCFStringEncodingUTF8
#elif !defined(_WIN32)
#include <langinfo.h>
#include <locale.h>
#endif

// Colour literals live HERE rather than in multiplayer.ini, deliberately (SCOPE Phase 4f).
// multiplayer.ini ships inside the .big archives, so recolouring a help heading through data would
// move dataID and every peer would then refuse to join with DATA_DIFFERS - a four-platform rollout
// to change a colour.
static const Color GX_HELP_TITLE_COLOR  = GameMakeColor( 255, 200,  90, 255 );	// amber
static const Color GX_HELP_GROUP_COLOR  = GameMakeColor( 120, 200, 255, 255 );	// sky
static const Color GX_HELP_ROW_COLOR    = GameMakeColor( 215, 215, 215, 255 );	// near white
static const Color GX_HELP_REFUSE_COLOR = GameMakeColor( 255, 140, 140, 255 );	// soft red

// Role and system-line colours, kept beside the four above for the same dataID reason.
// A chat row carries one colour: the NAME colour goes to ListboxPlayers (COLUMNS: 2), the TEXT colour to the chat row.
// Keep GX_CHAT_SYSTEM_COLOR distinct from GSCOLOR_CHAT_OWNER (255,255,0), and ADMIN from whisper violet (220,160,255).
// The ADMIN colour is the console's panel.css --accent #D62B44 on both surfaces, taken from GXChatLine.h.
static Color gxWebTone( unsigned int rgb, Color fallback )
{
	if (rgb == (unsigned int)GX_WEB_TONE_NONE)
		return fallback;
	return GameMakeColor( (UnsignedByte)((rgb >> 16) & 0xFF), (UnsignedByte)((rgb >> 8) & 0xFF),
		(UnsignedByte)(rgb & 0xFF), 255 );
}

static const Color GX_CHAT_SYSTEM_COLOR     = GameMakeColor( 250, 240, 130, 255 );	// pale yellow

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
#define GX_ON_SCORE			GX_SCREEN_BIT(GX_LOBBY_SCREEN_SCORE)
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
	{ "help",     L"[command]",      "GUI:GXHelpHelp",     L"list the commands, or explain one",               GX_ON_ALL | GX_ON_SCORE, GX_LOBBY_GROUP_CHAT },
	{ "clear",    nullptr,           "GUI:GXHelpClear",    L"empty this chat box",                             GX_ON_ALL | GX_ON_SCORE, GX_LOBBY_GROUP_CHAT },
	// GeneralsX @feature No /me: not listed, so every screen's guard refuses
	// "/me x" with the unknown-command note and sends nothing; "//me x" is said literally, as a normal line.
	{ "msg",      L"<name> <text>",  "GUI:GXHelpMsg",      L"private message - only they see it",              GX_ON_ONLINE,      GX_LOBBY_GROUP_CHAT    },
	{ "r",        L"<text>",         "GUI:GXHelpReply",    L"answer the last person who messaged you",         GX_ON_ONLINE,      GX_LOBBY_GROUP_CHAT    },
	// /ignore takes a friend CODE too, for somebody absent.
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
	// (Zero Hour's LanLobbyMenuUpdate does drain GXLoginFlow::takeMessage, since its relay notice tells the
	// player to type /login; the Generals LAN screen reports nothing, so the mask stays.)
	{ "login",    nullptr,           "GUI:GXHelpLogin",    L"link your Discord account",                       GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
	{ "logout",   nullptr,           "GUI:GXHelpLogout",   L"unlink it again",                                 GX_ON_ONLINE,      GX_LOBBY_GROUP_FRIENDS },
#if RTS_ZEROHOUR
	// GXApplyNickname is defined in GeneralsMD/.../WOLLobbyMenu.cpp and declared in the ZERO HOUR
	// GUICallbacks.h, so the Generals tree has nothing to call and must not be told it has. Core
	// sources are compiled once per product (they reach each target through the
	// corei_gameengine_private INTERFACE library), so this really does discriminate - the same way
	// Core/.../GameClient/MapUtil.cpp:994 already uses it.
	// GX_ON_ALL: the LAN browser now applies it too, and its
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
 * "Type /login again to renew" must renew on the second /login; the arm is that confirmation state.
 * It is deliberately not a timer (a slow reader would be disarmed) and is one-shot: consumed when the flow starts,
 * so it cannot open a browser twice, and only the prompt arms it.
 */
static Bool s_renewArmed = FALSE;

// See GXLobbyCommands.h. Defined here, beside s_renewArmed,
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

	// And the lobby presence socket's copy - the THIRD one, since
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
		// BOT MODE HAS NO RENEWAL TO OFFER. A bot login lives 24 h, so it
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
	// (GeneralsX:), still nothing in the Generals tree. Outside this
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

	// The score screen runs /help and /clear only; the guard refuses everything else typed there.
	if (screen == GX_LOBBY_SCREEN_SCORE && verb != "help" && verb != "clear")
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

		// The block above no longer exists - say so explicitly.
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
		// NOT INSIDE A RELAY GAME ROOM: signing out there leaves the relay a member with no identity, and a relay that
		// requires one removes that member from a room others are waiting in. So the room refuses and says where to do it.
		// Relay rooms only, and only a signed-in player is refused; plain LAN rooms and the lobbies are unaffected.
		const Bool relayRoom = (TheLAN != nullptr && TheLAN->getTransport() != nullptr
			&& TheLAN->getTransport()->isRelayEnabled());
		if (screen == GX_LOBBY_SCREEN_GAME && relayRoom && GXPlayerIdentity::hasVerifiedIdentity())
		{
			say(TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXLogoutInRoom",
				L"Leave this game room first, then sign out from the lobby."), GX_HELP_REFUSE_COLOR);
			return TRUE;
		}
		// Cancel any flow waiting on the browser and disarm the renewal BEFORE dropping the identity: a renewal holds both
		// a verified identity and a pending flow, and approving it after /logout would sign the player straight back in.
		// The sequence is GXLobbySignOut, shared with the lobby's right-click Sign Out. Its result is reported: if the file
		// survived, the player is signed back in next launch, so "signed out" would be wrong.
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
	// BROADCAST as chat by the guard, name and all.
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
		// BUT NOT when the send path would re-read it as a command: LAN screens hand the line to LANAPI::RequestChat, which
		// parses it again with applyLobbyCommand, so "//password x" stripped to "/password x" would bind a password.
		// The relay lobby has no second parser, so the escape is unrestricted there.
		// The verb list is a literal copy of what applyLobbyCommand consumes (LANAPI.cpp is digested): add new verbs here too.
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
// Role presentation. See the declarations in the header.
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
	// GeneralsX @feature One name shape on every chat surface: "[Name]", "[@Name]" for an admin,
	// "[+Name]" for a moderator. The mark comes from the role only; any role mark at the front of the
	// name itself is dropped (GXChatLine.h gxChatNameStart), so no name can draw as a role.
	const UnicodeString bare( name.str() + gxChatNameStart( name.str() ) );
	UnicodeString out;
	switch (role)
	{
		case GX_ROLE_MOD:
			out.format(L"[+%ls]", bare.str());
			break;
		case GX_ROLE_ADMIN:
			out.format(L"[@%ls]", bare.str());
			break;
		case GX_ROLE_PLAYER:
		default:
			out.format(L"[%ls]", bare.str());
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

UnicodeString GXMatchOperatorName( GXAssertRole role )
{
	// GeneralsX @feature In a match the player lines are drawn by ConnectionManager::processChat (in the
	// simulation digest) as "[<their own name>]: text" with no name check, so a bracketed operator name
	// could be copied by a player called "@Admin". An unbracketed one cannot: every player line there
	// starts with '['.
	UnicodeString out;
	if (role == GX_ROLE_MOD)
		out.format(L"<@%ls>", TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXModeratorName", L"Mod").str());
	else
		out.format(L"@%ls", TheGameText->FETCH_OR_SUBSTITUTE("GUI:GXOperatorName", L"Admin").str());
	return out;
}

UnicodeString GXLobbyOperatorName( GXAssertRole role )
{
	// THE ROLE IS AN ARGUMENT, because it used to be the constant
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
		case GX_ROLE_MOD:   return gxWebTone(GX_CHAT_TONE_MOD, fallback);
		case GX_ROLE_ADMIN: return gxWebTone(GX_CHAT_TONE_ADMIN, fallback);
		case GX_ROLE_PLAYER:
		default:            return fallback;
	}
}

Color GXLobbyRoleNameColor( GXAssertRole role, Color fallback )
{
	switch (role)
	{
		case GX_ROLE_MOD:   return gxWebTone(GX_CHAT_TONE_MOD, fallback);
		case GX_ROLE_ADMIN: return gxWebTone(GX_CHAT_TONE_ADMIN, fallback);
		case GX_ROLE_PLAYER:
		default:            return fallback;
	}
}

// GeneralsX @feature The OS's short time pattern, as the platform spells it; "" when it cannot be read.
static void gxOsShortTimePattern( char *out, size_t size )
{
	out[0] = '\0';
#if defined(_WIN32)
	wchar_t wide[80];
	if (GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STIMEFORMAT, wide, (int)(sizeof(wide) / sizeof(wide[0]))) > 0)
	{
		size_t i = 0;
		for (; wide[i] != L'\0' && i + 1 < size; ++i)
			out[i] = (wide[i] < 0x80) ? (char)wide[i] : '?';	// only the ASCII field letters matter
		out[i] = '\0';
	}
#elif defined(__APPLE__)
	// The short time style follows the user's own 12/24-hour switch, not just the region default.
	const void *locale = gxCFLocaleCopyCurrent();
	if (locale != NULL)
	{
		void *fmt = gxCFDateFormatterCreate(NULL, locale, GX_CF_DATE_NO_STYLE, GX_CF_DATE_SHORT_STYLE);
		if (fmt != NULL)
		{
			const void *pattern = gxCFDateFormatterGetFormat(fmt);	// owned by the formatter
			if (pattern == NULL || gxCFStringGetCString(pattern, out, (long)size, GX_CF_STRING_UTF8) == 0)
				out[0] = '\0';
			gxCFRelease(fmt);
		}
		gxCFRelease(locale);
	}
#else
	// The environment's LC_TIME, read without touching the process-wide locale.
	locale_t loc = newlocale(LC_TIME_MASK, "", (locale_t)0);
	if (loc != (locale_t)0)
	{
		const char *pattern = nl_langinfo_l(T_FMT, loc);
		if (pattern != NULL)
		{
			strncpy(out, pattern, size - 1);
			out[size - 1] = '\0';
		}
		freelocale(loc);
	}
#endif
}

// TRUE when chat stamps read 12-hour. Asked once per process. GX_CHAT_CLOCK=12 or 24 overrides it, for
// tests and pictures.
static Bool gxChatClockTwelveHour( void )
{
	static Int s_twelve = -1;
	if (s_twelve < 0)
	{
		const char *forced = getenv("GX_CHAT_CLOCK");
		if (forced != NULL && strcmp(forced, "12") == 0)
			s_twelve = 1;
		else if (forced != NULL && strcmp(forced, "24") == 0)
			s_twelve = 0;
		else
		{
			char pattern[96];
			gxOsShortTimePattern(pattern, sizeof(pattern));
			s_twelve = gxChatTwelveHourFromPattern(pattern) ? 1 : 0;
		}
	}
	return s_twelve == 1;
}

UnicodeString GXLobbyTimestamp( void )
{
	// LOCAL time, not UTC. A player reads this against the clock on their own wall; a UTC stamp
	// would be correct and useless. No timezone suffix - it would cost width in a pane that wraps,
	// to say something the reader already knows.
	//
	// The reader's own clock format: "21:16 " on a 24-hour system, "9:16PM " on a 12-hour one
	// (GXChatLine.h gxChatClockText).
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
	char stamp[GX_CHAT_CLOCK_MAX];
	gxChatClockText((Int)lt->tm_hour, (Int)lt->tm_min, gxChatClockTwelveHour() != FALSE, stamp);
	out.translate(AsciiString(stamp));
	return out;
}

UnicodeString GXLobbySystemPrefix( void )
{
	// The TAG goes first and the stamp follows it with no separator - "[S]04:44 ". See the design note on the header.
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

// One prefix per run of like lines. Design note on the header.

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

// ---------------------------------------------------------------------------------------------
// GeneralsX @feature THE CHAT BOXES: a gray time column beside the words.
//
// A listbox row draws each CELL in one colour (DisplayString::draw takes one Color, and there is no
// colour markup), so a gray time beside white or crimson words needs two cells: the chat boxes are
// one column in their .wnd and are given a second here, when the screen creates them. Rows allocate
// their cells lazily with the box's column count, so the switch happens on an empty box (reset first)
// and every row after it has two cells. A box this cannot change stays one column and gets the time
// inline, in the line's colour.
//
// EVERY WRITER TO A TWO-COLUMN BOX COMES THROUGH GXChatBoxAdd. A row added without a column lands in
// column 0, the narrow time column; GXChatBoxAdopt moves such a row (written by code that cannot be
// changed here) into the text column.

static const Color GX_CHAT_MUTED_COLOR = GameMakeColor( (GX_CHAT_TONE_MUTED >> 16) & 0xFF, (GX_CHAT_TONE_MUTED >> 8) & 0xFF,
	GX_CHAT_TONE_MUTED & 0xFF, 255 );

Color GXChatPlayerColor( void )
{
	return GameMakeColor( (GX_CHAT_TONE_PLAYER >> 16) & 0xFF, (GX_CHAT_TONE_PLAYER >> 8) & 0xFF, GX_CHAT_TONE_PLAYER & 0xFF, 255 );
}

Color GXChatMutedColor( void )
{
	return GX_CHAT_MUTED_COLOR;
}

Color GXChatNoticeColor( Color requested )
{
	(void)requested;		// every notice is gray: a red refusal read like an admin's crimson
	return GX_CHAT_MUTED_COLOR;
}

Bool GXChatBoxUseTimeColumn( GameWindow *listbox )
{
	GXLobbyChatRowReset();		// a new box: its first notice never continues the last box's run
	if (listbox == nullptr)
		return FALSE;
	ListboxData *list = (ListboxData *)listbox->winGetUserData();
	if (list == nullptr || list->columns != 1 || list->columnWidth == nullptr || list->listData == nullptr)
		return FALSE;

	Int totalWidth = 0, height = 0;
	listbox->winGetSize(&totalWidth, &height);
	if (list->slider)
	{
		ICoord2D sliderSize;
		list->slider->winGetSize(&sliderSize.x, &sliderSize.y);
		totalWidth -= sliderSize.x;
	}

	// The widest stamp this clock prints, in the box's own font.
	Int stampWidth = 0;
	if (TheDisplayStringManager != nullptr)
	{
		DisplayString *probe = TheDisplayStringManager->newDisplayString();
		if (probe != nullptr)
		{
			probe->setFont(listbox->winGetFont());
			probe->setText(gxChatClockTwelveHour() ? UnicodeString(L"12:00PM") : UnicodeString(L"00:00"));
			stampWidth = probe->getWidth();
			TheDisplayStringManager->freeDisplayString(probe);
		}
	}
	if (stampWidth <= 0 || totalWidth <= 0)
		return FALSE;

	// Wide enough that the stamp never wraps (a cell wraps at its width less TEXT_WIDTH_OFFSET), plus
	// a gap before the words.
	const Int need = stampWidth + TEXT_WIDTH_OFFSET + TEXT_X_OFFSET;
	Int timePct = (need * 100 + totalWidth - 1) / totalWidth;
	while (timePct < 50 && timePct * totalWidth / 100 < need)
		++timePct;
	if (timePct >= 50)
		return FALSE;		// a box too narrow for a time column keeps the time inline

	GadgetListBoxReset(listbox);		// frees every row's one-column cells
	for (Int i = 0; i < list->listLength; ++i)
	{
		if (list->listData[i].cell != nullptr)
			return FALSE;
	}

	Int *pct = NEW Int[2];
	Int *widths = NEW Int[2];
	pct[0] = timePct;
	pct[1] = 100 - timePct;
	for (Int i = 0; i < 2; ++i)
		widths[i] = pct[i] * totalWidth / 100;
	delete[] list->columnWidth;
	delete[] list->columnWidthPercentage;
	list->columnWidth = widths;
	list->columnWidthPercentage = pct;
	list->columns = 2;
	return TRUE;
}

static Bool gxChatBoxHasTimeColumn( GameWindow *listbox )
{
	const ListboxData *list = (listbox != nullptr) ? (const ListboxData *)listbox->winGetUserData() : nullptr;
	return list != nullptr && list->columns == 2;
}

Int GXChatBoxAdd( GameWindow *listbox, const UnicodeString &tag, const UnicodeString &body, Color color )
{
	if (listbox == nullptr)
		return -1;

	const UnicodeString stamp = GXLobbyTimestamp();
	if (body.isEmpty())
		return GXChatBoxAdd(listbox, tag, UnicodeString(L" "), color);	// column 1 is never empty (GXChatBoxAdopt)
	Int index = -1;
	if (!gxChatBoxHasTimeColumn(listbox))
	{
		UnicodeString text;
		if (tag.isEmpty())
		{
			UnicodeString spoken = stamp;
			spoken.concat(body);
			text = GXLobbyChatRow(listbox, UnicodeString::TheEmptyString, spoken);
		}
		else
		{
			UnicodeString prefix = tag;
			prefix.concat(stamp);
			text = GXLobbyChatRow(listbox, prefix, body);
		}
		index = GadgetListBoxAddEntryText(listbox, text, color, -1, -1);
	}
	else
	{
		UnicodeString time = stamp;
		time.trim();
		UnicodeString text;
		if (tag.isEmpty())
		{
			GXLobbyChatRow(listbox, UnicodeString::TheEmptyString, body);	// somebody talking closes a notice run
			text = body;
		}
		else
		{
			// A run of notices in the same minute prints its tag and time once, as in a one-column box.
			UnicodeString prefix = tag;
			prefix.concat(stamp);
			const UnicodeString row = GXLobbyChatRow(listbox, prefix, body);
			if (row.startsWith(prefix))
			{
				text = tag;
				text.concat(L' ');
				text.concat(body);
			}
			else
			{
				time.clear();
				text = row;
			}
		}
		index = GadgetListBoxAddEntryText(listbox, time, GX_CHAT_MUTED_COLOR, -1, 0);
		if (index >= 0)
			GadgetListBoxAddEntryText(listbox, text, color, index, 1);
	}
	if (index >= 0)
		GadgetListBoxSetItemData(listbox, (void *)-1, index);
	return index;
}

void GXChatBoxAdopt( GameWindow *listbox )
{
	// The whole box, not just the newest rows: a full box drops its top row as it adds one, so "rows
	// added since" is not an index. A row GXChatBoxAdd wrote always has text in column 1.
	if (!gxChatBoxHasTimeColumn(listbox))
		return;
	Bool adopted = FALSE;
	const Int rows = GadgetListBoxGetNumEntries(listbox);
	for (Int row = 0; row < rows; ++row)
	{
		if (!GadgetListBoxGetText(listbox, row, 1).isEmpty())
			continue;
		Color written = 0;
		const UnicodeString text = GadgetListBoxGetTextAndColor(listbox, &written, row, 0);
		if (text.isEmpty())
			continue;
		UnicodeString time = GXLobbyTimestamp();
		time.trim();
		UnicodeString notice(L"[S] ");
		notice.concat(text);
		GadgetListBoxAddEntryText(listbox, time, GX_CHAT_MUTED_COLOR, row, 0);
		GadgetListBoxAddEntryText(listbox, notice, GXChatNoticeColor(written), row, 1);
		adopted = TRUE;
	}
	if (!adopted)
		return;

	// An overwrite only ever grows a row, and the adopted row grew while its words wrapped in the narrow
	// time column. Lay the box out again exactly as GadgetListBox.cpp computeTotalHeight does (it is static
	// there), then let GLM_UPDATE_DISPLAY place the view at the end.
	ListboxData *list = (ListboxData *)listbox->winGetUserData();
	const Bool oneLine = BitIsSet(listbox->winGetStatus(), WIN_STATUS_ONE_LINE);
	Int height = 0;
	for (Int i = 0; i < list->endPos; ++i)
	{
		ListEntryRow &entry = list->listData[i];
		if (entry.cell == nullptr)
			continue;
		Int rowHeight = 0;
		for (Int j = 0; j < list->columns; ++j)
		{
			Int cellHeight = 0;
			if (entry.cell[j].cellType == LISTBOX_TEXT)
			{
				if (oneLine)
					cellHeight = TheWindowManager->winFontHeight(listbox->winGetFont());
				else if (entry.cell[j].data != nullptr)
					((DisplayString *)entry.cell[j].data)->getSize(nullptr, &cellHeight);
			}
			if (cellHeight > rowHeight)
				rowHeight = cellHeight;
		}
		entry.height = rowHeight;
		height += rowHeight + 1;
		entry.listHeight = height;
	}
	list->totalHeight = height;
	TheWindowManager->winSendSystemMsg(listbox, GLM_UPDATE_DISPLAY, (WindowMsgData)list->endPos, 0);
}
