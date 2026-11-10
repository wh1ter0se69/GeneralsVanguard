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

/*
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#include "OpenALAudioManager.h"
#include "OpenALAudioDevice/OpenALAudioStream.h"   // AudioDebugDump()
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/LobbySetupUi.h"	// ComboTextTypedByKey, ComboKeyDownVouch, ComboVouchSurvives
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/GlobalData.h"
#include "GameLogic/GameLogic.h"		// isInLanGame / isInInternetGame, for the LANAPI pump below
#include "Common/MessageStream.h"	// MSG_META_DEMO_INSTANT_QUIT on window close
#include "GameNetwork/LANAPI.h"
#include "GameClient/GXLobbyPresence.h"	// TheGXLobbyPresence::update			// LANAPI::update
#include "GameClient/GXIdentityPush.h"	// gxDeclareIdentityIfChanged - the headless peer's GXIDN
#include "GameClient/GXReplayDiag.h"	// gxReplayTelSuspend / gxReplayTelAway - the replay telemetry's gap and away flag
#include "GameNetwork/LANAPICallbacks.h"	// extern LANAPI *TheLAN
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include "GameClient/GXShippedMaps.h"	// gxInstallShippedMaps - the launcher's map copy, for a platform with no launcher
#include "GameClient/MapUtil.h"	// TheMapCache->getUserMapDir(), the folder MapCache scans as user maps
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <atomic>

// ---------------------------------------------------------------------------
// iOS app lifecycle
//
// iOS suspends the process when the app leaves the foreground. Any GPU work
// submitted around suspension stalls on drawable acquisition (MoltenVK waits
// out a timeout per present), which surfaces as multi-second input hangs right
// after resuming. SDL warns that lifecycle events can arrive outside the
// normal poll cycle, so they are captured in an event watcher that fires
// immediately on the delivering thread; the engine update loop checks the
// flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop on iOS:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// Acquiring a Metal drawable during EITHER state fights iOS for the layer; across
// repeated suspend/switcher cycles MoltenVK is driven into an unrecoverable
// surface state and the app crashes (the reported "crashes after backgrounding /
// multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

// Defined in W3DDevice/GameClient/W3DDisplay.cpp - see the note there for why it lives in that
// file rather than this one.
extern void GX_SetRenderSuspended( Bool suspended );

static inline bool iosShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

// GeneralsX: Ask iOS for background execution time.
//
// Defined in SDL3Device/Common/IOSBackgroundTask.mm - Objective-C++, because UIApplication has no
// C API. No-ops on every other platform, so no #if is needed at the call sites.
extern "C" void GX_iOSBeginBackgroundTask(void);
extern "C" void GX_iOSEndBackgroundTask(void);
extern "C" bool GX_iOSBackgroundTaskExpired(void);

static bool SDLCALL iosLifecycleWatcher(void *userdata, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);

			// Take the background assertion HERE, in the watcher, and not in the
			// SDL_EVENT_DID_ENTER_BACKGROUND arm of pollSDL3Events() further down this file -
			// that arm is unreachable. SDL delivers these six lifecycle events by direct watcher
			// callback from inside UIKit's own stack and never queues them, precisely so that a
			// call like this can be made while UIKit is still willing to grant it. Code added to
			// the polling arm compiles, reads as correct, and does nothing.
			//
			// Idempotent, so being called for both WILL_ and DID_ is harmless.
			GX_iOSBeginBackgroundTask();
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			GX_iOSEndBackgroundTask();
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// Stay paused until fully active again (focus regained), which arrives
		// after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// iOS touch -> mouse gesture translation
//
// SDL's automatic touch-mouse synthesis is disabled on iOS (SDL3Main.cpp sets
// SDL_HINT_TOUCH_MOUSE_EVENTS=0); every mouse event the game sees on iOS is
// synthesized here, through the same SDL3Mouse::addSDLEvent path real mice use.
//
// Gestures (matching the game's stock control scheme, which is LMB-centric):
//   1 finger tap/drag     -> left button click / drag (select, command, drag-box)
//   1 finger long-press   -> right button click (deselect), if finger stays put
//   2 finger pinch        -> mouse wheel (camera zoom), camera stays put
//   3 finger drag         -> right-button drag (camera scroll), terrain follows
//                            the fingers rather than running from them
//
// While a shell menu is up there is no camera to move, and the useful gesture is
// scrolling a list (map selection, replays, saves). Two fingers there emit wheel
// ticks from vertical movement instead, and never press the right button.
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,        // no fingers tracked
		PENDING,     // finger1 down, gesture identity not yet known, nothing sent
		DRAGGING,    // finger1 drag in progress, LMB held
		LONGPRESSED, // long-press fired (RMB click sent), swallow until lift
		PINCH,       // two-finger zoom, camera anchored, no button held
		PAN,         // three-finger camera pan, RMB held
		SCROLL       // two-finger list scroll in a shell menu, no button held
	};

	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	SDL_FingerID finger3 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points)
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position
	float panX = 0.0f, panY = 0.0f;     // pan centroid
	float pinchDist = 0.0f;             // finger distance at last wheel step
	float scrollAccum = 0.0f;           // unspent vertical travel while SCROLLing
	Uint64 downTicks = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f; // normalized per finger
	float f3x = 0.0f, f3y = 0.0f;
	float lastDist = 0.0f;                 // finger spread at the previous motion
	float lastCx = 0.0f, lastCy = 0.0f;    // centroid at the previous motion
	bool  suppressRightButton = false;     // placement mode: RMB would cancel
	bool  rightButtonHeld = false;         // have we actually pressed RMB?
	bool  placementAnchored = false;       // LMB held to set a building's facing
	float cursorX = 0.0f, cursorY = 0.0f;  // where the synthetic cursor actually is
	bool  relativeDrag = false;            // move the cursor by deltas, not absolutely
	int   activeFingers = 0;               // fingers currently down, any phase
	Uint64 pinchTicks = 0;                 // when the two-finger gesture started

	// Two-finger intent, decided once per gesture and then held. Comparing the
	// two per-event lets the winner flip frame to frame — a pinch where one
	// finger travels further than the other swings the midpoint a long way, so
	// "pan" keeps winning individual frames and the camera crawls while you are
	// plainly just zooming. Measure both from where the gesture started, commit
	// to whichever crosses the threshold first, and ignore the other until the
	// fingers lift.
	enum Intent { UNDECIDED, ZOOMING, PANNING };
	Intent intent = UNDECIDED;
	float startDist = 0.0f;
	float startCx = 0.0f, startCy = 0.0f;
};

TouchState s_touch;

const Uint64 LONG_PRESS_MS = 600;
const float PINCH_STEP_RATIO = 0.06f;  // 6% distance change per wheel tick
const float TAP_DEAD_ZONE_PX = 8.0f;   // jitter below this keeps a tap a tap
// GeneralsX @tweak wh1ter0se 12/08/2026 Expressed in POINTS and scaled by pixel density at use,
// so a wheel tick costs the same PHYSICAL finger travel on every display. The touch coordinate fix
// above moved this arithmetic from points into pixels, which on a 2x iPad would otherwise have
// made menu scrolling twice as sensitive overnight - a silent feel change riding along with a
// correctness fix, which is exactly the kind of thing nobody attributes later.
const float SCROLL_STEP_POINTS = 36.0f;	// vertical finger travel per wheel tick in menus
const float GESTURE_COMMIT_PX = 24.0f; // travel needed to commit to zoom or pan
const Uint64 TWO_FINGER_TAP_MS = 250;  // quick two-finger tap = dismiss/back

void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window, Uint32 type,
                        float x, float y, Uint8 button = 0, float wheelY = 0.0f)
{
	// The windowID must be valid: SDL3Mouse::scaleMouseCoordinates() looks the
	// window up by id to map window points into the game's internal resolution,
	// and silently skips scaling when the lookup fails.
	const SDL_WindowID windowID = SDL_GetWindowID(window);

	// Remember where the synthetic cursor is. Placement dragging moves it by a
	// delta rather than jumping it to the finger midpoint, and needs this anchor.
	if (type == SDL_EVENT_MOUSE_MOTION) {
		s_touch.cursorX = x;
		s_touch.cursorY = y;
	}

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = type;
	switch (type) {
		case SDL_EVENT_MOUSE_MOTION:
			ev.motion.windowID = windowID;
			ev.motion.x = x;
			ev.motion.y = y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev.button.windowID = windowID;
			ev.button.button = button;
			ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			ev.button.clicks = 1;
			ev.button.x = x;
			ev.button.y = y;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			ev.wheel.windowID = windowID;
			ev.wheel.x = 0.0f;
			ev.wheel.y = wheelY;
			ev.wheel.mouse_x = x;
			ev.wheel.mouse_y = y;
			break;
	}
	mouse->addSDLEvent(&ev);
}

// Two fingers land: a shell menu scrolls its list, otherwise we zoom.
//
// Zoom deliberately holds no button and emits no motion. Driving the camera and
// the zoom from the same two fingers meant every pinch also dragged the view,
// which makes precise zooming impossible — the anchor slides out from under you.
// Panning now needs a third finger (beginPan below).
// A quick two-finger tap means "dismiss / back". ESC is what the engine already
// treats as cancel everywhere — menus, popups, placement, the in-game menu — so
// synthesising it reuses all of that rather than special-casing each screen.
void sendSyntheticEscape(SDL_Window *window)
{
	SDL3Keyboard *keyboard = dynamic_cast<SDL3Keyboard *>(TheKeyboard);
	if (keyboard == nullptr) {
		return;
	}
	const SDL_WindowID windowID = SDL_GetWindowID(window);
	for (int press = 1; press >= 0; --press) {
		SDL_Event ev;
		SDL_zero(ev);
		ev.type = press ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		ev.key.windowID = windowID;
		ev.key.scancode = SDL_SCANCODE_ESCAPE;
		ev.key.key = SDLK_ESCAPE;
		ev.key.down = press ? true : false;
		ev.key.repeat = false;
		keyboard->addSDLEvent(&ev);
	}
}

void beginTwoFinger(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
	const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
	const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
	s_touch.pinchDist = SDL_sqrtf(dx * dx + dy * dy);

	// In a shell menu two fingers mean "scroll this list", not "zoom the camera".
	// Hold no button: an RMB press on a listbox is at best meaningless and the
	// wheel is what the gadgets actually listen for.
	if (TheShell != nullptr && TheShell->isShellActive()) {
		s_touch.scrollAccum = 0.0f;
		s_touch.phase = TouchState::SCROLL;
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.panX, s_touch.panY);
		return;
	}

	// Two fingers drive both zoom and camera, but never in the same event: see the
	// PINCH motion handler, which picks whichever of the two actually moved.
	s_touch.lastDist = s_touch.pinchDist;
	s_touch.lastCx = s_touch.panX;
	s_touch.lastCy = s_touch.panY;
	s_touch.intent = TouchState::UNDECIDED;
	s_touch.pinchTicks = SDL_GetTicks();
	s_touch.startDist = s_touch.pinchDist;
	s_touch.startCx = s_touch.panX;
	s_touch.startCy = s_touch.panY;
	// While a building is waiting to be placed, the right button means "cancel", so
	// it must never be pressed during placement — two fingers there only move the
	// cursor, which drags the ghost where you want it.
	s_touch.suppressRightButton =
		(TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr);

	// During placement, do not teleport the cursor to the midpoint between the two
	// fingers. The ghost building follows the cursor, so jumping it to the centroid
	// the instant the second finger lands moves the building away from where the
	// player was actually pointing — and that is where it then gets placed. Anchor
	// on the current cursor position and apply finger movement as a delta instead.
	if (s_touch.suppressRightButton) {
		s_touch.relativeDrag = true;
	}

	// Do NOT press the right button yet, even outside placement.
	//
	// The engine's right-drag scroll (SCROLL_RMB) is velocity-based: while the
	// button is held the camera keeps scrolling from the cursor's displacement,
	// with no further input needed. Pressing it as soon as a second finger landed
	// meant every pinch ran on top of a live scroll gesture — the view drifted
	// during the zoom and kept drifting afterwards while the fingers rested. The
	// button is now pressed only once the gesture commits to panning.
	s_touch.rightButtonHeld = false;
	s_touch.phase = TouchState::PINCH;
}

// Third finger joins a pinch: keep dragging the camera, now from the three-finger
// centroid. The cursor tracks the centroid directly — the engine's right-drag
// scroll direction is what players already expect.
void beginPan(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	s_touch.panX = (s_touch.f1x + s_touch.f2x + s_touch.f3x) * (1.0f / 3.0f) * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y + s_touch.f3y) * (1.0f / 3.0f) * (float)winH;
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.panX, s_touch.panY);
	// A third finger is an explicit request to move the camera, so take the right
	// button here if the pinch had not already committed to panning. Never during
	// placement, where the right button cancels.
	if (!s_touch.suppressRightButton && !s_touch.rightButtonHeld) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
		s_touch.rightButtonHeld = true;
	}
	s_touch.phase = TouchState::PAN;
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	// GeneralsX @bugfix wh1ter0se 12/08/2026 PIXELS, not points. This is why the iPad menus could
	// not be tapped at all.
	//
	// GeneralsX: POINTS after all - because the scaling this compensated
	// for is now actually running. READ BOTH HALVES BEFORE CHANGING THIS AGAIN.
	//
	// SDL_tfinger.x/y are normalised 0..1 across the window, and have to be scaled into whatever
	// space the CONSUMER wants. The consumer is SDL3Mouse::scaleMouseCoordinates(), which every
	// event goes through on its way to the game (SDL3Mouse.cpp:946), and it derives its factor as
	// TheDisplay->getWidth() / SDL_GetWindowSize() - internal resolution over logical POINTS.
	//
	// The 12/08 fix was correct against the code as it then behaved: sendSyntheticMouse's own
	// comment records that scaleMouseCoordinates "silently skips scaling when the lookup fails",
	// and with no scaling applied the raw value had to be in pixels already. That lookup was
	// subsequently repaired, so the factor is now applied ON TOP of the pixel conversion. On this
	// iPad, internal 2360 over 1180 points is exactly 2.0: a tap at the centre of the screen was
	// multiplied to 2360 - the far right edge - and everything past the horizontal midpoint landed
	// off-screen entirely. Only the top-left quadrant mapped onto the UI at all, which is why the
	// menus read as completely dead rather than merely offset.
	//
	// Two fixes that were each right in isolation, multiplying. Emitting points hands
	// scaleMouseCoordinates the one space it actually expects and lets it do the single
	// conversion, which is also what every real mouse event on every platform already does.
	//
	// Nothing here is iOS-conditional: on a 1.0-density display the two calls return the same
	// numbers, so this is the correct expression everywhere.
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	if (winW <= 0 || winH <= 0) {
		// Fall back rather than divide the gesture into the corner: a zero here would put every
		// touch at (0,0), which is indistinguishable from "touch is broken" - the very bug above.
		SDL_GetWindowSizeInPixels(window, &winW, &winH);
	}
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		// Audio diagnostic trigger, counted independently of the gesture state
		// machine. It was previously hooked to the three-finger PAN phase, which
		// does not exist on menu screens — so on the very screen where the audio
		// fault shows up, a four-finger tap did nothing at all.
		++s_touch.activeFingers;
		if (s_touch.activeFingers == 4) {
			AudioDebugDump("four-finger tap");
		}
		else if (s_touch.activeFingers == 5) {
			// Five fingers = ESC: the in-game menu / pause, or back out of a screen.
			// Deliberately not two — that is the deselect tap — and not three, which
			// pans. Five is unreachable by accident during play.
			sendSyntheticEscape(window);
		}
		if (s_touch.phase == TouchState::IDLE) {
			// Defer all BUTTON output: a finger landing could become a tap, a
			// drag-box, a long-press, or the first finger of a camera pan. A
			// premature LMB down+up is a real click to the game (e.g. it sets a
			// rally point when a production building is selected).
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.phase = TouchState::PENDING;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			// Move the cursor to the touch point NOW (motion clicks nothing, so the
			// deferred-tap protection is intact). This lets the GUI process hover
			// over the next frame(s) before the tap commits — hover-driven widgets
			// (e.g. the Generals Challenge general buttons, which are checkboxes
			// that ignore a click unless WIN_STATE_HILITED was set by a prior
			// mouse-enter) then accept the click. Real mice hover before clicking;
			// without this, a synthetic tap teleports + clicks in one instant and
			// the widget is never hilited, so only the default/first item responds.
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PENDING) {
			// Second finger before the first committed to anything: zoom (or list
			// scroll in a menu), no left-click ever happened.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			beginTwoFinger(mouse, window, winW, winH);
		}
		else if (s_touch.phase == TouchState::DRAGGING) {
			// Second finger during a live drag: finish the drag-box, then zoom.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			beginTwoFinger(mouse, window, winW, winH);
		}
		else if (s_touch.phase == TouchState::PINCH) {
			s_touch.finger3 = event.tfinger.fingerID;
			s_touch.f3x = event.tfinger.x;
			s_touch.f3y = event.tfinger.y;
			if (s_touch.suppressRightButton) {
				// Placing a building: the engine already has a rotate gesture — press
				// and drag sets the facing (m_placeAnchorInProgress). Two fingers have
				// the ghost where it should go, so the third takes the left button
				// there and its movement swings the angle. Releasing commits, exactly
				// as a mouse press-drag-release would.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
				                   s_touch.panX, s_touch.panY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = true;
				s_touch.phase = TouchState::PAN;
			} else {
				// Third finger promotes the gesture to a camera pan.
				beginPan(mouse, window, winW, winH);
			}
		}
		// LONGPRESSED / PAN / SCROLL with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.lastX = px;
			s_touch.lastY = py;
		} else if ((s_touch.phase == TouchState::PINCH || s_touch.phase == TouchState::PAN ||
		            s_touch.phase == TouchState::SCROLL) &&
		           event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else if (s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger3) {
			s_touch.f3x = event.tfinger.x;
			s_touch.f3y = event.tfinger.y;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// Commit to a drag: anchor the LMB at the original touch point so
				// drag-boxes start where the finger first landed.
				//
				// GeneralsX @revert /07/2026 A previous attempt special-cased
				// building placement here: it held no button and synthesised a click on
				// release, on the theory that touch wants drag-to-position rather than
				// the mouse's press-to-place-then-drag-to-face. The operator tested it and it was
				// worse - it destroyed hold-and-rotate, which is what he actually uses.
				// The mouse semantics are the correct ones on touch too. Do not "fix"
				// this again without a concrete report of the old behaviour failing;
				// four attempts here have all been regressions.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				s_touch.phase = TouchState::DRAGGING;
			}
		}
		else if (s_touch.phase == TouchState::DRAGGING && event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::SCROLL) {
			// Content follows the fingers, as everywhere else on the platform:
			// dragging down reveals earlier entries, i.e. a wheel-up tick.
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			s_touch.scrollAccum += (cy - s_touch.panY);
			s_touch.panY = cy;
			s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			// scrollAccum is in the same space as winH, which since 16/08/2026 is POINTS - so this
			// ratio is 1.0 and the threshold is already the 36 points it says it is. The division
			// is KEPT rather than deleted: it is what makes the constant mean physical finger
			// travel regardless of which space winH is in, so if this file ever moves back to
			// pixels the scroll feel follows instead of silently doubling. Deleting it would
			// hard-code the assumption that just cost a release.
			int logH = 0;
			SDL_GetWindowSize(window, NULL, &logH);
			const float density = (logH > 0) ? ((float)winH / (float)logH) : 1.0f;
			const float step = SCROLL_STEP_POINTS * density;
			while (s_touch.scrollAccum >= step) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL,
				                   s_touch.panX, s_touch.panY, 0, 1.0f);
				s_touch.scrollAccum -= step;
			}
			while (s_touch.scrollAccum <= -step) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL,
				                   s_touch.panX, s_touch.panY, 0, -1.0f);
				s_touch.scrollAccum += step;
			}
		}
		else if (s_touch.phase == TouchState::PINCH) {
			// A two-finger gesture is either a zoom or a pan, never both, and which
			// one it is gets decided once. Both candidates are measured from where
			// the gesture began, in the same units, and the first to travel far
			// enough wins for the rest of the gesture.
			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
			const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
			const float dist = SDL_sqrtf(dx * dx + dy * dy);

			if (s_touch.intent == TouchState::UNDECIDED) {
				const float spreadTravel = SDL_fabsf(dist - s_touch.startDist);
				const float centreDx = cx - s_touch.startCx;
				const float centreDy = cy - s_touch.startCy;
				const float centreTravel = SDL_sqrtf(centreDx * centreDx + centreDy * centreDy);

				if (spreadTravel >= GESTURE_COMMIT_PX && spreadTravel >= centreTravel) {
					s_touch.intent = TouchState::ZOOMING;
				} else if (centreTravel >= GESTURE_COMMIT_PX) {
					s_touch.intent = TouchState::PANNING;
					// Re-base the pan so it starts from here rather than snapping the
					// camera by the distance already travelled while deciding.
					s_touch.panX = cx;
					s_touch.panY = cy;
					// Now, and only now, take hold of the right button — anchoring the
					// scroll where the pan actually begins.
					if (!s_touch.suppressRightButton && !s_touch.rightButtonHeld) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
						                   cx, cy, SDL_BUTTON_RIGHT);
						s_touch.rightButtonHeld = true;
					}
				}
			}

			if (s_touch.intent == TouchState::ZOOMING) {
				if (s_touch.pinchDist > 1.0f) {
					const float ratio = dist / s_touch.pinchDist;
					if (ratio > 1.0f + PINCH_STEP_RATIO) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, 1.0f);
						s_touch.pinchDist = dist;
					} else if (ratio < 1.0f - PINCH_STEP_RATIO) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, -1.0f);
						s_touch.pinchDist = dist;
					}
				}
			}
			else if (s_touch.intent == TouchState::PANNING) {
				if (s_touch.relativeDrag) {
					// Placement: shift the ghost by how far the fingers moved, keeping
					// it under where the player was already pointing.
					const float nx = s_touch.cursorX + (cx - s_touch.lastCx);
					const float ny = s_touch.cursorY + (cy - s_touch.lastCy);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, nx, ny);
					s_touch.panX = nx;
					s_touch.panY = ny;
				} else {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
					s_touch.panX = cx;
					s_touch.panY = cy;
				}
				s_touch.pinchDist = dist;
			}
			// UNDECIDED: emit nothing. A gesture below the threshold is indistinguishable
			// from resting fingers, and acting on it is what made the camera creep.

			s_touch.lastDist = dist;
			s_touch.lastCx = cx;
			s_touch.lastCy = cy;
		}
		else if (s_touch.phase == TouchState::PAN) {
			// Rotating a building follows the third finger alone: the facing is the
			// direction from the anchor to the cursor, so averaging in the two fingers
			// holding position would drag the angle back towards them.
			const float cx = s_touch.placementAnchored
				? (s_touch.f3x * (float)winW)
				: ((s_touch.f1x + s_touch.f2x + s_touch.f3x) * (1.0f / 3.0f) * (float)winW);
			const float cy = s_touch.placementAnchored
				? (s_touch.f3y * (float)winH)
				: ((s_touch.f1y + s_touch.f2y + s_touch.f3y) * (1.0f / 3.0f) * (float)winH);
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
			s_touch.panX = cx;
			s_touch.panY = cy;
		}
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (s_touch.activeFingers > 0) {
			--s_touch.activeFingers;
		}
		// Once the screen is clear the machine MUST return to IDLE, whichever finger
		// happened to lift last. This is checked before the "not a finger we track"
		// early-out below: that early-out used to skip the reset, so lifting finger1
		// first left the phase parked at LONGPRESSED and the lift of the remaining
		// finger returned here and broke out — stranding the machine in an inert
		// state with no way back. Every subsequent touch was swallowed: the game kept
		// rendering but accepted no input at all.
		if (s_touch.activeFingers <= 0) {
			s_touch.activeFingers = 0;
			s_touch.relativeDrag = false;
			if (s_touch.rightButtonHeld) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
				s_touch.rightButtonHeld = false;
			}
			if (s_touch.placementAnchored) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = false;
			}
			// Set it here as well as at the bottom: the early-out below can skip the
			// bottom entirely, and leaving the phase non-IDLE with no fingers down is
			// exactly what killed input.
			if (s_touch.phase != TouchState::PENDING &&
			    s_touch.phase != TouchState::DRAGGING) {
				s_touch.phase = TouchState::IDLE;
			}
		}
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !((s_touch.phase == TouchState::PINCH || s_touch.phase == TouchState::PAN ||
		       s_touch.phase == TouchState::SCROLL) &&
		      event.tfinger.fingerID == s_touch.finger2) &&
		    !(s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger3)) {
			break;
		}
		switch (s_touch.phase) {
			case TouchState::PENDING:
				// A CANCELED touch (incoming call, notification shade, palm
				// rejection) must not become a committed tap — that would be a
				// phantom select/command/rally-point click at the cancel point.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				// Clean tap: deliver the full click at the exact press position.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				break;
			case TouchState::DRAGGING:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, px, py, SDL_BUTTON_LEFT);
				break;
			case TouchState::PAN:
				if (s_touch.placementAnchored) {
					// Releasing the rotate drag commits the building at its chosen angle.
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
					s_touch.placementAnchored = false;
				}
				if (s_touch.rightButtonHeld) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					s_touch.rightButtonHeld = false;
				}
				break;
			case TouchState::PINCH:
				// A CANCELED touch is the OS taking the gesture away (palm rejection,
				// a notification banner, an incoming call). It is not a decision by
				// the player, so it must not commit a placement or fire a click.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					if (s_touch.rightButtonHeld) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
						s_touch.rightButtonHeld = false;
					}
					break;
				}
				// Two fingers on and straight back off, having committed to neither
				// zoom nor pan, is a right-click: deselect units, cancel a pending
				// placement, back out of a command. That is what the engine already
				// binds to the right button, so this reuses it everywhere instead of
				// special-casing screens. Previously this only happened by adding a
				// third finger, whose beginPan() press incidentally deselected.
				//
				// The gesture-intent lock does the hard part: UNDECIDED means neither
				// zoom nor pan travelled far enough, so a real pinch or drag can never
				// be mistaken for a tap.
				if (s_touch.intent == TouchState::UNDECIDED &&
				    (SDL_GetTicks() - s_touch.pinchTicks) <= TWO_FINGER_TAP_MS) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
					                   s_touch.panX, s_touch.panY);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					break;
				}
				if (s_touch.suppressRightButton) {
					// Placement mode: the two fingers were dragging the ghost, so
					// lifting them means "put it here". Without this the building
					// stayed pending and needed a separate tap to commit, which also
					// moved it again to wherever that tap landed.
					//
					// Not after a zoom: spreading two fingers to look around is not a
					// decision to build.
					if (s_touch.intent != TouchState::ZOOMING) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
						                   s_touch.panX, s_touch.panY);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
					}
				} else if (s_touch.rightButtonHeld) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					s_touch.rightButtonHeld = false;
				}
				break;
			case TouchState::SCROLL:
				// No button was ever pressed, so nothing to release.
				break;
			default:
				break;
		}
		// Resetting on the first lift while other fingers are still down would let
		// the leftovers read as a fresh gesture — a one-finger tap commits a click
		// wherever it happens to be, which in placement mode looks like the building
		// jumping. So stay inert until the hand actually leaves the glass; the
		// unconditional check at the top of this handler guarantees we get back to
		// IDLE even if the last finger to lift is not one we were tracking.
		if (s_touch.activeFingers <= 0) {
			s_touch.phase = TouchState::IDLE;
		} else {
			s_touch.phase = TouchState::LONGPRESSED;  // inert: swallows input until release
		}
		break;
	}
}

// Called once per engine frame (not just per touch event): a perfectly
// stationary finger produces no SDL events, so the long-press timer must be
// polled from the frame loop or it would never fire.
void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	// Watchdog against a stranded gesture.
	//
	// The state machine tracks fingers by counting DOWN and UP events, and if that
	// count ever drifts — a lost event across an app suspend, a cancel the OS never
	// pairs, a finger that lands during a modal — the machine can sit in a
	// non-IDLE phase forever and silently swallow every touch. The symptom is
	// brutal and non-obvious: the game keeps rendering and simulating but accepts
	// no input. Ask SDL what is genuinely on the glass and recover.
	if (s_touch.phase != TouchState::IDLE) {
		int liveFingers = 0;
		int numDevices = 0;
		if (SDL_TouchID *devices = SDL_GetTouchDevices(&numDevices)) {
			for (int d = 0; d < numDevices; ++d) {
				int n = 0;
				if (SDL_Finger **f = SDL_GetTouchFingers(devices[d], &n)) {
					liveFingers += n;
					SDL_free(f);
				}
			}
			SDL_free(devices);
		}
		if (liveFingers == 0) {
			if (s_touch.rightButtonHeld) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
				s_touch.rightButtonHeld = false;
			}
			if (s_touch.placementAnchored) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = false;
			}
			if (s_touch.phase == TouchState::DRAGGING) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			}
			s_touch.activeFingers = 0;
			s_touch.phase = TouchState::IDLE;
		}
	}

	if (s_touch.phase == TouchState::PENDING &&
	    (SDL_GetTicks() - s_touch.downTicks) >= LONG_PRESS_MS) {
		// No LMB was sent yet (deferred), so this is a pure right-click.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		s_touch.phase = TouchState::LONGPRESSED;
	}
}

} // anonymous namespace
#endif // TARGET_OS_IPHONE

namespace {

// GeneralsX: The last SDL key-down, for forwardTextInputEvent to tell a keystroke's
// text from an IME's. A text event is a keystroke only when it is the one character that key-down itself
// names (its layout keycode, case aside), and only the first text straight after it (pollSDL3Events clears it
// on any event but a key-up in between): an IME's commit follows the key that committed it (Space, Return, a
// candidate digit), which names something else, so even a one-character commit is not mistaken for a typed
// letter. A Ctrl/Cmd chord vouches for nothing. The repeat flag is that key-down's own.
static WideChar s_keyDownChar = 0;
static bool s_keyDownRepeat = false;

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	// GeneralsX: Close the lobby-presence socket with the engine.
	if (TheGXLobbyPresence != NULL)
	{
		delete TheGXLobbyPresence;
		TheGXLobbyPresence = NULL;
	}

	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
/*
 * GeneralsX: THE SHIPPED MAPS ON AN iPAD.
 * The bundle carries them in <GeneralsXPatch>/ShippedMaps/<Name>/ (package-ios-zh.sh), and on the desktop the
 * launcher's PLAY copies that folder into <userdata>/Maps. An iPad has no launcher, so this does the same copy, by the same
 * rule (GXShippedMaps.h), once per start, after GameEngine::init() has made TheMapCache and fixed the user-data folder and
 * before any screen lists maps. Into getUserMapDir() and nowhere else: those are user maps with the PORTABLE_USER_MAPS lobby
 * path a desktop gives the same map. The overlay folder is resolved the way StdBIGFileSystem resolves it: GENERALSX_PATCH_PATH,
 * else <exe dir>/GeneralsXPatch (on iOS the executable sits at the bundle root, which is SDL_GetBasePath()). A failure is a
 * line on stderr and the game starts regardless; the next start tries again.
 */
