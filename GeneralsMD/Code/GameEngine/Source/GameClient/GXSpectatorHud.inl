/*
**	GeneralsX: the GeneralsX spectator HUD: "C2" caster layout + the "B3" Tab scoreboard,
**	in the game's blue (the owner's pick of 05/10/2026, points 1-16).
**
**	Included ONCE, at the end of InGameUI.cpp, instead of being a .cpp of its own: a new translation unit has to be
**	named in GeneralsMD/Code/GameEngine/CMakeLists.txt, and that file is inside the SimID digest - a display feature
**	would then split every lobby. InGameUI.cpp is digested only between its GX-SIMID markers; nothing here is.
**
**	WHO SEES IT: SpectatorUI=generalsx in GXHud.ini AND gxViewerMaySeeFeed() (replay playback, an observer seat, or a
**	defeated player). SpectatorUI=legacy (or the key absent) leaves every readout exactly as it was. The decisions that
**	can go wrong silently live in GXSpectatorHud.h, which is unit-tested.
**
**	PLAYER MODE (owner's pick of 05/10/2026, layout "C"): the same switch also gives a player still playing a versus game
**	the GeneralsX look - the control bar's art replaced by these navy panels, the retail radar window in the map's own
**	shape, the players bar folded into its tab, an economy line on the command bar and a player Tab board. FAIR BY
**	CONSTRUCTION: gatherRows() reads another player's private numbers only when maySeePrivate() allows it (never for a
**	player), and names his general and colour as retail's Diplomacy screen does (GameSlot's apparent values). See the
**	PLAYER MODE section below.
**
**	COST: everything is a plain read of state the local simulation already holds. Text is re-set only when its value
**	changes (txt() keys every string), unit and building counts are refreshed 4 times a second, and the "what they
**	bought" list is built only while the mouse is on a player's stars. Nothing here writes game state; the engine calls
**	with an effect are ControlBar::setObserverLookAtPlayer (the focus; retail's observer panel sets the same pointer),
**	HideControlBar (the retail F9 path; window visibility and the tactical view's height only). A spectator's click on the
**	battlefield only remembers an object id (inspectAt) - never the selection - and the OrderGate translator drops the
**	selection orders retail would still send for him.
*/

#include "GameClient/GXSpectatorHud.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/Mouse.h"
#include "GameClient/View.h"
#include "Common/GameUtility.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/Radar.h"
#include "Common/Science.h"
#include "Common/Upgrade.h"
#include "Common/UserPreferences.h"
#include "Common/OptionPreferences.h"
#include "GameLogic/GhostObject.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/RankInfo.h"
#include "GameNetwork/NetworkInterface.h"

#include <map>
#include <vector>
#include "GameClient/GXVoteKick.h"
#include <string>
#include <utility>

extern void HideControlBar( Bool immediate );

/// W3DRadar::drawViewBox clips to this while the HUD draws the radar into its own panel (defined in W3DRadar.cpp).
extern IRegion2D *gxRadarViewBoxClip;
extern void ShowControlBar( Bool immediate );

/// ControlBarScheme.cpp: TRUE skips the control bar's background art while player mode draws its own panels under it.
extern Bool gxControlBarArtOff;
extern IRegion2D gxControlBarArtHole;
extern Bool gxRadarRetailMargins;
/// ControlBar.cpp: a copy of retail's m_genStarFlash, written at every place the original is.
extern Bool gxGenStarFlash;

Bool gxVoteKickBannerRect( Int *x, Int *y, Int *w, Int *h );		// GXVoteKickHud.inl (global, included after this file)

namespace GXSpecHud
{
using namespace GXSpectatorHud;

// ---------------------------------------------------------------------------------------------------------------------
// Palette: ControlBar Pro's navy glass and steel-blue frame, one blue accent. No green (owner: "do the game's blue").
// ---------------------------------------------------------------------------------------------------------------------
static Color navy( Int a )		{ return GameMakeColor( 10, 26, 54, a ); }
static Color navyDeep( Int a )	{ return GameMakeColor( 4, 13, 30, a ); }
static Color steel()				{ return GameMakeColor( 118, 158, 214, 235 ); }
static Color steelDim()			{ return GameMakeColor( 64, 92, 134, 220 ); }
static Color edge()					{ return GameMakeColor( 0, 0, 0, 190 ); }
static Color accent()				{ return GameMakeColor( 111, 168, 240, 255 ); }
static Color white()				{ return GameMakeColor( 238, 243, 250, 255 ); }
static Color label()				{ return GameMakeColor( 186, 210, 244, 255 ); }	// owner 05/10: "the text in blue is barely visible"
static Color dim()					{ return GameMakeColor( 140, 156, 180, 255 ); }
static Color red()					{ return GameMakeColor( 255, 112, 112, 255 ); }
static Color green()				{ return GameMakeColor( 120, 220, 120, 255 ); }	// the lobby/Diplomacy greens, lifted for the navy panel
static Color amber()				{ return GameMakeColor( 240, 192, 82, 255 ); }
static Color qualityColor( Int cls )	{ return cls == 1 ? green() : cls == 2 ? amber() : cls == 3 ? red() : dim(); }	// pingClass / fpsClass; 0 = no figure
static Color team1()				{ return GameMakeColor( 111, 168, 240, 255 ); }
static Color team2()				{ return GameMakeColor( 224, 162, 74, 255 ); }

enum HitKind { H_TAB, H_BODY, H_CARD, H_ROW, H_STARS, H_MAP, H_CHECK, H_PRESET, H_RESET, H_SLIDER, H_HIDEP, H_HIDEDEAD, H_CLOSE,
	H_BLOCK, H_MUTE, H_KICK, H_TOAST, H_CHIP, H_FOLLOWCAM, H_BARPAGE, H_SCROLL };	///< H_BLOCK: a panel that only keeps clicks off the battlefield under it (panel -1: no dock action);
								///< H_MUTE / H_KICK: the player board's row buttons (panel -1; id = game slot / player index)
enum Capture { CAP_NONE, CAP_DOCK, CAP_MAP, CAP_SLIDER, CAP_WORLD, CAP_SCROLL };	///< CAP_WORLD: a spectator's press on the battlefield (a click there inspects)
enum Action { A_BOARD, A_PLAYERS, A_MAP, A_FOCUS, A_TOAST, A_DISPLAY, A_HIDEDEAD, A_DOCKALL, A_OPDOWN, A_OPUP, A_NEXT, A_PREV, A_DIPLOMACY, A_COUNT };

struct Hit { Rect r; Int kind; Int id; Int panel; };
/// One player as the HUD shows him. The private numbers are the PrivateStats base (GXSpectatorHud.h): cleared before
/// every fill and filled only for a viewer allowed to see them, so a field added there cannot leak to a player.
struct Row : PrivateStats
{
	Player *player;
	Int slot, team;
	Color color, textColor;
	Status status;
	Bool own;									///< the local player's own row
	Bool ally;								///< another player on the local player's lobby team (same team number, >= 0)
	const Image *portrait;
	UnicodeString general;
	Bool randomGeneral;				///< `general` is the lobby's "Random": said without " General"
};
struct Chord { Int key; Int mods; Bool valid; };
struct Txt { DisplayString *ds; Int fontKey; Int gen; long long key; Bool has; };

// ---- per-match state (reset() clears it) ----------------------------------------------------------------------------
static Settings s_set;
static Bool s_loaded = FALSE;
static Bool s_modeOn = FALSE;				///< this match is showing the GeneralsX spectator HUD
static Mode s_mode = MODE_LEGACY;		///< what this frame draws: decideMode(), from the switch and the viewer
static Bool s_inputLive = FALSE;		///< drawn this frame and not under the quit menu: the translator may act
static Dock s_dock;
static Bool s_boardHeld = FALSE;
static Bool s_escOwned = FALSE;		///< an Esc DOWN that left the view: its repeats and up are ours, like the board key below
static Int s_boardOwnKey = 0;			///< the physical key whose DOWN opened the board: its repeats and up are ours until the up is drained, shown or not
static Bool s_boardKeyUp = FALSE;		///< that key's up is already in the stream: BoardGate (priority 9) sees it before Translator (11) closes the board
static Bool s_puiTest = FALSE;			///< GX_PLAYERUI_SHOW is set (tests): the board gate's [GXPUI] trace
static Bool s_hidden[MAX_SLOTS];
static Status s_lastStatus[MAX_SLOTS];
static UnsignedInt s_statusFrame[MAX_SLOTS];
static Bool s_boardNameCut[MAX_SLOTS];		///< this slot's name was shortened on the player Tab board (its hover tip)
static Bool s_nameCut[MAX_SLOTS];			///< this slot's name was shortened on the top bar: its full name goes on the hover tip

/// A teammate by the LOBBY is not enough: an alliance can be broken in the match (PLAYER_RELATES_PLAYER), and a former
/// teammate is an enemy whose purchases a player must not see. The live relationship decides.
static Bool liveAlly( const Player *other )
{
	const Player *me = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	return me != nullptr && other != nullptr && other != me && me->getRelationship( other->getDefaultTeam() ) == ALLIES
		&& other->getRelationship( me->getDefaultTeam() ) == ALLIES;
}

/// The name every panel shows for a player. GX_SPECHUD_LONGNAME="<player index>:<text>" (tests only: a PICTURE of a 16+
/// character name on one player) replaces that one player's name here and nowhere else - it reaches no setting and no message.
static UnicodeString hudName( Player *p )
{
	static Int idx = -2;
	static UnicodeString text;
	if (idx == -2)
	{
		idx = -1;
		if (const char *e = getenv( "GX_SPECHUD_LONGNAME" ))
			if (const char *c = strchr( e, ':' ))
			{
				idx = atoi( e );
				AsciiString a( c + 1 );
				text.translate( a );
			}
	}
	if (idx >= 0 && p != nullptr && p->getPlayerIndex() == idx)
		return text;
	return p->getPlayerDisplayName();
}
static Int s_units[MAX_SLOTS], s_bldgs[MAX_SLOTS], s_inf, s_veh, s_air;
static UnsignedInt s_countMs = 0;
static Bool s_seeded = FALSE;				///< the first gather of the match has run (see the status times)
static Int s_needed[32];
static Int s_levels = 0;
static Chord s_chords[A_COUNT];
static UnsignedInt s_matchStartMs = 0;	///< the toast ignores announcements older than this match
struct Stage { UnsignedInt frame; std::string list; };
struct FakeStatus { Int slot; UnsignedInt frame; Status status; };
static Stage s_stages[8];
static Int s_stageCount = 0, s_stageNext = 0;
static FakeStatus s_fake[4];
static Int s_fakeCount = 0;
static Int s_forceHover = -1;				///< GX_SPECHUD_SHOW hover=<slot>
static Int s_forceFollow = -1;			///< GX_SPECHUD_SHOW follow=<slot>, applied on the first frame
static Int s_forceView = -1;				///< GX_SPECHUD_SHOW view=<slot>: through his eyes (the retail toggle player view)
static Rect s_toastRect = { 0, 0, 0, 0 };	///< where the event toast was last drawn, and when (GX_SPECHUD_SHOW clicktoast, tests only)
static UnsignedInt s_toastRectMs = 0;
static Rect s_focusDrawn = { 0, 0, 0, 0 };		///< this frame's followed-player panel, its object band and its FOCUS tab (the toasts keep off)
static Rect s_chipRect = { 0, 0, 0, 0 };	///< ...and the VIEWING chip (GX_SPECHUD_SHOW clickchip, tests only)
static UnsignedInt s_chipRectMs = 0;
static const Image *s_portrait[MAX_SLOTS];
static const PlayerTemplate *s_portraitFor[MAX_SLOTS];
static UnicodeString s_general[MAX_SLOTS];

// ---- per-frame ----------------------------------------------------------------------------------------------------
static Row s_rows[MAX_SLOTS];
static Int s_rowCount = 0;
static Bool s_twoTeams = FALSE;
static Int s_blocks[MAX_SLOTS + 1];		///< team blocks in display order (teamBlocks), -1 = the teamless
static Int s_blockCount = 0;
static Hit s_hits[256];
static Int s_hitCount = 0;
static Real s_scale = 1.0f;
static Int s_W = 0, s_H = 0;
static Int s_alpha = 128;
static Rect s_mapInner = { 0, 0, 0, 0 };
static Rect s_slider = { 0, 0, 0, 0 };
static Int s_topInset = 0;					///< the retail message list and superweapon timers start below this
static Bool s_insetSpanOn = FALSE;			///< the span below is a bar actually DRAWN this frame (the folded tab reserves nothing sideways)
static Int s_insetX0 = 0, s_insetX1 = 0, s_insetY0 = 0;	///< the horizontal span of what s_topInset protects (valid while s_topInset > 0): the retail message column only needs to clear it where it overlaps
static GameWindow *s_barWindow = nullptr;	///< ControlBar.wnd:ControlBarParent, the retail bar's only top-level window

// ---- input --------------------------------------------------------------------------------------------------------
static Capture s_capture = CAP_NONE;
static Int s_staleMoves = 0;				///< position messages seen with the button up while a capture is open
static Hit s_pressHit;
static ICoord2D s_worldPress = { 0, 0 };	///< where that battlefield press began (CAP_WORLD)
static Bool s_leftOwned = FALSE;		///< a left-button DOWN we took (a capture): its drag and up are ours even if the HUD hides before it
static Bool s_rightOwned = FALSE;		///< a right-button DOWN over a player-mode panel was ours: so are its drag and up
static Rect s_boardRect = { 0, 0, 0, 0 };	///< the player Tab board as drawn this frame (drawPlayerBoard); w 0 = not up (BoardGate)
static Int s_boardPress = 0;				///< mouse buttons (1 left, 2 right, 4 middle) pressed on that board and not yet released
static Hit s_boardPressHit = { { 0, 0, 0, 0 }, -1, 0, -1 };	///< the board hit under the left press BoardGate took (kind -1: none)

// ---- text and fonts -----------------------------------------------------------------------------------------------
static Int s_fontGen = 0;
static std::map<Int, GameFont *> s_fonts;
static std::map<std::pair<const char *, Int>, Txt> s_txt;

static Int S( Real v ) { return (Int)(v * s_scale + 0.5f); }

/// Pixel heights are 1080p pixels; the point size is solved from one reference font so a 4K screen gets the same
/// proportions without walking a table of sizes (each winFindFont of a new size builds a font).
/// The "GXHUD:" prefix asks the font library for the smooth (supersampled) rasteriser: Core/.../WW3D2/gxtextsmooth.h.
static const char *const HUD_FONT = "GXHUD:Arial";
static GameFont *font( Int px, Bool bold )
{
	const Int want = max( 7, S( (Real)px ) );
	const Int k = want * 2 + (bold ? 1 : 0);
	std::map<Int, GameFont *>::iterator it = s_fonts.find( k );
	if (it != s_fonts.end())
		return it->second;
	GameFont *f = nullptr;
	if (TheWindowManager)
	{
		GameFont *ref = TheWindowManager->winFindFont( AsciiString( HUD_FONT ), 20, bold );
		if (ref && ref->height > 0)
			f = TheWindowManager->winFindFont( AsciiString( HUD_FONT ), max( 4, (Int)(want * 20.0f / ref->height + 0.5f) ), bold );
	}
	s_fonts[k] = f;
	return f;
}

/// One DisplayString per (id, sub); its text is rebuilt only when `key` changes - setText on a W3D string re-lays the
/// whole sentence, which is the expensive part of HUD text.
template <typename F>
static DisplayString *txt( const char *id, Int sub, Int px, Bool bold, long long key, F make )
{
	GameFont *f = font( px, bold );
	if (f == nullptr || TheDisplayStringManager == nullptr)
		return nullptr;
	Txt &t = s_txt[std::make_pair( id, sub )];
	if (t.ds == nullptr)
	{
		t.ds = TheDisplayStringManager->newDisplayString();
		if (t.ds == nullptr)
			return nullptr;
		t.fontKey = -1;
		t.has = FALSE;
	}
	const Int fk = px * 2 + (bold ? 1 : 0);
	if (t.fontKey != fk || t.gen != s_fontGen)
	{
		t.ds->setFont( f );
		t.fontKey = fk;
		t.gen = s_fontGen;
	}
	if (!t.has || t.key != key)
	{
		t.ds->setText( make() );
		t.key = key;
		t.has = TRUE;
	}
	return t.ds;
}

static DisplayString *fixed( const char *id, Int px, Bool bold, const wchar_t *text )
{
	return txt( id, 0, px, bold, 0, [text]() { return UnicodeString( text ); } );
}

static DisplayString *number( const char *id, Int sub, Int px, Bool bold, Int v, Bool plus = FALSE, Bool dollar = FALSE )
{
	return txt( id, sub, px, bold, v, [v, plus, dollar]() {
		// `plus` is the income figure ("+$1,800 /MIN"; a negative one reads "-$1,800"), `dollar` an amount ("$180,424"): cashPrefix
		UnicodeString p( cashPrefix( v, plus, dollar ) );
		p.concat( formatGroupedValue( (UnsignedInt)(plus ? (v < 0 ? -v : v) : max( 0, v )) ) );
		return p;
	} );
}

static Int tw( DisplayString *ds ) { return ds ? ds->getWidth() : 0; }
static Int th( Int px ) { GameFont *f = font( px, FALSE ); return f ? f->height : S( (Real)px ); }
static void put( DisplayString *ds, Int x, Int y, Color c ) { if (ds) ds->draw( x, y, c, 0 ); }
static void putR( DisplayString *ds, Int right, Int y, Color c ) { if (ds) ds->draw( right - ds->getWidth(), y, c, 0 ); }
static void putC( DisplayString *ds, Int cx, Int y, Color c ) { if (ds) ds->draw( cx - ds->getWidth() / 2, y, c, 0 ); }

/// `name` cut to fit `maxW` pixels in this font, with an ellipsis ("Hard Arm" is a bug, "Hard Ar..." is a name; owner
/// 06/10). Measured on a scratch string with the real font, so it is exact for any script; the result is cached by the
/// caller's txt() key. Returns whether it was cut through *cut (the full name then goes on the hover tip).
static UnicodeString ellipsize( const UnicodeString &name, Int px, Bool bold, Int maxW, Bool *cut )
{
	if (cut)
		*cut = FALSE;
	GameFont *f = font( px, bold );
	if (f == nullptr || TheDisplayStringManager == nullptr || maxW <= 0)
		return name;
	DisplayString *m = TheDisplayStringManager->newDisplayString();
	if (m == nullptr)
		return name;
	m->setFont( f );
	m->setText( name );
	UnicodeString out = name;
	if (m->getWidth() > maxW)
	{
		if (cut)
			*cut = TRUE;
		for (Int n = name.getLength() - 1; n >= 1; --n)
		{
			UnicodeString t = name;
			t.truncateTo( n );
			t.concat( L"\x2026" );
			m->setText( t );
			if (m->getWidth() <= maxW || n == 1)
			{
				out = t;
				break;
			}
		}
	}
	TheDisplayStringManager->freeDisplayString( m );
	return out;
}

static void fill( Int x, Int y, Int w, Int h, Color c ) { if (w > 0 && h > 0) TheDisplay->drawFillRect( x, y, w, h, c ); }
static void fill( const Rect &r, Color c ) { fill( r.x, r.y, r.w, r.h, c ); }
static void box( const Rect &r, Color c ) { if (r.w > 1 && r.h > 1) TheDisplay->drawOpenRect( r.x, r.y, r.w, r.h, 1.0f, c ); }

static Real s_fade = 1.0f;		///< < 1 only while the event toast fades in or out

static Color faded( Color c )
{
	if (s_fade >= 1.0f)
		return c;
	UnsignedByte r = 0, g = 0, b = 0, a = 255;
	GameGetColorComponents( c, &r, &g, &b, &a );
	return GameMakeColor( r, g, b, (Int)(a * s_fade) );
}

/// Panels that carry text keep at least this much navy whatever the opacity (owner 05/10/2026: "the values in white
/// compared to the text in blue is not good; the text in blue is barely visible"; no text shadows - removed on purpose in
/// 0.9.56). ~71% holds the labels above 4.5:1 over the brightest ground measured in real frames (player-ui-c contrast.py).
/// Panels without text of ours (the bar's islands, the map) follow the opacity setting alone.
enum { TEXT_ALPHA_FLOOR = 182 };
static Int textAlpha() { return max( s_alpha, (Int)TEXT_ALPHA_FLOOR ); }

/// A panel: navy glass at the viewer's opacity, a dark outer line and the steel-blue frame (one shape: square).
static void plate( const Rect &r, Bool deep = FALSE, Bool text = FALSE )
{
	const Int a = text ? textAlpha() : s_alpha;
	fill( r, deep ? navyDeep( a ) : navy( a ) );
	Rect o = { r.x - 1, r.y - 1, r.w + 2, r.h + 2 };
	box( o, faded( edge() ) );
	box( r, faded( steel() ) );
}

static void addHit( const Rect &r, Int kind, Int id, Int panel )
{
	if (s_hitCount < (Int)ARRAY_SIZE( s_hits ))
	{
		Hit &h = s_hits[s_hitCount++];
		h.r = r;
		h.kind = kind;
		h.id = id;
		h.panel = panel;
	}
}

/// Income per minute is for sandbox play only (owner 06/10, after a community player: "I don't need this info"): a skirmish or
/// single-player game, or any match whose lobby verdict is GameInfo::START_SANDBOX (nobody's stats count). In every other
/// multiplayer match a PLAYER sees neither it nor UNITS/BUILDINGS; spectators and observers keep every figure.
static Bool sandboxMatch()
{
	static const Bool forced = getenv( "GX_PLAYERUI_SANDBOX" ) != nullptr;		// tests only: a picture of the sandbox figures in an AI match
	if (forced)
		return TRUE;
	if (TheGameLogic != nullptr && (TheGameLogic->isInSkirmishGame() || TheGameLogic->isInSinglePlayerGame()))
		return TRUE;
	return TheGameInfo != nullptr && TheGameInfo->getStartVerdict() == GameInfo::START_SANDBOX;
}

static Bool mouseIn( const Rect &r )
{
	const MouseIO *m = TheMouse ? TheMouse->getMouseStatus() : nullptr;
	return m != nullptr && r.contains( m->pos.x, m->pos.y );
}

static UnicodeString clockText( UnsignedInt frame )
{
	const UnsignedInt s = frame / LOGICFRAMES_PER_SECOND;
	UnicodeString u;
	if (s >= 3600)
		u.format( L"%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60 );
	else
		u.format( L"%02u:%02u", s / 60, s % 60 );
	return u;
}

/// A small filled triangle, the dock tabs' arrow (no glyph needed from the font).
static void arrow( Int cx, Int cy, Int size, Bool up, Color c )
{
	for (Int i = 0; i < size; ++i)
	{
		const Int y = up ? cy - size / 2 + i : cy + size / 2 - i;
		fill( cx - i, y, i * 2 + 1, 1, c );
	}
}

/// The same triangle pointing sideways (the band's page buttons).
static void arrowSide( Int cx, Int cy, Int size, Bool left, Color c )
{
	for (Int i = 0; i < size; ++i)
	{
		const Int x = left ? cx - size / 2 + i : cx + size / 2 - i;
		fill( x, cy - i, 1, i * 2 + 1, c );
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings and keybinds
// ---------------------------------------------------------------------------------------------------------------------
static AsciiString gxHudPath()
{
	AsciiString path = TheGlobalData ? TheGlobalData->getPath_UserData() : AsciiString::TheEmptyString;
	path.concat( "GXHud.ini" );
	return path;
}

// GeneralsX: GXHud.ini is read and written through SettingsFile (GXSpectatorHud.h), which
// tells a file that is not there from one that cannot be read: the second is never replaced, and a match whose load
// could not read it saves nothing at all (it would save the defaults). The reasons go to the log once a match.
static SettingsFile s_file;
static Bool s_fileNoted = FALSE;

static void noteFileOnce( const std::string &why )
{
	if (s_fileNoted || why.empty())
		return;
	s_fileNoted = TRUE;
	fprintf( stderr, "[GXSPEC] %s\n", why.c_str() );
}

/// The in-game panel writes back through writeSettings, which keeps the launcher's lines (the HUD anchor) untouched.
/// Written to a sibling file and swapped in, so a crash mid-write cannot leave the launcher a half file.
static void saveSettings()
{
	std::string why;
	if (!s_file.save( gxHudPath().str(), s_set, &why ))
		noteFileOnce( why );
}

static Bool lookupName( const LookupListRec *table, const std::string &name, Int *out )
{
	for (const LookupListRec *r = table; r->name; ++r)
		if (name == r->name)
		{
			*out = r->value;
			return TRUE;
		}
	return FALSE;
}

static void loadChords()
{
	static const char *const keys[A_COUNT] = {
		"GXSpecBoardKey", "GXSpecPlayersKey", "GXSpecMapKey", "GXSpecFocusKey", "GXSpecToastKey", "GXSpecDisplayKey",
		"GXSpecHideDeadKey", "GXSpecDockAllKey", "GXSpecOpacityDownKey", "GXSpecOpacityUpKey", "GXSpecNextKey", "GXSpecPrevKey",
		"GXSpecDiplomacyKey" };
	// The four panel keys are Shift+Alt+1..4: Alt+1..4 are retail's VIEW_TEAM1..4 (Data\English\CommandMap.ini), and retail binds nothing to
	// Shift+Alt. A player who set his own chord keeps it: only the defaults moved (07/10/2026, wh1ter0se: "see with WHAT it clashes").
	static const char *const defaults[A_COUNT] = {
		"KEY_TAB", "SHIFT_ALT+KEY_1", "SHIFT_ALT+KEY_2", "SHIFT_ALT+KEY_3", "SHIFT_ALT+KEY_4", "ALT+KEY_S",
		"ALT+KEY_H", "ALT+KEY_D", "ALT+KEY_MINUS", "ALT+KEY_EQUAL", "ALT+KEY_N", "ALT+KEY_B",
		"SHIFT+KEY_TAB" };	// Tab is the scoreboard here, so retail Diplomacy (mute, ping) moves to Shift+Tab
	UserPreferences prefs;
	const Bool have = prefs.load( "CommandButtonHotKeys.ini" );
	for (Int i = 0; i < A_COUNT; ++i)
	{
		AsciiString value;
		if (have)
		{
			value = prefs.getAsciiString( keys[i], AsciiString::TheEmptyString );
			value.trim();
		}
		std::string mod, key;
		Chord &c = s_chords[i];
		c.valid = FALSE;
		Int k = 0, m = 0;
		const Bool parsed = parseChord( value.isEmpty() ? std::string( defaults[i] ) : std::string( value.str() ), mod, key );
		if (parsed && lookupName( KeyNames, key, &k ) && (mod.empty() || lookupName( ModifierNames, mod, &m )))
		{
			c.key = k;
			c.mods = m;
			c.valid = TRUE;
		}
		else if (!value.isEmpty())
			fprintf( stderr, "[GXSPEC] %s = '%s' ignored - not KEY_x or MOD+KEY_x\n", keys[i], value.str() );
	}
}

static void loadPlayerChords();		// player mode, below
static void loadPlayerStages();

static void loadMatchState()
{
	std::string why;
	s_fileNoted = FALSE;
	s_set = s_file.load( gxHudPath().str(), &why );
	noteFileOnce( why );
	// "Follow the player's camera" is Options.ini's UseCameraInReplays, read again for every replay: on a map whose map.ini
	// overrides GameData, TheWritableGlobalData is a copy for that match only, so the row's change during it died with the
	// copy while Options.ini kept it.
	if (TheRecorder != nullptr && TheRecorder->isPlaybackMode() && TheWritableGlobalData != nullptr)
		TheWritableGlobalData->m_useCameraInReplay = OptionPreferences().useCameraInReplays();
	loadChords();
	loadPlayerChords();
	loadPlayerStages();
	s_levels = TheRankInfoStore ? min( (Int)ARRAY_SIZE( s_needed ), TheRankInfoStore->getRankLevelCount() ) : 0;
	for (Int i = 0; i < s_levels; ++i)
	{
		const RankInfo *ri = TheRankInfoStore->getRankInfo( i + 1 );
		s_needed[i] = ri ? ri->m_skillPointsNeeded : 0;
	}
	s_loaded = TRUE;
	s_matchStartMs = timeGetTime();

	// Screenshot runs open what a spectator would open by hand - synthetic input is not an option on the test Mini, where a
	// click is a game order. GX_SPECHUD_SHOW = stages "<frame>:<list>" separated by ';' (a bare list = frame 0), list =
	// players,map,focus,board,display,hover=<slot>,follow=<slot>,view=<slot>, and real clicks: click=<x>x<y>,
	// lookunit=<slot>[v] (camera to one of his objects, testUnit), clickunit (a press and release where it is drawn), clicktoast
	// (the event toast, if one is up); each
	// stage starts from everything docked. '/' also separates stages.
	// GX_SPECHUD_FAKE = "dead=<slot>@<frame>,left=<slot>@<frame>" forces a status for a PICTURE of Dead/Left in a match
	// where nobody dies or quits in time; it is logged, and decideStatus() itself is unit-tested.
	s_stageCount = s_stageNext = s_fakeCount = 0;
	s_forceHover = s_forceFollow = -1;
	if (const char *e = getenv( "GX_SPECHUD_SHOW" ))
	{
		const std::string all( e );
		size_t pos = 0;
		while (pos <= all.size() && s_stageCount < (Int)ARRAY_SIZE( s_stages ))
		{
			size_t semi = all.find_first_of( ";/", pos );	// '/' too: a ';' cannot cross an unquoted ssh command line
			if (semi == std::string::npos)
				semi = all.size();
			std::string st = all.substr( pos, semi - pos );
			const size_t colon = st.find( ':' );
			s_stages[s_stageCount].frame = colon == std::string::npos ? 0 : (UnsignedInt)atoi( st.c_str() );
			s_stages[s_stageCount].list = colon == std::string::npos ? st : st.substr( colon + 1 );
			++s_stageCount;
			pos = semi + 1;
		}
		fprintf( stderr, "[GXSPEC] TEST GX_SPECHUD_SHOW=%s\n", e );
	}
	if (const char *e = getenv( "GX_SPECHUD_FAKE" ))
	{
		for (const char *p = e; *p && s_fakeCount < (Int)ARRAY_SIZE( s_fake ); )
		{
			FakeStatus &f = s_fake[s_fakeCount];
			f.status = strncmp( p, "left=", 5 ) == 0 ? STATUS_LEFT : STATUS_DEAD;
			const char *eq = strchr( p, '=' ), *at = eq ? strchr( eq, '@' ) : nullptr;
			if (eq == nullptr || at == nullptr)
				break;
			f.slot = atoi( eq + 1 );
			f.frame = (UnsignedInt)atoi( at + 1 );
			++s_fakeCount;
			const char *comma = strchr( at, ',' );
			if (comma == nullptr)
				break;
			p = comma + 1;
		}
		fprintf( stderr, "[GXSPEC] TEST GX_SPECHUD_FAKE=%s (%d forced statuses - pictures only)\n", e, s_fakeCount );
	}
}

static void injectClick( const std::string &xy, Bool right );		// player mode's test click (below)

/// What a container's own module says about listing its contents (the card adds whether it shows the portrait, and disguises).
static ContainFacts containFacts( Object *o )
{
	ContainFacts f;
	if (ContainModuleInterface *contain = o->getContain())
	{
		f.hasContain = true;
		f.displayedOnControlBar = contain->isDisplayedOnControlBar();
		f.garrisonable = contain->isGarrisonable();
		f.tunnel = contain->isTunnelContain();
		f.riderChange = contain->isRiderChangeContain();
	}
	f.underConstruction = o->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION );
	return f;
}

/// GX_SPECHUD_SHOW lookunit=<slot>[v|g|t|n] / clickunit (tests only): lookunit picks one of that player's selectable objects - a
/// building with something in its production queue first (a 'v' suffix: a vehicle first), then a vehicle, a building, anything -
/// remembers it and points the camera at it; a later clickunit sends a real press and release to where it is drawn then.
/// g / t / n (the occupants pictures): the garrison or bunker / the transport or rider / the tunnel holding the most occupants;
/// a slot no player has (99) searches every player.
struct UnitFind { const Object *found; Int rank; Bool vehicleFirst; Char want; };
static ObjectID s_testUnit = INVALID_ID;
static void findUnit( Object *obj, void *userData )
{
	UnitFind *f = (UnitFind *)userData;
	if (obj->isEffectivelyDead() || obj->getDrawable() == nullptr || !obj->isSelectable())
		return;
	if (f->want != 0)
	{
		const OccupantKind k = occupantKind( containFacts( obj ) );
		const Bool match = f->want == 'g' ? k == OCC_GARRISON : f->want == 'n' ? k == OCC_TUNNEL : k == OCC_PASSENGERS || k == OCC_RIDER;
		const Int inside = match ? (Int)obj->getContain()->getContainCount() : 0;
		if (inside > 0 && (f->found == nullptr || -inside < f->rank))
		{
			f->found = obj;
			f->rank = -inside;
		}
		return;
	}
	ProductionUpdateInterface *pu = obj->getProductionUpdateInterface();
	const Bool producing = obj->isKindOf( KINDOF_STRUCTURE ) && pu != nullptr && pu->firstProduction() != nullptr;
	const Bool vehicle = obj->isKindOf( KINDOF_VEHICLE );
	const Int rank = producing ? (f->vehicleFirst ? 1 : 0) : vehicle ? (f->vehicleFirst ? 0 : 1) : obj->isKindOf( KINDOF_STRUCTURE ) ? 2 : 3;
	if (f->found == nullptr || rank < f->rank)
	{
		f->found = obj;
		f->rank = rank;
	}
}

static void testUnit( const std::string &item, Bool click )
{
	const Object *o = nullptr;
	if (!click)
	{
		const size_t eq = item.find( '=' );
		Player *p = ThePlayerList ? ThePlayerList->getPlayerFromSlotIndex( atoi( item.c_str() + eq + 1 ) ) : nullptr;
		const Char last = item.back();
		UnitFind f = { nullptr, 4, last == 'v', (Char)(last == 'g' || last == 't' || last == 'n' ? last : 0) };
		if (p != nullptr)
			p->iterateObjects( findUnit, &f );
		for (Int i = 0; p == nullptr && f.want != 0 && ThePlayerList != nullptr && i < ThePlayerList->getPlayerCount(); ++i)
			ThePlayerList->getNthPlayer( i )->iterateObjects( findUnit, &f );		// no such slot: anyone's
		o = f.found;
		s_testUnit = o != nullptr ? o->getID() : INVALID_ID;
		if (o != nullptr && TheTacticalView != nullptr)
			TheTacticalView->lookAt( o->getPosition() );
	}
	else if ((o = TheGameLogic->findObjectByID( s_testUnit )) != nullptr && TheTacticalView != nullptr)
	{
		ICoord2D sp;
		if (TheTacticalView->worldToScreen( o->getPosition(), &sp ))
		{
			char xy[40];
			snprintf( xy, sizeof( xy ), "%d;%d", sp.x, sp.y );
			injectClick( xy, FALSE );
		}
	}
	// ... and where the minimap shows it (the inverse of minimapLookAt, to the cell's centre): a picture pair can move both arms'
	// cameras there with click=<x>x<y> on the map panel, which a build without this change understands too.
	ICoord2D cell = { 0, 0 };
	Int mx = -1, my = -1;
	if (o != nullptr && s_mapInner.w > 0 && s_mapInner.h > 0 && TheRadar != nullptr && TheRadar->worldToRadar( o->getPosition(), &cell ))
	{
		mx = s_mapInner.x + (2 * cell.x + 1) * s_mapInner.w / (2 * RADAR_CELL_WIDTH);
		my = s_mapInner.y + (2 * (RADAR_CELL_HEIGHT - 1 - cell.y) + 1) * s_mapInner.h / (2 * RADAR_CELL_HEIGHT);
	}
	fprintf( stderr, "[GXSPEC] TEST %s obj=%u tmpl=%s minimap=%dx%d\n", item.c_str(), o != nullptr ? (unsigned)o->getID() : 0u,
		o != nullptr ? o->getTemplate()->getName().str() : "-", mx, my );
}

/// GX_SPECHUD_SHOW occscan=<frames> (tests only, the occupants pictures): from then on, every <frames> logic frames, one
/// [GXSPEC] occscan line per container ON SCREEN that lists its contents and holds some, with where it is drawn - so a picture
/// pair can click it with click=<x>x<y>, which a build without this change understands too (same replay, same GX_CAM camera).
static UnsignedInt s_occScanEvery = 0, s_occScanNext = 0;
static void occScanOne( Object *obj, void * )
{
	if (obj->isEffectivelyDead() || obj->getDrawable() == nullptr || !obj->isSelectable())
		return;
	const OccupantKind k = occupantKind( containFacts( obj ) );
	ContainModuleInterface *c = obj->getContain();
	if (k == OCC_NONE || c->getContainCount() == 0 || TheTacticalView == nullptr)
		return;
	ICoord2D sp;
	if (!TheTacticalView->worldToScreen( obj->getPosition(), &sp ) || sp.x < 0 || sp.y < 0 || sp.x >= s_W || sp.y >= s_H)
		return;
	fprintf( stderr, "[GXSPEC] occscan f=%u obj=%u kind=%d inside=%u max=%d tmpl=%s at=%dx%d\n", TheGameLogic->getFrame(),
		(unsigned)obj->getID(), (int)k, (unsigned)c->getContainCount(), c->getContainMax(), obj->getTemplate()->getName().str(), sp.x, sp.y );
}
static void occScan()
{
	if (s_occScanEvery == 0 || ThePlayerList == nullptr || TheGameLogic->getFrame() < s_occScanNext)
		return;
	s_occScanNext = TheGameLogic->getFrame() + s_occScanEvery;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		ThePlayerList->getNthPlayer( i )->iterateObjects( occScanOne, nullptr );
}

static void applyStage( const std::string &list )
{
	s_dock.reset();
	s_forceHover = -1;
	size_t pos = 0;
	while (pos <= list.size())
	{
		size_t comma = list.find( ',', pos );
		if (comma == std::string::npos)
			comma = list.size();
		const std::string item = trim( list.substr( pos, comma - pos ) );
		if (item == "players") s_dock.panel[PANEL_PLAYERS].open = true;
		else if (item == "map") s_dock.panel[PANEL_MAP].open = true;
		else if (item == "focus") s_dock.panel[PANEL_FOCUS].open = true;
		else if (item == "board") s_dock.panel[PANEL_BOARD].open = true;
		else if (item == "display") s_dock.panel[PANEL_DISPLAY].open = true;
		else if (item.compare( 0, 6, "hover=" ) == 0) s_forceHover = atoi( item.c_str() + 6 );
		else if (item.compare( 0, 7, "follow=" ) == 0) s_forceFollow = atoi( item.c_str() + 7 );
		else if (item.compare( 0, 5, "view=" ) == 0) s_forceView = atoi( item.c_str() + 5 );
		else if (item.compare( 0, 6, "click=" ) == 0)
		{
			std::string xy = item.substr( 6 );		// <x>x<y>: ';' already separates stages (spectator-click)
			const size_t sep = xy.find( 'x' );
			if (sep != std::string::npos)
				injectClick( xy.replace( sep, 1, ";" ), FALSE );
		}
		else if (item.compare( 0, 9, "lookunit=" ) == 0) testUnit( item, FALSE );
		else if (item == "clickunit") testUnit( item, TRUE );
		else if (item.compare( 0, 8, "occscan=" ) == 0) s_occScanEvery = (UnsignedInt)max( 1, atoi( item.c_str() + 8 ) );
		else if (item == "clicktoast" && s_toastRect.w > 0 && timeGetTime() - s_toastRectMs < 500)
		{
			char xy[40];
			snprintf( xy, sizeof( xy ), "%d;%d", s_toastRect.x + s_toastRect.w / 2, s_toastRect.y + s_toastRect.h / 2 );
			injectClick( xy, FALSE );
		}
		else if (item == "clickchip" && s_chipRect.w > 0 && timeGetTime() - s_chipRectMs < 500)
		{
			char xy[40];
			snprintf( xy, sizeof( xy ), "%d;%d", s_chipRect.x + s_chipRect.w / 2, s_chipRect.y + s_chipRect.h / 2 );
			injectClick( xy, FALSE );
		}
		pos = comma + 1;
	}
	fprintf( stderr, "[GXSPEC] TEST stage f=%u '%s'\n", TheGameLogic->getFrame(), list.c_str() );
}

// ---------------------------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------------------------
static const Image *portraitFor( const PlayerTemplate *pt )
{
	if (pt == nullptr || TheMappedImageCollection == nullptr)
		return nullptr;
	// The retail face portraits (SCGenChallengeSelect512); the three base factions have none, so their round badge.
	static const char *const map[][2] = {
		{ "FactionAmericaAirForceGeneral", "PAAirGen_S" }, { "FactionAmericaLaserGeneral", "PALaserGen_S" },
		{ "FactionAmericaSuperWeaponGeneral", "PASuperwpnGen_S" }, { "FactionChinaInfantryGeneral", "PNInfantryGen_S" },
		{ "FactionChinaNukeGeneral", "PNNukeGen_S" }, { "FactionChinaTankGeneral", "PNTankGen_S" },
		{ "FactionGLADemolitionGeneral", "PUDemolGen_S" }, { "FactionGLAStealthGeneral", "PUStealth_S" },
		{ "FactionGLAToxinGeneral", "PUThraxGen_S" }, { "FactionBossGeneral", "PNBossGen_S" },
		{ "FactionAmerica", "USAGeneral_blue" }, { "FactionChina", "ChinaGeneral_orng" }, { "FactionGLA", "GLAGeneral_slvr" } };
	for (size_t i = 0; i < ARRAY_SIZE( map ); ++i)
		if (pt->getName().compareNoCase( map[i][0] ) == 0)
		{
			// Never findImageByName here: it mints a NameKey for a name a modded data set lacks, on spectators only.
			if (const Image *im = TheMappedImageCollection->findImageByNameNoMint( map[i][1] ))
				return im;
			break;
		}
	return pt->getEnabledImage();
}

// ---- who another player IS, as a PLAYER may know it ----------------------------------------------------------------
static Int s_apparentFor[MAX_SLOTS];				///< apparent template index + 2 the caches below hold (0 = none yet)
static const Image *s_apparentPortrait[MAX_SLOTS];
static UnicodeString s_apparentGeneral[MAX_SLOTS];

/// The general, portrait and colour of another player exactly as retail's Diplomacy screen names them for a player
/// (Diplomacy.cpp: getApparentPlayerTemplateDisplayName, TheMultiplayerSettings->getColor( getApparentColor() )): you and
/// your allies see the real ones (GameSlot::isSlotLocalAlly), an enemy who picked Random stays "Random" with the Random
/// swatch for the whole match - his units on the map keep their real colour, as always. No slot: Random, fail closed.
static void applyApparent( Row &r )
{
	const GameSlot *gs = TheGameInfo ? TheGameInfo->getConstSlot( r.slot ) : nullptr;
	if (TheMultiplayerSettings)
		if (const MultiplayerColorDefinition *cd = TheMultiplayerSettings->getColor( gs ? gs->getApparentColor() : -1 ))
			r.color = cd->getColor();
	r.textColor = brightenForText( r.color );
	const Int apparent = gs ? gs->getApparentPlayerTemplate() : PLAYERTEMPLATE_RANDOM;
	const PlayerTemplate *apt = (apparent >= 0 && ThePlayerTemplateStore) ? ThePlayerTemplateStore->getNthPlayerTemplate( apparent ) : nullptr;
	if (s_apparentFor[r.slot] != apparent + 2)
	{
		s_apparentFor[r.slot] = apparent + 2;
		s_apparentPortrait[r.slot] = apt ? portraitFor( apt ) : nullptr;
		s_apparentGeneral[r.slot] = apt ? shortArmyName( apt->getName(), apt->getDisplayName() ) : UnicodeString( L"Random" );
		if (s_apparentGeneral[r.slot].compare( L"SWG" ) == 0)
			s_apparentGeneral[r.slot] = UnicodeString( L"Superweapon" );
	}
	r.portrait = s_apparentPortrait[r.slot];
	r.general = s_apparentGeneral[r.slot];
	r.randomGeneral = apt == nullptr;
}

static Int countKinds( Player *p, KindOfType a, KindOfType b, Int clearA = -1, Int clearB = -1 )
{
	KindOfMaskType mask, clear;
	mask.set( a );
	if (b != KINDOF_INVALID)
		mask.set( b );
	if (clearA >= 0)
		clear.set( (KindOfType)clearA );
	if (clearB >= 0)
		clear.set( (KindOfType)clearB );
	return p->countObjects( mask, clear );
}

/// The same counts the retail observer panel shows (ControlBarObserver.cpp): units = scoring non-structures,
/// buildings = the three scoring structure kinds.
static void recount( Player *p, Int slot )
{
	s_units[slot] = countKinds( p, KINDOF_SCORE, KINDOF_INVALID, KINDOF_STRUCTURE );
	s_bldgs[slot] = countKinds( p, KINDOF_SCORE, KINDOF_STRUCTURE ) + countKinds( p, KINDOF_SCORE_CREATE, KINDOF_STRUCTURE )
		+ countKinds( p, KINDOF_SCORE_DESTROY, KINDOF_STRUCTURE );
}

static Player *followedPlayer()
{
	Player *p = TheControlBar ? TheControlBar->getObserverLookAtPlayer() : nullptr;
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].player == p)
			return p;
	return nullptr;
}

/// Whether the engine can look through a player's eyes at all: rts::changeObservedPlayer begins a view only with
/// PlayerObserverEnabled on and every player's ghost objects tracked (GameUtility.cpp) - else the chip offers no "their view".
static Bool canViewAs()
{
	return TheGlobalData != nullptr && TheGlobalData->m_enablePlayerObserver && TheGhostObjectManager != nullptr
		&& TheGhostObjectManager->trackAllPlayers();
}

/// The followed player's status (whether he still has eyes to look through); nobody followed reads ALIVE, which viewAfterChip
/// ignores then.
static Status followedStatus()
{
	Player *p = followedPlayer();
	for (Int i = 0; i < s_rowCount; ++i)
		if (p != nullptr && s_rows[i].player == p)
			return s_rows[i].status;
	return STATUS_ALIVE;
}

/// Dead / Left and WHEN, kept every frame before the windows paint, whether or not any panel is drawn - so a status that
/// changes while the HUD is hidden (F10, F11, the quit menu) still gets the frame it happened on.
/// All of it public: retail's Diplomacy shows Alive / Dead / Gone, and EVA announces a defeat as it happens.
static void sampleStatuses()
{
	if (ThePlayerList == nullptr)
		return;
	const Bool hasNetwork = TheNetwork != nullptr && !(TheRecorder && TheRecorder->isPlaybackMode());
	const UnsignedInt frame = TheGameLogic->getFrame();
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		Player *p = ThePlayerList->getPlayerFromSlotIndex( slot );
		if (p == nullptr || p->isPlayerObserver())
			continue;
		const GameSlot *gs = TheGameInfo ? TheGameInfo->getConstSlot( slot ) : nullptr;
		// The network never lists the local player as connected to himself, which read as "LEFT" on his own row from the
		// first frame, with no time (owner 06/10: "why does You Left doesnt say the time?"). While he is looking at this
		// screen he has not left.
		const Bool connected = (hasNetwork && p != ThePlayerList->getLocalPlayer()) ? TheNetwork->isPlayerConnected( slot ) : TRUE;
		Status st = decideStatus( hasNetwork, connected, gs != nullptr && gs->isHuman(), p->isPlayerActive() );
		// The first word sticks: a destroyed player who then leaves the game stays "Dead" with his death time.
		if (s_lastStatus[slot] != STATUS_ALIVE)
			st = s_lastStatus[slot];
		for (Int k = 0; k < s_fakeCount; ++k)
			if (s_fake[k].slot == slot && frame >= s_fake[k].frame)
				st = s_fake[k].status;
		// A status already in place at the first look (a replay opened part-way) has no time we saw: 0 = unknown, and no
		// time stamp is drawn rather than a wrong one.
		if (st != STATUS_ALIVE && s_lastStatus[slot] == STATUS_ALIVE)
			s_statusFrame[slot] = s_seeded ? frame : 0;
		s_lastStatus[slot] = st;
	}
	s_seeded = TRUE;
}

