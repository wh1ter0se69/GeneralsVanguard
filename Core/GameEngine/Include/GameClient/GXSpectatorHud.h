/*
**	GeneralsX: the pure half of the GeneralsX spectator HUD (the owner's pick of 05/10/2026:
**	"C2" caster layout + the "B3" Tab scoreboard, in the game's blue). The drawing lives in
**	GeneralsMD/Code/GameEngine/Source/GameClient/GXSpectatorHud.inl; this header holds every decision that can go
**	wrong without anything looking wrong, so it can be driven with no engine:
**	scripts/test/spectator-hud-model-test.cpp (`c++ -std=c++17 -I Core/GameEngine/Include ...`).
**
**	  * SETTINGS live in <userdata>/GXHud.ini next to the HUD anchor, NOT in Options.ini: a key read through
**	    GlobalData/OptionPreferences would sit in Core/.../Common, inside the SimID digest, and a display toggle would
**	    then split every lobby (InGameUI.cpp's GXHud note). The launcher writes the same keys; the in-game panel writes
**	    them back with writeSettings(), which keeps every line it does not own.
**	  * KEYBINDS are `MOD+KEY_x` values in CommandButtonHotKeys.ini, the format the launcher's Keybinds > GENERAL tab
**	    already writes (MetaEvent.cpp gxParseChord). Unlike those, a bare key is allowed: these fire for a spectator
**	    only, who has no command a bare key could also mean.
**	  * STATUS: "Left" (quit or dropped) wins over "Dead", because a player who leaves is also destroyed a moment later
**	    and the reason is the news. A replay has no network, so it can only ever say "Dead".
**	  * DOCKING: everything starts docked to its tab; a panel can be dragged anywhere; clicking the tab
**	    of a MOVED panel snaps it home instead of closing it.
**
**	Client-only, read by nothing the simulation runs; no engine types, so it cannot reach the digest either.
*/

#pragma once

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "GameClient/GXReplaceFile.h"

namespace GXSpectatorHud
{

// ---------------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------------
enum ShowItem
{
	SHOW_PORTRAITS, SHOW_NAMES, SHOW_CASH, SHOW_INCOME, SHOW_POWER, SHOW_XP, SHOW_UNITS_BUILDINGS,
	SHOW_CLOCK_TOTALS, SHOW_EVENT_TOAST, SHOW_FOLLOW_PANEL, SHOW_MINIMAP, SHOW_STATUS_TIME, SHOW_TAB_BOARD,
	SHOW_COUNT
};

inline const char *showKey( int item )
{
	static const char *const keys[SHOW_COUNT] = {
		"SpectatorShowPortraits", "SpectatorShowNames", "SpectatorShowCash", "SpectatorShowIncome",
		"SpectatorShowPower", "SpectatorShowXP", "SpectatorShowUnitsBuildings", "SpectatorShowClockTotals",
		"SpectatorShowEventToast", "SpectatorShowFollowPanel", "SpectatorShowMinimap", "SpectatorShowStatusTime",
		"SpectatorShowTabBoard" };
	return (item >= 0 && item < SHOW_COUNT) ? keys[item] : "";
}

/// Units and buildings are off by default: the round-3 design keeps them for the follow panel and the Tab board.
inline bool showDefault( int item ) { return item != SHOW_UNITS_BUILDINGS; }

enum { OPACITY_MIN = 25, OPACITY_MAX = 100, OPACITY_DEFAULT = 50, OPACITY_STEP = 5 };

inline int clampOpacity( int v ) { return v < OPACITY_MIN ? OPACITY_MIN : (v > OPACITY_MAX ? OPACITY_MAX : v); }
inline int stepOpacity( int cur, int dir ) { return clampOpacity( cur + (dir < 0 ? -OPACITY_STEP : OPACITY_STEP) ); }

struct Settings
{
	bool generalsx;					///< SpectatorUI=generalsx; anything else, or absent, is legacy. The ONE switch: it
													///< drives the GeneralsX UI while spectating AND while playing
	int opacity;						///< panel plates, 25..100 %, spectating and playing alike
	bool show[SHOW_COUNT];
	bool hideDead;					///< hide Dead and Left players from the bar and the board
	bool playerStrip;				///< PlayerShowStrip: while playing, the economy line on the command bar
	bool playerTabBoard;		///< PlayerShowTabBoard: while playing, Tab holds the player board (0 = Tab stays Diplomacy)
	Settings() : generalsx( false ), opacity( OPACITY_DEFAULT ), hideDead( false ), playerStrip( true ), playerTabBoard( true )
	{
		for (int i = 0; i < SHOW_COUNT; ++i)
			show[i] = showDefault( i );
	}
};

inline bool equalsNoCase( const std::string &a, const char *b )
{
	size_t i = 0;
	for (; i < a.size() && b[i] != '\0'; ++i)
		if (std::tolower( (unsigned char)a[i] ) != std::tolower( (unsigned char)b[i] ))
			return false;
	return i == a.size() && b[i] == '\0';
}

inline std::string trim( const std::string &s )
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t'))
		++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
		--e;
	return s.substr( b, e - b );
}

/// Splits "key = value"; false for blank, comment or key-less lines. The KEY match is exact (case-insensitive), so
/// "SpectatorUIX" is never read or rewritten as "SpectatorUI".
inline bool splitLine( const std::string &line, std::string &key, std::string &value )
{
	const std::string t = trim( line );
	if (t.empty() || t[0] == ';' || t[0] == '#')
		return false;
	const size_t eq = t.find( '=' );
	if (eq == std::string::npos || eq == 0)
		return false;
	key = trim( t.substr( 0, eq ) );
	value = trim( t.substr( eq + 1 ) );
	return !key.empty();
}

/// strtol semantics: "no digits" keeps the fallback rather than silently becoming 0 (same as gxHudSetting).
inline int parseIntOr( const std::string &v, int fallback )
{
	const char *p = v.c_str();
	char *end = nullptr;
	const long n = std::strtol( p, &end, 10 );
	return end != p ? (int)n : fallback;
}

inline Settings parseSettings( const std::string &iniText )
{
	Settings s;
	size_t pos = 0;
	while (pos < iniText.size())
	{
		size_t nl = iniText.find( '\n', pos );
		if (nl == std::string::npos)
			nl = iniText.size();
		std::string key, value;
		if (splitLine( iniText.substr( pos, nl - pos ), key, value ))
		{
			if (equalsNoCase( key, "SpectatorUI" ))
				s.generalsx = equalsNoCase( value, "generalsx" );
			else if (equalsNoCase( key, "SpectatorOpacity" ))
				s.opacity = clampOpacity( parseIntOr( value, s.opacity ) );
			else if (equalsNoCase( key, "SpectatorHideDead" ))
				s.hideDead = parseIntOr( value, s.hideDead ? 1 : 0 ) != 0;
			else if (equalsNoCase( key, "PlayerShowStrip" ))
				s.playerStrip = parseIntOr( value, s.playerStrip ? 1 : 0 ) != 0;
			else if (equalsNoCase( key, "PlayerShowTabBoard" ))
				s.playerTabBoard = parseIntOr( value, s.playerTabBoard ? 1 : 0 ) != 0;
			else
				for (int i = 0; i < SHOW_COUNT; ++i)
					if (equalsNoCase( key, showKey( i ) ))
						s.show[i] = parseIntOr( value, s.show[i] ? 1 : 0 ) != 0;
		}
		pos = nl + 1;
	}
	return s;
}

/// Replace the first `key=` line in place (later duplicates are dropped), or append one. Every other line - the
/// launcher's anchor, comments, unknown keys - is kept byte for byte.
inline std::string mergeSetting( const std::string &iniText, const std::string &key, const std::string &value )
{
	std::string out;
	bool done = false;
	size_t pos = 0;
	while (pos < iniText.size())
	{
		size_t nl = iniText.find( '\n', pos );
		const bool hasNl = nl != std::string::npos;
		if (!hasNl)
			nl = iniText.size();
		const std::string line = iniText.substr( pos, nl - pos );
		std::string k, v;
		if (splitLine( line, k, v ) && equalsNoCase( k, key.c_str() ))
		{
			if (!done)
				out += key + "=" + value + "\n";
			done = true;
		}
		else
		{
			out += line;
			out += "\n";
		}
		pos = nl + 1;
	}
	if (!done)
		out += key + "=" + value + "\n";
	return out;
}

