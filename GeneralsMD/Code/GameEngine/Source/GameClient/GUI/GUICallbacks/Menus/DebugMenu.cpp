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

// FILE: DebugMenu.cpp /////////////////////////////////////////////////////////////////////////////
// Desc:   Code-built Debug overlay: stats, live player panel, LAN-safe cheats, camera tuning,
//         config and saves
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/Energy.h"
#include "Common/FileSystem.h"
#include "Common/FramePacer.h"
#include "Common/GameCommon.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GameUtility.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "Common/MessageStream.h"
#include "Common/Money.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Recorder.h"
#include "Common/Radar.h"
#include "Common/ThingFactory.h"
#include "Common/ThingSort.h"
#include "Common/ThingTemplate.h"

#include "GameClient/CodeGui.h"
#include "GameClient/CodeGuiSearchField.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/DebugMenu.h"
#include "GameClient/GXDebugIntelGate.h"	// who may read other players' state off this menu
#include "GameClient/GXSpectatorFeed.h"
#include "GameClient/GXAdminKey.h"	// the admin's match key, for the refusal line
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/SaveLoadFeedback.h"
#include "GameClient/Shell.h"
#include "GameClient/View.h"
#include "GameClient/GXHostCamera.h"
#include "GameClient/WindowLayout.h"

#include "GameNetwork/GameInfo.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"

#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"	// TheNetwork: the local slot an admin order is signed as

#include <algorithm>

//-------------------------------------------------------------------------------------------------
// Layout, in the 800x600 authoring space the stock .wnd files use. CodeGuiScale* maps it onto the
// real backbuffer; runtime-created windows get none of the parser's CREATIONRESOLUTION scaling.
//-------------------------------------------------------------------------------------------------
enum
{
	DBG_PANEL_X			=  60, DBG_PANEL_Y		=  40,
	DBG_PANEL_W			= 680, DBG_PANEL_H		= 520,
	DBG_PAD					=  12,
	DBG_TITLE_H			=  22,
	DBG_TAB_Y				=  40, DBG_TAB_H			=  26, DBG_TAB_GAP = 6,
	DBG_CONTENT_Y		=  76,
	DBG_LINE_H			=  20,
	DBG_ROW_H				=  30,
	DBG_BTN_H				=  28,
	DBG_FOOTER_BTN_W = 130, DBG_FOOTER_BTN_H = 30
};

// Spawn is its OWN tab, not another block on Cheats.
//
// The panes are a fixed contentH = 520 - 76 - 40*... == 390 authoring-Y units tall, and the Cheats
// pane already ran to 354 of them. A template picker needs a listbox, a search row, two steppers and
// four action buttons - roughly another 200. Squeezing that in would have put controls PAST the
// bottom of the pane, and a child outside its parent's rect is not merely ugly: winPointInWindow
// hit-tests against the parent first, so an off-pane button is invisible AND unclickable.
enum
{
	DBG_TAB_STATS = 0,
	DBG_TAB_PLAYERS,
	DBG_TAB_CHEATS,
	DBG_TAB_SPAWN,
	DBG_TAB_CAMERA,
	DBG_TAB_CONFIG,
	DBG_TAB_SAVES,
	DBG_TAB_COUNT
};

enum { DBG_NUM_STAT_LINES = 9 };
enum { DBG_NUM_SLIDERS = 5 };

/// One row per player slot. MAX_PLAYER_COUNT is 16 (GameCommon.h); the neutral player is skipped, so
/// this is always one row more than we can fill, which costs eight windows and buys not having to
/// care whether a map ever ships with a full sixteen.
enum { DBG_PL_ROWS = MAX_PLAYER_COUNT };
enum { DBG_PL_COLS = 8 };

/// Read-only config report and the pref editor share one refresh; both are capped so a fat install
/// cannot spend a second building a listbox nobody scrolls to the bottom of.
enum { DBG_MAX_REPORT_ROWS = 200 };

/// How many template rows the picker will actually put in the listbox at once.
///
/// Not a cosmetic cap. GadgetListBoxAddEntryText allocates a DisplayString per row and calls
/// setText on it, which lays the glyphs out immediately (GadgetListBox.cpp addEntry), and the list
/// is rebuilt on EVERY keystroke of the search field. Zero Hour ships a few thousand Object
/// templates, so an uncapped refill would re-lay out thousands of strings per character typed.
/// Anything past the cap is reported as a "... N more" row instead, which is also the honest UI:
/// if the answer is off the end of two hundred rows, scrolling is not how you were going to find it.
enum { DBG_MAX_SPAWN_ROWS = 200 };

/// The handler's own clamp, GX_CHEAT_MAX_SPAWN_COUNT in GameLogicDispatch.cpp. Mirrored here rather
/// than shared because that constant is a file static in a .cpp; the UI clamps so the user sees the
/// real number they are about to get instead of silently asking for more than the wire will honour.
enum { DBG_MAX_SPAWN_COUNT = 50 };

//-------------------------------------------------------------------------------------------------
static const WideChar *s_tabLabels[ DBG_TAB_COUNT ] =
{
	L"Stats", L"Players", L"Cheats", L"Spawn", L"Camera", L"Config", L"Saves"
};

/// Count ladder for the spawn/delete stepper. A plain +1/-1 step would be forty taps to reach the
/// ceiling on a touch screen; this is the same "step one preset" idiom the render-cap buttons on the
/// Stats tab already use. The last rung IS DBG_MAX_SPAWN_COUNT, so the top of the ladder is exactly
/// the wire's ceiling and the UI can never ask for a number the handler will quietly reduce.
static const Int s_spawnCountSteps[] = { 1, 2, 3, 4, 5, 10, 15, 20, 25, 30, 40, DBG_MAX_SPAWN_COUNT };
enum { DBG_NUM_SPAWN_COUNTS = sizeof( s_spawnCountSteps ) / sizeof( s_spawnCountSteps[0] ) };

struct DebugSliderSpec
{
	const char		 *idName;
	const WideChar *label;
	const char		 *prefKey;
	Int							minVal;
	Int							maxVal;
	Int							defVal;
};

// Ranges and pref keys are carried over verbatim from the Extras screen (ExtrasMenu.cpp), which is
// still shipped and still works; this screen is the new home for the same five settings.
static const DebugSliderSpec s_sliderSpecs[ DBG_NUM_SLIDERS ] =
{
	// Max up to the whole map (the view stops there; 50000 bounds the setting), min down to 5: beyond the launcher's
	// 1000 / 50, so Apply marks a pair outside that range (View::setPrefDebugCameraHeights) and it holds across a restart.
	// The pitch row is 20-60, or 5-90 while Uncap pitch is on (setPitchSliderRange). Defaults restore Generals Online's
	// 310 / 100.
	{ "DebugMenu:SliderMaxCameraHeight", L"Max camera height",	"MaxCameraHeight",	View::CameraMaxHeightLo, View::DebugCameraMaxHeightHi, 310 },
	{ "DebugMenu:SliderMinCameraHeight", L"Min camera height",	"MinCameraHeight",	View::DebugCameraMinHeightLo, View::CameraMinHeightHi, 100 },
	{ "DebugMenu:SliderCameraPitch",		 L"Camera pitch",				"CameraPitch",	View::CameraPitchLo, View::CameraPitchHi,  37 },
	{ "DebugMenu:SliderScrollSpeed",		 L"Scroll speed",				"ScrollFactor",							  1,  200, 100 },
	{ "DebugMenu:SliderDrawDistance",		 L"Draw distance",			"TerrainDrawDistanceScale",	100,  200, 105 }
};

//-------------------------------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------
static WindowLayout			*s_layout			= nullptr;
static GameWindow				*s_root				= nullptr;
static GameWinDrawFunc	 s_rootDraw		= nullptr;		///< chained; see debugMenuRootDraw

static GameWindow *s_tabButton[ DBG_TAB_COUNT ]	= { nullptr };
static GameWindow *s_tabPanel [ DBG_TAB_COUNT ]	= { nullptr };
static Int				 s_activeTab									= DBG_TAB_STATS;
// The text field the player typed in, kept while its tab is up - see debugMenuRootDraw.
static GameWindow *s_keyboardKept = nullptr;

static GameWindow *s_buttonBack			= nullptr;

// Stats
static GameWindow *s_statLine[ DBG_NUM_STAT_LINES ] = { nullptr };

// Stats - frame pacing block
static GameWindow *s_fpsRenderValue			= nullptr;
static GameWindow *s_fpsLogicValue			= nullptr;
static GameWindow *s_fpsSpeedValue			= nullptr;
static GameWindow *s_fpsNote						= nullptr;
static GameWindow *s_buttonRenderFpsDown	= nullptr;
static GameWindow *s_buttonRenderFpsUp		= nullptr;
static GameWindow *s_buttonLogicFpsDown		= nullptr;
static GameWindow *s_buttonLogicFpsUp			= nullptr;
static GameWindow *s_ffValue						= nullptr;	// fast forward (changeFastForward)
static GameWindow *s_buttonFastForwardDown	= nullptr;
static GameWindow *s_buttonFastForwardUp		= nullptr;

// Cheats
static GameWindow *s_cheatStatus		= nullptr;
static GameWindow *s_cheatResult		= nullptr;
static GameWindow *s_buttonGiveCash	= nullptr;
static GameWindow *s_buttonReveal		= nullptr;

// Cheats - target selection and the networked actions
static GameWindow *s_listCheatTarget	= nullptr;
static GameWindow *s_cheatTargetInfo	= nullptr;
static GameWindow *s_entryCashAmount	= nullptr;
static GameWindow *s_buttonSetCash		= nullptr;
static GameWindow *s_buttonUnreveal		= nullptr;
static GameWindow *s_buttonForceRadar	= nullptr;
static GameWindow *s_buttonKillPlayer	= nullptr;
static GameWindow *s_buttonKillUnits	= nullptr;
static GameWindow *s_buttonKillAll		= nullptr;

/// Listbox row -> player index. Same pattern as s_prefRowKeys: the listbox item-data slot is a raw
/// void* and must not be handed anything with a lifetime.
static std::vector<Int> s_cheatTargetIndex;

/// Change detection for the target listbox. Rebuilding a listbox four times a second would reset the
/// selection under the user's finger, so the list is only rebuilt when the roster actually changed.
/// NameKeyType is an Int, so this caches integers - never Player pointers, which would dangle across
/// a match transition and be compared after the fact.
static Int					s_cheatRosterCount							= -1;
static NameKeyType	s_cheatRosterKey[ MAX_PLAYER_COUNT ]	= { NAMEKEY_INVALID };

/// Client-side radar force. See toggleForceRadar() for why this is not a cheat message.
/// s_forceRadarShown is what the button's label currently says (-1 == unknown, i.e. freshly built);
/// GadgetButtonSetText posts a GGM_SET_LABEL, so it is only worth doing when the state changed.
static Bool s_forceRadar			= FALSE;
static Int	s_forceRadarShown	= -1;

// Spawn tab
//
// A searchable template picker, since typing exact names on a phone is impractical.
// The selection is stored as the INI name, not a template id or row index: ids are reassigned on data reload.
struct DebugSpawnEntry
{
	AsciiString		name;						///< the INI Object name; the selection's backing value
	UnicodeString	label;					///< display name, falling back to the INI name
	AsciiString		search;					///< lowercased "label name", so one strstr filters on both
	Bool					spawnable;			///< survived debugTemplateLooksSpawnable()
	Bool					hasDisplayName;	///< FALSE when label IS the INI name, so the row does not say it twice
};

/// Every template in the factory, sorted, built once per screen lifetime. Holds ALL of them with a
/// per-entry flag rather than only the spawnable ones, so the "Show all" toggle costs no rebuild.
static std::vector<DebugSpawnEntry>	s_spawnCatalog;
static Bool													s_spawnCatalogBuilt	= FALSE;

/// Listbox row -> index into s_spawnCatalog. Same pattern as s_cheatTargetIndex and s_prefRowKeys:
/// the listbox's own item-data slot is a raw void* that must not be handed anything with a lifetime.
/// A -1 marks the trailing "... N more" row, which is text, not a template.
static std::vector<Int> s_spawnRowIndex;

static AsciiString s_spawnPick;										///< INI name of the armed template, "" for none
static Bool				 s_spawnShowAll	= FALSE;				///< FALSE == apply the sanity filter
static Int				 s_spawnCountIdx = 0;						///< index into s_spawnCountSteps
static Int				 s_spawnVet			= (Int)LEVEL_REGULAR;

static CodeGuiSearchField s_spawnSearch;
// The search text, held across a teardown. s_spawnSearch
// itself cannot hold it: reset() clears m_filter because the field's window pointers die with
// the pane, so the text has to live somewhere that outlives the layout.
static UnicodeString s_spawnSearchSaved;

static GameWindow *s_listSpawnTemplate		= nullptr;
static GameWindow *s_spawnTargetInfo			= nullptr;
static GameWindow *s_spawnPickLabel				= nullptr;
static GameWindow *s_spawnCountValue			= nullptr;
static GameWindow *s_spawnVetValue				= nullptr;
static GameWindow *s_spawnResult					= nullptr;
static GameWindow *s_buttonSpawnScope			= nullptr;
static GameWindow *s_buttonSpawnCountDown	= nullptr;
static GameWindow *s_buttonSpawnCountUp		= nullptr;
static GameWindow *s_buttonSpawnVetPrev		= nullptr;
static GameWindow *s_buttonSpawnVetNext		= nullptr;
static GameWindow *s_buttonSpawn					= nullptr;
static GameWindow *s_buttonSpawnDelete		= nullptr;
static GameWindow *s_buttonSpawnDeleteType	= nullptr;
static GameWindow *s_buttonSpawnDeleteAll		= nullptr;

// Players tab - a grid of labels, because CodeGuiListbox hard-codes one column.
static GameWindow *s_plCell[ DBG_PL_COLS ][ DBG_PL_ROWS ] = { { nullptr } };
static Int				 s_plRowsShown												= 0;
static GameWindow *s_plRefusal													= nullptr;	///< over the first row, shown only while the table is withheld

// Camera (the five ported Extras sliders)
static GameWindow *s_slider[ DBG_NUM_SLIDERS ]			= { nullptr };
static GameWindow *s_sliderValue[ DBG_NUM_SLIDERS ]	= { nullptr };
static GameWindow *s_buttonCamDefaults	= nullptr;
static GameWindow *s_buttonCamApply			= nullptr;
static GameWindow *s_buttonUncapPitch		= nullptr;
static Bool				 s_uncapPitch					= FALSE;	///< the toggle as shown; written by Apply (View::setPrefCameraPitchUncapped)
static GameWindow *s_cameraNote				= nullptr;	///< the tab's foot line; says when a match caps the heights
static void refreshCameraNote( void );

// Config
static GameWindow *s_listResolved			= nullptr;
static GameWindow *s_listPrefs				= nullptr;
static GameWindow *s_entryPrefValue		= nullptr;
static GameWindow *s_buttonCfgRefresh	= nullptr;
static GameWindow *s_buttonPrefSet		= nullptr;
static GameWindow *s_buttonPrefSave		= nullptr;

// Saves
static GameWindow *s_listSaves				= nullptr;
static GameWindow *s_entrySaveDesc		= nullptr;
static GameWindow *s_buttonSaveRefresh	= nullptr;
static GameWindow *s_buttonSaveLoad			= nullptr;
static GameWindow *s_buttonSaveHere			= nullptr;

static OptionPreferences *s_pref = nullptr;
// What Camera Apply does with the pitch - see ExtrasMenu.cpp's PitchEdit.
enum PitchEdit { PITCH_UNTOUCHED, PITCH_MOVED, PITCH_DEFAULTS };
static PitchEdit s_pitchEdit = PITCH_UNTOUCHED;

/// Row index -> pref key. The listbox item data slot only holds a void*, and an AsciiString's
/// buffer is refcounted, so parking a raw str() pointer in there would dangle on the next refresh.
static std::vector<AsciiString> s_prefRowKeys;

static UnsignedInt s_lastRefreshMs	= 0;
static Bool				 s_configLoaded		= FALSE;		///< the resolved report is built lazily, it walks the archives

//-------------------------------------------------------------------------------------------------
// FORWARD DECLARATIONS ///////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuSystem( GameWindow *window, UnsignedInt msg,
																						 WindowMsgData mData1, WindowMsgData mData2 );
static WindowMsgHandledType DebugMenuInput( GameWindow *window, UnsignedInt msg,
																						WindowMsgData mData1, WindowMsgData mData2 );

//-------------------------------------------------------------------------------------------------
/** May we issue a SYNCHRONISED cheat from here?
	*
	* Cheats are messages in the 1000..1999 network band, sent with an execution frame and re-delivered on every peer.
	* This tests only where appending a message is illegal; deliberately NOT TheNetwork or getGameMode(). */