static void gxInstallShippedMapsAtStart()
{
	if (TheMapCache == nullptr)
		return;
	std::filesystem::path overlay;
	const char *env = getenv("GENERALSX_PATCH_PATH");
	if (env != nullptr && *env != '\0')
	{
		overlay = env;
	}
	else
	{
		const char *base = SDL_GetBasePath();
		if (base == nullptr)
			return;
		overlay = std::filesystem::path(base) / "GeneralsXPatch";
	}
	const GXShippedMapsReport r = gxInstallShippedMaps(overlay / "ShippedMaps", TheMapCache->getUserMapDir().str());
	for (const std::string &n : r.installed)
		fprintf(stderr, "[gx] shipped map installed: %s\n", n.c_str());
	for (const std::string &n : r.replaced)
		fprintf(stderr, "[gx] shipped map installed: %s\n", n.c_str());
	for (const std::string &why : r.failed)
		fprintf(stderr, "[gx] shipped maps: %s; the next start tries again\n", why.c_str());
}
#endif

/**
 * From GameEngine: init() - initialize subsystems
 *
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	// GeneralsX: The shared-lobby presence socket. Created HERE, in the
	// device engine, and not in GameEngine::init(): Common/ is inside the sourceID digest and this
	// feature is deliberately digest-neutral so it can ship without an atomic four-platform rebuild.
	// It opens no socket until an Online screen calls enterLobby(), so constructing it always costs
	// nothing on the singleplayer path.
	if (TheGXLobbyPresence == NULL)
	{
		TheGXLobbyPresence = NEW GXLobbyPresence;

		// GeneralsX: GX_LOBBY_TEST=1 takes presence immediately, without the
		// Online screen. The screen requires a login, so there is otherwise no way to exercise this
		// path headlessly - and "it compiles" is not evidence that a socket registers. Off unless
		// the variable is set, and it only ever ADDS a lobby membership; nothing else reads it.
		if (getenv("GX_LOBBY_TEST") != NULL)
		{
			TheGXLobbyPresence->enterLobby();
			TheGXLobbyPresence->setNickname(AsciiString("GXHeadless"));
		}

		/*
		 * GeneralsX: A DIRECT MATCH LAUNCH NEVER LEARNED WHERE THE RELAY
		 * WAS, so it filed no telemetry at all - and one silent member turns the panel's
		 * attribution off for its whole room, because it will not name a player unless everybody in
		 * the room is reporting. The standing lobby host is a `-lanhost`, so that was production.
		 *
		 * HERE, IN init(), AND DELIBERATELY NOT IN update(). ResolveIP on a hostname is a blocking
		 * gethostbyname, and under lockstep a stall on one peer is charged to every peer;
		 * tools/relay/test-perf.js [B2-1] is the standing scan against that call being reachable
		 * from a frame. This runs once, before the main loop exists.
		 *
		 * FOR EVERY LAUNCH WITH A RELAY CONFIGURED, AND THE GATE THAT USED TO BE HERE WAS WRONG.
		 * It read `m_lanRole != LANROLE_NONE` on the theory that everything else reaches the Online
		 * screen, which calls enterLobby(). It does not: Transport::initRelay turns relay mode on
		 * for ANY launch that has a RelayAddress and opens the lobby or game port, so a GUI LAN or
		 * Direct Connect match is relay-backed without ever having visited Online - and under that
		 * gate it never acquired the address, reported nothing, and took its whole room's
		 * attribution down with it. The right gate is "a relay is configured", and
		 * acquireRelayAddr() applies that itself: an empty RelayAddress returns before ResolveIP,
		 * so a single-player start on a machine that has never played online still pays nothing.
		 */
		TheGXLobbyPresence->acquireRelayAddr();
	}


	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX: Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Lifecycle events can fire outside the poll cycle on iOS; catch them
	// immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	gxInstallShippedMapsAtStart();