inline std::string writeSettings( const std::string &iniText, const Settings &s )
{
	std::string out = mergeSetting( iniText, "SpectatorUI", s.generalsx ? "generalsx" : "legacy" );
	out = mergeSetting( out, "SpectatorOpacity", std::to_string( clampOpacity( s.opacity ) ) );
	for (int i = 0; i < SHOW_COUNT; ++i)
		out = mergeSetting( out, showKey( i ), s.show[i] ? "1" : "0" );
	out = mergeSetting( out, "SpectatorHideDead", s.hideDead ? "1" : "0" );
	out = mergeSetting( out, "PlayerShowStrip", s.playerStrip ? "1" : "0" );
	return mergeSetting( out, "PlayerShowTabBoard", s.playerTabBoard ? "1" : "0" );
}

// ---------------------------------------------------------------------------------------------------------------------
// The file
//
// GeneralsX: The panel's own reader took ANY fopen failure, and an fread error, for an empty
// GXHud.ini (file-wipe class, the sweep after). The load then gave the defaults - the LEGACY UI among them -
// and the next toggle merged them into nothing and renamed that over the file: the launcher's HUD anchor, the message
// delay, SpectatorUI=generalsx and every other line were gone. ONLY a file that is not there (ENOENT) is empty now. An
// existing file that cannot be read is never replaced, and a match whose load could not read it saves nothing, even
// once the file is readable again: what it would save is the defaults, not the player's settings.
// ---------------------------------------------------------------------------------------------------------------------
enum FileRead { FILE_LOADED, FILE_MISSING, FILE_UNREADABLE };

inline FileRead readWholeFile( const char *path, std::string &out, int *err )
{
	out.clear();
	errno = 0;
	FILE *f = fopen( path, "rb" );
	if (f == nullptr)
	{
		*err = errno;
		// A dangling symlink opens as ENOENT too (gxNameExists, GXReplaceFile.h).
		return (*err == ENOENT && !gxNameExists( path )) ? FILE_MISSING : FILE_UNREADABLE;
	}
	char buf[4096];
	size_t n;
	while ((n = fread( buf, 1, sizeof( buf ), f )) > 0)
		out.append( buf, n );
	const bool failed = ferror( f ) != 0;	// fread's 0 is the end OR an error (EISDIR for a directory, EIO)
	*err = failed ? errno : 0;
	fclose( f );
	return failed ? FILE_UNREADABLE : FILE_LOADED;
}

inline std::string unreadableNote( const char *path, int err, const char *what )
{
	return std::string( path ) + " exists but could not be read (" + strerror( err ) + ", errno " + std::to_string( err )
		+ "): it is left exactly as it is and " + what;
}

/// One per panel: load() at the start of a match, save() on every toggle. `why` gets the sentence for the log when
/// either refuses.
struct SettingsFile
{
	bool unreadable;	///< the last load() could not read an existing file, so save() writes nothing until one does
	SettingsFile() : unreadable( false ) {}

	Settings load( const char *path, std::string *why )
	{
		std::string text;
		int err = 0;
		unreadable = readWholeFile( path, text, &err ) == FILE_UNREADABLE;
		if (unreadable)
		{
			*why = unreadableNote( path, err, "this match uses the default settings; no change in it is saved." );
			text.clear();
		}
		return parseSettings( text );
	}

	bool save( const char *path, const Settings &s, std::string *why )
	{
		if (unreadable)
		{
			*why = std::string( path ) + " could not be read when this match started, so this change is not saved over it.";
			return false;
		}
		std::string text;
		int err = 0;
		if (readWholeFile( path, text, &err ) == FILE_UNREADABLE)
		{
			*why = unreadableNote( path, err, "this change is not saved." );
			return false;
		}
		const std::string out = writeSettings( text, s );
		std::string tmp;
		FILE *f = gxCreateTempFor( path, tmp );	// a fresh exclusive temp: a link left at "GXHud.ini.tmp" is never followed
		if (f == nullptr)
		{
			*why = std::string( "cannot make a temp file beside " ) + path + " (" + strerror( errno ) + "): this change is not saved.";
			return false;
		}
		if (fwrite( out.data(), 1, out.size(), f ) != out.size())
		{
			fclose( f );
			remove( tmp.c_str() );
			*why = "cannot write " + tmp + ": this change is not saved.";
			return false;
		}
		// Flushed, then swapped in, on Windows too (MoveFileEx): the remove-then-rename this replaced left no GXHud.ini
		// at all if the second rename failed.
		if (!gxCommitAndReplace( f, tmp.c_str(), path ))
		{
			*why = "cannot replace " + std::string( path ) + ": this change is not saved.";
			return false;
		}
		return true;
	}
};

enum Preset { PRESET_FULL, PRESET_CASTER, PRESET_MINIMAL };