//-------------------------------------------------------------------------------------------------
static const WideChar *debugSyncedCheatsRefusal( void )
{
	if( TheGameLogic == nullptr || ThePlayerList == nullptr || TheMessageStream == nullptr )
		return L"no live match";

	// GameMessage's constructor dereferences ThePlayerList->getLocalPlayer() unconditionally
	// (MessageStream.cpp:54). A null there is a release-build crash, not an assert.
	if( ThePlayerList->getLocalPlayer() == nullptr )
		return L"no local player";

	if( TheGameLogic->isInGame() == FALSE )
		return L"no live match";
	if( TheGameLogic->isInShellGame() )
		return L"shell map, not a match";

	// A replay feeds TheCommandList straight from the recording; an injected order would be a
	// divergence from the file rather than a cheat.
	if( TheGameLogic->isInReplayGame() )
		return L"this is a replay";

	// Any peer's cheat orders replicate correctly, but with a second human in the match the simulation refuses
	// them (GameLogic::gxCheatsAllowed); this mirrors that so the buttons say so. A relay-proven admin is exempt.
	// Uses the NETWORK slot orders are signed with (Network.cpp gxSealAdminOrder): observers share one Player.
	const Int localSlot = ( TheNetwork != nullptr ) ? (Int)TheNetwork->getLocalPlayerID() : -1;
	const Bool adminSlot = localSlot >= 0 && localSlot < MAX_SLOTS && ( TheGameLogic->getGxAdminSlotMask() & ( (UnsignedInt64)1 << localSlot ) ) != 0;
	if( !GameLogic::gxCheatsAllowed() && !adminSlot )
		return L"more than one human in this match, and the server does not list you as an admin";
	// An admin slot signs its orders with this match's key (GameClient/GXAdminKey.h); without one, every peer would
	// refuse them, so say so instead of sending.
	if( !GameLogic::gxCheatsAllowed() && !gxAdminKeyState().matchOk )
		return L"you are an admin of this match, but this client holds no key for it";

	return nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Client-only debug toggles - the radar force-on and the read-only player panel.
	*
	* Nothing behind this gate touches logic state, so a network game is not a reason to refuse ON SYNC GROUNDS.
	* It is on FAIRNESS grounds: what these tools show is what the fog of war and
	* the build order hide, so anything that reads ANOTHER player's state also asks debugOthersRefusal() below. */
//-------------------------------------------------------------------------------------------------
static Bool debugClientToolsAllowed( void )
{
	return ( TheGameLogic != nullptr && TheGameLogic->isInGame() &&
					 TheGameLogic->isInShellGame() == FALSE &&
					 ThePlayerList != nullptr && ThePlayerList->getLocalPlayer() != nullptr );
}

//-------------------------------------------------------------------------------------------------
/** May this menu show OTHER players' state? Null when it may, else the reason.
	*
	* Asked by everything here that reads another player: the Players tab, Force radar, the Cheats and Spawn target lines'
	* cash / power / counts, the Stats tab's world counts and a Random enemy's real general in the target list. The rule
	* and its reasoning are GXDebugIntelGate.h: a replay, an observer seat, a defeated player, a game that is not a
	* network game, or a network game with no second human. A player still playing against another human is refused.
	* Read per call: a defeat flips it mid-match. */
//-------------------------------------------------------------------------------------------------
static const WideChar *debugOthersRefusal( void )
{
	const Bool playback = TheRecorder != nullptr && TheRecorder->isPlaybackMode();
	const Bool network = TheNetwork != nullptr || ( TheGameLogic != nullptr && TheGameLogic->isInMultiplayerGame() );
	const Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;
	if( GXDebugIntel::mayShowOthersFor( playback, network, GameLogic::gxCheatsAllowed(), local ) )
		return nullptr;
	return L"you are playing a network match against another human (it opens to a spectator, a defeated player or a replay)";
}

/// May this menu show p's own economy and counts? Your own always; anyone else's only where debugOthersRefusal() allows.
static Bool debugMayShowPlayer( const Player *p )
{
	if( p != nullptr && ThePlayerList != nullptr && p == ThePlayerList->getLocalPlayer() )
		return TRUE;
	return debugOthersRefusal() == nullptr;
}

//-------------------------------------------------------------------------------------------------
/** Echo one line of cheat feedback.
	*
	* Written to BOTH result labels, because the Cheats pane and the Spawn pane are separate hidden
	* containers and only one of them is on screen at a time. beginCheatOrder()'s refusals ("not in a
	* live match", "pick a target player first") come through here, so a Spawn button that declined
	* has to be able to say so on the pane the user is actually looking at. Writing the label of a
	* hidden pane is free - GadgetStaticTextSetText only re-lays out a DisplayString. */
//-------------------------------------------------------------------------------------------------
static void setCheatResult( const UnicodeString &text )
{
	if( s_cheatResult )
		GadgetStaticTextSetText( s_cheatResult, text );
	if( s_spawnResult )
		GadgetStaticTextSetText( s_spawnResult, text );
}

//-------------------------------------------------------------------------------------------------
// CAMERA TAB - the ported Extras sliders /////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void refreshUncapPitchLabel( void )
{
	if( s_buttonUncapPitch )
		GadgetButtonSetText( s_buttonUncapPitch, s_uncapPitch
			? UnicodeString( L"Uncap pitch: on" ) : UnicodeString( L"Uncap pitch: off" ) );
}

//-------------------------------------------------------------------------------------------------
/** The pitch row's range follows the Uncap pitch toggle. GSM_SET_MIN_MAX puts the thumb at the new minimum, so the
	* position is put back, pinned into the new range; returns TRUE when pinning moved it. */
static Bool setPitchSliderRange( Bool uncapped )
{
	GameWindow *slider = s_slider[2];
	if( slider == nullptr || TheWindowManager == nullptr )
		return FALSE;
	const Int lo = uncapped ? View::DebugCameraPitchLo : View::CameraPitchLo;
	const Int hi = uncapped ? View::DebugCameraPitchHi : View::CameraPitchHi;
	const Int was = GadgetSliderGetPosition( slider );
	TheWindowManager->winSendSystemMsg( slider, GSM_SET_MIN_MAX, lo, hi );
	const Int now = was < lo ? lo : ( was > hi ? hi : was );
	GadgetSliderSetPosition( slider, now );
	return now != was;
}

//-------------------------------------------------------------------------------------------------
static void refreshSliderValueLabel( Int i )
{
	// GadgetSliderGetPosition null-checks the SliderData but NOT the window (GadgetSlider.h), so the
	// window test has to happen out here.
	if( i < 0 || i >= DBG_NUM_SLIDERS || s_slider[i] == nullptr || s_sliderValue[i] == nullptr )
		return;

	UnicodeString txt;
	txt.format( L"%d", GadgetSliderGetPosition( s_slider[i] ) );
	GadgetStaticTextSetText( s_sliderValue[i], txt );
}

//-------------------------------------------------------------------------------------------------
static void loadSlidersFromPrefs( void )
{
	if( s_pref == nullptr )
		return;

	// Same conversions the Extras screen used: the two scale factors live as 0..2 reals in the pref
	// file and as integer percents on the slider.
	// Rounded, and draw distance through View's resolver - see the
	// matching note in ExtrasMenu.cpp. Truncating opened the shipped 1.05 as 104. Still read from
	// s_pref, so a Config-tab edit of the key reaches the slider and Camera Apply keeps it.
	const Int values[ DBG_NUM_SLIDERS ] =
	{
		(Int)View::getPrefMaxCameraHeight( *s_pref ),		// not the pref getter: it never allows the Debug range
		(Int)View::getPrefMinCameraHeight( *s_pref ),
		(Int)( View::getPrefCameraPitch( *s_pref ) + 0.5f ),		// see ExtrasMenuInit
		(Int)( s_pref->getScrollFactor() * 100.0f + 0.5f ),
		(Int)( View::getPrefTerrainDrawDistanceScale( *s_pref ) * 100.0f + 0.5f )
	};

	s_uncapPitch = View::getPrefCameraPitchUncapped( *s_pref );
	setPitchSliderRange( s_uncapPitch );
	refreshUncapPitchLabel();
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		if( s_slider[i] )
			GadgetSliderSetPosition( s_slider[i], values[i] );
		refreshSliderValueLabel( i );
	}
	// The pitch slider now shows the file, not a move the player made (a Config-tab edit lands here too).
	s_pitchEdit = PITCH_UNTOUCHED;
	refreshCameraNote();
}

//-------------------------------------------------------------------------------------------------
/** Apply the sliders to the running game and to the preference file.
	*
	* Writing TheWritableGlobalData is what makes the change visible now; writing the pref map is what
	* survives the session. Both halves are carried over from ExtrasMenu::saveExtras(). */
//-------------------------------------------------------------------------------------------------
static void applySlidersToPrefs( void )
{
	if( s_pref == nullptr || TheWritableGlobalData == nullptr )
		return;

	// Read before the loop below resets it: whether this Apply carries a pitch the player moved or reset.
	const Bool pitchEdited = s_pitchEdit != PITCH_UNTOUCHED;

	// Uncap pitch first: the pitch reader below (View::getPrefCameraPitch) and the whole-map max follow it.
	const Bool wasUncapped = View::isCameraPitchUncapped();
	View::setPrefCameraPitchUncapped( *s_pref, s_uncapPitch );
	View::setCameraPitchUncapped( s_uncapPitch );

	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		if( s_slider[i] == nullptr )
			continue;

		const Int val = GadgetSliderGetPosition( s_slider[i] );
		if( val <= 0 )
			continue;

		AsciiString prefString;

		switch( i )
		{
			case 0:
				TheWritableGlobalData->m_maxCameraHeight = (Real)val;
				prefString.format( "%d", val );
				break;
			case 1:
				TheWritableGlobalData->m_minCameraHeight = (Real)val;
				prefString.format( "%d", val );
				break;
			case 2:
				// Through View's one writer, and only for a slider the
				// player moved or reset - it may remove the key, so it bypasses the shared assignment below.
				// See saveExtras in ExtrasMenu.cpp.
				if( s_pitchEdit == PITCH_DEFAULTS )
					View::clearPrefCameraPitch( *s_pref );
				else if( s_pitchEdit == PITCH_MOVED )
					View::setPrefCameraPitch( *s_pref, val );
				s_pitchEdit = PITCH_UNTOUCHED;
				continue;
			case 3:
				// ScrollFactor is stored as an integer percent; OptionPreferences::getScrollFactor
				// divides by 100 on the way back out.
				TheWritableGlobalData->m_keyboardScrollFactor = val / 100.0f;
				prefString.format( "%d", val );
				break;
			case 4:
				// Write the draw-distance scale as the real it is.
				// View::getPrefTerrainDrawDistanceScale() atof's the string and takes only 1.00-2.00, so the
				// raw slider percent ("105") would be ignored on reopen and the slider would not keep its place.
				TheWritableGlobalData->m_terrainDrawDistanceScale = val / 100.0f;
				prefString.format( "%.2f", val / 100.0f );
				break;
			default:
				continue;
		}

		(*s_pref)[ s_sliderSpecs[i].prefKey ] = prefString;
	}

	// Mark the pair when it leaves the launcher's range, so startup keeps it
	// instead of clamping it back to 1000 / 50 (View.cpp, getPrefMaxCameraHeight).
	if( s_slider[0] != nullptr && s_slider[1] != nullptr )
		View::setPrefDebugCameraHeights( *s_pref, TheWritableGlobalData->m_minCameraHeight, TheWritableGlobalData->m_maxCameraHeight );

	// Push the camera settings into the live view.
	// The view does not read TheGlobalData per frame. View::View snapshots the height limits
	// (View.cpp:99-100) and the default pitch (View.cpp:104) at construction, and W3DView only
	// recomputes them inside setCameraHeightAboveGroundLimitsToDefault/setDefaultPitch. Writing
	// the global alone left these three sliders with no visible effect until the next GameLogic
	// reset re-created the view. OptionsMenu.cpp:891 re-applies the limits the same way after a
	// resolution change.
	if( TheTacticalView != nullptr )
	{
		TheTacticalView->setCameraHeightAboveGroundLimitsToDefault();
		TheTacticalView->setDefaultPitch( DEG_TO_RADF( View::getPrefCameraPitch( *s_pref ) ) );

		// Keep the camera height, pinned into the new limits, so it moves now. setZoom( getZoom() ) is not a
		// round trip (lifts the camera ~44% per Apply); setHeightAboveGround is the wheel's own clamp.
		// Then setZoom( height / max above ground ) re-bases the zoom on the new limits.
		// Both stay out of the shell's scripted camera. A changed pitch goes live the same way.
		if( TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() )
		{
			TheTacticalView->setHeightAboveGround( TheTacticalView->getHeightAboveGround() );
			const Real maxAbove = TheTacticalView->getZoomReferenceHeight();	// setZoom's own scale
			if( maxAbove > 0.0f )
				TheTacticalView->setZoom( TheTacticalView->getHeightAboveGround() / maxAbove );
			if( pitchEdited )
				TheTacticalView->setPitch( TheTacticalView->getDefaultPitch() );
			// Uncap pitch turned off: a tilt past the player's range made while it was on comes back inside it now.
			if( wasUncapped && !s_uncapPitch )
			{
				Real pitchLo, pitchHi;
				View::getPlayerPitchRange( &pitchLo, &pitchHi );
				const Real pitchNow = TheTacticalView->getPitch();
				if( pitchNow < pitchLo || pitchNow > pitchHi )
					TheTacticalView->setPitch( pitchNow < pitchLo ? pitchLo : pitchHi );
			}
		}
	}

	s_pref->write();
	refreshCameraNote();
}

//-------------------------------------------------------------------------------------------------
static void setSliderDefaults( void )
{
	s_uncapPitch = FALSE;
	setPitchSliderRange( FALSE );
	refreshUncapPitchLabel();
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		// Pitch shows the data default, rounded, and Camera Apply removes
		// the choice; the table's 37 was stored as a choice below the default.
		const Int def = ( i == 2 ) ? (Int)( TheGlobalData->m_cameraPitch + 0.5f ) : s_sliderSpecs[i].defVal;
		if( s_slider[i] )
			GadgetSliderSetPosition( s_slider[i], def );
		refreshSliderValueLabel( i );
	}
	s_pitchEdit = PITCH_DEFAULTS;
}

//-------------------------------------------------------------------------------------------------
// CONFIG TAB /////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

struct DebugProbeDir
{
	const char *directory;
	const char *mask;
};

// ArchiveFileSystem keeps m_archiveFileMap protected and exposes no way to enumerate the archives it
// loaded, so the loaded set is recovered by asking which archive actually provides a file. These
// four directories are the ones that matter when a screen or a rule misbehaves, and they are the
// only ones small enough to walk interactively: the override test below stats the local disk once
// per file, so a probe over Art\W3D (10k+ entries) would stall the frame for the better part of a
// second. They cover INI*.big, Window*.big, Maps*.big, the text archives, and every Patch*.big that
// shadows one of them.
static const DebugProbeDir s_probeDirs[] =
{
	{ "Data\\INI\\",	"*.ini" },
	{ "Window\\",			"*.wnd" },
	{ "Maps\\",				"*.map" },
	{ "Data\\",				"*.csf" }
};
enum { DBG_NUM_PROBE_DIRS = sizeof( s_probeDirs ) / sizeof( s_probeDirs[0] ) };

//-------------------------------------------------------------------------------------------------
static void addReportLine( Int &rows, const UnicodeString &text, Color color )
{
	if( s_listResolved == nullptr || rows >= DBG_MAX_REPORT_ROWS )
		return;

	GadgetListBoxAddEntryText( s_listResolved, text, color, -1 );
	++rows;
}

//-------------------------------------------------------------------------------------------------
/** Build the read-only "what actually resolved" report.
	*
	* Two questions, one walk. Which archives are live (an archive is live if it wins at least one
	* probed file), and what shadows what: FileSystem::doesFileExist consults TheLocalFileSystem
	* first, so a loose file on disk takes instance 0 and pushes every archive copy down one; below
	* that, instance N of getArchiveFile() is the Nth archive providing the same path, in the order
	* the multimap holds them. Anything with more than one provider is an override. */
//-------------------------------------------------------------------------------------------------
static void refreshResolvedConfig( void )
{
	if( s_listResolved == nullptr )
		return;

	GadgetListBoxReset( s_listResolved );

	const CodeGuiTheme &theme = CodeGuiGetTheme();
	Int rows = 0;

	if( TheArchiveFileSystem == nullptr )
	{
		addReportLine( rows, UnicodeString( L"TheArchiveFileSystem is not up yet." ), theme.textNormal );
		s_configLoaded = TRUE;
		return;
	}

	typedef std::map<AsciiString, Int> ArchiveHitMap;
	ArchiveHitMap winners;						// archive name -> number of probed files it wins
	std::vector<UnicodeString> overrides;
	Int probedFiles = 0;

	for( Int d = 0; d < DBG_NUM_PROBE_DIRS; ++d )
	{
		FilenameList files;
		TheArchiveFileSystem->getFileListInDirectory( AsciiString::TheEmptyString,
																									AsciiString( s_probeDirs[d].directory ),
																									AsciiString( s_probeDirs[d].mask ),
																									files, TRUE );

		for( FilenameListIter it = files.begin(); it != files.end(); ++it )
		{
			const AsciiString &path = *it;
			++probedFiles;

			// Instance 0 is the winner as far as the archives are concerned; a loose file on disk beats
			// all of them and is reported separately because it is the single most confusing override.
			ArchiveFile *winner = TheArchiveFileSystem->getArchiveFile( path, 0 );
			if( winner == nullptr )
				continue;

			winners[ winner->getName() ]++;

			const Bool loose = ( TheLocalFileSystem != nullptr &&
													 TheLocalFileSystem->doesFileExist( path.str() ) );
			ArchiveFile *shadowed = TheArchiveFileSystem->getArchiveFile( path, 1 );

			if( loose == FALSE && shadowed == nullptr )
				continue;
			if( (Int)overrides.size() >= DBG_MAX_REPORT_ROWS )
				continue;

			UnicodeString line;
			if( loose )
				line.format( L"%S  <-  loose file on disk  (over %S)", path.str(), winner->getName().str() );
			else
				line.format( L"%S  <-  %S  (over %S)", path.str(), winner->getName().str(),
										 shadowed->getName().str() );
			overrides.push_back( line );
		}
	}

	UnicodeString line;

	line.format( L"ARCHIVES PROVIDING CONTENT  (%d probed files)", probedFiles );
	addReportLine( rows, line, theme.accent );

	for( ArchiveHitMap::const_iterator a = winners.begin(); a != winners.end(); ++a )
	{
		line.format( L"  %S   %d file(s) win", a->first.str(), a->second );
		addReportLine( rows, line, theme.textNormal );
	}

	line.format( L"OVERRIDES  (%d)", (Int)overrides.size() );
	addReportLine( rows, line, theme.accent );

	if( overrides.empty() )
		addReportLine( rows, UnicodeString( L"  nothing is being shadowed" ), theme.textNormal );

	for( size_t o = 0; o < overrides.size(); ++o )
		addReportLine( rows, overrides[o], theme.textNormal );

	// The handful of resolved values this screen can actually change, so the read-only pane and the
	// editable pane can be compared side by side.
	if( TheGlobalData )
	{
		addReportLine( rows, UnicodeString( L"RESOLVED CAMERA / TERRAIN VALUES" ), theme.accent );

		line.format( L"  MaxCameraHeight            %.1f", TheGlobalData->m_maxCameraHeight );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  MinCameraHeight            %.1f", TheGlobalData->m_minCameraHeight );
		addReportLine( rows, line, theme.textNormal );
		// The view's, not TheGlobalData's: a chosen pitch lives in the view (see View::init).
		line.format( L"  CameraPitch                %.1f", TheTacticalView != nullptr
			? RAD_TO_DEGF( TheTacticalView->getDefaultPitch() ) : TheGlobalData->m_cameraPitch );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  KeyboardScrollFactor       %.2f", TheGlobalData->m_keyboardScrollFactor );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  TerrainDrawDistanceScale   %.2f", TheGlobalData->m_terrainDrawDistanceScale );
		addReportLine( rows, line, theme.textNormal );
	}

	s_configLoaded = TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Fill the editable pane from the live preference map.
	*
	* OptionPreferences IS a std::map<AsciiString,AsciiString> (UserPreferences derives from it), so
	* this is the real resolved user config, not a curated copy of it. */
//-------------------------------------------------------------------------------------------------
static void refreshPrefList( void )
{
	if( s_listPrefs == nullptr )
		return;

	GadgetListBoxReset( s_listPrefs );
	s_prefRowKeys.clear();

	if( s_pref == nullptr )
		return;

	const CodeGuiTheme &theme = CodeGuiGetTheme();

	for( OptionPreferences::const_iterator it = s_pref->begin(); it != s_pref->end(); ++it )
	{
		UnicodeString line;
		line.format( L"%S = %S", it->first.str(), it->second.str() );

		const Int row = GadgetListBoxAddEntryText( s_listPrefs, line, theme.textNormal, -1 );
		if( row < 0 )
			break;		// listbox is full; listLength is a hard cap

		s_prefRowKeys.push_back( it->first );
	}
}

//-------------------------------------------------------------------------------------------------
static void onPrefRowSelected( void )
{
	if( s_listPrefs == nullptr || s_entryPrefValue == nullptr || s_pref == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listPrefs, &sel );
	if( sel < 0 || sel >= (Int)s_prefRowKeys.size() )
		return;

	UnicodeString value;
	value.translate( (*s_pref)[ s_prefRowKeys[sel] ] );
	GadgetTextEntrySetText( s_entryPrefValue, value );
}

//-------------------------------------------------------------------------------------------------
static void applyPrefEdit( void )
{
	if( s_listPrefs == nullptr || s_entryPrefValue == nullptr || s_pref == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listPrefs, &sel );
	if( sel < 0 || sel >= (Int)s_prefRowKeys.size() )
		return;

	const AsciiString key = s_prefRowKeys[sel];

	AsciiString value;
	value.translate( GadgetTextEntryGetText( s_entryPrefValue ) );
	(*s_pref)[ key ] = value;

	// Keep the two panes honest with each other: the row text is stale the instant the map changes,
	// and the sliders share four of these keys.
	refreshPrefList();
	if( sel < (Int)s_prefRowKeys.size() )
		GadgetListBoxSetSelected( s_listPrefs, sel );
	loadSlidersFromPrefs();
}