/// The player whose eyes a spectator sees through (the retail "toggle player view": his fog of war), else the one he
/// follows; nullptr = the whole map and nobody followed.
static Player *viewedPlayer()
{
	Player *o = TheControlBar ? TheControlBar->getObservedPlayer() : nullptr;
	for (Int i = 0; o != nullptr && i < s_rowCount; ++i)
		if (s_rows[i].player == o)
			return o;
	return followedPlayer();
}

static void gatherRows()
{
	s_rowCount = 0;
	if (ThePlayerList == nullptr)
		return;
	const UnsignedInt now = timeGetTime();
	const Bool recountNow = now - s_countMs >= 250;
	if (recountNow)
		s_countMs = now;
	const UnsignedInt frame = TheGameLogic->getFrame();
	// FAIRNESS: what a row may carry is decided by the viewer, here, before any panel draws. Anything other than the
	// spectator HUD counts as a player (fail closed): another player's private numbers are never even read for him.
	const Bool spectator = s_mode == MODE_SPECTATOR;
	Player *local = ThePlayerList->getLocalPlayer();

	Int teams[MAX_SLOTS];
	for (Int slot = 0; slot < MAX_SLOTS; ++slot)
	{
		Player *p = ThePlayerList->getPlayerFromSlotIndex( slot );
		if (p == nullptr || p->isPlayerObserver())
			continue;
		const GameSlot *gs = TheGameInfo ? TheGameInfo->getConstSlot( slot ) : nullptr;
		Row &r = s_rows[s_rowCount];
		clearPrivate( r );
		r.player = p;
		r.slot = slot;
		r.own = p == local;
		r.team = gs ? gs->getTeamNumber() : -1;
		{
			const GameSlot *ls = TheGameInfo ? TheGameInfo->getConstSlot( TheGameInfo->getLocalSlotNum() ) : nullptr;
			r.ally = !r.own && r.team >= 0 && ls != nullptr && ls->getTeamNumber() == r.team && liveAlly( p );
		}
		teams[s_rowCount] = r.team;
		r.status = s_lastStatus[slot];		// sampleStatuses(), every frame

		if (maySeePrivate( spectator, r.own ))
		{
			r.cash = (Int)p->getMoney()->countMoney();
			r.cpm = (Int)p->getMoney()->getCashPerMinute();
			const Energy *e = p->getEnergy();
			const Bool sabotaged = frame < e->getPowerSabotagedTillFrame();
			r.prod = e->getProduction();
			r.use = e->getConsumption();
			r.grid = r.prod > 0 || r.use > 0 || sabotaged;
			r.deficit = r.grid && (sabotaged || !e->hasSufficientPower());
			r.rank = p->getRankLevel();
			r.xp = p->getSkillPoints();
			r.xpNext = nextLevelXP( s_needed, s_levels, r.rank );
			r.kills = p->getScoreKeeper()->getTotalUnitsDestroyed();
			r.losses = p->getScoreKeeper()->getTotalUnitsLost();
			if (recountNow)
				recount( p, slot );
			r.units = s_units[slot];
			r.bldgs = s_bldgs[slot];
		}
		if (apparentIdentity( spectator, r.own ))
			applyApparent( r );
		else
		{
			r.color = p->getPlayerColor();
			r.textColor = brightenForText( r.color );
			const PlayerTemplate *pt = p->getPlayerTemplate();
			if (s_portraitFor[slot] != pt)
			{
				s_portrait[slot] = portraitFor( pt );
				s_general[slot] = pt ? shortArmyName( pt->getName(), pt->getDisplayName() ) : UnicodeString::TheEmptyString;
				// The table's abbreviation reads as jargon in a sentence ("SWG General"); the panels say it in full.
				if (s_general[slot].compare( L"SWG" ) == 0)
					s_general[slot] = UnicodeString( L"Superweapon" );
				s_portraitFor[slot] = pt;
			}
			r.portrait = s_portrait[slot];
			r.general = s_general[slot];
			r.randomGeneral = FALSE;
		}
		++s_rowCount;
	}
	s_twoTeams = isTwoTeamGame( teams, s_rowCount );
	// Rows in team blocks (teamBlocks): a player's own team first, then the lobby's order, the teamless last; slot order
	// inside a block. Two teams: team A first, as before. 2v2v2v2v2v2: six blocks of two (owner 05/10/2026).
	Int ownTeam = -1;
	for (Int i = 0; !spectator && i < s_rowCount; ++i)
		if (s_rows[i].own)
			ownTeam = s_rows[i].team;
	s_blockCount = teamBlocks( teams, s_rowCount, ownTeam, s_blocks );
	if (s_blockCount > 1)
	{
		Row sorted[MAX_SLOTS];
		Int n = 0;
		for (Int b = 0; b < s_blockCount; ++b)
			for (Int i = 0; i < s_rowCount; ++i)
				if ((s_rows[i].team < 0 ? -1 : s_rows[i].team) == s_blocks[b])
					sorted[n++] = s_rows[i];
		for (Int i = 0; i < n; ++i)
			s_rows[i] = sorted[i];
	}

	if (recountNow && spectator)
		if (Player *f = followedPlayer())
		{
			s_inf = countKinds( f, KINDOF_SCORE, KINDOF_INFANTRY );
			s_veh = countKinds( f, KINDOF_SCORE, KINDOF_VEHICLE, KINDOF_AIRCRAFT );
			s_air = countKinds( f, KINDOF_SCORE, KINDOF_AIRCRAFT );
		}
}

static Bool visible( const Row &r ) { return rowVisible( s_hidden[r.slot], r.status, s_set.hideDead ); }

static Bool inBlock( const Row &r, Int block ) { return (r.team < 0 ? -1 : r.team) == s_blocks[block]; }
static Int blockVisibleRows( Int block )
{
	Int n = 0;
	for (Int i = 0; i < s_rowCount; ++i)
		n += (inBlock( s_rows[i], block ) && visible( s_rows[i] )) ? 1 : 0;
	return n;
}

/// A block's header: YOUR TEAM / ENEMY TEAM for a player in a two-team game, else the lobby's team number; the teamless
/// block is FREE FOR ALL when nobody has a team.
static DisplayString *blockTitle( Int block, Int ownTeam )
{
	const Int t = s_blocks[block];
	if (t < 0)
		return s_blockCount == 1 ? fixed( "blk.ffa", 12, TRUE, L"FREE FOR ALL" ) : fixed( "blk.none", 12, TRUE, L"NO TEAM" );
	if (ownTeam >= 0 && t == ownTeam)
		return fixed( "blk.own", 12, TRUE, L"YOUR TEAM" );
	if (ownTeam >= 0 && s_twoTeams)
		return fixed( "blk.enemy", 12, TRUE, L"ENEMY TEAM" );
	return txt( "blk.team", block, 12, TRUE, t, [t]() { UnicodeString u; u.format( L"TEAM %d", t + 1 ); return u; } );
}

/// What a card shows: the spectator's what-shows switches are his; while playing, the bar always shows its defaults.
static Bool shown( Int item ) { return s_mode == MODE_PLAYER ? showDefault( item ) : s_set.show[item]; }

static const Row *rowForFocus()
{
	Player *f = followedPlayer();
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].player == f)
			return &s_rows[i];
	return nullptr;		// whole map = nobody focused: the panel says so, it never keeps (or invents) a player's card
}

/// onSetReplayCamera locks the spectator's camera input until the next logic frame each time it plays a recorded camera. A
/// paused replay never reaches that frame, so after leaving the follow (or switching the row off) wheel zoom and keyboard
/// scrolling stayed dead until playback resumed. The lock only guards the frame a recorded camera was
/// applied in; a follow that changed, or a camera the HUD itself moved (minimap, a feed row's jump), has nothing left for it
/// to guard - and the lock is also what the chip's THEIR CAMERA reads (followsTheirCamera). Client view
/// state only.
static void releaseReplayCameraLock()
{
	if (TheTacticalView != nullptr)
		TheTacticalView->lockUserControlUntilFrame( 0 );
}

/// The follow changed by a path that is not ours - the retail observer bar behind F10/F11, its hotkeys: in a PAUSED replay the
/// recorded camera lock of the player followed before is released too. Only when no logic step ran
/// since the last look: a step may have just applied the NEW player's camera and its fresh lock, which must stay,
/// and a running replay drops a stale lock on its own at the next step. Every frame, overlay hidden or not.
static Player *s_lockLookAt = nullptr;
static UnsignedInt s_lockFrame = ~0u;
static void releaseLockOnForeignFollow()
{
	Player *now = TheControlBar != nullptr ? TheControlBar->getObserverLookAtPlayer() : nullptr;
	const UnsignedInt frame = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0u;
	if (now != s_lockLookAt && frame == s_lockFrame)
		releaseReplayCameraLock();
	s_lockLookAt = now;
	s_lockFrame = frame;
}

/// Follow = the panel's focus (the look-at player) only. rts::changeObservedPlayer would also put the spectator into that
/// player's fog of war, as the retail observer buttons do; a caster wants the whole map, so that stays one keypress away
/// on the retail "toggle player view" hotkey (MSG_META_TOGGLE_PLAYER_OBSERVER), which switches to this same player.
static void follow( Int slot )
{
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].slot == slot && TheControlBar)
		{
			// Already seeing through someone's eyes (that hotkey): move the view along with the focus, as retail's
			// own buttons do, rather than leave focus and fog on two different players.
			if (TheControlBar->getObservedPlayer() != nullptr)
				rts::changeObservedPlayer( s_rows[i].player );
			else
				TheControlBar->setObserverLookAtPlayer( s_rows[i].player );
			releaseReplayCameraLock();		// the follow keys too
		}
}

/// The spectator's current local view, read back from the engine: through whose eyes (the retail observed player) and whose
/// panel (the look-at player), as game slots; -1 = none.
static ViewState currentView()
{
	ViewState v = VIEW_FREE;
	Player *o = TheControlBar ? TheControlBar->getObservedPlayer() : nullptr;
	Player *f = followedPlayer();
	for (Int i = 0; i < s_rowCount; ++i)
	{
		if (o != nullptr && s_rows[i].player == o)
			v.view = s_rows[i].slot;
		if (f != nullptr && s_rows[i].player == f)
			v.follow = s_rows[i].slot;
	}
	return v;
}

/// Put the engine into a view: through t.view's eyes (his fog of war and audio: rts::changeObservedPlayer, the retail
/// observer buttons' own call, so no second path) or the whole map, and the follow panel on t.follow. CLIENT view state only:
/// it moves the observer's drawn shroud, ghosts, radar and which player's sounds play, never the simulation's shroud grid.
/// changeObservedPlayer deselects: call it from a message translator, or from draw only with an empty selection.
static void applyView( const ViewState &t )
{
	if (TheControlBar == nullptr)
		return;
	Player *vp = nullptr, *fp = nullptr;
	for (Int i = 0; i < s_rowCount; ++i)
	{
		if (s_rows[i].slot == t.view)
			vp = s_rows[i].player;
		if (s_rows[i].slot == t.follow)
			fp = s_rows[i].player;
	}
	if (vp != nullptr)
		rts::changeObservedPlayer( vp );
	else if (TheControlBar->getObservedPlayer() != nullptr)
		rts::changeObservedPlayer( nullptr );		// back to the whole map (it also clears the look-at player)
	TheControlBar->setObserverLookAtPlayer( fp );
	releaseReplayCameraLock();
}

static Bool isReplay() { return TheRecorder != nullptr && TheRecorder->isPlaybackMode(); }

/// "Follow the player's camera" (owner 14/10/2026: "i want us replaying exactly what the player was ... or maybe we can have
/// that enabled/disabled"): retail's own "Use camera in replays" - TheGlobalData->m_useCameraInReplay, Options.ini
/// UseCameraInReplays, default on - so the Options menu's box and this row are one setting. Following a player in a replay
/// plays back his recorded camera while it is on (GameLogic::onSetReplayCamera); off, the camera stays the spectator's.
static Bool followCameraOn() { return TheGlobalData != nullptr && TheGlobalData->m_useCameraInReplay; }
static void setFollowCamera( Bool on )
{
	if (TheWritableGlobalData == nullptr || followCameraOn() == on)
		return;
	TheWritableGlobalData->m_useCameraInReplay = on;
	if (!on)
		releaseReplayCameraLock();
	OptionPreferences pref;		// as OptionsMenu's Accept writes it (saveOptions)
	pref["UseCameraInReplays"] = on ? "yes" : "no";
	pref.write();
	fprintf( stderr, "[GXSPEC] followcam=%d f=%u\n", on ? 1 : 0, TheGameLogic ? TheGameLogic->getFrame() : 0u );
}

/// The followed player's recorded camera is what the spectator sees (the chip says THEIR CAMERA): see followsTheirCamera.
static Bool followsTheirCameraNow( Bool following )
{
	return followsTheirCamera( isReplay(), followCameraOn(), following, TheTacticalView != nullptr && TheTacticalView->isUserControlLocked() );
}

/// A click on a player's card, board row or stars (GXSpectatorHud.h viewAfterClick).
static void clickPlayer( Int slot )
{
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].slot == slot)
		{
			applyView( viewAfterClick( currentView(), slot, s_rows[i].status ) );
			return;
		}
}

/// GX_SPECHUD_RETAIL_CLICKS=1 - a TEST fail arm: a spectator's battlefield clicks go the retail way (meta map, selection
/// translator) and the order gate lets every selection order out, i.e. the behaviour before 12/10/2026. With
/// GX_SPECHUD_NETPROBE=1 its run must show the selection orders the default arm does not.
static Bool retailClicks()
{
	static const Bool on = getenv( "GX_SPECHUD_RETAIL_CLICKS" ) != nullptr;
	return on;
}

/// A modal window (a scripted popup) is up: the battlefield behind it inspects nothing (spectator-click).
static Bool modalUp() { return TheWindowManager != nullptr && TheWindowManager->winGetModal() != nullptr; }

/// What a spectator's click shows of an object, by retail's observer-portrait rules (ControlBar::setPortraitByObject): a
/// disguised enemy is its disguise and the player it pretends to be. *tt = the template to show; returns the owner to show.
static Player *apparentOwner( Object *o, const ThingTemplate **tt )
{
	*tt = o->getTemplate();
	Player *owner = o->getControllingPlayer();
	Drawable *dr = o->getDrawable();
	if (dr != nullptr && dr->getStealthLook() == STEALTHLOOK_DISGUISED_ENEMY)
	{
		*tt = dr->getTemplate();
		StealthUpdate *stealth = o->getStealth();
		if (stealth != nullptr && stealth->isDisguised())
			owner = ThePlayerList->getNthPlayer( stealth->getDisguisedPlayerIndex() );
	}
	return owner;
}

static ObjectID s_inspectID = INVALID_ID;	///< what a spectator's battlefield click shows (inspectAt); looked up by id every frame

/// A spectator's click on the battlefield (owner 12/10/2026: "click any unit and it shows me the building and upgrades"): the
/// unit or building under it goes on the FOCUS panel and its owner is followed (viewAfterObjectClick; in a replay the follow
/// stays, since there it is also whose recorded camera plays - owner 14/10/2026); empty ground clears the
/// card and keeps the focus. It picks what retail's point click picks (SelectionTranslator: pickDrawable + CanSelectDrawable)
/// and only REMEMBERS it: the client selection is never touched, because retail and simulation code read it (the observer
/// bar's portrait mints NameKeys for its cameos, a solo mission's "named unit selected" condition, a selection carried over
/// to a replacement object as MSG_CREATE_SELECTED_GROUP_NO_SOUND - spectator-click /P2).
static void inspectAt( Int x, Int y )
{
	if (TheTacticalView == nullptr || TheGameLogic == nullptr)
		return;
	ICoord2D at;
	at.x = x;
	at.y = y;
	const Drawable *picked = TheTacticalView->pickDrawable( &at, FALSE, PICK_TYPE_SELECTABLE );
	s_inspectID = picked != nullptr && CanSelectDrawable( picked, FALSE ) ? picked->getObject()->getID() : INVALID_ID;
	if (Object *o = TheGameLogic->findObjectByID( s_inspectID ))
	{
		const ThingTemplate *shown = nullptr;
		const Player *owner = apparentOwner( o, &shown );		// the card's owner (drawObjectBand)
		for (Int i = 0; i < s_rowCount; ++i)
			if (s_rows[i].player == owner)
			{
				const ViewState cur = currentView(), next = viewAfterObjectClick( cur, s_rows[i].slot, s_rows[i].status, isReplay() );
				if (!(next == cur))
					applyView( next );
				break;
			}
	}
	fprintf( stderr, "[GXSPEC] inspect at=%d,%d obj=%u follow=%d view=%d f=%u\n", x, y, (unsigned)s_inspectID, currentView().follow,
		currentView().view, TheGameLogic->getFrame() );
}

// ---- what a general bought: only while the mouse is on his stars ---------------------------------------------------
static Int s_buySlot = -1;					///< whose names s_buyNames holds
static UnsignedInt s_buyMs[MAX_SLOTS];
static UnicodeString s_buyNames[24];
static Int s_buyCount = 0;
static Int s_buySpent[MAX_SLOTS], s_buyTotal[MAX_SLOTS];