#endif
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();

	// GeneralsX: Pump LANAPI from the ENGINE, not from a menu screen.
	//
	// Until now nothing on this platform pumped it except lobby screen update callbacks -
	// LanLobbyMenuUpdate, WOLLobbyMenuUpdate and NetworkDirectConnectUpdate - each behind its own
	// gates, and the screen a joiner actually SITS on says `//TheLAN->update(); // this is handled
	// in the lobby`. That inheritance held only because Shell::update happens to run every layout
	// on the stack; every gate on the lobby underneath is a way for a joiner's networking to die
	// without the joiner doing anything:
	//
	//     LanLobbyMenuUpdate   if (TheShell->isAnimFinished() && !LANbuttonPushed && TheLAN)
	//     WOLLobbyMenuUpdate   if (s_relayLobby && !isShuttingDown && !buttonPushed)
	//
	// Measured on the relay path: a joiner's pump stopped at the ACCEPT click, seven host datagrams
	// arrived at the NIC 10 s apart and were never read, and 60 s later the relay reaped its
	// membership - because a pump that is not called drains no socket, sends no periodic HELLO and,
	// worst of all, sends no relay registration. The host then announced "player was not
	// responding": a lobby that looks fine, is deaf and mute, and blames the wire.
	//
	// Generals Online reached the same conclusion - it ticks its online services manager from
	// GameEngine::update, right beside TheNetwork->UPDATE, with a null check and nothing else.
	//
	// AHEAD of the iOS pause check on purpose. That is the same reasoning as Win32GameEngine's
	// alt-tabbed branch, which pumps LANAPI precisely so that a player who tabs away does not fall
	// out of the lobby; an iPad whose owner pulled the notification shade down must likewise keep
	// its relay registration alive, or it is reaped for a glance.
	//
	// GeneralsX: Pump LANAPI DURING a match too. Not pumping it is why a
	// game vanishes from the browser the instant it starts.
	//
	// The exclusion above was deliberate and its reasoning was sound - "this can only ADD pumping in
	// the menu phase and can never change what happens during a match" - but it has a consequence
	// nobody traced: LANAPI::update() contains the host's periodic re-advertisement, including the
	// branch written specifically for this case (LANAPI.cpp:697-702):
	//
	//     else if (m_currentGame)
	//     {
	//         // game is in progress - RequestGameAnnounce will check if we should send it
	//         RequestGameAnnounce();
	//     }
	//
	// That branch can never execute, because the function containing it is not called once
	// isInLanGame() is true. So the host stops advertising the moment the match begins, the relay
	// ages the listing out on lastSeen, and the game disappears from every browser - including that
	// of a player who leaves it while it is still running and looks for it again. Reported live: an
	// iPad left a match hosted by the Linux peer and could not find the still-running game.
	//
	// THE CURE IS SAFE BY THE CODE'S OWN GUARDS, which is the part worth checking rather than
	// assuming. The destructive half of update() - timing players out and synthesising leave
	// messages - is ALREADY gated on `m_currentGame && !m_currentGame->isGameInProgress()`
	// (LANAPI.cpp:746). The lobby-player weeding walks m_lobbyPlayers, not the match roster, and the
	// game weeding explicitly skips m_currentGame (`game != m_currentGame`). Whoever wrote that
	// anticipated running during a match; only this gate prevented it.
	//
	// Cost on the match path is a socket drain plus a 200 ms-throttled send. Set against what was
	// removed from the host this session - a full object-list CRC walk and a blocking fflush on
	// EVERY logic frame - it is not measurable. Measure it anyway before believing that.
	if (TheLAN != nullptr && TheGameLogic != nullptr)
	{
		TheLAN->update();
	}

	// GeneralsX: Shared-lobby presence, pumped HERE and nowhere else.
	//
	// Same reason TheLAN->update() had to move here in eebae98ea: a pump hung off a UI callback only
	// runs while that particular screen is on the stack, so it stops the moment a menu changes and
	// the peer goes deaf without a single error anywhere. This socket carries the Online roster and
	// lobby chat, so a stalled pump reads to the player as "nobody is online".
	//
	// Deliberately NOT gated on TheGameLogic: the presence socket is meaningful in the menus, before
	// any game object exists. It is cheap when idle - the state machine returns immediately unless a
	// millisecond deadline has passed - and it is a different socket from the game's, so nothing it
	// does can touch lockstep traffic.
	if (TheGXLobbyPresence != nullptr)
	{
		TheGXLobbyPresence->update();
	}

	// GeneralsX: Declare the SIGNED identity from HERE too, for the same
	// reason the two pumps above are here: every writer of Transport::m_relayIdentity was GUI menu
	// code, so a peer with no menus - the standing Linux host, every harness peer - sent no GXIDN
	// however valid its GXLogin.txt was, and RELAY_REQUIRE_IDENTITY=1 drops it 15 s after it joins.
	// The whole trace, and why it cannot live in HeadlessMatch.cpp or Transport.cpp, is in the header.
	// IDENTICAL to Win32GameEngine.cpp - see the d19090d14 note above.
	gxDeclareIdentityIfChanged();

	// GeneralsX: The replay telemetry's AWAY flag (U): in a match the loop keeps simulating while
	// nobody is looking - an iOS app inactive or in the background (the render-suspended branch below), a desktop window that lost
	// focus - and those rows read fps 0 at 60 TPS with no flag. Set BEFORE GameEngine::update()
	// samples, so this iteration's row carries it; the solo iOS stop below is flagged suspended as well, which wins in the reader.
	// m_IsActive: true from init (headless too), false on SDL_EVENT_WINDOW_FOCUS_LOST / DID_ENTER_BACKGROUND.
	Bool telAway = !m_IsActive;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	if (iosShouldPauseRendering())
		telAway = TRUE;