//-------------------------------------------------------------------------------------------------
// SAVES TAB //////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void refreshSaveList( void )
{
	if( s_listSaves == nullptr || TheGameState == nullptr )
		return;

	// SLLT_LOAD_ONLY so the list is purely the files on disk; the "new save game" pseudo-row the
	// save layouts prepend carries null item data and would just be a dead row here.
	TheGameState->populateSaveGameListbox( s_listSaves, SLLT_LOAD_ONLY );
}

//-------------------------------------------------------------------------------------------------
/** Load the selected save.
	*
	* Mirrors PopupSaveLoad::doLoadGame(), including the two different preambles: from the shell the
	* logic has to be told a single-player game is starting, from inside a game the quit menu has to
	* go away first. The overlay is torn down before the load because loadGame resets the engine. */
//-------------------------------------------------------------------------------------------------
static void doLoadSelectedSave( void )
{
	if( s_listSaves == nullptr || TheGameState == nullptr || TheGameLogic == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listSaves, &sel );
	if( sel < 0 )
		return;

	AvailableGameInfo *info = (AvailableGameInfo *)GadgetListBoxGetItemData( s_listSaves, sel );
	if( info == nullptr )
		return;

	// Copy before the teardown: the item data points into GameState's available-games list, which
	// clearAvailableGames() frees, and loadGame takes it by value for exactly that reason.
	AvailableGameInfo gameInfo = *info;

	// before anything is torn down; see SaveLoadFeedback.cpp
	if( gateSaveLoad( gameInfo.filename, doLoadSelectedSave ) )
		return;

	DebugMenuClose();

	if( TheShell != nullptr && TheShell->isShellActive() == FALSE )
	{
		destroyQuitMenu();
	}
	else
	{
		if( TheTransitionHandler )
		{
			TheTransitionHandler->remove( "MainMenuLoadReplayMenu" );
			TheTransitionHandler->remove( "MainMenuLoadReplayMenuBack" );
		}
		TheGameLogic->prepareNewGame( GAME_SINGLE_PLAYER, DIFFICULTY_NORMAL, 0 );
	}

	const SaveCode loadResult = TheGameState->loadGame( gameInfo );
	presentLoadResult( loadResult, gameInfo.filename );
	if( loadResult != SC_OK )
	{
		if( TheGameLogic->isInGame() )
			TheGameLogic->clearGameData( FALSE );
		TheGameEngine->reset();
		TheShell->showShell( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
/** Save-anywhere.
	*
	* GameState::saveGame() itself has no notion of "you may only save between missions" - that rule
	* lives entirely in the PopupSaveLoad UI, which only ever offers SLLT_SAVE_ONLY at a mission
	* boundary. Passing an empty filename makes it mint the next Save000N.sav itself. */
//-------------------------------------------------------------------------------------------------
static void doSaveHere( void )
{
	if( TheGameState == nullptr || TheGameLogic == nullptr )
		return;

	if( TheGameLogic->isInGame() == FALSE || TheGameLogic->isInShellGame() ||
			TheGameLogic->isInReplayGame() )
		return;

	// see SaveLoadFeedback.cpp
	if( refuseNetworkMatchSave() )
		return;

	UnicodeString desc;
	if( s_entrySaveDesc )
		desc = GadgetTextEntryGetText( s_entrySaveDesc );
	if( desc.isEmpty() )
		desc = UnicodeString( L"Debug save" );

	presentSaveResult( TheGameState->saveGame( AsciiString::TheEmptyString, desc, SAVE_FILE_TYPE_NORMAL ) );
	refreshSaveList();
}

// CHEATS: every cheat below is a network order aimed at a CHOSEN player.
// The target travels as a PLAYER INDEX, the one identifier lockstep-identical on every peer; the net slot is
// overwritten by the sender's. Index 0 is the neutral player and handlers refuse it, so targets start at 1.

/// Does this viewer see the REAL colours (a replay, an observer seat, a defeated player - the HUD's own gxViewerMaySeeFeed test)
/// rather than the apparent ones a player still playing gets? Read per call: defeat flips it mid-match.
static Bool debugViewerSeesReal( void )
{
	Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;
	return GXSpectatorFeed::viewerMaySeeFeedFor( TheRecorder != nullptr && TheRecorder->isPlaybackMode(), local );
}
/// What debugViewerSeesReal() said when the target list was last built; the list is rebuilt when it changes (a defeat).
static Int s_cheatViewerSeesReal = -1;
/// Likewise debugOthersRefusal() == nullptr, which decides the rows' general:
/// a list built where it was allowed must not survive into a match where it is not.
static Int s_cheatOthersShown = -1;

//-------------------------------------------------------------------------------------------------
/** Would the target listbox come out different from what is in it right now?
	*
	* The roster only changes at newGame, but this screen survives a match transition, so the list has
	* to notice. Comparing NameKeyTypes rather than Player pointers is deliberate: a cached Player* is
	* dangling the moment PlayerList is rebuilt, and comparing a dangling pointer is exactly the class
	* of bug this whole screen keeps tripping over. NameKeyType is a 32-bit int. */
//-------------------------------------------------------------------------------------------------
static Bool cheatRosterChanged( void )
{
	if( ThePlayerList == nullptr )
		return FALSE;								// nothing to build from; refreshCheatTargets would bail anyway

	const Int count = ThePlayerList->getPlayerCount();
	if( count != s_cheatRosterCount )
		return TRUE;
	if( (Int)debugViewerSeesReal() != s_cheatViewerSeesReal )
		return TRUE;								// the row colours depend on it: a defeated player now sees the real ones
	if( (Int)( debugOthersRefusal() == nullptr ) != s_cheatOthersShown )
		return TRUE;								// the rows' general depends on it (debugFactionName)

	for( Int i = 0; i < count && i < MAX_PLAYER_COUNT; ++i )
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		const NameKeyType key = ( p != nullptr ) ? p->getPlayerNameKey() : NAMEKEY_INVALID;
		if( key != s_cheatRosterKey[i] )
			return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** The colour a player row is drawn in, as the GX spectator rows draw it (GXSpectatorHud.inl applyApparent):
	* a PLAYER still playing sees every other player's colour the way retail's Diplomacy screen shows it
	* (GameSlot::getApparentColor - an enemy who picked Random keeps the Random swatch); a replay viewer, observer,
	* defeated player or the row's own owner sees the real one. Lifted 35% toward white (InGameUI brightenForText) so the
	* dark palette entries stay readable on the debug panel's dark ground. */
//-------------------------------------------------------------------------------------------------
static Color debugRowColor( Player *p )
{
	Color c = p->getPlayerColor();
	Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;
	// The HUD's own viewer test (InGameUI gxViewerMaySeeFeed): a replay, an observer seat or a DEFEATED player sees the real
	// colours - isPlayerObserver() alone misses the defeated one.
	const Bool spectator = debugViewerSeesReal();
	const Bool apparent = local != nullptr && !spectator && p != local;
	if( apparent && TheGameInfo != nullptr && TheMultiplayerSettings != nullptr )
	{
		Int slot = -1;
		for( Int s = 0; s < MAX_SLOTS; ++s )
			if( ThePlayerList->getPlayerFromSlotIndex( s ) == p ) { slot = s; break; }
		const GameSlot *gs = ( slot >= 0 ) ? TheGameInfo->getConstSlot( slot ) : nullptr;
		// No lobby slot (Generals Challenge registers only the local one): keep the player's own colour - getColor( -1 )
		// would answer the Random swatch for every map-defined opponent.
		const MultiplayerColorDefinition *cd = ( gs != nullptr ) ? TheMultiplayerSettings->getColor( gs->getApparentColor() ) : nullptr;
		if( cd != nullptr )
			c = cd->getColor();
	}
	UnsignedByte r = 0, g = 0, b = 0, a = 255;
	GameGetColorComponents( c, &r, &g, &b, &a );
	const Real k = 0.35f;
	return GameMakeColor( (Int)( r + ( 255 - r ) * k ), (Int)( g + ( 255 - g ) * k ), (Int)( b + ( 255 - b ) * k ), a );
}

//-------------------------------------------------------------------------------------------------
/** The general a target-list row names. The real one where debugOthersRefusal()
	* allows it; otherwise, for anyone but you, the lobby slot's apparent one (GameSlot::getApparentPlayerTemplateDisplayName:
	* an enemy who picked Random stays "Random", as on retail's Diplomacy screen). A player with no lobby slot keeps the
	* real one, as debugRowColor keeps its colour. */
//-------------------------------------------------------------------------------------------------
static UnicodeString debugFactionName( const Player *p, const PlayerTemplate *pt )
{
	const Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;
	if( p != local && TheGameInfo != nullptr && debugOthersRefusal() != nullptr )
	{
		for( Int s = 0; s < MAX_SLOTS; ++s )
		{
			if( ThePlayerList->getPlayerFromSlotIndex( s ) != p )
				continue;
			const GameSlot *gs = TheGameInfo->getConstSlot( s );
			if( gs != nullptr )
				return gs->getApparentPlayerTemplateDisplayName();
			break;
		}
	}
	return pt->getDisplayName();
}

//-------------------------------------------------------------------------------------------------
/** Rebuild the target list, preserving the selection by player index.
	*
	* Mirrors refreshPrefList(): GadgetListBoxReset, then GadgetListBoxAddEntryText per row with a
	* parallel index vector. Only called when cheatRosterChanged() says so - rebuilding at 4 Hz would
	* clear the selection out from under the user between aiming and pressing. */
//-------------------------------------------------------------------------------------------------
static void refreshCheatTargets( void )
{
	if( s_listCheatTarget == nullptr || ThePlayerList == nullptr )
		return;

	Int keep = -1;
	GadgetListBoxGetSelected( s_listCheatTarget, &keep );
	const Int keptIndex = ( keep >= 0 && keep < (Int)s_cheatTargetIndex.size() )
												? s_cheatTargetIndex[keep] : -1;

	GadgetListBoxReset( s_listCheatTarget );
	s_cheatTargetIndex.clear();

	Player *local = ThePlayerList->getLocalPlayer();
	const Int count = ThePlayerList->getPlayerCount();

	s_cheatRosterCount = count;
	s_cheatViewerSeesReal = (Int)debugViewerSeesReal();
	s_cheatOthersShown = (Int)( debugOthersRefusal() == nullptr );
	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
	{
		Player *p = ( i < count ) ? ThePlayerList->getNthPlayer( i ) : nullptr;
		s_cheatRosterKey[i] = ( p != nullptr ) ? p->getPlayerNameKey() : NAMEKEY_INVALID;
	}

	for( Int i = 1; i < count; ++i )				// 0 is neutral, the handlers refuse it
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		if( p == nullptr )
			continue;

		// An unused slot is a live Player with a null template. Offering it as a target would just be
		// a button that silently does nothing.
		const PlayerTemplate *pt = p->getPlayerTemplate();
		if( pt == nullptr )
			continue;

		UnicodeString name = p->getPlayerDisplayName();
		if( name.isEmpty() )
			name.translate( p->getPlayerName() );

		UnicodeString line;
		line.format( L"%d   %s   -   %s   %s%s", i, name.str(), debugFactionName( p, pt ).str(),
								 ( p->getPlayerType() == PLAYER_HUMAN ) ? L"human" : L"AI",
								 ( p == local ) ? L"   (you)" : L"" );

		const Int row = GadgetListBoxAddEntryText( s_listCheatTarget, line, debugRowColor( p ), -1 );
		if( row < 0 )
			break;										// listbox is full; listLength is a hard cap

		s_cheatTargetIndex.push_back( i );
	}

	// Restore the previous target, or pre-select the local player on a fresh list. selectedCheatTarget()
	// would fall back to the local player anyway, but an IMPLICIT target on a screen with a "Defeat
	// player" button is a bad idea: this way the highlighted row always says who is about to be hit.
	const Int want = ( keptIndex >= 0 ) ? keptIndex
										 : ( ( local != nullptr ) ? (Int)local->getPlayerIndex() : -1 );

	for( size_t r = 0; r < s_cheatTargetIndex.size(); ++r )
	{
		if( s_cheatTargetIndex[r] == want )
		{
			GadgetListBoxSetSelected( s_listCheatTarget, (Int)r );
			break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** The player every cheat below aims at, read at press time rather than tracked through GLM_ events.
	*
	* Falls back to the local player so the screen is still useful before anything is picked - which is
	* also what makes the three converted buttons behave like they used to out of the box. */
//-------------------------------------------------------------------------------------------------
static Int selectedCheatTarget( void )
{
	Int sel = -1;
	if( s_listCheatTarget )
		GadgetListBoxGetSelected( s_listCheatTarget, &sel );

	if( sel >= 0 && sel < (Int)s_cheatTargetIndex.size() )
		return s_cheatTargetIndex[sel];

	if( ThePlayerList == nullptr )
		return -1;

	Player *local = ThePlayerList->getLocalPlayer();
	return ( local != nullptr ) ? (Int)local->getPlayerIndex() : -1;
}

//-------------------------------------------------------------------------------------------------
static UnicodeString cheatTargetName( Int index )
{
	UnicodeString name;

	if( ThePlayerList == nullptr || index <= 0 || index >= ThePlayerList->getPlayerCount() )
		return name;

	Player *p = ThePlayerList->getNthPlayer( index );
	if( p == nullptr )
		return name;

	name = p->getPlayerDisplayName();
	if( name.isEmpty() )
		name.translate( p->getPlayerName() );

	return name;
}

//-------------------------------------------------------------------------------------------------
/** Why a target takes no order but reveal, or null when it does.
	* Observer FIRST: Player::initFromTemplate sets m_isPlayerDead = m_observer, so an observer also
	* reads as dead and would otherwise be called "defeated". */
//-------------------------------------------------------------------------------------------------
static const WideChar *cheatTargetInactiveWord( const Player *p )
{
	if( p == nullptr || p->isPlayerActive() )
		return nullptr;
	return p->isPlayerObserver() ? L"observing" : L"defeated";
}

//-------------------------------------------------------------------------------------------------
/** Open a cheat order and append argument 0, the target player index.
	*
	* One place owns the argument-0 contract so a new cheat cannot get it wrong. Returns null and
	* explains itself in the result line if the order may not be sent; the caller then appends the
	* rest of ITS arguments, in the exact order and count the handler checks with gxCheatArgsOk(). */
//-------------------------------------------------------------------------------------------------
static GameMessage *beginCheatOrder( GameMessage::Type type, Int &targetOut )
{
	targetOut = -1;

	const WideChar *refusal = debugSyncedCheatsRefusal();
	if( refusal != nullptr )
	{
		UnicodeString msg;
		msg.format( L"Refused: %s.", refusal );
		setCheatResult( msg );
		return nullptr;
	}

	const Int target = selectedCheatTarget();
	if( target <= 0 )
	{
		setCheatResult( L"Pick a target player first." );
		return nullptr;
	}

	// A DEFEATED OR OBSERVING TARGET TAKES NO ORDER BUT REVEAL. A status line reading "0 units, 0 buildings  DEAD" must not still accept "Give cash" (money into a player who can never spend it). isPlayerActive() is the engine's own "alive and not observing" (Player.h), the same test the status line prints DEAD from.
	// Reveal / Re-shroud stay allowed: showing a defeated player the map is how they watch the rest. So does Spawn: the simulation makes its units civilians (the Neutral player's, vehicles unmanned) instead of refusing it (GameLogic::gxSpawnGoesNeutral). Delete and the rest stay refused.
	if( type != GameMessage::MSG_GX_CHEAT_REVEAL_MAP && type != GameMessage::MSG_GX_CHEAT_SPAWN_UNIT )
	{
		const Player *p = ( ThePlayerList != nullptr ) ? ThePlayerList->getNthPlayer( target ) : nullptr;
		if( const WideChar *inactive = cheatTargetInactiveWord( p ) )
		{
			UnicodeString refused;
			refused.format( L"Refused: %s is %s - only Reveal / Re-shroud and Spawn (as civilians) still apply.",
											cheatTargetName( target ).str(), inactive );
			setCheatResult( refused );
			return nullptr;
		}
	}

	GameMessage *msg = TheMessageStream->appendMessage( type );
	if( msg == nullptr )
		return nullptr;

	msg->appendIntegerArgument( target );
	targetOut = target;
	return msg;
}

//-------------------------------------------------------------------------------------------------
/** Say "sent", never "done".
	*
	* In a LAN game the effect lands m_runAhead frames after the tap, on the sender's own screen too -
	* that lateness IS the proof the order went round-trip instead of being applied locally. Claiming
	* success here would make the normal case read as a bug. */
//-------------------------------------------------------------------------------------------------
static void setCheatOrderSent( const WideChar *what, Int target )
{
	UnicodeString msg;
	msg.format( L"Sent: %s -> %s (player %d). Lands in a few frames.",
							what, cheatTargetName( target ).str(), target );
	setCheatResult( msg );
}

//-------------------------------------------------------------------------------------------------
/** How much cash the two money buttons move. Negative is legal and means "take it away". */
//-------------------------------------------------------------------------------------------------
static Int cheatCashAmount( void )
{
	if( s_entryCashAmount == nullptr )
		return 10000;

	AsciiString text;
	text.translate( GadgetTextEntryGetText( s_entryCashAmount ) );
	text.trim();
	if( text.isEmpty() )
		return 10000;

	return atoi( text.str() );
}

//-------------------------------------------------------------------------------------------------
static void gxGiveMoney( void )
{
	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_GIVE_MONEY, target );
	if( msg == nullptr )
		return;

	const Int amount = cheatCashAmount();
	msg->appendIntegerArgument( amount );

	DEBUG_LOG(( "DebugMenu: sent GIVE_MONEY %d to player %d", amount, target ));

	UnicodeString what;
	what.format( L"%s $%d", ( amount < 0 ) ? L"take" : L"give", ( amount < 0 ) ? -amount : amount );
	setCheatOrderSent( what.str(), target );
}

//-------------------------------------------------------------------------------------------------
static void gxSetMoney( void )
{
	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_SET_MONEY, target );
	if( msg == nullptr )
		return;

	const Int amount = cheatCashAmount();
	msg->appendIntegerArgument( amount );

	DEBUG_LOG(( "DebugMenu: sent SET_MONEY %d for player %d", amount, target ));

	UnicodeString what;
	what.format( L"set cash to $%d", ( amount < 0 ) ? 0 : amount );
	setCheatOrderSent( what.str(), target );
}

//-------------------------------------------------------------------------------------------------
/** Reveal or re-shroud the TARGET player's map.
	*
	* Two explicit buttons rather than one toggle, on purpose: the "is it revealed" flag lives in the
	* logic handler (it has to - PartitionCell::addLooker is a counter, not a flag) and the client has
	* no way to read it back, so a client-side toggle would drift out of step with the truth the first
	* time two peers pressed it. Two idempotent orders cannot drift.
	*
	* Revealing SOMEONE ELSE'S map is real and is in the CRC, but it repaints nothing on your machine -
	* PartitionCell::addLooker only pushes to TheDisplay/TheRadar for the observed-or-local index. To
	* see through the fog yourself, target yourself. */
//-------------------------------------------------------------------------------------------------
static void gxRevealMap( Bool reveal )
{
	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_REVEAL_MAP, target );
	if( msg == nullptr )
		return;

	msg->appendBooleanArgument( reveal );

	DEBUG_LOG(( "DebugMenu: sent REVEAL_MAP %d for player %d", (Int)reveal, target ));
	setCheatOrderSent( reveal ? L"reveal map" : L"re-shroud map", target );
}

//-------------------------------------------------------------------------------------------------
/** Defeat the target player: hand the assets to a living mutual ally, then killPlayer().
	*
	* There is no Player::setDefeated - defeat is DERIVED. VictoryConditions::update polls
	* hasSinglePlayerBeenDefeated() on every peer every frame and does the rest itself: the banner, the
	* permanent reveal for the dead player, the AI slot's last frame, the score screen, observer mode.
	* All we owe it is a synchronised kill. */
//-------------------------------------------------------------------------------------------------
static void gxKillPlayer( Bool transferToAlly )
{
	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_KILL_PLAYER, target );
	if( msg == nullptr )
		return;

	msg->appendBooleanArgument( transferToAlly );

	DEBUG_LOG(( "DebugMenu: sent KILL_PLAYER (ally transfer %d) for player %d",
		(Int)transferToAlly, target ));
	setCheatOrderSent( L"defeat", target );
}

//-------------------------------------------------------------------------------------------------
static void gxKillObjects( Bool includeStructures )
{
	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_KILL_OBJECTS, target );
	if( msg == nullptr )
		return;

	msg->appendBooleanArgument( includeStructures );

	DEBUG_LOG(( "DebugMenu: sent KILL_OBJECTS (structures %d) for player %d",
		(Int)includeStructures, target ));
	setCheatOrderSent( includeStructures ? L"kill units and buildings" : L"kill units", target );
}

//-------------------------------------------------------------------------------------------------
// SPAWN TAB - the template picker /////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** Would spawning this template produce something a human would recognise as a unit?
	* Drop templates with no draw module, KINDOF_DRAWABLE_ONLY, in-flight-only objects, and editor buckets
	* ES_SYSTEM, ES_AUDIO, ES_TEST, ES_FOR_REVIEW, ES_ROAD, ES_WAYPOINT. ES_NONE is kept; m_editorSorting is
	* range-checked first because the constructor never initialises it. */
//-------------------------------------------------------------------------------------------------
static Bool debugTemplateLooksSpawnable( const ThingTemplate *tmpl )
{
	if( tmpl == nullptr )
		return FALSE;

	if( tmpl->getDrawModuleInfo().getCount() == 0 )
		return FALSE;

	if( tmpl->isKindOf( KINDOF_DRAWABLE_ONLY ) )
		return FALSE;

	if( tmpl->isKindOf( KINDOF_PROJECTILE ) ||
			tmpl->isKindOf( KINDOF_SMALL_MISSILE ) ||
			tmpl->isKindOf( KINDOF_BALLISTIC_MISSILE ) ||
			tmpl->isKindOf( KINDOF_PARACHUTE ) )
		return FALSE;

	const Int sort = (Int)tmpl->getEditorSorting();
	if( sort > (Int)ES_NONE && sort < (Int)ES_NUM_SORTING_TYPES )
	{
		if( sort == (Int)ES_SYSTEM || sort == (Int)ES_AUDIO || sort == (Int)ES_TEST ||
				sort == (Int)ES_FOR_REVIEW || sort == (Int)ES_ROAD || sort == (Int)ES_WAYPOINT )
			return FALSE;
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/// Alphabetical by the searchable key, which starts with the display label - so the list reads in
/// the order of the text the user is actually looking at.
static bool debugSpawnEntryLess( const DebugSpawnEntry &a, const DebugSpawnEntry &b )
{
	return strcmp( a.search.str(), b.search.str() ) < 0;
}

//-------------------------------------------------------------------------------------------------
/** Walk the ThingFactory once and cache every template.
	*
	* The factory's list is a singly linked chain built by PREPENDING (ThingFactory::addTemplate), so
	* raw iteration order is reverse INI parse order - unusable as a browse order. Hence the sort.
	*
	* Built lazily on the first visit to the pane and thrown away by nullifyControls(), i.e. once per
	* screen lifetime. That is also the correctness argument for not caching template POINTERS or IDS
	* here: ThingFactory::reset() deletes override templates and renumbers, and a match transition -
	* which is exactly what tears this screen down - is when that happens. */
//-------------------------------------------------------------------------------------------------
static void buildSpawnCatalog( void )
{
	s_spawnCatalog.clear();
	s_spawnCatalogBuilt = TRUE;			// set even on the early-out, so we do not re-walk every tick

	if( TheThingFactory == nullptr )
		return;

	Int spawnableCount = 0;

	for( const ThingTemplate *tmpl = TheThingFactory->firstTemplate();
			 tmpl != nullptr;
			 tmpl = tmpl->friend_getNextTemplate() )
	{
		if( tmpl->getName().isEmpty() )
			continue;

		DebugSpawnEntry entry;
		entry.name			= tmpl->getName();
		entry.label			= tmpl->getDisplayName();
		entry.spawnable	= debugTemplateLooksSpawnable( tmpl );

		// "Has a display name" is not the same as "the field was set". DisplayName is parsed with
		// INI::parseAndTranslateLabel, which runs the label through TheGameText->fetch, and fetch
		// hands back the literal string MISSING: 'OBJECT:Foo' when the CSF has no such entry
		// (GameText.cpp) rather than failing. Rows reading MISSING: '...' would be worse than
		// useless as a browse label, so that counts as not having one.
		entry.hasDisplayName = ( entry.label.isEmpty() == FALSE ) &&		// no isNotEmpty on UnicodeString
													 ( entry.label.startsWithNoCase( L"MISSING:" ) == FALSE );

		if( entry.hasDisplayName == FALSE )
			entry.label.translate( entry.name );		// no usable OBJECT: string; the INI name it is

		if( entry.spawnable )
			++spawnableCount;

		// One lowercase ASCII haystack covering BOTH names, so a single strstr per row per keystroke
		// filters on either. Lossy for a localised display name in a non-Latin script - translate()
		// is single-byte - but the INI name is always ASCII, so such a template stays findable.
		entry.search.translate( entry.label );
		entry.search.concat( ' ' );
		entry.search.concat( entry.name );
		entry.search.toLower();

		s_spawnCatalog.push_back( entry );
	}

	std::sort( s_spawnCatalog.begin(), s_spawnCatalog.end(), debugSpawnEntryLess );

	// Belt and braces for a rule that cannot be verified against shipped data from here. If the
	// filter hid absolutely everything, the pane would be an empty box with no hint that a toggle
	// exists - strictly worse than the text box this replaces. Fall open instead.
	if( spawnableCount == 0 && s_spawnCatalog.empty() == FALSE )
		s_spawnShowAll = TRUE;
}

//-------------------------------------------------------------------------------------------------
static void refreshSpawnPickLabel( void )
{
	if( s_spawnPickLabel == nullptr )
		return;

	if( s_spawnPick.isEmpty() )
	{
		GadgetStaticTextSetText( s_spawnPickLabel,
			UnicodeString( L"Picked: nothing yet - tap a row above." ) );
		return;
	}

	UnicodeString text;
	text.format( L"Picked: %S", s_spawnPick.str() );
	GadgetStaticTextSetText( s_spawnPickLabel, text );
}

//-------------------------------------------------------------------------------------------------
static void refreshSpawnCountLabel( void )
{
	if( s_spawnCountValue == nullptr )
		return;

	// Range-checked even though both step handlers clamp: this reads a file-static index into a
	// fixed array from a draw-driven path, and the cost of being wrong is an out-of-bounds read.
	const Int idx = ( s_spawnCountIdx >= 0 && s_spawnCountIdx < DBG_NUM_SPAWN_COUNTS )
										? s_spawnCountIdx : 0;

	UnicodeString text;
	text.format( L"%d", s_spawnCountSteps[ idx ] );
	GadgetStaticTextSetText( s_spawnCountValue, text );
}

//-------------------------------------------------------------------------------------------------
static void refreshSpawnVetLabel( void )
{
	if( s_spawnVetValue == nullptr )
		return;

	// Labelled from the engine's own table rather than four hardcoded strings, so the pane cannot
	// drift from the enum the wire argument carries. GameCommon.cpp asserts the array is LEVEL_COUNT
	// long plus a null, and s_spawnVet is clamped to LEVEL_FIRST..LEVEL_LAST everywhere it is set.
	const char *name = ( s_spawnVet >= (Int)LEVEL_FIRST && s_spawnVet <= (Int)LEVEL_LAST )
											? TheVeterancyNames[ s_spawnVet ] : "?";

	UnicodeString text;
	text.format( L"%S", name );
	GadgetStaticTextSetText( s_spawnVetValue, text );
}

//-------------------------------------------------------------------------------------------------
/** Refill the picker from the catalog, honouring the search filter and the scope toggle.
	*
	* Runs on every keystroke, so the per-row work is one strstr against a pre-lowercased key. The
	* armed template is re-selected by NAME rather than by row, and - importantly - is NOT cleared
	* when it filters out: narrowing the search to find something else and then clearing the box must
	* not silently disarm the button you were about to press. */
//-------------------------------------------------------------------------------------------------
static void repopulateSpawnList( void )
{
	if( s_listSpawnTemplate == nullptr )
		return;

	AsciiString filter;
	filter.translate( s_spawnSearch.getFilter() );
	filter.trim();
	filter.toLower();

	GadgetListBoxReset( s_listSpawnTemplate );
	s_spawnRowIndex.clear();

	const CodeGuiTheme &theme = CodeGuiGetTheme();

	Int matched	= 0;
	Int shown		= 0;
	Int keptRow	= -1;

	for( size_t i = 0; i < s_spawnCatalog.size(); ++i )
	{
		const DebugSpawnEntry &entry = s_spawnCatalog[i];

		if( s_spawnShowAll == FALSE && entry.spawnable == FALSE )
			continue;
		if( filter.isNotEmpty() && strstr( entry.search.str(), filter.str() ) == nullptr )
			continue;

		++matched;
		if( shown >= DBG_MAX_SPAWN_ROWS )
			continue;								// keep counting, stop drawing - see DBG_MAX_SPAWN_ROWS

		// The INI name is echoed next to the display name on purpose: it is the value the order
		// actually carries, and seeing it is how anyone learns the name for a script or a log line.
		UnicodeString line;
		if( entry.hasDisplayName )
			line.format( L"%s   -   %S", entry.label.str(), entry.name.str() );
		else
			line = entry.label;

		const Int row = GadgetListBoxAddEntryText( s_listSpawnTemplate, line, theme.textNormal, -1 );
		if( row < 0 )
			break;									// listbox is full; listLength is a hard cap

		s_spawnRowIndex.push_back( (Int)i );
		if( entry.name.compare( s_spawnPick ) == 0 )
			keptRow = row;

		++shown;
	}

	if( matched > shown )
	{
		UnicodeString more;
		more.format( L"... and %d more. Type in Search to narrow the list.", matched - shown );
		if( GadgetListBoxAddEntryText( s_listSpawnTemplate, more, theme.textDisabled, -1 ) >= 0 )
			s_spawnRowIndex.push_back( -1 );		// text, not a template
	}
	else if( matched == 0 )
	{
		// An empty box explains nothing, and the three reasons it can be empty want three different
		// answers: no data loaded at all, a filter that matched nothing, or the sanity filter hiding
		// the thing being looked for.
		UnicodeString none;
		if( s_spawnCatalog.empty() )
			none = UnicodeString( L"No templates loaded - the game data is not up yet." );
		else if( s_spawnShowAll )
			none.format( L"Nothing matches. Clear the search to see all %d templates.",
									 (Int)s_spawnCatalog.size() );
		else
			none = UnicodeString( L"Nothing matches. Clear the search, or press \"Show: all\" - "
														L"this list hides templates that cannot sensibly be spawned." );

		if( GadgetListBoxAddEntryText( s_listSpawnTemplate, none, theme.textDisabled, -1 ) >= 0 )
			s_spawnRowIndex.push_back( -1 );
	}

	if( keptRow >= 0 )
	{
		GadgetListBoxSetSelected( s_listSpawnTemplate, keptRow );
		GadgetListBoxSetTopVisibleEntry( s_listSpawnTemplate, CodeGuiAtLeast( keptRow - 3, 0 ) );
	}

	refreshSpawnPickLabel();
}

//-------------------------------------------------------------------------------------------------
static void spawnSearchChanged( CodeGuiSearchField *field )
{
	repopulateSpawnList();
}

//-------------------------------------------------------------------------------------------------
/** A row was tapped. Arm the template it names, or ignore the "... and N more" footer row. */
//-------------------------------------------------------------------------------------------------
static void onSpawnRowSelected( void )
{
	if( s_listSpawnTemplate == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listSpawnTemplate, &sel );
	if( sel < 0 || sel >= (Int)s_spawnRowIndex.size() )
		return;

	const Int cat = s_spawnRowIndex[ sel ];
	if( cat < 0 || cat >= (Int)s_spawnCatalog.size() )
		return;										// the footer row - leave the previous pick armed

	s_spawnPick = s_spawnCatalog[ cat ].name;
	refreshSpawnPickLabel();
}

//-------------------------------------------------------------------------------------------------
/** Resolve the armed INI name to a live template, or explain why not.
	*
	* check == FALSE: findTemplate asserts on a miss otherwise, and a name that stopped resolving
	* because the data was reloaded under us is an expected outcome here, not a bug. Called BEFORE the
	* message is opened, so a miss cannot leave a half-built order on the stream. */
//-------------------------------------------------------------------------------------------------
static const ThingTemplate *spawnPickedTemplate( void )
{
	if( TheThingFactory == nullptr )
		return nullptr;

	if( s_spawnPick.isEmpty() )
	{
		setCheatResult( L"Pick a template from the list first." );
		return nullptr;
	}

	const ThingTemplate *tmpl = TheThingFactory->findTemplate( s_spawnPick, FALSE );
	if( tmpl == nullptr )
	{
		UnicodeString msg;
		msg.format( L"Template %S is gone - the game data reloaded. Pick it again.", s_spawnPick.str() );
		setCheatResult( msg );
	}

	return tmpl;
}

//-------------------------------------------------------------------------------------------------
static Int spawnCount( void )
{
	if( s_spawnCountIdx < 0 || s_spawnCountIdx >= DBG_NUM_SPAWN_COUNTS )
		return 1;
	return s_spawnCountSteps[ s_spawnCountIdx ];
}

//-------------------------------------------------------------------------------------------------
/** Spawn N of the picked template on the TARGET player's default team, at the camera look-at point.
	* The template id and position are resolved here and sent on the wire: there is no string argument type,
	* and reading the view in the handler would desync. Count and veterancy are optional trailing arguments, always
	* both appended in this order (the handler reads each only when present, so sending one alone is not expressible). */
//-------------------------------------------------------------------------------------------------
static void gxSpawnUnit( void )
{
	if( TheTacticalView == nullptr )
		return;

	const ThingTemplate *tmpl = spawnPickedTemplate();
	if( tmpl == nullptr )
		return;

	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_SPAWN_UNIT, target );
	if( msg == nullptr )
		return;

	const Coord3D pos		= TheTacticalView->getPosition();
	const Int     count	= spawnCount();

	msg->appendIntegerArgument( (Int)tmpl->getTemplateID() );
	msg->appendLocationArgument( pos );
	msg->appendIntegerArgument( count );
	msg->appendIntegerArgument( s_spawnVet );

	DEBUG_LOG(( "DebugMenu: sent SPAWN_UNIT template %d x%d vet %d at %.1f,%.1f for player %d",
		(Int)tmpl->getTemplateID(), count, s_spawnVet, pos.x, pos.y, target ));

	// Say "as civilians" when the simulation will give them to the Neutral player.
	const Bool neutral = ThePlayerList != nullptr && GameLogic::gxSpawnGoesNeutral( ThePlayerList->getNthPlayer( target ) );
	UnicodeString what;
	what.format( L"spawn %d x %S (%S)%s", count, s_spawnPick.str(),
							 ( s_spawnVet >= (Int)LEVEL_FIRST && s_spawnVet <= (Int)LEVEL_LAST )
								 ? TheVeterancyNames[ s_spawnVet ] : "?",
							 neutral ? L" as civilians (Neutral)" : L"" );
	setCheatOrderSent( what.str(), target );
}

//-------------------------------------------------------------------------------------------------
/** Remove objects the target player owns. The handler's sentinels (templateID <= 0 = every type, count <= 0 = all)
	* are reached via three buttons: Delete, Delete all of type, Delete everything.
	* Destroy, not kill: destroyObject leaves no wreckage, death EVA or score kill. */
//-------------------------------------------------------------------------------------------------
static void gxDeleteObjects( Bool anyType, Bool allOfThem )
{
	Int templateID = 0;								// 0 == the handler's "every type" sentinel

	if( anyType == FALSE )
	{
		const ThingTemplate *tmpl = spawnPickedTemplate();
		if( tmpl == nullptr )
			return;
		templateID = (Int)tmpl->getTemplateID();
	}

	Int target = -1;
	GameMessage *msg = beginCheatOrder( GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS, target );
	if( msg == nullptr )
		return;

	const Int count = allOfThem ? 0 : spawnCount();		// 0 == the handler's "all of them" sentinel

	msg->appendIntegerArgument( templateID );
	msg->appendIntegerArgument( count );

	DEBUG_LOG(( "DebugMenu: sent DELETE_OBJECTS template %d (0=any) count %d (0=all) for player %d",
		templateID, count, target ));

	UnicodeString what;
	if( anyType )
		what = UnicodeString( L"delete EVERYTHING owned" );
	else if( allOfThem )
		what.format( L"delete every %S", s_spawnPick.str() );
	else
		what.format( L"delete %d x %S", count, s_spawnPick.str() );

	setCheatOrderSent( what.str(), target );
}

//-------------------------------------------------------------------------------------------------
/** Force the LOCAL minimap on. Client-side, no message, cannot desync.
	* Without a Command Center localPlayerHasRadar() is false and the minimap draws black.
	* Not TheRadar->hide( idx, FALSE ): Radar::reset() keeps m_radarHidden, so it would stomp doRadarDisable(). */
//-------------------------------------------------------------------------------------------------
static void applyForceRadar( Bool on )
{
	if( TheRadar == nullptr || ThePlayerList == nullptr )
		return;

	Player *local = ThePlayerList->getLocalPlayer();
	if( local == nullptr )
		return;

	TheRadar->forceOn( local->getPlayerIndex(), on );
}

//-------------------------------------------------------------------------------------------------
static void toggleForceRadar( void )
{
	if( debugClientToolsAllowed() == FALSE )
	{
		setCheatResult( L"Refused: not in a live match." );
		return;
	}

	// A radar the player never built is an advantage over the other humans.
	const WideChar *hidden = debugOthersRefusal();
	if( hidden != nullptr )
	{
		UnicodeString msg;
		msg.format( L"Refused: %s.", hidden );
		setCheatResult( msg );
		return;
	}

	s_forceRadar = !s_forceRadar;
	applyForceRadar( s_forceRadar );

	if( TheRadar )
		TheRadar->refreshObjects();

	setCheatResult( s_forceRadar
		? UnicodeString( L"Radar forced on for you. Local only - nothing was sent." )
		: UnicodeString( L"Radar force released." ) );
}

//-------------------------------------------------------------------------------------------------
// LIVE READOUTS //////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** One walk of the object list, sixteen buckets.
	*
	* The Stats tab already walked every object in the world once
	* per 250ms tick and already called getControllingPlayer() and isKindOf(KINDOF_STRUCTURE) on each
	* one. Bucketing that same pass by player index feeds the Players tab and the cheat target readout
	* for free.
	*
	* The alternative - Player::countObjects - walks a player's whole object list PER CALL, and the
	* shipped observer panel needs four of them per player. Sixteen rows would be sixty-four full
	* walks per refresh for numbers this single pass already has. Do not reintroduce it. */
//-------------------------------------------------------------------------------------------------
struct DebugPlayerTally
{
	Int units    [ MAX_PLAYER_COUNT ];		///< alive, not a structure, not a projectile or inert prop
	Int buildings[ MAX_PLAYER_COUNT ];		///< alive, KINDOF_STRUCTURE
	Int totalObjects;
	Int deadObjects;
	Int localUnits;											///< the Stats tab's historical "Your units"
	Int localStructures;
	Int otherUnits;											///< the Stats tab's historical "Other players' units"
};
static DebugPlayerTally s_tally;

//-------------------------------------------------------------------------------------------------
static void refreshObjectTally( void )
{
	memset( &s_tally, 0, sizeof( s_tally ) );

	if( TheGameLogic == nullptr )
		return;

	// Same guard as the walk this replaces. It runs from the draw pass so it cannot land inside a
	// logic update, but a load or a clearGameData spans frames.
	const Bool objectListStable = TheGameLogic->isInGame() &&
																TheGameLogic->isLoadingMap()       == FALSE &&
																TheGameLogic->isLoadingSave()      == FALSE &&
																TheGameLogic->isClearingGameData() == FALSE;
	if( objectListStable == FALSE )
		return;

	const Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;

	for( Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		++s_tally.totalObjects;

		if( obj->isEffectivelyDead() )
		{
			++s_tally.deadObjects;
			continue;
		}

		const Bool isStructure = obj->isKindOf( KINDOF_STRUCTURE );

		// The three legacy Stats counters are reproduced exactly, including that an object with no controlling player lands in "other players' units".
		const Player *owner = obj->getControllingPlayer();
		if( local != nullptr && owner == local )
		{
			if( isStructure )
				++s_tally.localStructures;
			else
				++s_tally.localUnits;
		}
		else if( isStructure == FALSE )
		{
			++s_tally.otherUnits;
		}

		if( owner == nullptr )
			continue;

		const Int idx = owner->getPlayerIndex();
		if( idx < 0 || idx >= MAX_PLAYER_COUNT )
			continue;

		if( isStructure )
		{
			++s_tally.buildings[ idx ];
		}
		else if( obj->isKindOf( KINDOF_PROJECTILE ) == FALSE && obj->isKindOf( KINDOF_INERT ) == FALSE )
		{
			// Per-player "units" excludes shells in flight and scenery props, which is what someone
			// reading a roster means by the word. The Stats tab's counter above deliberately does not.
			++s_tally.units[ idx ];
		}
	}
}

//-------------------------------------------------------------------------------------------------
static void setStatLine( Int i, const UnicodeString &text )
{
	if( i >= 0 && i < DBG_NUM_STAT_LINES && s_statLine[i] )
		GadgetStaticTextSetText( s_statLine[i], text );
}

//-------------------------------------------------------------------------------------------------
static void setPlayerCell( Int col, Int row, const UnicodeString &text )
{
	if( col >= 0 && col < DBG_PL_COLS && row >= 0 && row < DBG_PL_ROWS && s_plCell[col][row] )
		GadgetStaticTextSetText( s_plCell[col][row], text );
}

//-------------------------------------------------------------------------------------------------
/** The live player panel. Client-only, writes nothing; shows nothing unless debugOthersRefusal() allows it.
	* Every accessor is an O(1) member read. Never use Player::getCurrentEnemy() (writes AI state, diverges targeting),
	* ScoreKeeper::calculateScore() (dirties save state) or okToPlayRadarEdgeSound (queues audio; use hasRadar()). */
//-------------------------------------------------------------------------------------------------
static void refreshPlayersTab( void )
{
	const WideChar *hidden = debugOthersRefusal();
	if( s_plRefusal != nullptr )
	{
		s_plRefusal->winHide( hidden == nullptr );
		if( hidden != nullptr )
		{
			UnicodeString msg;
			msg.format( L"Hidden: %s.", hidden );
			GadgetStaticTextSetText( s_plRefusal, msg );
		}
	}

	if( ThePlayerList == nullptr || hidden != nullptr )
	{
		for( Int r = 0; r < s_plRowsShown; ++r )
			for( Int c = 0; c < DBG_PL_COLS; ++c )
				setPlayerCell( c, r, UnicodeString::TheEmptyString );
		s_plRowsShown = 0;
		return;
	}

	// Player*, not const Player*: getPlayerDisplayName() is non-const and has no const overload.
	Player *local   = ThePlayerList->getLocalPlayer();
	Player *neutral = ThePlayerList->getNeutralPlayer();
	const Int count = ThePlayerList->getPlayerCount();

	UnicodeString text;
	Int row = 0;

	for( Int i = 0; i < count && row < DBG_PL_ROWS; ++i )
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		if( p == nullptr || p == neutral )
			continue;

		const PlayerTemplate *pt = p->getPlayerTemplate();
		if( pt == nullptr )
			continue;											// unused slot

		const Int idx = p->getPlayerIndex();

		text = p->getPlayerDisplayName();
		if( text.isEmpty() )
			text.translate( p->getPlayerName() );
		if( p == local )
			text.concat( L"  (you)" );
		setPlayerCell( 0, row, text );

		// The player's own colour, so a row is identifiable at a glance the same way the minimap is.
		// winSetEnabledTextColors wants a drop-shadow colour too; opaque black is what the theme uses.
		if( s_plCell[0][row] )
			s_plCell[0][row]->winSetEnabledTextColors( debugRowColor( p ), GameMakeColor( 0, 0, 0, 255 ) );

		setPlayerCell( 1, row, pt->getDisplayName() );

		if( p->getPlayerType() == PLAYER_HUMAN )
			text = UnicodeString( L"human" );
		else
			text.format( L"AI %s", p->isSkirmishAIPlayer() ? L"skm" : L"scr" );
		setPlayerCell( 2, row, text );

		const Money *money = p->getMoney();
		text.format( L"%u  (+%u/m)", money ? money->countMoney() : 0,
								 money ? money->getCashPerMinute() : 0 );
		setPlayerCell( 3, row, text );

		const Energy *energy = p->getEnergy();
		const Int prod = energy ? energy->getProduction()  : 0;
		const Int cons = energy ? energy->getConsumption() : 0;
		text.format( L"%d/%d %+d", prod, cons, prod - cons );
		setPlayerCell( 4, row, text );

		text.format( L"%d", s_tally.units[ idx ] );
		setPlayerCell( 5, row, text );
		text.format( L"%d", s_tally.buildings[ idx ] );
		setPlayerCell( 6, row, text );

		const WideChar *rel = L"neut";
		if( p == local )
			rel = L"self";
		else if( local != nullptr && local->getRelationship( p->getDefaultTeam() ) == ALLIES )
			rel = L"ally";
		else if( local != nullptr && local->getRelationship( p->getDefaultTeam() ) == ENEMIES )
			rel = L"enemy";

		const WideChar *life = p->isPlayerObserver() ? L"obs"
												 : ( p->isPlayerActive() ? L"alive" : L"DEAD" );

		text.format( L"%s %s%s", life, rel, p->hasRadar() ? L" R" : L"" );
		setPlayerCell( 7, row, text );

		++row;
	}

	// Only clear what was actually written last time. Blanking sixteen rows of eight labels every
	// tick would re-lay out 128 DisplayStrings four times a second for no reason.
	for( Int blank = row; blank < s_plRowsShown; ++blank )
		for( Int c = 0; c < DBG_PL_COLS; ++c )
			setPlayerCell( c, blank, UnicodeString::TheEmptyString );

	s_plRowsShown = row;
}

//-------------------------------------------------------------------------------------------------
/** One line describing whoever the cheat buttons are currently aimed at. */
//-------------------------------------------------------------------------------------------------
static void refreshCheatTargetInfo( void )
{
	if( s_cheatTargetInfo == nullptr )
		return;

	const Int target = selectedCheatTarget();
	Player *p = ( ThePlayerList != nullptr && target > 0 &&
								target < ThePlayerList->getPlayerCount() )
								? ThePlayerList->getNthPlayer( target ) : nullptr;

	if( p == nullptr )
	{
		GadgetStaticTextSetText( s_cheatTargetInfo, UnicodeString( L"Target: none" ) );
		return;
	}

	const Money  *money  = p->getMoney();
	const Energy *energy = p->getEnergy();
	const Int     tally  = ( target < MAX_PLAYER_COUNT ) ? target : 0;

	// Another player's cash, power and counts only where debugOthersRefusal() allows.
	UnicodeString state;
	if( debugMayShowPlayer( p ) )
		state.format( L"$%u   power %d/%d   %d units, %d buildings",
									money ? money->countMoney() : 0,
									energy ? energy->getProduction() : 0,
									energy ? energy->getConsumption() : 0,
									s_tally.units[ tally ], s_tally.buildings[ tally ] );
	else
		state = UnicodeString( L"(cash, power and counts hidden in this match)" );

	UnicodeString text;
	const WideChar *inactive = cheatTargetInactiveWord( p );
	text.format( L"Target %d  %s   %s   %s%s",
							 target, cheatTargetName( target ).str(), state.str(),
							 inactive == nullptr ? L"alive" : ( p->isPlayerObserver() ? L"OBSERVER" : L"DEAD" ),
							 inactive == nullptr ? L"" : L" - only Reveal / Re-shroud and Spawn (as civilians) apply" );

	GadgetStaticTextSetText( s_cheatTargetInfo, text );
}

//-------------------------------------------------------------------------------------------------
/** The Spawn pane's copy of "who is this aimed at".
	*
	* Reads the SAME selection the Cheats pane owns - selectedCheatTarget() - rather than adding a
	* second target list. The Cheats pane's listbox is hidden while this pane is up, not destroyed, so
	* the selection is live; what the pane cannot do is show it, hence this line and the pointer back
	* to where it is changed. Two target lists on one screen would be a guaranteed source of "I set it
	* to player 3 and it hit player 2". */
//-------------------------------------------------------------------------------------------------
static void refreshSpawnTarget( void )
{
	if( s_spawnTargetInfo == nullptr )
		return;

	// This pane has no equivalent of the Cheats pane's status line, so it carries the "why is
	// everything greyed" answer here rather than leaving four dead buttons unexplained. Asked before
	// the target, because a replay has a perfectly good target and still refuses every order.
	const WideChar *spawnRefusal = debugSyncedCheatsRefusal();
	if( spawnRefusal != nullptr )
	{
		UnicodeString msg;
		msg.format( L"Disabled (%s). Spawn and Delete stay greyed.", spawnRefusal );
		GadgetStaticTextSetText( s_spawnTargetInfo, msg );
		return;
	}

	const Int target = selectedCheatTarget();
	Player *p = ( ThePlayerList != nullptr && target > 0 &&
								target < ThePlayerList->getPlayerCount() )
								? ThePlayerList->getNthPlayer( target ) : nullptr;

	if( p == nullptr )
	{
		GadgetStaticTextSetText( s_spawnTargetInfo,
			UnicodeString( L"Target: none. Pick a player on the Cheats tab first." ) );
		return;
	}

	const Int tally = ( target < MAX_PLAYER_COUNT ) ? target : 0;

	UnicodeString text;
	const WideChar *inactive = cheatTargetInactiveWord( p );
	// Spawn stays live and makes civilians - the Neutral player's, vehicles unmanned -
	// when the target is out of the game (GameLogic::gxSpawnGoesNeutral); said here, before the click. Delete
	// still refuses such a target. GeneralsX: Whether YOU are observing or defeated no longer
	// matters: the target owns what you spawn.
	if( inactive != nullptr )
		text.format( L"Target %d  %s is %s - Spawn makes civilians (Neutral), Delete stays greyed   (change it on the Cheats tab)",
								 target, cheatTargetName( target ).str(), inactive );
	else if( debugMayShowPlayer( p ) )
		text.format( L"Target %d  %s   -   owns %d units, %d buildings   (change it on the Cheats tab)",
								 target, cheatTargetName( target ).str(),
								 s_tally.units[ tally ], s_tally.buildings[ tally ] );
	else		// debugOthersRefusal()
		text.format( L"Target %d  %s   -   counts hidden in this match   (change it on the Cheats tab)",
								 target, cheatTargetName( target ).str() );

	GadgetStaticTextSetText( s_spawnTargetInfo, text );
}

//-------------------------------------------------------------------------------------------------
/** Frame pacing readout.
	*
	* The two values are not independent and the failure mode of treating them as if they were is
	* silent. GameEngine::canUpdateRegularGameLogic() ticks the simulation once per RENDERED frame
	* whenever the logic time scale is off - or is sitting at or above the render cap, which is the
	* same thing as far as that test is concerned - so raising the render cap on its own does not
	* make the game smoother, it makes it run at renderFps/30 times normal speed. Hence the third
	* line: the fps pair is diagnostic, the resulting game speed is the answer. */
//-------------------------------------------------------------------------------------------------
static void refreshFpsReadout( void )
{
	if( TheFramePacer == nullptr )
		return;

	const Int  maxRenderFps		= TheFramePacer->getFramesPerSecondLimit();
	const Int  logicFps				= TheFramePacer->getLogicTimeScaleFps();
	const Bool logicEnabled		= TheFramePacer->isLogicTimeScaleEnabled();
	const Bool renderUncapped	= ( maxRenderFps >= (Int)RenderFpsPreset::UncappedFpsValue );

	// Exactly the test canUpdateRegularGameLogic() makes. "Enabled" alone is not enough: a scale at
	// or above the render cap can never gate anything, so the time accumulator is skipped entirely
	// and the logic falls back to one tick per rendered frame.
	const Bool logicFollowsRender = ( logicEnabled == FALSE || logicFps >= maxRenderFps );

	UnicodeString text;

	if( s_fpsRenderValue )
	{
		if( renderUncapped )
			text = UnicodeString( L"Uncapped" );
		else if( TheFramePacer->isActualFramesPerSecondLimitEnabled() == FALSE )
			text.format( L"%d   (limiter off, running uncapped)", maxRenderFps );
		else
			text.format( L"%d", maxRenderFps );

		GadgetStaticTextSetText( s_fpsRenderValue, text );
	}

	if( s_fpsLogicValue )
	{
		// Ignore*: this line reports the configured scale, so it must not read 0.00 just because a
		// cutscene has time frozen or the network is stalling. Those two are the Game speed line's
		// business, and answering them in both places at once is what makes a readout unreadable.
		const Real ratio = TheFramePacer->getActualLogicTimeScaleRatio(
			FramePacer::IgnoreFrozenTime | FramePacer::IgnoreHaltedGame );

		if( TheNetwork != nullptr )
			text.format( L"fixed by the network match   ratio %.2f", ratio );
		else if( logicFollowsRender )
			text.format( L"off - follows render   (stored %d)", logicFps );
		else
			text.format( L"%d   ratio %.2f", logicFps, ratio );

		GadgetStaticTextSetText( s_fpsLogicValue, text );
	}

	if( s_fpsSpeedValue )
	{
		if( TheFramePacer->isFastForwardActive() )
		{
			text.format( L"fast forward: logic paced at %d Hz", TheFramePacer->getFastForwardTickFps() );
		}
		else if( TheNetwork != nullptr )
		{
			text = UnicodeString( L"paced by the network host" );
		}
		else if( TheFramePacer->isTimeFrozen() || TheFramePacer->isGameHalted() )
		{
			text = UnicodeString( L"0.00x - time is frozen" );
		}
		else
		{
			// One logic tick always advances a fixed 1/30s of simulated time. What changes is how
			// often a tick is allowed, so the speed multiple is simply tick rate over the base rate.
			const Real tickFps = logicFollowsRender ? TheFramePacer->getUpdateFps() : (Real)logicFps;
			text.format( L"%.2fx normal   (logic ticks at %.0f Hz, base is %d)",
									 tickFps / LOGICFRAMES_PER_SECONDS_REAL, tickFps, (Int)LOGICFRAMES_PER_SECOND );
		}

		GadgetStaticTextSetText( s_fpsSpeedValue, text );
	}

	if( s_ffValue )
	{
		const WideChar *refusal = gxFastForwardRefusal();
		const Int speed = TheFramePacer->getFastForward();
		if( refusal != nullptr )
		{
			text = UnicodeString( L"off - " );
			text.concat( refusal );
		}
		else if( speed <= 1 )
			text = UnicodeString( L"1x" );
		else if( TheFramePacer->getFastForwardAchieved() > 0.0f )
			text.format( L"%dx   achieved %.1fx", speed, (double)TheFramePacer->getFastForwardAchieved() );
		else
			text.format( L"%dx", speed );
		GadgetStaticTextSetText( s_ffValue, text );
		if( s_buttonFastForwardDown )	s_buttonFastForwardDown->winEnable( refusal == nullptr );
		if( s_buttonFastForwardUp )		s_buttonFastForwardUp->winEnable( refusal == nullptr );
	}

	if( s_fpsNote )
	{
		if( TheNetwork != nullptr )
			text = UnicodeString( L"Logic time scale is refused in a network match: both peers must step the same way." );
		else if( logicFollowsRender && maxRenderFps > (Int)LOGICFRAMES_PER_SECOND )
			text = UnicodeString( L"The match is running fast. Step the logic time scale below the render cap." );
		else if( logicFollowsRender )
			text = UnicodeString( L"Logic time scale is off, so the simulation runs at render speed." );
		else
			text = UnicodeString( L"Logic time scale paces the simulation; the render cap only buys smoothness." );

		GadgetStaticTextSetText( s_fpsNote, text );
	}
}

//-------------------------------------------------------------------------------------------------
void DebugMenuUpdate( void )
{
	if( s_layout == nullptr || s_root == nullptr )
		return;

	// Four refreshes a second. Walking every object in the world once per rendered frame would be a
	// measurable cost in exactly the situation the readout exists to diagnose.
	const UnsignedInt now = timeGetTime();
	if( s_lastRefreshMs != 0 && ( now - s_lastRefreshMs ) < 250 )
		return;
	s_lastRefreshMs = now;

	// Cheats can become legal or illegal without the screen closing (start a skirmish with the
	// overlay up), so the gate is re-evaluated here rather than once at build time.
	const WideChar *refusal = debugSyncedCheatsRefusal();
	const Bool cheats = ( refusal == nullptr );
	const Bool client = debugClientToolsAllowed();
	const Bool others = ( debugOthersRefusal() == nullptr );	// Force radar + the readouts below

	// Say why the row is dead before it is clicked. A guest in a
	// LAN game sees seven greyed buttons and no reason; the result line is otherwise only written on
	// a click, which a disabled button never delivers. Only written while refusing, so it does not
	// stomp the outcome text of a cheat that just went through.
	if( refusal != nullptr && s_cheatResult != nullptr )
	{
		UnicodeString msg;
		msg.format( L"Disabled: %s.", refusal );
		GadgetStaticTextSetText( s_cheatResult, msg );
	}

	// Grey what beginCheatOrder() would refuse for a defeated or
	// observing target, so the row says so before it is clicked. No target: left to beginCheatOrder.
	const Int cheatTarget = selectedCheatTarget();
	const Player *targetPlayer = ( cheatTarget > 0 && ThePlayerList != nullptr ) ? ThePlayerList->getNthPlayer( cheatTarget ) : nullptr;
	// The reason is shown on the two target lines (refreshCheatTargetInfo / refreshSpawnTarget), NOT on
	// the result line: this runs every 250 ms and would overwrite a Reveal's "Sent:".
	const Bool orders = cheats && cheatTargetInactiveWord( targetPlayer ) == nullptr;

	if( s_buttonGiveCash )		s_buttonGiveCash->winEnable( orders );
	if( s_buttonSetCash )			s_buttonSetCash->winEnable( orders );
	if( s_buttonReveal )			s_buttonReveal->winEnable( cheats );
	if( s_buttonUnreveal )		s_buttonUnreveal->winEnable( cheats );
	if( s_buttonKillPlayer )	s_buttonKillPlayer->winEnable( orders );
	if( s_buttonKillUnits )		s_buttonKillUnits->winEnable( orders );
	if( s_buttonKillAll )			s_buttonKillAll->winEnable( orders );
	if( s_buttonSpawn )				s_buttonSpawn->winEnable( cheats );	// a target out of the game gets civilians
	if( s_buttonForceRadar )	s_buttonForceRadar->winEnable( client && others );

	// The Spawn pane's three Delete buttons ride the same gate as the orders above (Spawn is set above, on `cheats`).
	// The steppers, the scope toggle and the picker are pure client state and stay live, so the pane
	// can be set up before a match starts and fired the moment one does.
	if( s_buttonSpawnDelete )			s_buttonSpawnDelete->winEnable( orders );
	if( s_buttonSpawnDeleteType )	s_buttonSpawnDeleteType->winEnable( orders );
	if( s_buttonSpawnDeleteAll )	s_buttonSpawnDeleteAll->winEnable( orders );

	if( s_cheatStatus )
	{
		GadgetStaticTextSetText( s_cheatStatus, cheats
			? UnicodeString( L"Cheats are sent as network orders: they apply on EVERY peer, a few frames late." )
			: UnicodeString( L"Disabled: no live match, or this is a replay (an injected order would diverge from the file)." ) );
	}

	// Radar::newMap calls reset(), which clears m_radarForceOn, so the force dies on every map
	// transition. Re-assert it rather than making the user notice and press the button again. Only
	// ever asserted, never cleared: pushing FALSE every tick would stomp the forceOn that
	// VictoryConditions sets for a locally defeated player.
	//
	// ...and never into a match debugOthersRefusal() refuses: a force switched on in
	// a skirmish used to come back in the next online match while this screen was up. There the flag is dropped instead
	// (the button then reads "Force radar" again). No forceOn( FALSE ): the match's own Radar::newMap -> reset() clears any
	// force taken before it (prepareNewGame sets the LAN / internet mode first, so this already refuses by then), and FALSE
	// would stomp a map script's own forceOn.
	if( others == FALSE )
		s_forceRadar = FALSE;
	else if( s_forceRadar )
		applyForceRadar( TRUE );

	if( s_buttonForceRadar && s_forceRadarShown != (Int)s_forceRadar )
	{
		GadgetButtonSetText( s_buttonForceRadar, s_forceRadar
			? UnicodeString( L"Radar: forced" )
			: UnicodeString( L"Force radar" ) );
		s_forceRadarShown = (Int)s_forceRadar;
	}

	if( s_buttonSaveHere )
	{
		const Bool canSave = ( TheGameLogic != nullptr && TheGameLogic->isInGame() &&
													 TheGameLogic->isInShellGame() == FALSE &&
													 TheGameLogic->isInReplayGame() == FALSE );
		s_buttonSaveHere->winEnable( canSave );
	}

	// changeLogicTimeScale() returns false without doing anything while TheNetwork is up, and joining
	// or leaving a match does not close this screen. A button that silently does nothing reads as a
	// broken button, so it is greyed for as long as the refusal stands. The render cap is untouched
	// by that rule - it is purely local presentation - so it stays live.
	const Bool logicScaleAllowed = ( TheNetwork == nullptr );
	if( s_buttonLogicFpsDown )	s_buttonLogicFpsDown->winEnable( logicScaleAllowed );
	if( s_buttonLogicFpsUp )		s_buttonLogicFpsUp->winEnable( logicScaleAllowed );

	// The object walk is shared by four panes, so it happens once and only for a pane that wants it.
	if( s_activeTab != DBG_TAB_STATS && s_activeTab != DBG_TAB_PLAYERS &&
			s_activeTab != DBG_TAB_CHEATS && s_activeTab != DBG_TAB_SPAWN )
		return;

	refreshObjectTally();

	if( s_activeTab == DBG_TAB_CHEATS || s_activeTab == DBG_TAB_SPAWN )
	{
		// The roster only changes at newGame, but this screen outlives a match, so notice when it does.
		// Rebuilding unconditionally would clear the selection four times a second.
		//
		// Done for the Spawn pane too even though its listbox is not the one being rebuilt: the target
		// is READ off the Cheats pane's list, so a roster that changed while the user sat on Spawn
		// would leave the parallel index vector naming players that no longer exist.
		if( cheatRosterChanged() )
			refreshCheatTargets();

		if( s_activeTab == DBG_TAB_SPAWN )
			refreshSpawnTarget();
		else
			refreshCheatTargetInfo();
		return;
	}

	if( s_activeTab == DBG_TAB_PLAYERS )
	{
		refreshPlayersTab();
		return;
	}

	refreshFpsReadout();

	UnicodeString line;
	Int n = 0;

	line.format( L"Render FPS            %.1f", TheDisplay ? TheDisplay->getAverageFPS() : 0.0f );
	setStatLine( n++, line );

	line.format( L"Display               %d x %d", CodeGuiScreenW(), CodeGuiScreenH() );
	setStatLine( n++, line );

	if( TheGameLogic == nullptr )
	{
		for( ; n < DBG_NUM_STAT_LINES; ++n )
			setStatLine( n, UnicodeString::TheEmptyString );
		return;
	}

	line.format( L"Game mode             %S", toString( TheGameLogic->getGameMode() ) );
	setStatLine( n++, line );

	line.format( L"Logic frame           %u", TheGameLogic->getFrame() );
	setStatLine( n++, line );

	// refreshObjectTally() above did the walk these four lines used to do inline, and reproduces all
	// four counters exactly - including the quirk that an object with no controlling player is
	// counted under "other players' units".
	// The world total less your own is the other players' army, so both totals ride
	// debugOthersRefusal() like the Players tab does; your own two lines stay.
	if( others )
		line.format( L"Objects in world      %d  (%d effectively dead)",
								 s_tally.totalObjects, s_tally.deadObjects );
	else
		line = UnicodeString( L"Objects in world      hidden in this match" );
	setStatLine( n++, line );
	line.format( L"Your units            %d", s_tally.localUnits );
	setStatLine( n++, line );
	line.format( L"Your structures       %d", s_tally.localStructures );
	setStatLine( n++, line );
	if( others )
		line.format( L"Other players' units  %d", s_tally.otherUnits );
	else
		line = UnicodeString( L"Other players' units  hidden in this match" );
	setStatLine( n++, line );

	line.format( L"Players               %d", ( ThePlayerList != nullptr )
																								? ThePlayerList->getPlayerCount() : 0 );
	setStatLine( n++, line );

	for( ; n < DBG_NUM_STAT_LINES; ++n )
		setStatLine( n, UnicodeString::TheEmptyString );
}

//-------------------------------------------------------------------------------------------------
// The active tab's text field, or nullptr for a tab without one. A field that is hidden or disabled is not offered:
// winSetFocus would bounce off it every frame.
static GameWindow *debugMenuKeyboardHome( void )
{
	GameWindow *home = nullptr;
	switch( s_activeTab )
	{
		case DBG_TAB_SPAWN:		home = s_spawnSearch.getEntry(); break;
		case DBG_TAB_CHEATS:	home = s_entryCashAmount; break;
		case DBG_TAB_CONFIG:	home = s_entryPrefValue; break;
		case DBG_TAB_SAVES:		home = s_entrySaveDesc; break;
		default: break;
	}
	if( home == nullptr || home->winIsHidden() || !home->winGetEnabled() )
		return nullptr;
	return home;
}

//-------------------------------------------------------------------------------------------------
/** Root draw func.
	*
	* The overlay is off the shell stack on purpose, so nothing in Shell::update ticks it. drawWindow
	* paints a window before its children, so refreshing the labels from here lands in the same frame
	* they are drawn, and the original default draw is chained rather than replaced. */
//-------------------------------------------------------------------------------------------------
static void debugMenuRootDraw( GameWindow *window, WinInstanceData *instData )
{
	if( s_rootDraw )
		s_rootDraw( window, instData );

	// The overlay keeps the keyboard while drawn: switchToContext drops focus on its root, which would send
	// Escape and hotkeys to the game. Focus that went somewhere real (chat, message box) is left alone.
	// A used text field keeps the keyboard while its tab is up, but is never grabbed before the first click (iOS).
	// Cost: arrow keys no longer move through those lists once the field has been used.
	if( TheWindowManager != nullptr )
	{
		GameWindow *focus = TheWindowManager->winGetFocus();
		GameWindow *modal = TheWindowManager->winGetModal();
		GameWindow *home = debugMenuKeyboardHome();
		if( home != nullptr && focus == home )
			s_keyboardKept = home;
		else if( s_keyboardKept != home )
			s_keyboardKept = nullptr;
		const Bool focusInOverlay = focus == nullptr || focus == s_root || ( s_root != nullptr && s_root->winIsChild( focus ) );
		if( modal == nullptr && s_keyboardKept != nullptr && focus != s_keyboardKept && focusInOverlay
				&& !( focus != nullptr && BitIsSet( focus->winGetStyle(), GWS_ENTRY_FIELD ) ) )
			TheWindowManager->winSetFocus( s_keyboardKept );
		if( TheWindowManager->winGetFocus() == nullptr )
			TheWindowManager->winSetFocus( modal != nullptr ? modal : s_root );
	}

	DebugMenuUpdate();
}

//-------------------------------------------------------------------------------------------------
// TAB SWITCHING //////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void selectTab( Int tab )
{
	if( tab < 0 || tab >= DBG_TAB_COUNT )
		return;

	s_activeTab = tab;

	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		if( s_tabPanel[i] )
			s_tabPanel[i]->winHide( i != tab );

		// The tab button for the visible pane is disabled so it reads as "you are here". Colour alone
		// would not survive the hilite state a touch leaves behind.
		if( s_tabButton[i] )
			s_tabButton[i]->winEnable( i != tab );
	}

	// The archive walk is not cheap, so it happens the first time the pane is actually looked at.
	if( tab == DBG_TAB_CONFIG && s_configLoaded == FALSE )
	{
		refreshResolvedConfig();
		refreshPrefList();
	}
	else if( tab == DBG_TAB_SAVES )
	{
		refreshSaveList();
	}
	else if( tab == DBG_TAB_SPAWN )
	{
		// Walking a few thousand templates and sorting them is not something to do at screen-open time
		// for a pane nobody may visit, so the catalog is built on first sight - the same lazy rule the
		// Config pane's archive walk uses. Thrown away by nullifyControls(), hence once per lifetime.
		if( s_spawnCatalogBuilt == FALSE )
		{
			buildSpawnCatalog();
			repopulateSpawnList();
		}

		s_lastRefreshMs = 0;
		DebugMenuUpdate();
	}
	else if( tab == DBG_TAB_PLAYERS || tab == DBG_TAB_CHEATS )
	{
		// Fill the pane now instead of showing an empty grid for up to a quarter of a second. Clearing
		// the throttle stamp is what lets the tick body run out of turn; it re-stamps itself.
		s_lastRefreshMs = 0;
		DebugMenuUpdate();
	}

	// Hiding a pane that contained the focused text entry clears m_keyboardFocus outright
	// (GameWindowManager::windowHiding), which would silently kill the ESC handler. Take it back.
	if( TheWindowManager && s_root )
		TheWindowManager->winSetFocus( s_root );
}

