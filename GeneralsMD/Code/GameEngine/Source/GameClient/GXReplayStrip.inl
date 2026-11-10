/*
**	GeneralsX: the replay strip: time reached / whole length, click ahead to race there, pause,
**	speeds 0.5x-8x. Included ONCE, after GXSpectatorHud.inl at the end of InGameUI.cpp, for the same reason that file is
**	(a new .cpp would move the SimID digest) and so that it shares the spectator HUD's palette and text helpers without
**	editing that file. Nothing here is read by the simulation.
**
**	LOOK: the C2 spectator bar's navy glass, steel-blue frame, one blue accent, square corners, no CodeGui. It draws in a
**	spectator-HUD match and in a legacy one alike (it is not gated on SpectatorUI); in the legacy layout it stands above
**	the retail control bar.
**
**	HOW A SEEK WORKS (Reforged's InGameUI::updateReplaySeek, redone): update() runs once per client pass, between logic
**	frames. While the frame is short of the target it holds m_TiVOFastMode on - the retail fast mode: uncapped logic - and on
**	reaching the target it turns it off and pauses, so the match stands on exactly that frame. Since 07/10 a race draws only
**	a progress frame every 250 ms of wall time (GXReplayStrip::passRenders, W3DDisplay::draw), runs with fast forward off
**	and sound effects muted, and lands at the speed it began at (holdRace / leaveRace). A click BEHIND the playhead cannot go back without a checkpoint (replay step 3, planned): it takes
**	the quit menu's own Restart for a replay and races forward again; the strip says so while it does.
**
**	SCRIPTED DRIVER (rendered replays only - the headless simulation never runs GameClient::update): GX_REPLAY_SEEK=<frame>[,<frame>...]
**	seeks to each in turn (a smaller frame restarts the replay) and logs "[GXSEEK] reached frame=<f> want=<w>". With
**	GX_REPLAY_SEEK_EXIT=1 the engine quits GX_REPLAY_SEEK_HOLD seconds after the last one (the strip stays up, paused, for a picture
**	taken from outside). Not GX_TEST_QUIT_AT: its queued clear-game command cannot run once the seek has paused the game.
**	GX_REPLAY_STRIP_INPUT (inputDriver below) drives the MOUSE path instead: clicks and drags through the translator.
*/

#include "GameClient/GXReplayStrip.h"
#include "Common/Recorder.h"
#include "Common/GameEngine.h"
#include "Common/GameAudio.h"
#include "Common/AudioAffect.h"

extern Bool gxRestartReplay();	// QuitMenu.cpp