/// Points spent / earned for any player (the follow panel), and the bought names for the hovered one; at most twice a
/// second per player, so the panel and the hover card showing two different generals do not thrash one cache.
static void refreshBought( const Row &r, Bool wantNames )
{
	const UnsignedInt now = timeGetTime();
	if (now - s_buyMs[r.slot] < 500 && (!wantNames || s_buySlot == r.slot))
		return;
	s_buyMs[r.slot] = now;
	if (wantNames)
	{
		s_buySlot = r.slot;
		s_buyCount = 0;
	}
	Int spent = 0;
	const PlayerTemplate *pt = r.player->getPlayerTemplate();
	if (pt == nullptr || TheControlBar == nullptr || TheScienceStore == nullptr)
		return;
	// The general's promotion screen's own three command sets (ControlBar::populatePurchaseScience); a science
	// counts when it is owned AND costs points - rank-granted freebies are not purchases.
	const AsciiString sets[3] = { pt->getPurchaseScienceCommandSetRank1(), pt->getPurchaseScienceCommandSetRank3(),
		pt->getPurchaseScienceCommandSetRank8() };
	ScienceType seen[64];
	Int seenCount = 0;
	for (Int s = 0; s < 3; ++s)
	{
		const CommandSet *cs = sets[s].isEmpty() ? nullptr : TheControlBar->findCommandSet( sets[s] );
		for (Int i = 0; cs != nullptr && i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton *b = cs->getCommandButton( i );
			if (b == nullptr || b->getCommandType() != GUI_COMMAND_PURCHASE_SCIENCE)
				continue;
			const ScienceVec &v = b->getScienceVec();
			for (size_t k = 0; k < v.size(); ++k)
			{
				const ScienceType st = v[k];
				const Int cost = TheScienceStore->getSciencePurchaseCost( st );
				if (cost <= 0 || !r.player->hasScience( st ) || r.player->isScienceHidden( st ))
					continue;
				Bool dup = FALSE;
				for (Int d = 0; d < seenCount; ++d)
					dup = dup || seen[d] == st;
				if (dup || seenCount >= (Int)ARRAY_SIZE( seen ))
					continue;
				seen[seenCount++] = st;
				spent += cost;
				UnicodeString name, desc;
				if (wantNames && s_buyCount < (Int)ARRAY_SIZE( s_buyNames ) && TheScienceStore->getNameAndDescription( st, name, desc ))
					s_buyNames[s_buyCount++] = name;
			}
		}
	}
	s_buySpent[r.slot] = spent;
	s_buyTotal[r.slot] = spent + r.player->getSciencePurchasePoints();
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout: home rects (1080p design units scaled by s_scale), moved by the dock offsets and kept on screen.
// ---------------------------------------------------------------------------------------------------------------------
static Rect placed( const Rect &home, Int panel )
{
	Dock::Panel &p = s_dock.panel[panel];
	Rect r = { home.x + p.dx, home.y + p.dy, home.w, home.h };
	const Int minVis = S( 40 );
	if (r.x > s_W - minVis) { p.dx -= r.x - (s_W - minVis); r.x = s_W - minVis; }
	if (r.x + r.w < minVis) { p.dx += minVis - (r.x + r.w); r.x = minVis - r.w; }
	if (r.y > s_H - minVis) { p.dy -= r.y - (s_H - minVis); r.y = s_H - minVis; }
	if (r.y < 0) { p.dy -= r.y; r.y = 0; }
	return r;
}

/// A dock tab: drawn on top of everything, follows its panel, arrow pointing the way the panel will go.
static void dockTab( const char *id, const wchar_t *text, Int x, Int y, Int panel, Bool arrowUp )
{
	DisplayString *ds = fixed( id, 10, TRUE, text );
	const Int w = tw( ds ) + S( 44 ), h = S( 20 );
	Rect r = { x, y, w, h };
	plate( r, TRUE, TRUE );
	const Bool pressed = s_dock.pressed == panel && s_dock.pressedTab;
	if (pressed)
		fill( r, GameMakeColor( 111, 168, 240, 110 ) );
	else if (mouseIn( r ))
		fill( r, GameMakeColor( 111, 168, 240, 50 ) );
	putC( ds, x + w / 2, y + (h - th( 10 )) / 2, white() );
	arrow( x + S( 12 ), y + h / 2, S( 4 ), arrowUp, white() );
	arrow( x + w - S( 12 ), y + h / 2, S( 4 ), arrowUp, white() );
	addHit( r, H_TAB, 0, panel );
}

/// A tab hung by its CENTRE: the PLAYERS tab sits centred under the clock block, folded or open, so it never jumps sideways
/// when it folds (owner 06/10: "move this here").
static void dockTabCentred( const char *id, const wchar_t *text, Int cx, Int y, Int panel, Bool arrowUp )
{
	DisplayString *ds = fixed( id, 10, TRUE, text );
	dockTab( id, text, dockTabRectCentred( cx, y, tw( ds ), s_scale ).x, y, panel, arrowUp );
}

/// The PLAYERS tab's text width: the same string dockTab draws, so the toast stack avoids the rect the tab is drawn in.
static Int playersTabTextW() { return tw( fixed( "tab.players", 10, TRUE, L"PLAYERS" ) ); }

// ---------------------------------------------------------------------------------------------------------------------
// C2: the bar of player cards with the clock in the middle
// ---------------------------------------------------------------------------------------------------------------------
static void drawCard( const Row &r, Int x, Int y, Int w, Int h, Bool followed )
{
	const Bool alive = r.status == STATUS_ALIVE;
	Rect c = { x, y, w, h };
	fill( c, navyDeep( textAlpha() ) );
	box( c, followed ? accent() : steelDim() );
	fill( x, y, S( 4 ), h, r.color );
	Int tx = x + S( 10 );
	const Bool player = s_mode == MODE_PLAYER;
	Bool showPortrait = shown( SHOW_PORTRAITS ) && r.portrait;
	// A narrow card (a 4:3 screen, many players) while playing: my own keeps its numbers and drops the picture; everyone
	// else is his portrait (or "?" for a Random enemy) with the status tag on it - a clipped "Har" says nothing. The names
	// are on the Tab board.
	const Bool narrow = player && w - S( 16 ) - (showPortrait ? h - S( 18 ) : 0) < S( 36 );
	if (narrow && r.own)
		showPortrait = FALSE;
	if (narrow && !r.own)
	{
		const Int pz = min( w - S( 10 ), h - S( 10 ) );
		if (r.portrait)
			TheDisplay->drawImage( r.portrait, x + S( 6 ), y + (h - pz) / 2, x + S( 6 ) + pz, y + (h - pz) / 2 + pz, 0xFFFFFFFF,
				alive ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
		else
			putC( fixed( "card.q", 24, TRUE, L"?" ), x + S( 6 ) + pz / 2, y + (h - th( 24 )) / 2, label() );
		if (!alive)
		{
			const Bool left = r.status == STATUS_LEFT;
			DisplayString *tag = left ? fixed( "tag.left", 13, TRUE, L"LEFT" ) : fixed( "tag.dead", 13, TRUE, L"DEAD" );
			const Rect t = { x + S( 6 ), y + h - S( 5 ) - th( 13 ) - S( 3 ), tw( tag ) + S( 12 ), th( 13 ) + S( 3 ) };
			fill( t, left ? GameMakeColor( 120, 86, 16, 230 ) : GameMakeColor( 120, 24, 24, 230 ) );
			box( t, left ? amber() : red() );
			put( tag, t.x + S( 6 ), t.y + S( 1 ), left ? amber() : red() );
		}
		return;
	}
	// The NAME takes the whole top row, so a long one fits; the portrait sits UNDER it with the numbers beside it (owner
	// 06/10: "move the picture of the faction a bit down to make the player name snap to the left").
	const Bool names = shown( SHOW_NAMES );
	const Int nameRow = names ? th( 13 ) + S( 1 ) : 0;
	Int ty = y + S( 5 );
	if (showPortrait)
	{
		const Int pz = names ? h - S( 5 ) - nameRow - S( 5 ) : h - S( 18 );
		const Int py = names ? ty + nameRow : y + (h - pz) / 2;
		TheDisplay->drawImage( r.portrait, x + S( 6 ), py, x + S( 6 ) + pz, py + pz, 0xFFFFFFFF,
			alive ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
		tx = x + S( 6 ) + pz + S( 6 );
	}
	IRegion2D clip;
	clip.lo.x = tx;
	clip.lo.y = y;
	clip.hi.x = x + w - S( 3 );
	clip.hi.y = y + h;
	if (names)
	{
		Player *pl = r.player;
		const Int nx = showPortrait ? x + S( 10 ) : tx;
		const Int maxW = x + w - S( 4 ) - nx;
		const Int slot = r.slot;
		DisplayString *n = txt( "card.name", slot, 13, TRUE, maxW, [pl, maxW, slot]() { Bool cut = FALSE; UnicodeString u = ellipsize( hudName( pl ), 13, TRUE, maxW, &cut ); s_nameCut[slot] = cut; return u; } );
		if (n)
			put( n, nx, ty, alive ? r.textColor : dim() );
		ty += nameRow;
	}
	if (!alive)
	{
		const Bool left = r.status == STATUS_LEFT;
		DisplayString *tag = left ? fixed( "tag.left", 13, TRUE, L"LEFT" ) : fixed( "tag.dead", 13, TRUE, L"DEAD" );
		const Rect t = { tx, ty + S( 2 ), tw( tag ) + S( 12 ), th( 13 ) + S( 3 ) };
		fill( t, left ? GameMakeColor( 120, 86, 16, 230 ) : GameMakeColor( 120, 24, 24, 230 ) );
		box( t, left ? amber() : red() );
		put( tag, tx + S( 6 ), ty + S( 3 ), left ? amber() : red() );
		ty += t.h + S( 5 );
		if (shown( SHOW_STATUS_TIME ) && s_statusFrame[r.slot] != 0)
		{
			const UnsignedInt f = s_statusFrame[r.slot];
			put( txt( "card.time", r.slot, 11, FALSE, (Int)f, [f]() { return clockText( f ); } ), tx, ty, dim() );
		}
		return;
	}
	if (player && !r.own)
	{
		// Who, and which general - what the lobby and retail's Diplomacy already told a player. The numbers below would be
		// zeros anyway (gatherRows never filled them); this is the look, not the guard.
		const UnicodeString gen = r.general;
		DisplayString *g = txt( "card.gen", r.slot, 12, FALSE, 0, [gen]() { return gen; } );
		if (g)
			g->setClipRegion( &clip );
		put( g, tx, ty, label() );
		return;
	}
	if (shown( SHOW_CASH ))
	{
		put( number( "card.cash", r.slot, 13, TRUE, r.cash, FALSE, TRUE ), tx, ty, white() );
		ty += th( 13 ) + S( 1 );
	}
	if (shown( SHOW_POWER ))
	{
		if (!r.grid)
			put( fixed( "card.nogrid", 12, TRUE, L"0 / 0" ), tx, ty, dim() );
		else
		{
			const Int u = r.use, p = r.prod;		// USED / PRODUCED everywhere (owner 06/10: "4 used out of 20")
			put( txt( "card.pw", r.slot, 12, TRUE, ((long long)u << 32) | (UnsignedInt)p, [u, p]() { UnicodeString s; s.format( L"%d / %d", u, p ); return s; } ),
				tx, ty, r.deficit ? red() : white() );
		}
		ty += th( 12 ) + S( 1 );
	}
	// A PLAYER's own card: income only in sandbox play, never units/buildings (owner 06/10). Spectators keep both.
	if (shown( SHOW_INCOME ) && (s_mode != MODE_PLAYER || sandboxMatch()))
	{
		put( number( "card.cpm", r.slot, 11, FALSE, r.cpm, TRUE ), tx, ty, label() );
		ty += th( 11 ) + S( 1 );
	}
	if (shown( SHOW_UNITS_BUILDINGS ) && s_mode != MODE_PLAYER)
	{
		const Int u = r.units, b = r.bldgs;
		put( txt( "card.ub", r.slot, 11, FALSE, (u << 16) ^ b, [u, b]() { UnicodeString s; s.format( L"%d u  %d b", u, b ); return s; } ),
			tx, ty, label() );
	}
}

static Rect s_barHome = { 0, 0, 0, 0 };
static Int s_barFit = 0, s_barPageW = 0;				///< barHomeRect's paging: card places per half, each end's page buttons (0 = no paging)
static Int s_barFirst[2] = { 0, 0 };					///< the first card each half shows while it pages (barWindow clamps it)
static Int s_barPageMax[2] = { -1, -1 };				///< the furthest that half can start, -1 = it does not page
static Rect s_barSide[2] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };	///< the two halves as drawn this frame, w 0 = not drawn (the wheel's target)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
static const Int kListMaxRows = 10;		// the iPad's smaller screen
#else
static const Int kListMaxRows = 14;
#endif
static Int s_listFirst = 0, s_listRows = 0, s_listShown = 0;	///< the open player list: first row shown, its rows in all, rows that fit
static Rect s_listBox = { 0, 0, 0, 0 };					///< that list's board this frame, w 0 = none (the wheel's target)
static Rect s_listTrack = { 0, 0, 0, 0 };				///< its scroll track this frame, w 0 = no scroll bar
static Bool s_listDrag = FALSE;							///< the left button went down on the track and has not come up
static UnsignedInt s_listDrawnMs = 0;					///< a list not drawn for a moment opens at its top again (or at my own row)
static Real s_wheelRest = 0.0f;							///< a touchpad's fractions of a wheel notch, kept until they make a whole one

static void barHome( Int leftCount, Int rightCount, Int &cw, Int &cenW )
{
	const BarHome b = barHomeRect( leftCount, rightCount, shown( SHOW_CLOCK_TOTALS ), s_W, s_scale );		// model-tested geometry
	cw = b.cw;
	cenW = b.cenW;
	s_barHome = b.r;
	s_barFit = b.fit;
	s_barPageW = b.pageW;
}

/// One half's page buttons, stacked at its outer end. Greyed at the end of the list; a half that does not page has none.
static void drawBarPager( Int side, Int x, Int y, Int h )
{
	if (s_barPageMax[side] < 0)
		return;
	const Int w = s_barPageW - S( 4 ), bh = (h - S( 2 )) / 2;
	for (Int dir = 0; dir < 2; ++dir)		// 0 back, 1 on
	{
		const Rect r = { x, y + dir * (bh + S( 2 )), w, bh };
		const Bool can = dir == 0 ? s_barFirst[side] > 0 : s_barFirst[side] < s_barPageMax[side];
		fill( r, can && mouseIn( r ) ? GameMakeColor( 111, 168, 240, 70 ) : navyDeep( 200 ) );
		box( r, can ? steel() : steelDim() );
		arrowSide( r.x + r.w / 2, r.y + r.h / 2, S( 5 ), dir == 0, can ? white() : steelDim() );
		if (can)
			addHit( r, H_BARPAGE, side * 2 + dir, -1 );
	}
}

/// Each frame a player list draws: how many rows fit, and where it opens. `openAt` is the row that must be in view when it opens.
static void listPrepare( Int rows, Int rowH, Int roomPx, Int openAt )
{
	const UnsignedInt now = timeGetTime();
	s_listRows = rows;
	s_listShown = listRowsShown( rows, kListMaxRows, roomPx, rowH );
	if (now - s_listDrawnMs > 400)
		s_listFirst = openAt < s_listShown ? 0 : openAt - s_listShown / 2;
	s_listDrawnMs = now;
	s_listFirst = listClampFirst( s_listFirst, s_listRows, s_listShown );
}

static void listScrollBy( Int rows ) { s_listFirst = listClampFirst( s_listFirst + rows, s_listRows, s_listShown ); }

static void listScrollToY( Int y )
{
	if (s_listDrag && s_listTrack.w > 0)
		s_listFirst = listFirstAtY( y - s_listTrack.y, s_listTrack.h, s_listRows, s_listShown, S( 24 ) );
}

/// A press on the scroll bar: an end button moves one row, the track puts the thumb under the cursor and drags from there.
static void listScrollPress( const Hit &h, Int y )
{
	if (h.id == 0)
	{
		s_listDrag = TRUE;
		listScrollToY( y );
	}
	else
		listScrollBy( h.id == 1 ? -1 : 1 );
}

/// The list's scroll bar: x is its left edge, top and bottom the list's own.
static void drawListScroll( Int x, Int top, Int bottom )
{
	const Int w = S( 10 ), ah = S( 18 );
	const Rect up = { x, top, w, ah }, down = { x, bottom - ah, w, ah }, track = { x, top + ah, w, bottom - top - ah * 2 };
	s_listTrack = track;
	const ListThumb t = listThumb( track.h, s_listRows, s_listShown, s_listFirst, S( 24 ) );
	const Rect knob = { track.x + 1, track.y + t.y, track.w - 2, t.h };
	fill( track, navyDeep( 200 ) );
	box( track, steelDim() );
	fill( knob, s_listDrag || mouseIn( track ) ? accent() : steel() );
	const Bool canUp = s_listFirst > 0, canDown = s_listFirst < s_listRows - s_listShown;
	box( up, canUp ? steel() : steelDim() );
	box( down, canDown ? steel() : steelDim() );
	arrow( up.x + up.w / 2, up.y + up.h / 2, S( 4 ), TRUE, canUp ? white() : steelDim() );
	arrow( down.x + down.w / 2, down.y + down.h / 2, S( 4 ), FALSE, canDown ? white() : steelDim() );
	addHit( track, H_SCROLL, 0, -1 );
	addHit( up, H_SCROLL, 1, -1 );
	addHit( down, H_SCROLL, 2, -1 );
}

static void drawBar( Rect &out )
{
	const Row *left[MAX_SLOTS], *right[MAX_SLOTS];
	Int nl = 0, nr = 0, alive = 0, total = 0;
	const Bool player = s_mode == MODE_PLAYER;
	Int teamA = s_rowCount > 0 ? s_rows[0].team : -1;
	for (Int i = 0; player && i < s_rowCount; ++i)
		if (s_rows[i].own)
			teamA = s_rows[i].team;		// a player's own team is always the left half
	Int visibleCount = 0;
	for (Int i = 0; i < s_rowCount; ++i)
		if (visible( s_rows[i] ))
			++visibleCount;
	for (Int i = 0; i < s_rowCount; ++i)
	{
		const Row &r = s_rows[i];
		++total;
		alive += r.status == STATUS_ALIVE ? 1 : 0;
		if (!visible( r ))
			continue;
		if (s_twoTeams ? r.team == teamA : (player ? r.own || nl < visibleCount / 2 : nl < (visibleCount + 1) / 2))
			left[nl++] = &r;
		else
			right[nr++] = &r;
	}
	// A player's own card sits next to the clock (the half of the bar the eye returns to), his team beyond it.
	for (Int i = 0; player && i < nl - 1; ++i)
		if (left[i]->own)
			std::swap( left[i], left[nl - 1] );
	Int cw = 0, cenW = 0;
	barHome( nl, nr, cw, cenW );
	s_barPageMax[0] = s_barPageMax[1] = -1;
	if (s_barPageW > 0)
	{
		// More cards than places: each half keeps a window on its own list, a player's own card pinned beside the clock.
		const Bool pinLeft = player && nl > 0 && left[nl - 1]->own;
		const Row **lists[2] = { left, right };
		Int *counts[2] = { &nl, &nr };
		for (Int sd = 0; sd < 2; ++sd)
		{
			const Bool pin = sd == 0 && pinLeft;
			if (*counts[sd] > s_barFit)
				s_barPageMax[sd] = barMaxFirst( *counts[sd], s_barFit, pin );
			Int idx[MAX_SLOTS];
			const Row *kept[MAX_SLOTS];
			const Int n = barWindow( *counts[sd], s_barFit, pin, s_barFirst[sd], idx );
			for (Int j = 0; j < n; ++j)
				kept[j] = lists[sd][idx[j]];
			for (Int j = 0; j < n; ++j)
				lists[sd][j] = kept[j];
			*counts[sd] = n;
		}
	}
	const Rect bar = placed( s_barHome, PANEL_PLAYERS );
	out = bar;
	if (!s_dock.panel[PANEL_PLAYERS].open)
		return;
	const Int side = (bar.w - cenW) / 2;
	const Rect lr = { bar.x, bar.y, side, bar.h }, rr = { bar.x + side + cenW, bar.y, side, bar.h };
	plate( lr );
	plate( rr );
	addHit( bar, H_BODY, 0, PANEL_PLAYERS );
	if (s_barPageW > 0)
	{
		s_barSide[0] = lr;
		s_barSide[1] = rr;
		drawBarPager( 0, lr.x + S( 3 ), bar.y + S( 5 ), bar.h - S( 10 ) );
		drawBarPager( 1, rr.x + rr.w - s_barPageW + S( 1 ), bar.y + S( 5 ), bar.h - S( 10 ) );
	}
	Player *f = viewedPlayer();
	const Int ch = bar.h - S( 10 );
	for (Int i = 0; i < nl; ++i)
	{
		// Team A runs toward the clock, so the two teams meet in the middle.
		const Int x = lr.x + lr.w - S( 4 ) - (nl - i) * cw;
		drawCard( *left[i], x, bar.y + S( 5 ), cw - S( 4 ), ch, player ? left[i]->own : left[i]->player == f );
		const Rect hr = { x, bar.y + S( 5 ), cw - S( 4 ), ch };
		addHit( hr, H_CARD, left[i]->slot, PANEL_PLAYERS );
	}
	for (Int i = 0; i < nr; ++i)
	{
		const Int x = rr.x + S( 4 ) + i * cw;
		drawCard( *right[i], x, bar.y + S( 5 ), cw - S( 4 ), ch, player ? right[i]->own : right[i]->player == f );
		const Rect hr = { x, bar.y + S( 5 ), cw - S( 4 ), ch };
		addHit( hr, H_CARD, right[i]->slot, PANEL_PLAYERS );
	}
	// A name cut to fit its card ("Hard Ar...") is shown whole on a tip under the card while the mouse is on it.
	if (TheMouse != nullptr && shown( SHOW_NAMES ))
	{
		const MouseIO *mio = TheMouse->getMouseStatus();
		const Row *const *sides[2] = { left, right };
		const Int counts[2] = { nl, nr };
		for (Int sd = 0; mio != nullptr && sd < 2; ++sd)
			for (Int i = 0; i < counts[sd]; ++i)
			{
				const Row &r = *sides[sd][i];
				const Int x = sd == 0 ? lr.x + lr.w - S( 4 ) - (nl - i) * cw : rr.x + S( 4 ) + i * cw;
				const Rect hr = { x, bar.y + S( 5 ), cw - S( 4 ), ch };
				if (!s_nameCut[r.slot] || !hr.contains( mio->pos.x, mio->pos.y ) || r.player == nullptr)
					continue;
				const UnicodeString full = hudName( r.player );
				DisplayString *tip = txt( "card.tip", r.slot, 13, TRUE, 1, [full]() { return full; } );
				if (tip == nullptr)
					continue;
				const Rect t = { max( S( 4 ), min( x, s_W - tw( tip ) - S( 24 ) ) ), bar.y + bar.h + S( 4 ), tw( tip ) + S( 20 ), th( 13 ) + S( 12 ) };
				plate( t, FALSE, TRUE );
				put( tip, t.x + S( 10 ), t.y + S( 6 ), r.textColor );
			}
	}
	if (!shown( SHOW_CLOCK_TOTALS ))
		return;

	// The middle: clock + team totals, or (no two teams) clock + how many are still alive.
	const Rect mid = { bar.x + side, bar.y, cenW, bar.h };
	plate( mid, FALSE, TRUE );
	const UnsignedInt frame = TheGameLogic->getFrame();
	const UnsignedInt secs = frame / LOGICFRAMES_PER_SECOND;
	DisplayString *clock = txt( "bar.clock", 0, 30, TRUE, (Int)secs, [frame]() { return clockText( frame ); } );
	const Int barY = mid.y + mid.h - S( 14 );
	const Int barX = mid.x + S( 14 ), barW = mid.w - S( 28 );
	if (player)
	{
		// A player's centre: the clock and who is still standing - retail's Diplomacy tells a player that much. No team
		// cash and no cash bar: other players' money is exactly what a player must not see.
		Int aA = 0, nA = 0, aB = 0, nB = 0;
		for (Int i = 0; i < s_rowCount; ++i)
		{
			const Bool mine = teamA >= 0 ? s_rows[i].team == teamA : s_rows[i].own;
			(mine ? nA : nB) += 1;
			(mine ? aA : aB) += s_rows[i].status == STATUS_ALIVE ? 1 : 0;
		}
		const Int cy = mid.y + S( 14 );
		putC( clock, mid.x + mid.w / 2, cy, white() );
		const Int cx0 = mid.x + mid.w / 2 - tw( clock ) / 2 - S( 12 ), cx1 = mid.x + mid.w / 2 + tw( clock ) / 2 + S( 12 );
		putR( txt( "bar.pa", 0, 20, TRUE, (aA << 8) | nA, [aA, nA]() { UnicodeString u; u.format( L"%d/%d", aA, nA ); return u; } ),
			cx0, cy, white() );
		putR( fixed( teamA >= 0 ? "bar.pteam" : "bar.pyou", 10, TRUE, teamA >= 0 ? L"YOUR TEAM" : L"YOU" ), cx0, cy + th( 20 ), label() );
		put( txt( "bar.pb", 0, 20, TRUE, (aB << 8) | nB, [aB, nB]() { UnicodeString u; u.format( L"%d/%d", aB, nB ); return u; } ),
			cx1, cy, white() );
		put( fixed( s_twoTeams ? "bar.pfoe" : "bar.pothers", 10, TRUE, s_twoTeams ? L"ENEMY" : L"OTHERS" ), cx1, cy + th( 20 ), label() );
		putC( fixed( "bar.pstand", 10, TRUE, L"STILL STANDING" ), mid.x + mid.w / 2, barY - S( 4 ), dim() );
	}
	else if (s_twoTeams)
	{
		Int c1 = 0, c2 = 0;
		for (Int i = 0; i < s_rowCount; ++i)
			(s_rows[i].team == teamA ? c1 : c2) += s_rows[i].cash;
		const Int cy = mid.y + S( 14 );
		putC( clock, mid.x + mid.w / 2, cy, white() );
		// wh1ter0se 03/11 "add $ sign": the totals read like the cards' cash ("$672"), $313,480
		DisplayString *a = number( "bar.t1", 0, 20, TRUE, c1, FALSE, TRUE ), *b = number( "bar.t2", 0, 20, TRUE, c2, FALSE, TRUE );
		const Int cx0 = mid.x + mid.w / 2 - tw( clock ) / 2 - S( 12 ), cx1 = mid.x + mid.w / 2 + tw( clock ) / 2 + S( 12 );
		putR( a, cx0, cy, white() );
		putR( fixed( "bar.team1", 10, TRUE, L"TEAM 1" ), cx0, cy + th( 20 ), label() );
		put( b, cx1, cy, white() );
		put( fixed( "bar.team2", 10, TRUE, L"TEAM 2" ), cx1, cy + th( 20 ), label() );
		const Int sum = max( 1, c1 + c2 );
		const Int w1 = (Int)((Real)barW * c1 / sum);
		fill( barX, barY, w1, S( 6 ), team1() );
		fill( barX + w1, barY, barW - w1, S( 6 ), team2() );
	}
	else
	{
		putC( clock, mid.x + mid.w / 2, mid.y + S( 10 ), white() );
		const Int key = (alive << 8) | total;
		putC( txt( "bar.ffa", 0, 10, TRUE, key, [alive, total]() {
			UnicodeString u;
			u.format( L"FREE FOR ALL    %d OF %d ALIVE", alive, total );
			return u; } ), mid.x + mid.w / 2, mid.y + S( 10 ) + th( 30 ) + S( 2 ), label() );
		Int sum = 0;
		for (Int i = 0; i < s_rowCount; ++i)
			sum += max( 0, s_rows[i].cash );
		Int x = barX;
		for (Int i = 0; i < s_rowCount && sum > 0; ++i)
		{
			const Int w = (i == s_rowCount - 1) ? barX + barW - x : (Int)((Real)barW * max( 0, s_rows[i].cash ) / sum);
			fill( x, barY, w, S( 6 ), s_rows[i].status == STATUS_ALIVE ? s_rows[i].color : GameMakeColor( 58, 70, 88, 255 ) );
			x += w;
		}
	}
}

/// The newest announcements ("X used Y") stacked under the PLAYERS tab (wh1ter0se 03/11): up to TOAST_MAX, newest on top, each
/// TOAST_LIFE_MS with a fade in and out, a newer one pushing the oldest out - toastLayout / toastStack in GXSpectatorHud.h, model-tested
/// frame by frame against the tab, the gear, the name tips and the customise panel. Click one: the camera jumps to that player's base
/// (or where its power was aimed). Same feed and same spectator gate as the legacy banners; only the place and the look change.
static Rect s_mapHome = { 0, 0, 0, 0 };		///< the minimap's home rect (mapHome), defined here because drawToasts keeps off the map
static const Int kToastRing = 4;				///< InGameUI's SKILL_BANNER_SLOTS: the four newest announcements the ring holds
struct ToastLine { Int id; Int slot; Int about; ObjectID object; Int science; UnsignedInt frame; UnsignedInt power; };
static ToastLine s_toastLines[kToastRing];		///< what each drawn toast's click acts on, rebuilt with the hits every frame
static Int s_toastLineCount = 0;
static Int s_toastSeenId[kToastRing] = { 0, 0, 0, 0 };				///< per ring slot: the toast (toastHitId) it last held...
static UnsignedInt s_toastSeenMs[kToastRing] = { 0, 0, 0, 0 };		///< ...and when this HUD first had it to draw (its motion starts there)
/// Where the toast stack goes now (toastLayout, model-tested). Home: under the bar's HOME spot, its PLAYERS tab, the gear and where a cut
/// name's tip appears - folded or open, any player count, so the stack never jumps (dragging the bar moves nothing else).
/// And never ON anything drawn: the bar and its tab where they are (dragged or not), the gear, the VIEWING
/// chip, the vote-kick ballot, the customise panel, the minimap and the followed-player panel with their tabs - home when free, else
/// the nearest free place. Called by drawToasts (after those panels, so their rects are this frame's) and by the retail message list
/// (gxSpectatorHudToastZone, earlier in the frame: the dock offsets and open panels are current; the chip, the ballot and the focus
/// panel's footprint are the last frame's there, so one appearing in this very frame can meet a message line for that one frame).
struct ToastScene { Rect above[4]; Rect avoid[12 + GX_READOUT_MAX]; Int na; };
/// What the stack hangs from and must keep off, as known now (see toastLayoutNow).
static void toastSceneNow( ToastScene &sc )
{
	Rect *above = sc.above;
	Rect *avoid = sc.avoid;
	above[0] = s_barHome;
	above[1] = playersTabRect( s_barHome, playersTabTextW(), s_scale );
	above[2] = gearTabRect( s_barHome, s_scale );
	above[3] = nameTipBand( s_barHome, th( 13 ), s_W, s_scale );
	Int na = 0;
	const Rect pb = placed( s_barHome, PANEL_PLAYERS );
	if (s_dock.panel[PANEL_PLAYERS].open)
	{
		avoid[na++] = pb;
		avoid[na++] = nameTipBand( pb, th( 13 ), s_W, s_scale );
	}
	avoid[na++] = playersTabRect( pb, playersTabTextW(), s_scale );
	avoid[na++] = gearTabRect( s_barHome, s_scale );
	if (s_chipRect.w > 0 && timeGetTime() - s_chipRectMs < 500)
		avoid[na++] = s_chipRect;
	Int vx = 0, vy = 0, vw = 0, vh = 0;
	if (::gxVoteKickBannerRect( &vx, &vy, &vw, &vh ))
		avoid[na++] = Rect{ vx, vy, vw, vh };
	if (s_dock.panel[PANEL_DISPLAY].open)
		avoid[na++] = placed( displayPanelHome( s_W, s_rowCount, s_scale ), PANEL_DISPLAY );		// = drawDisplayPanel's rect, known before it draws
	if (s_set.show[SHOW_MINIMAP])
	{
		const Rect mr = placed( s_mapHome, PANEL_MAP );
		Rect map = { mr.x + S( 40 ), mr.y - S( 22 ), tw( fixed( "tab.map", 10, TRUE, L"MAP" ) ) + S( 44 ), S( 20 ) };	// its tab
		if (s_dock.panel[PANEL_MAP].open)
			map = rectUnion( map, mr );
		avoid[na++] = map;
	}
	if (s_focusDrawn.w > 0)
		avoid[na++] = s_focusDrawn;
	// InGameUI.cpp's superweapon countdowns, drawn earlier this frame (at the message list: the last frame's)
	if (s_gxSwDrawnOn)
		avoid[na++] = Rect{ s_gxSwDrawn[0], s_gxSwDrawn[1], s_gxSwDrawn[2] - s_gxSwDrawn[0], s_gxSwDrawn[3] - s_gxSwDrawn[1] };
	// InGameUI.cpp's readouts (fps, logic rate, latency, clocks, fast forward), wherever HudAnchorX/Y or a mod put them
	for (Int i = 0; i < s_gxReadoutCount && i < GX_READOUT_MAX; ++i)
		avoid[na++] = Rect{ s_gxReadout[i][0], s_gxReadout[i][1], s_gxReadout[i][2], s_gxReadout[i][3] };
	sc.na = na;
}
static ToastLayout toastLayoutNow()
{
	ToastScene sc;
	toastSceneNow( sc );
	return toastLayout( sc.above, 4, sc.avoid, sc.na, s_W, s_H, s_scale );
}

/// ONE place per frame for the stack: decided at the first ask - the retail message list's, which runs before
/// the HUD and the vote ballot draw - and drawn there by drawToasts, so the list never clears one place while the toasts land on
/// another. s_frameSerial moves once per frame, where InGameUI::postDraw starts (gxSpectatorHudUnderWindows).
static UnsignedInt s_frameSerial = 0;
static UnsignedInt s_layoutSerial = 0xFFFFFFFFu;
static UnsignedInt s_zoneAskedSerial = 0xFFFFFFFFu;	///< the frame the message list last asked gxSpectatorHudToastZone...
static Bool s_zoneGiven = FALSE;						///< ...and whether it was given a zone: if not, no toast is drawn that frame
static ToastLayout s_frameLayout;
static ToastLayout toastLayoutForFrame()
{
	if (s_layoutSerial != s_frameSerial)
	{
		s_frameLayout = toastLayoutNow();
		s_layoutSerial = s_frameSerial;
	}
	return s_frameLayout;
}

static void drawToasts()
{
	s_toastLineCount = 0;
	UnicodeString text[kToastRing];
	Color color[kToastRing];
	UnsignedInt born[kToastRing], seen[kToastRing];
	ToastLine line[kToastRing];
	const UnsignedInt now = timeGetTime();
	Int n = 0;
	for (Int k = 0; k < kToastRing; ++k)
	{
		ToastLine &l = line[n];
		if (!TheInGameUI->gxNewestSkillBanner( k, text[n], color[n], born[n], l.about, l.object, l.science, l.frame, l.power, l.slot )
			|| l.slot < 0 || l.slot >= kToastRing)
			break;
		l.id = toastHitId( born[n], l.slot );
		if (s_toastSeenId[l.slot] != l.id)
		{
			s_toastSeenId[l.slot] = l.id;
			s_toastSeenMs[l.slot] = now;
		}
		seen[n] = s_toastSeenMs[l.slot];
		++n;
	}
	if (n == 0)
		return;
	// The place this frame's message list was given (toastLayoutForFrame). Something drawn since (a ballot that just appeared, a panel
	// dragged this frame) may now stand on it: then no toast this one frame - never on top of it, never off the list's clearance; the
	// next frame decides again with it known.
	const ToastLayout L = toastLayoutForFrame();
	if (!L.clear)
		return;		// no free place this frame: nothing drawn rather than a known collision; they come back when one frees up
	{
		ToastScene sc;
		toastSceneNow( sc );
		if (!toastFits( L.x, L.top, L.w, L.h, L.step, S( TOAST_DROP ), sc.avoid, sc.na, s_W, s_H ))
			return;
	}
	// The message list was told there was no toast zone this frame (the HUD was hidden last frame - F11, the Debug screen - so it
	// could not know it would draw now): its lines were not kept off, so no toast this one frame either.
	if (s_zoneAskedSerial == s_frameSerial && !s_zoneGiven)
		return;
	// The banner ring is wall-clock and survives reset() (InGameUI.cpp): toastStack's toastShows gate never replays last match's
	// final word. The toasts are the one thing in this HUD that moves on its own, so the one thing that fades and slides; everything
	// a spectator opens by hand stays instant (it is used all match long, often from the keyboard).
	ToastSlot slot[kToastRing];
	if (toastStack( born, seen, n, now, s_matchStartMs, s_loaded, L.top, L.step, S( TOAST_DROP ), slot ) == 0)
		return;
	static UnsignedInt s_loggedBorn = 0;
	if (slot[0].shown && born[0] != s_loggedBorn)
	{
		s_loggedBorn = born[0];
		fprintf( stderr, "[GXSPEC] toast f=%u\n", TheGameLogic->getFrame() );
	}
	// Oldest first, so the newest is drawn (and hit-tested: hitAt walks backwards) on top while one slides out from under it.
	const Int plateA = textAlpha();
	for (Int k = n - 1; k >= 0; --k)
	{
		if (!slot[k].shown)
			continue;
		const Real fade = slot[k].alpha;
		const Rect r = { L.x, slot[k].y, L.w, L.h };
		// The whole toast fades, its navy plate included (plate() keeps a text panel at TEXT_ALPHA_FLOOR whatever the fade, which
		// is why the old toast's dark box popped in and out while only its text faded).
		s_fade = fade;
		fill( r, navyDeep( (Int)(plateA * fade) ) );
		box( Rect{ r.x - 1, r.y - 1, r.w + 2, r.h + 2 }, faded( edge() ) );
		box( r, faded( steel() ) );
		if (line[k].about >= 0 && s_toastLineCount < kToastRing)
		{
			addHit( r, H_TOAST, line[k].id, -1 );
			s_toastLines[s_toastLineCount++] = line[k];
			if (mouseIn( r ))
				fill( r, GameMakeColor( 111, 168, 240, (Int)(50 * fade) ) );		// clickable: the dock tabs' own hover
		}
		if (k == 0)
		{
			s_toastRect = r;
			s_toastRectMs = now;
		}
		fill( r.x, r.y, S( 4 ), r.h, faded( color[k] ) );
		const UnicodeString t0 = text[k];
		DisplayString *t = txt( "toast", line[k].slot, 14, TRUE, (Int)born[k], [t0]() { return t0; } );
		IRegion2D clip;
		clip.lo.x = r.x + S( 12 );
		clip.lo.y = r.y;
		clip.hi.x = r.x + r.w - S( 80 );
		clip.hi.y = r.y + r.h;
		if (t)
			t->setClipRegion( &clip );
		const Int ta = (Int)(255 * fade);
		put( t, r.x + S( 14 ), r.y + (L.h - th( 14 )) / 2, GameMakeColor( 238, 243, 250, ta ) );
		const UnsignedInt secs = (now - born[k]) / 1000;
		putR( txt( "toast.age", line[k].slot, 10, TRUE, (Int)secs, [secs]() { UnicodeString u; u.format( L"%u:%02u AGO", secs / 60, secs % 60 ); return u; } ),
			r.x + r.w - S( 12 ), r.y + (L.h - th( 10 )) / 2, GameMakeColor( 150, 172, 204, ta ) );
	}
	s_fade = 1.0f;
}

/// The toast a click was pressed on, by its identity (toastHitId); nullptr once it is gone.
static const ToastLine *toastLine( Int id )
{
	for (Int i = 0; i < s_toastLineCount; ++i)
		if (s_toastLines[i].id == id)
			return &s_toastLines[i];
	return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// The fitted minimap and the followed player's panel (s_mapHome is defined above drawToasts, which keeps off the map)
// ---------------------------------------------------------------------------------------------------------------------

static void mapHome()
{
	Int mw = 1, mh = 1;
	if (TheTerrainLogic)
	{
		Region3D ext;
		TheTerrainLogic->getExtent( &ext );
		mw = (Int)ext.width();
		mh = (Int)ext.height();
	}
	const Int pad = S( 6 );
	const Rect fit = fitMinimap( mw, mh, S( 560 ) - 2 * pad, S( 430 ) - 2 * pad );
	s_mapHome.w = fit.w + 2 * pad;
	s_mapHome.h = fit.h + 2 * pad;
	s_mapHome.x = 0;
	s_mapHome.y = s_H - s_mapHome.h;
}

static void drawMap( Rect &out )
{
	out = placed( s_mapHome, PANEL_MAP );
	s_mapInner.w = 0;
	if (!s_dock.panel[PANEL_MAP].open || TheRadar == nullptr)
		return;
	plate( out );
	addHit( out, H_BODY, 0, PANEL_MAP );
	const Int pad = S( 6 );
	const Rect in = { out.x + pad, out.y + pad, out.w - 2 * pad, out.h - 2 * pad };
	// The panel already has the map's shape, so the radar's own aspect fit (Radar::findDrawPositions) draws edge to
	// edge: no black bars, and no change to the radar code (Radar.cpp is digested).
	IRegion2D clip;
	clip.lo.x = out.x;
	clip.lo.y = out.y;
	clip.hi.x = out.x + out.w;
	clip.hi.y = out.y + out.h;
	gxRadarViewBoxClip = &clip;
	TheRadar->draw( in.x, in.y, in.w, in.h );
	gxRadarViewBoxClip = nullptr;
	s_mapInner = in;
	addHit( in, H_MAP, 0, PANEL_MAP );
}

static void minimapLookAt( Int px, Int py )
{
	const Rect &m = s_mapInner;
	if (m.w <= 0 || m.h <= 0 || TheRadar == nullptr || TheTacticalView == nullptr)
		return;
	ICoord2D cell;
	cell.x = clamp( 0, (px - m.x) * RADAR_CELL_WIDTH / m.w, RADAR_CELL_WIDTH - 1 );
	cell.y = clamp( 0, (RADAR_CELL_HEIGHT - 1) - (py - m.y) * RADAR_CELL_HEIGHT / m.h, RADAR_CELL_HEIGHT - 1 );
	Coord3D world;
	if (TheRadar->radarToWorld( &cell, &world ))
	{
		TheTacticalView->lookAt( &world );
		releaseReplayCameraLock();		// a paused replay: the view is the spectator's now, not the followed player's
	}
}

static void drawStars( Int x, Int y, Int size, Int rank, Color tint = 0xFFFFFFFF )
{
	static const Image *s_star = nullptr;
	static Bool s_starLooked = FALSE;	// a data set without Star-Gold is asked once, not on every draw (the lookup is linear)
	if (!s_starLooked && TheMappedImageCollection)
	{
		s_star = TheMappedImageCollection->findImageByNameNoMint( "Star-Gold" );	// never mints a NameKey, see portraitFor
		s_starLooked = TRUE;
	}
	for (Int i = 0; i < rank; ++i)
		if (s_star)
			TheDisplay->drawImage( s_star, x + i * size, y, x + i * size + size - S( 1 ), y + size - S( 1 ), tint );
}

// ---- the inspected object: a spectator's battlefield click (inspectAt) -----------------------------------------------
/// The object the spectator clicked, while it lives: a dead, sold or captured-and-gone object clears the card.
static Object *inspectedObject()
{
	Object *o = TheGameLogic != nullptr ? TheGameLogic->findObjectByID( s_inspectID ) : nullptr;
	if (o == nullptr || o->isEffectivelyDead() || o->getDrawable() == nullptr)
	{
		s_inspectID = INVALID_ID;
		return nullptr;
	}
	return o;
}

/// Corner brackets around the inspected object on the battlefield - which unit the card is about (the HUD's feedback for the
/// click, as the retail selection ring was before the click stopped selecting). Under every panel; off-screen draws nothing.
static void drawInspectMarker( const Object *o )
{
	if (TheTacticalView == nullptr)
		return;
	const Coord3D *pos = o->getPosition();
	Coord3D top = *pos, side = *pos;
	top.z += o->getGeometryInfo().getMaxHeightAbovePosition();
	side.x += o->getGeometryInfo().getBoundingCircleRadius();
	ICoord2D foot, head, edge;
	if (!TheTacticalView->worldToScreen( pos, &foot ) || !TheTacticalView->worldToScreen( &top, &head ))
		return;
	const Int halfW = TheTacticalView->worldToScreen( &side, &edge ) ? max( S( 12 ), abs( edge.x - foot.x ) + S( 4 ) ) : S( 24 );
	const Int x0 = foot.x - halfW, x1 = foot.x + halfW, y0 = min( head.y, foot.y - S( 16 ) ) - S( 4 ), y1 = foot.y + S( 6 );
	const Int len = S( 9 ), t = max( 1, S( 2 ) );
	const Color c = accent();
	fill( x0, y0, len, t, c ); fill( x0, y0, t, len, c );
	fill( x1 - len, y0, len, t, c ); fill( x1 - t, y0, t, len, c );
	fill( x0, y1 - t, len, t, c ); fill( x0, y1 - len, t, len, c );
	fill( x1 - len, y1 - t, len, t, c ); fill( x1 - t, y1 - len, t, len, c );
}

/// An upgrade cameo's template by a plain walk of the list: UpgradeCenter::findUpgrade goes through nameToKey, which MINTS a
/// NameKey for a name no upgrade defines - on this client only, shifting every key minted after it (a desync hazard).
static const UpgradeTemplate *upgradeNamed( const AsciiString &name )
{
	if (name.isEmpty() || TheUpgradeCenter == nullptr)
		return nullptr;
	for (const UpgradeTemplate *u = TheUpgradeCenter->firstUpgradeTemplate(); u != nullptr; u = u->friend_getNext())
		if (u->getUpgradeName() == name)
			return u;
	return nullptr;
}

/// Retail's veterancy badge for an object (the three chevrons ControlBar::calculateVeterancyOverlayForObject returns; that one is
/// protected), looked up without minting a NameKey. nullptr for a regular.
static const Image *chevronFor( const Object *o )
{
	static const Image *s_chevron[3] = { nullptr, nullptr, nullptr };
	static Bool s_chevronLooked = FALSE;
	if (!s_chevronLooked && TheMappedImageCollection)
	{
		s_chevron[0] = TheMappedImageCollection->findImageByNameNoMint( "SSChevron1L" );
		s_chevron[1] = TheMappedImageCollection->findImageByNameNoMint( "SSChevron2L" );
		s_chevron[2] = TheMappedImageCollection->findImageByNameNoMint( "SSChevron3L" );
		s_chevronLooked = TRUE;
	}
	const Int vet = (Int)o->getVeterancyLevel();		// LEVEL_REGULAR 0 .. LEVEL_HEROIC 3
	return vet >= 1 && vet <= 3 ? s_chevron[vet - 1] : nullptr;
}

// ---- what is inside: the card's occupants ----------------------------------------------------------------------------
// GeneralsX: Owner, on a spectator's China Bunker card: "Am I not supposed to be able to see how many
// units he has in those?" The card lists a container's occupants as retail's exit buttons do for their owner
// (ControlBar::doTransportInventoryUI / populateStructureInventory): each occupant's ButtonImage cameo with its veterancy overlay,
// then one empty box per free slot. Spectators only (mayShowOccupants); the decisions are GXSpectatorHud.h's, unit-tested.
// READ-ONLY: the contain module's getters and the occupants' own templates, nothing else.
static ArrivalWatch s_arrivals;
static std::vector<Object *> s_occ;			///< this frame's occupants, in the contain list's own (entry) order: cameos never jump
static std::vector<unsigned> s_occIds;
static ObjectID s_occLogObj = INVALID_ID;	///< what the last [GXSPEC] occupants line said: it prints on a change only
static std::wstring s_occLogText;			///< ...its head line and its layout (cameos, free boxes, +N), so a resize logs too
static const UnsignedInt kArrivalMs = 1000;	///< an occupant that just went in keeps a lit frame this long

/// Draws the group at cx (head line at headY, cameos at iy, `icon` px at most) and returns the x after it; cx itself when the
/// object shows no contents.
static Int drawOccupants( const Rect &b, Object *o, const ThingTemplate *tt, Bool showPortrait, Int cx, Int headY, Int iy, Int icon )
{
	if (!mayShowOccupants( s_mode ))
		return cx;
	ContainModuleInterface *contain = o->getContain();
	ContainFacts f = containFacts( o );
	f.cardShowsPortrait = showPortrait;
	f.disguisedAsOther = tt != o->getTemplate();
	const OccupantKind kind = occupantKind( f );
	if (kind == OCC_NONE)
		return cx;

	// A tunnel's list is its owner's whole network (TunnelContain asks the owner's TunnelTracker); an Overlord's is its Battle
	// Bunker's (the redirect). A portable structure (an Overlord's or Helix's turret) is the vehicle's add-on, not a passenger.
	s_occ.clear();
	s_occIds.clear();
	if (const ContainedItemsList *items = contain->getContainedItemsList())
		for (Object *x : *items)
			if (x != nullptr && !x->isKindOf( KINDOF_PORTABLE_STRUCTURE ))
			{
				s_occ.push_back( x );
				s_occIds.push_back( (unsigned)x->getID() );
			}
	const UnsignedInt now = timeGetTime();
	s_arrivals.update( (unsigned)o->getID(), s_occIds.data(), (int)s_occIds.size(), now );
	// Slots as the owner's pips count them (getContainerPipsToShow, a getter in every override: an Overlord's follow its Battle
	// Bunker's redirect, whose extra slots getExtraSlotsInUse would miss); a container that draws no pips (a Combat Cycle, a Helix
	// whose data turns them off) from its own getters.
	Int pipTotal = 0, pipFull = 0;
	const SlotCount slots = contain->getContainerPipsToShow( pipTotal, pipFull ) ? slotCount( pipFull, 0, pipTotal )
		: slotCount( (Int)contain->getContainCount(), contain->getExtraSlotsInUse(), contain->getContainMax() );

	// Head line: the word in the section-head style (UPGRADES, PRODUCING), the count beside it in white - the number is the news.
	DisplayString *word = txt( "obj.occw", 0, 10, TRUE, (long long)kind, [kind]() { return UnicodeString( occupantWord( kind ) ); } );
	put( word, cx, headY, label() );
	Int headEnd = cx + tw( word );
	const std::wstring countText = occupantCount( kind, slots );
	if (!countText.empty())
	{
		DisplayString *count = txt( "obj.occn", 0, 10, TRUE, ((long long)slots.used << 32) | ((long long)slots.total << 1) | (slots.known ? 1 : 0),
			[countText]() { return UnicodeString( countText.c_str() ); } );
		put( count, headEnd + S( 8 ), headY, white() );
		headEnd += S( 8 ) + tw( count );
	}

	// The row: occupants, then the free slots, shrunk to fit the card (free boxes give way first, then a "+N" cell). Cells keep
	// retail's button shape, 5:4 (a ButtonImage is 60x48 or 120x96, as is the chevron overlay): `icon` is the row's height.
	const Int gap = S( 6 );
	const CameoFit fit = fitCameos( (Int)s_occ.size(), slots.free, b.x + b.w - S( 14 ) - cx, icon * 5 / 4, S( 32 ), gap );
	const Int cw = fit.icon, ch = fit.icon * 4 / 5;
	const Int y = iy + (icon - ch) / 2;
	Int x = cx;
	const Object *tip = nullptr;
	Rect tipAt = { 0, 0, 0, 0 };
	for (Int i = 0; i < fit.units; ++i, x += cw + gap)
	{
		const Object *occ = s_occ[i];
		const Rect ir = { x, y, cw, ch };
		fill( ir, navyDeep( 255 ) );
		if (const Image *cameo = occ->getTemplate()->getButtonImage())
			TheDisplay->drawImage( cameo, ir.x, ir.y, ir.x + ir.w, ir.y + ir.h );
		if (const Image *chev = chevronFor( occ ))		// over the whole cameo, as retail's button overlay draws it
			TheDisplay->drawImage( chev, ir.x, ir.y, ir.x + ir.w, ir.y + ir.h );
		if (s_arrivals.lit( s_occIds[i], now, kArrivalMs ))
		{
			box( ir, accent() );
			box( Rect{ ir.x + 1, ir.y + 1, ir.w - 2, ir.h - 2 }, accent() );
		}
		else
			box( ir, steel() );
		if (mouseIn( ir ))
		{
			tip = occ;
			tipAt = ir;
		}
	}
	if (fit.moreCell)
	{
		const Rect orr = { x, y, cw, ch };
		fill( orr, navyDeep( 255 ) );
		box( orr, steelDim() );
		const Int more = fit.overflow;
		putC( txt( "obj.occmore", 0, 12, TRUE, more, [more]() { UnicodeString u; u.format( L"+%d", more ); return u; } ),
			orr.x + orr.w / 2, orr.y + (orr.h - th( 12 )) / 2, label() );
		x += cw + gap;
	}
	for (Int i = 0; i < fit.free; ++i, x += cw + gap)
	{
		const Rect er = { x, y, cw, ch };
		fill( er, navyDeep( 150 ) );
		box( er, steelDim() );
	}

	// The occupant under the mouse: its name, on a plate just above the cameo (below it when the card sits at the top edge).
	if (tip != nullptr)
	{
		const ThingTemplate *ttt = tip->getTemplate();
		DisplayString *d = txt( "obj.occtip", 0, 13, TRUE, (long long)(intptr_t)ttt, [ttt]() { return ttt->getDisplayName(); } );
		if (d != nullptr)
		{
			const Int w = tw( d ) + S( 20 ), h = th( 13 ) + S( 12 );
			Rect r = { max( S( 4 ), min( tipAt.x, s_W - w - S( 4 ) ) ), tipAt.y - h - S( 6 ), w, h };
			if (r.y < 0)
				r.y = tipAt.y + tipAt.h + S( 6 );
			plate( r, TRUE, TRUE );
			put( d, r.x + S( 10 ), r.y + S( 6 ), white() );
		}
	}

	// Run evidence (pictures, tests): one line per change of what the group says or how it is laid out (spectator-occupants r1).
	const std::wstring text = occupantLabel( kind, slots ) + L"|" + std::to_wstring( fit.units ) + L"," + std::to_wstring( fit.free ) + L","
		+ std::to_wstring( fit.overflow );
	if (s_occLogObj != o->getID() || s_occLogText != text)
	{
		s_occLogObj = o->getID();
		s_occLogText = text;
		std::string ascii;
		for (wchar_t c : occupantLabel( kind, slots ))
			ascii.push_back( c > 0 && c < 128 ? (char)c : '?' );
		fprintf( stderr, "[GXSPEC] occupants obj=%u '%s' cameos=%d free=%d more=%d f=%u\n", (unsigned)o->getID(), ascii.c_str(),
			fit.units, fit.free, fit.overflow, TheGameLogic->getFrame() );
	}
	return max( x - gap, headEnd ) + S( 34 );
}

static const Int kObjectBandH = 112;	///< 1080p pixels

/// What retail's observer bar shows for a selected object (ControlBar::setPortraitByObject: the portrait, its veterancy badge, the
/// template's five upgrade cameos lit or dim) plus what a caster asks about it: owner, health, what it is building. Read-only.
static void drawObjectBand( const Rect &b, Object *o )
{
	plate( b, FALSE, TRUE );
	addHit( b, H_BODY, 0, PANEL_FOCUS );
	// Retail's portrait rules: a disguise is shown as what it pretends to be (apparentOwner), and a civilian vehicle with nobody
	// in it - or a disguise that is one - shows no portrait (it would show the terrorist inside a disguised car bomb).
	const ThingTemplate *tt = nullptr;
	Player *owner = apparentOwner( o, &tt );
	// Retail hides the portrait, its badge and the cameos together then (setPortraitByObject( nullptr )).
	const Bool showPortrait = !(o->isKindOf( KINDOF_SHOW_PORTRAIT_WHEN_CONTROLLED ) && !o->isLocallyControlled())
		&& !(tt != o->getTemplate() && tt->isKindOf( KINDOF_SHOW_PORTRAIT_WHEN_CONTROLLED ));
	const Row *ownerRow = nullptr;
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].player == owner)
			ownerRow = &s_rows[i];

	const Int pad = S( 12 ), ph = b.h - 2 * pad;
	const Image *pimg = showPortrait ? tt->getSelectedPortraitImage() : nullptr;
	const Int pw = pimg != nullptr && pimg->getImageHeight() > 0 ? ph * pimg->getImageWidth() / pimg->getImageHeight() : ph * 5 / 4;
	const Rect pr = { b.x + S( 14 ), b.y + pad, pw, ph };
	fill( pr, navyDeep( 255 ) );
	if (pimg != nullptr)
		TheDisplay->drawImage( pimg, pr.x, pr.y, pr.x + pr.w, pr.y + pr.h );
	const Image *chev = showPortrait ? chevronFor( o ) : nullptr;
	if (chev != nullptr)
		TheDisplay->drawImage( chev, pr.x + S( 3 ), pr.y + S( 3 ), pr.x + S( 25 ), pr.y + S( 25 ) );
	box( pr, steel() );
	fill( pr.x, pr.y + pr.h - S( 3 ), pr.w, S( 3 ), ownerRow != nullptr ? ownerRow->color : dim() );

	// Name, owner, health.
	const Int tx = pr.x + pr.w + S( 16 );
	put( txt( "obj.name", 0, 21, TRUE, (long long)(intptr_t)tt, [tt]() { return tt->getDisplayName(); } ), tx, b.y + pad, white() );
	const Int oy = b.y + pad + th( 21 ) + S( 2 );
	Player *op = owner;
	put( txt( "obj.owner", 0, 12, FALSE, ownerRow != nullptr ? (long long)ownerRow->slot : -1, [op, ownerRow]() {
		return ownerRow != nullptr ? hudName( op ) : UnicodeString( L"NEUTRAL" ); } ), tx, oy, ownerRow != nullptr ? ownerRow->textColor : dim() );
	const BodyModuleInterface *body = o->getBodyModule();
	const Int hp = body != nullptr ? (Int)(body->getHealth() + 0.5f) : 0, hpMax = body != nullptr ? (Int)(body->getMaxHealth() + 0.5f) : 0;
	const Bool building = o->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION );
	const Int pct = building ? (Int)o->getConstructionPercent() : 0;
	const Int by = oy + th( 12 ) + S( 12 ), bw = S( 240 ), bh = S( 8 );
	fill( tx, by, bw, bh, navyDeep( 255 ) );
	if (hpMax > 0)
	{
		// The retail health bar's thresholds (Drawable::drawHealthBar): green above half, yellow above a quarter, red below.
		const Int ratio100 = hp * 100 / hpMax;
		const Color hc = building ? accent() : ratio100 > 50 ? green() : ratio100 > 25 ? amber() : red();
		fill( tx, by, bw * (building ? pct : ratio100) / 100, bh, hc );
	}
	box( Rect{ tx, by, bw, bh }, steelDim() );
	put( txt( "obj.hp", 0, 12, TRUE, building ? -1 - pct : ((long long)hp << 32) | (UnsignedInt)hpMax, [hp, hpMax, building, pct]() {
		UnicodeString u;
		if (building)
			u.format( L"BUILDING %d%%", pct );
		else
			u.format( L"%d / %d", hp, hpMax );
		return u; } ), tx + bw + S( 10 ), by + (bh - th( 12 )) / 2, label() );

	// The template's upgrade cameos, lit when this object (or, for a player upgrade, its owner) has it - retail's test.
	Int cx = tx + S( 400 );
	const Int icon = S( 44 ), gap = S( 6 ), iy = b.y + pad + th( 10 ) + S( 6 );
	Int shown = 0;
	for (Int i = 0; showPortrait && i < MAX_UPGRADE_CAMEO_UPGRADES; ++i)
	{
		const UpgradeTemplate *ut = upgradeNamed( tt->getUpgradeCameoName( i ) );
		if (ut == nullptr || ut->getButtonImage() == nullptr)
			continue;
		if (shown == 0)
			put( fixed( "obj.upg", 10, TRUE, L"UPGRADES" ), cx, b.y + pad, label() );
		const Bool has = o->hasUpgrade( ut ) || (owner != nullptr && owner->hasUpgradeComplete( ut ));
		const Rect ir = { cx + shown * (icon + gap), iy, icon, icon };
		TheDisplay->drawImage( ut->getButtonImage(), ir.x, ir.y, ir.x + ir.w, ir.y + ir.h, 0xFFFFFFFF,
			has ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
		box( ir, has ? steel() : steelDim() );
		++shown;
	}
	if (shown > 0)
		cx += shown * (icon + gap) + S( 34 );

	// What is inside it (a spectator's view only): the group draws itself, and moves cx past it.
	cx = drawOccupants( b, o, tt, showPortrait, cx, b.y + pad, iy, icon );

	// What it is building: the head of its queue, and how many wait behind it.
	ProductionUpdateInterface *pu = o->getProductionUpdateInterface();
	const ProductionEntry *e = pu != nullptr ? pu->firstProduction() : nullptr;
	if (e != nullptr && cx + S( 260 ) <= b.x + b.w)
	{
		put( fixed( "obj.prod", 10, TRUE, L"PRODUCING" ), cx, b.y + pad, label() );
		// One union in ProductionEntry: read only the member its type names (spectator-click).
		const ThingTemplate *unit = e->getProductionType() == PRODUCTION_UNIT ? e->getProductionObject() : nullptr;
		const UpgradeTemplate *upg = e->getProductionType() == PRODUCTION_UPGRADE ? e->getProductionUpgrade() : nullptr;
		const void *what = unit != nullptr ? (const void *)unit : (const void *)upg;
		put( txt( "obj.pname", 0, 15, TRUE, (long long)(intptr_t)what, [unit, upg]() {
			return unit != nullptr ? unit->getDisplayName() : upg != nullptr ? TheGameText->fetch( upg->getDisplayNameLabel() ) : UnicodeString(); } ),
			cx, iy, white() );
		const Int p = (Int)e->getPercentComplete(), qy = iy + th( 15 ) + S( 8 ), qw = S( 200 );
		fill( cx, qy, qw, S( 6 ), navyDeep( 255 ) );
		fill( cx, qy, qw * clamp( 0, p, 100 ) / 100, S( 6 ), accent() );
		const Int queued = (Int)pu->getProductionCount() - 1;
		put( txt( "obj.pq", 0, 12, FALSE, p * 1000 + queued, [p, queued]() {
			UnicodeString u;
			if (queued > 0)
				u.format( L"%d%%   +%d QUEUED", p, queued );
			else
				u.format( L"%d%%", p );
			return u; } ), cx + qw + S( 10 ), qy + (S( 6 ) - th( 12 )) / 2, label() );
	}
}

static const Int kFocusFullH = 160;	///< 1080p pixels: the followed player's full panel, and the dock offsets' reference height

namespace Strip { static Bool liveRect( Rect &out ); }	///< GXReplayStrip.inl (same unity TU): the replay strip's rect while it is drawn

static Int s_focusHidden = 0;	///< px of the FOCUS offset hidden by the drawn top clamp last frame (focusHiddenTop)

static void drawFocus( const Rect &mapRect )
{
	const Int x = s_mapHome.w + S( 16 );
	(void)mapRect;
	// One compact bar: the selected object's card and the followed player's stats share it, and with nobody followed the
	// empty panel is gone (wh1ter0se 14/10: "still taking too much space").
	const Row *f = rowForFocus();
	Object *sel = s_dock.panel[PANEL_FOCUS].open ? inspectedObject() : nullptr;
	const FocusBarLayout lay = focusBarLayout( f != nullptr, sel != nullptr, kFocusFullH, kObjectBandH, 56 );
	const Int homeW = max( S( 700 ), s_W - S( 16 ) - x );
	// The dock offset (dx, dy) is measured against the FULL panel's home whatever is showing, so a collapsed or compact bar never
	// clamps a zero-height rect into a permanent offset; the shown strip sits on the home's bottom edge.
	// In a replay the GeneralsX replay strip (pause / speed / scrub) sits on the bottom edge: the bar stands on top of it, not behind
	// it (seen on the NEW picture, the card's lower rows ran under the strip).
	// A CONSTANT clearance (the strip's home height + its gap), not its live rect: the bar's home just sits higher while a replay
	// strip is up, so the saved dock offset is never rewritten and a drag starts from what is drawn. A strip the
	// user dragged elsewhere is the user's doing; the tab is still kept out from under it below.
	Rect strip = { 0, 0, 0, 0 };
	const Bool stripUp = Strip::liveRect( strip );		// up or not - never where the user dragged it
	const Int clearance = stripUp ? S( 44 + 6 ) : 0;
	// The offset is clamped (and written back) against the strip-free home only; the clearance lifts what is drawn. A frame without
	// the strip (the first after F11 brings overlays back, the quit menu) used to clamp a low offset 50 px up for good.
	const Rect fullHome = { x, s_H - S( 18 ) - S( kFocusFullH ), homeW, S( kFocusFullH ) };
	Rect fullR = placed( fullHome, PANEL_FOCUS );
	s_focusHidden = focusHiddenTop( fullR.y, clearance );	// a drag starts from the drawn bar
	fullR.y = focusDrawnTop( fullR.y, clearance );
	const Rect home = { x, focusBarTop( fullHome.y - clearance, S( kFocusFullH ), S( lay.panelH ) ), homeW, S( lay.panelH ) };
	const Rect r = { fullR.x, focusBarTop( fullR.y, S( kFocusFullH ), S( lay.panelH ) ), fullR.w, S( lay.panelH ) };
	Int tabY = r.y;
	Rect drawnBand = { 0, 0, 0, 0 };
	if (sel != nullptr)
	{
		// The clicked object stacks on the panel, above it - or below it when the panel was dragged to the top edge.
		Rect band = { r.x, r.y - S( 4 ) - S( lay.bandH ), r.w, S( lay.bandH ) };
		if (band.y - S( 22 ) < 0)		// no room above for it AND the tab on top of it (spectator-click)
			band.y = r.y + r.h + S( 4 );
		else
			tabY = band.y;
		drawObjectBand( band, sel );
		drawnBand = band;
	}
	tabY = min( tabY, s_H - S( 4 ) );		// a collapsed bar dragged down keeps its tab on screen
	if (stripUp)		// ... and never under the replay strip, which takes input first
	{
		// above the strip, or below one docked at the top edge; never off the top
		tabY = focusTabY( tabY, home.x + S( 330 ) + s_dock.panel[PANEL_FOCUS].dx, S( 120 ), S( 20 ), strip, S( 22 ), S( 4 ) );
	}
	if (s_dock.panel[PANEL_FOCUS].open && f != nullptr && lay.compact)
	{
		// Compact strip: small portrait, name + general, then the four numbers a caster reads first.
		plate( r, FALSE, TRUE );
		addHit( r, H_BODY, 0, PANEL_FOCUS );
		Int tx = r.x + S( 14 );
		if (shown( SHOW_PORTRAITS ))
		{
			const Int pz = r.h - S( 16 );
			const Rect pr = { tx, r.y + S( 8 ), pz, pz };
			fill( pr, navyDeep( 255 ) );
			if (f->portrait)
				TheDisplay->drawImage( f->portrait, pr.x, pr.y, pr.x + pr.w, pr.y + pr.h, 0xFFFFFFFF,
					f->status == STATUS_ALIVE ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
			box( pr, steel() );
			fill( pr.x, pr.y + pr.h - S( 3 ), pr.w, S( 3 ), f->color );
			tx += pz + S( 12 );
		}
		Player *pl = f->player;
		const Int nameRoom = max( S( 100 ), r.x + r.w - S( 16 ) - tx ) * 35 / 100;		// the columns start after this
		DisplayString *nm = txt( "focus.cname", f->slot, 21, TRUE, nameRoom, [pl, nameRoom]() {
			return ellipsize( hudName( pl ), 21, TRUE, nameRoom, nullptr ); } );
		put( nm, tx, r.y + S( 8 ), f->textColor );
		const UnicodeString gen = f->general;
		put( txt( "focus.cgen", f->slot, 12, FALSE, 0, [gen]() { UnicodeString u = gen; u.concat( L" General" ); return u; } ),
			tx, r.y + S( 8 ) + th( 21 ) + S( 2 ), label() );
		// The columns share what the name leaves: the name is capped at 35% of the room so a long one cannot push them off the plate.
		const Int avail = max( S( 100 ), r.x + r.w - S( 16 ) - tx );
		const Int cx = tx + min( max( tw( nm ), S( 150 ) ), avail * 35 / 100 ) + S( 24 );
		const Int cw = max( S( 40 ), (r.x + r.w - S( 16 ) - cx) / 5 );
		for (Int i = 0; i < 5; ++i)
		{
			static const wchar_t *const heads[5] = { L"CASH", L"INCOME / MIN", L"POWER USED", L"UNITS", L"BUILDINGS" };
			static const char *const ids[5] = { "c.h0", "c.h1", "c.h2", "c.h3", "c.h4" };
			const Int px = cx + i * cw, py = r.y + S( 8 );
			put( fixed( ids[i], 10, TRUE, heads[i] ), px, py, label() );
			const Int vy = py + th( 10 ) + S( 3 );
			DisplayString *v = nullptr;
			Color vc = white();
			switch (i)
			{
				case 0: v = number( "c.cash", 0, 21, TRUE, f->cash, FALSE, TRUE ); break;
				case 1: v = number( "c.cpm", 0, 21, TRUE, f->cpm, TRUE ); break;
				case 2:
				{
					const Int pr = f->prod, u = f->use;
					v = txt( "c.pw", 0, 21, TRUE, ((long long)u << 32) | (UnsignedInt)pr, [u, pr]() { UnicodeString q; q.format( L"%d / %d", u, pr ); return q; } );
					vc = f->deficit ? red() : white();
					break;
				}
				case 3: v = number( "c.units", 0, 21, TRUE, s_units[f->slot] ); break;
				default: v = number( "c.bldgs", 0, 21, TRUE, s_bldgs[f->slot] ); break;
			}
			put( v, px, vy, vc );
		}
	}
	else if (s_dock.panel[PANEL_FOCUS].open && f != nullptr)
	{
		plate( r, FALSE, TRUE );
		addHit( r, H_BODY, 0, PANEL_FOCUS );
		{
			const Int pz = S( 128 );
			const Rect pr = { r.x + S( 14 ), r.y + (r.h - pz) / 2, pz, pz };
			fill( pr, navyDeep( 255 ) );
			if (f->portrait)
				TheDisplay->drawImage( f->portrait, pr.x, pr.y, pr.x + pr.w, pr.y + pr.h, 0xFFFFFFFF,
					f->status == STATUS_ALIVE ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
			box( pr, steel() );
			fill( pr.x, pr.y + pr.h - S( 3 ), pr.w, S( 3 ), f->color );
			const Int tx = pr.x + pr.w + S( 18 );
			Player *pl = f->player;
			put( txt( "focus.name", f->slot, 30, TRUE, 0, [pl]() { return hudName( pl ); } ), tx, r.y + S( 16 ), f->textColor );
			const UnicodeString gen = f->general;
			const Int team = f->team;
			const Bool two = s_twoTeams;
			DisplayString *g = txt( "focus.gen", f->slot, 15, FALSE, (team << 1) | (two ? 1 : 0), [gen]() {
				UnicodeString u = gen;
				u.concat( L" General" );
				return u; } );
			const Int gy = r.y + S( 16 ) + th( 30 ) + S( 6 );
			put( g, tx, gy, label() );
			drawStars( tx + tw( g ) + S( 10 ), gy + S( 1 ), S( 14 ), f->rank );
			if (two && team >= 0)
				put( txt( "focus.team", f->slot, 15, FALSE, team, [team]() { UnicodeString u; u.format( L"Team %d", team + 1 ); return u; } ),
					tx + tw( g ) + S( 20 ) + f->rank * S( 14 ), gy, label() );
			// Army mix: infantry, vehicles, aircraft, buildings (4 Hz counts).
			const Int my = gy + th( 15 ) + S( 10 ), mw = S( 440 );
			const Int b = s_bldgs[f->slot];
			const Int parts[4] = { s_inf, s_veh, s_air, b };
			const Color cols[4] = { GameMakeColor( 106, 169, 255, 255 ), GameMakeColor( 214, 207, 106, 255 ),
				GameMakeColor( 123, 224, 224, 255 ), GameMakeColor( 160, 168, 168, 255 ) };
			const Int sum = max( 1, parts[0] + parts[1] + parts[2] + parts[3] );
			Int mx = tx;
			for (Int i = 0; i < 4; ++i)
			{
				const Int w = i == 3 ? tx + mw - mx : mw * parts[i] / sum;
				fill( mx, my, w, S( 6 ), cols[i] );
				mx += w;
			}
			static const wchar_t *const names[4] = { L"Inf", L"Veh", L"Air", L"Bldg" };
			Int lx = tx;
			for (Int i = 0; i < 4; ++i)
			{
				fill( lx, my + S( 14 ), S( 8 ), S( 8 ), cols[i] );
				const wchar_t *nm = names[i];
				const Int v = parts[i];
				DisplayString *l = txt( "focus.mix", i, 12, FALSE, v, [nm, v]() { UnicodeString u; u.format( L"%ls %d", nm, v ); return u; } );
				put( l, lx + S( 12 ), my + S( 11 ), label() );
				lx += S( 12 ) + tw( l ) + S( 14 );
			}
			// Stats: two rows of four.
			const Int gx = tx + mw + S( 40 ), gw = r.x + r.w - S( 20 ) - gx, cw = max( S( 90 ), gw / 4 );
			for (Int i = 0; i < 8; ++i)
			{
				const Int cx = gx + (i % 4) * cw, cy = r.y + S( 30 ) + (i / 4) * S( 54 );
				static const wchar_t *const heads[8] = { L"CASH", L"INCOME / MIN", L"POWER USED", L"K / D", L"UNITS", L"BUILDINGS", L"XP", L"GEN. POINTS" };
				static const char *const ids[8] = { "f.h0", "f.h1", "f.h2", "f.h3", "f.h4", "f.h5", "f.h6", "f.h7" };
				if (i == 6 && !s_set.show[SHOW_XP])
					continue;
				put( fixed( ids[i], 10, TRUE, heads[i] ), cx, cy, label() );
				const Int vy = cy + th( 10 ) + S( 3 );
				DisplayString *v = nullptr;
				Color vc = white();
				switch (i)
				{
					case 0: v = number( "f.cash", 0, 21, TRUE, f->cash, FALSE, TRUE ); break;
					case 1: v = number( "f.cpm", 0, 21, TRUE, f->cpm, TRUE ); break;
					case 2:
					{
						const Int p = f->prod, u = f->use;		// USED / PRODUCED
						v = txt( "f.pw", 0, 21, TRUE, ((long long)u << 32) | (UnsignedInt)p, [u, p]() { UnicodeString s; s.format( L"%d / %d", u, p ); return s; } );
						vc = f->deficit ? red() : white();
						break;
					}
					case 3:
					{
						const Int k = f->kills, d = f->losses;
						v = txt( "f.kd", 0, 21, TRUE, (k << 16) ^ d, [k, d]() {
							UnicodeString s;
							s.format( L"%.1f", d > 0 ? (Real)k / (Real)d : (Real)k );
							return s; } );
						break;
					}
					case 4: v = number( "f.units", 0, 21, TRUE, s_units[f->slot] ); break;
					case 5: v = number( "f.bldgs", 0, 21, TRUE, s_bldgs[f->slot] ); break;
					case 6:
					{
						const Int xp = f->xp, nx = f->xpNext;
						v = txt( "f.xp", 0, 21, TRUE, xp * 31 + nx, [xp, nx]() {
							UnicodeString s = formatGroupedValue( (UnsignedInt)max( 0, xp ) );
							if (nx > 0)
							{
								s.concat( L" / " );
								s.concat( formatGroupedValue( (UnsignedInt)nx ) );
							}
							else
								s.concat( L"  MAX" );
							return s; } );
						break;
					}
					case 7:
					{
						refreshBought( *f, FALSE );
						const Int sp = s_buySpent[f->slot], tot = s_buyTotal[f->slot];
						v = txt( "f.gp", 0, 21, TRUE, (sp << 16) ^ tot, [sp, tot]() { UnicodeString s; s.format( L"%d of %d", sp, tot ); return s; } );
						break;
					}
				}
				put( v, cx, vy, vc );
			}
		}
	}
	const Int focusTabX = home.x + S( 330 ) + s_dock.panel[PANEL_FOCUS].dx;
	dockTab( "tab.focus", L"FOCUS", focusTabX, tabY - S( 22 ), PANEL_FOCUS, !s_dock.panel[PANEL_FOCUS].open );
	// What was drawn, for the toast stack to keep off: the panel (open), its object band, its tab.
	s_focusDrawn = Rect{ focusTabX, tabY - S( 22 ), tw( fixed( "tab.focus", 10, TRUE, L"FOCUS" ) ) + S( 44 ), S( 20 ) };
	if (s_dock.panel[PANEL_FOCUS].open)
		s_focusDrawn = rectUnion( s_focusDrawn, r );
	s_focusDrawn = rectUnion( s_focusDrawn, drawnBand );
}

// ---------------------------------------------------------------------------------------------------------------------
// B3: the Tab scoreboard
// ---------------------------------------------------------------------------------------------------------------------
enum Col { C_STRIPE, C_PORTRAIT, C_PLAYER, C_GENERAL, C_CASH, C_INCOME, C_UNITS, C_BLDGS, C_KD, C_RANK, C_XP, C_POWER, C_STATUS, C_COUNT };
static const Int kColW[C_COUNT] = { 6, 36, 170, 120, 84, 96, 56, 56, 50, 84, 124, 84, 118 };
static const wchar_t *const kColHead[C_COUNT] = { L"", L"", L"PLAYER", L"GENERAL", L"CASH", L"INCOME/MIN", L"UNITS", L"BLDGS",
	L"K/D", L"RANK", L"XP", L"POWER USED", L"STATUS" };
static const Bool kColRight[C_COUNT] = { FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE, TRUE, TRUE, TRUE };

static Int s_hoverSlot = -1;
static Rect s_hoverAnchor = { 0, 0, 0, 0 };

static void drawBoard()
{
	Int colX[C_COUNT], x = S( 22 );
	for (Int c = 0; c < C_COUNT; ++c)
	{
		if (c == C_XP && !s_set.show[SHOW_XP])
		{
			colX[c] = -1;
			continue;
		}
		colX[c] = x;
		x += S( (Real)kColW[c] ) + S( 10 );
	}
	Int shown = 0;
	for (Int i = 0; i < s_rowCount; ++i)
		shown += visible( s_rows[i] ) ? 1 : 0;
	const Int rh = S( shown > 12 ? 38 : 44 ), headH = S( 28 ), teamH = S( 32 ), footH = S( 30 );
	const Int chrome = headH + max( 1, s_blockCount ) * teamH + footH;
	listPrepare( shown, rh, s_H - S( 48 ) - chrome, 0 );
	const Int sbW = s_listShown < shown ? S( 18 ) : 0;
	const Int W = x + S( 12 ) + sbW, H = chrome + s_listShown * rh;
	const Rect home = { (s_W - W) / 2, max( S( 40 ), (s_H - H) / 2 - S( 30 ) ), W, H };
	const Rect b = placed( home, PANEL_BOARD );
	plate( b, FALSE, TRUE );
	addHit( b, H_BODY, 0, PANEL_BOARD );
	if (sbW > 0)
		s_listBox = b;

	Int y = b.y + S( 8 );
	for (Int c = 0; c < C_COUNT; ++c)
		if (colX[c] >= 0 && kColHead[c][0])
		{
			DisplayString *h = txt( "b.head", c, 10, TRUE, 0, [c]() { return UnicodeString( kColHead[c] ); } );
			const Int cx = b.x + colX[c];
			if (kColRight[c])
				putR( h, cx + S( (Real)kColW[c] ), y, label() );
			else
				put( h, cx, y, label() );
		}
	y = b.y + headH;
	fill( b.x + S( 12 ), y - S( 2 ), b.w - S( 24 ), 1, steelDim() );

	Player *f = viewedPlayer();
	s_hoverSlot = -1;
	Int skip = s_listFirst, room = s_listShown;		// rows scrolled away above, rows still to draw
	for (Int block = 0; block < s_blockCount; ++block)
	{
		const Int blockRows = blockVisibleRows( block );
		if (sbW > 0 && (blockRows <= skip || room == 0))
		{
			skip -= min( skip, blockRows );
			continue;
		}
		// Team header: name on the left, totals on the right.
		Int cash = 0, units = 0, kills = 0;
		for (Int i = 0; i < s_rowCount; ++i)
			if (inBlock( s_rows[i], block ))
			{
				cash += s_rows[i].cash;
				units += s_units[s_rows[i].slot];
				kills += s_rows[i].kills;
			}
		fill( b.x + S( 6 ), y, b.w - S( 12 ) - sbW, teamH - S( 4 ), GameMakeColor( 111, 168, 240, 28 ) );
		const Int ty = y + (teamH - S( 4 ) - th( 12 )) / 2;
		put( blockTitle( block, -1 ), b.x + S( 18 ), ty, accent() );
		const long long k3 = ((long long)cash << 32) | ((long long)(units & 0xFFFF) << 16) | (kills & 0xFFFF);
		putR( txt( "b.tot", block, 12, TRUE, k3, [cash, units, kills]() {
			UnicodeString u;
			u.format( L"CASH %ls    UNITS %d    KILLS %d", formatGroupedValue( (UnsignedInt)max( 0, cash ) ).str(), units, kills );
			return u; } ), b.x + b.w - S( 20 ) - sbW, ty, white() );
		y += teamH;

		for (Int i = 0; i < s_rowCount; ++i)
		{
			const Row &r = s_rows[i];
			if (!inBlock( r, block ) || !visible( r ))
				continue;
			if (skip > 0)
			{
				--skip;
				continue;
			}
			if (room == 0)
				continue;
			--room;
			const Bool alive = r.status == STATUS_ALIVE;
			const Rect rr = { b.x + S( 6 ), y, b.w - S( 12 ) - sbW, rh };
			if (r.player == f)
			{
				fill( rr, GameMakeColor( 111, 168, 240, 45 ) );
				box( rr, accent() );
			}
			else if (mouseIn( rr ))
				fill( rr, GameMakeColor( 111, 168, 240, 22 ) );
			addHit( rr, H_ROW, r.slot, PANEL_BOARD );
			const Int cy = y + (rh - th( 15 )) / 2;
			const Color tc = alive ? white() : dim();
			fill( b.x + colX[C_STRIPE], y + S( 6 ), S( 5 ), rh - S( 12 ), r.color );
			if (r.portrait)
			{
				const Int pz = rh - S( 10 );
				const Int px = b.x + colX[C_PORTRAIT];
				TheDisplay->drawImage( r.portrait, px, y + S( 5 ), px + pz, y + S( 5 ) + pz, 0xFFFFFFFF,
					alive ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
			}
			Player *pl = r.player;
			put( txt( "b.name", r.slot, 15, TRUE, 0, [pl]() { return hudName( pl ); } ), b.x + colX[C_PLAYER], cy,
				alive ? r.textColor : dim() );
			const UnicodeString gen = r.general;
			put( txt( "b.gen", r.slot, 14, FALSE, 0, [gen]() { return gen; } ), b.x + colX[C_GENERAL], cy, alive ? label() : dim() );
			const Int rx = 0;
			(void)rx;
			putR( number( "b.cash", r.slot, 15, FALSE, r.cash, FALSE, TRUE ), b.x + colX[C_CASH] + S( (Real)kColW[C_CASH] ), cy, tc );
			if (alive)
			{
				putR( number( "b.cpm", r.slot, 14, FALSE, r.cpm, TRUE ), b.x + colX[C_INCOME] + S( (Real)kColW[C_INCOME] ), cy, label() );
				putR( number( "b.units", r.slot, 15, FALSE, s_units[r.slot] ), b.x + colX[C_UNITS] + S( (Real)kColW[C_UNITS] ), cy, tc );
				putR( number( "b.bldgs", r.slot, 15, FALSE, s_bldgs[r.slot] ), b.x + colX[C_BLDGS] + S( (Real)kColW[C_BLDGS] ), cy, tc );
			}
			else
			{
				putR( fixed( "b.dash", 15, FALSE, L"-" ), b.x + colX[C_INCOME] + S( (Real)kColW[C_INCOME] ), cy, dim() );
				putR( fixed( "b.dash", 15, FALSE, L"-" ), b.x + colX[C_UNITS] + S( (Real)kColW[C_UNITS] ), cy, dim() );
				putR( fixed( "b.dash", 15, FALSE, L"-" ), b.x + colX[C_BLDGS] + S( (Real)kColW[C_BLDGS] ), cy, dim() );
			}
			const Int k = r.kills, d = r.losses;
			putR( txt( "b.kd", r.slot, 15, FALSE, (k << 16) ^ d, [k, d]() {
				UnicodeString s;
				s.format( L"%.1f", d > 0 ? (Real)k / (Real)d : (Real)k );
				return s; } ), b.x + colX[C_KD] + S( (Real)kColW[C_KD] ), cy, tc );
			// Rank stars: hovering them shows what this general bought (owner, 05/10: bought only).
			const Int sz = S( 15 );
			const Rect sr = { b.x + colX[C_RANK] - S( 3 ), y + S( 4 ), max( sz * max( 1, r.rank ) + S( 6 ), S( 40 ) ), rh - S( 8 ) };
			drawStars( b.x + colX[C_RANK], y + (rh - sz) / 2, sz, r.rank );
			addHit( sr, H_STARS, r.slot, PANEL_BOARD );
			if (mouseIn( sr ) || r.slot == s_forceHover)
			{
				box( sr, accent() );
				s_hoverSlot = r.slot;
				s_hoverAnchor = sr;
			}
			if (colX[C_XP] >= 0)
			{
				const Int xp = r.xp, nx = r.xpNext;
				putR( txt( "b.xp", r.slot, 14, FALSE, xp * 31 + nx, [xp, nx]() {
					UnicodeString s = formatGroupedValue( (UnsignedInt)max( 0, xp ) );
					s.concat( L"/" );
					if (nx > 0)
						s.concat( formatGroupedValue( (UnsignedInt)nx ) );
					else
						s.concat( L"MAX" );
					return s; } ), b.x + colX[C_XP] + S( (Real)kColW[C_XP] ), cy, tc );
			}
			if (!alive)
				putR( fixed( "b.dash", 15, FALSE, L"-" ), b.x + colX[C_POWER] + S( (Real)kColW[C_POWER] ), cy, dim() );
			else if (!r.grid)
				putR( fixed( "b.nogrid", 14, FALSE, L"0/0" ), b.x + colX[C_POWER] + S( (Real)kColW[C_POWER] ), cy, dim() );
			else
			{
				const Int u = r.use, p = r.prod;		// USED / PRODUCED
				const Int right = b.x + colX[C_POWER] + S( (Real)kColW[C_POWER] );
				putR( txt( "b.pw", r.slot, 14, TRUE, ((long long)u << 32) | (UnsignedInt)p, [u, p]() { UnicodeString s; s.format( L"%d / %d", u, p ); return s; } ),
					right, cy, r.deficit ? red() : white() );
			}
			if (!alive)
			{
				const Bool left = r.status == STATUS_LEFT;
				const Int right = b.x + colX[C_STATUS] + S( (Real)kColW[C_STATUS] );
				Int tagRight = right;
				if (s_set.show[SHOW_STATUS_TIME] && s_statusFrame[r.slot] != 0)
				{
					const UnsignedInt fr = s_statusFrame[r.slot];
					DisplayString *t = txt( "b.time", r.slot, 11, FALSE, (Int)fr, [fr]() { return clockText( fr ); } );
					putR( t, right, cy + S( 2 ), dim() );
					tagRight = right - tw( t ) - S( 6 );
				}
				DisplayString *tag = left ? fixed( "tag.left", 13, TRUE, L"LEFT" ) : fixed( "tag.dead", 13, TRUE, L"DEAD" );
				const Rect t = { tagRight - tw( tag ) - S( 10 ), cy + S( 1 ), tw( tag ) + S( 12 ), th( 13 ) + S( 3 ) };
				fill( t, left ? GameMakeColor( 120, 86, 16, 230 ) : GameMakeColor( 120, 24, 24, 230 ) );
				box( t, left ? amber() : red() );
				put( tag, t.x + S( 6 ), t.y + S( 1 ), left ? amber() : red() );
			}
			y += rh;
		}
	}
	if (sbW > 0)
		drawListScroll( b.x + b.w - S( 16 ), b.y + headH, b.y + b.h - footH );
	// Footer.
	const Int fy = b.y + b.h - footH + (footH - th( 10 )) / 2;
	// The first hint says how THIS board closes: held on the key, or pinned open from its tab.
	put( s_dock.panel[PANEL_BOARD].open
			? fixed( "b.hintp", 10, TRUE, L"CLOSE TO HIDE   |   CLICK A ROW TO FOLLOW   |   HOVER THE STARS: WHAT THEY BOUGHT" )
			: fixed( "b.hint", 10, TRUE, L"RELEASE TAB TO CLOSE   |   CLICK A ROW TO FOLLOW   |   HOVER THE STARS: WHAT THEY BOUGHT" ),
		b.x + S( 22 ), fy, label() );
	const Int op = s_set.opacity;
	putR( txt( "b.op", 0, 10, TRUE, op, [op]() { UnicodeString u; u.format( L"PANEL OPACITY %d%%", op ); return u; } ),
		b.x + b.w - S( 22 ), fy, label() );

	dockTab( "tab.board", L"SCOREBOARD", b.x + S( 60 ), b.y - S( 21 ), PANEL_BOARD, s_dock.panel[PANEL_BOARD].open );
	DisplayString *cl = fixed( "tab.close", 10, TRUE, L"CLOSE" );
	const Rect cr = { b.x + b.w - S( 110 ), b.y - S( 21 ), tw( cl ) + S( 44 ), S( 20 ) };
	plate( cr, TRUE, TRUE );
	if (mouseIn( cr ))
		fill( cr, GameMakeColor( 111, 168, 240, 50 ) );
	putC( cl, cr.x + cr.w / 2, cr.y + (cr.h - th( 10 )) / 2, white() );
	arrow( cr.x + S( 12 ), cr.y + cr.h / 2, S( 4 ), TRUE, white() );
	arrow( cr.x + cr.w - S( 12 ), cr.y + cr.h / 2, S( 4 ), TRUE, white() );
	addHit( cr, H_CLOSE, 0, PANEL_BOARD );
}

static void drawHoverCard()
{
	const Row *r = nullptr;
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].slot == s_hoverSlot)
			r = &s_rows[i];
	// Purchases are private: anything but the spectator HUD gets its own card only, whatever slot a previous match or
	// panel left in s_hoverSlot (fail closed).
	// A player may also see what his ALLIES bought (owner 06/10) - never an enemy's: the lobby team decides, Row::ally.
	if (r == nullptr || (s_mode != MODE_SPECTATOR && !r->own && !(s_mode == MODE_PLAYER && r->ally && liveAlly( r->player ))))
		return;
	refreshBought( *r, TRUE );
	const Int w = S( 300 ), rowH = S( 26 );
	const Int h = S( 112 ) + max( 1, s_buyCount ) * rowH + S( 10 );
	Rect c = { s_hoverAnchor.x - S( 40 ), s_hoverAnchor.y + s_hoverAnchor.h + S( 10 ), w, h };
	if (c.y + c.h > s_H)
		c.y = s_hoverAnchor.y - S( 10 ) - c.h;
	if (c.x + c.w > s_W)
		c.x = s_W - c.w - S( 4 );
	if (c.y < 0)
		c.y = 0;
	fill( c, navy( 245 ) );
	box( c, steel() );
	addHit( c, H_BLOCK, 0, -1 );
	const Int pz = S( 52 );
	if (r->portrait)
		TheDisplay->drawImage( r->portrait, c.x + S( 14 ), c.y + S( 14 ), c.x + S( 14 ) + pz, c.y + S( 14 ) + pz );
	box( Rect{ c.x + S( 14 ), c.y + S( 14 ), pz, pz }, steel() );
	const Int tx = c.x + S( 14 ) + pz + S( 12 );
	Player *pl = r->player;
	put( txt( "h.name", r->slot, 18, TRUE, 0, [pl]() { return hudName( pl ); } ), tx, c.y + S( 14 ), r->textColor );
	const UnicodeString gen = r->general;
	const Bool rnd = r->randomGeneral;
	put( txt( "h.gen", r->slot, 12, FALSE, rnd, [gen, rnd]() { UnicodeString u = gen; if (!rnd) u.concat( L" General" ); return u; } ),
		tx, c.y + S( 14 ) + th( 18 ) + S( 2 ), label() );
	// an ally's rank is not in the private block (cleared for everyone but me): his own player object, read here for an ally
	drawStars( tx, c.y + S( 14 ) + th( 18 ) + th( 12 ) + S( 6 ), S( 13 ), (r->own || s_mode == MODE_SPECTATOR) ? r->rank : pl->getRankLevel() );
	putR( fixed( "h.ps", 10, TRUE, L"POINTS SPENT" ), c.x + w - S( 14 ), c.y + S( 16 ), label() );
	const Int sp = s_buySpent[r->slot], tot = s_buyTotal[r->slot];
	putR( txt( "h.pts", 0, 16, TRUE, (sp << 16) ^ tot, [sp, tot]() { UnicodeString s; s.format( L"%d of %d", sp, tot ); return s; } ),
		c.x + w - S( 14 ), c.y + S( 16 ) + th( 10 ) + S( 3 ), white() );
	Int y = c.y + S( 84 );
	put( fixed( "h.bought", 10, TRUE, L"BOUGHT" ), c.x + S( 14 ), y, label() );
	y += th( 10 ) + S( 6 );
	if (s_buyCount == 0)
		put( fixed( "h.none", 13, FALSE, L"Nothing bought yet" ), c.x + S( 14 ), y + S( 4 ), dim() );
	for (Int i = 0; i < s_buyCount; ++i)
	{
		fill( c.x + S( 14 ), y + (rowH - S( 8 )) / 2, S( 8 ), S( 8 ), accent() );
		const UnicodeString nm = s_buyNames[i];
		const Int slot = r->slot;
		put( txt( "h.item", i, 14, FALSE, (slot << 8) ^ s_buySpent[slot], [nm]() { return nm; } ), c.x + S( 30 ), y + (rowH - th( 14 )) / 2, white() );
		y += rowH;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The customise panel ("Spectator display", the gear)
// ---------------------------------------------------------------------------------------------------------------------
static void checkbox( Int x, Int y, Bool on, DisplayString *text, Int kind, Int id, Int w )
{
	const Int bs = S( 14 );
	const Rect b = { x, y + (S( 22 ) - bs) / 2, bs, bs };
	fill( b, navyDeep( 255 ) );
	box( b, on ? accent() : steelDim() );
	if (on)
		fill( b.x + S( 3 ), b.y + S( 3 ), bs - S( 6 ), bs - S( 6 ), accent() );
	put( text, x + bs + S( 8 ), y + (S( 22 ) - th( 13 )) / 2, on ? white() : dim() );
	const Rect hit = { x, y, w, S( 22 ) };
	addHit( hit, kind, id, PANEL_DISPLAY );
}

static Bool presetMatches( Preset p )
{
	Settings t = s_set;
	applyPreset( t, p );
	for (Int i = 0; i < SHOW_COUNT; ++i)
		if (t.show[i] != s_set.show[i])
			return FALSE;
	return TRUE;
}

static void drawDisplayPanel()
{
	const Int playerRows = (s_rowCount + 1) / 2;
	const Rect home = displayPanelHome( s_W, s_rowCount, s_scale );		// model-tested: the toast stack keeps off it
	const Rect r = placed( home, PANEL_DISPLAY );
	fill( r, navy( 240 ) );
	box( Rect{ r.x - 1, r.y - 1, r.w + 2, r.h + 2 }, edge() );
	box( r, steel() );
	addHit( r, H_BODY, 0, PANEL_DISPLAY );
	Int y = r.y + S( 16 );
	put( fixed( "d.title", 20, TRUE, L"Spectator display" ), r.x + S( 22 ), y, white() );
	putR( fixed( "d.saved", 10, TRUE, L"SAVED ON THIS PC" ), r.x + r.w - S( 22 ), y + S( 6 ), label() );
	y += th( 20 ) + S( 12 );
	static const wchar_t *const pn[3] = { L"FULL", L"CASTER", L"MINIMAL" };
	Int bx = r.x + S( 22 );
	for (Int p = 0; p < 3; ++p)
	{
		DisplayString *t = txt( "d.preset", p, 11, TRUE, 0, [p]() { return UnicodeString( pn[p] ); } );
		const Rect b = { bx, y, tw( t ) + S( 28 ), S( 24 ) };
		const Bool on = presetMatches( (Preset)p );
		fill( b, on ? GameMakeColor( 50, 96, 170, 255 ) : navyDeep( 255 ) );
		box( b, on ? accent() : steelDim() );
		putC( t, b.x + b.w / 2, b.y + (b.h - th( 11 )) / 2, white() );
		addHit( b, H_PRESET, p, PANEL_DISPLAY );
		bx += b.w + S( 8 );
	}
	DisplayString *rt = fixed( "d.reset", 11, TRUE, L"RESET" );
	const Rect rb = { r.x + r.w - S( 22 ) - tw( rt ) - S( 28 ), y, tw( rt ) + S( 28 ), S( 24 ) };
	fill( rb, navyDeep( 255 ) );
	box( rb, steelDim() );
	putC( rt, rb.x + rb.w / 2, rb.y + (rb.h - th( 11 )) / 2, white() );
	addHit( rb, H_RESET, 0, PANEL_DISPLAY );
	y += S( 36 );

	const Int colW = (r.w - S( 44 )) / 2, c0 = r.x + S( 22 ), c1 = c0 + colW + S( 10 );
	put( fixed( "d.cards", 10, TRUE, L"PLAYER CARDS" ), c0, y, label() );
	put( fixed( "d.panels", 10, TRUE, L"PANELS" ), c1, y, label() );
	y += th( 10 ) + S( 4 );
	static const Int cards[] = { SHOW_PORTRAITS, SHOW_NAMES, SHOW_CASH, SHOW_INCOME, SHOW_POWER, SHOW_UNITS_BUILDINGS };
	static const wchar_t *const cardText[] = { L"Portraits", L"Names", L"Cash", L"Income per minute", L"Power (produced / used)", L"Units and buildings" };
	static const Int panels[] = { SHOW_CLOCK_TOTALS, SHOW_EVENT_TOAST, SHOW_FOLLOW_PANEL, SHOW_MINIMAP, SHOW_STATUS_TIME, SHOW_TAB_BOARD, SHOW_XP };
	static const wchar_t *const panelText[] = { L"Clock and team cash", L"Event toast", L"Followed-player panel", L"Minimap",
		L"Dead / Left time stamp", L"Scoreboard on Tab", L"XP / next rank XP" };
	for (Int i = 0; i < (Int)ARRAY_SIZE( cards ); ++i)
		checkbox( c0, y + i * S( 22 ), s_set.show[cards[i]], txt( "d.card", i, 13, FALSE, 0, [i]() { return UnicodeString( cardText[i] ); } ),
			H_CHECK, cards[i], colW );
	for (Int i = 0; i < (Int)ARRAY_SIZE( panels ); ++i)
		checkbox( c1, y + i * S( 22 ), s_set.show[panels[i]], txt( "d.panel", i, 13, FALSE, 0, [i]() { return UnicodeString( panelText[i] ); } ),
			H_CHECK, panels[i], colW );
	y += (Int)ARRAY_SIZE( panels ) * S( 22 ) + S( 10 );

	// Players: hide all dead ones, or specific ones (owner, 05/10 point 13). Specific hides last for this match only:
	// names change from match to match.
	put( fixed( "d.players", 10, TRUE, L"PLAYERS" ), c0, y, label() );
	y += th( 10 ) + S( 4 );
	checkbox( c0, y, s_set.hideDead, fixed( "d.hidedead", 13, FALSE, L"Hide dead and left players" ), H_HIDEDEAD, 0,
		isReplay() ? colW : colW * 2 );
	if (isReplay())		// a live match has no recorded camera to follow
		checkbox( c1, y, followCameraOn(), fixed( "d.followcam", 13, FALSE, L"Follow the player's camera" ), H_FOLLOWCAM, 0, colW );
	y += S( 24 );
	for (Int i = 0; i < s_rowCount; ++i)
	{
		const Row &pr = s_rows[i];
		Player *pl = pr.player;
		const Int cx = (i % 2) ? c1 : c0, cy = y + (i / 2) * S( 22 );
		DisplayString *nm = txt( "d.p", pr.slot, 13, FALSE, 0, [pl]() { return hudName( pl ); } );
		checkbox( cx, cy, !s_hidden[pr.slot], nm, H_HIDEP, pr.slot, colW );
		fill( cx + S( 14 ) + S( 8 ) + tw( nm ) + S( 6 ), cy + S( 7 ), S( 8 ), S( 8 ), pr.color );
	}
	y += playerRows * S( 22 ) + S( 10 );

	// Opacity slider, 25..100.
	put( fixed( "d.op", 10, TRUE, L"PANEL OPACITY" ), c0, y, label() );
	const Int op = s_set.opacity;
	putR( txt( "d.opv", 0, 13, TRUE, op, [op]() { UnicodeString u; u.format( L"%d%%", op ); return u; } ), r.x + r.w - S( 22 ), y - S( 2 ), white() );
	y += th( 10 ) + S( 8 );
	const Rect track = { c0, y + S( 6 ), r.w - S( 44 ), S( 5 ) };
	fill( track, navyDeep( 255 ) );
	box( track, steelDim() );
	const Int kx = track.x + (track.w * (op - OPACITY_MIN)) / (OPACITY_MAX - OPACITY_MIN);
	fill( track.x, track.y, kx - track.x, track.h, accent() );
	fill( kx - S( 5 ), track.y - S( 6 ), S( 10 ), S( 17 ), white() );
	s_slider = track;
	const Rect sh = { track.x - S( 6 ), track.y - S( 10 ), track.w + S( 12 ), S( 25 ) };
	addHit( sh, H_SLIDER, 0, PANEL_DISPLAY );
	y += S( 22 );
	put( fixed( "d.op0", 10, FALSE, L"25% see-through" ), c0, y, dim() );
	putR( fixed( "d.op1", 10, FALSE, L"solid 100%" ), r.x + r.w - S( 22 ), y, dim() );
}

// ---------------------------------------------------------------------------------------------------------------------
// PLAYER MODE - the GeneralsX UI for a player still playing a versus game (owner's pick of 05/10/2026, layout "C"):
//   * the control bar's art gives way to these navy panels at the shared opacity; every retail button, cameo, the money
//     text, the power bar, the general's button and the superweapon buttons stay where they are, drawn on top;
//   * the retail radar WINDOW is resized to the map's own shape (fitRadar): the radar draws, clips its view box and maps
//     clicks, right-click orders and pings through that window's rect, so all of that keeps working untouched;
//   * the players bar (own team left, my card beside the clock) is folded into its tab until Alt+P or a click;
//   * an economy line on the command bar (PlayerShowStrip) and a player Tab board (PlayerShowTabBoard; Shift+Tab =
//     Diplomacy) - my own numbers in full, everyone else only as retail's Diplomacy screen shows him.
// Nothing here writes game state: window size and the art flag are display state, and the keys and clicks taken are
// the player's own raw input over these panels.
// ---------------------------------------------------------------------------------------------------------------------
enum { BW_COMMAND, BW_MONEY, BW_UPGRADES, BW_GENERAL, BW_RADAR, BW_RADARBLOCK, BW_COUNT };
static GameWindow *s_bw[BW_COUNT];
static Bool s_bwLooked[BW_COUNT];
static GXSpectatorHud::GeneralPoint s_gen;	///< the General's button pulse state (trackGeneralPoint)
static Rect s_radarHome = { 0, 0, 0, 0 };	///< the radar window's own rect (parent-relative), as the bar laid it out
static Rect s_radarSet = { 0, 0, 0, 0 };		///< what we resized it to; w == 0 = untouched

enum PAction { PA_BOARD, PA_BAR, PA_COUNT };		// no Diplomacy key: the board mutes (owner 06/10: "no need for diplomacy")
static Chord s_pchords[PA_COUNT];
static Bool s_pOwned[PA_COUNT];			///< this key's DOWN was ours: its up and repeats are ours too, whatever the modifiers
static Int s_forceLook = -1;					///< GX_PLAYERUI_SHOW: -1 the switch decides, 0 legacy, 1 player mode (tests only)
static Bool s_pBoardForced = FALSE;		///< GX_PLAYERUI_SHOW "board": the board as if Tab were held; the Communicator button latches it too
static Int s_pAllyHover = -1;				///< GX_PLAYERUI_SHOW allyhover=<slot> (tests only): that ally's row as if hovered
static Bool s_overlayHiddenNow = FALSE;	///< F11 has hidden the GeneralsX panels this frame (underWindows)
static Bool s_pHoverForced = FALSE;		///< GX_PLAYERUI_SHOW "hover": my own stars as if hovered
static Stage s_pstages[16];
static Bool s_fakeHuman[MAX_SLOTS];		///< GX_PLAYERUI_SHOW human=<slot;slot> (tests only): drawn and muted as if human
static Int s_pstageCount = 0, s_pstageNext = 0;
static UnsignedInt s_auditMs = 0;

static Bool sameRect( const Rect &a, const Rect &b ) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

/// ControlBar.wnd:ControlBarParent, the retail bar's only top-level window. Found by name among the top-level windows,
/// never through nameToKey: that would mint a NameKey on this client for a ControlBar.wnd without it (a mod).
static GameWindow *barWindow()
{
	if (s_barWindow == nullptr && TheWindowManager)
		for (GameWindow *w = TheWindowManager->winGetWindowList(); w != nullptr; w = w->winGetNext())
			if (w->winGetInstanceData()->m_decoratedNameString.compare( "ControlBar.wnd:ControlBarParent" ) == 0)
			{
				s_barWindow = w;
				break;
			}
	return s_barWindow;
}

/// A window under `parent` by its .wnd name, by walking the tree (no NameKey minted, as above).
static GameWindow *findChild( GameWindow *parent, const char *name )
{
	for (GameWindow *c = parent ? parent->winGetChild() : nullptr; c != nullptr; c = c->winGetNext())
	{
		if (c->winGetInstanceData()->m_decoratedNameString.compare( name ) == 0)
			return c;
		if (GameWindow *g = findChild( c, name ))
			return g;
	}
	return nullptr;
}

/// The bar's islands (ControlBar Pro names; a bar without one simply gets no panel there). Looked up once per match.
static GameWindow *barChild( Int i )
{
	static const char *const names[BW_COUNT] = { "ControlBar.wnd:CommandWindow", "ControlBar.wnd:MoneyDisplay",
		"ControlBar.wnd:ControlBarProBlockInputUpgrades", "ControlBar.wnd:ButtonGeneral", "ControlBar.wnd:LeftHUD",
		"ControlBar.wnd:ControlBarProBlockInputRadar" };
	if (!s_bwLooked[i] && barWindow() != nullptr)
	{
		s_bw[i] = findChild( s_barWindow, names[i] );
		s_bwLooked[i] = TRUE;
	}
	return s_bw[i];
}

static Rect screenRect( GameWindow *w )
{
	Rect r = { 0, 0, 0, 0 };
	if (w != nullptr)
	{
		w->winGetScreenPosition( &r.x, &r.y );
		w->winGetSize( &r.w, &r.h );
	}
	return r;
}

static Rect localRect( GameWindow *w )
{
	Rect r = { 0, 0, 0, 0 };
	w->winGetPosition( &r.x, &r.y );
	w->winGetSize( &r.w, &r.h );
	return r;
}

static void setLocalRect( GameWindow *w, const Rect &r )
{
	w->winSetPosition( r.x, r.y );
	w->winSetSize( r.w, r.h );
}

enum RadarFit { RADAR_HOME, RADAR_CORNER, RADAR_CENTRED };

/// The radar window in the map's own shape on the corner under the GeneralsX panels, or its home rect - the retail box,
/// for the legacy look (W3DRadar fills the margins with the map's dimmed out-of-bounds terrain) and at the end of a match
/// (unless someone else has moved it since: then it is theirs and stays where they put it).
static void fitRadarWindow( GameWindow *w, RadarFit how )
{
	if (w == nullptr)
		return;
	const Rect now = localRect( w );
	if (how == RADAR_HOME)
	{
		if (s_radarSet.w > 0 && sameRect( now, s_radarSet ))
			setLocalRect( w, s_radarHome );
		s_radarSet.w = 0;
		return;
	}
	if (s_radarSet.w == 0 || !sameRect( now, s_radarSet ))
		s_radarHome = now;		// first look, or the bar laid itself out again (a new resolution): its home now
	Int mw = 0, mh = 0;
	if (TheTerrainLogic)
	{
		Region3D ext;
		TheTerrainLogic->getExtent( &ext );
		mw = (Int)ext.width();
		mh = (Int)ext.height();
	}
	const Rect fit = how == RADAR_CENTRED ? fitRadarCentred( s_radarHome, mw, mh ) : fitRadar( s_radarHome, mw, mh );
	if (!sameRect( fit, now ))
		setLocalRect( w, fit );
	s_radarSet = fit;
}

/// The legacy look when the map does not fill the retail radar box (owner 06/10: "get rid of the black bars ... the map on its
/// own with the borders"): the bar art is cut out around the retail box (the baked frame stands around the box, not the map),
/// and a bevelled frame in the retail frame's own colours is drawn hugging the map instead; the world shows where the
/// box was. Nothing is cut or drawn when the map fills the box (a square map), so those pixels stay retail's.
static Rect s_blockHome = { 0, 0, 0, 0 }, s_blockSet = { 0, 0, 0, 0 };		///< the Pro bar's radar input blocker: its own rect / what we set (w == 0 = untouched)

/// Pro's input blocker over the radar keeps the retail square: left alone it would eat clicks on the world the hole
/// exposes. Resized to `target` (a screen rect), or put back (nullptr) unless someone else moved it.
static void fitRadarBlock( const Rect *target )
{
	GameWindow *b = barChild( BW_RADARBLOCK );
	if (b == nullptr)
		return;
	const Rect now = localRect( b );
	if (target == nullptr)
	{
		if (s_blockSet.w > 0 && sameRect( now, s_blockSet ))
			setLocalRect( b, s_blockHome );
		s_blockSet.w = 0;
		return;
	}
	if (s_blockSet.w == 0 || !sameRect( now, s_blockSet ))
		s_blockHome = now;
	const Rect scr = screenRect( b );
	const Rect t = { target->x - (scr.x - now.x), target->y - (scr.y - now.y), target->w, target->h };
	if (!sameRect( t, now ))
		setLocalRect( b, t );
	s_blockSet = t;
}

static Rect s_legacyMap = { 0, 0, 0, 0 };		///< the radar window's screen rect while the hugging frame is up; w == 0 = none

static void legacyRadarFrame( GameWindow *rw )
{
	s_legacyMap.w = 0;
	fitRadarBlock( nullptr );
	gxControlBarArtHole.lo.x = gxControlBarArtHole.lo.y = gxControlBarArtHole.hi.x = gxControlBarArtHole.hi.y = 0;
	if (rw == nullptr || s_radarSet.w <= 0 || sameRect( s_radarSet, s_radarHome ))
		return;
	const Rect now = localRect( rw );
	if (!sameRect( now, s_radarSet ))
		return;
	const Rect scr = screenRect( rw );
	const Int hx = scr.x - now.x + s_radarHome.x, hy = scr.y - now.y + s_radarHome.y;	// the home box on the screen
	const Int side = s_radarHome.h / 20, top = s_radarHome.h * 7 / 100;			// the baked frame's reach beyond the box (Pro scheme, 1080p: 14 / 22 px)
	gxControlBarArtHole.lo.x = hx - side;
	gxControlBarArtHole.lo.y = hy - top;
	gxControlBarArtHole.hi.x = hx + s_radarHome.w + side;
	gxControlBarArtHole.hi.y = hy + s_radarHome.h + side;
	s_legacyMap = scr;
	const Int ring = s_radarHome.h / 54 > 3 ? s_radarHome.h / 54 : 3;
	const Rect blk = { scr.x - ring, scr.y - ring, scr.w + 2 * ring, scr.h + 2 * ring };
	fitRadarBlock( &blk );
}

static void drawLegacyFrame()
{
	if (s_legacyMap.w <= 0)
		return;
	static const Int ring[6][3] = { { 10, 18, 24 }, { 122, 142, 159 }, { 79, 102, 115 }, { 72, 90, 103 }, { 52, 71, 88 }, { 124, 145, 154 } };
	const Int t = s_radarHome.h / 54 > 3 ? s_radarHome.h / 54 : 3;
	for (Int k = 0; k < t; ++k)
	{
		const Int *c = ring[k * 6 / t];
		const Color col = GameMakeColor( c[0], c[1], c[2], 255 );
		const Int g = k + 1;	// this layer's distance from the map's edge
		const Rect o = { s_legacyMap.x - g, s_legacyMap.y - g, s_legacyMap.w + 2 * g, s_legacyMap.h + 2 * g };
		fill( o.x, o.y, o.w, 1, col );
		fill( o.x, o.y + o.h - 1, o.w, 1, col );
		fill( o.x, o.y + 1, 1, o.h - 2, col );
		fill( o.x + o.w - 1, o.y + 1, 1, o.h - 2, col );
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The retail pop-ups that sit on the right of the screen - the General's promotion window (GeneralsExpPoints.wnd) and the
// superweapon shortcut tray (GenPowersShortcutBar*.wnd) - in the player look (owner 06/10: "make the UI update consistent
// too for the right side thing and the generals promotions"). Their grey-steel frames and header bars are one IMAGE per
// window, drawn by the default window draw only while WIN_STATUS_IMAGE is set; with the bit cleared the same window draws
// its own colour fill and border. So each such window is switched, for as long as the look is up, to a navy fill with a
// steel border (the bar's own panel colours) and put back exactly as it was afterwards. The command cells (ButtonRank*,
// ButtonCommand*), the XP bar and every text stay retail's: only frames and bars change, nothing is hidden or moved.
// ---------------------------------------------------------------------------------------------------------------------
struct ChromeSaved { Color color, border; Bool image; const GameWindow *win; };		// win: the window that was switched (a rebuilt layout has new ones)
static std::map<std::string, ChromeSaved> s_chrome;		///< by decorated .wnd name; only while the player look has them switched

/// The promotion window's plate is as see-through as the Tab board's (owner 06/10: "make the generals menu see through
/// more"): the board's own text-panel alpha, a touch less. Only the plate - the rank headers, the cells and the star counter
/// keep their own fills.
static Int chromeRootAlpha()
{
	const Int a = clamp( 0, s_set.opacity * 235 / 100, 255 );
	return max( (Int)TEXT_ALPHA_FLOOR, a ) - 32;
}

static Bool chromeRoot( const std::string &n )
{
	return n.compare( 0, 22, "GeneralsExpPoints.wnd:" ) == 0 || n.compare( 0, 20, "GenPowersShortcutBar" ) == 0;
}
static Bool chromeKeep( const std::string &n )
{
	const char *const keep[] = { "ButtonRank", "ButtonCommand", "ProgressBar" };
	for (size_t i = 0; i < ARRAY_SIZE( keep ); ++i)
	{
		const size_t at = n.find( ':' );
		if (n.compare( at == std::string::npos ? 0 : at + 1, strlen( keep[i] ), keep[i] ) == 0)
			return TRUE;
	}
	return FALSE;
}

/// What the fade-in transition (GameWindowTransitionsStyles.cpp FadeTransition::draw) fills a restyled promotion window with
/// instead of drawing its retail frame image: the plate's own colour; alpha 0 = this window is not restyled right now.
static Color chromeFadeColor( GameWindow *w )
{
	const std::map<std::string, ChromeSaved>::const_iterator it = s_chrome.find( w->winGetInstanceData()->m_decoratedNameString.str() );
	if (it == s_chrome.end() || it->second.win != w || w->winGetParent() != nullptr)
		return 0;
	return navy( chromeRootAlpha() );
}

static void chromeWalk( GameWindow *c, Bool on, Bool dump )		// this window, then its children (never its siblings)
{
	{
		const std::string name = c->winGetInstanceData()->m_decoratedNameString.str();
		const Bool image = BitIsSet( c->winGetStatus(), WIN_STATUS_IMAGE );
		if (dump && on)
		{
			ICoord2D p, z;
			c->winGetScreenPosition( &p.x, &p.y );
			c->winGetSize( &z.x, &z.y );
			fprintf( stderr, "[GXCHROME] %s at %d,%d %dx%d image=%d img0=%s hidden=%d keep=%d\n", name.c_str(), p.x, p.y, z.x, z.y, image ? 1 : 0,
				c->winGetEnabledImage( 0 ) ? c->winGetEnabledImage( 0 )->getName().str() : "-", c->winIsHidden() ? 1 : 0, chromeKeep( name ) ? 1 : 0 );
		}
		std::map<std::string, ChromeSaved>::iterator it = s_chrome.find( name );
		if (it != s_chrome.end() && it->second.win != c)
		{
			s_chrome.erase( it );		// a snapshot of a window that is gone (layout rebuilt): never put onto its replacement
			it = s_chrome.end();
		}
		if (on && it != s_chrome.end() && c->winGetParent() == nullptr && name.compare( 0, 22, "GeneralsExpPoints.wnd:" ) == 0)
			c->winSetEnabledColor( 0, navy( chromeRootAlpha() ) );		// follows the opacity setting
		if (on)
		{
			if (it == s_chrome.end() && image && c->winGetEnabledImage( 0 ) != nullptr && !chromeKeep( name ))
			{
				const ChromeSaved sv = { c->winGetEnabledColor( 0 ), c->winGetEnabledBorderColor( 0 ), TRUE, c };
				s_chrome[name] = sv;
				c->winClearStatus( WIN_STATUS_IMAGE );
				const Bool root = c->winGetParent() == nullptr;
				// The tray's frame window (ButtonParentN) is far larger than the icon cell inside it: it draws nothing at all, and
				// drawTrayFrames() puts a thin frame right round the cell instead (owner 06/10: "what's this XDD").
				const Bool tray = name.find( ":ButtonParent" ) != std::string::npos;
				c->winSetEnabledColor( 0, tray ? WIN_COLOR_UNDEFINED : root ? navy( chromeRootAlpha() ) : navyDeep( 222 ) );
				c->winSetEnabledBorderColor( 0, tray ? WIN_COLOR_UNDEFINED : root ? steel() : steelDim() );
			}
		}
		else if (it != s_chrome.end())
		{
			c->winSetStatus( WIN_STATUS_IMAGE );
			c->winSetEnabledColor( 0, it->second.color );
			c->winSetEnabledBorderColor( 0, it->second.border );
			s_chrome.erase( it );
		}
	}
	for (GameWindow *k = c->winGetChild(); k != nullptr; k = k->winGetNext())
		chromeWalk( k, on, dump );
}

/// Every frame, before the windows paint: `on` = the player look is up. Roots are found by name among the top-level windows.
static void applyChrome( Bool on )
{
	if (!on && s_chrome.empty())
		return;
	static const Bool dump = getenv( "GX_PLAYERUI_CHROME_DUMP" ) != nullptr;
	static UnsignedInt dumpedAt = 0;
	const UnsignedInt now = timeGetTime();
	const Bool doDump = dump && now - dumpedAt > 4000;
	Bool any = FALSE;
	for (GameWindow *w = TheWindowManager ? TheWindowManager->winGetWindowList() : nullptr; w != nullptr; w = w->winGetNext())
		if (chromeRoot( w->winGetInstanceData()->m_decoratedNameString.str() ))
		{
			any = TRUE;
			if (doDump && !w->winIsHidden())
				fprintf( stderr, "[GXCHROME] ---- root %s\n", w->winGetInstanceData()->m_decoratedNameString.str() );
			chromeWalk( w, on, doDump && !w->winIsHidden() );
		}
	if (doDump && any)
		dumpedAt = now;
}

/// The bar's look for THIS frame's repaint (set before the windows paint): skin = the GeneralsX panels (no art, the radar
/// on the corner); otherwise the retail art with the radar centred in its box. Both draw the map in its own shape.
static void applyLook( Bool skin )
{
	gxControlBarArtOff = skin;
	// The same window shows a briefing/campaign movie, stretched over its rectangle (W3DLeftHUDDraw): while one plays it
	// keeps the retail rect, and the map's shape resumes after.
	const Bool movie = TheInGameUI != nullptr && TheInGameUI->videoBuffer() != nullptr;
	GameWindow *rw = barChild( BW_RADAR );
	// The hug needs the world to show where the bar's frame was: only with a tactical view as tall as the screen (Pro's
	// full viewport). With the stock 80% view the exposed strip would be the black clear colour: the retail box then, its
	// margins W3DRadar's own.
	const Bool worldBehind = TheTacticalView != nullptr && TheDisplay != nullptr && TheTacticalView->getHeight() >= TheDisplay->getHeight() - 4;
	// The legacy look (the switch off) keeps the radar exactly as retail lays it out: its own box inside the bar's own frame,
	// black margins and all (owner 21/10/2026, a legacy screenshot: "These shouldn't change if the UI is legacy" - newer than,
	// and replacing, his 06/10 legacy asks this hug served). The hug stays for the GeneralsX UI's legacy-looking moments
	// (F11, a campaign), which that request did not touch.
	const Bool hug = !skin && !movie && worldBehind && s_set.generalsx;
	fitRadarWindow( rw, movie ? RADAR_HOME : skin ? RADAR_CORNER : hug ? RADAR_CENTRED : RADAR_HOME );
	legacyRadarFrame( hug ? rw : nullptr );
	applyChrome( skin );
}

/// The display flag and pointers are this client's own; reset() runs on the way to the shell, where the bar may already
/// have been rebuilt - so the radar is put back by a fresh walk of the live window list, never through a cached pointer.
static void resetSkin()
{
	gxControlBarArtOff = FALSE;
	s_gen.reset();		// the next match's first update picks up retail's flag (ControlBar::reset sets it)
	legacyRadarFrame( nullptr );		// also puts the Pro input blocker back
	applyChrome( FALSE );
	s_chrome.clear();
	s_barWindow = nullptr;
	if (s_radarSet.w > 0)
		fitRadarWindow( findChild( barWindow(), "ControlBar.wnd:LeftHUD" ), RADAR_HOME );
	s_barWindow = nullptr;
	s_radarSet.w = 0;
	for (Int i = 0; i < BW_COUNT; ++i)
	{
		s_bw[i] = nullptr;
		s_bwLooked[i] = FALSE;
	}
}

/// The launcher's GENERAL rows MetaEvent.cpp catches in FRONT of the meta map (gxLoadGeneralKeys: select infantry, select
/// all, Remastered) - not in MetaMapRec, so read here from the same file with the same defaults.
static Bool generalChordTaken( Int k, Int m )
{
	static const char *const keys[] = { "GXSelectInfantryKey", "GXSelectAllKey",
#if GX_REMASTERED_UI
		"GXRemasteredKey",
#endif
	};
	static const char *const defaults[] = { "CTRL+KEY_I", "ALT+KEY_I",
#if GX_REMASTERED_UI
		"SHIFT_CTRL+KEY_R",
#endif
	};
	UserPreferences prefs;
	const Bool have = prefs.load( "CommandButtonHotKeys.ini" );
	for (size_t i = 0; i < ARRAY_SIZE( keys ); ++i)
	{
		AsciiString value;
		if (have)
		{
			value = prefs.getAsciiString( keys[i], AsciiString::TheEmptyString );
			value.trim();
		}
		std::string mod, key;
		Int gk = 0, gm = 0;
		// MetaEvent's own parse: a value it cannot read leaves the default in force, so test the default then.
		// MetaEvent's gxParseChord, exactly: split at the first '+', NO trimming of either half (so "CTRL +KEY_O" is
		// refused there and the default stays), a modifier that is not NONE and a key that is not KEY_NONE.
		std::string v = value.str();
		for (size_t c = 0; c < v.size(); ++c)
			v[c] = (char)std::toupper( (unsigned char)v[c] );		// the engine's lookup is case-blind (stricmp)
		const size_t plus = v.find( '+' );
		const Bool ok = plus != std::string::npos && plus != 0 && plus < 32
			&& lookupName( ModifierNames, v.substr( 0, plus ), &gm ) && gm != 0
			&& lookupName( KeyNames, v.substr( plus + 1 ), &gk ) && gk != 0;
		if (!ok && !(parseChord( defaults[i], mod, key ) && lookupName( KeyNames, key, &gk ) && lookupName( ModifierNames, mod, &gm )))
			continue;
		if (gk == k && gm == m)
			return TRUE;
	}
	// GXCameraResetKey (MetaEvent.cpp, caught in front of the meta map too): a MODIFIED chord set there is taken. Its default
	// and any bare navigation key carry no modifier, so they can never equal a spectator or player chord.
	if (have)
	{
		AsciiString value = prefs.getAsciiString( "GXCameraResetKey", AsciiString::TheEmptyString );
		value.trim();
		std::string v = value.str();
		for (size_t c = 0; c < v.size(); ++c)
			v[c] = (char)std::toupper( (unsigned char)v[c] );
		const size_t plus = v.find( '+' );
		Int gk = 0, gm = 0;
		if (plus != std::string::npos && plus != 0 && plus < 32
			&& lookupName( ModifierNames, v.substr( 0, plus ), &gm ) && gm != 0
			&& lookupName( KeyNames, v.substr( plus + 1 ), &gk ) && gk != 0 && gk == k && gm == m)
			return TRUE;
	}
	return FALSE;
}

/// Player keys (CommandButtonHotKeys.ini, the Keybinds page's "While playing" rows). They are caught in FRONT of the
/// meta map, so one may never shadow an in-match binding: a chord needs a modifier (playerChordAllowed - bare Tab
/// excepted, retail Diplomacy's key, which the board takes over: GeneralsX mode has no Diplomacy window, its Tab board
/// mutes - owner 06/10), and a chord the active CommandMap already binds in a
/// match is skipped (retail binds Alt+0..9 to "view team"). A value that fails either test falls back to the default,
/// which is tested the same way.
static void loadPlayerChords()
{
	static const char *const keys[PA_COUNT] = { "GXPlayBoardKey", "GXPlayBarKey" };
	static const char *const defaults[PA_COUNT] = { "KEY_TAB", "ALT+KEY_P" };
	UserPreferences prefs;
	const Bool have = prefs.load( "CommandButtonHotKeys.ini" );
	for (Int i = 0; i < PA_COUNT; ++i)
	{
		AsciiString value;
		if (have)
		{
			value = prefs.getAsciiString( keys[i], AsciiString::TheEmptyString );
			value.trim();
		}
		Chord &c = s_pchords[i];
		c.valid = FALSE;
		for (Int pass = value.isEmpty() ? 1 : 0; pass < 2 && !c.valid; ++pass)
		{
			const std::string text = pass == 0 ? std::string( value.str() ) : std::string( defaults[i] );
			std::string mod, key;
			Int k = 0, m = 0;
			if (!parseChord( text, mod, key ) || !playerChordAllowed( mod, key ) || !lookupName( KeyNames, key, &k )
				|| !(mod.empty() || lookupName( ModifierNames, mod, &m )) || (m == 0 && k != KEY_TAB))
			{
				fprintf( stderr, "[GXPUI] %s = '%s' ignored - a player key needs a modifier (Tab alone excepted)\n", keys[i], text.c_str() );
				continue;
			}
			const MetaMapRec *clash = nullptr;
			for (const MetaMapRec *r = TheMetaMap ? TheMetaMap->getFirstMetaMapRec() : nullptr; r != nullptr && clash == nullptr; r = r->m_next)
				if (r->m_key == k && (Int)r->m_modState == m && (r->m_usableIn & COMMANDUSABLE_GAME) != 0
					&& r->m_meta != GameMessage::MSG_META_DIPLOMACY)	// Diplomacy is the one the board takes over on purpose
					clash = r;
			if (clash == nullptr && generalChordTaken( k, m ))
			{
				fprintf( stderr, "[GXPUI] %s = '%s' ignored - a GeneralsX key (Keybinds > GENERAL) already uses it\n", keys[i], text.c_str() );
				continue;
			}
			if (clash != nullptr)
			{
				fprintf( stderr, "[GXPUI] %s = '%s' ignored - the game already binds it in a match (meta %d)\n", keys[i], text.c_str(),
					(Int)clash->m_meta );
				continue;
			}
			c.key = k;
			c.mods = m;
			c.valid = TRUE;
			fprintf( stderr, "[GXPUI] key %s = %s\n", keys[i], text.c_str() );		// once per match: which chord is in force
		}
	}
}

/// GX_PLAYERUI_SHOW (tests and screenshots only, like GX_SPECHUD_SHOW): stages "<frame>:<list>" separated by '/', list =
/// L (the legacy look) or C (player mode), bar (players bar open), board (as if Tab were held), hover (my stars),
/// click=<x;y> / rclick=<x;y> (one raw click at that pixel: the board gate's test), human=<slot;slot> (draw and mute those
/// AI rows as humans, for a picture of the MUTE buttons in an all-AI match). It can only choose between the two LOOKS a
/// player may have; it never makes a player a spectator.
static void loadPlayerStages()
{
	s_pstageCount = s_pstageNext = 0;
	s_forceLook = -1;
	s_pBoardForced = s_pHoverForced = FALSE, s_pAllyHover = -1;
	const char *e = getenv( "GX_PLAYERUI_SHOW" );
	s_puiTest = e != nullptr;
	if (e == nullptr)
		return;
	const std::string all( e );
	size_t pos = 0;
	while (pos < all.size() && s_pstageCount < (Int)ARRAY_SIZE( s_pstages ))
	{
		size_t end = all.find( '/', pos );
		if (end == std::string::npos)
			end = all.size();
		const std::string st = all.substr( pos, end - pos );
		const size_t colon = st.find( ':' );
		s_pstages[s_pstageCount].frame = colon == std::string::npos ? 0 : (UnsignedInt)atoi( st.c_str() );
		s_pstages[s_pstageCount].list = colon == std::string::npos ? st : st.substr( colon + 1 );
		++s_pstageCount;
		pos = end + 1;
	}
	fprintf( stderr, "[GXPUI] TEST GX_PLAYERUI_SHOW=%s (%d stages)\n", e, s_pstageCount );
}

/// GX_PLAYERUI_SHOW click=<x;y> / rclick=<x;y> (tests only): a raw press and release at that pixel, appended to the message
/// stream exactly as Mouse::createStreamMessages builds them, so it meets every translator a real click meets.
static void injectClick( const std::string &xy, Bool right )
{
	const size_t semi = xy.find( ';' );
	if (semi == std::string::npos || TheMessageStream == nullptr)
		return;
	ICoord2D at;
	at.x = atoi( xy.c_str() );
	at.y = atoi( xy.c_str() + semi + 1 );
	const GameMessage::Type types[2] = { right ? GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN : GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN,
		right ? GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP : GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP };
	for (Int i = 0; i < 2; ++i)
	{
		GameMessage *m = TheMessageStream->appendMessage( types[i] );
		m->appendPixelArgument( at );
		m->appendIntegerArgument( 0 );
		m->appendIntegerArgument( (Int)timeGetTime() );
	}
	fprintf( stderr, "[GXPUI] TEST %s at %d,%d f=%u\n", right ? "rclick" : "click", at.x, at.y, TheGameLogic->getFrame() );
}

/// GX_PLAYERUI_SHOW clickocc (tests only, the occupants fairness picture): a real left click on the ENEMY container nearest the
/// screen centre that this player could click himself - on screen, clear of the bars, not under his shroud, and the drawable a
/// click there picks (pickDrawable: nothing stealthed or unselectable). It never reads what is inside (spectator-occupants
/// GX_PLAYERUI_SHOW is an environment variable a player can set, so a hook that ranked or logged by contents was an
/// enemy-contents leak); an empty container proves the same thing, since the spectator card shows even "0 / 5". "[GXPUI] TEST
/// clickocc" names the object it clicked, or obj=0.
struct OccPick { const Object *best; Int dist; ICoord2D at; const Player *me; };
static void pickEnemyOcc( Object *obj, void *userData )
{
	OccPick *p = (OccPick *)userData;
	Drawable *dr = obj->getDrawable();
	if (obj->isEffectivelyDead() || dr == nullptr || !obj->isSelectable() || dr->getFullyObscuredByShroud() || p->me == nullptr
		|| obj->getTeam() == nullptr || p->me->getRelationship( obj->getTeam() ) != ENEMIES || occupantKind( containFacts( obj ) ) == OCC_NONE)
		return;
	ICoord2D sp;
	if (TheTacticalView == nullptr || !TheTacticalView->worldToScreen( obj->getPosition(), &sp )
		|| sp.x < S( 40 ) || sp.y < S( 60 ) || sp.x >= s_W - S( 40 ) || sp.y >= s_H - S( 300 ))
		return;
	const Int dist = abs( sp.x - s_W / 2 ) + abs( sp.y - s_H / 2 );
	if ((p->best != nullptr && dist >= p->dist) || TheTacticalView->pickDrawable( &sp, FALSE, PICK_TYPE_SELECTABLE ) != dr)
		return;
	p->best = obj;
	p->dist = dist;
	p->at = sp;
}
static void clickEnemyContainer()
{
	OccPick p = { nullptr, 0, { 0, 0 }, ThePlayerList != nullptr ? ThePlayerList->getLocalPlayer() : nullptr };
	for (Int i = 0; ThePlayerList != nullptr && i < ThePlayerList->getPlayerCount(); ++i)
		ThePlayerList->getNthPlayer( i )->iterateObjects( pickEnemyOcc, &p );
	if (p.best != nullptr)
	{
		char xy[40];
		snprintf( xy, sizeof( xy ), "%d;%d", p.at.x, p.at.y );
		injectClick( xy, FALSE );
	}
	fprintf( stderr, "[GXPUI] TEST clickocc obj=%u tmpl=%s at=%dx%d f=%u\n", p.best != nullptr ? (unsigned)p.best->getID() : 0u,
		p.best != nullptr ? p.best->getTemplate()->getName().str() : "-", p.at.x, p.at.y, TheGameLogic->getFrame() );
}

static void applyPlayerStage( const std::string &list )
{
	s_dock.reset();
	s_pBoardForced = s_pHoverForced = FALSE, s_pAllyHover = -1;
	for (Int i = 0; i < MAX_SLOTS; ++i)
		s_fakeHuman[i] = FALSE;
	size_t pos = 0;
	while (pos <= list.size())
	{
		size_t comma = list.find( ',', pos );
		if (comma == std::string::npos)
			comma = list.size();
		const std::string item = trim( list.substr( pos, comma - pos ) );
		if (item == "L") s_forceLook = 0;
		else if (item == "C") s_forceLook = 1;
		else if (item == "bar") s_dock.panel[PANEL_PLAYERS].open = true;
		else if (item == "board") s_pBoardForced = TRUE;
		else if (item == "hover") s_pHoverForced = TRUE;
		else if (item.compare( 0, 10, "listfirst=" ) == 0) s_listFirst = atoi( item.c_str() + 10 ), s_listDrawnMs = timeGetTime();
		else if (item.compare( 0, 9, "barfirst=" ) == 0) s_barFirst[0] = s_barFirst[1] = atoi( item.c_str() + 9 );
		else if (item.compare( 0, 10, "allyhover=" ) == 0) s_pAllyHover = atoi( item.c_str() + 10 );
		else if (item.compare( 0, 6, "click=" ) == 0) injectClick( item.substr( 6 ), FALSE );
		else if (item.compare( 0, 9, "clickwin=" ) == 0)		// the centre of a bar window by its .wnd name (tests only)
		{
			const Rect cr = screenRect( findChild( barWindow(), item.c_str() + 9 ) );
			char xy[40];
			snprintf( xy, sizeof( xy ), "%d;%d", cr.x + cr.w / 2, cr.y + cr.h / 2 );
			if (cr.w > 0)
				injectClick( xy, FALSE );
		}
		else if (item.compare( 0, 7, "rclick=" ) == 0) injectClick( item.substr( 7 ), TRUE );
		else if (item == "clickocc") clickEnemyContainer();
		else if (item.compare( 0, 6, "human=" ) == 0)
			for (size_t q = 6; q < item.size(); q = item.find( ';', q ) == std::string::npos ? item.size() : item.find( ';', q ) + 1)
			{
				const Int sl = atoi( item.c_str() + q );
				if (sl >= 0 && sl < MAX_SLOTS)
					s_fakeHuman[sl] = TRUE;
			}
		pos = comma + 1;
	}
	fprintf( stderr, "[GXPUI] TEST stage f=%u '%s'\n", TheGameLogic->getFrame(), list.c_str() );
}

/// GX_PLAYERUI_AUDIT=1 (tests only): once a second, checks the rows a PLAYER's panels draw from - leaks = rows of another
/// player carrying any private number. It reads the rows and nothing else: an audit that compared them with the real
/// values would itself print what the HUD refuses to draw (a cash-above-zero count told a 1v1
/// player his opponent's balance). The rule's red arm is the model test; this proves the shipped gather obeys it.
static void auditFairness()
{
	static const Bool on = getenv( "GX_PLAYERUI_AUDIT" ) != nullptr && strcmp( getenv( "GX_PLAYERUI_AUDIT" ), "1" ) == 0;
	const UnsignedInt now = timeGetTime();
	if (!on || now - s_auditMs < 1000)
		return;
	s_auditMs = now;
	const PrivateStats zero;
	Int rows = 0, leaks = 0;
	for (Int i = 0; i < s_rowCount; ++i)
		if (!s_rows[i].own)
		{
			++rows;
			if (memcmp( static_cast<const PrivateStats *>( &s_rows[i] ), &zero, sizeof( zero ) ) != 0)
				++leaks;
		}
	fprintf( stderr, "[GXPUI] audit f=%u rows=%d leaks=%d\n", TheGameLogic->getFrame(), rows, leaks );
}

static const Row *ownRow()
{
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].own)
			return &s_rows[i];
	return nullptr;
}

/// The bar's islands as navy panels, drawn before the window manager paints the bar, so every retail window sits on top.
/// The radar's panel says NO RADAR when retail draws no radar (rts::localPlayerHasRadar, as W3DLeftHUDDraw).
/// The economy row's rect: level with the $ box, or - on a bar with none - just above the command window.
static Rect ownRowRect( const Rect &cw, const Rect &mo )
{
	return mo.w > 0 ? Rect{ cw.x, mo.y, cw.w, mo.h } : Rect{ cw.x, cw.y - S( 34 ), cw.w, S( 30 ) };
}

static void drawBarPlates()
{
	const Rect cw = screenRect( barChild( BW_COMMAND ) ), up = screenRect( barChild( BW_UPGRADES ) );
	const Rect mo = screenRect( barChild( BW_MONEY ) );
	const Rect ra = screenRect( barChild( BW_RADAR ) );
	if (cw.w > 0)
	{
		// The command window's plate starts BELOW the economy row (owner 05/10 via wh1ter0se on Windows: the row sat on the
		// plate's top border, its frame line running through the row's panels). Only the plate moves; the retail buttons
		// stay where the bar put them, and none of them is that high.
		Rect p = cw;
		if (s_mode == MODE_PLAYER && mo.w > 0)
		{
			const Rect row = ownRowRect( cw, mo );
			const Int top = row.y + row.h - S( 1 );		// the row and the plate share one border line
			if (top > p.y && top < p.y + p.h)
			{
				p.h -= top - p.y;
				p.y = top;
			}
		}
		plate( p );
	}
	if (up.w > 0)
		plate( up );
	if (mo.w > 0)
		plate( Rect{ mo.x - S( 8 ), mo.y, mo.w + S( 16 ), mo.h }, TRUE, TRUE );
	// No plate under the General's button: drawGeneralButton() paints the whole button over it after the windows, and a socket
	// here, offset up and left of that rect, showed round it as a second, older frame (wh1ter0se 09/10 on 0.9.61).
	if (ra.w > 0)
	{
		plate( Rect{ ra.x - S( 6 ), ra.y - S( 6 ), ra.w + S( 12 ), ra.h + S( 12 ) } );
		if (!rts::localPlayerHasRadar())
			putC( fixed( "pm.noradar", 11, TRUE, L"NO RADAR" ), ra.x + ra.w / 2, ra.y + ra.h / 2 - th( 11 ) / 2, dim() );
	}
}

static UnicodeString xpText( Int xp, Int next )
{
	UnicodeString s = formatGroupedValue( (UnsignedInt)max( 0, xp ) );
	if (next > 0)
	{
		s.concat( L" / " );
		s.concat( formatGroupedValue( (UnsignedInt)next ) );
	}
	else
		s.concat( L"  MAX" );
	return s;
}

/// My economy on ONE row with the $ box (owner 05/10/2026: "I dont like how the money thing is going under the bar make it
/// align better"): INCOME and POWER to its left, ARMY and XP to its right, the same height and plate as the money's, so
/// the money sits IN the line instead of under it. A bar with no money box gets the row on top of the command window.
static void drawOwnStrip( const Row &me )
{
	const Rect cw = screenRect( barChild( BW_COMMAND ) ), mo = screenRect( barChild( BW_MONEY ) );
	if (cw.w <= 0)
		return;
	const Bool money = mo.w > 0;
	const Rect row = ownRowRect( cw, mo );
	// The three panels SNAP: the side blocks butt against the $ box's plate and share its border line, instead of three
	// separate bordered boxes with gaps (owner 06/10: "remove the excess borders to make the Units block snap").
	const Int midL = money ? mo.x - S( 8 ) + 1 : row.x + row.w / 2, midR = money ? mo.x + mo.w + S( 8 ) - 1 : midL;
	const Rect halves[2] = { { row.x, row.y, midL - row.x, row.h }, { midR, row.y, row.x + row.w - midR, row.h } };
	for (Int i = 0; i < 2; ++i)
	{
		plate( halves[i], TRUE, TRUE );
		addHit( halves[i], H_BLOCK, 0, -1 );
	}
	// ONE shared baseline for every label and value (owner 06/10: "the values are not aligned with their text"): the 16 px
	// values are centred in the row and each 11 px label sits on the value's baseline - bottoms of the two text boxes
	// aligned, less the 1 px the larger face's deeper descender adds.
	const Int vy = row.y + (row.h - th( 16 )) / 2, ly = vy + th( 16 ) - th( 11 ) - S( 1 );
	// Left block: INCOME, POWER. Right block: UNITS, BUILDINGS, XP - whole words, no abbreviations (owner 06/10: "bldgs is
	// annoying"). Digits are tabular in this face, so the figures do not jitter as they change.
	static const wchar_t *const heads[5] = { L"INCOME", L"POWER USED", L"UNITS", L"BUILDINGS", L"XP" };
	static const char *const ids[5] = { "ps.h0", "ps.h1", "ps.h2", "ps.h3", "ps.h4" };
	// UNITS and BUILDINGS are gone for a player; INCOME shows in sandbox play only (sandboxMatch). The cells that are left
	// share their half evenly: POWER | XP in a real match, INCOME POWER | XP in a sandbox.
	const Bool showIncome = sandboxMatch();
	const auto active = [showIncome]( Int j ) { return j == 1 || j == 4 || (j == 0 && showIncome); };
	for (Int i = 0; i < 5; ++i)
	{
		if (!active( i ))
			continue;
		const Int block = i < 2 ? 0 : 1;
		Int nth = 0, cells = 0;
		for (Int j = 0; j < 5; ++j)
			if (active( j ) && (j < 2 ? 0 : 1) == block)
			{
				if (j < i)
					++nth;
				++cells;
			}
		const Rect &h = halves[block];
		const Int cellW = h.w / cells, x0 = h.x + nth * cellW;
		if (nth > 0)
			fill( x0, row.y + S( 5 ), 1, row.h - S( 10 ), steelDim() );
		const Int cx = x0 + S( 10 );
		// Short of power (the engine's own test, Energy::hasSufficientPower - also true while the grid is sabotaged): the
		// label itself reads OFFLINE in red (owner 06/10), the figure beside it stays red as before. Own row only.
		const Bool offline = i == 1 && me.deficit;
		DisplayString *hd = offline ? fixed( "ps.h1off", 11, TRUE, L"OFFLINE" ) : fixed( ids[i], 11, TRUE, heads[i] );
		put( hd, cx, ly, offline ? red() : label() );
		const Int vx = cx + tw( hd ) + S( 7 );
		if (i == 0)
		{
			DisplayString *v = number( "ps.cpm", 0, 16, TRUE, me.cpm, TRUE );
			put( v, vx, vy, white() );
			put( fixed( "ps.pm", 11, TRUE, L"/MIN" ), vx + tw( v ) + S( 3 ), ly, label() );
		}
		else if (i == 1)
		{
			// USED / PRODUCED, the way it is read aloud: "4 used out of 20" (owner 06/10: "why 20/4 more like 4 used out of 20
			// no?"). The whole figure goes red when the base uses more than it makes.
			if (!me.grid)
				put( fixed( "ps.nogrid", 16, TRUE, L"0 / 0" ), vx, vy, dim() );
			else
			{
				const Int u = me.use, p = me.prod;
				put( txt( "ps.pw", 0, 16, TRUE, ((long long)u << 32) | (UnsignedInt)p, [u, p]() { UnicodeString s; s.format( L"%d / %d", u, p ); return s; } ),
					vx, vy, me.deficit ? red() : white() );
			}
		}
		else if (i == 2)
			put( number( "ps.units", 0, 16, TRUE, me.units ), vx, vy, white() );
		else if (i == 3)
			put( number( "ps.bldgs", 0, 16, TRUE, me.bldgs ), vx, vy, white() );
		else
		{
			const Int xp = me.xp, nx = me.xpNext;
			DisplayString *v = txt( "ps.xp", 0, 16, TRUE, ((long long)xp << 32) | (UnsignedInt)nx, [xp, nx]() { return xpText( xp, nx ); } );
			put( v, vx, vy, white() );
			drawStars( vx + tw( v ) + S( 8 ), row.y + (row.h - S( 13 )) / 2, S( 13 ), me.rank );
		}
	}
}

/// A thin frame hugging each superweapon shortcut cell (ButtonCommandN of GenPowersShortcutBar*.wnd) while the player look
/// has the tray's own frame switched off: a navy edge and the steel line, like the bar's command buttons. After the windows,
/// outside the cell's own rect, so the icon, its clock and its hotkey letter are retail's untouched.
static void trayFramesIn( GameWindow *w )
{
	for (GameWindow *c = w; c != nullptr; c = c->winGetNext())
	{
		if (!c->winIsHidden() && strstr( c->winGetInstanceData()->m_decoratedNameString.str(), ":ButtonCommand" ) != nullptr)
		{
			const Rect r = screenRect( c );
			if (r.w > 0 && r.h > 0)
			{
				box( Rect{ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, edge() );
				box( Rect{ r.x - 1, r.y - 1, r.w + 2, r.h + 2 }, mouseIn( r ) ? accent() : steel() );
			}
		}
		if (!c->winIsHidden() && c->winGetChild() != nullptr)
			trayFramesIn( c->winGetChild() );
	}
}

static void drawTrayFrames()
{
	if (s_chrome.empty() || TheWindowManager == nullptr || TheWindowManager->winGetModal() != nullptr
		|| (TheInGameUI != nullptr && TheInGameUI->isQuitMenuVisible()))
		return;
	Bool trayRestyled = FALSE;		// only when a ButtonParent frame was actually switched off: no second frame over retail's
	for (std::map<std::string, ChromeSaved>::const_iterator it = s_chrome.begin(); it != s_chrome.end() && !trayRestyled; ++it)
		trayRestyled = it->first.find( ":ButtonParent" ) != std::string::npos;
	if (!trayRestyled)
		return;
	for (GameWindow *w = TheWindowManager->winGetWindowList(); w != nullptr; w = w->winGetNext())
		if (!w->winIsHidden() && strncmp( w->winGetInstanceData()->m_decoratedNameString.str(), "GenPowersShortcutBar", 20 ) == 0)
			trayFramesIn( w->winGetChild() );
}

/// Retail's cue for a new promotion point is ControlBar's m_genStarFlash (set by a points/rank change, cleared by getStarImage's
/// balance check and by opening the promotion panel). It is read through gxGenStarFlash, once per UPDATE of a match
/// (InGameUI::update, right after TheControlBar->update(), where retail runs getStarImage). The draw path only reads s_gen.
static void trackGeneralPoint()
{
	s_gen.update( gxGenStarFlash != FALSE, timeGetTime() );
}

/// The green 3-star promotion button on the right edge (ControlBar.wnd:ButtonGeneral) in the player look: drawn AFTER the
/// windows, opaque, over the retail button (like the $ box below) - navy with gold stars, the border lit while points wait
/// to be spent. The retail window underneath keeps its job: the click, its tooltip and its hotkey are untouched. It must be
/// fully opaque: under it the window still paints its own scheme art (!GXUISide.big's stars since 0.9.61), which showed
/// through a 238 fill as a second button (wh1ter0se 09/10).
/// A new point PULSES it (retail blinked it): the stars and the inner frame line swing from lit to dim and back once a second
/// on a cosine, light only - nothing moves - and the pulse ends for good when the panel is opened or a point is spent.
/// Pressed, the stars sit a pixel lower; disabled (an observer's bar), they are dim.
static void drawGeneralButton()
{
	GameWindow *g = barChild( BW_GENERAL );
	if (g == nullptr || g->winIsHidden())
		return;
	const Rect r = screenRect( g );
	if (r.w <= 0 || r.h <= 0)
		return;
	const Player *p = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	const Int pts = p != nullptr ? p->getSciencePurchasePoints() : 0;
	const Bool on = BitIsSet( g->winGetStatus(), WIN_STATUS_ENABLED );
	const Bool lit = on && pts > 0;
	const Bool hot = on && mouseIn( r );
	const Bool down = hot && BitIsSet( g->winGetInstanceData()->getState(), WIN_STATE_SELECTED );
	Real k = 1.0f;		// 1 = lit, 0 = the dim end of the pulse
	if (lit && s_gen.flash)		// kept by trackGeneralPoint
		k = 0.5f + 0.5f * cosf( (Real)((timeGetTime() - s_gen.since) % 1000u) * (6.2831853f / 1000.0f) );
	fill( r, navyDeep( 255 ) );
	box( r, lit ? accent() : (hot ? steel() : steelDim()) );
	if (lit)
		box( Rect{ r.x + 1, r.y + 1, r.w - 2, r.h - 2 }, GameMakeColor( 111, 168, 240, 60 + (Int)(195.0f * k) ) );
	const Int star = S( 15 ), sx = r.x + (r.w - 3 * star) / 2, sy = r.y + (r.h - star) / 2 + (down ? max( 1, S( 1 ) ) : 0);
	const Int lum = on ? 150 + (Int)(105.0f * k) : 110;
	drawStars( sx, sy, star, 3, GameMakeColor( lum, lum, lum, 255 ) );
}

/// The $ box in the SAME face as the other cells: CASH label + the figure on the shared baseline. Drawn AFTER the windows,
/// opaque, over the retail money display (a thin 30 px face of its own: owner 06/10 "the cash is inconsistent"), from the
/// same number the retail display shows (the local player's money, read every frame).
static void drawOwnMoney( const Row &me )
{
	const Rect cw = screenRect( barChild( BW_COMMAND ) ), mo = screenRect( barChild( BW_MONEY ) );
	if (cw.w <= 0 || mo.w <= 0)
		return;
	const Rect box_ = { mo.x - S( 8 ), mo.y, mo.w + S( 16 ), mo.h };
	fill( box_, navyDeep( 255 ) );
	box( box_, steelDim() );
	const Int vy = mo.y + (mo.h - th( 16 )) / 2, ly = vy + th( 16 ) - th( 11 ) - S( 1 );
	DisplayString *hd = fixed( "ps.cash", 11, TRUE, L"CASH" );
	DisplayString *v = number( "ps.cashv", 0, 16, TRUE, me.cash );
	const Int total = tw( hd ) + S( 7 ) + S( 11 ) + tw( v );
	const Int x = mo.x + (mo.w - total) / 2;
	put( hd, x, ly, label() );
	put( fixed( "ps.dollar", 16, TRUE, L"$" ), x + tw( hd ) + S( 7 ), vy, white() );
	put( v, x + tw( hd ) + S( 7 ) + S( 11 ), vy, white() );
}

/// The board's hover tip - one per frame, drawn after every row so it sits on top of them.
static UnicodeString s_boardTipText;
static Rect s_boardTipAt = { 0, 0, 0, 0 };
static Color s_boardTipColor = 0;
static Bool s_boardTip = FALSE;

static void boardTip( const UnicodeString &text, const Rect &anchor, Color c = 0 )
{
	s_boardTipText = text;
	s_boardTipAt = anchor;
	s_boardTipColor = c != 0 ? c : white();
	s_boardTip = TRUE;
}

static void drawBoardTip()
{
	if (!s_boardTip)
		return;
	s_boardTip = FALSE;
	long long key = 0;
	for (Int i = 0; i < s_boardTipText.getLength(); ++i)
		key = key * 31 + s_boardTipText.getCharAt( i );
	const UnicodeString t = s_boardTipText;
	DisplayString *d = txt( "pb.tip", 0, 13, TRUE, key, [t]() { return t; } );
	if (d == nullptr)
		return;
	const Int w = tw( d ) + S( 20 ), h = th( 13 ) + S( 12 );
	const Rect r = { max( S( 4 ), min( s_boardTipAt.x, s_W - w - S( 4 ) ) ), s_boardTipAt.y + s_boardTipAt.h + S( 4 ), w, h };
	plate( r, TRUE, TRUE );
	put( d, r.x + S( 10 ), r.y + S( 6 ), s_boardTipColor );
}

/// Retail Diplomacy's "in game" for a human (Diplomacy.cpp ~:525): connected right now. A defeated player who has since
/// disconnected is DEAD on the board but can no longer be muted. No network (skirmish) = nobody else to mute.
static Bool slotConnected( Int slot )
{
	if (slot >= 0 && slot < MAX_SLOTS && s_fakeHuman[slot])
		return TRUE;		// GX_PLAYERUI_SHOW human= (tests only): an AI row drawn as a human
	return TheNetwork != nullptr && TheNetwork->isPlayerConnected( slot );
}

// ---- VOTE-KICK. The vote itself is B . vote-kick's (GameClient/GXVoteKick.h, GXVoteKickHud.inl): the board's KICK button
// asks gxVoteKickCanStart (every rule - live multiplayer match, living voters, cooldown, running vote, at least three players -
// and the tooltip's reason) and starts the vote with gxVoteKickStart. The column exists in a network match only (never in
// skirmish or a replay); who may be kicked is the vote's own business. playerIndex = Player::getPlayerIndex().
static Bool kickAvailable() { return TheNetwork != nullptr && TheGameLogic != nullptr && TheGameLogic->isInMultiplayerGame(); }
static Bool kickCanStart( Int playerIndex, UnicodeString *whyNot ) { return gxVoteKickCanStart( playerIndex, whyNot ); }
static void kickStart( Int playerIndex ) { gxVoteKickStart( playerIndex ); }

/// The player Tab board: ME in full at the top (all of it my own), then every player as retail's Diplomacy screen shows
/// him - name, general, team, Dead/Left with the time. The rows never draw a number: gatherRows gave them none.
static void drawPlayerBoard( const Row *me )
{
	Int shown = 0;
	for (Int i = 0; i < s_rowCount; ++i)
		shown += visible( s_rows[i] ) ? 1 : 0;
	const Bool kick = kickAvailable();
	const Int rh = S( s_blockCount > 2 ? 30 : (shown > 12 ? 32 : 36) ), heroH = S( 132 ), teamH = S( 30 ), footH = S( 30 );
	const Int colH = S( 24 );
	// The list ends above the command bar, or above the screen's edge with the bar hidden (F9), and opens with my own row in view.
	Int limit = s_H - S( 8 );
	GameWindow *cb = barWindow();
	if (cb != nullptr && !cb->winIsHidden())
	{
		// The command panel and its money strip: the bar's own window reaches far higher, for the radar and the general's button at its ends.
		const Rect cw = screenRect( barChild( BW_COMMAND ) ), mo = screenRect( barChild( BW_MONEY ) );
		const Int top = (mo.h > 0 && (cw.h <= 0 || mo.y < cw.y)) ? mo.y : cw.y;
		const Int edge = top > s_H / 2 ? top : screenRect( cb ).y;
		if (edge > s_H / 2)
			limit = edge - S( 8 );
	}
	Int ownAt = 0, seen = 0;
	for (Int block = 0; block < s_blockCount; ++block)
		for (Int i = 0; i < s_rowCount; ++i)
			if (inBlock( s_rows[i], block ) && visible( s_rows[i] ))
			{
				if (s_rows[i].own)
					ownAt = seen;
				++seen;
			}
	const Int chrome = S( 10 ) + heroH + S( 6 ) + colH + max( 1, s_blockCount ) * teamH + footH;
	// A list that has to scroll anyway counts its rows from under the band and its PLAYERS tab, so it is never placed over them.
	const Int underBand = s_barHome.y + s_barHome.h + S( 26 );
	Int roomPx = limit - S( 40 ) - chrome;
	if (listRowsShown( shown, kListMaxRows, roomPx, rh ) < shown)
		roomPx = limit - underBand - chrome;
	listPrepare( shown, rh, roomPx, ownAt );
	const Int sbW = s_listShown < shown ? S( 18 ) : 0;
	const Int W = S( kick ? 904 : 840 ) + sbW, H = chrome + s_listShown * rh;
	Int boardTop = max( S( 40 ), (s_H - H) / 2 - S( 70 ) );
	if (sbW > 0 && boardTop < underBand && underBand + H <= limit)
		boardTop = underBand;
	if (boardTop + H > limit)
		boardTop = max( S( 40 ), limit - H );
	const Rect b = { (s_W - W) / 2, boardTop, W, H };
	plate( b, FALSE, TRUE );
	if (sbW > 0)
		s_listBox = b;
	addHit( b, H_BLOCK, 0, -1 );		// a click on the board is not a click on the ground under it
	s_boardRect = b;					// ...nor on a retail window under it: BoardGate

	Int y = b.y + S( 10 );
	if (me)
	{
		const Int pz = S( 108 );
		const Rect pr = { b.x + S( 16 ), y + (heroH - pz) / 2, pz, pz };
		fill( pr, navyDeep( 255 ) );
		if (me->portrait)
			TheDisplay->drawImage( me->portrait, pr.x, pr.y, pr.x + pr.w, pr.y + pr.h );
		box( pr, steel() );
		fill( pr.x, pr.y + pr.h - S( 3 ), pr.w, S( 3 ), me->color );
		const Int tx = pr.x + pr.w + S( 16 );
		Player *pl = me->player;
		put( txt( "pb.name", me->slot, 26, TRUE, 0, [pl]() { return hudName( pl ); } ), tx, y + S( 8 ), me->textColor );
		const UnicodeString gen = me->general;
		DisplayString *g = txt( "pb.gen", me->slot, 14, FALSE, 0, [gen]() { UnicodeString u = gen; u.concat( L" General" ); return u; } );
		const Int gy = y + S( 8 ) + th( 26 ) + S( 4 );
		put( g, tx, gy, label() );
		const Int sz = S( 15 );
		const Rect sr = { tx + tw( g ) + S( 7 ), gy - S( 2 ), max( sz * max( 1, me->rank ) + S( 6 ), S( 40 ) ), sz + S( 4 ) };
		drawStars( sr.x + S( 3 ), gy + S( 1 ), sz, me->rank );
		if (s_pHoverForced || mouseIn( sr ))
		{
			box( sr, accent() );
			s_hoverSlot = me->slot;
			s_hoverAnchor = sr;
		}
		put( fixed( "pb.youtag", 10, TRUE, L"YOU" ), tx, gy + th( 14 ) + S( 8 ), accent() );
		// Two rows of four, as the spectator's follow panel - all of it mine.
		// Five figures in a real match (no income, units or buildings), six in a sandbox: two rows, a half each.
		Int shown[8], nShown = 0;
		const Bool showIncome = sandboxMatch();
		for (Int j = 0; j < 8; ++j)
			if (!((j == 1 && !showIncome) || j == 4 || j == 5))
				shown[nShown++] = j;
		const Int perRow = (nShown + 1) / 2;
		const Int gx = tx + S( 250 ), cw = (b.x + b.w - S( 16 ) - gx) / perRow;
		static const wchar_t *const heads[8] = { L"CASH", L"INCOME / MIN", L"POWER USED", L"K / D", L"UNITS", L"BUILDINGS", L"XP", L"GEN. POINTS" };
		static const char *const ids[8] = { "pb.h0", "pb.h1", "pb.h2", "pb.h3", "pb.h4", "pb.h5", "pb.h6", "pb.h7" };
		for (Int k = 0; k < nShown; ++k)
		{
			const Int i = shown[k];
			const Int cx = gx + (k % perRow) * cw, cy = y + S( 14 ) + (k / perRow) * S( 56 );
			put( fixed( ids[i], 11, TRUE, heads[i] ), cx, cy, label() );
			const Int vy = cy + th( 11 ) + S( 3 );
			DisplayString *v = nullptr;
			Color vc = white();
			switch (i)
			{
				case 0: v = number( "pb.cash", 0, 21, TRUE, me->cash, FALSE, TRUE ); break;
				case 1: v = number( "pb.cpm", 0, 21, TRUE, me->cpm, TRUE ); break;
				case 2:
				{
					const Int p = me->prod, u = me->use;		// USED / PRODUCED, as on the economy row
					v = txt( "pb.pw", 0, 21, TRUE, ((long long)u << 32) | (UnsignedInt)p, [u, p]() { UnicodeString s; s.format( L"%d / %d", u, p ); return s; } );
					vc = me->deficit ? red() : (me->grid ? white() : dim());
					break;
				}
				case 3:
				{
					const Int k = me->kills, d = me->losses;
					v = txt( "pb.kd", 0, 21, TRUE, ((long long)k << 32) | (UnsignedInt)d, [k, d]() { UnicodeString s; s.format( L"%d / %d", k, d ); return s; } );
					break;
				}
				case 4: v = number( "pb.units", 0, 21, TRUE, me->units ); break;
				case 5: v = number( "pb.bldgs", 0, 21, TRUE, me->bldgs ); break;
				case 6:
				{
					const Int xp = me->xp, nx = me->xpNext;
					v = txt( "pb.xp", 0, 21, TRUE, ((long long)xp << 32) | (UnsignedInt)nx, [xp, nx]() { return xpText( xp, nx ); } );
					break;
				}
				case 7:
				{
					refreshBought( *me, FALSE );
					const Int sp = s_buySpent[me->slot], tot = s_buyTotal[me->slot];
					v = txt( "pb.gp", 0, 21, TRUE, ((long long)sp << 32) | (UnsignedInt)tot, [sp, tot]() { UnicodeString s; s.format( L"%d of %d", sp, tot ); return s; } );
					break;
				}
			}
			put( v, cx, vy, vc );
		}
	}
	y += heroH + S( 6 );

	// ---- everyone: ONE table (owner 06/10: "this is very ugly" - one aligned grid, a quiet band per row, no boxes
	// behind single words). Columns in 1080p px from the board's left; the column heads sit on the same x as the cells.
	enum { X_CHIP = 14, X_PORT = 24, X_NAME = 62, X_GEN = 306, X_STATUS = 476, R_PING = 642, R_FPS = 712, X_MUTE = 752, W_MUTE = 72,
		X_KICK = 834, W_KICK = 58 };
	fill( b.x + S( 6 ), y + colH - 1, b.w - S( 12 ), 1, steelDim() );			// the one line between the hero and the list
	{
		const Int hy = y + (colH - th( 10 )) / 2;
		put( fixed( "pb.c.name", 10, TRUE, L"PLAYER" ), b.x + S( X_NAME ), hy, label() );
		put( fixed( "pb.c.gen", 10, TRUE, L"GENERAL" ), b.x + S( X_GEN ), hy, label() );
		put( fixed( "pb.c.st", 10, TRUE, L"STATUS" ), b.x + S( X_STATUS ), hy, label() );
		DisplayString *ph = fixed( "pb.c.ping", 10, TRUE, L"PING" );
		putR( ph, b.x + S( R_PING ), hy, label() );
		putR( fixed( "pb.c.fps", 10, TRUE, L"FPS" ), b.x + S( R_FPS ), hy, label() );
		const Rect pr = { b.x + S( R_PING ) - tw( ph ), y, tw( ph ), colH };
		if (mouseIn( pr ))
			boardTip( L"Round trip to the host, in ms", pr );
	}
	y += colH;
	const Int listTop = y;
	const Int myTeam = me ? me->team : -1;
	const Bool net = TheNetwork != nullptr;
	const Int localSlot = net ? (Int)TheNetwork->getLocalPlayerID() : -1, routerSlot = net ? TheNetwork->getPacketRouterSlot() : -1;
	const Bool iRoute = net && routerSlot >= 0 && routerSlot == localSlot;
	// PING and FPS are shown once a second (owner 06/10: the figure "updates too fast"): the last value is held between
	// refreshes, so the colour follows the shown value. A board not drawn for 1.5 s (Tab was up) refreshes at once.
	static Int s_heldPing[MAX_SLOTS], s_heldFps[MAX_SLOTS];
	static UnsignedInt s_heldAt = 0, s_heldDrawn = 0;
	static Bool s_heldInit = FALSE;
	if (!s_heldInit)
	{
		s_heldInit = TRUE;
		for (Int i = 0; i < MAX_SLOTS; ++i)
			s_heldPing[i] = s_heldFps[i] = -2;		// -2 = never taken: a row that turns human mid-second is read at once
	}
	const UnsignedInt nowMs = timeGetTime();
	const Bool refreshFigures = s_heldAt == 0 || nowMs - s_heldDrawn > 1500 || nowMs - s_heldAt >= 1000;
	if (refreshFigures)
		s_heldAt = nowMs;
	s_heldDrawn = nowMs;
	const Int localPing = net && localSlot >= 0 ? TheNetwork->getPeerPingMs( localSlot ) : -1;
	Int band = 0;
	Int skip = s_listFirst, room = s_listShown;		// rows scrolled away above, rows still to draw
	for (Int block = 0; block < s_blockCount; ++block)
	{
		const Int blockRows = blockVisibleRows( block );
		if (sbW > 0 && (blockRows <= skip || room == 0))
		{
			skip -= min( skip, blockRows );
			continue;
		}
		Int alive = 0, total = 0;
		for (Int i = 0; i < s_rowCount; ++i)
			if (inBlock( s_rows[i], block ))
			{
				++total;
				alive += s_rows[i].status == STATUS_ALIVE ? 1 : 0;
			}
		const Int ty = y + (teamH - th( 12 )) / 2;
		put( blockTitle( block, myTeam ), b.x + S( X_NAME ), ty, accent() );
		putR( txt( "pb.alive", block, 12, TRUE, (alive << 8) | total, [alive, total]() { UnicodeString u; u.format( L"%d OF %d STANDING", alive, total ); return u; } ),
			b.x + b.w - S( 10 ) - sbW, ty, label() );		// right of the FPS column with a clear gap (owner 06/10: it "feels like it is within the fps")
		y += teamH;
		for (Int i = 0; i < s_rowCount; ++i)
		{
			const Row &r = s_rows[i];
			if (!inBlock( r, block ) || !visible( r ))
				continue;
			if (skip > 0)
			{
				--skip;
				continue;
			}
			if (room == 0)
				continue;
			--room;
			const Bool up = r.status == STATUS_ALIVE;
			const Rect rr = { b.x + S( 6 ), y, b.w - S( 12 ) - sbW, rh };
			// The band IS the text backing: a bright patch of map under the see-through board cannot swallow a name, with the
			// board's own opacity untouched (owner 06/10). Alternate rows a shade apart; my own row in the accent.
			fill( rr, r.own ? GameMakeColor( 111, 168, 240, 52 ) : navyDeep( (band++ & 1) ? 132 : 100 ) );
			const Int cy = y + (rh - th( 15 )) / 2, sy = y + (rh - th( 14 )) / 2;
			fill( b.x + S( X_CHIP ), y + S( 6 ), S( 4 ), rh - S( 12 ), r.color );
			if (r.portrait)
			{
				const Int pz = rh - S( 8 ), px = b.x + S( X_PORT );
				TheDisplay->drawImage( r.portrait, px, y + S( 4 ), px + pz, y + S( 4 ) + pz, 0xFFFFFFFF,
					up ? Display::DRAW_IMAGE_ALPHA : Display::DRAW_IMAGE_GRAYSCALE );
			}
			Player *pl = r.player;
			const Int nameW = S( (Real)(X_GEN - X_NAME - (r.own ? 46 : 14)) ), slot = r.slot;
			DisplayString *rn = txt( "pb.rname", r.slot, 15, TRUE, nameW, [pl, nameW, slot]() { Bool cut = FALSE; UnicodeString u = ellipsize( hudName( pl ), 15, TRUE, nameW, &cut ); s_boardNameCut[slot] = cut; return u; } );
			put( rn, b.x + S( X_NAME ), cy, up ? r.textColor : dim() );
			if (s_boardNameCut[r.slot] && pl != nullptr)
			{
				const Rect nr = { b.x + S( X_NAME ), y, tw( rn ), rh };
				if (mouseIn( nr ))
					boardTip( hudName( pl ), nr, r.textColor );
			}
			if (r.own)
				put( fixed( "pb.you", 10, TRUE, L"YOU" ), b.x + S( X_NAME ) + tw( rn ) + S( 8 ), cy + S( 3 ), accent() );
			const UnicodeString gen = r.general;
			const Bool rnd = r.randomGeneral;
			const Int genW = S( (Real)(X_STATUS - X_GEN - 12) );
			put( txt( "pb.rgen", r.slot, 14, FALSE, (genW << 1) | (rnd ? 1 : 0), [gen, rnd, genW]() { UnicodeString u = gen; if (!rnd) u.concat( L" General" ); return ellipsize( u, 14, FALSE, genW, nullptr ); } ),
				b.x + S( X_GEN ), sy, up ? label() : dim() );
			if (!up)
			{
				const Bool left = r.status == STATUS_LEFT;
				DisplayString *tag = left ? fixed( "tag.left", 13, TRUE, L"LEFT" ) : fixed( "tag.dead", 13, TRUE, L"DEAD" );
				const Rect t = { b.x + S( X_STATUS ), y + (rh - th( 13 ) - S( 4 )) / 2, tw( tag ) + S( 12 ), th( 13 ) + S( 4 ) };
				fill( t, left ? GameMakeColor( 120, 86, 16, 230 ) : GameMakeColor( 120, 24, 24, 230 ) );
				put( tag, t.x + S( 6 ), t.y + S( 2 ), left ? amber() : red() );
				if (s_statusFrame[r.slot] != 0)
				{
					const UnsignedInt fr = s_statusFrame[r.slot];
					put( txt( "pb.time", r.slot, 13, FALSE, (Int)fr, [fr]() { return clockText( fr ); } ), t.x + t.w + S( 8 ), sy, dim() );
				}
			}
			// An ally's row: hovering it shows what he bought (the hover card, same view as my own stars). Allies only.
			if (r.ally && (mouseIn( Rect{ b.x, y, b.w, rh } ) || r.slot == s_pAllyHover))
			{
				s_hoverSlot = r.slot;
				s_hoverAnchor = Rect{ b.x + S( X_GEN ), y, S( 120 ), rh };
				box( Rect{ b.x + S( 4 ), y + 1, b.w - S( 8 ), rh - 2 }, accent() );
			}
			// PING to the host for every human (public, as retail Diplomacy shows it); FPS mine, everyone's only on the host.
			// "--" = not known here. AI rows have neither and leave the cells empty.
			const GameSlot *gs = TheGameInfo ? TheGameInfo->getConstSlot( r.slot ) : nullptr;
			const Bool human = gs != nullptr && (gs->isHuman() || s_fakeHuman[r.slot]);
			if (human)
			{
				if (refreshFigures || s_heldPing[r.slot] == -2)
					s_heldPing[r.slot] = pingShown( net ? TheNetwork->getPeerPingMs( r.slot ) : -1, r.slot == routerSlot, iRoute, localPing );
				const Int ping = s_heldPing[r.slot];
				putR( ping >= 0 ? txt( "pb.ping", r.slot, 14, TRUE, ping, [ping]() { UnicodeString u; u.format( L"%d ms", ping ); return u; } ) : fixed( "pb.nping", 14, TRUE, L"--" ),
					b.x + S( R_PING ), sy, qualityColor( pingClass( ping ) ) );
				// A player who has gone keeps his last average in the router's table: not shown once he is no longer
				// connected.
				if (refreshFigures || s_heldFps[r.slot] == -2)
					s_heldFps[r.slot] = fpsShown( r.own, (Int)(TheDisplay->getAverageFPS() + 0.5f),
						(net && slotConnected( r.slot )) ? TheNetwork->getSlotAverageFPS( r.slot ) : -1 );
				const Int fps = s_heldFps[r.slot];
				putR( fps >= 0 ? number( "pb.fps", r.slot, 14, TRUE, fps ) : fixed( "pb.nfps", 14, TRUE, L"--" ), b.x + S( R_FPS ), sy, qualityColor( fpsClass( fps ) ) );
				if (mayMute( human, r.own, r.status != STATUS_LEFT && slotConnected( r.slot ) ))
				{
					const Rect mb = { b.x + S( X_MUTE ), y + S( 6 ), S( W_MUTE ), rh - S( 12 ) };
					const Bool muted = gs->isMuted(), hot = mouseIn( mb );
					fill( mb, muted ? GameMakeColor( 120, 86, 16, 230 ) : (hot ? GameMakeColor( 111, 168, 240, 70 ) : navyDeep( 200 )) );
					box( mb, muted ? amber() : (hot ? accent() : steelDim()) );
					DisplayString *ml = muted ? (hot ? fixed( "pb.unmute", 11, TRUE, L"UNMUTE" ) : fixed( "pb.muted", 11, TRUE, L"MUTED" )) : fixed( "pb.mute", 11, TRUE, L"MUTE" );
					putC( ml, mb.x + mb.w / 2, mb.y + (mb.h - th( 11 )) / 2, muted ? amber() : white() );
					addHit( mb, H_MUTE, r.slot, -1 );
				}
				if (kick && !r.own && pl != nullptr)
				{
					UnicodeString why;
					const Bool can = kickCanStart( pl->getPlayerIndex(), &why );
					const Rect kb = { b.x + S( X_KICK ), y + S( 6 ), S( W_KICK ), rh - S( 12 ) };
					const Bool hot = mouseIn( kb );
					fill( kb, can && hot ? GameMakeColor( 120, 24, 24, 200 ) : navyDeep( 200 ) );
					box( kb, can ? (hot ? red() : steelDim()) : faded( steelDim() ) );
					putC( fixed( "pb.kick", 11, TRUE, L"KICK" ), kb.x + kb.w / 2, kb.y + (kb.h - th( 11 )) / 2, can ? white() : dim() );
					if (can)
						addHit( kb, H_KICK, pl->getPlayerIndex(), -1 );
					else if (hot && why.getLength() > 0)
						boardTip( why, kb );
				}
			}
			y += rh;
		}
	}
	if (sbW > 0)
		drawListScroll( b.x + b.w - S( 16 ), listTop, b.y + b.h - footH );
	const Int fy = b.y + b.h - footH + (footH - th( 10 )) / 2;
	put( fixed( "pb.hint", 10, TRUE, L"HOLD TAB AND CLICK TO MUTE   |   RELEASE TAB TO CLOSE   |   HOVER YOUR STARS: WHAT YOU BOUGHT" ),
		b.x + S( 22 ), fy, label() );
	drawBoardTip();
}

static void updateScale()
{
	if (s_W != TheDisplay->getWidth() || s_H != TheDisplay->getHeight())
	{
		s_W = TheDisplay->getWidth();
		s_H = TheDisplay->getHeight();
		s_scale = s_H / 1080.0f;
		s_fonts.clear();
		++s_fontGen;
	}
}

/// The mode for this frame (decideMode), once the test stages due by now are in. Called before the windows paint and
/// again after; the second call finds nothing new.
static void decide()
{
	while (s_pstageNext < s_pstageCount && TheGameLogic->getFrame() >= s_pstages[s_pstageNext].frame)
		applyPlayerStage( s_pstages[s_pstageNext++].list );
	const Bool feed = gxViewerMaySeeFeed();
	Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	const Bool observer = local != nullptr && local->isPlayerObserver();
	const Bool versus = TheGameLogic->isInMultiplayerGame() || TheGameLogic->isInSkirmishGame();
	s_mode = decideMode( s_set.generalsx, feed, local != nullptr, observer, versus );
	if (s_forceLook >= 0 && decideMode( TRUE, feed, local != nullptr, observer, versus ) == MODE_PLAYER)
		s_mode = s_forceLook ? MODE_PLAYER : MODE_LEGACY;		// a test stage picks one of a PLAYER's two looks, nothing else
}

static Bool inMatch()
{
	return TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && TheDisplay != nullptr;
}

/// InGameUI::update's hook: the General's button state, read per update (see trackGeneralPoint). Reads only.
static void trackGeneralPointUpdate()
{
	if (!inMatch())
		return;
	if (!s_loaded)
		loadMatchState();
	trackGeneralPoint();
}

/// Every frame BEFORE the windows paint (InGameUI::postDraw): the mode, the Dead/Left times, and the bar's look for this
/// frame's repaint - the art flag and the radar window's shape for everyone (the legacy look gets the map without black
/// bars too) and, in player mode, the navy panels and the economy row UNDER the retail windows: drawn after them they
/// dimmed every button. The overlays (players bar, board, hover card) still draw after.
static void underWindows( Bool overlayHidden )
{
	s_overlayHiddenNow = overlayHidden;
	legacyRadarFrame( nullptr );		// off unless applyLook raises it again this frame
	if (!inMatch())
		return;
	if (s_boardHeld && IsInGameChatActive())
		s_boardHeld = FALSE;		// every frame, hidden overlay or not: chat eats the release (ownership stays, ownedKeyDrained swallows its up)
	if (!s_loaded)
		loadMatchState();
	// The switch, not the mode: the legacy look is retail's radar margins and the side buttons, General's button and ladder
	// in their original art (owner 21/10/2026); every GeneralsX moment keeps !GXUISide.big's art and the border image.
	gxRadarRetailMargins = !s_set.generalsx;
	if (TheMappedImageCollection)
		TheMappedImageCollection->gxShowGeneralsXLook( s_set.generalsx );
	releaseLockOnForeignFollow();
	decide();
	if (s_mode != MODE_LEGACY || s_set.generalsx)
		sampleStatuses();
	s_hitCount = 0;
	GameWindow *bar = barWindow();
	if (s_mode == MODE_SPECTATOR)
		gxControlBarArtOff = FALSE;		// the spectator HUD hides the bar itself
	if (s_mode == MODE_SPECTATOR || bar == nullptr || bar->winIsHidden())
	{
		// A defeated player with F11 (overlay hidden) still sees the retail bar art: its radar must be the retail box again,
		// not the centred map shape left from the legacy look.
		if (bar != nullptr && s_radarSet.w > 0)
			fitRadarWindow( barChild( BW_RADAR ), RADAR_HOME );
		applyChrome( FALSE );		// the retail pop-ups are retail again wherever the look is not up
		return;
	}
	const Bool skin = s_mode == MODE_PLAYER && !overlayHidden;		// F11 hides the GeneralsX panels with the readouts
	applyLook( skin );
	drawLegacyFrame();
	if (!skin)
		return;
	updateScale();
	s_alpha = clamp( 0, s_set.opacity * 235 / 100, 255 );
	gatherRows();
	auditFairness();
	drawBarPlates();
	const Row *me = ownRow();
	if (s_set.playerStrip && me != nullptr)
		drawOwnStrip( *me );
}

/// After the windows (InGameUI::postWindowDraw, through draw()): the player overlays. Rows, panels and the economy row
/// were done before the windows painted (underWindows), this same frame.
static Bool drawPlayerMode()
{
	GameWindow *bar = barWindow();
	if (s_boardHeld && IsInGameChatActive())
		s_boardHeld = FALSE;		// chat eats the release; ownership stays, so ownedKeyDrained still swallows its up
	// F10 hides the bar (and with it everything here); the quit menu keeps the look but takes no input.
	const Bool on = s_mode == MODE_PLAYER && bar != nullptr && !bar->winIsHidden();
	if (!on)
	{
		drawTrayFrames();		// the tray's own frames are off while the look is up, bar shown or not (F10)
		return FALSE;
	}		// a held board stays held through F10: its key's up (ownedKeyDrained) ends it
	s_hoverSlot = -1;		// only this frame's board may set it
	if (s_set.playerStrip && ownRow() != nullptr)
		drawOwnMoney( *ownRow() );
	drawGeneralButton();
	drawTrayFrames();
	if (TheInGameUI->isQuitMenuVisible())
		return TRUE;

	const Row *me = ownRow();
	Rect pbar;
#if defined(GX_TEST_INSTRUMENTS)
	{
		// TEST INSTRUMENT ONLY (never a player path): GX_TEST_PLAYBAR=1 opens the PLAYERS panel once per match, so the open-bar layout
		// can be photographed where a synthetic click cannot reach the tab. Absent variable = no effect at all.
		static const char *const s_testOpen = getenv( "GX_TEST_PLAYBAR" );
		static Bool s_testOpened = FALSE;
		if (s_testOpen != nullptr && TheGameLogic != nullptr)
		{
			const UnsignedInt f = TheGameLogic->getFrame();
			if (f < 30)
				s_testOpened = FALSE;
			else if (!s_testOpened && f > 90)
			{
				s_dock.panel[PANEL_PLAYERS].open = TRUE;
				s_testOpened = TRUE;
			}
		}
	}
#endif
	drawBar( pbar );
	const Bool open = s_dock.panel[PANEL_PLAYERS].open;
	// Messages and superweapon timers start under the bar's HOME position and size, open or folded, dragged or not: they
	// never move when the strip does (owner 06/10: "moving this around also moves the nuclear missile timer").
	if (s_barHome.y < s_H / 3)
	{
		s_topInset = s_barHome.y + s_barHome.h + S( 26 );
		// The message column avoids the bar's HOME footprint only while the bar is drawn OPEN (lead 11/10: a folded bar must not
		// make the list wrap at a line nobody can see). The vertical start never depends on it.
		s_insetSpanOn = open;
		s_insetX0 = s_barHome.x; s_insetX1 = s_barHome.x + s_barHome.w; s_insetY0 = s_barHome.y;
	}
	// Folded, the tab waits top-centre (one click, or the bar key); open, it hangs under the bar like the spectator's.
	const Rect phome = placed( s_barHome, PANEL_PLAYERS );
	dockTabCentred( "tab.players", L"PLAYERS", phome.x + phome.w / 2, open ? pbar.y + pbar.h + S( 1 ) : phome.y, PANEL_PLAYERS, open );
	if (s_boardHeld && IsInGameChatActive())
		s_boardHeld = FALSE;		// chat's text entry eats the Tab release (see draw())
	if (s_set.playerTabBoard && (s_boardHeld || s_pBoardForced) && (TheWindowManager == nullptr || TheWindowManager->winGetModal() == nullptr))
		drawPlayerBoard( me );		// not over a modal popup: it owns the screen's input then (BoardGate)
	if (s_hoverSlot >= 0)
		drawHoverCard();
	s_inputLive = TRUE;
	return TRUE;
}

static GameMessageDisposition onPlayerKey( Int key, Int mods, Bool down, Bool repeat )
{
	const Chord &b = s_pchords[PA_BOARD];
	if (s_set.playerTabBoard && b.valid && key == b.key && down && !repeat && mods == b.mods)		// repeats and the up: ownedKeyDrained
	{
		s_boardHeld = TRUE;
		s_boardOwnKey = key;
		s_boardKeyUp = FALSE;
		return DESTROY_MESSAGE;
	}
	// A chord whose DOWN we took owns its key until the key goes up, whatever the modifiers by then (ownedKeyDrained, which
	// runs before everything else): Alt+P, let go of Alt, then P - the bare P up would otherwise reach the command bar's
	// hotkey, which fires on key-up. Ownership starts on a FRESH down only: a modified auto-repeat of a key
	// that was already down belongs to whoever took its first press.
	for (Int a = PA_BAR; a < PA_COUNT; ++a)
	{
		const Chord &c = s_pchords[a];
		if (!c.valid || key != c.key || !down || repeat || mods != c.mods)
			continue;
		s_dock.panel[PANEL_PLAYERS].open = !s_dock.panel[PANEL_PLAYERS].open;
		s_pOwned[a] = TRUE;
		return DESTROY_MESSAGE;
	}
	return KEEP_MESSAGE;
}

/// Owner 05/10/2026: "How can I tell who am I spectating exactly? seems hard to tell". One chip at the top centre, always
/// up for a spectator: VIEWING <name> in his colour when the view is through his eyes (his fog of war), else VIEWING
/// WHOLE MAP, then FOLLOWING <name> when the follow panel is on someone. His card and his board row carry the accent
/// frame too. It changes on the frame the view does, with no animation: the view moves on a key press, many times a match.
/// THEIR CAMERA when the camera is the followed player's own, played back from the replay (owner 14/10/2026: following
/// himself showed a base he had parked his camera on, and nothing said whose camera that was).
/// A click switches between the whole map and the followed player's eyes (owner 24/10/2026, decision A; viewAfterChip), and the
/// chip says which a click gives; Esc leaves both. The plate lights its frame under the mouse while a click would act.
static void drawViewingChip()
{
	Player *o = TheControlBar ? TheControlBar->getObservedPlayer() : nullptr;
	Player *f = followedPlayer();
	const Row *ro = nullptr, *rf = nullptr;
	for (Int i = 0; i < s_rowCount; ++i)
	{
		if (s_rows[i].player == o)
			ro = &s_rows[i];
		if (s_rows[i].player == f)
			rf = &s_rows[i];
	}
	struct Part { DisplayString *ds; Color c; const Row *swatch; Int px; };
	Part parts[8];
	Int n = 0;
	parts[n++] = { fixed( "vw.head", 11, TRUE, L"VIEWING" ), label(), nullptr, 11 };
	if (ro)
	{
		Player *pl = ro->player;
		parts[n++] = { txt( "vw.name", ro->slot, 13, TRUE, 0, [pl]() { return hudName( pl ); } ), ro->textColor, ro, 13 };
		parts[n++] = { fixed( "vw.eyes", 11, TRUE, L"THROUGH THEIR EYES" ), label(), nullptr, 11 };
	}
	else
	{
		parts[n++] = { fixed( "vw.all", 13, TRUE, L"WHOLE MAP" ), white(), nullptr, 13 };
		if (rf)
		{
			Player *pl = rf->player;
			parts[n++] = { fixed( "vw.fol", 11, TRUE, L"FOLLOWING" ), label(), nullptr, 11 };
			parts[n++] = { txt( "vw.fname", rf->slot, 13, TRUE, 0, [pl]() { return hudName( pl ); } ), rf->textColor, rf, 13 };
		}
	}
	if (followsTheirCameraNow( rf != nullptr ))
		parts[n++] = { fixed( "vw.cam", 11, TRUE, L"THEIR CAMERA" ), label(), nullptr, 11 };
	const Bool leavable = s_mode == MODE_SPECTATOR && (ro != nullptr || rf != nullptr);
	const Bool toggles = s_mode == MODE_SPECTATOR && chipToggles( currentView(), rf != nullptr ? rf->status : STATUS_ALIVE, canViewAs() );
	if (toggles)
		parts[n++] = ro != nullptr && ro == rf ? Part{ fixed( "vw.tomap", 11, TRUE, L"CLICK FOR WHOLE MAP" ), white(), nullptr, 11 }
			: Part{ fixed( "vw.toeyes", 11, TRUE, L"CLICK FOR THEIR VIEW" ), white(), nullptr, 11 };
	if (leavable)
		parts[n++] = { fixed( "vw.esc", 11, TRUE, L"ESC TO LEAVE" ), label(), nullptr, 11 };
	const Int pad = S( 10 ), sw = S( 10 ), h = S( 24 );
	Int w = pad;
	for (Int i = 0; i < n; ++i)
		w += (parts[i].swatch ? sw + S( 5 ) : 0) + tw( parts[i].ds ) + S( 10 );
	w += pad - S( 10 );
	const Rect r = { s_W / 2 - w / 2, S( 4 ), w, h };
	s_chipRect = r;
	s_chipRectMs = timeGetTime();
	plate( r, TRUE, TRUE );
	if (toggles)
	{
		addHit( r, H_CHIP, 0, -1 );		// the followed player's eyes <-> the whole map
		if (mouseIn( r ))
			box( r, accent() );
	}
	else
		addHit( r, H_BLOCK, 0, -1 );	// a click on the chip is never a click on the ground under it
	Int x = r.x + pad;
	for (Int i = 0; i < n; ++i)
	{
		if (parts[i].swatch)
		{
			fill( x, r.y + (h - sw) / 2, sw, sw, parts[i].swatch->color );
			x += sw + S( 5 );
		}
		put( parts[i].ds, x, r.y + (h - th( parts[i].px )) / 2, parts[i].c );
		x += tw( parts[i].ds ) + S( 10 );
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------------------------------------------------
static void enterMode()
{
	s_modeOn = TRUE;
	// The caster layout replaces the retail observer bar; its minimap is ours, drawn in the map's own shape.
	HideControlBar( TRUE );
	if (TheRecorder && TheRecorder->isPlaybackMode())
		showReplayControls();
}

static void leaveMode()
{
	if (s_modeOn)
		ShowControlBar( TRUE );
	s_modeOn = FALSE;
}

// ---- where the toast's power was aimed (owner 12/10/2026: "I wanna see WHERE it was called to like show the actual landing
// thing just like it would show a player like the target preview") ---------------------------------------------------------
/// Every targeted power's newest order, one per caster and power (a ring let unannounced radar scans push a live row's
/// target out, spectator-click r1-BC P2); emptied each match.
struct NoteAt { PowerNote note; Coord3D at; };
static std::map<std::pair<UnsignedInt, UnsignedInt>, NoteAt> s_notes;
static RadiusDecal s_reticle;							///< the target preview drawn at a toast's target, kReticleMs long
static UnsignedInt s_reticleMs = 0;				///< timeGetTime when it appeared; 0 = none
static const UnsignedInt kReticleMs = 4000;

static void notePowerTarget( const PowerNote &note, const Coord3D &at )
{
	NoteAt &n = s_notes[std::make_pair( (UnsignedInt)note.caster, (UnsignedInt)note.power )];
	n.note = note;
	n.at = at;
}

static void clearReticle()
{
	s_reticle.clear();
	s_reticleMs = 0;
}

/// The preview fades in over 150 ms and out over its last 800 ms, like the toast it came from; steady between (the caster's
/// cursor throbs to say "aiming", this says "here").
static void updateReticle()
{
	if (s_reticleMs == 0)
		return;
	const UnsignedInt age = timeGetTime() - s_reticleMs;
	if (age >= kReticleMs)
	{
		clearReticle();
		return;
	}
	s_reticle.setOpacity( 0.85f * min( 1.0f, min( age / 150.0f, (kReticleMs - age) / 800.0f ) ) );
}

static Bool draw()
{
	s_inputLive = FALSE;
	s_topInset = 0;
	s_insetSpanOn = FALSE;
	s_toastLineCount = 0;		// drawToasts sets the toasts' click lines again if one is drawn this frame
	s_focusDrawn.w = 0;			// drawFocus sets it again if drawn this frame
	s_boardRect.w = 0;			// drawPlayerBoard sets it again if the board is up this frame
	s_listBox.w = s_listTrack.w = 0;	// ...and a list that scrolls, its box and track, if one is
	s_barSide[0].w = s_barSide[1].w = 0;
	if (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheDisplay == nullptr)
		return FALSE;
	updateReticle();
	if (!s_loaded)
		loadMatchState();
	decide();
	if (s_mode != MODE_SPECTATOR)
	{
		leaveMode();
		return drawPlayerMode();
	}
	if (!s_modeOn)
		enterMode();
	// Match start shows the bar again after our first frame (GameLogic::startNewGame -> ShowControlBar), and scripts may
	// too: read the window's own hidden bit, the state every path writes (ControlBarCallback.cpp ToggleGameHud), and
	// hide it again whenever it is up. Found by name among the top-level windows, never through nameToKey: that would mint
	// a NameKey, on spectators only, for a ControlBar.wnd without this window (a mod) - see portraitFor.
	if (barWindow() != nullptr && !s_barWindow->winIsHidden())
		enterMode();
	if (TheInGameUI->isQuitMenuVisible())
		return TRUE;

	updateScale();
	s_alpha = clamp( 0, s_set.opacity * 235 / 100, 255 );
	s_hitCount = 0;
	while (s_stageNext < s_stageCount && TheGameLogic->getFrame() >= s_stages[s_stageNext].frame)
		applyStage( s_stages[s_stageNext++].list );
	occScan();
	gatherRows();
	if (s_forceFollow >= 0 && (TheInGameUI == nullptr || TheInGameUI->getSelectCount() == 0
		|| TheControlBar == nullptr || TheControlBar->getObservedPlayer() == nullptr))		// follow() may deselect: no message from draw
	{
		follow( s_forceFollow );
		s_forceFollow = -1;
	}
	if (s_mode == MODE_SPECTATOR && (TheInGameUI == nullptr || TheInGameUI->getSelectCount() == 0))
	{
		// The viewed player died or left: he has no eyes, so the map goes whole (he stays followed). Waits for an empty
		// selection like the staged view below: changeObservedPlayer deselects, and a render-stage hook appends no message.
		const ViewState cur = currentView();
		for (Int i = 0; cur.view >= 0 && i < s_rowCount; ++i)
			if (s_rows[i].slot == cur.view)
			{
				const ViewState n = viewAfterStatus( cur, s_rows[i].status );
				if (!(n == cur))
					applyView( n );
			}
	}
	if (s_forceView >= 0)
	{
		// changeObservedPlayer deselects, which appends MSG_DESTROY_SELECTED_GROUP: a render-stage hook may append no
		// message, so a staged view change waits for an empty selection.
		if (TheInGameUI == nullptr || TheInGameUI->getSelectCount() == 0)
		{
			for (Int i = 0; i < s_rowCount; ++i)
				if (s_rows[i].slot == s_forceView)
					rts::changeObservedPlayer( s_rows[i].player );		// the retail observer buttons' own call
			s_forceView = -1;
		}
	}

	if (const Object *o = inspectedObject())
		drawInspectMarker( o );
	Rect bar;
	drawBar( bar );
	drawViewingChip();
	if (s_barHome.y < s_H / 3)
	{
		s_topInset = s_barHome.y + s_barHome.h + S( 26 );		// under the bar's home spot and its PLAYERS tab, open or not (never jumps)
		s_insetSpanOn = s_dock.panel[PANEL_PLAYERS].open;		// drawBar draws nothing while the panel is folded
		s_insetX0 = s_barHome.x; s_insetX1 = s_barHome.x + s_barHome.w; s_insetY0 = s_barHome.y;
	}
	mapHome();
	Rect mapRect = s_mapHome;
	if (s_set.show[SHOW_MINIMAP])
		drawMap( mapRect );
	if (s_set.show[SHOW_FOLLOW_PANEL])
		drawFocus( mapRect );
	if (s_dock.panel[PANEL_DISPLAY].open)
		drawDisplayPanel();
	// After every panel it must keep off, so their rects are this frame's (the board and the tabs still draw over everything).
	if (s_set.show[SHOW_EVENT_TOAST])
		drawToasts();
	// Chat's text entry takes the keyboard and eats the scoreboard key's release (GadgetTextEntry handles Tab), so a board
	// held while chat opens would stay up after the key is let go: let it go now (p1-spectator).
	if (s_boardHeld && IsInGameChatActive())
		s_boardHeld = FALSE;
	const Bool board = s_set.show[SHOW_TAB_BOARD] && (s_boardHeld || s_dock.panel[PANEL_BOARD].open);
	s_hoverSlot = -1;
	if (board)
		drawBoard();
	if (s_hoverSlot >= 0)
		drawHoverCard();

	// Tabs last: always reachable, even under an open board.
	const Rect pb = placed( s_barHome, PANEL_PLAYERS );
	const Rect pt = playersTabRect( pb, playersTabTextW(), s_scale );		// the rect the toast stack keeps off (at home)
	dockTab( "tab.players", L"PLAYERS", pt.x, pt.y, PANEL_PLAYERS, s_dock.panel[PANEL_PLAYERS].open );
	if (s_set.show[SHOW_MINIMAP])
	{
		const Rect mr = placed( s_mapHome, PANEL_MAP );
		dockTab( "tab.map", L"MAP", mr.x + S( 40 ), mr.y - S( 22 ), PANEL_MAP, !s_dock.panel[PANEL_MAP].open );
	}
	// The gear (the customise panel's tab) sits at the bar's right end, docked or not.
	const Rect g = gearTabRect( s_barHome, s_scale );
	plate( g, TRUE, TRUE );
	if (mouseIn( g ) || s_dock.panel[PANEL_DISPLAY].open)
		fill( g, GameMakeColor( 111, 168, 240, 60 ) );
	const Int gx = g.x + g.w / 2, gy = g.y + g.h / 2;
	box( Rect{ gx - S( 5 ), gy - S( 5 ), S( 10 ), S( 10 ) }, white() );
	fill( gx - S( 1 ), gy - S( 7 ), S( 3 ), S( 15 ), white() );
	fill( gx - S( 7 ), gy - S( 1 ), S( 15 ), S( 3 ), white() );
	addHit( g, H_TAB, 0, PANEL_DISPLAY );

	s_inputLive = TRUE;
	return TRUE;
}

/// Every cached HUD string back to TheDisplayStringManager: between matches (reset) and when InGameUI goes (its destructor).
static void freeStrings()
{
	clearReticle();		// with the strings: the shadow manager it lives in goes with the UI
	for (std::map<std::pair<const char *, Int>, Txt>::iterator it = s_txt.begin(); it != s_txt.end(); ++it)
		if (it->second.ds != nullptr && TheDisplayStringManager != nullptr)
			TheDisplayStringManager->freeDisplayString( it->second.ds );
	s_txt.clear();
}

static void reset()
{
	// No ShowControlBar here: reset() runs on the way back to the shell, and every new match shows the bar itself
	// (GameLogic::startNewGame), so the next match starts from the retail state either way.
	s_modeOn = FALSE;
	s_mode = MODE_LEGACY;
	s_loaded = FALSE;
	resetSkin();							// the retail bar art and radar rect; windows can be rebuilt between matches
	s_pstageCount = s_pstageNext = 0;
	s_forceLook = -1;
	s_pBoardForced = s_pHoverForced = FALSE, s_pAllyHover = -1;
	s_hoverSlot = -1;
	s_forceView = -1;
	clearReticle();
	s_notes.clear();			// last match's ids and frames mean nothing in this one
	s_rightOwned = FALSE;
	s_leftOwned = FALSE;
	s_inspectID = INVALID_ID;
	s_arrivals.reset();			// object ids are reused by the next match
	s_occLogObj = INVALID_ID;
	s_occLogText.clear();
	s_occScanEvery = s_occScanNext = 0;
	s_boardKeyUp = FALSE;
	s_boardPressHit.kind = -1;
	s_boardPress = 0;
	s_boardRect.w = 0;
	s_barFirst[0] = s_barFirst[1] = 0;
	s_listFirst = 0;
	s_listDrag = FALSE;
	s_blockCount = 0;
	for (Int a = 0; a < PA_COUNT; ++a)
		s_pOwned[a] = FALSE;
	s_topInset = 0;
	s_insetSpanOn = FALSE;
	s_toastLineCount = 0;
	for (Int i = 0; i < kToastRing; ++i)
	{
		s_toastSeenId[i] = 0;
		s_toastSeenMs[i] = 0;
	}
	s_dock.reset();
	s_boardHeld = FALSE;
	s_boardOwnKey = 0;
	s_escOwned = FALSE;
	s_capture = CAP_NONE;
	s_inputLive = FALSE;
	s_hitCount = 0;
	s_rowCount = 0;			// never act on last match's Player pointers
	s_seeded = FALSE;
	s_countMs = 0;
	s_buySlot = -1;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		s_buyMs[i] = 0;
		s_buySpent[i] = s_buyTotal[i] = 0;
		s_hidden[i] = FALSE;
		s_lastStatus[i] = STATUS_ALIVE;
		s_statusFrame[i] = 0;
		s_units[i] = s_bldgs[i] = 0;
		s_portrait[i] = nullptr;
		s_portraitFor[i] = nullptr;
		s_general[i] = UnicodeString::TheEmptyString;
		s_apparentFor[i] = 0;
		s_apparentPortrait[i] = nullptr;
		s_apparentGeneral[i] = UnicodeString::TheEmptyString;
	}
	// Names and fixed text are keyed 0 per slot; a new match can put a different player in the same slot - and the strings
	// go back to their manager between matches (: its destructor asserts the list empty).
	freeStrings();
}

// ---------------------------------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------------------------------
static const Hit *hitAt( Int x, Int y )
{
	for (Int i = s_hitCount - 1; i >= 0; --i)
		if (s_hits[i].r.contains( x, y ))
			return &s_hits[i];
	return nullptr;
}

static void setOpacityFromX( Int x )
{
	if (s_slider.w <= 0)
		return;
	const Int v = OPACITY_MIN + (x - s_slider.x) * (OPACITY_MAX - OPACITY_MIN) / s_slider.w;
	s_set.opacity = clampOpacity( (v + 2) / 5 * 5 );
}

/// A click on the event toast: where its power was AIMED, when the line is a power's and its order was heard (the target and the
/// caster's decal); else the building the line is about; else that player's command centre.
/// The object is looked up by id NOW (never a cached pointer): one sold or destroyed since the line was posted falls back to
/// the command centre; no command centre = nothing happens. A purchased science names no object, so it always takes the fallback.
/// FAIRNESS: the toast is only ever drawn to a viewer who may see the whole feed (gxViewerMaySeeFeed: replay, observer,
/// defeated player), so the jump shows him nothing his own vision does not already allow; a playing viewer gets no toast at all.
/// The gate is re-asked here, at click time.
static void toastJump( Int playerIndex, ObjectID objectID, Int science, UnsignedInt frame, UnsignedInt powerID )
{
	if (!gxViewerMaySeeFeed() || ThePlayerList == nullptr || TheTacticalView == nullptr)
		return;
	// A power's line jumps to where it was AIMED and shows the decal its caster aimed it with (pickPowerNote: a launch line by its
	// launcher and power, a general's-skill line by its player and skill). No note (a power with no target) keeps the old jump below.
	std::vector<PowerNote> notes;
	std::vector<const NoteAt *> at;
	for (std::map<std::pair<UnsignedInt, UnsignedInt>, NoteAt>::const_iterator it = s_notes.begin(); it != s_notes.end(); ++it)
	{
		notes.push_back( it->second.note );
		at.push_back( &it->second );
	}
	const Int n = notes.empty() ? -1 : pickPowerNote( &notes[0], (int)notes.size(), (unsigned)objectID, powerID, playerIndex, science, frame,
		LOGICFRAMES_PER_SECOND, LOGICFRAMES_PER_SECOND * 5 );
	if (n >= 0)
	{
		const Coord3D target = at[n]->at;
		TheTacticalView->lookAt( &target );
		releaseReplayCameraLock();
		clearReticle();
		const SpecialPowerTemplate *power = TheSpecialPowerStore ? TheSpecialPowerStore->findSpecialPowerTemplateByID( notes[n].power ) : nullptr;
		if (TheInGameUI->gxPowerTargetDecal( power, target, s_reticle ))
		{
			s_reticleMs = max( (UnsignedInt)1, (UnsignedInt)timeGetTime() );	// one type: timeGetTime() is a DWORD (unsigned long) on Windows
			updateReticle();
		}
		fprintf( stderr, "[GXSPEC] toast target x=%.0f y=%.0f power=%u decal=%d f=%u\n", target.x, target.y, notes[n].power,
			s_reticleMs != 0 ? 1 : 0, TheGameLogic->getFrame() );
		return;
	}
	Player *p = ThePlayerList->getNthPlayer( playerIndex );
	if (p == nullptr)
		return;
	Object *o = objectID != INVALID_ID && TheGameLogic != nullptr ? TheGameLogic->findObjectByID( objectID ) : nullptr;
	if (o != nullptr && (o->isEffectivelyDead() || o->getControllingPlayer() != p))
		o = nullptr;
	if (o == nullptr)
		o = p->findNaturalCommandCenter();
	if (o != nullptr)
	{
		TheTacticalView->lookAt( o->getPosition() );
		releaseReplayCameraLock();
	}
}

static void act( const Hit &h )
{
	// One line per spectator action: cheap (a click), and the only way to read input behaviour back from a run.
	fprintf( stderr, "[GXSPEC] act kind=%d id=%d panel=%d f=%u\n", h.kind, h.id, h.panel, TheGameLogic->getFrame() );
	switch (h.kind)
	{
		case H_CARD:
		case H_ROW:
		case H_STARS:
			if (s_mode == MODE_SPECTATOR)		// a player follows nobody: the focus pointer is the observer panel's
				clickPlayer( h.id );
			break;
		case H_BARPAGE:
		{
			// A page of cards; a player's left half has one place fewer, his own card keeps it. barWindow clamps on the next draw.
			const Int side = h.id / 2;
			const Int step = max( 1, s_barFit - ((side == 0 && s_mode == MODE_PLAYER) ? 1 : 0) );
			s_barFirst[side] += (h.id & 1) ? step : -step;
			break;
		}
		case H_CHIP:
			if (s_mode == MODE_SPECTATOR)
				applyView( viewAfterChip( currentView(), followedStatus(), canViewAs() ) );		// from the translator: changeObservedPlayer may deselect
			break;
		case H_CHECK:
			s_set.show[h.id] = !s_set.show[h.id];
			saveSettings();
			break;
		case H_HIDEDEAD:
			s_set.hideDead = !s_set.hideDead;
			saveSettings();
			break;
		case H_FOLLOWCAM:
			setFollowCamera( !followCameraOn() );		// Options.ini, not GXHud.ini: the Options menu's box is the same setting
			break;
		case H_HIDEP:
			if (h.id >= 0 && h.id < MAX_SLOTS)
				s_hidden[h.id] = !s_hidden[h.id];
			break;
		case H_PRESET:
			applyPreset( s_set, (Preset)h.id );
			saveSettings();
			break;
		case H_RESET:
		{
			const Settings keep = s_set;
			s_set = Settings();
			s_set.generalsx = keep.generalsx;			// the switch, and the player rows, are not the spectator panel's
			s_set.playerStrip = keep.playerStrip;
			s_set.playerTabBoard = keep.playerTabBoard;
			for (Int i = 0; i < MAX_SLOTS; ++i)
				s_hidden[i] = FALSE;
			saveSettings();
			if (isReplay())
				setFollowCamera( TRUE );		// the panel's row, back to its default (written only if it was off)
			break;
		}
		case H_CLOSE:
			s_dock.panel[PANEL_BOARD].open = FALSE;
			s_boardHeld = FALSE;
			break;
		case H_MUTE:
			// Retail Diplomacy's mute, client side (Diplomacy.cpp:485-493): this machine stops showing that player's chat.
			if (s_mode == MODE_PLAYER && TheGameInfo != nullptr && h.id >= 0 && h.id < MAX_SLOTS && h.id != TheGameInfo->getLocalSlotNum()
				&& slotConnected( h.id ))
			{
				GameSlot *gs = TheGameInfo->getSlot( h.id );
				if (gs != nullptr && (gs->isHuman() || s_fakeHuman[h.id]))
					gs->mute( !gs->isMuted() );
			}
			break;
		case H_TOAST:
			if (const ToastLine *t = toastLine( h.id ))
				toastJump( t->about, t->object, t->science, t->frame, t->power );
			break;
		case H_KICK:
			if (s_mode == MODE_PLAYER)
				kickStart( h.id );
			break;
	}
}

static void followStep( Int dir )
{
	Int cur = -1;
	Player *f = followedPlayer();
	for (Int i = 0; i < s_rowCount; ++i)
		if (s_rows[i].player == f)
			cur = i;
	for (Int n = 1; n <= s_rowCount; ++n)
	{
		const Int i = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * n + s_rowCount * 2) % max( 1, s_rowCount );
		if (visible( s_rows[i] ) && s_rows[i].status == STATUS_ALIVE)
		{
			follow( s_rows[i].slot );
			return;
		}
	}
}

static void runAction( Int a )
{
	fprintf( stderr, "[GXSPEC] key action=%d f=%u\n", a, TheGameLogic->getFrame() );
	switch (a)
	{
		case A_PLAYERS: s_dock.panel[PANEL_PLAYERS].open = !s_dock.panel[PANEL_PLAYERS].open; break;
		case A_MAP: s_dock.panel[PANEL_MAP].open = !s_dock.panel[PANEL_MAP].open; break;
		case A_FOCUS: s_dock.panel[PANEL_FOCUS].open = !s_dock.panel[PANEL_FOCUS].open; break;
		case A_DISPLAY: s_dock.panel[PANEL_DISPLAY].open = !s_dock.panel[PANEL_DISPLAY].open; break;
		case A_TOAST: s_set.show[SHOW_EVENT_TOAST] = !s_set.show[SHOW_EVENT_TOAST]; saveSettings(); break;
		case A_HIDEDEAD: s_set.hideDead = !s_set.hideDead; saveSettings(); break;
		case A_DOCKALL: s_dock.redockAll(); break;
		case A_OPDOWN: s_set.opacity = stepOpacity( s_set.opacity, -1 ); saveSettings(); break;
		case A_OPUP: s_set.opacity = stepOpacity( s_set.opacity, +1 ); saveSettings(); break;
		case A_NEXT: followStep( +1 ); break;
		case A_PREV: followStep( -1 ); break;
		case A_DIPLOMACY: ToggleDiplomacy( FALSE ); break;
	}
}

class Translator : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage( const GameMessage *msg ) override;
	virtual ~Translator() override {}
};

/// The open player Tab board owns the mouse (owner 06/10: "with tab opened i can play through the game"). Translator runs
/// at 11, AFTER the WindowTranslator (10), so a retail window under the board - the General's promotion panel, the command
/// bar - took the click before the board's H_BLOCK hit could eat it. This gate runs at 9, ahead of the windows: a press or
/// wheel turn over the open board, and the drag and release of a press that began on it (boardGateTakes), stop here - the
/// gate itself works the board's buttons (MUTE, KICK) - so no window, selection, order or camera under it sees them.
/// Outside the board nothing changes, and with no board up it passes everything.
class BoardGate : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage( const GameMessage *msg ) override;
	virtual ~BoardGate() override {}
};

/// The battlefield mouse is the HUD's whenever the GeneralsX look is a spectator's, decided NOW from the same inputs as decide():
/// a network game translates and sends its input before the frame's draw refreshes s_mode, so a player defeated in the last
/// tick would otherwise click the retail way once more (spectator-click was the panels never drawing).
/// Both guards ask this: the click capture and the OrderGate.
static Bool spectatorInput()
{
	if (retailClicks() || !inMatch())
		return FALSE;
	if (!s_loaded)
		loadMatchState();
	Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	const Bool versus = TheGameLogic->isInMultiplayerGame() || TheGameLogic->isInSkirmishGame();
	return decideMode( s_set.generalsx, gxViewerMaySeeFeed(), local != nullptr, local != nullptr && local->isPlayerObserver(), versus )
		== MODE_SPECTATOR;
}

/// A spectator's left release that was never the HUD's is drained, but its press may have reached the selection translator
/// (pressed while still a player): end the drag it may have begun, as its own release would (SelectionTranslator::
/// onRawMouseLeftButtonUp), or the box and the mouse lock would stay up with no release to end them.
static void endRetailLeftPress()
{
	// Only a drag selection's own lock and box, as that release unlocks only when it was dragging: another gesture's camera
	// lock (a held scroll key) is not this press's to end (spectator-click). The translator's own flag decides; the
	// UI's can already be cleared by a script disabling input mid-drag.
	const Bool dragging = (TheSelectionTranslator != nullptr && TheSelectionTranslator->isDragSelecting())
		|| (TheInGameUI != nullptr && TheInGameUI->isSelecting());
	if (TheSelectionTranslator != nullptr)
	{
		TheSelectionTranslator->setLeftMouseButton( FALSE );
		TheSelectionTranslator->setDragSelecting( FALSE );
	}
	if (dragging && TheInGameUI != nullptr)
	{
		if (TheTacticalView != nullptr)
			TheTacticalView->setMouseLock( FALSE );
		TheInGameUI->setSelecting( FALSE );
		TheInGameUI->endAreaSelectHint( nullptr );
	}
}

/// A spectator's right DOUBLE click goes on as a plain right press: the camera (LookAtTranslator) treats both alike, but from a
/// double click the meta map makes MSG_MOUSE_RIGHT_DOUBLE_CLICK, which with the alternate mouse and double-click attack-move on
/// becomes a MSG_DO_GUARD_POSITION order (and an AcademyStats count) with no selection or ownership check (CommandXlat.cpp;
/// spectator-click). The press keeps the double click's position, modifiers and time.
static GameMessageDisposition rightPressInstead( const GameMessage *msg )
{
	GameMessage *press = TheMessageStream->insertMessage( GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN, const_cast<GameMessage *>( msg ) );
	for (Int i = 0; i < (Int)msg->getArgumentCount(); ++i)
	{
		if (msg->getArgumentDataType( i ) == ARGUMENTDATATYPE_PIXEL)
			press->appendPixelArgument( msg->getArgument( i )->pixel );
		else
			press->appendIntegerArgument( msg->getArgument( i )->integer );
	}
	return DESTROY_MESSAGE;
}

/// The order gate, attached by GameClient.cpp at 990: after the selection (50), command (70) and hint (100) translators, so it
/// sees what they made. While this HUD is a spectator's it drops the four selection orders that retail still makes on its own:
/// changeObservedPlayer's deselect, a selection carried over to a replacement object (CreateObjectDie, ReplaceObjectUpgrade),
/// a click while F11 hides the panels. A spectator controls nothing, so each would only travel to every peer, be executed for
/// nobody and be written into the replay. Every other message passes, and with any other HUD mode, everything does.
/// GX_SPECHUD_NETPROBE=1 (tests only): one [GXSPEC] line per network-range message that reaches it, taken or passed.
class OrderGate : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage( const GameMessage *msg ) override;
	virtual ~OrderGate() override {}
};

GameMessageDisposition OrderGate::translateGameMessage( const GameMessage *msg )
{
	const GameMessage::Type t = msg->getType();
	if (t <= GameMessage::MSG_BEGIN_NETWORK_MESSAGES || t >= GameMessage::MSG_END_NETWORK_MESSAGES)
		return KEEP_MESSAGE;
	// ...and the double-click guard order, the one order retail makes with no selection or ownership check (CommandXlat.cpp,
	// MSG_MOUSE_*_DOUBLE_CLICK): a MetaEvent double-click flag left from before a defeat can still form it.
	const Bool selection = t == GameMessage::MSG_CREATE_SELECTED_GROUP || t == GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND
		|| t == GameMessage::MSG_DESTROY_SELECTED_GROUP || t == GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP
		|| t == GameMessage::MSG_DO_GUARD_POSITION;
	const Bool take = selection && spectatorInput();
	static const Bool s_probe = getenv( "GX_SPECHUD_NETPROBE" ) != nullptr;
	if (s_probe)
		fprintf( stderr, "[GXSPEC] net type=%d %s f=%u\n", (Int)t, take ? "dropped" : "sent", TheGameLogic ? TheGameLogic->getFrame() : 0u );
	return take ? DESTROY_MESSAGE : KEEP_MESSAGE;
}

static GameMessageDisposition onKey( const GameMessage *msg )
{
	const Int key = msg->getArgument( 0 )->integer;
	const Int state = msg->getArgument( 1 )->integer;
	const Bool down = msg->getType() == GameMessage::MSG_RAW_KEY_DOWN;
	Int mods = 0;
	if (state & KEY_STATE_CONTROL)
		mods |= CTRL;
	if (state & KEY_STATE_SHIFT)
		mods |= SHIFT;
	if (state & KEY_STATE_ALT)
		mods |= ALT;
	const Bool repeat = (state & KEY_STATE_AUTOREPEAT) != 0;
	if (s_mode == MODE_PLAYER)
		return onPlayerKey( key, mods, down, repeat );

	// The scoreboard is HELD: shown while the key is down, so its key-up matches whatever the modifiers are by then - but
	// only the up of a down this HUD took (s_boardHeld), and while held its auto-repeats whatever the modifiers. With a
	// modified binding (Ctrl+Up) a bare Up's down goes on to the camera, and eating its up left the camera scrolling
	// (p1-spectator).
	const Chord &b = s_chords[A_BOARD];
	if (b.valid && key == b.key && down && !repeat && mods == b.mods && s_set.show[SHOW_TAB_BOARD])		// repeats and the up: ownedKeyDrained
	{
		s_boardHeld = TRUE;
		s_boardOwnKey = key;
		return DESTROY_MESSAGE;
	}
	// Esc leaves the view and the follow (back to the whole map, free camera) - but only when there is one to leave and no
	// menu is open, so the quit menu still opens on an Esc with nothing to leave.
	if (key == KEY_ESC && mods == 0 && s_mode == MODE_SPECTATOR
		&& escapeLeavesView( currentView(), TheWindowManager == nullptr || TheWindowManager->winGetModal() != nullptr
			|| (TheInGameUI != nullptr && TheInGameUI->isQuitMenuVisible()) ))
	{
		if (down && !repeat)
		{
			applyView( viewAfterEscape( currentView() ) );
			s_escOwned = TRUE;		// its auto-repeats and up are ours too (ownedKeyDrained): a held Esc must not open the quit menu
		}
		return DESTROY_MESSAGE;
	}
	for (Int a = A_BOARD + 1; a < A_COUNT; ++a)
	{
		const Chord &c = s_chords[a];
		if (!c.valid || key != c.key || mods != c.mods)
			continue;
		if (down && !repeat)
			runAction( a );
		return DESTROY_MESSAGE;
	}
	return KEEP_MESSAGE;
}

/// A key whose DOWN a chord of ours took - the held board, a player chord - stays ours until its up: repeats and release,
/// whatever the modifiers, the mode (player -> spectator on defeat), the bar's visibility or chat do meanwhile.
/// Ownership is the physical key and nothing else. A fresh non-repeat down of an owned key means its up was eaten
/// upstream: that ownership is stale and ends here, the press then routed as new.
static Bool ownedKeyDrained( Int key, Bool down, Bool repeat )
{
	Bool taken = FALSE;
	if (s_escOwned && key == KEY_ESC)
	{
		if (!down || !repeat)
			s_escOwned = FALSE;		// its up, or a fresh down (the up was eaten upstream)
		taken = !down || repeat;
	}
	if (s_boardOwnKey != 0 && key == s_boardOwnKey)
	{
		if (down && !repeat)
		{
			s_boardOwnKey = 0;
			s_boardHeld = FALSE;
		}
		else
		{
			if (!down)
			{
				s_boardOwnKey = 0;
				s_boardHeld = FALSE;
			}
			taken = TRUE;
		}
	}
	for (Int a = PA_BAR; a < PA_COUNT; ++a)
		if (s_pOwned[a] && s_pchords[a].valid && key == s_pchords[a].key)
		{
			taken = taken || !down || repeat;
			if (!down || !repeat)
				s_pOwned[a] = FALSE;		// its up, or a fresh down (stale); an auto-repeat keeps it
		}
	return taken;
}

GameMessageDisposition Translator::translateGameMessage( const GameMessage *msg )
{
	if (msg->getType() == GameMessage::MSG_RAW_KEY_DOWN || msg->getType() == GameMessage::MSG_RAW_KEY_UP)
		if (ownedKeyDrained( msg->getArgument( 0 )->integer, msg->getType() == GameMessage::MSG_RAW_KEY_DOWN,
			(msg->getArgument( 1 )->integer & KEY_STATE_AUTOREPEAT) != 0 ))
			return DESTROY_MESSAGE;
	if (msg->getType() == GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK && spectatorInput())
		return rightPressInstead( msg );
	if (!s_inputLive && spectatorInput() && !(TheInGameUI != nullptr && TheInGameUI->isQuitMenuVisible()))
	{
		// Hidden panels (F11, the Debug screen) hide the HUD, not the spectator: his battlefield clicks still only inspect (the card
		// shows when the panels come back) and never go the retail way, whose selection and double-click guard order would be
		// sent to every peer (spectator-click). Every left message is drained, a release or drag of a press that was
		// never ours included (the meta map would make it a click). Other messages pass; a panel capture ends as below.
		if (s_capture != CAP_WORLD)
		{
			s_capture = CAP_NONE;
			s_dock.cancel();
		}
		const GameMessage::Type ht = msg->getType();
		if (msg->getArgumentCount() < 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_PIXEL)
			return KEEP_MESSAGE;
		const ICoord2D at = msg->getArgument( 0 )->pixel;
		if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN || ht == GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK)
		{
			s_capture = modalUp() ? CAP_NONE : CAP_WORLD;
			s_leftOwned = TRUE;
			s_worldPress = at;
			return DESTROY_MESSAGE;
		}
		if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_DRAG)
			return DESTROY_MESSAGE;
		if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP)
		{
			if (s_capture == CAP_WORLD && !modalUp() && isClickNotDrag( s_worldPress.x, s_worldPress.y, at.x, at.y, S( 4 ) ))
				inspectAt( at.x, at.y );
			if (!s_leftOwned)
				endRetailLeftPress();
			s_capture = CAP_NONE;
			s_leftOwned = FALSE;
			return DESTROY_MESSAGE;
		}
		return KEEP_MESSAGE;
	}
	if (!s_inputLive)
	{
		// The HUD is not up (quit menu, hidden overlay): act on nothing, but let a held scoreboard go, or it would
		// still be open when the HUD comes back.
		const GameMessage::Type ht = msg->getType();
		s_capture = CAP_NONE;
		s_dock.cancel();
		if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN || ht == GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK)
			s_leftOwned = FALSE;		// a fresh press while hidden: not ours
		else if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_DRAG || ht == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP)
		{
			// The press was ours before the HUD hid (F11): its release must not reach the meta map as a click that could
			// execute a pending targeted command.
			if (s_leftOwned)
			{
				if (ht == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP)
					s_leftOwned = FALSE;
				return DESTROY_MESSAGE;
			}
		}
		else if (ht == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN || ht == GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK)
			s_rightOwned = FALSE;		// a fresh press while hidden: an earlier release that never reached us is not ours to wait for
		else if (ht == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP || ht == GameMessage::MSG_RAW_MOUSE_RIGHT_DRAG)
		{
			if (s_rightOwned)
			{
				if (ht == GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP)
					s_rightOwned = FALSE;
				return DESTROY_MESSAGE;
			}
		}
		return KEEP_MESSAGE;
	}
	const GameMessage::Type t = msg->getType();
	if (t == GameMessage::MSG_RAW_KEY_DOWN || t == GameMessage::MSG_RAW_KEY_UP)
		return onKey( msg );

	const MouseIO *m = TheMouse ? TheMouse->getMouseStatus() : nullptr;
	if (m == nullptr || msg->getArgumentCount() < 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_PIXEL)
		return KEEP_MESSAGE;
	// Each mouse message carries the position it happened at (Mouse::createStreamMessages). Several can queue in one
	// client update and the status holds only the last, so reading it made a press-drag-release inside one update a
	// click (p1-spectator). The status is still what the stale-capture test below reads.
	const ICoord2D at = msg->getArgument( 0 )->pixel;
	const Int x = at.x, y = at.y;
	switch (t)
	{
		case GameMessage::MSG_RAW_MOUSE_POSITION:
		case GameMessage::MSG_RAW_MOUSE_LEFT_DRAG:
			// A retail window (the replay controls, say) can eat the release (WindowTranslator, priority 10): a
			// capture whose button is no longer down ends instead of dragging a panel with no button held. NOT on the
			// first such message: Mouse::createStreamMessages appends the frame's position BEFORE its button events,
			// and translators read m_currMouse after all of them, so a normal release frame shows "up" here one
			// message before its own LEFT_BUTTON_UP arrives. Two in a row = the up was eaten.
			// Counted on POSITION only: it is appended exactly once per frame, while a LEFT_DRAG can precede the UP inside
			// the very release frame (a 1 px jitter on a click), which made it a false second strike.
			if (t == GameMessage::MSG_RAW_MOUSE_POSITION && s_capture != CAP_NONE && m->leftState != MBS_Down
				&& m->leftState != MBS_DoubleClick)
			{
				if (++s_staleMoves < 2)
					return KEEP_MESSAGE;
				if (s_capture == CAP_SLIDER)
					saveSettings();
				fprintf( stderr, "[GXSPEC] capture %d cancelled: its release never arrived\n", (Int)s_capture );
				s_capture = CAP_NONE;
				s_leftOwned = FALSE;
				s_staleMoves = 0;
				s_dock.cancel();
				return KEEP_MESSAGE;
			}
			if (t == GameMessage::MSG_RAW_MOUSE_POSITION)
				s_staleMoves = 0;
			if (s_capture == CAP_DOCK)
				s_dock.move( x, y );
			else if (s_capture == CAP_MAP)
				minimapLookAt( x, y );
			else if (s_capture == CAP_SLIDER)
				setOpacityFromX( x );
			else if (s_capture == CAP_SCROLL)
				listScrollToY( y );
			// Positions keep flowing to the cursor code; a drag that we own does not, nor any left drag of a spectator's (no box select).
			return (t == GameMessage::MSG_RAW_MOUSE_LEFT_DRAG && (s_capture != CAP_NONE || spectatorInput())) ? DESTROY_MESSAGE : KEEP_MESSAGE;

		case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN:
		case GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK:
		{
			// A fresh left press while a capture is still open: that capture's release never reached us (a retail window
			// ate it), so it is retired HERE, at this press's place in the stream - never from BoardGate, which runs a
			// whole batch ahead of this translator.
			if (s_capture != CAP_NONE)
			{
				if (s_capture == CAP_SLIDER)
					saveSettings();
				fprintf( stderr, "[GXSPEC] capture %d retired: a new press came before its release\n", (Int)s_capture );
				s_capture = CAP_NONE;
				s_staleMoves = 0;
				s_dock.cancel();
			}
			const Hit *h = hitAt( x, y );
			if (h == nullptr && spectatorInput())
			{
				// A spectator's press on the battlefield is ours (inspectAt on its release): no box select, no order, and the
				// meta map never makes it a click for the selection translator, whose selection would be sent to every peer.
				// Behind a modal popup it is drained and inspects nothing.
				s_leftOwned = TRUE;
				s_capture = modalUp() ? CAP_NONE : CAP_WORLD;
				s_worldPress = at;
				s_staleMoves = 0;
				return DESTROY_MESSAGE;
			}
			if (h == nullptr)
			{
				s_leftOwned = FALSE;
				return KEEP_MESSAGE;
			}
			s_leftOwned = TRUE;
			s_pressHit = *h;
			s_staleMoves = 0;
			if (h->kind == H_MAP)
			{
				s_capture = CAP_MAP;
				minimapLookAt( x, y );
			}
			else if (h->kind == H_SLIDER)
			{
				s_capture = CAP_SLIDER;
				setOpacityFromX( x );
			}
			else if (h->kind == H_SCROLL)
			{
				s_capture = CAP_SCROLL;
				s_listDrag = FALSE;
				listScrollPress( *h, y );
			}
			else
			{
				s_capture = CAP_DOCK;
				s_dock.press( h->panel, h->kind == H_TAB, x, y, h->panel == PANEL_FOCUS ? s_focusHidden : 0 );
			}
			return DESTROY_MESSAGE;
		}

		case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP:
		{
			const Capture c = s_capture;
			const Bool owned = s_leftOwned;
			s_capture = CAP_NONE;
			s_leftOwned = FALSE;
			s_staleMoves = 0;
			s_listDrag = FALSE;
			if (c == CAP_NONE && !owned && spectatorInput())
			{
				endRetailLeftPress();	// a spectator's release is never the meta map's, even one whose press was not ours
				return DESTROY_MESSAGE;
			}
			if (c == CAP_NONE)
				return owned ? DESTROY_MESSAGE : KEEP_MESSAGE;
			if (c == CAP_SLIDER)
				saveSettings();
			else if (c == CAP_WORLD)
			{
				// Not where a panel now is: one opened under the cursor, or a release just inside one (spectator-click).
				if (isClickNotDrag( s_worldPress.x, s_worldPress.y, x, y, S( 4 ) ) && hitAt( x, y ) == nullptr && !modalUp())
					inspectAt( x, y );
			}
			else if (c == CAP_DOCK && (s_pressHit.kind == H_TOAST || s_pressHit.kind == H_CHIP || s_pressHit.kind == H_BARPAGE))
			{
				// Panel -1: the dock tracks no press for it (Dock::press ignores the id), so Dock::release would say NONE and the
				// click would never act. A click = released over the same toast it was pressed on.
				const Hit *up = hitAt( x, y );
				if (up != nullptr && up->kind == s_pressHit.kind && up->id == s_pressHit.id)
					act( s_pressHit );
			}
			else if (c == CAP_DOCK)
			{
				const Int panel = s_dock.pressed;
				const Dock::Release r = s_dock.release( x, y );
				fprintf( stderr, "[GXSPEC] release=%d panel=%d open=%d dx=%d dy=%d\n", (Int)r, panel,
					panel >= 0 ? (Int)s_dock.panel[panel].open : -1, panel >= 0 ? s_dock.panel[panel].dx : 0, panel >= 0 ? s_dock.panel[panel].dy : 0 );
				if (r == Dock::RELEASE_BODY_CLICK)
					act( s_pressHit );
			}
			return DESTROY_MESSAGE;
		}

		case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN:
		case GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK:
			// While playing, a right click on a panel is not an order to the ground under it.
			// A spectator's right click orders nothing, so it passes as before.
			if (s_mode == MODE_PLAYER && hitAt( x, y ) != nullptr)
			{
				s_rightOwned = TRUE;
				return DESTROY_MESSAGE;
			}
			s_rightOwned = FALSE;		// a fresh press elsewhere: an earlier release that never reached us is not ours to wait for
			return KEEP_MESSAGE;

		case GameMessage::MSG_RAW_MOUSE_RIGHT_DRAG:
			return s_rightOwned ? DESTROY_MESSAGE : KEEP_MESSAGE;

		case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP:
			if (!s_rightOwned)
				return KEEP_MESSAGE;
			s_rightOwned = FALSE;
			return DESTROY_MESSAGE;

		default:
			return KEEP_MESSAGE;
	}
}