//-------------------------------------------------------------------------------------------------
// CONSTRUCTION ///////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void nullifyControls( void )
{
	s_keyboardKept = nullptr;
	s_root					= nullptr;
	s_rootDraw			= nullptr;
	s_buttonBack		= nullptr;

	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		s_tabButton[i]	= nullptr;
		s_tabPanel[i]		= nullptr;
	}
	for( Int i = 0; i < DBG_NUM_STAT_LINES; ++i )
		s_statLine[i] = nullptr;

	s_fpsRenderValue			= nullptr;
	s_fpsLogicValue				= nullptr;
	s_fpsSpeedValue				= nullptr;
	s_fpsNote							= nullptr;
	s_buttonRenderFpsDown	= nullptr;
	s_buttonRenderFpsUp		= nullptr;
	s_buttonLogicFpsDown	= nullptr;
	s_buttonLogicFpsUp		= nullptr;
	s_ffValue							= nullptr;
	s_buttonFastForwardDown	= nullptr;
	s_buttonFastForwardUp	= nullptr;

	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		s_slider[i]				= nullptr;
		s_sliderValue[i]	= nullptr;
	}

	s_cheatStatus					= nullptr;
	s_cheatResult					= nullptr;
	s_buttonGiveCash			= nullptr;
	s_buttonReveal				= nullptr;

	// The targeted-cheat and player-panel controls. Every one of
	// these MUST be here: GameWindowManager::reset() runs winDestroyAll() on every match transition
	// without going through DebugMenuClose, and the next DebugMenuUpdate would then write through the
	// freed windows. A non-null freed pointer passes every guard in this file.
	s_listCheatTarget			= nullptr;
	s_cheatTargetInfo			= nullptr;
	s_entryCashAmount			= nullptr;
	s_buttonSetCash				= nullptr;
	s_buttonUnreveal			= nullptr;
	s_buttonForceRadar		= nullptr;
	s_buttonKillPlayer		= nullptr;
	s_buttonKillUnits			= nullptr;
	s_buttonKillAll				= nullptr;

	for( Int c = 0; c < DBG_PL_COLS; ++c )
		for( Int r = 0; r < DBG_PL_ROWS; ++r )
			s_plCell[c][r] = nullptr;
	s_plRowsShown = 0;
	s_plRefusal = nullptr;

	s_cheatTargetIndex.clear();
	s_cheatRosterCount = -1;					// forces a rebuild of the target list on the next tick
	for( Int i = 0; i < MAX_PLAYER_COUNT; ++i )
		s_cheatRosterKey[i] = NAMEKEY_INVALID;

	// The Spawn pane. Same rule as the block above: every one of
	// these is written unconditionally by the next DebugMenuUpdate, and a freed pointer is not null.
	s_listSpawnTemplate			= nullptr;
	s_spawnTargetInfo				= nullptr;
	s_spawnPickLabel				= nullptr;
	s_spawnCountValue				= nullptr;
	s_spawnVetValue					= nullptr;
	s_spawnResult						= nullptr;
	s_buttonSpawn						= nullptr;
	s_buttonSpawnScope			= nullptr;
	s_buttonSpawnCountDown	= nullptr;
	s_buttonSpawnCountUp		= nullptr;
	s_buttonSpawnVetPrev		= nullptr;
	s_buttonSpawnVetNext		= nullptr;
	s_buttonSpawnDelete			= nullptr;
	s_buttonSpawnDeleteType	= nullptr;
	s_buttonSpawnDeleteAll	= nullptr;

	// The search field holds three raw window pointers of its own and does NOT own them - the pane's
	// layout destroys them. reset() is how it forgets them; see the lifetime note in
	// CodeGuiSearchField.h. Skipping this is the same dangle as any pointer above.
	s_spawnSearchSaved = s_spawnSearch.getFilter();
	s_spawnSearch.reset();

	// The catalog caches template NAMES, which are safe to hold, but the whole point of rebuilding is
	// that ThingFactory::reset() runs on the same match transition that gets us here and can add,
	// remove and renumber templates. s_spawnRowIndex must go with it or it would index a shorter
	// vector on the next tap.
	s_spawnCatalog.clear();
	s_spawnRowIndex.clear();
	s_spawnCatalogBuilt = FALSE;

	// The armed pick survives, like the steppers beside it. A stale pick (template gone after a data reload)
	// is handled at the consumer: findTemplate returns null and the spawn path says to pick it again.

	// s_forceRadar is deliberately NOT cleared: it mirrors engine state that outlives this screen, and
	// the tick re-asserts it after Radar::reset(). It is only released when the user presses the
	// button again. The LABEL state is cleared, because the next button is a brand new window.
	s_forceRadarShown = -1;

	s_buttonCamDefaults		= nullptr;
	s_buttonCamApply			= nullptr;
	s_buttonUncapPitch		= nullptr;
	s_cameraNote					= nullptr;
	s_listResolved				= nullptr;
	s_listPrefs						= nullptr;
	s_entryPrefValue			= nullptr;
	s_buttonCfgRefresh		= nullptr;
	s_buttonPrefSet				= nullptr;
	s_buttonPrefSave			= nullptr;
	s_listSaves						= nullptr;
	s_entrySaveDesc				= nullptr;
	s_buttonSaveRefresh		= nullptr;
	s_buttonSaveLoad			= nullptr;
	s_buttonSaveHere			= nullptr;

	s_prefRowKeys.clear();
	s_configLoaded	= FALSE;
	s_lastRefreshMs	= 0;
}