namespace GXSpecHud
{
namespace Strip
{
using namespace GXReplayStrip;

enum { K_PAUSE, K_SPEED, K_BAR, K_DRAG };
struct SHit { Rect r; Int kind; Int id; };

static SHit s_hits[16];
static Int s_hitCount = 0;
static Bool s_live = FALSE;					///< drawn this pass and not under the quit menu: the translator may act
static SHit s_press = { { 0, 0, 0, 0 }, -1, 0 };
static Bool s_pressed = FALSE;				///< a press that began on one of our controls
static UnsignedInt s_target = 0;			///< the frame being raced to; 0 = no race
static Bool s_viaRestart = FALSE;			///< the race in progress began with a restart (said on the strip)
static Bool s_halved = FALSE;				///< 0.5x is ours: the logic pace was cut to half
static Bool s_savedEnabled = FALSE;
static Int s_savedFps = 0;
static Int s_halfFps = 0;				///< the pace 0.5x set
static GameWindow *s_retailBar = nullptr;
static Rect s_strip = { 0, 0, 0, 0 };
static Rect s_bar = { 0, 0, 0, 0 };
static Dock s_dock;						///< panel 0 = the strip: the spectator panels' own drag state (threshold, press, move, release)
static UnsignedInt s_lastFrame = 0;
static RaceSpeed s_raceSpeed;				///< a race's speed to land at; fast forward is off while it runs (holdRace)
static Bool s_savedSfx = TRUE;				///< sound effects as they were before a race muted them
static UnsignedInt s_raceStartMs = 0, s_raceStartFrame = 0;	///< for the [GXSEEK] landed line
static SeekFlight s_flight;				///< a restart in flight: the frame to race to once the new playback is up, and whether it is awaited
static LostRelease s_lostRelease;			///< debounce of the lost-release recovery in draw()

// The strip keeps its own scale (the HUD's draw() sets the same fields, but a legacy-layout replay never runs it).
static void ensureScale()
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

static Bool playbackUp()
{
	return TheRecorder != nullptr && TheRecorder->isPlaybackMode() && TheGameLogic != nullptr && TheGameLogic->isInGame()
		&& !TheGameLogic->isInShellGame() && TheGameLogic->isInReplayGame();
}

static UnsignedInt length() { return TheRecorder != nullptr ? TheRecorder->getPlaybackFrameCount() : 0; }

static void stopRace()
{
	if (TheWritableGlobalData != nullptr)
		TheWritableGlobalData->m_TiVOFastMode = FALSE;
	s_target = 0;
	s_viaRestart = FALSE;
}

static void setPaused( Bool paused )
{
	if (TheGameLogic != nullptr && TheGameLogic->isGamePaused() != paused)
		TheGameLogic->setGamePaused( paused, FALSE, FALSE );	// music and input untouched: the strip itself must stay clickable
}

/// Cuts the logic pace to half (a replay only: it is local playback, nothing else shares the pace). The state it found is
/// kept so the next speed puts it back.
static void halve()
{
	if (TheFramePacer == nullptr)
		return;
	if (!s_halved)
	{
		s_savedEnabled = TheFramePacer->isLogicTimeScaleEnabled();
		s_savedFps = TheFramePacer->getLogicTimeScaleFps();
	}
	// Half of the pace the replay plays at: its recorded game speed when the scale is on (GameLogic::onNewGame restores
	// it), else the tick rate.
	s_halfFps = max( 1, (s_savedEnabled ? s_savedFps : (Int)LOGICFRAMES_PER_SECOND) / 2 );
	TheFramePacer->setFastForward( 1 );
	TheFramePacer->setLogicTimeScaleFps( s_halfFps );
	TheFramePacer->enableLogicTimeScale( TRUE );
	s_halved = TRUE;
}

static void unhalve()
{
	if (!s_halved || TheFramePacer == nullptr)
		return;
	s_halved = FALSE;
	if (s_savedEnabled)
		TheFramePacer->setLogicTimeScaleFps( s_savedFps );
	else
		TheFramePacer->enableLogicTimeScale( FALSE );
}

static Bool isHalved()
{
	return s_halved && TheFramePacer != nullptr && TheFramePacer->isLogicTimeScaleEnabled()
		&& TheFramePacer->getLogicTimeScaleFps() == s_halfFps;
}

static Int currentSpeed()
{
	if (s_raceSpeed.on)
		return s_raceSpeed.shown();	// the speed it will land at, not the suspended one
	return speedFromState( TheFramePacer != nullptr ? TheFramePacer->getFastForward() : 1, isHalved() );
}

// GeneralsX: A SEEK RACE RUNS WITH FAST FORWARD OFF AND SOUND EFFECTS MUTED, and puts both back
// where they were when it lands. Fast forward capped the race at k x the tick rate (the fast mode is uncapped) and its
// every-k-th-pass draws never met the race's own frames, so a 2x-10x seek drew nothing until it landed (wh1ter0se 07/10: "it
// froze when i did like 4x and dragged"); W3DDisplay::draw now draws a race by GXReplayStrip::passRenders alone. Sound
// effects: an uncapped race plays every shot of minutes of battle; switched off they are what a player with sound
// effects off gets, which holds lockstep (AudioManager::addAudioEvent - no simulation state reads a handle). Speech and
// music stay. Re-applied every pass of the race: a seek behind restarts the replay, and the new game resets fast forward.
static void holdRace()
{
	if (TheFramePacer != nullptr && TheFramePacer->getFastForward() != 1)
	{
		// Set since the last pass: by a speed key (CommandXlat changeFastForward) - the speed to land at - or by a restart's
		// new game, which only ever sets 1x and so never gets here.
		s_raceSpeed.ff = TheFramePacer->getFastForward();
		s_raceSpeed.halved = false;
		TheFramePacer->setFastForward( 1 );
	}
	if (TheAudio != nullptr && TheAudio->isOn( AudioAffect_Sound ))
		TheAudio->setOn( FALSE, AudioAffect_Sound );
}

static Int raceLandingSpeed() { return s_raceSpeed.on ? s_raceSpeed.ff : 0; }
static void raceChooseSpeed( Int mult ) { s_raceSpeed.chooseFF( mult ); }

static void enterRace()
{
	gxRaceLandingSpeed = raceLandingSpeed;	// CommandXlat's speed keys step from, and record into, the race's landing speed
	gxRaceChooseSpeed = raceChooseSpeed;
	if (!s_raceSpeed.on)
	{
		s_raceSpeed.begin( TheFramePacer != nullptr ? TheFramePacer->getFastForward() : 1, isHalved() );
		s_savedSfx = TheAudio == nullptr || TheAudio->isOn( AudioAffect_Sound );
		s_raceStartMs = timeGetTime();
		s_raceStartFrame = TheGameLogic->getFrame();
	}
	holdRace();
}

/// The race is over (landed, paused, or a plain restart is up): sound back, and - in a playback, not at the menu - the
/// speed it began at (or the one picked meanwhile). A restart's new game came up at 1x and its own pace, so put back
/// both the multiplier and a halved pace.
static void leaveRace( Bool restoreSpeed )
{
	if (!s_raceSpeed.on)
		return;
	s_raceSpeed.end();
	if (TheAudio != nullptr)
		TheAudio->setOn( s_savedSfx, AudioAffect_Sound );
	if (!restoreSpeed)
		return;
	if (s_raceSpeed.halved)
		halve();
	else
		unhalve();
	if (TheFramePacer != nullptr)
		TheFramePacer->setFastForward( s_raceSpeed.ff );
	fprintf( stderr, "[GXSEEK] landed: speed ff=%d halved=%d sfx=%d frame=%u from=%u ms=%u\n", s_raceSpeed.ff,
		(Int)s_raceSpeed.halved, (Int)(TheAudio != nullptr && TheAudio->isOn( AudioAffect_Sound )), TheGameLogic->getFrame(),
		s_raceStartFrame, timeGetTime() - s_raceStartMs );
}

static void setSpeed( Int speed )
{
	if (s_raceSpeed.on)
	{
		s_raceSpeed.choose( speed );	// mid-race: the speed it lands at
		return;
	}
	if (speed == SPEED_HALF)
		halve();
	else
	{
		unhalve();
		setFastForwardSpeed( speedMultiplier( speed ) );
	}
}

/// Starts a race to `target` from where the replay stands. Ahead races on; behind restarts first.
static void seekTo( UnsignedInt target )
{
	const UnsignedInt frame = TheGameLogic->getFrame();
	const UnsignedInt len = length();
	if (len > 1 && target > len - 1)
		target = len - 1;
	const Plan plan = planSeek( frame, target, LOGICFRAMES_PER_SECOND );
	fprintf( stderr, "[GXSEEK] seek from=%u to=%u plan=%d length=%u\n", frame, target, (Int)plan, len );
	switch (plan)
	{
		case PLAN_NONE:
			return;
		case PLAN_RACE_AHEAD:
			enterRace();
			s_target = target;
			s_viaRestart = FALSE;
			setPaused( FALSE );
			return;
		case PLAN_RESTART_AND_RACE:
			// fall through
		case PLAN_RESTART:
			enterRace();	// a plain restart too: its new game comes up at 1x, update() puts the speed back
			stopRace();
			setPaused( FALSE );
			s_flight.begin( plan == PLAN_RESTART_AND_RACE ? target : 0 );
			if (s_flight.finish( gxRestartReplay() ))
			{
				// The replay could not be started again (its file is gone): no new game is coming, so give the sound and the speed back.
				fprintf( stderr, "[GXSEEK] restart failed: no replay to play again\n" );
				leaveRace( FALSE );
			}
			return;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Between frames
// ---------------------------------------------------------------------------------------------------------------------
static void envDriver()
{
	static const char *s_spec = getenv( "GX_REPLAY_SEEK" );
	static const char *s_next = nullptr;
	if (s_spec == nullptr || *s_spec == '\0' || s_target != 0 || s_flight.pending != 0)
		return;
	if (s_next == nullptr)
		s_next = s_spec;
	if (*s_next == '\0')
	{
		// The list is done and the last race ended: with GX_REPLAY_SEEK_EXIT=1 leave after GX_REPLAY_SEEK_HOLD seconds (the
		// strip stays up and paused on the frame meanwhile, for a picture taken from outside).
		static UnsignedInt s_doneMs = 0;
		if (getenv( "GX_REPLAY_SEEK_EXIT" ) != nullptr && TheGameEngine != nullptr)
		{
			const char *hold = getenv( "GX_REPLAY_SEEK_HOLD" );
			if (s_doneMs == 0)
				s_doneMs = timeGetTime();
			if (timeGetTime() - s_doneMs >= (UnsignedInt)((hold != nullptr ? atoi( hold ) : 0) * 1000))
			{
				fprintf( stderr, "[GXSEEK] list done, leaving at frame=%u\n", TheGameLogic->getFrame() );
				TheGameEngine->setQuitting( TRUE );
			}
		}
		return;
	}
	if (TheGameLogic->getFrame() == 0)	// not before the first logic frame
		return;
	unsigned int want = 0;
	int used = 0;
	if (sscanf( s_next, "%u%n", &want, &used ) != 1)
	{
		fprintf( stderr, "[GXSEEK] GX_REPLAY_SEEK: cannot parse '%s'\n", s_next );
		s_next = "";
		return;
	}
	s_next += used;
	if (*s_next == ',')
		++s_next;
	seekTo( want );
}

static const SHit *hitOf( Int kind, Int id );

// GeneralsX: GX_REPLAY_STRIP_INPUT=<ms>:<act>[,...] - the strip's MOUSE path, scripted: raw left-button
// messages into TheMessageStream exactly as Mouse::createStreamMessages makes them, so a click goes through the translator
// (and its seek runs inside a message callback, as a real click's does - GX_REPLAY_SEEK calls seekTo between frames instead).
// <ms> counts from the first pass a replay was up and never restarts; entries fire in order, one raw message per pass.
// <act>: click@<p> (down, up), dbl@<p> (down, up, double-click, up), down@<p>, drag@<p>, up@<p> - p = per mille along the
// seek bar; pause, speed<0-4> - a click on that button; quit - leave. Coordinates are the last drawn strip's.
static void inputDriver()
{
	static const char *s_spec = getenv( "GX_REPLAY_STRIP_INPUT" );
	if (s_spec == nullptr || *s_spec == '\0' || TheMessageStream == nullptr)
		return;
	static const char *s_next = s_spec;
	static UnsignedInt s_t0 = 0;
	static Int s_queue[4];		// GameMessage types still to send for the current entry, one per pass
	static Int s_queued = 0, s_sent = 0;
	static ICoord2D s_at = { 0, 0 };
	if (s_t0 == 0)
	{
		if (!playbackUp() || s_hitCount == 0)
			return;
		s_t0 = timeGetTime();
	}
	if (s_sent == s_queued && *s_next != '\0')
	{
		unsigned int ms = 0;
		char act[16] = { 0 };
		int p = 0, used = 0;
		if (sscanf( s_next, "%u:%15[a-z0-9]%n", &ms, act, &used ) != 2)
		{
			fprintf( stderr, "[GXSEEK] GX_REPLAY_STRIP_INPUT: cannot parse '%s'\n", s_next );
			s_next = "";
			return;
		}
		if (timeGetTime() - s_t0 < ms)
			return;
		const char *rest = s_next + used;
		if (*rest == '@')
		{
			int more = 0;
			sscanf( rest, "@%d%n", &p, &more );
			rest += more;
		}
		s_next = (*rest == ',') ? rest + 1 : rest;
		s_queued = s_sent = 0;
		const Int down = GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN, up = GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP;
		s_at.x = s_bar.x + (Int)((long long)s_bar.w * clamp( 0, p, 1000 ) / 1000);
		s_at.y = s_bar.y + s_bar.h / 2;
		const SHit *button = nullptr;
		if (strcmp( act, "pause" ) == 0)
			button = hitOf( K_PAUSE, 0 );
		else if (strncmp( act, "speed", 5 ) == 0)
			button = hitOf( K_SPEED, atoi( act + 5 ) );
		if (button != nullptr)
		{
			s_at.x = button->r.x + button->r.w / 2;
			s_at.y = button->r.y + button->r.h / 2;
		}
		if (strcmp( act, "quit" ) == 0)
		{
			fprintf( stderr, "[GXSEEK] input quit at frame=%u\n", TheGameLogic->getFrame() );
			TheGameEngine->setQuitting( TRUE );
			return;
		}
		if (strcmp( act, "click" ) == 0 || button != nullptr)
		{
			s_queue[s_queued++] = down;
			s_queue[s_queued++] = up;
		}
		else if (strcmp( act, "dbl" ) == 0)
		{
			s_queue[s_queued++] = down;
			s_queue[s_queued++] = up;
			s_queue[s_queued++] = GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK;
			s_queue[s_queued++] = up;
		}
		else if (strcmp( act, "down" ) == 0)
			s_queue[s_queued++] = down;
		else if (strcmp( act, "drag" ) == 0)
			s_queue[s_queued++] = GameMessage::MSG_RAW_MOUSE_LEFT_DRAG;
		else if (strcmp( act, "up" ) == 0)
			s_queue[s_queued++] = up;
		else
			fprintf( stderr, "[GXSEEK] GX_REPLAY_STRIP_INPUT: unknown act '%s'\n", act );
	}
	if (s_sent == s_queued)
		return;
	const GameMessage::Type t = (GameMessage::Type)s_queue[s_sent++];
	GameMessage *msg = TheMessageStream->appendMessage( t );
	msg->appendPixelArgument( s_at );
	if (t == GameMessage::MSG_RAW_MOUSE_LEFT_DRAG)
	{
		const ICoord2D delta = { 1, 0 };
		msg->appendPixelArgument( delta );
		msg->appendIntegerArgument( 0 );
	}
	else
	{
		msg->appendIntegerArgument( 0 );
		msg->appendIntegerArgument( timeGetTime() );
	}
	fprintf( stderr, "[GXSEEK] input type=%d x=%d y=%d frame=%u live=%d inGame=%d target=%u pending=%u paused=%d\n", (Int)t, s_at.x,
		s_at.y, TheGameLogic->getFrame(), (Int)s_live, (Int)TheGameLogic->isInGame(), s_target, s_flight.pending,
		(Int)TheGameLogic->isGamePaused() );
}

void update()
{
	if (TheGameLogic == nullptr || TheWritableGlobalData == nullptr)
		return;
	inputDriver();
	if (TheGameLogic->isInShellGame() || !TheGameLogic->isInGame())
	{
		// Back at the menu, or the replay ended (its score screen): a race from a match that is gone is dropped and the sound
		// comes back (the shell runs at 1x). Between a restart's clear and its load it all waits for the new playback.
		if (TheGameLogic->isInShellGame() || !s_flight.restarting)
		{
			s_flight.pending = 0;
			stopRace();
			s_dock.reset();		// a strip dragged in one replay does not carry into the next (the spectator panels reset per match too)
			leaveRace( FALSE );
		}
		return;
	}
	if (!playbackUp())
	{
		if (s_target != 0)
			stopRace();
		s_flight.pending = 0;
		leaveRace( FALSE );
		return;
	}
	s_flight.restarting = FALSE;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame < s_lastFrame)	// a new playback: the pace it starts at is not ours
		s_halved = FALSE;
	s_lastFrame = frame;
	if (s_flight.pending != 0)
	{
		s_target = s_flight.pending;
		s_viaRestart = TRUE;
		s_flight.pending = 0;
	}
	if (s_target != 0)
	{
		if (raceDone( frame, s_target ))
		{
			fprintf( stderr, "[GXSEEK] reached frame=%u want=%u\n", frame, s_target );
			stopRace();
			setPaused( TRUE );
			leaveRace( TRUE );
		}
		else if (TheGameLogic->isGamePaused())
		{
			// Paused since the race began (the quit menu, the pause key, our own button): that is not ours to undo, and
			// a pause ends a race. seekTo() and the restart unpause BEFORE the race, so this is always someone else's.
			fprintf( stderr, "[GXSEEK] race to %u ended by a pause at frame=%u\n", s_target, frame );
			stopRace();
			leaveRace( TRUE );
		}
		else
		{
			TheWritableGlobalData->m_TiVOFastMode = TRUE;
			holdRace();
		}
	}
	else
	{
		leaveRace( TRUE );	// a race the pause button ended, or a plain restart now up
		envDriver();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------------------------------------------------
static void addStripHit( const Rect &r, Int kind, Int id )
{
	if (s_hitCount < (Int)ARRAY_SIZE( s_hits ))
	{
		SHit &h = s_hits[s_hitCount++];
		h.r = r;
		h.kind = kind;
		h.id = id;
	}
}

static const SHit *stripHitAt( Int x, Int y )
{
	for (Int i = 0; i < s_hitCount; ++i)
		if (s_hits[i].r.contains( x, y ))
			return &s_hits[i];
	return nullptr;
}

static const SHit *hitOf( Int kind, Int id )
{
	for (Int i = 0; i < s_hitCount; ++i)
		if (s_hits[i].kind == kind && s_hits[i].id == id)
			return &s_hits[i];
	return nullptr;
}

static Bool isPressed( Int kind, Int id )
{
	return s_pressed && s_press.kind == kind && s_press.id == id;
}

/// A button: the plate, a blue wash while it is the lit one, a lighter wash under the mouse and a darker one while held
/// (the press answers at once, on the frame it happens).
static void button( const Rect &r, Int kind, Int id, Bool lit )
{
	const Bool held = isPressed( kind, id ) && mouseIn( r );
	fill( r, held ? navyDeep( 235 ) : navy( 200 ) );
	box( r, lit ? accent() : steelDim() );
	if (lit)
		fill( r.x + 1, r.y + 1, r.w - 2, r.h - 2, GameMakeColor( 111, 168, 240, 56 ) );
	else if (mouseIn( r ) && !held)
		fill( r.x + 1, r.y + 1, r.w - 2, r.h - 2, GameMakeColor( 111, 168, 240, 28 ) );
	addStripHit( r, kind, id );
}

static void findRetailBar()
{
	if (s_retailBar != nullptr || TheWindowManager == nullptr)
		return;
	for (GameWindow *w = TheWindowManager->winGetWindowList(); w != nullptr; w = w->winGetNext())
		if (w->winGetInstanceData()->m_decoratedNameString.compare( "ControlBar.wnd:ControlBarParent" ) == 0)
		{
			s_retailBar = w;
			return;
		}
}

static UnicodeString clock2( UnsignedInt frame, UnsignedInt len )
{
	UnicodeString a = clockText( frame ), b = clockText( len );
	a.concat( L" / " );
	a.concat( b );
	return a;
}

/// The strip's rect while it is drawn (the spectator HUD stands its bottom bar on top of it); one frame stale, which is fine.
static Bool liveRect( Rect &out )
{
	if (!s_live)
		return FALSE;
	out = s_strip;
	return TRUE;
}

Bool draw()
{
	s_live = FALSE;
	s_hitCount = 0;
	if (TheDisplay == nullptr || !playbackUp() || TheInGameUI == nullptr || TheInGameUI->isQuitMenuVisible())
		return FALSE;
	ensureScale();
	findRetailBar();

	const UnsignedInt frame = TheGameLogic->getFrame();
	const UnsignedInt len = length();
	const Int speed = currentSpeed();
	const Bool paused = TheGameLogic->isGamePaused();

	// Bottom centre. In the legacy layout the retail control bar is up: stand above it.
	const Int h = S( 44 );
	const Int w = min( S( 800 ), s_W * 7 / 10 );
	Int y = s_H - S( 18 ) - h;
	if (s_retailBar != nullptr && !s_retailBar->winIsHidden())
	{
		Int bx = 0, by = 0;
		s_retailBar->winGetScreenPosition( &bx, &by );
		y = min( y, by - S( 12 ) - h );
	}
	s_strip = Rect{ (s_W - w) / 2, y, w, h };
	{	// Dragged off home: the spectator panels' offset idiom, kept wholly on screen at every resolution.
		Dock::Panel &dp = s_dock.panel[0];
		clampOffset( s_strip.x, s_strip.y, w, h, s_W, s_H, dp.dx, dp.dy );
		s_strip.x += dp.dx;
		s_strip.y += dp.dy;
	}
	{	// The release never came (a retail window ate it): the button stayed up across two draws and the drag is still held. One
		// pass is not enough - a release is queued until after this draw (see LostRelease), and cancelling then swallowed it.
		const MouseIO *dm = TheMouse ? TheMouse->getMouseStatus() : nullptr;
		const Bool down = dm == nullptr || dm->leftState == MBS_Down || dm->leftState == MBS_DoubleClick;
		if (s_lostRelease.observe( s_dock.dragging(), down ))
		{
			s_dock.cancel();
			s_pressed = FALSE;
		}
	}
	plate( s_strip );
	if (s_dock.dragging())
		box( s_strip, accent() );		// held: the frame lights while it moves

	const Int pad = S( 8 ), cell = h - 2 * pad;
	Int x = s_strip.x + pad;
	const Int cy = s_strip.y + h / 2;

	// Pause / play.
	const Rect pb = { x, s_strip.y + pad, cell, cell };
	button( pb, K_PAUSE, 0, paused );
	{
		const Color c = paused ? accent() : white();
		const Int ix = pb.x + pb.w / 2, sz = S( 12 );
		if (paused)			// a right-pointing triangle: the thing a click will do
			for (Int i = 0; i < sz; ++i)
				fill( ix - sz / 3 + i, cy - (sz - i) / 2, 1, max( 1, sz - i ), c );
		else
		{
			fill( ix - S( 5 ), cy - sz / 2, S( 4 ), sz, c );
			fill( ix + S( 1 ), cy - sz / 2, S( 4 ), sz, c );
		}
	}
	x += cell + S( 8 );

	// Speeds.
	const Int sw = S( 42 );
	for (Int s = 0; s < SPEED_COUNT; ++s)
	{
		const Rect r = { x, s_strip.y + pad, sw, cell };
		const Bool lit = s == speed && !paused;
		button( r, K_SPEED, s, lit );
		const wchar_t *lbl = speedLabel( s );
		putC( fixed( s == 0 ? "rs.s0" : s == 1 ? "rs.s1" : s == 2 ? "rs.s2" : s == 3 ? "rs.s3" : "rs.s4", 13, lit, lbl ),
			r.x + r.w / 2, r.y + (r.h - th( 13 )) / 2, lit ? white() : label() );
		x += sw + S( 3 );
	}
	x += S( 9 );

	// Time reached / whole length.
	const UnsignedInt shown = s_target != 0 ? s_target : frame;
	const UnicodeString clk = len > 1 ? clock2( shown, len ) : clockText( shown );
	DisplayString *cs = txt( "rs.clock", 0, 14, TRUE, (long long)(shown / LOGICFRAMES_PER_SECOND) * 100000 + len / LOGICFRAMES_PER_SECOND,
		[&clk]() { return clk; } );
	const Int cw = S( 118 );
	put( cs, x, cy - th( 14 ) / 2, white() );
	x += cw + S( 6 );

	// The timeline: a thin track, played part in the accent, the playhead a bright tick; unknown length = no bar.
	const Int bx1 = s_strip.x + s_strip.w - pad - S( 4 );
	s_bar = Rect{ x, s_strip.y + pad, max( 0, bx1 - x ), cell };
	if (len > 1 && s_bar.w > S( 40 ))
	{
		const Int ty = cy - S( 3 ), th2 = S( 6 );
		fill( s_bar.x, ty, s_bar.w, th2, navyDeep( 240 ) );
		box( Rect{ s_bar.x, ty, s_bar.w, th2 }, steelDim() );
		const Int px = xAtFrame( frame, s_bar.w, len );
		fill( s_bar.x, ty + 1, px, th2 - 2, accent() );
		if (s_target != 0)
		{
			const Int tx = xAtFrame( s_target, s_bar.w, len );
			fill( s_bar.x + tx - 1, ty - S( 4 ), 2, th2 + S( 8 ), amber() );
		}
		fill( s_bar.x + px - 1, ty - S( 5 ), 3, th2 + S( 10 ), white() );
		addStripHit( s_bar, K_BAR, 0 );

		// Hover: the time under the mouse, above the bar.
		const MouseIO *m = TheMouse ? TheMouse->getMouseStatus() : nullptr;
		if (m != nullptr && s_bar.contains( m->pos.x, m->pos.y ))
		{
			const UnsignedInt hf = frameAtX( m->pos.x, s_bar.x, s_bar.w, len );
			fill( m->pos.x, ty - S( 3 ), 1, th2 + S( 6 ), steel() );
			DisplayString *hs = txt( "rs.hover", 0, 12, FALSE, hf / LOGICFRAMES_PER_SECOND, [hf]() { return clockText( hf ); } );
			const Int hw = tw( hs ) + S( 12 );
			const Rect tip = { clamp( s_strip.x, m->pos.x - hw / 2, s_strip.x + s_strip.w - hw ), s_strip.y - S( 26 ), hw, S( 22 ) };
			plate( tip, TRUE );
			putC( hs, tip.x + tip.w / 2, tip.y + (tip.h - th( 12 )) / 2, white() );
		}
	}

	// One plain line above the strip while a race runs (and always says what a click behind does).
	if (s_target != 0)
	{
		UnicodeString msg;
		const UnsignedInt tgt = s_target;
		msg.format( s_viaRestart ? L"Restarted. Racing to %ls" : L"Racing to %ls", clockText( tgt ).str() );
		DisplayString *ms = txt( "rs.race", s_viaRestart ? 1 : 0, 13, FALSE, tgt / LOGICFRAMES_PER_SECOND, [msg]() { return msg; } );
		const Rect lab = { s_strip.x, s_strip.y - S( 26 ), tw( ms ) + S( 16 ), S( 22 ) };
		plate( lab, TRUE );
		put( ms, lab.x + S( 8 ), lab.y + (lab.h - th( 13 )) / 2, amber() );
	}
	else if (mouseIn( s_bar ) && len > 1)
	{
		const MouseIO *m = TheMouse->getMouseStatus();
		if (frameAtX( m->pos.x, s_bar.x, s_bar.w, len ) < frame)
		{
			DisplayString *ms = fixed( "rs.back", 13, FALSE, L"Behind you: restarts the replay and races back" );
			const Rect lab = { s_strip.x, s_strip.y - S( 26 ), tw( ms ) + S( 16 ), S( 22 ) };
			plate( lab, TRUE );
			put( ms, lab.x + S( 8 ), lab.y + (lab.h - th( 13 )) / 2, label() );
		}
	}

	s_live = TRUE;
	return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input: a press on a control is ours and acts on release over the same control; everything else passes through.
// ---------------------------------------------------------------------------------------------------------------------
static void act( const SHit &h, Int x )
{
	fprintf( stderr, "[GXSEEK] act kind=%d id=%d frame=%u\n", h.kind, h.id, TheGameLogic->getFrame() );
	switch (h.kind)
	{
		case K_PAUSE:
			stopRace();		// a pause is a stop: it ends any race
			s_flight.pending = 0;
			setPaused( !TheGameLogic->isGamePaused() );
			break;
		case K_SPEED:
			setSpeed( h.id );
			break;
		case K_BAR:
			seekTo( frameAtX( x, s_bar.x, s_bar.w, length() ) );
			break;
	}
}

class Translator : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage( const GameMessage *msg ) override;
	virtual ~Translator() override {}
};

GameMessageDisposition Translator::translateGameMessage( const GameMessage *msg )
{
	if (!s_live)
	{
		s_pressed = FALSE;
		s_dock.cancel();
		return KEEP_MESSAGE;
	}
	const GameMessage::Type t = msg->getType();
	if (t != GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN && t != GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK
		&& t != GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP && t != GameMessage::MSG_RAW_MOUSE_LEFT_DRAG)
		return KEEP_MESSAGE;
	if (msg->getArgumentCount() < 1 || msg->getArgumentDataType( 0 ) != ARGUMENTDATATYPE_PIXEL)
		return KEEP_MESSAGE;
	const ICoord2D at = msg->getArgument( 0 )->pixel;
	if (t != GameMessage::MSG_RAW_MOUSE_LEFT_DRAG)
		s_lostRelease.reset();		// a press or a release starts the count over: a draw that saw drag A's queued release must not count toward drag B
	if (t == GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP)
	{
		if (!s_pressed)
			return KEEP_MESSAGE;
		const SHit press = s_press;
		s_pressed = FALSE;
		if (press.kind == K_DRAG)
		{
			s_dock.release( at.x, at.y );		// a drag ends where it is; a plain click on the background does nothing
			return DESTROY_MESSAGE;
		}
		const SHit *h = stripHitAt( at.x, at.y );
		if (h != nullptr && h->kind == press.kind && h->id == press.id)
			act( *h, at.x );
		return DESTROY_MESSAGE;
	}
	if (t == GameMessage::MSG_RAW_MOUSE_LEFT_DRAG)
	{
		if (s_pressed && s_press.kind == K_DRAG)
			s_dock.move( at.x, at.y );
		return s_pressed ? DESTROY_MESSAGE : KEEP_MESSAGE;
	}
	const SHit *h = stripHitAt( at.x, at.y );
	s_dock.cancel();		// a fresh press retires a drag whose release another translator swallowed (legacy radar)
	if (h == nullptr)
	{
		if (!startsDrag( Cell{ s_strip.x, s_strip.y, s_strip.w, s_strip.h }, FALSE, at.x, at.y ))
		{
			s_pressed = FALSE;		// a press elsewhere ends one whose release another translator swallowed (legacy radar)
			return KEEP_MESSAGE;
		}
		// The strip's empty background: ours to drag. A double click snaps it back home (the spectator tabs' snap-home idiom).
		if (t == GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK)
			s_dock.panel[0].dx = s_dock.panel[0].dy = 0;
		s_press = SHit{ s_strip, K_DRAG, 0 };
		s_pressed = TRUE;
		s_dock.press( 0, FALSE, at.x, at.y );
		return DESTROY_MESSAGE;
	}
	s_press = *h;
	s_pressed = TRUE;
	return DESTROY_MESSAGE;
}

} // namespace Strip
} // namespace GXSpecHud

static Bool gxReplayStripDraw() { return GXSpecHud::Strip::draw(); }
static void gxReplayStripNotDrawn() { GXSpecHud::Strip::s_live = FALSE; }

void gxReplayStripUpdate() { GXSpecHud::Strip::update(); }

/// W3DDisplay::draw: is the strip racing to a frame (GXReplayStrip::passRenders)?
Bool gxReplayRaceActive() { return GXSpecHud::Strip::s_target != 0; }

GameMessageTranslator *gxNewReplayStripTranslator()
{
	return MSGNEW( "GameClientSubsystem" ) GXSpecHud::Strip::Translator;
}