#endif
	gxReplayTelAway(telAway);

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// GeneralsX: Suspend the GPU, not the simulation.
	//
	// The comment above used to say "Pause sim + render", and pausing the sim is what broke every
	// OTHER player. Lockstep cannot advance frame N until every peer's input for N has arrived, so
	// returning before GameEngine::update() - which drives TheGameLogic, TheMessageStream and
	// TheNetwork - froze the whole lobby for as long as an iPad player had a notification banner
	// down or was peeking at the app switcher. iosShouldPauseRendering() is true for plain
	// INACTIVE, not just backgrounded, so a banner was enough.
	//
	// Windows keeps simulating while minimised for precisely this reason (Win32GameEngine's
	// `break; // keep running.`). This makes the two engines agree.
	//
	// Outside a networked match nobody is waiting on us, so the cheap full stop is still right -
	// it saves battery and is the behaviour that was actually wanted on a phone.
	const Bool inNetMatch = (TheGameLogic != NULL)
		&& (TheGameLogic->isInLanGame() || TheGameLogic->isInInternetGame());

	if (iosShouldPauseRendering()) {
		if (!inNetMatch) {
			// GeneralsX: The loop stops: the replay's telemetry shows a "suspended" gap,
			// never one long frame (GXReplayDiag.h). Idempotent; cleared below once the loop runs again.
			gxReplayTelSuspend(true);
			SDL_Delay(50);
			return;
		}
		// In a match: keep the loop running, but let nothing reach Metal. W3DDisplay::draw()
		// returns early on this flag, which is the same shape as its existing IsIconic guard.
		GX_SetRenderSuspended(TRUE);
	} else {
		GX_SetRenderSuspended(FALSE);
	}
	gxReplayTelSuspend(false);
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * GeneralsX: The refresh rate of the display the game window is on, rounded to
 * whole Hz (59.94 -> 60, 119.88 -> 120), or 0 when SDL cannot say: headless (no window), or a mode that
 * reports no rate. The max-FPS cap's default when the launcher sets none (Common/FpsLimitPolicy.h). The
 * same SDL_GetDisplayForWindow + SDL_GetCurrentDisplayMode pair W3DDisplay already sizes the window with.
 * On iOS this is the screen's maximum rate (120 on a ProMotion iPad).
 */