//-------------------------------------------------------------------------------------------------
static void buildStatsTab( GameWindow *pane, Int paneW )
{
	const Int pad		= CodeGuiScaleX( DBG_PAD );
	const Int lineH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int step	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 15 );
	const Int innerW	= paneW - pad * 2;

	for( Int i = 0; i < DBG_NUM_STAT_LINES; ++i )
	{
		s_statLine[i] = CodeGuiLabel( pane, pad, i * step, innerW, lineH,
																	nullptr, L"", FALSE, 12 );
	}

	// Frame pacing. This lives on Stats rather than Config because the two knobs are only
	// meaningful next to the measured render FPS above them, and because the pair has to be read
	// together: the render cap alone decides nothing about how fast the match plays.
	const Int btnH			= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int labelW		= CodeGuiScaleX( 160 );
	const Int valueW		= CodeGuiScaleX( 232 );
	const Int valueX		= pad + CodeGuiScaleX( 164 );
	const Int stepBtnW	= CodeGuiScaleX( 70 );
	const Int minusX		= pad + CodeGuiScaleX( 404 );
	const Int plusX			= pad + CodeGuiScaleX( 480 );

	Int y = DBG_NUM_STAT_LINES * step + CodeGuiScaleY( 8 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"FRAME PACING", FALSE, 12 );

	y += CodeGuiScaleY( 24 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 4 ), labelW, lineH,
								nullptr, L"Max render FPS", FALSE, 12 );
	s_fpsRenderValue = CodeGuiLabel( pane, valueX, y + CodeGuiScaleY( 4 ),
																	 valueW, lineH, nullptr, L"", FALSE, 12 );
	s_buttonRenderFpsDown = CodeGuiButton( pane, minusX, y, stepBtnW, btnH,
																				 "DebugMenu:ButtonRenderFpsDown", L"-",
																				 L"Step the render cap down one preset" );
	s_buttonRenderFpsUp = CodeGuiButton( pane, plusX, y, stepBtnW, btnH,
																			 "DebugMenu:ButtonRenderFpsUp", L"+",
																			 L"Step the render cap up one preset" );

	y += CodeGuiScaleY( 34 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 4 ), labelW, lineH,
								nullptr, L"Logic time scale", FALSE, 12 );
	s_fpsLogicValue = CodeGuiLabel( pane, valueX, y + CodeGuiScaleY( 4 ),
																	valueW, lineH, nullptr, L"", FALSE, 12 );
	s_buttonLogicFpsDown = CodeGuiButton( pane, minusX, y, stepBtnW, btnH,
																				"DebugMenu:ButtonLogicFpsDown", L"-",
																				L"Step the simulation rate down 5 fps" );
	s_buttonLogicFpsUp = CodeGuiButton( pane, plusX, y, stepBtnW, btnH,
																			"DebugMenu:ButtonLogicFpsUp", L"+",
																			L"Step the simulation rate up 5 fps" );

	// Fast forward: 1x 2x 3x 4x 5x 10x, only while this player is the match's only
	// human (changeFastForward says why not otherwise, and the buttons grey on the same refusal).
	y += CodeGuiScaleY( 34 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 4 ), labelW, lineH,
								nullptr, L"Fast forward", FALSE, 12 );
	s_ffValue = CodeGuiLabel( pane, valueX, y + CodeGuiScaleY( 4 ),
														valueW, lineH, nullptr, L"", FALSE, 12 );
	s_buttonFastForwardDown = CodeGuiButton( pane, minusX, y, stepBtnW, btnH,
																					 "DebugMenu:ButtonFastForwardDown", L"-",
																					 L"Slower: 10x 5x 4x 3x 2x 1x" );
	s_buttonFastForwardUp = CodeGuiButton( pane, plusX, y, stepBtnW, btnH,
																				 "DebugMenu:ButtonFastForwardUp", L"+",
																				 L"Faster: 1x 2x 3x 4x 5x 10x - only with no other human in the match" );

	// The line that answers the question the other two only pose.
	y += CodeGuiScaleY( 36 );
	CodeGuiLabel( pane, pad, y, labelW, lineH, nullptr, L"Game speed", FALSE, 12 );
	s_fpsSpeedValue = CodeGuiLabel( pane, valueX, y,
																	innerW - CodeGuiScaleX( 164 ), lineH, nullptr, L"", FALSE, 12 );

	y += CodeGuiScaleY( 20 );
	s_fpsNote = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