/// The wheel over a list that scrolls, or over a half of the band that pages: ours, the camera keeps it everywhere else. One notch =
/// one row or one card; a touchpad's fractions wait until they make a whole notch. Up moves back, like every list.
static Bool hudWheel( Int x, Int y, Real notches )
{
	if (!s_inputLive || modalUp())
		return FALSE;
	const Bool list = s_listBox.w > 0 && s_listBox.contains( x, y );
	Int side = -1;
	for (Int i = 0; !list && i < 2; ++i)
		if (s_barPageMax[i] >= 0 && s_barSide[i].w > 0 && s_barSide[i].contains( x, y ))
			side = i;
	if (!list && side < 0)
		return FALSE;
	s_wheelRest += notches;
	const Int steps = (Int)s_wheelRest;
	s_wheelRest -= (Real)steps;
	if (list)
		listScrollBy( -steps );
	else
		s_barFirst[side] -= steps;
	return TRUE;
}

GameMessageDisposition BoardGate::translateGameMessage( const GameMessage *msg )
{
	const GameMessage::Type t = msg->getType();
	if (t == GameMessage::MSG_RAW_KEY_UP && s_boardOwnKey != 0 && msg->getArgumentCount() > 0 && msg->getArgument( 0 )->integer == s_boardOwnKey)
		s_boardKeyUp = TRUE;		// noted, not taken: Translator still drains it and closes the board
	if (t <= GameMessage::MSG_RAW_MOUSE_BEGIN || t >= GameMessage::MSG_RAW_MOUSE_END
		|| msg->getArgumentCount() < 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_PIXEL)
		return KEEP_MESSAGE;
	Int button = 0, kind = GATE_FOLLOW;
	Bool down = FALSE, up = FALSE;
	switch (t)
	{
		case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN: case GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK: button = 1; down = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP: button = 1; up = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_LEFT_DRAG: case GameMessage::MSG_RAW_MOUSE_LEFT_CLICK: button = 1; break;
		case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN: case GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK: button = 2; down = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP: button = 2; up = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_RIGHT_DRAG: button = 2; break;
		case GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_DOWN: case GameMessage::MSG_RAW_MOUSE_MIDDLE_DOUBLE_CLICK: button = 4; down = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_UP: button = 4; up = TRUE; break;
		case GameMessage::MSG_RAW_MOUSE_MIDDLE_DRAG: button = 4; break;
		case GameMessage::MSG_RAW_MOUSE_WHEEL: kind = GATE_WHEEL; break;
		default: kind = GATE_POSITION; break;
	}
	if (down)
		kind = GATE_PRESS;
	const ICoord2D at = msg->getArgument( 0 )->pixel;
	if (kind == GATE_WHEEL && msg->getArgumentCount() >= 2 && hudWheel( at.x, at.y, msg->getArgument( 1 )->real ))
		return DESTROY_MESSAGE;
	// A fresh press is judged by where it lands alone: a release that never reached us (focus lost mid-press) must not
	// make the next press elsewhere the board's.
	const Bool owned = button != 0 && !down && (s_boardPress & button) != 0;
	// A modal window (a scripted popup that pauses the match) wins fresh input over the board, which is not drawn then
	// either (drawPlayerMode).
	const Bool open = s_inputLive && s_mode == MODE_PLAYER && (TheWindowManager == nullptr || TheWindowManager->winGetModal() == nullptr);
	if (!boardGateTakes( open, s_boardRect, at.x, at.y, kind, owned ))
	{
		if (down)
			s_boardPress &= ~button;
		if (s_puiTest && (down || up) && s_boardRect.w > 0)		// tests only (never in a real player's log)
			fprintf( stderr, "[GXPUI] gate passed %s button=%d at %d,%d f=%u\n", down ? "down" : "up", button, at.x, at.y, TheGameLogic->getFrame() );
		return KEEP_MESSAGE;
	}
	if (s_puiTest && (down || up))
		fprintf( stderr, "[GXPUI] gate took %s button=%d at %d,%d f=%u\n", down ? "down" : "up", button, at.x, at.y, TheGameLogic->getFrame() );
	// The board's own buttons are handled HERE, in stream order, with the gate's own press record - never through the
	// shared Translator capture state: the gate (9) sees a whole batch before Translator (11) does, so a newer press on the
	// board would otherwise become the "capture" an older battlefield release then completed.
	if (down)
	{
		s_boardPress |= button;
		if (button == 1)
		{
			const Hit *h = hitAt( at.x, at.y );
			s_boardPressHit.kind = -1;
			s_listDrag = FALSE;
			if (h != nullptr)
				s_boardPressHit = *h;
			if (h != nullptr && h->kind == H_SCROLL)
				listScrollPress( *h, at.y );
		}
	}
	else if (up)
	{
		s_boardPress &= ~button;
		if (button == 1 && (s_boardPressHit.kind == H_MUTE || s_boardPressHit.kind == H_KICK))
		{
			// A click is a press and a release on the same button, still on screen, while the board is still held: not
			// after Tab was let go, nor in the batch its release is queued in.
			// ...and not behind a modal popup that opened meanwhile: the release is still drained.
			const Hit *now = hitAt( at.x, at.y );
			if (open && now != nullptr && now->kind == s_boardPressHit.kind && now->id == s_boardPressHit.id
				&& (s_pBoardForced || (s_boardHeld && !s_boardKeyUp)))
				act( s_boardPressHit );
		}
		if (button == 1)
		{
			s_boardPressHit.kind = -1;
			s_listDrag = FALSE;
		}
	}
	else if (button == 1)
		listScrollToY( at.y );
	return DESTROY_MESSAGE;
}