/// Presets change only what shows; opacity and hide-dead are the spectator's own and stay.
inline void applyPreset( Settings &s, Preset p )
{
	for (int i = 0; i < SHOW_COUNT; ++i)
	{
		if (p == PRESET_FULL)
			s.show[i] = true;
		else if (p == PRESET_CASTER)
			s.show[i] = showDefault( i );
		else
			s.show[i] = (i == SHOW_NAMES || i == SHOW_CASH || i == SHOW_CLOCK_TOTALS || i == SHOW_TAB_BOARD || i == SHOW_MINIMAP);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Keybinds: "MOD+KEY_x" or a bare "KEY_x"; names upper-cased for MetaEvent.h's KeyNames / ModifierNames tables.
// ---------------------------------------------------------------------------------------------------------------------
inline bool parseChord( const std::string &text, std::string &mod, std::string &key )
{
	std::string t = trim( text );
	for (size_t i = 0; i < t.size(); ++i)
		t[i] = (char)std::toupper( (unsigned char)t[i] );
	const size_t plus = t.find( '+' );
	mod = plus == std::string::npos ? std::string() : trim( t.substr( 0, plus ) );
	key = plus == std::string::npos ? t : trim( t.substr( plus + 1 ) );
	return key.size() > 4 && key.compare( 0, 4, "KEY_" ) == 0 && (plus == std::string::npos || !mod.empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------------------------------
struct Rect
{
	int x, y, w, h;
	bool contains( int px, int py ) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

/// The bar shows two team halves only when every player is on one of exactly two teams; anything else (no teams,
/// three teams, a teamless player) is laid out as a free-for-all.
inline bool isTwoTeamGame( const int *teams, int n )
{
	int a = -1, b = -1;
	for (int i = 0; i < n; ++i)
	{
		if (teams[i] < 0)
			return false;
		if (a < 0 || teams[i] == a)
			a = teams[i];
		else if (b < 0 || teams[i] == b)
			b = teams[i];
		else
			return false;
	}
	return a >= 0 && b >= 0;
}

/// The minimap panel takes the map's own shape, so the radar's
/// aspect-preserving fit (Radar::findDrawPositions) has nothing to letter-box. Height first, capped by the width.
inline Rect fitMinimap( int mapW, int mapH, int maxW, int maxH )
{
	Rect r = { 0, 0, maxW, maxH };
	if (mapW <= 0 || mapH <= 0 || maxW <= 0 || maxH <= 0)
		return r;
	r.w = (int)(((long long)maxH * mapW + mapH / 2) / mapH);
	if (r.w > maxW)
	{
		r.w = maxW;
		r.h = (int)(((long long)maxW * mapH + mapW / 2) / mapW);
	}
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------------------------------------------------
enum Status { STATUS_ALIVE, STATUS_DEAD, STATUS_LEFT };

/// hasNetwork: a live network game (a replay has none). connected: the peer is still in the match.
inline Status decideStatus( bool hasNetwork, bool connected, bool human, bool active )
{
	if (hasNetwork && human && !connected)
		return STATUS_LEFT;
	return active ? STATUS_ALIVE : STATUS_DEAD;
}

inline bool rowVisible( bool hiddenBySpectator, Status st, bool hideDead )
{
	return !hiddenBySpectator && !(hideDead && st != STATUS_ALIVE);
}

/// The spectator's local view: whose eyes he sees through (view; -1 = the whole map) and whose panel he follows (follow; -1 =
/// nobody, free camera). Client state only: the simulation never reads it.
struct ViewState { int view; int follow; };
inline bool operator==( const ViewState &a, const ViewState &b ) { return a.view == b.view && a.follow == b.follow; }
const ViewState VIEW_FREE = { -1, -1 };

/// A click on a player's card or board row views AS him (his fog of war and his audio, rts::changeObservedPlayer) and follows
/// him. The same click again - on whoever is viewed or followed - leaves both: whole map, nobody followed. A dead or left
/// player has no eyes, so he is only followed.
inline ViewState viewAfterClick( ViewState cur, int clickedSlot, Status clickedStatus )
{
	if (clickedSlot == cur.view || clickedSlot == cur.follow)
		return VIEW_FREE;
	ViewState n = { clickedStatus == STATUS_ALIVE ? clickedSlot : -1, clickedSlot };
	return n;
}
/// The VIEWING chip:
/// one click switches between the whole map and the FOLLOWED player's eyes (his fog of war, through
/// the retail toggle's own call). Nobody followed, or a followed player with no eyes left (dead, left): the click does nothing.
/// canViewAs: the engine can look through a player's eyes at all - rts::changeObservedPlayer refuses to begin unless
/// PlayerObserverEnabled is on and every player's ghosts are tracked (spectator-occupants r1); going back always works.
inline ViewState viewAfterChip( ViewState cur, Status followedStatus, bool canViewAs )
{
	if (cur.follow < 0)
		return cur;
	ViewState n = cur;
	if (cur.view == cur.follow)
		n.view = -1;
	else if (followedStatus == STATUS_ALIVE && canViewAs)
		n.view = cur.follow;
	return n;
}
/// The chip offers its click (hint, hover frame, hit) exactly when the click would change something.
inline bool chipToggles( ViewState cur, Status followedStatus, bool canViewAs )
{
	return !(viewAfterChip( cur, followedStatus, canViewAs ) == cur);
}
/// Esc with no menu open leaves both: whole map, nobody followed.
inline ViewState viewAfterEscape( ViewState ) { return VIEW_FREE; }
/// Esc is the HUD's only when it has something to leave and no menu wants it.
inline bool escapeLeavesView( ViewState cur, bool menuOpen ) { return !menuOpen && (cur.view >= 0 || cur.follow >= 0); }
/// The viewed player dies or leaves: the map goes whole, he stays followed.
inline ViewState viewAfterStatus( ViewState cur, Status viewedStatus )
{
	if (cur.view >= 0 && viewedStatus != STATUS_ALIVE)
		cur.view = -1;
	return cur;
}

/// A spectator's left click on a unit or building
/// follows its OWNER. ownerSlot -1 = nobody's row (neutral, civilian): view and follow stay. It never unfollows -
/// more of the followed player's units keep him - and the eyes move only when the spectator already looks through someone's
/// (as the follow keys do); a dead or left owner has none, so the map goes whole.
/// In a REPLAY the click shows the card and changes nothing else:
/// following a player there also plays back his recorded camera (retail "Use camera in
/// replays", GameLogic::onSetReplayCamera), so following the owner snapped the camera to wherever that player was looking.
inline ViewState viewAfterObjectClick( ViewState cur, int ownerSlot, Status ownerStatus, bool replay )
{
	if (ownerSlot < 0 || replay)
		return cur;
	ViewState n = cur;
	n.follow = ownerSlot;
	if (cur.view >= 0)
		n.view = ownerStatus == STATUS_ALIVE ? ownerSlot : -1;
	return n;
}

/// Whether the camera on screen is the followed player's own, played back from the replay (the chip then says THEIR CAMERA):
/// "Follow the player's camera" (retail UseCameraInReplays, default on) is on and GameLogic::onSetReplayCamera applied his
/// recorded camera in the last logic step (cameraApplied: it locks user control until the next frame, View::isUserControlLocked).
/// Read from that lock rather than predicted from the game mode and the seat, because a skirmish recorded with
/// SaveCameraInReplays = no, an AI and a multiplayer seat have no camera to play.
inline bool followsTheirCamera( bool replay, bool followCamera, bool following, bool cameraApplied )
{
	return replay && followCamera && following && cameraApplied;
}

/// A press and its release within `tol` pixels on both axes is a click; anything more is a drag, which a spectator has no
/// use for (a box select takes only units he controls: none).
inline bool isClickNotDrag( int x0, int y0, int x1, int y1, int tol )
{
	return std::abs( x1 - x0 ) <= tol && std::abs( y1 - y0 ) <= tol;
}

/// A special power's target as this client heard it where the simulation triggers the power (InGameUI::
/// gxNoteSpecialPowerTarget). caster = the object id of the building or unit that holds the power; science = the general's skill
/// the power needs, -1 for none (a superweapon, a unit ability); power = the power template's id.
struct PowerNote { unsigned caster; int player; int science; unsigned power; unsigned frame; bool live; };

/// The note a feed row's click jumps to;
/// -1 = none, and the row keeps its old jump.
///   * A superweapon row names its launcher and its power (rowCaster, rowPower): that launcher's newest order for that power, at
///     any age - the order comes first and a SCUD Storm held by an EMP may fire much later - up to `after` frames past the row.
///   * A general's-skill row names the player and the skill: his note for a power needing it, the nearest within `window` frames.
///   * Any other row (an upgrade names its building but no power) matches nothing (spectator-click r1-BC P2).
inline int pickPowerNote( const PowerNote *notes, int count, unsigned rowCaster, unsigned rowPower, int rowPlayer, int rowScience,
	unsigned rowFrame, unsigned window, unsigned after )
{
	const bool launch = rowCaster != 0 && rowPower != 0;
	if (!launch && rowScience < 0)
		return -1;
	int best = -1;
	long long bestKey = 0;
	for (int i = 0; i < count; ++i)
	{
		const PowerNote &n = notes[i];
		if (!n.live)
			continue;
		const long long d = (long long)n.frame - (long long)rowFrame;
		long long key;		// smaller is better
		if (launch)
		{
			if (n.caster != rowCaster || n.power != rowPower || d > (long long)after)
				continue;
			key = -(long long)n.frame;		// the newest
		}
		else
		{
			if (n.player != rowPlayer || n.science != rowScience || d < -(long long)window || d > (long long)window)
				continue;
			key = d < 0 ? -d : d;				// the nearest
		}
		if (best < 0 || key < bestKey)
		{
			best = i;
			bestKey = key;
		}
	}
	return best;
}

/// needed[i] = skill points needed for rank i+1. Returns the next rank's threshold, or -1 at the top rank.
inline int nextLevelXP( const int *needed, int levelCount, int rank )
{
	if (rank < 1)
		rank = 0;
	return rank < levelCount ? needed[rank] : -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Player mode: the GeneralsX UI for a player still playing.
//
// ---------------------------------------------------------------------------------------------------------------------
enum Mode { MODE_LEGACY, MODE_SPECTATOR, MODE_PLAYER };

/// generalsx: the one switch. viewerMaySeeFeed: replay, observer seat or a defeated player (gxViewerMaySeeFeed).
/// versusGame: skirmish, LAN or online - never the campaign, whose scripts hide and force parts of the bar themselves.
inline Mode decideMode( bool generalsx, bool viewerMaySeeFeed, bool haveLocalPlayer, bool localIsObserver, bool versusGame )
{
	if (!generalsx)
		return MODE_LEGACY;
	if (viewerMaySeeFeed)
		return MODE_SPECTATOR;
	return (haveLocalPlayer && !localIsObserver && versusGame) ? MODE_PLAYER : MODE_LEGACY;
}

/// FAIRNESS (owner, verbatim): "a player sees only what retail already lets a player know. No enemy cash, income, power,
/// unit counts, purchases or anything fog-of-war hides; allies only what retail shares. The spectator board's extra
/// columns exist for spectators only."
///
/// Everything private about a player's economy and army, as ONE block: the HUD's row derives from it, a row is cleared
/// with clearPrivate() before it is filled, and it is filled only when maySeePrivate() says so. A field added here is
/// therefore cleared by construction. Every field is an int (the two flags too), so the block has no padding and the
/// model test can compare it byte for byte with a cleared one.
struct PrivateStats
{
	int cash, cpm, prod, use, xp, xpNext, rank, kills, losses, units, bldgs;
	int grid, deficit;					///< flags: the player has a power grid / it is short
	PrivateStats() : cash( 0 ), cpm( 0 ), prod( 0 ), use( 0 ), xp( 0 ), xpNext( 0 ), rank( 0 ), kills( 0 ), losses( 0 ),
		units( 0 ), bldgs( 0 ), grid( 0 ), deficit( 0 ) {}
};

/// A spectator (gxViewerMaySeeFeed) sees everyone's numbers; a player still playing sees only his own.
inline bool maySeePrivate( bool viewerIsSpectator, bool ownRow ) { return viewerIsSpectator || ownRow; }

template <class R> inline void clearPrivate( R &row ) { static_cast<PrivateStats &>( row ) = PrivateStats(); }

/// Who another player IS (general, portrait, colour), as retail's Diplomacy screen tells a player: the GameSlot's
/// apparent template and colour - real for you and your allies, still "Random" for an enemy who picked Random.
inline bool apparentIdentity( bool viewerIsSpectator, bool ownRow ) { return !viewerIsSpectator && !ownRow; }

/// The player keys are caught in front of the meta map, so a bare key would steal a command-bar letter or a control
/// group. They need a modifier - except Tab, retail Diplomacy's own key, which the player board takes over (Diplomacy
/// moves to Shift+Tab, as it does for spectators). The engine also skips a chord the active CommandMap already binds.
/// "NONE+KEY_R" is a bare R (ModifierNames maps NONE to 0), so it counts as no modifier.
inline bool playerChordAllowed( const std::string &mod, const std::string &key )
{
	return !(mod.empty() || equalsNoCase( mod, "NONE" )) || key == "KEY_TAB";
}

/// The player Tab board's blocks: one per team, my own team first, then the others in the order they first appear;
/// players with no team (-1) share one last block, so a free-for-all is one block. `out` holds up to n + 1 entries.
/// Owner, 05/10/2026: "how would a 2v2v2v2v2v2 look like then?" - six blocks of two, not one list.
inline int teamBlocks( const int *teams, int n, int ownTeam, int *out )
{
	int count = 0;
	const auto add = [&]( int t ) {
		for (int i = 0; i < count; ++i)
			if (out[i] == t)
				return;
		out[count++] = t;
	};
	if (ownTeam >= 0)
		add( ownTeam );
	for (int i = 0; i < n; ++i)
		if (teams[i] >= 0)
			add( teams[i] );
	for (int i = 0; i < n; ++i)
		if (teams[i] < 0)
		{
			add( -1 );
			break;
		}
	return count;
}

/// The retail radar window in the map's own shape on its home rect's bottom-left corner (the GeneralsX panels, where the
/// bar's corner is). The radar draws, clips its view box and maps clicks through that window's rect. Where the GeneralsX UI
/// keeps the retail box instead, W3DRadar::draw fills the margins of a map that does not fill it with the map's own
/// out-of-bounds terrain, dimmed; the legacy look keeps retail's black margins (21/10).
inline Rect fitRadar( const Rect &home, int mapW, int mapH )
{
	if (mapW <= 0 || mapH <= 0)
		return home;
	Rect r = fitMinimap( mapW, mapH, home.w, home.h );
	r.x = home.x;
	r.y = home.y + home.h - r.h;
	return r;
}

/// The retail bar under the GeneralsX UI (F11, a campaign;
/// - asked of the legacy look, which since 21/10 is retail's own radar again: "These shouldn't
/// change if the UI is legacy"): the radar window in the map's own shape, in the bottom-left corner of its home rect, so
/// Radar::findDrawPositions has no bars to leave. A map whose shape already fills the box (within 2 px - the radar draws it
/// inset by 1) keeps the home rect untouched, so a square map stays exactly as retail draws it.
inline Rect fitRadarCentred( const Rect &home, int mapW, int mapH )
{
	if (mapW <= 0 || mapH <= 0)
		return home;
	// W3DLeftHUDDraw draws the radar one pixel inside the window on every side: fit the INNER box, add the inset back
	// (fitting the outer one left 1-2 px margins on 2:1 and 4:1 maps).
	Rect r = fitMinimap( mapW, mapH, home.w - 2, home.h - 2 );
	r.w += 2;
	r.h += 2;
	if (r.w >= home.w - 2 && r.h >= home.h - 2)
		return home;
	// Tucked into the box's bottom-left corner, where retail's radar sits:
	// the freed strip goes to the right (tall map) or the top (wide map).
	r.x = home.x;
	r.y = home.y + home.h - r.h;
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Docking and dragging
// ---------------------------------------------------------------------------------------------------------------------
/// The bottom bar's heights (1080p px) for what is on it: the followed player's panel and the selected object's card used to
/// stack at 160 + 112 even when the panel only said "no player focused". Nothing followed = no panel at all; both = the player
/// strip shrinks to one row of key stats under the card.
struct FocusBarLayout { int panelH; int bandH; bool compact; };
inline FocusBarLayout focusBarLayout( bool hasFocus, bool hasObject, int fullH = 160, int bandH = 112, int compactH = 56 )
{
	FocusBarLayout l = { 0, hasObject ? bandH : 0, false };
	if (hasFocus)
	{
		l.compact = hasObject;
		l.panelH = hasObject ? compactH : fullH;
	}
	return l;
}

/// Top edge of the shown bar: it sits on the bottom edge of the FULL panel's home, so the dock offset is always measured against one
/// reference rect and a collapsed or compact bar can never clamp itself into a permanent offset.
inline int focusBarTop( int fullHomeTop, int fullH, int panelH ) { return fullHomeTop + fullH - panelH; }

/// Drawn top of the full panel: the replay strip's clearance lifts what is DRAWN, never the saved offset. placed() clamps (and writes
/// back) the offset against the strip-free home only, so a frame where the strip is not up - the first frame after overlays come back
/// (F11), the quit menu - can no longer shrink the offset for good.
inline int focusDrawnTop( int placedTop, int clearance ) { return placedTop - clearance < 0 ? 0 : placedTop - clearance; }

/// Px of the offset the top clamp of focusDrawnTop hides (the bar is drawn at 0 while its placed top is below the clearance).
/// A drag starts from what is drawn: Dock::press takes this, so the first px of a downward drag move the bar.
inline int focusHiddenTop( int placedTop, int clearance ) { return placedTop < clearance ? clearance - placedTop : 0; }

/// The FOCUS tab's bottom line (the tab, tabW x tabH at tabX, is drawn tabRise px above it). It never leaves the top edge, and a tab
/// over the replay strip, which takes input first, goes just above the strip - or just below it when the strip is docked at the top
/// edge and there is no room above (the scb-r7 top clamp put it back under such a strip). The top clamp comes
/// BEFORE the overlap test: a full panel at the top edge was tested off screen and then clamped under the strip.
inline int focusTabY( int tabY, int tabX, int tabW, int tabH, const Rect &strip, int tabRise, int gap )
{
	if (tabY < tabRise)
		tabY = tabRise;
	const int ty = tabY - tabRise;
	if (tabX < strip.x + strip.w && tabX + tabW > strip.x && ty < strip.y + strip.h && ty + tabH > strip.y)
	{
		tabY = strip.y - gap;
		if (tabY < tabRise)
			tabY = strip.y + strip.h + gap + tabRise;
	}
	return tabY;
}

enum PanelId { PANEL_PLAYERS, PANEL_MAP, PANEL_FOCUS, PANEL_BOARD, PANEL_DISPLAY, PANEL_COUNT };

struct Dock
{
	enum { DRAG_THRESHOLD = 4 };
	enum Release { RELEASE_NONE, RELEASE_TAB_CLICK, RELEASE_BODY_CLICK, RELEASE_DRAG };
	struct Panel { bool open; int dx, dy; };		///< dx/dy: offset from the panel's home position

	Panel panel[PANEL_COUNT];
	int pressed;					///< panel under the press, or -1
	bool pressedTab;
	bool drag;
	int downX, downY, startDx, startDy;

	Dock() { reset(); }

	void reset()
	{
		for (int i = 0; i < PANEL_COUNT; ++i)
		{
			panel[i].open = false;
			panel[i].dx = panel[i].dy = 0;
		}
		pressed = -1;
		pressedTab = drag = false;
		downX = downY = startDx = startDy = 0;
	}

	void redockAll() { reset(); }
	/// The release never came (a retail window ate it): drop the press where it is - no click, no toggle.
	void cancel() { pressed = -1; drag = false; }
	bool dragging() const { return pressed >= 0 && drag; }

	/// hiddenDy: px of the offset the panel's drawn rect does not show (focusHiddenTop); a DRAG starts from what is drawn, a plain
	/// click leaves the offset alone.
	void press( int id, bool onTab, int x, int y, int hiddenDy = 0 )
	{
		if (id < 0 || id >= PANEL_COUNT)
			return;
		pressed = id;
		pressedTab = onTab;
		drag = false;
		downX = x;
		downY = y;
		startDx = panel[id].dx;
		startDy = panel[id].dy + hiddenDy;
	}

	void move( int x, int y )
	{
		if (pressed < 0)
			return;
		if (!drag && std::abs( x - downX ) <= DRAG_THRESHOLD && std::abs( y - downY ) <= DRAG_THRESHOLD)
			return;
		drag = true;
		panel[pressed].dx = startDx + (x - downX);
		panel[pressed].dy = startDy + (y - downY);
	}

	Release release( int x, int y )
	{
		if (pressed < 0)
			return RELEASE_NONE;
		move( x, y );
		Panel &p = panel[pressed];
		Release r = RELEASE_DRAG;
		if (!drag)
		{
			if (pressedTab)
			{
				if (p.dx != 0 || p.dy != 0)
				{
					p.dx = p.dy = 0;			// a moved panel's tab snaps it home and leaves it open
					p.open = true;
				}
				else
					p.open = !p.open;
				r = RELEASE_TAB_CLICK;
			}
			else
				r = RELEASE_BODY_CLICK;
		}
		pressed = -1;
		drag = false;
		return r;
	}
};

/// The PING cell of a board row, in ms, or -1 for "--". `pingMs` is NetworkInterface::getPeerPingMs(slot): the router's
/// round trip to that peer, -1 when unknown. The router has no round trip to itself, so its own row borrows the viewer's
/// leg to it - 0 when the viewer IS the router (the same rule retail Diplomacy uses, Diplomacy.cpp ~:703).
inline int pingShown( int pingMs, bool isRouterRow, bool localIsRouter, int localPingMs )
{
	if (pingMs >= 0)
		return pingMs;
	if (!isRouterRow)
		return -1;
	return localIsRouter ? 0 : (localPingMs >= 0 ? localPingMs : -1);
}

/// Board colour class of a figure: 0 = no figure (the neutral colour), 1 = good, 2 = warning, 3 = bad (owner
/// 06/10: "show ping in colors ... and same for fps"). Ping: < 100 ms good, < 200 warning, else bad. FPS: >= 50 good,
/// >= 30 warning, else bad.
inline int pingClass( int ms ) { return ms < 0 ? 0 : ms < 100 ? 1 : ms < 200 ? 2 : 3; }
inline int fpsClass( int fps ) { return fps < 0 ? 0 : fps >= 50 ? 1 : fps >= 30 ? 2 : 3; }

/// The FPS cell, or -1 for "--": your own render rate, and anyone else's average whenever this machine has it -
/// NetworkInterface::getSlotAverageFPS answers -1 for a figure it does not hold (today every peer but the router's own;
/// makes every peer hold every player's), so no extra rule is needed here.
inline int fpsShown( bool ownRow, int ownFps, int slotFps )
{
	if (ownRow)
		return ownFps >= 0 ? ownFps : -1;
	return slotFps >= 0 ? slotFps : -1;
}

/// What a mouse message is to the board gate: a button press, a press's drag or release (or click), a wheel turn, a move.
enum GateMsg { GATE_PRESS, GATE_FOLLOW, GATE_WHEEL, GATE_POSITION };

/// The open Tab board owns a press or a wheel turn over its rectangle, and the drag and release of a press that began on it:
/// nothing reaches the windows, the selection or the camera under it.
/// A press that began on the world finishes on the world wherever it is released - the camera's rotate and the
/// selection box clean up on their own release - and moves always pass, so edge scrolling and
/// every hover keep their bookkeeping.
inline bool boardGateTakes( bool boardOpen, const Rect &board, int x, int y, int kind, bool pressOwnedByBoard )
{
	switch (kind)
	{
		case GATE_PRESS:
		case GATE_WHEEL:	return boardOpen && board.w > 0 && board.contains( x, y );
		case GATE_FOLLOW:	return pressOwnedByBoard;
		default:			return false;
	}
}

/// Retail Diplomacy's rule for its mute buttons: not yourself, not an AI, not a player who is no longer in the game.
inline bool mayMute( bool human, bool own, bool inGame ) { return human && !own && inGame; }

/// GeneralsX: No GX overlay (spectator HUD, replay strip, vote banner, net/pacing readouts, match
/// timer) draws while the load screen is up. The game's own signals, not a timer: GameLogic::startNewGame raises
/// m_loadScreenRender with the load screen and lowers it at its very end, m_loadingMap / m_loadingSave bracket the load.
inline bool mayDrawOverlays( bool loadScreenRender, bool loadingMap, bool loadingSave )
{
	return !loadScreenRender && !loadingMap && !loadingSave;
}

/// Rising edge of "the load screen is up": the one moment the per-match HUD state is dropped, so the first frame of the
/// next match starts clean even when the previous match never reached InGameUI::reset (rematch, refused start).
struct LoadEdge
{
	bool wasUp = false;
	bool rose( bool up ) { const bool r = up && !wasUp; wasUp = up; return r; }
};

/// The event toast's rule (was inline in drawToast): shown while young, and only if announced after this match's HUD
/// state was loaded - the banner ring is wall-clock and outlives a match. matchStartMs 0 = no match loaded yet = never.
inline bool toastShows( unsigned bornMs, unsigned nowMs, unsigned ttlMs, unsigned matchStartMs, bool matchLoaded )
{
	return matchLoaded && (nowMs - bornMs) < ttlMs && (int)(bornMs - matchStartMs) >= 0;
}

/// GeneralsX: The geometry the event toasts are laid out against, out of GXSpectatorHud.inl so the
/// model test builds the very rects the HUD draws. Every length is a 1080p design pixel scaled by `scale` (= screen height / 1080),
/// rounded as the HUD's own S() rounds.
inline int hudS( float v, float scale ) { return (int)(v * scale + 0.5f); }
inline bool rectsMeet( const Rect &a, const Rect &b )
{
	return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/// The player bar's home rect (before any drag), its card width and its centre block's width.
/// GeneralsX: More players than the band has room for at its normal card size (32-player maps,
/// 31 cards in one row, every name cut to "Hard ..."): the cards keep that size, each half shows what fits and pages through the
/// rest with two buttons at its outer end. 14 players or fewer never page: their band is what it always was.
enum { BAR_UNPAGED_MAX = 14 };
struct BarHome { Rect r; int cw; int cenW; int fit; int pageW; };	///< fit: card places per side; pageW: each end's page buttons, 0 = no paging
inline BarHome barHomeRect( int leftCount, int rightCount, bool clockTotals, int screenW, float scale )
{
	BarHome b;
	int perSide = leftCount > rightCount ? (leftCount > 1 ? leftCount : 1) : (rightCount > 1 ? rightCount : 1);
	const bool many = (leftCount + rightCount) >= 13;
	b.cw = hudS( many ? 114.0f : 130.0f, scale );
	b.cenW = clockTotals ? hudS( many ? 240.0f : 300.0f, scale ) : hudS( 16.0f, scale );
	b.fit = perSide;
	b.pageW = 0;
	if (leftCount + rightCount > BAR_UNPAGED_MAX)
	{
		const int pageW = hudS( 26.0f, scale );
		const int room = (screenW - b.cenW - hudS( 24.0f, scale ) - pageW * 2) / (b.cw * 2);
		if (room < perSide)
		{
			b.fit = perSide = room > 1 ? room : 1;
			b.pageW = pageW;
		}
	}
	const int maxCw = (screenW - b.cenW - hudS( 24.0f, scale )) / (perSide * 2);
	if (b.cw > maxCw)
		b.cw = maxCw > hudS( 60.0f, scale ) ? maxCw : hudS( 60.0f, scale );
	const int side = perSide * b.cw + hudS( 8.0f, scale ) + b.pageW;
	b.r.w = side * 2 + b.cenW;
	b.r.h = hudS( 84.0f, scale );
	b.r.x = (screenW - b.r.w) / 2;
	b.r.y = hudS( 34.0f, scale );
	return b;
}

/// A pinned card takes a place, so it needs a second place for the pager to show anything: with room for one card nothing is pinned.
inline bool barPins( bool pinLast, int fit ) { return pinLast && fit >= 2; }
/// The furthest a paged half can start. `pinLast`: its last card (a player's own, beside the clock) stays put and takes one place.
inline int barMaxFirst( int count, int fit, bool pinLast )
{
	pinLast = barPins( pinLast, fit );
	const int scroll = pinLast ? count - 1 : count, places = pinLast ? fit - 1 : fit;
	return scroll > places ? scroll - places : 0;
}
/// Which of a paged half's cards it shows: clamps `first` in place, fills `idx` in draw order and returns how many.
inline int barWindow( int count, int fit, bool pinLast, int &first, int *idx )
{
	const int maxFirst = barMaxFirst( count, fit, pinLast );
	first = first < 0 ? 0 : (first > maxFirst ? maxFirst : first);
	pinLast = barPins( pinLast, fit );
	const int scroll = pinLast ? count - 1 : count, places = pinLast ? fit - 1 : fit;
	int n = 0;
	for (int i = first; i < scroll && i < first + places; ++i)
		idx[n++] = i;
	if (pinLast && count > 0)
		idx[n++] = count - 1;
	return n;
}

/// GeneralsX: The player lists (Tab board, spectator board) show at most maxRows rows and never more than
/// roomPx holds, and scroll through the rest. At least one row is always shown.
inline int listRowsShown( int rows, int maxRows, int roomPx, int rowH )
{
	int n = rows < maxRows ? rows : maxRows;
	if (rowH > 0 && roomPx / rowH < n)
		n = roomPx / rowH;
	return n < 1 ? (rows > 0 ? 1 : 0) : n;
}
inline int listClampFirst( int first, int rows, int shown )
{
	const int maxFirst = rows > shown ? rows - shown : 0;
	return first < 0 ? 0 : (first > maxFirst ? maxFirst : first);
}
struct ListThumb { int y; int h; };	///< relative to the top of the track
inline ListThumb listThumb( int trackH, int rows, int shown, int first, int minH )
{
	ListThumb t = { 0, trackH };
	if (rows <= shown || rows <= 0)
		return t;
	t.h = trackH * shown / rows;
	if (t.h < minH)
		t.h = minH;
	if (t.h > trackH)
		t.h = trackH;
	t.y = (trackH - t.h) * listClampFirst( first, rows, shown ) / (rows - shown);
	return t;
}
/// The first row that puts the thumb's centre on `y` (relative to the track), for a press or drag on the scroll bar.
inline int listFirstAtY( int y, int trackH, int rows, int shown, int minH )
{
	const int range = trackH - listThumb( trackH, rows, shown, 0, minH ).h;
	if (range <= 0 || rows <= shown)
		return 0;
	const int pos = y - listThumb( trackH, rows, shown, 0, minH ).h / 2;
	return listClampFirst( (pos * (rows - shown) + range / 2) / range, rows, shown );
}

/// A dock tab hung by its centre (the PLAYERS tab): its text plus 44 px of arrows and padding, 20 px tall.
inline Rect dockTabRectCentred( int cx, int y, int textW, float scale )
{
	const int w = textW + hudS( 44.0f, scale );
	return Rect{ cx - w / 2, y, w, hudS( 20.0f, scale ) };
}
/// The PLAYERS tab under a bar rect (home or dragged): centred, 1 px below it, folded or open (spectator).
inline Rect playersTabRect( const Rect &bar, int textW, float scale )
{
	return dockTabRectCentred( bar.x + bar.w / 2, bar.y + bar.h + hudS( 1.0f, scale ), textW, scale );
}
/// The gear (the customise panel's tab) at the bar home's right end.
inline Rect gearTabRect( const Rect &barHome, float scale )
{
	return Rect{ barHome.x + barHome.w - hudS( 26.0f, scale ), barHome.y + barHome.h + hudS( 1.0f, scale ), hudS( 26.0f, scale ), hudS( 20.0f, scale ) };
}
/// Where a cut card name's tip can be drawn under the bar (any card, so the whole screen width): 4 px under the bar, text + 12 tall.
inline Rect nameTipBand( const Rect &bar, int tipTextH, int screenW, float scale )
{
	return Rect{ 0, bar.y + bar.h + hudS( 4.0f, scale ), screenW, tipTextH + hudS( 12.0f, scale ) };
}
/// The customise ("Spectator display") panel's home rect.
inline Rect displayPanelHome( int screenW, int rowCount, float scale )
{
	const int w = hudS( 640.0f, scale );
	const int h = hudS( 360.0f, scale ) + ((rowCount + 1) / 2) * hudS( 22.0f, scale );
	return Rect{ screenW - w - hudS( 100.0f, scale ), hudS( 160.0f, scale ), w, h };
}

/// The event toasts.
/// A stack hung under the PLAYERS tab, newest
/// on top: the newest is always where the eye already looks, an arrival moves the stack (one motion, at the moment something
/// happened) and a toast always leaves from the bottom (the oldest dies first), so nothing reflows when one simply expires.
/// Lengths in 1080p design pixels, times in milliseconds.
enum
{
	TOAST_MAX = 3,				///< shown at once; a 4th pushes the oldest out
	TOAST_LIFE_MS = 3000,		///< each one's whole life, both fades included
	TOAST_IN_MS = 200,			///< fade in and drop into place, ease-out
	TOAST_OUT_MS = 250,			///< fade out at the end of its life
	TOAST_PUSH_MS = 160,		///< fade out when a newer one pushes it out of the stack: faster than its own end
	TOAST_SLIDE_MS = 220,		///< an older toast moving down one place to make room, ease-out
	TOAST_STAGGER_MS = 40,		///< a burst (first drawn in the same frame) fades in one after another, top down
	TOAST_W = 460, TOAST_H = 36, TOAST_GAP = 6,
	TOAST_DROP = 6,				///< how far a toast drops into its place
	TOAST_CLEAR = 12			///< the stack's distance from what it hangs under (more than TOAST_DROP: the drop never reaches it)
};

inline float toastClamp01( float t ) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
inline float toastEaseOutCubic( float t ) { const float u = 1.0f - toastClamp01( t ); return 1.0f - u * u * u; }
inline float toastEaseOutQuint( float t ) { const float u = 1.0f - toastClamp01( t ); return 1.0f - u * u * u * u * u; }

/// Where the stack goes: x, the top of its first place, the toast's size and the distance between places. `clear` is FALSE when no
/// place was free: the toasts are then not drawn at all (never a known collision; they come back once a place frees up).
struct ToastLayout { int x, top, w, h, step; bool clear; };

/// The rect every place of the stack can reach from (x, top): TOAST_DROP above the first, down to the leaving fourth.
inline Rect toastZone( int x, int top, int w, int h, int step, int drop ) { return Rect{ x, top - drop, w, TOAST_MAX * step + h + drop }; }

/// Whether the stack at (x, top) fits: its whole zone on screen and 1 px off every rect's outer line (toastLayout's own test, also how
/// the HUD re-checks a layout decided earlier in the frame against what was drawn since).
inline bool toastFits( int x, int top, int w, int h, int step, int drop, const Rect *avoid, int nAvoid, int screenW, int screenH )
{
	const Rect z = toastZone( x, top, w, h, step, drop );
	if (z.x < 0 || z.y < 0 || z.x + z.w > screenW || z.y + z.h > screenH)
		return false;
	const Rect zo = { z.x - 1, z.y - 1, z.w + 2, z.h + 2 };		// the toasts' outer line
	for (int i = 0; i < nAvoid; ++i)
		if (rectsMeet( zo, Rect{ avoid[i].x - 1, avoid[i].y - 1, avoid[i].w + 2, avoid[i].h + 2 } ))
			return false;
	return true;
}

/// `above`: where the stack hangs from - the bar's HOME rect, its PLAYERS tab, the gear, the name-tip band. Home is TOAST_CLEAR under
/// the lowest of them that shares its columns, folded or open, any player count. `avoid`: everything ACTUALLY drawn this frame that
/// the stack must never meet - the bar, its tab and its name tips where they are drawn (dragged or not), the gear, the VIEWING chip,
/// the vote-kick ballot, the customise panel, the minimap and the followed-player panel with their tabs. Home wins when free (the
/// stack never moves just because something moved elsewhere); otherwise the nearest free place beside, below or above one of them.
/// Every candidate keeps the whole zone on screen and 1 px off each rect's outer line.
inline ToastLayout toastLayout( const Rect *above, int nAbove, const Rect *avoid, int nAvoid, int screenW, int screenH, float scale )
{
	ToastLayout L;
	L.w = hudS( (float)TOAST_W, scale );
	L.h = hudS( (float)TOAST_H, scale );
	L.step = L.h + hudS( (float)TOAST_GAP, scale );
	const int homeX = screenW / 2 - L.w / 2;
	int lowest = 0;
	for (int i = 0; i < nAbove; ++i)
		if (above[i].w > 0 && above[i].h > 0 && above[i].x < homeX + L.w && homeX < above[i].x + above[i].w && above[i].y + above[i].h > lowest)
			lowest = above[i].y + above[i].h;
	const int clearPx = hudS( (float)TOAST_CLEAR, scale );
	const int homeTop = lowest + clearPx;
	const int drop = hudS( (float)TOAST_DROP, scale );
	const int gap = hudS( (float)TOAST_GAP, scale );
	const int zoneH = TOAST_MAX * L.step + L.h + drop;
	struct Cand { int x, top; };
	enum { MAX_AVOID = 32 };
	Cand cands[1 + 4 * MAX_AVOID];
	int nc = 0;
	cands[nc++] = Cand{ homeX, homeTop };
	for (int i = 0; i < nAvoid && i < MAX_AVOID; ++i)
	{
		const Rect &a = avoid[i];
		if (a.w <= 0 || a.h <= 0)
			continue;
		cands[nc++] = Cand{ a.x - 1 - gap - 1 - L.w, homeTop };			// left of it (its outer line, a gap, the toast's own line)
		cands[nc++] = Cand{ a.x + a.w + 1 + gap + 1, homeTop };			// right of it
		cands[nc++] = Cand{ homeX, a.y + a.h + 1 + clearPx };				// under it, as home hangs under the tab
		cands[nc++] = Cand{ homeX, a.y - 1 - clearPx - (zoneH - drop) };	// over it
	}
	int best = -1;
	long bestD = 0;
	for (int c = 0; c < nc; ++c)
	{
		if (!toastFits( cands[c].x, cands[c].top, L.w, L.h, L.step, drop, avoid, nAvoid, screenW, screenH ))
			continue;
		const long d = (long)(cands[c].x > homeX ? cands[c].x - homeX : homeX - cands[c].x) + (cands[c].top > homeTop ? cands[c].top - homeTop : homeTop - cands[c].top);
		if (best < 0 || d < bestD)
		{
			best = c;
			bestD = d;
		}
	}
	L.clear = best >= 0;
	L.x = best >= 0 ? cands[best].x : homeX;
	L.top = best >= 0 ? cands[best].top : homeTop;
	return L;
}

/// A rect grown to cover another (an empty one covers nothing).
inline Rect rectUnion( const Rect &a, const Rect &b )
{
	if (a.w <= 0 || a.h <= 0) return b;
	if (b.w <= 0 || b.h <= 0) return a;
	const int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
	const int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
	return Rect{ x0, y0, x1 - x0, y1 - y0 };
}

/// One toast this frame: drawn or not, its opacity and its top.
struct ToastSlot { bool shown; float alpha; int y; };

/// The stagger of toast k (0 = newest): TOAST_STAGGER_MS for every newer toast first drawn in the same frame as it.
inline unsigned toastDelay( const unsigned *seen, int k )
{
	unsigned c = 0;
	for (int j = 0; j < k; ++j)
		if (seen[j] == seen[k])
			++c;
	return c * (unsigned)TOAST_STAGGER_MS;
}

/// The stack this frame. born[0..n) newest first (the banner ring's order) - its life is counted from its birth; seen[] = when the
/// HUD first had it to draw (the motion is counted from there, and toasts first drawn in the same frame are one burst: laid out
/// directly in their places and faded in top down, TOAST_STAGGER_MS apart). Toast k sits k places down once the arrivals after it
/// have made room: each arrival adds one place, eased over TOAST_SLIDE_MS from when it appeared, so a second arrival mid-slide just
/// adds its own step (interruptible, never a jump). The 4th from the top fades out over TOAST_PUSH_MS from the arrival that pushed
/// it (never drawn at all when that arrival came in the same frame); a 5th is gone. Returns how many are drawn.
inline int toastStack( const unsigned *born, const unsigned *seen, int n, unsigned nowMs, unsigned matchStartMs, bool matchLoaded,
	int top, int step, int drop, ToastSlot *out )
{
	int drawn = 0;
	for (int k = 0; k < n; ++k)
	{
		out[k].shown = false;
		out[k].alpha = 0.0f;
		out[k].y = top;
		if (k > TOAST_MAX || !toastShows( born[k], nowMs, TOAST_LIFE_MS, matchStartMs, matchLoaded ))
			continue;
		const unsigned start = seen[k] + toastDelay( seen, k );
		if ((int)(nowMs - start) < 0)
			continue;		// still waiting for its turn in a burst
		float push = 1.0f;
		if (k == TOAST_MAX)
		{
			const unsigned pushed = seen[0];
			if (pushed == seen[k] || nowMs - pushed >= (unsigned)TOAST_PUSH_MS)
				continue;
			push = 1.0f - toastEaseOutCubic( (float)(nowMs - pushed) / (float)TOAST_PUSH_MS );
		}
		const float in = (float)(nowMs - start) / (float)TOAST_IN_MS;
		const unsigned left = TOAST_LIFE_MS - (nowMs - born[k]);
		const float fadeOut = left >= (unsigned)TOAST_OUT_MS ? 1.0f : 1.0f - toastEaseOutCubic( 1.0f - (float)left / (float)TOAST_OUT_MS );
		const float alpha = toastEaseOutCubic( in ) * fadeOut * push;
		float places = 0.0f;
		for (int j = 0; j < k; ++j)
		{
			if (seen[j] == seen[k])
				places += 1.0f;
			else
			{
				const unsigned sj = seen[j] + toastDelay( seen, j );
				places += (int)(nowMs - sj) <= 0 ? 0.0f : toastEaseOutQuint( (float)(nowMs - sj) / (float)TOAST_SLIDE_MS );
			}
		}
		out[k].y = top + (int)(places * step + 0.5f) - (int)(drop * (1.0f - toastEaseOutQuint( in )) + 0.5f);
		out[k].alpha = alpha;
		out[k].shown = alpha > 0.0f;
		drawn += out[k].shown ? 1 : 0;
	}
	return drawn;
}

/// A toast's click identity: its birth and ring slot, so a press and a release match the same line even when another toast
/// about the same player sits next to it, or a new one moved the stack in between.
inline int toastHitId( unsigned bornMs, int ringSlot ) { return (int)((bornMs << 2) | (unsigned)(ringSlot & 3)); }

/// The money prefix before a grouped figure: "+$" / "-$" for an income, "$" for an amount
/// else none.
inline const wchar_t *cashPrefix( int v, bool plus, bool dollar )
{
	return plus ? (v < 0 ? L"-$" : L"+$") : (dollar ? L"$" : L"");
}

/// The toast's "<player> launched <weapon>": one watcher per superweapon building, fed whether the launcher is IN USE
/// (Particle Cannon: its update module is active; Nuclear Missile / SCUD Storm: the silo doors are in their post-firing
/// waiting-to-close / closing states) from the client's own timer walk (InGameUI::postDraw), so the simulation is never touched.
/// A launch is the idle -> in-use edge. This is not the ready-frame jump: a Saboteur resets a ready weapon's timer without
/// firing, and a ready-state edge needs the READY frame to have been rendered, which fast-forward skips.
/// In use lasts seconds, so a skipped frame still sees it; the first sample is only a baseline, so a seek or a join in the
/// middle of a launch announces nothing.
struct SuperweaponWatch
{
	bool seen = false;
	bool wasInUse = false;
	bool fired( bool inUse )
	{
		const bool launched = seen && inUse && !wasInUse;
		seen = true;
		wasInUse = inUse;
		return launched;
	}
};

/// SCUD Storm has no silo doors (GLAScudStorm fires through OCLSpecialPower's Attack nugget), but that nugget orders the BUILDING to attack
/// with its own PRIMARY weapon, so the building's last-shot frame moves when, and only when, a salvo is fired: a Saboteur's timer reset moves
/// no shot, and a skipped render or a replay seek cannot hide it (the last-shot frame persists).
/// A shot starts a NEW salvo (= a launch) when the previous shot this watcher knew of is more than `newSalvoGap` older (or there was none);
/// the caller passes 90% of the power's reload time: one salvo cannot be followed by another before the power has recharged, while the shots of
/// one salvo are seconds apart, even across an EMP or a Microwave Tank subdue that parks the building (the shots resume afterwards). The first sample is only a baseline, and it counts as the previous shot, so joining or becoming a
/// spectator in the middle of a salvo announces nothing. The shot must also be recent (10 s), so a seek over an old salvo stays quiet.
struct ScudLaunchWatch
{
	bool seen = false;
	unsigned lastShot = 0;
	bool step( unsigned shotFrame, unsigned frame, unsigned fps, unsigned newSalvoGap )
	{
		const unsigned previous = lastShot;
		const bool moved = seen && shotFrame > previous;
		lastShot = shotFrame;		// also on a seek back: start over from what the building says now
		seen = true;
		if (!moved || frame < shotFrame || frame - shotFrame > fps * 10)
			return false;
		return previous == 0 || shotFrame - previous > newSalvoGap;
	}
};

/// The General's button's "a new promotion point is waiting" state: retail's own m_genStarFlash, read through the mirror
/// ControlBar.cpp keeps (gxGenStarFlash, written at every assignment, in the tick's own event order - nothing is sampled
/// or re-derived here). Fed once per update, right after ControlBar::update (where retail's getStarImage has just run).
/// `since` is the pulse's phase: when the flag was last seen to rise.
struct GeneralPoint
{
	bool flash = false;
	unsigned since = 0;
	void reset() { flash = false; since = 0; }
	void update( bool retailFlag, unsigned nowMs )
	{
		if (retailFlag && !flash)
			since = nowMs;
		flash = retailFlag;
	}
};

// ---------------------------------------------------------------------------------------------------------------------
// What is inside.
// The spectator object card lists a container's occupants the way retail's exit buttons do for its owner:
// one cameo per occupant, then one empty box per free slot. Retail gives an OBSERVER nothing (ControlBar::update returns
// before any context UI for the observer bar) and a PLAYER only his own containers and neutral garrisons
// (ControlBar::evaluateContextUI: "Can't peek inside enemy/allied containers period!"); neither changes here.
// ---------------------------------------------------------------------------------------------------------------------
enum OccupantKind { OCC_NONE, OCC_GARRISON, OCC_TUNNEL, OCC_RIDER, OCC_PASSENGERS };

/// What the card knows about the clicked object, read from its contain module's own getters.
struct ContainFacts
{
	bool hasContain = false;
	bool displayedOnControlBar = false;	///< ContainModuleInterface::isDisplayedOnControlBar: retail's own "my contents are listed"
	bool garrisonable = false, tunnel = false, riderChange = false;
	bool cardShowsPortrait = true;		///< retail hides the portrait AND the cameos together (an empty civilian car)
	bool disguisedAsOther = false;		///< the card shows a disguise's template: listing the real object's contents would betray it
	bool underConstruction = false;
};

/// Which containers list their contents is retail's call, not ours: isDisplayedOnControlBar is TRUE for transports (Humvee,
/// Chinook, Troop Crawler, Battle Bus, Helix, the Internet Center), garrisons and bunkers, tunnels and the Combat Cycle's rider,
/// and for an Overlord only while its Battle Bunker takes the passengers; FALSE for a hospital, a cave, a parachute.
inline OccupantKind occupantKind( const ContainFacts &f )
{
	if (!f.hasContain || !f.displayedOnControlBar || !f.cardShowsPortrait || f.disguisedAsOther || f.underConstruction)
		return OCC_NONE;
	if (f.tunnel)
		return OCC_TUNNEL;
	if (f.riderChange)
		return OCC_RIDER;
	return f.garrisonable ? OCC_GARRISON : OCC_PASSENGERS;
}

/// FAIRNESS: contents are a spectator's view only (replay, observer seat, defeated player - the same viewers who see every
/// player's cash). A player still playing keeps exactly retail's view: his own containers on his control bar, nobody else's.
inline bool mayShowOccupants( Mode mode ) { return mode == MODE_SPECTATOR; }

struct SlotCount
{
	int used, total, free;
	bool known;		///< the container states a capacity (getContainMax() > 0)
};

/// used = occupants + the extra slots multi-slot riders take (the pips' rule, ContainModuleInterface::getContainerPipsToShow);
/// free = the empty exit buttons retail's inventory row leaves (ControlBar::doTransportInventoryUI: max - extra slots - occupants).
inline SlotCount slotCount( int count, int extraSlots, int max )
{
	SlotCount s;
	s.used = count + (extraSlots > 0 ? extraSlots : 0);
	s.known = max > 0;
	s.total = s.known ? max : 0;
	s.free = s.known && max > s.used ? max - s.used : 0;
	return s;
}

/// The card's word for the group: the capacity a caster asks about, in the card's own upper case.
inline const wchar_t *occupantWord( OccupantKind k )
{
	return k == OCC_GARRISON ? L"GARRISON" : k == OCC_TUNNEL ? L"TUNNEL NETWORK" : k == OCC_RIDER ? L"RIDER"
		: k == OCC_PASSENGERS ? L"PASSENGERS" : L"";
}

/// "3 / 10"; a rider has no count (a Combat Cycle always carries its one); no stated capacity = the count alone.
inline std::wstring occupantCount( OccupantKind k, const SlotCount &s )
{
	if (k == OCC_NONE || k == OCC_RIDER)
		return std::wstring();
	std::wstring out = std::to_wstring( s.used );
	if (s.known)
		out += L" / " + std::to_wstring( s.total );
	return out;
}

/// The whole head line as one string ("GARRISON  3 / 10") - for the [GXSPEC] trace; the card draws the two parts in two weights.
inline std::wstring occupantLabel( OccupantKind k, const SlotCount &s )
{
	const std::wstring count = occupantCount( k, s );
	return count.empty() ? std::wstring( occupantWord( k ) ) : std::wstring( occupantWord( k ) ) + L"  " + count;
}

struct CameoFit
{
	int icon;			///< px of one cell along the row (its width: the caller picks the shape)
	int units;			///< occupant cameos drawn
	int free;			///< empty-slot boxes drawn after them
	int overflow;		///< occupants not drawn
	bool moreCell;		///< ...counted in one "+N" cell, when the row has room for it (else the head line's count alone)
};

/// One row of availW px: every occupant then every free slot, at the largest cell size from maxIcon down to minIcon that fits
/// them all. If none does, the free boxes give way first, then the last occupants give way to one "+N" cell - and with no room
/// for even one cell, nothing is drawn past the row (spectator-occupants r1).
inline CameoFit fitCameos( int units, int freeSlots, int availW, int maxIcon, int minIcon, int gap )
{
	CameoFit f = { maxIcon, units, freeSlots, 0, false };
	for (int icon = maxIcon; icon >= minIcon; --icon)
	{
		const int cells = units + freeSlots;
		if (cells <= 0 || cells * icon + (cells - 1) * gap <= availW)
		{
			f.icon = icon;
			return f;
		}
	}
	f.icon = minIcon;
	const int room = availW + gap > 0 ? (availW + gap) / (minIcon + gap) : 0;
	if (units <= room)
	{
		f.free = room - units;
		return f;
	}
	f.free = 0;
	f.units = room > 0 ? room - 1 : 0;
	f.overflow = units - f.units;
	f.moreCell = room > 0;
	return f;
}

/// Which occupants just went in: a caster sees a unit enter. Fed every drawn frame with the container's id and its occupants'
/// ids; an occupant that was not inside on the previous update is "arrived" until ms have passed. The first look at a container
/// (another one, or the same one after the card was away for more than kGapMs) lights nothing - only arrivals while it is up.
struct ArrivalWatch
{
	enum { kGapMs = 500 };
	struct Seen { unsigned id, ms; bool arrived; };
	unsigned container = 0, lastMs = 0;
	bool seeded = false;
	std::vector<Seen> seen;
	void update( unsigned containerId, const unsigned *ids, int n, unsigned nowMs )
	{
		const bool fresh = !seeded || containerId != container || nowMs - lastMs > (unsigned)kGapMs;
		lastMs = nowMs;
		std::vector<Seen> next;
		next.reserve( n > 0 ? (size_t)n : 0 );
		for (int i = 0; i < n; ++i)
		{
			Seen s = { ids[i], nowMs, !fresh };
			for (const Seen &old : seen)
				if (!fresh && old.id == ids[i])
					s = old;
			next.push_back( s );
		}
		seen.swap( next );
		container = containerId;
		seeded = true;
	}
	bool lit( unsigned id, unsigned nowMs, unsigned ms ) const
	{
		for (const Seen &s : seen)
			if (s.id == id)
				return s.arrived && nowMs - s.ms < ms;
		return false;
	}
	void reset() { seen.clear(); seeded = false; container = 0; }
};

} // namespace GXSpectatorHud