/** The live player panel.
	*
	* A grid of labels rather than a listbox, because CodeGuiListbox hard-codes listboxData.columns = 1
	* and a multi-column listbox is simply not reachable from code. Columns are authoring-space and the
	* last one must end at or before 632: the pane is already inset by pad on both sides of a panel
	* that was itself inset by pad, so the usable width is paneW - 2*pad == 680 - 4*12. */
//-------------------------------------------------------------------------------------------------
static void buildPlayersTab( GameWindow *pane, Int paneW )
{
	const Int pad		= CodeGuiScaleX( DBG_PAD );
	const Int lineH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int step	= CodeGuiAtLeast( CodeGuiScaleY( 20 ), 14 );
	const Int innerW	= paneW - pad * 2;

	static const Int colX[ DBG_PL_COLS ] = {   0, 112, 208, 258, 340, 428, 466, 504 };
	static const Int colW[ DBG_PL_COLS ] = { 108,  92,  46,  78,  84,  34,  34, 128 };
	static const WideChar *hdr[ DBG_PL_COLS ] =
		{ L"PLAYER", L"FACTION", L"TYPE", L"CASH", L"POWER", L"U", L"B", L"STATE" };

	for( Int c = 0; c < DBG_PL_COLS; ++c )
	{
		CodeGuiLabel( pane, pad + CodeGuiScaleX( colX[c] ), 0,
									CodeGuiScaleX( colW[c] ), lineH, nullptr, hdr[c], FALSE, 12 );
	}

	for( Int r = 0; r < DBG_PL_ROWS; ++r )
	{
		for( Int c = 0; c < DBG_PL_COLS; ++c )
		{
			s_plCell[c][r] = CodeGuiLabel( pane, pad + CodeGuiScaleX( colX[c] ), ( r + 1 ) * step,
																		 CodeGuiScaleX( colW[c] ), lineH, nullptr, L"", FALSE, 12 );
		}
	}

	// Why the table is empty (refreshPlayersTab). One label across the first row,
	// which is blank whenever this shows, so the two never overlap.
	s_plRefusal = CodeGuiLabel( pane, pad, step, innerW, lineH, nullptr, L"", FALSE, 12 );
	if( s_plRefusal != nullptr )
		s_plRefusal->winHide( TRUE );

	CodeGuiLabel( pane, pad, ( DBG_PL_ROWS + 1 ) * step, innerW, lineH, nullptr,
								L"Read-only, nothing is sent. POWER is produced/used. This shows information the "
								L"fog of war hides, so a network match against another human withholds it.", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
static void buildCheatsTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	s_cheatStatus = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Target player - every order below is aimed at this one, on every machine.",
								FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listCheatTarget = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 104 ),
																			"DebugMenu:ListCheatTarget", MAX_PLAYER_COUNT, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 110 );
	s_cheatTargetInfo = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );

	// Money. One amount field feeds both buttons; a negative amount is a legal way to take cash away.
	y += CodeGuiScaleY( 26 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 2 ), CodeGuiScaleX( 70 ), lineH,
								nullptr, L"Amount", FALSE, 12 );
	s_entryCashAmount = CodeGuiTextEntry( pane, pad + CodeGuiScaleX( 74 ), y, CodeGuiScaleX( 110 ), fieldH,
																				"DebugMenu:EntryCashAmount", 12 );
	s_buttonGiveCash = CodeGuiButton( pane, pad + CodeGuiScaleX( 192 ), y - CodeGuiScaleY( 3 ),
																		CodeGuiScaleX( 110 ), btnH,
																		"DebugMenu:ButtonGiveCash", L"Give cash",
																		L"Add the amount to the target's account (negative takes it away)" );
	s_buttonSetCash = CodeGuiButton( pane, pad + CodeGuiScaleX( 310 ), y - CodeGuiScaleY( 3 ),
																	 CodeGuiScaleX( 110 ), btnH,
																	 "DebugMenu:ButtonSetCash", L"Set cash",
																	 L"Replace the target's account with the amount" );

	y += CodeGuiScaleY( 34 );
	s_buttonReveal = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 120 ), btnH,
																	"DebugMenu:ButtonRevealMap", L"Reveal map",
																	L"Permanently reveal the shroud for the target player" );
	s_buttonUnreveal = CodeGuiButton( pane, pad + CodeGuiScaleX( 126 ), y, CodeGuiScaleX( 120 ), btnH,
																		"DebugMenu:ButtonUnrevealMap", L"Re-shroud",
																		L"Undo the reveal and put the fog back" );
	s_buttonForceRadar = CodeGuiButton( pane, pad + CodeGuiScaleX( 252 ), y, CodeGuiScaleX( 130 ), btnH,
																			"DebugMenu:ButtonForceRadar", L"Force radar",
																			L"Light YOUR minimap with no Command Center. Local only, nothing is sent" );
	s_buttonKillPlayer = CodeGuiButton( pane, pad + CodeGuiScaleX( 390 ), y, CodeGuiScaleX( 140 ), btnH,
																			"DebugMenu:ButtonKillPlayer", L"Defeat player",
																			L"Destroy everything the target owns, handing it to a living ally first" );

	y += CodeGuiScaleY( 34 );
	s_buttonKillUnits = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 140 ), btnH,
																		 "DebugMenu:ButtonKillUnits", L"Kill units",
																		 L"Kill the target's units and leave the buildings standing" );
	s_buttonKillAll = CodeGuiButton( pane, pad + CodeGuiScaleX( 148 ), y, CodeGuiScaleX( 190 ), btnH,
																	 "DebugMenu:ButtonKillAll", L"Kill units + buildings",
																	 L"Kill everything the target owns" );

	// The spawn text box used to be here. It is now a searchable
	// picker on its own pane, because a control that only works if you can type "AmericaVehicleHumvee"
	// exactly is not a control on a touch device. The pointer line stays so the button does not just
	// vanish for anyone who knew where it was.
	y += CodeGuiScaleY( 34 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Spawning and deleting units moved to the Spawn tab - it has a searchable list.",
								FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_cheatResult = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
/** The spawn/delete pane.
	* Vertical budget is 390 authoring units, offsets run through CodeGuiScaleY; a child past it is clipped
	* and unhittable. The last control ends at 380, so additions must come out of the listbox.
	* CodeGuiSearchField::create() builds itself above the listbox and shrinks it: create it right after the listbox. */
//-------------------------------------------------------------------------------------------------
static void buildSpawnTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Pick a template, set how many and how veteran, then Spawn or Delete.", FALSE, 12 );

	y += CodeGuiScaleY( 20 );
	s_spawnTargetInfo = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );

	// listLength has to clear DBG_MAX_SPAWN_ROWS plus the trailing "... and N more" row, or the cap
	// would be enforced by GadgetListBoxAddEntryText returning -1 instead of by us, and the footer
	// row - the only thing that says the list was truncated - would be the entry that got dropped.
	y = CodeGuiScaleY( 44 );
	s_listSpawnTemplate = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 190 ),
																				"DebugMenu:ListSpawnTemplate",
																				DBG_MAX_SPAWN_ROWS + 2, FALSE, TRUE, 10 );

	// reset() before create(), per the lifetime rule in CodeGuiSearchField.h: winDestroy only QUEUES a
	// window, so reopening this screen in the frame it closed would otherwise still see a live-looking
	// entry pointer here and skip construction entirely.
	s_spawnSearch.reset();
	s_spawnSearch.create( s_listSpawnTemplate, spawnSearchChanged,
												"DebugMenu:EntrySpawnSearch", "DebugMenu:ButtonSpawnSearchClear" );

	// Put the search text back, like the armed pick and the
	// active tab. reset() above wipes m_filter, so without this the box comes back empty and the
	// list unfiltered every time the screen is reopened - the same complaint as the other two.
	//
	// setFilter rather than writing the gadget text: m_filter is what repopulateSpawnList reads,
	// and the two have to move together or the box shows a search the list is not applying.
	// It fires no callback, so nothing is ACTED on here - repopulateSpawnList below is what
	// re-applies it, on the normal path, once the catalog exists.
	if( !s_spawnSearchSaved.isEmpty() )
		s_spawnSearch.setFilter( s_spawnSearchSaved );

	// Picked line and the escape hatch for the sanity filter, on one row.
	y = CodeGuiScaleY( 238 );
	s_spawnPickLabel = CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 4 ), CodeGuiScaleX( 456 ), lineH,
																	 nullptr, L"", FALSE, 12 );
	s_buttonSpawnScope = CodeGuiButton( pane, pad + CodeGuiScaleX( 464 ), y,
																			CodeGuiScaleX( 168 ), btnH,
																			"DebugMenu:ButtonSpawnScope", L"",
																			L"Spawnable hides templates with no model, and the engine's system objects. "
																			L"All shows every Object in the INI." );

	// Count and veterancy.
	y = CodeGuiScaleY( 272 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 4 ), CodeGuiScaleX( 52 ), lineH,
								nullptr, L"Count", FALSE, 12 );
	s_buttonSpawnCountDown = CodeGuiButton( pane, pad + CodeGuiScaleX( 56 ), y,
																					CodeGuiScaleX( 34 ), btnH,
																					"DebugMenu:ButtonSpawnCountDown", L"-",
																					L"Fewer" );
	s_spawnCountValue = CodeGuiLabel( pane, pad + CodeGuiScaleX( 94 ), y + CodeGuiScaleY( 4 ),
																		CodeGuiScaleX( 44 ), lineH, nullptr, L"1", TRUE, 12 );
	s_buttonSpawnCountUp = CodeGuiButton( pane, pad + CodeGuiScaleX( 142 ), y,
																				CodeGuiScaleX( 34 ), btnH,
																				"DebugMenu:ButtonSpawnCountUp", L"+",
																				L"More, up to 50 - the ceiling the order handler enforces" );

	CodeGuiLabel( pane, pad + CodeGuiScaleX( 200 ), y + CodeGuiScaleY( 4 ), CodeGuiScaleX( 76 ), lineH,
								nullptr, L"Veterancy", FALSE, 12 );
	s_buttonSpawnVetPrev = CodeGuiButton( pane, pad + CodeGuiScaleX( 280 ), y,
																				CodeGuiScaleX( 34 ), btnH,
																				"DebugMenu:ButtonSpawnVetPrev", L"<",
																				L"Previous rank" );
	s_spawnVetValue = CodeGuiLabel( pane, pad + CodeGuiScaleX( 318 ), y + CodeGuiScaleY( 4 ),
																	CodeGuiScaleX( 96 ), lineH, nullptr, L"", TRUE, 12 );
	s_buttonSpawnVetNext = CodeGuiButton( pane, pad + CodeGuiScaleX( 418 ), y,
																				CodeGuiScaleX( 34 ), btnH,
																				"DebugMenu:ButtonSpawnVetNext", L">",
																				L"Next rank" );

	// The three delete buttons exist so the wire's two sentinels - "every type" and "all of them" -
	// are named actions instead of magic zeroes the user has to know to type.
	y = CodeGuiScaleY( 306 );
	s_buttonSpawn = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 96 ), btnH,
																 "DebugMenu:ButtonSpawnUnit", L"Spawn",
																 L"Create Count of the picked template on the target's team" );
	s_buttonSpawnDelete = CodeGuiButton( pane, pad + CodeGuiScaleX( 104 ), y,
																			 CodeGuiScaleX( 96 ), btnH,
																			 "DebugMenu:ButtonSpawnDelete", L"Delete",
																			 L"Remove Count of the picked template - the oldest ones - from the target. "
																			 L"No explosion, no wreckage, no score: they just stop existing" );
	s_buttonSpawnDeleteType = CodeGuiButton( pane, pad + CodeGuiScaleX( 208 ), y,
																					 CodeGuiScaleX( 160 ), btnH,
																					 "DebugMenu:ButtonSpawnDeleteType", L"Delete all of type",
																					 L"Remove EVERY one of the picked template the target owns, ignoring Count" );
	s_buttonSpawnDeleteAll = CodeGuiButton( pane, pad + CodeGuiScaleX( 376 ), y,
																					CodeGuiScaleX( 160 ), btnH,
																					"DebugMenu:ButtonSpawnDeleteAll", L"Delete everything",
																					L"Remove EVERY object the target owns, units and buildings, of every type" );

	// The line the old text box never had: nothing on screen said where the units come out.
	//
	// The 36-unit gap below the button row rather than the 34 the Cheats pane uses is deliberate.
	// CodeGuiScaleY shrinks the gaps on a short backbuffer but CodeGuiAtLeast floors btnH at 26
	// device pixels, so the rows close up faster than the buttons do; the extra couple of units buys
	// back most of that. The last control still ends at 384 of the 390 available.
	y = CodeGuiScaleY( 342 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Units appear at YOUR camera centre, spread into a ring - scroll the view to aim. "
								L"Delete ignores it.", FALSE, 12 );

	y = CodeGuiScaleY( 364 );
	s_spawnResult = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );

	// Initial label state. The scope button's text is only ever written here and in its own handler,
	// so it needs no per-tick refresh and no "what does it currently say" shadow variable.
	if( s_buttonSpawnScope )
		GadgetButtonSetText( s_buttonSpawnScope, s_spawnShowAll
			? UnicodeString( L"Show: all" ) : UnicodeString( L"Show: spawnable" ) );
	refreshSpawnCountLabel();
	refreshSpawnVetLabel();
	refreshSpawnPickLabel();
}