/// The bottom bar's Communicator button (owner 06/10: it "opens the old players screen"): in the GeneralsX player look it
/// latches the Tab board open and shut - the same rows (mute, ping, fps, team, status) - instead of retail's Diplomacy
/// window. FALSE = not ours (legacy look, the panels hidden with F11, a spectator, the board switched off): retail's runs.
static Bool communicatorToggle()
{
	if (s_mode != MODE_PLAYER || !s_set.playerTabBoard || s_overlayHiddenNow || !inMatch())
		return FALSE;
	s_pBoardForced = !s_pBoardForced;
	return TRUE;
}

} // namespace GXSpecHud

extern Bool (*gxCommunicatorHook)();	// ControlBarCallback.cpp
extern Color (*gxChromeFadeColorHook)( GameWindow * );		// GameWindowTransitionsStyles.cpp
static const struct GXCommunicatorHookInit { GXCommunicatorHookInit() { gxCommunicatorHook = &GXSpecHud::communicatorToggle; gxChromeFadeColorHook = &GXSpecHud::chromeFadeColor; } } s_gxCommunicatorHookInit;

static Bool gxSpectatorHudDraw() { return GXSpecHud::draw(); }
static void gxSpectatorHudNotDrawn() { GXSpecHud::clearReticle(); GXSpecHud::s_inputLive = FALSE; GXSpecHud::s_topInset = 0; GXSpecHud::s_insetSpanOn = FALSE; GXSpecHud::s_toastLineCount = 0; GXSpecHud::s_boardRect.w = 0; }
/// InGameUI::postDraw's first call every frame, before the message list: a new frame for toastLayoutForFrame.
static void gxSpectatorHudUnderWindows( Bool overlayHidden ) { ++GXSpecHud::s_frameSerial; GXSpecHud::underWindows( overlayHidden ); }
static void gxSpectatorHudUpdate() { GXSpecHud::trackGeneralPointUpdate(); }
Int gxSpectatorHudTopInset() { return GXSpecHud::s_topInset; }
/// The span [x0,x1) x [y0, inset) the inset protects, FALSE when there is no inset this frame. The bar's HOME rect (and the toast), never
/// the dragged position: the message list must not move when the strip does (owner 06/10).
Bool gxSpectatorHudInsetSpan( Int *x0, Int *x1, Int *y0 )
{
	if (GXSpecHud::s_topInset <= 0 || !GXSpecHud::s_insetSpanOn) return FALSE;
	*x0 = GXSpecHud::s_insetX0; *x1 = GXSpecHud::s_insetX1; *y0 = GXSpecHud::s_insetY0;
	return TRUE;
}
/// Where the event toasts can be this frame, [x0,x1) x [y0,y1) - every place of the stack, the drop and the leaving fourth, outer lines
/// included - while at least one announcement is alive (TOAST_LIFE_MS); FALSE when none is, the toasts are off, the spectator HUD is
/// not up or the stack has no free place. The retail message list (InGameUI::postDraw) runs BEFORE the HUD draws (postWindowDraw), so
/// this is computed when asked, never kept from the last frame: a toast about to appear or slide is already
/// in it. The toasts no longer move s_topInset (a burst of three would push the superweapon timers and the message list a whole stack
/// down and back up every few seconds): the list keeps only a line that would run into this zone off it.
Bool gxSpectatorHudToastZone( Int *x0, Int *x1, Int *y0, Int *y1 )
{
	using namespace GXSpecHud;
	s_zoneAskedSerial = s_frameSerial;	// drawToasts draws this frame only where this answer said it may (s_zoneGiven)
	s_zoneGiven = FALSE;
	if (!s_inputLive || s_mode != MODE_SPECTATOR || !s_set.show[SHOW_EVENT_TOAST] || TheInGameUI == nullptr || s_W <= 0)
		return FALSE;
	const UnsignedInt now = timeGetTime();
	Bool alive = FALSE;
	for (Int k = 0; k < kToastRing && !alive; ++k)
	{
		UnicodeString text;
		Color color = 0;
		UnsignedInt born = 0, frame = 0, power = 0;
		Int about = -1, science = -1, slot = -1;
		ObjectID object = INVALID_ID;
		if (!TheInGameUI->gxNewestSkillBanner( k, text, color, born, about, object, science, frame, power, slot ))
			break;
		alive = toastShows( born, now, TOAST_LIFE_MS, s_matchStartMs, s_loaded );
	}
	if (!alive)
		return FALSE;
	const ToastLayout L = toastLayoutForFrame();		// the place drawToasts will use this frame
	if (!L.clear)
		return FALSE;
	const Rect z = toastZone( L.x, L.top, L.w, L.h, L.step, S( TOAST_DROP ) );
	*x0 = z.x - 1; *x1 = z.x + z.w + 1; *y0 = z.y - 1; *y1 = z.y + z.h + 1;
	s_zoneGiven = TRUE;
	return TRUE;
}
static void gxSpectatorHudReset() { GXSpecHud::reset(); }
static void gxSpectatorHudFreeStrings() { GXSpecHud::freeStrings(); }
/// The player Tab board is on screen this frame (banner stays out of its way).
static inline Bool gxSpectatorHudBoardUp() { return GXSpecHud::s_boardRect.w > 0; }