Int SDL3GameEngine::getDisplayRefreshRate(void)
{
	// m_SDLWindow, not TheSDL3Window: init() leaves it null in headless mode, which has no display to follow.
	if (m_SDLWindow == nullptr)
		return 0;

	const SDL_DisplayID displayId = SDL_GetDisplayForWindow(m_SDLWindow);
	if (displayId == 0)
		return 0;

	const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(displayId);
	if (mode == nullptr || !(mode->refresh_rate > 0.0f))
		return 0;

	return (Int)(mode->refresh_rate + 0.5f);
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		// GeneralsX: A key-down vouches only for the text that comes straight
		// after it (s_keyDownChar, ComboVouchSurvives): key-ups may come between, nothing else.
		if (!ComboVouchSurvives(event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
		                        event.type == SDL_EVENT_TEXT_INPUT)) {
			s_keyDownChar = 0;
		}
		switch (event.type) {
			// GeneralsX: Quit through the game's own quit path, so peers
			// get a RESIGN instead of a peer that simply stops sending.
			//
			// Setting m_quitting here kills the process on the spot. In a networked match that is
			// the worst possible exit: lockstep cannot advance frame N until every peer's input
			// for N has arrived, so everyone else blocks on the departed peer until the disconnect
			// timeout expires. Windows has never had this problem - WinMain's WM_CLOSE posts
			// MSG_META_DEMO_INSTANT_QUIT, which reaches GameLogic::quit(TRUE) and either opens the
			// quit-confirmation menu or appends MSG_SELF_DESTRUCT, and the other peers see a
			// clean resign.
			//
			// Same divergence class as d19090d14: an engine-loop behaviour that exists on one of
			// the two engines. Copied from WinMain.cpp rather than reinvented.
			//
			// Safe outside a match too: GameClient treats a pending MSG_META_DEMO_INSTANT_QUIT as
			// "quitting", so the menu case still closes the app. The fallback covers the window
			// where the message stream is not accepting messages yet.
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				if (TheGameEngine != NULL && !TheGameEngine->getQuitting()) {
					if (TheMessageStream != NULL && TheMessageStream->isReadyForMessages()) {
						TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_INSTANT_QUIT);
					} else {
						TheGameEngine->setQuitting(TRUE);
					}
				}
				break;

			// GeneralsX: Two things Windows does on a focus change and
			// this engine did not. Both are one-liners; both are user-visible.
			//
			// muteAudio: WinMain's WM_ACTIVATE mutes with MuteAudioReason_WindowFocus. Without it,
			// alt-tabbing away on macOS or Linux leaves music, speech and battle audio playing at
			// full volume over whatever you switched to.
			//
			// TheLAN->setIsActive: LANAPI sends MSG_INACTIVE on the FALSE edge, and the host
			// un-accepts that slot - the enum comment reads "I've alt-tabbed out. Unaccept me".
			// Without it a peer who tabs away in the staging room still shows as ACCEPTED, so the
			// host can start a match with somebody who is not at the keyboard; they load in and
			// their base sits idle. Windows only does this while MINIMISED, which is itself half
			// the story, so this engine is now the stricter of the two.
			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				if (TheAudio) {
					TheAudio->unmuteAudio(AudioManager::MuteAudioReason_WindowFocus);
				}
				if (TheLAN != NULL) {
					TheLAN->setIsActive(TRUE);
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				gxReplayTelAway(true);	// GeneralsX: latch: a focus regained in this same poll still marks the open row
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				if (TheAudio) {
					TheAudio->muteAudio(AudioManager::MuteAudioReason_WindowFocus);
				}
				if (TheLAN != NULL) {
					TheLAN->setIsActive(FALSE);
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				gxReplayTelAway(true);	// GeneralsX: latch, as SDL_EVENT_WINDOW_FOCUS_LOST above
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				if (event.type == SDL_EVENT_KEY_DOWN) {	// read by forwardTextInputEvent
					s_keyDownChar = ComboKeyDownVouch((UnsignedInt)event.key.key,
						(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0);
					s_keyDownRepeat = event.key.repeat;
				}
				// F9 dumps the audio diagnostic ring. Handled here rather than through
				// the game's keybinding layer so it works on any screen, including
				// while a menu has focus.
				if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 &&
				    !event.key.repeat) {
					AudioDebugDump("F9 pressed");
				}
				// Fighter19 pattern: direct addSDLEvent() call
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}

				// GeneralsX: Make RETURN commit a text entry.
				//
				// Return is inert in EVERY GadgetTextEntry on macOS, Linux and iPadOS - the lobby
				// chat box is only where it is most obvious. The key arrives correctly and is
				// swallowed by the gadget: GadgetTextEntry's `case KEY_KPENTER: case KEY_ENTER:`
				// branch, the one that sends GEM_EDIT_DONE, is inside a /* */ block in retail EA
				// code. Nothing matches KEY_ENTER, and the handler still returns MSG_HANDLED, which
				// also ends winProcessKey's bubbling loop so the owning screen never sees it either.
				//
				// The only live Enter -> GEM_EDIT_DONE path is the gadget's GWM_IME_CHAR arm, and
				// GWM_IME_CHAR has exactly two producers: IMEManager - which is #ifdef _WIN32, so
				// TheIMEManager is null on every SDL3 build - and forwardTextInputEvent, which
				// cannot help because SDL_SendKeyboardText early-returns on control characters, so
				// Return never arrives as SDL_EVENT_TEXT_INPUT on any backend. Windows works purely
				// because WM_CHAR feeds IMEManager. Same divergence family as d19090d14.
				//
				// So synthesise exactly what Windows' WM_CHAR produces, and only for a focused
				// entry field. Deliberately NOT by uncommenting the gadget's KEY_ENTER branch: its
				// `e->receivedUnichar` guard is dead - the only assignment that sets it TRUE is
				// itself inside another commented block - so Windows would fire GEM_EDIT_DONE
				// twice, once from DirectInput and once from WM_CHAR. The GWM_CHAR(KEY_ENTER) that
				// still reaches the gadget stays inert, so there is no double-fire here either.
				if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
					&& (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
					&& TheWindowManager != NULL) {
					GameWindow *focused = TheWindowManager->winGetFocus();
					if (focused != NULL && BitIsSet(focused->winGetStyle(), GWS_ENTRY_FIELD)) {
						TheWindowManager->winSendInputMsg(focused, GWM_IME_CHAR,
							// 0x0D, the literal WM_CHAR wParam the Windows path delivers. Written out
							// rather than including keyboard_compat.h for one constant.
							(WindowMsgData)0x0D, 0);
					}
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Poll the long-press timer every frame; a stationary finger emits no events.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchLongPress(touchMouse, m_SDLWindow);
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

	// GeneralsX: Ask SDL what it is doing; do not trust a shadow flag.
	//
	// This is why the iOS keyboard did not come back after sending one message, and why the fix
	// was to tap the game list and then the chat bar again.
	//
	// The function derived what it WANTS from the engine (focus + GWS_ENTRY_FIELD) but what it
	// HAS from its own private m_IsTextInputActive. SDL clears its side behind our back: UIKit's
	// keyboardWillHide calls SDL_StopTextInput when the on-screen keyboard dismisses, which is
	// exactly what happens after a send. Our flag stayed true and focus never left the entry, so
	// the raise branch was never taken again and the keyboard could not be brought back - on
	// UIKit, becomeFirstResponder inside SDL_StartTextInput is the only thing that can raise it.
	//
	// The reported detour proves the mechanism: tapping the game list moved focus, which ran the
	// else branch and finally set the flag false, so the next tap on the entry passed the guard.
	// A single tap on the chat bar could never work, because focus had never left it.
	//
	// SDL_TextInputActive() is the real state, so use that as "have". The flag is kept only so
	// the rest of the class keeps working; it is now a mirror of SDL rather than a source of truth.
	const bool sdlHasTextInput = SDL_TextInputActive(m_SDLWindow);

	if (wantsTextInput) {
		if (!sdlHasTextInput) {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// GeneralsX: A dismissed keyboard means "I am done typing".
			//
			// Reading SDL's real state fixed the keyboard never coming back, and then produced the
			// mirror-image bug: tapping Hide slid it down and this function put it straight back
			// up, because the engine still had focus on the entry so it still WANTED text input.
			// It went down halfway and bounced.
			//
			// On iOS the only way to dismiss the keyboard is to stop text input, so SDL going
			// inactive while we still want it can only mean the user dismissed it. Honour that by
			// dropping focus, which is what dismissing a keyboard means everywhere else on the
			// platform - and tapping the entry again re-focuses and re-raises it normally.
			//
			// m_IsTextInputActive is the "was active last frame" mirror, so it distinguishes a
			// dismissal from the ordinary case of not having started text input yet.
			if (m_IsTextInputActive) {
				TheWindowManager->winSetFocus(NULL);
				m_IsTextInputActive = false;
				m_TextInputFocusWindow = nullptr;
				return;
			}
#endif
			if (SDL_StartTextInput(m_SDLWindow)) {
				m_IsTextInputActive = true;
			}
		} else {
			m_IsTextInputActive = true;
		}
		m_TextInputFocusWindow = focusedWindow;
	} else {
		if (sdlHasTextInput) {
			SDL_StopTextInput(m_SDLWindow);
		}
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	// GeneralsX: Taken before any return: one key-down vouches for one text event.
	const WideChar keyDownChar = s_keyDownChar;
	s_keyDownChar = 0;

	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	std::vector<WideChar> chars;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		chars.push_back(static_cast<WideChar>(codepoint));
	}

	// GeneralsX: Say how the text came (GWM_IME_CHAR_* in GameWindow.h), as Win32's
	// WM_CHAR lParam does: the one character the key just pressed names is a keystroke, flagged as an
	// auto-repeat when that key-down was SDL's repeat (see s_keyDownChar); anything else - IME or dead-key
	// composition output - is not. The drop-down letter keys pick only with a typed, non-repeated letter
	// (GadgetComboBoxDropDownOwnsText).
	const bool keystroke = !chars.empty() && ComboTextTypedByKey((Int)chars.size(), chars[0], keyDownChar);
	const WindowMsgData how = keystroke
		? (WindowMsgData)(1 | (s_keyDownRepeat ? GWM_IME_CHAR_REPEAT : 0))
		: 0;
	for (size_t i = 0; i < chars.size(); ++i) {
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(chars[i]), how);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}
// GX-SIMID-END

// GX-SIMID-BEGIN the simulation reads this - resources/gitinfo/simsourcedigest_watcher.cmake
FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}
// GX-SIMID-END

// GeneralsX: Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX: Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX: Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32