//-------------------------------------------------------------------------------------------------
static void buildCameraTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int rowH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_ROW_H ), 20 );
	const Int labelW	= CodeGuiScaleX( 200 );
	const Int sliderW	= CodeGuiScaleX( 260 );
	const Int valueW	= CodeGuiScaleX( 70 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );

	Int y = 0;
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		CodeGuiLabel( pane, pad, y, labelW, lineH, nullptr, s_sliderSpecs[i].label, FALSE, 12 );

		s_slider[i] = CodeGuiHSlider( pane, pad + labelW, y, sliderW, lineH,
																	s_sliderSpecs[i].idName,
																	s_sliderSpecs[i].minVal, s_sliderSpecs[i].maxVal );

		s_sliderValue[i] = CodeGuiLabel( pane, pad + labelW + sliderW + CodeGuiScaleX( 12 ), y,
																		 valueW, lineH, nullptr, L"0", FALSE, 12 );
		y += rowH;
	}

	y += CodeGuiScaleY( 12 );
	s_buttonCamDefaults = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 130 ), btnH,
																			 "DebugMenu:ButtonCameraDefaults", L"Defaults",
																			 L"Restore the shipped values" );
	s_buttonCamApply = CodeGuiButton( pane, pad + CodeGuiScaleX( 140 ), y, CodeGuiScaleX( 130 ), btnH,
																		"DebugMenu:ButtonCameraApply", L"Apply",
																		L"Apply now and write to Options.ini" );
	s_buttonUncapPitch = CodeGuiButton( pane, pad + CodeGuiScaleX( 280 ), y, CodeGuiScaleX( 150 ), btnH,
																			"DebugMenu:ButtonUncapPitch", L"Uncap pitch: off",
																			L"Camera pitch 5-90 instead of 20-60 (not with another human in the match)" );

	y += CodeGuiScaleY( 36 );
	s_cameraNote = CodeGuiLabel( pane, pad, y, paneW - pad * 2, lineH, nullptr,
															 L"Scroll speed and draw distance are percentages.", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
// In a multiplayer match the camera heights stay within the wire's range - the
// host's pair, or this machine's clamped to it (GXHostCamera::effectiveLimits) - so a Debug pair past the player's range
// is saved but cannot apply there. Say so on the tab instead of letting Apply look like it did.
// The Debug range also applies to the one human in a multiplayer match (GXHostCamera::debugRangeAllowed),
// and in a game the note says from which Max camera height this map is whole on screen.
static void refreshCameraNote( void )
{
	if( s_cameraNote == nullptr )
		return;
	if( !GXHostCamera::debugRangeAllowed() )
	{
		GadgetStaticTextSetText( s_cameraNote,
			UnicodeString( L"With another human: 100-1000 / 50-300 or host's; pitch 20-60." ) );	// one line at 800x600
		return;
	}
	const Real whole = ( TheTacticalView != nullptr && TheGameLogic != nullptr && TheGameLogic->isInGame()
		&& !TheGameLogic->isInShellGame() ) ? TheTacticalView->getWholeMapCameraHeightSetting() : 0.0f;
	UnicodeString txt;
	if( whole > 0.0f )
		txt.format( L"Whole map from Max camera height %d on this map.", ( (Int)whole / 100 + 1 ) * 100 );
	else
		txt = L"Scroll speed and draw distance are percentages.";
	GadgetStaticTextSetText( s_cameraNote, txt );
}

//-------------------------------------------------------------------------------------------------
static void buildConfigTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( 26 ), 24 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Resolved (read-only): loaded archives and what shadows what", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listResolved = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 140 ),
																	 "DebugMenu:ListResolved", DBG_MAX_REPORT_ROWS, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 146 );
	s_buttonCfgRefresh = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 130 ), btnH,
																			"DebugMenu:ButtonConfigRefresh", L"Rescan",
																			L"Walk the archives again" );

	y += CodeGuiScaleY( 34 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"User preferences (editable): pick a row, edit, Set, then Save", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listPrefs = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 100 ),
																"DebugMenu:ListPrefs", 256, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 106 );
	s_entryPrefValue = CodeGuiTextEntry( pane, pad, y, CodeGuiScaleX( 300 ), fieldH,
																			 "DebugMenu:EntryPrefValue", 128 );
	s_buttonPrefSet = CodeGuiButton( pane, pad + CodeGuiScaleX( 310 ), y - CodeGuiScaleY( 2 ),
																	 CodeGuiScaleX( 100 ), btnH,
																	 "DebugMenu:ButtonPrefSet", L"Set",
																	 L"Write the value into the live preference map" );
	s_buttonPrefSave = CodeGuiButton( pane, pad + CodeGuiScaleX( 420 ), y - CodeGuiScaleY( 2 ),
																		CodeGuiScaleX( 100 ), btnH,
																		"DebugMenu:ButtonPrefSave", L"Save",
																		L"Write Options.ini to disk" );
}