/// GameClient.cpp attaches this right after the WindowTranslator (10), so a chat box or menu with focus still wins,
/// and ahead of the MetaEventTranslator (20), so Tab never reaches the retail Diplomacy toggle for a spectator in
/// this mode. Every other message, and every message when the HUD is not up, passes through untouched.
GameMessageTranslator *gxNewSpectatorHudTranslator()
{
	return MSGNEW( "GameClientSubsystem" ) GXSpecHud::Translator;
}

/// GameClient.cpp attaches this at 990, after every translator that can make an order and before the client dispatcher.
GameMessageTranslator *gxNewSpectatorHudOrderGate()
{
	return MSGNEW( "GameClientSubsystem" ) GXSpecHud::OrderGate;
}

/// GameClient.cpp attaches this at 9, AHEAD of the WindowTranslator: the open player Tab board's mouse gate (BoardGate).
GameMessageTranslator *gxNewSpectatorHudBoardGate()
{
	return MSGNEW( "GameClientSubsystem" ) GXSpecHud::BoardGate;
}

/// The nth newest announcement in the skill-banner ring (0 = the newest), live or already faded (the toast keeps it longer), and the
/// ring slot it sits in. FALSE past the last one there is.
Bool InGameUI::gxNewestSkillBanner( Int nth, UnicodeString &text, Color &color, UnsignedInt &bornMs, Int &playerIndex, ObjectID &objectID,
	Int &science, UnsignedInt &frame, UnsignedInt &powerID, Int &ringSlot ) const
{
	// Newest first by birth (the ring's write order is the same order; ties keep it).
	Int order[SKILL_BANNER_SLOTS];
	Int count = 0;
	for (Int i = 0; i < SKILL_BANNER_SLOTS; ++i)
	{
		const Int n = (m_skillBannerNext - 1 - i + 2 * SKILL_BANNER_SLOTS) % SKILL_BANNER_SLOTS;
		const SkillBanner &b = m_skillBanners[n];
		if (b.text == nullptr || b.bornMs == 0)
			continue;
		Int at = count++;
		while (at > 0 && (Int)(b.bornMs - m_skillBanners[order[at - 1]].bornMs) > 0)
		{
			order[at] = order[at - 1];
			--at;
		}
		order[at] = n;
	}
	if (nth < 0 || nth >= count)
		return FALSE;
	const SkillBanner &best = m_skillBanners[order[nth]];
	text = best.text->getText();
	color = best.color;
	bornMs = best.bornMs;
	playerIndex = best.playerIndex;
	objectID = best.objectID;
	science = best.science;
	frame = best.frame;
	powerID = best.powerID;
	ringSlot = order[nth];
	return TRUE;
}