//-------------------------------------------------------------------------------------------------
static void buildSavesTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"Save games", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listSaves = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 230 ),
																"DebugMenu:ListSaves", 128, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 238 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 2 ), CodeGuiScaleX( 100 ), lineH,
								nullptr, L"Description", FALSE, 12 );
	s_entrySaveDesc = CodeGuiTextEntry( pane, pad + CodeGuiScaleX( 110 ), y, CodeGuiScaleX( 320 ), fieldH,
																			"DebugMenu:EntrySaveDesc", 64 );

	y += CodeGuiScaleY( 32 );
	s_buttonSaveRefresh = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 110 ), btnH,
																			 "DebugMenu:ButtonSaveRefresh", L"Refresh",
																			 L"Re-read the save directory" );
	s_buttonSaveLoad = CodeGuiButton( pane, pad + CodeGuiScaleX( 120 ), y, CodeGuiScaleX( 110 ), btnH,
																		"DebugMenu:ButtonSaveLoad", L"Load",
																		L"Load the selected save" );
	s_buttonSaveHere = CodeGuiButton( pane, pad + CodeGuiScaleX( 240 ), y, CodeGuiScaleX( 140 ), btnH,
																		"DebugMenu:ButtonSaveHere", L"Save here",
																		L"Save the running game at this exact point" );
}

//-------------------------------------------------------------------------------------------------
static Bool buildDebugScreen( void )
{
	const Int px = CodeGuiScaleX( DBG_PANEL_X );
	const Int py = CodeGuiScaleY( DBG_PANEL_Y );
	const Int pw = CodeGuiScaleX( DBG_PANEL_W );
	const Int ph = CodeGuiScaleY( DBG_PANEL_H );

	// WIN_STATUS_ABOVE: last draw pass and first hit-test pass, so the overlay sits over the Options
	// sheet in the shell AND over the control bar in a running match.
	s_root = CodeGuiPanel( nullptr, px, py, pw, ph,
												 DebugMenuSystem, "DebugMenu:Root", WIN_STATUS_ABOVE );
	if( s_root == nullptr )
		return FALSE;

	s_root->winSetInputFunc( DebugMenuInput );		// ESC
	s_rootDraw = s_root->winGetDrawFunc();
	s_root->winSetDrawFunc( debugMenuRootDraw );

	// X and Y are scaled independently, so the two paddings are not interchangeable: on a 16:9
	// backbuffer CodeGuiScaleX(12) and CodeGuiScaleY(12) differ by roughly a factor of two, and
	// mixing them is what makes the footer collide with the content pane.
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int padY		= CodeGuiScaleY( DBG_PAD );
	const Int titleH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_TITLE_H ), 18 );
	const Int tabH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_TAB_H ), 24 );
	const Int tabGap	= CodeGuiScaleX( DBG_TAB_GAP );
	const Int footerH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_FOOTER_BTN_H ), 30 );
	const Int innerW	= pw - pad * 2;

	// All child coordinates are PARENT-relative; winCreate stores m_region relative to the parent.
	CodeGuiLabel( s_root, pad, padY, innerW, titleH, "DebugMenu:Title", L"DEBUG", TRUE, 16 );

	const Int tabY = CodeGuiScaleY( DBG_TAB_Y );
	const Int tabW = ( innerW - tabGap * ( DBG_TAB_COUNT - 1 ) ) / DBG_TAB_COUNT;
	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		AsciiString id;
		id.format( "DebugMenu:Tab%d", i );
		s_tabButton[i] = CodeGuiButton( s_root, pad + i * ( tabW + tabGap ), tabY, tabW, tabH,
																		id.str(), s_tabLabels[i] );
	}

	const Int contentY = CodeGuiScaleY( DBG_CONTENT_Y );
	const Int contentH = ph - contentY - footerH - padY * 2;

	// One SEE_THRU container per tab. SEE_THRU suppresses only its own fill, children still draw, and
	// hiding the container hides and un-hit-tests the whole pane in one call. Each pane carries
	// DebugMenuSystem so its children's GBM_/GLM_/GSM_ messages land in the same handler as the
	// root's - gogoGadget* sets owner == the parent you pass, not the screen root.
	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		AsciiString id;
		id.format( "DebugMenu:Pane%d", i );
		s_tabPanel[i] = CodeGuiPanel( s_root, pad, contentY, innerW, contentH,
																	DebugMenuSystem, id.str(), WIN_STATUS_SEE_THRU );
		if( s_tabPanel[i] == nullptr )
			return FALSE;
	}

	buildStatsTab  ( s_tabPanel[ DBG_TAB_STATS   ], innerW );
	buildPlayersTab( s_tabPanel[ DBG_TAB_PLAYERS ], innerW );
	buildCheatsTab ( s_tabPanel[ DBG_TAB_CHEATS  ], innerW );
	buildSpawnTab  ( s_tabPanel[ DBG_TAB_SPAWN   ], innerW );
	buildCameraTab ( s_tabPanel[ DBG_TAB_CAMERA  ], innerW );
	buildConfigTab ( s_tabPanel[ DBG_TAB_CONFIG  ], innerW );
	buildSavesTab  ( s_tabPanel[ DBG_TAB_SAVES   ], innerW );

	// A sensible default so "Give cash" does something useful on the first tap, and so an empty field
	// is a deliberate act rather than the initial state. cheatCashAmount() falls back to the same
	// number if it is cleared.
	if( s_entryCashAmount )
		GadgetTextEntrySetText( s_entryCashAmount, UnicodeString( L"10000" ) );

	s_buttonBack = CodeGuiButton( s_root,
																pw - pad - CodeGuiScaleX( DBG_FOOTER_BTN_W ),
																ph - padY - footerH,
																CodeGuiScaleX( DBG_FOOTER_BTN_W ), footerH,
																"DebugMenu:ButtonBack", L"Back", L"Close the debug screen" );

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// PUBLIC API /////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
void DebugMenuOpen( void )
{
	if( TheWindowManager == nullptr )
		return;

	if( s_layout != nullptr )		// already open - just raise it
	{
		s_layout->hide( FALSE );
		s_layout->bringForward();
		TheWindowManager->winSetFocus( s_root );
		return;
	}

	if( s_pref == nullptr )
		s_pref = NEW OptionPreferences;

	if( buildDebugScreen() == FALSE )
	{
		if( s_root )
			TheWindowManager->winDestroy( s_root );
		nullifyControls();
		return;
	}

	s_layout = CodeGuiMakeLayout( s_root );
	if( s_layout == nullptr )
	{
		TheWindowManager->winDestroy( s_root );
		nullifyControls();
		return;
	}

	loadSlidersFromPrefs();
	// Re-open on the tab you were last on, not always on Stats.
	//
	// s_activeTab is file-scope and already survives the screen being torn down - the state was
	// there all along, and this one line threw it away on every open. It starts at DBG_TAB_STATS,
	// so a fresh session is unchanged.
	//
	// Safe as a RESTORE, which is the rule that matters here: selectTab only shows and hides panes,
	// enables the tab buttons, and lazily populates a list the pane is about to display. It applies
	// nothing, spawns nothing and saves nothing, so re-entering on Cheats or Spawn cannot take an
	// action on the user's behalf. Its range check also makes a bad value harmless.
	selectTab( s_activeTab );
	DebugMenuUpdate();

	s_layout->hide( FALSE );
	s_layout->bringForward();

	// The line the old Extras screen never had. Without it winProcessKey has no m_keyboardFocus and
	// ESC can never reach DebugMenuInput. The root has no WIN_STATUS_NO_FOCUS and DebugMenuSystem
	// answers GWM_INPUT_FOCUS, so this actually sticks.
	TheWindowManager->winSetFocus( s_root );
}

//-------------------------------------------------------------------------------------------------
void DebugMenuClose( void )
{
	if( s_layout == nullptr )
		return;

	// Deferred winDestroy all the way down, so this is safe from inside one of our own handlers.
	CodeGuiDestroyLayout( s_layout );
	nullifyControls();

	if( s_pref )
	{
		delete s_pref;
		s_pref = nullptr;
	}

	// Hand the keyboard back. Nothing else will: winSetFocus leaves m_keyboardFocus null once the
	// focused window is destroyed. A null result here is fine, it just clears focus.
	if( TheWindowManager )
	{
		GameWindow *opts = TheWindowManager->winGetWindowFromId(
			nullptr, (Int)TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:OptionsMenuParent" ) );
		TheWindowManager->winSetFocus( opts );
	}
}

//-------------------------------------------------------------------------------------------------
Bool DebugMenuIsOpen( void )
{
	return s_layout != nullptr;
}

//-------------------------------------------------------------------------------------------------
void DebugMenuToggle( void )
{
	if( DebugMenuIsOpen() )
		DebugMenuClose();
	else
		DebugMenuOpen();
}

//-------------------------------------------------------------------------------------------------
// CALLBACKS //////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuSystem( GameWindow *window, UnsignedInt msg,
																						 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		// ------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			// GameWindowManager::reset() runs winDestroyAll() without calling DebugMenuClose, which would leave
			// s_layout and ~30 control pointers dangling. Full teardown inside a destroy is safe: winDestroy returns early.
			if( window == s_root )
			{
				nullifyControls();
				CodeGuiDestroyLayout( s_layout );		// idempotent here; also nulls s_layout
				if( s_pref )
				{
					delete s_pref;
					s_pref = nullptr;
				}
			}
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// Mandatory. winSetFocus walks up asking each window whether it wants focus and resets
			// m_keyboardFocus to null if nobody says yes.
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;

			// FIRST, before anything compares this control against one of ours. The search field's
			// clear button is code-created and belongs to the field, not to this screen; offering the
			// message to the field up here is what keeps it out of the comparisons below. Returns TRUE
			// only when it was in fact that button.
			if( s_spawnSearch.handleSystemMsg( msg, control ) )
				return MSG_HANDLED;

			for( Int i = 0; i < DBG_TAB_COUNT; ++i )
			{
				if( control == s_tabButton[i] )
				{
					selectTab( i );
					return MSG_HANDLED;
				}
			}

			if( control == s_buttonBack )
			{
				DebugMenuClose();
				return MSG_HANDLED;
			}
			// All of these append to TheMessageStream and return. None of them touch logic state directly, which is why none of them is gated to single-player. The one exception is the radar force, which is pure presentation on this machine and is therefore gated on debugClientToolsAllowed() and on debugOthersRefusal() (toggleForceRadar).
			if( control == s_buttonGiveCash )
			{
				gxGiveMoney();
				return MSG_HANDLED;
			}
			if( control == s_buttonSetCash )
			{
				gxSetMoney();
				return MSG_HANDLED;
			}
			if( control == s_buttonReveal )
			{
				gxRevealMap( TRUE );
				return MSG_HANDLED;
			}
			if( control == s_buttonUnreveal )
			{
				gxRevealMap( FALSE );
				return MSG_HANDLED;
			}
			if( control == s_buttonForceRadar )
			{
				toggleForceRadar();
				return MSG_HANDLED;
			}
			if( control == s_buttonKillPlayer )
			{
				gxKillPlayer( TRUE );			// hand the assets to a living mutual ally, like surrender
				return MSG_HANDLED;
			}
			if( control == s_buttonKillUnits )
			{
				gxKillObjects( FALSE );
				return MSG_HANDLED;
			}
			if( control == s_buttonKillAll )
			{
				gxKillObjects( TRUE );
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawn )
			{
				gxSpawnUnit();
				return MSG_HANDLED;
			}
			// The three deletes differ only in which of the handler's two sentinels they use.
			if( control == s_buttonSpawnDelete )
			{
				gxDeleteObjects( FALSE, FALSE );		// picked type, Count of them
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawnDeleteType )
			{
				gxDeleteObjects( FALSE, TRUE );			// picked type, all of them
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawnDeleteAll )
			{
				gxDeleteObjects( TRUE, TRUE );			// every type, all of them
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawnScope )
			{
				s_spawnShowAll = !s_spawnShowAll;
				GadgetButtonSetText( control, s_spawnShowAll
					? UnicodeString( L"Show: all" ) : UnicodeString( L"Show: spawnable" ) );
				repopulateSpawnList();
				return MSG_HANDLED;
			}
			// The count ladder is clamped at both ends rather than wrapping: "+" past the top must not
			// silently drop the user back to 1 when the number they wanted was 50.
			if( control == s_buttonSpawnCountDown )
			{
				if( s_spawnCountIdx > 0 )
					--s_spawnCountIdx;
				refreshSpawnCountLabel();
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawnCountUp )
			{
				if( s_spawnCountIdx < DBG_NUM_SPAWN_COUNTS - 1 )
					++s_spawnCountIdx;
				refreshSpawnCountLabel();
				return MSG_HANDLED;
			}
			// Veterancy WRAPS, unlike the count: it is a four-item cycle with no natural extreme, and
			// two buttons that go dead at the ends of a four-item list read as broken.
			if( control == s_buttonSpawnVetPrev )
			{
				s_spawnVet = ( s_spawnVet <= (Int)LEVEL_FIRST ) ? (Int)LEVEL_LAST : s_spawnVet - 1;
				refreshSpawnVetLabel();
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawnVetNext )
			{
				s_spawnVet = ( s_spawnVet >= (Int)LEVEL_LAST ) ? (Int)LEVEL_FIRST : s_spawnVet + 1;
				refreshSpawnVetLabel();
				return MSG_HANDLED;
			}
			// The four frame pacing steps call the same two CommandXlat functions the MSG_META_* key
			// handlers do rather than poking TheFramePacer, so button and key cannot drift apart: the
			// enable-flag toggle, the render-cap ceiling and the network refusal stay in one place.
			// The readout is refreshed here rather than waiting for the next 250ms tick, so a tap
			// reads as having done something even when the step lands on the same preset.
			if( control == s_buttonRenderFpsDown )
			{
				changeMaxRenderFps( FpsValueChange_Decrease );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonRenderFpsUp )
			{
				changeMaxRenderFps( FpsValueChange_Increase );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonLogicFpsDown )
			{
				changeLogicTimeScale( FpsValueChange_Decrease );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonLogicFpsUp )
			{
				changeLogicTimeScale( FpsValueChange_Increase );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonFastForwardDown )
			{
				changeFastForward( FpsValueChange_Decrease );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonFastForwardUp )
			{
				changeFastForward( FpsValueChange_Increase );
				refreshFpsReadout();
				return MSG_HANDLED;
			}
			if( control == s_buttonCamDefaults )
			{
				setSliderDefaults();
				return MSG_HANDLED;
			}
			if( control == s_buttonCamApply )
			{
				applySlidersToPrefs();
				refreshPrefList();
				return MSG_HANDLED;
			}
			if( control == s_buttonUncapPitch )
			{
				s_uncapPitch = !s_uncapPitch;
				if( setPitchSliderRange( s_uncapPitch ) )
					s_pitchEdit = PITCH_MOVED;	// pinned into 20-60: Apply writes what the slider now shows
				refreshUncapPitchLabel();
				refreshSliderValueLabel( 2 );
				return MSG_HANDLED;
			}
			if( control == s_buttonCfgRefresh )
			{
				refreshResolvedConfig();
				refreshPrefList();
				return MSG_HANDLED;
			}
			if( control == s_buttonPrefSet )
			{
				applyPrefEdit();
				return MSG_HANDLED;
			}
			if( control == s_buttonPrefSave )
			{
				if( s_pref )
					s_pref->write();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveRefresh )
			{
				refreshSaveList();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveLoad )
			{
				doLoadSelectedSave();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveHere )
			{
				doSaveHere();
				return MSG_HANDLED;
			}
			break;
		}

		// ------------------------------------------------------------------------------------------
		case GSM_SLIDER_TRACK:
		case GSM_SLIDER_DONE:
		{
			GameWindow *control = (GameWindow *)mData1;
			for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
			{
				if( control == s_slider[i] )
				{
					refreshSliderValueLabel( i );
					if( i == 2 )
						s_pitchEdit = PITCH_MOVED;	// a move the player made, see applySlidersToPrefs
					return MSG_HANDLED;
				}
			}
			break;
		}

		// ------------------------------------------------------------------------------------------
		case GLM_SELECTED:
		case GLM_DOUBLE_CLICKED:
		{
			GameWindow *control = (GameWindow *)mData1;
			if( control == s_listPrefs )
			{
				onPrefRowSelected();
				return MSG_HANDLED;
			}
			if( control == s_listCheatTarget )
			{
				// Update the readout on the tap rather than up to 250ms later, so picking a row reads as
				// having done something. The tally is whatever the last tick measured, which is exactly
				// what the row would have shown anyway.
				refreshCheatTargetInfo();
				return MSG_HANDLED;
			}
			if( control == s_listSpawnTemplate )
			{
				onSpawnRowSelected();
				return MSG_HANDLED;
			}
			if( control == s_listSaves && msg == GLM_DOUBLE_CLICKED )
			{
				doLoadSelectedSave();
				return MSG_HANDLED;
			}
			break;
		}

		// ------------------------------------------------------------------------------------------
		// GadgetTextEntryInput posts these to the entry's OWNER - which gogoGadgetTextEntry set to the
		// pane we passed as parent, so they arrive here - on every accepted character and on Return.
		// This is the only route the search field has to notice typing. Anything that is not its entry
		// falls through to MSG_IGNORED, which is what the cash, pref and save-description fields want:
		// they are read at press time, not tracked.
		case GEM_UPDATE_TEXT:
		case GEM_EDIT_DONE:
		{
			if( s_spawnSearch.handleSystemMsg( msg, (GameWindow *)mData1 ) )
				return MSG_HANDLED;
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuInput( GameWindow *window, UnsignedInt msg,
																						WindowMsgData mData1, WindowMsgData mData2 )
{
	if( msg == GWM_CHAR )
	{
		const UnsignedByte key		= (UnsignedByte)mData1;
		const UnsignedByte state	= (UnsignedByte)mData2;

		if( key == KEY_ESC )
		{
			if( BitIsSet( state, KEY_STATE_UP ) )
				DebugMenuClose();
			return MSG_HANDLED;
		}

		// Swallow every other key too. The overlay owns the keyboard while it is up, and the
		// in-game hotkeys underneath would otherwise fire from typing into the spawn field.
		return MSG_HANDLED;
	}

	// Consume mouse input: WIN_STATUS_ABOVE only wins the hit-test, so MSG_IGNORED let taps reach the world.
	// GWM_MOUSE_POS must be claimed too, or the cursor changes shape over units hidden behind the panel.
	switch( msg )
	{
		case GWM_LEFT_DOWN:
		case GWM_LEFT_UP:
		case GWM_LEFT_DRAG:
		case GWM_LEFT_DOUBLE_CLICK:
		case GWM_MIDDLE_DOWN:
		case GWM_MIDDLE_UP:
		case GWM_MIDDLE_DRAG:
		case GWM_MIDDLE_DOUBLE_CLICK:
		case GWM_RIGHT_DOWN:
		case GWM_RIGHT_UP:
		case GWM_RIGHT_DRAG:
		case GWM_RIGHT_DOUBLE_CLICK:
		case GWM_MOUSE_POS:
		case GWM_MOUSE_ENTERING:
		case GWM_MOUSE_LEAVING:
		case GWM_WHEEL_UP:
		case GWM_WHEEL_DOWN:
			return MSG_HANDLED;

		default:
			break;
	}

	return MSG_IGNORED;
}