/// Called by the simulation where it triggers a special power (SpecialPowerModule::triggerSpecialPower), on every peer alike:
/// it only copies the caster, the power and the point into this client's map (the newest per caster and power) - deferred to
/// the end of the tick like the banners, so the HUD reads it on its own side - and reads nothing back into the simulation.
void InGameUI::gxNoteSpecialPowerTarget( const Object *caster, const SpecialPowerTemplate *power, const Coord3D *target )
{
	if (caster == nullptr || power == nullptr || target == nullptr || caster->getControllingPlayer() == nullptr || TheGameLogic == nullptr)
		return;
	GXSpectatorHud::PowerNote note;
	note.caster = (unsigned)caster->getID();
	note.player = (int)caster->getControllingPlayer()->getPlayerIndex();
	note.science = (int)power->getRequiredScience();
	note.power = power->getID();
	note.frame = TheGameLogic->getFrame();
	note.live = true;
	const Coord3D at = *target;
	GXRenderSplit &split = GXRenderSplit::get();
	if (split.simSide())
	{
		split.defer( GXRenderSplit::Banner, note.frame, [note, at]() { GXSpecHud::notePowerTarget( note, at ); } );
		return;
	}
	GXSpecHud::notePowerTarget( note, at );
}

/// The decal the power's caster saw while aiming it: its command button's RadiusCursorType at the power's cursor radius. Made for
/// the LOCAL player - the cursor templates are OnlyVisibleToOwningPlayer and the spectator is the one looking - in the template's
/// own colour (InGameUI.ini gives every power cursor one), so it looks as it did to the caster.
Bool InGameUI::gxPowerTargetDecal( const SpecialPowerTemplate *power, const Coord3D &at, RadiusDecal &out ) const
{
	out.clear();
	Player *viewer = ThePlayerList != nullptr ? ThePlayerList->getLocalPlayer() : nullptr;
	if (power == nullptr || viewer == nullptr || TheControlBar == nullptr || power->getRadiusCursorRadius() <= 0.0f)
		return FALSE;
	for (const CommandButton *b = TheControlBar->getCommandButtons(); b != nullptr; b = b->getNext())
	{
		const Int type = (Int)b->getRadiusCursorType();
		if (b->getSpecialPowerTemplate() == power && type > (Int)RADIUSCURSOR_NONE && type < (Int)RADIUSCURSOR_COUNT)
		{
			m_radiusCursors[type].createRadiusDecal( at, power->getRadiusCursorRadius(), viewer, out );
			return !out.isEmpty();
		}
	}
	return FALSE;
}
